// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// ChainReading (SID train r2, doc 78 section 5 owner decision 2026-11-07): the per-axis READING of the chain estimator. It turns the
// evidence of ChainEvidence (an accumulator: summary + the increments of the newest evaluation) into what P14g publishes per axis:
//   - MEASURED  the diagnostic best hypothesis (n, kernel, phase, LLR): what the evidence currently favours, whatever its strength;
//   - APPLIED   the CONFIDENT reading: written only while the best hypothesis is above c1 (AxisSummary::aboveC1; native: the native
//               LLR above c1) AND this evaluation was informative. Otherwise the axis is UNSURE: nothing is applied, with a reason word.
// There is NO state machine here: no dwell, no sticky value, no contrary-evidence timer, no minimum hold, no regime switch, no unstable
// state (all parked, doc 78 section 5-parked). The reading is a pure function of the accumulator and of this evaluation; WHEN a
// re-base happens stays the original latch's business (engage moments, dwell, crossfade), and what the original latch is told (WHAT)
// is the consensus's business (SourceIdConsensus.h, SourceIdVote.h ChainVote). c1 is a DETECTION-confidence threshold from the null
// tables (ChainEvidence.h ComputeThresholds) and adds no timing.
//
// States written to P14g (Geometry::LatchState; Candidate and Unstable stay reserved numerics, never written, no unit bump):
//   Unknown      nothing informative yet (start, discontinuity, active-area change, an evidence reset)
//   Provisional  a best hypothesis exists but below c1 (UNSURE; the reason says "below c1" or "axes disagree")
//   Latched      CONFIDENT (the name is the reserved numeric; every text says "confident")
//   Hold         > 20% of the informative lines contradict the majority (overlay / PiP / menu): UNSURE, "mixed (HOLD)"
//   Abstain      the evaluation had no power (dark, flat, soft, no power, low detail): UNSURE with the reason words
//
// Also here (kept from the former latch file): MakeFieldEvidence + the kernel equivalence-class pick, ConfFromLlr, KernelConfidence.
//
// Header-only, standard library only, C++14, no clock (time = source time in EvalContext), deterministic.
#include <VP-V/modules/input/source_analysis/identity/SourceGeometryProduct.h>

#include <VP-V/modules/input/source_analysis/identity/SourceIdVote.h>

