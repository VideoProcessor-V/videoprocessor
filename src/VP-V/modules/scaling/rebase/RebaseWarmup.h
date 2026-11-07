// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <VP-V/core/FallbackCatalog.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

// Re-base shader warm-up gate (scaling.rebase; fix FX-RB). A re-base to a new plane size needs shader
// variants libplacebo has never compiled (GLSL -> SPIR-V -> HLSL -> DXBC, seconds on a cold cache). On
// the render thread that is a multi-second stall and dozens of dropped frames. This gate holds the
// frame on the CURRENT path until a worker has compiled the variants on a private device into the
// shared pl_cache, then lets the re-base proceed. Pure, header-only, C++14: no libplacebo, no Windows,
// no clock of its own (the caller passes seconds), so it is unit-tested on the CPU. One instance, used
// from the render thread only; the worker's progress arrives as `Status`.
//
// A hold is never open-ended: every wanted re-base has a HOLD BUDGET (an epoch, from its first held
// frame, kept across candidate-key changes) after which the re-base is let through visibly (status +
// log + fallback entry), compiling on demand like before this gate existed.
//
// Nothing here runs unless a re-base is wanted: with source_resolution off/native the gate is never
// asked and no state changes (the Off / Original behaviour is untouched).
namespace VpRebaseWarmup
{
	// The worker's view of one candidate key (mirrors NlsPipelinePreparation::Status).
	enum class Status { Missing, Pending, Ready, Failed };

	struct Step
	{
		bool allow = true;     // true: the re-base may run this frame; false: stay on the current path
		bool submit = false;   // true: hand the candidate to the worker now (the caller owns the frames)
		bool pending = false;  // the status line / "preparing" flag: warm-up in flight
		bool failed = false;   // visible failure: warm-up unavailable or too slow, current path until retry
		bool bypass = false;   // allowed WITHOUT a compiled warm-up (budget, failures, no key, no cache); visible
		std::string log;       // a line to log (only on a state change; empty otherwise)
		std::string status;    // a plain-language status for the fallback/status surfaces (empty = none)
	};

	// The "Fallbacks engaged" entry for a status: engaged while there is a status, released otherwise.
	inline FallbackCatalog::Decision FallbackDecision(const std::string& status)
	{
		return status.empty() ? FallbackCatalog::Released("re-base warm-up") :
			FallbackCatalog::Engaged("re-base warm-up", status);
	}

	class Gate
	{
	public:
		// A warm-up that has not finished after this long counts as one failure (the worker keeps going;
		// a later completion is still honoured because Ready always wins).
		static constexpr double kTimeoutSeconds = 45.0;
		static constexpr double kRetryFirstSeconds = 5.0;
		static constexpr double kRetryMaxSeconds = 60.0;
		static constexpr size_t kMaxReadyKeys = 16;
		// After this many failed (or timed-out) warm-ups of one key the re-base is let through (compiling
		// on demand, the old behaviour), visibly: a worker that can never succeed must not disable it.
		static constexpr int kMaxFailures = 3;
		// The hold budget of one wanted re-base, from its first held frame until it is allowed. It does not
		// restart when the candidate key changes (churn must not hold forever) and covers a worker that
		// never answers (a refused submission leaves the key Missing).
		static constexpr double kHoldBudgetSeconds = 3.0 * kTimeoutSeconds;
		// A wanted re-base that is not evaluated for this long ends its epoch (a new wanted period starts
		// a new budget); shorter gaps (a one-frame flicker of `wanted`) do not.
		static constexpr double kEpochGraceSeconds = 10.0;

