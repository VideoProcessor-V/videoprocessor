#include "pch.h"
#include "CppUnitTest.h"

// Queue stress harness, the lanes with state of their own (2026-10-18): the LED drive
// module with look-ahead on the manual LedDrive clock behind a simulated serial port,
// the content-intent observer fed by the harness's kept-frame stream, and the
// source-resolution monitor under the same queue faults. See QueueStressHarness.h.

#include "LedDriveTestSupport.h"
#include "QueueStressHarness.h"

#include <ContentIntentObserver.h>
#include <led/LedMeasurement.h>
#include <vprenderer/SourceResolutionMonitor.h>
#include <SourceResolutionDetector.h>

#include <chrono>
#include <map>
#include <thread>
#include <tuple>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace QueueStress;

namespace
{
	void LaneLog(const std::string& text) { Logger::WriteMessage((text + "\n").c_str()); }
	std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }

	int EnvCount(const char* name, int fallback)
	{
		char* value = nullptr;
		size_t length = 0;
		int result = fallback;
		if (_dupenv_s(&value, &length, name) == 0 && value) { result = std::atoi(value); free(value); }
		return result > 0 ? result : fallback;
	}

	// ---------------- the LED lane ----------------
	// One frame: most blocks at `code`, `bright` blocks at `highlight` (LedDriveCompensateTests).
	LedDrive::RawMeasurement LedFrame(double code, uint32_t bright, int highlight, uint32_t blocks = 10000)
	{
		LedDrive::RawMeasurement raw{};
		raw.histogram[static_cast<int>(code)] += blocks - bright;
		raw.histogram[highlight] += bright;
		raw.blocks = blocks;
		raw.sum = static_cast<uint32_t>((code * (blocks - bright) + highlight * bright) * LedDrive::SUM_SCALE);
		for (int i = 0; i < LedDrive::GRID_W * LedDrive::GRID_H; ++i)
		{
			raw.cellCount[i] = 4;
			raw.cellSum[i] = static_cast<uint32_t>(4 * code * LedDrive::SUM_SCALE);
		}
		return raw;
	}

	struct LedScene { double seconds; double code; uint32_t bright; int highlight; };

	// The frames as the render loop shows them to the module: sequence, display time and the
	// queue's frames ahead (the look-ahead depth), with the queue faults applied.
	struct LedFrameEvent { uint64_t sequence; double ms; size_t ahead; double contentMs; };

	struct LedScenario
	{
		uint64_t seed = 0;
		double fps = 24.0;
		double seconds = 60.0;
		DynamicDrive drive = DynamicDrive::Compensate;
		bool lookahead = true;
		std::vector<LedScene> scenes;
		std::vector<Event> events;   // Starve, Burst, Restart, Stall, Profile
	};

	LedScenario RandomLed(uint64_t seed, double seconds)
	{
		Rng r(seed * 7919 + 13);
		LedScenario s;
		s.seed = seed;
		s.seconds = seconds;
		const double rates[] = { 24.0, 24000.0 / 1001.0, 25.0, 30.0, 50.0, 60.0 };
		s.fps = rates[r.I(0, 5)];
		const int drive = r.I(0, 9);
		s.drive = drive < 6 ? DynamicDrive::Compensate : drive < 9 ? DynamicDrive::Script : DynamicDrive::None;
		s.lookahead = r.P(0.7);
		double t = 0.0;
		while (t < seconds)
		{
			LedScene sc;
			sc.seconds = r.U(1.0, 12.0);
			switch (r.I(0, 4))
			{
			case 0: sc.code = 18.0; sc.bright = 100; sc.highlight = 230; break;   // dark, small highlight
			case 1: sc.code = 120.0; sc.bright = 100; sc.highlight = 235; break;  // bright
			case 2: sc.code = 16.0; sc.bright = 0; sc.highlight = 16; break;      // black
			case 3: sc.code = 60.0; sc.bright = 400; sc.highlight = 220; break;   // mid, larger highlight
			default: sc.code = 30.0; sc.bright = 0; sc.highlight = 30; break;     // dim, no highlight
			}
			s.scenes.push_back(sc);
			t += sc.seconds;
		}
		const int n = r.I(1, 6);
		for (int i = 0; i < n; ++i)
		{
			Event e;
			const int k = r.I(0, 4);
			e.kind = k == 0 ? Ev::Starve : k == 1 ? Ev::Burst : k == 2 ? Ev::Restart : k == 3 ? Ev::Stall : Ev::Profile;
			e.t = r.U(3.0, seconds - 8.0);
			e.a = e.kind == Ev::Starve ? (r.P(0.5) ? r.U(0.1, 1.0) : r.U(1.0, 10.0)) : e.kind == Ev::Burst ? r.I(2, 30) :
				e.kind == Ev::Stall ? r.U(20.0, 600.0) : e.kind == Ev::Profile ? (r.P(0.5) ? 1.0 : 0.0) : 0.0;
			s.events.push_back(e);
		}
		std::sort(s.events.begin(), s.events.end(), [](const Event& a, const Event& b) { return a.t < b.t; });
		return s;
	}

	std::vector<LedFrameEvent> LedSchedule(const LedScenario& s)
	{
		std::vector<LedFrameEvent> out;
		const double period = 1000.0 / s.fps;
		const size_t normalAhead = static_cast<size_t>(std::ceil(0.5 * s.fps));   // a normal queue's frames ahead
		size_t ahead = s.lookahead ? normalAhead : 0;
		uint64_t sequence = 1;
		double shift = 0.0;   // display delay (ms) from stalls / starvation
		size_t next = 0;
		int burstLeft = 0;
		std::vector<LedFrameEvent> held;
		for (double t = 0.0; t < s.seconds * 1000.0; t += period)
		{
			while (next < s.events.size() && s.events[next].t * 1000.0 <= t)
			{
				const Event& e = s.events[next++];
				if (e.kind == Ev::Starve) { sequence += static_cast<uint64_t>(e.a * s.fps); t += e.a * 1000.0; }
				if (e.kind == Ev::Burst) burstLeft = static_cast<int>(e.a);
				if (e.kind == Ev::Restart) sequence += 1000;   // a new epoch: the old sequences are gone
				if (e.kind == Ev::Stall) shift += e.a;
				if (e.kind == Ev::Profile) ahead = e.a > 0.5 || !s.lookahead ? 0 : normalAhead;
			}
			LedFrameEvent f{ sequence++, t + shift, ahead, t };
			if (burstLeft > 0)
			{
				--burstLeft;
				held.push_back(f);
				if (burstLeft == 0)
				{
					for (LedFrameEvent& h : held) { h.ms = f.ms; out.push_back(h); }
					held.clear();
				}
				continue;
			}
			out.push_back(f);
		}
		return out;
	}

	const LedScene& SceneAt(const LedScenario& s, double seconds)
	{
		double t = 0.0;
		for (const LedScene& sc : s.scenes)
		{
			if (seconds < t + sc.seconds) return sc;
			t += sc.seconds;
		}
		return s.scenes.back();
	}

	struct LedRun
	{
		bool started = false, settled = true;
		std::vector<Failure> failures;
		int lightWrites = 0, contrastWrites = 0, frames = 0;
		double wallSeconds = 0.0;
	};

	LedRun RunLed(const LedScenario& s)
	{
		const auto wallStart = std::chrono::steady_clock::now();
		LedRun run;
		auto projector = std::make_shared<FakeProjector>();
		projector->contrast = 50;
		LedDrive::LedDriveConfig config;
		config.enabled = true;
		config.mode = s.drive == DynamicDrive::Compensate ? LedDrive::Mode::Compensate :
			s.drive == DynamicDrive::Script ? LedDrive::Mode::Script : LedDrive::Mode::PictureOnly;
		config.lookahead = s.lookahead;
		config.scriptLookahead = s.lookahead;
		config.cntrAssist = true;
		LedDrive::ProjectorConfig port;   // the owner's defaults: latency 80 ms, settle 300 ms, gap 200 ms
		port.protocol = "barco_ascii";
		port.terminator = LedDrive::Terminator::Cr;
		TestClock clock;
		LedDrive::LedDriveModule module(port, config, [projector] { return std::make_unique<FakePort>(projector); }, clock);
		Bind(clock, module, projector);
		module.Start();
		if (s.drive != DynamicDrive::None && !clock.RunUntil([&] { return module.DrivingProjector(); }, 5000))
		{
			run.failures.push_back({ "LED_NOT_STARTED", 0.0, "the module never drove the projector" });
			module.Stop();
			return run;
		}
		run.started = true;
		const double t0 = clock.Ms() + 10.0;
		const std::vector<LedFrameEvent> frames = LedSchedule(s);
		const double period = 1000.0 / s.fps;
		LedDrive::StatsBuilder builder(module.Config());
		uint64_t predictedUpTo = 0;
		for (const LedFrameEvent& f : frames)
		{
			const LedScene& sc = SceneAt(s, (std::min)(f.contentMs / 1000.0, s.seconds - 0.001));
			const double t = t0 + f.ms;
			if (t > clock.Ms()) clock.To(t);
			if (f.ahead > 0)
			{
				// The queued frames ahead, each seen once (predictions), then the walk's depth.
				for (uint64_t q = (std::max)(predictedUpTo + 1, f.sequence); q <= f.sequence + f.ahead; ++q)
				{
					const double qs = (f.contentMs + static_cast<double>(q - f.sequence) * period) / 1000.0;
					const LedScene& qsc = SceneAt(s, (std::min)(qs, s.seconds - 0.001));
					module.SubmitPrediction(q, builder.Build(LedFrame(qsc.code, qsc.bright, qsc.highlight), t + (q - f.sequence) * period));
				}
				predictedUpTo = (std::max)(predictedUpTo, f.sequence + f.ahead);
				module.SetLookaheadDepth(f.ahead);
			}
			else
				module.SetLookaheadDepth(0);
			module.SubmitMeasurement(LedFrame(sc.code, sc.bright, sc.highlight), f.sequence);
			module.BeginFrameAt(f.sequence, t, s.fps);
			if (!clock.Settle())
			{
				run.settled = false;
				run.failures.push_back({ "LED_NOT_SETTLED", t, "the module's thread did not park on the clock" });
				break;
			}
			++run.frames;
		}
		// The tail: 3 s without frames (the port drains, idle rules may act).
		clock.Run(3000.0, 20.0);
		// The projector's writes, replayed with their times.
		const auto writes = projector->Writes();
		const auto times = projector->WriteTimes();
		int light = 255, contrast = 50;
		double lightSince = 0.0, contrastAboveSince = -1.0;
		double lastLightRiseMs = -1e9;
		bool lastLightRiseJump = false;
		const bool trace = EnvCount("VP_QSTRESS_LED_TRACE", 0) == static_cast<int>(s.seed);
		if (trace)
		{
			double st = 0.0;
			for (const LedScene& sc : s.scenes)
			{
				LaneLog(Fmt("scene at %.0f ms (+t0 %.0f): code %.0f bright %u highlight %d", st * 1000.0, t0, sc.code, sc.bright, sc.highlight));
				st += sc.seconds;
			}
		}
		for (size_t i = 0; i < writes.size(); ++i)
		{
			const std::string& w = writes[i];
			const double ms = std::chrono::duration<double, std::milli>(times[i] - clock.origin).count();
			if (trace) LaneLog(Fmt("  %.0f ms: %s", ms, w.substr(0, w.size() - 1).c_str()));
			if (w.find('?') != std::string::npos || w.size() < 7) continue;
			const bool isLight = w.rfind(":LPW1", 0) == 0, isContrast = w.rfind(":CNTR", 0) == 0;
			if (!isLight && !isContrast) continue;
			const int value = std::atoi(w.c_str() + 5);
			// CNTR above need: with the LED at its ceiling the need is 1, VP's gain covers it, so CNTR
			// must be 50 (compensate; the script's CNTR is its intent). Checked at every write:
			// the LED at the ceiling and CNTR above 50 together for longer than latency + settle +
			// a command gap + a tick.
			if (s.drive == DynamicDrive::Compensate && light >= config.ledCeiling && contrast > 50 && contrastAboveSince >= 0.0)
			{
				const double both = ms - (std::max)(lightSince, contrastAboveSince);
				const double allowed = port.ledLatencyMs + port.ledSettleMs + 2.0 * port.minCommandGapMs + 250.0;
				if (both > allowed)
					run.failures.push_back({ "LED_CNTR_ABOVE_NEED", ms, Fmt("CNTR %d with the LED at %d for %.0f ms (allowed %.0f)",
						contrast, light, both, allowed) });
			}
			if (isLight)
			{
				++run.lightWrites;
				if (value < (std::min)(config.ledFloorBlack, config.ledFloor) || value > config.ledCeiling)
					run.failures.push_back({ "LED_LIGHT_RANGE", ms, Fmt("LPW1 %d outside [%d, %d]", value, config.ledFloorBlack, config.ledCeiling) });
				if (value > light) { lastLightRiseMs = ms; lastLightRiseJump = value - light >= 40; }
				if (value != light) lightSince = ms;
				light = value;
			}
			else
			{
				++run.contrastWrites;
				const int hi = s.drive == DynamicDrive::Compensate ? config.cntrAssistMax : 100;
				const int lo = s.drive == DynamicDrive::Compensate ? 50 : 0;
				if (value < lo || value > hi)
					run.failures.push_back({ "LED_CONTRAST_RANGE", ms, Fmt("CNTR %d outside [%d, %d]", value, lo, hi) });
				// Order: a cut's CNTR decrease goes before its LPW1 jump; one written after the jump lands
				// late (CNTR above need while the LED rises). Compensate: the bright cut's dump (to the
				// cut level, dmd_clip_cntr 59 by default, or above); the paired descent below it follows
				// the LED by design (owner 2026-10-11: CNTR's headroom stays while the LED is low).
				// Script (script_lookahead): the cut's CNTR goes a command gap ahead of its LPW1.
				const int cutLevel = config.cutCntrLevel >= 50 ? config.cutCntrLevel : 59;
				const bool dump = s.drive == DynamicDrive::Compensate ? value >= cutLevel : s.drive == DynamicDrive::Script && s.lookahead;
				if (dump && value < contrast && lastLightRiseJump &&
					ms - lastLightRiseMs > 0.0 && ms - lastLightRiseMs <= 2.0 * port.minCommandGapMs + 1.0)
					run.failures.push_back({ "LED_ORDER", ms, Fmt("CNTR %d -> %d written %.0f ms after an LPW1 jump", contrast, value,
						ms - lastLightRiseMs) });
				contrastAboveSince = value > 50 ? (contrast > 50 ? contrastAboveSince : ms) : -1.0;
				contrast = value;
			}
		}
		if (s.drive == DynamicDrive::None && !writes.empty())
			run.failures.push_back({ "LED_PICTURE_ONLY_WRITES", 0.0, Fmt("%zu writes with Dynamic Drive none", writes.size()) });
		module.Stop();
		// The exit restores: full light and CNTR 50.
		if (s.drive != DynamicDrive::None)
		{
			std::lock_guard<std::mutex> guard(projector->mutex);
			if (projector->contrast != 50 || projector->light != 255)
				run.failures.push_back({ "LED_EXIT_RESTORE", 0.0, Fmt("after Stop: LPW1 %d CNTR %d", projector->light, projector->contrast) });
		}
		run.wallSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - wallStart).count();
		return run;
	}

	std::string LedDescribe(const LedScenario& s)
	{
		std::string text = Fmt("led seed=%llu fps=%.3f %.0fs drive=%s lookahead=%d", static_cast<unsigned long long>(s.seed), s.fps,
			s.seconds, s.drive == DynamicDrive::Compensate ? "compensate" : s.drive == DynamicDrive::Script ? "script" : "none",
			s.lookahead ? 1 : 0);
		for (const Event& e : s.events) text += Fmt(" [%s@%.2f %.3g]", Name(e.kind), e.t, e.a);
		return text;
	}

	// ---------------- the intent lane ----------------
	// A smooth texture panning at a speed that changes slowly (no picture cuts): the class may
	// change only on a frame marked discontinuity (lost frames, a lock change, a resync), at the
	// first classification, or into / out of a still (static UI).
	constexpr int kWide = 4096, kW = 192, kH = 108;

	ContentIntent::LumaGrid PanGrid(double offset, double brightness)
	{
		// A precomputed wide texture (periodic), sampled at the pan offset with linear interpolation.
		static const std::vector<float> texture = []() -> std::vector<float> {
			std::vector<float> t(static_cast<size_t>(kWide) * kH);
			for (int y = 0; y < kH; ++y)
				for (int x = 0; x < kWide; ++x)
				{
					// Periodic over kWide (72, 27 and 135 whole cycles): no seam scrolls through.
					const double u = x * 2.0 * 3.14159265358979 * 72.0 / kWide, v = y * 0.13;
					t[static_cast<size_t>(y) * kWide + x] = static_cast<float>(0.5 + 0.2 * std::sin(u) * std::cos(v * 0.7) +
						0.15 * std::sin(u * 0.375 + v * 1.3) + 0.1 * std::cos(u * 1.875 - v * 0.4));
				}
			return t;
		}();
		ContentIntent::LumaGrid g;
		g.width = kW;
		g.height = kH;
		g.pictureAspect = 16.0 / 9.0;
		g.luma.resize(static_cast<size_t>(kW) * kH);
		const double base = std::floor(offset);
		const double frac = offset - base;
		const long long shift = static_cast<long long>(base);
		for (int y = 0; y < kH; ++y)
			for (int x = 0; x < kW; ++x)
			{
				const size_t x0 = static_cast<size_t>(((shift + x) % kWide + kWide) % kWide), x1 = (x0 + 1) % kWide;
				const double value = (1.0 - frac) * texture[static_cast<size_t>(y) * kWide + x0] + frac * texture[static_cast<size_t>(y) * kWide + x1];
				g.luma[static_cast<size_t>(y) * kW + x] = static_cast<uint16_t>((std::max)(64.0, (std::min)(940.0, 64.0 + brightness * 876.0 * value)));
			}
		return g;
	}
}

