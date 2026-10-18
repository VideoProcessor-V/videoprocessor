#include "pch.h"
#include "CppUnitTest.h"

#include "ExtendedRangeGpuProbe.h"

#include <vprenderer/CrossfadeMix.h>
#include <vprenderer/ExtendedRange.h>
#include <vprenderer/OutputEncodingRepair.h>
#include <vprenderer/SourceResolutionMonitor.h>

#include <libplacebo/d3d11.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <type_traits>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// Hunt 3 F1: the source-resolution crossfade mixed in the output signal domain by VP's own
// PRE_OUTPUT hooks (CrossfadeMix.h). CPU: the mix (exact ends, monotonic, BTB/WTW carried as
// values), which hooks the path being left renders with, where the mix goes, and a model of a
// ramp through a forced 0.5 fade with and without a calibration LUT against libplacebo's
// linear-light mixer. GPU (CrossfadeGpu*, for Claude: WARP or VP_TEST_GPU=hardware): the real
// render of a BTB/WTW ramp through both paths at weight 0.5.
namespace CrossfadeMixTestsNs
{
	inline std::wstring W(const std::string& s) { return std::wstring(s.begin(), s.end()); }

	// The normalised signal of a 10-bit limited code (BTB < 0, WTW > 1).
	inline double Signal(int code) { return (code - 64) / 876.0; }

	// A pure-power transfer as libplacebo's frame mixer applies it around the mix: linearise
	// with a clamp at 0 (renderer.c: pl_shader_linearize after the frame's colour), mix,
	// delinearise.
	inline double LibplaceboMix(double from, double to, double w, double gamma)
	{
		const double lf = std::pow((std::max)(from, 0.0), gamma), lt = std::pow((std::max)(to, 0.0), gamma);
		return std::pow((1.0 - w) * lf + w * lt, 1.0 / gamma);
	}

	inline pl_hook FakeHook(enum pl_hook_stage stages, uint64_t signature)
	{
		pl_hook h{};
		h.stages = stages;
		h.signature = signature;
		return h;
	}

	// An affine LUT with cross-talk (as ExtendedRangeCarryTests): out = A p + t.
	inline void Affine(float r, float g, float b, float out[3])
	{
		out[0] = 0.02f + 0.90f * r + 0.05f * g + 0.01f * b;
		out[1] = 0.01f + 0.03f * r + 0.92f * g + 0.02f * b;
		out[2] = 0.03f + 0.02f * r + 0.04f * g + 0.88f * b;
	}

	inline std::vector<float> AffineLut(int n)
	{
		std::vector<float> data(static_cast<size_t>(n) * n * n * 3);
		for (int b = 0; b < n; ++b)
			for (int g = 0; g < n; ++g)
				for (int r = 0; r < n; ++r)
				{
					float out[3];
					Affine(r / float(n - 1), g / float(n - 1), b / float(n - 1), out);
					const size_t i = ((static_cast<size_t>(b) * n + g) * n + r) * 3;
					data[i] = out[0]; data[i + 1] = out[1]; data[i + 2] = out[2];
				}
		return data;
	}

