// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// ChainEvidence (SID-C1 unit 4): turns verifier results into calibrated evidence, per axis. It is an
// ACCUMULATOR with reason bits; the per-axis reading (confident vs UNSURE, ChainReading.h) reads its summaries. There is no latch
// state machine anywhere (doc 78 section 5: parked).
// SID r2 (default off, EvidenceSettings::conventionOnly / pairPrior): R2 a candidate whose ratio is no published conventional geometry
// (SourceGeometrySet.h, ratios derived from the delivered raster, AxisObservation::rasterW/H) earns no evidence; R3 the two-axis prior is whether
// the ratio PAIR is one listed, NON-parked geometry (+agreementBonus) or not (-agreementBonus), and (r2 fix M5) a pair that is not one such
// geometry is NEVER above c1 on both axes: Summary vetoes both (kReasonNonConventional), so a mismatched pair such as 1920x720 cannot become
// confident through strong per-axis evidence. The latch lives in ChainLatch.h, above the readers; it confirms only on the evidence CREDITED by
// the newest evaluation (CreditAtN, r2 fix M1), never on the windowed total, so its hard bound limits the age of the evidence.
//
// floorCodes (AxisObservation) is the dof-NORMALISED sigma of the rounding/dither noise (1.155 for an 8-bit output,
// 0.289 for 10-bit), the same unit as the verifier's line baselines; the verifier's CandidateResult.floorCodes is
// the raw rms residual and must not be fed here.
// Clustering: exact sizes within +-1 of each other are ONE hypothesis (the best of them per evaluation counts, the
// slot follows its value), so refinement jitter cannot split the evidence.
// Per submitted evaluation and candidate:
//   line gate      a line is informative only when its baseline (detail above the rounding/dither floor) is at
//                  least lineGateFactor (1.5) x floorCodes; fewer than max(3, 8/32 of the lines) informative lines =>
//                  the candidate is not informative (kReasonLowDetail), abstain.
//   spatial        among the informative lines, those contradicting the majority (a robust distance from the
//                  median in the opposite direction: subtitles, PiP, menus, logos) are MASKED when they are
//                  below maskLimit (20%) of the lines, otherwise the candidate is mixed. The evaluation is a HOLD hint
//                  (kReasonMixed, no evidence accumulated) ONLY when the TOP hypothesis (the informative candidate
//                  with the lowest median line value) is mixed; a wrong candidate that happens to be mixed is just dropped. The per-256-px segment map is judged the same way along
//                  the lines (segments of informative content only).
//   increment      LLR from the NullTables class of n (ChainNullTables.h) of the masked trimmed-mean statistic,
//                  clamped to [incrementMin, incrementMax].
//   native         the native hypothesis only earns evidence when the power probe SAW its own synthetic
//                  upscale (otherwise kReasonNoPower and 0): -(best candidate increment), or a small constant
//                  when there was no candidate at all.
// Accumulation: per hypothesis (a latent size, or native) the increments of one SHOT (shotId from the caller,
// bumped at every cut) are summed and the shot's sum is capped at +-shotCapFraction*c1 (temporal correlation:
// frames of one shot are not independent), so c1 can only be reached over at least two shots.
// RECENT WINDOW (SID train r2, N3): the closed shots of a hypothesis expire after a window of SECONDS derived from the MEASURED
// evaluation rate (never a frame count): windowMargin x (c1 / windowYieldNats) evaluation intervals, clamped to
// [windowMinSec, windowMaxSec]; the interval is a running mean of the source-time gaps between submitted evaluations. So an ABR rung
// change mid-play is not outvoted for ever by hundreds of nats of old evidence. The window needs the evaluation's source time
// (AxisObservation::timeUs; -1 = unknown: nothing expires). windowMargin is 1.5 (SID revert window, owner-approved): W = 1.5 x (c1 / yield)
// x the acquiring interval, about 16 s at a 1 s interval (c1 14.873 / yield 1.4 = 10.6 evaluations needed, 15.9 in the window). The
// accumulation of an axis is RESET by ResetAxis (resync, format change, discontinuity) and by a HOLD evaluation (an overlay / menu / PiP over
// the picture: what came before says nothing about what is shown); the estimator also calls Reset() (both axes) when a menu / mixed episode
// ends, at the first cut after such an episode and on a ClearWindow event (ChainEstimator.h), so a switch to native at one of those
// moments is not outvoted by the old evidence. Black frames and ordinary cuts do not clear it.
// MINIMUM SHOT DWELL (SID-CAL2 E2, OWNER-APPROVED latch-semantics change, 2026-11-08): the evidence of a shot counts toward a candidate hypothesis only
// once that shot (the caller's shotId, bumped at every cut) has lasted at least kMinShotDwellSec of SOURCE time. The evidence of a shot is held while
// the shot is younger than the dwell and credited, in order and with its own times, the moment the shot has lasted it (an evaluation of the shot at
// least the dwell after its first, or the first evaluation of the NEXT shot at least the dwell after it: the cut time is the best end of a shot the
// sampled evaluations give). A shot that ends before it has lasted the dwell adds nothing: rapid cuts (a trailer, a montage; the calibration reel with cut ids 15, 17, 19, 21 inside 5 s)
// cannot add up to c1 out of single-evaluation shots. A shot that lasts the dwell is credited in full, so its evidence is the same as
// without the dwell, only later (by at most the dwell); the per-shot cap is unchanged. Source time unknown (timeUs < 0) = no dwell (nothing can be
// measured). The native hypothesis is not a candidate and is not held. No frame counts: the dwell is seconds.
// Two axes: when both axes' best hypotheses are positive, agreeing ratios add +agreementBonus to each and
// disagreeing ones subtract it; when one axis abstains nothing is added or subtracted (relaxed).
// Threshold: c1 = -ln(alpha) + ln(candidates x kernels), alpha = target rate per hour x interval / 3600. It is the DETECTION-confidence
// level (reading vs UNSURE); it adds no timing.
#include "ChainNullTables.h"
#include "DescaleVerifier.h"

