#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/modules/input/queue_timing/PulldownDetector.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sstream>
#include <utility>
#include <vector>

#include <windows.h>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// Pulldown reliability (audit 2026-10-16, docs/audit/pulldown-review-2026-10-16.md):
// CPU-only streams of synthesized pictures - film at several rates carried in
// several signal rates, every start phase, with the events real sources give -
// replayed through Pulldown::Detector the way the renderer feeds it, checked
// against the review's pass criteria:
//  1 no new picture is ever dropped;
//  2 no lock on video, stills, animation on twos or random repeats;
//  3 a lock within FramesToLock + one cycle;
//  4 no unlock through fades, black, cuts or subtitles;
//  5 locked >= 98% of film time;
//  6 <= 2 wrong-time frames per phase event (fixes on);
//  7 a monotonic film clock spaced 1/f within +-1 ms under +-3 ms jitter;
//  8 every lock change marks the next kept frame (D1).
// Cadences are derived from the rates (exact ratios), never assumed.

namespace VideoProcessorTest
{
	namespace
	{
		// A rate as an exact ratio (24000/1001).
		struct Rate
		{
			long long num = 0, den = 1;
			double Hz() const { return static_cast<double>(num) / static_cast<double>(den); }
		};

		std::wstring Wide(const std::string& text) { return std::wstring(text.begin(), text.end()); }

		std::string RateText(const Rate& rate)
		{
			char text[32];
			std::snprintf(text, sizeof(text), "%.3f", rate.Hz());
			return text;
		}

		uint64_t Mix(uint64_t x)
		{
			x += 0x9E3779B97F4A7C15ull;
			x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
			x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
			return x ^ (x >> 31);
		}

		// ---- Synthetic pictures ----
		// Small 16-bit luma pictures: every row is sampled by the signature
		// (64 rows), so the real Pulldown::Signature decides what repeats.
		constexpr int kPictureWidth = 16;
		constexpr int kPictureHeight = 64;

		struct Picture
		{
			uint64_t content = 0;   // the film frame (scene + frame number)
			int gain = 1000;        // per mille: fades; 0 = black
			int subtitle = 0;       // a device subtitle drawn over it (0 none)
			bool operator==(const Picture& o) const { return content == o.content && gain == o.gain && subtitle == o.subtitle; }
		};

		uint64_t PictureSignature(const Picture& picture)
		{
			uint16_t samples[kPictureWidth * kPictureHeight];
			for (int y = 0; y < kPictureHeight; ++y)
				for (int x = 0; x < kPictureWidth; x += 4)
				{
					const uint64_t bits = Mix(picture.content * 1315423911ull + static_cast<uint64_t>(y * kPictureWidth + x));
					for (int k = 0; k < 4; ++k)
					{
						const int noise = static_cast<int>(((bits >> (16 * k)) & 0xFFFF) % 877);
						int value = 64 + noise * picture.gain / 1000;
						// A device subtitle: a text-like block near the bottom.
						if (picture.subtitle != 0 && y >= 52 && y < 60 && x + k >= 2 && x + k < 14)
							value = (Mix(static_cast<uint64_t>(picture.subtitle) * 977u + static_cast<uint64_t>(y * 31 + x + k)) & 1) ? 940 : 235;
						samples[y * kPictureWidth + x + k] = static_cast<uint16_t>(value);
					}
				}
			return Pulldown::Signature(samples, kPictureWidth * sizeof(uint16_t), kPictureHeight);
		}

		// ---- A signal: frames with their ground truth ----
		enum class Kind { Film, Video, Still };

		struct SignalFrame
		{
			Picture picture;
			Kind kind = Kind::Film;
			bool cadenceNew = false;    // truth: the first signal frame of a film frame
			bool subtitleOnly = false;  // truth: only the device subtitle changed (the same film frame)
			bool event = false;         // a phase event (edit, miss, restart, subtitle) starts here
			bool spike = false;         // the late-arrival spike
			bool slip = false;          // the source's own cadence slips here (a rate mismatch)
			int missedBefore = 0;       // signal frames lost in capture just before it (counted)
			double arrival = 0.0;       // seconds
		};

		class Tape
		{
		public:
			explicit Tape(Rate signal) : m_signal(signal) {}

			double SignalHz() const { return m_signal.Hz(); }
			int ApproxCycle() const { return m_approxCycle; }
			const std::vector<SignalFrame>& Frames() const { return m_frames; }

			// Start (or restart) film: the carried cadence `offset` signal
			// frames into its pattern - any phase. `event`: a restart.
			void StartFilm(Rate film, long long offset, bool event = false)
			{
				m_film = film;
				m_position = offset;
				m_lastFilmFrame = -1;
				m_scene += 1000000;
				m_eventPending = m_eventPending || event;
				// The shortest cycle within 0.2% of film / signal (the one a lock
				// takes): where the source's own cadence breaks it, it slips.
				const double ratio = film.Hz() / m_signal.Hz();
				m_approxCycle = 0;
				for (int q = 1; q <= Pulldown::Detector::MaxCycle(m_signal.Hz()) && m_approxCycle == 0; ++q)
					if (std::fabs(std::round(ratio * q) / q - ratio) <= ratio * 0.002)
						m_approxCycle = q;
				m_cadence.clear();
				m_lastSlip = -1000000;
			}

