// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "ExtensionHub.h"
#include "CurrentFrame.h"
#include "HubQuery.h"
#include <VP-V/core/ComputeOnce.h>
#include <VP-V/modules/motion/consumers/DejudderAhead.h>
#include <VP-V/segments/CoreSegmentCaches.h>

#include <atomic>
#include <thread>
#include <utility>
#include <vector>

namespace Vpv
{
	namespace Hub
	{
		namespace
		{
			// The registry owns a forwarding wrapper; the real hub lives for the whole process.
			class Forwarder : public RE::IRendererExtension
			{
			public:
				void OnLifecycle(RE::LifecycleFacts& f) override { ExtensionHub::Instance().OnLifecycle(f); }
				void OnSettingsLoad(RE::SettingsLoadContext& c) override { ExtensionHub::Instance().OnSettingsLoad(c); }
				void OnIngress(RE::IngressContext& c) override { ExtensionHub::Instance().OnIngress(c); }
				void OnQueue(const RE::QueueFacts& f) override { ExtensionHub::Instance().OnQueue(f); }
				bool OnFrame(RE::FrameContext& c) override { return ExtensionHub::Instance().OnFrame(c); }
				double OnResolve(RE::Resolve k, const RE::Context& c, double v) override { return ExtensionHub::Instance().OnResolve(k, c, v); }
				void OnStatus(const RE::Context& c, RE::StatusKind k, std::vector<std::string>& l, const char* w, const char* x) override
				{
					ExtensionHub::Instance().OnStatus(c, k, l, w, x);
				}
				bool OnCommand(const RE::Context& c, const char* m, const char* cmd, std::string& r) override
				{
					return ExtensionHub::Instance().OnCommand(c, m, cmd, r);
				}
				void* Query(const char* iid) override { return ExtensionHub::Instance().Query(iid); }
			};

			// HubQuery.h: code without the hub header (VideoProcessor-Lib) reaches this hub's Query through the slot. Installed at
			// static initialisation of this object file, which is compiled into every binary that has the hub.
			void* QueryThisHub(const char* iid) { return ExtensionHub::Instance().Query(iid); }
			const bool kQuerySlotInstalled = (QuerySlot().store(&QueryThisHub, std::memory_order_release), true);
		}

		// A throwing module never stops the modules after it; the fault is reported with its name.
		static void Contain(const RE::Context& ctx, const char* module) noexcept
		{
			try
			{
				std::vector<std::string> none;
				ExtensionHub::Instance().OnStatus(ctx, RE::StatusKind::WorkerFault, none, module ? module : "module", "exception");
			}
			catch (...) {}
		}

		ExtensionHub& ExtensionHub::Instance()
		{
			static ExtensionHub hub;
			return hub;
		}

		void ExtensionHub::AddModule(std::unique_ptr<ISkeletonModule> module)
		{
			if (module) modules_.push_back(std::move(module));
		}

		void ExtensionHub::ResetModulesForTesting()
		{
			modules_.clear();
			{
				std::lock_guard<std::mutex> lock(hostsMutex_);
				hosts_.clear();
			}
			std::vector<const void*> keys;
			pathway_.ForEach([&](const void* key, const std::shared_ptr<PathwayFact>&) { keys.push_back(key); });
			for (const void* key : keys)
				pathway_.Remove(key, true);
			pathwayRefusals_.store(0, std::memory_order_relaxed);
		}

		bool ExtensionHub::OriginalPathway(const void* host) const
		{
			const std::shared_ptr<PathwayFact> fact = pathway_.Find(host);
			return fact && fact->original.load(std::memory_order_acquire);
		}

		bool ExtensionHub::NoteOriginalPathway(const void* host, bool original)
		{
			if (!host)
				return false;
			if (const std::shared_ptr<PathwayFact> fact = pathway_.Find(host))
			{
				fact->original.store(original, std::memory_order_release);
				return true;
			}
			std::shared_ptr<PathwayFact> fact = std::make_shared<PathwayFact>();
			fact->host = host;
			fact->original.store(original, std::memory_order_release);
			// False only when the 16-host slot table is full (hosts leave it at GpuReleased / Shutdown, so one
			// renderer never fills it). The hub has no log sink of its own (DebugLog.h brings Windows.h min/max
			// into this translation unit); the caller reads the result and the gate tests pin it to true.
			return pathway_.Put(host, fact);
		}

		size_t ExtensionHub::LiveHostCount() const
		{
			std::lock_guard<std::mutex> lock(hostsMutex_);
			return hosts_.size();
		}

		bool ExtensionHub::HasHost(const void* host) const
		{
			std::lock_guard<std::mutex> lock(hostsMutex_);
			return hosts_.find(host) != hosts_.end();
		}

