// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// ChainLatch (SID-C2a): the per-field latch state machine of doc 78 section 5. ONE ChainLatch per axis (the estimator
// worker holds two). It turns the evidence of ChainEvidence (an accumulator: summary + the increments of the newest
// evaluation) into the field STATE and the two geometries of P14g: MEASURED (the current best, provisional) and APPLIED
// (the LATCHED, sticky values).
//
// Two evidence stores, one per phase of a field's life:
//   - acquiring (nothing latched): ChainEvidence's accumulated, shot-capped totals (best size, native) decide PROVISIONAL ->
//     CANDIDATE -> LATCHED. After a discontinuity the worker resets that accumulator, so the totals are fresh.
//   - latched: contrary evidence is a CUSUM per hypothesis built from the per-evaluation increments (floor 0, ceiling c2, the
//     gain inside one shot capped at the per-shot cap), because ChainEvidence's totals never forget and a long upscaled film
//     would bury a later native menu under hundreds of nats of old evidence. The held hypothesis has its own CUSUM (it starts at
//     the ceiling), so the MEASURED geometry can move to the contrary one within a few evaluations at a boundary.
// Call-site contract for the flags: cut / blank / fade are set on the FIRST Step after the event (a regime boundary needs the
// flag at or before the first evaluation of the new hypothesis).
//
// CONTRACT WITH THE ESTIMATOR WORKER (SID-C2b), all of it required:
//   - One Step per evaluation per axis, in source-time order. EvalContext: `cuts` = the number of cuts since the previous Step (a
//     bool is not enough: each cut is a shot), `blank` / `fade` = such a frame since the previous Step, `eventTimeUs` = the source time
//     of the newest of those events (-1: unknown, the evaluation time is used). The flags belong to the FIRST Step after the event.
//   - Publish the P14g record after EVERY Step, including ABSTAIN, HOLD and repeat evaluations: the record's maxAgeUs is
//     SourceGeometry::MaxAgeUs(the longest PLANNED evaluation interval, i.e. the governor's stretched interval, not the nominal one),
//     so a published record is the proof that the estimator is alive. evalSeq = the newest source sequence behind the evaluation;
//     evalTimeUs = source time in the SAME domain as the frame clock the pipeline uses; streamKey = the Indicator's FULL/era counter.
//   - On a discontinuity (or an active-area change) call Step with `discontinuity` / `activeAreaChanged`, call
//     ChainEvidence::ResetAxis when LatchOutput::resetEvidence is set, bump Record::discontinuities and PUBLISH THE RESET RECORD
//     IMMEDIATELY (before the next pipeline read): until then the ring still holds the old LATCHED record, which the pipeline refuses
//     only by its sequence floor (Pipeline::Discontinuity) and the time ordering below.
//   - Tighten the cadence while LatchOutput::reason is ContraryBuilding or HeldMinHold (a regime switch needs >= 4 near-maximum native
//     evaluations at +2.0 each, i.e. 15-20 s at a 5 s cadence): the planned interval, and with it maxAgeUs, follows.
//   - Replay and tests publish SYNCHRONOUSLY (publish before the next pipeline frame): the live async worker is not reproducible.
//
// Usage (worker, one call per evaluation per axis):
//   FieldEvidence ev = MakeFieldEvidence(evidence.Summary(axis), update, axisLen, latchSettings, &verifyResult);
//   LatchOutput out = latch[axis].Step(ev, ctx);   // out.measured / out.applied fill Geometry::Record
//   if (out.resetEvidence) evidence.ResetAxis(axis);
// Terms (lead decision): LATCHED = the identity decision. CANDIDATE -> LATCHED when LLR >= c1 over >= 4 informative evaluations
// spanning >= 2 shots or >= 5 s. A visible re-base WAITS for the Monitor's engage moments; it is not a precondition of LATCHED.
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

		// All timers are derived from the evidence thresholds and these settings (hidden tuning, doc 78 section 7).
		struct LatchSettings
		{
			int dwellEvaluations = 4;              // informative evaluations the candidate must persist (doc 78 section 4)
			int dwellShots = 2;                    // ... spanning at least this many shots (cuts), or
			int64_t dwellUs = 5000000;             // ... at least this long (5 s)
			int64_t minHoldFloorUs = 10000000;     // T_min = max(2 x dwell, this floor)
			int contraryEvaluations = 3;           // T2: evaluations the contrary evidence must stay >= c2 ...
			int64_t contraryUs = 3000000;          // ... for at least this long (3 s)
			int regimeEvaluations = 3;             // informative evaluations of a hypothesis starting after a cut / blank / fade
			int64_t unstableWindowUs = 60000000;   // two sizes alternating within this = UNSTABLE
			double kernelConfMin = 0.5;            // a kernel is VERIFIED (re-base may use it) at or above this confidence
			double kernelMarginScale = 0.05;       // family margin (ln units) -> confidence 1 - exp(-margin / scale); only without per-kernel T
			double kernelClassMargin = 0.05;       // kernels whose T is within this (ln units) of the best form ONE equivalence class; placeholder until CAL
			double kernelDipMax = -0.15;           // the best kernel must dip at least this much (= ProbeSettings::seenThreshold) to count as verified
			double phaseTolerance = 0.25;          // measured phase moves smaller than this are no change (= VerifierSettings::phaseStep)

			int64_t TMinUs() const { return (std::max)(2 * dwellUs, minHoldFloorUs); }
		};

		// The three levels the latch needs, derived from ChainEvidence's thresholds: c1, c2 and the per-shot cap
		// (shotCapFraction x c1), which is also the bar of a regime switch (one shot cannot show more).
		struct LatchLevels
		{
			double c1 = 15.0, c2 = 18.0, shotCap = 7.5;
			static LatchLevels From(const Thresholds& t, const EvidenceSettings& e)
			{
				LatchLevels l;
				l.c1 = t.c1;
				l.c2 = t.c2;
				l.shotCap = e.shotCapFraction * t.c1;
				return l;
			}
		};

		// What the call site knows about THIS evaluation (all flags from the frame flags of the evaluated frame).
		struct EvalContext
		{
			uint64_t seq = 0;
			int64_t timeUs = 0;               // source time, non-decreasing
			bool cut = false, blank = false, fade = false;
			int cuts = 0;                     // cuts since the previous Step (0 with cut = true counts as 1); each is a shot
			int64_t eventTimeUs = -1;         // source time of the newest cut / blank / fade since the previous Step (-1: unknown)
			bool repeat = false;              // a cadence repeat of the previous frame: no evidence, no change
			bool discontinuity = false;       // Indicator FULL/era, format change, resync, seek: everything back to UNKNOWN
			bool activeAreaChanged = false;   // aspect / zoom toggle: the axis geometry changed, back to UNKNOWN
		};

		// The evidence of one evaluation of one axis, in the latch's own plain terms (built from ChainEvidence by
		// MakeFieldEvidence; tests build it directly).
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
			// ChainEvidence's accumulated state after this evaluation (acquisition, shot-capped, two-axis prior included).
			bool hasBest = false;
			int bestN = 0;
			double bestLlr = 0.0;
			double nativeLlr = 0.0;
			int nativeEvals = 0;
			// This evaluation's increments (used against a latched value: contrary evidence, kernel).
			int incCount = 0;
			HypothesisIncrement inc[kMaxCandidates];
			bool nativeCounted = false;       // the native hypothesis earned evidence (the power probe SAW its injection)
			double nativeIncrement = 0.0;
		};

		enum class LatchReason : uint8_t
		{
			NoEvidence = 0,    // UNKNOWN: nothing informative yet
			Acquiring,         // PROVISIONAL: a best hypothesis below c1
			Candidate,         // CANDIDATE: >= c1, the dwell is not met yet
			Latched,           // the value was latched at this evaluation
			Agrees,            // LATCHED, the evidence agrees (or the contrary is below its bar)
			HeldNoEvidence,    // absence of evidence (dark / flat / soft / no power): the latched value stands
			HeldMixed,         // > 20% contradicting lines: HOLD, no transitions
			ContraryBuilding,  // LATCHED, contrary evidence building toward c2
			HeldMinHold,       // contrary evidence ready but T_min since the last change has not passed
			Reverted,          // LATCHED value changed on sustained contrary evidence
			RegimeNative,      // LATCHED value changed toward native at a regime boundary (T_min bypassed)
			Unstable,          // two sizes alternate: the higher n is applied
			Discontinuity,     // reset: discontinuity
			ActiveArea,        // reset: active area change
			Repeat,            // a cadence repeat: nothing changed
			KernelAdopted,     // the applied kernel was set / changed (n holds)
			Count
		};

		inline const char* LatchReasonText(LatchReason r)
		{
			switch (r)
			{
			case LatchReason::NoEvidence: return "no evidence yet";
			case LatchReason::Acquiring: return "acquiring";
			case LatchReason::Candidate: return "candidate";
			case LatchReason::Latched: return "latched";
			case LatchReason::Agrees: return "agrees";
			case LatchReason::HeldNoEvidence: return "held: no evidence (dark, flat, soft or no power)";
			case LatchReason::HeldMixed: return "held: mixed picture (overlay)";
			case LatchReason::ContraryBuilding: return "held: contrary evidence building";
			case LatchReason::HeldMinHold: return "held: min hold";
			case LatchReason::Reverted: return "changed: contrary evidence";
			case LatchReason::RegimeNative: return "changed: regime boundary toward native";
			case LatchReason::Unstable: return "unstable: alternating sizes, higher size applied";
			case LatchReason::Discontinuity: return "reset: discontinuity";
			case LatchReason::ActiveArea: return "reset: active area changed";
			case LatchReason::Repeat: return "repeat";
			case LatchReason::KernelAdopted: return "kernel adopted";
			default: return "?";
			}
		}

		struct LatchOutput
		{
			LatchState state = LatchState::Unknown;   // the field state (HOLD / ABSTAIN overlay the core state while they last)
			Geometry::AxisGeom measured;              // the current best (provisional)
			Geometry::AxisGeom applied;               // the LATCHED values (sticky); state Unknown until the first latch
			bool stateChanged = false;
			bool measuredChanged = false;             // n / native / kernel / phase of the measured geometry moved (bumps geometryGen)
			bool appliedChanged = false;              // n / native / kernel / state of the applied geometry moved (bumps appliedGen)
			bool kernelChanged = false;               // ... of which the kernel part changed
			bool bypassedTMin = false;                // a regime boundary changed the value inside T_min
			bool resetEvidence = false;               // the worker must ChainEvidence::ResetAxis(axis) (discontinuity, active area)
			LatchReason reason = LatchReason::NoEvidence;
			uint32_t reasonBits = 0;                  // Chain::kReason* of the evaluation (for the status / log)
			int64_t timeUs = 0;
		};

		struct LatchStats
		{
			uint64_t evaluations = 0;
			uint64_t latches = 0;           // first latches
			uint64_t reverts = 0;           // applied value changed by contrary evidence (incl. regime switches)
			uint64_t regimeBypasses = 0;    // reverts that bypassed T_min at a regime boundary
			uint64_t tMinHolds = 0;         // evaluations in which contrary evidence was ready but T_min held it
			uint64_t holds = 0;             // HOLD (mixed) evaluations
			uint64_t abstains = 0;          // non-informative evaluations
			uint64_t unstableEntries = 0;
			uint64_t kernelChanges = 0;
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

		inline double KernelConfidence(double familyMargin, double scale)
		{
			if (!(familyMargin > 0.0) || !(scale > 0.0))
				return 0.0;
			return 1.0 - std::exp(-familyMargin / scale);
		}

		// Builds the latch's evidence from ChainEvidence's output for ONE axis after Submit. `verify` (optional) gives the sub-sample
		// phase of each candidate; without it the phase is 0.
		inline FieldEvidence MakeFieldEvidence(const AxisSummary& summary, const EvidenceUpdate& update, int axisLen,
			const LatchSettings& ls, const VerifyResult* verify = nullptr)
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
				h.kernelConf = h.kernelIndex >= 0 ? KernelConfidence(c.familyMargin, ls.kernelMarginScale) : 0.0;
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
							if ((cr->kernelMask & (1u << k)) == 0u || cr->kernelT[k] - cr->stat > ls.kernelClassMargin)
								continue;
							cls |= 1u << k;
							if (ChainKernelToClassical(k) >= 0 && (pick < 0 || cr->kernelT[k] < pickT))
							{
								pick = k;
								pickT = cr->kernelT[k];
							}
						}
						h.kernelClass = cls;
						const bool dips = cr->stat <= ls.kernelDipMax;
						h.kernelIndex = (dips && pick >= 0) ? pick : -1;
						h.kernelConf = h.kernelIndex >= 0 ? 1.0 : 0.0;
					}
				}
			}
			e.nativeCounted = update.nativeCounted;
			e.nativeIncrement = update.nativeIncrement;
			return e;
		}

		class ChainLatch
		{
		public:
			ChainLatch() {}
			ChainLatch(const LatchSettings& s, const LatchLevels& l) : s_(s), lv_(l) {}

			const LatchSettings& Settings() const { return s_; }
			const LatchLevels& Levels() const { return lv_; }
			const LatchStats& Stats() const { return stats_; }
			const LatchOutput& Last() const { return last_; }
			bool HasApplied() const { return hasApplied_; }
			bool AppliedNative() const { return hasApplied_ && applied_.native; }
			int AppliedN() const { return hasApplied_ ? applied_.n : 0; }

			// One evaluation of this axis.
			LatchOutput Step(const FieldEvidence& ev, const EvalContext& ctx)
			{
				++stats_.evaluations;
				if (ctx.discontinuity || ctx.activeAreaChanged)
					return DoReset(ev, ctx);
				if (!haveTime_ || ctx.timeUs > nowUs_)   // source time never steps backwards inside the latch
				{
					nowUs_ = ctx.timeUs;
					haveTime_ = true;
				}
				const int64_t now = nowUs_;
				const int cuts = ctx.cuts > 0 ? ctx.cuts : (ctx.cut ? 1 : 0);
				if (cuts > 0)
					shotSerial_ += static_cast<uint64_t>(cuts);
				if (cuts > 0 || ctx.blank || ctx.fade)
				{
					haveFlag_ = true;
					flagUs_ = (ctx.eventTimeUs >= 0 && ctx.eventTimeUs <= now) ? ctx.eventTimeUs : now;
					// A boundary: the positive runs of the contrary hypotheses restart and their gain is counted from here.
					for (int i = 0; i < kContrarySlots; ++i)
						if (contrary_[i].used)
						{
							contrary_[i].posEvals = 0;
							contrary_[i].regimeBase = contrary_[i].score;
							contrary_[i].regimeStart = false;
						}
				}
				if (ctx.repeat || !ev.evaluated)
				{
					LatchOutput o = last_;
					o.stateChanged = o.measuredChanged = o.appliedChanged = o.kernelChanged = false;
					o.bypassedTMin = o.resetEvidence = false;
					o.reason = LatchReason::Repeat;
					o.timeUs = now;
					last_ = o;
					return o;
				}
				if (ev.axisLen > 0)
					axisLen_ = ev.axisLen;
				LatchReason reason = LatchReason::NoEvidence;
				bool bypass = false;
				bool kernelChg = false;
				if (ev.hold)
				{
					++stats_.holds;
					overlay_ = LatchState::Hold;
					ResetRuns();
					reason = LatchReason::HeldMixed;
				}
				else if (!ev.informative)
				{
					++stats_.abstains;
					overlay_ = LatchState::Abstain;
					reason = hasApplied_ ? LatchReason::HeldNoEvidence : LatchReason::NoEvidence;
				}
				else
				{
					overlay_ = LatchState::Count;   // none
					if (haveInfUs_ && now - lastInfUs_ > s_.TMinUs())
						ResetRuns();   // a gap longer than T_min: the runs are not consecutive any more
					haveInfUs_ = true;
					lastInfUs_ = now;
					reason = hasApplied_ ? LatchedPath(ev, now, bypass, kernelChg) : Acquire(ev, now);
				}
				return Finish(ev, reason, bypass, kernelChg);
			}

		private:
			static constexpr int kContrarySlots = 6;

			// A CUSUM score of one hypothesis against the latched value.
			struct Slot
			{
				bool used = false;
				bool native = false;
				int n = 0;
				double score = 0.0;
				double shotBase = 0.0;
				uint64_t shotId = 0;
				bool shotSet = false;
				int posEvals = 0;             // consecutive evaluations with score > 0
				int64_t posStartUs = 0;
				bool regimeStart = false;     // that run started after a cut / blank / fade flag, within one dwell of it
				int c2Evals = 0;              // consecutive evaluations with score >= c2
				int64_t c2StartUs = 0;
				uint64_t touched = 0;         // the informative evaluation that last gave it an increment (runs are consecutive)
				double regimeBase = 0.0;      // the score when the newest boundary flag arrived: a regime switch counts the gain after it
				uint32_t kernelClass = 0;
				int kernelIndex = -1;
				double kernelConf = 0.0;
				double phase = 0.0;
			};

			struct Applied
			{
				bool native = false;
				int n = 0;
				double phase = 0.0;
				int kernelIndex = -1;         // -1 = not verified ("n known, kernel unsure")
				double kernelConf = 0.0;
				double llr = 0.0;
			};

			struct Measured
			{
				bool valid = false;
				bool native = false;
				int n = 0;
				double phase = 0.0;
				int kernelIndex = -1;
				double kernelConf = 0.0;
				double llr = 0.0;
			};

			static bool SameN(int a, int b) { return (a > b ? a - b : b - a) <= 1; }

			void ResetAll()
			{
				core_ = LatchState::Unknown;
				overlay_ = LatchState::Count;
				unstable_ = false;
				hasApplied_ = false;
				applied_ = Applied();
				measured_ = Measured();
				held_ = Slot();
				ClearContrary();
				runEvals_ = 0;
				runNative_ = false;
				runN_ = 0;
				shotSerial_ = 0;
				haveFlag_ = false;
				flagUs_ = 0;
				haveInfUs_ = false;
				lastInfUs_ = 0;
				hasLatchChange_ = false;
				lastLatchChangeUs_ = 0;
				lastKernelChangeUs_ = 0;
				kernelAltIndex_ = -1;
				kernelAltEvals_ = 0;
				kernelAltStartUs_ = 0;
				c1Evals_ = 0;
				c1StartUs_ = 0;
				c1StartShot_ = 0;
				evalSerial_ = 0;
				reported_ = Geometry::AxisGeom();
				hasLastChange_ = false;
				lastChangeFromNative_ = false;
				lastChangeFromN_ = 0;
				lastChangeAtUs_ = 0;
				unstableSinceUs_ = 0;
				unstableLowN_ = 0;
			}

			void ClearContrary()
			{
				for (int i = 0; i < kContrarySlots; ++i)
					contrary_[i] = Slot();
			}

			// Acquisition runs and the contrary evidence restart (HOLD, a long gap); the latched value and the held score stay.
			void ResetRuns()
			{
				runEvals_ = 0;
				c1Evals_ = 0;
				ClearContrary();
				kernelAltEvals_ = 0;
			}

			LatchOutput DoReset(const FieldEvidence& ev, const EvalContext& ctx)
			{
				const LatchOutput was = last_;
				ResetAll();
				nowUs_ = ctx.timeUs;
				haveTime_ = true;
				axisLen_ = ev.axisLen > 0 ? ev.axisLen : 0;
				LatchOutput o;
				o.state = LatchState::Unknown;
				o.measured = MakeGeom(false, false, 0, 0.0, -1, 0.0, 0.0, LatchState::Unknown, ev.reason);
				o.applied = MakeGeom(false, false, 0, 0.0, -1, 0.0, 0.0, LatchState::Unknown, ev.reason);
				o.stateChanged = was.state != LatchState::Unknown;
				o.measuredChanged = was.measured.n != 0;
				o.appliedChanged = was.applied.state != LatchState::Unknown;
				o.kernelChanged = was.applied.kernelIndex >= 0;
				o.resetEvidence = true;
				o.reason = ctx.discontinuity ? LatchReason::Discontinuity : LatchReason::ActiveArea;
				o.reasonBits = ev.reason;
				o.timeUs = ctx.timeUs;
				++stats_.discontinuities;
				last_ = o;
				return o;
			}

			Geometry::AxisGeom MakeGeom(bool valid, bool native, int n, double phase, int kernelIndex, double kernelConf, double llr,
				LatchState state, uint32_t reason) const
			{
				Geometry::AxisGeom g;
				g.state = state;
				g.N = axisLen_;
				g.n = valid ? (native ? axisLen_ : n) : 0;
				g.ratio = (valid && axisLen_ > 0) ? static_cast<float>(static_cast<double>(g.n) / static_cast<double>(axisLen_)) : 1.0f;
				g.phase = static_cast<float>(phase);
				if (valid && kernelIndex >= 0 && !native)
				{
					const KernelInfo& k = Kernel(kernelIndex);
					g.kernelFamily = static_cast<uint8_t>(k.family);
					g.kernelIndex = static_cast<int8_t>(kernelIndex);
					g.kernelParamA = static_cast<float>(k.a);
					g.kernelParamB = static_cast<float>(k.b);
				}
				g.kernelConf = static_cast<float>(kernelConf);
				g.conf = static_cast<float>(ConfFromLlr(llr));
				g.llr = static_cast<float>(llr);
				g.reasonBits = reason;
				return g;
			}

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

			// ---- acquiring: PROVISIONAL -> CANDIDATE -> LATCHED ----------------------------------------------------------
			LatchReason Acquire(const FieldEvidence& ev, int64_t now)
			{
				const bool nat = ev.nativeEvals > 0 && ev.nativeLlr > 0.0 && (!ev.hasBest || ev.nativeLlr > ev.bestLlr);
				const bool up = !nat && ev.hasBest && ev.bestLlr > 0.0;
				if (!nat && !up)
				{
					core_ = LatchState::Unknown;
					measured_ = Measured();
					runEvals_ = 0;
					c1Evals_ = 0;
					return LatchReason::NoEvidence;
				}
				const double llr = nat ? ev.nativeLlr : ev.bestLlr;
				const int n = nat ? 0 : ev.bestN;
				if (runEvals_ == 0 || runNative_ != nat || (!nat && !SameN(runN_, n)))
				{
					runNative_ = nat;
					runN_ = n;
					runEvals_ = 0;
					c1Evals_ = 0;
				}
				runN_ = n;
				++runEvals_;
				Measured m;
				m.valid = true;
				m.native = nat;
				m.n = nat ? axisLen_ : n;
				m.llr = llr;
				if (!nat)
				{
					const HypothesisIncrement* h = FindInc(ev, n);
					if (h != nullptr)
					{
						m.phase = h->phase;
						m.kernelIndex = h->kernelIndex;
						m.kernelConf = h->kernelConf;
					}
					else if (measured_.valid && !measured_.native && SameN(measured_.n, m.n))
					{
						m.phase = measured_.phase;
						m.kernelIndex = measured_.kernelIndex;
						m.kernelConf = measured_.kernelConf;
					}
				}
				measured_ = m;
				// The dwell clock runs while the evidence is at c1 or above (doc 78 D9: c1 over >= 4 evaluations spanning >= 2 shots or
				// >= 5 s); a dip below c1 or another hypothesis restarts it.
				if (llr >= lv_.c1 - 1e-9)
				{
					if (c1Evals_ == 0)
					{
						c1StartUs_ = now;
						c1StartShot_ = shotSerial_;
					}
					++c1Evals_;
				}
				else
					c1Evals_ = 0;
				const uint64_t shots = shotSerial_ - c1StartShot_ + 1;
				if (c1Evals_ > 0)
				{
					if (c1Evals_ >= s_.dwellEvaluations &&
						(shots >= static_cast<uint64_t>(s_.dwellShots) || now - c1StartUs_ >= s_.dwellUs))
					{
						LatchNow(m, now);
						++stats_.latches;
						return LatchReason::Latched;
					}
					core_ = LatchState::Candidate;
					return LatchReason::Candidate;
				}
				core_ = LatchState::Provisional;
				return LatchReason::Acquiring;
			}

			// Sets the applied (LATCHED) value from a measured hypothesis; resets the contrary evidence and the timers.
			void LatchNow(const Measured& m, int64_t now)
			{
				hasApplied_ = true;
				applied_.native = m.native;
				applied_.n = m.native ? axisLen_ : m.n;
				applied_.phase = m.phase;
				const bool verified = !m.native && m.kernelIndex >= 0 && m.kernelConf >= s_.kernelConfMin;
				applied_.kernelIndex = verified ? m.kernelIndex : -1;
				applied_.kernelConf = m.kernelConf;
				applied_.llr = m.llr;
				core_ = LatchState::Latched;
				hasLatchChange_ = true;
				lastLatchChangeUs_ = now;
				lastKernelChangeUs_ = now;
				kernelAltIndex_ = -1;
				kernelAltEvals_ = 0;
				held_ = Slot();
				held_.used = true;
				held_.native = m.native;
				held_.n = applied_.n;
				held_.score = lv_.c2;
				held_.kernelIndex = m.kernelIndex;
				held_.kernelConf = m.kernelConf;
				held_.phase = m.phase;
				ClearContrary();
			}

			// ---- CUSUM slots ---------------------------------------------------------------------------------------------
			bool RegimeFlagOk(int64_t now) const
			{
				return haveFlag_ && flagUs_ > lastLatchChangeUs_ && now - flagUs_ <= s_.dwellUs;
			}

			void UpdateSlot(Slot& s, double inc, int64_t now)
			{
				s.touched = evalSerial_;
				if (!s.shotSet || s.shotId != shotSerial_)
				{
					s.shotBase = s.score;
					s.shotId = shotSerial_;
					s.shotSet = true;
				}
				double v = s.score + inc;
				if (v < 0.0)
					v = 0.0;
				if (v > s.shotBase + lv_.shotCap)
					v = s.shotBase + lv_.shotCap;
				if (v > lv_.c2)
					v = lv_.c2;
				s.score = v;
				if (v > 0.0)
				{
					if (s.posEvals == 0)
					{
						s.posStartUs = now;
						s.regimeStart = RegimeFlagOk(now);
					}
					++s.posEvals;
				}
				else
					s.posEvals = 0;
				if (v >= lv_.c2 - 1e-9)
				{
					if (s.c2Evals == 0)
						s.c2StartUs = now;
					++s.c2Evals;
				}
				else
					s.c2Evals = 0;
			}

			Slot* FindContrary(bool native, int n)
			{
				for (int i = 0; i < kContrarySlots; ++i)
					if (contrary_[i].used && contrary_[i].native == native && (native || SameN(contrary_[i].n, n)))
						return &contrary_[i];
				return nullptr;
			}

			Slot* NewContrary(bool native, int n)
			{
				int w = -1;
				for (int i = 0; i < kContrarySlots; ++i)
				{
					if (!contrary_[i].used)
					{
						w = i;
						break;
					}
					if (w < 0 || contrary_[i].score < contrary_[w].score)
						w = i;
				}
				contrary_[w] = Slot();
				contrary_[w].used = true;
				contrary_[w].native = native;
				contrary_[w].n = n;
				return &contrary_[w];
			}

			const Slot* BestContrary() const
			{
				const Slot* best = nullptr;
				for (int i = 0; i < kContrarySlots; ++i)
				{
					const Slot& s = contrary_[i];
					if (!s.used)
						continue;
					if (best == nullptr || s.score > best->score ||
						(s.score == best->score && ((s.native && !best->native) || (s.native == best->native && s.n < best->n))))
						best = &s;
				}
				return best;
			}

			// Toward native = the contrary value is native, or a larger n / N than the applied one.
			bool TowardNative(const Slot& s) const
			{
				if (applied_.native)
					return false;
				return s.native || s.n > applied_.n;
			}

			// ---- latched ---------------------------------------------------------------------------------------------------
			LatchReason LatchedPath(const FieldEvidence& ev, int64_t now, bool& bypass, bool& kernelChg)
			{
				++evalSerial_;
				const HypothesisIncrement* heldInc = nullptr;
				for (int i = 0; i < ev.incCount && i < kMaxCandidates; ++i)
				{
					const HypothesisIncrement& h = ev.inc[i];
					if (!held_.native && SameN(h.n, applied_.n))
					{
						if (heldInc == nullptr || h.increment > heldInc->increment)
							heldInc = &h;
						continue;
					}
					Slot* s = FindContrary(false, h.n);
					if (s == nullptr)
					{
						if (!(h.increment > 0.0))
							continue;
						s = NewContrary(false, h.n);
					}
					s->n = h.n;
					s->kernelIndex = h.kernelIndex;
					s->kernelConf = h.kernelConf;
					s->kernelClass = h.kernelClass;
					s->phase = h.phase;
					UpdateSlot(*s, h.increment, now);
				}
				if (heldInc != nullptr)
				{
					UpdateSlot(held_, heldInc->increment, now);
					held_.kernelIndex = heldInc->kernelIndex;
					held_.kernelConf = heldInc->kernelConf;
					held_.phase = heldInc->phase;
				}
				if (ev.nativeCounted)
				{
					if (held_.native)
						UpdateSlot(held_, ev.nativeIncrement, now);
					else
					{
						Slot* s = FindContrary(true, 0);
						if (s == nullptr && ev.nativeIncrement > 0.0)
							s = NewContrary(true, 0);
						if (s != nullptr)
							UpdateSlot(*s, ev.nativeIncrement, now);
					}
				}

				// The runs are consecutive: a contrary hypothesis that got no increment in this evaluation loses its runs (its score stays).
				for (int i = 0; i < kContrarySlots; ++i)
					if (contrary_[i].used && contrary_[i].touched != evalSerial_)
					{
						contrary_[i].posEvals = 0;
						contrary_[i].c2Evals = 0;
					}

				// UNSTABLE ends once the window has passed since its entry (or the last real flip): the applied higher n stands, and a
				// lower partner that persists is then an ordinary contrary hypothesis.
				if (unstable_ && now - unstableSinceUs_ >= s_.unstableWindowUs)
					unstable_ = false;

				LatchReason reason = LatchReason::Agrees;
				if (heldInc != nullptr && AdoptKernel(*heldInc, now))
				{
					kernelChg = true;
					reason = LatchReason::KernelAdopted;
				}

				// Measured: the contrary hypothesis once it leads the held one, else the held one.
				const Slot* best = BestContrary();
				Measured m;
				m.valid = true;
				if (best != nullptr && best->score > held_.score)
				{
					m.native = best->native;
					m.n = best->native ? axisLen_ : best->n;
					m.phase = best->phase;
					m.kernelIndex = best->kernelIndex;
					m.kernelConf = best->kernelConf;
					m.llr = best->score;
				}
				else
				{
					m.native = applied_.native;
					m.n = applied_.n;
					m.phase = held_.phase;
					m.kernelIndex = held_.kernelIndex >= 0 ? held_.kernelIndex : applied_.kernelIndex;
					m.kernelConf = held_.kernelIndex >= 0 ? held_.kernelConf : applied_.kernelConf;
					m.llr = held_.score;
				}
				measured_ = m;

				// Revert candidates: sustained contrary evidence (T2, after T_min), or a regime switch toward native.
				const bool tOk = !hasLatchChange_ || now - lastLatchChangeUs_ >= s_.TMinUs();
				const Slot* stdBest = nullptr;
				const Slot* stdWait = nullptr;
				const Slot* regBest = nullptr;
				for (int i = 0; i < kContrarySlots; ++i)
				{
					const Slot& s = contrary_[i];
					if (!s.used)
						continue;
					const bool standard = s.c2Evals >= s_.contraryEvaluations && now - s.c2StartUs >= s_.contraryUs;
					const double need = (std::min)(lv_.shotCap, lv_.c2 - s.regimeBase);
					const bool regime = TowardNative(s) && s.posEvals >= s_.regimeEvaluations &&
						s.score - s.regimeBase >= need - 1e-9 && s.regimeStart;
					if (standard && tOk)
					{
						if (stdBest == nullptr || s.score > stdBest->score)
							stdBest = &s;
					}
					else if (standard)
					{
						if (stdWait == nullptr || s.score > stdWait->score)
							stdWait = &s;
					}
					if (regime && (regBest == nullptr || s.score > regBest->score))
						regBest = &s;
				}
				if (stdBest != nullptr)
					return ApplyChange(*stdBest, now, false, bypass);
				if (regBest != nullptr)
					return ApplyChange(*regBest, now, true, bypass);
				if (stdWait != nullptr)
				{
					++stats_.tMinHolds;
					return LatchReason::HeldMinHold;
				}
				if (reason == LatchReason::Agrees && best != nullptr && best->score > 0.0)
					return LatchReason::ContraryBuilding;
				return reason;
			}

			static uint32_t ClassOf(const HypothesisIncrement& h)
			{
				return h.kernelClass != 0u ? h.kernelClass : (h.kernelIndex >= 0 ? (1u << h.kernelIndex) : 0u);
			}

			// The kernel of the held hypothesis. The first adoption needs the same persistence as a change (T2: >= 3 consecutive
			// evaluations over >= 3 s); a CHANGE needs the evidence to leave the applied kernel's equivalence class (a within-class flip
			// of the best member, e.g. bicubic 0.5 vs 0.6, is no change), a sustained run (T2) and T_min since the last kernel change.
			// n and the latch state never move here.
			bool AdoptKernel(const HypothesisIncrement& h, int64_t now)
			{
				if (h.kernelIndex < 0 || h.kernelConf < s_.kernelConfMin)
				{
					kernelAltEvals_ = 0;
					return false;
				}
				const uint32_t cls = ClassOf(h);
				if (applied_.kernelIndex >= 0 && (cls & (1u << applied_.kernelIndex)) != 0u)
				{
					kernelAltEvals_ = 0;   // still the same class: the applied kernel stands
					applied_.kernelConf = h.kernelConf;
					return false;
				}
				// The run continues while the kernel proposed first is still in the class of the evidence (the best member may flip).
				if (kernelAltEvals_ > 0 && kernelAltIndex_ >= 0 && (cls & (1u << kernelAltIndex_)) != 0u)
					++kernelAltEvals_;
				else
				{
					kernelAltIndex_ = h.kernelIndex;
					kernelAltEvals_ = 1;
					kernelAltStartUs_ = now;
				}
				const bool tMinOk = applied_.kernelIndex < 0 || now - lastKernelChangeUs_ >= s_.TMinUs();
				if (kernelAltEvals_ >= s_.contraryEvaluations && now - kernelAltStartUs_ >= s_.contraryUs && tMinOk)
				{
					SetKernel(h, kernelAltIndex_, now);
					return true;
				}
				return false;
			}

			void SetKernel(const HypothesisIncrement& h, int index, int64_t now)
			{
				applied_.kernelIndex = index;
				applied_.kernelConf = h.kernelConf;
				applied_.phase = h.phase;
				lastKernelChangeUs_ = now;
				kernelAltEvals_ = 0;
				++stats_.kernelChanges;
			}

			// Changes the applied value to the contrary hypothesis (the slot is copied first: LatchNow clears the slots).
			LatchReason ApplyChange(const Slot& c, int64_t now, bool viaRegime, bool& bypass)
			{
				const bool toNative = c.native;
				const int newN = toNative ? axisLen_ : c.n;
				const int oldN = applied_.native ? axisLen_ : applied_.n;
				Measured m;
				m.valid = true;
				m.native = toNative;
				m.n = newN;
				m.phase = c.phase;
				m.kernelIndex = c.kernelIndex;
				m.kernelConf = c.kernelConf;
				m.llr = c.score;
				const bool inTMin = hasLatchChange_ && now - lastLatchChangeUs_ < s_.TMinUs();

				// The lower partner of an UNSTABLE field: inside the window since the entry the higher n stands; once the lower rung has
				// persisted past it, UNSTABLE ends and the partner is applied like any other contrary hypothesis.
				bool leavingUnstable = false;
				if (unstable_ && SameN(newN, unstableLowN_))
				{
					if (now - unstableSinceUs_ < s_.unstableWindowUs)
					{
						ClearContrary();
						return LatchReason::Unstable;
					}
					unstable_ = false;
					leavingUnstable = true;
				}
				const bool alternation = !leavingUnstable && hasLastChange_ && now - lastChangeAtUs_ < s_.unstableWindowUs &&
					lastChangeFromNative_ == toNative && (toNative || SameN(lastChangeFromN_, newN));
				LatchReason reason = viaRegime ? LatchReason::RegimeNative : LatchReason::Reverted;
				if (alternation)
				{
					++stats_.unstableEntries;
					unstable_ = true;
					unstableSinceUs_ = now;
					if (!(newN > oldN))
					{
						unstableLowN_ = newN;
						ClearContrary();
						return LatchReason::Unstable;
					}
					unstableLowN_ = oldN;
					reason = LatchReason::Unstable;
				}
				else
					unstable_ = false;

				hasLastChange_ = true;
				lastChangeFromNative_ = applied_.native;
				lastChangeFromN_ = applied_.n;
				lastChangeAtUs_ = now;
				LatchNow(m, now);
				++stats_.reverts;
				if (viaRegime && inTMin)
				{
					bypass = true;
					++stats_.regimeBypasses;
				}
				return reason;
			}

			// ---- output ----------------------------------------------------------------------------------------------------
			static bool Moved(const Geometry::AxisGeom& a, const Geometry::AxisGeom& b, double phaseTol)
			{
				const double dp = static_cast<double>(a.phase) - static_cast<double>(b.phase);
				// N never moves without a reset (active-area change), so it is not compared.
				return a.n != b.n || a.kernelIndex != b.kernelIndex || (dp < 0.0 ? -dp : dp) > phaseTol;
			}

			LatchOutput Finish(const FieldEvidence& ev, LatchReason reason, bool bypass, bool kernelChg)
			{
				LatchOutput o;
				o.timeUs = nowUs_;
				o.reason = reason;
				o.reasonBits = ev.reason;
				o.bypassedTMin = bypass;
				o.state = overlay_ != LatchState::Count ? overlay_ : (unstable_ ? LatchState::Unstable : core_);
				o.measured = MakeGeom(measured_.valid, measured_.native, measured_.n, measured_.phase, measured_.kernelIndex,
					measured_.kernelConf, measured_.llr, o.state, ev.reason);
				if (hasApplied_)
					o.applied = MakeGeom(true, applied_.native, applied_.n, applied_.phase, applied_.kernelIndex, applied_.kernelConf,
						applied_.llr, unstable_ ? LatchState::Unstable : LatchState::Latched, ev.reason);
				else
					o.applied = MakeGeom(false, false, 0, 0.0, -1, 0.0, 0.0, LatchState::Unknown, ev.reason);
				o.stateChanged = o.state != last_.state;
				// Against the last REPORTED measured geometry, so a slow phase drift cannot hide below the tolerance for ever.
				o.measuredChanged = Moved(reported_, o.measured, s_.phaseTolerance);
				if (o.measuredChanged)
					reported_ = o.measured;
				o.kernelChanged = last_.applied.kernelIndex != o.applied.kernelIndex || kernelChg;
				o.appliedChanged = Moved(last_.applied, o.applied, 1e300) || last_.applied.state != o.applied.state;
				last_ = o;
				return o;
			}

			LatchSettings s_;
			LatchLevels lv_;
			LatchStats stats_;
			LatchOutput last_;

			LatchState core_ = LatchState::Unknown;        // Unknown / Provisional / Candidate / Latched
			LatchState overlay_ = LatchState::Count;       // Hold / Abstain while they last, else Count (none)
			bool unstable_ = false;
			bool hasApplied_ = false;
			Applied applied_;
			Measured measured_;
			Slot held_;
			Slot contrary_[kContrarySlots];
			int axisLen_ = 0;
			int64_t nowUs_ = 0;
			bool haveTime_ = false;
			uint64_t shotSerial_ = 0;
			bool haveFlag_ = false;
			int64_t flagUs_ = 0;
			bool haveInfUs_ = false;
			int64_t lastInfUs_ = 0;
			// The acquisition run: the best hypothesis' identity and where it started.
			int runEvals_ = 0;
			bool runNative_ = false;
			int runN_ = 0;
			int c1Evals_ = 0;               // consecutive informative evaluations with the best hypothesis at c1 or above
			int64_t c1StartUs_ = 0;
			uint64_t c1StartShot_ = 0;
			uint64_t evalSerial_ = 0;       // informative evaluations on the latched path
			Geometry::AxisGeom reported_;   // the last measured geometry that was reported as changed
			bool hasLatchChange_ = false;
			int64_t lastLatchChangeUs_ = 0;
			int64_t lastKernelChangeUs_ = 0;
			int kernelAltIndex_ = -1;
			int kernelAltEvals_ = 0;
			int64_t kernelAltStartUs_ = 0;
			// The previous applied value and when it was replaced (alternation = UNSTABLE).
			bool hasLastChange_ = false;
			bool lastChangeFromNative_ = false;
			int lastChangeFromN_ = 0;
			int64_t lastChangeAtUs_ = 0;
			int64_t unstableSinceUs_ = 0;
			int unstableLowN_ = 0;
		};
	}
}
