// GPU checks of the Beta renderer's libplacebo (master, API 377) on D3D11
// WARP, independent of the rest of VP:
// 1. The output clamp (upstream e2972fdd) keeps limited-range footroom and
//    headroom (FL35 patches 0001/0002: legal codes 4-1019, identical on R, G
//    and B). Unpatched it cuts to 64-940 (G/B to 960).
// 2. libplacebo's PRE_OUTPUT bug (OutputEncodingRepair.h) is still present:
//    a texture-input PRE_OUTPUT hook drops the output encoding, and VP's
//    repair restores it. If a libplacebo update fixes the bug, the repair
//    would encode twice and must be switched off for the Beta.
#include "../VideoProcessor-Lib/vprenderer/OutputEncodingRepair.h"
#include "../VideoProcessor-Lib/vprenderer/DejudderWarp.h"
#include "../VideoProcessor-Lib/vprenderer/OcclusionMaskHook.h"
#include "../VideoProcessor-Lib/vprenderer/RenderedWarp.h"
#include "../VideoProcessor-Test/ExtendedRangeGpuProbe.h"
#include "../VideoProcessor-Test/ChainGpuProbe.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>

#include <libplacebo/d3d11.h>
#include <libplacebo/log.h>
#include <libplacebo/renderer.h>
#include <libplacebo/shaders/custom.h>

#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <sstream>
#include <string>
#include <vector>

namespace
{
	void Check(bool result, const std::string& message)
	{
		if (!result) throw std::runtime_error("Beta GPU: " + message);
	}

	struct Pixel { int r, g, b; };

	class Gpu
	{
	public:
		Gpu()
		{
			pl_log_params logParams{};
			logParams.log_level = PL_LOG_WARN;
			logParams.log_cb = [](void* priv, pl_log_level, const char* message)
			{
				static_cast<std::string*>(priv)->append(message).append("\n");
			};
			logParams.log_priv = &messages;
			log = pl_log_create(PL_API_VER, &logParams);
			// WARP unless VP_TEST_GPU=hardware (the NVIDIA adapter when present).
			std::string device;
			const pl_d3d11_params deviceParams = ExtendedRangeGpuProbe::TestDeviceParams(&device);
			const bool hardware = ExtendedRangeGpuProbe::OnHardwareGpu();
			static bool reported = false;
			if (!reported)
				std::cout << "GPU tests on: " << device << "\n";
			reported = true;
			d3d11 = log ? pl_d3d11_create(log, &deviceParams) : nullptr;
			renderer = d3d11 ? pl_renderer_create(log, d3d11->gpu) : nullptr;
			Check(renderer != nullptr, hardware ? "no libplacebo hardware device" : "no libplacebo WARP device");
		}

		~Gpu()
		{
			if (renderer) pl_renderer_destroy(&renderer);
			if (d3d11) pl_d3d11_destroy(&d3d11);
			if (log) pl_log_destroy(&log);
		}

		static pl_frame Frame(pl_tex texture)
		{
			pl_frame frame{};
			frame.num_planes = 1;
			frame.planes[0].texture = texture;
			frame.planes[0].components = 4;
			for (int i = 0; i < 4; ++i) frame.planes[0].component_mapping[i] = i;
			frame.repr.sys = PL_COLOR_SYSTEM_RGB;
			frame.repr.levels = PL_COLOR_LEVELS_FULL;
			frame.color.primaries = PL_COLOR_PRIM_BT_709;
			frame.color.transfer = PL_COLOR_TRC_GAMMA22;
			return frame;
		}

		// One grey value (any float, including below 0 and above 1) through a
		// same-colour-space render onto a 10-bit limited RGB target.
		Pixel Render(float value, int colorDepth, const std::vector<const pl_hook*>& hooks = {},
			enum pl_color_levels levels = PL_COLOR_LEVELS_LIMITED)
		{
			pl_gpu gpu = d3d11->gpu;
			pl_fmt sourceFormat = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32, PL_FMT_CAP_SAMPLEABLE);
			pl_fmt targetFormat = nullptr;
			const auto caps = PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE;
			for (int i = 0; i < gpu->num_formats; ++i)
			{
				pl_fmt f = gpu->formats[i];
				if (f->type == PL_FMT_UNORM && f->num_components == 4 && f->component_depth[0] == 10 &&
					f->texel_size == 4 && (f->caps & caps) == caps && f->host_bits[0] == 10)
				{
					targetFormat = f;
					break;
				}
			}
			Check(sourceFormat && targetFormat, "no float source / RGB10A2 target format");
			std::vector<float> texels(2 * 2 * 4);
			for (int i = 0; i < 4; ++i)
			{
				texels[i * 4] = texels[i * 4 + 1] = texels[i * 4 + 2] = value;
				texels[i * 4 + 3] = 1.0f;
			}
			pl_tex_params sp{};
			sp.w = sp.h = 2;
			sp.format = sourceFormat;
			sp.sampleable = true;
			sp.initial_data = texels.data();
			pl_tex source = pl_tex_create(gpu, &sp);
			pl_tex_params tp{};
			tp.w = tp.h = 2;
			tp.format = targetFormat;
			tp.renderable = true;
			tp.host_readable = true;
			pl_tex target = pl_tex_create(gpu, &tp);
			Check(source && target, "texture creation failed");
			pl_frame image = Frame(source);
			pl_frame output = Frame(target);
			output.repr.levels = levels;
			output.repr.bits.sample_depth = 10;
			output.repr.bits.color_depth = colorDepth;
			output.repr.bits.bit_shift = 10 - colorDepth;
			pl_render_params params = pl_render_fast_params;
			params.dither_params = nullptr;
			params.error_diffusion = nullptr;
			params.hooks = hooks.empty() ? nullptr : hooks.data();
			params.num_hooks = static_cast<int>(hooks.size());
			Check(pl_render_image(renderer, &image, &output, &params), "pl_render_image failed");
			pl_gpu_finish(gpu);
			uint32_t pixels[4] = {};
			pl_tex_transfer_params download{};
			download.tex = target;
			download.ptr = pixels;
			Check(pl_tex_download(gpu, &download), "download failed");
			pl_tex_destroy(gpu, &source);
			pl_tex_destroy(gpu, &target);
			return { static_cast<int>(pixels[0] & 1023u), static_cast<int>((pixels[0] >> 10) & 1023u),
				static_cast<int>((pixels[0] >> 20) & 1023u) };
		}