		bool ExtensionHub::NoteFixSwitches(const void* host, uint32_t bits)
		{
			if (!host)
				return false;
			std::shared_ptr<PathwayFact> fact = pathway_.Find(host);
			if (!fact)
			{
				fact = std::make_shared<PathwayFact>();
				fact->host = host;
				// False = the 16-host slot table is full. Writers are the render thread only (UpdatePipelineSlots and the
				// SettingsApplied fan-out, serialised), so there is no racing Put. Counted (PathwayRefusals) and surfaced by the
				// caller (CoreSegments.inl EngageFallback); the capture / file threads then keep their switches as built.
				if (!pathway_.Put(host, fact))
				{
					pathwayRefusals_.fetch_add(1, std::memory_order_relaxed);
					return false;
				}
			}
			fact->fixBits.store(bits, std::memory_order_relaxed);
			fact->fixPublished.store(true, std::memory_order_release);
			return true;
		}

		bool ExtensionHub::FixSwitchBits(const void* host, uint32_t& bits) const
		{
			const std::shared_ptr<PathwayFact> fact = pathway_.Find(host);
			if (!fact || !fact->fixPublished.load(std::memory_order_acquire))
				return false;
			bits = fact->fixBits.load(std::memory_order_relaxed);
			return true;
		}

		void ExtensionHub::OnLifecycle(RE::LifecycleFacts& f)
		{
			if (f.kind == RE::Lifecycle::GpuCreated && f.host)
			{
				Vpv::Segments::CoreSegmentCaches::Instance().OnGpuCreated(f.host, f.gpu);
				std::lock_guard<std::mutex> lock(hostsMutex_);
				HostState& state = hosts_[f.host];
				state.host = f.host;
				state.hostView = f.hostView;
				state.gpuLive = true;
			}
			for (auto& m : modules_) { try { m->OnLifecycle(f); } catch (...) { Contain(f, m->Name()); } }
			// PHASE 1: the pathway fact, after the fan-out (a module's SettingsApplied handler compares the old fact
			// with the new f.code).
			if (f.kind == RE::Lifecycle::SettingsApplied && f.host && !NoteOriginalPathway(f.host, f.code != 0))
			{
				// T-AR3-N4: an unrecorded fact reads as "VP-V" for every gated call (the safe direction for a missed
				// Original switch is NOT this one), so it is counted and surfaced, never silent.
				pathwayRefusals_.fetch_add(1, std::memory_order_relaxed);
				try
				{
					std::vector<std::string> none;
					OnStatus(f, RE::StatusKind::WorkerFault, none, "hub", "pathway table full: the Original-pathway gate was not recorded for this renderer");
				}
				catch (...) {}
			}
			// Released only for this host: a replacement renderer's state is untouched.
			if ((f.kind == RE::Lifecycle::GpuReleased || f.kind == RE::Lifecycle::Shutdown) && f.host)
			{
				Vpv::Segments::CoreSegmentCaches::Instance().OnGpuReleased(f.host);
				{
					std::lock_guard<std::mutex> lock(hostsMutex_);
					hosts_.erase(f.host);
				}
				pathway_.Remove(f.host, true);
				ForgetCurrentFrame(f.host);   // L-CORE: a renderer recreated at this address starts with no frame
			}
		}

		void ExtensionHub::OnSettingsLoad(RE::SettingsLoadContext& c) { for (auto& m : modules_) { try { m->OnSettingsLoad(c); } catch (...) { Contain(c, m->Name()); } } }
		void ExtensionHub::OnIngress(RE::IngressContext& c) { for (auto& m : modules_) { try { m->OnIngress(c); } catch (...) { Contain(c, m->Name()); } } }
		void ExtensionHub::OnQueue(const RE::QueueFacts& f) { for (auto& m : modules_) { try { m->OnQueue(f); } catch (...) { Contain(f, m->Name()); } } }

		// R12: the module sees an empty outcome and writes only its own contribution; it is merged into what the earlier
		// modules (and the caller) left (OR / max / sum). A throw discards the module's partial writes.
		static void AfterPresentMerged(ISkeletonModule& m, RE::FrameContext& c, RE::PresentContext& p)
		{
			const RE::PresentOutcome before = p.outcome;
			p.outcome = RE::PresentOutcome();
			try { m.OnAfterPresent(c, p); }
			catch (...) { p.outcome = before; throw; }
			const RE::PresentOutcome own = p.outcome;
			p.outcome = before;
			p.outcome.extrasRan = before.extrasRan || own.extrasRan;
			p.outcome.committed = before.committed || own.committed;
			if (own.targetLeadMs > before.targetLeadMs) p.outcome.targetLeadMs = own.targetLeadMs;
			p.outcome.raiseTargetLead = before.raiseTargetLead || own.raiseTargetLead;
			if (own.estimatedLeadMs > before.estimatedLeadMs) p.outcome.estimatedLeadMs = own.estimatedLeadMs;
			p.outcome.spentSeconds = before.spentSeconds + own.spentSeconds;
		}

