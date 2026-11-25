// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

#include <VP-V/modules/motion/estimation/MotionAheadProducer.h>
#include <VP-V/modules/motion/estimation/R6LumaPass.h>
#include <VP-V/modules/motion/estimation/R6PipelineOverlap.h>
#include <VP-V/modules/motion/estimation/global_model/GlobalModelFit.h>

#include <atlbase.h>
#include <d3d11.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	namespace
	{
		// Reference two-pass U3 HalfLuma8 implementation for planar-16 (exact pre-R6 law).
		void ReferencePlanar16HalfLuma8(const uint16_t* plane, int w, int h, std::vector<uint8_t>& out, int& halfW, int& halfH)
		{
			halfW = (w + 1) / 2;
			halfH = (h + 1) / 2;
			out.assign(static_cast<size_t>(halfW) * static_cast<size_t>(halfH), 0);
			for (int y = 0; y < halfH; ++y)
			{
				const uint16_t* r0 = plane + static_cast<size_t>(VpUnit::HalfLuma::Tap0(y, h)) * static_cast<size_t>(w);
				const uint16_t* r1 = plane + static_cast<size_t>(VpUnit::HalfLuma::Tap1(y, h)) * static_cast<size_t>(w);
				for (int x = 0; x < halfW; ++x)
				{
					const int x0 = VpUnit::HalfLuma::Tap0(x, w);
					const int x1 = VpUnit::HalfLuma::Tap1(x, w);
					out[static_cast<size_t>(y) * static_cast<size_t>(halfW) + static_cast<size_t>(x)] =
						VpUnit::HalfLuma::FinishEngine8(VpUnit::HalfLuma::Box4(
							r0[x0] >> 6, r0[x1] >> 6, r1[x0] >> 6, r1[x1] >> 6));
				}
			}
		}

		std::shared_ptr<MotionAhead::Flow> MakeSyntheticFlow(int halfW, int halfH, int16_t vx, int16_t vy)
		{
			auto f = std::make_shared<MotionAhead::Flow>();
			f->halfW = halfW;
			f->halfH = halfH;
			f->gridW = (halfW + 3) / 4;
			f->gridH = (halfH + 3) / 4;
			const size_t cells = static_cast<size_t>(f->gridW) * static_cast<size_t>(f->gridH);
			f->forward.resize(cells * 2);
			f->backward.resize(cells * 2);
			f->forwardCost.assign(cells, 4u);
			f->backwardCost.assign(cells, 4u);
			for (size_t i = 0; i < cells; ++i)
			{
				f->forward[i * 2] = vx;
				f->forward[i * 2 + 1] = vy;
				f->backward[i * 2] = static_cast<int16_t>(-vx);
				f->backward[i * 2 + 1] = static_cast<int16_t>(-vy);
			}
			return f;
		}
	}

	TEST_CLASS(R6ThroughputTests)
	{
	public:
		TEST_METHOD(FusedPlanar16HalfLuma8IsBitIdenticalToReferenceEvenAndOddSizes)
		{
			// 1. Direct row-kernel check across both odd and even dimensions (including 1x1 trailing-column replication).
			const int rowSizes[][2] = { { 1, 1 }, { 2, 2 }, { 193, 117 }, { 320, 180 }, { 513, 289 } };
			for (const auto& sz : rowSizes)
			{
				const int w = sz[0], h = sz[1];
				std::vector<uint16_t> plane(static_cast<size_t>(w) * static_cast<size_t>(h));
				for (size_t i = 0; i < plane.size(); ++i)
					plane[i] = static_cast<uint16_t>(((i * 2654435761u) >> 16) & 0xFFFFu);
				plane.front() = 0x0000u;
				plane.back() = 0xFFFFu;

				std::vector<uint8_t> ref;
				int refW = 0, refH = 0;
				ReferencePlanar16HalfLuma8(plane.data(), w, h, ref, refW, refH);

				std::vector<uint8_t> direct(static_cast<size_t>(refW) * static_cast<size_t>(refH), 0);
				for (int y = 0; y < refH; ++y)
				{
					const uint16_t* r0 = plane.data() + static_cast<size_t>(VpUnit::HalfLuma::Tap0(y, h)) * static_cast<size_t>(w);
					const uint16_t* r1 = plane.data() + static_cast<size_t>(VpUnit::HalfLuma::Tap1(y, h)) * static_cast<size_t>(w);
					MotionAhead::R6::FusedPlanar16HalfLuma8Row(r0, r1, w, refW, &direct[static_cast<size_t>(y) * static_cast<size_t>(refW)]);
				}
				Assert::AreEqual(0, std::memcmp(ref.data(), direct.data(), ref.size()),
					L"R6 FusedPlanar16HalfLuma8Row must be bit-identical on odd and even dimensions");
			}

			// 2. End-to-end MotionAhead::HalfLuma8 check on valid P010 frames (even 4:2:0 dimensions, including odd halfW/halfH).
			const int p010Sizes[][2] = { { 2, 2 }, { 320, 180 }, { 382, 214 }, { 514, 290 } };
			for (const auto& sz : p010Sizes)
			{
				const int w = sz[0], h = sz[1];
				std::vector<uint16_t> plane(static_cast<size_t>(w) * static_cast<size_t>(h));
				for (size_t i = 0; i < plane.size(); ++i)
					plane[i] = static_cast<uint16_t>(((i * 2654435761u) >> 16) & 0xFFFFu);
				plane.front() = 0x0000u;
				plane.back() = 0xFFFFu;

				std::vector<uint8_t> ref;
				int refW = 0, refH = 0;
				ReferencePlanar16HalfLuma8(plane.data(), w, h, ref, refW, refH);

				const size_t lumaRowBytes = static_cast<size_t>(w) * sizeof(uint16_t);
				const size_t chromaRows = static_cast<size_t>((h + 1) / 2);
				std::vector<uint8_t> p010Buf(lumaRowBytes * static_cast<size_t>(h) + lumaRowBytes * chromaRows, 0);
				std::memcpy(p010Buf.data(), plane.data(), plane.size() * sizeof(uint16_t));

				AnalysisLumaSource src;
				src.data = p010Buf.data();
				src.dataBytes = p010Buf.size();
				src.width = w;
				src.height = h;
				src.rowBytes = lumaRowBytes;
				src.chromaRowBytes = lumaRowBytes;
				src.format = AnalysisLumaFormat::P010;

				std::vector<uint8_t> fast;
				int fastW = 0, fastH = 0;
				Assert::IsTrue(MotionAhead::HalfLuma8(src, fast, fastW, fastH));
				Assert::AreEqual(refW, fastW);
				Assert::AreEqual(refH, fastH);
				Assert::AreEqual(ref.size(), fast.size());
				Assert::AreEqual(0, std::memcmp(ref.data(), fast.data(), ref.size()),
					L"R6 fused planar-16 HalfLuma8 must be bit-identical to U3 reference");
			}
		}

		TEST_METHOD(HlslShaderExactIntegerMirrorMatchesCpuHalfAndQuarterBitIdentically)
		{
			// Verify the HLSL shaders exist and declare exact integer arithmetic.
			const std::string halfHlsl(MotionAhead::R6::kR6HalfLumaHlsl);
			const std::string qtrHlsl(MotionAhead::R6::kR6QuarterLumaHlsl);
			Assert::IsTrue(halfHlsl.find("Texture2D<uint>") != std::string::npos);
			Assert::IsTrue(halfHlsl.find(">> 6u") != std::string::npos);
			Assert::IsTrue(halfHlsl.find(">> 4u") != std::string::npos);
			Assert::IsTrue(qtrHlsl.find("+ 2u) >> 2u") != std::string::npos);

			// Sweep representative 16-bit codes across full 0..65535 range.
			const uint16_t probes[] = { 0u, 63u, 64u, 1024u, 4096u, 32768u, 65472u, 65535u };
			for (uint16_t a : probes)
				for (uint16_t b : probes)
					for (uint16_t c : probes)
						for (uint16_t d : probes)
						{
							const uint8_t cpu = VpUnit::HalfLuma::FinishEngine8(
								VpUnit::HalfLuma::Box4(a >> 6, b >> 6, c >> 6, d >> 6));
							const uint8_t gpuMirror = MotionAhead::R6::HlslHalfTexelMirror(a, b, c, d);
							Assert::AreEqual(cpu, gpuMirror, L"HLSL half-luma mirror matches CPU bit-for-bit");
						}

			const uint8_t qProbes[] = { 0u, 1u, 2u, 3u, 127u, 128u, 254u, 255u };
			for (uint8_t a : qProbes)
				for (uint8_t b : qProbes)
					for (uint8_t c : qProbes)
						for (uint8_t d : qProbes)
						{
							Assert::AreEqual(VpUnit::HalfLuma::QuarterEngine8(a, b, c, d),
								MotionAhead::R6::HlslQuarterTexelMirror(a, b, c, d),
								L"HLSL quarter-luma mirror matches CPU bit-for-bit");
						}
		}

		TEST_METHOD(R6ExtrasWorkerPreservesOrderAndMatchesSynchronousFlowGlobalsAndExtrasBitIdentically)
		{
			MotionAhead::R6::ExtrasWorker worker;
			Assert::IsTrue(worker.Start(4));
			Assert::IsTrue(worker.Running());

			std::mutex orderMutex;
			std::vector<uint64_t> publishedOrder;
			std::vector<std::shared_ptr<MotionAhead::Flow>> asyncFlows(8);
			std::vector<std::shared_ptr<MotionAhead::Flow>> syncFlows(8);

			for (uint64_t i = 0; i < 8; ++i)
			{
				const int16_t vx = static_cast<int16_t>((i + 1) * 64);
				const int16_t vy = static_cast<int16_t>(-static_cast<int>(i) * 32);
				auto fSync = MakeSyntheticFlow(320, 180, vx, vy);
				MotionAhead::FlowGlobals(*fSync, true);
				MotionAhead::FlowExtras(*fSync);
				syncFlows[i] = fSync;

				auto fAsync = MakeSyntheticFlow(320, 180, vx, vy);
				asyncFlows[i] = fAsync;
				worker.Submit([fAsync, i, &orderMutex, &publishedOrder]() {
					MotionAhead::FlowGlobals(*fAsync, true);
					MotionAhead::FlowExtras(*fAsync);
					std::lock_guard<std::mutex> lock(orderMutex);
					publishedOrder.push_back(i);
				});
			}
			worker.Flush();
			worker.Stop();
			Assert::IsFalse(worker.Running());

			Assert::AreEqual(size_t(8), publishedOrder.size());
			for (uint64_t i = 0; i < 8; ++i)
			{
				Assert::AreEqual(i, publishedOrder[i], L"R6 extras worker preserves strict FIFO publish order");
				Assert::AreEqual(0, std::memcmp(syncFlows[i]->global, asyncFlows[i]->global, sizeof(syncFlows[i]->global)));
				Assert::AreEqual(0, std::memcmp(syncFlows[i]->second, asyncFlows[i]->second, sizeof(syncFlows[i]->second)));
				Assert::AreEqual(syncFlows[i]->costWeight.size(), asyncFlows[i]->costWeight.size());
				Assert::AreEqual(0, std::memcmp(syncFlows[i]->costWeight.data(), asyncFlows[i]->costWeight.data(),
					syncFlows[i]->costWeight.size() * sizeof(float)));
				Assert::AreEqual(syncFlows[i]->fbError.size(), asyncFlows[i]->fbError.size());
				Assert::AreEqual(0, std::memcmp(syncFlows[i]->fbError.data(), asyncFlows[i]->fbError.data(),
					syncFlows[i]->fbError.size() * sizeof(float)));
			}

			// Submitting when stopped runs synchronously and counts the fallback visibly (never drops a pair).
			bool ranWhenStopped = false;
			Assert::IsFalse(worker.Submit([&ranWhenStopped]() { ranWhenStopped = true; }));
			Assert::IsTrue(ranWhenStopped, L"stopped worker executes synchronously instead of dropping");
			Assert::IsTrue(worker.Stats().syncFallbacks >= 1);
		}

		TEST_METHOD(DefaultR6OptInFlagsAreOffAndCpuLabelsRemainsTrue)
		{
			// Plan-CE-k3-k10.md s2 0a: R7(c) cpuLabels = false is NOT taken; leave it true.
			CameraModel::Config fitCfg;
			Assert::IsTrue(fitCfg.cpuLabels, L"Config::cpuLabels must remain true per plan");

			MotionAhead::Producer producer;
			Assert::IsFalse(producer.R6AsyncExtras(), L"R6 async extras defaults to OFF");
			Assert::IsTrue(producer.R6StatusLine().find("off") != std::string::npos,
				L"default-off R6 status line is explicit and visible");
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(HlslHalfAndQuarterShadersCompileAndMatchCpuOnWarp)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(HlslHalfAndQuarterShadersCompileAndMatchCpuOnWarp)
		{
			CComPtr<ID3D11Device> device;
			CComPtr<ID3D11DeviceContext> context;
			const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
			HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
				&device, nullptr, &context);
			if (FAILED(hr))
			{
				hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels + 1, 1, D3D11_SDK_VERSION,
					&device, nullptr, &context);
			}
			Assert::IsTrue(SUCCEEDED(hr) && device && context,
				L"D3D11 WARP device creation must succeed");

			auto compileCs = [&](const char* source, const char* name, CComPtr<ID3D11ComputeShader>& shaderOut) {
				CComPtr<ID3DBlob> code;
				CComPtr<ID3DBlob> errors;
				const HRESULT cHr = D3DCompile(source, std::strlen(source), name, nullptr, nullptr, "main", "cs_5_0",
					D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
				if (FAILED(cHr) || !code)
				{
					std::wstring msg = L"D3DCompile failed for ";
					for (const char* p = name; *p; ++p)
						msg.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*p)));
					if (errors && errors->GetBufferPointer() && errors->GetBufferSize() > 0)
					{
						msg += L": ";
						const char* errText = static_cast<const char*>(errors->GetBufferPointer());
						const size_t errLen = errors->GetBufferSize();
						for (size_t i = 0; i < errLen && errText[i] != '\0'; ++i)
							msg.push_back(static_cast<wchar_t>(static_cast<unsigned char>(errText[i])));
					}
					Assert::Fail(msg.c_str());
				}
				Assert::IsTrue(SUCCEEDED(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shaderOut)),
					L"CreateComputeShader must succeed on D3D11 WARP");
				Assert::IsNotNull(shaderOut.p);
			};

			CComPtr<ID3D11ComputeShader> halfCs;
			CComPtr<ID3D11ComputeShader> quarterCs;
			compileCs(MotionAhead::R6::kR6HalfLumaHlsl, "kR6HalfLumaHlsl", halfCs);
			compileCs(MotionAhead::R6::kR6QuarterLumaHlsl, "kR6QuarterLumaHlsl", quarterCs);

			struct HlslParams
			{
				uint32_t srcW;
				uint32_t srcH;
				uint32_t dstW;
				uint32_t dstH;
			};
			static_assert(sizeof(HlslParams) == 16, "HLSL Params cbuffer must be 16 bytes");

			D3D11_BUFFER_DESC cbDesc{};
			cbDesc.ByteWidth = sizeof(HlslParams);
			cbDesc.Usage = D3D11_USAGE_DEFAULT;
			cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			CComPtr<ID3D11Buffer> paramsBuf;
			Assert::IsTrue(SUCCEEDED(device->CreateBuffer(&cbDesc, nullptr, &paramsBuf)),
				L"CreateBuffer(cbuffer) must succeed");

			auto readBackR8 = [&](ID3D11Texture2D* gpuTex, int rw, int rh, std::vector<uint8_t>& out) {
				D3D11_TEXTURE2D_DESC stDesc{};
				stDesc.Width = static_cast<UINT>(rw);
				stDesc.Height = static_cast<UINT>(rh);
				stDesc.MipLevels = 1;
				stDesc.ArraySize = 1;
				stDesc.Format = DXGI_FORMAT_R8_UNORM;
				stDesc.SampleDesc.Count = 1;
				stDesc.Usage = D3D11_USAGE_STAGING;
				stDesc.BindFlags = 0;
				stDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
				CComPtr<ID3D11Texture2D> staging;
				Assert::IsTrue(SUCCEEDED(device->CreateTexture2D(&stDesc, nullptr, &staging)),
					L"CreateTexture2D(staging) must succeed");
				context->CopyResource(staging, gpuTex);
				D3D11_MAPPED_SUBRESOURCE mapped{};
				Assert::IsTrue(SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)),
					L"Map(staging) must succeed");
				out.assign(static_cast<size_t>(rw) * static_cast<size_t>(rh), 0);
				for (int y = 0; y < rh; ++y)
				{
					std::memcpy(&out[static_cast<size_t>(y) * static_cast<size_t>(rw)],
						static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch,
						static_cast<size_t>(rw));
				}
				context->Unmap(staging, 0);
			};

			// Test both:
			//   1. Valid even 4:2:0 P010 dimensions (66x50) whose half grid (33x25) is odd on both axes ->
			//      compares directly against MotionAhead::HalfLuma8 and exercises quarter odd-edge replication.
			//   2. Odd source dimensions (35x27) -> exercises half-luma odd-edge replication & non-16-aligned dispatch.
			const int sizes[][2] = { { 66, 50 }, { 35, 27 } };
			for (const auto& sz : sizes)
			{
				const int w = sz[0];
				const int h = sz[1];
				const int halfW = (w + 1) / 2;
				const int halfH = (h + 1) / 2;
				const int qW = (halfW + 1) / 2;
				const int qH = (halfH + 1) / 2;

				// Deterministic full-range 0..65535 P010 luma image with explicit rounding-boundary probes.
				std::vector<uint16_t> plane(static_cast<size_t>(w) * static_cast<size_t>(h));
				for (size_t i = 0; i < plane.size(); ++i)
					plane[i] = static_cast<uint16_t>(((i * 2654435761u) >> 16) & 0xFFFFu);
				auto setPx = [&](int x, int y, uint16_t v) {
					plane[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)] = v;
				};
				// Quad (0,0): low-6-bit P010 padding must be discarded before summing (>> 6 == 0).
				setPx(0, 0, 0x0000u); setPx(1, 0, 0x003Fu);
				setPx(0, 1, 0x003Fu); setPx(1, 1, 0x003Fu);
				// Quad (2,0): exact (3+3+3+4)>>4 == 0 vs naive unshifted (255*3+319)>>10 == 1.
				setPx(2, 0, 0x00FFu); setPx(3, 0, 0x00FFu);
				setPx(2, 1, 0x00FFu); setPx(3, 1, 0x013Fu);
				// Quad (0,2): exact (4+4+0+0)>>4 == 0 vs naive rounded (8+8)>>4 == 1.
				setPx(0, 2, 0x0100u); setPx(1, 2, 0x0100u);
				setPx(0, 3, 0x0000u); setPx(1, 3, 0x0000u);
				// Quad (2,2): max-code saturation (0xFFFF / 0xFFC0 -> 255).
				setPx(2, 2, 0xFFFFu); setPx(3, 2, 0xFFC0u);
				setPx(2, 3, 0xFFFFu); setPx(3, 3, 0xFFC0u);
				// 4x4 block at (4..7, 0..3) produces half-luma (1, 1, 0, 0) -> quarter tie (1+1+0+0+2)>>2 == 1
				// vs naive truncating (1+1+0+0)>>2 == 0.
				for (int y = 0; y < 2; ++y)
					for (int x = 4; x < 8; ++x)
						setPx(x, y, 0x0100u);
				for (int y = 2; y < 4; ++y)
					for (int x = 4; x < 8; ++x)
						setPx(x, y, 0x0000u);
				// Boundary/edge pixels for odd-edge clamping checks.
				setPx(w - 1, 0, 0xFFFFu);
				setPx(0, h - 1, 0x0040u);
				setPx(w - 1, h - 1, 0xFFFFu);

				// CPU reference: real MotionAhead::HalfLuma8 on valid even P010 sizes, exact U3 law on odd sizes.
				std::vector<uint8_t> halfCpu;
				int cpuHalfW = 0, cpuHalfH = 0;
				if ((w & 1) == 0 && (h & 1) == 0)
				{
					const size_t lumaRowBytes = static_cast<size_t>(w) * sizeof(uint16_t);
					const size_t chromaRows = static_cast<size_t>((h + 1) / 2);
					std::vector<uint8_t> p010Buf(lumaRowBytes * static_cast<size_t>(h) + lumaRowBytes * chromaRows, 0);
					std::memcpy(p010Buf.data(), plane.data(), plane.size() * sizeof(uint16_t));

					AnalysisLumaSource src;
					src.data = p010Buf.data();
					src.dataBytes = p010Buf.size();
					src.width = w;
					src.height = h;
					src.rowBytes = lumaRowBytes;
					src.chromaRowBytes = lumaRowBytes;
					src.format = AnalysisLumaFormat::P010;

					Assert::IsTrue(MotionAhead::HalfLuma8(src, halfCpu, cpuHalfW, cpuHalfH),
						L"MotionAhead::HalfLuma8 must succeed on synthetic P010 input");
				}
				else
				{
					ReferencePlanar16HalfLuma8(plane.data(), w, h, halfCpu, cpuHalfW, cpuHalfH);
				}
				Assert::AreEqual(halfW, cpuHalfW);
				Assert::AreEqual(halfH, cpuHalfH);

				std::vector<uint8_t> qtrCpu(static_cast<size_t>(qW) * static_cast<size_t>(qH), 0);
				for (int y = 0; y < qH; ++y)
				{
					const int y0 = VpUnit::HalfLuma::Tap0(y, halfH);
					const int y1 = VpUnit::HalfLuma::Tap1(y, halfH);
					for (int x = 0; x < qW; ++x)
					{
						const int x0 = VpUnit::HalfLuma::Tap0(x, halfW);
						const int x1 = VpUnit::HalfLuma::Tap1(x, halfW);
						qtrCpu[static_cast<size_t>(y) * static_cast<size_t>(qW) + static_cast<size_t>(x)] =
							VpUnit::HalfLuma::QuarterEngine8(
								halfCpu[static_cast<size_t>(y0) * static_cast<size_t>(halfW) + static_cast<size_t>(x0)],
								halfCpu[static_cast<size_t>(y0) * static_cast<size_t>(halfW) + static_cast<size_t>(x1)],
								halfCpu[static_cast<size_t>(y1) * static_cast<size_t>(halfW) + static_cast<size_t>(x0)],
								halfCpu[static_cast<size_t>(y1) * static_cast<size_t>(halfW) + static_cast<size_t>(x1)]);
					}
				}

				// Upload P010 luma plane as R16_UINT SRV.
				D3D11_TEXTURE2D_DESC srcDesc{};
				srcDesc.Width = static_cast<UINT>(w);
				srcDesc.Height = static_cast<UINT>(h);
				srcDesc.MipLevels = 1;
				srcDesc.ArraySize = 1;
				srcDesc.Format = DXGI_FORMAT_R16_UINT;
				srcDesc.SampleDesc.Count = 1;
				srcDesc.Usage = D3D11_USAGE_DEFAULT;
				srcDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
				D3D11_SUBRESOURCE_DATA srcInit{};
				srcInit.pSysMem = plane.data();
				srcInit.SysMemPitch = static_cast<UINT>(w * sizeof(uint16_t));
				CComPtr<ID3D11Texture2D> srcTex;
				CComPtr<ID3D11ShaderResourceView> srcSrv;
				Assert::IsTrue(SUCCEEDED(device->CreateTexture2D(&srcDesc, &srcInit, &srcTex)));
				Assert::IsTrue(SUCCEEDED(device->CreateShaderResourceView(srcTex, nullptr, &srcSrv)));

				// Half-luma UAV + SRV (DXGI_FORMAT_R8_UNORM).
				D3D11_TEXTURE2D_DESC halfDesc{};
				halfDesc.Width = static_cast<UINT>(halfW);
				halfDesc.Height = static_cast<UINT>(halfH);
				halfDesc.MipLevels = 1;
				halfDesc.ArraySize = 1;
				halfDesc.Format = DXGI_FORMAT_R8_UNORM;
				halfDesc.SampleDesc.Count = 1;
				halfDesc.Usage = D3D11_USAGE_DEFAULT;
				halfDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
				CComPtr<ID3D11Texture2D> halfTex;
				CComPtr<ID3D11UnorderedAccessView> halfUav;
				CComPtr<ID3D11ShaderResourceView> halfSrv;
				Assert::IsTrue(SUCCEEDED(device->CreateTexture2D(&halfDesc, nullptr, &halfTex)));
				Assert::IsTrue(SUCCEEDED(device->CreateUnorderedAccessView(halfTex, nullptr, &halfUav)));
				Assert::IsTrue(SUCCEEDED(device->CreateShaderResourceView(halfTex, nullptr, &halfSrv)));

				// Quarter-luma UAV (DXGI_FORMAT_R8_UNORM).
				D3D11_TEXTURE2D_DESC qtrDesc = halfDesc;
				qtrDesc.Width = static_cast<UINT>(qW);
				qtrDesc.Height = static_cast<UINT>(qH);
				qtrDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
				CComPtr<ID3D11Texture2D> qtrTex;
				CComPtr<ID3D11UnorderedAccessView> qtrUav;
				Assert::IsTrue(SUCCEEDED(device->CreateTexture2D(&qtrDesc, nullptr, &qtrTex)));
				Assert::IsTrue(SUCCEEDED(device->CreateUnorderedAccessView(qtrTex, nullptr, &qtrUav)));

				// Dispatch kR6HalfLumaHlsl on WARP.
				const HlslParams halfParams{
					static_cast<uint32_t>(w), static_cast<uint32_t>(h),
					static_cast<uint32_t>(halfW), static_cast<uint32_t>(halfH)
				};
				context->UpdateSubresource(paramsBuf, 0, nullptr, &halfParams, 0, 0);
				ID3D11Buffer* cbs[1] = { paramsBuf.p };
				ID3D11ShaderResourceView* srvs[1] = { srcSrv.p };
				ID3D11UnorderedAccessView* uavs[1] = { halfUav.p };
				context->CSSetShader(halfCs, nullptr, 0);
				context->CSSetConstantBuffers(0, 1, cbs);
				context->CSSetShaderResources(0, 1, srvs);
				context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
				context->Dispatch(static_cast<UINT>((halfW + 15) / 16), static_cast<UINT>((halfH + 15) / 16), 1u);

				// Unbind UAV before rebinding halfTex as SRV for kR6QuarterLumaHlsl.
				ID3D11UnorderedAccessView* nullUav[1] = { nullptr };
				ID3D11ShaderResourceView* nullSrv[1] = { nullptr };
				context->CSSetUnorderedAccessViews(0, 1, nullUav, nullptr);
				context->CSSetShaderResources(0, 1, nullSrv);

				// Dispatch kR6QuarterLumaHlsl on WARP reading the GPU halfTex output.
				const HlslParams qtrParams{
					static_cast<uint32_t>(halfW), static_cast<uint32_t>(halfH),
					static_cast<uint32_t>(qW), static_cast<uint32_t>(qH)
				};
				context->UpdateSubresource(paramsBuf, 0, nullptr, &qtrParams, 0, 0);
				srvs[0] = halfSrv.p;
				uavs[0] = qtrUav.p;
				context->CSSetShader(quarterCs, nullptr, 0);
				context->CSSetConstantBuffers(0, 1, cbs);
				context->CSSetShaderResources(0, 1, srvs);
				context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
				context->Dispatch(static_cast<UINT>((qW + 15) / 16), static_cast<UINT>((qH + 15) / 16), 1u);
				context->CSSetUnorderedAccessViews(0, 1, nullUav, nullptr);
				context->CSSetShaderResources(0, 1, nullSrv);
				context->CSSetShader(nullptr, nullptr, 0);

				std::vector<uint8_t> halfGpu;
				std::vector<uint8_t> qtrGpu;
				readBackR8(halfTex, halfW, halfH, halfGpu);
				readBackR8(qtrTex, qW, qH, qtrGpu);

				Assert::AreEqual(halfCpu.size(), halfGpu.size());
				Assert::AreEqual(0, std::memcmp(halfCpu.data(), halfGpu.data(), halfCpu.size()),
					L"WARP-compiled kR6HalfLumaHlsl must match CPU MotionAhead::HalfLuma8 bit-identically");
				Assert::AreEqual(qtrCpu.size(), qtrGpu.size());
				Assert::AreEqual(0, std::memcmp(qtrCpu.data(), qtrGpu.data(), qtrCpu.size()),
					L"WARP-compiled kR6QuarterLumaHlsl must match CPU VpUnit::HalfLuma::QuarterEngine8 bit-identically");

				// Same-input negative controls: naive shift formulas MUST differ from reference on this exact input.
				std::vector<uint8_t> naiveHalfUnshifted(halfCpu.size(), 0);
				std::vector<uint8_t> naiveHalfRounded(halfCpu.size(), 0);
				for (int y = 0; y < halfH; ++y)
				{
					const uint16_t* r0 = plane.data() + static_cast<size_t>(VpUnit::HalfLuma::Tap0(y, h)) * static_cast<size_t>(w);
					const uint16_t* r1 = plane.data() + static_cast<size_t>(VpUnit::HalfLuma::Tap1(y, h)) * static_cast<size_t>(w);
					for (int x = 0; x < halfW; ++x)
					{
						const int x0 = VpUnit::HalfLuma::Tap0(x, w);
						const int x1 = VpUnit::HalfLuma::Tap1(x, w);
						const size_t idx = static_cast<size_t>(y) * static_cast<size_t>(halfW) + static_cast<size_t>(x);
						const uint32_t rawSum = static_cast<uint32_t>(r0[x0]) + r0[x1] + r1[x0] + r1[x1];
						naiveHalfUnshifted[idx] = static_cast<uint8_t>((std::min)(255u, rawSum >> 10u));
						const uint32_t codeSum = static_cast<uint32_t>(r0[x0] >> 6) + (r0[x1] >> 6) + (r1[x0] >> 6) + (r1[x1] >> 6);
						naiveHalfRounded[idx] = static_cast<uint8_t>((std::min)(255u, (codeSum + 8u) >> 4u));
					}
				}
				Assert::AreNotEqual(0, std::memcmp(halfCpu.data(), naiveHalfUnshifted.data(), halfCpu.size()),
					L"negative control: naive unshifted 16-bit average must differ on the exact same P010 input");
				Assert::AreNotEqual(0, std::memcmp(halfCpu.data(), naiveHalfRounded.data(), halfCpu.size()),
					L"negative control: naive +8 rounded half shift must differ on the exact same P010 input");

				std::vector<uint8_t> naiveQtrTrunc(qtrCpu.size(), 0);
				for (int y = 0; y < qH; ++y)
				{
					const int y0 = VpUnit::HalfLuma::Tap0(y, halfH);
					const int y1 = VpUnit::HalfLuma::Tap1(y, halfH);
					for (int x = 0; x < qW; ++x)
					{
						const int x0 = VpUnit::HalfLuma::Tap0(x, halfW);
						const int x1 = VpUnit::HalfLuma::Tap1(x, halfW);
						const uint32_t sum =
							static_cast<uint32_t>(halfCpu[static_cast<size_t>(y0) * static_cast<size_t>(halfW) + static_cast<size_t>(x0)]) +
							static_cast<uint32_t>(halfCpu[static_cast<size_t>(y0) * static_cast<size_t>(halfW) + static_cast<size_t>(x1)]) +
							static_cast<uint32_t>(halfCpu[static_cast<size_t>(y1) * static_cast<size_t>(halfW) + static_cast<size_t>(x0)]) +
							static_cast<uint32_t>(halfCpu[static_cast<size_t>(y1) * static_cast<size_t>(halfW) + static_cast<size_t>(x1)]);
						naiveQtrTrunc[static_cast<size_t>(y) * static_cast<size_t>(qW) + static_cast<size_t>(x)] =
							static_cast<uint8_t>(sum >> 2u);
					}
				}
				Assert::AreNotEqual(0, std::memcmp(qtrCpu.data(), naiveQtrTrunc.data(), qtrCpu.size()),
					L"negative control: naive truncating quarter shift (missing +2u) must differ on the exact same input");
			}
		}

		TEST_METHOD(ProducerSetR6AsyncExtrasMatchesSyncBitIdenticallyAndPreservesOrder)
		{
			// Synthetic P010 sequence across two generations:
			//   gen 1: seq 1..6 (5 consecutive pairs)
			//   gen 2: seq 7..10 (3 consecutive pairs; exercises generation-boundary Flush)
			const int w = 64;
			const int h = 36;
			const size_t lumaRowBytes = static_cast<size_t>(w) * sizeof(uint16_t);
			const size_t chromaRows = static_cast<size_t>((h + 1) / 2);
			const size_t frameBytes = lumaRowBytes * static_cast<size_t>(h) + lumaRowBytes * chromaRows;
			const uint64_t kGen1Frames = 6;
			const uint64_t kGen1Pairs = kGen1Frames - 1;
			const uint64_t kGen2Frames = 4;
			const uint64_t kGen2Pairs = kGen2Frames - 1;
			const uint64_t kTotalFrames = kGen1Frames + kGen2Frames;

			std::vector<std::vector<uint8_t>> backing(static_cast<size_t>(kTotalFrames + 1));
			std::vector<AnalysisLumaSource> sources(static_cast<size_t>(kTotalFrames + 1));
			for (uint64_t seq = 1; seq <= kTotalFrames; ++seq)
			{
				backing[static_cast<size_t>(seq)].assign(frameBytes, 0);
				uint16_t* luma = reinterpret_cast<uint16_t*>(backing[static_cast<size_t>(seq)].data());
				for (int y = 0; y < h; ++y)
				{
					for (int x = 0; x < w; ++x)
					{
						const uint32_t code10 = static_cast<uint32_t>((x * 9 + y * 5 + static_cast<int>(seq) * 17) & 0x3FF);
						luma[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)] =
							static_cast<uint16_t>(code10 << 6);
					}
				}
				AnalysisLumaSource& s = sources[static_cast<size_t>(seq)];
				s.data = backing[static_cast<size_t>(seq)].data();
				s.dataBytes = backing[static_cast<size_t>(seq)].size();
				s.width = w;
				s.height = h;
				s.rowBytes = lumaRowBytes;
				s.chromaRowBytes = lumaRowBytes;
				s.format = AnalysisLumaFormat::P010;
			}

			struct ProducerRun
			{
				std::vector<MotionAhead::FlowPtr> gen1;
				std::vector<MotionAhead::FlowPtr> gen2;
				std::string activeStatusLine;
				MotionAhead::R6::ExtrasStats activeStats{};
				MotionAhead::Producer::Stats finalStats{};
			};

			auto runProducer = [&](bool enableR6Async) -> ProducerRun {
				ProducerRun run;
				MotionAhead::Producer producer;
				producer.SetDepth(8);
				producer.SetR6AsyncExtras(enableR6Async);
				Assert::AreEqual(enableR6Async, producer.R6AsyncExtras(),
					L"Producer::R6AsyncExtras must reflect SetR6AsyncExtras");

				auto seam = std::make_shared<MotionAhead::Producer::TestEngine>();
				seam->job = [](uint64_t seq, uint64_t) {
					if (seq == 3 || seq == 4)
						std::this_thread::yield();
					return true;
				};
				Assert::IsTrue(producer.StartForTest(seam), L"Producer::StartForTest must succeed");

				// Generation 1 burst: verify strict publish order into P2Slot while waiting.
				for (uint64_t seq = 1; seq <= kGen1Frames; ++seq)
					producer.Submit(seq, 1, sources[static_cast<size_t>(seq)]);

				const auto deadline1 = std::chrono::steady_clock::now() + std::chrono::seconds(5);
				while (std::chrono::steady_clock::now() < deadline1)
				{
					bool allReady = producer.GetStats().pairs >= kGen1Pairs;
					for (uint64_t k = 1; k <= kGen1Pairs; ++k)
					{
						if (producer.Take(k, k + 1) != nullptr)
						{
							for (uint64_t prev = 1; prev < k; ++prev)
							{
								Assert::IsNotNull(producer.Take(prev, prev + 1).get(),
									L"Producer must publish consecutive pairs in strict monotonic order");
							}
						}
						else
						{
							allReady = false;
						}
					}
					if (allReady)
						break;
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}

				for (uint64_t k = 1; k <= kGen1Pairs; ++k)
				{
					MotionAhead::FlowPtr f = producer.Take(k, k + 1);
					Assert::IsNotNull(f.get(), L"every gen-1 pair must be published in P2Slot");
					run.gen1.push_back(std::move(f));
				}
				run.activeStatusLine = producer.R6StatusLine();
				run.activeStats = producer.GetR6ExtrasStats();

				// Generation 2 burst: verifies clean generation-boundary flush and ordering.
				for (uint64_t seq = kGen1Frames + 1; seq <= kTotalFrames; ++seq)
					producer.Submit(seq, 2, sources[static_cast<size_t>(seq)]);

				const auto deadline2 = std::chrono::steady_clock::now() + std::chrono::seconds(5);
				while (std::chrono::steady_clock::now() < deadline2)
				{
					bool allReady = producer.GetStats().pairs >= (kGen1Pairs + kGen2Pairs);
					for (uint64_t k = kGen1Frames + 1; k < kTotalFrames; ++k)
					{
						if (producer.Take(k, k + 1) != nullptr)
						{
							for (uint64_t prev = kGen1Frames + 1; prev < k; ++prev)
							{
								Assert::IsNotNull(producer.Take(prev, prev + 1).get(),
									L"Producer must preserve strict monotonic order after generation transition");
							}
						}
						else
						{
							allReady = false;
						}
					}
					if (allReady)
						break;
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}

				Assert::IsNull(producer.Take(1, 2).get(),
					L"gen-1 pairs must be cleared on transition to gen 2");
				for (uint64_t k = kGen1Frames + 1; k < kTotalFrames; ++k)
				{
					MotionAhead::FlowPtr f = producer.Take(k, k + 1);
					Assert::IsNotNull(f.get(), L"every gen-2 pair must be published in P2Slot");
					run.gen2.push_back(std::move(f));
				}

				run.finalStats = producer.GetStats();
				producer.Stop();
				Assert::IsFalse(producer.Running(), L"Producer::Stop must leave producer stopped cleanly");
				Assert::IsFalse(producer.GetR6ExtrasStats().running,
					L"Producer::Stop must join and stop the R6 extras worker without hangs/leaks");
				Assert::AreEqual(0ull, run.finalStats.dropped, L"no frames should be dropped in bounded synthetic run");
				return run;
			};

			const ProducerRun syncRun = runProducer(false);
			const ProducerRun asyncRun = runProducer(true);

			// Same-input status negative control: sync stays explicitly "off" with worker stopped,
			// while async must actively run on r6ExtrasWorker with zero sync fallbacks.
			Assert::IsTrue(syncRun.activeStatusLine.find("off") != std::string::npos,
				L"sync Producer (SetR6AsyncExtras(false)) must report R6 async extras off");
			Assert::IsFalse(syncRun.activeStats.running,
				L"sync Producer must not start the R6 extras worker");
			Assert::AreEqual(static_cast<uint64_t>(0), syncRun.activeStats.asyncCompleted,
				L"sync Producer must not execute pairs on R6 extras worker");
			Assert::IsTrue(asyncRun.activeStats.running,
				L"R6 async extras worker must be running when enabled");
			Assert::IsTrue(asyncRun.activeStats.asyncCompleted >= kGen1Pairs,
				L"R6 async extras must complete gen-1 pairs asynchronously");
			Assert::AreEqual<uint64_t>(0ull, asyncRun.activeStats.syncFallbacks,
				L"R6 async extras must not fall back to sync when worker is running");
			Assert::IsTrue(asyncRun.activeStatusLine.find("(on,") != std::string::npos,
				L"R6 status line must visibly report (on, ...)");

			auto assertPairsBitIdentical = [](const std::vector<MotionAhead::FlowPtr>& a, const std::vector<MotionAhead::FlowPtr>& b) {
				Assert::AreEqual(a.size(), b.size());
				for (size_t i = 0; i < a.size(); ++i)
				{
					Assert::IsNotNull(a[i].get());
					Assert::IsNotNull(b[i].get());
					// Non-vacuousness: published flows must actually have computed FlowGlobals & FlowExtras (never blank Flow).
					Assert::IsTrue(a[i]->globalSet, L"sync Producer published Flow must have FlowGlobals computed");
					Assert::IsTrue(b[i]->globalSet, L"async Producer published Flow must have FlowGlobals computed");
					Assert::AreEqual(a[i]->globalGeneral, b[i]->globalGeneral);
					Assert::IsFalse(a[i]->costWeight.empty(), L"sync Producer published Flow must carry non-empty costWeight");
					Assert::IsFalse(b[i]->costWeight.empty(), L"async Producer published Flow must carry non-empty costWeight");
					Assert::IsFalse(a[i]->fbError.empty(), L"sync Producer published Flow must carry non-empty fbError");
					Assert::IsFalse(b[i]->fbError.empty(), L"async Producer published Flow must carry non-empty fbError");

					Assert::AreEqual(0, std::memcmp(a[i]->global, b[i]->global, sizeof(a[i]->global)),
						L"Flow::global must match bit-for-bit between SetR6AsyncExtras(false) and (true)");
					Assert::AreEqual(0, std::memcmp(a[i]->second, b[i]->second, sizeof(a[i]->second)),
						L"Flow::second must match bit-for-bit between SetR6AsyncExtras(false) and (true)");
					Assert::AreEqual(a[i]->costWeight.size(), b[i]->costWeight.size());
					Assert::AreEqual(0, std::memcmp(a[i]->costWeight.data(), b[i]->costWeight.data(),
						a[i]->costWeight.size() * sizeof(float)),
						L"Flow::costWeight must match bit-for-bit between SetR6AsyncExtras(false) and (true)");
					Assert::AreEqual(a[i]->fbError.size(), b[i]->fbError.size());
					Assert::AreEqual(0, std::memcmp(a[i]->fbError.data(), b[i]->fbError.data(),
						a[i]->fbError.size() * sizeof(float)),
						L"Flow::fbError must match bit-for-bit between SetR6AsyncExtras(false) and (true)");
				}
			};
			assertPairsBitIdentical(syncRun.gen1, asyncRun.gen1);
			assertPairsBitIdentical(syncRun.gen2, asyncRun.gen2);

			// Negative control on the bit-identity comparator: mutating an interior backward vector
			// sampled by FbError under forward (+64, -32) changes fbError.
			auto probeClean = MakeSyntheticFlow(64, 36, 64, -32);
			auto probeCorrupt = MakeSyntheticFlow(64, 36, 64, -32);
			const size_t midCell = (static_cast<size_t>(probeCorrupt->gridH / 2) * static_cast<size_t>(probeCorrupt->gridW) +
				static_cast<size_t>(probeCorrupt->gridW / 2)) * 2u;
			probeCorrupt->backward[midCell] = static_cast<int16_t>(probeCorrupt->backward[midCell] + 128);
			MotionAhead::FlowExtras(*probeClean);
			MotionAhead::FlowExtras(*probeCorrupt);
			Assert::IsFalse(probeClean->fbError.empty());
			Assert::AreNotEqual(0, std::memcmp(probeClean->fbError.data(), probeCorrupt->fbError.data(),
				probeClean->fbError.size() * sizeof(float)),
				L"negative control: perturbed flow vector must produce distinct fbError");
		}

		TEST_METHOD(FlowGlobalBinOfMatchesLroundExhaustivelyOverAllInt16AndHistogramMatchesNthElement)
		{
			// 1. Exhaustive int16_t check (all 65,536 values in [-32768, 32767]):
			//    FlowGlobalBinOf(v) must match std::lround(v / 64.0) bit-for-bit on every input.
			int naiveShiftMismatches = 0;
			for (int v = -32768; v <= 32767; ++v)
			{
				const int expected = static_cast<int>(std::lround(static_cast<double>(v) / 64.0));
				const int actual = MotionAhead::FlowGlobalBinOf(static_cast<int16_t>(v));
				Assert::AreEqual(expected, actual,
					L"FlowGlobalBinOf must match std::lround(v / 64.0) across all 65,536 int16_t values");
				const int naiveShift = (v + 32) >> 6;
				if (naiveShift != expected)
					++naiveShiftMismatches;
			}

			// Same-input negative control (Rule 3): naive truncating shift (v + 32) >> 6 without
			// symmetric negative handling fails at every negative half-bin boundary (32768 / 64 = 512).
			const int negHalfExpected = static_cast<int>(std::lround(-32.0 / 64.0));
			const int negHalfActual = MotionAhead::FlowGlobalBinOf(static_cast<int16_t>(-32));
			const int negHalfNaive = (-32 + 32) >> 6;
			Assert::AreEqual(-1, negHalfExpected);
			Assert::AreEqual(negHalfExpected, negHalfActual,
				L"FlowGlobalBinOf(-32) must round half-away-from-zero to -1");
			Assert::AreNotEqual(negHalfExpected, negHalfNaive,
				L"negative control: naive (v + 32) >> 6 produces 0 instead of -1 at v = -32");
			Assert::AreEqual(512, naiveShiftMismatches,
				L"negative control: naive (v + 32) >> 6 must fail at all 512 negative half-bin boundaries");

			// 2. Independent pre-J#5 reference FlowGlobal: std::nth_element on a fresh cost copy,
			//    std::lround(v / 64.0) binning, and a fresh per-call std::vector<int> votes buffer.
			auto refFlowGlobal = [](const std::vector<int16_t>& vectors, const std::vector<uint8_t>& costs,
				int gridW, int gridH, float global[4], float second[4], bool general) {
				const int kRange = MotionAhead::FlowVoteRange(gridH, general);
				const int kBins = 4 * kRange + 1;
				global[0] = global[1] = 0.0f; global[2] = 1.0f; global[3] = 1.0f;
				second[0] = second[1] = second[2] = second[3] = 0.0f;
				const size_t cells = static_cast<size_t>(gridW) * static_cast<size_t>(gridH);
				if (gridW <= 0 || gridH <= 0 || vectors.size() < cells * 2 || costs.size() < cells)
					return;
				std::vector<uint8_t> sorted(costs.begin(), costs.begin() + static_cast<std::ptrdiff_t>(cells));
				std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(cells / 2), sorted.end());
				const uint8_t costLimit = sorted[cells / 2];
				std::vector<int> votes(static_cast<size_t>(kBins) * static_cast<size_t>(kBins), 0);
				long confident = 0;
				const int binOffset = 2 * kRange;
				for (size_t i = 0; i < cells; ++i)
				{
					if (costs[i] > costLimit) continue;
					const int bx = static_cast<int>(std::lround(static_cast<double>(vectors[i * 2]) / 64.0)) + binOffset;
					const int by = static_cast<int>(std::lround(static_cast<double>(vectors[i * 2 + 1]) / 64.0)) + binOffset;
					if (bx < 0 || by < 0 || bx >= kBins || by >= kBins) continue;
					++votes[static_cast<size_t>(by) * static_cast<size_t>(kBins) + static_cast<size_t>(bx)];
					++confident;
				}
				if (confident == 0 || confident < static_cast<long>(cells / 10))
					return;
				auto window = [&](int bx, int by) {
					int s = 0;
					for (int dy = -1; dy <= 1; ++dy)
						for (int dx = -1; dx <= 1; ++dx)
						{
							const int x = bx + dx, y = by + dy;
							if (x >= 0 && y >= 0 && x < kBins && y < kBins)
								s += votes[static_cast<size_t>(y) * static_cast<size_t>(kBins) + static_cast<size_t>(x)];
						}
					return s;
				};
				int best = -1, bestX = 0, bestY = 0;
				for (int y = 0; y < kBins; ++y)
					for (int x = 0; x < kBins; ++x)
					{
						if (!votes[static_cast<size_t>(y) * static_cast<size_t>(kBins) + static_cast<size_t>(x)]) continue;
						const int s = window(x, y);
						if (s > best) { best = s; bestX = x; bestY = y; }
					}
				const double px = (bestX - binOffset) / 2.0, py = (bestY - binOffset) / 2.0;
				double sx = 0.0, sy = 0.0;
				long close = 0;
				for (size_t i = 0; i < cells; ++i)
				{
					if (costs[i] > costLimit) continue;
					const double vx = vectors[i * 2] / 32.0 / 4.0, vy = vectors[i * 2 + 1] / 32.0 / 4.0;
					if (std::fabs(vx - px) <= 1.0 && std::fabs(vy - py) <= 1.0) { sx += vx; sy += vy; ++close; }
				}
				const double share = static_cast<double>(close) / confident;
				global[0] = static_cast<float>(std::round((close ? sx / close : px) * 2.0) / 2.0);
				global[1] = static_cast<float>(std::round((close ? sy / close : py) * 2.0) / 2.0);
				global[2] = static_cast<float>((std::min)(1.0, (std::max)(0.05, 1.05 - share * 0.45 / 0.3)));
				int secondBest = 0, sxBin = 0, syBin = 0;
				for (int y = 0; y < kBins; ++y)
					for (int x = 0; x < kBins; ++x)
					{
						if (!votes[static_cast<size_t>(y) * static_cast<size_t>(kBins) + static_cast<size_t>(x)]) continue;
						if (std::abs(x - bestX) <= 2 && std::abs(y - bestY) <= 2) continue;
						const int s = window(x, y);
						if (s > secondBest) { secondBest = s; sxBin = x; syBin = y; }
					}
				if (secondBest > 0 && secondBest >= confident / 10)
				{
					second[0] = static_cast<float>((sxBin - binOffset) / 2.0);
					second[1] = static_cast<float>((syBin - binOffset) / 2.0);
					second[3] = 1.0f;
				}
			};

			auto verifyFlow = [&](const MotionAhead::Flow& flow, bool general,
				float prodGlobal[4], float prodSecond[4], const wchar_t* label) {
				float refGlobal[4] = { 99.0f, 99.0f, 99.0f, 99.0f };
				float refSecond[4] = { 99.0f, 99.0f, 99.0f, 99.0f };
				prodGlobal[0] = prodGlobal[1] = prodGlobal[2] = prodGlobal[3] = -99.0f;
				prodSecond[0] = prodSecond[1] = prodSecond[2] = prodSecond[3] = -99.0f;
				MotionAhead::FlowGlobal(flow.forward, flow.forwardCost, flow.gridW, flow.gridH, prodGlobal, prodSecond, general);
				refFlowGlobal(flow.forward, flow.forwardCost, flow.gridW, flow.gridH, refGlobal, refSecond, general);
				Assert::AreEqual(0, std::memcmp(refGlobal, prodGlobal, sizeof(refGlobal)), label);
				Assert::AreEqual(0, std::memcmp(refSecond, prodSecond, sizeof(refSecond)), label);
			};

			// Case A: Even cell count (10 x 360 = 3600), gridH > 270 so general=true expands kRange (> 40),
			//         exact acc == cells/2 median boundary (catches >= vs > histogram off-by-one),
			//         negative half-bin boundary vectors (-96), and two competing clusters.
			Assert::IsTrue(MotionAhead::FlowVoteRange(360, true) > MotionAhead::FlowVoteRange(360, false),
				L"gridH=360 must expand FlowVoteRange when general=true so shrinking kBins is genuinely exercised");
			MotionAhead::Flow flowLargeEven;
			flowLargeEven.gridW = 10;
			flowLargeEven.gridH = 360;
			flowLargeEven.halfW = flowLargeEven.gridW * 4;
			flowLargeEven.halfH = flowLargeEven.gridH * 4;
			const size_t nLarge = static_cast<size_t>(flowLargeEven.gridW) * static_cast<size_t>(flowLargeEven.gridH);
			flowLargeEven.forward.assign(nLarge * 2, 0);
			flowLargeEven.forwardCost.assign(nLarge, 0);
			for (size_t i = 0; i < nLarge; ++i)
			{
				// Permute via coprime stride 997 (gcd(997, 3600) == 1) so costs are genuinely unsorted.
				const size_t dst = (i * 997u) % nLarge;
				if (i < 1500)
				{
					flowLargeEven.forwardCost[dst] = 12u;
					flowLargeEven.forward[dst * 2] = -96;      // negative half-bin boundary (-1.5 bins -> -2)
					flowLargeEven.forward[dst * 2 + 1] = 160;
				}
				else if (i < 1800)
				{
					flowLargeEven.forwardCost[dst] = 12u;      // cumulative 1800 == cells / 2 exactly
					flowLargeEven.forward[dst * 2] = 1920;     // +15.0 texels (in-range for both general=true and false)
					flowLargeEven.forward[dst * 2 + 1] = -1920;
				}
				else if (i < 2700)
				{
					flowLargeEven.forwardCost[dst] = 48u;      // true median (sorted[1800] == 48)
					flowLargeEven.forward[dst * 2] = 5632;     // +44.0 texels (in-range for general=true, out-of-range for false)
					flowLargeEven.forward[dst * 2 + 1] = -512;
				}
				else
				{
					flowLargeEven.forwardCost[dst] = 210u;     // above median: must be excluded
					flowLargeEven.forward[dst * 2] = 640;
					flowLargeEven.forward[dst * 2 + 1] = 640;
				}
			}

			float largeGenTrueGlobal1[4] = {}, largeGenTrueSecond1[4] = {};
			verifyFlow(flowLargeEven, true, largeGenTrueGlobal1, largeGenTrueSecond1,
				L"Case A (even, expanded kBins, general=true, acc==target boundary) must match nth_element reference bit-exactly");
			Assert::IsTrue(largeGenTrueGlobal1[2] < 0.9f, L"Case A must produce a usable primary global vector");
			Assert::AreEqual(1.0f, largeGenTrueSecond1[3], L"Case A must populate second motion layer");
			Assert::AreEqual(44.0f, largeGenTrueSecond1[0], L"Case A general=true must capture +44-texel second cluster");

			// Case B: Same flow with general=false -> immediately shrinks thread_local votes (213x213 -> 161x161).
			float largeGenFalseGlobal[4] = {}, largeGenFalseSecond[4] = {};
			verifyFlow(flowLargeEven, false, largeGenFalseGlobal, largeGenFalseSecond,
				L"Case B (same large flow, general=false, shrunk kBins) must match nth_element reference bit-exactly");
			Assert::AreEqual(1.0f, largeGenFalseSecond[3], L"Case B must populate second motion layer from in-range cluster");
			Assert::AreEqual(15.0f, largeGenFalseSecond[0], L"Case B general=false must clip +44-texel cluster and pick +15-texel cluster");

			// Case C: Odd cell count (13 x 11 = 143), heavy ties straddling median index 71, two competing clusters,
			//         and a decoy high-cost cluster placed at Case A's peak to catch any stale vote leak.
			MotionAhead::Flow flowOddTies;
			flowOddTies.gridW = 13;
			flowOddTies.gridH = 11;
			flowOddTies.halfW = flowOddTies.gridW * 4;
			flowOddTies.halfH = flowOddTies.gridH * 4;
			const size_t nOdd = static_cast<size_t>(flowOddTies.gridW) * static_cast<size_t>(flowOddTies.gridH);
			flowOddTies.forward.assign(nOdd * 2, 0);
			flowOddTies.forwardCost.assign(nOdd, 0);
			size_t boundaryCellDst = 0;
			for (size_t i = 0; i < nOdd; ++i)
			{
				// Coprime stride 37 (gcd(37, 143) == 1).
				const size_t dst = (i * 37u) % nOdd;
				if (i < 40)
				{
					flowOddTies.forwardCost[dst] = 15u;
					flowOddTies.forward[dst * 2] = 384;       // +3.0 texels
					flowOddTies.forward[dst * 2 + 1] = -256;  // -2.0 texels
				}
				else if (i < 59)
				{
					flowOddTies.forwardCost[dst] = 77u;       // heavy tie straddling median index 71
					flowOddTies.forward[dst * 2] = 384;
					flowOddTies.forward[dst * 2 + 1] = -256;
				}
				else if (i == 59)
				{
					flowOddTies.forwardCost[dst] = 77u;
					flowOddTies.forward[dst * 2] = 480;       // +3.75 texels (within <= 1.0 texel of +3.0 peak)
					flowOddTies.forward[dst * 2 + 1] = -256;
					boundaryCellDst = dst;
				}
				else if (i < 100)
				{
					flowOddTies.forwardCost[dst] = 77u;       // heavy tie at median: second cluster (40 cells)
					flowOddTies.forward[dst * 2] = -512;      // -4.0 texels
					flowOddTies.forward[dst * 2 + 1] = 448;   // +3.5 texels
				}
				else
				{
					flowOddTies.forwardCost[dst] = 190u;      // above median: decoy at Case A's peak
					flowOddTies.forward[dst * 2] = -96;
					flowOddTies.forward[dst * 2 + 1] = 160;
				}
			}

			float oddGlobal1[4] = {}, oddSecond1[4] = {};
			verifyFlow(flowOddTies, true, oddGlobal1, oddSecond1,
				L"Case C (odd cell count, heavy median ties, general=true) must match nth_element reference bit-exactly");
			Assert::IsTrue(oddGlobal1[2] < 0.9f, L"Case C must produce a usable primary global vector");
			Assert::AreEqual(1.0f, oddSecond1[3], L"Case C must populate second motion layer");

			// Case D: All-0 costs (even 12 x 9 = 108, general=false) with negative half-bin (-32) dominant cluster.
			MotionAhead::Flow flowAllZero;
			flowAllZero.gridW = 12;
			flowAllZero.gridH = 9;
			flowAllZero.halfW = flowAllZero.gridW * 4;
			flowAllZero.halfH = flowAllZero.gridH * 4;
			const size_t nZero = static_cast<size_t>(flowAllZero.gridW) * static_cast<size_t>(flowAllZero.gridH);
			flowAllZero.forward.assign(nZero * 2, 0);
			flowAllZero.forwardCost.assign(nZero, 0u);
			for (size_t i = 0; i < nZero; ++i)
			{
				if (i < 64)
				{
					flowAllZero.forward[i * 2] = -32;
					flowAllZero.forward[i * 2 + 1] = 32;
				}
				else
				{
					flowAllZero.forward[i * 2] = 768;
					flowAllZero.forward[i * 2 + 1] = -640;
				}
			}

			float allZeroGlobal[4] = {}, allZeroSecond[4] = {};
			verifyFlow(flowAllZero, false, allZeroGlobal, allZeroSecond,
				L"Case D (all-0 costs, general=false) must match nth_element reference bit-exactly");
			Assert::IsTrue(allZeroGlobal[2] < 0.9f, L"Case D must produce a usable primary global vector");
			Assert::AreEqual(1.0f, allZeroSecond[3], L"Case D must populate second motion layer");

			// Case E: All-255 costs (odd 9 x 9 = 81, general=true) + INT16_MIN/INT16_MAX out-of-range outliers.
			MotionAhead::Flow flowAll255;
			flowAll255.gridW = 9;
			flowAll255.gridH = 9;
			flowAll255.halfW = flowAll255.gridW * 4;
			flowAll255.halfH = flowAll255.gridH * 4;
			const size_t nMax = static_cast<size_t>(flowAll255.gridW) * static_cast<size_t>(flowAll255.gridH);
			flowAll255.forward.assign(nMax * 2, 0);
			flowAll255.forwardCost.assign(nMax, 255u);
			for (size_t i = 0; i < nMax; ++i)
			{
				if (i < 48)
				{
					flowAll255.forward[i * 2] = -256;
					flowAll255.forward[i * 2 + 1] = -320;
				}
				else if (i < 77)
				{
					flowAll255.forward[i * 2] = 512;
					flowAll255.forward[i * 2 + 1] = 192;
				}
				else
				{
					flowAll255.forward[i * 2] = (i & 1u) ? static_cast<int16_t>(-32768) : static_cast<int16_t>(32767);
					flowAll255.forward[i * 2 + 1] = (i & 1u) ? static_cast<int16_t>(32767) : static_cast<int16_t>(-32768);
				}
			}

			float all255Global[4] = {}, all255Second[4] = {};
			verifyFlow(flowAll255, true, all255Global, all255Second,
				L"Case E (all-255 costs, INT16 extreme outliers, general=true) must match nth_element reference bit-exactly");
			Assert::IsTrue(all255Global[2] < 0.9f, L"Case E must produce a usable primary global vector");
			Assert::AreEqual(1.0f, all255Second[3], L"Case E must populate second motion layer");

			// 3. Repeatability across growing and shrinking kBins on the same thread:
			//    Re-run Case A (grows thread_local votes back to 213x213) and Case C (shrinks back to 161x161, general=false).
			float largeGenTrueGlobal2[4] = {}, largeGenTrueSecond2[4] = {};
			verifyFlow(flowLargeEven, true, largeGenTrueGlobal2, largeGenTrueSecond2,
				L"Repeat Case A after smaller-grid calls must match nth_element reference bit-exactly");
			Assert::AreEqual(0, std::memcmp(largeGenTrueGlobal1, largeGenTrueGlobal2, sizeof(largeGenTrueGlobal1)),
				L"thread_local votes scratch must not leak stale counts across repeated Case A calls");
			Assert::AreEqual(0, std::memcmp(largeGenTrueSecond1, largeGenTrueSecond2, sizeof(largeGenTrueSecond1)),
				L"thread_local votes scratch must not leak stale second-layer counts across repeated Case A calls");

			float oddGlobal2[4] = {}, oddSecond2[4] = {};
			verifyFlow(flowOddTies, false, oddGlobal2, oddSecond2,
				L"Repeat Case C (general=false) after shrinking kBins must match nth_element reference bit-exactly");
			Assert::AreEqual(0, std::memcmp(oddGlobal1, oddGlobal2, sizeof(oddGlobal1)),
				L"thread_local votes scratch must not leak stale counts when shrinking kBins");
			Assert::AreEqual(0, std::memcmp(oddSecond1, oddSecond2, sizeof(oddSecond1)),
				L"thread_local votes scratch must not leak stale second-layer counts when shrinking kBins");

			// 4. Same-input negative controls on FlowGlobal comparator (Rule 3):
			//    (a) Corrupt the dominant cluster vector by +64 raw units (1 half-texel bin) -> global[0] shifts by +0.5.
			MotionAhead::Flow flowClusterShifted = flowOddTies;
			for (size_t i = 0; i < nOdd; ++i)
			{
				if (flowClusterShifted.forward[i * 2] == 384 && flowClusterShifted.forward[i * 2 + 1] == -256)
					flowClusterShifted.forward[i * 2] = static_cast<int16_t>(flowClusterShifted.forward[i * 2] + 64);
			}
			float shiftedClusterGlobal[4] = {}, shiftedClusterSecond[4] = {};
			MotionAhead::FlowGlobal(flowClusterShifted.forward, flowClusterShifted.forwardCost,
				flowClusterShifted.gridW, flowClusterShifted.gridH, shiftedClusterGlobal, shiftedClusterSecond, true);
			Assert::AreNotEqual(0, std::memcmp(oddGlobal1, shiftedClusterGlobal, sizeof(oddGlobal1)),
				L"negative control: shifting dominant cluster by +64 raw units (1 bin) must change FlowGlobal output");

			//    (b) Corrupt a single confident dominant-cluster boundary cell by +64 raw units (1 bin) ->
			//        moves it past the 1.0-texel refinement radius and changes share / global[2].
			MotionAhead::Flow flowSingleCellShifted = flowOddTies;
			flowSingleCellShifted.forward[boundaryCellDst * 2] =
				static_cast<int16_t>(flowSingleCellShifted.forward[boundaryCellDst * 2] + 64);
			float singleCellGlobal[4] = {}, singleCellSecond[4] = {};
			MotionAhead::FlowGlobal(flowSingleCellShifted.forward, flowSingleCellShifted.forwardCost,
				flowSingleCellShifted.gridW, flowSingleCellShifted.gridH, singleCellGlobal, singleCellSecond, true);
			Assert::AreNotEqual(0, std::memcmp(oddGlobal1, singleCellGlobal, sizeof(oddGlobal1)),
				L"negative control: perturbing a single dominant-cluster cell by +64 raw units (1 bin) must change FlowGlobal output");
		}
	};
}
