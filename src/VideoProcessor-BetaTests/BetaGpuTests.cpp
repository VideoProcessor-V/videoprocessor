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
#include "../VideoProcessor-Lib/vprenderer/HookInstanceTag.h"
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

#include <chrono>
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

	// A test fixture beside the native tests (src/VideoProcessor-Test/fixtures).
	std::string LoadFixtureShader(const char* fileName)
	{
		std::string path = __FILE__;
		for (int level = 0; level < 2; ++level) path.resize(path.find_last_of("\\/"));
		std::ifstream input(path + "\\VideoProcessor-Test\\fixtures\\" + fileName, std::ios::binary);
		return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
	}

	std::unique_ptr<Shader> Load(pl_gpu gpu, const char* file, const std::map<std::string, double>& parameters,
		int outputWidth = 2, int outputHeight = 2, bool fixture = false)
	{
		std::string source = fixture ? LoadFixtureShader(file) : LoadRepoShader(file);
		Check(!source.empty(), std::string("bundled shader not found: ") + file);
		for (const auto& token : { std::make_pair("{{output_width}}", outputWidth), std::make_pair("{{output_height}}", outputHeight) })
			for (size_t at; (at = source.find(token.first)) != std::string::npos;)
				source.replace(at, std::string(token.first).size(), std::to_string(token.second));
		auto shader = std::make_unique<Shader>();
		shader->gpu = gpu;
		shader->hook = pl_mpv_user_shader_parse(gpu, source.data(), source.size());
		Check(shader->hook != nullptr, std::string("libplacebo 377 rejected ") + file);
		SetParam(shader->hook, "instance_tag", NextHookInstanceTag());   // as the hook chain (item 7)
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

	// Shader-compiler timing notes ("Spent N ms translating HLSL to DXBC" or "GLSL to SPIR-V (slow!)", a
	// loaded machine) are not failures; anything else is.
	void CheckCleanLog(const Gpu& gpu, const char* test)
	{
		std::string remaining;
		std::istringstream lines(gpu.messages);
		for (std::string entry; std::getline(lines, entry);)
			if ((entry.find("translating HLSL to DXBC (slow!)") == std::string::npos && entry.find("translating GLSL to SPIR-V (slow!)") == std::string::npos))
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
					shift, g, out, warp.SubField(), &warp.GetSettings()), "6d: rendered warp failed: " + rendered.Error());
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
			// Every refinement on (D1-D4; planner addendum: both warp paths): the
			// paths still agree, and the extra is no farther from the true
			// halfway picture than without them (on this pan the scale is 1;
			// vector smoothing promotes held blocks whose own match is good, so
			// a little more of the frame moves).
			warp.SetSettings(ChainGpuProbe::Refined(settings));
			Check(warp.Estimate(a, b), "6d: Estimate with the refinements failed: " + warp.LastError());
			// Delta 9 H2 (hold_hysteresis 2): a block warps only after two trusted
			// source frames running, so the pan is estimated a second time.
			Check(warp.Estimate(a, b), "6d: second Estimate with the refinements failed: " + warp.LastError());
			Dejudder::Planes refinedPlanes;
			Check(warp.Warp(a, 0.5f, refinedPlanes), "6d: source warp with the refinements failed");
			const auto refinedSource = source(refinedPlanes, nullptr);
			const auto refinedRendered = fromCache(0.5f);
			Dejudder::Planes middle;
			frame(dx / 2, dy / 2, middle);
			const auto ideal = source(middle, nullptr);
			pl_tex_destroy(handle, &middle.luma);
			pl_tex_destroy(handle, &middle.chroma);
			int refinedMax = 0, plainIdealMax = 0, refinedIdealMax = 0;
			double refinedMean = 0.0, plainIdeal = 0.0, refinedIdeal = 0.0;
			difference(refinedSource, refinedRendered, refinedMax, refinedMean);
			difference(ideal, bySource, plainIdealMax, plainIdeal);
			difference(ideal, refinedSource, refinedIdealMax, refinedIdeal);
			pl_tex_destroy(handle, &targetTex);
			Check(extraMean <= 0.6, "6d: rendered extra differs from the source extra (mean " + std::to_string(extraMean) + ")");
			Check(refinedMean <= 0.6, "6d with the refinements: rendered extra differs from the source extra (mean " +
				std::to_string(refinedMean) + ")");
			Check(refinedIdeal <= plainIdeal + 0.05, "6d: the refinements moved the extra away from the halfway picture (mean " +
				std::to_string(refinedIdeal) + " vs " + std::to_string(plainIdeal) + " without them)");
			std::cout << "Beta features: 6d with the refinements: rendered vs source mean " << refinedMean
				<< "; vs the halfway picture " << refinedIdeal << " (without them " << plainIdeal << ")\n";
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

// Delta 8 R1 on libplacebo 377 with fork patch 0004: the sigma-delta takes the
// de-judder's vectors on the extras (VPVEC), along a (12, 6) pan, 6d path.
void RunBetaDitherVectorsTests()
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
	Check(warp.Estimate(a, b), "R1: Estimate failed: " + warp.LastError());
	// The parallel block search (one work group per block) against the
	// serial search it replaced: the same field, byte for byte, with the
	// default settings and with every refinement (two estimates each, so E1's
	// previous-vector start and E4's gate take part).
	{
		auto download = [&](pl_tex tex)
		{
			std::vector<uint16_t> half(static_cast<size_t>(tex->params.w) * tex->params.h * 4);
			pl_tex_transfer_params d{};
			d.tex = tex;
			d.ptr = half.data();
			Check(pl_tex_download(handle, &d), "parallel search: field not readable");
			return half;
		};
		for (int refined = 0; refined < 2; ++refined)
		{
			Dejudder::Settings s = refined ? ChainGpuProbe::Refined(settings) : settings;
			s.hostReadable = true;
			Dejudder::MotionWarp parallel(gpu.Log(), handle, s), serial(gpu.Log(), handle, s);
			serial.SetSerialSearchForTest(true);
			for (int pass = 0; pass < 2; ++pass)
			{
				Check(parallel.Estimate(a, b) && serial.Estimate(a, b), "parallel search: estimate failed");
				// D1 in parallel: the same global vector; its match ratio up
				// to the sums' float order.
				{
					std::vector<float> gp(4), gs(4);
					auto global = [&](pl_tex tex, std::vector<float>& out)
					{
						std::vector<uint16_t> h = download(tex);
						for (int c = 0; c < 4; ++c)
						{
							const uint16_t v = h[c];
							const int e = (v >> 10) & 31, m = v & 1023;
							const float mag = e == 0 ? std::ldexp(static_cast<float>(m), -24) :
								std::ldexp(1.0f + m / 1024.0f, e - 15);
							out[c] = (v & 0x8000) ? -mag : mag;
						}
					};
					global(parallel.GlobalVector(), gp);
					global(serial.GlobalVector(), gs);
					Check(gp[0] == gs[0] && gp[1] == gs[1] && std::abs(gp[2] - gs[2]) <= 0.002f * std::max(1.0f, gs[2]),
						"parallel global motion differs from the serial body: (" + std::to_string(gp[0]) + ", " +
							std::to_string(gp[1]) + ", " + std::to_string(gp[2]) + ") vs (" + std::to_string(gs[0]) + ", " +
							std::to_string(gs[1]) + ", " + std::to_string(gs[2]) + ")");
				}
				Check(download(parallel.Field()) == download(serial.Field()),
					std::string("parallel block search differs from the serial search (") +
						(refined ? "refined" : "defaults") + ", estimate " + std::to_string(pass + 1) + ")");
			}
		}
		std::cout << "Beta de-judder: parallel block search byte-identical to the serial search, parallel global motion the same vector (defaults, refined)\n";
	}

	pl_tex_params tp{};
	tp.w = width;
	tp.h = height;
	tp.format = pl_find_fmt(handle, PL_FMT_UNORM, 4, 8, 8,
		static_cast<enum pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE));
	tp.renderable = true;
	tp.host_readable = true;
	pl_tex targetTex = pl_tex_create(handle, &tp);
	Check(targetTex != nullptr, "R1: no RGBA8 target");
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
		Check(pl_tex_download(handle, &d), "R1: download failed");
		return px;
	};
	DejudderRendered::Cache cache;
	auto renderA = [&](const std::vector<const pl_hook*>& extraHooks)
	{
		pl_frame image{};
		image.num_planes = 2;
		image.planes[0].texture = a.luma;
		image.planes[0].components = 1;
		image.planes[0].component_mapping[0] = PL_CHANNEL_Y;
		image.planes[1].texture = a.chroma;
		image.planes[1].components = 2;
		image.planes[1].component_mapping[0] = PL_CHANNEL_CB;
		image.planes[1].component_mapping[1] = PL_CHANNEL_CR;
		image.repr.sys = PL_COLOR_SYSTEM_BT_709;
		image.repr.levels = PL_COLOR_LEVELS_FULL;
		image.color.primaries = PL_COLOR_PRIM_BT_709;
		image.color.transfer = PL_COLOR_TRC_BT_1886;
		pl_frame_set_chroma_location(&image, PL_CHROMA_LEFT);
		std::vector<const pl_hook*> hooks = extraHooks;
		cache.Arm(3);
		hooks.push_back(cache.Hook());
		pl_frame t = target();
		pl_render_params p = params;
		p.hooks = hooks.data();
		p.num_hooks = static_cast<int>(hooks.size());
		Check(pl_render_image(gpu.RendererHandle(), &image, &t, &p), "R1: A render failed");
		return readback();
	};
	DejudderRendered::Geometry g;
	g.sourceCrop = { 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height) };
	g.targetCrop = g.sourceCrop;
	DejudderRendered::Warp rendered(gpu.Log(), handle);
	auto extra = [&](float shift, const pl_hook* sigma)
	{
		pl_tex out = nullptr;
		Check(rendered.Run(cache.Texture(3), warp.Field(), warp.FrameTrust(), settings.minTrustedShare,
			shift, g, out, warp.SubField(), &warp.GetSettings()), "R1: rendered warp failed: " + rendered.Error());
		pl_frame image = DejudderRendered::Frame(out, cache.Repr(), cache.Color());
		pl_frame t = target();
		std::vector<const pl_hook*> kept;
		const pl_render_params p = DejudderRendered::OutputOnly(params, { sigma }, kept);
		Check(pl_render_image(gpu.RendererHandle(), &image, &t, &p), "R1: extra render failed");
		return readback();
	};
	const std::map<std::string, double> sigmaParams = { { "enabled", 1 }, { "levels_full", 0 }, { "swapchain_bits", 8 },
		{ "color_bits", 8 }, { "noise_amplitude", 0 }, { "motion_compensation", 1 } };
	auto loadSigma = [&]() { return Load(handle, "Temporal sigma-delta.glsl", sigmaParams, width, height); };
	DejudderRendered::VectorField field(gpu.Log(), handle, true);
	const float shifts[3] = { 0.25f, 0.5f, 0.75f };

	// The field itself: interior blocks move by the step x (12, 6), trusted.
	{
		pl_tex vectors = nullptr;
		Check(field.Run(warp.Field(), warp.FrameTrust(), settings.minTrustedShare, 0.5f, g, width, height, vectors,
			warp.SubField(), &warp.GetSettings()), "R1: vector field failed: " + field.Error());
		const int bw = DejudderRendered::VectorField::Blocks(width), bh = DejudderRendered::VectorField::Blocks(height);
		Check(vectors && vectors->params.w == bw && vectors->params.h == bh, "R1: vector field size");
		std::vector<uint16_t> half(static_cast<size_t>(bw) * bh * 4);
		pl_tex_transfer_params d{};
		d.tex = vectors;
		d.ptr = half.data();
		Check(pl_tex_download(handle, &d), "R1: vector field not readable");
		auto toFloat = [](uint16_t h)
		{
			const int sign = (h >> 15) & 1, exponent = (h >> 10) & 31, mantissa = h & 1023;
			float value = exponent == 0 ? std::ldexp(static_cast<float>(mantissa), -24) :
				exponent == 31 ? NAN : std::ldexp(static_cast<float>(mantissa | 1024), exponent - 25);
			return sign ? -value : value;
		};
		int interior = 0, good = 0;
		for (int by = 2; by < bh - 2; ++by)
			for (int bx = 2; bx < bw - 2; ++bx)
			{
				const size_t i = (static_cast<size_t>(by) * bw + bx) * 4;
				++interior;
				if (toFloat(half[i + 2]) > 0.5f && std::abs(toFloat(half[i]) - 0.5f * dx) < 0.6f &&
					std::abs(toFloat(half[i + 1]) - 0.5f * dy) < 0.6f)
					++good;
			}
		Check(good * 100 >= interior * 90, "R1: the field has (6, 3) in only " + std::to_string(good) + " of " +
			std::to_string(interior) + " blocks");
		std::cout << "Beta R1: vector field (6, 3) at step 0.5 in " << good << " of " << interior << " blocks\n";
	}

	auto difference = [](const std::vector<uint32_t>& x, const std::vector<uint32_t>& y, int& maxDiff, double& mean)
	{
		maxDiff = 0;
		double sum = 0.0;
		for (size_t i = 0; i < x.size(); ++i)
			for (int ch = 0; ch < 3; ++ch)
			{
				const int d = std::abs(static_cast<int>((x[i] >> (8 * ch)) & 255u) - static_cast<int>((y[i] >> (8 * ch)) & 255u));
				maxDiff = std::max(maxDiff, d);
				sum += d;
			}
		mean = sum / (x.size() * 3.0);
	};

	// Own search (today) along A and three extras.
	std::vector<std::vector<uint32_t>> own;
	{
		auto sigma = loadSigma();
		own.push_back(renderA({ sigma->hook }));
		for (float s : shifts)
			own.push_back(extra(s, sigma->hook));
	}
	// The parallel motion search (one work group per block) against the
	// serial reference it replaced: byte-identical on every refresh.
	{
		auto reference = Load(handle, "Temporal sigma-delta (serial reference).glsl", sigmaParams, width, height, true);
		std::vector<std::vector<uint32_t>> serial;
		serial.push_back(renderA({ reference->hook }));
		for (float s : shifts)
			serial.push_back(extra(s, reference->hook));
		for (size_t i = 0; i < serial.size(); ++i)
		{
			int maxDiff = 0;
			double mean = 0.0;
			difference(serial[i], own[i], maxDiff, mean);
			Check(maxDiff == 0, "parallel motion search differs from the serial reference on refresh " + std::to_string(i) +
				" (max " + std::to_string(maxDiff) + ")");
		}
		std::cout << "Beta sigma-delta: parallel motion search byte-identical to the serial reference on "
			<< serial.size() << " refreshes\n";
	}
	own.erase(own.begin());   // the comparisons below are per extra
	// A cadence repeat (the same picture again) with the search skipped
	// (static_refresh) gives exactly what the search gives.
	{
		auto searched = loadSigma();
		renderA({ searched->hook });
		const auto withSearch = renderA({ searched->hook });
		auto skipped = loadSigma();
		renderA({ skipped->hook });
		Check(SetParam(skipped->hook, "static_refresh", 1.0), "no static_refresh parameter");
		const auto withoutSearch = renderA({ skipped->hook });
		int maxDiff = 0;
		double mean = 0.0;
		difference(withSearch, withoutSearch, maxDiff, mean);
		Check(maxDiff == 0, "static_refresh: a repeat without the search differs from the searched repeat (max " +
			std::to_string(maxDiff) + ")");
		std::cout << "Beta sigma-delta: a cadence repeat without the search is identical to the searched repeat\n";
	}
	// External vectors: the step since the previous present, bound as VPVEC.
	std::vector<std::vector<uint32_t>> external;
	{
		auto sigma = loadSigma();
		renderA({ sigma->hook });
		float previous = 0.0f;
		for (float s : shifts)
		{
			pl_tex vectors = nullptr;
			Check(field.Run(warp.Field(), warp.FrameTrust(), settings.minTrustedShare, s - previous, g, width, height,
				vectors, warp.SubField(), &warp.GetSettings()), "R1: vector field failed: " + field.Error());
			Check(pl_mpv_user_shader_set_texture(sigma->hook, "VPVEC", vectors), "R1: VPVEC not accepted");
			Check(SetParam(sigma->hook, "vector_source", 1.0), "R1: no vector_source parameter");
			external.push_back(extra(s, sigma->hook));
			SetParam(sigma->hook, "vector_source", 0.0);
			Check(pl_mpv_user_shader_set_texture(sigma->hook, "VPVEC", nullptr), "R1: VPVEC not restored");
			previous = s;
		}
	}
	for (size_t i = 0; i < own.size(); ++i)
	{
		int maxDiff = 0;
		double mean = 0.0;
		difference(own[i], external[i], maxDiff, mean);
		// Both quantise the same picture; the carried residual (at most half
		// a code) may differ where the two vectors differ.
		Check(maxDiff <= 1, "R1: the external carry differs from the own carry by " + std::to_string(maxDiff) +
			" codes on extra " + std::to_string(i));
		std::cout << "Beta R1: extra " << i << " (shift " << shifts[i] << ") external vs own: max " << maxDiff
			<< ", mean " << mean << "\n";
	}
	// The own search run again with a fresh hook: how far two identical runs
	// agree on this GPU (the reference for the two path comparisons below).
	int repeatMax = 0;
	{
		auto sigma = loadSigma();
		renderA({ sigma->hook });
		const auto again = extra(shifts[0], sigma->hook);
		double repeatMean = 0.0;
		difference(own[0], again, repeatMax, repeatMean);
		std::cout << "Beta R1: own search repeated with a fresh hook: max " << repeatMax << ", mean " << repeatMean << "\n";
		// Item 7: with per-instance tags a fresh hook never trusts a freed
		// hook's state, so two identical runs agree exactly on every GPU.
		Check(repeatMax == 0, "item 7: two fresh sigma-delta hooks disagree on identical input (max " +
			std::to_string(repeatMax) + ")");
	}
	// vector_source 0 with VPVEC bound is the own search exactly (own =
	// today's output byte for byte).
	{
		auto sigma = loadSigma();
		renderA({ sigma->hook });
		pl_tex vectors = nullptr;
		Check(field.Run(warp.Field(), warp.FrameTrust(), settings.minTrustedShare, shifts[0], g, width, height, vectors,
			warp.SubField(), &warp.GetSettings()), "R1: vector field failed");
		Check(pl_mpv_user_shader_set_texture(sigma->hook, "VPVEC", vectors), "R1: VPVEC not accepted");
		const auto bound = extra(shifts[0], sigma->hook);
		int maxDiff = 0;
		double mean = 0.0;
		difference(own[0], bound, maxDiff, mean);
		Check(maxDiff == 0, "R1: vector_source 0 is not the own search (max " + std::to_string(maxDiff) + ")");
		std::cout << "Beta R1: vector_source 0 with VPVEC bound vs own: max " << maxDiff << ", mean " << mean << "\n";
		// The texture bound at destroy: libplacebo restores its own and leaves
		// VP's alive (no dangling reference).
		const int bw = vectors->params.w;
		sigma.reset();
		std::vector<uint16_t> half(static_cast<size_t>(vectors->params.w) * vectors->params.h * 4);
		pl_tex_transfer_params d{};
		d.tex = vectors;
		d.ptr = half.data();
		Check(vectors->params.w == bw && pl_tex_download(handle, &d),
			"R1: VP's texture did not survive the hook's destruction");
	}
	// The search is not used: an all-untrusted field gives exactly the output
	// of the sigma-delta without motion compensation.
	{
		std::vector<uint16_t> zeros(static_cast<size_t>(DejudderRendered::VectorField::Blocks(width)) *
			DejudderRendered::VectorField::Blocks(height) * 4, 0);
		pl_tex_params zp{};
		zp.w = DejudderRendered::VectorField::Blocks(width);
		zp.h = DejudderRendered::VectorField::Blocks(height);
		zp.format = pl_find_fmt(handle, PL_FMT_FLOAT, 4, 16, 16, PL_FMT_CAP_SAMPLEABLE);
		zp.sampleable = true;
		zp.initial_data = zeros.data();
		pl_tex untrusted = pl_tex_create(handle, &zp);
		Check(untrusted != nullptr, "R1: no rgba16f texture");
		auto sigma = loadSigma();
		renderA({ sigma->hook });
		Check(pl_mpv_user_shader_set_texture(sigma->hook, "VPVEC", untrusted), "R1: VPVEC not accepted");
		Check(!pl_mpv_user_shader_set_texture(sigma->hook, "NO_SUCH_TEXTURE", untrusted), "R1: an unknown name was accepted");
		SetParam(sigma->hook, "vector_source", 1.0);
		const auto skipped = extra(shifts[0], sigma->hook);
		auto plain = loadSigma();
		SetParam(plain->hook, "motion_compensation", 0.0);
		renderA({ plain->hook });
		const auto noSearch = extra(shifts[0], plain->hook);
		int maxDiff = 0;
		double mean = 0.0;
		difference(skipped, noSearch, maxDiff, mean);
		Check(maxDiff == 0, "R1: with external vectors the own search still acted (max " + std::to_string(maxDiff) + ")");
		std::cout << "Beta R1: untrusted external field vs no motion compensation: max " << maxDiff << ", mean " << mean << "\n";
		sigma.reset();
		pl_tex_destroy(handle, &untrusted);
	}
	pl_tex_destroy(handle, &targetTex);
	pl_tex_destroy(handle, &a.luma);
	pl_tex_destroy(handle, &a.chroma);
	pl_tex_destroy(handle, &b.luma);
	pl_tex_destroy(handle, &b.chroma);
	CheckCleanLog(gpu, "R1");
	std::cout << "Beta R1: fresh hooks agree exactly; own search unchanged with VPVEC bound; no dangling texture; untrusted field = no search\n";
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
		if ((entry.find("translating HLSL to DXBC (slow!)") == std::string::npos && entry.find("translating GLSL to SPIR-V (slow!)") == std::string::npos))
			remaining += entry + "\n";
	Check(remaining.empty(), "whole chain on 377: libplacebo logged: " + remaining);

	// Motion, a de-judder extra and the LED drive's gain sequence; as built
	// and with every refinement on (D1-D4, S1).
	for (bool refinements : { false, true })
	{
		Gpu moving;
		const ChainGpuProbe::Outcome motion = ChainGpuProbe::RunMotion(moving.GpuHandle(), moving.RendererHandle(),
			moving.LogHandle(), [](const char* file) { return LoadRepoShader(file); }, false, 63u, refinements);
		std::cout << (refinements ? "-- refinements on\n" : "") << motion.report;
		Check(motion.ok, std::string("whole chain with motion on 377") + (refinements ? " (refinements on)" : "") +
			": " + motion.failure);
		remaining.clear();
		std::istringstream movingLines(moving.messages);
		for (std::string entry; std::getline(movingLines, entry);)
			if ((entry.find("translating HLSL to DXBC (slow!)") == std::string::npos && entry.find("translating GLSL to SPIR-V (slow!)") == std::string::npos))
				remaining += entry + "\n";
		Check(remaining.empty(), "whole chain with motion on 377: libplacebo logged: " + remaining);
	}

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

