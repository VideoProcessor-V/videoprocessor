// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// P10 D5 phase A (refs/118): the P13 source ring (seq + generation keyed, graveyard release at Begin, demand gate), the
// non-mutating EarlyUploadHandoff::PeekAttached, and the lane tap-source decision (every cause visible). CPU only.
#include "pch.h"
#include "CppUnitTest.h"
#include <VP-V/modules/input/capture/SourceRing.h>
#include <VP-V/modules/input/capture/EarlyUploadHandoff.h>
#include <VP-V/modules/input/source_analysis/lane/shared/TapSource.h>

#include <memory>
#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	TEST_CLASS(SourceRingTests)
	{
	public:
		using Ring = Vpv::Capture::SourceRing;
		using Tap = Vpv::TapSource::Cause;

		static RendererExtension::AheadSlot Slot(uint64_t seq, uint64_t generation, std::shared_ptr<EarlyUploadHandoff> h,
			VideoFrameEncoding enc = VideoFrameEncoding::P010, uint32_t rowBytes = 3840 * 2, int height = 2160)
		{
			RendererExtension::AheadSlot s;
			s.seq = seq;
			s.generation = generation;
			s.earlyHandoff = std::static_pointer_cast<void>(h);
			s.rawEncoding = static_cast<int32_t>(enc);
			s.rawRowBytes = rowBytes;
			s.height = height;
			return s;
		}

		TEST_METHOD(AnUnwantedRingTakesNothingAndAnyWantedFollowsTheDemand)
		{
			const bool before = Ring::AnyWanted();
			Ring r;
			auto h = std::make_shared<EarlyUploadHandoff>();
			const RendererExtension::AheadSlot s[1] = { Slot(1, 1, h) };
			r.Update(s, 1);
			Assert::AreEqual<size_t>(0, r.GetStats().entries);
			r.SetWanted(true);
			Assert::IsTrue(Ring::AnyWanted());
			r.SetWanted(true);   // idempotent
			r.SetWanted(false);
			Assert::IsTrue(Ring::AnyWanted() == before, L"one wanted ring, counted once");
		}

		TEST_METHOD(EntriesAreKeyedBySeqAndGenerationAndLeaveToTheGraveyardUntilBegin)
		{
			Ring r;
			r.SetWanted(true);
			auto a = std::make_shared<EarlyUploadHandoff>();
			auto b = std::make_shared<EarlyUploadHandoff>();
			const RendererExtension::AheadSlot first[2] = { Slot(10, 1, a), Slot(11, 1, b) };
			r.Update(first, 2);
			Assert::AreEqual<size_t>(2, r.GetStats().entries);
			RendererExtension::AheadSlot got;
			Assert::IsTrue(r.Find(10, 1, got) && got.earlyHandoff == std::static_pointer_cast<void>(a));
			got = RendererExtension::AheadSlot();
			Assert::IsFalse(r.Find(10, 2, got), L"the same seq in another generation is another frame");
			// frame 10 leaves the queue, frame 12 arrives
			auto c = std::make_shared<EarlyUploadHandoff>();
			const RendererExtension::AheadSlot second[2] = { Slot(11, 1, b), Slot(12, 1, c) };
			r.Update(second, 2);
			Assert::IsFalse(r.Find(10, 1, got));
			Assert::AreEqual<size_t>(1, r.GetStats().graveyard);
			const long held = a.use_count();
			Assert::IsTrue(held >= 2, L"nothing is destroyed under the queue lock: the graveyard still holds frame 10");
			Assert::AreEqual<size_t>(1, r.ReleaseGraveyard());
			Assert::IsTrue(a.use_count() == held - 1, L"the reference drops at Begin");
			Assert::AreEqual<size_t>(0, r.GetStats().graveyard);
			// an empty pass clears the ring (the queue ran dry)
			r.Update(nullptr, 0);
			Assert::AreEqual<size_t>(0, r.GetStats().entries);
			r.ReleaseGraveyard();
			r.SetWanted(false);
		}

		TEST_METHOD(AFullRingCountsWhatItRefuses)
		{
			Ring r;
			r.SetWanted(true);
			auto h = std::make_shared<EarlyUploadHandoff>();
			RendererExtension::AheadSlot s[Ring::kCapacity + 2];
			for (size_t i = 0; i < Ring::kCapacity + 2; ++i)
				s[i] = Slot(i + 1, 1, h);
			r.Update(s, Ring::kCapacity + 2);
			Assert::AreEqual<size_t>(Ring::kCapacity, r.GetStats().entries);
			Assert::AreEqual<uint64_t>(2, r.GetStats().overCapacity);
			r.SetWanted(false);
			Assert::AreEqual<size_t>(0, r.GetStats().entries);
			r.ReleaseGraveyard();
		}

		TEST_METHOD(PeekAttachedNeverAbandonsOrConsumesTheRendersTake)
		{
			EarlyUploadHandoff pending;
			EarlyUploadHandoff::State st{};
			Assert::IsTrue(pending.PeekAttached(&st) == nullptr);
			Assert::IsTrue(st == EarlyUploadHandoff::State::Pending);
			Assert::IsTrue(pending.BeginUpload(), L"a peek left the upload runnable (no Abandon)");
			EarlyUploadHandoff ready;
			auto slot = std::make_shared<int>(5);
			Assert::IsTrue(ready.BeginUpload());
			ready.Finish(slot);
			Assert::IsTrue(ready.PeekAttached(&st) == slot && st == EarlyUploadHandoff::State::Attached);
			Assert::IsTrue(ready.PeekAttached() == slot);
			EarlyUploadHandoff::Outcome o{};
			bool first = false;
			Assert::IsTrue(ready.Take(&o, &first) == slot);
			Assert::IsTrue(first, L"the render's first Take is still the first (peeks set no taken flag)");
			Assert::IsTrue(o == EarlyUploadHandoff::Outcome::Early);
		}

		TEST_METHOD(TheTapDecisionNamesEveryAbsentCause)
		{
			Ring r;
			r.SetWanted(true);
			using Vpv::TapSource::Mode;
			using Vpv::TapSource::Pick;
			Pick p;
			Assert::IsTrue(Vpv::TapSource::Decide(Mode::Cpu, r, 1, 1, false, 3840, 2160, p) == Tap::SettingCpu);
			Assert::IsTrue(Vpv::TapSource::Decide(Mode::Auto, r, 1, 1, false, 3840, 2160, p) == Tap::NotInRing);

			auto none = std::make_shared<EarlyUploadHandoff>();
			RendererExtension::AheadSlot noRaw = Slot(2, 1, none);
			noRaw.earlyHandoff.reset();
			auto pendingH = std::make_shared<EarlyUploadHandoff>();
			auto okH = std::make_shared<EarlyUploadHandoff>();
			Assert::IsTrue(okH->BeginUpload());
			auto texture = std::make_shared<int>(1);
			okH->Finish(texture);
			auto abandoned = std::make_shared<EarlyUploadHandoff>();
			EarlyUploadHandoff::Outcome o{};
			abandoned->Take(&o);   // Pending -> Abandoned
			const RendererExtension::AheadSlot s[8] = {
				noRaw,
				Slot(3, 1, pendingH),
				Slot(4, 1, okH),
				Slot(5, 1, abandoned),
				Slot(6, 1, okH, VideoFrameEncoding::BGRA_8BIT),
				Slot(7, 1, okH, VideoFrameEncoding::NV12),
				Slot(8, 1, okH, VideoFrameEncoding::P010, 64, 2160),
				Slot(9, 1, okH, VideoFrameEncoding::V210, 3840 * 16 / 6 + 16, 2160) };
			r.Update(s, 8);
			Assert::IsTrue(Vpv::TapSource::Decide(Mode::Auto, r, 2, 1, false, 3840, 2160, p) == Tap::NoRawPayload);
			Assert::IsTrue(Vpv::TapSource::Decide(Mode::Auto, r, 3, 1, false, 3840, 2160, p) == Tap::UploadPending);
			Assert::IsTrue(Vpv::TapSource::Decide(Mode::Auto, r, 4, 1, false, 3840, 2160, p) == Tap::None);
			Assert::IsTrue(p.slot == texture);
			Assert::IsTrue(Vpv::TapSource::Decide(Mode::Auto, r, 4, 1, true, 3840, 2160, p) == Tap::FullRange);
			Assert::IsTrue(Vpv::TapSource::Decide(Mode::Auto, r, 5, 1, false, 3840, 2160, p) == Tap::NoEarlySlot);
			Assert::IsTrue(Vpv::TapSource::Decide(Mode::Auto, r, 6, 1, false, 3840, 2160, p) == Tap::FormatRgb);
			Assert::IsTrue(Vpv::TapSource::Decide(Mode::Auto, r, 7, 1, false, 3840, 2160, p) == Tap::FormatOther);
			Assert::IsTrue(Vpv::TapSource::Decide(Mode::Auto, r, 8, 1, false, 3840, 2160, p) == Tap::SizeMismatch);
			Assert::IsTrue(Vpv::TapSource::Decide(Mode::Auto, r, 4, 1, false, 3840, 4000, p) == Tap::SizeMismatch, L"a short slot");
			// the decisions only peeked: the pending hand-off is still pending, no Take was consumed
			Assert::IsTrue(pendingH->Peek() == EarlyUploadHandoff::State::Pending);
			r.SetWanted(false);
			r.ReleaseGraveyard();
		}

		TEST_METHOD(TheCountersAndTheDumpTextShowEveryCause)
		{
			Vpv::TapSource::Counters c;
			Assert::AreEqual<uint64_t>(0, c.Fallbacks());
			c.Count(Tap::None);
			c.Count(Tap::UploadPending);
			c.Count(Tap::UploadPending);
			c.Count(Tap::FullRange);
			Assert::AreEqual<uint64_t>(3, c.Fallbacks());
			Assert::IsTrue(c.Last() == Tap::FullRange);
			const std::string text = Vpv::TapSource::Describe(c);
			Assert::IsTrue(text.find("gpu 1") != std::string::npos);
			Assert::IsTrue(text.find("upload pending 2") != std::string::npos);
			Assert::IsTrue(text.find("full range 1") != std::string::npos);
			Assert::IsTrue(text.find("last: full range") != std::string::npos);
		}
	};
}