		pl_gpu GpuHandle() const { return d3d11->gpu; }
		pl_log LogHandle() const { return log; }
		pl_renderer RendererHandle() const { return renderer; }
		pl_log Log() const { return log; }

		// A run of refreshes of one grey (the sigma-delta's state persists in
		// its hooks between them).
		std::vector<int> Sequence(float value, int frames, int colorDepth, const std::vector<const pl_hook*>& hooks,
			enum pl_color_levels levels = PL_COLOR_LEVELS_LIMITED)
		{
			std::vector<int> codes;
			for (int i = 0; i < frames; ++i) codes.push_back(Render(value, colorDepth, hooks, levels).r);
			return codes;
		}
		std::string messages;

	private:
		pl_log log = nullptr;
		pl_d3d11 d3d11 = nullptr;
		pl_renderer renderer = nullptr;
	};

	std::string Describe(const Pixel& p)
	{
		return std::to_string(p.r) + "/" + std::to_string(p.g) + "/" + std::to_string(p.b);
	}
}

void RunBetaGpuTests()
{
	Gpu gpu;
	// 1. The output clamp keeps footroom and headroom, the same on R, G, B.
	auto expect = [](double signal) { return 64.0 + 876.0 * signal; };
	const Pixel below = gpu.Render(static_cast<float>((40.0 - 64.0) / 876.0), 10);
	const Pixel above = gpu.Render(static_cast<float>((980.0 - 64.0) / 876.0), 10);
	const Pixel white = gpu.Render(1.0f, 10);
	std::cout << "Beta GPU: code 40 -> " << Describe(below) << ", code 980 -> " << Describe(above)
		<< ", white -> " << Describe(white) << "\n";
	Check(std::abs(white.r - 940) <= 1, "white is not 940: " + Describe(white));
	Check(below.r == below.g && below.g == below.b && above.r == above.g && above.g == above.b,
		"R, G and B clamped differently (cyan super-whites): " + Describe(below) + ", " + Describe(above));
	Check(std::abs(below.r - expect((40.0 - 64.0) / 876.0)) <= 1.5,
		"below black was clamped (unpatched libplacebo clamps to 64): " + Describe(below));
	Check(std::abs(above.r - expect((980.0 - 64.0) / 876.0)) <= 1.5,
		"above white was clamped (unpatched libplacebo clamps to 940): " + Describe(above));
	const Pixel floorCode = gpu.Render(-1.0f, 10);
	const Pixel ceilingCode = gpu.Render(2.0f, 10);
	Check(floorCode.r == 4 && ceilingCode.r == 1019,
		"the reserved codes are not excluded: " + Describe(floorCode) + ", " + Describe(ceilingCode));
	// VP's swapchain set-up (8-bit precision in 10 bits): footroom kept, on the 8-bit grid.
	const Pixel below8 = gpu.Render(static_cast<float>((40.0 - 64.0) / 876.0), 8);
	Check(below8.r < 64 && below8.r % 4 == 0, "8-in-10: below black not kept on the 8-bit grid: " + Describe(below8));

	// 2. The PRE_OUTPUT bug. The probe scales the picture by 0.25, so the
	// result tells all three cases apart for white on a 10-bit limited target:
	// encoded after the hook (the bug is fixed) 64 + 876 / 4 = 283; encoding
	// dropped (the bug, OutputEncodingRepair.h) 1023 / 4 = 256; the hook did
	// not run at all 940.
	static const char probe[] =
		"//!HOOK PREOUTPUT\n"
		"//!BIND HOOKED\n"
		"//!DESC VP Beta PRE_OUTPUT probe\n"
		"vec4 hook() { vec4 c = HOOKED_tex(HOOKED_pos); return vec4(c.rgb * 0.25, c.a); }\n";
	const pl_hook* hook = pl_mpv_user_shader_parse(gpu.GpuHandle(), probe, sizeof(probe) - 1);
	Check(hook != nullptr, "the probe shader did not parse");
	gpu.messages.clear();
	const Pixel alone = gpu.Render(1.0f, 10, { hook });
	const std::string aloneLog = gpu.messages;
	OutputEncodingRepair::Hook repair;
	pl_color_repr limited{};
	limited.sys = PL_COLOR_SYSTEM_RGB;
	limited.levels = PL_COLOR_LEVELS_LIMITED;
	limited.bits.sample_depth = 10;
	limited.bits.color_depth = 10;
	repair.SetTarget(limited);
	const bool needed = OutputEncodingRepair::Needed({ repair.Get(), hook });
	const Pixel withRepair = gpu.Render(1.0f, 10, { repair.Get(), hook });
	pl_mpv_user_shader_destroy(&hook);
	const bool ran = std::abs(alone.r - 940) > 2;
	const bool encoded = std::abs(alone.r - 283) <= 2;
	std::cout << "Beta GPU: PREOUTPUT probe (x0.25) alone, white -> " << Describe(alone)
		<< (!ran ? " (the hook did not run)" : encoded ? " (encoded after the hook: the bug is fixed)" :
			" (encoding dropped: the bug is present)") << "\n";
	std::cout << "Beta GPU: with VP's repair -> " << Describe(withRepair) << ", Needed() = " << needed << "\n";
	if (aloneLog.find("immutable") != std::string::npos)
		std::cout << "Beta GPU: libplacebo logged the immutable-shader message\n";
	Check(ran, "the PREOUTPUT probe did not run");
	Check(encoded, "libplacebo drops the output encoding after a PREOUTPUT hook (the 7.360 bug): " + Describe(alone));
	// On this libplacebo the encoding is applied after PRE_OUTPUT hooks, so
	// VP's repair must stay out of the chain (it would encode twice).
	Check(!needed, "OutputEncodingRepair::Needed() asks for the repair on a libplacebo without the bug");
}

