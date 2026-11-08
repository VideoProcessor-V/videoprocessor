// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// VP-V extension ABI (M2 P2, decisions D-P2a..c of doc 61).
//
// The plugin hands IVideoRenderer* across the DLL boundary, so Bill's vtable is the ABI and
// VP_LIBPLACEBO_PLUGIN_API_VERSION (vprenderer/LibplaceboRendererPluginApi.h) is Bill's own number.
// Everything VP-V adds to that boundary goes through ONE trailing virtual, IVideoRenderer::QueryExtension(iid),
// and the pure-abstract interfaces in VpvInterfaces.h. A changed interface gets a new iid, never an edit;
// kVpvExtensionAbi counts changes to the set (a new interface, a new export, a changed iid) and is checked
// at load by PluginGuard (VpvPluginGuard.h): an executable and renderer DLL of different extension ABIs
// never pair, and the refusal is shown to the user (the same path as an API mismatch).
//
// kVpvExtensionAbi history (the numbers are VP-V's own, starting at 1 with the P2 split):
//   1  The 22 virtuals that VP-V had appended to IVideoRenderer moved behind QueryExtension (see the table in
//      README.md): vpv.modules.v1, vpv.status.v1, vpv.led.v1, vpv.intent.v1, vpv.calibration.v1,
//      vpv.timed_file_frames.v1, vpv.presentation_ctl.v1.
//
// What the old VP-V plugin API counter recorded before the split (so git history stays readable). It ran
// 25..45 on top of Bill's number; the whole line is retired by P2:
//   25: GetDejudderStatus (stage 6b-2b) and, separately, Bill's rc4 Beta flavor export; 26: both together (rc4
//   merge); 27: Suspend/ResumeLedDrive for the RS-232 connection test; 28 (Bill, v1.4.02): the analysis diagnostic
//   sink, Dolby luminance, chroma/motion profiles; 29: 27 and 28 together (v1.4.02 merge, 2026-10-15);
//   30: GetLedDriveStatus (crash state); 31: GetSourceResolutionStatus (Stage 12, crash state); 32: the DV L1 scene
//   minimum in DolbyVisionLuminanceStatus; 33: StopLedDriveForShutdown (2026-10-17 integration); 34: GetContentIntentStatus
//   and the DV L5/L6/L11 fields in DolbyVisionTunnelFrameMetadata; 35: the required build-ID export (hunt 2; the
//   build ID is the mechanical pair check, VpBuildIdentity.h); 36: WriteDiagnosticHistory (report-a-problem);
//   37: the VP Renderer DirectShow filter (NV12/P010/P016, FileFrameTiming, SupportsTimedFileFrames,
//   OnTimedVideoFrame, WaitForFileFrameSlot, CancelFileFrameWait); 38: GetIntentStatus / SetAmbientLux;
//   39: GetCalibrationOutput / SetCalibrationPattern (PatternOutputInfo grew within 39 before any 39 build shipped);
//   40: SetActivePictureLookaheadAutomatic (bug hunt 3 F2); 41: GetFallbacksStatus (r6 fix 7); 42: SetQueueDelayBudget
//   (ADR-0020); 43: ToggleDvCompareCapture (feat/dv-l8); 44: GetModuleStatus / InvokeModuleCommand (WP-11);
//   45: v1.4.02-beta merge (Bill's API 34 and VP-V 44 together).
// P2 (this change) returns the number to Bill's value: VP-V's 22 vtable entries are gone from IVideoRenderer, so
// the vtable is Bill's again plus the one VPV-HOOK virtual, which a plain Bill executable or DLL never calls.

#include <cstdint>

namespace Vpv
{
	static constexpr uint32_t kVpvExtensionAbi = 1;

	// The plugin's export returning kVpvExtensionAbi. Required: a DLL without it (a Bill DLL, or a VP-V DLL of
	// the old 45-number line) is refused by PluginGuard.
	static constexpr const char* kVpvAbiExport = "VideoProcessorLibplaceboVpvAbi";
	using VpvAbiFn = uint32_t(__cdecl*)();
}