#include <VP-V/modules/scaling/geometry/SourceGeometrySet.h>

namespace SourceIdentity
{
	namespace Chain
	{
		// SID-CAL2 E2 (owner decision 2026-11-08): minimum informative-shot dwell, source seconds. The evidence of a shot counts toward a candidate
		// once the shot has lasted this long (about the 2 x acquiring interval of a 1 s cadence plus one evaluation); shorter shots add no evidence.
		// 3 s: the calibration's live run showed false confident readings built from rapid cuts (cut ids 15, 17, 19, 21 within 5 s).
		constexpr double kMinShotDwellSec = 3.0;

		struct EvidenceSettings
		{
			double targetRatePerHour = 0.01;     // confident-wrong target: 1 per 100 title-hours
			double thresholdIntervalSec = 10.0;  // T in alpha = rate * T / 3600
			int candidateCount = kMaxCandidates; // hypotheses searched (multiplicity of the threshold)
			int kernelCount = kKernelCount;
			double shotCapFraction = 0.5;
			double agreementBonus = 2.0;
			double agreementTolerance = 0.015;   // relative difference of n/N counted as agreeing
			// SID E0 safety rules (owner 2026-11-07). Both OFF by default (EstimatorSettings::safetyRules turns them on).
			bool conventionOnly = false;         // R2: only candidate sizes whose ratio is a published conventional geometry (SourceGeometrySet.h) earn evidence
			bool pairPrior = false;              // R3: the two-axis prior is the convention PAIR (the ratio pair is one listed geometry), replacing the ratio agreement
			double lineGateFactor = 1.5;        // detail baseline must reach this x the rounding/dither floor (a cliff: tune with care)
			double minGateFraction = 0.25;       // 8 of 32
			int minGateLinesFloor = 3;
			double maskLimit = 0.20;
			double contradictionMin = 0.15;      // smallest distance from the median that contradicts (ln units)
			double majorityDip = -0.03;          // median below this = the majority sees a dip
			double incrementMax = 5.0;
			double incrementMin = -3.0;
			double nativeMax = 2.0;
			double nativeMin = -3.0;
			double nativeNoCandidate = 0.5;
			double trimFraction = 0.2;
			int maxHypotheses = 16;
			// The recent window (N3): seconds = windowMargin x (c1 / windowYieldNats) x measured evaluation interval, clamped.
			double windowYieldNats = 1.4;        // F0: the hard 8-bit grainy cases gain about 1.4 nats per evaluation
			double windowMargin = 1.5;           // the window spans this many times the time that yield needs to reach c1 (3.0 until the revert window: 31.9 s -> 15.9 s at 1 s)
			double windowMinSec = 10.0;
			double windowMaxSec = 600.0;
			double rateSmoothing = 0.25;         // weight of the newest gap in the running mean of the evaluation interval
			double minShotDwellSec = kMinShotDwellSec;   // E2: a shot's evidence counts once the shot lasted this many source seconds (0 = off)
		};

		struct Thresholds
		{
			double alpha = 0.0;
			double c1 = 0.0;
		};

		inline Thresholds ComputeThresholds(const EvidenceSettings& s)
		{
			Thresholds t;
			double alpha = s.targetRatePerHour * s.thresholdIntervalSec / 3600.0;
			if (!(alpha > 1e-12))
				alpha = 1e-12;
			if (alpha > 0.5)
				alpha = 0.5;
			t.alpha = alpha;
			const double multiplicity = static_cast<double>((std::max)(1, s.candidateCount)) * static_cast<double>((std::max)(1, s.kernelCount));
			t.c1 = -std::log(alpha) + std::log(multiplicity);
			return t;
		}

		struct AxisObservation
		{
			const VerifyResult* verify = nullptr;
			int axisLen = 0;
			double floorCodes = 0.3;      // rounding/dither floor estimate in 10-bit codes (8-bit output: about 1.15)
			bool probeRan = false;
			bool probeSeen = false;
			uint32_t probeReason = 0;     // reason bits of the PowerProbe (flat / dark / soft)
			uint32_t shotId = 0;          // bumped by the caller at every cut
			int64_t timeUs = -1;          // source time of the evaluation (non-decreasing); -1 = unknown: the recent window cannot expire anything
			int rasterW = 0;              // delivered raster (R2/R3 ratios, SourceGeometrySet.h); 0 = unknown: nothing is conventional
			int rasterH = 0;
		};

