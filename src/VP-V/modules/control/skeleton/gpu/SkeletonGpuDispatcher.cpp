// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// Built without the precompiled header (libplacebo + VP-V headers only).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <VP-V/modules/control/skeleton/gpu/SkeletonGpuDispatcher.h>

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

			bool CreateR16(pl_gpu gpu, pl_tex& tex, int w, int h, bool readable)
			{
				const pl_fmt fmt = VpSkeleton::PlTexBackend::FindFormat(gpu, VpSkeleton::TexFormat::R16);
				if (!fmt)
					return false;
				pl_tex_params tp{};
				tp.w = w;
				tp.h = h;
				tp.format = fmt;
				tp.sampleable = true;
				tp.storable = true;
				tp.host_writable = true;
				tp.host_readable = readable;
				return pl_tex_recreate(gpu, &tex, &tp);
			}
		}

		SkeletonGpuDispatcher::SkeletonGpuDispatcher(pl_gpu gpu, bool hostReadable)
			: m_gpu(gpu), m_backend(gpu, hostReadable)
		{
			if (m_gpu)
				m_dispatch = pl_dispatch_create(m_gpu->log, m_gpu);
		}

		SkeletonGpuDispatcher::~SkeletonGpuDispatcher()
		{
			Release();
			if (m_dispatch)
				pl_dispatch_destroy(&m_dispatch);
		}

		void SkeletonGpuDispatcher::AddKernel(std::unique_ptr<VpLaneProducts::IProductKernel> kernel)
		{
			if (kernel)
				m_kernels.push_back(std::move(kernel));
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
			const VpSkeleton::TierPlan& plan, Dims dims, uint32_t settingsGen)
		{
			if (!Valid())
				return Fail("no GPU device for the analysis products");
			uint64_t key = Mix(Mix(0xCBF29CE484222325ull, dims.width), dims.height);
			for (const Node& n : graph.nodes)
				if (n.backend == VpSkeleton::Backend::GpuCompute && KernelOf(n.id))
					key = Mix(Mix(Mix(Mix(key, n.id), n.ring), n.livePool), plan.Laned(n.id) ? 1u : 0u);
			const bool sizeChanged = dims.width != m_dims.width || dims.height != m_dims.height;
			if (key == m_graphKey && settingsGen == m_settingsGen && !m_products.empty())
				return true;
			const bool rebuildTextures = key != m_graphKey || m_products.empty();
			m_graphKey = key;
			m_settingsGen = settingsGen;
			bool ok = true;
			if (rebuildTextures)
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
					ps.tex.reset(new GpuProductTextures(m_backend));
					if (!ps.tex->Allocate(n, reg.Get(n.id), k->Layout(dims), dims, plan))
					{
						ps.refusal = ps.tex->LastRefusal().empty() ? std::string("video memory could not be allocated")
						                                           : ps.tex->LastRefusal();
						ps.tex.reset();
						ok = false;
						continue;
					}
					++m_counters.allocations;
					ps.timer = pl_timer_create(m_gpu);
				}
			}
			if (sizeChanged)
			{
				for (Tap& t : m_taps)
				{
					t.used = false;
					if (t.tap)
						pl_tex_destroy(m_gpu, &t.tap);
					if (t.extended)
						pl_tex_destroy(m_gpu, &t.extended);
				}
				m_dims = dims;
			}
			for (const auto& k : m_kernels)
				if (m_products.count(k->Product()) != 0 && !k->Prepare(m_gpu, m_dispatch, dims))
				{
					m_products[k->Product()].refusal = "preparation failed: " + k->LastError();
					m_products[k->Product()].tex.reset();
					ok = false;
				}
			if (!ok)
				m_error = "an analysis product is unavailable (see the status)";
			return ok;
		}

		void SkeletonGpuDispatcher::FlushReleases()
		{
			m_counters.releasesDone += m_graveyard.size();
			m_graveyard.clear();   // ~ProductTextures destroys through the backend
		}

		void SkeletonGpuDispatcher::Release()
		{
			FlushReleases();
			for (auto& kv : m_products)
			{
				if (kv.second.tex)
					++m_counters.releasesDone;
				kv.second.tex.reset();
				if (kv.second.timer && m_gpu)
					pl_timer_destroy(m_gpu, &kv.second.timer);
			}
			m_products.clear();
			for (Tap& t : m_taps)
			{
				t.used = false;
				if (t.tap && m_gpu)
					pl_tex_destroy(m_gpu, &t.tap);
				if (t.extended && m_gpu)
					pl_tex_destroy(m_gpu, &t.extended);
			}
			for (const auto& k : m_kernels)
				if (m_gpu)
					k->Release(m_gpu);
			m_graphKey = 0;
			m_dims = Dims{ 0, 0 };
		}

		const GpuProductTextures* SkeletonGpuDispatcher::TexturesOf(ProductId id) const
		{
			const auto it = m_products.find(id);
			return it == m_products.end() ? nullptr : it->second.tex.get();
		}

		bool SkeletonGpuDispatcher::Available(ProductId id) const
		{
			const auto it = m_products.find(id);
			return it != m_products.end() && it->second.tex != nullptr;
		}

		bool SkeletonGpuDispatcher::HoldsVideoMemory() const
		{
			if (AllocatedBytes() != 0)
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
			m_backend.gpu = nullptr;
			m_dispatch = nullptr;
			m_lane = nullptr;
			for (Tap& t : m_taps)
			{
				t.tap = nullptr;
				t.extended = nullptr;
				t.used = false;
			}
			for (auto& kv : m_products)
				kv.second.timer = nullptr;
		}

		uint64_t SkeletonGpuDispatcher::AllocatedBytes() const
		{
			uint64_t b = 0;
			for (const auto& kv : m_products)
				if (kv.second.tex)
					b += kv.second.tex->Bytes();
			return b;
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
			if (!create)
				return nullptr;
			Tap* victim = &m_taps[0];
			for (Tap& t : m_taps)
			{
				if (!t.used)
				{
					victim = &t;
					break;
				}
				if (t.seq < victim->seq)
					victim = &t;
			}
			const int w = int(m_dims.width), h = int(m_dims.height);
			const bool readable = m_backend.hostReadable;
			if (!CreateR16(m_gpu, victim->tap, w, h, readable) || !CreateR16(m_gpu, victim->extended, w, h, readable))
				return nullptr;
			victim->used = true;
			victim->seq = seq;
			victim->bands = 0;
			victim->extendedDone = false;
			return victim;
		}

		bool SkeletonGpuDispatcher::EnsureTap(Tap& t, const VpSkeleton::SourceFrameView& view, const RegionEvidence::Rect& picture)
		{
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
			if (!t.extendedDone)
			{
				t.picture = picture;
				if (!VpLaneProducts::ExtendTap(m_dispatch, t.tap, t.extended, picture))
					return Fail("the extend pass failed");
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
				ps.lastNs = ns;
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
			if (job.header != nullptr && ps.lastNs != 0)
				job.header->costUs = uint32_t((std::min<uint64_t>)(ps.lastNs / 1000u, 0xFFFFFFFFull));

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
					return Fail("the tap plane is missing (evicted or not created)");
				if (chunk == 0)
				{
					// A new job for this seq (a graph swap hands an unfinished job to the next runner, possibly for the same
					// frame): the tap's upload progress restarts with it.
					t->bands = 0;
					t->extendedDone = false;
				}
				if (chunk < bands)
				{
					if (t->bands != chunk)
						return Fail("the tap bands are out of order");
					const uint32_t rpb = VpLaneProducts::TapRowsPerBand(m_dims);
					const uint32_t y0 = chunk * rpb;
					if (!VpLaneProducts::UploadTapRows(m_gpu, t->tap, *job.source, y0, (std::min)(m_dims.height, y0 + rpb), m_scratch))
						return Fail("the tap plane could not be uploaded");
					++t->bands;
					++m_counters.tapBands;
					return true;
				}
				t->extendedDone = false;
				return EnsureTap(*t, *job.source, picture);
			}
			if (k->NeedsTap())
			{
				Tap* t = TapFor(job.seq, !ownsTap);
				if (!t)
					return Fail("the tap plane is missing (evicted or not created)");
				if (!ownsTap)
				{
					if (job.source == nullptr || !job.source->Valid())
						return Fail("the lane job has no source frame");
					if (!EnsureTap(*t, *job.source, picture))
						return false;
				}
				c.tap = t->tap;
				c.extended = t->extendedDone ? t->extended : nullptr;
				c.picture = t->picture;
			}
			else
				c.picture = picture;
			if (!k->Step(c, chunk - prefix))
				return Fail(c.error.empty() ? k->LastError() : c.error);
			return true;
		}

		bool SkeletonGpuDispatcher::DispatchLive(const Node& node, uint64_t seq, int liveIndex, const VpSkeleton::Header&,
			uint32_t missingInputs)
		{
			if (!Valid())
				return Fail("no GPU device");
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
			PollTimer(m_products[node.id]);
			const VpLaneProducts::GpuInput* p4 = c.Input(VpSkeleton::kP4SourceStats);
			c.picture = VpLaneProducts::PictureFromStats(p4 && !p4->missing ? p4->cpu : nullptr, p4 ? p4->cpuBytes : 0u,
				int(m_dims.width), int(m_dims.height));
			if (k->NeedsTap())
			{
				VpSkeleton::SourceFrameView view;
				const bool have = m_lane != nullptr ? m_lane->SourceOf(seq, &view) : (m_liveSource && m_liveSource(seq, &view));
				if (!have)
					return Fail("the live fill has no source frame");
				Tap* t = TapFor(seq, true);
				if (!t || !EnsureTap(*t, view, c.picture))
					return Fail(m_error.empty() ? "the tap plane could not be made" : m_error);
				c.tap = t->tap;
				c.extended = t->extended;
				c.picture = t->picture;
			}
			const uint32_t steps = k->Steps(m_dims);
			for (uint32_t i = 0; i < steps; ++i)
				if (!k->Step(c, i))
					return Fail(c.error.empty() ? k->LastError() : c.error);
			++m_counters.liveFills;
			return true;
		}

		double SkeletonGpuDispatcher::MeasuredMs(VpSkeleton::ProductId id) const
		{
			const auto it = m_products.find(id);
			return it == m_products.end() ? 0.0 : double(it->second.lastNs) / 1.0e6;
		}

		uint64_t SkeletonGpuDispatcher::PrivateBytes(VpSkeleton::Dims dims, bool laneForm) const
	{
		uint64_t total = uint64_t(kTapRing) * 2u * uint64_t(dims.width) * dims.height * 2u;
		for (const std::unique_ptr<VpLaneProducts::IProductKernel>& k : m_kernels)
			total += k->PrivateBytes(dims, laneForm);
		return total;
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
