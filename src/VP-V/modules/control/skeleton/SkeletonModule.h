// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The skeleton module: activates the analysis lane (core/AnalysisLane.h) through the VP-V hub.
// See README.md in this folder for the data sources, what is live and what is declined.
//
// One lane per renderer (keyed by the `host` of every hub callback, never a global "current renderer"):
// the source frames come from the frame-analysis worker through SourceFrameTap, the one seal per frame
// from ResyncExtension's FrameSeal (a provider + a subscriber), the render-thread phases from OnFrame
// (Begin: OnRenderStart; AfterRender: post-submit gap + retire) and OnQueue (Drop). Off (the default):
// no state, no thread, no tap, no status line. The Original pathway forces off.
//
// 70b G5 (GPU products P10/P11/P12): the pl_gpu is stored at GpuCreated; a SkeletonGpuDispatcher (+ the three
// kernels) exists per renderer only while the lane is wanted AND analysis_lane = on AND a pl_gpu was given. Shadow, Off,
// Original and "no render device" never construct one (no GPU call; the status says why). The dispatcher's texture
// allocation (Configure) and its deferred releases run in AfterRender (GPU-legal, before the gap); with
// skeleton_gpu_gap = after_render the GPU lane jobs are registered as lane producers and run in that gap (visible: ahead
// of the present); with off (default) they are not registered (the products stay in-render-only). The live (in-render)
// form is deferred until the Entrance phase (M2 P4): declined and shown.

#include <VP-V/core/AnalysisLane.h>
#include <VP-V/modules/control/skeleton/P2LaneFeed.h>
#include <VP-V/core/hub/ExtensionHub.h>
#include <VP-V/core/hub/HostSlots.h>
#include <VP-V/modules/control/skeleton/SkeletonSettings.h>
#include <VP-V/modules/control/skeleton/SkeletonFacts.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace VpMotion { class IVectorEngine; }   // modules/motion/estimation/VectorEngineInterface.h (the P5a engine)
namespace VpSkeleton { class SpareHost; }   // core/PostPresentSpare.h (B3-Z Z2: the post-present spare slot)
namespace VpSkeleton { class SkeletonMemo; }   // core/SkeletonAcquire.h (B1: GET-OR-COMPUTE-AND-PUBLISH)
namespace VpSkeleton { class QueueWindowBoard; }   // core/QueueWindow.h (QKP B: the per-host window snapshot)

namespace Vpv
{
	namespace Skeleton
	{
		namespace RE = RendererExtension;

		struct HostState;   // per renderer (SkeletonModule.cpp)
		struct MemoHost;    // per renderer: the GET-OR-COMPUTE-AND-PUBLISH memo, with or without a lane (SkeletonModule.cpp; L-CORE)
		struct Bundle;      // the lane and its producers for one source size (SkeletonModule.cpp)
		class SkeletonGpuDispatcher;   // gpu/SkeletonGpuDispatcher.h (libplacebo; not included here)
		struct GpuSlotTag
		{
			explicit GpuSlotTag(const void* h = nullptr) : host(h) {}
			const void* host = nullptr;
			std::atomic<uint32_t> renderTid{ 0 };   // FIX3-D7: per-host Win32 render thread ID (also covers memo-only hosts)
		};   // the per-renderer pl_gpu (kept as a HostSlots aux) and per-host render TID

		class SkeletonModule : public Hub::ISkeletonModule, public IExternalDemand
		{
		public:
			static constexpr const char* kName = "skeleton";
			static constexpr const char* kLineKey = "skeleton";

			// skeleton_history: engaged only when a capped record history (quality pattern, td trail summary) is in THIS graph.
			static FallbackCatalog::Decision HistoryCapDecision(const VpSkeleton::Graph& graph, double periodMs)
			{
				return FallbackCatalog::SkeletonHistoryCapped(VpSkeleton::GraphHistoryCapNote(graph, periodMs));
			}

			// `loader` reads the configuration (null = the working directory's VideoProcessor.cfg);
			// `nowUs` is the lane's microsecond clock (null = steady_clock).
			explicit SkeletonModule(Skeleton::ConfigLoader loader = Skeleton::ConfigLoader(), std::function<uint64_t()> nowUs = nullptr);
			~SkeletonModule() override;