			// Film for `seconds`: each signal frame shows the latest film
			// frame due, as a source does; the gain ramps per film frame.
			void Film(double seconds, int gainFrom = 1000, int gainTo = 1000)
			{
				const long long count = std::llround(seconds * m_signal.Hz());
				for (long long i = 0; i < count; ++i)
				{
					const long long n = FilmFrameAt(m_position++);
					const bool cadenceNew = n != m_lastFilmFrame;
					m_lastFilmFrame = n;
					// (The segment's first frame is new by construction: not compared.)
					m_cadence.push_back(cadenceNew);
					const long long at = static_cast<long long>(m_cadence.size()) - 1;
					if (m_approxCycle > 0 && at - m_approxCycle >= 1 && m_cadence[static_cast<size_t>(at)] != m_cadence[static_cast<size_t>(at - m_approxCycle)] && at - m_lastSlip > m_approxCycle)
					{
						m_slipPending = true;
						m_lastSlip = at;
					}
					if (cadenceNew)
					{
						if (m_cutPending)
						{
							m_scene += 1000000;
							m_cutPending = false;
						}
						m_gain = gainFrom + static_cast<int>(std::lround((gainTo - gainFrom) * static_cast<double>(i) / static_cast<double>(count)));
					}
					SignalFrame frame;
					frame.kind = Kind::Film;
					frame.cadenceNew = cadenceNew;
					frame.picture.content = m_scene + static_cast<uint64_t>(n);
					frame.picture.gain = m_gain;
					if (m_subtitlePending && !cadenceNew)
					{
						++m_subtitle;
						frame.subtitleOnly = true;
						frame.event = true;
						m_subtitlePending = false;
					}
					frame.picture.subtitle = m_subtitle;
					if (m_dropPending > 0)
					{
						--m_dropPending;   // an edit took this frame out of the programme
						m_eventPending = true;
						continue;
					}
					if (m_missPending > 0)
					{
						--m_missPending;   // lost in capture: the renderer counts it
						++m_missed;
						m_eventPending = true;
						continue;
					}
					Push(frame);
				}
			}

			void Cut() { m_cutPending = true; }
			void Subtitle() { m_subtitlePending = true; }
			void EditDrop(int frames = 1) { m_dropPending += frames; }
			void Miss(int frames = 1) { m_missPending += frames; }

			// An edit that repeats the last signal frame once.
			void EditDuplicate()
			{
				SignalFrame frame = m_frames.back();
				frame.cadenceNew = false;
				frame.subtitleOnly = false;
				frame.missedBefore = 0;
				frame.spike = false;
				frame.event = true;
				m_frames.push_back(frame);
			}

			// Paused: the last picture held.
			void Pause(double seconds)
			{
				const long long count = std::llround(seconds * m_signal.Hz());
				for (long long i = 0; i < count; ++i)
				{
					SignalFrame frame = m_frames.back();
					frame.kind = Kind::Still;
					frame.cadenceNew = false;
					frame.subtitleOnly = false;
					frame.event = false;
					frame.missedBefore = 0;
					m_frames.push_back(frame);
				}
			}

			// Native video: every frame a new picture.
			void Video(double seconds)
			{
				m_scene += 1000000;
				const long long count = std::llround(seconds * m_signal.Hz());
				for (long long i = 0; i < count; ++i)
				{
					SignalFrame frame;
					frame.kind = Kind::Video;
					frame.cadenceNew = true;
					frame.picture.content = m_scene + static_cast<uint64_t>(i);
					frame.picture.subtitle = m_subtitle;
					Push(frame);
				}
			}

			// Arrival times: one signal period per slot (counted misses take
			// their slots), +-jitterMs uniform, one late spike.
			void Finish(uint32_t seed, double jitterMs, double spikeSeconds = -1.0, double spikeMs = 0.0)
			{
				uint64_t state = seed;
				long long slot = 0;
				const size_t spikeFrame = spikeSeconds >= 0.0 ? static_cast<size_t>(spikeSeconds * m_signal.Hz()) : static_cast<size_t>(-1);
				for (size_t i = 0; i < m_frames.size(); ++i)
				{
					slot += 1 + m_frames[i].missedBefore;
					state = Mix(state);
					const double u = static_cast<double>(state >> 11) / 9007199254740992.0;   // [0, 1)
					double jitter = (u * 2.0 - 1.0) * jitterMs * 1e-3;
					if (i == spikeFrame)
					{
						jitter += spikeMs * 1e-3;
						m_frames[i].spike = true;
					}
					m_frames[i].arrival = 10.0 + static_cast<double>(slot) * static_cast<double>(m_signal.den) / static_cast<double>(m_signal.num) + jitter;
				}
			}

		private:
			long long FilmFrameAt(long long position) const
			{
				// floor(position * film / signal), exactly.
				return (position * m_film.num * m_signal.den) / (m_film.den * m_signal.num);
			}

			void Push(SignalFrame frame)
			{
				frame.missedBefore = m_missed;
				m_missed = 0;
				frame.slip = m_slipPending;
				m_slipPending = false;
				if (m_eventPending)
				{
					frame.event = true;
					m_eventPending = false;
				}
				m_frames.push_back(frame);
			}

			Rate m_signal;
			Rate m_film{ 24, 1 };
			long long m_position = 0;
			long long m_lastFilmFrame = -1;
			int m_approxCycle = 0;
			std::vector<bool> m_cadence;   // cadenceNew by source position in the segment
			long long m_lastSlip = -1000000;
			bool m_slipPending = false;
			uint64_t m_scene = 0;
			int m_gain = 1000;
			int m_subtitle = 0;
			bool m_cutPending = false;
			bool m_subtitlePending = false;
			bool m_eventPending = false;
			int m_dropPending = 0;
			int m_missPending = 0;
			int m_missed = 0;
			std::vector<SignalFrame> m_frames;
		};

		// ---- The renderer's use of the detector ----
		struct Replayed
		{
			Pulldown::Decision decision;
			bool repeat = false;
			bool marked = false;     // this kept frame consumed a pending discontinuity
			bool timed = false;      // given a film-clock time
			double filmTime = 0.0;
		};