// S2 (planner 2026-10-12, patch 0003): with output_fbo_float32 the output
// pass (PRE_OUTPUT and OUTPUT hook textures) is rgba32f, earlier stages keep
// the normal format; highlights reach a 16-bit target within 1/16 of a
// 10-bit step; extended codes survive the output pass.
void RunBetaOutputFloat32Tests()
{
#if !defined(PL_RENDER_PARAMS_OUTPUT_FBO_FLOAT32)
	Check(false, "S2: this libplacebo has no output_fbo_float32 (patch 0003 missing)");
#else
	Gpu gpu;
	pl_gpu g = gpu.GpuHandle();
	// Records the texture format each texture-input hook stage sees.
	struct Seen { std::map<int, pl_fmt> formats; };
	Seen seen;
	pl_hook recorder{};
	recorder.stages = static_cast<pl_hook_stage>(PL_HOOK_PRE_OUTPUT | PL_HOOK_OUTPUT | PL_HOOK_SCALED);
	recorder.input = PL_HOOK_SIG_TEX;
	recorder.priv = &seen;
	recorder.hook = [](void* priv, const struct pl_hook_params* params) -> pl_hook_res
	{
		static_cast<Seen*>(priv)->formats[static_cast<int>(params->stage)] = params->tex->params.format;
		return pl_hook_res{};
	};
	const pl_hook* hooks[] = { &recorder };

	pl_fmt sourceFormat = pl_find_fmt(g, PL_FMT_FLOAT, 4, 32, 32, PL_FMT_CAP_SAMPLEABLE);
	pl_fmt targetFormat = nullptr;
	const auto caps = PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE;
	for (int i = 0; i < g->num_formats && !targetFormat; ++i)
	{
		pl_fmt f = g->formats[i];
		if (f->type == PL_FMT_UNORM && f->num_components == 4 && f->component_depth[0] == 16 &&
			f->host_bits[0] == 16 && (f->caps & caps) == caps)
			targetFormat = f;
	}
	Check(sourceFormat && targetFormat, "S2: no float source / RGBA16 target format");

	// Codes 900..1019 and a few extended codes, one per column; 10-bit
	// limited-range signal on a float source, 2x upscaled so a scaled
	// texture exists before the output pass.
	std::vector<double> codes;
	for (int c = 900; c <= 1019; ++c) codes.push_back(c);
	for (int c : { 4, 16, 40, 64, 980 }) codes.push_back(c);
	const int n = static_cast<int>(codes.size());
	std::vector<float> texels(static_cast<size_t>(n) * 4);
	for (int i = 0; i < n; ++i)
	{
		const float v = static_cast<float>((codes[i] - 64.0) / 876.0);
		texels[i * 4] = texels[i * 4 + 1] = texels[i * 4 + 2] = v;
		texels[i * 4 + 3] = 1.0f;
	}
	pl_tex_params sp{};
	sp.w = n;
	sp.h = 1;
	sp.format = sourceFormat;
	sp.sampleable = true;
	sp.initial_data = texels.data();
	pl_tex source = pl_tex_create(g, &sp);
	pl_tex_params tp{};
	tp.w = n * 2;
	tp.h = 2;
	tp.format = targetFormat;
	tp.renderable = true;
	tp.host_readable = true;
	pl_tex target = pl_tex_create(g, &tp);
	Check(source && target, "S2: texture creation failed");

	// mode 0: normal FBOs; 1: output pass rgba32f; 2: every FBO rgba32f.
	auto render = [&](int mode, std::vector<double>& got)
	{
		seen.formats.clear();
		pl_frame image = Gpu::Frame(source);
		pl_frame output = Gpu::Frame(target);
		output.repr.levels = PL_COLOR_LEVELS_LIMITED;
		output.repr.bits.sample_depth = 16;
		output.repr.bits.color_depth = 16;
		pl_render_params params = pl_render_fast_params;
		params.upscaler = &pl_filter_nearest;
		params.dither_params = nullptr;
		params.error_diffusion = nullptr;
		params.hooks = hooks;
		params.num_hooks = 1;
		params.output_fbo_float32 = mode >= 1;
		params.fbo_float32 = mode == 2;
		Check(pl_render_image(gpu.RendererHandle(), &image, &output, &params), "S2: pl_render_image failed");
		pl_gpu_finish(g);
		std::vector<uint16_t> pixels(static_cast<size_t>(tp.w) * tp.h * 4);
		pl_tex_transfer_params download{};
		download.tex = target;
		download.ptr = pixels.data();
		Check(pl_tex_download(g, &download), "S2: download failed");
		got.clear();
		for (int i = 0; i < n; ++i)   // green, row 1, left texel of each column pair
			got.push_back(pixels[(static_cast<size_t>(tp.w) + i * 2) * 4 + 1] / 64.0);   // 10-bit code c is 16-bit c x 64
	};
	std::vector<double> on, off, all;
	render(1, on);
	const std::map<int, pl_fmt> formatsOn = seen.formats;
	render(0, off);
	const std::map<int, pl_fmt> formatsOff = seen.formats;
	render(2, all);
	const std::map<int, pl_fmt> formatsAll = seen.formats;
	pl_tex_destroy(g, &source);
	pl_tex_destroy(g, &target);

	auto name = [](const std::map<int, pl_fmt>& m, pl_hook_stage stage) -> std::string
	{
		const auto it = m.find(static_cast<int>(stage));
		return it == m.end() ? std::string("(not run)") : std::string(it->second->name);
	};
	auto isFloat32 = [](const std::map<int, pl_fmt>& m, pl_hook_stage stage)
	{
		const auto it = m.find(static_cast<int>(stage));
		return it != m.end() && it->second->type == PL_FMT_FLOAT && it->second->component_depth[0] == 32;
	};
	std::cout << "Beta S2: flag on: SCALED " << name(formatsOn, PL_HOOK_SCALED) << ", PRE_OUTPUT "
		<< name(formatsOn, PL_HOOK_PRE_OUTPUT) << ", OUTPUT " << name(formatsOn, PL_HOOK_OUTPUT)
		<< "; flag off: PRE_OUTPUT " << name(formatsOff, PL_HOOK_PRE_OUTPUT) << "; all: SCALED "
		<< name(formatsAll, PL_HOOK_SCALED) << "\n";
	Check(isFloat32(formatsAll, PL_HOOK_SCALED) && isFloat32(formatsAll, PL_HOOK_PRE_OUTPUT),
		"S2: fbo_float32 did not make every FBO rgba32f");
	Check(isFloat32(formatsOn, PL_HOOK_PRE_OUTPUT) && isFloat32(formatsOn, PL_HOOK_OUTPUT),
		"S2: the output pass is not rgba32f with the flag");
	Check(!isFloat32(formatsOn, PL_HOOK_SCALED), "S2: a stage before the output pass changed format");
	Check(!isFloat32(formatsOff, PL_HOOK_PRE_OUTPUT) && !isFloat32(formatsOff, PL_HOOK_OUTPUT),
		"S2: the output pass is rgba32f without the flag");

	double maxOn = 0.0, maxOff = 0.0, maxAll = 0.0;
	for (int i = 0; i < 120; ++i)
	{
		maxOn = (std::max)(maxOn, std::abs(on[i] - codes[i]));
		maxOff = (std::max)(maxOff, std::abs(off[i] - codes[i]));
		maxAll = (std::max)(maxAll, std::abs(all[i] - codes[i]));
	}
	std::cout << "Beta S2: codes 900-1019 max error " << maxAll << " (all rgba32f), " << maxOn
		<< " (output pass rgba32f), " << maxOff << " (normal FBOs) of a 10-bit step; extended (all):";
	for (int i = 120; i < n; ++i)
		std::cout << " " << codes[i] << "->" << std::round(all[i] * 100.0) / 100.0;
	std::cout << "\n";
	// The main pass's float16 FBOs (SCALED) set the floor before the output
	// pass (above white a float16 step is 0.86 code), so only every-FBO
	// float32 reaches 1/16 of a step.
	Check(maxAll < 1.0 / 16.0, "S2: highlight error " + std::to_string(maxAll) + " is not below 1/16 of a step");
	for (int i = 120; i < n; ++i)
		Check(std::abs(all[i] - codes[i]) < 1.0 / 16.0, "S2: extended code " + std::to_string(codes[i]) +
			" changed: " + std::to_string(all[i]));
	CheckCleanLog(gpu, "S2");
#endif
}

