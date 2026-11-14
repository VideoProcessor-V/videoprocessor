// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// L-B4 step (a): the motion producer's P1 reader over Acquire (P1AcquireSource.h).
//   1. per format: the engine half read through Acquire(kP1HalfLuma, seq, Cpu) is byte-identical to the producer's own
//      HalfLuma8 for the formats whose codes are the shared decode (YUV / 10-bit), and the reader REFUSES (own decode count)
//      the named exception formats (RGB structure luma, raw full-range 4:2:x: FIX-B4-RGB), so the producer keeps its decode;
//   2. the counter test: N producer reads of one frame run the owner fill ONCE (one P1 build per frame), also when the
//      readers are on several threads;
//   3. no memo / a miss: false, how = None, counted unread (the producer keeps its decode and shows it).
// CPU only.

#include <VP-V/core/SkeletonAcquire.h>
#include <VP-V/modules/input/source_analysis/P1AcquireSource.h>
#include <VP-V/modules/input/source_analysis/VpFrameAnalysis.h>
#include <VP-V/modules/input/source_analysis/lane/LaneCpuProducts.h>
#include <VP-V/modules/motion/estimation/MotionAheadProducer.h>
#include <VP-V/tests/SyntheticAnalysisFrames.h>

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace P1AcquireSourceTestsNs
{
	using namespace SyntheticAnalysisFrames;
	using namespace VpSkeleton;

	bool Identical(Kind k)
	{
		return k == Kind::P010 || k == Kind::P210 || k == Kind::DecodedDv422Full || k == Kind::V210 || k == Kind::Uyvy ||
			k == Kind::SdrTunnel || k == Kind::PackedDvR210;
	}

	// The lane's P1 owner fill for one frame: the lane's own CPU law over the frame's tap view; counts its runs.
	struct P1Owner
	{
		AnalysisLumaSource source;
		std::atomic<int> fills{ 0 };

		bool Register(SkeletonMemo& memo)
		{
			OwnerFill f;
			f.fill = [this](const OwnerFillRequest&, SkeletonMemo&, OwnerFillOutput& out) {
				SourceFrameView view;
				if (!VpFrameAnalysis::SourceViewOf(source, view))
					return false;
				out.bytes.assign(VpLaneProducts::HalfLumaBytes(Dims{ view.width, view.height }), 0);
				++fills;
				return VpLaneProducts::ComputeHalfLuma(view, out.bytes.data(), out.bytes.size());
			};
			return memo.RegisterOwnerFill(kP1HalfLuma, AcquireForm::Cpu, SkeletonMemo::OwnerUnitOf(kP1HalfLuma), std::move(f))
				== SkeletonMemo::Registration::Registered;
		}
	};

	TEST_CLASS(P1AcquireSourceTests)
	{
	public:
		TEST_METHOD(EngineHalfThroughAcquireMatchesTheProducerOrIsRefused)
		{
			for (int k = 0; k < static_cast<int>(Kind::Count); ++k)
			{
				const Kind kind = static_cast<Kind>(k);
				const Frame f = Make(kind, 3);
				SkeletonMemo memo;
				P1Owner owner;
				owner.source = f.source;
				Assert::IsTrue(owner.Register(memo), KindName(kind));
				VpFrameAnalysis::P1AcquireSource reader([&memo](uint64_t seq) { return memo.Acquire(kP1HalfLuma, seq, AcquireForm::Cpu); });
				std::vector<uint8_t> got, want;
				int gw = 0, gh = 0, ww = 0, wh = 0;
				P1ReadKind how = P1ReadKind::None;
				const bool read = reader.ReadEngine8(5, f.source, got, gw, gh, how);
				if (Identical(kind))
				{
					Assert::IsTrue(read && how != P1ReadKind::None, KindName(kind));
					Assert::IsTrue(MotionAhead::HalfLuma8(f.source, want, ww, wh), KindName(kind));
					Assert::IsTrue(gw == ww && gh == wh && got == want, KindName(kind));
					Assert::AreEqual<uint64_t>(1u, reader.Acquired(), KindName(kind));
				}
				else
				{
					Assert::IsFalse(read, KindName(kind));
					Assert::IsTrue(how == P1ReadKind::None, KindName(kind));
					Assert::AreEqual<uint64_t>(1u, reader.OwnDecode(), KindName(kind));
					Assert::AreEqual(0, owner.fills.load(), L"a refused format never builds P1 for the producer");
				}
			}
		}

		TEST_METHOD(OneBuildPerFrameForManyProducerReads)
		{
			const Frame f = Make(Kind::P010, 2);
			SkeletonMemo memo;
			P1Owner owner;
			owner.source = f.source;
			Assert::IsTrue(owner.Register(memo));
			VpFrameAnalysis::P1AcquireSource reader([&memo](uint64_t seq) { return memo.Acquire(kP1HalfLuma, seq, AcquireForm::Cpu); });
			std::vector<std::vector<uint8_t>> out(4);
			std::vector<std::thread> pool;
			for (size_t t = 0; t < out.size(); ++t)
				pool.emplace_back([&, t]() {
					int w = 0, h = 0;
					P1ReadKind how = P1ReadKind::None;
					for (int i = 0; i < 3; ++i)
						reader.ReadEngine8(7, f.source, out[t], w, h, how);
				});
			for (std::thread& th : pool) th.join();
			Assert::AreEqual(1, owner.fills.load(), L"one P1 build for 12 reads on 4 threads");
			for (size_t t = 1; t < out.size(); ++t)
				Assert::IsTrue(!out[0].empty() && out[t] == out[0], L"every reader got the one frame's bytes");
			Assert::AreEqual<uint64_t>(12u, reader.Acquired());
			// Another frame builds again (once per frame).
			std::vector<uint8_t> o2;
			int w = 0, h = 0;
			P1ReadKind how = P1ReadKind::None;
			Assert::IsTrue(reader.ReadEngine8(8, f.source, o2, w, h, how));
			Assert::AreEqual(2, owner.fills.load());
		}

		TEST_METHOD(NoMemoOrAMissReadsFalse)
		{
			const Frame f = Make(Kind::P010, 1);
			std::vector<uint8_t> out;
			int w = 0, h = 0;
			P1ReadKind how = P1ReadKind::LiveFill;
			VpFrameAnalysis::P1AcquireSource none;
			Assert::IsFalse(none.ReadEngine8(1, f.source, out, w, h, how));
			Assert::IsTrue(how == P1ReadKind::None);
			// A memo with no owner for P1 (the lane off, no fill): Unavailable, counted unread.
			SkeletonMemo memo;
			VpFrameAnalysis::P1AcquireSource miss([&memo](uint64_t seq) { return memo.Acquire(kP1HalfLuma, seq, AcquireForm::Cpu); });
			how = P1ReadKind::LiveFill;
			Assert::IsFalse(miss.ReadEngine8(1, f.source, out, w, h, how));
			Assert::IsTrue(how == P1ReadKind::None);
			Assert::AreEqual<uint64_t>(1u, miss.Unread());
		}
	};
}
