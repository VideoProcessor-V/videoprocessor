#include "pch.h"
#include "CppUnitTest.h"

// Real-footage suite (2026-10-18): VP's CPU-side timing, cadence, classifier and
// source-resolution components fed with real decoded pictures from the clip library
// (D:\testclips, RealClipLibrary.h). Each clip's rate, cadence and planned events come
// from the manifest generated from the library's README; nothing here fixes a resolution,
// a rate or a file.
//
// One pass per clip run decodes the clip once (ffmpeg, 8-bit luma) and feeds every
// configuration ("lane") at the same time, each lane doing what
// LibplaceboVideoRenderer::OnVideoFrame and the render loop do with a frame:
//   - the capture timestamp check (CaptureTimestampGap, against the mode's nominal period,
//     as UpdateFrameRateAndPPM) and the cumulative measured rate;
//   - Pulldown::Detector and Pulldown::FilmClock with the renderer's switch wiring
//     (pulldown_fixes, cadence_class_fixes, timing_continuity_fixes);
//   - the enqueue (gap state, film rate, film update) and, one frame later, the dequeue's
//     decisions: MotionValidity (Invalidate on gapBefore, NoteKept), the gap-fill / hiccup
//     pair (InterpolationTimeline::GapFillPair, AdjacentPairTimes), the timeline's schedule
//     (InterpolationTimeline::Timeline restarts / re-anchors) and the matcher history chain
//     (DejudderPolicy::MatcherHistoryChained, motion_history_fixes);
//   - ContentIntent::Observer (the content classifier over source_class);
//   - SourceResolutionMonitor (tile copies every 0.5 s on the frame clock, AutoFactors /
//     AutoTransition every frame), latch fixes on and off.
// After the pass the kept-frame stream goes through a presentation simulation of the
// alpha queue (AlphaQueuePolicy: dequeue gate, timeline steering, backlog flush) at display
// rates 59.94, 60, 50, 23.976 and 120 Hz, with the display clock 0 / +-100 ppm off.
//
// Signal models
//   direct: the clip's frames are the signal at their own timestamps (a capture at the
//     clip's rate). Encoded repeats are not bit-exact (codec noise), so a "transport"
//     model gives the detector what a source device sends: a decoded frame whose mean
//     absolute difference from the previous one is at most kTransportRepeatCodes and whose
//     worst 16x16 block differs by at most kTransportBlockCodes (8-bit codes, measured on
//     tos_32_5994p: encoded repeats <= 0.53 mean / 5.7 per block, new pictures >= 0.89 mean
//     at the 1st percentile; a burned-in counter changing is a new picture) is delivered as
//     a bit-exact copy of the previous picture. The raw
//     bit-exact comparison (what VP sees from a player that sends the decoded stream) is
//     also counted ("exact" locks).
//   player: a player outputs the clip at a fixed signal rate (59.94 / 60 / 50 Hz): each
//     signal frame shows the latest decoded frame due (a quarter period of slack), a
//     bit-exact repeat otherwise
//     (pulldown made by the player, as the Apple TV does), with +-1 ms arrival jitter.
//
// Results: every run is logged (trx) and written as a per-clip table to
// VP_TEST_RESULTS_DIR (default %TEMP%\vp-realclip\results.md).

#include "RealClipLibrary.h"

#include <ContentClassifier.h>
#include <ContentIntentObserver.h>
#include <VP-V/modules/motion/estimation/MotionValidity.h>
#include <SourceResolutionDetector.h>
#include <vprenderer/AlphaQueuePolicy.h>
#include <VP-V/modules/input/queue_timing/AlphaQueuePolicyVpCompat.h>
#include <vprenderer/CaptureTimestampGap.h>
#include <VP-V/modules/motion/vector_delivery/DejudderPolicy.h>
#include <VP-V/modules/motion/consumers/InterpolationTimeline.h>
#include <VP-V/modules/input/queue_timing/PulldownDetector.h>
#include <vprenderer/SourceResolutionMonitor.h>
#include <vprenderer/StartupCropHold.h>

#include <algorithm>
#include <cstdarg>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
namespace RealFootage
{
	constexpr double kTransportRepeatCodes = 0.75;
	constexpr double kTransportBlockCodes = 8.0;
	constexpr long long kTicksPerSecond = 10000000;   // the capture clock (100 ns)
	constexpr double kTileLatency = 0.1;              // tile copy -> snapshot (as SourceResolutionLatchTests)

	inline std::string Fmt(const char* format, ...)
	{
		char buffer[2048];
		va_list args;
		va_start(args, format);
		vsnprintf(buffer, sizeof(buffer), format, args);
		va_end(args);
		return buffer;
	}

	inline const char* ClassName(ContentClass c) { return c == ContentClass::Unknown ? "unknown" : ToString(c); }

	inline void Log(const std::string& text) { Logger::WriteMessage((text + "\n").c_str()); }

	// ---- configurations ----
	struct Config
	{
		std::string name;
		bool pulldownFixes = true, classFixes = true, timingFixes = true;
	};
	inline std::vector<Config> Lanes()
	{
		std::vector<Config> lanes(4);
		lanes[0].name = "fixes on";
		lanes[1].name = "pulldown_fixes off";
		lanes[1].pulldownFixes = false;
		lanes[2].name = "cadence_class_fixes off";
		lanes[2].classFixes = false;
		lanes[3].name = "timing_continuity_fixes off";
		lanes[3].timingFixes = false;
		return lanes;
	}

	enum class Model { Direct, Player };

	struct RunSpec
	{
		std::string clip, filter, label;
		Model model = Model::Direct;
		double playerHz = 0.0;
		bool sourceResolution = false;
		std::string Key() const { return label.empty() ? clip : label; }
	};

	// ---- per-lane records ----
	struct LockEvent
	{
		int signal = 0, clipFrame = 0;
		double t = 0.0;
		bool locked = false, relocked = false, rephased = false;
		int cycle = 0, unique = 0;
		double filmHz = 0.0;
	};
	struct GapEvent
	{
		int clipFrame = 0;
		double t = 0.0;
		std::string kind;   // missed / break / resync
		long long frames = 0;
	};
	struct ClassChange
	{
		double t = 0.0;
		int clipFrame = 0;
		ContentClass content = ContentClass::Unknown;
		std::string reason;
	};
	struct Kept
	{
		double enqueue = 0.0;      // the arrival on the capture clock (seconds)
		double time = 0.0;         // the frame's time: film clock when locked, else the arrival
		double filmHz = 0.0;       // 0 = not locked
		double signalHz = 0.0;
		bool gapBefore = false, filmUpdate = false;
		uint32_t gapFrames = 0;
		int clipFrame = 0;
		uint64_t sequence = 0;
	};
	struct SrChange
	{
		double t = 0.0, fromY = 1.0, toY = 1.0, fade = 0.0;
		std::string why;
	};
	struct SrResult
	{
		bool run = false, latchFixes = true;
		std::vector<double> cutTimes;
		std::vector<SrChange> changes;
		double firstDeclared = -1.0, minDeclared = 1.0;
		std::vector<std::pair<double, double>> declared;   // (seconds, declared ratio) at each change
		int analyses = 0, lowerReadings = 0, nativeReadings = 0, blank = 0;
		std::vector<std::string> declaredEvidence;   // shots / evidence seconds at each declared change
		unsigned long long flashes = 0, coveredSteps = 0;
	};

	struct LaneResult
	{
		Config cfg;
		std::vector<LockEvent> locks;
		std::vector<GapEvent> gaps;
		std::vector<int> invalidations;        // clip frames whose dequeue invalidated the vectors
		std::vector<std::string> invalidationCauses;
		std::vector<int> gapFillPairs;         // clip frames (B) of hiccup-repair pairs
		std::vector<int> refusedGapPairs;      // B after a gap, no pair
		std::vector<int> adjacencyRefused;     // ordinary pairs refused by AdjacentPairTimes
		std::vector<int> timelineRestarts, timelineReanchors;
		std::vector<int> repeatFlags;          // clip frames delivered as a repeat (transport)
		std::vector<ClassChange> classes;
		std::map<int, long long> classFrames;  // frames per class after the first class
		long long keptFrames = 0, droppedRepeats = 0, filmUpdates = 0, signalFrames = 0, lockedSignal = 0;
		long long matcherRestarts = 0, matcherStaleUses = 0;   // pairs whose A was not the last estimated end
		long long pairs = 0, pairsRefused = 0;
		// FilmClock (locked kept frames, not updates): spacing error, earliness, monotonicity.
		double clockMaxSpacingErrorMs = 0.0, clockMaxEarlyMs = 0.0;
		long long clockSamples = 0, clockNonMonotonic = 0, clockSpacingOver1ms = 0;
		std::vector<int> clockOver1msSignals;
		std::string classStats;   // the classifier's last window statistics
		std::vector<Kept> kept;
		std::vector<char> lockedBySignal;      // per signal frame: locked after the decision
		std::vector<int> signalClipFrame;      // per signal frame: the clip frame shown
		std::vector<double> signalTime;
		SrResult srOn, srOff;
		SrResult srCutOff;   // h3-latch: latch fixes on, source_resolution_cut_fixes off (the judge before)
		long long exactLocks = 0;              // locks with raw bit-exact repeat flags (fixes on only)
	};

	struct RunResult
	{
		RunSpec spec;
		const RealClip::Clip* clip = nullptr;
		bool ok = false;
		std::string error;
		int decoded = 0, signalFrames = 0, width = 0, height = 0;
		double signalHz = 0.0, seconds = 0.0, wallSeconds = 0.0;
		std::vector<double> pts;
		std::vector<LaneResult> lanes;
		std::vector<int> transportRepeats;     // decoded frames that are codec-noise copies of the previous
		std::vector<double> frameMad;          // per decoded frame (vs previous)
	};

	// ---- one lane: the renderer's per-frame logic ----
	struct SignalFrame
	{
		int index = 0;            // signal frame number
		int clipFrame = 0;        // the decoded frame shown
		double arrival = 0.0;     // seconds on the capture clock
		long long ticks = 0;
		bool repeat = false;      // a bit-exact repeat of the previous signal frame (as delivered)
		bool exactRepeat = false; // the raw decoded comparison
		bool resync = false;      // a mode change before this frame (everything restarts)
		double nominalHz = 0.0;   // the mode's rate
		const ContentIntent::LumaGrid* grid = nullptr;
		const std::vector<uint8_t>* luma = nullptr;
		int width = 0, height = 0;
	};

	class Lane
	{
	public:
		explicit Lane(const Config& cfg, bool classifier, bool sr, bool exactShadow)
			: m_classifier(classifier), m_sr(sr), m_exactShadow(exactShadow)
		{
			result.cfg = cfg;
			Restart();
		}

		LaneResult result;

		void Restart()
		{
			m_pd.reset(new Pulldown::Detector());
			m_exact.reset(new Pulldown::Detector());
			m_clock = Pulldown::FilmClock();
			m_settingsApplied = false;
			m_forceGap = false;
			m_filmTime = 0.0;
			m_firstTicks = m_lastTicks = m_publishTicks = 0;
			m_rateFrames = 0;
			m_measuredHz = 0.0;
			m_mv.reset(new MotionValidity::State());
			m_timeline = InterpolationTimeline::Timeline();
			m_restartsSeen = m_reanchorsSeen = 0;
			m_havePending = false;
			m_lastEstimatedEnd = 0;
			if (m_classifier)
			{
				m_observer.reset(new ContentIntent::Observer());
				m_observer->Configure(true, 5.0);
			}
			if (m_sr)
			{
				m_srMonitors.clear();
				for (int i = 0; i < kSrLanes; ++i)
				{
					// 0: latch fixes + cut fixes (default), 1: latch fixes off, 2: latch fixes, cut fixes off.
					std::unique_ptr<SourceResolutionMonitor::Monitor> m(new SourceResolutionMonitor::Monitor());
					m->SetSelection("auto");
					m->SetLatchFixes(i != 1);
					m->SetCutFixes(i == 0);
					m->SetEngageTiming(20.0, 1.5, 0.25);
					m_srMonitors.push_back(std::move(m));
					m_srJobs[i].clear();
					m_srFx[i] = m_srFy[i] = 1.0;
					m_srCuts[i] = 0;
				}
				m_srQueue.clear();
				m_srLastSubmit = -1e9;
			}
		}

		void OnSignal(const SignalFrame& f)
		{
			if (f.resync)
			{
				Flush();
				Restart();
				result.gaps.push_back({ f.clipFrame, f.arrival, "resync", 0 });
			}
			m_nominalHz = f.nominalHz;
			const Config& cfg = result.cfg;
			++result.signalFrames;
			result.signalClipFrame.push_back(f.clipFrame);
			result.signalTime.push_back(f.arrival);
			if (f.repeat)
				result.repeatFlags.push_back(f.clipFrame);
			// UpdateFrameRateAndPPM: the step against the mode's period.
			bool gapBefore = false;
			uint32_t gapFrames = 0;
			const long long expected = f.nominalHz > 0.0 ? std::llround(kTicksPerSecond / f.nominalHz) : 0;
			bool brk = false;
			long long missed = 0;
			if (m_lastTicks > 0 && expected > 0)
			{
				const CaptureTimestampGap::Step step = CaptureTimestampGap::Classify(f.ticks - m_lastTicks, expected, kTicksPerSecond);
				if (step.kind == CaptureTimestampGap::Kind::Missed)
				{
					missed = step.periods - 1;
					m_rateFrames += step.periods - 1;
					result.gaps.push_back({ f.clipFrame, f.arrival, "missed", missed });
				}
				else if (step.kind == CaptureTimestampGap::Kind::Break)
				{
					brk = true;
					result.gaps.push_back({ f.clipFrame, f.arrival, "break", step.periods });
				}
			}
			if (brk)
			{
				m_firstTicks = m_lastTicks = m_publishTicks = f.ticks;
				m_rateFrames = 1;
				m_measuredHz = 0.0;
			}
			else if (m_firstTicks == 0)
			{
				m_firstTicks = m_lastTicks = m_publishTicks = f.ticks;
				m_rateFrames = 1;
			}
			else
			{
				++m_rateFrames;
				m_lastTicks = f.ticks;
				if (f.ticks - m_publishTicks >= 5 * kTicksPerSecond && m_rateFrames > 1)
				{
					m_publishTicks = f.ticks;
					m_measuredHz = static_cast<double>(m_rateFrames - 1) / (static_cast<double>(f.ticks - m_firstTicks) / kTicksPerSecond);
				}
			}
			m_lastTicks = f.ticks;
			if (missed > 0)
			{
				gapBefore = true;
				gapFrames = static_cast<uint32_t>((std::min)(missed, 1000LL));
			}
			if (brk && cfg.timingFixes)
			{
				gapBefore = true;
				gapFrames = 0;
			}
			if (f.resync)
			{
				gapBefore = true;
				gapFrames = 0;
			}

			// The raw bit-exact shadow (fixes on): what VP sees when the player sends the decoded stream.
			if (m_exactShadow)
			{
				const bool was = m_exact->Locked();
				const Pulldown::Decision d = m_exact->Next(f.exactRepeat, gapBefore, SignalHz(), gapBefore ? static_cast<int>(missed) : -1);
				if (d.locked && !was)
					++result.exactLocks;
			}

			// Pulldown, as OnVideoFrame.
			const double signalHz = SignalHz();
			const bool fixes = cfg.pulldownFixes, classFixes = cfg.classFixes;
			if (!m_settingsApplied)
			{
				Pulldown::Settings s;
				s.keepLockAcrossMisses = s.rephaseInPlace = s.cadenceAwareClock = fixes;
				s.exclusiveFilmFloor = s.trackingLatenessMax = s.retryNeedsFailure = classFixes;
				s.maximallyEvenPattern = classFixes;
				m_pd->SetSettings(s);
				m_settingsApplied = true;
			}
			if (m_clock.SetRobustAnchor(classFixes))
			{
				m_pd->MarkDiscontinuity();
				m_forceGap = true;
			}
			if (m_clock.SetRebaseOnRateChange(cfg.timingFixes))
			{
				m_pd->MarkDiscontinuity();
				m_forceGap = true;
			}
			const bool wasLocked = m_pd->Locked();
			const Pulldown::Decision d = m_pd->Next(f.repeat, gapBefore, signalHz,
				gapBefore && fixes ? static_cast<int>((std::min)(missed, 1000LL)) : -1);
			result.lockedBySignal.push_back(d.locked ? 1 : 0);
			if (d.locked)
				++result.lockedSignal;
			if (d.locked != wasLocked || d.relocked)
			{
				gapBefore = true;
				gapFrames = 0;
				m_clock.Reset();
				LockEvent e;
				e.signal = f.index;
				e.clipFrame = f.clipFrame;
				e.t = f.arrival;
				e.locked = d.locked;
				e.relocked = d.relocked;
				e.cycle = d.cycle;
				e.unique = d.unique;
				e.filmHz = d.filmHz;
				result.locks.push_back(e);
			}
			if (d.drop)
			{
				if (gapBefore && fixes)
					m_pd->MarkDiscontinuity();
				++result.droppedRepeats;
				return;
			}
			if (gapBefore || d.rephased || d.update || d.phaseUncertain || filmHzChanged(d))
				m_lastDisturbance = f.arrival;
			const bool forcedGap = m_forceGap;
			m_forceGap = false;
			if (forcedGap && classFixes)
			{
				gapBefore = true;
				gapFrames = 0;
			}
			if (m_pd->ConsumeDiscontinuity() && fixes)
			{
				gapBefore = true;
				gapFrames = 0;
			}
			double filmHz = 0.0, time = f.arrival;
			bool filmUpdate = false;
			if (d.rephased)
			{
				gapFrames = 0;
				if (classFixes)
					m_clock.Reanchor();
				LockEvent e;
				e.signal = f.index;
				e.clipFrame = f.clipFrame;
				e.t = f.arrival;
				e.locked = true;
				e.rephased = true;
				e.cycle = d.cycle;
				e.unique = d.unique;
				e.filmHz = d.filmHz;
				result.locks.push_back(e);
			}
			if (d.locked)
			{
				filmHz = d.filmHz;
				if (d.update && m_filmTime > 0.0)
				{
					filmUpdate = true;
					time = m_filmTime;
					++result.filmUpdates;
				}
				else
				{
					time = fixes ? m_clock.Time(d.filmIndex, f.arrival, filmHz, d.cadenceLateness, d.cadenceLatenessMax) :
						m_clock.Time(d.filmIndex, f.arrival, filmHz);
					// C7 (as PulldownCorpusTests): never before the arrival, monotonic; in quiet stretches
					// (no lock change, re-phase, gap, film update or rate change for the clock window +
					// 0.5 s) spaced exactly 1/f per film frame within 1 ms.
					if (m_lastClock > 0.0 && m_lastClockFilmHz == filmHz)
					{
						const double dt = time - m_lastClock;
						if (dt <= 0.0)
							++result.clockNonMonotonic;
						if (f.arrival - m_lastDisturbance >= Pulldown::FilmClock::kWindowSeconds + 0.5 && d.filmIndex > m_lastFilmIndex)
						{
							++result.clockSamples;
							const double err = std::fabs(dt - static_cast<double>(d.filmIndex - m_lastFilmIndex) / filmHz) * 1000.0;
							result.clockMaxSpacingErrorMs = (std::max)(result.clockMaxSpacingErrorMs, err);
							if (err > 1.0)
							{
								++result.clockSpacingOver1ms;
								result.clockOver1msSignals.push_back(f.index);
							}
						}
					}
					m_lastFilmIndex = d.filmIndex;
					result.clockMaxEarlyMs = (std::max)(result.clockMaxEarlyMs, (f.arrival - time) * 1000.0);
					m_lastClock = time;
					m_lastClockFilmHz = filmHz;
					m_filmTime = time;
				}
			}
			else
			{
				m_filmTime = 0.0;
				m_lastClock = 0.0;
			}
			// Enqueue.
			Kept k;
			k.enqueue = f.arrival;
			k.time = time;
			k.filmHz = filmHz;
			k.signalHz = signalHz;
			k.gapBefore = gapBefore;
			k.gapFrames = gapFrames;
			k.filmUpdate = filmUpdate;
			k.clipFrame = f.clipFrame;
			k.sequence = ++m_sequence;
			++result.keptFrames;
			result.kept.push_back(k);
			// The classifier sees each queued frame once, in source order.
			if (m_observer && f.grid)
			{
				ContentIntent::FrameContext c;
				c.contentHz = filmHz > 0.0 ? filmHz : signalHz;
				c.displayHz = 60000.0 / 1001.0;
				c.pulldownLocked = filmHz > 0.0;
				c.discontinuity = gapBefore;
				c.classFixes = classFixes;
				m_observer->ObserveGrid(k.sequence, *f.grid, c);
				const ContentIntentStatus s = m_observer->Status();
				if (result.classes.empty() || result.classes.back().content != s.content)
				{
					ClassChange ch;
					ch.t = f.arrival;
					ch.clipFrame = f.clipFrame;
					ch.content = s.content;
					ch.reason = s.reason;
					result.classes.push_back(ch);
				}
				if (s.content != ContentClass::Unknown)
					++result.classFrames[static_cast<int>(s.content)];
				result.classStats = Fmt("candidate %s (%s) pan %.3f hps wide %.2f window %.1f s motion %d",
					ClassName(s.candidate), s.reason, s.panPictureHeightsPerSecond, s.widePanShare, s.windowSeconds, s.motionValid ? 1 : 0);
			}
			if (m_sr && f.luma)
				SourceResolutionStep(f, k);
			// The dequeue of the previous frame, now that this one is queued behind it.
			if (m_havePending)
				Dequeue(m_pending, &k);
			m_pending = k;
			m_havePending = true;
		}

