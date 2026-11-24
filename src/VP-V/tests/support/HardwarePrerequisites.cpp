// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "HardwarePrerequisites.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <cstdio>
#include <cstdlib>
#include <string>

namespace HardwarePrerequisites
{
	// True when the named environment variable is exactly "1".
	bool EnvIsOne(const char* name)
	{
		char* v = nullptr;
		size_t n = 0;
		const bool on = _dupenv_s(&v, &n, name) == 0 && v && std::string(v) == "1";
		std::free(v);
		return on;
	}

	// VP_TEST_REQUIRE_ALL=1 makes every kind of missing precondition fail.
	bool RequireAll()
	{
		return EnvIsOne("VP_TEST_REQUIRE_ALL");
	}

	bool RequireHardware()
	{
		return RequireAll() || EnvIsOne("VP_TEST_REQUIRE_HARDWARE");
	}

	bool Required(Kind kind)
	{
		if (RequireAll())
			return true;
		switch (kind)
		{
		case Kind::Hardware: return EnvIsOne("VP_TEST_REQUIRE_HARDWARE");
		case Kind::Gpu: return EnvIsOne("VP_TEST_REQUIRE_GPU");
		case Kind::Meter: return EnvIsOne("VP_TEST_REQUIRE_METER");
		case Kind::Projector: return EnvIsOne("VP_TEST_REQUIRE_PROJECTOR");
		case Kind::Footage: return EnvIsOne("VP_TEST_REQUIRE_FOOTAGE");
		case Kind::Corpus: return EnvIsOne("VP_TEST_REQUIRE_CORPUS");
		case Kind::Bench: return EnvIsOne("VP_TEST_REQUIRE_BENCH");
		case Kind::ParkedNets: return EnvIsOne("VP_TEST_REQUIRE_NETS");
		}
		return false;
	}

	const char* KindMarker(Kind kind)
	{
		switch (kind)
		{
		case Kind::Hardware: return "SKIP-NO-HARDWARE";
		case Kind::Gpu: return "SKIP-NO-GPU";
		case Kind::Meter: return "SKIP-NO-METER";
		case Kind::Projector: return "SKIP-NO-PROJECTOR";
		case Kind::Footage: return "SKIP-NO-FOOTAGE";
		case Kind::Corpus: return "SKIP-NO-CORPUS";
		case Kind::Bench: return "SKIP-OPT-IN"; // a benchmark is opt-in (VP_TEST_BENCH=1); check_trx_coverage counts it as not executed
		case Kind::ParkedNets: return "SKIP-PARKED-NETS"; // owner-parked neural-net tests; check_trx_coverage counts it as not executed
		}
		return "SKIP";
	}

	const char* KindEnvName(Kind kind)
	{
		switch (kind)
		{
		case Kind::Hardware: return "VP_TEST_REQUIRE_HARDWARE";
		case Kind::Gpu: return "VP_TEST_REQUIRE_GPU";
		case Kind::Meter: return "VP_TEST_REQUIRE_METER";
		case Kind::Projector: return "VP_TEST_REQUIRE_PROJECTOR";
		case Kind::Footage: return "VP_TEST_REQUIRE_FOOTAGE";
		case Kind::Corpus: return "VP_TEST_REQUIRE_CORPUS";
		case Kind::Bench: return "VP_TEST_REQUIRE_BENCH";
		case Kind::ParkedNets: return "VP_TEST_REQUIRE_NETS";
		}
		return "VP_TEST_REQUIRE_ALL";
	}

	// True when the test can run. Otherwise logs "<SKIP-NO-...> <test>: <reason>" and returns
	// false, or fails the test when that kind (or VP_TEST_REQUIRE_ALL=1) requires it.
	bool RequireOrSkip(Kind kind, const char* test, bool met, const std::string& reason)
	{
		using Microsoft::VisualStudio::CppUnitTestFramework::Assert;
		using Microsoft::VisualStudio::CppUnitTestFramework::Logger;
		if (met)
			return true;
		const std::string marker = std::string(KindMarker(kind)) + " " + test + ": " + reason;
		if (Required(kind))
		{
			const std::string message = marker + " (" + KindEnvName(kind) + "=1 or VP_TEST_REQUIRE_ALL=1: a missing precondition fails)";
			Assert::Fail(std::wstring(message.begin(), message.end()).c_str());
		}
		Logger::WriteMessage((marker + "\n").c_str());
		return false;
	}

	// True when a Bench test is opted in: VP_TEST_BENCH=1. Use RequireOrSkip(Kind::Bench, test, BenchOptedIn(), "set VP_TEST_BENCH=1"):
	// without the opt-in the test logs SKIP-OPT-IN (not executed) and returns; under VP_TEST_REQUIRE_BENCH=1 or
	// VP_TEST_REQUIRE_ALL=1 (the dedicated Bench job, gen_test_filters --job-env Bench) the missing opt-in fails.
	bool BenchOptedIn()
	{
		return EnvIsOne("VP_TEST_BENCH");
	}

	// True when the test can run. Otherwise logs the SKIP-NO-HARDWARE marker and returns
	// false, or fails the test under VP_TEST_REQUIRE_HARDWARE=1 / VP_TEST_REQUIRE_ALL=1.
	bool RequireConditionOrSkip(const char* test, bool met, const std::string& reason)
	{
		return RequireOrSkip(Kind::Hardware, test, met, reason);
	}

	// Windows caps a window's size at the maximum tracking size (SM_CX/CYMAXTRACK), which it
	// derives from the desktop; a SetWindowPos to larger bounds is clamped. The work area is
	// reported alongside for context.
	bool DesktopFitsWindow(int width, int height, std::string& reason)
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

	bool PerformanceCpu(std::string& reason)
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
	bool HardwareD3D11Renderer(std::string& reason)
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
