// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// Built without the precompiled header (libplacebo + VP-V headers only).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <VP-V/modules/control/skeleton/gpu/SkeletonGpuDispatcher.h>
#include <VP-V/core/ComputeOnce.h>
#include <VP-V/core/GpuAttributionPl.h>

#include <algorithm>

namespace Vpv
{
	namespace Skeleton
	{
		using VpLaneProducts::GpuProductTextures;
		using VpLaneProducts::KernelContext;
		using VpSkeleton::Dims;
		using VpSkeleton::Node;
		using VpSkeleton::ProductId;

		namespace
		{
			uint64_t Mix(uint64_t h, uint64_t v) { return (h ^ v) * 0x100000001B3ull; }

			bool CreateR16(pl_gpu gpu, pl_tex& tex, int w, int h, bool readable, std::string& why)
			{
				const pl_fmt fmt = VpSkeleton::PlTexBackend::FindFormat(gpu, VpSkeleton::TexFormat::R16);
				if (!fmt)
				{
					why = "the device has no sampleable and storable 16-bit single-channel texture format for the tap plane";
					return false;
				}
				pl_tex_params tp{};
				tp.w = w;
				tp.h = h;
				tp.format = fmt;
				tp.sampleable = true;
				tp.storable = true;
				tp.host_writable = true;
				tp.host_readable = readable && (fmt->caps & PL_FMT_CAP_HOST_READABLE) != 0;   // only what the format supports
				if (pl_tex_recreate(gpu, &tex, &tp))
					return true;
				why = std::string("the GPU refused the tap plane texture ") + std::to_string(w) + "x" + std::to_string(h) + " " +
					(fmt->name ? fmt->name : "?") + (tp.host_readable ? " (host readable)" : "");
				return false;
			}
		}

		SkeletonGpuDispatcher::SkeletonGpuDispatcher(pl_gpu gpu, bool hostReadable)
			: m_gpu(gpu), m_texPool(std::make_shared<TexPool>(gpu, hostReadable)), m_taps(kTapRing)
		{
			if (m_gpu)
				m_dispatch = pl_dispatch_create(m_gpu->log, m_gpu);
			WatchAttributionIfEnabled();
		}

		SkeletonGpuDispatcher::~SkeletonGpuDispatcher()
		{
			Release();
			if (m_texPool)
			{
				// W23-REG-L1: drain any retired textures freed during Release() before marking the pool abandoned.
				// Any external Acquire keepAlive still surviving past ~SkeletonGpuDispatcher must never invoke
				// libplacebo on a destroyed/gone device.
				for (;;)
				{
					std::vector<std::unique_ptr<GpuProductTextures>> dead;
					{
						std::lock_guard<std::mutex> lk(m_texPool->mutex);
						if (m_texPool->retired.empty())
						{
							m_texPool->backend.gpu = nullptr;
							m_texPool->abandoned = true;
							break;
						}
						dead.swap(m_texPool->retired);
					}
					dead.clear();
				}
			}
			if (m_dispatch)
				pl_dispatch_destroy(&m_dispatch);
		}

		void SkeletonGpuDispatcher::WatchAttributionIfEnabled()
		{
			if (!m_attributionWatched && m_dispatch != nullptr && ::ComputeOnce::Enabled())
			{
				GpuAttribution::Watch(m_dispatch, "lane_gpu");
				m_attributionWatched = true;
			}
		}

		std::shared_ptr<GpuProductTextures> SkeletonGpuDispatcher::MakeProductTextures()
		{
			const std::shared_ptr<TexPool> pool = m_texPool;
			GpuProductTextures* raw = new GpuProductTextures(pool->backend);
			return std::shared_ptr<GpuProductTextures>(raw, [pool](GpuProductTextures* p) {
				if (!p)
					return;
				std::lock_guard<std::mutex> lk(pool->mutex);
				if (pool->abandoned)
				{
					// backend.gpu is nullptr: free CPU wrapper without touching libplacebo.
					delete p;
					return;
				}
				pool->retired.emplace_back(p);
			});
		}

		void SkeletonGpuDispatcher::DrainRetiredTextures()
		{
			if (!m_texPool)
				return;
			std::vector<std::unique_ptr<GpuProductTextures>> dead;
			{
				std::lock_guard<std::mutex> lk(m_texPool->mutex);
				dead.swap(m_texPool->retired);
			}
			// Destroy outside lock on render thread (GPU-legal phase).
			dead.clear();
		}

