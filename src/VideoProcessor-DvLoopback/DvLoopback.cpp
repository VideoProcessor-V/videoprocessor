// VideoProcessorDvLoopback (DV tunnel loopback, 2026-10-17): the hardware side
// of the loop. Opt-in and separate from the normal suite (like
// VideoProcessorOutputProbe).
//
//   --probe          read-only: DXGI adapters / outputs, which GPU drives each
//                    output, NVIDIA connectors (connected or not), and for each
//                    NVIDIA display its HDR / Dolby Vision capabilities (EDID
//                    VSVDB) and current HDR mode. Changes nothing.
//   --send           OWNER PRESENT ONLY (--i-am-present): full screen on one
//                    NVIDIA-driven output, switches it to NVIDIA's experimental
//                    NV_HDR_MODE_DOLBY_VISION ("VPV"; or UHDA passthrough / SDR),
//                    presents the DV loopback pattern (dv_loopback/
//                    DvLoopbackPattern.h, packed by the VPV packer) once per
//                    refresh and logs every picture's index and QPC to sent.csv;
//                    restores the previous HDR mode on exit, Ctrl+C or error.
//   --restore-sdr    recovery: sets NV_HDR_MODE_OFF on one display (the public
//                    report of a TV stuck black in DV mode).
//
// VP records the capture side (VP_RAW_CAPTURE_PATH + raw_capture_toggle, or
// VideoProcessorStreamRecorder --raw N); the CPU test
// HardwareLoopbackRecordingAnalyses then compares bit for bit
// (docs/VP-dv-tunnel-autotest.md). The output is never labelled Dolby Vision.
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <nvapi.h>