		void Flush()
		{
			if (m_havePending)
				Dequeue(m_pending, nullptr);
			m_havePending = false;
		}

	private:
		double SignalHz() const { return m_measuredHz > 0.0 ? m_measuredHz : m_nominalHz; }

		void Dequeue(const Kept& a, const Kept* b)
		{
			const Config& cfg = result.cfg;
			if (a.gapBefore)
			{
				m_mv->Invalidate(MotionValidity::Cause::Gap);
				result.invalidations.push_back(a.clipFrame);
			}
			m_mv->NoteKept(a.sequence);
			double nextPts = 0.0;
			bool pair = false;
			if (b)
			{
				const double pairRateHz = a.filmHz > 0.0 ? a.filmHz : SignalHz();
				const double timeA = a.time, timeB = b->time;
				const bool spanOk = pairRateHz > 0.0 && timeA > 0.0 && timeB > timeA &&
					timeB - timeA <= (InterpolationTimeline::kMaxSpanFrames + 0.25) / pairRateHz;
				const bool fillGap = InterpolationTimeline::GapFillPair(b->gapBefore, true, b->gapFrames, spanOk, false);
				const bool adjacentOk = fillGap || !cfg.timingFixes ||
					CaptureTimestampGap::AdjacentPairTimes(timeA, a.filmHz > 0.0, timeB, b->filmHz > 0.0, pairRateHz);
				pair = (!b->gapBefore || fillGap) && !b->filmUpdate && adjacentOk;
				if (b->gapBefore)
					(fillGap ? result.gapFillPairs : result.refusedGapPairs).push_back(b->clipFrame);
				if (!adjacentOk)
					result.adjacencyRefused.push_back(b->clipFrame);
				if (pair)
				{
					++result.pairs;
					nextPts = timeB;
					if (!DejudderPolicy::MatcherHistoryChained(a.sequence, m_lastEstimatedEnd) && m_lastEstimatedEnd != 0)
						++result.matcherRestarts;   // with motion_history_fixes: restarted; without: stale history used
					m_lastEstimatedEnd = b->sequence;
				}
				else
					++result.pairsRefused;
			}
			const double rateHz = a.filmHz > 0.0 ? a.filmHz : SignalHz();
			m_timeline.Plan(1, a.sequence, a.time, nextPts, rateHz, 60000.0 / 1001.0, false);
			if (m_timeline.Restarts() != m_restartsSeen)
				result.timelineRestarts.push_back(a.clipFrame);
			if (m_timeline.Reanchors() != m_reanchorsSeen)
				result.timelineReanchors.push_back(a.clipFrame);
			m_restartsSeen = m_timeline.Restarts();
			m_reanchorsSeen = m_timeline.Reanchors();
		}

		// The renderer's look-ahead (sourceAheadFrames): the queued frames after the one rendered are shown
		// to LookAheadFrame before it is rendered. Modelled as kSrQueuedFrames frames queued ahead: each
		// arriving frame goes to the look-ahead, and the frame that many frames older is rendered (its
		// tile copy and AutoFactors).
		static constexpr size_t kSrQueuedFrames = 3;
		void SourceResolutionStep(const SignalFrame& f, const Kept& k)
		{
			SrQueued q;
			q.f = f;
			q.k = k;
			q.luma = std::make_shared<std::vector<uint8_t>>(*f.luma);
			q.f.luma = q.luma.get();
			const int W = f.width, H = f.height;
			const std::vector<uint8_t>& luma = *q.luma;
			for (auto& m : m_srMonitors)
				m->LookAheadFrame(k.sequence, W, H, [&luma, W](int x, int y) { return static_cast<int>(luma[static_cast<size_t>(y) * W + x]) * 4; },
					0, 0, W, H);
			m_srQueue.push_back(std::move(q));
			if (m_srQueue.size() > kSrQueuedFrames)
			{
				SrQueued front = std::move(m_srQueue.front());
				m_srQueue.pop_front();
				SourceResolutionRender(front.f, front.k);
			}
		}

		void SourceResolutionRender(const SignalFrame& f, const Kept& k)
		{
			const double t = k.enqueue;
			const int W = f.width, H = f.height;
			const int tile = m_srMonitors[0]->TileFor(H);
			const double black = 64.0 / 1023.0;
			if (t - m_srLastSubmit >= 0.5)
			{
				m_srLastSubmit = t;
				for (int i = 0; i < kSrLanes; ++i)
				{
					int base = 0;
					const std::vector<SourceResolution::TilePlace> places =
						SourceResolution::PlaceTiles(0, 0, W, H, i == 0 ? tile : SourceResolutionMonitor::Monitor::kTile,
							m_srMonitors[static_cast<size_t>(i)]->DenseGrid(), &base);
					const int size = m_srMonitors[static_cast<size_t>(i)]->TileFor(H);
					SrJob j;
					j.due = t + kTileLatency;
					j.seconds = t;
					j.sequence = k.sequence;
					j.count = static_cast<int>(places.size());
					j.base = base;
					j.size = size;
					j.values.resize(static_cast<size_t>(j.count) * size * size);
					for (int p = 0; p < j.count; ++p)
					{
						const SourceResolution::TilePlace& pl = places[static_cast<size_t>(p)];
						for (int y = 0; y < size; ++y)
						{
							const int sy = (std::min)(H - 1, pl.y0 + y);
							const uint8_t* row = f.luma->data() + static_cast<size_t>(sy) * W;
							float* dst = &j.values[(static_cast<size_t>(p) * size + y) * size];
							for (int x = 0; x < size; ++x)
								dst[x] = static_cast<float>(row[(std::min)(W - 1, pl.x0 + x)] * 4) / 1023.0f;
						}
						j.tx.push_back(pl.centreX);
						j.ty.push_back(pl.centreY);
					}
					m_srJobs[i].push_back(std::move(j));
				}
			}
			for (int i = 0; i < kSrLanes; ++i)
			{
				SourceResolutionMonitor::Monitor& m = *m_srMonitors[static_cast<size_t>(i)];
				SrResult& r = i == 0 ? result.srOn : i == 1 ? result.srOff : result.srCutOff;
				r.run = true;
				r.latchFixes = i != 1;
				while (!m_srJobs[i].empty() && m_srJobs[i].front().due <= t + 1e-9)
				{
					SrJob& j = m_srJobs[i].front();
					m.AnalyseTilesForTest(j.values, j.count, j.seconds, W, H, j.tx, j.ty, j.base, j.size, black, j.sequence);
					++r.analyses;
					const SourceResolutionMonitor::Snapshot s = m.Read();
					if (s.frameBlank)
						++r.blank;
					else
						++(s.frameRatio < 1.0 ? r.lowerReadings : r.nativeReadings);
					if (s.declaredRatio < 1.0 && r.firstDeclared < 0)
						r.firstDeclared = j.seconds;
					if (r.declared.empty() ? s.declaredRatio < 1.0 : std::fabs(r.declared.back().second - s.declaredRatio) > 1e-9)
					{
						r.declared.push_back({ j.seconds, s.declaredRatio });
						r.declaredEvidence.push_back(Fmt("%d shots %.1f s conf %.2f", s.shots, s.evidenceSeconds, s.confidence));
					}
					r.flashes = s.flashes;
					r.coveredSteps = s.coveredSteps;
					r.minDeclared = (std::min)(r.minDeclared, s.declaredRatio);
					if (s.cuts != m_srCuts[i])
					{
						m_srCuts[i] = s.cuts;
						// The cut's own moment (a deferred confirmation reports the frame after the cut).
						r.cutTimes.push_back(s.lastCutSeconds >= 0.0 ? s.lastCutSeconds : j.seconds);
					}
					m_srJobs[i].pop_front();
				}
				std::string why;
				double nx = 1.0, ny = 1.0;
				m.AutoFactors(W, H, 0.8, why, nx, ny, t, k.sequence);
				if (std::fabs(nx - m_srFx[i]) > 1e-9 || std::fabs(ny - m_srFy[i]) > 1e-9)
				{
					SrChange c;
					c.t = t;
					c.fromY = m_srFy[i];
					c.toY = ny;
					c.why = why;
					c.fade = m.AutoTransition(t).duration;
					r.changes.push_back(c);
				}
				m_srFx[i] = nx;
				m_srFy[i] = ny;
			}
		}

		struct SrJob
		{
			double due = 0.0, seconds = 0.0;
			std::vector<float> values, tx, ty;
			int count = 0, base = 0, size = 0;
			uint64_t sequence = 0;
		};
		struct SrQueued
		{
			SignalFrame f;
			Kept k;
			std::shared_ptr<std::vector<uint8_t>> luma;
		};
		static constexpr int kSrLanes = 3;
		std::deque<SrQueued> m_srQueue;

		bool m_classifier, m_sr, m_exactShadow;
		std::unique_ptr<Pulldown::Detector> m_pd, m_exact;
		Pulldown::FilmClock m_clock;
		bool m_settingsApplied = false, m_forceGap = false;
		double m_filmTime = 0.0, m_lastClock = 0.0, m_lastClockFilmHz = 0.0, m_lastDisturbance = -1e9, m_disturbFilmHz = 0.0;
		uint64_t m_lastFilmIndex = 0;
		bool filmHzChanged(const Pulldown::Decision& d)
		{
			const bool changed = d.locked && d.filmHz != m_disturbFilmHz;
			m_disturbFilmHz = d.locked ? d.filmHz : 0.0;
			return changed;
		}
		long long m_firstTicks = 0, m_lastTicks = 0, m_publishTicks = 0, m_rateFrames = 0;
		double m_measuredHz = 0.0, m_nominalHz = 0.0;
		uint64_t m_sequence = 0, m_lastEstimatedEnd = 0;
		std::unique_ptr<MotionValidity::State> m_mv;
		InterpolationTimeline::Timeline m_timeline;
		unsigned long long m_restartsSeen = 0, m_reanchorsSeen = 0;
		Kept m_pending;
		bool m_havePending = false;
		std::unique_ptr<ContentIntent::Observer> m_observer;
		std::vector<std::unique_ptr<SourceResolutionMonitor::Monitor>> m_srMonitors;
		std::deque<SrJob> m_srJobs[kSrLanes];
		double m_srLastSubmit = -1e9, m_srFx[kSrLanes] = { 1, 1, 1 }, m_srFy[kSrLanes] = { 1, 1, 1 };
		unsigned long long m_srCuts[kSrLanes] = { 0, 0, 0 };
	};

	// ---- decoded-frame helpers ----
	inline double MeanAbsDiff(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, int w, int h)
	{
		// Every 2nd column of every 4th row: the codec-noise test needs no more.
		long long sum = 0, count = 0;
		for (int y = 0; y < h; y += 4)
		{
			const uint8_t* pa = a.data() + static_cast<size_t>(y) * w;
			const uint8_t* pb = b.data() + static_cast<size_t>(y) * w;
			for (int x = 0; x < w; x += 2)
			{
				sum += std::abs(static_cast<int>(pa[x]) - static_cast<int>(pb[x]));
				++count;
			}
		}
		return count ? static_cast<double>(sum) / static_cast<double>(count) : 0.0;
	}

	// The largest mean absolute difference of any 16 x 16 block (full resolution): a codec's
	// re-coded repeat stays below kTransportBlockCodes everywhere (measured on tos_32_5994p: at
	// most 5.7), a real local change (a burned-in counter, a small moving object) does not.
	inline double MaxBlockDiff(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, int w, int h)
	{
		double worst = 0.0;
		const int bw = w / 16, bh = h / 16;
		std::vector<int> sums(static_cast<size_t>(bw), 0);
		for (int by = 0; by < bh; ++by)
		{
			std::fill(sums.begin(), sums.end(), 0);
			for (int y = by * 16; y < by * 16 + 16; ++y)
			{
				const uint8_t* pa = a.data() + static_cast<size_t>(y) * w;
				const uint8_t* pb = b.data() + static_cast<size_t>(y) * w;
				for (int x = 0; x < bw * 16; ++x)
					sums[static_cast<size_t>(x >> 4)] += std::abs(static_cast<int>(pa[x]) - static_cast<int>(pb[x]));
			}
			for (int s : sums)
				worst = (std::max)(worst, s / 256.0);
		}
		return worst;
	}

	inline void BuildGrid(const std::vector<uint8_t>& luma, int w, int h, ContentIntent::LumaGrid& grid)
	{
		ContentIntent::BuildGrid([&](int x, int y, uint16_t& v) {
			v = static_cast<uint16_t>(luma[static_cast<size_t>(y) * w + x] * 4);
			return true;
		}, 0, 0, w, h, grid);
	}