// The cost of float32 FBOs (owner question 2026-10-12): 4K to 2560x1600,
// Mitchell in linear light plus a texture-input PRE_OUTPUT hook (as the LED
// hook), wall-clock per frame with the GPU drained, for normal FBOs, the
// output pass in rgba32f (S2 as built) and every FBO in rgba32f. Report
// only; run with VP_TEST_TIMING=1 (meaningful with VP_TEST_GPU=hardware).
void RunBetaFboTiming()
{
#if defined(PL_RENDER_PARAMS_FBO_FLOAT32)
	Gpu gpu;
	pl_gpu g = gpu.GpuHandle();
	const int sw = 3840, sh = 2160, ow = 2560, oh = 1600;
	std::vector<uint16_t> texels(static_cast<size_t>(sw) * sh * 4);
	for (int y = 0; y < sh; ++y)
		for (int x = 0; x < sw; ++x)
		{
			const unsigned h = (static_cast<unsigned>(x) * 73856093u) ^ (static_cast<unsigned>(y) * 19349663u);
			uint16_t* t = &texels[(static_cast<size_t>(y) * sw + x) * 4];
			t[0] = static_cast<uint16_t>(16384 + (h % 32768u));
			t[1] = static_cast<uint16_t>(16384 + ((h >> 8) % 32768u));
			t[2] = static_cast<uint16_t>(16384 + ((h >> 16) % 32768u));
			t[3] = 65535;
		}
	pl_tex_params sp{};
	sp.w = sw;
	sp.h = sh;
	sp.format = pl_find_fmt(g, PL_FMT_UNORM, 4, 16, 16, PL_FMT_CAP_SAMPLEABLE);
	sp.sampleable = true;
	sp.initial_data = texels.data();
	pl_tex source = sp.format ? pl_tex_create(g, &sp) : nullptr;
	pl_tex_params tp{};
	tp.w = ow;
	tp.h = oh;
	tp.format = pl_find_fmt(g, PL_FMT_UNORM, 4, 10, 0, PL_FMT_CAP_RENDERABLE);
	tp.renderable = true;
	pl_tex target = tp.format ? pl_tex_create(g, &tp) : nullptr;
	Check(source && target, "timing: texture creation failed");
	static const char probe[] =
		"//!HOOK PREOUTPUT\n"
		"//!BIND HOOKED\n"
		"//!DESC VP timing PRE_OUTPUT texture hook\n"
		"vec4 hook() { return HOOKED_tex(HOOKED_pos); }\n";
	const pl_hook* hook = pl_mpv_user_shader_parse(g, probe, sizeof(probe) - 1);
	Check(hook != nullptr, "timing: the probe shader did not parse");
	const char* names[3] = { "normal FBOs", "output pass rgba32f (S2)", "every FBO rgba32f" };
	std::cout << "Beta timing (" << sw << "x" << sh << " -> " << ow << "x" << oh << "):";
	for (int mode = 0; mode < 3; ++mode)
	{
		pl_frame image = Gpu::Frame(source);
		image.repr.levels = PL_COLOR_LEVELS_LIMITED;
		image.color.transfer = PL_COLOR_TRC_BT_1886;
		pl_frame output = Gpu::Frame(target);
		output.repr.levels = PL_COLOR_LEVELS_LIMITED;
		output.repr.bits.sample_depth = 10;
		output.repr.bits.color_depth = 10;
		output.color.primaries = PL_COLOR_PRIM_DCI_P3;
		output.color.transfer = PL_COLOR_TRC_GAMMA22;
		pl_render_params params = pl_render_default_params;
		params.downscaler = &pl_filter_mitchell;
		params.hooks = &hook;
		params.num_hooks = 1;
		params.output_fbo_float32 = mode >= 1;
		params.fbo_float32 = mode == 2;
		for (int i = 0; i < 5; ++i)
			Check(pl_render_image(gpu.RendererHandle(), &image, &output, &params), "timing: render failed");
		pl_gpu_finish(g);
		const int frames = 30;
		const auto start = std::chrono::steady_clock::now();
		for (int i = 0; i < frames; ++i)
		{
			Check(pl_render_image(gpu.RendererHandle(), &image, &output, &params), "timing: render failed");
			pl_gpu_finish(g);
		}
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / frames;
		std::cout << " " << names[mode] << " " << ms << " ms;";
	}
	std::cout << std::endl;
	pl_mpv_user_shader_destroy(&hook);
	pl_tex_destroy(g, &source);
	pl_tex_destroy(g, &target);
#endif
}

// Per-pass GPU cost at the cinema's size (owner 2026-10-13: "are the shaders
// efficient?"): 4K source to 2560x1600 with the hooks VP runs - SSIM
// downscaler, SDR tone shape (knee + recovery), LED compensation (gain on),
// temporal sigma-delta (motion carry, 10-bit) - each pass's GPU time from
// libplacebo's pass timers, sorted; then the de-judder matcher on 4K planes
// (all refinements), the rendered warp and the R1 vector field, wall-clock
// with the GPU drained. Report only; VP_TEST_TIMING=1 with VP_TEST_GPU=hardware.
namespace
{
	struct PassTimes
	{
		std::map<std::string, std::pair<double, int>> byName;   // ns sum, samples
	};

	void CollectPass(void* priv, const struct pl_render_info* info)
	{
		if (!priv || !info || !info->pass || !info->pass->shader || info->pass->last == 0)
			return;
		const char* d = info->pass->shader->description;
		auto& slot = static_cast<PassTimes*>(priv)->byName[d ? d : "(unnamed)"];
		slot.first += static_cast<double>(info->pass->last);
		++slot.second;
	}

	double DrainedMs(pl_gpu g, int reps, const std::function<void()>& work)
	{
		work();
		pl_gpu_finish(g);
		const auto start = std::chrono::steady_clock::now();
		for (int i = 0; i < reps; ++i)
		{
			work();
			pl_gpu_finish(g);
		}
		return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / reps;
	}
}

void RunBetaPassTiming()
{
	Gpu gpu;
	pl_gpu g = gpu.GpuHandle();
	const int sw = 3840, sh = 2160, ow = 2560, oh = 1600;
	auto noise = [](int x, int y, unsigned salt)
	{
		const unsigned h = (static_cast<unsigned>(x) * 73856093u) ^ (static_cast<unsigned>(y) * 19349663u) ^ (salt * 83492791u);
		return static_cast<uint16_t>(16384 + (h % 32768u));
	};
	std::vector<uint16_t> texels(static_cast<size_t>(sw) * sh * 4);
	for (int y = 0; y < sh; ++y)
		for (int x = 0; x < sw; ++x)
		{
			uint16_t* t = &texels[(static_cast<size_t>(y) * sw + x) * 4];
			t[0] = noise(x / 8, y / 8, 1);
			t[1] = noise(x / 8, y / 8, 2);
			t[2] = noise(x / 8, y / 8, 3);
			t[3] = 65535;
		}
	pl_tex_params sp{};
	sp.w = sw;
	sp.h = sh;
	sp.format = pl_find_fmt(g, PL_FMT_UNORM, 4, 16, 16, PL_FMT_CAP_SAMPLEABLE);
	sp.sampleable = true;
	sp.initial_data = texels.data();
	pl_tex source = sp.format ? pl_tex_create(g, &sp) : nullptr;
	pl_tex_params tp{};
	tp.w = ow;
	tp.h = oh;
	tp.format = pl_find_fmt(g, PL_FMT_UNORM, 4, 10, 0, PL_FMT_CAP_RENDERABLE);
	tp.renderable = true;
	pl_tex target = tp.format ? pl_tex_create(g, &tp) : nullptr;
	Check(source && target, "pass timing: texture creation failed");
	auto ssim = Load(g, "SSimDownscaler.glsl", {}, ow, oh);
	auto tone = Load(g, "SDR tone shape.glsl", { { "headroom", 0.15 }, { "strength", 0.5 }, { "recovery", 0.3 } }, ow, oh);
	auto led = Load(g, "LED compensation.glsl", { { "gain", 1.1 }, { "gain_domain", 1.0 }, { "compensation_eotf", 2.41 } }, ow, oh);
	auto sigma = Load(g, "Temporal sigma-delta.glsl", { { "enabled", 1 }, { "levels_full", 0 }, { "swapchain_bits", 10 },
		{ "color_bits", 10 }, { "motion_compensation", 1 } }, ow, oh);
	const pl_hook* hooks[4] = { ssim->hook, tone->hook, led->hook, sigma->hook };
	pl_frame image = Gpu::Frame(source);
	image.repr.levels = PL_COLOR_LEVELS_LIMITED;
	image.color.transfer = PL_COLOR_TRC_BT_1886;
	pl_frame output = Gpu::Frame(target);
	output.repr.levels = PL_COLOR_LEVELS_LIMITED;
	output.repr.bits.sample_depth = 10;
	output.repr.bits.color_depth = 10;
	output.color.primaries = PL_COLOR_PRIM_DCI_P3;
	output.color.transfer = PL_COLOR_TRC_GAMMA22;
	pl_render_params params = pl_render_default_params;
	params.downscaler = &pl_filter_mitchell;
	params.hooks = hooks;
	params.num_hooks = 4;
	params.dither_params = nullptr;
#if defined(PL_RENDER_PARAMS_FBO_FLOAT32)
	params.output_fbo_float32 = true;
#endif
	PassTimes times;
	params.info_callback = CollectPass;
	params.info_priv = &times;
	for (int i = 0; i < 10; ++i)
		Check(pl_render_image(gpu.RendererHandle(), &image, &output, &params), "pass timing: render failed");
	pl_gpu_finish(g);
	times.byName.clear();
	const double frameMs = DrainedMs(g, 30, [&] {
		Check(pl_render_image(gpu.RendererHandle(), &image, &output, &params), "pass timing: render failed"); });
	std::vector<std::pair<double, std::string>> sorted;
	double total = 0.0;
	for (const auto& entry : times.byName)
	{
		const double ms = entry.second.first / entry.second.second / 1e6;
		// Passes that run several times per frame (e.g. SSIM's) are summed by
		// their per-frame count.
		const double perFrame = ms * entry.second.second / 31.0;
		sorted.emplace_back(perFrame, entry.first);
		total += perFrame;
	}
	// A cadence repeat: the search skipped (static_refresh).
	{
		PassTimes repeatTimes;
		params.info_priv = &repeatTimes;
		SetParam(sigma->hook, "static_refresh", 1.0);
		for (int i = 0; i < 10; ++i)
			Check(pl_render_image(gpu.RendererHandle(), &image, &output, &params), "pass timing: render failed");
		pl_gpu_finish(g);
		repeatTimes.byName.clear();
		for (int i = 0; i < 20; ++i)
			Check(pl_render_image(gpu.RendererHandle(), &image, &output, &params), "pass timing: render failed");
		pl_gpu_finish(g);
		SetParam(sigma->hook, "static_refresh", 0.0);
		params.info_priv = &times;
		const auto found = repeatTimes.byName.find("VideoProcessor sigma-delta: block motion search");
		if (found != repeatTimes.byName.end() && found->second.second > 0)
			std::cout << "  sigma-delta search on a cadence repeat: " << found->second.first / found->second.second / 1e6 << " ms\n";
	}
	std::sort(sorted.rbegin(), sorted.rend());
	std::cout << "Beta pass timing (" << sw << "x" << sh << " -> " << ow << "x" << oh << ", output pass rgba32f): frame "
		<< frameMs << " ms wall-clock, passes " << total << " ms GPU\n";
	for (const auto& entry : sorted)
		if (entry.first >= 0.01)
			std::cout << "  " << entry.first << " ms  " << entry.second << "\n";

	// The de-judder matcher on 4K planes (every refinement), the rendered
	// warp and the R1 field at the output size.
	const auto caps = static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_STORABLE | PL_FMT_CAP_LINEAR);
	auto plane = [&](int w, int h, int comps, int shift)
	{
		std::vector<uint16_t> data(static_cast<size_t>(w) * h * comps);
		for (int y = 0; y < h; ++y)
			for (int x = 0; x < w; ++x)
				for (int c = 0; c < comps; ++c)
					data[(static_cast<size_t>(y) * w + x) * comps + c] = noise((x - shift) / 4, y / 4, 7 + c);
		pl_tex_params p{};
		p.w = w;
		p.h = h;
		p.format = pl_find_fmt(g, PL_FMT_UNORM, comps, 16, 16, caps);
		p.sampleable = true;
		p.storable = true;
		p.initial_data = data.data();
		return pl_tex_create(g, &p);
	};
	Dejudder::Planes a, b;
	a.luma = plane(sw, sh, 1, 0);
	a.chroma = plane(sw / 2, sh, 2, 0);
	b.luma = plane(sw, sh, 1, 6);
	b.chroma = plane(sw / 2, sh, 2, 3);
	a.chromaShiftX = b.chromaShiftX = 1;
	a.chromaShiftY = b.chromaShiftY = 0;
	Check(a.luma && a.chroma && b.luma && b.chroma, "pass timing: plane upload failed");
	Dejudder::MotionWarp warp(gpu.Log(), g, ChainGpuProbe::Refined(Dejudder::Settings{}));
	int slot = 0;
	const double matcherMs = DrainedMs(g, 10, [&] { Check(warp.Estimate(a, b, slot, true), "pass timing: estimate failed"); slot ^= 1; });
	{
		// Where the matcher's time goes (each pass drained; per estimate).
		warp.SetPassTiming(true);
		const int reps = 5;
		for (int i = 0; i < reps; ++i)
		{
			Check(warp.Estimate(a, b, slot, true), "pass timing: estimate failed");
			slot ^= 1;
		}
		std::vector<std::pair<double, std::string>> passes;
		for (const auto& entry : warp.PassMs())
			passes.emplace_back(entry.second / reps, entry.first);
		warp.SetPassTiming(false);
		std::sort(passes.rbegin(), passes.rend());
		std::cout << "  de-judder matcher passes (4K, every refinement, drained each):\n";
		for (const auto& entry : passes)
			if (entry.first >= 0.05)
				std::cout << "    " << entry.first << " ms  " << entry.second << "\n";
		Dejudder::MotionWarp plain(gpu.Log(), g);
		const double plainMs = DrainedMs(g, 10, [&] { Check(plain.Estimate(a, b, slot, true), "pass timing: estimate failed"); slot ^= 1; });
		std::cout << "  de-judder matcher with default settings " << plainMs << " ms\n";
	}
	pl_tex_params cp{};
	cp.w = ow;
	cp.h = oh;
	cp.format = pl_find_fmt(g, PL_FMT_FLOAT, 4, 16, 16, static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_STORABLE));
	cp.sampleable = true;
	cp.storable = true;
	pl_tex cached = pl_tex_create(g, &cp);
	DejudderRendered::Geometry geometry;
	geometry.sourceCrop = { 0.0f, 0.0f, static_cast<float>(sw), static_cast<float>(sh) };
	geometry.targetCrop = { 0.0f, 0.0f, static_cast<float>(ow), static_cast<float>(oh) };
	DejudderRendered::Warp rendered(gpu.Log(), g);
	DejudderRendered::VectorField field(gpu.Log(), g);
	pl_tex out = nullptr;
	const double warpMs = DrainedMs(g, 20, [&] {
		Check(rendered.Run(cached, warp.Field(), warp.FrameTrust(), warp.GetSettings().minTrustedShare, 0.5f, geometry, out,
			warp.SubField(), &warp.GetSettings()), "pass timing: warp failed"); });
	const double fieldMs = DrainedMs(g, 20, [&] {
		Check(field.Run(warp.Field(), warp.FrameTrust(), warp.GetSettings().minTrustedShare, 0.5f, geometry, ow, oh, out,
			warp.SubField(), &warp.GetSettings()), "pass timing: field failed"); });
	std::cout << "  de-judder matcher (4K, every refinement) " << matcherMs << " ms; rendered warp " << warpMs
		<< " ms; R1 vector field " << fieldMs << " ms (wall-clock, GPU drained)\n";
	for (pl_tex* t : { &a.luma, &a.chroma, &b.luma, &b.chroma, &cached, &source, &target })
		pl_tex_destroy(g, t);
}