		// R5 support (r2 fix M1): the change of the windowed total of one hypothesis caused by one credit of the newest evaluation (after the
		// shot cap), with the source time of the evidence (a dwell-held credit keeps its own, older, time).
		struct EvidenceCredit
		{
			int n = 0;
			double gain = 0.0;            // change of the hypothesis' shot-capped windowed total
			double raw = 0.0;             // the increment before the cap
			int64_t timeUs = -1;
		};

		struct CandidateEvidence
		{
			int n = 0;
			int cls = kClassGeneric;
			bool informative = false;
			int linesGated = 0;
			int linesMasked = 0;
			bool gatedOk = false;         // enough informative lines
			bool nonConventional = false; // R2: judged informative but not a published conventional ratio: no evidence (conventionOnly)
			bool mixed = false;           // line or segment disagreement beyond the mask limit
			bool clustered = false;       // dropped: a neighbour within +-1 px has more evidence
			double median = 0.0;          // median line value of the informative lines (the top hypothesis has the lowest)
			double stat = 0.0;            // masked trimmed mean (valid only when statValid)
			bool statValid = false;       // the judge got as far as the statistic (the gate and the spatial agreement passed)
			double increment = 0.0;
			int kernel = -1;
			int family = -1;
			double kernelMargin = 0.0;
			double familyMargin = 0.0;
			uint32_t reason = 0;
		};

		struct EvidenceUpdate
		{
			bool informative = false;     // at least one candidate (or the native hypothesis) received evidence
			bool hold = false;            // mixed / overlay hint: nothing accumulated
			uint32_t reason = 0;
			int candidateCount = 0;
			CandidateEvidence cand[kMaxCandidates];
			bool nativeCounted = false;
			double nativeIncrement = 0.0;
		};

		struct AxisSummary
		{
			bool hasBest = false;
			int n = 0;                    // best hypothesis (0 = none)
			double ratio = 0.0;           // n / N
			double llr = 0.0;             // its own evidence (shot-capped)
			double prior = 0.0;           // two-axis agreement prior
			double llrWithPrior = 0.0;
			int informativeEvals = 0;
			int shots = 0;                // distinct shots with informative evaluations
			double nativeLlr = 0.0;
			int nativeEvals = 0;
			uint32_t lastReason = 0;
			bool aboveC1 = false;
			bool otherAxisAbstains = false;
		};

		class ChainEvidence
		{
		public:
			explicit ChainEvidence(const EvidenceSettings& s = EvidenceSettings(), const NullTables& t = NullTables::Placeholder())
				: s_(s), nulls_(t), thr_(ComputeThresholds(s))
			{
				Reset();
			}

			const Thresholds& GetThresholds() const { return thr_; }
			const NullTables& Nulls() const { return nulls_; }

			void Reset()
			{
				ResetAxis(0);
				ResetAxis(1);
				holdSinceCut_ = false;
			}

			// A cut was consumed (the estimator calls it for the evaluation that carries one, BEFORE its Submit). If a HOLD evaluation (an overlay /
			// menu / PiP) was read since the last clear, this is the first cut after such an episode: both axes are cleared (the window starts
			// over) and true is returned. An ordinary cut (no HOLD since the last clear) and a black / abstaining evaluation clear nothing.
			bool NoteCut()
			{
				if (!holdSinceCut_)
					return false;
				Reset();
				return true;
			}

			void ResetAxis(int axis)
			{
				AxisState& a = ax_[axis == 0 ? 0 : 1];
				a = AxisState();
			}

			// R5 support (M1): forgets the credits of the newest evaluation on both axes. The estimator calls it at the start of every evaluation,
			// so an axis that is not submitted this time reports no new credit (Submit clears its own axis too).
			void ClearCredits()
			{
				ax_[0].credits.clear();
				ax_[1].credits.clear();
			}

			// R5 support (M1): the evidence for size n (+-1 px) CREDITED by the newest Submit of the axis (the change of the shot-capped windowed
			// total, summed per slot; the best slot), the sum of the positive raw increments of that slot (before the cap) and the source time of
			// its newest positive credit (-1: none). 0 when nothing was credited: old evidence that still sits in the window is NOT new evidence.
			double CreditAtN(int axis, int n, int64_t* newestPositiveUs = nullptr, double* rawPositive = nullptr) const
			{
				const AxisState& st = ax_[axis == 0 ? 0 : 1];
				if (newestPositiveUs != nullptr)
					*newestPositiveUs = -1;
				if (rawPositive != nullptr)
					*rawPositive = 0.0;
				double bestRaw = 0.0;
				int64_t bestNewest = -1;
				double best = 0.0;
				bool any = false;
				for (size_t i = 0; i < st.credits.size(); ++i)
				{
					const int di = st.credits[i].n > n ? st.credits[i].n - n : n - st.credits[i].n;
					if (di > 1)
						continue;
					bool seen = false;   // this slot was already summed at an earlier credit
					for (size_t j = 0; j < i && !seen; ++j)
						seen = st.credits[j].n == st.credits[i].n;
					if (seen)
						continue;
					double sum = 0.0, raw = 0.0;
					int64_t newest = -1;
					for (size_t j = i; j < st.credits.size(); ++j)
					{
						if (st.credits[j].n != st.credits[i].n)
							continue;
						sum += st.credits[j].gain;
						if (st.credits[j].raw > 0.0)
							raw += st.credits[j].raw;
						if (st.credits[j].gain > 0.0 && st.credits[j].timeUs > newest)
							newest = st.credits[j].timeUs;
					}
					if (!any || sum > best)
					{
						best = sum;
						bestRaw = raw;
						bestNewest = newest;
					}
					any = true;
				}
				if (newestPositiveUs != nullptr)
					*newestPositiveUs = bestNewest;
				if (rawPositive != nullptr)
					*rawPositive = bestRaw;
				return any ? best : 0.0;
			}