	// ---- one clip run ----
	inline RunResult Run(const RealClip::Manifest& manifest, const RunSpec& spec)
	{
		const auto started = std::chrono::steady_clock::now();
		RunResult r;
		r.spec = spec;
		r.clip = manifest.Find(spec.clip);
		if (!r.clip)
		{
			r.error = "clip not in the manifest: " + spec.clip;
			return r;
		}
		const RealClip::Clip& clip = *r.clip;
		r.pts = RealClip::ProbeTimestamps(manifest, clip, r.error);
		if (r.pts.empty())
		{
			r.error = "no timestamps: " + r.error;
			return r;
		}
		RealClip::Decoder decoder;
		if (!decoder.Open(manifest, clip, spec.filter, r.error))
			return r;
		const int W = decoder.width, H = decoder.height;
		r.width = W;
		r.height = H;
		const std::vector<Config> configs = Lanes();
		std::vector<std::unique_ptr<Lane>> lanes;
		for (size_t i = 0; i < configs.size(); ++i)
			lanes.emplace_back(new Lane(configs[i], true, spec.sourceResolution && i == 0, i == 0));
		const double p0 = r.pts.front();
		// The rate change (direct model): an HDMI mode change, the only way a capture's rate changes.
		int rateChangeFrame = -1;
		double rateAfter = 0.0;
		for (const RealClip::Event& e : clip.EventsOf("rate_change"))
		{
			rateChangeFrame = e.Frame();
			rateAfter = e.json->Num("rate_num") / e.json->Num("rate_den", 1.0);
		}
		std::vector<uint8_t> raw, previousRaw, delivered;
		ContentIntent::LumaGrid grid;
		bool haveDelivered = false;
		int decodedIndex = -1;
		auto decodeNext = [&]() -> bool {
			previousRaw.swap(raw);
			if (!decoder.Next(raw))
				return false;
			++decodedIndex;
			double mad = 1e9;
			bool exact = false;
			if (decodedIndex > 0 && previousRaw.size() == raw.size())
			{
				mad = MeanAbsDiff(raw, previousRaw, W, H);
				exact = std::memcmp(raw.data(), previousRaw.data(), raw.size()) == 0;
			}
			r.frameMad.push_back(decodedIndex > 0 ? mad : -1.0);
			const bool transport = decodedIndex > 0 && mad <= kTransportRepeatCodes &&
				(exact || MaxBlockDiff(raw, previousRaw, W, H) <= kTransportBlockCodes);
			if (transport)
				r.transportRepeats.push_back(decodedIndex);
			if (!transport || !haveDelivered)
			{
				delivered = raw;
				haveDelivered = true;
				BuildGrid(delivered, W, H, grid);
			}
			(void)exact;
			return true;
		};
		int signal = 0;
		if (spec.model == Model::Direct)
		{
			r.signalHz = clip.RateHz();
			while (decodeNext())
			{
				SignalFrame f;
				f.index = signal++;
				f.clipFrame = decodedIndex;
				const double t = decodedIndex < static_cast<int>(r.pts.size()) ? r.pts[static_cast<size_t>(decodedIndex)] - p0 :
					(r.pts.back() - p0) + (decodedIndex - static_cast<int>(r.pts.size()) + 1) / clip.RateHz();
				f.arrival = 1.0 + t;   // a capture clock that does not start at 0
				f.ticks = std::llround(f.arrival * kTicksPerSecond);
				f.repeat = decodedIndex > 0 && r.transportRepeats.size() && r.transportRepeats.back() == decodedIndex;
				f.exactRepeat = decodedIndex > 0 && r.frameMad.back() == 0.0 && raw == previousRaw;
				f.nominalHz = rateChangeFrame >= 0 && decodedIndex >= rateChangeFrame ? rateAfter : clip.RateHz();
				f.resync = decodedIndex == rateChangeFrame;
				f.grid = &grid;
				f.luma = &delivered;
				f.width = W;
				f.height = H;
				for (auto& lane : lanes)
					lane->OnSignal(f);
			}
		}
		else
		{
			// A player at a fixed signal rate: the latest decoded frame due, else a repeat. A frame
			// due within a quarter period after a vblank is shown at it (a player's scheduling
			// slack: container timestamps rounded to 1 ms must not move frames by a vblank).
			r.signalHz = spec.playerHz;
			const double end = r.pts.back() - p0 + 1.0 / clip.RateHz();
			int shown = -1;
			bool more = decodeNext();
			std::vector<uint8_t> shownLuma;
			for (long long k = 0; more || shown >= 0; ++k)
			{
				const double tk = static_cast<double>(k) / spec.playerHz;
				if (tk >= end)
					break;
				// Advance to the latest frame due at tk.
				while (more && decodedIndex + 1 < static_cast<int>(r.pts.size()) &&
					r.pts[static_cast<size_t>(decodedIndex + 1)] - p0 <= tk + 0.25 / spec.playerHz)
				{
					more = decodeNext();
				}
				if (!more && decodedIndex < 0)
					break;
				SignalFrame f;
				f.index = signal++;
				f.clipFrame = decodedIndex;
				const double jitter = (static_cast<double>((k * 7919) % 5) - 2.0) * 0.0005;   // +-1 ms
				f.arrival = 1.0 + tk + jitter;   // QPC arrival (callback jitter)
				f.ticks = std::llround((1.0 + tk) * kTicksPerSecond);   // the capture's hardware clock
				const bool sameFrame = decodedIndex == shown;
				const bool noiseCopy = !sameFrame && !r.transportRepeats.empty() && r.transportRepeats.back() == decodedIndex;
				f.repeat = k > 0 && (sameFrame || noiseCopy);
				f.exactRepeat = k > 0 && sameFrame;
				shown = decodedIndex;
				f.nominalHz = spec.playerHz;
				f.grid = &grid;
				f.luma = &delivered;
				f.width = W;
				f.height = H;
				for (auto& lane : lanes)
					lane->OnSignal(f);
				if (!more && decodedIndex + 1 >= static_cast<int>(r.pts.size()) && tk > end)
					break;
			}
		}
		for (auto& lane : lanes)
		{
			lane->Flush();
			r.lanes.push_back(std::move(lane->result));
		}
		decoder.Close();
		r.decoded = decodedIndex + 1;
		r.signalFrames = signal;
		r.seconds = r.pts.back() - p0;
		r.ok = r.decoded > 0;
		if (r.decoded != static_cast<int>(r.pts.size()))
			r.error = Fmt("decoded %d frames, %zu timestamps", r.decoded, r.pts.size());
		r.wallSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
		return r;
	}

	// ---- the presentation simulation of the alpha queue ----
	struct QueueSimResult
	{
		int flushes = 0, holds = 0, settledHolds = 0;
		size_t maxDepth = 0;
		std::vector<double> flushTimes;
		double medianFlushInterval = 0.0;
	};

	// The kept frames enqueue at their arrival; the render loop wakes on the display's vblanks
	// (displayHz, the display clock ppm off the capture's) and consumes source frames at the
	// queue's own rate (the film rate while locked) through the timeline's source clock, trimmed
	// by the queue steering; after each dequeue the backlog check runs as the renderer's
	// (AlphaQueuePolicy::ShouldRecoverBacklog with the queue's rate, or the old rule).
	inline QueueSimResult SimulateQueue(const std::vector<Kept>& kept, double displayHz, double ppm, bool fixes,
		size_t target = 5, double settleSeconds = 200.0, double jitterMs = 2.0)
	{
		QueueSimResult result;
		if (kept.empty())
			return result;
		const double vblank = (1.0 / displayHz) * (1.0 + ppm * 1e-6);
		std::deque<size_t> queue;
		size_t next = 0;
		AlphaQueuePolicy::TimelineQueueSteering steering;
		bool startup = true;
		double sourceClock = 0.0, nextFrameAt = 0.0;   // seconds of source consumed / the next frame's start
		const double t0 = kept.front().enqueue;
		const double end = kept.back().enqueue + 1.0;
		double lastDequeueRate = kept.front().filmHz > 0.0 ? kept.front().filmHz : kept.front().signalHz;
		for (long long v = 0;; ++v)
		{
			const double now = t0 + static_cast<double>(v) * vblank;
			if (now > end)
				break;
			// The enqueue on the render side: the frame's arrival plus the capture callback's jitter
			// (deterministic +-jitterMs, never reordering), as QueueTimingFixesTests' simulation.
			auto enqueueAt = [&](size_t i) {
				const double j = (static_cast<double>((i * 7919) % 9) - 4.0) / 4.0 * jitterMs * 1e-3;
				return kept[i].enqueue + j;
			};
			while (next < kept.size() && enqueueAt(next) <= now)
			{
				if (!kept[next].filmUpdate)   // a film update replaces its film frame (no time of its own)
					queue.push_back(next);
				++next;
			}
			bool held = false;
			int dequeues = 0;
			while (sourceClock + 1e-12 >= nextFrameAt && dequeues < 8)
			{
				if (!AlphaQueuePolicy::CanDequeue(queue.size(), target, startup))
				{
					held = true;
					break;
				}
				startup = false;
				const Kept& f = kept[queue.front()];
				queue.pop_front();
				++dequeues;
				const double rate = f.filmHz > 0.0 ? f.filmHz : f.signalHz;
				lastDequeueRate = rate;
				steering.Update(queue.size(), target, rate, fixes);
				nextFrameAt += 1.0 / rate;
				result.maxDepth = (std::max)(result.maxDepth, queue.size());
				// The backlog check after the frame's render.
				const double frontFilm = queue.empty() ? 0.0 : kept[queue.front()].filmHz;
				const double signalRate = f.signalHz;
				const double queueRate = fixes ? AlphaQueuePolicy::QueueFrameRateHz(frontFilm, signalRate) : rate;
				const double renderCycleMs = (std::max)(1.0, std::floor(displayHz / rate)) * vblank * 1000.0;
				const double oldestAgeMs = queue.empty() ? 0.0 : (now - enqueueAt(queue.front())) * 1000.0;
				if (AlphaQueuePolicy::ShouldRecoverBacklog(queue.size(), target, oldestAgeMs, renderCycleMs, queueRate, fixes))
				{
					while (queue.size() > target)
						queue.pop_front();
					++result.flushes;
					result.flushTimes.push_back(now - t0);
					steering.Reset();
				}
			}
			if (held)
			{
				if (!startup)
				{
					++result.holds;
					if (now - t0 >= settleSeconds)
						++result.settledHolds;
				}
				continue;   // the source clock waits for a frame
			}
			if (!startup)
				sourceClock += (1.0 / displayHz) * steering.RateTrim();
		}
		(void)lastDequeueRate;
		if (result.flushTimes.size() >= 2)
		{
			std::vector<double> d;
			for (size_t i = 1; i < result.flushTimes.size(); ++i)
				d.push_back(result.flushTimes[i] - result.flushTimes[i - 1]);
			std::sort(d.begin(), d.end());
			result.medianFlushInterval = d[d.size() / 2];
		}
		return result;
	}

	// ---- the suite: runs, cache, results table ----
	struct Row
	{
		std::string category, clip, check, result, numbers;
	};

	class Suite
	{
	public:
		static Suite& Get()
		{
			static Suite suite;
			return suite;
		}

		RealClip::Manifest manifest;
		bool available = false;
		std::string skipReason;

		// The cached run (decoded once per process).
		const RunResult& Result(const RunSpec& spec)
		{
			const std::string key = spec.Key() + (spec.model == Model::Player ? Fmt("@player%.3f", spec.playerHz) : std::string());
			auto it = m_cache.find(key);
			if (it != m_cache.end())
				return *it->second;
			std::unique_ptr<RunResult> r(new RunResult(Run(manifest, spec)));
			Log(Fmt("RUN %s: %d frames decoded (%dx%d), %d signal frames at %.3f Hz, %.1f s of clip in %.1f s%s%s",
				key.c_str(), r->decoded, r->width, r->height, r->signalFrames, r->signalHz, r->seconds, r->wallSeconds,
				r->error.empty() ? "" : " - ", r->error.c_str()));
			const RunResult& ref = *r;
			m_cache[key] = std::move(r);
			return ref;
		}

		void Add(const std::string& category, const std::string& clip, const std::string& check, bool pass,
			const std::string& numbers)
		{
			m_rows.push_back({ category, clip, check, pass ? "PASS" : "FAIL", numbers });
			Log(Fmt("%s | %s | %s | %s | %s", category.c_str(), clip.c_str(), check.c_str(), pass ? "PASS" : "FAIL", numbers.c_str()));
			Write();
		}
		// A real bug documented for bug hunt round 3 (not fixed here): reported, not failed.
		void Deferred(const std::string& category, const std::string& clip, const std::string& check, bool pass, const std::string& numbers)
		{
			m_rows.push_back({ category, clip, check, pass ? "PASS" : "KNOWN BUG (round 3)", numbers });
			Log(Fmt("%s | %s | %s | %s | %s", category.c_str(), clip.c_str(), check.c_str(), pass ? "PASS" : "KNOWN BUG (round 3)", numbers.c_str()));
			Write();
		}
		void Info(const std::string& category, const std::string& clip, const std::string& check, const std::string& numbers)
		{
			m_rows.push_back({ category, clip, check, "INFO", numbers });
			Log(Fmt("%s | %s | %s | INFO | %s", category.c_str(), clip.c_str(), check.c_str(), numbers.c_str()));
			Write();
		}

		std::vector<const RealClip::Clip*> Clips(const std::function<bool(const RealClip::Clip&)>& wanted) const
		{
			std::vector<const RealClip::Clip*> out;
			// VP_TEST_CLIP_FILTER: comma-separated id substrings (a quick subset; empty = all).
			std::vector<std::string> filters;
			std::stringstream list(RealClip::Env("VP_TEST_CLIP_FILTER"));
			std::string item;
			while (std::getline(list, item, ','))
				if (!item.empty())
					filters.push_back(item);
			for (const RealClip::Clip& c : manifest.clips)
			{
				bool selected = filters.empty();
				for (const std::string& f : filters)
					selected = selected || c.id.find(f) != std::string::npos;
				if (selected && wanted(c) && RealClip::FileExists(manifest.FullPath(c)))
					out.push_back(&c);
			}
			return out;
		}

		bool Skip(const char* test)
		{
			if (available)
				return false;
			const bool require = RealClip::Env("VP_TEST_REQUIRE_FOOTAGE") == "1";
			Log(Fmt("%s: SKIP-NO-FOOTAGE (%s)", test, skipReason.c_str()));
			if (require)
				Assert::Fail(L"VP_TEST_REQUIRE_FOOTAGE=1 and the clip library is missing");
			return true;
		}

	private:
		Suite()
		{
			manifest = RealClip::Manifest::Load();
			available = manifest.Loaded() && RealClip::FileExists(manifest.ffmpeg) && RealClip::FileExists(manifest.FfprobePath());
			if (!manifest.Loaded())
				skipReason = manifest.error;
			else if (!available)
				skipReason = "ffmpeg / ffprobe not found at " + manifest.ffmpeg;
			std::string dir = RealClip::Env("VP_TEST_RESULTS_DIR");
			if (dir.empty())
			{
				char temp[MAX_PATH] = {};
				GetTempPathA(MAX_PATH, temp);
				dir = std::string(temp) + "vp-realclip";
			}
			CreateDirectoryA(dir.c_str(), nullptr);
			m_resultsPath = dir + "\\results.md";
		}

		void Write() const
		{
			FILE* f = nullptr;
			if (fopen_s(&f, m_resultsPath.c_str(), "wb") != 0 || !f)
				return;
			fprintf(f, "| Category | Clip run | Check | Result | Numbers |\n|---|---|---|---|---|\n");
			for (const Row& row : m_rows)
				fprintf(f, "| %s | %s | %s | %s | %s |\n", row.category.c_str(), row.clip.c_str(), row.check.c_str(),
					row.result.c_str(), row.numbers.c_str());
			fclose(f);
		}

		std::map<std::string, std::unique_ptr<RunResult>> m_cache;
		std::vector<Row> m_rows;
		std::string m_resultsPath;
	};

	// ---- analysis helpers ----
	inline const LaneResult& On(const RunResult& r) { return r.lanes[0]; }
	inline const LaneResult* LaneNamed(const RunResult& r, const std::string& name)
	{
		for (const LaneResult& l : r.lanes)
			if (l.cfg.name == name)
				return &l;
		return nullptr;
	}