		void SkeletonGpuDispatcher::AddKernel(std::unique_ptr<VpLaneProducts::IProductKernel> kernel)
		{
			if (kernel)
			{
				kernel->SetStaticTextures(&m_statics);
				m_kernels.push_back(std::move(kernel));
			}
		}

		VpLaneProducts::IProductKernel* SkeletonGpuDispatcher::KernelOf(ProductId id) const
		{
			for (const auto& k : m_kernels)
				if (k->Product() == id)
					return k.get();
			return nullptr;
		}

		bool SkeletonGpuDispatcher::Fail(const std::string& why)
		{
			m_error = why;
			++m_counters.failures;
			return false;
		}

		bool SkeletonGpuDispatcher::Configure(const VpSkeleton::Graph& graph, const VpSkeleton::Registry& reg,
			const VpSkeleton::TierPlan& plan, Dims dims, uint32_t settingsGen, bool oldJobsJoined)
		{
			if (!Valid())
				return Fail("no GPU device for the analysis products");
			uint64_t key = Mix(Mix(0xCBF29CE484222325ull, dims.width), dims.height);
			size_t maxGpuRing = 0;
			for (const Node& n : graph.nodes)
				if (n.backend == VpSkeleton::Backend::GpuCompute && KernelOf(n.id))
				{
					key = Mix(Mix(Mix(Mix(key, n.id), n.ring), n.livePool), plan.Laned(n.id) ? 1u : 0u);
					if (plan.Laned(n.id) && size_t(n.ring) > maxGpuRing)
						maxGpuRing = size_t(n.ring);
				}
			// W23-SK-H2: size the tap ring from the backing lane-lead depth (at least kTapRing, capped at kMaxTapRing)
			// so chunked P10 taps never evict an in-flight frame prematurely.
			const size_t wantTaps = (std::min<size_t>)(kMaxTapRing, (std::max<size_t>)(kTapRing, 1u + maxGpuRing));
			const bool sizeChanged = dims.width != m_dims.width || dims.height != m_dims.height;
			if (key == m_graphKey && settingsGen == m_settingsGen && !m_products.empty() && m_taps.size() == wantTaps)
				return true;
			const bool rebuildTextures = key != m_graphKey || m_products.empty();
			m_graphKey = key;
			m_settingsGen = settingsGen;
			bool ok = true;
			if (rebuildTextures)
			{
				NotePeakBytes();
				// W23-SK-M2: preserve measured per-product GPU cost across same-dims graph re-plans so the gap
				// scheduler never loses its measured cost history on non-size reconfigures.
				std::map<ProductId, std::pair<GpuProductCost, uint64_t>> savedCosts;
				if (!sizeChanged)
				{
					for (const auto& kv : m_products)
						savedCosts[kv.first] = std::make_pair(kv.second.cost, kv.second.lastNs);
				}
				if (oldJobsJoined)
				{
					// K#12 (PERF-6) + W23-SK-B1: old workers/jobs are already joined; drop unpinned textures before
					// allocating replacements so peak VRAM stays <= max(old, new). If an external Acquire reader still
					// holds keepAlive (use_count > 1), move to m_graveyard so its pl_tex stays valid until released.
					FlushReleases();
					for (auto& kv : m_products)
					{
						if (kv.second.tex)
						{
							if (kv.second.tex.use_count() > 1)
							{
								m_graveyard.push_back(std::move(kv.second.tex));
								++m_counters.releasesQueued;
							}
							else
							{
								kv.second.tex.reset();
								++m_counters.releasesDone;
							}
						}
						if (kv.second.timer)
							pl_timer_destroy(m_gpu, &kv.second.timer);
					}
					DrainRetiredTextures();
				}
				else
				{
					// Old textures: destroyed at the next GPU-legal phase (a lane job of the old graph may still name them).
					for (auto& kv : m_products)
					{
						if (kv.second.tex)
						{
							m_graveyard.push_back(std::move(kv.second.tex));
							++m_counters.releasesQueued;
						}
						if (kv.second.timer)
							pl_timer_destroy(m_gpu, &kv.second.timer);
					}
				}
				m_products.clear();
				for (const Node& n : graph.nodes)
				{
					if (n.backend != VpSkeleton::Backend::GpuCompute)
						continue;
					VpLaneProducts::IProductKernel* k = KernelOf(n.id);
					if (!k)
						continue;
					ProductState& ps = m_products[n.id];
					ps.history = n.history;
					const auto sc = savedCosts.find(n.id);
					if (sc != savedCosts.end())
					{
						ps.cost = sc->second.first;
						ps.lastNs = sc->second.second;
					}
					ps.tex = MakeProductTextures();
					m_texPool->backend.lastError.clear();
					if (!ps.tex->Allocate(n, reg.Get(n.id), k->Layout(dims), dims, plan))
					{
						ps.refusal = ps.tex->LastRefusal().empty()
							? std::string("video memory could not be allocated: ") + (m_texPool->backend.lastError.empty() ? "unknown reason" : m_texPool->backend.lastError)
							: ps.tex->LastRefusal();
						ps.tex.reset();
						DrainRetiredTextures();
						ok = false;
						continue;
					}
					++m_counters.allocations;
					NotePeakBytes();
					ps.timer = pl_timer_create(m_gpu);
				}
			}
			if (sizeChanged)
			{
				for (Tap& t : m_taps)
				{
					t.used = false;
					t.pinned = false;
					if (t.tap)
						pl_tex_destroy(m_gpu, &t.tap);
					if (t.extended)
						pl_tex_destroy(m_gpu, &t.extended);
				}
				m_dims = dims;
			}
			if (rebuildTextures && oldJobsJoined)
			{
				for (Tap& t : m_taps)
					t.pinned = false;
			}
			if (m_taps.size() != wantTaps)
			{
				if (wantTaps < m_taps.size())
				{
					// W23-REG-M1: preserve any in-flight pinned taps by partitioning pinned first; never destroy a pinned tap.
					size_t pinnedCount = 0;
					for (const Tap& t : m_taps)
						if (t.pinned)
							++pinnedCount;
					const size_t keepCount = (std::max)(wantTaps, pinnedCount);
					std::stable_partition(m_taps.begin(), m_taps.end(), [](const Tap& t) { return t.pinned; });
					for (size_t i = keepCount; i < m_taps.size(); ++i)
					{
						if (m_taps[i].tap)
							pl_tex_destroy(m_gpu, &m_taps[i].tap);
						if (m_taps[i].extended)
							pl_tex_destroy(m_gpu, &m_taps[i].extended);
					}
					m_taps.resize(keepCount);
				}
				else
					m_taps.resize(wantTaps);
			}
			for (const auto& k : m_kernels)
				if (m_products.count(k->Product()) != 0 && !k->Prepare(m_gpu, m_dispatch, dims))
				{
					ProductState& ps = m_products[k->Product()];
					ps.refusal = "preparation failed: " + k->LastError();
					// W23-SK-M2 / B1: account for released textures cleanly on Prepare() failure.
					if (ps.tex)
					{
						if (!oldJobsJoined || ps.tex.use_count() > 1)
						{
							m_graveyard.push_back(std::move(ps.tex));
							++m_counters.releasesQueued;
						}
						else
						{
							ps.tex.reset();
							DrainRetiredTextures();
							++m_counters.releasesDone;
						}
					}
					ok = false;
				}
			if (!ok)
			{
				// The first refusal is the reason (the status lists every product's own).
				std::string first;
				for (const auto& kv : m_products)
					if (!kv.second.refusal.empty()) { first = kv.second.refusal; break; }
				m_error = "an analysis product is unavailable (see the status)" + (first.empty() ? std::string() : ": " + first);
			}
			return ok;
		}

