// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// UpscaleLatch (SID r2 safety rules, E0 rule R5 + R7, owner decisions 2026-11-07): the ASYMMETRIC latch over the two axis readings.
//
//   NATIVE is sticky: the stream starts there and nothing but a conventional, doubly confirmed upscaled PAIR leaves it.
//   An UPSCALED latch must earn its keep:
//     enter / move  both axes above the BAR = c1 x (1 + clamp((stable - graceSec) / growSec, 0, growMax)): a long stable stream raises the bar
//                   (a codec-grid fluke at 1/2 gets harder to believe the longer the picture stays put). The pair must be one published,
//                   NON-parked conventional geometry on the delivered raster (scaling.geometry SourceGeometrySet.h, R2): anything else stays
//                   UNSURE, visibly (heldBack). Moving between upscaled sizes uses the same bar; while latched the estimator publishes ONLY
//                   the latched size (a different confident pair is never published without a Move: r2 fix H1, ChainEstimator ApplyLatch).
//     keep          re-confirmed by NEW evidence (r2 fix M1): the evidence CREDITED to the latched size since the last confirmation (the change
//                   of the shot-capped windowed total that each new evaluation causes, ChainEvidence::CreditAtN; net, floored at -need) reaches
//                   confirmFraction x c1 (E0: 0.25) on BOTH axes. Evidence that merely still sits in the recent window does not re-confirm,
//                   so the bounds below are bounds on the AGE of the evidence (the window can be about 80 s at the latched 5 s cadence).
//                   While not re-confirmed a release clock runs (source seconds); it runs at absentClockRate (R5b: 1/4 speed) through
//                   evaluations that earned no evidence at all (a dark / flat / soft stretch says nothing against the latch) and through
//                   evaluations whose only evidence was a positive increment of the latched size that the shot cap absorbed (consistent,
//                   but not independent evidence: a long shot of a true upscale), so a true upscale is not flickered off.
//     release       the clock passes releaseBoundSec, OR hardBoundSec of source time pass since the newest evidence that confirmed (no lock
//                   without release; counted apart as hard-bound releases), OR native evidence (the readers say native on both axes), OR a
//                   menu / overlay HOLD (R7: the latch is not carried into a menu: E0 found 4 menu reels latched 1920x1080 through a 7 s
//                   native menu), OR (a switch, default OFF) an ABR rendition step, OR Reset (a discontinuity). Every release is visible
//                   (the output, the status counters, the one-time notice) and counted; after one the bar restarts. A release for lack of
//                   new evidence also discredits the recent window (the estimator clears it), so stale evidence cannot re-enter at once.
//   N1 (r2 re-check, coordinator 2026-11-11): the two EVIDENCE-STARVATION releases (the release clock and the hard bound) do not change the
//                 picture mid-shot. Reaching either bound is counted at once (releasesBound / releasesHardBound, and `deferred`) and the
//                 release is DEFERRED to the next shot boundary (a cut, black or fade of the evaluation: LatchInput::shotBoundary); the latch
//                 stays held and published meanwhile, visibly (Deferred(), the status). From the first evaluation after the boundary the new
//                 shot has ONE dwell of deferDwellSec (3 s: ChainEvidence's minimum shot dwell, so the new shot's evidence is credited by
//                 then) to re-confirm: a re-confirmation (new evidence on both axes, the rule above; at any time while deferred) CANCELS
//                 the release (counted: deferCancelled), so a true upscale never changes the picture; otherwise it releases at the dwell's
//                 end (releasesAtCut). With no boundary at all the release happens deferCeilingSec (60 s) after the bound (so at most
//                 hardBoundSec + 60 = 180 s after the newest confirming evidence) as LatchEvent::ReleasedCeiling, which the monitor shows
//                 through its engage crossfade instead of the release fade. Native, menu, ABR-step and reset releases stay immediate (also
//                 while deferred). The bounds are NOT scaled with the shot length.
//   A backward source-time step without a reset (L5) re-anchors the stable and confirmation times to the new time (the hard bound keeps running).
// It reuses the existing inputs only (rules 24 / 25): ChainEvidence's per-axis evidence and credits, the HOLD evaluations and the monitor's
// mixed-episode events (WindowClearPolicy), the AbrStepDetector's events. It adds no private measurement. Pure, no clock of its own, header-only,
// C++14.
#include "ChainCommon.h"