			// One evaluation of one axis. Returns false only on unusable input (reason bits in `out`).
			bool Submit(int axis, const AxisObservation& obs, EvidenceUpdate& out)
			{
				out = EvidenceUpdate();
				AxisState& st = ax_[axis == 0 ? 0 : 1];
				st.credits.clear();
				if (obs.verify == nullptr || obs.axisLen < 64)
				{
					out.reason = kReasonBadInput;
					st.lastReason = out.reason;
					return false;
				}
				const double floorC = obs.floorCodes > 1e-3 ? obs.floorCodes : 1e-3;
				const VerifyResult& vr = *obs.verify;
				if (st.axisLen != 0 && st.axisLen != obs.axisLen)
					st = AxisState();   // the axis changed size (active area / zoom): old hypotheses mean nothing
				st.axisLen = obs.axisLen;
				st.rasterW = obs.rasterW;
				st.rasterH = obs.rasterH;
				NoteTime(st, obs.timeUs);
				const double cap = s_.shotCapFraction * thr_.c1;
				const bool dwellOn = s_.minShotDwellSec > 0.0 && obs.timeUs >= 0;   // E2: needs the source time
				if (dwellOn)
					TrackShot(st, obs.shotId, obs.timeUs, cap);
				else
					FlushPending(st, cap);   // no time on this evaluation: whatever was held counts now
				bool anyUsable = false;
				for (size_t i = 0; i < vr.candidates.size() && out.candidateCount < kMaxCandidates; ++i)
				{
					const CandidateResult& cr = vr.candidates[i];
					if (!cr.valid || !cr.exactDone || cr.lineCount < 1)
						continue;
					anyUsable = true;
					Judge(cr, obs.axisLen, floorC, out.cand[out.candidateCount]);
					CandidateEvidence& ce = out.cand[out.candidateCount++];
					// R2 (E0): a size that is not a conventional ratio of the measured axis is UNSURE material, never evidence.
					if (s_.conventionOnly && ce.informative
						&& !Scaling::GeometrySet::ConventionalAxis(ce.n, obs.axisLen, axis == 0 ? 0 : 1, obs.rasterW, obs.rasterH))
					{
						ce.informative = false;
						ce.nonConventional = true;
						ce.reason |= kReasonNonConventional;
					}
				}
				// The top hypothesis (informative lines, lowest median line value; ties: smaller n) decides HOLD.
				int top = -1;
				for (int i = 0; i < out.candidateCount; ++i)
				{
					if (!out.cand[i].gatedOk)
						continue;
					if (top < 0 || out.cand[i].median < out.cand[top].median
						|| (out.cand[i].median == out.cand[top].median && out.cand[i].n < out.cand[top].n))
						top = i;
				}
				for (int i = 0; i < out.candidateCount; ++i)
				{
					uint32_t r = out.cand[i].reason;
					if (i != top)
						r &= ~kReasonMixed;   // a wrong candidate's disagreement never holds the evaluation
					out.reason |= r;
				}
				if (top >= 0 && out.cand[top].mixed)
					out.hold = true;
				// Cluster informative candidates within +-1 px: the one with the larger increment (then the smaller n) stays.
				for (int i = 0; i < out.candidateCount; ++i)
				{
					if (!out.cand[i].informative)
						continue;
					for (int j = 0; j < out.candidateCount; ++j)
					{
						if (j == i || !out.cand[j].informative)
							continue;
						const int d = out.cand[i].n > out.cand[j].n ? out.cand[i].n - out.cand[j].n : out.cand[j].n - out.cand[i].n;
						if (d > 1)
							continue;
						const bool jBetter = out.cand[j].increment > out.cand[i].increment
							|| (out.cand[j].increment == out.cand[i].increment && out.cand[j].n < out.cand[i].n);
						if (jBetter)
						{
							out.cand[i].clustered = true;
							break;
						}
					}
				}
				bool anyInformative = false;
				double maxInc = -1e300;
				for (int i = 0; i < out.candidateCount; ++i)
				{
					if (out.cand[i].clustered)
						out.cand[i].informative = false;
					if (out.cand[i].informative)
					{
						anyInformative = true;
						if (out.cand[i].increment > maxInc)
							maxInc = out.cand[i].increment;
					}
				}
				out.reason |= vr.reason;
				if (!anyUsable)
					out.reason |= kReasonNoCandidates;

				if (out.hold)
				{
					out.reason |= kReasonMixed;
					st.lastReason = out.reason;
					ForgetAccumulation(st);   // an overlay / menu / PiP: nothing accumulated, and what came before says nothing about it
					holdSinceCut_ = true;     // the first cut after the episode clears both axes (NoteCut)
					return true;
				}

				// Native hypothesis: only with proven power.
				if (obs.probeSeen)
				{
					out.nativeCounted = true;
					if (anyInformative)
						out.nativeIncrement = ClampD(-maxInc, s_.nativeMin, s_.nativeMax);
					else if (!anyUsable)
						out.nativeIncrement = s_.nativeNoCandidate;
					else
						out.nativeCounted = false;   // candidates present but none informative: nothing to say
				}
				else
				{
					out.reason |= kReasonNoPower | obs.probeReason;
				}

				for (int i = 0; i < out.candidateCount; ++i)
					if (out.cand[i].informative)
					{
						if (dwellOn && !st.shotMet)
						{
							// E2: the shot has not lasted the dwell yet: held (credited when it does, dropped when it ends first).
							PendingEvidence p;
							p.n = out.cand[i].n;
							p.increment = out.cand[i].increment;
							p.shotId = obs.shotId;
							p.timeUs = obs.timeUs;
							st.pending.push_back(p);
						}
						else
							AddCandidate(st, out.cand[i].n, out.cand[i].increment, obs.shotId, obs.timeUs, cap);
					}
				if (out.nativeCounted)
					Add(st.native, out.nativeIncrement, obs.shotId, obs.timeUs, cap);

				out.informative = anyInformative || out.nativeCounted;
				if (out.informative)
				{
					++st.informativeEvals;
					if (st.lastInformativeShot != obs.shotId || st.shots == 0)
					{
						++st.shots;
						st.lastInformativeShot = obs.shotId;
					}
				}
				st.lastReason = out.reason;
				return true;
			}