		// One module's share of a phase: the M2 P0b phases go to their typed method (null payload = skipped), the
		// earlier phases to OnFrame as before. Fan-out phases return false (the hub ORs nothing for them).
		static bool DispatchPhase(ISkeletonModule& m, RE::FrameContext& c)
		{
			switch (c.phase)
			{
			case RE::FramePhase::PlanOutputs:   if (auto* p = RE::PayloadOf<RE::OutputPlan>(c)) return m.OnPlanOutputs(c, *p); return false;
			case RE::FramePhase::PlanPresent:   if (auto* p = RE::PayloadOf<RE::PresentPlan>(c)) return m.OnPlanPresent(c, *p); return false;
			case RE::FramePhase::AfterPresent:  if (auto* p = RE::PayloadOf<RE::PresentContext>(c)) AfterPresentMerged(m, c, *p); return false;
			case RE::FramePhase::PresentSample: if (auto* p = RE::PayloadOf<RE::PresentSampleView>(c)) m.OnPresentSample(c, *p); return false;
			case RE::FramePhase::ResolveTime:   if (auto* p = RE::PayloadOf<RE::TimeQuery>(c)) m.OnResolveTime(c, *p); return false;
			case RE::FramePhase::DecideIngress: if (auto* p = RE::PayloadOf<RE::IngressContext>(c)) return m.OnDecideIngress(c, *p); return false;
			case RE::FramePhase::Convert:       if (auto* p = RE::PayloadOf<RE::ConvertContext>(c)) return m.OnConvertedFrame(c, *p); return false;
			case RE::FramePhase::AfterPublish:  if (auto* p = RE::PayloadOf<RE::PublishView>(c)) m.OnAfterPublish(c, *p); return false;
			case RE::FramePhase::Upload:        if (auto* p = RE::PayloadOf<RE::UploadContext>(c)) return m.OnTryUpload(c, *p); return false;
			case RE::FramePhase::QueueTarget:   if (auto* p = RE::PayloadOf<RE::QueueTargetContext>(c)) m.OnQueueTarget(c, *p); return false;
			default:                            return m.OnFrame(c);
			}
		}

		// ---- L-CORE: the current render frame (CurrentFrame.h) ----------------------------------------------------------
		namespace
		{
			// One host's record. Written by its render thread only; read by any thread through a sequence word (odd = writing).
			struct FrameFact
			{
				explicit FrameFact(const void* h) : host(h) {}
				const void* host;
				std::atomic<uint64_t> version{ 0 };
				std::atomic<int64_t> seq{ -1 }, nextSeq{ -1 }, generation{ 0 };
				std::atomic<int> phase{ 0 };
				std::atomic<uint64_t> serial{ 0 };
			};
			HostSlots<FrameFact>& Frames()
			{
				static HostSlots<FrameFact> frames;
				return frames;
			}
			CurrentFrame& Sticky()
			{
				static thread_local CurrentFrame frame;
				return frame;
			}
		}

		CurrentFrame ThisThreadFrame() { return Sticky(); }

		CurrentFrame CurrentFrameOf(const void* host)
		{
			CurrentFrame out;
			const std::shared_ptr<FrameFact> f = Frames().Find(host);
			if (!f)
				return out;
			for (int attempt = 0; attempt < 64; ++attempt)
			{
				const uint64_t v0 = f->version.load(std::memory_order_acquire);
				if (v0 & 1u)
				{
					std::this_thread::yield();
					continue;
				}
				out.host = host;
				out.seq = f->seq.load(std::memory_order_relaxed);
				out.nextSeq = f->nextSeq.load(std::memory_order_relaxed);
				out.generation = f->generation.load(std::memory_order_relaxed);
				out.phase = static_cast<RE::FramePhase>(f->phase.load(std::memory_order_relaxed));
				out.serial = f->serial.load(std::memory_order_relaxed);
				std::atomic_thread_fence(std::memory_order_acquire);
				if (f->version.load(std::memory_order_relaxed) == v0)
					return out;
			}
			return CurrentFrame();   // a writer kept the record busy: no frame rather than a torn one
		}