		pl_tex SkeletonGpuDispatcher::StaticStore::Rgba32f(const void* owner, int slot, int w, int h, const float* rgba, bool replace)
		{
			m_error.clear();
			const pl_gpu gpu = m_gpu != nullptr ? *m_gpu : nullptr;
			if (!gpu)
			{
				m_error = "no GPU device";
				return nullptr;
			}
			const auto key = std::make_pair(owner, slot);
			const auto it = m_tex.find(key);
			if (it != m_tex.end())
			{
				if (!replace)
					return it->second;
				pl_tex_destroy(gpu, &it->second);
				m_tex.erase(it);
			}
			const pl_fmt fmt = VpSkeleton::PlTexBackend::FindFormat(gpu, VpSkeleton::TexFormat::RGBA32F);
			if (!fmt)
			{
				m_error = "the device has no sampleable and storable rgba32f texture format";
				return nullptr;
			}
			pl_tex_params tp{};
			tp.w = w;
			tp.h = h;
			tp.format = fmt;
			tp.sampleable = true;
			tp.initial_data = rgba;
			pl_tex tex = pl_tex_create(gpu, &tp);
			if (!tex)
			{
				m_error = std::string("the GPU refused a ") + std::to_string(w) + "x" + std::to_string(h) + " rgba32f texture";
				return nullptr;
			}
			m_tex[key] = tex;
			return tex;
		}