		// As LibplaceboVideoRenderer feeds it with the audit fixes wired:
		// repeat = signature equal to the previous captured frame; a counted
		// miss passes its count; dropped repeats are not queued; every kept
		// frame consumes the pending discontinuity; the film clock resets on a
		// new lock or an unlock (not on a re-phase).
		std::vector<Replayed> Replay(const std::vector<SignalFrame>& frames, double signalHz, const Pulldown::Settings& settings)
		{
			Pulldown::Detector detector;
			detector.SetSettings(settings);
			Pulldown::FilmClock clock;
			std::vector<Replayed> out(frames.size());
			uint64_t previous = 0;
			Picture previousPicture;
			bool havePicture = false, wasLocked = false;
			for (size_t i = 0; i < frames.size(); ++i)
			{
				const SignalFrame& frame = frames[i];
				const uint64_t signature = havePicture && frame.picture == previousPicture ? previous : PictureSignature(frame.picture);
				Replayed& r = out[i];
				r.repeat = previous != 0 && signature == previous;
				previous = signature;
				previousPicture = frame.picture;
				havePicture = true;
				const int missed = frame.missedBefore;
				r.decision = detector.Next(r.repeat, missed > 0, signalHz, missed > 0 ? missed : -1);
				const Pulldown::Decision& d = r.decision;
				if (d.relocked || (wasLocked && !d.locked))
					clock.Reset();
				wasLocked = d.locked;
				if (d.drop)
					continue;
				r.marked = detector.ConsumeDiscontinuity();
				if (d.locked && !d.update)
				{
					r.timed = true;
					r.filmTime = clock.Time(d.filmIndex, frame.arrival, d.filmHz, d.cadenceLateness, d.cadenceLatenessMax);
				}
			}
			return out;
		}

		// ---- Metrics ----
		struct Report
		{
			int newDropped = 0;          // 1
			long long firstLock = -1;    // 3
			int lockCycle = 0;
			double lockFilmHz = 0.0;
			int unlocksAfterLock = 0;    // 4
			long long filmFrames = 0, filmLocked = 0;   // 5
			int events = 0, worstEventWrong = 0, strayWrong = 0, totalWrong = 0;   // 6
			int clockBackwards = 0, clockEarly = 0, spacingChecked = 0, spacingOff = 0;   // 7
			double worstSpacingMs = 0.0;
			int lockChanges = 0, lockChangesOnDrops = 0, unmarked = 0;   // 8
			int rephases = 0;

			std::string Text() const
			{
				char text[400];
				std::snprintf(text, sizeof(text),
					"lock@%lld cycle %d film %.3f | unlocks %d locked %.2f%% | events %d worst %d stray %d total %d | rephases %d | "
					"clock back %d early %d spacing %d/%d worst %.3f ms | changes %d (on drops %d) unmarked %d | new dropped %d",
					firstLock, lockCycle, lockFilmHz, unlocksAfterLock, filmFrames ? 100.0 * static_cast<double>(filmLocked) / static_cast<double>(filmFrames) : 0.0,
					events, worstEventWrong, strayWrong, totalWrong, rephases, clockBackwards, clockEarly, spacingOff, spacingChecked,
					worstSpacingMs, lockChanges, lockChangesOnDrops, unmarked, newDropped);
				return text;
			}
		};

		// A frame decided at the wrong film time while locked: a new film frame
		// shown as an update of the previous one; a subtitle change taken for
		// a new film frame; a repeat slot taken for a new film frame; a film
		// frame's own slot dropped (its clock slot lost).
		bool WrongTime(const SignalFrame& frame, const Replayed& r)
		{
			const Pulldown::Decision& d = r.decision;
			if (!d.locked || frame.kind != Kind::Film)
				return false;
			if (frame.subtitleOnly)
				return !d.update;
			if (!r.repeat)
				return d.update;
			if (!frame.cadenceNew)
				return !d.drop && !d.update;
			return d.drop;
		}

		Report Evaluate(const std::vector<SignalFrame>& frames, const std::vector<Replayed>& run)
		{
			Report report;
			const size_t n = frames.size();
			bool expectMark = false, wasLocked = false;
			for (size_t i = 0; i < n; ++i)
			{
				const Pulldown::Decision& d = run[i].decision;
				if (d.drop && !run[i].repeat)
					++report.newDropped;
				if (d.lockChanged)
				{
					++report.lockChanges;
					report.lockChangesOnDrops += d.drop ? 1 : 0;
					expectMark = true;
				}
				if (!d.drop)
				{
					if (expectMark && !run[i].marked)
						++report.unmarked;
					expectMark = false;
				}
				report.rephases += d.rephased ? 1 : 0;
				if (wasLocked && !d.locked)
					++report.unlocksAfterLock;
				if (d.locked && report.firstLock < 0)
				{
					report.firstLock = static_cast<long long>(i);
					report.lockCycle = d.cycle;
					report.lockFilmHz = d.filmHz;
				}
				wasLocked = d.locked;
				if (frames[i].kind == Kind::Film && report.firstLock >= 0)
				{
					++report.filmFrames;
					report.filmLocked += d.locked ? 1 : 0;
				}
			}

			// Phase events: the edits, misses, restarts, subtitles, the spike and
			// the source's own slips; a wrong-time frame belongs to the latest
			// event at most 3 frames after it and 3 cycles + 3 frames before it.
			const int cycle = report.lockCycle > 0 ? report.lockCycle : 1;
			const long long window = 3LL * cycle + 3;
			std::vector<long long> events;
			for (size_t i = 0; i < n; ++i)
				if (frames[i].event || frames[i].spike || frames[i].slip)
					events.push_back(static_cast<long long>(i));
			report.events = static_cast<int>(events.size());
			std::vector<int> perEvent(events.size(), 0);
			for (size_t i = 0; i < n; ++i)
			{
				if (!WrongTime(frames[i], run[i]))
					continue;
				++report.totalWrong;
				int owner = -1;
				for (size_t e = 0; e < events.size(); ++e)
					if (events[e] <= static_cast<long long>(i) + 3 && static_cast<long long>(i) <= events[e] + window)
						owner = static_cast<int>(e);
				if (owner >= 0)
					++perEvent[owner];
				else
					++report.strayWrong;
			}
			for (int count : perEvent)
				report.worstEventWrong = (std::max)(report.worstEventWrong, count);

			// The film clock: never backwards, never before arrival; in quiet
			// stretches (no event, lock change or miss for the clock's window
			// plus 0.5 s) spaced exactly 1/f per film frame within 1 ms.
			const double settle = Pulldown::FilmClock::kWindowSeconds + 0.5;
			std::vector<bool> disturbs(n, false);
			for (long long e : events)
				disturbs[static_cast<size_t>(e)] = true;
			double lastDisturbance = -1e9, lastTime = 0.0;
			uint64_t lastIndex = 0;
			bool haveLast = false;
			for (size_t i = 0; i < n; ++i)
			{
				const Pulldown::Decision& d = run[i].decision;
				if (disturbs[i] || d.lockChanged || frames[i].missedBefore > 0)
					lastDisturbance = frames[i].arrival;
				if (d.relocked || !d.locked)
					haveLast = false;
				if (!run[i].timed)
					continue;
				const double t = run[i].filmTime;
				if (t < frames[i].arrival - 1e-9)
					++report.clockEarly;
				if (haveLast)
				{
					if (t <= lastTime)
						++report.clockBackwards;
					if (frames[i].arrival - lastDisturbance >= settle && d.filmIndex > lastIndex)
					{
						++report.spacingChecked;
						const double errorMs = std::fabs((t - lastTime) - static_cast<double>(d.filmIndex - lastIndex) / d.filmHz) * 1e3;
						report.worstSpacingMs = (std::max)(report.worstSpacingMs, errorMs);
						report.spacingOff += errorMs > 1.0 ? 1 : 0;
					}
				}
				lastTime = t;
				lastIndex = d.filmIndex;
				haveLast = true;
			}
			return report;
		}