		void RecordCurrentFrame(const RE::FrameContext& c)
		{
			if (!c.host || c.seq < 0 || !PhaseRecordsCurrentFrame(c.phase))
				return;
			CurrentFrame& t = Sticky();
			const bool begin = c.phase == RE::FramePhase::Begin;
			const bool sameHost = t.host == c.host;
			std::shared_ptr<FrameFact> f = Frames().Find(c.host);
			if (!f)
			{
				f = std::make_shared<FrameFact>(c.host);
				if (!Frames().Put(c.host, f))
					f = Frames().Find(c.host);   // another thread put it first, or the table is full (null: thread record only)
			}
			uint64_t serial = sameHost ? t.serial : (f ? f->serial.load(std::memory_order_relaxed) : 0u);
			if (begin)
				++serial;
			t.host = c.host;
			t.seq = c.seq;
			t.phase = c.phase;
			t.serial = serial;
			if (begin)
				t.nextSeq = -1;
			else if (c.nextSeq >= 0)
				t.nextSeq = c.nextSeq;
			if (!begin)
				t.generation = c.generation;
			else if (!sameHost)
				t.generation = 0;
			if (!f)
				return;
			const uint64_t v = f->version.load(std::memory_order_relaxed);
			f->version.store(v + 1u, std::memory_order_relaxed);
			std::atomic_thread_fence(std::memory_order_release);
			f->seq.store(t.seq, std::memory_order_relaxed);
			f->nextSeq.store(t.nextSeq, std::memory_order_relaxed);
			f->generation.store(t.generation, std::memory_order_relaxed);
			f->phase.store(static_cast<int>(t.phase), std::memory_order_relaxed);
			f->serial.store(t.serial, std::memory_order_relaxed);
			f->version.store(v + 2u, std::memory_order_release);
		}

		void ForgetCurrentFrame(const void* host)
		{
			if (!host)
				return;
			Frames().Remove(host, true);
			if (Sticky().host == host)
				Sticky() = CurrentFrame();
		}

		void ResetThisThreadFrameForTesting() { Sticky() = CurrentFrame(); }

		// Take-over phases stop at the first module that returns true; fan-out phases (RE::PhaseFansOut) call every module.
		bool ExtensionHub::OnFrame(RE::FrameContext& c)
		{
			RecordCurrentFrame(c);   // L-CORE: before the fan-out, so every module (and the render's hooks) sees this frame
			const bool fanOut = RE::PhaseFansOut(c.phase);
			if (c.seq >= 0)
				ComputeOnce::SetThreadFrame(c.seq, "hub", static_cast<int64_t>(c.phase));   // compute-once frame scope (no-op when off)
			bool handled = false;
			for (auto& m : modules_)
				if (fanOut || !handled) { try { if (DispatchPhase(*m, c)) handled = true; } catch (...) { Contain(c, m->Name()); } }
			return handled;
		}

		double ExtensionHub::OnResolve(RE::Resolve kind, const RE::Context& ctx, double billValue)
		{
			double value = billValue;
			for (auto& m : modules_) { try { value = m->OnResolve(kind, ctx, value); } catch (...) { Contain(ctx, m->Name()); } }
			return value;
		}

		void ExtensionHub::OnStatus(const RE::Context& ctx, RE::StatusKind kind, std::vector<std::string>& lines, const char* worker, const char* what)
		{
			for (auto& m : modules_) { try { m->OnStatus(ctx, kind, lines, worker, what); } catch (...) { if (kind != RE::StatusKind::WorkerFault) Contain(ctx, m->Name()); } }
		}

		bool ExtensionHub::OnCommand(const RE::Context& ctx, const char* module, const char* command, std::string& reply)
		{
			bool handled = false;
			for (auto& m : modules_)
				if (!handled) { try { handled = m->OnCommand(ctx, module, command, reply); } catch (...) { Contain(ctx, m->Name()); } }
			return handled;
		}

		void* ExtensionHub::Query(const char* iid)
		{
			void* found = nullptr;
			for (auto& m : modules_)
				if (!found) { try { found = m->Query(iid); } catch (...) {} }
			return found;
		}

		void RegisterHub()
		{
			static bool registered = false;
			if (registered) return;
			registered = true;
			ExtensionHub::Instance(); // construct before the registry owns the forwarder, so it is destroyed after it
			RE::Register(std::unique_ptr<RE::IRendererExtension>(new Forwarder()));
		}
	}

	namespace Segments
	{
		CoreSegmentCaches& CoreSegmentCaches::Instance()
		{
			static CoreSegmentCaches caches;
			return caches;
		}

		void CoreSegmentCaches::OnGpuCreated(const void* host, const void* gpu)
		{
			if (!host)
				return;
			std::lock_guard<std::mutex> lock(mutex_);
			Entry& e = FindOrAddLocked(host);
			++e.gpuEpoch;
			e.gpu = gpu;
			e.float32Known = false;
			e.float32Supported = false;
		}

		bool CoreSegmentCaches::OnGpuCreated(const void* host, const void* gpu, Float32FboProbeFn probe)
		{
			if (!host)
				return (gpu && probe) ? probe(gpu) : false;
			uint64_t epoch = 0;
			{
				std::lock_guard<std::mutex> lock(mutex_);
				Entry& e = FindOrAddLocked(host);
				++e.gpuEpoch;
				e.gpu = gpu;
				e.float32Known = false;
				e.float32Supported = false;
				epoch = e.gpuEpoch;
			}
			if (!gpu || !probe)
				return false;
			const bool supported = probe(gpu);
			std::lock_guard<std::mutex> lock(mutex_);
			if (Entry* e = FindLocked(host))
			{
				if (e->gpu == gpu && e->gpuEpoch == epoch)
				{
					e->float32Known = true;
					e->float32Supported = supported;
				}
			}
			return supported;
		}

