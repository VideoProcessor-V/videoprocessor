// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <VP-V/core/GpuAllocator.h>
#include <VP-V/modules/control/gpu_memory/PlTexBackend.h>
#include <VP-V/modules/repair/grain_split/GrainNode.h>
#include <VP-V/modules/repair/grain_split/GrainSplitGpu.h>

#include <functional>
#include <string>
#include <thread>

// The GPU side of the GrainNode (GrainNode.h): the compute interface GrainNode calls (`Fill`) implemented by the ONE
// implementation of the grain split, GrainSplit::Gpu::Splitter::Compute. The lane producer (LanePrecompute, a ring
// slot) and the render's single live fill (Acquire, a live-pool buffer) both come through Fill, so there is no second
// implementation: what a consumer reads is the same whichever path made it.
//
// ONE CODE PATH, ONE INSTANCE PER THREAD (audit 2 A). A Splitter holds mutable state (its last Frame, error and storage
// note, private intermediates) and a pl_dispatch is not thread-safe, so the lane thread and the render thread each own
// their own SplitterFill + Splitter + pl_dispatch (and, if any, their own ScratchArena: the render's arena is
// render-thread only and the lane's arena is lane-thread only, each a separate governor-counted pool). SplitterFill binds
// to the thread of its first Fill; a Fill from another thread, or with an arena bound to another thread, is refused
// (counted, LastRefusal(), FillDecision() visible) and never touches the GPU. The two instances write different
// textures (ring buffers for the lane, live buffers for the render), so nothing aliases.
//
// The slot textures come from the node's ProductTextures, laid out by GrainLayout(storage, desc) for the storage that
// ChooseStorage actually grants (ChooseNodeLayout): a Compact request refused for this plane is laid out as Full, so
// the targets always match what Compute writes (audit 2 C). The block grid is sized from the node's real block edge
// (ceil(w / B) x ceil(h / B)), not from a fixed edge (audit 2 B).
namespace GrainSplit
{
	namespace Gpu
	{
		// The layout of one P10 buffer for the plane `desc` describes. Full: split rgba32f + blocks rgba32f; Compact: A rg16f +
		// B rg16 + blocks rgba32f (N, e3, e1, e2). The block grid is ABSOLUTE: ceil(w / B) x ceil(h / B) of the node's real
		// block edge (the declaration counts it at the smallest edge, 16 px, an upper bound: ProductTexLayout::MatchesDecl).
		inline VpSkeleton::ProductTexLayout GrainLayout(Storage storage, const GrainDesc& desc)
		{
			using VpSkeleton::TexFormat;
			VpSkeleton::ProductTexLayout l;
			auto term = [&l](uint8_t shift, TexFormat f, uint32_t absW, uint32_t absH) {
				VpSkeleton::TexTerm t;
				t.shift = shift;
				t.fmt = f;
				t.absW = absW;
				t.absH = absH;
				l.terms.push_back(t);
			};
			const int B = desc.B > 0 ? desc.B : 1;
			const uint32_t bw = static_cast<uint32_t>((desc.w + B - 1) / B), bh = static_cast<uint32_t>((desc.h + B - 1) / B);
			if (storage == Storage::Compact)
			{
				term(0, TexFormat::RG16F, 0, 0);
				term(0, TexFormat::RG16, 0, 0);
				term(4, TexFormat::RGBA32F, bw, bh);
			}
			else
			{
				term(0, TexFormat::RGBA32F, 0, 0);
				term(4, TexFormat::RGBA32F, bw, bh);
			}
			return l;
		}