			const char* Name() const override { return kName; }
			void OnLifecycle(RE::LifecycleFacts&) override;
			void OnIngress(RE::IngressContext&) override;
			void OnQueue(const RE::QueueFacts&) override;
			bool OnFrame(RE::FrameContext&) override;
			// R12 AfterPresent (refs/107 S5): the one post-present seam drives this renderer's spare slot (B3-Z Z2). Inert without an active lane.
			void OnAfterPresent(RE::FrameContext&, RE::PresentContext&) override;
			void OnStatus(const RE::Context&, RE::StatusKind, std::vector<std::string>&, const char*, const char*) override;
			bool OnCommand(const RE::Context&, const char*, const char*, std::string&) override;
			// Query(kExternalDemandQueryId) = this module's IExternalDemand (the camera extension declares its consumers through it).
			void* Query(const char* iid) override;

			// Diagnostics and tests.
			bool Active(const void* host) const;                      // the lane is switched on for this renderer
			bool TapRegistered(const void* host) const;
			size_t HostStates() const;
			// The renderer's lane (null while there is none). Render thread / single-threaded tests only.
			VpSkeleton::AnalysisLane* LaneOf(const void* host) const;
			// B1 (refs/126 s1.2): the renderer's GET-OR-COMPUTE-AND-PUBLISH access path (Acquire / RegisterOwnerFill). L-CORE: the memo is
			// the RENDERER's (made on the first MemoFor or lane build, never in the Original pathway) and exists with or without the lane;
			// a lane binds it while it lives. Null = none made yet. Memo spots live for one frame (released at the next seal).
			VpSkeleton::SkeletonMemo* MemoOf(const void* host) const;
			// L-CORE (lead8/iface-L-CORE.md s2-s3): IExternalDemand's memo access and the module-wide owner registrations.
			FrameMemo MemoFor(const void* host, uint64_t seq) override;
			VpSkeleton::SkeletonMemo::Registration RegisterOwnerFill(VpSkeleton::ProductId id, VpSkeleton::AcquireForm form,
				const std::string& unit, VpSkeleton::OwnerFill fill) override;
			VpSkeleton::SkeletonMemo::Registration ReplaceOwnerFill(VpSkeleton::ProductId id, VpSkeleton::AcquireForm form,
				const std::string& unit, VpSkeleton::OwnerFill fill) override;
			bool UnregisterOwnerFill(VpSkeleton::ProductId id, VpSkeleton::AcquireForm form, const std::string& unit) override;
			// Module-wide registrations held (tests / diagnostics).
			size_t OwnerFillsRegistered() const;
			// The renderer's post-present spare slot (null without host state). Render thread / single-threaded tests only.
			VpSkeleton::SpareHost* SpareOf(const void* host) const;
			// Dimensions of the current lane graph (0 x 0 without one).
			VpSkeleton::Dims DimsOf(const void* host) const;
			bool WorkersRunning(const void* host) const;
			// S5 regulator feed (render thread / single-threaded tests): what OnFrameTiming was given at the last AfterRender. `derived` = the
			// slack came from the measured period and render time because the host delivers no presentation facts.
			struct TimingFeed
			{
				uint64_t frames = 0;        // frames OBSERVED (the Begin..AfterRender span: it includes lock waits and pacing sleeps)
				uint64_t skipped = 0;       // AfterRender without a Begin of the same frame, or with the period unknown (nothing observed)
				uint64_t missed = 0;        // observed frames whose span was above the period
				uint64_t fed = 0;           // frames given to the regulator: ONLY measured render-only spans (NoteRenderOnlyMs); never the observed span
				bool measured = false;      // a measured render-only span has been given (the regulator acts on it)
				double lastSlackMs = 0.0, lastRenderOnlyMs = 0.0, lastPeriodMs = 0.0;
				bool lastMissed = false;
				bool derived = true;        // the figures above are the observed span, not a measured render-only time
				// B4 S2: every host-measured render-only span given to NoteRenderOnlyMs, fed or not (additive).
				uint64_t noted = 0;
				double notedMsSum = 0.0, lastNotedMs = 0.0;
			};
			// B4 S2 (doc 96 section 4): whether a host-measured render-only span reaches the regulator. Observe (default) records it
			// (TimingFeed noted / notedMsSum / lastNotedMs, the status line and Ctrl+Alt+D) and never feeds the regulator, so nothing is shed; Act
			// feeds it as before (AnalysisLane::OnFrameTiming: the regulator sheds). The owner decides the regulator's role; until then Observe.
			enum class RegulatorGate { Observe, Act };
			void SetRegulatorGate(RegulatorGate gate) { regulatorGate_.store(gate == RegulatorGate::Act, std::memory_order_relaxed); }
			RegulatorGate GetRegulatorGate() const { return regulatorGate_.load(std::memory_order_relaxed) ? RegulatorGate::Act : RegulatorGate::Observe; }
			TimingFeed TimingFeedOf(const void* host) const;
			// The regulator's input (render thread, once per frame, any time before the next Begin): the render-only time of frame `seq` that the HOST
			// measured (the PassTimers render span, not the module's Begin..AfterRender clock). Slack = the measured source period - renderOnlyMs,
			// missed = renderOnlyMs > period; with RegulatorGate::Act it goes to AnalysisLane::OnFrameTiming, so the regulator acts (sheds) on it; with
			// Observe (the default) it is only recorded (TimingFeed::noted). Until a measured span arrives the
			// regulator is NOT fed: the module's own span includes lock waits and pacing sleeps, and shedding on that would act on waits. False when
			// there is no lane or no measured period.
			bool NoteRenderOnlyMs(const void* host, uint64_t seq, double renderOnlyMs);
			// The bytes a declared VRAM user still holds although the plan gave it less (a shed user keeps its texture until its module frees it): counted
			// as transient in the next plan, so the products are not granted memory that is still occupied. 0 when nothing is pending.
			uint64_t VramPendingFreeOf(const void* host) const;