			// R5 support: the evidence of the size n on one axis (the slot within +-1 px), shot-capped, WITHOUT the two-axis prior; -1e9 = none.
			double LlrAtN(int axis, int n) const
			{
				const AxisState& st = ax_[axis == 0 ? 0 : 1];
				const double cap = s_.shotCapFraction * thr_.c1;
				double best = -1e9;
				for (size_t i = 0; i < st.slots.size(); ++i)
				{
					const int d = st.slots[i].n > n ? st.slots[i].n - n : n - st.slots[i].n;
					if (d > 1)
						continue;
					const double v = Total(st.slots[i], cap);
					if (v > best)
						best = v;
				}
				return best;
			}

			// The recent window of an axis in seconds (N3): derived from the measured evaluation rate; 0 = the rate is not measured yet (no window).
			double WindowSeconds(int axis) const { return WindowUs(ax_[axis == 0 ? 0 : 1]) / 1e6; }

			AxisSummary Summary(int axis) const
			{
				const int a = axis == 0 ? 0 : 1;
				AxisSummary r = WithPrior(a);
				if (s_.pairPrior && r.aboveC1)
				{
					// M5: a pair that is not one listed, non-parked geometry is never confident on BOTH axes (the prior alone, -bonus, does not
					// guarantee it: strong per-axis evidence can pass c1 + bonus). Both axes are vetoed, the safe direction (native never wrongly
					// called upscaled; single-axis re-base is parked anyway).
					const AxisSummary o = WithPrior(1 - a);
					if (o.aboveC1 && !PairAgrees(a == 0 ? r.n : o.n, a == 0 ? o.n : r.n))
					{
						r.aboveC1 = false;
						r.lastReason |= kReasonNonConventional;
					}
				}
				return r;
			}

		private:
			int PairRasterW() const { return ax_[0].rasterW > 0 ? ax_[0].rasterW : ax_[1].rasterW; }
			int PairRasterH() const { return ax_[0].rasterH > 0 ? ax_[0].rasterH : ax_[1].rasterH; }

			// R3 (L1): the ratio PAIR (x first) is one listed, NON-parked geometry on the delivered raster.
			bool PairAgrees(int nx, int ny) const
			{
				return Scaling::GeometrySet::AgreeingPair(nx, ax_[0].axisLen, ny, ax_[1].axisLen, PairRasterW(), PairRasterH());
			}

			AxisSummary WithPrior(int a) const
			{
				AxisSummary r = Own(ax_[a]);
				const AxisSummary o = Own(ax_[1 - a]);
				r.otherAxisAbstains = o.informativeEvals == 0 || !o.hasBest || o.llr <= 0.0;
				if (r.hasBest && r.llr > 0.0 && o.hasBest && o.llr > 0.0)
				{
					if (s_.pairPrior)
					{
						// R3: a non-square listed pair agrees too; an unlisted pair and the PARKED anamorphic pair do not (L1).
						r.prior = PairAgrees(a == 0 ? r.n : o.n, a == 0 ? o.n : r.n) ? s_.agreementBonus : -s_.agreementBonus;
					}
					else
					{
						const double d = std::abs(r.ratio - o.ratio);
						r.prior = (d <= s_.agreementTolerance * r.ratio) ? s_.agreementBonus : -s_.agreementBonus;
					}
				}
				r.llrWithPrior = r.llr + r.prior;
				r.aboveC1 = r.hasBest && r.llrWithPrior >= thr_.c1;
				return r;
			}