		void SkeletonGpuDispatcher::StaticStore::Forget(const void* owner)
		{
			const pl_gpu gpu = m_gpu != nullptr ? *m_gpu : nullptr;
			for (auto it = m_tex.begin(); it != m_tex.end();)
			{
				if (it->first.first == owner)
				{
					if (gpu)
						pl_tex_destroy(gpu, &it->second);
					it = m_tex.erase(it);
				}
				else
					++it;
			}
		}

		void SkeletonGpuDispatcher::StaticStore::Clear()
		{
			const pl_gpu gpu = m_gpu != nullptr ? *m_gpu : nullptr;
			for (auto& kv : m_tex)
				if (gpu && kv.second)
					pl_tex_destroy(gpu, &kv.second);
			m_tex.clear();
		}

		void SkeletonGpuDispatcher::StaticStore::Abandon()
		{
			m_tex.clear();
		}

		void SkeletonGpuDispatcher::FlushReleases()
		{
			NotePeakBytes();
			// W23-SK-B1: keep any graveyard entry still pinned by an external Acquire keepAlive until released.
			std::vector<std::shared_ptr<GpuProductTextures>> stillPinned;
			for (auto& tex : m_graveyard)
			{
				if (tex && tex.use_count() > 1)
					stillPinned.push_back(std::move(tex));
				else if (tex)
				{
					tex.reset();
					++m_counters.releasesDone;
				}
			}
			m_graveyard.swap(stillPinned);
			DrainRetiredTextures();
		}

		void SkeletonGpuDispatcher::Release()
		{
			NotePeakBytes();
			FlushReleases();
			for (auto& kv : m_products)
			{
				if (kv.second.tex)
				{
					if (kv.second.tex.use_count() == 1)
						++m_counters.releasesDone;
					kv.second.tex.reset();
				}
				if (kv.second.timer && m_gpu)
					pl_timer_destroy(m_gpu, &kv.second.timer);
			}
			m_products.clear();
			m_graveyard.clear();
			DrainRetiredTextures();
			for (Tap& t : m_taps)
			{
				t.used = false;
				t.pinned = false;
				if (t.tap && m_gpu)
					pl_tex_destroy(m_gpu, &t.tap);
				if (t.extended && m_gpu)
					pl_tex_destroy(m_gpu, &t.extended);
			}
			for (const auto& k : m_kernels)
				if (m_gpu)
					k->Release(m_gpu);
			m_statics.Clear();   // the kernels' stand-ins and LUT (their Release asked for it; this catches the rest)
			m_graphKey = 0;
			m_dims = Dims{ 0, 0 };
		}

		const GpuProductTextures* SkeletonGpuDispatcher::TexturesOf(ProductId id) const
		{
			const auto it = m_products.find(id);
			return it == m_products.end() ? nullptr : it->second.tex.get();
		}

		std::shared_ptr<const GpuProductTextures> SkeletonGpuDispatcher::SharedTexturesOf(ProductId id) const
		{
			const auto it = m_products.find(id);
			return it == m_products.end() ? std::shared_ptr<const GpuProductTextures>() : std::shared_ptr<const GpuProductTextures>(it->second.tex);
		}

		bool SkeletonGpuDispatcher::Available(ProductId id) const
		{
			const auto it = m_products.find(id);
			return it != m_products.end() && it->second.tex != nullptr;
		}

		bool SkeletonGpuDispatcher::HoldsVideoMemory() const
		{
			if (AllocatedBytes() != 0 || GraveyardBytes() != 0)
				return true;
			for (const Tap& t : m_taps)
				if (t.tap != nullptr || t.extended != nullptr)
					return true;
			return false;
		}

		void SkeletonGpuDispatcher::Abandon()
		{
			// The renderer's device is gone: nothing may be handed to libplacebo any more (its objects died with it).
			m_gpu = nullptr;
			if (m_texPool)
			{
				std::lock_guard<std::mutex> lk(m_texPool->mutex);
				m_texPool->backend.gpu = nullptr;
				m_texPool->abandoned = true;
				m_texPool->retired.clear();
			}
			m_dispatch = nullptr;
			m_attributionWatched = false;
			m_lane = nullptr;
			m_statics.Abandon();
			for (Tap& t : m_taps)
			{
				t.tap = nullptr;
				t.extended = nullptr;
				t.used = false;
				t.pinned = false;
			}
			for (auto& kv : m_products)
				kv.second.timer = nullptr;
			// W23-SK-M4: clear dead-device product maps and graveyard immediately (with backend.gpu == nullptr).
			m_graveyard.clear();
			m_products.clear();
			m_graphKey = 0;
			m_dims = Dims{ 0, 0 };
		}

