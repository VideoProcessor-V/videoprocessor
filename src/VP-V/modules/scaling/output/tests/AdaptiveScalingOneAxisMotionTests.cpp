// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Stage 8b one-axis (N9-P12) and the motion-adapted scaler blend (owner 2026-10-31):
//  - AdaptiveScalingOneAxisMotionTests (CPU): the either-axis gate, the motion weight's CPU mirror,
//    the LUMA prescaler recognition, and the stage's hook list / status lines.
//  - AdaptiveScalingMotionHookTests (GpuWarp, WARP unless VP_TEST_GPU=hardware): the motion map on
//    the GPU, the blend on fast / steady motion and the one-axis upscale = the faithful branch.
#include "pch.h"
#include <VP-V/tests/VpLutGpuTestSupport.h>
#include <VP-V/modules/output/dither/CustomHookChain.h>
#include <VP-V/modules/scaling/output/AdaptiveDownscaleStage.h>
#include <VP-V/modules/scaling/shared/HybridBlend.h>
#include <VP-V/core/PipelineSlots.h>
#include <VP-V/core/Products.h>
#include <VP-V/core/OnePole.h>
#include <VP-V/core/hub/CurrentFrame.h>
#include <VP-V/core/hub/ExtensionHub.h>
#include <VP-V/modules/input/source_analysis/SourceFrameTap.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#if __has_include(<VP-V/modules/scaling/output/AdaptiveDownscaleCut.h>)
#define X5AB_TIP_CUT 1
#include <VP-V/modules/scaling/output/AdaptiveDownscaleCut.h>
#include <VP-V/modules/input/source_analysis/lane/shared/SourceStats.h>
#include <VP-V/modules/input/source_analysis/events/shared/CutFadeJudge.h>   // the X5 A/B harness models the scene product with U7's CutTracker
#endif

namespace VideoProcessorTest
{
	namespace
	{
		pl_rect2df OneAxisRect(float w, float h) { return pl_rect2df{ 0.0f, 0.0f, w, h }; }

		bool HasLine(const std::vector<std::string>& lines, const char* part)
		{
			for (const std::string& l : lines)
				if (l.find(part) != std::string::npos) return true;
			return false;
		}

		struct OneAxisStageFixture
		{
			AdaptiveDownscale::Module module;
			AdaptiveDownscaleStage::Stage stage{ module };
			AdaptiveDownscale::Settings settings;
			double weight = 1.0;
			bool grainWanted = false;
			bool noSsim = false;
			int published = 0;
			bool ssim = true;
			const pl_hook* prescaler = nullptr;
			AdaptiveDownscaleStage::Host host;
			OneAxisStageFixture()
			{
				host.fallbackFramePeriod = []() { return 1.0f / 24.0f; };
				host.bitrate = [](double& q) { q = 1.0; return false; };
				host.nlsMapping = []() { return NlsMaskMapping::Mapping(); };
				host.ssimInChain = [this]() { return ssim; };
				host.motion = []() { return AdaptiveDownscaleStage::MotionSource(); };
				host.publishStatus = [this]() { ++published; };
				host.isPrescaler = [this](const pl_hook* h) { return h != nullptr && h == prescaler; };
			}
			AdaptiveDownscaleStage::Frame Frame(float sw, float sh, float tw, float th)
			{
				AdaptiveDownscaleStage::Frame f;
				f.adaptive = &settings;
				f.sourceSequence = 3;
				f.nominalSourceRateHz = 24.0;
				f.sourceCrop = OneAxisRect(sw, sh);
				f.targetCrop = OneAxisRect(tw, th);
				return f;
			}
			void Run(const AdaptiveDownscaleStage::Frame& f, std::vector<const pl_hook*>& hooks)
			{
				stage.Register(f, host, { weight, grainWanted, noSsim }, hooks);
			}
		};
	}

