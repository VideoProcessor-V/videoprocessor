/*
 * Copyright(C) 2026 Bill Slack
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * vpv-probe: harness for NVIDIA's experimental NV_HDR_MODE_DOLBY_VISION
 * ("VPV": VP sending a TV-led Dolby Vision tunnel through the GPU).
 *
 *   vpv-probe --dry-run [--display \\.\DISPLAYn]        query only (default)
 *   vpv-probe --self-test [--width W --height H]         CPU pack/unpack check
 *   vpv-probe --i-am-present --display \\.\DISPLAYn [--seconds N] [...]
 *   vpv-probe --recover-only --i-am-present [--display \\.\DISPLAYn]
 *
 * Without --i-am-present nothing that changes display or HDR state is
 * called. See README.md for the owner's test procedure and manual recovery.
 */
#include "VpvLog.h"
#include "VpvRecovery.h"
#include "VpvTunnelPacker.h"

#include <DolbyVisionTunnelUnpackReference.h>

#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <nvapi.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")

using Microsoft::WRL::ComPtr;

namespace
{
	enum class Mode { DRY_RUN, SELF_TEST, LIVE, RECOVER_ONLY, GUARDIAN };

	struct Options
	{
		Mode mode = Mode::DRY_RUN;
		bool present = false;              // --i-am-present
		std::wstring display;              // \\.\DISPLAYn
		NvU32 displayId = 0;               // --display-id (hex)
		int seconds = 10;                  // 1..120
		int watchdogGrace = 10;
		vpv::Pattern pattern = vpv::Pattern::RAMP;
		vpv::DmParams dm;
		std::wstring dmFile;               // captured DM body, sent verbatim
		std::wstring dumpFrame;            // write the packed BGRA frame
		NV_COLOR_FORMAT format = NV_COLOR_FORMAT_RGB;
		NV_DYNAMIC_RANGE range = NV_DYNAMIC_RANGE_VESA;
		NV_BPC bpc = NV_BPC_8;
		bool ignoreCaps = false;
		bool exitMinimal = false;
		bool noGuardian = false;
		bool noMode = false;               // present the tunnel frame in plain SDR, no NVAPI HDR call
		uint32_t width = 1920, height = 1080;   // self-test
		DWORD parentPid = 0;               // guardian
		DWORD guardianDeadlineMs = 0;
		std::string logPath = "vpv-probe.log";
	};

	Options g_opt;
	vpv::Snapshot g_snapshot;
	std::atomic<bool> g_modeChanged{ false };
	std::atomic<bool> g_recovered{ false };
	std::atomic<bool> g_stop{ false };
	HANDLE g_cleanEvent = nullptr;

	std::wstring Widen(const char* s)
	{
		const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
		std::wstring w(n > 0 ? n - 1 : 0, L'\0');
		if (n > 1)
			MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
		return w;
	}

	void Usage()
	{
		puts("vpv-probe --dry-run | --self-test | --i-am-present | --recover-only --i-am-present\n"
			"  --display \\\\.\\DISPLAYn   target output (GDI name)\n"
			"  --display-id 0xHEX        target by NVAPI display id\n"
			"  --seconds N               DV presentation time (1..120, default 10)\n"
			"  --watchdog-grace N        extra seconds before the watchdog restores (default 10)\n"
			"  --pattern ramp|bars|flat  intensity pattern (neutral chroma)\n"
			"  --dm-preset ycbcr2020|ipt signal domain of the synthetic DM\n"
			"  --dm-file PATH            send a captured DM body verbatim\n"
			"  --l1 MIN,MAX,AVG          L1 12-bit PQ codes\n"
			"  --wire rgb8|yuv422-12     NV_HDR_COLOR_DATA_V2 colour fields (default rgb8 = RGB full 8bpc)\n"
			"  --dump-frame PATH         write the packed BGRA frame (for loopback comparison)\n"
			"  --no-mode                 present the tunnel frame as plain SDR (no HDR call): loopback pixel test\n"
			"  --exit-minimal            exit with OFF + verify only (no UHDA toggle / modeset)\n"
			"  --ignore-caps             allow a sink without isDolbyVisionSupported\n"
			"  --log PATH                log file (default vpv-probe.log)\n"
			"  --width W --height H      self-test picture size");
	}

