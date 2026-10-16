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
#include <cstring>
#include <functional>
#include <thread>
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
					shift, g, out, warp.SubField(), &warp.GetSettings(), warp.Camera(), warp.PixelMask(), warp.VectorOk()), "6d: rendered warp failed: " + rendered.Error());
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
			int renderedIdealMax = 0;
			double renderedIdeal = 0.0;
			difference(ideal, refinedRendered, renderedIdealMax, renderedIdeal);
			Check(refinedMean <= 0.6, "6d with the refinements: rendered extra differs from the source extra (mean " +
				std::to_string(refinedMean) + ", max " + std::to_string(refinedMax) + "; from the halfway picture: source " +
				std::to_string(refinedIdeal) + ", rendered " + std::to_string(renderedIdeal) + ", plain source " + std::to_string(plainIdeal) + ")");
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
	// 2026-10-15: a pair whose A is the last pair's B reuses B's uncorrected
	// half-size planes. Consecutive pairs with frame numbers against the same
	// pairs without (every plane rebuilt): the same fields, byte for byte, at
	// full size and on the half-size path (lowRes, with and without lowResAll),
	// on a plain pan and on a pan through a brightness fade (the fade
	// correction rewrites B's planes; the next A must still be the
	// uncorrected B).
	{
		const int rw = 384, rh = 1536;   // tall enough to halve (lowRes)
		const enum pl_fmt_caps caps = static_cast<enum pl_fmt_caps>(
			PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_STORABLE | PL_FMT_CAP_LINEAR | PL_FMT_CAP_HOST_READABLE);
		auto make = [&](int w, int h, int comps, const std::vector<uint16_t>& data)
		{
			pl_tex_params params{};
			params.w = w;
			params.h = h;
			params.format = pl_find_fmt(handle, PL_FMT_UNORM, comps, 16, 16, caps);
			params.sampleable = true;
			params.host_readable = true;
			params.initial_data = data.data();
			return pl_tex_create(handle, &params);
		};
		auto fieldBytes = [&](pl_tex tex)
		{
			std::vector<uint16_t> out(static_cast<size_t>(tex->params.w) * tex->params.h * 4);
			pl_tex_transfer_params d{};
			d.tex = tex;
			d.ptr = out.data();
			Check(pl_tex_download(handle, &d), "plane reuse: field not readable");
			return out;
		};
		int fadeActivePairs = 0;
		for (int fade = 0; fade < 2; ++fade)
		{
			std::vector<Dejudder::Planes> frames(4);
			for (int k = 0; k < 4; ++k)
			{
				// The fade frames: a still picture brightening (a pan moves the 1/32 statistics the
				// fade test reads); the pan frames: a pan at constant brightness.
				const double gain = fade ? 1.0 + 0.05 * k : 1.0;
				const int m = fade ? 0 : k;
				std::vector<uint16_t> luma(static_cast<size_t>(rw) * rh), chroma(static_cast<size_t>(rw / 2) * rh * 2);
				for (int y = 0; y < rh; ++y)
					for (int x = 0; x < rw; ++x)
						luma[static_cast<size_t>(y) * rw + x] = static_cast<uint16_t>(
							std::min(65535.0, Texture(x - 6 * m, y - 3 * m) * gain));
				for (int y = 0; y < rh; ++y)
					for (int x = 0; x < rw / 2; ++x)
						for (int c = 0; c < 2; ++c)
							chroma[(static_cast<size_t>(y) * (rw / 2) + x) * 2 + c] = Texture((x - 3 * m) * 2, y - 3 * m, 7 + c);
				frames[k].luma = make(rw, rh, 1, luma);
				frames[k].chroma = make(rw / 2, rh, 2, chroma);
				frames[k].chromaShiftX = 1;
				frames[k].chromaShiftY = 0;
			}
			for (int path = 0; path < 3; ++path)
			{
				Dejudder::Settings s = ChainGpuProbe::Refined(settings);
				s.hostReadable = true;
				s.lowRes = path > 0;
				s.lowResAll = path == 2;
				Dejudder::MotionWarp reuse(gpu.Log(), handle, s), fresh(gpu.Log(), handle, s);
				for (int k = 0; k + 1 < 4; ++k)
				{
					Dejudder::Planes ra = frames[k], rb = frames[k + 1];
					ra.sequence = static_cast<uint64_t>(k + 1);
					rb.sequence = static_cast<uint64_t>(k + 2);
					Check(reuse.Estimate(ra, rb) && fresh.Estimate(frames[k], frames[k + 1]),
						"plane reuse: estimate failed: " + reuse.LastError() + fresh.LastError());
					if (fade && reuse.FadeStateForTest())
					{
						float st[4] = {};   // the fade state: 1x1 rgba32f, z = correction on
						pl_tex_transfer_params d{};
						d.tex = reuse.FadeStateForTest();
						d.ptr = st;
						Check(d.tex->params.format->texel_size == sizeof(st) && pl_tex_download(handle, &d),
							"plane reuse: fade state not readable");
						fadeActivePairs += st[2] > 0.5f ? 1 : 0;
					}
					Check(fieldBytes(reuse.Field()) == fieldBytes(fresh.Field()),
						std::string("plane reuse: the field differs from the rebuilt planes' (") + (fade ? "fade" : "pan") +
							", path " + std::to_string(path) + ", pair " + std::to_string(k + 1) + ")");
				}
				Check(reuse.ReusedPlanesForTest() == 2 && fresh.ReusedPlanesForTest() == 0,
					"plane reuse: expected 2 reused pairs, got " + std::to_string(reuse.ReusedPlanesForTest()));
			}
			for (auto& f : frames)
			{
				pl_tex_destroy(handle, &f.luma);
				pl_tex_destroy(handle, &f.chroma);
			}
		}
		Check(fadeActivePairs > 0, "plane reuse: the fade correction never engaged on the fade frames");
		std::cout << "Beta de-judder: reused half-size planes byte-identical to rebuilt ones (full size, lowRes, lowResAll; pan and fade, "
			<< fadeActivePairs << " pairs with the fade correction on)\n";
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
			shift, g, out, warp.SubField(), &warp.GetSettings(), warp.Camera(), warp.PixelMask(), warp.VectorOk()), "R1: rendered warp failed: " + rendered.Error());
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
	// Planner D2: with M6 (the renderer's defaults) the warp's own per-pixel
	// displacement (VPPIX, the step since the previous present) instead of
	// block vectors. On the same M6 pictures: the own search, the block
	// vectors (VPVEC) and the per-pixel displacement; per-pixel stays within
	// 1 code of the own carry and no farther on average than block vectors.
	{
		Dejudder::Settings m6 = ChainGpuProbe::Refined(settings);
		m6.hostReadable = true;
		Dejudder::MotionWarp warpReal(gpu.Log(), handle, m6);
		// Every pixel weight 1 (thresholds far above any error): the picture
		// moves as whole blocks, so per-pixel must equal block vectors exactly.
		Dejudder::Settings full = m6;
		full.pixelTrustLo = 1000.0f;
		full.pixelTrustHi = 2000.0f;
		Dejudder::MotionWarp warpFull(gpu.Log(), handle, full);
		for (Dejudder::MotionWarp* w : { &warpReal, &warpFull })
			Check(w->Estimate(a, b) && w->Estimate(a, b), "D2: Estimate failed: " + w->LastError());
		Check(warpReal.PixelMask() != nullptr, "D2: the defaults have no pixel weights");
		Dejudder::MotionWarp* current = &warpReal;
		auto sequence = [&](int mode)   // 0 own search, 1 block vectors, 2 per-pixel
		{
			Dejudder::MotionWarp& warpM6 = *current;
			auto sigma = loadSigma();
			renderA({ sigma->hook });
			std::vector<std::vector<uint32_t>> result;
			float previous = 0.0f;
			for (float s : shifts)
			{
				pl_tex out = nullptr, disp = nullptr, vectors = nullptr;
				Check(rendered.Run(cache.Texture(3), warpM6.Field(), warpM6.FrameTrust(), m6.minTrustedShare,
					s, g, out, warpM6.SubField(), &warpM6.GetSettings(), warpM6.Camera(), warpM6.PixelMask(), warpM6.VectorOk(),
					mode == 2 ? &disp : nullptr, (s - previous) / s), "D2: rendered warp failed: " + rendered.Error());
				if (mode >= 1)
				{
					Check(field.Run(warpM6.Field(), warpM6.FrameTrust(), m6.minTrustedShare, s - previous, g, width, height,
						vectors, warpM6.SubField(), &warpM6.GetSettings()), "D2: vector field failed: " + field.Error());
					Check(pl_mpv_user_shader_set_texture(sigma->hook, "VPVEC", vectors), "D2: VPVEC not accepted");
					Check(SetParam(sigma->hook, "vector_source", 1.0), "D2: no vector_source parameter");
				}
				if (mode == 2)
				{
					Check(disp != nullptr, "D2: no displacement texture");
					Check(pl_mpv_user_shader_set_texture(sigma->hook, "VPPIX", disp), "D2: VPPIX not accepted");
					Check(SetParam(sigma->hook, "vector_pixel", 1.0), "D2: no vector_pixel parameter");
				}
				pl_frame image = DejudderRendered::Frame(out, cache.Repr(), cache.Color());
				pl_frame t = target();
				std::vector<const pl_hook*> kept;
				const pl_render_params pr = DejudderRendered::OutputOnly(params, { sigma->hook }, kept);
				Check(pl_render_image(gpu.RendererHandle(), &image, &t, &pr), "D2: extra render failed");
				result.push_back(readback());
				SetParam(sigma->hook, "vector_source", 0.0);
				SetParam(sigma->hook, "vector_pixel", 0.0);
				Check(pl_mpv_user_shader_set_texture(sigma->hook, "VPVEC", nullptr) &&
					pl_mpv_user_shader_set_texture(sigma->hook, "VPPIX", nullptr), "D2: textures not restored");
				previous = s;
			}
			return result;
		};
		current = &warpFull;
		const auto blockFull = sequence(1), pixelFull = sequence(2);
		for (size_t i = 0; i < blockFull.size(); ++i)
		{
			int maxDiff = 0;
			double mean = 0.0;
			difference(blockFull[i], pixelFull[i], maxDiff, mean);
			// Not byte-exact: the block field holds one vector per 16x16 block,
			// the warp's interpolates within it (a vector near a half pixel
			// rounds either way across a block).
			Check(maxDiff <= 1 && mean <= 0.03, "D2: with every pixel weight 1 the per-pixel carry differs from the block vectors (max " +
				std::to_string(maxDiff) + ", mean " + std::to_string(mean) + ", extra " + std::to_string(i) + ")");
			std::cout << "Beta D2: extra " << i << ", every pixel weight 1: per-pixel vs block vectors max " << maxDiff << ", mean " << mean << "\n";
		}
		current = &warpReal;
		const auto ownM6 = sequence(0), blockM6 = sequence(1), pixelM6 = sequence(2);
		for (size_t i = 0; i < ownM6.size(); ++i)
		{
			int maxPixel = 0, maxBlock = 0;
			double meanPixel = 0.0, meanBlock = 0.0;
			difference(ownM6[i], pixelM6[i], maxPixel, meanPixel);
			difference(ownM6[i], blockM6[i], maxBlock, meanBlock);
			Check(maxPixel <= 1, "D2: the per-pixel carry differs from the own carry by " + std::to_string(maxPixel) +
				" codes on extra " + std::to_string(i));
			// The own search and block vectors assume whole 16x16 blocks moved;
			// with M6 a partially weighted pixel moved less, which the per-pixel
			// displacement follows: slightly farther from the own search there.
			Check(meanPixel <= meanBlock + 0.03, "D2: the per-pixel carry is farther from the own carry (mean " +
				std::to_string(meanPixel) + ") than the block vectors (" + std::to_string(meanBlock) + ") on extra " + std::to_string(i));
			std::cout << "Beta D2: extra " << i << " (shift " << shifts[i] << ", M6) per-pixel vs own: max " << maxPixel
				<< ", mean " << meanPixel << "; block vectors vs own: max " << maxBlock << ", mean " << meanBlock << "\n";
		}
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

// The live matcher alone, quickly (VP_TEST_MATCHER_TIMING=1, hardware GPU; a
// minute instead of the whole suite). VP_TEST_MATCHER_VARIANTS="a;b;c" names
// variants, each set as VP_TEST_SEARCH_PROFILE (test builds that read it);
// they are timed in rotation (VP_TEST_MATCHER_ROUNDS rounds, default 5) so a
// drifting GPU clock affects them alike, and the medians are reported: the
// live settings on consecutive frames (planes reused), wall-clock and passes.
void RunBetaMatcherTiming()
{
	Gpu gpu;
	pl_gpu g = gpu.GpuHandle();
	const int sw = 3840, sh = 2160;
	auto env = [](const char* name) -> std::string
	{
		char* v = nullptr;
		size_t n = 0;
		std::string r;
		if (_dupenv_s(&v, &n, name) == 0 && v)
			r = v;
		std::free(v);
		return r;
	};
	std::vector<std::string> variants;
	{
		std::string list = env("VP_TEST_MATCHER_VARIANTS");
		if (list.empty())
			list = "none";
		size_t start = 0;
		while (start <= list.size())
		{
			const size_t end = list.find(';', start);
			variants.push_back(list.substr(start, end == std::string::npos ? std::string::npos : end - start));
			if (end == std::string::npos)
				break;
			start = end + 1;
		}
	}
	const int rounds = std::max(1, std::atoi(env("VP_TEST_MATCHER_ROUNDS").empty() ? "5" : env("VP_TEST_MATCHER_ROUNDS").c_str()));
	auto noise = [](int x, int y, unsigned salt)
	{
		const unsigned h = (static_cast<unsigned>(x) * 73856093u) ^ (static_cast<unsigned>(y) * 19349663u) ^ (salt * 83492791u);
		return static_cast<uint16_t>(16384 + (h % 32768u));
	};
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
	Check(a.luma && a.chroma && b.luma && b.chroma, "matcher timing: plane upload failed");
	Dejudder::Settings live = ChainGpuProbe::Refined(Dejudder::Settings{});
	live.lowRes = true;
	live.lowResAll = true;
	live.pixelTrust = true;
	live.pixelTrustLo = 3.0f;
	live.pixelTrustHi = 8.0f;
	live.landingCheck = 1.5f;
	struct Result { std::vector<double> wall, gpu; std::map<std::string, std::vector<double>> passes; };
	std::vector<Result> results(variants.size());
	const int reps = 10;
	for (int round = 0; round < rounds; ++round)
		for (size_t vi = 0; vi < variants.size(); ++vi)
		{
			// Tokens: SKIP_* go to the search profile; the others switch one
			// feature off (its cost, for the low-latency design).
			Dejudder::Settings vs = live;
			std::string skips;
			{
				const std::string& v = variants[vi];
				size_t st = 0;
				while (st <= v.size())
				{
					const size_t en = v.find(',', st);
					const std::string t = v.substr(st, en == std::string::npos ? std::string::npos : en - st);
					if (t.rfind("SKIP_", 0) == 0 || t.rfind("OLD_", 0) == 0) skips += t + ",";
					else if (t == "noglobal") vs.globalPrior = false;
					else if (t == "nosmooth") vs.vectorSmooth = false;
					else if (t == "nocand") vs.estCandidates = false;
					else if (t == "noqpel") vs.estQpel = false;
					else if (t == "nogate") vs.estGate = false;
					else if (t == "notwo") vs.globalTwo = false;
					else if (t == "noregional") vs.regionalHold = false;
					else if (t == "nofade") vs.fadeMatch = false;
					else if (t == "nopixel") vs.pixelTrust = false;
					else if (t == "nolanding") vs.landingCheck = 0.0f;
					else if (t == "nohyst") vs.holdHysteresis = 1;
					else if (t == "nopicture") vs.pictureArea = false;
					else if (t == "nolowresall") vs.lowResAll = false;
					else if (t == "nolowres") { vs.lowRes = false; vs.lowResAll = false; }
					else Check(t == "none" || t.empty(), "matcher timing: unknown variant token " + t);
					if (en == std::string::npos)
						break;
					st = en + 1;
				}
			}
			_putenv_s("VP_TEST_SEARCH_PROFILE", skips.c_str());
			Dejudder::MotionWarp m(gpu.Log(), g, vs);
			uint64_t seq = 0;
			int slot = 0;
			auto next = [&]
			{
				Dejudder::Planes pa = (seq % 2) ? b : a, pb = (seq % 2) ? a : b;
				pa.sequence = seq + 1;
				pb.sequence = seq + 2;
				++seq;
				const bool ok = m.Estimate(pa, pb, slot, true);
				Check(ok, "matcher timing: estimate failed: " + m.LastError());
				slot ^= 1;
			};
			next();   // the shaders compiled outside the timing
			auto compiles = [&] { size_t c = 0, at = 0; while ((at = gpu.messages.find("(slow!)", at)) != std::string::npos) { ++c; ++at; } return c; };
			const size_t before = compiles();
			results[vi].wall.push_back(DrainedMs(g, reps, next));
			if (compiles() != before)
				std::cout << "  [" << variants[vi] << "] shader compiles during the timed estimates: " << (compiles() - before) << "\n";
			m.SetPassTiming(true);
			for (int i = 0; i < reps; ++i)
				next();
			double sum = 0.0;
			for (const auto& entry : m.PassMs())
			{
				results[vi].passes[entry.first].push_back(entry.second / reps);
				sum += entry.second / reps;
			}
			results[vi].gpu.push_back(sum);
			m.SetPassTiming(false);
		}
	_putenv_s("VP_TEST_SEARCH_PROFILE", "");
	auto median = [](std::vector<double> v)
	{
		std::sort(v.begin(), v.end());
		return v.empty() ? 0.0 : v.size() % 2 ? v[v.size() / 2] : 0.5 * (v[v.size() / 2 - 1] + v[v.size() / 2]);
	};
	std::cout << "Beta matcher timing (live settings, 4K, consecutive frames; medians of " << rounds << " rotated rounds):\n";
	for (size_t vi = 0; vi < variants.size(); ++vi)
	{
		std::vector<std::pair<double, std::string>> sorted;
		for (const auto& entry : results[vi].passes)
			sorted.emplace_back(median(entry.second), entry.first);
		std::sort(sorted.rbegin(), sorted.rend());
		std::cout << "  [" << variants[vi] << "] " << median(results[vi].wall) << " ms wall-clock, " << median(results[vi].gpu)
			<< " ms GPU over " << sorted.size() << " passes; top:";
		for (size_t k = 0; k < sorted.size() && k < 4; ++k)
			std::cout << " " << sorted[k].second.substr(sorted[k].second.find(':') == std::string::npos ? 0 : sorted[k].second.find(':') + 2)
				<< " " << sorted[k].first << ";";
		std::cout << "\n";
	}
	pl_tex_destroy(g, &a.luma);
	pl_tex_destroy(g, &a.chroma);
	pl_tex_destroy(g, &b.luma);
	pl_tex_destroy(g, &b.chroma);
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
	const double matcherMs = DrainedMs(g, 10, [&] { { const bool ok_ = warp.Estimate(a, b, slot, true); Check(ok_, "pass timing: estimate failed: " + warp.LastError() + " | " + gpu.messages.substr(gpu.messages.find("rror") == std::string::npos ? 0 : (gpu.messages.find("rror") > 1500 ? gpu.messages.find("rror") - 1500 : 0), 3000)); } slot ^= 1; });
	{
		// Where the matcher's time goes (each pass drained; per estimate).
		warp.SetPassTiming(true);
		const int reps = 5;
		for (int i = 0; i < reps; ++i)
		{
			{ const bool ok_ = warp.Estimate(a, b, slot, true); Check(ok_, "pass timing: estimate failed: " + warp.LastError()); }
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
		const double plainMs = DrainedMs(g, 10, [&] { { const bool ok_ = plain.Estimate(a, b, slot, true); Check(ok_, "pass timing: estimate failed: " + plain.LastError()); } slot ^= 1; });
		std::cout << "  de-judder matcher with default settings " << plainMs << " ms\n";
		// The live configuration (owner 2026-10-15): the refined settings with
		// the half-size matcher for every pass (dejudder_low_res all), M6's
		// pixel trust and the landing check, as the renderer runs it; wall
		// clock (GPU drained) and each pass drained.
		Dejudder::Settings liveSettings = ChainGpuProbe::Refined(Dejudder::Settings{});
		liveSettings.lowRes = true;
		liveSettings.lowResAll = true;
		liveSettings.pixelTrust = true;
		liveSettings.pixelTrustLo = 3.0f;
		liveSettings.pixelTrustHi = 8.0f;
		liveSettings.landingCheck = 1.5f;
		Dejudder::MotionWarp live(gpu.Log(), g, liveSettings);
		const double liveMs = DrainedMs(g, 10, [&] { { const bool ok_ = live.Estimate(a, b, slot, true); Check(ok_, "pass timing: estimate failed: " + live.LastError()); } slot ^= 1; });
		// docs/VP-nvof-ahead.md: the live configuration with the optical-flow engine as a
		// refinement candidate, and as the seed in place of the searches (wall clock, GPU
		// drained; the engine is separate hardware but its result is waited for here).
		for (int mode = 0; mode < 2; ++mode)
		{
			Dejudder::Settings ofSettings = liveSettings;
			ofSettings.opticalFlow = true;
			ofSettings.opticalFlowCandidate = true;
			ofSettings.opticalFlowSeed = mode == 1;
			Dejudder::MotionWarp of(gpu.Log(), g, ofSettings);
			const double ofMs = DrainedMs(g, 10, [&] { { const bool ok_ = of.Estimate(a, b, slot, true); Check(ok_, "pass timing: estimate failed: " + of.LastError()); } slot ^= 1; });
			std::cout << "  de-judder live matcher + optical flow " << (mode == 1 ? "seed" : "candidate") << " " << ofMs << " ms (engine "
				<< (of.OpticalFlowUsed() ? "used" : ("not used: " + of.OpticalFlowError()).c_str()) << "; live matcher " << liveMs << " ms)\n";
		}
		live.SetPassTiming(true);
		for (int i = 0; i < reps; ++i)
		{
			{ const bool ok_ = live.Estimate(a, b, slot, true); Check(ok_, "pass timing: estimate failed: " + live.LastError()); }
			slot ^= 1;
		}
		std::vector<std::pair<double, std::string>> livePasses;
		double liveSum = 0.0;
		for (const auto& entry : live.PassMs())
		{
			livePasses.emplace_back(entry.second / reps, entry.first);
			liveSum += entry.second / reps;
		}
		live.SetPassTiming(false);
		std::sort(livePasses.rbegin(), livePasses.rend());
		std::cout << "  de-judder matcher, live settings (half size for every pass, M6, landing check): " << liveMs
			<< " ms wall-clock; passes " << liveSum << " ms GPU over " << livePasses.size() << " passes:\n";
		for (const auto& entry : livePasses)
			if (entry.first >= 0.02)
				std::cout << "    " << entry.first << " ms  " << entry.second << "\n";
		// The live settings on consecutive frames as the renderer runs them
		// ((A, B), (B, A'), ... with frame numbers): each pair's A is the last
		// pair's B, so its planes are reused (2026-10-15). Plus the small
		// passes' share (under 0.15 ms each: mostly one work group, little to
		// gain from a larger GPU).
		{
			Dejudder::MotionWarp chain(gpu.Log(), g, liveSettings);
			uint64_t seq = 0;
			auto next = [&]
			{
				Dejudder::Planes pa = (seq % 2) ? b : a, pb = (seq % 2) ? a : b;
				pa.sequence = seq + 1;
				pb.sequence = seq + 2;
				++seq;
				Check(chain.Estimate(pa, pb, slot, true), "pass timing: consecutive estimate failed");
				slot ^= 1;
			};
			next();
			const double chainMs = DrainedMs(g, 10, next);
			chain.SetPassTiming(true);
			for (int i = 0; i < reps; ++i)
				next();
			double chainSum = 0.0, smallSum = 0.0;
			int smallCount = 0;
			std::vector<std::pair<double, std::string>> chainPasses;
			for (const auto& entry : chain.PassMs())
			{
				const double ms = entry.second / reps;
				chainPasses.emplace_back(ms, entry.first);
				chainSum += ms;
				if (ms < 0.15)
				{
					smallSum += ms;
					++smallCount;
				}
			}
			chain.SetPassTiming(false);
			std::sort(chainPasses.rbegin(), chainPasses.rend());
			std::cout << "  de-judder matcher, live settings on consecutive frames (planes reused " << chain.ReusedPlanesForTest()
				<< "): " << chainMs << " ms wall-clock; passes " << chainSum << " ms GPU over " << chainPasses.size()
				<< " passes; " << smallCount << " small passes " << smallSum << " ms:\n";
			for (const auto& entry : chainPasses)
				if (entry.first >= 0.02)
					std::cout << "    " << entry.first << " ms  " << entry.second << "\n";
		}
		// The serial reference search (one thread per block, no work-group
		// barriers) on the same live settings, for the restructure plan.
		Dejudder::MotionWarp serialLive(gpu.Log(), g, liveSettings);
		serialLive.SetSerialSearchForTest(true);
		serialLive.SetPassTiming(true);
		for (int i = 0; i < reps; ++i)
		{
			Check(serialLive.Estimate(a, b, slot, true), "pass timing: estimate failed");
			slot ^= 1;
		}
		for (const auto& entry : serialLive.PassMs())
			if (entry.first.find("block search") != std::string::npos || entry.first.find("global motion") != std::string::npos)
				std::cout << "  live settings, serial reference: " << entry.second / reps << " ms  " << entry.first << "\n";
		serialLive.SetPassTiming(false);
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
			warp.SubField(), &warp.GetSettings(), warp.Camera(), warp.PixelMask(), warp.VectorOk()), "pass timing: warp failed"); });
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
							&warp.GetSettings(), warp.Camera(), warp.PixelMask(), warp.VectorOk()), "dither compare: warp");
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