		uint64_t SkeletonGpuDispatcher::AllocatedBytes() const
		{
			uint64_t b = 0;
			for (const auto& kv : m_products)
				if (kv.second.tex)
					b += kv.second.tex->Bytes();
			return b;
		}

		uint64_t SkeletonGpuDispatcher::GraveyardBytes() const
		{
			uint64_t b = 0;
			for (const std::shared_ptr<GpuProductTextures>& tex : m_graveyard)
				if (tex)
					b += tex->Bytes();
			if (m_texPool)
			{
				std::lock_guard<std::mutex> lk(m_texPool->mutex);
				for (const std::unique_ptr<GpuProductTextures>& tex : m_texPool->retired)
					if (tex)
						b += tex->Bytes();
			}
			return b;
		}

		void SkeletonGpuDispatcher::NotePeakBytes()
		{
			const uint64_t cur = AllocatedBytes() + GraveyardBytes();
			if (cur > m_peakBytes)
				m_peakBytes = cur;
		}

		pl_tex SkeletonGpuDispatcher::TapOf(uint64_t seq, bool extended) const
		{
			for (const Tap& t : m_taps)
				if (t.used && t.seq == seq)
					return extended ? (t.extendedDone ? t.extended : nullptr) : t.tap;
			return nullptr;
		}

		SkeletonGpuDispatcher::Tap* SkeletonGpuDispatcher::TapFor(uint64_t seq, bool create)
		{
			for (Tap& t : m_taps)
				if (t.used && t.seq == seq)
					return &t;
			if (!create || m_taps.empty())
			{
				m_tapError = "the tap plane of frame " + std::to_string(seq) + " is not there (evicted, or its first chunk did not run)";
				return nullptr;
			}
			// W23-SK-H2 / W23-REG-M1: release stale pins whose frame has already left the lane source ring, then
			// prefer an unused slot, then the lowest-seq unpinned slot; only evict a pinned slot if all are pinned.
			if (m_lane != nullptr)
			{
				VpSkeleton::SourceFrameView probe;
				for (Tap& t : m_taps)
					if (t.used && t.pinned && t.seq != seq && !m_lane->SourceOf(t.seq, &probe))
						t.pinned = false;
			}
			Tap* victim = nullptr;
			m_tapError.clear();
			for (Tap& t : m_taps)
			{
				if (!t.used)
				{
					victim = &t;
					break;
				}
				if (!t.pinned && (victim == nullptr || t.seq < victim->seq))
					victim = &t;
			}
			if (victim == nullptr)
			{
				victim = &m_taps[0];
				for (Tap& t : m_taps)
					if (t.seq < victim->seq)
						victim = &t;
			}
			const int w = int(m_dims.width), h = int(m_dims.height);
			const bool readable = m_texPool && m_texPool->backend.hostReadable;
			std::string why;
			if (!CreateR16(m_gpu, victim->tap, w, h, readable, why) || !CreateR16(m_gpu, victim->extended, w, h, readable, why))
			{
				m_tapError = why;
				return nullptr;
			}
			victim->used = true;
			victim->pinned = false;
			victim->seq = seq;
			victim->bands = 0;
			victim->tapLuma = nullptr;
			victim->tapShift = 0;
			victim->extendedDone = false;
			return victim;
		}

		bool SkeletonGpuDispatcher::EnsureTap(Tap& t, const VpSkeleton::SourceFrameView& view, const RegionEvidence::Rect& picture, pl_timer timer)
		{
			if (t.tapLuma != view.luma || t.tapShift != view.msbShift)
			{
				t.bands = 0;
				t.extendedDone = false;
				t.tapLuma = view.luma;
				t.tapShift = view.msbShift;
			}
			const Dims d = m_dims;
			const uint32_t rpb = VpLaneProducts::TapRowsPerBand(d);
			const uint32_t bands = VpLaneProducts::TapBands(d);
			while (t.bands < bands)
			{
				const uint32_t y0 = t.bands * rpb;
				const uint32_t y1 = (std::min)(d.height, y0 + rpb);
				if (!VpLaneProducts::UploadTapRows(m_gpu, t.tap, view, y0, y1, m_scratch))
					return Fail("the tap plane could not be uploaded");
				++t.bands;
				++m_counters.tapBands;
			}
			const bool pictureChanged = t.picture.left != picture.left || t.picture.top != picture.top ||
				t.picture.right != picture.right || t.picture.bottom != picture.bottom;
			if (!t.extendedDone || pictureChanged)
			{
				t.picture = picture;
				// W23-SK-M3: time the extend pass with the owning product's timer, and record ComputeOnce (W23-SK-H2).
				if (!VpLaneProducts::ExtendTap(m_dispatch, t.tap, t.extended, picture, timer))
					return Fail("the extend pass failed");
				VPV_COMPUTE_ONCE_AT("lane_tap_extend", t.seq, 0);
				t.extendedDone = true;
				++m_counters.extends;
			}
			return true;
		}

