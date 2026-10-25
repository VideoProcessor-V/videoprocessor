#include <pch.h>
#include "CalibrationLutHost.h"

#include <DebugLog.h>

#include <libplacebo/d3d11.h>
#include <libplacebo/shaders/colorspace.h>

#include <atlbase.h>

#include <cstring>

namespace CalibrationLutHost
{
	// ---- output.calibration_lut: the VP stage ----------------------------------------------------

	TakeResult Adapter::TakeLut(const pl_custom_lut* lut, const StageFacts& facts, const std::string& path,
		std::string& status)
	{
		TakeResult result;
		stage.SetLut(nullptr);
		if (!lut)
			return result;
		// R1 (calibration_lut_stage = vp): VP applies the LUT as its last colour hook, after the
		// LED compensation, instead of libplacebo's target LUT (which runs before VP's hooks).
		// The Original pathway keeps libplacebo's target LUT, as upstream did.
		if (UseVpStage(facts, stageFailed))
		{
			stage.SetLut(lut);
			result.vpStage = true;
			result.statusChanged = VpStageStatus(status, FileName(path));
			if (!stageLogged)
				DebugLog::Log("display calibration LUT: applied by VP as its last colour hook (calibration_lut_stage vp; "
					"after the LED compensation; BTB/WTW carried at slope 1)");
			stageLogged = true;
			return result;
		}
		if (stageLogged)
			DebugLog::Log("display calibration LUT: libplacebo target LUT (calibration_lut_stage libplacebo)");
		stageLogged = false;
		return result;
	}

	const pl_hook* Adapter::HookForFrame()
	{
		return stage.Lut() ? stage.Get() : nullptr;
	}

	FallbackCatalog::Decision Adapter::AfterRender(const pl_render_errors& errors)
	{
		// A disabled or failing hook reverts to libplacebo's target LUT for the rest of the
		// renderer instance, visibly (skeleton audit item 5).
		FallbackCatalog::Decision decision;
		decision.id = "calibration LUT";
		if (stageFailed || !stage.Lut())
			return decision;
		bool disabled = stage.Failures() > 0;
		for (int i = 0; !disabled && i < errors.num_disabled_hooks; ++i)
			disabled = errors.disabled_hooks[i] == stage.Get()->signature;
		if (!disabled)
			return decision;
		stageFailed = true;
		return FallbackCatalog::Engaged("calibration LUT",
			"the VP stage failed; libplacebo's target LUT (before the LED compensation)");
	}

	bool Adapter::LutAttached(bool rendered, bool targetLutApplied) const
	{
		return CalibrationLutHost::LutAttached(rendered, targetLutApplied, stage.Lut() != nullptr);
	}

	void Adapter::NoteLutFileVersion(const LibplaceboDisplayLut::FileVersion& version, uint64_t nowMs)
	{
		reload.NoteLoaded(version.available, version.fileBytes, version.fileWriteTime, nowMs);
	}

	bool Adapter::LutReloadDue(const std::string& path, bool enabled, uint64_t nowMs)
	{
		if (reload.TakeCalibrationReload() && !path.empty() && enabled)
		{
			DebugLog::Log("display calibration LUT: %s: file=%s", ReloadPolicy::CalibrationReloadNotice(), path.c_str());
			return true;
		}
		if (!reload.ProbeDue(!path.empty(), enabled, nowMs))
			return false;
		const LibplaceboDisplayLut::FileVersion observed = LibplaceboDisplayLut::ProbeFileVersion(path);
		if (!reload.Changed(observed.available, observed.fileBytes, observed.fileWriteTime))
			return false;
		DebugLog::Log(
			"display calibration LUT file changed in place: file=%s; validating transactional reload",
			path.c_str());
		return true;
	}

	void Adapter::ReleaseGpu(pl_gpu gpu)
	{
		if (gpu)
			pl_tex_destroy(gpu, &patchTexture);
		stage.Release();   // R1: the VP calibration stage's LUT texture
	}

	// ---- automatic calibration: patterns (docs/calibration/AUTO-CALIBRATION.md) -------------------
	//
	// The patch the render loop presents instead of video, as raw codes written to the backbuffer.
	// No libplacebo pass runs for it, so the sigma-delta, dither, LUT, tone / gamut mapping and every
	// hook are bypassed for the patch only; the swapchain, its DXGI colour space and HDR signalling
	// stay as they are.

