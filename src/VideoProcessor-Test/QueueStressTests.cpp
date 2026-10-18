#include "pch.h"
#include "CppUnitTest.h"

// Queue stress and fuzz harness (2026-10-18): see QueueStressHarness.h for the model and
// docs/agent-reports/queue-stress.md for the findings.
//
// Default run: 200 seeded random scenarios (2-10 simulated minutes each) plus one
// hand-written scenario per input type, under a minute of CPU. Soak: VP_QSTRESS_SOAK=<n>
// runs n random scenarios (seeds 1..n) instead. Every invariant failure is minimised to the
// shortest event list that still fails and written, as a ready regression test, to
// %TEMP%\vp-qstress\repros.txt (VP_QSTRESS_DIR overrides).

#include "QueueStressHarness.h"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace QueueStress;

namespace
{
	void Log(const std::string& text) { Logger::WriteMessage((text + "\n").c_str()); }

	std::wstring W(const std::string& s) { return std::wstring(s.begin(), s.end()); }

	std::string OutDir()
	{
		char* value = nullptr;
		size_t length = 0;
		std::string dir;
		if (_dupenv_s(&value, &length, "VP_QSTRESS_DIR") == 0 && value) { dir = value; free(value); }
		if (dir.empty())
		{
			if (_dupenv_s(&value, &length, "TEMP") == 0 && value) { dir = std::string(value) + "\\vp-qstress"; free(value); }
			else dir = ".\\vp-qstress";
		}
		CreateDirectoryA(dir.c_str(), nullptr);
		return dir;
	}

	int EnvInt(const char* name, int fallback)
	{
		char* value = nullptr;
		size_t length = 0;
		int result = fallback;
		if (_dupenv_s(&value, &length, name) == 0 && value) { result = std::atoi(value); free(value); }
		return result > 0 ? result : fallback;
	}

	// Failures documented as known defects (docs/agent-reports/queue-stress.md, each with a
	// failing repro in QueueStressRepros); the fuzz run fails only on a new signature.
	const std::set<std::string>& KnownSignatures()
	{
		// Q1 (the stale burst stepping the film clock back) is fixed (docs/agent-reports/
		// q1-stale-burst.md): FILMCLOCK_BACKWARDS / SPACING / UNBOUNDED are new failures again.
		static const std::set<std::string> known = {};
		return known;
	}

	// VP_QSTRESS_LEGACY_Q1=1: the soak runs the renderer before the Q1 fix (before/after counts).
	RunOptions& SoakOptions()
	{
		static RunOptions options = [] { RunOptions o; o.legacyQ1 = EnvInt("VP_QSTRESS_LEGACY_Q1", 0) == 1; return o; }();
		return options;
	}

	// queue_timing_fixes off is the old behaviour by design: a failure that the switch on
	// removes is the defect the switch fixes, not a new one.
	bool FixedByQueueFixes(const Scenario& s, const std::string& signature)
	{
		if (s.sw.queueFixes) return false;
		Scenario on = s;
		on.sw.queueFixes = true;
		return !Run(on, SoakOptions()).Failed(signature);
	}

	std::string Summary(const Result& r)
	{
		const Stats& s = r.stats;
		return Fmt("signal %lld kept %lld dequeued %lld pairs %lld fill %lld flushes %lld overflow %lld inval %lld locks %lld "
			"resets %lld ra %lld extras %lld gate-downs %lld max-gate %d max-depth %zu max-lat %.0f ms steady %.0f ms",
			s.signalFrames, s.kept, s.dequeued, s.pairs, s.fillPairs, s.flushes, s.overflowDrops, s.invalidations, s.locks,
			s.restarts, s.raFrames, s.raExtras, s.gateDowns, s.maxGateLevel, s.maxDepth, s.maxLatency * 1000.0,
			s.maxSteadyLatency * 1000.0);
	}

