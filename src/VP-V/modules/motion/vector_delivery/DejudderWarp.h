#pragma once

#include <libplacebo/gpu.h>
#include <libplacebo/log.h>
#include <libplacebo/dispatch.h>

#include <map>
#include <VP-V/modules/motion/estimation/NvOpticalFlow.h>
#include <GeneralRate.h>

namespace MotionAhead { struct Flow; }
#include <memory>
#include <string>
#include <vector>

// Stage 6 (6a): the de-judder's motion matcher and warp, as a standalone GPU
// component. The renderer runs it from Stage 6b (estimate in 6b-1, extra
// presents in 6b-2).
//
// Given two consecutive source frames A and B as planar YCbCr textures (the
// renderer's upload planes: a Y plane and an interleaved CbCr plane, 4:2:2
// or 4:2:0), it
//   1. estimates block motion A->B and B->A on half-resolution luma
//      (8x8 blocks at half resolution = 16x16 source pixels). A three-level
//      pyramid: a full search at 1/32 resolution; at 1/8, half-texel steps
//      within +-3 of zero and of the 1/32 result; at half resolution, whole
//      pixels within +-3 of both 1/8 results and +-2 of zero, then half a
//      pixel. Carrying both candidates down means a poor coarse guess (small
//      pictures, flat areas) is outvoted by the exact match, not followed.
//      Note: libplacebo reports a rejected pass (for example the same
//      texture bound twice) only in its log, not as a failed dispatch, so
//      every pass here binds each texture once,
//   2. marks each block trusted when its match error is within
//      `matchTolerance` and the two directions agree within
//      `consistencyPixels` (half-resolution pixels) - otherwise it is an
//      occlusion or a bad match,
//   3. counts the trusted blocks on the GPU (frame trust): below
//      `minTrustedShare` the whole frame is held (a cut between A and B),
//   4. warps A's planes by `shift` times the A->B motion (shift = s * delta:
//      strength times the refresh's position after A, in source frames).
//      Untrusted blocks are a hard hold of A's own pixels - never a blend -
//      and samples that fall outside the picture clamp to its edge.
// Everything downstream (scaling, colour, NLS, LED, shaders, dither) then
// renders the warped planes exactly as it would render A.
namespace Dejudder
{
	// GLSL: `bool field_vector(vec2 source_pos, out vec2 v)`, the warp's
	// vector rule (bilinear between trusted block centres; the position's own
	// block decides hold; the whole frame holds below min_trusted_share).
	// Needs the samplers `field` and `frame_trust` and the float
	// `min_trusted_share`. Shared with Stage 6d's rendered warp.
	const char* FieldVectorGlsl();
	// 11a E2: the same rule reading the sub-field (`sub_field` sampler) where
	// the split flag marks a block split; parent blocks as before.
	const char* FieldVectorGlslSplit();
	// M6: `vec2 field_vector_all(vec2 source_pos)` (sampler `field`): every
	// block's vector, or with PIXEL_TRUSTED_VECTORS defined only trusted
	// blocks' (a failed block follows its trusted neighbours).
	const char* FieldAllGlsl();


	struct Planes
	{
		pl_tex luma = nullptr;     // Y, one component
		pl_tex chroma = nullptr;   // CbCr interleaved, two components
		int chromaShiftX = 1;      // log2 horizontal subsampling (1 for 4:2:x)
		int chromaShiftY = 0;      // log2 vertical subsampling (0 for 4:2:2, 1 for 4:2:0)
		// The source's luma range (delta 5, D5-3): full-range luma is
		// measured in limited-range-equivalent codes (219 per unit, not
		// 255), so matchTolerance means the same for both.
		bool fullRange = false;
		// The source frame's number (0 = unknown): a pair whose A is the last
		// pair's B reuses B's uncorrected half-size luma and pyramid (exact).
		uint64_t sequence = 0;
	};