	bool Parse(int argc, char** argv)
	{
		for (int i = 1; i < argc; ++i)
		{
			const std::string a = argv[i];
			auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
			if (a == "--dry-run") g_opt.mode = Mode::DRY_RUN;
			else if (a == "--self-test") g_opt.mode = Mode::SELF_TEST;
			else if (a == "--recover-only") g_opt.mode = Mode::RECOVER_ONLY;
			else if (a == "--guardian") g_opt.mode = Mode::GUARDIAN;
			else if (a == "--i-am-present") g_opt.present = true;
			else if (a == "--display") g_opt.display = Widen(next());
			else if (a == "--display-id") g_opt.displayId = strtoul(next(), nullptr, 16);
			else if (a == "--seconds") g_opt.seconds = atoi(next());
			else if (a == "--watchdog-grace") g_opt.watchdogGrace = atoi(next());
			else if (a == "--pattern")
			{
				const std::string p = next();
				g_opt.pattern = p == "bars" ? vpv::Pattern::BARS : p == "flat" ? vpv::Pattern::FLAT : vpv::Pattern::RAMP;
			}
			else if (a == "--dm-preset")
				g_opt.dm.preset = std::string(next()) == "ipt" ? vpv::DmPreset::IPT_PQ_C2 : vpv::DmPreset::YCBCR_BT2020;
			else if (a == "--dm-file") g_opt.dmFile = Widen(next());
			else if (a == "--l1")
			{
				unsigned mn = 0, mx = 0, av = 0;
				if (sscanf_s(next(), "%u,%u,%u", &mn, &mx, &av) == 3)
				{
					g_opt.dm.l1MinPq = static_cast<uint16_t>(mn);
					g_opt.dm.l1MaxPq = static_cast<uint16_t>(mx);
					g_opt.dm.l1AvgPq = static_cast<uint16_t>(av);
				}
			}
			else if (a == "--wire")
			{
				if (std::string(next()) == "yuv422-12")
				{
					g_opt.format = NV_COLOR_FORMAT_YUV422;
					g_opt.range = NV_DYNAMIC_RANGE_CEA;
					g_opt.bpc = NV_BPC_12;
				}
			}
			else if (a == "--dump-frame") g_opt.dumpFrame = Widen(next());
			else if (a == "--exit-minimal") g_opt.exitMinimal = true;
			else if (a == "--ignore-caps") g_opt.ignoreCaps = true;
			else if (a == "--no-guardian") g_opt.noGuardian = true;
			else if (a == "--no-mode") g_opt.noMode = true;
			else if (a == "--log") g_opt.logPath = next();
			else if (a == "--width") g_opt.width = static_cast<uint32_t>(atoi(next()));
			else if (a == "--height") g_opt.height = static_cast<uint32_t>(atoi(next()));
			else if (a == "--parent") g_opt.parentPid = strtoul(next(), nullptr, 10);
			else if (a == "--guardian-deadline-ms") g_opt.guardianDeadlineMs = strtoul(next(), nullptr, 10);
			else if (a == "--help" || a == "-h") { Usage(); return false; }
			else { printf("unknown argument %s\n", a.c_str()); Usage(); return false; }
		}
		if (g_opt.seconds < 1) g_opt.seconds = 1;
		if (g_opt.seconds > 120) g_opt.seconds = 120;
		if (g_opt.watchdogGrace < 3) g_opt.watchdogGrace = 3;
		// Safety: anything that may change display state needs the flag.
		if ((g_opt.mode == Mode::RECOVER_ONLY || g_opt.mode == Mode::GUARDIAN) && !g_opt.present)
		{
			puts("refused: --recover-only changes display state and needs --i-am-present");
			return false;
		}
		if (g_opt.present && g_opt.mode == Mode::DRY_RUN)
			g_opt.mode = Mode::LIVE;
		return true;
	}

	// ---------------------------------------------------------------- queries

	const char* ConnName(NV_MONITOR_CONN_TYPE t)
	{
		switch (t)
		{
		case NV_MONITOR_CONN_TYPE_HDMI: return "HDMI";
		case NV_MONITOR_CONN_TYPE_DP: return "DP";
		case NV_MONITOR_CONN_TYPE_DVI: return "DVI";
		case NV_MONITOR_CONN_TYPE_VGA: return "VGA";
		default: return "other";
		}
	}

	void LogCaps(NvU32 id, NV_HDR_CAPABILITIES_V3& caps, bool& dv)
	{
		caps = {};
		caps.version = NV_HDR_CAPABILITIES_VER3;
		caps.driverExpandDefaultHdrParameters = 0;
		NvAPI_Status st = NV_LOGGED("NvAPI_Disp_GetHdrCapabilities(V3)", NvAPI_Disp_GetHdrCapabilities(id, &caps));
		if (st == NVAPI_INCOMPATIBLE_STRUCT_VERSION)
		{
			NV_HDR_CAPABILITIES_V2 v2 = {};
			v2.version = NV_HDR_CAPABILITIES_VER2;
			st = NV_LOGGED("NvAPI_Disp_GetHdrCapabilities(V2)",
				NvAPI_Disp_GetHdrCapabilities(id, reinterpret_cast<NV_HDR_CAPABILITIES*>(&v2)));
			if (st == NVAPI_OK)
			{
				memcpy(&caps, &v2, sizeof(v2));
				caps.isHdr10PlusSupported = caps.isHdr10PlusGamingSupported = caps.isNvidiaCertifiedDisplay = 0;
			}
		}
		dv = false;
		if (st != NVAPI_OK)
			return;
		dv = caps.isDolbyVisionSupported != 0;
		VPV_LOG("  caps: ST2084=%u tradHDR=%u tradSDR=%u EDR=%u DolbyVision=%u HDR10+=%u HDR10+Gaming=%u nvCertified=%u",
			caps.isST2084EotfSupported, caps.isTraditionalHdrGammaSupported, caps.isTraditionalSdrGammaSupported,
			caps.isEdrSupported, caps.isDolbyVisionSupported, caps.isHdr10PlusSupported,
			caps.isHdr10PlusGamingSupported, caps.isNvidiaCertifiedDisplay);
		const auto& d = caps.display_data;
		VPV_LOG("  HDR static: R(%u,%u) G(%u,%u) B(%u,%u) W(%u,%u) maxLum=%u minLum=%u maxFALL=%u",
			d.displayPrimary_x0, d.displayPrimary_y0, d.displayPrimary_x1, d.displayPrimary_y1,
			d.displayPrimary_x2, d.displayPrimary_y2, d.displayWhitePoint_x, d.displayWhitePoint_y,
			d.desired_content_max_luminance, d.desired_content_min_luminance,
			d.desired_content_max_frame_average_luminance);
		if (dv)
		{
			const auto& v = caps.dv_static_metadata;
			VPV_LOG("  DV VSVDB: version=%u dm_version=0x%02X 2160p60=%u yuv422_12=%u global_dimming=%u colorimetry(P3)=%u",
				v.VSVDB_version, v.dm_version, v.supports_2160p60hz, v.supports_YUV422_12bit,
				v.supports_global_dimming, v.colorimetry);
			VPV_LOG("  DV VSVDB: interface=%u (0=std/TV-led ...) backlight_ctl=%u backlt_min=%u 10b12b444=%u parity=%u",
				v.interface_supported_by_sink, v.supports_backlight_control, v.backlt_min_luma,
				v.supports_10b_12b_444, v.parity);
			VPV_LOG("  DV VSVDB: target min=%u max=%u R(%u,%u) G(%u,%u) B(%u,%u) W(%u,%u) (DV HDMI spec encoding)",
				v.target_min_luminance, v.target_max_luminance, v.cc_red_x, v.cc_red_y, v.cc_green_x,
				v.cc_green_y, v.cc_blue_x, v.cc_blue_y, v.cc_white_x, v.cc_white_y);
		}
	}

