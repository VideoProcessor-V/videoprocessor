// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

// Re-base shader warm-up gate (scaling.rebase; fix FX-RB). A re-base to a new plane size needs shader
// variants libplacebo has never compiled (GLSL -> SPIR-V -> HLSL -> DXBC, seconds on a cold cache). On
// the render thread that is a multi-second stall and dozens of dropped frames. This gate holds the
// frame on the CURRENT path (native planes) until a worker has compiled the variants on a private
// device into the shared pl_cache, then lets the re-base proceed. Pure, header-only, C++14: no
// libplacebo, no Windows, no clock of its own (the caller passes seconds), so it is unit-tested on
// the CPU. One instance, used from the render thread only; the worker's progress arrives as `Status`.
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
		bool failed = false;   // visible failure: warm-up unavailable, native frames until retry
		std::string log;       // a line to log (only on a state change; empty otherwise)
		std::string status;    // a plain-language status for the fallback/status surfaces (empty = none)
	};

	class Gate
	{
	public:
		// A warm-up that has not finished after this long counts as failed (the worker keeps going; a
		// later completion is still honoured because Ready always wins).
		static constexpr double kTimeoutSeconds = 45.0;
		static constexpr double kRetryFirstSeconds = 5.0;
		static constexpr double kRetryMaxSeconds = 60.0;
		static constexpr size_t kMaxReadyKeys = 16;
		// After this many failed warm-ups of one key the re-base is let through (compiling on demand,
		// the old behaviour), visibly: a worker that can never succeed must not disable the re-base.
		static constexpr int kMaxFailures = 3;

		// `wanted`: a re-base to `key` is wanted this frame (factor below 1 on an axis, not paused, not
		// temporal). `label` names the target plane size for the log ("1920x1080").
		Step Evaluate(bool wanted, const std::string& key, const std::string& label, Status status, double nowSeconds)
		{
			Step step;
			if (!wanted || key.empty())
			{
				// No re-base wanted: the pending announcement ends; remembered Ready keys and the
				// candidate's failure history stay (the same candidate wanted again resumes, not restarts).
				announced = false;
				return step;
			}
			if (key != activeKey)
			{
				activeKey = key;
				since = nowSeconds;
				retryAt = 0.0;
				retrySeconds = kRetryFirstSeconds;
				announced = false;
				timedOut = false;
				failures = 0;
				bypassLogged = false;
			}
			if (failures >= kMaxFailures)
			{
				step.status = "re-base shader warm-up unavailable for " + label + "; compiling on demand";
				if (!bypassLogged)
				{
					bypassLogged = true;
					step.log = "Re-base shader warm-up for " + label + " failed " + std::to_string(failures) +
						" times; the re-base now compiles on demand (a stall is possible)";
				}
				return step;   // allow = true
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
				return step;   // allow = true
			}
			step.allow = false;
			if (status == Status::Failed)
			{
				step.failed = true;
				step.status = "re-base waiting: shader warm-up for " + label + " failed; native frames, retrying";
				if (nowSeconds >= retryAt)
				{
					if (++failures >= kMaxFailures) return Evaluate(wanted, key, label, status, nowSeconds);
					step.submit = true;
					retryAt = nowSeconds + retrySeconds;
					char text[200];
					std::snprintf(text, sizeof(text), "Re-base shader warm-up for %s failed; staying on the current path, retry in %.0f s",
						label.c_str(), retrySeconds);
					step.log = text;
					const double cap = kRetryMaxSeconds;
					retrySeconds = (std::min)(cap, retrySeconds * 2.0);
					since = nowSeconds;   // the retry's own wait starts now
				}
				return step;
			}
			if (status == Status::Missing)
			{
				step.submit = true;
				step.pending = true;
				since = announced ? since : nowSeconds;
			}
			else   // Pending
			{
				step.pending = true;
				if (nowSeconds - since > kTimeoutSeconds)
				{
					step.failed = true;
					step.pending = false;
					step.status = "re-base waiting: shader warm-up for " + label + " is taking too long; native frames";
					if (!timedOut)
					{
						timedOut = true;
						step.log = "Re-base shader warm-up for " + label + " is taking too long; staying on the current path until it finishes";
					}
					return step;
				}
			}
			if (!announced)
			{
				announced = true;
				step.log = "Re-base pending shader warm-up for " + label + " (current path continues)";
			}
			step.status = "re-base pending shader warm-up for " + label + " (current path continues)";
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
		}

		bool IsKnownReady(const std::string& key) const
		{
			return std::find(ready.begin(), ready.end(), key) != ready.end();
		}

	private:
		void Remember(const std::string& key)
		{
			if (IsKnownReady(key)) return;
			if (ready.size() >= kMaxReadyKeys) ready.erase(ready.begin());
			ready.push_back(key);
		}
		std::vector<std::string> ready;
		std::string activeKey;
		double since = 0.0;
		double retryAt = 0.0;
		double retrySeconds = kRetryFirstSeconds;
		bool announced = false;
		bool timedOut = false;
		bool bypassLogged = false;
		int failures = 0;
	};
}