	TEST_CLASS(CrossfadeMixTests)
	{
	public:
		TEST_METHOD(CrossfadeSignalMixIsExactMonotonicAndKeepsBtbWtw)
		{
			const double pairs[][2] = { { 0.0626, 0.9188 }, { 0.9188, 0.0626 }, { -0.0685, -0.0457 }, { 1.0959, 1.0457 }, { -0.0685, 1.0959 } };
			for (const auto& pr : pairs)
			{
				Assert::IsTrue(CrossfadeMix::MixValue(pr[0], pr[1], 0.0) == pr[0], L"weight 0: the old path exactly");
				Assert::IsTrue(CrossfadeMix::MixValue(pr[0], pr[1], 1.0) == pr[1], L"weight 1: the new path exactly");
				double last = pr[0];
				for (int i = 0; i <= 1000; ++i)
				{
					const double v = CrossfadeMix::MixValue(pr[0], pr[1], i / 1000.0);
					Assert::IsTrue(pr[1] >= pr[0] ? v >= last - 1e-15 : v <= last + 1e-15, L"monotonic");
					last = v;
				}
				Assert::AreEqual(0.5 * (pr[0] + pr[1]), CrossfadeMix::MixValue(pr[0], pr[1], 0.5), 1e-15, L"the midpoint");
			}
			// The same BTB / WTW value on both paths stays exactly that value at every weight.
			for (int code : { 4, 16, 40, 64, 940, 980, 1019 })
				for (int i = 0; i <= 20; ++i)
					Assert::AreEqual(Signal(code), CrossfadeMix::MixValue(Signal(code), Signal(code), i / 20.0), 1e-15);
		}

		TEST_METHOD(CrossfadeRampThroughAHalfWeightFadeKeepsBtbWtw)
		{
			// A ramp from code 4 (BTB) to 1019 (WTW), the two paths' pictures a little apart (another
			// re-base resamples: here +-1 code) at weight 0.5. VP's signal-domain mix of the two
			// restored pictures gives their midpoint (the ramp itself) everywhere; libplacebo's mixer
			// (linear light, clamped at 0) clips every BTB code to black. With a calibration LUT each
			// path's value after the late restore (the LUT extrapolated beyond its edge) is mixed:
			// the midpoint of the two exact values; libplacebo's late restore after the mix adds
			// the last-rendered frame's cut part only.
			const std::vector<float> lutData = AffineLut(17);
			ExtendedRange::LutView lut;
			lut.size[0] = lut.size[1] = lut.size[2] = 17;
			lut.data = lutData.data();
			double worstVp = 0.0, worstVpLut = 0.0, worstLateLut = 0.0;
			int clipped = 0, btb = 0;
			for (int code = 4; code <= 1019; ++code)
			{
				const double v = Signal(code), from = v - 1.0 / 876.0, to = v + 1.0 / 876.0;
				const double vp = CrossfadeMix::MixValue(from, to, 0.5);
				worstVp = (std::max)(worstVp, std::fabs(vp - v) * 876.0);
				if (code < 64)
				{
					++btb;
					const double lp = LibplaceboMix(from, to, 0.5, 2.4);
					if (lp >= 0.0) ++clipped;
				}
				// The LUT case: grey (r = g = b), the green output channel.
				float fv[3] = { float(from), float(from), float(from) }, tv[3] = { float(to), float(to), float(to) }, iv[3] = { float(v), float(v), float(v) };
				float fo[3], to3[3], ideal[3];
				ExtendedRange::LutExtrapolate(lut, fv, fo);
				ExtendedRange::LutExtrapolate(lut, tv, to3);
				Affine(iv[0], iv[1], iv[2], ideal);
				const double mixed = CrossfadeMix::MixValue(fo[1], to3[1], 0.5);
				worstVpLut = (std::max)(worstVpLut, std::fabs(mixed - ideal[1]) * 876.0);
				// libplacebo: LUT(clamp) per frame, mixed, then the late restore with the last
				// frame's (the new path's) cut part only.
				float fc[3] = { float((std::min)(1.0, (std::max)(0.0, from))), float((std::min)(1.0, (std::max)(0.0, from))), float((std::min)(1.0, (std::max)(0.0, from))) };
				float tc[3] = { float((std::min)(1.0, (std::max)(0.0, to))), float((std::min)(1.0, (std::max)(0.0, to))), float((std::min)(1.0, (std::max)(0.0, to))) };
				float lf[3], lt[3];
				ExtendedRange::LutSample(lut, fc, lf);
				ExtendedRange::LutSample(lut, tc, lt);
				const double late = 0.5 * (lf[1] + lt[1]) + (to3[1] - lt[1]);
				worstLateLut = (std::max)(worstLateLut, std::fabs(late - ideal[1]) * 876.0);
			}
			char text[260];
			snprintf(text, sizeof(text), "half-weight fade of a 4..1019 ramp: VP mix worst %.2e codes (LUT %.2e); libplacebo mixer clips %d of %d BTB codes; its late LUT restore off by up to %.2f codes",
				worstVp, worstVpLut, clipped, btb, worstLateLut);
			Logger::WriteMessage(text);
			Assert::IsTrue(worstVp < 1e-9 && worstVpLut < 1e-3, W(text).c_str());
			Assert::AreEqual(btb, clipped, L"(the finding: libplacebo's mixer clips BTB)");
		}

		TEST_METHOD(CrossfadeFromPathTakesThePictureHooksOnly)
		{
			// The path being left renders with every hook before PRE_OUTPUT (the hooks libplacebo's
			// mixer ran per frame) and the extended-range late restore; never the LED hooks, the
			// rendered-picture cache, the sigma-delta or any other PRE_OUTPUT / OUTPUT hook (they run
			// once, on the mixed picture). The mix goes right after the late restore.
			const pl_hook late = FakeHook(PL_HOOK_PRE_OUTPUT, 1), restore = FakeHook(PL_HOOK_SCALED, 2), nls = FakeHook(PL_HOOK_NATIVE, 3),
				led = FakeHook(PL_HOOK_PRE_OUTPUT, 4), sigma = FakeHook(PL_HOOK_OUTPUT, 5), carry = FakeHook(PL_HOOK_RGB, 6),
				both = FakeHook(static_cast<enum pl_hook_stage>(PL_HOOK_LUMA_INPUT | PL_HOOK_OUTPUT), 7), cache = FakeHook(PL_HOOK_PRE_OUTPUT, 8),
				kernel = FakeHook(PL_HOOK_PRE_KERNEL, 9);
			const pl_hook* list[] = { &sigma, &late, &restore, &nls, &kernel, &led, &both, &carry, &cache };
			const std::vector<const pl_hook*> from = CrossfadeMix::FromPathHooks(list, 9, &late);
			const std::vector<const pl_hook*> want = { &late, &restore, &nls, &kernel, &carry };
			Assert::IsTrue(from == want, L"picture-stage hooks and the late restore, in order");
			const std::vector<const pl_hook*> noLate = CrossfadeMix::FromPathHooks(list, 9, nullptr);
			Assert::AreEqual(size_t(4), noLate.size(), L"no late restore given: picture stages only");
			std::vector<const pl_hook*> all(list, list + 9);
			Assert::AreEqual(size_t(2), CrossfadeMix::MixInsertIndex(all, &late), L"right after the late restore");
			Assert::AreEqual(size_t(0), CrossfadeMix::MixInsertIndex(all, nullptr), L"first without one");
			const pl_hook* none[] = { &sigma };
			Assert::AreEqual(size_t(0), CrossfadeMix::MixInsertIndex(std::vector<const pl_hook*>(none, none + 1), &late));
		}
	};

