// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// ChainEstimator (SID-C2b, doc 78 sections 2, 3, 8, 9b): the worker that runs the C1 chain units on the sampled lines of the source
// frames and publishes P14g `source_geometry`.
//
//   SourceFrameTap sink "chain"   (frame-analysis worker)   wants a plane ONLY when an evaluation is due (the governor's interval);
//        |                                                   copies just the LineSampler lines (integer codes, ~0.4 MB at 4K),
//        |                                                   attaches the consumed events + the source time, releases the plane.
//        v   bounded queue (1-2 jobs, NEWEST WINS, counted)
//   estimator worker thread       proposer -> verifier -> power probe -> ChainEvidence -> AxisReader (per axis: confident / UNSURE) -> HeldState::Publish.
//
// Nothing here runs on the render thread beyond three leaf-lock notes (NoteFrame, NoteEvent, NoteDiscontinuity) and the sealed-record
// read. The evaluation is a pure function of (lines, source time, flags): the same job sequence gives byte-identical records (the tests
// pin it; a dumped evidence ring replays exactly). Wall-clock reads only decide WHEN an evaluation runs (the CPU-share governor) and
// the reported cost; tests inject the clock.
//
// Event contract: the monitor calls NoteEvent(Cut | Blank | Fade, seq) when its analysis sees one; the event is consumed by the FIRST
// evaluation of a frame with seq >= the event's seq (each cut is a shot for ChainEvidence's per-shot cap and the flags go to the log; a job
// replaced by a newer one hands its consumed events to the replacement; while both axes are confident an event pulls the next evaluation
// forward, at most once per second). The monitor reports events from its own analysis worker, so an evaluation that already ran may miss
// one: the next evaluation carries it (the readings are not timed by events any more). A discontinuity
// (NoteDiscontinuity, a stream-key change, a format change) resets everything at once and publishes a reset record whose evalSeq is the
// era's first sequence (one past the newest announced frame). A repeat (NoteRepeat) is
// skipped without consuming the due slot. The active area and the source time of a frame come from NoteFrame (the monitor's per-frame
// call on the render thread); a frame whose time is not known yet is extrapolated from the newest two, else skipped (counted).
//
// Revert window (owner-approved): NoteEvent(ClearWindow, seq) asks for the evidence window (both axes, ChainEvidence::Reset + the proposers, the
// path of a resync reset without the reader reset and without a reset record: the reading is a pure function of the evidence, so the next
// evaluation reads UNSURE until new evidence builds) to be cleared before the first evaluation of a frame with seq >= the event's. The monitor
// sends it at the END of a mixed (menu / overlay) episode that lasted at least one acquiring interval and at the first cut after one (WindowClearPolicy.h, debounced: a one-frame blip clears nothing); the estimator also clears at
// its own first cut after a HOLD evaluation. Black frames (Blank) and ordinary cuts never clear. A clear event pulls the next evaluation
// forward while the readings are confident, like any event, so the reading is re-made at once.
//
// Modes: Off (nothing runs, the sink is unregistered, the held record is reset), Shadow (published + logged, the classical decision
// stands; the record carries kFlagShadow), On (published; the consensus may use it). `Original pathway` = Off.
//
// Header + .cpp, C++14. One persistent worker thread, created the first time the estimator runs, joined by Stop() / the destructor.
#include "ChainEvidence.h"
#include "ChainLatch.h"
#include "ChainEvidenceLog.h"
#include "ChainGovernor.h"
#include "ChainLines.h"
#include "ChainReading.h"
#include "ChainStatus.h"
#include "DescaleVerifier.h"
#include "PolyphaseProposer.h"
#include "PowerProbe.h"

