// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only

#include <pch.h>
#include "CameraExtension.h"

#include "CameraLinkCore.h"

#include <DebugLog.h>

#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/modules/input/signal/ResyncExtension.h>
#include <VP-V/modules/input/source_analysis/SourceFrameTap.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace Vpv
{
	namespace Camera
	{
		namespace
		{
			constexpr uint64_t kEarlyRefreshFrames = 8;
			constexpr uint64_t kRefreshEvery = 16;

			std::atomic<unsigned long long>& Faults()
			{
				static std::atomic<unsigned long long> n{ 0 };
				return n;
			}

			bool DumpRequested()
			{
				char* value = nullptr;
				size_t size = 0;
				bool on = false;
				if (_dupenv_s(&value, &size, "VP_CAMERA_DUMP") == 0 && value)
				{
					on = std::strcmp(value, "1") == 0;
					free(value);
				}
				return on;
			}
		}

		struct CameraHostState
		{
			explicit CameraHostState(const void* h) : host(h), ownerGen(VpFrameAnalysis::SourceFrameTap::Generation(h)),
				seenEpoch(VpFrameAnalysis::SourceFrameTap::Epoch()) {}
			const void* host;
			const uint64_t ownerGen;   // tap generation of the renderer this state was built for (address reuse = stale)
			uint64_t seenEpoch;
			FrameAnalysis::Store* builtStore = nullptr;   // the store the core references

			Skeleton::Settings settings;
			std::atomic<bool> active{ false };
			std::atomic<bool> reporting{ false };   // off, but an invalid key is being shown
			bool sealHooked = false;
			bool dump = false;
			std::string problem;                        // why there is no core (visible)
			std::unique_ptr<CameraLink::CameraLinkCore> core;   // render thread only

			uint64_t frames = 0;
			std::chrono::steady_clock::time_point lastDump;
			bool haveDump = false;

			std::mutex statusMutex;
			std::vector<std::string> fallbackLines;
			std::vector<std::string> lines;
			std::string fullText;
		};

		CameraExtension::CameraExtension(Skeleton::ConfigLoader loader) : loader_(std::move(loader)) {}

		CameraExtension::~CameraExtension()
		{
			std::vector<std::shared_ptr<CameraHostState>> all;
			hosts_.ForEach([&all](const void*, const std::shared_ptr<CameraHostState>& st) { all.push_back(st); });
			for (const std::shared_ptr<CameraHostState>& st : all)
				Teardown(st, false);
		}

		bool CameraExtension::Active(const void* host) const
		{
			const std::shared_ptr<CameraHostState> st = hosts_.Find(host);
			return st && st->active.load(std::memory_order_acquire);
		}
		CameraLink::CameraLinkCore* CameraExtension::CoreOf(const void* host) const
		{
			const std::shared_ptr<CameraHostState> st = hosts_.Find(host);
			return st ? st->core.get() : nullptr;
		}
		size_t CameraExtension::HostStates() const { return hosts_.States(); }
		const void* CameraExtension::StoreOf(const void* host) const
		{
			const std::shared_ptr<CameraHostState> st = hosts_.Find(host);
			return st && st->core ? st->builtStore : nullptr;
		}

		void CameraExtension::OnLifecycle(RE::LifecycleFacts& f)
		{
			if (!f.host)
				return;
			switch (f.kind)
			{
			case RE::Lifecycle::GpuCreated:
				hosts_.SetAux(f.host, const_cast<void*>(static_cast<const void*>(f.gpu)));   // kept so a camera switched on later gets it
				Reconcile(f.host, -1, f.gpu);
				break;
			case RE::Lifecycle::SettingsApplied:
				Reconcile(f.host, f.code != 0 ? 1 : 0, nullptr);
				break;
			case RE::Lifecycle::GpuReleased:
			case RE::Lifecycle::Shutdown:
			{
				const std::shared_ptr<CameraHostState> st = hosts_.Find(f.host);
				if (st)
					Teardown(st, true);
				else
					hosts_.Remove(f.host, true);
				break;
			}
			default:
				break;
			}
		}

		void CameraExtension::Reconcile(const void* host, int originalFlag, pl_gpu gpu)
		{
			Skeleton::Settings s = Skeleton::Load(loader_);
			if (originalFlag >= 0)
				s.originalPathway = originalFlag != 0;
			if (!gpu)
				gpu = static_cast<pl_gpu>(hosts_.Aux(host));
			std::shared_ptr<CameraHostState> st = hosts_.Find(host);
			if (st && st->ownerGen != VpFrameAnalysis::SourceFrameTap::Generation(host))
			{
				// A new renderer at the address of one that died without GpuReleased: its core's store is gone.
				Teardown(st, false);
				hosts_.Remove(host, false);
				st.reset();
			}
			if (!s.CameraWanted())
			{
				if (st)
					Teardown(st, false);
				// An unreadable camera_link is shown, never ignored.
				if (!s.originalPathway && !s.cameraProblems.empty())
				{
					if (!st)
					{
						st = std::make_shared<CameraHostState>(host);
						if (!hosts_.Put(host, st))
							return;
					}
					std::string problems;
					for (const std::string& p : s.cameraProblems)
						problems += (problems.empty() ? "" : "; ") + p;
					{
						std::lock_guard<std::mutex> lock(st->statusMutex);
						st->fallbackLines.clear();
						st->fallbackLines.push_back("camera_link: " + problems);
					}
					st->reporting.store(true, std::memory_order_release);
				}
				return;
			}
			if (!st)
			{
				st = std::make_shared<CameraHostState>(host);
				if (!hosts_.Put(host, st))
				{
					DebugLog::Log("Camera: host table full; camera_link stays off for this renderer");
					return;
				}
			}
			st->settings = s;
			st->dump = DumpRequested();
			st->problem.clear();
			FrameAnalysis::Store* store = VpFrameAnalysis::SourceFrameTap::StoreFor(host);
			if (st->core && st->builtStore != store)
			{
				st->core->OnGpuReleased();
				st->core.reset();   // built over a store that is gone or replaced
				st->builtStore = nullptr;
			}
			if (!st->core)
			{
				if (!store)
					st->problem = "no frame analysis store for this renderer (the renderer does not register one); the camera link cannot report";
				else
				{
					st->core.reset(new CameraLink::CameraLinkCore(*store));
					st->builtStore = store;
					CameraLink::CameraLinkSettings settings;
					settings.p7Wanted = false;   // no vector engine yet (P5a): nothing for the fit to read, nothing is allocated
					st->core->OnSettings(settings);
				}
			}
			if (st->core && gpu)
				st->core->OnGpuCreated(gpu);
			// The reader of the camera fit on the Indicator: registered through the one shared seal, once per renderer.
			if (!st->sealHooked)
			{
				if (VpSeal::FrameSeal* seal = Resync::ResyncExtension::SealFor(host))
				{
					const std::weak_ptr<CameraHostState> weak = st;
					seal->AddProvider(kName, [weak](const std::shared_ptr<VpIndicator::Indicator>& ind) {
						if (const std::shared_ptr<CameraHostState> s2 = weak.lock())
							if (s2->core && ind)
								s2->core->RegisterReader(*ind);
					});
					st->sealHooked = true;
				}
				else if (st->problem.empty())
					st->problem = "no resync seal for this renderer (resync_event=on required); the camera link cannot report";
			}
			st->active.store(true, std::memory_order_release);
			Refresh(*st);
		}

		void CameraExtension::Teardown(const std::shared_ptr<CameraHostState>& st, bool releaseSlot)
		{
			if (!st)
				return;
			st->active.store(false, std::memory_order_release);
			st->reporting.store(false, std::memory_order_release);
			if (st->core)
			{
				st->core->OnGpuReleased();
				st->core.reset();   // no thread exists in report mode; the core stops its own worker if one ever did
			}
			{
				std::lock_guard<std::mutex> lock(st->statusMutex);
				st->fallbackLines.clear();
				st->lines.clear();
				st->fullText.clear();
			}
			if (releaseSlot)
				hosts_.Remove(st->host, true);
		}

		bool CameraExtension::OnFrame(RE::FrameContext& c)
		{
			if (c.phase != RE::FramePhase::Begin && c.phase != RE::FramePhase::AfterRender)
				return false;
			const std::shared_ptr<CameraHostState> st = hosts_.Find(c.host);
			if (!st || !st->active.load(std::memory_order_acquire) || c.seq <= 0)
				return false;
			const uint64_t epoch = VpFrameAnalysis::SourceFrameTap::Epoch();
			if (epoch != st->seenEpoch)
			{
				st->seenEpoch = epoch;
				if (st->ownerGen != VpFrameAnalysis::SourceFrameTap::Generation(c.host) ||
					st->builtStore != VpFrameAnalysis::SourceFrameTap::StoreFor(c.host))
				{
					Reconcile(c.host, -1, nullptr);   // stale core: rebuild over the current store (or say there is none)
					return false;
				}
			}
			if (c.phase == RE::FramePhase::Begin)
			{
				++st->frames;
				Resync::HostResync* resync = Resync::ResyncExtension::For(c.host);
				if (st->core && resync && resync->shadow.Driving())
				{
					CameraLink::FramePhaseFacts facts;
					facts.seq = static_cast<uint64_t>(c.seq);
					st->core->OnFrameStart(facts);
				}
				Refresh(*st);
			}
			else if (st->core)
				st->core->OnPostSubmitGap();
			return false;   // reporting only: never a take-over, never feeds a consumer
		}

		// Render thread: the cached text the other threads read.
		void CameraExtension::Refresh(CameraHostState& st)
		{
			const bool due = st.frames <= kEarlyRefreshFrames || st.frames % kRefreshEvery == 1;
			if (!due && !st.dump)
				return;
			std::vector<std::string> lines;
			std::vector<std::string> fb;
			auto addDecision = [&fb](const FallbackCatalog::Decision& d) {
				if (d.engaged)
					fb.push_back(d.id + ": " + d.reason);
			};
			addDecision(FallbackCatalog::CameraLinkReport(st.problem));
			std::string problems;
			for (const std::string& p : st.settings.cameraProblems)
				problems += (problems.empty() ? "" : "; ") + p;
			if (!problems.empty())
				fb.push_back("camera_link: " + problems);
			const unsigned long long faults = Faults().load(std::memory_order_relaxed);
			if (faults != 0)
				fb.push_back("camera_fault: " + std::to_string(faults) + " camera callback exception(s) contained");
			if (st.core)
			{
				lines.push_back(kReportLine);
				st.core->CollectStatus(lines);   // empty while the fit is not wanted
				for (const FallbackCatalog::Decision& d : st.core->CollectDecisions())
					addDecision(d);
			}
			else
				lines.push_back("camera: report mode unavailable (" + (st.problem.empty() ? std::string("starting") : st.problem) + ")");

			if (st.dump)
			{
				const auto now = std::chrono::steady_clock::now();
				if (!st.haveDump || now - st.lastDump >= std::chrono::seconds(1))
				{
					st.haveDump = true;
					st.lastDump = now;
					for (const std::string& l : lines)
						DebugLog::Log("%s", l.c_str());
					for (const std::string& l : fb)
						DebugLog::Log("%s", l.c_str());
				}
			}

			std::string text;
			for (const std::string& l : lines)
				text += (text.empty() ? "" : "\n") + l;
			for (const std::string& l : fb)
				text += "\n" + l;
			std::lock_guard<std::mutex> lock(st.statusMutex);
			st.lines.swap(lines);
			st.fallbackLines.swap(fb);
			st.fullText.swap(text);
		}

		void CameraExtension::OnStatus(const RE::Context& ctx, RE::StatusKind kind, std::vector<std::string>& out, const char* worker, const char*)
		{
			if (kind == RE::StatusKind::WorkerFault)
			{
				if (worker && std::strcmp(worker, kName) == 0)
					Faults().fetch_add(1, std::memory_order_relaxed);
				return;
			}
			if (kind == RE::StatusKind::WorkerOk)
				return;
			const std::shared_ptr<CameraHostState> st = hosts_.Find(ctx.host);
			if (!st)
				return;
			const bool active = st->active.load(std::memory_order_acquire);
			if (!active && !st->reporting.load(std::memory_order_acquire))
				return;
			std::lock_guard<std::mutex> lock(st->statusMutex);
			if (kind == RE::StatusKind::Lines)
			{
				if (!active)
					return;
				// "camera: text" becomes the key=value row the status page shows: "camera=text".
				const std::string prefix = "camera: ";
				for (const std::string& l : st->lines)
					out.push_back(l.compare(0, prefix.size(), prefix) == 0 ? std::string(kLineKey) + "=" + l.substr(prefix.size()) : l);
			}
			else
			{
				for (const std::string& l : st->fallbackLines)
					out.push_back(l);
			}
		}

		bool CameraExtension::OnCommand(const RE::Context& ctx, const char* module, const char* command, std::string& reply)
		{
			if (!module || !command || std::strcmp(module, kName) != 0 || std::strcmp(command, "status") != 0)
				return false;
			const std::shared_ptr<CameraHostState> st = hosts_.Find(ctx.host);
			if (!st || !st->active.load(std::memory_order_acquire))
			{
				reply = "camera: off (camera_link = off, or the Original pathway)";
				return true;
			}
			std::lock_guard<std::mutex> lock(st->statusMutex);
			reply = st->fullText.empty() ? std::string("camera: starting") : st->fullText;
			return true;
		}

		void RegisterCameraExtension()
		{
			static bool registered = false;
			if (registered)
				return;
			registered = true;
			Hub::ExtensionHub::Instance().AddModule(std::unique_ptr<Hub::ISkeletonModule>(new CameraExtension()));
		}
	}
}
