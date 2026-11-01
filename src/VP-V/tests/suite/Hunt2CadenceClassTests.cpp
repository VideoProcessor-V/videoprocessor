// Hunt 2 2026-10-17, timing and motion group, sub-agent B (cadence_class_fixes): the
// exclusive 15 fps pulldown floor (06-interp H1), the film clock's re-anchor and robust
// anchor (M1, L6), the lateness while a re-phase is tracked (M1), the retry rule (L3),
// stale D3/D4 state on a settings change (L2), the locked queue target (M3 / D2), the
// source-only film / video class (M2 = 15-intent C4) and film near 1:1 (L5), and the
// content classifier's carrier / animation / static UI rules (02-dv M2, M3) and the
// content-gated profile warning (02-dv L6). CPU only; each with the switch off as well.
#include "pch.h"
#include "CppUnitTest.h"

#include <ConfigFile.h>
#include <ContentClassifier.h>
#include <ContentIntentObserver.h>
#include <MainConfigSchema.h>
#include <RendererProfileConfig.h>
#include <VP-V/modules/motion/consumers/InterpolationPolicy.h>
#include <VP-V/modules/input/queue_timing/PulldownDetector.h>

#include <Windows.h>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Hunt2CadenceClass
{
	// One signal slot of `film` (num/den fps) carried in `signal` Hz: 'N' when a new film
	// frame starts at slot i.
	inline std::string Carried(long long filmNum, long long filmDen, double signalHz, long long from, long long count)
	{
		std::string out;
		auto index = [&](long long i) { return static_cast<long long>(std::floor((static_cast<double>(i) * filmNum) / (filmDen * signalHz) + 1e-9)); };
		for (long long i = from; i < from + count; ++i)
			out += index(i) != index(i - 1) ? 'N' : 'R';
		return out;
	}

	inline Pulldown::Settings Fixes(bool on)
	{
		Pulldown::Settings s;
		s.exclusiveFilmFloor = s.trackingLatenessMax = s.retryNeedsFailure = on;
		return s;
	}

	inline int LockedFrames(const std::string& pattern, double signalHz, const Pulldown::Settings& settings)
	{
		Pulldown::Detector d;
		d.SetSettings(settings);
		int locked = 0;
		for (char c : pattern)
			locked += d.Next(c == 'R', false, signalHz).locked ? 1 : 0;
		return locked;
	}

	TEST_CLASS(Hunt2CadenceClassPulldownTests)
	{
	public:
		// H1: 30p on twos (one new of four at 60 Hz = 15.000 fps, 14.985 at 59.94, either side
		// of 15 on a wobbling measured 60) never locks with the fixes; real film rates still do.
		TEST_METHOD(FilmFloorIsExclusiveAtEverySignalRate)
		{
			for (const double signal : { 60.0, 60000.0 / 1001.0, 60.0003, 59.9998, 120.0, 30.0 })
			{
				// The 15 fps floor itself: one new frame per signal / 15 slots.
				const int hold = static_cast<int>(std::lround(signal / 15.0));
				std::string pattern;
				for (int i = 0; i < static_cast<int>(signal * 10.0); ++i)
					pattern += i % hold == 0 ? 'N' : 'R';
				const std::wstring at = L" at " + std::to_wstring(signal);
				Assert::AreEqual(0, LockedFrames(pattern, signal, Fixes(true)), (L"15 fps never locks" + at).c_str());
				const double filmHz = signal / hold;
				if (filmHz >= Pulldown::kMinFilmHz)
					Assert::IsTrue(LockedFrames(pattern, signal, Fixes(false)) > 0, (L"old: the inclusive floor locked" + at).c_str());
			}
			// Real pulldown is untouched: 24p in 60 and 59.94, 16 fps silent film in 48 Hz, 25p in 50.
			Assert::IsTrue(LockedFrames(Carried(24, 1, 60.0, 0, 600), 60.0, Fixes(true)) > 0, L"24 in 60");
			Assert::IsTrue(LockedFrames(Carried(24000, 1001, 60000.0 / 1001.0, 0, 600), 60000.0 / 1001.0, Fixes(true)) > 0, L"23.976 in 59.94");
			Assert::IsTrue(LockedFrames(Carried(16, 1, 48.0, 0, 480), 48.0, Fixes(true)) > 0, L"16 in 48");
			Assert::IsTrue(LockedFrames(Carried(25, 1, 50.0, 0, 500), 50.0, Fixes(true)) > 0, L"25 in 50");
			// The floor as a function: relative margin, exclusive.
			Assert::IsFalse(Pulldown::AboveFilmFloor(15.0, true));
			Assert::IsFalse(Pulldown::AboveFilmFloor(15.0 * 1.001, true));
			Assert::IsFalse(Pulldown::AboveFilmFloor(14.985, true));
			Assert::IsTrue(Pulldown::AboveFilmFloor(16.0, true));
			Assert::IsTrue(Pulldown::AboveFilmFloor(15.0, false));
			Assert::IsFalse(Pulldown::AboveFilmFloor(14.985, false));
		}

		// M1 (detector part): while a re-phase is tracked and the locked phase has failed, the
		// kept frames report the most lateness (they cannot raise the clock's anchor).
		TEST_METHOD(LatenessIsTheMostWhileThePhaseIsUncertain)
		{
			for (int on = 0; on < 2; ++on)
			{
				Pulldown::Detector d;
				d.SetSettings(Fixes(on == 1));
				// 24 in 60 for 3 s, then one repeat removed (an edit): the phase moves.
				std::string pattern = Carried(24, 1, 60.0, 0, 180) + Carried(24, 1, 60.0, 181, 120);
				int uncertain = 0, unequal = 0, rephased = 0;
				for (char c : pattern)
				{
					const auto decision = d.Next(c == 'R', false, 60.0);
					rephased += decision.rephased ? 1 : 0;
					if (decision.phaseUncertain)
					{
						++uncertain;
						unequal += decision.cadenceLateness != decision.cadenceLatenessMax ? 1 : 0;
					}
				}
				Assert::IsTrue(rephased >= 1, L"the shift re-phases in place");
				if (on)
				{
					Assert::IsTrue(uncertain > 0, L"tracking frames flagged");
					Assert::AreEqual(0, unequal, L"lateness = the most while uncertain");
				}
				else
					Assert::AreEqual(0, uncertain, L"off: the behaviour before");
			}
		}

		// L3: a counted miss (probation), then a lone subtitle in the verification cycle, then the
		// same cadence: the lock is kept with the fixes (the old rule dropped it).
		TEST_METHOD(ALoneSubtitleAfterAMissKeepsTheLock)
		{
			for (int on = 0; on < 2; ++on)
			{
				Pulldown::Detector d;
				d.SetSettings(Fixes(on == 1));
				const std::string film = Carried(24, 1, 60.0, 0, 400);
				bool lostAfterMiss = false;
				bool lockedAtMiss = false;
				bool subtitleShown = false;
				for (int i = 0; i < static_cast<int>(film.size()); ++i)
				{
					if (i == 200)
						continue;   // one frame missed (counted below)
					bool repeat = film[static_cast<size_t>(i)] == 'R';
					const bool gap = i == 201;
					// The first repeat slot after the miss shows a subtitle (a new picture).
					if (i > 201 && repeat && !subtitleShown)
					{
						repeat = false;
						subtitleShown = true;
					}
					const auto decision = d.Next(repeat, gap, 60.0, gap ? 1 : -1);
					if (i == 199) lockedAtMiss = decision.locked;
					if (i > 201 && !decision.locked) lostAfterMiss = true;
				}
				Assert::IsTrue(lockedAtMiss, L"locked before the miss");
				if (on)
					Assert::IsFalse(lostAfterMiss, L"a lone subtitle in the verification keeps the lock");
				else
					Assert::IsTrue(lostAfterMiss, L"off: the retry unlocked (the behaviour before)");
			}
		}

		// L2: a settings change mid-tracking ends the tracking (no stale candidates resume):
		// no re-phase within one cycle of the change.
		TEST_METHOD(ASettingsChangeEndsStaleTracking)
		{
			Pulldown::Detector d;
			const std::string before = Carried(24, 1, 60.0, 0, 180);
			for (char c : before)
				d.Next(c == 'R', false, 60.0);
			Assert::IsTrue(d.Locked());
			const std::string after = Carried(24, 1, 60.0, 181, 200);
			for (int i = 0; i < 3; ++i)
				d.Next(after[static_cast<size_t>(i)] == 'R', false, 60.0);
			Pulldown::Settings off;
			off.rephaseInPlace = false;
			d.SetSettings(off);
			d.SetSettings(Pulldown::Settings{});
			for (int i = 3; i < 3 + d.Cycle(); ++i)
				Assert::IsFalse(d.Next(after[static_cast<size_t>(i)] == 'R', false, 60.0).rephased,
					L"no adoption from tracking state that predates the settings change");
		}

		TEST_METHOD(LockedQueueTargetFollowsTheFilmRatio)
		{
			using Pulldown::LockedQueueTarget;
			Assert::AreEqual(0.4, Pulldown::FilmToSignalRatio(5, 2), 1e-12);
			Assert::AreEqual(1.0, Pulldown::FilmToSignalRatio(0, 0));
			Assert::AreEqual(1.0, Pulldown::FilmToSignalRatio(4, 4));
			Assert::AreEqual(size_t{ 5 }, LockedQueueTarget(12, 0.4, 2), L"200 ms at 24 in 60: 5 film frames, not 12");
			Assert::AreEqual(size_t{ 2 }, LockedQueueTarget(4, 0.4, 2), L"never below the pair");
			Assert::AreEqual(size_t{ 3 }, LockedQueueTarget(6, 0.5, 2), L"30 in 60");
			Assert::AreEqual(size_t{ 1 }, LockedQueueTarget(1, 0.4, 2), L"never above the target");
			Assert::AreEqual(size_t{ 4 }, LockedQueueTarget(4, 1.0, 2), L"not locked: the target");
			Assert::AreEqual(size_t{ 0 }, LockedQueueTarget(0, 0.4, 2));
			// Gate r3 (2026-10-18): the renderer's pair floor is A, B converted and the arrival
			// (kLockedQueuePairFrames = 3). The harness's 24-in-29.97 run: target 3 stays 3 (2 left
			// B unconverted at every dequeue and the de-judder never estimated); 3:2 with a target
			// of 4 keeps 3; a target below the floor is never raised.
			Assert::AreEqual(size_t{ 3 }, Pulldown::kLockedQueuePairFrames);
			Assert::AreEqual(size_t{ 3 }, LockedQueueTarget(3, 0.8, Pulldown::kLockedQueuePairFrames), L"24 in 30: 3 stays 3");
			Assert::AreEqual(size_t{ 3 }, LockedQueueTarget(4, 0.4, Pulldown::kLockedQueuePairFrames), L"3:2, target 4: 3");
			Assert::AreEqual(size_t{ 2 }, LockedQueueTarget(2, 0.4, Pulldown::kLockedQueuePairFrames), L"never above the target");
			Assert::AreEqual(size_t{ 5 }, LockedQueueTarget(12, 0.4, Pulldown::kLockedQueuePairFrames), L"deeper targets still scale");
			// The look-ahead minimum (motion-ahead depth 4 + the frame shown = 5) is kept: at the
			// default target of 5 nothing changes; a deeper target still shrinks to it.
			Assert::AreEqual(size_t{ 5 }, LockedQueueTarget(5, 0.4, 5), L"motion-ahead keeps its 5");
			Assert::AreEqual(size_t{ 5 }, LockedQueueTarget(12, 0.4, 5), L"12 at 3:2 -> 5 (the look-ahead floor)");
			Assert::AreEqual(size_t{ 8 }, LockedQueueTarget(20, 0.4, 5), L"20 at 3:2 -> 8");
			// The latency is kept (within a frame's rounding) across the lock: target / signal Hz
			// unlocked vs frames / film Hz locked, any carrier.
			for (const auto& c : { std::make_pair(60.0, 0.4), std::make_pair(50.0, 0.5), std::make_pair(120.0, 0.2) })
			{
				const size_t target = 12;
				const double unlocked = target / c.first;
				const double locked = LockedQueueTarget(target, c.second, 2) / (c.first * c.second);
				Assert::IsTrue(std::abs(locked - unlocked) <= 0.5 / (c.first * c.second) + 1e-9, L"latency kept");
			}
		}
	};

	TEST_CLASS(Hunt2CadenceClassFilmClockTests)
	{
	public:
		// M1: frames arrive one signal frame earlier from a re-phase on; re-anchored there, the
		// film intervals stay even (no step about 2 s later); without, the anchor steps.
		TEST_METHOD(ReanchorOnARephaseKeepsTheIntervalsEven)
		{
			const double film = 24.0, shift = 1.0 / 60.0;
			for (int on = 0; on < 2; ++on)
			{
				Pulldown::FilmClock clock;
				clock.SetRebaseOnRateChange(true);
				double previous = 0.0, worst = 0.0;
				for (uint64_t n = 1; n < 200; ++n)
				{
					if (on && n == 100)
						clock.Reanchor();
					const double arrival = 5.0 + n / film - (n >= 100 ? shift : 0.0);
					const double t = clock.Time(n, arrival, film);
					Assert::IsTrue(t >= arrival - 1e-12, L"never before arrival");
					if (n > 101)
						worst = (std::max)(worst, std::abs((t - previous) - 1.0 / film));
					previous = t;
				}
				if (on)
					Assert::IsTrue(worst < 1e-9, L"re-anchored: even intervals after the re-phase");
				else
					Assert::IsTrue(worst > 0.5 * shift, L"before: the anchor stepped inside the shot");
			}
		}

		// L6: one late arrival (a callback spike) no longer times every frame late for the window.
		TEST_METHOD(RobustAnchorIgnoresOneSpike)
		{
			const double film = 24.0, spike = 0.010;
			for (int on = 0; on < 2; ++on)
			{
				Pulldown::FilmClock clock;
				Assert::IsFalse(clock.SetRobustAnchor(on == 1), L"nothing timed yet: no restart");
				int late = 0;
				for (uint64_t n = 1; n < 150; ++n)
				{
					const double ideal = 5.0 + n / film;
					const double arrival = ideal + (n == 50 ? spike : 0.0);
					const double t = clock.Time(n, arrival, film);
					Assert::IsTrue(t >= arrival - 1e-12);
					if (n > 50 && n < 90 && t - ideal > 0.5 * spike)
						++late;
				}
				if (on)
					Assert::AreEqual(0, late, L"robust: the frames after the spike keep their time");
				else
					Assert::IsTrue(late > 30, L"before: timed late for the window");
				Assert::IsTrue(clock.SetRobustAnchor(on == 0), L"toggled in use: restarts (caller marks a gap)");
			}
		}
	};

	TEST_CLASS(Hunt2CadenceClassPolicyTests)
	{
	public:
		// M2 / C4: source_class measured classifies the source alone (24p@30, 30p@50 film;
		// 60p@120 video), never by the display ratio; off = the ratio rule.
		TEST_METHOD(MeasuredClassIsSourceOnly)
		{
			using InterpolationPolicy::IsFilmLike;
			using InterpolationPolicy::SourceClass;
			Assert::IsTrue(IsFilmLike(SourceClass::Measured, 24.0, 30.0, false, true), L"24p@30 film");
			Assert::IsTrue(IsFilmLike(SourceClass::Measured, 30.0, 50.0, false, true), L"30p@50 film");
			Assert::IsFalse(IsFilmLike(SourceClass::Measured, 60.0, 120.0, false, true), L"60p@120 video");
			Assert::IsFalse(IsFilmLike(SourceClass::Measured, 50.0, 120.0, true, true), L"50p carried in 120: a lock is not film");
			Assert::IsFalse(IsFilmLike(SourceClass::Measured, 24.0, 30.0, false, false), L"off: 24p@30 video");
			Assert::IsFalse(IsFilmLike(SourceClass::Measured, 30.0, 50.0, false, false), L"off: 30p@50 video");
			Assert::IsTrue(IsFilmLike(SourceClass::Measured, 60.0, 120.0, false, false), L"off: 60p@120 film");
			// The decisions: film goes to the de-judder, 60p@120 smooths in always (video).
			for (const auto& c : { std::make_pair(24.0, 30.0), std::make_pair(30.0, 50.0) })
			{
				InterpolationPolicy::Input in;
				in.mode = InterpolationPolicy::Mode::Auto;
				in.sourceClass = SourceClass::Measured;
				in.sourceHz = c.first;
				in.displayHz = c.second;
				in.haveNext = true;
				in.dejudderStrength = 0.6;
				Assert::IsTrue(InterpolationPolicy::Decide(in).reason == InterpolationPolicy::Reason::FilmToDejudder, L"film to the de-judder");
				in.classFixes = false;
				Assert::IsTrue(InterpolationPolicy::Decide(in).reason == InterpolationPolicy::Reason::Interpolate, L"off: interpolated as video");
			}
			InterpolationPolicy::Input hfr;
			hfr.mode = InterpolationPolicy::Mode::Always;
			hfr.sourceClass = SourceClass::Measured;
			hfr.sourceHz = 60.0;
			hfr.displayHz = 120.0;
			hfr.haveNext = true;
			Assert::IsTrue(InterpolationPolicy::Decide(hfr).reason == InterpolationPolicy::Reason::Interpolate, L"60p@120 always: video");
			hfr.classFixes = false;
			Assert::IsTrue(InterpolationPolicy::Decide(hfr).reason == InterpolationPolicy::Reason::FilmIntegerRatio, L"off: film");
		}

		// L5: film within the cadence tolerance of 1:1 is drift correction's, not interpolated.
		TEST_METHOD(FilmNearOneToOneIsNotInterpolated)
		{
			for (const auto& c : { std::make_pair(24.0, 25.0), std::make_pair(25.0, 25.6), std::make_pair(25.0, 24.0) })
			{
				InterpolationPolicy::Input in;
				in.mode = InterpolationPolicy::Mode::Auto;
				in.sourceHz = c.first;
				in.displayHz = c.second;
				in.haveNext = true;
				in.perVsyncAvailable = true;
				Assert::IsTrue(InterpolationPolicy::Decide(in).reason == InterpolationPolicy::Reason::FilmIntegerRatio, L"film near 1:1");
				in.classFixes = false;
				Assert::IsTrue(InterpolationPolicy::Decide(in).reason == InterpolationPolicy::Reason::Interpolate, L"off: interpolated");
			}
		}
	};

	// ---- content classifier (02-dv M2, M3, L6) ----
	namespace
	{
		using namespace ContentIntent;

		// A dark flat picture with a textured block that moves one cell per new picture
		// (flat share about 0.8; a new picture differs by tens of codes, no cut).
		LumaGrid Picture(int index)
		{
			LumaGrid g;
			g.width = 192;
			g.height = 108;
			g.luma.assign(static_cast<size_t>(g.width) * g.height, 80);
			uint32_t state = 12345;
			std::vector<uint16_t> texture(400);
			for (auto& t : texture) { state = state * 1664525u + 1013904223u; t = static_cast<uint16_t>(80 + (state >> 8) % 300); }
			for (int y = 30; y < 70; ++y)
				for (int x = 40; x < 100; ++x)
					g.luma[static_cast<size_t>(y) * g.width + x] = texture[static_cast<size_t>((x + index) % 200 + (y % 2) * 200)];
			return g;
		}

		FrameContext Ctx(double contentHz, bool classFixes, bool locked = false)
		{
			FrameContext c;
			c.contentHz = contentHz;
			c.displayHz = 60.0;
			c.pulldownLocked = locked;
			c.classFixes = classFixes;
			return c;
		}

		// Feeds `pattern` ('N' new picture, 'R' exact hold) repeatedly for `seconds`.
		ContentClass Run(const std::string& pattern, double contentHz, bool classFixes, double seconds = 12.0)
		{
			Observer o;
			o.Configure(true, 2.0);
			const FrameContext c = Ctx(contentHz, classFixes);
			int picture = 0;
			const int frames = static_cast<int>(seconds * contentHz);
			for (int i = 0; i < frames; ++i)
			{
				if (pattern[static_cast<size_t>(i) % pattern.size()] == 'N')
					++picture;
				o.ObserveGrid(static_cast<uint64_t>(i + 1), Picture(picture), c);
			}
			return o.Status().candidate;   // the class of the latest window (the hysteresis waits for a cut)
		}

		const wchar_t* N(ContentClass c)
		{
			switch (c)
			{
			case ContentClass::Film: return L"film";
			case ContentClass::Video: return L"video";
			case ContentClass::Sports: return L"sports";
			case ContentClass::Animation: return L"animation";
			case ContentClass::StaticUi: return L"static_ui";
			default: return L"unknown";
			}
		}
	}

	TEST_CLASS(Hunt2CadenceClassContentTests)
	{
	public:
		// M2 case A: 24p film in a 60 Hz carrier without a lock (3:2 holds 2,1) is film, not animation.
		TEST_METHOD(ThreeTwoCarrierWithoutALockIsFilm)
		{
			const std::string pulldown = Carried(24, 1, 60.0, 1, 10);
			const ContentClass fixed = Run(pulldown, 60.0, true);
			Assert::IsTrue(fixed == ContentClass::Film, N(fixed));
			const ContentClass old = Run(pulldown, 60.0, false);
			Assert::IsTrue(old == ContentClass::Animation, (std::wstring(L"off: the behaviour before, got ") + N(old)).c_str());
			// 25p in 50 (2:2): film. 30p drawn on twos in 60 (holds of 3): 15 pictures a second,
			// at the pulldown floor - drawn holds, animation (as Pulldown never locks on it).
			Assert::IsTrue(Run("NR", 50.0, true) == ContentClass::Film, L"25 in 50");
			const ContentClass twos = Run("NRRR", 60.0, true);
			Assert::IsTrue(twos == ContentClass::Animation, N(twos));
		}

		// M2 case B: native 24p animation on twos (runs 1,1,1) is animation with the fixes.
		TEST_METHOD(NativeAnimationOnTwosIsAnimation)
		{
			const ContentClass fixed = Run("NR", 24.0, true);
			Assert::IsTrue(fixed == ContentClass::Animation, N(fixed));
			const ContentClass old = Run("NR", 24.0, false);
			Assert::IsTrue(old == ContentClass::Film, (std::wstring(L"off: excluded as even, got ") + N(old)).c_str());
		}

		// M3: dark native-24p film whose noise straddles the hold threshold, flat: film with the
		// fixes (no positive animation evidence); the old rules took it for animation.
		TEST_METHOD(DarkNoisyFilmIsNotAnimation)
		{
			for (int on = 0; on < 2; ++on)
			{
				Statistics stats;
				uint64_t state = 99;
				for (int i = 0; i < 24 * 12; ++i)
				{
					state = state * 6364136223846793005ull + 1442695040888963407ull;
					FrameMeasure m;
					m.valid = true;
					m.meanDiffCodes = 0.9 + static_cast<double>((state >> 33) % 1000) / 1000.0;   // 0.9 .. 1.9
					m.held = m.meanDiffCodes < kHeldCodes;
					m.flatShare = 0.8;
					m.edgeCodes = 1.0;
					stats.Add(m, 24.0);
				}
				ClassifierInput in;
				in.rateKnown = true;
				in.filmLike = true;
				in.classFixes = on == 1;
				in.stats = stats.Current();
				Assert::IsTrue(in.stats.holdShare >= kAnimationHoldShare, L"the noise reads as holds");
				const ContentClass c = Classify(in).content;
				if (on)
					Assert::IsTrue(c == ContentClass::Film, N(c));
				else
					Assert::IsTrue(c == ContentClass::Animation, N(c));
			}
		}

		// M3: a long locked-off shot of film is not a menu (3x the still at a film rate).
		TEST_METHOD(LongFilmStillIsNotStaticUi)
		{
			WindowStats still;
			still.stillSeconds = kStaticUiSeconds + 1.0;
			ClassifierInput in;
			in.rateKnown = true;
			in.filmLike = true;
			in.stats = still;
			Assert::IsTrue(Classify(in).content == ContentClass::Film, L"film still: film");
			in.classFixes = false;
			Assert::IsTrue(Classify(in).content == ContentClass::StaticUi, L"off: static UI");
			in.classFixes = true;
			in.stats.stillSeconds = kStaticUiSeconds * kStaticUiFilmFactor + 1.0;
			Assert::IsTrue(Classify(in).content == ContentClass::StaticUi, L"a really long still: static UI");
			in.filmLike = false;
			in.stats.stillSeconds = kStaticUiSeconds + 1.0;
			Assert::IsTrue(Classify(in).content == ContentClass::StaticUi, L"video rate: unchanged");
		}

		// The observer: rate known without a display rate under measured (source-only class).
		TEST_METHOD(MeasuredNeedsNoDisplayRate)
		{
			FrameContext c;
			c.contentHz = 50.0;
			c.sourceClass = InterpolationPolicy::SourceClass::Measured;
			bool known = false;
			Assert::IsFalse(FilmLike(c, known));
			Assert::IsTrue(known, L"known from the source alone");
			c.classFixes = false;
			FilmLike(c, known);
			Assert::IsFalse(known, L"off: waits for the display rate");
		}

		// Integration round 4: the observer's film check follows general_rate_fixes
		// (40 fps: film by the derived rule, video by the fixed bands).
		TEST_METHOD(ObserverFilmCheckFollowsGeneralRateFixes)
		{
			FrameContext c;
			c.contentHz = 40.0;
			c.displayHz = 60.0;
			c.sourceClass = InterpolationPolicy::SourceClass::Measured;
			bool known = false;
			Assert::IsTrue(FilmLike(c, known), L"on (default): 40 fps is below the video floor, film");
			Assert::IsTrue(known);
			c.generalRates = false;
			Assert::IsFalse(FilmLike(c, known), L"off: the fixed bands, 40 fps is video");
			c.contentHz = 24.0;
			Assert::IsTrue(FilmLike(c, known), L"off: 24p stays film");
			c.generalRates = true;
			c.contentHz = 59.94;
			Assert::IsFalse(FilmLike(c, known), L"on: 59.94 stays video");
		}

		// 02-dv L6: a profile selected by `content` that sets the classifier's inputs warns.
		TEST_METHOD(ContentGatedProfilesWarnForClassifierInputs)
		{
			char directory[MAX_PATH]{}, file[MAX_PATH]{};
			Assert::IsTrue(GetTempPathA(MAX_PATH, directory) != 0);
			Assert::IsTrue(GetTempFileNameA(directory, "vph", 0, file) != 0);
			{
				std::ofstream out(file, std::ios::trunc);
				out << "[general]\nrenderer: VideoProcessor Renderer (Alpha)\n"
					"[vprenderer.everyday]\nquality: high\nintent_content: on\n"
					"[vprenderer.films]\nwhen: $content==\"film\"\nintent_content: off\nsdr_black_nits: 0.05\n";
			}
			ConfigFile config;
			Assert::IsTrue(config.Load(file, ConfigFile::ReadPolicy::Fresh));
			RendererProfileConfig::Model model;
			std::string error;
			const bool read = RendererProfileConfig::Read(config, model, error);
			DeleteFileA(file);
			Assert::IsTrue(read, std::wstring(error.begin(), error.end()).c_str());
			int warned = 0;
			for (const std::string& w : model.warnings)
				warned += w.find("intent_content is ignored") != std::string::npos ? 1 : 0;
			Assert::AreEqual(1, warned, L"one warning, for the content-gated profile only");
			Assert::IsTrue(RendererProfileConfig::IsContentClassifierInput("source_class"));
			Assert::IsTrue(RendererProfileConfig::IsContentClassifierInput("dejudder_pulldown"));
			Assert::IsFalse(RendererProfileConfig::IsContentClassifierInput("sdr_black_nits"));
		}
	};
}
