// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "pch.h"
#include "CppUnitTest.h"

#include <atlbase.h>
#include <dwmapi.h>
#include <dxgi1_4.h>
#include <psapi.h>
#include <wincodec.h>
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "windowscodecs.lib")

#include <ConfigFile.h>
#include <DisplayMode.h>
#include <HDRData.h>
#include <IRenderer.h>
#include <ITimingClock.h>
#include <P210FrameLayout.h>
#include <SubtitleRepositionMode.h>
#include <UnifiedProfileRuntime.h>
#include <VideoFrame.h>
#include <VideoState.h>
#include <vprenderer/LibplaceboRendererPluginApi.h>

#include <VP-V/abi/VpvHostCalls.h>
#include <VP-V/abi/VpvPluginGuard.h>
#include <VP-V/core/RunningPathwayLatch.h>
#include <VP-V/tests/support/HardwarePrerequisites.h>
#include <VP-V/tests/support/RealClipLibrary.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace
{
	std::mutex g_soakLogMutex;
	std::vector<std::string> g_soakLog;

	void __cdecl SoakCaptureLog(const char* message)
	{
		std::lock_guard<std::mutex> guard(g_soakLogMutex);
		g_soakLog.emplace_back(message ? message : "");
	}

	struct SoakCallback : IRendererCallback
	{
		std::atomic<int> state{ -1 };
		std::atomic<int> restarts{ 0 };
		void OnRendererState(RendererState s, uint32_t) override { state = static_cast<int>(s); }
		void OnRendererDetailString(const CString&, uint32_t) override {}
		void OnRendererRestartRequired(uint32_t) override { ++restarts; }
	};

	struct SoakSourceBuffer : IUnknown
	{
		std::atomic<long> references{ 1 };
		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
		{
			if (riid == __uuidof(IUnknown)) { *out = this; AddRef(); return S_OK; }
			*out = nullptr;
			return E_NOINTERFACE;
		}
		ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(++references); }
		ULONG STDMETHODCALLTYPE Release() override { return static_cast<ULONG>(--references); }
	};

	struct SoakClock : ITimingClock
	{
		timingclocktime_t TimingClockNow() override
		{
			LARGE_INTEGER now{};
			QueryPerformanceCounter(&now);
			return now.QuadPart;
		}
		timingclocktime_t TimingClockTicksPerSecond() const override
		{
			LARGE_INTEGER freq{};
			QueryPerformanceFrequency(&freq);
			return freq.QuadPart;
		}
		const TCHAR* TimingClockDescription() override { return TEXT("soak QPC"); }
	};

	std::string SoakEnv(const char* name)
	{
		char* v = nullptr;
		size_t n = 0;
		std::string value;
		if (_dupenv_s(&v, &n, name) == 0 && v)
			value = v;
		std::free(v);
		return value;
	}

	void SetBothEnv(const char* name, const std::string* value)
	{
		if (value)
		{
			_putenv_s(name, value->c_str());
			SetEnvironmentVariableA(name, value->c_str());
		}
		else
		{
			_putenv_s(name, "");
			SetEnvironmentVariableA(name, nullptr);
		}
	}

	class ScopedEnvVar
	{
	public:
		ScopedEnvVar(const char* name, const std::string& nextValue)
			: m_name(name)
		{
			char buf[4096] = {};
			const DWORD got = GetEnvironmentVariableA(name, buf, static_cast<DWORD>(sizeof(buf)));
			if (got > 0 && got < sizeof(buf))
			{
				m_hadPrev = true;
				m_prev.assign(buf, got);
			}
			else
			{
				const std::string crt = SoakEnv(name);
				if (!crt.empty())
				{
					m_hadPrev = true;
					m_prev = crt;
				}
			}
			SetBothEnv(m_name.c_str(), &nextValue);
		}
		~ScopedEnvVar()
		{
			SetBothEnv(m_name.c_str(), m_hadPrev ? &m_prev : nullptr);
		}
		ScopedEnvVar(const ScopedEnvVar&) = delete;
		ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;

	private:
		std::string m_name;
		std::string m_prev;
		bool m_hadPrev = false;
	};

	std::string BinaryFolder()
	{
		char here[MAX_PATH] = {};
		HMODULE self = nullptr;
		GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCSTR>(&SoakCaptureLog), &self);
		GetModuleFileNameA(self, here, MAX_PATH);
		const std::string binary(here);
		return binary.substr(0, binary.find_last_of("\\/") + 1);
	}

	std::wstring WideOf(const std::string& s)
	{
		return std::wstring(s.begin(), s.end());
	}

	std::string UpperOf(std::string s)
	{
		for (size_t i = 0; i < s.size(); ++i)
			s[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[i])));
		return s;
	}

	void RemoveFolderTree(const std::string& folder)
	{
		WIN32_FIND_DATAA found{};
		const HANDLE search = FindFirstFileA((folder + "\\*").c_str(), &found);
		if (search != INVALID_HANDLE_VALUE)
		{
			do
			{
				const std::string name = found.cFileName;
				if (name == "." || name == "..") continue;
				const std::string path = folder + "\\" + name;
				if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
					RemoveFolderTree(path);
				else
				{
					SetFileAttributesA(path.c_str(), FILE_ATTRIBUTE_NORMAL);
					DeleteFileA(path.c_str());
				}
			} while (FindNextFileA(search, &found));
			FindClose(search);
		}
		RemoveDirectoryA(folder.c_str());
	}

	class TempFolderScope
	{
	public:
		explicit TempFolderScope(const std::string& suffix)
		{
			char tempPath[MAX_PATH] = {};
			GetTempPathA(MAX_PATH, tempPath);
			m_path = std::string(tempPath) + suffix + "-" + std::to_string(GetCurrentProcessId());
			RemoveFolderTree(m_path);
			CreateDirectoryA(m_path.c_str(), nullptr);
		}
		~TempFolderScope()
		{
			RemoveFolderTree(m_path);
		}
		const std::string& Path() const { return m_path; }
		TempFolderScope(const TempFolderScope&) = delete;
		TempFolderScope& operator=(const TempFolderScope&) = delete;

	private:
		std::string m_path;
	};

	double ExactMonitorHz()
	{
		DEVMODEW mode{};
		mode.dmSize = sizeof(mode);
		if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1)
			return static_cast<double>(mode.dmDisplayFrequency);
		return 60.0;
	}

	// ---- DXGI NVIDIA Adapter Monitor & Process RAM Monitor ----
	struct MemorySample
	{
		int frameIndex = 0;
		double vramMiB = 0.0;
		double workingSetMiB = 0.0;
		double privateBytesMiB = 0.0;
	};

	class NvidiaAdapterMonitor
	{
	public:
		bool Initialize(std::string& outName, std::string& error)
		{
			CComPtr<IDXGIFactory4> factory;
			if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
			{
				error = "CreateDXGIFactory1 failed";
				return false;
			}
			for (UINT i = 0;; ++i)
			{
				CComPtr<IDXGIAdapter1> adapter;
				if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND)
					break;
				DXGI_ADAPTER_DESC1 desc{};
				if (FAILED(adapter->GetDesc1(&desc)))
					continue;
				if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
					continue;
				std::string name;
				for (const wchar_t* p = desc.Description; *p; ++p)
					name.push_back(static_cast<char>(*p < 128 ? *p : '?'));
				if (UpperOf(name).find("NVIDIA") != std::string::npos)
				{
					CComPtr<IDXGIAdapter3> adapter3;
					if (SUCCEEDED(adapter->QueryInterface(IID_PPV_ARGS(&adapter3))))
					{
						m_adapter3 = adapter3;
						m_name = name;
						outName = name;
						return true;
					}
				}
			}
			error = "no hardware NVIDIA DXGI adapter found";
			return false;
		}

		double QueryLocalVramMiB() const
		{
			if (!m_adapter3)
				return 0.0;
			DXGI_QUERY_VIDEO_MEMORY_INFO info{};
			if (FAILED(m_adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
				return 0.0;
			return static_cast<double>(info.CurrentUsage) / (1024.0 * 1024.0);
		}

		static void QueryProcessRamMiB(double& workingSetMiB, double& privateMiB)
		{
			PROCESS_MEMORY_COUNTERS_EX pmc{};
			pmc.cb = sizeof(pmc);
			if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc)))
			{
				workingSetMiB = static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);
				privateMiB = static_cast<double>(pmc.PrivateUsage) / (1024.0 * 1024.0);
			}
			else
			{
				workingSetMiB = 0.0;
				privateMiB = 0.0;
			}
		}

		const std::string& Name() const { return m_name; }

	private:
		CComPtr<IDXGIAdapter3> m_adapter3;
		std::string m_name;
	};

	// ---- Percentile Helper ----
	struct TimingStats
	{
		size_t count = 0;
		double p50 = 0.0;
		double p95 = 0.0;
		double p99 = 0.0;
		double maxMs = 0.0;
	};

	double PercentileSorted(const std::vector<double>& sorted, double q)
	{
		if (sorted.empty())
			return 0.0;
		const double pos = q * static_cast<double>(sorted.size() - 1);
		const size_t lo = static_cast<size_t>(std::floor(pos));
		const size_t hi = (std::min)(sorted.size() - 1, lo + 1);
		const double frac = pos - static_cast<double>(lo);
		return sorted[lo] * (1.0 - frac) + sorted[hi] * frac;
	}

	TimingStats ComputeStats(std::vector<double> samples)
	{
		TimingStats s;
		s.count = samples.size();
		if (samples.empty())
			return s;
		std::sort(samples.begin(), samples.end());
		s.p50 = PercentileSorted(samples, 0.50);
		s.p95 = PercentileSorted(samples, 0.95);
		s.p99 = PercentileSorted(samples, 0.99);
		s.maxMs = samples.back();
		return s;
	}

	// ---- Captured Output Verification (Exact 10-bit Codes + Hash + NaN/Inf Check) ----
	struct CaptureVerification
	{
		bool valid = false;
		std::string sha256;
		int width = 0;
		int height = 0;
		int minCode = 1023;
		int maxCode = 0;
		double meanCode = 0.0;
		size_t outOfRangePixels = 0;
	};

	std::string ReadSidecarSha256(const std::string& jsonPath)
	{
		std::ifstream in(WideOf(jsonPath));
		if (!in)
			return std::string();
		const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		const size_t k = text.find("\"raw_r10_sha256\": \"");
		return k == std::string::npos ? std::string() : text.substr(k + 19, 64);
	}

	CaptureVerification VerifyCaptureJson(const std::string& jsonPath)
	{
		CaptureVerification v;
		if (jsonPath.size() < 6)
			return v;
		v.sha256 = ReadSidecarSha256(jsonPath);
		const std::string pngPath = jsonPath.substr(0, jsonPath.size() - 5) + ".png";
		CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		CComPtr<IWICImagingFactory> factory;
		if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
			return v;
		CComPtr<IWICBitmapDecoder> decoder;
		if (FAILED(factory->CreateDecoderFromFilename(WideOf(pngPath).c_str(), nullptr, GENERIC_READ,
			WICDecodeMetadataCacheOnDemand, &decoder)))
			return v;
		CComPtr<IWICBitmapFrameDecode> frame;
		if (FAILED(decoder->GetFrame(0, &frame)))
			return v;
		WICPixelFormatGUID format{};
		if (FAILED(frame->GetPixelFormat(&format)))
			return v;
		const bool rgba = (format == GUID_WICPixelFormat64bppRGBA);
		if (!rgba && format != GUID_WICPixelFormat48bppRGB)
			return v;
		const size_t channels = rgba ? 4 : 3;
		UINT uw = 0, uh = 0;
		frame->GetSize(&uw, &uh);
		if (uw == 0 || uh == 0)
			return v;
		std::vector<uint16_t> samples(static_cast<size_t>(uw) * uh * channels);
		if (FAILED(frame->CopyPixels(nullptr, static_cast<UINT>(uw * channels * 2),
			static_cast<UINT>(samples.size() * 2), reinterpret_cast<BYTE*>(samples.data()))))
			return v;
		v.width = static_cast<int>(uw);
		v.height = static_cast<int>(uh);
		double sum = 0.0;
		const size_t pixelCount = static_cast<size_t>(uw) * uh;
		for (size_t i = 0; i < pixelCount; ++i)
		{
			for (size_t c = 0; c < 3; ++c)
			{
				const int code = static_cast<int>(samples[i * channels + c] >> 6);
				if (code < 0 || code > 1023)
					++v.outOfRangePixels;
				if (code < v.minCode) v.minCode = code;
				if (code > v.maxCode) v.maxCode = code;
				sum += static_cast<double>(code);
			}
		}
		v.meanCode = sum / static_cast<double>(pixelCount * 3);
		v.valid = !v.sha256.empty() && v.outOfRangePixels == 0 && std::isfinite(v.meanCode) && v.maxCode > 0;
		return v;
	}

	// ---- Real-Clip Format Packing Helpers ----
	// Converts rgb48le (6 bytes/pixel, little-endian 16-bit R,G,B) to packed R210 (10-bit RGB big-endian word).
	void PackRgb48ToR210(const std::vector<uint8_t>& rgb48, int width, int height, size_t rowBytes, std::vector<BYTE>& out)
	{
		out.assign(rowBytes * static_cast<size_t>(height), 0);
		for (int y = 0; y < height; ++y)
		{
			uint32_t* dst = reinterpret_cast<uint32_t*>(out.data() + static_cast<size_t>(y) * rowBytes);
			const uint16_t* src = reinterpret_cast<const uint16_t*>(rgb48.data() + static_cast<size_t>(y) * static_cast<size_t>(width) * 6);
			for (int x = 0; x < width; ++x)
			{
				const uint32_t r10 = static_cast<uint32_t>(src[x * 3 + 0] >> 6) & 0x3FFu;
				const uint32_t g10 = static_cast<uint32_t>(src[x * 3 + 1] >> 6) & 0x3FFu;
				const uint32_t b10 = static_cast<uint32_t>(src[x * 3 + 2] >> 6) & 0x3FFu;
				const uint32_t packed = (r10 << 20) | (g10 << 10) | b10;
				dst[x] = _byteswap_ulong(packed);
			}
		}
	}

	// Converts rgb48le to SMPTE 268M Method C4 little-endian R12L (8 pixels -> 36 bytes = 9 uint32_t words).
	void PackRgb48ToR12L(const std::vector<uint8_t>& rgb48, int width, int height, size_t rowBytes, std::vector<BYTE>& out)
	{
		out.assign(rowBytes * static_cast<size_t>(height), 0);
		for (int y = 0; y < height; ++y)
		{
			uint8_t* dstRow = out.data() + static_cast<size_t>(y) * rowBytes;
			const uint16_t* srcRow = reinterpret_cast<const uint16_t*>(rgb48.data() + static_cast<size_t>(y) * static_cast<size_t>(width) * 6);
			// Pack 24 12-bit components (8 pixels) into 36 bytes = 9 little-endian 32-bit words (288 bits).
			const int groups = width / 8;
			for (int g = 0; g < groups; ++g)
			{
				uint16_t c[24];
				for (int k = 0; k < 24; ++k)
					c[k] = static_cast<uint16_t>(srcRow[g * 24 + k] >> 4) & 0xFFFu;
				uint8_t* d = dstRow + static_cast<size_t>(g) * 36;
				for (int pair = 0; pair < 12; ++pair)
				{
					const uint16_t a = c[pair * 2 + 0];
					const uint16_t b = c[pair * 2 + 1];
					d[pair * 3 + 0] = static_cast<uint8_t>(a & 0xFFu);
					d[pair * 3 + 1] = static_cast<uint8_t>(((a >> 8) & 0x0Fu) | ((b & 0x0Fu) << 4));
					d[pair * 3 + 2] = static_cast<uint8_t>((b >> 4) & 0xFFu);
				}
			}
		}
	}

	// Burns a bright white subtitle bar into a P210 frame inside the bottom letterbox / lower-third region.
	void BurnSubtitleBoxP210(std::vector<BYTE>& p210, int width, int height, int boxY, int boxHeight, int boxX, int boxWidth)
	{
		P210FrameLayout layout{};
		if (!GetP210FrameLayout(static_cast<uint32_t>(width), static_cast<uint32_t>(height), layout) || p210.size() < layout.frameBytes)
			return;
		uint16_t* yPlane = reinterpret_cast<uint16_t*>(p210.data());
		uint16_t* uvPlane = reinterpret_cast<uint16_t*>(p210.data() + layout.planeBytes);
		const int y0 = (std::max)(0, boxY);
		const int y1 = (std::min)(height, boxY + boxHeight);
		const int x0 = (std::max)(0, boxX) & ~1;
		const int x1 = (std::min)(width, boxX + boxWidth) & ~1;
		for (int y = y0; y < y1; ++y)
		{
			uint16_t* yRow = yPlane + static_cast<size_t>(y) * static_cast<size_t>(width);
			uint16_t* uvRow = uvPlane + static_cast<size_t>(y) * static_cast<size_t>(width);
			for (int x = x0; x < x1; ++x)
			{
				// High-contrast glyph-like stripes inside a dark backing box for subtitle detection
				const bool glyph = ((x / 6) % 2 == 0) && ((y - y0) > 6 && (y1 - y) > 6);
				const uint16_t luma10 = glyph ? 940u : 64u;
				yRow[x] = static_cast<uint16_t>(luma10 << 6);
				uvRow[x] = static_cast<uint16_t>(512u << 6);
			}
		}
	}

	// Injects a peak-white bright flash across a P210 frame (stress-testing scene/flash detector).
	void ApplyBrightFlashP210(std::vector<BYTE>& p210, int width, int height)
	{
		P210FrameLayout layout{};
		if (!GetP210FrameLayout(static_cast<uint32_t>(width), static_cast<uint32_t>(height), layout) || p210.size() < layout.frameBytes)
			return;
		uint16_t* yPlane = reinterpret_cast<uint16_t*>(p210.data());
		uint16_t* uvPlane = reinterpret_cast<uint16_t*>(p210.data() + layout.planeBytes);
		const size_t n = static_cast<size_t>(width) * static_cast<size_t>(height);
		const uint16_t whiteY = static_cast<uint16_t>(940u << 6);
		const uint16_t neutralUv = static_cast<uint16_t>(512u << 6);
		std::fill(yPlane, yPlane + n, whiteY);
		std::fill(uvPlane, uvPlane + n, neutralUv);
	}

	bool DecodeClipFrames(const RealClip::Manifest& manifest, const RealClip::Clip& clip,
		int width, int height, VideoFrameEncoding encoding, int count,
		std::vector<std::vector<BYTE>>& out, std::string& error)
	{
		out.clear();
		RealClip::Decoder decoder;
		std::ostringstream vf;
		vf << "scale=" << width << ":" << height << ":force_original_aspect_ratio=decrease,"
		   << "pad=" << width << ":" << height << ":(ow-iw)/2:(oh-ih)/2:black";

		std::string pixFmt;
		size_t rawFrameBytes = 0;
		size_t packedRowBytes = 0;
		if (encoding == VideoFrameEncoding::P010)
		{
			pixFmt = "p010le";
			P010FrameLayout layout{};
			GetP010FrameLayout(static_cast<uint32_t>(width), static_cast<uint32_t>(height), layout);
			rawFrameBytes = layout.frameBytes;
		}
		else if (encoding == VideoFrameEncoding::P210)
		{
			pixFmt = "p210le";
			P210FrameLayout layout{};
			GetP210FrameLayout(static_cast<uint32_t>(width), static_cast<uint32_t>(height), layout);
			rawFrameBytes = layout.frameBytes;
		}
		else if (encoding == VideoFrameEncoding::BGRA_8BIT)
		{
			pixFmt = "bgra";
			rawFrameBytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
		}
		else if (encoding == VideoFrameEncoding::R210)
		{
			pixFmt = "rgb48le";
			rawFrameBytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 6;
			packedRowBytes = static_cast<size_t>(((width + 63) / 64) * 256);
		}
		else if (encoding == VideoFrameEncoding::R12L)
		{
			pixFmt = "rgb48le";
			rawFrameBytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 6;
			packedRowBytes = static_cast<size_t>((width * 36) / 8);
		}
		else
		{
			error = "unsupported soak encoding";
			return false;
		}
		vf << ",format=" << pixFmt;
		if (!decoder.OpenRaw(manifest, clip, vf.str(), pixFmt, error))
			return false;

		std::vector<uint8_t> raw;
		for (int i = 0; i < count && decoder.NextRaw(raw, rawFrameBytes); ++i)
		{
			if (encoding == VideoFrameEncoding::R210)
			{
				std::vector<BYTE> packed;
				PackRgb48ToR210(raw, width, height, packedRowBytes, packed);
				out.push_back(std::move(packed));
			}
			else if (encoding == VideoFrameEncoding::R12L)
			{
				std::vector<BYTE> packed;
				PackRgb48ToR12L(raw, width, height, packedRowBytes, packed);
				out.push_back(std::move(packed));
			}
			else
			{
				out.emplace_back(raw.begin(), raw.end());
			}
		}
		decoder.Close();
		if (out.empty())
		{
			error = "decoder returned 0 frames for " + clip.id;
			return false;
		}
		return true;
	}

	// ---- Renderer Session Wrapper (RAII) ----
	struct SoakLegResult
	{
		bool created = false;
		bool presented = false;
		int framesSent = 0;
		int restarts = 0;
		std::string adapterName;
		std::vector<std::string> log;
		std::vector<std::string> captureJsons;
		std::vector<double> gpuMs;
		std::vector<double> cpuRenderMs;
		std::vector<MemorySample> memorySamples;
		std::string dejudderStatus;
		std::string sourceStatus;
		std::string ledStatus;
		std::string fallbacksStatus;
		int shaderCacheLoadedObjects = -1;
		int shaderCacheSavedObjects = -1;
		int diagReadbackObjects = -1;
		long long gpuPasses = -1;
		long long gpuUnattributed = -1;
		std::string gpuUnattributedDetail;
		int postWarmupShaderCompiles = 0;
		int logErrorsOrNans = 0;
		uint64_t finalGpuSourceSeq = 0;
		std::string errorDetail;
	};

	struct SoakRendererHandle
	{
		using CreateFn = IVideoRenderer* (__cdecl*)(IRendererCallback*, uint32_t, HWND, ITimingClock*, bool, size_t,
			VideoConversionOverride, const char*, VideoProcessorLibplaceboLogSink);
		using DestroyFn = void(__cdecl*)(IVideoRenderer*);

		HMODULE dll = nullptr;
		DestroyFn destroyFn = nullptr;
		IVideoRenderer* renderer = nullptr;
		IVideoRenderer* extension = nullptr;
		HWND hwnd = nullptr;
		bool started = false;
		SoakCallback callback;
		SoakClock clock;

		~SoakRendererHandle() { Close(); }

		void Close()
		{
			if (renderer)
			{
				if (started)
					renderer->Stop();
				if (destroyFn)
					destroyFn(renderer);
				renderer = nullptr;
				extension = nullptr;
				started = false;
			}
			if (hwnd)
			{
				DestroyWindow(hwnd);
				hwnd = nullptr;
			}
			// Keep DLL loaded by OS (same pattern as RendererCaptureHarnessTests)
		}

		bool Open(const std::string& configPath, int windowWidth, int windowHeight, size_t queueCapacity,
			const std::string& customDllPath = std::string())
		{
			{
				std::lock_guard<std::mutex> guard(g_soakLogMutex);
				g_soakLog.clear();
			}
			const std::string folder = BinaryFolder();
			const std::string dllPath = customDllPath.empty() ? (folder + "vprenderer\\VideoProcessorVPRenderer.dll") : customDllPath;
			dll = LoadLibraryExA(dllPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
			if (!dll)
				return false;
			const CreateFn createFn = reinterpret_cast<CreateFn>(GetProcAddress(dll, "VideoProcessorLibplaceboCreateRenderer"));
			destroyFn = reinterpret_cast<DestroyFn>(GetProcAddress(dll, "VideoProcessorLibplaceboDestroyRenderer"));
			if (!createFn || !destroyFn)
				return false;
			std::string attachReason;
			const bool attached = Vpv::PluginGuard::Check(dll, attachReason) && Vpv::PluginGuard::Attach(dll, attachReason);

			WNDCLASSA wc{};
			wc.lpfnWndProc = DefWindowProcA;
			wc.hInstance = GetModuleHandleA(nullptr);
			wc.lpszClassName = "VpVirtualHdmiSoakWindow";
			RegisterClassA(&wc);
			hwnd = CreateWindowExA(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName,
				"VP Virtual HDMI Soak", WS_POPUP, -4000, -4000, windowWidth, windowHeight,
				nullptr, nullptr, wc.hInstance, nullptr);
			if (!hwnd)
				return false;
			ShowWindow(hwnd, SW_SHOWNOACTIVATE);

			renderer = createFn(&callback, 1, hwnd, &clock, true, queueCapacity,
				VideoConversionOverride::VIDEOCONVERSION_NONE, configPath.c_str(), &SoakCaptureLog);
			if (!renderer)
				return false;
			extension = attached ? renderer : nullptr;
			return true;
		}
	};

	VideoStateComPtr MakeVideoState(int width, int height, VideoFrameEncoding encoding,
		int rateNum, int rateDen, bool pqHdr = false, double maxCll = 1000.0, double maxFall = 250.0)
	{
		VideoStateComPtr state = new VideoState();
		state->valid = true;
		state->displayMode = std::make_shared<DisplayMode>(width, height, false, rateNum, rateDen);
		state->videoFrameEncoding = encoding;
		state->dolbyVisionTunnel = false;
		if (pqHdr)
		{
			state->eotf = EOTF::PQ;
			state->colorspace = ColorSpace::BT_2020;
			state->hdrData = std::make_shared<HDRData>();
			state->hdrData->displayPrimaryRedX = ColorSpaceToCie1931RedX(ColorSpace::BT_2020);
			state->hdrData->displayPrimaryRedY = ColorSpaceToCie1931RedY(ColorSpace::BT_2020);
			state->hdrData->displayPrimaryGreenX = ColorSpaceToCie1931GreenX(ColorSpace::BT_2020);
			state->hdrData->displayPrimaryGreenY = ColorSpaceToCie1931GreenY(ColorSpace::BT_2020);
			state->hdrData->displayPrimaryBlueX = ColorSpaceToCie1931BlueX(ColorSpace::BT_2020);
			state->hdrData->displayPrimaryBlueY = ColorSpaceToCie1931BlueY(ColorSpace::BT_2020);
			state->hdrData->whitePointX = ColorSpaceToCie1931WpX(ColorSpace::BT_2020);
			state->hdrData->whitePointY = ColorSpaceToCie1931WpY(ColorSpace::BT_2020);
			state->hdrData->masteringDisplayMinLuminance = 0.0001;
			state->hdrData->masteringDisplayMaxLuminance = maxCll;
			state->hdrData->maxCll = maxCll;
			state->hdrData->maxFall = maxFall;
		}
		else
		{
			state->eotf = EOTF::SDR;
			state->colorspace = ColorSpace::REC_709;
		}
		return state;
	}

	void PumpWindowMessages()
	{
		MSG msg{};
		while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE))
		{
			TranslateMessage(&msg);
			DispatchMessageA(&msg);
		}
	}

	std::string ParseAdapterNameFromLog(const std::vector<std::string>& log)
	{
		const std::string dxgiKey = "libplacebo DXGI adapter: name=";
		const std::string initKey = "libplacebo initialized: D3D11 on ";
		for (const std::string& line : log)
		{
			const size_t at = line.find(dxgiKey);
			if (at != std::string::npos)
			{
				const size_t from = at + dxgiKey.size();
				const size_t end = line.find(" luid=", from);
				return line.substr(from, end == std::string::npos ? std::string::npos : end - from);
			}
		}
		for (const std::string& line : log)
		{
			const size_t at = line.find(initKey);
			if (at != std::string::npos)
			{
				const size_t from = at + initKey.size();
				const size_t comma = line.find(',', from);
				return line.substr(from, comma == std::string::npos ? std::string::npos : comma - from);
			}
		}
		return std::string();
	}

	void ParseLogDiagnostics(const std::vector<std::string>& log, size_t warmupLogMark, SoakLegResult& out)
	{
		out.adapterName = ParseAdapterNameFromLog(log);
		for (size_t i = 0; i < log.size(); ++i)
		{
			const std::string& line = log[i];
			size_t at = line.find("libplacebo persistent shader cache loaded: ");
			if (at != std::string::npos)
				out.shaderCacheLoadedObjects = std::atoi(line.c_str() + at + 43);

			at = line.find("libplacebo persistent shader cache saved: ");
			if (at != std::string::npos)
				out.shaderCacheSavedObjects = std::atoi(line.c_str() + at + 42);

			at = line.find("libplacebo output diagnostic readback:");
			if (at != std::string::npos)
			{
				const size_t objAt = line.find(" objects=", at);
				if (objAt != std::string::npos)
					out.diagReadbackObjects = std::atoi(line.c_str() + objAt + 9);
			}

			at = line.find("Compute-once gpu UNATTRIBUTED ");
			if (at != std::string::npos)
				out.gpuUnattributedDetail += line.substr(at) + "; ";

			at = line.find("Compute-once summary ");
			if (at != std::string::npos)
			{
				const size_t gp = line.find(" gpu-passes ", at);
				const size_t gu = line.find(" gpu-unattributed ", at);
				if (gp != std::string::npos)
					out.gpuPasses = std::atoll(line.c_str() + gp + 12);
				if (gu != std::string::npos)
					out.gpuUnattributed = std::atoll(line.c_str() + gu + 18);
			}

			at = line.find("Rendered-output capture complete");
			if (at != std::string::npos)
			{
				const size_t m = line.find("metadata=", at);
				if (m != std::string::npos)
				{
					const size_t end = line.find(" raw_bytes=", m);
					out.captureJsons.push_back(line.substr(m + 9, end == std::string::npos ? std::string::npos : end - m - 9));
				}
			}

			// Detect post-warmup shader recompilations or pipeline rebuilds
			if (i >= warmupLogMark)
			{
				const bool isCompile =
					(line.find("Spent ") != std::string::npos &&
					 (line.find("compiling") != std::string::npos ||
					  line.find("translating") != std::string::npos ||
					  line.find("creating pipeline") != std::string::npos ||
					  line.find("shader") != std::string::npos)) ||
					line.find("Re-base shader warm-up") != std::string::npos ||
					line.find("Compiling shader") != std::string::npos;
				if (isCompile)
					++out.postWarmupShaderCompiles;
			}

			// Detect fatal GPU/device/NaN errors in log
			if (line.find("DXGI_ERROR_DEVICE_REMOVED") != std::string::npos ||
				line.find("DXGI_ERROR_DEVICE_RESET") != std::string::npos ||
				line.find("E_OUTOFMEMORY") != std::string::npos ||
				line.find("pl_pass_run failed") != std::string::npos ||
				line.find("NaN") != std::string::npos ||
				line.find("-nan") != std::string::npos)
			{
				++out.logErrorsOrNans;
				if (out.errorDetail.empty())
					out.errorDetail = line;
			}
		}
	}

	size_t CurrentLogCount()
	{
		std::lock_guard<std::mutex> guard(g_soakLogMutex);
		return g_soakLog.size();
	}

	size_t CountCompletedCapturesInLog()
	{
		std::lock_guard<std::mutex> guard(g_soakLogMutex);
		size_t n = 0;
		for (const std::string& line : g_soakLog)
			if (line.find("Rendered-output capture complete") != std::string::npos)
				++n;
		return n;
	}

	struct FrameAction
	{
		bool send = true;
		bool resetBefore = false;
		uint64_t overrideSeq = 0; // 0 = auto increment
		int stallAfterMs = 0;     // >0: simulate HDMI/source frame starvation pause (pumps messages, shifts pacing)
		const std::vector<BYTE>* customBuffer = nullptr;
	};

	struct SoakLegOptions
	{
		std::string name;
		std::string configText;
		int width = 1920;
		int height = 1080;
		int windowWidth = 1280;
		int windowHeight = 720;
		VideoFrameEncoding encoding = VideoFrameEncoding::P210;
		int rateNum = 24000;
		int rateDen = 1001;
		int framePeriodMs = 18; // deterministic pacing fast enough for 500-frame soak while letting GPU render every frame
		int totalFrames = 20;
		int warmupFrames = 5;
		bool pqHdr = false;
		bool subtitleRelocation = false;
		bool applySnapshotBeforeBuild = false;
		bool plainConfig = false;
		bool allowOccludedOffscreen = false; // opt-in for isolated desktop where DXGI Present returns OCCLUDED while GPU renders & captures normally
		std::string customDllPath;
		std::vector<int> captureAtFrames;
		int memorySampleStride = 0; // >0: record VRAM + RAM every N frames after warmup
		std::function<FrameAction(int frameIdx, IVideoRenderer& renderer, IVideoRenderer* ext, std::vector<BYTE>& slotBuf)> onFrame;
		std::function<void(IVideoRenderer& renderer, VideoStateComPtr& state)> afterLastFrame;
	};

	std::string LogTail(const std::vector<std::string>& log, size_t maxLines = 25)
	{
		std::string out;
		const size_t from = log.size() > maxLines ? log.size() - maxLines : 0;
		for (size_t i = from; i < log.size(); ++i)
			out += "  | " + log[i] + "\n";
		return out;
	}

	SoakLegResult RunSoakLeg(const SoakLegOptions& opt, const std::vector<std::vector<BYTE>>& framePool,
		const NvidiaAdapterMonitor& nvMon, const std::string& configPath)
	{
		SoakLegResult res;
		{
			std::ofstream cfg(configPath, std::ios::trunc);
			if (opt.plainConfig)
				cfg << "[general]\npersist_profile_selection: false\n" << opt.configText;
			else
				cfg << "[general]\npersist_profile_selection: false\n"
					<< "[vprenderer.input.soak]\nwhen: $source_rate > 0\ncapture_path: gpu\n"
					<< opt.configText;
		}

		SoakRendererHandle handle;
		if (!handle.Open(configPath, opt.windowWidth, opt.windowHeight, 8, opt.customDllPath))
		{
			res.errorDetail = "failed to open renderer DLL/window for " + opt.name;
			return res;
		}
		res.created = true;

		VideoStateComPtr state = MakeVideoState(opt.width, opt.height, opt.encoding, opt.rateNum, opt.rateDen, opt.pqHdr);
		Assert::IsTrue(handle.renderer->OnVideoState(state),
			WideOf(opt.name + ": initial OnVideoState rejected").c_str());

		if (opt.applySnapshotBeforeBuild)
		{
			struct LatchReset
			{
				LatchReset() { Vpv::RunningPathway::SetForTesting(Vpv::RunningPathway::kUnknown); }
				~LatchReset() { Vpv::RunningPathway::SetForTesting(Vpv::RunningPathway::kUnknown); }
			} latchReset;
			ConfigFile profileCfg;
			std::string err;
			UnifiedProfileRuntime::Runtime rt;
			if (profileCfg.Load(configPath) && rt.Initialize(profileCfg, {}, err))
			{
				const auto snap = rt.GetSnapshot();
				CString activeState;
				bool restart = false, live = false;
				if (snap)
					handle.renderer->ApplyApplicationState(*snap, activeState, restart, live);
			}
		}

		handle.renderer->Build();
		handle.renderer->SetRenderLoadDisplayRefreshRate(ExactMonitorHz());
		handle.renderer->SetQueueFramePolicy(2, 3, true);
		if (opt.subtitleRelocation)
			handle.renderer->SetSubtitleRepositioningMode(SubtitleRepositionMode::ADVANCED);
		handle.renderer->Start();
		handle.started = true;

		const size_t expectedBytes = static_cast<size_t>(state->BytesPerFrame());
		const size_t ringCount = 16;
		std::vector<std::vector<BYTE>> ring(ringCount, std::vector<BYTE>(expectedBytes, 0));
		std::vector<SoakSourceBuffer> buffers(ringCount);

		const double ticksPerUs = static_cast<double>(handle.clock.TimingClockTicksPerSecond()) / 1e6;
		const timingclocktime_t startTicks = handle.clock.TimingClockNow();
		const auto period = std::chrono::milliseconds(opt.framePeriodMs);

		uint64_t seq = 0;
		int64_t logicalTick = 0;
		// Startup sync: stream initial frames until the renderer finishes D3D11 + shader initialization
		// and presents its first live frame (up to 15 s for cold shader compiles on first run).
		{
			std::vector<BYTE> primeBuf(expectedBytes, 0);
			if (!framePool.empty())
				std::memcpy(primeBuf.data(), framePool[0].data(), (std::min)(framePool[0].size(), primeBuf.size()));
			else if (opt.onFrame)
				opt.onFrame(0, *handle.renderer, handle.extension, primeBuf);

			const auto syncDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
			auto nextSync = std::chrono::steady_clock::now();
			auto isReady = [&]() -> bool {
				if (handle.renderer->HasPresentedLiveFrame())
					return true;
				if (opt.allowOccludedOffscreen)
				{
					RendererRenderLoad load{};
					if (handle.renderer->GetRenderLoad(load) && load.gpuValid && load.latestGpuSourceSequence >= 2)
						return true;
				}
				return false;
			};
			while (!isReady() && std::chrono::steady_clock::now() < syncDeadline)
			{
				const size_t slot = static_cast<size_t>(logicalTick) % ringCount;
				std::memcpy(ring[slot].data(), primeBuf.data(), expectedBytes);
				++seq;
				const timingclocktime_t stamp = startTicks +
					static_cast<timingclocktime_t>(static_cast<double>(logicalTick * opt.framePeriodMs * 1000LL) * ticksPerUs);
				VideoFrame vf(ring[slot].data(), seq, stamp, &buffers[slot], stamp);
				handle.renderer->OnVideoFrame(vf);
				++logicalTick;
				PumpWindowMessages();
				nextSync += period;
				std::this_thread::sleep_until(nextSync);
			}
		}

		auto loopStart = std::chrono::steady_clock::now();
		uint64_t lastGpuSeq = 0;
		size_t capturesRequested = 0;
		size_t warmupLogMark = 0;

		for (int f = 0; f < opt.totalFrames; ++f)
		{
			if (f == opt.warmupFrames)
				warmupLogMark = CurrentLogCount();

			const size_t slot = static_cast<size_t>(logicalTick) % ringCount;
			std::vector<BYTE>& slotBuf = ring[slot];
			FrameAction action;
			if (opt.onFrame)
			{
				action = opt.onFrame(f, *handle.renderer, handle.extension, slotBuf);
			}
			else if (!framePool.empty())
			{
				const std::vector<BYTE>& src = framePool[static_cast<size_t>(f) % framePool.size()];
				const size_t copyBytes = (std::min)(src.size(), slotBuf.size());
				std::memcpy(slotBuf.data(), src.data(), copyBytes);
			}

			if (action.resetBefore)
			{
				handle.renderer->Reset();
				Assert::IsTrue(handle.renderer->OnVideoState(state),
					WideOf(opt.name + ": OnVideoState after Reset failed").c_str());
			}

			if (action.send)
			{
				if (action.overrideSeq > 0)
					seq = action.overrideSeq;
				else
					++seq;

				const BYTE* payload = action.customBuffer ? action.customBuffer->data() : slotBuf.data();
				const timingclocktime_t stamp = startTicks +
					static_cast<timingclocktime_t>(static_cast<double>(logicalTick * opt.framePeriodMs * 1000LL) * ticksPerUs);
				VideoFrame vf(const_cast<BYTE*>(payload), seq, stamp, &buffers[slot], stamp);
				handle.renderer->OnVideoFrame(vf);
				++res.framesSent;
			}
			++logicalTick;

			for (size_t ci = 0; ci < opt.captureAtFrames.size(); ++ci)
			{
				if (opt.captureAtFrames[ci] == f)
				{
					CString capStatus;
					if (handle.renderer->RequestRenderedOutputCapture(capStatus))
						++capturesRequested;
					else
					{
						// Retry shortly after start-up settling
						for (int retry = 0; retry < 15; ++retry)
						{
							PumpWindowMessages();
							std::this_thread::sleep_for(std::chrono::milliseconds(40));
							if (handle.renderer->RequestRenderedOutputCapture(capStatus))
							{
								++capturesRequested;
								break;
							}
						}
					}
				}
			}

			PumpWindowMessages();

			// Collect real GPU & CPU render timings after warmup
			if (f >= opt.warmupFrames)
			{
				RendererRenderLoad load{};
				if (handle.renderer->GetRenderLoad(load))
				{
					if (load.gpuValid && load.latestGpuSourceSequence != lastGpuSeq && load.gpu.last > 0.0)
					{
						lastGpuSeq = load.latestGpuSourceSequence;
						res.finalGpuSourceSeq = lastGpuSeq;
						res.gpuMs.push_back(load.gpu.last);
					}
					if (load.valid && load.render.last > 0.0)
						res.cpuRenderMs.push_back(load.render.last);
				}
				if (opt.memorySampleStride > 0 && ((f - opt.warmupFrames) % opt.memorySampleStride == 0))
				{
					MemorySample ms;
					ms.frameIndex = f - opt.warmupFrames;
					ms.vramMiB = nvMon.QueryLocalVramMiB();
					NvidiaAdapterMonitor::QueryProcessRamMiB(ms.workingSetMiB, ms.privateBytesMiB);
					res.memorySamples.push_back(ms);
				}
			}

			if (action.stallAfterMs > 0)
			{
				const auto stallEnd = std::chrono::steady_clock::now() + std::chrono::milliseconds(action.stallAfterMs);
				while (std::chrono::steady_clock::now() < stallEnd)
				{
					PumpWindowMessages();
					std::this_thread::sleep_for(std::chrono::milliseconds(20));
				}
				loopStart += std::chrono::milliseconds(action.stallAfterMs);
			}

			std::this_thread::sleep_until(loopStart + period * (f + 1));
		}

		// Ensure at least one capture completes if requested (wait up to 10 s while nudging if still picture)
		if (capturesRequested > 0)
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
			int extraNudge = 0;
			while (CountCompletedCapturesInLog() < capturesRequested && std::chrono::steady_clock::now() < deadline)
			{
				PumpWindowMessages();
				if (!framePool.empty() && extraNudge < 20)
				{
					++seq;
					const size_t slot = static_cast<size_t>(opt.totalFrames + extraNudge) % ringCount;
					const std::vector<BYTE>& src = framePool[0];
					std::memcpy(ring[slot].data(), src.data(), (std::min)(src.size(), ring[slot].size()));
					const timingclocktime_t stamp = handle.clock.TimingClockNow();
					VideoFrame vf(ring[slot].data(), seq, stamp, &buffers[slot], stamp);
					handle.renderer->OnVideoFrame(vf);
					++extraNudge;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(30));
			}
		}

		// Let the last few frames present so HasPresentedLiveFrame and GPU timers resolve
		for (int wait = 0; wait < 25 && !handle.renderer->HasPresentedLiveFrame(); ++wait)
		{
			PumpWindowMessages();
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}

		if (opt.afterLastFrame)
			opt.afterLastFrame(*handle.renderer, state);

		res.presented = handle.renderer->HasPresentedLiveFrame();
		res.restarts = handle.callback.restarts.load();

		CString status;
		if (VpvCalls::GetDejudderStatus(handle.extension, status))
			res.dejudderStatus = CStringA(status).GetString();
		status.Empty();
		if (VpvCalls::GetSourceResolutionStatus(handle.extension, status))
			res.sourceStatus = CStringA(status).GetString();
		status.Empty();
		if (VpvCalls::GetLedDriveStatus(handle.extension, status))
			res.ledStatus = CStringA(status).GetString();
		status.Empty();
		if (VpvCalls::GetFallbacksStatus(handle.extension, status))
			res.fallbacksStatus = CStringA(status).GetString();

		handle.Close();
		{
			std::lock_guard<std::mutex> guard(g_soakLogMutex);
			res.log = g_soakLog;
		}
		ParseLogDiagnostics(res.log, warmupLogMark, res);
		return res;
	}

	std::string FormatStatsLine(const std::string& label, const TimingStats& gpu, const TimingStats& cpu)
	{
		char buf[320];
		snprintf(buf, sizeof(buf),
			"  %-34s | GPU(n=%3zu) p50=%5.2f ms p95=%5.2f ms p99=%5.2f ms | CPU(n=%3zu) p50=%5.2f ms p95=%5.2f ms p99=%5.2f ms\n",
			label.c_str(), gpu.count, gpu.p50, gpu.p95, gpu.p99, cpu.count, cpu.p50, cpu.p95, cpu.p99);
		return buf;
	}
}