#include <VP-V/core/LaneInterfaces.h>
#include <VP-V/modules/input/source_analysis/identity/SourceGeometryProduct.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace SourceIdentity
{
	namespace Chain
	{
		enum class EstimatorMode : int { Off = 0, Shadow = 1, On = 2 };
		// MenuHold / AbrStep (SID r2): ignored unless the safety rules are on, so off stays byte identical.
		enum class EstimatorEvent : int { Cut = 0, Blank = 1, Fade = 2, ClearWindow = 3, MenuHold = 4, AbrStep = 5 };

		// Every tunable is hidden tuning (doc 78 section 7; the README "Tuning (not in Config)" table), none is read from a config file yet.
		struct EstimatorSettings
		{
			SamplerSettings sampler;                                     // lines while acquiring (32 + 32)
			SamplerSettings samplerLatched = SamplerSettings::Latched(); // lines while both axes are confident (16 + 16)
			VerifierSettings verifier;
			ProposerSettings proposer;
			ProbeSettings probe;
			EvidenceSettings evidence;
			LatchSettings latch;                 // SID r2 R5 / R7: the asymmetric upscale latch (inert unless safetyRules)
			bool safetyRules = false;            // SID r2 rules R2 + R3 + R5 + R7 as one switch, default OFF (SetSafetyRules)
			ReadingSettings reading;
			GovernorSettings governor;
			EvidenceLogSettings log;
			int queueDepth = 1;                  // jobs waiting for the worker (1..2); a newer job replaces the oldest, counted
			int outputStepOverride = 0;          // 0 = detect from the samples; 4 = an 8-bit picture; 1 = 10-bit
			int latchedExactLines = 16;          // exact-stage lines while both axes are confident (a confident n always gets the exact stage)
			int confidentAxisExactLines = Narrowing::kConfidentAxisExactLines;   // lever, OFF (0 = all sampled lines): exact-stage lines of ONE
			                                                                   // confident axis while the other is still acquiring; SID-OPT step 3
			int clockRing = 64;                  // frames of (seq, source time, active area) remembered
			int eventCap = 64;                   // pending events kept (oldest dropped)
			int repeatCap = 16;                  // pending repeat marks kept
		};

		// Exact-stage lines of one axis for this evaluation (0 = all sampled lines): latchedExactLines while both axes are confident and the
		// monitor holds nothing out (latchedMode); confidentAxisExactLines for a confident axis whose OTHER axis is not (the lever, off by
		// default); otherwise all lines. Both confident without latchedMode (N2: natives held out) re-tests on all lines.
		inline int ExactLinesFor(const EstimatorSettings& s, bool latchedMode, bool thisAxisConfident, bool otherAxisConfident)
		{
			if (latchedMode)
				return s.latchedExactLines;
			if (s.confidentAxisExactLines > 0 && thisAxisConfident && !otherAxisConfident)
				return s.confidentAxisExactLines;
			return 0;
		}

		struct EstimatorCounters
		{
			uint64_t evaluations = 0;        // evaluations run
			uint64_t delivered = 0;          // frames taken (lines copied and queued)
			uint64_t dropped = 0;            // queued jobs replaced by a newer one (newest wins)
			uint64_t repeatsSkipped = 0;     // frames skipped as cadence repeats
			uint64_t noClock = 0;            // frames skipped: no source time known
			uint64_t unusable = 0;           // frames whose active area leaves no room for the lines
			uint64_t stale = 0;              // queued jobs of an earlier era / a stopped estimator
			uint64_t discontinuities = 0;    // resets
			uint64_t windowClears = 0;       // evidence-window clears (ClearWindow events consumed, first cut after a HOLD): no reset record
			uint64_t fadeShots = 0;          // SID r2 N2 (F1): evidence shots started by a black / fade without a cut (safety rules on only)
			LatchCounters latch;             // SID r2: latch entries / moves / releases by cause, held back, non-conventional (zero while the rules are off)
		};

		// What one evaluation tested, per axis and candidate (the calibration dump of the harness reads it; any other observer may).
		struct CandidateDetail
		{
			int nProposed = 0;           // the size asked for
			int n = 0;                   // the refined size (exact stage) or the coarse-refined size
			bool exact = false;          // verified on full lines (stat is the exact statistic), else coarse only
			double stat = 0.0;           // T of the stage named by `exact`
			double coarseStat = 0.0;
			int kernel = -1;             // best kernel (KernelBank index; exact stage only)
			double kernelMargin = 0.0;
			double familyMargin = 0.0;
			int linesGated = 0;          // informative lines (the line gate; exact stage only, else 0)
			bool informative = false;    // the estimator would use this candidate (judged, enough gated lines, not clustered away)
			bool evJudged = false;       // ChainEvidence::Judge reached its statistic (exact stage; the gate and the spatial agreement passed)
			double evStat = 0.0;         // that statistic: the trimmed mean over the gated AND unmasked lines (the LLR input); valid when evJudged
		};
		struct AxisDetail
		{
			bool have = false;           // the axis had lines
			int N = 0;                   // active-area size along the axis
			int lines = 0;               // lines offered
			bool probeRan = false, probeSeen = false;
			double floorCodes = 0.0;     // the (dof-normalised) rounding floor handed to the evidence stage
			int exactLines = 0;          // lines of the exact stage (0 = no candidate reached it)
			int operatorBuilds = 0;      // verifier work of this evaluation on this axis (cold = builds > 0)
			int solves = 0;              // line solves executed
			int solvesReused = 0;        // line solves read back from an identical solve of the same call
			std::vector<CandidateDetail> cands;
		};
		struct EvaluationDetail
		{
			uint64_t seq = 0;
			int64_t timeUs = 0;
			uint64_t streamKey = 0;
			uint32_t shotId = 0;         // the estimator's own detected shot id (the cuts it consumed since the start / reset)
			int stepCodes = 1;           // 4 = an 8-bit picture, 1 = 10-bit
			double cachedMB = 0.0;       // operator caches (main + power probe) plus tap memos after this evaluation, MiB
			AxisDetail axis[2];
		};
		// Called on the thread that processes the evaluation (the worker; the caller in synchronous mode), after the evidence stage.
		typedef std::function<void(const EvaluationDetail&)> EvaluationObserver;

		class ChainEstimator
		{
		public:
			explicit ChainEstimator(const EstimatorSettings& settings = EstimatorSettings(), NowMsFn nowMs = NowMsFn(),
				std::shared_ptr<IEvidenceFs> fs = std::shared_ptr<IEvidenceFs>(), StampFn stamp = StampFn());
			~ChainEstimator();
			ChainEstimator(const ChainEstimator&) = delete;
			ChainEstimator& operator=(const ChainEstimator&) = delete;

			// ---- lifecycle (any thread) ----------------------------------------------------------------------------------
			// Off stops the work (the worker idles, the sink is unregistered, the held record is reset); a later On / Shadow starts
			// from a clean state. Cheap and never waits for an evaluation.
			void SetMode(EstimatorMode mode);
			EstimatorMode Mode() const;
			// The Original pathway: forces the estimator (and the evidence log) off.
			void SetOriginalPathway(bool original);
			// Mode != Off and not Original (the tap may still be unattached).
			bool Running() const;
			// Registers the sink "chain" (with its due predicate) for the renderer `owner`; re-registers when the registration was lost.
			bool AttachTap(const void* owner);
			void DetachTap();
			bool TapAttached() const;
			// Joins the worker. After Stop the estimator is inert (the destructor calls it).
			void Stop();

			// ---- inputs from the monitor (short leaf locks) -------------------------------------------------------------
			// The render thread, every source frame: the frame's source time (the frame clock's domain) and its active rectangle
			// (w <= 0: the whole frame).
			void NoteFrame(uint64_t seq, double seconds, int x, int y, int w, int h);
			void NoteEvent(EstimatorEvent kind, uint64_t seq);   // Cut | Blank | Fade | ClearWindow (see "Revert window" above)
			void NoteRepeat(uint64_t seq);
			// N2 (S1 bound): the monitor left a native-looking informative frame out on the axes of `axisMask` (bit 0 = H, bit 1 = V) because
			// this estimator's record reads them confidently below native. While such notes keep coming the estimator evaluates at its
			// ACQUIRING cadence (an evaluation due at the long confident interval is pulled forward to one acquiring interval after the last
			// start), so its own reading re-tests quickly. Leaf atomics; inert when the estimator does not run.
			void NoteNativeHeld(unsigned axisMask);
			// The planned interval (seconds) until the next evaluation as of now (the governor's plan, shortened by NoteNativeHeld).
			double IntervalSec() const;
			// The planned ACQUIRING interval (seconds) as of the last evaluation (the governor's cost-aware plan, never the long confident one): the unit of the monitor's mixed-episode debounce.
			double AcquiringIntervalSec() const;
			// Any thread: a resync, seek, format change, FULL era: everything restarts.
			void NoteDiscontinuity(const char* cause);
			// The Indicator's FULL / era key; a change is a discontinuity.
			void SetStreamKey(uint64_t key);
			// The classical decision's ratios (1 = native) for the evidence log's disagreement flags.
			void SetClassical(bool valid, double ratioH, double ratioV);
			// Free text for the evidence segments (input size, rate, bit depth, colour format, transfer).
			void SetFormatInfo(const std::string& text);

			// ---- SID r2 safety rules (default OFF; the Monitor setter / key plumbing is pending, SID-5) ----------------------------
			// R2 conventional geometry only, R3 convention-pair prior, R5 asymmetric latch, R7 no latch into a menu. Applied at the next clean start.
			void SetSafetyRules(bool on);
			bool SafetyRules() const;
			// R5 switch, default OFF: an ABR rendition step (NoteEvent AbrStep) drops an upscaled latch (E0: helps only after a rendition change).
			void SetLatchAbrStepTrigger(bool on);
			// N1: the count of latch releases by the no-cut CEILING (LatchEvent::ReleasedCeiling; zero while the rules are off). The monitor shows
			// the native switch that follows one through its engage crossfade instead of the release fade. Any thread.
			uint64_t LatchCeilingReleases() const;
			// Dump v4 (identity/SidDiagDump.h; written only while VP_SID_DUMP_V4_DIR is set, decision neutral): the file-name tag of this
			// estimator's b_evals / b_sizes files (a new tag re-reads the variable and reopens the files at the next evaluation), and the
			// caller's depth hint recorded on each row (coded = the decoded stream's bit depth, output = the delivered plane's; 0 = unknown).
			// Any thread.
			void SetDiagnosticTag(const std::string& tag);
			void SetDiagnosticDepths(int codedBits, int outputBits);

			// ---- evidence log (setter only; the key `source_id_evidence` is plumbed with P3) -----------------------------
			void SetEvidence(const std::string& folder, EvidenceLevel level);
			EvidenceLevel GetEvidenceLevel() const;
			// The report-a-problem hotkey: a dump at the next opportunity.
			void RequestEvidenceDump(const std::string& why);
			// Any thread: one-time notices ("evidence logging on (shadow)", write failures).
			bool TakeNotice(std::string& text);

			// ---- the sink (the SourceFrameTap calls these) -----------------------------------------------------------
			bool Wants() const;
			void Deliver(const VpSkeleton::SourceFrameView& view);

			// Set BEFORE the estimator runs (not synchronised with a running worker): one observer, called after each evaluation.
			void SetEvaluationObserver(EvaluationObserver observer);

			// ---- products and status ------------------------------------------------------------------------------------
			// P14g: hand its address to Monitor::SetGeometrySource (valid for the estimator's lifetime).
			const Geometry::HeldState& Held() const;
			ChainStatusInput StatusInput() const;
			std::string StatusLine() const;          // Ctrl+I and the Config status
			std::string DetailText() const;          // Ctrl+Alt+D
			std::string EvidenceStatusText() const;
			EstimatorCounters GetCounters() const;
			// The governor's plan as of the last evaluation (interval, cost, share, flags).
			GovernorPlan GetGovernorPlan() const;

			// ---- tests ------------------------------------------------------------------------------------------------------
			// Synchronous: no thread, every Deliver / event / reset is processed on the calling thread before it returns.
			void SetSynchronous(bool on);
			// True when the queue is empty and nothing is being processed (or the timeout passed: false).
			bool WaitIdle(int timeoutMs) const;
			// Holds the worker back (jobs queue up, newest wins, nothing is processed) / lets it go on. Synchronous mode ignores it.
			void PauseForTest(bool pause);
			// Sets the "both axes confident" power flag (the long cadence, the event pull-forward) without a real reading; the next evaluation recomputes it.
			void ForceLatchedPowerForTest(bool on);
			bool LatchedPowerForTest() const;
			// Counts one ceiling release (LatchCeilingReleases) without running the latch: the monitor's crossfade cue in isolation.
			void NoteLatchCeilingForTest();
			// Sets the due time (wall ms of the estimator's clock) and the interval it implies, as a confident evaluation would have left them.
			void ForceNextDueForTest(double ms);
			// The replay entry: feeds a (dumped) snapshot to the worker as if it had been delivered - its flags, source time and active
			// rectangle included, no due check, no clock lookup. False when the estimator does not run.
			bool SubmitLines(const LineSnapshot& snapshot);

		private:
			struct State;
			std::shared_ptr<State> st_;
		};
	}
}
