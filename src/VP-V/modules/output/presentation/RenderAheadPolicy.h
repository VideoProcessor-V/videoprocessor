#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <string>
#include <utility>
#include <vector>

#include "OpenLoopDisplayRate.h"

// Render-ahead (docs/VP-render-ahead-design.md, owner's top priority 2026-10-17):
// the de-judder's processing cost follows the source frame, as in madVR, not the
// display refresh. With `render_ahead` on, a source frame A's extras are planned
// from the film clock and queued straight after A (FIFO back-pressure, no vblank
// waits), so the next frame B renders while A and its extras are on screen.
//
// Pure logic (no GPU, no clock): the renderer's rules are unit-tested without one.
// Times are seconds on one clock (QPC). Nothing here fixes a rate, a cadence or a
// resolution: everything derives from the measured source period P_s and the
// measured display period P_d (owner [[vp-general-not-fixed]]).
//
// DXGI sync interval: a present made with SyncInterval n is shown no sooner than
// n vblanks after the previous present was shown, so the picture before it stays
// n refreshes. A refresh A owns but does not render (a hold) therefore adds one
// to the interval of the next present - the next extra, or B's own present.
namespace RenderAhead
{
	constexpr int kMaxSyncInterval = 4;     // DXGI's limit for SyncInterval
	constexpr int kMinBuffers = 3;          // today's VP-owned flip chain
	constexpr int kMaxBuffers = 6;          // design: ceil(max ratio) + 2, capped at 6
	constexpr unsigned kQueuedFrameLatency = 3;   // SetMaximumFrameLatency in this mode (today 2)
	constexpr double kTolerance = 0.05;     // refreshes, as DejudderPolicy's cadence tolerance

	// VP-owned swapchain buffers in render-ahead mode: ceil(refreshes per source
	// frame) + 2 (the front buffer and one being rendered), at least today's 3 and at
	// most 6. An unknown ratio (rates not measured when the chain is made) takes 6.
	inline int SwapchainBuffers(double ratio)
	{
		if (!(ratio > 0.0) || !std::isfinite(ratio))
			return kMaxBuffers;
		const int wanted = static_cast<int>(std::ceil(ratio - kTolerance)) + 2;
		return std::min(kMaxBuffers, std::max(kMinBuffers, wanted));
	}

	// The most extras a source frame may own at this ratio (as
	// DejudderPolicy::MaxExtras: the refreshes it owns minus its own).
	inline int MaxExtras(double ratio)
	{
		if (!(ratio > 1.0) || !std::isfinite(ratio))
			return 0;
		const int owned = static_cast<int>(std::ceil(ratio - kTolerance));
		return std::min(std::max(owned - 1, 0), 15);
	}

	// The planned lead (design step 7): the display time the queued extras occupy
	// beyond A's own refresh, so B renders while they are shown. Added to the
	// presentation target (audio) and, with render_ahead_queue_credit, taken back
	// from the input queue.
	inline double PlannedLead(double sourcePeriod, double displayPeriod)
	{
		if (!(sourcePeriod > 0.0) || !(displayPeriod > 0.0))
			return 0.0;
		return MaxExtras(sourcePeriod / displayPeriod) * displayPeriod;
	}

	// render_ahead_queue_credit: whole source frames the queue may give back for the
	// lead - floor(lead / P_s), only the latency the queued extras really add (bug hunt
	// 05 M4: ceil returned a whole frame for a smaller lead, 41.7 ms for 33 ms at 24/60,
	// and cut the queue's jitter margin below render_ahead off's).
	inline size_t CreditFrames(double lead, double sourcePeriod)
	{
		if (!(lead > 0.0) || !(sourcePeriod > 0.0) || !std::isfinite(lead / sourcePeriod))
			return 0;
		return static_cast<size_t>(std::floor(lead / sourcePeriod + 1e-9));
	}

	// The credit against a queue target: never below 2 queued frames (B must still be
	// queued when A renders).
	inline size_t QueueCredit(double lead, double sourcePeriod, size_t desiredDepth)
	{
		if (desiredDepth <= 2)
			return 0;
		return std::min(CreditFrames(lead, sourcePeriod), desiredDepth - 2);
	}

	// r6 fix 4 (2026-10-24, look-ahead inventory item 41): render-ahead must fit INSIDE the fixed
	// delay budget - the queue target's own delay (target frames x P_s; target_latency_ms already
	// sets that target). The queued extras add PlannedLead; it may run only when the queue gives
	// that time back (render_ahead_queue_credit) without the queue dropping below kBudgetMinQueued
	// frames (A shown, B queued and converted, one spare: as Pulldown::kLockedQueuePairFrames).
	// The credit is the fewest whole frames covering the lead (ceil), so the total never exceeds
	// render_ahead off's; a lead the queue cannot give back keeps render-ahead OFF for the source
	// (shown under "Fallbacks engaged"). No lead (no extras: 50/60p on 60 Hz) always fits.
	constexpr size_t kBudgetMinQueued = 3;

	// P6 rest S1 (TD-readiness audit 93 (a)2): which side gives way when the analysis floor
	// (the frames the queue's look-ahead users keep: the motion depth floor, a declared
	// analysis lead + 2) and the render-ahead credit compete for the same queued frame. The
	// policy is the owner's (parked for the TD planning), so it is an input:
	//  - CreditWins (default, today's decision): the credit is taken as before; the report
	//    says the floor holds the queue above the credit and gives the real delay.
	//  - FloorWins: the credit is only what the floor leaves; a lead it no longer covers
	//    vetoes render-ahead (its fallback says why).
	//  - RaiseQueue (S1 amendment, TD design (a)4/(b)6; for TD on, default off): the credit is
	//    taken as before and the floor is NOT capped at the desired queue, so the queue target
	//    rises above the user's setting (VpQueueRules::CombineQueueTarget, raiseAboveDesired);
	//    the report gives the raised queue and the real delay.
	// The queue keeps the floor (CombineQueueTarget; capped at the desired queue except with
	// RaiseQueue), and the decision is the same as without a floor while the floor is at most
	// kBudgetMinQueued.
	enum class FloorPolicy { CreditWins, FloorWins, RaiseQueue };

	struct BudgetDecision
	{
		bool fits = true;
		size_t creditFrames = 0;   // frames the queue gives back for the lead
		double leadMs = 0.0;
		double budgetMs = 0.0;     // render_ahead off's delay: the queue target
		double totalMs = 0.0;      // with render-ahead: (target - credit) x P_s + lead
		const char* reason = "no lead";
		// The analysis floor (P6 rest S1).
		size_t floorFrames = 0;    // the input
		bool floorCompetes = false;   // the floor took (FloorWins) or kept (CreditWins) a frame of the credit
		double realTotalMs = 0.0;  // the delay with the queue held at the floor: max(target - credit, floor) x P_s + lead
		size_t queueFrames = 0;    // the desired queue (the input)
		size_t raisedQueueFrames = 0;   // RaiseQueue: the queue the floor takes above the desired one (0 = not raised)
	};
	inline BudgetDecision FitDelayBudget(double sourcePeriod, double displayPeriod, size_t queueFrames, bool creditOn,
		size_t floorFrames, FloorPolicy policy);
	inline BudgetDecision FitDelayBudget(double sourcePeriod, double displayPeriod, size_t queueFrames, bool creditOn)
	{
		return FitDelayBudget(sourcePeriod, displayPeriod, queueFrames, creditOn, 0, FloorPolicy::CreditWins);
	}
	inline BudgetDecision FitDelayBudget(double sourcePeriod, double displayPeriod, size_t queueFrames, bool creditOn,
		size_t floorFrames, FloorPolicy policy)
	{
		BudgetDecision d;
		d.floorFrames = floorFrames;
		d.queueFrames = queueFrames;
		const double lead = PlannedLead(sourcePeriod, displayPeriod);
		d.leadMs = lead * 1000.0;
		d.budgetMs = sourcePeriod > 0.0 ? static_cast<double>(queueFrames) * sourcePeriod * 1000.0 : 0.0;
		d.totalMs = d.budgetMs;
		d.realTotalMs = d.totalMs;
		if (!(lead > 0.0) || !(sourcePeriod > 0.0) || !std::isfinite(lead / sourcePeriod))
			return d;
		constexpr double kSlackMs = 0.5;   // timestamp rounding
		const size_t wanted = static_cast<size_t>(std::ceil(lead / sourcePeriod - 1e-9));
		if (creditOn && queueFrames >= wanted + kBudgetMinQueued)
		{
			// The floor the queue keeps (CombineQueueTarget: never above the target, except RaiseQueue).
			const size_t floorKept = policy == FloorPolicy::RaiseQueue ? floorFrames : (std::min)(floorFrames, queueFrames);
			if (floorKept > queueFrames)
				d.raisedQueueFrames = floorKept;
			size_t credit = wanted;
			if (floorKept > queueFrames - wanted)
			{
				d.floorCompetes = true;
				if (policy == FloorPolicy::FloorWins)
					credit = queueFrames - (std::max)(floorKept, kBudgetMinQueued);   // what the floor leaves
			}
			if (credit < wanted)
			{
				// FloorWins: the frames left do not cover the lead.
				d.creditFrames = 0;
				d.totalMs = d.budgetMs + d.leadMs;
				d.realTotalMs = d.totalMs;
				d.fits = false;
				d.reason = "the analysis floor keeps the frames the lead needs";
				return d;
			}
			d.creditFrames = credit;
			d.totalMs = (static_cast<double>(queueFrames - credit) * sourcePeriod + lead) * 1000.0;
			d.realTotalMs = (static_cast<double>((std::max)(queueFrames - credit, floorKept)) * sourcePeriod + lead) * 1000.0;
			d.fits = d.totalMs <= d.budgetMs + kSlackMs;
			d.reason = d.fits ? "the queue gives the lead back" : "the lead exceeds the frames given back";
			return d;
		}
		d.totalMs = d.budgetMs + d.leadMs;
		d.realTotalMs = d.totalMs;
		d.fits = false;
		d.reason = !creditOn ? "the lead would add delay (render_ahead_queue_credit off)" :
			"the queue is too short to give the lead back";
		return d;
	}