namespace
{
	std::string LoadRepoShader(const char* fileName)
	{
		std::string path = __FILE__;
		for (int level = 0; level < 3; ++level) path.resize(path.find_last_of("\\/"));
		std::ifstream input(path + "\\shaders\\" + fileName, std::ios::binary);
		return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
	}

	// A //!PARAM by name (what CustomHookChain::SetHookParameter does).
	bool SetParam(const pl_hook* hook, const char* name, double value)
	{
		for (int i = 0; hook && i < hook->num_parameters; ++i)
		{
			const pl_hook_par& par = hook->parameters[i];
			if (std::string(par.name) != name) continue;
			switch (par.type)
			{
			case PL_VAR_FLOAT: par.data->f = static_cast<float>(value); return true;
			case PL_VAR_SINT: par.data->i = static_cast<int>(value); return true;
			case PL_VAR_UINT: par.data->u = static_cast<unsigned>(value); return true;
			default: return false;
			}
		}
		return false;
	}

	// A bundled shader parsed by libplacebo 377 for a 2x2 output, with the
	// given parameters (the hook chain's job in VP).
	struct Shader
	{
		pl_gpu gpu = nullptr;
		const pl_hook* hook = nullptr;
		~Shader() { if (hook) pl_mpv_user_shader_destroy(&hook); }
	};

	std::unique_ptr<Shader> Load(pl_gpu gpu, const char* file, const std::map<std::string, double>& parameters)
	{
		std::string source = LoadRepoShader(file);
		Check(!source.empty(), std::string("bundled shader not found: ") + file);
		for (const char* token : { "{{output_width}}", "{{output_height}}" })
			for (size_t at; (at = source.find(token)) != std::string::npos;)
				source.replace(at, std::string(token).size(), "2");
		auto shader = std::make_unique<Shader>();
		shader->gpu = gpu;
		shader->hook = pl_mpv_user_shader_parse(gpu, source.data(), source.size());
		Check(shader->hook != nullptr, std::string("libplacebo 377 rejected ") + file);
		for (const auto& parameter : parameters)
			Check(SetParam(shader->hook, parameter.first.c_str(), parameter.second),
				std::string(file) + ": no parameter " + parameter.first);
		return shader;
	}

	std::string Codes(const std::vector<int>& codes)
	{
		std::string text;
		for (int code : codes) text += std::to_string(code) + " ";
		return text;
	}

	// Shader-compiler timing notes ("translating HLSL to DXBC (slow!)", a
	// loaded machine) are not failures; anything else is.
	void CheckCleanLog(const Gpu& gpu, const char* test)
	{
		std::string remaining;
		std::istringstream lines(gpu.messages);
		for (std::string entry; std::getline(lines, entry);)
			if (entry.find("translating HLSL to DXBC (slow!)") == std::string::npos)
				remaining += entry + "\n";
		Check(remaining.empty(), std::string(test) + ": libplacebo logged: " + remaining);
	}

	// Aperiodic texture (the stable GPU tests' DejudderTexture), 16-bit codes.
	uint16_t Texture(int x, int y, unsigned salt = 0)
	{
		auto noise = [salt](int cx, int cy, unsigned level) -> unsigned
		{
			const unsigned cell = (static_cast<unsigned>(cx) * 73856093u) ^
				(static_cast<unsigned>(cy) * 19349663u) ^ (salt * 83492791u) ^ (level * 2246822519u);
			return ((cell * 2654435761u) >> 16) % 16000u;
		};
		auto cellOf = [](int v, int size) { return v >= 0 ? v / size : -((-v + size - 1) / size); };
		return static_cast<uint16_t>(8000 + noise(cellOf(x, 4), cellOf(y, 4), 1) +
			noise(cellOf(x, 16), cellOf(y, 16), 2) + noise(cellOf(x, 64), cellOf(y, 64), 3));
	}
}