#include <VP-V/modules/scaling/geometry/SourceGeometrySet.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace SourceIdentity
{
	namespace Chain
	{
		struct LatchSettings
		{
			bool enabled = false;               // OFF: the layer is inert (the readings pass unchanged)
			double confirmFraction = 0.25;      // of c1: the NEW evidence of the latched size on both axes that re-confirms it (E0)
			double releaseBoundSec = 60.0;      // release clock bound (E0: 30 s flickers, 60 s shows no release on true upscales)
			double absentClockRate = 0.25;      // R5b: the clock's speed through evaluations that earned no (independent) evidence
			double hardBoundSec = 120.0;        // R5b: source seconds since the newest confirming evidence that always release
			double graceSec = 30.0;             // bar growth starts after this much stable time
			double growSec = 120.0;             // ... and reaches growMax over this much more
			double growMax = 1.0;               // the bar at most doubles
			bool abrStepTrigger = false;        // a switch, OFF: an ABR rendition step drops the latch (E0: only helps after a rendition change)
			double movedTolerance = 0.004;      // relative change of a ratio that is a move to another size
			// N1: the evidence-starvation releases wait for the next shot boundary (true), or release at the bound (false: the pre-N1 rule).
			bool deferStarvation = true;
			double deferDwellSec = 3.0;         // N1: the new shot's one dwell to re-confirm (= ChainEvidence kMinShotDwellSec)
			double deferCeilingSec = 60.0;      // N1: no boundary within this long after the bound: released (crossfade) anyway
		};

		enum class LatchEvent : int
		{
			None = 0,
			Entered,          // native -> upscaled latch
			Moved,            // latched size -> another conventional size (same bar)
			Released,         // not re-confirmed within the release clock bound (to native)
			ReleasedNative,   // the readers see native on both axes
			ReleasedMenu,     // a menu / overlay HOLD
			ReleasedAbr,      // an ABR step (switch)
			ReleasedReset,    // a discontinuity / active-area change
			ReleasedHardBound, // no new confirming evidence for hardBoundSec (R5b)
			ReleasedCeiling   // N1: a deferred starvation release with no shot boundary within deferCeilingSec (shown by a crossfade)
		};

		inline const char* LatchEventText(LatchEvent e)
		{
			switch (e)
			{
			case LatchEvent::Entered: return "latched";
			case LatchEvent::Moved: return "moved to another size";
			case LatchEvent::Released: return "released to native (not re-confirmed in time)";
			case LatchEvent::ReleasedNative: return "released to native (native evidence)";
			case LatchEvent::ReleasedMenu: return "released to native (menu / overlay)";
			case LatchEvent::ReleasedAbr: return "released to native (rendition step)";
			case LatchEvent::ReleasedReset: return "released to native (reset)";
			case LatchEvent::ReleasedHardBound: return "released to native (no new evidence within the hard bound)";
			case LatchEvent::ReleasedCeiling: return "released to native (no new evidence and no cut within the ceiling; crossfade)";
			default: return "";
			}
		}

		inline bool LatchEventIsRelease(LatchEvent e) { return e >= LatchEvent::Released; }

		// A release for lack of new evidence: the window it was built on is discredited (the estimator clears the evidence).
		inline bool LatchEventIsEvidenceRelease(LatchEvent e)
		{
			return e == LatchEvent::Released || e == LatchEvent::ReleasedHardBound || e == LatchEvent::ReleasedCeiling;
		}

		struct LatchInput
		{
			int64_t timeUs = 0;           // source time of this evaluation (a backward step re-anchors, L5)
			double c1 = 15.0;
			bool informative = false;     // some axis earned evidence this evaluation (an uninformative one slows the release clock)
			bool menuHold = false;        // HOLD evaluation on an axis, or a menu / overlay episode event: R7
			bool abrStep = false;         // an ABR step event (used only with abrStepTrigger)
			bool nativeBoth = false;      // both axis readers are confident NATIVE
			bool shotBoundary = false;    // N1: a cut, black or fade was consumed with this evaluation (a deferred release's moment)
			// The confident UPSCALED pair of this evaluation (both axes), with each axis' evidence incl. the two-axis prior.
			bool havePair = false;
			int nx = 0, Nx = 0, ny = 0, Ny = 0;
			double llrX = 0.0, llrY = 0.0;
			// The delivered raster (R2 pair check on the raster the ratios belong to); 0 = unknown: no pair is conventional.
			int rasterW = 0, rasterH = 0;
			// M1: the evidence of the CURRENTLY latched size CREDITED by this evaluation on each axis (ChainEvidence::CreditAtN: the change of
			// the shot-capped windowed total), the positive raw increments behind it (before the shot cap: a capped increment still says the
			// picture agrees) and the source time of the newest positive credit (-1: none). Only read while latched.
			double gainX = 0.0, gainY = 0.0;
			double rawX = 0.0, rawY = 0.0;
			int64_t gainTimeX = -1, gainTimeY = -1;
		};

		struct LatchOutput
		{
			bool latched = false;         // an upscaled latch is held after this evaluation
			int nx = 0, ny = 0, Nx = 0, Ny = 0;   // the latched sizes (valid while latched)
			LatchEvent event = LatchEvent::None;
			bool confirmed = false;       // the latch was re-confirmed by new evidence this evaluation
			bool deferredNow = false;     // N1: a starvation bound was reached this evaluation and its release deferred to the next boundary
			bool deferCancelled = false;  // N1: a deferred release was cancelled by a re-confirmation this evaluation
			bool deferred = false;        // N1: a deferred release is pending after this evaluation (the latch is still held)
			bool heldBack = false;        // a confident upscaled pair was NOT latched (bar not met, or not conventional)
			bool nonConventional = false; // ... because the pair is not one published geometry
			uint32_t reasonBits = 0;      // Chain::kReason* for the status / log
		};

		struct LatchCounters
		{
			uint64_t entries = 0, moves = 0;
			uint64_t confirms = 0;                   // re-confirmations by new evidence (M1)
			uint64_t releases = 0;                   // all releases (any cause)
			uint64_t releasesBound = 0, releasesNative = 0, releasesMenu = 0, releasesAbr = 0, releasesReset = 0;
			uint64_t releasesHardBound = 0;          // R5b hard bound (no new evidence for hardBoundSec); not in releasesBound
			// N1: releasesBound / releasesHardBound count the bound being REACHED; with deferral the release itself follows (or not):
			uint64_t deferred = 0;                   // bounds reached whose release was deferred to the next shot boundary
			uint64_t deferCancelled = 0;             // ... cancelled by a re-confirmation (no release)
			uint64_t releasesAtCut = 0;              // ... released at the end of the new shot's dwell (in releases)
			uint64_t releasesCeiling = 0;            // ... released by the no-boundary ceiling (in releases; the monitor crossfades)
			uint64_t heldBack = 0;                   // evaluations where a confident upscaled pair was held back by the bar
			uint64_t nonConventional = 0;            // evaluations where the pair was not a published geometry
		};

		class UpscaleLatch
		{
		public:
			UpscaleLatch() {}
			explicit UpscaleLatch(const LatchSettings& s) : s_(s) {}

			const LatchSettings& Settings() const { return s_; }
			const LatchCounters& Counters() const { return c_; }
			bool Latched() const { return latched_; }
			int LatchedNx() const { return lnx_; }
			int LatchedNy() const { return lny_; }
			double ReleaseClockSec() const { return clock_; }
			bool Deferred() const { return deferred_; }
			bool Dwelling() const { return deferred_ && dwelling_; }   // N1: the new shot's re-confirm dwell is open

			// The bar for entering / moving at source time t: c1 x (1 + growth over the stable time).
			double Bar(int64_t timeUs, double c1) const
			{
				const double stable = started_ ? static_cast<double>(timeUs - stableSinceUs_) / 1e6 : 0.0;
				const double g = s_.growSec > 0.0 ? (stable - s_.graceSec) / s_.growSec : 0.0;
				return c1 * (1.0 + (std::min)(s_.growMax, (std::max)(0.0, g)));
			}

			// A discontinuity / active-area change: the latch goes (counted), the stable clock restarts at the next evaluation.
			LatchOutput Reset()
			{
				LatchOutput o;
				if (!s_.enabled)
					return o;
				if (latched_)
				{
					o.event = Release(LatchEvent::ReleasedReset);
					o.reasonBits = kReasonLatchRelease;
				}
				started_ = false;
				havePrev_ = false;
				Fill(o);
				return o;
			}

			LatchOutput Step(const LatchInput& in)
			{
				LatchOutput o;
				if (!s_.enabled)
					return o;
				const int64_t t = in.timeUs;
				if (!started_)
				{
					started_ = true;
					stableSinceUs_ = t;
				}
				if (havePrev_ && t < prevUs_)
				{
					// L5: a backward source-time step without a reset. Re-anchor, so neither the bar's stable time nor the hard bound freezes
					// on a negative age; the release clock keeps its value.
					stableSinceUs_ = t;
					if (latched_)
						lastConfUs_ = t;
					newestX_ = newestY_ = -1;
					if (deferred_)
					{
						deferUs_ = t;   // N1: the ceiling and the dwell run from the re-anchored time too
						dwellStartUs_ = t;
					}
				}
				const double dt = havePrev_ && t > prevUs_ ? static_cast<double>(t - prevUs_) / 1e6 : 0.0;
				prevUs_ = t;
				havePrev_ = true;

				if (in.menuHold)
				{
					// R7: not carried into a menu, and nothing enters during one.
					if (latched_)
					{
						o.event = Release(LatchEvent::ReleasedMenu);
						o.reasonBits |= kReasonLatchRelease;
					}
					stableSinceUs_ = t;
					o.reasonBits |= kReasonMenuHold;
					Fill(o);
					return o;
				}
				if (latched_ && s_.abrStepTrigger && in.abrStep)
				{
					o.event = Release(LatchEvent::ReleasedAbr);
					o.reasonBits |= kReasonAbrStep | kReasonLatchRelease;
				}
				else if (latched_ && in.nativeBoth)
				{
					o.event = Release(LatchEvent::ReleasedNative);
					o.reasonBits |= kReasonLatchRelease;
				}

				// A listed pair that needs a single-axis re-base alone (1440x1080 anamorphic) is parked: listed, never latched here.
				const bool conventional = in.havePair && Scaling::GeometrySet::AgreeingPair(in.nx, in.Nx, in.ny, in.Ny, in.rasterW, in.rasterH);
				const double bar = Bar(t, in.c1);
				const bool barMet = in.havePair && (std::min)(in.llrX, in.llrY) >= bar;

				if (!latched_)
				{
					if (o.event == LatchEvent::None && in.havePair)
					{
						if (!conventional)
						{
							++c_.nonConventional;
							o.nonConventional = true;
							o.heldBack = true;
							o.reasonBits |= kReasonNonConventional | kReasonLatchBar;
						}
						else if (barMet)
						{
							Latch(in);
							o.event = LatchEvent::Entered;
							++c_.entries;
							stableSinceUs_ = t;
							Anchor(t);
						}
						else
						{
							++c_.heldBack;
							o.heldBack = true;
							o.reasonBits |= kReasonLatchBar;
						}
					}
				}
				else
				{
					const double need = s_.confirmFraction * in.c1;
					// M1: only NEW evidence of the latched size counts, net since the last confirmation (a debt floored at -need).
					accX_ = (std::max)(-need, accX_ + in.gainX);
					accY_ = (std::max)(-need, accY_ + in.gainY);
					if (in.gainX > 0.0 && in.gainTimeX > newestX_)
						newestX_ = in.gainTimeX;
					if (in.gainY > 0.0 && in.gainTimeY > newestY_)
						newestY_ = in.gainTimeY;
					if (accX_ >= need && accY_ >= need)
					{
						// The hard bound runs from the OLDER of the two axes' newest confirming evidence (never later than now).
						int64_t ev = (std::min)(newestX_, newestY_);
						if (ev < 0 || ev > t)
							ev = t;
						lastConfUs_ = (std::max)(lastConfUs_, ev);
						accX_ = accY_ = 0.0;
						newestX_ = newestY_ = -1;
						clock_ = 0.0;
						++c_.confirms;
						o.confirmed = true;
						if (deferred_)
						{
							// N1: re-confirmed while a starvation release waited: cancelled (the picture never changed).
							ClearDeferral();
							++c_.deferCancelled;
							o.deferCancelled = true;
						}
					}
					else
					{
						const bool consistent = in.rawX > 0.0 || in.rawY > 0.0;   // the latched size was seen again, capped (not independent)
						clock_ += dt * ((in.informative && !consistent) ? 1.0 : s_.absentClockRate);
					}
					const double lrx = static_cast<double>(lnx_) / lNx_, lry = static_cast<double>(lny_) / lNy_;
					const bool moved = in.havePair && conventional && barMet &&
						(std::fabs(static_cast<double>(in.nx) / in.Nx - lrx) > s_.movedTolerance * lrx
						|| std::fabs(static_cast<double>(in.ny) / in.Ny - lry) > s_.movedTolerance * lry);
					if (moved)
					{
						Latch(in);
						o.event = LatchEvent::Moved;
						++c_.moves;
						stableSinceUs_ = t;
						Anchor(t);
						ClearDeferral();   // a move is a new latch on fresh evidence above the bar
					}
					else if (deferred_)
						StepDeferred(in, t, o);
					else if (static_cast<double>(t - lastConfUs_) / 1e6 > s_.hardBoundSec)
						Starved(LatchEvent::ReleasedHardBound, in, t, o);
					else if (clock_ > s_.releaseBoundSec)
						Starved(LatchEvent::Released, in, t, o);
				}
				Fill(o);
				return o;
			}

		private:
			// A starvation bound reached: counted at once; released at once (pre-N1 rule) or deferred to the next shot boundary (N1).
			void Starved(LatchEvent cause, const LatchInput& in, int64_t t, LatchOutput& o)
			{
				if (cause == LatchEvent::ReleasedHardBound)
					++c_.releasesHardBound;
				else
					++c_.releasesBound;
				if (!s_.deferStarvation)
				{
					o.event = Release(cause, false);
					o.reasonBits |= kReasonLatchRelease;
					return;
				}
				deferred_ = true;
				deferCause_ = cause;
				deferUs_ = t;
				dwelling_ = false;
				++c_.deferred;
				o.deferredNow = true;
				StepDeferred(in, t, o);   // a boundary consumed with this very evaluation opens the dwell now
			}

			void StepDeferred(const LatchInput& in, int64_t t, LatchOutput& o)
			{
				if (!dwelling_ && in.shotBoundary)
				{
					// The first evaluation of the new shot (the same reference ChainEvidence's shot dwell starts from).
					dwelling_ = true;
					dwellStartUs_ = t;
				}
				if (dwelling_)
				{
					if (static_cast<double>(t - dwellStartUs_) / 1e6 >= s_.deferDwellSec)
					{
						++c_.releasesAtCut;
						o.event = Release(deferCause_, false);
						o.reasonBits |= kReasonLatchRelease;
					}
				}
				else if (static_cast<double>(t - deferUs_) / 1e6 >= s_.deferCeilingSec)
				{
					++c_.releasesCeiling;
					o.event = Release(LatchEvent::ReleasedCeiling, false);
					o.reasonBits |= kReasonLatchRelease;
				}
			}

			void ClearDeferral()
			{
				deferred_ = false;
				dwelling_ = false;
			}

			void Latch(const LatchInput& in)
			{
				latched_ = true;
				lnx_ = in.nx;
				lNx_ = in.Nx;
				lny_ = in.ny;
				lNy_ = in.Ny;
			}

			void Anchor(int64_t t)
			{
				lastConfUs_ = t;
				clock_ = 0.0;
				accX_ = accY_ = 0.0;
				newestX_ = newestY_ = -1;
			}

			// `countCause` false: a starvation release whose bound was already counted when it was reached (Starved).
			LatchEvent Release(LatchEvent cause, bool countCause = true)
			{
				latched_ = false;
				clock_ = 0.0;
				accX_ = accY_ = 0.0;
				newestX_ = newestY_ = -1;
				ClearDeferral();   // N1: a native / menu / ABR / reset release while deferred is immediate and ends the deferral
				++c_.releases;
				switch (countCause ? cause : LatchEvent::None)
				{
				case LatchEvent::ReleasedNative: ++c_.releasesNative; break;
				case LatchEvent::ReleasedMenu: ++c_.releasesMenu; break;
				case LatchEvent::ReleasedAbr: ++c_.releasesAbr; break;
				case LatchEvent::ReleasedReset: ++c_.releasesReset; break;
				default: break;
				}
				stableSinceUs_ = prevUs_;   // the bar restarts after a release
				return cause;
			}

			void Fill(LatchOutput& o) const
			{
				o.latched = latched_;
				o.nx = lnx_;
				o.ny = lny_;
				o.Nx = lNx_;
				o.Ny = lNy_;
				o.deferred = deferred_;
			}

			LatchSettings s_;
			LatchCounters c_;
			bool latched_ = false;
			bool started_ = false;
			bool havePrev_ = false;
			int lnx_ = 0, lNx_ = 1, lny_ = 0, lNy_ = 1;
			int64_t prevUs_ = 0, stableSinceUs_ = 0, lastConfUs_ = 0;
			double clock_ = 0.0;
			double accX_ = 0.0, accY_ = 0.0;          // M1: net new evidence of the latched size since the last confirmation
			int64_t newestX_ = -1, newestY_ = -1;     // M1: source time of the newest positive credit since then
			bool deferred_ = false, dwelling_ = false;   // N1: a starvation release waits for a boundary / the new shot's dwell is open
			LatchEvent deferCause_ = LatchEvent::None;
			int64_t deferUs_ = 0, dwellStartUs_ = 0;
		};
	}
}