			// ---- SK-W stage D: demand and products from outside the lane (the camera link, the vector engine) ----------------------------
			// EXTERNAL DEMAND. A component beside the lane (the camera extension) declares its consumers here (for example
			// CameraLink::DeclareCameraConsumers: P7 / P8 / P9 / p7g with P2 / P3 / P4 as their inputs) and the products it publishes itself
			// (`publishedOutside`, e.g. P7 / P8 / p7g by the camera fit worker straight into the lane's slot table: LaneOf(host)->State()->table).
			// The next frame reconfigures the lane graph, which then holds those products and their ancestors; the declared consumers also start the
			// vector engine (IVectorEngine::SetCameraFitWanted) when any of them reads a camera product. `owner` identifies the declaring object;
			// one entry per (host, owner), replaced by the next call. Any thread. Consumer ids must be unique across owners (the first wins; the
			// rest are counted and shown). Cleared with ClearExternalDemand (the declaring component's GpuReleased / Shutdown).
			void SetExternalDemand(const void* host, const void* owner, const std::vector<VpSkeleton::ConsumerDecl>& consumers,
				const std::vector<VpSkeleton::ProductId>& publishedOutside) override;
			void ClearExternalDemand(const void* host, const void* owner) override;
			// 15c-c8: a RAISING declaration (IExternalDemand::SetRaisingExternalDemand). The entry is the same one (replaced / cleared
			// like any other); while it holds a consumer, the host's effective lane mode is the configured one raised to On (the
			// minimum a consumer that uses the results needs: Shadow's results are not used) with no GPU dispatcher and no GPU
			// products for the report (no consumer of them), and the frame-analysis store is raised (SourceFrameTap::SetStoreDemand).
			// Applied on the render thread at the host's next Begin (Reconcile), never across threads; withdrawn the same way.
			void SetRaisingExternalDemand(const void* host, const void* owner, const std::vector<VpSkeleton::ConsumerDecl>& consumers,
				const std::vector<VpSkeleton::ProductId>& publishedOutside, const std::string& reason) override;
			// The names of the raising consumers of `host` ("LED drive"; several joined by ", "), empty when none (tests / status).
			std::string RaiseReasonOf(const void* host) const;
			// The host lane's current lease of `product` (IExternalDemand::LeaseFor); empty without a lane or the product. Any thread.
			VpSkeleton::LaneLease LeaseFor(const void* host, VpSkeleton::ProductId product) override;
			// External-demand entries held for `host` (tests / diagnostics). Every entry of a host is erased at its GpuReleased / Shutdown and when
			// a new renderer is found at the address of a dead one, so a renderer recreated at the same pointer never inherits a stale demand.
			size_t ExternalEntriesOf(const void* host) const;
			// The engine of a renderer (null = none). Default: Query("vpv.vector_engine.v1") -> IVectorEngineDirectory::EngineFor(host). Tests inject a fake.
			using EngineProvider = std::function<VpMotion::IVectorEngine*(const void* host)>;
			void SetEngineProviderForTesting(EngineProvider provider) { engineProvider_ = std::move(provider); }
			// THE lane CPU job slot of the frame-role detector (P15 quality_pattern, P15c quality_pattern.causal; P15-DECL). DECLARATION ONLY:
			// nothing registers a producer today (both pointers stay null), so the lane is byte-identical. A producer set here is registered on
			// every NEW lane bundle right after p1 / p4, BEFORE its worker threads start (AnalysisLane::RegisterProducer asserts workers are not
			// running); it must outlive the module's lanes (the owner of the detector). Set it before the first frame; a lane already running keeps
			// the producers it was built with. Either pointer may stay null.
			void SetQualityPatternProducers(VpSkeleton::ILaneProducer* p15, VpSkeleton::ILaneProducer* p15c)
			{
				qualityP15_.store(p15, std::memory_order_release);
				qualityP15c_.store(p15c, std::memory_order_release);
			}
			// The ENGINE FEED (render thread): once per frame (AfterRender, retried at Begin) the module publishes the P2m / P3 (and P2) slots of the
			// frame the engine just made a verified field for (IVectorEngine::LatestVerifiedFieldSeq), with the stamp the field was computed under;
			// the frame's own seal at Begin accepts or rejects them like any lane result. The slot CONTENT of P3 is the engine's texture (a GPU
			// product: no kernel copies it into the slot yet, see README); what the module records and serves here are the facts of the field.
			// THREADING: FeedCountersOf and EngineFieldFactsOf are RENDER THREAD ONLY (and tests that drive the frames on their own thread). They
			// read the host state's plain (non-atomic) feed counters and facts ring, which the render thread writes in Begin / AfterRender; a call
			// from another thread would race those writes (torn counters, a facts entry read while it is stored). Not guarded on purpose: a lock
			// on the per-frame feed path would cost every frame for a reader that must sit on the render thread anyway (it consumes the facts
			// for the frame being rendered). Another thread that needs the numbers reads the status lines (Ctrl+I, the command), which are
			// published under the status mutex. SceneFactsOf goes through the memo's Acquire, which serves a published copy off the render thread (see SceneOf).
			FeedCounters FeedCountersOf(const void* host) const;
			// Y1b: the P2 publish sink's counters (atomic reads: any thread). Zero when no sink is installed.
			P2LaneFeed::Counters P2FeedCountersOf(const void* host) const;
			bool EngineFieldFactsOf(const void* host, uint64_t seq, EngineFieldFacts& out) const;
			// Tests: the retry step the module runs at every Begin and AfterRender (the P1 inputs are published by the lane's own threads).
			void RetryEngineFeedForTesting(const void* host);
			// The scene of frame `seq` (normally the render thread, after the seal of `seq`): the U7 v2 sequence verdict over the P4 records
			// around it, the pair fade against `seq - 1` (SkeletonFacts.h SceneFacts). The pair is asked for through the memo (get-or-compute);
			// off the render thread every record is the lane's published copy. False when a record is not readable (SceneFacts::why names it).
			bool SceneFactsOf(const void* host, uint64_t seq, SceneFacts& out) const override;
			// The SCENE PRODUCT (W1-C7): cut / fade of frame `seq` against `seq - 1`, the one reader every consumer uses (the camera extension's
			// ReadScene reads the same store group). Order: the frame-analysis store's scene group (published by this module at the seal of
			// `seq`); else classified from the two P4 records now (SceneFactsOf), written into the store when it holds the frame's record. The
			// answer is identical with frame_analysis_store off (nothing to read, always live). The live path GETS-OR-COMPUTES the two P4 records
			// through the renderer's memo (Acquire): a frame AHEAD of the seal has its P4 made in the lane's slot now or reuses the published
			// one, off the render thread the lane's published bytes are copied, so the path is no longer render-thread-only (the look-ahead
			// walk of the LED reads it). known = false only on a genuine failure; SceneAnswer::why names it (AcquireCause token, no lane ...).
			// Also IExternalDemand::SceneOf, the entry module code outside the skeleton uses (Vpv::Skeleton::SceneOf, SkeletonAccess.h).
			SceneAnswer SceneOf(const void* host, uint64_t seq) const override;
			SceneCounters SceneCountersOf(const void* host) const;
			// GF-P2b S3: IExternalDemand::ExternalErasures (bumped by EraseExternal when it erased an entry of `host`). Any thread.
			uint64_t ExternalErasures(const void* host) const override;
			// W23-4p (G-T1): render-thread GPU P10/P11 product reader and display-facts sync for live dither_need.
			DitherNeedGpuInputs ReadDitherNeedGpuInputs(const void* host, uint64_t seq) const override;
			void SyncDitherNeedDisplayFacts(const void* host, const DitherNeedDisplayFacts& facts) override;