	TEST_CLASS(AdaptiveScalingOneAxisMotionTests)
	{
	public:
		TEST_METHOD(UpscaleGateIsEitherAxisAndOneAxisIsExactlyOne)
		{
			using AdaptiveDownscale::UpscaleAxesOf;
			AdaptiveDownscale::UpscaleAxes a = UpscaleAxesOf(1920, 1080, 2560, 1440);
			Assert::IsTrue(a.any && !a.oneAxis, L"both axes");
			a = UpscaleAxesOf(2560, 1080, 2560, 1440);
			Assert::IsTrue(a.any && a.oneAxis, L"height only (V)");
			a = UpscaleAxesOf(1920, 1440, 2560, 1440);
			Assert::IsTrue(a.any && a.oneAxis, L"width only (H): missed by the old height-only gate");
			a = UpscaleAxesOf(3840, 1080, 2560, 1440);
			Assert::IsTrue(a.any && !a.oneAxis, L"mixed (V up, H down): today's two-axis upscale blend");
			a = UpscaleAxesOf(1920, 2160, 2560, 1440);
			Assert::IsFalse(a.any, L"mixed (H up, V down): today's downscale form");
			a = UpscaleAxesOf(3840, 2160, 2560, 1440);
			Assert::IsFalse(a.any, L"a downscale");
			a = UpscaleAxesOf(2560.3f, 1440.2f, 2560, 1440);
			Assert::IsFalse(a.any, L"within half a pixel: 1:1");
			a = UpscaleAxesOf(0, 1080, 2560, 1440);
			Assert::IsTrue(a.any && !a.oneAxis, L"no width given: the height decides (today's gate)");
		}

		TEST_METHOD(MotionTargetRampsBySpeedAndFloorsUntrusted)
		{
			AdaptiveDownscale::Settings s;
			s.motionSpeedLo = 0.1f;
			s.motionSpeedHi = 0.4f;
			s.motionFloor = 0.25f;
			using AdaptiveDownscale::MotionTarget;
			Assert::AreEqual(1.0f, MotionTarget(true, true, 0.0f, 1.0f, s), L"steady: the sharper branch's full weight");
			Assert::AreEqual(1.0f, MotionTarget(true, true, 0.1f, 1.0f, s));
			Assert::AreEqual(0.25f, MotionTarget(true, true, 0.4f, 1.0f, s), L"fast: the floor");
			Assert::AreEqual(0.25f, MotionTarget(true, true, 5.0f, 1.0f, s));
			Assert::AreEqual(0.25f + 0.75f * 0.5f, MotionTarget(true, true, 0.25f, 1.0f, s), 1e-6f, L"halfway: smoothstep 0.5");
			Assert::AreEqual(0.25f, MotionTarget(true, false, 0.0f, 1.0f, s), L"untrusted block: the floor");
			Assert::AreEqual(0.25f, MotionTarget(false, true, 0.0f, 1.0f, s), L"untrusted frame: the floor");
			Assert::AreEqual(0.25f + 0.75f * 0.5f, MotionTarget(true, true, 0.0f, 0.5f, s), 1e-6f, L"confidence scales it");
			// Monotone in speed.
			float last = 2.0f;
			for (int i = 0; i <= 50; ++i)
			{
				const float t = MotionTarget(true, true, i * 0.01f, 1.0f, s);
				Assert::IsTrue(t <= last + 1e-7f);
				last = t;
			}
		}

		TEST_METHOD(MotionSpeedIsGeneralOverSizesAndRates)
		{
			using AdaptiveDownscale::MotionSpeed;
			// The same motion in picture heights per second at 1080p24, 2160p24 (2x the pixels) and
			// 1080p48 (half the pixels per frame): no hard-coded size or rate.
			const float a = MotionSpeed(4.0f, 0.0f, 1080.0f, 1.0f / 24.0f);
			Assert::AreEqual(a, MotionSpeed(8.0f, 0.0f, 2160.0f, 1.0f / 24.0f), 1e-6f);
			Assert::AreEqual(a, MotionSpeed(2.0f, 0.0f, 1080.0f, 1.0f / 48.0f), 1e-6f);
			Assert::AreEqual(5.0f * 24.0f / 1080.0f, MotionSpeed(3.0f, 4.0f, 1080.0f, 1.0f / 24.0f), 1e-6f);
			Assert::AreEqual(0.0f, MotionSpeed(4.0f, 0.0f, 0.0f, 1.0f / 24.0f), L"no crop: no speed");
		}

		TEST_METHOD(MotionStepIsAOnePoleThatResets)
		{
			using namespace AdaptiveDownscale;
			const float alpha = OnePole::AlphaForMs((1.0f / 24.0f) * 1000.0f, 250.0f, OnePole::DtRule::AsGiven);
			Assert::AreEqual(1.0f - std::exp(-1000.0f / 24.0f / 250.0f), alpha, 1e-6f);
			Assert::AreEqual(1.0f, OnePole::AlphaForMs((1.0f / 24.0f) * 1000.0f, 0.0f, OnePole::DtRule::AsGiven), L"0 ms: no smoothing");
			// Same time constant at 24 and 60 fps: after 1 s both are within 1 % of each other.
			float v24 = 1.0f, v60 = 1.0f;
			for (int i = 0; i < 24; ++i) v24 = MotionStep(v24, 0.0f, OnePole::AlphaForMs((1.0f / 24.0f) * 1000.0f, 250.0f, OnePole::DtRule::AsGiven), false);
			for (int i = 0; i < 60; ++i) v60 = MotionStep(v60, 0.0f, OnePole::AlphaForMs((1.0f / 60.0f) * 1000.0f, 250.0f, OnePole::DtRule::AsGiven), false);
			Assert::AreEqual(v24, v60, 0.01f);
			Assert::AreEqual(0.25f, MotionStep(1.0f, 0.25f, alpha, true), L"a cut takes the target");
			const float one = MotionStep(1.0f, 0.0f, alpha, false);
			Assert::IsTrue(one > 0.8f && one < 0.9f, L"no jump at a motion boundary (one frame moves ~15 %)");
		}

		TEST_METHOD(MotionAdaptiveIsOffByDefaultAndPartOfTheSettings)
		{
			AdaptiveDownscale::Settings a, b;
			Assert::IsFalse(a.motionAdaptive);
			Assert::IsTrue(a == b);
			b.motionAdaptive = true;
			Assert::IsFalse(a == b);
			b = a; b.motionSpeedLo = 0.2f; Assert::IsFalse(a == b);
			b = a; b.motionSpeedHi = 0.6f; Assert::IsFalse(a == b);
			b = a; b.motionFloor = 0.5f; Assert::IsFalse(a == b);
			b = a; b.motionSmoothMs = 100.0f; Assert::IsFalse(a == b);
		}

		TEST_METHOD(LumaPrescalerRecognisedBySourceNotName)
		{
			using CustomHookChain::IsLumaPrescaler;
			Assert::IsTrue(IsLumaPrescaler(LoadBundledShader("FSRCNNX_x2_16-0-4-1.glsl")), L"FSRCNNX");
			Assert::IsTrue(IsLumaPrescaler(LoadBundledShader("ArtCNN_C4F16.glsl")), L"ArtCNN");
			for (const char* other : { "CfL_Prediction.glsl", "SSimDownscaler.glsl", "SSimSuperRes.glsl", "NLS.glsl",
				"Temporal sigma-delta.glsl" })
			{
				const std::string src = LoadBundledShader(other);
				Assert::IsFalse(src.empty(), L"bundled shader missing");
				Assert::IsFalse(IsLumaPrescaler(src), std::wstring(other, other + strlen(other)).c_str());
			}
			Assert::IsTrue(IsLumaPrescaler("//!HOOK LUMA\r\n//!BIND HOOKED\r\n//!WIDTH HOOKED.w 2 *\r\n//!HEIGHT HOOKED.h 2 *\r\nvec4 hook() { return HOOKED_tex(HOOKED_pos); }\r\n"),
				L"a HOOKED-form doubler with CRLF");
			Assert::IsFalse(IsLumaPrescaler("//!HOOK LUMA\n//!WIDTH LUMA.w 1.0 *\n//!HEIGHT LUMA.h 1.0 *\n"), L"1x: not a prescaler");
			Assert::IsFalse(IsLumaPrescaler("//!HOOK MAIN\n//!WIDTH MAIN.w 2 *\n"), L"MAIN: not a LUMA prescaler");
			Assert::IsFalse(IsLumaPrescaler("//!HOOK LUMA\n//!SAVE X\n//!WIDTH LUMA.w 2 * 1 +\n"), L"a longer expression");
			Assert::IsFalse(IsLumaPrescaler("//!HOOK CHROMA\n//!WIDTH LUMA.w 2 *\n//!HOOK LUMA\n//!DESC plain\n"),
				L"the size belongs to a CHROMA pass");
		}

		TEST_METHOD(OneAxisUpscaleDropsThePrescalerAndSaysSo)
		{
			OneAxisStageFixture x;
			const pl_hook doubler{}, other{};
			x.prescaler = &doubler;
			std::vector<const pl_hook*> hooks{ &doubler, &other };
			AdaptiveDownscaleStage::Frame f = x.Frame(2560, 1080, 2560, 1440);   // V only
			f.adaptiveUpscale = true;
			x.Run(f, hooks);
			Assert::IsTrue(x.stage.UpscaleThisFrame());
			Assert::IsTrue(x.stage.OneAxisThisFrame());
			Assert::AreEqual(1, x.stage.DroppedPrescalers());
			Assert::AreEqual(size_t(3), hooks.size());
			Assert::IsTrue(hooks[0] == x.module.Measure() && hooks[1] == &other && hooks[2] == x.module.Blend(),
				L"measure, the other hook, blend; the doubler gone");
			const std::vector<std::string> lines = x.stage.StatusLines();
			// No GPU here: the module never measured, so the line says the blend did not run yet (never a claim).
			Assert::IsTrue(HasLine(lines, "Adaptive upscale: not applied") && HasLine(lines, "CNN doubler skipped: one-axis"));
			Assert::AreEqual(1, x.published, L"the new status is published once");
			std::vector<const pl_hook*> again{ &doubler, &other };
			x.Run(f, again);
			Assert::AreEqual(1, x.published, L"unchanged status: not republished");
		}

		TEST_METHOD(BothAxesUpscaleKeepsThePrescaler)
		{
			OneAxisStageFixture x;
			const pl_hook doubler{}, other{};
			x.prescaler = &doubler;
			std::vector<const pl_hook*> hooks{ &doubler, &other };
			AdaptiveDownscaleStage::Frame f = x.Frame(1920, 1080, 2560, 1440);
			f.adaptiveUpscale = true;
			x.Run(f, hooks);
			Assert::IsTrue(x.stage.UpscaleThisFrame());
			Assert::IsFalse(x.stage.OneAxisThisFrame());
			Assert::AreEqual(0, x.stage.DroppedPrescalers());
			Assert::AreEqual(size_t(4), hooks.size());
			Assert::IsTrue(hooks[1] == &doubler, L"the doubler stays");
			Assert::IsTrue(HasLine(x.stage.StatusLines(), "Adaptive upscale: not applied") && !HasLine(x.stage.StatusLines(), "skipped"));
		}

		TEST_METHOD(WidthOnlyUpscaleTakesTheUpscaleForm)
		{
			// The height-only gate missed it; now either axis opens the gate.
			OneAxisStageFixture x;
			std::vector<const pl_hook*> hooks;
			AdaptiveDownscaleStage::Frame f = x.Frame(1920, 1440, 2560, 1440);
			f.adaptiveUpscale = true;
			x.Run(f, hooks);
			Assert::IsTrue(x.stage.UpscaleThisFrame());
			Assert::IsTrue(x.stage.OneAxisThisFrame());
			Assert::AreEqual(0, x.stage.DroppedPrescalers(), L"no prescaler in the list");
			Assert::IsTrue(HasLine(x.stage.StatusLines(), "Adaptive upscale: not applied") && !HasLine(x.stage.StatusLines(), "skipped"));
		}

		TEST_METHOD(UpscaleOffOrSlotOffIsUnchanged)
		{
			OneAxisStageFixture x;
			const pl_hook doubler{};
			x.prescaler = &doubler;
			std::vector<const pl_hook*> hooks{ &doubler };
			AdaptiveDownscaleStage::Frame f = x.Frame(2560, 1080, 2560, 1440);
			x.Run(f, hooks);   // adaptive_upscale off
			Assert::AreEqual(size_t(1), hooks.size());
			Assert::IsFalse(x.stage.UpscaleThisFrame());
			Assert::IsTrue(x.stage.StatusLines().empty());
			Assert::AreEqual(0, x.published);
			f.adaptiveUpscale = true;
			f.slotRuns = false;   // the Original pathway
			x.Run(f, hooks);
			Assert::AreEqual(size_t(1), hooks.size());
			Assert::IsFalse(x.stage.OneAxisThisFrame());
			Assert::IsTrue(x.stage.StatusLines().empty());
		}

		TEST_METHOD(MotionAdaptiveStatusIsVisibleWithoutVectors)
		{
			OneAxisStageFixture x;
			std::vector<const pl_hook*> hooks;
			AdaptiveDownscaleStage::Frame f = x.Frame(3840, 2160, 2560, 1440);
			f.adaptiveDownscale = true;
			x.Run(f, hooks);
			Assert::IsTrue(x.stage.StatusLines().empty(), L"off: no line");
			x.settings.motionAdaptive = true;
			hooks.clear();
			x.Run(f, hooks);
			Assert::IsTrue(HasLine(x.stage.StatusLines(), "Motion-adapted blend: idle"), L"no silent fallback: the module has not measured");
			f.slotRuns = false;
			hooks.clear();
			x.Run(f, hooks);
			Assert::IsTrue(x.stage.StatusLines().empty(), L"slot off: nothing");
		}

		// ---- CF-2 B1 (refs/123 v2 N1.2 / N1.4): the hybrid law, the edge / grain seams, the motion fallbacks ----
		// GPU rows the W4 build must prove (not provable on the CPU): (G1) the published kEdgeMap sampled at the output
		// pixel's source position (AD_EDGE_PUBLISHED) gives the private term's ramp within FIX-UPS-edge's domain note;
		// (G2) the shader motion floor (AD_MOTION_FLOOR) gives the CPU Weight() below; (G3) coded-input grain bands
		// (flat / dark-flat >= 0.95, texture >= 0.90 retention) on a real encode; (G4) OriginalPathwayByteIdenticalToBill
		// unchanged; (G5) AdaptiveUpscaleBlendsCnnAndFaithfulByGrain with the edge term now in the law.

		TEST_METHOD(HybridBaseRatioIsGeneralOverOutputRatiosAndUnequalAxes)
		{
			using AdaptiveDownscale::UpscaleBaseRatioOf;
			// No prescaler (PRE_KERNEL = the native source): ratio 1 whatever the output ratio, equal or unequal axes.
			const float ratios[][2] = { { 1.48f, 1.48f }, { 2.0f, 2.0f }, { 2.22f, 2.22f }, { 3.0f, 3.0f }, { 3.75f, 3.75f },
				{ 1.48f, 2.22f }, { 3.75f, 1.48f }, { 2.0f, 3.0f }, { 1.0f, 2.0f }, { 2.0f, 1.0f } };
			const float nw = 1024.0f, nh = 576.0f;
			for (const auto& r : ratios)
			{
				const int dw = static_cast<int>(std::lround(nw * r[0])), dh = static_cast<int>(std::lround(nh * r[1]));
				const std::wstring m = L"output ratio " + std::to_wstring(r[0]) + L" x " + std::to_wstring(r[1]);
				Assert::AreEqual(1, UpscaleBaseRatioOf(nw, nh, nw, nh, dw, dh), m.c_str());
				// A 2x LUMA prescaler (the CNN evaluation arm): ratio 2 at every output ratio.
				Assert::AreEqual(2, UpscaleBaseRatioOf(nw, nh, 2.0f * nw, 2.0f * nh, dw, dh), m.c_str());
				// Any other PRE_KERNEL size: not applied (the visible "not applied" line, never a guessed ratio).
				Assert::AreEqual(0, UpscaleBaseRatioOf(nw, nh, 1.5f * nw, 1.5f * nh, dw, dh), m.c_str());
				Assert::AreEqual(0, UpscaleBaseRatioOf(nw, nh, 2.0f * nw, nh, dw, dh), (m + L": a one-axis prescale").c_str());
			}
			// No upscale on either axis: 0 (the downscale and native forms are untouched).
			Assert::AreEqual(0, UpscaleBaseRatioOf(nw, nh, nw, nh, static_cast<int>(nw), static_cast<int>(nh)));
			Assert::AreEqual(0, UpscaleBaseRatioOf(nw, nh, nw, nh, 512, 288));
			// The gate agrees on unequal axes (either axis upscaled; the other 1:1 = one axis).
			Assert::IsTrue(AdaptiveDownscale::UpscaleAxesOf(1024, 576, 3840, 853).any);
			Assert::IsFalse(AdaptiveDownscale::UpscaleAxesOf(1024, 576, 3840, 853).oneAxis);
			Assert::IsTrue(AdaptiveDownscale::UpscaleAxesOf(1024, 576, 3840, 576).oneAxis);
		}

		TEST_METHOD(HybridWeightOrderMotionAfterTheClampAndTheEdgeTerm)
		{
			using namespace HybridBlend;
			Terms t;
			Assert::AreEqual(1.0f, Weight(t), L"defaults: the sharp branch");
			t.wBase = 1.6f;   // an over-unity setting x bitrate term clamps first ...
			t.motion = 0.5f;  // ... then the motion term scales (today's order)
			Assert::AreEqual(0.5f, Weight(t), 1e-6f);
			t.wBase = 0.8f;
			t.edge = 0.25f;
			t.grain = 0.5f;
			t.motion = 1.0f;
			Assert::AreEqual(0.8f * 0.75f * 0.5f, Weight(t), 1e-6f, L"edge term 1 - e, grain multiplies");
			t.edge = 1.0f;
			Assert::AreEqual(0.0f, Weight(t), L"a full edge: the alias-safe branch");
			t.edge = 2.0f;
			Assert::AreEqual(0.0f, Weight(t), L"e clamps to 1");
			t.edge = -1.0f;
			Assert::AreEqual(0.4f, Weight(t), 1e-6f, L"e clamps to 0");
			Assert::AreEqual(0.3f, MotionTerm(true, 0.3f, 0.25f), 1e-6f, L"valid vectors: the map's factor");
			Assert::AreEqual(0.25f, MotionTerm(false, 0.9f, 0.25f), 1e-6f, L"no or invalid vectors: the floor (alias-safe lean)");
			Assert::AreEqual(1.0f, MotionTerm(false, 0.0f, 3.0f), L"the floor clamps");
			// Monotone: more edge never raises the sharp weight.
			for (int i = 0; i < 20; ++i)
			{
				Terms a, b;
				a.wBase = b.wBase = 0.9f;
				a.edge = i * 0.05f;
				b.edge = a.edge + 0.05f;
				Assert::IsTrue(Weight(b) <= Weight(a) + 1e-7f);
			}
			// The GLSL text is the same law (order and clamps).
			const std::string glsl = WeightGlsl();
			Assert::IsTrue(glsl.find("clamp(w_base * (1.0 - clamp(e, 0.0, 1.0)) * g, 0.0, 1.0) * clamp(m, 0.0, 1.0)") != std::string::npos);
		}

		TEST_METHOD(HybridBlendPassesBtbWtwAndTwelveBitCodes)
		{
			using namespace HybridBlend;
			const float kw[3] = { 0.2627f, 0.6780f, 0.0593f };   // BT.2020 luma weights (the probe is R = G = B)
			const float ws[] = { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };
			// Below black (40 of 1023 < 64) and above white (960, 1023 > 940): no clamp of the output, exact through the blend.
			const float probes[] = { 4.0f / 1023.0f, 40.0f / 1023.0f, 960.0f / 1023.0f, 1023.0f / 1023.0f };
			for (float w : ws)
				for (float v : probes)
				{
					Assert::AreEqual(v, BlendLuma(v, v, w), L"equal branches: the code itself");
					const float sharp[3] = { v, v, v }, safe[3] = { v, v, v };
					float out[3];
					BlendRgb(sharp, safe, kw, w, out);
					for (int c = 0; c < 3; ++c)
						Assert::AreEqual(v, out[c], 1e-7f, L"BTB and WTW survive exactly");
				}
			// An excursion between branches stays where the branches put it (no 0..1 clamp, no limited-range clamp).
			Assert::AreEqual(-0.02f, BlendLuma(-0.02f, -0.02f, 0.3f), 1e-7f);
			Assert::AreEqual(1.03f, BlendLuma(1.05f, 1.01f, 0.5f), 1e-6f);
			// 12-bit ramp 0..4095: equal branches exact; a one-code branch difference lands between the two codes.
			for (int code = 0; code <= 4095; ++code)
			{
				const float v = code / 4095.0f;
				const int up = (std::min)(code + 1, 4095);
				const float v1 = up / 4095.0f;
				for (float w : ws)
				{
					Assert::AreEqual(code, static_cast<int>(std::lround(BlendLuma(v, v, w) * 4095.0f)));
					const float y = BlendLuma(v, v1, w) * 4095.0f;
					Assert::IsTrue(y >= code - 1e-3f && y <= up + 1e-3f);
				}
			}
		}

		TEST_METHOD(HybridGrainBandsRetainTheGrainCpuModel)
		{
			// CPU law model (the coded-input row G3 is for W4): a grain field n is carried by both branches; in the flat and
			// dark-flat bands both scalers reproduce it, on texture the alias-safe branch is modelled at 0.92 of it.
			// Retention = rms(out - base) / rms(n) over the law's whole term grid (edge, grain, motion incl. the floor).
			using namespace HybridBlend;
			auto noise = [](int i) { return static_cast<float>(((static_cast<unsigned>(i) * 1103515245u + 12345u) >> 16) & 1023u) / 1023.0f - 0.5f; };
			struct Band { const wchar_t* name; float base; float texture; float safeGain; double floor; };
			const Band bands[] = { { L"flat", 512.0f / 1023.0f, 0.0f, 1.0f, 0.95 }, { L"dark flat", 80.0f / 1023.0f, 0.0f, 1.0f, 0.95 },
				{ L"texture", 512.0f / 1023.0f, 0.08f, 0.92f, 0.90 } };
			for (const Band& b : bands)
				for (int ei = 0; ei <= 4; ++ei)
					for (int gi = 0; gi <= 4; ++gi)
						for (int mi = 0; mi <= 4; ++mi)
						{
							Terms t;
							t.edge = ei * 0.25f;
							t.grain = gi * 0.25f;
							t.motion = MotionTerm(mi < 4, mi * 0.25f, 0.25f);
							const float w = Weight(t);
							double num = 0.0, den = 0.0;
							for (int i = 0; i < 4096; ++i)
							{
								const float base = b.base + b.texture * std::sin(i * 0.37f);
								const float n = 4.0f / 1023.0f * noise(i);
								const float out = BlendLuma(base + n, base + b.safeGain * n, w);
								num += static_cast<double>(out - base) * (out - base);
								den += static_cast<double>(n) * n;
							}
							const double retention = std::sqrt(num / den);
							Assert::IsTrue(retention >= b.floor, (std::wstring(b.name) + L" retention " + std::to_wstring(retention)).c_str());
						}
		}

		TEST_METHOD(HybridEdgeSeamAsksOncePerFrameForTheStageVariant)
		{
			OneAxisStageFixture x;
			int asks = 0, motionReads = 0;
			VpSkeleton::ProductId askedId = VpSkeleton::kNoProduct;
			VpSkeleton::VariantKey askedKey;
			uint64_t askedSeq = 0;
			x.host.acquire = [&](VpSkeleton::ProductId id, uint64_t seq, const VpSkeleton::VariantKey& key)
			{
				++asks;
				askedId = id;
				askedKey = key;
				askedSeq = seq;
				VpSkeleton::AcquireResult r;
				r.cause = VpSkeleton::AcquireCause::VariantUndeclared;   // this tip: the owner serves no stage variant
				return r;
			};
			x.host.motion = [&]() { ++motionReads; return AdaptiveDownscaleStage::MotionSource(); };
			std::vector<const pl_hook*> hooks;
			AdaptiveDownscaleStage::Frame f = x.Frame(1920, 1080, 2560, 1440);
			f.adaptiveUpscale = true;
			x.Run(f, hooks);
			Assert::AreEqual(1, asks, L"one Acquire per frame per product");
			Assert::AreEqual(1, motionReads, L"one motion read per frame");
			Assert::IsTrue(askedId == VpSkeleton::kEdgeMap);
			Assert::AreEqual(static_cast<int>(VpSkeleton::VariantKey::StageAfter(static_cast<uint8_t>(PipelineSlots::Id::ScalingGeometry))),
				static_cast<int>(askedKey.stage), L"the planes leaving scaling.geometry");
			Assert::IsTrue(askedKey.cropX == 0 && askedKey.cropY == 0 && askedKey.cropW == 1920 && askedKey.cropH == 1080, L"the source crop");
			Assert::IsTrue(askedSeq == 3);
			Assert::IsFalse(x.stage.EdgeTerm().published);
			Assert::IsTrue(x.stage.EdgeTerm().cause.find("variant not served by the owner") != std::string::npos);
			Assert::IsFalse(x.stage.GrainTerm().published);
			Assert::IsTrue(HasLine(x.stage.StatusLines(), "edge/grain: private interim (edge: kEdgeMap stage variant"),
				L"no silent fallback: the interim is on the status");
			Assert::IsTrue(HasLine(x.stage.StatusLines(), "P10g grain product is not published"));
			f.sourceSequence = 4;
			hooks.clear();
			x.Run(f, hooks);
			Assert::AreEqual(2, asks);
			Assert::AreEqual(2, motionReads);
			// The downscale form never asks.
			AdaptiveDownscaleStage::Frame d = x.Frame(3840, 2160, 2560, 1440);
			d.adaptiveDownscale = true;
			hooks.clear();
			x.Run(d, hooks);
			Assert::AreEqual(2, asks, L"the downscale form is unchanged: no edge ask");
			Assert::IsFalse(HasLine(x.stage.StatusLines(), "edge/grain"));
		}

		TEST_METHOD(HybridPublishedEdgeClearsItsInterimCause)
		{
			OneAxisStageFixture x;
			static int dummyTexture = 0;
			x.host.acquire = [](VpSkeleton::ProductId, uint64_t, const VpSkeleton::VariantKey&)
			{
				VpSkeleton::AcquireResult r;
				r.status = VpSkeleton::AcquireStatus::Reused;
				r.gpu.resource = &dummyTexture;   // never sampled here (no GPU): only the seam's choice is checked
				return r;
			};
			std::vector<const pl_hook*> hooks;
			AdaptiveDownscaleStage::Frame f = x.Frame(1920, 1080, 2560, 1440);
			f.adaptiveUpscale = true;
			x.Run(f, hooks);
			Assert::IsTrue(x.stage.EdgeTerm().published, L"published: the module reads kEdgeMap");
			const std::vector<std::string> lines = x.stage.StatusLines();
			Assert::IsTrue(HasLine(lines, "edge/grain: private interim (grain:"), L"the grain term is still interim");
			Assert::IsFalse(HasLine(lines, "edge: kEdgeMap"));
			// An Ok result without a texture is not published.
			x.host.acquire = [](VpSkeleton::ProductId, uint64_t, const VpSkeleton::VariantKey&)
			{
				VpSkeleton::AcquireResult r;
				r.status = VpSkeleton::AcquireStatus::Reused;
				return r;
			};
			f.sourceSequence = 4;
			hooks.clear();
			x.Run(f, hooks);
			Assert::IsFalse(x.stage.EdgeTerm().published);
			Assert::IsTrue(HasLine(x.stage.StatusLines(), "edge: kEdgeMap stage variant has no GPU texture"));
			// Both published: no interim line.
			AdaptiveDownscaleStage::TermSource e, g;
			e.published = g.published = true;
			Assert::IsTrue(AdaptiveDownscaleStage::InterimLine(e, g).empty());
			g.published = false;
			g.cause = "grain: x";
			Assert::AreEqual(std::string("edge/grain: private interim (grain: x)"), AdaptiveDownscaleStage::InterimLine(e, g));
		}

		TEST_METHOD(HybridMotionFallbacksNameTheirCause)
		{
			using AdaptiveDownscaleStage::MotionDiscontinuity;
			Assert::AreEqual("motion vectors off", MotionDiscontinuity(false, false, true, true, 3, 4));
			Assert::AreEqual("NLS active", MotionDiscontinuity(true, true, true, true, 3, 4));
			Assert::AreEqual("resync", MotionDiscontinuity(true, false, true, true, 5, 4));
			Assert::AreEqual("dropped source frames", MotionDiscontinuity(true, false, true, true, 3, 5));
			Assert::AreEqual("no live estimate", MotionDiscontinuity(true, false, false, true, 3, 4));
			Assert::IsNull(MotionDiscontinuity(true, false, true, true, 3, 4), L"the next frame with a live estimate");
			Assert::IsNull(MotionDiscontinuity(true, false, true, false, 0, 9), L"the first frame has no history");

			// The stage: adaptive_motion on, the upscale form; the cause is on the status and on the Fallbacks entry.
			OneAxisStageFixture x;
			x.settings.motionAdaptive = true;
			static int tex = 0;
			bool live = false;
			x.host.motion = [&]()
			{
				AdaptiveDownscaleStage::MotionSource m;
				m.live = live;
				m.field = m.frameTrust = live ? reinterpret_cast<pl_tex>(&tex) : nullptr;
				return m;
			};
			std::vector<const pl_hook*> hooks;
			AdaptiveDownscaleStage::Frame f = x.Frame(1920, 1080, 2560, 1440);
			f.adaptiveUpscale = true;
			x.Run(f, hooks);
			Assert::IsTrue(HasLine(x.stage.StatusLines(), "Adaptive upscale motion: no vectors (alias-safe weight; no live estimate)"));
			const FallbackCatalog::Decision d = x.stage.Fallback();
			Assert::IsTrue(d.engaged && d.reason.find("alias-safe weight (no live estimate)") != std::string::npos);
			live = true;
			f.sourceSequence = 4;
			hooks.clear();
			x.Run(f, hooks);
			Assert::IsTrue(x.stage.UpscaleMotionCause().empty(), L"live and in sequence: the vectors are used");
			Assert::IsFalse(HasLine(x.stage.StatusLines(), "Adaptive upscale motion: no vectors"));
			hooks.clear();
			x.Run(f, hooks);   // a repeat of 4: keeps the verdict
			Assert::IsTrue(x.stage.UpscaleMotionCause().empty(), L"a repeated frame keeps its verdict");
			f.sourceSequence = 6;   // 5 dropped
			hooks.clear();
			x.Run(f, hooks);
			Assert::AreEqual(std::string("dropped source frames"), x.stage.UpscaleMotionCause());
			Assert::IsTrue(HasLine(x.stage.StatusLines(), "(alias-safe weight; dropped source frames)"));
			hooks.clear();
			x.Run(f, hooks);   // a repeat of 6: still the drop's verdict (no flicker)
			Assert::AreEqual(std::string("dropped source frames"), x.stage.UpscaleMotionCause());
			f.sourceSequence = 5;   // back in time: a resync
			hooks.clear();
			x.Run(f, hooks);
			Assert::AreEqual(std::string("resync"), x.stage.UpscaleMotionCause());
			f.sourceSequence = 6;
			hooks.clear();
			x.Run(f, hooks);
			Assert::IsTrue(x.stage.UpscaleMotionCause().empty());
			f.vectorsRun = false;
			f.sourceSequence = 7;
			hooks.clear();
			x.Run(f, hooks);
			Assert::AreEqual(std::string("motion vectors off"), x.stage.UpscaleMotionCause());
			// One axis: faithful only already; no motion line.
			AdaptiveDownscaleStage::Frame o = x.Frame(1920, 1440, 2560, 1440);
			o.adaptiveUpscale = true;
			o.vectorsRun = false;
			hooks.clear();
			x.Run(o, hooks);
			Assert::IsFalse(HasLine(x.stage.StatusLines(), "Adaptive upscale motion: no vectors"));
			// The downscale form: unchanged (no upscale motion cause, today's lines).
			AdaptiveDownscaleStage::Frame dn = x.Frame(3840, 2160, 2560, 1440);
			dn.adaptiveDownscale = true;
			hooks.clear();
			x.Run(dn, hooks);
			Assert::IsTrue(x.stage.UpscaleMotionCause().empty());
			Assert::IsTrue(HasLine(x.stage.StatusLines(), "Motion-adapted blend: idle"));
		}

		TEST_METHOD(HybridFallbackCatalogEntries)
		{
			using FallbackCatalog::OutputScalingUpscale;
			Assert::IsFalse(OutputScalingUpscale(false, true, "x", true, "y").engaged, L"not requested: released");
			FallbackCatalog::Decision d = OutputScalingUpscale(true, true, "shader compile", true, "resync");
			Assert::IsTrue(d.engaged && d.id == "output scaling");
			Assert::AreEqual(std::string("libplacebo main scaler alone (adaptive upscale failed on the GPU: alias-safe ScalerPass: shader compile)"), d.reason);
			d = OutputScalingUpscale(true, true, "", false, "");
			Assert::AreEqual(std::string("libplacebo main scaler alone (adaptive upscale failed on the GPU)"), d.reason);
			d = OutputScalingUpscale(true, false, "", true, "resync");
			Assert::AreEqual(std::string("adaptive upscale: no vectors, alias-safe weight (resync)"), d.reason);
			d = OutputScalingUpscale(true, false, "", true, "");
			Assert::AreEqual(std::string("adaptive upscale: no vectors, alias-safe weight (no motion map)"), d.reason);
			Assert::IsFalse(OutputScalingUpscale(true, false, "", false, "").engaged);
		}

		// CF-2 FIX1 (gate M1): the module's own motion floor (no usable factor map while the stage found the vectors usable)
		// engages the "output scaling" entry with its cause. Before FIX1 the entry read only the stage's cause, so this case
		// was Released (a floor shown on Ctrl+I only). N2: every precedence pair at once. S2: the edge/grain interim is last.
		TEST_METHOD(HybridModuleMotionFloorReachesTheCatalog)
		{
			using AdaptiveDownscale::MotionFloorCause;
			using AdaptiveDownscale::MotionState;
			Assert::AreEqual("vectors invalid", MotionFloorCause(true, MotionState::Used, true, true, true));
			Assert::AreEqual("motion map not built on this frame", MotionFloorCause(false, MotionState::Off, true, true, true));
			Assert::AreEqual("no motion map for this source frame", MotionFloorCause(false, MotionState::NoVectors, true, true, true));
			Assert::AreEqual("motion map not ready", MotionFloorCause(false, MotionState::Used, false, true, true));
			Assert::AreEqual("motion map from another source frame", MotionFloorCause(false, MotionState::Used, true, false, true));
			Assert::AreEqual("motion map texture missing", MotionFloorCause(false, MotionState::Used, true, true, false));
			Assert::IsNull(MotionFloorCause(false, MotionState::Used, true, true, true), L"the map is used: no floor");

			using AdaptiveDownscaleStage::UpscaleFallbackInputs;
			using AdaptiveDownscaleStage::UpscaleScalingFallback;
			UpscaleFallbackInputs in;
			in.requested = true;
			in.motionAdaptiveOn = true;
			in.moduleFloorUsed = true;
			in.moduleFloorCause = "motion map not ready";
			FallbackCatalog::Decision d = UpscaleScalingFallback(in);
			Assert::IsTrue(d.engaged && d.id == "output scaling", L"M1: the stage cause is empty, the module floor still engages");
			Assert::AreEqual(std::string("adaptive upscale: no vectors, alias-safe weight (motion map not ready)"), d.reason);
			in.stageMotionCause = "resync";
			Assert::AreEqual(std::string("adaptive upscale: no vectors, alias-safe weight (resync)"), UpscaleScalingFallback(in).reason,
				L"the stage's verdict names the cause when both are set");

			// N2: failure + floor + no cut + interim: the failure first; then the floor; then the cut; then the interim.
			in.failureHeld = true;
			in.scalerError = "shader compile";
			in.cutAvailable = false;
			in.cutCause = "no scene group";
			in.interim = "edge/grain: private interim (grain: x)";
			Assert::AreEqual(std::string("libplacebo main scaler alone (adaptive upscale failed on the GPU: alias-safe ScalerPass: shader compile)"),
				UpscaleScalingFallback(in).reason);
			in.failureHeld = false;
			Assert::AreEqual(std::string("adaptive upscale: no vectors, alias-safe weight (resync)"), UpscaleScalingFallback(in).reason);
			in.stageMotionCause.clear();
			in.moduleFloorUsed = false;
			Assert::AreEqual(std::string("adaptive downscale: no cut verdict this frame (no scene group)"), UpscaleScalingFallback(in).reason);
			in.cutAvailable = true;
			d = UpscaleScalingFallback(in);
			Assert::IsTrue(d.engaged && d.id == "output scaling");
			Assert::AreEqual(std::string("adaptive upscale: edge/grain: private interim (grain: x)"), d.reason, L"S2: the interim on Config status");
			in.interim.clear();
			Assert::IsFalse(UpscaleScalingFallback(in).engaged, L"nothing stands in: released");
			// A stage cause on one axis is faithful only already (no floor); the module floor flag is never set there.
			in.stageMotionCause = "resync";
			in.oneAxis = true;
			Assert::IsFalse(UpscaleScalingFallback(in).engaged);
			in.oneAxis = false;
			in.motionAdaptiveOn = false;
			Assert::IsFalse(UpscaleScalingFallback(in).engaged, L"adaptive_motion off: the stage cause is no floor");
			in.requested = false;
			in.moduleFloorUsed = true;
			in.failureHeld = true;
			in.interim = "x";
			Assert::IsFalse(UpscaleScalingFallback(in).engaged, L"not requested: released");

			// The stage, CPU fixture: the upscale form's entry carries the interim (the grain seam is private at this tip).
			OneAxisStageFixture x;
			std::vector<const pl_hook*> hooks;
			AdaptiveDownscaleStage::Frame f = x.Frame(1920, 1080, 2560, 1440);
			f.adaptiveUpscale = true;
			x.Run(f, hooks);
			d = x.stage.Fallback();
			Assert::IsTrue(d.engaged && d.reason.find("private interim") != std::string::npos);
		}
	};

