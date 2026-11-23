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
// frame. Offers are dropped at the next Begin of the host. Variant axes of a source frame: plane only (0 or 1 = the first plane:
// luma of a YUV source; 2 = the second plane; 3 = the third) with stage 0 (stage pictures: below). Declines say why (Acquire's LastCause / Fallbacks entry):
// "seq not offered this frame", "plane absent", "RGB source has no luma plane", "no texture".
// Original pathway: the skeleton gives no memo there, so nothing reaches the fill; the Entrance observer records nothing then
// (the hub fact). Cost when nobody asks: one table write per Entrance (four pointers), nothing allocated per frame after the first.
//
// CB slice 18 S1 (lead14/design-EDGEMAP-DOMAIN-AXIS.md rev 2 s2a; coreplan-CB-18-edgemap.md S1): STAGE PICTURES. A VP-V stage that
// holds a picture of a later pipeline stage on the render device (X5: the PRE_KERNEL input of scaling.output, inside its existing
// hook: seam S3, no hook line) offers it with OfferStage(host, seq, StagePicture); it is served through the same Acquire with
// VariantKey{stage, width, height, param1}: stage = VariantKey::StageAfter(PipelineSlots id) (non-zero), size = the picture's own
// texture size (it tells a ratio-2 half-size picture from the full one), param1 = the picture ordinal within the render frame
// (0 = the first; a re-render of the same seq in the same render frame, e.g. the de-judder extras, offers 1, 2, ...: 102b
// EDGEMAP-EXTRAS-KEY, so a re-render never receives an earlier picture's product). The same contract: a VIEW (0 bytes), render
// thread, frame-scoped, dropped at the next Begin. Declines: "p13: stage not offered this frame", "p13: stage picture has one
// plane", "p13: size / ordinal only with a stage". The facts (texture, colour space, luma system / primaries) come with the view
// (FactsOfStage).

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

		// One stage picture on the render device (S1). `tex` is the offering stage's texture (not owned): one RGB(A) picture, so a
		// single plane. The colour facts are the ones the offering stage measures in (X5: the hooked colour space and the source's
		// luma system / primaries); a fill derives its measure domain from them.
		struct StagePicture
		{
			uint64_t seq = 0;
			uint8_t stage = 0;        // VariantKey::StageAfter(PipelineSlots id); 0 is not a stage picture
			uint16_t ordinal = 0;     // the picture ordinal in this render frame (VariantKey::param1)
			pl_tex tex = nullptr;
			pl_color_space color{};
			pl_color_system system = PL_COLOR_SYSTEM_UNKNOWN;
			pl_color_primaries primaries = PL_COLOR_PRIM_UNKNOWN;
			const char* origin = "";  // the offering unit
		};

		class SourcePlanes
		{
		public:
			static constexpr const char* kUnit = "input/capture (queue + early upload)";   // the catalog's P13 owner (ProductCatalog.h)
			static constexpr size_t kMaxOffers = 4;   // frames one render frame may offer (A, B and two spare)
			static constexpr size_t kMaxStageOffers = 8;   // stage pictures one render frame may offer (main + re-renders)
			// The variant axes of P13: the plane of a source frame, or a stage picture (stage, size, ordinal).
			static constexpr uint32_t kAxes = VpSkeleton::VariantKey::kAxisPlane | VpSkeleton::VariantKey::kAxisStage
				| VpSkeleton::VariantKey::kAxisSize | VpSkeleton::VariantKey::kAxisParam1;

			// The Acquire key of a stage picture (S1): stage, the picture's texture size, the ordinal.
			static VpSkeleton::VariantKey StageKey(uint8_t stage, uint32_t width, uint32_t height, uint16_t ordinal)
			{
				VpSkeleton::VariantKey k;
				k.stage = stage;
				k.width = width;
				k.height = height;
				k.param1 = ordinal;
				return k;
			}

			// Render thread (S1). The picture of stage `pic.stage` of frame `pic.seq` is on the render device for this render frame.
			// A second offer of one (seq, stage, size, ordinal) replaces the first. False = no host / texture / stage, or the table is
			// full (counted).
			// INVARIANT (CB-18 SHOULD-3): a stage key names ONE picture. Every offerer of a stage gives each distinct picture of a
			// frame its own ordinal (per seq: scaling/output EdgeMapStageOrdinal.h for X5), never reusing (seq, ordinal) for a
			// different picture. Why: a product keyed on the stage key (kEdgeMap's stage variant) is memoised per key for the
			// frame, so a later picture offered under a used key would be measured from the FIRST picture's memoised result, and a
			// re-render that reuses the key overwrites the texture the earlier result was computed from. The replace above is for
			// re-offering the SAME picture (a retry), not a new one. Not asserted: replacing is a legal re-offer here and only the
			// offerer knows whether the picture is the same.
			static bool OfferStage(const void* host, const StagePicture& pic)
			{
				if (!host || pic.tex == nullptr || pic.stage == 0)
					return false;
				const std::shared_ptr<HostOffers> h = HostOf(host);
				if (!h)
					return false;
				std::lock_guard<std::mutex> lock(h->m);
				for (std::shared_ptr<const StagePicture>& e : h->stages)
					if (e && SameStageKey(*e, pic.seq, pic.stage, uint32_t(pic.tex->params.w), uint32_t(pic.tex->params.h), pic.ordinal))
					{
						e = std::make_shared<const StagePicture>(pic);
						++h->stageOffered;
						return true;
					}
				if (h->stages.size() >= kMaxStageOffers)
				{
					++h->stageRefused;
					return false;
				}
				h->stages.push_back(std::make_shared<const StagePicture>(pic));
				++h->stageOffered;
				return true;
			}
			// The offered stage picture of (host, seq, stage, size, ordinal) in this render frame, or null.
			static std::shared_ptr<const StagePicture> FindStage(const void* host, uint64_t seq, uint8_t stage, uint32_t width,
				uint32_t height, uint16_t ordinal)
			{
				const std::shared_ptr<HostOffers> h = Table().Find(host);
				if (!h)
					return nullptr;
				std::lock_guard<std::mutex> lock(h->m);
				for (const std::shared_ptr<const StagePicture>& e : h->stages)
					if (e && SameStageKey(*e, seq, stage, width, height, ordinal))
						return e;
				return nullptr;
			}
			// The facts of a P13 stage-picture Acquire result (null for anything else).
			static const StagePicture* FactsOfStage(const VpSkeleton::AcquireResult& r)
			{
				return r.Ok() && r.gpu.textureSet == StageFactsTag() ? static_cast<const StagePicture*>(r.gpu.keepAlive.get()) : nullptr;
			}

			// Render thread. The planes of frame `set.seq` are on the render device for this render frame. A second offer of one
			// (seq, generation) replaces the first. False = the host table is full or the set has no texture.
			static bool Offer(const void* host, const SourcePlaneSet& set)
			{
				if (!host || set.planes <= 0 || set.tex[0] == nullptr)
					return false;
				const std::shared_ptr<HostOffers> h = HostOf(host);
				if (!h)
					return false;
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
			// The host's next render frame begins: every offer of the previous one (frames and stage pictures) is dropped (render thread).
			static void BeginFrame(const void* host)
			{
				if (const std::shared_ptr<HostOffers> h = Table().Find(host))
				{
					std::lock_guard<std::mutex> lock(h->m);
					h->offers.clear();
					h->stages.clear();
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

			// THE P13 owner fill (GPU form, view, frame-scoped, render thread; the plane of a source frame, or a stage picture).
			static VpSkeleton::OwnerFill MakeOwnerFill()
			{
				VpSkeleton::OwnerFill f;
				f.variantAxes = kAxes;
				f.view = true;
				f.frameScoped = true;
				f.renderThreadOnly = true;
				f.fill = [](const VpSkeleton::OwnerFillRequest& req, VpSkeleton::SkeletonMemo&, VpSkeleton::OwnerFillOutput& out) {
					if (req.variant.stage != 0)
						return FillStage(req, out);
					if (req.variant.width != 0 || req.variant.height != 0 || req.variant.param1 != 0)
					{
						out.declineReason = "p13: size / ordinal only with a stage";
						return false;
					}
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

			struct Counters
			{
				uint64_t offered = 0, refused = 0;
				size_t current = 0;
				uint64_t stageOffered = 0, stageRefused = 0;
				size_t stageCurrent = 0;
			};
			static Counters CountersOf(const void* host)
			{
				Counters c;
				if (const std::shared_ptr<HostOffers> h = Table().Find(host))
				{
					std::lock_guard<std::mutex> lock(h->m);
					c.offered = h->offered;
					c.refused = h->refused;
					c.current = h->offers.size();
					c.stageOffered = h->stageOffered;
					c.stageRefused = h->stageRefused;
					c.stageCurrent = h->stages.size();
				}
				return c;
			}

		private:
			struct HostOffers
			{
				explicit HostOffers(const void* hst) : host(hst) { offers.reserve(kMaxOffers); stages.reserve(kMaxStageOffers); }
				const void* host;
				std::mutex m;
				std::vector<std::shared_ptr<const SourcePlaneSet>> offers;
				std::vector<std::shared_ptr<const StagePicture>> stages;
				uint64_t offered = 0, refused = 0;
				uint64_t stageOffered = 0, stageRefused = 0;
			};
			static Hub::HostSlots<HostOffers>& Table()
			{
				static Hub::HostSlots<HostOffers> table;
				return table;
			}
			// The host's offer table, made on its first offer (null: the host table is full).
			static std::shared_ptr<HostOffers> HostOf(const void* host)
			{
				std::shared_ptr<HostOffers> h = Table().Find(host);
				if (!h)
				{
					h = std::make_shared<HostOffers>(host);
					if (!Table().Put(host, h))
						h = Table().Find(host);
				}
				return h;
			}
			static bool SameStageKey(const StagePicture& e, uint64_t seq, uint8_t stage, uint32_t width, uint32_t height, uint16_t ordinal)
			{
				return e.seq == seq && e.stage == stage && e.ordinal == ordinal && e.tex != nullptr
					&& uint32_t(e.tex->params.w) == width && uint32_t(e.tex->params.h) == height;
			}
			// The stage-picture branch of the owner fill: the key's size must be the offered texture's (a half-size picture is a
			// different offer); one plane only.
			static bool FillStage(const VpSkeleton::OwnerFillRequest& req, VpSkeleton::OwnerFillOutput& out)
			{
				if (req.variant.plane > 1)
				{
					out.declineReason = "p13: stage picture has one plane";
					return false;
				}
				const std::shared_ptr<const StagePicture> pic =
					FindStage(req.host, req.seq, req.variant.stage, req.variant.width, req.variant.height, req.variant.param1);
				if (!pic)
				{
					out.declineReason = "p13: stage not offered this frame";
					return false;
				}
				out.gpu.resource = const_cast<void*>(static_cast<const void*>(pic->tex));
				out.gpu.textureSet = StageFactsTag();
				out.gpu.index = 0;
				out.gpu.width = uint32_t(pic->tex->params.w);
				out.gpu.height = uint32_t(pic->tex->params.h);
				out.gpu.bytes = 0;   // a view: nothing allocated
				out.gpu.keepAlive = std::const_pointer_cast<void>(std::static_pointer_cast<const void>(pic));
				return true;
			}
			static const void* FactsTag()
			{
				static const char tag = 0;
				return &tag;
			}
			static const void* StageFactsTag()
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