			// The queue the lane graph is sized for: the terms the record histories are derived from (render-ahead R, lane lead L,
			// look-ahead walk frames; VpSkeleton::QueueFramesFor). `wired` says which of the three a feed has delivered (kRenderAhead |
			// kLaneLead | kLookAhead); an undelivered term keeps its default (0 / 1 / 0), so the histories stay at their floors, and the
			// Fallbacks list says so ("record history: queue not yet wired (...); floors in use"). Delivered today: the look-ahead walk,
			// from the host's SettingsApplied call (the motion look-ahead depth). Not delivered: the render-ahead depth (the renderer's
			// runtime gate; the cfg key render_ahead is only on/off). The lane lead (not a setting) is delivered by the host's look-ahead copy
			// (MotionAhead::CollectQueueAhead raises CollectAhead with queuedFutureFrames every render-loop pass; B1 amend, 93-td-readiness-audit 3). A change reconfigures the graph at
			// the next frame, like a moved source period.
			struct QueueShape
			{
				static constexpr uint8_t kRenderAhead = 1, kLaneLead = 2, kLookAhead = 4, kAll = 7;
				uint8_t renderAhead = 0;
				uint8_t laneLead = 1;
				uint16_t lookAheadWalk = 0;
				uint8_t wired = 0;
				bool operator==(const QueueShape& o) const
				{
					return renderAhead == o.renderAhead && laneLead == o.laneLead && lookAheadWalk == o.lookAheadWalk && wired == o.wired;
				}
				bool operator!=(const QueueShape& o) const { return !(*this == o); }
			};
			// The queue the graph was last built from (render thread / tests): `fed` = the snapshot, `used` = after the ring-slot caps.
			struct QueueInUse
			{
				QueueShape fed, used;
				unsigned leadPeak = 0;       // the highest lane lead in use since the lane was built
				uint64_t starveEvents = 0;   // times the lane lead in use fell
			};
			QueueInUse QueueInUseOf(const void* host) const;
			// The live feed's entry (any thread, e.g. a settings change on the UI thread or the renderer's gate): ONE atomic snapshot
			// per host (K#9) that the render thread reads once per frame in Begin(); terms not marked in `q.wired` are reset to their defaults.
			// 0-arg overloads remain backwards-compatible for standalone single-host tests.
			void PublishQueue(const QueueShape& q);
			void PublishQueue(const void* host, const QueueShape& q);
			QueueShape CurrentQueue() const;
			QueueShape CurrentQueue(const void* host) const;
			// The three feeds below are shaped alike (FEED RULE, owner decision 2026-11-07): each one compare-and-swaps ITS term into the one
			// snapshot and leaves the others as they are. A feed must never PublishQueue a fresh QueueShape: that would erase the terms the
			// other feeds delivered. Any thread; a value equal to the published one does not touch the snapshot (change-only).
			//
			// The look-ahead walk term, delivered by the host's SettingsApplied lifecycle call (LifecycleFacts::value = the motion look-ahead
			// depth in frames, 0 = off). Host thread.
			void PublishLookAheadWalk(double frames);
			void PublishLookAheadWalk(const void* host, double frames);
			// The render-ahead depth term: frames the renderer's render-ahead plan keeps ahead of the present (planned lead / source period; a
			// partial frame holds a slot, so it rounds up). The host's one generic call at the render-ahead plan block delivers it.
			void PublishRenderAhead(double frames);
			void PublishRenderAhead(const void* host, double frames);
			// The lane lead term: frames actually queued ahead of the render (the input queue's size at the CollectAhead phase). Stored as fed
			// (0..255); the render thread caps it to the ring-slot budget when it builds the graph (LaneLeadCap, shown).
			void PublishLaneLead(double frames);
			void PublishLaneLead(const void* host, double frames);
			// The ring-slot budget the queue terms share (Products.h kRingHeadroomFrames: a ring holds record history + 1 + R + L + the queue
			// and span slots + the largest lead read, and a graph that needs more than 255 slots is rejected, so the lane would be lost): the
			// room left for render-ahead + lane lead when the record histories sit at their cap.
			static unsigned RingSlotBudget() { return VpSkeleton::kRingHeadroomFrames - 3u - unsigned(VpSkeleton::kMaxCameraLead); }
			static unsigned CappedRenderAhead(unsigned renderAhead) { return renderAhead < RingSlotBudget() ? renderAhead : RingSlotBudget(); }
			static unsigned LaneLeadCap(unsigned renderAhead) { return RingSlotBudget() - CappedRenderAhead(renderAhead); }
			// Withdraws the render-ahead term (not wired again: the depths fall back to their floors and the skeleton_queue entry names it): the plan
			// stopped (render-ahead off), or the host had no source period to express the lead in frames.
			void UnpublishRenderAhead();
			void UnpublishRenderAhead(const void* host);
			// Names of the undelivered terms ("" when all three are wired) and the Fallbacks decision for them.
			static std::string QueueNotWiredNote(uint8_t wired)
			{
				std::string out;
				if (!(wired & QueueShape::kRenderAhead)) out += "render-ahead depth";
				if (!(wired & QueueShape::kLaneLead)) out += std::string(out.empty() ? "" : ", ") + "lane lead";
				if (!(wired & QueueShape::kLookAhead)) out += std::string(out.empty() ? "" : ", ") + "look-ahead walk";
				return out;
			}
			static FallbackCatalog::Decision QueueDecision(uint8_t wired) { return FallbackCatalog::SkeletonQueueNotWired(QueueNotWiredNote(wired)); }
			// Tests only, single-threaded (a test drives the module on one thread): sets all three terms as delivered. Not a feed.
			void SetQueueForTesting(QueueShape q);
			void SetQueueForTesting(const void* host, QueueShape q);
			// Tests: do not start the lane worker threads (the test drives AnalysisLane::StepWorker itself).
			void SetWorkerThreadsForTesting(bool start) { startWorkers_ = start; }
			// Tests: use this card profile instead of the device query (no render device exists in a unit test).
			void SetCardForTesting(const VpSkeleton::CardProfile& card) { cardOverride_ = true; card_ = card; }
			// Test seam (R2b E2 pin): the render pl_gpu of the memo's attached owner GPU device for `host` (null = no memo or no
			// device). Read-only, under ownerFillsMutex_ as the attach / detach sites; no production caller. Any thread.
			const void* MemoDeviceGpuForTesting(const void* host) const;
			// 70b G5: the renderer's GPU dispatcher (null in Off / Shadow / the Original pathway / without a pl_gpu).
			// Render thread / single-threaded tests only.
			const SkeletonGpuDispatcher* GpuDispatcherFor(const void* host) const;
			// Dispatchers ever constructed by any SkeletonModule (tests: Off and Original must leave it unchanged).
			static unsigned long long GpuDispatchersCreated();
			// Hub-contained exceptions in this module (all renderers).
			static unsigned long long ContainedFaults();