		void SkeletonGpuDispatcher::PollTimer(ProductState& ps)
		{
			if (!ps.timer)
				return;
			for (uint64_t ns; (ns = pl_timer_query(m_gpu, ps.timer)) != 0;)
			{
				ps.lastNs = ns;
				ps.cost.NoteResult(ns);
			}
			ps.cost.NotePoll();
		}

		bool SkeletonGpuDispatcher::BuildContext(KernelContext& c, const Node& node, uint64_t seq, bool live,
			const VpSkeleton::LaneJobContext* job, uint32_t missing)
		{
			c.gpu = m_gpu;
			c.dispatch = m_dispatch;
			c.id = node.id;
			c.seq = seq;
			c.dims = m_dims;
			c.live = live;
			c.missing = missing;
			const auto it = m_products.find(node.id);
			if (it == m_products.end() || !it->second.tex)
				return false;
			c.own = it->second.tex.get();
			c.timer = it->second.timer;
			c.inCount = node.inputCount;
			for (size_t k = 0; k < node.inputCount && k < c.in.size(); ++k)
			{
				VpLaneProducts::GpuInput& in = c.in[k];
				in = VpLaneProducts::GpuInput();
				in.id = node.inputs[k].product;
				const uint64_t s = seq >= node.inputs[k].lag ? seq - node.inputs[k].lag : 0u;
				const auto pt = m_products.find(in.id);
				in.tex = pt == m_products.end() ? nullptr : pt->second.tex.get();
				if (job != nullptr)
				{
					// Lane: the pins the runner made (ring slot or live buffer of the input frame).
					if (job->inputs != nullptr && k < job->inputs->count && job->inputs->sets[k] != nullptr)
					{
						in.live = job->inputs->liveIndex[k];
						in.ringSlot = in.live >= 0 ? -1 : int(job->inputs->sets[k]->SlotOf(job->inputs->seqs[k]));
						in.cpu = job->inputData[k];
						in.cpuBytes = in.cpu != nullptr && in.id == VpSkeleton::kP4SourceStats ? size_t(VpLaneProducts::kStatsBytes) : 0u;
						in.missing = (missing >> k) & 1u;
					}
				}
				else if (m_lane != nullptr && ((missing >> k) & 1u) == 0)
				{
					const VpSkeleton::AnalysisLane::ProductRead r = m_lane->ReadOf(in.id, s);
					if (r.state == VpSkeleton::AnalysisLane::ProductRead::Ring)
						in.ringSlot = r.info.index;
					else if (r.state == VpSkeleton::AnalysisLane::ProductRead::Live)
						in.live = r.info.index;
					in.cpu = r.cpu;
					in.cpuBytes = r.cpuBytes;
					in.missing = in.ringSlot < 0 && in.live < 0;
				}
				if (in.missing)
					c.missing |= 1u << k;
			}
			// History products (P12): the product's own frame seq - 1.
			if (it->second.history > 0 && seq > 0 && m_lane != nullptr)
			{
				const VpSkeleton::AnalysisLane::ProductRead r = m_lane->ReadOf(node.id, seq - 1);
				if (r.state == VpSkeleton::AnalysisLane::ProductRead::Ring)
					c.ownPrevRingSlot = r.info.index;
				else if (r.state == VpSkeleton::AnalysisLane::ProductRead::Live)
					c.ownPrevLive = r.info.index;
			}
			return true;
		}