		void CoreSegmentCaches::OnGpuReleased(const void* host)
		{
			if (!host)
				return;
			std::lock_guard<std::mutex> lock(mutex_);
			for (size_t i = 0; i < entries_.size(); ++i)
			{
				if (entries_[i].host == host)
				{
					entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(i));
					return;
				}
			}
		}

		bool CoreSegmentCaches::GpuSupportsFloat32Fbo(const void* host, const void* gpu, Float32FboProbeFn probe)
		{
			if (!gpu)
				return false;
			if (!host)
				return probe ? probe(gpu) : false;
			uint64_t epoch = 0;
			{
				std::lock_guard<std::mutex> lock(mutex_);
				Entry& e = FindOrAddLocked(host);
				if (e.gpu == gpu && e.float32Known)
					return e.float32Supported;
				if (e.gpu != gpu || e.gpuEpoch == 0)
				{
					++e.gpuEpoch;
					e.gpu = gpu;
					e.float32Known = false;
					e.float32Supported = false;
				}
				epoch = e.gpuEpoch;
			}
			if (!probe)
				return false;
			const bool supported = probe(gpu);
			std::lock_guard<std::mutex> lock(mutex_);
			if (Entry* e = FindLocked(host))
			{
				if (e->gpu == gpu && e->gpuEpoch == epoch)
				{
					e->float32Known = true;
					e->float32Supported = supported;
				}
			}
			return supported;
		}

		bool CoreSegmentCaches::RefreshTraceFlag(const void* host, EnvProbeFn probe)
		{
			const bool enabled = probe ? probe() : false;
			if (host)
			{
				std::lock_guard<std::mutex> lock(mutex_);
				Entry& e = FindOrAddLocked(host);
				e.traceKnown = true;
				e.traceEnabled = enabled;
			}
			return enabled;
		}

		bool CoreSegmentCaches::TraceHookListEnabled(const void* host, EnvProbeFn probe)
		{
			if (!host)
				return probe ? probe() : false;
			{
				std::lock_guard<std::mutex> lock(mutex_);
				if (const Entry* e = FindLocked(host))
				{
					if (e->traceKnown)
						return e->traceEnabled;
				}
			}
			const bool enabled = probe ? probe() : false;
			std::lock_guard<std::mutex> lock(mutex_);
			Entry& e = FindOrAddLocked(host);
			if (!e.traceKnown)
			{
				e.traceKnown = true;
				e.traceEnabled = enabled;
			}
			return e.traceEnabled;
		}

		void CoreSegmentCaches::ResetForTesting()
		{
			std::lock_guard<std::mutex> lock(mutex_);
			entries_.clear();
		}

		size_t CoreSegmentCaches::HostCountForTesting() const
		{
			std::lock_guard<std::mutex> lock(mutex_);
			return entries_.size();
		}

		uint64_t CoreSegmentCaches::GpuEpochForTesting(const void* host) const
		{
			if (!host)
				return 0;
			std::lock_guard<std::mutex> lock(mutex_);
			const Entry* e = FindLocked(host);
			return e ? e->gpuEpoch : 0;
		}

		CoreSegmentCaches::Entry* CoreSegmentCaches::FindLocked(const void* host)
		{
			for (Entry& e : entries_)
				if (e.host == host)
					return &e;
			return nullptr;
		}

		const CoreSegmentCaches::Entry* CoreSegmentCaches::FindLocked(const void* host) const
		{
			for (const Entry& e : entries_)
				if (e.host == host)
					return &e;
			return nullptr;
		}

		CoreSegmentCaches::Entry& CoreSegmentCaches::FindOrAddLocked(const void* host)
		{
			if (Entry* existing = FindLocked(host))
				return *existing;
			Entry e;
			e.host = host;
			entries_.push_back(e);
			return entries_.back();
		}
	}
}

namespace VpMotion
{
	namespace LaneAhead
	{
		constexpr int AheadRing::kCapacityMax;

		const char* KeyName(SpareKey k)
		{
			switch (k) {
			case SpareKey::Auto: return "auto";
			case SpareKey::On: return "on";
			case SpareKey::Off: return "off";
			default: return "?";
			}
		}

		bool ParseKey(const char* text, SpareKey& out)
		{
			if (!text) return false;
			const char* names[] = { "auto", "on", "off" };
			for (int i = 0; i < 3; ++i) {
				const char* a = names[i];
				const char* b = text;
				while (*a && *a == *b) { ++a; ++b; }
				if (*a == 0 && *b == 0) { out = static_cast<SpareKey>(i); return true; }
			}
			return false;
		}