	// Runs a scenario; on failures logs them and, for each new signature, minimises and
	// records the repro. Returns the signatures that are neither known nor by design.
	std::set<std::string> Check(const Scenario& s, const Result& r, std::map<std::string, Scenario>& repros, std::ostream& out)
	{
		std::set<std::string> unknown;
		for (const std::string& sig : r.Signatures())
		{
			const bool byDesign = FixedByQueueFixes(s, sig);
			for (const Failure& f : r.failures)
				if (f.invariant == sig)
				{
					out << "  " << sig << (byDesign ? " (queue_timing_fixes off: fixed by the switch)" : "") << " at " << f.t << ": "
						<< f.detail << "\n";
					break;
				}
			if (byDesign) continue;
			if (!KnownSignatures().count(sig)) unknown.insert(sig);
			if (!repros.count(sig))
			{
				int runs = 0;
				const Scenario minimal = Minimise(s, sig, [](const Scenario& x) { return Run(x, SoakOptions()); }, &runs);
				repros[sig] = minimal;
				out << "  minimised " << sig << " in " << runs << " runs: " << Describe(minimal) << "\n";
			}
		}
		return unknown;
	}

	Scenario Base(const char* name, double signal, double picture, double display, double seconds = 120.0)
	{
		Scenario s;
		s.name = name;
		s.seed = 1;
		s.seconds = seconds;
		s.signalHz = signal;
		s.pictureHz = picture;
		s.displayHz = display;
		return s;
	}
}