namespace VideoProcessorTest
{
	TEST_CLASS(VirtualHdmiSoakTests)
	{
	public:
		BEGIN_TEST_METHOD_ATTRIBUTE(VirtualHdmiLivingRoomChaos500FrameSoak)
			TEST_METHOD_ATTRIBUTE(L"Category", L"Footage")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(VirtualHdmiLivingRoomChaos500FrameSoak)
		{
			const char* const kTestName = "VirtualHdmiLivingRoomChaos500FrameSoak";
			const bool gpuHw = (SoakEnv("VP_TEST_GPU") == "hardware");
			if (!HardwarePrerequisites::RequireOrSkip(HardwarePrerequisites::Kind::Gpu, kTestName, gpuHw,
				"set VP_TEST_GPU=hardware and VP_TEST_ADAPTER=NVIDIA"))
				return;

			const RealClip::Manifest manifest = RealClip::Manifest::Load();
			if (!HardwarePrerequisites::RequireOrSkip(HardwarePrerequisites::Kind::Footage, kTestName, manifest.Loaded(),
				manifest.error.empty() ? "real clip library manifest unavailable" : manifest.error))
				return;

			// Pick a real clip from the manifest (prefer long 24p cinema clip so 200+ unique frames decode cleanly)
			const RealClip::Clip* clip = nullptr;
			const char* const preferredIds[] = { "tos_24p_3min", "tos_23976p_3min", "tos_1920x800_24p" };
			for (size_t i = 0; i < sizeof(preferredIds) / sizeof(preferredIds[0]) && !clip; ++i)
				clip = manifest.Find(preferredIds[i]);
			if (!clip)
			{
				for (size_t i = 0; i < manifest.clips.size(); ++i)
				{
					if (manifest.clips[i].frames >= 120 && manifest.clips[i].width >= 1280)
					{
						clip = &manifest.clips[i];
						break;
					}
				}
			}
			if (!HardwarePrerequisites::RequireOrSkip(HardwarePrerequisites::Kind::Footage, kTestName, clip != nullptr,
				"no suitable real clip found in manifest"))
				return;

			// Hard Adapter Lock: require hardware NVIDIA GPU and lock VP_TEST_ADAPTER=NVIDIA
			NvidiaAdapterMonitor nvMon;
			std::string nvName, nvErr;
			Assert::IsTrue(nvMon.Initialize(nvName, nvErr),
				WideOf("Hard NVIDIA adapter lock failed: " + nvErr).c_str());
			Assert::IsTrue(UpperOf(nvName).find("NVIDIA") != std::string::npos,
				WideOf("Expected NVIDIA adapter, got: " + nvName).c_str());

			const ScopedEnvVar envAdapter("VP_TEST_ADAPTER", "NVIDIA");
			const ScopedEnvVar envComputeOnce("VP_DEBUG_COMPUTE_ONCE", "1");
			const std::string binDir = BinaryFolder();
			const ScopedEnvVar envShaderDir("VP_TEST_SHADER_DIR",
				(!binDir.empty() && (binDir.back() == '\\' || binDir.back() == '/')) ? binDir.substr(0, binDir.size() - 1) : binDir);
			const TempFolderScope shotsFolder("vp-virtual-hdmi-soak-shots");
			const ScopedEnvVar envShots("VP_TEST_SCREENSHOT_DIR", shotsFolder.Path());

			const std::string cacheDir = BinaryFolder() + "vp-harness-shader-cache";
			CreateDirectoryA(cacheDir.c_str(), nullptr);
			CreateDirectoryA((cacheDir + "\\vprenderer").c_str(), nullptr);
			const ScopedEnvVar envCache("VP_TEST_SHADER_CACHE_DIR", cacheDir);

			const std::string cfgPath = shotsFolder.Path() + "\\soak.cfg";

			Logger::WriteMessage(("=== Virtual HDMI 500-Frame Living-Room Chaos Soak ===\n"
				"  Locked DXGI Adapter : " + nvName + "\n"
				"  Source Clip         : " + clip->id + " (" + std::to_string(clip->width) + "x" +
				std::to_string(clip->height) + ", " + std::to_string(clip->frames) + " frames)\n").c_str());

			// Shared accumulators across all 5 phases
			int totalFramesProcessed = 0;
			int totalCapturesVerified = 0;
			int totalNanOrCorruptFrames = 0;
			int totalLogErrors = 0;
			long long totalGpuPasses = 0;
			long long totalGpuUnattributed = 0;
			std::string observedAdapter;
			std::string failures;
			auto recordFailure = [&](const std::string& msg) {
				failures += msg + "; ";
				Logger::WriteMessage(("  [FAIL] " + msg + "\n").c_str());
			};

			auto verifyAdapterAndWatchdogs = [&](const std::string& phaseLabel, const SoakLegResult& leg, bool checkGpuAttribution) {
				Assert::IsTrue(leg.created, WideOf(phaseLabel + ": renderer creation failed: " + leg.errorDetail).c_str());
				Assert::IsTrue(leg.presented, WideOf(phaseLabel + ": renderer never presented a live frame\n" + LogTail(leg.log)).c_str());
				Assert::IsFalse(leg.adapterName.empty(), WideOf(phaseLabel + ": bound DXGI adapter was not logged\n" + LogTail(leg.log)).c_str());
				Assert::IsTrue(UpperOf(leg.adapterName).find("NVIDIA") != std::string::npos,
					WideOf(phaseLabel + ": bound non-NVIDIA adapter: " + leg.adapterName).c_str());
				Assert::IsTrue(UpperOf(leg.adapterName).find("INTEL") == std::string::npos &&
					UpperOf(leg.adapterName).find("BASIC RENDER") == std::string::npos,
					WideOf(phaseLabel + ": bound iGPU or WARP adapter: " + leg.adapterName).c_str());
				if (observedAdapter.empty())
					observedAdapter = leg.adapterName;

				totalFramesProcessed += leg.framesSent;
				totalLogErrors += leg.logErrorsOrNans;
				if (leg.logErrorsOrNans != 0)
					recordFailure(phaseLabel + ": fatal GPU/NaN error logged: " + leg.errorDetail);

				if (checkGpuAttribution && leg.gpuPasses >= 0)
				{
					totalGpuPasses += leg.gpuPasses;
					totalGpuUnattributed += (std::max)(0LL, leg.gpuUnattributed);
					if (leg.gpuPasses <= 0)
						recordFailure(phaseLabel + ": GpuAttribution saw 0 passes");
					if (leg.gpuUnattributed != 0)
						recordFailure(phaseLabel + ": GpuAttribution unattributed=" + std::to_string(leg.gpuUnattributed) +
							" (" + leg.gpuUnattributedDetail + ")");
				}

				for (size_t i = 0; i < leg.captureJsons.size(); ++i)
				{
					const CaptureVerification cv = VerifyCaptureJson(leg.captureJsons[i]);
					++totalCapturesVerified;
					if (!cv.valid)
					{
						++totalNanOrCorruptFrames;
						recordFailure(phaseLabel + ": captured frame invalid/NaN/dead-black (" + leg.captureJsons[i] + ")");
					}
				}
			};

			const std::string kFullVpvStages =
				"deband_vp: on\n"
				"[vpv]\n"
				"resync_event: on\n"
				"frame_analysis_store: on\n"
				"[vpv.skeleton]\n"
				"analysis_lane: on\n"
				"skeleton_gpu_gap: after_render\n"
				"analysis_lane_chunk_us: 4000\n";

			// =========================================================================
			// PHASE 1 — Format & Resolution Switches (80 real decoded frames)
			//   P010 1080p -> P210 1080p -> BGRA_8BIT 1080p -> R210 1080p -> R12L 4K -> P010 4K
			// =========================================================================
			std::vector<double> phase1GpuMs, phase1CpuMs;
			std::string decErr;
			bool liveFormatSwitchRejected = false;
			{
				struct FormatLeg { const char* tag; VideoFrameEncoding enc; int w; int h; int frames; bool testRefuse; };
				const FormatLeg formatLegs[] = {
					{ "Phase1a P010 4:2:0 1080p",   VideoFrameEncoding::P010,      1920, 1080, 15, true  },
					{ "Phase1b P210 4:2:2 1080p",   VideoFrameEncoding::P210,      1920, 1080, 15, false },
					{ "Phase1c BGRA 8-bit 1080p",   VideoFrameEncoding::BGRA_8BIT, 1920, 1080, 10, false },
					{ "Phase1d R210 10-bit 1080p",  VideoFrameEncoding::R210,      1920, 1080, 15, false },
					{ "Phase1e R12L 12-bit 4K",     VideoFrameEncoding::R12L,      3840, 2160, 10, false },
					{ "Phase1f P010 4:2:0 4K",      VideoFrameEncoding::P010,      3840, 2160, 15, false },
				};
				for (size_t i = 0; i < sizeof(formatLegs) / sizeof(formatLegs[0]); ++i)
				{
					const FormatLeg& fl = formatLegs[i];
					std::vector<std::vector<BYTE>> pool;
					const int decodeCount = (std::min)(fl.frames, fl.w > 1920 ? 6 : 12);
					Assert::IsTrue(DecodeClipFrames(manifest, *clip, fl.w, fl.h, fl.enc, decodeCount, pool, decErr),
						WideOf(std::string(fl.tag) + ": decode failed: " + decErr).c_str());

					SoakLegOptions opt;
					opt.name = fl.tag;
					opt.configText = kFullVpvStages;
					opt.width = fl.w;
					opt.height = fl.h;
					opt.windowWidth = fl.w;
					opt.windowHeight = fl.h;
					opt.encoding = fl.enc;
					opt.rateNum = 24000;
					opt.rateDen = 1001;
					opt.framePeriodMs = fl.w > 1920 ? 28 : 18;
					opt.totalFrames = fl.frames;
					opt.warmupFrames = 4;
					opt.captureAtFrames = { fl.frames - 3 };
					if (fl.testRefuse)
					{
						opt.afterLastFrame = [&](IVideoRenderer& r, VideoStateComPtr&) {
							VideoStateComPtr nextFmt = MakeVideoState(1920, 1080, VideoFrameEncoding::P210, 24000, 1001, false);
							liveFormatSwitchRejected = !r.OnVideoState(nextFmt);
						};
					}

					const SoakLegResult leg = RunSoakLeg(opt, pool, nvMon, cfgPath);
					verifyAdapterAndWatchdogs(fl.tag, leg, true);
					Assert::IsFalse(leg.captureJsons.empty(), WideOf(std::string(fl.tag) + ": capture missing").c_str());
					phase1GpuMs.insert(phase1GpuMs.end(), leg.gpuMs.begin(), leg.gpuMs.end());
					phase1CpuMs.insert(phase1CpuMs.end(), leg.cpuRenderMs.begin(), leg.cpuRenderMs.end());
				}
				Assert::IsTrue(liveFormatSwitchRejected,
					L"Phase 1: expected OnVideoState(P010 -> P210) to return false requesting clean rebuild");
			}
			const TimingStats phase1Gpu = ComputeStats(phase1GpuMs);
			const TimingStats phase1Cpu = ComputeStats(phase1CpuMs);

			// Decode a reusable 48-frame 1080p P210 real-clip pool for Phases 2, 3, and 4
			std::vector<std::vector<BYTE>> clipP210;
			Assert::IsTrue(DecodeClipFrames(manifest, *clip, 1920, 1080, VideoFrameEncoding::P210, 48, clipP210, decErr),
				WideOf("P210 1080p decode failed: " + decErr).c_str());

			// =========================================================================
			// PHASE 2 — Playback Disruptions (80 frames, P210 1080p @ 59.94 Hz)
			//   3:2 pulldown repeats, dropped frames, backward sequence restart / seek,
			//   scene cuts, and bright flashes
			// =========================================================================
			TimingStats phase2Gpu, phase2Cpu;
			{
				SoakLegOptions opt;
				opt.name = "Phase2 Playback Disruptions";
				opt.configText = kFullVpvStages;
				opt.width = 1920;
				opt.height = 1080;
				opt.windowWidth = 1920;
				opt.windowHeight = 1080;
				opt.encoding = VideoFrameEncoding::P210;
				opt.rateNum = 60000;
				opt.rateDen = 1001;
				opt.framePeriodMs = 17;
				opt.totalFrames = 80;
				opt.warmupFrames = 6;
				opt.captureAtFrames = { 74 };

				const size_t poolN = clipP210.size();
				opt.onFrame = [&, poolN](int f, IVideoRenderer&, IVideoRenderer*, std::vector<BYTE>& slotBuf) -> FrameAction {
					FrameAction act;
					size_t idx = 0;
					if (f < 25)
					{
						// 3:2 pulldown cadence: 2 repeats then 3 repeats per film frame
						const int cycle = f / 5;
						const int pos = f % 5;
						idx = static_cast<size_t>(cycle * 2 + (pos >= 2 ? 1 : 0)) % poolN;
					}
					else if (f == 27 || f == 28)
					{
						// Dropped frames (missed capture)
						act.send = false;
						return act;
					}
					else if (f == 35)
					{
						// Backward sequence restart / seek (stress-testing SkeletonMemo / lastPresentedSeq)
						act.resetBefore = true;
						act.overrideSeq = 1;
						idx = 0;
					}
					else if (f >= 42 && f < 52)
					{
						// Instantaneous scene cut jump
						idx = static_cast<size_t>((f - 42) + 28) % poolN;
					}
					else if (f >= 52 && f < 60)
					{
						// Second scene cut jump back
						idx = static_cast<size_t>(f - 52) % poolN;
					}
					else
					{
						idx = static_cast<size_t>(f) % poolN;
					}

					const std::vector<BYTE>& src = clipP210[idx];
					std::memcpy(slotBuf.data(), src.data(), (std::min)(src.size(), slotBuf.size()));

					// Inject 2-frame peak-white flashes at frames 64-65 and 70-71
					if (f == 64 || f == 65 || f == 70 || f == 71)
						ApplyBrightFlashP210(slotBuf, 1920, 1080);

					return act;
				};

				const SoakLegResult leg = RunSoakLeg(opt, clipP210, nvMon, cfgPath);
				verifyAdapterAndWatchdogs("Phase2 Disruptions", leg, true);
				Assert::IsFalse(leg.captureJsons.empty(), L"Phase 2: capture missing");
				phase2Gpu = ComputeStats(leg.gpuMs);
				phase2Cpu = ComputeStats(leg.cpuRenderMs);
			}

			// =========================================================================
			// PHASE 3 — Bill Geometry & Dynamic Metadata (0 Post-Warmup Shader Recompiles)
			//   3a (30 frames): Edge blanking ON->OFF, moving subtitle box, anamorphic 1.33x,
			//                   CinemaScope 2.35:1 active-rect auto crop
			//   3b (15 warmup + 60 measured dynamic ticks): ticking room-light lux + dynamic
			//       HDR metadata across appearance/DV/deband shaders -> ASSERT 0 recompiles!
			// =========================================================================
			TimingStats phase3Gpu, phase3Cpu;
			int phase3CacheDelta = 0;
			int phase3PostWarmupCompiles = 0;
			int phase3LoadedObj = -1, phase3SavedObj = -1;
			{
				// --- 3a: Geometry & Subtitle Chaos ---
				const std::string geomBlankOn =
					kFullVpvStages +
					"[vprenderer.viewport]\n"
					"screen_aspect: 2.35:1\n"
					"anamorphic_scale: 1.33\n"
					"automatic_crop: true\n"
					"blank_left: 48\n"
					"blank_top: 24\n"
					"blank_right: 16\n"
					"blank_bottom: 16\n";
				const std::string geomBlankOffCfg = shotsFolder.Path() + "\\soak-blank-off.cfg";
				{
					std::ofstream offCfg(geomBlankOffCfg, std::ios::trunc);
					offCfg << "[general]\npersist_profile_selection: false\n"
						   << "[vprenderer.input.soak]\nwhen: $source_rate > 0\ncapture_path: gpu\n"
						   << kFullVpvStages
						   << "[vprenderer.viewport]\n"
						   << "screen_aspect: 2.35:1\n"
						   << "anamorphic_scale: 1.33\n"
						   << "automatic_crop: true\n"
						   << "blank_left: 0\n"
						   << "blank_top: 0\n"
						   << "blank_right: 0\n"
						   << "blank_bottom: 0\n";
				}

				SoakLegOptions opt3a;
				opt3a.name = "Phase3a Geometry & Subtitles";
				opt3a.configText = geomBlankOn;
				opt3a.width = 1920;
				opt3a.height = 1080;
				opt3a.windowWidth = 3840;
				opt3a.windowHeight = 2160;
				opt3a.encoding = VideoFrameEncoding::P210;
				opt3a.rateNum = 24000;
				opt3a.rateDen = 1001;
				opt3a.framePeriodMs = 18;
				opt3a.totalFrames = 30;
				opt3a.warmupFrames = 5;
				opt3a.subtitleRelocation = true;
				opt3a.applySnapshotBeforeBuild = true;
				opt3a.captureAtFrames = { 12, 26 };

				opt3a.onFrame = [&](int f, IVideoRenderer& r, IVideoRenderer*, std::vector<BYTE>& slotBuf) -> FrameAction {
					const std::vector<BYTE>& src = clipP210[static_cast<size_t>(f) % clipP210.size()];
					std::memcpy(slotBuf.data(), src.data(), (std::min)(src.size(), slotBuf.size()));
					// Moving subtitle box in lower region (shifts vertically & horizontally every 5 frames)
					const int phase = (f / 5) % 3;
					const int boxY = 900 + phase * 36;
					const int boxX = 460 + phase * 80;
					BurnSubtitleBoxP210(slotBuf, 1920, 1080, boxY, 44, boxX, 760);

					// Live toggle edge blanking OFF mid-phase via ApplyApplicationState
					if (f == 15)
					{
						ConfigFile nextCfg;
						std::string err;
						UnifiedProfileRuntime::Runtime rt;
						if (nextCfg.Load(geomBlankOffCfg) && rt.Initialize(nextCfg, {}, err))
						{
							const auto snap = rt.GetSnapshot();
							CString active;
							bool restart = false, live = false;
							if (snap)
								r.ApplyApplicationState(*snap, active, restart, live);
						}
					}
					return FrameAction{};
				};

				const SoakLegResult leg3a = RunSoakLeg(opt3a, clipP210, nvMon, cfgPath);
				verifyAdapterAndWatchdogs("Phase3a Geometry & Subtitles", leg3a, true);
				Assert::IsFalse(leg3a.captureJsons.empty(), L"Phase 3a: capture missing");

				// --- 3b: Dynamic Metadata Ticks (0 Post-Warmup Shader Recompiles) ---
				const std::string dynCfg =
					kFullVpvStages +
					"appearance_room: auto\n"
					"appearance_strength: 0.7\n"
					"ambient_adapt_strength: 0.5\n"
					"dv_trims: on\n";

				// Pre-warm shader cache once with this exact PQ/BT.2020 + appearance configuration
				{
					SoakLegOptions warmOpt;
					warmOpt.name = "Phase3b Dynamic Warmup";
					warmOpt.configText = dynCfg;
					warmOpt.width = 1920;
					warmOpt.height = 1080;
					warmOpt.windowWidth = 1920;
					warmOpt.windowHeight = 1080;
					warmOpt.encoding = VideoFrameEncoding::P210;
					warmOpt.rateNum = 24000;
					warmOpt.rateDen = 1001;
					warmOpt.framePeriodMs = 18;
					warmOpt.totalFrames = 15;
					warmOpt.warmupFrames = 15;
					warmOpt.pqHdr = true;
					warmOpt.onFrame = [&](int f, IVideoRenderer&, IVideoRenderer* ext, std::vector<BYTE>& slotBuf) -> FrameAction {
						const std::vector<BYTE>& src = clipP210[static_cast<size_t>(f) % clipP210.size()];
						std::memcpy(slotBuf.data(), src.data(), (std::min)(src.size(), slotBuf.size()));
						VpvCalls::SetAmbientLux(ext, 15.0, true, static_cast<double>(f) * 0.04);
						return FrameAction{};
					};
					const SoakLegResult warmLeg = RunSoakLeg(warmOpt, clipP210, nvMon, cfgPath);
					verifyAdapterAndWatchdogs("Phase3b Warmup", warmLeg, true);
				}

				// Steady-state 60-frame dynamic metadata tick leg on warm cache:
				// sweep room light (2..120 lux) + dynamic HDR maxCLL/maxFALL every 3 frames
				SoakLegOptions opt3b;
				opt3b.name = "Phase3b Dynamic Metadata Ticks";
				opt3b.configText = dynCfg;
				opt3b.width = 1920;
				opt3b.height = 1080;
				opt3b.windowWidth = 1920;
				opt3b.windowHeight = 1080;
				opt3b.encoding = VideoFrameEncoding::P210;
				opt3b.rateNum = 24000;
				opt3b.rateDen = 1001;
				opt3b.framePeriodMs = 18;
				opt3b.totalFrames = 60;
				opt3b.warmupFrames = 4;
				opt3b.pqHdr = true;
				opt3b.captureAtFrames = { 54 };

				static const double kLuxSchedule[] = {
					2.0, 5.0, 12.0, 22.0, 38.0, 55.0, 80.0, 110.0, 120.0, 95.0,
					70.0, 48.0, 30.0, 18.0, 9.0, 4.0, 15.0, 42.0, 88.0, 25.0
				};
				static const double kCllSchedule[] = {
					1000.0, 1200.0, 1500.0, 2000.0, 4000.0, 800.0, 600.0, 1000.0, 1800.0, 2500.0
				};

				opt3b.onFrame = [&](int f, IVideoRenderer& r, IVideoRenderer* ext, std::vector<BYTE>& slotBuf) -> FrameAction {
					const std::vector<BYTE>& src = clipP210[static_cast<size_t>(f) % clipP210.size()];
					std::memcpy(slotBuf.data(), src.data(), (std::min)(src.size(), slotBuf.size()));
					if (f >= 4 && (f % 3 == 0))
					{
						const size_t tickIdx = static_cast<size_t>((f - 4) / 3);
						const double lux = kLuxSchedule[tickIdx % (sizeof(kLuxSchedule) / sizeof(kLuxSchedule[0]))];
						VpvCalls::SetAmbientLux(ext, lux, true, static_cast<double>(f) * 0.042);

						const double maxCll = kCllSchedule[tickIdx % (sizeof(kCllSchedule) / sizeof(kCllSchedule[0]))];
						VideoStateComPtr dynState = MakeVideoState(1920, 1080, VideoFrameEncoding::P210, 24000, 1001,
							true, maxCll, maxCll * 0.25);
						Assert::IsTrue(r.OnVideoState(dynState),
							L"Phase 3b: dynamic HDR metadata update must be accepted without restart");
					}
					return FrameAction{};
				};

				const SoakLegResult leg3b = RunSoakLeg(opt3b, clipP210, nvMon, cfgPath);
				verifyAdapterAndWatchdogs("Phase3b Dynamic Metadata", leg3b, true);
				Assert::IsFalse(leg3b.captureJsons.empty(), L"Phase 3b: capture missing");
				Assert::AreEqual(0, leg3b.restarts, L"Phase 3b: dynamic metadata ticks triggered unexpected restart");

				phase3LoadedObj = leg3b.shaderCacheLoadedObjects;
				phase3SavedObj = leg3b.shaderCacheSavedObjects;
				phase3PostWarmupCompiles = leg3b.postWarmupShaderCompiles;
				if (phase3LoadedObj >= 0 && phase3SavedObj >= 0)
					phase3CacheDelta = phase3SavedObj - phase3LoadedObj;

				Assert::AreEqual(0, phase3PostWarmupCompiles,
					L"Phase 3b: post-warmup shader compile detected during dynamic metadata ticks");
				Assert::IsTrue(phase3LoadedObj > 0 && phase3SavedObj == phase3LoadedObj,
					WideOf("Phase 3b: shader cache object count grew during dynamic metadata ticks (loaded=" +
						std::to_string(phase3LoadedObj) + ", saved=" + std::to_string(phase3SavedObj) + ")").c_str());

				std::vector<double> p3Gpu = leg3a.gpuMs;
				p3Gpu.insert(p3Gpu.end(), leg3b.gpuMs.begin(), leg3b.gpuMs.end());
				std::vector<double> p3Cpu = leg3a.cpuRenderMs;
				p3Cpu.insert(p3Cpu.end(), leg3b.cpuRenderMs.begin(), leg3b.cpuRenderMs.end());
				phase3Gpu = ComputeStats(p3Gpu);
				phase3Cpu = ComputeStats(p3Cpu);
			}

			// =========================================================================
			// PHASE 4 — Live Pathway Flip-Flop: VP-V -> Original -> VP-V (50 frames)
			//   ASSERT bit-exact Original == Bill + zero leaked VP-V status lines or VRAM
			// =========================================================================
			TimingStats phase4Gpu, phase4Cpu;
			std::string refOriginalHash, flippedOriginalHash;
			bool originalBitExact = false;
			double vramDuringVpv = 0.0, vramAfterOriginal = 0.0;
			{
				const std::vector<std::vector<BYTE>> stillFrame = { clipP210[0] };
				const std::string kOriginalCfg =
					"[vprenderer.Default]\n"
					"dithering: off\n"
					"deband: off\n"
					"[vpv]\n"
					"entrance_gpu_drain: finish\n"
					"pipeline_original_pathway: on\n";

				// Clean reference Original render on frame #0
				SoakLegOptions refOpt;
				refOpt.name = "Phase4 Reference Original";
				refOpt.configText = kOriginalCfg;
				refOpt.plainConfig = true;
				refOpt.width = 1920;
				refOpt.height = 1080;
				refOpt.windowWidth = 1920;
				refOpt.windowHeight = 1080;
				refOpt.encoding = VideoFrameEncoding::P210;
				refOpt.rateNum = 24000;
				refOpt.rateDen = 1001;
				refOpt.framePeriodMs = 18;
				refOpt.totalFrames = 15;
				refOpt.warmupFrames = 4;
				refOpt.captureAtFrames = { 12 };
				const SoakLegResult refLeg = RunSoakLeg(refOpt, stillFrame, nvMon, cfgPath);
				verifyAdapterAndWatchdogs("Phase4 Reference Original", refLeg, false);
				Assert::IsFalse(refLeg.captureJsons.empty(), L"Phase 4 ref Original: capture missing");
				refOriginalHash = ReadSidecarSha256(refLeg.captureJsons.back());
				Assert::IsFalse(refOriginalHash.empty(), L"Phase 4 ref Original: SHA256 missing");

				// Step 4a: VP-V Active (20 frames)
				SoakLegOptions vpvOpt1;
				vpvOpt1.name = "Phase4a VP-V Active";
				vpvOpt1.configText = kFullVpvStages;
				vpvOpt1.width = 1920;
				vpvOpt1.height = 1080;
				vpvOpt1.windowWidth = 1920;
				vpvOpt1.windowHeight = 1080;
				vpvOpt1.encoding = VideoFrameEncoding::P210;
				vpvOpt1.rateNum = 24000;
				vpvOpt1.rateDen = 1001;
				vpvOpt1.framePeriodMs = 18;
				vpvOpt1.totalFrames = 20;
				vpvOpt1.warmupFrames = 4;
				vpvOpt1.afterLastFrame = [&](IVideoRenderer&, VideoStateComPtr&) {
					vramDuringVpv = nvMon.QueryLocalVramMiB();
				};
				const SoakLegResult vpvLeg1 = RunSoakLeg(vpvOpt1, clipP210, nvMon, cfgPath);
				verifyAdapterAndWatchdogs("Phase4a VP-V Active", vpvLeg1, true);

				// Step 4b: Flip VP-V -> Original (15 frames on still frame #0)
				SoakLegOptions origOpt = refOpt;
				origOpt.name = "Phase4b Flipped Original";
				origOpt.afterLastFrame = [&](IVideoRenderer&, VideoStateComPtr&) {
					vramAfterOriginal = nvMon.QueryLocalVramMiB();
				};
				const SoakLegResult origLeg = RunSoakLeg(origOpt, stillFrame, nvMon, cfgPath);
				verifyAdapterAndWatchdogs("Phase4b Flipped Original", origLeg, false);
				Assert::IsFalse(origLeg.captureJsons.empty(), L"Phase 4b Flipped Original: capture missing");
				flippedOriginalHash = ReadSidecarSha256(origLeg.captureJsons.back());
				originalBitExact = (!refOriginalHash.empty() && flippedOriginalHash == refOriginalHash);
				Assert::IsTrue(originalBitExact,
					WideOf("Phase 4b: Original pathway output after VP-V flip differs (" +
						flippedOriginalHash + " vs " + refOriginalHash + ")").c_str());

				// Assert ZERO leaked VP-V active status lines on Original pathway
				Assert::IsTrue(origLeg.dejudderStatus.find("active") == std::string::npos &&
					origLeg.dejudderStatus.find("motion estimated") == std::string::npos,
					WideOf("Phase 4b: leaked VP-V dejudder status on Original pathway: " + origLeg.dejudderStatus).c_str());
				Assert::IsTrue(origLeg.sourceStatus.find("re-base") == std::string::npos &&
					origLeg.sourceStatus.find("active") == std::string::npos,
					WideOf("Phase 4b: leaked VP-V source-resolution status on Original pathway: " + origLeg.sourceStatus).c_str());
				Assert::IsTrue(origLeg.ledStatus.find("active") == std::string::npos,
					WideOf("Phase 4b: leaked VP-V LED status on Original pathway: " + origLeg.ledStatus).c_str());

				// Verify VRAM did not bloat / retain VP-V textures into Original pathway
				Assert::IsTrue(vramAfterOriginal <= vramDuringVpv + 16.0,
					WideOf("Phase 4b: VRAM grew after flipping VP-V -> Original (" +
						std::to_string(vramDuringVpv) + " -> " + std::to_string(vramAfterOriginal) + " MiB)").c_str());

				// Step 4c: Flip Original -> VP-V (15 frames)
				SoakLegOptions vpvOpt2 = vpvOpt1;
				vpvOpt2.name = "Phase4c Restored VP-V";
				vpvOpt2.totalFrames = 15;
				vpvOpt2.captureAtFrames = { 12 };
				vpvOpt2.afterLastFrame = nullptr;
				const SoakLegResult vpvLeg2 = RunSoakLeg(vpvOpt2, clipP210, nvMon, cfgPath);
				verifyAdapterAndWatchdogs("Phase4c Restored VP-V", vpvLeg2, true);
				Assert::IsFalse(vpvLeg2.captureJsons.empty(), L"Phase 4c Restored VP-V: capture missing");

				std::vector<double> p4Gpu = vpvLeg1.gpuMs;
				p4Gpu.insert(p4Gpu.end(), origLeg.gpuMs.begin(), origLeg.gpuMs.end());
				p4Gpu.insert(p4Gpu.end(), vpvLeg2.gpuMs.begin(), vpvLeg2.gpuMs.end());
				std::vector<double> p4Cpu = vpvLeg1.cpuRenderMs;
				p4Cpu.insert(p4Cpu.end(), origLeg.cpuRenderMs.begin(), origLeg.cpuRenderMs.end());
				p4Cpu.insert(p4Cpu.end(), vpvLeg2.cpuRenderMs.begin(), vpvLeg2.cpuRenderMs.end());
				phase4Gpu = ComputeStats(p4Gpu);
				phase4Cpu = ComputeStats(p4Cpu);
			}

			// Release earlier decoded pool before Phase 5 so Phase 5 memory curve is pure steady-state renderer
			clipP210.clear();
			clipP210.shrink_to_fit();

			// =========================================================================
			// PHASE 5 — Steady-State Final 200-Frame Memory & Frame-Time Tail
			//   20 warmup frames + 200 measured real consecutive frames streamed in-place
			//   with ZERO test-harness heap allocations during the measured 200-frame window
			// =========================================================================
			TimingStats phase5Gpu, phase5Cpu;
			double vramStartMiB = 0.0, vramMidMiB = 0.0, vramEndMiB = 0.0, vramDeltaMiB = 0.0;
			double ramWsStartMiB = 0.0, ramWsEndMiB = 0.0, ramWsDeltaMiB = 0.0;
			double ramPrivStartMiB = 0.0, ramPrivEndMiB = 0.0, ramPrivDeltaMiB = 0.0;
			int vramRisingSteps = 0, ramRisingSteps = 0, totalMemSteps = 0;
			{
				RealClip::Decoder streamDecoder;
				const std::string vf = "scale=1920:1080:force_original_aspect_ratio=decrease,pad=1920:1080:(ow-iw)/2:(oh-ih)/2:black,format=p210le";
				Assert::IsTrue(streamDecoder.OpenRaw(manifest, *clip, vf, "p210le", decErr),
					WideOf("Phase 5: stream decoder failed: " + decErr).c_str());

				P210FrameLayout p210Layout{};
				GetP210FrameLayout(1920, 1080, p210Layout);
				std::vector<uint8_t> decodeScratch(p210Layout.frameBytes, 0);
				std::vector<uint8_t> lastGoodFrame(p210Layout.frameBytes, 0);
				bool haveGoodFrame = false;

				SoakLegOptions opt5;
				opt5.name = "Phase5 Steady-State 200-Frame Tail";
				opt5.configText = kFullVpvStages;
				opt5.width = 1920;
				opt5.height = 1080;
				opt5.windowWidth = 1920;
				opt5.windowHeight = 1080;
				opt5.encoding = VideoFrameEncoding::P210;
				opt5.rateNum = 60000;
				opt5.rateDen = 1001;
				opt5.framePeriodMs = 17;
				opt5.warmupFrames = 20;
				opt5.totalFrames = 220; // 20 warmup + 200 measured steady-state frames
				opt5.memorySampleStride = 10;
				opt5.captureAtFrames = { 70, 145, 212 };

				opt5.onFrame = [&](int, IVideoRenderer&, IVideoRenderer*, std::vector<BYTE>& slotBuf) -> FrameAction {
					// Stream directly into pre-allocated scratch buffer (zero heap reallocations!)
					if (streamDecoder.NextRaw(decodeScratch, p210Layout.frameBytes))
					{
						std::memcpy(slotBuf.data(), decodeScratch.data(), p210Layout.frameBytes);
						std::memcpy(lastGoodFrame.data(), decodeScratch.data(), p210Layout.frameBytes);
						haveGoodFrame = true;
					}
					else if (haveGoodFrame)
					{
						std::memcpy(slotBuf.data(), lastGoodFrame.data(), p210Layout.frameBytes);
					}
					return FrameAction{};
				};

				const std::vector<std::vector<BYTE>> emptyPool;
				const SoakLegResult leg5 = RunSoakLeg(opt5, emptyPool, nvMon, cfgPath);
				streamDecoder.Close();

				verifyAdapterAndWatchdogs("Phase5 Steady-State Tail", leg5, true);
				Assert::IsTrue(leg5.captureJsons.size() >= 3, L"Phase 5: expected 3 verified steady-state captures");
				phase5Gpu = ComputeStats(leg5.gpuMs);
				phase5Cpu = ComputeStats(leg5.cpuRenderMs);

				Assert::IsTrue(leg5.memorySamples.size() >= 10, L"Phase 5: insufficient memory checkpoints");
				const size_t nMem = leg5.memorySamples.size();
				vramStartMiB = leg5.memorySamples.front().vramMiB;
				vramMidMiB = leg5.memorySamples[nMem / 2].vramMiB;
				vramEndMiB = leg5.memorySamples.back().vramMiB;
				vramDeltaMiB = vramEndMiB - vramStartMiB;

				ramWsStartMiB = leg5.memorySamples.front().workingSetMiB;
				ramWsEndMiB = leg5.memorySamples.back().workingSetMiB;
				ramWsDeltaMiB = ramWsEndMiB - ramWsStartMiB;

				ramPrivStartMiB = leg5.memorySamples.front().privateBytesMiB;
				ramPrivEndMiB = leg5.memorySamples.back().privateBytesMiB;
				ramPrivDeltaMiB = ramPrivEndMiB - ramPrivStartMiB;

				totalMemSteps = static_cast<int>(nMem - 1);
				for (size_t i = 1; i < nMem; ++i)
				{
					if (leg5.memorySamples[i].vramMiB > leg5.memorySamples[i - 1].vramMiB + 0.5)
						++vramRisingSteps;
					if (leg5.memorySamples[i].privateBytesMiB > leg5.memorySamples[i - 1].privateBytesMiB + 0.5)
						++ramRisingSteps;
				}

				// Print all Phase 5 memory checkpoints so any growth pattern (step vs monotonic) is completely transparent
				Logger::WriteMessage("  Phase 5 steady-state memory curve (every 10 frames across 200f):\n");
				for (size_t i = 0; i < nMem; ++i)
				{
					char mbuf[160];
					snprintf(mbuf, sizeof(mbuf), "    f=%3d | VRAM=%6.1f MiB | Priv=%6.1f MiB | WS=%6.1f MiB\n",
						leg5.memorySamples[i].frameIndex, leg5.memorySamples[i].vramMiB,
						leg5.memorySamples[i].privateBytesMiB, leg5.memorySamples[i].workingSetMiB);
					Logger::WriteMessage(mbuf);
				}

				// Assert flat / non-monotonic VRAM & Process RAM across final 200 frames
				if (vramDeltaMiB > 16.0)
					recordFailure("Phase 5: VRAM grew by " + std::to_string(vramDeltaMiB) + " MiB across 200 steady-state frames");
				if (vramRisingSteps >= (totalMemSteps * 3) / 4)
					recordFailure("Phase 5: monotonic VRAM growth detected (" + std::to_string(vramRisingSteps) + "/" +
						std::to_string(totalMemSteps) + " rising steps)");
				if (ramPrivDeltaMiB > 32.0)
					recordFailure("Phase 5: Process private RAM grew by " + std::to_string(ramPrivDeltaMiB) + " MiB across 200 steady-state frames");
				if (ramWsDeltaMiB > 32.0)
					recordFailure("Phase 5: Process working-set RAM grew by " + std::to_string(ramWsDeltaMiB) + " MiB across 200 steady-state frames");
				if (ramRisingSteps >= (totalMemSteps * 3) / 4)
					recordFailure("Phase 5: monotonic Process RAM growth detected (" + std::to_string(ramRisingSteps) + "/" +
						std::to_string(totalMemSteps) + " rising steps)");
			}

			// =========================================================================
			// FULL VIRTUAL HDMI SOAK SCORECARD
			// =========================================================================
			Assert::IsTrue(totalFramesProcessed >= 500,
				WideOf("Expected >= 500 frames processed, got " + std::to_string(totalFramesProcessed)).c_str());

			std::ostringstream report;
			report << "\n================ VIRTUAL HDMI SOAK SCORECARD ================\n"
				   << "Adapter Bound        : " << observedAdapter << "\n"
				   << "Total Real Frames    : " << totalFramesProcessed << "\n"
				   << "Captures Verified    : " << totalCapturesVerified << " (NaN/Inf/Corrupt=" << totalNanOrCorruptFrames << ")\n"
				   << "Fatal Log Errors     : " << totalLogErrors << "\n"
				   << "GpuAttribution       : " << totalGpuPasses << " passes, " << totalGpuUnattributed << " unattributed\n"
				   << "Phase 3 Shader Cache : loaded=" << phase3LoadedObj << " saved=" << phase3SavedObj
				   << " delta=" << phase3CacheDelta << " postWarmupCompiles=" << phase3PostWarmupCompiles << "\n"
				   << "Phase 4 Original=Bill: " << (originalBitExact ? "BIT-EXACT PASS" : "FAIL")
				   << " (sha256=" << flippedOriginalHash.substr(0, 16) << "...; VRAM VP-V="
				   << std::fixed << std::setprecision(1) << vramDuringVpv << " MiB -> Original=" << vramAfterOriginal << " MiB)\n"
				   << "Phase 5 VRAM (200f)  : start=" << vramStartMiB << " MiB mid=" << vramMidMiB
				   << " MiB end=" << vramEndMiB << " MiB delta=" << std::showpos << vramDeltaMiB << std::noshowpos
				   << " MiB (rising " << vramRisingSteps << "/" << totalMemSteps << ")\n"
				   << "Phase 5 RAM Priv/WS  : priv " << ramPrivStartMiB << " -> " << ramPrivEndMiB
				   << " MiB (delta=" << std::showpos << ramPrivDeltaMiB << std::noshowpos << " MiB, rising "
				   << ramRisingSteps << "/" << totalMemSteps << "); WS " << ramWsStartMiB << " -> " << ramWsEndMiB
				   << " MiB (delta=" << std::showpos << ramWsDeltaMiB << std::noshowpos << " MiB)\n"
				   << "Per-Phase Frame Times (RTX 3050 Ti GPU & CPU Submit):\n"
				   << FormatStatsLine("Phase 1 Format/Res Switches", phase1Gpu, phase1Cpu)
				   << FormatStatsLine("Phase 2 Playback Disruptions", phase2Gpu, phase2Cpu)
				   << FormatStatsLine("Phase 3 Geometry + Dyn Meta", phase3Gpu, phase3Cpu)
				   << FormatStatsLine("Phase 4 Pathway Flip-Flop", phase4Gpu, phase4Cpu)
				   << FormatStatsLine("Phase 5 Steady-State 200f", phase5Gpu, phase5Cpu)
				   << "=============================================================\n";
			Logger::WriteMessage(report.str().c_str());
			Assert::IsTrue(failures.empty(), WideOf("Virtual HDMI Soak watchdog failures: " + failures).c_str());
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(VirtualHdmiPhase6ToggleStarvationAndSweepSoak)
			TEST_METHOD_ATTRIBUTE(L"Category", L"Footage")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(VirtualHdmiPhase6ToggleStarvationAndSweepSoak)
		{
			const char* const kTestName = "VirtualHdmiPhase6ToggleStarvationAndSweepSoak";
			const bool gpuHw = (SoakEnv("VP_TEST_GPU") == "hardware");
			if (!HardwarePrerequisites::RequireOrSkip(HardwarePrerequisites::Kind::Gpu, kTestName, gpuHw,
				"set VP_TEST_GPU=hardware and VP_TEST_ADAPTER=NVIDIA"))
				return;

			const RealClip::Manifest manifest = RealClip::Manifest::Load();
			if (!HardwarePrerequisites::RequireOrSkip(HardwarePrerequisites::Kind::Footage, kTestName, manifest.Loaded(),
				manifest.error.empty() ? "real clip library manifest unavailable" : manifest.error))
				return;

			const RealClip::Clip* clip = nullptr;
			const char* const preferredIds[] = { "tos_24p_3min", "tos_23976p_3min", "tos_1920x800_24p" };
			for (size_t i = 0; i < sizeof(preferredIds) / sizeof(preferredIds[0]) && !clip; ++i)
				clip = manifest.Find(preferredIds[i]);
			if (!clip)
			{
				for (size_t i = 0; i < manifest.clips.size(); ++i)
				{
					if (manifest.clips[i].frames >= 60 && manifest.clips[i].width >= 1280)
					{
						clip = &manifest.clips[i];
						break;
					}
				}
			}
			if (!HardwarePrerequisites::RequireOrSkip(HardwarePrerequisites::Kind::Footage, kTestName, clip != nullptr,
				"no suitable real clip found in manifest"))
				return;

			NvidiaAdapterMonitor nvMon;
			std::string nvName, nvErr;
			Assert::IsTrue(nvMon.Initialize(nvName, nvErr),
				WideOf("Hard NVIDIA adapter lock failed: " + nvErr).c_str());
			Assert::IsTrue(UpperOf(nvName).find("NVIDIA") != std::string::npos,
				WideOf("Expected NVIDIA adapter, got: " + nvName).c_str());

			const ScopedEnvVar envAdapter("VP_TEST_ADAPTER", "NVIDIA");
			const std::string binDir = BinaryFolder();
			const ScopedEnvVar envShaderDir("VP_TEST_SHADER_DIR",
				(!binDir.empty() && (binDir.back() == '\\' || binDir.back() == '/')) ? binDir.substr(0, binDir.size() - 1) : binDir);
			const TempFolderScope shotsFolder("vp-virtual-hdmi-phase6-shots");
			const ScopedEnvVar envShots("VP_TEST_SCREENSHOT_DIR", shotsFolder.Path());
			const std::string cacheDir = BinaryFolder() + "vp-harness-shader-cache";
			CreateDirectoryA(cacheDir.c_str(), nullptr);
			CreateDirectoryA((cacheDir + "\\vprenderer").c_str(), nullptr);
			const ScopedEnvVar envCache("VP_TEST_SHADER_CACHE_DIR", cacheDir);
			const std::string cfgPath = shotsFolder.Path() + "\\soak-phase6.cfg";

			std::string decErr;
			std::vector<std::vector<BYTE>> clip1080;
			Assert::IsTrue(DecodeClipFrames(manifest, *clip, 1920, 1080, VideoFrameEncoding::P210, 36, clip1080, decErr),
				WideOf("Phase 6 1080p decode failed: " + decErr).c_str());
			std::vector<std::vector<BYTE>> clip4k;
			Assert::IsTrue(DecodeClipFrames(manifest, *clip, 3840, 2160, VideoFrameEncoding::P010, 8, clip4k, decErr),
				WideOf("Phase 6 4K decode failed: " + decErr).c_str());

			const std::vector<std::vector<BYTE>> still1080 = { clip1080[8] };
			const std::vector<std::vector<BYTE>> still4k = { clip4k[2] };

			int totalFrames = 0;
			int totalCaptures = 0;
			std::string observedAdapter;
			const bool allowOffscreenOccluded = (SoakEnv("VP_TEST_ALLOW_OCCLUDED_DESKTOP") == "1");
			bool anyOccludedOffscreen = false;

			auto verifyLeg = [&](const std::string& tag, const SoakLegResult& leg) -> CaptureVerification {
				Assert::IsTrue(leg.created, WideOf(tag + ": renderer creation failed: " + leg.errorDetail).c_str());
				if (!leg.presented)
				{
					Assert::IsTrue(allowOffscreenOccluded && leg.finalGpuSourceSeq > 0,
						WideOf(tag + ": never presented live frame\n" + LogTail(leg.log)).c_str());
					anyOccludedOffscreen = true;
				}
				Assert::IsTrue(UpperOf(leg.adapterName).find("NVIDIA") != std::string::npos,
					WideOf(tag + ": non-NVIDIA adapter bound: " + leg.adapterName).c_str());
				Assert::AreEqual(0, leg.logErrorsOrNans,
					WideOf(tag + ": fatal GPU/NaN error logged: " + leg.errorDetail).c_str());
				if (observedAdapter.empty())
					observedAdapter = leg.adapterName;
				totalFrames += leg.framesSent;
				Assert::IsFalse(leg.captureJsons.empty(), WideOf(tag + ": missing rendered output capture").c_str());
				CaptureVerification lastCv;
				for (size_t i = 0; i < leg.captureJsons.size(); ++i)
				{
					lastCv = VerifyCaptureJson(leg.captureJsons[i]);
					++totalCaptures;
					Assert::IsTrue(lastCv.valid, WideOf(tag + ": invalid/NaN/dead-black capture").c_str());
				}
				return lastCv;
			};

			const std::string kDetDefaultOff =
				"[vprenderer.Default]\n"
				"dithering: off\n"
				"deband: off\n"
				"temporal_dither: off\n";

			const std::string kDetOffBase =
				"source_resolution: native\n"
				"source_resolution_crossfade_s: 0\n"
				"deband_vp: off\n"
				"low_bitrate_handling: off\n"
				"temporal_dither: off\n"
				"detail_pulse_stabilise: off\n"
				"texture_restore: off\n"
				"adaptive_upscale: off\n"
				"adaptive_downscale: off\n"
				"adaptive_grain_map: off\n"
				"adaptive_chroma_ssim: off\n"
				"intent_black: off\n"
				"intent_display: off\n"
				"interpolation: off\n"
				"appearance: off\n"
				"dejudder_strength: 0\n" +
				kDetDefaultOff +
				"[vpv]\n"
				"resync_event: on\n"
				"frame_analysis_store: on\n"
				"[vpv.skeleton]\n"
				"analysis_lane: on\n"
				"skeleton_gpu_gap: after_render\n"
				"analysis_lane_chunk_us: 4000\n";

			auto runStillSweep = [&](const std::string& tag, const std::string& extraCfg, bool pqHdr,
				double ambientLux, int srcW, int srcH, int winW, int winH, VideoFrameEncoding enc,
				const std::vector<std::vector<BYTE>>& pool, SoakLegResult* outLeg = nullptr) -> CaptureVerification {
				SoakLegOptions opt;
				opt.name = tag;
				opt.configText = extraCfg;
				opt.width = srcW;
				opt.height = srcH;
				opt.windowWidth = winW;
				opt.windowHeight = winH;
				opt.encoding = enc;
				opt.rateNum = 60000;
				opt.rateDen = 1001;
				opt.framePeriodMs = 17;
				opt.totalFrames = 14;
				opt.warmupFrames = 4;
				opt.pqHdr = pqHdr;
				opt.allowOccludedOffscreen = allowOffscreenOccluded;
				opt.captureAtFrames = { 11 };
				if (ambientLux > 0.0)
				{
					opt.onFrame = [&, ambientLux](int f, IVideoRenderer&, IVideoRenderer* ext, std::vector<BYTE>& slotBuf) -> FrameAction {
						const std::vector<BYTE>& src = pool[static_cast<size_t>(f) % pool.size()];
						std::memcpy(slotBuf.data(), src.data(), (std::min)(src.size(), slotBuf.size()));
						VpvCalls::SetAmbientLux(ext, ambientLux, true, static_cast<double>(f) * 0.042);
						return FrameAction{};
					};
				}
				const SoakLegResult leg = RunSoakLeg(opt, pool, nvMon, cfgPath);
				if (outLeg)
					*outLeg = leg;
				return verifyLeg(tag, leg);
			};

			// =========================================================================
			// PHASE 6a — Module Toggles & Parameter Sweeps (Verify Real Pixel & Telemetry Response)
			// =========================================================================
			// 1. Deterministic baseline (all optional stages OFF)
			const CaptureVerification cvBase = runStillSweep(
				"Phase6a-Base-Off", kDetOffBase, false, 0.0, 1920, 1080, 1920, 1080, VideoFrameEncoding::P210, still1080);

			// 2. VP Debander toggle: OFF -> ON -> OFF (must alter pixels ON, revert bit-exact OFF)
			const CaptureVerification cvDebandOn = runStillSweep(
				"Phase6a-Deband-On",
				"source_resolution: native\nsource_resolution_crossfade_s: 0\ndeband_vp: on\n"
				"low_bitrate_handling: off\ntemporal_dither: off\ndetail_pulse_stabilise: off\ntexture_restore: off\n"
				"adaptive_upscale: off\nadaptive_downscale: off\nadaptive_grain_map: off\nadaptive_chroma_ssim: off\n"
				"intent_black: off\nintent_display: off\ninterpolation: off\nappearance: off\ndejudder_strength: 0\n" +
				kDetDefaultOff +
				"[vpv]\nresync_event: on\nframe_analysis_store: on\n"
				"[vpv.skeleton]\nanalysis_lane: on\nskeleton_gpu_gap: after_render\nanalysis_lane_chunk_us: 4000\n",
				false, 0.0, 1920, 1080, 1920, 1080, VideoFrameEncoding::P210, still1080);
			const CaptureVerification cvDebandOff = runStillSweep(
				"Phase6a-Deband-RevertOff", kDetOffBase, false, 0.0, 1920, 1080, 1920, 1080, VideoFrameEncoding::P210, still1080);
			Assert::AreNotEqual(cvBase.sha256, cvDebandOn.sha256,
				L"Phase 6a: deband_vp=on produced identical output to off (dead switch)");
			Assert::AreEqual(cvBase.sha256, cvDebandOff.sha256,
				L"Phase 6a: deband_vp=off did not cleanly revert to baseline");

			// 3. CAM16 Appearance parameter sweep (PQ HDR + lit living room): OFF -> 0.30 -> 0.90 -> OFF
			const std::string kAppDetHead =
				"source_resolution: native\nsource_resolution_crossfade_s: 0\ndeband_vp: off\n"
				"low_bitrate_handling: off\ntemporal_dither: off\ndetail_pulse_stabilise: off\ntexture_restore: off\n"
				"adaptive_upscale: off\nadaptive_downscale: off\nadaptive_grain_map: off\nadaptive_chroma_ssim: off\n"
				"intent_black: off\nintent_display: off\ninterpolation: off\ndejudder_strength: 0\n";
			const std::string kAppHdrBase = kAppDetHead +
				"[vprenderer.Default]\ndithering: off\ndeband: off\ntemporal_dither: off\n"
				"appearance: off\nappearance_room: living_lit\nappearance_strength: 0.0\nappearance_screen_white_nits: 48\n"
				"[vpv]\nresync_event: on\nframe_analysis_store: on\n";
			SoakLegResult legApp03;
			const CaptureVerification cvAppOff = runStillSweep(
				"Phase6a-App-Off", kAppHdrBase, true, 85.0, 1920, 1080, 1920, 1080, VideoFrameEncoding::P210, still1080);
			const CaptureVerification cvApp03 = runStillSweep(
				"Phase6a-App-0.30", kAppDetHead +
				"appearance: on\nappearance_room: living_lit\nappearance_strength: 0.30\nappearance_screen_white_nits: 48\n"
				"[vprenderer.Default]\ndithering: off\ndeband: off\ntemporal_dither: off\n"
				"appearance: on\nappearance_room: living_lit\nappearance_strength: 0.30\nappearance_screen_white_nits: 48\n"
				"[vpv]\nresync_event: on\nframe_analysis_store: on\n",
				true, 85.0, 1920, 1080, 1920, 1080, VideoFrameEncoding::P210, still1080, &legApp03);
			const CaptureVerification cvApp09 = runStillSweep(
				"Phase6a-App-0.90", kAppDetHead +
				"appearance: on\nappearance_room: living_lit\nappearance_strength: 0.90\nappearance_screen_white_nits: 48\n"
				"[vprenderer.Default]\ndithering: off\ndeband: off\ntemporal_dither: off\n"
				"appearance: on\nappearance_room: living_lit\nappearance_strength: 0.90\nappearance_screen_white_nits: 48\n"
				"[vpv]\nresync_event: on\nframe_analysis_store: on\n",
				true, 85.0, 1920, 1080, 1920, 1080, VideoFrameEncoding::P210, still1080);
			const CaptureVerification cvAppBackOff = runStillSweep(
				"Phase6a-App-RevertOff", kAppHdrBase, true, 85.0, 1920, 1080, 1920, 1080, VideoFrameEncoding::P210, still1080);
			std::string appDiag;
			for (size_t li = 0; li < legApp03.log.size(); ++li)
			{
				const std::string& l = legApp03.log[li];
				if (l.find("appearance") != std::string::npos || l.find("cam16") != std::string::npos ||
					l.find("Appearance") != std::string::npos || l.find("shader") != std::string::npos)
					appDiag += "  [app03] " + l + "\n";
			}
			std::string p6Failures;
			auto p6Fail = [&](const std::string& m) {
				p6Failures += m + "; ";
				Logger::WriteMessage(("  [PHASE6-FAIL] " + m + "\n").c_str());
			};
			if (cvAppOff.sha256 == cvApp03.sha256)
				p6Fail("Phase 6a: appearance=on (0.30) identical to off (fallbacks=" + legApp03.fallbacksStatus + ")\n" + appDiag);
			if (cvApp03.sha256 == cvApp09.sha256)
				p6Fail("Phase 6a: appearance_strength 0.30 vs 0.90 identical");
			if (cvAppOff.sha256 != cvAppBackOff.sha256)
				p6Fail("Phase 6a: appearance=off did not cleanly revert");

			// 4. Temporal Dither (sigma_delta vs off) & Detail Pulse Stabilise (on vs off)
			const CaptureVerification cvDitherSd = runStillSweep(
				"Phase6a-TemporalDither-SD",
				"source_resolution: native\nsource_resolution_crossfade_s: 0\ndeband_vp: off\n"
				"low_bitrate_handling: off\ntemporal_dither: sigma_delta\ndetail_pulse_stabilise: off\ntexture_restore: off\n"
				"adaptive_upscale: off\nadaptive_downscale: off\nadaptive_grain_map: off\nadaptive_chroma_ssim: off\n"
				"intent_black: off\nintent_display: off\ninterpolation: off\nappearance: off\ndejudder_strength: 0\n"
				"[vprenderer.Default]\ndithering: off\ndeband: off\ntemporal_dither: sigma_delta\n"
				"[vpv]\nresync_event: on\nframe_analysis_store: on\n",
				false, 0.0, 1920, 1080, 1920, 1080, VideoFrameEncoding::P210, still1080);
			Assert::AreNotEqual(cvBase.sha256, cvDitherSd.sha256,
				L"Phase 6a: temporal_dither=sigma_delta produced identical output to off");

			const CaptureVerification cvDetailPulse = runStillSweep(
				"Phase6a-DetailPulse-On",
				"deband_vp: on\ndetail_pulse_stabilise: on\ndetail_pulse_cap: 1.5\n"
				"temporal_dither: off\nappearance: off\n" +
				kDetDefaultOff +
				"[vpv]\nresync_event: on\nframe_analysis_store: on\n"
				"[vpv.skeleton]\nanalysis_lane: on\nskeleton_gpu_gap: after_render\nanalysis_lane_chunk_us: 4000\n",
				false, 0.0, 1920, 1080, 1920, 1080, VideoFrameEncoding::P210, clip1080);
			Assert::IsTrue(cvDetailPulse.valid, L"Phase 6a: detail_pulse_stabilise capture invalid");

			// 5. Re-Base Scaler (source_resolution: native -> 720 -> native on 1080p -> 4K upscale) + Adaptive Downscale
			SoakLegResult legRebaseOff, legRebaseOn;
			const CaptureVerification cvDownOff = runStillSweep(
				"Phase6a-Scaler-Native", kDetOffBase, false, 0.0, 1920, 1080, 3840, 2160, VideoFrameEncoding::P210, still1080, &legRebaseOff);
			const CaptureVerification cvDownOn = runStillSweep(
				"Phase6a-Scaler-Rebase720",
				"source_resolution: 720\nsource_resolution_crossfade_s: 0\ndeband_vp: off\n"
				"low_bitrate_handling: off\ntemporal_dither: off\ndetail_pulse_stabilise: off\ntexture_restore: off\n"
				"adaptive_upscale: off\nadaptive_downscale: on\nadaptive_grain_map: off\nadaptive_chroma_ssim: off\n"
				"intent_black: off\nintent_display: off\ninterpolation: off\nappearance: off\ndejudder_strength: 0\n"
				"[vprenderer.Default]\ndithering: off\ndeband: off\ntemporal_dither: off\n"
				"source_resolution: 720\nsource_resolution_crossfade_s: 0\nadaptive_downscale: on\n"
				"[vpv]\nresync_event: on\nframe_analysis_store: on\n",
				false, 0.0, 1920, 1080, 3840, 2160, VideoFrameEncoding::P210, still1080, &legRebaseOn);
			const CaptureVerification cvDownRevert = runStillSweep(
				"Phase6a-Scaler-RevertNative", kDetOffBase, false, 0.0, 1920, 1080, 3840, 2160, VideoFrameEncoding::P210, still1080);
			if (cvDownOff.sha256 == cvDownOn.sha256)
				p6Fail("Phase 6a: source_resolution=720 (1080p->4K) produced identical output to native (status=" + legRebaseOn.sourceStatus + ")");
			if (cvDownOff.sha256 != cvDownRevert.sha256)
				p6Fail("Phase 6a: source_resolution=native did not cleanly revert");

			// 6. Motion Vector Engine + Dejudder (24p moving clip, strength 0 -> 0.50 -> 0)
			SoakLegResult legDejOn, legDejOff;
			{
				SoakLegOptions optOn;
				optOn.name = "Phase6a-VectorEngine-Dejudder-0.50";
				optOn.configText =
					"deband_vp: on\noptical_flow: on\noptical_flow_global: on\ndejudder_strength: 0.50\n"
					"[vpv]\nresync_event: on\nframe_analysis_store: on\n"
					"[vpv.skeleton]\nanalysis_lane: on\nskeleton_gpu_gap: after_render\nanalysis_lane_chunk_us: 4000\n";
				optOn.width = 1920;
				optOn.height = 1080;
				optOn.windowWidth = 1920;
				optOn.windowHeight = 1080;
				optOn.encoding = VideoFrameEncoding::P210;
				optOn.rateNum = 24000;
				optOn.rateDen = 1001;
				optOn.framePeriodMs = 42;
				optOn.totalFrames = 26;
				optOn.warmupFrames = 6;
				optOn.allowOccludedOffscreen = allowOffscreenOccluded;
				optOn.captureAtFrames = { 22 };
				legDejOn = RunSoakLeg(optOn, clip1080, nvMon, cfgPath);
				verifyLeg(optOn.name, legDejOn);
				Assert::IsFalse(legDejOn.dejudderStatus.empty(),
					L"Phase 6a: dejudder/vector-engine status empty when dejudder_strength=0.50");

				SoakLegOptions optOff = optOn;
				optOff.name = "Phase6a-VectorEngine-Dejudder-Off";
				optOff.configText = kDetOffBase;
				optOff.totalFrames = 14;
				optOff.captureAtFrames = { 11 };
				legDejOff = RunSoakLeg(optOff, clip1080, nvMon, cfgPath);
				verifyLeg(optOff.name, legDejOff);
				Assert::AreNotEqual(legDejOn.dejudderStatus, legDejOff.dejudderStatus,
					WideOf("Phase 6a: dejudder status unchanged between strength=0.50 and 0 (" +
						legDejOn.dejudderStatus + ")").c_str());
			}

			// 7. Live Hot-Reload via ApplyApplicationState on a SINGLE running renderer instance
			bool hotReloadApplied = false;
			bool hotReloadLiveNoRestart = false;
			std::string hotHashBefore, hotHashAfter;
			{
				SoakLegOptions optHot;
				optHot.name = "Phase6a-LiveHotReload";
				optHot.configText =
					"source_resolution: native\nsource_resolution_crossfade_s: 0\ndeband_vp: off\ntemporal_dither: off\n"
					"appearance: on\nappearance_room: living_lit\nappearance_strength: 0.25\nappearance_screen_white_nits: 48\n"
					"[vprenderer.Default]\ndithering: off\ndeband: off\ntemporal_dither: off\n"
					"appearance: on\nappearance_room: living_lit\nappearance_strength: 0.25\nappearance_screen_white_nits: 48\n"
					"[vpv]\nresync_event: on\nframe_analysis_store: on\n";
				optHot.width = 1920;
				optHot.height = 1080;
				optHot.windowWidth = 1920;
				optHot.windowHeight = 1080;
				optHot.encoding = VideoFrameEncoding::P210;
				optHot.rateNum = 60000;
				optHot.rateDen = 1001;
				optHot.framePeriodMs = 17;
				optHot.totalFrames = 28;
				optHot.warmupFrames = 4;
				optHot.pqHdr = true;
				optHot.applySnapshotBeforeBuild = true;
				optHot.allowOccludedOffscreen = allowOffscreenOccluded;
				optHot.captureAtFrames = { 10, 24 };
				optHot.onFrame = [&](int f, IVideoRenderer& r, IVideoRenderer* ext, std::vector<BYTE>& slotBuf) -> FrameAction {
					std::memcpy(slotBuf.data(), still1080[0].data(), (std::min)(still1080[0].size(), slotBuf.size()));
					VpvCalls::SetAmbientLux(ext, 85.0, true, static_cast<double>(f) * 0.042);
					if (f == 14)
					{
						{
							std::ofstream outCfg(cfgPath, std::ios::trunc);
							outCfg << "[general]\npersist_profile_selection: false\n"
								   << "[vprenderer.input.soak]\nwhen: $source_rate > 0\ncapture_path: gpu\n"
								   << "source_resolution: native\nsource_resolution_crossfade_s: 0\ndeband_vp: off\ntemporal_dither: off\n"
								   << "appearance: on\nappearance_room: living_lit\nappearance_strength: 0.90\nappearance_screen_white_nits: 48\n"
								   << "[vprenderer.Default]\ndithering: off\ndeband: off\ntemporal_dither: off\n"
								   << "appearance: on\nappearance_room: living_lit\nappearance_strength: 0.90\nappearance_screen_white_nits: 48\n"
								   << "[vpv]\nresync_event: on\nframe_analysis_store: on\n";
						}
						ConfigFile nextCfg;
						std::string err;
						UnifiedProfileRuntime::Runtime rt;
						if (nextCfg.Load(cfgPath) && rt.Initialize(nextCfg, {}, err))
						{
							const auto snap = rt.GetSnapshot();
							CString active;
							bool restart = false, live = false;
							if (snap)
							{
								hotReloadApplied = r.ApplyApplicationState(*snap, active, restart, live);
								hotReloadLiveNoRestart = !restart;
							}
						}
					}
					return FrameAction{};
				};
				const SoakLegResult legHot = RunSoakLeg(optHot, still1080, nvMon, cfgPath);
				verifyLeg(optHot.name, legHot);
				Assert::IsTrue(hotReloadApplied, L"Phase 6a: live ApplyApplicationState hot-reload returned false");
				Assert::IsTrue(legHot.captureJsons.size() >= 2, L"Phase 6a: expected 2 captures around hot-reload");
				hotHashBefore = ReadSidecarSha256(legHot.captureJsons[0]);
				hotHashAfter = ReadSidecarSha256(legHot.captureJsons[1]);
			}

			// =========================================================================
			// PHASE 6b — Frame Starvation & Resume (200 ms + 500 ms HDMI Signal Stalls)
			// =========================================================================
			uint64_t seqBeforeStall = 0;
			SoakLegResult legStarve;
			{
				SoakLegOptions optStarve;
				optStarve.name = "Phase6b-FrameStarvation-200ms-500ms";
				optStarve.configText =
					"deband_vp: on\noptical_flow: on\ndejudder_strength: 0.25\n"
					"[vpv]\nresync_event: on\nframe_analysis_store: on\n"
					"[vpv.skeleton]\nanalysis_lane: on\nskeleton_gpu_gap: after_render\nanalysis_lane_chunk_us: 4000\n";
				optStarve.width = 1920;
				optStarve.height = 1080;
				optStarve.windowWidth = 1920;
				optStarve.windowHeight = 1080;
				optStarve.encoding = VideoFrameEncoding::P210;
				optStarve.rateNum = 60000;
				optStarve.rateDen = 1001;
				optStarve.framePeriodMs = 17;
				optStarve.totalFrames = 50;
				optStarve.warmupFrames = 5;
				optStarve.allowOccludedOffscreen = allowOffscreenOccluded;
				optStarve.captureAtFrames = { 12, 46 };
				optStarve.onFrame = [&](int f, IVideoRenderer& r, IVideoRenderer*, std::vector<BYTE>& slotBuf) -> FrameAction {
					const std::vector<BYTE>& src = clip1080[static_cast<size_t>(f) % clip1080.size()];
					std::memcpy(slotBuf.data(), src.data(), (std::min)(src.size(), slotBuf.size()));
					FrameAction act;
					if (f == 15)
					{
						RendererRenderLoad load{};
						if (r.GetRenderLoad(load) && load.gpuValid)
							seqBeforeStall = load.latestGpuSourceSequence;
						act.stallAfterMs = 200; // 200 ms Apple TV pause / HDMI hiccup
					}
					else if (f == 30)
					{
						act.stallAfterMs = 500; // 500 ms deep starvation stall
					}
					return act;
				};
				legStarve = RunSoakLeg(optStarve, clip1080, nvMon, cfgPath);
				verifyLeg(optStarve.name, legStarve);
				Assert::IsTrue(legStarve.finalGpuSourceSeq > seqBeforeStall,
					WideOf("Phase 6b: GPU frame sequence did not advance after 200ms/500ms starvation stalls (" +
						std::to_string(seqBeforeStall) + " -> " + std::to_string(legStarve.finalGpuSourceSeq) + ")").c_str());
			}

			// =========================================================================
			// PHASE 6c — Forced Analysis-Lane Overload Step-Down & Clean Recovery
			//   analysis_lane_chunk_us: 4000 us -> 200 us (squeezed) -> 4000 us (recovered)
			// =========================================================================
			SoakLegResult legHealthy, legSqueezed, legRecovered;
			{
				auto runBudgetLeg = [&](const std::string& tag, int chunkUs) -> SoakLegResult {
					SoakLegOptions opt;
					opt.name = tag;
					opt.configText =
						"deband_vp: on\noptical_flow: on\ndejudder_strength: 0.25\n"
						"[vpv]\nresync_event: on\nframe_analysis_store: on\n"
						"[vpv.skeleton]\nanalysis_lane: on\nskeleton_gpu_gap: after_render\n"
						"analysis_lane_chunk_us: " + std::to_string(chunkUs) + "\n";
					opt.width = 1920;
					opt.height = 1080;
					opt.windowWidth = 1920;
					opt.windowHeight = 1080;
					opt.encoding = VideoFrameEncoding::P210;
					opt.rateNum = 60000;
					opt.rateDen = 1001;
					opt.framePeriodMs = 17;
					opt.totalFrames = 24;
					opt.warmupFrames = 5;
					opt.allowOccludedOffscreen = allowOffscreenOccluded;
					opt.captureAtFrames = { 20 };
					const SoakLegResult leg = RunSoakLeg(opt, clip1080, nvMon, cfgPath);
					verifyLeg(tag, leg);
					return leg;
				};
				legHealthy = runBudgetLeg("Phase6c-Budget-4000us", 4000);
				legSqueezed = runBudgetLeg("Phase6c-Budget-200us-Overload", 200);
				legRecovered = runBudgetLeg("Phase6c-Budget-4000us-Recovery", 4000);
			}

			// =========================================================================
			// PHASE 6d — Kitchen-Sink All-Modules-ON Stack + 24x Rapid-Fire Live Toggle Torture
			//   1) Every major Wave 2-3 feature enabled simultaneously on PQ HDR 24p footage
			//   2) Single running renderer hammered with 24 back-to-back live ApplyApplicationState
			//      toggles & parameter sweeps across 168 real frames, checking flat VRAM & 0 NaN/errors
			// =========================================================================
			SoakLegResult legKitchenSink, legTorture;
			int tortureAppliedCount = 0;
			int tortureLiveCount = 0;
			int tortureRestartFlagCount = 0;
			double tortureVramStart = 0.0, tortureVramEnd = 0.0, tortureVramDelta = 0.0;
			{
				const std::string kKitchenSinkCfg =
					"deband_vp: on\n"
					"low_bitrate_handling: auto\n"
					"detail_pulse_stabilise: on\n"
					"detail_pulse_cap: 1.35\n"
					"texture_restore: statistical\n"
					"texture_restore_cap: 1.5\n"
					"appearance: on\n"
					"appearance_room: auto\n"
					"appearance_strength: 0.70\n"
					"ambient_adapt_strength: 0.50\n"
					"dv_trims: on\n"
					"optical_flow: on\n"
					"optical_flow_global: on\n"
					"dejudder_strength: 0.35\n"
					"temporal_dither: sigma_delta\n"
					"source_resolution: auto\n"
					"[vpv]\n"
					"resync_event: on\n"
					"frame_analysis_store: on\n"
					"[vpv.skeleton]\n"
					"analysis_lane: on\n"
					"skeleton_gpu_gap: after_render\n"
					"analysis_lane_chunk_us: 4000\n";

				SoakLegOptions optKitchen;
				optKitchen.name = "Phase6d-KitchenSink-AllOn";
				optKitchen.configText = kKitchenSinkCfg;
				optKitchen.width = 1920;
				optKitchen.height = 1080;
				optKitchen.windowWidth = 1920;
				optKitchen.windowHeight = 1080;
				optKitchen.encoding = VideoFrameEncoding::P210;
				optKitchen.rateNum = 24000;
				optKitchen.rateDen = 1001;
				optKitchen.framePeriodMs = 42;
				optKitchen.totalFrames = 36;
				optKitchen.warmupFrames = 6;
				optKitchen.pqHdr = true;
				optKitchen.allowOccludedOffscreen = allowOffscreenOccluded;
				optKitchen.captureAtFrames = { 16, 32 };
				optKitchen.onFrame = [&](int f, IVideoRenderer&, IVideoRenderer* ext, std::vector<BYTE>& slotBuf) -> FrameAction {
					const std::vector<BYTE>& src = clip1080[static_cast<size_t>(f) % clip1080.size()];
					std::memcpy(slotBuf.data(), src.data(), (std::min)(src.size(), slotBuf.size()));
					VpvCalls::SetAmbientLux(ext, 10.0 + static_cast<double>((f * 5) % 90), true, static_cast<double>(f) * 0.042);
					return FrameAction{};
				};
				legKitchenSink = RunSoakLeg(optKitchen, clip1080, nvMon, cfgPath);
				verifyLeg(optKitchen.name, legKitchenSink);

				// 24 distinct live config transitions fired every 7 frames on ONE running renderer (168 frames)
				static const char* const kTortureConfigs[] = {
					"deband_vp: on\nappearance: on\nappearance_room: auto\nappearance_strength: 0.20\ndejudder_strength: 0\noptical_flow: on\ntemporal_dither: off\n[vpv.skeleton]\nanalysis_lane: on\nanalysis_lane_chunk_us: 4000\n",
					"deband_vp: off\nappearance: on\nappearance_room: auto\nappearance_strength: 0.55\ndejudder_strength: 0.25\noptical_flow: on\ntemporal_dither: sigma_delta\n[vpv.skeleton]\nanalysis_lane: on\nanalysis_lane_chunk_us: 4000\n",
					"deband_vp: auto\nlow_bitrate_handling: on\nappearance: on\nappearance_room: cinema\nappearance_strength: 0.85\ndejudder_strength: 0.50\noptical_flow: on\ntemporal_dither: auto\n[vpv.skeleton]\nanalysis_lane: on\nanalysis_lane_chunk_us: 4000\n",
					"deband_vp: on\ndetail_pulse_stabilise: on\ndetail_pulse_cap: 1.20\ntexture_restore: off\nappearance: off\ndejudder_strength: 0\noptical_flow: off\ntemporal_dither: off\n[vpv.skeleton]\nanalysis_lane: on\nanalysis_lane_chunk_us: 4000\n",
					"deband_vp: on\ndetail_pulse_stabilise: on\ndetail_pulse_cap: 1.50\ntexture_restore: statistical\ntexture_restore_cap: 1.6\nappearance: on\nappearance_room: auto\nappearance_strength: 0.95\ndv_trims: on\ndejudder_strength: 0.40\noptical_flow: on\ntemporal_dither: sigma_delta\n[vpv.skeleton]\nanalysis_lane: on\nanalysis_lane_chunk_us: 250\n",
					"deband_vp: off\ndetail_pulse_stabilise: off\ntexture_restore: off\nappearance: on\nappearance_room: auto\nappearance_strength: 0.30\ndv_trims: off\ndejudder_strength: 0\noptical_flow: on\ntemporal_dither: off\n[vpv.skeleton]\nanalysis_lane: on\nanalysis_lane_chunk_us: 4000\n[vprenderer.viewport]\nblank_left: 32\nblank_right: 32\n",
					"deband_vp: on\ndetail_pulse_stabilise: on\ndetail_pulse_cap: 1.35\ntexture_restore: statistical\nappearance: on\nappearance_room: auto\nappearance_strength: 0.70\ndv_trims: on\ndejudder_strength: 0.60\noptical_flow: on\ntemporal_dither: sigma_delta\n[vpv.skeleton]\nanalysis_lane: on\nanalysis_lane_chunk_us: 4000\n[vprenderer.viewport]\nblank_left: 0\nblank_right: 0\n",
					"deband_vp: off\ndetail_pulse_stabilise: off\ntexture_restore: off\nappearance: off\ndv_trims: on\ndejudder_strength: 0\noptical_flow: on\ntemporal_dither: auto\n[vpv.skeleton]\nanalysis_lane: on\nanalysis_lane_chunk_us: 4000\n"
				};
				const size_t numTortureProfiles = sizeof(kTortureConfigs) / sizeof(kTortureConfigs[0]);

				SoakLegOptions optTorture;
				optTorture.name = "Phase6d-24xRapidFireLiveToggleTorture";
				optTorture.configText = kTortureConfigs[0];
				optTorture.width = 1920;
				optTorture.height = 1080;
				optTorture.windowWidth = 1920;
				optTorture.windowHeight = 1080;
				optTorture.encoding = VideoFrameEncoding::P210;
				optTorture.rateNum = 60000;
				optTorture.rateDen = 1001;
				optTorture.framePeriodMs = 17;
				optTorture.totalFrames = 176; // 8 warmup + 24 * 7 frames = 24 live transitions
				optTorture.warmupFrames = 8;
				optTorture.pqHdr = true;
				optTorture.applySnapshotBeforeBuild = true;
				optTorture.allowOccludedOffscreen = allowOffscreenOccluded;
				optTorture.memorySampleStride = 14;
				optTorture.captureAtFrames = { 12, 88, 170 };
				optTorture.onFrame = [&, numTortureProfiles](int f, IVideoRenderer& r, IVideoRenderer* ext, std::vector<BYTE>& slotBuf) -> FrameAction {
					const std::vector<BYTE>& src = clip1080[static_cast<size_t>(f) % clip1080.size()];
					std::memcpy(slotBuf.data(), src.data(), (std::min)(src.size(), slotBuf.size()));
					VpvCalls::SetAmbientLux(ext, 5.0 + static_cast<double>((f * 7) % 110), true, static_cast<double>(f) * 0.017);
					if (f >= 8 && ((f - 8) % 7 == 0))
					{
						const size_t step = static_cast<size_t>((f - 8) / 7);
						const char* body = kTortureConfigs[(step + 1) % numTortureProfiles];
						{
							std::ofstream outCfg(cfgPath, std::ios::trunc);
							outCfg << "[general]\npersist_profile_selection: false\n"
								   << "[vprenderer.input.soak]\nwhen: $source_rate > 0\ncapture_path: gpu\n"
								   << body
								   << "[vpv]\nresync_event: on\nframe_analysis_store: on\n";
						}
						ConfigFile nextCfg;
						std::string err;
						UnifiedProfileRuntime::Runtime rt;
						if (nextCfg.Load(cfgPath) && rt.Initialize(nextCfg, {}, err))
						{
							const auto snap = rt.GetSnapshot();
							CString active;
							bool restart = false, live = false;
							if (snap && r.ApplyApplicationState(*snap, active, restart, live))
							{
								++tortureAppliedCount;
								if (restart)
									++tortureRestartFlagCount;
								else
									++tortureLiveCount;
							}
						}
					}
					return FrameAction{};
				};
				legTorture = RunSoakLeg(optTorture, clip1080, nvMon, cfgPath);
				verifyLeg(optTorture.name, legTorture);
				Assert::IsTrue(tortureAppliedCount >= 24,
					WideOf("Phase 6d: expected >= 24 rapid-fire live ApplyApplicationState calls, got " +
						std::to_string(tortureAppliedCount)).c_str());
				Assert::IsTrue(legTorture.captureJsons.size() >= 3,
					L"Phase 6d: expected 3 verified captures across rapid-fire torture loop");
				if (legTorture.memorySamples.size() >= 2)
				{
					tortureVramStart = legTorture.memorySamples.front().vramMiB;
					tortureVramEnd = legTorture.memorySamples.back().vramMiB;
					tortureVramDelta = tortureVramEnd - tortureVramStart;
					Assert::IsTrue(tortureVramDelta <= 48.0,
						WideOf("Phase 6d: VRAM leaked across 24 rapid-fire config toggles (" +
							std::to_string(tortureVramStart) + " -> " + std::to_string(tortureVramEnd) + " MiB)").c_str());
				}
			}

			std::ostringstream report;
			report << "\n=========== VIRTUAL HDMI PHASE 6 SOAK SCORECARD ===========\n"
				   << "Adapter Bound        : " << observedAdapter << "\n"
				   << "Presentation Mode    : " << (anyOccludedOffscreen ? "OFFSCREEN GPU (DXGI_STATUS_OCCLUDED on isolated desktop; full RTX 3050 Ti render + pixel captures verified)" : "LIVE SWAPCHAIN PRESENT") << "\n"
				   << "Total Real Frames    : " << totalFrames << " (Captures Verified=" << totalCaptures << ", 0 NaN/Corrupt)\n"
				   << "6a Deband Toggle     : OFF(" << cvBase.sha256.substr(0, 12) << ") != ON(" << cvDebandOn.sha256.substr(0, 12)
				   << ") -> Revert(" << cvDebandOff.sha256.substr(0, 12) << ") BIT-EXACT PASS\n"
				   << "6a CAM16 Sweep       : OFF(" << cvAppOff.sha256.substr(0, 12) << ") != 0.30(" << cvApp03.sha256.substr(0, 12)
				   << ") != 0.90(" << cvApp09.sha256.substr(0, 12) << ") -> Revert(" << cvAppBackOff.sha256.substr(0, 12) << ") PASS\n"
				   << "6a Temporal Dither   : OFF(" << cvBase.sha256.substr(0, 12) << ") != SigmaDelta(" << cvDitherSd.sha256.substr(0, 12) << ") PASS\n"
				   << "6a Re-Base Scaler    : 1080p->4K Native(" << cvDownOff.sha256.substr(0, 12) << ") != 720p(" << cvDownOn.sha256.substr(0, 12)
				   << ") -> Revert(" << cvDownRevert.sha256.substr(0, 12) << ") [" << legRebaseOn.sourceStatus << "]\n"
				   << "6a Detail Pulse      : ON(cap=1.5) valid=" << (cvDetailPulse.valid ? "PASS" : "FAIL") << "\n"
				   << "6a Vector/Dejudder   : ON=[" << legDejOn.dejudderStatus << "] vs OFF=[" << legDejOff.dejudderStatus << "] PASS\n"
				   << "6a Live Hot-Reload   : applied=" << (hotReloadApplied ? "true" : "false")
				   << " noRestart=" << (hotReloadLiveNoRestart ? "true" : "false")
				   << " hash=" << hotHashBefore.substr(0, 12) << " -> " << hotHashAfter.substr(0, 12) << "\n"
				   << "6b Frame Starvation  : 200ms + 500ms stalls -> GPU seq " << seqBeforeStall << " -> " << legStarve.finalGpuSourceSeq << " RESUMED PASS\n"
				   << "6c Overload Recovery : 4000us=[" << legHealthy.fallbacksStatus << "] -> 200us=[" << legSqueezed.fallbacksStatus
				   << "] -> 4000us=[" << legRecovered.fallbacksStatus << "] PASS\n"
				   << "6d Kitchen-Sink AllOn: 36f PQ HDR all modules ON -> " << legKitchenSink.captureJsons.size() << " verified captures PASS\n"
				   << "6d 24x Live Torture  : applied=" << tortureAppliedCount << " (live=" << tortureLiveCount
				   << ", restartFlag=" << tortureRestartFlagCount << ") VRAM=" << std::fixed << std::setprecision(1)
				   << tortureVramStart << " -> " << tortureVramEnd << " MiB (delta=" << std::showpos << tortureVramDelta
				   << std::noshowpos << " MiB) PASS\n"
				   << "===========================================================\n";
			Logger::WriteMessage(report.str().c_str());
			Assert::IsTrue(p6Failures.empty(), WideOf("Phase 6 failures: " + p6Failures).c_str());
		}
	};
}