// The stable GPU tests of our features, on the Beta's libplacebo (audit
// merge-rc4-beta, Should fix): no OutputEncodingRepair here (377 encodes
// after PRE_OUTPUT itself).
void RunBetaFeatureGpuTests()
{
	// 1. Sigma-delta on VP's swapchain set-up (8-bit precision in 10 bits,
	// limited), behind the LED measurement hook at PRE_OUTPUT.
	{
		Gpu gpu;
		const auto sigmaShader = Load(gpu.GpuHandle(), "Temporal sigma-delta.glsl",
			{ { "enabled", 1 }, { "levels_full", 0 }, { "swapchain_bits", 10 }, { "noise_amplitude", 0 } });
		// A texture-input PRE_OUTPUT hook ahead of it, as VP's LED hooks are.
		static const char passThrough[] =
			"//!HOOK PREOUTPUT\n//!BIND HOOKED\n//!DESC VP Beta PREOUTPUT pass-through\n"
			"vec4 hook() { return HOOKED_tex(HOOKED_pos); }\n";
		Shader preOutput;
		preOutput.hook = pl_mpv_user_shader_parse(gpu.GpuHandle(), passThrough, sizeof(passThrough) - 1);
		Check(preOutput.hook != nullptr, "the PREOUTPUT pass-through did not parse");
		const std::vector<const pl_hook*> hooks = { preOutput.hook, sigmaShader->hook };
		gpu.messages.clear();
		for (int code : gpu.Sequence(1.0f, 8, 8, hooks))
			Check(code == 940, "sigma-delta: white is not 940 on the Beta");
		for (int code : gpu.Sequence(0.0f, 8, 8, hooks))
			Check(code == 64, "sigma-delta: black is not 64 on the Beta");
		// libplacebo 377 rounds its own 8-in-10 output to the 8-bit grid (mid grey
		// 125.5 -> 126 = 504), so the reference is the exact value, 8-bit 125.5 =
		// 10-bit 502, which the sigma-delta's average must reproduce.
		const int reference = gpu.Render(0.5f, 8).r;
		const double exact = 64.0 + 876.0 * 0.5;
		const std::vector<int> grey = gpu.Sequence(0.5f, 8, 8, hooks);
		double mean = 0.0;
		for (int code : grey) mean += code / 8.0;
		Check(std::abs(mean - exact) <= 0.6, "sigma-delta: mid grey " + Codes(grey) + "vs exact 502");
		CheckCleanLog(gpu, "sigma-delta 8-in-10");
		std::cout << "Beta features: sigma-delta 8-in-10 white 940, black 64, grey mean " << mean << " (exact 502; libplacebo alone " << reference << ")\n";
	}

	// 2. Sigma-delta on a true 10-bit output (delta 5), both ranges.
	for (const bool full : { false, true })
	{
		Gpu gpu;
		const auto sigmaShader = Load(gpu.GpuHandle(), "Temporal sigma-delta.glsl",
			{ { "enabled", 1 }, { "levels_full", full ? 1 : 0 }, { "swapchain_bits", 10 },
			  { "color_bits", 10 }, { "noise_amplitude", 0 } });
		const pl_hook* sigma = sigmaShader->hook;
		const pl_color_levels levels = full ? PL_COLOR_LEVELS_FULL : PL_COLOR_LEVELS_LIMITED;
		gpu.messages.clear();
		const int white = full ? 1023 : 940, black = full ? 0 : 64;
		for (int code : gpu.Sequence(1.0f, 8, 10, { sigma }, levels))
			Check(code == white, "true 10-bit: white wrong on the Beta");
		for (int code : gpu.Sequence(0.0f, 8, 10, { sigma }, levels))
			Check(code == black, "true 10-bit: black wrong on the Beta");
		float grey = 0.0f;
		int reference = 0;
		for (int target = 501; target <= 507 && grey == 0.0f; ++target)
		{
			const float candidate = full ? target / 1023.0f : (target - 64) / 876.0f;
			const int code = gpu.Render(candidate, 10, {}, levels).r;
			if (code % 4 != 0) { grey = candidate; reference = code; }
		}
		Check(grey > 0.0f, "true 10-bit: no reference grey off the 8-bit grid");
		const std::vector<int> codes = gpu.Sequence(grey, 8, 10, { sigma }, levels);
		for (int code : codes)
			Check(code == reference, "true 10-bit: grey " + std::to_string(reference) + " became " + Codes(codes));
		CheckCleanLog(gpu, "sigma-delta true 10-bit");
		std::cout << "Beta features: sigma-delta true 10-bit " << (full ? "full" : "limited")
			<< ": white " << white << ", black " << black << ", grey " << reference << " held\n";
	}

	// 3. LED compensation (7a): below black survives the boost and a gain of
	// 1.3; light is constant from LED 255 to 100 within 1%, boost on and off.
	{
		Gpu gpu;
		const auto ledShader = Load(gpu.GpuHandle(), "LED compensation.glsl", {});
		const pl_hook* hook = ledShader->hook;
		auto set = [hook](double gain, double boost)
		{
			Check(SetParam(hook, "boost", boost) && SetParam(hook, "gain", gain) &&
				SetParam(hook, "gain_domain", 1.0) && SetParam(hook, "knee", 0.08) &&
				SetParam(hook, "compensation_eotf", 2.41), "LED compensation parameters");
		};
		auto signal = [](double code) { return (code - 64.0) / 876.0; };
		// Read on a true 10-bit target: libplacebo 377 rounds an 8-in-10 output
		// to the 8-bit grid, too coarse for a 1% light comparison.
		auto code = [&gpu, hook](double value) { return gpu.Render(static_cast<float>(value), 10, { hook }).r; };
		auto spow = [](double v, double e) { return (v < 0.0 ? -1.0 : 1.0) * std::pow(std::abs(v), e); };
		const double boost = 2.2 / 2.4;
		gpu.messages.clear();
		set(1.3, boost);
		const double btb = signal(40.0);
		const int btbOut = code(btb);
		const double btbExpected = 64.0 + 876.0 * spow(spow(spow(btb, boost), 2.41) * 1.3, 1.0 / 2.41);
		Check(btbOut < 64, "LED compensation: below black was cut to black on the Beta: " + std::to_string(btbOut));
		Check(std::abs(btbOut - btbExpected) <= 1.5, "LED compensation: below black " + std::to_string(btbOut) +
			", expected " + std::to_string(btbExpected));
		const double grey = (126.0 - 16.0) / 219.0;
		for (const double b : { boost, 1.0 })
		{
			set(1.0, b);
			const double reference = std::pow(signal(code(grey)), 2.41);
			set(255.0 / 100.0, b);
			const double compensated = std::pow(signal(code(grey)), 2.41) * 100.0 / 255.0;
			Check(std::abs(compensated / reference - 1.0) <= 0.01, "LED compensation: light at LED 100 vs 255 off by more than 1%");
		}
		CheckCleanLog(gpu, "LED compensation");
		std::cout << "Beta features: LED compensation code 40 -> " << btbOut << " (expected " << btbExpected
			<< "), light constant LED 255 -> 100\n";
	}

	// 4. Occlusion mask (6c): mask 1 keeps the sigma-delta's carry, mask 0
	// stops it, alpha back to 1.
	{
		Gpu gpu;
		pl_gpu handle = gpu.GpuHandle();
		const auto sigmaShader = Load(handle, "Temporal sigma-delta.glsl",
			{ { "enabled", 1 }, { "levels_full", 0 }, { "swapchain_bits", 10 }, { "noise_amplitude", 0 } });
		const pl_hook* sigma = sigmaShader->hook;
		auto maskTexture = [handle](uint16_t value)
		{
			pl_fmt r16 = pl_find_fmt(handle, PL_FMT_UNORM, 1, 16, 16, PL_FMT_CAP_SAMPLEABLE);
			pl_tex_params params{};
			params.w = params.h = 1;
			params.format = r16;
			params.sampleable = true;
			params.initial_data = &value;
			return pl_tex_create(handle, &params);
		};
		pl_tex seen = maskTexture(65535), fresh = maskTexture(0);
		OcclusionMask::Hook mask;
		const pl_rect2df whole{ 0.0f, 0.0f, 2.0f, 2.0f };
		gpu.messages.clear();
		Check(SetParam(sigma, "occlusion_mask", 1.0), "occlusion_mask parameter");
		mask.SetFrame(seen, whole, whole);
		const std::vector<int> carry = gpu.Sequence(0.5f, 8, 8, { mask.Get(), sigma });
		Check(std::find(carry.begin(), carry.end(), 500) != carry.end() &&
			std::find(carry.begin(), carry.end(), 504) != carry.end(), "occlusion mask 1 must keep the carry: " + Codes(carry));
		mask.SetFrame(fresh, whole, whole);
		const std::vector<int> stopped = gpu.Sequence(0.5f, 8, 8, { mask.Get(), sigma });
		for (int c : stopped)
			Check(c == stopped[0], "occlusion mask 0 must stop the carry: " + Codes(stopped));
		pl_tex_destroy(handle, &seen);
		pl_tex_destroy(handle, &fresh);
		CheckCleanLog(gpu, "occlusion mask");
		std::cout << "Beta features: occlusion mask keeps (" << Codes(carry) << ") / stops (" << Codes(stopped) << ")\n";
	}

	// 5. De-judder matcher (6a): content moved (12, 6) source pixels.
	{
		Gpu gpu;
		pl_gpu handle = gpu.GpuHandle();
		const int width = 256, height = 128, dx = 12, dy = 6;
		auto upload = [handle](int w, int h, int comps, const std::vector<uint16_t>& data)
		{
			const enum pl_fmt_caps caps = static_cast<enum pl_fmt_caps>(
				PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_STORABLE | PL_FMT_CAP_LINEAR | PL_FMT_CAP_HOST_READABLE);
			pl_tex_params params{};
			params.w = w;
			params.h = h;
			params.format = pl_find_fmt(handle, PL_FMT_UNORM, comps, 16, 16, caps);
			params.sampleable = true;
			params.host_readable = true;
			params.initial_data = data.data();
			Check(params.format != nullptr, "de-judder: no storable 16-bit format");
			return pl_tex_create(handle, &params);
		};
		auto frame = [&](int shiftX, int shiftY, Dejudder::Planes& planes)
		{
			std::vector<uint16_t> luma(static_cast<size_t>(width) * height), chroma(static_cast<size_t>(width / 2) * height * 2);
			for (int y = 0; y < height; ++y)
				for (int x = 0; x < width; ++x)
					luma[static_cast<size_t>(y) * width + x] = Texture(x - shiftX, y - shiftY);
			for (int y = 0; y < height; ++y)
				for (int x = 0; x < width / 2; ++x)
					for (int c = 0; c < 2; ++c)
						chroma[(static_cast<size_t>(y) * (width / 2) + x) * 2 + c] = Texture((x - shiftX / 2) * 2, y - shiftY, 7 + c);
			planes.luma = upload(width, height, 1, luma);
			planes.chroma = upload(width / 2, height, 2, chroma);
			planes.chromaShiftX = 1;
			planes.chromaShiftY = 0;
		};
		Dejudder::Planes a, b;
		frame(0, 0, a);
		frame(dx, dy, b);
		Dejudder::Settings settings;
		settings.hostReadable = true;
		gpu.messages.clear();
		Dejudder::MotionWarp warp(gpu.Log(), handle, settings);
		Check(warp.Estimate(a, b), "de-judder: Estimate failed on the Beta: " + warp.LastError());
		const int blocks = warp.BlocksX() * warp.BlocksY();
		std::vector<uint16_t> half(static_cast<size_t>(blocks) * 4);
		pl_tex_transfer_params download{};
		download.tex = warp.Field();
		download.ptr = half.data();
		Check(pl_tex_download(handle, &download), "de-judder: field not readable");
		auto toFloat = [](uint16_t h)
		{
			const int sign = (h >> 15) & 1, exponent = (h >> 10) & 31, mantissa = h & 1023;
			float value = exponent == 0 ? std::ldexp(static_cast<float>(mantissa), -24) :
				exponent == 31 ? NAN : std::ldexp(static_cast<float>(mantissa | 1024), exponent - 25);
			return sign ? -value : value;
		};
		int interior = 0, good = 0;
		for (int by = 2; by < warp.BlocksY() - 2; ++by)
			for (int bx = 2; bx < warp.BlocksX() - 2; ++bx)
			{
				const size_t i = (static_cast<size_t>(by) * warp.BlocksX() + bx) * 4;
				++interior;
				if (toFloat(half[i + 2]) > 0.5f && std::abs(toFloat(half[i]) - dx) < 0.6f &&
					std::abs(toFloat(half[i + 1]) - dy) < 0.6f)
					++good;
			}
		Check(good * 100 >= interior * 90, "de-judder: (12, 6) found in only " + std::to_string(good) + " of " +
			std::to_string(interior) + " blocks on the Beta");
		CheckCleanLog(gpu, "de-judder");
		std::cout << "Beta features: de-judder (12, 6) in " << good << " of " << interior << " blocks\n";

		// Stage 6d on 377, limited 8-bit target (no encoding repair here): the
		// cache leaves A within a code, the rendered hold is A as shown
		// exactly, and a rendered extra matches the source-path extra.
		{
			pl_tex_params tp{};
			tp.w = width;
			tp.h = height;
			tp.format = pl_find_fmt(handle, PL_FMT_UNORM, 4, 8, 8,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE));
			tp.renderable = true;
			tp.host_readable = true;
			Check(tp.format != nullptr, "6d: no RGBA8 target");
			pl_tex targetTex = pl_tex_create(handle, &tp);
			pl_render_params params = pl_render_fast_params;
			params.dither_params = nullptr;
			params.error_diffusion = nullptr;
			auto target = [&]()
			{
				pl_frame t = Gpu::Frame(targetTex);
				t.repr.levels = PL_COLOR_LEVELS_LIMITED;
				t.repr.bits = pl_bit_encoding{};
				t.color.primaries = PL_COLOR_PRIM_BT_709;
				t.color.transfer = PL_COLOR_TRC_BT_1886;
				return t;
			};
			auto readback = [&]()
			{
				pl_gpu_finish(handle);
				std::vector<uint32_t> px(static_cast<size_t>(width) * height);
				pl_tex_transfer_params d{};
				d.tex = targetTex;
				d.ptr = px.data();
				Check(pl_tex_download(handle, &d), "6d: download failed");
				return px;
			};
			auto source = [&](const Dejudder::Planes& planes, const pl_hook* hook)
			{
				pl_frame image{};
				image.num_planes = 2;
				image.planes[0].texture = planes.luma;
				image.planes[0].components = 1;
				image.planes[0].component_mapping[0] = PL_CHANNEL_Y;
				image.planes[1].texture = planes.chroma;
				image.planes[1].components = 2;
				image.planes[1].component_mapping[0] = PL_CHANNEL_CB;
				image.planes[1].component_mapping[1] = PL_CHANNEL_CR;
				image.repr.sys = PL_COLOR_SYSTEM_BT_709;
				image.repr.levels = PL_COLOR_LEVELS_FULL;
				image.color.primaries = PL_COLOR_PRIM_BT_709;
				image.color.transfer = PL_COLOR_TRC_BT_1886;
				pl_frame_set_chroma_location(&image, PL_CHROMA_LEFT);
				pl_frame t = target();
				pl_render_params p = params;
				p.hooks = hook ? &hook : nullptr;
				p.num_hooks = hook ? 1 : 0;
				Check(pl_render_image(gpu.RendererHandle(), &image, &t, &p), "6d: source render failed");
				return readback();
			};
			DejudderRendered::Cache cache;
			DejudderRendered::Warp rendered(gpu.Log(), handle);
			auto fromCache = [&](float shift)
			{
				DejudderRendered::Geometry g;
				g.sourceCrop = { 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height) };
				g.targetCrop = g.sourceCrop;
				pl_tex out = nullptr;
				Check(rendered.Run(cache.Texture(3), warp.Field(), warp.FrameTrust(), settings.minTrustedShare,
					shift, g, out), "6d: rendered warp failed: " + rendered.Error());
				pl_frame image = DejudderRendered::Frame(out, cache.Repr(), cache.Color());
				pl_frame t = target();
				std::vector<const pl_hook*> kept;
				const pl_render_params p = DejudderRendered::OutputOnly(params, {}, kept);
				Check(pl_render_image(gpu.RendererHandle(), &image, &t, &p), "6d: extra render failed");
				return readback();
			};
			auto difference = [](const std::vector<uint32_t>& x, const std::vector<uint32_t>& y, int& maxDiff, double& mean)
			{
				maxDiff = 0;
				double sum = 0.0;
				for (size_t i = 0; i < x.size(); ++i)
					for (int ch = 0; ch < 3; ++ch)
					{
						const int d = std::abs(static_cast<int>((x[i] >> (8 * ch)) & 255u) -
							static_cast<int>((y[i] >> (8 * ch)) & 255u));
						maxDiff = std::max(maxDiff, d);
						sum += d;
					}
				mean = sum / (x.size() * 3.0);
			};
			const auto own = source(a, nullptr);
			cache.Arm(3);
			const auto shown = source(a, cache.Hook());
			Check(cache.Texture(3) != nullptr, "6d: the cache hook kept nothing");
			int maxDiff = 0;
			double mean = 0.0;
			difference(own, shown, maxDiff, mean);
			Check(maxDiff <= 1, "6d: the cache hook changed A by " + std::to_string(maxDiff));
			const auto held = fromCache(0.0f);
			int heldMax = 0;
			difference(shown, held, heldMax, mean);
			Check(heldMax == 0, "6d: the rendered hold is not A (max " + std::to_string(heldMax) + ")");
			Dejudder::Planes warpedPlanes;
			Check(warp.Warp(a, 0.5f, warpedPlanes), "6d: source warp failed");
			const auto bySource = source(warpedPlanes, nullptr);
			const auto byRendered = fromCache(0.5f);
			int extraMax = 0;
			double extraMean = 0.0;
			difference(bySource, byRendered, extraMax, extraMean);
			pl_tex_destroy(handle, &targetTex);
			Check(extraMean <= 0.6, "6d: rendered extra differs from the source extra (mean " + std::to_string(extraMean) + ")");
			CheckCleanLog(gpu, "6d");
			std::cout << "Beta features: 6d cache max " << maxDiff << ", hold max " << heldMax << ", extra vs source mean "
				<< extraMean << " (max " << extraMax << ", whole frame incl. edges)\n";
		}
		pl_tex_destroy(handle, &a.luma);
		pl_tex_destroy(handle, &a.chroma);
		pl_tex_destroy(handle, &b.luma);
		pl_tex_destroy(handle, &b.chroma);
	}
}