		const char* ReasonName(Reason r)
		{
			switch (r) {
			case Reason::Ahead: return "ahead";
			case Reason::Off: return "off";
			case Reason::LowLatency: return "low latency: single-frame motion, no field ahead";
			case Reason::Competitive: return "competitive latency";
			case Reason::EngineOff: return "matcher not estimating";
			case Reason::ShortQueue: return "short queue";
			case Reason::RingFull: return "depth held";
			case Reason::P2NotReady: return "P2 not ready";
			case Reason::NoSpare: return "no spare";
			case Reason::SourceNotServed: return "source ring not served in the slot";
			default: return "?";
			}
		}

		Decision Decide(const Input& in)
		{
			Decision d;
			if (!KeyOn(in.key, in.autoAccepted)) { d.reason = Reason::Off; return d; }
			if (in.competitive) { d.reason = Reason::Competitive; return d; }
			if (in.lowLatency) { d.reason = Reason::LowLatency; return d; }
			if (!in.matcherActive) { d.reason = Reason::EngineOff; return d; }
			int depth = RingCapacity(in.requestedDepth, DepthMax(in.anchorSlots, in.extendedTier));
			if (in.queuedAfterNext < depth) depth = in.queuedAfterNext > 0 ? in.queuedAfterNext : 0;
			if (in.laneLead > 0 && in.laneLead - 1 < depth) depth = in.laneLead - 1 > 0 ? in.laneLead - 1 : 0;
			d.depth = depth;
			if (depth <= 0) { d.reason = Reason::ShortQueue; return d; }
			if (in.held >= depth) { d.reason = Reason::RingFull; return d; }
			if (in.producerRunning && !in.nextPairP2Ready) { d.reason = Reason::P2NotReady; return d; }
			d.reason = Reason::Ahead;
			d.pairsWanted = depth - (in.held > 0 ? in.held : 0);
			return d;
		}

		AfterEstimateResult AfterEstimateDispatch::Notify(IAfterEstimate* hook, const AfterEstimateView& view, Entry& entry, uint64_t serial)
		{
			AfterEstimateResult r = AfterEstimateResult::Called;
			if (serial == 0 || serial == m_lastSerial) r = AfterEstimateResult::Repeat;
			else if (!view.key.Valid() || view.key != entry.key) r = AfterEstimateResult::BadKey;
			else if (!view.fromLuma || !view.toLuma) r = AfterEstimateResult::PlanesMissing;
			else if (!hook) r = AfterEstimateResult::NoHook;
			if (r != AfterEstimateResult::Repeat) m_lastSerial = serial;
			if (r == AfterEstimateResult::Called) hook->AfterEstimate(view, entry);
			++m_counts[static_cast<size_t>(r)];
			if (r == AfterEstimateResult::Called) ++m_byPath[view.atRender ? 1 : 0];
			return r;
		}

		const char* DiscardCauseName(DiscardCause c)
		{
			switch (c) {
			case DiscardCause::NextDiffers: return "next differs";
			case DiscardCause::Drop: return "drop";
			case DiscardCause::Resync: return "resync";
			case DiscardCause::EpochMoved: return "epoch moved";
			case DiscardCause::PictureChanged: return "picture";
			case DiscardCause::LeaseLost: return "lease lost";
			default: return "?";
			}
		}

		void AheadRing::SetCapacity(int capacity)
		{
			m_capacity = capacity < 1 ? 1 : (capacity > kCapacityMax ? kCapacityMax : capacity);
			int keep = m_count < m_capacity ? m_count : m_capacity;
			for (int i = 0; i < keep; ++i)
				if (m_slots[static_cast<size_t>(i)] >= m_capacity) { keep = i; break; }
			while (m_count > keep) { --m_count; ++m_discarded[static_cast<size_t>(DiscardCause::Drop)]; }
		}

		int AheadRing::FreeSlot() const
		{
			for (int s = 0; s < m_capacity; ++s)
				if (!SlotUsed(s)) return s;
			return -1;
		}

		bool AheadRing::Holds(uint64_t from, uint64_t to) const
		{
			for (int i = 0; i < m_count; ++i)
				if (m_entries[static_cast<size_t>(i)].from == from && m_entries[static_cast<size_t>(i)].to == to) return true;
			return false;
		}

		PushResult AheadRing::Push(const EntryKey& k, int slot)
		{
			PushResult r = PushResult::Held;
			if (!k.Valid()) r = PushResult::BadKey;
			else if (Holds(k.from, k.to)) r = PushResult::AlreadyHeld;
			else if (m_count > 0 && k.from != BackTo()) r = PushResult::NotChained;
			else if (m_count >= m_capacity) r = PushResult::Full;
			else if (slot < 0 || slot >= m_capacity || SlotUsed(slot)) r = PushResult::BadSlot;
			++m_push[static_cast<size_t>(r)];
			if (r == PushResult::Held) {
				m_slots[static_cast<size_t>(m_count)] = slot;
				m_entries[static_cast<size_t>(m_count++)] = k;
			}
			return r;
		}