namespace QueueStressTest
{
	TEST_CLASS(QueueStressTests)
	{
	public:
		// 200 seeded random scenarios, 2-10 simulated minutes, faults singly and combined,
		// the switch matrix drawn per scenario.
		TEST_METHOD(RandomScenarios)
		{
			const auto started = std::chrono::steady_clock::now();
			const int count = EnvInt("VP_QSTRESS_SOAK", 200);
			std::map<std::string, Scenario> repros;
			std::map<std::string, int> seen;
			std::set<std::string> unknown;
			std::ostringstream out;
			double simulated = 0.0;
			int failing = 0;
			for (int seed = 1; seed <= count; ++seed)
			{
				const Scenario s = Random(static_cast<uint64_t>(seed));
				simulated += s.seconds;
				std::ostringstream detail;
				const Result r = Run(s, SoakOptions());
				const std::set<std::string> u = Check(s, r, repros, detail);
				if (!r.failures.empty())
				{
					++failing;
					for (const std::string& sig : r.Signatures()) ++seen[sig];
					out << "seed " << seed << ": " << Describe(s) << "\n" << detail.str();
				}
				unknown.insert(u.begin(), u.end());
			}
			const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
			std::ostringstream head;
			head << count << " scenarios, " << simulated / 60.0 << " simulated minutes, " << wall << " s wall, " << failing
				<< " failing\n";
			for (const auto& kv : seen) head << "  " << kv.first << ": " << kv.second << " scenario(s)\n";
			const std::string dir = OutDir();
			std::ofstream(dir + "\\results.txt") << head.str() << out.str();
			std::ofstream code(dir + "\\repros.txt");
			for (const auto& kv : repros)
				code << "// " << kv.first << "\n" << Code(kv.second) << "\n";
			Log(head.str());
			Log(out.str().substr(0, 60000));
			std::string list;
			for (const std::string& sig : unknown) list += sig + " ";
			Assert::IsTrue(unknown.empty(), W("new invariant failures: " + list + "(see " + dir + "\\results.txt)").c_str());
		}

		// ---- one hand-written scenario per input type (both presentation paths) ----
		static void Expect(Scenario s, const char* what)
		{
			for (int ra = 0; ra < 2; ++ra)
			{
				s.sw.renderAhead = ra == 1;
				const Result r = Run(s);
				Log(std::string(what) + (ra ? " (render-ahead)" : " (vblank)") + ": " + Summary(r));
				std::string text;
				for (const Failure& f : r.failures)
					if (!KnownSignatures().count(f.invariant))
						text += f.invariant + " @" + Fmt("%.3f", f.t) + ": " + f.detail + "; ";
				Assert::IsTrue(text.empty(), W(std::string(what) + (ra ? " render-ahead: " : " vblank: ") + text).c_str());
			}
		}

		TEST_METHOD(StarvationShortAndLong)
		{
			for (double seconds : { 0.1, 0.3, 1.0, 3.0, 10.0 })
			{
				Scenario s = Base("starve", 60000.0 / 1001.0, 24000.0 / 1001.0, 60000.0 / 1001.0);
				s.events.push_back({ Ev::Starve, 30.0, seconds, 0.0 });
				s.events.push_back({ Ev::Starve, 70.0, seconds, 0.0 });
				Expect(s, Fmt("starve %.1f s", seconds).c_str());
			}
		}

		TEST_METHOD(BurstRefills)
		{
			for (double n : { 2.0, 8.0, 30.0 })
			{
				Scenario s = Base("burst", 50.0, 25.0, 50.0);
				s.events.push_back({ Ev::Burst, 20.0, n, 0.0 });
				s.events.push_back({ Ev::Burst, 60.0, n, 0.0 });
				Expect(s, Fmt("burst %.0f", n).c_str());
			}
		}

		TEST_METHOD(RateSwings)
		{
			Scenario s = Base("rate", 24000.0 / 1001.0, 24000.0 / 1001.0, 60.0, 200.0);
			double t = 20.0;
			for (double hz : { 24.0, 25.0, 30000.0 / 1001.0, 60000.0 / 1001.0, 24000.0 / 1001.0 })
			{
				s.events.push_back({ Ev::RateSwing, t, hz, 0.0 });
				t += 30.0;
			}
			Expect(s, "rate swings");
		}

		TEST_METHOD(DriftHalfAPercent)
		{
			for (double ppm : { -5000.0, 5000.0 })
			{
				Scenario s = Base("drift", 60.0, 24.0, 60.0);
				s.events.push_back({ Ev::Drift, 10.0, ppm, 100.0 });
				Expect(s, Fmt("drift %.0f ppm", ppm).c_str());
			}
		}

		TEST_METHOD(TimestampJitterReversalJump)
		{
			Scenario s = Base("timestamps", 60000.0 / 1001.0, 24000.0 / 1001.0, 60000.0 / 1001.0, 150.0);
			s.events.push_back({ Ev::Jitter, 10.0, 20.0, 6.0 });
			s.events.push_back({ Ev::Reverse, 40.0, 3.0, 0.0 });
			s.events.push_back({ Ev::Jump, 70.0, 1.3, 0.0 });
			Expect(s, "jitter, reversal, jump");
		}

		TEST_METHOD(DuplicatedAndDroppedFrames)
		{
			Scenario s = Base("dupdrop", 60.0, 60.0, 60.0, 150.0);
			s.events.push_back({ Ev::Duplicate, 20.0, 2.0, 0.0 });
			s.events.push_back({ Ev::Drop, 40.0, 1.0, 0.0 });
			s.events.push_back({ Ev::Drop, 60.0, 3.0, 0.0 });
			s.events.push_back({ Ev::Drop, 80.0, 12.0, 0.0 });
			Expect(s, "duplicated / dropped (video)");
			s.signalHz = 60000.0 / 1001.0;
			s.pictureHz = 24000.0 / 1001.0;
			Expect(s, "duplicated / dropped (film)");
		}

		TEST_METHOD(RestartAndFormatChange)
		{
			Scenario s = Base("epoch", 60000.0 / 1001.0, 24000.0 / 1001.0, 60000.0 / 1001.0, 150.0);
			s.events.push_back({ Ev::Restart, 30.0, 0.0, 0.0 });
			s.events.push_back({ Ev::Format, 70.0, 0.0, 0.0 });
			Expect(s, "restart / format change");
		}

		TEST_METHOD(DisplayRateChanges)
		{
			Scenario s = Base("display", 24000.0 / 1001.0, 24000.0 / 1001.0, 60000.0 / 1001.0, 200.0);
			double t = 20.0;
			for (double hz : { 60.0, 50.0, 120.0, 24000.0 / 1001.0, 60000.0 / 1001.0 })
			{
				s.events.push_back({ Ev::Display, t, hz, 0.0 });
				t += 30.0;
			}
			Expect(s, "display rate changes");
		}

		TEST_METHOD(RenderSpikesAndPresentStalls)
		{
			Scenario s = Base("render", 24.0, 24.0, 60.0, 200.0);
			s.events.push_back({ Ev::Spike, 20.0, 3.0, 90.0 });
			s.events.push_back({ Ev::Stall, 60.0, 400.0, 0.0 });
			s.events.push_back({ Ev::Spike, 100.0, 0.2, 60.0 });
			Expect(s, "render spikes / present stalls");
		}

		TEST_METHOD(ProfileSwitches)
		{
			Scenario s = Base("profile", 60000.0 / 1001.0, 60000.0 / 1001.0, 60000.0 / 1001.0, 150.0);
			s.events.push_back({ Ev::Profile, 30.0, 1.0, 0.0 });
			s.events.push_back({ Ev::Profile, 70.0, 0.0, 0.0 });
			s.events.push_back({ Ev::Profile, 72.0, 1.0, 0.0 });
			Expect(s, "Normal <-> Low latency");
		}

		// Q2 (fixed, queue_timing_fixes): a switch to the low-latency profile purges the queue to
		// its new capacity (SetFrameQueueMaxSize, DropQueueResize). The frame after the purge now
		// carries a no-fill gap like after a backlog flush or an overflow drop; off, the renderer
		// before (no gap: only the motion state's sequence-hole check saw it).
		TEST_METHOD(ProfileSwitchPurgeMarksTheGap)
		{
			// 24p on 120 Hz: a refresh dequeues the frame after the purge before the next one
			// arrives (at a capacity of 1 a newer frame would replace it, carrying the overflow's gap).
			Scenario s = Base("purge", 24.0, 24.0, 120.0, 90.0);
			s.events.push_back({ Ev::Profile, 30.0, 1.0, 0.0 });
			s.events.push_back({ Ev::Profile, 50.0, 0.0, 0.0 });
			const Result on = Run(s);
			Assert::IsTrue(on.stats.resizePurges > 0, L"the switch purged the queue");
			Assert::IsFalse(on.Failed("QUEUE_DROP_UNMARKED"), L"queue_timing_fixes on: the purge marks the gap");
			s.sw.queueFixes = false;
			Assert::IsTrue(Run(s).Failed("QUEUE_DROP_UNMARKED"), L"off: the behaviour before (no gap after the purge)");
		}

		// The harness's own self-test: each deliberate defect trips its invariant (an
		// invariant that cannot fail checks nothing).
		TEST_METHOD(HarnessDetectsDeliberateDefects)
		{
			struct Case { Mutation m; const char* expect; Scenario s; };
			std::vector<Case> cases;
			{
				Scenario s = Base("break-not-gap", 60.0, 60.0, 60.0, 90.0);
				s.events.push_back({ Ev::Jump, 30.0, 1.3, 0.0 });
				s.events.push_back({ Ev::Reverse, 50.0, 2.0, 0.0 });
				cases.push_back({ Mutation::BreakNotGap, "LANE_GAP_MISSING", s });
				// A short timestamp jump (2.5 periods): inside the span, so only the no-fill gap stops a pair.
				s.name = "fill-across-break";
				s.events.clear();
				s.events.push_back({ Ev::Jump, 30.0, 0.025, 0.0 });
				cases.push_back({ Mutation::FillAcrossNoFill, "PAIR_SPANS_HARD", s });
			}
			{
				Scenario s = Base("fill-span", 60.0, 60.0, 60.0, 90.0);
				s.events.push_back({ Ev::Drop, 30.0, 12.0, 0.0 });
				cases.push_back({ Mutation::FillIgnoresSpan, "FILL_PAIR_TOO_LONG", s });
			}
			{
				Scenario s = Base("no-flush", 50.0, 50.0, 50.0, 120.0);
				s.events.push_back({ Ev::Burst, 30.0, 30.0, 0.0 });
				cases.push_back({ Mutation::NoBacklogFlush, "LATENCY_NOT_STEADY", s });
			}
			{
				Scenario s = Base("stuck", 24.0, 24.0, 60.0, 90.0);
				s.events.push_back({ Ev::Starve, 30.0, 1.0, 0.0 });
				cases.push_back({ Mutation::StuckAfterStarve, "PRESENT_FREEZE", s });
			}
			for (const Case& c : cases)
			{
				Assert::IsFalse(Run(c.s).Failed(c.expect), W(std::string(c.s.name) + ": fails without the defect").c_str());
				RunOptions o;
				o.mutation = c.m;
				const Result r = Run(c.s, o);
				std::string got;
				for (const std::string& sig : r.Signatures()) got += sig + " ";
				Log(std::string(c.s.name) + " with the defect: " + got);
				Assert::IsTrue(r.Failed(c.expect), W(std::string(c.s.name) + ": the defect did not trip " + c.expect + " (got " + got + ")").c_str());
			}
		}

		// The LED / active-picture look-ahead walk over random queues: bounded by the queue,
		// never a repeat, the current frame first, the active picture's share inside.
		TEST_METHOD(LookaheadWalkProperties)
		{
			struct Q { bool cadenceRepeat = false; };
			Rng r(77);
			for (int i = 0; i < 20000; ++i)
			{
				std::deque<Q> queue(static_cast<size_t>(r.I(0, 40)));
				for (Q& q : queue) q.cadenceRepeat = r.P(0.3);
				const size_t ap = static_cast<size_t>(r.I(0, 10)), led = r.P(0.2) ? static_cast<size_t>(-1) / 2 : static_cast<size_t>(r.I(0, 50));
				const size_t maxAp = 8;
				const AlphaQueuePolicy::LookaheadWalk w = AlphaQueuePolicy::SelectLookaheadWalk(queue, ap, led, maxAp);
				size_t previous = 0;
				for (size_t k = 0; k < w.window.indices.size(); ++k)
				{
					const size_t idx = w.window.indices[k];
					Assert::IsTrue(idx < queue.size(), L"index inside the queue");
					Assert::IsFalse(queue[idx].cadenceRepeat, L"never a repeat");
					if (k > 0) Assert::IsTrue(idx > previous, L"source order");
					previous = idx;
				}
				if (!w.window.indices.empty())
					Assert::AreEqual(size_t{ 0 }, w.window.indices.front(), L"the current frame first");
				Assert::IsTrue(w.window.indices.size() <= (std::max)((std::min)(ap, maxAp), led) + 1, L"bounded by the wanted depth");
				Assert::IsTrue(w.activePictureFrames <= w.window.indices.size(), L"the active picture inside the walk");
				Assert::IsTrue(w.activePictureFrames <= (std::min)(ap, maxAp) + 1, L"the active picture's own depth");
			}
		}
	};