	void Adapter::SetPattern(const Calibration::PatternRequest& request)
	{
		std::lock_guard<std::mutex> guard(patternMutex);
		pattern = request;
	}

	void Adapter::EndPattern()
	{
		std::lock_guard<std::mutex> guard(patternMutex);
		pattern.active = false;
	}

	void Adapter::ResetPatternLogs()
	{
		windowFallbackLogged.store(false, std::memory_order_release);
		failureLogged.store(false, std::memory_order_release);
	}

	// Tests (PatternReadback): the filled backbuffer's centre and corner pixels.
	void Adapter::ReadBack(const Surface& surface, ID3D11Texture2D* backbuffer, uint64_t generation)
	{
		std::vector<uint32_t> pixels;
		D3D11_TEXTURE2D_DESC desc{};
		std::string error = "no reader";
		if (!surface.readR10 || !surface.readR10(backbuffer, pixels, desc, error) || pixels.empty())
		{
			DebugLog::Log("Calibration patterns: readback unavailable (%s)", error.c_str());
			return;
		}
		readbackCenter.store(pixels[static_cast<size_t>(desc.Height / 2) * desc.Width + desc.Width / 2],
			std::memory_order_release);
		readbackCorner.store(pixels[0], std::memory_order_release);
		readbackGeneration.store(generation, std::memory_order_release);
	}

	// Writes the patch into fbo: the background, then the window (a cleared texture of the same
	// format blitted 1:1, so the codes stay exact).
	bool Adapter::FillPattern(pl_gpu gpu, pl_tex fbo, DXGI_FORMAT format, const Calibration::PatternRequest& request,
		bool& windowFallback, std::string& why)
	{
		windowFallback = false;
		if (!fbo || !gpu) return false;
		const double maxValue = PatchMaxCode(format);
		if (!(maxValue > 0.0) || !fbo->params.renderable) return false;
		const float patch[4] = {
			static_cast<float>(request.patch[0] / maxValue), static_cast<float>(request.patch[1] / maxValue),
			static_cast<float>(request.patch[2] / maxValue), 1.0f };
		const float background[4] = {
			static_cast<float>(request.background[0] / maxValue), static_cast<float>(request.background[1] / maxValue),
			static_cast<float>(request.background[2] / maxValue), 1.0f };
		const int width = fbo->params.w, height = fbo->params.h;
		const Calibration::PixelRect rect = Calibration::PatchWindowRect(width, height, request.windowAreaPercent);
		const bool fullField = rect.left == 0 && rect.top == 0 && rect.right == width && rect.bottom == height;
		if (fullField)
		{
			pl_tex_clear(gpu, fbo, patch);
			return true;
		}
		bool windowed = fbo->params.blit_dst;
		if (!windowed)
			why = "the surface takes no blit";
		if (windowed && (!patchTexture || patchTexture->params.w != rect.Width() ||
			patchTexture->params.h != rect.Height() || patchTexture->params.format != fbo->params.format))
		{
			pl_tex_destroy(gpu, &patchTexture);
			pl_tex_params params{};
			params.w = rect.Width();
			params.h = rect.Height();
			params.format = fbo->params.format;
			params.renderable = true;
			params.blit_src = true;
			patchTexture = pl_tex_create(gpu, &params);
			windowed = patchTexture != nullptr;
			if (!windowed)
				why = "the patch texture could not be created";
		}
		if (!windowed)
		{
			// The surface cannot take a blit: full field (exact codes still).
			windowFallback = true;
			if (!windowFallbackLogged.exchange(true, std::memory_order_acq_rel))
				DebugLog::Log("Calibration patterns: windowed patch unavailable on this surface (blit_dst=%d); full field shown",
					fbo->params.blit_dst ? 1 : 0);
			pl_tex_clear(gpu, fbo, patch);
			return true;
		}
		pl_tex_clear(gpu, fbo, background);
		pl_tex_clear(gpu, patchTexture, patch);
		pl_tex_blit_params blit{};
		blit.src = patchTexture;
		blit.dst = fbo;
		blit.src_rc.x0 = 0; blit.src_rc.y0 = 0; blit.src_rc.z0 = 0;
		blit.src_rc.x1 = rect.Width(); blit.src_rc.y1 = rect.Height(); blit.src_rc.z1 = 1;
		blit.dst_rc.x0 = rect.left; blit.dst_rc.y0 = rect.top; blit.dst_rc.z0 = 0;
		blit.dst_rc.x1 = rect.right; blit.dst_rc.y1 = rect.bottom; blit.dst_rc.z1 = 1;
		blit.sample_mode = PL_TEX_SAMPLE_NEAREST;
		pl_tex_blit(gpu, &blit);
		return true;
	}

