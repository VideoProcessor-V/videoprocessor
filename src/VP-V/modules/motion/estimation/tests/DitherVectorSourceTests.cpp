// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// W2-C8 steps 3-4 (refs/110 BRIEF C8): the sigma-delta's vector chain (DitherVectorSource.h). CPU only. Each step is forced by
// removing the one above it and the test asserts the step, its status segment and its counter; no step is reachable without a
// status (every Facts combination of the chain's inputs is enumerated); the reuse gate consumes MotionValidity::SpanStatus
// (r2/c2a-f) as coordinator Q1 says: Invalid never reuses, Unknown reuses VISIBLY; spans of 1, 2 and kMaxLead frames answer
// Invalid across a flush, a discontinuity and a settings stamp change, Unknown unbound.
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/core/Indicator.h>
#include <VP-V/modules/motion/estimation/DitherVectorSource.h>
#include <VP-V/modules/motion/estimation/MotionAhead.h>
#include <VP-V/modules/motion/estimation/MotionValidity.h>

#include <memory>
#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	namespace
	{
		namespace DV = VpMotion::DitherVectors;
		using Span = MotionValidity::State::SpanState;
		using Cause = FallbackCatalog::NoPairCause;

		// Every input of step 1 present: the chain takes the shared pair.
		DV::Facts AllPresent()
		{
			DV::Facts f;
			f.slotRuns = true;
			f.patch = true;
			f.ownSetting = false;
			f.motionCompensation = true;
			f.served = true;
			f.sequences = true;
			f.timelineDrew = false;
			f.matcher = true;
			f.geometryValid = true;
			f.engineField = true;
			f.pairServes = true;
			f.span = Span::Valid;
			f.onDemandServed = false;
			f.onDemandCause = Cause::None;
			f.matcherLive = true;
			return f;
		}

		bool Contains(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

		std::wstring W(const std::string& s) { return std::wstring(s.begin(), s.end()); }

		bool Engaged(const std::vector<FallbackCatalog::Decision>& d, const char* id)
		{
			for (const FallbackCatalog::Decision& x : d)
				if (x.id == id)
					return x.engaged;
			Assert::Fail((L"no entry " + W(id)).c_str());
			return false;
		}

		std::shared_ptr<VpIndicator::Indicator> Bound(MotionValidity::State& state)
		{
			auto ind = std::make_shared<VpIndicator::Indicator>(VpIndicator::NextEra(), 64);
			state.Bind(ind);
			ind->SealAndObserve(1);
			ind->SealAndObserve(2);
			return ind;
		}
	}

	TEST_CLASS(DitherVectorSourceTests)
	{
	public:
		TEST_METHOD(EachStepIsTakenWhenTheOneAboveIsRemoved)
		{
			DV::Facts f = AllPresent();
			DV::Choice c = DV::Choose(f);
			Assert::IsTrue(c.step == DV::Step::SharedPair && c.why == DV::Why::None && !c.spanUnknown, L"1: shared pair");

			f.onDemandServed = true;   // the same field, its optical-flow pair made on demand
			c = DV::Choose(f);
			Assert::IsTrue(c.step == DV::Step::OnDemand, L"3: on-demand pair");

			f = AllPresent();
			f.engineField = false;     // no verified field vouches for (seq, next): the matcher's own field of this frame
			c = DV::Choose(f);
			Assert::IsTrue(c.step == DV::Step::MatcherField, L"2: matcher field");

			f.matcherLive = false;     // the matcher did not run for this frame either
			c = DV::Choose(f);
			Assert::IsTrue(c.step == DV::Step::OwnSearch && c.why == DV::Why::NoField, L"4: own search, no field");

			for (const Cause miss : { Cause::OnDemandLate, Cause::OnDemandUnavailable, Cause::OnDemandNoBudget, Cause::OnDemandUncached })
			{
				f.onDemandCause = miss;   // the engine's on-demand pair for it was missed
				c = DV::Choose(f);
				Assert::IsTrue(c.step == DV::Step::OwnSearch && c.why == DV::Why::OnDemandMissed && c.onDemandCause == miss,
					L"4: own search, on-demand missed (cause kept)");
			}

			f = AllPresent();
			f.ownSetting = true;       // dither_vectors own = step 4 directly
			c = DV::Choose(f);
			Assert::IsTrue(c.step == DV::Step::OwnSearch && c.why == DV::Why::OwnSetting, L"own -> 4");

			f = AllPresent();
			f.served = false;          // engine off / absent / not the full engine: the previous behaviour, the own search
			c = DV::Choose(f);
			Assert::IsTrue(c.step == DV::Step::OwnSearch && c.why == DV::Why::NotServed, L"engine absent -> 4 (rule 25 addendum 2)");

			f = AllPresent();
			f.motionCompensation = false;   // the sigma-delta rule's motion_compensation 0 (the shader default)
			c = DV::Choose(f);
			Assert::IsTrue(c.step == DV::Step::Static && c.why == DV::Why::MotionCompensationOff, L"5: static");

			f = AllPresent();
			f.slotRuns = false;        // no sigma-delta: nothing to decide
			Assert::IsTrue(DV::Choose(f).step == DV::Step::None);
		}

		TEST_METHOD(OwnSearchNamesEveryMissingInput)
		{
			struct Row { void (*knock)(DV::Facts&); DV::Why why; };
			const Row rows[] = {
				{ [](DV::Facts& f) { f.patch = false; }, DV::Why::NoPatch },
				{ [](DV::Facts& f) { f.sequences = false; }, DV::Why::NoFrames },
				{ [](DV::Facts& f) { f.timelineDrew = true; }, DV::Why::TimelineDrew },
				{ [](DV::Facts& f) { f.matcher = false; }, DV::Why::NoMatcher },
				{ [](DV::Facts& f) { f.geometryValid = false; }, DV::Why::GeometryInvalid },
				{ [](DV::Facts& f) { f.pairServes = false; }, DV::Why::PairInvalid },
			};
			for (const Row& row : rows)
			{
				DV::Facts f = AllPresent();
				row.knock(f);
				const DV::Choice c = DV::Choose(f);
				Assert::IsTrue(c.step == DV::Step::OwnSearch && c.why == row.why, W(DV::WhyName(row.why)).c_str());
				Assert::IsTrue(std::string(DV::WhyName(c.why)).size() > 0, L"every reason has a text");
			}
		}

		// Coordinator Q1: the reuse gate is SpanStatus != Invalid; Unknown reuses, visibly.
		TEST_METHOD(InvalidSpanNeverReusesUnknownReusesVisibly)
		{
			DV::Facts f = AllPresent();
			f.span = Span::Invalid;
			Assert::IsTrue(DV::Choose(f).why == DV::Why::PairInvalid, L"shared pair: Invalid refuses even with pairServes true");
			f.engineField = false;
			Assert::IsTrue(DV::Choose(f).why == DV::Why::PairInvalid, L"matcher field: Invalid refuses");
			f = AllPresent();
			f.span = Span::Unknown;
			DV::Choice c = DV::Choose(f);
			Assert::IsTrue(c.step == DV::Step::SharedPair && c.spanUnknown, L"Unknown reuses, flagged");
			DV::Tracker t;
			c = t.Note(c, true, 1000);
			Assert::AreEqual(1ull, t.SpanUnknownUses(), L"counter");
			const std::string segment = t.StatusSegment();
			Assert::IsTrue(Contains(segment, "validity unknown") && Contains(segment, "unvalidated 1"), W(segment).c_str());
			Assert::IsTrue(Engaged(t.Decisions(1000), "dither vectors unvalidated"), L"fallback entry while recent");
			Assert::IsFalse(Engaged(t.Decisions(1000 + DV::Tracker::kHoldMs), "dither vectors unvalidated"), L"ages out");
		}

		TEST_METHOD(EveryStepHasItsStatusCounterAndEntry)
		{
			DV::Tracker t;
			Assert::IsTrue(t.StatusSegment().empty(), L"nothing before the first decision");
			unsigned long long now = 10;
			auto note = [&](DV::Facts f, bool mapped) { return t.Note(DV::Choose(f), mapped, ++now); };

			DV::Choice c = note(AllPresent(), true);
			Assert::IsTrue(Contains(t.StatusSegment(), "dither vectors: shared pair"), W(t.StatusSegment()).c_str());
			Assert::AreEqual(1ull, t.Count(DV::Step::SharedPair));
			Assert::IsTrue(t.TakeChanged() && !t.TakeChanged(), L"one log line per change");
			for (const auto& d : t.Decisions(now))
				Assert::IsFalse(d.engaged, L"the shared pair engages nothing");

			DV::Facts f = AllPresent();
			f.engineField = false;
			c = note(f, true);
			Assert::IsTrue(Contains(t.StatusSegment(), "matcher field") && t.Count(DV::Step::MatcherField) == 1);
			Assert::IsTrue(Engaged(t.Decisions(now), "dither vectors matcher"));

			f = AllPresent();
			f.onDemandServed = true;
			c = note(f, true);
			Assert::IsTrue(Contains(t.StatusSegment(), "on-demand pair") && t.Count(DV::Step::OnDemand) == 1);
			Assert::IsTrue(Engaged(t.Decisions(now), "dither vectors on demand"));

			f = AllPresent();
			f.engineField = f.matcherLive = false;
			f.onDemandCause = Cause::OnDemandLate;
			c = note(f, true);
			Assert::IsTrue(Contains(t.StatusSegment(), "own search") && Contains(t.StatusSegment(), "on-demand pair was missed") &&
				t.Count(DV::Step::OwnSearch) == 1, W(t.StatusSegment()).c_str());
			const std::vector<FallbackCatalog::Decision> d = t.Decisions(now);
			Assert::IsTrue(Engaged(d, "dither vectors own search"));
			for (const auto& x : d)
				if (x.id == std::string("dither vectors own search"))
					Assert::IsTrue(Contains(x.reason, "on-demand"), W(x.reason).c_str());

			// A mapping failure turns a reuse into the own search, named.
			c = note(AllPresent(), false);
			Assert::IsTrue(c.step == DV::Step::OwnSearch && c.why == DV::Why::MapFailed && t.Count(DV::Step::OwnSearch) == 2);

			f = AllPresent();
			f.motionCompensation = false;
			c = note(f, true);
			Assert::IsTrue(Contains(t.StatusSegment(), "static") && t.Count(DV::Step::Static) == 1, W(t.StatusSegment()).c_str());

			// Choices are not automatic downgrades: own by setting engages nothing once the held entries aged out.
			f = AllPresent();
			f.ownSetting = true;
			note(f, true);
			for (const auto& x : t.Decisions(now + DV::Tracker::kHoldMs))
				Assert::IsFalse(x.engaged, W(x.id).c_str());
			Assert::IsTrue(Contains(t.StatusSegment(), "dither_vectors own"), L"but the status says it");
		}

		// No step is reachable without a status: every combination of the chain's boolean inputs (and the three spans) yields a
		// named step whose status segment names it.
		TEST_METHOD(NoStepWithoutAStatus)
		{
			const Span spans[] = { Span::Unknown, Span::Invalid, Span::Valid };
			for (unsigned bits = 0; bits < (1u << 12); ++bits)
				for (const Span span : spans)
				{
					DV::Facts f;
					f.slotRuns = (bits & 1u) != 0;
					f.patch = (bits & 2u) != 0;
					f.ownSetting = (bits & 4u) != 0;
					f.motionCompensation = (bits & 8u) != 0;
					f.served = (bits & 16u) != 0;
					f.sequences = (bits & 32u) != 0;
					f.timelineDrew = (bits & 64u) != 0;
					f.matcher = (bits & 128u) != 0;
					f.geometryValid = (bits & 256u) != 0;
					f.engineField = (bits & 512u) != 0;
					f.pairServes = (bits & 1024u) != 0;
					f.matcherLive = (bits & 2048u) != 0;
					f.span = span;
					const DV::Choice c = DV::Choose(f);
					if (c.step == DV::Step::None)
					{
						Assert::IsFalse(f.slotRuns, L"None only without the sigma-delta");
						continue;
					}
					Assert::IsTrue(!c.Reuses() || span != Span::Invalid, L"never a reuse across an Invalid span");
					Assert::IsTrue(!c.Reuses() || c.spanUnknown == (span == Span::Unknown), L"Unknown always flagged");
					DV::Tracker t;
					t.Note(c, true, 5);
					Assert::IsTrue(Contains(t.StatusSegment(), DV::StepName(c.step)), W(t.StatusSegment()).c_str());
				}
		}

		// SpanStatus (c2a-f, consumed): the dither pair (seq, next) and longer spans up to kMaxLead answer Invalid across a flush,
		// a discontinuity and a settings stamp change, Unknown unbound; the chain then refuses (Invalid) or flags (Unknown).
		TEST_METHOD(SpanStatusOfTheDitherPairAndLongerSpans)
		{
			const uint64_t spans[] = { 1, 2, static_cast<uint64_t>(MotionAhead::kMaxLead) };
			{
				MotionValidity::State unbound;
				for (const uint64_t n : spans)
					Assert::IsTrue(unbound.SpanStatus(20, 20 + n) == Span::Unknown, L"unbound: Unknown");
			}
			struct Change { VpIndicator::Counter bits; VpResync::Cause cause; const wchar_t* what; };
			const Change changes[] = {
				{ VpIndicator::kFull, VpResync::Cause::RendererReset, L"flush" },
				{ VpIndicator::kContinuity, VpResync::Cause::MissedFrames, L"discontinuity" },
				{ VpIndicator::kSettingsMotion, VpResync::Cause::SettingsApply, L"settings stamp" },
			};
			for (const Change& change : changes)
				for (const uint64_t n : spans)
				{
					MotionValidity::State state;
					auto ind = Bound(state);
					const uint64_t at = 40;   // the change stamps frames >= 40
					if (change.bits == VpIndicator::kContinuity)
						ind->Advance(change.bits, change.cause, at, 1);
					else
						ind->Advance(change.bits, change.cause, at);
					Assert::IsTrue(state.SpanStatus(at - 1, at - 1 + n) == Span::Invalid, change.what);
					Assert::IsTrue(state.SpanStatus(at - n, at) == Span::Invalid, change.what);
					Assert::IsTrue(state.SpanStatus(at, at + n) == Span::Valid, change.what);
					Assert::IsTrue(state.SpanStatus(at - 1 - n, at - 1) == Span::Valid, change.what);
					DV::Facts f = AllPresent();
					f.span = state.SpanStatus(at - 1, at);
					Assert::IsTrue(DV::Choose(f).why == DV::Why::PairInvalid, change.what);
				}
		}

		TEST_METHOD(RegistryKeepsOneTrackerPerHostAndForgets)
		{
			int a = 0, b = 0;
			Assert::IsTrue(DV::Registry::Find(&a) == nullptr, L"the status reader never creates one");
			DV::Registry::ForHost(&a).Note(DV::Choose(AllPresent()), true, 1);
			DV::Registry::ForHost(&b);
			Assert::IsTrue(&DV::Registry::ForHost(&a) != &DV::Registry::ForHost(&b));
			Assert::AreEqual(1ull, DV::Registry::Find(&a)->Count(DV::Step::SharedPair));
			DV::Registry::Forget(&a);
			DV::Registry::Forget(&b);
			Assert::IsTrue(DV::Registry::Find(&a) == nullptr && DV::Registry::Find(&b) == nullptr);
			for (const auto& d : DV::Tracker::Released())
				Assert::IsFalse(d.engaged);
		}
	};
}