#include "ChainEvidence.h"
#include "KernelBank.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace SourceIdentity
{
	namespace Chain
	{
		using Geometry::LatchState;

		// Hidden tuning of the reading (doc 78 section 7): only the kernel decision and the change tolerance are left.
		struct ReadingSettings
		{
			double kernelConfMin = 0.5;            // a kernel is VERIFIED (re-base may use it) at or above this confidence
			double kernelMarginScale = 0.05;       // family margin (ln units) -> confidence 1 - exp(-margin / scale); only without per-kernel T
			double kernelClassMargin = 0.05;       // kernels whose T is within this (ln units) of the best form ONE equivalence class; placeholder until CAL
			double kernelDipMax = -0.15;           // the best kernel must dip at least this much (= ProbeSettings::seenThreshold) to count as verified
			double phaseTolerance = 0.25;          // measured phase moves smaller than this are no change (= VerifierSettings::phaseStep)
		};

		// The one level the reading needs: c1 from the null tables (ChainEvidence.h).
		struct ReadingLevels
		{
			double c1 = 15.0;
			static ReadingLevels From(const Thresholds& t)
			{
				ReadingLevels l;
				l.c1 = t.c1;
				return l;
			}
		};

		// What the call site knows about THIS evaluation.
		struct EvalContext
		{
			uint64_t seq = 0;
			int64_t timeUs = 0;               // source time, non-decreasing
			bool repeat = false;              // a cadence repeat of the previous frame: no evidence, nothing changes
			bool discontinuity = false;       // Indicator FULL/era, format change, resync, seek: everything back to UNKNOWN
			bool activeAreaChanged = false;   // aspect / zoom toggle: the axis geometry changed, back to UNKNOWN
		};

		// The evidence of one evaluation of one axis, in the reading's own plain terms (built from ChainEvidence by MakeFieldEvidence;
		// tests build it directly).
		struct HypothesisIncrement
		{
			int n = 0;                        // the latent size this increment belongs to (exact, refined)
			double increment = 0.0;           // LLR increment of THIS evaluation, nats
			int kernelIndex = -1;             // the kernel to use at n (KernelBank index WITH a classical equivalent), -1 = none / unsure
			double kernelConf = 0.0;          // 0..1: 1 = a kernel verified (it dips) with an invertible member in its class, else 0
			uint32_t kernelClass = 0;         // KernelBank indices (bit per kernel) indistinguishable from the best; 0 = just kernelIndex
			double phase = 0.0;
		};

		struct FieldEvidence
		{
			bool evaluated = false;           // an evaluation of this axis ran (false: nothing to do)
			bool informative = false;         // EvidenceUpdate::informative
			bool hold = false;                // EvidenceUpdate::hold: > 20% of the lines contradict (overlay / PiP / menu)
			uint32_t reason = 0;              // Chain::kReason* bits of the evaluation
			int axisLen = 0;                  // N of this axis (active area)
			// ChainEvidence's accumulated state after this evaluation (recent window, shot-capped, two-axis prior included).
			bool hasBest = false;
			int bestN = 0;
			double bestLlr = 0.0;             // the best hypothesis' LLR WITH the two-axis prior
			double prior = 0.0;               // the two-axis agreement prior inside bestLlr (negative: the axes disagree)
			double nativeLlr = 0.0;
			int nativeEvals = 0;
			// This evaluation's increments (kernel and phase of the best hypothesis).
			int incCount = 0;
			HypothesisIncrement inc[kMaxCandidates];
			bool nativeCounted = false;       // the native hypothesis earned evidence (the power probe SAW its injection)
			double nativeIncrement = 0.0;
		};

		enum class ReadingReason : uint8_t
		{
			NoEvidence = 0,    // UNKNOWN: nothing informative yet
			BelowC1,           // PROVISIONAL: a best hypothesis below c1
			Confident,         // CONFIDENT: the best hypothesis is above c1
			HeldMixed,         // > 20% contradicting lines: HOLD (UNSURE)
			NoPower,           // this evaluation had no power (dark / flat / soft / no power / low detail): UNSURE
			AxesDisagree,      // the axes disagree and the penalty keeps the best hypothesis below c1: UNSURE
			Discontinuity,     // reset: discontinuity
			ActiveArea,        // reset: active area change
			Repeat,            // a cadence repeat: nothing changed
			Count
		};

		inline const char* ReadingReasonText(ReadingReason r)
		{
			switch (r)
			{
			case ReadingReason::NoEvidence: return "no evidence yet";
			case ReadingReason::BelowC1: return "below c1";
			case ReadingReason::Confident: return "confident";
			case ReadingReason::HeldMixed: return "mixed (HOLD)";
			case ReadingReason::NoPower: return "no power (dark, flat, soft or low detail)";
			case ReadingReason::AxesDisagree: return "axes disagree";
			case ReadingReason::Discontinuity: return "reset: discontinuity";
			case ReadingReason::ActiveArea: return "reset: active area changed";
			case ReadingReason::Repeat: return "repeat";
			default: return "?";
			}
		}

		struct ReadingOutput
		{
			LatchState state = LatchState::Unknown;   // the field state: Unknown / Provisional / Latched (= confident) / Hold / Abstain
			Geometry::AxisGeom measured;              // the diagnostic best hypothesis
			Geometry::AxisGeom applied;               // the CONFIDENT reading; state Unknown while the axis is UNSURE
			bool stateChanged = false;
			bool measuredChanged = false;             // n / native / kernel / phase of the measured geometry moved (bumps geometryGen)
			bool appliedChanged = false;              // n / native / kernel / state of the applied geometry moved (bumps appliedGen)
			bool kernelChanged = false;               // ... of which the kernel part changed
			bool resetEvidence = false;               // the worker must ChainEvidence::ResetAxis(axis) (discontinuity, active area)
			ReadingReason reason = ReadingReason::NoEvidence;
			uint32_t reasonBits = 0;                  // Chain::kReason* of the evaluation (for the status / log)
			int64_t timeUs = 0;
		};

		struct ReadingStats
		{
			uint64_t evaluations = 0;
			uint64_t confident = 0;         // evaluations that read confident
			uint64_t unsure = 0;            // evaluations that read UNSURE (informative or not)
			uint64_t holds = 0;             // HOLD (mixed) evaluations
			uint64_t abstains = 0;          // non-informative evaluations
			uint64_t discontinuities = 0;
		};

		inline double ConfFromLlr(double llr)
		{
			if (llr <= 0.0)
				return 0.0;
			if (llr > 40.0)
				return 1.0;
			return 1.0 / (1.0 + std::exp(-llr));
		}

		// The voter's confidence basis (SourceIdVote.h ChainVote reads it from the record): LLR relative to c1, capped at 1.
		inline double ConfidenceOfLlr(double llr, double c1)
		{
			if (!(llr > 0.0) || !(c1 > 0.0))
				return 0.0;
			return llr >= c1 ? 1.0 : llr / c1;
		}

		inline double KernelConfidence(double familyMargin, double scale)
		{
			if (!(familyMargin > 0.0) || !(scale > 0.0))
				return 0.0;
			return 1.0 - std::exp(-familyMargin / scale);
		}

		// Builds the reading's evidence from ChainEvidence's output for ONE axis after Submit. `verify` (optional) gives the sub-sample
		// phase of each candidate; without it the phase is 0.
		inline FieldEvidence MakeFieldEvidence(const AxisSummary& summary, const EvidenceUpdate& update, int axisLen,
			const ReadingSettings& rs, const VerifyResult* verify = nullptr)
		{
			FieldEvidence e;
			e.evaluated = true;
			e.informative = update.informative;
			e.hold = update.hold;
			e.reason = update.reason;
			e.axisLen = axisLen;
			e.hasBest = summary.hasBest;
			e.bestN = summary.n;
			e.bestLlr = summary.llrWithPrior;
			e.prior = summary.prior;
			e.nativeLlr = summary.nativeLlr;
			e.nativeEvals = summary.nativeEvals;
			for (int i = 0; i < update.candidateCount && e.incCount < kMaxCandidates; ++i)
			{
				const CandidateEvidence& c = update.cand[i];
				if (!c.informative)
					continue;
				HypothesisIncrement& h = e.inc[e.incCount++];
				h.n = c.n;
				h.increment = c.increment;
				// Without per-kernel T: the verifier's best kernel, kept only when it has a classical equivalent.
				h.kernelIndex = (c.kernel >= 0 && ChainKernelToClassical(c.kernel) >= 0) ? c.kernel : -1;
				h.kernelConf = h.kernelIndex >= 0 ? KernelConfidence(c.familyMargin, rs.kernelMarginScale) : 0.0;
				h.kernelClass = c.kernel >= 0 ? (1u << c.kernel) : 0u;
				const CandidateResult* cr = nullptr;
				if (verify != nullptr)
					for (size_t k = 0; k < verify->candidates.size(); ++k)
						if (verify->candidates[k].exactDone && verify->candidates[k].n == c.n)
						{
							cr = &verify->candidates[k];
							break;
						}
				if (cr != nullptr)
				{
					h.phase = cr->phase;
					if (cr->kernelMask != 0u)
					{
						// The kernel equivalence class: kernels whose T is within the class margin of the best one cannot be told apart
						// (at exactly 2x bilinear and bicubic are nearly the same operator). The kernel to use is the best-scoring class
						// member that the re-base can invert (it has a classical equivalent); a class without one, or a best kernel that
						// does not dip at all (no LTI kernel verifies), is "kernel unsure".
						uint32_t cls = 0u;
						int pick = -1;
						double pickT = 0.0;
						for (int k = 0; k < kKernelCount; ++k)
						{
							if ((cr->kernelMask & (1u << k)) == 0u || cr->kernelT[k] - cr->stat > rs.kernelClassMargin)
								continue;
							cls |= 1u << k;
							if (ChainKernelToClassical(k) >= 0 && (pick < 0 || cr->kernelT[k] < pickT))
							{
								pick = k;
								pickT = cr->kernelT[k];
							}
						}
						h.kernelClass = cls;
						const bool dips = cr->stat <= rs.kernelDipMax;
						h.kernelIndex = (dips && pick >= 0) ? pick : -1;
						h.kernelConf = h.kernelIndex >= 0 ? 1.0 : 0.0;
					}
				}
			}
			e.nativeCounted = update.nativeCounted;
			e.nativeIncrement = update.nativeIncrement;
			return e;
		}

		// One reader per axis (the estimator holds two). Stateless apart from the last output (change flags) and the axis length.
		class AxisReader
		{
		public:
			AxisReader() {}
			AxisReader(const ReadingSettings& s, const ReadingLevels& l) : s_(s), lv_(l) {}

			const ReadingSettings& Settings() const { return s_; }
			const ReadingLevels& Levels() const { return lv_; }
			const ReadingStats& Stats() const { return stats_; }
			const ReadingOutput& Last() const { return last_; }
			// The confident reading of the LAST evaluation (not sticky: UNSURE clears it).
			bool HasConfident() const { return last_.applied.state == LatchState::Latched; }
			bool ConfidentNative() const { return HasConfident() && last_.applied.n == last_.applied.N; }
			int ConfidentN() const { return HasConfident() ? last_.applied.n : 0; }

			// One evaluation of this axis.
			ReadingOutput Step(const FieldEvidence& ev, const EvalContext& ctx)
			{
				++stats_.evaluations;
				if (ctx.discontinuity || ctx.activeAreaChanged)
					return DoReset(ev, ctx);
				if (!haveTime_ || ctx.timeUs > nowUs_)   // source time never steps backwards in the reading
				{
					nowUs_ = ctx.timeUs;
					haveTime_ = true;
				}
				if (ctx.repeat || !ev.evaluated)
				{
					ReadingOutput o = last_;
					o.stateChanged = o.measuredChanged = o.appliedChanged = o.kernelChanged = false;
					o.resetEvidence = false;
					o.reason = ReadingReason::Repeat;
					o.timeUs = nowUs_;
					last_ = o;
					return o;
				}
				if (ev.axisLen > 0)
					axisLen_ = ev.axisLen;

				LatchState state = LatchState::Unknown;
				ReadingReason reason = ReadingReason::NoEvidence;
				Hyp m;           // the diagnostic best
				bool confident = false;
				if (ev.hold)
				{
					++stats_.holds;
					++stats_.unsure;
					state = LatchState::Hold;
					reason = ReadingReason::HeldMixed;
					m = Best(ev);
				}
				else if (!ev.informative)
				{
					++stats_.abstains;
					++stats_.unsure;
					state = LatchState::Abstain;
					reason = ReadingReason::NoPower;
					m = Best(ev);
				}
				else
				{
					m = Best(ev);
					if (!m.valid)
					{
						++stats_.unsure;
						state = LatchState::Unknown;
						reason = ReadingReason::NoEvidence;
					}
					else if (m.llr >= lv_.c1 - 1e-9)
					{
						confident = true;
						++stats_.confident;
						state = LatchState::Latched;
						reason = ReadingReason::Confident;
					}
					else
					{
						++stats_.unsure;
						state = LatchState::Provisional;
						// The two-axis prior penalises a disagreement: say so when the penalty is what keeps it below c1.
						reason = (!m.native && ev.prior < 0.0 && m.llr - ev.prior >= lv_.c1 - 1e-9) ? ReadingReason::AxesDisagree
							: ReadingReason::BelowC1;
					}
				}
				return Finish(ev, state, reason, m, confident);
			}

		private:
			struct Hyp
			{
				bool valid = false;
				bool native = false;
				int n = 0;
				double phase = 0.0;
				int kernelIndex = -1;
				double kernelConf = 0.0;
				double llr = 0.0;
			};

			static const HypothesisIncrement* FindInc(const FieldEvidence& ev, int n)
			{
				const HypothesisIncrement* best = nullptr;
				int bestD = 0;
				for (int i = 0; i < ev.incCount && i < kMaxCandidates; ++i)
				{
					const int d = ev.inc[i].n > n ? ev.inc[i].n - n : n - ev.inc[i].n;
					if (d > 1)
						continue;
					if (best == nullptr || d < bestD)
					{
						best = &ev.inc[i];
						bestD = d;
					}
				}
				return best;
			}

			// The best hypothesis of the accumulator: native when it has earned more than the best size (the power probe saw its injection).
			Hyp Best(const FieldEvidence& ev) const
			{
				Hyp m;
				const bool nat = ev.nativeEvals > 0 && ev.nativeLlr > 0.0 && (!ev.hasBest || ev.nativeLlr > ev.bestLlr);
				const bool up = !nat && ev.hasBest && ev.bestLlr > 0.0;
				if (!nat && !up)
					return m;
				m.valid = true;
				m.native = nat;
				m.llr = nat ? ev.nativeLlr : ev.bestLlr;
				m.n = nat ? axisLen_ : ev.bestN;
				if (!nat)
				{
					const HypothesisIncrement* h = FindInc(ev, ev.bestN);
					if (h != nullptr)
					{
						m.phase = h->phase;
						m.kernelIndex = h->kernelIndex;
						m.kernelConf = h->kernelConf;
					}
				}
				return m;
			}

			void ResetAll()
			{
				last_ = ReadingOutput();
				reported_ = Geometry::AxisGeom();
				axisLen_ = 0;
			}

			ReadingOutput DoReset(const FieldEvidence& ev, const EvalContext& ctx)
			{
				const ReadingOutput was = last_;
				ResetAll();
				nowUs_ = ctx.timeUs;
				haveTime_ = true;
				axisLen_ = ev.axisLen > 0 ? ev.axisLen : 0;
				ReadingOutput o;
				o.state = LatchState::Unknown;
				o.measured = MakeGeom(Hyp(), LatchState::Unknown, ev.reason);
				o.applied = MakeGeom(Hyp(), LatchState::Unknown, ev.reason);
				o.stateChanged = was.state != LatchState::Unknown;
				o.measuredChanged = was.measured.n != 0;
				o.appliedChanged = was.applied.state != LatchState::Unknown;
				o.kernelChanged = was.applied.kernelIndex >= 0;
				o.resetEvidence = true;
				o.reason = ctx.discontinuity ? ReadingReason::Discontinuity : ReadingReason::ActiveArea;
				o.reasonBits = ev.reason;
				o.timeUs = ctx.timeUs;
				++stats_.discontinuities;
				last_ = o;
				return o;
			}

			Geometry::AxisGeom MakeGeom(const Hyp& h, LatchState state, uint32_t reason, bool onlyVerifiedKernel = false) const
			{
				Geometry::AxisGeom g;
				g.state = state;
				g.N = axisLen_;
				g.n = h.valid ? (h.native ? axisLen_ : h.n) : 0;
				g.ratio = (h.valid && axisLen_ > 0) ? static_cast<float>(static_cast<double>(g.n) / static_cast<double>(axisLen_)) : 1.0f;
				g.phase = static_cast<float>(h.phase);
				const bool useKernel = h.valid && !h.native && h.kernelIndex >= 0 && (!onlyVerifiedKernel || h.kernelConf >= s_.kernelConfMin);
				if (useKernel)
				{
					const KernelInfo& k = Kernel(h.kernelIndex);
					g.kernelFamily = static_cast<uint8_t>(k.family);
					g.kernelIndex = static_cast<int8_t>(h.kernelIndex);
					g.kernelParamA = static_cast<float>(k.a);
					g.kernelParamB = static_cast<float>(k.b);
				}
				g.kernelConf = static_cast<float>(h.kernelConf);
				g.conf = h.valid ? static_cast<float>(ConfidenceOfLlr(h.llr, lv_.c1)) : 0.0f;
				g.llr = static_cast<float>(h.llr);
				g.reasonBits = reason;
				return g;
			}

			static bool Moved(const Geometry::AxisGeom& a, const Geometry::AxisGeom& b, double phaseTol)
			{
				const double dp = static_cast<double>(a.phase) - static_cast<double>(b.phase);
				// N never moves without a reset (active-area change), so it is not compared.
				return a.n != b.n || a.kernelIndex != b.kernelIndex || (dp < 0.0 ? -dp : dp) > phaseTol;
			}

			ReadingOutput Finish(const FieldEvidence& ev, LatchState state, ReadingReason reason, const Hyp& m, bool confident)
			{
				ReadingOutput o;
				o.timeUs = nowUs_;
				o.reason = reason;
				o.reasonBits = ev.reason;
				o.state = state;
				o.measured = MakeGeom(m, state, ev.reason);
				// The applied geometry only exists for a CONFIDENT reading; its kernel only when VERIFIED ("n known, kernel unsure" otherwise).
				o.applied = confident ? MakeGeom(m, LatchState::Latched, ev.reason, true) : MakeGeom(Hyp(), LatchState::Unknown, ev.reason);
				o.stateChanged = o.state != last_.state;
				// Against the last REPORTED measured geometry, so a slow phase drift cannot hide below the tolerance for ever.
				o.measuredChanged = Moved(reported_, o.measured, s_.phaseTolerance);
				if (o.measuredChanged)
					reported_ = o.measured;
				o.kernelChanged = last_.applied.kernelIndex != o.applied.kernelIndex;
				o.appliedChanged = Moved(last_.applied, o.applied, 1e300) || last_.applied.state != o.applied.state;
				last_ = o;
				return o;
			}

			ReadingSettings s_;
			ReadingLevels lv_;
			ReadingStats stats_;
			ReadingOutput last_;
			int axisLen_ = 0;
			int64_t nowUs_ = 0;
			bool haveTime_ = false;
			Geometry::AxisGeom reported_;   // the last measured geometry that was reported as changed
		};
	}
}
