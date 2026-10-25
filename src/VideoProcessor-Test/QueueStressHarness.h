#pragma once

// Queue stress and fuzz harness (2026-10-18). VP's real CPU-side queue, cadence, timing,
// motion-validity, render-ahead and gap-fill decision logic driven by a deterministic
// simulated clock and a seeded scenario generator. Nothing here runs a GPU or a thread
// (the LED module lane, in QueueStressTests.cpp, runs the module's own thread on the
// manual LedDrive clock).
//
// One scenario = a signal (nominal rate, content picture rate), a display, a switch set
// and a list of timed fault events. The source model turns it into delivered capture
// frames (hardware timestamps on the capture clock, callback times on QPC). Each frame
// goes through the renderer's OnVideoFrame logic (the Lane: CaptureTimestampGap,
// Pulldown::Detector, Pulldown::FilmClock, gap state), then the presentation model:
//   - render_ahead off: the alpha queue paced by the display's vblanks through the
//     timeline's source clock (AlphaQueuePolicy::CanDequeue, TimelineQueueSteering,
//     ShouldRecoverBacklog, MergeDroppedGap on overflow, Pulldown::LockedQueueTarget),
//     the low-latency path one frame per vblank;
//   - render_ahead on: the FIFO back-pressure model of RenderAheadPolicyTests (Schedule,
//     PlanFrame, Gate, CostWindow / KnownCost, SustainedLateness, GateEpoch,
//     CreditLatch), closed loop on simulated DXGI statistics or open loop on the
//     present return times (OpenLoopDisplayRate::LateRefreshes).
// At every dequeue the renderer's pair decision runs (InterpolationTimeline::GapFillPair,
// CaptureTimestampGap::AdjacentPairTimes, MotionValidity::State, the timeline's Plan).
//
// Ground truth travels with every frame (lost frames, hard discontinuities), so the
// invariants compare VP's decisions with what really happened. Every check derives its
// bounds from the scenario's rates; nothing is fixed to a rate (owner rule
// [[vp-general-not-fixed]]).