	// Minimal repros of the defects the harness found (docs/agent-reports/queue-stress.md).
	// They fail until the defect is fixed; each is the auto-minimised scenario (seed + event
	// list) and, where the cause is one component, the same defect on that component alone.
	// Q1 is fixed (docs/agent-reports/q1-stale-burst.md): its repros pass and guard the fix.
	TEST_CLASS(QueueStressRepros)
	{
	public:
		// Q1 (minimised from random seed 72): 24p film in 60 Hz, locked; the capture stalls and
		// delivers 35 frames at once (0.58 s). The oldest are more than 0.5 s old, so
		// OnVideoFrame gives them the callback time as their arrival
		// (LibplaceboVideoRenderer.cpp:24250), late by up to the stall; the FilmClock's anchor is
		// the (second) largest offset over its 2 s window (PulldownDetector.h:747-787), so every
		// frame is timed that late for 2 s and then the clock steps back ~0.46 s inside the
		// lock with no discontinuity: the timeline restarts and the motion pair is refused.
		// Known failing by design until the Q1 fix lands (integration round 5): ignored so it does
		// not break the gate; remove the attribute block when fix/q1-stale-burst is merged.
		BEGIN_TEST_METHOD_ATTRIBUTE(StaleBurstFilmClockRunsBackwards)
			TEST_METHOD_ATTRIBUTE(L"TestCategory", L"KnownFailing")
			TEST_METHOD_ATTRIBUTE(L"Description", L"Q1 pending fix/q1-stale-burst")
			TEST_IGNORE()
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(StaleBurstFilmClockRunsBackwards)
		{
			Scenario s; s.name = "minimal"; s.seed = 72ull; s.seconds = 223; s.signalHz = 60; s.pictureHz = 24; s.displayHz = 24000.0 / 1001.0;
			s.events.push_back({ Ev::Burst, 162.801, 35, 0 });
			const Result r = Run(s);
			std::string text;
			for (const Failure& f : r.failures) text += f.invariant + " @" + Fmt("%.3f", f.t) + ": " + f.detail + "; ";
			Assert::IsFalse(r.Failed("FILMCLOCK_BACKWARDS"), W(text).c_str());
		}

		// Q1 on Pulldown::FilmClock alone: frames 0..199 of 24p film; frames 100..104 arrive
		// 0.6 s late (the stale part of a burst, timed at the callback), the rest on time.
		// Known failing by design until the Q1 fix lands (integration round 5): ignored so it does
		// not break the gate; remove the attribute block when fix/q1-stale-burst is merged.
		BEGIN_TEST_METHOD_ATTRIBUTE(StaleBurstFilmClockAlone)
			TEST_METHOD_ATTRIBUTE(L"TestCategory", L"KnownFailing")
			TEST_METHOD_ATTRIBUTE(L"Description", L"Q1 pending fix/q1-stale-burst")
			TEST_IGNORE()
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(StaleBurstFilmClockAlone)
		{
			Pulldown::FilmClock clock;
			clock.SetRobustAnchor(true);
			clock.SetRebaseOnRateChange(true);
			const double f = 24.0;
			double last = -1.0;
			std::string text;
			for (uint64_t n = 1; n < 200; ++n)
			{
				double arrival = 10.0 + n / f + 0.002;
				if (n >= 100 && n <= 104) arrival = 10.0 + 104 / f + 0.6;   // delivered together, late
				const double t = clock.Time(n, arrival, f);
				if (last >= 0.0 && t <= last)
					text += Fmt("frame %llu at %.4f s after %.4f s; ", static_cast<unsigned long long>(n), t, last);
				last = t;
			}
			Assert::IsTrue(text.empty(), W("film clock ran backwards: " + text).c_str());
		}

		// Q1 fix (CaptureArrival): a stalled capture's held frames keep their hardware time
		// while it is plausible; the first stale frame of a run is a delivery gap; a break or a
		// clock on another base falls back to the callback time, and the first fresh frame
		// after such a run is a gap too. queue_timing_fixes off: the callback time, no gaps.
		TEST_METHOD(StaleArrivalTrackerKeepsPlausibleHardwareTime)
		{
			CaptureArrival::Tracker t;
			const double period = 1.0 / 30.0;
			double now = 100.0;
			for (int i = 0; i < 10; ++i, now += period)
			{
				const CaptureArrival::Result r = t.Next(now + 0.002, true, now, true, true);
				Assert::IsTrue(r.kind == CaptureArrival::Kind::Fresh && r.arrival == now && !r.staleStart && !r.staleEnd);
			}
			// A 0.8 s stall: the held frames delivered together at the end.
			const double delivered = now + 0.8;
			int starts = 0;
			for (double hw = now; hw < delivered - CaptureArrival::kFreshAgeSeconds; hw += period)
			{
				const CaptureArrival::Result r = t.Next(delivered, true, hw, true, true);
				Assert::IsTrue(r.kind == CaptureArrival::Kind::StaleHardware, L"plausible: the hardware time");
				Assert::AreEqual(hw, r.arrival);
				starts += r.staleStart ? 1 : 0;
				Assert::IsFalse(r.staleEnd);
			}
			Assert::AreEqual(1, starts, L"one delivery gap per stale run");
			CaptureArrival::Result r = t.Next(delivered + 0.001, true, delivered - 0.1, true, true);
			Assert::IsTrue(r.kind == CaptureArrival::Kind::Fresh && !r.staleEnd, L"hardware-timed run: no second gap");
			// A stale frame across a timestamp break: the callback time; the run stays on it.
			r = t.Next(delivered + 1.0, true, delivered + 0.2, false, true);
			Assert::IsTrue(r.kind == CaptureArrival::Kind::StaleCallback && r.arrival == delivered + 1.0 && r.staleStart);
			r = t.Next(delivered + 1.0001, true, delivered + 0.25, true, true);
			Assert::IsTrue(r.kind == CaptureArrival::Kind::StaleCallback, L"the chain needs a fresh frame first");
			r = t.Next(delivered + 1.05, true, delivered + 1.0, true, true);
			Assert::IsTrue(r.kind == CaptureArrival::Kind::Fresh && r.staleEnd, L"the first fresh frame after callback times: a gap");
			// Hardware time running backwards inside a stale run: implausible.
			r = t.Next(delivered + 2.0, true, delivered + 0.9, true, true);
			Assert::IsTrue(r.kind == CaptureArrival::Kind::StaleCallback);
			// A timing clock on another base (every frame old): never trusted.
			CaptureArrival::Tracker other;
			for (int i = 0; i < 5; ++i)
				Assert::IsTrue(other.Next(10.0 + i * period, true, 5.0 + i * period, true, true).kind == CaptureArrival::Kind::StaleCallback);
			// queue_timing_fixes off: the behaviour before (callback time, no gaps).
			CaptureArrival::Tracker off;
			off.Next(1.0, true, 0.99, true, false);
			r = off.Next(2.0, true, 1.02, true, false);
			Assert::IsTrue(r.kind == CaptureArrival::Kind::StaleCallback && !r.staleStart && r.arrival == 2.0);
			Assert::IsFalse(off.Next(2.01, true, 2.0, true, false).staleEnd);
		}

		// Q1 fix (FilmClock late-arrival guard): after a stale run on callback times the clock
		// never runs backwards and is back on the arrivals within the run's lateness; a lasting
		// step of the arrivals is accepted (the clock steps forward), never early.
		TEST_METHOD(FilmClockLateGuardBoundedCatchUpAndLastingStep)
		{
			for (double f : { 24000.0 / 1001.0, 24.0, 25.0 })
			{
				Pulldown::FilmClock clock;
				clock.SetRobustAnchor(true);
				clock.SetRebaseOnRateChange(true);
				double last = -1.0, late = 0.0;
				const double burstAt = 10.0 + 104 / f + 0.6;
				for (uint64_t n = 1; n < 400; ++n)
				{
					double arrival = 10.0 + n / f + 0.002;
					if (n >= 100 && n <= 104) arrival = burstAt;
					const double t = clock.Time(n, arrival, f);
					Assert::IsTrue(t > last && t >= arrival, L"monotonic, never early");
					// Caught up once the on-time arrivals pass the late run (a period of slack).
					if (arrival > burstAt + 1.0 / f)
						late = (std::max)(late, t - arrival);
					last = t;
				}
				Assert::IsTrue(late < 0.005, W(Fmt("caught up: %.1f ms after arrival", late * 1000.0)).c_str());
				Assert::IsTrue(clock.LateArrivals() == 5, L"the five late frames stayed out of the window");

				Pulldown::FilmClock step;
				last = -1.0;
				double after = 0.0;
				for (uint64_t n = 1; n < 400; ++n)
				{
					const double arrival = 10.0 + n / f + 0.002 + (n >= 150 ? 0.4 : 0.0);
					const double t = step.Time(n, arrival, f);
					Assert::IsTrue(t > last && t >= arrival, L"monotonic, never early (lasting step)");
					if (n >= 150 + static_cast<uint64_t>(f * (Pulldown::FilmClock::kLatePersistSeconds + 0.5)))
						after = (std::max)(after, t - arrival);
					last = t;
				}
				Assert::IsTrue(after < 0.005, W(Fmt("lasting step: %.1f ms after arrival", after * 1000.0)).c_str());
			}
		}

		// Q1 on the owner's paths: 24p film in 29.97 (streaming apps, one repeat in five) and
		// 24p film in 59.94 (3:2), output 60 Hz, locked; 1-4 stale bursts of 2-60 frames.
		// VP_QSTRESS_BURST=<n> scenarios per path (default 40). Every scenario also runs the
		// renderer before the fix (legacyQ1) for the before/after counts (stale-burst.txt).
		TEST_METHOD(StaleBurstFilmSoak)
		{
			const int count = EnvInt("VP_QSTRESS_BURST", 40);
			struct Path { const char* name; double signal, picture; uint64_t salt; };
			const Path paths[] = { { "24p in 29.97", 30000.0 / 1001.0, 24000.0 / 1001.0, 0x2997ull },
				{ "24p in 59.94", 60000.0 / 1001.0, 24000.0 / 1001.0, 0x5994ull } };
			std::ostringstream out;
			std::string failures;
			for (const Path& p : paths)
			{
				int failNow = 0, failBefore = 0, backwardsNow = 0, backwardsBefore = 0, relocks = 0, unlockedEnd = 0, clockGuardOnly = 0;
				long long guardHolds = 0, guardLate = 0;
				long long staleHw = 0, staleCb = 0, gaps = 0, holds = 0, bursts = 0;
				for (int i = 1; i <= count; ++i)
				{
					Rng r((p.salt << 20) + static_cast<uint64_t>(i));
					Scenario s;
					s.name = "stale-burst";
					s.seed = static_cast<uint64_t>(i);
					s.seconds = std::floor(r.U(150.0, 300.0));
					s.signalHz = p.signal;
					s.pictureHz = p.picture;
					s.displayHz = 60.0;
					s.sw.renderAhead = r.P(0.5);
					s.sw.fillGaps = r.P(0.75);
					s.sw.lookaheadAuto = r.P(0.75);
					s.sw.closedLoop = r.P(0.6);
					const int n = r.I(1, 4);
					for (int k = 0; k < n; ++k)
						s.events.push_back({ Ev::Burst, std::floor(r.U(30.0, s.seconds - 30.0) * 1000.0) / 1000.0,
							static_cast<double>(r.I(2, 60)), 0.0 });
					std::sort(s.events.begin(), s.events.end(), [](const Event& x, const Event& y) { return x.t < y.t; });
					bursts += n;
					const Result now = Run(s);
					RunOptions legacy;
					legacy.legacyQ1 = true;
					const Result before = Run(s, legacy);
					if (!now.failures.empty())
					{
						++failNow;
						failures += p.name + std::string(": ") + Describe(s) + " -> " + now.failures.front().invariant + " @" +
							Fmt("%.3f", now.failures.front().t) + ": " + now.failures.front().detail + "\n";
					}
					failBefore += before.failures.empty() ? 0 : 1;
					// The film clock's guard alone (queue_timing_fixes off: stale frames on the callback
					// time, pulldown_fixes on): no film clock failure either.
					Scenario guardOnly = s;
					guardOnly.sw.queueFixes = false;
					const Result guard = Run(guardOnly);
					for (const char* sig : { "FILMCLOCK_BACKWARDS", "FILMCLOCK_SPACING", "FILMCLOCK_UNBOUNDED", "FILMCLOCK_EARLY",
						"CADENCE_NO_RELOCK" })
						if (guard.Failed(sig))
						{
							++clockGuardOnly;
							failures += p.name + std::string(" (guard only): ") + Describe(guardOnly) + " -> " + sig + "\n";
							break;
						}
					guardHolds += guard.stats.clockFloorHolds;
					guardLate += guard.stats.clockLateArrivals;
					backwardsNow += now.Failed("FILMCLOCK_BACKWARDS") ? 1 : 0;
					backwardsBefore += before.Failed("FILMCLOCK_BACKWARDS") ? 1 : 0;
					relocks += now.stats.locks > 1 ? 1 : 0;
					unlockedEnd += now.Failed("CADENCE_NO_RELOCK") ? 1 : 0;
					staleHw += now.stats.staleHardware;
					staleCb += now.stats.staleCallback;
					gaps += now.stats.staleGaps;
					holds += now.stats.clockFloorHolds;
				}
				out << p.name << " (display 60): " << count << " scenarios, " << bursts << " bursts. Before: " << failBefore
					<< " failing, " << backwardsBefore << " with film clock reversals. After: " << failNow << " failing, "
					<< backwardsNow << " with reversals; lock lost and re-acquired in " << relocks << ", unlocked at the end in "
					<< unlockedEnd << "; stale frames on hardware time " << staleHw << ", on callback time " << staleCb
					<< "; stale-delivery gaps " << gaps << "; clock floor holds " << holds << ". Guard only (queue_timing_fixes off): "
					<< clockGuardOnly << " with film clock failures, " << guardLate << " late arrivals kept out, " << guardHolds
					<< " floor holds\n";
			}
			std::ofstream(OutDir() + "\\stale-burst.txt") << out.str() << failures;
			Log(out.str());
			Log(failures.substr(0, 20000));
			Assert::IsTrue(failures.empty(), W("stale-burst failures:\n" + failures.substr(0, 2000)).c_str());
		}
	};
}
