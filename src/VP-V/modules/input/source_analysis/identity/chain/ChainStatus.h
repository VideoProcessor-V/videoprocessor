// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Chain estimator status texts (SID-C2b, doc 78 section 7): the Ctrl+I / Config line and the Ctrl+Alt+D detail, as pure functions of a
// plain input struct (the estimator fills it; the tests fill it by hand). No clock, no threads, header-only, C++14.
//
// Line, e.g.:
//   "Source identity: 1080 -> 2160 lanczos-like + sharpening, confident"           the upscale and the kernel family
//   "Source identity: native, confident"
//   "Source identity: H 1280 -> 1920 bicubic-like, confident; V unsure (dark)"      axes that disagree or are not both confident
//   "Source identity: unsure (dark, flat)"                                         no power: the reasons in words
//   "Source identity: holding (overlay)"                                           HOLD: > 20% of the lines contradict
//   "Source identity: acquiring, 7.2 of 15.0 nats"                                 UNSURE: the best hypothesis is below c1
//   "Source identity: classical only (<reason>)"                                   the estimator does not run
//   shadow: "<line> [shadow: would-be, classical decides]"
#include "ChainEvidenceLog.h"
#include "ChainGovernor.h"
#include "ChainLatch.h"
#include "KernelBank.h"

#include <VP-V/modules/input/source_analysis/identity/SourceGeometryView.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace SourceIdentity
{
	namespace Chain
	{
		struct ChainStatusInput
		{
			bool running = false;           // the estimator runs (mode shadow or on, not Original)
			bool tapAttached = false;       // a frame tap owner is attached and the sink is still registered with it (without one no frame arrives)
			bool tapLost = false;           // an owner was attached but the registration is gone (host teardown): no frame will come
			bool shadow = false;            // SHADOW: published and logged, the classical decision stands
			std::string off;                // why it does not run ("" while running)
			bool haveRecord = false;
			Geometry::Record record;        // the latest sealed P14g
			bool haveEval = false;
			LogEval last;                   // the latest evaluation (Ctrl+Alt+D)
			GovernorPlan governor;
			uint64_t evaluations = 0, delivered = 0, dropped = 0, repeatsSkipped = 0, noClock = 0, unusable = 0, stale = 0;
			uint64_t windowClears = 0;      // evidence-window clears (menu end, first cut after a menu / HOLD episode)
			std::string unusableReason;     // the latest "no lines" reason ("" = none)
			double wallMsSinceLastEvaluation = -1.0;   // wall time since the newest evaluation (or the clean start); -1 = not known
			EvidenceStatus evidence;
			std::string evidenceText;
			// SID r2 (M4): the safety rules applied at the clean start, the latched size (0 = none) and the latch counters. Nothing of it is
			// shown while the rules are off (the lines stay byte identical).
			bool safetyRules = false;
			int latchNx = 0, latchNy = 0;
			LatchCounters latch;
		};

		namespace StatusDetail
		{
			inline const char* FamilyWord(int kernelIndex)
			{
				if (kernelIndex < 0)
					return "kernel unsure";
				switch (Kernel(kernelIndex).family)
				{
				case Family::Bilinear: return "bilinear";
				case Family::Bicubic: return "bicubic-like";
				case Family::Lanczos: return "lanczos-like";
				case Family::Spline: return "spline-like";
				}
				return "kernel unsure";
			}

			inline bool Decided(const Geometry::AxisGeom& a)
			{
				return a.state == Geometry::LatchState::Latched && a.n > 0 && a.N > 0;   // Latched = the reserved numeric of CONFIDENT
			}

			// "1080 -> 2160 lanczos-like + sharpening" / "native" for one decided axis.
			inline std::string AxisWords(const Geometry::AxisGeom& a)
			{
				if (a.n == a.N)
					return "native";
				std::string s = std::to_string(a.n) + " -> " + std::to_string(a.N) + " " + FamilyWord(a.kernelIndex);
				if (a.sharpen.present)
					s += " + sharpening";
				if (a.dbl.present)
					s += " (double scaled)";
				return s;
			}

			inline std::string Conf(double c)
			{
				char b[16];
				std::snprintf(b, sizeof(b), "%.2f", c);
				return b;
			}

			inline std::string Num(double v, const char* fmt = "%.1f")
			{
				char b[48];
				std::snprintf(b, sizeof(b), fmt, v);
				return b;
			}

			inline const char* AxisName(int a) { return a == 0 ? "H" : "V"; }

			// SID r2 (M4): the latch reasons of the published (applied) geometry. Only the r2 bits: they are zero while the rules are off.
			constexpr uint32_t kLatchReasonBits = kReasonNonConventional | kReasonLatchBar | kReasonLatchRelease | kReasonMenuHold | kReasonAbrStep;

			// SID r2 (M4): "; upscale latch 1920x1080" / "; no upscale latch" while the rules are on, "" while off.
			inline std::string LatchClause(const ChainStatusInput& in)
			{
				if (!in.safetyRules)
					return std::string();
				if (in.latchNx > 0 && in.latchNy > 0)
					return "; upscale latch " + std::to_string(in.latchNx) + "x" + std::to_string(in.latchNy);
				return "; no upscale latch";
			}

			inline std::string KernelText(int kernelIndex)
			{
				return kernelIndex >= 0 ? std::string(Kernel(kernelIndex).name) : std::string("unsure");
			}
		}

		// The Ctrl+I / Config status line.
		inline std::string ChainStatusLine(const ChainStatusInput& in)
		{
			using namespace StatusDetail;
			const std::string prefix = "Source identity: ";
			if (!in.running)
				return prefix + "classical only (" + (in.off.empty() ? std::string("chain estimator off") : in.off) + ")";
			if (in.tapLost)
				return prefix + "classical only (frame tap lost: no new frames reach the estimator)";
			if (!in.tapAttached && !in.haveRecord)
				return prefix + "classical only (no frame tap)";
			const std::string tail = in.shadow ? " [shadow: would-be, classical decides]" : "";
			const std::string noLines = in.unusableReason.empty() ? std::string() : ", " + in.unusableReason;
			// A worker that stopped, or a source that gives no frames: the line must not keep showing the last result as healthy.
			const double stallMs = 2.5 * in.governor.intervalSec * 1000.0 + 1000.0;
			if (in.wallMsSinceLastEvaluation > stallMs)
				return prefix + "stalled (no frames for " + std::to_string(static_cast<long long>(in.wallMsSinceLastEvaluation / 1000.0 + 0.5)) +
					" s" + noLines + ")" + tail;
			if (!in.haveRecord)
				return prefix + "starting (no evaluation yet" + noLines + ")" + tail;
			const Geometry::Record& r = in.record;
			const bool dec0 = Decided(r.applied[0]), dec1 = Decided(r.applied[1]);
			std::string body;
			if (dec0 && dec1)
			{
				const double ra = r.applied[0].ratio, rb = r.applied[1].ratio;
				const double m = (std::max)(ra, rb);
				const bool agree = m > 0.0 && std::fabs(ra - rb) <= 0.015 * m;
				if (agree)
					body = AxisWords(r.applied[1]) + ", confident";   // the vertical axis names the rung (1080, 2160)
				else
					body = std::string("H ") + AxisWords(r.applied[0]) + "; V " + AxisWords(r.applied[1]) + ", confident (axes differ)";
			}
			else if (dec0 || dec1)
			{
				const int d = dec0 ? 0 : 1, o = 1 - d;
				const std::string why = Geometry::ReasonWords(r.measured[o].reasonBits | (r.applied[o].reasonBits & kLatchReasonBits));
				body = std::string(AxisName(d)) + " " + AxisWords(r.applied[d]) + ", confident; " +
					AxisName(o) + (r.measured[o].state == Geometry::LatchState::Hold ? " holding (overlay)" : " unsure") +
					(why.empty() || r.measured[o].state == Geometry::LatchState::Hold ? std::string() : " (" + why + ")");
			}
			else
			{
				bool hold = false;
				uint32_t bits = 0;
				bool anyAcquiring = false;
				double llr = 0.0;
				for (int a = 0; a < 2; ++a)
				{
					if (r.measured[a].state == Geometry::LatchState::Hold)
						hold = true;
					bits |= r.measured[a].reasonBits | (r.applied[a].reasonBits & kLatchReasonBits);
					if (r.measured[a].state == Geometry::LatchState::Provisional)
					{
						anyAcquiring = true;
						llr = (std::max)(llr, static_cast<double>(r.measured[a].llr));
					}
				}
				if (hold)
					body = "holding (overlay)";
				else
				{
					const std::string why = Geometry::ReasonWords(bits);
					if (!why.empty() && !anyAcquiring)
						body = "unsure (" + why + ")";
					else if (anyAcquiring)
						body = "acquiring, " + Num(llr) + " of " + Num(in.last.c1) + " nats" + (why.empty() ? std::string() : " (" + why + ")");
					else
						body = "unsure (no evidence yet)";
				}
			}
			return prefix + body + LatchClause(in) + tail;
		}

		// The Ctrl+Alt+D detail: per-axis state, LLR, power, spatial agreement, candidates, the governor and the evidence log.
		inline std::string ChainDetailText(const ChainStatusInput& in)
		{
			using namespace StatusDetail;
			std::string t = "Source identity (chain estimator)\n";
			t += std::string("  ") + ChainStatusLine(in) + "\n";
			if (!in.running)
				return t;
			if (in.haveRecord)
			{
				const Geometry::Record& r = in.record;
				t += "  record: eval seq " + std::to_string(r.evalSeq) + ", geometryGen " + std::to_string(r.geometryGen) + ", appliedGen " +
					std::to_string(r.appliedGen) + ", discontinuities " + std::to_string(r.discontinuities) + ", maxAge " +
					Num(static_cast<double>(r.maxAgeUs) / 1e6) + " s\n";
			}
			if (in.haveEval)
			{
				for (int a = 0; a < 2; ++a)
				{
					const LogAxis& L = in.last.axis[a];
					t += std::string("  ") + AxisName(a) + ": " + Geometry::StateText(L.state) + ", measured " + std::to_string(L.measuredN) + "/" +
						std::to_string(L.N) + " " + KernelText(L.measuredKernel) + " llr " + Num(L.measuredLlr) + " (c1 " + Num(in.last.c1) +
						"), confident " + std::to_string(L.appliedN) + " " + KernelText(L.appliedKernel) + " llr " +
						Num(L.appliedLlr) + "\n";
					t += "     power: probe " + std::string(L.probeRan ? (L.probeSeen ? "seen" : "unseen") : "not run") + ", lines " +
						std::to_string(L.gatedLines) + "/" + std::to_string(L.lines) + " informative, " + std::to_string(L.maskedLines) +
						" masked, reasons: " + (Geometry::ReasonWords(L.reasonBits).empty() ? std::string("none") : Geometry::ReasonWords(L.reasonBits)) +
						", reading: " + ReadingReasonText(static_cast<ReadingReason>(L.readingReason < 0 || L.readingReason >= static_cast<int>(ReadingReason::Count) ? 0 : L.readingReason)) + "\n";
					for (int i = 0; i < L.candCount && i < 3; ++i)
						t += "     candidate " + std::to_string(L.top[i].n) + ": stat " + Num(L.top[i].stat, "%.3f") + ", increment " +
							Num(L.top[i].increment, "%.2f") + ", kernel " + KernelText(L.top[i].kernel) + (L.top[i].informative ? "" : " (not informative)") + "\n";
				}
				t += std::string("  output step ") + (in.last.stepCodes == 4 ? "8-bit" : "10-bit") + ", active " + std::to_string(in.last.active.w) + "x" +
					std::to_string(in.last.active.h) + "\n";
			}
			const GovernorPlan& g = in.governor;
			t += "  governor: interval " + Num(g.intervalSec, "%.2f") + " s (nominal " + Num(g.nominalSec, "%.1f") + " s, share " +
				Num(g.share * 100.0, "%.0f") + "%, " + (g.latched ? "latched" : "acquiring") + "), cost " + Num(g.costMs, "%.1f") + " ms" +
				(g.cold ? " (cold)" : "") + (g.stretched ? ", cadence STRETCHED by the CPU budget" : "") +
				(g.overBudget ? ", OVER the per-evaluation budget" : "") + "\n";
			t += "  counts: evaluations " + std::to_string(in.evaluations) + ", frames taken " + std::to_string(in.delivered) + ", dropped (newest wins) " +
				std::to_string(in.dropped) + ", repeats skipped " + std::to_string(in.repeatsSkipped) + ", no clock " + std::to_string(in.noClock) +
				", no lines " + std::to_string(in.unusable) + (in.unusableReason.empty() ? std::string() : " (" + in.unusableReason + ")") +
				", stale " + std::to_string(in.stale) + ", window clears " + std::to_string(in.windowClears) + "\n";
			if (in.safetyRules)
			{
				const LatchCounters& c = in.latch;
				t += "  safety rules on: latch " + (in.latchNx > 0 && in.latchNy > 0 ? std::to_string(in.latchNx) + "x" + std::to_string(in.latchNy) :
					std::string("none")) + ", entries " + std::to_string(c.entries) + ", moves " + std::to_string(c.moves) + ", confirms " +
					std::to_string(c.confirms) + ", releases " + std::to_string(c.releases) + " (clock " + std::to_string(c.releasesBound) +
					", hard bound " + std::to_string(c.releasesHardBound) + ", native " + std::to_string(c.releasesNative) + ", menu " +
					std::to_string(c.releasesMenu) + ", ABR " + std::to_string(c.releasesAbr) + ", reset " + std::to_string(c.releasesReset) +
					"), held back " + std::to_string(c.heldBack) + ", not conventional " + std::to_string(c.nonConventional) + "\n";
			}
			if (!in.evidenceText.empty())
				t += "  " + in.evidenceText + "\n";
			return t;
		}
	}
}