		bool SkeletonGpuDispatcher::Dispatch(const VpSkeleton::LaneJobContext& job, uint32_t chunk, uint32_t chunkCount)
		{
			if (!Valid() || job.node == nullptr)
				return Fail("no GPU device");
			WatchAttributionIfEnabled();
			const Node& node = *job.node;
			VpLaneProducts::IProductKernel* k = KernelOf(node.id);
			if (!k)
				return Fail("no kernel for the product");
			KernelContext c;
			if (!BuildContext(c, node, job.seq, false, &job, 0u))
				return Fail("the product has no textures");
			for (size_t t = 0; t < c.out.size(); ++t)
				c.out[t] = c.own->Ring(size_t(job.ringSlot), t);
			while (c.outCount < c.out.size() && c.out[c.outCount])
				++c.outCount;
			if (c.outCount == 0)
				return Fail("the lane slot has no textures");
			++m_counters.laneChunks;
			ProductState& ps = m_products[node.id];
			PollTimer(ps);
			// FIX-S0-COST: the header's cost is the frame's pass SUM (the mean ms per frame over all timed passes), not the last single pass.
			if (job.header != nullptr && ps.cost.timedFrames != 0)
				job.header->costUs = uint32_t((std::min<double>)(ps.cost.MeanFrameMs() * 1000.0, 4294967295.0));

			const bool ownsTap = node.id == VpSkeleton::kP10GrainSplit;
			const uint32_t bands = ownsTap ? VpLaneProducts::TapBands(m_dims) : 0u;
			const uint32_t prefix = ownsTap ? bands + 1u : 0u;
			if (chunkCount != prefix + k->Steps(m_dims))
				return Fail("the lane plan does not match the product's passes");
			const VpLaneProducts::GpuInput* p4 = c.Input(VpSkeleton::kP4SourceStats);
			const RegionEvidence::Rect picture = VpLaneProducts::PictureFromStats(p4 && !p4->missing ? p4->cpu : nullptr,
				p4 ? p4->cpuBytes : 0u, int(m_dims.width), int(m_dims.height));
			if (ownsTap && chunk < prefix)
			{
				if (job.source == nullptr || !job.source->Valid())
					return Fail("the lane job has no source frame");
				Tap* t = TapFor(job.seq, chunk == 0);
				if (!t)
					return Fail("the tap plane is missing: " + m_tapError);
				if (chunk == 0)
				{
					// A new job for this seq (a graph swap hands an unfinished job to the next runner, possibly for the same
					// frame): the tap's upload progress restarts with it, and stays pinned until P10 finishes.
					t->bands = 0;
					t->tapLuma = job.source->luma;
					t->tapShift = job.source->msbShift;
					t->extendedDone = false;
					t->pinned = true;
				}
				if (chunk < bands)
				{
					if (t->bands != chunk)
					{
						t->pinned = false;
						return Fail("the tap bands are out of order");
					}
					const uint32_t rpb = VpLaneProducts::TapRowsPerBand(m_dims);
					const uint32_t y0 = chunk * rpb;
					if (!VpLaneProducts::UploadTapRows(m_gpu, t->tap, *job.source, y0, (std::min)(m_dims.height, y0 + rpb), m_scratch))
					{
						t->pinned = false;
						return Fail("the tap plane could not be uploaded");
					}
					++t->bands;
					++m_counters.tapBands;
					return true;
				}
				t->extendedDone = false;
				if (!EnsureTap(*t, *job.source, picture, ps.timer))
				{
					t->pinned = false;
					return false;
				}
				return true;
			}
			Tap* usedTap = nullptr;
			if (k->NeedsTap())
			{
				Tap* t = TapFor(job.seq, !ownsTap);
				if (!t)
					return Fail("the tap plane is missing: " + m_tapError);
				if (!ownsTap)
				{
					if (job.source == nullptr || !job.source->Valid())
						return Fail("the lane job has no source frame");
					if (!EnsureTap(*t, *job.source, picture, ps.timer))
						return false;
				}
				usedTap = t;
				c.tap = t->tap;
				c.extended = t->extendedDone ? t->extended : nullptr;
				c.picture = t->picture;
			}
			else
				c.picture = picture;
			if (!k->Step(c, chunk - prefix))
			{
				if (ownsTap && usedTap)
					usedTap->pinned = false;
				return Fail(c.error.empty() ? k->LastError() : c.error);
			}
			if (chunk + 1u == chunkCount)
			{
				if (ownsTap && usedTap)
					usedTap->pinned = false;
				ps.cost.NoteFrameDone();
			}
			return true;
		}