		// `wanted`: a re-base to `key` is wanted this frame (factor below 1 on an axis, not paused, not
		// temporal). `label` names the target plane size for the log ("1920x1080").
		Step Evaluate(bool wanted, const std::string& key, const std::string& label, Status status, double nowSeconds)
		{
			Step step;
			if (!wanted)
			{
				// No re-base wanted: the pending announcement ends; remembered Ready keys and the
				// candidate's failure history stay (the same candidate wanted again resumes, not restarts).
				announced = false;
				resumed = true;
				return step;
			}
			if (key.empty())
				return Skip("the frame contract cannot be keyed", label);
			skipReason.clear();
			if (key != activeKey)
			{
				// A new candidate restarts ITS wait and failure count; the epoch (hold budget) goes on.
				activeKey = key;
				since = nowSeconds;
				retryAt = 0.0;
				retrySeconds = kRetryFirstSeconds;
				announced = false;
				timedOut = false;
				failures = 0;
				bypassLogged = false;
				resumed = false;
			}
			else if (resumed)
			{
				since = nowSeconds;
				resumed = false;
			}
			if (IsKnownReady(key) || status == Status::Ready)
			{
				Remember(key);
				if (announced)
				{
					char text[160];
					std::snprintf(text, sizeof(text), "Re-base shader warm-up ready for %s after %.1f s; the re-base starts",
						label.c_str(), nowSeconds - since);
					step.log = text;
					announced = false;
				}
				timedOut = false;
				epochActive = false;
				epochBypass = false;
				return step;   // allow = true
			}
			if (!epochActive || nowSeconds - epochLast > kEpochGraceSeconds)
			{
				epochActive = true;
				epochStart = nowSeconds;
				epochBypass = false;
				epochLogged = false;
			}
			epochLast = nowSeconds;
			const double budget = kHoldBudgetSeconds;
			if (!epochBypass && nowSeconds - epochStart > budget)
				epochBypass = true;   // latched for the epoch: every later frame (and key) is allowed, visibly
			if (epochBypass)
			{
				char text[200];
				std::snprintf(text, sizeof(text), "re-base shader warm-up for %s did not finish within %.0f s; compiling on demand",
					label.c_str(), budget);
				step.bypass = true;
				step.status = text;
				if (!epochLogged)
				{
					epochLogged = true;
					step.log = std::string("Re-base shader warm-up for ") + label + " did not finish within " +
						std::to_string(static_cast<int>(budget)) + " s of holding; the re-base now compiles on demand (a stall is possible)";
				}
				return step;   // allow = true
			}
			if (failures >= kMaxFailures)
				return FailureBypass(label);
			step.allow = false;
			if (status == Status::Failed)
			{
				step.failed = true;
				step.status = "re-base waiting: shader warm-up for " + label + " failed; current path continues, retrying";
				if (nowSeconds >= retryAt)
				{
					if (++failures >= kMaxFailures) return FailureBypass(label);
					step.submit = true;
					retryAt = nowSeconds + retrySeconds;
					char text[200];
					std::snprintf(text, sizeof(text), "Re-base shader warm-up for %s failed; staying on the current path, retry in %.0f s",
						label.c_str(), retrySeconds);
					step.log = text;
					const double cap = kRetryMaxSeconds;
					retrySeconds = (std::min)(cap, retrySeconds * 2.0);
					since = nowSeconds;   // the retry's own wait starts now
					timedOut = false;
				}
				return step;
			}
			step.pending = true;
			if (status == Status::Missing)
				step.submit = true;   // not accepted by the worker yet (or refused): offered again
			if (nowSeconds - since > kTimeoutSeconds)
			{
				// Missing or Pending for too long: one failure (the worker keeps going; Ready still wins),
				// and the next wait starts now, so three of them hand the re-base back on demand.
				since = nowSeconds;
				if (++failures >= kMaxFailures) return FailureBypass(label);
				timedOut = true;
				step.log = "Re-base shader warm-up for " + label + " is taking too long (" + std::to_string(failures) + " of " +
					std::to_string(kMaxFailures) + "); staying on the current path until it finishes";
			}
			if (timedOut)
			{
				step.failed = true;
				step.pending = false;
				step.status = "re-base waiting: shader warm-up for " + label + " is taking too long; current path continues";
				return step;
			}
			if (!announced)
			{
				announced = true;
				step.log = "Re-base pending shader warm-up for " + label + " (current path continues)";
			}
			step.status = "re-base pending shader warm-up for " + label + " (current path continues)";
			return step;
		}

