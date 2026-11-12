// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only

#include <pch.h>
#include "SkeletonModule.h"

#include <DebugLog.h>

#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/core/GpuAllocator.h>
#include <VP-V/core/PassTimerInbox.h>
#include <VP-V/core/ReserveRows.h>
#include <VP-V/core/VramUsers.h>
#include <VP-V/modules/motion/estimation/VectorEngineInterface.h>
#include <VP-V/modules/input/signal/ResyncExtension.h>
#include <VP-V/core/FrameAnalysisStore.h>
#include <VP-V/modules/input/source_analysis/SourceFrameTap.h>
#include <VP-V/modules/control/skeleton/gpu/SkeletonGpuDispatcher.h>
#include <VP-V/modules/input/source_analysis/lane/LaneCpuProducts.h>
#include <VP-V/modules/input/source_analysis/lane/LaneGpuGrainKernel.h>
#include <VP-V/modules/input/source_analysis/lane/LaneGpuJobs.h>
#include <VP-V/modules/input/source_analysis/lane/LaneGpuLightKernel.h>
#include <VP-V/modules/input/source_analysis/lane/LaneGpuRegionKernel.h>

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
			constexpr double kLeadWindowSeconds = 0.5;   // source time the lane-lead feed keeps its lowest value over
			constexpr double kLeadPeakHoldSeconds = 10.0;   // the lane lead's best value is held this long, then decays to the lead in use
			constexpr size_t kFactsRing = 64;           // engine-field facts kept (frames)
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
			Bundle(Dims d, uint64_t seq, const std::function<uint64_t()>& clock, const GpuWiring& gpuWiring)
				: dims(d), createdSeq(seq), wiring(gpuWiring),
				  // Without a GPU the registry is exactly the one the lane always used; with one, the tap form (P10 / P11 read
				  // the tap plane, not the P13 ring that is RenderOnly and never filled: without this P10 is InputMissing).
				  registry(MakeRegistry(gpuWiring, 0.0, 0)),
				  p1(clock), p4(d, clock), lane(std::shared_ptr<VpIndicator::Indicator>(), clock), threads(lane)
			{
				lane.SetExternalSeal(true);   // FrameSeal owns the one seal
				lane.RegisterProducer(&p1);
				lane.RegisterProducer(&p4);
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
			}

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
			AnalysisLane lane;
			VpSkeleton::LaneWorkerThreads threads;
			std::atomic<uint64_t> liveDeclined{ 0 };
			// Render thread.
			uint32_t configuredGen = 0;
			double configuredPeriodMs = 0.0;
			SkeletonModule::QueueShape configuredQueue;
			uint64_t configuredVramVersion = 0;   // VramUsers::Version() at the last Configure
			uint64_t configuredExtVersion = 0;     // externalVersion_ at the last Configure
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
			uint8_t shownQueueWired = 0xFF;   // the queue-delivery state the cached status text was built for
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
			bool feedNeeded = false;             // the graph holds P2m / P3 for an external consumer: the engine feed runs
			bool fitWantedPushed = false;        // IVectorEngine::SetCameraFitWanted(true) was sent
			VpMotion::IVectorEngine* engine = nullptr;   // this renderer's engine; dropped at Teardown (valid until the renderer goes)
			FeedCounters feed;
			// The scene product's counters (W1-C7): written by the render thread (seal) and by any SceneOf caller, so atomic and relaxed.
			std::atomic<uint64_t> scenePublished{ 0 }, sceneAlready{ 0 }, sceneNotReady{ 0 }, sceneStoreReads{ 0 }, sceneLiveFills{ 0 }, sceneCacheHits{ 0 }, sceneUnknown{ 0 };
			// The live classification of ONE frame, kept until the next Begin (a frame number can restart after a seek, so nothing outlives a frame).
			std::mutex sceneMutex;
			uint64_t sceneCacheSeq = 0;
			bool sceneCacheValid = false;
			bool sceneCacheCut = false, sceneCacheFade = false;
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
					old->threads.Stop();   // joined here, before anything the threads read goes away
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
				const bool want = st.settings.lane.mode == LaneSettings::Mode::On && st.gpu != nullptr;
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
		SkeletonModule::SkeletonModule(Skeleton::ConfigLoader loader, std::function<uint64_t()> nowUs)
			: loader_(std::move(loader)), nowUs_(nowUs ? std::move(nowUs) : std::function<uint64_t()>(SteadyUs))
		{
		}

		SkeletonModule::~SkeletonModule()
		{
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
		unsigned long long SkeletonModule::GpuDispatchersCreated() { return DispatchersCreated().load(std::memory_order_relaxed); }

		// ---- lifecycle (render thread) --------------------------------------------------------------
		void SkeletonModule::OnLifecycle(RE::LifecycleFacts& f)
		{
			if (!f.host)
				return;
			switch (f.kind)
			{
			case RE::Lifecycle::GpuCreated:
				hosts_.SetAux(f.host, f.device);   // the raw device, for the video memory query only
				gpuSlots_.SetAux(f.host, const_cast<void*>(static_cast<const void*>(f.gpu)));   // 70b G5: the renderer's pl_gpu (null in a unit test: no GPU products)
				Reconcile(f.host, -1);
				break;
			case RE::Lifecycle::SettingsApplied:
				PublishLookAheadWalk(f.value);   // the host passes the motion look-ahead depth (frames) here: the one queue term it delivers
				Reconcile(f.host, f.code != 0 ? 1 : 0);   // code != 0: the Original pathway
				break;
			case RE::Lifecycle::QueueDepth:
			{
				// The render-ahead term (the host's one generic call at the render-ahead plan block, every planned frame): frames the plan keeps ahead
				// of the present. Only while this renderer's lane is on; PublishRenderAhead is change-only (a compare-and-swap that leaves the other terms).
				const std::shared_ptr<HostState> st = hosts_.Find(f.host);
				if (st && st->active.load(std::memory_order_acquire))
				{
					if (f.value >= 0.0)   // NaN and negative: the host has no value (no source period): unwired, never an old number
					{
						PublishRenderAhead(f.value);
						st->lastQueueDepthUs.store(nowUs_(), std::memory_order_relaxed);
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
				EraseExternal(f.host);   // a renderer recreated at this address never inherits the old one's external demand
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
				hosts_.Remove(host, false);
				EraseExternal(host);          // nor is its external demand (declared for the dead renderer)
				st.reset();
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
					seal->AddProvider(kName, [weak](const std::shared_ptr<VpIndicator::Indicator>& ind) {
						if (const std::shared_ptr<HostState> s2 = weak.lock())
							if (const std::shared_ptr<Bundle> b = std::atomic_load(&s2->bundle))
								b->lane.BindIndicator(ind);
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
			std::lock_guard<std::mutex> lock(externalMutex_);
			for (ExternalEntry& e : external_)
				if (e.host == host && e.owner == owner)
				{
					e.consumers = consumers;
					e.products = publishedOutside;
					externalVersion_.fetch_add(1, std::memory_order_release);
					return;
				}
			ExternalEntry e;
			e.host = host;
			e.owner = owner;
			e.consumers = consumers;
			e.products = publishedOutside;
			external_.push_back(std::move(e));
			externalVersion_.fetch_add(1, std::memory_order_release);
		}

		size_t SkeletonModule::EraseExternal(const void* host)
		{
			std::lock_guard<std::mutex> lock(externalMutex_);
			size_t erased = 0;
			for (size_t i = 0; i < external_.size();)
			{
				if (external_[i].host == host)
				{
					external_.erase(external_.begin() + static_cast<std::ptrdiff_t>(i));
					++erased;
				}
				else
					++i;
			}
			if (erased != 0)
				externalVersion_.fetch_add(1, std::memory_order_release);
			return erased;
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
					external_.erase(external_.begin() + static_cast<std::ptrdiff_t>(i));
					externalVersion_.fetch_add(1, std::memory_order_release);
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

		// The scene of `seq` against `seq - 1`: the two P4 records, classified by the shared unit. This is the classification only; the SCENE PRODUCT
		// (SceneOf / FillScene below) stores its cut / fade in the frame-analysis store's scene group once per frame and serves readers from there.
		bool SkeletonModule::SceneFactsOf(const void* host, uint64_t seq, SceneFacts& out) const
		{
			out = SceneFacts();
			const std::shared_ptr<HostState> st = hosts_.Find(host);
			const std::shared_ptr<Bundle> b = st ? std::atomic_load(&st->bundle) : nullptr;
			if (!b || seq < 2)
				return false;
			const AnalysisLane::ProductRead cur = b->lane.ReadOf(VpSkeleton::kP4SourceStats, seq);
			const AnalysisLane::ProductRead prev = b->lane.ReadOf(VpSkeleton::kP4SourceStats, seq - 1);
			VpLaneProducts::SourceStatsRecord a, c;
			if (cur.cpu == nullptr || prev.cpu == nullptr || !VpLaneProducts::ReadSourceStats(prev.cpu, prev.cpuBytes, &a) ||
				!VpLaneProducts::ReadSourceStats(cur.cpu, cur.cpuBytes, &c))
				return false;
			// Scene EVENTS (cut / fade / flash) are scene facts: classified from the two P4 records alone. This function reads NO validity source
			// (no VpIndicator::TemporalValid, no Indicator at all): whether a vector pair is usable across a resync is answered only by the P5a
			// MotionValidity facade (coordinator ruling 2026-11-08; tools/test_check_shared_units.py test_scene_facts_reads_no_validity_source).
			const VpLaneProducts::EventSignals e = VpLaneProducts::ClassifyEvents(a, c, false, false);
			out.valid = true;
			out.cut = e.cut;
			out.cutPossible = e.cutPossible;
			out.fade = e.fade;
			out.flash = e.flash;
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

		// The scene product's one fill: classify from the two P4 records (SceneFactsOf), write the store's scene group when the store holds the
		// frame's record. The same body serves the seal (Begin) and a reader that arrives before the seal / with no record.
		bool SkeletonModule::FillScene(HostState& st, uint64_t seq, SceneAnswer* answer) const
		{
			bool cut = false, fade = false;
			bool cached = false;
			{
				std::lock_guard<std::mutex> lock(st.sceneMutex);
				if (st.sceneCacheValid && st.sceneCacheSeq == seq)
				{
					cut = st.sceneCacheCut;
					fade = st.sceneCacheFade;
					cached = true;
				}
			}
			if (!cached)
			{
				SceneFacts f;
				if (!SceneFactsOf(st.host, seq, f) || !f.valid)
					return false;
				cut = f.cut;
				fade = f.fade;
				std::lock_guard<std::mutex> lock(st.sceneMutex);
				st.sceneCacheSeq = seq;
				st.sceneCacheValid = true;
				st.sceneCacheCut = cut;
				st.sceneCacheFade = fade;
			}
			if (answer)
			{
				answer->known = true;
				answer->cut = cut;
				answer->fade = fade;
			}
			if (cached)
				st.sceneCacheHits.fetch_add(1, std::memory_order_relaxed);
			// A store that is registered but OFF (frame_analysis_store=false: the Host always registers it, disabled) takes nothing.
			// Peek-then-publish is not atomic, but the skeleton is the only scene producer today (a second one could at worst be overwritten
			// with the same classification), so no compare-and-set is spent on it.
			if (FrameAnalysis::Store* store = VpFrameAnalysis::SourceFrameTap::StoreFor(st.host))
				if (store->Enabled())
				{
					FrameAnalysis::Scene scene;
					scene.cut = cut;
					scene.fade = fade;
					scene.origin = FrameAnalysis::SceneOrigin::Vp;
					if (store->PublishScene(seq, scene))
						st.scenePublished.fetch_add(1, std::memory_order_relaxed);
				}
			return true;
		}

		SceneAnswer SkeletonModule::SceneOf(const void* host, uint64_t seq) const
		{
			SceneAnswer a;
			const std::shared_ptr<HostState> st = hosts_.Find(host);
			if (!st)
				return a;
			if (FrameAnalysis::Store* store = VpFrameAnalysis::SourceFrameTap::StoreFor(host))
			{
				FrameAnalysis::Scene scene;
				if (store->PeekScene(seq, scene))
				{
					a.known = true;
					a.cut = scene.cut;
					a.fade = scene.fade;
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
				c.unknown = st->sceneUnknown.load(std::memory_order_relaxed);
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
				}
				return false;
			}
			if (c.phase != RE::FramePhase::Begin && c.phase != RE::FramePhase::AfterRender)
				return false;
			const std::shared_ptr<HostState> st = hosts_.Find(c.host);
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

		// The input-queue term of the lane lead. CALL CONTRACT: any thread, any rate (the host calls it once per frame; calls in bursts or several
		// per frame are fine): every call is one timestamped sample of the queue size, and the published value is the LOWEST size among the samples of
		// the last kLeadWindowSeconds of real time (a fall is taken at once, a rise only when the whole window is above it). A host that stops calling
		// leaves the last value published; the lane lead then ages out through the starve peak, not by itself. PublishLaneLead is a
		// compare-and-swap that leaves the other terms alone and does nothing when the value did not change.
		void SkeletonModule::OnCollectAhead(HostState& st, const RE::QueueView& view)
		{
			const uint8_t size = view.size > 255u ? uint8_t(255) : uint8_t(view.size);
			const uint64_t now = nowUs_();
			const uint64_t windowUs = static_cast<uint64_t>(kLeadWindowSeconds * 1e6);
			uint8_t lowest = size;
			{
				std::lock_guard<std::mutex> lock(st.leadMutex);
				st.leadWindow.push_back(Vpv::Skeleton::HostState::LeadSample{ now, size });
				while (st.leadWindow.size() > 1 && st.leadWindow.front().us + windowUs < now)
					st.leadWindow.pop_front();
				for (const Vpv::Skeleton::HostState::LeadSample& s : st.leadWindow)
					lowest = (std::min)(lowest, s.size);
			}
			PublishLaneLead(double(lowest));
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
				b = std::make_shared<Bundle>(dims, seq, nowUs_, wiring);
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
			if (rebuild || b->configuredGen != gen || queueMoved || vramMoved || extMoved || ((periodMoved || periodKnown) && b->framesSinceConfigure >= kReconfigureMinFrames))
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
				for (VpSkeleton::ProductId p : consumed)
					VpSkeleton::AddConsume(report, p);
				std::vector<ConsumerDecl> consumers{ report };
				// SK-W stage D: the consumers other components declared (the camera link): their products and ancestors join the graph.
				std::vector<VpSkeleton::ProductId> outside;
				size_t duplicates = 0;
				bool wantsFit = false;
				CollectExternal(st.host, consumers, outside, wantsFit, duplicates);
				st.externalConsumers = consumers.size() - 1;
				st.externalDuplicates = duplicates;
				st.wantsCameraFit = wantsFit;
				st.feedNeeded = st.externalConsumers != 0;
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
				{
					// What the declared users still hold above their grant: planned as transient at the next Configure (once).
					uint64_t shortfall = 0;
					if (const std::shared_ptr<VpSkeleton::LaneState> ls = b->lane.State())
						for (const std::pair<uint16_t, uint64_t>& h : declared.heldById)
							if (const VpSkeleton::UserGrant* g = ls->plan.governor.Find(h.first))
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
					if (sceneStore->PeekScene(seq, already))
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
			const bool due = st.frames <= kEarlyRefreshFrames || st.frames % kRefreshEvery == 1 || st.waiting != st.shownWaiting ||
				hasBundle != st.shownBundle || queueWired != st.shownQueueWired || queueKey != st.shownQueueKey;
			if (!due)
				return;
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
				addDecision(FallbackCatalog::SkeletonRegulatorInput(hasBundle && !st.timing.measured));
addDecision(FallbackCatalog::SkeletonVramKnown(hasBundle ? std::string(VpSkeleton::KnownUndeclaredVramUsers()) : std::string()));   // MV7 standing gap
				addDecision(FallbackCatalog::SkeletonEngineFeed(st.feedNeeded && st.engine == nullptr));   // SK-W stage D: P2m / P3 cannot be published
				// MV7 guard: a module that holds video memory is active but declared none of it: visible, never silent.
				addDecision(FallbackCatalog::SkeletonVramUsers(VpSkeleton::UnpopulatedUserNames(VpSkeleton::VramUsers::Instance().Take().unpopulated)));
			}
			// The GPU products (70b G5): shadow and "no render device" have no dispatcher and say so.
			const SkeletonGpuDispatcher* gd = st.dispatcher.get();
			const bool shadow = st.settings.lane.mode == LaneSettings::Mode::Shadow;
			addDecision(FallbackCatalog::SkeletonGpuShadow(shadow));
			addDecision(FallbackCatalog::SkeletonGpuDevice(!shadow && gd == nullptr));
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
			const char* mode = st.settings.lane.mode == LaneSettings::Mode::Shadow ? "shadow" : "on";
			if (b)
			{
				const AnalysisLane& lane = b->lane;
				for (const FallbackCatalog::Decision& d : lane.Fallbacks())
					addDecision(d);
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
				// MV7 ledger (Ctrl+Alt+D): the VRAM users outside the product catalog, as the governor granted them (one line each; products are
				// listed by the lane's own status). A user below level 0 also has its governor notice in "Analysis: ..." (plan.notice).
				if (const std::shared_ptr<VpSkeleton::LaneState> laneState = lane.State())
					for (const VpSkeleton::UserGrant& g : laneState->plan.governor.grants)
						if (g.id < VpSkeleton::kUserProductBase)
							full.push_back(std::string("VRAM ") + VpSkeleton::Token(g.tier) + " " + g.name + ": " + VpSkeleton::FormatMB(g.bytes) + " level " +
								std::to_string(g.level) + (g.shed ? " (off)" : g.reduced ? " (reduced)" : ""));
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
				for (const std::string& l : PassTimers::Inbox::Motion().Lines())
					full.push_back("Skeleton cost row " + l);
				if (PassTimers::Inbox::Motion().Posted() != 0 || PassTimers::Inbox::Motion().Dropped() != 0)
					full.push_back("Skeleton cost rows: " + std::to_string(PassTimers::Inbox::Motion().Posted()) + " posted, " +
						std::to_string(PassTimers::Inbox::Motion().Dropped()) + " dropped (the queue is bounded; with nobody draining it the oldest is dropped)");   // MV7: the producers' posted timings (also drained into the pass-timer readout by the host)
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
			if (!st)
				return;
			const bool active = st->active.load(std::memory_order_acquire);
			if (!active && !st->reporting.load(std::memory_order_acquire))
				return;   // off: no line anywhere
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