	struct Settings
	{
		// Mean absolute luma difference per sample (8-bit code units) up to
		// which a block's match is trusted.
		float matchTolerance = 3.0f;
		// Forward and backward vectors must agree within this many
		// half-resolution pixels.
		float consistencyPixels = 1.0f;
		// Search range at the top of the pyramid, in 1/32-resolution texels
		// (+-3 = +-96 source pixels per source frame, plus the refinements).
		int coarseRange = 3;
		// Frame trust (6b-2): below this share of trusted blocks the whole
		// frame is held (a cut between A and B). Measured and applied on the
		// GPU; no read-back.
		float minTrustedShare = 0.25f;
		// Refinements (docs/VP-build-plan-refinements.md). The defaults here
		// are off (the matcher as before); the renderer's keys default on.
		// D1: a whole-frame translation (1/32 then 1/8 resolution) becomes an
		// extra search start for every block when it is confident.
		bool globalPrior = false;
		// D3: a trusted block more than 1 source pixel from the median of the
		// trusted vectors around it takes the median; an untrusted block whose
		// 7+ trusted neighbours agree (within 0.5 px) takes their median when
		// its own match there is within matchTolerance.
		bool vectorSmooth = false;
		// D2: strength x ramp(mean trusted |v|, lo, hi) source px per source
		// frame (hi 0 = off). D4: x ramp(trusted share, minTrustedShare,
		// trustScaleFull) (0 = off). The product changes by at most
		// strengthRate per source frame; a cut (share below minTrustedShare)
		// still holds at once and restarts the ramp from 0. Applied on the GPU
		// (FrameTrust().g), by both warps.
		float motionScaleLo = 0.25f;
		float motionScaleHi = 0.0f;
		float trustScaleFull = 0.0f;
		float strengthRate = 0.25f;
		// D5 (experiment, source warp only): a held position whose block of B
		// is new content (uncovered) is taken from B along B's own motion
		// instead of held.
		bool occlusionFill = false;
		// Stage 11a, estimator-lite (docs/VP-stage11a-estimator-lite.md):
		// E1 a second fine sweep scoring the block's own result, its eight
		// neighbours' (from the first sweep), its previous-frame vector and
		// the global vector by SAD + estLambda x |v - median(neighbours)|
		// (codes per source pixel), then half-pel; the first sweep also tries
		// the previous-frame vector. E3 one quarter-pel step on trusted blocks.
		// E4 (with the global prior): when the global vector and the previous
		// frame's mean trusted |v| are both below motionScaleLo, both sweeps
		// keep the previous frame's vectors (decided on the GPU, no read-back).
		bool estCandidates = false;
		float estLambda = 0.5f;
		bool estQpel = false;
		bool estGate = false;
		// E2: blocks whose best search SAD (per sample) exceeds estSplitSad
		// (0 = matchTolerance) split into four 8x8-source-pixel sub-blocks,
		// each searched (parent and neighbour starts, +-2 px, half-pel) with
		// its own consistency trust, into SubField(). The split flag there
		// (w = 1) is the single source of granularity: the warps, the
		// occlusion pass and the grain-error pass read the sub-field where it
		// is set. Off: nothing allocated or bound; every consumer as before.
		bool estSplit = false;
		float estSplitSad = 0.0f;
		// Delta 9 (docs/VP-delta-8-9-10.md), synthesis-lite; off here, the
		// renderer's keys set them. H1: where a warped block meets a held one,
		// the warp fades to the unwarped A over this many output pixels on the
		// warped side (0 = a hard edge). H2: a block changes between warp and
		// hold only after this many consecutive source frames say so; a cut
		// holds everything at once (1 = off). H3: blocks whose match lies
		// between matchTolerance and taperSad (0 = 2 x tolerance) warp at a
		// strength ramped down by their SAD instead of holding (experiment).
		float holdFeatherPx = 0.0f;
		int holdHysteresis = 1;
		bool blockTaper = false;
		float taperSad = 0.0f;
		// Delta 14 (docs/VP-delta-14-soe-rules.md), against the soap-opera
		// look; off = D2 exactly. R-A: the strength also falls above a fast
		// pan, from 1 at rollLo to rollFloor at rollHi (source px per source
		// frame of the frame's motion: D1's global vector when reliable, else
		// the trusted blocks' mean), with D2's rate limit and cut hold. R-B:
		// the warps apply v_global + objectStrength x (v - v_global), the camera
		// smoothed fully and motion within the frame by objectStrength
		// (1 = off; without a reliable global vector the whole vector is
		// scaled). The occlusion mask and the grain map read the true field.
		bool motionRolloff = false;
		float rollLo = 8.0f;
		float rollHi = 24.0f;
		float rollFloor = 0.25f;
		float objectStrength = 1.0f;
		// Allocate host-readable outputs (tests only).
		bool hostReadable = false;
		// Owner 2026-10-13 efficiency (dejudder_low_res: off | search | all, all
		// by default in the renderer): for sources wider than 1920 the costly searches (global
		// motion, the block search, the second sweep) run one level coarser,
		// on the half-resolution luma, and their vectors are scaled back to the
		// source's 16-pixel block grid; consistency, quarter-pel, smoothing,
		// split, trust, hysteresis, occlusion and grain run at full resolution
		// as before. Every consumer reads the same accessors.
		// lowResAll: every pass at the coarser level (the trust, the field and
		// the sub-field scaled back); the dither's occlusion and the grain
		// measure are still made per 16 source pixels from the scaled-back
		// searches.
		bool lowRes = false;
		bool lowResAll = false;
		// Motion fixes (owner 2026-10-13 night, after the failure map; planner
		// answers docs/VP-review-2026-10-13-night-answers.md); all off here.
		// M1 texture tolerance: a block's match is good up to matchTolerance +
		// textureTolerance x its texture (the mean absolute neighbour
		// difference of A's half-resolution luma over the block, codes): fine
		// texture, grain and water raise the error of a correct match (0 = off).
		float textureTolerance = 0.0f;
		// M2 two global candidates: D1's best and the best away from it (the
		// background and the subject) both start every block's search, used
		// whether or not D1 is confident (the block's own match decides).
		bool globalTwo = false;
		// M3 regional hold: below minTrustedShare a frame is held whole only
		// when it is a cut (its 1/32 luma histogram changed by more than
		// cutHistogram, L1 of the normalised histograms, 0..2, or under 5%
		// trusted); otherwise its trusted blocks warp and the rest hold, and
		// D4 ramps from 0. FrameTrust().r is then the share the holds use.
		bool regionalHold = false;
		float cutHistogram = 0.3f;
		// M4 fades (planner Q11): when the frame's brightness changes smoothly
		// and globally (the 1/32 mean moves at least 0.75 code in the same
		// direction as the last pair's, less than 24: not a jump or a flash),
		// B's pyramid is matched after a global gain and offset (A's mean and
		// spread), and the strength is at most fadeFloor (a fading scene is not
		// where judder is noticed). A one-frame flash fails the test and holds.
		bool fadeMatch = false;
		float fadeFloor = 0.25f;
		// M5 verification (owner 2026-10-13 night: "if we over trust, back
		// track"): each trusted block predicts B from A pixel by pixel with the
		// vectors the warp will use (bilinear between trusted block centres);
		// more than verifyShare of its pixels off by more than verifyFactor x
		// matchTolerance (codes) withdraws its trust. Catches a block whose
		// average passed but whose pixels do not move together (an edge, an
		// uncovering, a partial wrong match).
		bool verifyPixels = false;
		float verifyShare = 0.2f;
		float verifyFactor = 3.0f;
		// M6 pixel trust (owner 2026-10-13 night, after the RAFT ceiling: the
		// block-level yes/no is the gap): every block's vector is used
		// (bilinear between all block centres) and each half-resolution pixel
		// of A gets a weight from how well it predicts B with that vector (the
		// 3x3 mean absolute error: 1 up to pixelTrustLo x matchTolerance, 0
		// from pixelTrustHi x); the source warp blends the moved pixel with the
		// hold by that weight. A cut still holds the whole frame. Source warp
		// only (the rendered warp keeps the block decision).
		bool pixelTrust = false;
		float pixelTrustLo = 1.0f;
		float pixelTrustHi = 2.5f;
		// Two tiers: pixels of blocks that failed the block test use these
		// (stricter) instead; 0 = the same as the trusted blocks'.
		float pixelTrustLoUntrusted = 0.0f;
		float pixelTrustHiUntrusted = 0.0f;
		// The landing check (owner 2026-10-14; with pixelTrust, both warps):
		// a looked-up source pixel counts only if its own motion carries it
		// back to the output position within this many source pixels; 0 = off.
		// Real clips: damaged tiles to a third for about half a point of the
		// judder removed (docs/VP-stage11b-learned-candidates.md).
		float landingCheck = 0.0f;
		// dejudder_source_warp_filter (owner 2026-10-17: grain is never averaged away): the
		// source warp samples A (and the fill's B) with Catmull-Rom instead of the hardware's
		// bilinear filter, which keeps only 0.62x of white grain at a 0.25 px shift and 0.5x
		// at 0.5 px. false = bilinear (the established output). Integration 2026-10-17: the
		// faithful path is the object's default as well as the profile's. The rendered warp
		// is Catmull-Rom already.
		bool sourceWarpCatmull = true;
		// General scaling (owner 2026-10-14: nothing tied to a resolution or a
		// rate). referenceHeight > 0: the pixel-valued settings (the D2 motion
		// scale, the roll-off, the landing check, the source warp's feather)
		// are given for a picture of that many lines and scaled to the input's
		// height, and the coarse search range is given for a 1080-line working
		// picture and scaled to the height the search runs at. < 0: the search
		// range only (a low-resolution inner, whose pixel values the outer has
		// scaled). 0: everything as given (direct use, the tests).
		float referenceHeight = 0.0f;
		// sourceHz > 0: strengthRate, holdHysteresis and the fade window are
		// given per frame at referenceRateHz and converted to this rate.
		double sourceHz = 0.0;
		double referenceRateHz = 24000.0 / 1001.0;
		// Fixed-constant batch B4 (2026-10-18, general_rate_fixes): the matcher's remaining pixel
		// distances follow its working height - the forward/backward consistency (and the optical-flow
		// engine's FB tolerance) by GeneralScale::AboveReference (unchanged up to 2160 lines), the
		// camera refinement's window continuously (DejudderPolicy::CameraWindowPixels(height)) instead
		// of a step at the half-search threshold (about 1527 lines). false = the legacy values.
		bool generalRates = true;
		// Black bars (owner 2026-10-14): only blocks inside the picture count.
		// Blocks outside it (letterbox / pillarbox bars) are untrusted (they
		// stay still and the warp interpolates only between picture blocks),
		// and the frame trust share, the cut histogram and the fade test are
		// taken over the picture alone - otherwise flat bars, trusted with
		// zero motion, keep the share above the cut threshold. The picture is
		// the renderer's detected active picture when known (SetPictureArea),
		// else the near-black full rows and columns at the frame's edges.
		bool pictureArea = true;
		// Motion source (owner 2026-10-14, experimental, off): the NVIDIA
		// optical flow engine's vectors (both directions, 4x4 grid on the
		// half-size picture; the median of a block's four) instead of the
		// block searches, re-scored per block and judged by the same trust
		// checks. Falls back to the searches where the engine is missing.
		bool opticalFlow = false;
		int opticalFlowPreset = 1;   // 0 slow, 1 medium, 2 fast
		// docs/VP-nvof-ahead.md (2026-10-16 corpus): the engine's vector as one more
		// candidate in the matcher's refinement (each block keeps whichever predicts B
		// best, then the half-pixel step) - true; false = the engine instead of the
		// searches (coarser: 88-95% of the matcher's removal on the corpus).
		bool opticalFlowCandidate = true;
		// With the candidate mode: the engine's vectors seed the refinement in place of
		// the first sweep's searches (the coarse pyramid search skipped - the saving),
		// the refinement, the half-pixel step and the trust tests unchanged. The default
		// (corpus 2026-10-16: 99.8-100% of the matcher's removal, the cut held; live 4K
		// matcher 7.5 -> 5.5 ms on the laptop; the candidate mode 12.0 ms for +0.2%).
		bool opticalFlowSeed = true;
		// Planner Q1 (2026-10-16): with the seed and a pair computed ahead, the whole frame's
		// motion (D1) from the engine's field (worked out on the producer's worker) instead
		// of the global search; the search stays the fallback (no pair ahead, no engine).
		bool opticalFlowGlobal = true;
		// With opticalFlowGlobal: the global search runs instead when the engine's
		// whole-frame motion is not clear (too few confident cells agree: ambiguity >= 0.9,
		// OpticalFlowTrust::GlobalUsable); false = the engine's value even then (before
		// 2026-10-17).
		bool opticalFlowGlobalFallback = true;
		// The engine's extras as trust inputs (OpticalFlowTrust.h), for a pair computed
		// ahead: the cost per block (a high cost lowers the block's trust) and the engine's
		// own forward/backward consistency per cell (inconsistent cells lower it). Both
		// raise the block's match error in the consistency pass by (1 - weight) x twice the
		// pass limit, where the final vector is the engine's (within a half-size pixel);
		// missing extras (an older driver, the render-time engine, the searches) change
		// nothing.
		// corpus 2026-10-17: no measured benefit on real clips (cost trust identical; fb check trades removal for damage) - off by default, owner gate
		bool opticalFlowCostTrust = false;
		bool opticalFlowFbCheck = false;
		// Tests only: the render-time engine's field read back (a stall) so the engine's
		// whole-frame motion can be compared on corpora without a producer.
		bool opticalFlowGlobalReadback = false;
		// Hunt 2 fix group 5 (motion_history_fixes; the renderer's key defaults on, the
		// object's default is the matcher as before): a pair not chained to the last
		// estimated one (its A is not that pair's B) starts the temporal history over
		// (04 M1), and ForgetHistory also forgets the D2/D4 strength state (04 L2).
		bool historyFixes = false;
		// The vectors M6 uses: true = only trusted blocks' (bilinear between
		// trusted centres; a failed block can only follow its trusted
		// neighbours, never its own vector); false = every block's. The
		// latter drags content from far away where a failed match's vector is
		// junk and the picture is smooth (seen on fast motion).
		bool pixelTrustedVectors = true;
		// With trusted vectors: a failed block's own vector is also used when
		// the block is detailed (mean absolute neighbour difference of A's
		// half-resolution luma at least pixelOwnTexture codes: there the pixel
		// check can tell a wrong vector) and its match error is at most
		// pixelOwnError x matchTolerance (0 = off: trusted blocks only).
		float pixelOwnTexture = 0.0f;
		float pixelOwnError = 3.0f;
		// How a partial weight applies: true = the pixel moves that share of
		// its motion (no double image; the same in both warps); false = a
		// crossfade of the moved and the held pixel.
		bool pixelTrustMotion = true;
		// Planner D1 (2026-10-14): the damage left with R-A on sits in fast
		// motion, where a partial move to a slightly wrong place still shows.
		// Above pixelFastFrom source px per source frame the pixel weights'
		// thresholds shrink, to pixelFastFloor x at pixelFastFrom x 4 (0 = off):
		// fast content must predict B more exactly to move.
		float pixelFastFrom = 0.0f;
		float pixelFastFloor = 0.4f;
	};

