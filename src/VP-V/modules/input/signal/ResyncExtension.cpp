// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only

#include <pch.h>
#include "ResyncExtension.h"

#include <DebugLog.h>
#include <VideoState.h>
#include <vprenderer/AlphaQueuePolicy.h>

#include <VP-V/core/FramesInFlight.h>
#include <VP-V/modules/input/signal/ResyncCallSites.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <utility>
#include <vector>

namespace Vpv
{
	namespace Resync
	{
		namespace RE = RendererExtension;

		namespace
		{
			double SteadySeconds()
			{
				return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
			}

			// Fixed lock-free host table: a handful of renderers at most exist at once (one live, one
			// retiring). Writers: HostBinding construction / destruction. Readers: every hook.
			constexpr size_t kHostSlots = 16;
			struct Slot
			{
				std::atomic<const void*> key{ nullptr };
				std::atomic<HostResync*> state{ nullptr };
			};
			std::array<Slot, kHostSlots>& Slots()
			{
				static std::array<Slot, kHostSlots> slots;
				return slots;
			}
			std::atomic<unsigned long long>& Overflows()
			{
				static std::atomic<unsigned long long> n{ 0 };
				return n;
			}

			// Render-thread status text, refreshed on an era change and every kStatusEvery seals.
			constexpr uint64_t kStatusEvery = 32;
			void RefreshDriveStatus(HostResync& h)
			{
				std::string text = h.seal.Describe() + "; " + h.metrics.StatusText() + "; " + h.dv.StatusText() +
					" (relatch not wired in this build)";
				std::lock_guard<std::mutex> lock(h.statusMutex);
				h.driveStatus.swap(text);
			}

			// The kinds the host can carry out in this build; the others are refused visibly.
			bool InjectKindAvailable(const std::string& args)
			{
				const size_t end = args.find_first_of(" \t");
				const std::string kind = args.substr(0, end);
				return kind == "minor" || kind == "full";
			}
		}

		// ---- HostResync ---------------------------------------------------------------------------
		HostResync::HostResync(const void* hostKey, VpResync::Shadow& shadowRef, VpResync::Drive& driveRef)
			: host(hostKey), shadow(shadowRef), drive(driveRef)
		{
			for (size_t i = 0; i < kSourceTimeRing; ++i)
			{
				ringSeq[i].store(0, std::memory_order_relaxed);
				ringSeconds[i].store(-1.0, std::memory_order_relaxed);
			}
			metrics.SetSink([](const std::string& line) { DebugLog::Log("%s", line.c_str()); });
			metrics.SetVectorsRequired(false);   // no vector consumer reports validity yet (P5a)
			metrics.AddReader(VpDvResync::Reader::kName);

			// The one seal (G3): providers in order, then subscribers in order. Render thread only.
			seal.AddProvider("resync-drive", [this](const std::shared_ptr<VpIndicator::Indicator>& ind) { drive.RegisterOn(ind); });
			seal.AddProvider(VpDvResync::Reader::kName, [this](const std::shared_ptr<VpIndicator::Indicator>& ind) { dv.RegisterOn(ind); });
			seal.Subscribe("resync-drive", [this](const std::shared_ptr<VpIndicator::Indicator>& ind, uint64_t seq, VpIndicator::CounterMask changed)
				{ drive.OnObserved(ind, seq, changed); });
			// The DV reader acts inside the seal, before the FULL is known to the metrics: its acts are
			// replayed by the metrics subscriber after the event is recorded.
			auto acts = std::make_shared<std::vector<std::pair<std::string, uint64_t>>>();
			dv.SetActObserver([acts](const char* name, uint64_t seq) { acts->emplace_back(name ? name : "", seq); });
			seal.Subscribe("event-metrics", [this, acts](const std::shared_ptr<VpIndicator::Indicator>& ind, uint64_t seq, VpIndicator::CounterMask changed)
				{
					if (ind && (changed & VpIndicator::Bit(VpIndicator::kFull)) != 0)
					{
						VpResync::Cause cause = VpResync::Cause::None;
						if (!ind->LastCauseIn(VpIndicator::kFull, lastSealedSeq, seq, cause))
							cause = VpResync::Cause::None;
						metrics.OnEvent(seq, VpResync::Level::Full, cause, 0, lastInjectSeq == seq);
					}
					for (const auto& a : *acts)
						metrics.OnReaderAct(a.first.c_str(), a.second);
					acts->clear();
					metrics.SetFrameRate(measuredRateHz.load(std::memory_order_relaxed));
					const double source = SourceSecondsOf(seq);
					metrics.OnSealed(ind, seq, changed, source >= 0.0 ? source * 1000.0 : 0.0);
				});
		}

