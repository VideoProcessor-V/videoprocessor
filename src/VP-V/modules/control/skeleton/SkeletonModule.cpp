// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only

#include <pch.h>
#include "SkeletonModule.h"

#include <DebugLog.h>

#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/core/GpuAllocator.h>
#include <VP-V/core/PassTimerInbox.h>
#include <VP-V/core/PostPresentSpare.h>
#include <VP-V/core/QueueFrames.h>
#include <VP-V/core/ReserveRows.h>
#include <VP-V/core/SkeletonAcquireLane.h>
#include <VP-V/core/hub/CurrentFrame.h>
#include <VP-V/core/MemoGpuGrant.h>
#include <VP-V/modules/control/skeleton/SlotBudgetFeedHost.h>
#include <VP-V/core/VramUsers.h>
#include <VP-V/modules/motion/estimation/MotionValidity.h>
#include <VP-V/modules/motion/estimation/VectorEngineInterface.h>
#include <VP-V/modules/input/signal/ResyncExtension.h>
#include <VP-V/core/FrameAnalysisStore.h>
#include <VP-V/modules/input/source_analysis/SourceFrameTap.h>
#include <VP-V/modules/control/skeleton/gpu/OwnerGpuDevice.h>
#include <VP-V/modules/input/source_analysis/FrameQuiet.h>
#include <VP-V/modules/control/skeleton/gpu/SkeletonGpuDispatcher.h>
#include <VP-V/modules/input/source_analysis/lane/LaneCpuProducts.h>
#include <VP-V/modules/input/source_analysis/lane/LaneGpuGrainKernel.h>
#include <VP-V/modules/input/source_analysis/lane/LaneGpuJobs.h>
#include <VP-V/modules/input/source_analysis/lane/LaneGpuLightKernel.h>
#include <VP-V/modules/input/source_analysis/lane/LaneGpuRegionKernel.h>
#include <VP-V/modules/input/source_analysis/regions/CodingEvidence.h>
#include <VP-V/modules/input/source_analysis/SourceResolutionMonitor.h>

#include <d3d11.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

namespace Vpv
{
	namespace Skeleton
	{
		using VpSkeleton::AnalysisLane;
		using VpSkeleton::CardProfile;
		using VpSkeleton::ConsumerDecl;
		using VpSkeleton::Dims;
		using VpSkeleton::GraphSettings;
		using VpSkeleton::LaneSettings;
		using VpSkeleton::SourceFrameView;
		using VpSkeleton::TierInput;

		namespace
		{
			constexpr size_t kPendingViews = 8;      // source frames held until the lane graph exists
			constexpr size_t kDropQueue = 128;       // queue drops waiting for the render thread
			constexpr uint64_t kMaxDropsPerEvent = 16;
			constexpr uint64_t kEarlyRefreshFrames = 8;
			constexpr uint64_t kRefreshEvery = 16;
			constexpr uint64_t kReconfigureMinFrames = 60;
			using VpSkeleton::kLeadWindowSeconds;     // source time the lane-lead feed keeps its lowest value over (core/QueueFrames.h)
			using VpSkeleton::kLeadPeakHoldSeconds;   // the lane lead's best value is held this long, then decays to the lead in use
			constexpr size_t kFactsRing = 64;           // engine-field facts kept (frames)
			constexpr size_t kSceneRunRing = 8;         // U7 fade runs kept (frames) and the rebuild depth on a ring miss; the law itself has no
			                                            // frame count, but a REDUCED rebuild (no ring entry joined) starts its run at most 7 pairs back
			                                            // (counted: sceneRunReduced, the "Skeleton scene" status line)
			constexpr unsigned kFeedRetries = 8;        // publish attempts per field before it is given up (Begin + AfterRender per frame)

			uint64_t SteadyUs()
			{
				return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
					std::chrono::steady_clock::now().time_since_epoch()).count());
			}

			std::atomic<unsigned long long>& Faults()
			{
				static std::atomic<unsigned long long> n{ 0 };
				return n;
			}

			bool SameDims(Dims a, Dims b) { return a.width == b.width && a.height == b.height; }

			// The engine of a renderer through the hub's query (null when none is registered or the renderer has no engine yet).
			VpMotion::IVectorEngine* DefaultEngine(const void* host)
			{
				VpMotion::IVectorEngineDirectory* dir = static_cast<VpMotion::IVectorEngineDirectory*>(RendererExtension::Query(VpMotion::kVectorEngineQueryId));
				return dir ? dir->EngineFor(host) : nullptr;
			}

			// A consumer that reads a camera product: the camera fit is wanted (the vector engine must run).
			bool ReadsCameraProduct(const VpSkeleton::ConsumerDecl& c)
			{
				for (size_t i = 0; i < c.count; ++i)
				{
					const VpSkeleton::ProductId p = c.consumes[i].product;
					if (p == VpSkeleton::kP7CameraFit || p == VpSkeleton::kP8CameraLabels || p == VpSkeleton::kP9CameraSmoothed || p == VpSkeleton::kP7gCameraGuide)
						return true;
				}
				return false;
			}

			// 15c-c8: a consumer that reads the engine's pair or field (published by the engine feed, never made by the lane).
			bool ReadsEngineProduct(const VpSkeleton::ConsumerDecl& c)
			{
				for (size_t i = 0; i < c.count; ++i)
				{
					const VpSkeleton::ProductId p = c.consumes[i].product;
					if (p == VpSkeleton::kP2VectorPair || p == VpSkeleton::kP2mMatcherPair || p == VpSkeleton::kP3AnalysisField)
						return true;
				}
				return false;
			}

			std::atomic<unsigned long long>& DispatchersCreated()
			{
				static std::atomic<unsigned long long> n{ 0 };
				return n;
			}

			// Lane GPU job cost prediction: the declaration's reference ms scaled by the pixel count, times this
			// safety factor, until the dispatcher has a measured time (the larger of the two is used).
			constexpr double kGpuPredictSafety = 2.0;

			GrainSplit::Storage RequestedStorage(const Settings& s)
			{
				return s.grainStorage == GrainStorage::Compact ? GrainSplit::Storage::Compact : GrainSplit::Storage::Full;
			}

