// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only

#include <pch.h>
#include "SkeletonModule.h"

#include <DebugLog.h>

#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/modules/input/signal/ResyncExtension.h>
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
				  registry(gpuWiring.dispatcher != nullptr ? VpLaneProducts::TapFormProducts(VpSkeleton::SkeletonProducts(gpuWiring.compactGrain))
														   : VpSkeleton::SkeletonProducts()),
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
							job12->SetBeginGate([l](const VpSkeleton::LaneJobContext& job) {
								return VpLaneProducts::LightLevelBeginGate(job.seq, [l](uint64_t s) {
									switch (l->ReadOf(VpSkeleton::kP12LightLevel, s).state)
									{
									case AnalysisLane::ProductRead::Ring: return VpLaneProducts::LightPrevState::Ring;
									case AnalysisLane::ProductRead::Live: return VpLaneProducts::LightPrevState::Live;
									case AnalysisLane::ProductRead::Pending: return VpLaneProducts::LightPrevState::Pending;
									case AnalysisLane::ProductRead::Failed: return VpLaneProducts::LightPrevState::Failed;
									default: return VpLaneProducts::LightPrevState::None;
									}
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
				lane.RegisterProducer(slot.get());
			}

			Dims dims;
			uint64_t createdSeq;
			GpuWiring wiring;
			VpSkeleton::Registry registry;   // what the lane (and the dispatcher's Configure) are built on
			// Declared before the lane: they outlive its state (the runner points at them).
			VpLaneProducts::HalfLumaProducer p1;
			VpLaneProducts::SourceStatsProducer p4;
			std::unique_ptr<VpLaneProducts::GpuComputeLaneJob> job10, job11, job12;   // null unless the GPU gap is on
			AnalysisLane lane;
			VpSkeleton::LaneWorkerThreads threads;
			std::atomic<uint64_t> liveDeclined{ 0 };
			// Render thread.
			uint32_t configuredGen = 0;
			double configuredPeriodMs = 0.0;
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
			void ReleaseDispatcher(HostState& st)
			{
				if (!st.dispatcher)
					return;
				StopBundle(st);
				st.dispatcher->Release();
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
				ReleaseDispatcher(st);
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
				Teardown(st, false);
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
				Reconcile(f.host, f.code != 0 ? 1 : 0);   // code != 0: the Original pathway
				break;
			case RE::Lifecycle::GpuReleased:
			case RE::Lifecycle::Shutdown:
			{
				const std::shared_ptr<HostState> st = hosts_.Find(f.host);
				if (st)
					Teardown(st, true);
				else
					hosts_.Remove(f.host, true);
				gpuSlots_.Remove(f.host, true);   // the pl_gpu is going away: the dispatcher was released by Teardown above
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
				Teardown(st, false);
				hosts_.Remove(host, false);
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

		void SkeletonModule::Teardown(const std::shared_ptr<HostState>& st, bool releaseSlot)
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
			StopBundle(*st);
			ReleaseDispatcher(*st);   // 70b G5: the GPU textures go with the lane (GpuReleased / switched off)
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
			wiring.gapAhead = wiring.dispatcher != nullptr && st.settings.gpuGap == GpuGapMode::AfterRender;
			wiring.compactGrain = wiring.dispatcher != nullptr && st.dispatcherCompact;
			const std::shared_ptr<Bundle> current = std::atomic_load(&st.bundle);
			const bool rebuild = !current || !SameDims(current->dims, dims) || !current->wiring.Same(wiring);
			std::shared_ptr<Bundle> b = current;
			if (rebuild)
			{
				b = std::make_shared<Bundle>(dims, seq, nowUs_, wiring);
				b->lane.BindIndicator(seal->Current());
				b->lane.OnSealed(seq, 0);   // the shared seal already passed this frame before this lane existed
			}
			const uint32_t gen = st.settingsGen.load(std::memory_order_relaxed);
			const bool periodMoved = b->configuredPeriodMs > 0.0 && st.periodMs > 0.0 &&
				std::abs(st.periodMs - b->configuredPeriodMs) > 0.3 * b->configuredPeriodMs;
			const bool periodKnown = b->configuredPeriodMs <= 0.0 && st.periodMs > 0.0;
			if (rebuild || b->configuredGen != gen || ((periodMoved || periodKnown) && b->framesSinceConfigure >= kReconfigureMinFrames))
			{
				CardProfile card;
				std::string why;
				if (cardOverride_)
					card = card_;
				const bool known = cardOverride_ || QueryCard(hosts_.Aux(st.host), card, why);
				st.cardNote = known ? std::string() : why;
				TierInput tier;
				tier.framePeriodMs = st.periodMs;
				GraphSettings gs;
				gs.dims = dims;
				gs.renderAhead = 0;
				gs.laneLead = 1;
				const size_t capacity = resync->queueCapacity.load(std::memory_order_acquire);
				gs.queueDepth = static_cast<uint16_t>((std::max<size_t>)(4u, (std::min<size_t>)(capacity, 64u)));
				gs.lookAheadWalk = 0;
				gs.ofaPresent = false;
				gs.cardProfileKey = card.Identity();
				ConsumerDecl report;
				report.id = 1;
				report.name = "skeleton.report";
				const VpSkeleton::ProductId consumed[] = { VpSkeleton::kP1HalfLuma, VpSkeleton::kP4SourceStats, VpSkeleton::kP10GrainSplit,
					VpSkeleton::kP11RegionBlockStats, VpSkeleton::kP12LightLevel };
				for (VpSkeleton::ProductId p : consumed)
					VpSkeleton::AddConsume(report, p);
				const std::vector<ConsumerDecl> consumers{ report };
				b->lane.SetStormHold(Resync::ResyncExtension::StormHeld(st.host));
				// The dispatcher's private working memory (tap ring, grain-split working textures) is outside the product
				// declarations the governor plans: count it as used, and refuse the GPU products when it alone does not fit.
				if (wiring.dispatcher != nullptr)
				{
					const uint64_t priv = wiring.dispatcher->PrivateBytes(dims, wiring.gapAhead);
					if (known && priv > card.FreeVram())
					{
						st.gpuMemoryNote = "GPU products not made: private working memory " + std::to_string(priv >> 20) +
							" MiB exceeds the free video memory " + std::to_string(card.FreeVram() >> 20) + " MiB (" +
							Dimensions(dims) + ")";
					}
					card.vramUsageBytes += priv;
				}
				// The registry is the Bundle's: the tap form (TapFormProducts) when a GPU dispatcher is wired, else the plain one.
				b->lane.Configure(st.settings.lane, b->registry, consumers, gs, card, tier, seq, false);
				b->configuredGen = gen;
				b->configuredPeriodMs = st.periodMs;
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

			b->lane.SetStormHold(Resync::ResyncExtension::StormHeld(st.host));
			uint32_t wait = 0;
			for (int guard = 0; guard < 64 && (wait = b->lane.DeterministicWaitUs(seq, 0)) > 0; ++guard)
				std::this_thread::sleep_for(std::chrono::microseconds(wait));   // N1: only a deterministic run or an explicit test wait
			b->lane.OnRenderStart(seq, 0);
			Refresh(st);
		}

		void SkeletonModule::AfterRender(HostState& st, int64_t seqIn)
		{
			const std::shared_ptr<Bundle> b = std::atomic_load(&st.bundle);
			if (!b)
				return;
			const uint64_t seq = static_cast<uint64_t>(seqIn);
			if (st.dispatcher && b->wiring.dispatcher == st.dispatcher.get())
			{
				// GPU-legal (before the flush and the gap): destroy what a size / graph change queued, then (re)allocate the
				// product textures for the lane's current graph (a no-op while nothing changed). Never at Begin (no render
				// lock there). A refused product shows through the dispatcher's Fallbacks().
				st.dispatcher->FlushReleases();
				if (const std::shared_ptr<VpSkeleton::LaneState> ls = b->lane.State())
					if (ls->graph.valid)
						st.dispatcher->Configure(ls->graph, b->registry, ls->plan, b->dims, b->configuredGen);
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
			const bool due = st.frames <= kEarlyRefreshFrames || st.frames % kRefreshEvery == 1 || st.waiting != st.shownWaiting ||
				hasBundle != st.shownBundle;
			if (!due)
				return;
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
				full = lane.StatusLines();
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
			}
			else
				line = std::string("analysis lane ") + mode + ": " + (st.waiting.empty() ? std::string("starting") : st.waiting);
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
