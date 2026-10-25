#pragma once

#include <dxgiformat.h>

#include <algorithm>
#include <cstdint>
#include <string>

// The decisions of the calibration host adapter (CalibrationLutHost.h), kept pure so the CPU tests
// cover them (tests/CalibrationLutHostTests.cpp). WP-12 (integration batch): the calibration glue
// moved out of LibplaceboVideoRenderer.cpp; the renderer keeps one call per hook point.
//
// Header-only, C++14, no libplacebo, no Windows beyond the DXGI format enum.
namespace CalibrationLutHost
{
	// ---- the VP stage (output.calibration_lut, calibration_lut_stage = vp; R1) ------------------

	// Facts the renderer already has when it configures the frame's target.
	struct StageFacts
	{
		bool vpStageWanted = false;     // calibration_lut_stage == "vp"
		bool originalPathway = false;   // pipeline_original_pathway on: always libplacebo's target LUT
		bool slotRuns = false;          // pipelineSlots.Runs(OutputCalibrationLut)
	};

	// The VP stage takes the LUT when it is wanted, the Original pathway is off, the slot runs and
	// the stage has not failed in this renderer instance. Otherwise libplacebo's target LUT.
	inline bool UseVpStage(const StageFacts& facts, bool stageFailed)
	{
		return facts.vpStageWanted && !facts.originalPathway && facts.slotRuns && !stageFailed;
	}

	constexpr const char* kVpStagePrefix = "Active (VP stage): ";

	inline bool StartsWith(const std::string& text, const char* prefix)
	{
		return text.rfind(prefix, 0) == 0;
	}

	// The LUT status while the VP stage applies the LUT. Changes `status` only when it differs (no
	// per-frame allocation once set). Returns true when it changed.
	inline bool VpStageStatus(std::string& status, const std::string& fileName)
	{
		const std::string::size_type prefix = std::char_traits<char>::length(kVpStagePrefix);
		if (status.size() == prefix + fileName.size() && StartsWith(status, kVpStagePrefix) &&
			status.compare(prefix, std::string::npos, fileName) == 0)
			return false;
		status = kVpStagePrefix + fileName;
		return true;
	}

	// The status "calibration LUT attached" (Ctrl+I limited-range text, "(inactive: calibration LUT)",
	// ActiveProfileStatus, Config's usable-LUT check).
	inline bool LutAttached(bool rendered, bool targetLutApplied, bool vpStagePushed)
	{
		(void)vpStagePushed;
		return rendered && targetLutApplied;
	}

	inline std::string FileName(const std::string& path)
	{
		const std::string::size_type separator = path.find_last_of("\\/");
		return separator == std::string::npos ? path : path.substr(separator + 1);
	}

	// ---- LUT file reload: VP's automatic reload vs Bill's explicit Apply (either/or) -------------
	//
	// VP (Auto, today's default): the renderer probes the selected LUT file once a second and
	// reloads it transactionally when it changed (a failed reload keeps the last good LUT).
	// Bill's upstream (ExplicitApply, arrives with the Bill merge: LibplaceboDisplayLut::AppliedCache,
	// ApplyPendingDisplayLutReloadLocked): files are read once per Config Apply; editing a file alone
	// never reloads it. Owner rule vp-either-or-overlaps: both stay, as a choice (planned key
	// calibration_lut_reload = auto | apply; the Original pathway selects apply). Owner decision 4:
	// VP's own calibration may load the LUT it has just written without a Config Apply, with a
	// notice, in either mode (RequestCalibrationReload).
	enum class ReloadMode
	{
		Auto,
		ExplicitApply,
	};

	class ReloadPolicy
	{
	public:
		static constexpr uint64_t kProbeIntervalMs = 1000;

		void SetMode(ReloadMode value) { mode = value; }
		ReloadMode Mode() const { return mode; }

		// The renderer loaded (or tried) the file: the version it read, and when.
		void NoteLoaded(bool available, uint64_t fileBytes, uint64_t fileWriteTime, uint64_t nowMs)
		{
			observedAvailable = available;
			observedBytes = fileBytes;
			observedWriteTime = fileWriteTime;
			nextProbeMs = nowMs + kProbeIntervalMs;
		}

		// Auto mode, render thread, every frame: true once a second while a LUT file is selected.
		bool ProbeDue(bool haveFile, bool enabled, uint64_t nowMs)
		{
			if (mode != ReloadMode::Auto || !haveFile || !enabled || nowMs < nextProbeMs)
				return false;
			nextProbeMs = nowMs + kProbeIntervalMs;
			return true;
		}

		// The probed version differs from the one last loaded (LibplaceboDisplayLut::SameFileVersion).
		bool Changed(bool available, uint64_t fileBytes, uint64_t fileWriteTime) const
		{
			return !(available == observedAvailable && fileBytes == observedBytes && fileWriteTime == observedWriteTime);
		}

		// VP's calibration applied a finished LUT (any thread).
		void RequestCalibrationReload() { calibrationReload = true; }
		// Render thread: take the request (true once per request).
		bool TakeCalibrationReload()
		{
			const bool wanted = calibrationReload;
			calibrationReload = false;
			return wanted;
		}
		static const char* CalibrationReloadNotice()
		{
			return "Calibration LUT loaded from the calibration run (no Config Apply)";
		}

	private:
		ReloadMode mode = ReloadMode::Auto;
		bool observedAvailable = false;
		uint64_t observedBytes = 0;
		uint64_t observedWriteTime = 0;
		uint64_t nextProbeMs = 0;
		bool calibrationReload = false;
	};

	// ---- the calibration patterns: output facts for GetCalibrationOutput ------------------------

	inline uint32_t ContainerBits(DXGI_FORMAT format)
	{
		return format == DXGI_FORMAT_R10G10B10A2_UNORM ? 10u :
			format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 16u :
			format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_R8G8B8A8_UNORM ? 8u : 0u;
	}

	// The link / dither target: display_bit_depth 8 or 10 limits an 8- or 10-bit container.
	inline uint32_t CodeBits(uint32_t containerBits, const std::string& displayBitDepth)
	{
		if ((displayBitDepth == "8" || displayBitDepth == "10") && (containerBits == 8 || containerBits == 10))
			return (std::min)(containerBits, static_cast<uint32_t>(displayBitDepth == "8" ? 8 : 10));
		return containerBits;
	}

	// The maximum code of the swapchain format a patch is written in (0 = patches unsupported).
	inline double PatchMaxCode(DXGI_FORMAT format)
	{
		return format == DXGI_FORMAT_R10G10B10A2_UNORM ? 1023.0 :
			format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM ? 255.0 : 0.0;
	}

	// GetCalibrationOutput refreshes the render-thread format facts under renderMutex (try-lock with
	// short sleeps). True = try for fresh facts now.
	inline bool WantFreshOutputFacts(bool patternsActive, uint32_t cachedContainerBits)
	{
		(void)patternsActive;
		(void)cachedContainerBits;
		return true;
	}
}
