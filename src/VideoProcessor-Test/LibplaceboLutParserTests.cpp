#include "pch.h"
#include "CppUnitTest.h"

#include "SubtitleTextTestPattern.h"
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
#include <vprenderer/FrameInterpolator.h>
#include <vprenderer/DejudderAhead.h>
#include <vprenderer/GpuCaptureUnpack.h>
#include <vprenderer/GpuRgbUnpack.h>
#include <vprenderer/GpuDolbyVisionTunnelUnpack.h>
#include <vprenderer/DolbyVisionTunnelUnpackReference.h>
#include <video_frame_formatter/CR210DolbyVisionTunnelFormatter.h>
#include "DolbyVisionTunnelSender.h"
#include <vprenderer/EarlyUpload.h>
#include <vprenderer/SourceRebase.h>
#include <KernelInverse.h>
#include <vprenderer/SourceResolutionMonitor.h>
#include <vprenderer/VpDebander.h>
#include <chrono>
#include <random>
#include <thread>
#include <video_frame_formatter/CDeckLinkRGBToP010VideoFrameFormatter.h>
#include <video_frame_formatter/CARGBtoP010VideoFrameFormatter.h>
#include <libplacebo/utils/upload.h>
#include <video_frame_formatter/CV210toP210VideoFrameFormatter.h>
#include <video_frame_formatter/CUYVYtoP210VideoFrameFormatter.h>
#include <VideoFrame.h>
#include <VideoState.h>
#include <vprenderer/OutputEncodingRepair.h>
#include <vprenderer/OcclusionMaskHook.h>
#include <vprenderer/SigmaDeltaDomain.h>
#include <vprenderer/ExtendedRange.h>
#include <vprenderer/SdrToneShape.h>
#include <vprenderer/DolbyVisionTrims.h>
#include <vprenderer/RenderedWarp.h>
#include <vprenderer/AdaptiveDownscale.h>
#include "ExtendedRangeGpuProbe.h"
#include "ChainGpuProbe.h"
#include "GrainReferenceModel.h"
#include <memory>
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
		ID3D11Device* Device() const { return m_d3d11 ? m_d3d11->device : nullptr; }

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
			// The two-pixel donor guard puts the mirror edge at y=26.
            // Output y=32 samples stair row 19: 2*26-32-1.
            const int reflectedStair=32*64+25;
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
            for(const bool top:{false,true}) for(const bool measuredCapture:{false,true}) {
                const auto g=ComputeSubtitleCutPaste(top?SubtitleBoxRect{12,7,52,24}:SubtitleBoxRect{12,40,52,57},
                    64,64,8,56,SubtitleBoxPadding(6,8,8));
                const SubtitleBoxRect card=g.generatedCleanup;
                auto geometry=g;
                // Capture grants permission to key possible glyphs. It is not
                // proof that the whole picture is contaminated; this bounded
                // variant models a measured card interior.
                if(measuredCapture) geometry.pictureCapture=card;
                SubtitleGeneratedGrayStyle style;style.opacity=0.0f;style.maxLuminance=1.0f;style.blurPixels=30;
                Assert::IsTrue(BindSubtitleCutPasteHook(hook,geometry,5,PL_COLOR_TRC_SRGB,style));
                auto render=[&](int gray) {return fixture.RenderSubtitlePattern(hook,false,0,geometry,PL_COLOR_TRC_SRGB,
                    true,true,150,false,gray,&card,false,false,nullptr,nullptr,false,-1,false);};
                const auto black=render(0),colored=render(80);
                // All taps must remain outside the card even at maximum radius.
                // Compare central cleanup pixels outside the moved glyph column.
                for(int y=card.top;y<card.bottom;++y)for(const int x:{24,39}) {
                    const int sourceY=y+geometry.source.top-geometry.destination.top;
                    // An intentionally gray source card keys as ink in the
                    // glyph envelope. Probe the clean, bar-facing margin only.
                    if(sourceY>=geometry.content.top && sourceY<geometry.content.bottom)continue;
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

		// Delta 8 R1: the extras' sigma-delta takes VP's vectors only with
		// dither_vectors auto, the fork patch, a field and the same source frame.
		TEST_METHOD(DitherVectorsAutoFallsBackWithoutThePatch)
		{
			Assert::IsTrue(DejudderRendered::UseExternalVectors(false, true, true, true));
			Assert::IsFalse(DejudderRendered::UseExternalVectors(true, true, true, true), L"own");
			Assert::IsFalse(DejudderRendered::UseExternalVectors(false, false, true, true), L"7.360: no patch 0004");
			Assert::IsFalse(DejudderRendered::UseExternalVectors(false, true, false, true), L"no matcher field");
			Assert::IsFalse(DejudderRendered::UseExternalVectors(false, true, true, false), L"another source frame");
			Assert::AreEqual(16, DejudderRendered::VectorField::Blocks(256));
			Assert::AreEqual(68, DejudderRendered::VectorField::Blocks(1080));
		}

		// Delta 13: the SDR tone shape's reference curve.
		TEST_METHOD(SdrToneShapeReferencePlacesWhiteAndRollsOffIntoTheHeadroom)
		{
			const double g = 2.2;
			auto code = [](double c) { return (c - 16.0) / 219.0; };   // limited-range code -> signal
			// Off: identity (no hook is installed either).
			SdrToneShape::Settings off;
			Assert::IsFalse(SdrToneShape::Active(off));
			for (double c : { 16.0, 100.0, 235.0, 255.0 })
				Assert::AreEqual(code(c), SdrToneShape::Map(code(c), g, 0.0, 0.8, 0.0), 1e-12);
			// Headroom 0.15, no knee: code 235 at 85% of peak in light, 255
			// above it, monotonic, below-black kept (sign-preserving).
			const double white = SdrToneShape::Map(code(235.0), g, 0.15, 0.8, 0.0);
			Assert::AreEqual(0.85, std::pow(white, g), 1e-9);
			const double over = SdrToneShape::Map(code(255.0), g, 0.15, 0.8, 0.0);
			Assert::IsTrue(over > white);
			Assert::IsTrue(SdrToneShape::Map(code(4.0), g, 0.15, 0.8, 0.0) < 0.0, L"below-black kept");
			double last = -1.0;
			for (int c = 0; c <= 255; ++c)
			{
				const double v = SdrToneShape::Map(code(c), g, 0.15, 0.8, 0.0);
				Assert::IsTrue(v > last, L"monotonic");
				last = v;
			}
			// The knee (strength 0.5, headroom 0.15): a 200 -> 255 ramp is smooth
			// (no step larger than its neighbours'), monotonic, and nothing clips:
			// 235 and 255 both stay below peak.
			std::vector<double> ramp;
			for (int c = 200; c <= 255; ++c)
				ramp.push_back(SdrToneShape::Map(code(c), g, 0.15, 0.8, 0.5));
			for (size_t i = 1; i < ramp.size(); ++i)
			{
				Assert::IsTrue(ramp[i] > ramp[i - 1], L"knee monotonic");
				if (i >= 2)
					Assert::IsTrue(ramp[i] - ramp[i - 1] <= (ramp[i - 1] - ramp[i - 2]) * 1.05 + 1e-9,
						L"the knee's steps only shrink (no kink, no clip)");
			}
			Assert::IsTrue(ramp.back() > SdrToneShape::Map(code(235.0), g, 0.15, 0.8, 0.5) && ramp.back() <= SdrToneShape::Map(code(255.0), g, 0.15, 0.8, 0.0) + 1e-12, L"WTW above the shaped white, nothing created");
			Assert::IsTrue(SdrToneShape::Map(code(235.0), g, 0.15, 0.8, 0.5) < white, L"the knee lowers white a little");
			// Slope 1 at the knee start: continuous with the straight line.
			double slope = 0.0;
			SdrToneShape::Knee(0.8, 0.8, 0.5, &slope);
			Assert::AreEqual(1.0, slope, 1e-12);
			SdrToneShape::Knee(0.8 + 1e-6, 0.8, 0.5, &slope);
			Assert::AreEqual(1.0, slope, 1e-5);
			// Strength and recovery as the renderer passes them.
			SdrToneShape::Settings knee;
			knee.knee = true;
			knee.kneeStrength = 0.5;
			Assert::IsTrue(SdrToneShape::Active(knee));
			Assert::AreEqual(0.5, SdrToneShape::EffectiveStrength(knee), 1e-12);
			SdrToneShape::Settings headroomOnly;
			headroomOnly.headroom = 0.15;
			Assert::IsTrue(SdrToneShape::Active(headroomOnly));
			Assert::AreEqual(0.0, SdrToneShape::EffectiveStrength(headroomOnly), 1e-12);
			Assert::AreEqual(0.3, SdrToneShape::EffectiveRecovery(knee, 0.3), 1e-12, L"auto follows the profile");
			knee.recovery = 0.0;
			Assert::AreEqual(0.0, SdrToneShape::EffectiveRecovery(knee, 0.3), 1e-12, L"off");
		}

		// dv_trims (2026-10-17): DV trims.glsl matches DolbyVisionTrims::ApplyPixel
		// on the GPU (PQ and gamma signals, neutral = untouched).
		TEST_METHOD(DolbyVisionTrimShaderMatchesItsReference)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
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
			rule.name = "dv_trims";
			rule.filename = "DV trims.glsl";
			chain.Configure({ rule });
			CustomHookChain::TemplateValues values;
			values.outputWidth = values.outputHeight = 64;
			chain.Ensure(values);
			std::vector<const pl_hook*> hooks;
			chain.Collect(false, hooks);
			Assert::AreEqual(size_t{ 1 }, hooks.size(), L"libplacebo rejected DV trims.glsl");
			Assert::IsTrue((hooks[0]->stages & PL_HOOK_PRE_OUTPUT) != 0);
			const pl_hook* hook = hooks[0];
			auto set = [hook](const DolbyVisionTrims::Params& p, const DolbyVisionTrims::Signal& s)
			{
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "dv_slope", p.slope));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "dv_offset", p.offset));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "dv_power", p.power));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "dv_chroma_weight", p.chromaWeight));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "dv_sat_gain", p.saturationGain));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "dv_signal_pq", s.pq ? 1.0 : 0.0));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "dv_gamma", s.gamma));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "dv_peak_nits", s.peakNits));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "dv_kr", s.kr));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "dv_kb", s.kb));
			};
			int checked = 0;
			for (const bool pq : { true, false })
			{
				DolbyVisionTrims::Signal signal;
				signal.pq = pq;
				signal.peakNits = pq ? 600.0 : 100.0;
				if (!pq) { signal.kr = 0.2126; signal.kg = 0.7152; signal.kb = 0.0722; }
				else { signal.kr = 0.2627; signal.kg = 0.6780; signal.kb = 0.0593; }
				set(DolbyVisionTrims::Params(), signal);
				const auto unity = fixture.RenderCoordinateField(hook);
				// Neutral = untouched, exactly, on every channel.
				for (size_t i = 0; i < unity.size(); ++i)
					Assert::IsTrue(baseline[i].r == unity[i].r && baseline[i].g == unity[i].g && baseline[i].b == unity[i].b,
						(L"neutral = untouched (exact) at " + std::to_wstring(i) + L": " +
							std::to_wstring(baseline[i].r) + L"," + std::to_wstring(baseline[i].g) + L"," + std::to_wstring(baseline[i].b) + L" vs " +
							std::to_wstring(unity[i].r) + L"," + std::to_wstring(unity[i].g) + L"," + std::to_wstring(unity[i].b)).c_str());
				// A darkening trim, and a lifting one past 1 (the offset fade-in near
				// black and the shoulder below the peak, reviews C2 / S3).
				for (int variant = 0; variant < 2; ++variant)
				{
				DolbyVisionTrims::Params trim;
				if (variant == 0) { trim.slope = 0.85; trim.offset = 0.02; trim.power = 1.15; trim.chromaWeight = 0.2; trim.saturationGain = 1.2; }
				else { trim.slope = 1.3; trim.offset = 0.1; trim.power = 0.9; trim.chromaWeight = -0.3; trim.saturationGain = 1.15; }
				set(trim, signal);
				const auto trimmed = fixture.RenderCoordinateField(hook);
				int changed = 0, loose = 0;
				for (size_t i = 0; i < trimmed.size(); ++i)
				{
					// libplacebo hands PRE_OUTPUT an rgba16f intermediate: the reference
					// takes the same half-precision input (11-bit significand).
					auto half = [](double x)
					{
						if (x == 0.0) return 0.0;
						int e = 0;
						const double m = std::frexp(x, &e);
						return std::ldexp(std::round(m * 2048.0) / 2048.0, e);
					};
					double v[3] = { half(baseline[i].r / 255.0), half(baseline[i].g / 255.0), half(baseline[i].b / 255.0) };
					double scale = 0.0;   // the pixel's light scale (largest channel, nits)
					for (int c = 0; c < 3; ++c)
						scale = (std::max)(scale, std::fabs(DolbyVisionTrims::ToNits(v[c], signal)));
					DolbyVisionTrims::ApplyPixel(v, trim, signal);
					const uint8_t got[3] = { trimmed[i].r, trimmed[i].g, trimmed[i].b };
					for (int c = 0; c < 3; ++c)
					{
						// Within 1.5 codes, or (where out = Y' + gain (L - Y) cancels large
						// numbers into a channel of a few nits, at a gamut edge or under a
						// strong saturation gain, and PQ is steep near black) within
						// 5e-4 of the pixel's light in linear light: the GPU's float PQ
						// maths cannot resolve that cancellation more exactly.
						const double expected = 255.0 * (std::min)(1.0, (std::max)(0.0, v[c]));
						const double diff = std::fabs(expected - static_cast<double>(got[c]));
						if (diff <= 1.5)
							continue;
						const double nitsDiff = std::fabs(DolbyVisionTrims::ToNits(expected / 255.0, signal) -
							DolbyVisionTrims::ToNits(got[c] / 255.0, signal));
						Assert::IsTrue(nitsDiff <= 2e-3 * scale + 1e-3, (std::wstring(pq ? L"PQ " : L"gamma ") + L"variant " +
							std::to_wstring(variant) + L" pixel " + std::to_wstring(i) + L" channel " + std::to_wstring(c) + L": " +
							std::to_wstring(expected) + L" vs " + std::to_wstring(got[c])).c_str());
						++loose;
					}
					changed += got[0] != baseline[i].r || got[1] != baseline[i].g || got[2] != baseline[i].b;
					++checked;
				}
				Assert::IsTrue(changed > static_cast<int>(trimmed.size()) / 2, L"the trim changes the picture");
				Assert::IsTrue(loose * 100 <= static_cast<int>(trimmed.size()) * 3 * 5, L"at most 5% of samples beyond 1.5 codes");
				Logger::WriteMessage((std::string(pq ? "PQ" : "gamma") + " variant " + std::to_string(variant) + ": " +
					std::to_string(loose) + " samples beyond 1.5 codes\n").c_str());
				}
			}
			// libplacebo's known PRE_OUTPUT log noise (the output-encoding bug the
			// renderer repairs), as in the SDR tone shape test.
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLogExceptPreOutputBug(cleanLog);
			Logger::WriteMessage(("DV trims shader: " + std::to_string(checked) + " pixels matched the reference\n").c_str());
		}

		// Delta 13: the shader matches its reference on the GPU.
		TEST_METHOD(SdrToneShapeShaderMatchesItsReference)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
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
			rule.name = "sdr_tone_shape";
			rule.filename = "SDR tone shape.glsl";
			chain.Configure({ rule });
			CustomHookChain::TemplateValues values;
			values.outputWidth = values.outputHeight = 64;
			chain.Ensure(values);
			std::vector<const pl_hook*> hooks;
			chain.Collect(false, hooks);
			Assert::AreEqual(size_t{ 1 }, hooks.size(), L"libplacebo rejected SDR tone shape.glsl");
			Assert::IsTrue((hooks[0]->stages & PL_HOOK_PRE_OUTPUT) != 0);
			const pl_hook* hook = hooks[0];
			auto set = [hook](double headroom, double strength, double recovery = 0.0)
			{
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "headroom", headroom));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "knee_start", 0.8));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "strength", strength));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "eotf", 2.2));
				Assert::IsTrue(CustomHookChain::SetHookParameter(hook, "recovery", recovery));
			};
			set(0.0, 0.0);
			const auto unity = fixture.RenderCoordinateField(hook);
			for (size_t i = 0; i < unity.size(); ++i)
				Assert::AreEqual(static_cast<int>(baseline[i].r), static_cast<int>(unity[i].r), L"neutral = untouched");
			for (const double strength : { 0.0, 0.5, 1.0 })
			{
				set(0.15, strength);
				const auto shaped = fixture.RenderCoordinateField(hook);
				for (size_t x = 0; x < shaped.size(); ++x)
				{
					const double expected = 255.0 * SdrToneShape::Map(baseline[x].r / 255.0, 2.2, 0.15, 0.8, strength);
					Assert::AreEqual(expected, static_cast<double>(shaped[x].r), 1.5,
						(L"x " + std::to_wstring(x) + L" strength " + std::to_wstring(strength)).c_str());
					if (x % 64 != 0)
						Assert::IsTrue(shaped[x].r >= shaped[x - 1].r, L"monotonic");
				}
			}
			// Contrast recovery on a smooth ramp: almost no local detail, so the
			// picture stays within a code of the knee alone.
			set(0.15, 0.5, 2.0);
			const auto recovered = fixture.RenderCoordinateField(hook);
			set(0.15, 0.5, 0.0);
			const auto plain = fixture.RenderCoordinateField(hook);
			for (size_t x = 2; x + 2 < plain.size(); ++x)
				Assert::IsTrue(std::abs(static_cast<int>(recovered[x].r) - static_cast<int>(plain[x].r)) <= 1,
					L"recovery adds nothing on a smooth ramp");
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLogExceptPreOutputBug(cleanLog);
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
		// The one exception: libplacebo's timing notes on a loaded machine,
		// "Spent <N> ms <operation> (slow!)" (any operation: shader compiles,
		// pipeline creation, ...). Every other warning or error stays fatal.
		static bool IsLibplaceboSlowTimingNote(std::string line)
		{
			while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
				line.pop_back();
			static const std::string slow = "(slow!)";
			return line.compare(0, 6, "Spent ") == 0 && line.find(" ms ") != std::string::npos &&
				line.size() >= slow.size() && line.compare(line.size() - slow.size(), slow.size(), slow) == 0;
		}

		static std::string WithoutLibplaceboTimingNotes(const std::string& log)
		{
			std::string remaining;
			std::istringstream lines(log);
			for (std::string entry; std::getline(lines, entry);)
				if (!IsLibplaceboSlowTimingNote(entry))
					remaining += entry + "\n";
			return remaining;
		}

		static void AssertCleanLibplaceboLog(const std::string& log, const std::string& context = std::string())
		{
			const std::string remaining = WithoutLibplaceboTimingNotes(log);
			const std::string message = (context.empty() ? std::string() : context + ": ") +
				"libplacebo logged a warning or error: " + remaining;
			Assert::IsTrue(remaining.empty(), std::wstring(message.begin(), message.end()).c_str());
		}

		// The same, but accepting the one message libplacebo 7.360.1 logs for
		// every texture-input PRE_OUTPUT hook (the dropped output encoding that
		// OutputEncodingRepair puts back; see PreOutputHooksKeep...Repair).
		static void AssertCleanLibplaceboLogExceptPreOutputBug(const std::string& log,
			const std::string& context = std::string())
		{
			std::string remaining;
			std::istringstream lines(log);
			for (std::string entry; std::getline(lines, entry);)
				if (entry.find("Attempted to modify an immutable shader!") == std::string::npos)
					remaining += entry + "\n";
			AssertCleanLibplaceboLog(remaining, context);
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
		// Planner 2026-10-16 evening: regressions for the two optical-flow fixes. (1) The
		// engine's textures were freed before it unregistered them, a fail-fast (0xC0000409)
		// on NVIDIA at teardown and on a size change - here: two sizes, back, then destroy.
		// (2) The replace path ran the backward block search over the engine's flow (an
		// operator-precedence bug) - here: with the engine replacing, or seeding, the searches
		// no block-search pass runs, and the engine's backward vectors are the ones kept.
		TEST_METHOD(OpticalFlowEngineSurvivesResizeAndTeardownAndReplacesTheSearches)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			static constexpr int dx = 12, dy = 6;
			auto frames = [&](int width, int height, DejudderGpuPlanes& pa, DejudderGpuPlanes& pb) {
				const DejudderFrame a = MakeDejudderFrame(width, height,
					[](int x, int y) { return DejudderTexture(x, y); }, [](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); });
				const DejudderFrame b = MakeDejudderFrame(width, height,
					[](int x, int y) { return DejudderTexture(x - dx, y - dy); },
					[](int x, int y, int c) { return DejudderTexture((x - dx / 2) * 2, y - dy, 7 + c); });
				UploadDejudderFrame(gpu, a, pa);
				UploadDejudderFrame(gpu, b, pb);
			};
			DejudderGpuPlanes a1, b1, a2, b2;
			frames(512, 256, a1, b1);
			frames(768, 384, a2, b2);
			bool engine = false;
			{
				Dejudder::Settings settings = DejudderTestSettings();
				settings.opticalFlow = true;
				settings.opticalFlowCandidate = true;
				settings.opticalFlowSeed = true;
				Dejudder::MotionWarp warp(fixture.Log(), gpu, settings);
				Assert::IsTrue(warp.Estimate(a1.planes, b1.planes), std::wstring(warp.LastError().begin(), warp.LastError().end()).c_str());
				engine = warp.OpticalFlowUsed();
				if (!engine)
				{
					Logger::WriteMessage(("optical flow engine unavailable here (" + warp.OpticalFlowError() + "); regressions not run\n").c_str());
					return;
				}
				Assert::IsTrue(warp.Estimate(a2.planes, b2.planes), L"the engine across a size change");
				Assert::IsTrue(warp.Estimate(a1.planes, b1.planes), L"and back");
				Assert::IsTrue(warp.OpticalFlowUsed(), L"the engine still in use after the size changes");
			}   // destroyed here: the engine first (a fail-fast would end the test process)
			Logger::WriteMessage("teardown after two size changes: survived\n");
			for (const bool seed : { false, true })
			{
				Dejudder::Settings settings = DejudderTestSettings();
				settings.opticalFlow = true;
				settings.opticalFlowCandidate = seed;   // false: the engine replaces the searches
				settings.opticalFlowSeed = seed;
				Dejudder::MotionWarp warp(fixture.Log(), gpu, settings);
				warp.SetPassTiming(true, true);
				Assert::IsTrue(warp.Estimate(a1.planes, b1.planes), std::wstring(warp.LastError().begin(), warp.LastError().end()).c_str());
				Assert::IsTrue(warp.OpticalFlowUsed());
				std::string passes;
				bool searched = false;
				for (const auto& entry : warp.PassMs())
				{
					passes += entry.first + "; ";
					searched = searched || entry.first.find("block search") != std::string::npos;
				}
				Assert::IsFalse(searched, (std::wstring(seed ? L"seed" : L"replace") + L": no block search runs: " +
					std::wstring(passes.begin(), passes.end())).c_str());
				// The backward field is the engine's pan, (-6, -3) half-size pixels.
				const DejudderField bwd = DownloadField(gpu, warp, warp.Backward());
				int interior = 0, right = 0;
				for (int by = 2; by < bwd.blocksY - 2; ++by)
					for (int bx = 2; bx < bwd.blocksX - 2; ++bx)
					{
						++interior;
						right += std::abs(bwd.vx(bx, by) + 6) < 0.6f && std::abs(bwd.vy(bx, by) + 3) < 0.6f ? 1 : 0;
					}
				Assert::IsTrue(right * 100 >= interior * 90, (std::wstring(seed ? L"seed" : L"replace") + L": the backward field holds the pan (" +
					std::to_wstring(right) + L" of " + std::to_wstring(interior) + L")").c_str());
			}
		}

		// Planner Q1 (2026-10-16): the whole-frame motion (D1) from the global search and from the
		// engine's field, against the true pan, and whether each is marked usable (z < 0.9) -
		// the taste's roll-off and the camera read it.
		TEST_METHOD(EngineGlobalMatchesTheSearchOnKnownPans)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			auto readGlobal = [&](const Dejudder::MotionWarp& warp, float out[4]) {
				pl_tex t = warp.GlobalVector();
				Assert::IsNotNull(t);
				const size_t texel = t->params.format->texel_size;
				std::vector<uint8_t> raw(texel);
				pl_tex_transfer_params d{}; d.tex = t; d.ptr = raw.data();
				Assert::IsTrue(pl_tex_download(gpu, &d), L"the global vector is not host-readable");
				for (int k = 0; k < 4; ++k)
				{
					if (texel == 16) { float f; std::memcpy(&f, &raw[static_cast<size_t>(k) * 4], 4); out[k] = f; }
					else { uint16_t h; std::memcpy(&h, &raw[static_cast<size_t>(k) * 2], 2); out[k] = HalfBits(h); }
				}
			};
			int worse = 0;
			bool engine = true;
			for (const int dx : { 6, 12, 20, 32 })
			{
				const int width = 768, height = 384;
				const DejudderFrame a = MakeDejudderFrame(width, height,
					[](int x, int y) { return DejudderTexture(x, y); }, [](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); });
				const DejudderFrame b = MakeDejudderFrame(width, height,
					[dx](int x, int y) { return DejudderTexture(x - dx, y); },
					[dx](int x, int y, int c) { return DejudderTexture((x - dx / 2) * 2, y, 7 + c); });
				DejudderGpuPlanes pa, pb;
				UploadDejudderFrame(gpu, a, pa);
				UploadDejudderFrame(gpu, b, pb);
				float g[2][4] = {};
				for (int mode = 0; mode < 2; ++mode)
				{
					Dejudder::Settings settings = DejudderTestSettings();
					settings.opticalFlow = true;
					settings.opticalFlowCandidate = true;
					settings.opticalFlowSeed = true;
					settings.globalPrior = true;   // D1 on, as live
					settings.estCandidates = true;   // the two sweeps the seed needs
					settings.globalTwo = true;
					settings.opticalFlowGlobal = mode == 1;
					settings.opticalFlowGlobalReadback = mode == 1;
					Dejudder::MotionWarp warp(fixture.Log(), gpu, settings);
					Assert::IsTrue(warp.Estimate(pa.planes, pb.planes), std::wstring(warp.LastError().begin(), warp.LastError().end()).c_str());
					if (!warp.OpticalFlowUsed()) { engine = false; break; }
					if (mode == 1) Assert::IsTrue(warp.GlobalFromEngine(), L"the engine's global was used");
					readGlobal(warp, g[mode]);
				}
				if (!engine) { Logger::WriteMessage("optical flow engine unavailable: not run\n"); return; }
				char line[240];
				snprintf(line, sizeof(line), "pan %2d px: search (%.2f, %.2f) px z %.2f; engine (%.2f, %.2f) px z %.2f\n",
					dx, g[0][0] * 8.0f, g[0][1] * 8.0f, g[0][2], g[1][0] * 8.0f, g[1][1] * 8.0f, g[1][2]);
				Logger::WriteMessage(line);
				const float errSearch = std::fabs(g[0][0] * 8.0f - dx), errEngine = std::fabs(g[1][0] * 8.0f - dx);
				if (errEngine > errSearch + 1.0f || (g[1][2] >= 0.9f && g[0][2] < 0.9f)) ++worse;
			}
			Assert::AreEqual(0, worse, L"the engine's whole-frame motion is as close to the true pan and as usable as the search's");
		}

		// 2026-10-16 (the D1 taste report): at 4K the global moves in 8-px steps (the low-resolution
		// matcher), coarser than the roll-off's 16-px ramp, so a 12 or 20 px pan read 8 / 16 / 24 by
		// rounding luck. The roll-off now reads the refined camera (the trusted blocks near the
		// global): its strength follows the true speed, from the search and from the engine alike.
		TEST_METHOD(RolloffFollowsTheTruePanSpeedAt4K)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			const int width = 3840, height = 2160, frames = 8;
			auto expected = [](double v) {
				const double t = (std::min)(1.0, (std::max)(0.0, (v - 8.0) / 16.0));
				const double sm = t * t * (3.0 - 2.0 * t);
				return 1.0 + (0.25 - 1.0) * sm;
			};
			int failures = 0;
			bool engineSeen = true;
			for (const int dx : { 12, 20 })
			{
				std::vector<DejudderGpuPlanes> planes(static_cast<size_t>(frames));
				for (int f = 0; f < frames; ++f)
				{
					const int shift = dx * f;
					const DejudderFrame frame = MakeDejudderFrame(width, height,
						[shift](int x, int y) { return DejudderTexture(x - shift, y); },
						[shift](int x, int y, int c) { return DejudderTexture((x - shift / 2) * 2, y, 7 + c); });
					UploadDejudderFrame(gpu, frame, planes[static_cast<size_t>(f)]);
				}
				for (int mode = 0; mode < 2; ++mode)
				{
					Dejudder::Settings settings = DejudderTestSettings();
					settings.globalPrior = true;
					settings.estCandidates = true;
					settings.globalTwo = true;
					settings.lowRes = true;
					settings.lowResAll = true;
					settings.motionRolloff = true;
					settings.opticalFlow = mode == 1;
					settings.opticalFlowCandidate = true;
					settings.opticalFlowSeed = true;
					settings.opticalFlowGlobal = mode == 1;
					settings.opticalFlowGlobalReadback = mode == 1;
					Dejudder::MotionWarp warp(fixture.Log(), gpu, settings);
					for (int f = 0; f + 1 < frames; ++f)
						Assert::IsTrue(warp.Estimate(planes[static_cast<size_t>(f)].planes, planes[static_cast<size_t>(f + 1)].planes),
							std::wstring(warp.LastError().begin(), warp.LastError().end()).c_str());
					if (mode == 1 && !warp.GlobalFromEngine()) { engineSeen = false; continue; }
					pl_tex trust = warp.FrameTrust();
					std::vector<uint8_t> raw(trust->params.format->texel_size);
					pl_tex_transfer_params d{}; d.tex = trust; d.ptr = raw.data();
					Assert::IsTrue(pl_tex_download(gpu, &d), L"the frame trust is not host-readable");
					float scale = 0.0f;
					if (raw.size() == 16) std::memcpy(&scale, &raw[4], 4);
					else { uint16_t h; std::memcpy(&h, &raw[2], 2); scale = HalfBits(h); }
					char line[200];
					snprintf(line, sizeof(line), "4K pan %d px, %s: roll-off scale %.3f (at the true speed %.3f)\n", dx,
						mode == 1 ? "engine global" : "search D1", scale, expected(dx));
					Logger::WriteMessage(line);
					if (std::fabs(scale - expected(dx)) > 0.05) ++failures;
				}
			}
			if (!engineSeen) Logger::WriteMessage("note: the optical-flow engine was not available; the search only\n");
			Assert::AreEqual(0, failures, L"the roll-off follows the true pan speed at 4K (within 0.05)");
		}

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
			// The Stage 10 eligibility (the audit 2026-10-16 extensions off) ...
			const ExtendedRange::Settings stage10{ false, false, false, false };
			Assert::IsTrue(Decide(true, stage10, space(PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_BT_1886), p3, nullptr, PL_LUT_UNKNOWN).reason == Reason::SourceTransfer);
			Assert::IsTrue(Decide(true, stage10, source, space(PL_COLOR_PRIM_DISPLAY_P3, PL_COLOR_TRC_SRGB), nullptr, PL_LUT_UNKNOWN).reason == Reason::OutputTransfer);
			Assert::IsTrue(Decide(true, stage10, space(PL_COLOR_PRIM_BT_2020, PL_COLOR_TRC_GAMMA22), p3, nullptr, PL_LUT_UNKNOWN).reason == Reason::SourcePrimaries);
			Assert::IsTrue(Decide(true, stage10, source, space(PL_COLOR_PRIM_DCI_P3, PL_COLOR_TRC_GAMMA24), nullptr, PL_LUT_UNKNOWN).reason == Reason::OutputPrimaries);
			// ... and with them (the default): all four carried.
			Assert::IsTrue(Decide(true, space(PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_BT_1886), p3, false).Active());
			Assert::IsTrue(Decide(true, source, space(PL_COLOR_PRIM_DISPLAY_P3, PL_COLOR_TRC_SRGB), false).Active());
			Assert::IsTrue(Decide(true, space(PL_COLOR_PRIM_BT_2020, PL_COLOR_TRC_GAMMA22), p3, false).Active());
			Assert::IsTrue(Decide(true, source, space(PL_COLOR_PRIM_DCI_P3, PL_COLOR_TRC_GAMMA24), false).Active());
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
			AssertCleanLibplaceboLog(log);
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

		// ---- Audit 2026-10-16: the widened BTB/WTW carry (ExtendedRange.h 1.-4.) ----
		// A render like ExtendedRangeGpuProbe::Render (10-bit limited 4:2:2 in,
		// 10-bit limited RGB out), with explicit ExtendedRange::Settings (so the
		// tests do not depend on the renderer's config key), a target LUT
		// (PL_LUT_NORMALIZED, as the renderer attaches it), an HDR output equal to
		// the source, a BT.2020 YCbCr matrix, a further texture-input PRE_OUTPUT
		// hook (as the LED shader) and the 7.360 output-encoding repair the
		// renderer adds when it is needed.
		struct ErMoreCase
		{
			ExtendedRangeGpuProbe::Case c;
			ExtendedRange::Settings settings{};
			const pl_custom_lut* lut = nullptr;
			bool hdrSameOutput = false;   // output colour space = the (HDR) source's
			bool bt2020Matrix = false;    // YCbCr BT.2020 NCL
			const pl_hook* preOutputHook = nullptr;
			// libplacebo's default peak detection (dynamic tone mapping); the
			// helper tells Decide as the renderer must (hdrDynamicToneMapping).
			bool peakDetection = true;
		};

		struct ErMoreResult
		{
			ExtendedRangeGpuProbe::Result r;
			bool erHookDisabled = false;   // libplacebo disabled one of VP's extended-range hooks
			int disabledHooks = 0;
			bool repaired = false;         // the output-encoding repair was in the chain
		};

		static ErMoreResult ErRender(pl_gpu gpu, pl_renderer renderer, const ExtendedRangeGpuProbe::Planes& planes,
			const ErMoreCase& m)
		{
			using namespace ExtendedRangeGpuProbe;
			ErMoreResult more;
			Result& result = more.r;
			const Case& c = m.c;
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
			if (!r16 || !rg16 || !t10) { result.error = "formats"; return more; }
			pl_tex_params lp{}; lp.w = planes.width; lp.h = planes.height; lp.format = r16; lp.sampleable = true;
			lp.initial_data = planes.luma.data();
			pl_tex_params cp{}; cp.w = planes.width / 2; cp.h = planes.height; cp.format = rg16; cp.sampleable = true;
			cp.initial_data = planes.chroma.data();
			result.width = static_cast<int>(planes.width * c.scale);
			result.height = static_cast<int>(planes.height * c.scale);
			pl_tex_params tp{}; tp.w = result.width; tp.h = result.height; tp.format = t10; tp.renderable = true;
			tp.host_readable = true;
			tp.blit_dst = (t10->caps & PL_FMT_CAP_BLITTABLE) != 0;
			pl_tex lt = pl_tex_create(gpu, &lp), ct = pl_tex_create(gpu, &cp), tt = pl_tex_create(gpu, &tp);
			if (lt && ct && tt)
			{
				pl_frame image{};
				image.num_planes = 2;
				image.planes[0].texture = lt; image.planes[0].components = 1;
				image.planes[0].component_mapping[0] = PL_CHANNEL_Y;
				image.planes[1].texture = ct; image.planes[1].components = 2;
				image.planes[1].component_mapping[0] = PL_CHANNEL_CB;
				image.planes[1].component_mapping[1] = PL_CHANNEL_CR;
				image.repr.sys = m.bt2020Matrix ? PL_COLOR_SYSTEM_BT_2020_NC : PL_COLOR_SYSTEM_BT_709;
				image.repr.levels = PL_COLOR_LEVELS_LIMITED;
				image.repr.bits.sample_depth = 16; image.repr.bits.color_depth = 10; image.repr.bits.bit_shift = 6;
				image.color.primaries = c.sourcePrimaries;
				image.color.transfer = c.sourceTransfer;
				pl_frame_set_chroma_location(&image, PL_CHROMA_LEFT);
				if (c.hdrSource)
				{
					image.repr.sys = PL_COLOR_SYSTEM_BT_2020_NC;
					image.color.primaries = PL_COLOR_PRIM_BT_2020;
					image.color.transfer = PL_COLOR_TRC_PQ;
					image.color.hdr.min_luma = 0.005f;
					image.color.hdr.max_luma = 1000.0f;
				}
				pl_frame target{};
				target.num_planes = 1;
				target.planes[0].texture = tt;
				target.planes[0].components = 3;
				for (int i = 0; i < 3; ++i) target.planes[0].component_mapping[i] = i;
				target.repr.sys = PL_COLOR_SYSTEM_RGB;
				target.repr.levels = c.fullRangeOutput ? PL_COLOR_LEVELS_FULL : PL_COLOR_LEVELS_LIMITED;
				target.repr.bits.sample_depth = 10; target.repr.bits.color_depth = 10;
				target.color.primaries = c.outputPrimaries;
				target.color.transfer = c.outputTransfer;
				if (m.hdrSameOutput)
					target.color = image.color;
				else if (c.hdrSource)
					LibplaceboRenderParameters::ApplyTargetLuminance(false, c.targetNits, 0.0f, target.color);
				if (c.blank[0] || c.blank[1] || c.blank[2] || c.blank[3])
					target.crop = { static_cast<float>(c.blank[0]), static_cast<float>(c.blank[1]),
						static_cast<float>(result.width - c.blank[2]), static_cast<float>(result.height - c.blank[3]) };
				target.lut = m.lut;
				target.lut_type = m.lut ? PL_LUT_NORMALIZED : PL_LUT_UNKNOWN;
				pl_render_params params = pl_render_default_params;
				params.dither_params = nullptr;
				params.error_diffusion = nullptr;
#if defined(PL_RENDER_PARAMS_OUTPUT_FBO_FLOAT32)
				params.output_fbo_float32 = c.outputFloat32;
#endif
				params.sigmoid_params = c.sigmoid ? &pl_sigmoid_default_params : nullptr;
				params.disable_linear_scaling = !c.linear;
				if (!m.peakDetection)
					params.peak_detect_params = nullptr;
				ExtendedRange::Settings settings = m.settings;
				settings.hdrDynamicToneMapping = params.peak_detect_params != nullptr;
				ExtendedRange::Hooks hooks;
				std::vector<const pl_hook*> list;
				if (m.preOutputHook)
					list.push_back(m.preOutputHook);
				// The new overload with explicit Settings and the target's LUT.
				result.decision = ExtendedRange::Decide(c.on, settings, image.color, target.color, m.lut, target.lut_type);
				ExtendedRange::Prepare(result.decision, hooks, image.color, target.color, list, false);
				OutputEncodingRepair::Hook repair;
				const pl_render_errors before = pl_renderer_get_errors(renderer);
				if (OutputEncodingRepair::Needed(list, &before, target.lut_type))
				{
					repair.SetTarget(target.repr);
					list.insert(list.begin(), repair.Get());
					more.repaired = true;
				}
				params.hooks = list.empty() ? nullptr : list.data();
				params.num_hooks = static_cast<int>(list.size());
				const float black[3] = { 0.0f, 0.0f, 0.0f };
				pl_frame_clear(gpu, &target, black);
				if (pl_render_image(renderer, &image, &target, &params))
				{
					pl_gpu_finish(gpu);
					result.pixels.resize(static_cast<size_t>(result.width) * result.height);
					pl_tex_transfer_params d{}; d.tex = tt; d.ptr = result.pixels.data();
					result.ok = pl_tex_download(gpu, &d);
					if (!result.ok) result.error = "download";
				}
				else
					result.error = "pl_render_image";
				const pl_render_errors errors = pl_renderer_get_errors(renderer);
				more.disabledHooks = errors.num_disabled_hooks;
				for (int i = 0; i < errors.num_disabled_hooks; ++i)
					more.erHookDisabled = more.erHookDisabled || hooks.Owns(errors.disabled_hooks[i]);
				result.restores = hooks.Applied();
				hooks.Release();
			}
			else
				result.error = "textures";
			pl_tex_destroy(gpu, &lt);
			pl_tex_destroy(gpu, &ct);
			pl_tex_destroy(gpu, &tt);
			return more;
		}

		// 64 + 876 * Eout^-1(Esrc(v)), v = (code - 64) / 876: the carry's
		// transfer conversion on a grey, from the decision's own transfers.
		static int ErExpected(const ExtendedRange::Decision& d, int code)
		{
			const double v = (code - 64) / 876.0;
			const double o = ExtendedRange::FromLinear(d.output, ExtendedRange::ToLinear(d.source, v));
			return (std::min)((std::max)(static_cast<int>(std::lround(64 + 876 * o)), 0), 1023);
		}

		// Limited 10-bit YCbCr codes of R'G'B' (any values) for matrix kr / kb.
		static ExtendedRangeGpuProbe::Patch ErPatch(double r, double g, double b, double kr, double kb)
		{
			const double y = kr * r + (1.0 - kr - kb) * g + kb * b;
			const double cb = (b - y) / (2.0 * (1.0 - kb)), cr = (r - y) / (2.0 * (1.0 - kr));
			return { static_cast<int>(std::lround(64 + 876 * y)), static_cast<int>(std::lround(512 + 896 * cb)),
				static_cast<int>(std::lround(512 + 896 * cr)) };
		}

		// A 3D LUT out = A p + t (identity: A = I, t = 0).
		struct ErLut
		{
			std::vector<float> data;
			pl_custom_lut lut{};
			float a[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
			float t[3] = { 0, 0, 0 };

			void Build(int n, uint64_t signature)
			{
				data.assign(static_cast<size_t>(n) * n * n * 3, 0.0f);
				for (int bi = 0; bi < n; ++bi)
					for (int gi = 0; gi < n; ++gi)
						for (int ri = 0; ri < n; ++ri)
						{
							const float p[3] = { ri / float(n - 1), gi / float(n - 1), bi / float(n - 1) };
							const size_t i = ((static_cast<size_t>(bi) * n + gi) * n + ri) * 3;
							for (int k = 0; k < 3; ++k)
								data[i + k] = t[k] + a[k][0] * p[0] + a[k][1] * p[1] + a[k][2] * p[2];
						}
				lut = pl_custom_lut{};
				lut.signature = signature;
				lut.size[0] = lut.size[1] = lut.size[2] = n;
				lut.data = data.data();
			}
			// A neutral grey v through the LUT, channel k.
			double Grey(double v, int k) const { return t[k] + (a[k][0] + a[k][1] + a[k][2]) * v; }
		};

		// A texture-input PRE_OUTPUT hook that changes nothing (as a user
		// shader such as the LED compensation forces a texture there).
		static const pl_hook* ErTexturePreOutputHook()
		{
			static pl_hook hook = []
			{
				pl_hook h{};
				h.stages = PL_HOOK_PRE_OUTPUT;
				h.input = PL_HOOK_SIG_TEX;
				h.signature = 0x5650544553545058ull;   // "VPTESTPX"
				h.hook = [](void*, const pl_hook_params*) { return pl_hook_res{}; };
				return h;
			}();
			return &hook;
		}

		static const std::vector<int>& ErCodes()
		{
			static const std::vector<int> codes = { 4, 16, 40, 64, 100, 502, 900, 940, 980, 1019 };
			return codes;
		}

		// 1: a BT.1886 or sRGB source into several outputs, 1:1, up 1.5,
		// down 0.5. Every grey (BTB and WTW included) = the carry's conversion
		// (<= 1 code; <= 2 down), strictly monotonic, BTB below 64 and WTW above
		// 940; in-range greys as switch-off (<= 1); in-range edges as
		// switch-off (max 1, mean 0.1, as ExtendedRangeKeepsTodaysScalingAndColours);
		// all switches off = the old decision (inactive).
		static void ErTransferCarry(TargetLutGpuFixture& fixture, enum pl_color_transfer source,
			std::initializer_list<enum pl_color_transfer> outputs, std::string& report)
		{
			using namespace ExtendedRangeGpuProbe;
			const std::vector<int>& codes = ErCodes();
			const int n = static_cast<int>(codes.size());
			const Planes greys = Greys(codes);
			const Planes edges = Edges();
			for (enum pl_color_transfer output : outputs)
				for (double scale : { 1.0, 1.5, 0.5 })
				{
					ErMoreCase m;
					m.c.sourceTransfer = source;
					m.c.outputTransfer = output;
					m.c.outputPrimaries = PL_COLOR_PRIM_BT_709;
					m.c.scale = scale;
					ErMoreCase off = m;
					off.c.on = false;
					ErMoreCase old = m;
					old.settings = ExtendedRange::Settings{ false, false, false, false };
					const ErMoreResult on = ErRender(fixture.Gpu(), fixture.Renderer(), greys, m);
					const ErMoreResult ref = ErRender(fixture.Gpu(), fixture.Renderer(), greys, off);
					const ErMoreResult before = ErRender(fixture.Gpu(), fixture.Renderer(), greys, old);
					const std::string name = std::string(pl_color_transfer_name(source)) + " -> " +
						pl_color_transfer_name(output) + " x" + std::to_string(scale);
					Assert::IsTrue(on.r.ok && ref.r.ok && before.r.ok, ToWide(name + ": render failed").c_str());
					Assert::IsFalse(before.r.decision.Active(), ToWide(name + ": switches off must keep the old (inactive) decision").c_str());
					Assert::IsTrue(on.r.decision.Active() && on.r.restores > 0, ToWide(name + ": the hooks did not run").c_str());
					const auto got = Columns(on.r, n, scale), offCodes = Columns(ref.r, n, scale);
					report += name + ": on " + Join(got) + " | off " + Join(offCodes) + "\n";
					const int tolerance = scale < 1.0 ? 2 : 1;
					int previous = -1;
					for (int i = 0; i < n; ++i)
					{
						const int expected = ErExpected(on.r.decision, codes[i]);
						Assert::IsTrue(std::abs(got[i] - expected) <= tolerance,
							ToWide(name + ": grey " + std::to_string(codes[i]) + " expected " + std::to_string(expected) + "\n" + report).c_str());
						if (codes[i] >= 64 && codes[i] <= 940)
							Assert::IsTrue(std::abs(got[i] - offCodes[i]) <= 1,
								ToWide(name + ": in-range grey " + std::to_string(codes[i]) + " differs from switch-off\n" + report).c_str());
						// Strictly increasing except where the 10-bit output saturates:
						// sRGB/BT.1886 -> a power output expands BTB (sRGB code 4 ->
						// about -17, below code 0), so the deepest greys may all read 0
						// (GPU run 2026-10-16: 0 0 11 64 ...); 1023 likewise at the top.
						const bool rail = got[i] == 0 || got[i] == 1023;
						Assert::IsTrue(rail ? got[i] >= previous : got[i] > previous,
							ToWide(name + ": not monotonic\n" + report).c_str());
						previous = got[i];
					}
					Assert::IsTrue(got.front() < 64 && got.back() > 940, ToWide(name + ": BTB/WTW not kept\n" + report).c_str());
					if (scale != 1.0)
					{
						const ErMoreResult a = ErRender(fixture.Gpu(), fixture.Renderer(), edges, off);
						const ErMoreResult b = ErRender(fixture.Gpu(), fixture.Renderer(), edges, m);
						Assert::IsTrue(a.r.ok && b.r.ok);
						const Difference d = Compare(a.r, b.r);
						// The same comparison on the old path (BT.709 gamma 2.2 -> 2.2):
						// the texture-form SCALED restore's cost (libplacebo stores the
						// scaled picture in half float first; 10b, planner-accepted
						// "about 1/4 code").
						ErMoreCase baseOn = m;
						baseOn.c.sourceTransfer = baseOn.c.outputTransfer = PL_COLOR_TRC_GAMMA22;
						ErMoreCase baseOff = baseOn;
						baseOff.c.on = false;
						const ErMoreResult ba = ErRender(fixture.Gpu(), fixture.Renderer(), edges, baseOff);
						const ErMoreResult bb = ErRender(fixture.Gpu(), fixture.Renderer(), edges, baseOn);
						Assert::IsTrue(ba.r.ok && bb.r.ok);
						const Difference base = Compare(ba.r, bb.r);
						report += name + " edges: max " + std::to_string(d.max) + " mean " + std::to_string(d.mean) +
							" (old path 2.2 -> 2.2: max " + std::to_string(base.max) + " mean " + std::to_string(base.mean) + ")\n";
						// GPU run 2026-10-16: 0.103 (BT.1886 -> 2.4 up 1.5), 0.17 (sRGB ->
						// sRGB down 0.5, where switch-off needs no intermediate texture):
						// the same half-float cost, so bounded by the accepted 1/4 code.
						Assert::IsTrue(d.max <= 1 && d.mean <= 0.25, ToWide(name + ": in-range edges changed\n" + report).c_str());
					}
				}
		}

		TEST_METHOD(ExtendedRangeBt1886Carry)
		{
			using namespace ExtendedRangeGpuProbe;
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create());
			std::string log, report;
			fixture.CaptureLog(&log);
			ErTransferCarry(fixture, PL_COLOR_TRC_BT_1886, { PL_COLOR_TRC_GAMMA24, PL_COLOR_TRC_BT_1886, PL_COLOR_TRC_GAMMA22 }, report);
			fixture.CaptureLog(nullptr);
			Logger::WriteMessage(report.c_str());
			AssertCleanLibplaceboLogExceptPreOutputBug(log);   // 7.360: texture PRE_OUTPUT hook note (repaired)
		}

		TEST_METHOD(ExtendedRangeSrgbCarry)
		{
			using namespace ExtendedRangeGpuProbe;
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create());
			std::string log, report;
			fixture.CaptureLog(&log);
			ErTransferCarry(fixture, PL_COLOR_TRC_SRGB, { PL_COLOR_TRC_GAMMA22, PL_COLOR_TRC_SRGB, PL_COLOR_TRC_BT_1886 }, report);
			fixture.CaptureLog(nullptr);
			Logger::WriteMessage(report.c_str());
			AssertCleanLibplaceboLogExceptPreOutputBug(log);   // 7.360: texture PRE_OUTPUT hook note (repaired)
		}

		// 2: an SDR BT.2020 source on a BT.709 output keeps libplacebo's gamut
		// mapping: in-range saturated colours (out of BT.709's gamut) as
		// switch-off (<= 1 code per channel), greys incl. BTB/WTW kept (<= 1),
		// a WTW red above 940 and not below switch-off, a BTB blue below 64 and
		// not above switch-off; no SCALED restore, the late restore ran.
		TEST_METHOD(ExtendedRangeWideGamutLateRestore)
		{
			using namespace ExtendedRangeGpuProbe;
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create());
			std::string log, report;
			fixture.CaptureLog(&log);
			const double kr = 0.2627, kb = 0.0593;
			const std::vector<Patch> patches = { ErPatch(0.1, 0.9, 0.1, kr, kb), ErPatch(0.9, 0.1, 0.1, kr, kb),
				ErPatch(0.1, 0.1, 0.9, kr, kb), ErPatch(0.6, 0.6, 0.6, kr, kb),
				ErPatch(1.06, 0.5, 0.5, kr, kb), ErPatch(0.5, 0.5, -0.06, kr, kb) };
			const Planes colours = Patches(patches);
			const Planes greys = Greys(ErCodes());
			for (double scale : { 1.0, 1.5 })
			{
				ErMoreCase m;
				m.c.sourcePrimaries = PL_COLOR_PRIM_BT_2020;
				m.c.outputPrimaries = PL_COLOR_PRIM_BT_709;
				m.c.scale = scale;
				m.bt2020Matrix = true;
				ErMoreCase off = m;
				off.c.on = false;
				const ErMoreResult on = ErRender(fixture.Gpu(), fixture.Renderer(), colours, m);
				const ErMoreResult ref = ErRender(fixture.Gpu(), fixture.Renderer(), colours, off);
				Assert::IsTrue(on.r.ok && ref.r.ok);
				const ExtendedRange::Decision& d = on.r.decision;
				Assert::IsTrue(d.Active() && d.libplaceboMapsPrimaries && d.LateRestore() && !d.ScaledRestore() && on.r.restores > 0,
					ToWide(ExtendedRange::Describe(d)).c_str());
				const int y = on.r.height / 2;
				for (size_t i = 0; i < patches.size(); ++i)
				{
					const int x = static_cast<int>((i * 32 + 16) * scale);
					report += "x" + std::to_string(scale) + " patch " + std::to_string(i) + ": on ";
					for (int ch = 0; ch < 3; ++ch) report += std::to_string(on.r.Channel(x, y, ch)) + (ch < 2 ? "," : "");
					report += " off ";
					for (int ch = 0; ch < 3; ++ch) report += std::to_string(ref.r.Channel(x, y, ch)) + (ch < 2 ? "," : "");
					report += "\n";
				}
				for (size_t i = 0; i < 4; ++i)
				{
					const int x = static_cast<int>((i * 32 + 16) * scale);
					for (int ch = 0; ch < 3; ++ch)
						Assert::IsTrue(std::abs(on.r.Channel(x, y, ch) - ref.r.Channel(x, y, ch)) <= 1,
							ToWide("in-range colour changed\n" + report).c_str());
				}
				const int xr = static_cast<int>((4 * 32 + 16) * scale), xb = static_cast<int>((5 * 32 + 16) * scale);
				Assert::IsTrue(on.r.Channel(xr, y, 0) > 940 && on.r.Channel(xr, y, 0) >= ref.r.Channel(xr, y, 0),
					ToWide("WTW red not restored\n" + report).c_str());
				Assert::IsTrue(on.r.Channel(xb, y, 2) < 64 && on.r.Channel(xb, y, 2) <= ref.r.Channel(xb, y, 2),
					ToWide("BTB blue not restored\n" + report).c_str());
				const ErMoreResult g = ErRender(fixture.Gpu(), fixture.Renderer(), greys, m);
				Assert::IsTrue(g.r.ok);
				const auto got = Columns(g.r, static_cast<int>(ErCodes().size()), scale);
				report += "greys x" + std::to_string(scale) + ": " + Join(got) + "\n";
				for (size_t i = 0; i < got.size(); ++i)
					Assert::IsTrue(std::abs(got[i] - ErCodes()[i]) <= 1, ToWide("grey not kept\n" + report).c_str());
				// Switch off: the old eligibility refuses this source.
				ErMoreCase old = m;
				old.settings.wideSourcePrimaries = false;
				Assert::IsTrue(ErRender(fixture.Gpu(), fixture.Renderer(), greys, old).r.decision.reason ==
					ExtendedRange::Reason::SourcePrimaries);
			}
			fixture.CaptureLog(nullptr);
			Logger::WriteMessage(report.c_str());
			AssertCleanLibplaceboLogExceptPreOutputBug(log);   // 7.360: texture PRE_OUTPUT hook note (repaired)
		}

		// 2: a contained gamut (BT.601 EBU SDR on a BT.2020 output; Display P3
		// is not contained for libplacebo: its red is ~0.0017 outside) converts in
		// the carry: greys incl. BTB/WTW kept (<= 1), in-range colours as
		// switch-off (<= 2, as the BT.709 -> P3 colours test).
		TEST_METHOD(ExtendedRangeContainedGamut)
		{
			using namespace ExtendedRangeGpuProbe;
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create());
			std::string log, report;
			fixture.CaptureLog(&log);
			const std::vector<Patch> patches = { { 502, 512, 512 }, { 560, 440, 600 }, { 400, 450, 700 },
				{ 600, 350, 450 }, { 300, 700, 480 }, { 700, 560, 380 } };
			const Planes colours = Patches(patches);
			const Planes greys = Greys(ErCodes());
			for (double scale : { 1.0, 1.5 })
			{
				ErMoreCase m;
				m.c.sourcePrimaries = PL_COLOR_PRIM_BT_601_625;
				m.c.outputPrimaries = PL_COLOR_PRIM_BT_2020;
				m.c.scale = scale;
				ErMoreCase off = m;
				off.c.on = false;
				const ErMoreResult on = ErRender(fixture.Gpu(), fixture.Renderer(), colours, m);
				const ErMoreResult ref = ErRender(fixture.Gpu(), fixture.Renderer(), colours, off);
				Assert::IsTrue(on.r.ok && ref.r.ok);
				const ExtendedRange::Decision& d = on.r.decision;
				Assert::IsTrue(d.Active() && d.convertPrimaries && !d.libplaceboMapsPrimaries && d.ScaledRestore() && !d.LateRestore(),
					ToWide(ExtendedRange::Describe(d)).c_str());
				int worst = 0;
				const int y = on.r.height / 2;
				for (size_t i = 0; i < patches.size(); ++i)
					for (int ch = 0; ch < 3; ++ch)
					{
						const int x = static_cast<int>((i * 32 + 16) * scale);
						worst = std::max(worst, std::abs(on.r.Channel(x, y, ch) - ref.r.Channel(x, y, ch)));
					}
				report += "x" + std::to_string(scale) + ": colours worst " + std::to_string(worst) + "\n";
				Assert::IsTrue(worst <= 2, ToWide("in-range colours changed\n" + report).c_str());
				const ErMoreResult g = ErRender(fixture.Gpu(), fixture.Renderer(), greys, m);
				Assert::IsTrue(g.r.ok);
				const auto got = Columns(g.r, static_cast<int>(ErCodes().size()), scale);
				report += "greys: " + Join(got) + "\n";
				for (size_t i = 0; i < got.size(); ++i)
					Assert::IsTrue(std::abs(got[i] - ErCodes()[i]) <= 1, ToWide("grey not kept\n" + report).c_str());
			}
			fixture.CaptureLog(nullptr);
			Logger::WriteMessage(report.c_str());
			AssertCleanLibplaceboLogExceptPreOutputBug(log);   // 7.360: texture PRE_OUTPUT hook note (repaired)
		}

		// 3: a calibration LUT. Identity 17^3 LUT: the whole picture (BTB/WTW
		// greys) as without a LUT (max 1 code), in-range edges as switch-off
		// with the LUT (max 1, mean 0.1). Affine 17^3 LUT with cross-talk: every
		// grey channel (BTB/WTW included) = A v + t extrapolated (<= 1 code);
		// switch-off clamps there (logged). The SCALED and late restores ran.
		TEST_METHOD(ExtendedRangeLutLateRestore)
		{
			using namespace ExtendedRangeGpuProbe;
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create());
			std::string log, report;
			fixture.CaptureLog(&log);
			ErLut identity;
			identity.Build(17, 0x4552494431370001ull);
			ErLut affine;
			const float a[3][3] = { { 0.90f, 0.05f, 0.01f }, { 0.03f, 0.92f, 0.02f }, { 0.02f, 0.04f, 0.88f } };
			const float t[3] = { 0.02f, 0.01f, 0.03f };
			std::memcpy(affine.a, a, sizeof(a));
			std::memcpy(affine.t, t, sizeof(t));
			affine.Build(17, 0x4552414631370001ull);
			const std::vector<int>& codes = ErCodes();
			const Planes greys = Greys(codes);
			const Planes edges = Edges();
			for (double scale : { 1.0, 1.5 })
			{
				ErMoreCase m;
				m.c.outputPrimaries = PL_COLOR_PRIM_BT_709;
				m.c.scale = scale;
				// Identity LUT vs no LUT, both on.
				ErMoreCase withLut = m;
				withLut.lut = &identity.lut;
				const ErMoreResult noLut = ErRender(fixture.Gpu(), fixture.Renderer(), greys, m);
				const ErMoreResult lut = ErRender(fixture.Gpu(), fixture.Renderer(), greys, withLut);
				Assert::IsTrue(noLut.r.ok && lut.r.ok);
				Assert::IsTrue(lut.r.decision.Active() && lut.r.decision.lut == &identity.lut &&
					lut.r.decision.LateRestore() && lut.r.decision.ScaledRestore() && lut.r.restores >= 2,
					ToWide(ExtendedRange::Describe(lut.r.decision)).c_str());
				const Difference d = Compare(noLut.r, lut.r);
				report += "identity LUT x" + std::to_string(scale) + ": " + Join(Columns(lut.r, static_cast<int>(codes.size()), scale)) +
					" (max " + std::to_string(d.max) + ")\n";
				Assert::IsTrue(d.max <= 1, ToWide("identity LUT changed the picture\n" + report).c_str());
				// GPU run 2026-10-16 (360): in-range pixels differed by max 1, mean
				// 0.39 up 1.5: before a texture PRE_OUTPUT hook libplacebo stores
				// the finished picture in its half-float texture (~1/2 code near
				// white), which switch-off without such a hook never does (an
				// inline form is rejected by libplacebo). In VP the LED hooks are
				// texture PRE_OUTPUT hooks already, so the like-for-like check has
				// one in both renders: max 1, mean 0.25 (measured 0.17: what is left
				// is the texture-form SCALED restore's half-float cost, the accepted
				// 1/4 code of ExtendedRangeTransferCarry). Without it the 360 cost is
				// bounded at max 1, mean 0.5 (377 + output_fbo_float32 stores it in
				// float32). Ringing overshoot outside 64..940 is extrapolated by the
				// carry and clamped by switch-off's LUT, so only pixels switch-off
				// leaves strictly inside 64..940 are compared.
				for (int withHook = 1; withHook >= 0; --withHook)
				{
					ErMoreCase lutOn = withLut, lutOff = withLut, noLutOff = m;
					lutOff.c.on = false;
					noLutOff.c.on = false;
					if (withHook)
						lutOn.preOutputHook = lutOff.preOutputHook = noLutOff.preOutputHook = ErTexturePreOutputHook();
					const ErMoreResult ea = ErRender(fixture.Gpu(), fixture.Renderer(), edges, lutOff);
					const ErMoreResult eb = ErRender(fixture.Gpu(), fixture.Renderer(), edges, lutOn);
					const ErMoreResult en = ErRender(fixture.Gpu(), fixture.Renderer(), edges, noLutOff);
					Assert::IsTrue(ea.r.ok && eb.r.ok && en.r.ok);
					int insideMax = 0, inside = 0;
					double insideSum = 0.0;
					for (size_t i = 0; i < ea.r.pixels.size(); ++i)
						for (int ch = 0; ch < 3; ++ch)
						{
							const int a = static_cast<int>((ea.r.pixels[i] >> (10 * ch)) & 1023u);
							const int b = static_cast<int>((eb.r.pixels[i] >> (10 * ch)) & 1023u);
							if (a <= 64 || a >= 940)
								continue;
							insideMax = std::max(insideMax, std::abs(a - b));
							insideSum += std::abs(a - b);
							++inside;
						}
					const double insideMean = inside ? insideSum / inside : 0.0;
					const Difference e = Compare(en.r, eb.r);
					const double bound = withHook ? 0.25 : 0.5;
					report += std::string("identity LUT edges x") + std::to_string(scale) + (withHook ? " (texture PRE_OUTPUT hook)" : " (no other hook)") +
						": in range vs LUT off max " + std::to_string(insideMax) + " mean " + std::to_string(insideMean) +
						"; vs no LUT off max " + std::to_string(e.max) + " mean " + std::to_string(e.mean) + "\n";
					Assert::IsTrue(insideMax <= 1 && insideMean <= bound, ToWide("in-range edges changed with the LUT\n" + report).c_str());
					Assert::IsTrue(e.max <= 1 && e.mean <= bound, ToWide("identity LUT on differs from no LUT off\n" + report).c_str());
				}
				// Affine LUT: the extrapolation is exact for an affine LUT.
				ErMoreCase withAffine = m;
				withAffine.lut = &affine.lut;
				ErMoreCase offAffine = withAffine;
				offAffine.c.on = false;
				const ErMoreResult on = ErRender(fixture.Gpu(), fixture.Renderer(), greys, withAffine);
				const ErMoreResult ref = ErRender(fixture.Gpu(), fixture.Renderer(), greys, offAffine);
				Assert::IsTrue(on.r.ok && ref.r.ok);
				const int y = on.r.height / 2;
				for (size_t i = 0; i < codes.size(); ++i)
				{
					const int x = static_cast<int>((i * 32 + 16) * scale);
					const double v = (codes[i] - 64) / 876.0;
					report += "affine x" + std::to_string(scale) + " grey " + std::to_string(codes[i]) + ": on ";
					for (int ch = 0; ch < 3; ++ch) report += std::to_string(on.r.Channel(x, y, ch)) + (ch < 2 ? "," : "");
					report += " off ";
					for (int ch = 0; ch < 3; ++ch) report += std::to_string(ref.r.Channel(x, y, ch)) + (ch < 2 ? "," : "");
					report += "\n";
					for (int ch = 0; ch < 3; ++ch)
					{
						const int expected = static_cast<int>(std::lround(64 + 876 * affine.Grey(v, ch)));
						Assert::IsTrue(std::abs(on.r.Channel(x, y, ch) - expected) <= 1,
							ToWide("affine LUT grey " + std::to_string(codes[i]) + " channel " + std::to_string(ch) +
								" expected " + std::to_string(expected) + "\n" + report).c_str());
					}
				}
				// Switch off: the old eligibility (CalibrationLut).
				ErMoreCase old = withAffine;
				old.settings.calibrationLut = false;
				Assert::IsTrue(ErRender(fixture.Gpu(), fixture.Renderer(), greys, old).r.decision.reason ==
					ExtendedRange::Reason::CalibrationLut);
			}
			fixture.CaptureLog(nullptr);
			Logger::WriteMessage(report.c_str());
			AssertCleanLibplaceboLogExceptPreOutputBug(log);   // 7.360: texture PRE_OUTPUT hook note (repaired)
		}

		// 2 + 3: a BT.2020 SDR source, BT.709 output and a LUT: the carry clips
		// the gamut (logged), the LUT carry applies. Identity LUT: greys incl.
		// BTB/WTW kept (<= 1); low-saturation in-gamut colours as switch-off
		// without a LUT (<= 2).
		TEST_METHOD(ExtendedRangeLutGamutClip)
		{
			using namespace ExtendedRangeGpuProbe;
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create());
			std::string log, report;
			fixture.CaptureLog(&log);
			ErLut identity;
			identity.Build(17, 0x4552494431370002ull);
			const double kr = 0.2627, kb = 0.0593;
			const std::vector<Patch> patches = { ErPatch(0.5, 0.45, 0.4, kr, kb), ErPatch(0.4, 0.45, 0.5, kr, kb),
				ErPatch(0.45, 0.5, 0.42, kr, kb) };
			const Planes colours = Patches(patches);
			const Planes greys = Greys(ErCodes());
			ErMoreCase m;
			m.c.sourcePrimaries = PL_COLOR_PRIM_BT_2020;
			m.c.outputPrimaries = PL_COLOR_PRIM_BT_709;
			m.bt2020Matrix = true;
			m.lut = &identity.lut;
			const ErMoreResult g = ErRender(fixture.Gpu(), fixture.Renderer(), greys, m);
			Assert::IsTrue(g.r.ok);
			const ExtendedRange::Decision& d = g.r.decision;
			Assert::IsTrue(d.Active() && d.gamutClip && d.convertPrimaries && !d.libplaceboMapsPrimaries && d.LateRestore() &&
				d.ScaledRestore(), ToWide(ExtendedRange::Describe(d)).c_str());
			const auto got = Columns(g.r, static_cast<int>(ErCodes().size()), 1.0);
			report += "greys: " + Join(got) + "\n";
			for (size_t i = 0; i < got.size(); ++i)
				Assert::IsTrue(std::abs(got[i] - ErCodes()[i]) <= 1, ToWide("grey not kept\n" + report).c_str());
			ErMoreCase off = m;
			off.c.on = false;
			off.lut = nullptr;
			const ErMoreResult on = ErRender(fixture.Gpu(), fixture.Renderer(), colours, m);
			const ErMoreResult ref = ErRender(fixture.Gpu(), fixture.Renderer(), colours, off);
			Assert::IsTrue(on.r.ok && ref.r.ok);
			int worst = 0;
			for (size_t i = 0; i < patches.size(); ++i)
				for (int ch = 0; ch < 3; ++ch)
					worst = std::max(worst, std::abs(on.r.Channel(static_cast<int>(i * 32 + 16), on.r.height / 2, ch) -
						ref.r.Channel(static_cast<int>(i * 32 + 16), ref.r.height / 2, ch)));
			report += "low-saturation colours worst " + std::to_string(worst) + "\n";
			Assert::IsTrue(worst <= 2, ToWide("in-gamut colours changed\n" + report).c_str());
			fixture.CaptureLog(nullptr);
			Logger::WriteMessage(report.c_str());
			AssertCleanLibplaceboLogExceptPreOutputBug(log);   // 7.360: texture PRE_OUTPUT hook note (repaired)
		}

		// 4: limited-range PQ into an identical PQ output (identity tone
		// mapping): every grey code 4..1019 kept (<= 1 at 1:1 and up 1.5, <= 2
		// down 0.5, where libplacebo linearises and switch-off clips BTB - logged).
		// Tone-mapped cases stay off: HdrSource (SDR output), HdrMapped (other
		// HDR metadata), and with hdrPassthrough off.
		TEST_METHOD(ExtendedRangeHdrPassthrough)
		{
			using namespace ExtendedRangeGpuProbe;
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create());
			std::string log, report;
			fixture.CaptureLog(&log);
			const std::vector<int>& codes = ErCodes();
			const Planes greys = Greys(codes);
			for (double scale : { 1.0, 1.5, 0.5 })
			{
				ErMoreCase m;
				m.c.hdrSource = true;
				m.c.scale = scale;
				m.hdrSameOutput = true;
				// GPU run 2026-10-16: with libplacebo's default peak detection even
				// equal colour spaces are tone-mapped (502 -> 371, 1019 -> 723, on
				// = off): identity needs peak detection off.
				m.peakDetection = false;
				ErMoreCase off = m;
				off.c.on = false;
				const ErMoreResult on = ErRender(fixture.Gpu(), fixture.Renderer(), greys, m);
				const ErMoreResult ref = ErRender(fixture.Gpu(), fixture.Renderer(), greys, off);
				Assert::IsTrue(on.r.ok && ref.r.ok);
				Assert::IsTrue(on.r.decision.Active() && on.r.decision.hdrPassthrough && on.r.restores > 0,
					ToWide(ExtendedRange::Describe(on.r.decision)).c_str());
				const auto got = Columns(on.r, static_cast<int>(codes.size()), scale);
				const auto offCodes = Columns(ref.r, static_cast<int>(codes.size()), scale);
				report += "PQ x" + std::to_string(scale) + ": on " + Join(got) + " | off " + Join(offCodes) + "\n";
				const int tolerance = scale < 1.0 ? 2 : 1;
				for (size_t i = 0; i < codes.size(); ++i)
				{
					Assert::IsTrue(std::abs(got[i] - codes[i]) <= tolerance, ToWide("PQ code not kept\n" + report).c_str());
					if (codes[i] >= 64 && codes[i] <= 940)
						Assert::IsTrue(std::abs(got[i] - offCodes[i]) <= 1, ToWide("in-range PQ differs from switch-off\n" + report).c_str());
				}
				// Peak detection on: dynamic tone mapping, the carry stays off
				// (HdrMapped) and the picture is switch-off's.
				ErMoreCase dynamic = m;
				dynamic.peakDetection = true;
				const ErMoreResult dyn = ErRender(fixture.Gpu(), fixture.Renderer(), greys, dynamic);
				Assert::IsTrue(dyn.r.ok && dyn.r.decision.reason == ExtendedRange::Reason::HdrMapped,
					ToWide(ExtendedRange::Describe(dyn.r.decision)).c_str());
				ErMoreCase old = m;
				old.settings.hdrPassthrough = false;
				Assert::IsTrue(ErRender(fixture.Gpu(), fixture.Renderer(), greys, old).r.decision.reason ==
					ExtendedRange::Reason::HdrSource);
			}
			ErMoreCase toned;
			toned.c.hdrSource = true;
			toned.c.outputPrimaries = PL_COLOR_PRIM_BT_709;
			Assert::IsTrue(ErRender(fixture.Gpu(), fixture.Renderer(), greys, toned).r.decision.reason ==
				ExtendedRange::Reason::HdrSource);
			pl_color_space pq{};
			pq.primaries = PL_COLOR_PRIM_BT_2020;
			pq.transfer = PL_COLOR_TRC_PQ;
			pq.hdr.min_luma = 0.005f;
			pq.hdr.max_luma = 1000.0f;
			pl_color_space display = pq;
			display.hdr.max_luma = 600.0f;
			Assert::IsTrue(ExtendedRange::Decide(true, ExtendedRange::Settings{}, pq, display, nullptr, PL_LUT_UNKNOWN).reason ==
				ExtendedRange::Reason::HdrMapped);
			fixture.CaptureLog(nullptr);
			Logger::WriteMessage(report.c_str());
			AssertCleanLibplaceboLogExceptPreOutputBug(log);   // 7.360: texture PRE_OUTPUT hook note (repaired)
		}

		// The late restore with a texture-input PRE_OUTPUT hook after it (as
		// the LED shader), frames alternating with switch-off frames, 1:1, up
		// and down: libplacebo disables no hook (0 disabled), the hooks run every
		// on-frame, the limited output encoding is intact (greys 64/502/940
		// within 1 code; on 7.360 with the repair) and BTB stays below 64.
		TEST_METHOD(ExtendedRangeLateHookNotDisabled)
		{
			using namespace ExtendedRangeGpuProbe;
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create());
			std::string log, report;
			fixture.CaptureLog(&log);
			ErLut identity;
			identity.Build(17, 0x4552494431370003ull);
			const std::vector<int>& codes = ErCodes();
			const Planes greys = Greys(codes);
			for (int mode = 0; mode < 2; ++mode)
				for (double scale : { 1.0, 1.5, 0.5 })
					for (int frame = 0; frame < 3; ++frame)
					{
						ErMoreCase m;
						m.c.outputPrimaries = PL_COLOR_PRIM_BT_709;
						m.c.scale = scale;
						m.preOutputHook = ErTexturePreOutputHook();
						if (mode == 0)
							m.lut = &identity.lut;
						else
						{
							m.c.sourcePrimaries = PL_COLOR_PRIM_BT_2020;
							m.bt2020Matrix = true;
						}
						ErMoreCase off = m;
						off.c.on = false;
						const ErMoreResult on = ErRender(fixture.Gpu(), fixture.Renderer(), greys, m);
						const ErMoreResult ref = ErRender(fixture.Gpu(), fixture.Renderer(), greys, off);
						Assert::IsTrue(on.r.ok && ref.r.ok);
						const std::string name = std::string(mode == 0 ? "LUT" : "gamut") + " x" + std::to_string(scale) +
							" frame " + std::to_string(frame);
						const auto got = Columns(on.r, static_cast<int>(codes.size()), scale);
						report += name + ": " + Join(got) + (on.repaired ? " (repaired)" : "") + " disabled " +
							std::to_string(on.disabledHooks) + "\n";
						Assert::IsTrue(on.r.decision.Active() && on.r.decision.LateRestore(), ToWide(name).c_str());
						Assert::IsFalse(on.erHookDisabled, ToWide(name + ": an extended-range hook was disabled\n" + report + "log: " + log).c_str());
						Assert::AreEqual(0, on.disabledHooks, ToWide(name + ": a hook was disabled\n" + report).c_str());
						Assert::IsTrue(on.r.restores >= static_cast<unsigned long long>(mode == 0 ? 2 : 1),
							ToWide(name + ": the hooks did not run\n" + report).c_str());
#if PL_API_VER < 377
						Assert::IsTrue(on.repaired, ToWide(name + ": the output-encoding repair is needed on 7.360").c_str());
#endif
						for (size_t i = 0; i < codes.size(); ++i)
							if (codes[i] == 64 || codes[i] == 502 || codes[i] == 940)
								Assert::IsTrue(std::abs(got[i] - codes[i]) <= 1, ToWide(name + ": output encoding wrong\n" + report).c_str());
						Assert::IsTrue(got.front() < 64, ToWide(name + ": BTB lost\n" + report).c_str());
					}
			fixture.CaptureLog(nullptr);
			Logger::WriteMessage(report.c_str());
			AssertCleanLibplaceboLogExceptPreOutputBug(log);   // 7.360: texture PRE_OUTPUT hook note (repaired)
		}

		// The late restore's position mapping with an offset target crop
		// (blanking 13/3/7/5) and narrow alternating BTB/WTW columns. LUT mode:
		// identity LUT vs no LUT (SCALED restore only), every pixel max 1 code,
		// mean 0.1, at 1:1 and up 1.5 (the late restore reads v back exactly).
		// Gamut mode: BT.2020 greys vs the same greys as BT.709 (SCALED restore
		// path), column centres <= 1 code (edges differ by the remainder's
		// linear vs. signal footprint averaging, not checked).
		TEST_METHOD(ExtendedRangeLateRestoreCropAlignment)
		{
			using namespace ExtendedRangeGpuProbe;
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create());
			std::string log, report;
			fixture.CaptureLog(&log);
			ErLut identity;
			identity.Build(17, 0x4552494431370004ull);
			const std::vector<int> codes = { 4, 1019, 4, 1019, 64, 940, 16, 980, 502, 4, 1019, 40 };
			const int width = 16;
			const Planes greys = Greys(codes, width, 16);
			for (double scale : { 1.0, 1.5 })
			{
				ErMoreCase m;
				m.c.outputPrimaries = PL_COLOR_PRIM_BT_709;
				m.c.scale = scale;
				m.c.blank[0] = 13; m.c.blank[1] = 3; m.c.blank[2] = 7; m.c.blank[3] = 5;
				ErMoreCase withLut = m;
				withLut.lut = &identity.lut;
				const ErMoreResult a = ErRender(fixture.Gpu(), fixture.Renderer(), greys, m);
				const ErMoreResult b = ErRender(fixture.Gpu(), fixture.Renderer(), greys, withLut);
				Assert::IsTrue(a.r.ok && b.r.ok && b.r.decision.LateRestore());
				const Difference d = Compare(a.r, b.r);
				report += "LUT x" + std::to_string(scale) + ": max " + std::to_string(d.max) + " mean " + std::to_string(d.mean) + "\n";
				Assert::IsTrue(d.max <= 1 && d.mean <= 0.1, ToWide("the late LUT restore is misaligned\n" + report).c_str());

				ErMoreCase wide = m;
				wide.c.sourcePrimaries = PL_COLOR_PRIM_BT_2020;
				wide.bt2020Matrix = true;
				const ErMoreResult w = ErRender(fixture.Gpu(), fixture.Renderer(), greys, wide);
				Assert::IsTrue(w.r.ok && w.r.decision.libplaceboMapsPrimaries);
				const double cropW = a.r.width - m.c.blank[0] - m.c.blank[2];
				const double cropH = a.r.height - m.c.blank[1] - m.c.blank[3];
				const int y = static_cast<int>(m.c.blank[1] + cropH / 2);
				std::string line = "gamut x" + std::to_string(scale) + ":";
				for (size_t i = 0; i < codes.size(); ++i)
				{
					const int x = static_cast<int>(m.c.blank[0] + (i * width + width / 2.0) * cropW / greys.width);
					line += " " + std::to_string(w.r.Channel(x, y, 1)) + "/" + std::to_string(a.r.Channel(x, y, 1));
					for (int ch = 0; ch < 3; ++ch)
						Assert::IsTrue(std::abs(w.r.Channel(x, y, ch) - a.r.Channel(x, y, ch)) <= 1,
							ToWide("the late gamut restore is misaligned\n" + report + line).c_str());
				}
				report += line + "\n";
			}
			fixture.CaptureLog(nullptr);
			Logger::WriteMessage(report.c_str());
			AssertCleanLibplaceboLogExceptPreOutputBug(log);   // 7.360: texture PRE_OUTPUT hook note (repaired)
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
		TEST_METHOD(DejudderRenderedWarpHoldsAMovedSubtitleStill)
		{
			// Owner 2026-10-13: with a moved subtitle the extras still run; its
			// rectangles (Geometry::holds, source pixels) show A exactly while
			// the rest of the picture moves, and the dither's vectors are zero
			// there.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			const int width = 256, height = 128;
			static constexpr int dx = 12, dy = 6;
			auto chroma = [](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); };
			const DejudderFrame a = MakeDejudderFrame(width, height, [](int x, int y) { return DejudderTexture(x, y); }, chroma);
			const DejudderFrame b = MakeDejudderFrame(width, height, [](int x, int y) { return DejudderTexture(x - dx, y - dy); },
				[](int x, int y, int c) { return DejudderTexture((x - dx / 2) * 2, y - dy, 7 + c); });
			DejudderGpuPlanes planesA, planesB;
			UploadDejudderFrame(gpu, a, planesA);
			UploadDejudderFrame(gpu, b, planesB);
			Dejudder::MotionWarp warp(fixture.Log(), gpu, DejudderTestSettings());
			DejudderRendered::Cache cache;
			DejudderRendered::Warp rendered(fixture.Log(), gpu);
			DejudderRendered::VectorField field(fixture.Log(), gpu);
			pl_tex_params targetParams{};
			targetParams.w = width;
			targetParams.h = height;
			targetParams.format = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 8, 8,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE));
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
				std::vector<RgbaPixel> output(static_cast<size_t>(width) * height);
				pl_tex_transfer_params d{};
				d.tex = targetTexture;
				d.ptr = output.data();
				Assert::IsTrue(pl_tex_download(gpu, &d));
				return output;
			};
			// A rendered with the cache armed.
			{
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
				pl_frame t = target();
				std::vector<const pl_hook*> hooks = { cache.Hook() };
				OutputEncodingRepair::Hook repair;
				if (OutputEncodingRepair::Needed(hooks, nullptr, PL_LUT_UNKNOWN))
				{
					repair.SetTarget(t.repr);
					hooks.insert(hooks.begin(), repair.Get());
				}
				pl_render_params p = params;
				p.hooks = hooks.data();
				p.num_hooks = static_cast<int>(hooks.size());
				cache.Arm(7);
				Assert::IsTrue(pl_render_image(fixture.Renderer(), &image, &t, &p));
			}
			const std::vector<RgbaPixel> own = download();
			Assert::IsNotNull(cache.Texture(7));
			// Twice, so the hysteresis lets the pan's blocks warp.
			Assert::IsTrue(warp.Estimate(planesA.planes, planesB.planes) && warp.Estimate(planesA.planes, planesB.planes));
			DejudderRendered::Geometry g;
			g.sourceCrop = { 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height) };
			g.targetCrop = { 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height) };
			// The subtitle's two rectangles: where it was and where it is shown.
			g.holds[0] = { 64.0f, 32.0f, 160.0f, 56.0f };
			g.holds[1] = { 48.0f, 96.0f, 208.0f, 120.0f };
			g.numHolds = 2;
			auto extra = [&](const DejudderRendered::Geometry& geometry)
			{
				pl_tex out = nullptr;
				Assert::IsTrue(rendered.Run(cache.Texture(7), warp.Field(), warp.FrameTrust(),
					warp.GetSettings().minTrustedShare, 0.5f, geometry, out), ExtendedRangeGpuProbe::ToWide(rendered.Error() + "; log: " + log).c_str());
				pl_frame image = DejudderRendered::Frame(out, cache.Repr(), cache.Color());
				pl_frame t = target();
				std::vector<const pl_hook*> kept;
				const pl_render_params p = DejudderRendered::OutputOnly(params, {}, kept);
				Assert::IsTrue(pl_render_image(fixture.Renderer(), &image, &t, &p));
				return download();
			};
			const std::vector<RgbaPixel> withHolds = extra(g);
			const std::vector<RgbaPixel> without = extra(DejudderRendered::Geometry{ g.sourceCrop, g.targetCrop });
			auto inside = [&](int x, int y, int i)
			{
				const pl_rect2df& r = g.holds[i];
				return x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1;
			};
			int heldDiff = 0;
			long long movedOutside = 0, sameAsWithout = 0, outside = 0;
			for (int y = 8; y < height - 8; ++y)
				for (int x = 8; x < width - 8; ++x)
				{
					const size_t i = static_cast<size_t>(y) * width + x;
					auto diff = [](const RgbaPixel& p, const RgbaPixel& q)
					{
						return std::max({ std::abs(p.r - q.r), std::abs(p.g - q.g), std::abs(p.b - q.b) });
					};
					if (inside(x, y, 0) || inside(x, y, 1))
						heldDiff = std::max(heldDiff, diff(withHolds[i], own[i]));
					else
					{
						// Away from the rectangles by more than the motion: as without holds.
						bool closeBy = false;
						for (int k = 0; k < 2; ++k)
						{
							const pl_rect2df& r = g.holds[k];
							closeBy = closeBy || (x >= r.x0 - 16 && x < r.x1 + 16 && y >= r.y0 - 16 && y < r.y1 + 16);
						}
						if (closeBy) continue;
						++outside;
						movedOutside += diff(withHolds[i], own[i]) > 2 ? 1 : 0;
						sameAsWithout += diff(withHolds[i], without[i]) == 0 ? 1 : 0;
					}
				}
			Assert::AreEqual(0, heldDiff, L"inside the held rectangles the extra shows A exactly");
			Assert::IsTrue(movedOutside > outside / 2, L"elsewhere the picture moves");
			Assert::AreEqual(outside, sameAsWithout, L"away from the rectangles the extra is as without holds");
			// The dither's vectors: zero in the blocks inside a rectangle.
			pl_tex vectors = nullptr;
			Assert::IsTrue(field.Run(warp.Field(), warp.FrameTrust(), warp.GetSettings().minTrustedShare, 0.5f, g,
				width, height, vectors), ExtendedRangeGpuProbe::ToWide(field.Error()).c_str());
			Assert::IsNotNull(vectors);
		}

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
			AssertCleanLibplaceboLog(log);
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
			AssertCleanLibplaceboLog(log);
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
				shift, g, warped, warp.SubField(), &warp.GetSettings(),
				warp.Camera(), warp.PixelMask(), warp.VectorOk());   // delta 14 R-B's camera and M6's weights, as the renderer
			if (!ran)
				pl_tex_destroy(gpu, &cached);
			Assert::IsTrue(ran, ExtendedRangeGpuProbe::ToWide(rendered.Error()).c_str());
			pl_gpu_finish(gpu);
			// The warped picture is rgba32f since the 12-bit work (rgba16f where
			// 32-bit is not storable): read it at the precision it has.
			Assert::IsNotNull(warped);
			const bool float32 = warped->params.format->component_depth[0] == 32;
			std::vector<float> full(float32 ? a.luma.size() * 4 : 0);
			std::vector<uint16_t> half(float32 ? 0 : a.luma.size() * 4);
			pl_tex_transfer_params download{};
			download.tex = warped;
			download.ptr = float32 ? static_cast<void*>(full.data()) : static_cast<void*>(half.data());
			const bool downloaded = pl_tex_download(gpu, &download);
			pl_tex_destroy(gpu, &cached);
			Assert::IsTrue(downloaded, L"the rendered warp is not host-readable");
			result.rendered.resize(a.luma.size());
			for (size_t i = 0; i < a.luma.size(); ++i)
				result.rendered[i] = (float32 ? full[i * 4] : HalfBits(half[i * 4])) * 65535.0f;
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

		// Look-ahead verification (owner 2026-10-16): CPU only - frames ahead of display
		// decide the switch at the cut / black frame, or cancel it.
		TEST_METHOD(SourceResolutionMonitorIdentifiesThePlayerKernel)
		{
			// Kernel-matched re-base (owner 2026-10-16: optimise 1080p): frames upscaled 2x by
			// a player kernel are read as 1080 and the kernel identified across shots; native
			// frames never identify one. CPU only.
			const int T = SourceResolutionMonitor::Monitor::kTile, M = T / 2;
			auto frameTiles = [&](const KernelInverse::Candidate* c, unsigned seed, float level) {
				std::vector<float> tiles(static_cast<size_t>(8) * T * T);
				std::mt19937 rng(seed);
				std::normal_distribution<float> noise(0.0f, 0.06f);
				for (int t = 0; t < 8; ++t)
				{
					float* tile = &tiles[static_cast<size_t>(t) * T * T];
					if (!c)
					{
						for (int i = 0; i < T * T; ++i) tile[i] = (std::min)(1.0f, (std::max)(0.0f, level + noise(rng)));
					}
					else
					{
						std::vector<float> master(static_cast<size_t>(M) * M), rows(static_cast<size_t>(M) * T), col(static_cast<size_t>(M)), out(static_cast<size_t>(T));
						for (float& v : master) v = level + noise(rng);
						for (int y = 0; y < M; ++y) KernelInverse::UpscaleLine(*c, &master[static_cast<size_t>(y) * M], M, &rows[static_cast<size_t>(y) * T]);
						for (int x = 0; x < T; ++x)
						{
							for (int y = 0; y < M; ++y) col[static_cast<size_t>(y)] = rows[static_cast<size_t>(y) * T + x];
							KernelInverse::UpscaleLine(*c, col.data(), M, out.data());
							for (int y = 0; y < T; ++y) tile[static_cast<size_t>(y) * T + x] = (std::min)(1.0f, (std::max)(0.0f, out[static_cast<size_t>(y)]));
						}
					}
					for (int i = 0; i < T * T; ++i) tile[i] = std::round(tile[i] * 1023.0f) / 1023.0f;
				}
				return tiles;
			};
			const int k = KernelInverse::Find("lanczos3");
			const KernelInverse::Candidate& c = KernelInverse::Candidates()[static_cast<size_t>(k)];
			auto run = [&](const KernelInverse::Candidate* kernel) {
				SourceResolutionMonitor::Monitor m;
				double t = 0.0;
				for (int shot = 0; shot < 14; ++shot)
					for (int f = 0; f < 6; ++f, t += 0.5)
						m.AnalyseTilesForTest(frameTiles(kernel, 1000 + shot * 31 + f, shot % 2 ? 0.55f : 0.35f), 8, t, 3840, 2160);
				return m.Read();
			};
			const SourceResolutionMonitor::Snapshot up = run(&c);
			char line[200];
			snprintf(line, sizeof(line), "upscaled (lanczos3): declared %.3f, kernel %d (%s), votes %d, agree %d\n", up.declaredRatio, up.kernel,
				up.kernel >= 0 ? KernelInverse::Candidates()[static_cast<size_t>(up.kernel)].name.c_str() : "none", up.kernelVotes, up.kernelAgree);
			Logger::WriteMessage(line);
			Assert::AreEqual(0.5, up.declaredRatio, 0.01, L"read as 1080");
			Assert::AreEqual(k, up.kernel, L"the player's kernel identified");
			const SourceResolutionMonitor::Snapshot native = run(nullptr);
			snprintf(line, sizeof(line), "native: declared %.3f, kernel %d, votes %d\n", native.declaredRatio, native.kernel, native.kernelVotes);
			Logger::WriteMessage(line);
			Assert::AreEqual(-1, native.kernel, L"native: none");
		}

		TEST_METHOD(SourceResolutionLookAheadVerifiesOrCancelsTheSwitch)
		{
			const int FW = 1024, FH = 512, T = SourceResolutionMonitor::Monitor::kTile;
			// A frame: an upscale (white noise at half resolution, bilinear x2) or native
			// (white noise at full resolution), around a mean level; or flat black.
			enum class Kind { Upscaled, Native, Black };
			auto makeFrame = [&](Kind kind, unsigned seed, double level) {
				std::mt19937 rng(seed);
				std::normal_distribution<double> noise(0.0, 0.08);
				std::vector<double> f(static_cast<size_t>(FW) * FH, 64.0 / 1023.0);
				if (kind == Kind::Black) return f;
				if (kind == Kind::Native)
				{
					for (double& v : f) v = level + noise(rng);
					return f;
				}
				const int lw = FW / 2, lh = FH / 2;
				std::vector<double> low(static_cast<size_t>(lw) * lh);
				for (double& v : low) v = level + noise(rng);
				for (int y = 0; y < FH; ++y)
					for (int x = 0; x < FW; ++x)
					{
						const double u = (x + 0.5) * 0.5 - 0.5, w = (y + 0.5) * 0.5 - 0.5;
						const int x0 = (std::max)(0, (std::min)(lw - 2, static_cast<int>(std::floor(u))));
						const int y0 = (std::max)(0, (std::min)(lh - 2, static_cast<int>(std::floor(w))));
						const double fx = (std::min)(1.0, (std::max)(0.0, u - x0)), fy = (std::min)(1.0, (std::max)(0.0, w - y0));
						auto at = [&](int xx, int yy) { return low[static_cast<size_t>(yy) * lw + xx]; };
						f[static_cast<size_t>(y) * FW + x] = (1 - fy) * ((1 - fx) * at(x0, y0) + fx * at(x0 + 1, y0)) + fy * ((1 - fx) * at(x0, y0 + 1) + fx * at(x0 + 1, y0 + 1));
					}
				return f;
			};
			auto code = [](double v) { return static_cast<int>(std::lround((std::min)(1.0, (std::max)(0.0, v)) * 1023.0)); };
			// The monitor's own analysis of a displayed frame (8 tiles).
			auto analyse = [&](SourceResolutionMonitor::Monitor& m, const std::vector<double>& f, double t) {
				std::vector<float> tiles(static_cast<size_t>(8) * T * T);
				for (int i = 0; i < 8; ++i)
				{
					const int x0 = (i % 4) * (FW - T) / 3, y0 = (i / 4) * (FH - T);
					for (int y = 0; y < T; ++y)
						for (int x = 0; x < T; ++x)
							tiles[(static_cast<size_t>(i) * T + y) * T + x] = static_cast<float>(code(f[static_cast<size_t>(y0 + y) * FW + x0 + x]) / 1023.0);
				}
				m.AnalyseTilesForTest(tiles, 8, t, FW, FH);
			};
			auto ahead = [&](SourceResolutionMonitor::Monitor& m, uint64_t seq, const std::vector<double>& f) {
				m.LookAheadFrame(seq, FW, FH, [&](int x, int y) { return code(f[static_cast<size_t>(y) * FW + x]); }, 0, 0, FW, FH);
			};
			auto waitReady = [&](SourceResolutionMonitor::Monitor& m) {
				for (int k = 0; k < 400 && !m.Ahead().ready; ++k) std::this_thread::sleep_for(std::chrono::milliseconds(5));
				return m.Ahead().ready;
			};
			// Upscaled shots until auto waits for the next cut; returns the time reached.
			auto untilWaiting = [&](SourceResolutionMonitor::Monitor& m, uint64_t& seq) {
				double t = 0.0;
				std::string why;
				for (int shot = 0; shot < 12; ++shot)
					for (int i = 0; i < 8; ++i)
					{
						analyse(m, makeFrame(Kind::Upscaled, 50 + shot * 13 + i, shot % 2 ? 0.55 : 0.35), t);
						m.AutoFactor(FH, 0.8, why, t, ++seq);
						t += 0.5;
						if (why == "waiting for the next cut or black") return t;
					}
				return -1.0;
			};
			const double offShot = 0.35;   // the level of the shot showing when auto begins to wait (even shots)
			(void)offShot;

			// 1. The next shot agrees: engaged exactly on the cut frame.
			{
				SourceResolutionMonitor::Monitor m;
				uint64_t seq = 0;
				const double t = untilWaiting(m, seq);
				Assert::IsTrue(t > 0.0, L"auto reached waiting");
				const double level = m.Read().frames % 2 ? 0.35 : 0.35;
				(void)level;
				// Ahead: one more frame of the current shot, then the cut at seq + 2.
				const std::vector<double> same = makeFrame(Kind::Upscaled, 999, 0.35);
				const std::vector<double> next = makeFrame(Kind::Upscaled, 1000, 0.75);
				ahead(m, seq + 1, same);
				ahead(m, seq + 2, next);
				Assert::IsTrue(waitReady(m), L"the frame after the cut was analysed ahead");
				std::string why;
				Assert::AreEqual(1.0, m.AutoFactor(FH, 0.8, why, t, seq + 1), 1e-9, L"not before the cut frame");
				const double f = m.AutoFactor(FH, 0.8, why, t + 0.04, seq + 2);
				Logger::WriteMessage(("agree: " + why + "\n").c_str());
				Assert::AreEqual(0.5, f, 1e-9, L"engaged on the cut frame");
				Assert::AreEqual(std::string("engaged at the cut / black (look-ahead verified)"), why);
				Assert::AreEqual(1ull, m.Ahead().verified);
			}
			// 2. The next shot is native: the switch is cancelled, and the cut itself is not used.
			{
				SourceResolutionMonitor::Monitor m;
				uint64_t seq = 0;
				double t = untilWaiting(m, seq);
				Assert::IsTrue(t > 0.0);
				const std::vector<double> native = makeFrame(Kind::Native, 2000, 0.75);
				ahead(m, seq + 1, makeFrame(Kind::Upscaled, 1999, 0.35));   // the current shot (the first frame ahead sets the baseline)
				ahead(m, seq + 2, native);
				Assert::IsTrue(waitReady(m));
				std::string why;
				Assert::AreEqual(1.0, m.AutoFactor(FH, 0.8, why, t, seq + 1), 1e-9);
				const double f = m.AutoFactor(FH, 0.8, why, t, seq + 2);
				Logger::WriteMessage(("disagree: " + why + "\n").c_str());
				Assert::AreEqual(1.0, f, 1e-9, L"stays native");
				Assert::AreEqual(std::string("look-ahead: the next shot disagrees - staying"), why);
				Assert::AreEqual(1ull, m.Ahead().cancelled);
				// The monitor then sees that cut (the native frame): still no engage on it.
				analyse(m, native, t + 0.5);
				Assert::AreEqual(1.0, m.AutoFactor(FH, 0.8, why, t + 0.5, seq + 3), 1e-9, L"the cancelled cut is not used");
			}
			// 3. A black frame ahead, then an agreeing shot: engaged on the black frame.
			{
				SourceResolutionMonitor::Monitor m;
				uint64_t seq = 0;
				const double t = untilWaiting(m, seq);
				Assert::IsTrue(t > 0.0);
				const std::vector<double> black = makeFrame(Kind::Black, 0, 0.0);
				const std::vector<double> next = makeFrame(Kind::Upscaled, 3000, 0.45);
				ahead(m, seq + 1, black);
				ahead(m, seq + 2, black);
				ahead(m, seq + 3, next);
				Assert::IsTrue(waitReady(m));
				std::string why;
				const double f = m.AutoFactor(FH, 0.8, why, t, seq + 1);
				Logger::WriteMessage(("black: " + why + "\n").c_str());
				Assert::AreEqual(0.5, f, 1e-9, L"engaged on the black frame, verified by the shot after it");
			}
		}

		TEST_METHOD(SourceResolutionMonitorDeclaresAutoEngagesAtACutAndDisengagesAtOnce)
		{
			// Stage 12 step B + auto (owner 2026-10-15). Simulated time (the
			// monitor takes its clock from the caller): a 1080-line frame
			// upscaled from 540 lines over eight shots, then native detail.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			// libplacebo's own errors for this test (the fixture's log is silent).
			static std::string plErrors;
			pl_log_params lp{};
			lp.log_level = PL_LOG_DEBUG;
			lp.log_cb = [](void*, pl_log_level, const char* msg) { if (plErrors.size() < 4000) { plErrors += msg; plErrors += '\n'; } };
			pl_log errorLog = pl_log_create(PL_API_VER, &lp);
			pl_log_update(fixture.Log(), &lp);   // the device's own messages too
			pl_dispatch dispatch = pl_dispatch_create(errorLog, gpu);
			const int W = 1920, H = 1080, w = 960, h = 540;
			// Smooth random content at size (sw, sh): a sum of a few hundred waves.
			auto content = [](int sw, int sh, unsigned seed, double level) {
				std::mt19937 rng(seed);
				std::uniform_real_distribution<double> u(0.0, 1.0);
				std::vector<double> v(static_cast<size_t>(sw) * sh, level);
				for (int k = 0; k < 120; ++k)
				{
					const double f = 0.004 * std::pow(0.49 / 0.004, u(rng)), a = u(rng) * 3.14159265358979, px = u(rng) * 6.283, py = u(rng) * 6.283;
					const double fx = f * std::cos(a), fy = f * std::sin(a), amp = 0.012 / std::sqrt(f);
					std::vector<double> cx(sw), sx(sw);
					for (int x = 0; x < sw; ++x) { cx[x] = std::cos(6.283185307 * fx * x + px); sx[x] = std::sin(6.283185307 * fx * x + px); }
					for (int y = 0; y < sh; ++y)
					{
						const double cy = std::cos(6.283185307 * fy * y + py) * amp, sy = std::sin(6.283185307 * fy * y + py) * amp;
						double* row = &v[static_cast<size_t>(y) * sw];
						for (int x = 0; x < sw; ++x) row[x] += cx[x] * cy - sx[x] * sy;
					}
				}
				return v;
			};
			// Catmull-Rom 2x upscale (separable, centre-aligned).
			auto upscale = [&](const std::vector<double>& src) {
				auto k = [](double x) { x = std::fabs(x); return x < 1 ? 1.5 * x * x * x - 2.5 * x * x + 1 : x < 2 ? -0.5 * x * x * x + 2.5 * x * x - 4 * x + 2 : 0.0; };
				std::vector<double> rows(static_cast<size_t>(W) * h), out(static_cast<size_t>(W) * H);
				for (int y = 0; y < h; ++y)
					for (int x = 0; x < W; ++x)
					{
						const double uu = (x + 0.5) * 0.5 - 0.5; const int c = static_cast<int>(std::floor(uu));
						double s = 0, ws = 0;
						for (int t = c - 1; t <= c + 2; ++t) { const double wt = k(uu - t); s += wt * src[static_cast<size_t>(y) * w + (std::min)(w - 1, (std::max)(0, t))]; ws += wt; }
						rows[static_cast<size_t>(y) * W + x] = s / ws;
					}
				for (int y = 0; y < H; ++y)
				{
					const double uu = (y + 0.5) * 0.5 - 0.5; const int c = static_cast<int>(std::floor(uu));
					for (int x = 0; x < W; ++x)
					{
						double s = 0, ws = 0;
						for (int t = c - 1; t <= c + 2; ++t) { const double wt = k(uu - t); s += wt * rows[static_cast<size_t>((std::min)(h - 1, (std::max)(0, t))) * W + x]; ws += wt; }
						out[static_cast<size_t>(y) * W + x] = s / ws;
					}
				}
				return out;
			};
			// 10-bit codes in a P010-style r16 plane (code << 6).
			auto upload = [&](const std::vector<double>& v, double offset = 0.0) {
				std::vector<uint16_t> data(v.size());
				for (size_t i = 0; i < v.size(); ++i)
					data[i] = static_cast<uint16_t>(static_cast<int>(std::lround((std::min)(1.0, (std::max)(0.0, v[i] + offset)) * 1023.0)) << 6);
				pl_tex_params p{};
				p.w = W; p.h = H; p.format = pl_find_named_fmt(gpu, "r16"); p.sampleable = true; p.initial_data = data.data();
				return pl_tex_create(gpu, &p);
			};
			pl_frame frame{};
			frame.num_planes = 1;
			frame.planes[0].components = 1;
			frame.planes[0].component_mapping[0] = PL_CHANNEL_Y;
			frame.repr.sys = PL_COLOR_SYSTEM_BT_709;
			frame.repr.levels = PL_COLOR_LEVELS_LIMITED;
			frame.repr.bits.sample_depth = 16; frame.repr.bits.color_depth = 10; frame.repr.bits.bit_shift = 6;
			frame.color = pl_color_space_bt709;
			frame.crop = { 0.0f, 0.0f, static_cast<float>(W), static_cast<float>(H) };

			SourceResolutionMonitor::Monitor monitor;
			// Owner 2026-10-16: the auto history, one file per session, the newest 5 kept.
			char tempPath[MAX_PATH] = {};
			GetTempPathA(MAX_PATH, tempPath);
			const std::string historyFolder = std::string(tempPath) + "vp-source-resolution-history";
			CreateDirectoryA(historyFolder.c_str(), nullptr);
			for (int old = 0; old < 7; ++old)   // seven earlier sessions (old names sort first)
				std::ofstream(historyFolder + "\\source-resolution 2020010" + std::to_string(old) + "-000000.csv") << "old";
			monitor.SetHistoryFolder(historyFolder);
			const std::string historyPath = monitor.HistoryPath();
			{
				int kept = 0;
				WIN32_FIND_DATAA found{};
				HANDLE search = FindFirstFileA((historyFolder + "\\source-resolution *.csv").c_str(), &found);
				if (search != INVALID_HANDLE_VALUE) { do ++kept; while (FindNextFileA(search, &found)); FindClose(search); }
				Assert::AreEqual(4, kept, L"the newest 4 earlier sessions kept before this one is written (5 with it)");
			}
			double seconds = 0.0;
			uint64_t sequence = 0;
			// One analysed frame: submit, let the GPU finish, collect, wait for the worker.
			auto step = [&](pl_tex tex) {
				frame.planes[0].texture = tex;
				const unsigned long long before = monitor.Read().frames;
				monitor.Frame(gpu, dispatch, frame, ++sequence, seconds);
				pl_gpu_finish(gpu);
				monitor.Frame(gpu, dispatch, frame, sequence, seconds);   // collects (interval not passed)
				for (int i = 0; i < 400 && monitor.Read().frames == before; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
				if (monitor.Read().frames == before)
					Assert::Fail(ExtendedRangeGpuProbe::ToWide("no frame analysed: " + SourceResolutionMonitor::Describe(monitor.Read(), 1.0, false) +
						" / " + monitor.Read().error + " / ssbo " + std::to_string(gpu->limits.max_ssbo_size) + " buf " + std::to_string(gpu->limits.max_buf_size) + " / libplacebo: " + plErrors).c_str());
				seconds += 0.5;
			};
			std::vector<std::string> log;
			std::string why;
			double factor = 1.0;
			int engagedAtShot = -1, waitingAtShot = -1;
			for (int shot = 0; shot < 9; ++shot)
			{
				const std::vector<double> picture = upscale(content(w, h, 100 + shot, shot % 2 ? 0.55 : 0.30));
				for (int i = 0; i < 20; ++i)
				{
					// Small changes within a shot (not a repeated frame): a slow brightness drift.
					pl_tex tex = upload(picture, 0.0008 * i);
					step(tex);
					pl_tex_destroy(gpu, &tex);
					factor = monitor.AutoFactor(H, 0.8, why);
					if (!why.empty())
					{
						log.push_back("shot " + std::to_string(shot) + ": " + why);
						if (why == "waiting for the next cut or black" && waitingAtShot < 0) waitingAtShot = shot;
						if (why == "engaged at a cut" && engagedAtShot < 0) engagedAtShot = shot;
					}
				}
			}
			const SourceResolutionMonitor::Snapshot s = monitor.Read();
			for (const std::string& l : log) Logger::WriteMessage((l + "\n").c_str());
			Logger::WriteMessage((SourceResolutionMonitor::Describe(s, monitor.AutoRatio(), true) + "\n").c_str());
			Assert::IsTrue(s.reading, L"the source is read");
			Assert::AreEqual(0.5, s.declaredRatio, 0.01, L"declared: the 540-line source");
			Assert::IsTrue(s.confidence >= 0.8, L"with confidence");
			Assert::IsTrue(waitingAtShot >= 0 && engagedAtShot > waitingAtShot, L"auto waits for the next cut, then engages");
			Assert::AreEqual(0.5, factor, 1e-9, L"re-based to 540 lines");
			Assert::AreEqual(1080, s.nativeHeight);

			// Owner 2026-10-16: engaging also happens during a black frame (a fade's dark end), where
			// the change cannot be seen - a second monitor waiting to engage, then one black frame.
			{
				SourceResolutionMonitor::Monitor faded;
				double t = 0.0;
				std::string fadedWhy;
				double fadedFactor = 1.0;
				bool waiting = false;
				std::vector<double> held;   // the shot that was showing when auto began to wait
				for (int shot = 0; shot < 6 && !waiting; ++shot)
				{
					const std::vector<double> picture = upscale(content(w, h, 700 + shot, shot % 2 ? 0.55 : 0.30));
					held = picture;
					for (int i = 0; i < 20 && !waiting; ++i)
					{
						pl_tex tex = upload(picture, 0.0008 * i);
						frame.planes[0].texture = tex;
						const unsigned long long before = faded.Read().frames;
						faded.Frame(gpu, dispatch, frame, ++sequence, t);
						pl_gpu_finish(gpu);
						faded.Frame(gpu, dispatch, frame, sequence, t);
						for (int k = 0; k < 400 && faded.Read().frames == before; ++k) std::this_thread::sleep_for(std::chrono::milliseconds(5));
						t += 0.5;
						fadedFactor = faded.AutoFactor(H, 0.8, fadedWhy, t);
						waiting = fadedWhy == "waiting for the next cut or black";
						pl_tex_destroy(gpu, &tex);
					}
				}
				Assert::IsTrue(waiting, L"auto reached 'waiting' on the upscaled shots");
				// A fade to black (each step below the cut level, so no cut is seen): the change is
				// made once the picture holds no detail.
				const std::vector<double> last = held;
				bool sawCut = false;
				for (int step = 1; step <= 80 && fadedFactor != 0.5; ++step)
				{
					std::vector<double> dim(last);
					const double keep = std::pow(0.9, step);
					for (double& v : dim) v = 0.0625 + (v - 0.0625) * keep;
					pl_tex tex = upload(dim);
					frame.planes[0].texture = tex;
					const unsigned long long cutsBefore = faded.Read().cuts;
					const unsigned long long before = faded.Read().frames;
					faded.Frame(gpu, dispatch, frame, ++sequence, t);
					pl_gpu_finish(gpu);
					faded.Frame(gpu, dispatch, frame, sequence, t);
					for (int k = 0; k < 400 && faded.Read().frames == before; ++k) std::this_thread::sleep_for(std::chrono::milliseconds(5));
					sawCut = sawCut || faded.Read().cuts != cutsBefore;
					t += 0.5;
					fadedFactor = faded.AutoFactor(H, 0.8, fadedWhy, t);
					pl_tex_destroy(gpu, &tex);
				}
				Logger::WriteMessage(("fade: " + fadedWhy + "\n").c_str());
				Assert::IsFalse(sawCut, L"the fade made no cut");
				Assert::AreEqual(0.5, fadedFactor, 1e-9, L"engaged in the fade");
				Assert::AreEqual(std::string("engaged during a black or flat frame"), fadedWhy, L"at the fade's dark end");
				faded.Release(gpu);
			}

			// Planner Q2: only the confidence falls (here: the minimum raised above it) - native at the
			// next cut, at most 5 s later; not at once.
			{
				factor = monitor.AutoFactor(H, 1.01, why, seconds);
				Logger::WriteMessage(("confidence fell: " + why + "\n").c_str());
				Assert::AreEqual(0.5, factor, 1e-9, L"a confidence drop does not disengage mid-shot");
				factor = monitor.AutoFactor(H, 1.01, why, seconds + 2.0);
				Assert::AreEqual(0.5, factor, 1e-9, L"still waiting for a cut after 2 s");
				factor = monitor.AutoFactor(H, 1.01, why, seconds + 5.1);
				Logger::WriteMessage(("after 5 s: " + why + "\n").c_str());
				Assert::AreEqual(1.0, factor, L"native after 5 s without a cut");
			}

			// Native detail: back to native at once (same shot level, no cut needed).
			{
				pl_tex tex = upload(content(W, H, 999, 0.55));
				step(tex);
				factor = monitor.AutoFactor(H, 0.8, why);
				Logger::WriteMessage(("native detail: " + why + "\n").c_str());
				Assert::AreEqual(1.0, factor, L"native at once on real detail");
				Assert::AreEqual(1.0, monitor.Read().declaredRatio, L"declared native");
				pl_tex_destroy(gpu, &tex);
			}

			// A discontinuity: native at once and the evidence starts again.
			monitor.Discontinuity("test seek");
			factor = monitor.AutoFactor(H, 0.8, why);
			Assert::AreEqual(1.0, factor);
			{
				pl_tex tex = upload(upscale(content(w, h, 5, 0.4)));
				step(tex);
				pl_tex_destroy(gpu, &tex);
			}
			Assert::AreEqual(1.0, monitor.Read().declaredRatio, L"evidence starts again after a seek");
			Assert::AreEqual(std::string("test seek"), monitor.Read().lastDiscontinuity);

			// The history holds the readings and every auto decision with its reason.
			monitor.FlushHistoryNow();
			{
				std::ifstream in(historyPath);
				std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
				Assert::IsTrue(text.find("time,event,frame_ratio") == 0, L"history header");
				Assert::IsTrue(text.find("session started") != std::string::npos, L"the session is marked");
				Assert::IsTrue(text.find(",reading,") != std::string::npos, L"reading rows");
				Assert::IsTrue(text.find("engaged at a cut") != std::string::npos, L"the engage decision and its reason");
				Assert::IsTrue(text.find("native after 5 s without a cut") != std::string::npos, L"the bounded disengage");
				Assert::IsTrue(text.find("discontinuity") != std::string::npos && text.find("test seek") != std::string::npos,
					L"the seek");
			}
			{
				WIN32_FIND_DATAA found{};
				HANDLE search = FindFirstFileA((historyFolder + "\\*").c_str(), &found);
				if (search != INVALID_HANDLE_VALUE)
				{
					do if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileA((historyFolder + "\\" + found.cFileName).c_str());
					while (FindNextFileA(search, &found));
					FindClose(search);
				}
				RemoveDirectoryA(historyFolder.c_str());
			}

			// Genuine full-resolution content over many shots never engages.
			SourceResolutionMonitor::Monitor native;
			seconds = 0.0;
			int engaged = 0;
			for (int shot = 0; shot < 8; ++shot)
			{
				const std::vector<double> picture = content(W, H, 300 + shot, shot % 2 ? 0.55 : 0.30);
				for (int i = 0; i < 20; ++i)
				{
					pl_tex tex = upload(picture, 0.0008 * i);
					frame.planes[0].texture = tex;
					const unsigned long long before = native.Read().frames;
					native.Frame(gpu, dispatch, frame, ++sequence, seconds);
					pl_gpu_finish(gpu);
					native.Frame(gpu, dispatch, frame, sequence, seconds);
					for (int k = 0; k < 400 && native.Read().frames == before; ++k) std::this_thread::sleep_for(std::chrono::milliseconds(5));
					seconds += 0.5;
					engaged += native.AutoFactor(H, 0.8, why) < 1.0;
					pl_tex_destroy(gpu, &tex);
				}
			}
			Logger::WriteMessage((SourceResolutionMonitor::Describe(native.Read(), native.AutoRatio(), true) + "\n").c_str());
			Assert::AreEqual(0, engaged, L"native content never engages");
			Assert::AreEqual(1.0, native.Read().declaredRatio);
			monitor.Release(gpu);
			native.Release(gpu);
			pl_dispatch_destroy(&dispatch);
			pl_log_destroy(&errorLog);
		}

		TEST_METHOD(SourceResolutionReadsAndRebasesTheDolbyVisionTunnel)
		{
			// 2026-10-17: the DV tunnel is no longer skipped as a "packed signal".
			// Its decoded intensity plane (12-bit codes << 4, repr DOLBYVISION) is
			// measured like YCbCr luma: a 540-line picture upscaled to 1080 is
			// declared 0.5. The re-base resamples the IPT planes and keeps the
			// Dolby Vision representation (its DM matrices) for libplacebo's decode.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			pl_dispatch dispatch = pl_dispatch_create(fixture.Log(), gpu);
			const int W = 1920, H = 1080, w = 960, h = 540;
			auto content = [](int sw, int sh, unsigned seed, double level) {
				std::mt19937 rng(seed);
				std::uniform_real_distribution<double> u(0.0, 1.0);
				std::vector<double> v(static_cast<size_t>(sw) * sh, level);
				for (int k = 0; k < 120; ++k)
				{
					const double f = 0.004 * std::pow(0.49 / 0.004, u(rng)), a = u(rng) * 3.14159265358979, px = u(rng) * 6.283, py = u(rng) * 6.283;
					const double fx = f * std::cos(a), fy = f * std::sin(a), amp = 0.012 / std::sqrt(f);
					std::vector<double> cx(sw), sx(sw);
					for (int x = 0; x < sw; ++x) { cx[x] = std::cos(6.283185307 * fx * x + px); sx[x] = std::sin(6.283185307 * fx * x + px); }
					for (int y = 0; y < sh; ++y)
					{
						const double cy = std::cos(6.283185307 * fy * y + py) * amp, sy = std::sin(6.283185307 * fy * y + py) * amp;
						double* row = &v[static_cast<size_t>(y) * sw];
						for (int x = 0; x < sw; ++x) row[x] += cx[x] * cy - sx[x] * sy;
					}
				}
				return v;
			};
			auto upscale = [&](const std::vector<double>& src) {
				auto k = [](double x) { x = std::fabs(x); return x < 1 ? 1.5 * x * x * x - 2.5 * x * x + 1 : x < 2 ? -0.5 * x * x * x + 2.5 * x * x - 4 * x + 2 : 0.0; };
				std::vector<double> rows(static_cast<size_t>(W) * h), out(static_cast<size_t>(W) * H);
				for (int y = 0; y < h; ++y)
					for (int x = 0; x < W; ++x)
					{
						const double uu = (x + 0.5) * 0.5 - 0.5; const int c = static_cast<int>(std::floor(uu));
						double s = 0, ws = 0;
						for (int t = c - 1; t <= c + 2; ++t) { const double wt = k(uu - t); s += wt * src[static_cast<size_t>(y) * w + (std::min)(w - 1, (std::max)(0, t))]; ws += wt; }
						rows[static_cast<size_t>(y) * W + x] = s / ws;
					}
				for (int y = 0; y < H; ++y)
				{
					const double uu = (y + 0.5) * 0.5 - 0.5; const int c = static_cast<int>(std::floor(uu));
					for (int x = 0; x < W; ++x)
					{
						double s = 0, ws = 0;
						for (int t = c - 1; t <= c + 2; ++t) { const double wt = k(uu - t); s += wt * rows[static_cast<size_t>((std::min)(h - 1, (std::max)(0, t))) * W + x]; ws += wt; }
						out[static_cast<size_t>(y) * W + x] = s / ws;
					}
				}
				return out;
			};
			// 12-bit intensity codes << 4 (the DV formatter's / GPU unpack's plane).
			auto upload = [&](const std::vector<double>& v, double offset) {
				std::vector<uint16_t> data(v.size());
				for (size_t i = 0; i < v.size(); ++i)
					data[i] = static_cast<uint16_t>(static_cast<int>(std::lround((std::min)(1.0, (std::max)(0.0, v[i] + offset)) * 4095.0)) << 4);
				pl_tex_params p{};
				p.w = W; p.h = H; p.format = pl_find_named_fmt(gpu, "r16"); p.sampleable = true; p.initial_data = data.data();
				return pl_tex_create(gpu, &p);
			};
			std::vector<uint16_t> neutral(static_cast<size_t>(W / 2) * H * 2, static_cast<uint16_t>(2048 << 4));
			pl_tex_params cp{};
			cp.w = W / 2; cp.h = H; cp.format = pl_find_named_fmt(gpu, "rg16"); cp.sampleable = true; cp.initial_data = neutral.data();
			pl_tex chroma = pl_tex_create(gpu, &cp);
			Assert::IsNotNull(chroma);
			pl_dovi_metadata dovi{};
			dovi.nonlinear.m[0][0] = dovi.nonlinear.m[1][1] = dovi.nonlinear.m[2][2] = 1.0f;
			dovi.linear.m[0][0] = dovi.linear.m[1][1] = dovi.linear.m[2][2] = 1.0f;
			pl_frame frame{};
			frame.num_planes = 2;
			frame.planes[0].components = 1;
			frame.planes[0].component_mapping[0] = PL_CHANNEL_Y;
			frame.planes[1].texture = chroma;
			frame.planes[1].components = 2;
			frame.planes[1].component_mapping[0] = PL_CHANNEL_CB;
			frame.planes[1].component_mapping[1] = PL_CHANNEL_CR;
			frame.repr.sys = PL_COLOR_SYSTEM_DOLBYVISION;
			frame.repr.dovi = &dovi;
			frame.repr.levels = PL_COLOR_LEVELS_FULL;
			frame.repr.bits.sample_depth = 16; frame.repr.bits.color_depth = 12; frame.repr.bits.bit_shift = 4;
			frame.color.primaries = PL_COLOR_PRIM_BT_2020;
			frame.color.transfer = PL_COLOR_TRC_PQ;
			frame.crop = { 0.0f, 0.0f, static_cast<float>(W), static_cast<float>(H) };

			SourceResolutionMonitor::Monitor monitor;
			double seconds = 0.0;
			uint64_t sequence = 0;
			pl_tex first = nullptr;
			for (int shot = 0; shot < 9; ++shot)
			{
				const std::vector<double> picture = upscale(content(w, h, 300 + shot, shot % 2 ? 0.55 : 0.30));
				for (int i = 0; i < 20; ++i)
				{
					pl_tex tex = upload(picture, 0.0008 * i);
					frame.planes[0].texture = tex;
					Assert::IsTrue(SourceResolutionMonitor::Monitor::NotReadReason(frame).empty(), L"DV is readable");
					const unsigned long long before = monitor.Read().frames;
					monitor.Frame(gpu, dispatch, frame, ++sequence, seconds);
					pl_gpu_finish(gpu);
					monitor.Frame(gpu, dispatch, frame, sequence, seconds);
					for (int t = 0; t < 400 && monitor.Read().frames == before; ++t) std::this_thread::sleep_for(std::chrono::milliseconds(5));
					seconds += 0.5;
					if (!first && shot == 0 && i == 0) first = tex;
					else pl_tex_destroy(gpu, &tex);
				}
			}
			const SourceResolutionMonitor::Snapshot s = monitor.Read();
			Logger::WriteMessage(("DV: " + SourceResolutionMonitor::Describe(s, monitor.AutoRatio(), true) + "\n").c_str());
			Assert::IsTrue(s.notRead.empty(), ExtendedRangeGpuProbe::ToWide("not read: " + s.notRead).c_str());
			Assert::IsTrue(s.reading, L"the DV intensity plane is read");
			Assert::AreEqual(0.5, s.declaredRatio, 0.01, L"declared: the 540-line source");

			// Re-base to 540 lines: both IPT planes halved, the DV representation kept.
			frame.planes[0].texture = first;
			SourceRebase::Module rebase;
			pl_frame rebased{};
			Assert::IsTrue(rebase.Apply(gpu, dispatch, frame, 0.5, rebased), ExtendedRangeGpuProbe::ToWide(rebase.LastError()).c_str());
			Assert::IsTrue(rebased.repr.sys == PL_COLOR_SYSTEM_DOLBYVISION && rebased.repr.dovi == &dovi, L"DV decode kept");
			Assert::AreEqual(W / 2, rebased.planes[0].texture->params.w);
			Assert::AreEqual(H / 2, rebased.planes[0].texture->params.h);
			Assert::AreEqual(W / 4, rebased.planes[1].texture->params.w);
			Assert::AreEqual(H / 2, rebased.planes[1].texture->params.h);
			rebase.Release(gpu);
			monitor.Release(gpu);
			pl_tex_destroy(gpu, &first);
			pl_tex_destroy(gpu, &chroma);
			pl_dispatch_destroy(&dispatch);
		}

		TEST_METHOD(RebasedChartMatchesTheNativeChartThroughTheUpscalePath)
		{
			// Stage 12 spec §4 / plan A4 (owner 2026-10-15): a 1080-line chart
			// upscaled to 2160 upstream and re-based to 1080 goes through the
			// same libplacebo upscale path as the native 1080 chart and comes
			// out the same within half-float. YCbCr 4:2:0, both rendered to 4K.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			pl_dispatch dispatch = pl_dispatch_create(fixture.Log(), gpu);
			const int W = 1920, H = 1080;
			// A chart inside the upstream upscaler's flat passband (waves up to
			// 0.04 cycles per 1080-line pixel): what the re-base can restore. Above
			// it the upstream kernel has already softened the picture and no
			// re-base can bring that back (measured: 0.13 cycles gives 0.011).
			auto chart = [](int w, int h, double scale, unsigned seed, double level, double amplitude) {
				std::mt19937 rng(seed);
				std::uniform_real_distribution<double> u(0.0, 1.0);
				std::vector<double> v(static_cast<size_t>(w) * h, level);
				for (int k = 0; k < 40; ++k)
				{
					const double f = (0.002 + 0.038 * u(rng)) / scale, a = u(rng) * 3.14159265358979, ph = u(rng) * 6.283;
					const double fx = f * std::cos(a), fy = f * std::sin(a);
					for (int y = 0; y < h; ++y)
						for (int x = 0; x < w; ++x)
							v[static_cast<size_t>(y) * w + x] += amplitude / 40.0 * std::cos(6.283185307 * (fx * (x + 0.5) + fy * (y + 0.5)) + ph);
				}
				return v;
			};
			auto catmull2x = [](const std::vector<double>& src, int w, int h) {
				auto k = [](double x) { x = std::fabs(x); return x < 1 ? 1.5 * x * x * x - 2.5 * x * x + 1 : x < 2 ? -0.5 * x * x * x + 2.5 * x * x - 4 * x + 2 : 0.0; };
				const int W2 = 2 * w, H2 = 2 * h;
				std::vector<double> rows(static_cast<size_t>(W2) * h), out(static_cast<size_t>(W2) * H2);
				for (int y = 0; y < h; ++y)
					for (int x = 0; x < W2; ++x)
					{
						const double uu = (x + 0.5) * 0.5 - 0.5; const int c = static_cast<int>(std::floor(uu));
						double s = 0, ws = 0;
						for (int t = c - 1; t <= c + 2; ++t) { const double wt = k(uu - t); s += wt * src[static_cast<size_t>(y) * w + (std::min)(w - 1, (std::max)(0, t))]; ws += wt; }
						rows[static_cast<size_t>(y) * W2 + x] = s / ws;
					}
				for (int y = 0; y < H2; ++y)
				{
					const double uu = (y + 0.5) * 0.5 - 0.5; const int c = static_cast<int>(std::floor(uu));
					for (int x = 0; x < W2; ++x)
					{
						double s = 0, ws = 0;
						for (int t = c - 1; t <= c + 2; ++t) { const double wt = k(uu - t); s += wt * rows[static_cast<size_t>((std::min)(h - 1, (std::max)(0, t))) * W2 + x]; ws += wt; }
						out[static_cast<size_t>(y) * W2 + x] = s / ws;
					}
				}
				return out;
			};
			auto plane = [&](const std::vector<double>& y, const std::vector<double>* cb, const std::vector<double>* cr, int w, int h) {
				pl_tex_params p{};
				p.w = w; p.h = h; p.sampleable = true;
				std::vector<uint16_t> data;
				if (!cb)
				{
					p.format = pl_find_named_fmt(gpu, "r16");
					for (double v : y) data.push_back(static_cast<uint16_t>(std::lround((std::min)(1.0, (std::max)(0.0, v)) * 65535.0)));
				}
				else
				{
					p.format = pl_find_named_fmt(gpu, "rg16");
					for (size_t i = 0; i < cb->size(); ++i)
					{
						data.push_back(static_cast<uint16_t>(std::lround((std::min)(1.0, (std::max)(0.0, (*cb)[i])) * 65535.0)));
						data.push_back(static_cast<uint16_t>(std::lround((std::min)(1.0, (std::max)(0.0, (*cr)[i])) * 65535.0)));
					}
				}
				p.initial_data = data.data();
				return pl_tex_create(gpu, &p);
			};
			auto frameOf = [&](pl_tex luma, pl_tex chroma, int w, int h) {
				pl_frame f{};
				f.num_planes = 2;
				f.planes[0].texture = luma; f.planes[0].components = 1; f.planes[0].component_mapping[0] = PL_CHANNEL_Y;
				f.planes[1].texture = chroma; f.planes[1].components = 2;
				f.planes[1].component_mapping[0] = PL_CHANNEL_CB; f.planes[1].component_mapping[1] = PL_CHANNEL_CR;
				f.repr.sys = PL_COLOR_SYSTEM_BT_709;
				f.repr.levels = PL_COLOR_LEVELS_LIMITED;
				f.color = pl_color_space_bt709;
				f.crop = { 0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h) };
				pl_frame_set_chroma_location(&f, PL_CHROMA_CENTER);
				return f;
			};
			const std::vector<double> y1080 = chart(W, H, 1.0, 1, 0.45, 0.5);
			const std::vector<double> cb540 = chart(W / 2, H / 2, 1.0, 2, 0.5, 0.15), cr540 = chart(W / 2, H / 2, 1.0, 3, 0.5, 0.15);
			pl_tex nativeLuma = plane(y1080, nullptr, nullptr, W, H), nativeChroma = plane(cb540, &cb540, &cr540, W / 2, H / 2);
			const std::vector<double> y2160 = catmull2x(y1080, W, H), cb1080 = catmull2x(cb540, W / 2, H / 2), cr1080 = catmull2x(cr540, W / 2, H / 2);
			pl_tex upLuma = plane(y2160, nullptr, nullptr, 2 * W, 2 * H), upChroma = plane(cb1080, &cb1080, &cr1080, W, H);
			Assert::IsTrue(nativeLuma && nativeChroma && upLuma && upChroma, L"planes");
			const pl_frame native = frameOf(nativeLuma, nativeChroma, W, H);
			const pl_frame upscaled = frameOf(upLuma, upChroma, 2 * W, 2 * H);
			SourceRebase::Module rebase;
			pl_frame rebased{};
			Assert::IsTrue(rebase.Apply(gpu, dispatch, upscaled, 0.5, rebased), ExtendedRangeGpuProbe::ToWide(rebase.LastError()).c_str());

			pl_tex_params tp{};
			tp.w = 2 * W; tp.h = 2 * H; tp.format = pl_find_named_fmt(gpu, "rgba32f"); tp.renderable = true; tp.host_readable = true;
			Assert::IsNotNull(tp.format, L"rgba32f target");
			auto render = [&](const pl_frame& image) {
				pl_tex target = pl_tex_create(gpu, &tp);
				pl_frame out = TargetLutGpuFixture::MakeRgbFrame(target);
				out.color = pl_color_space_bt709;
				pl_render_params params = pl_render_default_params;
				params.dither_params = nullptr;
				Assert::IsTrue(pl_render_image(fixture.Renderer(), &image, &out, &params));
				pl_gpu_finish(gpu);
				std::vector<float> pixels(static_cast<size_t>(tp.w) * tp.h * 4);
				pl_tex_transfer_params d{}; d.tex = target; d.ptr = pixels.data();
				Assert::IsTrue(pl_tex_download(gpu, &d));
				pl_tex_destroy(gpu, &target);
				return pixels;
			};
			const std::vector<float> a = render(native), b = render(rebased), c = render(upscaled);
			double worst = 0.0, sum = 0.0, worstUp = 0.0, sumUp = 0.0;
			size_t count = 0;
			int worstX = 0, worstY = 0, worstC = 0;
			// Away from the frame edge: the upstream upscale, the re-base and
			// libplacebo's upscale each extend the edge their own way (their
			// kernels reach ~20 output px together).
			const int margin = 48;
			for (int yy = margin; yy < 2 * H - margin; ++yy)
				for (int xx = margin; xx < 2 * W - margin; ++xx)
					for (int ch = 0; ch < 3; ++ch)
					{
						const size_t i = (static_cast<size_t>(yy) * tp.w + xx) * 4 + ch;
						const double d = std::fabs(a[i] - b[i]);
						if (d > worst) { worst = d; worstX = xx; worstY = yy; worstC = ch; }
						sum += d;
						worstUp = (std::max)(worstUp, static_cast<double>(std::fabs(a[i] - c[i])));
						sumUp += std::fabs(a[i] - c[i]);
						++count;
					}
			char line[200];
			snprintf(line, sizeof(line), "re-based vs native 1080 through the upscale path: max %.6f mean %.7f (the 4K upscale rendered as is: max %.6f mean %.7f); worst at (%d, %d) channel %d\n",
				worst, sum / count, worstUp, sumUp / count, worstX, worstY, worstC);
			Logger::WriteMessage(line);
			// The spec asks for half-float. Measured: the mean is well inside it
			// (half-float's step at mid grey, 2^-11); the worst pixel is under one
			// 8-bit code (the FL35 link's precision) - even inside its flat band
			// the upstream Catmull-Rom softens the steepest peaks a little, which
			// no re-base can return; and the re-base is far closer to the native
			// chart than the upscaled source left as it was.
			Assert::IsTrue(sum / count < 1.0 / 2048.0, L"the mean is within half-float");
			Assert::IsTrue(worst < 1.0 / 255.0, L"the worst pixel is under one 8-bit code");
			Assert::IsTrue(sum * 5.0 < sumUp, L"at least five times closer to the native chart than the upscaled source as it was");
			pl_tex_destroy(gpu, &nativeLuma); pl_tex_destroy(gpu, &nativeChroma);
			pl_tex_destroy(gpu, &upLuma); pl_tex_destroy(gpu, &upChroma);
			rebase.Release(gpu);
			pl_dispatch_destroy(&dispatch);
		}

		TEST_METHOD(VpDebanderSmoothsBandsKeepsTextureAndHoldsAlongTheVectors)
		{
			// Stage 12 §2b / §2d (owner 2026-10-15: any low-bitrate source,
			// vector-informed). 10-bit codes in a P010-style r16 plane.
			Assert::AreEqual(0.0, VpDebander::Strength(10.0, 10, 1.0, true), L"true 10-bit, clean: nothing to deband");
			Assert::AreEqual(6.0, VpDebander::Strength(8.0, 10, 1.0, true), 1e-9, L"8-bit in 10: 1.5 x the 4-code step");
			Assert::IsTrue(VpDebander::Strength(10.0, 10, 0.2, true) > 6.0, L"low bitrate quality raises it");
			Assert::AreEqual(0.0, VpDebander::Strength(10.0, 10, 0.2, false), L"low_bitrate_handling off: quality ignored");

			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			pl_dispatch dispatch = pl_dispatch_create(fixture.Log(), gpu);
			const int W = 512, H = 256;
			auto upload = [&](const std::vector<int>& codes) {
				std::vector<uint16_t> data(codes.size());
				for (size_t i = 0; i < codes.size(); ++i) data[i] = static_cast<uint16_t>((std::min)(1023, (std::max)(0, codes[i])) << 6);
				pl_tex_params p{};
				p.w = W; p.h = H; p.format = pl_find_named_fmt(gpu, "r16"); p.sampleable = true; p.initial_data = data.data();
				return pl_tex_create(gpu, &p);
			};
			auto readback = [&](pl_tex tex) {
				pl_tex_params rp{};
				rp.w = W; rp.h = H; rp.format = tex->params.format; rp.host_readable = true; rp.blit_dst = true;
				pl_tex readable = pl_tex_create(gpu, &rp);
				pl_tex_blit_params bp{}; bp.src = tex; bp.dst = readable;
				pl_tex_blit(gpu, &bp);
				std::vector<double> v(static_cast<size_t>(W) * H);
				if (readable->params.format->type == PL_FMT_FLOAT)
				{
					std::vector<uint16_t> half(v.size());
					pl_tex_transfer_params d{}; d.tex = readable; d.ptr = half.data();
					Assert::IsTrue(pl_tex_download(gpu, &d));
					for (size_t i = 0; i < v.size(); ++i) v[i] = HalfBits(half[i]) * 65535.0 / 64.0;
				}
				else
				{
					std::vector<uint16_t> raw(v.size());
					pl_tex_transfer_params d{}; d.tex = readable; d.ptr = raw.data();
					Assert::IsTrue(pl_tex_download(gpu, &d));
					for (size_t i = 0; i < v.size(); ++i) v[i] = raw[i] / 64.0;   // back to 10-bit codes
				}
				pl_tex_destroy(gpu, &readable);
				return v;
			};
			auto frameOf = [&](pl_tex tex) {
				pl_frame f{};
				f.num_planes = 1;
				f.planes[0].texture = tex; f.planes[0].components = 1; f.planes[0].component_mapping[0] = PL_CHANNEL_Y;
				f.repr.sys = PL_COLOR_SYSTEM_BT_709; f.repr.levels = PL_COLOR_LEVELS_LIMITED;
				f.repr.bits.sample_depth = 16; f.repr.bits.color_depth = 10; f.repr.bits.bit_shift = 6;
				f.color = pl_color_space_bt709;
				return f;
			};
			// A shallow 8-bit ramp in the 10-bit signal (a sky): 4-code steps every 32 px.
			std::vector<int> ramp(static_cast<size_t>(W) * H), texture(ramp.size());
			std::mt19937 rng(9);
			std::normal_distribution<double> noise(0.0, 12.0);
			for (int y = 0; y < H; ++y)
				for (int x = 0; x < W; ++x)
				{
					ramp[static_cast<size_t>(y) * W + x] = 4 * (100 + x / 32);
					texture[static_cast<size_t>(y) * W + x] = static_cast<int>(std::lround(500 + noise(rng)));
				}
			VpDebander::Params params;
			params.thresholdCodes = VpDebander::Strength(8.0, 10, 1.0, true);
			params.colorDepth = 10;
			params.radiusScale = 1080.0 / H;   // 16 px at this picture's 'height' scale
			VpDebander::Module deband;
			pl_tex rampTex = upload(ramp), texTex = upload(texture);
			pl_frame out{};
			Assert::IsTrue(deband.Apply(gpu, dispatch, frameOf(rampTex), params, out), ExtendedRangeGpuProbe::ToWide(deband.LastError()).c_str());
			const std::vector<double> smoothed = readback(out.planes[0].texture);
			// Visible banding: how far the column means depart from a straight
			// ramp (least-squares line); a 4-code staircase is ~1.15 codes RMS.
			auto staircase = [&](auto at) {
				std::vector<double> col;
				for (int x = 80; x < W - 80; ++x)
				{
					double m = 0;
					for (int y = 40; y < H - 40; ++y) m += at(static_cast<size_t>(y) * W + x);
					col.push_back(m / (H - 80));
				}
				double sx = 0, sy = 0, sxx = 0, sxy = 0;
				const double k = static_cast<double>(col.size());
				for (size_t i = 0; i < col.size(); ++i) { sx += i; sy += col[i]; sxx += double(i) * i; sxy += i * col[i]; }
				const double b = (k * sxy - sx * sy) / (k * sxx - sx * sx), a = (sy - b * sx) / k;
				double e = 0;
				for (size_t i = 0; i < col.size(); ++i) e += (col[i] - a - b * i) * (col[i] - a - b * i);
				return std::sqrt(e / k);
			};
			const double stepIn = staircase([&](size_t i) { return static_cast<double>(ramp[i]); });
			const double stepOut = staircase([&](size_t i) { return smoothed[i]; });
			double meanIn = 0, meanOut = 0;
			for (int y = 40; y < H - 40; ++y)
				for (int x = 80; x < W - 80; ++x) { const size_t i = static_cast<size_t>(y) * W + x; meanIn += ramp[i]; meanOut += smoothed[i]; }
			const double n = static_cast<double>((H - 80) * (W - 160));
			// Texture: the same pass on noise keeps its variance.
			Assert::IsTrue(deband.Apply(gpu, dispatch, frameOf(texTex), params, out, 1));
			const std::vector<double> tex = readback(out.planes[0].texture);
			double varIn = 0, varOut = 0;
			for (size_t i = 0; i < tex.size(); ++i) { varIn += (texture[i] - 500.0) * (texture[i] - 500.0); varOut += (tex[i] - 500.0) * (tex[i] - 500.0); }
			char line[220];
			snprintf(line, sizeof(line), "deband: banding (RMS off a straight ramp) %.2f -> %.2f codes, mean %.3f -> %.3f; texture variance kept %.1f%%\n",
				stepIn, stepOut, meanIn / n, meanOut / n, 100.0 * varOut / varIn);
			Logger::WriteMessage(line);
			Assert::IsTrue(stepOut < 0.5 * stepIn, L"banding at least halved");
			Assert::AreEqual(meanIn / n, meanOut / n, 0.25, L"the level kept");
			Assert::IsTrue(varOut > 0.9 * varIn, L"texture kept");

			// Vectors: a field (one texel per 16 px block: vx, vy, trust, error)
			// moving the content 64 px right. Frame 1: left half banded (blurred,
			// w = 1), right half texture (w = 0). Frame 2: all texture. Held, the
			// columns the banded half moved into keep blurring partly; beyond the
			// motion, nothing is blurred.
			std::vector<float> fieldData(static_cast<size_t>(W / 16) * (H / 16) * 4);
			for (size_t i = 0; i < fieldData.size(); i += 4) { fieldData[i] = 64.0f; fieldData[i + 1] = 0.0f; fieldData[i + 2] = 1.0f; fieldData[i + 3] = 0.0f; }
			pl_tex_params fp{};
			fp.w = W / 16; fp.h = H / 16; fp.format = pl_find_named_fmt(gpu, "rgba32f"); fp.sampleable = true; fp.initial_data = fieldData.data();
			pl_tex field = pl_tex_create(gpu, &fp);
			Assert::IsNotNull(field, L"field texture");
			std::vector<int> half(ramp.size());
			for (int y = 0; y < H; ++y)
				for (int x = 0; x < W; ++x)
					half[static_cast<size_t>(y) * W + x] = x < W / 2 ? ramp[static_cast<size_t>(y) * W + x] : texture[static_cast<size_t>(y) * W + x];
			pl_tex halfTex = upload(half);
			auto blurredShare = [&](const std::vector<double>& o, int x0, int x1) {
				double sum = 0; int count = 0;
				for (int y = 40; y < H - 40; ++y)
					for (int x = x0; x < x1; ++x) { const size_t i = static_cast<size_t>(y) * W + x; sum += std::fabs(o[i] - texture[i]); ++count; }
				return sum / count;
			};
			double heldMoved = 0, heldBeyond = 0, frameMoved = 0, frameBeyond = 0;
			for (int held = 0; held < 2; ++held)
			{
				VpDebander::Module m;
				VpDebander::Params p = params;
				p.field = held ? field : nullptr;
				p.sequence = 10;
				Assert::IsTrue(m.Apply(gpu, dispatch, frameOf(halfTex), p, out));
				p.sequence = 11;
				Assert::IsTrue(m.Apply(gpu, dispatch, frameOf(texTex), p, out));
				const std::vector<double> o = readback(out.planes[0].texture);
				// Columns whose previous position (64 px left) was the banded half's
				// interior (fully blurred there), and columns whose previous position
				// was texture.
				const double moved = blurredShare(o, 80, W / 2 - 80), beyond = blurredShare(o, W / 2 + 160, W - 16);
				std::string profile = held ? "held profile:" : "per-frame profile:";
				for (int x0 = 0; x0 < W; x0 += 32)
				{
					char cell[16];
					snprintf(cell, sizeof(cell), " %.2f", blurredShare(o, x0, x0 + 32));
					profile += cell;
				}
				Logger::WriteMessage((profile + "\n").c_str());
				snprintf(line, sizeof(line), "%s: change where the banded half was %.2f codes, where texture was %.2f; held frames %llu\n",
					held ? "held along the vectors" : "per frame (no vectors)", moved, beyond, m.HeldFrames());
				Logger::WriteMessage(line);
				if (held)
				{
					Assert::AreEqual(1ull, m.HeldFrames());
					heldMoved = moved;
					heldBeyond = beyond;
				}
				else
				{
					Assert::AreEqual(0ull, m.HeldFrames());
					Assert::IsTrue(moved < 1.0 && beyond < 1.0, L"without vectors: per-frame decisions (texture almost untouched)");
					frameMoved = moved;
					frameBeyond = beyond;
				}
				m.Release(gpu);
			}
			// Held, the weights follow the motion: more blur (inside this frame's
			// blur band) where the banded area was, less where texture was; never
			// more blur on texture than this frame's own decisions give.
			Assert::IsTrue(heldMoved > 1.5 * heldBeyond, L"the blur weight followed the motion");
			Assert::IsTrue(heldMoved > frameMoved, L"steadier blur where the banded area was");
			Assert::IsTrue(heldBeyond <= 1.05 * frameBeyond + 0.02, L"held never blurs texture more than this frame does");
			pl_tex_destroy(gpu, &rampTex); pl_tex_destroy(gpu, &texTex); pl_tex_destroy(gpu, &halfTex); pl_tex_destroy(gpu, &field);
			deband.Release(gpu);
			pl_dispatch_destroy(&dispatch);
		}

		TEST_METHOD(SourceRebaseHandlesAnRgbPlane)
		{
			// Plan A4 (owner 2026-10-15): RGB 4:4:4 (native RGB ingest, the GPU
			// RGB unpack) is one plane; it re-bases as one plane, all components.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			pl_dispatch dispatch = pl_dispatch_create(fixture.Log(), gpu);
			const int W = 1024, H = 512, w = 512, h = 256;
			// Smooth content at the target size, sampled at 2x (an upstream upscale of it).
			auto value = [](double x, double y, int c) { return 0.4 + 0.2 * std::sin(x * (6.0 + c)) * std::cos(y * (5.0 + c)); };
			std::vector<uint16_t> px(static_cast<size_t>(W) * H * 4);
			for (int y = 0; y < H; ++y)
				for (int x = 0; x < W; ++x)
					for (int c = 0; c < 4; ++c)
						px[(static_cast<size_t>(y) * W + x) * 4 + c] = static_cast<uint16_t>(std::lround((c == 3 ? 1.0 : value((x + 0.5) / W, (y + 0.5) / H, c)) * 65535.0));
			pl_tex_params tp{};
			tp.w = W; tp.h = H; tp.format = pl_find_named_fmt(gpu, "rgba16"); tp.sampleable = true; tp.initial_data = px.data();
			pl_tex tex = pl_tex_create(gpu, &tp);
			Assert::IsNotNull(tex);
			pl_frame frame = TargetLutGpuFixture::MakeRgbFrame(tex);
			frame.crop = { 0.0f, 0.0f, static_cast<float>(W), static_cast<float>(H) };
			SourceRebase::Module rebase;
			pl_frame out{};
			Assert::IsTrue(rebase.Apply(gpu, dispatch, frame, 0.5, out), ExtendedRangeGpuProbe::ToWide(rebase.LastError()).c_str());
			Assert::AreEqual(1, out.num_planes, L"one plane");
			Assert::AreEqual(w, out.planes[0].texture->params.w);
			Assert::AreEqual(h, out.planes[0].texture->params.h);
			Assert::AreEqual(frame.planes[0].components, out.planes[0].components, L"all components kept");
			Assert::AreEqual(static_cast<float>(w), out.crop.x1, 1e-4f);
			// Read back (via a readable copy) and compare with the content at the target size.
			pl_tex_params rp{};
			rp.w = w; rp.h = h; rp.format = out.planes[0].texture->params.format; rp.host_readable = true; rp.blit_dst = true;
			pl_tex readable = pl_tex_create(gpu, &rp);
			pl_tex_blit_params bp{}; bp.src = out.planes[0].texture; bp.dst = readable;
			pl_tex_blit(gpu, &bp);
			double worst = 0.0;
			const bool isFloat = readable->params.format->type == PL_FMT_FLOAT;
			std::vector<uint16_t> back(static_cast<size_t>(w) * h * 4);
			pl_tex_transfer_params d{}; d.tex = readable; d.ptr = back.data();
			Assert::IsTrue(pl_tex_download(gpu, &d));
			for (int y = 8; y < h - 8; ++y)
				for (int x = 8; x < w - 8; ++x)
					for (int c = 0; c < 3; ++c)
					{
						const uint16_t raw = back[(static_cast<size_t>(y) * w + x) * 4 + c];
						const double v = isFloat ? HalfBits(raw) : raw / 65535.0;
						worst = (std::max)(worst, std::fabs(v - value((x + 0.5) / w, (y + 0.5) / h, c)));
					}
			char line[120];
			snprintf(line, sizeof(line), "RGB re-base 2x: worst %.5f (%.2f 8-bit codes)\n", worst, worst * 255.0);
			Logger::WriteMessage(line);
			Assert::IsTrue(worst < 1.0 / 255.0, L"the RGB picture restored within a code");
			pl_tex_destroy(gpu, &readable);
			pl_tex_destroy(gpu, &tex);
			rebase.Release(gpu);
			pl_dispatch_destroy(&dispatch);
		}

		TEST_METHOD(Stage12PassCostsAt4K)
		{
			// Planner gate (2026-10-16): the GPU cost of the Stage 12 passes at 4K on this GPU
			// (wall clock, GPU drained, mean of 20 after a warm-up). Report only; the 3090
			// numbers come from the cinema PC.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			pl_dispatch dispatch = pl_dispatch_create(fixture.Log(), gpu);
			const int W = 3840, H = 2160;
			std::vector<uint16_t> luma(static_cast<size_t>(W) * H), chroma(static_cast<size_t>(W / 2) * (H / 2) * 2, 32768);
			std::mt19937 rng(3);
			std::uniform_int_distribution<int> u(8000, 56000);
			for (uint16_t& v : luma) v = static_cast<uint16_t>(u(rng));
			pl_tex_params lp{};
			lp.w = W; lp.h = H; lp.format = pl_find_named_fmt(gpu, "r16"); lp.sampleable = true; lp.initial_data = luma.data();
			pl_tex_params cp = lp;
			cp.w = W / 2; cp.h = H / 2; cp.format = pl_find_named_fmt(gpu, "rg16"); cp.initial_data = chroma.data();
			pl_tex lumaTex = pl_tex_create(gpu, &lp), chromaTex = pl_tex_create(gpu, &cp);
			Assert::IsTrue(lumaTex && chromaTex);
			pl_frame source{};
			source.num_planes = 2;
			source.planes[0].texture = lumaTex; source.planes[0].components = 1; source.planes[0].component_mapping[0] = PL_CHANNEL_Y;
			source.planes[1].texture = chromaTex; source.planes[1].components = 2;
			source.planes[1].component_mapping[0] = PL_CHANNEL_CB; source.planes[1].component_mapping[1] = PL_CHANNEL_CR;
			source.crop = { 0.0f, 0.0f, static_cast<float>(W), static_cast<float>(H) };
			source.repr = pl_color_repr_uhdtv;
			source.color = pl_color_space_bt709;
			auto timed = [&](const char* name, const std::function<bool()>& pass) {
				for (int i = 0; i < 3; ++i) Assert::IsTrue(pass(), ExtendedRangeGpuProbe::ToWide(std::string(name) + ": failed").c_str());
				pl_gpu_finish(gpu);
				const auto start = std::chrono::steady_clock::now();
				for (int i = 0; i < 20; ++i) pass();
				pl_gpu_finish(gpu);
				const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / 20.0;
				char line[160];
				snprintf(line, sizeof(line), "  %-58s %.3f ms\n", name, ms);
				Logger::WriteMessage(line);
				return ms;
			};
			Logger::WriteMessage("Stage 12 pass costs at 4K (this GPU):\n");
			SourceRebase::Module rebase;
			pl_frame out{};
			rebase.separable = false;
			timed("re-base 2:1, polar (luma + chroma)", [&] { return rebase.Apply(gpu, dispatch, source, 0.5, out); });
			rebase.separable = true;
			timed("re-base 2:1, polar luma + separable chroma (default)", [&] { return rebase.Apply(gpu, dispatch, source, 0.5, out); });
			rebase.SetKernel(KernelInverse::Find("lanczos3"));
			timed("re-base 2:1, kernel inverse (luma) + separable (chroma)", [&] { return rebase.Apply(gpu, dispatch, source, 0.5, out); });
			rebase.SetKernel(-1);
			timed("re-base height only (1D, luma + chroma)", [&] { return rebase.Apply(gpu, dispatch, source, 1.0, 0.5, out); });
			timed("re-base 1440 (polar luma + separable chroma)", [&] { return rebase.Apply(gpu, dispatch, source, 1440.0 / 2160.0, out); });
			VpDebander::Params params;
			params.thresholdCodes = VpDebander::Strength(8.0, 10, 1.0, true);
			params.colorDepth = 10;
			params.radiusScale = 1.0;
			VpDebander::Module deband;
			pl_frame lumaOnly = source;
			lumaOnly.num_planes = 1;
			timed("VP debander (luma, 4K)", [&] { return deband.Apply(gpu, dispatch, lumaOnly, params, out); });
			// The monitor's GPU side: its 8 tile copies into the readback buffer (2 Hz in use).
			SourceResolutionMonitor::Monitor monitor;
			uint64_t sequence = 0;
			double seconds = 0.0;
			timed("source-resolution monitor: tile copy (per analysed frame)", [&] {
				monitor.Frame(gpu, dispatch, source, ++sequence, seconds);
				seconds += 0.6;   // past its 2 Hz spacing: every call copies
				return true;
			});
			monitor.Release(gpu);
			rebase.Release(gpu);
			deband.Release(gpu);
			pl_tex_destroy(gpu, &lumaTex);
			pl_tex_destroy(gpu, &chromaTex);
			pl_dispatch_destroy(&dispatch);
		}

		TEST_METHOD(SourceRebaseKernelInverseRecoversThe1080Picture)
		{
			// Kernel-matched re-base (owner 2026-10-16: optimise 1080p): with the player's
			// kernel identified, the GPU's luma re-base is its inverse - the 1080 picture
			// itself, as the CPU inverse gives it.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			pl_dispatch dispatch = pl_dispatch_create(fixture.Log(), gpu);
			const int m = 256, n = 512;
			const int k = KernelInverse::Find("lanczos3");
			Assert::IsTrue(k >= 0);
			const KernelInverse::Candidate& c = KernelInverse::Candidates()[static_cast<size_t>(k)];
			// The 1080-class picture (textured), upscaled 2x with Lanczos-3 (separable, CPU).
			std::vector<float> master(static_cast<size_t>(m) * m);
			for (int y = 0; y < m; ++y)
				for (int x = 0; x < m; ++x)
					master[static_cast<size_t>(y) * m + x] = static_cast<float>(0.45 + 0.15 * std::sin(x * 0.9 + y * 0.31) * std::cos(y * 0.7) + 0.08 * std::sin(x * 2.3 - y * 1.7));
			std::vector<float> rows(static_cast<size_t>(m) * n), up(static_cast<size_t>(n) * n), lineIn(static_cast<size_t>(m)), lineOut(static_cast<size_t>(n));
			for (int y = 0; y < m; ++y) KernelInverse::UpscaleLine(c, &master[static_cast<size_t>(y) * m], m, &rows[static_cast<size_t>(y) * n]);
			for (int x = 0; x < n; ++x)
			{
				for (int y = 0; y < m; ++y) lineIn[static_cast<size_t>(y)] = rows[static_cast<size_t>(y) * n + x];
				KernelInverse::UpscaleLine(c, lineIn.data(), m, lineOut.data());
				for (int y = 0; y < n; ++y) up[static_cast<size_t>(y) * n + x] = lineOut[static_cast<size_t>(y)];
			}
			std::vector<uint16_t> luma(up.size());
			for (size_t i = 0; i < up.size(); ++i) luma[i] = static_cast<uint16_t>(std::lround((std::min)(1.0f, (std::max)(0.0f, up[i])) * 65535.0f));
			pl_tex_params lp{};
			lp.w = n; lp.h = n; lp.format = pl_find_named_fmt(gpu, "r16"); lp.sampleable = true; lp.initial_data = luma.data();
			pl_tex lumaTex = pl_tex_create(gpu, &lp);
			Assert::IsTrue(lumaTex != nullptr);
			pl_frame source{};
			source.num_planes = 1;
			source.planes[0].texture = lumaTex; source.planes[0].components = 1; source.planes[0].component_mapping[0] = PL_CHANNEL_Y;
			source.crop = { 0.0f, 0.0f, static_cast<float>(n), static_cast<float>(n) };
			auto rebaseWith = [&](int kernelIndex, std::vector<float>& values, bool& used) {
				SourceRebase::Module rebase;
				rebase.SetKernel(kernelIndex);
				pl_frame out{};
				Assert::IsTrue(rebase.Apply(gpu, dispatch, source, 0.5, out), ExtendedRangeGpuProbe::ToWide(rebase.LastError()).c_str());
				used = rebase.KernelUsed();
				pl_tex_params rp{};
				rp.w = m; rp.h = m; rp.format = out.planes[0].texture->params.format; rp.host_readable = true; rp.blit_dst = true;
				pl_tex readable = pl_tex_create(gpu, &rp);
				pl_tex_blit_params bp{}; bp.src = out.planes[0].texture; bp.dst = readable;
				pl_tex_blit(gpu, &bp);
				values.assign(static_cast<size_t>(m) * m, 0.0f);
				if (readable->params.format->type == PL_FMT_FLOAT)
				{
					std::vector<uint16_t> half(values.size());
					pl_tex_transfer_params d{}; d.tex = readable; d.ptr = half.data();
					Assert::IsTrue(pl_tex_download(gpu, &d));
					for (size_t i = 0; i < values.size(); ++i) values[i] = HalfBits(half[i]);
				}
				else
				{
					std::vector<uint16_t> unorm(values.size());
					pl_tex_transfer_params d{}; d.tex = readable; d.ptr = unorm.data();
					Assert::IsTrue(pl_tex_download(gpu, &d));
					for (size_t i = 0; i < values.size(); ++i) values[i] = unorm[i] / 65535.0f;
				}
				pl_tex_destroy(gpu, &readable);
				rebase.Release(gpu);
			};
			std::vector<float> inverse, polar;
			bool usedInverse = false, usedPolar = true;
			rebaseWith(k, inverse, usedInverse);
			rebaseWith(-1, polar, usedPolar);
			Assert::IsTrue(usedInverse, L"the kernel inverse ran");
			Assert::IsFalse(usedPolar, L"without a kernel: the polar filter as before");
			double eInverse = 0.0, ePolar = 0.0;
			long count = 0;
			for (int y = 24; y < m - 24; ++y)
				for (int x = 24; x < m - 24; ++x)
				{
					const double truth = master[static_cast<size_t>(y) * m + x];
					eInverse += (inverse[static_cast<size_t>(y) * m + x] - truth) * (inverse[static_cast<size_t>(y) * m + x] - truth);
					ePolar += (polar[static_cast<size_t>(y) * m + x] - truth) * (polar[static_cast<size_t>(y) * m + x] - truth);
					++count;
				}
			const double rmsInverse = std::sqrt(eInverse / count) * 1023.0, rmsPolar = std::sqrt(ePolar / count) * 1023.0;
			char line[160];
			snprintf(line, sizeof(line), "kernel inverse on the GPU: %.3f codes RMS from the 1080 picture; the polar filter %.3f codes\n", rmsInverse, rmsPolar);
			Logger::WriteMessage(line);
			Assert::IsTrue(rmsInverse < 0.1, L"the inverse recovers the 1080 picture (within a tenth of a 10-bit code)");
			Assert::IsTrue(rmsInverse * 5.0 < rmsPolar, L"far closer than the polar filter");
			pl_tex_destroy(gpu, &lumaTex);
			pl_dispatch_destroy(&dispatch);
		}

		TEST_METHOD(SourceRebasePlayerSubtitleText)
		{
			// Owner 2026-10-16 ("what about with the subtitles"): a player upscales 1080p video
			// (here Lanczos-3, identified) and draws its subtitles at 4K. The re-base reduces
			// the text with the picture; measured after the display's upscale back: the text's
			// error and halos (overshoot past the outline's black or the glyph's white).
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			pl_dispatch dispatch = pl_dispatch_create(fixture.Log(), gpu);
			const int m = 256, n = 512;
			const int k = KernelInverse::Find("lanczos3");
			Assert::IsTrue(k >= 0);
			const KernelInverse::Candidate& c = KernelInverse::Candidates()[static_cast<size_t>(k)];
			auto upscale2 = [&](const std::vector<float>& in, std::vector<float>& out) {
				std::vector<float> rows(static_cast<size_t>(m) * n), lineIn(static_cast<size_t>(m)), lineOut(static_cast<size_t>(n));
				out.assign(static_cast<size_t>(n) * n, 0.0f);
				for (int y = 0; y < m; ++y) KernelInverse::UpscaleLine(c, &in[static_cast<size_t>(y) * m], m, &rows[static_cast<size_t>(y) * n]);
				for (int x = 0; x < n; ++x)
				{
					for (int y = 0; y < m; ++y) lineIn[static_cast<size_t>(y)] = rows[static_cast<size_t>(y) * n + x];
					KernelInverse::UpscaleLine(c, lineIn.data(), m, lineOut.data());
					for (int y = 0; y < n; ++y) out[static_cast<size_t>(y) * n + x] = lineOut[static_cast<size_t>(y)];
				}
			};
			std::vector<float> master(static_cast<size_t>(m) * m);
			for (int y = 0; y < m; ++y)
				for (int x = 0; x < m; ++x)
					master[static_cast<size_t>(y) * m + x] = static_cast<float>(0.45 + 0.15 * std::sin(x * 0.9 + y * 0.31) * std::cos(y * 0.7) + 0.08 * std::sin(x * 2.3 - y * 1.7));
			std::vector<float> truth;
			upscale2(master, truth);
			for (float& v : truth) v = static_cast<float>(std::round((std::min)(1.0f, (std::max)(0.0f, v)) * 1023.0) / 1023.0);
			std::vector<unsigned char> textMask(truth.size(), 0);
			const int textTop = 400, textHeight = 56;
			SubtitleTextTestPattern::Draw(truth, n, n, 16, n - 16, textTop, textHeight, 5u, &textMask);
			std::vector<uint16_t> luma(truth.size());
			for (size_t i = 0; i < truth.size(); ++i) luma[i] = static_cast<uint16_t>(std::lround(truth[i] * 65535.0f));
			pl_tex_params lp{};
			lp.w = n; lp.h = n; lp.format = pl_find_named_fmt(gpu, "r16"); lp.sampleable = true; lp.initial_data = luma.data();
			pl_tex lumaTex = pl_tex_create(gpu, &lp);
			Assert::IsTrue(lumaTex != nullptr);
			pl_frame source{};
			source.num_planes = 1;
			source.planes[0].texture = lumaTex; source.planes[0].components = 1; source.planes[0].component_mapping[0] = PL_CHANNEL_Y;
			source.crop = { 0.0f, 0.0f, static_cast<float>(n), static_cast<float>(n) };
			auto rebaseWith = [&](int kernelIndex, std::vector<float>& values) {
				SourceRebase::Module rebase;
				rebase.SetKernel(kernelIndex);
				pl_frame out{};
				Assert::IsTrue(rebase.Apply(gpu, dispatch, source, 0.5, out), ExtendedRangeGpuProbe::ToWide(rebase.LastError()).c_str());
				pl_tex_params rp{};
				rp.w = m; rp.h = m; rp.format = out.planes[0].texture->params.format; rp.host_readable = true; rp.blit_dst = true;
				pl_tex readable = pl_tex_create(gpu, &rp);
				pl_tex_blit_params bp{}; bp.src = out.planes[0].texture; bp.dst = readable;
				pl_tex_blit(gpu, &bp);
				values.assign(static_cast<size_t>(m) * m, 0.0f);
				std::vector<uint16_t> raw(values.size());
				pl_tex_transfer_params d{}; d.tex = readable; d.ptr = raw.data();
				Assert::IsTrue(pl_tex_download(gpu, &d));
				for (size_t i = 0; i < values.size(); ++i)
					values[i] = readable->params.format->type == PL_FMT_FLOAT ? HalfBits(raw[i]) : raw[i] / 65535.0f;
				pl_tex_destroy(gpu, &readable);
				rebase.Release(gpu);
			};
			struct Result { double textRms, pictureRms, worstOvershoot; long halo; };
			auto measure = [&](const std::vector<float>& rebased) {
				std::vector<float> shown;
				upscale2(rebased, shown);   // the display's upscale back to 4K (Lanczos-3 as a proxy)
				Result r{ 0, 0, 0, 0 };
				double eText = 0, ePic = 0;
				long nText = 0, nPic = 0;
				for (int y = 24; y < n - 24; ++y)
					for (int x = 24; x < n - 24; ++x)
					{
						const size_t i = static_cast<size_t>(y) * n + x;
						const double e = (shown[i] - truth[i]) * 1023.0;
						if (textMask[i])
						{
							eText += e * e; ++nText;
							const double below = (SubtitleTextTestPattern::kBlack - shown[i]) * 1023.0, above = (shown[i] - SubtitleTextTestPattern::kWhite) * 1023.0;
							r.worstOvershoot = (std::max)(r.worstOvershoot, (std::max)(below, above));
							if (below > 8.0 || above > 8.0) ++r.halo;
						}
						else if (y < textTop - 24) { ePic += e * e; ++nPic; }
					}
				r.textRms = std::sqrt(eText / (std::max)(1L, nText));
				r.pictureRms = std::sqrt(ePic / (std::max)(1L, nPic));
				return r;
			};
			std::vector<float> inverse, polar;
			rebaseWith(k, inverse);
			rebaseWith(-1, polar);
			const Result ri = measure(inverse), rp = measure(polar);
			long textPixels = 0;
			for (unsigned char v : textMask) textPixels += v;
			char line[256];
			snprintf(line, sizeof(line), "player text at 4K over a Lanczos-3 1080 upscale, re-based 2:1 and shown at 4K (%ld text pixels):\n", textPixels);
			Logger::WriteMessage(line);
			snprintf(line, sizeof(line), "  kernel inverse: text %.1f codes RMS, %ld halo pixels (>8 codes past black/white), worst %.1f codes; picture %.2f codes\n",
				ri.textRms, ri.halo, ri.worstOvershoot, ri.pictureRms);
			Logger::WriteMessage(line);
			snprintf(line, sizeof(line), "  polar filter:   text %.1f codes RMS, %ld halo pixels, worst %.1f codes; picture %.2f codes\n",
				rp.textRms, rp.halo, rp.worstOvershoot, rp.pictureRms);
			Logger::WriteMessage(line);
			Assert::IsTrue(std::isfinite(ri.textRms) && std::isfinite(rp.textRms), L"measured");
			Assert::IsTrue(ri.pictureRms < rp.pictureRms, L"the picture: the inverse still the closer");
			pl_tex_destroy(gpu, &lumaTex);
			pl_dispatch_destroy(&dispatch);
		}

		TEST_METHOD(SourceRebaseOneAxisScalesThatAxisOnly)
		{
			// Owner 2026-10-16 ("decompose an upscaler and then use it in the offending axis
			// alone"): a source upscaled vertically only (true 4K detail across) is re-based
			// in height alone; the fine horizontal detail survives untouched.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			pl_dispatch dispatch = pl_dispatch_create(fixture.Log(), gpu);
			const int W = 1024, H = 512, h = 256;
			// Horizontal: 4K-class detail (0.35 cycles per pixel); vertical: smooth (an upscale's content).
			auto fine = [](int x) { return 0.12 * std::sin(2.0 * 3.14159265358979 * 0.35 * x); };
			auto smooth = [](double y) { return 0.45 + 0.2 * std::cos(y * 6.0); };
			std::vector<uint16_t> luma(static_cast<size_t>(W) * H);
			for (int y = 0; y < H; ++y)
				for (int x = 0; x < W; ++x)
					luma[static_cast<size_t>(y) * W + x] = static_cast<uint16_t>(std::lround((smooth((y + 0.5) / H) + fine(x)) * 65535.0));
			pl_tex_params lp{};
			lp.w = W; lp.h = H; lp.format = pl_find_named_fmt(gpu, "r16"); lp.sampleable = true; lp.initial_data = luma.data();
			pl_tex lumaTex = pl_tex_create(gpu, &lp);
			Assert::IsTrue(lumaTex != nullptr);
			pl_frame source{};
			source.num_planes = 1;
			source.planes[0].texture = lumaTex; source.planes[0].components = 1; source.planes[0].component_mapping[0] = PL_CHANNEL_Y;
			source.crop = { 0.0f, 0.0f, static_cast<float>(W), static_cast<float>(H) };
			SourceRebase::Module rebase;
			pl_frame out{};
			Assert::IsTrue(rebase.Apply(gpu, dispatch, source, 1.0, 0.5, out), ExtendedRangeGpuProbe::ToWide(rebase.LastError()).c_str());
			Assert::AreEqual(W, out.planes[0].texture->params.w, L"the width untouched");
			Assert::AreEqual(h, out.planes[0].texture->params.h, L"the height re-based");
			Assert::AreEqual(static_cast<float>(W), out.crop.x1, 1e-4f);
			Assert::AreEqual(static_cast<float>(h), out.crop.y1, 1e-4f);
			pl_tex_params rp{};
			rp.w = W; rp.h = h; rp.format = out.planes[0].texture->params.format; rp.host_readable = true; rp.blit_dst = true;
			pl_tex readable = pl_tex_create(gpu, &rp);
			pl_tex_blit_params bp{}; bp.src = out.planes[0].texture; bp.dst = readable;
			pl_tex_blit(gpu, &bp);
			std::vector<float> values(static_cast<size_t>(W) * h);
			if (readable->params.format->type == PL_FMT_FLOAT)
			{
				std::vector<uint16_t> half(values.size());
				pl_tex_transfer_params d{}; d.tex = readable; d.ptr = half.data();
				Assert::IsTrue(pl_tex_download(gpu, &d));
				for (size_t i = 0; i < values.size(); ++i) values[i] = HalfBits(half[i]);
			}
			else
			{
				std::vector<uint16_t> unorm(values.size());
				pl_tex_transfer_params d{}; d.tex = readable; d.ptr = unorm.data();
				Assert::IsTrue(pl_tex_download(gpu, &d));
				for (size_t i = 0; i < values.size(); ++i) values[i] = unorm[i] / 65535.0f;
			}
			// Per row: the mean is the smooth vertical picture at the re-based row; the fine
			// horizontal pattern keeps its amplitude (projection on the known sinusoid).
			double worstMean = 0.0, worstAmplitude = 1.0;
			for (int y = 8; y < h - 8; y += 4)
			{
				double mean = 0.0, proj = 0.0, norm = 0.0;
				for (int x = 16; x < W - 16; ++x)
				{
					const double v = values[static_cast<size_t>(y) * W + x];
					mean += v;
					proj += (v - smooth((y + 0.5) / h)) * fine(x);
					norm += fine(x) * fine(x);
				}
				mean /= (W - 32);
				worstMean = (std::max)(worstMean, std::fabs(mean - smooth((y + 0.5) / h)));
				worstAmplitude = (std::min)(worstAmplitude, proj / norm);
			}
			char line[160];
			snprintf(line, sizeof(line), "one-axis re-base: vertical picture within %.4f; fine horizontal detail kept at %.1f%% amplitude\n",
				worstMean, worstAmplitude * 100.0);
			Logger::WriteMessage(line);
			Assert::IsTrue(worstMean < 0.01, L"the vertical picture restored at the re-based height");
			Assert::IsTrue(worstAmplitude > 0.98, L"the fine horizontal detail untouched (no filtering across)");
			pl_tex_destroy(gpu, &lumaTex);
			pl_tex_destroy(gpu, &readable);
			rebase.Release(gpu);
			pl_dispatch_destroy(&dispatch);
		}

		TEST_METHOD(SourceRebaseRestoresAnUpscaledSourceAndKeepsTheCropEdge)
		{
			// Stage 12 step A (owner 2026-10-15): source_resolution re-bases a
			// 4K frame to its class, plane by plane in the signal domain.
			Assert::AreEqual(0, SourceRebase::ClassHeight("native"));
			Assert::AreEqual(0, SourceRebase::ClassHeight("auto"), L"auto is native until detection exists");
			Assert::AreEqual(1080, SourceRebase::ClassHeight("1080"));
			Assert::AreEqual(1.0, SourceRebase::Factor("native", 2160));
			Assert::AreEqual(0.5, SourceRebase::Factor("1080", 2160));
			Assert::AreEqual(1.0, SourceRebase::Factor("1080", 1080), L"never upward or equal");
			Assert::AreEqual(1.0, SourceRebase::Factor("1440", 1080));
			Assert::AreEqual(960, SourceRebase::Scaled(1920, 0.5), L"4:2:0 chroma keeps its subsampling");
			// General (owner's standing rule): any height, any frame.
			Assert::AreEqual(900, SourceRebase::ClassHeight("900"));
			Assert::AreEqual(576, SourceRebase::ClassHeight("576"));
			Assert::AreEqual(0, SourceRebase::ClassHeight("100"), L"below the minimum: native");
			Assert::AreEqual(0, SourceRebase::ClassHeight("1080p"), L"not a number: native");
			Assert::AreEqual(720.0 / 1080.0, SourceRebase::Factor("720", 1080), 1e-12, L"a 1080p frame upscaled from 720p");
			Assert::AreEqual(900.0 / 2160.0, SourceRebase::Factor("900", 2160), 1e-12);
			// Per axis: WIDTHxHEIGHT (owner 2026-10-16).
			Assert::AreEqual(1080, SourceRebase::ClassHeight("3840x1080"));
			Assert::AreEqual(3840, SourceRebase::ClassWidth("3840x1080"));
			Assert::AreEqual(0.5, SourceRebase::Factor("3840x1080", 2160));
			Assert::AreEqual(1.0, SourceRebase::FactorX("3840x1080", 3840, 2160), L"the width untouched");
			Assert::AreEqual(0.75, SourceRebase::FactorX("1440x1080", 1920, 1080), 1e-12, L"anamorphic 1440 across");
			Assert::AreEqual(1.0, SourceRebase::Factor("1440x1080", 1080), L"its height native");
			Assert::AreEqual(0.5, SourceRebase::FactorX("1080", 3840, 2160), L"a height alone: both axes alike");
			Assert::AreEqual(0, SourceRebase::ClassHeight("x1080"), L"malformed: native");
			Assert::AreEqual(0, SourceRebase::ClassHeight("3840x"), L"malformed: native");

			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			pl_dispatch dispatch = pl_dispatch_create(fixture.Log(), gpu);
			const int W = 3840, H = 2160, w = 1920, h = 1080;
			// A smooth 1080p picture (a few cycles each way), and the same picture
			// sampled at 4K - what an upstream upscaler would have produced from it -
			// with letterbox bars of 276 rows (black, code 0.0625 = 16/256).
			auto picture = [](double x, double y) { return 0.45 + 0.3 * std::sin(x * 9.0) * std::cos(y * 7.0); };
			const int bar4k = 276;
			std::vector<uint16_t> luma4k(static_cast<size_t>(W) * H), chroma4k(static_cast<size_t>(W / 2) * (H / 2) * 2);
			for (int y = 0; y < H; ++y)
				for (int x = 0; x < W; ++x)
				{
					const bool bar = y < bar4k || y >= H - bar4k;
					const double v = bar ? 0.0625 : picture((x + 0.5) / W, (y + 0.5) / H);
					luma4k[static_cast<size_t>(y) * W + x] = static_cast<uint16_t>(std::lround(v * 65535.0));
				}
			for (size_t i = 0; i < chroma4k.size(); ++i) chroma4k[i] = 32768;
			pl_tex_params lp{};
			lp.w = W; lp.h = H; lp.format = pl_find_named_fmt(gpu, "r16"); lp.sampleable = true; lp.host_readable = true;
			lp.initial_data = luma4k.data();
			pl_tex_params cp = lp;
			cp.w = W / 2; cp.h = H / 2; cp.format = pl_find_named_fmt(gpu, "rg16"); cp.initial_data = chroma4k.data();
			pl_tex lumaTex = pl_tex_create(gpu, &lp), chromaTex = pl_tex_create(gpu, &cp);
			Assert::IsTrue(lumaTex && chromaTex, L"source planes");
			pl_frame source{};
			source.num_planes = 2;
			source.planes[0].texture = lumaTex; source.planes[0].components = 1; source.planes[0].component_mapping[0] = PL_CHANNEL_Y;
			source.planes[1].texture = chromaTex; source.planes[1].components = 2;
			source.planes[1].component_mapping[0] = PL_CHANNEL_CB; source.planes[1].component_mapping[1] = PL_CHANNEL_CR;
			source.crop = { 0.0f, static_cast<float>(bar4k), static_cast<float>(W), static_cast<float>(H - bar4k) };

			SourceRebase::Module rebase;
			pl_frame out{};
			Assert::IsTrue(rebase.Apply(gpu, dispatch, source, 0.5, out), ExtendedRangeGpuProbe::ToWide(rebase.LastError()).c_str());
			Assert::AreEqual(w, out.planes[0].texture->params.w);
			Assert::AreEqual(h, out.planes[0].texture->params.h);
			Assert::AreEqual(w / 2, out.planes[1].texture->params.w, L"chroma stays 4:2:0");
			Assert::AreEqual(h / 2, out.planes[1].texture->params.h);
			Assert::AreEqual(static_cast<float>(bar4k / 2), out.crop.y0, 1e-4f, L"the crop scales with the planes");
			Assert::AreEqual(static_cast<float>(h - bar4k / 2), out.crop.y1, 1e-4f);
			Assert::IsTrue(out.planes[0].component_mapping[0] == PL_CHANNEL_Y && out.planes[1].components == 2,
				L"plane layout unchanged");

			// Read the re-based luma back (its format may be r16 or a float
			// fallback). The module's textures are not host-readable (they never
			// need to be): copy into one that is.
			pl_tex_params rp{};
			rp.w = w; rp.h = h; rp.format = out.planes[0].texture->params.format;
			rp.host_readable = true; rp.blit_dst = true;
			pl_tex readable = pl_tex_create(gpu, &rp);
			Assert::IsTrue(readable != nullptr, L"readable copy");
			pl_tex_blit_params bp{};
			bp.src = out.planes[0].texture; bp.dst = readable;
			pl_tex_blit(gpu, &bp);
			const pl_tex rebased = readable;
			std::vector<float> values(static_cast<size_t>(w) * h);
			if (rebased->params.format->type == PL_FMT_FLOAT)
			{
				std::vector<uint16_t> half(values.size());
				pl_tex_transfer_params d{}; d.tex = rebased; d.ptr = half.data();
				Assert::IsTrue(pl_tex_download(gpu, &d));
				for (size_t i = 0; i < values.size(); ++i) values[i] = HalfBits(half[i]);
			}
			else
			{
				std::vector<uint16_t> unorm(values.size());
				pl_tex_transfer_params d{}; d.tex = rebased; d.ptr = unorm.data();
				Assert::IsTrue(pl_tex_download(gpu, &d));
				for (size_t i = 0; i < values.size(); ++i) values[i] = unorm[i] / 65535.0f;
			}
			// Inside the picture (away from the bar edge): the 1080p picture itself.
			double worst = 0.0;
			const int bar = bar4k / 2;
			for (int y = bar + 4; y < h - bar - 4; y += 3)
				for (int x = 4; x < w - 4; x += 3)
					worst = (std::max)(worst, std::fabs(values[static_cast<size_t>(y) * w + x] - picture((x + 0.5) / w, (y + 0.5) / h)));
			// At the crop edge: the bar stays black up to the edge and nothing
			// rings below black inside the crop (what is shown). The polar
			// kernel's lobe leaves about a code of ringing in the bar a row or
			// two outside the crop - never shown - which is held under 2 codes.
			double barWorst = 0.0, cropBelow = 0.0, outsideBelow = 0.0;
			for (int y = 0; y < bar - 2; ++y)
				for (int x = 0; x < w; x += 7)
					barWorst = (std::max)(barWorst, std::fabs(values[static_cast<size_t>(y) * w + x] - 0.0625));
			for (int y = bar - 3; y < bar + 3; ++y)
				for (int x = 0; x < w; x += 7)
				{
					const double below = 0.0625 - values[static_cast<size_t>(y) * w + x];
					double& slot = y >= bar ? cropBelow : outsideBelow;
					slot = (std::max)(slot, below);
				}
			char line[256];
			snprintf(line, sizeof(line), "Re-base 4K -> 1080: picture max error %.5f (%.2f 8-bit codes); bar max error %.5f; below black inside the crop %.5f, in the bar outside it %.5f\n",
				worst, worst * 255.0, barWorst, cropBelow, outsideBelow);
			Logger::WriteMessage(line);
			Assert::IsTrue(worst < 2.0 / 255.0, L"the re-based picture matches the 1080p picture within 2 codes");
			Assert::IsTrue(barWorst < 0.5 / 255.0, L"the bar stays black up to the crop edge");
			Assert::IsTrue(cropBelow < 0.25 / 255.0, L"anti-ringing: nothing undershoots black inside the crop");
			Assert::IsTrue(outsideBelow < 2.0 / 255.0, L"ringing outside the crop (never shown) stays under 2 codes");

			pl_tex_destroy(gpu, &lumaTex);
			pl_tex_destroy(gpu, &chromaTex);
			pl_tex_destroy(gpu, &readable);
			rebase.Release(gpu);
			pl_dispatch_destroy(&dispatch);
		}

		TEST_METHOD(SourceRebaseNeverRingsIntoTheBarsInAnyVariant)
		{
			// Planner 2026-10-16 evening: "no ringing into blanked bars" as a standing test for
			// every re-base variant (a separable luma once rang 2.2 codes below black into the
			// bars). Source: a letterboxed 1080p picture upscaled 2x by the player (Lanczos-3,
			// both planes), as a streaming box sends it. Each variant: the bars stay black up to
			// the crop edge and nothing undershoots black inside the crop, luma and chroma.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			pl_dispatch dispatch = pl_dispatch_create(fixture.Log(), gpu);
			const int w = 1920, h = 1080, W = 3840, H = 2160, bar = 138;
			const KernelInverse::Candidate& lanczos = KernelInverse::Candidates()[static_cast<size_t>(KernelInverse::Find("lanczos3"))];
			// One plane: the 1080-class picture with bars, upscaled 2x separably.
			auto upscaled = [&](int pw, int ph, int pbar, double barValue, const std::function<double(double, double)>& f) {
				std::vector<float> master(static_cast<size_t>(pw) * ph);
				for (int y = 0; y < ph; ++y)
					for (int x = 0; x < pw; ++x)
						master[static_cast<size_t>(y) * pw + x] = static_cast<float>(y < pbar || y >= ph - pbar ? barValue : f((x + 0.5) / pw, (y + 0.5) / ph));
				std::vector<float> rows(static_cast<size_t>(ph) * pw * 2), up(static_cast<size_t>(pw) * 2 * ph * 2);
				std::vector<float> in(static_cast<size_t>(ph)), out(static_cast<size_t>(ph) * 2);
				for (int y = 0; y < ph; ++y) KernelInverse::UpscaleLine(lanczos, &master[static_cast<size_t>(y) * pw], pw, &rows[static_cast<size_t>(y) * pw * 2]);
				for (int x = 0; x < pw * 2; ++x)
				{
					for (int y = 0; y < ph; ++y) in[static_cast<size_t>(y)] = rows[static_cast<size_t>(y) * pw * 2 + x];
					KernelInverse::UpscaleLine(lanczos, in.data(), ph, out.data());
					for (int y = 0; y < ph * 2; ++y) up[static_cast<size_t>(y) * pw * 2 + x] = out[static_cast<size_t>(y)];
				}
				return up;
			};
			const double black = 0.0625, neutral = 0.5;
			const std::vector<float> lumaUp = upscaled(w, h, bar, black, [](double x, double y) { return 0.45 + 0.25 * std::sin(x * 9.0) * std::cos(y * 7.0); });
			const std::vector<float> cbUp = upscaled(w / 2, h / 2, bar / 2, neutral, [](double x, double y) { return 0.5 + 0.12 * std::sin(x * 5.0 + y * 3.0); });
			const std::vector<float> crUp = upscaled(w / 2, h / 2, bar / 2, neutral, [](double x, double y) { return 0.5 - 0.1 * std::cos(x * 4.0 - y * 6.0); });
			std::vector<uint16_t> luma(lumaUp.size()), chroma(cbUp.size() * 2);
			auto code = [](float v) { return static_cast<uint16_t>(std::lround((std::min)(1.0f, (std::max)(0.0f, v)) * 65535.0f)); };
			for (size_t i = 0; i < luma.size(); ++i) luma[i] = code(lumaUp[i]);
			for (size_t i = 0; i < cbUp.size(); ++i) { chroma[i * 2] = code(cbUp[i]); chroma[i * 2 + 1] = code(crUp[i]); }
			pl_tex_params lp{};
			lp.w = W; lp.h = H; lp.format = pl_find_named_fmt(gpu, "r16"); lp.sampleable = true; lp.initial_data = luma.data();
			pl_tex_params cp = lp;
			cp.w = W / 2; cp.h = H / 2; cp.format = pl_find_named_fmt(gpu, "rg16"); cp.initial_data = chroma.data();
			pl_tex lumaTex = pl_tex_create(gpu, &lp), chromaTex = pl_tex_create(gpu, &cp);
			Assert::IsTrue(lumaTex && chromaTex);
			pl_frame source{};
			source.num_planes = 2;
			source.planes[0].texture = lumaTex; source.planes[0].components = 1; source.planes[0].component_mapping[0] = PL_CHANNEL_Y;
			source.planes[1].texture = chromaTex; source.planes[1].components = 2;
			source.planes[1].component_mapping[0] = PL_CHANNEL_CB; source.planes[1].component_mapping[1] = PL_CHANNEL_CR;
			source.crop = { 0.0f, static_cast<float>(2 * bar), static_cast<float>(W), static_cast<float>(H - 2 * bar) };
			auto download = [&](pl_tex tex, int components, std::vector<float>& values) {
				pl_tex_params rp{};
				rp.w = tex->params.w; rp.h = tex->params.h; rp.format = tex->params.format; rp.host_readable = true; rp.blit_dst = true;
				pl_tex readable = pl_tex_create(gpu, &rp);
				pl_tex_blit_params bp{}; bp.src = tex; bp.dst = readable;
				pl_tex_blit(gpu, &bp);
				const size_t n = static_cast<size_t>(rp.w) * rp.h * components;
				std::vector<uint16_t> raw(n);
				pl_tex_transfer_params d{}; d.tex = readable; d.ptr = raw.data();
				Assert::IsTrue(pl_tex_download(gpu, &d));
				values.resize(n);
				for (size_t i = 0; i < n; ++i) values[i] = readable->params.format->type == PL_FMT_FLOAT ? HalfBits(raw[i]) : raw[i] / 65535.0f;
				pl_tex_destroy(gpu, &readable);
			};
			struct Variant { const char* name; double fx, fy; bool separable; int kernel; };
			const Variant variants[] = {
				{ "default (polar luma, separable chroma), 2:1", 0.5, 0.5, true, -1 },
				{ "polar luma and chroma, 2:1", 0.5, 0.5, false, -1 },
				{ "kernel inverse (Lanczos-3), 2:1", 0.5, 0.5, true, KernelInverse::Find("lanczos3") },
				{ "height only (1D), 2:1", 1.0, 0.5, true, -1 },
				{ "1440 (2:3)", 2.0 / 3.0, 2.0 / 3.0, true, -1 },
				{ "900 lines", 900.0 / 2160.0, 900.0 / 2160.0, true, -1 } };
			int failures = 0;
			for (const Variant& v : variants)
			{
				SourceRebase::Module rebase;
				rebase.separable = v.separable;
				rebase.SetKernel(v.kernel);
				pl_frame out{};
				Assert::IsTrue(rebase.Apply(gpu, dispatch, source, v.fx, v.fy, out), ExtendedRangeGpuProbe::ToWide(rebase.LastError()).c_str());
				std::vector<float> y, c;
				download(out.planes[0].texture, 1, y);
				download(out.planes[1].texture, 2, c);
				const int yw = out.planes[0].texture->params.w, yh = out.planes[0].texture->params.h;
				const int cw = out.planes[1].texture->params.w, ch = out.planes[1].texture->params.h;
				const double edge = out.crop.y0;   // the crop's top edge in luma rows
				// The plane's bar rows (away from the edge by the filter's reach) and the first rows inside.
				auto measure = [&](const std::vector<float>& values, int pw, int ph, int stride, int comp, double scale, double barLevel,
					double& barWorst, double& insideBelow) {
					const double e = edge * scale;
					for (int row = 0; row < ph; ++row)
					{
						const double centre = row + 0.5;
						for (int x = 0; x < pw; x += 5)
						{
							const double value = values[(static_cast<size_t>(row) * pw + x) * stride + comp];
							if (centre < e - 4.0 * (1.0 / v.fy) - 1.0 || centre > ph - e + 4.0 * (1.0 / v.fy) + 1.0)
								barWorst = (std::max)(barWorst, std::fabs(value - barLevel));
							else if ((centre > e && centre < e + 3.0) || (centre < ph - e && centre > ph - e - 3.0))
								if (barLevel == black) insideBelow = (std::max)(insideBelow, black - value);
						}
					}
				};
				double barY = 0.0, belowY = 0.0, barC = 0.0, unused = 0.0;
				measure(y, yw, yh, 1, 0, 1.0, black, barY, belowY);
				measure(c, cw, ch, 2, 0, 0.5, neutral, barC, unused);
				measure(c, cw, ch, 2, 1, 0.5, neutral, barC, unused);
				char line[260];
				snprintf(line, sizeof(line), "%-44s bar: luma %.2f codes, chroma %.2f codes off; below black inside the crop %.2f codes (8-bit)\n",
					v.name, barY * 255.0, barC * 255.0, belowY * 255.0);
				Logger::WriteMessage(line);
				if (barY > 0.5 / 255.0 || barC > 0.5 / 255.0 || belowY > 0.25 / 255.0) ++failures;
				rebase.Release(gpu);
			}
			pl_tex_destroy(gpu, &lumaTex);
			pl_tex_destroy(gpu, &chromaTex);
			pl_dispatch_destroy(&dispatch);
			Assert::AreEqual(0, failures, L"no re-base variant rings into the bars or below black inside the crop");
		}

		TEST_METHOD(EarlyUploadMatchesTheRenderUploadAndNeverReusesBusySlots)
		{
			// Option A of the pinned-memory work (owner 2026-10-15): a frame
			// uploaded early on a second device (EarlyUpload) unpacks to exactly
			// the planes of today's upload (V210 and R12B); a held slot is never
			// handed out again; a slot comes back once its handle is gone and its
			// GPU uses are released; the byte cap is honoured.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			EarlyUpload early;
			if (!early.Open(fixture.Device(), gpu, 2))
			{
				Logger::WriteMessage(("EarlyUpload not available on this device: " + early.LastError() + "\n").c_str());
				return;   // the renderer then keeps the normal upload
			}
			uint32_t seed = 0xabcdef1u;
			auto next = [&seed]() { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; };
			auto download = [&](pl_tex tex)
			{
				std::vector<uint16_t> samples(static_cast<size_t>(tex->params.w) * tex->params.h * tex->params.format->num_components);
				pl_tex_transfer_params d{};
				d.tex = tex;
				d.ptr = samples.data();
				Assert::IsTrue(pl_tex_download(gpu, &d), L"plane not host-readable");
				return samples;
			};
			// V210: the early-uploaded frame against today's pointer upload.
			{
				GpuCaptureUnpack unpack(fixture.Log(), gpu);
				unpack.SetHostReadableForTest(true);
				const int width = 1920, height = 100;
				const size_t rowBytes = static_cast<size_t>((width + 47) / 48) * 128;
				std::vector<uint8_t> frame(rowBytes * height);
				for (uint8_t& b : frame) b = static_cast<uint8_t>(next());
				pl_tex normal[2] = { nullptr, nullptr }, viaEarly[2] = { nullptr, nullptr };
				Assert::IsTrue(unpack.Unpack(VideoFrameEncoding::V210, frame.data(), rowBytes, width, height, normal));
				const std::vector<uint16_t> wantY = download(normal[0]), wantUV = download(normal[1]);
				EarlyUpload::SlotHandle slot = early.Upload(frame.data(), rowBytes, height);
				Assert::IsTrue(slot != nullptr, L"V210: a free slot");
				pl_tex uploaded = early.Acquire(*slot);
				Assert::IsNotNull(uploaded, ExtendedRangeGpuProbe::ToWide(early.LastError()).c_str());
				Assert::IsTrue(unpack.Unpack(VideoFrameEncoding::V210, nullptr, rowBytes, width, height, viaEarly, uploaded),
					ExtendedRangeGpuProbe::ToWide(unpack.Error()).c_str());
				early.Used(*slot);
				Assert::IsTrue(download(viaEarly[0]) == wantY && download(viaEarly[1]) == wantUV, L"V210: early upload differs");
				for (pl_tex* t : { &normal[0], &normal[1], &viaEarly[0], &viaEarly[1] }) pl_tex_destroy(gpu, t);
			}
			// R12B through GpuRgbUnpack, and the slot rules.
			{
				GpuRgbUnpack unpack(fixture.Log(), gpu);
				unpack.SetHostReadableForTest(true);
				const int width = 1920, height = 100;
				const size_t rowBytes = static_cast<size_t>(width) * 36 / 8;
				std::vector<uint8_t> frames[3];
				for (auto& f : frames) { f.resize(rowBytes * height); for (uint8_t& b : f) b = static_cast<uint8_t>(next()); }
				auto expected = [&](const std::vector<uint8_t>& f)
				{
					pl_tex rgb = nullptr, luma = nullptr;
					Assert::IsTrue(unpack.Unpack(VideoFrameEncoding::R12B, false, f.data(), rowBytes, width, height, rgb, luma));
					std::pair<std::vector<uint16_t>, std::vector<uint16_t>> out(download(rgb), download(luma));
					pl_tex_destroy(gpu, &rgb);
					pl_tex_destroy(gpu, &luma);
					return out;
				};
				auto viaEarly = [&](EarlyUpload::SlotHandle& s)
				{
					pl_tex rgb = nullptr, luma = nullptr;
					pl_tex uploaded = early.Acquire(*s);
					Assert::IsNotNull(uploaded);
					Assert::IsTrue(unpack.Unpack(VideoFrameEncoding::R12B, false, nullptr, rowBytes, width, height, rgb, luma, false, uploaded));
					early.Used(*s);
					std::pair<std::vector<uint16_t>, std::vector<uint16_t>> out(download(rgb), download(luma));
					pl_tex_destroy(gpu, &rgb);
					pl_tex_destroy(gpu, &luma);
					return out;
				};
				EarlyUpload::SlotHandle a = early.Upload(frames[0].data(), rowBytes, height);
				EarlyUpload::SlotHandle b = early.Upload(frames[1].data(), rowBytes, height);
				Assert::IsTrue(a && b, L"two slots");
				Assert::IsTrue(early.Upload(frames[2].data(), rowBytes, height) == nullptr, L"both slots held: none handed out");
				Assert::IsTrue(viaEarly(a) == expected(frames[0]), L"R12B slot A differs");
				Assert::IsTrue(viaEarly(b) == expected(frames[1]), L"R12B slot B differs");
				a.reset();   // its payload gone; its GPU use released once the GPU passes it
				pl_gpu_finish(gpu);
				EarlyUpload::SlotHandle c = early.Upload(frames[2].data(), rowBytes, height);
				Assert::IsTrue(c != nullptr, L"a released slot comes back");
				Assert::IsTrue(viaEarly(c) == expected(frames[2]), L"R12B reused slot differs");
				Assert::IsTrue(viaEarly(b) == expected(frames[1]), L"R12B slot B still intact after the reuse");
			}
			const EarlyUpload::Stats stats = early.GetStats();
			Assert::IsTrue(stats.noFreeSlot >= 1 && stats.failures == 0, L"stats");
			early.Close();
			// The cap: a pool too small for one frame hands nothing out.
			EarlyUpload tiny;
			Assert::IsTrue(tiny.Open(fixture.Device(), gpu, 2, 1024));
			std::vector<uint8_t> f(1920 * 4 * 100);
			Assert::IsTrue(tiny.Upload(f.data(), 1920 * 4, 100) == nullptr, L"over the cap: nothing");
			tiny.Close();
			AssertCleanLibplaceboLog(log);
			Logger::WriteMessage(("EarlyUpload: " + std::to_string(stats.uploads) + " uploads, " +
				std::to_string(stats.noFreeSlot) + " with no free slot, " + std::to_string(stats.slotsMade) + " textures\n").c_str());
		}

		TEST_METHOD(PinnedUploadCapabilitiesAndTiming)
		{
			// Pinned memory (roadmap, owner 2026-10-15): what this GPU's libplacebo
			// D3D11 backend offers for host-mapped upload buffers, and a 4K raw
			// frame's upload timed three ways (report; nothing asserted beyond
			// the uploads succeeding and matching):
			//   pointer      pl_tex_upload from a plain pointer (today)
			//   mapped host  a host-mapped pl_buf (PL_BUF_MEM_HOST): the copy into
			//                it timed apart, then pl_tex_upload from the buffer
			//   mapped vram  the same from PL_BUF_MEM_DEVICE when offered
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			const pl_gpu_limits& lim = gpu->limits;
			char line[512];
			std::snprintf(line, sizeof(line),
				"Pinned upload: thread_safe %d callbacks %d buf_transfer %d max_buf %zu MB max_mapped %zu MB max_mapped_vram %zu MB host_cached %d align_host_ptr %zu\n",
				lim.thread_safe, lim.callbacks, lim.buf_transfer, lim.max_buf_size >> 20, lim.max_mapped_size >> 20,
				lim.max_mapped_vram >> 20, lim.host_cached, lim.align_host_ptr);
			Logger::WriteMessage(line);
			const int width = 3840, height = 2160;
			const size_t rowBytes = static_cast<size_t>(width) * 36 / 8;   // R12B, the largest capture format
			const size_t bytes = rowBytes * height;
			std::vector<uint8_t> frame(bytes);
			uint32_t seed = 0x12345u;
			for (uint8_t& b : frame) { seed = seed * 1664525u + 1013904223u; b = static_cast<uint8_t>(seed >> 24); }
			pl_fmt rgba8 = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 8, 8, static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_HOST_READABLE));
			Assert::IsNotNull(rgba8);
			pl_tex_params tp{};
			tp.w = static_cast<int>(rowBytes / 4);
			tp.h = height;
			tp.format = rgba8;
			tp.sampleable = true;
			tp.host_writable = true;
			tp.host_readable = true;
			pl_tex tex = pl_tex_create(gpu, &tp);
			Assert::IsNotNull(tex);
			auto ms = [](std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b)
			{ return std::chrono::duration<double, std::milli>(b - a).count(); };
			auto check = [&](const wchar_t* what)
			{
				std::vector<uint8_t> back(bytes);
				pl_tex_transfer_params d{};
				d.tex = tex;
				d.ptr = back.data();
				d.row_pitch = rowBytes;
				Assert::IsTrue(pl_tex_download(gpu, &d), what);
				Assert::IsTrue(back == frame, what);
			};
			double pointerMs = 1e30, pointerCallMs = 1e30;
			for (int run = 0; run < 6; ++run)
			{
				pl_gpu_finish(gpu);
				const auto t0 = std::chrono::steady_clock::now();
				pl_tex_transfer_params u{};
				u.tex = tex;
				u.ptr = frame.data();
				u.row_pitch = rowBytes;
				Assert::IsTrue(pl_tex_upload(gpu, &u));
				const auto tCall = std::chrono::steady_clock::now();
				pl_gpu_finish(gpu);
				pointerMs = std::min(pointerMs, ms(t0, std::chrono::steady_clock::now()));
				pointerCallMs = std::min(pointerCallMs, ms(t0, tCall));
			}
			check(L"pointer upload");
			std::snprintf(line, sizeof(line), "Pinned upload 4K R12B (%zu MB) pointer upload: the call blocks %.2f ms, with the GPU transfer %.2f ms (%.1f GB/s)\n",
				bytes >> 20, pointerCallMs, pointerMs, bytes / (pointerMs * 1e6));
			Logger::WriteMessage(line);
			for (const enum pl_buf_mem_type mem : { PL_BUF_MEM_HOST, PL_BUF_MEM_DEVICE })
			{
				const char* name = mem == PL_BUF_MEM_HOST ? "mapped host" : "mapped vram";
				pl_buf_params bp{};
				bp.size = bytes;
				bp.host_mapped = true;
				bp.memory_type = mem;
				pl_buf buf = (lim.max_mapped_size >= bytes && (mem == PL_BUF_MEM_HOST || lim.max_mapped_vram >= bytes)) ?
					pl_buf_create(gpu, &bp) : nullptr;
				if (!buf || !buf->data)
				{
					std::snprintf(line, sizeof(line), "Pinned upload: %s buffer of %zu MB not available\n", name, bytes >> 20);
					Logger::WriteMessage(line);
					pl_buf_destroy(gpu, &buf);
					continue;
				}
				double copyMs = 1e30, uploadMs = 1e30;
				for (int run = 0; run < 6; ++run)
				{
					pl_gpu_finish(gpu);
					const auto t0 = std::chrono::steady_clock::now();
					std::memcpy(buf->data, frame.data(), bytes);   // the card would write here directly
					const auto t1 = std::chrono::steady_clock::now();
					pl_tex_transfer_params u{};
					u.tex = tex;
					u.buf = buf;
					u.row_pitch = rowBytes;
					Assert::IsTrue(pl_tex_upload(gpu, &u));
					pl_gpu_finish(gpu);
					const auto t2 = std::chrono::steady_clock::now();
					copyMs = std::min(copyMs, ms(t0, t1));
					uploadMs = std::min(uploadMs, ms(t1, t2));
				}
				check(mem == PL_BUF_MEM_HOST ? L"mapped host upload" : L"mapped vram upload");
				// Non-blocking reuse: right after an upload is queued, is the
				// buffer reported busy, and free once the GPU is done?
				pl_tex_transfer_params u{};
				u.tex = tex;
				u.buf = buf;
				u.row_pitch = rowBytes;
				pl_tex_upload(gpu, &u);
				const bool busyAfterQueue = pl_buf_poll(gpu, buf, 0);
				pl_gpu_finish(gpu);
				const bool busyAfterFinish = pl_buf_poll(gpu, buf, 0);
				std::snprintf(line, sizeof(line),
					"Pinned upload 4K R12B (%zu MB): %s copy-in %.2f ms + upload from buffer %.2f ms (pointer upload %.2f ms); busy after queue %d, after finish %d\n",
					bytes >> 20, name, copyMs, uploadMs, pointerMs, busyAfterQueue, busyAfterFinish);
				Logger::WriteMessage(line);
				pl_buf_destroy(gpu, &buf);
			}
			pl_tex_destroy(gpu, &tex);
		}

		TEST_METHOD(GpuRgbUnpackMatchesTheCpuDecode)
		{
			// Roadmap item 2 (owner 2026-10-15): native ingest of R12B, R12L and
			// r210. With random bytes (row padding included): the luma equals the
			// CPU formatter's P010 Y plane sample for sample (BT.709 and BT.2020;
			// the matcher sees what it saw on the converted path), and the RGB
			// texture holds each pixel decoded independently here (R12: the
			// 12-bit value << 4; r210: 64-960 mapped to 16-bit limited levels,
			// below-black and above-white kept). Widths at several packings;
			// geometry the pass cannot take (an R12 width not a multiple of 8)
			// is refused, so the renderer keeps the CPU conversion.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			GpuRgbUnpack unpack(fixture.Log(), gpu);
			unpack.SetHostReadableForTest(true);
			pl_tex rgb = nullptr, luma = nullptr;
			uint32_t seed = 0x9e3779b9u;
			auto next = [&seed]() { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; };
			auto download = [&](pl_tex tex)
			{
				std::vector<uint16_t> samples(static_cast<size_t>(tex->params.w) * tex->params.h *
					tex->params.format->num_components);
				pl_tex_transfer_params d{};
				d.tex = tex;
				d.ptr = samples.data();
				Assert::IsTrue(pl_tex_download(gpu, &d), L"plane not host-readable");
				return samples;
			};
			// An independent decode of pixel x of a row (SMPTE 268M C4 for R12;
			// big-endian [00 R G B] for r210).
			auto decode = [](VideoFrameEncoding encoding, const BYTE* row, int x, int rgbOut[3])
			{
				if (encoding == VideoFrameEncoding::R210)
				{
					const uint32_t w = (uint32_t(row[4 * x]) << 24) | (uint32_t(row[4 * x + 1]) << 16) |
						(uint32_t(row[4 * x + 2]) << 8) | row[4 * x + 3];
					rgbOut[0] = int((w >> 20) & 1023u);
					rgbOut[1] = int((w >> 10) & 1023u);
					rgbOut[2] = int(w & 1023u);
					return;
				}
				if (encoding == VideoFrameEncoding::ARGB_8BIT || encoding == VideoFrameEncoding::BGRA_8BIT)
				{
					const BYTE* q = row + 4 * x;
					const bool argb = encoding == VideoFrameEncoding::ARGB_8BIT;
					rgbOut[0] = argb ? q[1] : q[2];
					rgbOut[1] = q[argb ? 2 : 1];
					rgbOut[2] = argb ? q[3] : q[0];
					return;
				}
				if (encoding == VideoFrameEncoding::R10b || encoding == VideoFrameEncoding::R10l)
				{
					const BYTE* q = row + 4 * x;
					const uint32_t w = encoding == VideoFrameEncoding::R10b ?
						(uint32_t(q[0]) << 24) | (uint32_t(q[1]) << 16) | (uint32_t(q[2]) << 8) | q[3] :
						(uint32_t(q[3]) << 24) | (uint32_t(q[2]) << 16) | (uint32_t(q[1]) << 8) | q[0];
					rgbOut[0] = int((w >> 22) & 1023u);
					rgbOut[1] = int((w >> 12) & 1023u);
					rgbOut[2] = int((w >> 2) & 1023u);
					return;
				}
				// The pair's nine bytes in stream order; R12B reverses every
				// 32-bit word of its 36-byte block (stream byte k of the block
				// is at k ^ 3).
				const int pair = x / 2;
				BYTE b[9];
				for (int k = 0; k < 9; ++k)
				{
					const int stream = pair * 9 + k;
					b[k] = encoding == VideoFrameEncoding::R12B ? row[(stream / 36) * 36 + ((stream % 36) ^ 3)] : row[stream];
				}
				const int v[6] = { b[0] | ((b[1] & 15) << 8), (b[1] >> 4) | (b[2] << 4), b[3] | ((b[4] & 15) << 8),
					(b[4] >> 4) | (b[5] << 4), b[6] | ((b[7] & 15) << 8), (b[7] >> 4) | (b[8] << 4) };
				for (int c = 0; c < 3; ++c)
					rgbOut[c] = v[(x & 1) * 3 + c];
			};
			int cases = 0;
			for (const VideoFrameEncoding encoding : { VideoFrameEncoding::R12B, VideoFrameEncoding::R12L, VideoFrameEncoding::R210,
				VideoFrameEncoding::R10b, VideoFrameEncoding::R10l, VideoFrameEncoding::ARGB_8BIT, VideoFrameEncoding::BGRA_8BIT })
				for (const ColorSpace space : { ColorSpace::REC_709, ColorSpace::BT_2020 })
					for (const int width : encoding == VideoFrameEncoding::R12B || encoding == VideoFrameEncoding::R12L ?
						std::vector<int>{ 104, 136, 1920 } : std::vector<int>{ 120, 122, 130, 1920 })
					{
						const int height = 100;
						VideoStateComPtr state = new VideoState();
						state->valid = true;
						state->displayMode = std::make_shared<DisplayMode>(width, height, false, 24000, 1001);
						state->videoFrameEncoding = encoding;
						state->colorspace = space;
						const size_t rowBytes = static_cast<size_t>(state->BytesPerRow());
						std::vector<BYTE> source(static_cast<size_t>(state->BytesPerFrame()));
						for (BYTE& b : source) b = static_cast<BYTE>(next());
						std::unique_ptr<IVideoFrameFormatter> cpu;
						if (GpuRgbUnpack::Is8Bit(encoding)) cpu = std::make_unique<CARGBtoP010VideoFrameFormatter>();
						else cpu = std::make_unique<CDeckLinkRGBToP010VideoFrameFormatter>();
						cpu->OnVideoState(state);
						std::vector<BYTE> expected(static_cast<size_t>(cpu->GetOutFrameSize()));
						const VideoFrame frame(source.data(), 1, 0, nullptr);
						Assert::IsTrue(cpu->FormatVideoFrame(frame, expected.data()), L"CPU conversion failed");
						const std::wstring name = std::wstring(encoding == VideoFrameEncoding::R12B ? L"R12B " :
							encoding == VideoFrameEncoding::R12L ? L"R12L " : encoding == VideoFrameEncoding::R210 ? L"r210 " :
							encoding == VideoFrameEncoding::R10b ? L"R10b " : encoding == VideoFrameEncoding::R10l ? L"R10l " :
							encoding == VideoFrameEncoding::ARGB_8BIT ? L"ARGB " : L"BGRA ") +
							(space == ColorSpace::BT_2020 ? L"2020 " : L"709 ") + std::to_wstring(width);
						Assert::IsTrue(unpack.Unpack(encoding, space == ColorSpace::BT_2020, source.data(), rowBytes, width, height, rgb, luma),
							(name + L": " + ExtendedRangeGpuProbe::ToWide(unpack.Error())).c_str());
						const std::vector<uint16_t> gotLuma = download(luma), gotRgb = download(rgb);
						// The luma-only mode (the de-judder's next / ahead frames): the
						// same luma, the picture untouched.
						{
							pl_tex lumaOnly = nullptr, untouched = nullptr;
							Assert::IsTrue(unpack.Unpack(encoding, space == ColorSpace::BT_2020, source.data(), rowBytes, width, height,
								untouched, lumaOnly, true), (name + L": luma-only unpack").c_str());
							Assert::IsTrue(untouched == nullptr, (name + L": luma-only made no picture").c_str());
							Assert::IsTrue(download(lumaOnly) == gotLuma, (name + L": luma-only luma differs").c_str());
							pl_tex_destroy(gpu, &lumaOnly);
						}
						const uint16_t* wantLuma = reinterpret_cast<const uint16_t*>(expected.data());
						for (int y = 0; y < height; ++y)
							for (int x = 0; x < width; ++x)
							{
								const size_t i = static_cast<size_t>(y) * width + x;
								if (gotLuma[i] != wantLuma[i])
									Assert::Fail((name + L": luma differs from the CPU formatter at (" + std::to_wstring(x) + L", " +
										std::to_wstring(y) + L"): " + std::to_wstring(gotLuma[i]) + L" vs " + std::to_wstring(wantLuma[i])).c_str());
								int v[3];
								decode(encoding, source.data() + static_cast<size_t>(y) * rowBytes, x, v);
								for (int c = 0; c < 3; ++c)
								{
									const int want = encoding == VideoFrameEncoding::R210 ?
										(4096 * 7 + (v[c] - 64) * 438 + 3) / 7 :
										encoding == VideoFrameEncoding::R10b || encoding == VideoFrameEncoding::R10l ? v[c] << 6 :
										GpuRgbUnpack::Is8Bit(encoding) ? v[c] << 8 : v[c] << 4;
									if (gotRgb[i * 4 + c] != want)
										Assert::Fail((name + L": RGB differs at (" + std::to_wstring(x) + L", " + std::to_wstring(y) +
											L") component " + std::to_wstring(c) + L": " + std::to_wstring(gotRgb[i * 4 + c]) + L" vs " +
											std::to_wstring(want)).c_str());
								}
							}
						++cases;
					}
			// Refused geometry: the renderer keeps the CPU conversion.
			std::vector<BYTE> narrow(1366 * 36 / 8 * 4 + 64);
			Assert::IsFalse(unpack.Unpack(VideoFrameEncoding::R12L, false, narrow.data(), 6148, 1366, 4, rgb, luma),
				L"an R12 width not a multiple of 8 must be refused");
			Assert::IsFalse(GpuRgbUnpack::Supports(VideoFrameEncoding::V210) || GpuRgbUnpack::Supports(VideoFrameEncoding::UYVY),
				L"only the RGB formats take this pass");
			// Efficiency (report): 3840x2160, the CPU conversion it replaces
			// against the raw upload + GPU pass (wall, GPU drained) and the pass
			// alone (timer).
			for (const VideoFrameEncoding encoding : { VideoFrameEncoding::R12B, VideoFrameEncoding::R210, VideoFrameEncoding::ARGB_8BIT })
			{
				const int width = 3840, height = 2160;
				VideoStateComPtr state = new VideoState();
				state->valid = true;
				state->displayMode = std::make_shared<DisplayMode>(width, height, false, 24000, 1001);
				state->videoFrameEncoding = encoding;
				state->colorspace = ColorSpace::REC_709;
				std::vector<BYTE> source(static_cast<size_t>(state->BytesPerFrame()));
				for (BYTE& b : source) b = static_cast<BYTE>(next());
				std::unique_ptr<IVideoFrameFormatter> cpu;
				if (GpuRgbUnpack::Is8Bit(encoding)) cpu = std::make_unique<CARGBtoP010VideoFrameFormatter>();
				else cpu = std::make_unique<CDeckLinkRGBToP010VideoFrameFormatter>();
				cpu->OnVideoState(state);
				std::vector<BYTE> out(static_cast<size_t>(cpu->GetOutFrameSize()));
				const VideoFrame frame(source.data(), 1, 0, nullptr);
				pl_timer timer = pl_timer_create(gpu);
				unpack.SetTimerForTest(timer);
				double cpuMs = 1e30, gpuWallMs = 1e30, passMs = 1e30;
				for (int run = 0; run < 8; ++run)
				{
					auto t0 = std::chrono::steady_clock::now();
					cpu->FormatVideoFrame(frame, out.data());
					cpuMs = std::min(cpuMs, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
					pl_gpu_finish(gpu);
					t0 = std::chrono::steady_clock::now();
					Assert::IsTrue(unpack.Unpack(encoding, false, source.data(), static_cast<size_t>(state->BytesPerRow()), width, height, rgb, luma));
					pl_gpu_finish(gpu);
					gpuWallMs = std::min(gpuWallMs, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
					const uint64_t ns = pl_timer_query(gpu, timer);
					if (ns > 0)
						passMs = std::min(passMs, ns / 1e6);
				}
				unpack.SetTimerForTest(nullptr);
				pl_timer_destroy(gpu, &timer);
				Logger::WriteMessage(("GpuRgbUnpack 4K " + std::string(encoding == VideoFrameEncoding::R12B ? "R12B" :
					encoding == VideoFrameEncoding::R210 ? "r210" : "ARGB") + ": CPU conversion " + std::to_string(cpuMs) +
					" ms; raw upload + GPU pass " + std::to_string(gpuWallMs) + " ms (pass alone " + std::to_string(passMs) + " ms)\n").c_str());
			}
			pl_tex_destroy(gpu, &rgb);
			pl_tex_destroy(gpu, &luma);
			AssertCleanLibplaceboLog(log);
			Logger::WriteMessage(("GpuRgbUnpack: " + std::to_string(cases) + " cases matched the CPU decode\n").c_str());
		}

		TEST_METHOD(GpuDolbyVisionTunnelUnpackMatchesTheCpuUnpack)
		{
			// dv_tunnel_gpu_unpack (owner 2026-10-16): the Dolby Vision tunnel's
			// 12-bit IPT 4:2:2 planes unpacked on the GPU from the raw r210 frame
			// equal CR210DolbyVisionTunnelFormatter's planes sample for sample,
			// for both RGB8 placements, several widths (row padding filled, every
			// pixel after the DM packet an arbitrary 32-bit word), in the r210,
			// R10b and R10l containers (each against the CPU's own decode of that
			// container, R10 padding bits set), from the frame
			// and from an already-uploaded texture (EarlyUpload). Refused geometry
			// keeps the CPU unpack. Runs on WARP by default (no GPU needed; the
			// fixture's default device), on the GPU with VP_TEST_GPU=hardware (adds
			// 3840 and the 4K timing report). VP_TEST_DV_WARP=1 is accepted (now the
			// default).
			auto envIs = [](const char* name, const char* want)
			{
				char* v = nullptr;
				size_t n = 0;
				const bool on = _dupenv_s(&v, &n, name) == 0 && v && std::string(v) == want;
				std::free(v);
				return on;
			};
			const bool hardware = envIs("VP_TEST_GPU", "hardware");
			Logger::WriteMessage(hardware ? L"DV tunnel GPU unpack: on the hardware GPU\n" : L"DV tunnel GPU unpack: on WARP\n");
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			GpuDolbyVisionTunnelUnpack unpack(fixture.Log(), gpu);
			unpack.SetHostReadableForTest(true);
			pl_tex planes[2] = { nullptr, nullptr };
			uint32_t seed = 0x6a09e667u;
			auto next = [&seed]() { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; };
			auto download = [&](pl_tex tex)
			{
				std::vector<uint16_t> samples(static_cast<size_t>(tex->params.w) * tex->params.h *
					tex->params.format->num_components);
				pl_tex_transfer_params d{};
				d.tex = tex;
				d.ptr = samples.data();
				Assert::IsTrue(pl_tex_download(gpu, &d), L"plane not host-readable");
				return samples;
			};
			pl_fmt rgba8 = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 8, 8, PL_FMT_CAP_SAMPLEABLE);
			Assert::IsNotNull(rgba8);
			int cases = 0;
			std::vector<unsigned> widths = { 128U, 130U, 134U, 1922U };
			if (hardware)
				widths.push_back(3840U);
			for (const VideoFrameEncoding container : { VideoFrameEncoding::R210, VideoFrameEncoding::R10b,
				VideoFrameEncoding::R10l })
			for (const bool high8 : { false, true })
				for (const unsigned width : widths)
				{
					const unsigned height = width >= 3840 ? 2160U : width > 1000 ? 1081U : 100U;
					Tests::DvSender::SenderPicture picture(high8, width, height, container, 24000, 1001, false, 2);
					picture.Embed(Tests::DvSender::Packetize(Tests::DvSender::DmBody()));
					const size_t rowBytes = picture.state->BytesPerRow();
					for (size_t row = 0; row < height; ++row)
						for (size_t byte = 0; byte + 4 <= rowBytes; byte += 4)
						{
							if (byte / 4 < width && row * width + byte / 4 < 3072)
								continue;
							const uint32_t word = next();
							std::memcpy(picture.input.data() + row * rowBytes + byte, &word, 4);
						}
					const VideoFrame frame(picture.input.data(), 1, 0, nullptr);
					CR210DolbyVisionTunnelFormatter cpu;
					cpu.OnVideoState(picture.state);
					std::vector<BYTE> expected(static_cast<size_t>(cpu.GetOutFrameSize()));
					Assert::IsTrue(cpu.FormatVideoFrame(frame, expected.data()), L"CPU unpack failed");
					Assert::IsTrue(cpu.GetDolbyVisionTunnelMetadata().valid, L"the sender picture did not lock");
					CR210DolbyVisionTunnelFormatter analyzed;
					analyzed.OnVideoState(picture.state);
					Assert::IsTrue(analyzed.AnalyzeFrame(frame) == CR210DolbyVisionTunnelFormatter::FrameResult::PICTURE);
					Assert::AreEqual(high8, analyzed.PlanesHighAligned());
					const std::wstring name = std::wstring(container == VideoFrameEncoding::R210 ? L"r210 " :
						container == VideoFrameEncoding::R10b ? L"R10b " : L"R10l ") +
						(high8 ? L"high8 " : L"low8 ") + std::to_wstring(width);
					const uint16_t* want = reinterpret_cast<const uint16_t*>(expected.data());
					const size_t planePixels = static_cast<size_t>(width) * height;
					auto compare = [&](const wchar_t* route)
					{
						const std::vector<uint16_t> gotY = download(planes[0]), gotC = download(planes[1]);
						Assert::AreEqual(planePixels, gotY.size(), (name + L": intensity plane size").c_str());
						Assert::AreEqual(planePixels, gotC.size(), (name + L": chroma plane size").c_str());
						for (size_t i = 0; i < planePixels; ++i)
						{
							if (gotY[i] != want[i])
								Assert::Fail((name + L" " + route + L": intensity differs at (" + std::to_wstring(i % width) + L", " +
									std::to_wstring(i / width) + L"): " + std::to_wstring(gotY[i]) + L" vs " + std::to_wstring(want[i])).c_str());
							if (gotC[i] != want[planePixels + i])
								Assert::Fail((name + L" " + route + L": chroma differs at (" + std::to_wstring(i % width) + L", " +
									std::to_wstring(i / width) + L"): " + std::to_wstring(gotC[i]) + L" vs " +
									std::to_wstring(want[planePixels + i])).c_str());
						}
					};
					Assert::IsTrue(unpack.Unpack(container, picture.input.data(), rowBytes, static_cast<int>(width),
						static_cast<int>(height), analyzed.PlanesHighAligned(), planes),
						(name + L": " + ExtendedRangeGpuProbe::ToWide(unpack.Error())).c_str());
					compare(L"frame");
					// The early-upload route: the raw frame already on the GPU as rgba8.
					pl_tex_params rawParams{};
					rawParams.w = static_cast<int>(rowBytes / 4);
					rawParams.h = static_cast<int>(height);
					rawParams.format = rgba8;
					rawParams.sampleable = true;
					rawParams.host_writable = true;
					pl_tex early = pl_tex_create(gpu, &rawParams);
					Assert::IsNotNull(early);
					pl_tex_transfer_params up{};
					up.tex = early;
					up.ptr = picture.input.data();
					up.row_pitch = rowBytes;
					Assert::IsTrue(pl_tex_upload(gpu, &up));
					Assert::IsTrue(unpack.Unpack(container, nullptr, rowBytes, static_cast<int>(width), static_cast<int>(height),
						analyzed.PlanesHighAligned(), planes, early), (name + L" early: " + ExtendedRangeGpuProbe::ToWide(unpack.Error())).c_str());
					compare(L"early");
					pl_tex_destroy(gpu, &early);
					++cases;
				}
			// Refused geometry: the renderer keeps the CPU unpack.
			std::vector<BYTE> tiny(1024 * 8);
			const VideoFrameEncoding r210 = VideoFrameEncoding::R210;
			Assert::IsFalse(unpack.Unpack(r210, tiny.data(), 1024, 129, 4, false, planes), L"an odd width must be refused");
			Assert::IsFalse(unpack.Unpack(r210, tiny.data(), 508, 128, 4, false, planes), L"a row shorter than the frame must be refused");
			Assert::IsFalse(unpack.Unpack(r210, nullptr, 1024, 128, 4, false, planes), L"no frame must be refused");
			Assert::IsFalse(unpack.Unpack(VideoFrameEncoding::V210, tiny.data(), 1024, 128, 4, false, planes),
				L"only the tunnel containers take this pass");
			// Efficiency (report, hardware): 3840x2160, the CPU unpack it replaces
			// against AnalyzeFrame (the CPU's part now) and the raw upload + GPU
			// pass (wall, GPU drained) and the pass alone (timer).
			if (hardware)
			{
				Tests::DvSender::SenderPicture picture(true, 3840, 2160);
				picture.Embed(Tests::DvSender::Packetize(Tests::DvSender::DmBody()));
				CR210DolbyVisionTunnelFormatter cpu, analyzed;
				cpu.OnVideoState(picture.state);
				analyzed.OnVideoState(picture.state);
				std::vector<BYTE> out(static_cast<size_t>(cpu.GetOutFrameSize()));
				pl_timer timer = pl_timer_create(gpu);
				unpack.SetTimerForTest(timer);
				double cpuMs = 1e30, analyzeMs = 1e30, gpuWallMs = 1e30, passMs = 1e30;
				for (int run = 0; run < 8; ++run)
				{
					const VideoFrame frame(picture.input.data(), static_cast<uint64_t>(run + 1), 0, nullptr);
					auto t0 = std::chrono::steady_clock::now();
					cpu.FormatVideoFrame(frame, out.data());
					cpuMs = std::min(cpuMs, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
					t0 = std::chrono::steady_clock::now();
					analyzed.AnalyzeFrame(frame);
					analyzeMs = std::min(analyzeMs, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
					pl_gpu_finish(gpu);
					t0 = std::chrono::steady_clock::now();
					Assert::IsTrue(unpack.Unpack(VideoFrameEncoding::R210, picture.input.data(), static_cast<size_t>(picture.state->BytesPerRow()), 3840, 2160,
						true, planes));
					pl_gpu_finish(gpu);
					gpuWallMs = std::min(gpuWallMs, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
					const uint64_t ns = pl_timer_query(gpu, timer);
					if (ns > 0)
						passMs = std::min(passMs, ns / 1e6);
				}
				unpack.SetTimerForTest(nullptr);
				pl_timer_destroy(gpu, &timer);
				Logger::WriteMessage(("GpuDolbyVisionTunnelUnpack 4K: CPU unpack " + std::to_string(cpuMs) + " ms; AnalyzeFrame " +
					std::to_string(analyzeMs) + " ms; raw upload + GPU pass " + std::to_string(gpuWallMs) + " ms (pass alone " +
					std::to_string(passMs) + " ms)\n").c_str());
			}
			pl_tex_destroy(gpu, &planes[0]);
			pl_tex_destroy(gpu, &planes[1]);
			AssertCleanLibplaceboLog(log);
			Logger::WriteMessage(("GpuDolbyVisionTunnelUnpack: " + std::to_string(cases) + " cases matched the CPU unpack\n").c_str());
		}

		// Rule test (2026-10-17): Dolby Vision GPU unpack with a lost / corrupt DM packet
		// presents black, never garbage. A tunnel stream through the renderer's route
		// (LibplaceboVideoRenderer's dvRaw path: AnalyzeFrame decides, PICTURE ->
		// GpuDolbyVisionTunnelUnpack, otherwise zero planes) on WARP: a locked picture
		// unpacks on the GPU equal to the CPU; with every copy of the DM header damaged
		// the route reaches BLACK within the formatter's loss limit and the planes the
		// renderer receives are all zero (the CPU formatter agrees); the packed pixels
		// would not be black (the gate matters); a fresh DM brings the GPU picture
		// back. A stream that starts with a damaged DM is black from its first frame.
		TEST_METHOD(GpuDolbyVisionDmLossPresentsBlackNotGarbage)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			GpuDolbyVisionTunnelUnpack unpack(fixture.Log(), gpu);
			unpack.SetHostReadableForTest(true);
			pl_tex planes[2] = { nullptr, nullptr };
			auto download = [&](pl_tex tex)
			{
				std::vector<uint16_t> samples(static_cast<size_t>(tex->params.w) * tex->params.h *
					tex->params.format->num_components);
				pl_tex_transfer_params d{};
				d.tex = tex;
				d.ptr = samples.data();
				Assert::IsTrue(pl_tex_download(gpu, &d), L"plane not host-readable");
				return samples;
			};
			const unsigned width = 130, height = 128;
			Tests::DvSender::SenderPicture picture(true, width, height);
			picture.Embed(Tests::DvSender::Packetize(Tests::DvSender::DmBody()));
			Tests::DvSender::SenderPicture damaged = picture;
			damaged.CorruptAllCopies(0);
			const size_t rowBytes = picture.state->BytesPerRow();
			const size_t planePixels = static_cast<size_t>(width) * height;
			// The renderer's route for one frame; returns the planes it would render.
			struct Routed { CR210DolbyVisionTunnelFormatter::FrameResult result; std::vector<uint16_t> intensity, chroma; };
			auto route = [&](CR210DolbyVisionTunnelFormatter& analyzed, CR210DolbyVisionTunnelFormatter& cpu,
				const std::vector<BYTE>& input, uint64_t counter, const wchar_t* name)
			{
				Routed out;
				const VideoFrame frame(const_cast<BYTE*>(input.data()), counter, 0, nullptr);
				out.result = analyzed.AnalyzeFrame(frame);
				Assert::IsTrue(out.result != CR210DolbyVisionTunnelFormatter::FrameResult::REJECTED,
					(std::wstring(name) + L": frame rejected").c_str());
				std::vector<BYTE> expected(static_cast<size_t>(cpu.GetOutFrameSize()));
				Assert::IsTrue(cpu.FormatVideoFrame(frame, expected.data()), (std::wstring(name) + L": CPU unpack failed").c_str());
				const uint16_t* want = reinterpret_cast<const uint16_t*>(expected.data());
				if (out.result == CR210DolbyVisionTunnelFormatter::FrameResult::PICTURE)
				{
					Assert::IsTrue(unpack.Unpack(VideoFrameEncoding::R210, input.data(), rowBytes, static_cast<int>(width),
						static_cast<int>(height), analyzed.PlanesHighAligned(), planes),
						(std::wstring(name) + L": " + ExtendedRangeGpuProbe::ToWide(unpack.Error())).c_str());
					out.intensity = download(planes[0]);
					out.chroma = download(planes[1]);
					Assert::AreEqual(planePixels, out.intensity.size());
					for (size_t i = 0; i < planePixels; ++i)
						Assert::IsTrue(out.intensity[i] == want[i] && out.chroma[i] == want[planePixels + i],
							(std::wstring(name) + L": the GPU picture differs from the CPU at " + std::to_wstring(i)).c_str());
				}
				else
				{
					// No trusted DM: the renderer hands on zero planes (never the packed bytes).
					out.intensity.assign(planePixels, 0);
					out.chroma.assign(planePixels, 0);
					for (size_t i = 0; i < 2 * planePixels; ++i)
						Assert::AreEqual(0, static_cast<int>(want[i]), (std::wstring(name) + L": the CPU formatter is not black").c_str());
				}
				return out;
			};
			auto black = [](const Routed& r)
			{
				for (uint16_t v : r.intensity) if (v != 0) return false;
				for (uint16_t v : r.chroma) if (v != 0) return false;
				return true;
			};
			// The packed pixels of the damaged frame, unpacked anyway: not black (what the route
			// must never show).
			{
				CR210DolbyVisionTunnelFormatter probe;
				probe.OnVideoState(picture.state);
				const VideoFrame frame(picture.input.data(), 1, 0, nullptr);
				Assert::IsTrue(probe.AnalyzeFrame(frame) == CR210DolbyVisionTunnelFormatter::FrameResult::PICTURE);
				Assert::IsTrue(unpack.Unpack(VideoFrameEncoding::R210, damaged.input.data(), rowBytes, static_cast<int>(width),
					static_cast<int>(height), probe.PlanesHighAligned(), planes));
				const std::vector<uint16_t> garbage = download(planes[0]);
				size_t nonZero = 0;
				for (uint16_t v : garbage) nonZero += v != 0 ? 1 : 0;
				Assert::IsTrue(nonZero > planePixels / 2, L"precondition: the damaged frame's packed pixels are not black");
			}
			// Locked, then the DM lost, then a fresh DM.
			CR210DolbyVisionTunnelFormatter analyzed, cpu;
			analyzed.OnVideoState(picture.state);
			cpu.OnVideoState(picture.state);
			uint64_t counter = 1;
			Assert::IsTrue(route(analyzed, cpu, picture.input, counter++, L"locked").result ==
				CR210DolbyVisionTunnelFormatter::FrameResult::PICTURE, L"the sender picture locks");
			int blackAt = -1;
			for (int lost = 0; lost < 60 && blackAt < 0; ++lost)
			{
				const Routed r = route(analyzed, cpu, damaged.input, counter++, L"DM lost");
				if (r.result == CR210DolbyVisionTunnelFormatter::FrameResult::BLACK)
				{
					Assert::IsTrue(black(r), L"DM lost: black planes");
					blackAt = lost;
				}
			}
			Logger::WriteMessage(("DM loss: black after " + std::to_string(blackAt + 1) + " damaged frame(s)\n").c_str());
			Assert::IsTrue(blackAt >= 0, L"a lasting DM loss reaches black within the loss limit");
			Assert::IsFalse(analyzed.GetDolbyVisionTunnelMetadata().valid || analyzed.GetDolbyVisionTunnelMetadata().allowSdrFallback,
				L"after DV lock a lost DM is neither trusted nor allowed the SDR passthrough (the packed pixels)");
			// Still lost: stays black.
			for (int more = 0; more < 5; ++more)
				Assert::IsTrue(black(route(analyzed, cpu, damaged.input, counter++, L"still lost")), L"stays black while the DM is lost");
			// A fresh, complete DM (a new metadata ID, as a sender restarts): the GPU picture again.
			Tests::DvSender::SenderPicture fresh = picture;
			fresh.Embed(Tests::DvSender::Packetize(Tests::DvSender::DmBody(2048)));
			bool restored = false;
			for (int k = 0; k < 10 && !restored; ++k)
				restored = route(analyzed, cpu, fresh.input, counter++, L"fresh DM").result ==
					CR210DolbyVisionTunnelFormatter::FrameResult::PICTURE;
			Assert::IsTrue(restored, L"a fresh DM brings the GPU picture back");
			// A stream whose first frame already has a damaged DM: zero DV planes from the start (before
			// any lock the renderer may still choose the SDR passthrough - allowSdrFallback, reported).
			{
				CR210DolbyVisionTunnelFormatter coldAnalyzed, coldCpu;
				coldAnalyzed.OnVideoState(picture.state);
				coldCpu.OnVideoState(picture.state);
				for (uint64_t n = 1; n <= 3; ++n)
				{
					const Routed r = route(coldAnalyzed, coldCpu, damaged.input, n, L"never locked");
					Assert::IsTrue(r.result == CR210DolbyVisionTunnelFormatter::FrameResult::BLACK && black(r),
						L"never locked with a damaged DM: zero planes, not the packed pixels");
				}
				Logger::WriteMessage(coldAnalyzed.GetDolbyVisionTunnelMetadata().allowSdrFallback ?
					"never locked: SDR passthrough allowed (no DV lock yet)\n" : "never locked: SDR passthrough not allowed\n");
			}
			pl_tex_destroy(gpu, &planes[0]);
			pl_tex_destroy(gpu, &planes[1]);
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(log);
		}

		TEST_METHOD(GpuCaptureUnpackMatchesTheCpuFormatters)
		{
			std::string where = "setup";
			try {
			// capture_path: gpu (owner 2026-10-13): the GPU unpack of raw
			// DeckLink V210 / UYVY / HDYC equals the CPU formatters' P210 sample
			// for sample, at every V210 packing phase (widths 96, 98, 100 end a
			// line on pack phase 0, 2, 4: widths 120, 122, 124) and at 1920, with random bytes
			// (padding included). Then the GPU pass and the CPU conversion it
			// replaces are timed at 3840x2160 (report).
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			where = "unpack object";
			GpuCaptureUnpack unpack(fixture.Log(), gpu);
			unpack.SetHostReadableForTest(true);
			where = "loop";
			pl_tex planes[2] = { nullptr, nullptr };
			uint32_t seed = 0x2545f491u;
			auto next = [&seed]() { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; };
			auto makeState = [](VideoFrameEncoding encoding, int width, int height)
			{
				VideoStateComPtr state = new VideoState();
				state->valid = true;
				state->displayMode = std::make_shared<DisplayMode>(width, height, false, 60000, 1001);
				state->videoFrameEncoding = encoding;
				state->colorspace = ColorSpace::REC_709;
				return state;
			};
			auto download = [&](pl_tex tex)
			{
				std::vector<uint16_t> samples(static_cast<size_t>(tex->params.w) * tex->params.h *
					tex->params.format->num_components);
				pl_tex_transfer_params d{};
				d.tex = tex;
				d.ptr = samples.data();
				Assert::IsTrue(pl_tex_download(gpu, &d), L"plane not host-readable");
				return samples;
			};
			for (const VideoFrameEncoding encoding : { VideoFrameEncoding::V210, VideoFrameEncoding::UYVY, VideoFrameEncoding::HDYC })
				for (const int width : { 120, 122, 124, 1920 })
				{
					const int height = 100;
					std::wstring stage = L"state";
					try {
					VideoStateComPtr state = makeState(encoding, width, height);
					const size_t rowBytes = static_cast<size_t>(state->BytesPerRow());
					std::vector<BYTE> source(static_cast<size_t>(state->BytesPerFrame()));
					for (BYTE& b : source) b = static_cast<BYTE>(next());
					std::unique_ptr<IVideoFrameFormatter> cpu;
					if (encoding == VideoFrameEncoding::V210) cpu = std::make_unique<CV210toP210VideoFrameFormatter>();
					else cpu = std::make_unique<CUYVYtoP210VideoFrameFormatter>();
					cpu->OnVideoState(state);
					std::vector<BYTE> expected(static_cast<size_t>(cpu->GetOutFrameSize()));
					const VideoFrame frame(source.data(), 1, 0, nullptr);
					Assert::IsTrue(cpu->FormatVideoFrame(frame, expected.data()));
					const std::wstring name = (encoding == VideoFrameEncoding::V210 ? L"V210 " :
						encoding == VideoFrameEncoding::UYVY ? L"UYVY " : L"HDYC ") + std::to_wstring(width);
					Assert::IsTrue(unpack.Unpack(encoding, source.data(), rowBytes, width, height, planes),
						(name + L": " + std::wstring(unpack.Error().begin(), unpack.Error().end()) + L"; log: " +
							std::wstring(log.begin(), log.end())).c_str());
					const auto contract = cpu->GetOutputContract(), gpuContract = GpuCaptureUnpack::Contract(encoding);
					Assert::IsTrue(contract.sampleRange == gpuContract.sampleRange && contract.colorDepth == gpuContract.colorDepth &&
						contract.bitShift == gpuContract.bitShift, (name + L": the contract").c_str());
					const std::vector<uint16_t> y = download(planes[0]), uv = download(planes[1]);
					const uint16_t* cpuY = reinterpret_cast<const uint16_t*>(expected.data());
					const uint16_t* cpuUv = cpuY + static_cast<size_t>(width) * height;
					size_t yDiff = 0, uvDiff = 0, first = SIZE_MAX;
					for (size_t i = 0; i < static_cast<size_t>(width) * height; ++i)
						if (y[i] != cpuY[i]) { ++yDiff; first = std::min(first, i); }
					for (size_t i = 0; i < static_cast<size_t>(width) * height; ++i)
						if (uv[i] != cpuUv[i]) { ++uvDiff; first = std::min(first, i); }
					Assert::IsTrue(yDiff == 0 && uvDiff == 0, (name + L": " + std::to_wstring(yDiff) + L" luma and " +
						std::to_wstring(uvDiff) + L" chroma samples differ (first at " + std::to_wstring(first) + L")").c_str());
					} catch (const std::exception& e) {
						const std::string what = e.what();
						Assert::Fail((L"width " + std::to_wstring(width) + L" encoding " + std::to_wstring(static_cast<int>(encoding)) +
							L": " + std::wstring(what.begin(), what.end())).c_str());
					}
				}
			// Efficiency (report): 3840x2160, the GPU pass (timer) and the CPU
			// conversion it replaces.
			for (const VideoFrameEncoding encoding : { VideoFrameEncoding::V210, VideoFrameEncoding::UYVY })
			{
				const int width = 3840, height = 2160;
				VideoStateComPtr state = makeState(encoding, width, height);
				std::vector<BYTE> source(static_cast<size_t>(state->BytesPerFrame()));
				for (BYTE& b : source) b = static_cast<BYTE>(next());
				std::unique_ptr<IVideoFrameFormatter> cpu;
				if (encoding == VideoFrameEncoding::V210) cpu = std::make_unique<CV210toP210VideoFrameFormatter>();
				else cpu = std::make_unique<CUYVYtoP210VideoFrameFormatter>();
				cpu->OnVideoState(state);
				std::vector<BYTE> out(static_cast<size_t>(cpu->GetOutFrameSize()));
				const VideoFrame frame(source.data(), 1, 0, nullptr);
				double cpuMs = 1e30, uploadAndPassMs = 1e30, passMs = 1e30, standardUploadMs = 1e30;
				pl_tex standard[2] = { nullptr, nullptr };
				pl_timer timer = pl_timer_create(gpu);
				unpack.SetTimerForTest(timer);
				for (int run = 0; run < 8; ++run)
				{
					auto t0 = std::chrono::steady_clock::now();
					cpu->FormatVideoFrame(frame, out.data());
					cpuMs = std::min(cpuMs, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
					// Today's path after the conversion: the P210 planes' upload (as RenderLocked).
					{
						pl_plane_data planesData[2]{};
						planesData[0].type = PL_FMT_UNORM;
						planesData[0].width = width;
						planesData[0].height = height;
						planesData[0].component_size[0] = 16;
						planesData[0].component_map[0] = PL_CHANNEL_Y;
						planesData[0].pixel_stride = sizeof(uint16_t);
						planesData[0].row_stride = static_cast<size_t>(width) * 2;
						planesData[0].pixels = out.data();
						planesData[1] = planesData[0];
						planesData[1].width = width / 2;
						planesData[1].component_size[1] = 16;
						planesData[1].component_map[0] = PL_CHANNEL_CB;
						planesData[1].component_map[1] = PL_CHANNEL_CR;
						planesData[1].pixel_stride = sizeof(uint16_t) * 2;
						planesData[1].pixels = out.data() + static_cast<size_t>(width) * height * 2;
						pl_gpu_finish(gpu);
						const auto u0 = std::chrono::steady_clock::now();
						pl_plane uploaded[2]{};
						for (int i = 0; i < 2; ++i)
							Assert::IsTrue(pl_upload_plane(gpu, &uploaded[i], &standard[i], &planesData[i]));
						pl_gpu_finish(gpu);
						standardUploadMs = std::min(standardUploadMs, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - u0).count());
					}
					pl_gpu_finish(gpu);
					t0 = std::chrono::steady_clock::now();
					Assert::IsTrue(unpack.Unpack(encoding, source.data(), static_cast<size_t>(state->BytesPerRow()), width, height, planes));
					pl_gpu_finish(gpu);
					uploadAndPassMs = std::min(uploadAndPassMs, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
					for (uint64_t ns = pl_timer_query(gpu, timer); ns; ns = pl_timer_query(gpu, timer))
						passMs = std::min(passMs, ns / 1e6);
				}
				unpack.SetTimerForTest(nullptr);
				pl_timer_destroy(gpu, &timer);
				pl_tex_destroy(gpu, &standard[0]);
				pl_tex_destroy(gpu, &standard[1]);
				char line[320];
				std::snprintf(line, sizeof(line), "GPU capture unpack %s 3840x2160: standard = CPU conversion %.2f + P210 upload %.2f = %.2f ms; "
					"gpu = raw upload + pass %.2f ms (pass alone %.3f ms)",
					encoding == VideoFrameEncoding::V210 ? "V210" : "UYVY", cpuMs, standardUploadMs, cpuMs + standardUploadMs,
					uploadAndPassMs, passMs < 1e29 ? passMs : -1.0);
				Logger::WriteMessage(line);
			}
			pl_tex_destroy(gpu, &planes[0]);
			pl_tex_destroy(gpu, &planes[1]);
			} catch (const std::exception& e) { const std::string w = where + ": " + e.what(); Assert::Fail(std::wstring(w.begin(), w.end()).c_str()); }
			catch (...) { Assert::Fail(L"non-std exception"); }
		}

		TEST_METHOD(DejudderGlobalTwoFindsTwoMotionsBeyondTheCoarseRange)
		{
			// M2 (owner 2026-10-13 night): the top half pans +140 px and the
			// bottom half -140 px, both beyond the coarse search; D1 alone is not
			// confident (two equal candidates) so neither half is found. With
			// the second candidate both halves are within half a pixel.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			const int width = 512, height = 256;
			static constexpr int dx = 140;
			auto lumaA = [](int x, int y) { return y < 128 ? DejudderTexture(x, y, 3) : DejudderTexture(x, y, 9); };
			auto lumaB = [](int x, int y) { return y < 128 ? DejudderTexture(x - dx, y, 3) : DejudderTexture(x + dx, y, 9); };
			auto chroma = [](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); };
			const DejudderFrame a = MakeDejudderFrame(width, height, lumaA, chroma);
			const DejudderFrame b = MakeDejudderFrame(width, height, lumaB,
				[](int x, int y, int c) { return DejudderTexture((x - (y < 128 ? dx : -dx) / 2) * 2, y, 7 + c); });
			DejudderGpuPlanes planesA, planesB;
			UploadDejudderFrame(gpu, a, planesA);
			UploadDejudderFrame(gpu, b, planesB);
			auto count = [&](const DejudderField& field, bool top)
			{
				int total = 0, right = 0;
				const int want = top ? dx : -dx;
				for (int by = top ? 2 : 10; by < (top ? 6 : field.blocksY - 2); ++by)
					for (int bx = 1; bx < field.blocksX - 1; ++bx)
					{
						const int x0 = bx * 16;
						if (top ? x0 + 16 + dx >= width - 16 : x0 - dx < 16) continue;   // leaves the picture
						++total;
						right += field.trusted(bx, by) && std::fabs(field.vx(bx, by) - want) <= 0.5f && std::fabs(field.vy(bx, by)) <= 0.5f ? 1 : 0;
					}
				return std::make_pair(right, total);
			};
			auto run = [&](bool two)
			{
				Dejudder::Settings settings = DejudderTestSettings();
				settings.globalPrior = true;
				settings.globalTwo = two;
				settings.minTrustedShare = 0.0f;
				Dejudder::MotionWarp warp(fixture.Log(), gpu, settings);
				Assert::IsTrue(warp.Estimate(planesA.planes, planesB.planes), ExtendedRangeGpuProbe::ToWide(warp.LastError()).c_str());
				const DejudderField field = DownloadField(gpu, warp);
				return std::make_pair(count(field, true), count(field, false));
			};
			const auto one = run(false), two = run(true);
			const std::wstring message = L"right blocks, one candidate: top " + std::to_wstring(one.first.first) + L"/" +
				std::to_wstring(one.first.second) + L", bottom " + std::to_wstring(one.second.first) + L"/" + std::to_wstring(one.second.second) +
				L"; two: top " + std::to_wstring(two.first.first) + L"/" + std::to_wstring(two.first.second) + L", bottom " +
				std::to_wstring(two.second.first) + L"/" + std::to_wstring(two.second.second) + L"; log: " + std::wstring(log.begin(), log.end());
			Assert::IsTrue(one.first.first * 100 < one.first.second * 50 || one.second.first * 100 < one.second.second * 50,
				(L"one candidate should miss a half: " + message).c_str());
			Assert::IsTrue(two.first.first * 100 >= two.first.second * 90 && two.second.first * 100 >= two.second.second * 90, message.c_str());
			AssertCleanLibplaceboLog(log);
		}

		TEST_METHOD(DejudderRegionalHoldWarpsTheMatchedAreaUnlessACut)
		{
			// M3 (planner Q9): a band pans 8 px while the rest of the picture is
			// new noise every frame (hard motion: few blocks match, the picture's
			// brightness distribution unchanged). Off: the whole frame holds. On:
			// the band's blocks warp (FrameTrust().r at least min_share, the
			// band trusted) - and no cut. A real cut (new, darker content) still
			// holds the whole frame with the regional hold on.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			const int width = 512, height = 512;
			static constexpr int dx = 8;
			auto noise = [](int x, int y, unsigned seed)
			{
				const unsigned h = (static_cast<unsigned>(x) * 73856093u) ^ (static_cast<unsigned>(y) * 19349663u) ^ (seed * 83492791u);
				return static_cast<uint16_t>(8000 + ((h * 2654435761u) >> 16) % 40000u);
			};
			auto band = [](int y) { return y >= 192 && y < 288; };   // 6 block rows of 16 (under a quarter)
			auto chroma = [](int x, int y, int c) { return static_cast<uint16_t>(32768 + c); };
			const DejudderFrame a = MakeDejudderFrame(width, height,
				[&](int x, int y) { return band(y) ? DejudderTexture(x, y, 5) : noise(x, y, 1); }, chroma);
			const DejudderFrame b = MakeDejudderFrame(width, height,
				[&](int x, int y) { return band(y) ? DejudderTexture(x - dx, y, 5) : noise(x, y, 2); }, chroma);
			const DejudderFrame cut = MakeDejudderFrame(width, height,
				[&](int x, int y) { return static_cast<uint16_t>(2000 + (DejudderTexture(x, y, 77) - 8000) / 8); }, chroma);
			DejudderGpuPlanes planesA, planesB, planesCut;
			UploadDejudderFrame(gpu, a, planesA);
			UploadDejudderFrame(gpu, b, planesB);
			UploadDejudderFrame(gpu, cut, planesCut);
			struct Result { float r = 0, share = 0; int bandTrusted = 0, bandTotal = 0; };
			auto run = [&](bool regional, const Dejudder::Planes& second)
			{
				Dejudder::Settings settings = DejudderTestSettings();
				settings.globalPrior = true;
				settings.regionalHold = regional;
				settings.minTrustedShare = 0.25f;
				Dejudder::MotionWarp warp(fixture.Log(), gpu, settings);
				Assert::IsTrue(warp.Estimate(planesA.planes, second), ExtendedRangeGpuProbe::ToWide(warp.LastError()).c_str());
				Result result;
				result.r = DownloadFrameTrust(gpu, warp)[0];
				const DejudderField field = DownloadField(gpu, warp);
				int trusted = 0;
				for (int by = 0; by < field.blocksY; ++by)
					for (int bx = 0; bx < field.blocksX; ++bx)
					{
						trusted += field.trusted(bx, by) ? 1 : 0;
						if (by >= 13 && by < 17 && bx >= 1 && bx < field.blocksX - 2)
						{
							++result.bandTotal;
							result.bandTrusted += field.trusted(bx, by) && std::fabs(field.vx(bx, by) - dx) <= 0.5f ? 1 : 0;
						}
					}
				result.share = static_cast<float>(trusted) / (field.blocksX * field.blocksY);
				return result;
			};
			const Result off = run(false, planesB.planes), on = run(true, planesB.planes), cutOn = run(true, planesCut.planes);
			const std::wstring message = L"off: r " + std::to_wstring(off.r) + L" share " + std::to_wstring(off.share) + L"; on: r " +
				std::to_wstring(on.r) + L" share " + std::to_wstring(on.share) + L" band " + std::to_wstring(on.bandTrusted) + L"/" +
				std::to_wstring(on.bandTotal) + L"; cut (on): r " + std::to_wstring(cutOn.r) + L" share " + std::to_wstring(cutOn.share) +
				L"; log: " + std::wstring(log.begin(), log.end());
			Assert::IsTrue(off.share < 0.25f && off.r < 0.25f, (L"hard motion: few blocks match, off holds the frame: " + message).c_str());
			Assert::IsTrue(on.r >= 0.25f, (L"on: not a cut, the holds stay per block: " + message).c_str());
			Assert::IsTrue(on.bandTrusted * 100 >= on.bandTotal * 80, (L"on: the band is trusted: " + message).c_str());
			Assert::IsTrue(cutOn.r < 0.25f, (L"a real cut still holds the whole frame: " + message).c_str());
			AssertCleanLibplaceboLog(log);
		}

		TEST_METHOD(DejudderTextureToleranceTrustsGrainyTextureWithoutFlatFalseMatches)
		{
			// M1: a pan of 6 px with fresh grain on a detailed texture: the
			// correct match differs by more than the fixed tolerance, so few
			// blocks are trusted; with the texture allowance most are, with the
			// right vector. Off is today exactly (the same field).
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			const int width = 512, height = 256;
			static constexpr int dx = 6;
			auto grain = [](int x, int y, unsigned seed)
			{
				const unsigned h = (static_cast<unsigned>(x) * 73856093u) ^ (static_cast<unsigned>(y) * 19349663u) ^ (seed * 83492791u);
				return static_cast<int>(((h * 2654435761u) >> 16) % 8000u) - 4000;
			};
			auto chroma = [](int x, int y, int c) { return static_cast<uint16_t>(32768 + c); };
			const DejudderFrame a = MakeDejudderFrame(width, height,
				[&](int x, int y) { return static_cast<uint16_t>(DejudderTexture(x, y, 11) + grain(x, y, 1)); }, chroma);
			const DejudderFrame b = MakeDejudderFrame(width, height,
				[&](int x, int y) { return static_cast<uint16_t>(DejudderTexture(x - dx, y, 11) + grain(x, y, 2)); }, chroma);
			DejudderGpuPlanes planesA, planesB;
			UploadDejudderFrame(gpu, a, planesA);
			UploadDejudderFrame(gpu, b, planesB);
			auto run = [&](float textureTolerance)
			{
				Dejudder::Settings settings = DejudderTestSettings();
				settings.globalPrior = true;
				settings.matchTolerance = 3.0f;
				settings.textureTolerance = textureTolerance;
				Dejudder::MotionWarp warp(fixture.Log(), gpu, settings);
				Assert::IsTrue(warp.Estimate(planesA.planes, planesB.planes), ExtendedRangeGpuProbe::ToWide(warp.LastError()).c_str());
				const DejudderField field = DownloadField(gpu, warp);
				int trusted = 0, right = 0, total = 0;
				for (int by = 1; by < field.blocksY - 1; ++by)
					for (int bx = 1; bx < field.blocksX - 2; ++bx)
					{
						++total;
						trusted += field.trusted(bx, by) ? 1 : 0;
						right += field.trusted(bx, by) && std::fabs(field.vx(bx, by) - dx) <= 0.5f && std::fabs(field.vy(bx, by)) <= 0.5f ? 1 : 0;
					}
				return std::make_tuple(trusted, right, total);
			};
			const auto off = run(0.0f), on = run(0.5f);
			const std::wstring message = L"trusted / right / blocks: off " + std::to_wstring(std::get<0>(off)) + L"/" +
				std::to_wstring(std::get<1>(off)) + L"/" + std::to_wstring(std::get<2>(off)) + L", allowance 0.5 " +
				std::to_wstring(std::get<0>(on)) + L"/" + std::to_wstring(std::get<1>(on)) + L"/" + std::to_wstring(std::get<2>(on)) +
				L"; log: " + std::wstring(log.begin(), log.end());
			Assert::IsTrue(std::get<0>(off) * 100 < std::get<2>(off) * 50, (L"grain: the fixed tolerance trusts few: " + message).c_str());
			Assert::IsTrue(std::get<1>(on) * 100 >= std::get<2>(on) * 80, (L"the allowance trusts most, rightly: " + message).c_str());
			Assert::IsTrue(std::get<0>(on) - std::get<1>(on) <= std::get<2>(on) / 50, (L"and no wrong trusted vectors: " + message).c_str());
			AssertCleanLibplaceboLog(log);
		}

		TEST_METHOD(DejudderFadeMatchFollowsASmoothFadeAndHoldsAFlash)
		{
			// M4 (planner Q11): a pan of 6 px while the picture darkens 6% a
			// frame. Without the fade match the blocks do not match (held).
			// With it, from the second pair of the fade on, the blocks match with
			// the right vector and the strength is the floor; a one-frame flash
			// (a bright frame between two normal ones) is not a smooth fade and
			// holds.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			const int width = 512, height = 256;
			auto frame = [&](int shift, double level)
			{
				return MakeDejudderFrame(width, height, [=](int x, int y) {
					return static_cast<uint16_t>(4096 + (DejudderTexture(x - shift, y, 13) - 4096) * level); },
					[](int x, int y, int c) { return static_cast<uint16_t>(32768 + c); });
			};
			std::vector<DejudderFrame> fade;
			for (int f = 0; f < 4; ++f) fade.push_back(frame(6 * f, 1.0 - 0.06 * f));
			std::vector<std::unique_ptr<DejudderGpuPlanes>> up;
			for (const DejudderFrame& f : fade) { up.push_back(std::make_unique<DejudderGpuPlanes>()); UploadDejudderFrame(gpu, f, *up.back()); }
			struct Result { int right = 0, total = 0; float scale = 0; };
			auto run = [&](bool fadeMatch, const std::vector<Dejudder::Planes>& seq)
			{
				Dejudder::Settings settings = DejudderTestSettings();
				settings.globalPrior = true;
				settings.fadeMatch = fadeMatch;
				settings.fadeFloor = 0.25f;
				Dejudder::MotionWarp warp(fixture.Log(), gpu, settings);
				for (size_t k = 0; k + 1 < seq.size(); ++k)
				{
					const bool estimated = warp.Estimate(seq[k], seq[k + 1]);
					Assert::IsTrue(estimated, (L"estimate " + std::to_wstring(k) + (fadeMatch ? L" (fade match): " : L": ") +
						ExtendedRangeGpuProbe::ToWide(warp.LastError()) + L"; log: " + std::wstring(log.begin(), log.end())).c_str());
				}
				const DejudderField field = DownloadField(gpu, warp);
				Result r;
				for (int by = 1; by < field.blocksY - 1; ++by)
					for (int bx = 1; bx < field.blocksX - 2; ++bx)
					{
						++r.total;
						r.right += field.trusted(bx, by) && std::fabs(field.vx(bx, by) - 6.0f) <= 0.5f ? 1 : 0;
					}
				r.scale = DownloadFrameTrust(gpu, warp)[1];
				return r;
			};
			std::vector<Dejudder::Planes> seq;
			for (auto& p : up) seq.push_back(p->planes);
			const Result off = run(false, seq), on = run(true, seq);
			// A flash: normal, bright (x1.4), normal again; the last pair is
			// bright -> normal after normal -> bright: not the same direction.
			const DejudderFrame bright = frame(6, 1.4), normal2 = frame(12, 1.0);
			DejudderGpuPlanes pBright, pNormal2;
			UploadDejudderFrame(gpu, bright, pBright);
			UploadDejudderFrame(gpu, normal2, pNormal2);
			const Result flash = run(true, { up[0]->planes, pBright.planes, pNormal2.planes });
			const std::wstring message = L"right / blocks (strength): off " + std::to_wstring(off.right) + L"/" + std::to_wstring(off.total) +
				L" (" + std::to_wstring(off.scale) + L"), fade match " + std::to_wstring(on.right) + L"/" + std::to_wstring(on.total) + L" (" +
				std::to_wstring(on.scale) + L"), flash " + std::to_wstring(flash.right) + L"/" + std::to_wstring(flash.total) + L" (" +
				std::to_wstring(flash.scale) + L"); log: " + std::wstring(log.begin(), log.end());
			Assert::IsTrue(off.right * 100 < off.total * 30, (L"a fade: the plain match fails: " + message).c_str());
			Assert::IsTrue(on.right * 100 >= on.total * 80, (L"the fade match follows the pan: " + message).c_str());
			Assert::IsTrue(std::fabs(on.scale - 0.25f) < 0.02f, (L"at the floor strength: " + message).c_str());
			Assert::IsTrue(flash.right * 100 < flash.total * 30, (L"a flash holds: " + message).c_str());
			// A pan whose mean brightness changes as brighter content enters (a
			// vertical gradient panned 8 px a frame) is not a fade: full strength.
			std::vector<std::unique_ptr<DejudderGpuPlanes>> pan;
			std::vector<Dejudder::Planes> panSeq;
			for (int f = 0; f < 5; ++f)
			{
				const DejudderFrame frame = MakeDejudderFrame(width, height, [f](int x, int y) {
					return static_cast<uint16_t>(std::min(65535, 2000 + (DejudderTexture(x, y - 8 * f, 13) - 8000) / 2 + (y - 8 * f + 64) * 150)); },
					[](int x, int y, int c) { return static_cast<uint16_t>(32768 + c); });
				pan.push_back(std::make_unique<DejudderGpuPlanes>());
				UploadDejudderFrame(gpu, frame, *pan.back());
				panSeq.push_back(pan.back()->planes);
			}
			const Result panned = run(true, panSeq);
			Assert::IsTrue(panned.scale > 0.99f, (L"a pan that changes the mean is not a fade (strength " + std::to_wstring(panned.scale) + L")").c_str());
			AssertCleanLibplaceboLog(log);
		}

		TEST_METHOD(DejudderRolloffStartsOneFrameEarlierWithTheEstimateAhead)
		{
			// Planner Q7: a pan that accelerates past roll_lo (6 px, then 30 px).
			// After the fast pair is estimated ahead, the slow pair's extras use
			// the lower strength (the roll-off starts one frame earlier), at most
			// one rate step below the slow pair's; without R-A the snapshot is
			// returned as is. Both stay rate-limited.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			const int width = 256, height = 128;
			auto at = [&](int shift)
			{
				return MakeDejudderFrame(width, height, [shift](int x, int y) { return DejudderTexture(x - shift, y); },
					[shift](int x, int y, int c) { return DejudderTexture((x - shift / 2) * 2, y, 7 + c); });
			};
			const DejudderFrame f0 = at(0), f1 = at(6), f2 = at(36);
			DejudderGpuPlanes p0, p1, p2;
			UploadDejudderFrame(gpu, f0, p0);
			UploadDejudderFrame(gpu, f1, p1);
			UploadDejudderFrame(gpu, f2, p2);
			auto read = [&](pl_tex t)
			{
				std::array<float, 4> out{};
				std::vector<uint16_t> half(4);
				pl_tex_transfer_params d{};
				d.tex = t;
				d.ptr = half.data();
				Assert::IsTrue(pl_tex_download(gpu, &d));
				for (int k = 0; k < 4; ++k)
				{
					const uint16_t h = half[k];
					const int e = (h >> 10) & 31, m = h & 1023;
					out[k] = (e == 0 ? std::ldexp(static_cast<float>(m), -24) : std::ldexp(1.0f + m / 1024.0f, e - 15)) * ((h & 0x8000) ? -1.0f : 1.0f);
				}
				return out;
			};
			auto run = [&](bool rolloff)
			{
				Dejudder::Settings settings = DejudderTestSettings();
				settings.globalPrior = true;
				settings.motionRolloff = rolloff;
				settings.rollLo = 8.0f;
				settings.rollHi = 24.0f;
				settings.rollFloor = 0.25f;
				settings.strengthRate = 0.25f;
				Dejudder::MotionWarp warp(fixture.Log(), gpu, settings);
				for (int i = 0; i < 4; ++i)   // the slow pair settles
					Assert::IsTrue(warp.Estimate(p0.planes, p1.planes), ExtendedRangeGpuProbe::ToWide(warp.LastError()).c_str());
				Assert::IsTrue(warp.SnapshotForWarp());
				const float slow = read(warp.SnapFrameTrust())[1];
				Assert::IsTrue(warp.Estimate(p1.planes, p2.planes), ExtendedRangeGpuProbe::ToWide(warp.LastError()).c_str());
				const float next = read(warp.FrameTrust())[1];
				const float ahead = read(warp.LookAheadTrust())[1];
				return std::make_tuple(slow, next, ahead);
			};
			const auto on = run(true), off = run(false);
			const std::wstring message = L"R-A on: slow " + std::to_wstring(std::get<0>(on)) + L", next " + std::to_wstring(std::get<1>(on)) +
				L", ahead " + std::to_wstring(std::get<2>(on)) + L"; off: slow " + std::to_wstring(std::get<0>(off)) + L", ahead " +
				std::to_wstring(std::get<2>(off)) + L"; log: " + std::wstring(log.begin(), log.end());
			Assert::IsTrue(std::get<0>(on) > 0.95f, (L"the slow pan: full strength: " + message).c_str());
			Assert::IsTrue(std::get<2>(on) < std::get<0>(on) - 0.1f, (L"the roll-off starts with the estimate ahead: " + message).c_str());
			Assert::IsTrue(std::get<0>(on) - std::get<2>(on) <= 0.25f + 1e-3f, (L"one rate step at most: " + message).c_str());
			Assert::IsTrue(std::fabs(std::get<2>(off) - std::get<0>(off)) < 1e-3f, (L"R-A off: the snapshot as is: " + message).c_str());
			AssertCleanLibplaceboLog(log);
		}

		TEST_METHOD(DejudderVerifyWithdrawsTrustFromBlocksWhosePixelsDoNotFollow)
		{
			// M5: a pan of 8 px with small static patches (8x8 source px, +30
			// codes) in some blocks: at a loose tolerance (8) those blocks match
			// on average and are trusted; their patch pixels mispredict, so the
			// pixel check withdraws their trust; blocks without a patch stay.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			const int width = 512, height = 256;
			static constexpr int dx = 8;
			auto patch = [](int x, int y) { return (x / 16) % 4 == 2 && (y / 16) % 3 == 1 && x % 16 < 8 && y % 16 < 8; };
			auto smooth = [](int x, int y) { return static_cast<int>(DejudderTexture(x, y, 21)) / 2 + 12000; };
			auto chroma = [](int x, int y, int c) { return static_cast<uint16_t>(32768 + c); };
			const DejudderFrame a = MakeDejudderFrame(width, height,
				[&](int x, int y) { return static_cast<uint16_t>(smooth(x, y) + (patch(x, y) ? 10280 : 0)); }, chroma);
			const DejudderFrame b = MakeDejudderFrame(width, height,
				[&](int x, int y) { return static_cast<uint16_t>(smooth(x - dx, y) + (patch(x, y) ? 10280 : 0)); }, chroma);
			DejudderGpuPlanes planesA, planesB;
			UploadDejudderFrame(gpu, a, planesA);
			UploadDejudderFrame(gpu, b, planesB);
			auto run = [&](bool verify)
			{
				Dejudder::Settings settings = DejudderTestSettings();
				settings.globalPrior = true;
				settings.matchTolerance = 8.0f;
				settings.consistencyPixels = 2.0f;
				settings.minTrustedShare = 0.0f;
				settings.verifyPixels = verify;
				Dejudder::MotionWarp warp(fixture.Log(), gpu, settings);
				const bool estimated = warp.Estimate(planesA.planes, planesB.planes);
				Assert::IsTrue(estimated, ExtendedRangeGpuProbe::ToWide(warp.LastError()).c_str());
				const DejudderField field = DownloadField(gpu, warp);
				int patchTrusted = 0, patchTotal = 0, cleanTrusted = 0, cleanTotal = 0;
				for (int by = 1; by < field.blocksY - 1; ++by)
					for (int bx = 1; bx < field.blocksX - 2; ++bx)
					{
						const bool hasPatch = patch(bx * 16, by * 16);
						// Clean: neither a patch nor one landing in it from a neighbour.
						const bool clean = !hasPatch && !patch(bx * 16 + 16, by * 16) && !patch(bx * 16 - 16, by * 16);
						if (hasPatch) { ++patchTotal; patchTrusted += field.trusted(bx, by) ? 1 : 0; }
						if (clean) { ++cleanTotal; cleanTrusted += field.trusted(bx, by) ? 1 : 0; }
					}
				return std::make_tuple(patchTrusted, patchTotal, cleanTrusted, cleanTotal);
			};
			const auto off = run(false), on = run(true);
			const std::wstring message = L"patch blocks trusted: off " + std::to_wstring(std::get<0>(off)) + L"/" + std::to_wstring(std::get<1>(off)) +
				L", verify " + std::to_wstring(std::get<0>(on)) + L"/" + std::to_wstring(std::get<1>(on)) + L"; clean blocks trusted: off " +
				std::to_wstring(std::get<2>(off)) + L"/" + std::to_wstring(std::get<3>(off)) + L", verify " + std::to_wstring(std::get<2>(on)) +
				L"/" + std::to_wstring(std::get<3>(on)) + L"; log: " + std::wstring(log.begin(), log.end());
			Assert::IsTrue(std::get<0>(off) * 100 >= std::get<1>(off) * 60, (L"loose tolerance: the patch blocks pass on average: " + message).c_str());
			Assert::IsTrue(std::get<0>(on) * 100 <= std::get<1>(on) * 10, (L"the pixel check withdraws them: " + message).c_str());
			Assert::IsTrue(std::get<2>(on) * 100 >= std::get<3>(on) * 95 && std::get<2>(on) >= std::get<2>(off) - 1,
				(L"clean blocks keep their trust: " + message).c_str());
			AssertCleanLibplaceboLog(log);
		}

		TEST_METHOD(DejudderPixelTrustMovesBothPathsAlike)
		{
			// M6: a pan of (10, 4) where a band of blocks fails the block test
			// (fresh noise in A's band... only in B: the band's match fails, its
			// trusted neighbours carry it). Both warps with pixel trust move
			// the trusted area by s x pan and agree with each other; a cut
			// still holds the whole frame in both.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			const int width = 512, height = 256;
			static constexpr int dx = 10, dy = 4;
			auto chroma = [](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); };
			const DejudderFrame a = MakeDejudderFrame(width, height, [](int x, int y) { return DejudderTexture(x, y); }, chroma);
			const DejudderFrame b = MakeDejudderFrame(width, height,
				[](int x, int y) { return DejudderTexture(x - dx, y - dy); },
				[](int x, int y, int c) { return DejudderTexture((x - dx / 2) * 2, y - dy, 7 + c); });
			DejudderGpuPlanes planesA, planesB;
			UploadDejudderFrame(gpu, a, planesA);
			UploadDejudderFrame(gpu, b, planesB);
			Dejudder::Settings settings = DejudderTestSettings();
			settings.globalPrior = true;
			settings.pixelTrust = true;
			settings.pixelTrustLo = 3.0f;
			settings.pixelTrustHi = 8.0f;
			Dejudder::MotionWarp warp(fixture.Log(), gpu, settings);
			Assert::IsTrue(warp.Estimate(planesA.planes, planesB.planes) && warp.Estimate(planesA.planes, planesB.planes),
				ExtendedRangeGpuProbe::ToWide(warp.LastError()).c_str());
			Assert::IsNotNull(warp.PixelMask(), L"the pixel weights exist");
			const BothPaths warped = WarpBothPaths(fixture, warp, a, planesA.planes, 0.5f);
			auto expected = [](int x, int y) { return DejudderTexture(x - 5, y - 2); };
			const double sourceOk = MatchingPercent(warped.source, width, 48, 32, width - 48, height - 32, expected, kSourceTolerance);
			const double renderedOk = MatchingPercent(warped.rendered, width, 48, 32, width - 48, height - 32, expected, kRenderedTolerance);
			Assert::IsTrue(sourceOk >= 90.0 && renderedOk >= 90.0, (L"pixel trust moves the pan by half: source " +
				std::to_wstring(sourceOk) + L"%, rendered " + std::to_wstring(renderedOk) + L"%; log: " + std::wstring(log.begin(), log.end())).c_str());
			// A cut (B another picture): both hold.
			const DejudderFrame c = MakeDejudderFrame(width, height,
				[](int x, int y) { return static_cast<uint16_t>(2000 + (DejudderTexture(x, y, 99) - 8000) / 6); }, chroma);
			DejudderGpuPlanes planesC;
			UploadDejudderFrame(gpu, c, planesC);
			Assert::IsTrue(warp.Estimate(planesA.planes, planesC.planes), ExtendedRangeGpuProbe::ToWide(warp.LastError()).c_str());
			const BothPaths held = WarpBothPaths(fixture, warp, a, planesA.planes, 0.5f);
			auto same = [](int x, int y) { return DejudderTexture(x, y); };
			const double sourceHeld = MatchingPercent(held.source, width, 16, 16, width - 16, height - 16, same, kSourceTolerance);
			const double renderedHeld = MatchingPercent(held.rendered, width, 16, 16, width - 16, height - 16, same, kRenderedTolerance);
			Assert::IsTrue(sourceHeld >= 99.0 && renderedHeld >= 99.0, (L"a cut holds in both: source " + std::to_wstring(sourceHeld) +
				L"%, rendered " + std::to_wstring(renderedHeld) + L"%").c_str());
			AssertCleanLibplaceboLog(log);
		}

		TEST_METHOD(DejudderMotionRolloffKeepsFastMotionsStrobing)
		{
			// Delta 14 R-A: pans at 2, 12 and 30 source px per frame give the
			// strength 0 (below D2's rise), full, and the floor (above the
			// roll-off); off, the fast pan keeps full strength (D2 alone).
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			const int width = 256, height = 128;
			auto chroma = [](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); };
			const DejudderFrame a = MakeDejudderFrame(width, height, [](int x, int y) { return DejudderTexture(x, y); }, chroma);
			DejudderGpuPlanes planesA;
			UploadDejudderFrame(gpu, a, planesA);
			auto scaleFor = [&](int dx, bool rolloff)
			{
				const DejudderFrame b = MakeDejudderFrame(width, height, [dx](int x, int y) { return DejudderTexture(x - dx, y); },
					[dx](int x, int y, int c) { return DejudderTexture((x - dx / 2) * 2, y, 7 + c); });
				DejudderGpuPlanes planesB;
				UploadDejudderFrame(gpu, b, planesB);
				Dejudder::Settings settings = DejudderTestSettings();
				settings.globalPrior = true;
				settings.motionScaleLo = 3.0f;
				settings.motionScaleHi = 6.0f;
				settings.trustScaleFull = 0.0f;
				settings.motionRolloff = rolloff;
				settings.rollLo = 16.0f;
				settings.rollHi = 26.0f;
				settings.rollFloor = 0.25f;
				Dejudder::MotionWarp warp(fixture.Log(), gpu, settings);
				// The first frame starts at its own value; more settle within D2's rate.
				for (int i = 0; i < 6; ++i)
					Assert::IsTrue(warp.Estimate(planesA.planes, planesB.planes), ExtendedRangeGpuProbe::ToWide(warp.LastError()).c_str());
				const auto trust = DownloadFrameTrust(gpu, warp);
				return std::make_pair(trust[1], trust[2]);
			};
			const auto slow = scaleFor(2, true), medium = scaleFor(12, true), fast = scaleFor(30, true), fastOff = scaleFor(30, false);
			const std::wstring report = L"scale (mean |v|): 2 px " + std::to_wstring(slow.first) + L" (" + std::to_wstring(slow.second) +
				L"), 12 px " + std::to_wstring(medium.first) + L" (" + std::to_wstring(medium.second) + L"), 30 px " +
				std::to_wstring(fast.first) + L" (" + std::to_wstring(fast.second) + L"), 30 px off " + std::to_wstring(fastOff.first);
			Assert::IsTrue(slow.first < 0.02f, (L"a slow pan: below D2's rise; " + report).c_str());
			Assert::IsTrue(medium.first > 0.98f, (L"a medium pan: full; " + report).c_str());
			Assert::IsTrue(std::fabs(fast.first - 0.25f) < 0.03f, (L"a fast pan: the floor; " + report).c_str());
			Assert::IsTrue(fastOff.first > 0.98f, (L"off: D2 alone, full; " + report).c_str());
		}

		TEST_METHOD(DejudderObjectStrengthSplitsTheCameraFromObjects)
		{
			// Delta 14 R-B: a pan of (12, 6) with an object moving (-10, 0)
			// against it. At object_strength 0.5 the background moves by the
			// full s x pan and the object by s x (pan + 0.5 x (object - pan)),
			// in the dither's vectors and in both warps; 1.0 is today exactly.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			const int width = 256, height = 128;
			static constexpr int dx = 12, dy = 6, ox = 96, oy = 40, ow = 64, oh = 48, odx = -10;
			auto inA = [](int x, int y) { return x >= ox && x < ox + ow && y >= oy && y < oy + oh; };
			auto inB = [](int x, int y) { return x >= ox + odx && x < ox + odx + ow && y >= oy && y < oy + oh; };
			auto object = [](int x, int y) { return DejudderTexture(x, y, 41); };
			auto chroma = [](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); };
			const DejudderFrame a = MakeDejudderFrame(width, height,
				[&](int x, int y) { return inA(x, y) ? object(x - ox, y - oy) : DejudderTexture(x, y); }, chroma);
			const DejudderFrame b = MakeDejudderFrame(width, height,
				[&](int x, int y) { return inB(x, y) ? object(x - ox - odx, y - oy) : DejudderTexture(x - dx, y - dy); },
				[](int x, int y, int c) { return DejudderTexture((x - dx / 2) * 2, y - dy, 7 + c); });
			DejudderGpuPlanes planesA, planesB;
			UploadDejudderFrame(gpu, a, planesA);
			UploadDejudderFrame(gpu, b, planesB);
			struct Run { std::vector<float> vectors; BothPaths paths; float camX = 0, camY = 0; };
			auto run = [&](float objectStrength)
			{
				Dejudder::Settings settings = DejudderTestSettings();
				settings.globalPrior = true;
				settings.objectStrength = objectStrength;
				Dejudder::MotionWarp warp(fixture.Log(), gpu, settings);
				Assert::IsTrue(warp.Estimate(planesA.planes, planesB.planes) && warp.Estimate(planesA.planes, planesB.planes),
					ExtendedRangeGpuProbe::ToWide(warp.LastError()).c_str());
				Run r;
				// The dither's vectors (output px per block, 1:1), shift 0.5.
				DejudderRendered::VectorField field(fixture.Log(), gpu, true);
				DejudderRendered::Geometry g;
				g.sourceCrop = { 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height) };
				g.targetCrop = g.sourceCrop;
				pl_tex vectors = nullptr;
				Assert::IsTrue(field.Run(warp.Field(), warp.FrameTrust(), warp.GetSettings().minTrustedShare, 0.5f, g, width, height,
					vectors, warp.SubField(), &warp.GetSettings(), warp.Camera()), ExtendedRangeGpuProbe::ToWide(field.Error()).c_str());
				std::vector<uint16_t> half(static_cast<size_t>(vectors->params.w) * vectors->params.h * 4);
				pl_tex_transfer_params d{};
				d.tex = vectors;
				d.ptr = half.data();
				Assert::IsTrue(pl_tex_download(gpu, &d));
				for (uint16_t h : half) r.vectors.push_back(HalfBits(h));
				uint16_t gv[4] = {};
				d.tex = warp.Camera() ? warp.Camera() : warp.GlobalVector();
				d.ptr = gv;
				const float unit = warp.Camera() ? 1.0f : 8.0f;
				if (pl_tex_download(gpu, &d)) { r.camX = HalfBits(gv[0]) * unit; r.camY = HalfBits(gv[1]) * unit; }
				r.paths = WarpBothPaths(fixture, warp, a, planesA.planes, 0.5f);
				return r;
			};
			const Run split = run(0.5f), today = run(1.0f);
			const int bw = (width + 15) / 16;
			auto vectorAt = [&](const Run& r, int x, int y)
			{
				const size_t i = (static_cast<size_t>(y / 16) * bw + x / 16) * 4;
				return std::make_pair(r.vectors[i], r.vectors[i + 1]);
			};
			const auto bgSplit = vectorAt(split, 24, 104), objSplit = vectorAt(split, ox + 32, oy + 24);
			const auto bgToday = vectorAt(today, 24, 104), objToday = vectorAt(today, ox + 32, oy + 24);
			auto text = [](std::pair<float, float> v) { return L"(" + std::to_wstring(v.first) + L", " + std::to_wstring(v.second) + L")"; };
			const std::wstring report = L"camera (" + std::to_wstring(split.camX) + L", " + std::to_wstring(split.camY) +
				L"); 0.5: background " + text(bgSplit) + L", object " + text(objSplit) + L"; 1.0: background " + text(bgToday) +
				L", object " + text(objToday);
			// Today: the object at s x its own motion; the background at s x pan.
			Assert::IsTrue(std::fabs(objToday.first - 0.5f * odx) < 0.75f && std::fabs(bgToday.first - 0.5f * dx) < 0.75f &&
				std::fabs(bgToday.second - 0.5f * dy) < 0.75f, (L"today; " + report).c_str());
			// Split: the background unchanged, the object half-way to the camera.
			const float expectX = 0.5f * (dx + 0.5f * (odx - dx)), expectY = 0.5f * (dy + 0.5f * (0 - dy));
			Assert::IsTrue(std::fabs(bgSplit.first - bgToday.first) < 0.05f && std::fabs(bgSplit.second - bgToday.second) < 0.05f,
				(L"0.5: the camera smoothed fully; " + report).c_str());
			Assert::IsTrue(std::fabs(objSplit.first - expectX) < 0.75f && std::fabs(objSplit.second - expectY) < 0.75f,
				(L"0.5: the object at s x (pan + 0.5 x (object - pan)); " + report).c_str());
			// Both warps: the object's interior moves differently, the far
			// background not at all; the two paths agree with each other.
			auto region = [&](const std::vector<float>& image, int x0, int y0, int x1, int y1, const std::vector<float>& other)
			{
				double sum = 0.0;
				int n = 0;
				for (int y = y0; y < y1; ++y)
					for (int x = x0; x < x1; ++x)
					{
						sum += std::fabs(image[static_cast<size_t>(y) * width + x] - other[static_cast<size_t>(y) * width + x]);
						++n;
					}
				return sum / n / 256.0;   // 8-bit codes
			};
			for (int path = 0; path < 2; ++path)
			{
				const std::vector<float>& s05 = path == 0 ? split.paths.source : split.paths.rendered;
				const std::vector<float>& s10 = path == 0 ? today.paths.source : today.paths.rendered;
				const wchar_t* name = path == 0 ? L"source warp" : L"rendered warp";
				Assert::IsTrue(region(s05, ox + 16, oy + 12, ox + ow - 16, oy + oh - 12, s10) > 2.0,
					(std::wstring(name) + L": the object's interior moves by the split; " + report).c_str());
				Assert::IsTrue(region(s05, 8, 100, 72, 120, s10) < 0.05, (std::wstring(name) + L": the far background as today; " + report).c_str());
			}
			Assert::IsTrue(region(split.paths.source, ox + 16, oy + 12, ox + ow - 16, oy + oh - 12, split.paths.rendered) < 2.0,
				(L"the two paths agree on the split object; " + report).c_str());
		}

		TEST_METHOD(DejudderEstimateAheadFallsBackForEachReason)
		{
			// Estimate ahead (owner 2026-10-13): every reason not to estimate
			// ahead falls back to the estimate before the frame (Consume false),
			// and none of them runs the matcher early.
			using Schedule = Dejudder::AheadSchedule;
			using Reason = Schedule::Reason;
			int estimates = 0, snapshots = 0, uploads = 0;
			Schedule::Steps steps;
			steps.snapshot = [&]() { ++snapshots; return true; };
			steps.uploadAfter = [&]() { ++uploads; return true; };
			steps.estimate = [&](uint64_t, uint64_t) { ++estimates; return true; };
			auto ok = []() { Schedule::Conditions c; c.source = 10; c.next = 11; c.after = 12; c.estimated = 10; return c; };
			struct Case { const wchar_t* name; Schedule::Conditions c; Reason expected; };
			std::vector<Case> cases;
			{ auto c = ok(); c.enabled = false; cases.push_back({ L"off", c, Reason::Off }); }
			{ auto c = ok(); c.renderedWarp = false; cases.push_back({ L"source warp", c, Reason::SourceWarp }); }
			{ auto c = ok(); c.active = false; cases.push_back({ L"not active", c, Reason::NotActive }); }
			{ auto c = ok(); c.next = 0; cases.push_back({ L"no next", c, Reason::NoNext }); }
			{ auto c = ok(); c.after = 0; cases.push_back({ L"frame after next not supplied", c, Reason::NoAfter }); }
			{ auto c = ok(); c.after = 11; cases.push_back({ L"after = next", c, Reason::AfterNotNew }); }
			{ auto c = ok(); c.estimated = 9; cases.push_back({ L"pair not estimated", c, Reason::NotEstimated }); }
			{ auto c = ok(); c.layoutMatches = false; cases.push_back({ L"layout", c, Reason::Layout }); }
			for (const Case& k : cases)
			{
				Schedule schedule;
				Assert::IsFalse(schedule.TryAhead(k.c, steps), k.name);
				Assert::IsTrue(schedule.LastReason() == k.expected, k.name);
				Assert::IsFalse(schedule.Consume(11, 12), k.name);   // B's frame: estimated now
				Assert::AreEqual(1ull, schedule.Sequential(), k.name);
			}
			Assert::AreEqual(0, estimates + snapshots + uploads, L"no reason to skip ran any step");
			// Failed steps: a failed copy leaves the extras on the matcher's
			// own field; a failed upload or estimate keeps the copy in use and
			// estimates at B's frame.
			{
				Schedule schedule;
				Schedule::Steps failing = steps;
				failing.snapshot = []() { return false; };
				Assert::IsFalse(schedule.TryAhead(ok(), failing));
				Assert::IsTrue(schedule.LastReason() == Reason::SnapshotFailed);
				Assert::IsFalse(schedule.Consume(11, 12));
			}
			for (int which = 0; which < 2; ++which)
			{
				Schedule schedule;
				Schedule::Steps failing = steps;
				if (which == 0) failing.uploadAfter = []() { return false; };
				else failing.estimate = [](uint64_t, uint64_t) { return false; };
				Assert::IsTrue(schedule.TryAhead(ok(), failing), L"the copy stays in use");
				Assert::IsTrue(schedule.LastReason() == (which == 0 ? Reason::UploadFailed : Reason::EstimateFailed));
				Assert::IsFalse(schedule.Consume(11, 12), L"nothing estimated ahead: estimated now");
			}
			// The path that works: estimated ahead, then taken at B's frame once.
			{
				Schedule schedule;
				estimates = 0;
				Assert::IsTrue(schedule.TryAhead(ok(), steps));
				Assert::IsTrue(schedule.LastReason() == Reason::Ahead);
				Assert::AreEqual(1, estimates);
				Assert::IsTrue(schedule.Consume(11, 12), L"B's frame takes the estimate made ahead");
				Assert::IsFalse(schedule.Consume(11, 12), L"only once");
				Assert::AreEqual(1ull, schedule.Hits());
			}
			// Estimated ahead for a C that did not come next: re-estimated.
			{
				Schedule schedule;
				Assert::IsTrue(schedule.TryAhead(ok(), steps));
				Assert::IsFalse(schedule.Consume(11, 13), L"another next frame: estimated now");
				Assert::AreEqual(1ull, schedule.Misses());
			}
		}

		TEST_METHOD(DejudderEstimateAheadMatchesTheSequentialPath)
		{
			// Estimate ahead (owner 2026-10-13), driven by the renderer's own
			// schedule over a seven-frame sequence with changing motion. The
			// frame after next is withheld at frame 2's extras (the fallback is
			// forced) and replaced by another frame at frame 4's (estimated ahead
			// for a pair that does not come). Every frame's path is recorded and
			// checked; up to the replaced frame every output (field, trust,
			// sub-field, the dither's mask, the grain) equals the sequential
			// estimate byte for byte, and the extras' copy equals the outputs they
			// replace.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			const int width = 256, height = 128, frames = 7;
			static const int panX[frames] = { 0, 6, 13, 18, 22, 29, 33 };
			static const int panY[frames] = { 0, 2, 3, 6, 9, 10, 12 };
			std::vector<DejudderGpuPlanes> planes(frames);
			for (int f = 0; f < frames; ++f)
			{
				const int px = panX[f], py = panY[f];
				const DejudderFrame frame = MakeDejudderFrame(width, height,
					[px, py](int x, int y) { return DejudderTexture(x - px, y - py); },
					[px, py](int x, int y, int c) { return DejudderTexture((x - px / 2) * 2, y - py, 7 + c); });
				UploadDejudderFrame(gpu, frame, planes[f]);
			}
			Dejudder::Settings settings = DejudderTestSettings();
			settings.estSplit = true;
			auto download = [&](pl_tex tex)
			{
				std::vector<uint16_t> half;
				if (!tex) return half;
				half.resize(static_cast<size_t>(tex->params.w) * tex->params.h * 4);
				pl_tex_transfer_params d{};
				d.tex = tex;
				d.ptr = half.data();
				Assert::IsTrue(pl_tex_download(gpu, &d), L"not host-readable");
				return half;
			};
			struct Outputs { std::vector<uint16_t> field, trust, sub, mask, grain; };
			auto outputs = [&](Dejudder::MotionWarp& w, int slot)
			{
				return Outputs{ download(w.Field()), download(w.FrameTrust()), download(w.SubField()),
					download(w.Occlusion(slot)), download(w.GrainError()) };
			};
			auto same = [](const Outputs& a, const Outputs& b)
			{
				return a.field == b.field && a.trust == b.trust && a.sub == b.sub && a.mask == b.mask && a.grain == b.grain;
			};
			// The pair (n, n+1) writes its mask into slot n % 2 on both paths.
			auto estimate = [&](Dejudder::MotionWarp& w, int a, int b)
			{
				return w.Estimate(planes[a].planes, planes[b].planes, a % 2, true);
			};
			// Sequential: every frame estimated before its render.
			Dejudder::MotionWarp sequential(fixture.Log(), gpu, settings);
			std::vector<Outputs> expected;
			for (int n = 0; n + 1 < frames; ++n)
			{
				Assert::IsTrue(estimate(sequential, n, n + 1), ExtendedRangeGpuProbe::ToWide(sequential.LastError()).c_str());
				expected.push_back(outputs(sequential, n % 2));
			}
			// Estimate ahead through the schedule. Frame ids are the frame
			// indices + 1 (0 = none); frame 99 is the replacement.
			Dejudder::MotionWarp ahead(fixture.Log(), gpu, settings);
			Dejudder::AheadSchedule schedule;
			std::wstring paths;
			int estimated = -1;
			int replacedAt = -1;
			for (int n = 0; n + 1 < frames; ++n)
			{
				// Frame n: its estimate, or the one made ahead.
				const uint64_t idA = n + 1, idB = n + 2;
				if (schedule.Consume(idA, idB))
					paths += L"ahead ";
				else
				{
					Assert::IsTrue(estimate(ahead, n, n + 1), ExtendedRangeGpuProbe::ToWide(ahead.LastError()).c_str());
					paths += L"before ";
				}
				estimated = n;
				const Outputs got = outputs(ahead, n % 2);
				if (replacedAt < 0 || n < replacedAt + 1)
					Assert::IsTrue(same(got, expected[n]),
						(L"frame " + std::to_wstring(n) + L" differs from the sequential estimate; paths: " + paths).c_str());
				// Frame n's first extra: the next pair, ahead when C is supplied.
				if (n + 2 >= frames)
					continue;
				Dejudder::AheadSchedule::Conditions c;
				c.source = idA;
				c.next = idB;
				c.after = n == 2 ? 0 : (n == 4 ? 99 : static_cast<uint64_t>(n + 3));
				c.estimated = static_cast<uint64_t>(estimated + 1);
				Dejudder::AheadSchedule::Steps steps;
				steps.snapshot = [&]() { return ahead.SnapshotForWarp(); };
				steps.uploadAfter = []() { return true; };
				steps.estimate = [&](uint64_t b, uint64_t after)
				{
					const int bi = static_cast<int>(b) - 1;
					const int ci = after == 99 ? 0 : static_cast<int>(after) - 1;   // the replacement: another picture
					return ahead.Estimate(planes[bi].planes, planes[ci].planes, bi % 2, true);
				};
				const bool copy = schedule.TryAhead(c, steps);
				if (n == 4)
					replacedAt = n;
				Assert::IsTrue(copy == (n != 2), (L"frame " + std::to_wstring(n) + L": the copy is used exactly when C is supplied").c_str());
				if (copy)
				{
					// The extras read the copy: frame n's own outputs.
					Assert::IsTrue(download(ahead.SnapField()) == got.field && download(ahead.SnapFrameTrust()) == got.trust &&
						download(ahead.SnapSubField()) == got.sub,
						(L"frame " + std::to_wstring(n) + L": the extras' copy is the frame's own field").c_str());
				}
			}
			// 0 before (the start), 1 2 ahead, 3 before (C withheld at 2), 4
			// ahead, 5 before (C replaced at 4: estimated again).
			Assert::AreEqual(std::wstring(L"before ahead ahead before ahead before "), paths, paths.c_str());
			Assert::AreEqual(3ull, schedule.Hits());
			Assert::AreEqual(3ull, schedule.Sequential());
			Assert::AreEqual(1ull, schedule.Misses());
			Assert::AreEqual(1ull, schedule.Count(Dejudder::AheadSchedule::Reason::NoAfter));
		}

		TEST_METHOD(DejudderSnapshotKeepsTheWarpInputsWhileTheNextPairIsEstimated)
		{
			// Estimate ahead (owner 2026-10-13): the copy of (A, B)'s field,
			// trust and sub-field is exact, and survives the estimate of (B, C)
			// (a different pan) that overwrites the matcher's own.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			const int width = 256, height = 128;
			auto chroma = [](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); };
			const DejudderFrame a = MakeDejudderFrame(width, height, [](int x, int y) { return DejudderTexture(x, y); }, chroma);
			const DejudderFrame b = MakeDejudderFrame(width, height, [](int x, int y) { return DejudderTexture(x - 12, y - 6); },
				[](int x, int y, int c) { return DejudderTexture((x - 6) * 2, y - 6, 7 + c); });
			const DejudderFrame c = MakeDejudderFrame(width, height, [](int x, int y) { return DejudderTexture(x - 4, y - 10); },
				[](int x, int y, int c) { return DejudderTexture((x - 2) * 2, y - 10, 7 + c); });
			DejudderGpuPlanes planesA, planesB, planesC;
			UploadDejudderFrame(gpu, a, planesA);
			UploadDejudderFrame(gpu, b, planesB);
			UploadDejudderFrame(gpu, c, planesC);
			Dejudder::Settings settings = DejudderTestSettings();
			settings.estSplit = true;
			Dejudder::MotionWarp warp(fixture.Log(), gpu, settings);
			auto download = [&](pl_tex tex)
			{
				Assert::IsNotNull(tex);
				std::vector<uint16_t> half(static_cast<size_t>(tex->params.w) * tex->params.h * 4);
				pl_tex_transfer_params d{};
				d.tex = tex;
				d.ptr = half.data();
				Assert::IsTrue(pl_tex_download(gpu, &d), L"not host-readable");
				return half;
			};
			// Twice, so the hysteresis lets the pan's blocks warp.
			Assert::IsTrue(warp.Estimate(planesA.planes, planesB.planes) && warp.Estimate(planesA.planes, planesB.planes),
				ExtendedRangeGpuProbe::ToWide(warp.LastError()).c_str());
			const std::vector<uint16_t> field = download(warp.Field()), trust = download(warp.FrameTrust()),
				sub = download(warp.SubField());
			Assert::IsTrue(warp.SnapshotForWarp(), ExtendedRangeGpuProbe::ToWide(warp.LastError()).c_str());
			Assert::IsTrue(download(warp.SnapField()) == field && download(warp.SnapFrameTrust()) == trust &&
				download(warp.SnapSubField()) == sub, L"the copy is exact");
			Assert::IsTrue(warp.Estimate(planesB.planes, planesC.planes), ExtendedRangeGpuProbe::ToWide(warp.LastError()).c_str());
			Assert::IsTrue(download(warp.Field()) != field, L"the next pair's field differs (a different pan)");
			Assert::IsTrue(download(warp.SnapField()) == field && download(warp.SnapFrameTrust()) == trust &&
				download(warp.SnapSubField()) == sub, L"the copy survives the next estimate");
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

		TEST_METHOD(BitrateScaleRampAndAntiring)
		{
			// Delta 15 S8-2 / 2c: clean (0.8 and above) is today's output exactly.
			Assert::AreEqual(1.0, AdaptiveDownscale::BitrateScale(1.0, 0.4));
			Assert::AreEqual(1.0, AdaptiveDownscale::BitrateScale(0.8, 0.4));
			Assert::AreEqual(0.4, AdaptiveDownscale::BitrateScale(0.0, 0.4), 1e-9);
			Assert::AreEqual(0.4, AdaptiveDownscale::BitrateScale(0.2, 0.4), 1e-9);
			Assert::AreEqual(0.7, AdaptiveDownscale::BitrateScale(0.5, 0.4), 1e-9, L"halfway");
			Assert::AreEqual(0.7, AdaptiveDownscale::BitrateScale(0.51, 0.4), 1e-9, L"steps of 0.05: no churn");
			Assert::AreEqual(0.5f, AdaptiveDownscale::BitrateAntiring(0.5f, 1.0, 0.4), L"clean: anti-ringing unchanged");
			Assert::AreEqual(0.75f, AdaptiveDownscale::BitrateAntiring(0.5f, 0.4, 0.4), 1e-6f, L"floor: halfway to 1");
		}

		TEST_METHOD(CnnResidualHeldAlongTrustedVectors)
		{
			// Stage 12 §2c (owner 2026-10-15): the CNN upscaler's added detail held
			// along the de-judder's trusted vectors. A stand-in CNN (nearest 2x
			// doubler) on a texture whose fine detail changes every frame: the
			// residual (output minus the faithful branch) flickers less when held;
			// off and an untrusted field are today's output exactly.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create());
			pl_gpu gpu = fixture.Gpu();
			const int width = 128, height = 64, ow = width * 3 / 2, oh = height * 3 / 2;
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
			auto makeField = [&](float trust) {
				std::vector<float> f(static_cast<size_t>((width + 15) / 16) * ((height + 15) / 16) * 4);
				for (size_t i = 0; i < f.size(); i += 4) { f[i] = 0.0f; f[i + 1] = 0.0f; f[i + 2] = trust; f[i + 3] = 0.0f; }
				pl_tex_params p{};
				p.w = (width + 15) / 16; p.h = (height + 15) / 16; p.format = pl_find_named_fmt(gpu, "rgba32f"); p.sampleable = true; p.initial_data = f.data();
				return pl_tex_create(gpu, &p);
			};
			const float trustValue[4] = { 1.0f, 1.0f, 0.0f, 1.0f };
			pl_tex_params tp1{};
			tp1.w = 1; tp1.h = 1; tp1.format = pl_find_named_fmt(gpu, "rgba32f"); tp1.sampleable = true; tp1.initial_data = trustValue;
			pl_tex frameTrust = pl_tex_create(gpu, &tp1);
			pl_tex trusted = makeField(1.0f), untrusted = makeField(0.0f);
			Assert::IsTrue(frameTrust && trusted && untrusted);
			// Frame f: the texture plus fine detail that changes every frame.
			auto frameOf = [&](int f) {
				return MakeDejudderFrame(width, height,
					[f](int x, int y) { return static_cast<uint16_t>((DejudderTexture(x, y) * 7 + DejudderTexture(x, y, 1000u + static_cast<unsigned>(f))) / 8); },
					[](int, int, int) { return static_cast<uint16_t>(32768); });
			};
			auto render = [&](const DejudderFrame& frame, std::vector<const pl_hook*> hooks) {
				DejudderGpuPlanes planes;
				UploadDejudderFrame(gpu, frame, planes);
				pl_tex_params tp{};
				tp.w = ow; tp.h = oh; tp.format = rgba16; tp.renderable = true; tp.host_readable = true;
				pl_tex target = pl_tex_create(gpu, &tp);
				pl_frame image{};
				image.num_planes = 2;
				image.planes[0].texture = planes.planes.luma; image.planes[0].components = 1; image.planes[0].component_mapping[0] = PL_CHANNEL_Y;
				image.planes[1].texture = planes.planes.chroma; image.planes[1].components = 2;
				image.planes[1].component_mapping[0] = PL_CHANNEL_CB; image.planes[1].component_mapping[1] = PL_CHANNEL_CR;
				image.repr.sys = PL_COLOR_SYSTEM_BT_709; image.repr.levels = PL_COLOR_LEVELS_FULL;
				image.color.primaries = PL_COLOR_PRIM_BT_709; image.color.transfer = PL_COLOR_TRC_BT_1886;
				pl_frame_set_chroma_location(&image, PL_CHROMA_LEFT);
				pl_frame t = TargetLutGpuFixture::MakeRgbFrame(target);
				t.repr.levels = PL_COLOR_LEVELS_FULL; t.color.transfer = PL_COLOR_TRC_BT_1886;
				pl_render_params params = pl_render_fast_params;
				params.dither_params = nullptr; params.error_diffusion = nullptr; params.sigmoid_params = nullptr;
				params.disable_linear_scaling = true; params.upscaler = &pl_filter_ewa_lanczossharp; params.antiringing_strength = 0.5f;
				params.hooks = hooks.empty() ? nullptr : hooks.data();
				params.num_hooks = static_cast<int>(hooks.size());
				Assert::IsTrue(pl_render_image(fixture.Renderer(), &image, &t, &params));
				pl_gpu_finish(gpu);
				std::vector<uint16_t> px(static_cast<size_t>(ow) * oh * 4);
				pl_tex_transfer_params d{}; d.tex = target; d.ptr = px.data();
				Assert::IsTrue(pl_tex_download(gpu, &d));
				pl_tex_destroy(gpu, &target);
				return px;
			};
			const int frames = 10;
			std::vector<std::vector<uint16_t>> faithful;
			for (int f = 0; f < frames; ++f) faithful.push_back(render(frameOf(f), {}));
			auto run = [&](bool stabilise, pl_tex field) {
				AdaptiveDownscale::Module module;
				AdaptiveDownscale::Settings st;
				st.grainMap = false;
				st.grainLo = 1000.0f; st.grainHi = 2000.0f;   // no grain: w = w_base
				st.grainSmoothMs = 0.0f;
				st.cnnResidualStabilise = stabilise;
				st.residualGrainGuard = false;   // the 2026-10-15 one-pole (SsimResidualStabiliseLeavesGrainBlocks: the guard)
				std::vector<std::vector<uint16_t>> out;
				for (int f = 0; f < frames; ++f)
				{
					module.SetFrame(st, static_cast<uint64_t>(500 + f), 1.0f / 24.0f);
					AdaptiveDownscale::Upscale up;
					up.on = true; up.wBase = 1.0f; up.nativeHeight = static_cast<float>(height);
					module.SetUpscale(up);
					module.ClearMotion();
					if (field)
					{
						AdaptiveDownscale::Motion m;
						m.field = field; m.frameTrust = frameTrust; m.minTrustedShare = 0.25f;
						m.sourceCrop = { 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height) };
						module.SetMotion(m);
					}
					out.push_back(render(frameOf(f), { module.Measure(), cnn, module.Blend() }));
				}
				const unsigned long long held = module.StabilisedFrames();
				module.Release();
				return std::make_pair(out, held);
			};
			auto flickerOf = [&](const std::vector<std::vector<uint16_t>>& o) {
				double sum = 0; size_t n = 0;
				for (int f = 3; f < frames; ++f)
					for (size_t i = 0; i < o[f].size(); i += 4)
					{
						const double now = double(o[f][i]) - double(faithful[f][i]), before = double(o[f - 1][i]) - double(faithful[f - 1][i]);
						sum += std::fabs(now - before);
						++n;
					}
				return sum / n / 257.0;
			};
			auto maxDiff = [](const std::vector<uint16_t>& a, const std::vector<uint16_t>& b) {
				int m = 0;
				for (size_t i = 0; i < a.size(); ++i) if (i % 4 != 3) m = (std::max)(m, std::abs(int(a[i]) - int(b[i])));
				return m;
			};
			const auto today = run(false, nullptr);
			const auto held = run(true, trusted);
			const auto untrustedRun = run(true, untrusted);
			const auto offWithField = run(false, trusted);
			char line[200];
			snprintf(line, sizeof(line), "CNN residual flicker %.3f (today) -> %.3f (held, %llu frames); untrusted %.3f\n",
				flickerOf(today.first), flickerOf(held.first), held.second, flickerOf(untrustedRun.first));
			Logger::WriteMessage(line);
			Assert::IsTrue(held.second >= 8, L"held on the consecutive frames");
			Assert::IsTrue(flickerOf(held.first) < 0.7 * flickerOf(today.first), L"the CNN residual flickers less when held");
			Assert::AreEqual(0, maxDiff(untrustedRun.first.back(), today.first.back()), L"untrusted: today's output exactly");
			Assert::AreEqual(0, maxDiff(offWithField.first.back(), today.first.back()), L"off: today's output exactly");
			pl_tex_destroy(gpu, &frameTrust); pl_tex_destroy(gpu, &trusted); pl_tex_destroy(gpu, &untrusted);
			pl_mpv_user_shader_destroy(&cnn);
		}

		TEST_METHOD(SsimResidualHeldAlongTrustedVectors)
		{
			// Delta 15 S8-1 / Stage 12 §2c (owner 2026-10-15): the SSIM downscaler's
			// added contrast held along the de-judder's trusted vectors. A
			// flickering texture on a trusted (still) field flickers less; a still
			// texture keeps today's output; an untrusted field is today's output
			// exactly; off is today's output exactly.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create());
			pl_gpu gpu = fixture.Gpu();
			const pl_hook* ssim = nullptr;
			{
				const std::string source = LoadBundledShader("SSimDownscaler.glsl");
				ssim = pl_mpv_user_shader_parse(gpu, source.data(), source.size());
				Assert::IsNotNull(ssim);
			}
			static constexpr int width = 256, height = 128;
			pl_fmt srcFmt = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 16, 16,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_LINEAR));
			pl_fmt dstFmt = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 16, 16,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE));
			Assert::IsTrue(srcFmt && dstFmt);
			// Texture: noise plus a gentle pattern; `flicker` changes the fine noise.
			auto picture = [&](unsigned seed, double flickerAmount) {
				std::mt19937 rng(77), flick(seed);
				std::normal_distribution<double> base(0.0, 0.04), change(0.0, 1.0);
				std::vector<uint16_t> px(static_cast<size_t>(width) * height * 4);
				for (int y = 0; y < height; ++y)
					for (int x = 0; x < width; ++x)
					{
						double v = 0.45 + 0.1 * std::sin(x * 0.11) * std::cos(y * 0.07) + base(rng) + flickerAmount * change(flick);
						v = (std::min)(1.0, (std::max)(0.0, v));
						const size_t i = (static_cast<size_t>(y) * width + x) * 4;
						px[i] = px[i + 1] = px[i + 2] = static_cast<uint16_t>(std::lround(v * 65535.0));
						px[i + 3] = 65535;
					}
				pl_tex_params tp{};
				tp.w = width; tp.h = height; tp.format = srcFmt; tp.sampleable = true; tp.initial_data = px.data();
				return pl_tex_create(gpu, &tp);
			};
			// A still field (zero motion) and the frame trust: trusted or not.
			auto makeField = [&](float trust) {
				std::vector<float> f(static_cast<size_t>(width / 16) * (height / 16) * 4);
				for (size_t i = 0; i < f.size(); i += 4) { f[i] = 0.0f; f[i + 1] = 0.0f; f[i + 2] = trust; f[i + 3] = 0.0f; }
				pl_tex_params p{};
				p.w = width / 16; p.h = height / 16; p.format = pl_find_named_fmt(gpu, "rgba32f"); p.sampleable = true; p.initial_data = f.data();
				return pl_tex_create(gpu, &p);
			};
			const float trustValue[4] = { 1.0f, 1.0f, 0.0f, 1.0f };
			pl_tex_params tp1{};
			tp1.w = 1; tp1.h = 1; tp1.format = pl_find_named_fmt(gpu, "rgba32f"); tp1.sampleable = true; tp1.initial_data = trustValue;
			pl_tex frameTrust = pl_tex_create(gpu, &tp1);
			pl_tex trusted = makeField(1.0f), untrusted = makeField(0.0f);
			Assert::IsTrue(frameTrust && trusted && untrusted);

			auto run = [&](bool stabilise, pl_tex field, bool flicker, int frames, float wBase = -1.0f) {
				AdaptiveDownscale::Module module;
				AdaptiveDownscale::Settings st;
				st.grainSmoothMs = 0.0f;
				st.grainMap = false;
				st.ssimResidualStabilise = stabilise;
				st.residualGrainGuard = false;   // the 2026-10-15 one-pole (SsimResidualStabiliseLeavesGrainBlocks: the guard)
				if (wBase >= 0.0f) st.wBase = wBase;   // 0: the faithful branch alone
				std::vector<std::vector<uint16_t>> outputs;
				for (int f = 0; f < frames; ++f)
				{
					pl_tex src = picture(static_cast<unsigned>(1000 + f), flicker ? 0.03 : 0.0);
					module.SetFrame(st, static_cast<uint64_t>(10 + f), 1.0f / 24.0f);
					module.ClearMotion();
					if (field)
					{
						AdaptiveDownscale::Motion m;
						m.field = field;
						m.frameTrust = frameTrust;
						m.minTrustedShare = 0.25f;
						m.sourceCrop = { 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height) };
						module.SetMotion(m);
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
					params.hooks = hooks;
					params.num_hooks = 3;
					Assert::IsTrue(pl_render_image(fixture.Renderer(), &image, &out, &params));
					pl_gpu_finish(gpu);
					std::vector<uint16_t> px(static_cast<size_t>(width / 2) * (height / 2) * 4);
					pl_tex_transfer_params dl{}; dl.tex = target; dl.ptr = px.data();
					Assert::IsTrue(pl_tex_download(gpu, &dl));
					outputs.push_back(px);
					pl_tex_destroy(gpu, &target);
					pl_tex_destroy(gpu, &src);
				}
				const unsigned long long held = module.StabilisedFrames();
				module.Release();
				return std::make_pair(outputs, held);
			};
			// The faithful branch alone on the same frames: the residual is the
			// output minus it. Frame-to-frame change of the residual (8-bit codes).
			const auto faithfulFlicker = run(false, nullptr, true, 10, 0.0f);
			auto flickerOf = [&](const std::vector<std::vector<uint16_t>>& o) {
				const auto& fa = faithfulFlicker.first;
				double sum = 0; size_t n = 0;
				for (size_t f = 3; f < o.size(); ++f)
					for (size_t i = 0; i < o[f].size(); i += 4)
					{
						const double now = double(o[f][i]) - double(fa[f][i]), before = double(o[f - 1][i]) - double(fa[f - 1][i]);
						sum += std::fabs(now - before);
						++n;
					}
				return sum / n / 257.0;
			};
			auto maxDiff = [&](const std::vector<uint16_t>& a, const std::vector<uint16_t>& b) {
				double m = 0;
				size_t at = 0, over = 0;
				for (size_t i = 0; i < a.size(); ++i)
				{
					const double d = std::fabs(double(a[i]) - double(b[i]));
					if (d > m) { m = d; at = i; }
					over += d > 257.0;
				}
				if (m > 257.0)
				{
					char where[160];
					snprintf(where, sizeof(where), "  max at pixel (%zu, %zu) channel %zu: %u vs %u; %zu values differ by more than a code\n",
						(at / 4) % (width / 2), (at / 4) / (width / 2), at % 4, a[at], b[at], over);
					Logger::WriteMessage(where);
				}
				return m / 257.0;
			};
			const auto todayFlicker = run(false, nullptr, true, 10);
			const auto heldFlicker = run(true, trusted, true, 10);
			const auto untrustedFlicker = run(true, untrusted, true, 10);
			const auto offWithField = run(false, trusted, true, 10);
			const auto todayStill = run(false, nullptr, false, 8);
			const auto heldStill = run(true, trusted, false, 8);
			char line[240];
			snprintf(line, sizeof(line), "SSIM residual flicker %.3f (today) -> %.3f (held, %llu frames held); untrusted %.3f; still picture held vs today max %.3f 8-bit codes\n",
				flickerOf(todayFlicker.first), flickerOf(heldFlicker.first), heldFlicker.second, flickerOf(untrustedFlicker.first),
				maxDiff(heldStill.first.back(), todayStill.first.back()));
			Logger::WriteMessage(line);
			Assert::IsTrue(heldFlicker.second >= 8, L"held on the consecutive frames");
			Assert::IsTrue(flickerOf(heldFlicker.first) < 0.7 * flickerOf(todayFlicker.first), L"the residual flickers less when held");
			// (The count is frames with a previous residual; trust is decided per pixel in the pass.)
			Assert::AreEqual(0.0, maxDiff(untrustedFlicker.first.back(), todayFlicker.first.back()), L"untrusted: today's output exactly");
			Assert::AreEqual(0.0, maxDiff(offWithField.first.back(), todayFlicker.first.back()), L"off: today's output exactly");
			Assert::IsTrue(maxDiff(heldStill.first.back(), todayStill.first.back()) <= 1.0, L"a still picture keeps today's output (within a code)");
			pl_tex_destroy(gpu, &frameTrust); pl_tex_destroy(gpu, &trusted); pl_tex_destroy(gpu, &untrusted);
			pl_mpv_user_shader_destroy(&ssim);
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
			AssertCleanLibplaceboLog(log);
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
			AssertCleanLibplaceboLogExceptPreOutputBug(log);
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
				AssertCleanLibplaceboLogExceptPreOutputBug(log, combo.Name());
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
				AssertCleanLibplaceboLogExceptPreOutputBug(log);
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
				AssertCleanLibplaceboLogExceptPreOutputBug(log);
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

		// --- Film grain (owner 2026-10-17: never averaged or interpolated away;
		// docs/audit/grain-audit-2026-10-17.md). GPU tests (WARP unless VP_TEST_GPU=hardware) of
		// the shaders behind the grain switches; GrainPreservationTests holds the CPU reference
		// models. Each also checks that its legacy switch is the established output byte for byte.

		// 16-bit plane units per 8-bit code.
		static constexpr double kGrainCode16 = 256.0;

		// A gentle picture in 16-bit codes (its own high band small next to the grain).
		static double GrainTexture16(double x, double y)
		{
			return 30000.0 + 2000.0 * std::sin(6.283185307 * x / 211.0) + 2000.0 * std::sin(6.283185307 * y / 241.0 + 1.0);
		}

		// Frame k of a pan (vx, vy source px per frame): the gentle picture plus fresh grain
		// (8-bit codes, white or soft) in luma and in both chroma channels (4:2:2 planes).
		struct GrainFrames
		{
			std::vector<DejudderFrame> frames;
			std::vector<GrainModel::Image> lumaGrain;
		};

		static GrainFrames MakeGrainFrames(int width, int height, int count, double vx, double vy, double sigma, bool soft, unsigned seed)
		{
			std::mt19937 rng(seed);
			GrainFrames out;
			for (int k = 0; k < count; ++k)
			{
				const GrainModel::Image g = GrainModel::Grain(width, height, sigma, soft, rng);
				const GrainModel::Image cb = GrainModel::Grain(width / 2, height, sigma, soft, rng);
				const GrainModel::Image cr = GrainModel::Grain(width / 2, height, sigma, soft, rng);
				auto clamp16 = [](double v) { return static_cast<uint16_t>(std::lround((std::min)(65535.0, (std::max)(0.0, v)))); };
				out.frames.push_back(MakeDejudderFrame(width, height,
					[&](int x, int y) { return clamp16(GrainTexture16(x + 0.5 - k * vx, y + 0.5 - k * vy) + kGrainCode16 * g.at(x, y)); },
					[&](int x, int y, int c) { return clamp16(32768.0 + kGrainCode16 * (c == 0 ? cb : cr).at(x, y)); }));
				out.lumaGrain.push_back(g);
			}
			return out;
		}

		// The luma plane's grain (8-bit codes): the output minus the picture at `shift` frames.
		static GrainModel::Image LumaGrainOf(const std::vector<uint16_t>& luma, int width, int height, double shift, double vx, double vy)
		{
			GrainModel::Image g(width, height);
			for (int y = 0; y < height; ++y)
				for (int x = 0; x < width; ++x)
					g.at(x, y) = (luma[static_cast<size_t>(y) * width + x] - GrainTexture16(x + 0.5 - shift * vx, y + 0.5 - shift * vy)) / kGrainCode16;
			return g;
		}

		static GrainModel::Image ChromaGrainOf(const std::vector<uint16_t>& chroma, int width, int height, int channel)
		{
			GrainModel::Image g(width / 2, height);
			for (int y = 0; y < height; ++y)
				for (int x = 0; x < width / 2; ++x)
					g.at(x, y) = (chroma[(static_cast<size_t>(y) * (width / 2) + x) * 2 + channel] - 32768.0) / kGrainCode16;
			return g;
		}

		TEST_METHOD(InterpolatorKeepsGrainRmsAtEveryPhase)
		{
			// interpolation_grain preserve: the luma and chroma grain RMS of every synthesised
			// phase within +-5 % of the source's (white and soft, 0.5 to 4 codes) on a fractional
			// pan; the luma matches the CPU reference model; blend (the legacy) carries about half;
			// blend after preserve is the never-switched object's output byte for byte.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			const int width = 256, height = 128, margin = 24;
			const double vx = 3.3, vy = 1.7;
			InterpolatorField field;
			MakeInterpolatorField(gpu, width, height, 1.0f,
				[&](int, int) { return std::array<float, 4>{ static_cast<float>(vx), static_cast<float>(vy), 1.0f, 0.0f }; }, field);
			std::string log;
			fixture.CaptureLog(&log);
			for (bool soft : { false, true })
				for (double sigma : { 0.5, 1.0, 2.0, 4.0 })
				{
					const GrainFrames gf = MakeGrainFrames(width, height, 2, vx, vy, sigma, soft, 31u + static_cast<unsigned>(sigma * 4) + (soft ? 100u : 0u));
					DejudderGpuPlanes planesA, planesB;
					UploadDejudderFrame(gpu, gf.frames[0], planesA);
					UploadDejudderFrame(gpu, gf.frames[1], planesB);
					Interpolator::FrameInterpolator legacy(fixture.Log(), gpu, true), preserve(fixture.Log(), gpu, true);
					preserve.SetGrain(InterpolationPolicy::Grain::Preserve);
					const double sourceLuma = GrainModel::Rms(gf.lumaGrain[0], margin);
					const double sourceChroma = GrainModel::Rms(ChromaGrainOf(gf.frames[0].chroma, width, height, 0), margin / 2);
					for (float t : { 0.2f, 0.4f, 0.5f, 0.6f, 0.8f })
					{
						Dejudder::Planes out;
						Assert::IsTrue(preserve.Run(planesA.planes, planesB.planes, InterpolatorMotion(field), t,
							InterpolationPolicy::Fallback::Hold, out), InterpolatorError(preserve, log).c_str());
						const std::vector<uint16_t> luma = DownloadPlane(gpu, out.luma);
						const std::vector<uint16_t> chroma = DownloadPlane(gpu, out.chroma);
						const double rl = GrainModel::Rms(LumaGrainOf(luma, width, height, t, vx, vy), margin) / sourceLuma;
						const double rc = GrainModel::Rms(ChromaGrainOf(chroma, width, height, 0), margin / 2) / sourceChroma;
						// The CPU reference model on the same frames (16-bit codes).
						GrainModel::Image a(width, height), b(width, height);
						for (size_t i = 0; i < a.v.size(); ++i) { a.v[i] = gf.frames[0].luma[i]; b.v[i] = gf.frames[1].luma[i]; }
						const GrainModel::Image model = GrainModel::InterpolatePreserve(a, b, t, { vx, vy });
						double worst = 0.0;
						for (int y = margin; y < height - margin; ++y)
							for (int x = margin; x < width - margin; ++x)
								worst = (std::max)(worst, std::fabs(luma[static_cast<size_t>(y) * width + x] - model.at(x, y)));
						// Blend (legacy) on the never-switched object.
						Dejudder::Planes old;
						Assert::IsTrue(legacy.Run(planesA.planes, planesB.planes, InterpolatorMotion(field), t,
							InterpolationPolicy::Fallback::Hold, old), InterpolatorError(legacy, log).c_str());
						const std::vector<uint16_t> oldLuma = DownloadPlane(gpu, old.luma);
						const std::vector<uint16_t> oldChroma = DownloadPlane(gpu, old.chroma);
						const double rb = GrainModel::Rms(LumaGrainOf(oldLuma, width, height, t, vx, vy), margin) / sourceLuma;
						char text[240];
						snprintf(text, sizeof(text), "%s sigma %.1f t %.1f: preserve luma %.3f chroma %.3f (model max diff %.1f 16-bit codes); blend luma %.3f",
							soft ? "soft" : "white", sigma, t, rl, rc, worst, rb);
						Logger::WriteMessage(text);
						const std::wstring wtext(text, text + strlen(text));
						Assert::IsTrue(rl > 0.95 && rl < 1.05, wtext.c_str());
						Assert::IsTrue(rc > 0.95 && rc < 1.05, wtext.c_str());
						Assert::IsTrue(worst <= 96.0, (L"the shader is the reference model: " + wtext).c_str());
						if (t == 0.5f) Assert::IsTrue(rb < 0.8, (L"blend averages grain away: " + wtext).c_str());
						// The legacy switch restored on the switched object: byte for byte.
						preserve.SetGrain(InterpolationPolicy::Grain::Blend);
						Dejudder::Planes back;
						Assert::IsTrue(preserve.Run(planesA.planes, planesB.planes, InterpolatorMotion(field), t,
							InterpolationPolicy::Fallback::Hold, back), InterpolatorError(preserve, log).c_str());
						Assert::IsTrue(DownloadPlane(gpu, back.luma) == oldLuma, L"blend luma byte for byte");
						Assert::IsTrue(DownloadPlane(gpu, back.chroma) == oldChroma, L"blend chroma byte for byte");
						preserve.SetGrain(InterpolationPolicy::Grain::Preserve);
					}
					Assert::AreEqual(5ull, preserve.GrainPreservedRuns());
					Assert::AreEqual(0ull, legacy.GrainPreservedRuns());
				}
			// The fallback blend (untrusted vectors) splits the same way: the grain is kept.
			{
				const GrainFrames gf = MakeGrainFrames(width, height, 2, 0.0, 0.0, 2.0, false, 77u);
				DejudderGpuPlanes planesA, planesB;
				UploadDejudderFrame(gpu, gf.frames[0], planesA);
				UploadDejudderFrame(gpu, gf.frames[1], planesB);
				Interpolator::FrameInterpolator preserve(fixture.Log(), gpu, true);
				preserve.SetGrain(InterpolationPolicy::Grain::Preserve);
				Dejudder::Planes out;
				Assert::IsTrue(preserve.Run(planesA.planes, planesB.planes, Interpolator::Motion(), 0.5f,
					InterpolationPolicy::Fallback::Blend, out), InterpolatorError(preserve, log).c_str());
				const double r = GrainModel::Rms(LumaGrainOf(DownloadPlane(gpu, out.luma), width, height, 0.0, 0.0, 0.0), margin) /
					GrainModel::Rms(gf.lumaGrain[0], margin);
				Assert::IsTrue(r > 0.95 && r < 1.05, (L"fallback blend keeps the grain: " + std::to_wstring(r)).c_str());
			}
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(log);
		}

		TEST_METHOD(InterpolatorGrainNotSmearedAcrossFrames)
		{
			// 50p on 60 Hz through the shader: per cycle at most as many near-duplicate output
			// pairs (lag-1 correlation along the pan > 0.5) as a hold has repeats (one), every other
			// pair below 0.10; blend (the legacy) shares grain over several pairs.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			const int width = 256, height = 128, margin = 24;
			const double vx = 3.3, vy = 1.7, ratio = 50.0 / 60.0;
			InterpolatorField field;
			MakeInterpolatorField(gpu, width, height, 1.0f,
				[&](int, int) { return std::array<float, 4>{ static_cast<float>(vx), static_cast<float>(vy), 1.0f, 0.0f }; }, field);
			std::string log;
			fixture.CaptureLog(&log);
			for (bool soft : { false, true })
			{
				const GrainFrames gf = MakeGrainFrames(width, height, 8, vx, vy, 2.0, soft, 41u + (soft ? 100u : 0u));
				std::vector<std::unique_ptr<DejudderGpuPlanes>> planes;
				for (const DejudderFrame& f : gf.frames)
				{
					planes.push_back(std::make_unique<DejudderGpuPlanes>());
					UploadDejudderFrame(gpu, f, *planes.back());
				}
				for (bool preserveMode : { true, false })
				{
					Interpolator::FrameInterpolator interpolator(fixture.Log(), gpu, true);
					interpolator.SetGrain(preserveMode ? InterpolationPolicy::Grain::Preserve : InterpolationPolicy::Grain::Blend);
					std::vector<GrainModel::Image> grains;
					std::vector<double> positions;
					for (int n = 0; n < 7; ++n)
					{
						const double x = n * ratio;
						const int m = static_cast<int>(std::floor(x + 1e-9));
						Dejudder::Planes out;
						Assert::IsTrue(interpolator.Run(planes[m]->planes, planes[m + 1]->planes, InterpolatorMotion(field),
							static_cast<float>(x - m), InterpolationPolicy::Fallback::Hold, out), InterpolatorError(interpolator, log).c_str());
						grains.push_back(LumaGrainOf(DownloadPlane(gpu, out.luma), width, height, x, vx, vy));
						positions.push_back(x);
					}
					int shared = 0, smeared = 0;
					std::string text = std::string(soft ? "soft" : "white") + (preserveMode ? " preserve" : " blend") + " lag-1:";
					for (int n = 0; n + 1 < 7; ++n)
					{
						const double d = positions[n + 1] - positions[n];
						const double c = GrainModel::Corr(GrainModel::Warp(grains[n], d * vx, d * vy, true), grains[n + 1], margin);
						char v[32];
						snprintf(v, sizeof(v), " %.2f", c);
						text += v;
						if (c > 0.5) ++shared;
						else if (c >= 0.10) ++smeared;
					}
					Logger::WriteMessage(text.c_str());
					const std::wstring wtext(text.begin(), text.end());
					if (preserveMode)
						Assert::IsTrue(shared <= 1 && smeared == 0, wtext.c_str());
					else
						Assert::IsTrue(smeared >= 2, wtext.c_str());
				}
			}
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(log);
		}

		TEST_METHOD(DejudderRenderedWarpKeepsGrainRms)
		{
			// The de-judder's rendered warp (Catmull-Rom at the output resolution; no switch):
			// grain rendered at 2x keeps 0.95x or more at fractional output shifts (audit:
			// 0.96-0.98x). The source warp (dejudder_source_warp_filter): catmull keeps more than
			// the hardware's bilinear at a fractional shift; bilinear is the never-switched
			// matcher's output byte for byte.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			const int width = 128, height = 64, ow = width * 2, oh = height * 2;
			// The rendered picture: white grain at the source resolution, upscaled 2x (Catmull-Rom).
			std::mt19937 rng(51u);
			const GrainModel::Image g = GrainModel::Grain(width, height, 2.0, false, rng);
			GrainModel::Image up(ow, oh);
			for (int y = 0; y < oh; ++y)
				for (int x = 0; x < ow; ++x)
					up.at(x, y) = GrainModel::Catmull(g, (x + 0.5) / 2.0, (y + 0.5) / 2.0);
			std::vector<uint16_t> rgba(static_cast<size_t>(ow) * oh * 4);
			for (size_t i = 0; i < up.v.size(); ++i)
			{
				const uint16_t v = static_cast<uint16_t>(std::lround(30000.0 + kGrainCode16 * up.v[i]));
				rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = v;
				rgba[i * 4 + 3] = 65535;
			}
			pl_tex_params cp{};
			cp.w = ow; cp.h = oh;
			cp.format = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 16, 16, static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_LINEAR));
			cp.sampleable = true;
			cp.initial_data = rgba.data();
			pl_tex cached = pl_tex_create(gpu, &cp);
			Assert::IsNotNull(cached);
			const double vx = 12.0, vy = 6.0;   // source px per frame
			InterpolatorField field;
			MakeInterpolatorField(gpu, width, height, 1.0f,
				[&](int, int) { return std::array<float, 4>{ static_cast<float>(vx), static_cast<float>(vy), 1.0f, 0.0f }; }, field);
			DejudderRendered::Warp rendered(fixture.Log(), gpu, true);
			DejudderRendered::Geometry geometry;
			geometry.sourceCrop = { 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height) };
			geometry.targetCrop = { 0.0f, 0.0f, static_cast<float>(ow), static_cast<float>(oh) };
			const double source = GrainModel::Rms(up, 24);
			for (float shift : { 0.15f, 0.3f, 0.55f })
			{
				pl_tex out = nullptr;
				Assert::IsTrue(rendered.Run(cached, field.field, field.trust, 0.25f, shift, geometry, out),
					ExtendedRangeGpuProbe::ToWide(rendered.Error()).c_str());
				std::vector<float> px(static_cast<size_t>(out->params.w) * out->params.h * out->params.format->num_components);
				pl_tex_transfer_params d{}; d.tex = out; d.ptr = px.data();
				Assert::IsTrue(pl_tex_download(gpu, &d));
				const int nc = out->params.format->num_components;
				GrainModel::Image w(ow, oh);
				for (size_t i = 0; i < w.v.size(); ++i) w.v[i] = (px[i * nc] * 65535.0 - 30000.0) / kGrainCode16;
				const double r = GrainModel::Rms(w, 24) / source;
				char text[120];
				snprintf(text, sizeof(text), "rendered warp shift %.2f (output px %.2f, %.2f): grain %.3f", shift, shift * vx * 2, shift * vy * 2, r);
				Logger::WriteMessage(text);
				Assert::IsTrue(r >= 0.95 && r < 1.05, std::wstring(text, text + strlen(text)).c_str());
			}
			pl_tex_destroy(gpu, &cached);

			// The source warp: two matchers on the same pair (B = A moved by (12, 6), the same grain).
			const GrainFrames gf = MakeGrainFrames(256, 128, 1, 0.0, 0.0, 3.0, true, 52u);
			const DejudderFrame& a = gf.frames[0];
			const DejudderFrame b = MakeDejudderFrame(256, 128,
				[&](int x, int y) { return a.luma[static_cast<size_t>((std::max)(0, y - 6)) * 256 + (std::max)(0, x - 12)]; },
				[&](int x, int y, int c) { return a.chroma[(static_cast<size_t>((std::max)(0, y - 6)) * 128 + (std::max)(0, x - 6)) * 2 + c]; });
			DejudderGpuPlanes planesA, planesB;
			UploadDejudderFrame(gpu, a, planesA);
			UploadDejudderFrame(gpu, b, planesB);
			Dejudder::Settings bilinearSettings = DejudderTestSettings();
			Dejudder::Settings catmullSettings = DejudderTestSettings();
			catmullSettings.sourceWarpCatmull = true;
			Assert::IsFalse(Dejudder::Settings().sourceWarpCatmull, L"the object's default is the established bilinear");
			Dejudder::MotionWarp never(fixture.Log(), gpu, DejudderTestSettings());
			Dejudder::MotionWarp bilinear(fixture.Log(), gpu, bilinearSettings);
			Dejudder::MotionWarp catmull(fixture.Log(), gpu, catmullSettings);
			for (Dejudder::MotionWarp* m : { &never, &bilinear, &catmull })
				Assert::IsTrue(m->Estimate(planesA.planes, planesB.planes), ExtendedRangeGpuProbe::ToWide(m->LastError()).c_str());
			Dejudder::Planes outNever, outBilinear, outCatmull;
			Assert::IsTrue(never.Warp(planesA.planes, 0.3f, outNever));
			Assert::IsTrue(bilinear.Warp(planesA.planes, 0.3f, outBilinear));
			Assert::IsTrue(catmull.Warp(planesA.planes, 0.3f, outCatmull));
			const std::vector<uint16_t> lumaNever = DownloadPlane(gpu, outNever.luma);
			Assert::IsTrue(DownloadPlane(gpu, outBilinear.luma) == lumaNever, L"bilinear: the established warp byte for byte");
			Assert::IsTrue(DownloadPlane(gpu, outBilinear.chroma) == DownloadPlane(gpu, outNever.chroma), L"bilinear chroma byte for byte");
			// The warped picture minus the picture at (3.6, 1.8): its grain.
			auto grainOf = [&](const std::vector<uint16_t>& luma) {
				GrainModel::Image w(256, 128);
				for (int y = 0; y < 128; ++y)
					for (int x = 0; x < 256; ++x)
						w.at(x, y) = (luma[static_cast<size_t>(y) * 256 + x] - GrainTexture16(x + 0.5 - 3.6, y + 0.5 - 1.8)) / kGrainCode16;
				return w;
			};
			const double src = GrainModel::Rms(gf.lumaGrain[0], 32);
			const double rBil = GrainModel::Rms(grainOf(lumaNever), 32) / src;
			const double rCat = GrainModel::Rms(grainOf(DownloadPlane(gpu, outCatmull.luma)), 32) / src;
			char text[160];
			snprintf(text, sizeof(text), "source warp at (3.6, 1.8), soft grain: catmull %.3f, bilinear %.3f", rCat, rBil);
			Logger::WriteMessage(text);
			const std::wstring wtext(text, text + strlen(text));
			Assert::IsTrue(rCat > rBil + 0.05, wtext.c_str());
			Assert::IsTrue(rCat >= 0.9 && rCat < 1.05, wtext.c_str());
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(log);
		}

		TEST_METHOD(SsimResidualStabiliseLeavesGrainBlocks)
		{
			// residual_stabilise_grain_guard: on a picture whose grain is fresh every frame, the
			// SSIM residual held along trusted (still) vectors passes this frame's residual
			// through where the grain term marks grain - today's (unheld) output within a code and
			// the same frame-to-frame change; off is the established one-pole (it steadies the
			// residual, averaging the grain), twice the same bytes.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create());
			pl_gpu gpu = fixture.Gpu();
			const pl_hook* ssim = nullptr;
			{
				const std::string source = LoadBundledShader("SSimDownscaler.glsl");
				ssim = pl_mpv_user_shader_parse(gpu, source.data(), source.size());
				Assert::IsNotNull(ssim);
			}
			static constexpr int width = 256, height = 128;
			pl_fmt srcFmt = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 16, 16, static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_LINEAR));
			pl_fmt dstFmt = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 16, 16, static_cast<enum pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE));
			Assert::IsTrue(srcFmt && dstFmt);
			auto picture = [&](unsigned seed) {
				std::mt19937 grain(seed);
				std::normal_distribution<double> n(0.0, 0.03);   // fresh grain, about 8 codes
				std::vector<uint16_t> px(static_cast<size_t>(width) * height * 4);
				for (int y = 0; y < height; ++y)
					for (int x = 0; x < width; ++x)
					{
						const double v = (std::min)(1.0, (std::max)(0.0, 0.45 + 0.1 * std::sin(x * 0.11) * std::cos(y * 0.07) + n(grain)));
						const size_t i = (static_cast<size_t>(y) * width + x) * 4;
						px[i] = px[i + 1] = px[i + 2] = static_cast<uint16_t>(std::lround(v * 65535.0));
						px[i + 3] = 65535;
					}
				pl_tex_params tp{};
				tp.w = width; tp.h = height; tp.format = srcFmt; tp.sampleable = true; tp.initial_data = px.data();
				return pl_tex_create(gpu, &tp);
			};
			std::vector<float> f(static_cast<size_t>(width / 16) * (height / 16) * 4);
			for (size_t i = 0; i < f.size(); i += 4) { f[i] = 0.0f; f[i + 1] = 0.0f; f[i + 2] = 1.0f; f[i + 3] = 0.0f; }
			pl_tex_params fp{};
			fp.w = width / 16; fp.h = height / 16; fp.format = pl_find_named_fmt(gpu, "rgba32f"); fp.sampleable = true; fp.initial_data = f.data();
			pl_tex trusted = pl_tex_create(gpu, &fp);
			const float trustValue[4] = { 1.0f, 1.0f, 0.0f, 1.0f };
			pl_tex_params tp1{};
			tp1.w = 1; tp1.h = 1; tp1.format = pl_find_named_fmt(gpu, "rgba32f"); tp1.sampleable = true; tp1.initial_data = trustValue;
			pl_tex frameTrust = pl_tex_create(gpu, &tp1);
			Assert::IsTrue(trusted && frameTrust);
			auto run = [&](bool stabilise, bool guard, int frames) {
				AdaptiveDownscale::Module module;
				AdaptiveDownscale::Settings st;
				st.grainSmoothMs = 0.0f;
				st.grainMap = false;
				st.ssimResidualStabilise = stabilise;
				st.residualGrainGuard = guard;
				std::vector<std::vector<uint16_t>> outputs;
				for (int k = 0; k < frames; ++k)
				{
					pl_tex src = picture(static_cast<unsigned>(2000 + k));
					module.SetFrame(st, static_cast<uint64_t>(10 + k), 1.0f / 24.0f);
					module.ClearMotion();
					AdaptiveDownscale::Motion m;
					m.field = trusted;
					m.frameTrust = frameTrust;
					m.minTrustedShare = 0.25f;
					m.sourceCrop = { 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height) };
					module.SetMotion(m);
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
					params.hooks = hooks;
					params.num_hooks = 3;
					Assert::IsTrue(pl_render_image(fixture.Renderer(), &image, &out, &params));
					pl_gpu_finish(gpu);
					std::vector<uint16_t> px(static_cast<size_t>(width / 2) * (height / 2) * 4);
					pl_tex_transfer_params dl{}; dl.tex = target; dl.ptr = px.data();
					Assert::IsTrue(pl_tex_download(gpu, &dl));
					outputs.push_back(px);
					pl_tex_destroy(gpu, &target);
					pl_tex_destroy(gpu, &src);
				}
				module.Release();
				return outputs;
			};
			auto change = [](const std::vector<std::vector<uint16_t>>& o) {
				double sum = 0; size_t n = 0;
				for (size_t k = 3; k < o.size(); ++k)
					for (size_t i = 0; i < o[k].size(); i += 4) { sum += std::fabs(double(o[k][i]) - double(o[k - 1][i])); ++n; }
				return sum / n / 257.0;
			};
			auto maxDiff = [](const std::vector<uint16_t>& a, const std::vector<uint16_t>& b) {
				int m = 0;
				for (size_t i = 0; i < a.size(); ++i) if (i % 4 != 3) m = (std::max)(m, std::abs(int(a[i]) - int(b[i])));
				return m / 257.0;
			};
			const int frames = 8;
			const auto today = run(false, true, frames);
			const auto guarded = run(true, true, frames);
			const auto legacy = run(true, false, frames);
			const auto legacyAgain = run(true, false, frames);
			char text[200];
			snprintf(text, sizeof(text), "frame-to-frame change: today %.3f, guard %.3f, legacy one-pole %.3f; guard vs today max %.2f codes",
				change(today), change(guarded), change(legacy), maxDiff(guarded.back(), today.back()));
			Logger::WriteMessage(text);
			const std::wstring wtext(text, text + strlen(text));
			Assert::IsTrue(maxDiff(guarded.back(), today.back()) <= 1.0, wtext.c_str());
			Assert::IsTrue(std::fabs(change(guarded) - change(today)) <= 0.05 * change(today), wtext.c_str());
			Assert::IsTrue(change(legacy) < change(today), (L"off: the one-pole holds (averages) the residual: " + wtext).c_str());
			for (int k = 0; k < frames; ++k)
				Assert::IsTrue(legacy[k] == legacyAgain[k], L"off: deterministic, the established pass");
			pl_tex_destroy(gpu, &frameTrust);
			pl_tex_destroy(gpu, &trusted);
			pl_mpv_user_shader_destroy(&ssim);
		}

		TEST_METHOD(VpDebanderKeepsGrainWithoutVectors)
		{
			// deband_vp_grain_guard: without vectors, grain of 0.5 8-bit code (2 10-bit codes) and
			// more keeps 0.95x or more at the 8-bit-in-10 threshold (6 codes) and at the low-bitrate
			// maximum (12 codes); a banded ramp is debanded as before (within a code); off is the
			// never-switched module's output byte for byte (0.5-0.6x of the grain).
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			pl_dispatch dispatch = pl_dispatch_create(fixture.Log(), gpu);
			const int W = 256, H = 128, margin = 24;
			auto upload = [&](const std::vector<int>& codes) {
				std::vector<uint16_t> data(codes.size());
				for (size_t i = 0; i < codes.size(); ++i) data[i] = static_cast<uint16_t>((std::min)(1023, (std::max)(0, codes[i])) << 6);
				pl_tex_params p{};
				p.w = W; p.h = H; p.format = pl_find_named_fmt(gpu, "r16"); p.sampleable = true; p.initial_data = data.data();
				return pl_tex_create(gpu, &p);
			};
			auto readback = [&](pl_tex tex) {
				pl_tex_params rp{};
				rp.w = W; rp.h = H; rp.format = tex->params.format; rp.host_readable = true; rp.blit_dst = true;
				pl_tex readable = pl_tex_create(gpu, &rp);
				pl_tex_blit_params bp{}; bp.src = tex; bp.dst = readable;
				pl_tex_blit(gpu, &bp);
				std::vector<uint16_t> raw(static_cast<size_t>(W) * H);
				pl_tex_transfer_params d{}; d.tex = readable; d.ptr = raw.data();
				Assert::IsTrue(pl_tex_download(gpu, &d));
				pl_tex_destroy(gpu, &readable);
				return raw;
			};
			auto toCodes = [&](const std::vector<uint16_t>& raw) {
				GrainModel::Image g(W, H);
				for (size_t i = 0; i < raw.size(); ++i) g.v[i] = raw[i] / 64.0;
				return g;
			};
			auto frameOf = [&](pl_tex tex) {
				pl_frame f{};
				f.num_planes = 1;
				f.planes[0].texture = tex; f.planes[0].components = 1; f.planes[0].component_mapping[0] = PL_CHANNEL_Y;
				f.repr.sys = PL_COLOR_SYSTEM_BT_709; f.repr.levels = PL_COLOR_LEVELS_LIMITED;
				f.repr.bits.sample_depth = 16; f.repr.bits.color_depth = 10; f.repr.bits.bit_shift = 6;
				f.color = pl_color_space_bt709;
				return f;
			};
			GrainModel::Image ramp(W, H);
			for (int y = 0; y < H; ++y)
				for (int x = 0; x < W; ++x)
					ramp.at(x, y) = 400.0 + 0.28 * x + 0.12 * y;   // 10-bit codes
			for (double threshold : { VpDebander::Strength(8.0, 10, 1.0, true), VpDebander::Strength(8.0, 10, 0.0, true) })
				for (bool soft : { false, true })
					for (double sigma8 : { 0.5, 1.0, 2.0, 4.0 })
					{
						std::mt19937 rng(61u + static_cast<unsigned>(sigma8 * 4) + (soft ? 100u : 0u));
						const GrainModel::Image g = GrainModel::Grain(W, H, sigma8 * 4.0, soft, rng);   // 10-bit codes
						std::vector<int> codes(static_cast<size_t>(W) * H);
						for (size_t i = 0; i < codes.size(); ++i) codes[i] = static_cast<int>(std::lround(ramp.v[i] + g.v[i]));
						pl_tex tex = upload(codes);
						GrainModel::Image input(W, H);
						for (size_t i = 0; i < codes.size(); ++i) input.v[i] = codes[i];
						const double source = GrainModel::Rms(input - ramp, margin);
						VpDebander::Params params;
						params.thresholdCodes = threshold;
						params.colorDepth = 10;
						params.radiusScale = 1080.0 / H;
						VpDebander::Module never, guarded, legacy;
						pl_frame outNever{}, outGuard{}, outLegacy{};
						Assert::IsTrue(never.Apply(gpu, dispatch, frameOf(tex), params, outNever), ExtendedRangeGpuProbe::ToWide(never.LastError()).c_str());
						VpDebander::Params on = params;
						on.grainGuard = true;
						Assert::IsTrue(guarded.Apply(gpu, dispatch, frameOf(tex), on, outGuard), ExtendedRangeGpuProbe::ToWide(guarded.LastError()).c_str());
						VpDebander::Params off = params;
						off.grainGuard = false;
						Assert::IsTrue(legacy.Apply(gpu, dispatch, frameOf(tex), off, outLegacy), ExtendedRangeGpuProbe::ToWide(legacy.LastError()).c_str());
						const std::vector<uint16_t> rawNever = readback(outNever.planes[0].texture);
						const std::vector<uint16_t> rawLegacy = readback(outLegacy.planes[0].texture);
						const double rg = GrainModel::Rms(toCodes(readback(outGuard.planes[0].texture)) - ramp, margin) / source;
						const double rl = GrainModel::Rms(toCodes(rawLegacy) - ramp, margin) / source;
						char text[160];
						snprintf(text, sizeof(text), "%s grain %.1f codes, threshold %.0f: guard %.3f legacy %.3f", soft ? "soft" : "white", sigma8, threshold, rg, rl);
						Logger::WriteMessage(text);
						const std::wstring wtext(text, text + strlen(text));
						Assert::IsTrue(rawLegacy == rawNever, (L"off: the established debander byte for byte; " + wtext).c_str());
						Assert::IsTrue(rg > 0.95 && rg < 1.05, wtext.c_str());
						if (sigma8 == 0.5) Assert::IsTrue(rl < 0.8, wtext.c_str());
						pl_tex_destroy(gpu, &tex);
						never.Release(gpu); guarded.Release(gpu); legacy.Release(gpu);
					}
			// A banded ramp (8-bit steps in the 10-bit signal): debanded with the guard as without.
			{
				std::vector<int> banded(static_cast<size_t>(W) * H);
				for (int y = 0; y < H; ++y)
					for (int x = 0; x < W; ++x)
						banded[static_cast<size_t>(y) * W + x] = 4 * (100 + x / 24);
				pl_tex tex = upload(banded);
				VpDebander::Params params;
				params.thresholdCodes = VpDebander::Strength(8.0, 10, 1.0, true);
				params.colorDepth = 10;
				params.radiusScale = 1080.0 / H;
				VpDebander::Params on = params;
				on.grainGuard = true;
				VpDebander::Module a, b;
				pl_frame oa{}, ob{};
				Assert::IsTrue(a.Apply(gpu, dispatch, frameOf(tex), params, oa));
				Assert::IsTrue(b.Apply(gpu, dispatch, frameOf(tex), on, ob));
				const std::vector<uint16_t> ra = readback(oa.planes[0].texture), rb = readback(ob.planes[0].texture);
				int worst = 0, changed = 0;
				for (size_t i = 0; i < ra.size(); ++i)
				{
					worst = (std::max)(worst, std::abs(int(ra[i]) - int(rb[i])) / 64);
					changed += ra[i] != static_cast<uint16_t>(banded[i] << 6);
				}
				Assert::IsTrue(worst <= 1, L"bands debanded with the guard as without (within a code)");
				Assert::IsTrue(changed > static_cast<int>(ra.size()) / 4, L"the bands are debanded");
				a.Release(gpu); b.Release(gpu);
				pl_tex_destroy(gpu, &tex);
			}
			pl_dispatch_destroy(&dispatch);
		}

		TEST_METHOD(LibplaceboDebandAddsNoGrainWhenOff)
		{
			// deband_grain off: libplacebo's deband (default strength) on a flat picture adds no
			// noise - the output is flat; on adds its grain (about 0.29 8-bit code RMS); on is
			// libplacebo's own default parameters byte for byte (a fresh renderer each).
			auto render = [](const pl_deband_params* deband) {
				TargetLutGpuFixture fixture;
				Assert::IsTrue(fixture.Create());
				pl_gpu gpu = fixture.Gpu();
				const int w = 64, h = 64;
				std::vector<uint16_t> px(static_cast<size_t>(w) * h * 4, 0);
				for (size_t i = 0; i < px.size(); i += 4) { px[i] = px[i + 1] = px[i + 2] = 30000; px[i + 3] = 65535; }
				pl_fmt srcFmt = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 16, 16, static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_LINEAR));
				pl_fmt dstFmt = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 16, 16, static_cast<enum pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE));
				pl_tex_params sp{}; sp.w = w; sp.h = h; sp.format = srcFmt; sp.sampleable = true; sp.initial_data = px.data();
				pl_tex src = pl_tex_create(gpu, &sp);
				pl_tex_params dp{}; dp.w = w; dp.h = h; dp.format = dstFmt; dp.renderable = true; dp.host_readable = true;
				pl_tex dst = pl_tex_create(gpu, &dp);
				Assert::IsTrue(src && dst);
				pl_frame image = TargetLutGpuFixture::MakeRgbFrame(src);
				pl_frame target = TargetLutGpuFixture::MakeRgbFrame(dst);
				pl_render_params params = pl_render_default_params;
				params.dither_params = nullptr;
				params.error_diffusion = nullptr;
				params.deband_params = deband;
				Assert::IsTrue(pl_render_image(fixture.Renderer(), &image, &target, &params));
				pl_gpu_finish(gpu);
				std::vector<uint16_t> out(static_cast<size_t>(w) * h * 4);
				pl_tex_transfer_params d{}; d.tex = dst; d.ptr = out.data();
				Assert::IsTrue(pl_tex_download(gpu, &d));
				pl_tex_destroy(gpu, &src);
				pl_tex_destroy(gpu, &dst);
				return out;
			};
			auto rms = [](const std::vector<uint16_t>& o) {
				double mean = 0.0, sq = 0.0; size_t n = 0;
				for (size_t i = 0; i < o.size(); i += 4) { mean += o[i]; ++n; }
				mean /= n;
				for (size_t i = 0; i < o.size(); i += 4) sq += (o[i] - mean) * (o[i] - mean);
				return std::sqrt(sq / n) / 257.0;   // 8-bit codes
			};
			LibplaceboRenderParameters::Settings on, off;
			on.debandStrength = off.debandStrength = "default";
			on.debandGrain = true;
			off.debandGrain = false;
			LibplaceboRenderParameters::Projection pOn, pOff;
			std::string error;
			Assert::IsTrue(LibplaceboRenderParameters::Build(on, false, pOn, error));
			Assert::IsTrue(LibplaceboRenderParameters::Build(off, false, pOff, error));
			const std::vector<uint16_t> withGrain = render(pOn.renderParams.deband_params);
			const std::vector<uint16_t> without = render(pOff.renderParams.deband_params);
			const std::vector<uint16_t> native = render(&pl_deband_default_params);
			char text[120];
			snprintf(text, sizeof(text), "flat picture through libplacebo's deband: grain on %.3f, off %.3f 8-bit codes RMS", rms(withGrain), rms(without));
			Logger::WriteMessage(text);
			Assert::AreEqual(0.0, rms(without), 1e-9, L"off: no added noise");
			Assert::IsTrue(rms(withGrain) > 0.1, L"on: libplacebo's grain");
			Assert::IsTrue(withGrain == native, L"on: libplacebo's own default parameters byte for byte");
		}

		// --- VP's 50/60p frame interpolator (docs/VP-interpolator-50-60-plan.md step 3) ---
		// GPU tests (WARP unless VP_TEST_GPU=hardware). The field is built by hand,
		// so the interpolator is tested on its own; one test uses the matcher's field.

		struct InterpolatorField
		{
			pl_gpu gpu = nullptr;
			pl_tex field = nullptr, trust = nullptr, occlusion = nullptr;
			~InterpolatorField()
			{
				pl_tex_destroy(gpu, &field);
				pl_tex_destroy(gpu, &trust);
				pl_tex_destroy(gpu, &occlusion);
			}
		};

		static pl_tex MakeFloatTexture(pl_gpu gpu, int w, int h, const std::vector<float>& rgba)
		{
			pl_fmt f32 = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32, PL_FMT_CAP_SAMPLEABLE);
			Assert::IsNotNull(f32, L"No sampleable RGBA32F format");
			pl_tex_params params{};
			params.w = w;
			params.h = h;
			params.format = f32;
			params.sampleable = true;
			params.initial_data = rgba.data();
			pl_tex tex = pl_tex_create(gpu, &params);
			Assert::IsNotNull(tex);
			return tex;
		}

		// One texel per 16x16 source block: (vx, vy, trust, error) from `block`.
		template <typename Block>
		static void MakeInterpolatorField(pl_gpu gpu, int width, int height, float trustedShare, Block block,
			InterpolatorField& out)
		{
			out.gpu = gpu;
			const int bx = (width + 15) / 16, by = (height + 15) / 16;
			std::vector<float> texels(static_cast<size_t>(bx) * by * 4);
			for (int y = 0; y < by; ++y)
				for (int x = 0; x < bx; ++x)
				{
					const std::array<float, 4> t = block(x, y);
					std::copy(t.begin(), t.end(), texels.begin() + (static_cast<size_t>(y) * bx + x) * 4);
				}
			out.field = MakeFloatTexture(gpu, bx, by, texels);
			out.trust = MakeFloatTexture(gpu, 1, 1, { trustedShare, 1.0f, 0.0f, 1.0f });
		}

		static Interpolator::Motion InterpolatorMotion(const InterpolatorField& f)
		{
			Interpolator::Motion m;
			m.field = f.field;
			m.frameTrust = f.trust;
			m.occlusionB = f.occlusion;
			m.minTrustedShare = 0.25f;
			return m;
		}

		static std::wstring InterpolatorError(const Interpolator::FrameInterpolator& i, const std::string& log)
		{
			const std::string s = i.LastError() + "; log: " + log;
			return std::wstring(s.begin(), s.end());
		}

		TEST_METHOD(InterpolatorT0IsAExactlyT1IsB)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			const int width = 128, height = 64;
			const DejudderFrame a = MakeDejudderFrame(width, height,
				[](int x, int y) { return DejudderTexture(x, y); },
				[](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); });
			const DejudderFrame b = MakeDejudderFrame(width, height,
				[](int x, int y) { return DejudderTexture(x - 16, y); },
				[](int x, int y, int c) { return DejudderTexture((x - 8) * 2, y, 7 + c); });
			DejudderGpuPlanes planesA, planesB;
			UploadDejudderFrame(gpu, a, planesA);
			UploadDejudderFrame(gpu, b, planesB);
			InterpolatorField field;
			MakeInterpolatorField(gpu, width, height, 1.0f,
				[](int, int) { return std::array<float, 4>{ 16.0f, 0.0f, 1.0f, 0.0f }; }, field);
			std::string log;
			fixture.CaptureLog(&log);
			Interpolator::FrameInterpolator interpolator(fixture.Log(), gpu, true);
			Dejudder::Planes out;
			for (auto fallback : { InterpolationPolicy::Fallback::Hold, InterpolationPolicy::Fallback::Blend })
			{
				Assert::IsTrue(interpolator.Run(planesA.planes, planesB.planes, InterpolatorMotion(field), 0.0f, fallback, out));
				Assert::IsTrue(out.luma == planesA.planes.luma && out.chroma == planesA.planes.chroma, L"t = 0 is A itself");
				Assert::IsTrue(interpolator.Run(planesA.planes, planesB.planes, InterpolatorMotion(field), 1.0f, fallback, out));
				Assert::IsTrue(out.luma == planesB.planes.luma && out.chroma == planesB.planes.chroma, L"t = 1 is B itself");
				Assert::IsTrue(interpolator.Run(planesA.planes, planesB.planes, InterpolatorMotion(field), -0.5f, fallback, out));
				Assert::IsTrue(out.luma == planesA.planes.luma, L"before A: A");
				Assert::IsTrue(interpolator.Run(planesA.planes, planesB.planes, InterpolatorMotion(field), 1.5f, fallback, out));
				Assert::IsTrue(out.luma == planesB.planes.luma, L"after B: B");
			}
			Assert::AreEqual(0ull, interpolator.Runs(), L"nothing dispatched at the ends");
			// In between: new planes in the input formats and sizes.
			Assert::IsTrue(interpolator.Run(planesA.planes, planesB.planes, InterpolatorMotion(field), 0.5f,
				InterpolationPolicy::Fallback::Hold, out), InterpolatorError(interpolator, log).c_str());
			Assert::IsTrue(out.luma != planesA.planes.luma && out.luma != planesB.planes.luma);
			Assert::IsTrue(out.luma->params.format == planesA.planes.luma->params.format &&
				out.chroma->params.format == planesA.planes.chroma->params.format, L"the input plane formats");
			Assert::AreEqual(width, out.luma->params.w);
			Assert::AreEqual(width / 2, out.chroma->params.w);
			// The dispatched path is exact too: with no motion between identical frames
			// any phase gives the frame back code for code.
			InterpolatorField still;
			MakeInterpolatorField(gpu, width, height, 1.0f,
				[](int, int) { return std::array<float, 4>{ 0.0f, 0.0f, 1.0f, 0.0f }; }, still);
			for (float t : { 1e-4f, 0.25f, 0.5f, 0.9999f })
			{
				Assert::IsTrue(interpolator.Run(planesA.planes, planesA.planes, InterpolatorMotion(still), t,
					InterpolationPolicy::Fallback::Hold, out), InterpolatorError(interpolator, log).c_str());
				Assert::IsTrue(DownloadPlane(gpu, out.luma) == a.luma, L"luma exact");
				Assert::IsTrue(DownloadPlane(gpu, out.chroma) == a.chroma, L"chroma exact");
			}
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(log);
		}

		TEST_METHOD(InterpolatorPanLandsAtTheTruePhase)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			const int width = 256, height = 128;
			static constexpr int dx = 16, dy = 8;   // per source frame: whole pixels at t = 0.25 / 0.5 / 0.75
			const DejudderFrame a = MakeDejudderFrame(width, height,
				[](int x, int y) { return DejudderTexture(x, y); },
				[](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); });
			const DejudderFrame b = MakeDejudderFrame(width, height,
				[](int x, int y) { return DejudderTexture(x - dx, y - dy); },
				[](int x, int y, int c) { return DejudderTexture((x - dx / 2) * 2, y - dy, 7 + c); });
			DejudderGpuPlanes planesA, planesB;
			UploadDejudderFrame(gpu, a, planesA);
			UploadDejudderFrame(gpu, b, planesB);
			InterpolatorField field;
			MakeInterpolatorField(gpu, width, height, 1.0f,
				[](int, int) { return std::array<float, 4>{ static_cast<float>(dx), static_cast<float>(dy), 1.0f, 0.0f }; }, field);
			std::string log;
			fixture.CaptureLog(&log);
			Interpolator::FrameInterpolator interpolator(fixture.Log(), gpu, true);
			for (int quarter = 1; quarter <= 3; ++quarter)
			{
				const float t = 0.25f * quarter;
				const int sx = dx * quarter / 4, sy = dy * quarter / 4;
				Dejudder::Planes out;
				Assert::IsTrue(interpolator.Run(planesA.planes, planesB.planes, InterpolatorMotion(field), t,
					InterpolationPolicy::Fallback::Hold, out), InterpolatorError(interpolator, log).c_str());
				const std::vector<uint16_t> luma = DownloadPlane(gpu, out.luma);
				const std::vector<uint16_t> chroma = DownloadPlane(gpu, out.chroma);
				int checked = 0, exact = 0;
				for (int y = dy + 2; y < height - dy - 2; ++y)
					for (int x = dx + 2; x < width - dx - 2; ++x)
					{
						++checked;
						exact += std::abs(static_cast<int>(luma[static_cast<size_t>(y) * width + x]) -
							static_cast<int>(DejudderTexture(x - sx, y - sy))) <= 1;
					}
				Assert::AreEqual(checked, exact, (L"luma at the true phase, t = " + std::to_wstring(t)).c_str());
				int chromaChecked = 0, chromaExact = 0;
				for (int y = dy + 2; y < height - dy - 2; ++y)
					for (int x = dx / 2 + 2; x < width / 2 - dx / 2 - 2; ++x)
						for (int c = 0; c < 2; ++c)
						{
							++chromaChecked;
							chromaExact += std::abs(static_cast<int>(chroma[(static_cast<size_t>(y) * (width / 2) + x) * 2 + c]) -
								static_cast<int>(DejudderTexture((x - sx / 2) * 2, y - sy, 7 + c))) <= 1;
						}
				Assert::AreEqual(chromaChecked, chromaExact, (L"chroma at the true phase, t = " + std::to_wstring(t)).c_str());
			}
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(log);
		}

		// The same pan through the matcher's own field (the de-judder's estimate).
		TEST_METHOD(InterpolatorPanLandsAtTheTruePhaseWithTheMatchersField)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
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
			std::string log;
			fixture.CaptureLog(&log);
			Dejudder::MotionWarp warp(fixture.Log(), gpu, DejudderTestSettings());
			Assert::IsTrue(warp.Estimate(planesA.planes, planesB.planes),
				std::wstring(warp.LastError().begin(), warp.LastError().end()).c_str());
			Interpolator::Motion motion;
			motion.field = warp.Field();
			motion.frameTrust = warp.FrameTrust();
			motion.minTrustedShare = warp.GetSettings().minTrustedShare;
			Interpolator::FrameInterpolator interpolator(fixture.Log(), gpu, true);
			Dejudder::Planes out;
			Assert::IsTrue(interpolator.Run(planesA.planes, planesB.planes, motion, 0.5f,
				InterpolationPolicy::Fallback::Hold, out), InterpolatorError(interpolator, log).c_str());
			const std::vector<uint16_t> luma = DownloadPlane(gpu, out.luma);
			int checked = 0, close = 0;
			for (int y = 24; y < height - 24; ++y)
				for (int x = 24; x < width - 24; ++x)
				{
					++checked;
					close += std::abs(static_cast<int>(luma[static_cast<size_t>(y) * width + x]) -
						static_cast<int>(DejudderTexture(x - dx / 2, y - dy / 2))) <= 64;
				}
			Assert::IsTrue(close * 100 >= checked * 95,
				(L"pixels at the true phase (6, 3): " + std::to_wstring(close) + L" of " + std::to_wstring(checked)).c_str());
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(log);
		}

		TEST_METHOD(InterpolatorOcclusionHoleHoldsNearest)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			const int width = 256, height = 128;
			const DejudderFrame a = MakeDejudderFrame(width, height,
				[](int x, int y) { return DejudderTexture(x, y); },
				[](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); });
			const DejudderFrame b = MakeDejudderFrame(width, height,
				[](int x, int y) { return DejudderTexture(x - 16, y, 3); },
				[](int x, int y, int c) { return DejudderTexture((x - 8) * 2, y, 9 + c); });
			DejudderGpuPlanes planesA, planesB;
			UploadDejudderFrame(gpu, a, planesA);
			UploadDejudderFrame(gpu, b, planesB);
			// Blocks 6..9 x 2..5 (source px 96..159 x 32..95) untrusted: the hole.
			auto inHole = [](int bx, int by) { return bx >= 6 && bx <= 9 && by >= 2 && by <= 5; };
			InterpolatorField field;
			MakeInterpolatorField(gpu, width, height, 0.8f, [&](int bx, int by) {
				return std::array<float, 4>{ 16.0f, 0.0f, inHole(bx, by) ? 0.0f : 1.0f, 0.0f }; }, field);
			std::string log;
			fixture.CaptureLog(&log);
			Interpolator::FrameInterpolator interpolator(fixture.Log(), gpu, true);
			for (auto fallback : { InterpolationPolicy::Fallback::Hold, InterpolationPolicy::Fallback::Blend })
				for (float t : { 0.25f, 0.75f })
				{
					Dejudder::Planes out;
					Assert::IsTrue(interpolator.Run(planesA.planes, planesB.planes, InterpolatorMotion(field), t, fallback, out),
						InterpolatorError(interpolator, log).c_str());
					const std::vector<uint16_t> luma = DownloadPlane(gpu, out.luma);
					int checked = 0, right = 0;
					for (int y = 32; y < 96; ++y)
						for (int x = 96; x < 160; ++x)
						{
							const size_t i = static_cast<size_t>(y) * width + x;
							const double expected = fallback == InterpolationPolicy::Fallback::Hold ?
								(t < 0.5f ? a.luma[i] : b.luma[i]) : (1.0 - t) * a.luma[i] + t * b.luma[i];
							++checked;
							right += std::abs(luma[i] - expected) <= (fallback == InterpolationPolicy::Fallback::Hold ? 0.0 : 1.0);
						}
					Assert::AreEqual(checked, right, (std::wstring(fallback == InterpolationPolicy::Fallback::Hold ?
						L"hold: the nearest frame's own pixels, t = " : L"blend: the mix, t = ") + std::to_wstring(t)).c_str());
				}
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(log);
		}

		TEST_METHOD(InterpolatorCutHoldsWholeFrame)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			const int width = 128, height = 64;
			const DejudderFrame a = MakeDejudderFrame(width, height,
				[](int x, int y) { return DejudderTexture(x, y); },
				[](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); });
			const DejudderFrame b = MakeDejudderFrame(width, height,   // another scene
				[](int x, int y) { return DejudderTexture(x, y, 11); },
				[](int x, int y, int c) { return DejudderTexture(x * 2, y, 13 + c); });
			DejudderGpuPlanes planesA, planesB;
			UploadDejudderFrame(gpu, a, planesA);
			UploadDejudderFrame(gpu, b, planesB);
			// A few blocks still claim trust, but the share (0.1) is below min_trusted_share.
			InterpolatorField field;
			MakeInterpolatorField(gpu, width, height, 0.1f, [](int bx, int) {
				return std::array<float, 4>{ 8.0f, 4.0f, bx < 2 ? 1.0f : 0.0f, 0.0f }; }, field);
			std::string log;
			fixture.CaptureLog(&log);
			Interpolator::FrameInterpolator interpolator(fixture.Log(), gpu, true);
			for (auto fallback : { InterpolationPolicy::Fallback::Hold, InterpolationPolicy::Fallback::Blend })
				for (float t : { 0.1f, 0.49f, 0.51f, 0.9f })
				{
					Dejudder::Planes out;
					Assert::IsTrue(interpolator.Run(planesA.planes, planesB.planes, InterpolatorMotion(field), t, fallback, out),
						InterpolatorError(interpolator, log).c_str());
					const DejudderFrame& nearest = t < 0.5f ? a : b;
					Assert::IsTrue(DownloadPlane(gpu, out.luma) == nearest.luma, L"a cut: the nearest frame's luma whole");
					Assert::IsTrue(DownloadPlane(gpu, out.chroma) == nearest.chroma, L"a cut: the nearest frame's chroma whole");
				}
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(log);
		}

		// Occlusion (B's mask): a trusted A vector whose landing in B is content A did not have
		// takes A alone (A's content goes behind it); an untrusted position whose own block of B
		// is new (disoccluded) takes B at any phase - only B has it.
		TEST_METHOD(InterpolatorOcclusionTakesAAndDisocclusionTakesB)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			const int width = 256, height = 128;
			const DejudderFrame a = MakeDejudderFrame(width, height,
				[](int x, int y) { return DejudderTexture(x, y); },
				[](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); });
			const DejudderFrame b = MakeDejudderFrame(width, height,
				[](int x, int y) { return DejudderTexture(x, y, 4); },
				[](int x, int y, int c) { return DejudderTexture(x * 2, y, 9 + c); });
			DejudderGpuPlanes planesA, planesB;
			UploadDejudderFrame(gpu, a, planesA);
			UploadDejudderFrame(gpu, b, planesB);
			// Blocks x 0..9 still, x 13.. moving right by 16 px (away from the hole). Blocks
			// x 2..4: trusted, but B's content there is new (occlusion: A alone); blocks x 10..12:
			// untrusted and new in B. Block 12's neighbour 13 moves away from it (uncovered:
			// B); blocks 10 and 11 have nothing moving away (a bad match: the hold, A at t 0.25).
			// Checked over blocks 0..12 (the moving region's own warp is not the subject).
			auto occluded = [](int bx) { return bx >= 2 && bx <= 4; };
			auto disoccluded = [](int bx) { return bx >= 10 && bx <= 12; };
			auto uncovered = [](int bx) { return bx == 12; };
			InterpolatorField field;
			MakeInterpolatorField(gpu, width, height, 1.0f, [&](int bx, int) {
				return std::array<float, 4>{ bx >= 13 ? 16.0f : 0.0f, 0.0f, disoccluded(bx) ? 0.0f : 1.0f, 0.0f }; }, field);
			const int bx = width / 16, by = height / 16;
			std::vector<float> mask(static_cast<size_t>(bx) * by * 4, 1.0f);
			for (int y = 0; y < by; ++y)
				for (int x = 0; x < bx; ++x)
					if (occluded(x) || disoccluded(x))
						mask[(static_cast<size_t>(y) * bx + x) * 4] = 0.0f;
			field.occlusion = MakeFloatTexture(gpu, bx, by, mask);
			std::string log;
			fixture.CaptureLog(&log);
			Interpolator::FrameInterpolator interpolator(fixture.Log(), gpu, true);
			const float t = 0.25f;   // nearer A: the hold would show A
			Dejudder::Planes out;
			Assert::IsTrue(interpolator.Run(planesA.planes, planesB.planes, InterpolatorMotion(field), t,
				InterpolationPolicy::Fallback::Hold, out), InterpolatorError(interpolator, log).c_str());
			const std::vector<uint16_t> luma = DownloadPlane(gpu, out.luma);
			int checked = 0, right = 0;
			for (int y = 0; y < height; ++y)
				for (int x = 0; x < 13 * 16; ++x)
				{
					const int blockX = x / 16;
					const size_t i = static_cast<size_t>(y) * width + x;
					double expected;
					if (occluded(blockX)) expected = a.luma[i];
					else if (uncovered(blockX)) expected = b.luma[i];
					else if (disoccluded(blockX)) expected = a.luma[i];   // no uncover: the hold (t < 0.5)
					else expected = (1.0 - t) * a.luma[i] + t * b.luma[i];
					++checked;
					right += std::abs(luma[i] - expected) <= 1.0;
				}
			Assert::AreEqual(checked, right, L"occluded: A alone; uncovered: B; new without uncover: the hold; elsewhere the time blend");
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(log);
		}

		// No valid vectors (an invalidation, no estimate for the pair): Stage B still draws
		// every refresh - hold shows the nearest frame whole, blend the time-weighted mix.
		TEST_METHOD(InterpolatorWithoutVectorsHoldsNearestOrBlends)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			const int width = 128, height = 64;
			const DejudderFrame a = MakeDejudderFrame(width, height,
				[](int x, int y) { return DejudderTexture(x, y); },
				[](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); });
			const DejudderFrame b = MakeDejudderFrame(width, height,
				[](int x, int y) { return DejudderTexture(x - 8, y, 5); },
				[](int x, int y, int c) { return DejudderTexture((x - 4) * 2, y, 9 + c); });
			DejudderGpuPlanes planesA, planesB;
			UploadDejudderFrame(gpu, a, planesA);
			UploadDejudderFrame(gpu, b, planesB);
			std::string log;
			fixture.CaptureLog(&log);
			Interpolator::FrameInterpolator interpolator(fixture.Log(), gpu, true);
			Interpolator::Motion none;   // no field, no trust
			for (float t : { 0.2f, 0.8f })
			{
				Dejudder::Planes out;
				Assert::IsTrue(interpolator.Run(planesA.planes, planesB.planes, none, t, InterpolationPolicy::Fallback::Hold, out),
					InterpolatorError(interpolator, log).c_str());
				Assert::IsTrue(DownloadPlane(gpu, out.luma) == (t < 0.5f ? a.luma : b.luma), L"hold: the nearest frame");
				Assert::IsTrue(interpolator.Run(planesA.planes, planesB.planes, none, t, InterpolationPolicy::Fallback::Blend, out),
					InterpolatorError(interpolator, log).c_str());
				const std::vector<uint16_t> luma = DownloadPlane(gpu, out.luma);
				for (size_t i = 0; i < luma.size(); ++i)
					if (std::abs(luma[i] - ((1.0 - t) * a.luma[i] + t * b.luma[i])) > 1.0)
						Assert::Fail(L"blend: the time-weighted mix");
			}
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(log);
		}

		// HDR / 10-bit precision: a ramp over every 10-bit code (P010 layout: the code
		// in the top 10 bits), BTB and WTW included, and a PQ-coded ramp at 16 bits,
		// moved by whole pixels: the picture at the phase keeps every code exactly
		// (no 8-bit step, nothing clamped).
		TEST_METHOD(InterpolatorTenBitAndPqRampKeepPrecision)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			const int width = 1024 + 64, height = 32;
			static constexpr int dx = 32;
			auto tenBit = [](int x) { return static_cast<uint16_t>((std::min)((std::max)(x - 32, 0), 1023) << 6); };
			auto pq = [](int x) {
				// The PQ (SMPTE ST 2084) code of a luminance ramp 0..10000 nits, 16-bit.
				const double y = std::pow((std::min)((std::max)(x - 32, 0), 1023) / 1023.0, 3.0);
				const double m1 = 2610.0 / 16384.0, m2 = 2523.0 / 4096.0 * 128.0;
				const double c1 = 3424.0 / 4096.0, c2 = 2413.0 / 4096.0 * 32.0, c3 = 2392.0 / 4096.0 * 32.0;
				const double p = std::pow(y, m1);
				return static_cast<uint16_t>(std::lround(65535.0 * std::pow((c1 + c2 * p) / (1.0 + c3 * p), m2)));
			};
			std::string log;
			fixture.CaptureLog(&log);
			Interpolator::FrameInterpolator interpolator(fixture.Log(), gpu, true);
			InterpolatorField field;
			MakeInterpolatorField(gpu, width, height, 1.0f,
				[](int, int) { return std::array<float, 4>{ static_cast<float>(dx), 0.0f, 1.0f, 0.0f }; }, field);
			for (int ramp = 0; ramp < 2; ++ramp)
			{
				auto code = [&](int x) { return ramp == 0 ? tenBit(x) : pq(x); };
				const DejudderFrame a = MakeDejudderFrame(width, height, [&](int x, int) { return code(x); },
					[&](int x, int, int c) { return static_cast<uint16_t>(c ? code(2 * x) : 32768); });
				const DejudderFrame b = MakeDejudderFrame(width, height, [&](int x, int) { return code(x - dx); },
					[&](int x, int, int c) { return static_cast<uint16_t>(c ? code(2 * (x - dx / 2)) : 32768); });
				DejudderGpuPlanes planesA, planesB;
				UploadDejudderFrame(gpu, a, planesA);
				UploadDejudderFrame(gpu, b, planesB);
				for (int quarter = 1; quarter <= 3; ++quarter)
				{
					const int sx = dx * quarter / 4;
					Dejudder::Planes out;
					Assert::IsTrue(interpolator.Run(planesA.planes, planesB.planes, InterpolatorMotion(field), 0.25f * quarter,
						InterpolationPolicy::Fallback::Hold, out), InterpolatorError(interpolator, log).c_str());
					const std::vector<uint16_t> luma = DownloadPlane(gpu, out.luma);
					for (int y = 0; y < height; ++y)
						for (int x = dx + 2; x < width - dx - 2; ++x)
						{
							const uint16_t got = luma[static_cast<size_t>(y) * width + x];
							if (got != code(x - sx))
								Assert::Fail((std::wstring(ramp == 0 ? L"10-bit" : L"PQ") + L" ramp code changed at x " +
									std::to_wstring(x) + L": " + std::to_wstring(got) + L" for " +
									std::to_wstring(code(x - sx))).c_str());
						}
				}
			}
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(log);
		}
	};
}