namespace QueueStressTest
{
	TEST_CLASS(QueueStressLaneTests)
	{
	public:
		// The LED module with look-ahead under queue faults: commands in range, CNTR never left
		// above need, the cut order, picture-only silent, the exit restored, the thread alive.
		TEST_METHOD(LedDriveUnderQueueFaults)
		{
			const int count = EnvCount("VP_QSTRESS_LED", 16);
			std::string failures;
			int failing = 0;
			double wall = 0.0;
			for (int seed = 1; seed <= count; ++seed)
			{
				const LedScenario s = RandomLed(static_cast<uint64_t>(seed), 45.0);
				const LedRun r = RunLed(s);
				wall += r.wallSeconds;
				LaneLog(Fmt("%s: frames %d LPW1 %d CNTR %d (%.1f s wall)", LedDescribe(s).c_str(), r.frames, r.lightWrites,
					r.contrastWrites, r.wallSeconds));
				if (!r.failures.empty())
				{
					++failing;
					std::set<std::string> seen;
					for (const Failure& f : r.failures)
						if (seen.insert(f.invariant).second)
						{
							failures += LedDescribe(s) + ": " + f.invariant + " @" + Fmt("%.0f ms", f.t) + " " + f.detail + "\n";
							LaneLog("  " + f.invariant + " @" + Fmt("%.0f ms ", f.t) + f.detail);
						}
				}
			}
			LaneLog(Fmt("%d LED scenarios, %d failing, %.1f s wall", count, failing, wall));
			Assert::IsTrue(failures.empty(), Wide(failures).c_str());
		}

		// The content-intent observer over the harness's kept stream (random scenarios).
		TEST_METHOD(IntentObserverOnTheKeptStream)
		{
			std::string failures;
			int changes = 0, scenarios = 0;
			for (uint64_t seed = 1; seed <= 12; ++seed)
			{
				Scenario s = Random(seed * 101, 120.0, 240.0);
				Result r;
				Source source(s);
				Lane lane(s, r);
				for (const SignalFrame& f : source.Generate()) lane.OnSignal(f);
				ContentIntent::Observer observer;
				observer.Configure(true, 5.0);
				ContentClass current = ContentClass::Unknown;
				bool classified = false;
				double offset = 0.0;
				uint64_t lastSequence = 0;
				++scenarios;
				for (const Kept& k : lane.kept)
				{
					// The pan speed drifts slowly between still and fast (picture heights per second).
					const double t = k.captureTime;
					const double hps = 0.25 + 0.25 * std::sin(t * 0.05);
					const double rate = k.filmHz > 0.0 ? k.filmHz : k.signalHz;
					offset += hps * 108.0 / (std::max)(1.0, rate);
					ContentIntent::FrameContext c;
					c.contentHz = rate;
					c.displayHz = s.displayHz;
					c.pulldownLocked = k.filmHz > 0.0;
					c.discontinuity = k.gapBefore;
					observer.ObserveGrid(k.laneSequence, PanGrid(offset, 0.8), c);
					const ContentIntentStatus st = observer.Status();
					if (k.laneSequence <= lastSequence)
						failures += Fmt("seed %llu: sequence %llu after %llu\n", static_cast<unsigned long long>(seed),
							static_cast<unsigned long long>(k.laneSequence), static_cast<unsigned long long>(lastSequence));
					lastSequence = k.laneSequence;
					if (st.content != current)
					{
						++changes;
						const bool allowed = !classified || k.gapBefore || st.content == ContentClass::StaticUi ||
							current == ContentClass::StaticUi;
						if (!allowed)
							failures += Fmt("seed %llu: class %s -> %s at %.3f s mid-shot (no discontinuity)\n",
								static_cast<unsigned long long>(seed), ToString(current), ToString(st.content), t);
						classified = classified || st.content != ContentClass::Unknown;
						current = st.content;
					}
					if (st.windowSeconds > 60.0)
						failures += Fmt("seed %llu: statistics window %.1f s\n", static_cast<unsigned long long>(seed), st.windowSeconds);
				}
			}
			LaneLog(Fmt("intent: %d scenarios, %d class changes", scenarios, changes));
			Assert::IsTrue(failures.empty(), Wide(failures.substr(0, 4000)).c_str());
		}

		// The source-resolution monitor under queue faults: an upscaled picture (bilinear 2x),
		// tile copies every 0.5 s on the frame clock, look-ahead frames as the queue holds them,
		// discontinuities at restarts / format changes, the clock stopping, bunching and going
		// backwards. Factors stay in (0, 1]; after a discontinuity the picture is native.
		TEST_METHOD(SourceResolutionUnderQueueFaults)
		{
			std::string failures;
			int engaged = 0, analyses = 0, resets = 0;
			for (uint64_t seed = 1; seed <= 4; ++seed)
			{
				Rng r(seed * 31337);
				SourceResolutionMonitor::Monitor m;
				m.SetSelection("auto");
				m.SetLatchFixes(seed % 3 != 0);
				m.SetEngageTiming(20.0, 1.5, 0.25);
				int W = 1920, H = 1080;
				const double fps = seed % 2 ? 24.0 : 60.0;
				double t = 10.0, lastSubmit = -1e9;
				uint64_t sequence = 0;
				int shot = 0;
				double nextCut = t + r.U(4.0, 12.0), resetAt = -1.0;
				unsigned long long discontinuitiesSeen = 0;
				std::map<std::tuple<int, int, int>, std::vector<float>> cache;
				int cacheShot = -1, cacheW = 0;
				auto pixel = [&](int x, int y, int s) -> double {
					// Bilinear 2x upscale of a deterministic noise field (half resolution).
					auto noise = [&](int xi, int yi) {
						uint64_t h = static_cast<uint64_t>(xi) * 73856093ull ^ static_cast<uint64_t>(yi) * 19349663ull ^
							static_cast<uint64_t>(s) * 83492791ull;
						h ^= h >> 13; h *= 0x5bd1e995ull; h ^= h >> 15;
						return 0.25 + 0.5 * static_cast<double>(h & 1023) / 1023.0;
					};
					const double fx = x * 0.5, fy = y * 0.5;
					const int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy);
					const double ax = fx - x0, ay = fy - y0;
					return (1 - ay) * ((1 - ax) * noise(x0, y0) + ax * noise(x0 + 1, y0)) + ay * ((1 - ax) * noise(x0, y0 + 1) + ax * noise(x0 + 1, y0 + 1));
				};
				const double end = t + 180.0;
				while (t < end)
				{
					// Faults: a starvation (the clock jumps), a burst (same time), the clock backwards,
					// a restart or a format change (discontinuity).
					const double u = r.U();
					double step = 1.0 / fps;
					if (u < 0.002) step = r.U(1.0, 8.0);
					else if (u < 0.004) step = 0.0;
					else if (u < 0.0045) step = -r.U(0.05, 2.0);
					if (u > 0.9985)
					{
						discontinuitiesSeen = m.Read().discontinuities;
						if (r.P(0.5)) { W = W == 1920 ? 1280 : 1920; H = W == 1920 ? 1080 : 720; m.Discontinuity("source format change"); }
						else m.Discontinuity("resync");
						resetAt = t;
						++resets;
						// The worker resets the evidence: wait for it (it runs on its own thread).
						for (int w = 0; w < 2000 && m.Read().discontinuities <= discontinuitiesSeen; ++w)
							std::this_thread::sleep_for(std::chrono::milliseconds(1));
					}
					t += step;
					++sequence;
					if (t >= nextCut) { ++shot; nextCut = t + r.U(4.0, 12.0); }
					const int tile = m.TileFor(H);
					if (t - lastSubmit >= 0.5 || t < lastSubmit)
					{
						lastSubmit = t;
						int base = 0;
						const std::vector<SourceResolution::TilePlace> places = SourceResolution::PlaceTiles(0, 0, W, H, tile, m.DenseGrid(), &base);
						std::vector<float> values(places.size() * static_cast<size_t>(tile) * tile);
						std::vector<float> tx, ty;
						for (size_t p = 0; p < places.size(); ++p)
						{
							// A shot is a still picture: its tiles are made once.
							if (cacheShot != shot || cacheW != W) { cache.clear(); cacheShot = shot; cacheW = W; }
							std::vector<float>& c = cache[std::make_tuple(places[p].x0, places[p].y0, tile)];
							if (c.empty())
							{
								c.resize(static_cast<size_t>(tile) * tile);
								for (int y = 0; y < tile; ++y)
									for (int x = 0; x < tile; ++x)
										c[static_cast<size_t>(y) * tile + x] = static_cast<float>(std::round(pixel(places[p].x0 + x, places[p].y0 + y, shot) * 1023.0) / 1023.0);
							}
							std::copy(c.begin(), c.end(), values.begin() + static_cast<std::ptrdiff_t>(p * tile * tile));
							tx.push_back(places[p].centreX);
							ty.push_back(places[p].centreY);
						}
						m.AnalyseTilesForTest(values, static_cast<int>(places.size()), t, W, H, tx, ty, base, tile, 64.0 / 1023.0);
						++analyses;
					}
					std::string why;
					double nx = 1.0, ny = 1.0;
					m.AutoFactors(W, H, 0.8, why, nx, ny, t, sequence);
					const SourceResolutionMonitor::Snapshot snap = m.Read();
					if (!(nx > 0.0 && nx <= 1.0 && ny > 0.0 && ny <= 1.0) || !std::isfinite(nx) || !std::isfinite(ny))
						failures += Fmt("seed %llu: factors %.4f x %.4f at %.3f s\n", static_cast<unsigned long long>(seed), nx, ny, t);
					if (!(snap.declaredRatio > 0.0 && snap.declaredRatio <= 1.0))
						failures += Fmt("seed %llu: declared %.4f at %.3f s\n", static_cast<unsigned long long>(seed), snap.declaredRatio, t);
					if (nx < 1.0 || ny < 1.0) ++engaged;
					// Right after a discontinuity the picture is native (no evidence carried over).
					if (resetAt >= 0.0 && sequence > 0 && t >= resetAt && t - resetAt < 0.25 && (nx < 1.0 || ny < 1.0))
						failures += Fmt("seed %llu: engaged %.3f x %.3f %.3f s after a discontinuity (%s)\n",
							static_cast<unsigned long long>(seed), nx, ny, t - resetAt, why.c_str());
					if (failures.size() > 3000) break;
				}
			}
			LaneLog(Fmt("source resolution: %d analyses, %d engaged frames, %d discontinuities", analyses, engaged, resets));
			Assert::IsTrue(failures.empty(), Wide(failures.substr(0, 4000)).c_str());
		}
	};
}