			// VRAM and adapter facts for the tier plan: the render device's adapter, local segment. A failure
			// leaves zeros and says why (shown, not silent).
			bool QueryCard(void* device, CardProfile& card, std::string& why)
			{
				MEMORYSTATUSEX mem = {};
				mem.dwLength = sizeof(mem);
				if (GlobalMemoryStatusEx(&mem))
					card.systemRamFreeBytes = static_cast<uint64_t>(mem.ullAvailPhys);
				ID3D11Device* dev = static_cast<ID3D11Device*>(device);
				if (!dev)
				{
					why = "no render device (video memory unknown)";
					return false;
				}
				Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
				Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
				Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter3;
				if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) || FAILED(dxgiDevice->GetAdapter(&adapter)) ||
					FAILED(adapter.As(&adapter3)))
				{
					why = "the adapter cannot report video memory";
					return false;
				}
				DXGI_ADAPTER_DESC desc = {};
				if (SUCCEEDED(adapter->GetDesc(&desc)))
					card.key.luid = (static_cast<uint64_t>(static_cast<uint32_t>(desc.AdapterLuid.HighPart)) << 32) |
						static_cast<uint64_t>(desc.AdapterLuid.LowPart);
				// W2-C1 (B9): the motion engine's reading of the same adapter, when at most a second old (one DXGI reading a second).
				uint64_t sharedUsage = 0, sharedBudget = 0;
				if (card.key.luid != 0 && VpSkeleton::VramUsers::Instance().LastBudget(card.key.luid, sharedUsage, sharedBudget,
					static_cast<double>(GetTickCount64()) / 1000.0))
				{
					card.vramBudgetBytes = sharedBudget;
					card.vramUsageBytes = sharedUsage;
					return true;
				}
				DXGI_QUERY_VIDEO_MEMORY_INFO info = {};
				if (FAILED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
				{
					why = "QueryVideoMemoryInfo failed";
					return false;
				}
				card.vramBudgetBytes = info.Budget;
				card.vramUsageBytes = info.CurrentUsage;
				return true;
			}
		}

		// What the GPU products of a lane are wired to (70b G5). `dispatcher` null = none (Off / Shadow / no pl_gpu).
		struct GpuWiring
		{
			SkeletonGpuDispatcher* dispatcher = nullptr;
			bool gapAhead = false;       // skeleton_gpu_gap = after_render: the three GPU lane jobs are registered
			bool compactGrain = false;   // P10 declared (and made) Compact
			bool Same(const GpuWiring& o) const { return dispatcher == o.dispatcher && gapAhead == o.gapAhead && compactGrain == o.compactGrain; }
		};

		// Everything that depends on the source dimensions: the producers (P4 is built for a size), the
		// lane and its worker threads. Replaced as a whole when the source size or the GPU wiring changes.
		struct Bundle
		{
			Bundle(Dims d, uint64_t seq, const std::function<uint64_t()>& clock, const GpuWiring& gpuWiring,
				std::shared_ptr<VpSkeleton::SkeletonMemo> hostMemo, VpSkeleton::LaneLeaseOf leaseOf,
				VpSkeleton::ILaneProducer* qualityP15 = nullptr, VpSkeleton::ILaneProducer* qualityP15c = nullptr)
				: dims(d), createdSeq(seq), wiring(gpuWiring), memo(std::move(hostMemo)),
				  // Without a GPU the registry is exactly the one the lane always used; with one, the tap form (P10 / P11 read
				  // the tap plane, not the P13 ring that is RenderOnly and never filled: without this P10 is InputMissing).
				  registry(MakeRegistry(gpuWiring, 0.0, 0)),
				  p1(clock), p4(d, clock), lane(std::shared_ptr<VpIndicator::Indicator>(), clock), threads(lane)
			{
				lane.SetExternalSeal(true);   // FrameSeal owns the one seal
				lane.RegisterProducer(&p1);
				lane.RegisterProducer(&p4);
				// The P15 / P15c frame-role detector slot (P15-DECL): inert with null providers (nothing is registered); registered here, before the
				// worker threads start, exactly like p1 / p4.
				if (qualityP15 != nullptr) lane.RegisterProducer(qualityP15);
				if (qualityP15c != nullptr) lane.RegisterProducer(qualityP15c);
				lane.RegisterCpuLiveCompute(VpSkeleton::kP1HalfLuma,
					[](const VpSkeleton::Node&, const SourceFrameView* v, const std::array<const uint8_t*, VpSkeleton::kMaxInputs>&,
						uint8_t* dst, size_t n) { return v != nullptr && VpLaneProducts::ComputeHalfLuma(*v, dst, n); });
				lane.RegisterCpuLiveCompute(VpSkeleton::kP4SourceStats,
					[d](const VpSkeleton::Node&, const SourceFrameView*, const std::array<const uint8_t*, VpSkeleton::kMaxInputs>& in,
						uint8_t* dst, size_t n) { return in[0] != nullptr && VpLaneProducts::ComputeSourceStats(in[0], d, dst, n); });
				if (wiring.dispatcher != nullptr)
				{
					// Producers are registered BEFORE the worker threads start (RegisterProducer asserts). Without
					// skeleton_gpu_gap the GPU products have no lane producer: they are in-render-only.
					if (wiring.gapAhead)
					{
						AddGpuJob(VpSkeleton::kP10GrainSplit, job10);
						AddGpuJob(VpSkeleton::kP11RegionBlockStats, job11);
						AddGpuJob(VpSkeleton::kP12LightLevel, job12);
						if (job12)
						{
							// P12(N) needs P12(N-1) (the one-pole state): readable in the ring or live, else Begin says no.
							const AnalysisLane* l = &lane;
							SkeletonGpuDispatcher* disp = wiring.dispatcher;
							std::atomic<uint64_t>* pendingAsNone = &lightPendingAsNone;   // a Bundle member: it outlives the job
							job12->SetBeginGate([l, disp, pendingAsNone](const VpSkeleton::LaneJobContext& job) {
								if (!disp->Available(VpSkeleton::kP12LightLevel))
									return false;   // refused / not allocated yet: declined, shown by the dispatcher's own entry
								return VpLaneProducts::LightLevelBeginGate(job.seq, [l, pendingAsNone](uint64_t s) {
									// LightPrevStateOf: Pending reads as None until G6 (a pending live fill is run by nothing before it).
									// Counted and shown (skeleton_gpu_light_pending), never silent.
									return VpLaneProducts::LightPrevStateOf(l->ReadOf(VpSkeleton::kP12LightLevel, s).state, *pendingAsNone);
								});
							});
						}
					}
					// The dispatcher serves the GPU products; their live (in-render) form is only RECORDED here and runs at
					// the Entrance phase (RunDeferredLive, M2 P4): until then those fills stay pending (shown).
					lane.SetGpuDispatcher(wiring.dispatcher);
					lane.SetGpuLiveDeferred(true);
					wiring.dispatcher->SetLane(&lane);
				}
				else
				{
					// No dispatcher: every other product (P10, P11, P12, ...) has no producer and no live fill. Declined,
					// counted. (liveFill_ wins over a dispatcher in LiveFillOf, so this is installed ONLY without one.)
					lane.SetLiveFill([this](const VpSkeleton::Node&, uint64_t, int, VpSkeleton::Header&, uint32_t) {
						liveDeclined.fetch_add(1, std::memory_order_relaxed);
						return false;
					});
				}
				// B1 (refs/126 s1.2): the one GET-OR-COMPUTE-AND-PUBLISH path, on the lane's slots and its CPU live computes (one law). Without a
				// dispatcher a GPU form is Unavailable(no GPU dispatcher), counted and on the status lines (never a consumer's private path).
				memo->SetNames(&registry);
				VpSkeleton::LaneGpuHandleOf gpuHandleOf;
				if (SkeletonGpuDispatcher* disp = wiring.dispatcher)
				{
					// A GPU lane product reused through Acquire names the dispatcher's texture of that slot (term 0) and the fence to wait on.
					gpuHandleOf = [disp](VpSkeleton::ProductId id, bool ring, int index, uint64_t fence, VpSkeleton::GpuMemoHandle* out) {
						const VpLaneProducts::GpuProductTextures* set = disp->TexturesOf(id);
						if (set == nullptr || index < 0)
							return false;
						const pl_tex tex = ring ? set->Ring(size_t(index), 0) : set->Live(size_t(index), 0);
						if (tex == nullptr)
							return false;
						out->resource = const_cast<void*>(static_cast<const void*>(tex));
						out->textureSet = set;
						out->index = index;
						out->ringSlot = ring;
						out->fence = fence;
						out->width = uint32_t(tex->params.w);
						out->height = uint32_t(tex->params.h);
						return true;
					};
				}
				// L-CORE: the memo is the renderer's (it outlives this lane); off the render thread the lane's CPU products are read through
				// `leaseOf` (the host's CURRENT bundle, held for the call: never this object after it ended).
				// GF-P1 AHEAD-DECL call site (GF-P2b): the consumers are not known before the first Configure, so this first bind carries none;
				// BindConsumers (after every Configure) rebinds with the configured list whenever the lane fills' ahead depths it gives move.
				boundGpuHandleOf = gpuHandleOf;
				boundLeaseOf = leaseOf;
				VpSkeleton::BindLaneToMemo(lane, *memo, gpuHandleOf, std::move(leaseOf));
			}

			// GF-P1 AHEAD-DECL call site (GF-P2b, gate G6 C2): the lane fills' ahead depths follow the configured consumers (external demand
			// included). The graph fingerprint leaves ConsumerDecl::aheadDepth out (a depth is not graph structure), so a change of depth alone
			// keeps the graph: the key below is what decides the rebind, not the graph. BindLane replaces the binding (laneBinds_ moves). The
			// workers call the lane's ahead sinks, which the binder reassigns: they are joined first when running (Begin restarts them).
			// Render thread. True = rebound.
			bool BindConsumers(const std::vector<VpSkeleton::ConsumerDecl>& consumers)
			{
				std::vector<AheadKey> key;
				for (const VpSkeleton::ProductId id : lane.CpuLiveProducts())
				{
					const VpSkeleton::LaneAheadDepth d = VpSkeleton::LaneAheadDepthOf(id, 0, consumers);
					AheadKey k;
					k.id = id;
					k.depth = d.depth;
					k.asked = d.asked;
					k.by = d.by;
					key.push_back(k);
				}
				if (boundWithConsumers && key == boundAheadKey)
					return false;
				if (threads.Running())
					threads.Stop();
				VpSkeleton::BindLaneToMemo(lane, *memo, boundGpuHandleOf, boundLeaseOf, consumers);
				boundAheadKey = key;
				boundWithConsumers = true;
				return true;
			}
			struct AheadKey
			{
				VpSkeleton::ProductId id = 0;
				uint16_t depth = 0;
				uint16_t asked = 0;
				const char* by = nullptr;
				bool operator==(const AheadKey& o) const { return id == o.id && depth == o.depth && asked == o.asked && by == o.by; }
			};
			// The binding is dropped on the render thread by StopBundle / a rebuild's new bind; this is the safety net (by token: a newer
			// lane's binding is kept).
			~Bundle() { VpSkeleton::UnbindLaneFromMemo(lane, *memo); }

			// The registry for a wiring, the measured source period and the queue frames (0 = the unmeasured defaults). Re-made at each
			// Configure so the record histories follow the period and the queue; the GPU declarations do not depend on either.
			static VpSkeleton::Registry MakeRegistry(const GpuWiring& w, double periodMs, unsigned queueFrames)
			{
				VpSkeleton::Registry reg = VpSkeleton::SkeletonProducts(w.compactGrain, w.compactGrain, periodMs, queueFrames);
				return w.dispatcher != nullptr ? VpLaneProducts::TapFormProducts(reg) : reg;
			}

			// One fixed-step GPU lane job (steps = StepsFor of the dims; cost from the dispatcher's measurement, else a
			// conservative figure derived from the declaration's reference ms and the pixel count).
			void AddGpuJob(VpSkeleton::ProductId id, std::unique_ptr<VpLaneProducts::GpuComputeLaneJob>& slot)
			{
				const uint32_t steps = VpLaneProducts::StepsFor(id, dims);
				if (steps == 0 || !registry.Has(id))
					return;
				SkeletonGpuDispatcher* disp = wiring.dispatcher;
				const double refMs = registry.Get(id).refRenderMs;
				const double pixels = double(dims.width) * double(dims.height);
				std::function<double()> predicted = [disp, id, refMs, pixels]() {
					const double derived = refMs * (pixels / VpSkeleton::kReferencePixels) * kGpuPredictSafety;
					const double measured = disp->MeasuredMs(id);
					return measured > derived ? measured : derived;
				};
				slot.reset(new VpLaneProducts::GpuComputeLaneJob(VpLaneProducts::GpuComputeLaneJob::FixedStepsTag(), id, disp,
					std::move(predicted), steps));
				// A product the dispatcher could not allocate (refused: shown) or has not allocated yet declines its lane job
				// instead of failing every chunk (P12 adds its own gate below).
				slot->SetBeginGate([disp, id](const VpSkeleton::LaneJobContext&) { return disp->Available(id); });
				lane.RegisterProducer(slot.get());
			}

			Dims dims;
			uint64_t createdSeq;
			GpuWiring wiring;
			VpSkeleton::Registry registry;   // what the lane (and the dispatcher's Configure) are built on
			// Declared before the lane: they outlive its state (the runner points at them).
			std::atomic<uint64_t> lightPendingAsNone{ 0 };   // P12 gate: a Pending previous frame read as not made (until G6)
			VpLaneProducts::HalfLumaProducer p1;
			VpLaneProducts::SourceStatsProducer p4;
			std::unique_ptr<VpLaneProducts::GpuComputeLaneJob> job10, job11, job12;   // null unless the GPU gap is on
			std::shared_ptr<VpSkeleton::SkeletonMemo> memo;   // B1 / L-CORE: the renderer's memo, declared before the lane (bound to it)
			AnalysisLane lane;
			VpSkeleton::LaneWorkerThreads threads;
			std::atomic<uint64_t> liveDeclined{ 0 };
			// Render thread.
			uint32_t configuredGen = 0;
			double configuredPeriodMs = 0.0;
			SkeletonModule::QueueShape configuredQueue;
			uint64_t configuredVramVersion = 0;   // VramUsers::Version() at the last Configure
			uint64_t configuredExtVersion = 0;     // externalVersion_ at the last Configure
			uint64_t configuredMemoNeed = 0;       // GEM-W1A S1 / GF-P2b MUST-4: the memo's top GPU need level planned at the last Configure (0 = none)
			// GF-P2b (AHEAD-DECL call sites): what the memo binding was made with, for BindConsumers' rebinds.
			VpSkeleton::LaneGpuHandleOf boundGpuHandleOf;
			VpSkeleton::LaneLeaseOf boundLeaseOf;
			std::vector<AheadKey> boundAheadKey;
			bool boundWithConsumers = false;
			uint64_t framesSinceConfigure = 0;
		};

		struct HostState
		{
			explicit HostState(const void* h) : host(h), ownerGen(VpFrameAnalysis::SourceFrameTap::Generation(h)),
				seenEpoch(VpFrameAnalysis::SourceFrameTap::Epoch()) {}
			const void* host;
			const uint64_t ownerGen;   // the tap generation of the renderer this state was built for (address reuse = stale)
			uint64_t seenEpoch;

			// Written by the lifecycle thread (the render thread), read there.
			Settings settings;
			std::atomic<bool> active{ false };
			std::atomic<bool> reporting{ false };   // off, but an invalid key is being shown
			std::atomic<uint32_t> settingsGen{ 1 };
			bool sealHooked = false;
			bool sealLateHook = false;
			bool tapOn = false;
			std::string cardNote;           // why video memory is unknown, if it is
			// 70b G5 (lifecycle / render thread): the renderer's pl_gpu (GpuCreated) and, only for analysis_lane = on with a
			// pl_gpu, the GPU dispatcher with its three kernels. Null in Off / Shadow / Original / no render device.
			pl_gpu gpu = nullptr;
			std::unique_ptr<SkeletonGpuDispatcher> dispatcher;
			GrainSplit::Storage dispatcherStorage = GrainSplit::Storage::Full;
			bool dispatcherCompact = false;   // the storage the kernel was GRANTED (declared the same way)
			pl_gpu dispatcherGpu = nullptr;
			// Render thread: set when the dispatcher's private working memory does not fit the free video memory (the GPU
			// products are then not wired; shown as skeleton_gpu_memory). Cleared at a settings or size change.
			std::string gpuMemoryNote;
			uint64_t gpuMemorySizeKey = 0;
			uint32_t gpuMemoryGen = 0;

			// Any thread.
			std::atomic<uint32_t> srcW{ 0 }, srcH{ 0 };
			std::atomic<uint64_t> srcFrames{ 0 };
			std::shared_ptr<Bundle> bundle;   // std::atomic_load / atomic_store

			std::mutex pendingMutex;
			std::deque<SourceFrameView> pending;
			std::atomic<uint64_t> pendingEvicted{ 0 };

			std::mutex dropMutex;
			std::vector<uint64_t> drops;
			std::atomic<uint64_t> dropsLost{ 0 };
			std::atomic<uint64_t> dropsInferred{ 0 };   // dropped sequences inferred as contiguous before the survivor

			// Render thread.
			uint64_t frames = 0;
			uint64_t lastBeginUs = 0;
			double periodMs = 0.0;
			uint64_t gapsSkipped = 0;
			uint64_t gapsRun = 0;
			uint64_t dropsApplied = 0;
			uint64_t configures = 0;
			std::string waiting;
			std::string shownWaiting;
			bool shownBundle = false;
			std::string shownVetoOff;   // the cut veto state the cached status text was built for ("" = on)
			bool shownNoGrid = false;   // the no-grid fallback state the cached status text was built for
			uint8_t shownQueueWired = 0xFF;   // the queue-delivery state the cached status text was built for
			Vpv::Skeleton::SlotBudgetFeed budgetFeed;   // S0: feeds spare.SetBudget once per source frame + Presenter::spareSlotMs (SlotBudgetFeed.h)
			VpSkeleton::SpareHost spare;       // B3-Z Z2: the post-present spare slot, driven by R12 AfterPresent (OnAfterPresent)
			uint64_t shownQueueKey = ~uint64_t(0);   // the queue-in-use / starve state the cached status text was built for

			// S5 regulator feed (render thread): the module's own clock between its Begin and AfterRender of one frame.
			uint64_t renderBeginUs = 0;
			int64_t renderBeginSeq = 0;
			SkeletonModule::TimingFeed timing;

			// The queue in use (render thread; Begin): the snapshot after the ring-slot caps, the best lane lead and the falls.
			SkeletonModule::QueueShape queueFed, queueUsed;
			unsigned leadPeak = 0;               // the best lane lead of the last kLeadPeakHoldSeconds (decays: not a permanent high-water mark)
			uint64_t leadPeakAtUs = 0;
			unsigned lastUsedLead = 0;
			uint64_t starveEvents = 0;
			std::string starveReduced;   // look-ahead products that lost their lead in the current graph (names)

			// SK-W stage D (render thread): external demand and the engine feed.
			size_t externalConsumers = 0;        // consumers declared from outside, in the current graph
			size_t externalDuplicates = 0;       // declared ids that were already taken (ignored, shown)
			bool wantsCameraFit = false;         // an external consumer reads a camera product
			bool feedNeeded = false;             // an external consumer reads P2 / P2m / P3 or a camera product: the engine feed runs
			bool fitWantedPushed = false;        // IVectorEngine::SetCameraFitWanted(true) was sent
			// 15c-c8 (render thread, Reconcile): the raising consumers that turned this lane on (empty = the lane runs as configured) and
			// the configured mode beside it. A raised lane is On with no GPU dispatcher, and its report reads only P1 / P4.
			std::string laneRaise;
			VpSkeleton::LaneSettings::Mode configuredLaneMode = VpSkeleton::LaneSettings::Mode::Off;
			VpMotion::IVectorEngine* engine = nullptr;   // this renderer's engine; dropped at Teardown (valid until the renderer goes)
			FeedCounters feed;
			// The scene product's counters (W1-C7): written by the render thread (seal) and by any SceneOf caller, so atomic and relaxed.
			std::atomic<uint64_t> scenePublished{ 0 }, sceneAlready{ 0 }, sceneNotReady{ 0 }, sceneStoreReads{ 0 }, sceneLiveFills{ 0 }, sceneCacheHits{ 0 }, sceneUnknown{ 0 };
			// The live classification of ONE frame, kept until the next Begin (a frame number can restart after a seek, so nothing outlives a frame).
			std::mutex sceneMutex;
			uint64_t sceneCacheSeq = 0;
			bool sceneCacheValid = false;
			bool sceneCacheCut = false, sceneCacheFade = false;
			bool sceneCacheFlash = false, sceneCacheCutPossible = false, sceneCacheConfirmed = false, sceneCacheLate = false, sceneCacheHere = false;
			float sceneCacheStrength = 0.0f;
			float sceneCacheFadeKeep = -1.0f;   // U7 v3: the frame's history keep (FrameAnalysis::Scene::fadeKeep)
			std::atomic<uint64_t> sceneFadePartial{ 0 };   // U7 v3: fades classified with a history keep below 1 (partial reset, no restart)
			// U7 v2 (CutFadeJudge sequence law, U7-accuracy): the tracker over this renderer's P4 records in frame order, the newest frame it
			// holds (with a fingerprint of that record, so a frame number reused after a seek re-primes it) and the recent verdicts by frame
			// (with their record fingerprints). All under sceneMutex. A verdict is provisional until frame seq + 1 was judged with it.
			struct SceneVerdictSlot
			{
				bool used = false;
				uint64_t seq = 0;
				uint64_t print = 0;
				VpUnit::CutFadeJudge::CutVerdict v;   // known = false until judged
				bool deliveredOwn = false;            // this frame's own cut went out with its group (confirmed at its publish)
				bool deliveredLate = false;           // this frame's own cut went out late, with a later frame's group
				bool carriesLate = false;             // this frame's group carries an earlier frame's late cut (a re-fill keeps it)
			};
			static constexpr size_t kSceneVerdicts = 16;
			std::unique_ptr<VpUnit::CutFadeJudge::CutTracker> sceneTracker;
			bool sceneTrackerPrimed = false;
			uint64_t sceneTrackerNewest = 0, sceneTrackerNewestPrint = 0;
			SceneVerdictSlot sceneVerdicts[kSceneVerdicts];
			std::atomic<uint64_t> sceneConfirmed{ 0 }, sceneProvisional{ 0 }, sceneReprimes{ 0 }, sceneRepublished{ 0 };
			// Delivery (audit U7ACC MUST 3): cuts delivered late (with a later frame's group), of them more than one frame late (frame
			// c + 1 had no group: dropped, unsealed or no record), and the newest frame a fill judged (the late search starts there).
			std::atomic<uint64_t> sceneLate{ 0 }, sceneLateGap{ 0 };
			std::atomic<uint64_t> sceneNoGrid{ 0 };   // no-grid fallback verdicts (the v1 pair law: plane under 32 x 18), counted per tracker push (re-primes push again)
			bool sceneHasFilled = false;
			uint64_t sceneLastFilled = 0;
			// U7's multi-frame fade law (VpUnit::CutFadeJudge::FadeRun, unit version 3): per classified frame, the run that ends at its P4
			// record (written by SceneFactsOf, under its own mutex). An entry is used only when it ends at the very record the next pair starts
			// from, so a seek that restarts the frame numbering never continues an old run.
			struct SceneRunEntry
			{
				uint64_t seq = 0;
				VpUnit::CutFadeJudge::FadeRun run;
			};
			std::mutex sceneRunMutex;
			SceneRunEntry sceneRuns[kSceneRunRing];
			// GF-P2a S2b C1: the runs of frames classified AHEAD of the render seal (the LED walk front), in a ring of their own so they never
			// overwrite a render frame's entry; SceneRunBefore checks it after sceneRuns (FadeRunEndsAt checks the record, so mixing is safe).
			SceneRunEntry sceneRunsAhead[kSceneRunRing];
			std::atomic<uint64_t> sceneRunRebuilt{ 0 }, sceneRunReduced{ 0 };   // ring misses rebuilt exactly / with reduced evidence (SceneRunBefore)
			uint64_t feedEra = 0;
			bool feedHadEra = false;
			uint64_t feedLastEngineSeq = 0;      // the last field seq handled (published, slipped or given up)
			uint64_t feedPendingSeq = 0;         // a field whose slots are still being published (0 = none)
			unsigned feedPendingMask = 0;        // bit 1 P2, 2 P2m, 4 P3: slots done
			unsigned feedAttempts = 0;
			EngineFieldFacts feedPending;
			EngineFieldFacts facts[kFactsRing];

			// The live lane-lead feed (CollectAhead phase; any thread that delivers it). The queue size jitters by a frame or two: the lead
			// the graph is built for is the LOWEST size seen over the last kLeadWindowSeconds of source time (a fall is taken at once, a rise
			// only when the whole window is above it), so the graph is not rebuilt for every wobble.
			// Samples are timestamped (the module's microsecond clock): the window is kLeadWindowSeconds of REAL time whatever the call rate, so a
			// host that calls CollectAhead once per frame, several times per frame or in bursts gives the same answer.
			struct LeadSample { uint64_t us; uint8_t size; };
			std::mutex leadMutex;
			std::deque<LeadSample> leadWindow;
			std::atomic<uint64_t> lastQueueDepthUs{ 0 };   // the last QueueDepth fact (render-ahead plan): older than 8 source periods = the plan stopped
			uint64_t vramShortfall = 0;          // declared users' bytes still held although planned lower (transient in the next plan)
			uint64_t vramShortfallUsed = 0;      // the transient the last Configure planned with
			bool vramReplan = false;             // the shortfall changed: re-plan once

			// The render-thread status cache, read by any thread.
			std::mutex statusMutex;
			std::vector<std::string> fallbackLines;
			std::string oneLine;
			std::string fullText;
		};

		namespace
		{
			// U7 v2 (U7-accuracy): the scene sequence judge over the lane's P4 records, under st.sceneMutex. Render thread: ReadOf (a peek,
			// never a compute). Off the render thread (15c-code: the LED look-ahead walk asks SceneOf there) ReadOf would race the lane's
			// render-thread state, so the record comes from the memo, which off the render thread serves the lane's PUBLISHED bytes (Acquire
			// G2 a') and never runs the lane's render-thread compute.
			constexpr uint64_t kScenePrimeFrames = 8;   // frames pushed before `seq` when the tracker starts over (ref 4 + context 3 + 1)
			using SceneSlot = HostState::SceneVerdictSlot;

			bool ReadP4(Bundle& b, uint64_t seq, VpLaneProducts::SourceStatsRecord* out)
			{
				if (seq == 0)
					return false;
				if (b.memo && !b.memo->OnRenderThread())
				{
					// MUST-9: a PEEK of the lane's published bytes through its lease (the binder's copyPublished pattern,
					// SkeletonAcquireLane.h), never the memo's Acquire: a miss here is a normal look-ahead miss, and an Acquire would set
					// the memo's lastCause_ (a false "skeleton acquire p4_source_stats" Fallbacks entry) and count a failure.
					const VpSkeleton::LaneLease lease = b.lane.Lease(VpSkeleton::kP4SourceStats);
					if (!lease.Valid() || lease.bytes == nullptr || lease.bytes->Bytes() != VpLaneProducts::kStatsBytes)
						return false;
					bool decoded = false;
					const VpSkeleton::PublishedRead pr = VpSkeleton::ReadPublishedBytes(lease, VpSkeleton::kP4SourceStats, seq,
						VpLaneProducts::kStatsBytes, [out, &decoded](const auto* bytes, size_t n) {   // the P4 stats record's bytes (not a pixel buffer)
							decoded = VpLaneProducts::ReadSourceStats(bytes, n, out);
						});
					return pr == VpSkeleton::PublishedRead::Copied && decoded;
				}
				const AnalysisLane::ProductRead r = b.lane.ReadOf(VpSkeleton::kP4SourceStats, seq);
				if (r.cpu != nullptr)
					return VpLaneProducts::ReadSourceStats(r.cpu, r.cpuBytes, out);
				// C5-SKREDS: an ahead frame the lane could not make in its slots was made by P4's owner fill in the memo's own spot (the
				// SceneFactsFor Acquire): a PEEK of that publication (SkeletonMemo::CopyPublished: no compute, no cause, nothing counted).
				if (!b.memo)
					return false;
				const size_t statsBytes = VpLaneProducts::kStatsBytes;   // the P4 stats record (not a pixel buffer)
				std::vector<uint8_t> record(statsBytes);
				return b.memo->CopyPublished(VpSkeleton::kP4SourceStats, seq, record.data(), record.size())
					&& VpLaneProducts::ReadSourceStats(record.data(), record.size(), out);
			}

			// FNV-1a over the record's histogram, cells and counts: a frame number reused for another picture (a seek) is seen.
			uint64_t RecordPrint(const VpLaneProducts::SourceStatsRecord& r)
			{
				uint64_t h = 1469598103934665603ull;
				const auto mix = [&h](uint32_t v) { h ^= v; h *= 1099511628211ull; };
				for (uint32_t v : r.hist)
					mix(v);
				for (uint32_t v : r.cells)
					mix(v);
				mix(r.gridFound);
				mix(r.pixelCount);
				return h;
			}

			// Pushes frame `s` into the tracker; the confirmed verdict it returns for the frame before goes into that frame's slot and onto
			// `done` (the caller rewrites those Vp groups in the store).
			void PushScene(HostState& st, uint64_t s, const VpLaneProducts::SourceStatsRecord& r, uint64_t print, std::vector<uint64_t>* done)
			{
				const VpUnit::CutFadeJudge::CutVerdict v = st.sceneTracker->Push(s, r);
				if (v.known && !v.gridLaw && !v.partStart)
					st.sceneNoGrid.fetch_add(1, std::memory_order_relaxed);   // shown: "Skeleton scene:" and skeleton_scene_no_grid
				if (v.known)
				{
					SceneSlot& d = st.sceneVerdicts[v.seq % HostState::kSceneVerdicts];
					if (d.used && d.seq == v.seq)
					{
						d.v = v;
						if (done)
							done->push_back(v.seq);
					}
				}
				SceneSlot& n = st.sceneVerdicts[s % HostState::kSceneVerdicts];
				const bool samePicture = n.used && n.seq == s && n.print == print;
				n.deliveredOwn = samePicture && n.deliveredOwn;   // the same picture again keeps its delivery
				n.deliveredLate = samePicture && n.deliveredLate;
				n.carriesLate = samePicture && n.carriesLate;
				n.used = true;
				n.seq = s;
				n.print = print;
				n.v = VpUnit::CutFadeJudge::CutVerdict();
				st.sceneTrackerPrimed = true;
				st.sceneTrackerNewest = s;
				st.sceneTrackerNewestPrint = print;
			}

			// An older frame than the tracker holds (and no kept verdict): judged on a tracker of its own, the renderer's stays where it is.
			VpUnit::CutFadeJudge::CutVerdict JudgeSceneApart(Bundle& b, uint64_t seq)
			{
				std::unique_ptr<VpUnit::CutFadeJudge::CutTracker> t(new VpUnit::CutFadeJudge::CutTracker());
				t->Reset();
				VpUnit::CutFadeJudge::CutVerdict out;
				bool lastWasSeq = false;
				VpLaneProducts::SourceStatsRecord r;
				for (uint64_t s = seq > kScenePrimeFrames ? seq - kScenePrimeFrames : 1; s <= seq + 1; ++s)
				{
					if (!ReadP4(b, s, &r))
						continue;
					const VpUnit::CutFadeJudge::CutVerdict v = t->Push(s, r);
					if (v.known && v.seq == seq)
						out = v;
					lastWasSeq = s == seq;
				}
				if (!out.known && lastWasSeq)
					out = t->Provisional();
				return out;
			}

			// The sequence verdict of `seq` (known = false: its record is not readable). Feeds the renderer's tracker up to `seq` (contiguous
			// extension, else a re-prime from seq - 8) and one frame past it when frame seq + 1 is readable, so the verdict is confirmed; else
			// it is provisional (judged without its successor) and counted. `done` gets the frames whose confirmed verdict arrived now.
			// Caller holds st.sceneMutex.
			VpUnit::CutFadeJudge::CutVerdict SceneVerdictLocked(HostState& st, Bundle& b, uint64_t seq, std::vector<uint64_t>* done)
			{
				VpLaneProducts::SourceStatsRecord rec;
				if (!ReadP4(b, seq, &rec))
					return VpUnit::CutFadeJudge::CutVerdict();
				const uint64_t print = RecordPrint(rec);
				SceneSlot& own = st.sceneVerdicts[seq % HostState::kSceneVerdicts];
				const bool ownSame = own.used && own.seq == seq && own.print == print;
				if (ownSame && own.v.known && own.v.confirmed)
					return own.v;   // judged before with the same record
				if (!st.sceneTracker)
					st.sceneTracker.reset(new VpUnit::CutFadeJudge::CutTracker());
				bool reprime = true;
				if (st.sceneTrackerPrimed)
				{
					if (seq < st.sceneTrackerNewest)
					{
						if (!(own.used && own.seq == seq))
							return JudgeSceneApart(b, seq);
						// else: the same frame number holds another record (a seek renumbered): start over at `seq`
					}
					else if (seq - st.sceneTrackerNewest <= kScenePrimeFrames)
					{
						VpLaneProducts::SourceStatsRecord last;
						reprime = !(ReadP4(b, st.sceneTrackerNewest, &last) && RecordPrint(last) == st.sceneTrackerNewestPrint);
					}
				}
				VpLaneProducts::SourceStatsRecord r;
				if (reprime)
				{
					st.sceneTracker->Reset();
					st.sceneTrackerPrimed = false;
					st.sceneReprimes.fetch_add(1, std::memory_order_relaxed);
					for (uint64_t s = seq > kScenePrimeFrames ? seq - kScenePrimeFrames : 1; s < seq; ++s)
						if (ReadP4(b, s, &r))
							PushScene(st, s, r, RecordPrint(r), done);
				}
				else
				{
					for (uint64_t s = st.sceneTrackerNewest + 1; s < seq; ++s)
						if (ReadP4(b, s, &r))
							PushScene(st, s, r, RecordPrint(r), done);   // an unreadable frame between ends the part (the tracker's rule)
				}
				if (!st.sceneTrackerPrimed || st.sceneTrackerNewest != seq)
					PushScene(st, seq, rec, print, done);
				if (ReadP4(b, seq + 1, &r))
					PushScene(st, seq + 1, r, RecordPrint(r), done);
				SceneSlot& mine = st.sceneVerdicts[seq % HostState::kSceneVerdicts];
				if (mine.used && mine.seq == seq && mine.v.known && mine.v.confirmed)
				{
					st.sceneConfirmed.fetch_add(1, std::memory_order_relaxed);
					return mine.v;
				}
				// seq is the tracker's pending frame (seq + 1 was not readable): judged without its successor.
				const VpUnit::CutFadeJudge::CutVerdict v = st.sceneTracker->Provisional();
				mine.used = true;
				mine.seq = seq;
				mine.print = print;
				mine.v = v;
				st.sceneProvisional.fetch_add(1, std::memory_order_relaxed);
				return v;
			}

			// Takes the pending views of `st` and gives the ones that fit `b` to its lane. Any thread (the
			// pending mutex serialises); frames of another size are not analysed (counted as evicted).
			void DrainPending(HostState& st, Bundle& b)
			{
				std::vector<SourceFrameView> take;
				{
					std::lock_guard<std::mutex> lock(st.pendingMutex);
					take.assign(st.pending.begin(), st.pending.end());
					st.pending.clear();
				}
				for (const SourceFrameView& v : take)
				{
					if (v.width == b.dims.width && v.height == b.dims.height)
						b.lane.OnSourceFrame(v);
					else
						st.pendingEvicted.fetch_add(1, std::memory_order_relaxed);
				}
			}

			// The tap sink (frame-analysis worker thread).
			void OnSource(HostState& st, const SourceFrameView& v)
			{
				st.srcW.store(v.width, std::memory_order_relaxed);
				st.srcH.store(v.height, std::memory_order_relaxed);
				st.srcFrames.fetch_add(1, std::memory_order_relaxed);
				std::shared_ptr<Bundle> b = std::atomic_load(&st.bundle);
				if (b && b->dims.width == v.width && b->dims.height == v.height)
				{
					b->lane.OnSourceFrame(v);
					return;
				}
				{
					std::lock_guard<std::mutex> lock(st.pendingMutex);
					st.pending.push_back(v);
					while (st.pending.size() > kPendingViews)
					{
						st.pending.pop_front();
						st.pendingEvicted.fetch_add(1, std::memory_order_relaxed);
					}
				}
				// The render thread may have published the bundle between the two steps: do not strand the view.
				b = std::atomic_load(&st.bundle);
				if (b && b->dims.width == v.width && b->dims.height == v.height)
					DrainPending(st, *b);
			}

			std::string Dimensions(Dims d) { return std::to_string(d.width) + "x" + std::to_string(d.height); }

			// Stops and drops the lane bundle (joins its threads first). Lifecycle / render thread.
			void StopBundle(HostState& st)
			{
				std::shared_ptr<Bundle> old = std::atomic_exchange(&st.bundle, std::shared_ptr<Bundle>());
				if (old)
				{
					old->threads.Stop();   // joined here first: the workers call the lane's ahead sinks, so nothing may reassign them yet
					VpSkeleton::UnbindLaneFromMemo(old->lane, *old->memo);   // L-CORE: on the render thread, after the join
				}
			}

			// Destroys the GPU dispatcher (and everything it allocated). The bundle (whose lane and jobs point at it) goes first.
			void ReleaseDispatcher(HostState& st, bool gpuAlive = true)
			{
				if (!st.dispatcher)
					return;
				StopBundle(st);
				if (gpuAlive)
					st.dispatcher->Release();
				else
					st.dispatcher->Abandon();   // the renderer's device died without GpuReleased: no libplacebo call may touch it
				st.dispatcher.reset();
				st.dispatcherGpu = nullptr;
			}

			// 70b G5: the dispatcher exists only while the lane is wanted AND mode == on AND a pl_gpu was given. Shadow, Off,
			// the Original pathway and "no render device" never construct one (no GPU call at all). A changed pl_gpu or
			// storage choice rebuilds it (kernels carry the storage) and drops the lane bundle that points at the old one.
			void UpdateDispatcher(HostState& st)
			{
				// 15c-c8: a lane raised by consumer demand makes no GPU products (no consumer of them): no dispatcher.
				const bool want = st.settings.lane.mode == LaneSettings::Mode::On && st.gpu != nullptr && st.laneRaise.empty();
				const GrainSplit::Storage storage = RequestedStorage(st.settings);
				if (!want)
				{
					ReleaseDispatcher(st);
					return;
				}
				if (st.dispatcher && st.dispatcherGpu == st.gpu && st.dispatcherStorage == storage)
					return;
				ReleaseDispatcher(st, st.dispatcherGpu == st.gpu);   // another pl_gpu: the old device is gone
				std::unique_ptr<SkeletonGpuDispatcher> d(new SkeletonGpuDispatcher(st.gpu));
				DispatchersCreated().fetch_add(1, std::memory_order_relaxed);
				if (!d->Valid())
					return;   // no usable dispatch on this device: shown as "no render device"
				VpLaneProducts::GrainSplitKernel* grain = new VpLaneProducts::GrainSplitKernel(storage);
				st.dispatcherCompact = grain->Granted() == GrainSplit::Storage::Compact;
				d->AddKernel(std::unique_ptr<VpLaneProducts::IProductKernel>(grain));
				d->AddKernel(std::unique_ptr<VpLaneProducts::IProductKernel>(new VpLaneProducts::RegionStatsKernel()));
				d->AddKernel(std::unique_ptr<VpLaneProducts::IProductKernel>(new VpLaneProducts::LightLevelKernel()));
				st.dispatcher = std::move(d);
				st.dispatcherGpu = st.gpu;
				st.dispatcherStorage = storage;
			}
		}

		// ---- construction -------------------------------------------------------------------------
		namespace
		{
			// U7-accuracy (owner ruling Q5): vector validity is invalidated at CONFIRMED cuts. The P5a facade asks this through its scene
			// seam (MotionValidity::State::ConfirmedCutReader); the answer is the scene group the skeleton published: confirmed && cutHere (the
			// frame's own verdict, judged with frame seq + 1). A provisional verdict refuses nothing.
			// Vector pairs the veto let through because their frame had no confirmed verdict yet (process-wide, as the seam), and of them
			// the pairs into a frame later CONFIRMED as a cut (vectors across a real cut were used before the verdict: one frame of the
			// one-frame-late law). A small ring of the recent unjudged frames links the two; shown on "Skeleton scene:". Approximate:
			// every ask counts (a frame asked twice counts twice), and a link older than kUnjudgedRing frames is lost. GF-P4 S-2: the asks
			// come from ALL vector readers (PairValidAt / PairServes / PairSpansSlip: the estimate, FlowOf, the de-judder policy, the
			// sigma-delta gate and the dither vector source, SharedMotionSource, EstimateState, the camera link, and the P2 CPU form, whose
			// gap read asks each frame of its span), not only the estimate; the status line says so.
			constexpr size_t kUnjudgedRing = 32;
			std::atomic<uint64_t>& ScenePairsUnjudged() { static std::atomic<uint64_t> n{ 0 }; return n; }
			std::atomic<uint64_t>& ScenePairsAcrossLateCut() { static std::atomic<uint64_t> n{ 0 }; return n; }
			std::atomic<uint64_t>* UnjudgedRing()
			{
				static std::atomic<uint64_t> ring[kUnjudgedRing];   // static storage: zero-initialised (0 = no frame)
				return ring;
			}
			// A frame's verdict was just CONFIRMED as a cut (any thread): counts the pairs into it the veto passed before.
			void NoteConfirmedCut(uint64_t seq)
			{
				if (seq == 0)
					return;
				uint64_t expected = seq;
				if (UnjudgedRing()[seq % kUnjudgedRing].compare_exchange_strong(expected, 0, std::memory_order_acq_rel))
					ScenePairsAcrossLateCut().fetch_add(1, std::memory_order_relaxed);
			}
			bool ConfirmedCutAt(uint64_t seq)
			{
				FrameAnalysis::Store* store = VpFrameAnalysis::SourceFrameTap::SingleStore();
				if (store == nullptr)
					return false;
				FrameAnalysis::Scene s;
				const bool judged = store->PeekScene(seq, s) && s.confirmed;
				if (!judged && store->Enabled())
				{
					ScenePairsUnjudged().fetch_add(1, std::memory_order_relaxed);
					UnjudgedRing()[seq % kUnjudgedRing].store(seq, std::memory_order_release);
				}
				return judged && s.cutHere;
			}
			// Why the cut veto cannot work for renderer `host` ("" = it works): its store is off, or the seam's single store is not its
			// store (more than one renderer: the seam carries no renderer key, so it refuses nothing). No store registered at all happens
			// only without the Host (tests): nothing is published, nothing to show. Shown as skeleton_scene_veto and on "Skeleton scene:".
			std::string SceneVetoOff(const void* host)
			{
				FrameAnalysis::Store* own = VpFrameAnalysis::SourceFrameTap::StoreFor(host);
				if (own == nullptr)
					return std::string();
				if (!own->Enabled())
					return "frame-analysis store off (frame_analysis_store=false)";
				if (VpFrameAnalysis::SourceFrameTap::SingleStore() != own)
					return "more than one renderer (the scene seam carries no renderer key)";
				if (MotionValidity::State::ConfirmedCutReader().load(std::memory_order_acquire) != &ConfirmedCutAt)
					return "scene seam not bound to the skeleton";
				return std::string();
			}
			// Live skeleton modules: the seam is cleared when the last one goes (it is ours only while one lives).
			std::atomic<int>& SceneSeamUsers()
			{
				static std::atomic<int> users{ 0 };
				return users;
			}
		}

		SkeletonModule::SkeletonModule(Skeleton::ConfigLoader loader, std::function<uint64_t()> nowUs)
			: loader_(std::move(loader)), nowUs_(nowUs ? std::move(nowUs) : std::function<uint64_t()>(SteadyUs))
		{
			// P16: the coding evidence hub finds a host's memo through the module's per-renderer memo (L-CORE MemoFor). The token lets
			// the destructor clear only its own installation; the hub calls the lambda under a lock the clear waits on (no call after ~).
			codingProviderToken_ = VpUnit::CodingEvidence::Hub::Instance().SetMemoProvider([this](const void* host) -> std::shared_ptr<VpSkeleton::SkeletonMemo> {
				return MemoFor(host, 0).memo;
			});
			// P16 kCodingEvidence owner fill (CPU per-tile form; lead8/iface-P16.md): the one measure of the source picture's coding, module-wide
			// (installed in every renderer's memo). keepFrames covers the monitor's worker latency (derived, SourceResolutionMonitor).
			RegisterOwnerFill(VpSkeleton::kCodingEvidence, VpSkeleton::AcquireForm::Cpu, VpUnit::CodingEvidence::kUnitPath,
				VpUnit::CodingEvidence::MakeOwnerFill(VpUnit::CodingEvidence::Hub::Instance(),
					static_cast<uint8_t>(SourceResolutionMonitor::Monitor::kLatencyFrames)));
			SceneSeamUsers().fetch_add(1, std::memory_order_acq_rel);
			MotionValidity::State::ConfirmedCutReader().store(&ConfirmedCutAt, std::memory_order_release);
		}

		SkeletonModule::~SkeletonModule()
		{
			// First, before any member goes: returns only when no provider call into this module is running; a newer module's provider stays.
			VpUnit::CodingEvidence::Hub::Instance().ClearMemoProvider(codingProviderToken_);
			UnregisterOwnerFill(VpSkeleton::kCodingEvidence, VpSkeleton::AcquireForm::Cpu, VpUnit::CodingEvidence::kUnitPath);
			if (SceneSeamUsers().fetch_sub(1, std::memory_order_acq_rel) == 1)
			{
				MotionValidity::State::ConfirmedCutFn ours = &ConfirmedCutAt;
				MotionValidity::State::ConfirmedCutReader().compare_exchange_strong(ours, nullptr, std::memory_order_acq_rel);
			}
			std::vector<std::shared_ptr<HostState>> all;
			hosts_.ForEach([&all](const void*, const std::shared_ptr<HostState>& st) { all.push_back(st); });
			for (const std::shared_ptr<HostState>& st : all)
				Teardown(st, false, false);   // process end: the render device may already be gone
		}

		// ---- diagnostics --------------------------------------------------------------------------
		bool SkeletonModule::Active(const void* host) const
		{
			const std::shared_ptr<HostState> st = hosts_.Find(host);
			return st && st->active.load(std::memory_order_acquire);
		}
		bool SkeletonModule::TapRegistered(const void* host) const { return VpFrameAnalysis::SourceFrameTap::Wants(host); }
		size_t SkeletonModule::HostStates() const { return hosts_.States(); }
		AnalysisLane* SkeletonModule::LaneOf(const void* host) const
		{
			const std::shared_ptr<HostState> st = hosts_.Find(host);
			const std::shared_ptr<Bundle> b = st ? std::atomic_load(&st->bundle) : nullptr;
			return b ? &b->lane : nullptr;
		}
		VpSkeleton::SkeletonMemo* SkeletonModule::MemoOf(const void* host) const
		{
			return MemoHostOf(host, false).get();
		}

		// QKP slice B: the lane lead's min-window over one host's CollectAhead samples (one copy: the lane host and the memo-only host both use it).
		// Any thread, any rate: every call is a timestamped sample of the queue size; the result is the LOWEST size of the last window.
		namespace
		{
			uint8_t LowestLeadSample(std::mutex& m, std::deque<Vpv::Skeleton::HostState::LeadSample>& window, uint64_t now, uint8_t size)
			{
				const uint64_t windowUs = static_cast<uint64_t>(VpSkeleton::kLeadWindowSeconds * 1e6);
				uint8_t lowest = size;
				std::lock_guard<std::mutex> lock(m);
				window.push_back(Vpv::Skeleton::HostState::LeadSample{ now, size });
				while (window.size() > 1 && window.front().us + windowUs < now)
					window.pop_front();
				for (const Vpv::Skeleton::HostState::LeadSample& smp : window)
					lowest = (std::min)(lowest, smp.size);
				return lowest;
			}
		}

		// ---- L-CORE: the renderer's memo, with or without the lane (lead8/iface-L-CORE.md s2-s3) ----------------------------
		struct MemoHost
		{
			explicit MemoHost(const void* h) : host(h), memo(std::make_shared<VpSkeleton::SkeletonMemo>()) {}
			// R2: the memo may outlive this (a reader's pin): it never keeps a pointer to the device that goes with it.
			~MemoHost()
			{
				if (gpuDevice)
					memo->SetGpuDevice(nullptr);
			}
			const void* host;
			std::shared_ptr<VpSkeleton::SkeletonMemo> memo;
			// Render thread (PlanNoLaneGpu): what the last no-lane grant was planned for.
			bool planned = false;
			uint64_t plannedNeed = 0, plannedVramVersion = 0, plannedBinds = 0;
			uint32_t framesSincePlan = 0;
			VpSkeleton::MemoGpuGrant grant;
			std::string cardWhy;
			// R2 (lead 12): the device side of the sanctioned GPU context of the memo's non-view GPU fills (gpu/OwnerGpuDevice.h): follows
			// the renderer's pl_gpu (AttachMemoGpu / DetachMemoGpu), with or without the lane.
			std::shared_ptr<OwnerGpuDevice> gpuDevice;
			// QKP slice B (design s2.3 / s6): the feeds a memo-only host (no lane) still delivers, and the window derived from them at Begin.
			std::mutex leadMutex;                                          // the lane lead's min-window (CollectAhead; any thread)
			std::deque<Vpv::Skeleton::HostState::LeadSample> leadWindow;
			std::atomic<uint64_t> lastQueueDepthUs{ 0 };                   // the last QueueDepth fact of this host
			std::atomic<uint64_t> lastBeginUs{ 0 };                        // the previous Begin (the period for the stale test)
			std::atomic<uint64_t> periodUs{ 0 };
			std::atomic<uint64_t> lastPresentedSeq{ 0 };                   // AfterPresent: the last presented source seq (0 = none)
			VpSkeleton::WindowPeak presentLag{ static_cast<uint64_t>(VpSkeleton::kLeadWindowSeconds * 1e6) };   // render thread: P
			VpSkeleton::QueueWindowBoard board;                            // written once per Begin, any thread reads
			// OnQueue Drop (any thread): dropped frames past kMaxDropsPerEvent in one event are not discarded from the memo; counted and
			// shown on the Fallbacks status (audit S4, the memo-only twin of HostState::dropsLost), never silent.
			std::atomic<uint64_t> dropsLost{ 0 };
		};

		namespace
		{
			// R2: the memo's GPU device follows the renderer's pl_gpu. Render thread (GpuCreated) or under ownerFillsMutex_ at the memo's
			// creation. Another pl_gpu at the same address = the old device died (its objects are forgotten, never destroyed).
			void DetachMemoGpu(MemoHost& mh, bool gpuAlive, bool laneGpuDispatcher = false);
			void AttachMemoGpu(MemoHost& mh, const void* gpu)
			{
				if (gpu == nullptr || (mh.gpuDevice && mh.gpuDevice->OwnerGpu() == gpu))
					return;
				if (mh.gpuDevice)
					DetachMemoGpu(mh, false);
				mh.gpuDevice = std::make_shared<OwnerGpuDevice>(static_cast<pl_gpu>(gpu));
				mh.memo->SetGpuDevice(mh.gpuDevice.get());
				// GF-P2b MUST-3 (GV AMEND, attach side; the binder side is GF-P1 d0f2cbee3): a bound lane's binder set the presence from
				// lane dispatcher || device at bind time, and PlanNoLaneGpu skips a lane-bound memo, so a device attached after the bind
				// re-asserts it here (a shadow / raised lane has no dispatcher, yet the device serves the non-view GPU owner fills).
				if (mh.memo->LaneBound())
					mh.memo->SetGpuDispatcherPresent(true);
			}
			// The device goes: the memo stops handing the context out, releases every GPU spot (their textures go back to the device),
			// then the device destroys them (alive) or forgets them (died). `laneGpuDispatcher`: the bound lane has its own GPU dispatcher.
			void DetachMemoGpu(MemoHost& mh, bool gpuAlive, bool laneGpuDispatcher)
			{
				if (!mh.gpuDevice)
					return;
				mh.memo->SetGpuDevice(nullptr);
				// GF-P2b MUST-3 (detach side): while a lane is bound the presence is the lane's dispatcher only once the device is gone (the
				// binder's rule, lane dispatcher || device); a memo without a lane is re-planned by PlanNoLaneGpu (pl_gpu known).
				if (mh.memo->LaneBound())
					mh.memo->SetGpuDispatcherPresent(laneGpuDispatcher);
				if (!gpuAlive)
					mh.gpuDevice->Abandon();
				mh.memo->ReleaseGpuSpots();
				if (gpuAlive)
					mh.gpuDevice->Release();
				// Audit S1: what a reader still pins is forgotten (not destroyed through a device that is going): counted and shown.
				const OwnerGpuDevice::Stats left = mh.gpuDevice->GetStats();
				if (left.live != 0)
				{
					mh.memo->NoteGpuForgotten(left.live, left.liveBytes);
					DebugLog::Log("Skeleton: %llu owner GPU texture(s) (%llu bytes) still pinned when the GPU device went: left to its teardown",
						static_cast<unsigned long long>(left.live), static_cast<unsigned long long>(left.liveBytes));
				}
				mh.gpuDevice.reset();
			}
		}

		namespace
		{
			constexpr uint32_t kNoLaneReplanFrames = 240;   // a re-read of the card's free memory without a lane (no per-frame DXGI query)
		}

		void SkeletonModule::PlanNoLaneGpu(const void* host)
		{
			const std::shared_ptr<MemoHost> mh = memoHosts_.Find(host);
			if (!mh || mh->memo->LaneBound())
				return;   // a bound lane plans the memo (its scratch-arena grant, Configure)
			VpSkeleton::SkeletonMemo& memo = *mh->memo;
			const std::vector<uint64_t> levels = memo.GpuNeedLevels();   // QKP B (amendment M6): one level per ahead-ladder step
			const uint64_t need = levels.empty() ? 0u : levels.front();
			const uint64_t vramVersion = VpSkeleton::VramUsers::Instance().Version();
			const uint64_t binds = memo.LaneBinds();
			++mh->framesSincePlan;
			if (mh->planned && mh->plannedNeed == need && mh->plannedVramVersion == vramVersion && mh->plannedBinds == binds
				&& mh->framesSincePlan < kNoLaneReplanFrames)
				return;
			mh->planned = true;
			mh->plannedNeed = need;
			mh->plannedVramVersion = vramVersion;
			mh->plannedBinds = binds;
			mh->framesSincePlan = 0;
			// The render GPU is the "dispatcher" of a no-lane GPU fill (FrameMemo::gpu): present while the renderer's pl_gpu is known.
			memo.SetGpuDispatcherPresent(gpuSlots_.Aux(host) != nullptr);
			CardProfile card;
			std::string why;
			if (cardOverride_)
				card = card_;
			const bool known = need != 0 && (cardOverride_ || QueryCard(hosts_.Aux(host), card, why));
			mh->cardWhy = known || need == 0 ? std::string() : why;
			mh->grant = VpSkeleton::PlanMemoGpuGrantLevels(card, known, VpSkeleton::VramUsers::Instance().Take().heldBytes, levels);
			memo.SetGpuBudget(mh->grant.granted, mh->grant.bytes);
			// The interim ahead plan (S-A / S-B only; S-C needs the delay budget feed): the first ladder step that fits the grant. Planning
			// only: it constrains the NEXT acquires (NotPlannedAhead), never evicts what is attached.
			VpSkeleton::AheadNeed an = memo.MakeAheadNeed(memo.Window());
			an.grantBytes = mh->grant.granted ? mh->grant.bytes : 0u;
			memo.SetAheadPlan(VpSkeleton::PlanAheadLadder(an));
		}

		namespace
		{
			// The product names of a memo made before any lane (the plain registry: the tokens do not depend on the period or queue).
			const VpSkeleton::Registry& NameRegistry()
			{
				static const VpSkeleton::Registry reg = VpSkeleton::SkeletonProducts(false, false, 0.0, 0);
				return reg;
			}
		}

		std::shared_ptr<VpSkeleton::SkeletonMemo> SkeletonModule::MemoHostOf(const void* host, bool create) const
		{
			if (!host)
				return nullptr;
			if (const std::shared_ptr<MemoHost> m = memoHosts_.Find(host))
				return m->memo;
			if (!create)
				return nullptr;
			// Never in the Original pathway (no VP-V state: Original == Bill, zero cost). GF-P2b S1 / C5-SKREDS: the renderer's own word as
			// this module recorded it at SettingsApplied (NotePathway) counts too. The hub records the pathway only AFTER its SettingsApplied
			// fan-out, so between this module's Reconcile (which removed the memo host) and that record, a MemoFor (a later module's
			// SettingsApplied handler, another thread) would otherwise make a new memo for an Original renderer.
			if (Hub::ExtensionHub::Instance().OriginalPathway(host) || RecordedPathway(host) == 1)
				return nullptr;
			// L-CORE M3: made and published UNDER the registration lock, so a RegisterOwnerFill running at the same time either sees this
			// memo in the table (and installs into it) or ran before the copy below (and is in it): never neither. A second thread making the
			// same host's memo finds the first one here.
			std::lock_guard<std::mutex> lock(ownerFillsMutex_);
			if (const std::shared_ptr<MemoHost> m = memoHosts_.Find(host))
				return m->memo;
			std::shared_ptr<MemoHost> made = std::make_shared<MemoHost>(host);
			VpSkeleton::SkeletonMemo& memo = *made->memo;
			memo.SetHost(host);
			memo.SetNames(&NameRegistry());
			// GPU retire: memo spots are owner-made pl_tex objects on the D3D11 backend, whose destruction with reads still queued is
			// safe (the runtime keeps a resource alive until the GPU is done with it), so a noted read never holds a spot (G0 S4).
			memo.SetFenceCompleted([]() { return ~uint64_t(0); });
			for (const auto& kv : ownerFills_)
				memo.RegisterOwnerFill(kv.first.first, kv.first.second, kv.second.first, kv.second.second);
			AttachMemoGpu(*made, gpuSlots_.Aux(host));   // R2: the sanctioned GPU context's device, when the pl_gpu is known
			Hub::HostSlots<MemoHost>& slots = const_cast<Hub::HostSlots<MemoHost>&>(memoHosts_);
			if (!slots.Put(host, made))
			{
				if (const std::shared_ptr<MemoHost> other = memoHosts_.Find(host))
					return other->memo;   // another thread made it first
				return nullptr;          // the table is full (sixteen renderers): no memo, the caller's "unavailable" shows it
			}
			return made->memo;
		}

		FrameMemo SkeletonModule::MemoFor(const void* host, uint64_t seq)
		{
			FrameMemo fm;
			fm.memo = MemoHostOf(host, true);
			if (!fm.memo)
				return FrameMemo();
			fm.host = host;
			fm.seq = seq;
			const Hub::CurrentFrame cur = Hub::CurrentFrameOf(host);
			if (cur.Valid() && cur.seq == static_cast<int64_t>(seq) && cur.nextSeq >= 0)
			{
				fm.nextSeq = static_cast<uint64_t>(cur.nextSeq);
				fm.haveNext = true;
			}
			fm.gpu = gpuSlots_.Aux(host);
			return fm;
		}

		VpSkeleton::SkeletonMemo::Registration SkeletonModule::RegisterOwnerFill(VpSkeleton::ProductId id, VpSkeleton::AcquireForm form,
			const std::string& unit, VpSkeleton::OwnerFill fill)
		{
			using Reg = VpSkeleton::SkeletonMemo::Registration;
			const std::string owner = VpSkeleton::SkeletonMemo::OwnerUnitOf(id);
			if (owner.empty())
				return Reg::NotInCatalog;
			if (unit != owner)
				return Reg::NotOwner;
			if (fill.defaults.stage != 0)
				return Reg::BadDefaults;
			std::vector<std::shared_ptr<VpSkeleton::SkeletonMemo>> memos;
			{
				std::lock_guard<std::mutex> lock(ownerFillsMutex_);
				const auto key = std::make_pair(id, form);
				if (ownerFills_.count(key) != 0)
					return Reg::AlreadyRegistered;
				ownerFills_[key] = std::make_pair(unit, fill);
				memoHosts_.ForEach([&memos](const void*, const std::shared_ptr<MemoHost>& m) { memos.push_back(m->memo); });
			}
			for (const std::shared_ptr<VpSkeleton::SkeletonMemo>& m : memos)
				m->RegisterOwnerFill(id, form, unit, fill);
			return Reg::Registered;
		}

		bool SkeletonModule::UnregisterOwnerFill(VpSkeleton::ProductId id, VpSkeleton::AcquireForm form, const std::string& unit)
		{
			std::vector<std::shared_ptr<VpSkeleton::SkeletonMemo>> memos;
			{
				std::lock_guard<std::mutex> lock(ownerFillsMutex_);
				const auto it = ownerFills_.find(std::make_pair(id, form));
				if (it == ownerFills_.end() || it->second.first != unit)
					return false;
				ownerFills_.erase(it);
				memoHosts_.ForEach([&memos](const void*, const std::shared_ptr<MemoHost>& m) { memos.push_back(m->memo); });
			}
			for (const std::shared_ptr<VpSkeleton::SkeletonMemo>& m : memos)
				m->UnregisterOwnerFill(id, form, unit);
			return true;
		}

		size_t SkeletonModule::OwnerFillsRegistered() const
		{
			std::lock_guard<std::mutex> lock(ownerFillsMutex_);
			return ownerFills_.size();
		}
		VpSkeleton::SpareHost* SkeletonModule::SpareOf(const void* host) const
		{
			const std::shared_ptr<HostState> st = hosts_.Find(host);
			return st ? &st->spare : nullptr;
		}
		VpSkeleton::LaneLease SkeletonModule::LeaseFor(const void* host, VpSkeleton::ProductId product)
		{
			// The bundle is held for the call only; the lease holds the lane state itself (SK-W lease rules).
			const std::shared_ptr<HostState> st = hosts_.Find(host);
			const std::shared_ptr<Bundle> b = st ? std::atomic_load(&st->bundle) : nullptr;
			if (!b)
				return VpSkeleton::LaneLease();
			VpSkeleton::LaneLease l = b->lane.Lease(product);
			if (!l.table || !l.table->Find(product))
				return VpSkeleton::LaneLease();   // the graph does not hold the product (no external demand for it)
			return l;
		}
		Dims SkeletonModule::DimsOf(const void* host) const
		{
			const std::shared_ptr<HostState> st = hosts_.Find(host);
			const std::shared_ptr<Bundle> b = st ? std::atomic_load(&st->bundle) : nullptr;
			return b ? b->dims : Dims{ 0, 0 };
		}
		bool SkeletonModule::WorkersRunning(const void* host) const
		{
			const std::shared_ptr<HostState> st = hosts_.Find(host);
			const std::shared_ptr<Bundle> b = st ? std::atomic_load(&st->bundle) : nullptr;
			return b && b->threads.Running();
		}
		SkeletonModule::TimingFeed SkeletonModule::TimingFeedOf(const void* host) const
		{
			const std::shared_ptr<HostState> st = hosts_.Find(host);
			return st ? st->timing : TimingFeed();
		}
		bool SkeletonModule::NoteRenderOnlyMs(const void* host, uint64_t seq, double renderOnlyMs)
		{
			const std::shared_ptr<HostState> st = hosts_.Find(host);
			const std::shared_ptr<Bundle> b = st ? std::atomic_load(&st->bundle) : nullptr;
			if (!b || !(st->periodMs > 0.0) || !(renderOnlyMs >= 0.0))
				return false;
			++st->timing.noted;   // B4 S2: recorded in both gates
			st->timing.notedMsSum += renderOnlyMs;
			st->timing.lastNotedMs = renderOnlyMs;
			if (GetRegulatorGate() != RegulatorGate::Act)
				return true;   // observe only: the regulator is not fed, nothing is shed
			b->lane.OnFrameTiming(seq, st->periodMs - renderOnlyMs, renderOnlyMs, renderOnlyMs > st->periodMs);
			++st->timing.fed;
			st->timing.measured = true;
			return true;
		}
		uint64_t SkeletonModule::VramPendingFreeOf(const void* host) const
		{
			const std::shared_ptr<HostState> st = hosts_.Find(host);
			return st ? st->vramShortfall : 0u;
		}
		SkeletonModule::QueueInUse SkeletonModule::QueueInUseOf(const void* host) const
		{
			QueueInUse out;
			if (const std::shared_ptr<HostState> st = hosts_.Find(host))
			{
				out.fed = st->queueFed;
				out.used = st->queueUsed;
				out.leadPeak = st->leadPeak;
				out.starveEvents = st->starveEvents;
			}
			return out;
		}
		unsigned long long SkeletonModule::ContainedFaults() { return Faults().load(std::memory_order_relaxed); }
		const SkeletonGpuDispatcher* SkeletonModule::GpuDispatcherFor(const void* host) const
		{
			const std::shared_ptr<HostState> st = hosts_.Find(host);
			return st ? st->dispatcher.get() : nullptr;
		}
		const void* SkeletonModule::MemoDeviceGpuForTesting(const void* host) const
		{
			const std::shared_ptr<MemoHost> mh = memoHosts_.Find(host);
			if (!mh)
				return nullptr;
			std::lock_guard<std::mutex> lock(ownerFillsMutex_);   // as the attach / detach sites
			return mh->gpuDevice ? mh->gpuDevice->OwnerGpu() : nullptr;
		}
		unsigned long long SkeletonModule::GpuDispatchersCreated() { return DispatchersCreated().load(std::memory_order_relaxed); }

		// ---- lifecycle (render thread) --------------------------------------------------------------
		void SkeletonModule::OnLifecycle(RE::LifecycleFacts& f)
		{
			if (!f.host)
				return;
			switch (f.kind)
			{
			case RE::Lifecycle::GpuCreated:
				// Audit S3: a memo device still attached at GpuCreated belongs to a renderer that never sent GpuReleased (a dead device,
				// whatever the new pl_gpu's address): abandoned before the new one is known, so a same-address pl_gpu is never mistaken
				// for it (its pl_dispatch is forgotten, not reused). Covers a memo-only host too (no lane state).
				if (const std::shared_ptr<MemoHost> mh = memoHosts_.Find(f.host))
				{
					bool laneGpuDispatcher = false;   // GF-P2b MUST-3: the presence a bound lane keeps once the device is gone
					if (const std::shared_ptr<HostState> hs = hosts_.Find(f.host))
						if (const std::shared_ptr<Bundle> hb = std::atomic_load(&hs->bundle))
							laneGpuDispatcher = hb->lane.HasGpuDispatcher();
					std::lock_guard<std::mutex> lock(ownerFillsMutex_);
					DetachMemoGpu(*mh, false, laneGpuDispatcher);
				}
				hosts_.SetAux(f.host, f.device);   // the raw device, for the video memory query only
				gpuSlots_.SetAux(f.host, const_cast<void*>(static_cast<const void*>(f.gpu)));   // 70b G5: the renderer's pl_gpu (null in a unit test: no GPU products)
				Reconcile(f.host, -1);
				if (const std::shared_ptr<MemoHost> mh = memoHosts_.Find(f.host))
				{
					std::lock_guard<std::mutex> lock(ownerFillsMutex_);
					AttachMemoGpu(*mh, f.gpu);   // R2: a memo made before the device was known gets its GPU context device now
				}
				break;
			case RE::Lifecycle::SettingsApplied:
				PublishLookAheadWalk(f.value);   // the host passes the motion look-ahead depth (frames) here: the one queue term it delivers
				NotePathway(f.host, f.code != 0 ? 1 : 0);   // 15c-c8: kept for the demand-driven reconciles (never a raise in Original)
				Reconcile(f.host, f.code != 0 ? 1 : 0);   // code != 0: the Original pathway
				break;
			case RE::Lifecycle::QueueDepth:
			{
				// The render-ahead term (the host's one generic call at the render-ahead plan block, every planned frame): frames the plan keeps ahead
				// of the present. Only while this renderer's lane is on; PublishRenderAhead is change-only (a compare-and-swap that leaves the other terms).
				// QKP B: also for a renderer whose memo exists without the lane (the window needs R whatever the lane does).
				const std::shared_ptr<HostState> st = hosts_.Find(f.host);
				const std::shared_ptr<MemoHost> mhq = memoHosts_.Find(f.host);
				if ((st && st->active.load(std::memory_order_acquire)) || mhq)
				{
					if (f.value >= 0.0)   // NaN and negative: the host has no value (no source period): unwired, never an old number
					{
						PublishRenderAhead(f.value);
						const uint64_t nowQ = nowUs_();
						if (st)
							st->lastQueueDepthUs.store(nowQ, std::memory_order_relaxed);
						if (mhq)
							mhq->lastQueueDepthUs.store(nowQ, std::memory_order_relaxed);
					}
					else
						UnpublishRenderAhead();
				}
				break;
			}
			case RE::Lifecycle::GpuReleased:
			case RE::Lifecycle::Shutdown:
			{
				const std::shared_ptr<HostState> st = hosts_.Find(f.host);
				if (st)
					Teardown(st, true);
				else
					hosts_.Remove(f.host, true);
				gpuSlots_.Remove(f.host, true);   // the pl_gpu is going away: the dispatcher was released by Teardown above
				if (const std::shared_ptr<MemoHost> mh = memoHosts_.Find(f.host))
				{
					std::lock_guard<std::mutex> lock(ownerFillsMutex_);   // as at GpuCreated (C2 gate SHOULD 2): one lock context for the memo's device
					DetachMemoGpu(*mh, true);     // R2: the memo's GPU spots and the owner textures go while the device lives
				}
				memoHosts_.Remove(f.host, true);  // L-CORE: the renderer's memo (a reader's pin keeps its spots until released)
				EraseExternal(f.host);   // a renderer recreated at this address never inherits the old one's external demand
				// 15c-c8: with the demand goes its raise of the store; version 0 = the next Begin looks again (a later declaration raises anew).
				VpFrameAnalysis::SourceFrameTap::SetStoreDemand(f.host, false, std::string());
				NoteRaiseApplied(f.host, 0, std::string());
				break;
			}
			default:
				break;
			}
		}

		// Reads the settings and brings this renderer's state in line: build, update or tear down.
		void SkeletonModule::Reconcile(const void* host, int originalFlag)
		{
			Settings s = Skeleton::Load(loader_);
			if (originalFlag >= 0)
				s.originalPathway = originalFlag != 0;   // the renderer's own word beats the file
			std::shared_ptr<HostState> st = hosts_.Find(host);
			if (st && st->ownerGen != VpFrameAnalysis::SourceFrameTap::Generation(host))
			{
				// A new renderer at the address of one that died without GpuReleased (a failed device create):
				// the old state's seal hooks, tap and lane belong to the dead one.
				Teardown(st, false, false);   // the dead renderer's device objects are not ours to destroy
				if (const std::shared_ptr<MemoHost> mh = memoHosts_.Find(host))
				{
					std::lock_guard<std::mutex> memoLock(ownerFillsMutex_);   // GEM SK-LIFECYCLE: as the other two sites (the MemoHost is kept)
					// GF-P2b E2: only the DEAD renderer's device goes. A device on the host's current pl_gpu was attached by this renderer's
					// GpuCreated (which abandons any older device first, audit S3): the store registering at the first SettingsApplied after it
					// moves the tap generation and lands here, and detaching that device would leave the new renderer without one (never
					// re-attached: every GPU owner fill would decline). A later GpuCreated still abandons a device it finds.
					const void* currentGpu = gpuSlots_.Aux(host);
					if (!(mh->gpuDevice && currentGpu != nullptr && mh->gpuDevice->OwnerGpu() == currentGpu))
						DetachMemoGpu(*mh, false);   // R2: nor are its owner textures (forgotten, counted)
				}
				hosts_.Remove(host, false);
				EraseExternal(host);          // nor is its external demand (declared for the dead renderer)
				st.reset();
			}
			// GF-P2b S1 (ADR-0019: Original == Bill, zero cost): a live switch to the Original pathway ends the renderer's memo too. MemoHostOf
			// returns an existing memo before its Original check, so a kept memo would still be sealed, windowed, planned (card query) and
			// shown on the status lines. Its device goes while it lives (as at GpuReleased), then the table entry; MemoHostOf(false) is then
			// null and every memo-host site is inert, and MemoFor makes none while the hub records Original. The renderer's word decides
			// (as MemoHostOf: the hub's record when this Reconcile carries none), never the file alone.
			const bool originalNow = originalFlag >= 0 ? originalFlag != 0 : Hub::ExtensionHub::Instance().OriginalPathway(host);
			if (originalNow)
				if (const std::shared_ptr<MemoHost> mh = memoHosts_.Find(host))
				{
					{
						std::lock_guard<std::mutex> memoLock(ownerFillsMutex_);   // the one lock context for the memo's device (GpuReleased / GpuCreated)
						DetachMemoGpu(*mh, true);
					}
					memoHosts_.Remove(host, true);
				}
			// 15c-c8 (coordinator ruling 2026-11-15; the TD fill rule's shape, core/AdmissionTable.h Admit: a product TD OFF still filled FOR the
			// active consumer that depends on it): a registered consumer that needs the lane's results RAISES the effective lane mode to On (the
			// least that serves it: Shadow's results are not used) and the frame-analysis store that feeds the lane, while it is registered.
			// The configured value stays as read (shown beside the raise); never in the Original pathway (the renderer's word, also the one it
			// last reported, so a GpuCreated reconcile with no word cannot raise there). The version is read first: a declaration made while
			// this runs moves it again and the next Begin reconciles once more.
			const uint64_t raiseVer = raiseVersion_.load(std::memory_order_acquire);
			const bool raiseBlocked = s.originalPathway || RecordedPathway(host) == 1;
			const std::string raise = raiseBlocked ? std::string() : RaiseReasonOf(host);
			VpFrameAnalysis::SourceFrameTap::SetStoreDemand(host, !raise.empty(), raise);
			NoteRaiseApplied(host, raiseVer, raise);
			std::string laneRaise;
			const LaneSettings::Mode configuredMode = s.lane.mode;
			if (!raise.empty() && s.lane.mode != LaneSettings::Mode::On)
			{
				s.lane.mode = LaneSettings::Mode::On;
				laneRaise = raise;
			}
			if (!s.LaneWanted())
			{
				if (st)
					Teardown(st, false);
				// An unreadable key is shown, never ignored: the only thing an otherwise-off module ever says.
				if (!s.originalPathway && !s.laneProblems.empty())
				{
					if (!st)
					{
						st = std::make_shared<HostState>(host);
						if (!hosts_.Put(host, st))
							return;
					}
					std::string problems;
					for (const std::string& p : s.laneProblems)
						problems += (problems.empty() ? "" : "; ") + p;
					{
						std::lock_guard<std::mutex> lock(st->statusMutex);
						st->fallbackLines.clear();
						const FallbackCatalog::Decision d = FallbackCatalog::SkeletonKeys(problems);
						st->fallbackLines.push_back(d.id + ": " + d.reason);
					}
					st->reporting.store(true, std::memory_order_release);
				}
				return;
			}
			if (!st)
			{
				st = std::make_shared<HostState>(host);
				if (!hosts_.Put(host, st))
				{
					// The table holds sixteen renderers; without a slot there is no state to show this on.
					DebugLog::Log("Skeleton: host table full; the analysis lane stays off for this renderer");
					return;
				}
			}
			st->settings = s;
			st->laneRaise = laneRaise;               // 15c-c8: before UpdateDispatcher (a raised lane has no GPU dispatcher)
			st->configuredLaneMode = configuredMode;
			st->gpu = static_cast<pl_gpu>(gpuSlots_.Aux(host));   // const pl_gpu_t*
			UpdateDispatcher(*st);
			st->settingsGen.fetch_add(1, std::memory_order_relaxed);
			const bool wasActive = st->active.load(std::memory_order_acquire);
			// The shared seal: hooked once per renderer (a FrameSeal cannot drop a provider or subscriber).
			if (!st->sealHooked)
			{
				if (VpSeal::FrameSeal* seal = Resync::ResyncExtension::SealFor(host))
				{
					const std::weak_ptr<HostState> weak = st;
					st->sealLateHook = seal->Seals() > 0;   // added after the first frame: bound at the next era
					seal->AddProvider(kName, [weak, this, host](const std::shared_ptr<VpIndicator::Indicator>& ind) {
						if (const std::shared_ptr<HostState> s2 = weak.lock())
						{
							if (const std::shared_ptr<Bundle> b = std::atomic_load(&s2->bundle))
								b->lane.BindIndicator(ind);
							// G0 S4: a new resync era is a discontinuity: the renderer's memo spots are released (Clear).
							if (const std::shared_ptr<VpSkeleton::SkeletonMemo> m = MemoHostOf(host, false))
								m->Clear();
						}
					});
					seal->Subscribe(kName, [weak](const std::shared_ptr<VpIndicator::Indicator>&, uint64_t seq, VpIndicator::CounterMask changed) {
						if (const std::shared_ptr<HostState> s2 = weak.lock())
							if (const std::shared_ptr<Bundle> b = std::atomic_load(&s2->bundle))
								b->lane.OnSealed(seq, changed);
					});
					st->sealHooked = true;
				}
			}
			st->active.store(true, std::memory_order_release);
			if (!wasActive || !VpFrameAnalysis::SourceFrameTap::Wants(host))   // the tap is also re-registered when it was lost
			{
				st->frames = 0;
				st->lastBeginUs = 0;
				st->periodMs = 0.0;
				const std::weak_ptr<HostState> weak = st;
				VpFrameAnalysis::SourceFrameTap::Register(host, [weak](const SourceFrameView& v) {
					if (const std::shared_ptr<HostState> s2 = weak.lock())
						OnSource(*s2, v);
				});
				st->tapOn = true;
			}
			if (!st->sealHooked)
				st->waiting = "waiting: no resync seal for this renderer (resync_event=on required)";
			st->shownBundle = !st->shownBundle;   // force a refresh
			Refresh(*st);
		}

		void SkeletonModule::Teardown(const std::shared_ptr<HostState>& st, bool releaseSlot, bool gpuAlive)
		{
			if (!st)
				return;
			const void* host = st->host;
			st->active.store(false, std::memory_order_release);
			st->reporting.store(false, std::memory_order_release);
			if (st->tapOn)
			{
				VpFrameAnalysis::SourceFrameTap::Unregister(host);
				st->tapOn = false;
			}
			if (st->engine != nullptr && gpuAlive && st->fitWantedPushed)
				st->engine->SetCameraFitWanted(false);   // the engine is still valid at GpuReleased / Shutdown; not for a dead renderer's address
			st->engine = nullptr;
			st->fitWantedPushed = false;
			st->feedNeeded = false;
			st->wantsCameraFit = false;
			st->laneRaise.clear();   // 15c-c8 (the store demand is Reconcile's / GpuReleased's: a torn-down lane may still be raised again)
			st->feedPendingSeq = 0;
			StopBundle(*st);
			ReleaseDispatcher(*st, gpuAlive);   // 70b G5: the GPU textures go with the lane (GpuReleased / switched off)
			{
				std::lock_guard<std::mutex> lock(st->pendingMutex);
				st->pending.clear();
			}
			{
				std::lock_guard<std::mutex> lock(st->dropMutex);
				st->drops.clear();
			}
			{
				std::lock_guard<std::mutex> lock(st->statusMutex);
				st->fallbackLines.clear();
				st->oneLine.clear();
				st->fullText.clear();
			}
			st->waiting.clear();
			st->srcW.store(0, std::memory_order_relaxed);
			st->srcH.store(0, std::memory_order_relaxed);
			if (releaseSlot)
				hosts_.Remove(host, true);   // the seal hooks hold weak references: they become no-ops
		}

		// ---- external demand and the engine feed (SK-W stage D) ---------------------------------------
		void SkeletonModule::SetExternalDemand(const void* host, const void* owner, const std::vector<VpSkeleton::ConsumerDecl>& consumers,
			const std::vector<VpSkeleton::ProductId>& publishedOutside)
		{
			SetRaisingExternalDemand(host, owner, consumers, publishedOutside, std::string());   // a plain declaration raises nothing
		}

		// 15c-c8: the one entry writer. A raising entry (non-empty reason with a consumer) also moves raiseVersion_, so the host's next Begin
		// reconciles its effective lane / store (render thread); a plain entry replacing a raising one withdraws the raise the same way.
		void SkeletonModule::SetRaisingExternalDemand(const void* host, const void* owner, const std::vector<VpSkeleton::ConsumerDecl>& consumers,
			const std::vector<VpSkeleton::ProductId>& publishedOutside, const std::string& reason)
		{
			std::lock_guard<std::mutex> lock(externalMutex_);
			ExternalEntry* entry = nullptr;
			for (ExternalEntry& e : external_)
				if (e.host == host && e.owner == owner)
					entry = &e;
			if (entry == nullptr)
			{
				ExternalEntry e;
				e.host = host;
				e.owner = owner;
				external_.push_back(std::move(e));
				entry = &external_.back();
			}
			const bool wasRaising = RaisingLocked(*entry);
			const std::string wasReason = entry->raise;
			entry->consumers = consumers;
			entry->products = publishedOutside;
			entry->raise = reason;
			externalVersion_.fetch_add(1, std::memory_order_release);
			if (wasRaising != RaisingLocked(*entry) || (wasRaising && wasReason != reason))
				raiseVersion_.fetch_add(1, std::memory_order_release);
		}

		std::string SkeletonModule::RaiseReasonLocked(const void* host) const
		{
			std::string why;
			for (const ExternalEntry& e : external_)
				if (e.host == host && RaisingLocked(e) && why.find(e.raise) == std::string::npos)
					why += (why.empty() ? "" : ", ") + e.raise;
			return why;
		}

		std::string SkeletonModule::RaiseReasonOf(const void* host) const
		{
			std::lock_guard<std::mutex> lock(externalMutex_);
			return RaiseReasonLocked(host);
		}

		void SkeletonModule::NoteRaiseApplied(const void* host, uint64_t version, const std::string& applied)
		{
			std::lock_guard<std::mutex> lock(externalMutex_);
			for (RaiseSeen& r : raiseSeen_)
				if (r.host == host)
				{
					r.version = version;
					r.applied = applied;
					return;
				}
			RaiseSeen r;
			r.host = host;
			r.version = version;
			r.applied = applied;
			raiseSeen_.push_back(r);
		}

		void SkeletonModule::NotePathway(const void* host, int originalFlag)
		{
			std::lock_guard<std::mutex> lock(externalMutex_);
			for (RaiseSeen& r : raiseSeen_)
				if (r.host == host)
				{
					r.originalFlag = originalFlag;
					return;
				}
			RaiseSeen r;
			r.host = host;
			r.originalFlag = originalFlag;
			raiseSeen_.push_back(r);   // version 0: the next Begin reconciles (Reconcile follows this call anyway)
		}

		int SkeletonModule::RecordedPathway(const void* host) const
		{
			std::lock_guard<std::mutex> lock(externalMutex_);
			for (const RaiseSeen& r : raiseSeen_)
				if (r.host == host)
					return r.originalFlag;
			return -1;
		}

		// Render thread (Begin): a raising declaration made or withdrawn since this host's last Reconcile is applied now. While nothing moved:
		// one atomic load and one short locked scan. A move that leaves this host's raise as it was (another host's demand) only records the
		// version; a real change reconciles with the pathway the renderer last reported (never a raise in the Original pathway).
		void SkeletonModule::ReconcileIfRaiseMoved(const void* host)
		{
			const uint64_t version = raiseVersion_.load(std::memory_order_acquire);
			if (version == 1)
				return;   // no raising declaration was ever made: one atomic load, no lock (the default configuration's cost)
			int flag = -1;
			{
				std::lock_guard<std::mutex> lock(externalMutex_);
				RaiseSeen* seen = nullptr;
				for (RaiseSeen& r : raiseSeen_)
					if (r.host == host)
						seen = &r;
				if (seen != nullptr && seen->version == version)
					return;
				const std::string now = RaiseReasonLocked(host);
				if (seen == nullptr)
				{
					RaiseSeen r;
					r.host = host;
					raiseSeen_.push_back(r);
					seen = &raiseSeen_.back();
				}
				if (now == seen->applied)
				{
					seen->version = version;   // nothing to change for this host: the configured state (or the raise in force) stays
					return;
				}
				flag = seen->originalFlag;
			}
			Reconcile(host, flag);
		}

		size_t SkeletonModule::EraseExternal(const void* host)
		{
			std::lock_guard<std::mutex> lock(externalMutex_);
			size_t erased = 0;
			bool raising = false;
			for (size_t i = 0; i < external_.size();)
			{
				if (external_[i].host == host)
				{
					raising = raising || RaisingLocked(external_[i]);
					external_.erase(external_.begin() + static_cast<std::ptrdiff_t>(i));
					++erased;
				}
				else
					++i;
			}
			if (erased != 0)
			{
				externalVersion_.fetch_add(1, std::memory_order_release);
				// GF-P2b S3: the declaring components learn that their entry went (ExternalErasures), the tap generation untouched (moving it
				// would run the dead-renderer teardown).
				bool counted = false;
				for (std::pair<const void*, uint64_t>& e : externalErasures_)
					if (e.first == host)
					{
						++e.second;
						counted = true;
					}
				if (!counted)
					externalErasures_.push_back(std::make_pair(host, uint64_t(1)));
			}
			if (raising)
				raiseVersion_.fetch_add(1, std::memory_order_release);
			return erased;
		}

		uint64_t SkeletonModule::ExternalErasures(const void* host) const
		{
			std::lock_guard<std::mutex> lock(externalMutex_);
			for (const std::pair<const void*, uint64_t>& e : externalErasures_)
				if (e.first == host)
					return e.second;
			return 0;
		}

		size_t SkeletonModule::ExternalEntriesOf(const void* host) const
		{
			std::lock_guard<std::mutex> lock(externalMutex_);
			size_t n = 0;
			for (const ExternalEntry& e : external_)
				n += e.host == host ? 1u : 0u;
			return n;
		}

		void SkeletonModule::ClearExternalDemand(const void* host, const void* owner)
		{
			std::lock_guard<std::mutex> lock(externalMutex_);
			for (size_t i = 0; i < external_.size(); ++i)
				if (external_[i].host == host && external_[i].owner == owner)
				{
					const bool raising = RaisingLocked(external_[i]);
					external_.erase(external_.begin() + static_cast<std::ptrdiff_t>(i));
					externalVersion_.fetch_add(1, std::memory_order_release);
					if (raising)
						raiseVersion_.fetch_add(1, std::memory_order_release);   // withdrawn: the host's next Begin lowers the lane / store again
					return;
				}
		}

		// Adds this host's external consumers (ids not already in `consumers`: the first wins, the rest are counted) and products.
		bool SkeletonModule::CollectExternal(const void* host, std::vector<VpSkeleton::ConsumerDecl>& consumers,
			std::vector<VpSkeleton::ProductId>& products, bool& wantsCameraFit, size_t& duplicates) const
		{
			std::lock_guard<std::mutex> lock(externalMutex_);
			bool any = false;
			for (const ExternalEntry& e : external_)
			{
				if (e.host != host)
					continue;
				any = true;
				for (const VpSkeleton::ConsumerDecl& c : e.consumers)
				{
					bool taken = false;
					for (const VpSkeleton::ConsumerDecl& have : consumers)
						taken = taken || have.id == c.id;
					if (taken)
					{
						++duplicates;
						continue;
					}
					consumers.push_back(c);
					wantsCameraFit = wantsCameraFit || ReadsCameraProduct(c);
				}
				for (VpSkeleton::ProductId p : e.products)
				{
					bool have = false;
					for (VpSkeleton::ProductId q : products)
						have = have || q == p;
					if (!have)
						products.push_back(p);
				}
			}
			return any;
		}

		FeedCounters SkeletonModule::FeedCountersOf(const void* host) const
		{
			const std::shared_ptr<HostState> st = hosts_.Find(host);
			return st ? st->feed : FeedCounters();
		}

		bool SkeletonModule::EngineFieldFactsOf(const void* host, uint64_t seq, EngineFieldFacts& out) const
		{
			out = EngineFieldFacts();
			const std::shared_ptr<HostState> st = hosts_.Find(host);
			if (!st || seq == 0)
				return false;
			const EngineFieldFacts& f = st->facts[static_cast<size_t>(seq % kFactsRing)];
			if (f.seq != seq)
				return false;
			out = f;
			return true;
		}

		void SkeletonModule::RetryEngineFeedForTesting(const void* host)
		{
			if (const std::shared_ptr<HostState> st = hosts_.Find(host))
				FeedEngine(*st, true);
		}

		// The scene of `seq`: the sequence law's verdict (U7 v2, the renderer's CutTracker over the P4 records) and the fade over `seq - 1` ->
		// `seq` (U7 v3: the pair law's OR the multi-frame law's, SceneRunBefore / FoldFadeRun). This is the classification only; the SCENE
		// PRODUCT (SceneOf / FillScene below) stores its delivered cut / fade in the frame-analysis store's scene group once per frame and
		// serves readers from there.
		namespace
		{
			// The U7 fade run that ends at P4(seq - 1) = `prev` (get-or-compute, rule 2): the ring's entry when it ends at that very record
			// (the seal classifies every frame in order, so this is the normal case); else rebuilt forward from the older P4 records the lane
			// still holds (at most kSceneRunRing - 1 pairs back, inside the P4 read lag kScenePrimeFrames, starting from a ring entry that
			// ends at the oldest of them, or from an empty run); nothing older readable: an empty run, i.e. the pair law alone for this pair.
			// Caller holds st.sceneRunMutex.
			// The rebuild is NOT always equivalent to the ring path: unless it joins a ring entry or reaches the first frame (P4(1)), its run
			// starts at most kSceneRunRing - 1 pairs back, so less gain has built up and a slow fade can be found later. That reduced-evidence
			// path is counted (sceneRunReduced, SceneCounters::fadeRunReduced, the "Skeleton scene" status line, rule 5); an exact rebuild
			// is counted as sceneRunRebuilt.
			VpUnit::CutFadeJudge::FadeRun SceneRunBefore(HostState& st, Bundle& b, uint64_t seq, const VpLaneProducts::SourceStatsRecord& prev)
			{
				using VpUnit::CutFadeJudge::FadeRun;
				const auto entryEndingAt = [&st](uint64_t s, const VpLaneProducts::SourceStatsRecord& r, FadeRun& out) {
					// The render ring first, then the ahead ring (S2b C1): whichever holds the run that ends at this very record.
					const HostState::SceneRunEntry* const rings[2] = { st.sceneRuns, st.sceneRunsAhead };
					for (const HostState::SceneRunEntry* ring : rings)
					{
						const HostState::SceneRunEntry& en = ring[static_cast<size_t>(s % kSceneRunRing)];
						if (en.seq == s && VpUnit::CutFadeJudge::FadeRunEndsAt(en.run, r))
						{
							out = en.run;
							return true;
						}
					}
					return false;
				};
				FadeRun run;
				if (entryEndingAt(seq - 1, prev, run))
					return run;
				// recs[i] = P4(seq - 1 - i); the oldest one is P4(oldest).
				std::vector<VpLaneProducts::SourceStatsRecord> recs(1, prev);
				uint64_t oldest = seq - 1;
				FadeRun start;
				bool joined = false;
				while (recs.size() < kSceneRunRing && oldest > 1)
				{
					VpLaneProducts::SourceStatsRecord older;
					if (!ReadP4(b, oldest - 1, &older))
						break;
					recs.push_back(older);
					--oldest;
					if (entryEndingAt(oldest, recs.back(), start))
					{
						joined = true;
						break;
					}
				}
				// Exact when the walk joined a ring entry or reached the stream's first frame (nothing earlier to know); else reduced evidence.
				if (joined || oldest <= 1)
				{
					if (seq > 2)
						st.sceneRunRebuilt.fetch_add(1, std::memory_order_relaxed);
				}
				else
					st.sceneRunReduced.fetch_add(1, std::memory_order_relaxed);
				run = start;
				for (size_t i = recs.size() - 1; i > 0; --i)
				{
					const VpLaneProducts::EventSignals e = VpLaneProducts::ClassifyEvents(recs[i], recs[i - 1], false, false);
					VpUnit::CutFadeJudge::FoldFadeRun(recs[i], recs[i - 1], e, run);
				}
				return run;
			}

			// SceneFactsOf's body; `done` (may be nullptr) gets the frames whose confirmed sequence verdict arrived in this call.
			// `ahead` (GF-P2a S2b): `seq` is ahead of the render seal (SkeletonMemo::AheadOfSeal). Its fade run is folded as usual and kept in
			// the AHEAD ring (sceneRunsAhead, C1), never in sceneRuns: an ahead frame 8+ out would overwrite the render frames' entries, and
			// their runs would then be rebuilt with reduced evidence (sceneRunReduced). With the store on the walk front is usually the only
			// fill of a frame (the seal finds its confirmed group), so the ahead ring is what keeps that chain exact.
			bool SceneFactsFor(HostState& st, Bundle& bundle, uint64_t seq, SceneFacts& out, std::vector<uint64_t>* done, bool ahead)
			{
				out = SceneFacts();
				if (seq < 2)
				{
					out.why = "no previous frame";
					return false;
				}
				// GET-OR-COMPUTE-AND-PUBLISH (OD 11-12 17:26 / 11-13 13:34): P4 of both frames is ASKED for through the renderer's memo, not peeked.
				// A sealed / live slot is served through the lane binder exactly as ReadOf did; a frame AHEAD of the seal makes P4 in the lane's own
				// slot now (or reuses the published one, never twice); off the render thread the lane's published bytes are copied. Both results
				// stay alive (their pins) until both records are decoded.
				// C5-SKREDS: the sequence judge below reads P4 of every frame from its tracker's newest up to `seq` (ReadP4, a peek); a frame
				// between the seal and seq - 1 that nobody asked for yet would be unreadable and END the judge's part there (its rule), so an
				// ahead frame would be judged without its context (a real cut two frames ahead of the seal judged no cut). On the render
				// thread those frames are got-or-computed too, in order, through the same memo (made once: the seal later accepts them).
				if (ahead && seq >= 3 && bundle.memo->OnRenderThread())
				{
					uint64_t first = seq - 1;
					while (first > 1 && seq - first < kScenePrimeFrames && bundle.memo->AheadOfSeal(first - 1))
						--first;
					for (uint64_t s = first; s + 1 < seq; ++s)
						(void)bundle.memo->Acquire(VpSkeleton::kP4SourceStats, s, VpSkeleton::AcquireForm::Cpu);   // the record stays in its slot / spot
				}
				const VpSkeleton::AcquireResult cur = bundle.memo->Acquire(VpSkeleton::kP4SourceStats, seq, VpSkeleton::AcquireForm::Cpu);
				const VpSkeleton::AcquireResult prev = bundle.memo->Acquire(VpSkeleton::kP4SourceStats, seq - 1, VpSkeleton::AcquireForm::Cpu);
				VpLaneProducts::SourceStatsRecord a, c;
				if (!cur.Ok() || !prev.Ok() || cur.cpu == nullptr || prev.cpu == nullptr || cur.cpuBytes != VpLaneProducts::kStatsBytes ||
					prev.cpuBytes != VpLaneProducts::kStatsBytes || !VpLaneProducts::ReadSourceStats(prev.cpu, prev.cpuBytes, &a) ||
					!VpLaneProducts::ReadSourceStats(cur.cpu, cur.cpuBytes, &c))
				{
					const VpSkeleton::AcquireResult& bad = !cur.Ok() ? cur : prev;
					out.why = !bad.Ok() ? VpSkeleton::Token(bad.cause) : "P4 record unreadable";
					return false;
				}
				// Scene EVENTS (cut / fade / flash) are scene facts: classified from the P4 records alone. This function reads NO validity source
				// (no VpIndicator::TemporalValid, no Indicator at all): whether a vector pair is usable across a resync is answered only by the P5a
				// MotionValidity facade (coordinator ruling 2026-11-08; tools/test_check_shared_units.py test_scene_facts_reads_no_validity_source).
				VpLaneProducts::EventSignals e = VpLaneProducts::ClassifyEvents(a, c, false, false);
				double fadeKeep = 1.0;
				{
					// U7 v3: the multi-frame fade law folds this pair into the run that ends at P4(seq - 1); the run that ends here is kept for
					// seq + 1. The run is ended by the pair law's cut / flash (the law as designed: one record pair at a time, independent of
					// the sequence verdict's one-frame delivery); a sequence-law cut still restarts every consumer whatever the fade says.
					// The frame's history keep (the partial reset, owner 2026-11-15) comes from the same fold: CutFadeJudge::FrameFadeKeep.
					std::lock_guard<std::mutex> runLock(st.sceneRunMutex);
					VpUnit::CutFadeJudge::FadeRun run = SceneRunBefore(st, bundle, seq, a);
					double runKeep = 1.0;
					const bool runFade = VpUnit::CutFadeJudge::FoldFadeRun(a, c, e, run, &runKeep);
					fadeKeep = VpUnit::CutFadeJudge::FrameFadeKeep(a, c, e.fade, runFade, runKeep);
					e.fade = runFade || e.fade;
					// S2b C1: an ahead frame keeps its run in the ahead ring (the walk front chains exactly; render entries are never clobbered).
					HostState::SceneRunEntry& kept = (ahead ? st.sceneRunsAhead : st.sceneRuns)[static_cast<size_t>(seq % kSceneRunRing)];
					kept.seq = seq;
					kept.run = run;
				}
				// U7 v2: cut / flash / cutPossible / strength from the sequence law (the renderer's tracker).
				VpUnit::CutFadeJudge::CutVerdict v;
				{
					std::lock_guard<std::mutex> lock(st.sceneMutex);
					v = SceneVerdictLocked(st, bundle, seq, done);
				}
				if (!v.known)
				{
					out.why = "P4 record unreadable";   // the sequence judge could not read the frame's record (15c-code: why names every miss)
					return false;
				}
				out.valid = true;
				out.cut = v.cut;
				out.cutPossible = v.candidate;
				out.flash = v.flash;
				out.confirmed = v.confirmed;
				out.gridLaw = v.gridLaw;
				out.cutStrength = v.strength;
				out.pairCut = e.cut;
				out.pairCutPossible = e.cutPossible;
				out.pairFlash = e.flash;
				out.fade = e.fade;
				out.fadeKeep = static_cast<float>(fadeKeep);
				out.histDistance = e.histDistance;
				out.prevMean = a.mean;
				out.curMean = c.mean;
				out.prevSpread = a.p99 > a.p01 ? a.p99 - a.p01 : 0u;
				out.curSpread = c.p99 > c.p01 ? c.p99 - c.p01 : 0u;
				out.rectFound = c.rectFound != 0;
				out.rectX0 = c.rectX0;
				out.rectY0 = c.rectY0;
				out.rectX1 = c.rectX1;
				out.rectY1 = c.rectY1;
				return true;
			}

			// The same, ahead-ness read from the renderer's memo now (SceneFactsOf).
			bool SceneFactsFor(HostState& st, Bundle& bundle, uint64_t seq, SceneFacts& out, std::vector<uint64_t>* done)
			{
				return SceneFactsFor(st, bundle, seq, out, done, bundle.memo->AheadOfSeal(seq));
			}
		}

		bool SkeletonModule::SceneFactsOf(const void* host, uint64_t seq, SceneFacts& out) const
		{
			out = SceneFacts();
			const std::shared_ptr<HostState> st = hosts_.Find(host);
			const std::shared_ptr<Bundle> b = st ? std::atomic_load(&st->bundle) : nullptr;
			if (!b)
			{
				out.why = "no analysis lane";
				return false;
			}
			return SceneFactsFor(*st, *b, seq, out, nullptr);   // 15c-code's memo Acquire of the P4 pair lives in SceneFactsFor (U7 v2 body)
		}

		// The scene product's one fill: judge the frame (SceneFactsFor), decide the delivered cut, write the store's scene group when the store
		// holds the frame's record and confirm the provisional groups of earlier frames. The same body serves the seal (Begin) and a reader that
		// arrives before the seal / with no record.
		bool SkeletonModule::FillScene(HostState& st, uint64_t seq, SceneAnswer* answer) const
		{
			FrameAnalysis::Scene scene;
			scene.origin = FrameAnalysis::SceneOrigin::Vp;
			bool cached = false;
			{
				std::lock_guard<std::mutex> lock(st.sceneMutex);
				if (st.sceneCacheValid && st.sceneCacheSeq == seq)
				{
					scene.cut = st.sceneCacheCut;
					scene.fade = st.sceneCacheFade;
					scene.cutLate = st.sceneCacheLate;
					scene.cutHere = st.sceneCacheHere;
					scene.flash = st.sceneCacheFlash;
					scene.cutPossible = st.sceneCacheCutPossible;
					scene.confirmed = st.sceneCacheConfirmed;
					scene.cutStrength = st.sceneCacheStrength;
					scene.fadeKeep = st.sceneCacheFadeKeep;
					cached = true;
				}
			}
			// U7 v2: frames whose confirmed verdict arrived with this frame (normally seq - 1); their Vp groups are rewritten below.
			std::vector<uint64_t> done;
			bool ahead = false;   // MUST-7: this frame is ahead of the render seal (set below for a classified frame)
			// MUST-8: one hold of st.sceneMutex from the delivery decision through the store publish and the confirmation rewrite, so a
			// provisional publish of seq can never land after (or between) the rewrite of seq by a fill of seq + 1 on another thread.
			// Order: sceneMutex, then the store's own mutex (the store calls nothing back). SceneFactsFor runs before it (it locks itself).
			std::unique_lock<std::mutex> sceneLock(st.sceneMutex, std::defer_lock);
			if (!cached)
			{
				const std::shared_ptr<Bundle> b = std::atomic_load(&st.bundle);
				// MUST-7 (A20-C5 ruling (A), replacing GV's amendment): the fill is split by RENDER ORDER, not by thread. A frame ahead of
				// the render seal (the LED look-ahead walk) is classified and its own group published (computed once, found by the seal:
				// OD 2026-11-15), but it never claims an earlier frame's late cut except its natural predecessor's, never moves the late
				// search (sceneLastFilled) and never writes the render frame's sceneCache or the render sceneRuns ring (S2b: it keeps its run in sceneRunsAhead).
				ahead = b && b->memo && b->memo->AheadOfSeal(seq);
				// seq - 1 has had its render-order fill (it is not ahead of the seal itself).
				const bool prevRendered = ahead && seq > 1 && !b->memo->AheadOfSeal(seq - 1);
				SceneFacts f;
				if (!b)
					f.why = "no analysis lane";
				if (!b || !SceneFactsFor(st, *b, seq, f, &done, ahead) || !f.valid)
				{
					if (answer)
						answer->why = f.why;
					return false;
				}
				FrameAnalysis::Store* const deliveryStore = VpFrameAnalysis::SourceFrameTap::StoreFor(st.host);
				sceneLock.lock();   // MUST-8: held through the store publish and the confirmation rewrite below
				// Delivery (FrameAnalysis::Scene::cut): this frame's confirmed cut now, else the OLDEST confirmed cut since the last frame
				// judged that did not go out with its own group (it was provisional then): exactly once per cut. Normally that is frame
				// seq - 1 (one frame late); when frame c + 1 had no group (dropped, unsealed or no record) cut c is confirmed at the part end
				// and goes out with the next frame that has one (counted: sceneLateGap). Reach: a gap longer than the tracker's re-prime
				// span (kScenePrimeFrames) restarts the tracker, and a cut inside it is not judged at all (counted as a re-prime, refs/102b).
				// A provisional verdict never delivers a cut (on the
				// footage it holds false cuts the successor removes: refs/102b U7-accuracy).
				HostState::SceneVerdictSlot& own = st.sceneVerdicts[seq % HostState::kSceneVerdicts];
				const bool ownSlot = own.used && own.seq == seq;
				// MUST-8: SceneFactsFor judged outside this lock; a fill of seq + 1 on another thread may have confirmed this frame's
				// verdict since. Its confirmed verdict wins (never a downgrade: only a confirmed slot refreshes a provisional answer).
				if (ownSlot && own.v.known && own.v.confirmed && !f.confirmed)
				{
					f.confirmed = true;
					f.cut = own.v.cut;
					f.flash = own.v.flash;
					f.cutPossible = own.v.candidate;
					f.cutStrength = own.v.strength;
				}
				bool late = false;
				uint64_t nextUndelivered = 0;
				if (ownSlot && own.carriesLate)
					late = true;   // a re-fill of the same picture: the late cut it carried stays with it
				else if (ahead)
				{
					// MUST-7: an ahead fill claims only frame seq - 1's late cut (its natural successor's delivery), and only when that
					// frame's own group can no longer carry it: seq - 1 had its render-order fill already, or its Vp group is out (the `done`
					// rewrite below confirms it, so the seal skips it and the cut would otherwise be stranded). Anything older belongs to the
					// render-order fills (frame N + 1 delivers cut N, never an ahead N + k).
					const uint64_t d = seq - 1;
					HostState::SceneVerdictSlot& c = st.sceneVerdicts[d % HostState::kSceneVerdicts];
					if (d >= 1 && c.used && c.seq == d && c.v.known && c.v.confirmed && c.v.cut && !c.deliveredOwn && !c.deliveredLate)
					{
						bool claim = prevRendered;
						if (!claim && deliveryStore != nullptr && deliveryStore->Enabled())
						{
							// Its group is out (the seal will skip it once confirmed), or it has no record at all (records open at
							// arrival, so seq - 1 was dropped or evicted: it never gets a group of its own).
							FrameAnalysis::Scene g;
							claim = (deliveryStore->PeekScene(d, g) && g.origin == FrameAnalysis::SceneOrigin::Vp) || !deliveryStore->HasRecord(d);
						}
						else if (!claim)
						{
							// C5-SKREDS (owner: the store on / off judge alike, no silent difference): with the store off seq - 1 has no group
							// that could ever carry its cut, and its natural successor (this frame) is the one that delivers it in render order
							// anyway (the late search below). The ahead fill delivers it now, kept in this frame's slot (gate N1 below:
							// carriesLate, so the render-order re-fill of this frame carries the same cut, exactly once).
							claim = true;
						}
						// Gate N1: the claim must be kept somewhere: this frame's slot (carriesLate, so a re-fill carries it) or its store
						// record (the published group). Without either (JudgeSceneApart far past the tracker, no record yet) the render-order
						// fill delivers it instead.
						if (claim && !ownSlot)
							claim = deliveryStore != nullptr && deliveryStore->Enabled() && deliveryStore->HasRecord(seq);
						if (claim)
						{
							late = true;
							c.deliveredLate = true;
							if (ownSlot)
								own.carriesLate = true;
							st.sceneLate.fetch_add(1, std::memory_order_relaxed);
						}
					}
				}
				else
				{
					uint64_t lowest = seq - 1;
					if (st.sceneHasFilled && st.sceneLastFilled < seq)
						lowest = st.sceneLastFilled;
					if (seq - lowest > HostState::kSceneVerdicts - 1)
						lowest = seq - (HostState::kSceneVerdicts - 1);
					if (lowest == 0)
						lowest = 1;
					for (uint64_t d = lowest; d < seq; ++d)
					{
						HostState::SceneVerdictSlot& c = st.sceneVerdicts[d % HostState::kSceneVerdicts];
						if (c.used && c.seq == d && c.v.known && c.v.confirmed && c.v.cut && !c.deliveredOwn && !c.deliveredLate)
						{
							if (late)
							{
								// A second undelivered cut in the window (one cut per group): the next fill starts its search here.
								nextUndelivered = d;
								break;
							}
							late = true;
							c.deliveredLate = true;
							if (ownSlot)
								own.carriesLate = true;
							st.sceneLate.fetch_add(1, std::memory_order_relaxed);
							if (d + 1 < seq)
								st.sceneLateGap.fetch_add(1, std::memory_order_relaxed);
						}
					}
				}
				if (ahead)
				{
					// MUST-7: the late search is the render order's (an ahead frame neither starts nor moves it).
				}
				else if (nextUndelivered != 0)
				{
					st.sceneHasFilled = true;
					st.sceneLastFilled = nextUndelivered;   // still owed: the next fill delivers it
				}
				else if (!st.sceneHasFilled || seq > st.sceneLastFilled || seq + HostState::kSceneVerdicts < st.sceneLastFilled)
				{
					st.sceneHasFilled = true;
					st.sceneLastFilled = seq;   // a seek back (far below the newest) starts the search over
				}
				// Already delivered late with a later frame (that frame was filled first): not delivered again here.
				const bool now = f.confirmed && f.cut && !(ownSlot && own.deliveredLate);
				if (now && ownSlot)
					own.deliveredOwn = true;
				scene.cut = now || late;
				scene.cutLate = late;
				scene.cutHere = f.cut;
				scene.fade = f.fade;
				scene.flash = f.flash;
				scene.cutPossible = f.cutPossible;
				scene.confirmed = f.confirmed;
				scene.cutStrength = f.cutStrength;
				scene.fadeKeep = f.fadeKeep;   // U7 v3: 1 on a frame that is no fade
				if (f.fade && f.fadeKeep < 1.0f)
					st.sceneFadePartial.fetch_add(1, std::memory_order_relaxed);
				if (!ahead)   // MUST-7: the cache is the render frame's (one frame's classification); an ahead fill never overwrites it
				{
					st.sceneCacheSeq = seq;
					st.sceneCacheValid = true;
					st.sceneCacheCut = scene.cut;
					st.sceneCacheFade = scene.fade;
					st.sceneCacheLate = scene.cutLate;
					st.sceneCacheHere = scene.cutHere;
					st.sceneCacheFlash = scene.flash;
					st.sceneCacheCutPossible = scene.cutPossible;
					st.sceneCacheConfirmed = scene.confirmed;
					st.sceneCacheStrength = scene.cutStrength;
					st.sceneCacheFadeKeep = scene.fadeKeep;
				}
			}
			if (answer)
			{
				answer->known = true;
				answer->cut = scene.cut;
				answer->fade = scene.fade;
				answer->cutLate = scene.cutLate;
				answer->cutHere = scene.cutHere;
				answer->flash = scene.flash;
				answer->cutPossible = scene.cutPossible;
				answer->confirmed = scene.confirmed;
				answer->cutStrength = scene.cutStrength;
				answer->fadeKeep = scene.fadeKeep;
			}
			if (cached)
				st.sceneCacheHits.fetch_add(1, std::memory_order_relaxed);
			// A store that is registered but OFF (frame_analysis_store=false: the Host always registers it, disabled) takes nothing.
			// Peek-then-publish is not atomic, but the skeleton is the only scene producer today (a second one could at worst be overwritten
			// with the same classification), so no compare-and-set is spent on it.
			if (FrameAnalysis::Store* store = VpFrameAnalysis::SourceFrameTap::StoreFor(st.host))
				if (store->Enabled())
				{
					if (!sceneLock.owns_lock())
						sceneLock.lock();   // MUST-8 (the cached path): the publish and the rewrite are under st.sceneMutex too
					// SceneCounters::published counts records written at a SEAL (render order); an ahead publish is the reader's
					// get-or-compute write, found by the seal later (alreadyPresent), so it is not counted there (MUST-7).
					if (store->PublishScene(seq, scene) && !ahead)
						st.scenePublished.fetch_add(1, std::memory_order_relaxed);
					if (scene.confirmed && scene.cutHere)
						NoteConfirmedCut(seq);   // pairs into it the veto passed before this group was confirmed
					// The confirmation of earlier frames (normally seq - 1; `done` has any others): only a PROVISIONAL group this module wrote is rewritten
					// (another producer's group, or one already confirmed, is left alone); its fade is kept.
					if (seq > 1)
						done.push_back(seq - 1);
					for (const uint64_t d : done)
					{
						if (d == seq)
							continue;
						FrameAnalysis::Scene old;
						if (!store->PeekScene(d, old) || old.origin != FrameAnalysis::SceneOrigin::Vp || old.confirmed)
							continue;
						// st.sceneMutex is held (sceneLock, MUST-8).
						const HostState::SceneVerdictSlot& slot = st.sceneVerdicts[d % HostState::kSceneVerdicts];
						if (!(slot.used && slot.seq == d && slot.v.known && slot.v.confirmed))
							continue;
						const VpUnit::CutFadeJudge::CutVerdict v = slot.v;
						old.cutHere = v.cut;   // the delivered `cut` / cutLate stay as written (exactly once per cut)
						old.flash = v.flash;
						old.cutPossible = v.candidate;
						old.cutStrength = v.strength;
						old.confirmed = true;
						if (store->PublishScene(d, old))
							st.sceneRepublished.fetch_add(1, std::memory_order_relaxed);
						if (v.cut)
							NoteConfirmedCut(d);
					}
				}
			return true;
		}

		SceneAnswer SkeletonModule::SceneOf(const void* host, uint64_t seq) const
		{
			SceneAnswer a;
			const std::shared_ptr<HostState> st = hosts_.Find(host);
			if (!st)
			{
				a.why = "unknown renderer";
				return a;
			}
			if (FrameAnalysis::Store* store = VpFrameAnalysis::SourceFrameTap::StoreFor(host))
			{
				FrameAnalysis::Scene scene;
				if (store->PeekScene(seq, scene))
				{
					a.known = true;
					a.cut = scene.cut;
					a.fade = scene.fade;
					a.cutLate = scene.cutLate;
					a.cutHere = scene.cutHere;
					a.flash = scene.flash;
					a.cutPossible = scene.cutPossible;
					a.confirmed = scene.confirmed;
					a.cutStrength = scene.cutStrength;
					a.fadeKeep = scene.fadeKeep;
					a.from = SceneAnswer::From::Store;
					st->sceneStoreReads.fetch_add(1, std::memory_order_relaxed);
					return a;
				}
			}
			const uint64_t hitsBefore = st->sceneCacheHits.load(std::memory_order_relaxed);
			if (FillScene(*st, seq, &a))
			{
				a.from = SceneAnswer::From::Live;
				if (st->sceneCacheHits.load(std::memory_order_relaxed) == hitsBefore)
					st->sceneLiveFills.fetch_add(1, std::memory_order_relaxed);
				return a;
			}
			st->sceneUnknown.fetch_add(1, std::memory_order_relaxed);
			return a;
		}

		SceneCounters SkeletonModule::SceneCountersOf(const void* host) const
		{
			SceneCounters c;
			if (const std::shared_ptr<HostState> st = hosts_.Find(host))
			{
				c.published = st->scenePublished.load(std::memory_order_relaxed);
				c.alreadyPresent = st->sceneAlready.load(std::memory_order_relaxed);
				c.notReady = st->sceneNotReady.load(std::memory_order_relaxed);
				c.storeReads = st->sceneStoreReads.load(std::memory_order_relaxed);
				c.liveFills = st->sceneLiveFills.load(std::memory_order_relaxed);
				c.cacheHits = st->sceneCacheHits.load(std::memory_order_relaxed);
				c.fadeRunRebuilt = st->sceneRunRebuilt.load(std::memory_order_relaxed);
				c.fadeRunReduced = st->sceneRunReduced.load(std::memory_order_relaxed);
				c.fadePartial = st->sceneFadePartial.load(std::memory_order_relaxed);
				c.unknown = st->sceneUnknown.load(std::memory_order_relaxed);
				c.confirmed = st->sceneConfirmed.load(std::memory_order_relaxed);
				c.provisional = st->sceneProvisional.load(std::memory_order_relaxed);
				c.reprimes = st->sceneReprimes.load(std::memory_order_relaxed);
				c.republished = st->sceneRepublished.load(std::memory_order_relaxed);
				c.late = st->sceneLate.load(std::memory_order_relaxed);
				c.lateGap = st->sceneLateGap.load(std::memory_order_relaxed);
				c.noGrid = st->sceneNoGrid.load(std::memory_order_relaxed);
			}
			return c;
		}

		// Render thread. AfterRender (retryOnly = false): takes the engine's newest verified field and publishes its slots; Begin and AfterRender
		// both retry a field whose input slots were not there yet. Validity is the engine's own facade: the field's stamp (MotionValidity::StampAt),
		// FieldEra (a restart of the numbering forgets what was pending) and PairSpansSlip (a pair that spans a slip is not published).
		void SkeletonModule::FeedEngine(HostState& st, bool retryOnly)
		{
			if (!st.feedNeeded)
				return;
			const std::shared_ptr<Bundle> b = std::atomic_load(&st.bundle);
			if (!b)
				return;
			if (!st.engine)
				st.engine = engineProvider_ ? engineProvider_(st.host) : DefaultEngine(st.host);
			VpMotion::IVectorEngine* e = st.engine;
			if (!e)
			{
				if (!retryOnly)
					++st.feed.noEngine;
				return;
			}
			const uint64_t era = e->FieldEra();
			if (!st.feedHadEra || era != st.feedEra)
			{
				if (st.feedHadEra)
					++st.feed.eraResets;
				st.feedHadEra = true;
				st.feedEra = era;
				st.feedLastEngineSeq = 0;   // the numbering restarted: what was pending belongs to the old era
				st.feedPendingSeq = 0;
			}
			if (!retryOnly)
			{
				const uint64_t s = e->LatestVerifiedFieldSeq();
				if (s != 0 && s != st.feedLastEngineSeq && s != st.feedPendingSeq)
				{
					VpMotion::VerifiedField f;
					if (!e->FieldOf(s, f))
					{
						++st.feed.noField;
						st.feedLastEngineSeq = s;
					}
					else if (e->PairSpansSlip(f.pairFrom, f.pairTo))
					{
						++st.feed.slip;   // a pair across a gap / resync: no field of it is published, its readers fall back
						st.feedLastEngineSeq = s;
					}
					else
					{
						EngineFieldFacts ff;
						ff.seq = s;
						ff.pairFrom = f.pairFrom;
						ff.pairTo = f.pairTo;
						ff.era = era;
						ff.gridW = f.gridW;
						ff.gridH = f.gridH;
						ff.blockSize = f.blockSize;
						ff.scaleToSourcePx = f.scaleToSourcePx;
						ff.stamp = f.stamp;
						VpMotion::GlobalVec g;
						if (e->GlobalFromEngine() && e->EngineGlobal(s, g) && g.valid)
						{
							ff.globalValid = true;
							ff.globalX = g.x;
							ff.globalY = g.y;
						}
						st.feedPending = ff;
						st.feedPendingSeq = s;
						st.feedPendingMask = 0;
						st.feedAttempts = 0;
					}
				}
			}
			if (st.feedPendingSeq == 0)
				return;
			++st.feedAttempts;
			unsigned mask = st.feedPendingMask;
			const bool all = PublishEngineSlots(st, *b, st.feedPending, mask);
			st.feedPendingMask = mask;
			if (all)
			{
				st.facts[static_cast<size_t>(st.feedPendingSeq % kFactsRing)] = st.feedPending;
				++st.feed.published;
				st.feed.lastSeq = st.feedPendingSeq;
				st.feedLastEngineSeq = st.feedPendingSeq;
				st.feedPendingSeq = 0;
			}
			else if (st.feedAttempts >= kFeedRetries)
			{
				++st.feed.abandoned;
				st.feedLastEngineSeq = st.feedPendingSeq;
				st.feedPendingSeq = 0;
			}
		}

		// Publishes the P2 / P2m / P3 slots of facts.seq that the graph holds and that are not done yet (bits of doneMask), in topological order.
		// True when every one is done (published, or nothing more can be done for it).
		bool SkeletonModule::PublishEngineSlots(HostState& st, Bundle& b, EngineFieldFacts& facts, unsigned& doneMask)
		{
			const std::shared_ptr<VpSkeleton::LaneState> ls = b.lane.State();
			if (!ls || !ls->graph.valid || !ls->table)
				return false;
			VpSkeleton::SlotTable& table = *ls->table;
			const uint64_t seq = facts.seq;
			bool all = true;
			for (size_t i = 0; i < table.Count(); ++i)
			{
				const VpSkeleton::Node& n = table.NodeAt(i);
				unsigned bit = 0;
				if (n.id == VpSkeleton::kP2VectorPair)
					bit = 1;
				else if (n.id == VpSkeleton::kP2mMatcherPair)
					bit = 2;
				else if (n.id == VpSkeleton::kP3AnalysisField)
					bit = 4;
				else
					continue;
				if ((doneMask & bit) != 0)
					continue;
				if (n.id == VpSkeleton::kP2VectorPair && !facts.globalValid)
				{
					doneMask |= bit;   // the NVOF producer made no pair for this frame: the slot stays empty (its readers fall back)
					continue;
				}
				VpSkeleton::SlotSet& set = table.At(i);
				// The inputs are pinned BEFORE the claim (into a scratch header): a claim that is then abandoned leaves the slot Abandoned for
				// this frame, and the retry would find it Stale (a lost frame, counted as a claim failure: the engine feed's retry for
				// missing inputs could never succeed once a first attempt had claimed). Nothing is claimed while an input is missing.
				VpAncestry::Header scratch;
				VpSkeleton::LaneInputs pins;
				if (!table.LaneAncestors(n.id, seq, scratch, pins))
				{
					++st.feed.ancestorsMissing;   // an input (P1, P2m) is not published yet: retried at the next Begin / AfterRender
					all = false;
					continue;
				}
				VpSkeleton::LaneTicket ticket;
				const VpSkeleton::ClaimResult cr = set.Claim(seq, VpAncestry::Provenance::AheadLane, ~uint64_t(0), &ticket);
				if (cr == VpSkeleton::ClaimResult::TooEarly || cr == VpSkeleton::ClaimResult::ReleasePending)
				{
					pins.Unpin();
					all = false;   // the ring has not reached this frame yet: try again
					continue;
				}
				if (cr != VpSkeleton::ClaimResult::Claimed)
				{
					pins.Unpin();
					++st.feed.claimFailed;   // Stale / out of range: the frame already left the ring (or is published)
					doneMask |= bit;
					continue;
				}
				VpAncestry::Header& hdr = set.LaneHeader(ticket);
				hdr.ancestors = scratch.ancestors;
				hdr.ancestorCount = scratch.ancestorCount;
				hdr.chainReduced = scratch.chainReduced;
				hdr.reads = scratch.reads;
				hdr.honoured = facts.stamp;   // the stamp the engine made the field under; the seal of the frame compares it with StampOf(seq)
				const VpSkeleton::PublishResult pr = set.Publish(ticket);
				pins.Unpin();
				doneMask |= bit;
				if (pr == VpSkeleton::PublishResult::Published)
				{
					facts.p2Published = facts.p2Published || bit == 1;
					facts.p2mPublished = facts.p2mPublished || bit == 2;
					facts.p3Published = facts.p3Published || bit == 4;
				}
				else
					++st.feed.claimFailed;
			}
			return all;
		}


		// ---- queue (capture / queue threads) ----------------------------------------------------------
		void SkeletonModule::OnQueue(const RE::QueueFacts& f)
		{
			// QKP B (design s3 / amendment M5): the memo's epoch moves AT the call (any thread, coalesced per Begin interval); a Drop discards
			// its frames' spots. BacklogFlush is a timing restart, not a generation: nothing is released. Memo-only hosts included.
			if (const std::shared_ptr<MemoHost> mhq = memoHosts_.Find(f.host))
			{
				using IC = VpSkeleton::SkeletonMemo::InvalidateCause;
				switch (f.kind)
				{
				case RE::QueueEvent::Reset: mhq->memo->BumpEpoch(IC::Reset); break;
				case RE::QueueEvent::LiveQueueReset: mhq->memo->BumpEpoch(IC::LiveQueueReset); break;
				case RE::QueueEvent::Flush: mhq->memo->BumpEpoch(IC::Flush); break;
				case RE::QueueEvent::Discontinuity: mhq->memo->BumpEpoch(IC::Discontinuity); break;
				case RE::QueueEvent::Gap: mhq->memo->BumpEpoch(IC::Gap); break;
				case RE::QueueEvent::FormatChange: mhq->memo->BumpEpoch(IC::FormatChange); break;
				case RE::QueueEvent::Drop:
					if (f.seq > 0 && f.frames != 0)
					{
						const uint64_t survivor = static_cast<uint64_t>(f.seq);
						const uint64_t n = (std::min<uint64_t>)(f.frames, survivor);
						if (n > kMaxDropsPerEvent)
							mhq->dropsLost.fetch_add(n - kMaxDropsPerEvent, std::memory_order_relaxed);
						for (uint64_t i = 0; i < n && i < kMaxDropsPerEvent; ++i)
							mhq->memo->Discard(survivor - n + i);
					}
					break;
				case RE::QueueEvent::BacklogFlush: break;
				}
			}
			if (f.kind != RE::QueueEvent::Drop || f.seq <= 0 || f.frames == 0)
				return;
			const std::shared_ptr<HostState> st = hosts_.Find(f.host);
			if (!st || !st->active.load(std::memory_order_acquire))
				return;
			// The call sites pass the sequence that SURVIVED the drop and the number of frames dropped
			// before it: the dropped frames are [seq - frames, seq).
			const uint64_t survivor = static_cast<uint64_t>(f.seq);
			const uint64_t count = (std::min<uint64_t>)(f.frames, survivor);
			// ASSUMPTION (backlog-flush call site): the frames dropped before the survivor carry consecutive
			// sequences. Counted and shown (dropsInferred); a hole would discard the wrong slot, never a wrong product.
			st->dropsInferred.fetch_add(count, std::memory_order_relaxed);
			if (count > kMaxDropsPerEvent)
				st->dropsLost.fetch_add(count - kMaxDropsPerEvent, std::memory_order_relaxed);
			std::lock_guard<std::mutex> lock(st->dropMutex);
			for (uint64_t i = 0; i < count && i < kMaxDropsPerEvent; ++i)
			{
				if (st->drops.size() >= kDropQueue)
					st->dropsLost.fetch_add(1, std::memory_order_relaxed);
				else
					st->drops.push_back(survivor - count + i);
			}
		}

		// QKP B (design s2): the queue window of this Begin, derived from the fed terms (R, L, W module-wide snapshot; P = the highest of the
		// lead window of presented lag; H = the owner fills' declared history, plus the lane graph's lag edges once the lane reports them),
		// handed to the memo BEFORE its seal and published to the board. Render thread, with or without the lane. A term nobody fed stays
		// unwired (named by Ctrl+Alt+D), never a default.
		void SkeletonModule::DeriveMemoWindow(const void* host, const std::shared_ptr<VpSkeleton::SkeletonMemo>& memo, uint64_t seq, const std::shared_ptr<HostState>& st, bool sampleTiming)
		{
			const std::shared_ptr<MemoHost> mh = memoHosts_.Find(host);
			if (!mh)
				return;
			const uint64_t now = nowUs_();
			// The period for the stale test of the render-ahead term: the Begin interval of this host (memo-only hosts have no lane period).
			if (sampleTiming)
			{
				const uint64_t lastBegin = mh->lastBeginUs.exchange(now, std::memory_order_relaxed);
				if (lastBegin != 0 && now > lastBegin && now - lastBegin <= 1000000u)
				{
					const uint64_t prev = mh->periodUs.load(std::memory_order_relaxed);
					mh->periodUs.store(prev != 0 ? (prev * 9u + (now - lastBegin)) / 10u : now - lastBegin, std::memory_order_relaxed);
				}
			}
			if (!(st && st->active.load(std::memory_order_acquire)) && (CurrentQueue().wired & QueueShape::kRenderAhead) != 0)
			{
				const uint64_t last = mh->lastQueueDepthUs.load(std::memory_order_relaxed);
				const uint64_t period = mh->periodUs.load(std::memory_order_relaxed);
				if (last != 0 && period != 0 && now > last + 8u * period)
					UnpublishRenderAhead();   // the plan stopped: the term is unwired (named), never an old number
			}
			const QueueShape q = CurrentQueue();
			VpSkeleton::QueueWindowTerms t;
			if (q.wired & QueueShape::kRenderAhead) { t.renderAhead = uint16_t(CappedRenderAhead(q.renderAhead)); t.wired = uint8_t(t.wired | VpSkeleton::QueueWindowTerms::kRenderAhead); }
			if (q.wired & QueueShape::kLaneLead) { t.laneLead = uint16_t((std::min<unsigned>)(q.laneLead, LaneLeadCap(q.renderAhead))); t.wired = uint8_t(t.wired | VpSkeleton::QueueWindowTerms::kLaneLead); }
			if (q.wired & QueueShape::kLookAhead) { t.lookAheadWalk = q.lookAheadWalk; t.wired = uint8_t(t.wired | VpSkeleton::QueueWindowTerms::kLookAhead); }
			const uint64_t presented = mh->lastPresentedSeq.load(std::memory_order_relaxed);
			if (presented != 0)
			{
				mh->presentLag.Sample(now, uint16_t(seq > presented ? (std::min<uint64_t>)(seq - presented, 255u) : 0u));
				t.presentLag = mh->presentLag.Value(now);
				t.wired = uint8_t(t.wired | VpSkeleton::QueueWindowTerms::kPresentLag);
			}
			// BASIS (R14-67): H and the ahead need count CPU AND GPU fills, views included: the window is about frames any reader may still
			// hold or compute ahead, whatever the form. The VRAM view (GPU allocating fills only, views excluded) stays in GpuNeedLevels, a different
			// question that must not be mixed into these terms.
			// H: wired, from the lane graph's input lag edges once the lane hands its graph here (TODO 102b QKP-B-LANE-GRAPH-H). Until then H
			// is 0 because those lag edges are NOT FED, not a measurement of zero (per-owner history is kept by the memo, below).
			// The owner fills' declared reader lag (DeclaredFillHistory) is NOT folded in: the memo already keeps each owner's spots by ITS
			// declared history (Entry::keep, EdgeLocked(seal, keep), ReleasedForOwnerLocked; P16-LATCH-EVIDENCE), and one owner's lag in the
			// memo-wide H would hold every other product's frames too (P16 kCodingEvidence, keepFrames = Monitor::kLatencyFrames = 6, is
			// registered in every skeleton: no frame of a keep-0 product would leave before seal - 6; L-CORE: released at the next seal).
			t.history = VpSkeleton::HistoryTermOf(nullptr, 0u);
			t.wired = uint8_t(t.wired | VpSkeleton::QueueWindowTerms::kHistory);
			// The active ahead consumers' declared needs (L-CORE accessor); none declared = fed, no consumer (aheadCompute 0 / NoConsumer).
			std::vector<VpSkeleton::AheadDemand> demands;
			for (const VpSkeleton::SkeletonMemo::AheadDemand& d : memo->ActiveAheadDemands())
			{
				VpSkeleton::AheadDemand ad;
				ad.need = d.depth;
				// GF-P1 AHEAD-DECL call site (GF-P2b): the requester's name (OwnerFill::aheadBy, a static string) reaches the window's existing
				// ceiling notice ("ahead compute 6 (by X; ceiling 6)"); an unnamed request keeps the form wording.
				ad.consumer = d.by != nullptr ? d.by : (d.form != 0 ? "owner fill (GPU)" : "owner fill (CPU)");
				demands.push_back(ad);
			}
			VpSkeleton::SetAheadConsumers(t, demands);
			const uint16_t prevAhead = memo->Window().aheadCompute;
			const VpSkeleton::QueueWindow w = VpSkeleton::DeriveWindow(t, seq, memo->Epoch());
			memo->SetWindow(w);
			mh->board.Publish(w);
			if (sampleTiming && st && w.aheadCompute != prevAhead)
			{
				if (const std::shared_ptr<Bundle> b = std::atomic_load(&st->bundle))
				{
					bool memoGranted = false;
					uint64_t memoBudget = 0;
					if (const std::shared_ptr<const VpSkeleton::GovernorPlan> gp = b->lane.CurrentGovernorPlan())
						if (const VpSkeleton::UserGrant* g = gp->Find(uint16_t(VpSkeleton::kUserSkeletonMemo)))
						{
							memoGranted = g->bytes > 0;
							memoBudget = g->bytes;
						}
					VpSkeleton::AheadNeed an = memo->MakeAheadNeed(w);
					an.grantBytes = memoGranted ? memoBudget : 0u;
					memo->SetAheadPlan(VpSkeleton::PlanAheadLadder(an));
				}
			}
		}

		const VpSkeleton::QueueWindowBoard* SkeletonModule::WindowBoardOf(const void* host) const
		{
			const std::shared_ptr<MemoHost> mh = memoHosts_.Find(host);
			return mh ? &mh->board : nullptr;   // valid while the host's memo table entry lives (render-thread / test use)
		}

		// ---- frame phases (render thread) -------------------------------------------------------------
		bool SkeletonModule::OnFrame(RE::FrameContext& c)
		{
			if (c.phase == RE::FramePhase::CollectAhead)
			{
				// P5a's CollectAhead call site: the input queue's size is the lane lead (frames queued ahead of the render). Inert off.
				if (c.phaseData != nullptr)
				{
					const std::shared_ptr<HostState> q = hosts_.Find(c.host);
					if (q && q->active.load(std::memory_order_acquire))
						OnCollectAhead(*q, *static_cast<const RE::QueueView*>(c.phaseData));
					else if (const std::shared_ptr<MemoHost> mhq = memoHosts_.Find(c.host))
					{
						// QKP B: a memo-only host delivers the lane lead term too (its window needs L).
						const RE::QueueView& view = *static_cast<const RE::QueueView*>(c.phaseData);
						const uint8_t size = view.size > 255u ? uint8_t(255) : uint8_t(view.size);
						PublishLaneLead(double(LowestLeadSample(mhq->leadMutex, mhq->leadWindow, nowUs_(), size)));
					}
				}
				return false;
			}
			if (c.phase == RE::FramePhase::Entrance) { if (const std::shared_ptr<HostState> e = hosts_.Find(c.host)) e->budgetFeed.NoteCaptureRate(c.captureRateHz); return false; }   // S0: measured rate for the slot budget
			if (c.phase != RE::FramePhase::Begin && c.phase != RE::FramePhase::AfterRender)
				return false;
			if (c.phase == RE::FramePhase::AfterRender)
				if (const std::shared_ptr<MemoHost> mh = memoHosts_.Find(c.host))
					if (mh->gpuDevice)
						mh->gpuDevice->FlushReleases();   // R2: GPU-legal: the owner textures released at the seal are destroyed
			if (c.phase == RE::FramePhase::Begin)
				ReconcileIfRaiseMoved(c.host);   // 15c-c8: a consumer's raising demand made / withdrawn on another thread is applied here
			const std::shared_ptr<HostState> st = hosts_.Find(c.host);
			// L-CORE s3: the renderer's memo is sealed at every Begin with or without the lane. With a bound lane Begin() seals it after
			// the lane's seal (the seal reuses a publication); otherwise (no lane, lane waiting, lane off) it is sealed here.
			struct MemoSeal
			{
				std::shared_ptr<VpSkeleton::SkeletonMemo> memo;
				uint64_t seq = 0;
				uint64_t seals = 0;
				SkeletonModule* self = nullptr;
				const void* host = nullptr;
				~MemoSeal()
				{
					if (memo && memo->Seals() == seals)
					{
						memo->SetRenderThread(std::this_thread::get_id());
						memo->OnSeal(seq);   // releases the previous frame's GPU spots (their reserve) before the next grant
						self->PlanNoLaneGpu(host);
					}
				}
			} memoSeal;
			memoSeal.self = this;
			memoSeal.host = c.host;
			if (c.phase == RE::FramePhase::Begin && c.seq > 0)
				if ((memoSeal.memo = MemoHostOf(c.host, false)))
				{
					memoSeal.seq = static_cast<uint64_t>(c.seq);
					memoSeal.seals = memoSeal.memo->Seals();
				}
			if (memoSeal.memo)
				DeriveMemoWindow(c.host, memoSeal.memo, memoSeal.seq, st);
			if (!st || !st->active.load(std::memory_order_acquire) || c.seq <= 0)
				return false;
			const uint64_t epoch = VpFrameAnalysis::SourceFrameTap::Epoch();
			if (epoch != st->seenEpoch)
			{
				st->seenEpoch = epoch;
				if (st->ownerGen != VpFrameAnalysis::SourceFrameTap::Generation(c.host))
				{
					Reconcile(c.host, -1);   // stale state of a dead renderer at this address: rebuild
					return false;
				}
				if (!VpFrameAnalysis::SourceFrameTap::Wants(c.host))
					Reconcile(c.host, -1);   // the tap was lost: register it again
			}
			if (c.phase == RE::FramePhase::Begin)
				Begin(*st, c.seq);
			else
				AfterRender(*st, c.seq);
			return false;   // observation only: never a take-over
		}

		// R12 AfterPresent: ONE call per frame at the end of the host's present block (refs/107 S5). The kind maps onto the spare slot's trigger;
		// the slot keys per SOURCE frame (a cadence repeat keeps its film frame's key), so it fires at most once per source frame. Inert (no write
		// to the outcome) without an active lane for this host or with no sequence. Render thread.
		void SkeletonModule::OnAfterPresent(RE::FrameContext& c, RE::PresentContext& p)
		{
			// fx/dv-frame-quiet (refs/121): the frame's native GPU work VP deferred (the monitor's tile pass + copy, the hooks' copies)
			// is issued here, after the submit and outside libplacebo's frame, for every host and before any early return below.
			FrameQuiet::Deferred::EndThisFrame();
			const std::shared_ptr<HostState> st = hosts_.Find(c.host);
			if (c.seq > 0)
				if (const std::shared_ptr<MemoHost> mhq = memoHosts_.Find(c.host))
				{
					// QKP B: the presented source seq (a repeat re-presents the same seq: only a forward move counts), with or without the lane.
					const uint64_t pseq = static_cast<uint64_t>(c.seq);
					if (pseq > mhq->lastPresentedSeq.load(std::memory_order_relaxed))
					{
						mhq->lastPresentedSeq.store(pseq, std::memory_order_relaxed);
						mhq->memo->NotePresented(pseq);
					}
				}
			if (!st || !st->active.load(std::memory_order_acquire) || c.seq <= 0)
				return;
			const VpSkeleton::SpareTrigger trigger = p.kind == RE::AfterPresentKind::AfterLastExtra ? VpSkeleton::SpareTrigger::AfterLastExtra :
				p.kind == RE::AfterPresentKind::NoExtras ? VpSkeleton::SpareTrigger::PostPresentNoExtras : VpSkeleton::SpareTrigger::BeforeDequeueWait;
			st->budgetFeed.OnAfterPresent(st->spare, Vpv::Skeleton::FactsOf(c, p, trigger, st->timing.lastRenderOnlyMs, st->periodMs), p.presenter);   // S0
		}

		// The input-queue term of the lane lead. CALL CONTRACT: any thread, any rate (the host calls it once per frame; calls in bursts or several
		// per frame are fine): every call is one timestamped sample of the queue size, and the published value is the LOWEST size among the samples of
		// the last kLeadWindowSeconds of real time (a fall is taken at once, a rise only when the whole window is above it). A host that stops calling
		// leaves the last value published; the lane lead then ages out through the starve peak, not by itself. PublishLaneLead is a
		// compare-and-swap that leaves the other terms alone and does nothing when the value did not change.
		void SkeletonModule::OnCollectAhead(HostState& st, const RE::QueueView& view)
		{
			const uint8_t size = view.size > 255u ? uint8_t(255) : uint8_t(view.size);
			PublishLaneLead(double(LowestLeadSample(st.leadMutex, st.leadWindow, nowUs_(), size)));
		}

		void SkeletonModule::Begin(HostState& st, int64_t seqIn)
		{
			const uint64_t seq = static_cast<uint64_t>(seqIn);
			const uint64_t now = nowUs_();
			if (st.lastBeginUs != 0 && now > st.lastBeginUs)
			{
				const double dtMs = static_cast<double>(now - st.lastBeginUs) / 1000.0;
				if (dtMs >= 0.5 && dtMs <= 1000.0)   // a pause or a burst is not a period
					st.periodMs = st.periodMs > 0.0 ? 0.9 * st.periodMs + 0.1 * dtMs : dtMs;
			}
			st.lastBeginUs = now;
			++st.frames;

			Resync::HostResync* resync = Resync::ResyncExtension::For(st.host);
			VpSeal::FrameSeal* seal = resync ? &resync->seal : nullptr;
			if (!resync || !resync->shadow.Driving())
			{
				st.waiting = "waiting: resync_event=on required (one shared seal)";
				Refresh(st);
				return;
			}
			if (!st.sealHooked)
			{
				st.waiting = "waiting: the shared seal was not available when the lane was switched on (switch the lane on again)";
				Refresh(st);
				return;
			}
			if (!seal->Current())
			{
				st.waiting = "waiting: no Indicator sealed yet";
				Refresh(st);
				return;
			}
			const Dims dims{ st.srcW.load(std::memory_order_relaxed), st.srcH.load(std::memory_order_relaxed) };
			if (dims.width == 0 || dims.height == 0)
			{
				const uint64_t skipped = VpFrameAnalysis::SourceFrameTap::Skipped(st.host);
				st.waiting = skipped != 0 ?
					"waiting: source frames arrive but their luma could not be decoded (" + std::to_string(skipped) + " frame(s))" :
					"no source frames: needs frame_analysis_store=on";
				Refresh(st);
				return;
			}
			st.waiting.clear();

			GpuWiring wiring;
			const uint64_t sizeKey = (uint64_t(dims.width) << 32) | dims.height;
			if (st.gpuMemorySizeKey != sizeKey || st.gpuMemoryGen != st.settingsGen.load(std::memory_order_relaxed))
			{
				st.gpuMemoryNote.clear();   // a new size or settings generation: ask again
				st.gpuMemorySizeKey = sizeKey;
				st.gpuMemoryGen = st.settingsGen.load(std::memory_order_relaxed);
			}
			wiring.dispatcher = st.gpuMemoryNote.empty() ? st.dispatcher.get() : nullptr;
			if (st.dispatcher && wiring.dispatcher == nullptr)
				st.dispatcher->SetLane(nullptr);   // the old lane is about to be replaced: no dangling pointer
			wiring.gapAhead = wiring.dispatcher != nullptr && st.settings.gpuGap == GpuGapMode::AfterRender;
			wiring.compactGrain = wiring.dispatcher != nullptr && st.dispatcherCompact;
			const std::shared_ptr<Bundle> current = std::atomic_load(&st.bundle);
			const bool rebuild = !current || !SameDims(current->dims, dims) || !current->wiring.Same(wiring);
			std::shared_ptr<Bundle> b = current;
			if (rebuild)
			{
				std::shared_ptr<VpSkeleton::SkeletonMemo> hostMemo = MemoHostOf(st.host, true);
				if (!hostMemo)
					hostMemo = std::make_shared<VpSkeleton::SkeletonMemo>();   // memo table full: the lane keeps a memo of its own (not reachable by MemoFor)
				const std::weak_ptr<HostState> weakSt = hosts_.Find(st.host);
				VpSkeleton::LaneLeaseOf leaseOf = [weakSt](VpSkeleton::ProductId id) {
					// Any thread: the host's CURRENT bundle is held for the call (the lease then holds the lane state).
					if (const std::shared_ptr<HostState> s2 = weakSt.lock())
						if (const std::shared_ptr<Bundle> cur = std::atomic_load(&s2->bundle))
							return cur->lane.Lease(id);
					return VpSkeleton::LaneLease();
				};
				if (current)
					current->threads.Stop();   // GEM SK-LIFECYCLE: the current bundle's workers are joined before the replacement is built
				if (current)
					hostMemo->Clear();   // G0 S4: a graph rebuild is a discontinuity: every memo spot of the old graph is released
				b = std::make_shared<Bundle>(dims, seq, nowUs_, wiring, hostMemo, std::move(leaseOf),
					qualityP15_.load(std::memory_order_acquire), qualityP15c_.load(std::memory_order_acquire));
				st.leadPeak = 0;   // a new lane: the best lead is measured again
				st.leadPeakAtUs = now;
				st.lastUsedLead = 0;
				b->lane.BindIndicator(seal->Current());
				b->lane.OnSealed(seq, 0);   // the shared seal already passed this frame before this lane existed
			}
			const uint32_t gen = st.settingsGen.load(std::memory_order_relaxed);
			const bool periodMoved = b->configuredPeriodMs > 0.0 && st.periodMs > 0.0 &&
				std::abs(st.periodMs - b->configuredPeriodMs) > 0.3 * b->configuredPeriodMs;
			const bool periodKnown = b->configuredPeriodMs <= 0.0 && st.periodMs > 0.0;
			// A render-ahead term nobody refreshed for 8 source periods is stale (render-ahead was switched off, or its plan stopped): unwire it
			// (the depths fall back to their floors, shown by skeleton_queue) instead of keeping an old value wired.
			if ((CurrentQueue().wired & QueueShape::kRenderAhead) != 0 && st.periodMs > 0.0)
			{
				const uint64_t last = st.lastQueueDepthUs.load(std::memory_order_relaxed);
				if (last != 0 && now > last + static_cast<uint64_t>(8.0 * st.periodMs * 1000.0))
					UnpublishRenderAhead();
			}
			const QueueShape fedQueue = CurrentQueue();   // one snapshot per frame: a feed on another thread never races the graph build
			QueueShape queue = fedQueue;
			// The ring slots bound R + L together (RingSlotBudget); a feed above them is capped here, and the cap is shown (skeleton_lane_lead).
			queue.renderAhead = uint8_t(CappedRenderAhead(fedQueue.renderAhead));
			queue.laneLead = uint8_t((std::min<unsigned>)(fedQueue.laneLead, LaneLeadCap(fedQueue.renderAhead)));
			st.queueFed = fedQueue;
			st.queueUsed = queue;
			if (fedQueue.wired & QueueShape::kLaneLead)
			{
				// A decaying peak: the best lead of the last kLeadPeakHoldSeconds. A lead that stays lower for that long is the new normal (the
				// starve entry releases by itself); a dip shorter than that is shown.
				if (queue.laneLead >= st.leadPeak || now > st.leadPeakAtUs + static_cast<uint64_t>(kLeadPeakHoldSeconds * 1e6))
				{
					st.leadPeak = queue.laneLead;
					st.leadPeakAtUs = now;
				}
				if (queue.laneLead < st.lastUsedLead)
					++st.starveEvents;   // the queue the lane can look into got shorter: shown as skeleton_queue_starve while below the peak
				st.lastUsedLead = queue.laneLead;
			}
			const bool queueMoved = b->configuredQueue != queue;   // the record histories follow the queue (R + L + look-ahead)
			// MV7: the declared VRAM users (optical-flow engines, interpolator, camera model, BasicChroma's output, ...) moved: re-plan, but not
			// more often than a moved period (a producer re-sizing its textures must not rebuild the graph every frame).
			const uint64_t vramVersion = VpSkeleton::VramUsers::Instance().Version();
			const bool vramMoved = b->configuredVramVersion != vramVersion && b->framesSinceConfigure >= kReconfigureMinFrames;
			const uint64_t extVersion = externalVersion_.load(std::memory_order_acquire);
			const bool extMoved = b->configuredExtVersion != extVersion || st.vramReplan;   // external demand changed, or the shortfall did: re-plan at once (rare)
			// GEM-W1A S1, restored by GF-P2b MUST-4 (GV AMEND 1): the memo's GPU need follows its window (re-derived every frame) and its
			// lazily registered GPU fills (P1 GPU / edge map register at Entrance, after the first Begin's Configure), neither in QueueShape.
			// A bound lane is planned only here (PlanNoLaneGpu returns early), so a moved need re-plans, rate-limited like vramMoved (as the
			// no-lane path re-plans on plannedNeed). The lane's Configure re-plans a kept graph whose plan moved (AnalysisLane.h, MUST-4 (2)).
			const std::vector<uint64_t> memoLevels = b->memo->GpuNeedLevels();
			const uint64_t memoNeed = memoLevels.empty() ? 0u : memoLevels.front();
			const bool memoNeedMoved = b->configuredMemoNeed != memoNeed && b->framesSinceConfigure >= kReconfigureMinFrames;
			if (rebuild || b->configuredGen != gen || queueMoved || vramMoved || extMoved || memoNeedMoved
				|| ((periodMoved || periodKnown) && b->framesSinceConfigure >= kReconfigureMinFrames))
			{
				CardProfile card;
				std::string why;
				if (cardOverride_)
					card = card_;
				const bool known = cardOverride_ || QueryCard(hosts_.Aux(st.host), card, why);
				st.cardNote = known ? std::string() : why;
				TierInput tier;
				tier.framePeriodMs = st.periodMs;
				// MV7: the VRAM users outside the product catalog. What they hold is already inside the adapter's CurrentUsage, so it is VP's own
				// usage (re-planned, never counted twice) and each user is in the plan: the lane products are granted what is left.
				// OVER-GRANT NOTE: a STEPPED user (BasicChroma's output) that the plan sheds still holds its texture until its own module frees it
				// (nothing frees it for the governor yet), so the plan alone would grant that memory to products while it is occupied. The bytes a
				// user holds above what the plan gave it are planned as TRANSIENT in the next Configure (vramShortfall, below), and the plan's own
				// ownUsage drops only when the module Withdraws (VramUsers::Take: heldBytes follows Declare / Withdraw, never the plan).
				const VpSkeleton::VramUsers::Snapshot declared = VpSkeleton::VramUsers::Instance().Take();
				tier.otherUsers = declared.users;
				{
					// GEM SK-VRAM: the memo's GPU spots are a user of the SAME ledger (as PlanNoLaneGpu / MemoGpuGrant.h): added here from its
					// need levels, so the plan's scratch-arena grant (read below) exists. No second ledger.
					if (memoNeed != 0)
						tier.otherUsers.push_back(VpSkeleton::MakeMemoVramUser(memoLevels, "skeleton memo"));
				}
				tier.ownUsageBytes = declared.heldBytes;
				tier.transientBytes = st.vramShortfall;
				st.vramShortfallUsed = st.vramShortfall;
				st.vramReplan = false;
				GraphSettings gs;
				gs.dims = dims;
				gs.renderAhead = queue.renderAhead;
				gs.laneLead = queue.laneLead;
				const size_t capacity = resync->queueCapacity.load(std::memory_order_acquire);
				gs.queueDepth = static_cast<uint16_t>((std::max<size_t>)(4u, (std::min<size_t>)(capacity, 64u)));
				gs.lookAheadWalk = queue.lookAheadWalk;
				gs.ofaPresent = false;
				gs.cardProfileKey = card.Identity();
				ConsumerDecl report;
				report.id = 1;
				report.name = "skeleton.report";
				const VpSkeleton::ProductId consumed[] = { VpSkeleton::kP1HalfLuma, VpSkeleton::kP4SourceStats, VpSkeleton::kP10GrainSplit,
					VpSkeleton::kP11RegionBlockStats, VpSkeleton::kP12LightLevel };
				// U7 v2: the scene sequence judge reads P4 back to seq - kScenePrimeFrames (the re-prime window, the gap check against the
				// tracker's newest frame, a late reader's verdict): declared as a lag so the graph keeps those records (P4 history 1 alone
				// left them out of the ring: a frame without a record lost the cut after it, and a late reader judged without context).
				// 15c-c8: a lane raised by consumer demand reports only the CPU products P1 / P4 (the GPU products have no consumer there and
				// no dispatcher: nothing turns on without a consumer, coordinator ruling 2026-11-15).
				const size_t reported = st.laneRaise.empty() ? sizeof(consumed) / sizeof(consumed[0]) : 2u;
				for (size_t i = 0; i < reported; ++i)
					VpSkeleton::AddConsume(report, consumed[i],
						consumed[i] == VpSkeleton::kP4SourceStats ? uint8_t(kScenePrimeFrames) : uint8_t(0));
				std::vector<ConsumerDecl> consumers{ report };
				// SK-W stage D: the consumers other components declared (the camera link): their products and ancestors join the graph.
				std::vector<VpSkeleton::ProductId> outside;
				size_t duplicates = 0;
				bool wantsFit = false;
				CollectExternal(st.host, consumers, outside, wantsFit, duplicates);
				st.externalConsumers = consumers.size() - 1;
				st.externalDuplicates = duplicates;
				st.wantsCameraFit = wantsFit;
				// 15c-c8: the engine feed runs for an external consumer that reads the engine's pair / field or a camera product (whose inputs they
				// are), not for every external consumer: the LED drive reads P4 only, and a feed without a reader is work (and an "engine feed"
				// notice) with no consumer.
				st.feedNeeded = false;
				for (size_t i = 1; i < consumers.size(); ++i)
					st.feedNeeded = st.feedNeeded || ReadsEngineProduct(consumers[i]) || ReadsCameraProduct(consumers[i]);
				if (st.feedNeeded)
					for (VpSkeleton::ProductId p : { VpSkeleton::kP2VectorPair, VpSkeleton::kP2mMatcherPair, VpSkeleton::kP3AnalysisField })
					{
						bool have = false;
						for (VpSkeleton::ProductId q : outside)
							have = have || q == p;
						if (!have)
							outside.push_back(p);   // the engine feed publishes the pair and the field; the lane never makes them
					}
				b->lane.SetStormHold(Resync::ResyncExtension::StormHeld(st.host));
				// The dispatcher's private working memory (tap ring, grain-split working textures) is outside the product
				// declarations the governor plans: count it as used, and refuse the GPU products when it alone does not fit.
				if (wiring.dispatcher != nullptr)
				{
					// Counted only while the dispatcher holds nothing yet: afterwards the device query (CurrentUsage) already
					// includes the textures it allocated, and counting them again would shed more than needed.
					const bool held = wiring.dispatcher->HoldsVideoMemory();
					const uint64_t priv = held ? 0 : wiring.dispatcher->PrivateBytes(dims, wiring.gapAhead);
					if (known && !held && priv > card.FreeVram())
					{
						st.gpuMemoryNote = "GPU products not made: private working memory " + std::to_string(priv >> 20) +
							" MiB exceeds the free video memory " + std::to_string(card.FreeVram() >> 20) + " MiB (" +
							Dimensions(dims) + ")";
					}
					card.vramUsageBytes += priv;
				}
				// The registry is the Bundle's: the tap form (TapFormProducts) when a GPU dispatcher is wired, else the plain one;
				// either way built with the measured source period and the queue frames: the record histories
				// (quality_pattern, td.trail_summary) are max(floor seconds, queue x period).
				b->registry = Bundle::MakeRegistry(b->wiring, st.periodMs, VpSkeleton::QueueFrames(gs));
				b->lane.Configure(st.settings.lane, b->registry, consumers, gs, card, tier, seq, false);
				b->lane.SetExternalProducts(outside);
				// GF-P2b (gate G6 C2): the lane fills take the configured consumers' declared ahead depths (rebound when they move: also after
				// an extMoved Configure whose only change is a depth, which keeps the graph).
				b->BindConsumers(consumers);
				// Refresh the memo window immediately after BindConsumers so MakeAheadNeed sees the newly bound ahead depths (not the
				// pre-bind aheadCompute=0 snapshot from OnFrame before Begin).
				DeriveMemoWindow(st.host, b->memo, seq, hosts_.Find(st.host), false);
				{
					// B1: the memo's GPU spots are budgeted against the VRAM ledger: the memo user's grant of this plan (GF-P2b S10: its own
					// id kUserSkeletonMemo, MakeMemoVramUser above; the scratch arena's kUserScratchArena / Extra grants are the arena's).
					// No grant = GPU memo fills decline visibly (NoLedgerGrant), nothing is allocated outside the ledger.
					bool memoGranted = false;
					uint64_t memoBudget = 0;
					// GF-P2b MUST-4 (2): the CURRENT users plan (a kept lane state's users re-plan when the memo's need moved, else the state's).
					if (const std::shared_ptr<const VpSkeleton::GovernorPlan> gp = b->lane.CurrentGovernorPlan())
						if (const VpSkeleton::UserGrant* g = gp->Find(uint16_t(VpSkeleton::kUserSkeletonMemo)))
						{
							memoGranted = g->bytes > 0;
							memoBudget = g->bytes;
						}
					b->memo->SetGpuBudget(memoGranted, memoBudget);
					// GF-P2b MUST-4 (3) / S10b: the ahead plan follows the grant, exactly as the no-lane path (PlanNoLaneGpu), so a memo that was
					// planned without a lane never keeps that stale plan (NotPlannedAhead limits) once a lane binds and grants it.
					VpSkeleton::AheadNeed an = b->memo->MakeAheadNeed(b->memo->Window());
					an.grantBytes = memoGranted ? memoBudget : 0u;
					b->memo->SetAheadPlan(VpSkeleton::PlanAheadLadder(an));
				}
				{
					// What the declared users still hold above their grant: planned as transient at the next Configure (once).
					uint64_t shortfall = 0;
					if (const std::shared_ptr<const VpSkeleton::GovernorPlan> gp = b->lane.CurrentGovernorPlan())
						for (const std::pair<uint16_t, uint64_t>& h : declared.heldById)
							if (const VpSkeleton::UserGrant* g = gp->Find(h.first))
								if (g->bytes < h.second)
									shortfall += h.second - g->bytes;
					if (shortfall != st.vramShortfallUsed)
						st.vramReplan = true;
					st.vramShortfall = shortfall;
					// Which look-ahead products the queue could not feed (names of the short-queue substitutions), for the starve line: built here,
					// at a graph change, not every frame.
					std::string reduced;
					if (const std::shared_ptr<VpSkeleton::LaneState> ls = b->lane.State())
						for (const VpSkeleton::Node* n : ls->graph.ReducedNodes())
							if (n->reducedFrom != VpSkeleton::kNoProduct && b->registry.Has(n->reducedFrom))
								reduced += std::string(reduced.empty() ? "" : ", ") + b->registry.Get(n->reducedFrom).name;
					st.starveReduced = reduced;
				}
				b->configuredGen = gen;
				b->configuredPeriodMs = st.periodMs;
				b->configuredQueue = queue;
				b->configuredVramVersion = vramVersion;
				b->configuredExtVersion = extVersion;
				b->configuredMemoNeed = memoNeed;
				b->framesSinceConfigure = 0;
				++st.configures;
			}
			if (rebuild)
			{
				// Published after the graph is configured: a source frame arriving meanwhile waits in `pending`.
				std::atomic_store(&st.bundle, b);
				if (current)
					current->threads.Stop();
				DrainPending(st, *b);
			}
			++b->framesSinceConfigure;
			if (startWorkers_ && !b->threads.Running())
				b->threads.Start();   // false (no thread) while the lane has no graph; retried each frame

			// Drops reported by the queue thread, applied here (the lane is render-thread state).
			std::vector<uint64_t> drops;
			{
				std::lock_guard<std::mutex> lock(st.dropMutex);
				drops.swap(st.drops);
			}
			for (uint64_t d : drops)
			{
				b->lane.OnFrameDropped(d);
				++st.dropsApplied;
			}

			if ((st.feedNeeded || st.wantsCameraFit) && !st.engine)
				st.engine = engineProvider_ ? engineProvider_(st.host) : DefaultEngine(st.host);
			// Both directions: declaring the camera fit starts the engine (VectorDemand::Consumer::CameraFit), and WITHDRAWING the whole external
			// demand (feedNeeded and wantsCameraFit both false) must tell it so too. The push used to sit inside the "something is wanted" branch,
			// so the last demand going away left the engine running the fit for nobody.
			if (st.engine && st.wantsCameraFit != st.fitWantedPushed)
			{
				st.engine->SetCameraFitWanted(st.wantsCameraFit);
				st.fitWantedPushed = st.wantsCameraFit;
			}
			else if (!st.engine && st.fitWantedPushed)
				st.fitWantedPushed = false;
			FeedEngine(st, true);   // a field whose input slots (P1) were not published yet last time: publish before the seal of its frame
			b->lane.SetStormHold(Resync::ResyncExtension::StormHeld(st.host));
			uint32_t wait = 0;
			for (int guard = 0; guard < 64 && (wait = b->lane.DeterministicWaitUs(seq, 0)) > 0; ++guard)
				std::this_thread::sleep_for(std::chrono::microseconds(wait));   // N1: only a deterministic run or an explicit test wait
			b->lane.OnRenderStart(seq, 0);
			b->memo->SetRenderThread(std::this_thread::get_id());   // L-CORE s5: the render thread is declared at every Begin
			b->memo->OnSeal(seq);   // B1: the previous frame's memo spots are released (lifetime = frame)
			{
				std::lock_guard<std::mutex> cacheLock(st.sceneMutex);
				st.sceneCacheValid = false;   // one frame's classification only
			}
			// The SCENE record of this frame, at its seal (W1-C7): once per frame, skipped when already written, and no work at all while the
			// frame-analysis store is off (no store registered for the renderer). Published here, not at the P4 slot, because only a sealed P4
			// slot is readable (ReadOf); the camera describes its pairs after the seal of their frame.
			if (FrameAnalysis::Store* sceneStore = VpFrameAnalysis::SourceFrameTap::StoreFor(st.host))
			{
				// The Host registers its store even with frame_analysis_store=false, then DISABLED: Enabled() first, so an off store costs nothing.
				if (sceneStore->Enabled())
				{
					FrameAnalysis::Scene already;
					// U7 v2: a provisional group of this module's own (written by an early reader before frame seq + 1 existed) is judged again.
					if (sceneStore->PeekScene(seq, already) && !(already.origin == FrameAnalysis::SceneOrigin::Vp && !already.confirmed))
						st.sceneAlready.fetch_add(1, std::memory_order_relaxed);
					else if (!sceneStore->HasRecord(seq) || !FillScene(st, seq, nullptr))
						st.sceneNotReady.fetch_add(1, std::memory_order_relaxed);   // no record to write into, or no pair of P4 records: counted, not classified for nothing
				}
			}
			st.renderBeginSeq = seqIn;   // the render-only clock starts here (after the deterministic wait and the seal, which are not render work)
			st.renderBeginUs = nowUs_();
			Refresh(st);
		}

		void SkeletonModule::AfterRender(HostState& st, int64_t seqIn)
		{
			const uint64_t afterUs = nowUs_();   // first: the render-only time ends before this module's own AfterRender work
			const std::shared_ptr<Bundle> b = std::atomic_load(&st.bundle);
			if (!b)
				return;
			const uint64_t seq = static_cast<uint64_t>(seqIn);
			// S5: the span from the end of Begin to the entry of AfterRender is OBSERVED and shown (skeleton_regulator: observe only); the regulator is
			// fed only through NoteRenderOnlyMs. A frame without a Begin of its own, or with the period unmeasured, observes nothing (counted).
			if (st.renderBeginSeq == seqIn && st.periodMs > 0.0 && afterUs >= st.renderBeginUs)
			{
				const double renderMs = static_cast<double>(afterUs - st.renderBeginUs) / 1000.0;
				const double slackMs = st.periodMs - renderMs;
				const bool missed = renderMs > st.periodMs;
				// OBSERVED ONLY: Begin..AfterRender includes the render lock wait and the pacing sleeps, so it is not a render-only time and the
				// regulator is NOT fed with it (it would shed on waits). The regulator acts on NoteRenderOnlyMs (a span the host measured).
				++st.timing.frames;
				st.timing.missed += missed ? 1u : 0u;
				st.timing.lastSlackMs = slackMs;
				st.timing.lastRenderOnlyMs = renderMs;
				st.timing.lastPeriodMs = st.periodMs;
				st.timing.lastMissed = missed;
			}
			else
				++st.timing.skipped;
			st.renderBeginSeq = 0;
			FeedEngine(st, false);   // the pair the engine just estimated: its slots are sealed at the Begin of its frame
			if (st.dispatcher && b->wiring.dispatcher == st.dispatcher.get() && st.gpuMemoryNote.empty())
			{
				// GPU-legal (before the flush and the gap): destroy what a size / graph change queued, then (re)allocate the
				// product textures for the lane's current graph (a no-op while nothing changed). Never at Begin (no render
				// lock there). A refused product shows through the dispatcher's Fallbacks().
				st.dispatcher->FlushReleases();
				if (const std::shared_ptr<VpSkeleton::LaneState> ls = b->lane.State())
					if (ls->graph.valid)
						st.dispatcher->Configure(ls->graph, b->registry, ls->plan, b->dims, b->configuredGen);
			}
			else if (st.dispatcher && !st.gpuMemoryNote.empty() && st.dispatcher->HoldsVideoMemory())
			{
				// The memory refusal took effect after the frame that already allocated: give it all back now (GPU-legal).
				st.dispatcher->Release();
			}
			if (st.periodMs > 0.0)
			{
				b->lane.OnPostSubmitGap(nowUs_() + static_cast<uint64_t>(st.periodMs * 500.0));   // half the measured period
				++st.gapsRun;
			}
			else
				++st.gapsSkipped;   // the frame period is not measured yet: no gap is promised
			b->lane.OnFrameRetired(seq, 0);
		}

		// ---- status -----------------------------------------------------------------------------------
		// Render thread: rebuilds the cached text the other threads read (every frame at first and while
		// something changes, then every kRefreshEvery frames).
		void SkeletonModule::Refresh(HostState& st)
		{
			const std::shared_ptr<Bundle> b = std::atomic_load(&st.bundle);
			const bool hasBundle = b != nullptr;
			const uint8_t queueWired = CurrentQueue().wired;
			const uint64_t queueKey = (uint64_t(st.queueFed.laneLead) << 40) | (uint64_t(st.queueUsed.laneLead) << 32) | (uint64_t(st.queueFed.renderAhead) << 24) |
				(uint64_t(st.queueUsed.renderAhead) << 16) | (st.starveEvents & 0xFFFFu);
			// U7-accuracy Q5: the cut veto's reach (a store toggle, a second renderer) refreshes the text at once.
			const std::string vetoOff = SceneVetoOff(st.host);
			const bool noGrid = st.sceneNoGrid.load(std::memory_order_relaxed) != 0;   // the no-grid decision engages at once too
			const bool due = st.frames <= kEarlyRefreshFrames || st.frames % kRefreshEvery == 1 || st.waiting != st.shownWaiting ||
				hasBundle != st.shownBundle || queueWired != st.shownQueueWired || queueKey != st.shownQueueKey || vetoOff != st.shownVetoOff ||
				noGrid != st.shownNoGrid;
			if (!due)
				return;
			st.shownVetoOff = vetoOff;
			st.shownNoGrid = noGrid;
			st.shownQueueKey = queueKey;
			st.shownQueueWired = queueWired;
			st.shownWaiting = st.waiting;
			st.shownBundle = hasBundle;

			std::vector<std::string> fb;
			std::vector<std::string> full;
			auto addDecision = [&fb](const FallbackCatalog::Decision& d) {
				if (d.engaged)
					fb.push_back(d.id + ": " + d.reason);
			};
			addDecision(FallbackCatalog::SkeletonLane(st.waiting));
			std::string problems;
			for (const std::string& p : st.settings.laneProblems)
				problems += (problems.empty() ? "" : "; ") + p;
			addDecision(FallbackCatalog::SkeletonKeys(problems));
			addDecision(QueueDecision(queueWired));   // visible: the record histories sit at their floors until the queue is delivered
			{
				// The live queue feed (owner decision 2026-11-07): a feed above the ring-slot budget is capped, and a lane lead that fell below its
				// best is a visible queue starve (look-ahead products reduced, if the graph has any).
				std::string cap;
				if (st.queueFed.renderAhead > st.queueUsed.renderAhead)
					cap = "render-ahead depth capped at " + std::to_string(unsigned(st.queueUsed.renderAhead)) + " frame(s) (queue holds " +
						std::to_string(unsigned(st.queueFed.renderAhead)) + "; ring slot budget)";
				if (st.queueFed.laneLead > st.queueUsed.laneLead)
					cap += std::string(cap.empty() ? "" : "; ") + "lane lead capped at " + std::to_string(unsigned(st.queueUsed.laneLead)) +
						" frame(s) (queue holds " + std::to_string(unsigned(st.queueFed.laneLead)) + "; ring slot budget)";
				addDecision(FallbackCatalog::SkeletonLaneLeadCapped(cap));
				if (st.queueFed.wired & QueueShape::kLaneLead)
					addDecision(FallbackCatalog::SkeletonQueueStarve(st.queueUsed.laneLead, st.leadPeak, st.starveReduced, st.starveEvents));
				// S5: until the host delivers presentation facts the regulator's slack is derived from the render time.
				addDecision(FallbackCatalog::SkeletonRegulatorInput(hasBundle && !st.timing.measured, st.timing.noted, st.timing.lastNotedMs));
addDecision(FallbackCatalog::SkeletonVramKnown(hasBundle ? std::string(VpSkeleton::KnownUndeclaredVramUsers()) : std::string()));   // MV7 standing gap
				addDecision(FallbackCatalog::SkeletonEngineFeed(st.feedNeeded && st.engine == nullptr));   // SK-W stage D: P2m / P3 cannot be published
				addDecision(FallbackCatalog::SkeletonSceneVeto(vetoOff));   // U7-accuracy Q5: the cut veto's reach, never silent
				addDecision(FallbackCatalog::SkeletonSceneNoGrid(st.sceneNoGrid.load(std::memory_order_relaxed)));   // U7 v2 no-grid fallback
				// MV7 guard: a module that holds video memory is active but declared none of it: visible, never silent.
				addDecision(FallbackCatalog::SkeletonVramUsers(VpSkeleton::UnpopulatedUserNames(VpSkeleton::VramUsers::Instance().Take().unpopulated)));
			}
			// The GPU products (70b G5): shadow and "no render device" have no dispatcher and say so.
			const SkeletonGpuDispatcher* gd = st.dispatcher.get();
			const bool shadow = st.settings.lane.mode == LaneSettings::Mode::Shadow;
			addDecision(FallbackCatalog::SkeletonGpuShadow(shadow));
			addDecision(FallbackCatalog::SkeletonGpuDevice(!shadow && gd == nullptr && st.laneRaise.empty()));   // 15c-c8: raised = no GPU products by design
			if (st.sealLateHook)
				fb.push_back("skeleton_lane: switched on after the first frame; the lane binds the shared seal at the next source era");
			if (!st.cardNote.empty())
				fb.push_back("skeleton_lane: " + st.cardNote + "; the tier plan assumes none");
			const uint64_t evicted = st.pendingEvicted.load(std::memory_order_relaxed);
			const uint64_t lost = st.dropsLost.load(std::memory_order_relaxed);
			if (evicted != 0)
				fb.push_back("skeleton_lane: " + std::to_string(evicted) + " source frame(s) arrived before the lane graph or in another size and were not analysed");
			const uint64_t inferred = st.dropsInferred.load(std::memory_order_relaxed);
			if (inferred != 0)
				fb.push_back("skeleton_lane: " + std::to_string(inferred) + " dropped frame(s) were taken as consecutive sequences before the surviving frame (backlog flush)");
			if (lost != 0)
				fb.push_back("skeleton_lane: " + std::to_string(lost) + " queue drop(s) could not be reported to the lane");
			const unsigned long long faults = Faults().load(std::memory_order_relaxed);
			if (faults != 0)
				fb.push_back("skeleton_fault: " + std::to_string(faults) + " skeleton callback exception(s) contained; analysis may be incomplete");

			std::string line;
			// 15c-c8: a raised lane says by whom on Ctrl+I ("analysis lane on (demand: LED drive) ..."), and the store raised with it too.
			const std::string mode = !st.laneRaise.empty() ? "on (demand: " + st.laneRaise + ")" :
				st.settings.lane.mode == LaneSettings::Mode::Shadow ? std::string("shadow") : std::string("on");
			std::string storeDemand;
			const bool storeRaised = VpFrameAnalysis::SourceFrameTap::StoreRaisedByDemand(st.host) &&
				VpFrameAnalysis::SourceFrameTap::StoreDemanded(st.host, &storeDemand);
			if (b)
			{
				const AnalysisLane& lane = b->lane;
				for (const FallbackCatalog::Decision& d : lane.Fallbacks())
					addDecision(d);
				for (const FallbackCatalog::Decision& d : b->memo->Decisions())
					addDecision(d);   // B1 G5: "skeleton acquire <product>" (cause, frame) while a product is not served
				addDecision(FallbackCatalog::SkeletonGpuMemory(st.gpuMemoryNote));
				if (gd != nullptr && st.gpuMemoryNote.empty())
				{
					const bool gap = st.settings.gpuGap == GpuGapMode::AfterRender;
					addDecision(FallbackCatalog::SkeletonGpuLive(true));   // RunDeferredLive is not called before the Entrance phase
					addDecision(FallbackCatalog::SkeletonGpuGapAhead(gap));
					addDecision(FallbackCatalog::SkeletonGpuGapOff(!gap));
					addDecision(FallbackCatalog::SkeletonGpuChroma(true));   // P10c is not made before the source ring exists
					addDecision(FallbackCatalog::SkeletonGpuLightPending(b->lightPendingAsNone.load(std::memory_order_relaxed)));   // A2: visible until G6
					addDecision(FallbackCatalog::SkeletonGpuFailed(gd->Stats().failures, gd->LastError()));
					// The governor's tier decision: a GPU product whose lane ring did not fit is made in the render only.
					std::string shed;
					if (const std::shared_ptr<VpSkeleton::LaneState> ls = lane.State())
					{
						const VpSkeleton::ProductId gpuIds[] = { VpSkeleton::kP10GrainSplit, VpSkeleton::kP11RegionBlockStats,
							VpSkeleton::kP12LightLevel };
						for (VpSkeleton::ProductId id : gpuIds)
						{
							const VpSkeleton::TierEntry* e = ls->plan.Find(id);
							if (e != nullptr && e->why == VpSkeleton::GrantWhy::OverBudget && b->registry.Has(id))
								shed += std::string(shed.empty() ? "" : ", ") + b->registry.Get(id).name + " (T" +
									std::to_string(static_cast<unsigned>(e->tier)) + ")";
						}
					}
					addDecision(FallbackCatalog::SkeletonGpuShed(shed));
					// The dispatcher's own list already carries every kernel's (storage Compact/Full, P11 without bands, P12
					// frame-level, display facts: defaults, smoothing reset) and the products it could not allocate.
					for (const FallbackCatalog::Decision& d : gd->Fallbacks())
						addDecision(d);
				}
				if (const std::shared_ptr<VpSkeleton::LaneState> laneState = lane.State())
					addDecision(SkeletonModule::HistoryCapDecision(laneState->graph, b->configuredPeriodMs));
				full = lane.StatusLines();
				if (!st.laneRaise.empty() && !full.empty() && full[0] == "Analysis lane: on")
					full[0] += " (demand: " + st.laneRaise + "; analysis_lane = " +
						(st.configuredLaneMode == LaneSettings::Mode::Shadow ? "shadow" : "off") + " in the configuration, unchanged)";
				for (const std::string& memoLine : b->memo->StatusLines())
					full.push_back(memoLine);   // B1: reused / filled at read / unavailable (cause) per product asked
				// MV7 ledger (Ctrl+Alt+D): the VRAM users outside the product catalog, as the governor granted them (one line each; products are
				// listed by the lane's own status). A user below level 0 also has its governor notice in "Analysis: ..." (plan.notice).
				if (const std::shared_ptr<const VpSkeleton::GovernorPlan> usersPlan = lane.CurrentGovernorPlan())   // GF-P2b MUST-4 (2): the current users plan
					for (const VpSkeleton::UserGrant& g : usersPlan->grants)
						if (g.id < VpSkeleton::kUserProductBase)
							full.push_back(std::string("VRAM ") + VpSkeleton::Token(g.tier) + " " + g.name + ": " + VpSkeleton::FormatMB(g.bytes) + " level " +
								std::to_string(g.level) + (g.shed ? " (off)" : g.reduced ? " (reduced)" : ""));
				if (gd != nullptr)
				{
					// The measured device cost of the GPU products (every pass of a frame; P10 is five passes), written by the dispatcher
					// as the lane job runs; read-only, no behaviour depends on it.
					const VpSkeleton::ProductId costIds[] = { VpSkeleton::kP10GrainSplit, VpSkeleton::kP11RegionBlockStats, VpSkeleton::kP12LightLevel };
					const char* costNames[] = { "P10 grain split", "P11 region block stats", "P12 light level" };
					for (size_t i = 0; i < 3; ++i)
					{
						const std::string costLine = FormatCostLine(costNames[i], gd->CostOf(costIds[i]));
						if (!costLine.empty())
							full.push_back(costLine);
					}
				}
				line = std::string("analysis lane ") + mode + " " + Dimensions(b->dims) + ", " + std::to_string(st.frames) +
					" frames, ahead " + std::to_string(lane.TotalAhead()) + " in-render " + std::to_string(lane.TotalInRender()) +
					" declined " + std::to_string(lane.TotalDeclined());
				if (!lane.Active())
					line += " (lane unavailable)";
				if (st.periodMs > 0.0)
				{
					char period[40] = {};
					std::snprintf(period, sizeof(period), ", period %.2f ms", st.periodMs);
					line += period;
				}
				if (st.queueFed.wired & QueueShape::kLaneLead)
					line += ", lane lead " + std::to_string(unsigned(st.queueUsed.laneLead)) +
						(st.queueFed.laneLead != st.queueUsed.laneLead ? " (fed " + std::to_string(unsigned(st.queueFed.laneLead)) + ", capped)" : std::string());
			}
			else
				line = std::string("analysis lane ") + mode + ": " + (st.waiting.empty() ? std::string("starting") : st.waiting);
			if (storeRaised)
			{
				// 15c-c8: the store runs only for the demand (frame_analysis_store off in the configuration, unchanged).
				line += "; frame-analysis store on (demand: " + storeDemand + ")";
				full.push_back("Frame-analysis store: on (demand: " + storeDemand + "; frame_analysis_store = off in the configuration, unchanged)");
			}
			full.push_back(st.budgetFeed.StatusLine());   // S0: the spare slot budget, never silent (rule 5)
			{
				// Ctrl+I / Ctrl+Alt+D: the queue the lane is built for and the regulator's input, in plain words.
				const QueueShape& fq = st.queueFed;
				const QueueShape& uq = st.queueUsed;
				auto term = [](bool wired, unsigned fed, unsigned used) {
					if (!wired)
						return std::string("not fed");
					return fed == used ? std::to_string(used) : std::to_string(used) + " (fed " + std::to_string(fed) + ")";
				};
				full.push_back("Skeleton queue: render-ahead " + term((fq.wired & QueueShape::kRenderAhead) != 0, fq.renderAhead, uq.renderAhead) +
					", lane lead " + term((fq.wired & QueueShape::kLaneLead) != 0, fq.laneLead, uq.laneLead) + ", look-ahead walk " +
					term((fq.wired & QueueShape::kLookAhead) != 0, fq.lookAheadWalk, uq.lookAheadWalk) + "; queued ahead of output " +
					std::to_string(unsigned(uq.renderAhead) + unsigned(uq.laneLead)) + " frame(s); " + std::to_string(st.starveEvents) + " lead fall(s)");
				// P13-DECL: the VRAM reserve declared for the source ring and the temporal-detail store, per budget tier (shown, none taken).
				for (const std::string& rl : VpSkeleton::ReserveStatusLines(b ? b->dims : VpSkeleton::Dims()))
					full.push_back(rl);
				char regLine[512] = {};   // the fixed text alone is 237 characters: 240 cut off the " (none: shedding off)" suffix
				std::snprintf(regLine, sizeof(regLine), "Skeleton regulator input: derived from render time (observe only: the Begin..AfterRender span includes waits), %llu frame(s) observed (%llu skipped), last slack %.2f ms, span %.2f ms, %llu over the period; %llu measured frame(s) fed to the regulator%s",
					static_cast<unsigned long long>(st.timing.frames), static_cast<unsigned long long>(st.timing.skipped), st.timing.lastSlackMs,
					st.timing.lastRenderOnlyMs, static_cast<unsigned long long>(st.timing.missed), static_cast<unsigned long long>(st.timing.fed),
					st.timing.measured ? "" : " (none: shedding off)");
				full.push_back(regLine);
				char gateLine[256] = {};   // B4 S2
				std::snprintf(gateLine, sizeof(gateLine), "Skeleton regulator gate: %s; %llu host-measured render-only frame(s) noted, last %.2f ms, mean %.2f ms",
					GetRegulatorGate() == RegulatorGate::Act ? "act (the regulator sheds)" : "observe (recorded, not fed: nothing is shed)",
					static_cast<unsigned long long>(st.timing.noted), st.timing.lastNotedMs,
					st.timing.noted > 0 ? st.timing.notedMsSum / static_cast<double>(st.timing.noted) : 0.0);
				full.push_back(gateLine);
				if (st.feedNeeded || st.externalConsumers != 0)
				{
					char feedLine[640] = {};
					std::snprintf(feedLine, sizeof(feedLine),
						"Skeleton engine feed: %llu field(s) published (last frame %llu), %llu without a field, %llu slipped, %llu retried for missing inputs, %llu given up, %llu claim failure(s), %llu era reset(s); external demand: %llu consumer(s), %llu duplicate id(s), camera fit %s",
						static_cast<unsigned long long>(st.feed.published), static_cast<unsigned long long>(st.feed.lastSeq),
						static_cast<unsigned long long>(st.feed.noField), static_cast<unsigned long long>(st.feed.slip),
						static_cast<unsigned long long>(st.feed.ancestorsMissing), static_cast<unsigned long long>(st.feed.abandoned),
						static_cast<unsigned long long>(st.feed.claimFailed), static_cast<unsigned long long>(st.feed.eraResets),
						static_cast<unsigned long long>(st.externalConsumers), static_cast<unsigned long long>(st.externalDuplicates),
						st.wantsCameraFit ? "wanted" : "not wanted");
					full.push_back(feedLine);
					const std::string claimNote = ClaimFailedNote(st.feed.claimFailed, st.queueUsed.laneLead == 0);
					if (!claimNote.empty())
						full.push_back(claimNote);
				}
				{
					// U7 v2 (Ctrl+I / Ctrl+Alt+D): the scene product's sequence judge and the vector cut veto, in plain words.
					const std::string vetoState = vetoOff.empty() ? std::string("on") : "off (" + vetoOff + ")";
					char sceneLine[1024] = {};
					std::snprintf(sceneLine, sizeof(sceneLine),
						"Skeleton scene: %llu verdict(s) confirmed with the next frame, %llu provisional, %llu tracker restart(s), %llu group(s) confirmed later; %llu cut(s) delivered late (%llu more than one frame late); %llu no-grid fallback verdict(s) (v1 pair law); vector cut veto %s, %llu vector pair(s) refused at confirmed cuts, %llu asked before their frame's verdict (%llu of them into a cut confirmed later) (all renderers and all vector readers, P2 CPU form included; a gap read asks each frame of its span)",
						static_cast<unsigned long long>(st.sceneConfirmed.load(std::memory_order_relaxed)),
						static_cast<unsigned long long>(st.sceneProvisional.load(std::memory_order_relaxed)),
						static_cast<unsigned long long>(st.sceneReprimes.load(std::memory_order_relaxed)),
						static_cast<unsigned long long>(st.sceneRepublished.load(std::memory_order_relaxed)),
						static_cast<unsigned long long>(st.sceneLate.load(std::memory_order_relaxed)),
						static_cast<unsigned long long>(st.sceneLateGap.load(std::memory_order_relaxed)),
						static_cast<unsigned long long>(st.sceneNoGrid.load(std::memory_order_relaxed)),
						vetoState.c_str(),
						static_cast<unsigned long long>(MotionValidity::State::ConfirmedCutRefusalsAll().load(std::memory_order_relaxed)),
						static_cast<unsigned long long>(ScenePairsUnjudged().load(std::memory_order_relaxed)),
						static_cast<unsigned long long>(ScenePairsAcrossLateCut().load(std::memory_order_relaxed)));
					full.push_back(sceneLine);
				}
				for (const std::string& l : PassTimers::Inbox::Motion().Lines())
					full.push_back("Skeleton cost row " + l);
				if (PassTimers::Inbox::Motion().Posted() != 0 || PassTimers::Inbox::Motion().Dropped() != 0)
					full.push_back("Skeleton cost rows: " + std::to_string(PassTimers::Inbox::Motion().Posted()) + " posted, " +
						std::to_string(PassTimers::Inbox::Motion().Dropped()) + " dropped (the queue is bounded; with nobody draining it the oldest is dropped)");   // MV7: the producers' posted timings (also drained into the pass-timer readout by the host)
			}
			{
				const uint64_t rebuilt = st.sceneRunRebuilt.load(std::memory_order_relaxed);
				const uint64_t reduced = st.sceneRunReduced.load(std::memory_order_relaxed);
				if (rebuilt != 0 || reduced != 0)
					full.push_back("Skeleton scene: U7 fade run missing for " + std::to_string(rebuilt + reduced) + " frame(s): " +
						std::to_string(rebuilt) + " rebuilt exactly from older P4 records, " + std::to_string(reduced) +
						" with REDUCED evidence (the older records had left the lane ring; a slow fade there is found later)");
			}
			full.push_back("Skeleton: " + std::to_string(st.frames) + " frames, " + std::to_string(st.srcFrames.load(std::memory_order_relaxed)) +
				" source frames tapped, " + std::to_string(st.dropsApplied) + " drops applied, " + std::to_string(st.gapsRun) +
				" idle gaps run, " + std::to_string(st.gapsSkipped) + " skipped (frame period unknown), " + std::to_string(st.configures) +
				" graph configure(s)");
			for (const std::string& l : fb)
				full.push_back(l);
			std::string text;
			for (const std::string& l : full)
				text += (text.empty() ? "" : "\n") + l;

			std::lock_guard<std::mutex> lock(st.statusMutex);
			st.fallbackLines.swap(fb);
			st.oneLine.swap(line);
			st.fullText.swap(text);
		}

		void SkeletonModule::OnStatus(const RE::Context& ctx, RE::StatusKind kind, std::vector<std::string>& lines, const char* worker, const char*)
		{
			if (kind == RE::StatusKind::WorkerFault)
			{
				if (worker && std::strcmp(worker, kName) == 0)
					Faults().fetch_add(1, std::memory_order_relaxed);
				return;
			}
			if (kind == RE::StatusKind::WorkerOk)
				return;
			const std::shared_ptr<HostState> st = hosts_.Find(ctx.host);
			const bool laneShown = st && (st->active.load(std::memory_order_acquire) || st->reporting.load(std::memory_order_acquire));
			if (!laneShown || !st->active.load(std::memory_order_acquire))
			{
				// L-CORE s3 (no silent fallbacks): without a lane the renderer's memo still shows its "skeleton acquire <product>" entries.
				if (kind == RE::StatusKind::Fallbacks)
					if (const std::shared_ptr<VpSkeleton::SkeletonMemo> m = MemoHostOf(ctx.host, false))
						for (const FallbackCatalog::Decision& d : m->Decisions())
							lines.push_back(d.id + ": " + d.reason);
			}
			// QKP B (rule 5): a window term nobody feeds is shown, never a silent zero (the memo's window, lane or not).
			if (kind == RE::StatusKind::Fallbacks)
				if (const std::shared_ptr<VpSkeleton::SkeletonMemo> mw = MemoHostOf(ctx.host, false))
				{
					const VpSkeleton::QueueWindow w = mw->Window();
					if (w.wired != 0 && w.wired != VpSkeleton::QueueWindowTerms::kAll)
						lines.push_back("skeleton_queue_window: not fed: " + VpSkeleton::NotFed(w.wired) + " (those terms read 0 until delivered)");
				}
			// Audit S4: queue drops past the per-event cap that the memo did not discard (lane or not), shown, never silent.
			if (kind == RE::StatusKind::Fallbacks)
				if (const std::shared_ptr<MemoHost> mhd = memoHosts_.Find(ctx.host))
				{
					const uint64_t memoLost = mhd->dropsLost.load(std::memory_order_relaxed);
					if (memoLost != 0)
						lines.push_back("skeleton_memo: " + std::to_string(memoLost) + " dropped frame(s) past the per-event cap (" +
							std::to_string(kMaxDropsPerEvent) + ") were not discarded from the memo");
				}
			if (!st)
				return;
			const bool active = st->active.load(std::memory_order_acquire);
			if (!active && !st->reporting.load(std::memory_order_acquire))
				return;   // off: no lane line anywhere
			std::lock_guard<std::mutex> lock(st->statusMutex);
			if (kind == RE::StatusKind::Lines)
			{
				if (active && !st->oneLine.empty())
					lines.push_back(std::string(kLineKey) + "=" + st->oneLine);
			}
			else
			{
				for (const std::string& l : st->fallbackLines)
					lines.push_back(l);
			}
		}

		bool SkeletonModule::OnCommand(const RE::Context& ctx, const char* module, const char* command, std::string& reply)
		{
			if (!module || !command || std::strcmp(module, kName) != 0 || std::strcmp(command, "status") != 0)
				return false;
			const std::shared_ptr<HostState> st = hosts_.Find(ctx.host);
			if (!st || !st->active.load(std::memory_order_acquire))
			{
				reply = "skeleton: off (analysis_lane = off, or the Original pathway)";
				if (const std::shared_ptr<VpSkeleton::SkeletonMemo> m = MemoHostOf(ctx.host, false))
					for (const std::string& l : m->StatusLines())
						reply += "\n" + l;   // L-CORE: the memo works without the lane; its counters are shown
				return true;
			}
			std::lock_guard<std::mutex> lock(st->statusMutex);
			reply = st->fullText.empty() ? std::string("skeleton: starting") : st->fullText;
			return true;
		}

		void* SkeletonModule::Query(const char* iid)
		{
			if (iid && std::strcmp(iid, kExternalDemandQueryId) == 0)
				return static_cast<IExternalDemand*>(this);
			return nullptr;
		}

		void RegisterSkeletonModule()
		{
			static bool registered = false;
			if (registered)
				return;
			registered = true;
			Hub::ExtensionHub::Instance().AddModule(std::unique_ptr<Hub::ISkeletonModule>(new SkeletonModule()));
		}
	}
}