	// The queue credit, latched (bug hunt 05 M4): it stays while render-ahead runs at
	// one cadence, so a single break (a hold, a scene cut) no longer raises the queue
	// target by a frame (a repeat on screen) and lowers it again on the next plan (a
	// surplus frame). It changes with the cadence, and it is returned only once the
	// chain has been broken for kReleaseSeconds, or on Reset (a new generation, a new
	// cadence, render_ahead switched on).
	class CreditLatch
	{
	public:
		static constexpr double kReleaseSeconds = 2.0;
		static constexpr double kCadenceChange = 0.02;   // refreshes per source frame, as the phase reset

		void Reset()
		{
			frames = 0;
			ratio = 0.0;
			brokenSince = -1.0;
		}
		// A frame's plan was queued: the credit for its lead at this cadence.
		size_t Commit(double lead, double sourcePeriod, double cadenceRatio)
		{
			return CommitFrames(CreditFrames(lead, sourcePeriod), cadenceRatio);
		}
		// r6 fix 4: the frames the delay budget decided (FitDelayBudget's ceil credit).
		size_t CommitFrames(size_t wanted, double cadenceRatio)
		{
			brokenSince = -1.0;
			if (frames == 0 || std::abs(cadenceRatio - ratio) > kCadenceChange)
			{
				frames = wanted;
				ratio = cadenceRatio;
			}
			return frames;
		}
		// A frame not queued by the plan: the credit stays until the chain has been
		// broken for kReleaseSeconds.
		size_t Break(double now)
		{
			if (frames == 0)
				return 0;
			if (brokenSince < 0.0)
				brokenSince = now;
			else if (now - brokenSince >= kReleaseSeconds)
				Reset();
			return frames;
		}
		size_t Frames() const { return frames; }

	private:
		size_t frames = 0;
		double ratio = 0.0;
		double brokenSince = -1.0;
	};

	// The VP-owned chain's depth (bug hunt 05 M6). Low latency, and content without
	// extras (maxExtras 0, e.g. 50/60p on 60 Hz) or a cadence not known yet, keep
	// today's chain: 3 buffers, a frame latency of 2 (owner: low latency keeps today's
	// behaviour). Render-ahead with extras: SwapchainBuffers(ratio) and
	// kQueuedFrameLatency. Before, the chain was made before the first frame set the
	// source rate, so it took 6 buffers and a latency of 3 whatever the content.
	struct ChainDepth
	{
		int buffers = kMinBuffers;
		unsigned latency = 2;
		bool operator==(const ChainDepth& o) const { return buffers == o.buffers && latency == o.latency; }
		bool operator!=(const ChainDepth& o) const { return !(*this == o); }
	};

	inline ChainDepth ChainDepthFor(bool renderAhead, bool lowLatency, double ratio)
	{
		ChainDepth depth;
		if (!renderAhead || lowLatency || MaxExtras(ratio) == 0)
			return depth;
		depth.buffers = SwapchainBuffers(ratio);
		depth.latency = kQueuedFrameLatency;
		return depth;
	}

	// ---------------------------------------------------------------------
	// The frame planner.

	struct FrameInput
	{
		double filmTime = 0.0;        // A's time on the film / capture clock (its callback)
		double sourcePeriod = 0.0;    // P_s
		double displayPeriod = 0.0;   // P_d
		double firstVsync = 0.0;      // the predicted vsync of A's own present
		double latencyFloor = 0.0;    // L: the floor of (first vsync - film time), lead excluded
		double lead = 0.0;            // the planned lead (PlannedLead)
		double strength = 0.0;        // the extras' strength s
		int evenCycle = 0;            // N on an even cadence (extras at k/N), else 0
		int maxExtras = 0;            // the cap (MaxExtras of the ratio)
		int dropExtras = 0;           // extras to drop: lateness + the gate's ladder
		double arrivalJitter = 0.0;   // measured arrival jitter (s, Schedule::NoteArrival): widens the phase tolerance
	};

	struct Slot
	{
		double vsync = 0.0;    // the refresh this slot fills
		double delta = 0.0;    // its phase (0 = A, 1 = B)
		float shift = 0.0f;    // s * delta (or s * k/N): what the warp moves by
		bool render = false;   // an extra is rendered; false = A (or the last extra) holds
		bool dropped = false;  // a render slot the drop (lateness / ladder) turned into a hold
		int interval = 1;      // the extra's SyncInterval (1 + the holds before it)
	};

	struct FramePlan
	{
		bool valid = false;
		int refreshes = 1;            // the refreshes A owns, its own included
		std::vector<Slot> slots;      // A's refreshes after its own
		int nextInterval = 1;         // B's present: 1 + the holds after the last extra
		double nextFirstVsync = 0.0;  // where B lands when it is ready in time
		double phi0 = 0.0;            // A's own refresh's phase
		int rendered = 0;             // extras rendered
		int dropped = 0;              // extras dropped
	};

	// The phase of the refresh at vsync for frame A: (V - film time - L - lead) / P_s.
	inline double Phase(const FrameInput& in, double vsync)
	{
		return (vsync - in.filmTime - in.latencyFloor - in.lead) / in.sourcePeriod;
	}

	// A phase within kPhaseEpsilon of 0 is A's own time (rounding of the restart lead,
	// which is a whole number of refreshes): a hold, not an extra with no shift.
	constexpr double kPhaseEpsilon = 1e-6;

	// The phase boundaries' tolerance, expressed in phase: at least kTolerance
	// refreshes, or the measured arrival jitter when larger, at most
	// kMaxToleranceRefreshes. Arrival jitter moves every phase of a frame by the same
	// small amount; without a tolerance a refresh at phase 1 - jitter stays with A on a
	// whole ratio, so A owns n+1 refreshes and B n-1 (Bug hunt 05 H2: 3:1 / 1:3 splits
	// on 30p at 60 Hz). A refresh within it below 1 belongs to B (B's own picture then
	// shows at most the tolerance early instead of up to a refresh late), and a phase
	// within it of 0 is A's own time (a hold, not an extra with a near-zero shift).
	constexpr double kMaxToleranceRefreshes = 0.45;
	constexpr double kJitterMargin = 1.5;   // over the decaying peak (an anchor at a jitter extreme)
	inline double PhaseTolerance(const FrameInput& in)
	{
		if (!(in.sourcePeriod > 0.0) || !(in.displayPeriod > 0.0))
			return kPhaseEpsilon;
		const double jitterRefreshes = in.arrivalJitter > 0.0 && std::isfinite(in.arrivalJitter) ?
			kJitterMargin * in.arrivalJitter / in.displayPeriod : 0.0;
		const double refreshes = std::min(kMaxToleranceRefreshes, std::max(kTolerance, jitterRefreshes));
		return std::max(kPhaseEpsilon, refreshes * in.displayPeriod / in.sourcePeriod);
	}