	struct Target
	{
		NvU32 displayId = 0;
		std::wstring gdiName;
		bool dvSupported = false;
		bool windowsHdrOn = false;
		RECT desktop = {};
		ComPtr<IDXGIAdapter1> adapter;
		ComPtr<IDXGIOutput> output;
	};

	// Pure queries: NVAPI Get* functions, DXGI descriptors, current GDI mode.
	bool QueryAll(Target* target)
	{
		NvAPI_ShortString text{};
		NV_LOGGED("NvAPI_GetInterfaceVersionString", NvAPI_GetInterfaceVersionString(text));
		VPV_LOG("  interface: %s", text);
		NvU32 driver = 0;
		NvAPI_ShortString branch{};
		if (NV_LOGGED("NvAPI_SYS_GetDriverAndBranchVersion", NvAPI_SYS_GetDriverAndBranchVersion(&driver, branch)) == NVAPI_OK)
			VPV_LOG("  driver %u.%02u branch %s", driver / 100, driver % 100, branch);

		NvPhysicalGpuHandle gpus[NVAPI_MAX_PHYSICAL_GPUS] = {};
		NvU32 gpuCount = 0;
		NV_LOGGED("NvAPI_EnumPhysicalGPUs", NvAPI_EnumPhysicalGPUs(gpus, &gpuCount));
		for (NvU32 g = 0; g < gpuCount; ++g)
		{
			NvAPI_ShortString name{};
			NV_LOGGED("NvAPI_GPU_GetFullName", NvAPI_GPU_GetFullName(gpus[g], name));
			VPV_LOG("GPU %u: %s", g, name);
			NvU32 count = 0;
			if (NV_LOGGED("NvAPI_GPU_GetConnectedDisplayIds(count)",
				NvAPI_GPU_GetConnectedDisplayIds(gpus[g], nullptr, &count, 0)) != NVAPI_OK || count == 0)
				continue;
			std::vector<NV_GPU_DISPLAYIDS> ids(count);
			for (auto& d : ids)
				d.version = NV_GPU_DISPLAYIDS_VER;
			if (NV_LOGGED("NvAPI_GPU_GetConnectedDisplayIds",
				NvAPI_GPU_GetConnectedDisplayIds(gpus[g], ids.data(), &count, 0)) != NVAPI_OK)
				continue;
			for (NvU32 i = 0; i < count; ++i)
			{
				VPV_LOG("display 0x%08X conn=%s active=%u connected=%u physical=%u osVisible=%u",
					ids[i].displayId, ConnName(ids[i].connectorType), ids[i].isActive, ids[i].isConnected,
					ids[i].isPhysicallyConnected, ids[i].isOSVisible);
				NV_HDR_CAPABILITIES_V3 caps;
				bool dv = false;
				LogCaps(ids[i].displayId, caps, dv);
				NV_DISPLAY_OUTPUT_MODE om = NV_DISPLAY_OUTPUT_MODE_SDR;
				if (NV_LOGGED("NvAPI_Disp_GetOutputMode", NvAPI_Disp_GetOutputMode(ids[i].displayId, &om)) == NVAPI_OK)
					VPV_LOG("  OS output mode: %d (0=SDR 1=HDR10 2=HDR10+gaming)", static_cast<int>(om));
				if (target && target->displayId == ids[i].displayId)
					target->dvSupported = dv;
			}
		}

		ComPtr<IDXGIFactory1> factory;
		if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
			return false;
		ComPtr<IDXGIAdapter1> adapter;
		for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a)
		{
			DXGI_ADAPTER_DESC1 ad;
			adapter->GetDesc1(&ad);
			VPV_LOG("DXGI adapter %u: %ls vendor=0x%04X", a, ad.Description, ad.VendorId);
			ComPtr<IDXGIOutput> output;
			for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o)
			{
				DXGI_OUTPUT_DESC1 od = {};
				ComPtr<IDXGIOutput6> o6;
				if (SUCCEEDED(output.As(&o6)))
					o6->GetDesc1(&od);
				else
				{
					DXGI_OUTPUT_DESC basic;
					output->GetDesc(&basic);
					memcpy(od.DeviceName, basic.DeviceName, sizeof(od.DeviceName));
					od.DesktopCoordinates = basic.DesktopCoordinates;
				}
				const bool hdr = od.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
				DEVMODEW dm = {};
				dm.dmSize = sizeof(dm);
				EnumDisplaySettingsW(od.DeviceName, ENUM_CURRENT_SETTINGS, &dm);
				char name[64];
				WideCharToMultiByte(CP_UTF8, 0, od.DeviceName, -1, name, sizeof(name), nullptr, nullptr);
				NvU32 id = 0;
				const NvAPI_Status st = NV_LOGGED("NvAPI_DISP_GetDisplayIdByDisplayName",
					NvAPI_DISP_GetDisplayIdByDisplayName(name, &id));
				VPV_LOG("  output %ls nvDisplayId=%s0x%08X desktop=(%ld,%ld)-(%ld,%ld) mode=%lux%lu@%luHz %lubpp bpc=%u windowsHDR=%s maxLum=%.0f",
					od.DeviceName, st == NVAPI_OK ? "" : "(none) ", id,
					od.DesktopCoordinates.left, od.DesktopCoordinates.top, od.DesktopCoordinates.right,
					od.DesktopCoordinates.bottom, dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency,
					dm.dmBitsPerPel, od.BitsPerColor, hdr ? "ON" : "off", od.MaxLuminance);
				if (target && st == NVAPI_OK &&
					((g_opt.displayId && id == g_opt.displayId) ||
						(!g_opt.display.empty() && _wcsicmp(g_opt.display.c_str(), od.DeviceName) == 0)))
				{
					target->displayId = id;
					target->gdiName = od.DeviceName;
					target->windowsHdrOn = hdr;
					target->desktop = od.DesktopCoordinates;
					target->adapter = adapter;
					target->output = output;
				}
			}
		}
		return true;
	}

	// -------------------------------------------------------------- self-test

	std::vector<uint8_t> LoadDm()
	{
		if (g_opt.dmFile.empty())
			return vpv::BuildDmBody(g_opt.dm);
		std::vector<uint8_t> body;
		FILE* f = nullptr;
		if (_wfopen_s(&f, g_opt.dmFile.c_str(), L"rb") == 0 && f)
		{
			uint8_t buf[4096];
			size_t n;
			while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
				body.insert(body.end(), buf, buf + n);
			fclose(f);
		}
		VPV_LOG("DM body from %ls: %zu bytes", g_opt.dmFile.c_str(), body.size());
		return body;
	}

	// Packs a picture, then decodes the BGRA bytes with VP's own unpack
	// reference (DolbyVisionTunnelUnpackReference, the CPU transcription of
	// the GPU tunnel unpack) and extracts/CRC-checks every packet copy.
	int SelfTest()
	{
		const uint32_t w = g_opt.width, h = g_opt.height;
		const auto body = LoadDm();
		const auto packets = vpv::Packetize(body);
		vpv::TunnelPicture pic(w, h, g_opt.pattern);
		if (!pic.Embed(packets))
		{
			VPV_LOG("self-test: picture too small for %zu packets", packets.size());
			return 1;
		}
		std::vector<uint8_t> bgra(static_cast<size_t>(w) * h * 4);
		pic.WriteBgra(bgra.data(), static_cast<size_t>(w) * 4);
		size_t planeErrors = 0;
		std::vector<uint16_t> i12(pic.intensity.size()), c12(pic.chroma.size());
		for (size_t px = 0; px < i12.size(); ++px)
		{
			const uint8_t b = bgra[px * 4], g = bgra[px * 4 + 1], r = bgra[px * 4 + 2];
			const uint32_t word = (static_cast<uint32_t>(r) << 20) | (static_cast<uint32_t>(g) << 10) | b;
			const uint8_t be[4] = { static_cast<uint8_t>(word >> 24), static_cast<uint8_t>(word >> 16),
				static_cast<uint8_t>(word >> 8), static_cast<uint8_t>(word) };
			const uint32_t p = DolbyVisionTunnelUnpackReference::Word(be, true);
			const uint32_t s = DolbyVisionTunnelUnpackReference::ContainerShift(VideoFrameEncoding::R210, false);
			i12[px] = static_cast<uint16_t>(DolbyVisionTunnelUnpackReference::Intensity(p, s) >> 4);
			c12[px] = static_cast<uint16_t>(DolbyVisionTunnelUnpackReference::Chroma(p, s) >> 4);
			if (i12[px] != pic.intensity[px] || c12[px] != pic.chroma[px])
				++planeErrors;
		}
		size_t badCopies = 0, mismatches = 0;
		for (size_t p = 0; p < packets.size(); ++p)
			for (size_t copy = 0; copy < 3; ++copy)
			{
				vpv::Packet got{};
				for (size_t bit = 0; bit < 1024; ++bit)
				{
					const size_t px = p * 3072 + copy * 1024 + bit;
					const unsigned md = (c12[px] & 1U) ^ vpv::TunnelPicture::ScramblingParity(i12[px], c12[px]);
					got[bit / 8] = static_cast<uint8_t>(got[bit / 8] | (md << (7 - bit % 8)));
				}
				const uint32_t crc = vpv::Crc32Mpeg2(got.data(), 124);
				const uint32_t sent = (static_cast<uint32_t>(got[124]) << 24) | (got[125] << 16) | (got[126] << 8) | got[127];
				if (crc != sent)
					++badCopies;
				if (got != packets[p])
					++mismatches;
			}
		const bool vpAccepts = body.size() >= 71 && body[60] == 12 && (body[61] == 0 || body[61] == 2);
		VPV_LOG("self-test %ux%u: DM %zu bytes in %zu packet(s); plane mismatches=%zu CRC-bad copies=%zu packet mismatches=%zu; VP ParseMetadata preconditions %s",
			w, h, body.size(), packets.size(), planeErrors, badCopies, mismatches, vpAccepts ? "met" : "NOT met");
		if (!g_opt.dumpFrame.empty())
		{
			FILE* f = nullptr;
			if (_wfopen_s(&f, g_opt.dumpFrame.c_str(), L"wb") == 0 && f)
			{
				fwrite(bgra.data(), 1, bgra.size(), f);
				fclose(f);
				VPV_LOG("packed BGRA frame written to %ls", g_opt.dumpFrame.c_str());
			}
		}
		const bool pass = planeErrors == 0 && badCopies == 0 && mismatches == 0 && vpAccepts;
		VPV_LOG("self-test %s", pass ? "PASS" : "FAIL");
		return pass ? 0 : 1;
	}

	// ---------------------------------------------------------------- safety

	vpv::RecoveryOptions RecoveryOpts(bool interactive)
	{
		vpv::RecoveryOptions r;
		r.minimal = g_opt.exitMinimal;
		r.askOwner = interactive;
		return r;
	}

	void RecoverOnce(const char* reason, bool interactive)
	{
		static std::atomic<bool> running{ false };
		if (!g_modeChanged.load() || running.exchange(true))
			return;
		const bool ok = vpv::RunRecovery(g_snapshot, reason, RecoveryOpts(interactive));
		g_recovered = true;
		if (ok)
		{
			if (g_cleanEvent)
				SetEvent(g_cleanEvent);
			vpv::ClearSnapshot();
		}
	}

	BOOL WINAPI ConsoleHandler(DWORD type)
	{
		VPV_LOG("console control event %lu -> recovery", type);
		g_stop = true;
		if (!g_modeChanged.load() && g_cleanEvent)
			SetEvent(g_cleanEvent); // nothing changed yet
		RecoverOnce("console control (Ctrl+C / close / logoff)", false);
		ExitProcess(2);
	}

	LONG WINAPI CrashFilter(EXCEPTION_POINTERS* e)
	{
		VPV_LOG("UNHANDLED EXCEPTION 0x%08lX at %p -> recovery (guardian repeats it if this fails)",
			e->ExceptionRecord->ExceptionCode, e->ExceptionRecord->ExceptionAddress);
		RecoverOnce("crash filter", false);
		return EXCEPTION_EXECUTE_HANDLER;
	}

	void OnTerminate()
	{
		VPV_LOG("std::terminate -> recovery");
		RecoverOnce("std::terminate", false);
		ExitProcess(4);
	}

	void OnAbort(int)
	{
		VPV_LOG("SIGABRT -> recovery");
		RecoverOnce("SIGABRT", false);
		ExitProcess(4);
	}

	void StartWatchdog(DWORD deadlineMs)
	{
		std::thread([deadlineMs]() {
			const ULONGLONG end = GetTickCount64() + deadlineMs;
			while (GetTickCount64() < end)
			{
				if (g_recovered.load())
					return;
				Sleep(100);
			}
			VPV_LOG("WATCHDOG: %lu ms elapsed without a completed exit -> forced recovery", deadlineMs);
			g_stop = true;
			RecoverOnce("watchdog", false);
			ExitProcess(3);
		}).detach();
	}

	// A second process that restores the display if this one dies without
	// signalling a clean exit (TerminateProcess, debugger kill, hard crash),
	// or hangs past the deadline.
	bool StartGuardian(DWORD deadlineMs)
	{
		wchar_t self[MAX_PATH];
		GetModuleFileNameW(nullptr, self, MAX_PATH);
		wchar_t eventName[64];
		swprintf_s(eventName, L"Local\\vpv-probe-clean-%lu", GetCurrentProcessId());
		g_cleanEvent = CreateEventW(nullptr, TRUE, FALSE, eventName);
		std::wstring cmd = L"\"" + std::wstring(self) + L"\" --guardian --i-am-present --parent " +
			std::to_wstring(GetCurrentProcessId()) + L" --guardian-deadline-ms " + std::to_wstring(deadlineMs) +
			L" --log \"" + Widen(g_opt.logPath.c_str()) + L"\"";
		if (g_opt.exitMinimal)
			cmd += L" --exit-minimal";
		STARTUPINFOW si = { sizeof(si) };
		PROCESS_INFORMATION pi = {};
		const BOOL ok = CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE,
			CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &si, &pi);
		VPV_LOG("guardian process %s (pid %lu)", ok ? "started" : "FAILED TO START", ok ? pi.dwProcessId : 0);
		if (ok)
		{
			CloseHandle(pi.hThread);
			CloseHandle(pi.hProcess);
		}
		return ok != FALSE;
	}

	int Guardian()
	{
		VPV_LOG("guardian: watching pid %lu, deadline %lu ms", g_opt.parentPid, g_opt.guardianDeadlineMs);
		HANDLE parent = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, g_opt.parentPid);
		wchar_t eventName[64];
		swprintf_s(eventName, L"Local\\vpv-probe-clean-%lu", g_opt.parentPid);
		HANDLE clean = OpenEventW(SYNCHRONIZE, FALSE, eventName);
		const DWORD wait = parent ? WaitForSingleObject(parent, g_opt.guardianDeadlineMs) : WAIT_FAILED;
		if (clean && WaitForSingleObject(clean, 0) == WAIT_OBJECT_0)
		{
			VPV_LOG("guardian: parent exited cleanly, nothing to do");
			return 0;
		}
		if (!vpv::LoadSnapshot(g_snapshot))
		{
			VPV_LOG("guardian: no state file -> parent never changed the mode, nothing to do");
			return 0;
		}
		if (NV_LOGGED("NvAPI_Initialize", NvAPI_Initialize()) != NVAPI_OK)
			return 5;
		VPV_LOG("guardian: parent %s without a clean exit -> recovery",
			wait == WAIT_TIMEOUT ? "HUNG past the deadline" : "died");
		const bool ok = vpv::RunRecovery(g_snapshot, "guardian", RecoveryOpts(false));
		if (ok)
			vpv::ClearSnapshot();
		if (wait == WAIT_TIMEOUT && parent)
		{
			VPV_LOG("guardian: terminating the hung parent");
			TerminateProcess(parent, 6);
		}
		NvAPI_Unload();
		return ok ? 0 : 7;
	}

	// ------------------------------------------------------------- live path

	LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
	{
		if ((msg == WM_KEYDOWN && wp == VK_ESCAPE) || msg == WM_CLOSE)
		{
			g_stop = true;
			return 0;
		}
		return DefWindowProcW(hwnd, msg, wp, lp);
	}

	void Pump()
	{
		MSG m;
		while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE))
		{
			TranslateMessage(&m);
			DispatchMessageW(&m);
		}
	}

	int Live(Target& t)
	{
		if (!t.displayId)
		{
			VPV_LOG("refused: target display not found or not driven by NVIDIA (use --display \\\\.\\DISPLAYn from the dry-run list)");
			return 10;
		}
		if (!t.dvSupported && !g_opt.ignoreCaps && !g_opt.noMode)
		{
			VPV_LOG("refused: 0x%08X does not report isDolbyVisionSupported", t.displayId);
			return 11;
		}
		if (t.windowsHdrOn)
		{
			VPV_LOG("refused: Windows HDR is ON for %ls; turn it off first (the DV mode's interaction with it is unknown)", t.gdiName.c_str());
			return 12;
		}
		const UINT w = static_cast<UINT>(t.desktop.right - t.desktop.left);
		const UINT h = static_cast<UINT>(t.desktop.bottom - t.desktop.top);

		// Snapshot before anything changes.
		g_snapshot = {};
		g_snapshot.displayId = t.displayId;
		wcsncpy_s(g_snapshot.gdiName, t.gdiName.c_str(), _TRUNCATE);
		g_snapshot.mode.dmSize = sizeof(DEVMODEW);
		g_snapshot.haveMode = EnumDisplaySettingsW(g_snapshot.gdiName, ENUM_CURRENT_SETTINGS, &g_snapshot.mode) != FALSE;
		g_snapshot.haveHdr = !g_opt.noMode && vpv::ReadHdr(t.displayId, g_snapshot.hdrBefore);
		if (g_snapshot.haveHdr && g_snapshot.hdrBefore.hdrMode != NV_HDR_MODE_OFF)
		{
			VPV_LOG("refused: NVAPI HDR mode is already %d (not OFF)", static_cast<int>(g_snapshot.hdrBefore.hdrMode));
			return 13;
		}
		g_snapshot.colorBefore.version = NV_COLOR_DATA_VER;
		g_snapshot.colorBefore.size = sizeof(NV_COLOR_DATA);
		g_snapshot.colorBefore.cmd = NV_COLOR_CMD_GET;
		g_snapshot.haveColor = !g_opt.noMode && NV_LOGGED("NvAPI_Disp_ColorControl(GET)",
			NvAPI_Disp_ColorControl(t.displayId, &g_snapshot.colorBefore)) == NVAPI_OK;
		if (g_snapshot.haveColor)
			VPV_LOG("  colour before: format=%u colorimetry=%u range=%u bpc=%d depth=%d",
				g_snapshot.colorBefore.data.colorFormat, g_snapshot.colorBefore.data.colorimetry,
				g_snapshot.colorBefore.data.dynamicRange, static_cast<int>(g_snapshot.colorBefore.data.bpc),
				static_cast<int>(g_snapshot.colorBefore.data.depth));

		// Safety nets, all armed before the first state change.
		const DWORD deadline = static_cast<DWORD>(g_opt.seconds + g_opt.watchdogGrace) * 1000 + 5000;
		SetConsoleCtrlHandler(ConsoleHandler, TRUE);
		SetUnhandledExceptionFilter(CrashFilter);
		std::set_terminate(OnTerminate);
		signal(SIGABRT, OnAbort);
		if (!g_opt.noGuardian && !g_opt.noMode && !StartGuardian(deadline + 30000))
		{
			VPV_LOG("refused: guardian did not start (use --no-guardian only if you accept the risk)");
			return 15;
		}
		StartWatchdog(deadline);

		// The frame.
		const auto body = LoadDm();
		const auto packets = vpv::Packetize(body);
		vpv::TunnelPicture pic(w, h, g_opt.pattern);
		if (body.size() < 71 || !pic.Embed(packets))
		{
			VPV_LOG("refused: DM body invalid or picture too small");
			return 16;
		}
		std::vector<uint8_t> bgra(static_cast<size_t>(w) * h * 4);
		pic.WriteBgra(bgra.data(), static_cast<size_t>(w) * 4);
		VPV_LOG("tunnel frame %ux%u, DM %zu bytes, %zu packet(s)", w, h, body.size(), packets.size());

		// Borderless window exactly covering the output, flip-model BGRA8
		// swapchain, no scaling, sRGB colour space (the bytes must reach the
		// wire untouched: no DWM colour transform, scaling or dithering).
		WNDCLASSW wc = {};
		wc.lpfnWndProc = WndProc;
		wc.hInstance = GetModuleHandleW(nullptr);
		wc.lpszClassName = L"vpv-probe";
		wc.hCursor = nullptr;
		RegisterClassW(&wc);
		HWND hwnd = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, L"vpv-probe", WS_POPUP | WS_VISIBLE,
			t.desktop.left, t.desktop.top, static_cast<int>(w), static_cast<int>(h), nullptr, nullptr, wc.hInstance, nullptr);
		ComPtr<ID3D11Device> device;
		ComPtr<ID3D11DeviceContext> ctx;
		HRESULT hr = D3D11CreateDevice(t.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
			D3D11_SDK_VERSION, &device, nullptr, &ctx);
		VPV_LOG("D3D11CreateDevice -> 0x%08lX", hr);
		ComPtr<IDXGIFactory2> factory;
		t.adapter->GetParent(IID_PPV_ARGS(&factory));
		DXGI_SWAP_CHAIN_DESC1 sd = {};
		sd.Width = w;
		sd.Height = h;
		sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		sd.SampleDesc.Count = 1;
		sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		sd.BufferCount = 2;
		sd.Scaling = DXGI_SCALING_NONE;
		sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
		ComPtr<IDXGISwapChain1> sc1;
		if (SUCCEEDED(hr))
			hr = factory->CreateSwapChainForHwnd(device.Get(), hwnd, &sd, nullptr, nullptr, &sc1);
		VPV_LOG("CreateSwapChainForHwnd(B8G8R8A8, flip discard, %ux%u) -> 0x%08lX", w, h, hr);
		ComPtr<IDXGISwapChain3> sc;
		if (SUCCEEDED(hr) && SUCCEEDED(sc1.As(&sc)))
			VPV_LOG("SetColorSpace1(RGB_FULL_G22_NONE_P709) -> 0x%08lX",
				sc->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709));
		ComPtr<ID3D11Texture2D> frame, black;
		if (SUCCEEDED(hr))
		{
			D3D11_TEXTURE2D_DESC td = {};
			td.Width = w;
			td.Height = h;
			td.MipLevels = td.ArraySize = 1;
			td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
			td.SampleDesc.Count = 1;
			td.Usage = D3D11_USAGE_IMMUTABLE;
			td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			D3D11_SUBRESOURCE_DATA init = { bgra.data(), w * 4, 0 };
			hr = device->CreateTexture2D(&td, &init, &frame);
			std::vector<uint8_t> zeros(bgra.size(), 0);
			for (size_t i = 3; i < zeros.size(); i += 4)
				zeros[i] = 0xFF;
			init.pSysMem = zeros.data();
			if (SUCCEEDED(hr))
				hr = device->CreateTexture2D(&td, &init, &black);
		}
		if (FAILED(hr))
		{
			VPV_LOG("refused: D3D setup failed before any mode change");
			g_recovered = true;
			vpv::ClearSnapshot();
			if (g_cleanEvent) SetEvent(g_cleanEvent);
			return 17;
		}
		ComPtr<ID3D11Texture2D> back;
		auto presentFrom = [&](ID3D11Texture2D* src) {
			sc->GetBuffer(0, IID_PPV_ARGS(&back));
			ctx->CopyResource(back.Get(), src);
			back.Reset();
			return sc->Present(1, 0);
		};

		// Present the tunnel bytes for ~0.5 s first (lets DWM promote the
		// window to independent flip), then switch the driver to DV.
		for (int i = 0; i < 30 && !g_stop; ++i) { Pump(); presentFrom(frame.Get()); }

		NV_HDR_COLOR_DATA_V2 dv = vpv::HdrRequest(NV_HDR_CMD_SET, NV_HDR_MODE_DOLBY_VISION);
		dv.hdrColorFormat = g_opt.format;
		dv.hdrDynamicRange = g_opt.range;
		dv.hdrBpc = g_opt.bpc;
		auto& md = dv.mastering_display_data; // DCI-P3 D65 mastering, 0.00002 units
		md.displayPrimary_x0 = 34000; md.displayPrimary_y0 = 16000;
		md.displayPrimary_x1 = 13250; md.displayPrimary_y1 = 34500;
		md.displayPrimary_x2 = 7500;  md.displayPrimary_y2 = 3000;
		md.displayWhitePoint_x = 15635; md.displayWhitePoint_y = 16450;
		md.max_display_mastering_luminance = 4000;
		md.min_display_mastering_luminance = 50;
		md.max_content_light_level = 1000;
		md.max_frame_average_light_level = 400;
		if (g_opt.noMode)
		{
			VPV_LOG("--no-mode: presenting the tunnel frame as ordinary SDR for %d s (no NVAPI state change; Esc stops)", g_opt.seconds);
			const ULONGLONG end = GetTickCount64() + static_cast<ULONGLONG>(g_opt.seconds) * 1000;
			uint64_t presents = 0;
			while (!g_stop && GetTickCount64() < end) { Pump(); presentFrom(frame.Get()); ++presents; }
			VPV_LOG("--no-mode: %llu presents", presents);
			sc.Reset(); sc1.Reset(); ctx->ClearState(); ctx->Flush();
			DestroyWindow(hwnd);
			g_recovered = true;
			return 0;
		}
		// The state file exists only while the mode may be changed: the
		// guardian and --recover-only act on it, nothing else does.
		if (!vpv::SaveSnapshot(g_snapshot))
		{
			VPV_LOG("refused: cannot write the state file; no mode change made");
			g_recovered = true;
			if (g_cleanEvent) SetEvent(g_cleanEvent);
			return 14;
		}
		g_modeChanged = true; // from here every exit path restores
		const bool set = vpv::SetHdr(t.displayId, dv, "NvAPI_Disp_HdrColorControl(SET DOLBY_VISION)");
		NV_HDR_COLOR_DATA_V2 applied;
		vpv::ReadHdr(t.displayId, applied);
		NV_INFOFRAME_DATA avi = {};
		avi.version = NV_INFOFRAME_DATA_VER;
		avi.size = sizeof(avi);
		avi.cmd = NV_INFOFRAME_CMD_GET;
		avi.type = 2; // AVI
		if (NV_LOGGED("NvAPI_Disp_InfoFrameControl(GET AVI)", NvAPI_Disp_InfoFrameControl(t.displayId, &avi)) == NVAPI_OK)
			VPV_LOG("  AVI: colorimetry=%u extendedColorimetry=%u rgbQuantization=%u",
				avi.infoframe.video.colorimetry, avi.infoframe.video.extendedColorimetry,
				avi.infoframe.video.rgbQuantizationRange);

		if (set)
		{
			const ULONGLONG end = GetTickCount64() + static_cast<ULONGLONG>(g_opt.seconds) * 1000;
			uint64_t presents = 0, failures = 0;
			ULONGLONG lastReport = GetTickCount64();
			while (!g_stop && GetTickCount64() < end)
			{
				Pump();
				if (FAILED(presentFrom(frame.Get())))
					++failures;
				++presents;
				if (GetTickCount64() - lastReport >= 1000)
				{
					DXGI_FRAME_STATISTICS fs = {};
					const HRESULT fsr = sc->GetFrameStatistics(&fs);
					VPV_LOG("presenting: %llu presents, %llu failed; frame stats hr=0x%08lX presentCount=%u syncRefresh=%u",
						presents, failures, fsr, fs.PresentCount, fs.SyncRefreshCount);
					lastReport = GetTickCount64();
				}
			}
		}
		else
			VPV_LOG("DV SET failed; going straight to recovery");

		// Exit: driver OFF first while our window still owns the output,
		// then SDR black, then release the swapchain, then the ladder's
		// remaining steps (RunRecovery repeats OFF; it is idempotent).
		NV_HDR_COLOR_DATA_V2 off = vpv::HdrRequest(NV_HDR_CMD_SET, NV_HDR_MODE_OFF);
		vpv::SetHdr(t.displayId, off, "exit HdrColorControl(SET OFF)");
		for (int i = 0; i < 30; ++i) { Pump(); presentFrom(black.Get()); }
		sc.Reset(); sc1.Reset(); ctx->ClearState(); ctx->Flush();
		DestroyWindow(hwnd);
		Pump();
		RecoverOnce("normal exit", true);
		return g_recovered ? 0 : 20;
	}
}