		// The warm-up cannot work for a reason known to the caller (no key, shader cache disabled): the
		// re-base proceeds on demand, visibly, and the reason is logged once until it changes.
		Step Skip(const std::string& why, const std::string& label)
		{
			Step step;
			step.bypass = true;
			step.status = "re-base shader warm-up skipped for " + label + " (" + why + "); compiling on demand";
			if (skipReason != why)
			{
				skipReason = why;
				step.log = "Re-base shader warm-up skipped for " + label + ": " + why +
					"; the re-base compiles on demand (a stall is possible)";
			}
			return step;
		}

		// The renderer got a Ready from the worker for a key it never evaluated (rare); remember it.
		void NoteReady(const std::string& key) { if (!key.empty()) Remember(key); }

		// A device / settings change: the compiled variants no longer apply.
		void Reset()
		{
			ready.clear();
			activeKey.clear();
			announced = false;
			timedOut = false;
			failures = 0;
			bypassLogged = false;
			resumed = false;
			epochActive = false;
			epochBypass = false;
			epochLogged = false;
			skipReason.clear();
		}

		bool IsKnownReady(const std::string& key) const
		{
			return std::find(ready.begin(), ready.end(), key) != ready.end();
		}

	private:
		Step FailureBypass(const std::string& label)
		{
			Step step;
			step.bypass = true;
			step.status = "re-base shader warm-up unavailable for " + label + "; compiling on demand";
			if (!bypassLogged)
			{
				bypassLogged = true;
				step.log = "Re-base shader warm-up for " + label + " failed or timed out " + std::to_string(failures) +
					" times; the re-base now compiles on demand (a stall is possible)";
			}
			return step;   // allow = true
		}
		void Remember(const std::string& key)
		{
			if (IsKnownReady(key)) return;
			if (ready.size() >= kMaxReadyKeys) ready.erase(ready.begin());
			ready.push_back(key);
		}
		std::vector<std::string> ready;
		std::string activeKey;
		std::string skipReason;
		double since = 0.0;
		double retryAt = 0.0;
		double retrySeconds = kRetryFirstSeconds;
		double epochStart = 0.0;
		double epochLast = 0.0;
		bool announced = false;
		bool timedOut = false;
		bool bypassLogged = false;
		bool resumed = false;
		bool epochActive = false;
		bool epochBypass = false;
		bool epochLogged = false;
		int failures = 0;
	};

	// The re-base factors the renderer has in force, and the CONTRACT (an opaque signature of the source /
	// target frames, the kernel and the device generation) they were made for. Factors are only meaningful
	// for that contract: a hold keeps them while the contract is unchanged and otherwise drops to native
	// (1.0), so a source change never renders new frames at the previous source's factors.
	class FactorsInForce
	{
	public:
		struct Factors { double x = 1.0; double y = 1.0; };

		// The candidate is exactly what is in force, backed by a compiled warm-up, on the same contract:
		// the re-base is running and there is nothing to ask the gate.
		bool Covers(double x, double y, unsigned long long contract) const
		{
			return valid && factorX == x && factorY == y && signature == contract;
		}
		// A frame was allowed. `warm`: a compiled warm-up backs it (false for a visible bypass, which is
		// asked again every frame).
		void Allowed(double x, double y, unsigned long long contract, bool warm)
		{
			valid = warm;
			factorX = x;
			factorY = y;
			signature = contract;
		}
		// The re-base is not running (not wanted, paused, temporal, slot off).
		void Clear() { valid = false; }
		// A frame is held: the factors to render with. `previous*`: what the renderer had last frame.
		Factors Hold(double previousX, double previousY, unsigned long long contract)
		{
			Factors keep;   // native
			if ((previousX < 1.0 || previousY < 1.0) && valid && signature == contract)
			{
				keep.x = previousX;
				keep.y = previousY;
			}
			else
				valid = false;
			return keep;
		}

	private:
		bool valid = false;
		double factorX = 1.0, factorY = 1.0;
		unsigned long long signature = 0;
	};
}