			struct ClosedShot
			{
				int64_t timeUs = -1;     // source time of the shot's newest increment (-1: unknown, never expires)
				double value = 0.0;      // the shot's sum, capped
			};

			struct Hypothesis
			{
				int n = 0;               // 0 = the native hypothesis
				std::vector<ClosedShot> closed;   // closed shots inside the recent window, each capped
				double shotSum = 0.0;    // the open shot
				int64_t shotTimeUs = -1; // source time of the open shot's newest increment
				uint32_t shotId = 0;
				int shots = 0;
				int evals = 0;
			};

			struct PendingEvidence
			{
				int n = 0;
				double increment = 0.0;
				uint32_t shotId = 0;
				int64_t timeUs = -1;
			};

			struct AxisState
			{
				// E2 (minimum shot dwell): the shot being gathered, when it started, whether it has lasted the dwell, and its held evidence.
				bool haveShot = false;
				uint32_t shotKey = 0;
				int64_t shotStartUs = 0;     // source time of the first evaluation of the shot
				bool shotMet = false;        // the shot has lasted the dwell: its evidence counts at once
				std::vector<PendingEvidence> pending;
				int64_t lastTimeUs = -1;     // source time of the newest submitted evaluation
				double intervalUs = 0.0;     // running mean of the gaps between submitted evaluations (0: not measured yet)
				std::vector<Hypothesis> slots;
				Hypothesis native;
				int informativeEvals = 0;
				int shots = 0;
				uint32_t lastInformativeShot = 0;
				uint32_t lastReason = 0;
				int axisLen = 0;
				int rasterW = 0;
				int rasterH = 0;
				std::vector<EvidenceCredit> credits;   // M1: what the newest Submit credited (cleared by Submit and ClearCredits)
			};

			void AddCandidate(AxisState& st, int n, double increment, uint32_t shotId, int64_t timeUs, double cap)
			{
				Hypothesis& h = FindSlot(st, n);
				const double before = Total(h, cap);
				h.n = n;   // the slot follows the latest best exact size
				Add(h, increment, shotId, timeUs, cap);
				EvidenceCredit c;
				c.n = h.n;
				c.gain = Total(h, cap) - before;
				c.raw = increment;
				c.timeUs = timeUs;
				st.credits.push_back(c);
			}

			// E2: the held evidence of a shot that has lasted the dwell is credited, in order and with its own shot id and time.
			void FlushPending(AxisState& st, double cap)
			{
				if (st.pending.empty())
					return;
				std::vector<PendingEvidence> held;
				held.swap(st.pending);
				for (size_t i = 0; i < held.size(); ++i)
					AddCandidate(st, held[i].n, held[i].increment, held[i].shotId, held[i].timeUs, cap);
			}

			// E2: follows the shot of the evaluation (source time known). A shot that ends (the next shot's first evaluation) after at least the dwell is
			// credited at that evaluation; a shot that ends sooner adds nothing (its held evidence is dropped).
			void TrackShot(AxisState& st, uint32_t shotId, int64_t timeUs, double cap)
			{
				const int64_t dwellUs = static_cast<int64_t>(s_.minShotDwellSec * 1e6);
				if (!st.haveShot || st.shotKey != shotId)
				{
					if (st.haveShot && !st.shotMet && timeUs - st.shotStartUs >= dwellUs)
					{
						st.shotMet = true;
						FlushPending(st, cap);
					}
					st.pending.clear();
					st.haveShot = true;
					st.shotKey = shotId;
					st.shotStartUs = timeUs;
					st.shotMet = false;
				}
				else if (!st.shotMet && timeUs - st.shotStartUs >= dwellUs)
				{
					st.shotMet = true;
					FlushPending(st, cap);
				}
			}

			Hypothesis& FindSlot(AxisState& st, int n)
			{
				int nearIdx = -1;
				int nearD = 0;
				for (size_t i = 0; i < st.slots.size(); ++i)
				{
					const int d = st.slots[i].n > n ? st.slots[i].n - n : n - st.slots[i].n;
					if (d > 1)
						continue;
					if (nearIdx < 0 || d < nearD || (d == nearD && st.slots[i].n < st.slots[static_cast<size_t>(nearIdx)].n))
					{
						nearIdx = static_cast<int>(i);
						nearD = d;
					}
				}
				if (nearIdx >= 0)
					return st.slots[static_cast<size_t>(nearIdx)];
				Hypothesis h;
				h.n = n;
				if (static_cast<int>(st.slots.size()) < (std::max)(1, s_.maxHypotheses))
				{
					st.slots.push_back(h);
					return st.slots.back();
				}
				// Full: replace the weakest (lowest total, then the larger n).
				size_t w = 0;
				for (size_t i = 1; i < st.slots.size(); ++i)
				{
					const double a = Total(st.slots[i], 1e300);
					const double b = Total(st.slots[w], 1e300);
					if (a < b || (a == b && st.slots[i].n > st.slots[w].n))
						w = i;
				}
				st.slots[w] = h;
				return st.slots[w];
			}

