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

#include <libplacebo/d3d11.h>
#include <libplacebo/log.h>
#include <libplacebo/renderer.h>
#include <libplacebo/shaders/custom.h>

#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
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
			pl_d3d11_params deviceParams{};
			deviceParams.force_software = true;
			deviceParams.allow_software = true;
			d3d11 = log ? pl_d3d11_create(log, &deviceParams) : nullptr;
			renderer = d3d11 ? pl_renderer_create(log, d3d11->gpu) : nullptr;
			Check(renderer != nullptr, "no libplacebo WARP device");
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
		Pixel Render(float value, int colorDepth, const std::vector<const pl_hook*>& hooks = {})
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
			output.repr.levels = PL_COLOR_LEVELS_LIMITED;
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