	inline int CountLocks(const LaneResult& l)
	{
		int n = 0;
		for (const LockEvent& e : l.locks)
			n += e.locked && !e.rephased && (e.relocked) ? 1 : 0;
		return n;
	}
	inline int CountUnlocks(const LaneResult& l)
	{
		int n = 0;
		for (const LockEvent& e : l.locks)
			n += !e.locked ? 1 : 0;
		return n;
	}
	inline int CountRephases(const LaneResult& l)
	{
		int n = 0;
		for (const LockEvent& e : l.locks)
			n += e.rephased ? 1 : 0;
		return n;
	}
	inline const LockEvent* FirstLock(const LaneResult& l, int fromSignal = 0)
	{
		for (const LockEvent& e : l.locks)
			if (e.locked && e.relocked && e.signal >= fromSignal)
				return &e;
		return nullptr;
	}
	// Share of signal frames in [first, last] that are locked to `unique` of `cycle` (0 = any).
	inline double LockedShare(const LaneResult& l, int first, int last, int cycle = 0, int unique = 0)
	{
		if (last < first)
			return 0.0;
		// Reconstruct the cadence per signal frame from the lock events.
		long long locked = 0, total = 0;
		size_t e = 0;
		int curCycle = 0, curUnique = 0;
		for (int s = 0; s <= last && s < static_cast<int>(l.lockedBySignal.size()); ++s)
		{
			while (e < l.locks.size() && l.locks[e].signal <= s)
			{
				curCycle = l.locks[e].locked ? l.locks[e].cycle : 0;
				curUnique = l.locks[e].locked ? l.locks[e].unique : 0;
				++e;
			}
			if (s < first)
				continue;
			++total;
			const bool ok = l.lockedBySignal[static_cast<size_t>(s)] &&
				(cycle == 0 || (curCycle * unique == cycle * curUnique));
			locked += ok ? 1 : 0;
		}
		return total ? static_cast<double>(locked) / total : 0.0;
	}
	inline std::string LockSummary(const LaneResult& l, size_t maxEvents = 12)
	{
		std::string s;
		size_t n = 0;
		for (const LockEvent& e : l.locks)
		{
			if (n++ >= maxEvents)
			{
				s += " ...";
				break;
			}
			if (e.rephased)
				s += Fmt(" rephase@%d", e.signal);
			else if (e.locked)
				s += Fmt(" lock@%d(%d/%d %.3f)", e.signal, e.unique, e.cycle, e.filmHz);
			else
				s += Fmt(" unlock@%d", e.signal);
		}
		return s.empty() ? std::string(" none") : s;
	}
	inline int SignalOfClipFrame(const LaneResult& l, int clipFrame)
	{
		for (size_t s = 0; s < l.signalClipFrame.size(); ++s)
			if (l.signalClipFrame[s] >= clipFrame)
				return static_cast<int>(s);
		return static_cast<int>(l.signalClipFrame.size());
	}
	inline std::string Join(const std::vector<int>& v, size_t maxItems = 16)
	{
		std::string s;
		for (size_t i = 0; i < v.size() && i < maxItems; ++i)
			s += (i ? "," : "") + std::to_string(v[i]);
		if (v.size() > maxItems)
			s += Fmt(",...(%zu)", v.size());
		return s.empty() ? std::string("-") : s;
	}
	inline std::string JoinD(const std::vector<double>& v, size_t maxItems = 24)
	{
		std::string s;
		for (size_t i = 0; i < v.size() && i < maxItems; ++i)
			s += (i ? "," : "") + Fmt("%.2f", v[i]);
		if (v.size() > maxItems)
			s += Fmt(",...(%zu)", v.size());
		return s.empty() ? std::string("-") : s;
	}
	inline ContentClass FinalClass(const LaneResult& l)
	{
		return l.classes.empty() ? ContentClass::Unknown : l.classes.back().content;
	}
	inline double ClassShare(const LaneResult& l, ContentClass c)
	{
		long long total = 0;
		for (const auto& kv : l.classFrames)
			total += kv.second;
		auto it = l.classFrames.find(static_cast<int>(c));
		return total && it != l.classFrames.end() ? static_cast<double>(it->second) / total : 0.0;
	}
	inline std::string ClassSummary(const LaneResult& l)
	{
		std::string s;
		size_t n = 0;
		for (const ClassChange& c : l.classes)
		{
			if (n++ >= 8)
			{
				s += " ...";
				break;
			}
			s += Fmt(" %s@%.1fs", ClassName(c.content), c.t - 1.0);
		}
		return s;
	}

	// The expected (cycle, unique) of a pulldown carrying pictureHz in signalHz: the shortest
	// whole cycle with pictureHz / signalHz = unique / cycle within 0.11% (the 1000/1001 family's
	// 0.1% and no more: 23/24 is 0.17% off 24/25), up to one second.
	inline bool ExpectedCadence(double pictureHz, double signalHz, int& cycle, int& unique)
	{
		cycle = unique = 0;
		if (!(pictureHz > 0.0 && signalHz > pictureHz * 1.01))
			return false;
		const double ratio = pictureHz / signalHz;
		for (int c = 2; c <= static_cast<int>(std::ceil(signalHz)); ++c)
		{
			const int u = static_cast<int>(std::lround(ratio * c));
			if (u >= 1 && u < c && std::fabs(static_cast<double>(u) / c - ratio) <= ratio * 0.0011)
			{
				cycle = c;
				unique = u;
				return true;
			}
		}
		return false;
	}


	// The earliest signal frame >= from at which the delivered repeat flags allow a lock to
	// (cycle, unique): the last FramesToLock frames periodic with that cycle and `unique` new
	// pictures in it - what the content permits, whatever the detector does (-1: never).
	inline int EarliestPossibleLock(const LaneResult& l, int from, int last, int cycle, int unique, double signalHz)
	{
		const int n = static_cast<int>(l.signalClipFrame.size());
		std::vector<char> repeat(static_cast<size_t>(n), 0);
		size_t r = 0;
		for (int sIdx = 0; sIdx < n; ++sIdx)
		{
			// Direct model: signal frame == clip frame; repeatFlags lists clip frames in order.
			while (r < l.repeatFlags.size() && l.repeatFlags[r] < l.signalClipFrame[static_cast<size_t>(sIdx)])
				++r;
			repeat[static_cast<size_t>(sIdx)] = r < l.repeatFlags.size() && l.repeatFlags[r] == l.signalClipFrame[static_cast<size_t>(sIdx)] ? 1 : 0;
		}
		const int need = Pulldown::Detector::FramesToLock(cycle, signalHz);
		for (int sIdx = (std::max)(from, need - 1); sIdx <= last && sIdx < n; ++sIdx)
		{
			bool periodic = true;
			for (int i = sIdx - need + 1 + cycle; i <= sIdx && periodic; ++i)
				periodic = repeat[static_cast<size_t>(i)] == repeat[static_cast<size_t>(i - cycle)];
			if (!periodic)
				continue;
			int fresh = 0;
			for (int i = sIdx - cycle + 1; i <= sIdx; ++i)
				fresh += repeat[static_cast<size_t>(i)] ? 0 : 1;
			if (fresh == unique)
				return sIdx;
		}
		return -1;
	}

	// FilmClock spacing outliers (> 1 ms) outside the re-phase windows: from three cycles before a
	// re-phase (the contradiction and the tracking, M1: lateness reported as the most) to one
	// cycle after it, where the content's own phase shift moves the clock by design.
	inline int ClockOutliers(const LaneResult& l)
	{
		int n = 0;
		for (int sIdx : l.clockOver1msSignals)
		{
			bool inWindow = false;
			for (const LockEvent& e : l.locks)
				if (e.rephased && sIdx >= e.signal - 3 * e.cycle && sIdx <= e.signal + e.cycle)
					inWindow = true;
			n += inWindow ? 0 : 1;
		}
		return n;
	}

	inline bool Near(int frame, const std::vector<int>& list, int before, int after)
	{
		for (int f : list)
			if (f >= frame - before && f <= frame + after)
				return true;
		return false;
	}
	inline int CountOutside(const std::vector<int>& list, const std::vector<int>& expected, int before, int after)
	{
		int n = 0;
		for (int f : list)
			if (!Near(f, expected, after, before))
				++n;
		return n;
	}

	// Scene-cut matching: ground-truth cuts within 0.5 s merge (a burst is one event for a 0.5 s
	// sampler); a found cut matches a truth cut when it lies in [cut - 0.05, cut + 0.5 + latency + 0.15].
	struct CutMatch
	{
		int truth = 0, found = 0, matched = 0, falseCuts = 0;
		std::vector<double> missed, extra;
	};
	inline CutMatch MatchCuts(const std::vector<double>& truthRaw, const std::vector<double>& found)
	{
		CutMatch m;
		std::vector<double> truth;
		for (double t : truthRaw)
			if (truth.empty() || t - truth.back() > 0.5)
				truth.push_back(t);
		m.truth = static_cast<int>(truth.size());
		m.found = static_cast<int>(found.size());
		std::vector<char> used(found.size(), 0);
		for (double t : truth)
		{
			bool hit = false;
			for (size_t i = 0; i < found.size(); ++i)
				if (!used[i] && found[i] >= t - 0.05 && found[i] <= t + 0.5 + kTileLatency + 0.15)
				{
					used[i] = 1;
					hit = true;
					break;
				}
			if (hit)
				++m.matched;
			else
				m.missed.push_back(t);
		}
		for (size_t i = 0; i < found.size(); ++i)
			if (!used[i])
			{
				// A second detection inside a merged burst is not a false cut.
				bool inBurst = false;
				for (double t : truthRaw)
					if (found[i] >= t - 0.05 && found[i] <= t + 0.5 + kTileLatency + 0.15)
						inBurst = true;
				if (!inBurst)
				{
					++m.falseCuts;
					m.extra.push_back(found[i]);
				}
			}
		return m;
	}

	inline std::vector<double> Relative(const std::vector<double>& t)
	{
		std::vector<double> out;
		for (double x : t)
			out.push_back(x - 1.0);   // the capture clock starts at 1 s
		return out;
	}