		void HostResync::NoteSourceSeconds(uint64_t seq, double seconds)
		{
			const size_t i = static_cast<size_t>(seq % kSourceTimeRing);
			ringSeq[i].store(0, std::memory_order_relaxed);
			ringSeconds[i].store(seconds, std::memory_order_relaxed);
			ringSeq[i].store(seq, std::memory_order_release);
		}

		double HostResync::SourceSecondsOf(uint64_t seq) const
		{
			if (seq == 0)
				return -1.0;
			const size_t i = static_cast<size_t>(seq % kSourceTimeRing);
			if (ringSeq[i].load(std::memory_order_acquire) != seq)
				return -1.0;
			const double s = ringSeconds[i].load(std::memory_order_relaxed);
			return ringSeq[i].load(std::memory_order_acquire) == seq ? s : -1.0;
		}

		// ---- HostBinding --------------------------------------------------------------------------
		HostBinding::HostBinding(const void* host, VpResync::Shadow& shadow, VpResync::Drive& drive)
			: state_(new HostResync(host, shadow, drive))
		{
			if (!host)
				return;
			for (Slot& s : Slots())
			{
				const void* expected = nullptr;
				if (s.key.compare_exchange_strong(expected, host, std::memory_order_acq_rel))
				{
					s.state.store(state_.get(), std::memory_order_release);
					registered_ = true;
					return;
				}
			}
			// Visible, never silent: the renderer's resync stays inert (as Off) and the log says why.
			Overflows().fetch_add(1, std::memory_order_relaxed);
			DebugLog::Log("Resync: host table full (%u renderers); resync stays off for this renderer", static_cast<unsigned>(kHostSlots));
		}

		HostBinding::~HostBinding()
		{
			if (!registered_)
				return;
			for (Slot& s : Slots())
			{
				if (s.state.load(std::memory_order_acquire) == state_.get())
				{
					s.state.store(nullptr, std::memory_order_release);
					s.key.store(nullptr, std::memory_order_release);
					break;
				}
			}
		}

		// ---- Lookup --------------------------------------------------------------------------------
		HostResync* ResyncExtension::For(const void* host)
		{
			if (!host)
				return nullptr;
			for (Slot& s : Slots())
				if (s.key.load(std::memory_order_acquire) == host)
					return s.state.load(std::memory_order_acquire);
			return nullptr;
		}

		VpSeal::FrameSeal* ResyncExtension::SealFor(const void* host)
		{
			HostResync* h = For(host);
			return h ? &h->seal : nullptr;
		}

		bool ResyncExtension::StormHeld(const void* host)
		{
			HostResync* h = For(host);
			return h && h->stormHeld.load(std::memory_order_acquire);
		}

		size_t ResyncExtension::BoundHosts()
		{
			size_t n = 0;
			for (Slot& s : Slots())
				if (s.state.load(std::memory_order_acquire) != nullptr)
					++n;
			return n;
		}

		unsigned long long ResyncExtension::BindOverflows() { return Overflows().load(std::memory_order_relaxed); }