			static double Total(const Hypothesis& h, double cap)
			{
				double t = ClampD(h.shotSum, -cap, cap);
				for (size_t i = 0; i < h.closed.size(); ++i)
					t += h.closed[i].value;
				return t;
			}

			static void Add(Hypothesis& h, double inc, uint32_t shotId, int64_t timeUs, double cap)
			{
				if (h.shots == 0 || h.shotId != shotId)
				{
					if (h.shots > 0)
					{
						ClosedShot c;
						c.timeUs = h.shotTimeUs;
						c.value = ClampD(h.shotSum, -cap, cap);
						h.closed.push_back(c);
					}
					h.shotSum = 0.0;
					h.shotId = shotId;
					++h.shots;
				}
				h.shotSum += inc;
				h.shotTimeUs = timeUs;
				++h.evals;
			}

			// The running mean of the gaps between submitted evaluations (source time) and the window it implies; expires the closed
			// shots (and an open shot) older than the window. Without a time nothing expires.
			double WindowUs(const AxisState& st) const
			{
				if (!(st.intervalUs > 0.0))
					return 0.0;   // not measured: no window
				const double yield = s_.windowYieldNats > 1e-9 ? s_.windowYieldNats : 1e-9;
				const double sec = ClampD(s_.windowMargin * (thr_.c1 / yield) * (st.intervalUs / 1e6), s_.windowMinSec, s_.windowMaxSec);
				return sec * 1e6;
			}

			void NoteTime(AxisState& st, int64_t timeUs)
			{
				if (timeUs < 0)
					return;
				if (st.lastTimeUs >= 0 && timeUs > st.lastTimeUs)
				{
					const double gap = static_cast<double>(timeUs - st.lastTimeUs);
					st.intervalUs = st.intervalUs > 0.0 ? (1.0 - s_.rateSmoothing) * st.intervalUs + s_.rateSmoothing * gap : gap;
				}
				if (st.lastTimeUs < 0 || timeUs > st.lastTimeUs)
					st.lastTimeUs = timeUs;
				const double window = WindowUs(st);
				if (!(window > 0.0))
					return;
				const double cutoff = static_cast<double>(st.lastTimeUs) - window;
				for (size_t i = 0; i < st.slots.size();)
				{
					Expire(st.slots[i], cutoff);
					if (st.slots[i].closed.empty() && st.slots[i].shots == 0)
						st.slots.erase(st.slots.begin() + static_cast<std::ptrdiff_t>(i));
					else
						++i;
				}
				Expire(st.native, cutoff);
			}

			static void Expire(Hypothesis& h, double cutoff)
			{
				size_t keep = 0;
				while (keep < h.closed.size() && h.closed[keep].timeUs >= 0 && static_cast<double>(h.closed[keep].timeUs) < cutoff)
					++keep;
				if (keep > 0)
					h.closed.erase(h.closed.begin(), h.closed.begin() + static_cast<std::ptrdiff_t>(keep));
				if (h.shots > 0 && h.shotTimeUs >= 0 && static_cast<double>(h.shotTimeUs) < cutoff)
				{
					// The open shot itself is outside the window: the hypothesis has no live evidence left.
					if (h.closed.empty())
						h = Hypothesis();
					else
					{
						h.shotSum = 0.0;
						h.shotTimeUs = -1;
					}
				}
			}

			// HOLD / overlay: the accumulation of this axis starts over (the rate measurement and the axis length stay).
			static void ForgetAccumulation(AxisState& st)
			{
				st.slots.clear();
				st.pending.clear();       // E2: the held evidence of the shot belongs to what the overlay replaces
				st.native = Hypothesis();
				st.informativeEvals = 0;
				st.shots = 0;
				st.lastInformativeShot = 0;
			}

			AxisSummary Own(const AxisState& st) const
			{
				AxisSummary r;
				r.informativeEvals = st.informativeEvals;
				r.shots = st.shots;
				r.lastReason = st.lastReason;
				const double cap = s_.shotCapFraction * thr_.c1;
				r.nativeLlr = Total(st.native, cap);
				r.nativeEvals = st.native.evals;
				int best = -1;
				double bestV = 0.0;
				for (size_t i = 0; i < st.slots.size(); ++i)
				{
					const double v = Total(st.slots[i], cap);
					if (best < 0 || v > bestV || (v == bestV && st.slots[i].n < st.slots[static_cast<size_t>(best)].n))
					{
						best = static_cast<int>(i);
						bestV = v;
					}
				}
				if (best >= 0)
				{
					r.hasBest = true;
					r.n = st.slots[static_cast<size_t>(best)].n;
					r.llr = bestV;
					r.ratio = st.axisLen > 0 ? static_cast<double>(r.n) / static_cast<double>(st.axisLen) : 0.0;
				}
				return r;
			}

