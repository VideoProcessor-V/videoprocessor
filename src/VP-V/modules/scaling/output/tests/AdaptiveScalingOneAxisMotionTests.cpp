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

#include <cmath>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

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
			const float alpha = MotionAlpha(1.0f / 24.0f, 250.0f);
			Assert::AreEqual(1.0f - std::exp(-1000.0f / 24.0f / 250.0f), alpha, 1e-6f);
			Assert::AreEqual(1.0f, MotionAlpha(1.0f / 24.0f, 0.0f), L"0 ms: no smoothing");
			// Same time constant at 24 and 60 fps: after 1 s both are within 1 % of each other.
			float v24 = 1.0f, v60 = 1.0f;
			for (int i = 0; i < 24; ++i) v24 = MotionStep(v24, 0.0f, MotionAlpha(1.0f / 24.0f, 250.0f), false);
			for (int i = 0; i < 60; ++i) v60 = MotionStep(v60, 0.0f, MotionAlpha(1.0f / 60.0f, 250.0f), false);
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
			const float alpha = AdaptiveDownscale::MotionAlpha(1.0f / 24.0f, 250.0f);
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
	};
}
