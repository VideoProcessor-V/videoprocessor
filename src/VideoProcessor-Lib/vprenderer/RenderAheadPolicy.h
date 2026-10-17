#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <utility>
#include <vector>

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

	// render_ahead_queue_credit: whole source frames the queue target may give back
	// for the lead (the latency the queued extras add), never below 2 queued frames
	// (B must still be queued when A renders).
	inline size_t QueueCredit(double lead, double sourcePeriod, size_t desiredDepth)
	{
		if (!(lead > 0.0) || !(sourcePeriod > 0.0) || desiredDepth <= 2)
			return 0;
		const size_t frames = static_cast<size_t>(std::ceil(lead / sourcePeriod - 1e-9));
		return std::min(frames, desiredDepth - 2);
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

	inline float ShiftFor(const FrameInput& in, double delta)
	{
		if (!(delta > kPhaseEpsilon))
			return 0.0f;
		if (in.evenCycle > 0)
		{
			// Delta 10: an even cadence's extras sit at the exact phases k/N.
			const double k = std::floor(delta * in.evenCycle + 1e-6);
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
		plan.phi0 = Phase(in, in.firstVsync);
		// A owns refreshes while the phase is below 1; at most its extras, the restart
		// lead's refreshes and DXGI's longest run of holds (a long restart lead is shown
		// as holds and re-rendered A's at most, all on A: B then continues the chain).
		const int leadRefreshes = in.lead > 0.0 ?
			static_cast<int>(std::ceil(in.lead / in.displayPeriod - 1e-6)) : 0;
		const int ownedCap = std::max(1, in.maxExtras + 1 + (kMaxSyncInterval - 1) + std::min(leadRefreshes, 16));
		int refreshes = 1;
		while (refreshes < ownedCap &&
			Phase(in, in.firstVsync + refreshes * in.displayPeriod) < 1.0 - 1e-9)
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
			pending.clear();
		}

		bool Active() const { return active; }
		double NextVsync() const { return nextVsync; }

		// The vsync A's own present (made at `now`) lands on: the chain's next
		// refresh when it is still ahead, else (a restart, or the chain fell behind)
		// the first refresh at or after `earliest`, on the vsync grid when known
		// (gridVsync > 0: any vsync seen on the display). restart reports a new anchor;
		// behind, that the chain was active but its refresh had passed (the frame is
		// late: the next frame drops one extra).
		double FirstVsync(double earliest, double displayPeriod, double gridVsync, bool& restart, bool& behind) const
		{
			restart = !active || !(nextVsync >= earliest - 0.25 * displayPeriod);
			behind = active && restart;
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
			active = true;
			nextVsync = nextFirstVsync;
			for (const auto& p : planned)
				pending.push_back(p);
			while (pending.size() > 64)
				pending.pop_front();
		}

		void Break() { active = false; pending.clear(); }

		// DXGI saw present `presentId` on screen at vsync `actual`. A present shown
		// more than half a refresh after its planned vsync is late: the chain moves by
		// the difference, and the next frame drops one extra (design step 4). Returns
		// true when late.
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
				if (error > 0.5 * displayPeriod)
				{
					late = true;
					lateDrops = 1;
					const double refreshes = std::round(error / displayPeriod);
					nextVsync += refreshes * displayPeriod;
					for (auto& p : pending)
						p.second += refreshes * displayPeriod;
				}
			}
			return late;
		}

		// The drops lateness asks of the next frame (consumed).
		int TakeLateDrops()
		{
			const int drops = lateDrops;
			lateDrops = 0;
			return drops;
		}

		size_t Pending() const { return pending.size(); }

	private:
		bool active = false;
		double nextVsync = 0.0;
		int lateDrops = 0;
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
	};

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
		return in.costs.newFrameMs + (extras > 0.0 ? in.costs.estimateMs : 0.0) + extras * x;
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
			change.from = level;
			const double ratio = in.displayPeriod > 0.0 ? in.sourcePeriod / in.displayPeriod : 0.0;
			const int maxExtras = MaxExtras(ratio);
			if (level > HoldLevel(maxExtras))
				level = HoldLevel(maxExtras);   // the cadence changed
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

	inline std::string LevelName(int level, int maxExtras)
	{
		if (level <= 0) return "full";
		if (level >= HoldLevel(maxExtras)) return "hold";
		if (level == 1) return "L1 (de-judder vectors for the sigma-delta)";
		return "drop " + std::to_string(DropsAt(level)) + " extra(s) per frame";
	}
}
