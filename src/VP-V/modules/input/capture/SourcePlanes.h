// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// L-CORE deliverable 5 (lead8/iface-L-CORE.md s6; refs/118 D5 Phase A, first slice; lead 8 ruling 2026-11-14 05:2x): the P13
// SOURCE-PLANE PROVIDER. P13 is declared "VIEWS of the unpacked planes of the queued frames (owned by queue/upload): 0 extra
// bytes" (core/Products.h P13; ProductCatalog unit "input/capture (queue + early upload)"). This slice serves the planes that
// are ON THE RENDER DEVICE in the current render frame, through Acquire(kP13SourceRing, seq, Gpu, VariantKey{plane}):
//   (a) the frame being rendered: its pl_frame planes as the host hands them to the EXISTING Entrance phase (FrameContext::image);
//   (b) any other frame whose planes a VP-V render-device uploader offers in this render frame (SourcePlanes::Offer; e.g. the
//       matcher's B upload in VP-V code; no Bill line).
// The queued-AHEAD ring (packed early-upload slots, refs/118 Phase A/B: needs the unpack kernel and RendererExtension.h lines)
// stays with the D5 lane; it will serve frames ahead behind the same key without changing this interface.
//
// Contract: a VIEW (no copy, no texture made, 0 bytes, no ledger grant), render thread, FRAME-SCOPED: valid on the render
// thread until the end of the render frame it was acquired in; a fill that needs the plane later DERIVES its own product in that
// frame. Offers are dropped at the next Begin of the host. Variant axes: plane only (0 or 1 = the first plane: luma of a YUV
// source; 2 = the second plane; 3 = the third); stage 0 only. Declines say why (Acquire's LastCause / Fallbacks entry):
// "seq not offered this frame", "plane absent", "RGB source has no luma plane", "no texture".
// Original pathway: the skeleton gives no memo there, so nothing reaches the fill; the Entrance observer records nothing then
// (the hub fact). Cost when nobody asks: one table write per Entrance (four pointers), nothing allocated per frame after the first.

