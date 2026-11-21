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

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

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
	};
}
