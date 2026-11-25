// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Facts the skeleton module publishes about the vector engine's field and the scene, for the readers that sit beside the lane
// (the camera fit, guidance). Plain values, no GPU object: a reader that needs the texture asks the engine (IVectorEngine::FieldOf)
// on the render thread. SK-W stage D.

#include <VP-V/core/Indicator.h>
#include <VP-V/core/LaneLease.h>
#include <VP-V/core/Products.h>
#include <VP-V/core/SkeletonAcquire.h>
#include <VP-V/core/hub/HubQuery.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Vpv
{
	namespace Skeleton
	{
		// The verified field of one pair as the engine made it, recorded at the moment the module published the P2m / P3 slots of
		// frame `seq` (= pairTo). `stamp` is the stamp the field was computed under (the engine's Validity facade: MotionValidity::StampAt);
		// the slots carry it as their honoured stamp, so the seal of frame `seq` rejects them when a resync event moved the stamp between.
		struct EngineFieldFacts
		{
			uint64_t seq = 0;                 // the frame the slots were published for (= pairTo)
			uint64_t pairFrom = 0, pairTo = 0;
			uint64_t era = 0;                 // IVectorEngine::FieldEra() when published
			int gridW = 0, gridH = 0, blockSize = 0;   // blockSize in source px
			float scaleToSourcePx = 1.0f;
			VpIndicator::Stamp stamp;
			bool globalValid = false;         // IVectorEngine::EngineGlobal(seq): the producer's whole-frame vector (source px over the pair)
			double globalX = 0.0, globalY = 0.0;
			bool p2Published = false;         // the NVOF pair slot (P2) was published (only when the graph holds P2 and the engine had a global)
			bool p2mPublished = false;        // the matcher pair slot (P2m)
			bool p3Published = false;         // the analysis field slot (P3)
		};

		// The scene of frame `seq` against the previous frame. U7 v2 (U7-accuracy): cut / flash / cutPossible / cutStrength are the SEQUENCE
		// law's verdict (VpUnit::CutFadeJudge::CutTracker over this renderer's P4 records, frames seq - 8 .. seq + 1), and the v1 pair cut /
		// flash / cutPossible are kept as pair* for diagnostics. `fade` (U7 v3, 102b U7-MULTIFRAME-FADE-LAW) is the pair law's fade
		// (ClassifyEvents over the two records) OR the multi-frame law's (CutFadeJudge::FoldFadeRun over the run of earlier pairs that ends
		// at the previous record). The verdict is stored once per frame by SceneOf's publisher. The raw record values are given so a reader
		// can run the fade fit (CutFadeJudge::FadeFit) with its own scaling. Codes are the 16-bit half-plane sample codes of the P4 record.
		struct SceneFacts
		{
			bool valid = false;               // both P4 records were readable
			const char* why = "";             // when !valid: the cause token (AcquireCause) or a short reason; static text, never freed
			bool cut = false, cutPossible = false, fade = false, flash = false;
			bool confirmed = false;           // the verdict saw frame seq + 1 (false = provisional, rewritten at the next frame's seal)
			bool gridLaw = false;             // the 32 x 18 grid judged it (false: no grid, the v1 pair law)
			float cutStrength = 0.0f;         // 0..1, 1 = cut
			// U7 v3 partial reset (owner 2026-11-15): the frame's history keep, 0..1 (CutFadeJudge::FrameFadeKeep; 1 = no fade)
			float fadeKeep = 1.0f;
			bool pairCut = false, pairCutPossible = false, pairFlash = false;   // the v1 pair law on the two records (diagnostic only)
			uint32_t histDistance = 0;        // 0..65535
			bool fullBetween = false;         // RESERVED, always false: scene events come from P4 alone; resync / validity is the P5a MotionValidity facade's
			bool continuityBetween = false;   // RESERVED, always false (see above)
			uint32_t prevMean = 0, curMean = 0;
			uint32_t prevSpread = 0, curSpread = 0;   // p99 - p01
			bool rectFound = false;           // Bill's active-picture rectangle of the CURRENT frame (half-plane px)
			uint32_t rectX0 = 0, rectY0 = 0, rectX1 = 0, rectY1 = 0;
		};

		// SCENE as a skeleton product (W1-C7): what a reader of "cut / fade of frame `seq`" gets from SkeletonModule::SceneOf. The product is
		// sealed per frame: the module publishes it into the frame-analysis store at the frame's seal (once; skipped when present). `from`
		// says where this answer came from: Store = the record's scene group; Live = filled from the two P4 records now (the store is off, has
		// no record for the frame, or the frame is not sealed yet). Both give the same answer (one judge: SceneFacts above).
		struct SceneAnswer
		{
			enum class From : uint8_t { None, Store, Live };
			bool known = false;               // cut / fade are answered (false: the frame has no sealed pair of P4 records yet)
			bool cut = false, fade = false;   // cut = the DELIVERED restart signal (FrameAnalysis::Scene::cut: once per cut, normally one frame late)
			bool cutLate = false, cutHere = false;                        // U7 v2: FrameAnalysis::Scene
			bool flash = false, cutPossible = false, confirmed = false;   // U7 v2 (SceneFacts above)
			float cutStrength = 0.0f;
			float fadeKeep = -1.0f;           // U7 v3: FrameAnalysis::Scene::fadeKeep (< 0 = not given)
			From from = From::None;
			const char* why = "";             // when !known: why (AcquireCause token, "no store owner", "no analysis lane"); static text
		};

		// What the scene product did (any thread; relaxed counters copied out).
		struct SceneCounters
		{
			uint64_t published = 0;       // records written at a seal
			uint64_t alreadyPresent = 0;  // seal found the scene already there (compute-once: nothing recomputed)
			uint64_t notReady = 0;        // seal could not classify or had no record to write into (counted once, not re-tried every call)
			uint64_t storeReads = 0;      // SceneOf answered from the store
			uint64_t liveFills = 0;       // SceneOf answered by classifying the P4 records now (and filled the store when it holds the record)
			uint64_t cacheHits = 0;       // SceneOf answered from this frame's live classification (no second classification)
			uint64_t unknown = 0;         // SceneOf could not answer
			// U7 v2: verdicts judged with frame seq + 1, verdicts given without it (provisional), tracker restarts (a seek, a gap, a
			// renumbered frame) and earlier Vp groups rewritten when their confirmation arrived.
			uint64_t confirmed = 0, provisional = 0, reprimes = 0, republished = 0;
			uint64_t provisionalCutWithdrawn = 0;   // provisional cut verdicts (cutHere while !confirmed) that became !cut on confirmation
			// Cuts delivered late (with a later frame's group) and, of them, more than one frame late (frame c + 1 had no group).
			uint64_t late = 0, lateGap = 0;
			uint64_t noGrid = 0;          // no-grid fallback verdicts (the v1 pair law), per tracker push (a re-prime pushes again)
			// U7's multi-frame fade run (unit version 3) on a ring miss: rebuilt exactly from older P4 records (joined a ring entry or the
			// stream start), or with REDUCED evidence (the run starts only a few pairs back; a slow fade may be found later). Rule 5.
			uint64_t fadeRunRebuilt = 0;
			uint64_t fadeRunReduced = 0;
			uint64_t fadeRunRebuildUsTotal = 0;   // cumulative microseconds spent rebuilding missed fade runs in SceneRunBefore
			uint64_t fadeRunRebuildUsMax = 0;     // worst single fade-run rebuild in microseconds
			uint64_t fadeRunRebuildUsLast = 0;    // most recent fade-run rebuild in microseconds
			// U7 v3 partial reset: frames classified as a fade with a history keep below 1 (no restart: the history is reduced).
			uint64_t fadePartial = 0;
		};

		// What the engine feed did (render thread): shown on the status lines, read by tests.
		struct FeedCounters
		{
			uint64_t published = 0;       // fields whose slots were published
			uint64_t noEngine = 0;        // calls without an engine for this renderer
			uint64_t noField = 0;         // the engine named a latest field it could not give
			uint64_t slip = 0;            // fields not published: the pair spans a slip / an invalid pair (PairSpansSlip)
			uint64_t ancestorsMissing = 0;   // publish attempts that found an input slot (P1) not published yet (retried)
			uint64_t claimFailed = 0;     // a slot could not be claimed (the frame left the ring)
			uint64_t abandoned = 0;       // fields given up after the retries
			uint64_t eraResets = 0;       // the engine's numbering restarted (seek, resync, format change)
			uint64_t lastSeq = 0;         // the last frame published
			uint64_t overwritten = 0;     // Y1b: fields pushed out of the pending ring (oldest goes) before they were published
			// Y1b: per-product AHEAD counters, from the frame results the seal returns at Begin (P2 / P3 only): sealed ahead vs absent (the reader
			// goes live, or the product was unavailable). A lagged-seal pending result is neither.
			uint64_t p2Ahead = 0, p2Absent = 0, p3Ahead = 0, p3Absent = 0;
		};

		// The EXTERNAL DEMAND entry of the skeleton module (SK-W stage D), for a component beside the lane that does not include the module
		// (the camera extension). SkeletonModule implements it and hands it out through Query(kExternalDemandQueryId). Any thread. The
		// contract is SkeletonModule::SetExternalDemand / ClearExternalDemand (one entry per (host, owner), replaced by the next call).
		constexpr const char* kExternalDemandQueryId = "vpv.skeleton.external_demand.v1";

		// L-CORE (lead8/iface-L-CORE.md s2): a renderer's GET-OR-COMPUTE-AND-PUBLISH memo for a frame. `memo` is shared: it stays
		// valid while held, also across a lane rebuild. `gpu` = the renderer's pl_gpu (opaque, null without one) for a GPU owner fill.
		// Empty (Valid() false) = no skeleton module, the Original pathway, or an unknown renderer: the caller shows its visible
		// "unavailable", never a private path.
		struct FrameMemo
		{
			std::shared_ptr<VpSkeleton::SkeletonMemo> memo;
			const void* host = nullptr;
			uint64_t seq = 0;
			uint64_t nextSeq = 0;
			bool haveNext = false;
			const void* gpu = nullptr;
			bool Valid() const { return memo && host; }
		};

		// W23-4p (G-T1): render-thread snapshot of P10/P11 GPU products (opaque pl_tex) and display facts for live dither_need.
		struct DitherNeedGpuInputs
		{
			static constexpr size_t kPrecursors = 6;
			const void* gsBlocks = nullptr;
			const void* precursors[kPrecursors] = {};
			int srcW = 0, srcH = 0, gridW = 0, gridH = 0, blockPx = 0;
			uint64_t grainSeq = 0, evidenceSeq = 0;
			bool haveGrain = false, haveEvidence = false;
		};
		struct DitherNeedDisplayFacts
		{
			int regionTransfer = 0, llContextKind = 0, llTransfer = 0;
			bool llFullRange = false, llSdrToneKnee = false;
			double sdrWhiteNits = 203.0, displayPeakNits = 203.0, displayBlackNits = 0.0, ambientNits = 0.0, weber = 0.02, sdrGamma = 2.4;
			double llSourcePeakNits = 0.0, llSourceMinNits = 0.0, llDynamicPeakNits = 0.0, llSdrTargetNits = 203.0, llSdrBlackNits = 0.0, llGammaPower = 2.4;
			double llSdrToneHeadroom = 0.0, llSdrToneKneeStart = 0.75, llSdrToneKneeStrength = 0.5;
			std::string llToneMapping = "auto", llQuality = "high";
		};

		struct IExternalDemand
		{
			virtual void SetExternalDemand(const void* host, const void* owner, const std::vector<VpSkeleton::ConsumerDecl>& consumers,
				const std::vector<VpSkeleton::ProductId>& publishedOutside) = 0;
			virtual void ClearExternalDemand(const void* host, const void* owner) = 0;
			// 15c-c8 (coordinator ruling 2026-11-15, the owner's demand-driven principle; the same shape as the TD fill rule,
			// core/AdmissionTable.h Admit "TD OFF: ... filled FOR that consumer"): the same entry, but the declaring component NEEDS the
			// lane's results, so its demand RAISES the host's lane (analysis_lane off / shadow -> on) and the frame-analysis store that
			// feeds it (frame_analysis_store off -> on) while the entry is registered; ClearExternalDemand withdraws the raise. `reason`
			// names the consumer on Ctrl+I ("Analysis lane: on (demand: <reason>)"). Never in the Original pathway; the configuration
			// is never changed. An empty consumer list raises nothing (no consumer, nothing on). Any thread.
			// Default (an implementer without lanes): the plain declaration.
			virtual void SetRaisingExternalDemand(const void* host, const void* owner, const std::vector<VpSkeleton::ConsumerDecl>& consumers,
				const std::vector<VpSkeleton::ProductId>& publishedOutside, const std::string& /*reason*/)
			{
				SetExternalDemand(host, owner, consumers, publishedOutside);
			}
			// The CURRENT lease of the host's lane for `product` (AnalysisLane::Lease: the product's bytes ring added), for a component that
			// publishes a product it declared publishedOutside (the camera's on-time P7). Any thread. EMPTY (no table) when the host has no
			// lane (analysis_lane off, no source yet) or the graph does not hold the product: the caller uses its own path, visibly.
			// Generations are per lane (a new source size builds a new lane and restarts them): take a fresh lease per use, never install
			// one across a rebuild with LaneLeaseNewer. Default: empty (an implementer without lanes).
			virtual VpSkeleton::LaneLease LeaseFor(const void* /*host*/, VpSkeleton::ProductId /*product*/) { return VpSkeleton::LaneLease(); }
			// L-CORE deliverables 1-2: the renderer's memo for frame `seq` (any thread). It exists with or without the analysis lane;
			// never in the Original pathway. nextSeq / haveNext are filled from the hub's current frame when `seq` is the frame the
			// renderer is rendering. Default: empty (an implementer without a memo).
			virtual FrameMemo MemoFor(const void* /*host*/, uint64_t /*seq*/) { return FrameMemo(); }
			// The OWNING unit's fill of (product, form), MODULE-WIDE: installed in every renderer's memo now and in every memo made
			// later (registration rules of SkeletonMemo::RegisterOwnerFill: the catalog's unit only, one per (product, form)).
			// Any thread; start-up is the usual place. Default: NotInCatalog (no memo to install it in).
			virtual VpSkeleton::SkeletonMemo::Registration RegisterOwnerFill(VpSkeleton::ProductId, VpSkeleton::AcquireForm,
				const std::string& /*unit*/, VpSkeleton::OwnerFill) { return VpSkeleton::SkeletonMemo::Registration::NotInCatalog; }
			// Atomic replacement of an existing module-wide owner fill (zero unregistered gap / NoOwnerFill race; S1-H2).
			// Default fallback for non-SkeletonModule implementers delegates to UnregisterOwnerFill + RegisterOwnerFill.
			virtual VpSkeleton::SkeletonMemo::Registration ReplaceOwnerFill(VpSkeleton::ProductId id, VpSkeleton::AcquireForm form,
				const std::string& unit, VpSkeleton::OwnerFill fill)
			{
				if (fill.defaults.stage != 0)
					return VpSkeleton::SkeletonMemo::Registration::BadDefaults;
				const bool had = UnregisterOwnerFill(id, form, unit);
				const auto r = RegisterOwnerFill(id, form, unit, std::move(fill));
				return (had && r == VpSkeleton::SkeletonMemo::Registration::Registered) ? VpSkeleton::SkeletonMemo::Registration::Replaced : r;
			}
			virtual bool UnregisterOwnerFill(VpSkeleton::ProductId, VpSkeleton::AcquireForm, const std::string& /*unit*/) { return false; }
			// The SCENE PRODUCT's get-or-compute-and-publish reader (SkeletonModule::SceneOf; owner 2026-11-14 17:38 "Ask the skeleton
			// for it same as everything else"): the store's scene group of frame `seq`, else classified now and published. The answer is
			// the same with frame_analysis_store off. RENDER THREAD for the live path (it reads the lane's slots). Default: unknown (an
			// implementer without a scene product; the caller shows its visible "unavailable").
			virtual SceneAnswer SceneOf(const void* /*host*/, uint64_t /*seq*/) const { return SceneAnswer(); }
			// O#1 (L0 one scene reader): raw scene facts (U7 verdict + P4 means/spreads + active-picture rect) through the same
			// IExternalDemand facade as SceneOf. Default: invalid ("no skeleton module").
			virtual bool SceneFactsOf(const void* /*host*/, uint64_t /*seq*/, SceneFacts& out) const { out = SceneFacts(); out.why = "no skeleton module"; return false; }
			// GF-P2b S3 (GV AMEND 1): how many times the skeleton ERASED this host's external demand entries (GpuReleased / Shutdown, a dead
			// renderer at the address). A declaring component folds it into its "already declared" key, so a GPU-only reset of the same
			// renderer (the tap generation unchanged) still re-declares. Only ever grows. Any thread. Default 0 (nothing is ever erased).
			virtual uint64_t ExternalErasures(const void* /*host*/) const { return 0; }
			// W23-4p (G-T1): render-thread reader of the skeleton's GPU P10 (grain split blocks) and P11 (region precursors) for frame `seq`.
			// Default returns empty (implementer without GPU products).
			virtual DitherNeedGpuInputs ReadDitherNeedGpuInputs(const void* /*host*/, uint64_t /*seq*/) const { return DitherNeedGpuInputs(); }
			// W23-4p (G-T1): render-thread sync of resolved display facts to P11 (RegionStatsKernel) and P12 (LightLevelKernel). Change-only.
			virtual void SyncDitherNeedDisplayFacts(const void* /*host*/, const DitherNeedDisplayFacts& /*facts*/) {}
		protected:
			~IExternalDemand() {}
		};

		// The skeleton module's demand entry (null without it). One module-list scan. Through the hub's query slot (core/hub/HubQuery.h),
		// so VideoProcessor-Lib code (the LED drive, SceneRecordReader), which cannot include ExtensionHub.h, asks the same hub.
		inline IExternalDemand* Demand()
		{
			return static_cast<IExternalDemand*>(Hub::QueryHub(kExternalDemandQueryId));
		}

		// The visible explanation of FeedCounters::claimFailed (empty while none). `backward` = the lane runs without a look-ahead lead (low-latency /
		// backward pairs, lane lead 0): the engine's pair (N-1, N) is only complete AFTER frame N was sealed, so the slot is already taken and the field
		// cannot be published for that frame; its readers fall back to their own form. That is how this mode works, not a fault, and it is said so.
		// With a lead the same counter means the frame had left the ring (or was published already) before the field arrived.
		std::string ClaimFailedNote(uint64_t claimFailed, bool backward);
	}
}