	PresentResult Adapter::Present(const Surface& surface)
	{
		PresentResult result;
		result.patchWindow.id = "calibration patch";
		if (!surface.gpu || !surface.swapchain)
			return result;
		Calibration::PatternRequest request;
		{
			std::lock_guard<std::mutex> guard(patternMutex);
			request = pattern;
		}
		if (!request.active)
			return result;
		bool filled = false;
		bool presented = false;
		bool windowFallback = false;
		std::string why;
		uint32_t width = 0, height = 0;
		if (surface.vpOwned)
		{
			CComPtr<ID3D11Texture2D> backbuffer;
			if (FAILED(surface.vpOwned->GetBuffer(0, IID_PPV_ARGS(&backbuffer))) || !backbuffer)
				return result;
			pl_d3d11_wrap_params wrapParams{};
			wrapParams.tex = static_cast<ID3D11Resource*>(backbuffer.p);
			pl_tex targetTexture = pl_d3d11_wrap(surface.gpu, &wrapParams);
			if (targetTexture)
			{
				width = static_cast<uint32_t>(targetTexture->params.w);
				height = static_cast<uint32_t>(targetTexture->params.h);
				filled = FillPattern(surface.gpu, targetTexture, surface.format, request, windowFallback, why);
				pl_gpu_flush(surface.gpu);
				pl_tex_destroy(surface.gpu, &targetTexture);
				if (filled && (request.flags & Calibration::PatternReadback) &&
					readbackGeneration.load(std::memory_order_acquire) != request.generation)
					ReadBack(surface, backbuffer, request.generation);
			}
			backbuffer.Release();
			if (filled)
				presented = SUCCEEDED(surface.vpOwned->Present(1, 0));
		}
		else
		{
			struct pl_swapchain_frame frame{};
			if (!pl_swapchain_start_frame(surface.swapchain, &frame))
				return result;
			if (frame.fbo)
			{
				width = static_cast<uint32_t>(frame.fbo->params.w);
				height = static_cast<uint32_t>(frame.fbo->params.h);
			}
			filled = FillPattern(surface.gpu, frame.fbo, surface.format, request, windowFallback, why);
			if (filled && (request.flags & Calibration::PatternReadback) &&
				readbackGeneration.load(std::memory_order_acquire) != request.generation)
			{
				pl_gpu_flush(surface.gpu);
				ReadBack(surface, nullptr, request.generation);
			}
			if (!filled)
			{
				struct pl_frame target{};
				pl_frame_from_swapchain(&target, &frame);
				const float black[] = { 0.0f, 0.0f, 0.0f };
				pl_frame_clear(surface.gpu, &target, black);
			}
			presented = pl_swapchain_submit_frame(surface.swapchain);
			if (presented)
				pl_swapchain_swap_buffers(surface.swapchain);
			presented = presented && filled;
		}
		if (!presented)
		{
			if (!failureLogged.exchange(true, std::memory_order_acq_rel))
				DebugLog::Log("Calibration patterns: present failed (filled=%d format=%d)", filled ? 1 : 0,
					static_cast<int>(surface.format));
			return result;
		}
		surfaceWidth.store(width, std::memory_order_release);
		surfaceHeight.store(height, std::memory_order_release);
		if (presentedGeneration.load(std::memory_order_acquire) != request.generation)
		{
			presentsOfGeneration.store(0, std::memory_order_release);
			presentedGeneration.store(request.generation, std::memory_order_release);
		}
		presentsOfGeneration.fetch_add(1, std::memory_order_acq_rel);
		result.presented = true;
		(void)windowFallback;
		return result;
	}

