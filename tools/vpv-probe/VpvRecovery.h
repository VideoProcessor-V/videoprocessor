#pragma once
/*
 * Copyright(C) 2026 Bill Slack
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * VPV probe: snapshot and restore of the display state around
 * NV_HDR_MODE_DOLBY_VISION. NVIDIA's only documented exit
 * (NV_HDR_MODE_OFF) is publicly reported to leave the sink black in DV mode
 * (developer forum thread 199530, unresolved as of 2026-10), so the exit is a
 * ladder of independent steps, each logged, plus manual instructions.
 *
 * The steps are idempotent and serialised across processes with a named
 * mutex, so the in-process paths (normal exit, Ctrl+C, watchdog, crash
 * filter) and the guardian process cannot run them concurrently.
 */
#include "VpvLog.h"
#include <Windows.h>
#include <nvapi.h>
#include <cstdint>
#include <cstring>
#include <string>

namespace vpv
{
	constexpr uint32_t kStateMagic = 0x31565056; // "VPV1"

	// Written to disk before any state change, so a guardian or a later
	// `--recover-only` run can restore after a kill or crash.
	struct Snapshot
	{
		uint32_t magic = kStateMagic;
		uint32_t size = sizeof(Snapshot);
		NvU32 displayId = 0;
		wchar_t gdiName[32] = {};
		DEVMODEW mode = {};
		bool haveMode = false;
		NV_HDR_COLOR_DATA_V2 hdrBefore = {};
		bool haveHdr = false;
		NV_COLOR_DATA colorBefore = {};
		bool haveColor = false;
	};

	struct RecoveryOptions
	{
		bool minimal = false;        // OFF + verify only (experiments)
		bool askOwner = false;       // interactive escalation prompt
		DWORD settleMs = 1500;       // wait after each HDR switch
	};