		bool AheadRing::Consume(uint64_t from, uint64_t to, uint64_t epoch, int* slot)
		{
			if (slot != nullptr) *slot = -1;
			while (m_count > 0 && m_entries[0].from < from) { PopFront(); ++m_discarded[static_cast<size_t>(DiscardCause::Drop)]; }
			if (m_count == 0) { ++m_sequential; m_consumedEpoch = 0; return false; }
			const EntryKey& f = m_entries[0];
			if (f.from == from && f.to == to && epoch != 0 && f.epoch == epoch) {
				m_consumedEpoch = f.epoch;
				if (slot != nullptr) *slot = m_slots[0];
				PopFront();
				++m_hits;
				return true;
			}
			Discard(f.from == from && f.to == to ? DiscardCause::EpochMoved : DiscardCause::NextDiffers);
			++m_sequential;
			m_consumedEpoch = 0;
			return false;
		}

		void AheadRing::Discard(DiscardCause c)
		{
			m_discarded[static_cast<size_t>(c)] += static_cast<unsigned long long>(m_count);
			m_count = 0;
		}

		void AheadRing::PopFront()
		{
			for (int i = 1; i < m_count; ++i) {
				m_entries[static_cast<size_t>(i - 1)] = m_entries[static_cast<size_t>(i)];
				m_slots[static_cast<size_t>(i - 1)] = m_slots[static_cast<size_t>(i)];
			}
			--m_count;
		}

		bool AheadRing::SlotUsed(int slot) const
		{
			for (int i = 0; i < m_count; ++i)
				if (m_slots[static_cast<size_t>(i)] == slot) return true;
			return false;
		}

		int AheadFeed::Pending(const AheadRing& ring, uint64_t* out, int max, int* slots) const
		{
			int n = 0;
			for (int i = 0; i < ring.Held() && n < max; ++i)
				if (ring.At(i).to > m_lastPublished) {
					if (slots != nullptr) slots[n] = ring.SlotAt(i);
					out[n++] = ring.At(i).to;
				}
			return n;
		}
	}
}

namespace Dejudder
{
	void AheadSchedule::Note(Reason r)
	{
		last = r;
		++counts[static_cast<size_t>(r)];
	}

	bool AheadSchedule::CheckConditions(const Conditions& c)
	{
		auto skip = [this](Reason r) { Note(r); return false; };
		if (!c.enabled) return skip(Reason::Off);
		if (!c.renderedWarp) return skip(Reason::SourceWarp);
		if (!c.active) return skip(Reason::NotActive);
		if (c.next == 0) return skip(Reason::NoNext);
		if (c.after == 0) return skip(c.afterGap ? Reason::AfterGap : Reason::NoAfter);
		if (c.after == c.next || c.after == c.source) return skip(Reason::AfterNotNew);
		const bool estimatedOk = (c.estimated == c.source) ||
			(!ring_.Empty() && c.estimated == ring_.At(ring_.Held() - 1).from);
		if (!estimatedOk) return skip(Reason::NotEstimated);
		if (!c.layoutMatches) return skip(Reason::Layout);
		return true;
	}

	bool AheadSchedule::TryAheadSlot(const Conditions& c, const Steps& steps, int* outSlot)
	{
		if (outSlot != nullptr)
			*outSlot = -1;
		if (!CheckConditions(c))
			return false;
		VpMotion::LaneAhead::EntryKey key;
		key.from = c.next;
		key.to = c.after;
		key.epoch = epochNow;
		const int slot = ring_.FreeSlot();
		if (!key.Valid() || ring_.Holds(key.from, key.to) || (!ring_.Empty() && key.from != ring_.BackTo()) || slot < 0)
		{
			ring_.Push(key, slot);
			return false;
		}
		if (!steps.snapshot || !steps.snapshot())
		{
			Note(Reason::SnapshotFailed);
			return false;
		}
		// From here the copy is in use whatever happens.
		if (!steps.uploadAfter || !steps.uploadAfter())
		{
			Note(Reason::UploadFailed);
			return true;
		}
		if (!steps.estimate || !steps.estimate(c.next, c.after))
		{
			Note(Reason::EstimateFailed);
			return true;
		}
		legacyFrom_ = legacyTo_ = 0;
		if (ring_.Push(key, slot) != VpMotion::LaneAhead::PushResult::Held)
		{
			Note(Reason::EstimateFailed);
			return true;
		}
		if (outSlot != nullptr)
			*outSlot = slot;
		Note(Reason::Ahead);
		return true;
	}