// Representative timing (owner 2026-10-13: "sufficient variation to be
// representative"): VP's chain on a moving sequence, as the renderer runs it -
// per new source frame the matcher, then the full render with the owner's
// first-session hooks (adaptive downscale + grain map, SSIM, extended range,
// LED compensation, occlusion mask, sigma-delta with motion carry, the 6d
// cache); per in-between refresh a de-judder extra (rendered warp + the R1
// vectors) or a cadence repeat (search skipped). Sources: a synthetic 24p
// sequence (texture at three scales, edges, fresh grain; still, slow / medium
// / whip pans, an object against a pan, a cut, a fade, a dark scene) at 4K
// and 1080p, and, with VP_TEST_CLIP=<dir of seg_*.yuv, yuv420p10le 4K>, a real
// clip. Only times are reported; frames are uploaded and discarded. Report
// only: VP_TEST_SCENARIO=1 with VP_TEST_GPU=hardware.
namespace
{
	struct Stats
	{
		std::vector<double> v;
		void Add(double x) { v.push_back(x); }
		std::string Text() const
		{
			if (v.empty()) return "-";
			std::vector<double> s(v);
			std::sort(s.begin(), s.end());
			double mean = 0.0;
			for (double x : s) mean += x;
			mean /= s.size();
			const double p95 = s[std::min(s.size() - 1, static_cast<size_t>(s.size() * 0.95))];
			const double p99 = s[std::min(s.size() - 1, static_cast<size_t>(s.size() * 0.99))];
			char text[128];
			std::snprintf(text, sizeof(text), "mean %.2f, p95 %.2f, p99 %.2f, max %.2f ms (n %zu)", mean, p95, p99, s.back(), s.size());
			return text;
		}
	};

	struct Segment
	{
		const char* name;
		int frames;
		float panX, panY;     // source pixels per frame
		float object;         // an object's own speed against the pan (0 = none)
		bool cut;             // jump to another part of the canvas at the start
		float fade;           // 0 = none; else brightness falls to 0 and back over the segment
		float level;          // brightness (dark scene < 1)
	};

	// A detailed canvas (16-bit luma, 4:2:0 chroma) the frames are cut from.
	struct Canvas
	{
		int w = 0, h = 0;
		std::vector<uint16_t> luma, cb, cr;
		void Build(int width, int height)
		{
			w = width; h = height;
			luma.resize(static_cast<size_t>(w) * h);
			cb.resize(static_cast<size_t>(w / 2) * (h / 2));
			cr.resize(cb.size());
			auto hash = [](int x, int y, unsigned salt)
			{
				unsigned v = static_cast<unsigned>(x) * 73856093u ^ static_cast<unsigned>(y) * 19349663u ^ salt * 83492791u;
				v ^= v >> 13; v *= 0x5bd1e995u; v ^= v >> 15;
				return (v & 0xFFFFu) / 65535.0f;
			};
			auto value = [&](float x, float y, float cell, unsigned salt)
			{
				const float fx = x / cell, fy = y / cell;
				const int ix = static_cast<int>(std::floor(fx)), iy = static_cast<int>(std::floor(fy));
				const float tx = fx - ix, ty = fy - iy;
				const float a = hash(ix, iy, salt), b = hash(ix + 1, iy, salt), c = hash(ix, iy + 1, salt), d = hash(ix + 1, iy + 1, salt);
				return (a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * ty;
			};
			for (int y = 0; y < h; ++y)
				for (int x = 0; x < w; ++x)
				{
					float l = 0.45f * value(x, y, 96.0f, 1) + 0.3f * value(x, y, 20.0f, 2) + 0.15f * value(x, y, 4.0f, 3);
					l += 0.1f * static_cast<float>(y) / h;                       // a gradient
					if (((x / 211) + (y / 157)) % 5 == 0) l = 0.9f * l + 0.1f;   // edges: brighter tiles
					if (x % 397 < 3 || y % 283 < 3) l = 0.05f;                   // thin dark lines
					luma[static_cast<size_t>(y) * w + x] = static_cast<uint16_t>(std::min(1.0f, std::max(0.0f, l)) * 60000.0f + 2000.0f);
				}
			for (int y = 0; y < h / 2; ++y)
				for (int x = 0; x < w / 2; ++x)
				{
					cb[static_cast<size_t>(y) * (w / 2) + x] = static_cast<uint16_t>(28000 + 9000 * value(x, y, 60.0f, 4));
					cr[static_cast<size_t>(y) * (w / 2) + x] = static_cast<uint16_t>(28000 + 9000 * value(x, y, 70.0f, 5));
				}
		}
	};
}

// VP_TEST_CHROMA: the chroma upscaler under test (lanczos, catmull_rom,
// bicubic, bilinear); nullptr = unset.
static const pl_filter_config* TestChromaFilter()
{
	char* v = nullptr;
	size_t n = 0;
	std::string name;
	if (_dupenv_s(&v, &n, "VP_TEST_CHROMA") == 0 && v)
		name = v;
	std::free(v);
	if (name == "lanczos") return &pl_filter_lanczos;
	if (name == "catmull_rom") return &pl_filter_catmull_rom;
	if (name == "bicubic") return &pl_filter_bicubic;
	if (name == "bilinear") return &pl_filter_bilinear;
	return nullptr;
}

// The efficiency options' quality (owner 2026-10-13: keep 90-95% of each
// feature's own benefit). On frames of the clip (VP_TEST_CLIP), rendered to a
// 16-bit target without the dither or the LED stage so only scaling differs:
//   reference  plain polar ewa_lanczossharp downscale (no SSIM): the grain as shot
//   ssim       the SSIM downscaler alone
//   polar      adaptive blend, faithful branch polar (today)
//   ortho      adaptive blend, faithful branch separable Lanczos
//   chroma     as polar with chroma_upscaler lanczos
// Faithful benefit = (SSIM's error - the blend's error) against the reference
// in grainy low-edge blocks; ortho's share of polar's. Edges: the same in
// high-edge blocks. Chroma: |chroma - polar| in 10-bit codes. Report only:
// VP_TEST_QUALITY=1.
void RunBetaOptionQuality()
{
	char* dir = nullptr;
	size_t length = 0;
	if (_dupenv_s(&dir, &length, "VP_TEST_CLIP") != 0 || !dir || !*dir)
	{
		std::free(dir);
		std::cout << "Beta option quality: VP_TEST_CLIP not set\n";
		return;
	}
	const std::string folder(dir);
	std::free(dir);
	std::vector<std::string> files;
	WIN32_FIND_DATAA fd{};
	HANDLE find = FindFirstFileA((folder + "\\seg_*.yuv").c_str(), &fd);
	if (find != INVALID_HANDLE_VALUE)
	{
		do files.push_back(folder + "\\" + fd.cFileName); while (FindNextFileA(find, &fd));
		FindClose(find);
	}
	std::sort(files.begin(), files.end());
	Check(!files.empty(), "quality: no clip files");
	Gpu gpu;
	pl_gpu g = gpu.GpuHandle();
	const int w = 3840, h = 2160, ow = 2560, oh = 1600;
	const size_t frameBytes = static_cast<size_t>(w) * h * 3;
	auto plane = [&](int pw, int ph, int comps)
	{
		pl_tex_params p{};
		p.w = pw; p.h = ph;
		p.format = pl_find_fmt(g, PL_FMT_UNORM, comps, 16, 16, static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_LINEAR));
		p.sampleable = true; p.host_writable = true;
		return pl_tex_create(g, &p);
	};
	pl_tex luma = plane(w, h, 1), chroma = plane(w / 2, h / 2, 2);
	pl_tex_params tp{};
	tp.w = ow; tp.h = oh;
	tp.format = pl_find_fmt(g, PL_FMT_UNORM, 4, 16, 16, static_cast<enum pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE));
	tp.renderable = true; tp.host_readable = true;
	pl_tex target = pl_tex_create(g, &tp);
	Check(luma && chroma && target, "quality: textures");
	auto ssim = Load(g, "SSimDownscaler.glsl", {}, ow, oh);
	AdaptiveDownscale::Module modules[2];
	AdaptiveDownscale::Settings settings[2];
	settings[1].faithfulOrtho = true;
	for (auto& s : settings) s.grainMap = false;   // the scalar grain measure (no matcher here)
	const pl_rect2df sourceCrop{ 0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h) };
	enum Variant { Reference, Ssim, Polar, Ortho, Chroma, Count };
	const char* names[Count] = { "reference", "ssim", "polar", "ortho", "chroma" };
	double grainErr[Count] = {}, edgeErr[Count] = {};
	// The feature's own measure: the grain's fine-detail (Laplacian) energy
	// in grainy blocks - SSIM sharpens grain, the blend calms it.
	double grainEnergy[Count] = {};
	int chromaOver8 = 0;
	// Where the larger chroma differences are: samples over 5 codes on strong
	// colour edges (top 10% of the polar output's colour gradient) or not.
	long long over5Edge = 0, over5Flat = 0;
	// VP_TEST_CROPS_DIR: side-by-side crops (polar | chroma filter | 4x the
	// difference) around the frames' worst spots, as PPM files.
	std::string cropsDir;
	{
		char* v = nullptr;
		size_t n = 0;
		if (_dupenv_s(&v, &n, "VP_TEST_CROPS_DIR") == 0 && v)
			cropsDir = v;
		std::free(v);
	}
	int cropsWritten = 0;
	double chromaMax = 0.0;
	int chromaMaxX = 0, chromaMaxY = 0, chromaMaxFrame = 0;
	double chromaSum = 0.0, chromaCount = 0.0;
	std::vector<double> chromaDiffs;
	int grainBlocks = 0, edgeBlocks = 0, frames = 0;
	std::vector<uint16_t> raw(frameBytes / 2), lumaData(static_cast<size_t>(w) * h), chromaData(static_cast<size_t>(w / 2) * (h / 2) * 2);
	std::vector<std::vector<uint16_t>> outs(Count, std::vector<uint16_t>(static_cast<size_t>(ow) * oh * 4));
	const int perFile = 15;   // frames sampled from each 150-frame piece
	for (const auto& file : files)
	{
		std::ifstream in(file, std::ios::binary);
		for (int k = 0; k < perFile; ++k)
		{
			in.seekg(static_cast<std::streamoff>(frameBytes) * (k * 10));
			if (!in.read(reinterpret_cast<char*>(raw.data()), frameBytes))
				break;
			for (size_t i = 0; i < lumaData.size(); ++i) lumaData[i] = static_cast<uint16_t>(raw[i] << 6);
			const uint16_t* u = raw.data() + lumaData.size();
			const uint16_t* v = u + lumaData.size() / 4;
			for (size_t i = 0; i < lumaData.size() / 4; ++i)
			{
				chromaData[i * 2] = static_cast<uint16_t>(u[i] << 6);
				chromaData[i * 2 + 1] = static_cast<uint16_t>(v[i] << 6);
			}
			pl_tex_transfer_params up{};
			up.tex = luma; up.ptr = lumaData.data();
			Check(pl_tex_upload(g, &up), "quality: upload");
			up.tex = chroma; up.ptr = chromaData.data();
			Check(pl_tex_upload(g, &up), "quality: upload");
			++frames;
			for (int var = 0; var < Count; ++var)
			{
				pl_frame image{};
				image.num_planes = 2;
				image.planes[0].texture = luma; image.planes[0].components = 1;
				image.planes[0].component_mapping[0] = PL_CHANNEL_Y;
				image.planes[1].texture = chroma; image.planes[1].components = 2;
				image.planes[1].component_mapping[0] = PL_CHANNEL_CB;
				image.planes[1].component_mapping[1] = PL_CHANNEL_CR;
				image.repr.sys = PL_COLOR_SYSTEM_BT_709;
				image.repr.levels = PL_COLOR_LEVELS_LIMITED;
				image.color.primaries = PL_COLOR_PRIM_BT_709;
				image.color.transfer = PL_COLOR_TRC_GAMMA24;
				pl_frame_set_chroma_location(&image, PL_CHROMA_LEFT);
				pl_frame out = Gpu::Frame(target);
				out.repr.levels = PL_COLOR_LEVELS_FULL;
				out.color.primaries = PL_COLOR_PRIM_BT_709;
				out.color.transfer = PL_COLOR_TRC_GAMMA24;
				pl_render_params params = pl_render_default_params;
				params.dither_params = nullptr;
				params.error_diffusion = nullptr;
				params.deband_params = nullptr;
				params.upscaler = &pl_filter_ewa_lanczossharp;
				params.downscaler = var == Reference ? &pl_filter_ewa_lanczossharp : &pl_filter_mitchell;
				if (var == Chroma)
					params.plane_upscaler = TestChromaFilter() ? TestChromaFilter() : &pl_filter_lanczos;
				std::vector<const pl_hook*> hooks;
				if (var == Polar || var == Ortho || var == Chroma)
				{
					AdaptiveDownscale::Module& m = modules[var == Ortho ? 1 : 0];
					m.SetFrame(settings[var == Ortho ? 1 : 0], static_cast<uint64_t>(frames), 1.0f / 30.0f);
					m.SetSourceDownscales(true);
					hooks = { m.Measure(), ssim->hook, m.Blend() };
				}
				else if (var == Ssim)
					hooks = { ssim->hook };
				params.hooks = hooks.empty() ? nullptr : hooks.data();
				params.num_hooks = static_cast<int>(hooks.size());
				Check(pl_render_image(gpu.RendererHandle(), &image, &out, &params), "quality: render");
				pl_tex_transfer_params d{};
				d.tex = target; d.ptr = outs[var].data();
				Check(pl_tex_download(g, &d), "quality: download");
			}
			// Blocks of the reference: edge strength (gradient) and grain
			// (Laplacian energy) on its luma.
			auto lum = [&](const std::vector<uint16_t>& o, int x, int y)
			{
				const uint16_t* px = &o[(static_cast<size_t>(y) * ow + x) * 4];
				return (0.2126 * px[0] + 0.7152 * px[1] + 0.0722 * px[2]) / 64.0;   // 10-bit codes
			};
			const std::vector<uint16_t>& ref = outs[Reference];
			std::vector<std::pair<double, double>> blockStats;
			for (int by = 1; by < oh / 16 - 1; ++by)
				for (int bx = 1; bx < ow / 16 - 1; ++bx)
				{
					double grad = 0.0, lap = 0.0;
					for (int y = by * 16; y < by * 16 + 16; ++y)
						for (int x = bx * 16; x < bx * 16 + 16; ++x)
						{
							const double c = lum(ref, x, y);
							grad += std::fabs(lum(ref, x + 1, y) - c) + std::fabs(lum(ref, x, y + 1) - c);
							lap += std::fabs(4 * c - lum(ref, x - 1, y) - lum(ref, x + 1, y) - lum(ref, x, y - 1) - lum(ref, x, y + 1));
						}
					blockStats.emplace_back(grad / 256.0, lap / 256.0);
				}
			std::vector<double> grads, laps;
			for (const auto& b : blockStats) { grads.push_back(b.first); laps.push_back(b.second); }
			std::vector<double> sg(grads), sl(laps);
			std::sort(sg.begin(), sg.end());
			std::sort(sl.begin(), sl.end());
			const double gradLow = sg[sg.size() / 3], gradHigh = sg[sg.size() * 9 / 10], lapMid = sl[sl.size() / 2];
			size_t idx = 0;
			for (int by = 1; by < oh / 16 - 1; ++by)
				for (int bx = 1; bx < ow / 16 - 1; ++bx, ++idx)
				{
					const bool grainy = grads[idx] <= gradLow && laps[idx] >= lapMid * 0.5;
					const bool edgy = grads[idx] >= gradHigh;
					if (!grainy && !edgy) continue;
					if (grainy) ++grainBlocks; else ++edgeBlocks;
					if (grainy)
						for (int var = Reference; var < Count; ++var)
						{
							double e = 0.0;
							for (int y = by * 16; y < by * 16 + 16; ++y)
								for (int x = bx * 16; x < bx * 16 + 16; ++x)
									e += std::fabs(4 * lum(outs[var], x, y) - lum(outs[var], x - 1, y) - lum(outs[var], x + 1, y) -
										lum(outs[var], x, y - 1) - lum(outs[var], x, y + 1));
							grainEnergy[var] += e / 256.0;
						}
					for (int var = Ssim; var < Count; ++var)
					{
						double e = 0.0;
						for (int y = by * 16; y < by * 16 + 16; ++y)
							for (int x = bx * 16; x < bx * 16 + 16; ++x)
								e += std::fabs(lum(outs[var], x, y) - lum(ref, x, y));
						(grainy ? grainErr : edgeErr)[var] += e / 256.0;
					}
				}
			// Colour edges of the polar output: the gradient of R-G and B-G.
			{
				std::vector<float> grad(static_cast<size_t>(ow) * oh, 0.0f);
				auto cd = [&](int x, int y, int c)
				{
					const uint16_t* px = &outs[Polar][(static_cast<size_t>(y) * ow + x) * 4];
					return (static_cast<float>(px[c]) - px[1]) / 64.0f;
				};
				for (int y = 1; y < oh - 1; ++y)
					for (int x = 1; x < ow - 1; ++x)
						grad[static_cast<size_t>(y) * ow + x] =
							std::fabs(cd(x + 1, y, 0) - cd(x - 1, y, 0)) + std::fabs(cd(x, y + 1, 0) - cd(x, y - 1, 0)) +
							std::fabs(cd(x + 1, y, 2) - cd(x - 1, y, 2)) + std::fabs(cd(x, y + 1, 2) - cd(x, y - 1, 2));
				std::vector<float> sorted;
				for (size_t i = 0; i < grad.size(); i += 13) sorted.push_back(grad[i]);
				std::sort(sorted.begin(), sorted.end());
				const float edgeLimit = sorted[sorted.size() * 9 / 10];
				int worstX = 0, worstY = 0;
				double worst = 0.0;
				for (int y = 1; y < oh - 1; ++y)
					for (int x = 1; x < ow - 1; ++x)
					{
						const size_t i = (static_cast<size_t>(y) * ow + x) * 4;
						double d = 0.0;
						for (int ch = 0; ch < 3; ++ch)
							d = std::max(d, std::fabs(static_cast<double>(outs[Chroma][i + ch]) - outs[Polar][i + ch]) / 64.0);
						if (d > 5.0)
							(grad[static_cast<size_t>(y) * ow + x] >= edgeLimit ? over5Edge : over5Flat)++;
						if (d > worst) { worst = d; worstX = x; worstY = y; }
					}
				if (!cropsDir.empty() && cropsWritten < 3 && worst > 20.0)
				{
					const int cw = 96, ch = 96, scale = 3;
					const int x0 = std::min(std::max(0, worstX - cw / 2), ow - cw), y0 = std::min(std::max(0, worstY - ch / 2), oh - ch);
					const int W = cw * 3 * scale, H = ch * scale;
					std::vector<unsigned char> img(static_cast<size_t>(W) * H * 3);
					for (int y = 0; y < H; ++y)
						for (int x = 0; x < W; ++x)
						{
							const int panel = x / (cw * scale), sx = x0 + (x % (cw * scale)) / scale, sy = y0 + y / scale;
							const size_t i = (static_cast<size_t>(sy) * ow + sx) * 4;
							for (int c = 0; c < 3; ++c)
							{
								double v;
								if (panel == 0) v = outs[Polar][i + c] / 257.0;
								else if (panel == 1) v = outs[Chroma][i + c] / 257.0;
								else v = 128.0 + 4.0 * (static_cast<double>(outs[Chroma][i + c]) - outs[Polar][i + c]) / 257.0;
								img[(static_cast<size_t>(y) * W + x) * 3 + c] = static_cast<unsigned char>(std::min(255.0, std::max(0.0, v)));
							}
						}
					const std::string name = cropsDir + "\\chroma-crop-" + std::to_string(++cropsWritten) + ".ppm";
					std::ofstream ppm(name, std::ios::binary);
					ppm << "P6\n" << W << " " << H << "\n255\n";
					ppm.write(reinterpret_cast<const char*>(img.data()), static_cast<std::streamsize>(img.size()));
					std::cout << "  crop " << cropsWritten << ": worst " << worst << " codes at (" << worstX << ", " << worstY
						<< ") in sampled frame " << frames << " -> " << name << "\n";
				}
			}
			// Chroma: Lanczos chroma against today's polar chroma (same blend).
			for (size_t i = 0; i < outs[Polar].size(); i += 4 * 7)
				for (int ch = 0; ch < 3; ++ch)
				{
					const double dlt = std::fabs(static_cast<double>(outs[Chroma][i + ch]) - outs[Polar][i + ch]) / 64.0;
					if (dlt > 8.0) ++chromaOver8;
					if (dlt > chromaMax)
					{
						chromaMax = dlt;
						chromaMaxX = static_cast<int>((i / 4) % ow);
						chromaMaxY = static_cast<int>((i / 4) / ow);
						chromaMaxFrame = frames;
					}
					chromaSum += dlt;
					chromaCount += 1.0;
					chromaDiffs.push_back(dlt);
				}
		}
	}
	std::sort(chromaDiffs.begin(), chromaDiffs.end());
	auto mean = [](double sum, int n) { return n > 0 ? sum / n : 0.0; };
	const double gs = mean(grainErr[Ssim], grainBlocks), gp = mean(grainErr[Polar], grainBlocks), go = mean(grainErr[Ortho], grainBlocks);
	const double es = mean(edgeErr[Ssim], edgeBlocks), ep = mean(edgeErr[Polar], edgeBlocks), eo = mean(edgeErr[Ortho], edgeBlocks);
	const double ls = mean(grainEnergy[Ssim], grainBlocks), lp = mean(grainEnergy[Polar], grainBlocks),
		lo = mean(grainEnergy[Ortho], grainBlocks), lr = mean(grainEnergy[Reference], grainBlocks);
	std::cout << "Beta option quality, grain calming (Laplacian energy in grainy blocks, 10-bit codes): plain polar "
		<< lr << ", SSIM alone " << ls << ", blend polar " << lp << ", blend ortho " << lo
		<< "; calming kept by ortho: " << (ls - lp > 1e-9 ? 100.0 * (ls - lo) / (ls - lp) : 0.0) << "%\n"
		<< "  chroma outliers: " << chromaOver8 << " samples over 8 codes of " << static_cast<long long>(chromaCount)
		<< "; worst " << chromaMax << " at (" << chromaMaxX << ", " << chromaMaxY << ") in sampled frame " << chromaMaxFrame
		<< ", p99.9 " << (chromaDiffs.empty() ? 0.0 : chromaDiffs[chromaDiffs.size() * 999 / 1000]) << "\n"
		<< "  pixels over 5 codes: " << over5Edge << " on strong colour edges, " << over5Flat << " elsewhere ("
		<< (over5Edge + over5Flat > 0 ? 100.0 * over5Edge / (over5Edge + over5Flat) : 0.0) << "% on edges)\n";
	std::cout << "Beta option quality (" << frames << " clip frames, 3840x2160 -> 2560x1600, error vs the plain polar downscale in 10-bit codes):\n"
		<< "  grainy low-edge blocks (" << grainBlocks << "): SSIM alone " << gs << ", blend polar " << gp << ", blend ortho " << go << "\n"
		<< "    faithful benefit kept by ortho: " << (gs - gp > 1e-9 ? 100.0 * (gs - go) / (gs - gp) : 0.0) << "%\n"
		<< "  high-edge blocks (" << edgeBlocks << "): SSIM alone " << es << ", blend polar " << ep << ", blend ortho " << eo << "\n"
		<< "  chroma lanczos vs polar (same blend): mean " << (chromaCount > 0 ? chromaSum / chromaCount : 0.0)
		<< ", p99 " << (chromaDiffs.empty() ? 0.0 : chromaDiffs[chromaDiffs.size() * 99 / 100])
		<< ", max " << (chromaDiffs.empty() ? 0.0 : chromaDiffs.back()) << " codes\n";
	pl_tex_destroy(g, &luma);
	pl_tex_destroy(g, &chroma);
	pl_tex_destroy(g, &target);
}

