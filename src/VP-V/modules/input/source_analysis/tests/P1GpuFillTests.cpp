// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// P1 GPU fill (L-B4): the float emulation of the R16 unorm kernel equals the lane's CPU half-plane law bit for bit at 8/10/12/16
// bits, odd sizes included; the Float32 variant equals the accessor's mean. CPU only (the kernel's GPU run is the coordinator's).
#include <VP-V/modules/input/source_analysis/P1GpuFill.h>
#include <VP-V/modules/input/source_analysis/lane/shared/HalfLuma.h>
#include <cstdint>
#include <string>
#include <vector>
#include "CppUnitTest.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	TEST_CLASS(P1GpuFillTests)
	{
	public:
		TEST_METHOD(Unorm16KernelEqualsTheCpuLawAtEveryBitDepth)
		{
			using namespace VpUnit::HalfLuma;
			uint32_t rng = 12345u;
			auto next = [&rng]() { rng = rng * 1664525u + 1013904223u; return rng >> 8; };
			const int depths[4] = { 8, 10, 12, 16 };
			for (int bits : depths)
			{
				const uint32_t maxCode = (1u << bits) - 1u;
				for (uint32_t w : { 7u, 8u, 33u })
				{
					const uint32_t h = w + 1;
					std::vector<uint32_t> code(size_t(w) * h);
					for (auto& c : code) c = next() % (maxCode + 1u);
					code[0] = 0; code[1] = maxCode;
					const uint32_t hw = HalfDim(w), hh = HalfDim(h);
					for (uint32_t y = 0; y < hh; ++y)
						for (uint32_t x = 0; x < hw; ++x)
						{
							const uint32_t x0 = Tap0(x, w), x1 = Tap1(x, w), y0 = Tap0(y, h), y1 = Tap1(y, h);
							const uint32_t shift = 16u - uint32_t(bits);
							const uint32_t sum = Box4(code[y0 * w + x0] << shift, code[y0 * w + x1] << shift, code[y1 * w + x0] << shift, code[y1 * w + x1] << shift);
							const uint16_t cpu = FinishP1(sum);
							auto norm = [&](uint32_t c) { return float(c) / float(maxCode); };
							const uint16_t gpu = VpFrameAnalysis::P1Gpu::EmulateUnorm16(norm(code[y0 * w + x0]), norm(code[y0 * w + x1]),
								norm(code[y1 * w + x0]), norm(code[y1 * w + x1]), bits);
							Assert::AreEqual(unsigned(cpu), unsigned(gpu));
						}
				}
			}
		}

		TEST_METHOD(Float32VariantIsTheAccessorMean)
		{
			Assert::AreEqual(VpUnit::HalfLuma::MeanF<float>(0.1f, 0.2f, 0.3f, 0.4f),
				VpFrameAnalysis::P1Gpu::EmulateFloat32(0.1f, 0.2f, 0.3f, 0.4f));
			Assert::IsTrue(std::string(VpFrameAnalysis::P1Gpu::FillBody()).find("vpu_half_luma(src, p)") != std::string::npos);
		}
	};
}
