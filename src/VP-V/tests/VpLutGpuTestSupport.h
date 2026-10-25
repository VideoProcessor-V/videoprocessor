#pragma once
// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Shared support for the VP-V GPU hook tests (moved out of Bill's LibplaceboLutParserTests.cpp):
// the includes, the WARP/hardware GPU fixture and the helper functions those tests use.
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
#include <vprenderer/MotionAheadProducer.h>
#include <vprenderer/FrameInterpolator.h>
#include <vprenderer/DejudderAhead.h>
#include <vprenderer/GpuCaptureUnpack.h>
#include <vprenderer/GpuRgbUnpack.h>
#include <vprenderer/GpuDolbyVisionTunnelUnpack.h>
#include <vprenderer/DolbyVisionTunnelUnpackReference.h>
#include <video_frame_formatter/CR210DolbyVisionTunnelFormatter.h>
#include "DolbyVisionTunnelSender.h"
#include <RawCaptureCorpus.h>
#include <vprenderer/EarlyUpload.h>
#include <vprenderer/SourceRebase.h>
#include <VP-V/modules/scaling/rebase/KernelInverse.h>
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
#include <libplacebo/shaders/colorspace.h>
#include <VP-V/modules/colour/sdr_tone_shape/DvDolbyStyle.h>   // feat/dv-l8: L8 / ICtCp shader parity
#include <vprenderer/OcclusionMaskHook.h>
#include <vprenderer/SigmaDeltaDomain.h>
#include <vprenderer/ExtendedRange.h>
#include <vprenderer/SdrToneShape.h>
#include <vprenderer/DolbyVisionTrims.h>
#include <vprenderer/RenderedWarp.h>
#include <vprenderer/AdaptiveDownscale.h>
#include "ExtendedRangeGpuProbe.h"
#include "HardwarePrerequisites.h"
#include "ChainGpuProbe.h"
#include "SigmaDeltaBiasProbe.h"
#include "GrainReferenceModel.h"
#include <memory>
#include <vprenderer/NlsMaskMapping.h>
#include <VP-V/modules/drive/led_measure/LedMeasurementHook.h>
#include <VP-V/modules/drive/led_compensation/LedRawTransfer.h>
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
		// src/VP-V/tests/<this file> -> the repository root (four levels; three when it lived in
		// src/VideoProcessor-Test before feat/lut-tests-out moved it).
		std::string path = __FILE__;
		for (int level = 0; level < 4; ++level)
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

namespace
{


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

		static Dejudder::Settings DejudderTestSettings()
		{
			Dejudder::Settings settings;
			settings.hostReadable = true;
			// Integration 2026-10-17: the object's default source warp became Catmull-Rom (the
			// faithful grain path). These object-level warp tests were written against the
			// bilinear source warp, so they keep it explicitly; the grain tests set catmull.
			settings.sourceWarpCatmull = false;
			return settings;
		}

		// 2026-10-17 (the engine's extras): a pair "computed ahead" (a synthetic engine field with
		// costs, no engine needed) through the de-judder. Unsure blocks (a high cost) lose their
		// trust; with every block as sure as usual, or the switches off, or no pair ahead, the
		// field is exactly the one without the extras. The engine's whole-frame motion falls
		// back to the global search when it is not clear.
		static std::shared_ptr<MotionAhead::Flow> SyntheticAheadFlow(int width, int height, int dxHalf, uint8_t unsureCost,
			int cellX0, int cellY0, int cellX1, int cellY1)
		{
			auto flow = std::make_shared<MotionAhead::Flow>();
			flow->halfW = (width + 1) / 2;
			flow->halfH = (height + 1) / 2;
			flow->gridW = (flow->halfW + 3) / 4;
			flow->gridH = (flow->halfH + 3) / 4;
			const size_t cells = static_cast<size_t>(flow->gridW) * flow->gridH;
			flow->forward.assign(cells * 2, 0);
			flow->backward.assign(cells * 2, 0);
			flow->forwardCost.assign(cells, 10);
			for (size_t i = 0; i < cells; ++i)
			{
				flow->forward[i * 2] = static_cast<int16_t>(dxHalf * 32);
				flow->backward[i * 2] = static_cast<int16_t>(-dxHalf * 32);
				const int x = static_cast<int>(i % flow->gridW), y = static_cast<int>(i / flow->gridW);
				if (x >= cellX0 && x < cellX1 && y >= cellY0 && y < cellY1) flow->forwardCost[i] = unsureCost;
			}
			flow->backwardCost = flow->forwardCost;
			MotionAhead::FlowGlobal(flow->forward, flow->forwardCost, flow->gridW, flow->gridH, flow->global[0], flow->second[0]);
			MotionAhead::FlowGlobal(flow->backward, flow->backwardCost, flow->gridW, flow->gridH, flow->global[1], flow->second[1]);
			MotionAhead::FlowExtras(*flow);
			return flow;
		}

		// CPU model of the shader's quantiser on a static pixel: the mean of the
		// emitted codes over many refreshes, with the shader's IGN dither.
		static double SigmaDeltaMean(double x, double amplitude, const SigmaDeltaDomain::CodeLimits& limits,
			int refreshes, double* lo = nullptr, double* hi = nullptr, float px = 37.0f, float py = 11.0f)
		{
			double r = 0.0, sum = 0.0;
			double mn = 1e9, mx = -1e9;
			for (int f = 0; f < refreshes; ++f)
			{
				const double d = (SigmaDeltaDomain::Ign(px, py, f) - 0.5) * 2.0 * amplitude;
				const double k = SigmaDeltaDomain::QuantiseStep(x, r, d, limits, amplitude);
				sum += k;
				mn = std::min(mn, k);
				mx = std::max(mx, k);
			}
			if (lo) *lo = mn;
			if (hi) *hi = mx;
			return sum / refreshes;
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
}