// The timing scenario's rates (owner rule: nothing tied to one refresh rate,
// frame rate or resolution), set in this one place: the display refresh and
// the film-like source rate (the synthetic sequences and the clip "as film").
// VP_TEST_DISPLAY_RATE and VP_TEST_FILM_RATE (frames per second, "50" or
// "60000/1001") override the defaults, 59.94 Hz and 23.976p. The timing
// gate's budgets are relative to the rates each run used (display_period,
// source_period in tools/timing-budgets.cfg).
static double ScenarioRate(const char* name, double fallback)
{
	char* v = nullptr;
	size_t n = 0;
	double rate = fallback;
	if (_dupenv_s(&v, &n, name) == 0 && v && *v)
	{
		const std::string text(v);
		const size_t slash = text.find('/');
		const double num = std::atof(text.substr(0, slash).c_str());
		const double den = slash == std::string::npos ? 1.0 : std::atof(text.substr(slash + 1).c_str());
		if (num > 0.0 && den > 0.0) rate = num / den;
	}
	std::free(v);
	return rate;
}
static double ScenarioDisplayRate() { return ScenarioRate("VP_TEST_DISPLAY_RATE", 60000.0 / 1001.0); }
static double ScenarioFilmRate() { return ScenarioRate("VP_TEST_FILM_RATE", 24000.0 / 1001.0); }
static std::string RateText(double rate)   // 23.976, 59.94, 24, 50
{
	std::ostringstream s;
	s.precision(5);
	s << rate;
	return s.str();
}

// Owner 2026-10-13: the timing gate collects each scenario run here.
struct ScenarioTimingRecord
{
	std::string label;
	bool extras = false;
	double displayRate = 0.0;   // the run's refresh (Hz) and source rate (fps):
	double sourceRate = 0.0;    // the gate's relative budgets are computed from them
	std::map<std::string, std::vector<double>> series;   // matcher, new_frame, extra, repeat, critical, ...
	std::map<std::string, double> passPerSourceFrame;     // GPU ms per source frame, all its refreshes
};
static std::vector<ScenarioTimingRecord>* g_scenarioSink = nullptr;

// The motion corpus and its failure map (owner 2026-10-13; docs/VP-stage11b-
// learned-candidates.md section 2, synthetic part): scenes rendered from an
// exact camera / object model at 3840x2160 (the default half-size matcher),
// so each pixel's true motion and the true midpoint picture are known. Per
// scene: six frames estimated in order (warm-up as in VP), the last pair
// scored: the warp at 0.5 against the true midpoint (and the hold), the
// trusted blocks' vector error, the share trusted, the trusted blocks that
// are wrong (> 2 px). Accuracy only: D2/D4 off (the strength is taste). Today's
// settings and 17 neighbours (match tolerance x E1 lambda x matcher size) give
// the headroom: today, the best single setting, the best per scene. Report
// only: VP_TEST_MOTION_CORPUS=1 (VP_TEST_GPU=hardware); results CSV and worst
// crops in VP_TEST_CORPUS_DIR.
namespace Corpus
{
	struct Obj
	{
		int kind = 0;          // 0 a patch of the canvas, 1 a thin bright bar, 2 a fence (bars every srcX px)
		float x0 = 0, y0 = 0, vx = 0, vy = 0, w = 0, h = 0, srcX = 0, srcY = 0;
	};
	struct Scene
	{
		std::string cls, name;
		float panX = 0, panY = 0, zoom = 0, rot = 0, fade = 0, grain = 0, level = 1, fencePeriod = 0;
		bool cut = false, blur = false, subtitle = false;
		std::vector<Obj> objects;
	};

	const int W = 3840, H = 2160;
	const float CX = W / 2.0f, CY = H / 2.0f;
	const float OX = 1280.0f, OY = 720.0f;   // the frame's place on the canvas at t = 0

	float Sample(const std::vector<uint16_t>& plane, int pw, int ph, float x, float y)
	{
		x = std::min(std::max(x, 0.0f), pw - 1.001f);
		y = std::min(std::max(y, 0.0f), ph - 1.001f);
		const int ix = static_cast<int>(x), iy = static_cast<int>(y);
		const float tx = x - ix, ty = y - iy;
		const uint16_t* r0 = &plane[static_cast<size_t>(iy) * pw + ix];
		const uint16_t* r1 = r0 + pw;
		return (r0[0] * (1 - tx) + r0[1] * tx) * (1 - ty) + (r1[0] * (1 - tx) + r1[1] * tx) * ty;
	}

	// Screen to canvas (the camera at time t) and back.
	void ToWorld(const Scene& s, float px, float py, float t, float& wx, float& wy)
	{
		const float scale = std::pow(1.0f + s.zoom, t);
		const float a = -s.rot * t * 3.14159265f / 180.0f;
		const float dx = (px - CX) / scale, dy = (py - CY) / scale;
		wx = dx * std::cos(a) - dy * std::sin(a) + CX + OX - s.panX * t;
		wy = dx * std::sin(a) + dy * std::cos(a) + CY + OY - s.panY * t;
	}
	void ToScreen(const Scene& s, float wx, float wy, float t, float& px, float& py)
	{
		const float scale = std::pow(1.0f + s.zoom, t);
		const float a = s.rot * t * 3.14159265f / 180.0f;
		const float dx = wx - CX - OX + s.panX * t, dy = wy - CY - OY + s.panY * t;
		px = (dx * std::cos(a) - dy * std::sin(a)) * scale + CX;
		py = (dx * std::sin(a) + dy * std::cos(a)) * scale + CY;
	}
	int ObjectAt(const Scene& s, float px, float py, float t)
	{
		for (int k = static_cast<int>(s.objects.size()) - 1; k >= 0; --k)
		{
			const Obj& o = s.objects[k];
			const float x = o.x0 + o.vx * t, y = o.y0 + o.vy * t;
			if (px >= x && px < x + o.w && py >= y && py < y + o.h)
				return k;
		}
		return -1;
	}
	uint32_t Hash(uint32_t x, uint32_t y, uint32_t z)
	{
		uint32_t v = x * 73856093u ^ y * 19349663u ^ z * 83492791u;
		v ^= v >> 13; v *= 0x5bd1e995u; v ^= v >> 15;
		return v;
	}