		// ---- Settings and state (lifecycle) --------------------------------------------------------
		void ResyncExtension::OnLifecycle(RE::LifecycleFacts& f)
		{
			HostResync* h = For(f.host);
			if (!h)
				return;
			switch (f.kind)
			{
			case RE::Lifecycle::SettingsApplied:
			{
				// subject = resync_event text, code = Original pathway, value = motion look-ahead frames.
				// Render thread (settings apply), as Shadow::Configure requires.
				const char* raw = static_cast<const char*>(f.subject);
				h->mode = VpResync::ModeFromSetting(raw ? std::string(raw) : std::string(), f.code != 0);
				h->motionAheadFrames = static_cast<int>(f.value);
				size_t capacity = h->queueCapacity.load(std::memory_order_acquire);
				if (capacity == 0)
					capacity = AlphaQueuePolicy::HardCapacity(AlphaQueuePolicy::DEFAULT_DESIRED_DEPTH);   // until the queue reports its own
				h->configuredCapacity = capacity;
				h->shadow.SetLogger([](const std::string& text) { DebugLog::Log("%s", text.c_str()); });
				// Frames in flight: queue capacity + motion look-ahead + render-ahead 2 + this frame (as before).
				h->shadow.Configure(h->mode, FramesInFlight(capacity, static_cast<size_t>((std::max)(0, h->motionAheadFrames)), 0, 2, 0));
				if (!h->shadow.Driving())
					h->drive.Detach();   // R3: the readers' pending changes are dropped when not `on`
				break;
			}
			case RE::Lifecycle::StateRefused:
				switch (f.code)
				{
				case kRefusalInitRace: h->shadow.NoteRefusal(VpResync::RefusalReason::InitRace); break;
				case kRefusalSettingsRebuild: h->shadow.NoteRefusal(VpResync::RefusalReason::SettingsRebuild); break;
				default:
					if (f.subject && f.previous)
						h->shadow.NoteRefusal(VpResync::ClassifyStateRefusal(*static_cast<const VideoState*>(f.subject),
							*static_cast<const VideoState*>(f.previous)));
					break;
				}
				break;
			case RE::Lifecycle::StateAccepted:
				h->shadow.ArmInPlaceSourceChange(reinterpret_cast<uintptr_t>(f.subject));
				break;
			default:
				break;
			}
		}

		// ---- Ingress (capture / file thread, renderer queue mutex held) ----------------------------
		void ResyncExtension::OnIngress(RE::IngressContext& c)
		{
			HostResync* h = For(c.host);
			if (!h || !h->shadow.Active() || c.seq <= 0)
				return;   // Off builds no facts
			VpResync::EnqueueFacts facts;
			facts.sequence = static_cast<uint64_t>(c.seq);
			facts.queueGeneration = static_cast<uint64_t>(c.queueGeneration);
			facts.stateKey = c.stateKey;
			facts.formatKey = c.formatKey;
			facts.frameCounter = static_cast<uint64_t>(c.frameCounter);
			facts.timestampTicks = c.timestamp;
			facts.ticksPerSecond = c.ticksPerSecond;
			facts.gapBefore = c.gapBefore;
			facts.gapFrames = c.gapFrames;
			facts.gapCause = c.gapCause;
			facts.sourceDiscontinuity = c.discontinuity;
			facts.fileFrame = c.fromFile;
			facts.measuredRateHz = c.measuredRateHz;
			facts.overflowSuccessorSeq = static_cast<uint64_t>(c.overflowSuccessor);
			h->shadow.OnEnqueue(facts, SteadySeconds());
			h->shadow.NoteEnqueued(facts.sequence);   // B2: the pulldown's cursor reads this frame's stamp next
			h->measuredRateHz.store(c.measuredRateHz, std::memory_order_relaxed);
			const double sourceSeconds = c.ticksPerSecond > 0 && c.timestamp != 0 ?
				static_cast<double>(c.timestamp) / static_cast<double>(c.ticksPerSecond) : -1.0;
			h->NoteSourceSeconds(facts.sequence, sourceSeconds);
			if (h->inject.Armed())
				h->inject.Release(sourceSeconds, facts.sequence);
		}

		// ---- Queue events --------------------------------------------------------------------------
		void ResyncExtension::OnQueue(const RE::QueueFacts& f)
		{
			HostResync* h = For(f.host);
			if (!h)
				return;
			switch (f.kind)
			{
			case RE::QueueEvent::Reset:
			case RE::QueueEvent::LiveQueueReset:
			case RE::QueueEvent::Flush:
				// BeginQueueGeneration (queue mutex held): arm the generation's FULL by its reason and keep the
				// real capacity for the ring (G5); the ring is resized on the render thread at the next seal.
				if (f.frames != 0)
					h->queueCapacity.store(f.frames, std::memory_order_release);
				h->shadow.ArmQueueGeneration(static_cast<uint64_t>(f.generation), f.text);
				break;
			case RE::QueueEvent::Drop:
				if (f.seq > 0)
					h->shadow.OnLateSlip(f.cause == kDropKeptRunHole ? VpResync::Cause::KeptRunHole : VpResync::Cause::DriftDrop,
						static_cast<uint64_t>(f.seq), f.frames, SteadySeconds());
				break;
			default:
				break;
			}
		}