	// ---- GPU (for Claude): WARP or VP_TEST_GPU=hardware ----

	struct FadeResult
	{
		bool ok = false;
		std::string error;
		std::vector<int> columns;   // green channel at each column's centre
	};

	// to: full-size planes; from: the same picture at half size (the other re-base). mode 0: the
	// new path alone; 1: libplacebo's frame mixer at weight w (the old crossfade); 2: VP's
	// signal-domain mix at weight w.
	inline FadeResult RenderFade(pl_gpu gpu, pl_renderer renderer, const ExtendedRangeGpuProbe::Planes& toPlanes,
		const ExtendedRangeGpuProbe::Planes& fromPlanes, const pl_custom_lut* lut, int mode, double w, int columns, int columnWidth)
	{
		FadeResult result;
		const auto sampleCaps = static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_LINEAR);
		pl_fmt r16 = pl_find_fmt(gpu, PL_FMT_UNORM, 1, 16, 16, sampleCaps);
		pl_fmt rg16 = pl_find_fmt(gpu, PL_FMT_UNORM, 2, 16, 16, sampleCaps);
		pl_fmt t10 = nullptr;
		const auto caps = static_cast<enum pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE);
		for (int i = 0; i < gpu->num_formats; ++i)
		{
			pl_fmt f = gpu->formats[i];
			if (f->type == PL_FMT_UNORM && f->num_components == 4 && f->component_depth[0] == 10 &&
				f->texel_size == 4 && (f->caps & caps) == caps && f->host_bits[0] == 10) { t10 = f; break; }
		}
		if (!r16 || !rg16 || !t10) { result.error = "formats"; return result; }
		auto planesTex = [&](const ExtendedRangeGpuProbe::Planes& p, pl_tex& l, pl_tex& c) {
			pl_tex_params lp{}; lp.w = p.width; lp.h = p.height; lp.format = r16; lp.sampleable = true; lp.initial_data = p.luma.data();
			pl_tex_params cp{}; cp.w = p.width / 2; cp.h = p.height; cp.format = rg16; cp.sampleable = true; cp.initial_data = p.chroma.data();
			l = pl_tex_create(gpu, &lp);
			c = pl_tex_create(gpu, &cp);
		};
		pl_tex tl = nullptr, tc = nullptr, fl = nullptr, fc = nullptr;
		planesTex(toPlanes, tl, tc);
		planesTex(fromPlanes, fl, fc);
		pl_tex_params tp{}; tp.w = toPlanes.width; tp.h = toPlanes.height; tp.format = t10; tp.renderable = true; tp.host_readable = true;
		tp.blit_dst = (t10->caps & PL_FMT_CAP_BLITTABLE) != 0;
		pl_tex tt = pl_tex_create(gpu, &tp);
		if (!tl || !tc || !fl || !fc || !tt) { result.error = "textures"; }
		else
		{
			auto frameOf = [&](pl_tex l, pl_tex c) {
				pl_frame image{};
				image.num_planes = 2;
				image.planes[0].texture = l; image.planes[0].components = 1; image.planes[0].component_mapping[0] = PL_CHANNEL_Y;
				image.planes[1].texture = c; image.planes[1].components = 2;
				image.planes[1].component_mapping[0] = PL_CHANNEL_CB; image.planes[1].component_mapping[1] = PL_CHANNEL_CR;
				image.repr.sys = PL_COLOR_SYSTEM_BT_709; image.repr.levels = PL_COLOR_LEVELS_LIMITED;
				image.repr.bits.sample_depth = 16; image.repr.bits.color_depth = 10; image.repr.bits.bit_shift = 6;
				image.color.primaries = PL_COLOR_PRIM_BT_709;
				image.color.transfer = PL_COLOR_TRC_GAMMA24;
				pl_frame_set_chroma_location(&image, PL_CHROMA_LEFT);
				return image;
			};
			pl_frame to = frameOf(tl, tc), from = frameOf(fl, fc);
			pl_frame out{};
			out.num_planes = 1;
			out.planes[0].texture = tt; out.planes[0].components = 3;
			for (int i = 0; i < 3; ++i) out.planes[0].component_mapping[i] = i;
			out.repr.sys = PL_COLOR_SYSTEM_RGB; out.repr.levels = PL_COLOR_LEVELS_LIMITED;
			out.repr.bits.sample_depth = 10; out.repr.bits.color_depth = 10;
			out.color.primaries = PL_COLOR_PRIM_BT_709;
			out.color.transfer = PL_COLOR_TRC_GAMMA24;
			if (lut) { out.lut = lut; out.lut_type = PL_LUT_NORMALIZED; }
			pl_render_params params = pl_render_default_params;
			params.dither_params = nullptr;
			params.error_diffusion = nullptr;
#if defined(PL_RENDER_PARAMS_OUTPUT_FBO_FLOAT32)
			params.output_fbo_float32 = true;
#endif
			ExtendedRange::Hooks hooks;
			std::vector<const pl_hook*> list;
			const ExtendedRange::Decision decision = ExtendedRange::Decide(true, ExtendedRange::Settings{}, to.color, out.color,
				lut, lut ? PL_LUT_NORMALIZED : PL_LUT_UNKNOWN);
			pl_color_space prepared = to.color;
			ExtendedRange::Prepare(decision, hooks, prepared, out.color, list, false);
			to.color = from.color = prepared;
			// As the renderer: libplacebo 7.360 drops the output encoding after a texture-input
			// PRE_OUTPUT hook (the late restore, the mix) unless the repair runs first at OUTPUT.
			OutputEncodingRepair::Hook repair;
			repair.SetTarget(out.repr);
			auto withRepair = [&](std::vector<const pl_hook*>& hooks) {
				if (OutputEncodingRepair::Needed(hooks, nullptr, lut ? PL_LUT_NORMALIZED : PL_LUT_UNKNOWN))
					hooks.insert(hooks.begin(), repair.Get());
			};
			withRepair(list);
			params.hooks = list.empty() ? nullptr : list.data();
			params.num_hooks = static_cast<int>(list.size());
			const float black[3] = { 0.0f, 0.0f, 0.0f };
			pl_frame_clear(gpu, &out, black);
			bool rendered = false;
			CrossfadeMix::Mixer mixer;
			const pl_hook* late = decision.Active() && decision.LateRestore() ? hooks.LateRestore() : nullptr;
			if (mode == 0)
				rendered = pl_render_image(renderer, &to, &out, &params);
			else if (mode == 1)
			{
				float ts[2];
				SourceResolutionMonitor::Monitor::FadeMixTimestamps(w, ts);
				const pl_frame* frames[2] = { &from, &to };
				const uint64_t signatures[2] = { 0x51u, 0x52u };
				using MixTimestamp = std::remove_const_t<std::remove_pointer_t<decltype(pl_frame_mix{}.timestamps)>>;
				const MixTimestamp mixTimestamps[2] = { static_cast<MixTimestamp>(ts[0]), static_cast<MixTimestamp>(ts[1]) };
				pl_frame_mix mix{};
				mix.num_frames = 2; mix.frames = frames; mix.signatures = signatures; mix.timestamps = mixTimestamps;
				mix.vsync_duration = 1e-3f;
				pl_render_params fade = params;
				fade.frame_mixer = &pl_filter_triangle;
				fade.preserve_mixing_cache = false;
				rendered = pl_render_image_mix(renderer, &mix, &out, &fade);
			}
			else
			{
				if (!mixer.RenderFrom(renderer, gpu, from, out, params, late))
					result.error = "RenderFrom";
				else
				{
					std::vector<const pl_hook*> withMix = mixer.WithMix(params.hooks, params.num_hooks, late, w);
					if (std::find(withMix.begin(), withMix.end(), repair.Get()) == withMix.end())
						withRepair(withMix);
					pl_render_params p2 = params;
					p2.hooks = withMix.data();
					p2.num_hooks = static_cast<int>(withMix.size());
					rendered = pl_render_image(renderer, &to, &out, &p2) && mixer.Mixed() > 0;
				}
			}
			if (rendered)
			{
				pl_gpu_finish(gpu);
				std::vector<uint32_t> pixels(static_cast<size_t>(tp.w) * tp.h);
				pl_tex_transfer_params d{}; d.tex = tt; d.ptr = pixels.data();
				result.ok = pl_tex_download(gpu, &d);
				for (int c = 0; result.ok && c < columns; ++c)
					result.columns.push_back(static_cast<int>((pixels[static_cast<size_t>(tp.h / 2) * tp.w + c * columnWidth + columnWidth / 2] >> 10) & 1023u));
			}
			else if (result.error.empty())
				result.error = "render";
			mixer.Release(gpu);
			hooks.Release();
		}
		for (pl_tex* t : { &tl, &tc, &fl, &fc, &tt })
			if (*t) pl_tex_destroy(gpu, t);
		return result;
	}

	TEST_CLASS(CrossfadeMixGpuTests)
	{
	public:
		TEST_METHOD(CrossfadeGpuKeepsBtbWtwOnARamp)
		{
			// A grey ramp with BTB and WTW columns, the path being left at half size, weight 0.5
			// (forced), extended range on, without and with a calibration LUT (an affine 17^3 LUT):
			// VP's mix equals the new path alone (+-1 code; the ramp is flat across each column,
			// so both paths have the same values); libplacebo's mixer is logged (it clipped BTB).
			pl_log_params logParams{};
			logParams.log_level = PL_LOG_NONE;
			pl_log log = pl_log_create(PL_API_VER, &logParams);
			std::string device;
			const pl_d3d11_params deviceParams = ExtendedRangeGpuProbe::TestDeviceParams(&device);
			Logger::WriteMessage(("GPU tests on: " + device + "\n").c_str());
			pl_d3d11 d3d = pl_d3d11_create(log, &deviceParams);
			Assert::IsNotNull(d3d, L"Could not create the libplacebo test device");
			pl_renderer renderer = pl_renderer_create(log, d3d->gpu);
			const std::vector<int> codes = { 4, 16, 40, 64, 100, 300, 600, 900, 940, 980, 1019 };
			const ExtendedRangeGpuProbe::Planes to = ExtendedRangeGpuProbe::Greys(codes, 32, 16);
			const ExtendedRangeGpuProbe::Planes from = ExtendedRangeGpuProbe::Greys(codes, 16, 8);
			const std::vector<float> lutData = AffineLut(17);
			pl_custom_lut lut{};
			lut.signature = 0x4655;
			lut.size[0] = lut.size[1] = lut.size[2] = 17;
			lut.data = lutData.data();
			for (int withLut = 0; withLut < 2; ++withLut)
			{
				const pl_custom_lut* l = withLut ? &lut : nullptr;
				const FadeResult alone = RenderFade(d3d->gpu, renderer, to, from, l, 0, 0.5, int(codes.size()), 32);
				const FadeResult old = RenderFade(d3d->gpu, renderer, to, from, l, 1, 0.5, int(codes.size()), 32);
				const FadeResult vp = RenderFade(d3d->gpu, renderer, to, from, l, 2, 0.5, int(codes.size()), 32);
				Assert::IsTrue(alone.ok && vp.ok, W("render failed: " + alone.error + " / " + vp.error).c_str());
				int worst = 0;
				for (size_t i = 0; i < codes.size(); ++i) worst = (std::max)(worst, std::abs(vp.columns[i] - alone.columns[i]));
				const std::string text = std::string(withLut ? "LUT: " : "no LUT: ") + "alone " + ExtendedRangeGpuProbe::Join(alone.columns) +
					" | VP mix " + ExtendedRangeGpuProbe::Join(vp.columns) + " | libplacebo mix " + (old.ok ? ExtendedRangeGpuProbe::Join(old.columns) : old.error);
				Logger::WriteMessage(text.c_str());
				Assert::IsTrue(worst <= 1, W(text).c_str());
				Assert::IsTrue(alone.columns[0] < alone.columns[3], W("BTB below black: " + text).c_str());
				Assert::IsTrue(vp.columns[0] < vp.columns[3] && vp.columns[10] > vp.columns[8], W("BTB/WTW kept by the VP mix: " + text).c_str());
			}
			pl_renderer_destroy(&renderer);
			pl_d3d11_destroy(&d3d);
			pl_log_destroy(&log);
		}
	};
}