	// The warp-side defines for the given settings (SPLIT, FEATHER, TAPER) in
	// front of the field-vector rule; the variables feather_px, taper_tol and
	// taper_sad are needed when FEATHER / TAPER are on.
	// camera: the warp binds `camera` (MotionWarp::Camera()) for R-B.
	std::string WarpHeader(const Settings& settings, bool split, bool camera = false);

	class MotionWarp
	{
	public:
		MotionWarp(pl_log log, pl_gpu gpu, const Settings& settings = Settings());
		~MotionWarp();
		MotionWarp(const MotionWarp&) = delete;
		MotionWarp& operator=(const MotionWarp&) = delete;

		// Motion between A and B. Call once per source-frame pair. With
		// occlusionSlot 0 or 1 it also writes B's occlusion mask (6c) into
		// that slot; the caller alternates slots so the mask for the frame
		// now showing survives the next estimate. With grainError it also
		// writes the high-passed temporal error per block (Stage 8-3).
		bool Estimate(const Planes& a, const Planes& b, int occlusionSlot = -1, bool grainError = false);
		// The active picture in this matcher's input pixels (right and bottom
		// exclusive) for the next Estimate; an empty rectangle = not known
		// (the bars are then found from the picture itself).
		void SetPictureArea(int left, int top, int right, int bottom)
		{
			pictureGiven[0] = left; pictureGiven[1] = top; pictureGiven[2] = right; pictureGiven[3] = bottom;
		}
		pl_tex PictureAreaTexture() const { return picture; }