	// One frame at time t (grain seeded by `seed`): 16-bit luma, interleaved CbCr.
	void Render(const Scene& s, const Canvas& c, float t, uint32_t seed, std::vector<uint16_t>& luma, std::vector<uint16_t>& chroma)
	{
		luma.assign(static_cast<size_t>(W) * H, 0);
		chroma.assign(static_cast<size_t>(W / 2) * (H / 2) * 2, 0);
		const float sceneT = s.cut && t >= 4.75f ? t + 37.0f : t;   // a cut: another place on the canvas
		const float offsetX = s.cut && t >= 4.75f ? 900.0f : 0.0f;
		auto pixel = [&](float px, float py, float tt, float& l, float& cb, float& cr)
		{
			const int k = ObjectAt(s, px, py, tt);
			if (k >= 0)
			{
				const Obj& o = s.objects[k];
				const float u = px - (o.x0 + o.vx * tt), v = py - (o.y0 + o.vy * tt);
				if (o.kind == 1) { l = 56000.0f + 2000.0f * ((static_cast<int>(v) / 6) % 2); cb = cr = 32768.0f; return; }
				if (o.kind == 2)
				{
					// Bars 45% of the period wide; between them the scene behind.
					const float phase = std::fmod(u, o.srcX);
					if ((phase < 0 ? phase + o.srcX : phase) < o.srcX * 0.45f) { l = 50000.0f + 0.05f * v; cb = cr = 32768.0f; return; }
					float wx, wy;
					ToWorld(s, px, py, tt, wx, wy);
					l = Sample(c.luma, c.w, c.h, wx, wy);
					cb = Sample(c.cb, c.w / 2, c.h / 2, wx / 2, wy / 2);
					cr = Sample(c.cr, c.w / 2, c.h / 2, wx / 2, wy / 2);
					return;
				}
				l = Sample(c.luma, c.w, c.h, o.srcX + u, o.srcY + v);
				cb = Sample(c.cb, c.w / 2, c.h / 2, (o.srcX + u) / 2, (o.srcY + v) / 2);
				cr = Sample(c.cr, c.w / 2, c.h / 2, (o.srcX + u) / 2, (o.srcY + v) / 2);
				return;
			}
			float wx, wy;
			ToWorld(s, px, py, s.cut ? sceneT : tt, wx, wy);
			wx += offsetX;
			if (s.fencePeriod > 0)
			{
				const float phase = std::fmod(wx, s.fencePeriod);
				const bool bar = (phase < 0 ? phase + s.fencePeriod : phase) < s.fencePeriod * 0.45f;
				l = (bar ? 50000.0f : 9000.0f) + 0.08f * Sample(c.luma, c.w, c.h, wx, wy);
				cb = cr = 32768.0f;
				return;
			}
			l = Sample(c.luma, c.w, c.h, wx, wy);
			cb = Sample(c.cb, c.w / 2, c.h / 2, wx / 2, wy / 2);
			cr = Sample(c.cr, c.w / 2, c.h / 2, wx / 2, wy / 2);
		};
		const float level = s.level * (1.0f - s.fade * t);
		auto shade = [&](float px, float py, float& l, float& cb, float& cr)
		{
			if (s.blur)
			{
				float sl = 0, scb = 0, scr = 0;
				for (float d : { -0.1875f, -0.0625f, 0.0625f, 0.1875f })
				{
					float a, b2, c2;
					pixel(px, py, t + d, a, b2, c2);
					sl += a; scb += b2; scr += c2;
				}
				l = sl / 4; cb = scb / 4; cr = scr / 4;
			}
			else
				pixel(px, py, t, l, cb, cr);
			l = 4096.0f + (l - 4096.0f) * level;
			cb = 32768.0f + (cb - 32768.0f) * level;
			cr = 32768.0f + (cr - 32768.0f) * level;
		};
		const int threads = std::max(2u, std::thread::hardware_concurrency());
		std::vector<std::thread> pool;
		for (int th = 0; th < threads; ++th)
			pool.emplace_back([&, th]()
			{
				for (int y = th; y < H; y += threads)
					for (int x = 0; x < W; ++x)
					{
						float l, cb, cr;
						shade(x + 0.5f, y + 0.5f, l, cb, cr);
						if (s.subtitle && y >= 1880 && y < 2000 && x >= 1120 && x < 2720)
						{
							// White strokes with a black edge, fixed on the screen.
							const int gx = (x - 1120) / 6, gy = (y - 1880) / 8;
							const bool stroke = (Hash(gx / 5, gy / 3, 7) % 3) == 0 && ((x - 1120) % 30) < 24 && ((y - 1880) % 120) > 16 && ((y - 1880) % 120) < 104;
							if (stroke) l = 60000.0f;
						}
						if (s.grain > 0)
							l += (static_cast<float>(Hash(x, y, seed) & 0xFFFF) / 65535.0f - 0.5f) * 2.0f * s.grain;
						luma[static_cast<size_t>(y) * W + x] = static_cast<uint16_t>(std::min(65535.0f, std::max(0.0f, l)));
						if ((x & 1) == 0 && (y & 1) == 0)
						{
							const size_t ci = (static_cast<size_t>(y / 2) * (W / 2) + x / 2) * 2;
							chroma[ci] = static_cast<uint16_t>(std::min(65535.0f, std::max(0.0f, cb)));
							chroma[ci + 1] = static_cast<uint16_t>(std::min(65535.0f, std::max(0.0f, cr)));
						}
					}
			});
		for (auto& t2 : pool) t2.join();
	}

	// The true motion from t to t + 1, per 16x16 block (the field's grid),
	// from a 4x4 sample of each block; NaN where the block is cut.
	std::vector<float> TrueField(const Scene& s, float t, int blocksX, int blocksY)
	{
		std::vector<float> v(static_cast<size_t>(blocksX) * blocksY * 2, 0.0f);
		for (int by = 0; by < blocksY; ++by)
			for (int bx = 0; bx < blocksX; ++bx)
			{
				float sx = 0, sy = 0;
				for (int j = 0; j < 4; ++j)
					for (int i = 0; i < 4; ++i)
					{
						const float px = bx * 16 + 2 + i * 4, py = by * 16 + 2 + j * 4;
						const int k = ObjectAt(s, px, py, t);
						if (k >= 0) { sx += s.objects[k].vx; sy += s.objects[k].vy; continue; }
						float wx, wy, qx, qy;
						ToWorld(s, px, py, t, wx, wy);
						ToScreen(s, wx, wy, t + 1, qx, qy);
						sx += qx - px; sy += qy - py;
					}
				v[(static_cast<size_t>(by) * blocksX + bx) * 2] = s.cut ? NAN : sx / 16;
				v[(static_cast<size_t>(by) * blocksX + bx) * 2 + 1] = s.cut ? NAN : sy / 16;
			}
		return v;
	}

