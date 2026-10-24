#pragma once

// Queue/timing POLICY interface for the renderer's frame queue (2026-10-24, DRAFT).
//
// UPSTREAMABLE: this header depends only on AlphaQueuePolicy.h and the standard library, so it can
// be offered to billslack2/videoprocessor as-is. The renderer's queue calls ONE policy object at
// four defined points; the policy decides, the host acts (drops, waits, logs, status):
//
//   1. PlanHold        - when the source/display rate or the settings change: is there a timed
//                        hold, and how long (a fixed total delay, or none for a frame-count queue).
//   2. QueueTarget     - PrefillTargetLocked(): the depth the queue steers to (prefill/dequeue gate).
//   3. TicksUntilDue   - WaitForDequeueLocked(): how long the head must still wait (0 = due now).
//   4. EvaluateBacklog - after a dequeue: shed a backlog? how many frames? which trigger?
//
// BillQueuePolicy below IS the current upstream behaviour (frame-count queue, no timed hold,
// AlphaQueuePolicy::ShouldRecoverBacklog with upstream's own signature, flush to target), so a
// renderer that installs it decides exactly as before. Other policies plug in through the same
// interface (VP's fixed total-delay policy lives in its fork: src/VP-V/modules/input/queue_timing/).
//
// Threads: a policy is called with the queue mutex held (QueueTarget, TicksUntilDue,
// EvaluateBacklog) or from the settings/rate path (PlanHold); implementations keep no locks and
// do no I/O. BacklogRunState is owned by the caller (the render loop), so one policy object may be
// shared.

#include <vprenderer/AlphaQueuePolicy.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

namespace QueueTimingPolicy
{
	enum class Kind
	{
		Upstream,       // BillQueuePolicy: the frame-count queue as shipped upstream (the default)
		External,       // a policy installed by a fork / plugin (e.g. VP's fixed total delay)
	};

	// ---- 1. PlanHold ---------------------------------------------------------------------------
	struct HoldContext
	{
		int budgetMs = 0;                  // a configured total delay (0 = none)
		double sourceRateHz = 0.0;         // the queued frames' rate (measured)
		double signalRateHz = 0.0;         // the capture signal's rate (0 = same as source)
		double displayHz = 0.0;            // measured display refresh (0 = not known yet)
		bool lowLatency = false;           // low-latency queue mode active
		bool renderAheadActive = false;    // presentation renders ahead of its refresh
		double renderAheadLeadSeconds = 0.0;   // its planned lead (0 when off)
		size_t renderAheadCreditFrames = 0;    // frames the queue target gives back for it
		size_t queueCapacity = 1;          // the FIFO cap (queue_size; 1 without a frame queue)
	};

	struct HoldPlan
	{
		bool active = false;               // a timed hold realises the delay
		double holdMs = 0.0;               // each frame's wait in the queue (after its transfer)
		double transferMs = 0.0;           // the capture transfer counted before the hold
		double fraction = 0.0;             // the sub-frame part of the hold (steering target)
		size_t targetFrames = 0;           // the hold's whole frames
		int queueFrames = 0;               // look-ahead frames the plan gives (ladder level)
		bool degraded = false;             // fewer look-ahead frames than designed
		double fixedMs = 0.0;              // fixed costs inside the total
		double paddingMs = 0.0;            // the rest that makes the total exact
		std::string note;                  // non-empty: why the delay is not exact (show it; never log-only)
	};

	// ---- 2. QueueTarget ------------------------------------------------------------------------
	struct TargetContext
	{
		bool useFrameQueue = true;
		size_t configuredDepth = AlphaQueuePolicy::DEFAULT_DESIRED_DEPTH;   // m_frameQueueDesiredDepth
		size_t capacity = AlphaQueuePolicy::DEFAULT_DESIRED_DEPTH;          // m_frameQueueMaxSize
		// Host-computed depth after any cadence scaling the host applies (equal to the clamped
		// configured depth when it applies none). Upstream ignores it.
		size_t cadenceScaledDepth = 0;
		size_t renderAheadCreditFrames = 0;
		size_t lookaheadFloor = 0;         // the deepest look-ahead consumer's need
		HoldPlan hold;                     // the current PlanHold result
	};

	// ---- 3. TicksUntilDue ----------------------------------------------------------------------
	struct DueContext
	{
		HoldPlan hold;
		bool timelineActive = false;       // a presentation timeline steers the queue itself
		bool fileFrame = false;            // the head is a file frame (paced by its own due time)
		int64_t arrivalQpc = 0;            // capture arrival, normalised to the frame's START (0 = unknown)
		int64_t enqueueQpc = 0;
		int64_t nowQpc = 0;
		int64_t qpcFrequency = 0;
	};

