// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <VP-V/core/FallbackCatalog.h>

#include <dxgiformat.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string>

// The decisions of the calibration host adapter (CalibrationLutHost.h), kept pure so the CPU tests
// cover them (tests/CalibrationLutHostTests.cpp). WP-12 (integration batch): the calibration glue
// moved out of LibplaceboVideoRenderer.cpp; the renderer keeps one call per hook point.
//
// Pure policy decisions, C++14, no libplacebo, no Windows beyond the DXGI format enum.
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

	// CF1-FIN SHOULD A: calibration_lut_stage auto resolves to vp in PostLoad when a step that must follow the LUT is
	// CONFIGURED (ResolveStage / DisplayMappingStepsRun). Per frame it keeps the VP stage only while such a step WANTS to
	// act on the picture: appearance on with a strength above 0, or a Dolby Vision picture with DM for VP's display
	// mapping (the trims or the Dolby-style curve). Otherwise libplacebo's target LUT, so a default profile (dv_trims on)
	// on an HDR10 or SDR picture stays byte-identical. An explicit vp passes through. The LUT is configured before the
	// frame's colour decisions, so stepWants is the previous frame's (a change lands one frame later, shown by the
	// steps' own "calibration LUT" states for that frame, never silent).
	inline bool AutoKeepsVpStage(bool configuredAuto, bool stepWants)
	{
		return !configuredAuto || stepWants;
	}

	// CF-1 P3: calibration_lut_stage = auto. VP's colour steps that shape the picture (dm_trims, and
	// colour.appearance when it exists) must come AFTER the LUT never before it: a LUT applied by
	// libplacebo runs before PRE_OUTPUT, so those steps would act on projector codes. Auto therefore
	// takes the VP stage while one of them runs, and libplacebo's LUT otherwise. The Original pathway
	// always keeps libplacebo's (the caller passes dmStepsRun = false there). "libplacebo" and "vp"
	// pass through; anything else reads as libplacebo.
	std::string ResolveStage(const std::string& configured, bool dmStepsRun);

	// Exact ASCII-case-insensitive token lookup in pipeline_slots_off (tokens separated by ',', ' ', ';', '\t').
	bool ContainsSlotToken(const std::string& slotsOff, const char* targetLower);

	// CF-1 FIX1 MUST 2: does a VP colour step that must follow the LUT run? dm_trims (dv_trims on, dv_display_mapping
	// not original, colour.dm_trims not in pipeline_slots_off) or colour.appearance (appearance on, the slot not off).
	// Never on the Original pathway. The ResolveStage input for auto.
	bool DisplayMappingStepsRun(bool originalPathway, const std::string& dvDisplayMapping, bool dvTrims, bool appearance, const std::string& slotsOff);

	constexpr const char* kVpStagePrefix = "Active (VP stage): ";
	// The renderer's LoadDisplayLut writes this when a changed file fails to reload and the last good
	// LUT stays (Bill's text; his ConfigureDisplayLutForTarget keeps it on the libplacebo stage).
	constexpr const char* kReloadRejectedPrefix = "Active (reload rejected: ";
	// The same state while the VP stage applies the LUT (module findings 2026-10-24 part 3: the VP
	// stage used to overwrite the rejection every frame, so it was never shown).
	constexpr const char* kVpReloadRejectedPrefix = "Active (VP stage; reload rejected: ";

	inline bool StartsWith(const std::string& text, const char* prefix)
	{
		return text.rfind(prefix, 0) == 0;
	}

	// The LUT status while the VP stage applies the LUT. Changes `status` only when it differs (no
	// per-frame allocation once set). A rejected reload stays visible ("Active (VP stage; reload
	// rejected: <reason>)") until the next successful load replaces the status. Returns true when it
	// changed.
	bool VpStageStatus(std::string& status, const std::string& fileName);

	// Back on libplacebo's target LUT (stage switched, failed, Original pathway): a rejection shown
	// for the VP stage goes back to Bill's text, which his libplacebo branch keeps.
	bool LibplaceboStageStatus(std::string& status);

	// GEM-197: BTB/WTW edge-slope extrapolation fallback decision (engaged while slopeFallbacks > 0).
	FallbackCatalog::Decision SlopeFallbackDecision(unsigned long long slopeFallbacks);

	// The status "calibration LUT attached" (Ctrl+I limited-range text, "(inactive: calibration LUT)",
	// ActiveProfileStatus, Config's usable-LUT check).
	// Module findings 2026-10-24 part 3: it used to read only libplacebo's target.lut, so with the VP
	// stage (target.lut empty on purpose) the LUT read as not attached. vpStageApplied = the VP stage
	// hook was in this frame's hooks and has not failed.
	inline bool LutAttached(bool rendered, bool targetLutApplied, bool vpStageApplied)
	{
		return rendered && (targetLutApplied || vpStageApplied);
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
		// The file version the last load read (WP-24's display-LUT identity reads it).
		uint64_t ObservedBytes() const { return observedBytes; }
		uint64_t ObservedWriteTime() const { return observedWriteTime; }
		bool Changed(bool available, uint64_t fileBytes, uint64_t fileWriteTime) const
		{
			return !(available == observedAvailable && fileBytes == observedBytes && fileWriteTime == observedWriteTime);
		}

		// VP's calibration applied a finished LUT (any thread).
		void RequestCalibrationReload() { calibrationReload.store(true, std::memory_order_release); }
		// Render thread: take the request (true once per request).
		bool TakeCalibrationReload() { return calibrationReload.exchange(false, std::memory_order_acq_rel); }
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
		std::atomic<bool> calibrationReload{ false };
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
	// short sleeps). While patterns show, the render loop holds renderMutex across each vsync-paced
	// Present and re-locks at once, so nearly every poll used to wait 100 tries x Sleep(2) (about
	// 0.2-1.6 s per patch; module README "Cost" item 1). The format does not change while patterns
	// show (video is paused) unless the swapchain is rebuilt mid-run (display mode change, the TV
	// toggling HDR), so then one try-lock per poll, without sleeping: a free renderMutex still
	// refreshes the facts, a busy one answers from the cache at once (audit 2026-10-25 item 3).
	// The presented-generation counters the session waits on are atomics and stay live.
	// Returns how many try-lock attempts (2 ms apart) the query makes.
	inline int FreshOutputAttempts(bool patternsActive, uint32_t cachedContainerBits)
	{
		return patternsActive && cachedContainerBits != 0 ? 1 : 100;
	}
}