#include <dv_loopback/DvLoopbackPattern.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace
{
	std::atomic<bool> g_stop{ false };
	NvU32 g_engagedDisplay = 0;
	bool g_engaged = false;
	NV_HDR_MODE g_previousMode = NV_HDR_MODE_OFF;

	BOOL WINAPI OnConsole(DWORD)
	{
		g_stop = true;
		return TRUE;
	}

	std::string Narrow(const wchar_t* w)
	{
		char out[512] = {};
		WideCharToMultiByte(CP_UTF8, 0, w, -1, out, sizeof(out), nullptr, nullptr);
		return out;
	}

	const char* VendorName(UINT id)
	{
		switch (id)
		{
		case 0x10DE: return "NVIDIA";
		case 0x8086: return "Intel";
		case 0x1002: return "AMD";
		case 0x1414: return "Microsoft (WARP)";
		default: return "other";
		}
	}

	const char* ColorSpaceName(DXGI_COLOR_SPACE_TYPE c)
	{
		switch (c)
		{
		case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709: return "SDR sRGB (G22 P709)";
		case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020: return "HDR10 (G2084 P2020)";
		case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709: return "scRGB (G10 P709)";
		default: return "other";
		}
	}

	bool NvOk(NvAPI_Status s, const char* what)
	{
		if (s == NVAPI_OK)
			return true;
		NvAPI_ShortString text{};
		NvAPI_GetErrorMessage(s, text);
		printf("  NVAPI %s: %s (%d)\n", what, text, static_cast<int>(s));
		return false;
	}

	NV_HDR_COLOR_DATA HdrGet(NvU32 displayId, bool& ok)
	{
		NV_HDR_COLOR_DATA data{};
		data.version = NV_HDR_COLOR_DATA_VER;
		data.cmd = NV_HDR_CMD_GET;
		ok = NvOk(NvAPI_Disp_HdrColorControl(displayId, &data), "HdrColorControl(GET)");
		return data;
	}

	bool HdrSet(NvU32 displayId, NV_HDR_MODE mode)
	{
		NV_HDR_COLOR_DATA data{};
		data.version = NV_HDR_COLOR_DATA_VER;
		data.cmd = NV_HDR_CMD_SET;
		data.hdrMode = mode;
		data.static_metadata_descriptor_id = NV_STATIC_METADATA_TYPE_1;
		if (mode == NV_HDR_MODE_DOLBY_VISION)
		{
			// DV: RGB, full range, 8 bpc - the RGB8 tunnel container.
			data.hdrColorFormat = NV_COLOR_FORMAT_RGB;
			data.hdrDynamicRange = NV_DYNAMIC_RANGE_VESA;
			data.hdrBpc = NV_BPC_8;
		}
		else
		{
			data.hdrColorFormat = NV_COLOR_FORMAT_DEFAULT;
			data.hdrDynamicRange = NV_DYNAMIC_RANGE_AUTO;
			data.hdrBpc = NV_BPC_DEFAULT;
		}
		return NvOk(NvAPI_Disp_HdrColorControl(displayId, &data), "HdrColorControl(SET)");
	}

	void Restore()
	{
		if (!g_engaged)
			return;
		g_engaged = false;
		printf("Restoring HDR mode %d on display 0x%08X\n", static_cast<int>(g_previousMode), g_engagedDisplay);
		if (!HdrSet(g_engagedDisplay, g_previousMode))
			HdrSet(g_engagedDisplay, NV_HDR_MODE_OFF);
	}

	struct RestoreGuard { ~RestoreGuard() { Restore(); } };

	void PrintHdrCaps(NvU32 displayId)
	{
		NV_HDR_CAPABILITIES caps{};
		caps.version = NV_HDR_CAPABILITIES_VER;
		if (!NvOk(NvAPI_Disp_GetHdrCapabilities(displayId, &caps), "GetHdrCapabilities"))
			return;
		printf("    HDR: ST2084=%u DolbyVision=%u HDR10+=%u; display max %u nits min %.4f nits\n",
			caps.isST2084EotfSupported, caps.isDolbyVisionSupported, caps.isHdr10PlusSupported,
			caps.display_data.desired_content_max_luminance, caps.display_data.desired_content_min_luminance / 10000.0);
		if (caps.isDolbyVisionSupported)
			printf("    DV VSVDB: version %u, DM %u.%u, 2160p60 %u, YUV422-12 %u, interface %u (0 = TV-led / standard, 1 = low latency + standard, 2/3 = low latency), target luminance codes %u..%u\n",
				caps.dv_static_metadata.VSVDB_version, caps.dv_static_metadata.dm_version >> 4, caps.dv_static_metadata.dm_version & 15,
				caps.dv_static_metadata.supports_2160p60hz, caps.dv_static_metadata.supports_YUV422_12bit,
				caps.dv_static_metadata.interface_supported_by_sink, caps.dv_static_metadata.target_min_luminance,
				caps.dv_static_metadata.target_max_luminance);
		else
			printf("    DV: the sink's EDID has no Dolby Vision VSVDB - the driver will not engage NV_HDR_MODE_DOLBY_VISION (put an HDFury with a TV-led DV EDID in the chain)\n");
		bool ok = false;
		const NV_HDR_COLOR_DATA current = HdrGet(displayId, ok);
		if (ok)
			printf("    current HDR mode: %d (0 off, 2 UHDA/HDR10, 5 UHDA passthrough, 7 Dolby Vision)\n", static_cast<int>(current.hdrMode));
	}

	int Probe()
	{
		printf("== DXGI adapters and outputs (which GPU scans out each display) ==\n");
		ComPtr<IDXGIFactory6> factory;
		if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))))
		{
			printf("CreateDXGIFactory2 failed\n");
			return 1;
		}
		ComPtr<IDXGIAdapter1> adapter;
		for (UINT a = 0; factory->EnumAdapters1(a, &adapter) == S_OK; ++a)
		{
			DXGI_ADAPTER_DESC1 desc{};
			adapter->GetDesc1(&desc);
			printf("adapter %u: %s (%s, vendor 0x%04X, device 0x%04X, %llu MB)%s\n", a, Narrow(desc.Description).c_str(),
				VendorName(desc.VendorId), desc.VendorId, desc.DeviceId,
				static_cast<unsigned long long>(desc.DedicatedVideoMemory >> 20),
				(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? " [software]" : "");
			ComPtr<IDXGIOutput> output;
			UINT outputs = 0;
			for (UINT o = 0; adapter->EnumOutputs(o, &output) == S_OK; ++o, ++outputs)
			{
				DXGI_OUTPUT_DESC od{};
				output->GetDesc(&od);
				std::string extra;
				ComPtr<IDXGIOutput6> o6;
				if (SUCCEEDED(output.As(&o6)))
				{
					DXGI_OUTPUT_DESC1 d1{};
					if (SUCCEEDED(o6->GetDesc1(&d1)))
					{
						char text[160];
						snprintf(text, sizeof(text), ", %u bpc, %s, max %.0f nits", d1.BitsPerColor, ColorSpaceName(d1.ColorSpace), d1.MaxLuminance);
						extra = text;
					}
				}
				MONITORINFOEXW mi{};
				mi.cbSize = sizeof(mi);
				GetMonitorInfoW(od.Monitor, &mi);
				DEVMODEW mode{};
				mode.dmSize = sizeof(mode);
				EnumDisplaySettingsW(od.DeviceName, ENUM_CURRENT_SETTINGS, &mode);
				printf("  output %u: %s %ldx%ld @ %lu Hz, attached=%d%s\n", o, Narrow(od.DeviceName).c_str(),
					od.DesktopCoordinates.right - od.DesktopCoordinates.left, od.DesktopCoordinates.bottom - od.DesktopCoordinates.top,
					mode.dmDisplayFrequency, od.AttachedToDesktop ? 1 : 0, extra.c_str());
			}
			if (outputs == 0)
				printf("  (no outputs: this adapter scans out no attached display now - on an Optimus laptop the dGPU renders and the iGPU scans out)\n");
		}

		printf("\n== NVIDIA GPUs and display connectors (NVAPI, read-only) ==\n");
		if (!NvOk(NvAPI_Initialize(), "Initialize"))
			return 0;
		NvPhysicalGpuHandle gpus[NVAPI_MAX_PHYSICAL_GPUS] = {};
		NvU32 gpuCount = 0;
		if (NvOk(NvAPI_EnumPhysicalGPUs(gpus, &gpuCount), "EnumPhysicalGPUs"))
		{
			for (NvU32 g = 0; g < gpuCount; ++g)
			{
				NvAPI_ShortString name{};
				NvAPI_GPU_GetFullName(gpus[g], name);
				NvU32 count = 0;
				NvAPI_GPU_GetAllDisplayIds(gpus[g], nullptr, &count);
				std::vector<NV_GPU_DISPLAYIDS> ids(count);
				for (auto& id : ids) id.version = NV_GPU_DISPLAYIDS_VER;
				printf("GPU %u: %s - %u display connector id(s) known to the driver\n", g, name, count);
				if (count && NvOk(NvAPI_GPU_GetAllDisplayIds(gpus[g], ids.data(), &count), "GetAllDisplayIds"))
					for (NvU32 k = 0; k < count; ++k)
						printf("  display id 0x%08X: connected=%u physically=%u active=%u os_visible=%u dynamic(MST)=%u\n",
							ids[k].displayId, ids[k].isConnected, ids[k].isPhysicallyConnected, ids[k].isActive,
							ids[k].isOSVisible, ids[k].isDynamic);
				if (count == 0)
					printf("  -> this GPU has no display connectors of its own: every port is scanned out by another GPU (Optimus without a MUX). VPV cannot run on its outputs.\n");
			}
		}
		// NVIDIA displays by GDI name.
		DISPLAY_DEVICEA dd{};
		dd.cb = sizeof(dd);
		for (DWORD i = 0; EnumDisplayDevicesA(nullptr, i, &dd, 0); ++i)
		{
			if (!(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP))
				continue;
			NvU32 displayId = 0;
			const bool nvidia = NvAPI_DISP_GetDisplayIdByDisplayName(dd.DeviceName, &displayId) == NVAPI_OK;
			printf("%s (%s): %s\n", dd.DeviceName, dd.DeviceString, nvidia ? "driven by NVIDIA" : "not an NVIDIA display");
			if (nvidia)
				PrintHdrCaps(displayId);
		}
		NvAPI_Unload();
		return 0;
	}

	struct SendOptions
	{
		std::wstring display;
		std::wstring out;
		double seconds = 20.0;
		std::string mode = "tunnel";
		bool present = false;
		bool force = false;
		DvLoopback::SequenceConfig sequence;
	};

	bool FindMonitor(const std::wstring& name, RECT& rect, DWORD& hz)
	{
		DEVMODEW mode{};
		mode.dmSize = sizeof(mode);
		if (!EnumDisplaySettingsW(name.c_str(), ENUM_CURRENT_SETTINGS, &mode))
			return false;
		rect = { mode.dmPosition.x, mode.dmPosition.y, mode.dmPosition.x + static_cast<LONG>(mode.dmPelsWidth),
			mode.dmPosition.y + static_cast<LONG>(mode.dmPelsHeight) };
		hz = mode.dmDisplayFrequency;
		return true;
	}

	int Send(SendOptions o)
	{
		if (!o.present)
		{
			printf("--send changes the display's HDR mode and takes over the screen: run it only with the owner present (--i-am-present).\n");
			return 2;
		}
		RECT rect{};
		DWORD hz = 0;
		if (!FindMonitor(o.display, rect, hz))
		{
			printf("Display %s not found (see --probe).\n", Narrow(o.display.c_str()).c_str());
			return 2;
		}
		const uint32_t width = static_cast<uint32_t>(rect.right - rect.left), height = static_cast<uint32_t>(rect.bottom - rect.top);
		o.sequence.width = width;
		o.sequence.height = height;
		if (o.sequence.period == 0)
			o.sequence.period = o.sequence.sceneFrames * 4;
		if (!NvOk(NvAPI_Initialize(), "Initialize"))
			return 2;
		NvU32 displayId = 0;
		if (NvAPI_DISP_GetDisplayIdByDisplayName(Narrow(o.display.c_str()).c_str(), &displayId) != NVAPI_OK)
		{
			printf("%s is not driven by an NVIDIA GPU: NVIDIA's DV mode cannot engage there.\n", Narrow(o.display.c_str()).c_str());
			return 2;
		}
		NV_HDR_CAPABILITIES caps{};
		caps.version = NV_HDR_CAPABILITIES_VER;
		NvAPI_Disp_GetHdrCapabilities(displayId, &caps);
		if (o.mode == "tunnel" && !caps.isDolbyVisionSupported && !o.force)
		{
			printf("The sink does not advertise Dolby Vision (no VSVDB in its EDID): put an HDFury with a TV-led DV EDID before the capture card, or pass --force.\n");
			return 2;
		}
		// Precompute the pictures (content repeats every `period` frames; only
		// the stamp rows change per frame).
		const DvLoopback::PatternSequence sequence(o.sequence);
		printf("Preparing %u pictures %ux%u ...\n", o.sequence.period, width, height);
		std::vector<std::vector<uint8_t>> pictures;
		for (uint32_t k = 0; k < o.sequence.period; ++k)
		{
			const DvLoopback::LoopFrame f = sequence.Make(k);
			const size_t metadataPixels = f.sdr ? 0 : DvLoopback::Packetize(DvLoopback::SerializeDm(f.effectiveDm), f.header).size() * 3072 * 2;
			if (metadataPixels >= static_cast<size_t>(DvLoopback::StampFirstRow(height)) * width)
			{
				printf("Raster too small: the stamp rows overlap the metadata area.\n");
				return 2;
			}
			std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4);
			for (size_t p = 0; p < static_cast<size_t>(width) * height; ++p)
			{
				rgba[p * 4] = f.rgb8[p * 3]; rgba[p * 4 + 1] = f.rgb8[p * 3 + 1]; rgba[p * 4 + 2] = f.rgb8[p * 3 + 2]; rgba[p * 4 + 3] = 255;
			}
			pictures.push_back(std::move(rgba));
		}
		CreateDirectoryW(o.out.c_str(), nullptr);
		FILE* log = nullptr;
		_wfopen_s(&log, (o.out + L"\\sent.csv").c_str(), L"w");
		if (!log)
		{
			printf("Cannot write sent.csv in the output folder.\n");
			return 2;
		}
		LARGE_INTEGER frequency{};
		QueryPerformanceFrequency(&frequency);
		fprintf(log, "# sequence width=%u height=%u sequence_id=%u scene_frames=%u sdr_start=%u sdr_length=%u sdr_eos=%u rich=%u qpc_frequency=%lld period=%u mode=%s display=%s refresh=%lu\n",
			width, height, o.sequence.sequenceId, o.sequence.sceneFrames, o.sequence.sdrStart, o.sequence.sdrLength,
			o.sequence.sdrEos ? 1 : 0, o.sequence.richMetadata ? 1 : 0, static_cast<long long>(frequency.QuadPart),
			o.sequence.period, o.mode.c_str(), Narrow(o.display.c_str()).c_str(), hz);
		fprintf(log, "index,present_qpc,sync_qpc,present_count\n");

		// Window and swapchain on the adapter that owns the output.
		WNDCLASSW wc{};
		wc.lpfnWndProc = DefWindowProcW;
		wc.hInstance = GetModuleHandleW(nullptr);
		wc.lpszClassName = L"VpDvLoopback";
		RegisterClassW(&wc);
		HWND hwnd = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, L"VP DV loopback", WS_POPUP, rect.left, rect.top,
			static_cast<int>(width), static_cast<int>(height), nullptr, nullptr, wc.hInstance, nullptr);
		ShowWindow(hwnd, SW_SHOW);
		SetForegroundWindow(hwnd);
		ComPtr<IDXGIFactory6> factory;
		CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
		ComPtr<IDXGIAdapter1> chosen, adapter;
		for (UINT a = 0; !chosen && factory->EnumAdapters1(a, &adapter) == S_OK; ++a)
		{
			ComPtr<IDXGIOutput> output;
			for (UINT k = 0; adapter->EnumOutputs(k, &output) == S_OK; ++k)
			{
				DXGI_OUTPUT_DESC od{};
				output->GetDesc(&od);
				if (o.display == od.DeviceName) { chosen = adapter; break; }
			}
		}
		ComPtr<ID3D11Device> device;
		ComPtr<ID3D11DeviceContext> context;
		if (!chosen || FAILED(D3D11CreateDevice(chosen.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
			D3D11_SDK_VERSION, &device, nullptr, &context)))
		{
			printf("No D3D11 device on the output's adapter.\n");
			return 2;
		}
		DXGI_SWAP_CHAIN_DESC1 sc{};
		sc.Width = width;
		sc.Height = height;
		sc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		sc.SampleDesc.Count = 1;
		sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		sc.BufferCount = 2;
		sc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		ComPtr<IDXGISwapChain1> swapchain;
		if (FAILED(factory->CreateSwapChainForHwnd(device.Get(), hwnd, &sc, nullptr, nullptr, &swapchain)))
		{
			printf("Swapchain creation failed.\n");
			return 2;
		}
		ComPtr<IDXGISwapChain3> swapchain3;
		swapchain.As(&swapchain3);
		if (swapchain3)
			swapchain3->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);   // identity: bytes pass through

		// Engage the mode (restored by the guard on every exit path).
		bool ok = false;
		const NV_HDR_COLOR_DATA before = HdrGet(displayId, ok);
		g_previousMode = ok ? before.hdrMode : NV_HDR_MODE_OFF;
		g_engagedDisplay = displayId;
		RestoreGuard guard;
		SetConsoleCtrlHandler(OnConsole, TRUE);
		const NV_HDR_MODE mode = o.mode == "tunnel" ? NV_HDR_MODE_DOLBY_VISION :
			o.mode == "hdr10" ? NV_HDR_MODE_UHDA_PASSTHROUGH : NV_HDR_MODE_OFF;
		if (mode != NV_HDR_MODE_OFF)
		{
			g_engaged = true;
			if (!HdrSet(displayId, mode))
			{
				printf("The driver refused the mode.\n");
				return 3;
			}
		}
		printf("Sending %s for %.0f s on %s (%ux%u @ %lu Hz). Ctrl+C stops and restores.\n", o.mode.c_str(), o.seconds,
			Narrow(o.display.c_str()).c_str(), width, height, hz);
		const auto start = std::chrono::steady_clock::now();
		std::vector<uint8_t> frame;
		for (uint32_t n = 0; !g_stop && std::chrono::steady_clock::now() - start < std::chrono::duration<double>(o.seconds); ++n)
		{
			MSG msg;
			while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
			const uint32_t k = n % o.sequence.period;
			frame = pictures[k];
			// Rewrite the stamp for the real index (RGBA: G = I[11:4]; the stamp
			// levels have a zero low nibble, so B keeps C[3:0] << 4 for DV).
			std::vector<uint8_t> rgb(static_cast<size_t>(width) * DvLoopback::kStampRows * 3, 0);
			const uint64_t word = DvLoopback::StampWord(o.sequence.sequenceId, n);
			const bool sdr = sequence.IsSdr(k);
			for (uint32_t r = 0; r < DvLoopback::kStampRows; ++r)
				for (uint32_t bit = 0; bit < DvLoopback::kStampBits; ++bit)
				{
					const bool one = ((word >> (DvLoopback::kStampBits - 1 - bit)) & 1ULL) != 0;
					const size_t p = static_cast<size_t>(DvLoopback::StampFirstRow(height) + r) * width + bit;
					const uint8_t level = one ? 192 : 64;
					frame[p * 4 + 1] = level;
					if (sdr) { frame[p * 4] = level; frame[p * 4 + 2] = level; }
					else frame[p * 4 + 2] = static_cast<uint8_t>(frame[p * 4 + 2] & 0xF0);
				}
			ComPtr<ID3D11Texture2D> back;
			swapchain->GetBuffer(0, IID_PPV_ARGS(&back));
			context->UpdateSubresource(back.Get(), 0, nullptr, frame.data(), width * 4, 0);
			swapchain->Present(1, 0);
			LARGE_INTEGER now{};
			QueryPerformanceCounter(&now);
			DXGI_FRAME_STATISTICS stats{};
			const bool haveStats = SUCCEEDED(swapchain->GetFrameStatistics(&stats));
			fprintf(log, "%u,%lld,%lld,%u\n", n, static_cast<long long>(now.QuadPart),
				haveStats ? static_cast<long long>(stats.SyncQPCTime.QuadPart) : 0LL, haveStats ? stats.PresentCount : 0U);
		}
		fclose(log);
		DestroyWindow(hwnd);
		printf("Done; sent.csv written to %s\n", Narrow(o.out.c_str()).c_str());
		return 0;
	}
}