	TEST_CLASS(AdaptiveScalingMotionHookTests)
	{
		struct Gpu
		{
			TargetLutGpuFixture fixture;
			pl_gpu gpu = nullptr;
			pl_fmt srcFmt = nullptr, dstFmt = nullptr, f4 = nullptr, f1 = nullptr;
			bool Create()
			{
				if (!fixture.Create()) return false;
				gpu = fixture.Gpu();
				srcFmt = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 16, 16, static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_LINEAR));
				dstFmt = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 8, 8, static_cast<enum pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE));
				f4 = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32, PL_FMT_CAP_SAMPLEABLE);
				f1 = pl_find_fmt(gpu, PL_FMT_FLOAT, 1, 32, 32, PL_FMT_CAP_SAMPLEABLE);
				return srcFmt && dstFmt && f4 && f1;
			}
			pl_tex Picture(int w, int h)
			{
				std::vector<uint16_t> px(static_cast<size_t>(w) * h * 4);
				for (int y = 0; y < h; ++y)
					for (int x = 0; x < w; ++x)
					{
						const double v = 0.45 + 0.2 * std::sin(x * 0.37) * std::cos(y * 0.29) + 0.1 * std::sin((x + 2 * y) * 0.9);
						const uint16_t c = static_cast<uint16_t>(std::lround(v * 65535.0));
						uint16_t* p = &px[(static_cast<size_t>(y) * w + x) * 4];
						p[0] = p[1] = p[2] = c;
						p[3] = 65535;
					}
				pl_tex_params tp{};
				tp.w = w; tp.h = h; tp.format = srcFmt; tp.sampleable = true; tp.initial_data = px.data();
				return pl_tex_create(gpu, &tp);
			}
			// A field of bw x bh blocks: (vx, vy, trust, error) per block from `cell`.
			pl_tex Field(int bw, int bh, const std::function<void(int, int, float*)>& cell)
			{
				std::vector<float> v(static_cast<size_t>(bw) * bh * 4);
				for (int y = 0; y < bh; ++y)
					for (int x = 0; x < bw; ++x)
						cell(x, y, &v[(static_cast<size_t>(y) * bw + x) * 4]);
				pl_tex_params tp{};
				tp.w = bw; tp.h = bh; tp.format = f4; tp.sampleable = true; tp.initial_data = v.data();
				return pl_tex_create(gpu, &tp);
			}
			pl_tex Trust(float share)
			{
				pl_tex_params tp{};
				tp.w = 1; tp.h = 1; tp.format = f1; tp.sampleable = true; tp.initial_data = &share;
				return pl_tex_create(gpu, &tp);
			}
			std::vector<RgbaPixel> Render(AdaptiveDownscale::Module& module, pl_tex src, int ow, int oh, bool upscale)
			{
				pl_tex_params tp{};
				tp.w = ow; tp.h = oh; tp.format = dstFmt; tp.renderable = true; tp.host_readable = true;
				pl_tex target = pl_tex_create(gpu, &tp);
				pl_frame image = TargetLutGpuFixture::MakeRgbFrame(src);
				pl_frame out = TargetLutGpuFixture::MakeRgbFrame(target);
				pl_render_params params = pl_render_default_params;
				params.dither_params = nullptr;
				params.error_diffusion = nullptr;
				params.downscaler = &pl_filter_bilinear;
				params.upscaler = &pl_filter_bilinear;
				(void)upscale;
				const pl_hook* hooks[2] = { module.Measure(), module.Blend() };
				params.hooks = hooks;
				params.num_hooks = 2;
				Assert::IsTrue(pl_render_image(fixture.Renderer(), &image, &out, &params));
				pl_gpu_finish(gpu);
				std::vector<RgbaPixel> px(static_cast<size_t>(ow) * oh);
				pl_tex_transfer_params dl{};
				dl.tex = target;
				dl.ptr = px.data();
				Assert::IsTrue(pl_tex_download(gpu, &dl));
				pl_tex_destroy(gpu, &target);
				return px;
			}
		};

		static int MaxDiff(const std::vector<RgbaPixel>& a, const std::vector<RgbaPixel>& b)
		{
			int m = 0;
			for (size_t i = 0; i < a.size(); ++i)
				for (int c = 0; c < 3; ++c)
				{
					const int d = std::abs(int((&a[i].r)[c]) - int((&b[i].r)[c]));
					m = d > m ? d : m;
				}
			return m;
		}

	public:
		TEST_METHOD(MotionMapRampsBySpeedSmoothsAndFloorsUntrusted)
		{
			Gpu g;
			Assert::IsTrue(g.Create(), L"Could not create the libplacebo WARP test device");
			std::string log;
			g.fixture.CaptureLog(&log);
			const int w = 128, h = 64, bw = w / 16, bh = h / 16;
			AdaptiveDownscale::Module module;
			module.hostReadableMap = true;
			AdaptiveDownscale::Settings st;
			st.motionAdaptive = true;
			st.motionFloor = 0.25f;
			st.motionSmoothMs = 250.0f;
			st.ssimResidualStabilise = false;
			st.grainMap = false;
			pl_tex src = g.Picture(w, h);
			pl_tex trust = g.Trust(1.0f);
			// Frame 1: left half steady, right half fast (16 px/frame = 6 heights/s), row 0 untrusted.
			pl_tex field1 = g.Field(bw, bh, [&](int x, int y, float* c) {
				c[0] = x >= bw / 2 ? 16.0f : 0.0f; c[1] = 0.0f; c[2] = y == 0 ? 0.0f : 1.0f; c[3] = 0.0f; });
			// Frame 2: everything steady and trusted.
			pl_tex field2 = g.Field(bw, bh, [&](int, int, float* c) { c[0] = 0.0f; c[1] = 0.0f; c[2] = 1.0f; c[3] = 0.0f; });
			auto frame = [&](pl_tex field, uint64_t seq)
			{
				module.SetFrame(st, seq, 1.0f / 24.0f);
				AdaptiveDownscale::Motion m;
				m.field = field;
				m.frameTrust = trust;
				m.minTrustedShare = 0.25f;
				m.sourceCrop = { 0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h) };
				module.SetMotion(m);
				g.Render(module, src, w / 2, h / 2, false);
			};
			auto read = [&]()
			{
				pl_tex map = module.MotionMap();
				Assert::IsNotNull(map, L"no motion map");
				std::vector<float> t(static_cast<size_t>(map->params.w) * map->params.h * 4);
				pl_tex_transfer_params d{};
				d.tex = map;
				d.ptr = t.data();
				Assert::IsTrue(pl_tex_download(g.gpu, &d));
				return t;
			};
			frame(field1, 1);
			Assert::IsTrue(module.LastMotionState() == AdaptiveDownscale::MotionState::Used, L"the map was not used");
			std::vector<float> t = read();
			for (int y = 0; y < bh; ++y)
				for (int x = 0; x < bw; ++x)
				{
					const float v = t[(static_cast<size_t>(y) * bw + x) * 4];
					const float expected = y == 0 || x >= bw / 2 ? 0.25f : 1.0f;
					Assert::AreEqual(expected, v, 1e-4f, L"frame 1: steady 1, fast / untrusted the floor");
				}
			frame(field1, 1);   // a repeat of the same source frame: the map stands
			frame(field2, 2);
			t = read();
			const float alpha = OnePole::AlphaForMs((1.0f / 24.0f) * 1000.0f, 250.0f, OnePole::DtRule::AsGiven);
			for (int y = 0; y < bh; ++y)
				for (int x = 0; x < bw; ++x)
				{
					const float v = t[(static_cast<size_t>(y) * bw + x) * 4];
					const float expected = y == 0 || x >= bw / 2 ? AdaptiveDownscale::MotionStep(0.25f, 1.0f, alpha, false) : 1.0f;
					Assert::AreEqual(expected, v, 1e-4f, L"frame 2: the one-pole (CPU mirror)");
				}
			// No field: factor 1, state visible.
			module.SetFrame(st, 3, 1.0f / 24.0f);
			module.ClearMotion();
			g.Render(module, src, w / 2, h / 2, false);
			Assert::IsTrue(module.LastMotionState() == AdaptiveDownscale::MotionState::NoVectors);
			// Off: nothing.
			st.motionAdaptive = false;
			module.SetFrame(st, 4, 1.0f / 24.0f);
			g.Render(module, src, w / 2, h / 2, false);
			Assert::IsTrue(module.LastMotionState() == AdaptiveDownscale::MotionState::Off);
			Assert::IsTrue(module.Failures() == 0, L"GPU failures");
			for (pl_tex* tex : { &src, &trust, &field1, &field2 }) pl_tex_destroy(g.gpu, tex);
			g.fixture.CaptureLog(nullptr);
			module.Release();
			AssertCleanLibplaceboLog(log);
		}

		TEST_METHOD(MotionAdaptedBlendIsFaithfulOnFastAndUnchangedOnSteady)
		{
			Gpu g;
			Assert::IsTrue(g.Create(), L"Could not create the libplacebo WARP test device");
			std::string log;
			g.fixture.CaptureLog(&log);
			const int w = 128, h = 64, bw = w / 16, bh = h / 16, ow = w * 2 / 3, oh = h * 2 / 3;
			pl_tex src = g.Picture(w, h);
			pl_tex trust = g.Trust(1.0f);
			pl_tex fast = g.Field(bw, bh, [](int, int, float* c) { c[0] = 16.0f; c[1] = 8.0f; c[2] = 1.0f; c[3] = 0.0f; });
			pl_tex steady = g.Field(bw, bh, [](int, int, float* c) { c[0] = 0.0f; c[1] = 0.0f; c[2] = 1.0f; c[3] = 0.0f; });
			AdaptiveDownscale::Settings base;
			base.ssimResidualStabilise = false;   // the comparison is the weight alone
			base.grainMap = false;
			base.motionFloor = 0.0f;
			auto run = [&](AdaptiveDownscale::Settings st, pl_tex field)
			{
				AdaptiveDownscale::Module module;
				module.SetFrame(st, 1, 1.0f / 24.0f);
				if (field)
				{
					AdaptiveDownscale::Motion m;
					m.field = field;
					m.frameTrust = trust;
					m.sourceCrop = { 0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h) };
					module.SetMotion(m);
				}
				std::vector<RgbaPixel> px = g.Render(module, src, ow, oh, false);
				Assert::IsTrue(module.Active() && module.Failures() == 0, L"the blend did not run");
				module.Release();
				return px;
			};
			AdaptiveDownscale::Settings faithful = base;
			faithful.wBase = 0.0f;
			const std::vector<RgbaPixel> plain = run(base, steady);
			const std::vector<RgbaPixel> faithfulOnly = run(faithful, steady);
			Assert::IsTrue(MaxDiff(plain, faithfulOnly) > 0, L"the branches must differ for the test to mean anything");
			AdaptiveDownscale::Settings on = base;
			on.motionAdaptive = true;
			Assert::AreEqual(0, MaxDiff(run(on, fast), faithfulOnly), L"fast motion, floor 0: the faithful branch");
			Assert::IsTrue(MaxDiff(run(on, steady), plain) <= 1, L"steady, trusted: today's blend");
			Assert::AreEqual(0, MaxDiff(run(on, nullptr), plain), L"no vectors: today's blend (factor 1)");
			for (pl_tex* tex : { &src, &trust, &fast, &steady }) pl_tex_destroy(g.gpu, tex);
			g.fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(log);
		}

		TEST_METHOD(OneAxisUpscaleIsTheFaithfulBranchAndWidthOnlyOpensTheGate)
		{
			Gpu g;
			Assert::IsTrue(g.Create(), L"Could not create the libplacebo WARP test device");
			std::string log;
			g.fixture.CaptureLog(&log);
			const int w = 128, h = 64;
			pl_tex src = g.Picture(w, h);
			AdaptiveDownscale::Settings st;
			st.cnnResidualStabilise = false;
			st.grainMap = false;
			auto run = [&](int ow, int oh, float wBase, float nativeW, bool oneAxis, bool& active, int& ratio)
			{
				AdaptiveDownscale::Module module;
				module.SetFrame(st, 1, 1.0f / 24.0f);
				AdaptiveDownscale::Upscale up;
				up.on = true;
				up.wBase = wBase;
				up.nativeHeight = static_cast<float>(h);
				up.nativeWidth = nativeW;
				up.oneAxis = oneAxis;
				module.SetUpscale(up);
				std::vector<RgbaPixel> px = g.Render(module, src, ow, oh, true);
				active = module.Active();
				ratio = module.LastUpscaleRatio();
				Assert::IsTrue(module.Failures() == 0, L"GPU failures");
				module.Release();
				return px;
			};
			bool active = false;
			int ratio = -1;
			// V only (128x64 -> 128x96).
			const auto oneAxis = run(w, h * 3 / 2, 1.0f, static_cast<float>(w), true, active, ratio);
			Assert::IsTrue(active && ratio == 1, L"one-axis: the module ran on the source picture");
			const auto faithfulOnly = run(w, h * 3 / 2, 0.0f, static_cast<float>(w), false, active, ratio);
			const auto cnnBlend = run(w, h * 3 / 2, 1.0f, static_cast<float>(w), false, active, ratio);
			Assert::IsTrue(MaxDiff(cnnBlend, faithfulOnly) > 0, L"the branches must differ for the test to mean anything");
			Assert::AreEqual(0, MaxDiff(oneAxis, faithfulOnly), L"one-axis = the faithful branch whatever w_base");
			// H only (128x64 -> 192x64): the height-only gate left it alone; with the width it runs.
			run(w * 3 / 2, h, 1.0f, 0.0f, false, active, ratio);
			Assert::IsFalse(active, L"no width given: today's height-only gate");
			run(w * 3 / 2, h, 1.0f, static_cast<float>(w), true, active, ratio);
			Assert::IsTrue(active && ratio == 1, L"width given: either axis opens the gate");
			pl_tex_destroy(g.gpu, &src);
			g.fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(log);
		}

		// CF-2 G9 (gate CF2-R2 SHOULD 2): the stage reads the module's own motion floor through Fallback() on a real Measure.
		// The host's vectors are live and consecutive, so the stage's verdict is usable (no stage cause); the map's input is
		// then withheld from the module (ClearMotion after Register), so the module's measure has no motion map and the blend
		// takes the motion floor (0: the alias-safe branch). Expected: the "output scaling" entry engaged with the module's
		// cause, the status line with that cause, and the floored picture = the alias-safe branch. Next frame, vectors kept:
		// the map is used, the floor and its line are gone. Before FIX1 (Fallback read the stage cause only) the entry would
		// name the cut verdict or the interim, not the floor.
		TEST_METHOD(StageFallbackCarriesTheModuleMotionFloorOnARealMeasure)
		{
			Gpu g;
			Assert::IsTrue(g.Create(), L"Could not create the libplacebo WARP test device");
			std::string log;
			g.fixture.CaptureLog(&log);
			auto wide = [](const std::string& s) { return std::wstring(s.begin(), s.end()); };
			const int w = 128, h = 64, bw = w / 16, bh = h / 16, ow = w * 3 / 2, oh = h * 3 / 2;
			pl_tex src = g.Picture(w, h);
			pl_tex trust = g.Trust(1.0f);
			pl_tex steady = g.Field(bw, bh, [](int, int, float* c) { c[0] = 0.0f; c[1] = 0.0f; c[2] = 1.0f; c[3] = 0.0f; });
			{
				AdaptiveDownscale::Module module;
				AdaptiveDownscaleStage::Stage stage{ module };
				AdaptiveDownscale::Settings settings;
				settings.cnnResidualStabilise = false;   // the comparison is the weight alone
				settings.ssimResidualStabilise = false;
				settings.grainMap = false;
				settings.motionAdaptive = true;
				settings.motionFloor = 0.0f;
				double weight = 1.0;
				bool grainWanted = false, noSsim = false;
				AdaptiveDownscaleStage::Host host;
				host.fallbackFramePeriod = []() { return 1.0f / 24.0f; };
				host.bitrate = [](double& q) { q = 1.0; return false; };
				host.nlsMapping = []() { return NlsMaskMapping::Mapping(); };
				host.ssimInChain = []() { return false; };
				host.publishStatus = []() {};
				host.motion = [&]()
				{
					AdaptiveDownscaleStage::MotionSource m;
					m.live = true;
					m.field = steady;
					m.frameTrust = trust;
					return m;
				};
				uint64_t sequence = 0;
				// One source frame through the stage: Register, optionally withhold the map's input, render its hooks.
				auto frame = [&](double wBase, bool withhold)
				{
					AdaptiveDownscaleStage::Frame f;
					f.adaptive = &settings;
					f.adaptiveUpscale = true;
					f.upscaleWBase = wBase;
					f.upscaleSuperresScale = false;
					f.lowBitrateOff = true;
					f.sourceSequence = ++sequence;
					f.nominalSourceRateHz = 24.0;
					f.sourceCrop = pl_rect2df{ 0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h) };
					f.targetCrop = pl_rect2df{ 0.0f, 0.0f, static_cast<float>(ow), static_cast<float>(oh) };
					std::vector<const pl_hook*> hooks;
					stage.Register(f, host, { weight, grainWanted, noSsim }, hooks);
					Assert::IsTrue(stage.UpscaleThisFrame(), L"the upscale form registered");
					Assert::IsTrue(hooks.size() == 2 && hooks[0] == module.Measure() && hooks[1] == module.Blend(),
						L"the stage placed the measure and the blend (Render binds exactly these)");
					if (withhold)
						module.ClearMotion();
					std::vector<RgbaPixel> px = g.Render(module, src, ow, oh, true);
					Assert::IsTrue(module.Active() && module.LastUpscaleRatio() == 1 && module.Failures() == 0, L"the blend did not run");
					return px;
				};
				const std::string cause = "no motion map for this source frame";
				const std::string floorReason = "adaptive upscale: no vectors, alias-safe weight (" + cause + ")";
				const std::string floorLine = "Adaptive upscale motion: no vectors (alias-safe weight; " + cause + ")";

				// Frame 1: the stage's verdict usable, the module without its map: the module's floor.
				const std::vector<RgbaPixel> floored = frame(1.0, true);
				Assert::IsTrue(module.MotionFloorUsed(), L"the module took the motion floor");
				Assert::AreEqual(cause, module.MotionFloorCauseText());
				FallbackCatalog::Decision d = stage.Fallback();
				Assert::IsTrue(d.engaged && d.id == "output scaling", wide("not engaged: " + d.reason).c_str());
				Assert::AreEqual(floorReason, d.reason, L"the module's floor and its cause reach the catalog through Fallback()");
				std::vector<std::string> lines = stage.StatusLines();
				Assert::IsTrue(HasLine(lines, floorLine.c_str()), L"the status line names the module's floor cause");

				// Frame 2: vectors kept: the map is built and used; no floor, no floor line, the entry no longer names it.
				const std::vector<RgbaPixel> mapped = frame(1.0, false);
				Assert::IsFalse(module.MotionFloorUsed(), L"the map is used: no floor");
				d = stage.Fallback();
				Assert::IsTrue(d.reason.find("no vectors") == std::string::npos, wide("still the floor: " + d.reason).c_str());
				lines = stage.StatusLines();
				Assert::IsFalse(HasLine(lines, "Adaptive upscale motion: no vectors"), L"no floor line with the map in use");

				// Frame 3: w_base 0, the alias-safe branch. The floored frame (floor 0) is that picture; the mapped one is not.
				const std::vector<RgbaPixel> aliasSafe = frame(0.0, false);
				Assert::IsTrue(MaxDiff(mapped, aliasSafe) > 0, L"the branches must differ for the test to mean anything");
				Assert::AreEqual(0, MaxDiff(floored, aliasSafe), L"floor 0: the alias-safe branch");
				module.Release();
			}
			for (pl_tex* tex : { &src, &trust, &steady }) pl_tex_destroy(g.gpu, tex);
			g.fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(log);
		}
	};

	// FIX-X5-CUT-P4 / C5 (CPU; audit x5dup MUST 1, WAIVER-X5DUP-SCENE-READ): the grain term's cut event is the skeleton's published
	// scene product (FrameAnalysis::Scene, U7 v2/v3), read, never classified here. Snap on the DELIVERED cut only (Scene::cut, normally
	// one frame late), never on a provisional verdict, never on a fade; a missing group of the current frame is an Unavailable verdict
	// with its cause (never a made-up cut, never a private re-classification).
	TEST_CLASS(AdaptiveScalingCutP4Tests)
	{
	public:
		static VpLaneProducts::SourceStatsRecord FlatRecord(uint32_t mean, uint32_t spreadLow, uint32_t spreadHigh, uint32_t bin)
		{
			VpLaneProducts::SourceStatsRecord r;
			r.pixelCount = 1000;
			r.mean = mean;
			r.p01 = spreadLow;
			r.p99 = spreadHigh;
			r.hist[bin] = 1000;
			return r;
		}

		// A scene reader over a map of groups (the published product of each frame), counting the frames asked.
		struct GroupMap
		{
			std::map<uint64_t, FrameAnalysis::Scene> groups;
			int asked = 0;
			bool operator()(uint64_t seq, FrameAnalysis::Scene& scene)
			{
				++asked;
				const auto it = groups.find(seq);
				if (it == groups.end())
					return false;
				scene = it->second;
				return true;
			}
		};

		static FrameAnalysis::Scene Group(bool cut, bool cutLate = false)
		{
			FrameAnalysis::Scene s;
			s.origin = FrameAnalysis::SceneOrigin::Vp;
			s.cut = cut;
			s.cutLate = cutLate;
			s.confirmed = true;
			s.fadeKeep = 1.0f;
			return s;
		}

		TEST_METHOD(AdaptiveScalingCutP4FirstFrameIsACutWithoutPreviousRecord)
		{
			GroupMap read;
			const AdaptiveDownscale::CutVerdict v = AdaptiveDownscale::VerdictFromScene(0, 5, std::ref(read), 8, "missing");
			Assert::IsTrue(v.state == AdaptiveDownscale::CutVerdict::State::Known && v.cut && v.first, L"first frame: a cut by definition");
			Assert::AreEqual(0, read.asked, L"no scene group is needed for the definitional first-frame cut");
		}

		TEST_METHOD(AdaptiveScalingCutP4SnapsOnTheDeliveredCutOnly)
		{
			GroupMap read;
			read.groups[11] = Group(false);
			const AdaptiveDownscale::CutVerdict same = AdaptiveDownscale::VerdictFromScene(10, 11, std::ref(read), 8, "missing");
			Assert::IsTrue(same.state == AdaptiveDownscale::CutVerdict::State::Known && !same.cut, L"no delivered cut: no snap");
			read.groups[11] = Group(true, true);
			const AdaptiveDownscale::CutVerdict cut = AdaptiveDownscale::VerdictFromScene(10, 11, std::ref(read), 8, "missing");
			Assert::IsTrue(cut.state == AdaptiveDownscale::CutVerdict::State::Known && cut.cut && cut.cutLate && cut.cutGroup == 11,
				L"the delivered cut (late, with the frame after the cut): a snap");
			// Precision first (owner 08:35): the frame's own verdict, provisional or confirmed after its seal, never snaps by itself.
			FrameAnalysis::Scene provisional = Group(false);
			provisional.cutHere = true;
			provisional.confirmed = false;
			read.groups[11] = provisional;
			const AdaptiveDownscale::CutVerdict prov = AdaptiveDownscale::VerdictFromScene(10, 11, std::ref(read), 8, "missing");
			Assert::IsTrue(prov.state == AdaptiveDownscale::CutVerdict::State::Known && !prov.cut, L"a provisional cutHere: no snap");
			FrameAnalysis::Scene here = Group(false);
			here.cutHere = true;
			read.groups[11] = here;
			const AdaptiveDownscale::CutVerdict own = AdaptiveDownscale::VerdictFromScene(10, 11, std::ref(read), 8, "missing");
			Assert::IsTrue(own.state == AdaptiveDownscale::CutVerdict::State::Known && !own.cut,
				L"a confirmed cutHere without its delivery: no snap (it is delivered once, with the next group)");
		}

		TEST_METHOD(AdaptiveScalingCutP4NoSnapOnAFade)
		{
			GroupMap read;
			FrameAnalysis::Scene fade = Group(false);
			fade.fade = true;
			fade.fadeKeep = 0.4f;
			read.groups[11] = fade;
			const AdaptiveDownscale::CutVerdict v = AdaptiveDownscale::VerdictFromScene(10, 11, std::ref(read), 8, "missing");
			Assert::IsTrue(v.state == AdaptiveDownscale::CutVerdict::State::Known && !v.cut && v.fade,
				L"a fade with its keep: no snap (owner 09:20: partial reset, never a restart)");
			fade.fadeKeep = -1.0f;
			read.groups[11] = fade;
			const AdaptiveDownscale::CutVerdict noKeep = AdaptiveDownscale::VerdictFromScene(10, 11, std::ref(read), 8, "missing");
			Assert::IsTrue(noKeep.state == AdaptiveDownscale::CutVerdict::State::Known && !noKeep.cut && noKeep.fade,
				L"a fade without its keep: still no snap (x5dup gives no snap on any fade)");
		}

		TEST_METHOD(AdaptiveScalingCutP4UnavailableRecordIsVisibleNotACut)
		{
			GroupMap read;
			const AdaptiveDownscale::CutVerdict none = AdaptiveDownscale::VerdictFromScene(10, 11, std::ref(read), 8,
				"scene group not sealed for source frame 11");
			Assert::IsTrue(none.state == AdaptiveDownscale::CutVerdict::State::Unavailable && !none.cut, L"unavailable, no cut");
			Assert::IsTrue(none.cause.find("not sealed") != std::string::npos, L"the cause is carried");
			// The previous frame's group is not needed: the current frame's group alone decides (a missing older group is no fault).
			read.groups[11] = Group(false);
			const AdaptiveDownscale::CutVerdict noPrev = AdaptiveDownscale::VerdictFromScene(10, 11, std::ref(read), 8, "missing");
			Assert::IsTrue(noPrev.state == AdaptiveDownscale::CutVerdict::State::Known && !noPrev.cut,
				L"previous group missing: the current group decides");
			// C5 (WAIVER-C5-SCENEOF-X5): the production cause is the skeleton's (its SceneOf did not answer the frame), carried with the
			// frame. The store being off is no longer a cause: SceneOf answers live (AdaptiveScalingCutP4StoreOffACutStillSnapsThroughTheSkeleton).
			const std::string why = AdaptiveDownscale::SceneProductMissingWhy("the skeleton could not classify the frame", 11);
			Assert::IsTrue(why.find("source frame 11") != std::string::npos && why.find("could not classify") != std::string::npos,
				L"the skeleton's cause, with the frame");
			Assert::IsTrue(AdaptiveDownscale::SceneProductMissingWhy(nullptr, 11).find("no cause given") != std::string::npos);
			// The current frame not answered: Unavailable with exactly that cause, no cut, and the skipped frames are not scanned.
			GroupMap skipped;
			skipped.groups[10] = Group(true, true);
			const AdaptiveDownscale::CutVerdict notAnswered = AdaptiveDownscale::VerdictFromSceneProduct(9, 11,
				[&why](uint64_t, FrameAnalysis::Scene&, std::string& w) { w = why; return false; }, std::ref(skipped), 8);
			Assert::IsTrue(notAnswered.state == AdaptiveDownscale::CutVerdict::State::Unavailable && !notAnswered.cut
				&& notAnswered.cause == why, L"not answered: visible Unavailable, the skeleton's cause");
			Assert::AreEqual(0, skipped.asked, L"no skipped-frame scan without the current frame's answer");
			// Without a memo (no skeleton, the Original pathway) the production source answers Unavailable with its cause.
			const AdaptiveDownscale::CutVerdict noMemo = AdaptiveDownscale::SceneCutSource()(10, 11);
			Assert::IsTrue(noMemo.state == AdaptiveDownscale::CutVerdict::State::Unavailable && !noMemo.cause.empty() && !noMemo.cut,
				L"no memo on this thread: visible Unavailable");
		}

		// Frames the module did not advance through: a cut delivered with their group is not lost (within the store's ring); a group
		// that is not there is not a fault; reach 0 (or a discontinuity: the module then asks with previous = sequence - 1) reads none.
		TEST_METHOD(AdaptiveScalingCutP4ReadsSkippedFramesInTheRing)
		{
			GroupMap read;
			read.groups[11] = Group(false);
			read.groups[12] = Group(true, true);
			read.groups[13] = Group(false);
			const AdaptiveDownscale::CutVerdict v = AdaptiveDownscale::VerdictFromScene(10, 13, std::ref(read), 8, "missing");
			Assert::IsTrue(v.state == AdaptiveDownscale::CutVerdict::State::Known && v.cut && v.cutLate && v.cutGroup == 12,
				L"the cut delivered with skipped frame 12: a snap");
			const AdaptiveDownscale::CutVerdict near1 = AdaptiveDownscale::VerdictFromScene(10, 13, std::ref(read), 1, "missing");
			Assert::IsTrue(near1.cut && near1.cutGroup == 12, L"reach 1: frame 12 is still in reach");
			const AdaptiveDownscale::CutVerdict none = AdaptiveDownscale::VerdictFromScene(10, 13, std::ref(read), 0, "missing");
			Assert::IsTrue(none.state == AdaptiveDownscale::CutVerdict::State::Known && !none.cut, L"reach 0: only the frame's own group");
			const AdaptiveDownscale::CutVerdict afterForget = AdaptiveDownscale::VerdictFromScene(12, 13, std::ref(read), 8, "missing");
			Assert::IsTrue(afterForget.state == AdaptiveDownscale::CutVerdict::State::Known && !afterForget.cut,
				L"previous = sequence - 1 (after ForgetMap): skipped groups are not scanned");
			read.groups.erase(12);
			const AdaptiveDownscale::CutVerdict gap = AdaptiveDownscale::VerdictFromScene(10, 13, std::ref(read), 8, "missing");
			Assert::IsTrue(gap.state == AdaptiveDownscale::CutVerdict::State::Known && !gap.cut,
				L"a skipped frame without a group (never sealed): no cut, not a fault");
		}

		// The APL (diagnostic mean luma) is the CURRENT frame's P4 record through the memo (Acquire: get-or-compute-and-publish), and a
		// released frame is refused by the memo (FrameReleased, never recomputed): shown as the APL cause, not a cut fault.
		TEST_METHOD(AdaptiveScalingCutP4AplFromTheCurrentP4RecordThroughTheMemo)
		{
			using namespace VpSkeleton;
			SkeletonMemo memo;
			memo.SetKeepFrames(0);
			int fills = 0;
			OwnerFill fill;
			fill.fill = [&fills](const OwnerFillRequest&, SkeletonMemo&, OwnerFillOutput& out)
			{
				++fills;
				const VpLaneProducts::SourceStatsRecord rec = FlatRecord(257 * 100, 1000, 60000, 3);
				out.bytes.assign(VpLaneProducts::kStatsBytes, 0);
				VpLaneProducts::WriteStatsRecord(rec, out.bytes.data(), out.bytes.size());
				return true;
			};
			Assert::IsTrue(memo.RegisterOwnerFill(kP4SourceStats, AcquireForm::Cpu, SkeletonMemo::OwnerUnitOf(kP4SourceStats), fill)
				== SkeletonMemo::Registration::Registered, L"P4 owner fill registered under its catalog unit");
			memo.OnSeal(1);
			AdaptiveDownscale::CutVerdict v;
			AdaptiveDownscale::ReadAplFromMemo(memo, 1, v);
			Assert::IsTrue(std::fabs(v.apl - 100.0f) < 0.01f && v.aplCause.empty(), L"APL = the P4 mean in 8-bit codes");
			Assert::AreEqual(1, fills, L"the current frame's P4 computed once");
			memo.OnSeal(2);
			Assert::IsTrue(memo.Released(1), L"the seal of frame 2 released frame 1");
			AdaptiveDownscale::CutVerdict old;
			AdaptiveDownscale::ReadAplFromMemo(memo, 1, old);
			Assert::IsTrue(old.apl < 0.0f && old.aplCause.find("frame released") != std::string::npos,
				L"a released frame: APL not read, cause shown");
			Assert::IsTrue(old.state == AdaptiveDownscale::CutVerdict::State::Unavailable && !old.cut, L"the APL read never makes a cut");
			Assert::AreEqual(1, fills, L"the released frame is never recomputed");
		}

		// A REAL frame-analysis store, groups written as the skeleton's seal writes them for contiguous frames (SkeletonModule::FillScene):
		// a cut at frame 3 is confirmed by frame 4 and DELIVERED with frame 4's group (cutLate); frame 3's own group says cutHere once
		// confirmed. The module's advance loop (previous = the last advanced frame) snaps exactly on the first frame and on frame 4:
		// delivery offset +1 (the scorer's after-side offset). A frame whose group was not sealed, and a store switched off, are the
		// visible Unavailable with their causes, never a made-up cut.
		TEST_METHOD(AdaptiveScalingCutP4DetectsACutAcrossAMemoSeal)
		{
			FrameAnalysis::Store store(FrameAnalysis::RingDepth(4, 4));
			for (uint64_t s = 1; s <= 6; ++s)
			{
				FrameAnalysis::Identity id;
				id.seq = s;
				Assert::IsTrue(store.Begin(id, FrameAnalysis::kScene) != 0, L"record opened");
				FrameAnalysis::Scene g = Group(s == 4, s == 4);   // frame 4's group delivers the cut of frame 3
				g.cutHere = s == 3;                               // frame 3's own verdict, confirmed when frame 4 arrived
				Assert::IsTrue(store.PublishScene(s, g), L"group published");
			}
			auto reader = [&store](uint64_t seq, FrameAnalysis::Scene& scene) { return store.PeekScene(seq, scene); };
			// C5 (WAIVER-C5-SCENEOF-X5): the current frame as the skeleton's SceneOf answers it: the store's group, else the live
			// classification (`live`: what the skeleton classifies and publishes then); skipped frames stay a Peek of the store.
			std::map<uint64_t, FrameAnalysis::Scene> live;
			auto sceneOf = [&store, &live](uint64_t seq, FrameAnalysis::Scene& scene, std::string& why)
			{
				if (store.PeekScene(seq, scene))
					return true;
				const auto it = live.find(seq);
				if (it != live.end())
				{
					scene = it->second;
					return true;
				}
				why = AdaptiveDownscale::SceneProductMissingWhy("the skeleton could not classify the frame (no sealed pair of P4 records)", seq);
				return false;
			};
			uint64_t advanced = 0;
			std::vector<uint64_t> snaps;
			auto frame = [&](uint64_t seq)
			{
				const AdaptiveDownscale::CutVerdict v = AdaptiveDownscale::VerdictFromSceneProduct(advanced, seq, sceneOf, reader, store.Depth());
				if (v.state == AdaptiveDownscale::CutVerdict::State::Known)
					advanced = seq;   // Module::Measuring: the frame advanced
				if (v.cut)
					snaps.push_back(seq);
				return v;
			};
			const AdaptiveDownscale::CutVerdict f1 = frame(1);
			Assert::IsTrue(f1.state == AdaptiveDownscale::CutVerdict::State::Known && f1.cut && f1.first, L"frame 1: the first frame is a cut");
			Assert::IsFalse(frame(2).cut, L"frame 2: same scene, no snap");
			const AdaptiveDownscale::CutVerdict f3 = frame(3);
			Assert::IsTrue(f3.state == AdaptiveDownscale::CutVerdict::State::Known && !f3.cut,
				L"frame 3 (the cut's first frame): its own cutHere does not snap; the cut is delivered with frame 4");
			const AdaptiveDownscale::CutVerdict f4 = frame(4);
			Assert::IsTrue(f4.state == AdaptiveDownscale::CutVerdict::State::Known && f4.cut && f4.cutLate && f4.cutGroup == 4,
				L"frame 4: the delivered cut of frame 3 snaps (offset +1)");
			Assert::IsFalse(frame(5).cut, L"frame 5: no snap");
			Assert::IsFalse(frame(6).cut, L"frame 6: no snap");
			Assert::AreEqual(size_t(2), snaps.size(), L"exactly two snaps: the first frame and the one delivered cut");
			Assert::IsTrue(snaps[0] == 1 && snaps[1] == 4, L"snaps at frames 1 and 4");
			const AdaptiveDownscale::CutVerdict f7 = frame(7);
			Assert::IsTrue(f7.state == AdaptiveDownscale::CutVerdict::State::Unavailable && !f7.cut
				&& f7.cause.find("not answered for source frame 7") != std::string::npos,
				L"frame 7: neither a group nor a live answer: visible Unavailable");
			// The store OFF (C5, reversed by design: it was a visible Unavailable while the current frame was a Peek): SceneOf answers
			// live, so a delivered cut still snaps and a frame without one does not; a frame the skeleton cannot answer stays Unavailable.
			store.SetEnabled(false);
			live[8] = Group(true, true);
			const AdaptiveDownscale::CutVerdict off = frame(8);
			Assert::IsTrue(off.state == AdaptiveDownscale::CutVerdict::State::Known && off.cut && off.cutLate && off.cutGroup == 8,
				L"the store off: the live answer's delivered cut snaps");
			Assert::AreEqual(size_t(3), snaps.size(), L"store off: one more snap, the delivered cut");
			Assert::IsTrue(snaps[2] == 8, L"at frame 8");
			live[9] = Group(false);
			const AdaptiveDownscale::CutVerdict offNoCut = frame(9);
			Assert::IsTrue(offNoCut.state == AdaptiveDownscale::CutVerdict::State::Known && !offNoCut.cut, L"the store off: no cut, no snap");
			const AdaptiveDownscale::CutVerdict offMissing = frame(10);
			Assert::IsTrue(offMissing.state == AdaptiveDownscale::CutVerdict::State::Unavailable && !offMissing.cut
				&& offMissing.cause.find("could not classify") != std::string::npos, L"the store off, no live answer: visible Unavailable");
		}

		// C5 (WAIVER-C5-SCENEOF-X5; owner 2026-11-14 17:38 "Ask the skeleton for it same as everything else"): the PRODUCTION source
		// (SceneCutSource) asks the skeleton's SceneOf for the current frame of the renderer this thread renders. The Host's store is
		// registered DISABLED (frame_analysis_store=false); a skeleton module in the hub answers SceneOf LIVE, as the real module does
		// with the store off (pinned on the real module: SkeletonModuleTests "2. Store off"). A cut still snaps.
		struct LiveSceneSkeleton : Vpv::Hub::ISkeletonModule, Vpv::Skeleton::IExternalDemand
		{
			const char* Name() const override { return "test skeleton (scene product, live)"; }
			void* Query(const char* iid) override
			{
				return iid && std::strcmp(iid, Vpv::Skeleton::kExternalDemandQueryId) == 0 ? static_cast<Vpv::Skeleton::IExternalDemand*>(this) : nullptr;
			}
			void SetExternalDemand(const void*, const void*, const std::vector<VpSkeleton::ConsumerDecl>&, const std::vector<VpSkeleton::ProductId>&) override {}
			void ClearExternalDemand(const void*, const void*) override {}
			Vpv::Skeleton::FrameMemo MemoFor(const void* host, uint64_t seq) override
			{
				Vpv::Skeleton::FrameMemo fm;
				fm.memo = memo;
				fm.host = host;
				fm.seq = seq;
				return fm;
			}
			Vpv::Skeleton::SceneAnswer SceneOf(const void* host, uint64_t seq) const override
			{
				asks.push_back(std::make_pair(host, seq));
				Vpv::Skeleton::SceneAnswer a;
				const auto it = groups.find(seq);
				if (it == groups.end())
					return a;
				a.known = true;
				a.cut = it->second.cut; a.cutLate = it->second.cutLate; a.cutHere = it->second.cutHere; a.confirmed = it->second.confirmed;
				a.fade = it->second.fade; a.fadeKeep = it->second.fadeKeep;
				a.from = Vpv::Skeleton::SceneAnswer::From::Live;
				return a;
			}
			std::shared_ptr<VpSkeleton::SkeletonMemo> memo = std::make_shared<VpSkeleton::SkeletonMemo>();
			std::map<uint64_t, FrameAnalysis::Scene> groups;
			mutable std::vector<std::pair<const void*, uint64_t>> asks;
		};

		TEST_METHOD(AdaptiveScalingCutP4StoreOffACutStillSnapsThroughTheSkeleton)
		{
			struct Scope
			{
				Scope() { Vpv::Hub::ExtensionHub::Instance().ResetModulesForTesting(); Vpv::Hub::ResetThisThreadFrameForTesting(); }
				~Scope() { Vpv::Hub::ExtensionHub::Instance().ResetModulesForTesting(); Vpv::Hub::ResetThisThreadFrameForTesting(); }
			} scope;
			int renderer = 0;
			FrameAnalysis::Store offStore(FrameAnalysis::RingDepth(4, 4));
			offStore.SetEnabled(false);   // what frame_analysis_store=false leaves
			VpFrameAnalysis::SourceFrameTap::SetStore(&renderer, &offStore);
			struct StoreScope { const void* h; ~StoreScope() { VpFrameAnalysis::SourceFrameTap::SetStore(h, nullptr); } } storeScope{ &renderer };
			LiveSceneSkeleton* sk = new LiveSceneSkeleton();
			sk->groups[4] = Group(true, true);   // the cut of frame 3, delivered with frame 4's answer
			sk->groups[5] = Group(false);
			FrameAnalysis::Scene fade = Group(false);
			fade.fade = true;
			fade.fadeKeep = 0.5f;
			sk->groups[6] = fade;
			Vpv::Hub::ExtensionHub::Instance().AddModule(std::unique_ptr<Vpv::Hub::ISkeletonModule>(sk));
			const AdaptiveDownscale::CutSource source = AdaptiveDownscale::SceneCutSource();
			auto at = [&renderer, &source](uint64_t prev, uint64_t seq)
			{
				RendererExtension::FrameContext c;
				c.host = &renderer;
				c.phase = RendererExtension::FramePhase::Entrance;
				c.seq = int64_t(seq);
				Vpv::Hub::RecordCurrentFrame(c);
				return source(prev, seq);
			};
			const AdaptiveDownscale::CutVerdict cut = at(3, 4);
			Assert::IsTrue(cut.state == AdaptiveDownscale::CutVerdict::State::Known && cut.cut && cut.cutLate && cut.cutGroup == 4,
				Wide("store off: the delivered cut still snaps (" + cut.cause + ")").c_str());
			Assert::AreEqual(size_t(1), sk->asks.size(), L"one SceneOf call for the frame");
			Assert::IsTrue(sk->asks[0].first == &renderer && sk->asks[0].second == 4u, L"asked for this thread's renderer and frame");
			const AdaptiveDownscale::CutVerdict still = at(4, 5);
			Assert::IsTrue(still.state == AdaptiveDownscale::CutVerdict::State::Known && !still.cut, L"store off: no cut, no snap");
			const AdaptiveDownscale::CutVerdict onFade = at(5, 6);
			Assert::IsTrue(onFade.state == AdaptiveDownscale::CutVerdict::State::Known && !onFade.cut && onFade.fade,
				L"store off: a fade, no snap (owner 09:20)");
			const AdaptiveDownscale::CutVerdict missing = at(6, 7);
			Assert::IsTrue(missing.state == AdaptiveDownscale::CutVerdict::State::Unavailable && !missing.cut
				&& missing.cause.find("could not classify") != std::string::npos, Wide(missing.cause).c_str());
			// A skipped frame is a Peek of the store, never a SceneOf call: with the store off it finds nothing (no cut, no fault).
			sk->groups[9] = Group(false);
			const size_t before = sk->asks.size();
			const AdaptiveDownscale::CutVerdict skip = at(7, 9);
			Assert::IsTrue(skip.state == AdaptiveDownscale::CutVerdict::State::Known && !skip.cut, L"the skipped frame 8: no cut");
			Assert::AreEqual(before + 1, sk->asks.size(), L"only the current frame is asked through SceneOf");
		}

		static std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }
	};

#include "X5AbCaptureTests.inl"
}