	inline float ShiftFor(const FrameInput& in, double delta)
	{
		const double tolerance = PhaseTolerance(in);
		if (!(delta > tolerance))
			return 0.0f;
		if (in.evenCycle > 0)
		{
			// Delta 10: an even cadence's extras sit at the exact phases k/N; a phase
			// within the tolerance below k/N is still k (the same tolerance in k).
			const double toleranceK = std::min(0.49, std::max(1e-6, tolerance * in.evenCycle));
			const double k = std::floor(delta * in.evenCycle + toleranceK);
			if (!(k >= 1.0))
				return 0.0f;
			return static_cast<float>(in.strength * std::min(k, static_cast<double>(in.evenCycle - 1)) / in.evenCycle);
		}
		return static_cast<float>(in.strength * std::min(delta, 1.0));
	}

	// Recomputes the intervals from the render flags.
	inline void AssignIntervals(FramePlan& plan)
	{
		int run = 0;
		plan.rendered = 0;
		for (Slot& slot : plan.slots)
		{
			if (slot.render)
			{
				slot.interval = 1 + run;
				run = 0;
				++plan.rendered;
			}
			else
			{
				slot.interval = 0;
				++run;
			}
		}
		plan.nextInterval = 1 + run;
	}

	inline bool IntervalsFit(const FramePlan& plan)
	{
		if (plan.nextInterval > kMaxSyncInterval)
			return false;
		for (const Slot& slot : plan.slots)
			if (slot.render && slot.interval > kMaxSyncInterval)
				return false;
		return true;
	}

	// All of A's refreshes and their phases. A owns the refreshes from its own
	// (firstVsync) while their phase is below 1. A refresh with phase <= 0 (A shown
	// before its time: a restart with the lead) holds A; the others render an extra,
	// except the `dropExtras` with the smallest s*delta (owner 2026-10-17: the least
	// visible extra goes first), so long as no present needs an interval above DXGI's.
	inline FramePlan PlanFrame(const FrameInput& in)
	{
		FramePlan plan;
		if (!(in.sourcePeriod > 0.0) || !(in.displayPeriod > 0.0) || !std::isfinite(in.firstVsync) ||
			!std::isfinite(in.filmTime))
			return plan;
		plan.valid = true;
		const double tolerance = PhaseTolerance(in);
		plan.phi0 = Phase(in, in.firstVsync);
		if (plan.phi0 < 0.0 && plan.phi0 > -tolerance)
			plan.phi0 = 0.0;   // within the tolerance of 0: A's own time (H2)
		// A owns refreshes while the phase is below 1; at most its extras, the restart
		// lead's refreshes and DXGI's longest run of holds (a long restart lead is shown
		// as holds and re-rendered A's at most, all on A: B then continues the chain).
		const int leadRefreshes = in.lead > 0.0 ?
			static_cast<int>(std::ceil(in.lead / in.displayPeriod - 1e-6)) : 0;
		const int ownedCap = std::max(1, in.maxExtras + 1 + (kMaxSyncInterval - 1) + std::min(leadRefreshes, 16));
		int refreshes = 1;
		while (refreshes < ownedCap &&
			Phase(in, in.firstVsync + refreshes * in.displayPeriod) < 1.0 - tolerance)
			++refreshes;
		plan.refreshes = refreshes;
		for (int k = 1; k < refreshes; ++k)
		{
			Slot slot;
			slot.vsync = in.firstVsync + k * in.displayPeriod;
			slot.delta = std::max(0.0, std::min(Phase(in, slot.vsync), std::nextafter(1.0, 0.0)));
			slot.shift = ShiftFor(in, Phase(in, slot.vsync));
			slot.render = slot.shift > 0.0f && in.strength > 0.0;
			plan.slots.push_back(slot);
		}
		// The extras the cap allows (the earliest render slots keep their place).
		int renderSlots = 0;
		for (Slot& slot : plan.slots)
			if (slot.render && ++renderSlots > in.maxExtras)
				slot.render = false;
		AssignIntervals(plan);
		// Too many holds in a row (a long lead at a restart): the present after them
		// would need an interval above DXGI's, so A is presented again there (at its
		// slot's shift, 0 before A's time).
		{
			int run = 0;
			for (Slot& slot : plan.slots)
			{
				if (slot.render) { run = 0; continue; }
				if (run + 1 > kMaxSyncInterval - 1) { slot.render = true; run = 0; }
				else ++run;
			}
			AssignIntervals(plan);
		}
		// The drops: smallest s*delta first (earliest on a tie), one at a time, each
		// only when every interval still fits.
		for (int d = 0; d < in.dropExtras; ++d)
		{
			int best = -1;
			for (size_t i = 0; i < plan.slots.size(); ++i)
			{
				Slot& slot = plan.slots[i];
				if (!slot.render || !(slot.shift > 0.0f))
					continue;
				slot.render = false;
				AssignIntervals(plan);
				const bool fits = IntervalsFit(plan);
				slot.render = true;
				AssignIntervals(plan);
				if (fits && (best < 0 || slot.shift < plan.slots[static_cast<size_t>(best)].shift))
					best = static_cast<int>(i);
			}
			if (best < 0)
				break;
			plan.slots[static_cast<size_t>(best)].render = false;
			plan.slots[static_cast<size_t>(best)].dropped = true;
			++plan.dropped;
			AssignIntervals(plan);
		}
		plan.nextFirstVsync = in.firstVsync + refreshes * in.displayPeriod;
		return plan;
	}

	// ---------------------------------------------------------------------
	// The schedule: where the next present lands. Closed loop with DXGI's frame
	// statistics (a present's real vsync corrects the chain; one later than planned
	// is late), open loop without (the internal panel: the chain alone).

	class Schedule
	{
	public:
		void Reset()
		{
			active = false;
			nextVsync = 0.0;
			lateDrops = 0;
			reanchorPending = false;
			pending.clear();
			anchored = false;
			anchorLatency = 0.0;
			tooLateSince = -1.0;
			haveLastFilm = false;
			lastFilm = 0.0;
			arrivalJitter = 0.0;
		}

		// L, the constant that fixes which refresh is phase 0 (FrameInput::latencyFloor),
		// for the frame whose own present lands at firstVsync (FirstVsync). Fixed at the
		// anchor: a restart's sample (first vsync - film time, the lead excluded) - and a
		// restart follows every Break, generation change and chain too far ahead or
		// behind. A chained frame keeps it (Bug hunt 05 H1): its sample is >= L by
		// construction (the ownership loop picks the first vsync at phase >= 1), so a
		// floor or percentile over chained samples ratcheted upward on every inexact
		// cadence (23.976 on 60: 12 -> 121 ms in 120 s, more refreshes planned than the
		// display gives). A closed-loop or open-loop correction (Observe / NoteLate)
		// moves the chain and not L: the next frame's phases follow, it owns the
		// refreshes back (FIFO: the intervals' sum is what the display shows), and the
		// chain returns to the anchor's latency, which stays bounded.
		// withoutLead (s): a restart that must add no latency anchors with the lead taken
		// off, so A's own refresh is at phase 0 instead of holding for the lead (a re-anchor
		// after a backlog flush: render-ahead on-screen 2026-10-17). In open loop the lead's
		// holds are real display time, which FIFO back-pressure turns into queued input
		// frames: the flush's re-anchor holding them again overflowed the queue once more,
		// a cascade of flushes and re-anchors.
		double AnchorLatency(bool restart, double firstVsync, double filmTime, double withoutLead = 0.0)
		{
			if (restart || !anchored)
			{
				anchorLatency = firstVsync - filmTime - std::max(0.0, withoutLead);
				anchored = true;
				tooLateSince = -1.0;
			}
			return anchorLatency;
		}
		bool Anchored() const { return anchored; }

		// The anchor's L is too late (render-ahead on-screen 2026-10-17): a chained frame
		// whose own refresh comes more than a source period before the anchor would place
		// it (phi0 below the restart's own -lead/P_s by more than one), for a second of
		// film time without a break. An anchor taken while frames waited in a backlog (the
		// renderer starting: L 1.1-1.5 s on the laptop) otherwise stays for good: every
		// frame then holds the ownership cap (about 8 refreshes at 24/60), output runs at a
		// third of the source rate and the input queue drops two frames in three. A frame
		// the anchor places too early is never corrected by FirstVsync (the chain is ahead,
		// not behind). True: replace the anchor now (the caller restarts on this frame's
		// own vsync, a re-anchor, not late). One short dip (a queue that drained a frame)
		// does not: the frame holds until its time, as designed.
		bool AnchorTooLate(double firstVsync, double filmTime, double lead, double sourcePeriod)
		{
			if (!anchored || !(sourcePeriod > 0.0) || !std::isfinite(firstVsync) || !std::isfinite(filmTime))
			{
				tooLateSince = -1.0;
				return false;
			}
			const double phi0 = (firstVsync - filmTime - anchorLatency - lead) / sourcePeriod;
			if (!(phi0 < -std::max(0.0, lead) / sourcePeriod - kAnchorLateMarginPeriods))
			{
				tooLateSince = -1.0;
				return false;
			}
			if (tooLateSince < 0.0)
			{
				tooLateSince = filmTime;
				return false;
			}
			if (filmTime - tooLateSince < kAnchorLateSeconds)
				return false;
			tooLateSince = -1.0;
			++anchorsLowered;
			return true;
		}
		unsigned long long AnchorsLowered() const { return anchorsLowered; }