		bool SkeletonGpuDispatcher::DispatchLive(const Node& node, uint64_t seq, int liveIndex, const VpSkeleton::Header&,
			uint32_t missingInputs)
		{
			if (!Valid())
				return Fail("no GPU device");
			WatchAttributionIfEnabled();
			VpLaneProducts::IProductKernel* k = KernelOf(node.id);
			if (!k)
				return Fail("no kernel for the product");
			KernelContext c;
			if (!BuildContext(c, node, seq, true, nullptr, missingInputs))
				return Fail("the product has no textures");
			for (size_t t = 0; t < c.out.size(); ++t)
				c.out[t] = c.own->Live(size_t(liveIndex), t);
			while (c.outCount < c.out.size() && c.out[c.outCount])
				++c.outCount;
			if (c.outCount == 0)
				return Fail("the live buffer has no textures");
			ProductState& ps = m_products[node.id];
			PollTimer(ps);
			const VpLaneProducts::GpuInput* p4 = c.Input(VpSkeleton::kP4SourceStats);
			c.picture = VpLaneProducts::PictureFromStats(p4 && !p4->missing ? p4->cpu : nullptr, p4 ? p4->cpuBytes : 0u,
				int(m_dims.width), int(m_dims.height));
			Tap* usedTap = nullptr;
			if (k->NeedsTap())
			{
				VpSkeleton::SourceFrameView view;
				const bool have = m_lane != nullptr ? m_lane->SourceOf(seq, &view) : (m_liveSource && m_liveSource(seq, &view));
				if (!have)
					return Fail("the live fill has no source frame");
				Tap* t = TapFor(seq, true);
				if (!t)
					return Fail("the tap plane could not be made: " + m_tapError);
				if (!EnsureTap(*t, view, c.picture, ps.timer))
				{
					if (node.id == VpSkeleton::kP10GrainSplit)
						t->pinned = false;
					return Fail(m_error.empty() ? "the tap plane could not be made" : m_error);
				}
				usedTap = t;
				c.tap = t->tap;
				c.extended = t->extended;
				c.picture = t->picture;
			}
			const uint32_t steps = k->Steps(m_dims);
			for (uint32_t i = 0; i < steps; ++i)
				if (!k->Step(c, i))
				{
					if (usedTap && node.id == VpSkeleton::kP10GrainSplit)
						usedTap->pinned = false;
					return Fail(c.error.empty() ? k->LastError() : c.error);
				}
			if (usedTap && node.id == VpSkeleton::kP10GrainSplit)
				usedTap->pinned = false;
			++m_counters.liveFills;
			ps.cost.NoteFrameDone();
			return true;
		}

		GpuProductCost SkeletonGpuDispatcher::CostOf(VpSkeleton::ProductId id) const
		{
			const auto it = m_products.find(id);
			return it == m_products.end() ? GpuProductCost() : it->second.cost;
		}

		double SkeletonGpuDispatcher::MeasuredMs(VpSkeleton::ProductId id) const
		{
			// FIX-S0-COST: the mean device ms per FRAME over all timed passes (the sum of a frame's passes), not the last single pass.
			const auto it = m_products.find(id);
			return it == m_products.end() ? 0.0 : it->second.cost.MeanFrameMs();
		}

		double SkeletonGpuDispatcher::LastPassMs(VpSkeleton::ProductId id) const
		{
			const auto it = m_products.find(id);
			return it == m_products.end() ? 0.0 : double(it->second.lastNs) / 1.0e6;
		}

		uint64_t SkeletonGpuDispatcher::PrivateBytes(VpSkeleton::Dims dims, bool laneForm, size_t targetTapSlots) const
	{
		const size_t currentSlots = m_taps.empty() ? kTapRing : m_taps.size();
		const size_t tapSlots = targetTapSlots != 0
			? (std::min<size_t>)(kMaxTapRing, (std::max<size_t>)(kTapRing, targetTapSlots))
			: currentSlots;
		uint64_t total = uint64_t(tapSlots) * 2u * uint64_t(dims.width) * dims.height * 2u;
		for (const std::unique_ptr<VpLaneProducts::IProductKernel>& k : m_kernels)
			total += k->PrivateBytes(dims, laneForm);
		return total;
	}

		uint64_t SkeletonGpuDispatcher::ActualPrivateBytes(VpSkeleton::Dims dims) const
	{
		uint64_t held = 0;
		if (dims.width == m_dims.width && dims.height == m_dims.height)
		{
			const uint64_t planeBytes = uint64_t(dims.width) * dims.height * 2u;
			for (const Tap& t : m_taps)
			{
				if (t.tap != nullptr)
					held += planeBytes;
				if (t.extended != nullptr)
					held += planeBytes;
			}
		}
		for (const std::unique_ptr<VpLaneProducts::IProductKernel>& k : m_kernels)
			held += k->ActualPrivateBytes(dims);
		return held;
	}

	std::vector<FallbackCatalog::Decision> SkeletonGpuDispatcher::Fallbacks() const
		{
			std::vector<FallbackCatalog::Decision> out;
			for (const auto& kv : m_products)
				if (!kv.second.refusal.empty())
					out.push_back(FallbackCatalog::Engaged("skeleton_gpu", kv.second.refusal + ": the product is unavailable (readers fall back)"));
			for (const auto& k : m_kernels)
				for (const FallbackCatalog::Decision& d : k->Fallbacks())
					out.push_back(d);
			return out;
		}
	}
}