		// A warped by shift x (A->B motion). Call once per extra present.
		// The returned planes belong to this object and stay valid until the
		// next Warp call. With trusted = false nothing is warped (a cut, or
		// no estimate): `out` is A's planes.
		bool Warp(const Planes& a, float shift, Planes& out, bool trusted = true);
		// B3 (2026-10-24): the same warp on A's PROCESSED planes (re-based / repaired / 4K-routed),
		// at their own size: the field stays in estimate px and the shaders map positions. The
		// occlusion fill is off here until B is processed too. Its own output planes.
		bool WarpProcessed(const Planes& a, float shift, Planes& out, bool trusted = true);

		// D3 on a raw field (one texel per block, as Field()) into Field(),
		// with the last Estimate's half-resolution planes for the promotion
		// check. Estimate runs it itself when vectorSmooth is on; public so the
		// rule can be tested on a constructed field.
		bool Smooth(pl_tex raw);

		// 11a E1's second sweep on a given first-sweep result (forward
		// direction, half-resolution vectors as Forward()) into Forward(), with
		// the last Estimate's planes, global vector and previous searches.
		// Estimate runs it itself when estCandidates is on; public so the
		// penalty can be tested on a constructed first sweep.
		bool SecondSweep(pl_tex firstSweep)
		{
			return Refine(halfA, halfB, firstSweep, previousSearch[0], globalVector[0], forward);
		}