// Stage 10 on libplacebo 377: BTB/WTW greys survive 1:1, upscaling and
// downscaling with preserve_extended_range on (the FL35 clamp patch keeps the
// legal 4-1019 at the output); in-range edges and in-gamut colours are as
// without the switch (the native tests' limits).
// Every hook at once on the deployed libplacebo (377, no encoding repair),
// in the renderer's order, over frames with a cut (ChainGpuProbe.h).
void RunBetaChainGpuTest()
{
	Gpu gpu;
	const ChainGpuProbe::Outcome outcome = ChainGpuProbe::Run(gpu.GpuHandle(), gpu.RendererHandle(), gpu.LogHandle(),
		[](const char* file) { return LoadRepoShader(file); }, false);
	std::cout << outcome.report;
	Check(outcome.ok, "whole chain on 377: " + outcome.failure);
	std::string remaining;
	std::istringstream lines(gpu.messages);
	for (std::string entry; std::getline(lines, entry);)
		if (entry.find("translating HLSL to DXBC (slow!)") == std::string::npos)
			remaining += entry + "\n";
	Check(remaining.empty(), "whole chain on 377: libplacebo logged: " + remaining);

	// Motion, a de-judder extra and the LED drive's gain sequence.
	Gpu moving;
	const ChainGpuProbe::Outcome motion = ChainGpuProbe::RunMotion(moving.GpuHandle(), moving.RendererHandle(),
		moving.LogHandle(), [](const char* file) { return LoadRepoShader(file); }, false);
	std::cout << motion.report;
	Check(motion.ok, "whole chain with motion on 377: " + motion.failure);
	remaining.clear();
	std::istringstream movingLines(moving.messages);
	for (std::string entry; std::getline(movingLines, entry);)
		if (entry.find("translating HLSL to DXBC (slow!)") == std::string::npos)
			remaining += entry + "\n";
	Check(remaining.empty(), "whole chain with motion on 377: libplacebo logged: " + remaining);

	// The combination matrix on the deployed libplacebo (planner 2026-10-12:
	// 377 first): a spread by default, all of it with VP_TEST_MATRIX=full.
	{
		char* value = nullptr;
		size_t length = 0;
		const bool full = _dupenv_s(&value, &length, "VP_TEST_MATRIX") == 0 && value && std::string(value) == "full";
		std::free(value);
		int passed = 0;
		for (const ChainGpuProbe::Combo& combo : ChainGpuProbe::MatrixCombos(full))
		{
			Gpu g;
			const ChainGpuProbe::Outcome outcome = ChainGpuProbe::RunMatrix(g.GpuHandle(), g.RendererHandle(),
				g.LogHandle(), [](const char* file) { return LoadRepoShader(file); }, false, combo);
			Check(outcome.ok, "matrix on 377: " + outcome.failure);
			CheckCleanLog(g, "matrix");
			++passed;
		}
		std::cout << "Beta matrix: " << passed << " combinations passed" << std::endl;
	}

	// Upscaling with every hook, FSRCNNX and ArtCNN.
	for (const char* prescaler : { "FSRCNNX_x2_16-0-4-1.glsl", "ArtCNN_C4F16.glsl" })
	{
		Gpu up;
		const ChainGpuProbe::Outcome upscale = ChainGpuProbe::RunUpscale(up.GpuHandle(), up.RendererHandle(),
			up.LogHandle(), [](const char* file) { return LoadRepoShader(file); }, false, prescaler);
		std::cout << upscale.report;
		Check(upscale.ok, std::string("whole chain upscaling on 377: ") + upscale.failure);
		CheckCleanLog(up, "whole chain upscaling");
	}
}

