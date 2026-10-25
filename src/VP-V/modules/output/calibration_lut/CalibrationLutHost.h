#pragma once

#include "CalibrationLutHostPolicy.h"
#include "CalibrationLutStage.h"
#include "calibration/CalibrationPattern.h"

#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/modules/drive/led_compensation/LedRawTransfer.h>
#include <vprenderer/LibplaceboDisplayLut.h>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <libplacebo/renderer.h>
#include <libplacebo/swapchain.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

// The VP calibration glue of the renderer host, as ONE adapter object owned by the renderer's
// Impl (WP-12 of the integration batch; central-migration-needs.md option (d), step b3; rule
// "host = call sites only"). LibplaceboVideoRenderer.cpp keeps one call per hook point
// (docs/design/wp12-calib-callsites.md lists them):
//
//  - output.calibration_lut, the VP stage (cluster 14, V part): which stage takes the LUT this
//    frame, the hook the render path pushes, the stage failure check after the render, the
//    "attached" status and the LUT status text. The LUT file itself (load, reload, reject, the
//    libplacebo target LUT) is Bill's display-LUT code (LibplaceboDisplayLut.*,
//    LibplaceboCalibrationLutPolicy.h, the renderer's LoadDisplayLut / ConfigureDisplayLutForTarget)
//    and stays where he has it: this adapter only receives the parsed LUT.
//  - the automatic-calibration patterns (cluster 10, V part): the patch request, the patch
//    presented instead of video (exact codes written to the backbuffer), the test readback, and
//    the output facts the calibration session reads (cluster 23, calibration getters), including
//    VP's tone curve for the HDR verify.
//
// The segment runner (feat/segment-runner) calls the VP calibration LUT as the LAST stage of S2:
// it holds the stage object by reference (Stage()), so S2 and this adapter share one hook.
//
// Threading: the LUT-stage calls and Present / NoteVideoTarget / NoteToneCurve run on the render
// thread. SetPattern / EndPattern / ResetPatternLogs run on the RPC thread (the session's sink);
// PublishOutputFacts runs with the renderer's renderMutex held; FillOutputInfo on any thread.
// Each piece of cross-thread state has its own mutex or is atomic, as before the move.
namespace CalibrationLutHost
{
	// The swapchain the patch is presented on (the renderer's; the adapter never owns it).
	struct Surface
	{
		pl_gpu gpu = nullptr;
		IDXGISwapChain1* vpOwned = nullptr;   // VP's own flip swapchain, or null: libplacebo's swapchain
		pl_swapchain swapchain = nullptr;
		DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
		// Reads a 10-bit backbuffer (tests' PatternReadback; null texture = libplacebo's current one).
		std::function<bool(ID3D11Texture2D*, std::vector<uint32_t>&, D3D11_TEXTURE2D_DESC&, std::string&)> readR10;
	};

	struct PresentResult
	{
		bool presented = false;
		// "calibration patch": the windowed patch fell back to full field on this surface.
		FallbackCatalog::Decision patchWindow;
	};

	// The render-thread format facts GetCalibrationOutput reports (read under renderMutex).
	struct OutputFacts
	{
		DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
		std::string displayBitDepth;
		bool limitedRange = true;
		bool bt2020 = false;
		double refreshHz = 0.0;
		bool available = false;
	};

	// TakeLut's answer. vpStage = the VP stage applies the LUT this frame (the host leaves target.lut
	// empty and returns); statusChanged = the LUT status text changed (Bill's beta publishes it to
	// the OSD telemetry: SetDisplayLutStatus).
	struct TakeResult
	{
		bool vpStage = false;
		bool statusChanged = false;
		explicit operator bool() const { return vpStage; }
	};

	class Adapter
	{
	public:
		Adapter() = default;
		Adapter(const Adapter&) = delete;
		Adapter& operator=(const Adapter&) = delete;

		// ---- output.calibration_lut: the VP stage -------------------------------------------------

		// The stage object (S2's last stage holds it by reference).
		CalibrationLutStage::Hook& Stage() { return stage; }

		// ConfigureDisplayLutForTarget, one call before its libplacebo branch. lut = the parsed LUT
		// (null: none this frame). True = the VP stage applies the LUT this frame; the host then leaves
		// target.lut empty and returns. Updates `status` (the renderer's LUT status) for the VP stage.
		TakeResult TakeLut(const pl_custom_lut* lut, const StageFacts& facts, const std::string& path, std::string& status);