	inline const std::vector<double>& DisplayRates()
	{
		static const std::vector<double> rates = { 60000.0 / 1001.0, 60.0, 50.0, 24000.0 / 1001.0, 120.0 };
		return rates;
	}
}

	TEST_CLASS(RealFootageSuiteTests)
	{
	public:
		// The manifest parses, every clip file exists, sizes were probed and the README's
		// events were all typed (the counts the README lists).
		TEST_METHOD(RealClipManifestIsComplete)
		{
			using namespace RealFootage;
			Suite& s = Suite::Get();
			if (s.Skip("RealClipManifestIsComplete"))
				return;
			int missing = 0, unsized = 0;
			for (const RealClip::Clip& c : s.manifest.clips)
			{
				if (!RealClip::FileExists(s.manifest.FullPath(c)))
					++missing;
				if (c.width <= 0 || c.height <= 0)
					++unsized;
			}
			auto count = [&](const char* id, const char* type) {
				const RealClip::Clip* c = s.manifest.Find(id);
				return c ? static_cast<int>(c->EventsOf(type).size()) : -1;
			};
			const int drops = count("dis_dropped_frames_23976_vfr", "drop");
			const int dups = count("dis_duplicated_frames_23976", "duplicate");
			const int breaks = count("cad_32_phase_jumps_5994p", "phase_break");
			const int slips = count("cad_22_50p_phase_breaks", "cadence_slip");
			const int segments = count("cad_32_30p_32_30p_5994p", "segment");
			s.Add("manifest", "all", "files, sizes, typed events", missing == 0 && unsized == 0 && drops == 5 && dups == 5 &&
				breaks == 5 && slips == 4 && segments == 4,
				Fmt("%zu clips, %d missing, %d unsized; drops %d dups %d phase breaks %d slips %d segments %d",
					s.manifest.clips.size(), missing, unsized, drops, dups, breaks, slips, segments));
			Assert::AreEqual(0, missing);
			Assert::AreEqual(0, unsized);
			Assert::AreEqual(5, drops);
			Assert::AreEqual(5, dups);
			Assert::AreEqual(5, breaks);
			Assert::AreEqual(4, slips);
			Assert::AreEqual(4, segments);
		}

		// Pulldown / cadence on every clip: frame-repeat pulldown locks to the right cadence within
		// FramesToLock + 2 cycles and stays locked (>= 98%); native, video, field-telecine and PsF
		// clips never lock; rate conversions lock at the expected cadence.
		TEST_METHOD(RealClipCadenceAndFilmClock)
		{
			using namespace RealFootage;
			Suite& s = Suite::Get();
			if (s.Skip("RealClipCadenceAndFilmClock"))
				return;
			int failures = 0;
			const auto clips = s.Clips([](const RealClip::Clip& c) {
				return c.group == "variants" || c.group == "timing/rates" || c.id == "cad_mixed_30p_overlay_on_24p_5994p";
			});
			for (const RealClip::Clip* c : clips)
			{
				RunSpec spec;
				spec.clip = c->id;
				spec.sourceResolution = c->group == "variants";
				const RunResult& r = s.Result(spec);
				if (!r.ok)
				{
					s.Add("cadence", c->id, "decode", false, r.error);
					++failures;
					continue;
				}
				const LaneResult& on = On(r);
				const double hz = c->RateHz();
				int cycle = 0, unique = 0;
				const bool pulldown = c->cadenceKind == "pulldown" || c->cadenceKind == "rate_conversion";
				const bool expectLock = pulldown && ExpectedCadence(c->PictureHz(), hz, cycle, unique);
				const int locks = CountLocks(on), unlocks = CountUnlocks(on);
				std::string numbers = Fmt("locks %d unlocks %d rephases %d; exact-repeat locks %lld; transport repeats %zu;%s",
					locks, unlocks, CountRephases(on), on.exactLocks, r.transportRepeats.size(), LockSummary(on).c_str());
				if (expectLock)
				{
					const LockEvent* first = FirstLock(on);
					const int within = Pulldown::Detector::FramesToLock(cycle, hz) + 2 * cycle;
					const double share = first ? LockedShare(on, first->signal, r.signalFrames - 1, cycle, unique) : 0.0;
					const bool rightCadence = first && first->cycle * unique == cycle * first->unique;
					const bool filmRate = first && std::fabs(first->filmHz - hz * unique / cycle) <= hz * unique / cycle * 0.002;
					const bool pass = first && first->signal <= within && rightCadence && filmRate && share >= 0.98;
					numbers = Fmt("expected %d/%d; first lock at %d (<= %d), locked share %.4f, film %.3f Hz; ", unique, cycle,
						first ? first->signal : -1, within, share, first ? first->filmHz : 0.0) + numbers;
					s.Add("cadence", c->id, "pulldown lock (C3/C5)", pass, numbers);
					failures += pass ? 0 : 1;
					// FilmClock (C7): monotonic, never early, spaced 1/f within 1 ms.
					const int outliers = ClockOutliers(on);
					const bool clock = on.clockNonMonotonic == 0 && on.clockMaxEarlyMs <= 1e-6 && outliers == 0;
					s.Add("cadence", c->id, "film clock (C7)", clock, Fmt("%lld samples, max spacing error %.3f ms, %lld over 1 ms (%d outside re-phase windows), %lld non-monotonic, max early %.3f ms",
						on.clockSamples, on.clockMaxSpacingErrorMs, on.clockSpacingOver1ms, outliers, on.clockNonMonotonic, on.clockMaxEarlyMs));
					failures += clock ? 0 : 1;
				}
				else
				{
					// Field telecine / PsF: no whole-frame repeats unless the film itself holds a frame
					// (animation on twos inside 2:3 fields gives one woven repeat in five): a lock is then
					// allowed only at the true film rate (the manifest's picture rate).
					bool pass = locks == 0;
					if (c->cadenceKind == "field_telecine_23" || c->cadenceKind == "psf_22")
					{
						pass = true;
						for (const LockEvent& e : on.locks)
							if (e.locked && e.relocked && std::fabs(e.filmHz - c->PictureHz()) > c->PictureHz() * 0.002)
								pass = false;
					}
					const char* what = c->cadenceKind == "field_telecine_23" ? "no lock but at the film rate (field telecine)" :
						c->cadenceKind == "psf_22" ? "no lock but at the film rate (PsF)" :
						c->cadenceKind == "mixed" ? "no lock (mixed cadence)" : "no false lock (C2)";
					s.Add("cadence", c->id, what, pass, numbers);
					failures += pass ? 0 : 1;
				}
				for (size_t i = 1; i < r.lanes.size(); ++i)
					s.Info("cadence", c->id, r.lanes[i].cfg.name, Fmt("locks %d unlocks %d rephases %d;%s", CountLocks(r.lanes[i]),
						CountUnlocks(r.lanes[i]), CountRephases(r.lanes[i]), LockSummary(r.lanes[i], 6).c_str()));
			}
			Assert::AreEqual(0, failures, L"see the RESULT rows");
		}

		// Cadence breaks: re-lock after each splice / phase break / slip at the logged frame.
		TEST_METHOD(RealClipCadenceBreaksAndSplices)
		{
			using namespace RealFootage;
			Suite& s = Suite::Get();
			if (s.Skip("RealClipCadenceBreaksAndSplices"))
				return;
			int failures = 0;
			// Film -> native 30p -> film -> 30p at 59.94: lock, unlock, re-lock to each segment's cadence.
			if (const RealClip::Clip* c = s.manifest.Find("cad_32_30p_32_30p_5994p"))
			{
				RunSpec spec;
				spec.clip = c->id;
				const RunResult& r = s.Result(spec);
				for (const LaneResult& lane : r.lanes)
				{
					std::string numbers;
					bool pass = true;
					for (const RealClip::Event& e : c->EventsOf("segment"))
					{
						const bool film = e.json->Str("kind") == "film32";
						const int cyc = film ? 5 : 2, uni = film ? 2 : 1;
						const int first = e.First(), last = e.Last();
						// The new cadence must hold from at most 3.5 s after the boundary (unlock on three
						// contradictions in 1 s, then a fresh 2 s lock).
						// C3 against what the delivered frames permit: the content's own static stretches
						// (identical consecutive pictures, more in native 30p video) break the pattern, so the
						// earliest possible lock is computed from the delivered repeat flags; the detector must
						// lock within one cycle of it (after unlocking the previous cadence).
						const int possible = EarliestPossibleLock(lane, first, last, cyc, uni, c->RateHz());
						const int settle = possible < 0 ? last - first : possible - first + cyc + 1;
						int relock = -1;
						for (const LockEvent& l : lane.locks)
							if (l.locked && l.signal >= first && l.signal <= last && l.cycle * uni == cyc * l.unique)
							{
								relock = l.signal;
								break;
							}
						const double share = relock >= 0 ? LockedShare(lane, relock, last, cyc, uni) : 0.0;
						const bool ok = relock >= 0 && relock - first <= settle && (film ? share >= 0.98 : share >= 0.9);
						pass = pass && ok;
						numbers += Fmt("[seg %d %s: lock +%d (possible +%d, <= %d), share %.3f] ", e.json->Int("index"), film ? "3:2" : "2:2(30p)",
							relock < 0 ? -1 : relock - first, possible < 0 ? -1 : possible - first, settle, share);
					}
					if (&lane == &r.lanes[0])
					{
						s.Add("splice", c->id, "re-lock per segment", pass, numbers + LockSummary(lane));
						failures += pass ? 0 : 1;
					}
					else
						s.Info("splice", c->id, lane.cfg.name, numbers);
				}
			}
			// Phase jumps (edits restarting the 2:3 phase) and 2:2 slips: with pulldown_fixes the lock
			// re-phases in place within two cycles of the break; off it unlocks and needs 2 s.
			for (const char* id : { "cad_32_phase_jumps_5994p", "cad_22_50p_phase_breaks" })
			{
				const RealClip::Clip* c = s.manifest.Find(id);
				if (!c)
					continue;
				RunSpec spec;
				spec.clip = c->id;
				const RunResult& r = s.Result(spec);
				std::vector<int> breaks;
				for (const RealClip::Event& e : c->EventsOf("phase_break"))
					breaks.push_back(e.Frame());
				for (const RealClip::Event& e : c->EventsOf("cadence_slip"))
					breaks.push_back(e.First() + (e.json->Int("shown") > e.json->Int("normal") ? e.json->Int("normal") : 0));
				const int cyc = c->holds.size() == 2 ? 5 : 2;
				for (const LaneResult& lane : r.lanes)
				{
					std::string numbers;
					int kept = 0, rephasedNear = 0, unlockedNear = 0;
					std::vector<int> lateness;
					for (int b : breaks)
					{
						int rephaseAt = -1, unlockAt = -1, relockAt = -1;
						for (const LockEvent& l : lane.locks)
						{
							if (l.signal < b || l.signal > b + static_cast<int>(3.5 * c->RateHz()))
								continue;
							if (l.rephased && rephaseAt < 0) rephaseAt = l.signal;
							if (!l.locked && unlockAt < 0) unlockAt = l.signal;
							if (l.locked && l.relocked && relockAt < 0) relockAt = l.signal;
						}
						if (unlockAt < 0) ++kept;
						else ++unlockedNear;
						if (rephaseAt >= 0 && rephaseAt - b <= 3 * cyc + 1) ++rephasedNear;
						lateness.push_back(rephaseAt >= 0 ? rephaseAt - b : relockAt >= 0 ? relockAt - b : -1);
						numbers += Fmt("[%d: %s %+d] ", b, rephaseAt >= 0 ? "rephase" : unlockAt >= 0 ? "unlock/relock" : "held",
							rephaseAt >= 0 ? rephaseAt - b : relockAt >= 0 ? relockAt - b : 0);
					}
					const double share = LockedShare(lane, FirstLock(lane) ? FirstLock(lane)->signal : 0, r.signalFrames - 1, cyc, cyc == 5 ? 2 : 1);
					numbers += Fmt("locked share %.4f;", share) + LockSummary(lane, 8);
					if (&lane == &r.lanes[0])
					{
						const bool pass = kept == static_cast<int>(breaks.size()) && rephasedNear == static_cast<int>(breaks.size()) && share >= 0.98;
						s.Add("splice", c->id, "re-phase in place at each break (D3)", pass, numbers);
						failures += pass ? 0 : 1;
					}
					else
						s.Info("splice", c->id, lane.cfg.name, numbers);
				}
			}
			Assert::AreEqual(0, failures, L"see the RESULT rows");
		}

		// Film / video / animation classification (ContentIntent over source_class) on every
		// variant and timing clip; the dark film variant is film; Meridian 59.94p is video.
		TEST_METHOD(RealClipContentClassification)
		{
			using namespace RealFootage;
			Suite& s = Suite::Get();
			if (s.Skip("RealClipContentClassification"))
				return;
			int failures = 0;
			const auto clips = s.Clips([](const RealClip::Clip& c) {
				return c.group == "variants" || c.group == "timing/motion" || c.group == "timing/cadence" ||
					c.id == "drift_10min_23976p_lowbitrate" || c.id == "rate_59.94" || c.id == "rate_25";
			});
			for (const RealClip::Clip* c : clips)
			{
				RunSpec spec;
				spec.clip = c->id;
				spec.sourceResolution = c->group == "variants";
				const RunResult& r = s.Result(spec);
				if (!r.ok)
					continue;
				const LaneResult& on = On(r);
				// The expectation from the manifest: the content and its picture rate (the owner's split:
				// up to 30.5 fps film-like, 50/60p video), and for synthetic pans their speed in picture
				// heights per second (sports: a sustained pan of at least ContentIntent::kWidePanHps).
				std::set<ContentClass> want;
				std::string wantCandidate;
				const double pictureHz = c->PictureHz();
				if (c->content == "video")
					want = { DejudderPolicy::IsFilmLike(pictureHz) ? ContentClass::Film : ContentClass::Video, ContentClass::Sports };
				else if (c->content == "synthetic-pan")
				{
					double pps = 0.0;
					for (const RealClip::Event& e : c->EventsOf("pan"))
						pps = e.json->Num("pixels_per_second");
					const double hps = pps / (std::max)(1, c->height);
					// A video-rate pan of at least kWidePanHps is a sports candidate, but a class changes only
					// at a cut after the first one (ContentIntentTests: "the first class (after the hold,
					// before any cut)" is video) - the cut-free pans stay video with sports pending.
					if (DejudderPolicy::IsFilmLike(pictureHz))
						want = { ContentClass::Film };
					else
					{
						want = { ContentClass::Video };
						if (hps >= ContentIntent::kWidePanHps)
							wantCandidate = "candidate sports";
					}
				}
				else if (c->content == "film-with-30p-overlay")
					// The whole picture changes 41.96 times a second (film 2:3 + graphic 2:2, not a pulldown
					// lock with the maximally-even rule): above the owner's film split, so video.
					want = { ContentClass::Video };
				else
					want = { ContentClass::Film };   // film, film-and-video30 (both film-like rates)
				const ContentClass final = FinalClass(on);
				ContentClass dominant = ContentClass::Unknown;
				double best = -1.0;
				for (const auto& kv : on.classFrames)
					if (kv.second > best) { best = static_cast<double>(kv.second); dominant = static_cast<ContentClass>(kv.first); }
				double wantShare = 0.0;
				for (ContentClass w : want)
					wantShare += ClassShare(on, w);
				const bool pass = want.count(final) && wantShare >= 0.8 &&
					(wantCandidate.empty() || on.classStats.compare(0, wantCandidate.size(), wantCandidate) == 0);
				std::string wantText;
				for (ContentClass w : want)
					wantText += std::string(wantText.empty() ? "" : "/") + ClassName(w);
				if (!wantCandidate.empty())
					wantText += " (" + wantCandidate + ")";
				s.Add("classifier", c->id, std::string("class ") + wantText + (c->dark ? " (dark film)" : ""), pass,
					Fmt("final %s, share of expected %.3f (dominant %s);%s; last %s", ClassName(final), wantShare, ClassName(dominant),
						ClassSummary(on).c_str(), on.classStats.c_str()));
				failures += pass ? 0 : 1;
				const LaneResult* off = LaneNamed(r, "cadence_class_fixes off");
				if (off)
					s.Info("classifier", c->id, "cadence_class_fixes off", Fmt("final %s;%s", ClassName(FinalClass(*off)), ClassSummary(*off).c_str()));
			}
			Assert::AreEqual(0, failures, L"see the RESULT rows");
		}

		// Drops, duplicates, gaps, the rate change and +-2 ms jitter: each flagged at its logged
		// frame (and nowhere else), vectors invalidated there, the hiccup repair offered for single
		// missed frames, never across a break.
		TEST_METHOD(RealClipDiscontinuities)
		{
			using namespace RealFootage;
			Suite& s = Suite::Get();
			if (s.Skip("RealClipDiscontinuities"))
				return;
			int failures = 0;
			auto check = [&](const std::string& clip, const std::string& what, bool pass, const std::string& numbers) {
				s.Add("discontinuity", clip, what, pass, numbers);
				failures += pass ? 0 : 1;
			};
			auto gapsOf = [](const LaneResult& l, const char* kind) {
				std::vector<int> v;
				for (const GapEvent& g : l.gaps)
					if (g.kind == kind)
						v.push_back(g.clipFrame);
				return v;
			};
			// Dropped frames: missed at each logged frame, one frame each, gap pair offered, vectors invalidated.
			if (const RealClip::Clip* c = s.manifest.Find("dis_dropped_frames_23976_vfr"))
			{
				RunSpec spec;
				spec.clip = c->id;
				const RunResult& r = s.Result(spec);
				std::vector<int> expected;
				for (const RealClip::Event& e : c->EventsOf("drop"))
					expected.push_back(e.Frame());
				for (const LaneResult& lane : r.lanes)
				{
					const std::vector<int> missed = gapsOf(lane, "missed"), breaks = gapsOf(lane, "break");
					bool onesEach = true;
					for (const GapEvent& g : lane.gaps)
						onesEach = onesEach && (g.kind != "missed" || g.frames == 1);
					const bool pass = missed == expected && breaks.empty() && onesEach && lane.invalidations == expected &&
						lane.gapFillPairs == expected && lane.timelineRestarts.empty();
					const std::string numbers = Fmt("expected %s; missed %s; breaks %s; invalidated %s; gap pairs %s; refused %s; timeline restarts %s; matcher restarts %lld",
						Join(expected).c_str(), Join(missed).c_str(), Join(breaks).c_str(), Join(lane.invalidations).c_str(),
						Join(lane.gapFillPairs).c_str(), Join(lane.refusedGapPairs).c_str(), Join(lane.timelineRestarts).c_str(), lane.matcherRestarts);
					if (&lane == &r.lanes[0])
						check(c->id, "missed frames flagged at the logged frames", pass, numbers);
					else
						s.Info("discontinuity", c->id, lane.cfg.name, numbers);
				}
			}
			// Duplicates: repeats at the logged frames, no gap, no lock.
			if (const RealClip::Clip* c = s.manifest.Find("dis_duplicated_frames_23976"))
			{
				RunSpec spec;
				spec.clip = c->id;
				const RunResult& r = s.Result(spec);
				std::vector<int> expected;
				for (const RealClip::Event& e : c->EventsOf("duplicate"))
					for (const RealClip::Json& x : e.json->Find("repeats")->a)
						expected.push_back(static_cast<int>(x.n));
				const LaneResult& on = On(r);
				const int outside = CountOutside(on.repeatFlags, expected, 0, 0);
				int found = 0;
				for (int f : expected)
					found += Near(f, on.repeatFlags, 0, 0) ? 1 : 0;
				const bool pass = found == static_cast<int>(expected.size()) && on.gaps.empty() && CountLocks(on) == 0 && on.invalidations.empty();
				check(c->id, "duplicates seen as repeats at the logged frames, no gap, no lock", pass,
					Fmt("expected %s; found %d/%zu; other repeat flags %d (static content); gaps %zu; locks %d; invalidations %zu",
						Join(expected).c_str(), found, expected.size(), outside, on.gaps.size(), CountLocks(on), on.invalidations.size()));
			}
			// Black and frozen 2.5 s gaps with regular timestamps: no timing event, no lock on 60 repeats.
			for (const char* id : { "dis_gap_black_2500ms_23976", "dis_gap_frozen_2500ms_23976" })
				if (const RealClip::Clip* c = s.manifest.Find(id))
				{
					RunSpec spec;
					spec.clip = c->id;
					spec.sourceResolution = true;
					const RunResult& r = s.Result(spec);
					const LaneResult& on = On(r);
					const bool pass = on.gaps.empty() && CountLocks(on) == 0 && on.invalidations.empty();
					check(c->id, "no timing event, the repeats never lock", pass,
						Fmt("gaps %zu; locks %d;%s; invalidations %zu; timeline restarts %zu; SR cuts %s",
							on.gaps.size(), CountLocks(on), LockSummary(on).c_str(), on.invalidations.size(), on.timelineRestarts.size(),
							JoinD(Relative(on.srOn.cutTimes)).c_str()));
				}
			// A 2.5 s timestamp gap: one break at the logged frame; vectors invalidated; no gap pair across it.
			if (const RealClip::Clip* c = s.manifest.Find("dis_gap_pts_2500ms_23976"))
			{
				RunSpec spec;
				spec.clip = c->id;
				const RunResult& r = s.Result(spec);
				int frame = -1;
				for (const RealClip::Event& e : c->EventsOf("pts_gap"))
					frame = e.Frame();
				for (const LaneResult& lane : r.lanes)
				{
					const std::vector<int> breaks = gapsOf(lane, "break"), missed = gapsOf(lane, "missed");
					const std::vector<int> expected = { frame };
					const bool pass = breaks == expected && missed.empty() && lane.invalidations == expected && lane.gapFillPairs.empty() &&
						lane.timelineRestarts == expected;
					const std::string numbers = Fmt("expected %d; breaks %s; missed %s; invalidated %s; gap pairs %s; timeline restarts %s",
						frame, Join(breaks).c_str(), Join(missed).c_str(), Join(lane.invalidations).c_str(), Join(lane.gapFillPairs).c_str(),
						Join(lane.timelineRestarts).c_str());
					if (&lane == &r.lanes[0])
						check(c->id, "timestamp gap: one break, vectors invalidated, no pair across", pass, numbers);
					else
						s.Info("discontinuity", c->id, lane.cfg.name, numbers);
				}
			}
			// +-2 ms jitter: no event of any kind.
			if (const RealClip::Clip* c = s.manifest.Find("dis_vfr_jitter_2ms_23976"))
			{
				RunSpec spec;
				spec.clip = c->id;
				const RunResult& r = s.Result(spec);
				for (const LaneResult& lane : r.lanes)
				{
					const bool pass = lane.gaps.empty() && lane.invalidations.empty() && lane.timelineRestarts.empty() &&
						lane.timelineReanchors.empty() && lane.adjacencyRefused.empty() && CountLocks(lane) == 0;
					const std::string numbers = Fmt("gaps %zu, invalidations %zu, timeline restarts %zu, re-anchors %zu, adjacency refusals %zu, locks %d",
						lane.gaps.size(), lane.invalidations.size(), lane.timelineRestarts.size(), lane.timelineReanchors.size(),
						lane.adjacencyRefused.size(), CountLocks(lane));
					if (&lane == &r.lanes[0])
						check(c->id, "+-2 ms jitter: no false event", pass, numbers);
					else
						s.Info("discontinuity", c->id, lane.cfg.name, numbers);
				}
			}
			// The rate change: captured, a mode change (resync) at the logged frame and nothing else;
			// played out at 59.94 Hz, the 3:2 lock is lost within 1 s of the change and never comes back.
			if (const RealClip::Clip* c = s.manifest.Find("dis_ratechange_23976_to_5994"))
			{
				int frame = -1;
				for (const RealClip::Event& e : c->EventsOf("rate_change"))
					frame = e.Frame();
				RunSpec spec;
				spec.clip = c->id;
				const RunResult& r = s.Result(spec);
				const LaneResult& on = On(r);
				const std::vector<int> resync = gapsOf(on, "resync");
				const bool pass = resync == std::vector<int>{ frame } && gapsOf(on, "break").empty() && gapsOf(on, "missed").empty() &&
					CountLocks(on) == 0;
				check(c->id, "capture: one mode change at the logged frame", pass,
					Fmt("expected %d; resync %s; breaks %zu; missed %zu; locks %d; class%s", frame, Join(resync).c_str(),
						gapsOf(on, "break").size(), gapsOf(on, "missed").size(), CountLocks(on), ClassSummary(on).c_str()));
				RunSpec player = spec;
				player.model = Model::Player;
				player.playerHz = 60000.0 / 1001.0;
				const RunResult& p = s.Result(player);
				const LaneResult& pl = On(p);
				const int changeSignal = SignalOfClipFrame(pl, frame);
				const LockEvent* first = FirstLock(pl);
				int unlockAt = -1, relockAfter = -1;
				for (const LockEvent& l : pl.locks)
				{
					if (!l.locked && l.signal >= changeSignal && unlockAt < 0) unlockAt = l.signal;
					if (l.locked && l.relocked && l.signal > changeSignal && relockAfter < 0) relockAfter = l.signal;
				}
				const bool ppass = first && first->unique * 5 == first->cycle * 2 && unlockAt >= 0 &&
					unlockAt - changeSignal <= static_cast<int>(std::ceil(p.signalHz)) && relockAfter < 0 &&
					LockedShare(pl, first->signal, changeSignal - 1, 5, 2) >= 0.98;
				check(c->id + "@player59.94", "played at 59.94: 3:2 lock, lost within 1 s of the change, no re-lock", ppass,
					Fmt("change at signal %d; first lock %d; unlock %d (+%d); re-lock after %d;%s; class%s", changeSignal,
						first ? first->signal : -1, unlockAt, unlockAt - changeSignal, relockAfter, LockSummary(pl).c_str(), ClassSummary(pl).c_str()));
			}
			Assert::AreEqual(0, failures, L"see the RESULT rows");
		}

		// Synthetic pans (and fades over a pan): constant motion, no event; the vectors stay valid.
		TEST_METHOD(RealClipMotionValidityOnPans)
		{
			using namespace RealFootage;
			Suite& s = Suite::Get();
			if (s.Skip("RealClipMotionValidityOnPans"))
				return;
			int failures = 0;
			for (const RealClip::Clip* c : s.Clips([](const RealClip::Clip& c) { return c.group == "timing/motion"; }))
			{
				RunSpec spec;
				spec.clip = c->id;
				spec.sourceResolution = !c->EventsOf("fade_in").empty();
				const RunResult& r = s.Result(spec);
				const LaneResult& on = On(r);
				const bool fade = !c->EventsOf("fade_in").empty();
				bool pass = on.gaps.empty() && on.invalidations.empty() && on.timelineRestarts.empty() && CountLocks(on) == 0 &&
					on.pairsRefused == 0;
				std::string numbers = Fmt("gaps %zu, invalidations %zu, timeline restarts %zu, locks %d, pairs %lld refused %lld, repeats %zu",
					on.gaps.size(), on.invalidations.size(), on.timelineRestarts.size(), CountLocks(on), on.pairs, on.pairsRefused, on.repeatFlags.size());
				if (fade)
				{
					numbers += Fmt("; SR cuts on the fades: %s (latch fixes off: %s)", JoinD(Relative(on.srOn.cutTimes)).c_str(),
						JoinD(Relative(on.srOff.cutTimes)).c_str());
					pass = pass && on.srOn.cutTimes.empty();
				}
				s.Add("motion", c->id, fade ? "pan with fades: no event, no cut" : "pan: no event, vectors valid", pass, numbers);
				failures += pass ? 0 : 1;
			}
			Assert::AreEqual(0, failures, L"see the RESULT rows");
		}

		// The alpha queue's presentation at 59.94 / 60 / 50 / 23.976 / 120 Hz: the 10-minute drift
		// clip never flushes (the 80-second regression) with queue_timing_fixes, captured directly and
		// played out with 3:2 (a film queue), the display clock 0 / +-100 ppm off.
		TEST_METHOD(RealClipDriftQueueNeverFlushes)
		{
			using namespace RealFootage;
			Suite& s = Suite::Get();
			if (s.Skip("RealClipDriftQueueNeverFlushes"))
				return;
			int failures = 0;
			const RealClip::Clip* c = s.manifest.Find("drift_10min_23976p_lowbitrate");
			if (!c)
				return;
			for (int model = 0; model < 2; ++model)
			{
				RunSpec spec;
				spec.clip = c->id;
				if (model == 1)
				{
					spec.model = Model::Player;
					spec.playerHz = 60000.0 / 1001.0;
				}
				const RunResult& r = s.Result(spec);
				const LaneResult& on = On(r);
				const std::string name = c->id + (model == 1 ? "@player59.94" : "");
				if (model == 1)
				{
					const LockEvent* first = FirstLock(on);
					const double share = first ? LockedShare(on, first->signal, r.signalFrames - 1, 5, 2) : 0.0;
					const bool pass = first && share >= 0.98 && on.clockNonMonotonic == 0 && ClockOutliers(on) == 0;
					s.Add("drift", name, "3:2 held for 10 min, film clock spaced 1/f", pass,
						Fmt("first lock %d, share %.4f, unlocks %d, clock max spacing error %.3f ms over %lld samples",
							first ? first->signal : -1, share, CountUnlocks(on), on.clockMaxSpacingErrorMs, on.clockSamples));
					failures += pass ? 0 : 1;
				}
				for (double display : DisplayRates())
				{
					int flushes = 0;
					size_t maxDepth = 0;
					std::string numbers;
					for (double ppm : { 0.0, 100.0, -100.0 })
					{
						const QueueSimResult q = SimulateQueue(on.kept, display, ppm, true);
						flushes += q.flushes;
						maxDepth = (std::max)(maxDepth, q.maxDepth);
						numbers += Fmt("[%+.0f ppm: flushes %d holds %d settled holds %d max depth %zu] ", ppm, q.flushes, q.holds, q.settledHolds, q.maxDepth);
					}
					const QueueSimResult old = SimulateQueue(on.kept, display, 0.0, false);
					numbers += Fmt("queue_timing_fixes off (0 ppm): flushes %d, first %.1f s, median interval %.1f s",
						old.flushes, old.flushTimes.empty() ? -1.0 : old.flushTimes.front(), old.medianFlushInterval);
					const bool pass = flushes == 0 && maxDepth <= 7;
					s.Add("drift", name, Fmt("no flush at %.3f Hz display", display), pass, numbers);
					failures += pass ? 0 : 1;
				}
			}
			Assert::AreEqual(0, failures, L"see the RESULT rows");
		}

		// Source resolution: the tracker's cuts at the real cut points (scene ground truth), no cut
		// on fades / flashes / long takes; native footage never declared lower; an upscaled long
		// take crossfades after the wait from its confirmation; an upscaled 3-minute trim engages at a cut.
		TEST_METHOD(RealClipSourceResolutionCutsAndLatch)
		{
			using namespace RealFootage;
			Suite& s = Suite::Get();
			if (s.Skip("RealClipSourceResolutionCutsAndLatch"))
				return;
			int failures = 0;
			for (const RealClip::Clip* c : s.Clips([](const RealClip::Clip& c) { return c.group == "variants"; }))
			{
				RunSpec spec;
				spec.clip = c->id;
				spec.sourceResolution = true;
				const RunResult& r = s.Result(spec);
				const LaneResult& on = On(r);
				const std::vector<double> found = Relative(on.srOn.cutTimes), foundOff = Relative(on.srOff.cutTimes),
					foundCutOff = Relative(on.srCutOff.cutTimes);
				int engages = 0;
				for (const SrChange& ch : on.srOn.changes)
					engages += ch.toY < ch.fromY ? 1 : 0;
				std::string declared, declaredCutOff;
				for (size_t d = 0; d < on.srOn.declared.size(); ++d)
					declared += Fmt(" %.1fs->%.2f (%s)", on.srOn.declared[d].first - 1.0, on.srOn.declared[d].second,
						d < on.srOn.declaredEvidence.size() ? on.srOn.declaredEvidence[d].c_str() : "");
				for (const auto& d : on.srCutOff.declared)
					declaredCutOff += Fmt(" %.1fs->%.2f", d.first - 1.0, d.second);
				// R3-2 (h3-latch): native footage never even declares a lower source.
				const bool neverDeclared = on.srOn.declared.empty();
				if (c->hasCuts)
				{
					// The scene-score list is a lower bound of the real cuts (it misses hard cuts in action:
					// ToS 51.5 s), and fast whip pans decorrelate every tile at the 0.5 s spacing, so cuts not
					// in the list are reported, not failed. Asserted: native footage never engages auto, most
					// listed cuts are found (recall >= 0.6), and the latch fixes find at least as many as before.
					const CutMatch m = MatchCuts(c->cuts, found), mo = MatchCuts(c->cuts, foundOff), mc = MatchCuts(c->cuts, foundCutOff);
					const double recall = m.truth ? static_cast<double>(m.matched) / m.truth : 1.0;
					const bool pass = engages == 0 && neverDeclared && recall >= 0.6 && m.matched >= mo.matched;
					s.Add("source-resolution", c->id, "cuts found, native never declared or engaged", pass,
						Fmt("truth %d, found %d, matched %d (recall %.2f), unlisted %d; missed %s; cut fixes off: found %d matched %d unlisted %d, declared%s; latch fixes off: found %d matched %d; engages %d; declared%s; flashes %llu, covered steps %llu; readings native %d lower %d blank %d",
							m.truth, m.found, m.matched, recall, m.falseCuts, JoinD(m.missed).c_str(), mc.found, mc.matched, mc.falseCuts,
							declaredCutOff.empty() ? " never" : declaredCutOff.c_str(), mo.found, mo.matched, engages,
							declared.empty() ? " never" : declared.c_str(), on.srOn.flashes, on.srOn.coveredSteps, on.srOn.nativeReadings,
							on.srOn.lowerReadings, on.srOn.blank));
					failures += pass ? 0 : 1;
				}
				else
				{
					// A cut-free shot (by scene score): native never engaged; its cuts are reported (a long
					// take's dolly to an extreme close-up and a credits scroll change every tile).
					// h3-latch (R3-1): a cut-free shot has no cut (a long take's dolly to an extreme close-up, a pass
					// through a dark occluder and a credits scroll change every tile but no two consecutive frames).
					const bool pass = engages == 0 && neverDeclared && found.empty();
					s.Add("source-resolution", c->id, "cut-free shot: no cut, native never declared or engaged", pass,
						Fmt("cuts %s (cut fixes off %s; latch fixes off %s); engages %d; declared%s; flashes %llu, covered steps %llu; native %d lower %d blank %d",
							JoinD(found).c_str(), JoinD(foundCutOff).c_str(), JoinD(foundOff).c_str(), engages, declared.empty() ? " never" : declared.c_str(),
							on.srOn.flashes, on.srOn.coveredSteps, on.srOn.nativeReadings, on.srOn.lowerReadings, on.srOn.blank));
					failures += pass ? 0 : 1;
				}
			}
			// Synthetic variants through ffmpeg filters (the size is kept): a 2:1 upscale of the long take
			// (Lanczos from half size), a flash inside the long take, a 2:1 upscale of a 3-minute trim.
			const std::string upscale = "scale=iw/2:ih/2:flags=area,scale=iw*2:ih*2:flags=lanczos";
			if (const RealClip::Clip* c = s.manifest.Find("cosmos_longtake_23976p_44s"))
			{
				RunSpec spec;
				spec.clip = c->id;
				spec.filter = upscale;
				spec.label = c->id + "@upscale2x";
				spec.sourceResolution = true;
				const RunResult& r = s.Result(spec);
				const LaneResult& on = On(r);
				const SrChange* engage = nullptr;
				for (const SrChange& ch : on.srOn.changes)
					if (ch.toY < ch.fromY) { engage = &ch; break; }
				const double declared = on.srOn.firstDeclared - 1.0;
				const double at = engage ? engage->t - 1.0 : -1.0;
				const double expected = declared + kTileLatency + 20.0;
				const bool pass = engage && engage->why.find("crossfade") != std::string::npos && std::fabs(engage->fade - 1.5) < 1e-9 &&
					declared >= 9.5 && declared <= 11.5 && std::fabs(at - expected) <= 0.7;
				// R3-1 (fixed by h3-latch, source_resolution_cut_fixes): the long take's dolly to an extreme close-up
				// at 28.5 s read as a cut, so auto engaged there mid-shot before the crossfade was due.
				const SrChange* engageCutOff = nullptr;
				for (const SrChange& ch : on.srCutOff.changes)
					if (ch.toY < ch.fromY) { engageCutOff = &ch; break; }
				s.Add("source-resolution", spec.label, "long take crossfades after the wait", pass,
					Fmt("declared %.2f s (expect ~10), engaged %.2f s (expect %.2f) by '%s', fade %.2f s, ratio %.2f; cuts %s; cut fixes off: engaged %.2f s by '%s', cuts %s; latch fixes off: %zu changes, declared %.2f",
						declared, at, expected, engage ? engage->why.c_str() : "never", engage ? engage->fade : 0.0, engage ? engage->toY : 1.0,
						JoinD(Relative(on.srOn.cutTimes)).c_str(), engageCutOff ? engageCutOff->t - 1.0 : -1.0,
						engageCutOff ? engageCutOff->why.c_str() : "never", JoinD(Relative(on.srCutOff.cutTimes)).c_str(),
						on.srOff.changes.size(), on.srOff.firstDeclared < 0 ? -1.0 : on.srOff.firstDeclared - 1.0));
				failures += pass ? 0 : 1;
				// The part fixed here (the evidence standard at the stream start) is asserted.
				const bool evidence = declared >= 9.5 && declared <= 11.5;
				s.Add("source-resolution", spec.label, "evidence standard kept at the stream start (10 s in one shot)", evidence,
					Fmt("declared %.2f s", declared));
				failures += evidence ? 0 : 1;
				// Flashes: three 2-frame flashes in the long take.
				RunSpec flash;
				flash.clip = c->id;
				flash.filter = "eq=brightness=0.25:enable='between(n,240,241)+between(n,480,481)+between(n,720,721)'";
				flash.label = c->id + "@flashes";
				flash.sourceResolution = true;
				const RunResult& fr = s.Result(flash);
				RunSpec base;
				base.clip = c->id;
				base.sourceResolution = true;
				const RunResult& br = s.Result(base);
				// The flashes add no cut to the long take's own (its camera moves are reported above).
				const bool fpass = On(fr).srOn.cutTimes == On(br).srOn.cutTimes;
				s.Add("source-resolution", flash.label, "flashes add no cut", fpass,
					Fmt("cuts %s vs %s without the flashes (latch fixes off: %s vs %s)", JoinD(Relative(On(fr).srOn.cutTimes)).c_str(),
						JoinD(Relative(On(br).srOn.cutTimes)).c_str(), JoinD(Relative(On(fr).srOff.cutTimes)).c_str(),
						JoinD(Relative(On(br).srOff.cutTimes)).c_str()));
				failures += fpass ? 0 : 1;
			}
			if (const RealClip::Clip* c = s.manifest.Find("tos_24p_3min"))
			{
				RunSpec spec;
				spec.clip = c->id;
				spec.filter = upscale;
				spec.label = c->id + "@upscale2x";
				spec.sourceResolution = true;
				const RunResult& r = s.Result(spec);
				const LaneResult& on = On(r);
				const SrChange* engage = nullptr;
				for (const SrChange& ch : on.srOn.changes)
					if (ch.toY < ch.fromY) { engage = &ch; break; }
				double sinceCut = 1e9;
				if (engage)
					for (double cut : c->cuts)
						if (engage->t - 1.0 >= cut - 0.05)
							sinceCut = (std::min)(sinceCut, engage->t - 1.0 - cut);
				const bool pass = engage && (engage->why.find("cut") != std::string::npos || engage->why.find("black") != std::string::npos ||
					engage->why.find("low-detail") != std::string::npos || engage->why.find("crossfade") != std::string::npos) &&
					std::fabs(engage->toY - 0.5) <= 0.06;
				s.Add("source-resolution", spec.label, "upscale engages (at a cut / black / crossfade) at ~0.5", pass,
					Fmt("declared %.2f s, engaged %.2f s by '%s' (%.2f s after a true cut), ratio %.3f, changes %zu; latch fixes off: changes %zu",
						on.srOn.firstDeclared - 1.0, engage ? engage->t - 1.0 : -1.0, engage ? engage->why.c_str() : "never", sinceCut,
						engage ? engage->toY : 1.0, on.srOn.changes.size(), on.srOff.changes.size()));
				failures += pass ? 0 : 1;
			}
			Assert::AreEqual(0, failures, L"see the RESULT rows");
		}

		// Player-made pulldown (a player outputting film to 59.94 / 60 / 50 Hz, bit-exact repeats as
		// over HDMI): every film source locks, the dark film too; Meridian 59.94p never locks.
		TEST_METHOD(RealClipPlayerPulldown)
		{
			using namespace RealFootage;
			Suite& s = Suite::Get();
			if (s.Skip("RealClipPlayerPulldown"))
				return;
			int failures = 0;
			struct P { const char* id; double hz; };
			const P runs[] = { { "tos_23976p_3min", 60000.0 / 1001.0 }, { "tos_24p_3min", 60.0 }, { "tos_25p_speedup_3min", 50.0 },
				{ "tos_dark_2stops_23976p_3min", 60000.0 / 1001.0 }, { "cosmos_23976p_3min", 60000.0 / 1001.0 },
				{ "cosmos_longtake_23976p_44s", 60000.0 / 1001.0 }, { "meridian_2997p_3min", 60000.0 / 1001.0 },
				{ "meridian_5994p_3min", 60000.0 / 1001.0 } };
			for (const P& p : runs)
			{
				const RealClip::Clip* c = s.manifest.Find(p.id);
				if (!c || !RealClip::FileExists(s.manifest.FullPath(*c)))
					continue;
				RunSpec spec;
				spec.clip = c->id;
				spec.model = Model::Player;
				spec.playerHz = p.hz;
				const RunResult& r = s.Result(spec);
				const LaneResult& on = On(r);
				int cycle = 0, unique = 0;
				const bool expectLock = ExpectedCadence(c->RateHz(), p.hz, cycle, unique);
				const std::string name = c->id + Fmt("@player%.2f", p.hz);
				if (expectLock)
				{
					const LockEvent* first = FirstLock(on);
					const int within = Pulldown::Detector::FramesToLock(cycle, p.hz) + 2 * cycle;
					const double share = first ? LockedShare(on, first->signal, r.signalFrames - 1, cycle, unique) : 0.0;
					const bool clock = on.clockNonMonotonic == 0 && ClockOutliers(on) == 0 && on.clockMaxEarlyMs <= 1e-6;
					const bool pass = first && first->signal <= within && share >= 0.98 && clock;
					s.Add("player-pulldown", name, Fmt("%d/%d lock, film clock", unique, cycle), pass,
						Fmt("first lock %d (<= %d), share %.4f, unlocks %d, rephases %d, clock max spacing error %.3f ms (%lld over 1 ms); class%s",
							first ? first->signal : -1, within, share, CountUnlocks(on), CountRephases(on), on.clockMaxSpacingErrorMs,
							on.clockSpacingOver1ms, ClassSummary(on).c_str()));
					failures += pass ? 0 : 1;
				}
				else
				{
					const bool pass = CountLocks(on) == 0;
					s.Add("player-pulldown", name, "no lock (video at the signal rate)", pass, Fmt("locks %d;%s", CountLocks(on), LockSummary(on).c_str()));
					failures += pass ? 0 : 1;
				}
			}
			Assert::AreEqual(0, failures, L"see the RESULT rows");
		}

		// The queue at every display rate for every clip run already decoded (fixes on vs off):
		// periodic flushes are the regression; logged for the comparison table.
		TEST_METHOD(RealClipQueueAcrossDisplayRates)
		{
			using namespace RealFootage;
			Suite& s = Suite::Get();
			if (s.Skip("RealClipQueueAcrossDisplayRates"))
				return;
			int failures = 0;
			const char* ids[] = { "tos_23976p_3min", "tos_24p_3min", "tos_25p_speedup_3min", "tos_32_5994p_3min", "tos_22_50p_3min",
				"meridian_5994p_3min", "meridian_2997p_3min", "rate_120", "dis_dropped_frames_23976_vfr", "dis_vfr_jitter_2ms_23976" };
			for (const char* id : ids)
			{
				const RealClip::Clip* c = s.manifest.Find(id);
				if (!c || !RealClip::FileExists(s.manifest.FullPath(*c)))
					continue;
				RunSpec spec;
				spec.clip = id;
				spec.sourceResolution = c->group == "variants";
				const RunResult& r = s.Result(spec);
				const LaneResult& on = On(r);
				std::string numbers;
				int flushes = 0, oldFlushes = 0;
				for (double display : DisplayRates())
				{
					const QueueSimResult q = SimulateQueue(on.kept, display, 0.0, true, 5, 60.0);
					const QueueSimResult o = SimulateQueue(on.kept, display, 0.0, false, 5, 60.0);
					flushes += q.flushes;
					oldFlushes += o.flushes;
					numbers += Fmt("[%.3f: %d/%d] ", display, q.flushes, o.flushes);
				}
				// A dropped capture frame leaves the queue a frame short, never a backlog: still no flush.
				const bool pass = flushes == 0;
				s.Add("queue", id, "no flush at 59.94/60/50/23.976/120 (fixes / off)", pass, numbers + Fmt("total %d vs %d off", flushes, oldFlushes));
				failures += pass ? 0 : 1;
			}
			Assert::AreEqual(0, failures, L"see the RESULT rows");
		}
	};

	// Minimal reproductions of the real bugs the suite found (no footage needed).
	TEST_CLASS(RealFootageRegressionTests)
	{
	public:
		// cad_mixed_30p_overlay_on_24p_5994p: 24p film (2:3) with a 30p graphic (2:2) in one picture at
		// 59.94 Hz repeats whole frames 3 times in 10 (holds 2,2,2,1,1,1,1 rotated) - even by
		// IsEvenPattern, so the detector locked "41.958p film (7 new of 10)". With cadence_class_fixes
		// (maximallyEvenPattern) no lock; plain 3:2 still locks.
		TEST_METHOD(MixedCadenceOverlayNeverLocksAsFilm)
		{
			const double hz = 60000.0 / 1001.0;
			for (int fixes = 0; fixes < 2; ++fixes)
			{
				Pulldown::Settings settings;
				settings.maximallyEvenPattern = fixes == 1;
				Pulldown::Detector mixed, plain;
				mixed.SetSettings(settings);
				plain.SetSettings(settings);
				int mixedLocks = 0, plainLocks = 0;
				for (int k = 0; k < 600; ++k)
				{
					// Base film frame at slot k: frame j starts at floor(j * 5 / 2); overlay picture every 2 slots.
					const bool baseNew = (k % 5 == 0) || (k % 5 == 2);
					const bool overlayNew = k % 2 == 0;
					mixedLocks += mixed.Next(k > 0 && !baseNew && !overlayNew, false, hz).relocked ? 1 : 0;
					plainLocks += plain.Next(k > 0 && !baseNew, false, hz).relocked ? 1 : 0;
				}
				Assert::AreEqual(1, plainLocks, L"3:2 locks either way");
				if (fixes)
					Assert::AreEqual(0, mixedLocks, L"no film lock on a mixed cadence");
				else
					Assert::AreEqual(1, mixedLocks, L"the old rule locked 7 of 10");
			}
			// Every real pulldown pattern is maximally even.
			for (const char* p : { "NRNRR", "NRRNR", "NR", "NNNNR", "NRRNRNRRNR" })
				Assert::IsTrue(Pulldown::IsMaximallyEvenPattern(p));
			Assert::IsFalse(Pulldown::IsMaximallyEvenPattern("NRNRNNNNNR"));
		}

		// cosmos_longtake_23976p_44s: the tracker's cut judge started a stream with a motion level of
		// 0, so the opening seconds of a moving shot read as cuts (two by 1.5 s): three "shots" met
		// minimumShots and an upscaled long take was declared at 1.5 s instead of after 10 s of
		// evidence. With the latch fixes the first shape starts at cutMotionAfterCut, as after a cut.
		TEST_METHOD(StreamStartMotionIsNotACut)
		{
			auto frame = [](double gain, double offset, double mix, unsigned seed) {
				SourceResolution::FrameReading f;
				f.tiles = 8;
				f.pattern.resize(32);
				f.patternFlat.assign(32, 0);
				unsigned x = 12345u, z = seed;
				for (size_t i = 0; i < 32; ++i)
				{
					x = x * 1103515245u + 12345u;
					z = z * 1103515245u + 12345u;
					const double a = 0.2 + 0.4 * ((x >> 16) & 0x7fff) / 32767.0;
					const double b = 0.2 + 0.4 * ((z >> 16) & 0x7fff) / 32767.0;
					f.pattern[i] = gain * a + mix * b + offset;
				}
				double sum = 0.0;
				for (double v : f.pattern)
					sum += v;
				f.mean = sum / 32.0;
				f.signature = static_cast<double>(seed);   // a changing frame (not a repeat)
				return f;
			};
			// A moving shot: the second analysed frame decorrelates by ~0.5 with a level jump of ~0.35
			// against the first (a cut at a motion level of 0, not at cutMotionAfterCut).
			const SourceResolution::FrameReading a = frame(1.0, 0.0, 0.0, 1), b = frame(0.5, 0.2, 0.3, 7);
			double decorrelation = 0.0, share = 0.0, jump = 0.0;
			Assert::IsTrue(SourceResolution::PatternChange(a.pattern, a.patternFlat, b.pattern, b.patternFlat, a.blackLevel, a.noiseLevel,
				decorrelation, share, &jump));
			Assert::IsTrue(decorrelation > 0.4 && decorrelation < 0.8 && jump >= 0.2, L"the shape of a moving shot, not of a cut");
			SourceResolution::Tracker tracker;
			tracker.Update(a, 0.0);
			tracker.Update(b, 0.5);
			Assert::AreEqual(0ull, tracker.Cuts(), L"no cut in the opening motion of a stream");
			// After a discontinuity the same.
			tracker.Discontinuity("test");
			tracker.Update(a, 3.0);
			tracker.Update(b, 3.5);
			Assert::AreEqual(0ull, tracker.Cuts());
			// A real cut (an unrelated picture) is still one.
			const SourceResolution::FrameReading c = frame(0.0, 0.1, 1.0, 99);
			tracker.Update(c, 4.0);
			// (source_resolution_cut_fixes: confirmed when the next analysed frame shows the new shot persists.)
			tracker.Update(frame(0.0, 0.1001, 1.0, 99), 4.5);
			Assert::AreEqual(1ull, tracker.Cuts(), L"an unrelated shape is a cut");
			Assert::AreEqual(4.0, tracker.LastCutSeconds(), 1e-9, L"at its own moment");
		}
	
		// R3-1 (bug hunt round 3, fixed by h3-latch: source_resolution_cut_fixes): a camera move inside a
		// long take is not a cut. Needs the clip library (skipped without it).
		TEST_METHOD(LongTakeCameraMoveIsNotACut)
		{
			using namespace RealFootage;
			Suite& s = Suite::Get();
			if (s.Skip("LongTakeCameraMoveIsNotACut"))
				return;
			RunSpec spec;
			spec.clip = "cosmos_longtake_23976p_44s";
			spec.sourceResolution = true;
			const RunResult& r = s.Result(spec);
			Assert::IsTrue(r.ok);
			Assert::AreEqual(size_t{ 0 }, On(r).srOn.cutTimes.size(), L"the scene-cut-free long take has no tracker cut");
		}
};

	// h3-latch (source_resolution_cut_fixes): the cut judge's rules on synthetic readings (no footage).
	TEST_CLASS(SourceResolutionCutFixesTests)
	{
		// A 32-value tile pattern (8 tiles x 4 quadrants) between 0.2 and 0.6 from a seed.
		static std::vector<double> Pattern(unsigned seed)
		{
			std::vector<double> p(32);
			unsigned x = seed * 2654435761u + 12345u;
			for (double& v : p)
			{
				x = x * 1103515245u + 12345u;
				v = 0.2 + 0.4 * ((x >> 16) & 0x7fff) / 32767.0;
			}
			return p;
		}
		static std::vector<double> Mix(const std::vector<double>& a, const std::vector<double>& b, double m)
		{
			std::vector<double> p(a.size());
			for (size_t i = 0; i < a.size(); ++i) p[i] = (1.0 - m) * a[i] + m * b[i];
			return p;
		}
		// A luma distribution centred on `bin`.
		static std::vector<double> Distribution(int bin)
		{
			std::vector<double> d(SourceResolution::kDistributionBins, 0.0);
			for (int i = -2; i <= 2; ++i) d[static_cast<size_t>((std::max)(0, (std::min)(SourceResolution::kDistributionBins - 1, bin + i)))] += 0.2;
			return d;
		}
		static SourceResolution::FrameReading Reading(const std::vector<double>& pattern, const std::vector<double>& distribution, double signature,
			double ratio = 1.0, bool informative = false)
		{
			SourceResolution::FrameReading f;
			f.tiles = 8;
			f.informativeTiles = informative ? 8 : 0;
			f.pattern = pattern;
			f.patternFlat.assign(pattern.size(), 0);
			f.distribution = distribution;
			double sum = 0.0;
			for (double v : pattern) sum += v;
			f.mean = sum / pattern.size();
			f.signature = signature;   // a changing frame (not a repeat)
			f.ratio = ratio;
			return f;
		}
		// Small per-frame noise so consecutive frames are new pictures.
		static std::vector<double> Jitter(const std::vector<double>& p, unsigned seed, double amount = 0.002)
		{
			std::vector<double> q = p;
			unsigned x = seed * 747796405u + 1u;
			for (double& v : q)
			{
				x = x * 1103515245u + 12345u;
				v += amount * (((x >> 16) & 0x7fff) / 32767.0 - 0.5);
			}
			return q;
		}
		static SourceResolution::Tracker::Settings Fixes(bool cutFixes)
		{
			SourceResolution::Tracker::Settings s;
			s.latchFixes = true;
			s.cutFixes = cutFixes;
			return s;
		}

		// A 2-frame flash (lightning: half the picture lights up) inside a shot: the look-ahead's judge
		// cancels it within the flash window, and a real cut later stands once the window has passed.
		TEST_METHOD(FrameJudgeFlashIsCancelledAndCutStands)
		{
			const SourceResolution::Tracker::Settings s = Fixes(true);
			SourceResolution::FrameCutJudge judge;
			const std::vector<double> a = Pattern(1), b = Pattern(2);
			std::vector<double> flash = a;
			for (size_t i = 0; i < 16; ++i) flash[i] += 0.35;
			const double per = 1.0 / 24.0, black = 64.0 / 1023.0, noise = 2.0 / 1023.0;
			uint64_t seq = 100;
			for (unsigned i = 0; i < 24; ++i) judge.Frame(++seq, Jitter(a, i), black, noise, per, s);
			const uint64_t flashAt = seq + 1;
			Assert::IsTrue(judge.Frame(++seq, flash, black, noise, per, s), L"the flash's first frame is a candidate");
			judge.Frame(++seq, Jitter(flash, 99), black, noise, per, s);
			for (unsigned i = 0; i < 24; ++i) judge.Frame(++seq, Jitter(a, 200 + i), black, noise, per, s);
			Assert::IsTrue(judge.Cancelled(flashAt), L"back to the shape before within 0.25 s: a flash");
			SourceResolution::FrameEvidence e = judge.Evidence(101, seq);
			Assert::IsTrue(e.covered);
			Assert::IsFalse(e.cut, L"a flash is no cut");
			const uint64_t cutAt = seq + 1;
			for (unsigned i = 0; i < 24; ++i) judge.Frame(++seq, Jitter(b, 300 + i), black, noise, per, s);
			Assert::IsFalse(judge.Cancelled(cutAt));
			e = judge.Evidence(cutAt - 1, cutAt);
			Assert::IsTrue(e.covered && e.cut && !e.unresolved, L"a cut to another shot stands");
			// Not covered: a gap in the frames shown to the look-ahead.
			judge.Frame(seq + 10, b, black, noise, per, s);
			Assert::IsFalse(judge.Evidence(seq, seq + 10).covered);
		}

		// A camera passing through a dark occluder: half the picture darkens (a candidate), then the
		// whole picture is dark for a few frames, then the shot again. Not a cut: the candidate ran into
		// darkness and the shot after it is judged against the shape before.
		TEST_METHOD(FrameJudgeDarkOccluderIsNotACut)
		{
			const SourceResolution::Tracker::Settings s = Fixes(true);
			SourceResolution::FrameCutJudge judge;
			const double black = 64.0 / 1023.0, noise = 2.0 / 1023.0, per = 1.0 / 24.0;
			const std::vector<double> a = Pattern(30);
			std::vector<double> half = a, dark = a;
			for (size_t i = 0; i < 16; ++i) half[i] = black + (half[i] - black) * 0.2;
			for (double& v : dark) v = black + (v - black) * 0.01;
			uint64_t seq = 0;
			for (unsigned i = 0; i < 24; ++i) judge.Frame(++seq, Jitter(a, i), black, noise, per, s);
			const uint64_t start = seq;
			const bool candidate = judge.Frame(++seq, half, black, noise, per, s);
			for (int i = 0; i < 6; ++i) judge.Frame(++seq, dark, black, noise, per, s);
			for (unsigned i = 0; i < 24; ++i) judge.Frame(++seq, Jitter(a, 400 + i), black, noise, per, s);
			Assert::IsTrue(candidate, L"the darkening is a candidate at first");
			const SourceResolution::FrameEvidence e = judge.Evidence(start, seq);
			Assert::IsTrue(e.covered);
			Assert::IsFalse(e.cut, L"through darkness back to the same shot: no cut");
		}

		// A camera move: the shape changes completely over 0.5 s but little between two frames. The
		// look-ahead sees no cut, so the tracker counts none; with the cut fixes off the same tiles are a cut.
		TEST_METHOD(CameraMoveWithLookAheadIsNotACut)
		{
			const std::vector<double> a = Pattern(3), b = Pattern(4), c = Pattern(5);
			SourceResolution::FrameCutJudge judge;
			const SourceResolution::Tracker::Settings s = Fixes(true);
			uint64_t seq = 0;
			std::vector<std::vector<double>> frames;
			// 12 frames per 0.5 s at 24 fps: a -> b -> c, morphing frame by frame.
			for (int i = 0; i <= 36; ++i)
				frames.push_back(i <= 12 ? a : i <= 24 ? Mix(a, b, (i - 12) / 12.0) : Mix(b, c, (i - 24) / 12.0));
			for (size_t i = 0; i < frames.size(); ++i) judge.Frame(++seq, Jitter(frames[i], static_cast<unsigned>(i)), 64.0 / 1023.0, 2.0 / 1023.0, 1.0 / 24.0, s);
			Assert::AreEqual(size_t{ 0 }, judge.Candidates().size(), L"no jump between consecutive frames");
			const SourceResolution::EvidenceSource look = [&judge](uint64_t from, uint64_t to) { return judge.Evidence(from, to); };
			auto run = [&](bool cutFixes) {
				SourceResolution::Tracker t(Fixes(cutFixes));
				// A still opening (its motion level settles), then the move at the 0.5 s tile spacing.
				for (int k = 0; k < 8; ++k)
					t.Update(Reading(Jitter(a, 500 + k), Distribution(8), 900.0 + k), 6.0 + 0.5 * k);
				const int at[] = { 0, 12, 24, 36 };
				for (int k = 0; k < 4; ++k)
				{
					const int i = at[k];
					t.Update(Reading(Jitter(frames[static_cast<size_t>(i)], static_cast<unsigned>(i)), Distribution(8 + 4 * k), 1.0 + k), 10.0 + 0.5 * k, false,
						static_cast<uint64_t>(i + 1), &look);
				}
				return t.Cuts();
			};
			Assert::AreEqual(0ull, run(true), L"camera move with the look-ahead: no cut");
			Assert::IsTrue(run(false) >= 1ull, L"cut fixes off: the move reads as a cut (the bug)");
		}

		// Tiles alone (no look-ahead): a flash caught by an analysed frame returns at the next one -
		// no cut; a real cut is confirmed one analysed frame later and keeps its own moment.
		TEST_METHOD(TileFlashIsNotACutAndCutKeepsItsMoment)
		{
			const std::vector<double> a = Pattern(6), b = Pattern(7);
			std::vector<double> flash = a;
			for (size_t i = 0; i < 16; ++i) flash[i] += 0.45;
			SourceResolution::Tracker t(Fixes(true));
			double time = 0.0;
			unsigned n = 0;
			auto feed = [&](const std::vector<double>& p, int bin) { t.Update(Reading(Jitter(p, n), Distribution(bin), ++n), time); time += 0.5; };
			for (int i = 0; i < 12; ++i) feed(a, 8);
			feed(flash, 14);
			feed(a, 8);
			for (int i = 0; i < 6; ++i) feed(a, 8);
			Assert::AreEqual(0ull, t.Cuts(), L"a flash that returns is no cut");
			Assert::IsTrue(t.Flashes() >= 1ull);
			const double cutTime = time;
			feed(b, 20);
			Assert::AreEqual(0ull, t.Cuts(), L"deferred: confirmed at the next analysed frame");
			Assert::AreEqual(2ull, t.CutMoments(), L"but a switch moment at once (the flash was one too)");
			feed(b, 20);
			Assert::AreEqual(1ull, t.Cuts());
			Assert::AreEqual(cutTime, t.LastCutSeconds(), 1e-9, L"the cut keeps the moment of its first frame");
			// The same with the cut fixes off: the flash counts.
			SourceResolution::Tracker old(Fixes(false));
			time = 0.0;
			for (unsigned i = 0; i < 12; ++i) { old.Update(Reading(Jitter(a, i), Distribution(8), 1000.0 + i), time); time += 0.5; }
			old.Update(Reading(flash, Distribution(14), 2000.0), time); time += 0.5;
			old.Update(Reading(Jitter(a, 77), Distribution(8), 2001.0), time);
			Assert::IsTrue(old.Cuts() >= 1ull, L"cut fixes off: the flash reads as cuts");
		}

		// Shot / reverse-shot at one level: decorrelation 0.78-0.90 with no level jump, under the 0.9 alone
		// threshold. With the cut fixes a cut when the next analysed frame confirms the new shape persists.
		TEST_METHOD(SameLevelShotReverseShotIsACut)
		{
			const std::vector<double> a = Pattern(8), b = Pattern(9);
			double best = 0.62;
			for (double m = 0.4; m <= 0.8; m += 0.01)
			{
				double d = 0.0, sh = 0.0, jump = 0.0;
				SourceResolution::PatternChange(a, std::vector<char>(32, 0), Mix(a, b, m), std::vector<char>(32, 0), 64.0 / 1023.0, 2.0 / 1023.0, d, sh, &jump);
				if (d >= 0.84) { best = m; break; }
			}
			const std::vector<double> r = Mix(a, b, best);
			double d = 0.0, sh = 0.0, jump = 0.0;
			Assert::IsTrue(SourceResolution::PatternChange(a, std::vector<char>(32, 0), r, std::vector<char>(32, 0), 64.0 / 1023.0, 2.0 / 1023.0, d, sh, &jump));
			Assert::IsTrue(d >= 0.75 && d < 0.9 && jump < 0.2, L"a near miss of the old rule");
			auto run = [&](bool cutFixes) {
				SourceResolution::Tracker t(Fixes(cutFixes));
				double time = 0.0;
				unsigned n = 0;
				for (int i = 0; i < 12; ++i) { t.Update(Reading(Jitter(a, n), Distribution(10), ++n), time); time += 0.5; }
				for (int i = 0; i < 4; ++i) { t.Update(Reading(Jitter(r, n), Distribution(13), ++n), time); time += 0.5; }
				return t.Cuts();
			};
			Assert::AreEqual(1ull, run(true));
			Assert::AreEqual(0ull, run(false), L"the latch fixes judge missed it");
		}

		// A scroll or pan inside one lighting: the tiles' shape changes, the luma distribution does not.
		TEST_METHOD(UnchangedDistributionIsNotACut)
		{
			const std::vector<double> a = Pattern(10), b = Pattern(11);
			SourceResolution::Tracker t(Fixes(true)), old(Fixes(false));
			double time = 0.0;
			for (unsigned i = 0; i < 12; ++i)
			{
				const auto f = Reading(Jitter(a, i), Distribution(9), 1.0 + i);
				t.Update(f, time); old.Update(f, time); time += 0.5;
			}
			for (unsigned i = 0; i < 3; ++i)
			{
				const auto f = Reading(Jitter(b, 50 + i), Distribution(9), 100.0 + i);
				t.Update(f, time); old.Update(f, time); time += 0.5;
			}
			Assert::AreEqual(0ull, t.Cuts());
			Assert::AreEqual(1ull, old.Cuts(), L"cut fixes off: a cut");
		}

		// A pass through a dark occluder: the frame's level above black is near zero; it is not compared,
		// and the shot on both sides is the same shape.
		TEST_METHOD(DarkPassIsNotACut)
		{
			const std::vector<double> a = Pattern(12);
			std::vector<double> dark = Pattern(13);
			for (double& v : dark) v = 64.0 / 1023.0 + (v - 0.2) * 0.02;
			SourceResolution::Tracker t(Fixes(true));
			double time = 0.0;
			for (unsigned i = 0; i < 12; ++i) { t.Update(Reading(Jitter(a, i), Distribution(9), 1.0 + i), time); time += 0.5; }
			t.Update(Reading(dark, Distribution(2), 50.0), time); time += 0.5;
			for (unsigned i = 0; i < 4; ++i) { t.Update(Reading(Jitter(a, 60 + i), Distribution(9), 60.0 + i), time); time += 0.5; }
			Assert::AreEqual(0ull, t.Cuts());
		}

		// R3-2: fast cuts with a few stray lower readings of native footage reach three shots in about a
		// second; with the cut fixes the shots also need half the evidence time, so nothing is declared.
		TEST_METHOD(FastCutsOnStrayReadingsDoNotDeclare)
		{
			auto run = [](bool cutFixes) {
				SourceResolution::Tracker t(Fixes(cutFixes));
				double time = 0.0;
				unsigned n = 0;
				for (int i = 0; i < 20; ++i) { t.Update(Reading(Jitter(Pattern(20), n), Distribution(8), ++n, 1.0, true), time); time += 0.5; }
				double lowest = 1.0;
				// Four short shots (2 analysed frames each) reading 0.75, then native again.
				for (unsigned shot = 0; shot < 4; ++shot)
					for (int i = 0; i < 2; ++i)
					{
						t.Update(Reading(Jitter(Pattern(21 + shot), n), Distribution(10 + 5 * static_cast<int>(shot)), ++n, 0.75, true), time);
						time += 0.5;
						lowest = (std::min)(lowest, t.Ratio());
					}
				return lowest;
			};
			Assert::AreEqual(1.0, run(true), 1e-9, L"never declared");
			Assert::IsTrue(run(false) < 1.0, L"cut fixes off: declared on the shots (the near miss)");
		}
	};

	// h3-latch K3 (active_picture_startup_hold): black until the first crop decision, the remembered
	// crop of the same source after a reset, a bounded hold, nothing to wait for without auto crop.
	TEST_CLASS(StartupCropHoldTests)
	{
		TEST_METHOD(BlackUntilTheFirstDecisionThenRememberedAfterAReset)
		{
			StartupCropHold hold;
			StartupCropHold::Key key;
			key.width = 3840; key.height = 2160; key.format = 7;
			StartupCropHold::Rect crop;
			crop.left = 0; crop.top = 276; crop.right = 3840; crop.bottom = 1884;
			const StartupCropHold::Rect none;
			hold.Begin(10.0);
			// Start-up: no decision for 1.0 s - black, not the uncropped picture.
			for (double t = 10.0; t < 11.0; t += 1.0 / 24.0)
				Assert::IsTrue(hold.Frame(key, t, true, false, false, none) == StartupCropHold::Action::Black);
			// The first decision ends the hold at once.
			Assert::IsTrue(hold.Frame(key, 11.0, true, true, true, crop) == StartupCropHold::Action::Present);
			Assert::IsFalse(hold.Holding());
			// A live queue reset with the same source: the remembered crop at once, until the new decision.
			hold.Begin(20.0);
			Assert::IsTrue(hold.Frame(key, 20.0, true, false, false, none) == StartupCropHold::Action::Remembered);
			Assert::AreEqual(276, hold.RememberedCrop().top);
			Assert::IsTrue(hold.Frame(key, 20.5, true, true, true, crop) == StartupCropHold::Action::Present);
			// A different source after a reset: black again (its bars are not known).
			StartupCropHold::Key other = key;
			other.height = 1080; other.width = 1920;
			hold.Begin(30.0);
			Assert::IsTrue(hold.Frame(other, 30.0, true, false, false, none) == StartupCropHold::Action::Black);
		}

		TEST_METHOD(FullRasterNeedsNoHoldAndTheHoldIsBounded)
		{
			StartupCropHold hold;
			StartupCropHold::Key key;
			key.width = 1920; key.height = 1080; key.format = 3;
			const StartupCropHold::Rect none;
			hold.Begin(0.0);
			// No decision ever (the detector cannot decide): the picture still appears after the bound.
			Assert::IsTrue(hold.Frame(key, 0.1, true, false, false, none) == StartupCropHold::Action::Black);
			Assert::IsTrue(hold.Frame(key, hold.MaximumHold() + 0.01, true, false, false, none) == StartupCropHold::Action::Present);
			Assert::AreEqual(1ull, hold.Timeouts());
			// A trusted full raster is a decision; after a reset of the same source there is nothing to hold.
			Assert::IsTrue(hold.Frame(key, 5.0, true, true, false, none) == StartupCropHold::Action::Present);
			hold.Begin(6.0);
			Assert::IsTrue(hold.Frame(key, 6.0, true, false, false, none) == StartupCropHold::Action::Present);
			// Automatic crop off: nothing to wait for; switch off: never black.
			hold.Begin(7.0);
			Assert::IsTrue(hold.Frame(StartupCropHold::Key{}, 7.0, false, false, false, none) == StartupCropHold::Action::Present);
			hold.SetEnabled(false);
			hold.Begin(8.0);
			Assert::IsTrue(hold.Frame(StartupCropHold::Key{}, 8.0, true, false, false, none) == StartupCropHold::Action::Present);
		}
	};

	// The gate-verify cut method (docs/agent-reports/gate-verify.md section 2; tools/score_film_cuts.py):
	// 2-fps 8-bit 4:2:0 y4m samples of a film (VP_TEST_CUT_FILMS, ';'-separated paths), the base 4 x 2 grid
	// plus the dense rows at the picture's tile size, each frame 0.5 s apart through two trackers: the cut
	// fixes on (column cut_fixes) and off (column cut_old = the latch fixes' judge). A deferred cut is
	// written on the step it belongs to (LastCutSeconds). Writes <film>.cuts.csv for the scoring script.
	TEST_CLASS(SourceResolutionFilmCuts)
	{
		TEST_METHOD(RealFilmCutDecisionsCutFixes)
		{
			const std::string films = RealClip::Env("VP_TEST_CUT_FILMS");
			if (films.empty()) { Logger::WriteMessage("RealFilmCutDecisionsCutFixes: VP_TEST_CUT_FILMS not set: skipped\n"); return; }
			size_t from = 0;
			while (from < films.size())
			{
				const size_t end = films.find(';', from);
				const std::string path = films.substr(from, end == std::string::npos ? std::string::npos : end - from);
				from = end == std::string::npos ? films.size() : end + 1;
				if (path.empty()) continue;
				FILE* file = nullptr;
				Assert::AreEqual(0, static_cast<int>(fopen_s(&file, path.c_str(), "rb")), L"film not found");
				char header[512] = {};
				Assert::IsTrue(fgets(header, sizeof(header), file) != nullptr);
				int W = 0, H = 0;
				for (const char* p = header; *p; ++p)
				{
					if (p[0] == ' ' && p[1] == 'W') W = atoi(p + 2);
					if (p[0] == ' ' && p[1] == 'H') H = atoi(p + 2);
				}
				Assert::IsTrue(W > 0 && H > 0);
				const double black = 64.0 / 1023.0;
				const int tile = SourceResolution::TileSize(H);
				int base = 0;
				const std::vector<SourceResolution::TilePlace> places = SourceResolution::PlaceTiles(0, 0, W, H, tile, true, &base);
				SourceResolution::Tracker::Settings on, off;
				on.latchFixes = off.latchFixes = true;
				on.cutFixes = true;
				off.cutFixes = false;
				SourceResolution::Tracker fixes(on), old(off);
				std::vector<uint8_t> y(static_cast<size_t>(W) * H), chroma(static_cast<size_t>(W) * H / 2);
				std::vector<float> values(static_cast<size_t>(tile) * tile);
				struct Row { double seconds, dec, share, jump, mean; int judged, cutOn, cutOff; };
				std::vector<Row> rows;
				SourceResolution::FrameReading last;
				bool haveLast = false;
				unsigned long long cutsOn = 0, cutsOff = 0;
				char line[256];
				for (int k = 0;; ++k)
				{
					if (!fgets(line, sizeof(line), file) || strncmp(line, "FRAME", 5) != 0) break;
					if (fread(y.data(), 1, y.size(), file) != y.size() || fread(chroma.data(), 1, chroma.size(), file) != chroma.size()) break;
					std::vector<SourceResolution::Spectrum> spectra(places.size());
					for (size_t p = 0; p < places.size(); ++p)
					{
						for (int r = 0; r < tile; ++r)
							for (int c = 0; c < tile; ++c)
								values[static_cast<size_t>(r) * tile + c] = static_cast<float>(y[static_cast<size_t>(places[p].y0 + r) * W + places[p].x0 + c] * 4) / 1023.0f;
						SourceResolution::Analyse(values.data(), tile, tile, spectra[p], 1023);
					}
					SourceResolution::Thresholds th;
					SourceResolution::SetTileCentres(places, th);
					th.signatureTiles = base;
					th.blackLevel = black;
					const SourceResolution::FrameReading f = SourceResolution::ReadFrame(spectra, th);
					const double t = k * 0.5;
					fixes.Update(f, t);
					old.Update(f, t);
					Row row{ t, -1.0, -1.0, -1.0, f.mean, 0, 0, 0 };
					if (haveLast)
						row.judged = SourceResolution::PatternChange(last.pattern, last.patternFlat, f.pattern, f.patternFlat, f.blackLevel, f.noiseLevel, row.dec, row.share, &row.jump) ? 1 : 0;
					rows.push_back(row);
					if (fixes.Cuts() != cutsOn)
					{
						cutsOn = fixes.Cuts();
						const double at = fixes.LastCutSeconds();
						for (size_t i = rows.size(); i-- > 0;)
							if (std::fabs(rows[i].seconds - at) < 1e-6) { rows[i].cutOn = 1; break; }
					}
					if (old.Cuts() != cutsOff) { cutsOff = old.Cuts(); rows.back().cutOff = 1; }
					last = f;
					haveLast = true;
				}
				fclose(file);
				std::string csv = "step,seconds,decorrelation,share,jump,judged,cut_fixes,cut_old,mean\n";
				for (size_t i = 0; i < rows.size(); ++i)
				{
					snprintf(line, sizeof(line), "%zu,%.2f,%.4f,%.4f,%.4f,%d,%d,%d,%.4f\n", i, rows[i].seconds, rows[i].dec, rows[i].share, rows[i].jump,
						rows[i].judged, rows[i].cutOn, rows[i].cutOff, rows[i].mean);
					csv += line;
				}
				const std::string out = RealClip::Env("VP_TEST_CUT_OUT").empty() ? path + ".h3.cuts.csv" : RealClip::Env("VP_TEST_CUT_OUT") + "\\" + path.substr(path.find_last_of("\\/") + 1) + ".h3.cuts.csv";
				FILE* o = nullptr;
				if (fopen_s(&o, out.c_str(), "wb") == 0 && o) { fwrite(csv.data(), 1, csv.size(), o); fclose(o); }
				snprintf(line, sizeof(line), "RESULT %s: %zu analysed frames; cuts with cut fixes %llu, without %llu; flashes %llu; csv %s\n", path.c_str(), rows.size(), cutsOn, cutsOff, fixes.Flashes(), out.c_str());
				Logger::WriteMessage(line);
			}
		}
	};
}
