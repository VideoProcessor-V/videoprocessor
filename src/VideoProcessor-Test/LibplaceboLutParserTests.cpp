#include "pch.h"
#include "CppUnitTest.h"

#include <algorithm>
#include <array>
#include <vprenderer/LibplaceboDisplayLut.h>
#include <vprenderer/LibplaceboRenderParameters.h>
#include <vprenderer/LibplaceboCalibrationLutPolicy.h>
#include <vprenderer/LibplaceboOutputPolicy.h>
#include <vprenderer/AnamorphicPresentation.h>
#include <vprenderer/NativeStatsOverlayPlacement.h>
#include <vprenderer/NlsHookRecovery.h>
#include <vprenderer/CustomHookChain.h>
#include <vprenderer/DejudderWarp.h>
#include <vprenderer/OutputEncodingRepair.h>
#include <vprenderer/OcclusionMaskHook.h>
#include <vprenderer/SigmaDeltaDomain.h>
#include <vprenderer/ExtendedRange.h>
#include <vprenderer/RenderedWarp.h>
#include <vprenderer/AdaptiveDownscale.h>
#include "ExtendedRangeGpuProbe.h"
#include "ChainGpuProbe.h"
#include <vprenderer/NlsMaskMapping.h>
#include <led/LedMeasurementHook.h>
#include <led/LedRawTransfer.h>
#include <NlsGeometryPolicy.h>
#include <libplacebo/d3d11.h>
#include <libplacebo/renderer.h>
#include <libplacebo/shaders/custom.h>
#include <SubtitleCutPasteShader.h>

#include <cstdint>
#include <cmath>
#include <sstream>
#include <cstring>
#include <algorithm>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace LibplaceboDisplayLut;

namespace
{
	class TemporaryFile
	{
	public:
		TemporaryFile()
		{
			char directory[MAX_PATH] = {};
			GetTempPathA(ARRAYSIZE(directory), directory);
			char path[MAX_PATH] = {};
			GetTempFileNameA(directory, "vpl", 0, path);
			m_path = path;
		}

		~TemporaryFile()
		{
			if (!m_path.empty())
				DeleteFileA(m_path.c_str());
		}

		const std::string& Path() const { return m_path; }

		void Write(const char* contents) const
		{
			std::ofstream output(m_path, std::ios::binary | std::ios::trunc);
			output << contents;
		}

		void ResizeTo(size_t bytes) const
		{
			HANDLE file = CreateFileA(
				m_path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
				FILE_ATTRIBUTE_NORMAL, nullptr);
			Assert::IsTrue(file != INVALID_HANDLE_VALUE);
			LARGE_INTEGER offset{};
			offset.QuadPart = static_cast<LONGLONG>(bytes);
			Assert::IsTrue(SetFilePointerEx(file, offset, nullptr, FILE_BEGIN));
			Assert::IsTrue(SetEndOfFile(file));
			CloseHandle(file);
		}

	private:
		std::string m_path;
	};

	class TemporaryDirectory
	{
	public:
		TemporaryDirectory()
		{
			char tempPath[MAX_PATH] = {};
			Assert::IsTrue(GetTempPathA(ARRAYSIZE(tempPath), tempPath) > 0);
			char directory[MAX_PATH] = {};
			Assert::IsTrue(GetTempFileNameA(tempPath, "vpl", 0, directory) != 0);
			Assert::IsTrue(DeleteFileA(directory));
			Assert::IsTrue(CreateDirectoryA(directory, nullptr));
			m_path = directory;
		}

		~TemporaryDirectory()
		{
			for (const std::string& file : m_files)
				DeleteFileA(file.c_str());
			if (!m_path.empty())
				RemoveDirectoryA(m_path.c_str());
		}

		const std::string& Path() const { return m_path; }

		std::string Write(const char* fileName, const char* contents)
		{
			const std::string path = m_path + "\\" + fileName;
			std::ofstream output(path, std::ios::binary | std::ios::trunc);
			Assert::IsTrue(static_cast<bool>(output));
			output << contents;
			output.close();
			m_files.push_back(path);
			return path;
		}

	private:
		std::string m_path;
		std::vector<std::string> m_files;
	};

	void Free(LoadResult& result)
	{
		pl_lut_free(&result.lut);
	}

	const char* Valid3dCube =
		"TITLE \"VP012 3D loader test\"\n"
		"LUT_3D_SIZE 2\n"
		"0.0 0.0 0.0\n"
		"1.0 0.0 0.0\n"
		"0.0 1.0 0.0\n"
		"1.0 1.0 0.0\n"
		"0.0 0.0 1.0\n"
		"1.0 0.0 1.0\n"
		"0.0 1.0 1.0\n"
		"1.0 1.0 1.0\n";

	const char* Valid1dCube =
		"TITLE \"VP012 1D rejection test\"\n"
		"LUT_1D_SIZE 2\n"
		"0.0 0.0 0.0\n"
		"1.0 1.0 1.0\n";

	const char* Green3dCube =
		"TITLE \"VP012 target LUT GPU test\"\n"
		"LUT_3D_SIZE 2\n"
		"0.0 1.0 0.0\n"
		"0.0 1.0 0.0\n"
		"0.0 1.0 0.0\n"
		"0.0 1.0 0.0\n"
		"0.0 1.0 0.0\n"
		"0.0 1.0 0.0\n"
		"0.0 1.0 0.0\n"
		"0.0 1.0 0.0\n";

	// Encodes the LUT input's red coordinate as green and its inverse as red.
	// A 25% linear-light gray encoded with Gamma 2.2 reaches the LUT near 53%,
	// so green must exceed red if the target LUT receives gamma-coded RGB.
	const char* GammaCoordinateProbeCube =
		"TITLE \"VP0166 gamma-coordinate probe\"\n"
		"LUT_3D_SIZE 2\n"
		"1.0 0.0 0.0\n"
		"0.0 1.0 0.0\n"
		"1.0 0.0 0.0\n"
		"0.0 1.0 0.0\n"
		"1.0 0.0 0.0\n"
		"0.0 1.0 0.0\n"
		"1.0 0.0 0.0\n"
		"0.0 1.0 0.0\n";

	struct RgbaPixel
	{
		uint8_t r;
		uint8_t g;
		uint8_t b;
		uint8_t a;
	};

    struct AnamorphicGpuCase
    {
        pl_rect2df sourceCrop{};
        int outputWidth = 192, outputHeight = 108;
        bool configured = true;
        double screenAspect = 64.0/27.0, lens = 4.0/3.0;
        pl_rect2df submittedCrop{};
        bool retried = false;
        bool overlay = false, cropLocalOverlay = false;
    };

	struct RenderStepCapture
	{
		bool errorDiffusion = false;

		static void Callback(void* privateData, const pl_render_info* info)
		{
			RenderStepCapture* capture =
				static_cast<RenderStepCapture*>(privateData);
			if (!capture || !info || !info->pass || !info->pass->shader)
				return;
			const pl_shader_info shader = info->pass->shader;
			for (int index = 0; index < shader->num_steps; ++index)
			{
				std::string step = shader->steps[index] ? shader->steps[index] : "";
				std::transform(step.begin(), step.end(), step.begin(),
					[](unsigned char value) {
						return static_cast<char>(std::tolower(value));
					});
				if (step.find("error diffusion") != std::string::npos ||
					step.find("error-diffusion") != std::string::npos)
					capture->errorDiffusion = true;
			}
		}
	};

	std::string LoadBundledShader(const char* fileName)
	{
		std::string path = __FILE__;
		for (int level = 0; level < 3; ++level)
		{
			const size_t separator = path.find_last_of("\\/");
			if (separator == std::string::npos)
				return {};
			path.resize(separator);
		}
		path += "\\shaders\\";
		path += fileName;
		std::ifstream input(path, std::ios::binary);
		return std::string(std::istreambuf_iterator<char>(input),
			std::istreambuf_iterator<char>());
	}

	void ReplaceShaderToken(std::string& source, const std::string& name,
		const std::string& value)
	{
		const std::string token = "{{" + name + "}}";
		size_t position = 0;
		while ((position = source.find(token, position)) != std::string::npos)
		{
			source.replace(position, token.size(), value);
			position += value.size();
		}
	}

	const pl_hook* ParseBundledNlsShader(pl_gpu gpu, const char* fileName,
		double axisBalance, double strength = 1.0)
	{
		std::string source = LoadBundledShader(fileName);
		Assert::IsFalse(source.empty(), L"Bundled NLS shader was not found");
		const std::map<std::string, std::string> parameters = {
			{ "strength", std::to_string(strength) },
			{ "curve", "2.0" },
			{ "geometry", "1" },
			{ "center_protection", "0.35" },
			{ "axis_balance", std::to_string(axisBalance) },
			{ "max_center_zoom", "1.08" },
			{ "horizontal_center_protection", "0.35" },
			{ "vertical_center_protection", "0.25" }
		};
		for (const auto& parameter : parameters)
			ReplaceShaderToken(source, parameter.first, parameter.second);
		Assert::IsTrue(source.find("{{") == std::string::npos,
			L"Bundled NLS shader still contains an unsubstituted token");
		const pl_hook* hook = pl_mpv_user_shader_parse(
			gpu, source.data(), source.size());
		Assert::IsNotNull(hook, L"libplacebo rejected bundled NLS shader");
		return hook;
	}

	void BindNlsShader(const pl_hook* hook, float stretchRatio, float warpAxis)
	{
		bool stretchBound = false;
		bool axisBound = false;
		for (int index = 0; index < hook->num_parameters; ++index)
		{
			const pl_hook_par& parameter = hook->parameters[index];
			if (!parameter.name || !parameter.data ||
				parameter.type != PL_VAR_FLOAT)
				continue;
			if (strcmp(parameter.name, "stretch_ratio") == 0)
			{
				parameter.data->f = stretchRatio;
				stretchBound = true;
			}
			else if (strcmp(parameter.name, "warp_axis") == 0)
			{
				parameter.data->f = warpAxis;
				axisBound = true;
			}
		}
		Assert::IsTrue(stretchBound && axisBound,
			L"Bundled NLS shader did not expose both dynamic parameters");
	}

	struct LuminanceCase
	{
		float targetNits = 203.0f;
		float blackNits = 0.203f;
		bool legacy = false;
		bool hdr = false;
		bool deband = false;
		pl_color_primaries primaries = PL_COLOR_PRIM_BT_709;
		int outputSize = 64;
	};

    pl_color_transfer TestTransfer(LibplaceboOutput::SdrTransfer transfer)
    {
        using LibplaceboOutput::SdrTransfer;
        switch (transfer)
        {
        case SdrTransfer::BT1886: return PL_COLOR_TRC_BT_1886;
        case SdrTransfer::SRGB: return PL_COLOR_TRC_SRGB;
        case SdrTransfer::GAMMA18: return PL_COLOR_TRC_GAMMA18;
        case SdrTransfer::GAMMA20: return PL_COLOR_TRC_GAMMA20;
        case SdrTransfer::GAMMA22: return PL_COLOR_TRC_GAMMA22;
        case SdrTransfer::GAMMA24: return PL_COLOR_TRC_GAMMA24;
        case SdrTransfer::GAMMA26: return PL_COLOR_TRC_GAMMA26;
        case SdrTransfer::GAMMA28: return PL_COLOR_TRC_GAMMA28;
        default: Assert::Fail(L"GPU case has no concrete SDR transfer");
        }
        return PL_COLOR_TRC_UNKNOWN;
    }

    int PixelDistance(RgbaPixel a, RgbaPixel b)
    {
        return std::abs(static_cast<int>(a.r) - b.r) +
            std::abs(static_cast<int>(a.g) - b.g) +
            std::abs(static_cast<int>(a.b) - b.b);
    }

	class TargetLutGpuFixture
	{
	public:
		~TargetLutGpuFixture()
		{
			if (m_renderer)
				pl_renderer_destroy(&m_renderer);
			if (m_d3d11)
				pl_d3d11_destroy(&m_d3d11);
			if (m_log)
				pl_log_destroy(&m_log);
		}

		bool Create()
		{
			pl_log_params logParams{};
			logParams.log_level = PL_LOG_NONE;
			m_log = pl_log_create(PL_API_VER, &logParams);
			if (!m_log)
				return false;

			// WARP unless VP_TEST_GPU=hardware (ExtendedRangeGpuProbe::TestDeviceParams).
			std::string device;
			const pl_d3d11_params deviceParams = ExtendedRangeGpuProbe::TestDeviceParams(&device);
			static bool reported = false;
			if (!reported)
				Logger::WriteMessage(("GPU tests on: " + device + "\n").c_str());
			reported = true;
			m_d3d11 = pl_d3d11_create(m_log, &deviceParams);
			if (!m_d3d11)
				return false;

			m_renderer = pl_renderer_create(m_log, m_d3d11->gpu);
			return m_renderer != nullptr;
		}

		RgbaPixel Render(const pl_custom_lut* lut)
		{
			return Render(lut, pl_render_fast_params,
				PL_COLOR_LEVELS_FULL, PL_LUT_NORMALIZED);
		}

		RgbaPixel Render(
			const pl_custom_lut* lut,
			const struct pl_render_params& params,
			enum pl_color_levels targetLevels = PL_COLOR_LEVELS_FULL,
			enum pl_lut_type lutType = PL_LUT_NORMALIZED,
			RgbaPixel sourcePixel = { 255, 0, 0, 255 },
			enum pl_color_transfer sourceTransfer = PL_COLOR_TRC_SRGB,
			enum pl_color_transfer targetTransfer = PL_COLOR_TRC_SRGB,
			enum pl_color_primaries sourcePrimaries = PL_COLOR_PRIM_BT_709,
			enum pl_color_primaries targetPrimaries = PL_COLOR_PRIM_BT_709,
			float sourceMaxNits = 0.0f,
			float targetMaxNits = 0.0f,
            bool useSdrReference = false)
		{
			pl_gpu gpu = m_d3d11->gpu;
			const enum pl_fmt_caps requiredCaps = static_cast<enum pl_fmt_caps>(
				PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE);
			pl_fmt format = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 8, 8, requiredCaps);
			Assert::IsNotNull(format, L"No host-readable RGBA8 render format is available");

			const RgbaPixel sourcePixels[4] = {
				sourcePixel, sourcePixel, sourcePixel, sourcePixel,
			};
			pl_tex_params sourceParams{};
			sourceParams.w = 2;
			sourceParams.h = 2;
			sourceParams.format = format;
			sourceParams.sampleable = true;
			sourceParams.initial_data = sourcePixels;
			pl_tex sourceTexture = pl_tex_create(gpu, &sourceParams);
			Assert::IsNotNull(sourceTexture);

			pl_tex_params targetParams{};
			targetParams.w = 2;
			targetParams.h = 2;
			targetParams.format = format;
			targetParams.renderable = true;
			targetParams.host_readable = true;
			pl_tex targetTexture = pl_tex_create(gpu, &targetParams);
			Assert::IsNotNull(targetTexture);

			pl_frame image = MakeRgbFrame(sourceTexture);
			pl_frame target = MakeRgbFrame(targetTexture);
			image.color.transfer = sourceTransfer;
			image.color.primaries = sourcePrimaries;
			image.color.hdr.max_luma = sourceMaxNits;
			if (sourceMaxNits > 0.0f)
				image.color.hdr.min_luma = PL_COLOR_HDR_BLACK;
			image.color.hdr.max_cll = sourceMaxNits;
			image.color.hdr.max_fall = sourceMaxNits;
			target.repr.levels = targetLevels;
			target.color.transfer = targetTransfer;
			target.color.primaries = targetPrimaries;
			target.color.hdr.max_luma = targetMaxNits;
			if (targetMaxNits > 0.0f)
				target.color.hdr.min_luma = PL_COLOR_HDR_BLACK;
            if (useSdrReference)
            {
                LibplaceboRenderParameters::ApplySourceLuminance(true, image.color);
                LibplaceboRenderParameters::ApplyTargetLuminance(true, 100.0f, 0.0f, target.color);
            }
			target.lut = lut;
			target.lut_type = lut ? lutType : PL_LUT_UNKNOWN;
			Assert::IsTrue(pl_render_image(m_renderer, &image, &target, &params));
			pl_gpu_finish(gpu);

			RgbaPixel result[4] = {};
			pl_tex_transfer_params download{};
			download.tex = targetTexture;
			download.ptr = result;
			Assert::IsTrue(pl_tex_download(gpu, &download));
			pl_tex_destroy(gpu, &targetTexture);
			pl_tex_destroy(gpu, &sourceTexture);
			return result[0];
		}

        std::vector<int> RenderGammaRamp(int bits, bool limited,
            pl_color_transfer sourceTransfer, pl_color_transfer targetTransfer,
            const pl_custom_lut* lut = nullptr)
        {
            pl_gpu gpu = m_d3d11->gpu;
            constexpr int count = 33;
            std::vector<float> input(count * 4);
            for (int i = 0; i < count; ++i)
            {
                input[4*i] = input[4*i+1] = input[4*i+2] = i / 32.0f;
                input[4*i+3] = 1.0f;
            }
            auto sourceFormat = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32, PL_FMT_CAP_SAMPLEABLE);
            Assert::IsNotNull(sourceFormat);
            pl_fmt targetFormat = nullptr;
            const auto caps = PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE;
            for (int i = 0; i < gpu->num_formats; ++i)
            {
                auto f = gpu->formats[i];
                if (f->type == PL_FMT_UNORM && f->num_components == 4 &&
                    f->component_depth[0] == bits && f->component_depth[1] == bits &&
                    f->component_depth[2] == bits && f->texel_size == 4 &&
                    (f->caps & caps) == caps && f->host_bits[0] == bits)
                { targetFormat = f; break; }
            }
            Assert::IsNotNull(targetFormat, L"Required RGBA8/RGB10A2 readback format unavailable");
            pl_tex_params sp{}; sp.w=count; sp.h=1; sp.format=sourceFormat;
            sp.sampleable=true; sp.initial_data=input.data();
            pl_tex source = pl_tex_create(gpu, &sp);
            pl_tex_params tp{}; tp.w=count; tp.h=1; tp.format=targetFormat;
            tp.renderable=true; tp.host_readable=true;
            pl_tex target = pl_tex_create(gpu, &tp);
            Assert::IsNotNull(source); Assert::IsNotNull(target);
            auto image = MakeRgbFrame(source); auto output = MakeRgbFrame(target);
            image.color.transfer=sourceTransfer; output.color.transfer=targetTransfer;
            LibplaceboRenderParameters::ApplySourceLuminance(true, image.color);
            LibplaceboRenderParameters::ApplyTargetLuminance(true, 100.0f, 0.0f, output.color);
            output.repr.levels=limited ? PL_COLOR_LEVELS_LIMITED : PL_COLOR_LEVELS_FULL;
            output.repr.bits.sample_depth=bits; output.repr.bits.color_depth=bits;
            output.lut=lut; output.lut_type=lut ? PL_LUT_NORMALIZED : PL_LUT_UNKNOWN;
            auto params=pl_render_fast_params;
            params.dither_params=nullptr; params.error_diffusion=nullptr;
            Assert::IsTrue(pl_render_image(m_renderer, &image, &output, &params));
            pl_gpu_finish(gpu);
            std::vector<uint32_t> downloaded(count);
            pl_tex_transfer_params download{}; download.tex=target; download.ptr=downloaded.data();
            Assert::IsTrue(pl_tex_download(gpu, &download));
            std::vector<int> codes;
            const unsigned mask=(1u << bits)-1;
            // Every RGB component has the same grayscale value, irrespective of
            // RGBA/BGRA ordering. Packed formats on D3D11 put RGB before alpha.
            for (auto pixel : downloaded) codes.push_back(static_cast<int>(pixel & mask));
            pl_tex_destroy(gpu,&source); pl_tex_destroy(gpu,&target);
            return codes;
        }

        void ResetFailedHook(uint64_t signature)
        {
            NlsHookRecovery::ResetFailedHook(m_renderer, signature);
        }

		pl_gpu Gpu() const
		{
			return m_d3d11 ? m_d3d11->gpu : nullptr;
		}

		pl_log Log() const { return m_log; }
		pl_renderer Renderer() const { return m_renderer; }


		// Routes libplacebo warnings and errors into `sink` (nullptr restores
		// silence), so a failing assertion can say why a shader was rejected.
		void CaptureLog(std::string* sink)
		{
			pl_log_params logParams{};
			logParams.log_level = sink ? PL_LOG_WARN : PL_LOG_NONE;
			logParams.log_priv = sink;
			if (sink)
				logParams.log_cb = [](void* priv, enum pl_log_level, const char* message)
				{
					*static_cast<std::string*>(priv) += std::string(message) + "\n";
				};
			pl_log_update(m_log, &logParams);
		}

		pl_render_errors Errors() const
		{
			return pl_renderer_get_errors(m_renderer);
		}

		bool DetectedHdrMetadata(pl_hdr_metadata& metadata) const
		{
			return pl_renderer_get_hdr_metadata(m_renderer, &metadata);
		}

		// Renders a sequence of frames through one hook (full-range 8-bit
		// target, same colour space in and out, no dither) and returns the red
		// codes of each frame. code(frame, x, y) is the source value in 8-bit
		// code units, with fractions (the source texture is 32-bit float).
		template <typename Code>
		std::vector<std::vector<int>> RenderSequence(const pl_hook* hook,
			int width, int height, int frames, Code code)
		{
			return RenderSequence(std::vector<const pl_hook*>{ hook }, width, height, frames, code);
		}

		template <typename Code>
		std::vector<std::vector<int>> RenderSequence(const std::vector<const pl_hook*>& hooks,
			int width, int height, int frames, Code code)
		{
			pl_gpu gpu = m_d3d11->gpu;
			pl_fmt sourceFormat = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32, PL_FMT_CAP_SAMPLEABLE);
			pl_fmt targetFormat = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 8, 8,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE));
			Assert::IsNotNull(sourceFormat);
			Assert::IsNotNull(targetFormat);
			pl_tex_params targetParams{};
			targetParams.w = width;
			targetParams.h = height;
			targetParams.format = targetFormat;
			targetParams.renderable = true;
			targetParams.host_readable = true;
			pl_tex targetTexture = pl_tex_create(gpu, &targetParams);
			Assert::IsNotNull(targetTexture);
			pl_render_params params = pl_render_fast_params;
			params.dither_params = nullptr;
			params.error_diffusion = nullptr;
			params.hooks = hooks.data();
			params.num_hooks = static_cast<int>(hooks.size());
			std::vector<std::vector<int>> result;
			std::vector<float> input(static_cast<size_t>(width) * height * 4);
			std::vector<RgbaPixel> output(static_cast<size_t>(width) * height);
			for (int frame = 0; frame < frames; ++frame)
			{
				for (int y = 0; y < height; ++y)
					for (int x = 0; x < width; ++x)
					{
						const float value = static_cast<float>(code(frame, x, y)) / 255.0f;
						float* texel = &input[(static_cast<size_t>(y) * width + x) * 4];
						texel[0] = texel[1] = texel[2] = value;
						texel[3] = 1.0f;
					}
				pl_tex_params sourceParams{};
				sourceParams.w = width;
				sourceParams.h = height;
				sourceParams.format = sourceFormat;
				sourceParams.sampleable = true;
				sourceParams.initial_data = input.data();
				pl_tex sourceTexture = pl_tex_create(gpu, &sourceParams);
				Assert::IsNotNull(sourceTexture);
				pl_frame image = MakeRgbFrame(sourceTexture);
				pl_frame target = MakeRgbFrame(targetTexture);
				Assert::IsTrue(pl_render_image(m_renderer, &image, &target, &params));
				pl_gpu_finish(gpu);
				pl_tex_transfer_params download{};
				download.tex = targetTexture;
				download.ptr = output.data();
				Assert::IsTrue(pl_tex_download(gpu, &download));
				pl_tex_destroy(gpu, &sourceTexture);
				std::vector<int> red(output.size());
				for (size_t i = 0; i < output.size(); ++i) red[i] = output[i].r;
				result.push_back(std::move(red));
			}
			pl_tex_destroy(gpu, &targetTexture);
			return result;
		}

		// Renders a constant grey (`value`, 0..1 full-range RGB) for `frames`
		// refreshes into a 10-bit target set up like VP's default swapchain
		// (R10G10B10A2, colour depth 8, bit shift 2) and returns the 10-bit red
		// code of one pixel per refresh.
		// colorDepth 8 = VP's swapchain set-up (8-bit precision in 10 bits);
		// 10 = a true 10-bit output (delta 5).
		std::vector<int> RenderTenBitSequence(const std::vector<const pl_hook*>& hooks,
			enum pl_color_levels levels, float value, int frames, std::vector<int>* alphas = nullptr,
			int colorDepth = 8)
		{
			pl_gpu gpu = m_d3d11->gpu;
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
			Assert::IsNotNull(sourceFormat);
			Assert::IsNotNull(targetFormat, L"No host-readable RGB10A2 target format");
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
			Assert::IsNotNull(source);
			Assert::IsNotNull(target);
			pl_frame image = MakeRgbFrame(source);
			pl_frame output = MakeRgbFrame(target);
			output.repr.levels = levels;
			output.repr.bits.sample_depth = 10;
			output.repr.bits.color_depth = colorDepth;
			output.repr.bits.bit_shift = 10 - colorDepth;
			pl_render_params params = pl_render_fast_params;
			params.dither_params = nullptr;
			params.error_diffusion = nullptr;
			params.hooks = hooks.empty() ? nullptr : hooks.data();
			params.num_hooks = static_cast<int>(hooks.size());
			std::vector<int> codes;
			for (int frame = 0; frame < frames; ++frame)
			{
				Assert::IsTrue(pl_render_image(m_renderer, &image, &output, &params));
				pl_gpu_finish(gpu);
				uint32_t pixels[4] = {};
				pl_tex_transfer_params download{};
				download.tex = target;
				download.ptr = pixels;
				Assert::IsTrue(pl_tex_download(gpu, &download));
				codes.push_back(static_cast<int>(pixels[0] & 1023u));
				if (alphas)
					alphas->push_back(static_cast<int>(pixels[0] >> 30));
			}
			pl_tex_destroy(gpu, &source);
			pl_tex_destroy(gpu, &target);
			return codes;
		}

		std::vector<RgbaPixel> RenderCoordinateField(
			const pl_hook* hook, int width = 64, int height = 64,
			const LuminanceCase* luminance = nullptr, AnamorphicGpuCase* geometry = nullptr)
		{
			pl_gpu gpu = m_d3d11->gpu;
			const enum pl_fmt_caps requiredCaps = static_cast<enum pl_fmt_caps>(
				PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_RENDERABLE |
				PL_FMT_CAP_HOST_READABLE);
			pl_fmt format = pl_find_fmt(
				gpu, PL_FMT_UNORM, 4, 8, 8, requiredCaps);
			Assert::IsNotNull(format);

			std::vector<RgbaPixel> sourcePixels(width * height);
			for (int y = 0; y < height; ++y)
				for (int x = 0; x < width; ++x)
					sourcePixels[y * width + x] = {
						static_cast<uint8_t>(x * 255 / (width - 1)),
						static_cast<uint8_t>(y * 255 / (height - 1)),
						static_cast<uint8_t>((x + y) * 255 /
							(width + height - 2)), 255 };

			pl_tex_params sourceParams{};
			sourceParams.w = width;
			sourceParams.h = height;
			sourceParams.format = format;
			sourceParams.sampleable = true;
			sourceParams.initial_data = sourcePixels.data();
			pl_tex sourceTexture = pl_tex_create(gpu, &sourceParams);
			Assert::IsNotNull(sourceTexture);

			pl_tex_params targetParams{};
			targetParams.blit_dst = geometry != nullptr; // pl_frame_clear requires this capability.
			targetParams.w = geometry ? geometry->outputWidth : (luminance ? luminance->outputSize : width);
			targetParams.h = geometry ? geometry->outputHeight : (luminance ? luminance->outputSize : height);
			targetParams.format = format;
			targetParams.renderable = true;
			targetParams.host_readable = true;
			pl_tex targetTexture = pl_tex_create(gpu, &targetParams);
			Assert::IsNotNull(targetTexture);

			pl_frame image = MakeRgbFrame(sourceTexture);
			pl_frame target = MakeRgbFrame(targetTexture);
			pl_render_params params = pl_render_fast_params;
			if (luminance)
			{
				params = pl_render_high_quality_params;
				params.dither_params = nullptr; // deterministic readback
				params.deband_params = luminance->deband ? &pl_deband_default_params : nullptr;
				image.color.primaries = luminance->primaries;
				image.color.transfer = luminance->hdr ? PL_COLOR_TRC_PQ : PL_COLOR_TRC_SRGB;
				image.color.hdr.max_luma = luminance->hdr ? 1000.0f : luminance->targetNits;
				image.color.hdr.min_luma = luminance->hdr ? 0.005f : luminance->blackNits;
				target.color.hdr.max_luma = luminance->targetNits;
				target.color.hdr.min_luma = luminance->blackNits;
				if (!luminance->legacy)
				{
					LibplaceboRenderParameters::ApplySourceLuminance(!luminance->hdr, image.color);
					LibplaceboRenderParameters::ApplyTargetLuminance(!luminance->hdr,
						luminance->targetNits, luminance->blackNits, target.color);
				}
			}
			if (hook)
			{
				params.hooks = &hook;
				params.num_hooks = 1;
			}
            if (geometry)
            {
                using namespace AlphaSourceCrop;
                const auto screen = AnamorphicPresentation::FitScreen(geometry->configured,
                    geometry->screenAspect, geometry->lens,
                    {0,0,static_cast<double>(targetParams.w),static_cast<double>(targetParams.h)},
                    VerticalPictureAlignment::CENTER, 0);
                image.crop = geometry->sourceCrop;
                const auto fit = AnamorphicPresentation::FitPicture(pl_rect2df_aspect(&image.crop),
                    geometry->lens, screen, VerticalPictureAlignment::CENTER);
                const auto rect = hook ? screen.rect : fit.picture;
                Assert::IsTrue(targetTexture->params.blit_dst, L"Recovery target must support clearing");
                const pl_frame fullTarget = target;
                target.crop = {static_cast<float>(rect.left),static_cast<float>(rect.top),
                    static_cast<float>(rect.right),static_cast<float>(rect.bottom)};
                const float black[] = {0,0,0};
                pl_frame_clear(gpu, &fullTarget, black);
                pl_tex overlayTexture = nullptr;
                pl_overlay overlay{};
                pl_overlay_part part{};
                if (geometry->overlay)
                {
                    std::vector<RgbaPixel> bitmap(40*20, RgbaPixel{255,0,255,255});
                    pl_tex_params bitmapParams = sourceParams;
                    bitmapParams.w = 40; bitmapParams.h = 20;
                    bitmapParams.initial_data = bitmap.data();
                    overlayTexture = pl_tex_create(gpu, &bitmapParams);
                    Assert::IsNotNull(overlayTexture);
                    overlay.tex = overlayTexture;
                    overlay.mode = PL_OVERLAY_NORMAL;
                    overlay.coords = geometry->cropLocalOverlay ? PL_OVERLAY_COORDS_DST_CROP : PL_OVERLAY_COORDS_DST_FRAME;
                    overlay.repr = pl_color_repr_rgb;
                    overlay.repr.alpha = PL_ALPHA_INDEPENDENT;
                    overlay.color = pl_color_space_srgb;
                    part.src = {0,0,40,20};
                    const auto placement = NativeStatsOverlayPlacement::PlaceTopLeft(
                        {target.crop.x0,target.crop.y0,target.crop.x1,target.crop.y1},
                        {0,0,static_cast<float>(targetParams.w),static_cast<float>(targetParams.h)},
                        AnamorphicPresentation::OverlayWidth(40, geometry->lens), 20, 4);
                    const float ox = geometry->cropLocalOverlay ? target.crop.x0 : 0;
                    const float oy = geometry->cropLocalOverlay ? target.crop.y0 : 0;
                    part.dst = {placement.panel.left-ox,placement.panel.top-oy,
                        placement.panel.right-ox,placement.panel.bottom-oy};
                    overlay.parts = &part; overlay.num_parts = 1;
                    target.overlays = &overlay; target.num_overlays = 1;
                }
                const auto result = NlsHookRecovery::Render(m_renderer, gpu, image, target,
                    fullTarget, params, hook, [&]()
                    {
                        target.crop = {static_cast<float>(fit.picture.left),static_cast<float>(fit.picture.top),
                            static_cast<float>(fit.picture.right),static_cast<float>(fit.picture.bottom)};
                    });
                Assert::IsTrue(result.rendered);
                geometry->retried = result.retried;
                geometry->submittedCrop = target.crop;
                pl_tex_destroy(gpu, &overlayTexture);
            }
            else
                Assert::IsTrue(pl_render_image(m_renderer, &image, &target, &params));
			pl_gpu_finish(gpu);

			std::vector<RgbaPixel> result(targetParams.w * targetParams.h);
			pl_tex_transfer_params download{};
			download.tex = targetTexture;
			download.ptr = result.data();
			Assert::IsTrue(pl_tex_download(gpu, &download));
			pl_tex_destroy(gpu, &targetTexture);
			pl_tex_destroy(gpu, &sourceTexture);
			return result;
		}

		std::vector<RgbaPixel> RenderSubtitlePattern(const pl_hook* hook, bool hdrP010, int phase,
			const SubtitleCutPasteGeometry& geometry, pl_color_transfer transfer=PL_COLOR_TRC_UNKNOWN,
			bool blackBar=false, bool oldSubtitleCard=false,
			int sceneGray=-1, bool diagonalStair=false, int cardGray=0,
            const SubtitleBoxRect* independentCard=nullptr, bool originalCornerInk=false, bool fringeSceneryInk=false,
            const pl_rect2df* sourceCrop=nullptr,const pl_hook* additionalHook=nullptr, bool brightBottomFringe=false, int brightFringeY=-1, bool auxiliarySamples=true, bool reconstructionPattern=false,
            const std::vector<RgbaPixel>* recordedPixels=nullptr, int width=64, int height=64)
		{

			const auto gpu=Gpu();
			const auto outputFormat=pl_find_fmt(gpu,PL_FMT_UNORM,4,8,8,
				static_cast<pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE|PL_FMT_CAP_RENDERABLE|PL_FMT_CAP_HOST_READABLE));
			Assert::IsNotNull(outputFormat);
			std::vector<RgbaPixel> pixels(width*height);
			std::vector<uint16_t> luma(width*height),chroma(width*height/2,512<<6);
			for(int y=0;y<height;++y) for(int x=0;x<width;++x) {
				pixels[y*width+x]={uint8_t((x*3+phase*43)%256),uint8_t((y*3+phase*57)%256),uint8_t((x+y+phase*71)%256),255};
				luma[y*width+x]=uint16_t((64+(x*3+y*7+phase*113)%800)<<6);
			}
			if(sceneGray>=0) for(int i=0;i<width*height;++i) {
				const auto gray=uint8_t((std::min)(sceneGray,255));
				pixels[i]={gray,gray,gray,255};
				luma[i]=uint16_t((64+gray*3)<<6);
			}
			// A bright descending balustrade edge above the cleanup region must not
			// be reflected into the old subtitle card below it.
			if(diagonalStair) for(int y=17;y<=21;++y)
				for(int x=20+2*(y-19);x<=44+2*(y-19);++x) {
					pixels[y*width+x]={245,245,245,255};
					luma[y*width+x]=uint16_t(800<<6);
				}
            // Independent two-dimensional texture for reconstruction filter tests.
            if(reconstructionPattern) for(int y=0;y<height;++y)for(int x=0;x<width;++x) {
                const uint8_t v=((x/4+y/4)%2)?220:32;
                pixels[y*width+x]={v,v,v,255};luma[y*width+x]=uint16_t((64+v*3)<<6);
            }
            if(blackBar) for(int y=0;y<(std::min)(height,geometry.pictureTop);++y)
                for(int x=0;x<width;++x) {
                    pixels[y*width+x]={0,0,0,255};luma[y*width+x]=uint16_t(64<<6);
                }
			if(blackBar) for(int y=(std::max)(0,geometry.pictureBottom);y<height;++y)
				for(int x=0;x<width;++x) {
					pixels[y*width+x]={0,0,0,255};luma[y*width+x]=uint16_t(64<<6);
				}
            const SubtitleBoxRect card=independentCard ? *independentCard :
                SubtitleBoxRect{geometry.source.left+1,geometry.source.top+1,
                    geometry.source.right-1,geometry.pictureBottom};
            if(oldSubtitleCard) for(int y=(std::max)(0,card.top);
                y<(std::min)(card.bottom,geometry.pictureBottom);++y)
                for(int x=(std::max)(0,card.left);x<(std::min)(card.right,width);++x) {
                    // An independent rounded card must not be manufactured from
                    // the geometry that the shader is being asked to validate.
                    if(independentCard && y<card.top+4) {
                        const int dx=(std::max)(card.left+4-x,x-(card.right-5));
                        const int dy=card.top+4-y;
                        if(dx>0 && dx*dx+dy*dy>16) continue;
                    }
                    pixels[y*width+x]={uint8_t(cardGray),uint8_t(cardGray),uint8_t(cardGray),255};
                    luma[y*width+x]=uint16_t((64+cardGray*3)<<6);
                }
			if(oldSubtitleCard && auxiliarySamples) {
				const int nearby=geometry.pictureBottom-2;
				pixels[nearby*width+geometry.source.left+1]={8,8,8,255};
				luma[nearby*width+geometry.source.left+1]=uint16_t(90<<6);
			}
			// A simple white glyph and controlled destination/source samples make
			// every experimental backing mode observable for every test geometry.
			const int glyphLeft=geometry.content.left+(geometry.content.right-geometry.content.left)/2-2;
			const int glyphTop=geometry.content.top+(geometry.content.bottom-geometry.content.top)/2-5;
			for(int y=glyphTop;y<glyphTop+11;++y)for(int x=glyphLeft;x<glyphLeft+4;++x) {
				pixels[y*width+x]={245,245,245,255};luma[y*width+x]=uint16_t(580<<6);
			}
            if(auxiliarySamples) {
			const int backingX=geometry.destination.left+(std::max)(1,geometry.content.left-geometry.source.left-1);
			const int backingY=geometry.destination.top+(std::max)(1,geometry.content.top-geometry.source.top-1);
			const int sourceX=geometry.source.left+1,sourceY=geometry.source.top+1;
			pixels[backingY*width+backingX]={200,180,160,255};
			luma[backingY*width+backingX]=uint16_t(800<<6);
			pixels[sourceY*width+sourceX]={0,0,0,255};
			luma[sourceY*width+sourceX]=uint16_t(64<<6);
            // A rejected-key sample in source/destination overlap proves that
            // cleanup continues through the moved panel without a black seam.
            const int overlapY=(std::max)(geometry.content.top,geometry.destination.top)+1;
            if(overlapY<geometry.destination.bottom) {
                const int mappedY=overlapY+geometry.source.top-geometry.destination.top;
                pixels[mappedY*width+geometry.content.left+1]={0,0,0,255};
                luma[mappedY*width+geometry.content.left+1]=uint16_t(64<<6);
            }
            // Bright scenery in the added padding must neither move as text nor
            // be erased from its original location by a keyed mode.
            const int borderX=geometry.source.left+1,borderY=geometry.source.bottom-2;
            pixels[borderY*width+borderX]={245,245,245,255};
            luma[borderY*width+borderX]=uint16_t(580<<6);
            }
            if(fringeSceneryInk) {
                // Same white stroke outside the opaque card in the picture,
                // and in the black bar. Only the latter is subtitle material.
                const int x=geometry.content.left+1;
                for(const int y:{geometry.content.top+3,geometry.pictureBottom+1}) {
                    pixels[y*width+x]={245,245,245,255};luma[y*width+x]=uint16_t(580<<6);
                }
            }
            if(brightBottomFringe) {
                const int y=brightFringeY>=0?brightFringeY:geometry.content.bottom-1;
                for(int x=geometry.content.left;x<geometry.content.right;++x) {
                    pixels[y*width+x]={245,245,245,255};luma[y*width+x]=uint16_t(580<<6);
                }
            }
            if(originalCornerInk) {
                // A fragment of the old first glyph overlaps the rounded
                // destination corner. Its mapped replacement sample is black.
                const int corner= (geometry.destination.top+1)*width+geometry.destination.left;
                pixels[corner]={245,245,245,255};luma[corner]=uint16_t(580<<6);
            }
            if(recordedPixels) { Assert::AreEqual(size_t(width*height),recordedPixels->size());pixels=*recordedPixels; }
			pl_tex textures[2]{};
			pl_tex_params input{};input.w=width;input.h=height;input.sampleable=true;
			input.format=hdrP010?pl_find_fmt(gpu,PL_FMT_UNORM,1,16,16,PL_FMT_CAP_SAMPLEABLE):outputFormat;
			Assert::IsNotNull(input.format);input.initial_data=hdrP010?static_cast<const void*>(luma.data()):pixels.data();
			textures[0]=pl_tex_create(gpu,&input);Assert::IsNotNull(textures[0]);
			auto image=MakeRgbFrame(textures[0]);
			if(hdrP010) {
				input.w=width/2;input.h=height/2;
				input.format=pl_find_fmt(gpu,PL_FMT_UNORM,2,16,16,PL_FMT_CAP_SAMPLEABLE);
				Assert::IsNotNull(input.format);input.initial_data=chroma.data();
				textures[1]=pl_tex_create(gpu,&input);Assert::IsNotNull(textures[1]);
				image.num_planes=2;image.planes[0].components=1;image.planes[0].component_mapping[0]=0;
				image.planes[1].texture=textures[1];image.planes[1].components=2;
				image.planes[1].component_mapping[0]=1;image.planes[1].component_mapping[1]=2;
				image.repr.sys=PL_COLOR_SYSTEM_BT_2020_NC;image.repr.levels=PL_COLOR_LEVELS_LIMITED;
				image.repr.alpha=PL_ALPHA_NONE;image.repr.bits.sample_depth=16;
				image.repr.bits.color_depth=10;image.repr.bits.bit_shift=6;
				image.color.primaries=PL_COLOR_PRIM_BT_2020;image.color.transfer=PL_COLOR_TRC_PQ;
				image.color.hdr.min_luma=0.005f;image.color.hdr.max_luma=1000.0f;
			}
            if(transfer!=PL_COLOR_TRC_UNKNOWN) image.color.transfer=transfer;
            if(sourceCrop)image.crop=*sourceCrop;
			pl_tex_params output{};output.w=width;output.h=height;output.format=outputFormat;
			output.renderable=true;output.host_readable=true;
			pl_tex targetTexture=pl_tex_create(gpu,&output);Assert::IsNotNull(targetTexture);
			auto target=MakeRgbFrame(targetTexture);target.color=image.color;
			auto params=pl_render_fast_params;params.dither_params=nullptr;params.peak_detect_params=nullptr;
            pl_tex compositionTexture=nullptr;
            if(sourceCrop && hook) {
                pl_frame composed{};
                Assert::IsTrue(ComposeSubtitleBeforeCrop(m_renderer,gpu,image,hook,&compositionTexture,composed));
                image=composed;
            }
            const pl_hook* hooks[2]={sourceCrop ? additionalHook : hook,additionalHook};
            if(hooks[0]) {params.hooks=hooks;params.num_hooks=sourceCrop?1:(additionalHook?2:1);}
            const auto rendered=NlsHookRecovery::Render(m_renderer,gpu,image,target,target,params,additionalHook,[](){});
            Assert::IsTrue(rendered.rendered);pl_gpu_finish(gpu);
			std::vector<RgbaPixel> result(width*height);
			pl_tex_transfer_params download{};download.tex=targetTexture;download.ptr=result.data();
			Assert::IsTrue(pl_tex_download(gpu,&download));
			pl_tex_destroy(gpu,&compositionTexture);
            pl_tex_destroy(gpu,&targetTexture);pl_tex_destroy(gpu,&textures[0]);pl_tex_destroy(gpu,&textures[1]);
			return result;
		}

		void CheckSdrSwapchain(float nits, bool force8bit)
		{
			HWND window = CreateWindowExW(0, L"STATIC", L"VP-0173 test", WS_POPUP,
				0, 0, 64, 64, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
			Assert::IsNotNull(window);
			pl_d3d11_swapchain_params params{};
			params.window = window;
			params.disable_10bit_sdr = force8bit;
			pl_swapchain sw = pl_d3d11_create_swapchain(m_d3d11, &params);
			Assert::IsNotNull(sw);
			pl_color_space output{};
			output.primaries = PL_COLOR_PRIM_BT_709;
			output.transfer = PL_COLOR_TRC_SRGB;
			output.hdr.max_luma = nits;
			const auto hint = LibplaceboRenderParameters::MakeSwapchainColorHint(output);
			pl_swapchain_colorspace_hint(sw, &hint);
			int width = 64, height = 64;
			Assert::IsTrue(pl_swapchain_resize(sw, &width, &height));
			pl_swapchain_frame frame{};
			Assert::IsTrue(pl_swapchain_start_frame(sw, &frame));
			Assert::IsFalse(pl_color_space_is_hdr(&frame.color_space));
			Assert::AreEqual(static_cast<int>(PL_COLOR_TRC_SRGB),
				static_cast<int>(frame.color_space.transfer));
			IDXGISwapChain* dxgi = pl_d3d11_swapchain_unwrap(sw);
			DXGI_SWAP_CHAIN_DESC desc{};
			Assert::IsTrue(SUCCEEDED(dxgi->GetDesc(&desc)));
			if (force8bit)
				Assert::AreEqual(static_cast<int>(DXGI_FORMAT_R8G8B8A8_UNORM),
					static_cast<int>(desc.BufferDesc.Format));
			Logger::WriteMessage(("VP-0173 swapchain nits=" + std::to_string(nits) +
				" force8=" + std::to_string(force8bit) + " DXGI_FORMAT=" +
				std::to_string(desc.BufferDesc.Format) + " transfer=sRGB\n").c_str());
			dxgi->Release();
			Assert::IsTrue(pl_swapchain_submit_frame(sw));
			pl_swapchain_destroy(&sw);
			DestroyWindow(window);
		}

		static pl_frame MakeRgbFrame(pl_tex texture)
		{
			pl_frame frame{};
			frame.num_planes = 1;
			frame.planes[0].texture = texture;
			frame.planes[0].components = 4;
			frame.planes[0].component_mapping[0] = 0;
			frame.planes[0].component_mapping[1] = 1;
			frame.planes[0].component_mapping[2] = 2;
			frame.planes[0].component_mapping[3] = 3;
			frame.repr.sys = PL_COLOR_SYSTEM_RGB;
			frame.repr.levels = PL_COLOR_LEVELS_FULL;
			frame.color.primaries = PL_COLOR_PRIM_BT_709;
			frame.color.transfer = PL_COLOR_TRC_SRGB;
			return frame;
		}

	private:
		pl_log m_log = nullptr;
		pl_d3d11 m_d3d11 = nullptr;
		pl_renderer m_renderer = nullptr;
	};
}


namespace VideoProcessorTest
{
	TEST_CLASS(LibplaceboLutParserTests)
	{
	public:

        TEST_METHOD(HdrTargetGammaMigratesRenderingAliasWithoutDisplayDependency)
        {
            using namespace LibplaceboCalibrationLut;
            Assert::AreEqual(std::string("2.4"), ResolveHdrTargetGamma("2.4", "2.2"));
            Assert::AreEqual(std::string("2.4"), ResolveHdrTargetGamma("", "2.4"));
            for (const std::string legacy : { "", "display", "auto" })
                Assert::AreEqual(std::string("2.2"), ResolveHdrTargetGamma("", legacy));
            for (const std::string explicitGamma : { "bt1886", "srgb", "1.8", "2.0",
                "2.2", "2.4", "2.6", "2.8" })
                Assert::AreEqual(explicitGamma, ResolveHdrTargetGamma(explicitGamma, "display"));
        }

        TEST_METHOD(RejectedReloadRetainsLutOnlyWithSameInputContract)
        {
            using namespace LibplaceboCalibrationLut;
            Contract loaded{ "luts/screen.cube", "BT709", "2.4", "base" };
            Contract next = loaded;
            Assert::IsTrue(ResolveReloadFailure(loaded == next, true) == ReloadFailureAction::RETAIN_LAST_KNOWN_GOOD);
            next.inputTransfer = "2.2";
            Assert::IsTrue(ResolveReloadFailure(loaded == next, true) == ReloadFailureAction::DETACH);
            next = loaded; next.gamut = "P3-D65";
            Assert::IsTrue(ResolveReloadFailure(loaded == next, true) == ReloadFailureAction::DETACH);
            loaded.inputTransfer = HdrInputContractKey("2.2");
            next = loaded; next.inputTransfer = HdrInputContractKey("2.4");
            Assert::IsTrue(ResolveReloadFailure(loaded == next, true) == ReloadFailureAction::DETACH);
        }

        TEST_METHOD(NoLutGamma22FullLimitedRampsMatchReferenceAt8And10Bits)
        {
            TargetLutGpuFixture fixture;
            Assert::IsTrue(fixture.Create());
            for (int bits : {8,10}) for (bool limited : {false,true})
            {
                const auto pass = fixture.RenderGammaRamp(bits,limited,PL_COLOR_TRC_GAMMA22,PL_COLOR_TRC_GAMMA22);
                const auto converted = fixture.RenderGammaRamp(bits,limited,PL_COLOR_TRC_GAMMA24,PL_COLOR_TRC_GAMMA22);
                const int low=limited ? (16 << (bits-8)) : 0;
                const int high=limited ? (235 << (bits-8)) : ((1 << bits)-1);
                for (int i=0;i<33;++i)
                {
                    const double x=i/32.0;
                    const int expectedPass=static_cast<int>(std::lround(low+(high-low)*x));
                    const int expectedConverted=static_cast<int>(std::lround(low+(high-low)*std::pow(x,2.4/2.2)));
                    Assert::IsTrue(std::abs(pass[i]-expectedPass)<=1,L"Pass-through gamma/range mismatch");
                    Assert::IsTrue(std::abs(converted[i]-expectedConverted)<=2,L"Reference-preserving gamma/range mismatch");
                }
                Assert::IsTrue(converted[16]<pass[16],L"2.4 reference was silently reinterpreted as 2.2");
            }
        }

        TEST_METHOD(ConvertingTargetLutMatchesNoLutGammaCorrectionExactlyOnce)
        {
            std::ostringstream cube; cube << "LUT_3D_SIZE 33\n";
            for (int b=0;b<33;++b) for(int g=0;g<33;++g) for(int r=0;r<33;++r)
                cube << std::pow(r/32.0,2.4/2.2) << ' ' << std::pow(g/32.0,2.4/2.2) << ' '
                    << std::pow(b/32.0,2.4/2.2) << '\n';
            TemporaryFile file; file.Write(cube.str().c_str());
            auto loaded=Load(nullptr,file.Path());
            Assert::IsTrue(loaded.status==Status::ACTIVE);
            TargetLutGpuFixture fixture; Assert::IsTrue(fixture.Create());
            for(int bits : {8,10}) for(bool limited : {false,true})
            {
                using namespace LibplaceboOutput;
                const auto noLut = ResolveCalibrationTransfers(true, true, false,
                    SdrAdjustGamma::ON, GammaRequest::GAMMA22, GammaRequest::GAMMA22,
                    SdrTransfer::GAMMA24, SdrTransfer::GAMMA22);
                const auto withLut = ResolveCalibrationTransfers(true, true, true,
                    SdrAdjustGamma::ON, GammaRequest::GAMMA22, GammaRequest::GAMMA22,
                    SdrTransfer::GAMMA24, SdrTransfer::GAMMA22);
                const auto direct=fixture.RenderGammaRamp(bits,limited,
                    TestTransfer(noLut.sdr.effectiveSource),TestTransfer(noLut.targetTransfer));
                const auto viaLut=fixture.RenderGammaRamp(bits,limited,
                    TestTransfer(withLut.sdr.effectiveSource),TestTransfer(withLut.targetTransfer),loaded.lut);
                const auto duplicate=fixture.RenderGammaRamp(bits,limited,PL_COLOR_TRC_GAMMA24,PL_COLOR_TRC_GAMMA22,loaded.lut);
                for(int i=0;i<33;++i)
                    Assert::IsTrue(std::abs(direct[i]-viaLut[i])<=2,L"LUT-domain conversion does not match direct correction");
                Assert::IsTrue(duplicate[16]<viaLut[16]-3,L"Probe cannot detect duplicate gamma correction");
            }
            Free(loaded);
        }

        TEST_METHOD(CalibrationWorkflowSdrLutPreservesRampsAcrossControlsAndTransports)
        {
            using namespace LibplaceboOutput;
            TemporaryFile file; file.Write(Valid3dCube);
            auto identity = Load(nullptr, file.Path());
            Assert::IsTrue(identity.status == Status::ACTIVE);
            TargetLutGpuFixture fixture; Assert::IsTrue(fixture.Create());
            for (int bits : { 8, 10 }) for (bool limited : { false, true })
            {
                const int low = limited ? (16 << (bits - 8)) : 0;
                const int high = limited ? (235 << (bits - 8)) : ((1 << bits) - 1);
                for (auto source : { SdrTransfer::BT1886, SdrTransfer::GAMMA22, SdrTransfer::GAMMA24 })
                for (auto requested : { SdrAdjustGamma::ON, SdrAdjustGamma::OFF,
                    SdrAdjustGamma::AUTO, SdrAdjustGamma::PRESERVE_CODES })
                for (auto display : { GammaRequest::GAMMA22, GammaRequest::GAMMA24 })
                {
                    const auto hdr = display == GammaRequest::GAMMA22 ?
                        GammaRequest::GAMMA24 : GammaRequest::GAMMA22;
                    const auto decision = ResolveCalibrationTransfers(true, true, true,
                        requested, display, hdr, source,
                        limited ? SdrTransfer::GAMMA22 : SdrTransfer::SRGB);
                    const auto codes = fixture.RenderGammaRamp(bits, limited,
                        TestTransfer(decision.sdr.effectiveSource),
                        TestTransfer(decision.targetTransfer), identity.lut);
                    for (int i = 0; i < 33; ++i)
                    {
                        const int expected = static_cast<int>(std::lround(low + (high - low) * i / 32.0));
                        Assert::IsTrue(std::abs(codes[i] - expected) <= 1,
                            L"An inactive display/conversion/HDR control changed SDR LUT input codes");
                    }
                }
            }
            Free(identity);
        }

        TEST_METHOD(CalibrationWorkflowSdrGamutMappingKeepsDeclaredSourceTransfer)
        {
            using namespace LibplaceboOutput;
            TemporaryFile file; file.Write(Valid3dCube);
            auto identity = Load(nullptr, file.Path());
            Assert::IsTrue(identity.status == Status::ACTIVE);
            TargetLutGpuFixture fixture; Assert::IsTrue(fixture.Create());
            auto params = pl_render_high_quality_params;
            params.dither_params = nullptr; params.error_diffusion = nullptr;
            params.deband_params = nullptr; params.peak_detect_params = nullptr;
            const RgbaPixel patches[] = { { 164, 94, 52, 255 }, { 77, 151, 109, 255 },
                { 114, 73, 171, 255 } };
            int maximumDifference = 0;
            for (const auto patch : patches)
            {
                RgbaPixel previous{};
                for (auto declared : { SdrTransfer::GAMMA22, SdrTransfer::GAMMA24 })
                {
                    const auto decision = ResolveCalibrationTransfers(true, true, true,
                        SdrAdjustGamma::ON, GammaRequest::GAMMA28, GammaRequest::GAMMA18,
                        declared, SdrTransfer::SRGB);
                    const auto render = [&](pl_color_primaries sourcePrimaries,
                        pl_color_transfer sourceTransfer, pl_color_transfer targetTransfer)
                    {
                        return fixture.Render(identity.lut, params, PL_COLOR_LEVELS_FULL,
                            PL_LUT_NORMALIZED, patch, sourceTransfer, targetTransfer,
                            sourcePrimaries, PL_COLOR_PRIM_BT_709, 0.0f, 0.0f, true);
                    };
                    const auto matched = render(PL_COLOR_PRIM_BT_709,
                        TestTransfer(decision.sdr.effectiveSource), TestTransfer(decision.targetTransfer));
                    Assert::IsTrue(PixelDistance(matched, patch) <= 3,
                        L"Matched-gamut LUT input must retain normalized SDR tone codes");
                    const auto mapped = render(PL_COLOR_PRIM_BT_2020,
                        TestTransfer(decision.sdr.effectiveSource), TestTransfer(decision.targetTransfer));
                    // Independent reference declares the expected transfer on both sides
                    // of genuine linear-light BT.2020-to-BT.709 gamut mapping.
                    const auto reference = render(PL_COLOR_PRIM_BT_2020,
                        TestTransfer(declared), TestTransfer(declared));
                    Assert::IsTrue(PixelDistance(mapped, reference) <= 3,
                        L"SDR gamut mapping decoded or encoded with a display/HDR gamma instead of the source declaration");
                    if (declared == SdrTransfer::GAMMA24)
                        maximumDifference = std::max(maximumDifference, PixelDistance(previous, mapped));
                    previous = mapped;
                }
            }
            Assert::IsTrue(maximumDifference >= 3,
                L"Wide-gamut probe did not detect the declared source gamma in linear-light processing");
            Free(identity);
        }

        TEST_METHOD(CalibrationWorkflowFiniteBlackBt1886ChangesWideGamutSdrOnly)
        {
            using namespace LibplaceboOutput;
            pl_color_space sourceReference{}, targetReference{};
            LibplaceboRenderParameters::ApplySourceLuminance(true, sourceReference);
            LibplaceboRenderParameters::ApplyTargetLuminance(true, 100.0f, 0.0f, targetReference);
            Assert::IsTrue(sourceReference.hdr.min_luma > 0.0f,
                L"BT.1886 versus pure 2.4 needs a finite-black reference to be a meaningful probe");
            Assert::AreEqual(sourceReference.hdr.min_luma, targetReference.hdr.min_luma);
            Assert::AreEqual(sourceReference.hdr.max_luma, targetReference.hdr.max_luma);
            TemporaryFile file; file.Write(Valid3dCube);
            auto identity = Load(nullptr, file.Path());
            Assert::IsTrue(identity.status == Status::ACTIVE);
            TargetLutGpuFixture fixture; Assert::IsTrue(fixture.Create());
            auto params = pl_render_high_quality_params;
            params.dither_params = nullptr; params.error_diffusion = nullptr;
            params.deband_params = nullptr; params.peak_detect_params = nullptr;
            const RgbaPixel patches[] = { { 48, 31, 14, 255 }, { 108, 64, 33, 255 },
                { 36, 82, 53, 255 }, { 164, 94, 52, 255 } };
            int maximumDifference = 0;
            for (const auto patch : patches)
            {
                RgbaPixel bt1886{};
                for (auto declared : { SdrTransfer::BT1886, SdrTransfer::GAMMA24 })
                {
                    const auto decision = ResolveCalibrationTransfers(true, true, true,
                        SdrAdjustGamma::ON, GammaRequest::GAMMA22, GammaRequest::GAMMA22,
                        declared, SdrTransfer::SRGB);
                    const auto render = [&](pl_color_primaries primaries,
                        pl_color_transfer source, pl_color_transfer target)
                    {
                        // The same finite-black source/target metadata as the real
                        // renderer is applied by the final true argument.
                        return fixture.Render(identity.lut, params, PL_COLOR_LEVELS_FULL,
                            PL_LUT_NORMALIZED, patch, source, target, primaries,
                            PL_COLOR_PRIM_BT_709, 0.0f, 0.0f, true);
                    };
                    const auto matched = render(PL_COLOR_PRIM_BT_709,
                        TestTransfer(decision.sdr.effectiveSource), TestTransfer(decision.targetTransfer));
                    Assert::IsTrue(PixelDistance(matched, patch) <= 3,
                        L"Finite-black matched-gamut SDR changed tone codes before the identity LUT");
                    const auto mapped = render(PL_COLOR_PRIM_BT_2020,
                        TestTransfer(decision.sdr.effectiveSource), TestTransfer(decision.targetTransfer));
                    const auto reference = render(PL_COLOR_PRIM_BT_2020,
                        TestTransfer(declared), TestTransfer(declared));
                    Assert::IsTrue(PixelDistance(mapped, reference) <= 3,
                        L"The policy substituted another transfer during BT.1886 gamut processing");
                    if (declared == SdrTransfer::BT1886) bt1886 = mapped;
                    else maximumDifference = std::max(maximumDifference, PixelDistance(bt1886, mapped));
                }
            }
            Logger::WriteMessage(("BT.1886 vs 2.4 finite-black SDR gamut maximum RGB code distance=" +
                std::to_string(maximumDifference) + "\n").c_str());
            Assert::IsTrue(maximumDifference >= 3,
                L"Finite-black BT.1886 and pure 2.4 did not produce distinct wide-gamut SDR processing");
            Free(identity);
        }

        TEST_METHOD(CalibrationWorkflowColoredHdrMapsSelectedGamutBeforeLut)
        {
            using namespace LibplaceboOutput;
            TemporaryFile identityFile; identityFile.Write(Valid3dCube);
            auto identity = Load(nullptr, identityFile.Path());
            TemporaryFile probeFile; probeFile.Write(GammaCoordinateProbeCube);
            auto probe = Load(nullptr, probeFile.Path());
            Assert::IsTrue(identity.status == Status::ACTIVE && probe.status == Status::ACTIVE);
            TargetLutGpuFixture fixture; Assert::IsTrue(fixture.Create());
            auto params = pl_render_high_quality_params;
            params.dither_params = nullptr; params.error_diffusion = nullptr;
            params.deband_params = nullptr; params.peak_detect_params = nullptr;
            const RgbaPixel patches[] = { { 140, 90, 60, 255 }, { 75, 130, 95, 255 },
                { 95, 70, 140, 255 } };
            for (auto hdrSource : { PL_COLOR_TRC_PQ, PL_COLOR_TRC_HLG })
            {
                int maximumGamutDifference = 0;
                for (const auto patch : patches)
                {
                    RgbaPixel rec709{};
                    for (auto targetPrimaries : { PL_COLOR_PRIM_BT_709, PL_COLOR_PRIM_DISPLAY_P3 })
                    {
                        const auto selection = LibplaceboCalibrationLut::Select(true,
                            targetPrimaries == PL_COLOR_PRIM_BT_709 ?
                                LibplaceboCalibrationLut::Primaries::BT709 :
                                LibplaceboCalibrationLut::Primaries::P3_D65, true, true, true);
                        Assert::IsTrue(selection.enabled);
                        Assert::IsTrue(selection.slot == (targetPrimaries == PL_COLOR_PRIM_BT_709 ?
                            LibplaceboCalibrationLut::Slot::BT709 : LibplaceboCalibrationLut::Slot::P3_D65));
                        const auto decision = ResolveCalibrationTransfers(false, true, true,
                            SdrAdjustGamma::ON, GammaRequest::GAMMA28, GammaRequest::GAMMA22,
                            SdrTransfer::OTHER, SdrTransfer::SRGB);
                        const auto render = [&](const pl_custom_lut* lut, pl_color_transfer targetTransfer)
                        {
                            return fixture.Render(lut, params, PL_COLOR_LEVELS_FULL, PL_LUT_NORMALIZED,
                                patch, hdrSource, targetTransfer, PL_COLOR_PRIM_BT_2020,
                                targetPrimaries, 1000.0f, 100.0f);
                        };
                        const auto mapped = render(identity.lut, TestTransfer(decision.targetTransfer));
                        // Explicit target reference with no LUT: the identity must
                        // preserve actual HDR tone/gamut mapping for each gamut.
                        const auto reference = render(nullptr, PL_COLOR_TRC_GAMMA22);
                        Assert::IsTrue(PixelDistance(mapped, reference) <= 3,
                            L"Identity LUT disturbed the selected HDR gamut-mapping target");
                        const auto calibrated = render(probe.lut, TestTransfer(decision.targetTransfer));
                        Assert::IsTrue(std::abs(static_cast<int>(calibrated.g) - mapped.r) <= 2 &&
                            std::abs(static_cast<int>(calibrated.r) - (255 - mapped.r)) <= 2 && calibrated.b <= 2,
                            L"Calibration LUT did not receive the gamut-mapped HDR code coordinates");
                        if (targetPrimaries == PL_COLOR_PRIM_BT_709) rec709 = mapped;
                        else maximumGamutDifference = std::max(maximumGamutDifference,
                            PixelDistance(rec709, mapped));
                    }
                }
                Logger::WriteMessage(("Colored HDR target-gamut maximum RGB code distance=" +
                    std::to_string(maximumGamutDifference) + "\n").c_str());
                Assert::IsTrue(maximumGamutDifference >= 3,
                    L"BT.2020 HDR produced the same coded colors for P3-D65 and Rec.709 targets");
            }
            Free(probe); Free(identity);
        }

        TEST_METHOD(CalibrationWorkflowHdrLutUsesHdrGammaAndDetachmentRestoresDisplay)
        {
            using namespace LibplaceboOutput;
            TemporaryFile file; file.Write(Valid3dCube);
            auto identity = Load(nullptr, file.Path());
            Assert::IsTrue(identity.status == Status::ACTIVE);
            TargetLutGpuFixture fixture; Assert::IsTrue(fixture.Create());
            auto params = pl_render_high_quality_params;
            params.dither_params = nullptr; params.error_diffusion = nullptr;
            params.deband_params = nullptr; params.peak_detect_params = nullptr;
            for (auto hdrSourceTransfer : { PL_COLOR_TRC_PQ, PL_COLOR_TRC_HLG })
            {
                const auto render = [&](bool lutActive, GammaRequest display, GammaRequest hdr)
                {
                    const auto decision = ResolveCalibrationTransfers(false, true, lutActive,
                        SdrAdjustGamma::ON, display, hdr, SdrTransfer::OTHER, SdrTransfer::SRGB);
                    return fixture.Render(lutActive ? identity.lut : nullptr, params,
                        PL_COLOR_LEVELS_FULL, PL_LUT_NORMALIZED, { 96, 96, 96, 255 },
                        hdrSourceTransfer, TestTransfer(decision.targetTransfer),
                        PL_COLOR_PRIM_BT_2020, PL_COLOR_PRIM_BT_709, 1000.0f, 100.0f);
                };
                const auto gamma22 = render(true, GammaRequest::GAMMA22, GammaRequest::GAMMA22);
                const auto changedDisplay = render(true, GammaRequest::GAMMA28, GammaRequest::GAMMA22);
                const auto gamma24 = render(true, GammaRequest::GAMMA22, GammaRequest::GAMMA24);
                Assert::IsTrue(PixelDistance(gamma22, changedDisplay) == 0,
                    L"Physical display gamma changed HDR output while the LUT owned calibration");
                Assert::IsTrue(static_cast<int>(gamma24.r) - gamma22.r >= 3,
                    L"HDR target gamma did not change the tone-mapped encoding");
                Assert::IsTrue(std::abs(std::pow(gamma22.r / 255.0, 2.2) -
                    std::pow(gamma24.r / 255.0, 2.4)) < 0.01,
                    L"HDR target gamma unexpectedly changed the tone-mapped linear result");
                const auto detached = render(false, GammaRequest::GAMMA24, GammaRequest::GAMMA22);
                const auto detachedChangedHdr = render(false, GammaRequest::GAMMA24, GammaRequest::GAMMA28);
                Assert::IsTrue(PixelDistance(detached, gamma24) <= 3,
                    L"LUT detachment did not restore physical display encoding");
                Assert::IsTrue(PixelDistance(detached, detachedChangedHdr) == 0,
                    L"Inactive HDR LUT gamma changed the no-LUT output");
            }
            Free(identity);
        }

		TEST_METHOD(SdrLuminanceGpuReadbackIsInvariantForUnityAndScaling)
		{
			for (int size : { 32, 64, 96 })
			{
				std::vector<RgbaPixel> reference;
				for (float nits : { 75.0f, 203.0f, 400.0f, 500.0f })
				{
					TargetLutGpuFixture fixture;
					Assert::IsTrue(fixture.Create());
					LuminanceCase test;
					test.targetNits = nits; test.blackNits = nits / 1000.0f;
					test.outputSize = size;
					const auto pixels = fixture.RenderCoordinateField(nullptr, 64, 64, &test);
					if (reference.empty()) reference = pixels;
					Assert::AreEqual(0, std::memcmp(reference.data(), pixels.data(), pixels.size() * sizeof(RgbaPixel)));
					if (size == 64)
						for (int y = 0; y < 64; ++y)
							for (int x = 0; x < 64; ++x)
							{
								const RgbaPixel expected = { static_cast<uint8_t>(x * 255 / 63),
									static_cast<uint8_t>(y * 255 / 63),
									static_cast<uint8_t>((x + y) * 255 / 126), 255 };
								Assert::AreEqual(0, std::memcmp(&expected, &pixels[y * 64 + x], sizeof(expected)),
									L"Unity SDR did not preserve input code values");
							}
					pl_hdr_metadata metadata{};
					Assert::IsFalse(fixture.DetectedHdrMetadata(metadata), L"SDR triggered HDR peak detection");
				}
			}
		}

		TEST_METHOD(SdrLuminanceGpuRetainsOptionalDebandingWithoutToneMapping)
		{
			std::vector<RgbaPixel> reference;
			for (float nits : { 75.0f, 203.0f, 400.0f, 500.0f })
			{
				LuminanceCase test; test.targetNits = nits;
				test.blackNits = nits / 1000.0f; test.deband = true;
				TargetLutGpuFixture fixture; Assert::IsTrue(fixture.Create());
				const auto pixels = fixture.RenderCoordinateField(nullptr, 64, 64, &test);
				if (reference.empty()) reference = pixels;
				Assert::AreEqual(0, std::memcmp(reference.data(), pixels.data(), pixels.size() * sizeof(RgbaPixel)),
					L"HDR destination changed SDR debanding output");
				pl_hdr_metadata metadata{};
				Assert::IsFalse(fixture.DetectedHdrMetadata(metadata));
			}
			LuminanceCase plain;
			TargetLutGpuFixture fixture; Assert::IsTrue(fixture.Create());
			const auto unprocessed = fixture.RenderCoordinateField(nullptr, 64, 64, &plain);
			Assert::IsTrue(std::memcmp(reference.data(), unprocessed.data(), reference.size() * sizeof(RgbaPixel)) != 0,
				L"SDR must still apply explicitly enabled debanding");
		}

		TEST_METHOD(SdrLuminanceGpuGamutMismatchMeasuresLegacyDifference)
		{
			for (auto primaries : { PL_COLOR_PRIM_BT_2020, PL_COLOR_PRIM_DISPLAY_P3 })
			{
				std::vector<RgbaPixel> reference;
				for (float nits : { 75.0f, 203.0f, 400.0f })
				{
					LuminanceCase test;
					test.primaries = primaries; test.targetNits = nits;
					test.blackNits = nits / 1000.0f;
					TargetLutGpuFixture before, after;
					Assert::IsTrue(before.Create()); Assert::IsTrue(after.Create());
					test.legacy = true;
					const auto oldPixels = before.RenderCoordinateField(nullptr, 64, 64, &test);
					test.legacy = false;
					const auto pixels = after.RenderCoordinateField(nullptr, 64, 64, &test);
					if (reference.empty()) reference = pixels;
					Assert::AreEqual(0, std::memcmp(reference.data(), pixels.data(), pixels.size() * sizeof(RgbaPixel)));
					size_t changed = 0; int maximumDelta = 0;
					const auto* a = reinterpret_cast<const uint8_t*>(oldPixels.data());
					const auto* b = reinterpret_cast<const uint8_t*>(pixels.data());
					for (size_t i = 0; i < pixels.size() * sizeof(RgbaPixel); ++i)
					{
						if (a[i] != b[i]) ++changed;
						maximumDelta = std::max(maximumDelta, std::abs(int(a[i]) - int(b[i])));
					}
					Logger::WriteMessage(("VP-0173 gamut=" + std::to_string(primaries) +
						" nits=" + std::to_string(nits) + " changed_bytes=" + std::to_string(changed) +
						" max_code_delta=" + std::to_string(maximumDelta) + "\n").c_str());
					if (nits == 203.0f) Assert::AreEqual(size_t(0), changed);
				}
			}
		}

		TEST_METHOD(HdrLuminanceGpuReadbackPreservesToneMappingAbove203Nits)
		{
			for (float nits : { 400.0f, 500.0f })
			{
				LuminanceCase test; test.hdr = true; test.targetNits = nits;
				test.blackNits = nits / 1000.0f; test.primaries = PL_COLOR_PRIM_BT_2020;
				TargetLutGpuFixture before, after;
				Assert::IsTrue(before.Create()); Assert::IsTrue(after.Create());
				test.legacy = true;
				const auto original = before.RenderCoordinateField(nullptr, 64, 64, &test);
				test.legacy = false;
				const auto pixels = after.RenderCoordinateField(nullptr, 64, 64, &test);
				Assert::AreEqual(0, std::memcmp(original.data(), pixels.data(), pixels.size() * sizeof(RgbaPixel)));
			}
		}

		TEST_METHOD(SdrLuminanceD3D11HintKeepsSdrAndForced8BitTransport)
		{
			TargetLutGpuFixture fixture; Assert::IsTrue(fixture.Create());
			for (bool force8bit : { false, true })
				for (float nits : { 40.0f, 75.0f, 203.0f, 203.01f, 400.0f, 500.0f })
					fixture.CheckSdrSwapchain(nits, force8bit);
		}

		TEST_METHOD(EmptyPathLeavesLutDisabled)
		{
			const LoadResult result = Load(nullptr, "");
			Assert::AreEqual(static_cast<int>(Status::DISABLED), static_cast<int>(result.status));
			Assert::AreEqual(static_cast<int>(Rejection::NONE), static_cast<int>(result.rejection));
			Assert::IsNull(result.lut);
		}

		TEST_METHOD(ThreeDimensionalCubeLoadsForTargetLut)
		{
			TemporaryFile file;
			file.Write(Valid3dCube);

			LoadResult result = Load(nullptr, file.Path());
			Assert::AreEqual(static_cast<int>(Status::ACTIVE), static_cast<int>(result.status));
			Assert::AreEqual(static_cast<int>(Rejection::NONE), static_cast<int>(result.rejection));
			Assert::IsNotNull(result.lut);
			Assert::AreEqual(2, result.lut->size[0]);
			Assert::AreEqual(2, result.lut->size[1]);
			Assert::AreEqual(2, result.lut->size[2]);
			Assert::IsTrue(result.fileBytes > 0);
			Free(result);
			Assert::IsNull(result.lut);
		}

		TEST_METHOD(OneDimensionalCubeIsExplicitlyRejected)
		{
			TemporaryFile file;
			file.Write(Valid1dCube);

			LoadResult result = Load(nullptr, file.Path());
			Assert::AreEqual(static_cast<int>(Status::REJECTED), static_cast<int>(result.status));
			Assert::AreEqual(static_cast<int>(Rejection::ONE_DIMENSIONAL), static_cast<int>(result.rejection));
			Assert::IsNull(result.lut);
			Assert::AreEqual("1D not supported", ShortReason(result.rejection));
		}

		TEST_METHOD(MixedOneAndThreeDimensionalCubeIsExplicitlyRejected)
		{
			TemporaryFile file;
			file.Write(
				"LUT_1D_SIZE 2\n"
				"LUT_3D_SIZE 2\n"
				"0.0 0.0 0.0\n"
				"1.0 0.0 0.0\n"
				"0.0 1.0 0.0\n"
				"1.0 1.0 0.0\n"
				"0.0 0.0 1.0\n"
				"1.0 0.0 1.0\n"
				"0.0 1.0 1.0\n"
				"1.0 1.0 1.0\n");

			const LoadResult result = Load(nullptr, file.Path());
			Assert::AreEqual(
				static_cast<int>(Status::REJECTED),
				static_cast<int>(result.status));
			Assert::AreEqual(
				static_cast<int>(Rejection::ONE_DIMENSIONAL),
				static_cast<int>(result.rejection));
			Assert::IsNull(result.lut);
		}

		TEST_METHOD(MalformedCubeIsRejectedWithoutLut)
		{
			TemporaryFile file;
			file.Write("LUT_3D_SIZE 2\n0.0 0.0 0.0\n");

			LoadResult result = Load(nullptr, file.Path());
			Assert::AreEqual(static_cast<int>(Status::REJECTED), static_cast<int>(result.status));
			Assert::AreEqual(static_cast<int>(Rejection::INVALID_CUBE), static_cast<int>(result.rejection));
			Assert::IsNull(result.lut);
		}

		TEST_METHOD(NonDefaultCubeDomainIsRejectedInsteadOfMisinterpreted)
		{
			TemporaryFile file;
			const std::string contents = std::string(
				"DOMAIN_MIN 0.1 0.0 0.0\n"
				"DOMAIN_MAX 1.0 1.0 1.0\n") + Valid3dCube;
			file.Write(contents.c_str());

			const LoadResult result = Load(nullptr, file.Path());
			Assert::AreEqual(static_cast<int>(Status::REJECTED),
				static_cast<int>(result.status));
			Assert::AreEqual(static_cast<int>(Rejection::UNSUPPORTED_DOMAIN),
				static_cast<int>(result.rejection));
			Assert::AreEqual("non-default domain", ShortReason(result.rejection));
			Assert::IsNull(result.lut);
		}

		TEST_METHOD(DefaultCubeDomainRemainsSupported)
		{
			TemporaryFile file;
			const std::string contents = std::string(
				"DOMAIN_MIN 0.0 0.0 0.0\n"
				"DOMAIN_MAX 1.0 1.0 1.0\n") + Valid3dCube;
			file.Write(contents.c_str());

			LoadResult result = Load(nullptr, file.Path());
			Assert::AreEqual(static_cast<int>(Status::ACTIVE),
				static_cast<int>(result.status));
			Free(result);
		}

		TEST_METHOD(EmptyFileIsRejectedWithoutLut)
		{
			TemporaryFile file;

			LoadResult result = Load(nullptr, file.Path());
			Assert::AreEqual(static_cast<int>(Status::REJECTED), static_cast<int>(result.status));
			Assert::AreEqual(static_cast<int>(Rejection::EMPTY), static_cast<int>(result.rejection));
			Assert::IsNull(result.lut);
		}

		TEST_METHOD(UnreadableFileIsRejectedWithoutLut)
		{
			const LoadResult result = Load(nullptr, "Z:\\VideoProcessor\\missing-vp012-lut.cube");
			Assert::AreEqual(static_cast<int>(Status::REJECTED), static_cast<int>(result.status));
			Assert::AreEqual(static_cast<int>(Rejection::UNREADABLE), static_cast<int>(result.rejection));
			Assert::IsNull(result.lut);
		}

		TEST_METHOD(PartialSamePathReplacementRetainsThenRetriesFinalCube)
		{
			TemporaryFile file;
			file.Write(Valid3dCube);
			LoadResult initial = Load(nullptr, file.Path());
			Assert::AreEqual(static_cast<int>(Status::ACTIVE),
				static_cast<int>(initial.status));
			const uint64_t initialSignature = initial.lut->signature;

			file.Write("invalid replacement");
			const LoadResult partial = Load(nullptr, file.Path());
			Assert::AreEqual(static_cast<int>(Status::REJECTED),
				static_cast<int>(partial.status));
			Assert::IsTrue(partial.fileVersion.available);
			Assert::AreEqual(
				static_cast<int>(LibplaceboCalibrationLut::ReloadFailureAction::RETAIN_LAST_KNOWN_GOOD),
				static_cast<int>(LibplaceboCalibrationLut::ResolveReloadFailure(true, true)));
			Assert::AreEqual(initialSignature, initial.lut->signature,
				L"The last-known-good Cube must remain owned during a failed reload");

			file.Write(Green3dCube);
			const FileVersion completed = ProbeFileVersion(file.Path());
			Assert::IsFalse(SameFileVersion(partial.fileVersion, completed),
				L"Completion after a partial write must schedule another reload");
			LoadResult replacement = Load(nullptr, file.Path());
			Assert::AreEqual(static_cast<int>(Status::ACTIVE),
				static_cast<int>(replacement.status));
			Assert::AreNotEqual(initialSignature, replacement.lut->signature,
				L"The completed same-path Cube was not validated and swapped");
			Free(replacement);
			Free(initial);
		}

		TEST_METHOD(FileLargerThanLimitIsRejectedBeforeParsing)
		{
			TemporaryFile file;
			file.ResizeTo(MAX_FILE_BYTES + 1);

			const LoadResult result = Load(nullptr, file.Path());
			Assert::AreEqual(static_cast<int>(Status::REJECTED), static_cast<int>(result.status));
			Assert::AreEqual(static_cast<int>(Rejection::TOO_LARGE), static_cast<int>(result.rejection));
			Assert::AreEqual(static_cast<size_t>(0), result.fileBytes);
			Assert::IsNull(result.lut);
		}

		TEST_METHOD(ConstrainedLutPathUsesTheOpenedFileHandleForContainment)
		{
			TemporaryDirectory configurationDirectory;
			TemporaryDirectory outsideDirectory;
			const std::string inside =
				configurationDirectory.Write("inside.cube", Valid3dCube);
			const std::string outside =
				outsideDirectory.Write("outside.cube", Valid3dCube);

			LoadResult accepted = Load(
				nullptr, inside, configurationDirectory.Path());
			Assert::AreEqual(
				static_cast<int>(Status::ACTIVE), static_cast<int>(accepted.status));
			Free(accepted);

			const LoadResult rejected = Load(
				nullptr, outside, configurationDirectory.Path());
			Assert::AreEqual(
				static_cast<int>(Status::REJECTED), static_cast<int>(rejected.status));
			Assert::AreEqual(
				static_cast<int>(Rejection::PATH_OUTSIDE_BASE),
				static_cast<int>(rejected.rejection));
			Assert::AreEqual("bad path", ShortReason(rejected.rejection));
			Assert::IsNull(rejected.lut);
		}

		TEST_METHOD(EveryRejectionHasAShortOsdSafeReason)
		{
			for (const Rejection rejection : {
				Rejection::UNREADABLE, Rejection::EMPTY, Rejection::TOO_LARGE,
				Rejection::READ_FAILED, Rejection::PATH_OUTSIDE_BASE,
				Rejection::INVALID_CUBE,
				Rejection::ONE_DIMENSIONAL, Rejection::UNSUPPORTED_DOMAIN,
				Rejection::UNSAFE_DIMENSIONS })
			{
				const std::string reason = ShortReason(rejection);
				Assert::IsFalse(reason.empty());
				Assert::IsTrue(reason.size() <= 20, L"LUT rejection reason is too long for the OSD");
			}
		}

		TEST_METHOD(ConfiguredCalibrationCubeExamplesLoadWhenProvided)
		{
			char directory[MAX_PATH] = {};
			const DWORD length = GetEnvironmentVariableA(
				"VP_LUT_EXAMPLE_DIR", directory, ARRAYSIZE(directory));
			if (length == 0)
				return;
			Assert::IsTrue(length < ARRAYSIZE(directory));

			std::string root(directory);
			if (!root.empty() && root.back() != '\\' && root.back() != '/')
				root.push_back('\\');
			for (const char* name : {
				"10^3 CENTER PKCHR 1886 20260126_BMD65.cube",
				"BW_BMD65.cube",
				"Unity_BMD65.cube" })
			{
				LoadResult result = Load(nullptr, root + name);
				Assert::AreEqual(
					static_cast<int>(Status::ACTIVE),
					static_cast<int>(result.status),
					std::wstring(name, name + strlen(name)).c_str());
				Assert::IsNotNull(result.lut);
				Assert::AreEqual(65, result.lut->size[0]);
				Assert::AreEqual(65, result.lut->size[1]);
				Assert::AreEqual(65, result.lut->size[2]);
				Free(result);
			}
		}

		TEST_METHOD(TargetLutGpuReadbackProvesIdentityAndNonIdentityPathsDiffer)
		{
			TemporaryFile identityFile;
			identityFile.Write(Valid3dCube);
			LoadResult identityLut = Load(nullptr, identityFile.Path());
			Assert::AreEqual(
				static_cast<int>(Status::ACTIVE),
				static_cast<int>(identityLut.status));

			TemporaryFile greenFile;
			greenFile.Write(Green3dCube);
			LoadResult greenLut = Load(nullptr, greenFile.Path());
			Assert::AreEqual(
				static_cast<int>(Status::ACTIVE),
				static_cast<int>(greenLut.status));

			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			const RgbaPixel baseline = fixture.Render(nullptr);
			const RgbaPixel identity = fixture.Render(identityLut.lut);
			const RgbaPixel calibrated = fixture.Render(greenLut.lut);
			Free(identityLut);
			Free(greenLut);

			Assert::IsTrue(baseline.r > 240 && baseline.g < 15 && baseline.b < 15,
				L"The no-LUT target path did not preserve the red input");
			Assert::AreEqual(baseline.r, identity.r);
			Assert::AreEqual(baseline.g, identity.g);
			Assert::AreEqual(baseline.b, identity.b);
			Assert::IsTrue(calibrated.r < 15 && calibrated.g > 240 && calibrated.b < 15,
				L"The target LUT did not produce its expected green output");
		}

		TEST_METHOD(NormalizedCalibrationLutRunsBeforeLimitedRangeEncoding)
		{
			TemporaryFile greenFile;
			greenFile.Write(Green3dCube);
			LoadResult greenLut = Load(nullptr, greenFile.Path());
			Assert::AreEqual(static_cast<int>(Status::ACTIVE),
				static_cast<int>(greenLut.status));

			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(),
				L"Could not create the libplacebo WARP test device");
			const RgbaPixel normalized = fixture.Render(greenLut.lut,
				pl_render_fast_params, PL_COLOR_LEVELS_LIMITED,
				PL_LUT_NORMALIZED);
			const RgbaPixel native = fixture.Render(greenLut.lut,
				pl_render_fast_params, PL_COLOR_LEVELS_LIMITED,
				PL_LUT_NATIVE);
			Free(greenLut);

			Assert::IsTrue(normalized.r >= 14 && normalized.r <= 18 &&
				normalized.g >= 233 && normalized.g <= 237 &&
				normalized.b >= 14 && normalized.b <= 18,
				L"Normalized target LUT was not followed by legal-range encoding");
			Assert::IsTrue(native.r < 4 && native.g > 251 && native.b < 4,
				L"Native target LUT unexpectedly ran before legal-range encoding");
		}

		TEST_METHOD(NormalizedCalibrationLutReceivesTargetGammaCoordinates)
		{
			TemporaryFile probeFile;
			probeFile.Write(GammaCoordinateProbeCube);
			LoadResult probeLut = Load(nullptr, probeFile.Path());
			Assert::AreEqual(static_cast<int>(Status::ACTIVE),
				static_cast<int>(probeLut.status));

			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(),
				L"Could not create the libplacebo WARP test device");
			const RgbaPixel result = fixture.Render(probeLut.lut,
				pl_render_fast_params, PL_COLOR_LEVELS_FULL,
				PL_LUT_NORMALIZED, { 64, 64, 64, 255 },
				PL_COLOR_TRC_LINEAR, PL_COLOR_TRC_GAMMA22);
			Free(probeLut);

			Assert::IsTrue(result.g > result.r && result.g >= 130,
				L"Calibration Cube did not receive Gamma-2.2 encoded target RGB");
		}

		TEST_METHOD(TargetLutGpuReadbackPreservesHighQualityErrorDiffusion)
		{
			TemporaryFile greenFile;
			greenFile.Write(Green3dCube);
			LoadResult greenLut = Load(nullptr, greenFile.Path());
			Assert::AreEqual(
				static_cast<int>(Status::ACTIVE),
				static_cast<int>(greenLut.status));

			pl_render_params compatibleParams = pl_render_high_quality_params;
			compatibleParams.error_diffusion =
				&pl_error_diffusion_floyd_steinberg;
			compatibleParams.dither_params = nullptr;
			RenderStepCapture steps;
			compatibleParams.info_callback = &RenderStepCapture::Callback;
			compatibleParams.info_priv = &steps;
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			const RgbaPixel calibrated = fixture.Render(greenLut.lut, compatibleParams);
			Free(greenLut);

			Assert::IsTrue(calibrated.r < 15 && calibrated.g > 240 && calibrated.b < 15,
				L"Calibration LUT plus final error diffusion did not produce green output");
			Assert::IsTrue(steps.errorDiffusion,
				L"The renderer did not dispatch the requested error-diffusion shader");
			Assert::IsTrue((fixture.Errors().errors &
				PL_RENDER_ERR_ERROR_DIFFUSION) == 0,
				L"libplacebo reported error-diffusion failure");
		}

		TEST_METHOD(PqHdrDynamicToneMappingRunsBeforeCalibrationLut)
		{
			TemporaryFile identityFile;
			identityFile.Write(Valid3dCube);
			LoadResult identity = Load(nullptr, identityFile.Path());
			TemporaryFile greenFile;
			greenFile.Write(Green3dCube);
			LoadResult green = Load(nullptr, greenFile.Path());
			Assert::AreEqual(static_cast<int>(Status::ACTIVE),
				static_cast<int>(identity.status));
			Assert::AreEqual(static_cast<int>(Status::ACTIVE),
				static_cast<int>(green.status));

			pl_render_params params = pl_render_high_quality_params;
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(),
				L"Could not create the libplacebo WARP test device");
			const auto render = [&](const pl_custom_lut* lut, float targetNits)
			{
				return fixture.Render(lut, params, PL_COLOR_LEVELS_FULL,
					PL_LUT_NORMALIZED, { 160, 160, 160, 255 },
					PL_COLOR_TRC_PQ, PL_COLOR_TRC_GAMMA22,
					PL_COLOR_PRIM_BT_2020, PL_COLOR_PRIM_BT_709,
					1000.0f, targetNits);
			};
			const RgbaPixel baseline = render(nullptr, 100.0f);
			pl_hdr_metadata detected{};
			const bool detectedAvailable = fixture.DetectedHdrMetadata(detected);
			const RgbaPixel alternateTarget = render(nullptr, 400.0f);
			const RgbaPixel withIdentity = render(identity.lut, 100.0f);
			const RgbaPixel withCalibration = render(green.lut, 100.0f);
			Free(identity);
			Free(green);

			Assert::IsTrue(std::abs(static_cast<int>(baseline.r) -
					static_cast<int>(baseline.g)) <= 2 &&
				std::abs(static_cast<int>(baseline.g) -
					static_cast<int>(baseline.b)) <= 2 &&
				std::abs(static_cast<int>(baseline.r) -
					static_cast<int>(alternateTarget.r)) >= 3,
				L"HDR/PQ result did not respond to the SDR DTM target luminance");
			Assert::IsTrue(detectedAvailable && detected.max_pq_y > 0.0f,
				L"Dynamic HDR peak analysis did not produce CIE-Y metadata");
			Assert::IsTrue((fixture.Errors().errors &
				PL_RENDER_ERR_PEAK_DETECT) == 0,
				L"libplacebo reported a dynamic peak-analysis failure");
			Assert::IsTrue(std::abs(static_cast<int>(baseline.r) -
				static_cast<int>(withIdentity.r)) <= 2 &&
				std::abs(static_cast<int>(baseline.g) -
					static_cast<int>(withIdentity.g)) <= 2 &&
				std::abs(static_cast<int>(baseline.b) -
					static_cast<int>(withIdentity.b)) <= 2,
				L"Identity target LUT replaced or perturbed the HDR-to-SDR DTM result");
			Assert::IsTrue(withCalibration.r < 15 &&
				withCalibration.g > 240 && withCalibration.b < 15,
				L"Distinct calibration LUT was not applied after HDR-to-SDR DTM");
		}


        TEST_METHOD(AnamorphicGpuMatchingContentFillsRasterAndCropsSourceBars)
        {
            TargetLutGpuFixture fixture;
            Assert::IsTrue(fixture.Create());
            AnamorphicGpuCase g;
            g.sourceCrop = {0,27,256,135}; // 256/108 = 64/27, selected from a taller source
            const auto pixels = fixture.RenderCoordinateField(nullptr, 256, 162, nullptr, &g);
            Assert::AreEqual(0.0f, g.submittedCrop.x0, 1e-4f);
            Assert::AreEqual(0.0f, g.submittedCrop.y0, 1e-4f);
            Assert::AreEqual(192.0f, g.submittedCrop.x1, 1e-4f);
            Assert::AreEqual(108.0f, g.submittedCrop.y1, 1e-4f);
            Assert::IsTrue(pixels[0].g > 25); // selected picture reaches top-left
            Assert::IsTrue(pixels.back().g > 170 && pixels.back().r > 240);
            Assert::IsFalse(g.retried);
        }

        TEST_METHOD(AnamorphicGpuNlsCompositionPreservesPhysicalCoordinateMapping)
        {
            TargetLutGpuFixture fixture;
            Assert::IsTrue(fixture.Create());
            for (const char* shader : {"NLS.glsl", "NLSPlus.glsl"})
            {
                const pl_hook* hook = ParseBundledNlsShader(fixture.Gpu(), shader, 0.25, 0.8);
                Assert::IsNotNull(hook);
                for (bool vertical : {false, true})
                {

                    AnamorphicGpuCase reference;
                    reference.lens = 1.0;
                    reference.screenAspect = vertical ? 16.0/9.0 : 2.4;
                    reference.sourceCrop = vertical ? pl_rect2df{8,20,152,92} : pl_rect2df{8,20,152,100};
                    reference.outputWidth = vertical ? 160 : 240;
                    reference.outputHeight = vertical ? 90 : 100;
                    const auto bindGeometry = [&](const AnamorphicGpuCase& g)
                    {
                        const double physical = AnamorphicPresentation::PhysicalTarget(g.configured,
                            g.screenAspect, double(g.outputWidth)/g.outputHeight, g.lens);
                        const auto mapping = EvaluateNlsMapping(true, pl_rect2df_aspect(&g.sourceCrop),
                            physical, 0.1, 0.0, NlsAspectDirection::ANY, 1.4);
                        Assert::AreEqual(static_cast<int>(NlsMappingMode::ACTIVE), static_cast<int>(mapping.mode));
                        BindNlsShader(hook, static_cast<float>(mapping.stretchRatio), mapping.verticalWarp ? 1.0f : 0.0f);
                    };
                    bindGeometry(reference);
                    const auto baseline = fixture.RenderCoordinateField(hook, 160, 120, nullptr, &reference);
                    for (double lens : {0.5, 16.0/15.0, 1.25, 4.0/3.0, 2.0})
                    {
                        auto candidate = reference;
                        candidate.lens = lens;
                        candidate.outputWidth = static_cast<int>(std::lround(reference.outputWidth/lens)) + 1;
                        candidate.outputHeight += 3; // odd raster and fractional target edges
                        bindGeometry(candidate);
                        const auto pixels = fixture.RenderCoordinateField(hook, 160, 120, nullptr, &candidate);
                        // Compare physical normalized positions after optical expansion.
                        for (double v : {0.1, 0.5, 0.9}) for (double u : {0.1, 0.5, 0.9})
                        {
                            auto sample = [u,v](const std::vector<RgbaPixel>& values, const AnamorphicGpuCase& c)
                            {
                                int x = static_cast<int>(c.submittedCrop.x0 + u*(c.submittedCrop.x1-c.submittedCrop.x0));
                                int y = static_cast<int>(c.submittedCrop.y0 + v*(c.submittedCrop.y1-c.submittedCrop.y0));
                                return values[y*c.outputWidth+x];
                            };
                            const auto a = sample(baseline, reference), b = sample(pixels, candidate);
                            Assert::IsTrue(std::abs(int(a.r)-int(b.r)) <= 6 && std::abs(int(a.g)-int(b.g)) <= 6);
                        }
                        Assert::IsFalse(candidate.retried);
                    }
                }
                pl_mpv_user_shader_destroy(&hook);
            }
        }


        TEST_METHOD(AnamorphicGpuFractionalCropAndNativeOverlayCoordinates)
        {
            TargetLutGpuFixture fixture;
            Assert::IsTrue(fixture.Create());
            for (bool cropLocal : {false, true})
            {
                AnamorphicGpuCase g;
                g.outputWidth = 193; g.outputHeight = 109;
                g.sourceCrop = {7.25f,13.5f,151.75f,103.25f};
                g.screenAspect = 2.4; g.lens = 1.25;
                g.overlay = true; g.cropLocalOverlay = cropLocal;
                const auto pixels = fixture.RenderCoordinateField(nullptr, 161, 121, nullptr, &g);
                int minX=193, minY=109, maxX=-1, maxY=-1;
                for (int y=0;y<109;++y) for (int x=0;x<193;++x)
                {
                    const auto pixel=pixels[y*193+x];
                    if (pixel.r>240 && pixel.b>240 && pixel.g<10)
                    { minX=std::min(minX,x); maxX=std::max(maxX,x); minY=std::min(minY,y); maxY=std::max(maxY,y); }
                }
                Assert::IsTrue(maxX>=minX && maxY>=minY);
                // 40x20 square-pixel bitmap must be 32x20 before a 1.25x lens.
                Assert::IsTrue(std::abs((maxX-minX+1)-32)<=1 && std::abs((maxY-minY+1)-20)<=1);
                Assert::IsTrue(minX>=g.submittedCrop.x0 && minY>=g.submittedCrop.y0);
                Assert::IsTrue(maxX<g.submittedCrop.x1 && maxY<g.submittedCrop.y1);
            }
        }

        TEST_METHOD(AnamorphicGpuRuntimeHookFailureRecoversAndCanBeReset)
        {
            TargetLutGpuFixture fixture;
            Assert::IsTrue(fixture.Create());
            int calls = 0;
            pl_hook broken{};
            broken.stages = PL_HOOK_RGB;
            broken.input = PL_HOOK_SIG_NONE;
            broken.signature = 0x0196;
            broken.priv = &calls;
            broken.hook = [](void* context, const pl_hook_params*)
            {
                ++*static_cast<int*>(context);
                pl_hook_res result{};
                result.failed = true;
                return result;
            };
            AnamorphicGpuCase g;
            g.sourceCrop = {0,0,160,90};
            const auto expected = fixture.RenderCoordinateField(nullptr, 160, 90, nullptr, &g);
            const auto pixels = fixture.RenderCoordinateField(&broken, 160, 90, nullptr, &g);
            Assert::IsTrue(g.retried);
            Assert::AreEqual(1, calls);
            Assert::AreEqual(24.0f, g.submittedCrop.x0, 1e-4f);
            Assert::AreEqual(168.0f, g.submittedCrop.x1, 1e-4f);
            Assert::IsTrue(pixels[54*192+3].r == 0 && pixels[54*192+188].g == 0, L"Fallback margins must be black");
            int maxDifference = 0;
            for (size_t i = 0; i < pixels.size(); ++i)
                maxDifference = std::max(maxDifference, std::max(std::abs(int(pixels[i].r)-int(expected[i].r)),
                    std::abs(int(pixels[i].g)-int(expected[i].g))));
            const std::wstring evidence = L"Recovery differs from ordinary fit: max=" + std::to_wstring(maxDifference) +
                L" center=" + std::to_wstring(pixels[54*192+96].r) + L" expected=" + std::to_wstring(expected[54*192+96].r);
            Assert::IsTrue(maxDifference <= 2, evidence.c_str());
            // Same failed signature remains disabled until explicitly reset.
            fixture.RenderCoordinateField(&broken, 160, 90, nullptr, &g);
            Assert::AreEqual(1, calls);
            fixture.ResetFailedHook(broken.signature);
            fixture.RenderCoordinateField(&broken, 160, 90, nullptr, &g);
            Assert::AreEqual(2, calls);
            Assert::IsTrue(g.retried);
        }
		TEST_METHOD(SubtitleCutPasteGpuReadbackUsesCurrentPixelsAndDestinationFirstOverlap)
		{
			TargetLutGpuFixture fixture;Assert::IsTrue(fixture.Create());
			const auto* hook=CreateSubtitleCutPasteHook(fixture.Gpu());Assert::IsNotNull(hook);
			for(const auto transfer:{PL_COLOR_TRC_SRGB,PL_COLOR_TRC_PQ,PL_COLOR_TRC_GAMMA18,
                PL_COLOR_TRC_GAMMA20,PL_COLOR_TRC_GAMMA22,PL_COLOR_TRC_GAMMA24,
                PL_COLOR_TRC_GAMMA26,PL_COLOR_TRC_GAMMA28}) for(int phase=0;phase<3;++phase) {
                const bool hdrP010=transfer==PL_COLOR_TRC_PQ;
                const double gamma=transfer==PL_COLOR_TRC_GAMMA18?1.8:transfer==PL_COLOR_TRC_GAMMA20?2.0:
                    transfer==PL_COLOR_TRC_GAMMA22?2.2:transfer==PL_COLOR_TRC_GAMMA26?2.6:
                    transfer==PL_COLOR_TRC_GAMMA28?2.8:2.4;
				const auto geometry=phase<2 ? ComputeSubtitleCutPaste({20,46,44,61},64,64,8,56,10,15) :
					ComputeSubtitleCutPaste({24,48,36,60},64,64,8,56,2,5);
				Assert::IsTrue(geometry.valid);Assert::IsTrue(BindSubtitleCutPasteHook(hook,geometry));
				Assert::IsFalse(BindSubtitleCutPasteHook(hook,geometry,-1));
				Assert::IsFalse(BindSubtitleCutPasteHook(hook,geometry,6));
				Assert::IsTrue(BindSubtitleCutPasteHook(hook,geometry,0));
				const auto baseline=fixture.RenderSubtitlePattern(nullptr,hdrP010,phase,geometry,transfer);
				const auto moved=fixture.RenderSubtitlePattern(hook,hdrP010,phase,geometry,transfer);
				for(int y=0;y<64;++y) for(int x=0;x<64;++x) {
					const auto sample=MapSubtitleCutPastePixel(geometry,x,y);
					const RgbaPixel expected=sample.clear?RgbaPixel{0,0,0,255}:baseline[sample.y*64+sample.x];
					Assert::IsTrue(PixelDistance(expected,moved[y*64+x])<=6,
						L"GPU cut/paste differs from immutable current-frame pixel mapping");
				}
				std::array<std::vector<RgbaPixel>,4> keyed;
				for(int mode=1;mode<=4;++mode) {
					Assert::IsTrue(BindSubtitleCutPasteHook(hook,geometry,mode,transfer));
					bool found=false;
					for(int parameter=0;parameter<hook->num_parameters;++parameter)
						if(hook->parameters[parameter].name &&
							std::strcmp(hook->parameters[parameter].name,"background_mode")==0) {
							found=true;Assert::AreEqual(float(mode),hook->parameters[parameter].data->f);
						}
					Assert::IsTrue(found,L"the selected backing mode reaches the parsed GPU hook");
					keyed[size_t(mode-1)]=fixture.RenderSubtitlePattern(hook,hdrP010,phase,geometry,transfer);
				}
			const int backingX=geometry.destination.left+(std::max)(1,geometry.content.left-geometry.source.left-1);
				const int backingY=geometry.destination.top+(std::max)(1,geometry.content.top-geometry.source.top-1);
				const int backingPixel=backingY*64+backingX;
				const int roundedCornerPixel=geometry.destination.top*64+geometry.destination.left;
                const int blackCornerPixel=roundedCornerPixel-6;
				const int glyphLeft=geometry.content.left+(geometry.content.right-geometry.content.left)/2-2;
				const int glyphTop=geometry.content.top+(geometry.content.bottom-geometry.content.top)/2-5;
				const int glyphX=glyphLeft+1;
				const int glyphY=glyphTop+1+geometry.destination.top-geometry.source.top;
				const int glyphPixel=glyphY*64+glyphX;
				const int originalGlyphPixel=(glyphTop+1)*64+glyphX;
				const auto& transparent=keyed[0][backingPixel];
				const auto& blended=keyed[1][backingPixel];
				const auto& black=keyed[2][backingPixel];
				const auto& gray=keyed[3][backingPixel];
				auto intensity=[](const RgbaPixel& p){return int(p.r)+int(p.g)+int(p.b);};
				Assert::IsTrue(PixelDistance(baseline[backingPixel],transparent)<=6,
					L"transparent backing preserves the active destination where no keyed glyph exists");
				Assert::IsTrue(PixelDistance(baseline[roundedCornerPixel],keyed[0][roundedCornerPixel])<=6,
					L"transparent mode has no generated backing in the rounded panel corner");
				Assert::IsTrue(intensity(transparent)>intensity(blended)+12 &&
					intensity(blended)>intensity(black)+8 && intensity(gray)>intensity(black)+8 &&
					intensity(gray)<intensity(transparent),
					L"blend, black, and dark-gray modes produce distinct destination backing pixels");
				Assert::IsTrue(PixelDistance(baseline[blackCornerPixel],keyed[2][blackCornerPixel])<
					PixelDistance(baseline[backingPixel],keyed[2][backingPixel]) &&
					PixelDistance(baseline[roundedCornerPixel],keyed[3][roundedCornerPixel])<
					PixelDistance(baseline[backingPixel],keyed[3][backingPixel]) &&
					PixelDistance(baseline[roundedCornerPixel],keyed[1][roundedCornerPixel])<
					PixelDistance(baseline[backingPixel],keyed[1][backingPixel]),
					L"generated black/gray panels curve away at antialiased corners");
                auto decode=[hdrP010,transfer,gamma](double v) {
                    v/=255.0;
                    if(!hdrP010 && transfer!=PL_COLOR_TRC_SRGB) return std::pow(v,gamma);
                    if(!hdrP010) return v<=0.04045?v/12.92:std::pow((v+0.055)/1.055,2.4);
                    const double q=std::pow(v,1.0/78.84375);
                    return std::pow((std::max)(q-0.8359375,0.0)/(18.8515625-18.6875*q),1.0/0.1593017578125)*10000.0/203.0;
                };
                auto encode=[hdrP010,transfer,gamma](double v) {
                    if(!hdrP010 && transfer!=PL_COLOR_TRC_SRGB) return 255.0*std::pow(v,1.0/gamma);
                    if(!hdrP010) return 255.0*(v<=0.0031308?v*12.92:1.055*std::pow(v,1.0/2.4)-0.055);
                    const double q=std::pow(v*203.0/10000.0,0.1593017578125);
                    return 255.0*std::pow((0.8359375+18.8515625*q)/(1.0+18.6875*q),78.84375);
                };
                Assert::IsTrue(std::abs(double(blended.r)-encode(decode(transparent.r)*0.5))<=5.0,
                    L"blend attenuates light by half, not the encoded PQ or SDR signal");
                Assert::IsTrue(std::abs(double(gray.r)-encode(0.08))<=5.0,
                    L"dark gray is eight percent reference white in SDR and PQ");
                const int cleanupX=geometry.content.left+1;
                auto checkFill=[&](int y,bool overlap) {
                    const int left=(std::max)(0,geometry.source.left-2);
                    const int right=(std::min)(63,geometry.source.right+1);
                    const double t=(cleanupX+0.5-geometry.source.left)/(geometry.source.right-geometry.source.left);
                    const double fill=decode(baseline[y*64+left].r)*(1.0-t)+decode(baseline[y*64+right].r)*t;
                    Assert::IsTrue(std::abs(double(keyed[0][y*64+cleanupX].r)-encode(fill))<=7.0,
                        L"transparent cleanup uses current side pixels across source and overlap");
                    Assert::IsTrue(std::abs(double(keyed[1][y*64+cleanupX].r)-encode(fill*(overlap?0.5:1.0)))<=7.0,
                        L"blend panel and source cleanup share the same approximation without a black overlap seam");
                };
                const int overlapY=(std::max)(geometry.content.top,geometry.destination.top)+1;
                if(overlapY<geometry.destination.bottom) checkFill(overlapY,true);
                const int vacatedY=(std::max)(geometry.content.top,geometry.destination.bottom)+1;
                if(vacatedY<geometry.pictureBottom && vacatedY<geometry.content.bottom) checkFill(vacatedY,false);
                const int borderX=geometry.source.left+1,borderY=geometry.source.bottom-2;
                const int borderOriginal=borderY*64+borderX;
                const int borderMoved=(borderY+geometry.destination.top-geometry.source.top)*64+borderX;
                Assert::IsTrue(PixelDistance(baseline[borderMoved],keyed[0][borderMoved])<=6,
                    L"bright picture pixels in padding must not be copied as subtitle glyphs");
                Assert::IsTrue(intensity(keyed[2][borderMoved])<12,
                    L"opaque black padding excludes bright source scenery from its key");
                for(const auto& image:keyed) Assert::IsTrue(PixelDistance(baseline[borderOriginal],image[borderOriginal])<=6,
                    L"keyed cleanup preserves original pixels outside detected content");
				for(const auto& image:keyed) {
					Assert::IsTrue(PixelDistance(baseline[originalGlyphPixel],image[glyphPixel])<=8,
						L"a white subtitle core survives the approximate color key at SDR and realistic PQ levels");
				}
				Assert::IsTrue(BindSubtitleCutPasteHook(hook,{}));
				const auto disabled=fixture.RenderSubtitlePattern(hook,hdrP010,phase,geometry,transfer);
				for(size_t i=0;i<baseline.size();++i) Assert::IsTrue(PixelDistance(baseline[i],disabled[i])<=6);
			}
			pl_mpv_user_shader_destroy(&hook);
		}

		TEST_METHOD(SubtitleCrossingPictureAndBlackBarDoesNotMoveBlackBackground)
		{
			TargetLutGpuFixture fixture;Assert::IsTrue(fixture.Create());
			const auto* hook=CreateSubtitleCutPasteHook(fixture.Gpu());Assert::IsNotNull(hook);
			const auto geometry=ComputeSubtitleCutPaste({20,46,44,61},64,64,8,56,10,15);
			Assert::IsTrue(geometry.valid);
			const auto baseline=fixture.RenderSubtitlePattern(nullptr,false,0,geometry,PL_COLOR_TRC_SRGB,true);
			std::array<std::vector<RgbaPixel>,5> keyed;
            SubtitleGeneratedGrayStyle tintOnly;tintOnly.blurPixels=0;
			for(int mode=1;mode<=5;++mode) {
                auto modeStyle=tintOnly;
                // Solid now honors the supplied color; this case tests black.
                if(mode==3) modeStyle.color={0.0f,0.0f,0.0f};
				Assert::IsTrue(BindSubtitleCutPasteHook(hook,geometry,mode,PL_COLOR_TRC_SRGB,modeStyle));
				keyed[size_t(mode-1)]=fixture.RenderSubtitlePattern(hook,false,0,geometry,PL_COLOR_TRC_SRGB,true);
			}
			const int glyphLeft=geometry.content.left+(geometry.content.right-geometry.content.left)/2-2;
			const int glyphTop=geometry.content.top+(geometry.content.bottom-geometry.content.top)/2-5;
			for(const int sourceY:{53,58}) {
				Assert::IsTrue((sourceY==53 && sourceY<geometry.pictureBottom) ||
					(sourceY==58 && sourceY>=geometry.pictureBottom),
					L"the two tested glyph rows straddle the real picture/bar boundary");
				const int sourceX=glyphLeft+1;
				const int destinationY=sourceY+geometry.destination.top-geometry.source.top;
				const int sourceIndex=sourceY*64+sourceX;
				const int destinationIndex=destinationY*64+sourceX;
				Assert::IsTrue(sourceY>=glyphTop && sourceY<glyphTop+11);
				Assert::IsTrue(PixelDistance(baseline[sourceIndex],keyed[0][destinationIndex])<=8 &&
					PixelDistance(baseline[sourceIndex],keyed[1][destinationIndex])<=8 &&
					PixelDistance(baseline[sourceIndex],keyed[2][destinationIndex])<=8 &&
					PixelDistance(baseline[sourceIndex],keyed[3][destinationIndex])<=8,
					L"subtitle glyph cores survive when a line crosses the active-picture/black-bar boundary");
			}
			const int barGapX=geometry.content.left+2,barGapY=58;
			const int movedBarGapY=barGapY+geometry.destination.top-geometry.source.top;
			const int barGapSource=barGapY*64+barGapX,movedBarGap=movedBarGapY*64+barGapX;
			Assert::IsTrue(PixelDistance(baseline[barGapSource],RgbaPixel{0,0,0,255})<=6);
			Assert::IsTrue(PixelDistance(baseline[movedBarGap],keyed[0][movedBarGap])<=6,
				L"transparent mode shows the active picture through the mapped black-bar background");
			auto decode=[](double v){v/=255.0;return v<=0.04045?v/12.92:std::pow((v+0.055)/1.055,2.4);};
			auto encode=[](double v){return 255.0*(v<=0.0031308?v*12.92:1.055*std::pow(v,1.0/2.4)-0.055);};
			for(const auto channel:{&RgbaPixel::r,&RgbaPixel::g,&RgbaPixel::b})
				Assert::IsTrue(std::abs(double(keyed[1][movedBarGap].*channel)-
					encode(decode(baseline[movedBarGap].*channel)*0.5))<=7.0,
					L"blend mode blends the active picture behind glyphs that originated in the black bar");
			Assert::IsTrue(PixelDistance(keyed[2][movedBarGap],RgbaPixel{0,0,0,255})<=6,
				L"black mode deliberately keeps an opaque black panel behind the moved glyph");
			const uint8_t gray=static_cast<uint8_t>(std::round(encode(0.08)));
			Assert::IsTrue(PixelDistance(keyed[3][movedBarGap],RgbaPixel{gray,gray,gray,255})<=7,
				L"dark-gray mode deliberately keeps its panel behind glyphs from either source region");
			for(const auto channel:{&RgbaPixel::r,&RgbaPixel::g,&RgbaPixel::b})
				Assert::IsTrue(std::abs(double(keyed[4][movedBarGap].*channel)-
					encode(decode(baseline[movedBarGap].*channel)*0.15+(4.0/255.0/12.92)*0.85))<=7.0,
					L"generated-gray backing preserves real destination picture under its gray overlay");
			Assert::IsTrue(PixelDistance(keyed[4][movedBarGap],keyed[4][movedBarGap-64])<45,
				L"the generated panel must not bring the source black-bar color into the picture");
			const int paddingX=geometry.source.left+2;
			const int movedPadding=(movedBarGapY*64+paddingX);
			Assert::IsTrue(PixelDistance(baseline[movedPadding],keyed[0][movedPadding])<=6,
				L"black-bar pixels in added padding do not become an opaque moved rectangle");
			Assert::IsTrue(PixelDistance(baseline[barGapSource],keyed[0][barGapSource])<=6 &&
				PixelDistance(baseline[barGapSource],keyed[1][barGapSource])<=6,
				L"the original black bar remains black after glyph cleanup");
			pl_mpv_user_shader_destroy(&hook);
		}

		TEST_METHOD(GeneratedGrayRestoresOldCardThroughPictureEdgeAndBindsStyle)
		{
			TargetLutGpuFixture fixture;Assert::IsTrue(fixture.Create());
			const auto* hook=CreateSubtitleCutPasteHook(fixture.Gpu());Assert::IsNotNull(hook);
			const auto geometry=ComputeSubtitleCutPaste({20,40,44,57},64,64,8,56,
				SubtitleBoxPadding(12,12,8));
			Assert::IsTrue(geometry.valid);
			SubtitleGeneratedGrayStyle style;
			Assert::IsTrue(ParseSubtitleRgbHex("FF0000",style.color));
			Assert::IsTrue(ParseSubtitleRgbHex("202020",style.borderColor));
			style.opacity=0.65f;style.borderOpacity=0.8f;style.borderWidth=2.0f;
			Assert::IsTrue(BindSubtitleCutPasteHook(hook,geometry,5,PL_COLOR_TRC_SRGB,style));
			auto parameter=[&](const char* name) {
				for(int i=0;i<hook->num_parameters;++i)
					if(hook->parameters[i].name && std::strcmp(hook->parameters[i].name,name)==0)
						return hook->parameters[i].data->f;
				return -1.0f;
			};
			Assert::AreEqual(0.65f,parameter("generated_opacity"));
			Assert::AreEqual(0.16f,parameter("generated_max_luminance"));
            Assert::AreEqual(3.0f,parameter("generated_blur_px"));
			Assert::AreEqual(2.0f,parameter("generated_border_width"));
			Assert::AreEqual(float(geometry.pictureBottom),parameter("generated_clean_bottom"));
			const auto baseline=fixture.RenderSubtitlePattern(nullptr,false,0,geometry,PL_COLOR_TRC_SRGB,true,true);
			const auto moved=fixture.RenderSubtitlePattern(hook,false,0,geometry,PL_COLOR_TRC_SRGB,true,true);
			const int bottomStrip=54*64+25;
			Assert::IsTrue(PixelDistance(baseline[bottomStrip],RgbaPixel{0,0,0,255})<=6);
			Assert::IsTrue(moved[bottomStrip].r>baseline[bottomStrip].r+20,
				L"old black card strip at the picture edge must receive generated picture under the moved panel");
			Assert::IsTrue(moved[bottomStrip].r>moved[bottomStrip].g+15,
				L"configured panel color and opacity must reach the GPU output");
			const int border=(geometry.destination.top+1)*64+
				(geometry.destination.left+geometry.destination.right)/2;
			style.borderWidth=0.0f;
			Assert::IsTrue(BindSubtitleCutPasteHook(hook,geometry,5,PL_COLOR_TRC_SRGB,style));
			const auto withoutBorder=fixture.RenderSubtitlePattern(hook,false,0,geometry,PL_COLOR_TRC_SRGB,true,true);
			Assert::IsTrue(moved[border].r+10<withoutBorder[border].r,
				L"the configured dark outline must be visible on the rounded panel edge");
			style.opacity=0.0f;
			// Probe beyond the additional six-pixel display strip.
			const int nearby=54*64+geometry.source.left-7;
			Assert::IsTrue(BindSubtitleCutPasteHook(hook,geometry,5,PL_COLOR_TRC_SRGB,style));
			const auto transparentPanel=fixture.RenderSubtitlePattern(hook,false,0,geometry,PL_COLOR_TRC_SRGB,true,true);
			Assert::IsTrue(PixelDistance(transparentPanel[nearby],baseline[nearby])<=6,
				L"nearby dark picture outside the card shape must survive source cleanup");
			pl_mpv_user_shader_destroy(&hook);
		}

        TEST_METHOD(OffsetSubtitleBackingReachesBothBarsWithoutMovingGlyphs)
        {
            TargetLutGpuFixture fixture;Assert::IsTrue(fixture.Create());
            const auto* hook=CreateSubtitleCutPasteHook(fixture.Gpu());Assert::IsNotNull(hook);
            // Rebind one hook repeatedly: no renderer/device recreation between styles.
            for(bool top:{false,true}) for(int mode:{3,5,3,5}) {
                const auto g=ComputeSubtitleCutPaste(top?SubtitleBoxRect{20,3,44,18}:SubtitleBoxRect{20,46,44,61},
                    64,64,8,56,SubtitleBoxPadding(4,4,4),6);
                Assert::IsTrue(g.valid);
                SubtitleGeneratedGrayStyle style;ParseSubtitleRgbHex("FF0000",style.color);
                style.opacity=1;style.maxLuminance=1;style.blurPixels=6;
                Assert::IsTrue(BindSubtitleCutPasteHook(hook,g,mode,PL_COLOR_TRC_SRGB,style));
                const auto moved=fixture.RenderSubtitlePattern(hook,false,0,g,PL_COLOR_TRC_SRGB,true);
                const auto original=fixture.RenderSubtitlePattern(nullptr,false,0,g,PL_COLOR_TRC_SRGB,true);
                const int x=(g.destination.left+g.destination.right)/2;
                const int first=top?g.pictureTop:g.destination.bottom;
                const int last=top?g.destination.top:g.pictureBottom;
                Assert::IsTrue(last>first);
                for(int y=first;y<last;++y)
                    Assert::IsTrue(moved[y*64+x].r>100 && moved[y*64+x].r>moved[y*64+x].g+60,
                        L"Offset gap must have the selected backing through the picture/bar edge");
                const int outside=(top?g.pictureTop-1:g.pictureBottom)*64+g.destination.left-1;
                Assert::IsTrue(PixelDistance(original[outside],moved[outside])<=6,
                    L"Background extension must stop at the bar edge");
                const auto roi=ComputeSubtitleGaussianRegion(g,5,6);
                Assert::IsTrue(roi.top<=g.pictureTop || !top);
                Assert::IsTrue(roi.top+roi.height*roi.step>=g.pictureBottom || top);
            }
            pl_mpv_user_shader_destroy(&hook);
        }

        TEST_METHOD(SubtitleTextTintDimsOnlyGlyphsInSdrAndHdr)
        {
            TargetLutGpuFixture fixture;Assert::IsTrue(fixture.Create());
            const auto* hook=CreateSubtitleCutPasteHook(fixture.Gpu());Assert::IsNotNull(hook);
            const auto geometry=ComputeSubtitleCutPaste({20,46,44,61},64,64,8,56,0,0);
            for(const int mode:{3,5}) for(const bool hdr:{false,true}) {
                const auto transfer=hdr?PL_COLOR_TRC_PQ:PL_COLOR_TRC_SRGB;
                SubtitleGeneratedGrayStyle style;
                Assert::IsTrue(BindSubtitleCutPasteHook(hook,geometry,mode,transfer,style));
                const auto original=fixture.RenderSubtitlePattern(hook,hdr,0,geometry,transfer,true);
                Assert::IsTrue(ParseSubtitleRgbHex("808080",style.textColor));
                Assert::IsTrue(BindSubtitleCutPasteHook(hook,geometry,mode,transfer,style));
                const auto dim=fixture.RenderSubtitlePattern(hook,hdr,0,geometry,transfer,true);
                const int glyph=(49+geometry.destination.top-geometry.source.top)*64+31;
                Assert::IsTrue(dim[glyph].r+15<original[glyph].r,L"Gray must dim glyph cores for both transfer functions");
                Assert::IsTrue(PixelDistance(dim[10*64+2],original[10*64+2])<=1,L"Text tint cannot affect unrelated picture");
                Assert::IsTrue(PixelDistance(dim[(geometry.destination.top+1)*64+geometry.destination.left+1],original[(geometry.destination.top+1)*64+geometry.destination.left+1])<=1,L"Text tint cannot recolor the backing");
                Assert::IsTrue(ParseSubtitleRgbHex("FF0000",style.textColor));
                Assert::IsTrue(BindSubtitleCutPasteHook(hook,geometry,mode,transfer,style));
                const auto red=fixture.RenderSubtitlePattern(hook,hdr,0,geometry,transfer,true);
                Assert::IsTrue(red[glyph].r>red[glyph].g+15 && red[glyph].r>red[glyph].b+15,L"Arbitrary text tint must reach glyph pixels");
                style.textColor={1,1,1};
                Assert::IsTrue(BindSubtitleCutPasteHook(hook,geometry,mode,transfer,style));
                const auto restored=fixture.RenderSubtitlePattern(hook,hdr,0,geometry,transfer,true);
                Assert::IsTrue(PixelDistance(restored[glyph],original[glyph])<=1,L"White restores original text without stale tint");
            }
            pl_mpv_user_shader_destroy(&hook);
        }

        TEST_METHOD(SolidSubtitleColorAndBorderReachGpuWithoutOpacityOrBlur)
        {
            TargetLutGpuFixture fixture;Assert::IsTrue(fixture.Create());
            const auto* hook=CreateSubtitleCutPasteHook(fixture.Gpu());Assert::IsNotNull(hook);
            const auto g=ComputeSubtitleCutPaste({20,40,44,57},64,64,8,56,SubtitleBoxPadding(12,12,8));
            SubtitleGeneratedGrayStyle style;
            Assert::IsTrue(ParseSubtitleRgbHex("FF0000",style.color));
            style.opacity=0.0f;style.blurPixels=30;style.maxLuminance=0.01f;
            style.borderColor={0,0,0};style.borderOpacity=1;style.borderWidth=2;
            Assert::IsTrue(BindSubtitleCutPasteHook(hook,g,3,PL_COLOR_TRC_SRGB,style));
            const auto bordered=fixture.RenderSubtitlePattern(hook,false,0,g,PL_COLOR_TRC_SRGB,true,true);
            const int middle=54*64+25;
            Assert::IsTrue(bordered[middle].r>240 && bordered[middle].g<10 && bordered[middle].b<10,
                L"Solid color must remain opaque, unblurred, and independent of reconstruction brightness");
            style.borderWidth=0;
            Assert::IsTrue(BindSubtitleCutPasteHook(hook,g,3,PL_COLOR_TRC_SRGB,style));
            const auto plain=fixture.RenderSubtitlePattern(hook,false,0,g,PL_COLOR_TRC_SRGB,true,true);
            const int edge=(g.destination.top+1)*64+(g.destination.left+g.destination.right)/2;
            Assert::IsTrue(bordered[edge].r+30<plain[edge].r,L"Solid border must reach the shader");
            const int outside=10*64+2;
            const auto original=fixture.RenderSubtitlePattern(nullptr,false,0,g,PL_COLOR_TRC_SRGB,true,true);
            Assert::IsTrue(PixelDistance(original[outside],plain[outside])<=6,L"Solid styling must not alter unrelated picture content");
            pl_mpv_user_shader_destroy(&hook);
        }

        TEST_METHOD(BlackAndGeneratedPanelsExtendSixPixelsWithoutMovingGlyphs)
        {
            TargetLutGpuFixture fixture;Assert::IsTrue(fixture.Create());
            const auto* hook=CreateSubtitleCutPasteHook(fixture.Gpu());Assert::IsNotNull(hook);
            const auto geometry=ComputeSubtitleCutPaste({20,46,44,61},64,64,8,56,0,0);
            const auto baseline=fixture.RenderSubtitlePattern(nullptr,false,0,geometry);
            for(const int mode:{3,5}) {
                Assert::IsTrue(BindSubtitleCutPasteHook(hook,geometry,mode,PL_COLOR_TRC_SRGB));
                const auto moved=fixture.RenderSubtitlePattern(hook,false,0,geometry);
                const int y=geometry.destination.bottom-1;
                for(const int x:{geometry.destination.left-5,geometry.destination.right+4})
                    Assert::IsTrue(PixelDistance(moved[y*64+x],baseline[y*64+x])>12,
                        L"both backgrounds must extend into the six-pixel side strip");
                for(const int x:{geometry.destination.left-7,geometry.destination.right+6})
                    Assert::IsTrue(PixelDistance(moved[y*64+x],baseline[y*64+x])<=6,
                        L"the extra background must stop after six source pixels");
                const int glyphX=31;
                const int originalGlyphY=49;
                const int movedGlyphY=originalGlyphY+geometry.destination.top-geometry.source.top;
                Assert::IsTrue(PixelDistance(moved[movedGlyphY*64+glyphX],baseline[originalGlyphY*64+glyphX])<=8,
                    L"widening the background must not shift subtitle glyphs");
            }
            pl_mpv_user_shader_destroy(&hook);
        }

        TEST_METHOD(KeyedSubtitleCaptureExcludesPictureSceneryOutsideMeasuredCard)
        {
            TargetLutGpuFixture fixture;Assert::IsTrue(fixture.Create());
            const auto* hook=CreateSubtitleCutPasteHook(fixture.Gpu());Assert::IsNotNull(hook);
            // Detection fringe is wider than the actual card. A bright scene
            // stroke lies in that fringe, beside the true subtitle glyphs.
            const SubtitleBoxRect content{16,40,48,60};
            const SubtitleBoxRect card{20,36,44,56};
            const auto geometry=ComputeSubtitleCutPaste(content,64,64,8,56,
                SubtitleBoxPadding(0),0,&card);
            Assert::IsTrue(geometry.valid);
            const int shift=geometry.destination.top-geometry.source.top;
            const int pictureStroke=(content.top+3+shift)*64+content.left+1;
            const int barStroke=(geometry.pictureBottom+1+shift)*64+content.left+1;
            const int trueGlyph=(content.top+(content.bottom-content.top)/2-5+shift)*64+30;
            for(const int mode:{1,2,3,4,5}) {
                Assert::IsTrue(BindSubtitleCutPasteHook(hook,geometry,mode,PL_COLOR_TRC_SRGB));
                const auto moved=fixture.RenderSubtitlePattern(hook,false,0,geometry,
                    PL_COLOR_TRC_SRGB,true,true,100,false,0,&card,false,true);
                Assert::IsTrue(moved[pictureStroke].r<180,
                    L"bright picture detail in the glyph-envelope fringe must not move as text");
                Assert::IsTrue(moved[barStroke].r>220,
                    L"bar glyphs outside the picture-card width must remain intact");
                Assert::IsTrue(moved[trueGlyph].r>220,
                    L"actual picture-side glyphs within the card must remain intact");
            }
            // Rectangle mode deliberately remains a literal copy diagnostic.
            Assert::IsTrue(BindSubtitleCutPasteHook(hook,geometry,0,PL_COLOR_TRC_SRGB));
            const auto raw=fixture.RenderSubtitlePattern(hook,false,0,geometry,
                PL_COLOR_TRC_SRGB,true,true,100,false,0,&card,false,true);
            Assert::IsTrue(raw[pictureStroke].r>220);
            // In the overlap region, generated backing must retain the live
            // scene outside the measured card rather than reflect replacement
            // pixels there. Disable tint and blur to isolate source ownership.
            SubtitleGeneratedGrayStyle untinted;
            untinted.opacity=0.0f;untinted.maxLuminance=1.0f;untinted.blurPixels=0.0f;
            Assert::IsTrue(BindSubtitleCutPasteHook(hook,geometry,5,PL_COLOR_TRC_SRGB,untinted));
            const auto overlap=fixture.RenderSubtitlePattern(hook,false,0,geometry,
                PL_COLOR_TRC_SRGB,true,true,100,false,0,&card,false,true);
            Assert::IsTrue(overlap[(content.top+3)*64+content.left+1].r>200,
                L"generated destination backing must use real overlap scenery outside the measured card");
            // An inset moves text up but backing now extends to the bar.
            // Untinted reconstruction must preserve live scenery in that fill;
            // solid mode intentionally covers it with the configured color.
            const auto inset=ComputeSubtitleCutPaste(content,64,64,8,56,
                SubtitleBoxPadding(0),16,&card);
            Assert::IsTrue(inset.valid);
            const int originalStroke=(content.top+3)*64+content.left+1;
            for(const int mode:{3,5}) {
                Assert::IsTrue(BindSubtitleCutPasteHook(hook,inset,mode,PL_COLOR_TRC_SRGB,untinted));
                const auto preserved=fixture.RenderSubtitlePattern(hook,false,0,inset,
                    PL_COLOR_TRC_SRGB,true,true,100,false,0,&card,false,true);
                if(mode==5)Assert::IsTrue(preserved[originalStroke].r>220,
                    L"reconstructed gap must retain live scenery outside measured cleanup");
                else Assert::IsTrue(preserved[originalStroke].r<40,
                    L"solid backing must cover the inset gap through the bar");
            }
            pl_mpv_user_shader_destroy(&hook);
        }

        TEST_METHOD(BlackRoundedDestinationCannotRevealOriginalGlyphAtCorner)
        {
            TargetLutGpuFixture fixture;Assert::IsTrue(fixture.Create());
            const auto* hook=CreateSubtitleCutPasteHook(fixture.Gpu());Assert::IsNotNull(hook);
            const SubtitleBoxRect glyphs{20,40,44,57};
            const SubtitleBoxRect card{8,28,56,56};
            const auto geometry=ComputeSubtitleCutPaste(glyphs,64,64,8,56,
                SubtitleBoxPadding(0),0,&card);
            Assert::IsTrue(geometry.valid);
            Assert::IsTrue(BindSubtitleCutPasteHook(hook,geometry,3,PL_COLOR_TRC_SRGB));
            const auto baseline=fixture.RenderSubtitlePattern(nullptr,false,0,geometry,
                PL_COLOR_TRC_SRGB,true,true,200,false,0,&card,true);
            const auto moved=fixture.RenderSubtitlePattern(hook,false,0,geometry,
                PL_COLOR_TRC_SRGB,true,true,200,false,0,&card,true);
            const int corner=(geometry.destination.top+1)*64+geometry.destination.left;
            Assert::IsTrue(baseline[corner].r>200);
            Assert::IsTrue(PixelDistance(moved[corner],RgbaPixel{0,0,0,255})<=6,
                L"the antialiased black destination corner must not expose the original glyph");
            pl_mpv_user_shader_destroy(&hook);
        }

        TEST_METHOD(MeasuredCardEdgesRestoreOutsideSamplingRectangleAtEveryDisplayPadding)
        {
            TargetLutGpuFixture fixture;Assert::IsTrue(fixture.Create());
            const auto* hook=CreateSubtitleCutPasteHook(fixture.Gpu());Assert::IsNotNull(hook);
            const SubtitleBoxRect glyphs{20,40,44,57};
            // Original card is wider/taller than glyphs, and deliberately does
            // not depend on ComputeSubtitleCutPaste or display padding.
            const SubtitleBoxRect card{8,28,56,56};
            for(const int margin:{0,3,7}) {
                const auto geometry=ComputeSubtitleCutPaste(glyphs,64,64,8,56,
                    SubtitleBoxPadding(margin),0,&card);
                Assert::IsTrue(geometry.valid);
                Assert::IsTrue(BindSubtitleCutPasteHook(hook,geometry,5,PL_COLOR_TRC_SRGB));
                const auto baseline=fixture.RenderSubtitlePattern(nullptr,false,0,geometry,
                    PL_COLOR_TRC_SRGB,true,true,200,false,0,&card,false,false,nullptr,nullptr,false,-1,false);
                const auto moved=fixture.RenderSubtitlePattern(hook,false,0,geometry,
                    PL_COLOR_TRC_SRGB,true,true,200,false,0,&card,false,false,nullptr,nullptr,false,-1,false);
                // Inspect the independently drawn old card without auxiliary fixture
                // markers. Widened display may now cover these same edge pixels.
                for(int y=card.top+4;y<card.bottom;++y)for(const int x:{9,54}) {
                    const int pixel=y*64+x;
                    Assert::IsTrue(PixelDistance(baseline[pixel],RgbaPixel{0,0,0,255})<=6);
                    Assert::IsTrue(moved[pixel].r>40 && moved[pixel].g>40,
                        L"measured old-card edges must restore even with zero display padding");
                }
                for(int y=card.top+4;y<card.bottom;++y)
                    for(int x=card.left;x<card.right;++x) {
                        if(x>=geometry.destination.left && x<geometry.destination.right &&
                            y>=geometry.destination.top && y<geometry.destination.bottom) continue;
                        const auto& pixel=moved[y*64+x];
                        Assert::IsTrue(pixel.r>40 && pixel.g>40,
                            L"all vacated original-card pixels must be restored, including old glyphs");
                    }
                for(const int y:{30,40,55})for(const int x:{1,62}) {
                    const int pixel=y*64+x;
                    Assert::IsTrue(PixelDistance(baseline[pixel],moved[pixel])<=6,
                        L"picture beyond measured card and destination must remain unchanged");
                }
                Assert::IsTrue(PixelDistance(moved[56*64+9],RgbaPixel{0,0,0,255})<=6,
                    L"restoration must stop exactly at the black-bar seam");
            }
            pl_mpv_user_shader_destroy(&hook);
        }

        TEST_METHOD(SubtitleGpuComposesBeforeCropAcrossAspectChangesAndNlsRecovery) {
            TargetLutGpuFixture fixture;Assert::IsTrue(fixture.Create());
            const auto* hook=CreateSubtitleCutPasteHook(fixture.Gpu());Assert::IsNotNull(hook);
            const auto source=ComputeSubtitleCutPaste({20,48,44,61},64,64,8,56,4);
            pl_hook broken{};broken.stages=PL_HOOK_RGB;broken.input=PL_HOOK_SIG_NONE;
            broken.signature=0x0070197;
            broken.hook=[](void*,const pl_hook_params*) {pl_hook_res r{};r.failed=true;return r;};
            for(const bool hdr:{false,true})for(const int mode:{3,5})
            for(const pl_rect2df crop: {pl_rect2df{0,8,64,56},pl_rect2df{0,12,64,52}}) {
                const auto g=FitSubtitleToVisiblePicture(source,{int(crop.x0),int(crop.y0),int(crop.x1),int(crop.y1)});
                Assert::IsTrue(g.valid);
                const auto transfer=hdr?PL_COLOR_TRC_PQ:PL_COLOR_TRC_SRGB;
                Assert::IsTrue(BindSubtitleCutPasteHook(hook,g,mode,transfer));
                const auto baseline=fixture.RenderSubtitlePattern(nullptr,hdr,0,g,transfer,true,false,100);
                const auto normal=fixture.RenderSubtitlePattern(hook,hdr,0,g,transfer,
                    true,false,100,false,0,nullptr,false,false,&crop);
                fixture.ResetFailedHook(broken.signature);
                const auto recovery=fixture.RenderSubtitlePattern(hook,hdr,0,g,transfer,
                    true,false,100,false,0,nullptr,false,false,&crop,&broken);
                const int sourceGlyph=54+g.destination.top-g.source.top;
                const int outputY=int((sourceGlyph+0.5-crop.y0)*64.0/(crop.y1-crop.y0));
                Assert::IsTrue(PixelDistance(normal[outputY*64+32],baseline[54*64+32])<=8,L"subtitle must be moved before source crop and scaling");
                for(size_t i=0;i<normal.size();++i)
                    Assert::IsTrue(PixelDistance(normal[i],recovery[i])<=6,
                        L"NLS-only failure must retain the precomposed subtitle on fallback");
            }
            pl_mpv_user_shader_destroy(&hook);
        }
        TEST_METHOD(SubtitleGpuRejectsBrightPictureSliverInsideCleanupFringe) {
            TargetLutGpuFixture fixture;Assert::IsTrue(fixture.Create());
            const auto* hook=CreateSubtitleCutPasteHook(fixture.Gpu());Assert::IsNotNull(hook);
            SubtitleBoxResult text;text.bounds={20,40,44,57};text.lineCount=1;
            text.lineBounds[0]={20,43,44,54};
            const SubtitleBoxRect measuredCard{15,37,49,58},actualCard{16,38,48,55};
            const auto original=ComputeSubtitleCutPaste(text.bounds,64,64,8,60,4,0,&measuredCard,20);
            Assert::IsTrue(original.valid);
            auto corrected=original;corrected.content=SubtitleGlyphCaptureBounds(text,64,64);
            const int seamY=56+original.destination.top-original.source.top;
            const int glyphY=48+original.destination.top-original.source.top;
            for(const int mode:{3,5}) {
                Assert::IsTrue(BindSubtitleCutPasteHook(hook,original,mode,PL_COLOR_TRC_SRGB));
                const auto before=fixture.RenderSubtitlePattern(hook,false,0,original,PL_COLOR_TRC_SRGB,
                    true,true,100,false,0,&actualCard,false,false,nullptr,nullptr,true);
                Assert::IsTrue(before[seamY*64+32].r>220,L"fixture must reproduce copied scenery in the old glyph fringe");
                Assert::IsTrue(BindSubtitleCutPasteHook(hook,corrected,mode,PL_COLOR_TRC_SRGB));
                const auto after=fixture.RenderSubtitlePattern(hook,false,0,original,PL_COLOR_TRC_SRGB,
                    true,true,100,false,0,&actualCard,false,false,nullptr,nullptr,true);
                Assert::IsTrue(after[seamY*64+32].r<(mode==3?8:160),L"picture seam must not be extracted as text");
                Assert::IsTrue(after[glyphY*64+32].r>220,L"actual glyph must remain intact");
            }
            pl_mpv_user_shader_destroy(&hook);
        }
        TEST_METHOD(PhysicalPictureEdgeDoesNotCopyCardFringeAsBarGlyph) {
            std::vector<RgbaPixel> pixels(64*64,RgbaPixel{0,0,0,255});
            auto fill=[&](int l,int t,int r,int b,uint8_t v) {
                for(int y=t;y<b;++y)for(int x=l;x<r;++x)pixels[y*64+x]={v,v,v,255};
            };
            fill(0,10,64,52,150);fill(20,39,44,49,0);
            fill(24,41,36,46,255);fill(20,50,44,51,170);
            fill(30,54,33,56,255);
            const SubtitleBoxRect cleanup{18,38,46,52};
            auto g=ComputeSubtitleCutPaste({20,40,44,56},64,64,10,49,{0,0,0},4,&cleanup);
            g.content={20,40,44,56};g.glyphLines[0]=g.content;
            g.pictureCapture={20,39,44,49};
            g=FitSubtitleToVisiblePicture(g,{0,10,64,52},4);
            Assert::IsTrue(g.valid);
            TargetLutGpuFixture fixture;Assert::IsTrue(fixture.Create());
            const auto* hook=CreateSubtitleCutPasteHook(fixture.Gpu());Assert::IsNotNull(hook);
            SubtitleGeneratedGrayStyle style;style.blurPixels=0;style.opacity=1;
            for(int mode:{3,5}) {
                Assert::IsTrue(BindSubtitleCutPasteHook(hook,g,mode,PL_COLOR_TRC_SRGB,style));
                const auto result=fixture.RenderSubtitlePattern(hook,false,0,g,PL_COLOR_TRC_SRGB,
                    true,false,-1,false,0,nullptr,false,false,nullptr,nullptr,false,-1,false,false,&pixels,64,64);
                const int dy=g.destination.top-g.source.top;
                Assert::IsTrue(result[(50+dy)*64+22].r<40,
                    L"picture fringe must not bypass card ownership through a one-row detector edge mismatch");
                Assert::IsTrue(result[(54+dy)*64+31].r>220,
                    L"actual glyph content in the physical black bar must still move");
            }
            // The same disputed rows remain available when the detector has
            // independently proved opaque backing and an actual glyph there.
            fill(20,49,44,53,0);fill(30,50,33,52,255);
            const SubtitleBoxRect proven{20,39,44,53};
            g=ComputeSubtitleCutPaste({20,40,44,56},64,64,10,49,{0,0,0},4,&cleanup,0,&proven);
            g.content={20,40,44,56};g.glyphLines[0]=g.content;
            g=FitSubtitleToVisiblePicture(g,{0,10,64,52},4);
            Assert::IsTrue(g.valid);
            Assert::IsTrue(BindSubtitleCutPasteHook(hook,g,3,PL_COLOR_TRC_SRGB,style));
            const auto attached=fixture.RenderSubtitlePattern(hook,false,0,g,PL_COLOR_TRC_SRGB,
                true,false,-1,false,0,nullptr,false,false,nullptr,nullptr,false,-1,false,false,&pixels,64,64);
            Assert::IsTrue(attached[(50+g.destination.top-g.source.top)*64+31].r>220,
                L"verified glyphs in boundary uncertainty rows must remain intact");
            pl_mpv_user_shader_destroy(&hook);
        }

        TEST_METHOD(CompositorBoundaryGapCannotEraseInteriorGlyphRows) {
            TargetLutGpuFixture fixture;Assert::IsTrue(fixture.Create());
            const auto* hook=CreateSubtitleCutPasteHook(fixture.Gpu());Assert::IsNotNull(hook);
            for(bool top:{false,true}) for(int gap:{1,2,3}) {
                std::vector<RgbaPixel> pixels(64*64,RgbaPixel{0,0,0,255});
                for(int y=10;y<52;++y)for(int x=0;x<64;++x)pixels[y*64+x]={80,80,80,255};
                for(int y=38;y<56;++y)for(int x=18;x<46;++x)pixels[y*64+x]={0,0,0,255};
                // Entire vertical strokes are already captured. Only the AR/card
                // ownership gap cuts them; no detector or OCR participates.
                for(int y=40;y<56;++y)for(int x=29;x<35;++x)pixels[y*64+x]={255,255,255,255};
                // Terminal strokes connect on just one side of the disputed band.
                for(int y=40;y<52;++y)for(int x=37;x<40;++x)pixels[y*64+x]={255,255,255,255};
                for(int y=52-gap;y<56;++y)for(int x=41;x<44;++x)pixels[y*64+x]={255,255,255,255};
                for(int y=52-gap;y<52;++y)pixels[y*64+22]={255,255,255,255}; // detached stripe
                if(top)for(int y=0;y<32;++y)for(int x=0;x<64;++x)std::swap(pixels[y*64+x],pixels[(63-y)*64+x]);
                const SubtitleBoxRect card{18,top?12+gap:38,46,top?26:52-gap};
                const SubtitleBoxRect ink{20,top?8:40,44,top?24:56};
                auto g=ComputeSubtitleCutPaste(ink,64,64,top?12+gap:10,top?54:52-gap,{4,4,4},4,&card,0,&card);
                g.content=ink;g.glyphLines[0]=ink;
                g=FitSubtitleToVisiblePicture(g,top?SubtitleBoxRect{0,12,64,54}:SubtitleBoxRect{0,10,64,52},4);
                Assert::IsTrue(g.valid);
                auto reference=g;if(top)reference.pictureCapture.top=12;else reference.pictureCapture.bottom=52;
                SubtitleGeneratedGrayStyle style;style.blurPixels=0;style.opacity=1;
                for(int mode:{3,5}) {
                    auto render=[&](const SubtitleCutPasteGeometry& geometry) {
                        Assert::IsTrue(BindSubtitleCutPasteHook(hook,geometry,mode,PL_COLOR_TRC_SRGB,style));
                        return fixture.RenderSubtitlePattern(hook,false,0,geometry,PL_COLOR_TRC_SRGB,
                            true,false,-1,false,0,nullptr,false,false,nullptr,nullptr,false,-1,false,false,&pixels,64,64);
                    };
                    const auto expected=render(reference),actual=render(g);
                    for(int y=ink.top;y<ink.bottom;++y) for(int x:{31,38,42}) {
                        const int index=(y+g.destination.top-g.source.top)*64+x;
                        if(x==31)Assert::IsTrue(expected[index].r>220);
                        Assert::IsTrue(std::abs(int(expected[index].r)-int(actual[index].r))<=1,
                            L"compositor must not erase interior glyph rows when card and AR boundaries differ");
                    }
                    for(int row=52-gap;row<52;++row) {
                        const int y=top?63-row:row;
                        Assert::IsTrue(actual[(y+g.destination.top-g.source.top)*64+22].r<40,
                            L"detached bright content in the disputed strip is not a connected glyph");
                    }
                }
            }
            pl_mpv_user_shader_destroy(&hook);
        }

        TEST_METHOD(AntialiasedBoundaryRowSurvivesSolidAndReconstructedComposition) {
            constexpr int w=640,h=360;
            TargetLutGpuFixture fixture;Assert::IsTrue(fixture.Create());
            const auto* hook=CreateSubtitleCutPasteHook(fixture.Gpu());Assert::IsNotNull(hook);
            for(bool top:{false,true}) {
                std::vector<RgbaPixel> pixels(w*h,RgbaPixel{0,0,0,255});
                auto fill=[&](int l,int t,int r,int b,uint8_t v) {
                    for(int y=t;y<b;++y)for(int x=l;x<r;++x)pixels[y*w+x]={v,v,v,255};
                };
                fill(0,45,w,315,50);fill(180,298,465,315,0);
                for(int n=0;n<18;++n) {
                    const int x=195+13*n;
                    fill(x-1,304,x+9,320,0);fill(x,305,x+8,319,255);fill(x+2,307,x+6,317,0);
                }
                for(int y=316;y<318;++y)for(int x=180;x<465;++x)
                    if(pixels[y*w+x].r==255)pixels[y*w+x]={120,120,120,255};
                if(top)for(int y=0;y<h/2;++y)for(int x=0;x<w;++x)std::swap(pixels[y*w+x],pixels[(h-1-y)*w+x]);
                AnalysisLumaSource source;source.data=reinterpret_cast<const uint8_t*>(pixels.data());
                source.dataBytes=pixels.size()*sizeof(RgbaPixel);source.width=w;source.height=h;source.rowBytes=w*4;
                source.format=AnalysisLumaFormat::NativeRgb;source.encoding=VideoFrameEncoding::BGRA_8BIT;source.generation=1;
                SubtitleBoxDetector detector;const auto text=detector.Analyze(source,45,315,1,1);
                Assert::IsTrue(text.detected);
                auto g=ComputeSubtitleCutPaste(text.bounds,w,h,45,315,{6,6,6},5,&text.sourcePanel,0,&text.capturePanel);
                g.content=SubtitleGlyphCaptureBounds(text,w,h);g.glyphLines=SubtitleGlyphCaptureLines(text,w,h);
                g=FitSubtitleToVisiblePicture(g,top?SubtitleBoxRect{0,43,w,315}:SubtitleBoxRect{0,45,w,317},5);
                Assert::IsTrue(g.valid);
                SubtitleGeneratedGrayStyle style;style.blurPixels=30;style.opacity=1;
                for(int mode:{3,5}) {
                    Assert::IsTrue(BindSubtitleCutPasteHook(hook,g,mode,PL_COLOR_TRC_SRGB,style));
                    const auto output=fixture.RenderSubtitlePattern(hook,false,0,g,PL_COLOR_TRC_SRGB,
                        true,false,-1,false,0,nullptr,false,false,nullptr,nullptr,false,-1,false,false,&pixels,w,h);
                    const int y=(top?h-1-316:316)+g.destination.top-g.source.top;
                    const int x=196+g.destination.left-g.source.left;
                    // Compare against explicitly known fixture backing; keying
                    // is transfer-dependent, so raw source byte120 is not an
                    // assertion about the final composited antialias value.
                    auto reference=g;reference.pictureCapture={180,top?42:298,465,top?62:318};
                    reference.content={195,top?41:305,424,top?55:319};
                    reference.glyphLines[0]=reference.content;
                    Assert::IsTrue(BindSubtitleCutPasteHook(hook,reference,mode,PL_COLOR_TRC_SRGB,style));
                    const auto expected=fixture.RenderSubtitlePattern(hook,false,0,reference,PL_COLOR_TRC_SRGB,
                        true,false,-1,false,0,nullptr,false,false,nullptr,nullptr,false,-1,false,false,&pixels,w,h);
                    Assert::IsTrue(expected[y*w+x].r>8,L"fixture must render visible antialiased ink at the disputed row");
                    std::ostringstream diagnostic;
                    diagnostic<<"AA boundary top="<<top<<" mode="<<mode<<" pixel="<<int(output[y*w+x].r)<<" expected="<<int(expected[y*w+x].r)
                        <<" card="<<g.pictureCapture.left<<","<<g.pictureCapture.top<<"-"<<g.pictureCapture.right<<","<<g.pictureCapture.bottom
                        <<" line="<<g.glyphLines[0].left<<","<<g.glyphLines[0].top<<"-"<<g.glyphLines[0].right<<","<<g.glyphLines[0].bottom
                        <<" source="<<g.source.top<<" destination="<<g.destination.top<<" sample="<<x<<","<<y;
                    Logger::WriteMessage(diagnostic.str().c_str());
                    Assert::IsTrue(std::abs(int(output[y*w+x].r)-int(expected[y*w+x].r))<=1,
                        L"an antialiased stroke must not become a missing row at the physical bar boundary");
                }
            }
            pl_mpv_user_shader_destroy(&hook);
        }
        TEST_METHOD(AttachedCleanupIncludesReconciledRowsBeforeGaussianBlur) {
            std::vector<RgbaPixel> pixels(64*64,RgbaPixel{0,0,0,255});
            for(int y=10;y<52;++y)for(int x=0;x<64;++x)pixels[y*64+x]={80,80,80,255};
            for(int y=39;y<49;++y)for(int x=20;x<44;++x)pixels[y*64+x]={0,0,0,255};
            for(int y=49;y<52;++y)for(int x=20;x<44;++x)pixels[y*64+x]={220,220,220,255};
            for(int y=54;y<56;++y)for(int x=30;x<33;++x)pixels[y*64+x]={255,255,255,255};
            const SubtitleBoxRect card{20,39,44,49};
            auto g=ComputeSubtitleCutPaste({20,40,44,56},64,64,10,49,{0,0,0},4,&card,0,&card);
            g.content={20,40,44,56};g.glyphLines[0]=g.content;
            g=FitSubtitleToVisiblePicture(g,{0,10,64,52},4);
            auto oracle=g;oracle.generatedCleanup.bottom=52;
            TargetLutGpuFixture fixture;Assert::IsTrue(fixture.Create());
            const auto* hook=CreateSubtitleCutPasteHook(fixture.Gpu());Assert::IsNotNull(hook);
            for(int mode:{3,5})for(float radius:{0.0f,3.0f,30.0f}) {
                SubtitleGeneratedGrayStyle style;style.blurPixels=radius;style.opacity=0;style.maxLuminance=1;
                auto render=[&](const SubtitleCutPasteGeometry& geometry) {
                    Assert::IsTrue(BindSubtitleCutPasteHook(hook,geometry,mode,PL_COLOR_TRC_SRGB,style));
                    return fixture.RenderSubtitlePattern(hook,false,0,geometry,PL_COLOR_TRC_SRGB,
                        true,false,-1,false,0,nullptr,false,false,nullptr,nullptr,false,-1,false,false,&pixels,64,64);
                };
                const auto expected=render(oracle),actual=render(g);
                for(size_t i=0;i<actual.size();++i)Assert::IsTrue(std::abs(int(actual[i].r)-int(expected[i].r))<=1,
                    L"attached edge must use the same reconstructed/blurred backing as the original card");
            }
            pl_mpv_user_shader_destroy(&hook);
        }
        TEST_METHOD(ReducedSubtitleGpuMatchesNativeAreaCoverageWithoutSceneLeakOrCleanupSliver) {
            constexpr int w=128,h=128;
            TargetLutGpuFixture fixture;Assert::IsTrue(fixture.Create());
            const auto* hook=CreateSubtitleCutPasteHook(fixture.Gpu());Assert::IsNotNull(hook);
            const SubtitleBoxPadding padding{4,4,4};
            SubtitleGeneratedGrayStyle style;style.color={0,0,0};style.opacity=1;style.blurPixels=6;style.maxLuminance=1;
            for(bool top:{false,true})for(int kind:{0,1,2})for(int gap:{0,7}) {
                SubtitleBoxRect content=kind==0?SubtitleBoxRect{40,114,88,124}:
                    kind==1?SubtitleBoxRect{40,104,88,118}:SubtitleBoxRect{40,92,88,102};
                SubtitleBoxRect card=kind==0?SubtitleBoxRect{}:
                    kind==1?SubtitleBoxRect{33,108,94,112}:SubtitleBoxRect{34,88,94,106};
                std::vector<RgbaPixel> pixels(w*h,RgbaPixel{0,0,0,255});
                auto fill=[&](int l,int t,int r,int b,RgbaPixel v) {
                    for(int y=t;y<b;++y)for(int x=l;x<r;++x)pixels[y*w+x]=v;
                };
                fill(0,16,w,112,{65,90,120,255});
                if(card.Valid())fill(card.left,card.top,card.right,card.bottom,{0,0,0,255});
                const int first=kind==1?108:content.top+1;
                fill(46,first,50,content.bottom-1,{255,255,255,255});
                fill(60,first,61,content.bottom-1,{255,255,255,255});
                pixels[(content.bottom-2)*w+85]={255,255,255,255}; // isolated punctuation
                if(kind==1)fill(40,104,88,106,{255,255,255,255}); // bright picture inside coarse ink envelope, outside card
                if(top) {
                    for(int y=0;y<h/2;++y)for(int x=0;x<w;++x)std::swap(pixels[y*w+x],pixels[(h-1-y)*w+x]);
                    content={content.left,h-content.bottom,content.right,h-content.top};
                    if(card.Valid())card={card.left,h-card.bottom,card.right,h-card.top};
                }
                auto base=ComputeSubtitleCutPaste(content,w,h,16,112,padding,gap,&card,20,&card,true);
                base.glyphLines[0]=content;base=FitSubtitleToVisiblePicture(base,{0,16,w,112},gap);
                Assert::IsTrue(base.valid);
                for(int mode:{3,5}) {
                    auto render=[&](const SubtitleCutPasteGeometry& geometry) {
                        Assert::IsTrue(BindSubtitleCutPasteHook(hook,geometry,mode,PL_COLOR_TRC_SRGB,style));
                        return fixture.RenderSubtitlePattern(hook,false,0,geometry,PL_COLOR_TRC_SRGB,
                            true,false,-1,false,0,nullptr,false,false,nullptr,nullptr,false,-1,false,false,&pixels,w,h);
                    };
                    // Zero reduction now uses centered placement too; verify a
                    // real shader result rather than the superseded source pivot.
                    const auto centered=ApplySubtitleTextReduction(base,0,padding);
                    const auto zero=render(centered);
                    Assert::IsTrue(centered.valid && centered.centeredGlyphMapping && !zero.empty());
                    for(int percent:{0,25,50,75}) {
                        const auto g=ApplySubtitleTextReduction(base,percent,padding);
                        const auto output=render(g);
                        const double scale=g.glyphScale,footprint=1.0/scale;
                        const int left=int(std::floor(scale*content.left+g.glyphTranslateX))-1;
                        const int right=int(std::ceil(scale*content.right+g.glyphTranslateX))+1;
                        const int firstRow=int(std::floor(scale*content.top+g.glyphTranslateY))-1;
                        const int lastRow=int(std::ceil(scale*content.bottom+g.glyphTranslateY))+1;
                        double expectedEnergy=0;unsigned coveredPixels=0;
                        for(int y=firstRow;y<lastRow;++y)for(int x=left;x<right;++x) {
                            const double sx=(x+0.5-g.glyphTranslateX)/scale,sy=(y+0.5-g.glyphTranslateY)/scale;
                            const double l=sx-footprint/2,r=sx+footprint/2,t=sy-footprint/2,b=sy+footprint/2;
                            double alpha=0;
                            // Independent exact cell-area oracle: only bright owned native pixels count.
                            for(int yy=int(std::floor(t));yy<int(std::ceil(b));++yy)
                                for(int xx=int(std::floor(l));xx<int(std::ceil(r));++xx) {
                                    if(xx<content.left || xx>=content.right || yy<content.top || yy>=content.bottom)continue;
                                    const bool picture=yy>=g.pictureTop && yy<g.pictureBottom;
                                    const bool owned=card.Valid() && xx>=card.left && xx<card.right && yy>=card.top && yy<card.bottom;
                                    if(picture && !owned)continue;
                                    if(pixels[yy*w+xx].r!=255 || pixels[yy*w+xx].g!=255)continue;
                                    const double overlapX=(std::min)(r,double(xx+1))-(std::max)(l,double(xx));
                                    alpha+=overlapX*((std::min)(b,double(yy+1))-(std::max)(t,double(yy)));
                                }
                            alpha/=footprint*footprint;
                            expectedEnergy+=alpha;coveredPixels+=alpha>0;
                            const double expected=255.0*(alpha<=0.0031308?12.92*alpha:1.055*std::pow(alpha,1.0/2.4)-0.055);
                            Assert::IsTrue(std::abs(int(output[y*w+x].r)-expected)<=4.0 &&
                                std::abs(int(output[y*w+x].g)-expected)<=4.0,
                                L"native-keyed area filtering preserves thin strokes and excludes unowned scene content");
                        }
                        Assert::IsTrue(coveredPixels>2 && expectedEnergy>0);
                        const int edgeRow=top?16:111;
                        Assert::IsTrue(output[edgeRow*w+64].r<5 && output[edgeRow*w+64].g<5,
                            L"reduced panel remains filled through the physical black-bar boundary");
                        for(int y=0;y<h;++y)if(y<16 || y>=112)
                            for(int x=content.left;x<content.right;++x)if(pixels[y*w+x].r==255)
                                Assert::IsTrue(output[y*w+x].r<5,L"original bar glyphs are removed at every text size");
                    }
                }
            }
            pl_mpv_user_shader_destroy(&hook);
        }
        TEST_METHOD(SubtitleFloatingCornersAndBridgeUseTheSameGpuMaskAtBothEdges) {
            constexpr int w=128,h=128;
            TargetLutGpuFixture fixture;Assert::IsTrue(fixture.Create());
            const auto* hook=CreateSubtitleCutPasteHook(fixture.Gpu());Assert::IsNotNull(hook);
            for(bool top:{false,true})for(bool rounded:{false,true})for(bool floating:{false,true})for(int mode:{3,5}) {
                std::vector<RgbaPixel> pixels(w*h,RgbaPixel{0,0,0,255});
                for(int y=16;y<112;++y)for(int x=0;x<w;++x)pixels[y*w+x]={80,100,120,255};
                const SubtitleBoxRect ink=top?SubtitleBoxRect{40,3,88,11}:SubtitleBoxRect{40,117,88,125};
                for(int y=ink.top;y<ink.bottom;++y)for(int x=50;x<78;++x)pixels[y*w+x]={255,255,255,255};
                const SubtitleBoxRect empty{};
                auto g=ComputeSubtitleCutPaste(ink,w,h,16,112,{5,5,5},12,&empty,0,&empty);
                g=ApplySubtitleTextReduction(g,0,{5,5,5},rounded,floating);
                Assert::IsTrue(g.valid);
                SubtitleGeneratedGrayStyle style;style.color={0,0,0};style.opacity=1;style.blurPixels=6;
                Assert::IsTrue(BindSubtitleCutPasteHook(hook,g,mode,PL_COLOR_TRC_SRGB,style));
                const auto output=fixture.RenderSubtitlePattern(hook,false,0,g,PL_COLOR_TRC_SRGB,
                    true,false,-1,false,0,nullptr,false,false,nullptr,nullptr,false,-1,false,false,&pixels,w,h);
                const int bridgeY=top?21:106;
                Assert::IsTrue(floating?output[bridgeY*w+64].r>=75:output[bridgeY*w+64].r<5,
                    L"floating retains the picture gap; attached fills it");
                const int cornerX=g.destination.left-6,cornerY=top?g.destination.bottom-1:g.destination.top;
                // At half a pixel inside a square edge, the existing 0.75px
                // smoothstep retains ~7.4% of linear backdrop (about 18 sRGB
                // levels here). Rounded corners expose much more picture.
                const auto cornerMessage=std::wstring(L"corner switch: r=")+std::to_wstring(output[cornerY*w+cornerX].r);
                Assert::IsTrue(rounded?output[cornerY*w+cornerX].r>50:output[cornerY*w+cornerX].r<25,
                    cornerMessage.c_str());
                const int cx=int(64*g.glyphScale+g.glyphTranslateX);
                const int cy=int((ink.top+ink.bottom)*0.5f*g.glyphScale+g.glyphTranslateY);
                Assert::IsTrue(output[cy*w+cx].r>230,L"explicit centered mapping also works at 100 percent size");
                const auto roi=ComputeSubtitleGaussianRegion(g,mode,6);
                if(mode==5 && floating)Assert::IsTrue(top?roi.top>0:roi.top+roi.height*roi.step<128,
                    L"float blur bounds exclude needless extension");
            }
            pl_mpv_user_shader_destroy(&hook);
        }
        TEST_METHOD(QuarterSizeSubtitlePunctuationSurvivesEveryNativePhaseAtBothEdges) {
            constexpr int w=128,h=128;
            TargetLutGpuFixture fixture;Assert::IsTrue(fixture.Create());
            const auto* hook=CreateSubtitleCutPasteHook(fixture.Gpu());Assert::IsNotNull(hook);
            SubtitleGeneratedGrayStyle style;style.color={0,0,0};style.opacity=1;style.blurPixels=0;
            for(bool top:{false,true})for(int phase=0;phase<4;++phase) {
                std::vector<RgbaPixel> pixels(w*h,RgbaPixel{0,0,0,255});
                for(int y=16;y<112;++y)for(int x=0;x<w;++x)pixels[y*w+x]={50,70,90,255};
                const int px=81+phase,py=115+phase;
                pixels[(top?h-1-py:py)*w+px]={255,255,255,255};
                const SubtitleBoxRect content=top?SubtitleBoxRect{40,4,88,14}:SubtitleBoxRect{40,114,88,124};
                const SubtitleBoxRect empty{};
                auto g=ComputeSubtitleCutPaste(content,w,h,16,112,{4,4,4},7,&empty,0,&empty);
                g.glyphLines[0]=content;g=FitSubtitleToVisiblePicture(g,{0,16,w,112},7);
                g=ApplySubtitleTextReduction(g,75,{4,4,4});Assert::IsTrue(g.valid);
                for(int mode:{3,5}) {
                    Assert::IsTrue(BindSubtitleCutPasteHook(hook,g,mode,PL_COLOR_TRC_SRGB,style));
                    const auto output=fixture.RenderSubtitlePattern(hook,false,0,g,PL_COLOR_TRC_SRGB,
                        true,false,-1,false,0,nullptr,false,false,nullptr,nullptr,false,-1,false,false,&pixels,w,h);
                    const int x=int(std::floor((px+0.5f)*g.glyphScale+g.glyphTranslateX));
                    const int y=int(std::floor(((top?h-1-py:py)+0.5f)*g.glyphScale+g.glyphTranslateY));
                    int peak=0;
                    for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx)
                        peak=(std::max)(peak,int(output[(y+dy)*w+x+dx].r));
                    Assert::IsTrue(peak>=30,L"keying after averaging would erase this single-pixel punctuation");
                }
            }
            pl_mpv_user_shader_destroy(&hook);
        }

        TEST_METHOD(RecordedSubtitlePlaybackDiagnostic) {
            char path[4096]{};
            if(!GetEnvironmentVariableA("VP_SUBTITLE_REPLAY_BGRA",path,sizeof(path))) { Logger::WriteMessage("Recording diagnostic not requested");return; }
            std::ifstream file(path,std::ios::binary|std::ios::ate);Assert::IsTrue(file.good());
            const bool uhd=file.tellg()==std::streamoff(3840ll*2160*4);file.seekg(0);
            const int width=uhd?3840:2560,height=uhd?2160:1440;
            const int pictureTop=uhd?263:175,pictureBottom=uhd?1897:1265;
            std::vector<RgbaPixel> pixels(width*height);
            file.read(reinterpret_cast<char*>(pixels.data()),pixels.size()*sizeof(RgbaPixel));
            Assert::AreEqual(std::streamsize(pixels.size()*sizeof(RgbaPixel)),file.gcount());
            char hdr[8]{};const bool pq=GetEnvironmentVariableA("VP_SUBTITLE_REPLAY_PQ",hdr,sizeof(hdr)) && hdr[0]=='1';
            const auto transfer=pq?PL_COLOR_TRC_PQ:PL_COLOR_TRC_SRGB;
            AnalysisLumaSource source;source.data=reinterpret_cast<const uint8_t*>(pixels.data());
            source.dataBytes=pixels.size()*4;source.width=width;source.height=height;
            source.rowBytes=width*4;source.format=AnalysisLumaFormat::NativeRgb;
            source.encoding=VideoFrameEncoding::BGRA_8BIT;source.generation=1;
            SubtitleBoxDetector detector;detector.SetNearBarDistance(20);
            const auto text=detector.Analyze(source,pictureTop,pictureBottom,1,1);
            Assert::IsTrue(text.detected);
            SubtitlePaddingPolicy policy{true};
            const auto pad=SubtitleRelativePadding(SubtitleTypicalLineHeight(text),policy);
            auto g=ComputeSubtitleCutPaste(SubtitleGlyphCaptureBounds(text,width,height),width,height,pictureTop,pictureBottom,pad,15,&text.sourcePanel,20,
                text.capturePanelMeasured?&text.capturePanel:nullptr);
            g.content=SubtitleGlyphCaptureBounds(text,width,height);g.glyphLines=SubtitleGlyphCaptureLines(text,width,height);
            g=FitSubtitleToVisiblePicture(g,{0,uhd?262:pictureTop,width,uhd?1898:pictureBottom},15);
            char reductionText[16]{};
            const int reduction=GetEnvironmentVariableA("VP_SUBTITLE_REPLAY_REDUCTION",reductionText,sizeof(reductionText))
                ? (std::max)(0,(std::min)(75,std::atoi(reductionText))) : 0;
            g=ApplySubtitleTextReduction(g,reduction,pad);
            Assert::IsTrue(g.valid);
            if(pq) for(auto& pixel:pixels) for(auto* channel:{&pixel.r,&pixel.g,&pixel.b}) {
                const double v=*channel/255.0;
                const double linear=v<=0.04045?v/12.92:std::pow((v+0.055)/1.055,2.4);
                const double power=std::pow(linear*203.0/10000.0,0.1593017578125);
                *channel=uint8_t(std::lround(255*std::pow((0.8359375+18.8515625*power)/(1+18.6875*power),78.84375)));
            }
            for(auto& pixel:pixels)std::swap(pixel.r,pixel.b);
            TargetLutGpuFixture fixture;Assert::IsTrue(fixture.Create());
            const auto* hook=CreateSubtitleCutPasteHook(fixture.Gpu());Assert::IsNotNull(hook);
            SubtitleGeneratedGrayStyle style;style.blurPixels=30;
            Assert::IsTrue(BindSubtitleCutPasteHook(hook,g,5,transfer,style));
            const auto result=fixture.RenderSubtitlePattern(hook,false,0,g,transfer,
                true,false,-1,false,0,nullptr,false,false,nullptr,nullptr,false,-1,false,false,&pixels,width,height);
            std::ofstream output(std::string(path)+".rgba",std::ios::binary);
            output.write(reinterpret_cast<const char*>(result.data()),result.size()*sizeof(RgbaPixel));
            std::ofstream geometry(std::string(path)+".geometry.txt");
            geometry<<"reduction "<<reduction<<" glyph_mapping "<<g.glyphScale<<","<<g.glyphTranslateX<<","<<g.glyphTranslateY<<"\n";
            geometry<<g.source.left<<","<<g.source.top<<","<<g.source.right<<","<<g.source.bottom<<"\n"
                <<g.destination.left<<","<<g.destination.top<<","<<g.destination.right<<","<<g.destination.bottom<<"\n"
                <<"panel "<<text.sourcePanel.left<<","<<text.sourcePanel.top<<","<<text.sourcePanel.right<<","<<text.sourcePanel.bottom<<"\n";
            for(const auto& line:text.lineBounds)geometry<<"line "<<line.left<<","<<line.top<<","<<line.right<<","<<line.bottom<<"\n";
            pl_mpv_user_shader_destroy(&hook);
        }

        TEST_METHOD(HorizontalCaptureGuardPreservesTerminalGlyphStroke) {
            TargetLutGpuFixture fixture;Assert::IsTrue(fixture.Create());
            const auto* hook=CreateSubtitleCutPasteHook(fixture.Gpu());Assert::IsNotNull(hook);
            SubtitleBoxResult text;text.bounds={20,40,44,57};text.lineCount=1;
            // The fixture's white stroke occupies x=30..33. A sampled terminal
            // extent at 32 used to clip its two rightmost native pixels.
            text.lineBounds[0]={20,43,32,54};
            const SubtitleBoxRect card{15,37,49,58};
            const auto original=ComputeSubtitleCutPaste(text.bounds,64,64,8,60,4,0,&card,20);
            auto guarded=original;guarded.content=SubtitleGlyphCaptureBounds(text,64,64);
            guarded.glyphLines=SubtitleGlyphCaptureLines(text,64,64);
            const int y=48+original.destination.top-original.source.top;
            for(int mode:{3,5}) {
                auto clipped=original;clipped.content=text.lineBounds[0];clipped.glyphLines[0]=text.lineBounds[0];
                Assert::IsTrue(BindSubtitleCutPasteHook(hook,clipped,mode,PL_COLOR_TRC_SRGB));
                const auto before=fixture.RenderSubtitlePattern(hook,false,0,original,PL_COLOR_TRC_SRGB,
                    true,true,100,false,0,&card,false,false);
                Assert::IsTrue(before[y*64+33].r<180,L"fixture reproduces truncated terminal stroke");
                Assert::IsTrue(BindSubtitleCutPasteHook(hook,guarded,mode,PL_COLOR_TRC_SRGB));
                const auto after=fixture.RenderSubtitlePattern(hook,false,0,original,PL_COLOR_TRC_SRGB,
                    true,true,100,false,0,&card,false,false);
                Assert::IsTrue(after[y*64+33].r>220,L"both native fringe pixels must move with the glyph");
            }
            pl_mpv_user_shader_destroy(&hook);
        }

        TEST_METHOD(SubtitleGpuRejectsSeamsBetweenLinesAndAtTrailingCell) {
            TargetLutGpuFixture fixture;Assert::IsTrue(fixture.Create());
            const auto* hook=CreateSubtitleCutPasteHook(fixture.Gpu());Assert::IsNotNull(hook);
            SubtitleBoxResult text;text.bounds={20,40,44,57};text.lineCount=2;
            text.lineBounds[0]={20,40,44,43};text.lineBounds[1]={20,47,44,54};
            const SubtitleBoxRect card{15,37,49,58};
            const auto original=ComputeSubtitleCutPaste(text.bounds,64,64,8,60,4,0,&card,20);
            auto corrected=original;corrected.content=SubtitleGlyphCaptureBounds(text,64,64);
            corrected.glyphLines=SubtitleGlyphCaptureLines(text,64,64);
            for(const int mode:{3,5}) for(const int sourceY:{44,54,55,56}) {
                const int y=sourceY+original.destination.top-original.source.top;
                Assert::IsTrue(BindSubtitleCutPasteHook(hook,original,mode,PL_COLOR_TRC_SRGB));
                const auto before=fixture.RenderSubtitlePattern(hook,false,0,original,PL_COLOR_TRC_SRGB,
                    true,true,100,false,0,&card,false,false,nullptr,nullptr,true,sourceY);
                Assert::IsTrue(before[y*64+32].r>220,L"original rectangular extraction reproduces seam");
                Assert::IsTrue(BindSubtitleCutPasteHook(hook,corrected,mode,PL_COLOR_TRC_SRGB));
                const auto after=fixture.RenderSubtitlePattern(hook,false,0,original,PL_COLOR_TRC_SRGB,
                    true,true,100,false,0,&card,false,false,nullptr,nullptr,true,sourceY);
                Assert::IsTrue(after[y*64+32].r<(mode==3?8:160));
                const int glyphY=48+original.destination.top-original.source.top;
                Assert::IsTrue(after[glyphY*64+32].r>220,L"glyph survives seam rejection");
            }
            pl_mpv_user_shader_destroy(&hook);
        }
        TEST_METHOD(TopSubtitleGpuMovesGlyphDownAndRestoresPictureFromBelow) {
            TargetLutGpuFixture fixture;Assert::IsTrue(fixture.Create());
            const auto* hook=CreateSubtitleCutPasteHook(fixture.Gpu());Assert::IsNotNull(hook);
            const SubtitleBoxRect card{10,2,54,19};
            const auto g=ComputeSubtitleCutPaste({20,3,44,16},64,64,8,56,
                SubtitleBoxPadding(4,2,5),0,&card);
            Assert::IsTrue(g.valid);Assert::AreEqual(8,g.destination.top);
            for(const int mode:{3,5}) {
                Assert::IsTrue(BindSubtitleCutPasteHook(hook,g,mode,PL_COLOR_TRC_SRGB));
                const auto moved=fixture.RenderSubtitlePattern(hook,false,0,g,
                    PL_COLOR_TRC_SRGB,true,true,160,false,0,&card);
                const int glyphY=8+g.destination.top-g.source.top;
                Assert::IsTrue(moved[glyphY*64+32].r>220,L"top glyph must survive downward movement");
                const auto& vacated=moved[9*64+11];
                if(mode==5) Assert::IsTrue(vacated.r>60,L"old top-card edge must use clean picture below");
                else Assert::IsTrue(vacated.r<8);
                Assert::IsTrue(moved[1*64+32].r<8,L"top bar remains black");
            }
            pl_mpv_user_shader_destroy(&hook);
        }
        TEST_METHOD(GeneratedGrayRemovesEntireCardRegardlessOfCardBrightness)
        {
            TargetLutGpuFixture fixture;Assert::IsTrue(fixture.Create());
            const auto* hook=CreateSubtitleCutPasteHook(fixture.Gpu());Assert::IsNotNull(hook);
            const auto geometry=ComputeSubtitleCutPaste({20,40,44,57},64,64,8,56,SubtitleBoxPadding(12,12,8));
            Assert::IsTrue(BindSubtitleCutPasteHook(hook,geometry,5,PL_COLOR_TRC_SRGB));
            const auto black=fixture.RenderSubtitlePattern(hook,false,0,geometry,PL_COLOR_TRC_SRGB,true,true,200,false,0);
            const auto gray=fixture.RenderSubtitlePattern(hook,false,0,geometry,PL_COLOR_TRC_SRGB,true,true,200,false,85);
            // These are outside the glyph rectangle and include the last picture
            // row: no original card tint/outline may survive under the new panel.
            for(int y=geometry.source.top+2;y<geometry.pictureBottom;++y)
                for(const int x:{geometry.source.left+2,geometry.source.right-3})
                    Assert::IsTrue(PixelDistance(black[y*64+x],gray[y*64+x])<=6,
                        L"source card shading must not survive outside the glyph bounds");
            pl_mpv_user_shader_destroy(&hook);
        }

		TEST_METHOD(GeneratedGrayPreservesReflectedTextureAndSoftensBrightBacking)
		{
			TargetLutGpuFixture fixture;Assert::IsTrue(fixture.Create());
			const auto* hook=CreateSubtitleCutPasteHook(fixture.Gpu());Assert::IsNotNull(hook);
			const auto geometry=ComputeSubtitleCutPaste({12,40,52,57},64,64,8,56,
				SubtitleBoxPadding(6,12,8));
			Assert::IsTrue(geometry.valid);
			SubtitleGeneratedGrayStyle style;
            // This regression exercises the previously configurable gray tint.
            // Near-black runtime defaults are covered independently.
            Assert::IsTrue(ParseSubtitleRgbHex("505050",style.color));
			Assert::IsTrue(BindSubtitleCutPasteHook(hook,geometry,5,PL_COLOR_TRC_SRGB,style));
			const auto dark=fixture.RenderSubtitlePattern(hook,false,0,geometry,
				PL_COLOR_TRC_SRGB,true,true,16,true);
			const auto bright=fixture.RenderSubtitlePattern(hook,false,0,geometry,
				PL_COLOR_TRC_SRGB,true,true,230,true);
			auto decode=[](uint8_t byte) {
				const double v=double(byte)/255.0;
				return v<=0.04045 ? v/12.92 : std::pow((v+0.055)/1.055,2.4);
			};
			const int panel=28*64+17;
			for(const auto* image:{&dark,&bright}) {
				const auto& pixel=(*image)[panel];
				const double luminance=0.2126*decode(pixel.r)+0.7152*decode(pixel.g)+
					0.0722*decode(pixel.b);
				Assert::IsTrue(luminance<=0.17,
					L"the gray overlay and soft shoulder must subdue the bright backing");
			}
			Assert::IsTrue(int(bright[panel].r)>int(dark[panel].r)+10,
				L"the backing must retain a visible difference between bright and dark scenes");
			const int oldCardEdge=54*64+25;
			Assert::IsTrue(dark[oldCardEdge].r>20 && bright[oldCardEdge].r>20,
				L"the old black card must be reconstructed through pictureBottom");
			const auto noStair=fixture.RenderSubtitlePattern(hook,false,0,geometry,
				PL_COLOR_TRC_SRGB,true,true,16,false);
			const int reflectedStair=36*64+25;
			const int textureDelta=int(dark[reflectedStair].r)-int(noStair[reflectedStair].r);
			const std::wstring textureEvidence=L"reflected texture delta="+std::to_wstring(textureDelta)+
				L" bright="+std::to_wstring(bright[reflectedStair].r)+
				L" side="+std::to_wstring(noStair[reflectedStair].r);
			Assert::IsTrue(textureDelta>4,textureEvidence.c_str());
			const int movedGlyph=39*64+31;
			Assert::IsTrue(bright[movedGlyph].r>220 && dark[movedGlyph].r>220,
				L"the generated backing must not dim moved white glyphs");
			pl_mpv_user_shader_destroy(&hook);
		}

        TEST_METHOD(GeneratedBlurSoftensWholeOverlayKeepingGlyphsAndOutsidePictureSharp)
        {
            TargetLutGpuFixture fixture;Assert::IsTrue(fixture.Create());
            const auto* hook=CreateSubtitleCutPasteHook(fixture.Gpu());Assert::IsNotNull(hook);
            const auto g=ComputeSubtitleCutPaste({12,40,52,57},64,64,8,56,SubtitleBoxPadding(6,12,8));
            SubtitleGeneratedGrayStyle style;style.opacity=0.0f;style.maxLuminance=1.0f;
            for(const bool hdr:{false,true}) {
                const auto transfer=hdr?PL_COLOR_TRC_PQ:PL_COLOR_TRC_SRGB;
                auto render=[&](float radius,int mode=5) {
                    style.blurPixels=radius;Assert::IsTrue(BindSubtitleCutPasteHook(hook,g,mode,transfer,style));
                    return fixture.RenderSubtitlePattern(hook,hdr,0,g,transfer,true,true,100,false,0,
                        nullptr,false,false,nullptr,nullptr,false,-1,false,true);
                };
                const auto sharp=render(0),blurred=render(3),again=render(0);
                // Check both real picture under the panel and reconstructed
                // picture below it. The former was previously left sharp.
                for(const auto rows:{std::pair<int,int>{24,28},{48,55}}) {
                    int sharpVariation=0,blurVariation=0;
                    for(int y=rows.first;y<rows.second;++y)for(int x=24;x<40;++x) {
                        sharpVariation+=PixelDistance(sharp[y*64+x],sharp[(y-1)*64+x]);
                        sharpVariation+=PixelDistance(sharp[y*64+x],sharp[y*64+x-1]);
                        blurVariation+=PixelDistance(blurred[y*64+x],blurred[(y-1)*64+x]);
                        blurVariation+=PixelDistance(blurred[y*64+x],blurred[y*64+x-1]);
                    }
                    Assert::IsTrue(sharpVariation>50 && blurVariation<sharpVariation*0.8,
                        (L"both background regions must soften; y="+std::to_wstring(rows.first)+L" sharp="+std::to_wstring(sharpVariation)+L" blur="+std::to_wstring(blurVariation)).c_str());
                }
                const int glyphX=(g.content.left+g.content.right)/2;
                const int glyphY=(g.content.top+g.content.bottom)/2+g.destination.top-g.source.top;
                Assert::IsTrue(PixelDistance(sharp[glyphY*64+glyphX],blurred[glyphY*64+glyphX])<=2,L"glyph center stays sharp");
                for(int y=0;y<64;++y)for(int x=0;x<64;++x) {
                    Assert::IsTrue(PixelDistance(sharp[y*64+x],again[y*64+x])<=2,L"zero disables blur on the same hook");
                    const bool inPanel=x>=g.destination.left-6 && x<g.destination.right+6 && y>=g.destination.top && y<g.destination.bottom;
                    const bool inCleanup=x>=g.generatedCleanup.left && x<g.generatedCleanup.right && y>=g.generatedCleanup.top && y<g.generatedCleanup.bottom;
                    if(!inPanel && !inCleanup)
                        Assert::IsTrue(PixelDistance(sharp[y*64+x],blurred[y*64+x])<=2,L"picture outside overlay and cleanup remains unchanged");
                }
                const auto blackSharp=render(0,3),blackBlur=render(30,3);
                for(int i=0;i<64*64;++i)Assert::IsTrue(PixelDistance(blackSharp[i],blackBlur[i])<=2,L"black mode ignores reconstruction blur");
            }
            pl_mpv_user_shader_destroy(&hook);
        }

        TEST_METHOD(GaussianWorkIsLimitedToPaddedSubtitleRegion)
        {
            const SubtitleBoxRect card{1300,1650,2540,1900};
            const auto g=ComputeSubtitleCutPaste({1320,1700,2520,1930},3840,2160,260,1900,
                SubtitleBoxPadding(60,30,20),0,&card);
            Assert::IsTrue(g.valid);
            for(const float radius:{0.5f,3.0f,8.0f,30.0f}) {
                const auto roi=ComputeSubtitleGaussianRegion(g,5,radius);
                Assert::IsTrue(roi.active);
                Assert::IsTrue(roi.left<=g.destination.left-6-radius && roi.top<=g.destination.top-radius);
                Assert::IsTrue(roi.left+roi.width*roi.step>=g.destination.right+6+radius);
                Assert::IsTrue(roi.top+roi.height*roi.step>=card.bottom+radius);
                Assert::IsTrue(roi.width*roi.height<3840.0f*2160.0f*0.15f,L"Gaussian targets must not cover the full 4K frame");
                Assert::AreEqual(radius>=4?2.0f:1.0f,roi.step);
            }
            Assert::IsFalse(ComputeSubtitleGaussianRegion(g,5,0).active);
            Assert::IsFalse(ComputeSubtitleGaussianRegion(g,3,30).active);
            Assert::IsFalse(ComputeSubtitleGaussianRegion({},5,30).active);
        }

        TEST_METHOD(GeneratedBlurSoftensRealOverlayForBarOnlyCaptions)
        {
            TargetLutGpuFixture fixture;Assert::IsTrue(fixture.Create());
            const auto* hook=CreateSubtitleCutPasteHook(fixture.Gpu());Assert::IsNotNull(hook);
            const SubtitleBoxRect noCard{};
            for(const bool top:{false,true}) {
                const auto g=ComputeSubtitleCutPaste(top?SubtitleBoxRect{12,0,52,13}:SubtitleBoxRect{12,50,52,63},
                    64,64,top?16:8,top?56:48,SubtitleBoxPadding(6,6,1),0,&noCard);
                Assert::IsTrue(g.valid);Assert::IsFalse(g.generatedCleanup.Valid());
                SubtitleGeneratedGrayStyle style;style.opacity=0;style.maxLuminance=1;
                auto render=[&](float radius) {
                    style.blurPixels=radius;
                    Assert::IsTrue(BindSubtitleCutPasteHook(hook,g,5,PL_COLOR_TRC_SRGB,style));
                    return fixture.RenderSubtitlePattern(hook,false,0,g,PL_COLOR_TRC_SRGB,true,false,100,false,0,
                        nullptr,false,false,nullptr,nullptr,false,-1,false,true);
                };
                const auto sharp=render(0),blurred=render(3);
                int before=0,after=0;
                for(int y=top?22:31;y<(top?26:35);++y)for(int x=16;x<25;++x) {
                    before+=PixelDistance(sharp[y*64+x],sharp[y*64+x-1]);
                    before+=PixelDistance(sharp[y*64+x],sharp[(y-1)*64+x]);
                    after+=PixelDistance(blurred[y*64+x],blurred[y*64+x-1]);
                    after+=PixelDistance(blurred[y*64+x],blurred[(y-1)*64+x]);
                }
                Assert::IsTrue(before>50 && after<before*0.8,(L"real overlay must blur; top="+std::to_wstring(top)+L" sharp="+std::to_wstring(before)+L" blur="+std::to_wstring(after)).c_str());
                // Radius 8 used to land on the identical phase of this 8-pixel
                // checkerboard, leaving full contrast: the sparse-grid defect.
                for(const float radius:{8.0f,30.0f}) {
                    const auto wide=render(radius);int variation=0;
                    for(int y=top?22:31;y<(top?26:35);++y)for(int x=16;x<25;++x) {
                        variation+=PixelDistance(wide[y*64+x],wide[y*64+x-1]);
                        variation+=PixelDistance(wide[y*64+x],wide[(y-1)*64+x]);
                    }
                    Assert::IsTrue(variation<before*0.20,
                        (L"wide Gaussian must remove grid resonance: radius="+std::to_wstring(radius)+L" sharp="+std::to_wstring(before)+L" wide="+std::to_wstring(variation)).c_str());
                }
            }
            pl_mpv_user_shader_destroy(&hook);
        }

        TEST_METHOD(GeneratedBlurClampsToCleanPictureForTopAndBottomCards)
        {
            TargetLutGpuFixture fixture;Assert::IsTrue(fixture.Create());
            const auto* hook=CreateSubtitleCutPasteHook(fixture.Gpu());Assert::IsNotNull(hook);
            for(const bool top:{false,true}) {
                const auto g=ComputeSubtitleCutPaste(top?SubtitleBoxRect{12,7,52,24}:SubtitleBoxRect{12,40,52,57},
                    64,64,8,56,SubtitleBoxPadding(6,8,8));
                const SubtitleBoxRect card=g.generatedCleanup;
                SubtitleGeneratedGrayStyle style;style.opacity=0.0f;style.maxLuminance=1.0f;style.blurPixels=30;
                Assert::IsTrue(BindSubtitleCutPasteHook(hook,g,5,PL_COLOR_TRC_SRGB,style));
                auto render=[&](int gray) {return fixture.RenderSubtitlePattern(hook,false,0,g,PL_COLOR_TRC_SRGB,
                    true,true,150,false,gray,&card,false,false,nullptr,nullptr,false,-1,false);};
                const auto black=render(0),colored=render(80);
                // All taps must remain outside the card even at maximum radius.
                // Compare central cleanup pixels outside the moved glyph column.
                for(int y=card.top;y<card.bottom;++y)for(const int x:{24,39}) {
                    const int sourceY=y+g.source.top-g.destination.top;
                    // An intentionally gray source card keys as ink in the
                    // glyph envelope. Probe the clean, bar-facing margin only.
                    if(sourceY>=g.content.top && sourceY<g.content.bottom)continue;
                    Assert::IsTrue(PixelDistance(black[y*64+x],colored[y*64+x])<=3,L"old backing must not leak into blur taps");
                    Assert::IsTrue(black[y*64+x].r>125,L"black physical bars must not bleed into the reconstructed picture");
                }
            }
            pl_mpv_user_shader_destroy(&hook);
        }

		TEST_METHOD(BlackPanelCoversLastPictureRowWithSquareLowerCorners)
		{
			TargetLutGpuFixture fixture;Assert::IsTrue(fixture.Create());
			const auto* hook=CreateSubtitleCutPasteHook(fixture.Gpu());Assert::IsNotNull(hook);
			const auto geometry=ComputeSubtitleCutPaste({20,46,44,61},64,64,8,56,
				SubtitleBoxPadding(10,10,10));
			Assert::IsTrue(geometry.valid);
			Assert::IsTrue(BindSubtitleCutPasteHook(hook,geometry,3,PL_COLOR_TRC_SRGB));
			const auto baseline=fixture.RenderSubtitlePattern(nullptr,false,0,geometry,
				PL_COLOR_TRC_SRGB,false,false,230);
			const auto moved=fixture.RenderSubtitlePattern(hook,false,0,geometry,
				PL_COLOR_TRC_SRGB,false,false,230);
			const int lastPictureY=geometry.pictureBottom-1;
			for(const int x:{geometry.destination.left+2,geometry.destination.right-3}) {
				const int pixel=lastPictureY*64+x;
				Assert::IsTrue(baseline[pixel].r>80,
					L"the synthetic source row must expose a measurable lower-corner seam");
				Assert::IsTrue(moved[pixel].r<12,
					L"black backing reaches the last picture row and has square lower corners");
			}
			const int firstBarPixel=geometry.pictureBottom*64+
				geometry.destination.left+2;
			Assert::IsTrue(PixelDistance(moved[firstBarPixel],baseline[firstBarPixel])<=6,
				L"the panel ends exactly at the picture/bar boundary");
			pl_mpv_user_shader_destroy(&hook);
		}

		TEST_METHOD(LedBoostToeAndShadowLift)
		{
			// Delta 11. The boost toe: knee 0 is today's boost; knee 0.10 is the
			// identity below the knee, today's boost above knee + width,
			// monotonic; below black untouched; the gain path unchanged. The
			// ANSI shadow lift: none without a veil; with veil 1 and strength
			// 0.3 the darks rise by 0.3 x 0.1 x 1.6875 x u(1 - u)^2.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			std::string log;
			fixture.CaptureLog(&log);
			CustomHookChain::Chain chain(SigmaLoader(fixture));
			ConfiguredShaderRule rule;
			rule.name = "led_compensation";
			rule.filename = "LED compensation.glsl";
			chain.Configure({ rule });
			CustomHookChain::TemplateValues values;
			values.outputWidth = values.outputHeight = 2;
			chain.Ensure(values);
			std::vector<const pl_hook*> hooks;
			chain.Collect(false, hooks);
			Assert::AreEqual(size_t{ 1 }, hooks.size(), L"libplacebo rejected LED compensation.glsl");
			const pl_hook* hook = hooks[0];
			const double boost = 2.2 / 2.4;
			auto set = [&](double gain, double knee, double lift, double veil)
			{
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "boost", boost));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "gain", gain));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "gain_domain", 1.0));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "knee", 0.08));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "compensation_eotf", 2.2));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "boost_knee", knee));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "boost_knee_width", 0.05));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "shadow_lift", lift));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "veil", veil));
			};
			// 7.360 drops the output encoding after a PRE_OUTPUT hook; the
			// renderer then puts VP's repair first, as here.
			OutputEncodingRepair::Hook repair;
			auto code = [&](double v, enum pl_color_levels levels = PL_COLOR_LEVELS_FULL)
			{
				pl_color_repr target{};
				target.sys = PL_COLOR_SYSTEM_RGB;
				target.levels = levels;
				target.bits.sample_depth = 10;
				target.bits.color_depth = 10;
				repair.SetTarget(target);
				std::vector<const pl_hook*> list = { hook };
				if (OutputEncodingRepair::Needed(list))
					list.insert(list.begin(), repair.Get());
				return fixture.RenderTenBitSequence(list, levels, static_cast<float>(v), 1, nullptr, 10)[0];
			};
			std::wstring problems;
			// Knee 0 = today's global boost.
			set(1.0, 0.0, 0.0, 0.0);
			for (double v : { 0.03, 0.12, 0.5, 0.9 })
				if (std::abs(code(v) - std::lround(1023.0 * std::pow(v, boost))) > 1)
					problems += L"knee 0 at " + std::to_wstring(v) + L"; ";
			// Knee 0.10: identity below, the boost above 0.15, monotonic.
			std::vector<int> toe;
			set(1.0, 0.10, 0.0, 0.0);
			for (int i = 0; i <= 200; ++i)
				toe.push_back(code(i * 0.005));
			for (int i = 0; i <= 200; ++i)
			{
				const double v = i * 0.005;
				if (v < 0.0999 && std::abs(toe[i] - std::lround(1023.0 * v)) > 1)
					problems += L"toe not identity at " + std::to_wstring(v) + L"; ";
				if (v > 0.1501 && std::abs(toe[i] - std::lround(1023.0 * std::pow(v, boost))) > 1)
					problems += L"toe not the boost at " + std::to_wstring(v) + L"; ";
				if (i > 0 && toe[i] < toe[i - 1])
					problems += L"toe not monotonic at " + std::to_wstring(v) + L"; ";
			}
			// Below black untouched (limited range code 40).
			const int btb = code((40.0 - 64.0) / 876.0, PL_COLOR_LEVELS_LIMITED);
			if (btb != 40)
				problems += L"code 40 became " + std::to_wstring(btb) + L"; ";
			// The gain path unchanged with the toe on (above the toe).
			set(2.0, 0.0, 0.0, 0.0);
			const int gainPlain = code(0.4);
			set(2.0, 0.10, 0.0, 0.0);
			if (code(0.4) != gainPlain)
				problems += L"the gain changed with the toe on; ";
			// Shadow lift.
			set(1.0, 0.10, 0.3, 0.0);
			if (code(0.0333) != toe[7] && std::abs(code(0.0333) - std::lround(1023.0 * 0.0333)) > 1)
				problems += L"a lift without a veil; ";
			set(1.0, 0.10, 0.3, 1.0);
			const double u = 0.0333 / 0.10;
			const double expectedLift = 0.3 * 0.10 * 1.6875 * u * (1.0 - u) * (1.0 - u);
			const int lifted = code(0.0333);
			if (std::abs(lifted - 1023.0 * (0.0333 + expectedLift)) > 1.5)
				problems += L"lift at 0.0333: " + std::to_wstring(lifted) + L" for " + std::to_wstring(1023.0 * (0.0333 + expectedLift)) + L"; ";
			if (code(0.0) != 0 || std::abs(code(0.5) - std::lround(1023.0 * std::pow(0.5, boost))) > 1)
				problems += L"the lift reached black or the mid-tones; ";
			fixture.CaptureLog(nullptr);
			Assert::IsTrue(problems.empty(), (problems + L"log: " + std::wstring(log.begin(), log.end())).c_str());
		}

		TEST_METHOD(LedHighlightStatisticIgnoresGrain)
		{
			// Delta 8 R2: with the grain map (g = 1 everywhere), grain on a flat
			// field no longer raises the highlight percentile, a real highlight
			// patch in the same grain still does, and the APL and the block
			// maximum (script mode, SF-4) are exactly as without the map.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			const int size = 192;
			auto grainOf = [](int x, int y)
			{
				const unsigned h = (static_cast<unsigned>(x) * 73856093u) ^ (static_cast<unsigned>(y) * 19349663u);
				return static_cast<int>((h * 2654435761u >> 24) % 61u) - 30;
			};
			auto flat = [&](int, int x, int y) { return static_cast<double>(100 + grainOf(x, y)); };
			auto patch = [&](int, int x, int y)
			{
				const bool in = x >= 60 && x < 132 && y >= 60 && y < 132;
				return static_cast<double>(std::min(255, (in ? 225 : 100) + grainOf(x, y)));
			};
			const float texel[4] = { 0.4f, 1.0f, 0.0f, 1.0f };   // r = the floor: g = 1
			pl_tex_params mp{};
			mp.w = mp.h = 1;
			mp.format = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32, PL_FMT_CAP_SAMPLEABLE);
			mp.sampleable = true;
			mp.initial_data = texel;
			pl_tex map = mp.format ? pl_tex_create(gpu, &mp) : nullptr;
			Assert::IsNotNull(map);
			const double weights[3] = { 0.299, 0.587, 0.114 };
			const pl_rect2df crop{ 0.0f, 0.0f, static_cast<float>(size), static_cast<float>(size) };
			auto measure = [&](auto code, bool withGrain)
			{
				LedDrive::MeasurementHook hook;
				hook.SetFrame({}, weights);
				hook.SetGrain(withGrain ? map : nullptr, 0.4f, crop);
				fixture.RenderSequence(std::vector<const pl_hook*>{ hook.Hook() }, size, size, 1, code);
				std::vector<LedDrive::RawMeasurement> measured;
				Assert::AreEqual(1, hook.Collect(measured, 5000), L"No measurement came back");
				hook.Release();
				return measured.front();
			};
			auto p99 = [](const LedDrive::RawMeasurement& r)
			{
				return LedDrive::HistogramPercentile(r.histogram, r.blocks, 99.0);
			};
			const LedDrive::RawMeasurement flatOff = measure(flat, false), flatOn = measure(flat, true);
			const LedDrive::RawMeasurement patchOff = measure(patch, false), patchOn = measure(patch, true);
			pl_tex_destroy(gpu, &map);
			const std::wstring message = L"flat grain p99 " + std::to_wstring(p99(flatOff)) + L" -> " +
				std::to_wstring(p99(flatOn)) + L"; highlight patch p99 " + std::to_wstring(p99(patchOff)) + L" -> " +
				std::to_wstring(p99(patchOn)) + L"; log: " + std::wstring(log.begin(), log.end());
			Assert::IsTrue(p99(flatOn) < p99(flatOff) - 5.0, message.c_str());
			// The highlight survives at its own level (8-bit 225 = limited code
			// 209.2); the plain statistic's extra was the grain riding on it.
			Assert::IsTrue(std::fabs(p99(patchOn) - (16.0 + 219.0 * 225.0 / 255.0)) <= 3.0 &&
				p99(patchOn) > p99(flatOn) + 50.0, message.c_str());
			for (const auto& pair : { std::make_pair(&flatOff, &flatOn), std::make_pair(&patchOff, &patchOn) })
			{
				Assert::AreEqual(pair.first->sum, pair.second->sum, (L"the APL must not change; " + message).c_str());
				Assert::AreEqual(pair.first->maximum, pair.second->maximum, (L"the block maximum must not change; " + message).c_str());
				Assert::AreEqual(pair.first->blocks, pair.second->blocks);
			}
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLogExceptPreOutputBug(log);
		}

		TEST_METHOD(LedMeasurementHookMeasuresWithoutTouchingThePicture)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(),
				L"Could not create the libplacebo WARP test device");
			// Every warning or error libplacebo logs fails the test (design review 4, N-6).
			std::string cleanLog;
			fixture.CaptureLog(&cleanLog);
			const std::vector<RgbaPixel> baseline = fixture.RenderCoordinateField(nullptr);
			LedDrive::MeasurementHook hook;
			const double weights[3] = { 0.299, 0.587, 0.114 };
			hook.SetFrame({}, weights);
			std::string libplaceboLog;
			fixture.CaptureLog(&libplaceboLog);
			const std::vector<RgbaPixel> pixels = fixture.RenderCoordinateField(hook.Hook());
			fixture.CaptureLog(nullptr);
			cleanLog += libplaceboLog;
			fixture.CaptureLog(&cleanLog);
			for (size_t i = 0; i < pixels.size(); ++i)
				Assert::IsTrue(pixels[i].r == baseline[i].r && pixels[i].g == baseline[i].g &&
					pixels[i].b == baseline[i].b, L"The measurement must never change the picture");
			std::vector<LedDrive::RawMeasurement> measured;
			const std::wstring failure = L"No measurement came back: " +
				std::wstring(libplaceboLog.begin(), libplaceboLog.end());
			Assert::AreEqual(1, hook.Collect(measured, 5000), failure.c_str());
			const LedDrive::RawMeasurement& raw = measured.front();

			// The same blocks measured on the CPU from the rendered pixels.
			const int blocks = 64 / 3;
			const double step = 64.0 / blocks;
			double expected = 0.0, brightest = 0.0;
			for (int by = 0; by < blocks; ++by)
				for (int bx = 0; bx < blocks; ++bx)
				{
					double acc = 0.0;
					for (int dy = 0; dy < 3; ++dy)
						for (int dx = 0; dx < 3; ++dx)
						{
							const int x = std::min(63, static_cast<int>(bx * step) + dx);
							const int y = std::min(63, static_cast<int>(by * step) + dy);
							const RgbaPixel& p = baseline[y * 64 + x];
							acc += (weights[0] * p.r + weights[1] * p.g + weights[2] * p.b) / 255.0;
						}
					expected += 16.0 + 219.0 * acc / 9.0;
					brightest = std::max(brightest, 16.0 + 219.0 * acc / 9.0);
				}
			expected /= blocks * blocks;
			Assert::AreEqual(static_cast<uint32_t>(blocks * blocks), raw.blocks);
			uint32_t histogramTotal = 0, cellTotal = 0;
			for (uint32_t count : raw.histogram) histogramTotal += count;
			for (uint32_t count : raw.cellCount) cellTotal += count;
			Assert::AreEqual(raw.blocks, histogramTotal);
			Assert::AreEqual(raw.blocks, cellTotal);
			const double apl = static_cast<double>(raw.sum) / LedDrive::SUM_SCALE / raw.blocks;
			Assert::AreEqual(expected, apl, 0.75);
			Assert::AreEqual(brightest, static_cast<double>(raw.maximum) / LedDrive::SUM_SCALE, 0.75,
				L"The block maximum comes back in the same readback buffer");

			// Measuring only the left half sees less red.
			hook.SetFrame({ 0.0, 0.0, 0.5, 1.0 }, weights);
			fixture.RenderCoordinateField(hook.Hook());
			measured.clear();
			Assert::AreEqual(1, hook.Collect(measured, 5000));
			const double left = static_cast<double>(measured.front().sum) / LedDrive::SUM_SCALE /
				measured.front().blocks;
			Assert::IsTrue(left < apl - 5.0);

			// The statistics describe the gamma-boosted picture.
			hook.SetFrame({}, weights, 2.2 / 2.4);
			fixture.RenderCoordinateField(hook.Hook());
			measured.clear();
			Assert::AreEqual(1, hook.Collect(measured, 5000));
			const double boostedApl = static_cast<double>(measured.front().sum) / LedDrive::SUM_SCALE /
				measured.front().blocks;
			Assert::IsTrue(boostedApl > apl + 2.0, L"The boost lifts the measured APL");
			hook.Release();
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLogExceptPreOutputBug(cleanLog);
		}

		TEST_METHOD(LedRawTransferMatchesTheRenderedOutput)
		{
			// The lookahead's CPU transfer against libplacebo rendering the same
			// grey levels with the same colour spaces and colour mapping.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(),
				L"Could not create the libplacebo WARP test device");
			// Every warning or error libplacebo logs fails the test (design review 4, N-6).
			std::string cleanLog;
			fixture.CaptureLog(&cleanLog);
			pl_render_params params = pl_render_default_params;
			params.dither_params = nullptr;
			params.error_diffusion = nullptr;
			params.deband_params = nullptr;
			params.peak_detect_params = nullptr;
			struct Case { pl_color_transfer source; float sourceNits; pl_color_transfer target; float targetNits; };
			const Case cases[] = {
				{ PL_COLOR_TRC_BT_1886, 0.0f, PL_COLOR_TRC_GAMMA22, 0.0f },
				{ PL_COLOR_TRC_BT_1886, 0.0f, PL_COLOR_TRC_GAMMA24, 0.0f },
				{ PL_COLOR_TRC_PQ, 1000.0f, PL_COLOR_TRC_GAMMA22, 100.0f },
				{ PL_COLOR_TRC_PQ, 4000.0f, PL_COLOR_TRC_GAMMA24, 48.0f } };
			std::wstring failures;
			for (const Case& c : cases)
			{
				pl_color_space source{}, target{};
				source.primaries = target.primaries = PL_COLOR_PRIM_BT_709;
				source.transfer = c.source;
				target.transfer = c.target;
				source.hdr.max_luma = source.hdr.max_cll = source.hdr.max_fall = c.sourceNits;
				if (c.sourceNits > 0.0f) source.hdr.min_luma = PL_COLOR_HDR_BLACK;
				target.hdr.max_luma = c.targetNits;
				if (c.targetNits > 0.0f) target.hdr.min_luma = PL_COLOR_HDR_BLACK;
				const LedDrive::RawTransfer transfer =
					LedDrive::BuildRawTransfer(source, target, params.color_map_params);
				double worst = 0.0;
				int worstLevel = 0;
				for (int level = 16; level <= 240; level += 16)
				{
					const uint8_t v = static_cast<uint8_t>(level);
					const RgbaPixel out = fixture.Render(nullptr, params, PL_COLOR_LEVELS_FULL,
						PL_LUT_NORMALIZED, { v, v, v, 255 }, c.source, c.target,
						PL_COLOR_PRIM_BT_709, PL_COLOR_PRIM_BT_709, c.sourceNits, c.targetNits);
					const double predicted = transfer[std::lround(level / 255.0 * (LedDrive::RAW_TRANSFER_SIZE - 1))];
					const double difference = std::abs(out.r / 255.0 - predicted);
					if (difference > worst) { worst = difference; worstLevel = level; }
				}
				const std::wstring evidence = L"transfer " + std::to_wstring(static_cast<int>(c.source)) +
					L"->" + std::to_wstring(static_cast<int>(c.target)) + L" worst " +
					std::to_wstring(worst * 255.0) + L" codes at " + std::to_wstring(worstLevel);
				Logger::WriteMessage(evidence.c_str());
				if (worst > 3.0 / 255.0) failures += evidence + L"; ";
			}
			Assert::IsTrue(failures.empty(), failures.c_str());
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(cleanLog);
		}

		TEST_METHOD(LedCompensationShaderAppliesGainWithASoftKnee)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(),
				L"Could not create the libplacebo WARP test device");
			// Every warning or error libplacebo logs fails the test (design review 4, N-6).
			std::string cleanLog;
			fixture.CaptureLog(&cleanLog);
			const std::vector<RgbaPixel> baseline = fixture.RenderCoordinateField(nullptr);
			CustomHookChain::Chain chain({
				[](const std::string& file, std::string& source, std::string& reason)
				{
					source = LoadBundledShader(file.c_str());
					reason = "bundled shader not found";
					return !source.empty();
				},
				[&fixture](const std::string& source) -> const pl_hook*
				{
					return pl_mpv_user_shader_parse(fixture.Gpu(), source.data(), source.size());
				},
				[](const pl_hook*& hook) { pl_mpv_user_shader_destroy(&hook); } });
			ConfiguredShaderRule rule;
			rule.name = "led_compensation";
			rule.filename = "LED compensation.glsl";
			chain.Configure({ rule });
			CustomHookChain::TemplateValues values;
			values.outputWidth = values.outputHeight = 64;
			chain.Ensure(values);
			std::vector<const pl_hook*> hooks;
			chain.Collect(false, hooks);
			Assert::AreEqual(size_t{ 1 }, hooks.size(), L"libplacebo rejected LED compensation.glsl");
			Assert::IsTrue((hooks[0]->stages & PL_HOOK_PRE_OUTPUT) != 0);
			const pl_hook* hook = hooks[0];
			auto set = [hook](double gain, double domain, double boost = 1.0)
			{
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "boost", boost));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "gain", gain));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "gain_domain", domain));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "knee", 0.08));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "compensation_eotf", 2.2));
			};

			set(1.0, 0.0);
			const auto unity = fixture.RenderCoordinateField(hook);
			for (size_t i = 0; i < unity.size(); ++i)
				Assert::AreEqual(static_cast<int>(baseline[i].r), static_cast<int>(unity[i].r));

			const size_t quarter = 16;           // r = 16/63 of full scale
			const size_t bright = 62;            // r close to white
			set(1.25, 0.0);
			const auto encoded = fixture.RenderCoordinateField(hook);
			Assert::AreEqual(baseline[quarter].r * 1.25, static_cast<double>(encoded[quarter].r), 1.5);
			Assert::IsTrue(encoded[bright].r >= baseline[bright].r && encoded[bright].r <= 255);
			for (size_t x = 1; x < 64; ++x)
				Assert::IsTrue(encoded[x].r >= encoded[x - 1].r, L"The knee keeps the ramp monotonic");

			set(2.0, 1.0);
			const auto linear = fixture.RenderCoordinateField(hook);
			const double expected = 255.0 * std::pow(2.0 * std::pow(baseline[quarter].r / 255.0, 2.2), 1.0 / 2.2);
			Assert::AreEqual(expected, static_cast<double>(linear[quarter].r), 2.0);

			// Gamma boost alone (gain 1): the old output_gamma 2.4 on film 2.2 lift.
			set(1.0, 0.0, 2.2 / 2.4);
			const auto boosted = fixture.RenderCoordinateField(hook);
			for (size_t x : { size_t{ 8 }, quarter, size_t{ 32 }, size_t{ 48 } })
				Assert::AreEqual(255.0 * std::pow(baseline[x].r / 255.0, 2.2 / 2.4),
					static_cast<double>(boosted[x].r), 1.5);
			Assert::AreEqual(static_cast<int>(baseline[63].r), static_cast<int>(boosted[63].r),
				L"White stays white");
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLogExceptPreOutputBug(cleanLog);
		}

		// libplacebo 7.360.1: after a texture-input hook at PRE_OUTPUT, the
		// renderer applies the output encoding (range, alpha) to the shader it
		// held before the hook, which the hook has already finished
		// ("Attempted to modify an immutable shader!"), so the picture is drawn
		// unencoded. VP's limited-range output then shows black at 0 and white
		// at 255. OutputEncodingRepair re-applies the encoding at OUTPUT.
		static RgbaPixel RenderWithHooks(TargetLutGpuFixture& fixture, std::vector<const pl_hook*> hooks,
			enum pl_color_levels levels, RgbaPixel source, std::string& log)
		{
			pl_render_params params = pl_render_fast_params;
			params.hooks = hooks.empty() ? nullptr : hooks.data();
			params.num_hooks = static_cast<int>(hooks.size());
			std::string captured;
			fixture.CaptureLog(&captured);
			const RgbaPixel out = fixture.Render(nullptr, params, levels, PL_LUT_NORMALIZED, source);
			fixture.CaptureLog(nullptr);
			log += captured;
			return out;
		}

		TEST_METHOD(PreOutputHooksKeepTheOutputEncodingWithTheRepair)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			LedDrive::MeasurementHook measurement;
			const double weights[3] = { 0.299, 0.587, 0.114 };
			measurement.SetFrame({}, weights);
			OutputEncodingRepair::Hook repair;
			std::string allLog;   // every render's libplacebo log (clean apart from the known message)
			for (const enum pl_color_levels levels : { PL_COLOR_LEVELS_LIMITED, PL_COLOR_LEVELS_FULL })
			{
				pl_color_repr targetRepr{};
				targetRepr.sys = PL_COLOR_SYSTEM_RGB;
				targetRepr.levels = levels;
				repair.SetTarget(targetRepr);
				for (const RgbaPixel source : { RgbaPixel{ 255, 0, 0, 255 }, RgbaPixel{ 128, 64, 200, 255 } })
				{
					std::string& log = allLog;
					const RgbaPixel plain = RenderWithHooks(fixture, {}, levels, source, log);
					std::vector<const pl_hook*> hooks = { measurement.Hook() };
					Assert::IsTrue(OutputEncodingRepair::Needed(hooks));
					hooks.insert(hooks.begin(), repair.Get());
					const RgbaPixel repaired = RenderWithHooks(fixture, hooks, levels, source, log);
					const std::wstring label = (levels == PL_COLOR_LEVELS_LIMITED ? L"limited" : L"full") +
						std::wstring(L": plain ") + std::to_wstring(plain.r) + L"/" + std::to_wstring(plain.g) +
						L"/" + std::to_wstring(plain.b) + L", with the repair " + std::to_wstring(repaired.r) + L"/" +
						std::to_wstring(repaired.g) + L"/" + std::to_wstring(repaired.b);
					Assert::IsTrue(std::abs(plain.r - repaired.r) <= 1 && std::abs(plain.g - repaired.g) <= 1 &&
						std::abs(plain.b - repaired.b) <= 1, label.c_str());
				}
			}
			Assert::IsTrue(repair.Applied() > 0, L"The repair hook never ran");
			AssertCleanLibplaceboLogExceptPreOutputBug(allLog);

			// Without the repair libplacebo 7.360.1 still drops the encoding. If
			// this ever fails, libplacebo has fixed the bug: remove the repair,
			// or it will encode twice.
			pl_color_repr limited{};
			limited.sys = PL_COLOR_SYSTEM_RGB;
			limited.levels = PL_COLOR_LEVELS_LIMITED;
			std::string log;
			const RgbaPixel plain = RenderWithHooks(fixture, {}, PL_COLOR_LEVELS_LIMITED, { 255, 0, 0, 255 }, log);
			const RgbaPixel broken = RenderWithHooks(fixture, { measurement.Hook() }, PL_COLOR_LEVELS_LIMITED,
				{ 255, 0, 0, 255 }, log);
			Assert::AreEqual(235, static_cast<int>(plain.r));
			Assert::AreEqual(255, static_cast<int>(broken.r),
				L"libplacebo no longer drops the encoding after a PRE_OUTPUT hook: remove OutputEncodingRepair");
			measurement.Release();

			// The LED compensation shader (an mpv PRE_OUTPUT hook, active whenever
			// the gamma boost is on, LED drive or not) with neutral settings:
			// limited-range output must come out as without it.
			CustomHookChain::Chain chain({
				[](const std::string& file, std::string& source, std::string& reason)
				{
					source = LoadBundledShader(file.c_str());
					reason = "bundled shader not found";
					return !source.empty();
				},
				[&fixture](const std::string& source) -> const pl_hook*
				{
					return pl_mpv_user_shader_parse(fixture.Gpu(), source.data(), source.size());
				},
				[](const pl_hook*& hook) { pl_mpv_user_shader_destroy(&hook); } });
			ConfiguredShaderRule rule;
			rule.name = "led_compensation";
			rule.filename = "LED compensation.glsl";
			chain.Configure({ rule });
			CustomHookChain::TemplateValues values;
			values.outputWidth = values.outputHeight = 64;
			chain.Ensure(values);
			std::vector<const pl_hook*> compensation;
			chain.Collect(false, compensation);
			Assert::AreEqual(size_t{ 1 }, compensation.size(), L"libplacebo rejected LED compensation.glsl");
			for (const char* name : { "gain", "boost" })
				Assert::IsTrue(CustomHookChain::SetHookParameter(compensation[0], name, 1.0));
			Assert::IsTrue(CustomHookChain::SetHookParameter(compensation[0], "gain_domain", 0.0));
			Assert::IsTrue(CustomHookChain::SetHookParameter(compensation[0], "knee", 0.08));
			Assert::IsTrue(CustomHookChain::SetHookParameter(compensation[0], "compensation_eotf", 2.2));
			repair.SetTarget(limited);
			for (const RgbaPixel source : { RgbaPixel{ 255, 0, 0, 255 }, RgbaPixel{ 128, 64, 200, 255 } })
			{
				const RgbaPixel reference = RenderWithHooks(fixture, {}, PL_COLOR_LEVELS_LIMITED, source, log);
				const RgbaPixel repaired = RenderWithHooks(fixture, { repair.Get(), compensation[0] },
					PL_COLOR_LEVELS_LIMITED, source, log);
				Assert::IsTrue(std::abs(reference.r - repaired.r) <= 1 && std::abs(reference.g - repaired.g) <= 1 &&
					std::abs(reference.b - repaired.b) <= 1,
					(L"LED compensation, limited: plain " + std::to_wstring(reference.r) + L"/" +
						std::to_wstring(reference.g) + L"/" + std::to_wstring(reference.b) + L", with the repair " +
						std::to_wstring(repaired.r) + L"/" + std::to_wstring(repaired.g) + L"/" +
						std::to_wstring(repaired.b)).c_str());
			}

			// Domain contract (planner, 2026-10-10): the sigma-delta assumes the
			// OUTPUT stage is already encoded (limited = code/256). With the LED
			// hook, the repair first and the quantizer enabled, white must reach
			// the target as 235 (limited) or 255 (full), black as 16 or 0.
			for (const enum pl_color_levels levels : { PL_COLOR_LEVELS_LIMITED, PL_COLOR_LEVELS_FULL })
			{
				const bool isLimited = levels == PL_COLOR_LEVELS_LIMITED;
				CustomHookChain::Chain sigmaChain({
					[](const std::string& file, std::string& source, std::string& reason)
					{
						source = LoadBundledShader(file.c_str());
						reason = "bundled shader not found";
						return !source.empty();
					},
					[&fixture](const std::string& source) -> const pl_hook*
					{
						return pl_mpv_user_shader_parse(fixture.Gpu(), source.data(), source.size());
					},
					[](const pl_hook*& hook) { pl_mpv_user_shader_destroy(&hook); } });
				ConfiguredShaderRule sigma;
				sigma.name = "sigma_delta";
				sigma.filename = "Temporal sigma-delta.glsl";
				sigma.parameters = { { "enabled", "1" }, { "levels_full", isLimited ? "0" : "1" },
					{ "swapchain_bits", "8" }, { "noise_amplitude", "0" } };
				sigmaChain.Configure({ sigma });
				CustomHookChain::TemplateValues sigmaValues;
				sigmaValues.outputWidth = sigmaValues.outputHeight = 2;
				sigmaChain.Ensure(sigmaValues);
				std::vector<const pl_hook*> sigmaHooks;
				sigmaChain.Collect(false, sigmaHooks);
				Assert::AreEqual(size_t{ 1 }, sigmaHooks.size(), L"libplacebo rejected the sigma-delta");
				pl_color_repr repr{};
				repr.sys = PL_COLOR_SYSTEM_RGB;
				repr.levels = levels;
				repair.SetTarget(repr);
				LedDrive::MeasurementHook led;
				led.SetFrame({}, weights);
				std::vector<const pl_hook*> chainHooks = { led.Hook(), sigmaHooks[0] };
				Assert::IsTrue(OutputEncodingRepair::Needed(chainHooks));
				chainHooks.insert(chainHooks.begin(), repair.Get());   // as the renderer does
				const RgbaPixel white = RenderWithHooks(fixture, chainHooks, levels, { 255, 255, 255, 255 }, log);
				const RgbaPixel black = RenderWithHooks(fixture, chainHooks, levels, { 0, 0, 0, 255 }, log);
				led.Release();
				Assert::AreEqual(isLimited ? 235 : 255, static_cast<int>(white.g),
					isLimited ? L"Limited white with LED hook, repair and sigma-delta" :
						L"Full white with LED hook, repair and sigma-delta");
				Assert::AreEqual(isLimited ? 16 : 0, static_cast<int>(black.g),
					isLimited ? L"Limited black with LED hook, repair and sigma-delta" :
						L"Full black with LED hook, repair and sigma-delta");
			}

			// No PRE_OUTPUT texture hook, no repair (an OUTPUT-stage hook alone
			// does not trigger the bug).
			Assert::IsFalse(OutputEncodingRepair::Needed({ repair.Get() }));
			// A hook libplacebo has disabled is skipped before the shader is
			// finished, so it does not trigger the bug: no repair (it would
			// encode twice). Nor with a CONVERSION target LUT.
			LedDrive::MeasurementHook disabledHook;
			const uint64_t signatures[1] = { disabledHook.Hook()->signature };
			pl_render_errors errors{};
			errors.errors = PL_RENDER_ERR_HOOKS;
			errors.disabled_hooks = signatures;
			errors.num_disabled_hooks = 1;
			Assert::IsTrue(OutputEncodingRepair::Needed({ disabledHook.Hook() }));
			Assert::IsFalse(OutputEncodingRepair::Needed({ disabledHook.Hook() }, &errors),
				L"A disabled PRE_OUTPUT hook must not bring the repair");
			Assert::IsTrue(OutputEncodingRepair::Needed({ disabledHook.Hook(), compensation[0] }, &errors),
				L"Another, active PRE_OUTPUT hook still does");
			Assert::IsFalse(OutputEncodingRepair::Needed({ compensation[0] }, nullptr, PL_LUT_CONVERSION));
		}

		// The cinema path (stage 6a re-audit): limited range on a 10-bit
		// swapchain with colour depth 8. libplacebo hands the OUTPUT stage code
		// k as 4k/1023 (= k/255.75), and the sigma-delta must read it so: with
		// the LED hook, the repair and the quantizer, white stays 940 (235) and
		// black 64 (16) on every refresh, and a 10-bit mid grey averages exactly.
		// Delta 5 (D5-2): with a true 10-bit output (colour depth 10) the
		// sigma-delta quantises to 10-bit codes, in both ranges: white and
		// black exact on every refresh, and a grey whose 10-bit code is not a
		// multiple of 4 held exactly (an 8-bit quantiser would alternate the
		// neighbouring multiples of 4).
		TEST_METHOD(SigmaDeltaKeepsCodesExactOnATrueTenBitOutput)
		{
			for (const bool full : { false, true })
			{
				TargetLutGpuFixture fixture;
				Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
				std::string log;
				fixture.CaptureLog(&log);
				CustomHookChain::Chain chain({
					[](const std::string& file, std::string& source, std::string& reason)
					{
						source = LoadBundledShader(file.c_str());
						reason = "bundled shader not found";
						return !source.empty();
					},
					[&fixture](const std::string& source) -> const pl_hook*
					{
						return pl_mpv_user_shader_parse(fixture.Gpu(), source.data(), source.size());
					},
					[](const pl_hook*& hook) { pl_mpv_user_shader_destroy(&hook); } });
				ConfiguredShaderRule sigma;
				sigma.name = "sigma_delta";
				sigma.filename = "Temporal sigma-delta.glsl";
				sigma.parameters = { { "enabled", "1" }, { "levels_full", full ? "1" : "0" },
					{ "swapchain_bits", "10" }, { "color_bits", "10" }, { "noise_amplitude", "0" } };
				chain.Configure({ sigma });
				CustomHookChain::TemplateValues values;
				values.outputWidth = values.outputHeight = 2;
				chain.Ensure(values);
				std::vector<const pl_hook*> hooks;
				chain.Collect(false, hooks);
				Assert::AreEqual(size_t{ 1 }, hooks.size(), L"libplacebo rejected the sigma-delta");
				const pl_color_levels levels = full ? PL_COLOR_LEVELS_FULL : PL_COLOR_LEVELS_LIMITED;
				auto describe = [](const std::vector<int>& codes)
				{
					std::wstring text;
					for (int code : codes) text += std::to_wstring(code) + L" ";
					return text;
				};
				const int whiteCode = full ? 1023 : 940, blackCode = full ? 0 : 64;
				const int libplaceboWhite = fixture.RenderTenBitSequence({}, levels, 1.0f, 1, nullptr, 10)[0];
				const int libplaceboBlack = fixture.RenderTenBitSequence({}, levels, 0.0f, 1, nullptr, 10)[0];
				for (int code : fixture.RenderTenBitSequence(hooks, levels, 1.0f, 8, nullptr, 10))
					Assert::AreEqual(whiteCode, code, (L"White on a true 10-bit output (libplacebo alone: " +
						std::to_wstring(libplaceboWhite) + L", black " + std::to_wstring(libplaceboBlack) + L")").c_str());
				for (int code : fixture.RenderTenBitSequence(hooks, levels, 0.0f, 8, nullptr, 10))
					Assert::AreEqual(blackCode, code, L"Black on a true 10-bit output");
				// A grey with a 10-bit code that is not a multiple of 4.
				float grey = 0.0f;
				int reference = 0;
				for (int target = 501; target <= 507 && grey == 0.0f; ++target)
				{
					const float candidate = full ? target / 1023.0f : (target - 64) / 876.0f;
					const int code = fixture.RenderTenBitSequence({}, levels, candidate, 1, nullptr, 10)[0];
					if (code % 4 != 0)
					{
						grey = candidate;
						reference = code;
					}
				}
				Assert::IsTrue(grey > 0.0f, L"no reference grey off the 8-bit grid");
				const std::vector<int> codes = fixture.RenderTenBitSequence(hooks, levels, grey, 8, nullptr, 10);
				for (int code : codes)
					Assert::AreEqual(reference, code,
						(L"10-bit grey " + std::to_wstring(reference) + L": " + describe(codes)).c_str());
				fixture.CaptureLog(nullptr);
				AssertCleanLibplaceboLog(log);
			}
		}

		TEST_METHOD(SigmaDeltaKeepsCodesExactOnTheTenBitSwapchain)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			std::string log;
			fixture.CaptureLog(&log);
			CustomHookChain::Chain chain({
				[](const std::string& file, std::string& source, std::string& reason)
				{
					source = LoadBundledShader(file.c_str());
					reason = "bundled shader not found";
					return !source.empty();
				},
				[&fixture](const std::string& source) -> const pl_hook*
				{
					return pl_mpv_user_shader_parse(fixture.Gpu(), source.data(), source.size());
				},
				[](const pl_hook*& hook) { pl_mpv_user_shader_destroy(&hook); } });
			ConfiguredShaderRule sigma;
			sigma.name = "sigma_delta";
			sigma.filename = "Temporal sigma-delta.glsl";
			sigma.parameters = { { "enabled", "1" }, { "levels_full", "0" }, { "swapchain_bits", "10" },
				{ "noise_amplitude", "0" } };
			chain.Configure({ sigma });
			CustomHookChain::TemplateValues values;
			values.outputWidth = values.outputHeight = 2;
			chain.Ensure(values);
			std::vector<const pl_hook*> sigmaHooks;
			chain.Collect(false, sigmaHooks);
			Assert::AreEqual(size_t{ 1 }, sigmaHooks.size(), L"libplacebo rejected the sigma-delta");
			LedDrive::MeasurementHook led;
			const double weights[3] = { 0.299, 0.587, 0.114 };
			led.SetFrame({}, weights);
			OutputEncodingRepair::Hook repair;
			pl_color_repr limited{};
			limited.sys = PL_COLOR_SYSTEM_RGB;
			limited.levels = PL_COLOR_LEVELS_LIMITED;
			limited.bits.sample_depth = 10;
			limited.bits.color_depth = 8;
			limited.bits.bit_shift = 2;
			repair.SetTarget(limited);
			const std::vector<const pl_hook*> hooks = { repair.Get(), led.Hook(), sigmaHooks[0] };
			auto describe = [](const std::vector<int>& codes)
			{
				std::wstring text;
				for (int code : codes) text += std::to_wstring(code) + L" ";
				return text;
			};
			const std::vector<int> white = fixture.RenderTenBitSequence(hooks, PL_COLOR_LEVELS_LIMITED, 1.0f, 8);
			for (int code : white)
				Assert::AreEqual(940, code, (L"White on the 10-bit swapchain: " + describe(white)).c_str());
			const std::vector<int> black = fixture.RenderTenBitSequence(hooks, PL_COLOR_LEVELS_LIMITED, 0.0f, 8);
			for (int code : black)
				Assert::AreEqual(64, code, (L"Black on the 10-bit swapchain: " + describe(black)).c_str());
			// The same grey with no hook is the reference (libplacebo's own
			// 10-bit result); the sigma-delta's 8-bit codes must average it.
			const std::vector<int> reference = fixture.RenderTenBitSequence({}, PL_COLOR_LEVELS_LIMITED, 0.5f, 1);
			const std::vector<int> grey = fixture.RenderTenBitSequence(hooks, PL_COLOR_LEVELS_LIMITED, 0.5f, 8);
			double mean = 0.0;
			for (int code : grey) mean += code / 8.0;
			Assert::AreEqual(static_cast<double>(reference[0]), mean, 0.6,
				(L"Mid grey: reference " + std::to_wstring(reference[0]) + L", sigma-delta " + describe(grey)).c_str());
			led.Release();
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLogExceptPreOutputBug(log);
		}

		// Stage 6c (M1'): the occlusion mask reaches the OUTPUT-stage sigma-delta
		// through the alpha channel. Proves, at VP's settings (10-bit limited
		// target, colour depth 8): writing alpha does not change the picture;
		// mask 1 keeps the carry, mask 0 stops it; the sigma-delta writes alpha
		// back to 1.
		static pl_tex MakeMaskTexture(pl_gpu gpu, int w, int h, const std::vector<uint16_t>& values)
		{
			pl_fmt r16 = pl_find_fmt(gpu, PL_FMT_UNORM, 1, 16, 16, PL_FMT_CAP_SAMPLEABLE);
			Assert::IsNotNull(r16);
			pl_tex_params params{};
			params.w = w;
			params.h = h;
			params.format = r16;
			params.sampleable = true;
			params.initial_data = values.data();
			return pl_tex_create(gpu, &params);
		}

		static CustomHookChain::Loader SigmaLoader(TargetLutGpuFixture& fixture)
		{
			return {
				[](const std::string& file, std::string& source, std::string& reason)
				{
					source = LoadBundledShader(file.c_str());
					reason = "bundled shader not found";
					return !source.empty();
				},
				[&fixture](const std::string& source) -> const pl_hook*
				{
					return pl_mpv_user_shader_parse(fixture.Gpu(), source.data(), source.size());
				},
				[](const pl_hook*& hook) { pl_mpv_user_shader_destroy(&hook); } };
		}

		static void ConfigureSigma(CustomHookChain::Chain& chain, int width, int height,
			std::vector<const pl_hook*>& hooks)
		{
			ConfiguredShaderRule sigma;
			sigma.name = "sigma_delta";
			sigma.filename = "Temporal sigma-delta.glsl";
			sigma.parameters = { { "enabled", "1" }, { "levels_full", "0" }, { "swapchain_bits", "10" },
				{ "noise_amplitude", "0" } };
			chain.Configure({ sigma });
			CustomHookChain::TemplateValues values;
			values.outputWidth = width;
			values.outputHeight = height;
			chain.Ensure(values);
			chain.Collect(false, hooks);
		}

		TEST_METHOD(OcclusionMaskReachesTheSigmaDeltaThroughAlpha)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			std::string log;
			fixture.CaptureLog(&log);
			pl_gpu gpu = fixture.Gpu();
			std::vector<const pl_hook*> sigmaHooks;
			CustomHookChain::Chain chain(SigmaLoader(fixture));
			ConfigureSigma(chain, 2, 2, sigmaHooks);
			Assert::AreEqual(size_t{ 1 }, sigmaHooks.size(), L"libplacebo rejected the sigma-delta");
			pl_tex seen = MakeMaskTexture(gpu, 1, 1, { 65535 });
			pl_tex fresh = MakeMaskTexture(gpu, 1, 1, { 0 });
			OcclusionMask::Hook mask;
			const pl_rect2df whole{ 0.0f, 0.0f, 2.0f, 2.0f };
			auto describe = [](const std::vector<int>& codes)
			{
				std::wstring text;
				for (int code : codes) text += std::to_wstring(code) + L" ";
				return text;
			};

			// 1. The mask hook alone does not change the picture.
			const std::vector<int> plain = fixture.RenderTenBitSequence({}, PL_COLOR_LEVELS_LIMITED, 0.5f, 1);
			mask.SetFrame(fresh, whole, whole);
			const std::vector<int> masked = fixture.RenderTenBitSequence({ mask.Get() }, PL_COLOR_LEVELS_LIMITED, 0.5f, 1);
			Assert::AreEqual(plain[0], masked[0], L"Writing the mask into alpha changed the picture");
			Assert::IsTrue(mask.Applied() > 0, L"The mask hook did not run");

			// 2. Mid grey (8-bit 125.5): the sigma-delta alternates 125/126. Mask 1
			// keeps that carry; mask 0 stops it (one code every refresh). Alpha out is 1.
			Assert::IsTrue(CustomHookChain::SetHookParameter(sigmaHooks[0], "occlusion_mask", 1.0));
			mask.SetFrame(seen, whole, whole);
			std::vector<int> alphas;
			const std::vector<int> carry = fixture.RenderTenBitSequence({ mask.Get(), sigmaHooks[0] },
				PL_COLOR_LEVELS_LIMITED, 0.5f, 8, &alphas);
			Assert::IsTrue(std::find(carry.begin(), carry.end(), 500) != carry.end() &&
				std::find(carry.begin(), carry.end(), 504) != carry.end(),
				(L"Mask 1 must keep the carry (125/126): " + describe(carry)).c_str());
			for (int alpha : alphas)
				Assert::AreEqual(3, alpha, L"The sigma-delta must write alpha back to 1");
			mask.SetFrame(fresh, whole, whole);
			alphas.clear();
			const std::vector<int> stopped = fixture.RenderTenBitSequence({ mask.Get(), sigmaHooks[0] },
				PL_COLOR_LEVELS_LIMITED, 0.5f, 8, &alphas);
			for (int code : stopped)
				Assert::AreEqual(stopped[0], code, (L"Mask 0 must stop the carry (one code every refresh): " +
					describe(stopped)).c_str());
			for (int alpha : alphas)
				Assert::AreEqual(3, alpha, L"The sigma-delta must write alpha back to 1");

			// 3. Without the parameter the sigma-delta ignores alpha (mask 0 present).
			Assert::IsTrue(CustomHookChain::SetHookParameter(sigmaHooks[0], "occlusion_mask", 0.0));
			const std::vector<int> ignored = fixture.RenderTenBitSequence({ mask.Get(), sigmaHooks[0] },
				PL_COLOR_LEVELS_LIMITED, 0.5f, 8);
			Assert::IsTrue(std::find(ignored.begin(), ignored.end(), 500) != ignored.end(),
				(L"occlusion_mask 0 must ignore alpha: " + describe(ignored)).c_str());
			pl_tex_destroy(gpu, &seen);
			pl_tex_destroy(gpu, &fresh);
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(log);
		}

		// Delta 5 (D5-4): the occlusion mask with full-range output. Mid grey
		// is 8-bit 127.5 there (10-bit 508/512): mask 1 keeps the carry, mask 0
		// stops it, and alpha goes back to 1.
		TEST_METHOD(OcclusionMaskWorksWithFullRangeOutput)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			std::string log;
			fixture.CaptureLog(&log);
			pl_gpu gpu = fixture.Gpu();
			std::vector<const pl_hook*> sigmaHooks;
			CustomHookChain::Chain chain(SigmaLoader(fixture));
			ConfigureSigma(chain, 2, 2, sigmaHooks);
			Assert::AreEqual(size_t{ 1 }, sigmaHooks.size(), L"libplacebo rejected the sigma-delta");
			Assert::IsTrue(CustomHookChain::SetHookParameter(sigmaHooks[0], "levels_full", 1.0));
			Assert::IsTrue(CustomHookChain::SetHookParameter(sigmaHooks[0], "occlusion_mask", 1.0));
			pl_tex seen = MakeMaskTexture(gpu, 1, 1, { 65535 });
			pl_tex fresh = MakeMaskTexture(gpu, 1, 1, { 0 });
			OcclusionMask::Hook mask;
			const pl_rect2df whole{ 0.0f, 0.0f, 2.0f, 2.0f };
			auto describe = [](const std::vector<int>& codes)
			{
				std::wstring text;
				for (int code : codes) text += std::to_wstring(code) + L" ";
				return text;
			};
			mask.SetFrame(seen, whole, whole);
			std::vector<int> alphas;
			const std::vector<int> carry = fixture.RenderTenBitSequence({ mask.Get(), sigmaHooks[0] },
				PL_COLOR_LEVELS_FULL, 0.5f, 8, &alphas);
			Assert::IsTrue(std::find(carry.begin(), carry.end(), 508) != carry.end() &&
				std::find(carry.begin(), carry.end(), 512) != carry.end(),
				(L"Full range, mask 1 must keep the carry (127/128): " + describe(carry)).c_str());
			for (int alpha : alphas)
				Assert::AreEqual(3, alpha, L"The sigma-delta must write alpha back to 1");
			mask.SetFrame(fresh, whole, whole);
			const std::vector<int> stopped = fixture.RenderTenBitSequence({ mask.Get(), sigmaHooks[0] },
				PL_COLOR_LEVELS_FULL, 0.5f, 8);
			for (int code : stopped)
				Assert::AreEqual(stopped[0], code, (L"Full range, mask 0 must stop the carry: " +
					describe(stopped)).c_str());
			pl_tex_destroy(gpu, &seen);
			pl_tex_destroy(gpu, &fresh);
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(log);
		}

		// The mask lands on the right output pixels after scaling (1.5x) and
		// edge blanking (16 px each side): source block column 1 (x 16..31)
		// is "new", so output x 40..63 (16 + 1.5 * [16, 32)) must stop the
		// carry while the rest of the picture keeps it.
		TEST_METHOD(OcclusionMaskAlignsWithThePictureAfterScalingAndBlanking)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			std::string log;
			fixture.CaptureLog(&log);
			pl_gpu gpu = fixture.Gpu();
			const int srcW = 64, srcH = 32, outW = 128, outH = 64;
			std::vector<const pl_hook*> sigmaHooks;
			CustomHookChain::Chain chain(SigmaLoader(fixture));
			ConfigureSigma(chain, outW, outH, sigmaHooks);
			Assert::AreEqual(size_t{ 1 }, sigmaHooks.size());
			Assert::IsTrue(CustomHookChain::SetHookParameter(sigmaHooks[0], "occlusion_mask", 1.0));
			// 4 x 2 blocks; column 1 is new content.
			pl_tex maskTex = MakeMaskTexture(gpu, 4, 2, { 65535, 0, 65535, 65535, 65535, 0, 65535, 65535 });
			OcclusionMask::Hook mask;
			const pl_rect2df sourceCrop{ 0.0f, 0.0f, static_cast<float>(srcW), static_cast<float>(srcH) };
			const pl_rect2df targetCrop{ 16.0f, 0.0f, 112.0f, static_cast<float>(outH) };
			mask.SetFrame(maskTex, sourceCrop, targetCrop);

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
			Assert::IsNotNull(targetFormat);
			std::vector<float> texels(static_cast<size_t>(srcW) * srcH * 4);
			for (size_t i = 0; i < texels.size(); i += 4)
			{
				texels[i] = texels[i + 1] = texels[i + 2] = 0.5f;
				texels[i + 3] = 1.0f;
			}
			pl_tex_params sp{};
			sp.w = srcW; sp.h = srcH; sp.format = sourceFormat; sp.sampleable = true; sp.initial_data = texels.data();
			pl_tex source = pl_tex_create(gpu, &sp);
			pl_tex_params tp{};
			tp.w = outW; tp.h = outH; tp.format = targetFormat; tp.renderable = true; tp.host_readable = true;
			tp.blit_dst = true;   // libplacebo clears outside the destination rectangle
			pl_tex target = pl_tex_create(gpu, &tp);
			pl_frame image = TargetLutGpuFixture::MakeRgbFrame(source);
			image.crop = sourceCrop;
			pl_frame output = TargetLutGpuFixture::MakeRgbFrame(target);
			output.repr.levels = PL_COLOR_LEVELS_LIMITED;
			output.repr.bits.sample_depth = 10;
			output.repr.bits.color_depth = 8;
			output.repr.bits.bit_shift = 2;
			output.crop = targetCrop;
			pl_render_params params = pl_render_fast_params;
			params.dither_params = nullptr;
			params.error_diffusion = nullptr;
			const pl_hook* hooks[2] = { mask.Get(), sigmaHooks[0] };
			params.hooks = hooks;
			params.num_hooks = 2;
			const int frames = 8;
			std::vector<std::vector<uint32_t>> rendered;
			for (int frame = 0; frame < frames; ++frame)
			{
				Assert::IsTrue(pl_render_image(fixture.Renderer(), &image, &output, &params));
				pl_gpu_finish(gpu);
				std::vector<uint32_t> pixels(static_cast<size_t>(outW) * outH);
				pl_tex_transfer_params download{};
				download.tex = target;
				download.ptr = pixels.data();
				Assert::IsTrue(pl_tex_download(gpu, &download));
				rendered.push_back(std::move(pixels));
			}
			auto constant = [&](int x, int y)
			{
				const uint32_t first = rendered[0][static_cast<size_t>(y) * outW + x] & 1023u;
				for (int frame = 1; frame < frames; ++frame)
					if ((rendered[frame][static_cast<size_t>(y) * outW + x] & 1023u) != first)
						return false;
				return true;
			};
			for (int y : { 8, 40 })
			{
				for (int x = 42; x < 62; ++x)
					Assert::IsTrue(constant(x, y), (L"Masked block not at x " + std::to_wstring(x)).c_str());
				for (int x : { 20, 30, 37, 66, 80, 100, 110 })
					Assert::IsFalse(constant(x, y), (L"Carry stopped outside the masked block at x " + std::to_wstring(x)).c_str());
			}
			pl_tex_destroy(gpu, &source);
			pl_tex_destroy(gpu, &target);
			pl_tex_destroy(gpu, &maskTex);
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(log);
		}

		// Stage 6c-2: with NLS active the mask follows the NLS shader's own map.
		// A white square at source block (13, 1), off centre where the map is
		// non-linear, is rendered through the real NLS shader; the mask marks
		// that block new, so the sigma-delta's carry must stop exactly where the
		// square appears, in both axes. The owner's four NLS profiles
		// (standard, protected, NLS+, NLS-V) with a centred and an off-centre source
		// crop: the NLS coordinates span the crop (the off-centre case tells the
		// crop from the whole texture by 5 px).
		TEST_METHOD(OcclusionMaskFollowsTheNlsMap)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			std::string log;
			fixture.CaptureLog(&log);
			pl_gpu gpu = fixture.Gpu();
			const int srcW = 256, srcH = 128, outW = 340, outH = 128;
			std::vector<const pl_hook*> sigmaHooks;
			CustomHookChain::Chain chain(SigmaLoader(fixture));
			ConfigureSigma(chain, outW, outH, sigmaHooks);
			Assert::AreEqual(size_t{ 1 }, sigmaHooks.size());
			// A square at source block (13, 1): x 208..223, y 16..31.
			std::vector<uint16_t> maskValues(16 * 8, 65535);
			maskValues[1 * 16 + 13] = 0;
			pl_tex maskTex = MakeMaskTexture(gpu, 16, 8, maskValues);
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
			Assert::IsNotNull(targetFormat);
			auto makeSource = [&](bool square)
			{
				std::vector<float> texels(static_cast<size_t>(srcW) * srcH * 4);
				for (int y = 0; y < srcH; ++y)
					for (int x = 0; x < srcW; ++x)
					{
						float* t = &texels[(static_cast<size_t>(y) * srcW + x) * 4];
						t[0] = t[1] = t[2] = square && x >= 208 && x < 224 && y >= 16 && y < 32 ? 1.0f : 0.5f;
						t[3] = 1.0f;
					}
				pl_tex_params sp{};
				sp.w = srcW; sp.h = srcH; sp.format = sourceFormat; sp.sampleable = true; sp.initial_data = texels.data();
				return pl_tex_create(gpu, &sp);
			};
			pl_tex squareSource = makeSource(true);
			pl_tex flatSource = makeSource(false);
			pl_tex_params tp{};
			tp.w = outW; tp.h = outH; tp.format = targetFormat; tp.renderable = true; tp.host_readable = true;
			tp.blit_dst = true;
			pl_tex target = pl_tex_create(gpu, &tp);
			const pl_rect2df targetCrop{ 0.0f, 0.0f, static_cast<float>(outW), static_cast<float>(outH) };
			OcclusionMask::Hook mask;

			// The owner's three NLS profiles (VideoProcessor.cfg), with the
			// shader loader's defaults filled in as the renderer sees them.
			struct Profile { const wchar_t* name; const char* file; bool plus; std::map<std::string, std::string> rule;
				bool vertical = false; };
			const Profile profiles[] = {
				{ L"standard", "NLS.glsl", false, { { "strength", "1" }, { "curve", "2" }, { "geometry", "0" },
					{ "center_protection", "0.35" } } },
				{ L"protected", "NLS.glsl", false, { { "strength", "1" }, { "curve", "2" }, { "geometry", "1" },
					{ "center_protection", "0.35" } } },
				{ L"plus", "NLSPlus.glsl", true, { { "strength", "1" }, { "curve", "2" }, { "geometry", "1" },
					{ "axis_balance", "0.25" }, { "max_center_zoom", "1.08" },
					{ "horizontal_center_protection", "0.35" }, { "vertical_center_protection", "0.25" } } },
				// NLS-V (VideoProcessor.cfg [shader.nls.vertical]): NLS.glsl warping
				// the vertical axis (warp_axis 1).
				{ L"vertical", "NLS.glsl", false, { { "strength", "1" }, { "curve", "2" }, { "geometry", "1" },
					{ "center_protection", "0.35" } }, true } };
			for (const Profile& profile : profiles)
			{
				std::string source = LoadBundledShader(profile.file);
				Assert::IsFalse(source.empty());
				std::map<std::string, std::string> tokens = profile.rule;
				tokens.emplace("center_protection", "0.35");
				tokens.emplace("axis_balance", "0");
				tokens.emplace("max_center_zoom", "1.08");
				tokens.emplace("horizontal_center_protection", "0.35");
				tokens.emplace("vertical_center_protection", "0.25");
				for (const auto& token : tokens)
					ReplaceShaderToken(source, token.first, token.second);
				const pl_hook* nls = pl_mpv_user_shader_parse(gpu, source.data(), source.size());
				Assert::IsNotNull(nls);
				BindNlsShader(nls, 1.33f, profile.vertical ? 1.0f : 0.0f);
				const NlsMaskMapping::Mapping mapping = NlsMaskMapping::FromParameters(profile.rule, 1.33,
					profile.vertical, profile.plus);
				Assert::IsTrue(mapping.valid, profile.name);
				for (const pl_rect2df sourceCrop : { pl_rect2df{ 0.0f, 0.0f, 256.0f, 128.0f },
					pl_rect2df{ 32.0f, 8.0f, 256.0f, 128.0f } })
				{
					auto render = [&](pl_tex sourceTex, const std::vector<const pl_hook*>& hooks)
					{
						pl_frame image = TargetLutGpuFixture::MakeRgbFrame(sourceTex);
						image.crop = sourceCrop;
						pl_frame output = TargetLutGpuFixture::MakeRgbFrame(target);
						output.repr.levels = PL_COLOR_LEVELS_LIMITED;
						output.repr.bits.sample_depth = 10;
						output.repr.bits.color_depth = 8;
						output.repr.bits.bit_shift = 2;
						output.crop = targetCrop;
						pl_render_params params = pl_render_fast_params;
						params.dither_params = nullptr;
						params.error_diffusion = nullptr;
						params.hooks = hooks.data();
						params.num_hooks = static_cast<int>(hooks.size());
						Assert::IsTrue(pl_render_image(fixture.Renderer(), &image, &output, &params));
						pl_gpu_finish(gpu);
						std::vector<uint32_t> pixels(static_cast<size_t>(outW) * outH);
						pl_tex_transfer_params download{};
						download.tex = target;
						download.ptr = pixels.data();
						Assert::IsTrue(pl_tex_download(gpu, &download));
						return pixels;
					};
					struct Box { int x0 = outW, y0 = outH, x1 = -1, y1 = -1; void Add(int x, int y)
						{ x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y); } };
					// Where NLS puts the square: red above mid grey (502).
					const std::vector<uint32_t> square = render(squareSource, { nls });
					Box seen;
					for (int y = 0; y < outH; ++y)
						for (int x = 0; x < outW; ++x)
							if ((square[static_cast<size_t>(y) * outW + x] & 1023u) > 700)
								seen.Add(x, y);
					// Where the mask stops the carry.
					Assert::IsTrue(CustomHookChain::SetHookParameter(sigmaHooks[0], "occlusion_mask", 1.0));
					mask.SetFrame(maskTex, sourceCrop, targetCrop);
					mask.SetNls(mapping, sourceCrop);
					std::vector<std::vector<uint32_t>> frames;
					for (int frame = 0; frame < 8; ++frame)
						frames.push_back(render(flatSource, { nls, mask.Get(), sigmaHooks[0] }));
					Box masked;
					for (int y = 0; y < outH; ++y)
						for (int x = 0; x < outW; ++x)
						{
							const uint32_t first = frames[0][static_cast<size_t>(y) * outW + x] & 1023u;
							bool constant = true;
							for (const auto& f : frames)
								constant = constant && (f[static_cast<size_t>(y) * outW + x] & 1023u) == first;
							if (constant)
								masked.Add(x, y);
						}
					const std::wstring where = std::wstring(profile.name) + L", crop " +
						std::to_wstring(static_cast<int>(sourceCrop.x0)) + L"," + std::to_wstring(static_cast<int>(sourceCrop.y0)) +
						L": square " + std::to_wstring(seen.x0) + L"," + std::to_wstring(seen.y0) + L".." +
						std::to_wstring(seen.x1) + L"," + std::to_wstring(seen.y1) + L", mask " +
						std::to_wstring(masked.x0) + L"," + std::to_wstring(masked.y0) + L".." +
						std::to_wstring(masked.x1) + L"," + std::to_wstring(masked.y1);
					Assert::IsTrue(seen.x1 >= 0 && masked.x1 >= 0 &&
						std::abs(masked.x0 - seen.x0) <= 2 && std::abs(masked.x1 - seen.x1) <= 2 &&
						std::abs(masked.y0 - seen.y0) <= 2 && std::abs(masked.y1 - seen.y1) <= 2, where.c_str());
				}
				pl_mpv_user_shader_destroy(&nls);
			}
			pl_tex_destroy(gpu, &squareSource);
			pl_tex_destroy(gpu, &flatSource);
			pl_tex_destroy(gpu, &target);
			pl_tex_destroy(gpu, &maskTex);
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(log);
		}

		// Stage 7 step 7a on the real shader and a 10-bit limited target set up
		// like VP's swapchain (colour depth 8, bit shift 2), with the
		// PRE_OUTPUT encoding repair as in VP:
		// - sign fix (BTB/WTW answers item 5): a below-black grey (code 40)
		//   keeps its below-black value through the boost and a gain of 1.3;
		// - delta 4: a mid grey (8-bit 126) at LED 100 with the compensation
		//   (gain 2.55 in linear light, E = 2.41) gives the same light as at
		//   LED 255 with no gain, within 1%, with the boost on and off.
		TEST_METHOD(LedCompensationKeepsBelowBlackAndLightExact)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			std::string log;
			fixture.CaptureLog(&log);
			CustomHookChain::Chain chain({
				[](const std::string& file, std::string& source, std::string& reason)
				{
					source = LoadBundledShader(file.c_str());
					reason = "bundled shader not found";
					return !source.empty();
				},
				[&fixture](const std::string& source) -> const pl_hook*
				{
					return pl_mpv_user_shader_parse(fixture.Gpu(), source.data(), source.size());
				},
				[](const pl_hook*& hook) { pl_mpv_user_shader_destroy(&hook); } });
			ConfiguredShaderRule rule;
			rule.name = "led_compensation";
			rule.filename = "LED compensation.glsl";
			chain.Configure({ rule });
			CustomHookChain::TemplateValues values;
			values.outputWidth = values.outputHeight = 2;
			chain.Ensure(values);
			std::vector<const pl_hook*> compensation;
			chain.Collect(false, compensation);
			Assert::AreEqual(size_t{ 1 }, compensation.size(), L"libplacebo rejected LED compensation.glsl");
			const pl_hook* hook = compensation[0];
			OutputEncodingRepair::Hook repair;
			pl_color_repr limited{};
			limited.sys = PL_COLOR_SYSTEM_RGB;
			limited.levels = PL_COLOR_LEVELS_LIMITED;
			limited.bits.sample_depth = 10;
			limited.bits.color_depth = 8;
			limited.bits.bit_shift = 2;
			repair.SetTarget(limited);
			const std::vector<const pl_hook*> hooks = { repair.Get(), hook };
			auto set = [hook](double gain, double boost)
			{
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "boost", boost));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "gain", gain));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "gain_domain", 1.0));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "knee", 0.08));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "compensation_eotf", 2.41));
			};
			// 10-bit limited code k <-> signal (k - 64) / 876 before the encode.
			auto signal = [](double code) { return (code - 64.0) / 876.0; };
			auto code = [&fixture, &hooks](double value)
			{
				return fixture.RenderTenBitSequence(hooks, PL_COLOR_LEVELS_LIMITED, static_cast<float>(value), 1)[0];
			};
			auto spow = [](double v, double e) { return (v < 0.0 ? -1.0 : 1.0) * std::pow(std::abs(v), e); };
			const double boost = 2.2 / 2.4;

			// Sign fix: code 40 (24 below black 64) stays below black.
			set(1.3, boost);
			const double btb = signal(40.0);
			const int btbOut = code(btb);
			const double btbExpected = 64.0 + 876.0 * spow(spow(spow(btb, boost), 2.41) * 1.3, 1.0 / 2.41);
			Assert::IsTrue(btbOut < 64, (L"Below black was cut to black: " + std::to_wstring(btbOut)).c_str());
			Assert::AreEqual(btbExpected, static_cast<double>(btbOut), 1.5,
				(L"Below black, expected " + std::to_wstring(btbExpected)).c_str());
			// Above white keeps its order against white (the knee rolls it off).
			const int white = code(signal(940.0));
			const int wtw = code(signal(1019.0));
			Assert::IsTrue(wtw >= white && wtw <= 1023, (L"White " + std::to_wstring(white) +
				L", above white " + std::to_wstring(wtw)).c_str());

			// Delta 4: light constant through LED 255 -> 100 (linear flux, 2.55x).
			const double grey = (126.0 - 16.0) / 219.0;   // 8-bit 126 as the signal
			for (const double b : { boost, 1.0 })
			{
				set(1.0, b);
				const double reference = std::pow(signal(code(grey)), 2.41);           // LED 255
				set(255.0 / 100.0, b);
				const double compensated = std::pow(signal(code(grey)), 2.41) * 100.0 / 255.0;   // LED 100
				Assert::AreEqual(1.0, compensated / reference, 0.01,
					(L"Light at LED 100 vs 255, boost " + std::to_wstring(b)).c_str());
			}
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLogExceptPreOutputBug(log);
		}

		TEST_METHOD(AllBundledStandardShadersLoadAndUpscalersEngageByHeight)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(),
				L"Could not create the libplacebo WARP test device");
			// Every warning or error libplacebo logs fails the test (design review 4, N-6).
			std::string cleanLog;
			fixture.CaptureLog(&cleanLog);
			CustomHookChain::Chain chain({
				[](const std::string& file, std::string& source, std::string& reason)
				{
					source = LoadBundledShader(file.c_str());
					reason = "bundled shader not found";
					return !source.empty();
				},
				[&fixture](const std::string& source) -> const pl_hook*
				{
					return pl_mpv_user_shader_parse(fixture.Gpu(), source.data(), source.size());
				},
				[](const pl_hook*& hook) { pl_mpv_user_shader_destroy(&hook); } });
			const char* files[] = { "CfL_Prediction.glsl", "FSRCNNX_x2_16-0-4-1.glsl",
				"ArtCNN_C4F16.glsl", "SSimDownscaler.glsl", "SSimSuperRes.glsl",
				"Temporal sigma-delta.glsl" };
			std::vector<ConfiguredShaderRule> rules;
			unsigned order = 0;
			for (const char* file : files)
			{
				ConfiguredShaderRule rule;
				rule.name = file;
				rule.filename = file;
				rule.profileSelectionOrder = order++;
				rules.push_back(rule);
			}
			chain.Configure(rules);
			CustomHookChain::TemplateValues values;
			values.outputWidth = 2560;
			values.outputHeight = 1600;
			values.sourceWidth = 1920;
			values.sourceHeight = 1080;
			values.refreshHz = 60.0;
			values.sourceFps = 24.0;
			std::string libplaceboLog;
			fixture.CaptureLog(&libplaceboLog);
			chain.Ensure(values);
			fixture.CaptureLog(nullptr);
			cleanLog += libplaceboLog;
			fixture.CaptureLog(&cleanLog);
			std::vector<const pl_hook*> hooks;
			chain.Collect(false, hooks);
			const std::wstring rejected = L"A bundled standard shader did not load: " +
				std::wstring(libplaceboLog.begin(), libplaceboLog.end());
			Assert::AreEqual(size_t{ 6 }, hooks.size(), rejected.c_str());
			Assert::IsTrue((hooks[1]->stages & PL_HOOK_LUMA_INPUT) != 0, L"FSRCNNX doubles luma");
			Assert::IsTrue((hooks[2]->stages & PL_HOOK_LUMA_INPUT) != 0, L"ArtCNN doubles luma");
			Assert::IsTrue((hooks[4]->stages & PL_HOOK_POST_KERNEL) != 0, L"SSimSuperRes refines after scaling");

			// With the 1.33x lens a 1080p 16:9 picture is enlarged ~1.48x
			// vertically but ~1.11x horizontally: the upscalers must engage on
			// height alone, and each keeps the authors' 1.3x minimum.
			for (const char* file : { "FSRCNNX_x2_16-0-4-1.glsl", "ArtCNN_C4F16.glsl" })
			{
				const std::string source = LoadBundledShader(file);
				// Directives start a line; the header comment quotes the old rule.
				Assert::IsTrue(source.find("\n//!WHEN OUTPUT.w LUMA.w") == std::string::npos,
					L"The upstream both-axes rule must be gone");
				Assert::IsTrue(source.find("\n//!WHEN OUTPUT.h LUMA.h / 1.3") != std::string::npos);
			}
			for (const char* file : { "SSimDownscaler.glsl", "SSimSuperRes.glsl" })
				Assert::IsTrue(LoadBundledShader(file).find("NATIVE_CROPPED.h") != std::string::npos,
					L"The SSIM pair engages from the live source and output heights");
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(cleanLog);
		}

		TEST_METHOD(BundledCustomHooksParseAndSigmaDeltaRunsOnTheRealGpuPath)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(),
				L"Could not create the libplacebo WARP test device");
			// Every warning or error libplacebo logs fails the test (design review 4, N-6).
			std::string cleanLog;
			fixture.CaptureLog(&cleanLog);
			CustomHookChain::Chain chain({
				[](const std::string& file, std::string& source, std::string& reason)
				{
					source = LoadBundledShader(file.c_str());
					reason = "bundled shader not found";
					return !source.empty();
				},
				[&fixture](const std::string& source) -> const pl_hook*
				{
					return pl_mpv_user_shader_parse(fixture.Gpu(), source.data(), source.size());
				},
				[](const pl_hook*& hook) { pl_mpv_user_shader_destroy(&hook); } });
			auto rule = [](const char* name, const char* file, unsigned order)
			{
				ConfiguredShaderRule configured;
				configured.name = name;
				configured.filename = file;
				configured.stageOrder = order;
				return configured;
			};
			ConfiguredShaderRule sigma = rule("sigma_delta", "Temporal sigma-delta.glsl", 90);
			sigma.parameters = { { "enabled", "1" }, { "levels_full", "1" },
				{ "swapchain_bits", "8" }, { "noise_amplitude", "0" }, { "test_ramp", "1" } };
			chain.Configure({ rule("cfl", "CfL_Prediction.glsl", 10),
				rule("ssim", "SSimDownscaler.glsl", 20), sigma });
			CustomHookChain::TemplateValues values;
			values.outputWidth = 64;
			values.outputHeight = 64;
			values.sourceWidth = 64;
			values.sourceHeight = 64;
			values.refreshHz = 60.0;
			values.sourceFps = 24.0;
			std::string libplaceboLog;
			fixture.CaptureLog(&libplaceboLog);
			chain.Ensure(values);
			fixture.CaptureLog(nullptr);
			cleanLog += libplaceboLog;
			fixture.CaptureLog(&cleanLog);
			std::vector<const pl_hook*> hooks;
			chain.Collect(false, hooks);
			const std::wstring rejected = L"libplacebo rejected a bundled custom shader: " +
				std::wstring(libplaceboLog.begin(), libplaceboLog.end());
			Assert::AreEqual(size_t{ 3 }, hooks.size(), rejected.c_str());
			Assert::IsTrue((hooks[0]->stages & PL_HOOK_CHROMA_INPUT) != 0, L"CfL must hook CHROMA");
			Assert::IsTrue((hooks[1]->stages & PL_HOOK_POST_KERNEL) != 0, L"SSimDownscaler must hook POSTKERNEL");
			Assert::IsTrue(CustomHookChain::HooksOutput(hooks[2]), L"Sigma-delta must hook OUTPUT");
			Assert::IsTrue(chain.SuppressesDither(false));

			// The ramp replaces the picture, so the readback proves the compute
			// pass, its storage textures and the parameter values all ran.
			const std::vector<RgbaPixel> pixels = fixture.RenderCoordinateField(hooks[2]);
			const size_t row = 32 * 64;
			Assert::IsTrue(pixels[row].r <= 2, L"Ramp must start at black");
			Assert::IsTrue(pixels[row + 63].r >= 253, L"Ramp must end at white");
			for (int x = 1; x < 64; ++x)
				Assert::IsTrue(pixels[row + x].r >= pixels[row + x - 1].r, L"Ramp must be monotonic");
			Assert::AreEqual(static_cast<int>(pixels[5 * 64 + 20].r),
				static_cast<int>(pixels[50 * 64 + 20].r), L"Ramp must not depend on the picture");
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(cleanLog);
		}

		// Stage 4: the sigma-delta's carried residual follows the picture's
		// motion. A textured picture with a quarter-code fraction moves 4
		// pixels right per refresh. With the plain gate every pixel changes, so
		// the residual is thrown away and the output is always the rounded
		// value (a quarter code low on average). With motion compensation the
		// residual travels with the content and the average along the motion
		// comes back to the true value.
		struct SigmaDeltaRun
		{
			double meanError = 0.0;   // along the motion, interior pixels
			std::vector<std::vector<int>> frames;
		};

		static double SigmaDeltaTexture(int x, int y)
		{
			// Aperiodic integer texture in 2x2 cells (so it survives the
			// quarter-resolution luma), plus a quarter code everywhere.
			const unsigned cell = static_cast<unsigned>((x >> 1) * 73856093u) ^
				static_cast<unsigned>((y >> 1) * 19349663u);
			return 40.0 + static_cast<double>((cell * 2654435761u >> 24) % 150u) + 0.25;
		}

		static SigmaDeltaRun RunSigmaDelta(bool motionCompensation, int speed, int frames,
			bool cutAtFrame4 = false, int speedY = 0, double noise = 0.0)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			CustomHookChain::Chain chain({
				[](const std::string& file, std::string& source, std::string& reason)
				{
					source = LoadBundledShader(file.c_str());
					reason = "bundled shader not found";
					return !source.empty();
				},
				[&fixture](const std::string& source) -> const pl_hook*
				{
					return pl_mpv_user_shader_parse(fixture.Gpu(), source.data(), source.size());
				},
				[](const pl_hook*& hook) { pl_mpv_user_shader_destroy(&hook); } });
			ConfiguredShaderRule sigma;
			sigma.name = "sigma_delta";
			sigma.filename = "Temporal sigma-delta.glsl";
			sigma.stageOrder = 90;
			sigma.parameters = { { "enabled", "1" }, { "levels_full", "1" }, { "swapchain_bits", "8" },
				{ "noise_amplitude", std::to_string(noise) }, { "motion_threshold", "1" },
				{ "motion_compensation", motionCompensation ? "1" : "0" } };
			chain.Configure({ sigma });
			const int width = 128 + speed * 8, height = 64 + speedY * 8;
			CustomHookChain::TemplateValues values;
			values.outputWidth = width;
			values.outputHeight = height;
			values.sourceWidth = width;
			values.sourceHeight = height;
			values.refreshHz = 60.0;
			values.sourceFps = 60.0;
			std::string log;
			fixture.CaptureLog(&log);
			chain.Ensure(values);
			fixture.CaptureLog(nullptr);
			std::vector<const pl_hook*> hooks;
			chain.Collect(false, hooks);
			const std::wstring rejected = L"libplacebo rejected the sigma-delta shader: " +
				std::wstring(log.begin(), log.end());
			Assert::AreEqual(size_t{ 1 }, hooks.size(), rejected.c_str());

			auto code = [speed, speedY, cutAtFrame4](int frame, int x, int y)
			{
				if (cutAtFrame4 && frame >= 4)
					return SigmaDeltaTexture(x * 3 + 1000, y * 5 + 777); // a different picture
				return SigmaDeltaTexture(x - speed * frame, y - speedY * frame);
			};
			AssertCleanLibplaceboLog(log);
			fixture.CaptureLog(&log);
			SigmaDeltaRun run;
			run.frames = fixture.RenderSequence(hooks[0], width, height, frames, code);
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(log);

			// Follow each piece of content along its motion over frames 1..8
			// (8 refreshes: whole cycles of a quarter-code residual).
			if (frames < 9)
				return run;
			double error = 0.0;
			int count = 0;
			for (int y0 = 16; y0 < height - 16 - speedY * 8; ++y0)
				for (int x0 = 16; x0 < width - 16 - speed * 8; ++x0)
				{
					double sum = 0.0;
					for (int frame = 1; frame <= 8; ++frame)
						sum += run.frames[frame][(y0 + speedY * frame) * width + x0 + speed * frame];
					error += std::abs(sum / 8.0 - SigmaDeltaTexture(x0, y0));
					++count;
				}
			run.meanError = error / count;
			return run;
		}

		TEST_METHOD(SigmaDeltaCarriesTheResidualAlongTheMotion)
		{
			const SigmaDeltaRun gate = RunSigmaDelta(false, 4, 9);
			const SigmaDeltaRun carried = RunSigmaDelta(true, 4, 9);
			Assert::IsTrue(gate.meanError > 0.2,
				(L"With the plain gate a moving picture should round (error " +
					std::to_wstring(gate.meanError) + L")").c_str());
			Assert::IsTrue(carried.meanError < 0.05,
				(L"With motion compensation the time-average along the motion should converge (error " +
					std::to_wstring(carried.meanError) + L")").c_str());

			// Speeds that are not a multiple of 4 px (the quarter-resolution
			// step) need the full-pixel refinement (stage 4 audit).
			for (int speed : { 5, 10 })
			{
				const SigmaDeltaRun odd = RunSigmaDelta(true, speed, 9);
				Assert::IsTrue(odd.meanError < 0.05,
					(std::to_wstring(speed) + L" px/refresh error " + std::to_wstring(odd.meanError)).c_str());
			}
			// Diagonal motion.
			const SigmaDeltaRun diagonal = RunSigmaDelta(true, 6, 9, false, 3);
			Assert::IsTrue(diagonal.meanError < 0.05,
				(L"Diagonal (6, 3) px/refresh error " + std::to_wstring(diagonal.meanError)).c_str());
			// With the default decorrelation noise (0.15) the carry still
			// converges; without it a moving picture keeps its rounding bias.
			const SigmaDeltaRun noisy = RunSigmaDelta(true, 5, 9, false, 0, 0.15);
			const SigmaDeltaRun noisyGate = RunSigmaDelta(false, 5, 9, false, 0, 0.15);
			Assert::IsTrue(noisy.meanError < 0.1 && noisyGate.meanError > 0.2,
				(L"With noise 0.15: carried " + std::to_wstring(noisy.meanError) +
					L", plain gate " + std::to_wstring(noisyGate.meanError)).c_str());

			// A still picture is unchanged by motion compensation (the plain
			// gate still carries the residual in place).
			// Faster motion (20 pixels per refresh) is beyond the +-8 pixel
			// fine search around zero.
			const SigmaDeltaRun fast = RunSigmaDelta(true, 20, 9);
			Assert::IsTrue(fast.meanError < 0.05,
				(L"Fast motion error " + std::to_wstring(fast.meanError)).c_str());
			// A fast pan (48 pixels per refresh) needs the coarse level of the
			// hierarchical search.
			const SigmaDeltaRun faster = RunSigmaDelta(true, 48, 9);
			Assert::IsTrue(faster.meanError < 0.05,
				(L"48 px/refresh error " + std::to_wstring(faster.meanError)).c_str());
			const SigmaDeltaRun still = RunSigmaDelta(true, 0, 9);
			Assert::IsTrue(still.meanError < 0.05,
				(L"Still picture error " + std::to_wstring(still.meanError)).c_str());
		}

		TEST_METHOD(SigmaDeltaGrainAwareCarry)
		{
			// S1: a moving texture with integer film grain (+-2 codes, new every
			// frame). With match_tolerance 1 the grain fails the block match and
			// the per-pixel gate, so the quarter code is rounded away (the
			// time-average along the motion is 0.25 low); with the grain map in
			// alpha (g = 1) and grain_tolerance_scale 4 the residual survives and
			// the average holds. Scale 1, or a map with no grain, changes nothing.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			const int speed = 4, frames = 9, width = 128 + speed * 8, height = 64;
			ConfiguredShaderRule sigma;
			sigma.name = "sigma_delta";
			sigma.filename = "Temporal sigma-delta.glsl";
			sigma.stageOrder = 90;
			sigma.parameters = { { "enabled", "1" }, { "levels_full", "1" }, { "swapchain_bits", "8" },
				{ "noise_amplitude", "0" }, { "motion_threshold", "1" }, { "match_tolerance", "1" },
				{ "motion_compensation", "1" } };
			CustomHookChain::TemplateValues values;
			values.outputWidth = values.sourceWidth = width;
			values.outputHeight = values.sourceHeight = height;
			values.refreshHz = values.sourceFps = 60.0;
			auto grain = [](int frame, int x, int y)
			{
				const unsigned h = (static_cast<unsigned>(x) * 73856093u) ^ (static_cast<unsigned>(y) * 19349663u) ^
					(static_cast<unsigned>(frame) * 83492791u);
				return static_cast<int>((h * 2654435761u >> 24) % 5u) - 2;
			};
			auto code = [&](int frame, int x, int y)
			{
				return SigmaDeltaTexture(x - speed * frame, y) + grain(frame, x, y);
			};
			// A grain map of one block: r = the floor (g = 1) or 1 (g = 0), trusted.
			auto makeMap = [&](float r)
			{
				const float texel[4] = { r, 1.0f, 0.0f, 1.0f };
				pl_tex_params mp{};
				mp.w = mp.h = 1;
				mp.format = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32, PL_FMT_CAP_SAMPLEABLE);
				mp.sampleable = true;
				mp.initial_data = texel;
				return mp.format ? pl_tex_create(gpu, &mp) : nullptr;
			};
			pl_tex grainy = makeMap(0.4f), clean = makeMap(1.0f);
			Assert::IsTrue(grainy && clean);
			OcclusionMask::Hook hook;
			const pl_rect2df whole{ 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height) };
			// A fresh sigma-delta per run: its residual state lives in its textures.
			auto run = [&](pl_tex map, double scale, bool withHook)
			{
				CustomHookChain::Chain chain(SigmaLoader(fixture));
				chain.Configure({ sigma });
				chain.Ensure(values);
				std::vector<const pl_hook*> sigmaHooks;
				chain.Collect(false, sigmaHooks);
				Assert::AreEqual(size_t{ 1 }, sigmaHooks.size(), (L"libplacebo rejected the sigma-delta: " +
					std::wstring(log.begin(), log.end())).c_str());
				Assert::IsTrue(CustomHookChain::SetHookParameter(sigmaHooks[0], "grain_tolerance_scale", scale));
				hook.SetFrame(nullptr, whole, whole);
				hook.SetGrain(map, 0.4f);
				std::vector<const pl_hook*> hooks;
				if (withHook)
					hooks.push_back(hook.Get());
				hooks.push_back(sigmaHooks[0]);
				return fixture.RenderSequence(hooks, width, height, frames, code);
			};
			// The time-average along the motion over frames 1..8 against the
			// noise-free value plus the grain that was really there.
			auto meanError = [&](const std::vector<std::vector<int>>& out)
			{
				double error = 0.0;
				int count = 0;
				for (int y0 = 16; y0 < height - 16; ++y0)
					for (int x0 = 16; x0 < width - 16 - speed * 8; ++x0)
					{
						double sum = 0.0, expected = 0.0;
						for (int frame = 1; frame <= 8; ++frame)
						{
							const int x = x0 + speed * frame;
							sum += out[frame][y0 * width + x];
							expected += code(frame, x, y0);
						}
						error += std::abs((sum - expected) / 8.0);
						++count;
					}
				return error / count;
			};
			const auto today = run(nullptr, 1.0, false);
			const auto scaleOne = run(grainy, 1.0, true);
			const auto noGrain = run(clean, 4.0, true);
			const auto grainAware = run(grainy, 4.0, true);
			pl_tex_destroy(gpu, &grainy);
			pl_tex_destroy(gpu, &clean);
			const double todayError = meanError(today), awareError = meanError(grainAware);
			const std::wstring message = L"along-motion error: today " + std::to_wstring(todayError) +
				L", grain-aware " + std::to_wstring(awareError) + L"; log: " + std::wstring(log.begin(), log.end());
			Assert::IsTrue(scaleOne == today, (L"scale 1 must not change the output; " + message).c_str());
			Assert::IsTrue(noGrain == today, (L"a map without grain must not change the output; " + message).c_str());
			Assert::IsTrue(todayError > 0.15, (L"the grain should defeat today's carry; " + message).c_str());
			Assert::IsTrue(awareError < 0.08, (L"the grain-aware carry should hold the average; " + message).c_str());
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(log);
		}

		TEST_METHOD(SigmaDeltaDropsTheResidualAtASceneCut)
		{
			// After a cut to an unrelated picture no residual is carried: every
			// pixel of the first new frame is the plain rounded value.
			const SigmaDeltaRun run = RunSigmaDelta(true, 4, 6, true);
			const int width = 128 + 4 * 8; // as RunSigmaDelta sizes it for speed 4
			int carriedPixels = 0, total = 0;
			for (int y = 0; y < 64; ++y)
				for (int x = 0; x < width; ++x)
				{
					const double value = SigmaDeltaTexture(x * 3 + 1000, y * 5 + 777);
					if (run.frames[4][y * width + x] != static_cast<int>(std::floor(value + 0.5)))
						++carriedPixels;
					++total;
				}
			Assert::IsTrue(carriedPixels * 50 <= total,
				(L"Pixels carrying a residual across the cut: " + std::to_wstring(carriedPixels)).c_str());
		}

		// Stage 6a: the de-judder's matcher and warp on the real GPU path.
		struct DejudderFrame
		{
			int width = 0, height = 0;
			std::vector<uint16_t> luma;     // width x height
			std::vector<uint16_t> chroma;   // (width / 2) x height, CbCr interleaved
		};

		// Aperiodic texture with detail at three scales (4, 16 and 64 px
		// cells), like a real picture: fine detail survives the half-resolution
		// luma and the broad structure survives the coarse pyramid levels.
		// Values in 16-bit code units.
		static uint16_t DejudderTexture(int x, int y, unsigned salt = 0)
		{
			auto noise = [salt](int cx, int cy, unsigned level) -> unsigned
			{
				const unsigned cell = (static_cast<unsigned>(cx) * 73856093u) ^
					(static_cast<unsigned>(cy) * 19349663u) ^ (salt * 83492791u) ^ (level * 2246822519u);
				return ((cell * 2654435761u) >> 16) % 16000u;
			};
			// Floor division, so negative coordinates (outside the picture) stay aperiodic.
			auto cellOf = [](int v, int size) { return v >= 0 ? v / size : -((-v + size - 1) / size); };
			return static_cast<uint16_t>(8000 + noise(cellOf(x, 4), cellOf(y, 4), 1) +
				noise(cellOf(x, 16), cellOf(y, 16), 2) + noise(cellOf(x, 64), cellOf(y, 64), 3));
		}

		template <typename Luma, typename Chroma>
		static DejudderFrame MakeDejudderFrame(int width, int height, Luma luma, Chroma chroma)
		{
			DejudderFrame frame;
			frame.width = width;
			frame.height = height;
			frame.luma.resize(static_cast<size_t>(width) * height);
			frame.chroma.resize(static_cast<size_t>(width / 2) * height * 2);
			for (int y = 0; y < height; ++y)
				for (int x = 0; x < width; ++x)
					frame.luma[static_cast<size_t>(y) * width + x] = luma(x, y);
			for (int y = 0; y < height; ++y)
				for (int x = 0; x < width / 2; ++x)
				{
					const size_t i = (static_cast<size_t>(y) * (width / 2) + x) * 2;
					frame.chroma[i] = chroma(x, y, 0);
					frame.chroma[i + 1] = chroma(x, y, 1);
				}
			return frame;
		}

		struct DejudderGpuPlanes
		{
			pl_gpu gpu = nullptr;
			Dejudder::Planes planes;
			~DejudderGpuPlanes()
			{
				pl_tex_destroy(gpu, &planes.luma);
				pl_tex_destroy(gpu, &planes.chroma);
			}
		};

		static void UploadDejudderFrame(pl_gpu gpu, const DejudderFrame& frame, DejudderGpuPlanes& out)
		{
			const enum pl_fmt_caps caps = static_cast<enum pl_fmt_caps>(
				PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_STORABLE | PL_FMT_CAP_LINEAR | PL_FMT_CAP_HOST_READABLE);
			pl_fmt r16 = pl_find_fmt(gpu, PL_FMT_UNORM, 1, 16, 16, caps);
			pl_fmt rg16 = pl_find_fmt(gpu, PL_FMT_UNORM, 2, 16, 16, caps);
			Assert::IsNotNull(r16, L"No storable R16 format");
			Assert::IsNotNull(rg16, L"No storable RG16 format");
			out.gpu = gpu;
			pl_tex_params luma{};
			luma.w = frame.width;
			luma.h = frame.height;
			luma.format = r16;
			luma.sampleable = true;
			luma.host_readable = true;
			luma.initial_data = frame.luma.data();
			out.planes.luma = pl_tex_create(gpu, &luma);
			pl_tex_params chroma = luma;
			chroma.w = frame.width / 2;
			chroma.format = rg16;
			chroma.initial_data = frame.chroma.data();
			out.planes.chroma = pl_tex_create(gpu, &chroma);
			out.planes.chromaShiftX = 1;
			out.planes.chromaShiftY = 0;
			Assert::IsNotNull(out.planes.luma);
			Assert::IsNotNull(out.planes.chroma);
		}

		static std::vector<uint16_t> DownloadPlane(pl_gpu gpu, pl_tex tex)
		{
			std::vector<uint16_t> data(static_cast<size_t>(tex->params.w) * tex->params.h *
				tex->params.format->num_components);
			pl_tex_transfer_params download{};
			download.tex = tex;
			download.ptr = data.data();
			Assert::IsTrue(pl_tex_download(gpu, &download));
			return data;
		}

		struct DejudderField
		{
			std::vector<float> texels;   // rgba per block
			int blocksX = 0, blocksY = 0;
			float vx(int bx, int by) const { return texels[(by * blocksX + bx) * 4]; }
			float vy(int bx, int by) const { return texels[(by * blocksX + bx) * 4 + 1]; }
			bool trusted(int bx, int by) const { return texels[(by * blocksX + bx) * 4 + 2] > 0.5f; }
		};

		static DejudderField DownloadField(pl_gpu gpu, const Dejudder::MotionWarp& warp, pl_tex which = nullptr)
		{
			pl_tex field = which ? which : warp.Field();
			Assert::IsNotNull(field);
			DejudderField result;
			result.blocksX = warp.BlocksX();
			result.blocksY = warp.BlocksY();
			std::vector<uint16_t> half(static_cast<size_t>(result.blocksX) * result.blocksY * 4);
			pl_tex_transfer_params download{};
			download.tex = field;
			download.ptr = half.data();
			Assert::IsTrue(pl_tex_download(gpu, &download), L"Field is not host-readable");
			result.texels.resize(half.size());
			for (size_t i = 0; i < half.size(); ++i)
			{
				// IEEE half -> float
				const uint16_t h = half[i];
				const int sign = (h >> 15) & 1, exponent = (h >> 10) & 31, mantissa = h & 1023;
				float value = exponent == 0 ? std::ldexp(static_cast<float>(mantissa), -24) :
					exponent == 31 ? (mantissa ? NAN : INFINITY) :
					std::ldexp(static_cast<float>(mantissa | 1024), exponent - 25);
				result.texels[i] = sign ? -value : value;
			}
			return result;
		}

		// libplacebo reports some failures (a rejected pass) only in its log,
		// with the call still returning success (design review 4, N-6), so the
		// GPU tests also require a clean log.
		static void AssertCleanLibplaceboLog(const std::string& log)
		{
			Assert::IsTrue(log.empty(),
				(L"libplacebo logged a warning or error: " + std::wstring(log.begin(), log.end())).c_str());
		}

		// The same, but accepting the one message libplacebo 7.360.1 logs for
		// every texture-input PRE_OUTPUT hook (the dropped output encoding that
		// OutputEncodingRepair puts back; see PreOutputHooksKeep...Repair).
		static void AssertCleanLibplaceboLogExceptPreOutputBug(std::string log)
		{
			const std::string known = "Attempted to modify an immutable shader!\n";
			for (size_t at; (at = log.find(known)) != std::string::npos; )
				log.erase(at, known.size());
			AssertCleanLibplaceboLog(log);
		}

		static Dejudder::Settings DejudderTestSettings()
		{
			Dejudder::Settings settings;
			settings.hostReadable = true;
			return settings;
		}

		TEST_METHOD(DejudderFollowsAMovingPictureAndWarpsBySDelta)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			const int width = 256, height = 128;
			// Content moves (12, 6) source pixels from A to B.
			static constexpr int dx = 12, dy = 6;
			auto chromaA = [](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); };
			const DejudderFrame a = MakeDejudderFrame(width, height,
				[](int x, int y) { return DejudderTexture(x, y); }, chromaA);
			const DejudderFrame b = MakeDejudderFrame(width, height,
				[](int x, int y) { return DejudderTexture(x - dx, y - dy); },
				[](int x, int y, int c) { return DejudderTexture((x - dx / 2) * 2, y - dy, 7 + c); });
			DejudderGpuPlanes planesA, planesB;
			UploadDejudderFrame(gpu, a, planesA);
			UploadDejudderFrame(gpu, b, planesB);

			std::string log;
			fixture.CaptureLog(&log);
			Dejudder::MotionWarp warp(fixture.Log(), gpu, DejudderTestSettings());
			const bool estimated = warp.Estimate(planesA.planes, planesB.planes);
			Assert::IsTrue(estimated, std::wstring(warp.LastError().begin(), warp.LastError().end()).c_str());

			// Interior blocks: trusted, vector (12, 6).
			const DejudderField field = DownloadField(gpu, warp);
			int interior = 0, good = 0;
			for (int by = 2; by < field.blocksY - 2; ++by)
				for (int bx = 2; bx < field.blocksX - 2; ++bx)
				{
					++interior;
					if (field.trusted(bx, by) && std::abs(field.vx(bx, by) - dx) < 0.6f &&
						std::abs(field.vy(bx, by) - dy) < 0.6f)
						++good;
				}
			// Both searches, for the message: the trust needs them to agree.
			const DejudderField fwdField = DownloadField(gpu, warp, warp.Forward());
			const DejudderField bwdField = DownloadField(gpu, warp, warp.Backward());
			int fwdOk = 0, bwdOk = 0;
			for (int by = 2; by < field.blocksY - 2; ++by)
				for (int bx = 2; bx < field.blocksX - 2; ++bx)
				{
					fwdOk += (std::abs(fwdField.vx(bx, by) - 6) < 0.3f && std::abs(fwdField.vy(bx, by) - 3) < 0.3f) ? 1 : 0;
					bwdOk += (std::abs(bwdField.vx(bx, by) + 6) < 0.3f && std::abs(bwdField.vy(bx, by) + 3) < 0.3f) ? 1 : 0;
				}
			Assert::IsTrue(good * 100 >= interior * 95,
				(L"Trusted (12, 6) vectors: " + std::to_wstring(good) + L" of " + std::to_wstring(interior) +
					L" (forward right " + std::to_wstring(fwdOk) + L", backward right " + std::to_wstring(bwdOk) +
					L"); log: " + std::wstring(log.begin(), log.end())).c_str());

			// shift 0.5: the picture lands half way, (6, 3).
			Dejudder::Planes out;
			Assert::IsTrue(warp.Warp(planesA.planes, 0.5f, out));
			Assert::IsTrue(out.luma != planesA.planes.luma, L"A warp writes new planes");
			const std::vector<uint16_t> luma = DownloadPlane(gpu, out.luma);
			int checked = 0, exact = 0;
			for (int y = 24; y < height - 24; ++y)
				for (int x = 24; x < width - 24; ++x)
				{
					++checked;
					if (std::abs(static_cast<int>(luma[y * width + x]) - DejudderTexture(x - 6, y - 3)) <= 2)
						++exact;
				}
			Assert::IsTrue(exact * 100 >= checked * 95, // held (untrusted) blocks excepted
				(L"Warped luma at (6, 3): " + std::to_wstring(exact) + L" of " + std::to_wstring(checked)).c_str());

			// CbCr (4:2:2): the horizontal motion is half in chroma pixels.
			const std::vector<uint16_t> chroma = DownloadPlane(gpu, out.chroma);
			int cchecked = 0, cexact = 0;
			for (int y = 24; y < height - 24; ++y)
				for (int x = 12; x < width / 2 - 12; ++x)
				{
					++cchecked;
					const int expected = DejudderTexture((x - 3) * 2, y - 3, 7);
					if (std::abs(static_cast<int>(chroma[(y * (width / 2) + x) * 2]) - expected) <= 2)
						++cexact;
				}
			Assert::IsTrue(cexact * 100 >= cchecked * 95,
				(L"Warped chroma at (3, 3): " + std::to_wstring(cexact) + L" of " + std::to_wstring(cchecked)).c_str());

			// Edges clamp to A's edge pixel, never black: the left columns of a
			// picture moving right sample beyond the edge.
			for (int y = 24; y < height - 24; y += 7)
				for (int x = 0; x < 6; ++x)
					Assert::IsTrue(luma[y * width + x] > 4000,
						L"A sample beyond the edge must clamp to the picture, not black");

			// shift 0 (the refresh A starts on) is A itself.
			Assert::IsTrue(warp.Warp(planesA.planes, 0.0f, out));
			Assert::IsTrue(out.luma == planesA.planes.luma && out.chroma == planesA.planes.chroma);
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(log);
		}

		// D5-1 (audit delta5-beta.md): the sigma-delta's domain from the
		// target; float / 16-bit targets are not a code domain it serves.
		TEST_METHOD(SigmaDeltaDomainFollowsTheTarget)
		{
			auto repr = [](enum pl_color_levels levels, int sample, int color)
			{
				pl_color_repr r{};
				r.sys = PL_COLOR_SYSTEM_RGB;
				r.levels = levels;
				r.bits.sample_depth = sample;
				r.bits.color_depth = color;
				return r;
			};
			const auto fl35 = SigmaDeltaDomain::FromTarget(repr(PL_COLOR_LEVELS_LIMITED, 10, 8));
			Assert::IsTrue(fl35.supported && !fl35.full && fl35.sample == 10 && fl35.color == 8);
			const auto ten = SigmaDeltaDomain::FromTarget(repr(PL_COLOR_LEVELS_FULL, 10, 10));
			Assert::IsTrue(ten.supported && ten.full && ten.color == 10);
			const auto eight = SigmaDeltaDomain::FromTarget(repr(PL_COLOR_LEVELS_UNKNOWN, 0, 0));
			Assert::IsTrue(eight.supported && eight.full && eight.sample == 8 && eight.color == 8);
			Assert::IsFalse(SigmaDeltaDomain::FromTarget(repr(PL_COLOR_LEVELS_FULL, 16, 16)).supported);
			Assert::IsFalse(SigmaDeltaDomain::FromTarget(repr(PL_COLOR_LEVELS_FULL, 16, 0)).supported);
		}

		// Stage 10: when preserve_extended_range is active, and why not.
		TEST_METHOD(ExtendedRangeDecision)
		{
			using namespace ExtendedRange;
			auto space = [](enum pl_color_primaries p, enum pl_color_transfer t)
			{
				pl_color_space c{};
				c.primaries = p;
				c.transfer = t;
				return c;
			};
			const pl_color_space source = space(PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_GAMMA22);
			const pl_color_space p3 = space(PL_COLOR_PRIM_DISPLAY_P3, PL_COLOR_TRC_GAMMA24);
			Assert::IsTrue(Decide(false, source, p3, false).reason == Reason::Off);
			const Decision on = Decide(true, source, p3, false);
			Assert::IsTrue(on.Active());
			Assert::IsTrue(on.convertGamma && on.convertPrimaries);
			Assert::AreEqual(2.2f, on.sourceGamma);
			Assert::AreEqual(2.4f, on.outputGamma);
			// White stays white: each row of the BT.709 -> P3 matrix sums to 1.
			for (int row = 0; row < 3; ++row)
				Assert::AreEqual(1.0, static_cast<double>(on.matrix[row] + on.matrix[3 + row] + on.matrix[6 + row]), 1e-5);
			Assert::IsTrue(on.matrix[0] < 1.0f && on.matrix[0] > 0.7f);   // red: 0.8225
			const Decision same = Decide(true, source, space(PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_GAMMA22), false);
			Assert::IsTrue(same.Active() && !same.convertGamma && !same.convertPrimaries);
			Assert::IsTrue(Decide(true, space(PL_COLOR_PRIM_BT_2020, PL_COLOR_TRC_GAMMA22),
				space(PL_COLOR_PRIM_BT_2020, PL_COLOR_TRC_GAMMA24), false).Active());
			Assert::IsTrue(Decide(true, space(PL_COLOR_PRIM_BT_2020, PL_COLOR_TRC_PQ), p3, false).reason == Reason::HdrSource);
			Assert::IsTrue(Decide(true, source, space(PL_COLOR_PRIM_BT_2020, PL_COLOR_TRC_PQ), false).reason == Reason::HdrOutput);
			Assert::IsTrue(Decide(true, source, p3, true).reason == Reason::CalibrationLut);
			Assert::IsTrue(Decide(true, space(PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_BT_1886), p3, false).reason == Reason::SourceTransfer);
			Assert::IsTrue(Decide(true, source, space(PL_COLOR_PRIM_DISPLAY_P3, PL_COLOR_TRC_SRGB), false).reason == Reason::OutputTransfer);
			Assert::IsTrue(Decide(true, space(PL_COLOR_PRIM_BT_2020, PL_COLOR_TRC_GAMMA22), p3, false).reason == Reason::SourcePrimaries);
			Assert::IsTrue(Decide(true, source, space(PL_COLOR_PRIM_DCI_P3, PL_COLOR_TRC_GAMMA24), false).reason == Reason::OutputPrimaries);
			// Prepare: nothing for an inactive decision; for an active one the
			// source is declared in the output's space, the restore goes first
			// (first at SCALED) and the carry last (last at RGB, after NLS).
			Hooks hooks;
			std::vector<const pl_hook*> list = { nullptr };
			pl_color_space declared = source;
			Prepare(Decide(false, source, p3, false), hooks, declared, p3, list);
			Assert::AreEqual(size_t(1), list.size());
			Assert::IsTrue(declared.primaries == PL_COLOR_PRIM_BT_709);
			Prepare(on, hooks, declared, p3, list);
			Assert::AreEqual(size_t(3), list.size());
			Assert::IsTrue(list[0] == hooks.Restore(true) && list[2] == hooks.Carry());
			Assert::IsTrue(hooks.Carry()->stages == PL_HOOK_RGB && hooks.Restore()->stages == PL_HOOK_SCALED);
			Assert::IsTrue(declared.primaries == PL_COLOR_PRIM_DISPLAY_P3 && declared.transfer == PL_COLOR_TRC_GAMMA24);
			Assert::IsTrue(hooks.Owns(hooks.Carry()->signature) && !hooks.Owns(1));
		}

		// Stage 10 (probe tables, docs/VP-btb-wtw-investigation.md): BTB and
		// WTW greys survive 1:1, sigmoid upscaling and linear-light
		// downscaling with the switch on; off is libplacebo's own path.
		TEST_METHOD(ExtendedRangeKeepsBtbAndWtwThroughScaling)
		{
			using namespace ExtendedRangeGpuProbe;
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create());
			std::string log;
			fixture.CaptureLog(&log);
			const std::vector<int> codes = { 4, 16, 40, 64, 100, 502, 900, 940, 980, 1019 };
			const Planes greys = Greys(codes);
			std::string report;
			auto run = [&](const char* name, Case c)
			{
				const Result r = Render(fixture.Gpu(), fixture.Renderer(), greys, c);
				Assert::IsTrue(r.ok, ToWide(std::string(name) + ": " + r.error).c_str());
				report += std::string(name) + ": " + Join(Columns(r, static_cast<int>(codes.size()), c.scale)) + "\n";
				return r;
			};
			{ Case c; c.on = false; c.scale = 1.5; run("off up 1.5", c); }
			for (double scale : { 1.0, 1.5, 0.5 })
				for (bool owner : { false, true })
				{
					if (owner && scale != 1.5) continue;
					Case c;
					c.scale = scale;
					c.ownerScaling = owner;
					const Result r = run(owner ? "on up 1.5 ewa_lanczossharp" : scale == 1.0 ? "on 1:1" : scale > 1.0 ? "on up 1.5" : "on down 0.5", c);
					Assert::IsTrue(r.decision.Active() && r.restores > 0, ToWide("the hooks did not run\n" + report).c_str());
					for (size_t i = 0; i < codes.size(); ++i)
					{
						const int x = static_cast<int>((i * 32 + 16) * scale);
						for (int ch = 0; ch < 3; ++ch)
							Assert::IsTrue(std::abs(r.Channel(x, r.height / 2, ch) - codes[i]) <= 1,
								ToWide("709->P3 grey " + std::to_string(codes[i]) + " not kept\n" + report).c_str());
					}
				}
			for (double scale : { 1.0, 1.5 })
			{
				Case c;
				c.scale = scale;
				c.outputTransfer = PL_COLOR_TRC_GAMMA24;
				const auto got = Columns(run(scale == 1.0 ? "on 2.2->2.4 1:1" : "on 2.2->2.4 up 1.5", c),
					static_cast<int>(codes.size()), scale);
				for (size_t i = 0; i < codes.size(); ++i)
				{
					const int ideal = Ideal(codes[i], 2.2, 2.4);
					Assert::IsTrue(ideal < 4 ? got[i] <= 4 : std::abs(got[i] - ideal) <= 1,
						ToWide("2.2->2.4 grey " + std::to_string(codes[i]) + " expected " + std::to_string(ideal) + "\n" + report).c_str());
				}
			}
			// Audit stage10-6d: a user MAIN shader that writes alpha 1 must not
			// change the picture. Since 10b alpha carries nothing: every code,
			// below black and above white included, is kept.
			{
				// A hook at the RGB stage after the carry that writes alpha 1,
				// as a user shader returning vec4(rgb, 1.0) would.
				pl_hook writer{};
				writer.stages = PL_HOOK_RGB;
				writer.input = PL_HOOK_SIG_COLOR;
				writer.hook = [](void*, const pl_hook_params* hp)
				{
					pl_custom_shader custom{};
					custom.description = "test: user shader writing alpha";
					custom.input = PL_SHADER_SIG_COLOR;
					custom.output = PL_SHADER_SIG_COLOR;
					custom.body = "color.a = 1.0;\n";
					pl_hook_res res{};
					res.failed = !pl_shader_custom(hp->sh, &custom);
					res.output = PL_HOOK_SIG_COLOR;
					res.sh = hp->sh;
					res.repr = hp->repr;
					res.color = hp->color;
					res.components = 4;
					res.rect = hp->rect;
					return res;
				};
				Case c;
				c.extraHook = &writer;
				const auto got = Columns(run("on 1:1, alpha-writing hook", c), static_cast<int>(codes.size()), 1.0);
				for (size_t i = 0; i < codes.size(); ++i)
					Assert::IsTrue(std::abs(got[i] - codes[i]) <= 2, ToWide("alpha writer changed the picture\n" + report).c_str());
			}
			fixture.CaptureLog(nullptr);
			Logger::WriteMessage(report.c_str());
			Assert::IsTrue(log.empty(), ToWide("libplacebo logged: " + log).c_str());
		}

		// Stage 10: in-range content is scaled as without the switch (planner:
		// max 1 code, mean <= 0.03 at up 1.5 and down 0.5; 10a measured <= 0.01.
		// 10b: max 1 code kept, mean up to 0.083 measured - libplacebo stores the
		// scaled picture in its half-float texture before the texture-form
		// restore, about 1/4 code; accepted by the planner 2026-10-12),
		// also with a gamma change, and in-gamut BT.709 colours come out as
		// libplacebo's own conversion. With ewa_lanczossharp the polar
		// sampler's four-channel path rounds differently (max 1 code, mean
		// 0.034 measured): bounded at 0.05.
		TEST_METHOD(ExtendedRangeKeepsTodaysScalingAndColours)
		{
			using namespace ExtendedRangeGpuProbe;
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create());
			std::string report;
			const Planes edges = Edges();
			auto compare = [&](const char* name, Case c)
			{
				Case off = c;
				off.on = false;
				const Result a = Render(fixture.Gpu(), fixture.Renderer(), edges, off);
				const Result b = Render(fixture.Gpu(), fixture.Renderer(), edges, c);
				Assert::IsTrue(a.ok && b.ok, ToWide(std::string(name) + ": render failed").c_str());
				const Difference d = Compare(a, b);
				char line[160];
				std::snprintf(line, sizeof(line), "%s: max %d, mean %.4f\n", name, d.max, d.mean);
				report += line;
				return d;
			};
			bool fail = false;
			for (enum pl_color_primaries prim : { PL_COLOR_PRIM_BT_709, PL_COLOR_PRIM_DISPLAY_P3 })
			for (double scale : { 1.5, 0.5, 0.75 })
			{
				Case c;
				c.scale = scale;
				c.outputPrimaries = prim;
				const Difference d = compare((std::string(prim == PL_COLOR_PRIM_BT_709 ? "709 " : "P3 ") +
					(scale == 1.5 ? "edges up 1.5" : scale == 0.5 ? "edges down 0.5" : "edges down 0.75")).c_str(), c);
				fail |= !(d.max <= 1 && d.mean <= 0.1);   // 10b: see the note above
			}
			{
				Case c;
				c.scale = 1.5;
				c.ownerScaling = true;
				const Difference d = compare("edges up 1.5 ewa_lanczossharp", c);
				fail |= !(d.max <= 1 && d.mean <= 0.1);   // 10b: see the note above
			}
			// A gamma change: the carry re-encodes to the output gamma, so
			// libplacebo's linear light is the same as without the switch.
			for (double scale : { 1.5, 0.5 })
			{
				Case c;
				c.scale = scale;
				c.outputTransfer = PL_COLOR_TRC_GAMMA24;
				const Difference d = compare(scale == 1.5 ? "edges up 1.5, 2.2->2.4" : "edges down 0.5, 2.2->2.4", c);
				fail |= !(d.max <= 1 && d.mean <= 0.1);   // 10b: see the note above
			}
			// In-gamut BT.709 colours, 1:1 into P3 with a gamma change.
			const std::vector<Patch> patches = { { 502, 512, 512 }, { 560, 440, 600 }, { 400, 450, 700 },
				{ 600, 350, 450 }, { 300, 700, 480 }, { 700, 560, 380 }, { 250, 560, 560 } };
			const Planes colours = Patches(patches);
			for (enum pl_color_transfer output : { PL_COLOR_TRC_GAMMA22, PL_COLOR_TRC_GAMMA24 })
			{
				Case c;
				c.outputTransfer = output;
				Case off = c;
				off.on = false;
				const Result a = Render(fixture.Gpu(), fixture.Renderer(), colours, off);
				const Result b = Render(fixture.Gpu(), fixture.Renderer(), colours, c);
				Assert::IsTrue(a.ok && b.ok);
				int worst = 0;
				for (size_t i = 0; i < patches.size(); ++i)
				{
					const int x = static_cast<int>(i * 32 + 16), y = a.height / 2;
					report += "colour " + std::to_string(i) + (output == PL_COLOR_TRC_GAMMA22 ? " 2.2" : " 2.4") + ": off ";
					for (int ch = 0; ch < 3; ++ch) report += std::to_string(a.Channel(x, y, ch)) + (ch < 2 ? "," : "");
					report += " on ";
					for (int ch = 0; ch < 3; ++ch)
					{
						report += std::to_string(b.Channel(x, y, ch)) + (ch < 2 ? "," : "");
						worst = std::max(worst, std::abs(a.Channel(x, y, ch) - b.Channel(x, y, ch)));
					}
					report += "\n";
				}
				fail |= worst > 2;
			}
			Assert::IsFalse(fail, ToWide(report).c_str());
			Logger::WriteMessage(report.c_str());
		}

		// Delta 5 (D5-3): the match tolerance means the same for full-range
		// sources. B is A moved by (8, 4) (whole pixels at the matcher's half
		// resolution, so no interpolation error) with every luma sample 3.2 8-bit codes
		// brighter: over the default tolerance (3 codes) as limited-range codes,
		// within it as full-range luma (3.2 x 219 / 255 = 2.75
		// limited-equivalent codes).
		// Stage 10: the first frame with the carry after frames without it is
		// right (scaling, light domain, Mitchell). After the SSIM user shader
		// too: AdaptiveDownscaleBlendsEdgesTextureAndGrain.
		TEST_METHOD(ExtendedRangeFirstFrameAfterOtherFrames)
		{
			using namespace ExtendedRangeGpuProbe;
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create());
			pl_gpu gpu = fixture.Gpu();
			const std::vector<int> codes = { 4, 16, 40, 64, 100, 502, 900, 940, 980, 1019 };
			std::string report;
			auto run = [&](const char* name, Case c)
			{
				const Result r = Render(gpu, fixture.Renderer(), Greys(codes, 64), c);
				Assert::IsTrue(r.ok);
				const auto got = Columns(r, static_cast<int>(codes.size()), c.scale, 64);
				report += std::string(name) + ": " + Join(got) + "\n";
				return got;
			};
			Case off;
			off.on = false;
			off.scale = 0.5;
			Case on;
			on.scale = 0.5;
			run("off down", off);
			run("on down (1st)", on);
			run("on down (2nd)", on);
			Case offUp = off;
			offUp.scale = 1.5;
			run("off up", offUp);
			run("on down after off up", on);
			Case gammaOff = off;
			gammaOff.linear = false;
			run("off down, gamma light", gammaOff);
			run("on down after gamma light", on);
			Case mitchellOff = off;
			mitchellOff.mitchell = true;
			run("off down, mitchell", mitchellOff);
			Case mitchellOn = on;
			mitchellOn.mitchell = true;
			run("on down mitchell after off mitchell", mitchellOn);
			Logger::WriteMessage(report.c_str());
			for (const std::string& row : { std::string("on down (1st): "), std::string("on down after off up: "),
				std::string("on down after gamma light: "), std::string("on down mitchell after off mitchell: ") })
				Assert::IsTrue(report.find(row + "4 16 40 64 100 502 900 940") != std::string::npos,
					ExtendedRangeGpuProbe::ToWide("a first carry frame is wrong\n" + report).c_str());
		}

		TEST_METHOD(DejudderToleranceFollowsTheSourceRange)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			const int width = 256, height = 128;
			auto chroma = [](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); };
			const DejudderFrame a = MakeDejudderFrame(width, height,
				[](int x, int y) { return DejudderTexture(x, y); }, chroma);
			const DejudderFrame b = MakeDejudderFrame(width, height,
				[](int x, int y)
				{
					return static_cast<uint16_t>(std::min(65535, DejudderTexture(x - 8, y - 4) + 822));   // 3.2 x 257
				}, chroma);
			auto trustedShare = [&](bool fullRange)
			{
				DejudderGpuPlanes planesA, planesB;
				UploadDejudderFrame(gpu, a, planesA);
				UploadDejudderFrame(gpu, b, planesB);
				planesA.planes.fullRange = planesB.planes.fullRange = fullRange;
				Dejudder::MotionWarp warp(fixture.Log(), gpu, DejudderTestSettings());
				Assert::IsTrue(warp.Estimate(planesA.planes, planesB.planes));
				const DejudderField field = DownloadField(gpu, warp);
				int interior = 0, trusted = 0;
				for (int by = 2; by < field.blocksY - 2; ++by)
					for (int bx = 2; bx < field.blocksX - 2; ++bx)
					{
						++interior;
						trusted += field.trusted(bx, by) ? 1 : 0;
					}
				return interior > 0 ? static_cast<double>(trusted) / interior : 0.0;
			};
			const double limited = trustedShare(false);
			const double full = trustedShare(true);
			Assert::IsTrue(limited <= 0.1, (L"limited range: 3.2 codes trusted in " + std::to_wstring(limited)).c_str());
			Assert::IsTrue(full >= 0.9, (L"full range: 2.75 codes trusted in " + std::to_wstring(full)).c_str());
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(log);
		}

		TEST_METHOD(DejudderRefinesToHalfAPixel)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			const int width = 256, height = 128;
			// (7, 3) source pixels = (3.5, 1.5) at half resolution.
			auto chroma = [](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); };
			const DejudderFrame a = MakeDejudderFrame(width, height,
				[](int x, int y) { return DejudderTexture(x, y); }, chroma);
			const DejudderFrame b = MakeDejudderFrame(width, height,
				[](int x, int y) { return DejudderTexture(x - 7, y - 3); }, chroma);
			DejudderGpuPlanes planesA, planesB;
			UploadDejudderFrame(gpu, a, planesA);
			UploadDejudderFrame(gpu, b, planesB);
			Dejudder::MotionWarp warp(fixture.Log(), gpu, DejudderTestSettings());
			Assert::IsTrue(warp.Estimate(planesA.planes, planesB.planes));
			const DejudderField field = DownloadField(gpu, warp);
			int interior = 0, good = 0;
			for (int by = 2; by < field.blocksY - 2; ++by)
				for (int bx = 2; bx < field.blocksX - 2; ++bx)
				{
					++interior;
					// Within half a source pixel: a matcher stopping at whole
					// half-resolution pixels (6 or 8, 2 or 4) would fail this.
					if (std::abs(field.vx(bx, by) - 7.0f) <= 0.5f && std::abs(field.vy(bx, by) - 3.0f) <= 0.5f)
						++good;
				}
			Assert::IsTrue(good * 100 >= interior * 90,
				(L"(7, 3) within half a pixel: " + std::to_wstring(good) + L" of " + std::to_wstring(interior)).c_str());
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(log);
		}

		TEST_METHOD(DejudderHoldsAtACutAndAtOcclusions)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			const int width = 256, height = 128;
			auto chroma = [](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); };

			// A cut: B is an unrelated picture. Nothing is trusted, so the warp
			// is a hard hold of A everywhere.
			{
				const DejudderFrame a = MakeDejudderFrame(width, height,
					[](int x, int y) { return DejudderTexture(x, y); }, chroma);
				const DejudderFrame b = MakeDejudderFrame(width, height,
					[](int x, int y) { return DejudderTexture(x, y, 99); }, chroma);
				DejudderGpuPlanes planesA, planesB;
				UploadDejudderFrame(gpu, a, planesA);
				UploadDejudderFrame(gpu, b, planesB);
				Dejudder::MotionWarp warp(fixture.Log(), gpu, DejudderTestSettings());
				Assert::IsTrue(warp.Estimate(planesA.planes, planesB.planes));
				const DejudderField field = DownloadField(gpu, warp);
				int trusted = 0;
				for (int by = 0; by < field.blocksY; ++by)
					for (int bx = 0; bx < field.blocksX; ++bx)
						trusted += field.trusted(bx, by) ? 1 : 0;
				Assert::IsTrue(trusted * 100 <= field.blocksX * field.blocksY * 2,
					(L"Trusted blocks across a cut: " + std::to_wstring(trusted)).c_str());
				// Frame trust (6b-2): the GPU's own count matches, and is below
				// the 25% whole-frame hold.
				const float share = DownloadField(gpu, warp, warp.FrameTrust()).texels[0];
				Assert::IsTrue(std::abs(share - static_cast<float>(trusted) / (field.blocksX * field.blocksY)) < 0.002f,
					(L"Frame trust " + std::to_wstring(share)).c_str());
				Assert::IsTrue(share < 0.25f);
				Dejudder::Planes out;
				Assert::IsTrue(warp.Warp(planesA.planes, 0.3f, out));
				Assert::IsTrue(out.luma != planesA.planes.luma, L"The GPU hold still writes the warped planes");
				// Below the frame trust the whole frame is held: every luma and
				// chroma sample is exactly A's, including the few blocks the
				// matcher trusted by chance.
				const std::vector<uint16_t> luma = DownloadPlane(gpu, out.luma);
				const std::vector<uint16_t> cbcr = DownloadPlane(gpu, out.chroma);
				Assert::IsTrue(luma == a.luma, L"Across a cut the luma must be exactly A");
				Assert::IsTrue(cbcr == a.chroma, L"Across a cut the chroma must be exactly A");
				// The caller's own cut flag holds everything.
				Assert::IsTrue(warp.Warp(planesA.planes, 0.3f, out, false));
				Assert::IsTrue(out.luma == planesA.planes.luma);
			}

			// Occlusion: a textured square moves 16 px right over a still
			// background. Blocks at its leading and trailing edges are covered
			// or revealed and must hold; the square and the far background
			// keep their own motion.
			{
				auto scene = [](int shift)
				{
					return [shift](int x, int y)
					{
						const bool inside = x >= 80 + shift && x < 144 + shift && y >= 32 && y < 96;
						return inside ? DejudderTexture(x - shift, y, 5) : DejudderTexture(x, y);
					};
				};
				const DejudderFrame a = MakeDejudderFrame(width, height, scene(0), chroma);
				const DejudderFrame b = MakeDejudderFrame(width, height, scene(16), chroma);
				DejudderGpuPlanes planesA, planesB;
				UploadDejudderFrame(gpu, a, planesA);
				UploadDejudderFrame(gpu, b, planesB);
				Dejudder::MotionWarp warp(fixture.Log(), gpu, DejudderTestSettings());
				Assert::IsTrue(warp.Estimate(planesA.planes, planesB.planes, 1));
				// 6c: B's occlusion mask. In B the square has moved to x 96..159,
				// so the background it uncovered (x 80..95, bx 5) is new; the
				// square's inside and the far background were seen in A.
				const DejudderField occlusion = DownloadField(gpu, warp, warp.Occlusion(1));
				auto seen = [&](int bx, int by) { return occlusion.texels[(by * occlusion.blocksX + bx) * 4] > 0.5f; };
				Assert::IsFalse(seen(5, 3), L"Uncovered background in B must be marked new");
				Assert::IsTrue(seen(7, 3), L"The square's inside was seen in A");
				Assert::IsTrue(seen(1, 1) && seen(14, 6), L"The still background was seen in A");
				Assert::IsNull(warp.Occlusion(2));
				const DejudderField field = DownloadField(gpu, warp);
				// Blocks are 16x16 source pixels: the square covers bx 5..8,
				// by 2..5. bx 9 is about to be covered (leading edge).
				Assert::IsFalse(field.trusted(9, 3), L"A block about to be covered must not be trusted");
				Assert::IsTrue(field.trusted(6, 3) && std::abs(field.vx(6, 3) - 16.0f) < 0.6f,
					L"The square's inside keeps its motion");
				Assert::IsTrue(field.trusted(1, 1) && std::abs(field.vx(1, 1)) < 0.6f,
					L"The still background far away is trusted with no motion");
				Assert::IsTrue(DownloadField(gpu, warp, warp.FrameTrust()).texels[0] > 0.5f,
					L"An occlusion is local: the frame stays trusted");
				Dejudder::Planes out;
				Assert::IsTrue(warp.Warp(planesA.planes, 0.4f, out));
				const std::vector<uint16_t> luma = DownloadPlane(gpu, out.luma);
				// Inside an untrusted block the output is A, unchanged.
				for (int y = 50; y < 60; ++y)
					for (int x = 150; x < 158; ++x)
						Assert::AreEqual(static_cast<int>(a.luma[y * width + x]),
							static_cast<int>(luma[y * width + x]), L"Occluded blocks are a hard hold of A");
			}
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(log);
		}

		// Stage 6b-2: an extra present renders A's frame with the warped planes
		// swapped in, through the normal pl_render_image. The rendered
		// picture must move by shift x the motion, and a zero shift (the
		// hold) must render exactly A's own picture.
		TEST_METHOD(DejudderExtraRenderMovesThePictureBySDelta)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			const int width = 256, height = 128;
			static constexpr int dx = 12, dy = 6;
			const DejudderFrame a = MakeDejudderFrame(width, height,
				[](int x, int y) { return DejudderTexture(x, y); },
				[](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); });
			const DejudderFrame b = MakeDejudderFrame(width, height,
				[](int x, int y) { return DejudderTexture(x - dx, y - dy); },
				[](int x, int y, int c) { return DejudderTexture((x - dx / 2) * 2, y - dy, 7 + c); });
			DejudderGpuPlanes planesA, planesB;
			UploadDejudderFrame(gpu, a, planesA);
			UploadDejudderFrame(gpu, b, planesB);
			Dejudder::MotionWarp warp(fixture.Log(), gpu, DejudderTestSettings());
			Assert::IsTrue(warp.Estimate(planesA.planes, planesB.planes));

			pl_fmt targetFormat = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 8, 8,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE));
			Assert::IsNotNull(targetFormat);
			pl_tex_params targetParams{};
			targetParams.w = width;
			targetParams.h = height;
			targetParams.format = targetFormat;
			targetParams.renderable = true;
			targetParams.host_readable = true;
			pl_tex targetTexture = pl_tex_create(gpu, &targetParams);
			Assert::IsNotNull(targetTexture);
			pl_render_params params = pl_render_fast_params;
			params.dither_params = nullptr;
			params.error_diffusion = nullptr;
			// The renderer's source frame: Y and interleaved CbCr, 4:2:2.
			auto render = [&](const Dejudder::Planes& planes)
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
				pl_frame target = TargetLutGpuFixture::MakeRgbFrame(targetTexture);
				target.color.transfer = PL_COLOR_TRC_BT_1886;
				Assert::IsTrue(pl_render_image(fixture.Renderer(), &image, &target, &params));
				pl_gpu_finish(gpu);
				std::vector<RgbaPixel> output(static_cast<size_t>(width) * height);
				pl_tex_transfer_params download{};
				download.tex = targetTexture;
				download.ptr = output.data();
				Assert::IsTrue(pl_tex_download(gpu, &download));
				return output;
			};
			const std::vector<RgbaPixel> own = render(planesA.planes);

			// The hold (shift 0) renders A's own picture exactly.
			Dejudder::Planes held;
			Assert::IsTrue(warp.Warp(planesA.planes, 0.0f, held));
			const std::vector<RgbaPixel> heldPixels = render(held);
			for (size_t i = 0; i < own.size(); ++i)
				Assert::IsTrue(own[i].r == heldPixels[i].r && own[i].g == heldPixels[i].g &&
					own[i].b == heldPixels[i].b, L"A zero shift must render A's own picture");

			// shift 0.5: the picture moves (6, 3) px. Find the best whole-pixel
			// displacement of the rendered output over the interior.
			Dejudder::Planes warped;
			Assert::IsTrue(warp.Warp(planesA.planes, 0.5f, warped));
			const std::vector<RgbaPixel> moved = render(warped);
			int bestX = 0, bestY = 0;
			double bestError = 1e30;
			for (int oy = -2; oy <= 6; ++oy)
				for (int ox = -2; ox <= 10; ++ox)
				{
					double error = 0.0;
					for (int y = 24; y < height - 24; ++y)
						for (int x = 32; x < width - 32; ++x)
						{
							const RgbaPixel& m = moved[static_cast<size_t>(y) * width + x];
							const RgbaPixel& o = own[static_cast<size_t>(y - oy) * width + (x - ox)];
							error += std::abs(m.r - o.r) + std::abs(m.g - o.g) + std::abs(m.b - o.b);
						}
					if (error < bestError) { bestError = error; bestX = ox; bestY = oy; }
				}
			Assert::IsTrue(bestX == dx / 2 && bestY == dy / 2,
				(L"The extra moved the picture by (" + std::to_wstring(bestX) + L", " +
					std::to_wstring(bestY) + L"), expected (6, 3)").c_str());
			pl_tex_destroy(gpu, &targetTexture);
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(log);
		}

		// Stage 6d: an extra made by warping A's cached PRE_OUTPUT picture
		// against one made by re-rendering a warped source (same vectors): at
		// 1:1 and 2x the picture moves by the same amount, the hold (shift 0)
		// and a cut are A exactly, and the interior differs only by the
		// kernels (bilinear on the source vs Catmull-Rom on the output) and,
		// on under 1% of samples, by the source path deciding chroma's hold
		// at chroma positions (measured: mean 0.10 / 0.15 codes, 0.28% /
		// 0.63% over 2 codes). Full-range target: on 7.360 a limited target
		// goes through VP's encoding repair for A but not for the extra (no
		// PRE_OUTPUT hook there), which rounds a code differently; the Beta
		// test checks the limited target on 377 (no repair).
		TEST_METHOD(DejudderRenderedWarpMatchesTheSourceWarp)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			const int width = 256, height = 128;
			static constexpr int dx = 12, dy = 6;
			auto chroma = [](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); };
			const DejudderFrame a = MakeDejudderFrame(width, height,
				[](int x, int y) { return DejudderTexture(x, y); }, chroma);
			const DejudderFrame b = MakeDejudderFrame(width, height,
				[](int x, int y) { return DejudderTexture(x - dx, y - dy); },
				[](int x, int y, int c) { return DejudderTexture((x - dx / 2) * 2, y - dy, 7 + c); });
			const DejudderFrame cut = MakeDejudderFrame(width, height,
				[](int x, int y) { return DejudderTexture(x, y, 99); }, chroma);
			DejudderGpuPlanes planesA, planesB, planesCut;
			UploadDejudderFrame(gpu, a, planesA);
			UploadDejudderFrame(gpu, b, planesB);
			UploadDejudderFrame(gpu, cut, planesCut);
			Dejudder::MotionWarp warp(fixture.Log(), gpu, DejudderTestSettings());
			DejudderRendered::Cache cache;
			DejudderRendered::Warp rendered(fixture.Log(), gpu);
			std::string report;

			for (int scale : { 1, 2 })
			{
				const int ow = width * scale, oh = height * scale;
				pl_fmt targetFormat = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 8, 8,
					static_cast<enum pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE));
				pl_tex_params targetParams{};
				targetParams.w = ow;
				targetParams.h = oh;
				targetParams.format = targetFormat;
				targetParams.renderable = true;
				targetParams.host_readable = true;
				pl_tex targetTexture = pl_tex_create(gpu, &targetParams);
				Assert::IsNotNull(targetTexture);
				pl_render_params params = pl_render_fast_params;
				params.dither_params = nullptr;
				params.error_diffusion = nullptr;
				auto target = [&]()
				{
					pl_frame t = TargetLutGpuFixture::MakeRgbFrame(targetTexture);
					t.repr.levels = PL_COLOR_LEVELS_FULL;
					t.color.transfer = PL_COLOR_TRC_BT_1886;
					return t;
				};
				auto download = [&]()
				{
					pl_gpu_finish(gpu);
					std::vector<RgbaPixel> output(static_cast<size_t>(ow) * oh);
					pl_tex_transfer_params d{};
					d.tex = targetTexture;
					d.ptr = output.data();
					Assert::IsTrue(pl_tex_download(gpu, &d));
					return output;
				};
				// The renderer's source frame, with the given hooks.
				auto renderSource = [&](const Dejudder::Planes& planes, std::vector<const pl_hook*> hooks)
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
					// libplacebo 7.360.1's PRE_OUTPUT bug: the renderer adds the
					// repair when a texture-input PRE_OUTPUT hook is present.
					OutputEncodingRepair::Hook repair;
					if (OutputEncodingRepair::Needed(hooks, nullptr, PL_LUT_UNKNOWN))
					{
						repair.SetTarget(t.repr);
						hooks.insert(hooks.begin(), repair.Get());
					}
					pl_render_params p = params;
					p.hooks = hooks.empty() ? nullptr : hooks.data();
					p.num_hooks = static_cast<int>(hooks.size());
					Assert::IsTrue(pl_render_image(fixture.Renderer(), &image, &t, &p));
					return download();
				};
				auto renderCached = [&](float shift, uint64_t sequence)
				{
					pl_tex out = nullptr;
					DejudderRendered::Geometry g;
					g.sourceCrop = { 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height) };
					g.targetCrop = { 0.0f, 0.0f, static_cast<float>(ow), static_cast<float>(oh) };
					Assert::IsTrue(rendered.Run(cache.Texture(sequence), warp.Field(), warp.FrameTrust(),
						warp.GetSettings().minTrustedShare, shift, g, out), ExtendedRangeGpuProbe::ToWide(rendered.Error()).c_str());
					pl_frame image = DejudderRendered::Frame(out, cache.Repr(), cache.Color());
					pl_frame t = target();
					std::vector<const pl_hook*> kept;
					const pl_render_params p = DejudderRendered::OutputOnly(params, {}, kept);
					Assert::IsTrue(pl_render_image(fixture.Renderer(), &image, &t, &p));
					return download();
				};
				auto best = [&](const std::vector<RgbaPixel>& moved, const std::vector<RgbaPixel>& own, int& bx, int& by)
				{
					double bestError = 1e30;
					for (int oy = -2; oy <= 4 * scale; ++oy)
						for (int ox = -2; ox <= 7 * scale; ++ox)
						{
							double error = 0.0;
							for (int y = 24 * scale; y < oh - 24 * scale; ++y)
								for (int x = 32 * scale; x < ow - 32 * scale; ++x)
								{
									const RgbaPixel& m = moved[static_cast<size_t>(y) * ow + x];
									const RgbaPixel& o = own[static_cast<size_t>(y - oy) * ow + (x - ox)];
									error += std::abs(m.r - o.r) + std::abs(m.g - o.g) + std::abs(m.b - o.b);
								}
							if (error < bestError) { bestError = error; bx = ox; by = oy; }
						}
				};

				// A with the cache armed: the picture is unchanged by the hook.
				const std::vector<RgbaPixel> own = renderSource(planesA.planes, {});
				cache.Arm(7);
				const std::vector<RgbaPixel> cachedOwn = renderSource(planesA.planes, { cache.Hook() });
				Assert::IsNotNull(cache.Texture(7), L"The cache hook did not keep A's picture");
				Assert::IsNull(cache.Texture(8));
				// The hook makes libplacebo keep A's picture in its 16-bit float
				// intermediate (as the LED measurement hook already does): at
				// most a code of rounding, and the encoding unchanged.
				{
					int maxDiff = 0;
					for (size_t i = 0; i < own.size(); ++i)
						maxDiff = std::max({ maxDiff, std::abs(own[i].r - cachedOwn[i].r), std::abs(own[i].g - cachedOwn[i].g),
							std::abs(own[i].b - cachedOwn[i].b) });
					report += "scale " + std::to_string(scale) + ": cache hook max " + std::to_string(maxDiff) + "; ";
					Assert::IsTrue(maxDiff <= 1, ExtendedRangeGpuProbe::ToWide("The cache hook changed A's picture: " + report).c_str());
				}

				Assert::IsTrue(warp.Estimate(planesA.planes, planesB.planes));
				// Hold: A exactly.
				const std::vector<RgbaPixel> held = renderCached(0.0f, 7);
				int heldMax = 0;
				for (size_t i = 0; i < own.size(); ++i)
					heldMax = std::max({ heldMax, std::abs(cachedOwn[i].r - held[i].r), std::abs(cachedOwn[i].g - held[i].g),
						std::abs(cachedOwn[i].b - held[i].b) });
				report += "hold max " + std::to_string(heldMax);
				Assert::AreEqual(0, heldMax, ExtendedRangeGpuProbe::ToWide("A zero shift must show A exactly\n" + report).c_str());

				// shift 0.5 by both paths.
				Dejudder::Planes warpedPlanes;
				Assert::IsTrue(warp.Warp(planesA.planes, 0.5f, warpedPlanes));
				const std::vector<RgbaPixel> bySource = renderSource(warpedPlanes, {});
				const std::vector<RgbaPixel> byRendered = renderCached(0.5f, 7);
				int sx = 0, sy = 0, rx = 0, ry = 0;
				best(bySource, own, sx, sy);
				best(byRendered, own, rx, ry);
				double sum = 0.0;
				int maxDiff = 0, count = 0, over2 = 0;
				for (int y = 24 * scale; y < oh - 24 * scale; ++y)
					for (int x = 32 * scale; x < ow - 32 * scale; ++x)
					{
						const RgbaPixel& m = bySource[static_cast<size_t>(y) * ow + x];
						const RgbaPixel& r = byRendered[static_cast<size_t>(y) * ow + x];
						for (int d : { std::abs(m.r - r.r), std::abs(m.g - r.g), std::abs(m.b - r.b) })
						{
							sum += d;
							maxDiff = std::max(maxDiff, d);
							over2 += d > 2 ? 1 : 0;
							++count;
						}
					}
				char line[200];
				std::snprintf(line, sizeof(line), "; moved source (%d,%d) rendered (%d,%d); interior diff mean %.3f max %d, %.2f%% over 2 codes\n",
					sx, sy, rx, ry, sum / count, maxDiff, 100.0 * over2 / count);
				report += line;
				Assert::IsTrue(rx == dx * scale / 2 && ry == dy * scale / 2 && sx == rx && sy == ry,
					ExtendedRangeGpuProbe::ToWide("The rendered extra moved differently\n" + report).c_str());
				Assert::IsTrue(sum / count <= 0.5 && over2 <= count / 100,
					ExtendedRangeGpuProbe::ToWide("The rendered extra differs from the source extra: " + report).c_str());

				// A cut (nothing trusted): A exactly.
				Assert::IsTrue(warp.Estimate(planesA.planes, planesCut.planes));
				const std::vector<RgbaPixel> atCut = renderCached(0.5f, 7);
				for (size_t i = 0; i < own.size(); ++i)
					Assert::IsTrue(cachedOwn[i].r == atCut[i].r && cachedOwn[i].g == atCut[i].g && cachedOwn[i].b == atCut[i].b,
						L"A cut must show A exactly");
				pl_tex_destroy(gpu, &targetTexture);
			}
			// Audit stage10-6d item 2: a fractional destination rectangle.
			// libplacebo renders onto it rounded; the extra uses the same
			// rounding, so a hold is still A exactly (not a resample).
			{
				const auto r = DejudderRendered::Rounded({ 0.0f, 0.0f, 256.0f, 128.0f },
					{ 10.4f, 5.6f, 250.3f, 120.5f }, 256.0f, 128.0f);
				Assert::IsTrue(r.target.x0 == 10.0f && r.target.y0 == 6.0f && r.target.x1 == 250.0f && r.target.y1 == 121.0f);
				Assert::AreEqual(-0.4f * 256.0f / 239.9f, r.source.x0, 1e-4f);
				pl_fmt targetFormat = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 8, 8,
					static_cast<enum pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE));
				pl_tex_params tp{};
				tp.w = width;
				tp.h = height;
				tp.format = targetFormat;
				tp.renderable = true;
				tp.host_readable = true;
				tp.blit_dst = true;
				pl_tex targetTexture = pl_tex_create(gpu, &tp);
				pl_render_params params = pl_render_fast_params;
				params.dither_params = nullptr;
				params.error_diffusion = nullptr;
				const pl_rect2df fractional = { 10.4f, 5.6f, 250.3f, 120.5f };
				auto frameTarget = [&](const pl_rect2df& crop)
				{
					pl_frame t = TargetLutGpuFixture::MakeRgbFrame(targetTexture);
					t.color.transfer = PL_COLOR_TRC_BT_1886;
					t.crop = crop;
					return t;
				};
				auto download = [&]()
				{
					pl_gpu_finish(gpu);
					std::vector<RgbaPixel> output(static_cast<size_t>(width) * height);
					pl_tex_transfer_params d{};
					d.tex = targetTexture;
					d.ptr = output.data();
					Assert::IsTrue(pl_tex_download(gpu, &d));
					return output;
				};
				pl_frame image{};
				image.num_planes = 2;
				image.planes[0].texture = planesA.planes.luma;
				image.planes[0].components = 1;
				image.planes[0].component_mapping[0] = PL_CHANNEL_Y;
				image.planes[1].texture = planesA.planes.chroma;
				image.planes[1].components = 2;
				image.planes[1].component_mapping[0] = PL_CHANNEL_CB;
				image.planes[1].component_mapping[1] = PL_CHANNEL_CR;
				image.repr.sys = PL_COLOR_SYSTEM_BT_709;
				image.repr.levels = PL_COLOR_LEVELS_FULL;
				image.color.primaries = PL_COLOR_PRIM_BT_709;
				image.color.transfer = PL_COLOR_TRC_BT_1886;
				pl_frame_set_chroma_location(&image, PL_CHROMA_LEFT);
				std::vector<const pl_hook*> hooks = { cache.Hook() };
				OutputEncodingRepair::Hook repair;
				pl_frame t = frameTarget(fractional);
				if (OutputEncodingRepair::Needed(hooks, nullptr, PL_LUT_UNKNOWN))
				{
					repair.SetTarget(t.repr);
					hooks.insert(hooks.begin(), repair.Get());
				}
				pl_render_params p = params;
				p.hooks = hooks.data();
				p.num_hooks = static_cast<int>(hooks.size());
				const float black[] = { 0.0f, 0.0f, 0.0f };
				pl_frame_clear(gpu, &t, black);
				cache.Arm(9);
				Assert::IsTrue(pl_render_image(fixture.Renderer(), &image, &t, &p));
				const std::vector<RgbaPixel> shown = download();
				Assert::IsNotNull(cache.Texture(9));
				Assert::IsTrue(cache.Texture(9)->params.w == 240 && cache.Texture(9)->params.h == 115,
					L"The cache is not libplacebo's rounded rectangle");
				const auto rounded = DejudderRendered::Rounded(image.crop.x1 > 0 ? image.crop :
					pl_rect2df{ 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height) }, fractional,
					static_cast<float>(width), static_cast<float>(height));
				DejudderRendered::Geometry g;
				g.sourceCrop = rounded.source;
				g.targetCrop = rounded.target;
				pl_tex out = nullptr;
				Assert::IsTrue(rendered.Run(cache.Texture(9), warp.Field(), warp.FrameTrust(),
					warp.GetSettings().minTrustedShare, 0.0f, g, out));
				pl_frame held = DejudderRendered::Frame(out, cache.Repr(), cache.Color());
				pl_frame ht = frameTarget(rounded.target);
				std::vector<const pl_hook*> kept;
				const pl_render_params hp = DejudderRendered::OutputOnly(params, {}, kept);
				pl_frame_clear(gpu, &ht, black);
				Assert::IsTrue(pl_render_image(fixture.Renderer(), &held, &ht, &hp));
				const std::vector<RgbaPixel> heldPixels = download();
				int maxDiff = 0;
				for (int y = 6; y < 121; ++y)
					for (int x = 10; x < 250; ++x)
					{
						const RgbaPixel& a1 = shown[static_cast<size_t>(y) * width + x];
						const RgbaPixel& b1 = heldPixels[static_cast<size_t>(y) * width + x];
						maxDiff = std::max({ maxDiff, std::abs(a1.r - b1.r), std::abs(a1.g - b1.g), std::abs(a1.b - b1.b) });
					}
				report += "fractional crop: hold max " + std::to_string(maxDiff) + "\n";
				Assert::AreEqual(0, maxDiff, ExtendedRangeGpuProbe::ToWide("A fractional crop resampled the extra: " + report).c_str());
				pl_tex_destroy(gpu, &targetTexture);
			}
			fixture.CaptureLog(nullptr);
			Logger::WriteMessage(report.c_str());
			// 7.360.1 logs its PRE_OUTPUT encoding bug for the cache hook (a
			// texture-input PRE_OUTPUT hook); VP's repair is in A's chain.
			AssertCleanLibplaceboLogExceptPreOutputBug(log);
			Assert::IsTrue(cache.Copies() == 3 && cache.Failed() == 0);
			cache.Release();
			Assert::IsNull(cache.Texture(9), L"Release must drop the cached picture");
		}

		// Stage 8-2: the adaptive downscale blend (SSIM / faithful, luma only)
		// on a 1.5x downscale. Hard edges come out as the faithful branch,
		// low-contrast texture as SSIM; the grain term falls to the floor on
		// noise, recovers smoothly, snaps on a cut; nothing happens on an
		// upscale; both light domains give the same weight map; and the Stage
		// 10 carry's BTB/WTW codes survive the measure, SSIM and the blend.
		TEST_METHOD(AdaptiveDownscaleBlendsEdgesTextureAndGrain)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create());
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			const pl_hook* ssim = nullptr;
			{
				const std::string source = LoadBundledShader("SSimDownscaler.glsl");
				Assert::IsFalse(source.empty());
				ssim = pl_mpv_user_shader_parse(gpu, source.data(), source.size());
				Assert::IsNotNull(ssim);
			}
			static constexpr int sw = 384, sh = 240;
			pl_fmt srcFmt = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 16, 16,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_LINEAR));
			pl_fmt dstFmt = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 8, 8,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE));
			Assert::IsTrue(srcFmt && dstFmt);
			auto makeSource = [&](auto code)
			{
				std::vector<uint16_t> px(static_cast<size_t>(sw) * sh * 4);
				for (int y = 0; y < sh; ++y)
					for (int x = 0; x < sw; ++x)
					{
						const double c = std::min(255.0, std::max(0.0, static_cast<double>(code(x, y))));
						const uint16_t v = static_cast<uint16_t>(std::lround(c * 257.0));
						uint16_t* p = &px[(static_cast<size_t>(y) * sw + x) * 4];
						p[0] = p[1] = p[2] = v;
						p[3] = 65535;
					}
				pl_tex_params tp{};
				tp.w = sw;
				tp.h = sh;
				tp.format = srcFmt;
				tp.sampleable = true;
				tp.initial_data = px.data();
				return pl_tex_create(gpu, &tp);
			};
			// Hard stripes on the left, low-contrast fine texture on the right.
			pl_tex scene = makeSource([](int x, int y)
			{
				if (x < sw / 2)
					return ((x / 12) % 2) ? 220.0 : 30.0;
				return 128.0 + 5.0 * std::sin(x * 1.7) * std::cos(y * 1.3);
			});
			uint32_t noiseState = 12345u;
			auto noise = [&noiseState]()
			{
				noiseState = noiseState * 1664525u + 1013904223u;
				return (static_cast<double>(noiseState >> 8) / 16777216.0 - 0.5) * 2.0;
			};
			std::vector<double> noiseField(static_cast<size_t>(sw) * sh);
			for (double& n : noiseField) n = noise() * 7.0;
			pl_tex grainy = makeSource([&](int x, int y) { return 128.0 + noiseField[static_cast<size_t>(y) * sw + x]; });
			pl_tex clean = makeSource([](int, int) { return 128.0; });
			pl_tex dark = makeSource([](int, int) { return 20.0; });
			pl_tex brighter = makeSource([&](int x, int y) { return 200.0 + noiseField[static_cast<size_t>(y) * sw + x]; });

			AdaptiveDownscale::Module module;
			uint64_t sequence = 0;
			auto render = [&](pl_tex src, int ow, int oh, bool withModule, const AdaptiveDownscale::Settings& st,
				bool gammaLight, bool advance)
			{
				pl_tex_params tp{};
				tp.w = ow;
				tp.h = oh;
				tp.format = dstFmt;
				tp.renderable = true;
				tp.host_readable = true;
				pl_tex target = pl_tex_create(gpu, &tp);
				pl_frame image = TargetLutGpuFixture::MakeRgbFrame(src);
				image.color.transfer = PL_COLOR_TRC_GAMMA22;
				pl_frame out = TargetLutGpuFixture::MakeRgbFrame(target);
				out.color.transfer = PL_COLOR_TRC_GAMMA22;
				pl_render_params params = pl_render_default_params;
				params.dither_params = nullptr;
				params.error_diffusion = nullptr;
				params.downscaler = &pl_filter_mitchell;
				params.disable_linear_scaling = gammaLight;
				std::vector<const pl_hook*> hooks;
				if (advance) ++sequence;
				if (withModule)
				{
					module.SetFrame(st, sequence, 1.0f / 24.0f);
					hooks.push_back(module.Measure());
				}
				hooks.push_back(ssim);
				if (withModule)
					hooks.push_back(module.Blend());
				params.hooks = hooks.data();
				params.num_hooks = static_cast<int>(hooks.size());
				Assert::IsTrue(pl_render_image(fixture.Renderer(), &image, &out, &params));
				pl_gpu_finish(gpu);
				std::vector<RgbaPixel> px(static_cast<size_t>(ow) * oh);
				pl_tex_transfer_params d{};
				d.tex = target;
				d.ptr = px.data();
				Assert::IsTrue(pl_tex_download(gpu, &d));
				pl_tex_destroy(gpu, &target);
				return px;
			};
			// The stats of the last advancing frame (polled by the next render).
			auto grainNow = [&](pl_tex src, const AdaptiveDownscale::Settings& st)
			{
				render(src, 256, 160, true, st, false, false);
				render(src, 256, 160, true, st, false, false);
				return module.LastStats();
			};
			std::string report;
			char line[200];

			// 1. Edges faithful, texture SSIM (grain term held at 1).
			AdaptiveDownscale::Settings noGrain;
			noGrain.grainLo = 63.0f;
			noGrain.grainHi = 64.0f;
			AdaptiveDownscale::Settings faithfulOnly = noGrain;
			faithfulOnly.wBase = 0.0f;
			// With edges switched off (thresholds above any gradient) w is 1
			// everywhere: the blend must be the SSIM picture (grey content: the
			// luma blend is the whole picture).
			AdaptiveDownscale::Settings allSsim = noGrain;
			allSsim.edgeLo = 254.0f;
			allSsim.edgeHi = 255.0f;
			const auto ssimOnly = render(scene, 256, 160, false, noGrain, false, true);
			const auto faithfulPx = render(scene, 256, 160, true, faithfulOnly, false, true);
			const auto asSsim = render(scene, 256, 160, true, allSsim, false, true);
			const auto blended = render(scene, 256, 160, true, noGrain, false, true);
			Assert::IsTrue(module.Active() && module.Failures() == 0, L"The blend did not run on a downscale");
			int ssimMax = 0, edgeMax = 0, edgeSsimVsFaithful = 0;
			for (int y = 20; y < 140; ++y)
				for (int x = 4; x < 252; ++x)
				{
					const size_t i = static_cast<size_t>(y) * 256 + x;
					ssimMax = std::max(ssimMax, std::abs(asSsim[i].g - ssimOnly[i].g));
					const int sx = static_cast<int>((x + 0.5) * 1.5);
					if (x < 120 && (sx % 12 == 0 || sx % 12 == 11))   // stripe transitions
					{
						edgeMax = std::max(edgeMax, std::abs(blended[i].g - faithfulPx[i].g));
						edgeSsimVsFaithful = std::max(edgeSsimVsFaithful, std::abs(ssimOnly[i].g - faithfulPx[i].g));
					}
				}
			std::snprintf(line, sizeof(line), "w=1 vs SSIM max %d; edges vs faithful max %d (SSIM vs faithful there %d)\n",
				ssimMax, edgeMax, edgeSsimVsFaithful);
			report += line;
			// Edges: the edge map is interpolated between source pixels at a
			// transition, so up to 2 codes of SSIM remain there.
			Assert::IsTrue(ssimMax <= 1 && edgeMax <= 2 && edgeSsimVsFaithful >= 4,
				ExtendedRangeGpuProbe::ToWide(report).c_str());

			// 2. Grain: after a cut (dark -> grey noise) it snaps to the floor;
			// a clean field of the same mean recovers smoothly; a luma jump
			// (grey -> bright noise) snaps again.
			AdaptiveDownscale::Settings grain;
			render(dark, 256, 160, true, grain, false, true);
			render(grainy, 256, 160, true, grain, false, true);
			const auto noisy = grainNow(grainy, grain);
			render(clean, 256, 160, true, grain, false, true);
			const auto oneClean = grainNow(clean, grain);
			for (int i = 0; i < 120; ++i)
				render(clean, 256, 160, true, grain, false, true);
			const auto manyClean = grainNow(clean, grain);
			render(brighter, 256, 160, true, grain, false, true);
			const auto afterCut = grainNow(brighter, grain);
			std::snprintf(line, sizeof(line), "grain: noise %.3f (valid %d, luma %.1f, w %.3f), clean 1 frame %.3f, 120 frames %.3f, cut to noise %.3f (luma %.1f)\n",
				noisy.grainScale, noisy.valid ? 1 : 0, noisy.meanLuma, noisy.meanW, oneClean.grainScale,
				manyClean.grainScale, afterCut.grainScale, afterCut.meanLuma);
			report += line;
			Assert::IsTrue(noisy.valid && noisy.grainScale <= grain.grainFloor + 0.05f, ExtendedRangeGpuProbe::ToWide(report).c_str());
			Assert::IsTrue(oneClean.grainScale < 0.5f, ExtendedRangeGpuProbe::ToWide("no smoothing: " + report).c_str());
			Assert::IsTrue(manyClean.grainScale > 0.95f, ExtendedRangeGpuProbe::ToWide("no recovery: " + report).c_str());
			Assert::IsTrue(afterCut.grainScale <= grain.grainFloor + 0.05f, ExtendedRangeGpuProbe::ToWide("no snap: " + report).c_str());

			// 3. No effect on an upscale (the SSIM shader's own condition).
			const auto upPlain = render(scene, 576, 360, false, noGrain, false, true);
			const auto upModule = render(scene, 576, 360, true, noGrain, false, true);
			Assert::IsFalse(module.Active(), L"The blend ran on an upscale");
			for (size_t i = 0; i < upPlain.size(); ++i)
				Assert::IsTrue(upPlain[i].r == upModule[i].r && upPlain[i].g == upModule[i].g, L"An upscale changed");
			// 3b. The source's own height decides, as SSIM's `NATIVE_CROPPED.h >
			// POSTKERNEL.h`: a picture a prescaler made taller than the output,
			// from a source that is not, is left alone (audit stage8-rs232 item 3).
			module.SetSourceDownscales(false);
			render(scene, 256, 160, true, noGrain, false, true);
			Assert::IsFalse(module.Active(), L"The blend ran although the source is not downscaled");
			module.SetSourceDownscales(true);

			// 4. Both light domains, the same weight map (edges from the source).
			render(scene, 256, 160, true, noGrain, false, true);
			const auto linearStats = grainNow(scene, noGrain);
			render(scene, 256, 160, true, noGrain, true, true);
			render(scene, 256, 160, true, noGrain, true, false);
			render(scene, 256, 160, true, noGrain, true, false);
			const auto gammaStats = module.LastStats();
			std::snprintf(line, sizeof(line), "mean w linear %.4f gamma %.4f\n", linearStats.meanW, gammaStats.meanW);
			report += line;
			Assert::IsTrue(std::abs(linearStats.meanW - gammaStats.meanW) < 0.02f, ExtendedRangeGpuProbe::ToWide(report).c_str());

			// 5. The carry through measure, SSIM and blend (BT.709 -> P3, down 0.5).
			{
				using namespace ExtendedRangeGpuProbe;
				const std::vector<int> codes = { 4, 16, 40, 64, 100, 502, 900, 940, 980, 1019 };
				Case c;
				c.scale = 0.5;
				c.mitchell = true;
				module.SetFrame(noGrain, ++sequence, 1.0f / 24.0f);
				c.extraHooks = { module.Measure(), ssim, module.Blend() };
				// With the SSIM downscaler (the owner's 4K chain) every code is
				// kept, below black too, from the first carry frame on. (The
				// restore renders its own texture: as an inline colour step it was
				// disabled after any texture-output hook at POST_KERNEL.)
				auto check = [&](const std::vector<int>& got, const char* what)
				{
					for (size_t i = 0; i < codes.size(); ++i)
						Assert::IsTrue(std::abs(got[i] - codes[i]) <= (codes[i] > 940 || codes[i] < 64 ? 2 : 1),
							ToWide(std::string(what) + " changed the picture\n" + report).c_str());
				};
				const Result first = Render(gpu, fixture.Renderer(), Greys(codes, 64), c);
				const auto firstGot = Columns(first, static_cast<int>(codes.size()), 0.5, 64);
				report += "first carry frame after SSIM without it: " + Join(firstGot) + "\n";
				check(firstGot, "the first carry frame");
				{
					Case plain;
					plain.scale = 0.5;
					plain.mitchell = true;
					plain.extraHooks = { ssim };
					const auto ssimGot = Columns(Render(gpu, fixture.Renderer(), Greys(codes, 64), plain),
						static_cast<int>(codes.size()), 0.5, 64);
					report += "SSIM alone with the carry: " + Join(ssimGot) + "\n";
					check(ssimGot, "SSIM with the carry");
				}
				module.SetFrame(noGrain, ++sequence, 1.0f / 24.0f);
				const Result r = Render(gpu, fixture.Renderer(), Greys(codes, 64), c);
				Assert::IsTrue(r.ok);
				const auto got = Columns(r, static_cast<int>(codes.size()), 0.5, 64);
				report += "carry through the blend: " + Join(got) + "\n";
				check(got, "the blend");
			}
			fixture.CaptureLog(nullptr);
			Logger::WriteMessage(report.c_str());
			for (pl_tex* t : { &scene, &grainy, &clean, &dark, &brighter })
				pl_tex_destroy(gpu, t);
			module.Release();
			pl_mpv_user_shader_destroy(&ssim);
			// Only shader-compiler timing notes may appear (a loaded machine).
			std::string remaining;
			std::istringstream lines(log);
			for (std::string entry; std::getline(lines, entry);)
				if ((entry.find("translating HLSL to DXBC (slow!)") == std::string::npos && entry.find("translating GLSL to SPIR-V (slow!)") == std::string::npos))
					remaining += entry + "\n";
			Assert::IsTrue(remaining.empty(), ExtendedRangeGpuProbe::ToWide("libplacebo logged: " + remaining).c_str());
			Assert::AreEqual(0, pl_renderer_get_errors(fixture.Renderer()).num_disabled_hooks, L"A hook was disabled");
		}

		// Audit stage8-rs232 note: the owner's chain NLS (scope profile) +
		// Stage 10 + SSIM. NLS hooks the picture at RGB after the carry (so it
		// warps the offset in alpha with the picture), SSIM at POST_KERNEL (the
		// texture restore). With the carry on, the same chain keeps in-range
		// codes as with it off and keeps below black / above white; no hook is
		// disabled, on the first frame and after.
		TEST_METHOD(ExtendedRangeKeepsBtbThroughNlsAndSsim)
		{
			using namespace ExtendedRangeGpuProbe;
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create());
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			const pl_hook* nls = ParseBundledNlsShader(gpu, "NLS.glsl", 0.0);
			BindNlsShader(nls, 1.32f, 0.0f);
			const std::string source = LoadBundledShader("SSimDownscaler.glsl");
			const pl_hook* ssim = pl_mpv_user_shader_parse(gpu, source.data(), source.size());
			Assert::IsNotNull(ssim);
			const std::vector<int> codes = { 4, 16, 40, 64, 100, 502, 900, 940, 980, 1019 };
			Case c;
			c.scale = 0.5;
			c.mitchell = true;
			c.extraHooks = { nls, ssim };
			Case off = c;
			off.on = false;
			std::string report;
			for (int frame = 0; frame < 2; ++frame)
			{
				const Result on = Render(gpu, fixture.Renderer(), Greys(codes, 64), c);
				const Result plain = Render(gpu, fixture.Renderer(), Greys(codes, 64), off);
				Assert::IsTrue(on.ok && plain.ok);
				const auto got = Columns(on, static_cast<int>(codes.size()), 0.5, 64);
				const auto ref = Columns(plain, static_cast<int>(codes.size()), 0.5, 64);
				report += "frame " + std::to_string(frame) + " on:  " + Join(got) + "\n";
				report += "frame " + std::to_string(frame) + " off: " + Join(ref) + "\n";
				for (size_t i = 0; i < codes.size(); ++i)
				{
					if (codes[i] >= 64 && codes[i] <= 940)
						Assert::IsTrue(std::abs(got[i] - ref[i]) <= 2, ToWide("in-range changed\n" + report).c_str());
					else if (codes[i] < 64)
						Assert::IsTrue(got[i] <= ref[i] - 8, ToWide("below black lost\n" + report).c_str());
					else
						// Kept (the plain chain keeps it here too: NLS's float texture).
						Assert::IsTrue(got[i] > 940 && got[i] >= ref[i] - 2, ToWide("above white lost\n" + report).c_str());
				}
				Assert::IsTrue(on.restores > 0, ToWide("the restore did not run\n" + report).c_str());
			}
			fixture.CaptureLog(nullptr);
			Logger::WriteMessage(report.c_str());
			pl_mpv_user_shader_destroy(&ssim);
			pl_mpv_user_shader_destroy(&nls);
			std::string remaining;
			std::istringstream lines(log);
			for (std::string entry; std::getline(lines, entry);)
				if ((entry.find("translating HLSL to DXBC (slow!)") == std::string::npos && entry.find("translating GLSL to SPIR-V (slow!)") == std::string::npos))
					remaining += entry + "\n";
			Assert::IsTrue(remaining.empty(), ToWide("libplacebo logged: " + remaining).c_str());
			Assert::AreEqual(0, pl_renderer_get_errors(fixture.Renderer()).num_disabled_hooks, L"A hook was disabled");
		}

		// Stage 8-3: the matcher's high-passed temporal error and the grain map.
		// A textured picture panned (8, 4) source pixels per frame; on the right
		// half of the screen every frame adds fresh noise (grain, sigma 3
		// codes). The error is low on the clean texture and ~0.56 sigma on the
		// noise; a small brightness change (a fade) does not count as grain.
		// The map then leans faithful on the noise (the grain floor) and keeps
		// SSIM on the texture, and falls back to the scalar without a field.
		static uint16_t GrainNoise(int x, int y, unsigned frame)
		{
			unsigned h = static_cast<unsigned>(x) * 73856093u ^ static_cast<unsigned>(y) * 19349663u ^ (frame + 1u) * 83492791u;
			h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
			// Uniform +-5.2 codes (sigma 3), in 16-bit units.
			const double u = (static_cast<double>(h & 0xFFFFu) / 65535.0 - 0.5) * 2.0;
			return static_cast<uint16_t>(std::lround(32768.0 + u * 5.2 * 257.0));
		}

		static DejudderFrame GrainFrame(int width, int height, int frame, int dx, int dy, int fade)
		{
			return MakeDejudderFrame(width, height,
				[=](int x, int y)
				{
					int v = DejudderTexture(x - frame * dx, y - frame * dy) + fade * 257;
					if (x >= width / 2)
						v += static_cast<int>(GrainNoise(x, y, static_cast<unsigned>(frame))) - 32768;
					return static_cast<uint16_t>(std::min(65535, std::max(0, v)));
				},
				[](int, int, int) { return static_cast<uint16_t>(32768); });
		}

		// ---- Refinements D1-D5 (docs/VP-build-plan-refinements.md and the
		// planner's answers): each is checked under both warp paths
		// (`dejudder_warp_stage` source and rendered).

		static float HalfBits(uint16_t h)
		{
			const int sign = (h >> 15) & 1, exponent = (h >> 10) & 31, mantissa = h & 1023;
			const float value = exponent == 0 ? std::ldexp(static_cast<float>(mantissa), -24) :
				exponent == 31 ? (mantissa ? NAN : INFINITY) : std::ldexp(static_cast<float>(mantissa | 1024), exponent - 25);
			return sign ? -value : value;
		}

		// FrameTrust() as (share, strength scale, mean |v|).
		static std::array<float, 3> DownloadFrameTrust(pl_gpu gpu, const Dejudder::MotionWarp& warp)
		{
			uint16_t texel[4] = {};
			pl_tex_transfer_params download{};
			download.tex = warp.FrameTrust();
			download.ptr = texel;
			Assert::IsTrue(pl_tex_download(gpu, &download), L"FrameTrust is not host-readable");
			return { HalfBits(texel[0]), HalfBits(texel[1]), HalfBits(texel[2]) };
		}

		// A's luma warped by `shift` through both paths, in 16-bit codes: the
		// source warp (Warp) and the rendered warp (6d) of A's luma as the
		// cached picture, 1:1.
		struct BothPaths
		{
			std::vector<float> source, rendered;
		};

		static BothPaths WarpBothPaths(TargetLutGpuFixture& fixture, Dejudder::MotionWarp& warp,
			const DejudderFrame& a, const Dejudder::Planes& planesA, float shift)
		{
			pl_gpu gpu = fixture.Gpu();
			BothPaths result;
			Dejudder::Planes out;
			Assert::IsTrue(warp.Warp(planesA, shift, out), ExtendedRangeGpuProbe::ToWide(warp.LastError()).c_str());
			const std::vector<uint16_t> luma = DownloadPlane(gpu, out.luma);
			result.source.assign(luma.begin(), luma.end());

			std::vector<float> texels(a.luma.size() * 4);
			for (size_t i = 0; i < a.luma.size(); ++i)
			{
				const float v = a.luma[i] / 65535.0f;
				texels[i * 4] = texels[i * 4 + 1] = texels[i * 4 + 2] = v;
				texels[i * 4 + 3] = 1.0f;
			}
			pl_tex_params cp{};
			cp.w = a.width;
			cp.h = a.height;
			cp.format = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32, PL_FMT_CAP_SAMPLEABLE);
			cp.sampleable = true;
			cp.initial_data = texels.data();
			pl_tex cached = cp.format ? pl_tex_create(gpu, &cp) : nullptr;
			Assert::IsNotNull(cached);
			DejudderRendered::Warp rendered(fixture.Log(), gpu, true);
			DejudderRendered::Geometry g;
			g.sourceCrop = { 0.0f, 0.0f, static_cast<float>(a.width), static_cast<float>(a.height) };
			g.targetCrop = g.sourceCrop;
			pl_tex warped = nullptr;
			const bool ran = rendered.Run(cached, warp.Field(), warp.FrameTrust(), warp.GetSettings().minTrustedShare,
				shift, g, warped, warp.SubField(), &warp.GetSettings());
			if (!ran)
				pl_tex_destroy(gpu, &cached);
			Assert::IsTrue(ran, ExtendedRangeGpuProbe::ToWide(rendered.Error()).c_str());
			pl_gpu_finish(gpu);
			std::vector<uint16_t> half(a.luma.size() * 4);
			pl_tex_transfer_params download{};
			download.tex = warped;
			download.ptr = half.data();
			const bool downloaded = pl_tex_download(gpu, &download);
			pl_tex_destroy(gpu, &cached);
			Assert::IsTrue(downloaded, L"the rendered warp is not host-readable");
			result.rendered.resize(a.luma.size());
			for (size_t i = 0; i < a.luma.size(); ++i)
				result.rendered[i] = HalfBits(half[i * 4]) * 65535.0f;
			return result;
		}

		// Percentage of the region [x0, x1) x [y0, y1) within `tolerance`
		// codes of expected(x, y).
		template <typename Expected>
		static double MatchingPercent(const std::vector<float>& image, int width, int x0, int y0, int x1, int y1,
			Expected expected, float tolerance)
		{
			int total = 0, good = 0;
			for (int y = y0; y < y1; ++y)
				for (int x = x0; x < x1; ++x)
				{
					++total;
					if (std::fabs(image[static_cast<size_t>(y) * width + x] - static_cast<float>(expected(x, y))) <= tolerance)
						++good;
				}
			return total ? 100.0 * good / total : 0.0;
		}

		// The rendered path stores rgba16f: about 1/2048 of the value.
		static constexpr float kSourceTolerance = 2.0f, kRenderedTolerance = 48.0f;

		TEST_METHOD(DejudderGlobalPriorFindsAPanBeyondTheCoarseRange)
		{
			// D1: a pan of (140, -20) source px per frame is beyond the coarse
			// search (about +-126 px with the refinements): without the prior the
			// blocks miss it; with it they are within half a pixel, and both warps
			// move the picture by shift x the pan.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			const int width = 512, height = 256;
			static constexpr int dx = 140, dy = -20;
			auto chroma = [](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); };
			const DejudderFrame a = MakeDejudderFrame(width, height, [](int x, int y) { return DejudderTexture(x, y); }, chroma);
			const DejudderFrame b = MakeDejudderFrame(width, height,
				[](int x, int y) { return DejudderTexture(x - dx, y - dy); },
				[](int x, int y, int c) { return DejudderTexture((x - dx / 2) * 2, y - dy, 7 + c); });
			DejudderGpuPlanes planesA, planesB;
			UploadDejudderFrame(gpu, a, planesA);
			UploadDejudderFrame(gpu, b, planesB);
			// Blocks whose content stays in the picture in both frames.
			std::wstring misses;
			auto countRight = [&](const DejudderField& field)
			{
				int total = 0, right = 0;
				misses.clear();
				for (int by = 3; by < field.blocksY - 3; ++by)
					for (int bx = 1; bx * 16 + 16 + dx < width - 16; ++bx)
					{
						++total;
						const bool ok = field.trusted(bx, by) && std::fabs(field.vx(bx, by) - dx) <= 0.5f &&
							std::fabs(field.vy(bx, by) - dy) <= 0.5f;
						right += ok ? 1 : 0;
						if (!ok && misses.size() < 600)
							misses += L" (" + std::to_wstring(bx) + L"," + std::to_wstring(by) + L": " +
								std::to_wstring(field.vx(bx, by)) + L"," + std::to_wstring(field.vy(bx, by)) +
								(field.trusted(bx, by) ? L" t)" : L" u)");
					}
				return std::make_pair(right, total);
			};
			Dejudder::Settings settings = DejudderTestSettings();
			Dejudder::MotionWarp without(fixture.Log(), gpu, settings);
			Assert::IsTrue(without.Estimate(planesA.planes, planesB.planes));
			const auto off = countRight(DownloadField(gpu, without));
			settings.globalPrior = true;
			Dejudder::MotionWarp with(fixture.Log(), gpu, settings);
			Assert::IsTrue(with.Estimate(planesA.planes, planesB.planes), ExtendedRangeGpuProbe::ToWide(with.LastError()).c_str());
			const auto on = countRight(DownloadField(gpu, with));
			const DejudderField global = DownloadField(gpu, with, with.GlobalVector());
			const std::wstring message = L"right blocks without the prior " + std::to_wstring(off.first) + L", with " +
				std::to_wstring(on.first) + L" of " + std::to_wstring(on.second) + L"; global (1/8 texels) " +
				std::to_wstring(global.texels[0]) + L", " + std::to_wstring(global.texels[1]) + L" ratio " +
				std::to_wstring(global.texels[2]) + L"; misses:" + misses + L"; log: " + std::wstring(log.begin(), log.end());
			Assert::IsTrue(off.first * 100 < off.second * 20, (L"the pan should be beyond the plain search: " + message).c_str());
			Assert::IsTrue(on.first * 100 >= on.second * 95, message.c_str());
			Assert::IsTrue(std::fabs(global.texels[0] - dx / 8.0f) <= 0.5f && std::fabs(global.texels[1] - dy / 8.0f) <= 0.5f &&
				global.texels[2] < 0.9f, message.c_str());

			// shift 0.5: (70, -10), both paths, on blocks whose content stays in the
			// picture (from x 352 on it leaves at 140 px per frame: those hold).
			const BothPaths warped = WarpBothPaths(fixture, with, a, planesA.planes, 0.5f);
			auto expected = [](int x, int y) { return DejudderTexture(x - 70, y + 10); };
			const double sourceOk = MatchingPercent(warped.source, width, 96, 40, 340, height - 40, expected, kSourceTolerance);
			const double renderedOk = MatchingPercent(warped.rendered, width, 96, 40, 340, height - 40, expected, kRenderedTolerance);
			Assert::IsTrue(sourceOk >= 90.0 && renderedOk >= 90.0, (L"warped by (70, -10): source " +
				std::to_wstring(sourceOk) + L"%, rendered " + std::to_wstring(renderedOk) + L"%; " + message).c_str());
			AssertCleanLibplaceboLog(log);
		}

		TEST_METHOD(DejudderMotionScaledStrength)
		{
			// D2: a static scene gives scale 0; a pan of 8 px with lo 0 / hi 16
			// gives 0.5, and both warps then move the picture by shift x 0.5 x 8.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			const int width = 256, height = 128;
			auto chroma = [](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); };
			const DejudderFrame a = MakeDejudderFrame(width, height, [](int x, int y) { return DejudderTexture(x, y); }, chroma);
			const DejudderFrame pan = MakeDejudderFrame(width, height,
				[](int x, int y) { return DejudderTexture(x - 8, y); },
				[](int x, int y, int c) { return DejudderTexture((x - 4) * 2, y, 7 + c); });
			DejudderGpuPlanes planesA, planesPan;
			UploadDejudderFrame(gpu, a, planesA);
			UploadDejudderFrame(gpu, pan, planesPan);
			Dejudder::Settings settings = DejudderTestSettings();
			settings.motionScaleLo = 0.0f;
			settings.motionScaleHi = 16.0f;
			settings.strengthRate = 1.0f;   // the rate limit has its own test
			{
				Dejudder::MotionWarp warp(fixture.Log(), gpu, settings);
				Assert::IsTrue(warp.Estimate(planesA.planes, planesA.planes));
				const auto trust = DownloadFrameTrust(gpu, warp);
				Assert::IsTrue(trust[0] > 0.9f && trust[1] == 0.0f,
					(L"static: share " + std::to_wstring(trust[0]) + L", scale " + std::to_wstring(trust[1])).c_str());
			}
			Dejudder::MotionWarp warp(fixture.Log(), gpu, settings);
			Assert::IsTrue(warp.Estimate(planesA.planes, planesPan.planes));
			const auto trust = DownloadFrameTrust(gpu, warp);
			Assert::IsTrue(std::fabs(trust[2] - 8.0f) < 0.3f && std::fabs(trust[1] - 0.5f) < 0.02f,
				(L"pan 8: mean |v| " + std::to_wstring(trust[2]) + L", scale " + std::to_wstring(trust[1])).c_str());
			// shift = 0.5 / scale: the warps apply the GPU's scale, so the picture
			// lands exactly 4 px on (8 px if the scale were ignored).
			const BothPaths warped = WarpBothPaths(fixture, warp, a, planesA.planes, 0.5f / trust[1]);
			auto expected = [](int x, int y) { return DejudderTexture(x - 4, y); };
			const double sourceOk = MatchingPercent(warped.source, width, 24, 24, width - 24, height - 24, expected, kSourceTolerance);
			const double renderedOk = MatchingPercent(warped.rendered, width, 24, 24, width - 24, height - 24, expected, kRenderedTolerance);
			auto unscaled = [](int x, int y) { return DejudderTexture(x - 8, y); };
			const double sourceUnscaled = MatchingPercent(warped.source, width, 24, 24, width - 24, height - 24, unscaled, kSourceTolerance);
			Assert::IsTrue(sourceOk >= 90.0 && renderedOk >= 90.0 && sourceUnscaled < 50.0, (L"shift x scale 0.5 = 4 px: source " +
				std::to_wstring(sourceOk) + L"%, rendered " + std::to_wstring(renderedOk) + L"% (unscaled 8 px " +
				std::to_wstring(sourceUnscaled) + L"%)").c_str());
			AssertCleanLibplaceboLog(log);
		}

		TEST_METHOD(DejudderTrustAdaptiveStrengthAndRateLimit)
		{
			// D4: half the picture cut (salt-99) gives a share between the hold
			// and full strength, so the scale is between 0 and 1 and both warps
			// move the trusted half by shift x scale; a full cut holds (share
			// below the minimum, scale 0). Then the rate limit: static -> pan
			// with D2 ramps by 0.25 per source frame.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			const int width = 256, height = 128;
			static constexpr int dx = 12, dy = 6;
			auto chroma = [](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); };
			const DejudderFrame a = MakeDejudderFrame(width, height, [](int x, int y) { return DejudderTexture(x, y); }, chroma);
			const DejudderFrame half = MakeDejudderFrame(width, height,
				[](int x, int y) { return x < 128 ? DejudderTexture(x, y, 99) : DejudderTexture(x - dx, y - dy); },
				[](int x, int y, int c) { return x < 64 ? DejudderTexture(x * 2, y, 99 + c) : DejudderTexture((x - dx / 2) * 2, y - dy, 7 + c); });
			const DejudderFrame cut = MakeDejudderFrame(width, height, [](int x, int y) { return DejudderTexture(x, y, 99); }, chroma);
			DejudderGpuPlanes planesA, planesHalf, planesCut;
			UploadDejudderFrame(gpu, a, planesA);
			UploadDejudderFrame(gpu, half, planesHalf);
			UploadDejudderFrame(gpu, cut, planesCut);
			Dejudder::Settings settings = DejudderTestSettings();
			settings.trustScaleFull = 0.9f;
			settings.strengthRate = 1.0f;
			{
				Dejudder::MotionWarp warp(fixture.Log(), gpu, settings);
				Assert::IsTrue(warp.Estimate(planesA.planes, planesHalf.planes));
				const auto trust = DownloadFrameTrust(gpu, warp);
				const float expectedScale = (trust[0] - 0.25f) / (0.9f - 0.25f);
				Assert::IsTrue(trust[0] > 0.3f && trust[0] < 0.6f && std::fabs(trust[1] - expectedScale) < 0.01f &&
					trust[1] > 0.05f && trust[1] < 0.95f,
					(L"half cut: share " + std::to_wstring(trust[0]) + L", scale " + std::to_wstring(trust[1])).c_str());
				// The trusted (right) half moves by shift x scale x (12, 6): with
				// shift = 1 / scale it lands at the full motion.
				const BothPaths warped = WarpBothPaths(fixture, warp, a, planesA.planes, 1.0f / trust[1]);
				auto expected = [](int x, int y) { return DejudderTexture(x - dx, y - dy); };
				const double sourceOk = MatchingPercent(warped.source, width, 160, 24, width - 24, height - 24, expected, kSourceTolerance);
				const double renderedOk = MatchingPercent(warped.rendered, width, 160, 24, width - 24, height - 24, expected, kRenderedTolerance);
				Assert::IsTrue(sourceOk >= 85.0 && renderedOk >= 85.0, (L"half cut, trusted half: source " +
					std::to_wstring(sourceOk) + L"%, rendered " + std::to_wstring(renderedOk) + L"%").c_str());
				Assert::IsTrue(warp.Estimate(planesA.planes, planesCut.planes));
				const auto cutTrust = DownloadFrameTrust(gpu, warp);
				Assert::IsTrue(cutTrust[0] < 0.25f && cutTrust[1] == 0.0f,
					(L"full cut: share " + std::to_wstring(cutTrust[0]) + L", scale " + std::to_wstring(cutTrust[1])).c_str());
				const BothPaths held = WarpBothPaths(fixture, warp, a, planesA.planes, 0.5f);
				auto own = [](int x, int y) { return DejudderTexture(x, y); };
				Assert::IsTrue(MatchingPercent(held.source, width, 0, 0, width, height, own, kSourceTolerance) == 100.0 &&
					MatchingPercent(held.rendered, width, 0, 0, width, height, own, kRenderedTolerance) == 100.0,
					L"a full cut holds A in both paths");
			}
			// The rate limit (D2 on: static, then three pan frames).
			Dejudder::Settings ramp = DejudderTestSettings();
			ramp.motionScaleLo = 0.25f;
			ramp.motionScaleHi = 1.5f;
			ramp.strengthRate = 0.25f;
			const DejudderFrame b = MakeDejudderFrame(width, height,
				[](int x, int y) { return DejudderTexture(x - dx, y - dy); },
				[](int x, int y, int c) { return DejudderTexture((x - dx / 2) * 2, y - dy, 7 + c); });
			DejudderGpuPlanes planesB;
			UploadDejudderFrame(gpu, b, planesB);
			Dejudder::MotionWarp warp(fixture.Log(), gpu, ramp);
			std::wstring sequence;
			float scales[4] = {};
			for (int frame = 0; frame < 4; ++frame)
			{
				Assert::IsTrue(warp.Estimate(planesA.planes, frame == 0 ? planesA.planes : planesB.planes));
				scales[frame] = DownloadFrameTrust(gpu, warp)[1];
				sequence += std::to_wstring(scales[frame]) + L" ";
			}
			Assert::IsTrue(scales[0] == 0.0f && std::fabs(scales[1] - 0.25f) < 0.01f && std::fabs(scales[2] - 0.5f) < 0.01f &&
				std::fabs(scales[3] - 0.75f) < 0.01f, (L"static then pan: " + sequence).c_str());
			AssertCleanLibplaceboLog(log);
		}

		static uint16_t FloatToHalf(float f)
		{
			if (f == 0.0f)
				return 0;
			int exponent;
			const float mantissa = std::frexp(std::fabs(f), &exponent);   // [0.5, 1)
			int e = exponent + 14;
			const uint16_t sign = f < 0.0f ? 0x8000 : 0;
			if (e <= 0)
				return sign;
			int m = static_cast<int>(std::lround((mantissa * 2.0f - 1.0f) * 1024.0f));
			if (m == 1024) { m = 0; ++e; }
			return static_cast<uint16_t>(sign | (e << 10) | m);
		}

		TEST_METHOD(DejudderVectorSmoothing)
		{
			// D3 on constructed fields over real planes (A: a pan of (12, 6)
			// with a 48x48 object that is gone in B). A trusted isolated outlier
			// cannot be made from real frames (the search window's margin breaks
			// its neighbours' trust too), so the raw field is built from the
			// real one and passed to Smooth():
			// - block (2, 2) set to a trusted (20, 6): replaced by the median (12, 6);
			// - block (12, 5) set untrusted; its content matches at (12, 6): promoted;
			// - the object's centre block (7, 3) untrusted with its 8 neighbours
			//   set to a trusted (12, 6): its own content (the object) does not
			//   match there (planner answer 4), so it stays held.
			// Then the whole Estimate with D3 on keeps the pan, and both warps
			// move the former outlier with the pan.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			const int width = 256, height = 128;
			static constexpr int dx = 12, dy = 6, ox = 96, oy = 32;
			auto inA = [](int x, int y) { return x >= ox && x < ox + 48 && y >= oy && y < oy + 48; };
			auto chroma = [](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); };
			const DejudderFrame a = MakeDejudderFrame(width, height,
				[&](int x, int y) { return inA(x, y) ? DejudderTexture(x, y, 55) : DejudderTexture(x, y); }, chroma);
			const DejudderFrame b = MakeDejudderFrame(width, height,
				[](int x, int y) { return DejudderTexture(x - dx, y - dy); },
				[](int x, int y, int c) { return DejudderTexture((x - dx / 2) * 2, y - dy, 7 + c); });
			DejudderGpuPlanes planesA, planesB;
			UploadDejudderFrame(gpu, a, planesA);
			UploadDejudderFrame(gpu, b, planesB);
			Dejudder::Settings settings = DejudderTestSettings();
			Dejudder::MotionWarp warp(fixture.Log(), gpu, settings);
			Assert::IsTrue(warp.Estimate(planesA.planes, planesB.planes));
			const DejudderField real = DownloadField(gpu, warp);
			Assert::IsTrue(real.trusted(2, 2) && real.trusted(12, 5) && !real.trusted(7, 3),
				L"the real field: (2, 2) and (12, 5) trusted, the object's centre held");

			std::vector<float> texels = real.texels;
			auto set = [&](int bx, int by, float vx, float vy, float trust)
			{
				float* t = &texels[(by * real.blocksX + bx) * 4];
				t[0] = vx; t[1] = vy; t[2] = trust;
			};
			set(2, 2, 20.0f, 6.0f, 1.0f);
			set(12, 5, 0.0f, 0.0f, 0.0f);
			for (int by = 2; by <= 4; ++by)
				for (int bx = 6; bx <= 8; ++bx)
					if (bx != 7 || by != 3)
						set(bx, by, static_cast<float>(dx), static_cast<float>(dy), 1.0f);
			std::vector<uint16_t> halfs(texels.size());
			for (size_t i = 0; i < texels.size(); ++i)
				halfs[i] = FloatToHalf(texels[i]);
			pl_tex_params rp{};
			rp.w = real.blocksX;
			rp.h = real.blocksY;
			rp.format = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 16, 16, PL_FMT_CAP_SAMPLEABLE);
			rp.sampleable = true;
			rp.initial_data = halfs.data();
			pl_tex raw = rp.format ? pl_tex_create(gpu, &rp) : nullptr;
			Assert::IsNotNull(raw);
			const bool smoothed = warp.Smooth(raw);
			pl_tex_destroy(gpu, &raw);
			Assert::IsTrue(smoothed, ExtendedRangeGpuProbe::ToWide(warp.LastError()).c_str());
			const DejudderField after = DownloadField(gpu, warp);
			auto describe = [&](int bx, int by)
			{
				return L"(" + std::to_wstring(after.vx(bx, by)) + L", " + std::to_wstring(after.vy(bx, by)) +
					(after.trusted(bx, by) ? L", trusted)" : L", held)");
			};
			const std::wstring message = L"outlier " + describe(2, 2) + L"; promoted " + describe(12, 5) +
				L"; object centre " + describe(7, 3);
			auto isPan = [&](int bx, int by)
			{
				return after.trusted(bx, by) && std::fabs(after.vx(bx, by) - dx) < 0.6f && std::fabs(after.vy(bx, by) - dy) < 0.6f;
			};
			Assert::IsTrue(isPan(2, 2), (L"the outlier takes the median: " + message).c_str());
			Assert::IsTrue(isPan(12, 5), (L"a matching block is promoted: " + message).c_str());
			Assert::IsFalse(after.trusted(7, 3), (L"new content is not promoted: " + message).c_str());

			// Both warps move the former outlier block with the pan (shift 0.5).
			const BothPaths warped = WarpBothPaths(fixture, warp, a, planesA.planes, 0.5f);
			auto expected = [](int x, int y) { return DejudderTexture(x - 6, y - 3); };
			const double sourceOk = MatchingPercent(warped.source, width, 40, 38, 52, 50, expected, kSourceTolerance);
			const double renderedOk = MatchingPercent(warped.rendered, width, 40, 38, 52, 50, expected, kRenderedTolerance);
			Assert::IsTrue(sourceOk >= 90.0 && renderedOk >= 90.0, (L"the former outlier warped with the pan: source " +
				std::to_wstring(sourceOk) + L"%, rendered " + std::to_wstring(renderedOk) + L"%").c_str());

			// The whole Estimate with D3 on: the pan unchanged away from the object.
			settings.vectorSmooth = true;
			Dejudder::MotionWarp smooth(fixture.Log(), gpu, settings);
			Assert::IsTrue(smooth.Estimate(planesA.planes, planesB.planes), ExtendedRangeGpuProbe::ToWide(smooth.LastError()).c_str());
			const DejudderField full = DownloadField(gpu, smooth);
			int interior = 0, right = 0;
			for (int by = 2; by < full.blocksY - 2; ++by)
				for (int bx = 2; bx < full.blocksX - 2; ++bx)
				{
					if (bx >= 5 && bx <= 9 && by >= 1 && by <= 5)
						continue;   // the object and its margin
					++interior;
					right += (full.trusted(bx, by) && std::fabs(full.vx(bx, by) - dx) < 0.6f && std::fabs(full.vy(bx, by) - dy) < 0.6f) ? 1 : 0;
				}
			Assert::IsTrue(right * 100 >= interior * 95, (L"the pan with D3 on: " + std::to_wstring(right) + L" of " +
				std::to_wstring(interior)).c_str());
			Assert::IsFalse(full.trusted(7, 3), L"the object's centre stays held with D3 on");
			AssertCleanLibplaceboLog(log);
		}

		TEST_METHOD(DejudderOcclusionFillFromB)
		{
			// D5 (source warp): an object in A that is gone in B leaves its
			// place as new content in B. With the fill on, the held place shows
			// B there; off (and in the rendered path, not wired yet) it holds A.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			const int width = 256, height = 128;
			static constexpr int dx = 12, dy = 6, ox = 96, oy = 32;
			auto inA = [](int x, int y) { return x >= ox && x < ox + 48 && y >= oy && y < oy + 48; };
			auto chroma = [](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); };
			const DejudderFrame a = MakeDejudderFrame(width, height,
				[&](int x, int y) { return inA(x, y) ? DejudderTexture(x, y, 55) : DejudderTexture(x, y); }, chroma);
			const DejudderFrame b = MakeDejudderFrame(width, height,
				[](int x, int y) { return DejudderTexture(x - dx, y - dy); },
				[](int x, int y, int c) { return DejudderTexture((x - dx / 2) * 2, y - dy, 7 + c); });
			DejudderGpuPlanes planesA, planesB;
			UploadDejudderFrame(gpu, a, planesA);
			UploadDejudderFrame(gpu, b, planesB);
			auto inner = [&](const std::vector<float>& image, bool fromB)
			{
				// The object's interior, away from its edges (blocks of 16).
				return MatchingPercent(image, width, ox + 16, oy + 16, ox + 32, oy + 32,
					[&](int x, int y) { return fromB ? b.luma[y * width + x] : a.luma[y * width + x]; },
					kSourceTolerance + 1.0f);
			};
			Dejudder::Settings settings = DejudderTestSettings();
			Dejudder::MotionWarp off(fixture.Log(), gpu, settings);
			Assert::IsTrue(off.Estimate(planesA.planes, planesB.planes));
			const BothPaths held = WarpBothPaths(fixture, off, a, planesA.planes, 0.5f);
			settings.occlusionFill = true;
			Dejudder::MotionWarp on(fixture.Log(), gpu, settings);
			Assert::IsTrue(on.Estimate(planesA.planes, planesB.planes), ExtendedRangeGpuProbe::ToWide(on.LastError()).c_str());
			const BothPaths filled = WarpBothPaths(fixture, on, a, planesA.planes, 0.5f);
			const double heldA = inner(held.source, false), filledB = inner(filled.source, true);
			Assert::IsTrue(heldA >= 95.0 && filledB >= 95.0, (L"object place: off holds A " + std::to_wstring(heldA) +
				L"%, on shows B " + std::to_wstring(filledB) + L"%").c_str());
			AssertCleanLibplaceboLog(log);
		}

		// ---- Stage 11a, estimator-lite (docs/VP-stage11a-estimator-lite.md).

		static int16_t NoiseCode(int x, int y, unsigned seed, int amplitude)
		{
			const unsigned h = (static_cast<unsigned>(x) * 73856093u) ^ (static_cast<unsigned>(y) * 19349663u) ^
				(seed * 83492791u);
			return static_cast<int16_t>(static_cast<int>((h * 2654435761u >> 16) % static_cast<unsigned>(2 * amplitude + 1)) - amplitude);
		}

		static pl_tex UploadVectors(pl_gpu gpu, const std::vector<float>& texels, int w, int h)
		{
			std::vector<uint16_t> halfs(texels.size());
			for (size_t i = 0; i < texels.size(); ++i)
				halfs[i] = FloatToHalf(texels[i]);
			pl_tex_params tp{};
			tp.w = w;
			tp.h = h;
			tp.format = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 16, 16, PL_FMT_CAP_SAMPLEABLE);
			tp.sampleable = true;
			tp.initial_data = halfs.data();
			return tp.format ? pl_tex_create(gpu, &tp) : nullptr;
		}

		TEST_METHOD(EstimatorNeighbourCandidatesBeatATextureOutlier)
		{
			// E1: block (5, 3)'s true landing in B carries noise (still a good
			// match), and an exact copy of its window sits 44 px further. With a
			// first sweep that found the copy, the second sweep keeps the copy at
			// lambda 0 (lower SAD) and takes the neighbours' pan at lambda 0.5.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			const int width = 256, height = 128;
			static constexpr int dx = 12, dy = 6, bx = 5, by = 3, wx = 44;
			auto chroma = [](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); };
			const DejudderFrame a = MakeDejudderFrame(width, height, [](int x, int y) { return DejudderTexture(x, y); }, chroma);
			DejudderFrame b = MakeDejudderFrame(width, height,
				[](int x, int y) { return DejudderTexture(x - dx, y - dy); },
				[](int x, int y, int c) { return DejudderTexture((x - dx / 2) * 2, y - dy, 7 + c); });
			// The block's window (16 + 2 x 4 source px) at its true landing: noise.
			for (int y = by * 16 - 4 + dy; y < by * 16 + 20 + dy; ++y)
				for (int x = bx * 16 - 4 + dx; x < bx * 16 + 20 + dx; ++x)
					b.luma[static_cast<size_t>(y) * width + x] = static_cast<uint16_t>(
						b.luma[static_cast<size_t>(y) * width + x] + NoiseCode(x, y, 3, 512));
			// An exact copy of A's window, 44 px right of the true landing.
			for (int y = by * 16 - 4; y < by * 16 + 20; ++y)
				for (int x = bx * 16 - 4; x < bx * 16 + 20; ++x)
					b.luma[static_cast<size_t>(y + dy) * width + x + dx + wx] = a.luma[static_cast<size_t>(y) * width + x];
			DejudderGpuPlanes planesA, planesB;
			UploadDejudderFrame(gpu, a, planesA);
			UploadDejudderFrame(gpu, b, planesB);
			Dejudder::Settings settings = DejudderTestSettings();
			Dejudder::MotionWarp warp(fixture.Log(), gpu, settings);
			Assert::IsTrue(warp.Estimate(planesA.planes, planesB.planes));
			DejudderField first = DownloadField(gpu, warp, warp.Forward());
			Assert::IsTrue(std::fabs(first.vx(bx - 1, by) - dx * 0.5f) < 0.6f && std::fabs(first.vx(bx + 1, by) - dx * 0.5f) < 0.6f,
				L"the neighbours should have found the pan");
			// The first sweep as if it had found the copy.
			float* t = &first.texels[(by * first.blocksX + bx) * 4];
			t[0] = (dx + wx) * 0.5f;
			t[1] = dy * 0.5f;
			pl_tex sweep = UploadVectors(gpu, first.texels, first.blocksX, first.blocksY);
			Assert::IsNotNull(sweep);
			auto second = [&](float lambda)
			{
				Dejudder::Settings s2 = settings;
				s2.estLambda = lambda;
				warp.SetSettings(s2);
				Assert::IsTrue(warp.SecondSweep(sweep), ExtendedRangeGpuProbe::ToWide(warp.LastError()).c_str());
				const DejudderField f = DownloadField(gpu, warp, warp.Forward());
				return std::make_pair(f.vx(bx, by) * 2.0f, f.vy(bx, by) * 2.0f);   // source pixels
			};
			const auto plain = second(0.0f);
			const auto penalised = second(0.5f);
			pl_tex_destroy(gpu, &sweep);
			const std::wstring message = L"lambda 0: (" + std::to_wstring(plain.first) + L", " + std::to_wstring(plain.second) +
				L"); lambda 0.5: (" + std::to_wstring(penalised.first) + L", " + std::to_wstring(penalised.second) + L"); log: " +
				std::wstring(log.begin(), log.end());
			Assert::IsTrue(std::fabs(plain.first - (dx + wx)) <= 1.0f, (L"without the penalty the outlier wins; " + message).c_str());
			Assert::IsTrue(std::fabs(penalised.first - dx) <= 1.0f && std::fabs(penalised.second - dy) <= 1.0f,
				(L"with the penalty the neighbours' pan wins; " + message).c_str());
			AssertCleanLibplaceboLog(log);
		}

		TEST_METHOD(EstimatorQuarterPelRefinesASubPixelPan)
		{
			// E3: a pan of 1.5 source px per frame (B linearly resampled). The
			// half-pel search lands on whole source pixels (1 or 2: 0.5 off);
			// the quarter-pel step reaches 1.5. Both warps then move the picture
			// closer to B at shift 1.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			const int width = 256, height = 128;
			// A smooth, aperiodic texture (several incommensurate periods),
			// shifted analytically: sub-pixel offsets are then exact.
			auto smooth = [](double x, double y, double phase)
			{
				const double k = 6.283185307179586;
				// Gentle enough (a few codes per pixel) that the half-pel result
				// is trusted, so the quarter-pel step applies.
				return static_cast<uint16_t>(std::lround(30000.0 + 1800.0 * std::sin(k * x / 13.0 + phase) +
					1500.0 * std::sin(k * (0.7 * x + y) / 23.0 + 2.0 * phase) + 1500.0 * std::sin(k * (1.3 * y - 0.4 * x) / 37.0) +
					1000.0 * std::sin(k * y / 11.0 + 1.0)));
			};
			const DejudderFrame a = MakeDejudderFrame(width, height, [&](int x, int y) { return smooth(x, y, 0.0); },
				[&](int x, int y, int c) { return smooth(x * 2.0, y, 1.0 + c); });
			const DejudderFrame b = MakeDejudderFrame(width, height, [&](int x, int y) { return smooth(x - 1.5, y, 0.0); },
				[&](int x, int y, int c) { return smooth(x * 2.0 - 1.5, y, 1.0 + c); });
			DejudderGpuPlanes planesA, planesB;
			UploadDejudderFrame(gpu, a, planesA);
			UploadDejudderFrame(gpu, b, planesB);
			std::string lastVectors;
			auto run = [&](bool qpel, double& vectorError, double& sourceError, double& renderedError)
			{
				Dejudder::Settings settings = DejudderTestSettings();
				settings.estQpel = qpel;
				Dejudder::MotionWarp warp(fixture.Log(), gpu, settings);
				Assert::IsTrue(warp.Estimate(planesA.planes, planesB.planes));
				const DejudderField f = DownloadField(gpu, warp);
				double sum = 0.0;
				int n = 0;
				for (int y = 2; y < f.blocksY - 2; ++y)
					for (int x = 2; x < f.blocksX - 2; ++x)
						if (f.trusted(x, y)) { sum += std::fabs(f.vx(x, y) - 1.5) + std::fabs(f.vy(x, y)); ++n; }
				vectorError = n ? sum / n : 1e9;
				std::string vs;
				for (int x = 4; x < 9; ++x)
					vs += std::to_string(f.vx(x, 3)) + "/" + std::to_string(f.trusted(x, 3)) + " ";
				Logger::WriteMessage(vs.c_str());
				lastVectors = vs;
				const BothPaths warped = WarpBothPaths(fixture, warp, a, planesA.planes, 1.0f);
				auto meanAbs = [&](const std::vector<float>& image)
				{
					double e = 0.0;
					int m = 0;
					for (int y = 24; y < height - 24; ++y)
						for (int x = 24; x < width - 24; ++x)
						{
							e += std::fabs(image[static_cast<size_t>(y) * width + x] - b.luma[static_cast<size_t>(y) * width + x]);
							++m;
						}
					return e / m;
				};
				sourceError = meanAbs(warped.source);
				renderedError = meanAbs(warped.rendered);
			};
			double vOff = 0, sOff = 0, rOff = 0, vOn = 0, sOn = 0, rOn = 0;
			run(false, vOff, sOff, rOff);
			const std::string vectorsOff = lastVectors;
			run(true, vOn, sOn, rOn);
			const std::string vectorsOn = lastVectors;
			const std::wstring message = L"vector error (source px) " + std::to_wstring(vOff) + L" -> " + std::to_wstring(vOn) +
				L"; warp error vs B, source path " + std::to_wstring(sOff) + L" -> " + std::to_wstring(sOn) +
				L", rendered path " + std::to_wstring(rOff) + L" -> " + std::to_wstring(rOn) + L" (16-bit codes); vectors off " +
				ExtendedRangeGpuProbe::ToWide(vectorsOff) + L" on " + ExtendedRangeGpuProbe::ToWide(vectorsOn) + L"; log: " +
				std::wstring(log.begin(), log.end());
			Assert::IsTrue(vOff >= 0.35 && vOn <= 0.25, message.c_str());
			Assert::IsTrue(sOn < sOff && rOn < rOff, message.c_str());
			AssertCleanLibplaceboLog(log);
		}

		TEST_METHOD(EstimatorGateKeepsTheFieldOnStillScenes)
		{
			// E4 (with D1): a still pair after a still pair keeps the previous
			// frame's searches (its SADs come back unchanged on new content); a
			// 3 px pan and a cut search again; off searches every time.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			const int width = 256, height = 128;
			auto still = [&](unsigned salt, unsigned noiseSeed, int shift = 0)
			{
				return MakeDejudderFrame(width, height,
					[=](int x, int y) { return static_cast<uint16_t>(DejudderTexture(x - shift, y, salt) + NoiseCode(x, y, noiseSeed, 256)); },
					[=](int x, int y, int c) { return DejudderTexture((x - shift / 2) * 2, y, 7 + c + salt); });
			};
			const DejudderFrame a0 = still(0, 0), a1 = still(0, 1), c0 = still(5, 0), c1 = still(5, 2), c2 = still(5, 0, 3),
				d0 = still(99, 0);
			DejudderGpuPlanes pa0, pa1, pc0, pc1, pc2, pd0;
			UploadDejudderFrame(gpu, a0, pa0);
			UploadDejudderFrame(gpu, a1, pa1);
			UploadDejudderFrame(gpu, c0, pc0);
			UploadDejudderFrame(gpu, c1, pc1);
			UploadDejudderFrame(gpu, c2, pc2);
			UploadDejudderFrame(gpu, d0, pd0);
			auto sads = [&](Dejudder::MotionWarp& warp)
			{
				const DejudderField f = DownloadField(gpu, warp, warp.Forward());
				std::vector<float> z;
				for (int i = 0; i < f.blocksX * f.blocksY; ++i)
					z.push_back(f.texels[i * 4 + 2]);
				return z;
			};
			for (bool gate : { true, false })
			{
				Dejudder::Settings settings = DejudderTestSettings();
				settings.globalPrior = true;
				settings.estGate = gate;
				Dejudder::MotionWarp warp(fixture.Log(), gpu, settings);
				Assert::IsTrue(warp.Estimate(pa0.planes, pa1.planes));
				const std::vector<float> first = sads(warp);
				Assert::IsTrue(warp.Estimate(pc0.planes, pc1.planes));
				const std::vector<float> second = sads(warp);
				const bool kept = first == second;
				Assert::AreEqual(gate, kept, gate ? L"the gate should keep the previous searches on a still scene" :
					L"without the gate the still scene is searched again");
				if (!gate)
					continue;
				// A 3 px pan resumes the search.
				Assert::IsTrue(warp.Estimate(pc0.planes, pc2.planes));
				const DejudderField pan = DownloadField(gpu, warp, warp.Forward());
				int right = 0, total = 0;
				for (int y = 2; y < pan.blocksY - 2; ++y)
					for (int x = 2; x < pan.blocksX - 2; ++x)
					{
						++total;
						right += std::fabs(pan.vx(x, y) - 1.5f) <= 0.5f ? 1 : 0;   // half-resolution pixels
					}
				Assert::IsTrue(right * 100 >= total * 90, (L"the 3 px pan should be searched: " + std::to_wstring(right) +
					L" of " + std::to_wstring(total)).c_str());
				// Still again (arms the gate: the pan's mean |v| is above the
				// threshold, so this one searches), then a cut searches too.
				Assert::IsTrue(warp.Estimate(pc0.planes, pc1.planes));
				Assert::IsTrue(warp.Estimate(pc0.planes, pc1.planes));
				const std::vector<float> armed = sads(warp);
				Assert::IsTrue(warp.Estimate(pc0.planes, pd0.planes));
				Assert::IsFalse(sads(warp) == armed, L"a cut should be searched");
			}
			AssertCleanLibplaceboLog(log);
		}

		TEST_METHOD(EstimatorSplitOffStateLeavesEveryConsumerAlone)
		{
			// E2 off-state (planner 2026-10-13): with a threshold nothing reaches,
			// the split on leaves every consumer of the field reading the parent
			// exactly as with it off: the field, the occlusion mask, the grain
			// error and both warps, byte for byte; off allocates no sub-field.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			const int width = 256, height = 128;
			static constexpr int dx = 12, dy = 6;
			auto chroma = [](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); };
			const DejudderFrame a = MakeDejudderFrame(width, height, [](int x, int y) { return DejudderTexture(x, y); }, chroma);
			const DejudderFrame b = MakeDejudderFrame(width, height,
				[](int x, int y) { return DejudderTexture(x - dx, y - dy); },
				[](int x, int y, int c) { return DejudderTexture((x - dx / 2) * 2, y - dy, 7 + c); });
			DejudderGpuPlanes planesA, planesB;
			UploadDejudderFrame(gpu, a, planesA);
			UploadDejudderFrame(gpu, b, planesB);
			struct Seen { std::vector<float> field, occlusion, grain; BothPaths warped; };
			auto run = [&](bool split, bool& subFieldPresent)
			{
				Dejudder::Settings settings = DejudderTestSettings();
				settings.estSplit = split;
				settings.estSplitSad = 1e9f;   // nothing splits (the edge blocks of a pan would)
				Dejudder::MotionWarp warp(fixture.Log(), gpu, settings);
				Assert::IsTrue(warp.Estimate(planesA.planes, planesB.planes, 0, true), ExtendedRangeGpuProbe::ToWide(warp.LastError()).c_str());
				subFieldPresent = warp.SubField() != nullptr;
				Seen seen;
				seen.field = DownloadField(gpu, warp).texels;
				seen.occlusion = DownloadField(gpu, warp, warp.Occlusion(0)).texels;
				seen.grain = DownloadField(gpu, warp, warp.GrainError()).texels;
				seen.warped = WarpBothPaths(fixture, warp, a, planesA.planes, 0.5f);
				return seen;
			};
			bool offPresent = true, onPresent = false;
			const Seen off = run(false, offPresent), on = run(true, onPresent);
			Assert::IsFalse(offPresent, L"split off: no sub-field");
			Assert::IsTrue(onPresent, L"split on: a sub-field");
			Assert::IsTrue(off.field == on.field, L"field");
			Assert::IsTrue(off.occlusion == on.occlusion, L"occlusion mask");
			Assert::IsTrue(off.grain == on.grain, L"grain error");
			Assert::IsTrue(off.warped.source == on.warped.source, L"source warp");
			Assert::IsTrue(off.warped.rendered == on.warped.rendered, L"rendered warp");
			AssertCleanLibplaceboLog(log);
		}

		TEST_METHOD(EstimatorSplitFollowsASmallObject)
		{
			// E2: an 8x8 object moving (14, 6) inside a pan of (12, 6) fills one
			// quarter of block (6, 3). The block matches poorly as a whole; split,
			// the object's quarter takes (14, 6), trusted, and the others the
			// pan; both warps then move the object by half its motion.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			const int width = 256, height = 128;
			static constexpr int dx = 12, dy = 6, ox = 104, oy = 56, odx = 14;
			auto inA = [](int x, int y) { return x >= ox && x < ox + 8 && y >= oy && y < oy + 8; };
			auto inB = [](int x, int y) { return x >= ox + odx && x < ox + odx + 8 && y >= oy + dy && y < oy + dy + 8; };
			// Per-pixel detail: a 2 px shift of the object does not match itself.
			auto object = [](int x, int y) { return static_cast<uint16_t>(32000 + NoiseCode(x, y, 77, 24000)); };
			auto chroma = [](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); };
			const DejudderFrame a = MakeDejudderFrame(width, height,
				[&](int x, int y) { return inA(x, y) ? object(x - ox, y - oy) : DejudderTexture(x, y); }, chroma);
			const DejudderFrame b = MakeDejudderFrame(width, height,
				[&](int x, int y) { return inB(x, y) ? object(x - ox - odx, y - oy - dy) : DejudderTexture(x - dx, y - dy); },
				[](int x, int y, int c) { return DejudderTexture((x - dx / 2) * 2, y - dy, 7 + c); });
			DejudderGpuPlanes planesA, planesB;
			UploadDejudderFrame(gpu, a, planesA);
			UploadDejudderFrame(gpu, b, planesB);
			Dejudder::Settings settings = DejudderTestSettings();
			settings.estSplit = true;
			settings.estSplitSad = 1.0f;   // the pan matches at ~0; the object's block does not
			Dejudder::MotionWarp warp(fixture.Log(), gpu, settings);
			Assert::IsTrue(warp.Estimate(planesA.planes, planesB.planes), ExtendedRangeGpuProbe::ToWide(warp.LastError()).c_str());
			// The sub-field: two texels per block each way.
			DejudderField sub;
			{
				Assert::IsNotNull(warp.SubField());
				const int sw = warp.SubField()->params.w, sh = warp.SubField()->params.h;
				std::vector<uint16_t> half(static_cast<size_t>(sw) * sh * 4);
				pl_tex_transfer_params d{};
				d.tex = warp.SubField();
				d.ptr = half.data();
				Assert::IsTrue(pl_tex_download(gpu, &d), L"the sub-field is not host-readable");
				sub.blocksX = sw;
				sub.blocksY = sh;
				for (uint16_t h : half)
					sub.texels.push_back(HalfBits(h));
			}
			auto describe = [&](int sx, int sy)
			{
				const float* t = &sub.texels[(static_cast<size_t>(sy) * sub.blocksX + sx) * 4];
				return L"(" + std::to_wstring(t[0]) + L", " + std::to_wstring(t[1]) + L") trust " + std::to_wstring(t[2]) +
					L" split " + std::to_wstring(t[3]);
			};
			const int sx = ox / 8, sy = oy / 8;   // the object's sub-block
			const float* objectSub = &sub.texels[(static_cast<size_t>(sy) * sub.blocksX + sx) * 4];
			const float* panSub = &sub.texels[(static_cast<size_t>(sy) * sub.blocksX + sx - 1) * 4];
			const std::wstring message = L"object sub-block " + describe(sx, sy) + L"; its neighbour " + describe(sx - 1, sy) +
				L"; log: " + std::wstring(log.begin(), log.end());
			Assert::IsTrue(objectSub[3] > 0.5f && objectSub[2] > 0.5f && std::fabs(objectSub[0] - odx) <= 0.6f &&
				std::fabs(objectSub[1] - dy) <= 0.6f, (L"the object's sub-block follows the object; " + message).c_str());
			Assert::IsTrue(panSub[3] > 0.5f && std::fabs(panSub[0] - dx) <= 0.6f, (L"the surround follows the pan; " + message).c_str());
			// Both warps (backward: each output pixel takes A at its own position
			// minus its own vector) use the object's vector inside its sub-block:
			// there the split output is closer to A moved by (7, 3) than the
			// output without the split.
			const BothPaths split = WarpBothPaths(fixture, warp, a, planesA.planes, 0.5f);
			Dejudder::Settings plainSettings = DejudderTestSettings();
			Dejudder::MotionWarp plain(fixture.Log(), gpu, plainSettings);
			Assert::IsTrue(plain.Estimate(planesA.planes, planesB.planes));
			const BothPaths unsplit = WarpBothPaths(fixture, plain, a, planesA.planes, 0.5f);
			auto errorIn = [&](const std::vector<float>& image)
			{
				double e = 0.0;
				int n = 0;
				for (int y = oy + 1; y < oy + 7; ++y)
					for (int x = ox + 1; x < ox + 7; ++x)
					{
						e += std::fabs(image[static_cast<size_t>(y) * width + x] -
							static_cast<float>(a.luma[static_cast<size_t>(y - 3) * width + x - 7]));
						++n;
					}
				return e / n;
			};
			const double sSplit = errorIn(split.source), sPlain = errorIn(unsplit.source);
			const double rSplit = errorIn(split.rendered), rPlain = errorIn(unsplit.rendered);
			Assert::IsTrue(sSplit < sPlain && rSplit < rPlain, (L"inside the object's sub-block, error vs A moved by (7, 3): source " +
				std::to_wstring(sPlain) + L" -> " + std::to_wstring(sSplit) + L", rendered " + std::to_wstring(rPlain) + L" -> " +
				std::to_wstring(rSplit) + L"; " + message).c_str());
			AssertCleanLibplaceboLog(log);
		}

		// ---- Delta 9, synthesis-lite (docs/VP-delta-8-9-10.md).

		TEST_METHOD(HoldFeatherSoftensTheHeldBoundary)
		{
			// H1: an object in A gone in B leaves held blocks inside a pan.
			// With a 4 px feather, every pixel of a warped block is
			// A + smoothstep(distance to a held neighbour block) x (the
			// unfeathered warp - A), in both warp paths; off is the hard edge.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			const int width = 256, height = 128;
			static constexpr int dx = 12, dy = 6, ox = 96, oy = 32;
			auto inA = [](int x, int y) { return x >= ox && x < ox + 48 && y >= oy && y < oy + 48; };
			auto chroma = [](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); };
			const DejudderFrame a = MakeDejudderFrame(width, height,
				[&](int x, int y) { return inA(x, y) ? DejudderTexture(x, y, 55) : DejudderTexture(x, y); }, chroma);
			const DejudderFrame b = MakeDejudderFrame(width, height,
				[](int x, int y) { return DejudderTexture(x - dx, y - dy); },
				[](int x, int y, int c) { return DejudderTexture((x - dx / 2) * 2, y - dy, 7 + c); });
			DejudderGpuPlanes planesA, planesB;
			UploadDejudderFrame(gpu, a, planesA);
			UploadDejudderFrame(gpu, b, planesB);
			auto run = [&](float feather, DejudderField& field)
			{
				Dejudder::Settings settings = DejudderTestSettings();
				settings.holdFeatherPx = feather;
				Dejudder::MotionWarp warp(fixture.Log(), gpu, settings);
				Assert::IsTrue(warp.Estimate(planesA.planes, planesB.planes));
				field = DownloadField(gpu, warp);
				return WarpBothPaths(fixture, warp, a, planesA.planes, 0.5f);
			};
			DejudderField field, fieldOn;
			const BothPaths hard = run(0.0f, field);
			const BothPaths soft = run(4.0f, fieldOn);
			Assert::IsTrue(field.texels == fieldOn.texels, L"the feather does not change the field");
			int held = 0;
			for (int by = 0; by < field.blocksY; ++by)
				for (int bx = 0; bx < field.blocksX; ++bx)
					held += field.trusted(bx, by) ? 0 : 1;
			Assert::IsTrue(held > 0, L"some blocks should be held");
			auto clampd = [](double v, double lo, double hi) { return std::min(std::max(v, lo), hi); };
			auto smooth = [&](double e, double x) { const double t = clampd(x / e, 0.0, 1.0); return t * t * (3.0 - 2.0 * t); };
			double worstSource = 0.0, worstRendered = 0.0;
			int banded = 0;
			for (int y = 8; y < height - 8; ++y)
				for (int x = 8; x < width - 8; ++x)
				{
					const int bx = x / 16, by = y / 16;
					if (!field.trusted(bx, by))
						continue;
					double d = 1e9;
					for (int ny = -1; ny <= 1; ++ny)
						for (int nx = -1; nx <= 1; ++nx)
						{
							const int cx = bx + nx, cy = by + ny;
							if ((nx == 0 && ny == 0) || cx < 0 || cy < 0 || cx >= field.blocksX || cy >= field.blocksY ||
								field.trusted(cx, cy))
								continue;
							const double px = x + 0.5, py = y + 0.5;
							const double qx = clampd(px, cx * 16.0, cx * 16.0 + 16.0), qy = clampd(py, cy * 16.0, cy * 16.0 + 16.0);
							d = std::min(d, std::hypot(px - qx, py - qy));
						}
					const double w = smooth(4.0, d);
					if (w < 1.0) ++banded;
					const size_t i = static_cast<size_t>(y) * width + x;
					const double av = a.luma[i];
					worstSource = std::max(worstSource, std::fabs(soft.source[i] - (av + w * (hard.source[i] - av))));
					worstRendered = std::max(worstRendered, std::fabs(soft.rendered[i] - (av + w * (hard.rendered[i] - av))));
				}
			const std::wstring message = L"pixels in the band " + std::to_wstring(banded) + L"; worst deviation from the feather: source " +
				std::to_wstring(worstSource) + L", rendered " + std::to_wstring(worstRendered) + L" (16-bit codes); log: " +
				std::wstring(log.begin(), log.end());
			Assert::IsTrue(banded > 50, message.c_str());
			Assert::IsTrue(worstSource <= 2.0 && worstRendered <= 96.0, message.c_str());
			AssertCleanLibplaceboLog(log);
		}

		TEST_METHOD(HoldHysteresisNeedsTwoFrames)
		{
			// H2 (2 frames): block (5, 3) is matchable in P and not in Q (its
			// landing in B replaced). P, P: it warps. Q, P, Q, P: it stays
			// warping. Q, Q: it holds after the second. P then a cut: every
			// block holds at once.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			const int width = 256, height = 128;
			static constexpr int dx = 12, dy = 6, bx = 5, by = 3;
			auto chroma = [](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); };
			const DejudderFrame a = MakeDejudderFrame(width, height, [](int x, int y) { return DejudderTexture(x, y); }, chroma);
			auto landing = [](int x, int y)
			{
				return x >= bx * 16 + dx - 4 && x < bx * 16 + dx + 20 && y >= by * 16 + dy - 4 && y < by * 16 + dy + 20;
			};
			const DejudderFrame p = MakeDejudderFrame(width, height, [](int x, int y) { return DejudderTexture(x - dx, y - dy); },
				[](int x, int y, int c) { return DejudderTexture((x - dx / 2) * 2, y - dy, 7 + c); });
			const DejudderFrame q = MakeDejudderFrame(width, height,
				[&](int x, int y) { return landing(x, y) ? DejudderTexture(x, y, 33) : DejudderTexture(x - dx, y - dy); },
				[](int x, int y, int c) { return DejudderTexture((x - dx / 2) * 2, y - dy, 7 + c); });
			const DejudderFrame cut = MakeDejudderFrame(width, height, [](int x, int y) { return DejudderTexture(x, y, 99); }, chroma);
			DejudderGpuPlanes pa, pp, pq, pc;
			UploadDejudderFrame(gpu, a, pa);
			UploadDejudderFrame(gpu, p, pp);
			UploadDejudderFrame(gpu, q, pq);
			UploadDejudderFrame(gpu, cut, pc);
			Dejudder::Settings settings = DejudderTestSettings();
			settings.holdHysteresis = 2;
			Dejudder::MotionWarp warp(fixture.Log(), gpu, settings);
			std::wstring sequence;
			auto step = [&](const Dejudder::Planes& b, const wchar_t* name)
			{
				Assert::IsTrue(warp.Estimate(pa.planes, b), ExtendedRangeGpuProbe::ToWide(warp.LastError()).c_str());
				const DejudderField f = DownloadField(gpu, warp);
				int trustedOthers = 0;
				for (int y = 2; y < f.blocksY - 2; ++y)
					for (int x = 2; x < f.blocksX - 3; ++x)
						if ((x != bx || y != by) && std::abs(x - bx) > 1 && std::abs(y - by) > 1)
							trustedOthers += f.trusted(x, y) ? 1 : 0;
				sequence += std::wstring(name) + (f.trusted(bx, by) ? L"=warp " : L"=hold ");
				return std::make_pair(f.trusted(bx, by), trustedOthers);
			};
			const bool first = step(pp.planes, L"P").first;
			const bool second = step(pp.planes, L"P").first;
			bool alternating = true;
			for (int i = 0; i < 2; ++i)
			{
				alternating = step(pq.planes, L"Q").first && alternating;
				alternating = step(pp.planes, L"P").first && alternating;
			}
			const bool q1 = step(pq.planes, L"Q").first;
			const bool q2 = step(pq.planes, L"Q").first;
			const int othersBefore = step(pp.planes, L"P").second;
			const auto afterCut = step(pc.planes, L"cut");
			const std::wstring message = sequence + L"; log: " + std::wstring(log.begin(), log.end());
			Assert::IsTrue(!first && second, (L"warps after two trusted frames: " + message).c_str());
			Assert::IsTrue(alternating, (L"an alternating block keeps warping: " + message).c_str());
			Assert::IsTrue(q1 && !q2, (L"holds after two untrusted frames: " + message).c_str());
			Assert::IsTrue(othersBefore > 0 && afterCut.second == 0 && !afterCut.first, (L"a cut holds at once: " + message).c_str());
			AssertCleanLibplaceboLog(log);
		}

		TEST_METHOD(BlockTaperWarpsMarginalBlocksAtARampedStrength)
		{
			// H3: block (5, 3)'s landing in B carries noise. With the tolerance
			// at 0.8 x its SAD and the taper's end at 1.2 x, it is held without
			// the taper and warps at half strength with it (the ramp's middle):
			// at shift 1 it moves by (6, 3), not (12, 6), in both warp paths.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			const int width = 256, height = 128;
			static constexpr int dx = 12, dy = 6, bx = 5, by = 3;
			auto chroma = [](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); };
			const DejudderFrame a = MakeDejudderFrame(width, height, [](int x, int y) { return DejudderTexture(x, y); }, chroma);
			DejudderFrame b = MakeDejudderFrame(width, height, [](int x, int y) { return DejudderTexture(x - dx, y - dy); },
				[](int x, int y, int c) { return DejudderTexture((x - dx / 2) * 2, y - dy, 7 + c); });
			for (int y = by * 16 - 4 + dy; y < by * 16 + 20 + dy; ++y)
				for (int x = bx * 16 - 4 + dx; x < bx * 16 + 20 + dx; ++x)
					b.luma[static_cast<size_t>(y) * width + x] = static_cast<uint16_t>(
						b.luma[static_cast<size_t>(y) * width + x] + NoiseCode(x, y, 5, 1500));
			DejudderGpuPlanes planesA, planesB;
			UploadDejudderFrame(gpu, a, planesA);
			UploadDejudderFrame(gpu, b, planesB);
			float sad = 0.0f;
			{
				Dejudder::MotionWarp probe(fixture.Log(), gpu, DejudderTestSettings());
				Assert::IsTrue(probe.Estimate(planesA.planes, planesB.planes));
				const DejudderField f = DownloadField(gpu, probe, probe.Forward());
				sad = f.texels[(by * f.blocksX + bx) * 4 + 2];
			}
			Assert::IsTrue(sad > 0.5f, (L"the noise should cost the block's match: SAD " + std::to_wstring(sad)).c_str());
			auto run = [&](bool taper, bool& trusted)
			{
				Dejudder::Settings settings = DejudderTestSettings();
				settings.matchTolerance = 0.8f * sad;
				settings.blockTaper = taper;
				settings.taperSad = 1.2f * sad;
				Dejudder::MotionWarp warp(fixture.Log(), gpu, settings);
				Assert::IsTrue(warp.Estimate(planesA.planes, planesB.planes));
				trusted = DownloadField(gpu, warp).trusted(bx, by);
				return WarpBothPaths(fixture, warp, a, planesA.planes, 1.0f);
			};
			bool heldTrusted = true, taperTrusted = false;
			const BothPaths held = run(false, heldTrusted);
			const BothPaths tapered = run(true, taperTrusted);
			auto inBlock = [&](const std::vector<float>& image, int sx, int sy, float tolerance)
			{
				return MatchingPercent(image, width, bx * 16 + 3, by * 16 + 3, bx * 16 + 13, by * 16 + 13,
					[&](int x, int y) { return a.luma[static_cast<size_t>(y - sy) * width + x - sx]; }, tolerance);
			};
			const double heldSource = inBlock(held.source, 0, 0, kSourceTolerance), heldRendered = inBlock(held.rendered, 0, 0, kRenderedTolerance);
			const double halfSource = inBlock(tapered.source, 6, 3, kSourceTolerance), halfRendered = inBlock(tapered.rendered, 6, 3, kRenderedTolerance);
			const std::wstring message = L"SAD " + std::to_wstring(sad) + L"; off: trusted " + std::to_wstring(heldTrusted) +
				L", held " + std::to_wstring(heldSource) + L"% / " + std::to_wstring(heldRendered) + L"%; on: trusted " +
				std::to_wstring(taperTrusted) + L", moved (6, 3) " + std::to_wstring(halfSource) + L"% / " +
				std::to_wstring(halfRendered) + L"%; log: " + std::wstring(log.begin(), log.end());
			Assert::IsTrue(!heldTrusted && heldSource >= 95.0 && heldRendered >= 95.0, (L"off: held; " + message).c_str());
			Assert::IsTrue(taperTrusted && halfSource >= 90.0 && halfRendered >= 90.0, (L"on: half strength; " + message).c_str());
			AssertCleanLibplaceboLog(log);
		}

		TEST_METHOD(DejudderGrainErrorSeparatesNoiseFromTexture)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			const int width = 256, height = 128;
			std::string log;
			fixture.CaptureLog(&log);
			Dejudder::MotionWarp warp(fixture.Log(), gpu, DejudderTestSettings());
			std::string report;
			for (int fade = 0; fade <= 2; fade += 2)
			{
				DejudderGpuPlanes planesA, planesB;
				UploadDejudderFrame(gpu, GrainFrame(width, height, 0, 8, 4, 0), planesA);
				UploadDejudderFrame(gpu, GrainFrame(width, height, 1, 8, 4, fade), planesB);
				Assert::IsNull(warp.GrainError(), L"no grain error before an estimate asks for it");
				Assert::IsTrue(warp.Estimate(planesA.planes, planesB.planes, -1, true),
					std::wstring(warp.LastError().begin(), warp.LastError().end()).c_str());
				const DejudderField field = DownloadField(gpu, warp);
				const DejudderField grain = DownloadField(gpu, warp, warp.GrainError());
				double clean = 0.0, noisy = 0.0;
				int nClean = 0, nNoisy = 0, untrusted = 0;
				for (int by = 1; by < field.blocksY - 1; ++by)
					for (int bx = 1; bx < field.blocksX - 1; ++bx)
					{
						if (bx == field.blocksX / 2 - 1 || bx == field.blocksX / 2)
							continue;   // the boundary blocks see both
						if (!field.trusted(bx, by)) { ++untrusted; continue; }
						const float e = grain.texels[(by * grain.blocksX + bx) * 4];
						if (bx < field.blocksX / 2) { clean += e; ++nClean; }
						else { noisy += e; ++nNoisy; }
					}
				Assert::IsTrue(nClean > 10 && nNoisy > 10, L"too few trusted blocks");
				clean /= nClean;
				noisy /= nNoisy;
				char line[200];
				std::snprintf(line, sizeof(line), "fade %d: clean %.3f noisy %.3f (expected ~%.2f) untrusted %d\n",
					fade, clean, noisy, 0.564 * 3.0, untrusted);
				report += line;
				Assert::IsTrue(clean < 0.35 && noisy > 1.2 && noisy < 2.2, ExtendedRangeGpuProbe::ToWide(report).c_str());
				// Without asking, the next estimate drops it.
				Assert::IsTrue(warp.Estimate(planesA.planes, planesB.planes));
				Assert::IsNull(warp.GrainError());
			}
			Logger::WriteMessage(report.c_str());
			AssertCleanLibplaceboLog(log);
		}

		// Stage 8b (docs/VP-stage8b-upscale-blend.md), with a nearest-neighbour
		// luma doubler standing in for the CNN (its 2x2 box average is the
		// source exactly, so the faithful branch must equal libplacebo's own
		// ewa_lanczossharp render). The grain term is forced by the ramp:
		// grain_lo 1000 (clean, g = 1) or grain_hi 0.001 (grain, g = floor 0.4).
		TEST_METHOD(AdaptiveUpscaleBlendsCnnAndFaithfulByGrain)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			const int width = 128, height = 64;
			// Neutral grey: the blend is luma-only, so the branches are compared
			// exactly where R = G = B.
			auto chroma = [](int, int, int) { return static_cast<uint16_t>(32768); };
			const DejudderFrame frame = MakeDejudderFrame(width, height,
				[](int x, int y) { return DejudderTexture(x, y); }, chroma);
			DejudderGpuPlanes planes;
			UploadDejudderFrame(gpu, frame, planes);
			static const char doubler[] =
				"//!HOOK LUMA\n"
				"//!BIND HOOKED\n"
				"//!WIDTH HOOKED.w 2 *\n"
				"//!HEIGHT HOOKED.h 2 *\n"
				"//!WHEN OUTPUT.h HOOKED.h / 1.300 >\n"
				"//!DESC test nearest luma doubler\n"
				"vec4 hook() { return HOOKED_tex((floor(HOOKED_pos * HOOKED_size) + 0.5) / HOOKED_size); }\n";
			const pl_hook* cnn = pl_mpv_user_shader_parse(gpu, doubler, sizeof(doubler) - 1);
			Assert::IsNotNull(cnn);
			pl_fmt rgba16 = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 16, 16,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE));
			Assert::IsNotNull(rgba16);
			auto render = [&](int ow, int oh, std::vector<const pl_hook*> hooks, const pl_filter_config* upscaler)
			{
				pl_tex_params tp{};
				tp.w = ow;
				tp.h = oh;
				tp.format = rgba16;
				tp.renderable = true;
				tp.host_readable = true;
				pl_tex target = pl_tex_create(gpu, &tp);
				Assert::IsNotNull(target);
				pl_frame image{};
				image.num_planes = 2;
				image.planes[0].texture = planes.planes.luma;
				image.planes[0].components = 1;
				image.planes[0].component_mapping[0] = PL_CHANNEL_Y;
				image.planes[1].texture = planes.planes.chroma;
				image.planes[1].components = 2;
				image.planes[1].component_mapping[0] = PL_CHANNEL_CB;
				image.planes[1].component_mapping[1] = PL_CHANNEL_CR;
				image.repr.sys = PL_COLOR_SYSTEM_BT_709;
				image.repr.levels = PL_COLOR_LEVELS_FULL;
				image.color.primaries = PL_COLOR_PRIM_BT_709;
				image.color.transfer = PL_COLOR_TRC_BT_1886;
				pl_frame_set_chroma_location(&image, PL_CHROMA_LEFT);
				pl_frame t = TargetLutGpuFixture::MakeRgbFrame(target);
				t.repr.levels = PL_COLOR_LEVELS_FULL;
				t.color.transfer = PL_COLOR_TRC_BT_1886;
				pl_render_params params = pl_render_fast_params;
				params.dither_params = nullptr;
				params.error_diffusion = nullptr;
				// upscale_light gamma for the exact comparisons: with linear light
				// libplacebo scales a doubler's 2x output DOWN to 1.5x in linear
				// light but upscales the plain source in gamma light (no linear
				// upscaling without sigmoid), so the references would differ in
				// domain; in gamma light every path is gamma.
				params.sigmoid_params = nullptr;
				params.disable_linear_scaling = true;
				params.upscaler = upscaler;
				params.antiringing_strength = 0.5f;   // as the module's faithful branch
				params.hooks = hooks.empty() ? nullptr : hooks.data();
				params.num_hooks = static_cast<int>(hooks.size());
				Assert::IsTrue(pl_render_image(fixture.Renderer(), &image, &t, &params));
				pl_gpu_finish(gpu);
				std::vector<uint16_t> px(static_cast<size_t>(ow) * oh * 4);
				pl_tex_transfer_params d{};
				d.tex = target;
				d.ptr = px.data();
				Assert::IsTrue(pl_tex_download(gpu, &d));
				pl_tex_destroy(gpu, &target);
				return px;
			};
			AdaptiveDownscale::Settings settings;
			settings.grainMap = false;
			auto withModule = [&](AdaptiveDownscale::Module& module, float grainLo, float grainHi, float wBase,
				float nativeH, int ow, int oh, std::vector<const pl_hook*> hooks, bool superresAlpha = false,
				size_t blendAt = static_cast<size_t>(-1))
			{
				AdaptiveDownscale::Settings s2 = settings;
				s2.grainLo = grainLo;
				s2.grainHi = grainHi;
				s2.grainSmoothMs = 0.0f;
				// A new source frame per call: the grain state advances once per frame.
				static uint64_t sequence = 0;
				module.SetFrame(s2, ++sequence, 1.0f / 24.0f);
				AdaptiveDownscale::Upscale up;
				up.on = true;   // the module's own gate must refuse native and downscaled frames
				up.wBase = wBase;
				up.superresAlpha = superresAlpha;
				up.nativeHeight = nativeH;
				module.SetUpscale(up);
				hooks.insert(hooks.begin(), module.Measure());
				if (blendAt < hooks.size())
					hooks.insert(hooks.begin() + static_cast<std::ptrdiff_t>(blendAt) + 1, module.Blend());
				else
					hooks.push_back(module.Blend());
				return render(ow, oh, hooks, &pl_filter_ewa_lanczossharp);
			};
			auto maxDiff = [](const std::vector<uint16_t>& a, const std::vector<uint16_t>& b)
			{
				int m = 0;
				for (size_t i = 0; i < a.size(); ++i)
					if (i % 4 != 3)
						m = std::max(m, std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i])));
				return m;
			};
			const int ow = width * 3 / 2, oh = height * 3 / 2;   // 1.5x: the doubler runs (2x > 1.3x)
			const std::vector<uint16_t> cnnBranch = render(ow, oh, { cnn }, &pl_filter_ewa_lanczossharp);
			const std::vector<uint16_t> faithfulRef = render(ow, oh, {}, &pl_filter_ewa_lanczossharp);
			Assert::IsTrue(maxDiff(cnnBranch, faithfulRef) > 1000, L"the stand-in CNN should differ from the faithful upscale");
			{
				// The stand-in at exactly 2x: the nearest-neighbour source (diagnostic).
				const std::vector<uint16_t> twice = render(width * 2, height * 2, { cnn }, &pl_filter_ewa_lanczossharp);
				int worst = 0, wx = 0, wy = 0;
				for (int y = 4; y < height * 2 - 4; ++y)
					for (int x = 4; x < width * 2 - 4; ++x)
					{
						const int d = std::abs(static_cast<int>(twice[(static_cast<size_t>(y) * width * 2 + x) * 4]) -
							static_cast<int>(frame.luma[static_cast<size_t>(y / 2) * width + x / 2]));
						if (d > worst) { worst = d; wx = x; wy = y; }
					}
				Assert::IsTrue(worst <= 64, ExtendedRangeGpuProbe::ToWide("doubler at 2x vs nearest source: max " + std::to_string(worst) + " at " +
					std::to_string(wx) + "," + std::to_string(wy) + ": got " +
					std::to_string(twice[(static_cast<size_t>(wy) * width * 2 + wx) * 4]) + ", source " +
					std::to_string(frame.luma[static_cast<size_t>(wy / 2) * width + wx / 2]) + ", left " +
					std::to_string(frame.luma[static_cast<size_t>(wy / 2) * width + wx / 2 - 1]) + ", right " +
					std::to_string(frame.luma[static_cast<size_t>(wy / 2) * width + wx / 2 + 1])).c_str());
			}

			// Clean (g = 1, w = 1): exactly the CNN branch, edges included.
			AdaptiveDownscale::Module module;
			const std::vector<uint16_t> clean = withModule(module, 1000.0f, 2000.0f, 1.0f, static_cast<float>(height), ow, oh, { cnn });
			Assert::AreEqual(2, module.LastUpscaleRatio(), L"the doubler's output should be averaged to the source");
			const int cleanDiff = maxDiff(clean, cnnBranch);
			// w_base 0: the luma of the faithful branch (chroma: the same scaler in both).
			const std::vector<uint16_t> faithful = withModule(module, 1000.0f, 2000.0f, 0.0f, static_cast<float>(height), ow, oh, { cnn });
			const int faithfulDiff = maxDiff(faithful, faithfulRef);
			std::wstring where;
			{
				double sum = 0.0;
				int mx = 0, at = 0;
				for (size_t i = 0; i < faithful.size(); i += 4)
				{
					const int d = std::abs(static_cast<int>(faithful[i]) - static_cast<int>(faithfulRef[i]));
					sum += d;
					if (d > mx) { mx = d; at = static_cast<int>(i / 4); }
				}
				where = L" (mean " + std::to_wstring(sum / (faithful.size() / 4)) + L", max at " + std::to_wstring(at % ow) +
					L"," + std::to_wstring(at / ow) + L": " + std::to_wstring(faithful[static_cast<size_t>(at) * 4]) + L" vs " +
					std::to_wstring(faithfulRef[static_cast<size_t>(at) * 4]) + L", CNN " + std::to_wstring(cnnBranch[static_cast<size_t>(at) * 4]) + L")";
			}
			// Grain (g = 0.4): luma = CNN + 0.6 (faithful - CNN).
			const std::vector<uint16_t> grainy = withModule(module, 0.0f, 0.001f, 1.0f, static_cast<float>(height), ow, oh, { cnn });
			int grainDiff = 0;
			for (size_t i = 0; i < grainy.size(); ++i)
			{
				if (i % 4 == 3)
					continue;
				const double expected = cnnBranch[i] + 0.6 * (static_cast<double>(faithful[i]) - cnnBranch[i]);
				grainDiff = std::max(grainDiff, static_cast<int>(std::lround(std::fabs(grainy[i] - expected))));
			}
			// Without the doubler the base is the source picture itself.
			const std::vector<uint16_t> plain = withModule(module, 1000.0f, 2000.0f, 0.0f, static_cast<float>(height), ow, oh, {});
			const int plainRatio = module.LastUpscaleRatio();
			const int plainDiff = maxDiff(plain, faithfulRef);
			// Gate: native size and a downscale are left alone.
			const std::vector<uint16_t> native = withModule(module, 0.0f, 0.001f, 0.0f, static_cast<float>(height), width, height, { cnn });
			const std::vector<uint16_t> nativeRef = render(width, height, { cnn }, &pl_filter_ewa_lanczossharp);
			const std::vector<uint16_t> down = withModule(module, 0.0f, 0.001f, 0.0f, static_cast<float>(height), width / 2, height / 2, { cnn });
			const std::vector<uint16_t> downRef = render(width / 2, height / 2, { cnn }, &pl_filter_ewa_lanczossharp);
			const std::wstring message = L"clean vs CNN " + std::to_wstring(cleanDiff) + L", faithful vs ewa " +
				std::to_wstring(faithfulDiff) + where + L", grain vs CNN + 0.6 (faithful - CNN) " + std::to_wstring(grainDiff) +
				L", no doubler: ratio " + std::to_wstring(plainRatio) + L" vs ewa " + std::to_wstring(plainDiff) +
				L" (16-bit codes); log: " + std::wstring(log.begin(), log.end());
			// The blend's output and the faithful branch are 16-bit float
			// textures: about 1/2048 of the value (32 of 65535 near white).
			Assert::IsTrue(cleanDiff <= 32, message.c_str());
			Assert::IsTrue(faithfulDiff <= 96 && plainDiff <= 96 && plainRatio == 1, message.c_str());
			Assert::IsTrue(grainDiff <= 96, message.c_str());
			Assert::AreEqual(0, maxDiff(native, nativeRef), (L"native size must be untouched; " + message).c_str());
			Assert::AreEqual(0, maxDiff(down, downRef), (L"a downscale must be untouched; " + message).c_str());

			// SuperRes strength follows the grain term (its vp_grain_strength,
			// alpha from the blend): with g = 0.4 its correction is scaled by 0.4.
			std::string superSource = LoadBundledShader("SSimSuperRes.glsl");
			Assert::IsFalse(superSource.empty());
			const pl_hook* superres = pl_mpv_user_shader_parse(gpu, superSource.data(), superSource.size());
			Assert::IsNotNull(superres);
			// On the plain upscale (no doubler), where SuperRes has work to do;
			// the blend sits before it (both POST_KERNEL).
			const std::vector<uint16_t> blendOnly = withModule(module, 0.0f, 0.001f, 1.0f, static_cast<float>(height), ow, oh, {});
			const int superAlone = maxDiff(render(ow, oh, { superres }, &pl_filter_ewa_lanczossharp), faithfulRef);
			Assert::IsTrue(CustomHookChain::SetHookParameter(superres, "vp_grain_strength", 0.0));
			const std::vector<uint16_t> fullSuper = withModule(module, 0.0f, 0.001f, 1.0f, static_cast<float>(height), ow, oh,
				{ superres }, false, 0);
			Assert::IsTrue(CustomHookChain::SetHookParameter(superres, "vp_grain_strength", 1.0));
			const std::vector<uint16_t> scaledSuper = withModule(module, 0.0f, 0.001f, 1.0f, static_cast<float>(height), ow, oh,
				{ superres }, true, 0);
			double num = 0.0, den = 0.0;
			for (size_t i = 0; i < blendOnly.size(); ++i)
			{
				if (i % 4 == 3)
					continue;
				const double full = static_cast<double>(fullSuper[i]) - blendOnly[i];
				const double scaled = static_cast<double>(scaledSuper[i]) - blendOnly[i];
				num += full * scaled;
				den += full * full;
			}
			const double ratio = den > 0.0 ? num / den : 0.0;
			// Clean (g = 1): the scaled SuperRes is the full one.
			const std::vector<uint16_t> cleanScaled = withModule(module, 1000.0f, 2000.0f, 1.0f, static_cast<float>(height), ow, oh,
				{ superres }, true, 0);
			Assert::IsTrue(CustomHookChain::SetHookParameter(superres, "vp_grain_strength", 0.0));
			const std::vector<uint16_t> cleanFull = withModule(module, 1000.0f, 2000.0f, 1.0f, static_cast<float>(height), ow, oh,
				{ superres }, false, 0);
			// SuperRes after a doubler is covered by the chain matrix, not here
			// (planner 2026-10-12, not a gap): on these synthetic pictures its
			// correction after a doubler is at half-float rounding level (31 of
			// 65535 even after a sharpening doubler, without the blend), so it
			// cannot be measured; the order and the alpha handoff are checked
			// above on the plain upscale, and the matrix's refinements axis runs
			// real FSRCNNX + SSimSuperRes with the blend at the upscale scales.
			pl_mpv_user_shader_destroy(&superres);
			pl_mpv_user_shader_destroy(&cnn);
			const std::wstring superMessage = L"SuperRes correction with g = 0.4: " + std::to_wstring(ratio) +
				L" of the full one (correction energy " + std::to_wstring(den) + L"; SuperRes alone changes the plain upscale by " +
				std::to_wstring(superAlone) + L"); clean scaled vs full " +
				std::to_wstring(maxDiff(cleanScaled, cleanFull)) + L"; log: " + std::wstring(log.begin(), log.end());
			Assert::IsTrue(den > 0.0 && std::fabs(ratio - 0.4) < 0.05, superMessage.c_str());
			Assert::IsTrue(maxDiff(cleanScaled, cleanFull) <= 1, superMessage.c_str());
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(log);
		}

		TEST_METHOD(AdaptiveDownscaleGrainMapFromTheVectors)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create());
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			const pl_hook* ssim = nullptr;
			{
				const std::string source = LoadBundledShader("SSimDownscaler.glsl");
				ssim = pl_mpv_user_shader_parse(gpu, source.data(), source.size());
				Assert::IsNotNull(ssim);
			}
			static constexpr int width = 256, height = 128, dx = 8, dy = 4;
			pl_fmt srcFmt = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 16, 16,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_LINEAR));
			pl_fmt dstFmt = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 8, 8,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE));
			Assert::IsTrue(srcFmt && dstFmt);
			Dejudder::MotionWarp warp(fixture.Log(), gpu, DejudderTestSettings());
			AdaptiveDownscale::Module module;
			module.hostReadableMap = true;
			AdaptiveDownscale::Settings st;
			st.grainSmoothMs = 0.0f;       // the scalar follows each frame
			st.grainMapSmoothMs = 100.0f;  // the map settles in a few frames
			// The picture as RGB (grey from the luma), the render's source.
			auto rgbOf = [&](const DejudderFrame& f)
			{
				std::vector<uint16_t> px(static_cast<size_t>(width) * height * 4);
				for (size_t i = 0; i < f.luma.size(); ++i)
				{
					px[i * 4] = px[i * 4 + 1] = px[i * 4 + 2] = f.luma[i];
					px[i * 4 + 3] = 65535;
				}
				pl_tex_params tp{};
				tp.w = width; tp.h = height; tp.format = srcFmt; tp.sampleable = true; tp.initial_data = px.data();
				return pl_tex_create(gpu, &tp);
			};
			auto render = [&](pl_tex src, uint64_t sequence, bool withField, int mode = 1)
			{
				// mode 1: measure + SSIM + blend; 0: SSIM alone.
				module.SetFrame(st, sequence, 1.0f / 24.0f);
				module.ClearGrainField();
				if (withField)
				{
					AdaptiveDownscale::GrainField gf;
					gf.error = warp.GrainError();
					gf.field = warp.Field();
					gf.frameTrust = warp.FrameTrust();
					gf.minTrustedShare = warp.GetSettings().minTrustedShare;
					gf.sourceCrop = { 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height) };
					module.SetGrainField(gf);
				}
				pl_tex_params tp{};
				tp.w = width / 2; tp.h = height / 2; tp.format = dstFmt; tp.renderable = true; tp.host_readable = true;
				pl_tex target = pl_tex_create(gpu, &tp);
				pl_frame image = TargetLutGpuFixture::MakeRgbFrame(src);
				pl_frame out = TargetLutGpuFixture::MakeRgbFrame(target);
				pl_render_params params = pl_render_default_params;
				params.dither_params = nullptr;
				params.error_diffusion = nullptr;
				params.downscaler = &pl_filter_mitchell;
				const pl_hook* hooks[3] = { module.Measure(), ssim, module.Blend() };
				const pl_hook* ssimOnly[1] = { ssim };
				params.hooks = mode == 1 ? hooks : ssimOnly;
				params.num_hooks = mode == 1 ? 3 : 1;
				Assert::IsTrue(pl_render_image(fixture.Renderer(), &image, &out, &params));
				pl_gpu_finish(gpu);
				std::vector<RgbaPixel> px(static_cast<size_t>(width / 2) * (height / 2));
				pl_tex_transfer_params dl{};
				dl.tex = target;
				dl.ptr = px.data();
				Assert::IsTrue(pl_tex_download(gpu, &dl));
				pl_tex_destroy(gpu, &target);
				return px;
			};
			std::string report;
			// Frames 0..7; each pair (f, f+1) estimated, frame f rendered with its field.
			for (int f = 0; f < 8; ++f)
			{
				const DejudderFrame a = GrainFrame(width, height, f, dx, dy, 0);
				const DejudderFrame b = GrainFrame(width, height, f + 1, dx, dy, 0);
				DejudderGpuPlanes planesA, planesB;
				UploadDejudderFrame(gpu, a, planesA);
				UploadDejudderFrame(gpu, b, planesB);
				Assert::IsTrue(warp.Estimate(planesA.planes, planesB.planes, -1, true));
				pl_tex src = rgbOf(a);
				render(src, static_cast<uint64_t>(f + 1), true);
				render(src, static_cast<uint64_t>(f + 1), true);   // a repeat: the map stands
				pl_tex_destroy(gpu, &src);
			}
			Assert::IsTrue(module.Active() && module.Failures() == 0, L"the blend did not run");
			Assert::IsTrue(module.LastMapState() == AdaptiveDownscale::MapState::Used, L"the map was not used");
			pl_tex map = module.GrainMap();
			Assert::IsNotNull(map);
			std::vector<float> texels(static_cast<size_t>(map->params.w) * map->params.h * 4);
			pl_tex_transfer_params d{};
			d.tex = map;
			d.ptr = texels.data();
			Assert::IsTrue(pl_tex_download(gpu, &d), L"map not readable");
			double clean = 0.0, noisy = 0.0;
			int nClean = 0, nNoisy = 0;
			const int bw = map->params.w, bh = map->params.h;
			for (int by = 1; by < bh - 1; ++by)
				for (int bx = 1; bx < bw - 1; ++bx)
				{
					if (bx == bw / 2 - 1 || bx == bw / 2)
						continue;
					const float* t = &texels[(static_cast<size_t>(by) * bw + bx) * 4];
					if (t[1] < 0.5f)
						continue;   // untrusted
					if (bx < bw / 2) { clean += t[0]; ++nClean; }
					else { noisy += t[0]; ++nNoisy; }
				}
			Assert::IsTrue(nClean > 10 && nNoisy > 10, L"too few trusted blocks in the map");
			clean /= nClean;
			noisy /= nNoisy;
			char line[200];
			std::snprintf(line, sizeof(line), "map: texture %.3f (%d blocks), noise %.3f (%d blocks), floor %.2f\n",
				clean, nClean, noisy, nNoisy, st.grainFloor);
			report += line;
			Assert::IsTrue(clean > 0.9 && noisy < st.grainFloor + 0.1, ExtendedRangeGpuProbe::ToWide(report).c_str());
			// The blend follows the map where the output shows it: on the
			// texture half (map 1) it is SSIM except at edges; on the noise
			// half (map at the floor) it moves towards faithful. Faithful is the
			// blend with w_base 0. Measured as the mean distance to SSIM per
			// half, against the scalar-only blend of the same frame.
			{
				const DejudderFrame a = GrainFrame(width, height, 7, dx, dy, 0);
				pl_tex src = rgbOf(a);
				const auto withMap = render(src, 8, true);          // the map stands for frame 8
				const auto ssimPx = render(src, 8, true, 0);
				AdaptiveDownscale::Settings saved = st;
				st.grainMap = false;
				const auto scalarPx = render(src, 8, true);
				st = saved;
				pl_tex_destroy(gpu, &src);
				double mapTex = 0.0, scalarTex = 0.0, mapNoise = 0.0, scalarNoise = 0.0;
				int nTex = 0, nNoise = 0;
				const int ow = width / 2, oh = height / 2;
				for (int y = 8; y < oh - 8; ++y)
					for (int x = 8; x < ow - 8; ++x)
					{
						if (x >= ow / 2 - 12 && x < ow / 2 + 12)
							continue;   // the boundary (bilinear between blocks)
						const size_t i = static_cast<size_t>(y) * ow + x;
						const double m = std::abs(withMap[i].g - ssimPx[i].g), sc = std::abs(scalarPx[i].g - ssimPx[i].g);
						if (x < ow / 2) { mapTex += m; scalarTex += sc; ++nTex; }
						else { mapNoise += m; scalarNoise += sc; ++nNoise; }
					}
				mapTex /= nTex; scalarTex /= nTex; mapNoise /= nNoise; scalarNoise /= nNoise;
				std::snprintf(line, sizeof(line), "blend vs SSIM: texture map %.3f scalar %.3f; noise map %.3f scalar %.3f (scalar g %.3f)\n",
					mapTex, scalarTex, mapNoise, scalarNoise, module.LastStats().grainScale);
				report += line;
				// Texture: the map keeps SSIM at least as much as the scalar;
				// noise: it leans away from SSIM at least as much; and the two
				// halves differ (the map is not one number).
				Assert::IsTrue(mapTex <= scalarTex + 0.05 && mapNoise >= scalarNoise - 0.05 && mapNoise > mapTex + 0.2,
					ExtendedRangeGpuProbe::ToWide(report).c_str());
			}
			// No field for the next source frame: the scalar.
			{
				const DejudderFrame a = GrainFrame(width, height, 8, dx, dy, 0);
				pl_tex src = rgbOf(a);
				render(src, 9, false);
				pl_tex_destroy(gpu, &src);
				Assert::IsTrue(module.LastMapState() == AdaptiveDownscale::MapState::NoField, L"no field: not the scalar");
			}
			// Map off: the scalar, nothing else.
			st.grainMap = false;
			{
				const DejudderFrame a = GrainFrame(width, height, 9, dx, dy, 0);
				pl_tex src = rgbOf(a);
				render(src, 10, true);
				pl_tex_destroy(gpu, &src);
				Assert::IsTrue(module.LastMapState() == AdaptiveDownscale::MapState::Off);
			}
			fixture.CaptureLog(nullptr);
			Logger::WriteMessage(report.c_str());
			module.Release();
			pl_mpv_user_shader_destroy(&ssim);
			std::string remaining;
			std::istringstream lines(log);
			for (std::string entry; std::getline(lines, entry);)
				if ((entry.find("translating HLSL to DXBC (slow!)") == std::string::npos && entry.find("translating GLSL to SPIR-V (slow!)") == std::string::npos))
					remaining += entry + "\n";
			Assert::IsTrue(remaining.empty(), ExtendedRangeGpuProbe::ToWide("libplacebo logged: " + remaining).c_str());
			Assert::AreEqual(0, pl_renderer_get_errors(fixture.Renderer()).num_disabled_hooks, L"A hook was disabled");
		}

		// Every hook at once, in the renderer's order, over frames with a cut
		// (ChainGpuProbe.h): libplacebo 7.360 with the encoding repair.
		TEST_METHOD(WholeHookChainKeepsThePictureOnEveryFrame)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create());
			std::string log;
			fixture.CaptureLog(&log);
			const ChainGpuProbe::Outcome outcome = ChainGpuProbe::Run(fixture.Gpu(), fixture.Renderer(), fixture.Log(),
				[](const char* file) { return LoadBundledShader(file); }, true);
			fixture.CaptureLog(nullptr);
			Logger::WriteMessage(outcome.report.c_str());
			Assert::IsTrue(outcome.ok, ExtendedRangeGpuProbe::ToWide(outcome.failure).c_str());
			// The 7.360 PRE_OUTPUT note (repaired) and shader-compiler timing notes only.
			std::string remaining;
			std::istringstream lines(log);
			for (std::string entry; std::getline(lines, entry);)
				if ((entry.find("translating HLSL to DXBC (slow!)") == std::string::npos && entry.find("translating GLSL to SPIR-V (slow!)") == std::string::npos) &&
					entry.find("Attempted to modify an immutable shader!") == std::string::npos)
					remaining += entry + "\n";
			Assert::IsTrue(remaining.empty(), ExtendedRangeGpuProbe::ToWide("libplacebo logged: " + remaining).c_str());
		}

		// The whole chain with motion (the matcher's look-ahead vectors, the
		// grain map following them, the occlusion mask, the sigma-delta's motion
		// compensation), a de-judder extra, and the LED drive's gain sequence
		// (ChainGpuProbe::RunMotion).
		// The Stage 10 carry on a single-plane RGB source (the subtitle move's
		// composed picture, an RGB capture): libplacebo 7.360 asserted
		// (renderer.c:457) after the inline colour carry; the carry's texture form
		// renders, with and without NLS / SSIM, cropped or not, 3 or 4 components.
		TEST_METHOD(ExtendedRangeOnASinglePlaneRgbSource)
		{
			for (unsigned mode : { 0u, 1u, 2u, 3u, 4u, 8u })
			{
			using namespace ExtendedRangeGpuProbe;
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create());
			pl_gpu gpu = fixture.Gpu();
			pl_fmt f16 = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 16, 0, static_cast<pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_RENDERABLE));
			std::vector<uint16_t> px(static_cast<size_t>(640) * 96 * 4, 0x3800);   // 0.5
			pl_tex_params tp{}; tp.w = 640; tp.h = 96; tp.format = f16; tp.sampleable = true; tp.renderable = true; tp.initial_data = px.data();
			pl_tex tex = pl_tex_create(gpu, &tp);
			pl_frame rgb{};
			rgb.num_planes = 1; rgb.planes[0].texture = tex; rgb.planes[0].components = 4;
			for (int i = 0; i < 4; ++i) rgb.planes[0].component_mapping[i] = i;
			rgb.repr.sys = PL_COLOR_SYSTEM_RGB; rgb.repr.levels = PL_COLOR_LEVELS_FULL; rgb.repr.alpha = PL_ALPHA_NONE;
			rgb.color.primaries = PL_COLOR_PRIM_BT_709; rgb.color.transfer = PL_COLOR_TRC_GAMMA22;
			rgb.crop = { 0.0f, 16.0f, 640.0f, 80.0f };
			const std::string nlsText = LoadBundledShader("SSimDownscaler.glsl");
			const pl_hook* ssim = pl_mpv_user_shader_parse(gpu, nlsText.data(), nlsText.size());
			const pl_hook* nls = ParseBundledNlsShader(gpu, "NLS.glsl", 0.0);
			BindNlsShader(nls, 1.0f, 0.0f);
			Case c; c.scale = 0.5; c.mitchell = true; c.on = true; c.image = &rgb;
			c.sourceCrop[0] = 0; c.sourceCrop[1] = 16; c.sourceCrop[2] = 640; c.sourceCrop[3] = 80;
			if (mode & 1u) c.extraHooks.push_back(nls);
			if (mode & 2u) c.extraHooks.push_back(ssim);
			if (mode & 4u) { c.sourceCrop[1] = 0; c.sourceCrop[3] = 96; rgb.crop = { 0.0f, 0.0f, 640.0f, 96.0f }; }
			if (mode & 8u) rgb.planes[0].components = 3;
			Planes size; size.width = 640; size.height = 96;
			const Result r = Render(gpu, fixture.Renderer(), size, c);
			pl_tex_destroy(gpu, &tex);
			pl_mpv_user_shader_destroy(&ssim);
			pl_mpv_user_shader_destroy(&nls);
			Assert::IsTrue(r.ok && r.restores > 0, (L"mode " + std::to_wstring(mode)).c_str());
			Assert::AreEqual(0, pl_renderer_get_errors(fixture.Renderer()).num_disabled_hooks);
			}
		}

		// Upscaling with every hook (ChainGpuProbe::RunUpscale), FSRCNNX and ArtCNN.
		// The Stage 10 carry after hooks on the planes before RGB: libplacebo 7.360
		// asserted (renderer.c:457) after the inline carry with CfL chroma
		// prediction; Prepare picks the carry's texture form then. CfL, the LUMA
		// prescaler and SSimSuperRes, alone and together, on a 2x upscale.
		// Stage 10b on saturated colours (10a's open item: its one-offset shift
		// made steps that SSIM enhanced, 46 codes). 10b hands libplacebo the
		// clipped picture and converts the primaries where libplacebo does, so
		// wherever no out-of-range source pixel lies within the filters' reach
		// (12 source pixels: Mitchell at 0.5 plus SSIM's window), Stage 10 must
		// give the picture without the switch.
		// Near out-of-range content the outputs legitimately differ (the true
		// values are mixed in instead of clipped ones); that is reported.
		// Plain scaling and with SSIM, down 0.5, a strongly coloured texture.
		TEST_METHOD(ExtendedRangeOnSaturatedColourWithSsim)
		{
			using namespace ExtendedRangeGpuProbe;
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create());
			const std::vector<int> ramp = { 4, 16, 40, 64, 100, 502, 900, 940, 980, 1019 };
			Planes a = ChainGpuProbe::MovingFrame(0, ramp);
			ChainGpuProbe::Colourise(a, 0, 64);
			// Which source pixels leave 0..1 in a channel (BT.709 Y'CbCr -> R'G'B').
			std::vector<char> outOfRange(static_cast<size_t>(a.width) * a.height, 0);
			for (int y = 0; y < a.height; ++y)
				for (int x = 0; x < a.width; ++x)
				{
					const double yv = ((a.luma[static_cast<size_t>(y) * a.width + x] >> 6) - 64) / 876.0;
					const size_t ci = (static_cast<size_t>(y) * (a.width / 2) + x / 2) * 2;
					const double cb = ((a.chroma[ci] >> 6) - 512) / 896.0, cr = ((a.chroma[ci + 1] >> 6) - 512) / 896.0;
					const double r = yv + 1.5748 * cr, g = yv - 0.1873 * cb - 0.4681 * cr, b = yv + 1.8556 * cb;
					outOfRange[static_cast<size_t>(y) * a.width + x] =
						(std::min)({ r, g, b }) < -0.002 || (std::max)({ r, g, b }) > 1.002;
				}
			auto nearOutOfRange = [&](int ox, int oy)
			{
				const int cx = ox * 2 + 1, cy = oy * 2 + 1;
				for (int y = (std::max)(0, cy - 12); y <= (std::min)(a.height - 1, cy + 12); ++y)
					for (int x = (std::max)(0, cx - 12); x <= (std::min)(a.width - 1, cx + 12); ++x)
						if (outOfRange[static_cast<size_t>(y) * a.width + x])
							return true;
				return false;
			};
			const std::string ssimText = LoadBundledShader("SSimDownscaler.glsl");
			const pl_hook* ssim = pl_mpv_user_shader_parse(fixture.Gpu(), ssimText.data(), ssimText.size());
			Assert::IsNotNull(ssim);
			std::string report;
			bool ok = true;
			for (enum pl_color_primaries primaries : { PL_COLOR_PRIM_DISPLAY_P3, PL_COLOR_PRIM_BT_709 })
				for (bool withSsim : { false, true })
				{
					Case on; on.scale = 0.5; on.mitchell = true; on.outputPrimaries = primaries;
					if (withSsim) on.extraHooks.push_back(ssim);
					Case off = on; off.on = false;
					const Result r = Render(fixture.Gpu(), fixture.Renderer(), a, on);
					const Result o = Render(fixture.Gpu(), fixture.Renderer(), a, off);
					Assert::IsTrue(r.ok && o.ok && r.restores > 0);
					int clearMax = 0, clear = 0, nearMax = 0, closeBy = 0;
					for (int y = 2; y < 30; ++y)   // the texture rows, clear of the ramp
						for (int x = 2; x < r.width - 2; ++x)
						{
							int d = 0;
							for (int ch = 0; ch < 3; ++ch)
								d = (std::max)(d, std::abs(r.Channel(x, y, ch) - o.Channel(x, y, ch)));
							if (nearOutOfRange(x, y)) { nearMax = (std::max)(nearMax, d); ++closeBy; }
							else { clearMax = (std::max)(clearMax, d); ++clear; }
						}
					// 10a: 46 codes here (P3, SSIM). The residual with SSIM is its
					// magnification of the two conversions' rounding.
					const int allowed = withSsim ? 3 : 1;
					ok = ok && clearMax <= allowed && clear > 1000;
					report += std::string(primaries == PL_COLOR_PRIM_BT_709 ? "BT.709 output, " : "P3 output, ") +
						(withSsim ? "SSIM" : "plain") + ": clear of out-of-range content " + std::to_string(clearMax) +
						" codes at most (" + std::to_string(clear) + " pixels, allowed " + std::to_string(allowed) + "); near it " +
						std::to_string(nearMax) + " (" + std::to_string(closeBy) + " pixels, the true values mixed in)\n";
				}
			pl_mpv_user_shader_destroy(&ssim);
			Logger::WriteMessage(report.c_str());
			Assert::IsTrue(ok, ToWide(report).c_str());
		}

		TEST_METHOD(ExtendedRangeAfterPlaneHooks)
		{
			using namespace ExtendedRangeGpuProbe;
			for (unsigned mode : { 1u, 2u, 3u, 7u })
			{
				TargetLutGpuFixture fixture;
				Assert::IsTrue(fixture.Create());
				pl_gpu gpu = fixture.Gpu();
				std::string error;
				auto src = [](const char* file) { return LoadBundledShader(file); };
				auto pre = ChainGpuProbe::Load(gpu, src, "FSRCNNX_x2_16-0-4-1.glsl", 1280, 192, {}, error);
				auto cfl = ChainGpuProbe::Load(gpu, src, "CfL_Prediction.glsl", 1280, 192, {}, error);
				auto sr = ChainGpuProbe::Load(gpu, src, "SSimSuperRes.glsl", 1280, 192, {}, error);
				Assert::IsTrue(pre && cfl && sr, ToWide(error).c_str());
				const std::vector<int> ramp = { 4, 16, 40, 64, 100, 502, 900, 940, 980, 1019 };
				Planes a = ChainGpuProbe::MovingFrame(0, ramp);
				ChainGpuProbe::Colourise(a, 0, 64);
				Case c;
				c.scale = 2.0;
				if (mode & 1u) c.extraHooks.push_back(pre->hook);
				if (mode & 2u) c.extraHooks.push_back(cfl->hook);
				if (mode & 4u) c.extraHooks.push_back(sr->hook);
				const Result r = Render(gpu, fixture.Renderer(), a, c);
				Assert::IsTrue(r.ok && r.restores > 0, (L"mode " + std::to_wstring(mode)).c_str());
				{ const pl_render_errors e = pl_renderer_get_errors(fixture.Renderer()); Assert::AreEqual(0, e.num_disabled_hooks, (L"mode " + std::to_wstring(mode) + L" disabled " + (e.num_disabled_hooks ? std::to_wstring(e.disabled_hooks[0]) : L"")).c_str()); }
			}
		}

		// The combination matrix (ChainGpuProbe::RunMatrix): a spread of 54
		// combinations by default, all 336 with VP_TEST_MATRIX=full.
		TEST_METHOD(WholeHookChainCombinationMatrix)
		{
			char* value = nullptr;
			size_t length = 0;
			const bool full = _dupenv_s(&value, &length, "VP_TEST_MATRIX") == 0 && value && std::string(value) == "full";
			std::free(value);
			std::string report;
			int passed = 0;
			for (const ChainGpuProbe::Combo& combo : ChainGpuProbe::MatrixCombos(full))
			{
				TargetLutGpuFixture fixture;
				Assert::IsTrue(fixture.Create());
				std::string log;
				fixture.CaptureLog(&log);
				const ChainGpuProbe::Outcome outcome = ChainGpuProbe::RunMatrix(fixture.Gpu(), fixture.Renderer(), fixture.Log(),
					[](const char* file) { return LoadBundledShader(file); }, true, combo);
				fixture.CaptureLog(nullptr);
				report += outcome.report;
				Assert::IsTrue(outcome.ok, ExtendedRangeGpuProbe::ToWide(outcome.failure).c_str());
				std::string remaining;
				std::istringstream lines(log);
				for (std::string entry; std::getline(lines, entry);)
					if ((entry.find("translating HLSL to DXBC (slow!)") == std::string::npos && entry.find("translating GLSL to SPIR-V (slow!)") == std::string::npos) &&
						entry.find("Attempted to modify an immutable shader!") == std::string::npos)
						remaining += entry + "\n";
				Assert::IsTrue(remaining.empty(), ExtendedRangeGpuProbe::ToWide(combo.Name() + ": libplacebo logged: " + remaining).c_str());
				++passed;
			}
			Logger::WriteMessage((report + std::to_string(passed) + " combinations passed\n").c_str());
		}

		TEST_METHOD(WholeHookChainUpscales)
		{
			for (const char* prescaler : { "FSRCNNX_x2_16-0-4-1.glsl", "ArtCNN_C4F16.glsl" })
			{
				TargetLutGpuFixture fixture;
				Assert::IsTrue(fixture.Create());
				std::string log;
				fixture.CaptureLog(&log);
				const ChainGpuProbe::Outcome outcome = ChainGpuProbe::RunUpscale(fixture.Gpu(), fixture.Renderer(), fixture.Log(),
					[](const char* file) { return LoadBundledShader(file); }, true, prescaler);
				fixture.CaptureLog(nullptr);
				Logger::WriteMessage(outcome.report.c_str());
				Assert::IsTrue(outcome.ok, ExtendedRangeGpuProbe::ToWide(outcome.failure).c_str());
				std::string remaining;
				std::istringstream lines(log);
				for (std::string entry; std::getline(lines, entry);)
					if ((entry.find("translating HLSL to DXBC (slow!)") == std::string::npos && entry.find("translating GLSL to SPIR-V (slow!)") == std::string::npos) &&
						entry.find("Attempted to modify an immutable shader!") == std::string::npos)
						remaining += entry + "\n";
				Assert::IsTrue(remaining.empty(), ExtendedRangeGpuProbe::ToWide("libplacebo logged: " + remaining).c_str());
			}
		}

		TEST_METHOD(WholeHookChainHandlesMotionAndTheLookAheads)
		{
			// Twice: as built, and with every refinement on (D1-D4, S1).
			for (bool refinements : { false, true })
			{
				TargetLutGpuFixture fixture;
				Assert::IsTrue(fixture.Create());
				std::string log;
				fixture.CaptureLog(&log);
				const ChainGpuProbe::Outcome outcome = ChainGpuProbe::RunMotion(fixture.Gpu(), fixture.Renderer(), fixture.Log(),
					[](const char* file) { return LoadBundledShader(file); }, true, 63u, refinements);
				fixture.CaptureLog(nullptr);
				Logger::WriteMessage((refinements ? "-- refinements on\n" : "-- refinements off\n"));
				Logger::WriteMessage(outcome.report.c_str());
				Assert::IsTrue(outcome.ok, ExtendedRangeGpuProbe::ToWide((refinements ? "refinements on: " : "") +
					outcome.failure).c_str());
				std::string remaining;
				std::istringstream lines(log);
				for (std::string entry; std::getline(lines, entry);)
					if ((entry.find("translating HLSL to DXBC (slow!)") == std::string::npos && entry.find("translating GLSL to SPIR-V (slow!)") == std::string::npos) &&
						entry.find("Attempted to modify an immutable shader!") == std::string::npos)
						remaining += entry + "\n";
				Assert::IsTrue(remaining.empty(), ExtendedRangeGpuProbe::ToWide("libplacebo logged: " + remaining).c_str());
			}
		}

		TEST_METHOD(BundledNlsGlSlHooksMovePixelsOnTheRealGpuPath)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(),
				L"Could not create the libplacebo WARP test device");
			// Every warning or error libplacebo logs fails the test (design review 4, N-6).
			std::string cleanLog;
			fixture.CaptureLog(&cleanLog);
			const std::vector<RgbaPixel> baseline =
				fixture.RenderCoordinateField(nullptr);

			const pl_hook* balanced = ParseBundledNlsShader(
				fixture.Gpu(), "NLSPlus.glsl", 0.25, 0.8);
			BindNlsShader(balanced, 1.32f, 0.0f);
			const std::vector<RgbaPixel> balancedPixels =
				fixture.RenderCoordinateField(balanced);
			const size_t quarter = 16 * 64 + 16;
			Assert::IsTrue(std::abs(static_cast<int>(balancedPixels[quarter].r) -
				static_cast<int>(baseline[quarter].r)) >= 3,
				L"NLS+ did not move horizontal coordinate pixels");
			Assert::IsTrue(std::abs(static_cast<int>(balancedPixels[quarter].g) -
				static_cast<int>(baseline[quarter].g)) >= 3,
				L"NLS+ did not move vertical coordinate pixels");
			Assert::IsTrue(std::abs(static_cast<int>(balancedPixels.front().r) -
				static_cast<int>(baseline.front().r)) <= 2 &&
				std::abs(static_cast<int>(balancedPixels.front().g) -
					static_cast<int>(baseline.front().g)) <= 2,
				L"NLS+ did not keep the fixed image edge bounded");

			// Dynamic changes must affect the already-parsed hook. This catches a
			// compiled shader whose runtime parameter data is never consumed.
			BindNlsShader(balanced, 1.0f, 0.0f);
			const std::vector<RgbaPixel> identityPixels =
				fixture.RenderCoordinateField(balanced);
			Assert::IsTrue(std::abs(static_cast<int>(identityPixels[quarter].r) -
				static_cast<int>(baseline[quarter].r)) <= 2 &&
				std::abs(static_cast<int>(identityPixels[quarter].g) -
					static_cast<int>(baseline[quarter].g)) <= 2,
				L"Updating the same NLS+ hook to ratio 1 was not identity");

			const pl_hook* oneAxisPlus = ParseBundledNlsShader(
				fixture.Gpu(), "NLSPlus.glsl", 0.0);
			const pl_hook* established = ParseBundledNlsShader(
				fixture.Gpu(), "NLS.glsl", 0.0);
			BindNlsShader(oneAxisPlus, 1.32f, 0.0f);
			BindNlsShader(established, 1.32f, 0.0f);
			const std::vector<RgbaPixel> oneAxisPlusPixels =
				fixture.RenderCoordinateField(oneAxisPlus);
			const std::vector<RgbaPixel> establishedPixels =
				fixture.RenderCoordinateField(established);
			for (size_t index = 0; index < establishedPixels.size(); ++index)
			{
				Assert::IsTrue(std::abs(
					static_cast<int>(oneAxisPlusPixels[index].r) -
					static_cast<int>(establishedPixels[index].r)) <= 2);
				Assert::IsTrue(std::abs(
					static_cast<int>(oneAxisPlusPixels[index].g) -
					static_cast<int>(establishedPixels[index].g)) <= 2);
			}

			BindNlsShader(established, 1.125f, 1.0f);
			const std::vector<RgbaPixel> verticalPixels =
				fixture.RenderCoordinateField(established);
			Assert::IsTrue(std::abs(static_cast<int>(verticalPixels[quarter].g) -
				static_cast<int>(baseline[quarter].g)) >= 3,
				L"Existing NLS GLSL did not move vertical coordinate pixels");
			Assert::IsTrue(std::abs(static_cast<int>(verticalPixels[quarter].r) -
				static_cast<int>(baseline[quarter].r)) <= 2,
				L"Vertical NLS unexpectedly altered the horizontal coordinate");

			pl_mpv_user_shader_destroy(&balanced);
			pl_mpv_user_shader_destroy(&oneAxisPlus);
			pl_mpv_user_shader_destroy(&established);
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(cleanLog);
		}
	};
}