		// The arrival jitter the phase tolerance allows for (FrameInput::arrivalJitter),
		// from the film times themselves: the peak of |film - previous film - P_s| (the
		// spread of two arrivals, what an anchored chain sees between its anchor and a
		// later frame), decaying slowly (half-life about 700 frames). A step of half a
		// source period or more is a dropped or repeated frame, not jitter. Kept across
		// breaks (it belongs to the source); Reset clears it.
		double NoteArrival(double filmTime, double sourcePeriod)
		{
			if (haveLastFilm && sourcePeriod > 0.0 && std::isfinite(filmTime))
			{
				const double step = std::abs(filmTime - lastFilm - sourcePeriod);
				if (step < 0.5 * sourcePeriod)
					arrivalJitter = std::max(step, arrivalJitter * kJitterDecay);
			}
			lastFilm = filmTime;
			haveLastFilm = std::isfinite(filmTime);
			return arrivalJitter;
		}
		double ArrivalJitter() const { return arrivalJitter; }

		bool Active() const { return active; }
		double NextVsync() const { return nextVsync; }

		// The vsync A's own present (made at `now`) lands on: the chain's next
		// refresh when it is still ahead, else (a restart, or the chain fell behind)
		// the first refresh at or after `earliest`, on the vsync grid when known
		// (gridVsync > 0: any vsync seen on the display). restart reports a new anchor;
		// behind, that the chain was active but its refresh had passed (the frame is
		// late: the next frame drops one extra).
		// maxAhead (s, 0 = no bound): the furthest a present made now can land - the
		// presents the queue holds, the lead and a margin. A chain further ahead than
		// that is not real (a burst of frames planned faster than they were shown, e.g.
		// after a backlog flush; the open loop never sees it): a restart, not late
		// (render-ahead follow-up 2026-10-17).
		double FirstVsync(double earliest, double displayPeriod, double gridVsync, bool& restart, bool& behind,
			double maxAhead = 0.0) const
		{
			const bool tooFarAhead = active && maxAhead > 0.0 && nextVsync > earliest + maxAhead;
			restart = !active || !(nextVsync >= earliest - 0.25 * displayPeriod) || tooFarAhead;
			behind = active && restart && !tooFarAhead;
			if (!restart)
				return nextVsync;
			if (gridVsync > 0.0 && displayPeriod > 0.0)
			{
				const double k = std::ceil((earliest - gridVsync) / displayPeriod - 1e-6);
				return gridVsync + k * displayPeriod;
			}
			return earliest;
		}

		// After A's plan is queued: B's refresh, and A's present ids with the vsync
		// each was planned for (for the lateness check).
		void Commit(double nextFirstVsync, const std::vector<std::pair<uint32_t, double>>& planned)
		{
			// A new anchor (the chain was not active): its first observation re-anchors
			// only (bug hunt 05 M2).
			if (!active)
				reanchorPending = true;
			active = true;
			nextVsync = nextFirstVsync;
			for (const auto& p : planned)
				pending.push_back(p);
			while (pending.size() > 64)
				pending.pop_front();
		}

		void Break() { active = false; pending.clear(); reanchorPending = false; }

		// DXGI saw present `presentId` on screen at vsync `actual`. A present shown
		// more than half a refresh after its planned vsync is late: the chain moves by
		// the difference, and the next frame drops one extra (design step 4). Returns
		// true when late.
		// The first observation after a new anchor re-anchors only (bug hunt 05 M2): a
		// restart plans A on the first vsync after its Present returned, while its GPU
		// work may still run, so A often lands a refresh or more later. That is where the
		// chain really starts, not lateness: the chain moves, nothing is dropped, and it
		// is not late (the gate does not step down).
		bool Observe(uint32_t presentId, double actual, double displayPeriod)
		{
			bool late = false;
			while (!pending.empty() && static_cast<int32_t>(presentId - pending.front().first) > 0)
				pending.pop_front();
			if (!pending.empty() && pending.front().first == presentId)
			{
				const double planned = pending.front().second;
				pending.pop_front();
				const double error = actual - planned;
				lastObservedId = presentId;
				lastObservedError = displayPeriod > 0.0 ? error / displayPeriod : 0.0;
				++observedCount;
				const bool reanchor = reanchorPending;
				reanchorPending = false;
				if (error > 0.5 * displayPeriod)
				{
					if (reanchor)
						++reanchors;
					else
					{
						late = true;
						lateDrops = 1;
					}
					const double refreshes = std::round(error / displayPeriod);
					nextVsync += refreshes * displayPeriod;
					for (auto& p : pending)
						p.second += refreshes * displayPeriod;
				}
				else if (error < -0.5 * displayPeriod)
				{
					// Shown earlier than planned: the chain ran ahead of the screen (a
					// FIFO present shows at the first free refresh, so it is never early
					// on a true plan). The chain moves back by the difference - not late.
					// Before, only lateness moved it and the plan stayed ahead for good
					// (render-ahead follow-up 2026-10-17).
					const double refreshes = std::round(-error / displayPeriod);
					nextVsync -= refreshes * displayPeriod;
					for (auto& p : pending)
						p.second -= refreshes * displayPeriod;
					++earlyCorrections;
				}
			}
			return late;
		}

		// Open loop (no statistics): a present seen late by its QPC return time
		// (OpenLoopDisplayRate::LateRefreshes). As Observe: the chain and the
		// pending plans move by the refreshes, and the next frame drops one extra.
		// The first after a new anchor re-anchors only (as Observe). Returns true when
		// it counts as late (an open-loop drift correction: the gate takes it only when
		// sustained, SustainedLateness).
		bool NoteLate(int refreshes, double displayPeriod)
		{
			if (refreshes <= 0 || !(displayPeriod > 0.0))
				return false;
			const bool reanchor = reanchorPending;
			reanchorPending = false;
			if (reanchor)
				++reanchors;
			else
				lateDrops = 1;
			nextVsync += refreshes * displayPeriod;
			for (auto& p : pending)
				p.second += refreshes * displayPeriod;
			return !reanchor;
		}

		// The drops lateness asks of the next frame (consumed).
		int TakeLateDrops()
		{
			const int drops = lateDrops;
			lateDrops = 0;
			return drops;
		}

		size_t Pending() const { return pending.size(); }
		unsigned long long EarlyCorrections() const { return earlyCorrections; }
		unsigned long long Reanchors() const { return reanchors; }
		// The last present matched with its plan: its id and shown - planned (refreshes),
		// and a count that moves on every match (diagnostics).
		uint32_t LastObservedId() const { return lastObservedId; }
		double LastObservedError() const { return lastObservedError; }
		unsigned long long ObservedCount() const { return observedCount; }

	private:
		unsigned long long earlyCorrections = 0;
		unsigned long long reanchors = 0;
		bool reanchorPending = false;
		uint32_t lastObservedId = 0;
		double lastObservedError = 0.0;
		unsigned long long observedCount = 0;
		bool active = false;
		double nextVsync = 0.0;
		int lateDrops = 0;
		bool anchored = false;
		double anchorLatency = 0.0;   // L at the anchor (AnchorLatency)
		static constexpr double kAnchorLateMarginPeriods = 1.0;   // AnchorTooLate: source periods
		static constexpr double kAnchorLateSeconds = 1.0;         // AnchorTooLate: sustained, film time
		double tooLateSince = -1.0;   // film time of the first frame in the current too-late run
		unsigned long long anchorsLowered = 0;
		static constexpr double kJitterDecay = 0.999;
		bool haveLastFilm = false;
		double lastFilm = 0.0;
		double arrivalJitter = 0.0;   // NoteArrival
		std::deque<std::pair<uint32_t, double>> pending;
	};

	// ---------------------------------------------------------------------
	// The gate (dejudder_gate: period | refresh).

	enum class GateMode { Refresh, Period };

	// dejudder_gate auto: period only with render_ahead (the budget assumes the
	// queued extras); refresh restores the rule before render-ahead.
	inline GateMode ResolveGate(const std::string& setting, bool renderAhead)
	{
		if (!renderAhead)
			return GateMode::Refresh;
		return setting == "refresh" ? GateMode::Refresh : GateMode::Period;
	}

	// Today's rule (dejudder_gate refresh): the extras end at the last extra's vblank
	// and only then does B's whole pipeline start, so it must fit in one refresh.
	inline bool RefreshGateHolds(double workMs, double displayPeriod)
	{
		return workMs + 4.0 > 1000.0 * displayPeriod;
	}

