// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <VP-V/core/GpuAllocator.h>
#include <VP-V/modules/control/gpu_memory/PlTexBackend.h>
#include <VP-V/modules/repair/grain_split/GrainNode.h>
#include <VP-V/modules/repair/grain_split/GrainSplitGpu.h>

#include <atomic>
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
// to its owning thread with Bind() (call it on that thread before the first Fill, e.g. at lane-thread start) or, without
// it, to the thread of its first Fill; the binding is an atomic compare-exchange (VpSkeleton::ThreadBinding), so two
// threads racing for the first use cannot both win. A Fill from another thread, or with an arena bound to another
// thread, is refused (counted, LastRefusal(), FillDecision() visible) and never touches the GPU; a refusal from another
// thread touches only atomics (it never writes the owner's state). The two instances write different
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
		// GrainLayout / GrainDecl live in GrainNode.h (CPU: the protocol tests check them without a GPU).
		using GrainSplit::GrainLayout;
		using GrainSplit::GrainDecl;

		// The node's storage, layout AND declaration: the REQUESTED storage passed through ChooseStorage for the node's plane
		// (a refused Compact becomes Full, with the visible StorageFallback decision), the layout of what was granted and the
		// P10 declaration of what was granted (finisher MUST 2). The host plans the governor with `decl` (or with
		// GrainRegistryFor(choice.storage)), allocates the node's ProductTextures with (`decl`, `layout`) and gives
		// `choice.storage` to SplitterFill::SetTargets. Declaring the REQUESTED storage would make a downgraded Compact
		// (Full layout, 16 B/px) fail every allocation against an 8 B/px declaration: never do that.
		struct NodeLayout
		{
			StorageChoice choice;
			VpSkeleton::ProductTexLayout layout;
			VpSkeleton::ProductDecl decl;
		};
		inline NodeLayout ChooseNodeLayout(Storage requested, pl_fmt planeFormat, double codeUnit, const GrainDesc& desc)
		{
			NodeLayout n;
			n.choice = ChooseStorage(requested, planeFormat, codeUnit);
			n.layout = GrainLayout(n.choice.storage, desc);
			n.decl = GrainDecl(n.choice.storage);
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

			// Binds this instance to the CALLING thread (its owner) when still unbound. True when the caller owns it. Call it on
			// the owning thread before the first Fill; otherwise the first Fill binds (atomically).
			bool Bind() { return m_binding.BindIfUnbound(); }
			bool BoundToCaller() const { return m_binding.BoundToCaller(); }

			// Owner thread only (like Fill).
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
				if (!m_binding.BindIfUnbound())
				{
					// Another thread owns this instance: touch only atomics (never the owner's string / source / targets).
					m_crossThread.fetch_add(1);
					m_crossPending.store(true);
					return false;
				}
				m_refusal.clear();
				m_crossPending.store(false);
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
			unsigned Refusals() const { return m_refusals + m_crossThread.load(); }
			unsigned CrossThreadRefusals() const { return m_crossThread.load(); }
			// Owner thread: the last refusal; a refused call from another thread since the owner's last Fill shows first.
			const std::string& LastRefusal() const { return m_crossPending.load() ? CrossThreadText() : m_refusal; }
			// Visible while the last Fill was refused (no silent fallbacks); released by the next accepted Fill.
			FallbackCatalog::Decision FillDecision() const
			{
				const std::string& why = LastRefusal();
				return why.empty() ? FallbackCatalog::Released("grain split fill")
					: FallbackCatalog::Engaged("grain split fill", "grain split not computed: " + why);
			}
			const Splitter& Computer() const { return *m_splitter; }

		private:
			static const std::string& CrossThreadText()
			{
				static const std::string s = "called from another thread (one SplitterFill per thread)";
				return s;
			}
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
			VpSkeleton::ThreadBinding m_binding;
			std::atomic<unsigned> m_crossThread{ 0 };
			std::atomic<bool> m_crossPending{ false };
			unsigned m_fills = 0;
			unsigned m_refusals = 0;
			std::string m_refusal;
		};
	}
}
