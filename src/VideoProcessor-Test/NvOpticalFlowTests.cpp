#include "pch.h"
#include "CppUnitTest.h"

#include <vprenderer/NvOpticalFlow.h>

#include <dxgi1_2.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// The NVIDIA optical flow engine's self-check (owner 2026-10-14): a textured
// picture moved by a known amount, through the engine in both directions,
// at 1080p and 4K on each preset; the vectors and the time. Hardware only
// (VP_TEST_GPU=hardware) and NVIDIA only; elsewhere it reports and passes.
namespace
{
	bool OnHardware()
	{
		char* v = nullptr;
		size_t n = 0;
		const bool on = _dupenv_s(&v, &n, "VP_TEST_GPU") == 0 && v && std::string(v) == "hardware";
		std::free(v);
		return on;
	}

	CComPtr<ID3D11Texture2D> Texture(ID3D11Device* device, int w, int h, DXGI_FORMAT format, const void* data, UINT pitch)
	{
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = static_cast<UINT>(w);
		desc.Height = static_cast<UINT>(h);
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = format;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		D3D11_SUBRESOURCE_DATA init{ data, pitch, 0 };
		CComPtr<ID3D11Texture2D> tex;
		device->CreateTexture2D(&desc, data ? &init : nullptr, &tex);
		return tex;
	}

	// The flow texture's vectors in pixels (S10.5 fixed point).
	std::vector<std::pair<float, float>> Read(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* flow)
	{
		D3D11_TEXTURE2D_DESC desc{};
		flow->GetDesc(&desc);
		desc.Usage = D3D11_USAGE_STAGING;
		desc.BindFlags = 0;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		CComPtr<ID3D11Texture2D> staging;
		std::vector<std::pair<float, float>> v;
		if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging)))
			return v;
		context->CopyResource(staging, flow);
		D3D11_MAPPED_SUBRESOURCE map{};
		if (FAILED(context->Map(staging, 0, D3D11_MAP_READ, 0, &map)))
			return v;
		for (UINT y = 0; y < desc.Height; ++y)
		{
			const int16_t* row = reinterpret_cast<const int16_t*>(static_cast<const uint8_t*>(map.pData) + y * map.RowPitch);
			for (UINT x = 0; x < desc.Width; ++x)
				v.emplace_back(row[x * 2] / 32.0f, row[x * 2 + 1] / 32.0f);
		}
		context->Unmap(staging, 0);
		return v;
	}

	int ClampInt(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

	float Median(std::vector<float> values)
	{
		if (values.empty()) return 0.0f;
		std::nth_element(values.begin(), values.begin() + values.size() / 2, values.end());
		return values[values.size() / 2];
	}
}