		// The block field from the last Estimate: rgba = (vx, vy) in source
		// pixels, trust (1/0), match error. One texel per 16x16 source block.
		pl_tex Field() const { return field; }
		// From the last Estimate (1x1): r = the share of trusted blocks,
		// g = the strength scale (D2/D4; 1 when both are off), b = the mean
		// trusted |v| in source pixels.
		pl_tex FrameTrust() const { return frameTrust; }
		// D1: the global vector of the last forward search (1x1): xy in 1/8
		// resolution texels (source pixels / 8), z = best / second-best match
		// (the prior is used below 0.9), w = 1 once written.
		pl_tex GlobalVector() const { return globalVector[0]; }
		// 11a E2: two texels per block each way (8x8 source pixels): rgba =
		// (vx, vy) source pixels, trust (1/0), split flag (1 = the parent block
		// split; 0 = a copy of the parent). nullptr while estSplit is off.
		// Tests only: time each pass with the GPU drained (slow; never in VP).
		// drain = false: GPU timers only, the GPU kept busy (a laptop GPU drops
		// its clock in the drained gaps); FlushPassTimers collects the last.
		void SetPassTiming(bool on, bool drain = true) { passTiming = on; passDrain = drain; passMs.clear(); passGpuMs.clear(); }
		// Efficiency timers T3 (audit 2026-10-24): production sampling of the same per-pass
		// pl_timers WITHOUT the drain (results polled with pl_timer_query when the pass next runs,
		// never waited for). A test's drained timing (SetPassTiming(true, true)) is left alone.
		void SetPassSampling(bool on)
		{
			if (passTiming && passDrain)
				return;
			passTiming = on;
			passDrain = false;
		}
		// The GPU ms per pass description collected since the last call (completed timers only).
		std::map<std::string, double> TakePassGpuMs()
		{
			std::map<std::string, double> taken;
			taken.swap(passGpuMs);
			return taken;
		}
		void FlushPassTimers();
		void PollPassTimers();   // non-blocking: never pl_gpu_finish
		// Tests only: the serial block search the parallel one must reproduce.
		void SetSerialSearchForTest(bool on) { serialSearchForTest = on; }
		uint64_t ReusedPlanesForTest() const { return reusedPlanesForTest; }
		pl_tex FadeStateForTest() const { return FadeStateTexture(); }
		// Tests only: lowRes without re-scoring the scaled-back searches.
		void SetLowResRescoreForTest(bool on) { rescoreForTest = on; }
		const std::map<std::string, double>& PassMs() const { return passMs; }
		// Owner 2026-10-13 (estimate ahead): a copy of what the warps read (the
		// field, the frame trust, the sub-field) for the extras still to come,
		// taken before the next pair is estimated over them.
		bool SnapshotForWarp();
		pl_tex SnapField() const { return snapField; }
		pl_tex SnapFrameTrust() const { return snapTrust; }
		pl_tex SnapSubField() const { return snapSubValid ? snapSub : nullptr; }
		pl_tex SnapCamera() const { return snapCamera; }
		// M6: A's pixel weights (half resolution), nullptr unless pixelTrust.
		pl_tex PixelMask() const { return settings.pixelTrust && pixelMaskReady ? pixelMask : nullptr; }
		pl_tex SnapPixelMask() const { return settings.pixelTrust && snapMaskValid ? snapMask : nullptr; }
		// The blocks whose vectors M6 uses, when failed blocks may use their own
		// (nullptr: trusted blocks only, the field's trust).
		bool PixelOwnVectors() const { return settings.pixelTrust && settings.pixelTrustedVectors && settings.pixelOwnTexture > 0.0f && vectorOk; }
		pl_tex VectorOk() const { return PixelOwnVectors() ? vectorOk : nullptr; }
		pl_tex SnapVectorOk() const { return PixelOwnVectors() && snapMaskValid ? snapVectorOk : nullptr; }
		// Planner Q7 (with R-A): after the next pair is estimated ahead, the
		// snapshot's frame trust with its strength lowered to the next pair's
		// when that is lower (the roll-off starts on a fast pan's first frame;
		// the next pair's strength is already rate-limited against this one, so
		// no step; a cut in the next pair does not touch this one). Returns
		// SnapFrameTrust() when R-A is off or the copy fails.
		pl_tex LookAheadTrust();
		// Delta 14 R-B: the camera's motion (1x1: x, y source px, reliable 1/0,
		// 1): the mean of the trusted blocks within 6 px of D1's global vector
		// (D1 alone is in 4-px steps). nullptr unless objectStrength < 1 with D1.
		pl_tex Camera() const { return settings.objectStrength < 1.0f && settings.globalPrior ? camera : nullptr; }
		// The same passes by the GPU's timestamp queries (no drain overhead).
		const std::map<std::string, double>& PassGpuMs() const { return passGpuMs; }
		pl_tex SubField() const { return settings.estSplit && estimated ? subField : nullptr; }
		// B's occlusion mask (6c), one texel per 16x16 source block of B:
		// r = 1 where B's content was visible in A (consistent both ways and
		// a good match), 0 where it is new or uncovered.
		pl_tex Occlusion(int slot) const { return slot == 0 || slot == 1 ? occlusion[slot] : nullptr; }
		// Stage 8-3, the grain map's input (from an Estimate with grainError):
		// one texel per 16x16 source block of A; r = the mean absolute
		// motion-compensated difference A - B after removing the block's mean
		// difference (a fade or lighting change is not grain), g = the block's
		// mean luma, both in 8-bit codes of half-resolution luma. Only
		// meaningful where Field() trusts the block; null before.
		pl_tex GrainError() const { return grainErrorValid ? grainError : nullptr; }
		// The raw half-resolution searches (rgba = vector in half-resolution
		// pixels, match error); for diagnostics.
		pl_tex Forward() const { return forward; }
		pl_tex Backward() const { return backward; }
		// Pyramid levels (0 half, 1 = 1/8, 2 = 1/32) of A (b = false) or B.
		pl_tex Level(int level, bool b) const
		{
			return level == 0 ? (b ? halfB : halfA) : level == 1 ? (b ? l1B : l1A) : (b ? l2B : l2A);
		}
		int BlocksX() const { return blocksX; }
		int BlocksY() const { return blocksY; }
		const std::string& LastError() const { return error; }
		// The settings as given (the renderer compares these), and as in use
		// for the current input (scaled for its height and source rate).
		const Settings& GetSettings() const { return base; }
		const Settings& EffectiveSettings() const { return settings; }
		// Stream robustness (owner 2026-10-14): after a discontinuity (resync,
		// a frame gap, a queue flush) the temporal history belongs to other
		// content - the previous searches, the hold hysteresis and the fade
		// state start over at the next Estimate.
		void ForgetHistory()
		{
			previousValid = false;
			ofHintValid = false;
			holdResetPending = true;
			keptSequence = 0;
			if (fadeState)
				pl_tex_destroy(gpu, &fadeState);
			// Hunt 2 04 L2: the D2/D4 strength scale is temporal state from the vectors
			// before the break; EnsureTextures recreates it zeroed (no history).
			if (base.historyFixes && strengthState)
				pl_tex_destroy(gpu, &strengthState);
			if (inner)
				inner->ForgetHistory();
		}
		// How many times the search input is halved (lowRes: once, an 8K input twice).
		int SearchHalvings() const
		{
			return HalvesForSearch() ? 1 + (inner ? inner->SearchHalvings() : 0) : 0;
		}
		uint64_t HistoryRestartsForTest() const { return historyRestarts; }
		void SetSettings(const Settings& given)
		{
			base = given;
			ApplyScaled(Scaled(given, height));
		}

