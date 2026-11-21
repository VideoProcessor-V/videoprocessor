/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// Slice 21 W21-1: the DV 12-bit lane tap. A packed DV tunnel source delivers the 12-bit tap (msbShift 4 = the Q6 axis) through the
// source frame tap; every other source delivers its unchanged 10-bit view; a refused 12-bit tap falls back to the 10-bit view
// visibly (counted); the 12-bit decode runs once per frame. CPU only. New class in a new file (rule 28: the existing tap tests
// are not edited).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

#include <VP-V/core/ComputeOnce.h>
#include <VP-V/modules/input/signal/DvTunnelTapLaw.h>
#include <VP-V/modules/input/source_analysis/AnalysisAxis10.h>
#include <VP-V/modules/input/source_analysis/SourceFrameTap.h>
#include <VP-V/modules/input/source_analysis/VpFrameAnalysis.h>
#include <VP-V/modules/input/source_analysis/VpFrameAnalysisHost.h>
#include <VP-V/tests/SyntheticAnalysisFrames.h>

#include <cstdint>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace DvTwelveBitTapTestsNs
{
	using namespace SyntheticAnalysisFrames;
	namespace Tap = VpFrameAnalysis::SourceFrameTap;
	using VpSkeleton::SourceFrameView;

	// 4096 r210 words (alignment 2, big endian): pixel `code` carries intensity code, I = hi8 << 4 | lo4.
	struct Ramp
	{
		std::vector<uint8_t> bytes;
		AnalysisLumaSource source;
		explicit Ramp(bool fullRange)
		{
			const int width = 64, height = 64;
			bytes.assign(static_cast<size_t>(width) * height * 4, 0);
			for (uint32_t code = 0; code < 4096u; ++code)
			{
				const uint32_t word = ((code >> 4) << 10) | (code & 15u);
				for (unsigned k = 0; k < 4; ++k) bytes[code * 4 + k] = static_cast<uint8_t>(word >> (24 - 8 * k));
			}
			source.format = AnalysisLumaFormat::PackedDolbyVision422;
			source.encoding = VideoFrameEncoding::R210;
			source.width = width;
			source.height = height;
			source.rgbBitAlignment = 2;
			source.fullRangeYuv = fullRange;
			source.data = bytes.data();
			source.dataBytes = bytes.size();
			source.rowBytes = static_cast<size_t>(width) * 4;
		}
	};

	TEST_CLASS(DvTwelveBitTapTests)
	{
	public:
		TEST_METHOD(DvSourceDeliversTwelveBitTap)
		{
			using namespace Vpv::AnalysisDomain;
			for (int pass = 0; pass < 2; ++pass)
			{
				const bool full = pass == 1;
				Ramp ramp(full);
				SourceFrameView view;
				bool refused = true;
				Assert::IsTrue(VpFrameAnalysis::TapViewOf(ramp.source, view, &refused));
				Assert::IsFalse(refused);
				Assert::AreEqual<int>(4, view.msbShift, L"12-bit tap: code12 << 4 is the Q6 axis");
				Assert::IsTrue(view.Valid());
				std::set<uint16_t> distinct;
				int maxDiff = 0;
				for (uint32_t code = 0; code < 4096u; ++code)
				{
					const uint16_t tap = view.luma[code];
					distinct.insert(tap);
					uint16_t expect = static_cast<uint16_t>(code);
					if (full) expect = DvTunnelTapLaw::FullLumaToLimited12(static_cast<int>(code));
					Assert::AreEqual<int>(expect, tap, L"the tap is the DvTunnelTapLaw code");
					// The axis value of the sample is Q6 = MsbAlign(raw); Floor10FromQ6 is the legacy ten-bit tap.
					const uint32_t q6 = view.MsbAlign(tap);
					Assert::AreEqual<uint32_t>(AxisQ6(tap, CodeMaximum(12)), q6);
					AnalysisLumaSample sample;
					Assert::IsTrue(ramp.source.Sample(static_cast<int>(code % 64), static_cast<int>(code / 64), sample));
					const int diff = std::abs(static_cast<int>(Floor10FromQ6(q6)) - static_cast<int>(sample.luma));
					if (diff > maxDiff) maxDiff = diff;
				}
				if (!full)
				{
					Assert::AreEqual<size_t>(4096, distinct.size(), L"limited ramp: 4096 distinct codes");
					Assert::AreEqual<int>(0, maxDiff, L"limited: Floor10FromQ6 equals the legacy tap");
				}
				else
					Assert::IsTrue(maxDiff <= 1, L"full range: within one code of the legacy tap");
			}
		}

		TEST_METHOD(DvTapRefusalIsVisibleAndFallsBackTo10)
		{
			VpFrameAnalysis::ResetDvTapRefusals();
			Frame frame = Make(Kind::PackedDvR210, 1);
			frame.source.sharedLuma = std::make_shared<AnalysisLumaPlane>();
			Assert::IsTrue(VpFrameAnalysis::DecodeSharedLuma(frame.source));
			AnalysisLumaSource refusedSource = frame.source;
			refusedSource.rgbBitAlignment = 3;   // not a tunnel placement the 12-bit tap covers
			SourceFrameView view;
			bool refused = false;
			Assert::IsTrue(VpFrameAnalysis::TapViewOf(refusedSource, view, &refused), L"the 10-bit view is delivered");
			Assert::IsTrue(refused);
			Assert::AreEqual<int>(6, view.msbShift, L"fell back to the 10-bit tap");
			Assert::AreEqual<uint64_t>(1, VpFrameAnalysis::DvTapRefusals(), L"counted: the Ctrl+I line reads this");
			SourceFrameView good;
			Assert::IsTrue(VpFrameAnalysis::TapViewOf(frame.source, good, &refused));
			Assert::IsFalse(refused);
			Assert::AreEqual<uint64_t>(1, VpFrameAnalysis::DvTapRefusals(), L"a served frame adds no refusal");
			VpFrameAnalysis::ResetDvTapRefusals();
		}

		TEST_METHOD(NonDvSourcesDeliverIdenticalView)
		{
			for (int k = 0; k < static_cast<int>(Kind::Count); ++k)
			{
				if (static_cast<Kind>(k) == Kind::PackedDvR210) continue;
				Frame frame = Make(static_cast<Kind>(k), 2);
				SourceFrameView a, b;
				bool refused = true;
				Assert::IsTrue(VpFrameAnalysis::SourceViewOf(frame.source, a), KindName(static_cast<Kind>(k)));
				Assert::IsTrue(VpFrameAnalysis::TapViewOf(frame.source, b, &refused), KindName(static_cast<Kind>(k)));
				Assert::IsFalse(refused);
				Assert::AreEqual<int>(a.msbShift, b.msbShift);
				Assert::AreEqual<uint32_t>(a.width, b.width);
				Assert::AreEqual<uint32_t>(a.height, b.height);
				Assert::AreEqual<uint32_t>(a.strideSamples, b.strideSamples);
				for (size_t i = 0; i < static_cast<size_t>(a.width) * a.height; ++i)
					if (a.luma[i] != b.luma[i])
						Assert::Fail(L"a non-DV tap view differs from SourceViewOf");
			}
		}

		TEST_METHOD(TwelveBitTapDecodedOncePerFrame)
		{
			ComputeOnce::SetEnabled(true);
			ComputeOnce::Global().Reset();
			VpFrameAnalysis::Host host;
			std::vector<SourceFrameView> got;
			Tap::Register(&host, [&got](const SourceFrameView& v) { got.push_back(v); });
			host.Owner(&host).Configure(true, false, false);
			host.Worker().Stop();
			for (uint64_t seq = 1; seq <= 4; ++seq)
			{
				Frame frame = Make(Kind::PackedDvR210, static_cast<int>(seq));
				VpFrameAnalysis::Host::AttachIfEnabled(frame.source);
				host.OnArrival(seq, frame.source, FrameAnalysis::Rect{}, 0, false);
				host.Worker().Drain();
			}
			const auto rows = ComputeOnce::Global().Rows();
			const ComputeOnce::UnitRow* row = nullptr;
			for (const ComputeOnce::UnitRow& r : rows)
				if (r.unit == "analysis.tap12") row = &r;
			Assert::AreEqual<size_t>(4, got.size());
			for (const SourceFrameView& v : got) Assert::AreEqual<int>(4, v.msbShift);
			Tap::Unregister(&host);
			host.Configure(false);
			ComputeOnce::Global().Reset();
			ComputeOnce::SetEnabled(false);
			Assert::IsNotNull(row);
			Assert::AreEqual<uint64_t>(4, row->frames);
			Assert::AreEqual<uint64_t>(4, row->calls);
			Assert::AreEqual<uint32_t>(1, row->maxPerFrame, L"one 12-bit decode per frame");
			Assert::AreEqual<uint64_t>(0, row->dupFrames);
		}
	};
}
