// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// I0 ancestry types and validator (src/VP-V/core/Ancestry.h): verdicts for a published product
// against the current stamp and its recorded ancestors, and the in-order cascade. CPU only.

#include <VP-V/core/Ancestry.h>

#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace AncestryTestsNs
{
	using namespace VpAncestry;
	using VpIndicator::Bit;
	using VpIndicator::CounterMask;
	using VpIndicator::Indicator;
	using VpIndicator::Stamp;
	using VpResync::Cause;

	AncestorRef Ref(uint16_t product, uint16_t slot, uint64_t seq, uint32_t cgen)
	{
		AncestorRef r;
		r.productId = product;
		r.slotIndex = slot;
		r.seq = seq;
		r.cgen = cgen;
		r.prov = Provenance::AheadLane;
		return r;
	}

	// A published header under `stamp`, reading `reads`, with the given ancestors.
	Header Published(uint32_t cgen, uint64_t seq, const Stamp& stamp, CounterMask reads,
	                const std::vector<AncestorRef>& ancestors)
	{
		Header h;
		h.cgen = cgen;
		h.seq = seq;
		h.prov = Provenance::AheadLane;
		h.reads = reads;
		h.honoured = stamp;
		h.ancestorCount = uint8_t(ancestors.size());
		for (size_t i = 0; i < ancestors.size(); ++i)
			h.ancestors[i] = ancestors[i];
		return h;
	}

	SlotView View(uint64_t seq, uint32_t cgen, bool valid = true)
	{
		SlotView v;
		v.present = true;
		v.seq = seq;
		v.cgen = cgen;
		v.valid = valid;
		return v;
	}

	TEST_CLASS(AncestryTests)
	{
	public:
		TEST_METHOD(CgenWrapsPastZero)
		{
			Assert::AreEqual(uint32_t(1), NextCgen(0));
			Assert::AreEqual(uint32_t(8), NextCgen(7));
			Assert::AreEqual(uint32_t(1), NextCgen(0xFFFFFFFFu));
		}

		TEST_METHOD(ValidWhenStampAndAncestorsHold)
		{
			Indicator ind(VpIndicator::NextEra(), 64);
			const Stamp s = ind.StampOf(10);
			Header h = Published(3, 10, s, Bit(VpIndicator::kContinuity), { Ref(1, 0, 10, 5) });
			auto lookup = [](uint16_t, uint16_t) { return View(10, 5); };
			Assert::IsTrue(Validate(h, ind.StampOf(10), lookup) == Verdict::Valid);
			Assert::AreEqual(std::string("valid"), std::string(Token(Verdict::Valid)));
		}

		TEST_METHOD(NeverPublishedWhenCgenIsZero)
		{
			Indicator ind(VpIndicator::NextEra(), 64);
			Header h = Published(0, 10, ind.StampOf(10), 0, {});
			Assert::IsTrue(Validate(h, ind.StampOf(10), [](uint16_t, uint16_t) { return SlotView(); }) == Verdict::NeverPublished);
		}

		TEST_METHOD(StampMismatchOnlyOnReadCounters)
		{
			Indicator ind(VpIndicator::NextEra(), 64);
			Header h = Published(2, 10, ind.StampOf(10), Bit(VpIndicator::kContinuity), {});
			auto none = [](uint16_t, uint16_t) { return SlotView(); };

			ind.Advance(VpIndicator::kSettingsOutput, Cause::SettingsApply, 10);   // not read: still valid
			Assert::IsTrue(Validate(h, ind.StampOf(10), none) == Verdict::Valid);

			ind.Advance(VpIndicator::kTiming, Cause::RateStep, 10);                // not read: still valid
			Assert::IsTrue(Validate(h, ind.StampOf(10), none) == Verdict::Valid);

			ind.Advance(VpIndicator::kContinuity, Cause::MissedFrames, 10);        // read: mismatch
			Assert::IsTrue(Validate(h, ind.StampOf(10), none) == Verdict::StampMismatch);

			// A FULL event implies continuity, so it also mismatches a continuity reader.
			Indicator other(VpIndicator::NextEra(), 64);
			Header g = Published(2, 10, other.StampOf(10), Bit(VpIndicator::kContinuity), {});
			other.Advance(VpIndicator::kFull, Cause::RendererReset, 10);
			Assert::IsTrue(Validate(g, other.StampOf(10), none) == Verdict::StampMismatch);
		}

		TEST_METHOD(AncestorReplacedAfterALiveRefill)
		{
			Indicator ind(VpIndicator::NextEra(), 64);
			Header h = Published(2, 10, ind.StampOf(10), 0, { Ref(1, 0, 10, 5) });
			auto refilled = [](uint16_t, uint16_t) { return View(10, NextCgen(5)); };
			Assert::IsTrue(Validate(h, ind.StampOf(10), refilled) == Verdict::AncestorReplaced);
		}

		TEST_METHOD(AncestorInvalidWhenTheAncestorIsNoLongerValid)
		{
			Indicator ind(VpIndicator::NextEra(), 64);
			Header h = Published(2, 10, ind.StampOf(10), 0, { Ref(1, 0, 10, 5) });
			auto invalid = [](uint16_t, uint16_t) { return View(10, 5, false); };
			Assert::IsTrue(Validate(h, ind.StampOf(10), invalid) == Verdict::AncestorInvalid);
		}

		TEST_METHOD(AncestorGoneWhenAbsentOrTheSeqWasReused)
		{
			Indicator ind(VpIndicator::NextEra(), 64);
			Header h = Published(2, 10, ind.StampOf(10), 0, { Ref(1, 0, 10, 5) });
			Assert::IsTrue(Validate(h, ind.StampOf(10), [](uint16_t, uint16_t) { return SlotView(); }) == Verdict::AncestorGone);
			// N2: the slot now holds a different frame (released and reused before this descendant sealed).
			Assert::IsTrue(Validate(h, ind.StampOf(10), [](uint16_t, uint16_t) { return View(11, 5); }) == Verdict::AncestorGone);
			Assert::AreEqual(std::string("ancestor_gone"), std::string(Token(Verdict::AncestorGone)));
		}

		TEST_METHOD(ValidateInOrderCascadesARefillToEveryDescendant)
		{
			Indicator ind(VpIndicator::NextEra(), 64);
			const Stamp s = ind.StampOf(10);
			// P2 <- (nothing); P3 <- P2; P6 <- P3. Recorded when all were at cgen 4/6/9.
			std::vector<SlotEntry> slots(3);
			slots[0].productId = 2; slots[0].slotIndex = 0; slots[0].header = Published(4, 10, s, 0, {});
			slots[1].productId = 3; slots[1].slotIndex = 0; slots[1].header = Published(6, 10, s, 0, { Ref(2, 0, 10, 4) });
			slots[2].productId = 6; slots[2].slotIndex = 0; slots[2].header = Published(9, 10, s, 0, { Ref(3, 0, 10, 6) });

			std::vector<TableEntry> table(3);
			table[0].productId = 2; table[0].slotIndex = 0; table[0].view = View(10, 4);
			table[1].productId = 3; table[1].slotIndex = 0; table[1].view = View(10, 6);
			table[2].productId = 6; table[2].slotIndex = 0; table[2].view = View(10, 9);

			std::vector<Verdict> verdicts;
			ValidateInOrder(slots, s, table, verdicts);
			Assert::AreEqual(size_t(3), verdicts.size());
			for (Verdict v : verdicts)
				Assert::IsTrue(v == Verdict::Valid);

			// Live refill of P2: its cgen advances. P3 sees a replaced ancestor, and P6, one pass later, an invalid one.
			table[0].view.cgen = NextCgen(4);
			ValidateInOrder(slots, s, table, verdicts);
			Assert::IsTrue(verdicts[0] == Verdict::Valid);
			Assert::IsTrue(verdicts[1] == Verdict::AncestorReplaced);
			Assert::IsTrue(verdicts[2] == Verdict::AncestorInvalid);
			Assert::IsFalse(table[1].view.valid);
			Assert::IsFalse(table[2].view.valid);
			Assert::IsTrue(table[0].view.valid);
		}
	};
}