#include <VP-V/core/SkeletonAcquire.h>
#include <VP-V/core/hub/ExtensionHub.h>
#include <VP-V/core/hub/HostSlots.h>
#include <VP-V/modules/control/skeleton/SkeletonFacts.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace Vpv
{
	namespace Capture
	{
		namespace RE = RendererExtension;

		// One frame's planes on the render device. `tex[i]` are the host's / uploader's textures (not owned).
		struct SourcePlaneSet
		{
			uint64_t seq = 0;
			int64_t generation = 0;
			int planes = 0;
			std::array<pl_tex, 4> tex{ { nullptr, nullptr, nullptr, nullptr } };
			std::array<int, 4> components{ { 0, 0, 0, 0 } };
			pl_color_repr repr{};
			bool rgb = false;         // repr.sys == RGB: no luma plane
			bool fullRange = false;   // repr.levels == FULL
			const char* origin = "";  // "render frame" (Entrance image) or the offering unit
		};

		class SourcePlanes
		{
		public:
			static constexpr const char* kUnit = "input/capture (queue + early upload)";   // the catalog's P13 owner (ProductCatalog.h)
			static constexpr size_t kMaxOffers = 4;   // frames one render frame may offer (A, B and two spare)

			// Render thread. The planes of frame `set.seq` are on the render device for this render frame. A second offer of one
			// (seq, generation) replaces the first. False = the host table is full or the set has no texture.
			static bool Offer(const void* host, const SourcePlaneSet& set)
			{
				if (!host || set.planes <= 0 || set.tex[0] == nullptr)
					return false;
				std::shared_ptr<HostOffers> h = Table().Find(host);
				if (!h)
				{
					h = std::make_shared<HostOffers>(host);
					if (!Table().Put(host, h))
						h = Table().Find(host);
					if (!h)
						return false;
				}
				std::lock_guard<std::mutex> lock(h->m);
				for (std::shared_ptr<const SourcePlaneSet>& e : h->offers)
					if (e && e->seq == set.seq && e->generation == set.generation)
					{
						e = std::make_shared<const SourcePlaneSet>(set);
						++h->offered;
						return true;
					}
				if (h->offers.size() >= kMaxOffers)
				{
					++h->refused;
					return false;
				}
				h->offers.push_back(std::make_shared<const SourcePlaneSet>(set));
				++h->offered;
				return true;
			}
			// The Entrance image of the frame being rendered (render thread; the SourcePlanesModule calls it).
			static bool OfferFrame(const void* host, uint64_t seq, int64_t generation, const pl_frame& image)
			{
				SourcePlaneSet s;
				s.seq = seq;
				s.generation = generation;
				s.planes = image.num_planes < 4 ? image.num_planes : 4;
				for (int i = 0; i < s.planes; ++i)
				{
					s.tex[size_t(i)] = image.planes[i].texture;
					s.components[size_t(i)] = image.planes[i].components;
				}
				s.repr = image.repr;
				s.rgb = image.repr.sys == PL_COLOR_SYSTEM_RGB;
				s.fullRange = image.repr.levels == PL_COLOR_LEVELS_FULL;
				s.origin = "render frame";
				return Offer(host, s);
			}
			// The host's next render frame begins: every offer of the previous one is dropped (render thread).
			static void BeginFrame(const void* host)
			{
				if (const std::shared_ptr<HostOffers> h = Table().Find(host))
				{
					std::lock_guard<std::mutex> lock(h->m);
					h->offers.clear();
				}
			}
			static void Forget(const void* host) { Table().Remove(host, true); }

			// The offered set of (host, seq) in this render frame (the newest generation), or null.
			static std::shared_ptr<const SourcePlaneSet> Find(const void* host, uint64_t seq)
			{
				const std::shared_ptr<HostOffers> h = Table().Find(host);
				if (!h)
					return nullptr;
				std::lock_guard<std::mutex> lock(h->m);
				std::shared_ptr<const SourcePlaneSet> best;
				for (const std::shared_ptr<const SourcePlaneSet>& e : h->offers)
					if (e && e->seq == seq && (!best || e->generation >= best->generation))
						best = e;
				return best;
			}
			// The facts of a P13 Acquire result (null for anything else).
			static const SourcePlaneSet* FactsOf(const VpSkeleton::AcquireResult& r)
			{
				return r.Ok() && r.gpu.textureSet == FactsTag() ? static_cast<const SourcePlaneSet*>(r.gpu.keepAlive.get()) : nullptr;
			}

			// THE P13 owner fill (GPU form, view, frame-scoped, render thread; plane axis only).
			static VpSkeleton::OwnerFill MakeOwnerFill()
			{
				VpSkeleton::OwnerFill f;
				f.variantAxes = VpSkeleton::VariantKey::kAxisPlane;
				f.view = true;
				f.frameScoped = true;
				f.renderThreadOnly = true;
				f.fill = [](const VpSkeleton::OwnerFillRequest& req, VpSkeleton::SkeletonMemo&, VpSkeleton::OwnerFillOutput& out) {
					const std::shared_ptr<const SourcePlaneSet> set = Find(req.host, req.seq);
					if (!set)
					{
						out.declineReason = "p13: seq not offered this frame";
						return false;
					}
					const int plane = req.variant.plane == 0 ? 0 : int(req.variant.plane) - 1;
					if (plane == 0 && set->rgb)
					{
						out.declineReason = "p13: RGB source has no luma plane";
						return false;
					}
					if (plane < 0 || plane >= set->planes)
					{
						out.declineReason = "p13: plane absent";
						return false;
					}
					const pl_tex tex = set->tex[size_t(plane)];
					if (tex == nullptr)
					{
						out.declineReason = "p13: no texture";
						return false;
					}
					out.gpu.resource = const_cast<void*>(static_cast<const void*>(tex));
					out.gpu.textureSet = FactsTag();
					out.gpu.index = plane;
					out.gpu.width = uint32_t(tex->params.w);
					out.gpu.height = uint32_t(tex->params.h);
					out.gpu.bytes = 0;   // a view: nothing allocated
					out.gpu.keepAlive = std::const_pointer_cast<void>(std::static_pointer_cast<const void>(set));
					return true;
				};
				return f;
			}

			struct Counters { uint64_t offered = 0, refused = 0; size_t current = 0; };
			static Counters CountersOf(const void* host)
			{
				Counters c;
				if (const std::shared_ptr<HostOffers> h = Table().Find(host))
				{
					std::lock_guard<std::mutex> lock(h->m);
					c.offered = h->offered;
					c.refused = h->refused;
					c.current = h->offers.size();
				}
				return c;
			}

		private:
			struct HostOffers
			{
				explicit HostOffers(const void* hst) : host(hst) { offers.reserve(kMaxOffers); }
				const void* host;
				std::mutex m;
				std::vector<std::shared_ptr<const SourcePlaneSet>> offers;
				uint64_t offered = 0, refused = 0;
			};
			static Hub::HostSlots<HostOffers>& Table()
			{
				static Hub::HostSlots<HostOffers> table;
				return table;
			}
			static const void* FactsTag()
			{
				static const char tag = 0;
				return &tag;
			}
		};

		// The hub observer of the P13 provider: Begin drops the previous render frame's offers, Entrance offers the frame being
		// rendered; the owner fill is registered with the skeleton (IExternalDemand) the first time a frame phase arrives.
		// Observation only (always false). GpuReleased / Shutdown forget the host.
		class SourcePlanesModule : public Hub::ISkeletonModule
		{
		public:
			const char* Name() const override { return "input.capture.source_planes"; }
			void OnLifecycle(RE::LifecycleFacts& f) override
			{
				if (f.host && (f.kind == RE::Lifecycle::GpuReleased || f.kind == RE::Lifecycle::Shutdown))
					SourcePlanes::Forget(f.host);
			}
			bool OnFrame(RE::FrameContext& c) override
			{
				if (c.phase != RE::FramePhase::Begin && c.phase != RE::FramePhase::Entrance)
					return false;
				EnsureRegistered();
				if (c.phase == RE::FramePhase::Begin)
					SourcePlanes::BeginFrame(c.host);
				else if (c.image != nullptr && c.seq >= 0 && !Hub::ExtensionHub::Instance().OriginalPathway(c.host))
					SourcePlanes::OfferFrame(c.host, uint64_t(c.seq), c.generation, *c.image);
				return false;
			}
			bool Registered() const { return registered_.load(std::memory_order_acquire); }
			// Registers the P13 fill through the skeleton found by the hub (tests pass their own demand).
			bool EnsureRegistered(Skeleton::IExternalDemand* demand = nullptr)
			{
				if (registered_.load(std::memory_order_acquire))
					return true;
				if (demand == nullptr)
					demand = static_cast<Skeleton::IExternalDemand*>(Hub::ExtensionHub::Instance().Query(Skeleton::kExternalDemandQueryId));
				if (demand == nullptr)
					return false;
				const VpSkeleton::SkeletonMemo::Registration r =
					demand->RegisterOwnerFill(VpSkeleton::kP13SourceRing, VpSkeleton::AcquireForm::Gpu, SourcePlanes::kUnit, SourcePlanes::MakeOwnerFill());
				const bool ok = r == VpSkeleton::SkeletonMemo::Registration::Registered
					|| r == VpSkeleton::SkeletonMemo::Registration::AlreadyRegistered;
				registered_.store(ok, std::memory_order_release);
				return ok;
			}
		private:
			std::atomic<bool> registered_{ false };
		};

		// Start-up only (HubBootstrap), after the skeleton module; idempotent.
		inline void RegisterSourcePlanesModule()
		{
			static bool registered = false;
			if (registered)
				return;
			registered = true;
			Hub::ExtensionHub::Instance().AddModule(std::unique_ptr<Hub::ISkeletonModule>(new SourcePlanesModule()));
		}
	}
}
