/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// R3 / L1 / R4a: resync_event = on. The Indicator drives the readers (FULL / MINOR / timing classes),
// T3 drift basics (an unannounced 59.94/60 move is timing only; a slip is MINOR; an announced flip is a
// FULL at the rebuilt renderer's first frame), the pulldown re-latch, the timeline re-anchor, the
// sigma-delta carry rule, Dynamic Drive's restart, and the re-base decision product P0 (H-only,
// V-only, both, anamorphic, source-time fade).
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/modules/input/signal/ResyncCallSites.h>
#include <VP-V/modules/input/signal/ResyncDrive.h>
#include <VP-V/modules/input/signal/ResyncShadow.h>
#include <VP-V/modules/input/queue_timing/PulldownDetector.h>
#include <VP-V/modules/motion/consumers/InterpolationTimeline.h>
#include <VP-V/modules/scaling/rebase/RebaseDecision.h>
#include <VP-V/modules/drive/led_compensation/LedDriveConfig.h>
#include <VP-V/modules/drive/led_compensation/LedDriveModule.h>

#include <memory>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace VpResync;
using VpIndicator::Bit;

namespace Tests
{
	namespace ResyncR3
	{
		constexpr VpIndicator::CounterMask F = Bit(VpIndicator::kFull);
		constexpr VpIndicator::CounterMask M = Bit(VpIndicator::kContinuity);
		constexpr VpIndicator::CounterMask T = Bit(VpIndicator::kTiming);

		// A renderer in miniature: the capture side enqueues through the shadow, the render side seals
		// each frame through the Drive (in order, one frame behind the enqueue).
		struct Rig
		{
			Shadow shadow;
			Drive drive;
			uint64_t seq = 0;
			uint64_t gen = 1;
			uint64_t state = 0x1000;
			uint64_t format = 7;
			uint64_t counter = 100;
			double rate = 60.0;

			explicit Rig(Shadow::Mode mode = Shadow::Mode::On, bool clearNote = true)
			{
				if (clearNote)
					RefusalNote::Take();   // process-global
				shadow.Configure(mode, 16);
			}
			EnqueueFacts Next()
			{
				EnqueueFacts f;
				f.sequence = ++seq;
				f.queueGeneration = gen;
				f.stateKey = state;
				f.formatKey = format;
				f.frameCounter = ++counter;
				f.measuredRateHz = rate;
				return f;
			}
			uint64_t Push() { shadow.OnEnqueue(Next(), double(seq)); return seq; }
			uint64_t PushGap(const char* text, uint32_t frames)
			{
				EnqueueFacts f = Next();
				f.gapBefore = true;
				f.gapCause = text;
				f.gapFrames = frames;
				shadow.OnEnqueue(f, double(seq));
				return seq;
			}
			VpIndicator::CounterMask Seal(uint64_t s) { return drive.SealAndObserve(shadow.GetIndicator(), s); }
			std::shared_ptr<VpIndicator::Indicator> Ind() const { return shadow.GetIndicator(); }
		};

		LedDrive::LedDriveConfig Compensate()
		{
			LedDrive::LedDriveConfig config;
			config.enabled = true;
			config.mode = LedDrive::Mode::Compensate;
			config.cntrAssist = false;
			return config;
		}

		// 24p in 60 (3:2): N R R N R repeated.
		void FeedThreeTwo(Pulldown::Detector& d, int frames, double hz)
		{
			static const bool repeats[5] = { false, true, true, false, true };
			for (int i = 0; i < frames; ++i)
				d.Next(repeats[i % 5], false, hz);
		}
	}

	using namespace ResyncR3;

