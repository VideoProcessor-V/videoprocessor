// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only

#include <pch.h>
#include "CameraExtension.h"

#include "CameraDump.h"
#include "CameraLabelFill.h"
#include "CameraLabelProducer.h"
#include "CameraLane.h"
#include "CameraLinkCore.h"
#include "CameraStatus.h"
#include "GuidanceAdapter.h"
#include "GuidanceCountsRing.h"

#include <DebugLog.h>

#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/modules/control/skeleton/SkeletonFacts.h>
#include <VP-V/modules/input/signal/ResyncExtension.h>
#include <VP-V/modules/motion/estimation/global_model/CameraProducts.h>
#include <VP-V/modules/motion/estimation/MotionAheadProducer.h>
#include <VP-V/modules/input/source_analysis/SourceFrameTap.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

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
			CameraSettings cam;                         // [vpv.camera] (K3), Original applied
			std::shared_ptr<CameraLink::CameraDump> dumpWriter;   // VP_CAMERA_DUMP=<dir>; null = no writer, no thread
			std::atomic<bool> active{ false };
			std::atomic<bool> reporting{ false };   // off, but an invalid key is being shown
			bool sealHooked = false;
			bool dump = false;
			std::string problem;                        // why there is no core (visible)

			// Camera live (render thread). Members are declared in dependency order (destroyed in reverse): the table outlives the core's
			// worker, the core outlives what refers to it. ReleaseLive undoes them explicitly in the detach order of the header.
			bool fitWanted = false;                     // camera_link report + camera_model on (p7Wanted)
			bool guideWanted = false;                   // ... and any camera_meas_* switch on
			bool labelsWanted = false;                  // ... and labels are wanted (overlay, a use switch, camera_labels_gpu)
			bool aheadWanted = false;                   // ... and camera_ahead_fit on (the fit reads the ahead NVOF pair, FlowOf)
			bool guideLocked = false;                   // a camera_meas_* switch is set but the guidance lock holds it off (shown)
			Skeleton::IExternalDemand* demand = nullptr;   // AHEAD FIT: the skeleton's demand entry the P7 demand was declared through (null = none)
			bool liveBuilt = false;                     // lane built and the core attached to it
			bool graphRefused = false;                  // the registry refused the camera graph (until the next settings change)
			bool liveLabelsGpu = false;                 // camera_labels_gpu the live parts were built with
			std::string liveWait;                       // why the fit is not running yet (shown as the status line)
			bool liveWaitIsFault = false;               // ... and whether that is a downgrade the Fallbacks list must name
			pl_gpu gpu = nullptr;
			VpMotion::IVectorEngine* engine = nullptr;  // the engine the live parts were built over (never used past ReleaseLive)
			std::shared_ptr<VpIndicator::Indicator> indicator;   // keeps the Indicator the lane's stamps and the P4 reader read alive
			std::string engineProblem;                  // the engine is absent (visible)
			std::string graphProblem;                   // the camera graph was refused (visible)
			std::string seedProblem;                    // F2: the engine has fields but no optical-flow seed (visible camera_fit)
			uint64_t lastFieldNoted = 0;
			CameraLink::TimingFacts timing;
			bool haveTiming = false;
			std::atomic<double> periodSeconds{ 0.0 };   // the latched source period (0 = unknown): the guide lane's nextPairSeconds
			std::atomic<std::thread::id> renderThread{ std::thread::id() };
			std::atomic<unsigned long long> sceneUnknown{ 0 };   // pairs described without a scene record (cut / fade unknown)
			CameraLink::CameraLane lane;
			std::unique_ptr<CameraLink::CameraLinkCore> core;   // render thread only
			std::unique_ptr<CameraLink::CameraLabelProducer> labelProducer;
			std::unique_ptr<CameraLink::CameraLabelFill> labelFill;
			std::unique_ptr<CameraLink::CameraGuidanceAdapter> adapter;

			uint64_t frames = 0;
			std::chrono::steady_clock::time_point lastDump;
			bool haveDump = false;

			std::mutex statusMutex;
			std::vector<std::string> fallbackLines;
			std::vector<std::string> lines;
			std::string fullText;
		};

		namespace
		{
			// K3: starts, keeps or stops the dump writer. It exists only while VP_CAMERA_DUMP names a directory and a core can feed
			// it (camera_link = report); with the variable unset no object and no thread exist.
			void ReconcileDump(CameraHostState& st, const CameraSettings& cam)
			{
				const std::string dir = CameraLink::CameraDump::DirectoryFromEnvironment();
				if (dir.empty() || !st.core)
				{
					if (st.core)
						st.core->SetDump(nullptr);
					st.dumpWriter.reset();   // joins the writer thread, which writes what is queued first
					return;
				}
				CameraLink::DumpConfig cfg;
				cfg.directory = dir;
				cfg.clip = CameraLink::CameraDump::ClipFromEnvironment();
				cfg.mode = (cam.ModelWanted() && cam.meas.Any()) ? "engine_camera" : "engine";
				if (st.dumpWriter && st.dumpWriter->Config().directory == cfg.directory && st.dumpWriter->Config().clip == cfg.clip &&
					st.dumpWriter->Config().mode == cfg.mode)
				{
					st.core->SetDump(st.dumpWriter);   // the same writer on a rebuilt core
					return;
				}
				st.core->SetDump(nullptr);
				st.dumpWriter.reset();
				st.dumpWriter = std::make_shared<CameraLink::CameraDump>(cfg, true);
				st.core->SetDump(st.dumpWriter);
			}
		}

		namespace
		{
			enum class SceneRead { Missing, Present };

			// What the frame-analysis store knows about frame `seq` (any thread: the store has its own mutex). Missing = no record at all
			// (never begun, evicted); Present = a record, with `have` true only when its scene group was written (cut / fade known).
			SceneRead ReadScene(FrameAnalysis::Store* store, uint64_t seq, bool& have, bool& cut, bool& fade, CameraModel::Rect* picture)
			{
				have = cut = fade = false;
				if (!store)
					return SceneRead::Missing;
				FrameAnalysis::Record rec;
				const FrameAnalysis::ReadStatus status = store->Read(seq, FrameAnalysis::Use::SingleFrame, FrameAnalysis::kIdentity, rec);
				if (status == FrameAnalysis::ReadStatus::Missing)
					return SceneRead::Missing;
				if (status != FrameAnalysis::ReadStatus::Ok)
					return SceneRead::Present;   // disabled or not measured: present without facts (counted by the caller)
				if (picture && !rec.id.picture.Empty())
				{
					picture->left = rec.id.picture.left;
					picture->top = rec.id.picture.top;
					picture->right = rec.id.picture.right;
					picture->bottom = rec.id.picture.bottom;
				}
				if ((rec.valid & FrameAnalysis::kScene) != 0)
				{
					have = true;
					cut = rec.scene.cut;
					fade = rec.scene.fade;
				}
				return SceneRead::Present;
			}

			// The PairFactsProvider of the core (render thread: the engine is render-thread only). Timing stays with the producer's latched
			// PairTiming (film lock, measured rate); no timestamps are passed (a cadence-converted source would alternate).
			bool DescribePair(CameraHostState& st, VpMotion::IVectorEngine& engine, FrameAnalysis::Store* store,
				const CameraLink::FieldCopy& f, CameraLink::PairDescription& d)
			{
				d = CameraLink::PairDescription();
				CameraLink::PairFacts& facts = d.facts;
				facts.spansSlip = engine.PairSpansSlip(f.pairFrom, f.pairTo);
				VpMotion::GlobalVec g;
				if (engine.GlobalFromEngine() && engine.EngineGlobal(f.pairTo, g) && g.valid)
				{
					facts.haveEngineGlobal = true;
					facts.engineGx = g.x;
					facts.engineGy = g.y;
				}
				bool have = false, cut = false, fade = false;
				ReadScene(store, f.pairTo, have, cut, fade, &facts.picture);
				facts.cut = cut;
				facts.fade = fade;
				if (!have)
					st.sceneUnknown.fetch_add(1, std::memory_order_relaxed);
				d.uniqueFramesBetween = 1;   // an engine pair is two adjacent UNIQUE source frames
				return true;
			}

			// AHEAD FIT: the facts of an ahead pair (render thread). The matcher has not estimated it yet, so the engine's render-time facts
			// do not exist for it: no slip by construction (FlowOf serves a pair only under the vectors-valid state, never across a resync /
			// gap / format change), the engine's whole-frame motion comes with the copy (CameraLinkCore sets it), the picture and the scene
			// from the frame-analysis store when the frame is described already (else counted as camera_scene, as for every pair).
			bool DescribeAhead(CameraHostState& st, FrameAnalysis::Store* store, const CameraLink::FieldCopy& f, CameraLink::PairDescription& d)
			{
				d = CameraLink::PairDescription();
				bool have = false, cut = false, fade = false;
				ReadScene(store, f.pairTo, have, cut, fade, &d.facts.picture);
				d.facts.cut = cut;
				d.facts.fade = fade;
				if (!have)
					st.sceneUnknown.fetch_add(1, std::memory_order_relaxed);
				d.uniqueFramesBetween = 1;
				return true;
			}

			// AHEAD FIT: the ahead pair of `seq` from the engine (render thread: FlowOf's contract), copied at once (the view dies at the
			// next FlowOf) with its whole-frame motion, worked out exactly as the engine does for its own record (MotionAhead::FlowGlobal on
			// the forward vectors and costs, one engine cell = cellEnginePx x enginePxToSource source px, usable at ambiguity < 0.9).
			CameraLink::AheadRefusal TakeAhead(VpMotion::IVectorEngine& engine, uint64_t seq, CameraLink::AheadCopy& out)
			{
				VpMotion::FlowView v;
				if (!engine.FlowOf(seq, v))
					return CameraLink::AheadRefusal::NoPair;
				const CameraLink::AheadRefusal r = CameraLink::CopyAhead(v, seq, out);
				if (r != CameraLink::AheadRefusal::None)
					return r;
				out.epoch = engine.FieldEra();   // G-A3: the P2 provenance (the epoch FlowOf just served the pair under; same render-thread call)
				float g[4] = {}, second[4] = {};
				MotionAhead::FlowGlobal(out.forward, out.forwardCost, out.gridW, out.gridH, g, second);
				out.globalValid = g[3] > 0.5f && g[2] < 0.9f;
				out.globalX = out.CellSourcePx() * static_cast<double>(g[0]);
				out.globalY = out.CellSourcePx() * static_cast<double>(g[1]);
				return r;
			}

			// The timing the fit latches (film lock when the cadence is locked, else the measured source rate). Relatched only when it moved
			// by more than 1%: a relatch resets the fit's timing basis.
			void UpdateTiming(CameraHostState& st, Resync::HostResync* resync, FrameAnalysis::Store* store, uint64_t seq)
			{
				CameraLink::TimingFacts t;
				if (resync)
					t.measuredSourceHz = resync->measuredRateHz.load(std::memory_order_relaxed);
				if (store && seq > 1)
				{
					FrameAnalysis::Record rec;
					if (store->Read(seq - 1, FrameAnalysis::Use::SingleFrame, FrameAnalysis::kCadence, rec) == FrameAnalysis::ReadStatus::Ok &&
						rec.cadence.filmHz > 0.0)
					{
						t.filmHz = rec.cadence.filmHz;
						t.cadenceLocked = true;
					}
				}
				auto moved = [](double a, double b) { return (a > 0.0) != (b > 0.0) || (a > 0.0 && std::fabs(a - b) > 0.01 * b); };
				if (st.haveTiming && st.timing.cadenceLocked == t.cadenceLocked && !moved(st.timing.measuredSourceHz, t.measuredSourceHz) &&
					!moved(st.timing.filmHz, t.filmHz))
					return;
				st.timing = t;
				st.haveTiming = true;
				const double hz = t.cadenceLocked && t.filmHz > 0.0 ? t.filmHz : t.measuredSourceHz;
				st.periodSeconds.store(hz > 0.0 ? 1.0 / hz : 0.0, std::memory_order_relaxed);
				if (st.core)
				{
					st.core->SetTimingFacts(t);
					if (st.core->Producer())
						st.core->Producer()->NoteTiming();   // the worker relatches before its next fit
				}
			}
		}

		CameraExtension::CameraExtension(Skeleton::ConfigLoader loader, EngineDirectoryProvider engines)
			: loader_(std::move(loader)), engines_(std::move(engines)) {}

		CameraExtension::~CameraExtension()
		{
			std::vector<std::shared_ptr<CameraHostState>> all;
			hosts_.ForEach([&all](const void*, const std::shared_ptr<CameraHostState>& st) { all.push_back(st); });
			for (const std::shared_ptr<CameraHostState>& st : all)
				Teardown(st, false);
		}

		std::string CameraExtension::LagBridgeText()
		{
			return "private lag bridge (P7 / P8 sealed at N+" + std::to_string(static_cast<int>(CameraLink::CameraLane::kSealLag)) +
				" in the camera's own table, not in the skeleton slots) until SK-W2";
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

		CameraSettings CameraExtension::CameraSettingsOf(const void* host) const
		{
			const std::shared_ptr<CameraHostState> st = hosts_.Find(host);
			return st ? st->cam : CameraSettings();
		}
		const CameraLink::CameraDump* CameraExtension::DumpOf(const void* host) const
		{
			const std::shared_ptr<CameraHostState> st = hosts_.Find(host);
			return st ? st->dumpWriter.get() : nullptr;
		}
		bool CameraExtension::FitWanted(const void* host) const
		{
			const std::shared_ptr<CameraHostState> st = hosts_.Find(host);
			return st && st->fitWanted;
		}
		bool CameraExtension::LiveBuilt(const void* host) const
		{
			const std::shared_ptr<CameraHostState> st = hosts_.Find(host);
			return st && st->liveBuilt;
		}
		const CameraLink::CameraLane* CameraExtension::LaneOf(const void* host) const
		{
			const std::shared_ptr<CameraHostState> st = hosts_.Find(host);
			return st ? &st->lane : nullptr;
		}
		CameraLink::CameraGuidanceAdapter* CameraExtension::AdapterOf(const void* host) const
		{
			const std::shared_ptr<CameraHostState> st = hosts_.Find(host);
			return st ? st->adapter.get() : nullptr;
		}
		std::string CameraExtension::EngineProblemOf(const void* host) const
		{
			const std::shared_ptr<CameraHostState> st = hosts_.Find(host);
			return st ? st->engineProblem : std::string();
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

		// ---- camera live ---------------------------------------------------------------------------------------------------

		VpMotion::IVectorEngine* CameraExtension::FindEngine(const void* host) const
		{
			VpMotion::IVectorEngineDirectory* directory = engines_ ? engines_() :
				static_cast<VpMotion::IVectorEngineDirectory*>(Hub::ExtensionHub::Instance().Query(VpMotion::kVectorEngineQueryId));
			return directory ? directory->EngineFor(host) : nullptr;
		}

		// Detach order (render thread, renderMutex held; the LOCK RULE in GuidanceAdapter.h applies): the adapter clears the engine's hook
		// FIRST and then the fit-done observer (it waits for a call in flight on the camera worker); then the core drops its engine,
		// providers and graph (DetachGraph joins the camera worker: nothing reads the table or the Indicator after it); then the label
		// fill / producer (GPU objects: the device is still alive here), the table, and last the engine pointer and the Indicator.
		void CameraExtension::ReleaseLive(CameraHostState& st)
		{
			if (st.adapter)
			{
				// The engine is called only while it is KNOWN valid: the directory still returns this very pointer for this host. A renderer
				// that died without GpuReleased (a stale tap), or an engine that is gone, is dropped without a call.
				const bool engineValid = st.engine != nullptr && FindEngine(st.host) == st.engine;
				st.adapter->Detach(engineValid);
				st.adapter.reset();
			}
			if (st.demand)
			{
				st.demand->ClearExternalDemand(st.host, &st);   // AHEAD FIT: the on-time P7 demand goes with the live parts
				st.demand = nullptr;
			}
			if (st.core)
			{
				st.core->AttachOnTime(VpSkeleton::LaneLeaseSource(), std::string());
				st.core->SetP2EpochSource(std::function<uint64_t()>());
				st.core->labelsProvider = nullptr;
				st.core->SetFactsProvider(nullptr);
				st.core->SetEngine(nullptr);
				st.core->DetachGraph();
			}
			st.labelFill.reset();
			st.labelProducer.reset();
			st.lane.Clear();
			st.engine = nullptr;
			st.indicator.reset();
			st.liveBuilt = false;
			st.lastFieldNoted = 0;
			st.haveTiming = false;
		}

		// Render thread, at Begin. True when the live parts exist for `engine` and `indicator`. The table is built at the first verified
		// field (its dimensions size the declarations); until then the status says "waiting for the first vector field".
		bool CameraExtension::EnsureLive(CameraHostState& st, const void* host, const std::shared_ptr<VpIndicator::Indicator>& indicator)
		{
			if (!st.fitWanted || !st.core || !indicator || st.graphRefused)
				return false;
			VpMotion::IVectorEngine* engine = FindEngine(host);
			if (!engine)
			{
				if (st.liveBuilt)
					ReleaseLive(st);   // the engine went away under us (the renderer is being rebuilt): nothing may keep pointing at it
				st.engineProblem = "the vector engine of this renderer is not available; the camera fit and guided matching cannot attach "
					"(it exists only while the renderer's motion path is built)";
				return false;
			}
			st.engineProblem.clear();
			if (st.liveBuilt && st.engine == engine && st.indicator == indicator)
				return true;
			if (st.liveBuilt)
				ReleaseLive(st);   // another engine, or another Indicator era: rebuild over the new ones

			// F2: the fit's input is the optical-flow seed (its 16-px source grid sizes the declarations); never FieldOf.
			const uint64_t latest = engine->LatestVerifiedFieldSeq();
			VpMotion::SeedField sf;
			if (latest == 0)
				return false;
			if (!engine->SeedFieldOf(latest, sf) || sf.sourceGridW <= 0 || sf.sourceGridH <= 0)
			{
				st.seedProblem = sf.absence == VpMotion::SeedAbsence::Skipped ? CameraLink::kSeedSkippedText : CameraLink::kSeedAbsentText;
				return false;
			}
			st.seedProblem.clear();
			const uint32_t width = static_cast<uint32_t>(sf.sourceGridW) * static_cast<uint32_t>(VpMotion::SeedField::kSourceBlockSize);
			const uint32_t height = static_cast<uint32_t>(sf.sourceGridH) * static_cast<uint32_t>(VpMotion::SeedField::kSourceBlockSize);
			if (!st.lane.Build(width, height, st.labelsWanted, indicator))
			{
				st.graphProblem = st.lane.Error();
				st.graphRefused = true;   // the same registry gives the same answer: shown, not retried every frame
				return false;
			}
			st.graphProblem.clear();

			CameraHostState* sp = &st;
			FrameAnalysis::Store* store = st.builtStore;
			VpIndicator::Indicator* ind = indicator.get();
			st.core->SetEngine(engine);
			st.core->SetFactsProvider([sp, engine, store](const CameraLink::FieldCopy& f, CameraLink::PairDescription& d) {
				return DescribePair(*sp, *engine, store, f, d);
			});
			if (st.aheadWanted)
			{
				// AHEAD FIT: the ahead pair (FlowOf, render thread), converted onto the seed's 16-px source grid (the grid that sizes the camera
				// graph), trust from the matcher's own consistency limit. No on-time target: SK-W gives the camera no lease of the skeleton
				// lane's P7 (link/README.md "Ahead fit: gaps"), so every ahead result is bridged, visibly.
				st.core->SetAheadSource([engine](uint64_t seq, CameraLink::AheadCopy& out) { return TakeAhead(*engine, seq, out); },
					[sp, store](const CameraLink::FieldCopy& f, CameraLink::PairDescription& d) { return DescribeAhead(*sp, store, f, d); });
				st.core->SetAheadGrid(sf.sourceGridW, sf.sourceGridH);
				st.core->SetP2EpochSource([engine]() { return engine->FieldEra(); });   // any thread (IVectorEngine::FieldEra)
				st.core->SetAheadConsistency(sf.consistencyPx);
				// G-A1: the on-time target is the skeleton lane's P7, reached through the skeleton's demand entry (Query, no module include):
				// the demand makes the lane hold P7 (published outside: the camera fit worker), LeaseFor gives its current lease per use. No
				// skeleton module / no lane: the lease is empty and every frame is bridged, visibly (camera.ahead.no_target). P8 stays on the
				// bridge (G-A4: not declared).
				Skeleton::IExternalDemand* demand = static_cast<Skeleton::IExternalDemand*>(
					Hub::ExtensionHub::Instance().Query(Skeleton::kExternalDemandQueryId));
				if (demand)
				{
					std::vector<VpSkeleton::ConsumerDecl> consumers;
					CameraLink::DeclareCameraConsumers(CameraLink::LiveCameraDemand(false), consumers);
					demand->SetExternalDemand(host, &st, consumers, { VpSkeleton::kP7CameraFit });
					st.demand = demand;
					st.core->AttachOnTime([demand, host]() { return demand->LeaseFor(host, VpSkeleton::kP7CameraFit); }, "skeleton lane");
				}
			}
			else
				st.core->SetAheadSource(CameraLink::AheadSource(), CameraLink::PairFactsProvider());
			if (st.lane.HasLabels() && st.gpu)
			{
				// P8 with CPU labels uploaded to a texture (camera_labels_gpu on: computed on the GPU instead).
				const CameraLink::LabelMode mode = CameraLink::ChooseLabelMode(st.cam.LabelsGpu(), false);
				st.labelProducer.reset(new CameraLink::CameraLabelProducer(nullptr, st.gpu, mode, st.lane.LabelRing(), st.lane.LabelLive()));
				// The fill holds the table through a lease (rebinds on a new generation), as it will hold the skeleton lane's after SK-W2.
				st.labelFill.reset(new CameraLink::CameraLabelFill(st.lane.Source(VpSkeleton::kP8CameraLabels), *st.core->Link(), *st.labelProducer));
				st.lane.SetLabelFill(st.labelFill.get());
				CameraLink::CameraLabelFill* fill = st.labelFill.get();
				st.core->labelsProvider = [fill](uint64_t seq, CameraLink::CameraConsumer c, CameraLink::CameraLabelsView& out) {
					return fill->LabelsFor(seq, c, out);
				};
			}
			if (st.guideWanted)
			{
				CameraLink::GuidanceSettings gs;
				gs.switches = st.cam.Measurement();
				CameraLink::GuidanceAdapterHooks hooks;
				// LOCK RULE: these run on the camera worker (the guide lane job): they read the store and the Indicator only, never
				// renderMutex and never anything that waits for the render thread.
				hooks.p4 = [sp, store, ind](uint64_t pair, CameraLink::PairContext& ctx) -> bool {
					bool have = false, cut = false, fade = false;
					if (ReadScene(store, pair, have, cut, fade, nullptr) == SceneRead::Missing)
						return false;   // not described yet: the lane asks again, the live fill treats it as "record late"
					if (!have)
						sp->sceneUnknown.fetch_add(1, std::memory_order_relaxed);
					ctx.ancientStamp = ind->StampOf(pair).ancient;
					ctx.cut = cut;
					ctx.fade = fade;
					ctx.nextPairSeconds = sp->periodSeconds.load(std::memory_order_relaxed);
					return true;
				};
				hooks.nextPair = [store](uint64_t after, uint64_t& next) -> bool {
					for (uint64_t s = after + 1; s <= after + 16; ++s)
					{
						bool have = false, cut = false, fade = false;
						if (ReadScene(store, s, have, cut, fade, nullptr) == SceneRead::Present)
						{
							next = s;
							return true;
						}
					}
					return false;
				};
				hooks.onRenderThread = [sp]() { return sp->renderThread.load(std::memory_order_relaxed) == std::this_thread::get_id(); };
				st.adapter.reset(new CameraLink::CameraGuidanceAdapter(*st.core, gs, hooks));
				std::unique_ptr<CameraLink::IGuidanceCountsSource> counts;
				if (st.gpu)
					counts.reset(new CameraLink::GuidanceCountsRing(st.gpu));
				st.adapter->RebuildGraph(st.periodSeconds.load(std::memory_order_relaxed), false);   // d from the MEASURED costs, before AttachLane restarts the meter
				st.adapter->Attach(engine, std::move(counts));                                      // registers the ONE hook (switches are on)
			}
			// LEASE path (SK-W): the fit worker holds the table, the P7 bytes and the Indicator through an owned lease and rebinds when the
			// generation changes (a job stamped with an older one is refused: PublishOutcome::StaleLane). Until SK-W2 the source is the lag
			// bridge; then it becomes the skeleton lane's AnalysisLane::Lease(kP7CameraFit).
			st.core->AttachLane(st.lane.Source(VpSkeleton::kP7CameraFit));
			if (!st.core->Allocated())
			{
				st.graphProblem = "the camera link did not allocate its fit on the camera graph (no P7 node or the fit is not wanted)";
				st.graphRefused = true;
				st.engine = engine;   // so ReleaseLive's engine-facing steps run in the usual order
				ReleaseLive(st);
				return false;
			}
			st.engine = engine;
			st.indicator = indicator;
			st.lastFieldNoted = latest;
			st.liveLabelsGpu = st.cam.LabelsGpu();
			st.liveBuilt = true;
			return true;
		}

		// Render thread, at Begin of a frame the resync drives. Everything the camera does per frame, in order: the engine's new fields are
		// noted, the timing latched, the lane publishes the input markers and seals the frame that left the window, the core runs its
		// frame start (seal observer, readback poll, field requests, fit jobs), the guidance adapter finishes the counts and pumps its lane job.
		void CameraExtension::DriveFrame(CameraHostState& st, const void* host, int64_t seqIn)
		{
			st.renderThread.store(std::this_thread::get_id(), std::memory_order_relaxed);
			Resync::HostResync* resync = Resync::ResyncExtension::For(host);
			if (!st.core || !resync || seqIn <= 0)
				return;
			if (!resync->shadow.Driving())
			{
				if (st.fitWanted)
				{
					st.liveWait = "camera fit waits for resync_event: turn resync_event on";
					st.liveWaitIsFault = true;
				}
				st.seedProblem.clear();   // not judged while nothing drives the fit
				return;
			}
			st.liveWait.clear();
			st.liveWaitIsFault = false;
			const uint64_t seq = static_cast<uint64_t>(seqIn);
			CameraLink::FramePhaseFacts facts;
			facts.seq = seq;
			std::shared_ptr<VpIndicator::Indicator> indicator;
			if (VpSeal::FrameSeal* seal = Resync::ResyncExtension::SealFor(host))
				indicator = seal->Current();
			if (st.fitWanted && EnsureLive(st, host, indicator))
			{
				UpdateTiming(st, resync, st.builtStore, seq);
				// The engine's fields since the last frame (a cadence repeat has none): the seal of a frame without one is not a late fit.
				VpMotion::IVectorEngine* engine = st.engine;
				const uint64_t latest = engine->LatestVerifiedFieldSeq();
				if (latest < st.lastFieldNoted)
					st.lastFieldNoted = 0;   // the numbering restarted (a seek, a resync, a graph rebuild)
				const uint64_t first = latest > st.lastFieldNoted + 16 ? latest - 15 : st.lastFieldNoted + 1;
				for (uint64_t s = first; s <= latest && latest != 0; ++s)
				{
					VpMotion::SeedField sf;   // F2: a frame has a fit input only when its pair has a seed
					if (engine->SeedFieldOf(s, sf))
						st.lane.NoteField(s);
				}
				{
					VpMotion::SeedField sf;
					if (latest != 0 && !engine->SeedFieldOf(latest, sf))
						st.seedProblem = sf.absence == VpMotion::SeedAbsence::Skipped ? CameraLink::kSeedSkippedText : CameraLink::kSeedAbsentText;
					else if (latest != 0)
						st.seedProblem.clear();
				}
				st.lastFieldNoted = latest;
				CameraLink::CameraLane::Sealed sealed;
				if (!st.lane.OnFrame(seq, *indicator, sealed))
				{
					ReleaseLive(st);   // the sequence went backwards or jumped past the window: rebuild at the next frame
					st.core->OnFrameStart(facts);
					return;
				}
				facts.renderSeq = seq;   // AHEAD FIT: the frame starting now (its on-time seal); facts.seq is the bridge's sealed frame
				if (sealed.any)
				{
					facts.seq = sealed.seq;
					facts.p7SealedPublished = sealed.p7Published || !sealed.hadField;
				}
				st.core->OnFrameStart(facts);
				if (st.adapter)
					st.adapter->OnFrame();
				if (st.labelFill)
					st.labelFill->OnFrameStart(st.lane.NextFloor());
				return;
			}
			if (st.fitWanted && st.engineProblem.empty() && st.graphProblem.empty() && st.seedProblem.empty())
				st.liveWait = "waiting for the first vector field from the engine";
			st.core->OnFrameStart(facts);   // not live (yet): the plain report-mode frame start (inert without a graph)
		}

		void CameraExtension::Reconcile(const void* host, int originalFlag, pl_gpu gpu)
		{
			Skeleton::Settings s = Skeleton::Load(loader_);
			if (originalFlag >= 0)
				s.originalPathway = originalFlag != 0;
			// [vpv.camera] (K3): the same file through the same loader (the default one when the hub registered none).
			CameraSettings cam = Load(loader_ ? loader_ : Skeleton::ConfigLoader(&Skeleton::DefaultLoader));
			cam.originalPathway = s.originalPathway;
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
			const bool linkWanted = s.CameraWanted();
			if (!linkWanted && !cam.Wanted())
			{
				if (st)
					Teardown(st, false);
				// An unreadable camera_link or [vpv.camera] key is shown, never ignored.
				StatusInput keyIn;
				keyIn.cam = cam;
				const std::vector<std::string> keyProblems = BuildStatus(keyIn).fallbacks;
				if (!s.originalPathway && (!s.cameraProblems.empty() || !keyProblems.empty()))
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
						if (!problems.empty())
							st->fallbackLines.push_back("camera_link: " + problems);
						for (const std::string& p : keyProblems)
							st->fallbackLines.push_back(p);
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
			st->cam = cam;
			st->dump = DumpRequested();
			st->problem.clear();
			// Camera live: what the keys ask for. The switches and consumers are inert unless camera_model is on (CameraSettings).
			const bool fitWanted = linkWanted && cam.ModelWanted();
			const bool guideRequested = fitWanted && cam.Measurement().Any();
			const bool guideWanted = guideRequested && !guidanceLocked_;   // GUIDANCE LOCK: off this reset (a late camera never guides)
			st->guideLocked = guideRequested && guidanceLocked_;
			const bool labelsWanted = fitWanted && (cam.Overlay() != OverlayMode::Off || cam.Use().Any() || cam.LabelsGpu());
			const bool aheadWanted = fitWanted && cam.AheadFit();
			if (st->liveBuilt && (st->fitWanted != fitWanted || st->guideWanted != guideWanted || st->labelsWanted != labelsWanted ||
				st->aheadWanted != aheadWanted || st->gpu != gpu || st->liveLabelsGpu != cam.LabelsGpu()))
				ReleaseLive(*st);   // rebuilt at the next frame over the new wants
			st->fitWanted = fitWanted;
			st->guideWanted = guideWanted;
			st->labelsWanted = labelsWanted;
			st->aheadWanted = aheadWanted;
			st->gpu = gpu;
			st->graphRefused = false;
			if (!fitWanted)
			{
				st->engineProblem.clear();
				st->graphProblem.clear();
			}
			FrameAnalysis::Store* store = linkWanted ? VpFrameAnalysis::SourceFrameTap::StoreFor(host) : nullptr;
			if (st->core && (!linkWanted || st->builtStore != store))
			{
				ReleaseLive(*st);   // the adapter and the table refer to the core and the store: they go first
				if (st->dumpWriter)
					st->core->SetDump(nullptr);
				st->core->OnGpuReleased();
				st->core.reset();   // camera_link went off, or built over a store that is gone or replaced
				st->builtStore = nullptr;
			}
			if (linkWanted && !st->core)
			{
				if (!store)
					st->problem = "no frame analysis store for this renderer (the renderer does not register one); the camera link cannot report";
				else
				{
					st->core.reset(new CameraLink::CameraLinkCore(*store));
					st->builtStore = store;
				}
			}
			if (st->core)
			{
				// p7Wanted only with camera_model on: then the fit exists once the engine has a field (EnsureLive), else nothing is allocated.
				CameraLink::CameraLinkSettings settings;
				settings.p7Wanted = fitWanted;
				settings.aheadFit = aheadWanted;
				st->core->OnSettings(settings);
				if (st->adapter)
				{
					CameraLink::GuidanceSettings gs;
					gs.switches = cam.Measurement();
					st->adapter->SetSettings(gs);
				}
			}
			if (st->core && gpu)
				st->core->OnGpuCreated(gpu);
			// K3: the dump writer exists only when VP_CAMERA_DUMP names a directory and a core can feed it.
			ReconcileDump(*st, cam);
			// The reader of the camera fit on the Indicator: registered through the one shared seal, once per renderer.
			if (linkWanted && !st->sealHooked)
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
			ReleaseLive(*st);   // adapter (hook off the engine first), core worker, table, GPU objects: before the core goes
			if (st->core)
				st->core->SetDump(nullptr);
			st->dumpWriter.reset();   // joins the writer (K3); a thread never outlives the host state
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
					(st->settings.CameraWanted() && st->builtStore != VpFrameAnalysis::SourceFrameTap::StoreFor(c.host)))
				{
					Reconcile(c.host, -1, nullptr);   // stale core: rebuild over the current store (or say there is none)
					return false;
				}
			}
			if (c.phase == RE::FramePhase::Begin)
			{
				++st->frames;
				DriveFrame(*st, c.host, c.seq);
				Refresh(*st);
			}
			else if (st->core)
			{
				st->core->OnPostSubmitGap();
				if (st->labelFill && st->liveBuilt)
				{
					VpSkeleton::IGapWork* gap = st->labelFill->GapWork();
					if (gap->HasWork())
						gap->RunChunk(0);   // P8 in the post-submit gap, before the seal (one chunk: the upload of one frame's labels)
				}
			}
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
			const bool linkWanted = st.settings.CameraWanted();
			if (st.core)
			{
				lines.push_back(kReportLine);
				st.core->CollectStatus(lines);   // empty while the fit is not wanted
				for (const FallbackCatalog::Decision& d : st.core->CollectDecisions())
					addDecision(d);
			}
			else if (linkWanted)
				lines.push_back("camera: report mode unavailable (" + (st.problem.empty() ? std::string("starting") : st.problem) + ")");

			// Camera live: what cannot attach is shown, never silent (Fallbacks); guidance state through the skeleton's status path.
			if (st.fitWanted)
			{
				if (!st.engineProblem.empty())
					fb.push_back("camera_engine: " + st.engineProblem);
				else if (!st.liveWait.empty())
				{
					// With every camera default off, a camera that is on was turned on explicitly: a missing resync_event is a visible
					// fallback entry (Fallbacks, Ctrl+I, Config status, Ctrl+Alt+D), not a plain status line. "Waiting for the first field" is a state.
					if (st.liveWaitIsFault)
						fb.push_back("camera_fit: " + st.liveWait);
					else
						lines.push_back("camera: " + st.liveWait + "; no fit yet");
				}
				if (st.engineProblem.empty() && !st.seedProblem.empty())
					fb.push_back("camera_fit: " + st.seedProblem);
				if (!st.graphProblem.empty())
					fb.push_back("camera_graph: " + st.graphProblem);
				if (st.liveBuilt && st.lane.MarkerFailures() != 0)
					fb.push_back("camera_graph: " + std::to_string(st.lane.MarkerFailures()) + " input marker(s) could not be published (an input slot "
						"was missing); those frames get no fit");
				if (st.liveBuilt)
				{
					// Standing state, never log-only (Ctrl+I and Fallbacks): P7 / P8 live in the camera's private lag bridge, not in the skeleton
					// slots, until SK-W2 seals them at N + lag in the skeleton lane (link/README.md "Lag bridge until SK-W2").
					const std::string bridge = std::string(kLagBridgeId) + ": " + LagBridgeText();
					lines.push_back("camera: " + LagBridgeText());
					fb.push_back(bridge);
				}
				if (!st.gpu)
					fb.push_back("camera_gpu: no render device for this renderer; the fit cannot read the engine's fields back");
				if (st.guideLocked)
				{
					lines.push_back(std::string("camera: ") + kGuidanceLockedText);
					fb.push_back(std::string("camera_guidance: ") + kGuidanceLockedText);
				}
				if (st.adapter)
				{
					st.adapter->CollectStatus(lines);
					for (const FallbackCatalog::Decision& d : st.adapter->CollectDecisions())
						addDecision(d);
				}
				const unsigned long long unknown = st.sceneUnknown.load(std::memory_order_relaxed);
				if (unknown != 0)
					fb.push_back("camera_scene: " + std::to_string(unknown) + " pair(s) described without a scene record (cut / fade / flash not "
						"known there; flash has no producer yet)");
			}

			// K3: the camera model's own state ([vpv.camera] keys, the fit, the dump), one builder for every surface.
			StatusInput in;
			in.cam = st.cam;
			in.linkReport = linkWanted;
			in.coreAlive = st.core != nullptr;
			in.engineAvailable = st.core && st.core->Allocated();
			in.linkProblem = st.problem;
			if (st.core)
			{
				in.fit = st.core->LastFit();
				const CameraLink::FitCounters& n = st.core->Counters();
				in.fitted = n.fitted.load(std::memory_order_relaxed);
				in.published = n.published.load(std::memory_order_relaxed);
				in.late = n.late.load(std::memory_order_relaxed);
			}
			if (st.dumpWriter)
			{
				in.dumpOn = true;
				in.dumpDirectory = st.dumpWriter->ClipDirectory();
				in.dump = st.dumpWriter->Stats();
			}
			const StatusText k3 = BuildStatus(in);
			for (const std::string& l : k3.lines)
				lines.push_back(l);
			for (const std::string& l : k3.fallbacks)
				fb.push_back(l);

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
			for (const std::string& l : k3.detail)   // Ctrl+Alt+D only
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
				reply = "camera: off (camera_link = off and no [vpv.camera] key set, or the Original pathway)";
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