		private:
			std::atomic<bool> regulatorGate_{ false };   // B4 S2: false = RegulatorGate::Observe (default)
			void Reconcile(const void* host, int originalFlag);
			void Teardown(const std::shared_ptr<HostState>& st, bool releaseSlot, bool gpuAlive = true);
			size_t EraseExternal(const void* host);   // every external-demand entry of `host` (returns how many)
			void Begin(HostState& st, int64_t seq);
			void AfterRender(HostState& st, int64_t seq, bool retireSourceFrame = true, bool retireOnly = false, uint64_t releaseFenceValue = 0, bool runPostSubmitGap = true, bool secondaryDraw = false);
			void Refresh(HostState& st);
			void OnCollectAhead(HostState& st, const RE::QueueView& view);
			void FeedEngine(HostState& st, bool retryOnly);
			// The scene product's one fill (W1-C7): classifies `seq` from the two P4 records and writes the store's scene group when the store holds
			// the frame's record. False when the pair is not readable. `answer` (optional) gets the classification.
			bool FillScene(HostState& st, uint64_t seq, SceneAnswer* answer) const;
			bool PublishEngineSlots(HostState& st, Bundle& b, EngineFieldFacts& facts, unsigned& doneMask);
			bool CollectExternal(const void* host, std::vector<VpSkeleton::ConsumerDecl>& consumers, std::vector<VpSkeleton::ProductId>& products,
				bool& wantsCameraFit, size_t& duplicates) const;
			static void MergeAtomicBits(std::atomic<uint64_t>& bits, const std::function<void(QueueShape&)>& set);
			uint64_t QueueBitsOf(const void* host) const;
			void EraseHostQueue(const void* host);
			void MergeHostQueue(const void* host, const std::function<void(QueueShape&)>& set);
			void MergeAllQueues(const std::function<void(QueueShape&)>& set);

