// Uses the test project's precompiled header (CustomHookChain.h needs MFC's CString via the
// madVR shader loader).
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/modules/colour/extended_range/ExtendedRangeStage.h>
#include <VP-V/modules/motion/consumers/DejudderCacheStage.h>
#include <VP-V/core/SlotRegistry.h>
#include <VP-V/modules/output/dither/DitherSlot.h>
#include <VP-V/modules/output/dither/SigmaDeltaVectors.h>
#include <VP-V/modules/scaling/output/AdaptiveDownscaleStage.h>

#include <libplacebo/colorspace.h>

#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// WP-13 (integration batch 1): the dither/output glue moved out of the renderer host into VP-V
// stages. The move is behaviour-free: each test compares a stage against today's host code (a
// copy of the statements it replaced) or pins the host-visible results (hook order, the shared
// state, the logs' once-only latches). GPU byte identity: docs/design/m2-output-callsites.md.
namespace VideoProcessorTest
{
	namespace
	{
		pl_color_space Space(enum pl_color_primaries primaries, enum pl_color_transfer transfer)
		{
			pl_color_space c{};
			c.primaries = primaries;
			c.transfer = transfer;
			return c;
		}

		pl_rect2df Rect(float x0, float y0, float x1, float y1) { return pl_rect2df{ x0, y0, x1, y1 }; }

		std::vector<uint64_t> Signatures(const std::vector<const pl_hook*>& hooks)
		{
			std::vector<uint64_t> out;
			for (const pl_hook* h : hooks) out.push_back(h ? h->signature : 0);
			return out;
		}

		// Today's host code (LibplaceboVideoRenderer.cpp at 29c3843d, RenderLocked's Stage 10
		// block, without the logs), on its own Hooks object.
		bool TodaysExtendedRange(const ExtendedRangeStage::Frame& f, bool hookFailed, ExtendedRange::Hooks& hooks,
			pl_color_space& sourceColor, const pl_color_space& targetColor, std::vector<const pl_hook*>& list,
			ExtendedRange::Reason& reason)
		{
			ExtendedRange::Settings extendedSettings;
			extendedSettings.piecewiseTransfers = extendedSettings.wideSourcePrimaries =
				extendedSettings.calibrationLut = extendedSettings.hdrPassthrough = f.moreCases;
			extendedSettings.hdrDynamicToneMapping = f.hdrDynamicToneMapping;
			ExtendedRange::Decision extended = ExtendedRange::Decide(f.preserve, extendedSettings,
				sourceColor, targetColor, f.targetLut, f.targetLutType);
			if (extended.Active() && hookFailed)
				extended = ExtendedRange::Decision{ ExtendedRange::Reason::HookFailed };
			if (f.temporalActive)
				extended = ExtendedRange::ForTemporal(extended, f.temporalMixing);
			const bool relabelled = extended.Active();
			ExtendedRange::Prepare(extended, hooks, sourceColor, targetColor, list, f.singlePlaneRgb);
			reason = extended.reason;
			return relabelled;
		}
	}