	// ---- 4. EvaluateBacklog --------------------------------------------------------------------
	enum class Trigger
	{
		None,
		RenderStall,       // the render cycle exceeded the stall threshold
		StaleAge,          // the oldest frame is stale
		HeldBacklog,       // a timed hold's head is late (shed exactly the late frames)
	};

	inline const char* TriggerName(Trigger t)
	{
		switch (t)
		{
		case Trigger::RenderStall: return "render_stall";
		case Trigger::StaleAge: return "stale_age";
		case Trigger::HeldBacklog: return "held_backlog";
		default: return "none";
		}
	}

	struct BacklogContext
	{
		size_t queueBefore = 0;            // depth after the dequeue
		size_t target = 0;                 // QueueTarget()
		double oldestQueuedAgeMs = 0.0;    // front frame's age since ENQUEUE
		double headArrivalAgeMs = -1.0;    // front frame's age since its START (arrival); < 0 unknown
		double renderCycleMs = 0.0;
		double captureRateHz = 0.0;        // the signal's nominal rate (upstream's input)
		double measuredSignalRateHz = 0.0; // measured signal rate (0 = unknown)
		double frontFilmHz = 0.0;          // front frame's film rate under a pulldown lock (0 = none)
		bool fileSource = false;           // current or front frame is a file frame: never shed
		bool qpcAvailable = true;
		HoldPlan hold;
	};

	struct BacklogDecision
	{
		bool recover = false;
		Trigger trigger = Trigger::None;
		// Frames to drop at most (always stopping at `target`). npos = flush to target.
		size_t maxDrop = std::numeric_limits<size_t>::max();
		// true: a full flush that restarts the queue's steering / presentation anchor. A held shed
		// keeps them (the queue is back at its hold, not restarted).
		bool restartsTiming = true;
		double queueRateHz = 0.0;          // the rate the decision used (for the log)
	};

	// Caller-owned state that must persist between evaluations (consecutive-late runs).
	struct BacklogRunState
	{
		unsigned heldExcessRun = 0;
		void Reset() { heldExcessRun = 0; }
	};

	class IQueueTimingPolicy
	{
	public:
		virtual ~IQueueTimingPolicy() = default;
		virtual Kind GetKind() const = 0;
		virtual const char* Name() const = 0;   // for Ctrl+I / logs ("upstream", "vp fixed delay", ...)
		virtual HoldPlan PlanHold(const HoldContext& context) const = 0;
		virtual size_t QueueTarget(const TargetContext& context) const = 0;
		virtual int64_t TicksUntilDue(const DueContext& context) const = 0;
		virtual BacklogDecision EvaluateBacklog(const BacklogContext& context, BacklogRunState& run) const = 0;
	};

	// The upstream behaviour (billslack2 v1.4.02-beta), unchanged: frame-count queue, no timed hold,
	// stale/stall recovery by AlphaQueuePolicy::ShouldRecoverBacklog called with upstream's own
	// five-argument signature (so upstream's later threshold changes apply here automatically),
	// flushed to the target.
	class BillQueuePolicy final : public IQueueTimingPolicy
	{
	public:
		Kind GetKind() const override { return Kind::Upstream; }
		const char* Name() const override { return "upstream"; }

		HoldPlan PlanHold(const HoldContext&) const override { return HoldPlan{}; }

		size_t QueueTarget(const TargetContext& c) const override
		{
			return c.useFrameQueue ?
				AlphaQueuePolicy::ClampDesiredDepthToCapacity(c.configuredDepth, c.capacity) : 1;
		}

		int64_t TicksUntilDue(const DueContext&) const override { return 0; }

		BacklogDecision EvaluateBacklog(const BacklogContext& c, BacklogRunState& run) const override
		{
			run.Reset();
			BacklogDecision d;
			d.queueRateHz = c.captureRateHz;
			// fileSource is always false upstream (no file frames there); a file queue never sheds.
			d.recover = !c.fileSource && AlphaQueuePolicy::ShouldRecoverBacklog(c.queueBefore, c.target, c.oldestQueuedAgeMs,
				c.renderCycleMs, c.captureRateHz);
			if (d.recover)
				d.trigger = c.renderCycleMs >= AlphaQueuePolicy::RenderStallThresholdMs(c.captureRateHz) ?
					Trigger::RenderStall : Trigger::StaleAge;
			return d;
		}
	};
}