	// The settings: today (renderer defaults, accuracy only) and neighbours.
	// "half size": dejudder_low_res all (today); "half-size search": the
	// search at half size, the rest at full size (dejudder_low_res search).
	struct Variant
	{
		std::string name; float tolerance, lambda; bool lowResAll;
		float textureTol = 0.0f; bool globalTwo = false, regional = false; int coarseRange = 3; float consistency = 1.0f;
		bool taper = false; float taperSad = 0.0f, splitSad = 0.0f;
		bool fade = false, split = true, verify = false; float verifyShare = 0.2f;
		bool fill = false; float minShare = 0.25f;
		bool pixel = false; float pixLo = 1.0f, pixHi = 2.5f;
		bool opticalFlow = false; int ofPreset = 1; bool ofCandidate = false, ofSeed = false, ofGlobal = false;   // docs/VP-nvof-ahead.md: the engine as the motion source
		bool rolloff = false; float pixLoU = 0.0f, pixHiU = 0.0f; bool pixAll = false;
		float rollHi = 24.0f, rollFloor = 0.25f;
		float ownTexture = 0.0f, ownError = 3.0f; bool crossfade = false;
		bool taste = false;   // D2 / D4 / R-A as deployed; scored where the picture is actually shown
		float fastFrom = 0.0f, fastFloor = 0.4f;
	};
	// VP_TEST_CORPUS_SET=fixes: today and the motion fixes (M1 texture
	// tolerance, M2 two global candidates, M3 regional hold, a wider coarse
	// search), alone and together; otherwise the settings grid.
	std::vector<Variant> Variants()
	{
		std::vector<Variant> variants;
		char* set = nullptr;
		size_t n = 0;
		const std::string which = _dupenv_s(&set, &n, "VP_TEST_CORPUS_SET") == 0 && set ? set : "";
		std::free(set);
		const bool fixes = which == "fixes";
		if (which == "opticalflow")
		{
			// docs/VP-nvof-ahead.md (owner 2026-10-16: the engine the established default):
			// the deployed matcher settings with the block searches against the optical-flow
			// engine (each preset), accuracy first, then with the deployed taste (D2/D4/R-A)
			// scored where shown. The trust pipeline after the source is the same.
			auto add = [&](const char* name, bool of, int preset, bool taste, bool candidate)
			{
				Variant v{ name, 4.5f, 0.5f, true };
				v.globalTwo = true; v.regional = true; v.coarseRange = 8; v.consistency = 2.0f;
				v.textureTol = 0.25f; v.fade = true; v.split = false; v.pixel = true; v.pixLo = 3.0f; v.pixHi = 8.0f;
				v.opticalFlow = of; v.ofPreset = preset; v.ofCandidate = candidate;
				if (taste) { v.taste = true; v.rolloff = true; }
				variants.push_back(v);
			};
			add("matcher (block searches), defaults", false, 1, false, false);
			add("optical flow instead, medium", true, 1, false, false);
			add("optical flow instead, slow", true, 0, false, false);
			add("matcher + optical flow candidate, medium", true, 1, false, true);
			add("matcher + optical flow candidate, fast", true, 2, false, true);
			add("matcher + optical flow candidate, slow", true, 0, false, true);
			{
				add("optical flow seed + matcher refinement, medium", true, 1, false, true);
				variants.back().ofSeed = true;
				add("optical flow seed + matcher refinement, slow", true, 0, false, true);
				variants.back().ofSeed = true;
				// Planner Q1: D1 from the engine's field instead of the global search.
				add("optical flow seed, slow, engine global", true, 0, false, true);
				variants.back().ofSeed = true; variants.back().ofGlobal = true;
				// The taste uses D1 (the roll-off, the camera): both with it.
				add("optical flow seed, slow, D2/D4/R-A", true, 0, true, true);
				variants.back().ofSeed = true;
				add("optical flow seed, slow, engine global, D2/D4/R-A", true, 0, true, true);
				variants.back().ofSeed = true; variants.back().ofGlobal = true;
			}
			add("matcher, D2/D4/R-A", false, 1, true, false);
			add("matcher + OF candidate medium, D2/D4/R-A", true, 1, true, true);
			return variants;
		}
		if (which == "d1")
		{
			// Planner D1: the damage with R-A and D2/D4 on (as deployed). Each
			// in-between picture is scored where it is shown: the warp's
			// effective shift (shift x the strength) against the true frame at
			// that position; positions never shown are skipped.
			auto add = [&](const char* name, bool pixel, bool rolloff, float rollHi, float rollFloor)
			{
				Variant v{ name, 4.5f, 0.5f, true };
				v.globalTwo = true; v.regional = true; v.coarseRange = 8; v.consistency = 2.0f;
				v.textureTol = 0.25f; v.fade = true; v.split = false; v.pixel = pixel; v.pixLo = 3.0f; v.pixHi = 8.0f;
				v.taste = true; v.rolloff = rolloff; v.rollHi = rollHi; v.rollFloor = rollFloor;
				variants.push_back(v);
			};
			Variant today{ "today (before 2026-10-13), D2/D4/R-A", 3.0f, 0.5f, true };
			today.taste = true; today.rolloff = true;
			variants.push_back(today);
			add("M6 off (block decision), D2/D4/R-A", false, true, 24.0f, 0.25f);
			add("M6 on (defaults), D2/D4/R-A", true, true, 24.0f, 0.25f);
			add("M6 on, D2/D4, no R-A", true, false, 24.0f, 0.25f);
			add("M6 on, R-A floor 0.1", true, true, 24.0f, 0.1f);
			add("M6 on, R-A hi 16", true, true, 16.0f, 0.25f);
			return variants;
		}
		if (which == "fast")
		{
			// D1 follow-up: M6's thresholds shrink with the motion (deployed
			// taste: D2 / D4 / R-A on; scored where shown).
			auto add = [&](const char* name, bool pixel, float from, float floor)
			{
				Variant v{ name, 4.5f, 0.5f, true };
				v.globalTwo = true; v.regional = true; v.coarseRange = 8; v.consistency = 2.0f;
				v.textureTol = 0.25f; v.fade = true; v.split = false; v.pixel = pixel; v.pixLo = 3.0f; v.pixHi = 8.0f;
				v.taste = true; v.rolloff = true; v.fastFrom = from; v.fastFloor = floor;
				variants.push_back(v);
			};
			Variant today{ "today (before 2026-10-13), D2/D4/R-A", 3.0f, 0.5f, true };
			today.taste = true; today.rolloff = true;
			variants.push_back(today);
			add("M6 off (block decision)", false, 0.0f, 0.4f);
			add("M6 on", true, 0.0f, 0.4f);
			add("M6, fast from 24 px to 0.4", true, 24.0f, 0.4f);
			add("M6, fast from 16 px to 0.3", true, 16.0f, 0.3f);
			add("M6, fast from 32 px to 0.25", true, 32.0f, 0.25f);
			add("M6, fast from 12 px to 0.2", true, 12.0f, 0.2f);
			return variants;
		}
		if (which == "pixmotion")
		{
			// M6: a partial weight as a share of the motion (the default) or as
			// a crossfade with the held pixel.
			Variant today{ "today (before tonight)", 3.0f, 0.5f, true };
			variants.push_back(today);
			Variant proposed{ "defaults, partial = motion share", 4.5f, 0.5f, true };
			proposed.globalTwo = true; proposed.regional = true; proposed.coarseRange = 8; proposed.consistency = 2.0f;
			proposed.textureTol = 0.25f; proposed.fade = true; proposed.split = false; proposed.pixel = true; proposed.pixLo = 3.0f; proposed.pixHi = 8.0f;
			variants.push_back(proposed);
			Variant cross = proposed;
			cross.name = "defaults, partial = crossfade";
			cross.crossfade = true;
			variants.push_back(cross);
			Variant more = proposed;
			more.name = "more (tolerance 8), motion share";
			more.tolerance = 8.0f; more.pixLo = 1.7f; more.pixHi = 4.5f;
			variants.push_back(more);
			return variants;
		}
		if (which == "final")
		{
			// The comparison for the defaults: today (before tonight), the
			// first set of motion-fix defaults, the proposed defaults, and the
			// proposed defaults with failed blocks' own vectors where detailed.
			Variant today{ "today (before tonight)", 3.0f, 0.5f, true };
			variants.push_back(today);
			Variant safe{ "first motion-fix defaults", 3.0f, 0.5f, true };
			safe.globalTwo = true; safe.regional = true; safe.coarseRange = 8; safe.consistency = 2.0f;
			variants.push_back(safe);
			Variant proposed = safe;
			proposed.name = "proposed defaults";
			proposed.tolerance = 4.5f; proposed.textureTol = 0.25f; proposed.fade = true; proposed.split = false;
			proposed.pixel = true; proposed.pixLo = 3.0f; proposed.pixHi = 8.0f;
			variants.push_back(proposed);
			Variant more = proposed;
			more.name = "more: proposed with tolerance 8";
			more.tolerance = 8.0f; more.pixLo = 1.7f; more.pixHi = 4.5f;
			variants.push_back(more);
			Variant own = proposed;
			own.name = "proposed + own vectors (detail 10, error 4x)";
			own.ownTexture = 10.0f; own.ownError = 4.0f;
			variants.push_back(own);
			return variants;
		}
		if (which == "pixloose")
		{
			// M6 (trusted vectors) with a looser block test: the pixel weights
			// filter what the block test lets through.
			auto add = [&](const char* name, float tol, float consistency, float tex, float lo, float hi)
			{
				Variant v{ name, tol, 0.5f, true };
				v.globalTwo = true; v.regional = true; v.coarseRange = 8; v.consistency = consistency;
				v.textureTol = tex; v.fade = true; v.split = false; v.pixel = true; v.pixLo = lo; v.pixHi = hi;
				variants.push_back(v);
			};
			add("pixel, tol 4.5 (candidate)", 4.5f, 2.0f, 0.25f, 3.0f, 8.0f);
			add("pixel, tol 6", 6.0f, 2.0f, 0.25f, 2.25f, 6.0f);
			add("pixel, tol 8", 8.0f, 2.0f, 0.25f, 1.7f, 4.5f);
			add("pixel, tol 6, agreement 3", 6.0f, 3.0f, 0.25f, 2.25f, 6.0f);
			add("pixel, tol 8, agreement 3", 8.0f, 3.0f, 0.25f, 1.7f, 4.5f);
			return variants;
		}
		if (which == "pixown")
		{
			// M6: failed blocks' own vectors where detailed with a fair match.
			auto add = [&](const char* name, float tex, float err)
			{
				Variant v{ name, 4.5f, 0.5f, true };
				v.globalTwo = true; v.regional = true; v.coarseRange = 8; v.consistency = 2.0f;
				v.textureTol = 0.25f; v.fade = true; v.split = false; v.pixel = true;
				v.pixLo = 3.0f; v.pixHi = 8.0f; v.ownTexture = tex; v.ownError = err;
				variants.push_back(v);
			};
			add("pixel trusted vectors", 0.0f, 3.0f);
			add("+ own where detail 3, error 3x", 3.0f, 3.0f);
			add("+ own where detail 6, error 3x", 6.0f, 3.0f);
			add("+ own where detail 6, error 2x", 6.0f, 2.0f);
			add("+ own where detail 10, error 4x", 10.0f, 4.0f);
			return variants;
		}
		if (which == "pixtrusted")
		{
			// M6 with trusted blocks' vectors only (a failed block follows its
			// trusted neighbours) against every block's.
			auto add = [&](const char* name, float lo, float hi, float loU, float hiU, bool all)
			{
				Variant v{ name, 4.5f, 0.5f, true };
				v.globalTwo = true; v.regional = true; v.coarseRange = 8; v.consistency = 2.0f;
				v.textureTol = 0.25f; v.fade = true; v.split = false; v.pixel = true;
				v.pixLo = lo; v.pixHi = hi; v.pixLoU = loU; v.pixHiU = hiU; v.pixAll = all;
				variants.push_back(v);
			};
			Variant cand{ "candidate", 4.5f, 0.5f, true };
			cand.globalTwo = true; cand.regional = true; cand.coarseRange = 8; cand.consistency = 2.0f;
			cand.textureTol = 0.25f; cand.fade = true; cand.split = false;
			variants.push_back(cand);
			add("pixel 3-8/1.5-4, every block's vectors", 3.0f, 8.0f, 1.5f, 4.0f, true);
			add("pixel 3-8/1.5-4, trusted vectors", 3.0f, 8.0f, 1.5f, 4.0f, false);
			add("pixel 3-8/3-8, trusted vectors", 3.0f, 8.0f, 0.0f, 0.0f, false);
			add("pixel 2-6/1-2.5, trusted vectors", 2.0f, 6.0f, 1.0f, 2.5f, false);
			return variants;
		}
		if (which == "tier")
		{
			// M6 in two tiers: lenient inside blocks that passed, strict outside.
			auto add = [&](const char* name, float lo, float hi, float loU, float hiU)
			{
				Variant v{ name, 4.5f, 0.5f, true };
				v.globalTwo = true; v.regional = true; v.coarseRange = 8; v.consistency = 2.0f;
				v.textureTol = 0.25f; v.fade = true; v.split = false; v.pixel = true;
				v.pixLo = lo; v.pixHi = hi; v.pixLoU = loU; v.pixHiU = hiU;
				variants.push_back(v);
			};
			Variant cand{ "candidate", 4.5f, 0.5f, true };
			cand.globalTwo = true; cand.regional = true; cand.coarseRange = 8; cand.consistency = 2.0f;
			cand.textureTol = 0.25f; cand.fade = true; cand.split = false;
			variants.push_back(cand);
			add("pixel 2-6", 2.0f, 6.0f, 0.0f, 0.0f);
			add("pixel 2-6 / untrusted 1-2.5", 2.0f, 6.0f, 1.0f, 2.5f);
			add("pixel 2-6 / untrusted 0.75-2", 2.0f, 6.0f, 0.75f, 2.0f);
			add("pixel 3-8 / untrusted 1-2.5", 3.0f, 8.0f, 1.0f, 2.5f);
			add("pixel 3-8 / untrusted 1.5-4", 3.0f, 8.0f, 1.5f, 4.0f);
			return variants;
		}
		if (which == "pixel2")
		{
			// M6 with the fixed-point lookup, and with R-A as deployed (a
			// fast pan at a quarter of the strength).
			auto add = [&](const char* name, float tol, bool pixel, float lo, float hi, bool rolloff)
			{
				Variant v{ name, tol, 0.5f, true };
				v.globalTwo = true; v.regional = true; v.coarseRange = 8; v.consistency = 2.0f;
				v.textureTol = 0.25f; v.fade = true; v.split = false; v.pixel = pixel; v.pixLo = lo; v.pixHi = hi; v.rolloff = rolloff;
				variants.push_back(v);
			};
			add("candidate", 4.5f, false, 1.0f, 2.5f, false);
			add("candidate + R-A", 4.5f, false, 1.0f, 2.5f, true);
			add("pixel trust 1.5-4", 4.5f, true, 1.5f, 4.0f, false);
			add("pixel trust 2-6", 4.5f, true, 2.0f, 6.0f, false);
			add("pixel trust 1.5-4 + R-A", 4.5f, true, 1.5f, 4.0f, true);
			add("pixel trust 2-6 + R-A", 4.5f, true, 2.0f, 6.0f, true);
			return variants;
		}
		if (which == "pixel")
		{
			// M6: per-pixel trust on every block's vector.
			auto add = [&](const char* name, float tol, bool pixel, float lo, float hi)
			{
				Variant v{ name, tol, 0.5f, true };
				v.globalTwo = true; v.regional = true; v.coarseRange = 8; v.consistency = 2.0f;
				v.textureTol = 0.25f; v.fade = true; v.split = false; v.pixel = pixel; v.pixLo = lo; v.pixHi = hi;
				variants.push_back(v);
			};
			add("candidate", 4.5f, false, 1.0f, 2.5f);
			add("candidate + pixel trust 1-2.5", 4.5f, true, 1.0f, 2.5f);
			add("candidate + pixel trust 0.5-1.5", 4.5f, true, 0.5f, 1.5f);
			add("candidate + pixel trust 1.5-4", 4.5f, true, 1.5f, 4.0f);
			add("candidate + pixel trust 2-6", 4.5f, true, 2.0f, 6.0f);
			add("tol 3 + pixel trust 1.5-4", 3.0f, true, 1.5f, 4.0f);
			Variant none{ "no trust test (every block warps)", 255.0f, 0.5f, true };
			none.globalTwo = true; none.regional = true; none.coarseRange = 8; none.consistency = 1000.0f;
			none.textureTol = 0.25f; none.fade = true; none.split = false; none.minShare = 0.0f;
			variants.push_back(none);
			return variants;
		}
		if (which == "bound")
		{
			// How good the block vectors are apart from the trust decision:
			// looser and looser trust, up to none (every block warps).
			auto add = [&](const char* name, float tol, float consistency, float minShare)
			{
				Variant v{ name, tol, 0.5f, true };
				v.globalTwo = true; v.regional = true; v.coarseRange = 8; v.consistency = consistency;
				v.textureTol = 0.25f; v.fade = true; v.split = false; v.minShare = minShare;
				variants.push_back(v);
			};
			add("candidate", 4.5f, 2.0f, 0.25f);
			add("tol 12, agreement 4", 12.0f, 4.0f, 0.25f);
			add("tol 24, agreement 8", 24.0f, 8.0f, 0.0f);
			add("no trust test (every block warps)", 255.0f, 1000.0f, 0.0f);
			return variants;
		}
		if (which == "fill")
		{
			// D5: B where A's content is uncovered (source warp), on the candidate.
			auto add = [&](const char* name, float tol, bool verify, bool fill)
			{
				Variant v{ name, tol, 0.5f, true };
				v.globalTwo = true; v.regional = true; v.coarseRange = 8; v.consistency = 2.0f;
				v.textureTol = 0.25f; v.fade = true; v.split = false; v.verify = verify; v.fill = fill;
				variants.push_back(v);
			};
			add("candidate", 4.5f, false, false);
			add("candidate + D5 fill", 4.5f, false, true);
			add("candidate + verify + D5 fill", 4.5f, true, true);
			add("tol 6 + verify + D5 fill", 6.0f, true, true);
			return variants;
		}
		if (which == "verify")
		{
			// M5: the pixel verification on the candidate defaults and on a
			// looser tolerance.
			auto add = [&](const char* name, float tol, bool verify, float share)
			{
				Variant v{ name, tol, 0.5f, true };
				v.globalTwo = true; v.regional = true; v.coarseRange = 8; v.consistency = 2.0f;
				v.textureTol = 0.25f; v.fade = true; v.split = false; v.verify = verify; v.verifyShare = share;
				variants.push_back(v);
			};
			Variant today{ "today", 3.0f, 0.5f, true };
			variants.push_back(today);
			add("candidate (tol 4.5, M1 0.25, fade, split off)", 4.5f, false, 0.2f);
			add("candidate + verify 0.2", 4.5f, true, 0.2f);
			add("candidate + verify 0.1", 4.5f, true, 0.1f);
			add("tol 6, M1 0.25, fade, split off", 6.0f, false, 0.2f);
			add("tol 6 ... + verify 0.2", 6.0f, true, 0.2f);
			add("tol 6 ... + verify 0.1", 6.0f, true, 0.1f);
			add("tol 8 ... + verify 0.2", 8.0f, true, 0.2f);
			return variants;
		}
		if (which == "fade")
		{
			// M4: the fade match (floor 1 here: accuracy, not taste).
			auto add = [&](const char* name, float tol, float tex, bool fade)
			{
				Variant v{ name, tol, 0.5f, true };
				v.globalTwo = true; v.regional = true; v.coarseRange = 8; v.consistency = 2.0f;
				v.textureTol = tex; v.fade = fade;
				variants.push_back(v);
			};
			add("defaults", 3.0f, 0.0f, false);
			add("defaults + fade match", 3.0f, 0.0f, true);
			add("more trust", 4.5f, 0.25f, false);
			add("more trust + fade match", 4.5f, 0.25f, true);
			add("tolerance 6 + M1 0.25 + fade match", 6.0f, 0.25f, true);
			add("more trust + fade match, split off", 4.5f, 0.25f, true);
			variants.back().split = false;
			add("defaults, split off", 3.0f, 0.0f, false);
			variants.back().split = false;
			return variants;
		}
		if (which == "held")
		{
			// After the defaults: what the held areas allow (owner 2026-10-13
			// night): H3 marginal blocks warped at a strength ramped by their
			// match, a lower split threshold (smaller blocks for shapes that
			// change), and the "more trust" A/B with them.
			auto add = [&](const char* name, float tol, float tex, bool taper, float taperSad, float splitSad)
			{
				Variant v{ name, tol, 0.5f, true };
				v.globalTwo = true; v.regional = true; v.coarseRange = 8; v.consistency = 2.0f;
				v.textureTol = tex; v.taper = taper; v.taperSad = taperSad; v.splitSad = splitSad;
				variants.push_back(v);
			};
			add("defaults", 3.0f, 0.0f, false, 0.0f, 0.0f);
			add("+ taper to 2x tolerance", 3.0f, 0.0f, true, 0.0f, 0.0f);
			add("+ taper to 3x tolerance", 3.0f, 0.0f, true, 9.0f, 0.0f);
			add("+ split at 2", 3.0f, 0.0f, false, 0.0f, 2.0f);
			add("+ split at 1.5", 3.0f, 0.0f, false, 0.0f, 1.5f);
			add("more trust", 4.5f, 0.25f, false, 0.0f, 0.0f);
			add("more trust + taper 2x", 4.5f, 0.25f, true, 0.0f, 0.0f);
			add("more trust + split at 2", 4.5f, 0.25f, false, 0.0f, 2.0f);
			add("more trust + taper 2x + split at 2", 4.5f, 0.25f, true, 0.0f, 2.0f);
			return variants;
		}
		if (which == "defaults")
		{
			// The candidates for the defaults: the fixes that add no wrongly
			// trusted blocks, then with the tolerance / texture allowance.
			auto add = [&](const char* name, float tol, float tex)
			{
				Variant v{ name, tol, 0.5f, true };
				v.textureTol = tex; v.globalTwo = true; v.regional = true; v.coarseRange = 8; v.consistency = 2.0f;
				variants.push_back(v);
			};
			Variant today{ "today", 3.0f, 0.5f, true };
			variants.push_back(today);
			add("safe set (M2 + M3 + range 8 + consistency 2)", 3.0f, 0.0f);
			add("safe set + M1 0.25", 3.0f, 0.25f);
			add("safe set + tolerance 4.5", 4.5f, 0.0f);
			add("safe set + tolerance 4.5 + M1 0.25", 4.5f, 0.25f);
			return variants;
		}
		if (fixes)
		{
			auto add = [&](const char* name, float tol, float tex, bool two, bool regional, int range, float consistency)
			{
				Variant v{ name, tol, 0.5f, true };
				v.textureTol = tex; v.globalTwo = two; v.regional = regional; v.coarseRange = range; v.consistency = consistency;
				variants.push_back(v);
			};
			add("today", 3.0f, 0.0f, false, false, 3, 1.0f);
			add("tolerance 4.5", 4.5f, 0.0f, false, false, 3, 1.0f);
			add("M1 texture 0.25", 3.0f, 0.25f, false, false, 3, 1.0f);
			add("M1 texture 0.5", 3.0f, 0.5f, false, false, 3, 1.0f);
			add("M2 two global", 3.0f, 0.0f, true, false, 3, 1.0f);
			add("M3 regional hold", 3.0f, 0.0f, false, true, 3, 1.0f);
			add("coarse range 8", 3.0f, 0.0f, false, false, 8, 1.0f);
			add("consistency 2", 3.0f, 0.0f, false, false, 3, 2.0f);
			add("M1 0.5 + M2 + M3 + range 8 + consistency 2", 3.0f, 0.5f, true, true, 8, 2.0f);
			add("tol 4.5 + M1 0.5 + M2 + M3 + range 8 + consistency 2", 4.5f, 0.5f, true, true, 8, 2.0f);
			return variants;
		}
		for (bool all : { true, false })
			for (float tol : { 3.0f, 2.0f, 4.5f })
				for (float lam : { 0.5f, 0.25f, 1.0f })
				{
					char text[96];
					std::snprintf(text, sizeof(text), "tolerance %.1f, lambda %.2f, matcher %s", tol, lam, all ? "half size" : "half-size search");
					variants.push_back({ text, tol, lam, all });
				}
		return variants;
	}
	Dejudder::Settings SettingsFor(const Variant& v)
	{
		Dejudder::Settings s = ChainGpuProbe::Refined(Dejudder::Settings{});
		s.matchTolerance = v.tolerance;
		s.minTrustedShare = 0.25f;
		s.estLambda = v.lambda;
		s.lowRes = true;
		s.lowResAll = v.lowResAll;
		s.textureTolerance = v.textureTol;
		s.globalTwo = v.globalTwo;
		s.regionalHold = v.regional;
		s.coarseRange = v.coarseRange;
		s.consistencyPixels = v.consistency;
		s.blockTaper = v.taper;
		s.taperSad = v.taperSad;
		s.estSplitSad = v.splitSad;
		s.fadeMatch = v.fade;
		s.estSplit = v.split;   // the variant decides (the renderer default is off)
		s.verifyPixels = v.verify;
		s.verifyShare = v.verifyShare;
		s.occlusionFill = v.fill;
		s.minTrustedShare = v.minShare;
		s.pixelTrust = v.pixel;
		s.opticalFlow = v.opticalFlow;
		s.opticalFlowPreset = v.ofPreset;
		s.opticalFlowCandidate = v.ofCandidate;
		s.opticalFlowSeed = v.ofSeed;
		s.opticalFlowGlobal = v.ofGlobal;
		s.opticalFlowGlobalReadback = v.ofGlobal;   // no producer in the corpus runs
		s.pixelTrustLo = v.pixLo;
		s.pixelTrustHi = v.pixHi;
		s.pixelTrustLoUntrusted = v.pixLoU;
		s.pixelTrustHiUntrusted = v.pixHiU;
		s.pixelTrustedVectors = !v.pixAll;
		s.pixelOwnTexture = v.ownTexture;
		s.pixelTrustMotion = !v.crossfade;
		s.pixelFastFrom = v.fastFrom;
		s.pixelFastFloor = v.fastFloor;
		s.pixelOwnError = v.ownError;
		if (v.rolloff)
		{
			s.motionRolloff = true;
			s.rollLo = 8.0f;
			s.rollHi = v.rollHi;
			s.rollFloor = v.rollFloor;
		}
		if (v.taste)
		{
			// The renderer's D2 / D4 and the fade floor (taste, as deployed).
			s.motionScaleLo = 0.25f;
			s.motionScaleHi = 1.5f;
			s.trustScaleFull = 0.6f;
			s.fadeFloor = 0.25f;
		}
		s.fadeFloor = 1.0f;
		s.motionScaleHi = 0.0f;   // accuracy only: D2 and D4 off
		s.trustScaleFull = 0.0f;
		s.hostReadable = true;
		return s;
	}