namespace Tests
{
	TEST_CLASS(NvOpticalFlowTests)
	{
	public:
		TEST_METHOD(EngineSelfCheckFindsAKnownPanQuickly)
		{
			if (!OnHardware())
			{
				Logger::WriteMessage(L"optical flow self-check: VP_TEST_GPU=hardware only; not run");
				return;
			}
			// The NVIDIA adapter.
			CComPtr<IDXGIFactory1> factory;
			Assert::IsTrue(SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))));
			CComPtr<IDXGIAdapter1> nvidia;
			for (UINT i = 0; !nvidia; ++i)
			{
				CComPtr<IDXGIAdapter1> adapter;
				if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND)
					break;
				DXGI_ADAPTER_DESC1 desc{};
				adapter->GetDesc1(&desc);
				if (desc.VendorId == 0x10DE)
					nvidia = adapter;
			}
			if (!nvidia)
			{
				Logger::WriteMessage(L"optical flow self-check: no NVIDIA GPU; not run");
				return;
			}
			CComPtr<ID3D11Device> device;
			CComPtr<ID3D11DeviceContext> context;
			Assert::IsTrue(SUCCEEDED(D3D11CreateDevice(nvidia, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
				D3D11_SDK_VERSION, &device, nullptr, &context)), L"D3D11 device on the NVIDIA GPU");

			const int dx = 8, dy = 3;   // the pan, pixels
			struct Size { int w, h; };
			for (Size size : { Size{ 1920, 1080 }, Size{ 3840, 2160 } })
			{
				// A textured picture (smoothed noise) and the same moved by (dx, dy).
				std::vector<uint8_t> a(static_cast<size_t>(size.w) * size.h), b(a.size());
				uint32_t seed = 12345u;
				std::vector<uint8_t> noise(a.size());
				for (uint8_t& p : noise) { seed = seed * 1664525u + 1013904223u; p = static_cast<uint8_t>(seed >> 24); }
				for (int y = 0; y < size.h; ++y)
					for (int x = 0; x < size.w; ++x)
					{
						int s = 0;
						for (int k = -1; k <= 1; ++k)
							for (int j = -1; j <= 1; ++j)
								s += noise[static_cast<size_t>(ClampInt(y + k, 0, size.h - 1)) * size.w + ClampInt(x + j, 0, size.w - 1)];
						a[static_cast<size_t>(y) * size.w + x] = static_cast<uint8_t>(s / 9);
					}
				for (int y = 0; y < size.h; ++y)
					for (int x = 0; x < size.w; ++x)
						b[static_cast<size_t>(y) * size.w + x] = a[static_cast<size_t>(ClampInt(y - dy, 0, size.h - 1)) * size.w +
							ClampInt(x - dx, 0, size.w - 1)];
				auto texA = Texture(device, size.w, size.h, NvOpticalFlow::Engine::InputFormat(), a.data(), static_cast<UINT>(size.w));
				auto texB = Texture(device, size.w, size.h, NvOpticalFlow::Engine::InputFormat(), b.data(), static_cast<UINT>(size.w));
				const int gw = size.w / NvOpticalFlow::Engine::Grid, gh = size.h / NvOpticalFlow::Engine::Grid;
				auto fwd = Texture(device, gw, gh, NvOpticalFlow::Engine::FlowFormat(), nullptr, 0);
				auto bwd = Texture(device, gw, gh, NvOpticalFlow::Engine::FlowFormat(), nullptr, 0);
				auto cost = Texture(device, gw, gh, NvOpticalFlow::Engine::CostFormat(), nullptr, 0);
				auto bwdCost = Texture(device, gw, gh, NvOpticalFlow::Engine::CostFormat(), nullptr, 0);
				Assert::IsTrue(texA && texB && fwd && bwd && cost && bwdCost, L"textures");

				const NvOpticalFlow::Preset presets[] = { NvOpticalFlow::Preset::Fast, NvOpticalFlow::Preset::Medium, NvOpticalFlow::Preset::Slow };
				const char* names[] = { "fast", "medium", "slow" };
				for (int p = 0; p < 3; ++p)
				{
					NvOpticalFlow::Engine engine;
					if (!engine.Open(device, context, size.w, size.h, presets[p]))
					{
						Logger::WriteMessage(("optical flow: not available: " + engine.LastError()).c_str());
						return;   // a GPU without the engine: reported, not a failure
					}
					// Warm up, then time 10 pairs.
					double total = 0.0, best = 1e9;
					for (int r = 0; r < 12; ++r)
					{
						NvOpticalFlow::Result result;
						const bool ran = engine.Run(texA, texB, fwd, bwd, cost, bwdCost, &result, false);
						const std::string why = engine.LastError();
						Assert::IsTrue(ran, std::wstring(why.begin(), why.end()).c_str());
						if (r >= 2) { total += result.gpuMs; best = std::min(best, result.gpuMs); }
					}
					std::vector<float> fx, fy, bx, by;
					for (const auto& v : Read(device, context, fwd)) { fx.push_back(v.first); fy.push_back(v.second); }
					for (const auto& v : Read(device, context, bwd)) { bx.push_back(v.first); by.push_back(v.second); }
					char line[256];
					std::snprintf(line, sizeof(line), "optical flow %dx%d %s: both directions %.2f ms mean, %.2f best; forward median (%.2f, %.2f), backward (%.2f, %.2f)",
						size.w, size.h, names[p], total / 10.0, best, Median(fx), Median(fy), Median(bx), Median(by));
					Logger::WriteMessage(line);
					Assert::AreEqual(static_cast<float>(dx), Median(fx), 0.5f, L"forward x");
					Assert::AreEqual(static_cast<float>(dy), Median(fy), 0.5f, L"forward y");
					Assert::AreEqual(static_cast<float>(-dx), Median(bx), 0.5f, L"backward x");
					Assert::AreEqual(static_cast<float>(-dy), Median(by), 0.5f, L"backward y");
				}
			}
		}
	};
}