#include <DisplayRefreshRatePolicy.h>
#include <VP-V/modules/motion/estimation/MotionValidity.h>
#include <vprenderer/AlphaQueuePolicy.h>
#include <vprenderer/CaptureTimestampGap.h>
#include <VP-V/modules/motion/vector_delivery/DejudderPolicy.h>
#include <vprenderer/InterpolationTimeline.h>
#include <vprenderer/OpenLoopDisplayRate.h>
#include <vprenderer/PulldownDetector.h>
#include <vprenderer/RenderAheadPolicy.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace QueueStress
{
	inline std::string Fmt(const char* format, ...)
	{
		char buffer[1024];
		va_list args;
		va_start(args, format);
		vsnprintf(buffer, sizeof(buffer), format, args);
		va_end(args);
		return buffer;
	}

	// splitmix64: the same sequence on every compiler (std distributions are not portable).
	struct Rng
	{
		uint64_t s;
		explicit Rng(uint64_t seed) : s(0)
		{
			// Each seed its own stream (a plain offset would make seed n+1 seed n shifted by one draw).
			uint64_t z = seed + 0x632BE59BD9B4E019ull;
			z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
			z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
			s = z ^ (z >> 31);
		}
		uint64_t Next()
		{
			uint64_t z = (s += 0x9E3779B97F4A7C15ull);
			z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
			z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
			return z ^ (z >> 31);
		}
		double U() { return static_cast<double>(Next() >> 11) * (1.0 / 9007199254740992.0); }
		double U(double a, double b) { return a + (b - a) * U(); }
		int I(int lo, int hi) { return lo + static_cast<int>(Next() % static_cast<uint64_t>(hi - lo + 1)); }
		bool P(double p) { return U() < p; }
		template <typename T> const T& Pick(const std::vector<T>& v) { return v[static_cast<size_t>(Next() % v.size())]; }
	};

	// ---- scenario ----
	enum class Ev
	{
		Starve,     // a: seconds without frames (the capture loses them; timestamps go on)
		Burst,      // a: frames held back, then delivered at once (timestamps intact)
		RateSwing,  // a: the new nominal signal rate (an HDMI mode change: resync)
		Drift,      // a: the signal's true rate off nominal (ppm), b: QPC vs capture clock (ppm)
		Jitter,     // a: seconds, b: callback jitter (ms)
		Reverse,    // a: capture timestamps step back by this many periods
		Jump,       // a: capture timestamps step forward (seconds; never whole periods)
		Duplicate,  // a: the same capture frame delivered again this many times
		Drop,       // a: frames the capture loses (timestamps go on)
		Restart,    // capture restart: a new epoch (queue reset, resync)
		Format,     // a mid-stream format / resolution change (resync, new format)
		Display,    // a: the new display rate
		Spike,      // a: seconds, b: render cost per frame (ms)
		Stall,      // a: present stall (ms): nothing is shown, the render loop blocks
		Profile,    // a: 1 = Low latency, 0 = Normal
		Count
	};

	inline const char* Name(Ev e)
	{
		static const char* const names[] = { "starve", "burst", "rate", "drift", "jitter", "reverse", "jump",
			"duplicate", "drop", "restart", "format", "display", "spike", "stall", "profile" };
		const int i = static_cast<int>(e);
		return i >= 0 && i < static_cast<int>(Ev::Count) ? names[i] : "?";
	}

	struct Event
	{
		Ev kind = Ev::Starve;
		double t = 0.0;
		double a = 0.0, b = 0.0;
	};

	enum class DynamicDrive { Compensate, Script, None };

	struct Switches
	{
		bool renderAhead = false;
		bool fillGaps = true;          // interpolation_fill_gaps (hiccup repair, default on)
		bool lookaheadAuto = true;     // active_picture_lookahead auto (else 0)
		DynamicDrive drive = DynamicDrive::Compensate;
		bool latchFixes = true;        // source_resolution_latch_fixes
		bool queueFixes = true;        // queue_timing_fixes
		bool closedLoop = true;        // DXGI statistics available (render-ahead)
		bool lowLatency = false;       // the profile at the start
	};

	struct Scenario
	{
		std::string name;
		uint64_t seed = 0;
		double seconds = 120.0;
		double signalHz = 60000.0 / 1001.0;
		double pictureHz = 24000.0 / 1001.0;   // the content's picture rate (pulldown when below the signal)
		double displayHz = 60000.0 / 1001.0;
		double baseRenderMs = 0.0;             // 0 = 30% of the source period
		Switches sw;
		std::vector<Event> events;
	};

	inline std::string Describe(const Scenario& s)
	{
		std::string text = Fmt("%s seed=%llu %.0fs signal=%.3f picture=%.3f display=%.3f ra=%d loop=%s fill=%d la=%s qfix=%d ll=%d",
			s.name.c_str(), static_cast<unsigned long long>(s.seed), s.seconds, s.signalHz, s.pictureHz, s.displayHz,
			s.sw.renderAhead ? 1 : 0, s.sw.closedLoop ? "closed" : "open", s.sw.fillGaps ? 1 : 0,
			s.sw.lookaheadAuto ? "auto" : "0", s.sw.queueFixes ? 1 : 0, s.sw.lowLatency ? 1 : 0);
		for (const Event& e : s.events)
			text += Fmt(" [%s@%.3f %.4g %.4g]", Name(e.kind), e.t, e.a, e.b);
		return text;
	}

	// The C++ initializer of a scenario (a repro saved as a regression test).
	inline std::string Code(const Scenario& s)
	{
		std::string text = Fmt("Scenario s; s.name = \"%s\"; s.seed = %lluull; s.seconds = %.17g; s.signalHz = %.17g; "
			"s.pictureHz = %.17g; s.displayHz = %.17g; s.baseRenderMs = %.17g;\n",
			s.name.c_str(), static_cast<unsigned long long>(s.seed), s.seconds, s.signalHz, s.pictureHz, s.displayHz, s.baseRenderMs);
		text += Fmt("s.sw.renderAhead = %s; s.sw.fillGaps = %s; s.sw.lookaheadAuto = %s; s.sw.queueFixes = %s; "
			"s.sw.closedLoop = %s; s.sw.lowLatency = %s;\n",
			s.sw.renderAhead ? "true" : "false", s.sw.fillGaps ? "true" : "false", s.sw.lookaheadAuto ? "true" : "false",
			s.sw.queueFixes ? "true" : "false", s.sw.closedLoop ? "true" : "false", s.sw.lowLatency ? "true" : "false");
		for (const Event& e : s.events)
			text += Fmt("s.events.push_back({ Ev::%s, %.17g, %.17g, %.17g });\n",
				e.kind == Ev::Starve ? "Starve" : e.kind == Ev::Burst ? "Burst" : e.kind == Ev::RateSwing ? "RateSwing" :
				e.kind == Ev::Drift ? "Drift" : e.kind == Ev::Jitter ? "Jitter" : e.kind == Ev::Reverse ? "Reverse" :
				e.kind == Ev::Jump ? "Jump" : e.kind == Ev::Duplicate ? "Duplicate" : e.kind == Ev::Drop ? "Drop" :
				e.kind == Ev::Restart ? "Restart" : e.kind == Ev::Format ? "Format" : e.kind == Ev::Display ? "Display" :
				e.kind == Ev::Spike ? "Spike" : e.kind == Ev::Stall ? "Stall" : "Profile", e.t, e.a, e.b);
		return text;
	}

	// The rates the generator draws from (signal, picture): film in video signals, video,
	// native film signals, HFR. Never a fixed rule in the code under test.
	struct RatePair { double signal, picture; };
	inline const std::vector<RatePair>& Rates()
	{
		static const std::vector<RatePair> rates = {
			{ 60000.0 / 1001.0, 24000.0 / 1001.0 }, { 60.0, 24.0 }, { 50.0, 25.0 }, { 60000.0 / 1001.0, 30000.0 / 1001.0 },
			{ 60000.0 / 1001.0, 60000.0 / 1001.0 }, { 50.0, 50.0 }, { 24000.0 / 1001.0, 24000.0 / 1001.0 }, { 24.0, 24.0 },
			{ 25.0, 25.0 }, { 30000.0 / 1001.0, 30000.0 / 1001.0 }, { 120.0, 24.0 }, { 59.94, 59.94 },
			// Streaming apps: 24p film in 29.97 (one repeat in five), the owner's main case.
			{ 30000.0 / 1001.0, 24000.0 / 1001.0 }, { 30.0, 24.0 } };
		return rates;
	}
	inline const std::vector<double>& DisplayRates()
	{
		static const std::vector<double> rates = { 60000.0 / 1001.0, 60.0, 50.0, 120.0, 24000.0 / 1001.0 };
		return rates;
	}
	inline const std::vector<double>& SwingRates()
	{
		static const std::vector<double> rates = { 24000.0 / 1001.0, 24.0, 25.0, 30000.0 / 1001.0, 60000.0 / 1001.0, 50.0, 60.0 };
		return rates;
	}

	// A random scenario: 2-10 simulated minutes, 2-12 faults singly and combined.
	inline Scenario Random(uint64_t seed, double minSeconds = 120.0, double maxSeconds = 600.0)
	{
		Rng r(seed);
		Scenario s;
		s.name = "random";
		s.seed = seed;
		s.seconds = std::floor(r.U(minSeconds, maxSeconds));
		const RatePair rp = r.Pick(Rates());
		s.signalHz = rp.signal;
		s.pictureHz = rp.picture;
		s.displayHz = r.Pick(DisplayRates());
		s.sw.renderAhead = r.P(0.5);
		s.sw.fillGaps = r.P(0.75);
		s.sw.lookaheadAuto = r.P(0.75);
		s.sw.queueFixes = r.P(0.8);
		s.sw.closedLoop = r.P(0.6);
		s.sw.lowLatency = r.P(0.1);
		s.sw.latchFixes = r.P(0.8);
		const int n = r.I(2, 12);
		for (int i = 0; i < n; ++i)
		{
			Event e;
			e.kind = static_cast<Ev>(r.I(0, static_cast<int>(Ev::Count) - 1));
			e.t = std::floor(r.U(5.0, s.seconds - 30.0) * 1000.0) / 1000.0;
			switch (e.kind)
			{
			case Ev::Starve: e.a = r.P(0.5) ? r.U(0.1, 1.0) : r.U(1.0, 10.0); break;
			case Ev::Burst: e.a = r.I(2, 40); break;
			case Ev::RateSwing: e.a = r.Pick(SwingRates()); break;
			case Ev::Drift: e.a = r.U(-5000.0, 5000.0); e.b = r.U(-200.0, 200.0); break;
			case Ev::Jitter: e.a = r.U(1.0, 30.0); e.b = r.U(0.5, 8.0); break;
			case Ev::Reverse: e.a = r.I(1, 5); break;
			case Ev::Jump: e.a = r.U(0.5, 5.0); break;
			case Ev::Duplicate: e.a = r.I(1, 3); break;
			case Ev::Drop: e.a = r.I(1, 12); break;
			case Ev::Restart: break;
			case Ev::Format: break;
			case Ev::Display: e.a = r.Pick(DisplayRates()); break;
			case Ev::Spike: e.a = r.U(0.1, 4.0); e.b = r.U(20.0, 120.0); break;
			case Ev::Stall: e.a = r.U(20.0, 600.0); break;
			case Ev::Profile: e.a = r.P(0.5) ? 1.0 : 0.0; break;
			default: break;
			}
			s.events.push_back(e);
		}
		std::sort(s.events.begin(), s.events.end(), [](const Event& x, const Event& y) { return x.t < y.t; });
		return s;
	}

	// Deliberate defects (the harness's self-test: each must trip its invariant).
	enum class Mutation { None, BreakNotGap, FillAcrossNoFill, FillIgnoresSpan, NoBacklogFlush, StuckAfterStarve };

	// ---- results ----
	struct Failure
	{
		std::string invariant;   // the signature: invariant name (minimisation keeps it)
		double t = 0.0;
		std::string detail;
	};

	struct Stats
	{
		long long signalFrames = 0, kept = 0, dequeued = 0, pairs = 0, fillPairs = 0, flushes = 0, overflowDrops = 0;
		long long invalidations = 0, locks = 0, restarts = 0, raFrames = 0, raExtras = 0, raLate = 0, gateDowns = 0;
		long long holds = 0, resizePurges = 0, reanchors = 0;
		// Q1: stale frames on hardware / callback time, stale-delivery gaps, film clock floor holds.
		long long staleHardware = 0, staleCallback = 0, staleGaps = 0, clockFloorHolds = 0, clockLateArrivals = 0;
		size_t maxDepth = 0, maxPending = 0, maxCostWindow = 0;
		double maxLatency = 0.0, maxSteadyLatency = 0.0;
		int maxGateLevel = 0;
	};

	struct Result
	{
		std::vector<Failure> failures;
		Stats stats;
		bool Failed(const std::string& invariant) const
		{
			for (const Failure& f : failures)
				if (f.invariant == invariant) return true;
			return false;
		}
		std::set<std::string> Signatures() const
		{
			std::set<std::string> s;
			for (const Failure& f : failures) s.insert(f.invariant);
			return s;
		}
	};

	// ---- the source model ----
	constexpr long long kTicksPerSecond = 10000000;   // the capture clock (100 ns)
	constexpr double kFreshAgeSeconds = CaptureArrival::kFreshAgeSeconds;   // OnVideoFrame's fresh limit

	struct SignalFrame
	{
		long long index = 0;
		double callback = 0.0;    // QPC seconds the callback runs (delivery)
		double arrival = 0.0;     // the renderer's arrival (the lane sets it: CaptureArrival, as OnVideoFrame)
		double hardware = 0.0;    // the hardware arrival on the QPC clock (the end of the frame)
		long long ticks = 0;      // the capture clock
		bool repeat = false;      // the content repeats the previous delivered frame
		bool resync = false;      // a resync before it (restart / format / rate change)
		bool formatChange = false;
		double nominalHz = 0.0;
		uint64_t picture = 0;     // the content picture shown
		// Ground truth since the previous delivered frame.
		long long lost = 0;       // signal frames lost (never delivered)
		bool hard = false;        // a timestamp reversal / jump, a duplicate, a resync
		bool duplicate = false;   // a duplicate delivery of the previous frame
	};

	struct DisplayEvent { double t; double hz; };
	struct WindowEvent { double t0, t1; double value; };

	// Generates the delivered frames, sorted by callback time.
	class Source
	{
	public:
		explicit Source(const Scenario& s) : sc(s) {}

		std::vector<SignalFrame> Generate()
		{
			std::vector<SignalFrame> out;
			std::vector<Event> ev = sc.events;
			std::sort(ev.begin(), ev.end(), [](const Event& x, const Event& y) { return x.t < y.t; });
			size_t next = 0;
			double signalHz = sc.signalHz, pictureHz = sc.pictureHz;
			double rateFactor = 1.0, skew = 0.0;   // true rate / nominal; QPC = capture * (1 + skew)
			const double t0 = 1.0;                 // capture time origin (seconds)
			double slotTime = t0;                  // capture time of the next slot
			double tickOffset = 0.0;               // timestamp steps (reverse / jump), seconds
			double qpcOffset = 0.25;               // QPC - capture at t0
			double lastCallback = 0.0;
			long long index = 0, pendingLost = 0;
			bool pendingHard = false, pendingResync = false, pendingFormat = false;
			int burstLeft = 0;
			std::vector<SignalFrame> burst;
			double jitterUntil = -1.0, jitterMs = 0.0;
			double starveUntil = -1.0;
			int dropLeft = 0, duplicateLeft = 0;
			uint64_t lastPicture = UINT64_MAX;
			double pictureOrigin = t0;
			Rng rng(sc.seed ^ 0xA5A5A5A5ull);
			const double end = t0 + sc.seconds;
			while (slotTime < end)
			{
				const double rel = slotTime - t0;
				while (next < ev.size() && ev[next].t <= rel)
				{
					const Event& e = ev[next++];
					switch (e.kind)
					{
					case Ev::Starve: starveUntil = slotTime + e.a; break;
					case Ev::Burst: burstLeft = static_cast<int>(e.a); break;
					case Ev::RateSwing:
						signalHz = e.a;
						pictureHz = pictureHz > signalHz * 0.99 ? signalHz : pictureHz;
						if (pictureHz > signalHz) pictureHz = signalHz;
						pendingResync = pendingHard = true;
						pictureOrigin = slotTime;
						lastPicture = UINT64_MAX;
						break;
					case Ev::Drift:
					{
						// The QPC mapping stays continuous at the change (a clock rate, not a step).
						const double newSkew = e.b * 1e-6;
						qpcOffset += slotTime * (skew - newSkew);
						rateFactor = 1.0 + e.a * 1e-6;
						skew = newSkew;
						break;
					}
					case Ev::Jitter: jitterUntil = slotTime + e.a; jitterMs = e.b; break;
					case Ev::Reverse: tickOffset -= e.a / signalHz; pendingHard = true; break;
					case Ev::Jump:
					{
						// Never whole periods within the missed-frame span: that is indistinguishable
						// from lost frames for any capture.
						double j = e.a;
						const double frac = j * signalHz - std::floor(j * signalHz);
						if (frac < 0.3 || frac > 0.7) j += (0.5 - frac) / signalHz;
						tickOffset += j;
						pendingHard = true;
						break;
					}
					case Ev::Duplicate: duplicateLeft = static_cast<int>(e.a); break;
					case Ev::Drop: dropLeft = static_cast<int>(e.a); break;
					case Ev::Restart: pendingResync = pendingHard = true; break;
					case Ev::Format: pendingResync = pendingHard = pendingFormat = true; break;
					default: break;   // display / render / profile events: the presentation model
					}
				}
				const double period = 1.0 / (signalHz * rateFactor);
				const double capture = slotTime;
				slotTime += period;
				// What the slot shows.
				const uint64_t picture = static_cast<uint64_t>(std::floor((capture - pictureOrigin) * pictureHz * rateFactor + 1e-6));
				const bool repeat = picture == lastPicture;
				if (capture < starveUntil || dropLeft > 0)
				{
					if (dropLeft > 0) --dropLeft;
					++pendingLost;
					lastPicture = picture;   // the content moved on unseen
					continue;
				}
				lastPicture = picture;
				SignalFrame f;
				f.index = index++;
				f.ticks = std::llround((capture + tickOffset) * static_cast<double>(kTicksPerSecond));
				f.nominalHz = signalHz;
				f.repeat = repeat;
				f.picture = picture;
				f.lost = pendingLost;
				f.hard = pendingHard;
				f.resync = pendingResync;
				f.formatChange = pendingFormat;
				pendingLost = 0;
				pendingHard = pendingResync = pendingFormat = false;
				const double hardwareQpc = capture * (1.0 + skew) + qpcOffset;
				double callback = hardwareQpc + 0.002;   // the callback runs 2 ms after the frame ends
				if (capture < jitterUntil)
					callback += rng.U(0.0, jitterMs) * 1e-3;
				if (burstLeft > 0)
				{
					--burstLeft;
					burst.push_back(f);
					burst.back().arrival = hardwareQpc;
					burst.back().hardware = hardwareQpc;
					if (burstLeft == 0)
					{
						for (SignalFrame& b : burst)
						{
							b.callback = (std::max)(callback, lastCallback + 1e-5);
							lastCallback = b.callback;
							b.arrival = b.callback - b.arrival < kFreshAgeSeconds ? b.arrival : b.callback;
							out.push_back(b);
						}
						burst.clear();
					}
					continue;
				}
				f.callback = (std::max)(callback, lastCallback + 1e-5);   // callbacks are serial
				lastCallback = f.callback;
				f.arrival = f.callback - hardwareQpc < kFreshAgeSeconds ? hardwareQpc : f.callback;
				f.hardware = hardwareQpc;
				out.push_back(f);
				while (duplicateLeft > 0)
				{
					--duplicateLeft;
					SignalFrame d = f;
					d.index = index++;
					d.callback = lastCallback + 0.001;
					lastCallback = d.callback;
					d.repeat = true;
					d.lost = 0;
					d.hard = true;
					d.duplicate = true;
					d.resync = d.formatChange = false;
					out.push_back(d);
				}
			}
			for (SignalFrame& b : burst)   // a burst still held at the end
			{
				b.callback = (std::max)(lastCallback + 1e-5, b.arrival);
				lastCallback = b.callback;
				out.push_back(b);
			}
			return out;
		}

		std::vector<DisplayEvent> Displays() const
		{
			std::vector<DisplayEvent> d;
			for (const Event& e : sc.events)
				if (e.kind == Ev::Display) d.push_back({ 1.25 + e.t, e.a });
			return d;
		}

	private:
		const Scenario& sc;
	};

	// ---- the kept frame (the renderer's QueuedFrame fields that matter here) ----
	struct Kept
	{
		double enqueue = 0.0;        // QPC seconds (the callback)
		double arrival = 0.0;        // the renderer's arrival
		double captureTime = 0.0;    // the capture timestamp (seconds)
		double time = 0.0;           // the timeline's time: film clock while locked, else the capture timestamp
		double filmHz = 0.0;
		double signalHz = 0.0;
		bool gapBefore = false;
		uint32_t gapFrames = 0;
		bool filmUpdate = false;
		bool resync = false;
		int cadenceCycle = 0, cadenceUnique = 0;   // the lock at this frame (0 = none)
		uint64_t laneSequence = 0;   // consecutive per kept frame
		uint64_t picture = 0;
		// Ground truth since the previous kept frame (merged over pulldown drops).
		long long lost = 0;
		bool hard = false;
	};

	// ---- the lane: LibplaceboVideoRenderer::OnVideoFrame's decisions ----
	struct LaneCheck
	{
		long long clockNonMonotonic = 0, clockSpacingOver1ms = 0, clockSamples = 0;
		double clockMaxSpacingErrorMs = 0.0, clockMaxLateMs = 0.0;
		std::vector<Failure>* failures = nullptr;
	};

	class Lane
	{
	public:
		Lane(const Scenario& s, Result& r) : sc(s), result(r) { Restart(); }

		std::vector<Kept> kept;

		void Restart()
		{
			pd.reset(new Pulldown::Detector());
			clock = Pulldown::FilmClock();
			floorHoldsSeen = lateSeen = 0;
			settingsApplied = false;
			forceGap = false;
			filmTime = 0.0;
			firstTicks = lastTicks = publishTicks = 0;
			rateFrames = 0;
			measuredHz = 0.0;
			lastClock = 0.0;
			lastClockFilmHz = 0.0;
			lastDisturbance = -1e9;
			disturbFilmHz = 0.0;
		}

		void OnSignal(const SignalFrame& delivered)
		{
			SignalFrame f = delivered;   // the arrival is the lane's decision (below)
			++result.stats.signalFrames;
			pendingLost += f.lost;
			pendingHard = pendingHard || f.hard;
			if (f.resync)
			{
				Restart();
				resyncPending = true;
			}
			nominalHz = f.nominalHz;
			bool gapBefore = false;
			uint32_t gapFrames = 0;
			const long long expected = f.nominalHz > 0.0 ? std::llround(kTicksPerSecond / f.nominalHz) : 0;
			bool brk = false;
			long long missed = 0;
			if (lastTicks > 0 && expected > 0)
			{
				const CaptureTimestampGap::Step step = CaptureTimestampGap::Classify(f.ticks - lastTicks, expected, kTicksPerSecond);
				if (step.kind == CaptureTimestampGap::Kind::Missed)
				{
					missed = step.periods - 1;
					rateFrames += step.periods - 1;
				}
				else if (step.kind == CaptureTimestampGap::Kind::Break)
					brk = true;
			}
			if (brk)
			{
				firstTicks = lastTicks = publishTicks = f.ticks;
				rateFrames = 1;
				measuredHz = 0.0;
			}
			else if (firstTicks == 0)
			{
				firstTicks = lastTicks = publishTicks = f.ticks;
				rateFrames = 1;
			}
			else
			{
				++rateFrames;
				if (f.ticks - publishTicks >= 5 * kTicksPerSecond && rateFrames > 1)
				{
					publishTicks = f.ticks;
					measuredHz = static_cast<double>(rateFrames - 1) / (static_cast<double>(f.ticks - firstTicks) / kTicksPerSecond);
				}
			}
			lastTicks = f.ticks;
			// Q1 (queue_timing_fixes): the arrival, as OnVideoFrame (CaptureArrival); a stale
			// delivery's gap is marked on the kept frame after the detector.
			{
				const CaptureArrival::Result ar = arrivals.Next(f.callback, true, f.hardware, !brk && !f.resync,
					sc.sw.queueFixes && !legacyQ1);
				f.arrival = ar.arrival;
				if (ar.kind == CaptureArrival::Kind::StaleHardware) ++result.stats.staleHardware;
				if (ar.kind == CaptureArrival::Kind::StaleCallback) ++result.stats.staleCallback;
				if (ar.staleStart || ar.staleEnd) staleGapPending = true;
			}
			if (missed > 0)
			{
				gapBefore = true;
				gapFrames = static_cast<uint32_t>((std::min)(missed, 1000LL));
			}
			if (brk && mutation != Mutation::BreakNotGap)   // timing_continuity_fixes (default on): a break is a no-fill gap
			{
				gapBefore = true;
				gapFrames = 0;
			}
			if (f.resync)
			{
				gapBefore = true;
				gapFrames = 0;
			}
			const double signalHz = SignalHz();
			if (!settingsApplied)
			{
				Pulldown::Settings s;   // pulldown_fixes and cadence_class_fixes on (defaults)
				pd->SetSettings(s);
				settingsApplied = true;
			}
			clock.SetLateGuard(!legacyQ1);   // pulldown_fixes (default on)
			if (clock.SetRobustAnchor(true)) { pd->MarkDiscontinuity(); forceGap = true; }
			if (clock.SetRebaseOnRateChange(true)) { pd->MarkDiscontinuity(); forceGap = true; }
			const bool wasLocked = pd->Locked();
			const Pulldown::Decision d = pd->Next(f.repeat, gapBefore, signalHz,
				gapBefore ? static_cast<int>((std::min)(missed, 1000LL)) : -1);
			if (d.locked != wasLocked || d.relocked)
			{
				gapBefore = true;
				gapFrames = 0;
				clock.Reset();
				if (d.locked) ++result.stats.locks;
				lockChangeAt = f.callback;
			}
			if (d.locked)
				lastLockedAt = f.callback;
			locked = d.locked;
			if (d.drop)
			{
				if (gapBefore)
					pd->MarkDiscontinuity();
				return;   // the repeat is not queued; its truth carries to the next kept frame
			}
			if (staleGapPending)
			{
				staleGapPending = false;
				gapBefore = true;
				gapFrames = 0;
				++result.stats.staleGaps;
			}
			if (gapBefore || d.rephased || d.update || d.phaseUncertain || FilmHzChanged(d))
				lastDisturbance = f.callback;
			const bool forced = forceGap;
			forceGap = false;
			if (forced) { gapBefore = true; gapFrames = 0; }
			if (pd->ConsumeDiscontinuity()) { gapBefore = true; gapFrames = 0; }
			double filmHz = 0.0, time = f.ticks / static_cast<double>(kTicksPerSecond);
			bool filmUpdate = false;
			if (d.rephased)
			{
				gapFrames = 0;
				clock.Reanchor();
			}
			if (d.locked)
			{
				filmHz = d.filmHz;
				if (d.update && filmTime > 0.0)
				{
					filmUpdate = true;
					time = filmTime;
				}
				else
				{
					time = clock.Time(d.filmIndex, f.arrival, filmHz, d.cadenceLateness, d.cadenceLatenessMax);
					result.stats.clockFloorHolds += static_cast<long long>(clock.FloorHolds()) - floorHoldsSeen;
					result.stats.clockLateArrivals += static_cast<long long>(clock.LateArrivals()) - lateSeen;
					floorHoldsSeen = static_cast<long long>(clock.FloorHolds());
					lateSeen = static_cast<long long>(clock.LateArrivals());
					CheckFilmClock(f, d, time, filmHz);
					filmTime = time;
				}
			}
			else
			{
				filmTime = 0.0;
				lastClock = 0.0;
			}
			Kept k;
			k.enqueue = f.callback;
			k.arrival = f.arrival;
			k.captureTime = f.ticks / static_cast<double>(kTicksPerSecond);
			k.time = time;
			k.filmHz = filmHz;
			k.signalHz = signalHz;
			k.gapBefore = gapBefore;
			k.gapFrames = gapFrames;
			k.filmUpdate = filmUpdate;
			k.resync = resyncPending;
			k.cadenceCycle = d.locked ? d.cycle : 0;
			k.cadenceUnique = d.locked ? d.unique : 0;
			k.laneSequence = ++sequence;
			k.picture = f.picture;
			k.lost = pendingLost;
			k.hard = pendingHard;
			resyncPending = false;
			pendingLost = 0;
			pendingHard = false;
			// Invariant: a discontinuity reaches the queue as a gap; a hard one as a no-fill gap.
			if ((k.hard || k.lost > 0) && !k.gapBefore)
				result.failures.push_back({ "LANE_GAP_MISSING", f.callback,
					Fmt("kept frame %llu after %lld lost / hard=%d carries no gap", static_cast<unsigned long long>(k.laneSequence),
						k.lost, k.hard ? 1 : 0) });
			if (k.hard && k.gapBefore && k.gapFrames > 0)
				result.failures.push_back({ "LANE_HARD_FILLABLE", f.callback,
					Fmt("kept frame %llu after a hard discontinuity has gapFrames %u (a gap pair may span it)",
						static_cast<unsigned long long>(k.laneSequence), k.gapFrames) });
			++result.stats.kept;
			kept.push_back(k);
		}

		bool Locked() const { return locked; }
		double LastLockChange() const { return lockChangeAt; }
		double LastDisturbance() const { return lastDisturbance; }

	private:
		double SignalHz() const { return measuredHz > 0.0 ? measuredHz : nominalHz; }

		bool FilmHzChanged(const Pulldown::Decision& d)
		{
			const bool changed = d.locked && d.filmHz != disturbFilmHz;
			disturbFilmHz = d.locked ? d.filmHz : 0.0;
			return changed;
		}

		// The film clock (as RealFootageSuite's C7): never before the arrival, monotonic, and
		// in quiet stretches spaced exactly 1/f per film frame (1 ms); bounded lateness.
		void CheckFilmClock(const SignalFrame& f, const Pulldown::Decision& d, double time, double filmHz)
		{
			if (time + 1e-9 < f.arrival)
				result.failures.push_back({ "FILMCLOCK_EARLY", f.callback, Fmt("film time %.6f before arrival %.6f", time, f.arrival) });
			const bool quiet = f.callback - lastDisturbance >= Pulldown::FilmClock::kWindowSeconds + 0.5 &&
				f.callback - quietResetAt >= Pulldown::FilmClock::kWindowSeconds + 0.5;
			if (lastClock > 0.0 && lastClockFilmHz == filmHz)
			{
				const double dt = time - lastClock;
				// Within one lock with no gap, re-phase, update or rate change at this frame the
				// film time never runs backwards (the timeline would restart, the pair be refused).
				if (dt <= 0.0 && lastDisturbance < f.callback)
					result.failures.push_back({ "FILMCLOCK_BACKWARDS", f.callback,
						Fmt("film time stepped by %.3f ms inside a lock with no discontinuity (film %.3f Hz)", dt * 1000.0, filmHz) });
				if (quiet && d.filmIndex > lastFilmIndex)
				{
					const double err = std::fabs(dt - static_cast<double>(d.filmIndex - lastFilmIndex) / filmHz) * 1000.0;
					if (err > 1.0 && !jitterNow)
						result.failures.push_back({ "FILMCLOCK_SPACING", f.callback,
							Fmt("quiet lock: spacing off by %.3f ms (film %.3f Hz)", err, filmHz) });
				}
			}
			// Lateness bound: the cadence's own lateness, the window's arrival spread and a period.
			const double late = time - f.arrival;
			const double bound = d.cadenceLatenessMax + 2.0 / filmHz + 0.02;
			if (late > bound && quiet)
				result.failures.push_back({ "FILMCLOCK_UNBOUNDED", f.callback,
					Fmt("film time %.1f ms after arrival (bound %.1f ms)", late * 1000.0, bound * 1000.0) });
			lastFilmIndex = d.filmIndex;
			lastClock = time;
			lastClockFilmHz = filmHz;
		}

	public:
		// Scenario knowledge the clock checks need: arrival jitter or a burst is a disturbance
		// of the arrivals themselves (the clock may follow it), not of the lock.
		void NoteArrivalDisturbance(double now) { quietResetAt = now; }
		bool jitterNow = false;
		Mutation mutation = Mutation::None;
		// Q1 off (the renderer before 2026-10-18): stale frames on the callback time, no gap,
		// no late-arrival guard in the film clock. For before/after comparisons only.
		bool legacyQ1 = false;

	private:
		const Scenario& sc;
		Result& result;
		std::unique_ptr<Pulldown::Detector> pd;
		Pulldown::FilmClock clock;
		bool settingsApplied = false, forceGap = false, locked = false, resyncPending = false;
		double filmTime = 0.0, lastClock = 0.0, lastClockFilmHz = 0.0, lastDisturbance = -1e9, disturbFilmHz = 0.0;
		double quietResetAt = -1e9, lockChangeAt = -1e9, lastLockedAt = -1e9;
		uint64_t lastFilmIndex = 0, sequence = 0;
		long long firstTicks = 0, lastTicks = 0, publishTicks = 0, rateFrames = 0;
		double measuredHz = 0.0, nominalHz = 0.0;
		long long pendingLost = 0;
		bool pendingHard = false;
		CaptureArrival::Tracker arrivals;   // not restarted on a resync (as the renderer)
		bool staleGapPending = false;
		long long floorHoldsSeen = 0, lateSeen = 0;   // the clock's counters (restarted with it)
	};

	// ---- the presentation model ----
	struct QItem
	{
		Kept k;
		uint64_t sourceSequence = 0;   // the renderer's m_sourceSequence (enqueue order)
		uint64_t generation = 0;
		bool queueDropBefore = false;  // truth: frames were dropped from the queue right before it
		long long droppedTruthLost = 0;
		bool droppedTruthHard = false;
	};

	struct PresentConfig
	{
		size_t normalCapacity = 32, normalTarget = 5;
		size_t lowCapacity = 1, lowTarget = 1;
		double profileResetDelay = 5.0;   // the queue policy reset after a profile switch
		int motionAheadDepth = 4;         // NVOF look-ahead on by default (owner 2026-10-16)
		double holdFraction = 0.0;        // VP-V delay budget: the sub-frame rest of the hold (DelayBudget::Plan)
		bool budgetActive = false;        // VP-V delay budget: normalTarget is the hold's whole frames (no film scaling)
	};

	class Presentation
	{
	public:
		Presentation(const Scenario& s, Result& r, const PresentConfig& c = PresentConfig()) : sc(s), result(r), cfg(c) {}

		void Run(const std::vector<Kept>& kept)
		{
			// Fault windows (presentation side).
			for (const Event& e : sc.events)
			{
				const double t = 1.25 + e.t;   // capture origin + the QPC offset
				if (e.kind == Ev::Spike) spikes.push_back({ t, t + e.a, e.b });
				if (e.kind == Ev::Stall) stalls.push_back({ t, t + e.a / 1000.0, 0.0 });
				if (e.kind == Ev::Display) displays.push_back({ t, e.a });
				if (e.kind == Ev::Profile) profiles.push_back({ t, t, e.a });
			}
			std::sort(displays.begin(), displays.end(), [](const DisplayEvent& a, const DisplayEvent& b) { return a.t < b.t; });
			std::sort(profiles.begin(), profiles.end(), [](const WindowEvent& a, const WindowEvent& b) { return a.t0 < b.t0; });
			lowLatency = sc.sw.lowLatency;
			capacity = lowLatency ? cfg.lowCapacity : cfg.normalCapacity;
			target = lowLatency ? cfg.lowTarget : cfg.normalTarget;
			displayHz = sc.displayHz;
			if (kept.empty()) return;
			if (sc.sw.renderAhead && !lowLatency)
				RunRenderAhead(kept);
			else
				RunVblank(kept);
			Finish();
		}

	private:
		// ---------------- shared queue mechanics ----------------
		double RenderMs(double now) const
		{
			double ms = sc.baseRenderMs > 0.0 ? sc.baseRenderMs : 0.30 * 1000.0 / (std::max)(1.0, CurrentRate());
			for (const WindowEvent& w : spikes)
				if (now >= w.t0 && now < w.t1) ms = (std::max)(ms, w.value);
			return ms;
		}
		double StallEnd(double now) const
		{
			for (const WindowEvent& w : stalls)
				if (now >= w.t0 && now < w.t1) return w.t1;
			return now;
		}
		double CurrentRate() const
		{
			if (!queue.empty())
				return queue.front().k.filmHz > 0.0 ? queue.front().k.filmHz : queue.front().k.signalHz;
			return lastRate > 0.0 ? lastRate : sc.signalHz;
		}

		size_t GateTarget() const
		{
			if (mutation == Mutation::StuckAfterStarve && everEmptied)
				return capacity + 1;
			return Target();
		}

		size_t Target() const
		{
			// PrefillTargetLocked: LockedFilmQueueTarget (cadence_class_fixes on), the render-ahead
			// credit (never below 2), the queue-fixes floor (the look-ahead frames).
			size_t desired = AlphaQueuePolicy::ClampDesiredDepthToCapacity(target, capacity);
			const size_t lookahead = !lowLatency && sc.sw.lookaheadAuto ? (std::min)({ target - 1, capacity - 1, size_t{ 8 } }) : 0;
			// A delay budget's hold is time-based already: no film-lock scaling (the renderer's rule).
			if (cadence != 0 && !cfg.budgetActive)
			{
				size_t minimum = 2;
				if (!lowLatency && cfg.motionAheadDepth > 0 && sc.sw.lookaheadAuto)
					minimum = (std::max)(minimum, static_cast<size_t>(cfg.motionAheadDepth) + 1);
				if (lookahead > 0)
					minimum = (std::max)(minimum, lookahead + 1);
				desired = Pulldown::LockedQueueTarget(desired, Pulldown::FilmToSignalRatio(cadence >> 16, cadence & 0xffff), minimum);
			}
			// The renderer's own rule (PrefillTargetLocked): credit, look-ahead floor capped at desired.
			return AlphaQueuePolicy::CombineQueueTarget(desired, credit, lookahead, sc.sw.queueFixes);
		}

		void Enqueue(const Kept& k)
		{
			if (k.resync)
				ResetQueue("resync");
			QItem item;
			item.k = k;
			item.generation = generation;
			// m_pulldownCadence is published by OnVideoFrame (at enqueue).
			cadence = k.cadenceCycle > 0 ? (k.cadenceCycle << 16) | k.cadenceUnique : 0;
			// Overflow: the oldest frame is dropped and its gap merged into its successor.
			bool dropped = false;
			while (queue.size() >= capacity)
			{
				QItem victim = queue.front();
				queue.pop_front();
				++result.stats.overflowDrops;
				dropped = true;
				const AlphaQueuePolicy::GapState droppedGap{ victim.k.gapBefore, victim.k.gapFrames };
				QItem* successor = queue.empty() ? &item : &queue.front();
				const AlphaQueuePolicy::GapState merged = AlphaQueuePolicy::MergeDroppedGap(droppedGap,
					AlphaQueuePolicy::GapState{ successor->k.gapBefore, successor->k.gapFrames }, sc.sw.queueFixes);
				successor->k.gapBefore = merged.gapBefore;
				successor->k.gapFrames = merged.gapFrames;
				successor->queueDropBefore = true;
				successor->droppedTruthLost += victim.k.lost + victim.droppedTruthLost + 1;
				successor->droppedTruthHard = successor->droppedTruthHard || victim.k.hard || victim.droppedTruthHard;
			}
			(void)dropped;
			item.sourceSequence = ++sourceSequence;
			queue.push_back(item);
			result.stats.maxDepth = (std::max)(result.stats.maxDepth, queue.size());
			if (queue.size() > capacity)
				Fail("QUEUE_OVER_CAPACITY", k.enqueue, Fmt("depth %zu > capacity %zu", queue.size(), capacity));
		}

		void ResetQueue(const char* why)
		{
			(void)why;
			queue.clear();
			if (++generation == 0) ++generation;
			startup = true;
			steering.Reset();
			mv.Invalidate(MotionValidity::Cause::QueueReset);
			lastDequeued.reset();
			haveTimeline = false;
			timeline.Reset();
			raEpochGeneration = generation;
			++result.stats.restarts;
		}

		// A profile switch (VideoProcessorDlg::ApplyUnifiedProfile): SetFrameQueueMaxSize purges
		// the queue down to the new capacity (DropQueueResize: front frames popped, nothing
		// marked), the target follows, and the queue policy reset comes after the delay.
		void ApplyProfiles(double now)
		{
			while (nextProfile < profiles.size() && profiles[nextProfile].t0 <= now)
			{
				const WindowEvent& p = profiles[nextProfile++];
				const bool low = p.value > 0.5;
				if (low == lowLatency && !pendingReset) continue;
				lowLatency = low;
				capacity = low ? cfg.lowCapacity : cfg.normalCapacity;
				target = low ? cfg.lowTarget : cfg.normalTarget;
				size_t purged = 0;
				long long lostTruth = 0;
				bool hardTruth = false;
				while (queue.size() > capacity)
				{
					lostTruth += queue.front().k.lost + queue.front().droppedTruthLost + 1;
					hardTruth = hardTruth || queue.front().k.hard || queue.front().droppedTruthHard;
					queue.pop_front();
					++purged;
				}
				if (purged > 0 && !queue.empty())
				{
					queue.front().queueDropBefore = true;
					queue.front().droppedTruthLost += lostTruth;
					queue.front().droppedTruthHard = queue.front().droppedTruthHard || hardTruth;
					if (resizeMarksGap)
					{
						queue.front().k.gapBefore = true;
						queue.front().k.gapFrames = 0;
					}
					++result.stats.resizePurges;
				}
				pendingReset = true;
				resetAt = now + cfg.profileResetDelay;
			}
			if (pendingReset && now >= resetAt)
			{
				pendingReset = false;
				ResetQueue("profile");
			}
		}

	public:
		// True: the resize purge marks the next frame's gap (the renderer with queue_timing_fixes).
		bool resizeMarksGap = false;
		Mutation mutation = Mutation::None;

	private:
		// The dequeue's decisions for frame A with B = the queue front (as the renderer).
		void Dequeue(QItem a, double now)
		{
			++result.stats.dequeued;
			// Order: generation then enqueue order, strictly increasing.
			if (lastDequeued && (a.generation < lastDequeued->generation ||
				(a.generation == lastDequeued->generation && a.sourceSequence <= lastDequeued->sourceSequence)))
				Fail("PRESENT_ORDER", now, Fmt("dequeued %llu after %llu", static_cast<unsigned long long>(a.sourceSequence),
					static_cast<unsigned long long>(lastDequeued->sourceSequence)));
			// Motion validity (the one vectors-valid state).
			const uint64_t epochBefore = mv.Epoch();
			if (a.k.gapBefore)
			{
				mv.Invalidate(MotionValidity::Cause::Gap);
				++result.stats.invalidations;
			}
			mv.NoteKept(a.sourceSequence, MotionValidity::Cause::DriftDrop);
			const bool invalidated = mv.Epoch() != epochBefore;
			// Every queue drop (overflow, backlog flush, resize purge) hands a gap to the frame after it.
			if (a.queueDropBefore && !a.k.gapBefore && lastDequeued && lastDequeued->generation == a.generation)
				Fail("QUEUE_DROP_UNMARKED", now, Fmt("frame %llu follows %lld frame(s) dropped from the queue but carries no gap",
					static_cast<unsigned long long>(a.sourceSequence), a.droppedTruthLost));
			const bool truthDiscontinuity = lastDequeued && (a.k.hard || a.k.lost > 0 || a.queueDropBefore ||
				a.droppedTruthHard || a.droppedTruthLost > 0 || a.generation != lastDequeued->generation);
			if (truthDiscontinuity && !invalidated && lastDequeued->generation == a.generation)
				Fail("VECTORS_NOT_INVALIDATED", now, Fmt("frame %llu follows a discontinuity (lost %lld, hard %d, queue drop %d) "
					"but the vectors stayed valid", static_cast<unsigned long long>(a.sourceSequence), a.k.lost + a.droppedTruthLost,
					(a.k.hard || a.droppedTruthHard) ? 1 : 0, a.queueDropBefore ? 1 : 0));
			// A pair made at the previous dequeue that ends at this frame must not span a discontinuity.
			if (pendingPair && pendingPairB == a.sourceSequence && lastDequeued && lastDequeued->generation == a.generation)
			{
				const bool hard = a.k.hard || a.droppedTruthHard;
				const long long missing = a.k.lost + a.droppedTruthLost;
				if (hard)
					Fail("PAIR_SPANS_HARD", now, Fmt("pair %llu->%llu spans a hard discontinuity (fill %d)",
						static_cast<unsigned long long>(lastDequeued->sourceSequence), static_cast<unsigned long long>(a.sourceSequence),
						pendingPairFill ? 1 : 0));
				else if (missing > 0 && !pendingPairFill)
					Fail("PAIR_SPANS_HOLE", now, Fmt("ordinary pair %llu->%llu spans %lld missing frame(s)",
						static_cast<unsigned long long>(lastDequeued->sourceSequence), static_cast<unsigned long long>(a.sourceSequence), missing));
			}
			pendingPair = false;
			// B and the pair (renderer: next = the queue front; gap fill; adjacency).
			if (!queue.empty() && queue.front().generation == a.generation)
			{
				const QItem& b = queue.front();
				const double pairRateHz = a.k.filmHz > 0.0 ? a.k.filmHz : a.k.signalHz;
				const double timeA = a.k.time, timeB = b.k.time;
				const int maxSpan = InterpolationTimeline::MaxPairSpanFrames(pairRateHz, displayHz, true);
				const bool spanOk = pairRateHz > 0.0 && timeA > 0.0 && timeB > timeA &&
					timeB - timeA <= (maxSpan + 0.25) / pairRateHz;
				bool fillGap = InterpolationTimeline::GapFillPair(b.k.gapBefore, sc.sw.fillGaps,
					mutation == Mutation::FillAcrossNoFill && b.k.gapFrames == 0 ? 1u : b.k.gapFrames,
					mutation == Mutation::FillIgnoresSpan ? true : spanOk, lowLatency,
					mutation == Mutation::FillIgnoresSpan ? 1000 : maxSpan);
				const bool adjacentOk = fillGap || CaptureTimestampGap::AdjacentPairTimes(timeA, a.k.filmHz > 0.0, timeB,
					b.k.filmHz > 0.0, pairRateHz);
				if ((!b.k.gapBefore || fillGap) && !b.k.filmUpdate && adjacentOk)
				{
					++result.stats.pairs;
					if (fillGap)
					{
						++result.stats.fillPairs;
						// The real span of the pair (truth: the hardware timestamps), in the pair's periods.
						const double realSpan = (b.k.captureTime - a.k.captureTime) * pairRateHz;
						if (!b.k.hard && !b.droppedTruthHard && realSpan > maxSpan + 0.5)
							Fail("FILL_PAIR_TOO_LONG", now, Fmt("gap pair spans %.2f periods in capture time, span %d", realSpan, maxSpan));
					}
					pendingPair = true;
					pendingPairB = b.sourceSequence;
					pendingPairFill = fillGap;
				}
				// The timeline (normal path): never restarted on a contiguous quiet stream is not
				// checked here; it only must not run backwards.
				const double rate = pairRateHz;
				if (!lowLatency)
				{
					timeline.SetSpanInTime(true);
					timeline.Plan(a.generation, a.sourceSequence, a.k.time, pendingPair ? timeB : 0.0, rate, displayHz, a.k.gapBefore);
				}
			}
			lastDequeued.reset(new QItem(a));
			lastRate = a.k.filmHz > 0.0 ? a.k.filmHz : a.k.signalHz;
			// Latency: shown about now (the next vblank); after the recovery time it must be steady.
			const double latency = now - a.k.enqueue;
			result.stats.maxLatency = (std::max)(result.stats.maxLatency, latency);
			latencySamples.push_back({ now, latency, SteadyLatencyBound(a) });
			lastDequeueAt = now;
		}

		// The steady latency: the target's frames, the stale-backlog margin above it, a frame
		// on screen, the display's refreshes and the render (derived from the rates).
		double SteadyLatencyBound(const QItem& a) const
		{
			const double rate = a.k.filmHz > 0.0 ? a.k.filmHz : a.k.signalHz;
			const double P = 1.0 / (std::max)(1.0, rate);
			const double Pd = 1.0 / displayHz;
			const double margin = AlphaQueuePolicy::StaleBacklogAgeThresholdMs(rate) / 1000.0;
			const double hold = a.k.filmHz > 0.0 && a.k.cadenceCycle > 0 ? (std::max)(0.0, a.k.time - a.k.arrival) : 0.0;
			const double render = RenderMs(a.k.enqueue) / 1000.0;
			const double raLead = sc.sw.renderAhead ? (RenderAhead::PlannedLead(P, Pd) +
				(RenderAhead::kQueuedFrameLatency + 1) * std::ceil(P / Pd) * Pd) : 0.0;
			return (Target() + 2) * P + margin + 3.0 * Pd + render + hold + raLead + 0.010;
		}

		void Fail(const char* invariant, double t, const std::string& detail)
		{
			if (failCount[invariant]++ < 3)
				result.failures.push_back({ invariant, t, detail });
		}

		// ---------------- render_ahead off: the vblank-paced loop ----------------
		void RunVblank(const std::vector<Kept>& kept)
		{
			size_t next = 0, nextDisplay = 0;
			double now = kept.front().enqueue;
			const double end = kept.back().enqueue + 3.0;
			double sourceClock = 0.0, nextFrameAt = 0.0;
			double busyUntil = now;
			lastDequeueAt = now;
			while (now < end)
			{
				while (nextDisplay < displays.size() && displays[nextDisplay].t <= now)
					displayHz = displays[nextDisplay++].hz;
				ApplyProfiles(now);
				while (next < kept.size() && kept[next].enqueue <= now)
				{
					const bool reset = kept[next].resync;
					Enqueue(kept[next++]);
					if (reset) { sourceClock = 0.0; nextFrameAt = 0.0; }
				}
				CheckFreeze(now, next < kept.size() ? kept[next].enqueue : 1e300);
				const double vblank = 1.0 / displayHz;
				const double stallEnd = StallEnd(now);
				if (stallEnd > now || now < busyUntil)
				{
					now = (std::max)(now + vblank, (std::max)(stallEnd, busyUntil));
					continue;
				}
				if (lowLatency)
				{
					// The low-latency path: the newest frame at each refresh (no timeline).
					if (AlphaQueuePolicy::CanDequeue(queue.size(), Target(), startup))
					{
						startup = false;
						QItem a = queue.front();
						queue.pop_front();
						Dequeue(a, now);
						busyUntil = now + RenderMs(now) / 1000.0;
					}
					else
						++result.stats.holds;
					now += vblank;
					continue;
				}
				bool held = false;
				int dequeues = 0;
				while (sourceClock + 1e-12 >= nextFrameAt && dequeues < 8)
				{
					if (!AlphaQueuePolicy::CanDequeue(queue.size(), GateTarget(), startup))
					{
						held = true;
						if (now - lastDequeueAt > 0.5) everEmptied = true;   // the StuckAfterStarve mutation's trigger
						break;
					}
					startup = false;
					QItem a = queue.front();
					queue.pop_front();
					++dequeues;
					const double rate = a.k.filmHz > 0.0 ? a.k.filmHz : a.k.signalHz;
					steering.Update(queue.size(), Target(), rate, sc.sw.queueFixes, lowLatency ? 0.0 : cfg.holdFraction);
					if (!a.k.filmUpdate)   // an update has no time of its own (no refresh)
						nextFrameAt += 1.0 / rate;
					Dequeue(a, now);
					const double renderMs = RenderMs(now);
					busyUntil = now + renderMs / 1000.0;
					Backlog(now, renderMs, a);
				}
				if (held)
				{
					if (!startup) ++result.stats.holds;
					now += vblank;
					continue;
				}
				if (!startup)
					sourceClock += vblank * steering.RateTrim();
				now += vblank;
			}
		}

		// The backlog check after a frame's render (renderer, line ~29600).
		void Backlog(double now, double renderCycleMs, const QItem& a)
		{
			const size_t recoveryTarget = Target();
			const double oldestMs = queue.empty() ? 0.0 : (now - queue.front().k.enqueue) * 1000.0;
			const double signalRate = a.k.signalHz;
			const double queueRate = sc.sw.queueFixes ? AlphaQueuePolicy::QueueFrameRateHz(
				queue.empty() ? 0.0 : queue.front().k.filmHz, signalRate) : (a.k.filmHz > 0.0 ? a.k.filmHz : signalRate);
			if (mutation != Mutation::NoBacklogFlush &&
				AlphaQueuePolicy::ShouldRecoverBacklog(queue.size(), recoveryTarget, oldestMs, renderCycleMs, queueRate, sc.sw.queueFixes))
			{
				size_t dropped = 0;
				long long lostTruth = 0;
				bool hardTruth = false;
				while (queue.size() > recoveryTarget)
				{
					lostTruth += queue.front().k.lost + queue.front().droppedTruthLost + 1;
					hardTruth = hardTruth || queue.front().k.hard || queue.front().droppedTruthHard;
					queue.pop_front();
					++dropped;
				}
				if (dropped > 0 && !queue.empty())
				{
					queue.front().k.gapBefore = true;
					queue.front().k.gapFrames = 0;
					queue.front().queueDropBefore = true;
					queue.front().droppedTruthLost += lostTruth;
					queue.front().droppedTruthHard = queue.front().droppedTruthHard || hardTruth;
					steering.Reset();
					raFlushReanchor = true;
				}
				++result.stats.flushes;
			}
		}

		// No permanent hold: while frames keep arriving and the queue holds its target, a
		// frame is presented within the target's frames, a stall and a second of slack.
		void CheckFreeze(double now, double nextArrival)
		{
			(void)nextArrival;
			if (queue.size() >= (std::max<size_t>)(Target(), 1) + 1 && !startup)
			{
				// Since the queue could dequeue (above its target) and nothing was presented.
				const double since = (std::max)(lastDequeueAt, readySince);
				double stalled = 0.0;
				for (const WindowEvent& w : stalls)
					if (w.t1 > since && w.t0 < now) stalled += (std::min)(w.t1, now) - (std::max)(w.t0, since);
				double spike = 0.0;
				for (const WindowEvent& w : spikes)
					if (w.t1 > since && w.t0 < now) spike = (std::max)(spike, w.value / 1000.0);
				const double bound = (Target() + 2) / (std::max)(1.0, CurrentRate()) + stalled + spike + 1.0;
				if (now - since > bound)
				{
					Fail("PRESENT_FREEZE", now, Fmt("no frame presented for %.3f s with %zu queued (target %zu)",
						now - since, queue.size(), Target()));
					lastDequeueAt = now;
				}
			}
			else
				readySince = now;
		}

		// ---------------- render_ahead on: the FIFO back-pressure model ----------------
		struct Grid
		{
			double v0 = 0.0, pd = 1.0 / 60.0;
			double At(double t) const { return v0 + std::ceil((t - v0) / pd - 1e-9) * pd; }
		};

		double VsyncAtOrAfter(double t)
		{
			// Display rate changes move the grid (the first vsync after the change).
			while (nextGridChange < displays.size() && displays[nextGridChange].t <= t)
			{
				const double first = grid.At(displays[nextGridChange].t);
				grid.v0 = first;
				grid.pd = 1.0 / displays[nextGridChange].hz;
				displayHz = displays[nextGridChange].hz;
				++nextGridChange;
			}
			double v = grid.At(t);
			for (const WindowEvent& w : stalls)
				if (v >= w.t0 && v < w.t1) v = grid.At(w.t1);
			return v;
		}

		double Present(double ready, int interval)
		{
			double earliest = ready;
			if (!shown.empty())
				earliest = (std::max)(earliest, shown.back() + (std::max)(1, interval) * grid.pd - 1e-9);
			const double v = VsyncAtOrAfter(earliest);
			if (!shown.empty() && v <= shown.back())
				Fail("RA_PRESENT_ORDER", v, Fmt("present shown at %.6f not after %.6f", v, shown.back()));
			shown.push_back(v);
			return v;
		}

		double StartAt(double gpuFree, int queueDepth) const
		{
			double start = gpuFree;
			const size_t k = shown.size();
			if (k >= static_cast<size_t>(queueDepth))
				start = (std::max)(start, shown[k - static_cast<size_t>(queueDepth)]);
			return start;
		}

		void RunRenderAhead(const std::vector<Kept>& kept)
		{
			grid.v0 = 1.25 + 0.0013;
			grid.pd = 1.0 / displayHz;
			size_t next = 0;
			double gpuFree = kept.front().enqueue;
			lastDequeueAt = gpuFree;
			RenderAhead::Schedule schedule;
			RenderAhead::Gate gate;
			RenderAhead::CostWindow newFrameCost;
			RenderAhead::KnownCost extraCost{ 4 }, estimateCost{ 1 };
			RenderAhead::SustainedLateness drift;
			RenderAhead::GateEpoch epoch;
			RenderAhead::CreditLatch creditLatch;
			bool openLoopLate = false;
			int mainInterval = 1;
			uint32_t presentIds = 0;
			const double end = kept.back().enqueue + 3.0;
			double lastSpikeEnd = -1e9, lastLateAt = -1e9;
			for (const WindowEvent& w : spikes) lastSpikeEnd = (std::max)(lastSpikeEnd, w.t1);
			for (const WindowEvent& w : stalls) lastSpikeEnd = (std::max)(lastSpikeEnd, w.t1);
			double t = gpuFree;
			int guard = 0;
			while (t < end && ++guard < 50000000)
			{
				const int queueDepth = static_cast<int>(chainLatency);
				t = (std::max)(t, StartAt(gpuFree, queueDepth));
				ApplyProfiles(t);
				if (lowLatency)
				{
					// The low-latency path keeps today's chain: one frame per refresh.
					while (next < kept.size() && kept[next].enqueue <= t) Enqueue(kept[next++]);
					if (AlphaQueuePolicy::CanDequeue(queue.size(), Target(), startup))
					{
						startup = false;
						QItem a = queue.front();
						queue.pop_front();
						Dequeue(a, t);
						const double done = t + RenderMs(t) / 1000.0;
						Present(done, 1);
						gpuFree = done;
						t = done;
					}
					else
						t = next < kept.size() ? (std::max)(t + 1e-4, kept[next].enqueue) : end;
					schedule.Break();
					continue;
				}
				while (next < kept.size() && kept[next].enqueue <= t)
				{
					const bool reset = kept[next].resync;
					Enqueue(kept[next++]);
					if (reset) schedule.Break();
				}
				CheckFreeze(t, next < kept.size() ? kept[next].enqueue : 1e300);
				if (!AlphaQueuePolicy::CanDequeue(queue.size(), Target(), startup))
				{
					++result.stats.holds;
					if (next >= kept.size()) break;
					t = (std::max)(t + 1e-4, kept[next].enqueue);
					continue;
				}
				startup = false;
				QItem a = queue.front();
				queue.pop_front();
				const double rate = a.k.filmHz > 0.0 ? a.k.filmHz : a.k.signalHz;
				const double Ps = 1.0 / rate, Pd = grid.pd;
				const double ratio = DejudderPolicy::CadenceRatio(rate, 1.0 / Pd);
				const int maxExtras = DejudderPolicy::MaxExtras(ratio);
				// GateEpoch: a new generation or cadence starts the gate, its costs and the credit.
				if (epoch.Changed(a.generation, ratio))
				{
					gate.Reset();
					newFrameCost.Reset();
					extraCost.Reset();
					estimateCost.Reset();
					drift.Reset();
					creditLatch.Reset();
					credit = 0;
					schedule.Break();
					chainLatency = RenderAhead::ChainDepthFor(true, false, ratio).latency;
				}
				Dequeue(a, t);
				// A: its whole render, then its present.
				const double start = t;
				const double nMs = RenderMs(start);
				const double done = start + nMs / 1000.0;
				newFrameCost.Add(done, nMs);
				gpuFree = done;
				const size_t idA = shown.size();
				const double now = done;
				double gridVsync = 0.0;
				bool late = false, closedLoop = sc.sw.closedLoop, openDrift = false;
				if (raFlushReanchor)
				{
					raFlushReanchor = false;
					schedule.Break();
					flushReanchorNow = true;
				}
				if (closedLoop)
				{
					size_t latest = idA;
					for (size_t j = idA; j-- > 0;)
						if (shown[j] <= now) { latest = j; break; }
					if (latest < idA)
					{
						gridVsync = shown[latest];
						late = schedule.Observe(static_cast<uint32_t>(latest + 1), shown[latest], Pd);
					}
				}
				else
				{
					gridVsync = grid.At(now) - Pd;   // DWM's vblank at this rate
					late = openLoopLate;
					openDrift = openLoopLate;
				}
				openLoopLate = false;
				bool restart = false, behind = false;
				const double maxAhead = RenderAhead::PlannedLead(Ps, Pd) +
					(static_cast<int>(chainLatency) + 1) * std::ceil(ratio - RenderAhead::kTolerance) * Pd;
				const double firstVsync = schedule.FirstVsync(now, Pd, gridVsync, restart, behind, maxAhead);
				if (restart) schedule.Break();
				const double lead = RenderAhead::PlannedLead(Ps, Pd);
				if (!restart && schedule.AnchorTooLate(firstVsync, a.k.time, lead, Ps))
				{
					schedule.Break();
					restart = true;
					++result.stats.reanchors;
				}
				late = late || behind;
				const double L = schedule.AnchorLatency(restart, firstVsync, a.k.time, flushReanchorNow && restart ? lead : 0.0);
				flushReanchorNow = false;
				int drops = schedule.TakeLateDrops() + (behind ? 1 : 0);
				// The gate (period mode).
				newFrameCost.Trim(now);
				extraCost.Trim(now);
				estimateCost.Trim(now);
				RenderAhead::GateInput g;
				g.sourcePeriod = Ps;
				g.displayPeriod = Pd;
				g.queueDepth = static_cast<int>(chainLatency);
				g.costs.newFrameMs = newFrameCost.P95();
				g.costs.estimateMs = estimateCost.Current();
				const RenderAhead::GateCosts ex = RenderAhead::ExtraCosts(extraCost.Current(), 0.0, 0.0, false);
				g.costs.extraMs = ex.extraMs;
				const bool degraded = gate.Level() > 0;
				const bool measured = newFrameCost.Count() >= 10 && (maxExtras == 0 || extraCost.Measured() || degraded);
				const bool openBehind = behind && !closedLoop;
				const bool gateLate = (behind && !openBehind) || (late && !openDrift && !openBehind) ||
					((openDrift || openBehind) && drift.Note(now));
				if (gateLate) lastLateAt = now;
				if (measured || gateLate)
				{
					if (!measured) g.costs = RenderAhead::GateCosts();
					const RenderAhead::GateChange change = gate.Update(now, g, gateLate);
					if (change.changed && change.to > change.from) ++result.stats.gateDowns;
				}
				const int level = gate.Level();
				result.stats.maxGateLevel = (std::max)(result.stats.maxGateLevel, level);
				drops += level >= RenderAhead::HoldLevel(maxExtras) ? 16 : RenderAhead::DropsAt(level);
				levelSamples.push_back({ now, level, lastSpikeEnd, lastLateAt });
				RenderAhead::FrameInput in;
				in.filmTime = a.k.time;
				in.sourcePeriod = Ps;
				in.displayPeriod = Pd;
				in.firstVsync = firstVsync;
				in.latencyFloor = L;
				in.lead = lead;
				in.strength = 1.0;
				in.evenCycle = DejudderPolicy::EvenCycle(ratio);
				in.maxExtras = maxExtras;
				in.dropExtras = drops;
				in.arrivalJitter = schedule.NoteArrival(a.k.time, Ps);
				const RenderAhead::FramePlan plan = RenderAhead::PlanFrame(in);
				++result.stats.raFrames;
				if (!plan.valid)
				{
					Fail("RA_NO_PLAN", now, "render-ahead: no plan");
					Present(done, mainInterval);
					schedule.Break();
					mainInterval = 1;
					t = done;
					continue;
				}
				// Budget: extras with a shift never exceed what the gate's level allows.
				int shifted = 0;
				for (const RenderAhead::Slot& s : plan.slots)
					if (s.render && s.shift > 0.0f) ++shifted;
				const int allowed = level >= RenderAhead::HoldLevel(maxExtras) ? 0 : RenderAhead::MaxExtrasPerFrame(ratio, RenderAhead::DropsAt(level));
				if (!restart && shifted > allowed + 0 && shifted > maxExtras)
					Fail("RA_OVER_BUDGET", now, Fmt("%d extras rendered, level %d allows %d (max %d)", shifted, level, allowed, maxExtras));
				if (!restart && !RenderAhead::IntervalsFit(plan))
					Fail("RA_INTERVAL", now, Fmt("a chained plan needs an interval above %d", RenderAhead::kMaxSyncInterval));
				// Present A, then its extras.
				std::vector<std::pair<uint32_t, double>> ids;
				const double shownA = Present(done, mainInterval);
				ids.emplace_back(++presentIds, firstVsync);
				// Latency of A on screen (film time to shown).
				raLatency.push_back({ shownA, shownA - a.k.enqueue, SteadyLatencyBound(a) });
				bool first = true;
				int lateBy = 0;
				for (const RenderAhead::Slot& s : plan.slots)
				{
					if (!s.render) continue;
					const double st = StartAt(gpuFree, static_cast<int>(chainLatency));
					const double xMs = 0.25 * nMs;
					const double ready = st + xMs / 1000.0;
					extraCost.Add(ready, xMs);
					gpuFree = ready;
					ids.emplace_back(++presentIds, s.vsync);
					Present(ready, s.interval);
					++result.stats.raExtras;
					// Open loop: a present returning more than a refresh after its planned vsync.
					const double returned = StartAt(ready, static_cast<int>(chainLatency));
					lateBy = (std::max)(lateBy, OpenLoopDisplayRate::LateRefreshes(returned, s.vsync, Pd));
					if (first) { gpuFree += 0.1 * nMs / 1000.0; estimateCost.Add(gpuFree, 0.1 * nMs); first = false; }
				}
				schedule.Commit(plan.nextFirstVsync, ids);
				if (!closedLoop && lateBy > 0)
					openLoopLate = schedule.NoteLate(lateBy, Pd);
				mainInterval = plan.nextInterval;
				credit = creditLatch.Commit(lead, Ps, ratio);
				result.stats.maxPending = (std::max)(result.stats.maxPending, schedule.Pending());
				result.stats.maxCostWindow = (std::max)(result.stats.maxCostWindow, newFrameCost.Count());
				if (schedule.Pending() > 64)
					Fail("RA_PENDING_UNBOUNDED", now, Fmt("%zu pending presents", schedule.Pending()));
				if (newFrameCost.Count() > RenderAhead::CostWindow::kCapacity)
					Fail("RA_COSTWINDOW_UNBOUNDED", now, Fmt("%zu cost samples", newFrameCost.Count()));
				// The backlog check after the main render.
				Backlog(gpuFree, nMs, a);
				t = gpuFree;
			}
			if (shown.size() > 2000000) shown.erase(shown.begin(), shown.end() - 64);
		}

		// After every run: latency steady outside the fault windows; the gate back at full.
		void Finish()
		{
			// Fault windows (QPC): every event, its duration and the recovery time.
			std::vector<std::pair<double, double>> faults;
			for (const Event& e : sc.events)
			{
				const double t = 1.25 + e.t;
				double dur = 0.0;
				if (e.kind == Ev::Starve || e.kind == Ev::Jitter || e.kind == Ev::Spike) dur = e.a;
				if (e.kind == Ev::Stall) dur = e.a / 1000.0;
				if (e.kind == Ev::Burst) dur = e.a / sc.signalHz;
				if (e.kind == Ev::Drift) dur = 1e9;   // a drifted signal is steady once settled; keep it out of the latency check
				// Recovery: the startup prefill and the target refill (a few seconds at any rate), the
				// profile reset delay, and a re-lock (FramesToLock at 3 cycles or 2 s).
				const double recovery = 4.0 + cfg.profileResetDelay + 3.0;
				faults.push_back({ t - 0.05, t + dur + recovery });
			}
			auto inFault = [&](double t) {
				for (const auto& f : faults)
					if (t >= f.first && t <= f.second) return true;
				return false;
			};
			const double start = latencySamples.empty() ? 0.0 : latencySamples.front().t + 8.0;
			int reported = 0;
			for (const auto& s : (sc.sw.renderAhead && !raLatency.empty() ? raLatency : latencySamples))
			{
				if (s.t < start || inFault(s.t)) continue;
				result.stats.maxSteadyLatency = (std::max)(result.stats.maxSteadyLatency, s.latency);
				if (s.latency > s.bound && reported++ < 1)
					Fail(sc.sw.renderAhead ? "RA_LATENCY_NOT_STEADY" : "LATENCY_NOT_STEADY", s.t,
						Fmt("latency %.1f ms > steady bound %.1f ms outside any fault", s.latency * 1000.0, s.bound * 1000.0));
			}
			// The gate recovers: back to full within T after the last spike / stall / late frame ends,
			// T = the cost window, the known-cost hold, the up-after-down wait and a step per level.
			if (!levelSamples.empty())
			{
				const double T = RenderAhead::CostWindow::kWindowSeconds + RenderAhead::KnownCost::kHoldSeconds +
					RenderAhead::Gate::kUpAfterDownSeconds + 6 * RenderAhead::Gate::kUpSeconds + 10.0;
				const auto& last = levelSamples.back();
				double quietFrom = (std::max)(last.spikeEnd, last.lateAt);
				for (const auto& f : faults) quietFrom = (std::max)(quietFrom, (std::min)(f.second, last.t));
				if (last.level > 0 && last.t - quietFrom > T)
					Fail("RA_GATE_STUCK", last.t, Fmt("gate level %d %.1f s after the last fault (bound %.1f s)", last.level,
						last.t - quietFrom, T));
			}
		}

		struct LatencySample { double t, latency, bound; };
		struct LevelSample { double t; int level; double spikeEnd, lateAt; };

		const Scenario& sc;
		Result& result;
		PresentConfig cfg;
		std::deque<QItem> queue;
		size_t capacity = 32, target = 5, credit = 0;
		bool lowLatency = false, startup = true, pendingReset = false, everEmptied = false;
		double resetAt = 0.0;
		uint64_t generation = 1, sourceSequence = 0;
		int cadence = 0;
		double displayHz = 60.0, lastRate = 0.0, lastDequeueAt = 0.0, readySince = 0.0;
		AlphaQueuePolicy::TimelineQueueSteering steering;
		MotionValidity::State mv;
		InterpolationTimeline::Timeline timeline;
		bool haveTimeline = false;
		std::unique_ptr<QItem> lastDequeued;
		bool pendingPair = false, pendingPairFill = false;
		uint64_t pendingPairB = 0;
		std::vector<WindowEvent> spikes, stalls, profiles;
		std::vector<DisplayEvent> displays;
		size_t nextProfile = 0, nextGridChange = 0;
		std::vector<LatencySample> latencySamples, raLatency;
		std::vector<LevelSample> levelSamples;
		std::map<std::string, int> failCount;
		// render-ahead
		Grid grid;
		std::vector<double> shown;
		unsigned chainLatency = 2;
		bool raFlushReanchor = false, flushReanchorNow = false;
		uint64_t raEpochGeneration = 0;
	};

	// ---- one scenario through the lane and the presentation ----
	struct RunOptions
	{
		bool resizeMarksGap = false;   // the queue-resize purge fix (see docs/agent-reports/queue-stress.md)
		Mutation mutation = Mutation::None;
		bool legacyQ1 = false;         // the renderer before the Q1 fix (docs/agent-reports/q1-stale-burst.md)
		PresentConfig present;         // queue capacity/target (the delay gate and the combination runner set it)
	};

	inline Result Run(const Scenario& s, const RunOptions& o = RunOptions())
	{
		Result r;
		Source source(s);
		const std::vector<SignalFrame> frames = source.Generate();
		Lane lane(s, r);
		lane.mutation = o.mutation;
		lane.legacyQ1 = o.legacyQ1;
		// Arrival disturbances (jitter, bursts, drift): the film clock may follow the arrivals.
		std::vector<std::pair<double, double>> arrivalWindows;
		std::vector<Event> ordered = s.events;
		std::sort(ordered.begin(), ordered.end(), [](const Event& x, const Event& y) { return x.t < y.t; });
		double rateNow = s.signalHz;   // the signal rate at each event (a burst holds frames at it)
		for (const Event& e : ordered)
		{
			const double t = 1.25 + e.t;
			if (e.kind == Ev::RateSwing) rateNow = e.a;
			if (e.kind == Ev::Jitter) arrivalWindows.push_back({ t, t + e.a });
			if (e.kind == Ev::Burst) arrivalWindows.push_back({ t, t + e.a / rateNow + 0.1 });
			if (e.kind == Ev::Drift) arrivalWindows.push_back({ t, t + 1e9 });
		}
		for (const SignalFrame& f : frames)
		{
			bool inWindow = false;
			for (const auto& w : arrivalWindows)
				if (f.callback >= w.first && f.callback <= w.second) inWindow = true;
			if (inWindow) lane.NoteArrivalDisturbance(f.callback);
			lane.jitterNow = inWindow;
			lane.OnSignal(f);
		}
		Presentation p(s, r, o.present);
		// The resize purge marks the next frame's gap with queue_timing_fixes (fixed 2026-10-18,
		// LibplaceboVideoRenderer::SetFrameQueueMaxSize); off, the renderer before.
		p.resizeMarksGap = o.resizeMarksGap || s.sw.queueFixes;
		p.mutation = o.mutation;
		p.Run(lane.kept);
		// Cadence re-lock: film content in a quiet stretch at the end must be locked.
		if (s.pictureHz < s.signalHz * 0.9)
		{
			double lastFault = 0.0;
			for (const Event& e : s.events)
				lastFault = (std::max)(lastFault, e.t + (e.kind == Ev::Starve || e.kind == Ev::Jitter ? e.a : 0.0));
			bool swung = false;
			for (const Event& e : s.events)
				if (e.kind == Ev::RateSwing || e.kind == Ev::Drift) swung = true;   // the picture rate may become video
			if (!swung && s.seconds - lastFault > 20.0 && !lane.Locked())
				r.failures.push_back({ "CADENCE_NO_RELOCK", 1.0 + s.seconds,
					Fmt("film %.3f in %.3f not locked %.1f s after the last fault", s.pictureHz, s.signalHz, s.seconds - lastFault) });
		}
		return r;
	}

	// ---- minimisation: the shortest event list (and duration) that keeps the signature ----
	inline Scenario Minimise(const Scenario& failing, const std::string& signature,
		const std::function<Result(const Scenario&)>& run, int* runs = nullptr)
	{
		Scenario best = failing;
		int count = 0;
		auto fails = [&](const Scenario& s) { ++count; return run(s).Failed(signature); };
		bool changed = true;
		while (changed && count < 400)
		{
			changed = false;
			for (size_t i = 0; i < best.events.size() && count < 400; ++i)
			{
				Scenario trial = best;
				trial.events.erase(trial.events.begin() + static_cast<std::ptrdiff_t>(i));
				if (fails(trial)) { best = trial; changed = true; --i; }
			}
		}
		// Shorter: end 60 s after the last event (or halve) while it still fails.
		double lastEvent = 0.0;
		for (const Event& e : best.events) lastEvent = (std::max)(lastEvent, e.t);
		for (double seconds : { lastEvent + 60.0, lastEvent + 120.0 })
		{
			if (seconds >= best.seconds) continue;
			Scenario trial = best;
			trial.seconds = std::ceil(seconds);
			if (fails(trial)) { best = trial; break; }
		}
		// Simpler switches.
		Scenario trial = best;
		trial.sw.closedLoop = true; trial.sw.fillGaps = true; trial.sw.lookaheadAuto = true; trial.sw.queueFixes = true;
		if (fails(trial)) best = trial;
		if (runs) *runs = count;
		best.name = "minimal";
		return best;
	}
}