// The de-judder's benefit on real footage (owner 2026-10-13, for judging a
// compromise against 90-95% of the feature's own benefit): in the 29.97p clip
// (VP_TEST_CLIP), frame n+1 is the true halfway picture between n and n+2.
// Per pair (n, n+2), consecutive so the hysteresis and the temporal start
// work as in playback: estimate, warp n by 0.5 (source warp, strength 1) and
// compare its luma with n+1; holding n is the baseline. Benefit = hold error
// - warp error (10-bit codes, interior). Variants: today's refined matcher,
// and with estScreen. Report only: VP_TEST_DEJUDDER_QUALITY=1.
void RunBetaDejudderQuality()
{
	char* dir = nullptr;
	size_t length = 0;
	if (_dupenv_s(&dir, &length, "VP_TEST_CLIP") != 0 || !dir || !*dir)
	{
		std::free(dir);
		std::cout << "Beta de-judder quality: VP_TEST_CLIP not set\n";
		return;
	}
	const std::string folder(dir);
	std::free(dir);
	std::vector<std::string> files;
	WIN32_FIND_DATAA fd{};
	HANDLE find = FindFirstFileA((folder + "\\seg_*.yuv").c_str(), &fd);
	if (find != INVALID_HANDLE_VALUE)
	{
		do files.push_back(folder + "\\" + fd.cFileName); while (FindNextFileA(find, &fd));
		FindClose(find);
	}
	std::sort(files.begin(), files.end());
	Check(!files.empty(), "de-judder quality: no clip files");
	Gpu gpu;
	pl_gpu g = gpu.GpuHandle();
	const int w = 3840, h = 2160;
	const size_t frameBytes = static_cast<size_t>(w) * h * 3;
	auto plane = [&](int pw, int ph, int comps)
	{
		pl_tex_params p{};
		p.w = pw; p.h = ph;
		p.format = pl_find_fmt(g, PL_FMT_UNORM, comps, 16, 16,
			static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_STORABLE | PL_FMT_CAP_LINEAR | PL_FMT_CAP_HOST_READABLE));
		p.sampleable = true; p.storable = true; p.host_writable = true; p.host_readable = true;
		return pl_tex_create(g, &p);
	};
	// mode: 0 = full resolution (today), 1 = the searches at half size,
	// 2 = every pass at half size; rescore: the scaled-back searches re-scored.
	struct Variant { const char* name; int mode; bool rescore; double hold = 0, warp = 0; int pairs = 0; double searchMs = 0;
		double matcherMs = 0; double newShare = 0; int maskSamples = 0; };
	std::vector<Variant> variants = { { "full resolution (today)", 0, false },
		{ "searches at half size", 1, false }, { "every pass at half size", 2, false } };
	if (std::getenv("VP_TEST_ALTERNATE"))   // clock drift check: today first and last
		variants.push_back({ "full resolution (today) again", 0, false });
	std::vector<uint16_t> raw(frameBytes / 2);
	auto load = [&](std::ifstream& in, int index, std::vector<uint16_t>& luma, std::vector<uint16_t>& chroma)
	{
		in.clear();
		in.seekg(static_cast<std::streamoff>(frameBytes) * index);
		if (!in.read(reinterpret_cast<char*>(raw.data()), frameBytes)) return false;
		luma.resize(static_cast<size_t>(w) * h);
		chroma.resize(static_cast<size_t>(w / 2) * (h / 2) * 2);
		for (size_t i = 0; i < luma.size(); ++i) luma[i] = static_cast<uint16_t>(raw[i] << 6);
		const uint16_t* u = raw.data() + luma.size();
		const uint16_t* v = u + luma.size() / 4;
		for (size_t i = 0; i < luma.size() / 4; ++i)
		{
			chroma[i * 2] = static_cast<uint16_t>(u[i] << 6);
			chroma[i * 2 + 1] = static_cast<uint16_t>(v[i] << 6);
		}
		return true;
	};
	for (Variant& var : variants)
	{
		Dejudder::Settings settings = ChainGpuProbe::Refined(Dejudder::Settings{});
		settings.hostReadable = true;
		settings.lowRes = var.mode > 0;
		settings.lowResAll = var.mode == 2;
		Dejudder::MotionWarp warp(gpu.Log(), g, settings);
		warp.SetLowResRescoreForTest(var.rescore);
		// Timing on its own matcher, four estimates back to back per sampled
		// pair with GPU timers and no drains (the GPU kept busy, as in VP).
		Dejudder::MotionWarp timing(gpu.Log(), g, settings);
		timing.SetLowResRescoreForTest(var.rescore);
		timing.SetPassTiming(true, false);
		int timedEstimates = 0;
		double timedWallMs = 0.0;
		pl_tex lumaT[2] = { plane(w, h, 1), plane(w, h, 1) }, chromaT[2] = { plane(w / 2, h / 2, 2), plane(w / 2, h / 2, 2) };
		std::vector<uint16_t> la, ca, lb, cb, lt, ct, warped(static_cast<size_t>(w) * h);
		for (const auto& file : files)
		{
			std::ifstream in(file, std::ios::binary);
			int slot = 0;
			if (!load(in, 0, la, ca)) continue;
			pl_tex_transfer_params up{};
			up.tex = lumaT[slot]; up.ptr = la.data(); Check(pl_tex_upload(g, &up), "upload");
			up.tex = chromaT[slot]; up.ptr = ca.data(); Check(pl_tex_upload(g, &up), "upload");
			for (int n = 0; n + 2 < 150; n += 2)
			{
				if (!load(in, n + 1, lt, ct) || !load(in, n + 2, lb, cb)) break;
				const int next = 1 - slot;
				up.tex = lumaT[next]; up.ptr = lb.data(); Check(pl_tex_upload(g, &up), "upload");
				up.tex = chromaT[next]; up.ptr = cb.data(); Check(pl_tex_upload(g, &up), "upload");
				Dejudder::Planes a, b, out;
				a.luma = lumaT[slot]; a.chroma = chromaT[slot]; b.luma = lumaT[next]; b.chroma = chromaT[next];
				a.chromaShiftX = b.chromaShiftX = 1;
				a.chromaShiftY = b.chromaShiftY = 1;
				const int maskSlot = (n / 2) % 2;
				Check(warp.Estimate(a, b, maskSlot, true), "de-judder quality: estimate: " + warp.LastError());
				// The dither mask: the share of blocks marked new or uncovered.
				if (n >= 6 && warp.Occlusion(maskSlot))
				{
					pl_tex m = warp.Occlusion(maskSlot);
					std::vector<uint16_t> mask(static_cast<size_t>(m->params.w) * m->params.h * 4);
					pl_tex_transfer_params md{};
					md.tex = m; md.ptr = mask.data();
					if (pl_tex_download(g, &md))
					{
						int fresh = 0;
						for (size_t i = 0; i < mask.size(); i += 4)
							fresh += mask[i] < 0x3800 ? 1 : 0;   // half-float r < 0.5
						var.newShare += static_cast<double>(fresh) / (mask.size() / 4);
						++var.maskSamples;
					}
				}
				Check(warp.Warp(a, 0.5f, out, true), "de-judder quality: warp");
				if (n >= 6 && n % 8 == 0)
				{
					pl_gpu_finish(g);
					const auto t0 = std::chrono::steady_clock::now();
					for (int r = 0; r < 4; ++r)
					{
						Check(timing.Estimate(a, b, r % 2, true), "de-judder quality: timing estimate");
						Dejudder::Planes timedOut;
						Check(timing.Warp(a, 0.5f, timedOut, true), "de-judder quality: timing warp");
					}
					pl_gpu_finish(g);
					timedWallMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
					timedEstimates += 4;
				}
				pl_tex_transfer_params d{};
				d.tex = out.luma; d.ptr = warped.data();
				Check(pl_tex_download(g, &d), "de-judder quality: download");
				// The previous frame's luma for the hold baseline is `la`
				// (frame n); the truth is lt (n+1).
				if (n >= 6)   // the hysteresis and the temporal start settled
				{
					double hold = 0.0, wr = 0.0;
					long long count = 0;
					for (int y = 64; y < h - 64; y += 2)
						for (int x = 64; x < w - 64; x += 2)
						{
							const size_t i = static_cast<size_t>(y) * w + x;
							hold += std::abs(static_cast<int>(la[i]) - static_cast<int>(lt[i]));
							wr += std::abs(static_cast<int>(warped[i]) - static_cast<int>(lt[i]));
							++count;
						}
					var.hold += hold / count / 64.0;
					var.warp += wr / count / 64.0;
					++var.pairs;
				}
				la.swap(lb);
				ca.swap(cb);
				slot = next;
			}
		}
		timing.FlushPassTimers();
		const double perEstimate = timedEstimates ? 1.0 / timedEstimates : 0.0;
		var.matcherMs = timedWallMs * perEstimate;   // estimate + warp, busy GPU, wall clock
		for (const auto& entry : timing.PassGpuMs())
		{
			if (entry.first.find("block search") != std::string::npos)
				var.searchMs += entry.second * perEstimate;
			std::cout << "  [" << var.name << "] " << entry.first << ": " << entry.second * perEstimate << " ms GPU\n";
		}
		for (pl_tex* t : { &lumaT[0], &lumaT[1], &chromaT[0], &chromaT[1] })
			pl_tex_destroy(g, t);
	}
	std::cout << "Beta de-judder quality (real clip, warp of n by 0.5 against the true n+1, luma, 10-bit codes):\n";
	const double baseBenefit = variants[0].pairs ? (variants[0].hold - variants[0].warp) / variants[0].pairs : 0.0;
	for (const Variant& var : variants)
	{
		const double hold = var.pairs ? var.hold / var.pairs : 0.0, wr = var.pairs ? var.warp / var.pairs : 0.0;
		const double benefit = hold - wr;
		std::cout << "  " << var.name << ": hold " << hold << ", warp " << wr << ", benefit " << benefit
			<< " (" << (baseBenefit > 1e-9 ? 100.0 * benefit / baseBenefit : 0.0) << "% of today's), block search "
			<< var.searchMs << " ms GPU, estimate + warp " << var.matcherMs << " ms (busy GPU, wall); dither mask marks "
			<< (var.maskSamples ? 100.0 * var.newShare / var.maskSamples : 0.0) << "% of blocks new\n";
	}
}