	std::vector<Scene> Scenes()
	{
		std::vector<Scene> list;
		auto add = [&](const char* cls, const char* name, std::function<void(Scene&)> set)
		{
			Scene s; s.cls = cls; s.name = name; set(s); list.push_back(s);
		};
		add("static", "still", [](Scene&) {});
		add("static", "still, grain", [](Scene& s) { s.grain = 900; });
		add("slow pan", "1 px", [](Scene& s) { s.panX = 1; });
		add("slow pan", "2.5 px", [](Scene& s) { s.panX = 2.5f; s.panY = 0.5f; });
		add("medium pan", "6 px", [](Scene& s) { s.panX = 6; s.panY = 1; });
		add("medium pan", "12 px", [](Scene& s) { s.panX = 12; });
		add("fast pan", "20 px", [](Scene& s) { s.panX = 20; s.panY = 3; });
		add("fast pan", "32 px", [](Scene& s) { s.panX = -32; });
		add("whip pan", "48 px", [](Scene& s) { s.panX = 48; });
		add("whip pan", "80 px", [](Scene& s) { s.panX = -80; s.panY = 6; });
		add("diagonal / vertical", "8, 8", [](Scene& s) { s.panX = 8; s.panY = 8; });
		add("diagonal / vertical", "0, 10", [](Scene& s) { s.panY = 10; });
		add("zoom", "in 1%", [](Scene& s) { s.zoom = 0.01f; });
		add("zoom", "out 1.5%", [](Scene& s) { s.zoom = -0.015f; });
		add("rotation", "0.4 deg", [](Scene& s) { s.rot = 0.4f; });
		add("rotation", "-0.8 deg", [](Scene& s) { s.rot = -0.8f; });
		add("object, still background", "12 px", [](Scene& s) { s.objects.push_back({ 0, 1400, 700, 12, 0, 420, 360, 200, 2600 }); });
		add("object, still background", "30 px", [](Scene& s) { s.objects.push_back({ 0, 900, 800, -30, 4, 360, 300, 4000, 300 }); });
		add("object against a pan", "pan 10, object -15", [](Scene& s) { s.panX = 10; s.objects.push_back({ 0, 1700, 700, -15, 0, 420, 420, 200, 2600 }); });
		add("object against a pan", "pan 6, object +20", [](Scene& s) { s.panX = 6; s.objects.push_back({ 0, 1000, 900, 20, -2, 300, 500, 4200, 2400 }); });
		add("thin objects", "bars over still", [](Scene& s) { for (int i = 0; i < 6; ++i) s.objects.push_back({ 1, 600.0f + i * 420, 200, 10, 0, 4, 1500, 0, 0 }); });
		add("thin objects", "bars over a pan", [](Scene& s) { s.panX = 8; for (int i = 0; i < 6; ++i) s.objects.push_back({ 1, 600.0f + i * 420, 300, -6, 0, 5, 1400, 0, 0 }); });
		add("repetitive texture", "fence 24, pan 7", [](Scene& s) { s.fencePeriod = 24; s.panX = 7; });
		add("repetitive texture", "fence 16, pan 12", [](Scene& s) { s.fencePeriod = 16; s.panX = 12; });
		add("repetitive texture", "fence 24 in a panned scene, pan 7", [](Scene& s) { s.panX = 7; s.objects.push_back({ 2, 1200, 500, 7, 0, 1400, 1000, 24, 0 }); });
		add("repetitive texture", "fence 24 moving 7 over a still scene", [](Scene& s) { s.objects.push_back({ 2, 1200, 500, 7, 0, 1400, 1000, 24, 0 }); });
		add("occlusion", "large object crossing", [](Scene& s) { s.panX = 5; s.objects.push_back({ 0, 1000, 200, 25, 0, 1200, 1600, 4300, 1000 }); });
		add("lighting change", "fade 3% a frame", [](Scene& s) { s.panX = 6; s.fade = 0.03f; });
		add("heavy grain", "pan 6", [](Scene& s) { s.panX = 6; s.grain = 3000; });
		add("heavy grain", "pan 15", [](Scene& s) { s.panX = 15; s.grain = 3000; });
		add("dark, low contrast", "pan 6", [](Scene& s) { s.panX = 6; s.level = 0.15f; });
		add("subtitles over motion", "pan 8", [](Scene& s) { s.panX = 8; s.subtitle = true; });
		add("subtitles over motion", "pan 20", [](Scene& s) { s.panX = 20; s.subtitle = true; });
		add("cut", "cut on the last pair", [](Scene& s) { s.panX = 6; s.cut = true; });
		add("motion blur", "pan 12", [](Scene& s) { s.panX = 12; s.blur = true; });
		add("motion blur", "pan 30", [](Scene& s) { s.panX = 30; s.blur = true; });
		return list;
	}
}

void RunBetaMotionCorpus()
{
	using namespace Corpus;
	Gpu gpu;
	pl_gpu g = gpu.GpuHandle();
	auto envValue = [](const char* name)
	{
		char* v = nullptr;
		size_t n = 0;
		std::string value;
		if (_dupenv_s(&v, &n, name) == 0 && v) value = v;
		std::free(v);
		return value;
	};
	const std::string outDir = envValue("VP_TEST_CORPUS_DIR");
	std::cout << "Beta motion corpus: building the canvas\n" << std::flush;
	Canvas canvas;
	canvas.Build(6400, 3600);
	auto plane = [&](int pw, int ph, int comps)
	{
		pl_tex_params p{};
		p.w = pw; p.h = ph;
		p.format = pl_find_fmt(g, PL_FMT_UNORM, comps, 16, 16,
			static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_STORABLE | PL_FMT_CAP_LINEAR | PL_FMT_CAP_HOST_READABLE));
		p.sampleable = true; p.storable = true; p.host_writable = true; p.host_readable = true;
		return pl_tex_create(g, &p);
	};
	const int frames = 6;
	std::vector<pl_tex> lumaT(frames), chromaT(frames);
	for (int f = 0; f < frames; ++f) { lumaT[f] = plane(W, H, 1); chromaT[f] = plane(W / 2, H / 2, 2); }
	const std::vector<Variant> variants = Variants();
	struct Result { double hold = 0, warp = 0, epe = 0, bad = 0, trusted = 0; bool scored = false; };
	const std::vector<Scene> scenes = Scenes();
	std::vector<std::vector<Result>> results(scenes.size(), std::vector<Result>(variants.size()));
	std::ofstream csv;
	if (!outDir.empty())
	{
		CreateDirectoryA(outDir.c_str(), nullptr);
		csv.open(outDir + "\\corpus.csv", std::ios::trunc);
		csv << "class,scene,variant,hold,warp,benefit,epe_trusted,bad_trusted_share,trusted_share\n";
	}
	std::vector<uint16_t> luma, chroma, truth, truthChroma, warped(static_cast<size_t>(W) * H), aLuma;
	const int blocksX = ((W + 1) / 2 + 7) / 8, blocksY = ((H + 1) / 2 + 7) / 8;
	auto halfToFloat = [](uint16_t h)
	{
		const int e = (h >> 10) & 31, m = h & 1023;
		const float mag = e == 0 ? std::ldexp(static_cast<float>(m), -24) : std::ldexp(1.0f + m / 1024.0f, e - 15);
		return (h & 0x8000) ? -mag : mag;
	};
	for (size_t si = 0; si < scenes.size(); ++si)
	{
		const Scene& scene = scenes[si];
		const auto renderStart = std::chrono::steady_clock::now();
		for (int f = 0; f < frames; ++f)
		{
			Render(scene, canvas, static_cast<float>(f), 1000u + f, luma, chroma);
			if (f == frames - 2) aLuma = luma;
			pl_tex_transfer_params up{};
			up.tex = lumaT[f]; up.ptr = luma.data();
			Check(pl_tex_upload(g, &up), "corpus: upload");
			up.tex = chromaT[f]; up.ptr = chroma.data();
			Check(pl_tex_upload(g, &up), "corpus: upload");
		}
		Render(scene, canvas, frames - 1.5f, 999u, truth, truthChroma);   // the true midpoint of the last pair
		const std::vector<float> trueField = TrueField(scene, frames - 2.0f, blocksX, blocksY);
		const double renderSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - renderStart).count();
		for (size_t vi = 0; vi < variants.size(); ++vi)
		{
			Dejudder::MotionWarp warp(gpu.Log(), g, SettingsFor(variants[vi]));
			Dejudder::Planes last;
			for (int f = 0; f + 1 < frames; ++f)
			{
				Dejudder::Planes a, b;
				a.luma = lumaT[f]; a.chroma = chromaT[f]; b.luma = lumaT[f + 1]; b.chroma = chromaT[f + 1];
				a.chromaShiftX = b.chromaShiftX = 1;
				a.chromaShiftY = b.chromaShiftY = 1;
				Check(warp.Estimate(a, b), "corpus: estimate: " + warp.LastError());
				last = a;
			}
			Dejudder::Planes out;
			Check(warp.Warp(last, 0.5f, out, true), "corpus: warp: " + warp.LastError());
			pl_tex_transfer_params d{};
			d.tex = out.luma; d.ptr = warped.data();
			Check(pl_tex_download(g, &d), "corpus: download");
			Result r;
			double hold = 0, wr = 0;
			long long n = 0;
			for (int y = 64; y < H - 64; y += 2)
				for (int x = 64; x < W - 64; x += 2)
				{
					const size_t i = static_cast<size_t>(y) * W + x;
					hold += std::abs(static_cast<int>(aLuma[i]) - static_cast<int>(truth[i]));
					wr += std::abs(static_cast<int>(warped[i]) - static_cast<int>(truth[i]));
					++n;
				}
			r.hold = hold / n / 64.0;
			r.warp = wr / n / 64.0;
			// The field: trusted blocks' error against the truth.
			std::vector<uint16_t> fieldHalf(static_cast<size_t>(blocksX) * blocksY * 4);
			d.tex = warp.Field(); d.ptr = fieldHalf.data();
			Check(pl_tex_download(g, &d), "corpus: field download");
			double epe = 0; long long trusted = 0, bad = 0, all = 0;
			for (int by = 2; by < blocksY - 2; ++by)
				for (int bx = 2; bx < blocksX - 2; ++bx)
				{
					const size_t i = static_cast<size_t>(by) * blocksX + bx;
					++all;
					if (halfToFloat(fieldHalf[i * 4 + 2]) < 0.5f) continue;
					++trusted;
					const float tx = trueField[i * 2], ty = trueField[i * 2 + 1];
					if (std::isnan(tx)) { ++bad; continue; }
					const float e = std::hypot(halfToFloat(fieldHalf[i * 4]) - tx, halfToFloat(fieldHalf[i * 4 + 1]) - ty);
					epe += e;
					bad += e > 2.0f ? 1 : 0;
				}
			r.trusted = all ? static_cast<double>(trusted) / all : 0.0;
			r.epe = trusted ? epe / trusted : 0.0;
			r.bad = trusted ? static_cast<double>(bad) / trusted : 0.0;
			r.scored = true;
			results[si][vi] = r;
			if (csv)
				csv << "\"" << scene.cls << "\",\"" << scene.name << "\",\"" << variants[vi].name << "\"," << r.hold << "," << r.warp << ","
					<< (r.hold - r.warp) << "," << r.epe << "," << r.bad << "," << r.trusted << "\n";
			// The worst places of today's warp, for the eye.
			if (vi == 0 && !outDir.empty())
			{
				double worst = -1; int wx = 0, wy = 0;
				for (int ty = 0; ty + 256 <= H; ty += 128)
					for (int tx = 0; tx + 384 <= W; tx += 128)
					{
						double e = 0;
						for (int y = ty; y < ty + 256; y += 4)
							for (int x = tx; x < tx + 384; x += 4)
							{
								const size_t i = static_cast<size_t>(y) * W + x;
								e += std::abs(static_cast<int>(warped[i]) - static_cast<int>(truth[i]));
							}
						if (e > worst) { worst = e; wx = tx; wy = ty; }
					}
				std::string file = outDir + "\\crop-" + std::to_string(si) + ".ppm";
				std::ofstream ppm(file, std::ios::binary | std::ios::trunc);
				ppm << "P6\n" << 384 * 3 << " 256\n255\n";
				for (int y = wy; y < wy + 256; ++y)
					for (int part = 0; part < 3; ++part)
						for (int x = wx; x < wx + 384; ++x)
						{
							const size_t i = static_cast<size_t>(y) * W + x;
							int v = part == 0 ? truth[i] >> 8 : part == 1 ? warped[i] >> 8 :
								std::min(255, std::abs(static_cast<int>(warped[i]) - static_cast<int>(truth[i])) >> 5);
							const unsigned char px[3] = { static_cast<unsigned char>(v), static_cast<unsigned char>(v), static_cast<unsigned char>(v) };
							ppm.write(reinterpret_cast<const char*>(px), 3);
						}
			}
		}
		const Result& today = results[si][0];
		char line[320];
		std::snprintf(line, sizeof(line), "  %-26s %-22s hold %6.2f  warp %6.2f  benefit %6.2f  trusted %5.1f%%  vector error %5.2f px  wrong trusted %5.1f%%  (render %.1f s)",
			scene.cls.c_str(), scene.name.c_str(), today.hold, today.warp, today.hold - today.warp, 100.0 * today.trusted, today.epe,
			100.0 * today.bad, renderSeconds);
		std::cout << line << "\n" << std::flush;
	}
	// Per class (today), and the headroom.
	std::cout << "Beta motion corpus: per class, today's settings (codes of 1023; vectors in source px):\n";
	std::map<std::string, std::vector<size_t>> byClass;
	for (size_t si = 0; si < scenes.size(); ++si) byClass[scenes[si].cls].push_back(si);
	for (const auto& cls : byClass)
	{
		double hold = 0, wr = 0, epe = 0, bad = 0, trusted = 0;
		for (size_t si : cls.second) { const Result& r = results[si][0]; hold += r.hold; wr += r.warp; epe += r.epe; bad += r.bad; trusted += r.trusted; }
		const double n = static_cast<double>(cls.second.size());
		char line[256];
		std::snprintf(line, sizeof(line), "  %-26s benefit %6.2f of hold %6.2f (%5.1f%% removed)  trusted %5.1f%%  vector error %5.2f  wrong trusted %5.1f%%",
			cls.first.c_str(), (hold - wr) / n, hold / n, hold > 0 ? 100.0 * (hold - wr) / hold : 0.0, 100.0 * trusted / n, epe / n, 100.0 * bad / n);
		std::cout << line << "\n";
	}
	double todayBenefit = 0, oracleBenefit = 0;
	std::vector<double> perVariant(variants.size(), 0.0);
	for (size_t si = 0; si < scenes.size(); ++si)
	{
		if (scenes[si].cut) continue;   // nothing to gain or lose but the hold
		double best = -1e30;
		for (size_t vi = 0; vi < variants.size(); ++vi)
		{
			const double b = results[si][vi].hold - results[si][vi].warp;
			perVariant[vi] += b;
			best = std::max(best, b);
		}
		todayBenefit += results[si][0].hold - results[si][0].warp;
		oracleBenefit += best;
	}
	size_t bestVariant = 0;
	for (size_t vi = 1; vi < variants.size(); ++vi) if (perVariant[vi] > perVariant[bestVariant]) bestVariant = vi;
	// Every variant (2026-10-16, planner Q1 comparison): its benefit against the first's, its
	// trusted share on the cut (should be near 0) and its wrong-trusted share over the scenes.
	std::cout << "Beta motion corpus: every variant (benefit summed, cut excluded; share of the first's; the cut's trusted; wrong trusted):\n";
	for (size_t vi = 0; vi < variants.size(); ++vi)
	{
		double bad = 0.0;
		for (size_t si = 0; si < scenes.size(); ++si) bad += results[si][vi].bad;
		char row[320];
		std::snprintf(row, sizeof(row), "  %-52s %8.2f  %6.1f%%  cut trusted %5.1f%%  wrong trusted %5.2f%%", variants[vi].name.c_str(), perVariant[vi],
			perVariant[0] > 0 ? 100.0 * perVariant[vi] / perVariant[0] : 0.0, 100.0 * results[scenes.size() - 3][vi].trusted,
			100.0 * bad / scenes.size());
		std::cout << row << "\n";
	}
	char line[400];
	std::snprintf(line, sizeof(line), "Beta motion corpus headroom (benefit summed over the scenes, cut excluded): today %.2f; best single setting %.2f (%s, %+.1f%%); best per scene %.2f (%+.1f%%)",
		todayBenefit, perVariant[bestVariant], variants[bestVariant].name.c_str(),
		todayBenefit > 0 ? 100.0 * (perVariant[bestVariant] - todayBenefit) / todayBenefit : 0.0, oracleBenefit,
		todayBenefit > 0 ? 100.0 * (oracleBenefit - todayBenefit) / todayBenefit : 0.0);
	std::cout << line << "\n";
	const Result& cutResult = results[scenes.size() - 3][0];
	std::cout << "  the cut: trusted " << 100.0 * cutResult.trusted << "% of blocks (should hold: near 0)\n";
	for (size_t f = 0; f < lumaT.size(); ++f) { pl_tex_destroy(g, &lumaT[f]); pl_tex_destroy(g, &chromaT[f]); }
}

