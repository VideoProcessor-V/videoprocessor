// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// UpscaleLatch (SID r2 safety rules, E0 rule R5 + R7, owner decisions 2026-11-07): the ASYMMETRIC latch over the two axis readings.
//
//   NATIVE is sticky: the stream starts there and nothing but a conventional, doubly confirmed upscaled PAIR leaves it.
//   An UPSCALED latch must earn its keep:
//     enter / move  both axes above the BAR = c1 x (1 + clamp((stable - graceSec) / growSec, 0, growMax)): a long stable stream raises the bar
//                   (a codec-grid fluke at 1/2 gets harder to believe the longer the picture stays put). The pair must be one published
//                   conventional geometry (scaling.geometry SourceGeometrySet.h, R2): anything else stays UNSURE, visibly (heldBack).
//                   Moving between upscaled sizes uses the same bar.
//     keep          re-confirmed: the evidence of the latched size on BOTH axes reaches confirmFraction x c1 (E0: 0.25). While it is not, a release
//                   clock runs (seconds of source time); it runs at absentClockRate (R5b: 1/4 speed) through evaluations that earned no
//                   evidence at all (a dark / flat / soft stretch says nothing against the latch), so a true upscale is not flickered off.
//     release       the clock passes releaseBoundSec, OR hardBoundSec of source time pass since the last confirmation (no lock without release),
//                   OR native evidence (the readers say native on both axes), OR a menu / overlay HOLD (R7: the latch is not carried into a
//                   menu: E0 found 4 menu reels latched 1920x1080 through a 7 s native menu), OR (a switch, default OFF) an ABR rendition step.
//                   Every release is visible (the output, the status counters, the one-time notice) and counted; after one the bar restarts.
// It reuses the existing inputs only (rules 24 / 25): ChainEvidence's per-axis evidence, the HOLD evaluations and the monitor's mixed-episode
// events (WindowClearPolicy), the AbrStepDetector's events. It adds no private measurement. Pure, no clock of its own, header-only, C++14.
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
			double confirmFraction = 0.25;      // of c1: the latched size's evidence on both axes that re-confirms it (E0)
			double releaseBoundSec = 60.0;      // release clock bound (E0: 30 s flickers, 60 s shows no release on true upscales)
			double absentClockRate = 0.25;      // R5b: the clock's speed through evaluations that earned no evidence
			double hardBoundSec = 120.0;        // R5b: source seconds since the last confirmation that always release
			double graceSec = 30.0;             // bar growth starts after this much stable time
			double growSec = 120.0;             // ... and reaches growMax over this much more
			double growMax = 1.0;               // the bar at most doubles
			bool abrStepTrigger = false;        // a switch, OFF: an ABR rendition step drops the latch (E0: only helps after a rendition change)
			double movedTolerance = 0.004;      // relative change of a ratio that is a move to another size
		};

		enum class LatchEvent : int
		{
			None = 0,
			Entered,        // native -> upscaled latch
			Moved,          // latched size -> another conventional size (same bar)
			Released,       // not re-confirmed within the bound (to native)
			ReleasedNative, // the readers see native on both axes
			ReleasedMenu,   // a menu / overlay HOLD
			ReleasedAbr,    // an ABR step (switch)
			ReleasedReset   // a discontinuity / active-area change
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
			default: return "";
			}
		}

		inline bool LatchEventIsRelease(LatchEvent e) { return e >= LatchEvent::Released; }

		struct LatchInput
		{
			int64_t timeUs = 0;           // source time of this evaluation, non-decreasing
			double c1 = 15.0;
			bool informative = false;     // some axis earned evidence this evaluation (an uninformative one slows the release clock)
			bool menuHold = false;        // HOLD evaluation on an axis, or a menu / overlay episode event: R7
			bool abrStep = false;         // an ABR step event (used only with abrStepTrigger)
			bool nativeBoth = false;      // both axis readers are confident NATIVE
			// The confident UPSCALED pair of this evaluation (both axes), with each axis' evidence incl. the two-axis prior.
			bool havePair = false;
			int nx = 0, Nx = 0, ny = 0, Ny = 0;
			double llrX = 0.0, llrY = 0.0;
			// The evidence of the CURRENTLY latched size on each axis (ChainEvidence::LlrAtN), only read while latched.
			double latchedLlrX = 0.0, latchedLlrY = 0.0;
		};

		struct LatchOutput
		{
			bool latched = false;         // an upscaled latch is held after this evaluation
			int nx = 0, ny = 0, Nx = 0, Ny = 0;   // the latched sizes (valid while latched)
			LatchEvent event = LatchEvent::None;
			bool heldBack = false;        // a confident upscaled pair was NOT latched (bar not met, or not conventional)
			bool nonConventional = false; // ... because the pair is not one published geometry
			uint32_t reasonBits = 0;      // Chain::kReason* for the status / log
		};

		struct LatchCounters
		{
			uint64_t entries = 0, moves = 0;
			uint64_t releases = 0;                   // all releases (any cause)
			uint64_t releasesBound = 0, releasesNative = 0, releasesMenu = 0, releasesAbr = 0, releasesReset = 0;
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

				bool conventional = false, parked = false;
				if (in.havePair)
					conventional = Scaling::GeometrySet::ConventionalPair(in.nx, in.Nx, in.ny, in.Ny, &parked) != nullptr && !parked;
				// A listed pair that needs a single-axis re-base alone (1440x1080 anamorphic) is parked: listed, never latched here.
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
							lastConfUs_ = t;
							clock_ = 0.0;
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
					const bool confirmed = in.latchedLlrX >= need && in.latchedLlrY >= need;
					if (confirmed)
					{
						lastConfUs_ = t;
						clock_ = 0.0;
					}
					else
						clock_ += dt * (in.informative ? 1.0 : s_.absentClockRate);
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
						lastConfUs_ = t;
						clock_ = 0.0;
					}
					else if (clock_ > s_.releaseBoundSec || static_cast<double>(t - lastConfUs_) / 1e6 > s_.hardBoundSec)
					{
						o.event = Release(LatchEvent::Released);
						o.reasonBits |= kReasonLatchRelease;
					}
				}
				Fill(o);
				return o;
			}

		private:
			void Latch(const LatchInput& in)
			{
				latched_ = true;
				lnx_ = in.nx;
				lNx_ = in.Nx;
				lny_ = in.ny;
				lNy_ = in.Ny;
			}

			LatchEvent Release(LatchEvent cause)
			{
				latched_ = false;
				clock_ = 0.0;
				++c_.releases;
				switch (cause)
				{
				case LatchEvent::Released: ++c_.releasesBound; break;
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
			}

			LatchSettings s_;
			LatchCounters c_;
			bool latched_ = false;
			bool started_ = false;
			bool havePrev_ = false;
			int lnx_ = 0, lNx_ = 1, lny_ = 0, lNy_ = 1;
			int64_t prevUs_ = 0, stableSinceUs_ = 0, lastConfUs_ = 0;
			double clock_ = 0.0;
		};
	}
}
