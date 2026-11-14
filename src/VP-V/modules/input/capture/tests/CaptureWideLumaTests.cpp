// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// S-2a GPU twin parity: GpuRgbUnpack's opt-in wide R12 matcher luma (Q6 fractions on the 10-bit axis, AnalysisAxis10.h
// au_axis_q6_scale) against the CPU law (Luma12FromRgb12 + AxisQ6 Scale), and the default-off path staying the legacy
// ten-bit luma (<< 6) with the wide path within one ten-bit code of it. New file: CaptureUnpackHookTests is not edited.
#include "pch.h"
#include <VP-V/tests/VpLutGpuTestSupport.h>
#include <VP-V/modules/input/source_analysis/AnalysisAxis10.h>

namespace VideoProcessorTest
{
	TEST_CLASS(CaptureWideLumaTests)
	{
	public:
		TEST_METHOD(WideLumaMatchesTheAxisLawAndOffStaysLegacy)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			GpuRgbUnpack unpack(fixture.Log(), gpu);
			unpack.SetHostReadableForTest(true);
			auto download = [&](pl_tex tex)
			{
				std::vector<uint16_t> samples(static_cast<size_t>(tex->params.w) * tex->params.h * tex->params.format->num_components);
				pl_tex_transfer_params d{};
				d.tex = tex;
				d.ptr = samples.data();
				Assert::IsTrue(pl_tex_download(gpu, &d), L"plane not host-readable");
				return samples;
			};
			uint32_t seed = 0x51ed270bu;
			auto next = [&seed]() { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; };
			const int64_t bt709[3] = { 13933, 46871, 4732 }, rec2020[3] = { 17218, 44444, 3886 };
			bool sawFraction = false;
			for (const VideoFrameEncoding encoding : { VideoFrameEncoding::R12B, VideoFrameEncoding::R12L })
				for (const bool bt2020 : { false, true })
				{
					const int width = 104, height = 40;
					VideoStateComPtr state = new VideoState();
					state->valid = true;
					state->displayMode = std::make_shared<DisplayMode>(width, height, false, 24000, 1001);
					state->videoFrameEncoding = encoding;
					state->colorspace = bt2020 ? ColorSpace::BT_2020 : ColorSpace::REC_709;
					const size_t rowBytes = static_cast<size_t>(state->BytesPerRow());
					std::vector<BYTE> source(static_cast<size_t>(state->BytesPerFrame()));
					for (BYTE& b : source) b = static_cast<BYTE>(next());
					pl_tex rgb = nullptr, legacyLuma = nullptr, wideLuma = nullptr, wideRgb = nullptr;
					unpack.SetWideLuma(false);
					Assert::IsTrue(unpack.Unpack(encoding, bt2020, source.data(), rowBytes, width, height, rgb, legacyLuma));
					const std::vector<uint16_t> off = download(legacyLuma), picture = download(rgb);
					unpack.SetWideLuma(true);
					Assert::IsTrue(unpack.Unpack(encoding, bt2020, source.data(), rowBytes, width, height, wideRgb, wideLuma));
					const std::vector<uint16_t> on = download(wideLuma);
					unpack.SetWideLuma(false);   // the default again
					const int64_t* cy = bt2020 ? rec2020 : bt709;
					for (size_t i = 0; i < static_cast<size_t>(width) * height; ++i)
					{
						// The picture holds the 12-bit value << 4: recover p exactly and run the CPU law.
						const int64_t p0 = picture[i * 4] >> 4, p1 = picture[i * 4 + 1] >> 4, p2 = picture[i * 4 + 2] >> 4;
						const uint32_t y12 = Vpv::AnalysisDomain::Luma12FromRgb12(cy[0], cy[1], cy[2], p0, p1, p2);
						const uint32_t want = Vpv::AnalysisDomain::AxisQ6(y12, 4095, Vpv::AnalysisDomain::AxisLaw::Scale);
						if (on[i] != want)
							Assert::Fail((std::wstring(L"wide luma differs from the CPU axis law at ") + std::to_wstring(i) + L": " +
								std::to_wstring(on[i]) + L" vs " + std::to_wstring(want)).c_str());
						Assert::AreEqual(static_cast<uint32_t>(0), static_cast<uint32_t>(off[i] & 63u));   // legacy: whole ten-bit codes
						const int gap = static_cast<int>(on[i]) - static_cast<int>(off[i]);
						Assert::IsTrue(gap > -130 && gap < 130);   // wide is the same luma to within two ten-bit codes
						if ((on[i] & 63u) != 0u) sawFraction = true;
					}
					pl_tex_destroy(gpu, &rgb);
					pl_tex_destroy(gpu, &legacyLuma);
					pl_tex_destroy(gpu, &wideLuma);
					pl_tex_destroy(gpu, &wideRgb);
				}
			Assert::IsTrue(sawFraction);
		}
	};
}