	private:
		// The settings in use for an input of `inputHeight` lines.
		static Settings Scaled(const Settings& given, int inputHeight);
		void ApplyScaled(const Settings& value)
		{
			// lowRes switched: the other path's temporal state is stale.
			if (value.lowRes != settings.lowRes || value.lowResAll != settings.lowResAll)
			{
				inner.reset();
				previousValid = false;
			}
			// H2's state belongs to the old settings (audit 2026-10-14).
			if (value.holdHysteresis != settings.holdHysteresis || value.estSplit != settings.estSplit)
				holdResetPending = true;
			settings = value;
		}
		// Whether this matcher searches on a half-size copy (lowRes): for
		// inputs of about 1530 lines and taller (sqrt 2 x 1080), so the search
		// always runs on a picture of about 760 to 1530 lines; an 8K input is
		// halved twice (owner 2026-10-14: was "wider than 1920", once).
		bool HalvesForSearch() const
		{
			return settings.lowRes && static_cast<long long>(height) * height >= 2LL * 1080 * 1080;
		}
		// The forward/backward agreement (half-resolution px of this matcher's picture): as set up to
		// 2160 lines, by height above (fixed-constant batch B4 2026-10-18, general_rate_fixes).
		float ConsistencyPixels() const
		{
			return static_cast<float>(settings.consistencyPixels * GeneralScale::AboveReference(height, settings.generalRates));
		}
		pl_tex FadeStateTexture() const { return fadeState ? fadeState : inner ? inner->FadeStateTexture() : nullptr; }
		bool EnsureTextures(const Planes& a);
		bool passTiming = false;
		bool serialSearchForTest = false;
		uint64_t reusedPlanesForTest = 0;   // pairs whose A planes were the last pair's B
		bool DispatchPerBlock(const char* description, const char* header, const char* body,
			const pl_shader_desc* descriptors, int numDescriptors,
			const pl_shader_var* variables, int numVariables, int w, int h, int threadsX, int threadsY);
		std::map<std::string, double> passMs;
		std::map<std::string, double> passGpuMs;
		std::map<std::string, pl_timer> passTimers;
		void TimedDispatch(const char* description, pl_dispatch_compute_params& compute, bool& ok);
		void CollectTimer(const std::string& description, pl_timer timer);
		bool passDrain = true;
		MotionWarp* timingParent = nullptr;   // lowRes inner: its passes counted in the outer's maps
		bool Dispatch(const char* description, const char* header, const char* body,
			const struct pl_shader_desc* descriptors, int numDescriptors,
			const struct pl_shader_var* variables, int numVariables,
			int width, int height, int group);
		// levels: 0 the half-resolution luma only, 1 also the 1/8 level, 2 also the 1/32 level.
		// plain (with fade): B's uncorrected half-size luma, copied where no fade is active.
		bool Downsample(pl_tex luma, pl_tex half, pl_tex l1, pl_tex l2, bool fullRange, pl_tex fade = nullptr, int levels = 2,
			pl_tex plain = nullptr);
		// The pyramid levels above `from` up to `levels` of an existing half-size luma.
		bool Pyramid(pl_tex half, pl_tex l1, pl_tex l2, int from, int levels);
		// B's uncorrected planes kept for the next pair's A (2026-10-15): in the B
		// set, or in the K set when the fade correction rewrote the B set.
		void KeepPlainBInK();
		pl_tex halfK = nullptr, l1K = nullptr, l2K = nullptr;
		uint64_t keptSequence = 0;
		uint64_t historyRestarts = 0;   // Hunt 2 04 M1: pairs not chained to the last estimate
		bool keptInK = false;
		int keptLevels = 0;
		bool keptFullRange = false;
		bool Search(pl_tex fromHalf, pl_tex toHalf, pl_tex fromL1, pl_tex toL1,
			pl_tex fromL2, pl_tex toL2, pl_tex global, pl_tex previous, pl_tex out, pl_tex globalTwo = nullptr);
		bool Global(pl_tex fromL1, pl_tex toL1, pl_tex fromL2, pl_tex toL2, pl_tex out, pl_tex second = nullptr);
		// Both directions (A to B into out[0], B to A into out[1]) in the same
		// four dispatches instead of eight (2026-10-15; each direction's sums,
		// order and picks as Global's).
		bool GlobalBoth(pl_tex l1A, pl_tex l1B, pl_tex l2A, pl_tex l2B, pl_tex out[2], pl_tex second[2]);
		// Two same-size copies in one dispatch (2026-10-15).
		bool CopyPair(pl_tex src0, pl_tex dst0, pl_tex src1, pl_tex dst1);
		bool Refine(pl_tex fromHalf, pl_tex toHalf, pl_tex sweep, pl_tex previous, pl_tex global, pl_tex out,
			pl_tex opticalFlowVectors = nullptr);
		bool Copy(pl_tex src, pl_tex dst);