		// The render path: the hook to push after the user PRE_OUTPUT shaders, or null.
		const pl_hook* HookForFrame();

		// After the render, with libplacebo's runtime errors. An engaged decision = the VP stage failed
		// (it falls back to libplacebo's target LUT for this renderer instance).
		FallbackCatalog::Decision AfterRender(const pl_render_errors& errors);

		// After RejectDisplayLutAfterRenderFailure (Bill's: the LUT is dropped for the renderer
		// instance): the "Fallbacks engaged" entry, so the drop is never log-only (ADR-0001).
		FallbackCatalog::Decision LutDropped(const std::string& path) const;

		// The "calibration LUT attached" status for this frame.
		bool LutAttached(bool rendered, bool targetLutApplied) const;

		bool StageFailed() const { return stageFailed; }

		// ---- LUT file reload policy (VP's auto reload vs Bill's explicit Apply; ReloadPolicy) ----

		ReloadPolicy& Reload() { return reload; }
		// LoadDisplayLut: the file version the load read (the same handle it parsed).
		void NoteLutFileVersion(const LibplaceboDisplayLut::FileVersion& version, uint64_t nowMs);
		// MaybeReloadDisplayLut, render thread, every frame: true = reload the selected LUT now
		// (auto mode: the file changed in place; any mode: VP's calibration applied a LUT).
		bool LutReloadDue(const std::string& path, bool enabled, uint64_t nowMs);

		// With the renderer's other GPU resources (before the GPU goes).
		void ReleaseGpu(pl_gpu gpu);

		// ---- automatic calibration: patterns ------------------------------------------------------

		void SetPattern(const Calibration::PatternRequest& request);
		void EndPattern();
		void ResetPatternLogs();

		// One refresh of the current patch (the caller holds renderMutex and has checked that the
		// output exists).
		PresentResult Present(const Surface& surface);

		// The last video frame's target transfer (pl_color_transfer).
		void NoteVideoTarget(int transfer) { targetTransfer.store(transfer, std::memory_order_release); }

		// Phase 2 (HDR verify): VP's tone curve for this frame's colour mapping (PQ targets only),
		// rebuilt only when the mapping changes.
		void NoteToneCurve(const pl_color_space& source, const pl_color_space& target, const pl_color_map_params* map);

		// GetCalibrationOutput: whether to try the render-thread facts now (patternsActive = a patch
		// is being shown), the facts read under renderMutex, and the answer.
		int FreshOutputAttempts(bool patternsActive) const;
		void PublishOutputFacts(const OutputFacts& facts);
		// Fills info from the cache and the atomics. fresh = facts were published by this call.
		// False with a reason when nothing is known yet; otherwise info.available.
		bool FillOutputInfo(Calibration::PatternOutputInfo& info, bool fresh) const;

	private:
		bool FillPattern(pl_gpu gpu, pl_tex fbo, DXGI_FORMAT format, const Calibration::PatternRequest& pattern,
			bool& windowFallback, std::string& why);
		void ReadBack(const Surface& surface, ID3D11Texture2D* backbuffer, uint64_t generation);

		// LUT stage (render thread)
		CalibrationLutStage::Hook stage;
		bool stageLogged = false;
		bool stageFailed = false;   // the VP stage failed once: libplacebo's target LUT for the instance
		ReloadPolicy reload;

		// Patterns
		std::mutex patternMutex;
		Calibration::PatternRequest pattern;
		std::atomic<uint64_t> presentedGeneration{ 0 };
		std::atomic<uint64_t> presentsOfGeneration{ 0 };
		std::atomic<uint32_t> surfaceWidth{ 0 };
		std::atomic<uint32_t> surfaceHeight{ 0 };
		std::atomic<int> targetTransfer{ -1 };   // pl_color_transfer of the last video frame's target
		pl_tex patchTexture = nullptr;
		std::atomic<bool> windowFallbackLogged{ false };
		std::atomic<bool> failureLogged{ false };
		mutable std::mutex outputCacheMutex;
		Calibration::PatternOutputInfo outputCache;
		std::atomic<uint64_t> readbackGeneration{ 0 };
		std::atomic<uint32_t> readbackCenter{ 0 };   // R10G10B10A2 words
		std::atomic<uint32_t> readbackCorner{ 0 };
		uint64_t toneSignature = 0;
		mutable std::mutex toneMutex;
		LedDrive::ToneCurveSamples tone;
		pl_hdr_metadata sentHdr{};
	};
}