// The failure map's real part (owner 2026-10-13): high-frame-rate clips give
// the true in-between pictures. UVG 120 fps (3840x2160 yuv420p10le .yuv):
// frames n and n+5 are a 24p pair, n+1 .. n+4 the truth at 0.2 .. 0.8. Xiph
// 50p (.y4m, 8-bit 4:2:0): n and n+2, n+1 the truth at 0.5. Warm-up as in VP
// (three earlier pairs at the same spacing). Per clip six places; per place
// the warp against the truth and the hold (frame n, what is shown without
// the de-judder), with the settings of the synthetic corpus. Report only:
// VP_TEST_REAL_FOOTAGE=<folder of the clips>; CSV (and, with
// VP_TEST_REAL_LPIPS=<folder>, 8-bit PGMs of truth / warp / hold at 0.4 or
// 0.5 for LPIPS) in VP_TEST_CORPUS_DIR.
void RunBetaRealFootage()
{
	using namespace Corpus;
	auto envValue = [](const char* name)
	{
		char* v = nullptr;
		size_t n = 0;
		std::string value;
		if (_dupenv_s(&v, &n, name) == 0 && v) value = v;
		std::free(v);
		return value;
	};
	const std::string folder = envValue("VP_TEST_REAL_FOOTAGE"), outDir = envValue("VP_TEST_CORPUS_DIR"),
		lpipsDir = envValue("VP_TEST_REAL_LPIPS");
	struct Clip { std::string name, path; bool y4m; int spacing; long long frames; long long headerBytes; };
	std::vector<Clip> clips;
	WIN32_FIND_DATAA found{};
	HANDLE search = FindFirstFileA((folder + "\\*").c_str(), &found);
	const long long yuvFrame = 3840LL * 2160 * 3;          // 10-bit in 16-bit words, 4:2:0
	const long long y4mFrame = 3840LL * 2160 * 3 / 2 + 6;  // "FRAME\n" + 8-bit 4:2:0
	if (search != INVALID_HANDLE_VALUE)
	{
		do
		{
			const std::string name = found.cFileName;
			const long long size = (static_cast<long long>(found.nFileSizeHigh) << 32) | found.nFileSizeLow;
			const bool yuv = name.size() > 4 && name.compare(name.size() - 4, 4, ".yuv") == 0 && name.find("3840x2160") != std::string::npos;
			const bool y4m = name.size() > 4 && name.compare(name.size() - 4, 4, ".y4m") == 0 && name.find("2160p50") != std::string::npos;
			if (yuv)
				clips.push_back({ name.substr(0, name.find('_')), folder + "\\" + name, false, 5, size / yuvFrame, 0 });
			else if (y4m)
			{
				std::ifstream in(folder + "\\" + name, std::ios::binary);
				std::string header;
				std::getline(in, header);
				const long long headerBytes = static_cast<long long>(header.size()) + 1;
				if (header.find("W3840") != std::string::npos && header.find("H2160") != std::string::npos &&
					(header.find(" C") == std::string::npos || header.find(" C420") != std::string::npos))
					clips.push_back({ name.substr(0, name.find("_2160")), folder + "\\" + name, true, 2, (size - headerBytes) / y4mFrame, headerBytes });
			}
		} while (FindNextFileA(search, &found));
		FindClose(search);
	}
	if (clips.empty())
	{
		std::cout << "Beta real footage: no 3840x2160 .yuv (120 fps, 10-bit) or 2160p50 .y4m in " << folder << "\n";
		return;
	}
	Gpu gpu;
	pl_gpu g = gpu.GpuHandle();
	auto plane = [&](int pw, int ph, int comps)
	{
		pl_tex_params p{};
		p.w = pw; p.h = ph;
		p.format = pl_find_fmt(g, PL_FMT_UNORM, comps, 16, 16,
			static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_STORABLE | PL_FMT_CAP_LINEAR | PL_FMT_CAP_HOST_READABLE));
		p.sampleable = true; p.storable = true; p.host_writable = true; p.host_readable = true;
		return pl_tex_create(g, &p);
	};
	// One frame: 16-bit luma, interleaved 16-bit CbCr at half size.
	auto readFrame = [&](const Clip& clip, long long index, std::vector<uint16_t>& luma, std::vector<uint16_t>* chroma)
	{
		FILE* f = nullptr;
		if (fopen_s(&f, clip.path.c_str(), "rb") != 0 || !f) return false;
		const size_t lumaCount = static_cast<size_t>(W) * H, chromaCount = lumaCount / 4;
		luma.resize(lumaCount);
		bool ok = true;
		if (!clip.y4m)
		{
			std::vector<uint16_t> buffer(lumaCount + 2 * chromaCount);
			_fseeki64(f, index * yuvFrame, SEEK_SET);
			ok = fread(buffer.data(), 2, buffer.size(), f) == buffer.size();
			for (size_t i = 0; i < lumaCount; ++i) luma[i] = static_cast<uint16_t>(std::min(1023, static_cast<int>(buffer[i])) * 64);
			if (chroma)
			{
				chroma->resize(chromaCount * 2);
				for (size_t i = 0; i < chromaCount; ++i)
				{
					(*chroma)[i * 2] = static_cast<uint16_t>(std::min(1023, static_cast<int>(buffer[lumaCount + i])) * 64);
					(*chroma)[i * 2 + 1] = static_cast<uint16_t>(std::min(1023, static_cast<int>(buffer[lumaCount + chromaCount + i])) * 64);
				}
			}
		}
		else
		{
			std::vector<uint8_t> buffer(static_cast<size_t>(y4mFrame));
			_fseeki64(f, clip.headerBytes + index * y4mFrame, SEEK_SET);
			ok = fread(buffer.data(), 1, buffer.size(), f) == buffer.size() && std::memcmp(buffer.data(), "FRAME", 5) == 0;
			const uint8_t* y = buffer.data() + 6;
			for (size_t i = 0; i < lumaCount; ++i) luma[i] = static_cast<uint16_t>(y[i] * 256);
			if (chroma)
			{
				chroma->resize(chromaCount * 2);
				for (size_t i = 0; i < chromaCount; ++i)
				{
					(*chroma)[i * 2] = static_cast<uint16_t>(y[lumaCount + i] * 256);
					(*chroma)[i * 2 + 1] = static_cast<uint16_t>(y[lumaCount + chromaCount + i] * 256);
				}
			}
		}
		fclose(f);
		return ok;
	};
	auto writePgm = [](const std::string& file, const std::vector<uint16_t>& luma)
	{
		std::ofstream out(file, std::ios::binary | std::ios::trunc);
		out << "P5\n" << W << " " << H << "\n255\n";
		std::vector<unsigned char> row(W);
		for (int y = 0; y < H; ++y)
		{
			for (int x = 0; x < W; ++x) row[x] = static_cast<unsigned char>(luma[static_cast<size_t>(y) * W + x] >> 8);
			out.write(reinterpret_cast<const char*>(row.data()), W);
		}
	};
	const std::vector<Variant> variants = Variants();
	const int sources = 5;   // three warm-up pairs and the scored pair
	std::vector<pl_tex> lumaT(sources), chromaT(sources);
	for (int f = 0; f < sources; ++f) { lumaT[f] = plane(W, H, 1); chromaT[f] = plane(W / 2, H / 2, 2); }
	std::ofstream csv;
	if (!outDir.empty())
	{
		CreateDirectoryA(outDir.c_str(), nullptr);
		csv.open(outDir + "\\real.csv", std::ios::trunc);
		csv << "clip,frame,shift,variant,hold,warp,benefit,trusted_share,harm\n";
	}
	if (!lpipsDir.empty()) CreateDirectoryA(lpipsDir.c_str(), nullptr);
	struct Sum { double hold = 0, warp = 0; int n = 0; };
	std::vector<Sum> perVariant(variants.size());
	struct Split { double hold[2] = { 0, 0 }, warp[2] = { 0, 0 }; long long n[2] = { 0, 0 }; };
	std::map<std::string, std::vector<Split>> splits;
	std::map<std::string, std::vector<Sum>> perClip;
	double oracle = 0.0;
	std::vector<uint16_t> luma, chroma, aLuma, warped(static_cast<size_t>(W) * H);
	std::vector<std::vector<uint16_t>> truths;
	// VP_TEST_REAL_DIAG=<clip>:<frame>: why a place holds. Today's settings
	// and variants (full-size matcher, wider coarse search, looser
	// tolerance): the global vectors, the frame trust, per block whether the
	// match error or the forward/backward agreement failed; trust and error
	// maps as PGMs in VP_TEST_CORPUS_DIR.
	const std::string diag = envValue("VP_TEST_REAL_DIAG");
	if (!diag.empty())
	{
		const std::string clipName = diag.substr(0, diag.find(':'));
		const long long n = std::atoll(diag.substr(diag.find(':') + 1).c_str());
		for (const Clip& clip : clips)
		{
			if (clip.name != clipName) continue;
			bool ok = true;
			for (int f = 0; f < sources && ok; ++f)
			{
				ok = readFrame(clip, n + (f - 3LL) * clip.spacing, luma, &chroma);
				if (f == 3) aLuma = luma;
				pl_tex_transfer_params up{};
				up.tex = lumaT[f]; up.ptr = luma.data();
				Check(pl_tex_upload(g, &up), "diag: upload");
				up.tex = chromaT[f]; up.ptr = chroma.data();
				Check(pl_tex_upload(g, &up), "diag: upload");
			}
			Check(ok, "diag: read");
			std::vector<uint16_t> truth;
			Check(readFrame(clip, n + clip.spacing / 2, truth, nullptr), "diag: truth");
			if (!outDir.empty()) { CreateDirectoryA(outDir.c_str(), nullptr); writePgm(outDir + "\\diag-a.pgm", aLuma); }
			struct Case { const char* name; Dejudder::Settings s; };
			std::vector<Case> cases;
			Dejudder::Settings today = SettingsFor(variants[0]);
			cases.push_back({ "today", today });
			Dejudder::Settings full = today; full.lowRes = false; cases.push_back({ "full-size matcher", full });
			Dejudder::Settings search = today; search.lowResAll = false; cases.push_back({ "half-size search", search });
			Dejudder::Settings wide = today; wide.coarseRange = 10; cases.push_back({ "coarse range 10", wide });
			Dejudder::Settings loose = today; loose.matchTolerance = 6.0f; cases.push_back({ "tolerance 6", loose });
			Dejudder::Settings agree = today; agree.consistencyPixels = 3.0f; cases.push_back({ "consistency 3 px", agree });
			Dejudder::Settings noGate = today; noGate.minTrustedShare = 0.0f; cases.push_back({ "no frame gate", noGate });
			Dejudder::Settings m1 = today; m1.textureTolerance = 0.5f; cases.push_back({ "M1 texture 0.5", m1 });
			Dejudder::Settings m2 = today; m2.globalTwo = true; cases.push_back({ "M2 two global", m2 });
			Dejudder::Settings m3 = today; m3.regionalHold = true; cases.push_back({ "M3 regional hold", m3 });
			Dejudder::Settings cand = today;
			cand.matchTolerance = 4.5f; cand.textureTolerance = 0.25f; cand.globalTwo = true; cand.regionalHold = true;
			cand.coarseRange = 8; cand.consistencyPixels = 2.0f; cand.fadeMatch = true; cand.estSplit = false;
			cases.push_back({ "candidate", cand });
			Dejudder::Settings pix = cand;
			pix.pixelTrust = true; pix.pixelTrustLo = 3.0f; pix.pixelTrustHi = 8.0f; pix.pixelTrustedVectors = true;
			cases.push_back({ "pixel trust", pix });
			Dejudder::Settings own = pix;
			own.pixelOwnTexture = 6.0f; own.pixelOwnError = 3.0f;
			cases.push_back({ "pixel own vectors", own });
			// VP_TEST_REAL_TIMING=1: the matcher's GPU time per pair, today vs
			// the motion fixes together (undrained GPU timers, 30 estimates).
			if (envValue("VP_TEST_REAL_TIMING") == "1")
			{
				Dejudder::Settings all = today;
				all.matchTolerance = 4.5f; all.textureTolerance = 0.5f; all.globalTwo = true; all.regionalHold = true;
				all.coarseRange = 8; all.consistencyPixels = 2.0f;
				// The candidate (tolerance 4.5, texture 0.25, fade, split off) and
				// with M6's pixel trust (two tiers).
				Dejudder::Settings cand = today;
				cand.matchTolerance = 4.5f; cand.textureTolerance = 0.25f; cand.globalTwo = true; cand.regionalHold = true;
				cand.coarseRange = 8; cand.consistencyPixels = 2.0f; cand.fadeMatch = true;
				Dejudder::Settings pix = cand;
				pix.pixelTrust = true; pix.pixelTrustLo = 3.0f; pix.pixelTrustHi = 8.0f; pix.pixelTrustLoUntrusted = 1.0f; pix.pixelTrustHiUntrusted = 2.5f;
				Dejudder::Settings live[4] = { today, all, cand, pix };
				for (Dejudder::Settings& t : live) { t.estCandidates = true; t.estQpel = true; t.vectorSmooth = true; t.estGate = true; t.holdHysteresis = 2; }
				live[0].estSplit = live[1].estSplit = true;
				const char* names[4] = { "today", "all fixes", "candidate", "candidate + pixel trust" };
				for (int k = 0; k < 4; ++k)
				{
					Dejudder::MotionWarp warp(gpu.Log(), g, live[k]);
					warp.SetPassTiming(true, false);
					for (int r = 0; r < 34; ++r)
					{
						Dejudder::Planes pa, pb;
						const int f = r % (sources - 1);
						pa.luma = lumaT[f]; pa.chroma = chromaT[f]; pb.luma = lumaT[f + 1]; pb.chroma = chromaT[f + 1];
						pa.chromaShiftX = pb.chromaShiftX = 1; pa.chromaShiftY = pb.chromaShiftY = 1;
						if (r == 4) warp.SetPassTiming(true, false);   // after the warm-up
						Check(warp.Estimate(pa, pb), "timing: estimate");
						// Two extras' warps per pair (source warp), as at 24p on 60 Hz.
						Dejudder::Planes out;
						Check(warp.Warp(pa, 0.4f, out, true), "timing: warp");
						Check(warp.Warp(pa, 0.8f, out, true), "timing: warp");
					}
					warp.FlushPassTimers();
					double total = 0;
					std::string detail;
					for (const auto& e : warp.PassGpuMs())
					{
						total += e.second;
						if (e.first.find("search") != std::string::npos || e.first.find("pixel") != std::string::npos ||
							e.first.find("warp") != std::string::npos || e.first.find("frame trust") != std::string::npos)
						{
							char part[160];
							std::snprintf(part, sizeof(part), "; %s %.2f", e.first.c_str() + (e.first.find(": ") != std::string::npos ? e.first.find(": ") + 2 : 0), e.second / 30.0);
							detail += part;
						}
					}
					char line[1200];
					std::snprintf(line, sizeof(line), "timing %s %s: matcher + two warps %.2f ms per pair%s", clip.name.c_str(), names[k], total / 30.0, detail.c_str());
					std::cout << line << "\n" << std::flush;
				}
			}
			for (const Case& c : cases)
			{
				Dejudder::MotionWarp warp(gpu.Log(), g, c.s);
				Dejudder::Planes a;
				for (int f = 0; f + 1 < sources; ++f)
				{
					Dejudder::Planes pa, pb;
					pa.luma = lumaT[f]; pa.chroma = chromaT[f]; pb.luma = lumaT[f + 1]; pb.chroma = chromaT[f + 1];
					pa.chromaShiftX = pb.chromaShiftX = 1;
					pa.chromaShiftY = pb.chromaShiftY = 1;
					Check(warp.Estimate(pa, pb), "diag: estimate: " + warp.LastError());
					a = pa;
				}
				auto half = [](uint16_t h)
				{
					const int e = (h >> 10) & 31, m = h & 1023;
					const float mag = e == 0 ? std::ldexp(static_cast<float>(m), -24) : std::ldexp(1.0f + m / 1024.0f, e - 15);
					return (h & 0x8000) ? -mag : mag;
				};
				auto download = [&](pl_tex t, std::vector<float>& out)
				{
					out.clear();
					if (!t) return;
					const size_t count = static_cast<size_t>(t->params.w) * t->params.h * t->params.format->num_components;
					pl_tex_transfer_params d{};
					d.tex = t;
					if (t->params.format->component_depth[0] == 32)
					{
						out.resize(count);
						d.ptr = out.data();
						Check(pl_tex_download(g, &d), "diag: download");
					}
					else
					{
						std::vector<uint16_t> raw(count);
						d.ptr = raw.data();
						Check(pl_tex_download(g, &d), "diag: download");
						for (uint16_t v : raw) out.push_back(half(v));
					}
				};
				std::vector<float> field, trust, gv, fwd, bwd;
				download(warp.Field(), field);
				download(warp.FrameTrust(), trust);
				download(warp.GlobalVector(), gv);
				download(warp.Forward(), fwd);
				download(warp.Backward(), bwd);
				const int bx = warp.BlocksX(), by = warp.BlocksY();
				long long trusted = 0, badMatch = 0, disagree = 0, all = 0;
				double meanErr = 0;
				std::vector<uint16_t> trustMap(static_cast<size_t>(W) * H), errMap(static_cast<size_t>(W) * H);
				const int fwW = warp.Forward() ? warp.Forward()->params.w : 0;
				for (int y = 0; y < by; ++y)
					for (int x = 0; x < bx; ++x)
					{
						const size_t i = (static_cast<size_t>(y) * bx + x) * 4;
						++all;
						const bool t = field[i + 2] > 0.5f;
						trusted += t ? 1 : 0;
						const float err = field[i + 3];
						meanErr += err;
						if (!t) { if (err > c.s.matchTolerance) ++badMatch; else ++disagree; }
						for (int yy = 0; yy < 16; ++yy)
							for (int xx = 0; xx < 16; ++xx)
							{
								const int px = x * 16 + xx, py = y * 16 + yy;
								if (px >= W || py >= H) continue;
								trustMap[static_cast<size_t>(py) * W + px] = t ? 65535 : 0;
								errMap[static_cast<size_t>(py) * W + px] = static_cast<uint16_t>(std::min(65535.0f, err * 4096.0f));
							}
					}
				(void)fwW;
				// The dominant field vector among the trusted blocks.
				std::map<std::pair<int, int>, int> votes;
				for (int y = 0; y < by; ++y)
					for (int x = 0; x < bx; ++x)
					{
						const size_t i = (static_cast<size_t>(y) * bx + x) * 4;
						if (field[i + 2] > 0.5f) ++votes[{ static_cast<int>(std::lround(field[i] / 8)) * 8, static_cast<int>(std::lround(field[i + 1] / 8)) * 8 }];
					}
				std::string top;
				std::vector<std::pair<int, std::pair<int, int>>> ranked;
				for (const auto& v : votes) ranked.push_back({ v.second, v.first });
				std::sort(ranked.rbegin(), ranked.rend());
				for (size_t k = 0; k < ranked.size() && k < 3; ++k)
					top += " (" + std::to_string(ranked[k].second.first) + "," + std::to_string(ranked[k].second.second) + ")x" + std::to_string(ranked[k].first);
				// The warp's own result against the truth.
				Dejudder::Planes out;
				Check(warp.Warp(a, 0.5f * 2 * (clip.spacing / 2) / clip.spacing, out, true), "diag: warp");
				pl_tex_transfer_params d{};
				d.tex = out.luma; d.ptr = warped.data();
				Check(pl_tex_download(g, &d), "diag: download");
				double hold = 0, wr = 0; long long cnt = 0;
				for (int y = 64; y < H - 64; y += 2)
					for (int x = 64; x < W - 64; x += 2)
					{
						const size_t i = static_cast<size_t>(y) * W + x;
						hold += std::abs(static_cast<int>(aLuma[i]) - static_cast<int>(truth[i]));
						wr += std::abs(static_cast<int>(warped[i]) - static_cast<int>(truth[i]));
						++cnt;
					}
				char line[600];
				std::snprintf(line, sizeof(line),
					"diag %s %lld [%s]: frame trust share %.3f scale %.2f mean|v| %.1f; global (%.1f, %.1f) px ratio %.2f set %.0f; blocks %lld trusted %.1f%%, untrusted by match error %.1f%%, by disagreement %.1f%%; mean error %.2f; top trusted vectors%s; hold %.2f warp %.2f",
					clip.name.c_str(), n, c.name, trust.size() > 0 ? trust[0] : -1.0f, trust.size() > 1 ? trust[1] : -1.0f, trust.size() > 2 ? trust[2] : -1.0f,
					gv.size() > 1 ? gv[0] * 8 : 0.0f, gv.size() > 1 ? gv[1] * 8 : 0.0f, gv.size() > 2 ? gv[2] : -1.0f, gv.size() > 3 ? gv[3] : -1.0f,
					all, 100.0 * trusted / all, 100.0 * badMatch / all, 100.0 * disagree / all, meanErr / all, top.c_str(),
					hold / cnt / 64.0, wr / cnt / 64.0);
				std::cout << line << "\n" << std::flush;
				if (!outDir.empty())
				{
					std::string tag = c.name;
					for (char& ch : tag) if (ch == ' ') ch = '-';
					writePgm(outDir + "\\diag-trust-" + tag + ".pgm", trustMap);
					writePgm(outDir + "\\diag-error-" + tag + ".pgm", errMap);
					// The three most damaged 32x32 tiles (warp worse than hold),
					// as 256x256 crops: truth | hold | warp, 8-bit.
					std::vector<std::pair<double, int>> damage;
					for (int ty = 2; ty + 2 < H / 32; ++ty)
						for (int tx = 2; tx + 2 < W / 32; ++tx)
						{
							double dh = 0, dw = 0;
							for (int y = ty * 32; y < ty * 32 + 32; y += 2)
								for (int x = tx * 32; x < tx * 32 + 32; x += 2)
								{
									const size_t i = static_cast<size_t>(y) * W + x;
									dh += std::abs(static_cast<int>(aLuma[i]) - static_cast<int>(truth[i]));
									dw += std::abs(static_cast<int>(warped[i]) - static_cast<int>(truth[i]));
								}
							damage.push_back({ dw - dh, ty * 1000 + tx });
						}
					std::sort(damage.rbegin(), damage.rend());
					for (int k = 0; k < 3 && k < static_cast<int>(damage.size()); ++k)
					{
						const int cy = std::min(std::max((damage[k].second / 1000) * 32 + 16 - 128, 0), H - 256);
						const int cx = std::min(std::max((damage[k].second % 1000) * 32 + 16 - 128, 0), W - 256);
						std::ofstream ppm(outDir + "\\crop-" + tag + "-" + std::to_string(k) + ".pgm", std::ios::binary | std::ios::trunc);
						ppm << "P5\n" << 768 << " 256\n255\n";
						for (int y = cy; y < cy + 256; ++y)
							for (int part = 0; part < 3; ++part)
								for (int x = cx; x < cx + 256; ++x)
								{
									const size_t i = static_cast<size_t>(y) * W + x;
									const unsigned char v = static_cast<unsigned char>((part == 0 ? truth[i] : part == 1 ? aLuma[i] : warped[i]) >> 8);
									ppm.write(reinterpret_cast<const char*>(&v), 1);
								}
					}
				}
			}
		}
		for (int f = 0; f < sources; ++f) { pl_tex_destroy(g, &lumaT[f]); pl_tex_destroy(g, &chromaT[f]); }
		return;
	}
	for (const Clip& clip : clips)
	{
		perClip[clip.name].assign(variants.size(), Sum());
		splits[clip.name].assign(variants.size(), Split());
		const int places = 6;
		const long long first = 3LL * clip.spacing + 2, last = clip.frames - clip.spacing - 2;
		if (last <= first) continue;
		for (int place = 0; place < places; ++place)
		{
			const long long n = first + (last - first) * place / (places - 1);
			bool ok = true;
			for (int f = 0; f < sources && ok; ++f)
			{
				ok = readFrame(clip, n + (f - 3LL) * clip.spacing, luma, &chroma);
				if (f == 3) aLuma = luma;
				pl_tex_transfer_params up{};
				up.tex = lumaT[f]; up.ptr = luma.data();
				Check(pl_tex_upload(g, &up), "real: upload");
				up.tex = chromaT[f]; up.ptr = chroma.data();
				Check(pl_tex_upload(g, &up), "real: upload");
			}
			truths.assign(clip.spacing - 1, std::vector<uint16_t>());
			for (int k = 1; k < clip.spacing && ok; ++k)
				ok = readFrame(clip, n + k, truths[k - 1], nullptr);
			if (!ok)
			{
				std::cout << "  " << clip.name << " frame " << n << ": read failed\n";
				continue;
			}
			// Per variant: the mean over the in-between pictures.
			std::vector<double> benefit(variants.size(), 0.0);
			double todayHold = 0, todayWarp = 0, todayTrusted = 0;
			for (size_t vi = 0; vi < variants.size(); ++vi)
			{
				Dejudder::MotionWarp warp(gpu.Log(), g, SettingsFor(variants[vi]));
				Dejudder::Planes a;
				for (int f = 0; f + 1 < sources; ++f)
				{
					Dejudder::Planes pa, pb;
					pa.luma = lumaT[f]; pa.chroma = chromaT[f]; pb.luma = lumaT[f + 1]; pb.chroma = chromaT[f + 1];
					pa.chromaShiftX = pb.chromaShiftX = 1;
					pa.chromaShiftY = pb.chromaShiftY = 1;
					Check(warp.Estimate(pa, pb), "real: estimate: " + warp.LastError());
					a = pa;
				}
				// The trusted share, from the field.
				const int blocksX = ((W + 1) / 2 + 7) / 8, blocksY = ((H + 1) / 2 + 7) / 8;
				std::vector<uint16_t> fieldHalf(static_cast<size_t>(blocksX) * blocksY * 4);
				pl_tex_transfer_params d{};
				d.tex = warp.Field(); d.ptr = fieldHalf.data();
				Check(pl_tex_download(g, &d), "real: field download");
				long long trusted = 0;
				for (size_t i = 0; i < static_cast<size_t>(blocksX) * blocksY; ++i)
					trusted += (fieldHalf[i * 4 + 2] & 0x7FFF) >= 0x3800 ? 1 : 0;   // half float >= 0.5
				const double trustedShare = static_cast<double>(trusted) / (static_cast<double>(blocksX) * blocksY);
				// D1 (taste on): the strength this pair is shown at.
				float strength = 1.0f;
				if (variants[vi].taste)
				{
					std::vector<uint16_t> ft(4);
					d.tex = warp.FrameTrust(); d.ptr = ft.data();
					Check(pl_tex_download(g, &d), "real: frame trust download");
					const uint16_t h = ft[1];
					const int e = (h >> 10) & 31, m = h & 1023;
					strength = e == 0 ? std::ldexp(static_cast<float>(m), -24) : std::ldexp(1.0f + m / 1024.0f, e - 15);
				}
				for (int k = 1; k < clip.spacing; ++k)
				{
					float shift = static_cast<float>(k) / clip.spacing;
					if (variants[vi].taste)
					{
						// Shown at shift x strength: the true frame k is that
						// picture when the refresh's shift is k/spacing / strength.
						if (strength < 1e-3f || shift / strength > 1.0f)
							continue;   // never shown at this position
						shift /= strength;
					}
					Dejudder::Planes out;
					Check(warp.Warp(a, shift, out, true), "real: warp: " + warp.LastError());
					d.tex = out.luma; d.ptr = warped.data();
					Check(pl_tex_download(g, &d), "real: download");
					const std::vector<uint16_t>& truth = truths[k - 1];
					double hold = 0, wr = 0;
					long long count = 0;
					Split& split = splits[clip.name][vi];
					// Local damage: 32x32 tiles where the warp is visibly worse
					// than the hold (mean more than 6 codes of 1023 worse).
					const int tilesX = W / 32, tilesY = H / 32;
					std::vector<double> tileHold(static_cast<size_t>(tilesX) * tilesY, 0.0), tileWarp(tileHold.size(), 0.0);
					for (int y = 64; y < H - 64; y += 2)
						for (int x = 64; x < W - 64; x += 2)
						{
							const size_t i = static_cast<size_t>(y) * W + x;
							const int h = std::abs(static_cast<int>(aLuma[i]) - static_cast<int>(truth[i]));
							const int w = std::abs(static_cast<int>(warped[i]) - static_cast<int>(truth[i]));
							hold += h;
							wr += w;
							++count;
							const size_t t = static_cast<size_t>(y / 32) * tilesX + x / 32;
							tileHold[t] += h; tileWarp[t] += w;
							// Where the remaining difference is: pixels the warp moved,
							// or pixels left as A (held, or not moving).
							const int part = warped[i] == aLuma[i] ? 1 : 0;
							split.hold[part] += h; split.warp[part] += w; split.n[part] += 1;
						}
					hold /= count * 64.0;
					wr /= count * 64.0;
					int harmed = 0, tiles = 0;
					for (size_t t = 0; t < tileHold.size(); ++t)
					{
						if (tileHold[t] == 0 && tileWarp[t] == 0) continue;
						++tiles;
						harmed += (tileWarp[t] - tileHold[t]) / (256.0 * 64.0) > 6.0 ? 1 : 0;   // 256 samples a tile
					}
					const double harm = tiles ? static_cast<double>(harmed) / tiles : 0.0;
					benefit[vi] += (hold - wr) / (clip.spacing - 1);
					perVariant[vi].hold += hold; perVariant[vi].warp += wr; ++perVariant[vi].n;
					perClip[clip.name][vi].hold += hold; perClip[clip.name][vi].warp += wr; ++perClip[clip.name][vi].n;
					if (vi == 0) { todayHold += hold / (clip.spacing - 1); todayWarp += wr / (clip.spacing - 1); todayTrusted = trustedShare; }
					if (csv)
						csv << "\"" << clip.name << "\"," << n << "," << shift << ",\"" << variants[vi].name << "\"," << hold << "," << wr << ","
							<< (hold - wr) << "," << trustedShare << "," << harm << "\n";
					if (vi == 0 && !lpipsDir.empty() && k == clip.spacing / 2)
					{
						const std::string base = lpipsDir + "\\" + clip.name + "-" + std::to_string(n);
						writePgm(base + "-truth.pgm", truth);
						writePgm(base + "-warp.pgm", warped);
						writePgm(base + "-hold.pgm", aLuma);
					}
				}
			}
			oracle += *std::max_element(benefit.begin(), benefit.end()) * (clip.spacing - 1);
			char line[256];
			std::snprintf(line, sizeof(line), "  %-12s frame %5lld  hold %6.2f  warp %6.2f  benefit %6.2f (%5.1f%% removed)  trusted %5.1f%%",
				clip.name.c_str(), n, todayHold, todayWarp, todayHold - todayWarp,
				todayHold > 0 ? 100.0 * (todayHold - todayWarp) / todayHold : 0.0, 100.0 * todayTrusted);
			std::cout << line << "\n" << std::flush;
		}
	}
	std::cout << "Beta real footage: per clip, today's settings (codes of 1023, mean over the in-between pictures):\n";
	for (const auto& c : perClip)
	{
		const Sum& t = c.second[0];
		if (t.n == 0) continue;
		char line[200];
		std::snprintf(line, sizeof(line), "  %-12s hold %6.2f  warp %6.2f  (%5.1f%% removed)", c.first.c_str(), t.hold / t.n, t.warp / t.n,
			t.hold > 0 ? 100.0 * (t.hold - t.warp) / t.hold : 0.0);
		std::cout << line << "\n";
	}
	size_t best = 0;
	for (size_t vi = 1; vi < variants.size(); ++vi)
		if (perVariant[vi].hold - perVariant[vi].warp > perVariant[best].hold - perVariant[best].warp) best = vi;
	std::cout << "Beta real footage: every variant (share of the hold removed):\n";
	for (size_t vi = 0; vi < variants.size(); ++vi)
	{
		char row[256];
		std::snprintf(row, sizeof(row), "  %-52s %5.1f%% removed", variants[vi].name.c_str(),
			perVariant[vi].hold > 0 ? 100.0 * (perVariant[vi].hold - perVariant[vi].warp) / perVariant[vi].hold : 0.0);
		std::cout << row << "\n";
	}
	// Where the difference left over lies (today and the best setting): in
	// the pixels the warp moved, or in those left as A (held or still). The
	// first is mostly grain, blur and resampling; the second is what a better
	// matcher could still take.
	std::cout << "Beta real footage: the difference left (share of the hold's), moved pixels / pixels left as A (share of the picture):\n";
	for (const auto& c : splits)
		for (size_t vi : { static_cast<size_t>(0), best })
		{
			const Split& sp = c.second[vi];
			const double total = sp.hold[0] + sp.hold[1];
			if (total <= 0) continue;
			char line[400];
			std::snprintf(line, sizeof(line), "  %-12s %-34.34s moved: %5.1f%% of the picture, left %5.1f%%; left as A: %5.1f%% of the picture, left %5.1f%%",
				c.first.c_str(), variants[vi].name.c_str(), 100.0 * sp.n[0] / std::max(1LL, sp.n[0] + sp.n[1]), 100.0 * sp.warp[0] / total,
				100.0 * sp.n[1] / std::max(1LL, sp.n[0] + sp.n[1]), 100.0 * sp.warp[1] / total);
			std::cout << line << "\n";
		}
	const double today = perVariant[0].hold - perVariant[0].warp, bestBenefit = perVariant[best].hold - perVariant[best].warp;
	char line[400];
	std::snprintf(line, sizeof(line), "Beta real footage headroom (benefit summed): today %.2f; best single setting %.2f (%s, %+.1f%%); best per place %.2f (%+.1f%%)",
		today, bestBenefit, variants[best].name.c_str(), today > 0 ? 100.0 * (bestBenefit - today) / today : 0.0, oracle,
		today > 0 ? 100.0 * (oracle - today) / today : 0.0);
	std::cout << line << "\n";
	for (int f = 0; f < sources; ++f) { pl_tex_destroy(g, &lumaT[f]); pl_tex_destroy(g, &chromaT[f]); }
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
		std::string label;
		int w, h;
		double sourceRate;   // frames per second
		std::function<bool(int index, std::vector<uint16_t>& luma, std::vector<uint16_t>& chroma, std::string& segment)> next;
		int frames;
		bool hdr = false;
	};
	std::vector<Source> sources;
	const double displayRate = ScenarioDisplayRate(), filmRate = ScenarioFilmRate();
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
	// VP_TEST_SCENARIO_HDR=1: the synthetic sequences as PQ / BT.2020 (the
	// tone-mapping cost; the pictures are not graded PQ).
	const bool syntheticHdr = envOn("VP_TEST_SCENARIO_HDR");
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
		src.label = "synthetic " + RateText(filmRate) + "p, " + std::to_string(w) + "x" + std::to_string(h);
		src.w = w; src.h = h; src.sourceRate = filmRate; src.frames = total;
		src.hdr = syntheticHdr;
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
				src.label = (as24 ? "clip as " + RateText(filmRate) + "p, 3840x2160" : std::string("clip 29.97p, 3840x2160")) +
					(clipHdr ? ", pseudo-HDR (PQ, BT.2020)" : "");
				src.hdr = clipHdr;
				src.w = w; src.h = h; src.sourceRate = as24 ? filmRate : 30000.0 / 1001.0; src.frames = frames;
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
						&warp.GetSettings(), warp.Camera(), warp.PixelMask(), warp.VectorOk()), "scenario: warp");
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
		if (g_scenarioSink)
		{
			ScenarioTimingRecord record;
			record.label = src.label;
			record.extras = extras;
			record.displayRate = displayRate;
			record.sourceRate = src.sourceRate;
			record.series["matcher"] = aheadOrder && extras ? std::vector<double>() : matcherMs.v;
			record.series["new_frame"] = newFrame.v;
			record.series["extra"] = extra.v;
			record.series["repeat"] = repeat.v;
			record.series["critical"] = newWithMatcher.v;
			record.series["extra_with_matcher"] = extraWithMatcher.v;
			record.series["any_slot"] = anySlot.v;
			record.series["source_frame"] = perSourceFrame.v;
			int sourceFrames = 0;
			for (const auto& kind : perKind)
				if (kind.first == "new frame")
					sourceFrames = kind.second.second;
			for (const auto& kind : perKind)
				for (const auto& pass : kind.second.first)
					record.passPerSourceFrame[pass.first] += pass.second / std::max(1, sourceFrames);
			g_scenarioSink->push_back(std::move(record));
		}
		for (pl_tex* t : { &luma[0], &luma[1], &chroma[0], &chroma[1] })
			pl_tex_destroy(g, t);
	}
	pl_tex_destroy(g, &targetTex);
}

