/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
// Plug-in seam of the source-identity chain evaluation (SourceIdChainEvalTests.cpp). Header only, no dependencies beyond the STL, so
// SID-C2's own test translation unit can register the chain estimator WITHOUT editing or including the harness:
//
//   static Tests::SidChain::Registrar reg([](Tests::SidChain::EvalSeam& s) {
//       s.reelBegin = [](const std::string& reel) { /* reset the estimator */ };
//       s.frame     = [](const uint16_t* luma, int w, int h, int stride, uint64_t seq, double t) { /* offer the plane */ };
//       s.chainRow  = [](double t, Tests::SidChain::ChainRow& row) { /* fill; return true to write a "chain" lane row */ return false; };
//       s.chainEvents = [](double t, std::vector<Tests::SidChain::ChainEvent>& out) { /* append latch / revert / hold / unstable ... */ };
//   });
//
// The harness applies every registration once at the start of the run (ApplyRegistrations) and calls the callbacks on its own thread.
//
// CONVENTIONS (the scorer relies on them):
//  * luma = the delivered raster's 10-bit luma CODES (0..1023; 8-bit segments are scaled x4 like the SourceFrameTap plane), row stride in
//    samples, valid only during the call.
//  * n values (measured / applied) are RASTER-referenced: the original size per axis that a re-base should find, i.e. raster x (coded size /
//    active-picture size) exactly like the reel truth `truth_n`. They are NOT window or active-area sizes: a 1080p picture shown in a 2560x1440
//    window on a 3840x2160 desktop has truth n = 2880x1620; a letterboxed rung's n is its coded raster (1920x1080), its active picture is reported
//    in the active_* fields (raster coordinates of the active area).
//  * state values: UNKNOWN PROVISIONAL CANDIDATE LATCHED HOLD UNSTABLE ABSTAIN. `state` of the row is the overall state the scorer uses
//    (LATCHED only when both axes are latched); the per-axis states are in x / y.
//  * Event types (chainEvents): latch, revert, hold, unstable (state transitions of an axis), geometry (the measured geometry changed: ABR /
//    rung change; compared with the truth boundary at +-2 s), plus any text the estimator wants kept (type "note").
#include <cstdint>
#include <functional>
#include <utility>
#include <string>
#include <vector>

namespace Tests
{
	namespace SidChain
	{
		// One axis of a chain-estimator evaluation. Unknown numbers are negative / 0 and are written empty.
		struct AxisRow
		{
			std::string state = "UNKNOWN";   // UNKNOWN PROVISIONAL CANDIDATE LATCHED HOLD UNSTABLE ABSTAIN
			double measuredN = 0;            // provisional / measured original size (0 = none)
			double appliedN = 0;             // the latched (applied) original size (0 = none)
			std::string kernel;              // kernel family of the latched hypothesis ("" = unknown)
			double sharpenAmount = -1;       // fitted unsharp amount (-1 = not reported, 0 = none seen)
			double llr = 0;                  // log-likelihood ratio of the leading hypothesis (nats)
			std::string power;               // "seen" / "unseen" / "" (power-by-injection result)
			double spatialAgreement = -1;    // 0..1 share of lines agreeing with the majority (-1 = not reported)
			unsigned reasonBits = 0;         // doc 78 section 6 reason bits
			std::string reason;              // short text of the UNSURE reason ("" = none)
			double doubleN1 = 0;             // first-stage size of a double upscale (0 = none)
		};

		struct ChainRow
		{
			std::string state = "UNKNOWN";   // overall state (see conventions)
			std::string reason;              // overall UNSURE reason text
			AxisRow x, y;
			double activeX = -1, activeY = -1, activeW = -1, activeH = -1;   // raster coordinates of the active picture (-1 = not reported)
			double ms = -1;                  // wall ms of this evaluation (-1 = not measured)
		};

		struct ChainEvent
		{
			std::string type;                // latch revert hold unstable geometry note
			std::string axis;                // "x", "y", "both" or ""
			double fromN = 0, toN = 0;       // n before / after (0 = none)
			double eventTime = -1;           // source time the event refers to (-1 = same as the call time)
			std::string detail;
		};

		struct EvalSeam
		{
			// every decoded frame (10-bit codes), before the 0.5 s analysis cadence
			std::function<void(const uint16_t* luma, int width, int height, int strideSamples, uint64_t seq, double sourceSeconds)> frame;
			std::function<void(const std::string& reelId)> reelBegin;
			std::function<void(const std::string& reelId)> reelEnd;
			// at the 0.5 s analysis cadence: fill the row and return true to write a lane "chain" row into evaluations.csv
			std::function<bool(double sourceSeconds, ChainRow& row)> chainRow;
			// drained right after chainRow: events that happened since the previous call (written to events.csv, lane "chain")
			std::function<void(double sourceSeconds, std::vector<ChainEvent>& events)> chainEvents;
		};

		inline EvalSeam& Seam()
		{
			static EvalSeam s;
			return s;
		}

		inline std::vector<std::function<void(EvalSeam&)>>& Registrations()
		{
			static std::vector<std::function<void(EvalSeam&)>> r;
			return r;
		}

		// Static registrar: construct one at namespace scope in the plug-in's own .cpp.
		struct Registrar
		{
			explicit Registrar(std::function<void(EvalSeam&)> fn) { Registrations().push_back(std::move(fn)); }
		};

		// Harness side: applies every registration to the seam exactly once.
		inline void ApplyRegistrations()
		{
			static bool applied = false;
			if (applied)
				return;
			applied = true;
			for (auto& r : Registrations())
				r(Seam());
		}
	}
}