			Skeleton::ConfigLoader loader_;
			std::function<uint64_t()> nowUs_;
			uint64_t codingProviderToken_ = 0;   // P16: this module's CodingEvidence::Hub provider installation (cleared only if still ours)
			Hub::HostSlots<HostState> hosts_;
			Hub::HostSlots<GpuSlotTag> gpuSlots_;   // aux = the renderer's pl_gpu (GpuCreated), cleared at GpuReleased
			Hub::HostSlots<MemoHost> memoHosts_;    // L-CORE: the renderers' memos (any thread; dropped at GpuReleased / Shutdown)
			// L-CORE: the module-wide owner fills (IExternalDemand::RegisterOwnerFill), installed in every memo made.
			mutable std::mutex ownerFillsMutex_;
			std::map<std::pair<VpSkeleton::ProductId, VpSkeleton::AcquireForm>, std::pair<std::string, VpSkeleton::OwnerFill>> ownerFills_;
			std::shared_ptr<VpSkeleton::SkeletonMemo> MemoHostOf(const void* host, bool create) const;
			// L-CORE (102b P13-NO-LANE-GPU-ALLOC) / FIX3-D7: the VRAM ledger grant and GPU presence of a memo no lane is bound to
			// (strictly per-host render-thread-only: guarded by RenderTidOf(host) recorded on render-thread entry points). Re-planned
			// when the need, the declared VRAM users, the GPU presence or the binding changed, else every kNoLaneReplanFrames.
			void PlanNoLaneGpu(const void* host);
			void RecordRenderTid(const void* host);
			uint32_t RenderTidOf(const void* host) const;
			// QKP B: the queue window of this Begin (derived before the memo seal, published to the board); the board is read by any thread.
			void DeriveMemoWindow(const void* host, const std::shared_ptr<MemoHost>& mh, uint64_t seq, const std::shared_ptr<HostState>& st, bool sampleTiming = true);
		public:
			const VpSkeleton::QueueWindowBoard* WindowBoardOf(const void* host) const;
		private:
			bool cardOverride_ = false;
			bool startWorkers_ = true;
			struct ExternalEntry
			{
				const void* host = nullptr;
				const void* owner = nullptr;
				std::vector<VpSkeleton::ConsumerDecl> consumers;
				std::vector<VpSkeleton::ProductId> products;
				std::string raise;   // 15c-c8: non-empty = a raising declaration (the consumer's name for Ctrl+I)
			};
			mutable std::mutex externalMutex_;
			std::vector<ExternalEntry> external_;
			std::atomic<uint64_t> externalVersion_{ 0 };
			std::vector<std::pair<const void*, uint64_t>> externalErasures_;   // GF-P2b S3: per host address, under externalMutex_ (only grows)
			// 15c-c8: bumped whenever the set of raising entries (or a reason) changes; a host's Begin compares it with the version its
			// last Reconcile applied (raiseSeen_) and reconciles on the render thread. The pathway each host last reported at
			// SettingsApplied is kept beside it: a demand-driven Reconcile never raises in the Original pathway (the renderer's word).
			struct RaiseSeen
			{
				const void* host = nullptr;
				uint64_t version = 0;
				int originalFlag = -1;   // -1 = no SettingsApplied seen yet
				std::string applied;     // the raise reason the last Reconcile applied (empty = none)
				bool appliedWantsGpu = false;   // W23-4p: whether the last applied raise included a GPU product consumer
			};
			std::atomic<uint64_t> raiseVersion_{ 1 };
			std::vector<RaiseSeen> raiseSeen_;   // under externalMutex_
			std::string RaiseReasonLocked(const void* host) const;
			bool RaisingWantsGpuLocked(const void* host) const;
			bool RaisingWantsGpuOf(const void* host) const;
			bool RaisingLocked(const ExternalEntry& e) const { return !e.raise.empty() && !e.consumers.empty(); }
			void NoteRaiseApplied(const void* host, uint64_t version, const std::string& applied, bool wantsGpu);
			void NotePathway(const void* host, int originalFlag);
			int RecordedPathway(const void* host) const;
			void ReconcileIfRaiseMoved(const void* host);
			EngineProvider engineProvider_;
			std::atomic<VpSkeleton::ILaneProducer*> qualityP15_{ nullptr };    // see SetQualityPatternProducers (null = none, the lane is unchanged)
			std::atomic<VpSkeleton::ILaneProducer*> qualityP15c_{ nullptr };
			VpSkeleton::CardProfile card_;
			static uint64_t Pack(QueueShape q)
			{
				if (!(q.wired & QueueShape::kRenderAhead)) q.renderAhead = 0;
				if (!(q.wired & QueueShape::kLaneLead)) q.laneLead = 1;
				if (!(q.wired & QueueShape::kLookAhead)) q.lookAheadWalk = 0;
				return uint64_t(q.renderAhead) | (uint64_t(q.laneLead) << 8) | (uint64_t(q.lookAheadWalk) << 16) | (uint64_t(q.wired) << 32);
			}
			static QueueShape Unpack(uint64_t bits)
			{
				QueueShape q;
				q.renderAhead = uint8_t(bits & 0xFF);
				q.laneLead = uint8_t((bits >> 8) & 0xFF);
				q.lookAheadWalk = uint16_t((bits >> 16) & 0xFFFF);
				q.wired = uint8_t((bits >> 32) & 0xFF);
				return q;
			}
			std::atomic<uint64_t> queueBits_{ uint64_t(1) << 8 };   // legacy 0-arg fallback snapshot; initial value = Pack of default QueueShape (0 / 1 / 0, nothing wired)
			mutable std::mutex hostQueueMutex_;                     // K#9: pre-creation per-host QueueShape storage (seeded into HostState / MemoHost on creation)
			mutable std::vector<std::pair<const void*, uint64_t>> hostQueues_;
		};

		// Adds SkeletonModule to the hub. Start-up only; idempotent. After RegisterResyncExtension.
		void RegisterSkeletonModule();
	}
}