// The dither with each matcher mode (owner 2026-10-13: how much of the
// motion's help to the sigma-delta survives the low-resolution matcher).
// The real clip as 23.976p at 59.94 (3:2 with de-judder extras, VPVEC),
// 4K -> 2560x1600, the occlusion mask and the sigma-delta only. Each refresh
// is rendered twice from the same input: with the sigma-delta (16-bit target)
// and without it (32-bit float, the exact value). The visible error is the
// dither error averaged over each source frame's refreshes (what the eye
// integrates) and over 4x4 pixels; "off" = motion compensation and the mask
// off. Report only: VP_TEST_DITHER_COMPARE=1 (VP_TEST_DITHER_FRAMES per
// segment, default 48).
void RunBetaDitherCompare()
{
	char* dir = nullptr;
	size_t length = 0;
	if (_dupenv_s(&dir, &length, "VP_TEST_CLIP") != 0 || !dir || !*dir)
	{
		std::free(dir);
		std::cout << "Beta dither compare: VP_TEST_CLIP not set\n";
		return;
	}
	const std::string folder(dir);
	std::free(dir);
	int perSegment = 48;
	{
		char* v = nullptr;
		size_t n = 0;
		if (_dupenv_s(&v, &n, "VP_TEST_DITHER_FRAMES") == 0 && v && *v)
			perSegment = std::max(8, std::atoi(v));
		std::free(v);
	}
	std::vector<std::string> files;
	WIN32_FIND_DATAA fd{};
	HANDLE find = FindFirstFileA((folder + "\\seg_*.yuv").c_str(), &fd);
	if (find != INVALID_HANDLE_VALUE)
	{
		do files.push_back(folder + "\\" + fd.cFileName); while (FindNextFileA(find, &fd));
		FindClose(find);
	}
	std::sort(files.begin(), files.end());
	Check(!files.empty(), "dither compare: no clip files");
	Gpu gpu;
	pl_gpu g = gpu.GpuHandle();
	const int w = 3840, h = 2160, ow = 2560, oh = 1600;
	const size_t frameBytes = static_cast<size_t>(w) * h * 3;
	auto plane = [&](int pw, int ph, int comps)
	{
		pl_tex_params tp{};
		tp.w = pw; tp.h = ph;
		tp.format = pl_find_fmt(g, PL_FMT_UNORM, comps, 16, 16,
			static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_STORABLE | PL_FMT_CAP_LINEAR));
		tp.sampleable = true; tp.storable = true; tp.host_writable = true;
		return pl_tex_create(g, &tp);
	};
	auto target = [&](enum pl_fmt_type type, int depth)
	{
		pl_tex_params tp{};
		tp.w = ow; tp.h = oh;
		tp.format = pl_find_fmt(g, type, 4, depth, depth,
			static_cast<enum pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE));
		tp.renderable = true; tp.host_readable = true;
		return pl_tex_create(g, &tp);
	};
	pl_tex dithered = target(PL_FMT_UNORM, 16), exact = target(PL_FMT_FLOAT, 32);
	Check(dithered && exact, "dither compare: no targets");
	std::vector<uint16_t> raw(frameBytes / 2), lumaData, chromaData;
	auto load = [&](std::ifstream& in, int index)
	{
		in.clear();
		in.seekg(static_cast<std::streamoff>(frameBytes) * index);
		if (!in.read(reinterpret_cast<char*>(raw.data()), frameBytes)) return false;
		lumaData.resize(static_cast<size_t>(w) * h);
		chromaData.resize(static_cast<size_t>(w / 2) * (h / 2) * 2);
		for (size_t i = 0; i < lumaData.size(); ++i) lumaData[i] = static_cast<uint16_t>(raw[i] << 6);
		const uint16_t* u = raw.data() + lumaData.size();
		const uint16_t* v = u + lumaData.size() / 4;
		for (size_t i = 0; i < lumaData.size() / 4; ++i)
		{
			chromaData[i * 2] = static_cast<uint16_t>(u[i] << 6);
			chromaData[i * 2 + 1] = static_cast<uint16_t>(v[i] << 6);
		}
		return true;
	};
	struct Mode { const char* name; int lowRes; bool motion; bool rescore; double visible = 0, temporal = 0, rms = 0; int groups = 0, refreshes = 0; };
	std::vector<Mode> modes = { { "full-resolution motion (today)", 0, true, false }, { "searches at half size", 1, true, false },
		{ "every pass at half size", 2, true, false }, { "every pass at half size, mask re-scored", 2, true, true },
		{ "motion compensation and mask off", 0, false, false } };
	const size_t pixels = static_cast<size_t>(ow) * oh;
	std::vector<uint16_t> outPx(pixels * 4);
	std::vector<float> exactPx(pixels * 4), groupSum(pixels * 3);
	for (Mode& mode : modes)
	{
		pl_tex luma[2] = { plane(w, h, 1), plane(w, h, 1) }, chroma[2] = { plane(w / 2, h / 2, 2), plane(w / 2, h / 2, 2) };
		Dejudder::Settings settings = ChainGpuProbe::Refined(Dejudder::Settings{});
		settings.lowRes = mode.lowRes > 0;
		settings.lowResAll = mode.lowRes == 2;
		Dejudder::MotionWarp warp(gpu.Log(), g, settings);
		warp.SetLowResRescoreForTest(mode.rescore);
		OcclusionMask::Hook mask;
		DejudderRendered::Cache cache;
		DejudderRendered::Warp rendered(gpu.Log(), g);
		DejudderRendered::VectorField field(gpu.Log(), g);
		auto sigma = Load(g, "Temporal sigma-delta.glsl", { { "enabled", 1 }, { "levels_full", 0 }, { "swapchain_bits", 10 },
			{ "color_bits", 10 }, { "motion_compensation", mode.motion ? 1.0 : 0.0 }, { "occlusion_mask", mode.motion ? 1.0 : 0.0 },
			{ "grain_tolerance_scale", 3 } }, ow, oh);
		const pl_rect2df sourceCrop{ 0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h) };
		const pl_rect2df targetCrop{ 0.0f, 0.0f, static_cast<float>(ow), static_cast<float>(oh) };
		uint64_t sequence = 0;
		for (const auto& file : files)
		{
			std::ifstream in(file, std::ios::binary);
			double phase = 0.0;
			for (int f = 0; f < perSegment; ++f)
			{
				if (!load(in, f))
					break;
				const int cur = f % 2, prev = 1 - cur;
				pl_tex_transfer_params up{};
				up.tex = luma[cur]; up.ptr = lumaData.data();
				Check(pl_tex_upload(g, &up), "dither compare: upload");
				up.tex = chroma[cur]; up.ptr = chromaData.data();
				Check(pl_tex_upload(g, &up), "dither compare: upload");
				if (f == 0)
					continue;
				++sequence;
				Dejudder::Planes a, b;
				a.luma = luma[prev]; a.chroma = chroma[prev]; b.luma = luma[cur]; b.chroma = chroma[cur];
				a.chromaShiftX = b.chromaShiftX = 1;
				a.chromaShiftY = b.chromaShiftY = 1;
				Check(warp.Estimate(a, b, f % 2, false), "dither compare: estimate: " + warp.LastError());
				mask.SetFrame(mode.motion ? warp.Occlusion(f % 2) : nullptr, sourceCrop, targetCrop);
				phase += 2.5;
				const int refreshes = static_cast<int>(std::floor(phase + 1e-9));
				phase -= refreshes;
				std::fill(groupSum.begin(), groupSum.end(), 0.0f);
				float previousShift = 0.0f;
				for (int r = 0; r < refreshes; ++r)
				{
					pl_frame image{};
					auto source = [&]()
					{
						image = pl_frame{};
						image.num_planes = 2;
						image.planes[0].texture = b.luma; image.planes[0].components = 1;
						image.planes[0].component_mapping[0] = PL_CHANNEL_Y;
						image.planes[1].texture = b.chroma; image.planes[1].components = 2;
						image.planes[1].component_mapping[0] = PL_CHANNEL_CB;
						image.planes[1].component_mapping[1] = PL_CHANNEL_CR;
						image.repr.sys = PL_COLOR_SYSTEM_BT_709;
						image.repr.levels = PL_COLOR_LEVELS_LIMITED;
						image.color.primaries = PL_COLOR_PRIM_BT_709;
						image.color.transfer = PL_COLOR_TRC_GAMMA24;
						pl_frame_set_chroma_location(&image, PL_CHROMA_LEFT);
					};
					auto out = [&](pl_tex tex)
					{
						pl_frame o{};
						o.num_planes = 1;
						o.planes[0].texture = tex;
						o.planes[0].components = 3;
						for (int i = 0; i < 3; ++i) o.planes[0].component_mapping[i] = i;
						o.repr.sys = PL_COLOR_SYSTEM_RGB;
						o.repr.levels = PL_COLOR_LEVELS_LIMITED;
						o.repr.bits.sample_depth = 10; o.repr.bits.color_depth = 10;
						o.color.primaries = PL_COLOR_PRIM_DCI_P3;
						o.color.transfer = PL_COLOR_TRC_GAMMA22;
						return o;
					};
					pl_render_params params = pl_render_default_params;
					params.dither_params = nullptr;
					params.error_diffusion = nullptr;
					params.downscaler = &pl_filter_mitchell;
					params.upscaler = &pl_filter_ewa_lanczossharp;
#if defined(PL_RENDER_PARAMS_OUTPUT_FBO_FLOAT32)
					params.output_fbo_float32 = true;
#endif
					std::vector<const pl_hook*> list, kept, keptExact;
					pl_render_params exactParams = params;
					pl_tex vectors = nullptr;
					const bool isExtra = r > 0 && cache.Texture(sequence) != nullptr;
					if (r == 0)
					{
						source();
						list.push_back(mask.Get());
						list.push_back(sigma->hook);
						cache.Arm(sequence);
						list.push_back(cache.Hook());
						SetParam(sigma->hook, "static_refresh", 0.0);
						params.hooks = list.data();
						params.num_hooks = static_cast<int>(list.size());
						exactParams.hooks = nullptr;
						exactParams.num_hooks = 0;
					}
					else if (isExtra)
					{
						const float shift = 0.25f * static_cast<float>(r) / 2.5f;
						pl_tex warped = nullptr;
						Check(rendered.Run(cache.Texture(sequence), warp.Field(), warp.FrameTrust(),
							warp.GetSettings().minTrustedShare, shift, { sourceCrop, targetCrop, {}, {} }, warped, warp.SubField(),
							&warp.GetSettings()), "dither compare: warp");
						if (mode.motion)
						{
							Check(field.Run(warp.Field(), warp.FrameTrust(), warp.GetSettings().minTrustedShare, shift - previousShift,
								{ sourceCrop, targetCrop, {}, {} }, ow, oh, vectors, warp.SubField(), &warp.GetSettings()), "dither compare: field");
							pl_mpv_user_shader_set_texture(sigma->hook, "VPVEC", vectors);
							SetParam(sigma->hook, "vector_source", 1.0);
						}
						previousShift = shift;
						image = DejudderRendered::Frame(warped, cache.Repr(), cache.Color());
						const pl_render_params base = params;
						params = DejudderRendered::OutputOnly(base, { sigma->hook }, kept);
						exactParams = DejudderRendered::OutputOnly(base, {}, keptExact);
					}
					else
					{
						source();
						list.push_back(sigma->hook);
						SetParam(sigma->hook, "static_refresh", 1.0);
						params.hooks = list.data();
						params.num_hooks = static_cast<int>(list.size());
						exactParams.hooks = nullptr;
						exactParams.num_hooks = 0;
					}
					pl_frame o = out(dithered);
					Check(pl_render_image(gpu.RendererHandle(), &image, &o, &params), "dither compare: render");
					if (isExtra && mode.motion)
					{
						SetParam(sigma->hook, "vector_source", 0.0);
						pl_mpv_user_shader_set_texture(sigma->hook, "VPVEC", nullptr);
					}
					SetParam(sigma->hook, "static_refresh", 0.0);
					pl_frame e = out(exact);
					Check(pl_render_image(gpu.RendererHandle(), &image, &e, &exactParams), "dither compare: exact render");
					pl_tex_transfer_params d{};
					d.tex = dithered; d.ptr = outPx.data();
					Check(pl_tex_download(g, &d), "dither compare: download");
					d.tex = exact; d.ptr = exactPx.data();
					Check(pl_tex_download(g, &d), "dither compare: download");
					if (f < 6)
						continue;   // the sigma-delta and the hysteresis settled
					double square = 0.0;
					for (size_t i = 0; i < pixels; ++i)
						for (int c = 0; c < 3; ++c)
						{
							const float err = outPx[i * 4 + c] * (1023.0f / 65535.0f) - exactPx[i * 4 + c] * 1023.0f;
							groupSum[i * 3 + c] += err;
							square += static_cast<double>(err) * err;
						}
					mode.rms += square / (pixels * 3.0);
					++mode.refreshes;
				}
				if (f < 6)
					continue;
				// The eye's integration: the group mean per pixel, then 4x4.
				double temporal = 0.0, visible = 0.0;
				for (size_t i = 0; i < pixels * 3; ++i)
				{
					const double m = groupSum[i] / refreshes;
					temporal += m * m;
				}
				for (int y = 0; y + 4 <= oh; y += 4)
					for (int x = 0; x + 4 <= ow; x += 4)
						for (int c = 0; c < 3; ++c)
						{
							double sum = 0.0;
							for (int yy = 0; yy < 4; ++yy)
								for (int xx = 0; xx < 4; ++xx)
									sum += groupSum[(static_cast<size_t>(y + yy) * ow + x + xx) * 3 + c];
							const double m = sum / (16.0 * refreshes);
							visible += m * m;
						}
				mode.temporal += temporal / (pixels * 3.0);
				mode.visible += visible / ((ow / 4) * (oh / 4) * 3.0);
				++mode.groups;
			}
		}
		for (pl_tex* t : { &luma[0], &luma[1], &chroma[0], &chroma[1] })
			pl_tex_destroy(g, t);
	}
	std::cout << "Beta dither compare (real clip as 3:2 with extras, 2560x1600, 10-bit codes; RMS of the dither error):\n";
	const double full = modes[0].groups ? std::sqrt(modes[0].visible / modes[0].groups) : 0.0;
	const double off = modes.back().groups ? std::sqrt(modes.back().visible / modes.back().groups) : 0.0;
	for (const Mode& mode : modes)
	{
		const double visible = mode.groups ? std::sqrt(mode.visible / mode.groups) : 0.0;
		const double temporal = mode.groups ? std::sqrt(mode.temporal / mode.groups) : 0.0;
		const double rms = mode.refreshes ? std::sqrt(mode.rms / mode.refreshes) : 0.0;
		std::cout << "  " << mode.name << ": per refresh " << rms << ", per source frame " << temporal << ", visible (4x4) " << visible;
		if (off - full > 1e-6 && mode.motion)
			std::cout << " (motion's help kept: " << 100.0 * (off - visible) / (off - full) << "%)";
		std::cout << "\n";
	}
}