		// ---- Frame begin: the one seal (render thread) ---------------------------------------------
		bool ResyncExtension::OnFrame(RE::FrameContext& c)
		{
			if (c.phase != RE::FramePhase::Begin)
				return false;
			HostResync* h = For(c.host);
			if (!h || !h->shadow.Active())
				return false;
			const uint64_t seq = c.seq > 0 ? static_cast<uint64_t>(c.seq) : 0;

			// The software trigger (armed only with VP_RESYNC_INJECT=1): minor / full at this frame.
			VpResync::ResyncInject::Request request;
			while (h->inject.Poll(request))
			{
				if (request.kind == VpResync::ResyncInject::Kind::Minor && seq != 0)
				{
					h->shadow.OnLateSlip(VpResync::Cause::GapOther, seq + 1, static_cast<uint32_t>(request.value), SteadySeconds());
					h->lastInjectSeq = seq + 1;
				}
				else if (request.kind == VpResync::ResyncInject::Kind::Full && seq != 0)
				{
					if (const std::shared_ptr<VpIndicator::Indicator> ind = h->shadow.GetIndicator())
						h->lastInjectSeq = ind->Advance(VpIndicator::kFull, VpResync::Cause::QueueGenerationOther, seq + 1);
				}
				else
					h->injectUnavailable.fetch_add(1, std::memory_order_relaxed);
				DebugLog::Log("Resync inject: %s%s at seq %llu", VpResync::ResyncInject::KindName(request.kind),
					request.injected ? " (schedule)" : "", static_cast<unsigned long long>(seq));
			}

			if (!h->shadow.Driving())
				return false;   // Shadow: nothing is sealed (as before)

			// G5: the queue reported a capacity other than the ring was sized for: resize here, on the
			// render thread (Configure keeps the Indicator unless the ring must grow).
			const size_t capacity = h->queueCapacity.load(std::memory_order_acquire);
			if (capacity != 0 && capacity != h->configuredCapacity)
			{
				h->configuredCapacity = capacity;
				h->shadow.Configure(h->mode, FramesInFlight(capacity, static_cast<size_t>((std::max)(0, h->motionAheadFrames)), 0, 2, 0));
			}

			const uint32_t erasBefore = h->seal.Eras();
			h->seal.Seal(h->shadow.GetIndicator(), seq);
			if (seq != 0)
				h->lastSealedSeq = seq;
			const double source = h->SourceSecondsOf(seq);
			if (source >= 0.0)
				h->lastSealedSourceSeconds.store(source, std::memory_order_relaxed);
			if (h->shadow.StormSourceTime() && source >= 0.0)
				h->shadow.AdvanceStormClock(source);
			const double renderSeconds = h->shadow.StormSourceTime() ? h->lastSealedSourceSeconds.load(std::memory_order_relaxed) : SteadySeconds();
			h->stormHeld.store(h->shadow.StormHold(renderSeconds), std::memory_order_release);
			const std::shared_ptr<VpIndicator::Indicator>& current = h->seal.Current();
			h->sealLateProviders.store(h->seal.LateProviders(), std::memory_order_relaxed);
			h->sealRejectedReaders.store(current ? current->RejectedReaders() : 0u, std::memory_order_relaxed);
			if (h->seal.Eras() != erasBefore || h->seal.Seals() % kStatusEvery == 1)
				RefreshDriveStatus(*h);
			return false;   // observation only: never a take-over
		}

		// ---- Signal rate (pulldown) ----------------------------------------------------------------
		double ResyncExtension::OnResolve(RE::Resolve kind, const RE::Context& ctx, double billValue)
		{
			if (kind != RE::Resolve::SourceSignalRate)
				return billValue;
			HostResync* h = For(ctx.host);
			return h ? VpResync::PulldownSignalHz(h->shadow, billValue) : billValue;   // Off / Shadow: billValue
		}