		Pulldown::Settings FixesOn() { return Pulldown::Settings{}; }
		Pulldown::Settings FixesOff()
		{
			Pulldown::Settings settings;
			settings.keepLockAcrossMisses = false;
			settings.rephaseInPlace = false;
			settings.exclusiveFilmFloor = settings.trackingLatenessMax = settings.retryNeedsFailure = false;   // Hunt 2
			return settings;
		}

		// The cadence of `film` in `signal` reduced to lowest terms (new, cycle).
		std::pair<long long, long long> Cadence(Rate film, Rate signal)
		{
			long long a = film.num * signal.den, b = film.den * signal.num;
			long long x = a, y = b;
			while (y != 0) { const long long t = x % y; x = y; y = t; }
			return { a / x, b / x };
		}

		std::string EnvValue(const char* name)
		{
			char* v = nullptr;
			size_t n = 0;
			std::string value;
			if (_dupenv_s(&v, &n, name) == 0 && v) value = v;
			std::free(v);
			return value;
		}
	}

	TEST_CLASS(PulldownReliabilityTests)
	{
	public:
		// Criteria 1, 3-8 at every film / signal combination and start phase,
		// through fade to black and back, a black hold, a cut, a subtitle on a
		// repeat slot, an edit dropping and one duplicating a signal frame, a
		// counted capture miss, a pause restarting at a random phase, rate-
		// mismatch slips, +-3 ms arrival jitter and one 15 ms spike.
		TEST_METHOD(SynthesizedFilmThroughEventsAtEveryRate)
		{
			const Rate films[] = { { 24, 1 }, { 24000, 1001 }, { 25, 1 }, { 30, 1 } };
			const Rate signals[] = { { 60, 1 }, { 60000, 1001 }, { 50, 1 }, { 30, 1 }, { 120, 1 } };
			const long long phases[] = { 0, 2, 7 };
			uint32_t seed = 1;
			int scenarios = 0, lockChangesOnDrops = 0;
			for (const Rate& film : films)
				for (const Rate& signal : signals)
				{
					if (film.Hz() >= signal.Hz() * 0.99)
						continue;   // not carried by repeats
					for (long long phase : phases)
					{
						++seed;
						const std::string name = RateText(film) + " in " + RateText(signal) + " phase " + std::to_string(phase);
						Tape tape(signal);
						tape.StartFilm(film, phase);
						tape.Film(4.0);
						tape.Film(1.0, 1000, 0);   // fade to black
						tape.Film(1.0, 0, 0);      // black hold
						tape.Film(1.0, 0, 1000);   // and back
						tape.Film(1.0);
						tape.Cut();
						tape.Film(1.5);
						tape.Subtitle();           // on the next repeat slot
						tape.Film(2.5);
						tape.EditDrop();
						tape.Film(3.0);
						tape.EditDuplicate();
						tape.Film(3.0);
						tape.Miss();
						tape.Film(3.0);
						tape.Pause(1.5);
						tape.StartFilm(film, static_cast<long long>(Mix(seed) % 97), true);
						tape.Film(5.0);
						tape.Finish(seed, 3.0, 25.3, 15.0);
						const auto run = Replay(tape.Frames(), tape.SignalHz(), FixesOn());
						const Report r = Evaluate(tape.Frames(), run);
						Logger::WriteMessage((name + ": " + r.Text() + "\n").c_str());
						++scenarios;
						lockChangesOnDrops += r.lockChangesOnDrops;

						const std::wstring w = Wide(name);
						// Safety at every combination: nothing new dropped, the clock
						// never back or early, every lock change marked.
						Assert::AreEqual(0, r.newDropped, (w + L": no new picture dropped").c_str());
						Assert::AreEqual(0, r.clockBackwards, (w + L": the film clock never steps back").c_str());
						Assert::AreEqual(0, r.clockEarly, (w + L": never timed before arrival").c_str());
						Assert::AreEqual(0, r.unmarked, (w + L": every lock change marks the next kept frame").c_str());
						// A source whose own cadence slips more often than 1.5 lock
						// windows (23.976p in 50 Hz: about every 1.4 s) has no cadence
						// to hold: the safety criteria only.
						int slips = 0;
						for (const SignalFrame& frame : tape.Frames())
							slips += frame.slip ? 1 : 0;
						const int approxCycle = tape.ApproxCycle() > 0 ? tape.ApproxCycle() : 1;
						if (slips > 0 && tape.Frames().size() / static_cast<size_t>(slips) <
							3 * static_cast<size_t>(Pulldown::Detector::FramesToLock(approxCycle, signal.Hz())) / 2)
						{
							Logger::WriteMessage((name + ": irregular cadence (" + std::to_string(slips) + " slips): safety criteria only\n").c_str());
							continue;
						}
						Assert::IsTrue(r.firstLock >= 0, (w + L": locks").c_str());
						Assert::AreEqual(film.Hz(), r.lockFilmHz, film.Hz() * 0.005, (w + L": the film rate").c_str());
						// From the start, or from the source's last slip before the
						// lock (a slip restarts the exact-periodicity window).
						long long acquireFrom = 0;
						for (long long i = 0; i < r.firstLock; ++i)
							acquireFrom = tape.Frames()[static_cast<size_t>(i)].slip ? i : acquireFrom;
						Assert::IsTrue(r.firstLock + 1 <= acquireFrom + Pulldown::Detector::FramesToLock(r.lockCycle, signal.Hz()) + r.lockCycle,
							(w + L": lock within FramesToLock + one cycle").c_str());
						Assert::AreEqual(0, r.unlocksAfterLock, (w + L": no unlock through fades, black, cuts, subtitles, edits, misses, pauses").c_str());
						Assert::IsTrue(r.filmLocked >= r.filmFrames * 98 / 100, (w + L": locked >= 98% of film time").c_str());
						Assert::IsTrue(r.worstEventWrong <= 2, (w + L": <= 2 wrong-time frames per phase event").c_str());
						Assert::AreEqual(0, r.strayWrong, (w + L": no wrong-time frame away from an event").c_str());
						Assert::IsTrue(r.spacingChecked > 0, (w + L": quiet stretches checked").c_str());
						Assert::AreEqual(0, r.spacingOff, (w + L": spaced 1/f within 1 ms under 3 ms jitter").c_str());
					}
				}
			char text[128];
			std::snprintf(text, sizeof(text), "%d scenarios; %d lock changes landed on dropped repeats (D1)\n", scenarios, lockChangesOnDrops);
			Logger::WriteMessage(text);
		}

		// Criterion 2 (with the fixes on): never a lock on video, a still,
		// animation on twos or threes, or chance repeats.
		TEST_METHOD(NeverLocksOnVideoStillsTwosOrRandom)
		{
			auto neverLocks = [](const Tape& tape, const char* what)
			{
				const auto run = Replay(tape.Frames(), tape.SignalHz(), FixesOn());
				for (size_t i = 0; i < run.size(); ++i)
				{
					Assert::IsFalse(run[i].decision.locked, Wide(std::string(what) + ": no lock").c_str());
					Assert::IsFalse(run[i].decision.drop, Wide(std::string(what) + ": nothing dropped").c_str());
				}
			};
			for (const Rate& signal : { Rate{ 60, 1 }, Rate{ 50, 1 }, Rate{ 30, 1 }, Rate{ 120, 1 } })
			{
				Tape video(signal);
				video.Video(10.0);
				video.Finish(3, 3.0);
				neverLocks(video, "video");
				Tape still(signal);
				still.StartFilm(Rate{ 24, 1 }, 0);
				still.Film(0.1);
				still.Pause(10.0);
				still.Finish(4, 3.0);
				neverLocks(still, "still");
				// Animation on twos and threes: a regular pattern under 15 fps.
				for (const Rate& drawn : { Rate{ 12, 1 }, Rate{ 10, 1 }, Rate{ 8, 1 } })
				{
					Tape twos(signal);
					twos.StartFilm(drawn, 1);
					twos.Film(10.0);
					twos.Finish(5, 3.0);
					neverLocks(twos, ("animation at " + RateText(drawn) + " in " + RateText(signal)).c_str());
				}
			}
			// Chance repeats: a stuttering source, 20% and 50% of frames repeated.
			for (int share : { 20, 50 })
			{
				Tape random(Rate{ 60, 1 });
				random.Video(10.0);
				std::vector<SignalFrame> frames = random.Frames();
				uint64_t state = static_cast<uint64_t>(share);
				for (size_t i = 1; i < frames.size(); ++i)
				{
					state = Mix(state);
					if (state % 100 < static_cast<uint64_t>(share))
						frames[i].picture = frames[i - 1].picture;
				}
				const auto run = Replay(frames, 60.0, FixesOn());
				for (const auto& r : run)
					Assert::IsFalse(r.decision.locked || r.decision.drop, L"random repeats: no lock");
			}
		}

		// 60p video and a 30p segment inside 24p film: video unlocks within a
		// second and loses no picture; 30p locks as 30p (by design); each film
		// run locks within FramesToLock + one cycle of its start.
		TEST_METHOD(VideoAnd30pSegmentsUnlockAndRelock)
		{
			for (const Rate& signal : { Rate{ 60, 1 }, Rate{ 60000, 1001 } })
			{
				const Rate film24 = signal.den == 1 ? Rate{ 24, 1 } : Rate{ 24000, 1001 };
				const Rate film30 = signal.den == 1 ? Rate{ 30, 1 } : Rate{ 30000, 1001 };
				Tape tape(signal);
				struct Run { size_t start, end; Rate film; bool video; };
				std::vector<Run> runs;
				auto mark = [&](Rate film, bool video, auto add)
				{
					const size_t start = tape.Frames().size();
					add();
					runs.push_back({ start, tape.Frames().size(), film, video });
				};
				mark(film24, false, [&] { tape.StartFilm(film24, 3); tape.Film(6.0); });
				mark(film24, true, [&] { tape.Video(3.0); });
				mark(film24, false, [&] { tape.StartFilm(film24, 1); tape.Film(6.0); });
				mark(film30, false, [&] { tape.StartFilm(film30, 0); tape.Film(4.0); });
				mark(film24, false, [&] { tape.StartFilm(film24, 4); tape.Film(6.0); });
				tape.Finish(11, 3.0);
				const auto run = Replay(tape.Frames(), tape.SignalHz(), FixesOn());
				const Report report = Evaluate(tape.Frames(), run);
				Logger::WriteMessage(("segments in " + RateText(signal) + ": " + report.Text() + "\n").c_str());
				Assert::AreEqual(0, report.newDropped, L"no new picture dropped");
				Assert::AreEqual(0, report.unmarked, L"every lock change marks the next kept frame");
				for (const Run& segment : runs)
				{
					if (segment.video)
					{
						const size_t unlockBy = segment.start + static_cast<size_t>(std::ceil(signal.Hz()));
						for (size_t i = unlockBy; i < segment.end; ++i)
							Assert::IsFalse(run[i].decision.locked, L"video unlocks within a second");
						continue;
					}
					const auto cadence = Cadence(segment.film, signal);
					const int cycle = static_cast<int>(cadence.second), unique = static_cast<int>(cadence.first);
					const size_t lockBy = segment.start + static_cast<size_t>(Pulldown::Detector::FramesToLock(cycle, signal.Hz()) + cycle);
					size_t lockAt = segment.end;
					for (size_t i = segment.start; i < segment.end && lockAt == segment.end; ++i)
						if (run[i].decision.locked && run[i].decision.cycle == cycle && run[i].decision.unique == unique)
							lockAt = i;
					Assert::IsTrue(lockAt < lockBy, Wide("segment at " + std::to_string(segment.start) + ": lock within FramesToLock + one cycle").c_str());
					for (size_t i = lockAt; i < segment.end; ++i)
						Assert::IsTrue(run[i].decision.locked, L"stays locked to the end of the run");
				}
			}
		}

		// D3: a phase shift at every rotation (pause, restart at each phase)
		// and an edit dropping or duplicating one signal frame re-phases in
		// place - no unlock, at most two wrong-time frames - at the common
		// cadences, long ones included.
		TEST_METHOD(RephaseInPlaceAtEveryRotation)
		{
			struct Case { Rate film, signal; };
			const Case cases[] = { { { 24, 1 }, { 60, 1 } }, { { 24, 1 }, { 30, 1 } }, { { 25, 1 }, { 50, 1 } }, { { 24, 1 }, { 120, 1 } },
				{ { 24, 1 }, { 50, 1 } }, { { 25, 1 }, { 60, 1 } }, { { 30, 1 }, { 60, 1 } } };
			for (const Case& c : cases)
			{
				const int cycle = static_cast<int>(Cadence(c.film, c.signal).second);
				// edit 0: pause and restart at each rotation; 1: drop; 2: duplicate.
				for (int variant = 0; variant < cycle + 2; ++variant)
				{
					const int edit = variant < cycle ? 0 : variant - cycle + 1;
					const int restart = variant < cycle ? variant : 0;
					Tape tape(c.signal);
					tape.StartFilm(c.film, 0);
					tape.Film(3.0);
					if (edit == 0)
					{
						tape.Pause(0.5);
						tape.StartFilm(c.film, restart, true);
					}
					else if (edit == 1)
						tape.EditDrop();
					else
						tape.EditDuplicate();
					tape.Film(3.0);
					tape.Finish(static_cast<uint32_t>(100 + variant), 0.0);
					const auto run = Replay(tape.Frames(), tape.SignalHz(), FixesOn());
					const Report r = Evaluate(tape.Frames(), run);
					const std::string name = RateText(c.film) + " in " + RateText(c.signal) + (edit == 0 ? " restart at " + std::to_string(restart) :
						edit == 1 ? std::string(" edit drop") : std::string(" edit duplicate"));
					const std::wstring w = Wide(name);
					if (r.worstEventWrong > 0 || r.rephases > 0)
						Logger::WriteMessage((name + ": " + r.Text() + "\n").c_str());
					Assert::AreEqual(0, r.newDropped, (w + L": no new picture dropped").c_str());
					Assert::AreEqual(0, r.unlocksAfterLock, (w + L": no unlock").c_str());
					Assert::IsTrue(r.worstEventWrong <= 2, (w + L": <= 2 wrong-time frames").c_str());
					Assert::AreEqual(0, r.strayWrong, (w + L": none away from the event").c_str());
					Assert::AreEqual(0, r.unmarked, (w + L": a re-phase marks the next kept frame").c_str());
					if (edit != 0)
						Assert::AreEqual(1, r.rephases, (w + L": one re-phase").c_str());
				}
			}
		}

		// D4: a counted miss of 1 .. cycle frames keeps the lock with no
		// wrong-time frame and the film clock's index moving on; a longer one
		// resets (the old path).
		TEST_METHOD(CountedMissKeepsTheLock)
		{
			struct Case { Rate film, signal; };
			const Case cases[] = { { { 24, 1 }, { 60, 1 } }, { { 24, 1 }, { 30, 1 } }, { { 25, 1 }, { 50, 1 } }, { { 24, 1 }, { 50, 1 } }, { { 24, 1 }, { 120, 1 } } };
			for (const Case& c : cases)
			{
				const int cycle = static_cast<int>(Cadence(c.film, c.signal).second);
				for (int missed = 1; missed <= cycle + 1; ++missed)
					for (int at = 0; at < 3; ++at)
					{
						Tape tape(c.signal);
						tape.StartFilm(c.film, at);
						tape.Film(3.0);
						tape.Miss(missed);
						tape.Film(1.5);
						tape.Finish(7, 0.0);
						const auto run = Replay(tape.Frames(), tape.SignalHz(), FixesOn());
						const Report r = Evaluate(tape.Frames(), run);
						const std::wstring w = Wide(RateText(c.film) + " in " + RateText(c.signal) + " missed " + std::to_string(missed));
						Assert::AreEqual(0, r.newDropped, (w + L": no new picture dropped").c_str());
						size_t missAt = 0;
						while (tape.Frames()[missAt].missedBefore == 0) ++missAt;
						if (missed <= cycle)
						{
							Assert::AreEqual(0, r.unlocksAfterLock, (w + L": keeps the lock").c_str());
							Assert::AreEqual(0, r.totalWrong, (w + L": no wrong-time frame").c_str());
							Assert::AreEqual(0, r.clockBackwards, (w + L": the clock moves on").c_str());
							Assert::IsTrue(run[missAt].decision.discontinuity, (w + L": the miss is a pending discontinuity").c_str());
						}
						else
							Assert::IsFalse(run[missAt].decision.locked, (w + L": longer than a cycle resets").c_str());
					}
			}
		}

		// The switches off reproduce the 2026-10-14 behaviour: a phase shift
		// unlocks (after up to ~3 cycles of wrong-time frames) and the next
		// lock needs a fresh two seconds; a counted miss resets at once.
		TEST_METHOD(SwitchesOffReproduceTheOldBehaviour)
		{
			const Rate signal{ 60, 1 }, film{ 24, 1 };
			const int need = Pulldown::Detector::FramesToLock(5, signal.Hz());
			// An edit dropping one signal frame.
			{
				Tape tape(signal);
				tape.StartFilm(film, 0);
				tape.Film(4.0);
				tape.EditDrop();
				tape.Film(5.0);
				tape.Finish(9, 0.0);
				size_t edit = 0;
				while (!tape.Frames()[edit].event) ++edit;
				const auto off = Replay(tape.Frames(), tape.SignalHz(), FixesOff());
				const Report r = Evaluate(tape.Frames(), off);
				Logger::WriteMessage(("old, edit: " + r.Text() + "\n").c_str());
				Assert::AreEqual(1, r.unlocksAfterLock, L"off: the shift unlocks");
				Assert::AreEqual(0, r.rephases, L"off: no re-phase");
				size_t unlock = edit, relock = edit;
				while (off[unlock].decision.locked) ++unlock;
				relock = unlock;
				while (relock < off.size() && !off[relock].decision.locked) ++relock;
				Assert::IsTrue(relock < off.size(), L"off: found again");
				Assert::IsTrue(relock + 1 >= edit + static_cast<size_t>(need), L"off: the relock needs a fresh two seconds");
				Assert::IsTrue(r.totalWrong > 2, L"off: the old wrong-time window");
				const Report on = Evaluate(tape.Frames(), Replay(tape.Frames(), tape.SignalHz(), FixesOn()));
				Assert::AreEqual(0, on.unlocksAfterLock, L"on: no unlock");
				Assert::AreEqual(1, on.rephases, L"on: re-phased");
			}
			// A counted miss.
			{
				Tape tape(signal);
				tape.StartFilm(film, 0);
				tape.Film(4.0);
				tape.Miss();
				tape.Film(3.0);
				tape.Finish(10, 0.0);
				size_t miss = 0;
				while (tape.Frames()[miss].missedBefore == 0) ++miss;
				const auto off = Replay(tape.Frames(), tape.SignalHz(), FixesOff());
				Assert::IsFalse(off[miss].decision.locked, L"off: a miss resets");
				size_t relock = miss;
				while (relock < off.size() && !off[relock].decision.locked) ++relock;
				Assert::IsTrue(relock < off.size() && relock + 1 >= miss + static_cast<size_t>(need), L"off: relock after two seconds");
				const Report on = Evaluate(tape.Frames(), Replay(tape.Frames(), tape.SignalHz(), FixesOn()));
				Assert::AreEqual(0, on.unlocksAfterLock, L"on: the miss keeps the lock");
			}
		}

		// D2: the film-to-signal ratio while locked, 1 when not.
		TEST_METHOD(FilmToSignalRatio)
		{
			struct Case { Rate film, signal; };
			const Case cases[] = { { { 24, 1 }, { 60, 1 } }, { { 24, 1 }, { 30, 1 } }, { { 25, 1 }, { 50, 1 } }, { { 24, 1 }, { 50, 1 } }, { { 30, 1 }, { 120, 1 } } };
			for (const Case& c : cases)
			{
				Tape tape(c.signal);
				tape.StartFilm(c.film, 1);
				tape.Film(3.0);
				Pulldown::Detector detector;
				Assert::AreEqual(1.0, detector.FilmToSignalRatio(), 1e-12, L"1 when not locked");
				uint64_t previous = 0;
				for (const SignalFrame& frame : tape.Frames())
				{
					const uint64_t signature = PictureSignature(frame.picture);
					detector.Next(signature == previous, false, c.signal.Hz());
					previous = signature;
				}
				Assert::IsTrue(detector.Locked());
				Assert::AreEqual(c.film.Hz() / c.signal.Hz(), detector.FilmToSignalRatio(), 1e-12, L"new / cycle = film / signal");
				// 200 ms at the film rate.
				const long long frames = std::llround(0.2 * c.signal.Hz() * detector.FilmToSignalRatio());
				Assert::AreEqual(std::llround(0.2 * c.film.Hz()), frames, L"a time target resolves at the film rate");
			}
		}

		// Real footage (VP_TEST_REAL_FOOTAGE, the D:\hfr clips; skipped when
		// unset): the 120p clips decimated by 5 to 24p and the 50p clips by 2
		// to 25p, telecined into 60 / 30 / 50 Hz the way a source carries them;
		// only the rows the signature samples are read. Real grain must never
		// hash equal between film frames, the lock comes within FramesToLock +
		// one cycle and holds, and only exact repeats are dropped.
		TEST_METHOD(RealFootageTelecined)
		{
			const std::string folder = EnvValue("VP_TEST_REAL_FOOTAGE");
			if (folder.empty()) { Logger::WriteMessage("VP_TEST_REAL_FOOTAGE not set: skipped\n"); return; }
			struct Clip { std::string file; bool y4m; int frames; int decimate; Rate film; int W = 3840, H = 2160; };
			std::vector<Clip> clips = {
				{ "Bosphorus_3840x2160_120fps_420_10bit_YUV.yuv", false, 600, 5, { 24, 1 } },
				{ "Jockey_3840x2160_120fps_420_10bit_YUV.yuv", false, 600, 5, { 24, 1 } },
				{ "ReadySetGo_3840x2160_120fps_420_10bit_YUV.yuv", false, 600, 5, { 24, 1 } },
				{ "YachtRide_3840x2160_120fps_420_10bit_YUV.yuv", false, 600, 5, { 24, 1 } },
				{ "ShakeNDry_3840x2160_120fps_420_10bit_YUV.yuv", false, 300, 5, { 24, 1 } },
				{ "park_joy_2160p50.y4m", true, 500, 2, { 25, 1 } },
				{ "crowd_run_2160p50.y4m", true, 500, 2, { 25, 1 } } };
			// Real-footage suite 2026-10-18: any other 8-bit 4:2:0 y4m in the folder (the clip
			// library's film clips, D:/testclips/film) at its own size and rate - read from its
			// header, nothing fixed: up to 30 fps its frames are the film, faster ones are decimated
			// by the whole number that brings them to 30 fps or less.
			{
				WIN32_FIND_DATAA data{};
				const HANDLE find = FindFirstFileA((folder + "\\*.y4m").c_str(), &data);
				if (find != INVALID_HANDLE_VALUE)
				{
					do
					{
						const std::string name = data.cFileName;
						bool listed = false;
						for (const Clip& c : clips)
							listed = listed || c.file == name;
						if (listed)
							continue;
						FILE* f = nullptr;
						if (fopen_s(&f, (folder + "\\" + name).c_str(), "rb") != 0 || !f)
							continue;
						char header[512] = {};
						if (!fgets(header, sizeof(header), f)) { fclose(f); continue; }
						fclose(f);
						int w = 0, h = 0;
						long long fn = 0, fd = 1;
						bool plain420 = true;
						std::istringstream tokens(header);
						std::string token;
						while (tokens >> token)
						{
							if (token[0] == 'W') w = std::atoi(token.c_str() + 1);
							else if (token[0] == 'H') h = std::atoi(token.c_str() + 1);
							else if (token[0] == 'F') sscanf_s(token.c_str() + 1, "%lld:%lld", &fn, &fd);
							else if (token[0] == 'C') plain420 = token == "C420" || token == "C420jpeg" || token == "C420paldv" || token == "C420mpeg2";   // 8-bit 4:2:0 only
						}
						if (w <= 0 || h <= 0 || fn <= 0 || fd <= 0 || !plain420)
							continue;
						const long long frameBytes = static_cast<long long>(w) * h * 3 / 2 + 6;
						const long long frames = static_cast<long long>(data.nFileSizeHigh) << 32 | data.nFileSizeLow;
						const int decimate = static_cast<int>((std::max)(1LL, (fn + 30 * fd - 1) / (30 * fd)));
						Clip c{ name, true, static_cast<int>(frames / frameBytes), decimate, { fn, fd * decimate } };
						c.W = w;
						c.H = h;
						clips.push_back(c);
					} while (FindNextFileA(find, &data));
					FindClose(find);
				}
			}
			int clipsRead = 0;
			for (const Clip& clip : clips)
			{
				const int W = clip.W, H = clip.H;
				FILE* f = nullptr;
				if (fopen_s(&f, (folder + "\\" + clip.file).c_str(), "rb") != 0 || !f) continue;
				++clipsRead;
				long long header = 0;
				if (clip.y4m)
				{
					int ch;
					while ((ch = fgetc(f)) != EOF && ch != '\n') ++header;
					++header;
				}
				const int bytesPerSample = clip.y4m ? 1 : 2;
				const size_t rowBytes = static_cast<size_t>(W) * bytesPerSample;
				const long long frameBytes = clip.y4m ? static_cast<long long>(W) * H * 3 / 2 + 6 : static_cast<long long>(W) * H * 3;
				// One signature per film frame from the 64 sampled rows.
				std::vector<uint64_t> film;
				std::vector<uint8_t> rows(rowBytes * 64);
				for (int source = 0; source < clip.frames; source += clip.decimate)
				{
					const long long base = header + source * frameBytes + (clip.y4m ? 6 : 0);
					bool ok = true;
					for (int r = 0; r < 64 && ok; ++r)
					{
						const long long y = static_cast<long long>(2 * r + 1) * H / 128;
						ok = _fseeki64(f, base + y * static_cast<long long>(rowBytes), SEEK_SET) == 0 &&
							fread(rows.data() + r * rowBytes, 1, rowBytes, f) == rowBytes;
					}
					if (!ok) break;
					film.push_back(Pulldown::Signature(rows.data(), rowBytes, 64));
				}
				fclose(f);
				int equalNeighbours = 0;
				for (size_t i = 1; i < film.size(); ++i)
					equalNeighbours += film[i] == film[i - 1] ? 1 : 0;
				Assert::AreEqual(0, equalNeighbours, Wide(std::string(clip.file) + ": film frames hash distinct").c_str());
				for (const Rate& signal : { Rate{ 60, 1 }, Rate{ 30, 1 }, Rate{ 50, 1 } })
				{
					if (clip.film.Hz() >= signal.Hz() * 0.99) continue;
					const auto cadence = Cadence(clip.film, signal);
					const int cycle = static_cast<int>(cadence.second);
					Pulldown::Detector detector;
					uint64_t previous = 0;
					long long lockAt = -1;
					int unlocks = 0, newDropped = 0;
					bool wasLocked = false;
					for (long long i = 0;; ++i)
					{
						const long long n = (i * clip.film.num * signal.den) / (clip.film.den * signal.num);
						if (n >= static_cast<long long>(film.size())) break;
						const bool repeat = film[static_cast<size_t>(n)] == previous;
						previous = film[static_cast<size_t>(n)];
						const Pulldown::Decision d = detector.Next(repeat, false, signal.Hz());
						newDropped += d.drop && !repeat ? 1 : 0;
						if (d.locked && lockAt < 0) lockAt = i;
						unlocks += wasLocked && !d.locked ? 1 : 0;
						wasLocked = d.locked;
					}
					char text[200];
					std::snprintf(text, sizeof(text), "%s %d film frames in %s: lock@%lld (need %d) unlocks %d\n",
						clip.file.c_str(), static_cast<int>(film.size()), RateText(signal).c_str(), lockAt, Pulldown::Detector::FramesToLock(cycle, signal.Hz()), unlocks);
					Logger::WriteMessage(text);
					const std::wstring w = Wide(std::string(clip.file) + " in " + RateText(signal));
					Assert::AreEqual(0, newDropped, (w + L": no new picture dropped").c_str());
					Assert::IsTrue(lockAt >= 0 && lockAt + 1 <= Pulldown::Detector::FramesToLock(cycle, signal.Hz()) + cycle, (w + L": locks in time").c_str());
					Assert::AreEqual(0, unlocks, (w + L": holds").c_str());
				}
			}
			// VP_TEST_REAL_FOOTAGE was set (the run asked for real footage): a folder with none of the
			// clips is a failure, not a silent pass.
			Assert::IsTrue(clipsRead > 0, Wide("VP_TEST_REAL_FOOTAGE=" + folder + ": no clip found").c_str());
		}
	};
}
