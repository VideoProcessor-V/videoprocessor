#pragma once

// Sigma-delta bias probe (gate verify 2026-10-17). Flat patches at many output
// levels go through the temporal sigma-delta alone (or after the LED hook and,
// on 7.360, the encoding repair, as the renderer assembles them), static, for
// many refreshes. Each patch's spatio-temporal mean is compared with the value
// the sigma-delta is meant to reproduce:
//   family A: every 10-bit level 0..1023 (the 10-bit grid, in the output code
//             domain: 8-in-10 code L/4, true 10-bit code L);
//   family B: off-grid levels around chosen codes, every 1/32 code from -0.5
//             to +0.47, made exactly representable in the rgba16f OUTPUT
//             texture (and nudged a quarter step up, so a truncating store
//             gives the same value), so the value the hook receives is known
//             exactly: anything the mean misses is the quantiser's.
// For a static pixel the first-order loop's time average lands within
// 2 * (0.5 + noise_amplitude) / frames of its input, so any larger signed
// error is a bias, not noise. Shared by the native tests (libplacebo 7.360)
// and VideoProcessor-BetaTests (377, output pass in rgba32f).

#include "ChainGpuProbe.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace SigmaDeltaBiasProbe
{
	struct Mode
	{
		const char* name;
		bool full;
		int colorDepth;   // 8 = 8-in-10 (the FL35 link), 10 = true 10-bit
	};

	inline std::vector<Mode> Modes()
	{
		return { { "8-in-10 limited (FL35)", false, 8 }, { "8-in-10 full", true, 8 },
			{ "10-bit limited", false, 10 }, { "10-bit full", true, 10 } };
	}

	struct Level
	{
		int family = 0;        // 0 = 10-bit grid, 1 = off-grid
		double xExact = 0.0;   // the level in output codes
		double xHook = 0.0;    // what the OUTPUT texture holds (rgba16f, round to nearest)
		double offset = 0.0;   // family B: the offset from the whole code
		double v = 0.0;        // the float RGB source value
		double xNudged = 0.0;  // family B: the source's own value (a quarter rgba16f step above xExact)
	};

	struct Result
	{
		double mean = 0.0;     // output codes
		double err = 0.0;      // mean - permissible(xExact)
		double errHook = 0.0;  // mean - permissible(xHook)
		double storeStep = 0.0; // one rgba16f step at the level (output codes): the OUTPUT texture's resolution
		int lo = 0, hi = 0;    // 10-bit readback codes
	};

	struct Report
	{
		bool ok = false;
		std::string error;
		std::vector<Level> levels;
		std::vector<Result> results;
		double codeMin = 0.0, codeMax = 0.0, unit10 = 1.0;   // output-code limits; 10-bit codes per output code
		double bound = 0.0;    // the static loop's deterministic bound (output codes)
		int nonGrid = 0;       // 8-in-10 readbacks off the 4k grid
		std::string text;
	};

	inline double HalfStep(double v)
	{
		v = std::fabs(v);
		int e = v > 0.0 ? static_cast<int>(std::floor(std::log2(v))) : -14;
		if (e < -14) e = -14;
		return std::ldexp(1.0, e - 10);
	}

	inline double HalfNearest(double v)
	{
		const double s = HalfStep(v);
		return std::nearbyint(v / s) * s;
	}

	inline double InScale(const Mode& m) { return m.colorDepth > 9 ? 1023.0 : (m.full ? 255.0 : 255.75); }

	// The OUTPUT-stage value (libplacebo's encoding, as the shader's domain
	// notes: limited 8-in-10 4k/1023, limited 10-bit code/1023, full code/max)
	// of a full-range RGB source value, and back.
	inline double SourceOf(const Mode& m, double out)
	{
		if (m.full) return out;
		return m.colorDepth > 9 ? (out * 1023.0 - 64.0) / 876.0 : (out * 1023.0 / 4.0 - 16.0) / 219.0;
	}

	inline std::vector<Level> Levels(const Mode& m)
	{
		std::vector<Level> levels;
		const double scale = InScale(m);
		const bool eight = m.colorDepth < 9;
		for (int L = 0; L <= 1023; ++L)
		{
			Level l;
			l.family = 0;
			l.xExact = eight ? L / 4.0 : static_cast<double>(L);
			const double out = l.xExact / scale;
			l.xHook = HalfNearest(out) * scale;
			l.v = SourceOf(m, out);
			levels.push_back(l);
		}
		const std::vector<int> codes8 = { 2, 4, 8, 16, 17, 20, 32, 50, 64, 100, 127, 128, 129, 160, 200, 234, 235, 236, 240, 250, 253 };
		const std::vector<int> codes10 = { 5, 8, 16, 64, 65, 100, 256, 400, 502, 512, 513, 700, 900, 940, 941, 1000, 1017 };
		for (int c : eight ? codes8 : codes10)
			for (int j = 0; j < 32; ++j)
			{
				Level l;
				l.family = 1;
				l.offset = -0.5 + j / 32.0;
				const double outHalf = HalfNearest((c + l.offset) / scale);
				l.xExact = l.xHook = outHalf * scale;
				l.v = SourceOf(m, outHalf + 0.25 * HalfStep(outHalf));
				l.xNudged = (outHalf + 0.25 * HalfStep(outHalf)) * scale;
				levels.push_back(l);
			}
		return levels;
	}

	inline Report Run(pl_gpu gpu, pl_renderer renderer, const ChainGpuProbe::ShaderSource& source, const Mode& m,
		bool outputFloat32, bool ledChain, bool withRepair, int warm = 16, int frames = 192, double noise = 0.15)
	{
		Report rep;
		rep.levels = Levels(m);
		// An rgba32f output pass holds the source's value itself: no half-float rounding, and the
		// off-grid levels' quarter-step nudge is part of the level.
		if (outputFloat32)
			for (Level& l : rep.levels)
				l.xHook = l.xExact = l.family == 1 ? l.xNudged : l.xExact;
		const int patch = 4, perRow = 64;
		const int n = static_cast<int>(rep.levels.size());
		const int w = perRow * patch, h = ((n + perRow - 1) / perRow) * patch;
		std::string error;
		auto sigma = ChainGpuProbe::Load(gpu, source, "Temporal sigma-delta.glsl", w, h, {
			{ "levels_full", m.full ? 1.0 : 0.0 }, { "swapchain_bits", 10.0 }, { "color_bits", static_cast<double>(m.colorDepth) },
			{ "noise_amplitude", noise } }, error);
		if (!sigma) { rep.error = error; return rep; }
		ChainGpuProbe::SetParam(sigma->hook, "output_float32", outputFloat32 ? 1.0 : 0.0);   // absent on old shaders
		std::unique_ptr<ChainGpuProbe::Shader> led;
		if (ledChain)
		{
			led = ChainGpuProbe::Load(gpu, source, "LED compensation.glsl", w, h, { { "gain", 1.0 }, { "boost", 1.0 },
				{ "gain_domain", 1.0 }, { "knee", 0.08 }, { "compensation_eotf", 2.41 } }, error);
			if (!led) { rep.error = error; return rep; }
		}
		OutputEncodingRepair::Hook repair;
		std::vector<const pl_hook*> hooks;
		if (ledChain) hooks.push_back(led->hook);
		hooks.push_back(sigma->hook);
		if (withRepair && OutputEncodingRepair::Needed(hooks))
		{
			pl_color_repr target{};
			target.sys = PL_COLOR_SYSTEM_RGB;
			target.levels = m.full ? PL_COLOR_LEVELS_FULL : PL_COLOR_LEVELS_LIMITED;
			target.bits.sample_depth = 10;
			target.bits.color_depth = m.colorDepth;
			target.bits.bit_shift = 10 - m.colorDepth;
			repair.SetTarget(target);
			hooks.insert(hooks.begin(), repair.Get());
		}

		pl_fmt sourceFormat = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32, PL_FMT_CAP_SAMPLEABLE);
		pl_fmt targetFormat = nullptr;
		const auto caps = PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE;
		for (int i = 0; i < gpu->num_formats; ++i)
		{
			pl_fmt f = gpu->formats[i];
			if (f->type == PL_FMT_UNORM && f->num_components == 4 && f->component_depth[0] == 10 &&
				f->texel_size == 4 && (f->caps & caps) == caps && f->host_bits[0] == 10)
			{ targetFormat = f; break; }
		}
		if (!sourceFormat || !targetFormat) { rep.error = "no float source / RGB10A2 target"; return rep; }
		std::vector<float> texels(static_cast<size_t>(w) * h * 4, 0.0f);
		for (int y = 0; y < h; ++y)
			for (int x = 0; x < w; ++x)
			{
				const int i = (y / patch) * perRow + x / patch;
				const float v = i < n ? static_cast<float>(rep.levels[i].v) : 0.0f;
				float* t = &texels[(static_cast<size_t>(y) * w + x) * 4];
				t[0] = t[1] = t[2] = v;
				t[3] = 1.0f;
			}
		pl_tex_params sp{};
		sp.w = w; sp.h = h; sp.format = sourceFormat; sp.sampleable = true; sp.initial_data = texels.data();
		pl_tex src = pl_tex_create(gpu, &sp);
		pl_tex_params tp{};
		tp.w = w; tp.h = h; tp.format = targetFormat; tp.renderable = true; tp.host_readable = true;
		pl_tex dst = pl_tex_create(gpu, &tp);
		struct Owner { pl_gpu g; pl_tex* a; pl_tex* b; ~Owner() { pl_tex_destroy(g, a); pl_tex_destroy(g, b); } } owner{ gpu, &src, &dst };
		if (!src || !dst) { rep.error = "texture creation failed"; return rep; }
		auto frame = [](pl_tex t) {
			pl_frame f{};
			f.num_planes = 1;
			f.planes[0].texture = t;
			f.planes[0].components = 4;
			for (int i = 0; i < 4; ++i) f.planes[0].component_mapping[i] = i;
			f.repr.sys = PL_COLOR_SYSTEM_RGB;
			f.repr.levels = PL_COLOR_LEVELS_FULL;
			f.color.primaries = PL_COLOR_PRIM_BT_709;
			f.color.transfer = PL_COLOR_TRC_GAMMA22;
			return f;
		};
		pl_frame image = frame(src), output = frame(dst);
		output.repr.levels = m.full ? PL_COLOR_LEVELS_FULL : PL_COLOR_LEVELS_LIMITED;
		output.repr.bits.sample_depth = 10;
		output.repr.bits.color_depth = m.colorDepth;
		output.repr.bits.bit_shift = 10 - m.colorDepth;
		pl_render_params params = pl_render_fast_params;
		params.dither_params = nullptr;
		params.error_diffusion = nullptr;
#if defined(PL_RENDER_PARAMS_OUTPUT_FBO_FLOAT32)
		params.output_fbo_float32 = outputFloat32;
#else
		if (outputFloat32) { rep.error = "this libplacebo has no output_fbo_float32"; return rep; }
#endif
		params.hooks = hooks.data();
		params.num_hooks = static_cast<int>(hooks.size());
		std::vector<uint32_t> pixels(static_cast<size_t>(w) * h);
		std::vector<double> sum(static_cast<size_t>(n), 0.0);
		std::vector<int> lo(static_cast<size_t>(n), 1023), hi(static_cast<size_t>(n), 0);
		const bool eight = m.colorDepth < 9;
		for (int f = 0; f < warm + frames; ++f)
		{
			if (!pl_render_image(renderer, &image, &output, &params)) { rep.error = "pl_render_image failed"; return rep; }
			if (pl_renderer_get_errors(renderer).num_disabled_hooks != 0) { rep.error = "a hook was disabled"; return rep; }
			if (f < warm) continue;
			pl_tex_transfer_params download{};
			download.tex = dst;
			download.ptr = pixels.data();
			if (!pl_tex_download(gpu, &download)) { rep.error = "download failed"; return rep; }
			for (int y = 0; y < h; ++y)
				for (int x = 0; x < w; ++x)
				{
					const int i = (y / patch) * perRow + x / patch;
					if (i >= n) continue;
					const int c = static_cast<int>(pixels[static_cast<size_t>(y) * w + x] & 1023u);
					sum[i] += c;
					lo[i] = (std::min)(lo[i], c);
					hi[i] = (std::max)(hi[i], c);
					if (eight && c % 4 != 0) ++rep.nonGrid;
				}
		}
		const auto limits = SigmaDeltaDomain::Limits(m.colorDepth, m.full);
		rep.codeMin = limits.min;
		rep.codeMax = limits.max;
		rep.unit10 = eight ? 4.0 : 1.0;
		rep.bound = 2.0 * (0.5 + noise) / frames;
		// 8-in-10 stores k as 4k (limited: 4k/1023; full: k/255 x 1020/1023 after the bit shift);
		// should the full-range store ever land off that grid, k/255 x 1023 is decoded instead.
		const double perCode = eight ? (rep.nonGrid == 0 ? 4.0 : 1023.0 / 255.0) : 1.0;
		auto permissible = [&](double x) { return (std::min)((std::max)(x, rep.codeMin), rep.codeMax); };
		const double samples = static_cast<double>(frames) * patch * patch;
		for (int i = 0; i < n; ++i)
		{
			Result r;
			r.mean = sum[i] / samples / perCode;
			r.err = r.mean - permissible(rep.levels[i].xExact);
			r.errHook = r.mean - permissible(rep.levels[i].xHook);
			r.lo = lo[i];
			r.hi = hi[i];
			r.storeStep = HalfStep(rep.levels[i].xExact / InScale(m)) * InScale(m);
			rep.results.push_back(r);
		}
		rep.ok = true;
		return rep;
	}

	// A readable summary, errors in 10-bit codes. `significant`: the threshold
	// (output codes) above which a signed error counts as a bias.
	// The worst level against the OUTPUT texture's own resolution: |mean - level| over
	// max(floor, 1.05 rgba16f steps at the level), clamped levels left out. Above 1: the quantiser
	// moved a level by more than the store it reads can account for (a bias of its own).
	inline double WorstAgainstStore(const Report& rep, double floor, size_t* at = nullptr)
	{
		double worst = 0.0;
		for (size_t i = 0; i < rep.levels.size(); ++i)
		{
			const Level& l = rep.levels[i];
			if (l.xExact < rep.codeMin || l.xExact > rep.codeMax) continue;
			const double ratio = std::fabs(rep.results[i].err) / (std::max)(floor, 1.05 * rep.results[i].storeStep);
			if (ratio > worst) { worst = ratio; if (at) *at = i; }
		}
		return worst;
	}

	inline std::string Summarise(const std::string& title, const Report& rep, double significant, double* worstHook = nullptr)
	{
		char line[400];
		std::string text = title + "\n";
		if (!rep.ok) return text + "  error: " + rep.error + "\n";
		std::snprintf(line, sizeof(line), "  loop bound %.4f code; 8-in-10 off-grid readbacks %d\n", rep.bound, rep.nonGrid);
		text += line;
		double worst = 0.0;
		for (int family = 0; family < 2; ++family)
		{
			// Bands by output level (fraction of full scale): near black, mid, near white.
			const char* bandName[3] = { "near-black (<12%)", "mid", "near-white (>80%)" };
			double sE[3] = {}, sH[3] = {}, mE[3] = {}, mH[3] = {};
			int cnt[3] = {}, sig[3] = {}, sigPos[3] = {}, sigNeg[3] = {};
			const double top = rep.codeMax;
			for (size_t i = 0; i < rep.levels.size(); ++i)
			{
				const Level& l = rep.levels[i];
				if (l.family != family) continue;
				if (l.xExact < rep.codeMin || l.xExact > rep.codeMax) continue;   // clamped: reported apart
				const Result& r = rep.results[i];
				const int b = l.xExact < 0.12 * top ? 0 : (l.xExact > 0.8 * top ? 2 : 1);
				sE[b] += r.err; sH[b] += r.errHook; ++cnt[b];
				mE[b] = (std::max)(mE[b], std::fabs(r.err));
				mH[b] = (std::max)(mH[b], std::fabs(r.errHook));
				worst = (std::max)(worst, std::fabs(r.errHook));
				if (std::fabs(r.errHook) > significant) { ++sig[b]; (r.errHook > 0 ? sigPos[b] : sigNeg[b])++; }
			}
			for (int b = 0; b < 3; ++b)
			{
				if (!cnt[b]) continue;
				std::snprintf(line, sizeof(line),
					"  %s %-18s n %4d | vs level: mean %+.3f max|%.3f| | vs hook input: mean %+.3f max|%.3f| biased %d (+%d/-%d)  [10-bit codes]\n",
					family == 0 ? "A grid    " : "B off-grid", bandName[b], cnt[b], sE[b] / cnt[b] * rep.unit10, mE[b] * rep.unit10,
					sH[b] / cnt[b] * rep.unit10, mH[b] * rep.unit10, sig[b], sigPos[b], sigNeg[b]);
				text += line;
			}
		}
		// Family B by offset from the whole code (mean over the codes): the
		// shape of any value-dependent bias.
		text += "  B signed error vs hook input by offset from the code (10-bit codes):";
		for (int j = 0; j < 32; ++j)
		{
			double s = 0.0;
			int c = 0;
			for (size_t i = 0; i < rep.levels.size(); ++i)
				if (rep.levels[i].family == 1 && std::fabs(rep.levels[i].offset - (-0.5 + j / 32.0)) < 1e-9 &&
					rep.levels[i].xExact >= rep.codeMin && rep.levels[i].xExact <= rep.codeMax)
				{ s += rep.results[i].errHook; ++c; }
			std::snprintf(line, sizeof(line), "%s %+.3f:%+.3f", j % 8 == 0 ? "\n   " : "", -0.5 + j / 32.0, c ? s / c * rep.unit10 : 0.0);
			text += line;
		}
		{
			size_t at = 0;
			const double w = WorstAgainstStore(rep, significant, &at);
			std::snprintf(line, sizeof(line), "\n  worst against the rgba16f step at the level: %.2f (level %.4f, error %+.3f 10-bit, step %.4f code)", w,
				rep.levels[at].xExact, rep.results[at].err * rep.unit10, rep.results[at].storeStep);
			text += line;
		}
		text += "\n  worst levels (output code: exact / hook input -> mean, readback range):";
		std::vector<size_t> order(rep.levels.size());
		for (size_t i = 0; i < order.size(); ++i) order[i] = i;
		std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return std::fabs(rep.results[a].errHook) > std::fabs(rep.results[b].errHook); });
		for (size_t k = 0; k < 8 && k < order.size(); ++k)
		{
			const size_t i = order[k];
			std::snprintf(line, sizeof(line), "\n    %.4f / %.4f -> %.4f (%+.3f 10-bit) [%d-%d]", rep.levels[i].xExact, rep.levels[i].xHook,
				rep.results[i].mean, rep.results[i].errHook * rep.unit10, rep.results[i].lo, rep.results[i].hi);
			text += line;
		}
		text += "\n";
		if (worstHook) *worstHook = worst;
		return text;
	}
}