		// The node's storage and layout: the REQUESTED storage passed through ChooseStorage for the node's plane (a refused
		// Compact becomes Full, with the visible StorageFallback decision), and the layout of what was granted. The host
		// allocates the node's ProductTextures from `layout` and gives `choice.storage` to SplitterFill::SetTargets.
		struct NodeLayout
		{
			StorageChoice choice;
			VpSkeleton::ProductTexLayout layout;
		};
		inline NodeLayout ChooseNodeLayout(Storage requested, pl_fmt planeFormat, double codeUnit, const GrainDesc& desc)
		{
			NodeLayout n;
			n.choice = ChooseStorage(requested, planeFormat, codeUnit);
			n.layout = GrainLayout(n.choice.storage, desc);
			return n;
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
		// first consumer. Everything goes through Splitter::Compute. One instance per thread (see the top of this file).
		class SplitterFill
		{
		public:
			using TargetsFn = std::function<Targets(const GrainTarget&)>;

			SplitterFill(Splitter& splitter, pl_gpu gpu, pl_dispatch dispatch) : m_splitter(&splitter), m_gpu(gpu), m_dispatch(dispatch) {}
			SplitterFill(const SplitterFill&) = delete;
			SplitterFill& operator=(const SplitterFill&) = delete;

			struct Source
			{
				pl_tex plane = nullptr;
				int component = 0;
				double codeUnit = 0.0;
				double scale = 1.0;
				int B = 16;
				Params params;
				Storage storage = Storage::Full;   // REQUESTED; Fill passes ChooseStorage's grant, which must equal the layout's
				// This thread's own arena (another thread's is refused). Null = the Splitter's own intermediates.
				VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend>* arena = nullptr;
			};

			void SetSource(const Source& s) { m_source = s; }
			// `layoutStorage`: the storage the targets were allocated for (NodeLayout::choice.storage).
			void SetTargets(TargetsFn f, Storage layoutStorage)
			{
				m_targets = std::move(f);
				m_layoutStorage = layoutStorage;
			}

			// GrainNode's compute interface. False (and no GPU work) when refused: another thread, another thread's arena, or
			// a plane whose granted storage no longer matches the targets' layout (the host lays the node out again).
			bool Fill(uint64_t seq, const GrainTarget& target)
			{
				m_refusal.clear();
				if (!m_threadBound)
				{
					m_thread = std::this_thread::get_id();
					m_threadBound = true;
				}
				if (std::this_thread::get_id() != m_thread)
					return Refuse("called from another thread (one SplitterFill per thread)");
				if (m_source.arena && m_source.arena->BoundToOtherThread())
					return Refuse("the scratch arena belongs to another thread");
				const StorageChoice granted = ChooseStorage(m_source.storage, m_source.plane ? m_source.plane->params.format : nullptr, m_source.codeUnit);
				if (m_targets && granted.storage != m_layoutStorage)
					return Refuse("the plane's storage changed: the node must be laid out again");
				++m_fills;
				Options o;
				o.storage = m_targets ? m_layoutStorage : m_source.storage;
				o.arena = m_source.arena;
				o.seq = seq;
				if (m_targets)
					o.targets = m_targets(target);
				return m_splitter->Compute(m_gpu, m_dispatch, m_source.plane, m_source.component, m_source.codeUnit, m_source.scale, m_source.B,
					m_source.params, o);
			}

			unsigned Fills() const { return m_fills; }
			unsigned Refusals() const { return m_refusals; }
			const std::string& LastRefusal() const { return m_refusal; }
			// Visible while the last Fill was refused (no silent fallbacks); released by the next accepted Fill.
			FallbackCatalog::Decision FillDecision() const
			{
				return m_refusal.empty() ? FallbackCatalog::Released("grain split fill")
					: FallbackCatalog::Engaged("grain split fill", "grain split not computed: " + m_refusal);
			}
			const Splitter& Computer() const { return *m_splitter; }

		private:
			bool Refuse(const char* why)
			{
				m_refusal = why;
				++m_refusals;
				return false;
			}

			Splitter* m_splitter;
			pl_gpu m_gpu;
			pl_dispatch m_dispatch;
			Source m_source;
			TargetsFn m_targets;
			Storage m_layoutStorage = Storage::Full;
			std::thread::id m_thread;
			bool m_threadBound = false;
			unsigned m_fills = 0;
			unsigned m_refusals = 0;
			std::string m_refusal;
		};
	}
}
