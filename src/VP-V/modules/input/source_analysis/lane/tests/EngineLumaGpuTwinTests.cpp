// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// refs/105 phase 5a: the GPU twin of the optical-flow producer's engine luma (VpUnit::EngineLumaGpu) is byte-identical to
// the CPU law - MotionAhead::HalfLuma8 for the half view, then the producer's quarter step (a copy of
// MotionAheadProducer.cpp Producer::Process, the `quarterBuffer` loop) - for P010, P210, v210, UYVY and HDYC on random
// bytes, odd half / quarter edges, v210 widths that end mid-group, padded strides and one 3840x2160 frame; and once
// through an EarlyUpload slot opened on a second reader device (owner O3: one upload, read by the producer's own device).
// On an own D3D11 device made on the libplacebo test fixture's adapter (WARP unless VP_TEST_GPU=hardware).
#include "pch.h"
#include <VP-V/tests/VpLutGpuTestSupport.h>
#include <VP-V/modules/input/capture/EarlyUpload.h>
#include <VP-V/modules/input/source_analysis/lane/shared/EngineLumaGpu.h>
#include <VP-V/modules/motion/estimation/MotionAheadProducer.h>

#include <d3d11_4.h>
#include <dxgi.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace VideoProcessorTest
{
	namespace
	{
		namespace ELG = VpUnit::EngineLumaGpu;

		enum class TwinFormat { P010, P210, V210, Uyvy, Hdyc };

		const char* NameOf(TwinFormat f)
		{
			switch (f)
			{
			case TwinFormat::P010: return "P010";
			case TwinFormat::P210: return "P210";
			case TwinFormat::V210: return "v210";
			case TwinFormat::Uyvy: return "UYVY";
			default: return "HDYC";
			}
		}

		ELG::Packing PackingOf(TwinFormat f)
		{
			if (f == TwinFormat::P010 || f == TwinFormat::P210) return ELG::Packing::Planar16;
			if (f == TwinFormat::V210) return ELG::Packing::V210;
			return ELG::Packing::Uyvy8;
		}

		// A random frame of the format (the luma plane / packed rows first: what the GPU twin uploads) and its source.
		struct TwinFrame
		{
			std::shared_ptr<std::vector<uint8_t>> bytes;
			AnalysisLumaSource source;
			size_t rowBytes = 0;
		};

		TwinFrame MakeFrame(TwinFormat f, int w, int h, size_t padBytes, uint32_t& seed)
		{
			auto next = [&seed]() { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; };
			TwinFrame t;
			size_t chromaBytes = 0;
			if (f == TwinFormat::P010 || f == TwinFormat::P210)
			{
				t.rowBytes = size_t(w) * 2 + padBytes;
				chromaBytes = size_t(w) * 2 * size_t(f == TwinFormat::P010 ? h / 2 : h);
			}
			else if (f == TwinFormat::V210)
				t.rowBytes = ((size_t(w) + 5) / 6) * 16 + padBytes;
			else
				t.rowBytes = size_t(w) * 2 + padBytes;
			t.bytes = std::make_shared<std::vector<uint8_t>>(t.rowBytes * size_t(h) + chromaBytes);
			for (uint8_t& b : *t.bytes) b = static_cast<uint8_t>(next() >> 7);
			AnalysisLumaSource& s = t.source;
			s.data = t.bytes->data();
			s.dataBytes = t.bytes->size();
			s.width = w;
			s.height = h;
			s.rowBytes = t.rowBytes;
			s.backingOwner = t.bytes;
			if (f == TwinFormat::P010 || f == TwinFormat::P210)
			{
				s.format = f == TwinFormat::P010 ? AnalysisLumaFormat::P010 : AnalysisLumaFormat::P210;
				s.chromaRowBytes = size_t(w) * 2;
			}
			else
			{
				s.format = AnalysisLumaFormat::NativeYuv422;
				s.encoding = f == TwinFormat::V210 ? VideoFrameEncoding::V210 :
					(f == TwinFormat::Uyvy ? VideoFrameEncoding::UYVY : VideoFrameEncoding::HDYC);
			}
			return t;
		}

		// The CPU law: HalfLuma8, then (quarter) the producer's step, copied from MotionAheadProducer.cpp Producer::Process.
		std::vector<uint8_t> CpuEngineLuma(const AnalysisLumaSource& source, bool quarter, int& outW, int& outH)
		{
			std::vector<uint8_t> lumaBuffer;
			int halfW = 0, halfH = 0;
			Assert::IsTrue(MotionAhead::HalfLuma8(source, lumaBuffer, halfW, halfH), L"HalfLuma8 refused the source");
			outW = halfW;
			outH = halfH;
			if (!quarter)
				return lumaBuffer;
			const int tw = (halfW + 1) / 2, th = (halfH + 1) / 2;
			std::vector<uint8_t> quarterBuffer(static_cast<size_t>(tw) * th);
			for (int y = 0; y < th; ++y)
			{
				const uint8_t* r0 = &lumaBuffer[static_cast<size_t>((std::min)(2 * y, halfH - 1)) * halfW];
				const uint8_t* r1 = &lumaBuffer[static_cast<size_t>((std::min)(2 * y + 1, halfH - 1)) * halfW];
				for (int x = 0; x < tw; ++x)
				{
					const int x0 = (std::min)(2 * x, halfW - 1), x1 = (std::min)(2 * x + 1, halfW - 1);
					quarterBuffer[static_cast<size_t>(y) * tw + x] = static_cast<uint8_t>((r0[x0] + r0[x1] + r1[x0] + r1[x1] + 2) / 4);
				}
			}
			outW = tw;
			outH = th;
			return quarterBuffer;
		}

		// A second D3D11 device on the fixture device's adapter (as the producer's own device is on the render adapter).
		CComPtr<ID3D11Device> DeviceOnAdapterOf(ID3D11Device* fixtureDevice)
		{
			CComPtr<IDXGIDevice> dxgiDevice;
			CComPtr<IDXGIAdapter> adapter;
			CComPtr<ID3D11Device> device;
			if (!fixtureDevice || FAILED(fixtureDevice->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) ||
				FAILED(dxgiDevice->GetAdapter(&adapter)))
				return device;
			const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
			D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &device, nullptr, nullptr);
			return device;
		}

		CComPtr<ID3D11Texture2D> RawTexture(ID3D11Device* device, const uint8_t* bytes, size_t rowBytes, int height)
		{
			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = static_cast<UINT>(rowBytes / 4);
			desc.Height = static_cast<UINT>(height);
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			desc.SampleDesc.Count = 1;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			D3D11_SUBRESOURCE_DATA init{};
			init.pSysMem = bytes;
			init.SysMemPitch = static_cast<UINT>(rowBytes);
			CComPtr<ID3D11Texture2D> tex;
			Assert::IsTrue(SUCCEEDED(device->CreateTexture2D(&desc, &init, &tex)), L"raw texture");
			return tex;
		}

		CComPtr<ID3D11Texture2D> EngineTexture(ID3D11Device* device, int w, int h)
		{
			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = static_cast<UINT>(w);
			desc.Height = static_cast<UINT>(h);
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			desc.Format = DXGI_FORMAT_R8_UNORM;
			desc.SampleDesc.Count = 1;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;   // as the producer's lumaTex reads it
			CComPtr<ID3D11Texture2D> tex;
			Assert::IsTrue(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &tex)), L"engine texture");
			return tex;
		}

		std::vector<uint8_t> ReadR8(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* tex, int w, int h)
		{
			D3D11_TEXTURE2D_DESC desc{};
			tex->GetDesc(&desc);
			desc.Usage = D3D11_USAGE_STAGING;
			desc.BindFlags = 0;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			desc.MiscFlags = 0;
			CComPtr<ID3D11Texture2D> staging;
			Assert::IsTrue(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging)), L"staging texture");
			context->CopyResource(staging, tex);
			D3D11_MAPPED_SUBRESOURCE mapped{};
			Assert::IsTrue(SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)), L"map");
			std::vector<uint8_t> out(static_cast<size_t>(w) * h);
			for (int y = 0; y < h; ++y)
				std::memcpy(&out[static_cast<size_t>(y) * w], static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch, size_t(w));
			context->Unmap(staging, 0);
			return out;
		}

		std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }

		std::string FirstDifference(const std::vector<uint8_t>& want, const std::vector<uint8_t>& got, int w)
		{
			for (size_t i = 0; i < want.size() && i < got.size(); ++i)
				if (want[i] != got[i])
				{
					char line[160];
					std::snprintf(line, sizeof(line), " first difference at (%d, %d): cpu %u gpu %u", int(i % size_t(w)), int(i / size_t(w)),
						unsigned(want[i]), unsigned(got[i]));
					return line;
				}
			return want.size() == got.size() ? std::string() : std::string(" size differs");
		}
	}

	TEST_CLASS(EngineLumaGpuTwinTests)
	{
	public:
		TEST_METHOD(GpuTwinEqualsTheCpuEngineLumaForEveryTwinFormat)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			CComPtr<ID3D11Device> device = DeviceOnAdapterOf(fixture.Device());
			Assert::IsNotNull(device.p, L"no own device on the fixture adapter");
			CComPtr<ID3D11DeviceContext> context;
			device->GetImmediateContext(&context);
			ELG::Kernel kernel;
			Assert::IsTrue(kernel.Open(device), Wide(kernel.LastError()).c_str());

			struct Case { TwinFormat f; int w, h; size_t pad; };
			const Case cases[] = {
				// P010: even heights only (production IsValid); odd half / quarter grids, small edges, a padded stride.
				{ TwinFormat::P010, 1918, 1080, 0 }, { TwinFormat::P010, 1916, 1078, 0 }, { TwinFormat::P010, 2, 2, 0 },
				{ TwinFormat::P010, 6, 4, 0 }, { TwinFormat::P010, 1278, 718, 64 },
				// 4:2:2: odd heights too.
				{ TwinFormat::P210, 1918, 1081, 0 }, { TwinFormat::P210, 1916, 1078, 0 }, { TwinFormat::P210, 2, 1, 0 },
				{ TwinFormat::P210, 4, 3, 32 },
				// v210: widths ending mid-group (W % 6 = 4, 2), whole groups, the DeckLink 128-byte stride padding, and 4K.
				{ TwinFormat::V210, 1918, 1081, 0 }, { TwinFormat::V210, 1916, 1078, 0 }, { TwinFormat::V210, 1920, 1080, 0 },
				{ TwinFormat::V210, 2, 1, 0 }, { TwinFormat::V210, 8, 3, 0 }, { TwinFormat::V210, 1280, 721, 32 },   // 3456 = ((1280 + 47) / 48) * 128
				{ TwinFormat::V210, 3840, 2160, 0 },
				// UYVY / HDYC (8-bit, << 2).
				{ TwinFormat::Uyvy, 1918, 1081, 0 }, { TwinFormat::Uyvy, 1916, 1078, 0 }, { TwinFormat::Uyvy, 2, 1, 0 },
				{ TwinFormat::Uyvy, 720, 487, 16 }, { TwinFormat::Hdyc, 1918, 1081, 0 }, { TwinFormat::Hdyc, 6, 5, 0 },
			};
			uint32_t seed = 0x5a17c0deu;
			int checked = 0;
			for (const Case& c : cases)
			{
				TwinFrame frame = MakeFrame(c.f, c.w, c.h, c.pad, seed);
				Assert::IsTrue(frame.source.IsValid(), Wide(std::string(NameOf(c.f)) + ": the test source is not valid").c_str());
				CComPtr<ID3D11Texture2D> raw = RawTexture(device, frame.bytes->data(), frame.rowBytes, c.h);
				for (int q = 0; q < 2; ++q)
				{
					const bool quarter = q == 1;
					int cw = 0, ch = 0;
					const std::vector<uint8_t> want = CpuEngineLuma(frame.source, quarter, cw, ch);
					const int gw = static_cast<int>(ELG::OutDim(uint32_t(c.w), quarter)), gh = static_cast<int>(ELG::OutDim(uint32_t(c.h), quarter));
					char label[128];
					std::snprintf(label, sizeof(label), "%s %dx%d pad %zu %s", NameOf(c.f), c.w, c.h, c.pad, quarter ? "quarter" : "half");
					Assert::IsTrue(cw == gw && ch == gh, Wide(std::string(label) + ": the engine size differs").c_str());
					CComPtr<ID3D11Texture2D> out = EngineTexture(device, gw, gh);
					Assert::IsTrue(kernel.Run(context, raw, PackingOf(c.f), uint32_t(c.w), uint32_t(c.h), quarter, out),
						Wide(std::string(label) + ": " + kernel.LastError()).c_str());
					const std::vector<uint8_t> got = ReadR8(device, context, out, gw, gh);
					Assert::IsTrue(got == want, Wide(std::string(label) + ": GPU twin differs from the CPU law" + FirstDifference(want, got, gw)).c_str());
					++checked;
				}
			}
			Logger::WriteMessage(("EngineLumaGpu twin: " + std::to_string(checked) + " format x size x view cases byte-identical\n").c_str());
		}

		TEST_METHOD(GpuTwinRefusesWhatItCannotRead)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			CComPtr<ID3D11Device> device = DeviceOnAdapterOf(fixture.Device());
			Assert::IsNotNull(device.p);
			CComPtr<ID3D11DeviceContext> context;
			device->GetImmediateContext(&context);
			ELG::Kernel kernel;
			std::vector<uint8_t> bytes(64 * 4 * 8, 0);
			CComPtr<ID3D11Texture2D> raw = RawTexture(device, bytes.data(), 64 * 4, 8);
			CComPtr<ID3D11Texture2D> out = EngineTexture(device, 64, 4);
			Assert::IsFalse(kernel.Run(context, raw, ELG::Packing::Planar16, 128, 8, false, out), L"not open: refused");
			Assert::IsTrue(kernel.Open(device), Wide(kernel.LastError()).c_str());
			Assert::IsTrue(kernel.Run(context, raw, ELG::Packing::Planar16, 128, 8, false, out), Wide(kernel.LastError()).c_str());
			Assert::IsFalse(kernel.Run(context, raw, ELG::Packing::Planar16, 130, 8, false, out), L"row too short: refused");
			Assert::IsFalse(kernel.Run(context, raw, ELG::Packing::Planar16, 128, 9, false, out), L"too few rows: refused");
			CComPtr<ID3D11Texture2D> narrowOut = EngineTexture(device, 32, 4);
			Assert::IsFalse(kernel.Run(context, raw, ELG::Packing::Planar16, 128, 8, false, narrowOut), L"output too small: refused");
			Assert::IsFalse(kernel.Run(context, raw, static_cast<ELG::Packing>(7u), 128, 8, false, out), L"unknown packing: refused");
			Assert::IsFalse(kernel.Run(context, out, ELG::Packing::Planar16, 2, 2, false, raw), L"swapped textures: refused");
			Assert::IsTrue(ELG::OutDim(1918, false) == 959 && ELG::OutDim(1918, true) == 480 && ELG::OutDim(1, true) == 1);
			Assert::IsTrue(ELG::MinRowBytes(ELG::Packing::V210, 1918) == 320 * 16 && ELG::MinRowBytes(ELG::Packing::Uyvy8, 6) == 12);
		}

		TEST_METHOD(GpuTwinReadsTheOneEarlyUploadOnTheReaderDevice)
		{
			// Owner O3: the frame is uploaded once (EarlyUpload, on its own upload device), the render device and the
			// producer's device both read that slot; the producer's twin on its own device equals the CPU law.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			EarlyUpload early;
			if (!early.Open(fixture.Device(), fixture.Gpu(), 2))
			{
				Logger::WriteMessage(("EarlyUpload not available on this device: " + early.LastError() + "\n").c_str());
				return;
			}
			CComPtr<ID3D11Device> device = DeviceOnAdapterOf(fixture.Device());
			Assert::IsNotNull(device.p);
			CComPtr<ID3D11DeviceContext> context;
			device->GetImmediateContext(&context);
			Assert::IsTrue(early.AttachReader(device), Wide(early.LastError()).c_str());
			ELG::Kernel kernel;
			Assert::IsTrue(kernel.Open(device), Wide(kernel.LastError()).c_str());
			uint32_t seed = 0x0dd5eedu;
			const TwinFormat formats[] = { TwinFormat::V210, TwinFormat::P010 };
			for (TwinFormat f : formats)
			{
				TwinFrame frame = MakeFrame(f, 1918, 1080, 0, seed);
				EarlyUpload::SlotHandle slot = early.Upload(frame.bytes->data(), frame.rowBytes, 1080);
				Assert::IsTrue(slot != nullptr, L"a free slot");
				Assert::IsNotNull(early.Acquire(*slot), Wide(early.LastError()).c_str());   // the render side reads it too
				early.Used(*slot);
				ID3D11Texture2D* raw = early.AcquireReader(*slot);
				Assert::IsNotNull(raw, Wide(early.LastError()).c_str());
				int cw = 0, ch = 0;
				const std::vector<uint8_t> want = CpuEngineLuma(frame.source, true, cw, ch);
				CComPtr<ID3D11Texture2D> out = EngineTexture(device, cw, ch);
				Assert::IsTrue(kernel.Run(context, raw, PackingOf(f), 1918, 1080, true, out), Wide(kernel.LastError()).c_str());
				early.UsedReader(*slot);
				const std::vector<uint8_t> got = ReadR8(device, context, out, cw, ch);
				Assert::IsTrue(got == want, Wide(std::string(NameOf(f)) + " via EarlyUpload: GPU twin differs" + FirstDifference(want, got, cw)).c_str());
			}
			early.DetachReader();
			const EarlyUpload::Stats stats = early.GetStats();
			Assert::IsTrue(stats.failures == 0 && stats.readerDetachTimeouts == 0, L"stats");
			early.Close();
		}
	};
}