	// The p95 of a cost over the last window (5 s), per slot type.
	class CostWindow
	{
	public:
		static constexpr double kWindowSeconds = 5.0;
		static constexpr size_t kCapacity = 2048;
		void Reset() { samples.clear(); }
		void Add(double now, double ms)
		{
			if (!(ms >= 0.0) || !std::isfinite(ms))
				return;
			samples.push_back(std::make_pair(now, ms));
			Trim(now);
			while (samples.size() > kCapacity)
				samples.pop_front();
		}
		void Trim(double now)
		{
			while (!samples.empty() && samples.front().first < now - kWindowSeconds)
				samples.pop_front();
		}
		size_t Count() const { return samples.size(); }
		double P95() const
		{
			if (samples.empty())
				return 0.0;
			std::vector<double> values;
			values.reserve(samples.size());
			for (const auto& s : samples)
				values.push_back(s.second);
			const size_t k = std::min(values.size() - 1, static_cast<size_t>(std::ceil(0.95 * values.size())) - 1);
			std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(k), values.end());
			return values[k];
		}

	private:
		std::deque<std::pair<double, double>> samples;
	};

	// A cost window that holds its last good p95 when it empties (a degraded level
	// renders fewer extras, none at hold), for kHoldSeconds; then the cost is unknown
	// again (0), so the ladder probes up instead of holding for good (bug hunt 05 H3).
	class KnownCost
	{
	public:
		static constexpr double kHoldSeconds = 30.0;

		explicit KnownCost(size_t minSamples = 4) : minSamples(minSamples) {}
		void Reset()
		{
			window.Reset();
			knownMs = 0.0;
			knownAt = 0.0;
		}
		void Add(double now, double ms) { window.Add(now, ms); }
		// Trims the window; a measured p95 refreshes the held value.
		void Trim(double now)
		{
			window.Trim(now);
			if (Measured())
			{
				knownMs = window.P95();
				knownAt = now;
			}
			else if (knownMs > 0.0 && now - knownAt > kHoldSeconds)
				knownMs = 0.0;
		}
		bool Measured() const { return window.Count() >= minSamples && minSamples > 0; }
		size_t Count() const { return window.Count(); }
		double P95() const { return window.P95(); }
		// The window's p95 when measured, else the held value (0 = unknown).
		double Current() const { return Measured() ? window.P95() : knownMs; }
		bool Known() const { return Measured() || knownMs > 0.0; }

	private:
		CostWindow window;
		size_t minSamples;
		double knownMs = 0.0;
		double knownAt = 0.0;
	};

	// The ladder (owner 2026-10-17: precision is never dropped automatically; a 16-bit
	// cache is a profile choice only):
	//   level 0          full
	//   level 1 (L1)     the sigma-delta takes the de-judder's vectors instead of its own
	//                    search (skipped when it already does)
	//   level 1 + d      d extras dropped per frame, the smallest s*delta first
	//   level 1 + max    hold (no extras)
	inline int DropsAt(int level) { return std::max(0, level - 1); }
	inline int HoldLevel(int maxExtras) { return 1 + std::max(0, maxExtras); }

	// Average extras per source frame with d dropped: a fraction frac(r) of the
	// frames own ceil(r) refreshes, the rest floor(r) (an even cadence: all r).
	inline double ExtrasPerFrame(double ratio, int drops)
	{
		if (!(ratio > 1.0))
			return 0.0;
		const double lo = std::floor(ratio + kTolerance);
		double frac = ratio - lo;
		if (frac < kTolerance) frac = 0.0;
		const double extrasLo = std::max(0.0, lo - 1.0 - drops);
		const double extrasHi = std::max(0.0, lo - drops);
		return frac * extrasHi + (1.0 - frac) * extrasLo;
	}

	inline int MaxExtrasPerFrame(double ratio, int drops)
	{
		return std::max(0, MaxExtras(ratio) - drops);
	}

	struct GateCosts
	{
		double newFrameMs = 0.0;       // N: a new frame's whole render (p95)
		double extraMs = 0.0;          // X: one extra (p95), as it runs now
		double extraCheaperMs = 0.0;   // X at L1 (the de-judder's vectors), 0 = not known
		double estimateMs = 0.0;       // E: the estimate ahead (p95)
		// S1 amendment (TD design (b)6): GPU work a look-ahead user runs in the spare slot, per
		// source frame (ms; 0 = none). ADDITIVE in the average check only: it never shares a
		// present's refresh, so the single-frame check does not see it.
		double spareSlotMs = 0.0;
	};

	// X per vector mode (bug hunt 05 M7): the extras timed with the sigma-delta's own
	// search (ownMs) and with the de-judder's vectors (externalMs), each priced at the
	// levels that run it. Before, one window held whatever ran: at L1 it held the
	// cheaper extras, so level 0 was priced cheap (the ladder climbed to full, went
	// over budget, dropped back - every ~12 s), and L1's price subtracted the saving a
	// second time. When only one mode is measured the other is estimated from the
	// sigma-delta's own/external time (saving = own - external, never negative).
	// Returns extraMs (level 0) and extraCheaperMs (L1 and below; 0 = no L1).
	inline GateCosts ExtraCosts(double ownMs, double externalMs, double savingMs, bool l1Available)
	{
		GateCosts c;
		const double saving = savingMs > 0.0 && std::isfinite(savingMs) ? savingMs : 0.0;
		if (!l1Available)
		{
			// One mode for every level (dither_vectors not own): the dearer of what was timed.
			c.extraMs = std::max(ownMs, externalMs);
			return c;
		}
		c.extraMs = ownMs > 0.0 ? ownMs : externalMs > 0.0 ? externalMs + saving : 0.0;
		c.extraCheaperMs = externalMs > 0.0 ? externalMs : ownMs > 0.0 ? std::max(0.0, ownMs - saving) : 0.0;
		return c;
	}

	struct GateInput
	{
		double sourcePeriod = 0.0;   // P_s (s)
		double displayPeriod = 0.0;  // P_d (s)
		double utilisation = 0.85;   // u (render_ahead_utilisation)
		double marginMs = 1.0;
		int queueDepth = static_cast<int>(kQueuedFrameLatency);   // Q: presents in flight
		bool l1Available = false;    // the sigma-delta searches itself (dither_vectors own)
		GateCosts costs;
	};

	// The GPU work of one source frame at a level (ms, average over the cadence).
	inline double FrameCostMs(const GateInput& in, int level)
	{
		const double ratio = in.sourcePeriod / in.displayPeriod;
		const double extras = ExtrasPerFrame(ratio, DropsAt(level));
		const double x = level >= 1 && in.costs.extraCheaperMs > 0.0 ? std::min(in.costs.extraMs, in.costs.extraCheaperMs) :
			in.costs.extraMs;
		return in.costs.newFrameMs + (extras > 0.0 ? in.costs.estimateMs : 0.0) + extras * x + in.costs.spareSlotMs;
	}

	// Both checks of the design: the average, sum(N + E + extras * X) + margin <=
	// u * P_s; and the single frame, N + X * n <= (n + Q - 1) * P_d.
	inline bool Fits(const GateInput& in, int level, double utilisation)
	{
		if (!(in.sourcePeriod > 0.0) || !(in.displayPeriod > 0.0))
			return false;
		const double ratio = in.sourcePeriod / in.displayPeriod;
		if (FrameCostMs(in, level) + in.marginMs > utilisation * 1000.0 * in.sourcePeriod)
			return false;
		const int n = MaxExtrasPerFrame(ratio, DropsAt(level));
		const double x = level >= 1 && in.costs.extraCheaperMs > 0.0 ? std::min(in.costs.extraMs, in.costs.extraCheaperMs) :
			in.costs.extraMs;
		return in.costs.newFrameMs + x * n <= (n + in.queueDepth - 1) * 1000.0 * in.displayPeriod;
	}

	struct GateChange
	{
		bool changed = false;
		int from = 0;
		int to = 0;
		const char* why = "";
	};

	class Gate
	{
	public:
		static constexpr double kDownSeconds = 2.0;      // over budget this long: one step down
		static constexpr double kUpSeconds = 5.0;        // under u - 0.10 this long: one step up
		static constexpr double kUpAfterDownSeconds = 10.0;
		static constexpr double kUpMargin = 0.10;

		void Reset()
		{
			level = 0;
			overSince = -1.0;
			underSince = -1.0;
			lastDown = -1e9;
		}
		int Level() const { return level; }

		// The next level down (more degraded) / up, skipping L1 when it changes nothing.
		static int Down(int level, bool l1Available, int maxExtras)
		{
			int next = level + 1;
			if (next == 1 && !l1Available) next = 2;
			return std::min(next, HoldLevel(maxExtras));
		}
		static int Up(int level, bool l1Available)
		{
			int next = level - 1;
			if (next == 1 && !l1Available) next = 0;
			return std::max(0, next);
		}

		// One step at a time: down after kDownSeconds over budget, or at once on a
		// late frame; up after kUpSeconds with the level above under u - 0.10, and no
		// sooner than kUpAfterDownSeconds after the last step down.
		GateChange Update(double now, const GateInput& in, bool lateFrame)
		{
			GateChange change;
			const double ratio = in.displayPeriod > 0.0 ? in.sourcePeriod / in.displayPeriod : 0.0;
			const int maxExtras = MaxExtras(ratio);
			// The cadence changed: the same hold at the new cadence, not a step down (bug
			// hunt 05 L1: it set lastDown, delaying recovery by 10 s, and logged a change
			// with no reason).
			if (level > HoldLevel(maxExtras))
				level = HoldLevel(maxExtras);
			change.from = level;
			const bool over = !Fits(in, level, in.utilisation);
			if (lateFrame && level < HoldLevel(maxExtras))
			{
				level = Down(level, in.l1Available, maxExtras);
				change.why = "late frame";
			}
			else if (over && level < HoldLevel(maxExtras))
			{
				if (overSince < 0.0) overSince = now;
				if (now - overSince >= kDownSeconds)
				{
					level = Down(level, in.l1Available, maxExtras);
					change.why = "over budget for 2 s";
				}
			}
			else
				overSince = -1.0;
			if (level != change.from)
			{
				lastDown = now;
				overSince = -1.0;
				underSince = -1.0;
			}
			else if (level > 0 && Fits(in, Up(level, in.l1Available), in.utilisation - kUpMargin))
			{
				if (underSince < 0.0) underSince = now;
				if (now - underSince >= kUpSeconds && now - lastDown >= kUpAfterDownSeconds)
				{
					level = Up(level, in.l1Available);
					underSince = -1.0;
					change.why = "under budget for 5 s";
				}
			}
			else
				underSince = -1.0;
			change.to = level;
			change.changed = change.to != change.from;
			return change;
		}

	private:
		int level = 0;
		double overSince = -1.0;
		double underSince = -1.0;
		double lastDown = -1e9;
	};

	// Lateness that moves the ladder only when sustained (bug hunt 05 M5): an open-loop
	// drift correction (a present seen late by its QPC return, Schedule::NoteLate)
	// moves the chain and drops an extra, but on its own it is the plan's display rate
	// drifting from the display, not the GPU falling behind. kCount corrections within
	// kWindowSeconds are lateness for the gate.
	class SustainedLateness
	{
	public:
		static constexpr size_t kCount = 2;
		static constexpr double kWindowSeconds = 2.0;

		void Reset() { times.clear(); }
		// A correction at `now`; true when it makes kCount within the window (the gate
		// steps down, and the count starts again).
		bool Note(double now)
		{
			while (!times.empty() && now - times.front() > kWindowSeconds)
				times.pop_front();
			times.push_back(now);
			if (times.size() < kCount)
				return false;
			times.clear();
			return true;
		}

	private:
		std::deque<double> times;
	};

	// What the gate's measurements belong to (bug hunt 05 H3): a new queue generation,
	// a new cadence (refreshes per source frame) or render_ahead switched on starts the
	// gate, its cost windows and the queue credit again. Changed reports a new epoch
	// (the first call too).
	class GateEpoch
	{
	public:
		static constexpr double kCadenceChange = 0.02;   // as the de-judder's phase reset

		void Invalidate() { valid = false; }
		bool Changed(uint64_t generation, double cadenceRatio)
		{
			const bool changed = !valid || generation != currentGeneration ||
				!(std::abs(cadenceRatio - currentRatio) <= kCadenceChange);
			valid = true;
			currentGeneration = generation;
			if (changed)
				currentRatio = cadenceRatio;
			return changed;
		}

	private:
		bool valid = false;
		uint64_t currentGeneration = 0;
		double currentRatio = 0.0;
	};

	inline std::string LevelName(int level, int maxExtras)
	{
		if (level <= 0) return "full";
		if (level >= HoldLevel(maxExtras)) return "hold";
		if (level == 1) return "L1 (de-judder vectors for the sigma-delta)";
		return "drop " + std::to_string(DropsAt(level)) + " extra(s) per frame";
	}

	// ---------------------------------------------------------------------
	// The Presenter API (M2 P6-4, doc 77 decision 4: interface first).
	//
	// The render-ahead presentation state - the present chain, the gate and its cost
	// windows, the epoch, the drift lateness, the queue credit and the chain depth -
	// and the decisions made on it each frame, in one object the renderer owns
	// (render-loop thread, except the two atomics). Pure: no GPU, no logging; the
	// methods take `now` from the caller (Now() is the steady clock the renderer
	// passes, P6 rest), and the caller logs from the results, so its log lines and
	// diagnostics stay where they were. The GPU timers stay renderer objects passed in
	// (ResolveTimers). P6 rest (doc 93 WAVE 3) adds the delay-budget activation, the
	// commit, the named chain restart and the present bookkeeping the de-judder
	// extension reads (P5c), so the extras read one object; the renderer keeps one
	// alias per moved member. Behaviour is the renderer's before P6-4, line for line.

	// What the gate needs from the renderer for one source period.
	struct PresenterGateFrame
	{
		double sourcePeriod = 0.0;       // P_s (s)
		double displayPeriod = 0.0;      // P_d (s)
		double utilisation = 0.85;       // render_ahead_utilisation
		bool vpOwnedChain = false;       // the VP-owned chain exists: its latency is Q, else 2
		bool l1Available = false;        // dither_vectors own and the sigma-delta supports vectors
		double ditherMsOwn = 0.0;        // the sigma-delta's measured ms with its own search
		double ditherMsExternal = 0.0;   // ... and with the de-judder's vectors
		bool late = false;               // a late frame (steps the ladder down at once)
		int maxExtras = 0;               // the most extras a source frame owns at this cadence
	};

	// The gate's answer, with what the caller logs.
	struct PresenterGateResult
	{
		int level = 0;                   // the ladder's level after the update
		bool evaluated = false;          // Gate::Update ran (measured, or a late frame)
		bool probing = false;            // degraded with no extra cost known (one step up probes)
		GateChange change;               // valid when evaluated
		GateInput input;                 // the costs and budget the gate saw
	};

	// One queued plan, as the renderer commits it (Presenter::Commit).
	struct PresenterCommit
	{
		double nextFirstVsync = 0.0;     // FramePlan::nextFirstVsync: where B lands
		int nextInterval = 1;            // FramePlan::nextInterval: B's SyncInterval
		uint32_t expectedPresent = 0;    // the present id B should get (last + 1; 0 = not known)
		int lateRefreshes = 0;           // open loop: the most an extra was late by its QPC return (0: none)
		double displayPeriod = 0.0;      // P_d (s), for the open-loop lateness
		bool storeCredit = false;        // an extras frame: the queue credit is (re)stored
		bool queueCredit = false;        // render_ahead_queue_credit
		double cadenceRatio = 0.0;       // refreshes per source frame (the credit latch)
	};

	// UpdateBudget's answer: whether the veto changes (the caller logs, then
	// ApplyBudgetVeto) and the new veto.
	struct PresenterBudgetChange
	{
		bool changed = false;
		bool veto = false;
	};

	class Presenter
	{
	public:
		// The steady clock in seconds (the renderer's RenderAheadNow).
		static double Now()
		{
			return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
		}

		Schedule schedule;
		Gate gate;
		CostWindow newFrameCost;
		// X per vector mode (bug hunt 05 M7): the sigma-delta's own search / the de-judder's
		// vectors; E, the estimate ahead. Each holds its last p95 for a while when it empties.
		KnownCost extraCostOwn{ 4 }, extraCostExternal{ 4 }, estimateCost{ 1 };
		GateEpoch epoch;                 // generation / cadence / render_ahead on (H3)
		SustainedLateness driftLateness; // open-loop drift corrections (M5)
		CreditLatch credit;              // render_ahead_queue_credit, latched (M4)
		ChainDepth chainDepth;           // the VP-owned chain as created (M6)
		unsigned long long gateResets = 0;
		bool committed = false;          // this frame's plan was queued
		bool l1 = false;                 // the ladder's L1: the de-judder's vectors for the sigma-delta
		int mainInterval = 1;            // SyncInterval of the next main present (holds after the last extra)
		double targetLeadMs = 0.0;       // A's planned refresh after its Present returned (ms)
		std::atomic<double> estimatedLeadMs{ 0.0 };   // open loop: the planned lead, an estimate (L3); 0 = none (any thread)
		std::atomic<size_t> creditFrames{ 0 };        // render_ahead_queue_credit: frames the queue target gives back (any thread)
		int lastMainInterval = 1;        // the last main present's SyncInterval (diagnostics)
		unsigned long long intervalPresents = 0;   // main presents made after holds

		// PHASE 1 of the host seams plan (refs/107 s2.4 item 1; owner E3 2026-11-11): Bill's present path has no
		// render-ahead, so in the Original pathway every report below returns at once (`// VPV-GATED`, verified by
		// tools/check_hook_gating.py). The fact comes from the host's settings seam (CoreSegments.inl
		// UpdatePipelineSlots); ENTERING Original breaks the chain and resets the gate once, ungated, so no plan,
		// credit or ladder level outlives the switch. Render thread, as the reports. Returns true when entered.
		bool originalPathway = false;
		bool SetOriginalPathway(bool on, double now)
		{
			const bool entering = on && !originalPathway;
			originalPathway = false;   // the hand-over runs ungated
			if (entering)
			{
				Break(now);
				ResetGate();
			}
			originalPathway = on;
			return entering;
		}
		// r6 fix 4 (P6 rest S1): render-ahead only inside the fixed delay budget
		// (FitDelayBudget); vetoed, the plain per-refresh path runs as with render_ahead off.
		bool budgetVeto = false;
		size_t budgetCredit = 0;         // the frames the budget lets the queue give back
		// The analysis floor's inputs (S1, TD-readiness 93 (a)2): the frames the queue keeps for
		// its look-ahead users (any thread; 0 = none) and who gives way when it competes with
		// the credit (the owner's policy; CreditWins = today).
		std::atomic<size_t> analysisFloor{ 0 };
		FloorPolicy floorPolicy = FloorPolicy::CreditWins;
		// The spare-slot GPU ms per source frame a look-ahead user declares (S1 amendment; any
		// thread; 0 = none): GateCosts::spareSlotMs, added to the gate's average check.
		std::atomic<double> spareSlotMs{ 0.0 };
		// The vector mode each extra's GPU timer ran with (timer serial -> de-judder vectors).
		std::deque<std::pair<unsigned long long, bool>> extraTimerModes;

		// Present bookkeeping (P6 rest S3), read by the de-judder extension (P5c).
		// Present ids of recent extras and the refresh each main present (A) showed on, so
		// presentation telemetry sees what it would without extras; ids belong to one
		// swapchain (identity only, not owned).
		std::deque<uint32_t> extraIds;
		std::deque<std::pair<uint32_t, uint32_t>> mainRefresh;
		const void* idsSwapchain = nullptr;
		// The display rate the extras plan with (OpenLoopDisplayRate::Choice; DisplayRateSource, doc 46).
		OpenLoopDisplayRate::Choice displayRate;
		uint32_t expectedPresent = 0;    // the present id the next main present should get (chain continuity)
		double lead = 0.0;               // the planned lead (s)
		unsigned long long frames = 0, restarts = 0, late = 0, dropped = 0, held = 0;
		// Diagnostics (render-ahead follow-up 2026-10-17): each planned present's SyncInterval
		// and, from the statistics, shown - planned in refreshes per kind (interval 1 or a
		// present after holds).
		std::deque<std::pair<uint32_t, int>> presentIntervals;
		unsigned long long observedSeen = 0;
		double shownError[2] = { 0.0, 0.0 };
		unsigned long long shownCount[2] = { 0, 0 }, shownOnTime[2] = { 0, 0 };
		// Open loop (no statistics).
		unsigned long long openLoopFrames = 0, lateByPresent = 0;
		unsigned long long flushReanchors = 0;   // new anchors after a backlog flush
		bool openLoopLate = false;       // seen on the last frame's extras: the gate's lateness for this one

		// The chain ends (a frame not queued by the plan): the next present is not
		// predicted. The queue credit is latched (bug hunt 05 M4): a single break keeps
		// it; it is returned once the chain has stayed broken for CreditLatch::kReleaseSeconds.
		void Break(double now)   // VPV-GATED
		{
			if (originalPathway) return;
			schedule.Break();
			mainInterval = 1;
			targetLeadMs = 0.0;
			creditFrames.store(credit.Break(now), std::memory_order_release);
			estimatedLeadMs.store(0.0, std::memory_order_relaxed);
		}

		// A new epoch for the gate (bug hunt 05 H3): the ladder, its cost windows, the
		// held costs, the drift-lateness count and the queue credit start again.
		// Returns the level before the reset (0 in the Original pathway: nothing to reset).
		int ResetGate()   // VPV-GATED
		{
			if (originalPathway) return 0;
			const int level = gate.Level();
			gate.Reset();
			newFrameCost.Reset();
			extraCostOwn.Reset();
			extraCostExternal.Reset();
			estimateCost.Reset();
			driftLateness.Reset();
			credit.Reset();
			creditFrames.store(0, std::memory_order_release);
			l1 = false;
			++gateResets;
			return level;
		}

		// The gate per source period (dejudder_gate period): p95 costs over 5 s, the
		// average and single-frame budgets, the ladder one step at a time.
		PresenterGateResult UpdateGate(double now, const PresenterGateFrame& frame)   // VPV-GATED
		{
			if (originalPathway) return PresenterGateResult();   // not evaluated, level 0
			newFrameCost.Trim(now);
			extraCostOwn.Trim(now);
			extraCostExternal.Trim(now);
			estimateCost.Trim(now);
			PresenterGateResult result;
			GateInput& g = result.input;
			g.sourcePeriod = frame.sourcePeriod;
			g.displayPeriod = frame.displayPeriod;
			g.utilisation = frame.utilisation;
			// The frame latency the VP-owned chain was made with (bug hunt 05 M6: today's 2
			// without extras or in low latency).
			g.queueDepth = frame.vpOwnedChain ? static_cast<int>(chainDepth.latency) : 2;
			g.l1Available = frame.l1Available;
			g.costs.newFrameMs = newFrameCost.P95();
			g.costs.estimateMs = estimateCost.Current();
			g.costs.spareSlotMs = spareSlotMs.load(std::memory_order_acquire);
			// X per vector mode, each level priced with its own (M7). A degraded level renders
			// fewer extras (none at hold), so their windows empty: the last p95 measured
			// stands for them (KnownCost, 30 s), and with none known the ladder probes one
			// step up with the extra's cost unknown.
			const double saving = frame.ditherMsOwn > 0.0 && frame.ditherMsExternal > 0.0 ?
				frame.ditherMsOwn - frame.ditherMsExternal : 0.0;
			const GateCosts extra = ExtraCosts(extraCostOwn.Current(), extraCostExternal.Current(), saving,
				g.l1Available);
			g.costs.extraMs = extra.extraMs;
			g.costs.extraCheaperMs = extra.extraCheaperMs;
			const bool degraded = gate.Level() > 0;
			const bool extraMeasured = extraCostOwn.Measured() || extraCostExternal.Measured();
			const bool extraKnown = extraCostOwn.Known() || extraCostExternal.Known();
			result.probing = degraded && !extraKnown;
			const bool measured = newFrameCost.Count() >= 10 &&
				(frame.maxExtras == 0 || extraMeasured || degraded);
			if (measured || frame.late)
			{
				if (!measured)
					g.costs = GateCosts();
				result.change = gate.Update(now, g, frame.late);
				result.evaluated = true;
			}
			result.level = gate.Level();
			l1 = g.l1Available && result.level >= 1 && result.level < HoldLevel(frame.maxExtras);
			return result;
		}

		// The SyncInterval for the main present now being made: the holds the last plan
		// left after its last extra (taken at the main present's entry, so every exit
		// consumes it, bug hunt 05 L2), 1 otherwise; at most kMaxSyncInterval.
		int TakeMainInterval(int pending)   // VPV-GATED
		{
			if (originalPathway) return 1;   // Bill presents every main frame at SyncInterval 1
			const int interval = std::max(1, std::min(pending, kMaxSyncInterval));
			lastMainInterval = interval;
			if (interval > 1)
				++intervalPresents;
			return interval;
		}

		// ---- P6 rest S1: the delay budget and the activation ----

		// render_ahead on and inside the delay budget.
		bool Active(bool renderAheadOn) const { return renderAheadOn && !budgetVeto; }

		// render_ahead off: no veto, no credit (the caller releases its fallback).
		void ClearBudget()
		{
			budgetVeto = false;
			budgetCredit = 0;
		}

		// The budget's decision for this source: the credit is stored; the veto is only
		// reported (the caller logs a change, then calls ApplyBudgetVeto).
		PresenterBudgetChange UpdateBudget(const BudgetDecision& d)
		{
			budgetCredit = d.fits ? d.creditFrames : 0;
			PresenterBudgetChange change;
			change.veto = !d.fits;
			change.changed = change.veto != budgetVeto;
			return change;
		}

		// The budget for this source with the Presenter's floor inputs.
		BudgetDecision FitBudget(double sourcePeriod, double displayPeriod, size_t queueFrames, bool creditOn) const
		{
			return FitDelayBudget(sourcePeriod, displayPeriod, queueFrames, creditOn,
				analysisFloor.load(std::memory_order_acquire), floorPolicy);
		}

		// The visible line when the floor and the credit compete (both policies; empty when
		// they do not): what was vetoed or kept and why.
		static std::string FloorReport(const BudgetDecision& d, FloorPolicy policy)
		{
			if (!d.floorCompetes)
				return std::string();
			char line[240];
			if (policy == FloorPolicy::FloorWins)
				snprintf(line, sizeof(line), "the analysis floor (%zu frames) wins: render-ahead's credit is vetoed, "
					"its %.0f ms lead does not fit", d.floorFrames, d.leadMs);
			else if (policy == FloorPolicy::RaiseQueue && d.raisedQueueFrames > 0)
				snprintf(line, sizeof(line), "the analysis floor (%zu frames) raises the queue above the desired %zu frames: "
					"the real delay is %.0f ms, not %.0f ms (%.0f ms budget)", d.floorFrames, d.queueFrames, d.realTotalMs,
					d.totalMs, d.budgetMs);
			else
				snprintf(line, sizeof(line), "the credit wins over the analysis floor (%zu frames): the queue stays at the "
					"floor, so the real delay is %.0f ms, not %.0f ms (%.0f ms budget)", d.floorFrames, d.realTotalMs,
					d.totalMs, d.budgetMs);
			return line;
		}

		// A changed veto: off breaks the chain and returns the credit at once; on starts
		// the gate again (as render_ahead switched on).
		void ApplyBudgetVeto(bool veto, double now)
		{
			if (veto)
			{
				Break(now);
				credit.Reset();
				creditFrames.store(0, std::memory_order_release);
			}
			else
				epoch.Invalidate();
			budgetVeto = veto;
		}

		// The vector mode an extra's GPU timer ran with (true: the de-judder's vectors), by
		// its submission serial; older entries are dropped. Unknown: `lastExternal` (the
		// last extra's).
		bool TakeExtraTimerMode(unsigned long long serial, bool lastExternal)
		{
			bool external = lastExternal;
			while (!extraTimerModes.empty() && extraTimerModes.front().first <= serial)
			{
				if (extraTimerModes.front().first == serial)
					external = extraTimerModes.front().second;
				extraTimerModes.pop_front();
			}
			return external;
		}

		// A timed extra: its serial's vector mode (the 64 newest kept).
		void NoteExtraTimerMode(unsigned long long serial, bool external)
		{
			extraTimerModes.emplace_back(serial, external);
			while (extraTimerModes.size() > 64)
				extraTimerModes.pop_front();
		}

		// The extras' (X) and the estimate ahead's (E) GPU times, resolved late, never
		// flushing: up to 8 per timer into the cost windows. The timers and their context
		// stay the renderer's (AlphaD3D11FrameGpuTimer); the caller checks they exist.
		template <class Timer, class Context>
		void ResolveTimers(Timer& extraTimer, Timer& estimateTimer, Context* context, double now, bool lastExternal)
		{
			for (int timer = 0; timer < 2; ++timer)
			{
				Timer& source = timer == 0 ? extraTimer : estimateTimer;
				for (int resolved = 0; resolved < 8; ++resolved)
				{
					typename Timer::Sample sample;
					const typename Timer::ResolveResult result = source.TryResolveOldest(context, sample);
					if (result == Timer::ResolveResult::Resolved)
					{
						if (timer == 1)
							estimateCost.Add(now, sample.milliseconds);
						else
							(TakeExtraTimerMode(sample.submissionSerial, lastExternal) ? extraCostExternal :
								extraCostOwn).Add(now, sample.milliseconds);
					}
					else if (result != Timer::ResolveResult::Discarded)
						break;
				}
			}
		}

		// ---- P6 rest S2: the commit ----

		// A frame starts: its plan is not queued yet.
		void BeginFrame() { committed = false; }

		// The plan is queued: B lands at the refresh after A's last one, with the holds
		// after the last extra as its interval. Open-loop lateness moves the chain as a
		// statistics-observed late present would (the first after a new anchor re-anchors
		// only, bug hunt 05 M2). An extras frame stores the queue credit, latched while the
		// cadence holds (M4); a hold-only frame leaves it as it is.
		void Commit(const PresenterCommit& c, const std::vector<std::pair<uint32_t, double>>& plannedIds)
		{
			schedule.Commit(c.nextFirstVsync, plannedIds);
			if (c.lateRefreshes > 0)
				openLoopLate = schedule.NoteLate(c.lateRefreshes, c.displayPeriod);
			mainInterval = c.nextInterval;
			expectedPresent = c.expectedPresent;
			committed = true;
			if (c.storeCredit)
				creditFrames.store(c.queueCredit ? credit.CommitFrames(budgetCredit, c.cadenceRatio) : 0,
					std::memory_order_release);
		}

		// Open loop (hunt 2 05-renderahead L3): the target is not measured, so a queued
		// frame's planned lead is published as an estimate; 0 otherwise.
		void PublishEstimatedLead(bool targetTimingKnown)
		{
			estimatedLeadMs.store(committed && !targetTimingKnown && displayRate.openLoop ? targetLeadMs : 0.0,
				std::memory_order_relaxed);
		}

		// ---- P6 rest S4: the named chain restart ----

		// A new anchor only: the schedule's pending presents no longer apply. NOT Break(now):
		// the main interval, the target lead, the latched credit and the estimate stay (the
		// renderer's six direct schedule breaks - a new generation or cadence, a resync, a
		// present outside the plan, a backlog flush, a restart, a re-anchor - as before P6).
		void RestartChain() { schedule.Break(); }

		// ---- P6 rest S3: the present bookkeeping ----

		// Present ids belong to one swapchain: another one starts them again.
		void NoteIdsSwapchain(const void* swapchain)
		{
			if (idsSwapchain != swapchain)
			{
				extraIds.clear();
				mainRefresh.clear();
				idsSwapchain = swapchain;
			}
		}

		// Teardown: no ids, no swapchain.
		void ClearPresentIds()
		{
			extraIds.clear();
			mainRefresh.clear();
			idsSwapchain = nullptr;
		}

		// An extra was presented with this id (the 256 newest kept).
		void NoteExtraPresent(uint32_t presentId)
		{
			extraIds.push_back(presentId);
			if (extraIds.size() > 256)
				extraIds.pop_front();
		}

		// The main present `presentId` showed on refresh `refresh` (the 64 newest kept).
		void NoteMainRefresh(uint32_t presentId, uint32_t refresh)
		{
			mainRefresh.emplace_back(presentId, refresh);
			if (mainRefresh.size() > 64)
				mainRefresh.pop_front();
		}

		// The statistics' latest present: kept as a main present's refresh unless it is
		// an extra or already the newest kept.
		void NoteStatisticsPresent(uint32_t presentId, uint32_t refresh)
		{
			if (std::find(extraIds.begin(), extraIds.end(), presentId) == extraIds.end() &&
				(mainRefresh.empty() || mainRefresh.back().first != presentId))
				NoteMainRefresh(presentId, refresh);
		}

		// A planned present's SyncInterval (diagnostics); `keep` > 0 keeps that many newest.
		void NotePresentInterval(uint32_t presentId, int interval, size_t keep)
		{
			presentIntervals.emplace_back(presentId, interval);
			if (keep > 0)
				while (presentIntervals.size() > keep)
					presentIntervals.pop_front();
		}

		// A new observation by the schedule: its shown - planned error counted per kind
		// (interval 1, or a present after holds) when its interval is known.
		void NoteObservation()
		{
			if (schedule.ObservedCount() == observedSeen)
				return;
			observedSeen = schedule.ObservedCount();
			const uint32_t id = schedule.LastObservedId();
			const auto known = std::find_if(presentIntervals.begin(), presentIntervals.end(),
				[id](const std::pair<uint32_t, int>& p) { return p.first == id; });
			if (known != presentIntervals.end())
			{
				const int kind = known->second > 1 ? 1 : 0;
				const double e = schedule.LastObservedError();
				shownError[kind] += e;
				++shownCount[kind];
				if (std::abs(e) <= 0.5) ++shownOnTime[kind];
			}
		}
	};
}
