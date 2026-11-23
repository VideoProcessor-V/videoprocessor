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

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <memory>
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
		// Pixel `code` carries intensity min(code + offset, 4095): distinct offsets give distinct ramps.
		explicit Ramp(bool fullRange, uint32_t offset = 0)
		{
			const int width = 64, height = 64;
			bytes.assign(static_cast<size_t>(width) * height * 4, 0);
			for (uint32_t code = 0; code < 4096u; ++code)
			{
				const uint32_t intensity = (std::min)(code + offset, 4095u);
				const uint32_t word = ((intensity >> 4) << 10) | (intensity & 15u);
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

	// The production path: Host::OnArrival begins the record, the worker drains it, and the registered sink gets the frame's tap view.
	struct Delivery
	{
		bool got = false;
		SourceFrameView view;
	};

	Delivery ThroughHost(const AnalysisLumaSource& in, uint64_t seq)
	{
		VpFrameAnalysis::Host host;
		Delivery d;
		Tap::Register(&host, [&d](const SourceFrameView& v) { d.got = true; d.view = v; });
		host.Owner(&host).Configure(true, false, false);
		host.Worker().Stop();
		AnalysisLumaSource source = in;
		VpFrameAnalysis::Host::AttachIfEnabled(source);
		host.OnArrival(seq, source, FrameAnalysis::Rect{}, 0, false);
		host.Worker().Drain();
		Tap::Unregister(&host);
		host.Configure(false);
		return d;
	}

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
					// The axis value of the sample is Q6 = MsbAlign(raw); q6 / 64 is the legacy ten-bit floor (4d removed Floor10FromQ6).
					const uint32_t q6 = view.MsbAlign(tap);
					Assert::AreEqual<uint32_t>(AxisQ6(tap, CodeMaximum(12)), q6);
					AnalysisLumaSample sample;
					Assert::IsTrue(ramp.source.Sample(static_cast<int>(code % 64), static_cast<int>(code / 64), sample));
					const int diff = std::abs(static_cast<int>(q6 / 64u) - static_cast<int>(sample.luma));
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

		// W2(i): the invariant that makes a law refusal unreachable: for a packed DV source, a valid frame is always a supported one.
		// Sweeps every encoding x alignment x width; at least one combination must be valid (the sweep is not vacuous).
		TEST_METHOD(DvValidImpliesTapSupported)
		{
			const int widths[] = { 0, 1, 2, 63, 64 };
			int valid = 0;
			for (int e = 0; e <= static_cast<int>(VideoFrameEncoding::P016); ++e)
				for (int alignment = 0; alignment <= 3; ++alignment)
					for (int width : widths)
					{
						const VideoFrameEncoding encoding = static_cast<VideoFrameEncoding>(e);
						const size_t rowBytes = static_cast<size_t>(4 * (width > 1 ? width : 1));
						std::vector<uint8_t> bytes(rowBytes * 2, 0);
						AnalysisLumaSource source;
						source.format = AnalysisLumaFormat::PackedDolbyVision422;
						source.encoding = encoding;
						source.width = width;
						source.height = 2;
						source.rgbBitAlignment = alignment;
						source.data = bytes.data();
						source.rowBytes = rowBytes;
						source.dataBytes = rowBytes * 2;
						if (!source.IsValid())
							continue;
						++valid;
						Assert::IsTrue(DvTunnelTapLaw::Supports(encoding, alignment, width),
							L"a valid packed DV source is never refused by the 12-bit law");
					}
			Assert::IsTrue(valid > 0, L"the sweep reaches a valid source");
		}

		// W2(iv): the one reachable refusal is an unallocatable 12-bit plane. A valid DV source of 2^22 x 2^18 samples needs a 2 TB plane:
		// SourceViewOf12 returns false, counts one refusal, and no 12-bit decode is counted. SourceViewOf12 ONLY: TapViewOf or Process
		// would go on to run the (unguarded) 10-bit decode of the same size.
		TEST_METHOD(AllocationFailureIsTheVisibleRefusal)
		{
			ComputeOnce::SetEnabled(true);
			ComputeOnce::Global().Reset();
			VpFrameAnalysis::ResetDvTapRefusals();
			static const uint8_t neverRead = 0;
			AnalysisLumaSource source;
			source.format = AnalysisLumaFormat::PackedDolbyVision422;
			source.encoding = VideoFrameEncoding::R210;
			source.rgbBitAlignment = 2;
			source.width = 1 << 22;
			source.height = 1 << 18;
			source.rowBytes = static_cast<size_t>(source.width) * 4;
			source.dataBytes = source.rowBytes * static_cast<size_t>(source.height);
			source.data = &neverRead;
			const bool valid = source.IsValid();
			SourceFrameView view;
			::ComputeOnce::FrameScope scope(1, "test", 0);   // unscoped notes go to Gaps(), not Rows(): the row check needs a frame
			const bool served = valid && VpFrameAnalysis::SourceViewOf12(source, view);
			const uint64_t refusals = VpFrameAnalysis::DvTapRefusals();
			bool anyTap12 = false;
			for (const ComputeOnce::UnitRow& r : ComputeOnce::Global().Rows())
				if (r.unit == "analysis.tap12") anyTap12 = true;
			ComputeOnce::Global().Reset();
			ComputeOnce::SetEnabled(false);
			VpFrameAnalysis::ResetDvTapRefusals();
			Assert::IsTrue(valid, L"a valid DV source: only the allocation can refuse it");
			Assert::IsFalse(served, L"no 12-bit plane: no view");
			Assert::AreEqual<uint64_t>(1, refusals, L"the refusal is counted (the Ctrl+I line reads it)");
			Assert::IsFalse(anyTap12, L"no 12-bit decode ran");
		}

		// W5: every non-DV source, through the Host, delivers its ten-bit plane unchanged: msbShift 6 and every pixel is the sampler's own
		// Sample().luma (an independent decode path from the plane the tap reads).
		TEST_METHOD(NonDvTapThroughHostIsTheTenBitSample)
		{
			uint64_t seq = 0;
			int kinds = 0;
			for (int k = 0; k < static_cast<int>(Kind::Count); ++k)
			{
				const Kind kind = static_cast<Kind>(k);
				if (kind == Kind::PackedDvR210) continue;
				Frame frame = Make(kind, 2);
				const Delivery d = ThroughHost(frame.source, ++seq);
				Assert::IsTrue(d.got, KindName(kind));
				Assert::AreEqual<int>(6, d.view.msbShift, KindName(kind));
				Assert::AreEqual<uint32_t>(static_cast<uint32_t>(kWidth), d.view.width, KindName(kind));
				Assert::AreEqual<uint32_t>(static_cast<uint32_t>(kHeight), d.view.height, KindName(kind));
				int mismatches = 0;
				for (int y = 0; y < kHeight; ++y)
					for (int x = 0; x < kWidth; ++x)
					{
						AnalysisLumaSample sample;
						if (!frame.source.Sample(x, y, sample) ||
							d.view.luma[static_cast<size_t>(y) * d.view.strideSamples + x] != sample.luma)
							++mismatches;
					}
				Assert::AreEqual(0, mismatches, KindName(kind));
				++kinds;
			}
			Assert::AreEqual(static_cast<int>(Kind::Count) - 1, kinds, L"every non-DV kind was checked");
			VpFrameAnalysis::ReleasePlanePool();
		}

		// W5: a DV source through the Host delivers the 12-bit law: limited codes are the ramp (4096 distinct values), full-range codes
		// are FullLumaToLimited12, the ten-bit floor of the Q6 value is the legacy Sample() (within one code for full range); and a
		// synthetic DV frame at the scene size delivers a 12-bit view whose ten-bit floor is its Sample().
		TEST_METHOD(DvTapThroughHostIsTheTwelveBitLaw)
		{
			using namespace Vpv::AnalysisDomain;
			uint64_t seq = 100;
			for (int pass = 0; pass < 2; ++pass)
			{
				const bool full = pass == 1;
				Ramp ramp(full);
				const Delivery d = ThroughHost(ramp.source, ++seq);
				Assert::IsTrue(d.got);
				Assert::AreEqual<int>(4, d.view.msbShift);
				Assert::AreEqual<uint64_t>(seq, d.view.seq);
				Assert::AreEqual<uint32_t>(64, d.view.width);
				Assert::AreEqual<uint32_t>(64, d.view.height);
				std::set<uint16_t> distinct;
				int maxDiff = 0;
				for (uint32_t code = 0; code < 4096u; ++code)
				{
					const uint16_t tap = d.view.luma[code];
					distinct.insert(tap);
					const uint16_t expect = full ? DvTunnelTapLaw::FullLumaToLimited12(static_cast<int>(code)) : static_cast<uint16_t>(code);
					Assert::AreEqual<int>(expect, tap, L"the delivered tap is the DvTunnelTapLaw code");
					AnalysisLumaSample sample;
					Assert::IsTrue(ramp.source.Sample(static_cast<int>(code % 64), static_cast<int>(code / 64), sample));
					const int diff = std::abs(static_cast<int>(d.view.MsbAlign(tap) / 64u) - static_cast<int>(sample.luma));
					if (diff > maxDiff) maxDiff = diff;
				}
				if (!full)
				{
					Assert::AreEqual<size_t>(4096, distinct.size(), L"limited ramp: 4096 distinct codes");
					Assert::AreEqual<int>(0, maxDiff, L"limited: the ten-bit floor equals the legacy Sample()");
				}
				else
					Assert::IsTrue(maxDiff <= 1, L"full range: within one code of the legacy Sample()");
			}
			Frame frame = Make(Kind::PackedDvR210, 3);
			const Delivery d = ThroughHost(frame.source, ++seq);
			Assert::IsTrue(d.got);
			Assert::AreEqual<int>(4, d.view.msbShift);
			Assert::AreEqual<uint32_t>(static_cast<uint32_t>(kWidth), d.view.width);
			Assert::AreEqual<uint32_t>(static_cast<uint32_t>(kHeight), d.view.height);
			int mismatches = 0;
			for (int y = 0; y < kHeight; ++y)
				for (int x = 0; x < kWidth; ++x)
				{
					AnalysisLumaSample sample;
					if (!frame.source.Sample(x, y, sample) ||
						(d.view.MsbAlign(d.view.luma[static_cast<size_t>(y) * d.view.strideSamples + x]) / 64u) != sample.luma)
						++mismatches;
				}
			Assert::AreEqual(0, mismatches, L"the ten-bit floor of every 12-bit tap is the legacy Sample()");
			VpFrameAnalysis::ReleasePlanePool();
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
			size_t tap12Rows = 0;
			const ComputeOnce::UnitRow* row = nullptr;
			for (const ComputeOnce::UnitRow& r : rows)
				if (r.unit == "analysis.tap12")
				{
					++tap12Rows;
					if (r.variant == "000000000000000c") row = &r;
				}
			Tap::Unregister(&host);
			host.Configure(false);
			ComputeOnce::Global().Reset();
			ComputeOnce::SetEnabled(false);
			Assert::AreEqual<size_t>(4, got.size());
			for (size_t i = 0; i < got.size(); ++i)
			{
				Assert::AreEqual<int>(4, got[i].msbShift);
				Assert::AreEqual<uint64_t>(i + 1, got[i].seq, L"the view carries its frame's sequence");
			}
			Assert::AreEqual<size_t>(1, tap12Rows, L"one tap12 row (the decode); no private look-ahead row");
			Assert::IsNotNull(row);
			Assert::AreEqual<uint64_t>(4, row->frames);
			Assert::AreEqual<uint64_t>(4, row->calls);
			Assert::AreEqual<uint32_t>(1, row->maxPerFrame, L"one 12-bit decode per frame");
			Assert::AreEqual<uint64_t>(0, row->dupFrames);
		}

		// Tap AND measure wanted, driven through the Host (the production path: OnArrival begins the record, the worker drains): the
		// frame's ONE 12-bit decode feeds both (rule 1, counted under analysis.tap12 variant 12); the measured stats carry the 12-bit
		// sub-code part; the retained 10-bit decode is its own counted unit.
		TEST_METHOD(MeasureAndTapShareOneTwelveBitDecode)
		{
			ComputeOnce::SetEnabled(true);
			ComputeOnce::Global().Reset();
			VpFrameAnalysis::ResetDvTapRefusals();
			VpFrameAnalysis::Host host;
			std::vector<SourceFrameView> got;
			Tap::Register(&host, [&got](const SourceFrameView& v) { got.push_back(v); });
			host.Owner(&host).Configure(true, false, true);   // third argument = record readers = "measure"
			host.Worker().Stop();
			Ramp ramp(false);
			for (uint64_t seq = 1; seq <= 4; ++seq)
			{
				AnalysisLumaSource source = ramp.source;
				VpFrameAnalysis::Host::AttachIfEnabled(source);
				host.OnArrival(seq, source, FrameAnalysis::Rect{}, 0, false);
				host.Worker().Drain();
			}
			const auto rows = ComputeOnce::Global().Rows();
			size_t tap12Rows = 0;
			const ComputeOnce::UnitRow* decode = nullptr;
			const ComputeOnce::UnitRow* luma10 = nullptr;
			for (const ComputeOnce::UnitRow& r : rows)
			{
				if (r.unit == "analysis.tap12")
				{
					++tap12Rows;
					if (r.variant == "000000000000000c") decode = &r;
				}
				if (r.unit == "analysis.luma10") luma10 = &r;
			}
			FrameAnalysis::Record records[4];
			FrameAnalysis::ReadStatus status[4];
			for (uint64_t seq = 1; seq <= 4; ++seq)
				status[seq - 1] = host.Store().Read(seq, FrameAnalysis::Use::Temporal, FrameAnalysis::kMeasuredGroups, records[seq - 1]);
			const uint64_t refusals = VpFrameAnalysis::DvTapRefusals();
			Tap::Unregister(&host);
			host.Configure(false);
			ComputeOnce::Global().Reset();
			ComputeOnce::SetEnabled(false);
			Assert::AreEqual<size_t>(1, tap12Rows, L"one tap12 row (the decode); no private look-ahead row");
			Assert::IsNotNull(decode);
			Assert::AreEqual<uint64_t>(4, decode->frames);
			Assert::AreEqual<uint64_t>(4, decode->calls);
			Assert::AreEqual<uint32_t>(1, decode->maxPerFrame, L"measure and tap share ONE 12-bit decode per frame");
			Assert::AreEqual<uint64_t>(0, decode->dupFrames);
			Assert::IsNotNull(luma10, L"the plane was decoded (a sink wants it)");
			Assert::AreEqual<uint32_t>(1, luma10->maxPerFrame, L"the retained 10-bit decode runs once per frame");
			Assert::AreEqual<uint64_t>(0, refusals, L"a served frame is no refusal");
			Assert::AreEqual<size_t>(4, got.size());
			for (size_t i = 0; i < got.size(); ++i)
			{
				Assert::AreEqual<int>(4, got[i].msbShift, L"12-bit tap");
				Assert::AreEqual<uint64_t>(i + 1, got[i].seq, L"the view carries its frame's sequence");
			}
			for (size_t i = 0; i < 4; ++i)
			{
				Assert::IsTrue(status[i] == FrameAnalysis::ReadStatus::Ok);
				Assert::AreEqual<uint32_t>(4095u << 4, records[i].luma.maxQ6, L"the measure read the 12-bit tap");
				Assert::AreEqual<uint32_t>(0, records[i].luma.minQ6);
				Assert::AreEqual<double>(32760.0, records[i].luma.meanQ6);
				Assert::AreEqual<uint64_t>(4096, records[i].luma.samples);
			}
		}

		// W2(ii): an INVALID packed DV frame (alignment 3) is neither measured nor tapped, and it is not a counted refusal: the record
		// stays late, the sink gets nothing and one skip, no 12-bit decode is counted.
		TEST_METHOD(InvalidDvFrameIsNeitherMeasuredNorTapped)
		{
			ComputeOnce::SetEnabled(true);
			ComputeOnce::Global().Reset();
			VpFrameAnalysis::ResetDvTapRefusals();
			FrameAnalysis::Store store(FrameAnalysis::RingDepth(4, 4));
			static int ownerKey = 0;
			std::vector<SourceFrameView> got;
			Tap::Register(&ownerKey, [&got](const SourceFrameView& v) { got.push_back(v); });
			const uint64_t skippedBefore = Tap::Skipped(&ownerKey);
			Ramp ramp(false);
			VpFrameAnalysis::Job job;
			job.identity.seq = 1;
			job.identity.picture = { 0, 0, 64, 64 };
			job.source = ramp.source;
			job.source.rgbBitAlignment = 3;   // not a tunnel placement the tap covers: IsValid() is false
			job.owner = &ownerKey;
			job.decodePlane = false;
			job.measure = true;
			const bool valid = job.source.IsValid();
			const bool begun = store.Begin(job.identity, FrameAnalysis::kMeasuredGroups) != 0;
			VpFrameAnalysis::Process(store, job);
			FrameAnalysis::Record record;
			const FrameAnalysis::ReadStatus status = store.Read(1, FrameAnalysis::Use::Temporal, FrameAnalysis::kMeasuredGroups, record);
			const uint64_t skipped = Tap::Skipped(&ownerKey) - skippedBefore;
			const uint64_t refusals = VpFrameAnalysis::DvTapRefusals();
			bool anyTap12 = false;
			for (const ComputeOnce::UnitRow& r : ComputeOnce::Global().Rows())
				if (r.unit == "analysis.tap12") anyTap12 = true;
			Tap::Unregister(&ownerKey);
			ComputeOnce::Global().Reset();
			ComputeOnce::SetEnabled(false);
			Assert::IsFalse(valid, L"the source is invalid");
			Assert::IsTrue(begun, L"the record was opened");
			Assert::IsTrue(status == FrameAnalysis::ReadStatus::Late, L"nothing was measured: the record stays late");
			Assert::AreEqual<uint64_t>(0, refusals, L"an invalid frame is not a counted refusal");
			Assert::IsFalse(anyTap12, L"no 12-bit decode ran");
			Assert::AreEqual<size_t>(0, got.size(), L"the sink got no view");
			Assert::AreEqual<uint64_t>(1, skipped, L"one skipped frame is noted");
		}

		// An INVALID DV source is not a 12-bit tap refusal (nothing to tap); only a valid source the law refuses is counted.
		TEST_METHOD(InvalidDvSourceIsNotCountedAsRefusal)
		{
			VpFrameAnalysis::ResetDvTapRefusals();
			AnalysisLumaSource empty;
			empty.format = AnalysisLumaFormat::PackedDolbyVision422;
			SourceFrameView view;
			bool refused = true;
			VpFrameAnalysis::TapViewOf(empty, view, &refused);
			Assert::IsFalse(refused);
			Assert::AreEqual<uint64_t>(0, VpFrameAnalysis::DvTapRefusals());
		}

		// W3: the pooled plane is bounded (12) but a full pool allocates an unpooled plane, so a consumer holding many views cannot push
		// frames into the ten-bit fallback. Forty distinct ramps are held at once: every view keeps its own plane (distinct pointers) and
		// still reads ITS ramp (a recycled plane would show another frame's codes), and nothing is refused.
		TEST_METHOD(HeldViewsNeverForceTenBitFallback)
		{
			VpFrameAnalysis::ReleasePlanePool();
			VpFrameAnalysis::ResetDvTapRefusals();
			std::vector<std::unique_ptr<Ramp>> ramps;
			std::vector<SourceFrameView> held;
			std::set<const uint16_t*> planes;
			for (uint32_t i = 0; i < 40u; ++i)
			{
				ramps.emplace_back(new Ramp(false, i));
				SourceFrameView view;
				Assert::IsTrue(VpFrameAnalysis::SourceViewOf12(ramps.back()->source, view), L"a view is always obtainable");
				Assert::AreEqual<int>(4, view.msbShift);
				planes.insert(view.luma);
				held.push_back(view);
			}
			Assert::AreEqual<size_t>(40, planes.size(), L"every held view has its own plane");
			int wrong = 0;
			for (uint32_t i = 0; i < 40u; ++i)
				for (uint32_t code = 0; code < 4096u; ++code)
					if (held[i].luma[code] != (std::min)(code + i, 4095u))
						++wrong;
			Assert::AreEqual(0, wrong, L"each held view still reads its own ramp");
			Assert::IsTrue(VpFrameAnalysis::PlanePoolSize() <= 12u, L"the pool stays bounded");
			Assert::AreEqual<uint64_t>(0, VpFrameAnalysis::DvTapRefusals(), L"no refusal, so no hidden ten-bit fallback");
			held.clear();
			VpFrameAnalysis::ReleasePlanePool();
		}

		// W3: the kept 12-bit views (the source-resolution look-ahead reads a queued frame some frames later) are bounded to half the pool
		// (six), oldest dropped first, keyed by sequence AND the frame's bytes, and dropped by ReleasePlanePool.
		TEST_METHOD(SharedViewsAreBoundedAndKeyed)
		{
			VpFrameAnalysis::ReleasePlanePool();
			VpFrameAnalysis::ResetDvTapRefusals();
			Ramp ramp(false);
			Ramp other(false, 1);
			std::vector<SourceFrameView> shared(9);
			SourceFrameView found;
			for (uint64_t seq = 1; seq <= 7; ++seq)
				Assert::IsTrue(VpFrameAnalysis::ShareViewOf12(seq, ramp.source, shared[seq]));
			Assert::IsFalse(VpFrameAnalysis::FindSharedViewOf12(1, ramp.source, found), L"after seq 7 the oldest (1) is dropped");
			Assert::IsTrue(VpFrameAnalysis::FindSharedViewOf12(2, ramp.source, found), L"the six newest are kept");
			Assert::IsTrue(VpFrameAnalysis::ShareViewOf12(8, ramp.source, shared[8]));
			for (uint64_t seq = 3; seq <= 8; ++seq)
			{
				SourceFrameView again;
				Assert::IsTrue(VpFrameAnalysis::FindSharedViewOf12(seq, ramp.source, again), L"seq 3..8 are found");
				Assert::IsTrue(again.luma == shared[seq].luma, L"the found view is the frame's own decode");
				Assert::AreEqual<int>(4, again.msbShift);
				Assert::IsFalse(VpFrameAnalysis::FindSharedViewOf12(seq, other.source, found), L"another buffer is not the frame");
			}
			Assert::IsFalse(VpFrameAnalysis::FindSharedViewOf12(2, ramp.source, found), L"seq 2 left when 8 arrived");
			Assert::IsFalse(VpFrameAnalysis::FindSharedViewOf12(0, ramp.source, found), L"sequence 0 is never kept");
			Assert::IsTrue(VpFrameAnalysis::PlanePoolSize() <= 12u);
			Assert::AreEqual<uint64_t>(0, VpFrameAnalysis::DvTapRefusals());
			VpFrameAnalysis::ReleasePlanePool();
			for (uint64_t seq = 3; seq <= 8; ++seq)
				Assert::IsFalse(VpFrameAnalysis::FindSharedViewOf12(seq, ramp.source, found), L"released views are gone");
		}
	};
}