			// Judges one verified candidate: gate, spatial agreement, statistic and increment.
			void Judge(const CandidateResult& cr, int axisLen, double floorC, CandidateEvidence& ce) const
			{
				ce.n = cr.n;
				ce.kernel = cr.kernel;
				ce.family = cr.family;
				ce.kernelMargin = cr.kernelMargin;
				ce.familyMargin = cr.familyMargin;
				ce.cls = ClassifyLatent(cr.n, axisLen);
				const int nl = cr.lineCount;
				std::vector<char> gate(static_cast<size_t>(nl), 0);
				int gated = 0;
				for (int j = 0; j < nl; ++j)
					if (cr.lineBase[static_cast<size_t>(j)] >= s_.lineGateFactor * floorC)
					{
						gate[static_cast<size_t>(j)] = 1;
						++gated;
					}
				ce.linesGated = gated;
				const int minGate = (std::max)(s_.minGateLinesFloor, static_cast<int>(std::ceil(s_.minGateFraction * static_cast<double>(nl))));
				if (gated < minGate)
				{
					ce.reason |= kReasonLowDetail;
					return;
				}
				ce.gatedOk = true;
				// Spatial agreement across lines.
				std::vector<double> v;
				std::vector<int> who;
				for (int j = 0; j < nl; ++j)
					if (gate[static_cast<size_t>(j)])
					{
						v.push_back(cr.lineStat[static_cast<size_t>(j)]);
						who.push_back(j);
					}
				const double med = MedianOf(v.data(), static_cast<int>(v.size()));
				ce.median = med;
				std::vector<double> dev(v.size());
				for (size_t i = 0; i < v.size(); ++i)
					dev[i] = std::abs(v[i] - med);
				const double mad = MedianOf(dev.data(), static_cast<int>(dev.size()));
				const double tau = (std::max)(s_.contradictionMin, 3.0 * 1.4826 * mad);
				const bool dipMajority = med < s_.majorityDip;
				std::vector<double> kept;
				int contradict = 0;
				for (size_t i = 0; i < v.size(); ++i)
				{
					const bool contra = dipMajority ? (v[i] - med >= tau) : (med - v[i] >= tau);
					if (contra)
						++contradict;
					else
						kept.push_back(v[i]);
				}
				const double frac = static_cast<double>(contradict) / static_cast<double>(v.size());
				ce.linesMasked = contradict;
				bool mixed = frac > s_.maskLimit;
				if (!mixed && contradict > 0)
					ce.reason |= kReasonMasked;
				// Spatial agreement along the lines: segments of informative content only.
				if (!mixed && cr.segCount >= 4)
				{
					std::vector<double> segMed;
					for (int g = 0; g < cr.segCount; ++g)
					{
						std::vector<double> col;
						for (size_t i = 0; i < who.size(); ++i)
						{
							const size_t cell = static_cast<size_t>(who[i]) * static_cast<size_t>(cr.segCount) + static_cast<size_t>(g);
							if (cr.segRows[cell] >= kSegPx / 2 && cr.segBase[cell] >= s_.lineGateFactor * floorC)
								col.push_back(cr.segStat[cell]);
						}
						if (col.size() >= 3)
							segMed.push_back(MedianOf(col.data(), static_cast<int>(col.size())));
					}
					if (segMed.size() >= 4)
					{
						const double m2 = MedianOf(segMed.data(), static_cast<int>(segMed.size()));
						std::vector<double> d2(segMed.size());
						for (size_t i = 0; i < segMed.size(); ++i)
							d2[i] = std::abs(segMed[i] - m2);
						const double mad2 = MedianOf(d2.data(), static_cast<int>(d2.size()));
						const double tau2 = (std::max)(s_.contradictionMin, 3.0 * 1.4826 * mad2);
						const bool dip2 = m2 < s_.majorityDip;
						int bad = 0;
						for (size_t i = 0; i < segMed.size(); ++i)
							if (dip2 ? (segMed[i] - m2 >= tau2) : (m2 - segMed[i] >= tau2))
								++bad;
						if (static_cast<double>(bad) / static_cast<double>(segMed.size()) > s_.maskLimit)
							mixed = true;
					}
				}
				if (mixed)
				{
					ce.mixed = true;
					ce.reason |= kReasonMixed;
					return;
				}
				if (static_cast<int>(kept.size()) < minGate)
				{
					ce.reason |= kReasonLowDetail;
					return;
				}
				ce.stat = TrimmedMeanOf(kept.data(), static_cast<int>(kept.size()), s_.trimFraction);
				ce.statValid = true;
				ce.increment = ClampD(nulls_.LogLikelihoodRatio(ce.cls, ce.stat), s_.incrementMin, s_.incrementMax);
				ce.informative = true;
			}

			EvidenceSettings s_;
			NullTables nulls_;
			Thresholds thr_;
			AxisState ax_[2];
			bool holdSinceCut_ = false;   // a HOLD evaluation was read since the last clear: the next cut clears the window (NoteCut)
		};
	}
}