void RunBetaScenarioTiming()
{
	Gpu gpu;
	pl_gpu g = gpu.GpuHandle();
	const int ow = 2560, oh = 1600;
	auto plane = [&](int w, int h, int comps)
	{
		pl_tex_params p{};
		p.w = w; p.h = h;
		p.format = pl_find_fmt(g, PL_FMT_UNORM, comps, 16, 16,
			static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_STORABLE | PL_FMT_CAP_LINEAR));
		p.sampleable = true; p.storable = true; p.host_writable = true;
		return pl_tex_create(g, &p);
	};
	pl_tex_params tp{};
	tp.w = ow; tp.h = oh;
	tp.format = pl_find_fmt(g, PL_FMT_UNORM, 4, 10, 0, PL_FMT_CAP_RENDERABLE);
	tp.renderable = true;
	pl_tex targetTex = pl_tex_create(g, &tp);
	Check(targetTex != nullptr, "scenario: no target");

	struct Source
	{
		const char* label;
		int w, h;
		double sourceRate;   // frames per second
		std::function<bool(int index, std::vector<uint16_t>& luma, std::vector<uint16_t>& chroma, std::string& segment)> next;
		int frames;
		bool hdr = false;
	};
	std::vector<Source> sources;
	auto envOn = [](const char* name)
	{
		char* value = nullptr;
		size_t length = 0;
		const bool on = _dupenv_s(&value, &length, name) == 0 && value && std::string(value) == "1";
		std::free(value);
		return on;
	};
	// VP_TEST_CLIP_HDR=1: the clip is PQ / BT.2020 (tone-mapped to the target).
	const bool clipHdr = envOn("VP_TEST_CLIP_HDR");
	const bool clipOnly = envOn("VP_TEST_SCENARIO_CLIP_ONLY");
	// VP_TEST_MATCHER_PASSES=1: the matcher's passes timed (each drained).
	const bool matcherPasses = envOn("VP_TEST_MATCHER_PASSES");
	// The efficiency options under test: VP_TEST_CHROMA=lanczos (chroma_upscaler)
	// and VP_TEST_ADAPTIVE_ORTHO=1 (adaptive_faithful ortho).
	const bool chromaLanczos = [] { char* v = nullptr; size_t n = 0; const bool on = _dupenv_s(&v, &n, "VP_TEST_CHROMA") == 0 && v &&
		std::string(v) != ""; std::free(v); return on; }();
	const bool adaptiveOrtho = envOn("VP_TEST_ADAPTIVE_ORTHO");
	// VP_TEST_AHEAD=1: VP's estimate-ahead order (owner 2026-10-13): the new
	// frame's slot only its render; the matcher right after the first extra,
	// in that extra's slot. The same GPU work as without, in that order.
	const bool aheadOrder = envOn("VP_TEST_AHEAD");
	// The synthetic sequence at 4K and 1080p.
	const std::vector<Segment> segments = {
		{ "still", 12, 0, 0, 0, false, 0, 1 },
		{ "slow pan (1 px)", 24, 1, 0, 0, false, 0, 1 },
		{ "medium pan (6 px)", 24, 6, 1, 0, false, 0, 1 },
		{ "whip pan (40 px)", 12, 40, 0, 0, false, 0, 1 },
		{ "object against a pan", 24, 3, 0, -10, false, 0, 1 },
		{ "cut", 6, 2, 0, 0, true, 0, 1 },
		{ "fade out and in", 24, 1, 0, 0, false, 1, 1 },
		{ "dark scene", 12, 2, 1, 0, false, 0, 0.12f },
	};
	int total = 0;
	for (const auto& s : segments) total += s.frames;
	auto canvas = std::make_shared<Canvas>();
	for (int big = 1; big >= 0 && !clipOnly; --big)
	{
		const int w = big ? 3840 : 1920, h = big ? 2160 : 1080;
		Source src;
		src.label = big ? "synthetic 23.976p, 3840x2160" : "synthetic 23.976p, 1920x1080";
		src.w = w; src.h = h; src.sourceRate = 24000.0 / 1001.0; src.frames = total;
		src.next = [segments, canvas, w, h](int index, std::vector<uint16_t>& luma, std::vector<uint16_t>& chroma, std::string& segment)
		{
			if (canvas->w != w + 1200)
				canvas->Build(w + 1200, h + 200);
			// Position: the sum of the pans so far; a cut jumps.
			int seg = 0, k = index;
			double px = 0.0, py = 0.0;
			for (int i = 0; i < static_cast<int>(segments.size()); ++i)
			{
				const int n = std::min(k, segments[i].frames);
				if (segments[i].cut && k > 0) { px += 300; py += 60; }
				px += segments[i].panX * n; py += segments[i].panY * n;
				if (k < segments[i].frames) { seg = i; break; }
				k -= segments[i].frames;
			}
			const Segment& s = segments[seg];
			segment = s.name;
			const int ox = static_cast<int>(px) % 1100, oy = static_cast<int>(py) % 180;
			float gain = s.level;
			if (s.fade > 0.0f)
				gain *= std::fabs(1.0f - 2.0f * k / std::max(1, s.frames - 1));
			luma.resize(static_cast<size_t>(w) * h);
			chroma.resize(static_cast<size_t>(w / 2) * (h / 2) * 2);
			const int objX = s.object != 0.0f ? static_cast<int>(w / 2 + s.object * k) : -100000;
			for (int y = 0; y < h; ++y)
			{
				const uint16_t* row = &canvas->luma[static_cast<size_t>(y + oy) * canvas->w + ox];
				uint16_t* out = &luma[static_cast<size_t>(y) * w];
				for (int x = 0; x < w; ++x)
				{
					int v = row[x];
					// The object: a 256x256 patch from elsewhere, moving on its own.
					if (x >= objX && x < objX + 256 && y >= h / 2 - 128 && y < h / 2 + 128)
						v = canvas->luma[static_cast<size_t>(y + 40) * canvas->w + (x - objX + 600)];
					// Film grain, new every frame.
					unsigned hsh = static_cast<unsigned>(x) * 2654435761u ^ static_cast<unsigned>(y) * 40503u ^ static_cast<unsigned>(index) * 97u;
					hsh ^= hsh >> 15;
					v += static_cast<int>(hsh % 1537u) - 768;
					v = static_cast<int>(4096 + (v - 4096) * gain);
					out[x] = static_cast<uint16_t>(std::min(65535, std::max(0, v)));
				}
			}
			for (int y = 0; y < h / 2; ++y)
				for (int x = 0; x < w / 2; ++x)
				{
					const size_t c = static_cast<size_t>(y + oy / 2) * (canvas->w / 2) + (x + ox / 2);
					chroma[(static_cast<size_t>(y) * (w / 2) + x) * 2] = canvas->cb[c];
					chroma[(static_cast<size_t>(y) * (w / 2) + x) * 2 + 1] = canvas->cr[c];
				}
			return true;
		};
		sources.push_back(src);
	}
	// A real clip (yuv420p10le, 3840x2160, seg_*.yuv in VP_TEST_CLIP).
	{
		char* dir = nullptr;
		size_t length = 0;
		if (_dupenv_s(&dir, &length, "VP_TEST_CLIP") == 0 && dir && *dir)
		{
			const std::string folder(dir);
			std::vector<std::string> files;
			WIN32_FIND_DATAA fd{};
			HANDLE find = FindFirstFileA((folder + "\\seg_*.yuv").c_str(), &fd);
			if (find != INVALID_HANDLE_VALUE)
			{
				do files.push_back(folder + "\\" + fd.cFileName); while (FindNextFileA(find, &fd));
				FindClose(find);
			}
			std::sort(files.begin(), files.end());
			const int w = 3840, h = 2160;
			const size_t frameBytes = static_cast<size_t>(w) * h * 3;   // yuv420p10le: 1.5 samples x 2 bytes
			int frames = 0;
			for (const auto& f : files)
			{
				WIN32_FILE_ATTRIBUTE_DATA a{};
				if (GetFileAttributesExA(f.c_str(), GetFileExInfoStandard, &a))
					frames += static_cast<int>((static_cast<uint64_t>(a.nFileSizeHigh) << 32 | a.nFileSizeLow) / frameBytes);
			}
			if (frames > 0)
			{
				Source src;
				// VP_TEST_CLIP_24P=1: the clip played as 23.976p (3:2 with the
				// de-judder extras, the cinema case).
				const bool as24 = envOn("VP_TEST_CLIP_24P");
				src.label = clipHdr ? (as24 ? "clip as 23.976p, 3840x2160, pseudo-HDR (PQ, BT.2020)" : "clip 29.97p, 3840x2160, pseudo-HDR (PQ, BT.2020)") :
					(as24 ? "clip as 23.976p, 3840x2160" : "clip 29.97p, 3840x2160");
				src.hdr = clipHdr;
				src.w = w; src.h = h; src.sourceRate = as24 ? 24000.0 / 1001.0 : 30000.0 / 1001.0; src.frames = frames;
				auto stream = std::make_shared<std::ifstream>();
				auto fileIndex = std::make_shared<size_t>(0);
				src.next = [files, stream, fileIndex, w, h, frameBytes](int, std::vector<uint16_t>& luma, std::vector<uint16_t>& chroma, std::string& segment)
				{
					std::vector<uint16_t> raw(frameBytes / 2);
					for (;;)
					{
						if (!stream->is_open())
						{
							if (*fileIndex >= files.size()) return false;
							stream->open(files[*fileIndex], std::ios::binary);
						}
						if (stream->read(reinterpret_cast<char*>(raw.data()), frameBytes)) break;
						stream->close();
						stream->clear();
						++*fileIndex;
					}
					segment = "clip part " + std::to_string(*fileIndex + 1);
					luma.resize(static_cast<size_t>(w) * h);
					chroma.resize(static_cast<size_t>(w / 2) * (h / 2) * 2);
					for (size_t i = 0; i < luma.size(); ++i) luma[i] = static_cast<uint16_t>(raw[i] << 6);
					const uint16_t* u = raw.data() + luma.size();
					const uint16_t* v = u + luma.size() / 4;
					for (size_t i = 0; i < luma.size() / 4; ++i)
					{
						chroma[i * 2] = static_cast<uint16_t>(u[i] << 6);
						chroma[i * 2 + 1] = static_cast<uint16_t>(v[i] << 6);
					}
					return true;
				};
				sources.push_back(src);
			}
		}
		std::free(dir);
	}

	const double displayRate = 60000.0 / 1001.0;
	for (Source& src : sources)
	{
		const int w = src.w, h = src.h;
		const bool down = w > ow;
		pl_tex luma[2] = { plane(w, h, 1), plane(w, h, 1) }, chroma[2] = { plane(w / 2, h / 2, 2), plane(w / 2, h / 2, 2) };
		Check(luma[0] && luma[1] && chroma[0] && chroma[1], "scenario: planes");
		// The matcher as VP's default (dejudder_low_res: all); VP_TEST_LOWRES=
		// off | search | all.
		Dejudder::Settings matcherSettings = ChainGpuProbe::Refined(Dejudder::Settings{});
		{
			char* v = nullptr;
			size_t n = 0;
			const std::string mode = _dupenv_s(&v, &n, "VP_TEST_LOWRES") == 0 && v && *v ? std::string(v) : std::string("all");
			std::free(v);
			matcherSettings.lowRes = mode != "off";
			matcherSettings.lowResAll = mode == "all";
		}
		Dejudder::MotionWarp warp(gpu.Log(), g, matcherSettings);
		warp.SetPassTiming(matcherPasses);
		AdaptiveDownscale::Module adaptive;
		AdaptiveDownscale::Settings adaptiveSettings;
		adaptiveSettings.faithfulOrtho = adaptiveOrtho;
		OcclusionMask::Hook mask;
		DejudderRendered::Cache cache;
		DejudderRendered::Warp rendered(gpu.Log(), g);
		DejudderRendered::VectorField field(gpu.Log(), g);
		auto ssim = Load(g, "SSimDownscaler.glsl", {}, ow, oh);
		auto led = Load(g, "LED compensation.glsl", { { "gain", 1.05 }, { "gain_domain", 1.0 }, { "compensation_eotf", 2.41 },
			{ "boost", 2.2 / 2.4 } }, ow, oh);
		auto sigma = Load(g, "Temporal sigma-delta.glsl", { { "enabled", 1 }, { "levels_full", 0 }, { "swapchain_bits", 10 },
			{ "color_bits", 10 }, { "motion_compensation", 1 }, { "occlusion_mask", 1 }, { "grain_tolerance_scale", 3 } }, ow, oh);
		const pl_rect2df sourceCrop{ 0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h) };
		const pl_rect2df targetCrop{ 0.0f, 0.0f, static_cast<float>(ow), static_cast<float>(oh) };
		Stats matcherMs, newFrame, extra, repeat, sigmaSearch;
		// The critical refresh (the matcher and the new frame's render run back
		// to back on the GPU) and everything one source frame costs.
		Stats newWithMatcher, perSourceFrame;
		// Per refresh slot (the GPU work it holds) and the slots over a refresh.
		Stats extraWithMatcher, anySlot;
		int slotsOver = 0;
		std::map<std::string, Stats> perSegment;
		// Per refresh type: each pass's GPU time summed, and the refreshes.
		std::map<std::string, std::pair<std::map<std::string, double>, int>> perKind;
		const int warmup = 6;   // shader compilation on the first frames
		std::vector<uint16_t> lumaData, chromaData;
		std::string segment;
		const double cadence = displayRate / src.sourceRate;   // 2.5 (3:2) or 2.0 (2:2)
		const bool extras = cadence > 2.01;   // de-judder 0.25 on 24p; 2:2 off by default
		double phase = 0.0;
		for (int f = 0; f < src.frames; ++f)
		{
			if (!src.next(f, lumaData, chromaData, segment))
				break;
			const int cur = f % 2, prev = 1 - cur;
			pl_tex_transfer_params up{};
			up.tex = luma[cur]; up.ptr = lumaData.data();
			Check(pl_tex_upload(g, &up), "scenario: upload");
			up.tex = chroma[cur]; up.ptr = chromaData.data();
			Check(pl_tex_upload(g, &up), "scenario: upload");
			pl_gpu_finish(g);
			Dejudder::Planes a, b;
			a.luma = luma[prev]; a.chroma = chroma[prev]; b.luma = luma[cur]; b.chroma = chroma[cur];
			a.chromaShiftX = b.chromaShiftX = 1;
			a.chromaShiftY = b.chromaShiftY = 1;
			if (f == 0)
				continue;   // the first frame only fills the pair
			// The matcher (A = the previous frame, B = this one); ahead, after
			// the first extra instead (the first frame before, as VP).
			const bool deferMatcher = aheadOrder && extras && f > 1;
			auto start = std::chrono::steady_clock::now();
			double estimateMs = 0.0;
			if (!deferMatcher)
			{
				Check(warp.Estimate(a, b, f % 2, true), "scenario: estimate: " + warp.LastError());
				pl_gpu_finish(g);
				estimateMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
			}
			double aheadMs = 0.0;
			bool aheadDone = !deferMatcher;
			if (f >= warmup)
			{
				matcherMs.Add(estimateMs);
				perSegment[std::string(segment) + ": matcher"].Add(estimateMs);
			}
			adaptive.SetFrame(adaptiveSettings, static_cast<uint64_t>(f), 1.0f / 24.0f);
			adaptive.SetSourceDownscales(down);
			AdaptiveDownscale::GrainField grain;
			grain.error = warp.GrainError();
			grain.field = warp.Field();
			grain.frameTrust = warp.FrameTrust();
			grain.minTrustedShare = warp.GetSettings().minTrustedShare;
			grain.sourceCrop = sourceCrop;
			adaptive.SetGrainField(grain);
			mask.SetFrame(warp.Occlusion(f % 2), sourceCrop, targetCrop);

			// The refreshes this source frame owns (3:2 or 2:2).
			phase += cadence;
			const int refreshes = static_cast<int>(std::floor(phase + 1e-9));
			phase -= refreshes;
			float previousShift = 0.0f;
			double frameTotal = estimateMs;
			for (int r = 0; r < refreshes; ++r)
			{
				pl_frame image{};
				pl_frame out{};
				out.num_planes = 1;
				out.planes[0].texture = targetTex;
				out.planes[0].components = 3;
				for (int i = 0; i < 3; ++i) out.planes[0].component_mapping[i] = i;
				out.repr.sys = PL_COLOR_SYSTEM_RGB;
				out.repr.levels = PL_COLOR_LEVELS_LIMITED;
				out.repr.bits.sample_depth = 10; out.repr.bits.color_depth = 10;
				out.color.primaries = PL_COLOR_PRIM_DCI_P3;
				out.color.transfer = PL_COLOR_TRC_GAMMA22;
				if (src.hdr)
					LibplaceboRenderParameters::ApplyTargetLuminance(false, 100.0f, 0.0f, out.color);
				pl_render_params params = pl_render_default_params;
				// The owner's Beta settings for HDR: peak detection off.
				if (src.hdr)
					params.peak_detect_params = nullptr;
				params.dither_params = nullptr;
				params.error_diffusion = nullptr;
				params.downscaler = &pl_filter_mitchell;
				params.upscaler = &pl_filter_ewa_lanczossharp;
				if (chromaLanczos)
					params.plane_upscaler = TestChromaFilter();
#if defined(PL_RENDER_PARAMS_OUTPUT_FBO_FLOAT32)
				params.output_fbo_float32 = true;
#endif
				PassTimes times;
				params.info_callback = CollectPass;
				params.info_priv = &times;
				std::vector<const pl_hook*> list;
				ExtendedRange::Hooks extended;
				const char* kind = "new frame";
				const bool isExtra = r > 0 && extras && cache.Texture(static_cast<uint64_t>(f)) != nullptr;
				pl_tex vectors = nullptr;
				std::vector<const pl_hook*> kept;
				if (r == 0)
				{
					image.num_planes = 2;
					image.planes[0].texture = b.luma; image.planes[0].components = 1;
					image.planes[0].component_mapping[0] = PL_CHANNEL_Y;
					image.planes[1].texture = b.chroma; image.planes[1].components = 2;
					image.planes[1].component_mapping[0] = PL_CHANNEL_CB;
					image.planes[1].component_mapping[1] = PL_CHANNEL_CR;
					image.repr.sys = PL_COLOR_SYSTEM_BT_709;
					image.repr.levels = PL_COLOR_LEVELS_LIMITED;
					image.color.primaries = PL_COLOR_PRIM_BT_709;
					image.color.transfer = PL_COLOR_TRC_GAMMA24;
					if (src.hdr)
					{
						image.repr.sys = PL_COLOR_SYSTEM_BT_2020_NC;
						image.color.primaries = PL_COLOR_PRIM_BT_2020;
						image.color.transfer = PL_COLOR_TRC_PQ;
						image.color.hdr.min_luma = 0.005f;
						image.color.hdr.max_luma = 400.0f;
					}
					pl_frame_set_chroma_location(&image, PL_CHROMA_LEFT);
					if (down) list.push_back(adaptive.Measure());
					list.push_back(led->hook);
					if (down) { list.push_back(ssim->hook); list.push_back(adaptive.Blend()); }
					list.push_back(mask.Get());
					list.push_back(sigma->hook);
					const ExtendedRange::Decision decision = ExtendedRange::Decide(true, image.color, out.color, false);
					ExtendedRange::Prepare(decision, extended, image.color, out.color, list, false);
					cache.Arm(static_cast<uint64_t>(f));
					list.push_back(cache.Hook());
					SetParam(sigma->hook, "static_refresh", 0.0);
					params.hooks = list.data();
					params.num_hooks = static_cast<int>(list.size());
				}
				else if (isExtra)
				{
					kind = "de-judder extra";
					const float shift = 0.25f * static_cast<float>(r) / static_cast<float>(cadence);
					pl_tex warped = nullptr;
					Check(rendered.Run(cache.Texture(static_cast<uint64_t>(f)), warp.Field(), warp.FrameTrust(),
						warp.GetSettings().minTrustedShare, shift, { sourceCrop, targetCrop, {}, {} }, warped, warp.SubField(),
						&warp.GetSettings()), "scenario: warp");
					Check(field.Run(warp.Field(), warp.FrameTrust(), warp.GetSettings().minTrustedShare, shift - previousShift,
						{ sourceCrop, targetCrop, {}, {} }, ow, oh, vectors, warp.SubField(), &warp.GetSettings()), "scenario: field");
					previousShift = shift;
					pl_mpv_user_shader_set_texture(sigma->hook, "VPVEC", vectors);
					SetParam(sigma->hook, "vector_source", 1.0);
					image = DejudderRendered::Frame(warped, cache.Repr(), cache.Color());
					params = DejudderRendered::OutputOnly(params, { sigma->hook }, kept);
					params.info_callback = CollectPass;
					params.info_priv = &times;
				}
				else
				{
					kind = "cadence repeat";
					image.num_planes = 2;
					image.planes[0].texture = b.luma; image.planes[0].components = 1;
					image.planes[0].component_mapping[0] = PL_CHANNEL_Y;
					image.planes[1].texture = b.chroma; image.planes[1].components = 2;
					image.planes[1].component_mapping[0] = PL_CHANNEL_CB;
					image.planes[1].component_mapping[1] = PL_CHANNEL_CR;
					image.repr.sys = PL_COLOR_SYSTEM_BT_709;
					image.repr.levels = PL_COLOR_LEVELS_LIMITED;
					image.color.primaries = PL_COLOR_PRIM_BT_709;
					image.color.transfer = PL_COLOR_TRC_GAMMA24;
					if (src.hdr)
					{
						image.repr.sys = PL_COLOR_SYSTEM_BT_2020_NC;
						image.color.primaries = PL_COLOR_PRIM_BT_2020;
						image.color.transfer = PL_COLOR_TRC_PQ;
						image.color.hdr.min_luma = 0.005f;
						image.color.hdr.max_luma = 400.0f;
					}
					pl_frame_set_chroma_location(&image, PL_CHROMA_LEFT);
					if (down) list.push_back(adaptive.Measure());
					list.push_back(led->hook);
					if (down) { list.push_back(ssim->hook); list.push_back(adaptive.Blend()); }
					list.push_back(sigma->hook);
					const ExtendedRange::Decision decision = ExtendedRange::Decide(true, image.color, out.color, false);
					ExtendedRange::Prepare(decision, extended, image.color, out.color, list, false);
					SetParam(sigma->hook, "static_refresh", 1.0);
					params.hooks = list.data();
					params.num_hooks = static_cast<int>(list.size());
				}
				pl_gpu_finish(g);
				start = std::chrono::steady_clock::now();
				Check(pl_render_image(gpu.RendererHandle(), &image, &out, &params), std::string("scenario: render (") + kind + ")");
				pl_gpu_finish(g);
				const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
				if (isExtra)
				{
					SetParam(sigma->hook, "vector_source", 0.0);
					pl_mpv_user_shader_set_texture(sigma->hook, "VPVEC", nullptr);
				}
				SetParam(sigma->hook, "static_refresh", 0.0);
				// Ahead: the matcher after the first extra, in its slot.
				double slotMs = r == 0 ? estimateMs + ms : ms;
				if (!aheadDone && isExtra)
				{
					pl_gpu_finish(g);
					const auto aheadStart = std::chrono::steady_clock::now();
					Check(warp.Estimate(a, b, f % 2, true), "scenario: estimate ahead: " + warp.LastError());
					pl_gpu_finish(g);
					aheadMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - aheadStart).count();
					aheadDone = true;
					slotMs += aheadMs;
					frameTotal += aheadMs;
					if (f >= warmup)
						extraWithMatcher.Add(slotMs);
				}
				if (f >= warmup)
				{
					anySlot.Add(slotMs);
					slotsOver += slotMs > 1000.0 / displayRate ? 1 : 0;
				}
				frameTotal += ms;
				if (f >= warmup && r == 0)
				{
					newWithMatcher.Add(estimateMs + ms);
					perSegment[std::string(segment) + ": new frame with the matcher"].Add(estimateMs + ms);
				}
				if (f >= warmup)
				{
					(r == 0 ? newFrame : (isExtra ? extra : repeat)).Add(ms);
					perSegment[std::string(segment) + ": " + kind].Add(ms);
					const auto search = times.byName.find("VideoProcessor sigma-delta: block motion search");
					if (r == 0 && search != times.byName.end())
						sigmaSearch.Add(search->second.first / 1e6);
					auto& totals = perKind[kind];
					for (const auto& pass : times.byName)
						totals.first[pass.first] += pass.second.first / 1e6;
					++totals.second;
				}
			}
			if (!aheadDone)
			{
				// No extra this frame (none planned): the matcher before the next.
				Check(warp.Estimate(a, b, f % 2, true), "scenario: estimate: " + warp.LastError());
				pl_gpu_finish(g);
			}
			if (f >= warmup)
			{
				perSourceFrame.Add(frameTotal);
				perSegment[std::string(segment) + ": one source frame"].Add(frameTotal);
			}
		}
		std::cout << "Beta scenario timing (matcher " << (matcherSettings.lowRes ? (matcherSettings.lowResAll ? "half size, every pass" : "half size searches") : "full size")
			<< "), " << src.label << (chromaLanczos ? ", chroma lanczos" : "") <<
			(adaptiveOrtho ? ", faithful ortho" : "") << " -> " << ow << "x" << oh << " (" << (extras ? "3:2 with de-judder extras" : "2:2, repeats") << "):\n"
			<< "  matcher per source frame: " << matcherMs.Text() << "\n"
			<< "  refresh, new frame:      " << newFrame.Text() << "\n"
			<< "  refresh, de-judder extra:" << extra.Text() << "\n"
			<< "  refresh, cadence repeat: " << repeat.Text() << "\n"
			<< "  sigma-delta search (new frame, GPU): " << sigmaSearch.Text() << "\n"
			<< "  new frame with the matcher (critical refresh): " << newWithMatcher.Text() << "\n"
			<< "  one source frame (matcher + its refreshes): " << perSourceFrame.Text() << "\n"
			<< "  matcher order: " << (aheadOrder && extras ? "ahead (after the first extra)" : "before the new frame") << "\n"
			<< "  extra slot holding the matcher: " << extraWithMatcher.Text() << "\n"
			<< "  any refresh slot: " << anySlot.Text() << ", over " << (1000.0 / displayRate) << " ms: " << slotsOver << "\n";
		for (const auto& entry : perSegment)
			std::cout << "    " << entry.first << ": " << entry.second.Text() << "\n";
		if (matcherPasses)
		{
			std::vector<std::pair<double, std::string>> passes;
			for (const auto& entry : warp.PassMs())
				passes.emplace_back(entry.second / std::max(1, src.frames - 1), entry.first);
			std::sort(passes.rbegin(), passes.rend());
			std::cout << "  matcher passes per source frame (each drained):\n";
			for (const auto& entry : passes)
				if (entry.first >= 0.05)
					std::cout << "    " << entry.first << " ms  " << entry.second << "\n";
		}
		// Where each refresh type's GPU time goes (libplacebo's pass timers
		// report the latest finished measurement, so neighbouring refreshes
		// can share a value; averaged over the run).
		for (const auto& kind : perKind)
		{
			std::vector<std::pair<double, std::string>> passes;
			double sum = 0.0;
			for (const auto& pass : kind.second.first)
			{
				passes.emplace_back(pass.second / kind.second.second, pass.first);
				sum += pass.second / kind.second.second;
			}
			std::sort(passes.rbegin(), passes.rend());
			std::cout << "  passes per " << kind.first << " (GPU, mean " << sum << " ms):\n";
			for (size_t i = 0; i < passes.size() && i < 14; ++i)
				if (passes[i].first >= 0.05)
					std::cout << "    " << passes[i].first << " ms  " << passes[i].second << "\n";
		}
		for (pl_tex* t : { &luma[0], &luma[1], &chroma[0], &chroma[1] })
			pl_tex_destroy(g, t);
	}
	pl_tex_destroy(g, &targetTex);
}