	void Adapter::NoteToneCurve(const pl_color_space& source, const pl_color_space& target, const pl_color_map_params* map)
	{
		if (target.transfer != PL_COLOR_TRC_PQ)
			return;
		const pl_color_map_params* toneColorMap = map ? map : &pl_color_map_default_params;
		const uint64_t signature = LedDrive::RawTransferSignature(source, target, toneColorMap);
		if (signature == toneSignature)
			return;
		toneSignature = signature;
		const LedDrive::ToneCurveSamples samples = LedDrive::SampleToneCurve(source, target, toneColorMap);
		std::lock_guard<std::mutex> guard(toneMutex);
		tone = samples;
		sentHdr = target.hdr;
	}

	bool Adapter::WantFreshOutputFacts(bool patternsActive) const
	{
		uint32_t cached = 0;
		{
			std::lock_guard<std::mutex> guard(outputCacheMutex);
			cached = outputCache.containerBits;
		}
		return CalibrationLutHost::WantFreshOutputFacts(patternsActive, cached);
	}

	void Adapter::PublishOutputFacts(const OutputFacts& facts)
	{
		Calibration::PatternOutputInfo current;
		current.containerBits = ContainerBits(facts.format);
		current.codeBits = CodeBits(current.containerBits, facts.displayBitDepth);
		current.limitedRange = facts.limitedRange;
		current.bt2020 = facts.bt2020;
		const int transfer = targetTransfer.load(std::memory_order_acquire);
		current.transfer = transfer == PL_COLOR_TRC_PQ ? Calibration::SignalTransfer::Pq :
			transfer == PL_COLOR_TRC_HLG ? Calibration::SignalTransfer::Hlg : Calibration::SignalTransfer::Sdr;
		current.refreshHz = facts.refreshHz;
		current.available = facts.available;
		if (!current.available) strncpy_s(current.reason, sizeof(current.reason), "the output is not ready", _TRUNCATE);
		std::lock_guard<std::mutex> guard(outputCacheMutex);
		outputCache = current;
	}

	bool Adapter::FillOutputInfo(Calibration::PatternOutputInfo& info, bool fresh) const
	{
		{
			std::lock_guard<std::mutex> guard(outputCacheMutex);
			info = outputCache;
		}
		if (!fresh && info.containerBits == 0)
		{
			strncpy_s(info.reason, sizeof(info.reason), "the VP Renderer is busy; try again", _TRUNCATE);
			return false;
		}
		info.width = surfaceWidth.load(std::memory_order_acquire);
		info.height = surfaceHeight.load(std::memory_order_acquire);
		info.presentedGeneration = presentedGeneration.load(std::memory_order_acquire);
		info.presentsOfGeneration = presentsOfGeneration.load(std::memory_order_acquire);
		info.readbackGeneration = readbackGeneration.load(std::memory_order_acquire);
		const uint32_t center = readbackCenter.load(std::memory_order_acquire);
		const uint32_t corner = readbackCorner.load(std::memory_order_acquire);
		for (int c = 0; c < 3; ++c)
		{
			info.readbackCenter[c] = static_cast<uint16_t>((center >> (10 * c)) & 0x3FF);
			info.readbackCorner[c] = static_cast<uint16_t>((corner >> (10 * c)) & 0x3FF);
		}
		if (info.transfer == Calibration::SignalTransfer::Pq)
		{
			std::lock_guard<std::mutex> guard(toneMutex);
			const uint32_t n = (std::min)(static_cast<uint32_t>((std::max)(tone.count, 0)),
				Calibration::PatternOutputInfo::ToneCurveMax);
			info.toneCurveCount = n;
			for (uint32_t i = 0; i < n; ++i)
			{
				info.toneCurveSourceNits[i] = tone.sourceNits[i];
				info.toneCurveOutputNits[i] = tone.outputNits[i];
			}
			info.sourcePeakNits = tone.sourcePeak;
			info.targetPeakNits = tone.targetPeak;
			info.targetBlackNits = tone.targetBlack;
			info.toneMappingActive = tone.active;
			strncpy_s(info.toneFunction, sizeof(info.toneFunction), tone.function ? tone.function : "", _TRUNCATE);
			info.sentMaxLuma = sentHdr.max_luma;
			info.sentMinLuma = sentHdr.min_luma;
			info.sentMaxCll = sentHdr.max_cll;
			info.sentMaxFall = sentHdr.max_fall;
		}
		return info.available;
	}
}