void RunBetaExtendedRangeGpuTests()
{
	using namespace ExtendedRangeGpuProbe;
	Gpu gpu;
	const std::vector<int> codes = { 4, 16, 40, 64, 100, 502, 900, 940, 980, 1019 };
	const Planes greys = Greys(codes);
	std::string report;
	for (double scale : { 1.0, 1.5, 0.5 })
		for (bool owner : { false, true })
		{
			if (owner && scale != 1.5) continue;
			Case c;
			c.scale = scale;
			c.ownerScaling = owner;
			const Result r = Render(gpu.GpuHandle(), gpu.RendererHandle(), greys, c);
			Check(r.ok && r.decision.Active() && r.restores > 0, "extended range: render failed " + r.error);
			report += Join(Columns(r, static_cast<int>(codes.size()), scale)) + "\n";
			for (size_t i = 0; i < codes.size(); ++i)
				for (int ch = 0; ch < 3; ++ch)
					Check(std::abs(r.Channel(static_cast<int>((i * 32 + 16) * scale), r.height / 2, ch) - codes[i]) <= 1,
						"extended range: grey " + std::to_string(codes[i]) + " not kept on 377\n" + report);
		}
	{
		Case c;
		c.scale = 1.5;
		c.outputTransfer = PL_COLOR_TRC_GAMMA24;
		const Result r = Render(gpu.GpuHandle(), gpu.RendererHandle(), greys, c);
		Check(r.ok, "extended range: 2.2->2.4 render failed");
		const auto got = Columns(r, static_cast<int>(codes.size()), c.scale);
		report += Join(got) + " (2.2->2.4 up 1.5)\n";
		for (size_t i = 0; i < codes.size(); ++i)
		{
			const int ideal = Ideal(codes[i], 2.2, 2.4);
			Check(ideal < 4 ? got[i] == 4 : std::abs(got[i] - ideal) <= 1,
				"extended range: 2.2->2.4 grey " + std::to_string(codes[i]) + " on 377\n" + report);
		}
	}
	const Planes edges = Edges();
	for (double scale : { 1.5, 0.5 })
		for (bool owner : { false, true })
		{
			if (owner && scale != 1.5) continue;
			Case on;
			on.scale = scale;
			on.ownerScaling = owner;
			Case off = on;
			off.on = false;
			const Difference d = Compare(Render(gpu.GpuHandle(), gpu.RendererHandle(), edges, off),
				Render(gpu.GpuHandle(), gpu.RendererHandle(), edges, on));
			report += "edges " + std::to_string(scale) + (owner ? " ewa" : "") + ": max " + std::to_string(d.max) +
				", mean " + std::to_string(d.mean) + "\n";
			// 10b: max 1 code; the mean includes libplacebo's half-float store
			// before the texture-form restore (accepted by the planner 2026-10-12).
			Check(d.max <= 1 && d.mean <= 0.1, "extended range: edges differ on 377\n" + report);
		}
	const Planes colours = Patches({ { 502, 512, 512 }, { 560, 440, 600 }, { 400, 450, 700 },
		{ 600, 350, 450 }, { 300, 700, 480 }, { 700, 560, 380 }, { 250, 560, 560 } });
	{
		Case on;
		on.outputTransfer = PL_COLOR_TRC_GAMMA24;
		Case off = on;
		off.on = false;
		const Difference d = Compare(Render(gpu.GpuHandle(), gpu.RendererHandle(), colours, off),
			Render(gpu.GpuHandle(), gpu.RendererHandle(), colours, on));
		Check(d.max <= 2, "extended range: in-gamut colours differ on 377 (max " + std::to_string(d.max) + ")");
	}
	CheckCleanLog(gpu, "extended range");
	std::cout << report;
}