	inline std::wstring StatePath()
	{
		wchar_t base[MAX_PATH] = {};
		if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH))
			wcscpy_s(base, L".");
		std::wstring dir = std::wstring(base) + L"\\vpv-probe";
		CreateDirectoryW(dir.c_str(), nullptr);
		return dir + L"\\active-state.bin";
	}

	inline bool SaveSnapshot(const Snapshot& s)
	{
		const std::wstring path = StatePath();
		HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
		if (f == INVALID_HANDLE_VALUE)
			return false;
		DWORD written = 0;
		const bool ok = WriteFile(f, &s, sizeof(s), &written, nullptr) && written == sizeof(s);
		FlushFileBuffers(f);
		CloseHandle(f);
		VPV_LOG("state file %ls %s", path.c_str(), ok ? "written" : "WRITE FAILED");
		return ok;
	}

	inline bool LoadSnapshot(Snapshot& s)
	{
		HANDLE f = CreateFileW(StatePath().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
			OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (f == INVALID_HANDLE_VALUE)
			return false;
		DWORD read = 0;
		const bool ok = ReadFile(f, &s, sizeof(s), &read, nullptr) && read == sizeof(s) &&
			s.magic == kStateMagic && s.size == sizeof(Snapshot);
		CloseHandle(f);
		return ok;
	}

	inline void ClearSnapshot()
	{
		DeleteFileW(StatePath().c_str());
		VPV_LOG("state file cleared");
	}

	inline NV_HDR_COLOR_DATA_V2 HdrRequest(NV_HDR_CMD cmd, NV_HDR_MODE mode)
	{
		NV_HDR_COLOR_DATA_V2 d = {};
		d.version = NV_HDR_COLOR_DATA_VER2;
		d.cmd = cmd;
		d.hdrMode = mode;
		d.static_metadata_descriptor_id = NV_STATIC_METADATA_TYPE_1;
		return d;
	}

	inline void LogHdr(const char* what, const NV_HDR_COLOR_DATA_V2& d)
	{
		VPV_LOG("  %s: hdrMode=%d colorFormat=%d dynamicRange=%d bpc=%d mdl max=%u min=%u maxCLL=%u maxFALL=%u",
			what, static_cast<int>(d.hdrMode), static_cast<int>(d.hdrColorFormat),
			static_cast<int>(d.hdrDynamicRange), static_cast<int>(d.hdrBpc),
			d.mastering_display_data.max_display_mastering_luminance,
			d.mastering_display_data.min_display_mastering_luminance,
			d.mastering_display_data.max_content_light_level,
			d.mastering_display_data.max_frame_average_light_level);
	}

	// NOTE: NvAPI_Disp_HdrColorControl is the call the hard limit forbids on
	// the development machine. Everything below runs only in live mode
	// (--i-am-present) or in a guardian / --recover-only started from it.
	inline bool ReadHdr(NvU32 displayId, NV_HDR_COLOR_DATA_V2& out)
	{
		out = HdrRequest(NV_HDR_CMD_GET, NV_HDR_MODE_OFF);
		const bool ok = NV_LOGGED("NvAPI_Disp_HdrColorControl(GET)",
			NvAPI_Disp_HdrColorControl(displayId, reinterpret_cast<NV_HDR_COLOR_DATA*>(&out))) == NVAPI_OK;
		if (ok)
			LogHdr("current", out);
		return ok;
	}

	inline bool SetHdr(NvU32 displayId, NV_HDR_COLOR_DATA_V2 request, const char* label)
	{
		request.cmd = NV_HDR_CMD_SET;
		LogHdr(label, request);
		return NV_LOGGED(label,
			NvAPI_Disp_HdrColorControl(displayId, reinterpret_cast<NV_HDR_COLOR_DATA*>(&request))) == NVAPI_OK;
	}

	inline bool HdrIsOff(NvU32 displayId)
	{
		NV_HDR_COLOR_DATA_V2 now;
		return ReadHdr(displayId, now) && now.hdrMode == NV_HDR_MODE_OFF;
	}

	inline bool AskOwnerPictureOk(DWORD timeoutMs)
	{
		VPV_LOG("OWNER: is the target display showing a normal SDR picture? type y + Enter (no answer in %lu s = yes)",
			timeoutMs / 1000);
		HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
		FlushConsoleInputBuffer(in);
		const ULONGLONG end = GetTickCount64() + timeoutMs;
		while (GetTickCount64() < end)
		{
			if (WaitForSingleObject(in, 200) != WAIT_OBJECT_0)
				continue;
			INPUT_RECORD rec;
			DWORD n = 0;
			if (!ReadConsoleInputW(in, &rec, 1, &n) || n == 0)
				continue;
			if (rec.EventType == KEY_EVENT && rec.Event.KeyEvent.bKeyDown)
			{
				const wchar_t c = rec.Event.KeyEvent.uChar.UnicodeChar;
				if (c == L'n' || c == L'N')
				{
					VPV_LOG("OWNER: picture NOT ok -> escalating");
					return false;
				}
				if (c == L'y' || c == L'Y')
					return true;
			}
		}
		VPV_LOG("OWNER: no answer, treating as ok");
		return true;
	}

	inline void PrintManualRecovery()
	{
		VPV_LOG("MANUAL RECOVERY (in order, stop when the picture is back):");
		VPV_LOG("  1. run: vpv-probe --recover-only --i-am-present   (repeats the automatic ladder)");
		VPV_LOG("  2. Win+Ctrl+Shift+B (restarts the graphics stack; screens blink)");
		VPV_LOG("  3. unplug the HDMI cable for 10 s, replug (hot-plug, link retrain)");
		VPV_LOG("  4. Device Manager -> Display adapters -> NVIDIA -> Disable, then Enable");
		VPV_LOG("     (admin PowerShell: Get-PnpDevice -Class Display -FriendlyName '*NVIDIA*' | Disable-PnpDevice -Confirm:$false; then Enable-PnpDevice)");
		VPV_LOG("  5. TV: power off at the mains for 30 s");
		VPV_LOG("  6. reboot the PC (the only fix every public report confirms)");
	}

	// The exit ladder. Returns true when the driver reports HDR OFF and,
	// in interactive mode, the owner confirmed the picture.
	inline bool RunRecovery(const Snapshot& s, const char* reason, const RecoveryOptions& opt)
	{
		HANDLE lock = CreateMutexW(nullptr, FALSE, L"Local\\vpv-probe-recovery");
		if (lock)
			WaitForSingleObject(lock, 60000);
		VPV_LOG("=== RECOVERY start (reason: %s, display 0x%08X %ls) ===", reason, s.displayId, s.gdiName);

		// Step 1: NV_HDR_MODE_OFF, repeated (NVIDIA's forum suggestion:
		// "a couple of extra OFF calls"), with the snapshot's colour fields.
		NV_HDR_COLOR_DATA_V2 off = HdrRequest(NV_HDR_CMD_SET, NV_HDR_MODE_OFF);
		if (s.haveHdr)
		{
			off.hdrColorFormat = s.hdrBefore.hdrColorFormat;
			off.hdrDynamicRange = s.hdrBefore.hdrDynamicRange;
			off.hdrBpc = s.hdrBefore.hdrBpc;
		}
		for (int i = 0; i < 3; ++i)
		{
			SetHdr(s.displayId, off, "step1 HdrColorControl(SET OFF)");
			Sleep(opt.settleMs);
		}
		bool driverOff = HdrIsOff(s.displayId);
		VPV_LOG("step1 verify: driver reports HDR %s", driverOff ? "OFF" : "NOT OFF");

		// Step 2: UHDA then OFF (forces a fresh HDR->SDR InfoFrame transition).
		if (!driverOff || !opt.minimal)
		{
			NV_HDR_COLOR_DATA_V2 uhda = HdrRequest(NV_HDR_CMD_SET, NV_HDR_MODE_UHDA);
			SetHdr(s.displayId, uhda, "step2 HdrColorControl(SET UHDA)");
			Sleep(opt.settleMs + 500);
			SetHdr(s.displayId, off, "step2 HdrColorControl(SET OFF)");
			Sleep(opt.settleMs);
			driverOff = HdrIsOff(s.displayId);
			VPV_LOG("step2 verify: driver reports HDR %s", driverOff ? "OFF" : "NOT OFF");
		}

		// Step 3: restore colour format / range / bpc (DV forces RGB full 8).
		if (s.haveColor)
		{
			NV_COLOR_DATA c = s.colorBefore;
			c.version = NV_COLOR_DATA_VER;
			c.size = sizeof(c);
			c.cmd = NV_COLOR_CMD_SET;
			NV_LOGGED("step3 NvAPI_Disp_ColorControl(SET snapshot)", NvAPI_Disp_ColorControl(s.displayId, &c));
		}

		bool ok = driverOff;
		if (!opt.minimal)
		{
			// Step 4: forced modeset of the target only (link retrain, new
			// AVI/VSIF state). The laptop panel is not touched.
			if (s.haveMode)
			{
				DEVMODEW m = s.mode;
				const LONG r = ChangeDisplaySettingsExW(s.gdiName, &m, nullptr, CDS_RESET, nullptr);
				VPV_LOG("step4 ChangeDisplaySettingsExW(%ls, saved mode %lux%lu@%lu, CDS_RESET) -> %ld",
					s.gdiName, m.dmPelsWidth, m.dmPelsHeight, m.dmDisplayFrequency, r);
				Sleep(2500);
			}
			if (opt.askOwner)
				ok = driverOff && AskOwnerPictureOk(30000);
			// Step 5: re-apply the whole CCD topology (every display blinks).
			if (!ok)
			{
				const LONG r = SetDisplayConfig(0, nullptr, 0, nullptr,
					SDC_APPLY | SDC_USE_DATABASE_CURRENT);
				VPV_LOG("step5 SetDisplayConfig(SDC_APPLY|SDC_USE_DATABASE_CURRENT) -> %ld", r);
				Sleep(3000);
				SetHdr(s.displayId, off, "step5 HdrColorControl(SET OFF)");
				Sleep(opt.settleMs);
				driverOff = HdrIsOff(s.displayId);
				ok = driverOff && (!opt.askOwner || AskOwnerPictureOk(30000));
			}
		}
		VPV_LOG("=== RECOVERY %s ===", ok ? "COMPLETE" : "INCOMPLETE");
		if (!ok)
			PrintManualRecovery();
		if (lock)
		{
			ReleaseMutex(lock);
			CloseHandle(lock);
		}
		return ok;
	}
}