int wmain(int argc, wchar_t** argv)
{
	std::wstring command;
	SendOptions send;
	std::wstring display;
	for (int i = 1; i < argc; ++i)
	{
		const std::wstring a = argv[i];
		const bool value = i + 1 < argc;
		if (a == L"--probe" || a == L"--send" || a == L"--restore-sdr") command = a;
		else if (a == L"--display" && value) display = argv[++i];
		else if (a == L"--out" && value) send.out = argv[++i];
		else if (a == L"--seconds" && value) send.seconds = _wtof(argv[++i]);
		else if (a == L"--mode" && value) send.mode = Narrow(argv[++i]);
		else if (a == L"--i-am-present") send.present = true;
		else if (a == L"--force") send.force = true;
		else if (a == L"--scene-frames" && value) send.sequence.sceneFrames = static_cast<uint32_t>(_wtoi(argv[++i]));
		else if (a == L"--period" && value) send.sequence.period = static_cast<uint32_t>(_wtoi(argv[++i]));
		else if (a == L"--sdr-start" && value) send.sequence.sdrStart = static_cast<uint32_t>(_wtoi(argv[++i]));
		else if (a == L"--sdr-length" && value) send.sequence.sdrLength = static_cast<uint32_t>(_wtoi(argv[++i]));
		else if (a == L"--no-eos") send.sequence.sdrEos = false;
	}
	if (command == L"--probe")
		return Probe();
	if (command == L"--restore-sdr")
	{
		if (display.empty() || !NvOk(NvAPI_Initialize(), "Initialize"))
			return 2;
		NvU32 id = 0;
		if (!NvOk(NvAPI_DISP_GetDisplayIdByDisplayName(Narrow(display.c_str()).c_str(), &id), "GetDisplayIdByDisplayName"))
			return 2;
		return HdrSet(id, NV_HDR_MODE_OFF) ? 0 : 3;
	}
	if (command == L"--send")
	{
		send.display = display;
		if (send.out.empty())
			send.out = L"dv-loopback-run";
		const int rc = Send(send);
		Restore();
		return rc;
	}
	printf("VideoProcessorDvLoopback --probe | --send --display \\\\.\\DISPLAYn --out <folder> --i-am-present [--mode tunnel|hdr10|sdr] [--seconds S]\n"
		"  [--scene-frames N] [--period N] [--sdr-start N --sdr-length N] [--no-eos] [--force] | --restore-sdr --display \\\\.\\DISPLAYn\n");
	return 1;
}