		// ---- Status --------------------------------------------------------------------------------
		void ResyncExtension::OnStatus(const RE::Context& ctx, RE::StatusKind kind, std::vector<std::string>& lines, const char*, const char*)
		{
			HostResync* h = For(ctx.host);
			if (!h)
				return;
			if (kind == RE::StatusKind::Lines)
			{
				std::string status, events;
				if (h->shadow.StatusLine(status, SteadySeconds()))   // false in Off: no row anywhere
					lines.push_back(std::string(kStatusLineKey) + "=" + status);
				if (h->shadow.EventLines(events))
				{
					if (h->shadow.Driving())
					{
						std::lock_guard<std::mutex> lock(h->statusMutex);
						if (!h->driveStatus.empty())
							events += " | " + h->driveStatus;
					}
					const std::string inject = h->inject.StatusText();
					if (!inject.empty())
					{
						events += " | " + inject;
						const unsigned long long refused = h->injectUnavailable.load(std::memory_order_relaxed);
						if (refused != 0)
							events += "; " + std::to_string(refused) + " request(s) not available in this build";
					}
					lines.push_back(std::string(kEventsLineKey) + "=" + events);
				}
			}
			else if (kind == RE::StatusKind::Fallbacks && h->shadow.Driving())
			{
				std::string storm;
				const double renderSeconds = h->shadow.StormSourceTime() ? h->lastSealedSourceSeconds.load(std::memory_order_relaxed) : SteadySeconds();
				if (h->shadow.StormNotice(storm, renderSeconds))
					lines.push_back("resync_storm: " + storm);
				const FallbackCatalog::Decision sealFallback = FallbackCatalog::ResyncSeal(0,
					h->sealLateProviders.load(std::memory_order_relaxed), h->sealRejectedReaders.load(std::memory_order_relaxed));
				if (sealFallback.engaged)
					lines.push_back(sealFallback.id + ": " + sealFallback.reason);
			}
		}

		// ---- Command: resync.inject (module "resync" + "inject <args>", or module "resync.inject" + "<args>")
		bool ResyncExtension::OnCommand(const RE::Context& ctx, const char* module, const char* command, std::string& reply)
		{
			if (!module || !command)
				return false;
			std::string args;
			if (std::strcmp(module, "resync.inject") == 0)
				args = command;
			else if (std::strcmp(module, "resync") == 0 && std::strncmp(command, "inject", 6) == 0 &&
				(command[6] == '\0' || command[6] == ' '))
				args = command[6] == ' ' ? std::string(command + 7) : std::string();
			else
				return false;
			HostResync* h = For(ctx.host);
			if (!h)
			{
				reply = "error: no renderer";
				return true;
			}
			if (!h->inject.Armed())
			{
				reply = h->inject.Invoke("resync.inject", args);   // "not armed"
				return true;
			}
			if (!InjectKindAvailable(args))
			{
				// seek/reset/inplace/rebuild need the renderer control hook; the adapter kinds need the capture
				// formatter extension (P13). Refused visibly instead of queued and never carried out.
				h->injectUnavailable.fetch_add(1, std::memory_order_relaxed);
				reply = "error: not available in this build: " + args.substr(0, args.find(' '));
				return true;
			}
			reply = h->inject.Invoke("resync.inject", args);
			return true;
		}

		void* ResyncExtension::Query(const char* iid)
		{
			return iid && std::strcmp(iid, kQueryId) == 0 ? static_cast<void*>(this) : nullptr;
		}

		// ---- Pulldown cursor (capture thread) ------------------------------------------------------
		PulldownStep PulldownResyncStep(const void* host, bool resetRequest)
		{
			PulldownStep step;
			HostResync* h = ResyncExtension::For(host);
			VpIndicator::CounterMask changed = 0;
			if (h)
			{
				step.driving = h->shadow.Driving();
				const uint64_t cursorSeq = h->shadow.PulldownCursorSeq();
				if (step.driving && cursorSeq != 0)
				{
					if (const std::shared_ptr<VpIndicator::Indicator> indicator = h->shadow.GetIndicator())
					{
						const bool latched = h->pulldownCursor.Has();
						changed = h->pulldownCursor.Advance(*indicator, cursorSeq);
						if (!latched)
							changed = 0;   // the first read only latches (no action on entering `on`)
					}
				}
				else if (h->pulldownCursor.Has())
					h->pulldownCursor.Reset();   // left `on`: a later `on` latches afresh
			}
			if (resetRequest || (changed & VpIndicator::Bit(VpIndicator::kFull)) != 0)
			{
				step.reset = true;
				// The request's own FULL (the new generation's first frame) must not reset it again.
				if (resetRequest && h)
					h->pulldownCursor.Reset();
				return step;
			}
			step.relatchTiming = (changed & VpIndicator::Bit(VpIndicator::kTiming)) != 0;
			return step;
		}

		void RegisterResyncExtension()
		{
			static bool registered = false;
			if (registered)
				return;
			registered = true;
			Hub::ExtensionHub::Instance().AddModule(std::unique_ptr<Hub::ISkeletonModule>(new ResyncExtension()));
		}
	}
}
