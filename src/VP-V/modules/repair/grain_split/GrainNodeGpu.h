// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <VP-V/core/GpuAllocator.h>
#include <VP-V/modules/control/gpu_memory/PlTexBackend.h>
#include <VP-V/modules/repair/grain_split/GrainNode.h>
#include <VP-V/modules/repair/grain_split/GrainSplitGpu.h>

#include <functional>

// The GPU side of the GrainNode (GrainNode.h): the compute interface GrainNode calls (`Fill`) implemented by the ONE
// implementation of the grain split, GrainSplit::Gpu::Splitter::Compute. The lane producer (LanePrecompute, a ring
// slot) and the render's single live fill (Acquire, a live-pool buffer) both come through Fill, so there is no second
// implementation: what a consumer reads is the same whichever path made it.
//
// The slot textures come from the node's ProductTextures (ring buffers for the lane, live buffers for the live fill),
// laid out by GrainLayout(storage); `TargetsFor` maps a GrainTarget to them.
namespace GrainSplit
{
	namespace Gpu
	{
		// The layout of one P10 buffer. Full: split rgba32f + blocks rgba32f; Compact: A rg16f + B rg16 + blocks rg32f
		// (blocks at the 16 px block edge: shift 4, the declaration's planning figure).
		inline VpSkeleton::ProductTexLayout GrainLayout(Storage storage)
		{
			using VpSkeleton::TexFormat;
			VpSkeleton::ProductTexLayout l;
			auto term = [&l](uint8_t shift, TexFormat f) {
				VpSkeleton::TexTerm t;
				t.shift = shift;
				t.fmt = f;
				l.terms.push_back(t);
			};
			if (storage == Storage::Compact)
			{
				term(0, TexFormat::RG16F);
				term(0, TexFormat::RG16);
				term(4, TexFormat::RG32F);
			}
			else
			{
				term(0, TexFormat::RGBA32F);
				term(4, TexFormat::RGBA32F);
			}
			return l;
		}

		// The output textures of ring slot / live buffer `target` of a node's ProductTextures.
		inline Targets TargetsFor(const VpSkeleton::ProductTextures<VpSkeleton::PlTexBackend>& tex, const GrainTarget& target, Storage storage)
		{
			Targets t;
			const size_t i = static_cast<size_t>(target.index);
			auto at = [&](size_t term) { return target.live ? tex.Live(i, term) : tex.Ring(i, term); };
			t.split = at(0);
			if (storage == Storage::Compact)
			{
				t.splitB = at(1);
				t.blocks = at(2);
			}
			else
				t.blocks = at(1);
			return t;
		}

		// The Frame a reader gets for a node slot that holds frame `seq` (computed by either path): the slot's textures
		// plus the geometry and the stamps the product was made with (unit version, source frame).
		inline Frame FrameOf(const VpSkeleton::ProductTextures<VpSkeleton::PlTexBackend>& tex, const GrainTarget& target, Storage storage,
			const GrainDesc& desc, uint64_t seq)
		{
			const Targets t = TargetsFor(tex, target, storage);
			Frame f;
			f.split = t.split;
			f.splitB = t.splitB;
			f.blocks = t.blocks;
			f.w = desc.w;
			f.h = desc.h;
			f.B = desc.B;
			f.bw = (desc.w + desc.B - 1) / desc.B;
			f.bh = (desc.h + desc.B - 1) / desc.B;
			f.scale = desc.scale;
			f.codeUnit = desc.codeUnit;
			f.component = desc.component;
			f.storage = storage;
			f.seq = seq;
			f.version = GrainSplit::kUnitVersion;
			return f;
		}

		// GrainNode's Compute: sets the source of the frame being produced, then Fill is called by the lane or by the
		// first consumer. Everything goes through Splitter::Compute.
		class SplitterFill
		{
		public:
			using TargetsFn = std::function<Targets(const GrainTarget&)>;

			SplitterFill(Splitter& splitter, pl_gpu gpu, pl_dispatch dispatch) : m_splitter(&splitter), m_gpu(gpu), m_dispatch(dispatch) {}

			struct Source
			{
				pl_tex plane = nullptr;
				int component = 0;
				double codeUnit = 0.0;
				double scale = 1.0;
				int B = 16;
				Params params;
				Storage storage = Storage::Full;   // REQUESTED: the Splitter may fall back to Full (ChooseStorage)
				VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend>* arena = nullptr;
			};

			void SetSource(const Source& s) { m_source = s; }
			void SetTargets(TargetsFn f) { m_targets = std::move(f); }

			// GrainNode's compute interface.
			bool Fill(uint64_t seq, const GrainTarget& target)
			{
				++m_fills;
				Options o;
				o.storage = m_source.storage;
				o.arena = m_source.arena;
				o.seq = seq;
				if (m_targets)
					o.targets = m_targets(target);
				return m_splitter->Compute(m_gpu, m_dispatch, m_source.plane, m_source.component, m_source.codeUnit, m_source.scale, m_source.B,
					m_source.params, o);
			}

			unsigned Fills() const { return m_fills; }
			const Splitter& Computer() const { return *m_splitter; }

		private:
			Splitter* m_splitter;
			pl_gpu m_gpu;
			pl_dispatch m_dispatch;
			Source m_source;
			TargetsFn m_targets;
			unsigned m_fills = 0;
		};
	}
}