// The timing gate (owner 2026-10-13): every component and combination
// within its budget on this GPU, so no option or pairing of options brings a
// hiccup. Runs the scenario over the combinations (SDR / HDR, matcher size
// all / off, estimate ahead on / off, chroma Catmull-Rom / the upscaler) on
// the synthetic 4K and 1080p sequences, and the real clip (VP_TEST_CLIP,
// with VP_TEST_CLIP_PQ for its HDR copy) as film ahead on / off, at the
// scenario's rates (ScenarioDisplayRate / ScenarioFilmRate: 59.94 Hz and
// 23.976p unless VP_TEST_DISPLAY_RATE / VP_TEST_FILM_RATE). Budgets:
// tools/timing-budgets.cfg (or VP_TEST_BUDGETS), the section whose name is
// part of this GPU's name; a GPU without one is reported only. A budget is
// ms, or relative to the run's rates (display_period - 2.0, source_period).
// VP_TEST_TIMING_GATE=1 with VP_TEST_GPU=hardware.
void RunBetaTimingGate()
{
	std::string device;
	ExtendedRangeGpuProbe::TestDeviceParams(&device);
	if (!ExtendedRangeGpuProbe::OnHardwareGpu())
	{
		std::cout << "Beta timing gate: needs VP_TEST_GPU=hardware (WARP times mean nothing); skipped\n";
		return;
	}
	// The budgets file.
	auto envValue = [](const char* name)
	{
		char* v = nullptr;
		size_t n = 0;
		std::string value;
		if (_dupenv_s(&v, &n, name) == 0 && v) value = v;
		std::free(v);
		return value;
	};
	std::string budgetPath = envValue("VP_TEST_BUDGETS");
	if (budgetPath.empty())
	{
		char module[MAX_PATH] = {};
		HMODULE self = nullptr;
		GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCSTR>(&RunBetaTimingGate), &self);
		GetModuleFileNameA(self, module, MAX_PATH);
		std::string dir(module);
		for (int up = 0; up < 6 && budgetPath.empty(); ++up)
		{
			const size_t slash = dir.find_last_of("\\/");
			if (slash == std::string::npos) break;
			dir = dir.substr(0, slash);
			const std::string candidate = dir + "\\tools\\timing-budgets.cfg";
			if (GetFileAttributesA(candidate.c_str()) != INVALID_FILE_ATTRIBUTES)
				budgetPath = candidate;
		}
	}
	auto lower = [](std::string t) { for (char& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return t; };
	// A budget: ms = constant + display x display_period + source x
	// source_period, the periods (ms) of the rates the run used. "14.6" is
	// absolute; "display_period - 2.0" is one refresh with a 2 ms margin;
	// "source_period" one source frame; a factor is written "2*display_period".
	struct Budget
	{
		double constant = 0.0, display = 0.0, source = 0.0;
		std::string text;
		double Ms(const ScenarioTimingRecord& r) const
		{
			return constant + (display != 0.0 ? display * 1000.0 / r.displayRate : 0.0) +
				(source != 0.0 ? source * 1000.0 / r.sourceRate : 0.0);
		}
	};
	auto parseBudget = [](const std::string& text, Budget& b) -> bool
	{
		b = Budget{};
		const size_t first = text.find_first_not_of(" \t"), last = text.find_last_not_of(" \t");
		b.text = first == std::string::npos ? std::string() : text.substr(first, last - first + 1);
		size_t i = 0;
		auto skip = [&] { while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i; };
		bool any = false;
		for (;;)
		{
			skip();
			if (i >= text.size()) return any;
			double sign = 1.0;
			if (text[i] == '+' || text[i] == '-')
			{
				sign = text[i] == '-' ? -1.0 : 1.0;
				++i;
				skip();
			}
			else if (any)
				return false;   // terms are joined by + or -
			double factor = 1.0;
			if (i < text.size() && (std::isdigit(static_cast<unsigned char>(text[i])) || text[i] == '.'))
			{
				char* end = nullptr;
				factor = std::strtod(text.c_str() + i, &end);
				i = static_cast<size_t>(end - text.c_str());
				skip();
				if (i < text.size() && text[i] == '*') { ++i; skip(); }
				else { b.constant += sign * factor; any = true; continue; }   // a plain number: ms
			}
			const size_t start = i;
			while (i < text.size() && (std::isalnum(static_cast<unsigned char>(text[i])) || text[i] == '_')) ++i;
			const std::string name = text.substr(start, i - start);
			if (name == "display_period") b.display += sign * factor;
			else if (name == "source_period") b.source += sign * factor;
			else return false;
			any = true;
		}
	};
	std::map<std::string, Budget> budgets;
	std::string section;
	int unreadable = 0;
	{
		std::ifstream in(budgetPath);
		std::string line, current;
		bool matched = false;
		while (std::getline(in, line))
		{
			if (!line.empty() && line.back() == '\r') line.pop_back();
			const size_t hash = line.find('#');
			if (hash != std::string::npos) line = line.substr(0, hash);
			while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.pop_back();
			size_t start = 0;
			while (start < line.size() && std::isspace(static_cast<unsigned char>(line[start]))) ++start;
			line = line.substr(start);
			if (line.empty()) continue;
			if (line.front() == '[' && line.back() == ']')
			{
				current = line.substr(1, line.size() - 2);
				matched = !matched && lower(device).find(lower(current)) != std::string::npos;
				if (matched) section = current;
				continue;
			}
			if (!matched || current != section) continue;
			const size_t eq = line.find('=');
			if (eq == std::string::npos) continue;
			std::string key = line.substr(0, eq), value = line.substr(eq + 1);
			while (!key.empty() && std::isspace(static_cast<unsigned char>(key.back()))) key.pop_back();
			Budget b;
			if (parseBudget(value, b))
				budgets[key] = b;
			else
			{
				++unreadable;
				std::cout << "FAIL timing budget not understood: " << key << " =" << value <<
					" (ms, or display_period / source_period with + - and a factor, e.g. display_period - 2.0)\n";
			}
		}
	}
	std::cout << "Beta timing gate on " << device << ": budgets " << (section.empty() ? std::string("none for this GPU (report only)") :
		"[" + section + "] from " + budgetPath + " (" + std::to_string(budgets.size()) + " limits)") << "; display " <<
		RateText(ScenarioDisplayRate()) << " Hz (period " << 1000.0 / ScenarioDisplayRate() << " ms), film source " <<
		RateText(ScenarioFilmRate()) << "p (period " << 1000.0 / ScenarioFilmRate() << " ms)\n";
	Check(unreadable == 0, "timing gate: " + std::to_string(unreadable) + " budgets not understood");

	struct Combo { std::string name; bool hdr; std::string lowRes; bool ahead; std::string chroma; bool clip; };
	std::vector<Combo> combos;
	for (int hdr = 0; hdr < 2; ++hdr)
		for (const char* lowRes : { "all", "off" })
			for (int ahead = 0; ahead < 2; ++ahead)
				for (const char* chroma : { "catmull_rom", "" })
				{
					Combo c{ std::string(hdr ? "HDR" : "SDR") + ", matcher " + lowRes + ", ahead " + (ahead ? "on" : "off") +
						", chroma " + (*chroma ? chroma : "upscaler"), hdr != 0, lowRes, ahead != 0, chroma, false };
					combos.push_back(c);
				}
	const std::string clip = envValue("VP_TEST_CLIP"), clipPq = envValue("VP_TEST_CLIP_PQ");
	for (int hdr = 0; hdr < 2; ++hdr)
		for (int ahead = 0; ahead < 2; ++ahead)
			if (!(hdr ? clipPq : clip).empty())
				combos.push_back({ "clip as " + RateText(ScenarioFilmRate()) + "p, " + (hdr ? "HDR" : "SDR") + ", defaults, ahead " + (ahead ? "on" : "off"),
					hdr != 0, "all", ahead != 0, "catmull_rom", true });

	struct Result { std::string name, scope; ScenarioTimingRecord record; };
	std::vector<Result> results;
	for (const Combo& c : combos)
	{
		std::vector<ScenarioTimingRecord> sink;
		g_scenarioSink = &sink;
		_putenv_s("VP_TEST_LOWRES", c.lowRes.c_str());
		_putenv_s("VP_TEST_AHEAD", c.ahead ? "1" : "0");
		_putenv_s("VP_TEST_CHROMA", c.chroma.empty() ? "" : c.chroma.c_str());
		_putenv_s("VP_TEST_SCENARIO_HDR", c.hdr && !c.clip ? "1" : "0");
		_putenv_s("VP_TEST_SCENARIO_CLIP_ONLY", c.clip ? "1" : "0");
		_putenv_s("VP_TEST_CLIP_24P", c.clip ? "1" : "0");
		_putenv_s("VP_TEST_CLIP_HDR", c.clip && c.hdr ? "1" : "0");
		_putenv_s("VP_TEST_CLIP", c.clip ? (c.hdr ? clipPq : clip).c_str() : "");
		std::cout << "== timing gate: " << c.name << "\n";
		RunBetaScenarioTiming();
		g_scenarioSink = nullptr;
		for (ScenarioTimingRecord& r : sink)
			results.push_back({ c.name, c.lowRes == "off" ? "full" : "half", std::move(r) });
	}
	_putenv_s("VP_TEST_CLIP", clip.c_str());

	// The checks: "[half:|full:]<series>.<p99|max|mean> = ms" and
	// "[half:|full:]pass:<name part>.mean = ms" (GPU per source frame, all its
	// refreshes); a relative budget is computed from each run's own rates. The scope is the matcher size of the run (half = all or
	// search, full = off); a key without one applies to every run.
	auto stat = [](std::vector<double> v, const std::string& which) -> double
	{
		if (v.empty()) return -1.0;
		std::sort(v.begin(), v.end());
		if (which == "max") return v.back();
		if (which == "mean") { double m = 0.0; for (double x : v) m += x; return m / v.size(); }
		return v[std::min(v.size() - 1, static_cast<size_t>(v.size() * 0.99))];
	};
	auto budgetFor = [&](const std::string& scope, const std::string& key) -> const Budget*
	{
		auto found = budgets.find(scope + ":" + key);
		if (found == budgets.end()) found = budgets.find(key);
		return found == budgets.end() ? nullptr : &found->second;
	};
	int checked = 0, over = 0;
	struct Worst { double value = 0.0; std::string where; const ScenarioTimingRecord* record = nullptr; };
	std::map<std::string, Worst> worst;   // scope:key -> worst value, where, and that run (its rates)
	auto check = [&](const Result& r, const std::string& key, double value)
	{
		const std::string where = r.name + " | " + r.record.label;
		auto& w = worst[r.scope + ":" + key];
		if (value > w.value) w = { value, where, &r.record };
		if (const Budget* budget = budgetFor(r.scope, key))
		{
			++checked;
			const double limit = budget->Ms(r.record);
			if (value > limit)
			{
				++over;
				std::cout << "FAIL timing " << r.scope << ":" << key << " " << value << " ms > " << limit << " ms (" <<
					budget->text << " at " << RateText(r.record.sourceRate) << "p, " << RateText(r.record.displayRate) << " Hz): " << where << "\n";
			}
		}
	};
	for (const Result& r : results)
	{
		for (const auto& series : r.record.series)
			for (const char* which : { "p99", "max", "mean" })
			{
				const double value = stat(series.second, which);
				if (value >= 0.0)
					check(r, series.first + "." + which, value);
			}
		// Passes: each named in a budget (summed over the passes it names),
		// and every pass of 0.3 ms or more in the report.
		std::map<std::string, double> named;
		for (const auto& budget : budgets)
		{
			const size_t colon = budget.first.find(':');
			std::string key = budget.first;
			if (colon != std::string::npos && budget.first.compare(0, 5, "pass:") != 0)
			{
				if (budget.first.substr(0, colon) != r.scope) continue;
				key = budget.first.substr(colon + 1);
			}
			if (key.compare(0, 5, "pass:") != 0 || key.size() <= 10) continue;
			const std::string part = key.substr(5, key.size() - 10);   // drop "pass:" and ".mean"
			double value = 0.0;
			for (const auto& pass : r.record.passPerSourceFrame)
				if (pass.first.find(part) != std::string::npos)
					value += pass.second;
			named[key] = value;
		}
		for (const auto& n : named)
			check(r, n.first, n.second);
		for (const auto& pass : r.record.passPerSourceFrame)
			if (pass.second >= 0.3 && !named.count("pass:" + pass.first + ".mean"))
			{
				auto& w = worst[r.scope + ":pass:" + pass.first + ".mean"];
				if (pass.second > w.value) w = { pass.second, r.name + " | " + r.record.label, &r.record };
			}
	}
	std::cout << "Beta timing gate: worst over " << results.size() << " runs (" << combos.size() << " combinations):\n";
	for (const auto& w : worst)
	{
		const size_t colon = w.first.find(':');
		const Budget* budget = budgetFor(w.first.substr(0, colon), w.first.substr(colon + 1));
		std::string limit;
		if (budget && w.second.record)
			limit = " (budget " + std::to_string(budget->Ms(*w.second.record)) + " = " + budget->text + ")";
		std::cout << "  " << w.first << " " << w.second.value << " ms" << limit << "  at " << w.second.where << "\n";
	}
	std::cout << (over == 0 ? "PASS" : "FAIL") << " Beta timing gate: " << checked << " checks, " << over << " over budget" <<
		(section.empty() ? " (no budgets for this GPU: report only)" : "") << "\n";
	Check(over == 0, "timing gate: " + std::to_string(over) + " checks over budget");
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