int main(int argc, char** argv)
{
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	if (!Parse(argc, argv))
		return 1;
	vpv::Log::Get().Open(g_opt.logPath.c_str());

	if (g_opt.mode == Mode::SELF_TEST)
		return SelfTest();
	if (g_opt.mode == Mode::GUARDIAN)
		return Guardian();

	VPV_LOG("vpv-probe mode=%s", g_opt.mode == Mode::DRY_RUN ? "dry-run (queries only)" :
		g_opt.mode == Mode::LIVE ? "LIVE (changes display state)" : "recover-only");
	if (NV_LOGGED("NvAPI_Initialize", NvAPI_Initialize()) != NVAPI_OK)
		return 2;
	int rc = 0;
	Target target;
	const bool wantTarget = !g_opt.display.empty() || g_opt.displayId;
	QueryAll(wantTarget ? &target : nullptr);

	if (g_opt.mode == Mode::LIVE)
		rc = Live(target);
	else if (g_opt.mode == Mode::RECOVER_ONLY)
	{
		if (!vpv::LoadSnapshot(g_snapshot))
		{
			if (!target.displayId)
			{
				VPV_LOG("no state file and no --display: nothing to recover");
				NvAPI_Unload();
				return 8;
			}
			g_snapshot = {};
			g_snapshot.displayId = target.displayId;
			wcsncpy_s(g_snapshot.gdiName, target.gdiName.c_str(), _TRUNCATE);
			g_snapshot.mode.dmSize = sizeof(DEVMODEW);
			g_snapshot.haveMode = EnumDisplaySettingsW(g_snapshot.gdiName, ENUM_CURRENT_SETTINGS, &g_snapshot.mode) != FALSE;
		}
		g_modeChanged = true;
		rc = vpv::RunRecovery(g_snapshot, "--recover-only", RecoveryOpts(true)) ? 0 : 9;
		if (rc == 0)
			vpv::ClearSnapshot();
	}
	else
	{
		vpv::Snapshot leftover;
		if (vpv::LoadSnapshot(leftover))
			VPV_LOG("WARNING: a state file from an unfinished live run exists (display 0x%08X %ls); run --recover-only --i-am-present",
				leftover.displayId, leftover.gdiName);
		VPV_LOG("dry-run complete: no state-changing call was made");
	}
	NvAPI_Unload();
	return rc;
}
