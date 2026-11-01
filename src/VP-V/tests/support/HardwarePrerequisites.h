#pragma once

// Hardware preconditions for tests that need more than a CPU and a build: a desktop
// large enough for the requested window bounds, a performance-class CPU, or a
// hardware D3D11 adapter the libplacebo renderer can start on.
//
// Each check measures the condition itself (never "am I on CI"). When it is missing,
// RequireConditionOrSkip logs the fixed marker
//   SKIP-NO-HARDWARE <test>: <reason>
// (searchable in the TRX; the CI summary counts it separately) and the test returns.
// With VP_TEST_REQUIRE_HARDWARE=1 (the owner's local gates) the test fails instead,
// so a machine that should have the hardware never passes by skipping.

#include "CppUnitTest.h"

#include <d3d11.h>
#include <dxgi.h>

#include <cstdio>
#include <cstdlib>
#include <string>

namespace HardwarePrerequisites
{
	inline bool RequireHardware()
	{
		char* v = nullptr;
		size_t n = 0;
		const bool on = _dupenv_s(&v, &n, "VP_TEST_REQUIRE_HARDWARE") == 0 && v && std::string(v) == "1";
		std::free(v);
		return on;
	}

	// True when the test can run. Otherwise logs the SKIP-NO-HARDWARE marker and returns
	// false, or fails the test under VP_TEST_REQUIRE_HARDWARE=1.
	inline bool RequireConditionOrSkip(const char* test, bool met, const std::string& reason)
	{
		using Microsoft::VisualStudio::CppUnitTestFramework::Assert;
		using Microsoft::VisualStudio::CppUnitTestFramework::Logger;
		if (met)
			return true;
		const std::string marker = std::string("SKIP-NO-HARDWARE ") + test + ": " + reason;
		if (RequireHardware())
		{
			const std::string message = marker + " (VP_TEST_REQUIRE_HARDWARE=1: a missing precondition fails)";
			Assert::Fail(std::wstring(message.begin(), message.end()).c_str());
		}
		Logger::WriteMessage((marker + "\n").c_str());
		return false;
	}

	// Windows caps a window's size at the maximum tracking size (SM_CX/CYMAXTRACK), which it
	// derives from the desktop; a SetWindowPos to larger bounds is clamped. The work area is
	// reported alongside for context.
	inline bool DesktopFitsWindow(int width, int height, std::string& reason)
	{
		const int maxTrackX = GetSystemMetrics(SM_CXMAXTRACK);
		const int maxTrackY = GetSystemMetrics(SM_CYMAXTRACK);
		RECT work{};
		SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
		char text[256] = {};
		sprintf_s(text, "window %dx%d exceeds the desktop's maximum window size %dx%d (work area %ldx%ld)",
			width, height, maxTrackX, maxTrackY, work.right - work.left, work.bottom - work.top);
		reason = text;
		return maxTrackX >= width && maxTrackY >= height;
	}

	// The sustained 4K60 converter budgets are set for the machines VP runs on (the laptop
	// has 16 logical processors, the cinema PC more); below this count the CPU class cannot
	// be expected to hold them while the OS and test host share the cores.
	constexpr unsigned kPerformanceLogicalProcessors = 8;

	inline bool PerformanceCpu(std::string& reason)
	{
		const DWORD logical = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
		char text[160] = {};
		sprintf_s(text, "%lu logical processors, fewer than the %u the sustained 4K60 budgets assume",
			static_cast<unsigned long>(logical), kPerformanceLogicalProcessors);
		reason = text;
		return logical >= kPerformanceLogicalProcessors;
	}

	// Mirrors the renderer's own device request (LibplaceboVideoRenderer: allow_software=false,
	// min_feature_level 10_0, default adapter): a hardware D3D11 device must be creatable, and
	// the adapter behind it must not be WARP / the Microsoft Basic Render Driver.
	inline bool HardwareD3D11Renderer(std::string& reason)
	{
		using CreateDevice = decltype(&D3D11CreateDevice);
		HMODULE d3d11 = LoadLibraryW(L"d3d11.dll");
		const auto create = d3d11 ? reinterpret_cast<CreateDevice>(GetProcAddress(d3d11, "D3D11CreateDevice")) : nullptr;
		if (!create)
		{
			reason = "d3d11.dll unavailable";
			if (d3d11) FreeLibrary(d3d11);
			return false;
		}
		const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
			D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
		ID3D11Device* device = nullptr;
		D3D_FEATURE_LEVEL level{};
		HRESULT hr = create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, ARRAYSIZE(levels),
			D3D11_SDK_VERSION, &device, &level, nullptr);
		if (hr == E_INVALIDARG) // D3D 11.0 runtime: no 11_1 in the list
			hr = create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels + 1, ARRAYSIZE(levels) - 1,
				D3D11_SDK_VERSION, &device, &level, nullptr);
		bool ok = false;
		char text[256] = {};
		if (FAILED(hr) || !device)
		{
			sprintf_s(text, "no hardware D3D11 device at feature level 10_0 or above (hr 0x%08lX)", static_cast<unsigned long>(hr));
		}
		else
		{
			DXGI_ADAPTER_DESC1 desc{};
			bool described = false;
			IDXGIDevice* dxgiDevice = nullptr;
			if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice))))
			{
				IDXGIAdapter* adapter = nullptr;
				if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter)))
				{
					IDXGIAdapter1* adapter1 = nullptr;
					if (SUCCEEDED(adapter->QueryInterface(__uuidof(IDXGIAdapter1), reinterpret_cast<void**>(&adapter1))))
					{
						described = SUCCEEDED(adapter1->GetDesc1(&desc));
						adapter1->Release();
					}
					adapter->Release();
				}
				dxgiDevice->Release();
			}
			char name[128] = {};
			WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name) - 1, nullptr, nullptr);
			const bool software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0 ||
				(desc.VendorId == 0x1414 && desc.DeviceId == 0x8C); // Microsoft Basic Render Driver
			if (!described)
				sprintf_s(text, "the D3D11 device's adapter could not be described");
			else if (software)
				sprintf_s(text, "only a software D3D11 adapter (%s, %04X:%04X); the renderer refuses software devices",
					name, desc.VendorId, desc.DeviceId);
			else
			{
				sprintf_s(text, "hardware D3D11 adapter %s, feature level 0x%04X", name, static_cast<unsigned>(level));
				ok = true;
			}
			device->Release();
		}
		reason = text;
		FreeLibrary(d3d11);
		return ok;
	}
}