		pl_log log = nullptr;
		pl_gpu gpu = nullptr;
		pl_dispatch dispatch = nullptr;
		Settings settings;   // in use (scaled)
		Settings base;       // as given
		std::string error;

		int width = 0, height = 0;          // source luma size
		int halfW = 0, halfH = 0;
		int l1W = 0, l1H = 0;               // 1/8 source (pyramid level 1)
		int l2W = 0, l2H = 0;               // 1/32 source (pyramid level 2)
		int blocksX = 0, blocksY = 0;
		pl_tex halfA = nullptr, halfB = nullptr;
		pl_tex l1A = nullptr, l1B = nullptr, l2A = nullptr, l2B = nullptr;
		pl_tex forward = nullptr, backward = nullptr, field = nullptr, frameTrust = nullptr;
		pl_tex rawField = nullptr;                          // D3: before smoothing
		pl_tex globalVector[2] = { nullptr, nullptr };      // D1: forward, backward
		pl_tex strengthState = nullptr;                     // D2/D4: last scale (rgba32f)
		pl_tex fillOcclusion = nullptr;                     // D5: B's occlusion
		pl_tex sweep[2] = { nullptr, nullptr };             // 11a E1: the first sweep (fwd, bwd)
		pl_tex ofSearch[2] = { nullptr, nullptr };          // the engine's block vectors (candidate mode)
		pl_tex ofNone = nullptr;                            // bound in their place when there are none
		pl_tex previousSearch[2] = { nullptr, nullptr };    // 11a: last frame's searches
		bool previousValid = false;
		pl_tex qpelField = nullptr;                         // 11a E3: before D3
		pl_tex subField = nullptr;                          // 11a E2
		pl_tex subSearch[2] = { nullptr, nullptr };         // 11a E2: the split searches
		pl_tex holdState[2] = { nullptr, nullptr };         // delta 9 H2 (ping-pong)
		pl_tex subHoldState[2] = { nullptr, nullptr };
		pl_tex hystField = nullptr, hystSub = nullptr;
		int holdIndex = 0;
		Planes lastB;                                       // D5: B of the last Estimate
		bool fillReady = false;
		pl_tex occlusion[2] = { nullptr, nullptr };
		pl_tex grainError = nullptr;
		bool grainErrorValid = false;
		pl_tex warpedLuma = nullptr, warpedChroma = nullptr;
		pl_tex processedLuma = nullptr, processedChroma = nullptr;   // B3: WarpProcessed's planes
		bool WarpImpl(const Planes& a, float shift, Planes& out, bool trusted, bool processedTarget);
		bool estimated = false;
		// lowRes: the inner matcher searching this one's half-resolution luma
		// (already in code units; searches only, no warp planes).
		std::unique_ptr<MotionWarp> inner;
		bool searchOnly = false;
		pl_tex snapField = nullptr, snapTrust = nullptr, snapSub = nullptr, snapCamera = nullptr;
		pl_tex aheadTrust = nullptr;
		pl_tex snapMask = nullptr;
		pl_tex snapVectorOk = nullptr;
		bool snapMaskValid = false;
		pl_tex verifyField = nullptr;   // M5's output, swapped with the field
		bool holdResetPending = false;  // H2's state cleared at the next Estimate
		pl_tex pixelMask = nullptr;     // M6: A's half-resolution weights
		pl_tex vectorOk = nullptr;      // M6: blocks whose vector M6 may use (r = 1)
		bool pixelMaskReady = false;
		pl_tex camera = nullptr;
		bool snapSubValid = false;
		bool innerOfLowRes = false;   // no warp planes
		// Tests only: re-score the scaled-back searches per 16-pixel block (on
		// the owner's clip the grain made good matches look worse: 83% of the
		// benefit with the searches at half size, so off).
		bool rescoreForTest = false;
		// D1 in parallel: per-candidate frame SADs (1/32, 1/8) and the pick.
		pl_tex globalSad2 = nullptr, globalSad1 = nullptr, globalPick = nullptr;
		pl_tex globalBothSad2 = nullptr, globalBothSad1 = nullptr, globalBothPick = nullptr;
		pl_tex globalSecond[2] = { nullptr, nullptr };   // M2: D1's second candidate (l1 texels, -, -, valid)
		pl_tex fadeState = nullptr;   // M4: gain, offset, active, the last mean change (rgba32f, persists)
		bool FadeMatch(const Planes& b);
		// Black bars: the picture rectangle (x0, y0, x1, y1 in input pixels,
		// rgba32f), written each Estimate.
		pl_tex picture = nullptr;
		int pictureGiven[4] = { 0, 0, 0, 0 };
		bool pictureFullRange = false;
		bool PictureArea();
		bool lumaInCodes = false;
		bool LowResSearch(const Planes& a, const Planes& b);
		// The optical flow source: forward / backward in the searches' format.
		bool OpticalFlowSearch(pl_tex outForward, pl_tex outBackward);
		// The pair's trust weights (cost, the engine's own consistency) into ofWeight per
		// block (ofWeighted); none when switched off, missing or not uploadable.
		bool UploadEngineWeights(const MotionAhead::Flow& flow);
		std::unique_ptr<NvOpticalFlow::Engine> opticalFlowEngine;
		bool opticalFlowFailed = false;   // logged once; the searches from then on
		int opticalFlowPresetOpen = -1;
		CComPtr<ID3D11Texture2D> ofIn[2], ofFlow[2], ofCost[2];
		pl_tex ofInTex[2] = { nullptr, nullptr }, ofFlowTex[2] = { nullptr, nullptr };
		int ofW = 0, ofH = 0;
		// The engine's temporal hints only from the pair just before (audit 2026-10-16: they carried
		// across resyncs, gaps and runs of pairs computed ahead).
		bool ofHintValid = false;
	public:
		// The searching instance may be the half-size inner one (lowRes): report it.
		const std::string& OpticalFlowError() const
		{
			return opticalFlowError.empty() && inner ? inner->OpticalFlowError() : opticalFlowError;
		}
		bool OpticalFlowUsed() const { return opticalFlowUsed || (inner && HalvesForSearch() && inner->OpticalFlowUsed()); }
		// The engine's input size where the searches run (this instance's half size,
		// or the inner's): the size a flow computed ahead must have. False before the
		// first estimate.
		bool EngineInputSize(int& w, int& h) const
		{
			if (HalvesForSearch() && inner) return inner->EngineInputSize(w, h);
			w = halfW; h = halfH;
			return halfW > 0 && halfH > 0;
		}
		// docs/VP-nvof-ahead.md: the next Estimate's pair as computed ahead by the
		// producer (null = none). Used when it matches the half size; then no engine
		// work happens at render time. Cleared by each Estimate.
		void SetExternalFlow(std::shared_ptr<const MotionAhead::Flow> flow) { externalFlow = std::move(flow); }
		bool ExternalFlowUsed() const { return externalFlowUsed || (inner && HalvesForSearch() && inner->ExternalFlowUsed()); }
		// The last Estimate's D1 came from the engine's field (planner Q1).
		bool GlobalFromEngine() const { return globalFromEngine || (inner && HalvesForSearch() && inner->GlobalFromEngine()); }
		// The engine's whole-frame motion was there but not clear, so the global search ran
		// (opticalFlowGlobalFallback), counted over this instance's life (and the inner's).
		unsigned long long GlobalFallbacks() const { return globalFallbacks + (inner ? inner->GlobalFallbacks() : 0ull); }
		// The last Estimate applied the engine's trust weights (cost / consistency).
		bool EngineWeightsUsed() const { return ofWeighted || (inner && HalvesForSearch() && inner->EngineWeightsUsed()); }
	private:
		bool globalFromEngine = false;
		unsigned long long globalFallbacks = 0;
		bool ofWeighted = false;          // this Estimate's engine block vectors carry trust weights (w)
		bool refineCarriesWeight = false; // Refine keeps the seed's weight with its own vector
		pl_tex ofWeight = nullptr;        // the weights per block (r32f / r16f), uploaded per pair
		std::vector<float> ofWeights, ofFbWeights;   // CPU scratch
		float engineGlobal[2][4] = {};   // the engine's whole-frame motion (D1 format), this Estimate
		bool engineGlobalValid = false;
		std::string opticalFlowError;
		bool opticalFlowUsed = false;
		std::shared_ptr<const MotionAhead::Flow> externalFlow;
		bool externalFlowUsed = false;
	};
}