	TEST_CLASS(OutputGlueStagesTests)
	{
	public:
		// ---- colour.extended_range ------------------------------------------------------------

		TEST_METHOD(ExtendedRangeStageIsTodaysDecisionRelabelAndHookList)
		{
			const pl_color_space sources[] = {
				Space(PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_GAMMA24), Space(PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_BT_1886),
				Space(PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_SRGB), Space(PL_COLOR_PRIM_BT_601_625, PL_COLOR_TRC_GAMMA22),
				Space(PL_COLOR_PRIM_BT_2020, PL_COLOR_TRC_PQ) };
			const pl_color_space targets[] = {
				Space(PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_GAMMA22), Space(PL_COLOR_PRIM_BT_2020, PL_COLOR_TRC_GAMMA24),
				Space(PL_COLOR_PRIM_DISPLAY_P3, PL_COLOR_TRC_GAMMA22), Space(PL_COLOR_PRIM_BT_2020, PL_COLOR_TRC_PQ) };
			int cases = 0, active = 0;
			for (const pl_color_space& source : sources)
				for (const pl_color_space& target : targets)
					for (int bits = 0; bits < 32; ++bits)
					{
						ExtendedRangeStage::Frame f;
						f.preserve = (bits & 1) != 0;
						f.moreCases = (bits & 2) != 0;
						f.temporalActive = (bits & 4) != 0;
						f.temporalMixing = (bits & 8) != 0;
						f.singlePlaneRgb = (bits & 16) != 0;
						ExtendedRangeStage::Stage stage;
						ExtendedRange::Hooks todaysHooks;
						pl_color_space a = source, b = source;
						std::vector<const pl_hook*> listA, listB;
						ExtendedRange::Reason reason{};
						const bool relabelA = stage.Prepare(f, a, target, listA);
						const bool relabelB = TodaysExtendedRange(f, false, todaysHooks, b, target, listB, reason);
						Assert::AreEqual(relabelB, relabelA);
						Assert::IsTrue(pl_color_space_equal(&a, &b));
						Assert::IsTrue(Signatures(listA) == Signatures(listB));
						Assert::IsTrue(stage.LastDecision().reason == reason);
						Assert::AreEqual(stage.LastDecision().Active() && f.temporalMixing, stage.MixClipsBtb());
						++cases;
						active += relabelA ? 1 : 0;
					}
			Assert::AreEqual(5 * 4 * 32, cases);
			Assert::IsTrue(active > 0);   // the comparison covered active decisions, not only refusals
		}

		TEST_METHOD(ExtendedRangeHookFailureLatchesOffAndIsAFallback)
		{
			ExtendedRangeStage::Stage stage;
			ExtendedRangeStage::Frame f;
			f.preserve = f.moreCases = true;
			const pl_color_space target = Space(PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_GAMMA22);
			pl_color_space source = Space(PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_GAMMA24);
			std::vector<const pl_hook*> hooks;
			Assert::IsTrue(stage.Prepare(f, source, target, hooks));
			Assert::IsFalse(stage.Fallback(true).engaged);
			const uint64_t unrelated[] = { 0x1234ull };
			stage.NoteDisabledHooks(unrelated, 1);
			Assert::IsFalse(stage.HookFailed());
			const uint64_t carry[] = { stage.CarryHooks().Carry()->signature };
			stage.NoteDisabledHooks(carry, 1);
			Assert::IsTrue(stage.HookFailed());
			source = Space(PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_GAMMA24);
			hooks.clear();
			Assert::IsFalse(stage.Prepare(f, source, target, hooks));   // off until restart
			Assert::IsTrue(hooks.empty());
			const FallbackCatalog::Decision d = stage.Fallback(true);
			Assert::IsTrue(d.engaged);
			Assert::AreEqual(std::string("BTB/WTW"), d.id);
			Assert::IsFalse(stage.Fallback(false).engaged);   // preserve off: a choice, not a downgrade
		}

		TEST_METHOD(ExtendedRangeFrameMixIsVisible)
		{
			ExtendedRangeStage::Stage stage;
			ExtendedRangeStage::Frame f;
			f.preserve = f.moreCases = true;
			f.temporalActive = f.temporalMixing = true;
			const pl_color_space target = Space(PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_GAMMA22);
			pl_color_space source = Space(PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_GAMMA24);
			std::vector<const pl_hook*> hooks;
			stage.Prepare(f, source, target, hooks);
			if (stage.LastDecision().Active())
				Assert::IsTrue(stage.MixClipsBtb());
			Assert::IsTrue(stage.Fallback(true).engaged);   // either the mix clips BTB or the late restore cannot follow
		}

		// ---- scaling.output -------------------------------------------------------------------

		struct AdaptiveFixture
		{
			AdaptiveDownscale::Module module;
			AdaptiveDownscaleStage::Stage stage{ module };
			AdaptiveDownscale::Settings settings;
			double weight = 7.0;
			bool grainWanted = true;
			bool noSsim = false;
			int published = 0, motionAsks = 0, nlsAsks = 0, bitrateAsks = 0;
			bool ssim = false;
			bool reading = false;
			double quality = 1.0;
			AdaptiveDownscaleStage::Host host;
			AdaptiveFixture()
			{
				host.fallbackFramePeriod = []() { return 1.0f / 24.0f; };
				host.bitrate = [this](double& q) { ++bitrateAsks; q = quality; return reading; };
				host.nlsMapping = [this]() { ++nlsAsks; return NlsMaskMapping::Mapping(); };
				host.ssimInChain = [this]() { return ssim; };
				host.motion = [this]() { ++motionAsks; return AdaptiveDownscaleStage::MotionSource(); };
				host.publishStatus = [this]() { ++published; };
			}
			AdaptiveDownscaleStage::Frame Frame(float sourceH, float targetH)
			{
				AdaptiveDownscaleStage::Frame f;
				f.adaptive = &settings;
				f.sourceSequence = 5;
				f.nominalSourceRateHz = 24.0;
				f.sourceCrop = Rect(0, 0, sourceH * 16 / 9, sourceH);
				f.targetCrop = Rect(0, 0, targetH * 16 / 9, targetH);
				return f;
			}
			void Run(const AdaptiveDownscaleStage::Frame& f, std::vector<const pl_hook*>& hooks)
			{
				stage.Register(f, host, { weight, grainWanted, noSsim }, hooks);
			}
		};

		TEST_METHOD(AdaptiveStageOffAddsNothing)
		{
			AdaptiveFixture x;
			std::vector<const pl_hook*> hooks;
			x.Run(x.Frame(2160, 1080), hooks);   // adaptive_downscale and adaptive_upscale off
			Assert::IsTrue(hooks.empty());
			Assert::IsFalse(x.grainWanted);
			Assert::AreEqual(1.0, x.weight);
			Assert::AreEqual(0, x.published);
			Assert::AreEqual(0, x.motionAsks);
			Assert::IsFalse(x.stage.UpscaleThisFrame());
		}

		TEST_METHOD(AdaptiveDownscaleNeedsSsimThenMeasuresFirstAndBlendsLast)
		{
			AdaptiveFixture x;
			const pl_hook other{};
			std::vector<const pl_hook*> hooks{ &other };
			AdaptiveDownscaleStage::Frame f = x.Frame(2160, 1080);
			f.adaptiveDownscale = true;
			x.Run(f, hooks);
			Assert::AreEqual(size_t(1), hooks.size());   // no SSimDownscaler: not used
			Assert::IsTrue(x.noSsim);
			Assert::IsFalse(x.grainWanted);
			Assert::AreEqual(1, x.published);
			x.ssim = true;
			x.Run(f, hooks);
			Assert::AreEqual(size_t(3), hooks.size());
			Assert::IsTrue(hooks.front() == x.module.Measure());
			Assert::IsTrue(hooks.back() == x.module.Blend());
			Assert::IsFalse(x.noSsim);
			Assert::AreEqual(1, x.motionAsks);
			Assert::AreEqual(1, x.published);   // the status at most once a second
		}

		TEST_METHOD(AdaptiveOutputScalingFallbackIsVisible)
		{
			// Findings 2026-10-25 #1: SSimDownscaler missing while adaptive downscale is on engages
			// "output scaling"; with the shader present (and no GPU failure) it is released; off is a choice.
			AdaptiveFixture x;
			std::vector<const pl_hook*> hooks;
			AdaptiveDownscaleStage::Frame f = x.Frame(2160, 1080);
			x.Run(f, hooks);
			Assert::IsFalse(x.stage.Fallback().engaged);
			f.adaptiveDownscale = true;
			x.Run(f, hooks);
			const FallbackCatalog::Decision d = x.stage.Fallback();
			Assert::IsTrue(d.engaged);
			Assert::AreEqual(std::string("output scaling"), d.id);
			Assert::IsTrue(d.reason.find("SSimDownscaler") != std::string::npos);
			x.ssim = true;
			x.Run(f, hooks);
			Assert::IsFalse(x.stage.Fallback().engaged);
		}

		TEST_METHOD(AdaptiveGpuFailureIsHeldThenReleased)
		{
			// Audit M-2: a one-off GPU failure stays listed for kFailureHoldMs, then is released.
			AdaptiveFixture x;
			std::vector<const pl_hook*> hooks;
			AdaptiveDownscaleStage::Frame f = x.Frame(2160, 1080);
			f.adaptiveDownscale = true;
			x.ssim = true;
			x.Run(f, hooks);
			Assert::IsFalse(x.stage.Fallback().engaged);
			x.stage.NoteFailures(1, 100000);
			Assert::IsTrue(x.stage.Fallback().engaged);
			x.stage.NoteFailures(0, 100000 + AdaptiveDownscaleStage::Stage::kFailureHoldMs - 1);
			Assert::IsTrue(x.stage.Fallback().engaged);
			x.stage.NoteFailures(0, 100000 + AdaptiveDownscaleStage::Stage::kFailureHoldMs);
			Assert::IsFalse(x.stage.Fallback().engaged);
		}

		TEST_METHOD(AdaptiveUpscaleTakesTheUpscaleFormAndItsLight)
		{
			AdaptiveFixture x;
			std::vector<const pl_hook*> hooks;
			AdaptiveDownscaleStage::Frame f = x.Frame(1080, 2160);
			f.adaptiveUpscale = true;
			x.Run(f, hooks);
			Assert::IsTrue(x.stage.UpscaleThisFrame());
			Assert::AreEqual(size_t(2), hooks.size());
			Assert::IsTrue(hooks.front() == x.module.Measure());
			Assert::IsTrue(hooks.back() == x.module.Blend());   // no SSimSuperRes: appended
			pl_sigmoid_params sigmoid = pl_sigmoid_default_params;
			pl_render_params p = pl_render_default_params;
			p.sigmoid_params = &sigmoid;
			x.stage.ApplyLight(p, { false, false, f.sourceCrop, f.targetCrop });
			Assert::IsTrue(p.sigmoid_params == nullptr);         // linear: no sigmoid
			Assert::IsFalse(p.disable_linear_scaling);
			p = pl_render_default_params;
			x.stage.ApplyLight(p, { false, true, f.sourceCrop, f.targetCrop });
			Assert::IsTrue(p.disable_linear_scaling);            // gamma
		}

		TEST_METHOD(AdaptiveLowBitrateWeightIsTodays)
		{
			AdaptiveFixture x;
			std::vector<const pl_hook*> hooks;
			AdaptiveDownscaleStage::Frame f = x.Frame(2160, 1080);
			f.adaptiveDownscale = true;
			f.lowBitrateOn = true;
			f.ssimBitrateFloor = 0.4;
			x.reading = true;
			x.quality = 0.9;
			x.ssim = true;
			x.Run(f, hooks);
			Assert::AreEqual(AdaptiveDownscale::BitrateScale(0.4, 0.4), x.weight);   // "on" caps the quality at 0.4
			Assert::AreEqual(2, x.bitrateAsks);   // today's two monitor reads: the weight and the grain guard
			f.lowBitrateOn = false;
			f.lowBitrateOff = true;
			x.Run(f, hooks);
			Assert::AreEqual(1.0, x.weight);
		}

		TEST_METHOD(DownscaleLightGammaOnlyOnADownscalingFrame)
		{
			AdaptiveFixture x;
			pl_render_params p = pl_render_default_params;
			x.stage.ApplyLight(p, { true, false, Rect(0, 0, 3840, 2160), Rect(0, 0, 1920, 1080) });
			Assert::IsTrue(p.disable_linear_scaling);
			p = pl_render_default_params;
			x.stage.ApplyLight(p, { true, false, Rect(0, 0, 1920, 1080), Rect(0, 0, 3840, 2160) });
			Assert::IsFalse(p.disable_linear_scaling);
			Assert::IsTrue(p.sigmoid_params == pl_render_default_params.sigmoid_params);
		}

		// ---- motion.consumers: the de-judder rendered-picture cache ----------------------------

		TEST_METHOD(DejudderCacheArmsOnlyWhenExtrasMayFollow)
		{
			DejudderRendered::Cache cache;
			DejudderRendered::Geometry geometry;
			bool valid = true;
			DejudderCacheStage::Stage stage(cache, geometry, valid);
			int nlsAsks = 0;
			const auto nls = [&]() { ++nlsAsks; return NlsMaskMapping::Mapping(); };
			std::vector<const pl_hook*> hooks;
			DejudderCacheStage::Frame f;
			f.sourceSequence = 9;
			f.sourceCrop = Rect(0, 0, 1920, 1080);
			f.targetCrop = Rect(0, 0, 3840, 2160);
			f.framebufferW = 3840;
			f.framebufferH = 2160;
			stage.Register(f, nls, hooks);   // not eligible
			Assert::IsFalse(valid);
			Assert::IsTrue(hooks.empty());
			f.extrasMayFollow = true;
			const SubtitleBoxRect holds[] = { { 100, 900, 600, 980 }, {}, { 100, 800, 600, 880 }, {} };
			f.subtitleHolds = holds;
			f.numSubtitleHolds = 4;
			stage.Register(f, nls, hooks);
			Assert::IsTrue(valid);
			Assert::AreEqual(size_t(1), hooks.size());
			Assert::IsTrue(hooks[0] == cache.Hook());
			Assert::AreEqual(0, nlsAsks);
			Assert::AreEqual(2, geometry.numHolds);   // the two valid rectangles, 2 px margin
			Assert::AreEqual(98.0f, geometry.holds[0].x0);
			Assert::AreEqual(982.0f, geometry.holds[0].y1);
			const DejudderRendered::Rects rounded = DejudderRendered::Rounded(f.sourceCrop, f.targetCrop, 3840, 2160);
			Assert::AreEqual(rounded.target.x1, geometry.targetCrop.x1);
			// Interpolating: the geometry, no cache.
			hooks.clear();
			f.interpolating = true;
			stage.Register(f, nls, hooks);
			Assert::IsTrue(valid);
			Assert::IsTrue(hooks.empty());
			// An NLS shader whose map is not known: the source path (no geometry, no cache).
			f.interpolating = false;
			f.nlsSubmitted = true;
			stage.Register(f, nls, hooks);
			Assert::AreEqual(1, nlsAsks);
			Assert::IsFalse(valid);
			Assert::IsTrue(hooks.empty());
		}

		// ---- slot switches (pipeline_slots_off, Original pathway): off = byte-identical --------

		TEST_METHOD(ExtendedRangeSlotOffIsPreserveOff)
		{
			// Slot off decides, relabels and places hooks exactly as preserve_extended_range off.
			const pl_color_space target = Space(PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_GAMMA22);
			for (int moreCases = 0; moreCases < 2; ++moreCases)
			{
				ExtendedRangeStage::Frame off;
				off.preserve = true;
				off.moreCases = moreCases != 0;
				off.slotRuns = false;
				ExtendedRangeStage::Frame preserveOff = off;
				preserveOff.preserve = false;
				preserveOff.slotRuns = true;
				ExtendedRangeStage::Stage a, b;
				pl_color_space ca = Space(PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_GAMMA24), cb = ca;
				std::vector<const pl_hook*> la, lb;
				Assert::AreEqual(b.Prepare(preserveOff, cb, target, lb), a.Prepare(off, ca, target, la));
				Assert::IsTrue(pl_color_space_equal(&ca, &cb));
				Assert::IsTrue(Signatures(la) == Signatures(lb));
				Assert::IsTrue(la.empty());
				Assert::IsTrue(a.LastDecision().reason == ExtendedRange::Reason::Off);
				Assert::IsFalse(a.Fallback(true).engaged);   // a choice, not a downgrade
				// On again: active.
				off.slotRuns = true;
				ca = Space(PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_GAMMA24);
				la.clear();
				Assert::IsTrue(a.Prepare(off, ca, target, la));
				Assert::IsFalse(la.empty());
			}
		}

		TEST_METHOD(OutputScalingSlotOffIsAdaptiveOff)
		{
			// Slot off = adaptive_downscale and adaptive_upscale off (no hooks, no grain-map request,
			// no fallback) and libplacebo's linear light.
			for (int up = 0; up < 2; ++up)
			{
				AdaptiveFixture x;
				x.ssim = true;
				std::vector<const pl_hook*> hooks;
				AdaptiveDownscaleStage::Frame f = up ? x.Frame(1080, 2160) : x.Frame(2160, 1080);
				f.adaptiveDownscale = f.adaptiveUpscale = true;
				f.slotRuns = false;
				x.Run(f, hooks);
				Assert::IsTrue(hooks.empty());
				Assert::IsFalse(x.grainWanted);
				Assert::IsFalse(x.stage.UpscaleThisFrame());
				Assert::IsFalse(x.stage.Fallback().engaged);
				pl_render_params p = pl_render_default_params;
				x.stage.ApplyLight(p, { true, true, f.sourceCrop, f.targetCrop, false });
				Assert::IsFalse(p.disable_linear_scaling);
				Assert::IsTrue(p.sigmoid_params == pl_render_default_params.sigmoid_params);
				f.slotRuns = true;   // on again: the module's hooks
				x.Run(f, hooks);
				Assert::IsFalse(hooks.empty());
			}
		}

		TEST_METHOD(GrainMapNeverAsksForVectorsThatDoNotRun)
		{
			// feat/m2-motion: with motion.estimation or motion.vector_delivery off the grain map must not
			// start an estimate (Frame::vectorsRun = MotionVectorsRun()). Default: today's request.
			AdaptiveFixture x;
			x.ssim = true;
			std::vector<const pl_hook*> hooks;
			AdaptiveDownscaleStage::Frame f = x.Frame(2160, 1080);
			f.adaptiveDownscale = true;
			f.vectorsRun = false;
			x.grainWanted = true;
			x.Run(f, hooks);
			Assert::IsFalse(x.grainWanted);
			Assert::AreEqual(size_t(2), hooks.size());   // the blend itself still runs
		}

		TEST_METHOD(DitherSlotOffRemovesOnlyTheQuantiser)
		{
			const pl_hook a{}, quantiser{}, b{};
			std::vector<const pl_hook*> hooks{ &a, &quantiser, &b };
			const size_t removed = DitherSlot::RemoveQuantisers(hooks, [&](const pl_hook* h) { return h == &quantiser; });
			Assert::AreEqual(size_t(1), removed);
			Assert::AreEqual(size_t(2), hooks.size());
			Assert::IsTrue(hooks[0] == &a && hooks[1] == &b);   // the rest keep their order
		}

		TEST_METHOD(SlotOffStopsTheSlotsOtherPasses)
		{
			// Modularity sweep 2026-10-25: with output.dither off no vector producer runs for it and
			// libplacebo keeps its dither (also with temporal_dither: off); with scaling.output off
			// libplacebo's FBOs keep their default format.
			Assert::IsTrue(DitherSlot::VectorsWanted(true, true));
			Assert::IsFalse(DitherSlot::VectorsWanted(false, true));
			Assert::IsFalse(DitherSlot::VectorsWanted(true, false));
			Assert::IsTrue(DitherSlot::ClearsLibplaceboDither(true, false, true));
			Assert::IsTrue(DitherSlot::ClearsLibplaceboDither(false, true, true));    // temporal_dither off, slot on
			Assert::IsFalse(DitherSlot::ClearsLibplaceboDither(false, true, false));  // slot off: upstream dithers
			Assert::IsFalse(DitherSlot::ClearsLibplaceboDither(false, false, true));
			Assert::IsTrue(AdaptiveDownscaleStage::FboFloat32(-1, true));    // auto = on
			Assert::IsFalse(AdaptiveDownscaleStage::FboFloat32(-1, false));  // slot off
			Assert::IsFalse(AdaptiveDownscaleStage::FboFloat32(0, true));
		}

		TEST_METHOD(OriginalPathwayTurnsTheExtendedRangeSlotOff)
		{
			// colour.extended_range is a VP addition: the Original pathway switches it off; dither and
			// output scaling are mixed slots and stay in their upstream form (their VP keys are reset
			// by the Original pathway overrides).
			PipelineSlots::Registry r;
			Assert::IsTrue(r.Runs(PipelineSlots::Id::ColourExtendedRange));
			r.ApplyOriginalPathway();
			Assert::IsFalse(r.Runs(PipelineSlots::Id::ColourExtendedRange));
			Assert::IsTrue(r.Runs(PipelineSlots::Id::OutputDither));
			Assert::IsTrue(r.Runs(PipelineSlots::Id::ScalingOutput));
		}

		// ---- output.dither: the sigma-delta's vectors -----------------------------------------

		TEST_METHOD(NextVectorGuardTouchesOnlySigmaDeltaHooks)
		{
			const pl_hook plain{};
			const pl_hook* hooks[] = { &plain, nullptr };
			SigmaDeltaVectors::NextVectorGuard guard;
			Assert::IsFalse(guard.Apply(hooks, 2, nullptr));   // no hook takes vector_source
			guard.Release();
#ifndef PL_VP_MPV_USER_SHADER_SET_TEXTURE
			Assert::IsFalse(SigmaDeltaVectors::Supported());   // libplacebo 7.360: the sigma-delta searches itself
			Assert::IsFalse(SetSigmaDeltaVectors(&plain, nullptr));
#endif
		}
	};
}