// S2 A/B regression (planner 2026-10-12): the chain bias is the float16
// round trip at the PRE_OUTPUT texture hooks. With patch 0003's output-pass
// float32 forced off it returns (0.25-0.6 code low); with it on it is gone.
// A libplacebo rebuild that loses the patch fails here.
void RunBetaChainBiasRegression()
{
	ChainGpuProbe::Combo combo;
	combo.scale = 0.5;
	combo.gammaLight = true;
	combo.stage10 = false;
	combo.fullRange = false;
	combo.matcher = true;
	combo.measureOnly = true;
	double means[2] = {};
	for (int on = 0; on < 2; ++on)
	{
		Gpu g;
		combo.outputFloat32 = on == 1;
		const ChainGpuProbe::Outcome outcome = ChainGpuProbe::RunMatrix(g.GpuHandle(), g.RendererHandle(),
			g.LogHandle(), [](const char* file) { return LoadRepoShader(file); }, false, combo);
		Check(outcome.ok, "bias A/B: " + outcome.failure);
		CheckCleanLog(g, "bias A/B");
		means[on] = outcome.signedMean;
	}
	std::cout << "Beta S2 A/B: chain minus reference " << means[0] << " code without the output-pass float32, "
		<< means[1] << " with it\n";
	Check(means[0] <= -0.2 && means[0] >= -0.7, "bias A/B: without the patch the bias should return (0.25-0.6 low): " +
		std::to_string(means[0]));
	Check(std::fabs(means[1]) < 0.1, "bias A/B: with the patch the bias should be gone: " + std::to_string(means[1]));
}
