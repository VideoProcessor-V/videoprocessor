// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only

// QKP slice B at module level (lead10/design-queue-keyed-products.md s3, s6): the queue window is derived at Begin from the feeds a
// renderer delivers WITHOUT the lane. MEMO-T4b NoPresentFactsKeepsRenderAheadDepth, MEMO-T6b FlushThenImmediateAheadAcquireNeverOldBytes
// (epoch moves at the OnQueue call), ResetThenFlushCountsOneDiscontinuity, BacklogFlush releases nothing, and the unwired terms are named.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "CppUnitTest.h"

#include <VP-V/core/QueueWindow.h>
#include <VP-V/core/SkeletonAcquire.h>
#include <VP-V/modules/control/skeleton/SkeletonModule.h>

#include <memory>
#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace QueueWindowWiringTestsNs
{
	namespace RE = RendererExtension;
	using Vpv::Skeleton::SkeletonModule;

	struct FakeHost { int dummy = 0; };

	struct Rig
	{
		Rig() : sk(Vpv::Skeleton::ConfigLoader(), [this]() { return clock += 40000; }) { sk.MemoFor(&host, 1); }
		void Queue(RE::QueueEvent kind)
		{
			RE::QueueFacts q; q.host = &host; q.kind = kind; q.generation = 1;
			sk.OnQueue(q);
		}
		void Depth(double frames)
		{
			RE::LifecycleFacts f; f.host = &host; f.kind = RE::Lifecycle::QueueDepth; f.value = frames;
			sk.OnLifecycle(f);
		}
		void Begin(int64_t seq)
		{
			RE::FrameContext c; c.host = &host; c.phase = RE::FramePhase::Begin; c.seq = seq;
			sk.OnFrame(c);
		}
		uint64_t clock = 1000;
		FakeHost host;
		SkeletonModule sk;
	};

	TEST_CLASS(QueueWindowWiringTests)
	{
	public:
		// MEMO-T4b: QueueDepth only (no AfterPresent, no lane): the window carries R, P is named unwired (no AfterPresent), H and the ahead need are fed.
		TEST_METHOD(NoPresentFactsKeepsRenderAheadDepth)
		{
			Rig r;
			r.Depth(3.0);
			r.Begin(10);
			const VpSkeleton::QueueWindow w = r.sk.MemoOf(&r.host)->Window();
			Assert::AreEqual(3, int(w.renderAhead));
			Assert::IsTrue((w.wired & VpSkeleton::QueueWindowTerms::kRenderAhead) != 0);
			Assert::IsTrue((w.wired & VpSkeleton::QueueWindowTerms::kPresentLag) == 0);
			Assert::IsTrue((w.wired & VpSkeleton::QueueWindowTerms::kAheadNeed) != 0);   // fed by the memo's accessor: no consumer declared
			Assert::IsTrue((w.wired & VpSkeleton::QueueWindowTerms::kHistory) != 0);
			Assert::IsTrue(w.back >= 3);
			VpSkeleton::QueueWindow b;
			Assert::IsTrue(r.sk.WindowBoardOf(&r.host)->Read(&b));
			Assert::AreEqual(uint64_t(10), b.current);
		}

		// MEMO-T6b: the epoch moves AT the OnQueue call, before any Begin (no pre-flush spot can be read afterwards).
		TEST_METHOD(FlushMovesTheEpochAtOnceAndResetThenFlushIsOneDiscontinuity)
		{
			Rig r;
			r.Begin(10);
			VpSkeleton::SkeletonMemo* m = r.sk.MemoOf(&r.host);
			const uint64_t e0 = m->Epoch();
			const uint64_t d0 = m->Discontinuities();
			r.Queue(RE::QueueEvent::Reset);
			r.Queue(RE::QueueEvent::Flush);
			Assert::IsTrue(m->Epoch() >= e0 + 2);
			Assert::AreEqual(d0 + 1, m->Discontinuities());
		}

		// BacklogFlush is a timing restart, not a generation: nothing is released, the epoch stays.
		TEST_METHOD(BacklogFlushReleasesNothing)
		{
			Rig r;
			r.Begin(10);
			VpSkeleton::SkeletonMemo* m = r.sk.MemoOf(&r.host);
			const uint64_t e0 = m->Epoch();
			r.Queue(RE::QueueEvent::BacklogFlush);
			Assert::AreEqual(e0, m->Epoch());
		}

		// ORIG-T1: a host with no memo (Original pathway / never asked) is untouched: no board, no epoch work.
		TEST_METHOD(AHostWithoutAMemoHasNoBoard)
		{
			SkeletonModule sk;
			FakeHost h;
			RE::QueueFacts q; q.host = &h; q.kind = RE::QueueEvent::Flush;
			sk.OnQueue(q);
			Assert::IsTrue(sk.WindowBoardOf(&h) == nullptr);
			Assert::IsTrue(sk.MemoOf(&h) == nullptr);
		}
	};
}
