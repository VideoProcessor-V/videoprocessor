#pragma once

#include <IntentDescriptor.h>

#include <cstdint>
#include <string>

// Bug hunt 2 (2026-10-17): pure policies for the GUI lifecycle fixes, kept out
// of the dialog so the CPU tests can check them.

// H1/M1/L5: the application shutdown deadline. It is armed when termination is
// first requested (OnClose) and polled from the 1 s UI timer whatever blocks
// the exit: a renderer retirement still in the worker (it may never return), a
// failed or fail-closed retirement, a renderer construction, a capture stop
// that never reaches READY, or a renderer that never stops. At expiry the exit
// is forced: LED (CNTR / dimming) restore, target-only topology restore, crash
// state KEPT (a forced exit is not a clean exit), then the process ends. A
// watchdog thread does the same if the UI thread itself is blocked.
namespace ShutdownDeadline
{
	// From the first close request to the forced exit. Long enough for a slow
	// but healthy renderer/graph stop; the fail-closed retirement keeps its own
	// shorter deadline (ShutdownBlockedRetirementDeadlineMs) once it is known.
	constexpr uint32_t ForcedExitDeadlineMs = 20000u;
	// Extra time the watchdog thread gives the UI thread's own forced exit
	// (LED restores, topology restore) before it acts itself.
	constexpr uint32_t WatchdogGraceMs = 15000u;
	// After DoModal returns: how long the application waits for the renderer
	// retirement worker and reset coordinator before the exit is forced.
	constexpr uint32_t WorkerQuiesceTimeoutMs = 5000u;
	// Process exit code of a forced exit (a clean exit returns 0).
	constexpr uint32_t ForcedExitCode = 3u;

	struct Blockers
	{
		bool retirementPending = false;       // worker still inside Retire()
		bool failedRetirement = false;        // retained renderer awaiting retry
		bool retirementShutdownBlocked = false; // fail-closed shutdown retirement
		bool constructionActive = false;
		bool captureStopping = false;         // capture STOPPING (or stop in progress)
		bool rendererPresent = false;         // renderer not yet stopped / removed
		bool capturePresent = false;          // capture device not yet removed
	};

	inline bool AnyBlocking(const Blockers& b)
	{
		return b.retirementPending || b.failedRetirement ||
			b.retirementShutdownBlocked || b.constructionActive ||
			b.captureStopping || b.rendererPresent || b.capturePresent;
	}

	// The most specific blocking kind, for the log.
	inline const char* Describe(const Blockers& b)
	{
		if (b.retirementShutdownBlocked) return "retirement-shutdown-blocked";
		if (b.retirementPending) return "retirement-pending";
		if (b.failedRetirement) return "retirement-failed";
		if (b.constructionActive) return "construction-active";
		if (b.captureStopping) return "capture-stopping";
		if (b.rendererPresent) return "renderer-not-stopped";
		if (b.capturePresent) return "capture-not-removed";
		return "none";
	}

	enum class Decision
	{
		NotArmed,   // termination not requested
		Waiting,    // armed, not yet due
		Expire,     // due now: force the exit (returned once)
		Expired     // already forced
	};

	class Tracker
	{
	public:
		// Arms once, at the first termination request. Later calls keep the
		// first deadline. Returns true when this call armed it.
		bool Arm(uint64_t now, uint32_t deadlineMs = ForcedExitDeadlineMs)
		{
			if (m_deadline != 0)
				return false;
			m_armedAt = now;
			m_deadline = now + deadlineMs;
			return true;
		}
		// Brings the deadline forward (never later), e.g. when the retirement
		// is known to be fail-closed. Arms it when not armed.
		void BringForward(uint64_t now, uint32_t deadlineMs)
		{
			const uint64_t candidate = now + deadlineMs;
			if (m_deadline == 0)
			{
				m_armedAt = now;
				m_deadline = candidate;
			}
			else if (candidate < m_deadline)
				m_deadline = candidate;
		}
		Decision Evaluate(uint64_t now)
		{
			if (m_deadline == 0)
				return Decision::NotArmed;
			if (m_expired)
				return Decision::Expired;
			if (now < m_deadline)
				return Decision::Waiting;
			m_expired = true;
			return Decision::Expire;
		}
		bool Armed() const { return m_deadline != 0; }
		uint64_t Deadline() const { return m_deadline; }
		uint64_t ArmedAt() const { return m_armedAt; }
	private:
		uint64_t m_armedAt = 0;
		uint64_t m_deadline = 0;
		bool m_expired = false;
	};

	// What a forced exit does. Every step is unconditional: the projector's
	// light/CNTR and the desktop topology are restored whatever blocked the
	// exit, and the crash state is kept for the next start.
	struct ForcedExitPlan
	{
		bool restoreLed = true;
		bool restoreTopology = true;
		bool keepCrashState = true;
		bool terminateProcess = true;
	};

	inline ForcedExitPlan PlanForcedExit(const Blockers&)
	{
		return ForcedExitPlan{};
	}

	// The app's verdict on the crash state after DoModal returned.
	inline bool CrashStateClean(bool fatalErrorRaised, bool dialogCouldNotRun,
		bool forcedShutdown, bool workersQuiesced)
	{
		return !fatalErrorRaised && !dialogCouldNotRun && !forcedShutdown &&
			workersQuiesced;
	}
}

// H2: the rules' `content` value is held across renderer stops and restarts.
// It used to become "" whenever the renderer was not RENDERING (and a new
// renderer starts its classifier from Unknown), so a content-ruled profile
// flipped to the fallback on every restart and back once reclassified - an
// endless loop when that profile itself needs a renderer restart. Now the
// value changes only on a real classification change: a renderer that is
// rendering reports a known class different from the held one, or reports
// that it has no classifier / intent_content is off (then ""). Not rendering,
// no renderer, or Unknown while (re)classifying all keep the held value.
class ContentRuleHold
{
public:
	// rendering: a renderer exists and is RENDERING. statusAvailable: its
	// GetContentIntentStatus returned true (false = no classifier, or the
	// switch is off). Returns the value the rules should see.
	const std::string& Update(bool rendering, bool statusAvailable,
		const ContentIntentStatus& status)
	{
		if (!rendering)
			return m_value;
		if (!statusAvailable || !status.enabled)
		{
			m_value.clear();
			return m_value;
		}
		const std::string measured = ContentRuleValue(status);
		if (!measured.empty())
			m_value = measured;
		return m_value;
	}
	const std::string& Value() const { return m_value; }
	void Clear() { m_value.clear(); }
private:
	std::string m_value;
};