	TEST_CLASS(ResyncDriveTests)
	{
	public:
		TEST_METHOD(ModeFromSettingAndOriginal)
		{
			Assert::IsTrue(ModeFromSetting("on", false) == Shadow::Mode::On);
			Assert::IsTrue(ModeFromSetting("shadow", false) == Shadow::Mode::Shadow);
			Assert::IsTrue(ModeFromSetting("off", false) == Shadow::Mode::Off);
			Assert::IsTrue(ModeFromSetting("on", true) == Shadow::Mode::Off, L"the Original pathway never drives");
			Rig r(Shadow::Mode::On);
			Assert::IsTrue(r.shadow.Active() && r.shadow.Driving());
			r.shadow.Configure(Shadow::Mode::Shadow, 16);
			Assert::IsTrue(r.shadow.Active() && !r.shadow.Driving());
		}

		TEST_METHOD(ShadowOnSwitchKeepsTheIndicator)
		{
			Rig r(Shadow::Mode::Shadow);
			const auto first = r.Ind();
			r.shadow.Configure(Shadow::Mode::On, 16);
			Assert::IsTrue(first == r.Ind(), L"shadow -> on keeps the era");
			r.shadow.Configure(Shadow::Mode::Off, 16);
			r.shadow.Configure(Shadow::Mode::On, 16);
			Assert::IsTrue(first != r.Ind(), L"off -> on starts a new era");
		}

		TEST_METHOD(FullReachesEveryFullReaderOnceThenNothing)
		{
			Rig r;
			const uint64_t s1 = r.Push();
			Assert::AreEqual(VpIndicator::kAllCounters, r.Seal(s1), L"the first observe reports everything");
			for (size_t i = 0; i < Drive::kReaders; ++i)
			{
				const Reader reader = static_cast<Reader>(i);
				Assert::IsTrue((r.drive.Take(reader) & F) != 0, L"every reader reads FULL");
				Assert::AreEqual(VpIndicator::CounterMask(0), r.drive.Take(reader), L"Take clears");
			}
			const uint64_t s2 = r.Push();
			Assert::AreEqual(VpIndicator::CounterMask(0), r.Seal(s2), L"an ordinary frame changes nothing");
			// A seek (live queue reset): FULL at the first frame of the new generation.
			r.shadow.ArmQueueGeneration(2, "live queue reset");
			r.gen = 2;
			const uint64_t s3 = r.Push();
			Assert::IsTrue((r.Seal(s3) & F) != 0);
			Assert::IsTrue((r.drive.Take(Reader::RenderAhead) & F) != 0);
			Assert::IsTrue((r.drive.Take(Reader::Libplacebo) & F) != 0, L"L1 reads the FULL");
			Assert::IsTrue((r.drive.Take(Reader::SceneDetector) & F) != 0);
			Assert::IsTrue((r.drive.Take(Reader::LedDrive) & F) != 0);
		}

		TEST_METHOD(MinorClassReachesOnlyItsReaders)
		{
			Rig r;
			r.Seal(r.Push());
			for (size_t i = 0; i < Drive::kReaders; ++i) r.drive.Take(static_cast<Reader>(i));
			const uint64_t s = r.PushGap("missed frames", 2);
			const VpIndicator::CounterMask changed = r.Seal(s);
			Assert::IsTrue((changed & M) != 0 && (changed & F) == 0, L"a missed frame is MINOR");
			Assert::AreEqual(M, r.drive.Take(Reader::RenderAhead), L"render-ahead re-anchors");
			Assert::AreEqual(M, r.drive.Take(Reader::Timeline));
			Assert::AreEqual(M, r.drive.Take(Reader::SigmaDelta));
			Assert::AreEqual(M, r.drive.Take(Reader::SourceMonitor), L"SRM holds (it sees the slip, does nothing)");
			Assert::AreEqual(M, r.drive.Take(Reader::LedDrive));
			Assert::AreEqual(VpIndicator::CounterMask(0), r.drive.Take(Reader::SceneDetector), L"no scene reset on a slip");
			Assert::AreEqual(VpIndicator::CounterMask(0), r.drive.Take(Reader::Libplacebo), L"no flush on a slip");
		}

		// T3: an unannounced 59.94/60 move is timing only - it never moves continuity or full.
		TEST_METHOD(UnannouncedRateMoveIsTimingOnly)
		{
			Rig r;
			r.rate = 60.0;
			r.Seal(r.Push());
			r.Seal(r.Push());
			for (size_t i = 0; i < Drive::kReaders; ++i) r.drive.Take(static_cast<Reader>(i));
			const auto ind = r.Ind();
			const uint32_t continuity = ind->Total(VpIndicator::kContinuity), full = ind->Total(VpIndicator::kFull);
			r.rate = 60000.0 / 1001.0;
			const uint64_t s = r.Push();
			Assert::AreEqual(T, r.Seal(s));
			Assert::AreEqual(continuity, ind->Total(VpIndicator::kContinuity));
			Assert::AreEqual(full, ind->Total(VpIndicator::kFull));
			Assert::AreEqual(T, r.drive.Take(Reader::RenderAhead));
			Assert::AreEqual(T, r.drive.Take(Reader::Timeline));
			Assert::AreEqual(T, r.drive.Take(Reader::LedDrive), L"LED re-latches its period smoothing");
			Assert::AreEqual(VpIndicator::CounterMask(0), r.drive.Take(Reader::SigmaDelta), L"vectors survive a timing event");
			Assert::AreEqual(VpIndicator::CounterMask(0), r.drive.Take(Reader::SourceMonitor));
			Assert::IsTrue(CarryValid(r.shadow, s - 1, s), L"a carry across a timing event is valid");
		}

		// T3: a drift-correction drop is a late MINOR at the slip; sealed frames keep their stamp.
		TEST_METHOD(DriftSlipIsLateMinor)
		{
			Rig r;
			const uint64_t a = r.Push(), b = r.Push(), c = r.Push(), d = r.Push();
			r.Seal(a);
			r.Seal(b);   // b sealed and dropped by drift correction: the slip is at c
			r.shadow.OnLateSlip(Cause::DriftDrop, b + 1, 1, 10.0);
			const auto ind = r.Ind();
			Assert::IsTrue(Drive::PairValid(*ind, a, b, M), L"before the slip");
			Assert::IsFalse(Drive::PairValid(*ind, b, c, M), L"a pair spanning the slip is invalid");
			Assert::IsTrue(Drive::PairValid(*ind, c, d, M), L"after the slip");
			Assert::IsFalse(CarryValid(r.shadow, b, c));
			Assert::IsTrue((r.Seal(c) & M) != 0);
			// A slip reported for an already sealed frame is clamped above the seal (never retroactive).
			r.shadow.OnLateSlip(Cause::KeptRunHole, a, 0, 11.0);
			Assert::IsTrue(Drive::PairValid(*ind, b, c, Bit(VpIndicator::kFull)));
			Assert::IsFalse(Drive::PairValid(*ind, c, d, M), L"clamped to the first unsealed frame");
		}

		// T3: an announced 59.94/60 flip is refused in place; the GUI rebuilds the renderer, whose first
		// frame is FULL (StartAfterSourceChange), observed once (no double action).
		TEST_METHOD(AnnouncedFlipIsFullAtTheRebuiltRenderer)
		{
			{
				Rig old;
				old.Seal(old.Push());
				old.shadow.NoteRefusal(RefusalReason::DisplayMode);
			}
			Rig rebuilt(Shadow::Mode::On, false);   // a new renderer: a new Shadow, a new era (the note kept)
			const uint64_t s1 = rebuilt.Push();
			std::string events;
			Assert::IsTrue(rebuilt.shadow.EventLines(events));
			Assert::IsTrue(events.find(std::string("full ") + Token(Cause::StartAfterSourceChange)) != std::string::npos,
				L"the FULL names the source change");
			Assert::AreEqual(uint32_t(1), rebuilt.Ind()->Total(VpIndicator::kFull));
			Assert::IsTrue((rebuilt.Seal(s1) & F) != 0);
			Assert::IsTrue((rebuilt.drive.Take(Reader::LedDrive) & F) != 0);
			const uint64_t s2 = rebuilt.Push();
			Assert::AreEqual(VpIndicator::CounterMask(0), rebuilt.Seal(s2), L"no second action");
			Assert::AreEqual(uint32_t(1), rebuilt.Ind()->Total(VpIndicator::kFull));
		}

		TEST_METHOD(ReaderGenerationFollowsFullOnlyWhenOn)
		{
			Rig r;
			const uint64_t a = r.Push();
			const uint64_t b = r.PushGap("missed frames", 1);
			Assert::AreEqual(ReaderGeneration(r.shadow, 5, a), ReaderGeneration(r.shadow, 5, b), L"a slip keeps the key");
			r.shadow.ArmInPlaceSourceChange(0x2000);
			r.state = 0x2000;
			const uint64_t c = r.Push();
			Assert::AreNotEqual(ReaderGeneration(r.shadow, 5, b), ReaderGeneration(r.shadow, 5, c),
				L"an in-place source change is a new key (the declared render-ahead change)");
			r.shadow.Configure(Shadow::Mode::Shadow, 16);
			Assert::AreEqual(uint64_t(5), ReaderGeneration(r.shadow, 5, c), L"shadow: the queue generation, unchanged");
			Assert::IsTrue(CarryValid(r.shadow, a, c), L"shadow: the carry rule is today's");
		}

		TEST_METHOD(CarryInvalidAcrossARebaseAxis)
		{
			Rig r;
			const uint64_t a = r.Push();
			r.Seal(a);
			r.drive.Take(Reader::RebaseDecision);   // the first observe reported everything
			const uint64_t b = r.Push();
			Assert::IsTrue(CarryValid(r.shadow, a, b));
			r.shadow.OnRebaseDecision(true, false, b);   // H only, from b
			Assert::IsFalse(CarryValid(r.shadow, a, b), L"re-base H changed between A and B");
			const uint64_t c = r.Push();
			Assert::IsTrue(CarryValid(r.shadow, b, c));
			r.Seal(b);
			const VpIndicator::CounterMask p0 = r.drive.Take(Reader::RebaseDecision);
			Assert::IsTrue((p0 & Bit(VpIndicator::kRebaseH)) != 0);
			Assert::IsTrue((p0 & Bit(VpIndicator::kRebaseV)) == 0, L"V untouched");
		}

		TEST_METHOD(CursorLatchesThenReportsChanges)
		{
			Rig r;
			Drive::Cursor cursor;
			const uint64_t a = r.Push();
			Assert::AreEqual(VpIndicator::kAllCounters, cursor.Advance(*r.Ind(), a));
			const uint64_t b = r.Push();
			Assert::AreEqual(VpIndicator::CounterMask(0), cursor.Advance(*r.Ind(), b));
			r.rate = 50.0;
			const uint64_t c = r.Push();
			Assert::AreEqual(T, cursor.Advance(*r.Ind(), c));
		}

		TEST_METHOD(PulldownRelatchKeepsTheLockForA5994Move)
		{
			Pulldown::Detector d;
			d.SetTimingRelatch(true);
			FeedThreeTwo(d, 400, 60.0);
			Assert::IsTrue(d.Locked(), L"3:2 locked");
			Assert::IsFalse(d.RelatchSignalHz(60000.0 / 1001.0), L"0.1% is within the lock's resolution");
			Assert::IsTrue(d.Locked());
			Assert::AreEqual(60000.0 / 1001.0, d.SignalHz(), 1e-9);
			FeedThreeTwo(d, 50, 60000.0 / 1001.0);
			Assert::IsTrue(d.Locked(), L"still locked at the new rate");
			Assert::IsTrue(d.RelatchSignalHz(50.0), L"a class change drops the lock");
			Assert::IsFalse(d.Locked());
			Assert::IsTrue(d.PendingDiscontinuity(), L"the next kept frame carries the break");
		}

		TEST_METHOD(PulldownFixedRuleOffOnlyWhileRelatching)
		{
			Pulldown::Detector rule;
			FeedThreeTwo(rule, 400, 60.0);
			Assert::IsTrue(rule.Locked());
			rule.Next(false, false, 59.0);   // 1.7%: the fixed 0.5% rule drops the lock (off/shadow)
			Assert::IsFalse(rule.Locked());
			Pulldown::Detector relatch;
			relatch.SetTimingRelatch(true);
			FeedThreeTwo(relatch, 400, 60.0);
			relatch.Next(false, false, 59.0);
			Assert::IsTrue(relatch.Locked(), L"on: no fixed rule; the timing event decides");
		}

		TEST_METHOD(TimelineReanchorKeepsTheCadence)
		{
			InterpolationTimeline::Timeline t;
			const double hz = 24.0, display = 60.0;
			uint64_t s = 0;
			for (int i = 0; i < 10; ++i)
			{
				++s;
				t.Plan(1, s, s / hz, 0.0, hz, display, false);
			}
			const auto restarts = t.Restarts(), reanchors = t.Reanchors();
			t.Reanchor();
			Assert::IsTrue(t.ReanchorPending());
			++s;
			const double jittered = s / hz + 0.01 / hz;   // small jitter: would be smoothed
			t.Plan(1, s, jittered, 0.0, hz, display, false);
			Assert::AreEqual(reanchors + 1, t.Reanchors(), L"re-anchored at the frame's own time");
			Assert::AreEqual(restarts, t.Restarts(), L"no restart: the display cadence is kept");
			Assert::IsFalse(t.ReanchorPending());
			Assert::AreEqual(jittered, t.Clock(), 1e-12);
			t.Reanchor();
			++s;
			t.Plan(2, s, s / hz, 0.0, hz, display, false);   // a new generation: restart wins
			Assert::AreEqual(restarts + 1, t.Restarts());
			Assert::IsFalse(t.ReanchorPending());
		}

		TEST_METHOD(LedRestartHoldsGainAndDropsTheOldSource)
		{
			LedDrive::LedDriveModule module(LedDrive::ProjectorConfig(), Compensate());
			LedDrive::FrameStats prediction{};
			prediction.apl = 50.0;
			module.SubmitPrediction(105, prediction);
			Assert::IsTrue(module.PredictionFor(105, prediction));
			module.BeginFrameAt(100, 1000.0, 24.0);
			module.BeginFrameAt(101, 1041.7, 24.0);
			const uint64_t epoch = module.LookaheadEpoch();
			module.OnSourceRestart(102, 60.0);
			Assert::IsTrue(module.LookaheadEpoch() > epoch, L"look-ahead restarted");
			Assert::IsFalse(module.PredictionFor(105, prediction), L"old predictions dropped");
			Assert::IsFalse(module.HaveStatsForTest(), L"compensate waits for the new source's measurement");
			Assert::AreEqual(1000.0 / 60.0, module.FramePeriodMsForTest(), 1e-9, L"period smoothing restarted at the rate");
			Assert::AreEqual(50, module.PairedContrastForTest(), L"CNTR at its baseline");
			module.ResetPeriodSmoothing(50.0);
			Assert::AreEqual(20.0, module.FramePeriodMsForTest(), 1e-9);
		}

		// R4a: P0, the re-base decision product.
		TEST_METHOD(P0HorizontalOnlyFadesOnlyH)
		{
			VpRebase::DecisionTracker t;
			const double period = 1.0e6 / 24.0, fade = 10 * period;   // a 10-frame fade
			t.Update(1, period, 1.0, 1.0, -1, 0, 0);
			const VpRebase::Decision d = t.Update(2, period, 0.75, 1.0, -1, fade, fade);
			Assert::IsTrue(d.axis[VpRebase::kH].changed && !d.axis[VpRebase::kV].changed);
			Assert::IsTrue(d.StampH() && !d.StampV());
			Assert::AreEqual(0.0, d.axis[VpRebase::kH].weight, 1e-12, L"the fade starts on the change");
			Assert::AreEqual(1.0, d.axis[VpRebase::kV].weight);
			const VpRebase::Decision mid = t.Update(7, period, 0.75, 1.0, -1, fade, fade);
			Assert::AreEqual(0.5, mid.axis[VpRebase::kH].weight, 1e-12, L"5 of 10 source frames");
			const auto blend = mid.TwoPicture();
			Assert::AreEqual(1.0, blend.fromX, 1e-12);
			Assert::AreEqual(1.0, blend.fromY, 1e-12);
			Assert::AreEqual(0.5, blend.weight, 1e-12);
			Assert::IsFalse(t.Update(12, period, 0.75, 1.0, -1, fade, fade).TwoPicture().Active(), L"done after 10 frames");
		}

		TEST_METHOD(P0VerticalOnlyFadesOnlyV)
		{
			VpRebase::DecisionTracker t;
			const double period = 1.0e6 / 60.0, fade = 4 * period;
			t.Update(1, period, 1.0, 1.0, -1, 0, 0);
			const VpRebase::Decision d = t.Update(2, period, 1.0, 0.5, -1, fade, fade);
			Assert::IsTrue(!d.StampH() && d.StampV());
			const auto b = t.Update(4, period, 1.0, 0.5, -1, fade, fade).TwoPicture();
			Assert::AreEqual(0.5, b.weight, 1e-12);
			Assert::AreEqual(1.0, b.fromY, 1e-12);
			Assert::AreEqual(1.0, b.fromX, 1e-12, L"H not fading: from = its factor");
		}

		TEST_METHOD(P0BothAxesTogether)
		{
			VpRebase::DecisionTracker t;
			const double period = 1.0e6 / 24.0, fade = 8 * period;
			t.Update(1, period, 1.0, 1.0, -1, 0, 0);
			const VpRebase::Decision d = t.Update(2, period, 0.5, 0.5, -1, fade, fade);
			Assert::IsTrue(d.StampH() && d.StampV());
			const auto b = t.Update(6, period, 0.5, 0.5, -1, fade, fade).TwoPicture();
			Assert::AreEqual(0.5, b.weight, 1e-12);
			Assert::AreEqual(1.0, b.fromX, 1e-12);
			Assert::AreEqual(1.0, b.fromY, 1e-12);
		}

		TEST_METHOD(P0AnamorphicIndependentFades)
		{
			VpRebase::DecisionTracker t;
			const double period = 1.0e6 / 24.0, fade = 10 * period;
			t.Update(1, period, 1.0, 1.0, -1, 0, 0);
			t.Update(2, period, 0.75, 1.0, -1, fade, fade);        // H starts at 2
			const VpRebase::Decision d = t.Update(6, period, 0.75, 0.5, -1, fade, fade);   // V starts at 6
			Assert::IsFalse(d.axis[VpRebase::kH].changed, L"H is not re-latched by V's change");
			Assert::AreEqual(0.4, d.axis[VpRebase::kH].weight, 1e-12);
			Assert::AreEqual(0.0, d.axis[VpRebase::kV].weight, 1e-12);
			const VpRebase::Decision later = t.Update(12, period, 0.75, 0.5, -1, fade, fade);
			Assert::AreEqual(1.0, later.axis[VpRebase::kH].weight, L"H done at 12");
			Assert::AreEqual(0.6, later.axis[VpRebase::kV].weight, 1e-12, L"V runs on its own clock");
			const auto b = later.TwoPicture();
			Assert::AreEqual(0.75, b.fromX, 1e-12, L"H done: from = its factor");
			Assert::AreEqual(1.0, b.fromY, 1e-12);
		}

		TEST_METHOD(P0FullResetRepeatsAndDeterminism)
		{
			const double period = 1.0e6 / 24.0, fade = 10 * period;
			std::vector<double> first, second;
			for (std::vector<double>* out : { &first, &second })
			{
				VpRebase::DecisionTracker t;
				t.Update(1, period, 1.0, 1.0, -1, 0, 0);
				for (uint64_t s = 2; s < 20; ++s)
				{
					const double f = s >= 3 ? 0.75 : 1.0;
					out->push_back(t.Update(s, period, f, f, -1, fade, fade).TwoPicture().weight);
					// A cadence repeat of the same frame: same weights, no change.
					const VpRebase::Decision rep = t.Update(s, period, f, f, -1, fade, fade);
					Assert::IsFalse(rep.StampH() || rep.StampV());
				}
			}
			Assert::IsTrue(first == second, L"the same frames give the same weights");
			VpRebase::DecisionTracker t;
			t.Update(1, period, 1.0, 1.0, -1, 0, 0);
			t.Update(2, period, 0.5, 0.5, -1, fade, fade);
			t.Reset();   // FULL
			const VpRebase::Decision d = t.Update(3, period, 1.0, 1.0, 2, fade, fade);
			Assert::IsFalse(d.TwoPicture().Active(), L"native, no fade after a FULL");
			const VpRebase::Decision& k = d;
			const VpRebase::Decision k2 = t.Update(5, period, 1.0, 1.0, 3, 0, 0);
			Assert::IsFalse(k.kernelChanged, L"the first kernel after a reset is a latch");
			Assert::IsTrue(k2.StampH() && k2.StampV(), L"a kernel change stamps both axes");
		}

		TEST_METHOD(P0ReversalMidFadeLeavesNothingToFade)
		{
			VpRebase::DecisionTracker t;
			const double period = 1.0e6 / 24.0, fade = 10 * period;
			t.Update(1, period, 1.0, 1.0, -1, 0, 0);
			t.Update(2, period, 0.5, 1.0, -1, fade, fade);
			const VpRebase::Decision back = t.Update(4, period, 1.0, 1.0, -1, fade, fade);
			Assert::IsTrue(back.axis[VpRebase::kH].changed);
			Assert::IsFalse(back.TwoPicture().Active(), L"back to the path being left: a hard switch");
		}

		TEST_METHOD(P0StatusLine)
		{
			VpRebase::StatusCell cell;
			std::string text;
			Assert::IsFalse(cell.Text(text));
			VpRebase::DecisionTracker t;
			const double period = 1.0e6 / 24.0;
			t.Update(1, period, 1.0, 1.0, -1, 0, 0);
			cell.Publish(t.Update(2, period, 0.75, 1.0, -1, 10 * period, 0));
			Assert::IsTrue(cell.Text(text));
			Assert::AreEqual(std::string("re-base H x0.750 (fading 0%), V native (source-time fade)"), text);
		}
	};
}