	bool AheadSchedule::TryAhead(const Conditions& c, const Steps& steps)
	{
		if (ring_.Capacity() > 1 && epochNow != 0)
			return TryAheadSlot(c, steps, nullptr);
		if (!CheckConditions(c))
			return false;
		if (!steps.snapshot || !steps.snapshot())
		{
			Note(Reason::SnapshotFailed);
			return false;
		}
		// From here the copy is in use whatever happens.
		if (!steps.uploadAfter || !steps.uploadAfter())
		{
			Note(Reason::UploadFailed);
			return true;
		}
		if (!steps.estimate || !steps.estimate(c.next, c.after))
		{
			Note(Reason::EstimateFailed);
			return true;
		}
		if (!ring_.Empty())
			ring_.Discard(VpMotion::LaneAhead::DiscardCause::NextDiffers);
		if (epochNow != 0 && c.after > c.next)
		{
			legacyFrom_ = legacyTo_ = 0;
			VpMotion::LaneAhead::EntryKey key;
			key.from = c.next;
			key.to = c.after;
			key.epoch = epochNow;
			ring_.Push(key, ring_.FreeSlot());
		}
		else
		{
			legacyFrom_ = c.next;
			legacyTo_ = c.after;
		}
		Note(Reason::Ahead);
		return true;
	}

	bool AheadSchedule::Consume(uint64_t source, uint64_t next)
	{
		if (!ring_.Empty())
		{
			const VpMotion::LaneAhead::EntryKey front = ring_.At(0);
			if (next != 0 && front.from == source && front.to == next)
			{
				ring_.Consume(source, next, front.epoch, nullptr);
				++hits;
				return true;
			}
			ring_.Discard(VpMotion::LaneAhead::DiscardCause::NextDiffers);
			++misses;
			++sequential;
			return false;
		}
		if (legacyFrom_ != 0)
		{
			const bool hit = next != 0 && legacyFrom_ == source && legacyTo_ == next;
			if (hit)
				++hits;
			else
			{
				++misses;
				++sequential;
			}
			legacyFrom_ = legacyTo_ = 0;
			return hit;
		}
		++sequential;
		return false;
	}

	bool AheadSchedule::Consume(uint64_t source, uint64_t next, uint64_t epoch, int* outSlot)
	{
		if (outSlot != nullptr)
			*outSlot = -1;
		if (legacyFrom_ != 0)
		{
			const bool pair = next != 0 && legacyFrom_ == source && legacyTo_ == next;
			++misses;
			if (pair)
				++epochMisses;
			++sequential;
			consumedEpoch = 0;
			legacyFrom_ = legacyTo_ = 0;
			return false;
		}
		if (ring_.Empty())
		{
			++sequential;
			consumedEpoch = 0;
			return false;
		}
		const unsigned long long epochDiscardsBefore = ring_.Discarded(VpMotion::LaneAhead::DiscardCause::EpochMoved);
		const bool hit = ring_.Consume(source, next, epoch, outSlot);
		if (hit)
		{
			++hits;
			consumedEpoch = ring_.ConsumedEpoch();
			return true;
		}
		++misses;
		if (ring_.Discarded(VpMotion::LaneAhead::DiscardCause::EpochMoved) > epochDiscardsBefore)
			++epochMisses;
		++sequential;
		consumedEpoch = 0;
		return false;
	}

	void AheadSchedule::SetCapacity(int depth)
	{
		ring_.SetCapacity(depth);
	}

	int AheadSchedule::Capacity() const
	{
		return ring_.Capacity();
	}

	int AheadSchedule::Held() const
	{
		return ring_.Held() + (legacyFrom_ != 0 ? 1 : 0);
	}

	int AheadSchedule::SlotAt(int i) const
	{
		return (i >= 0 && i < ring_.Held()) ? ring_.SlotAt(i) : -1;
	}

	const VpMotion::LaneAhead::EntryKey& AheadSchedule::EntryAt(int i) const
	{
		static const VpMotion::LaneAhead::EntryKey kEmpty;
		return (i >= 0 && i < ring_.Held()) ? ring_.At(i) : kEmpty;
	}

	void AheadSchedule::Discard(VpMotion::LaneAhead::DiscardCause cause)
	{
		legacyFrom_ = legacyTo_ = 0;
		ring_.Discard(cause);
	}

	void AheadSchedule::Reset()
	{
		legacyFrom_ = legacyTo_ = 0;
		ring_.Reset();
	}

	const char* AheadSchedule::Name(Reason r)
	{
		switch (r)
		{
		case Reason::Ahead: return "estimated ahead";
		case Reason::Off: return "off";
		case Reason::SourceWarp: return "source warp";
		case Reason::NotActive: return "de-judder not running";
		case Reason::NoNext: return "next frame unknown";
		case Reason::NoAfter: return "no frame after next (short queue)";
		case Reason::AfterNotNew: return "frame after next not new";
		case Reason::NotEstimated: return "this pair not estimated";
		case Reason::Layout: return "frame after next in another layout";
		case Reason::SnapshotFailed: return "copy failed";
		case Reason::UploadFailed: return "upload failed";
		case Reason::EstimateFailed: return "estimate failed";
		case Reason::AfterGap: return "frame after next follows a gap";
		default: return "?";
		}
	}
}
