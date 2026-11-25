// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// output.dither / dither need: the HOST SEAM (setting dither_need = off | auto, default off).
//
// DitherNeed.h is the law and the temporal state; DitherNeedGpu.h the GPU block pass. This header is the per-frame decision
// the output-finish segment takes before the sigma-delta runs, from the shared products only (rule 25: nothing is recomputed,
// no private frame history):
//
//   P12 light level   LightLevel::Lut (live fill via the Entrance seam, G6; absent today = visible fallback)
//   P11 region stats  RegionEvidence::View + GrainView (block statistics, banding, grain level)
//   P14 source quality  SourceIdentity::QualityView (the ONE read API; the tier and the quality 0..1 decide HOW MUCH extra
//                       amplitude a clean source needs: DitherNeed(quality) = 1 - quality scales the wanted amplitude)
//
// Every outcome that is not "on" is VISIBLE: the FallbackCatalog "dither need" entry (Ctrl+I line / Config status) with the
// named missing inputs, and counted (Counters). A product older than kMaxProductAgeSeconds counts as missing (kStaleProduct).
// Off, and the Original pathway, return at once: no product is read, no state is touched except the tracker reset, nothing
// is counted or reported (off == byte-identical; the sigma-delta's dither_need parameter stays 0, nothing is bound).
//
// LINK-NOISE limit: the extra dither is judged by its OWN limit, not by the +-5 % grain rule (owner design intent, doc 115
// items 12-13; tools/eval/link_noise.py): noise against plain + 4 at most kLinkNoiseLimitShadow in shadow blocks where the
// grain is already below the link step (sigmaOut < kLinkNoiseShadowSigmaOut) and kLinkNoiseLimitElsewhere elsewhere. The limit
// is a named constant here (the acceptance gate of the offline evaluation and of the owner's A/B), not a hidden literal.
//
// Pure CPU, header-only, C++14, deterministic (no clock: ages and dt are passed in; nothing is hard-coded to a rate or size).
#pragma once

#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/modules/input/source_analysis/identity/SourceQualityView.h>
#include <VP-V/modules/output/dither/DitherNeed.h>

#include <cstdint>
#include <string>

namespace DitherNeedSeam
{
	// ---- the LINK-NOISE limit (its own constants; doc 115 items 12-13) -------------------------------------------------
	constexpr double kLinkNoiseLimitShadow = 1.40;       // noise vs plain + 4, shadow blocks (sigmaOut < kLinkNoiseShadowSigmaOut)
	constexpr double kLinkNoiseLimitElsewhere = 1.05;    // noise vs plain + 4, every other block
	constexpr double kLinkNoiseShadowSigmaOut = 0.35;    // grain rms in link codes below which the "grain" is quantiser noise

	inline double LinkNoiseLimit(double sigmaOutCodes)
	{
		return sigmaOutCodes < kLinkNoiseShadowSigmaOut ? kLinkNoiseLimitShadow : kLinkNoiseLimitElsewhere;
	}
	inline bool LinkNoiseWithinLimit(double noiseRatio, double sigmaOutCodes)
	{
		return noiseRatio <= LinkNoiseLimit(sigmaOutCodes);
	}

	// A shared product older than this (source seconds, measured by the caller) is not trusted: it counts as missing.
	constexpr double kMaxProductAgeSeconds = 0.5;

	// What the host has this frame. Every pointer may be null (product absent). Ages are seconds since the product was
	// filled (0 = this frame); < 0 = unknown, treated as fresh.
	struct Feed
	{
		const RegionEvidence::GrainView* grain = nullptr;            // P11 grain level (CPU view)
		const RegionEvidence::View* evidence = nullptr;              // P11 region block statistics
		const LightLevel::Lut* lut = nullptr;                        // P12 light level
		const SourceIdentity::QualityView* quality = nullptr;        // P14 source quality (tier, quality)
		double grainAgeSeconds = 0.0, evidenceAgeSeconds = 0.0, lutAgeSeconds = 0.0;
		int colorBits = 8;                                           // the link's colour depth (SigmaDeltaDomain::Domain::color)
		bool domainSupported = true;                                 // the sigma-delta can serve this output domain
		double dtSeconds = 0.0;                                      // measured frame interval; <= 0 = unknown
		bool discontinuity = false;                                  // cut, resync, re-base, context change
	};

	struct Counters
	{
		uint64_t framesOn = 0, framesNotNeeded = 0, framesUnavailable = 0, framesStale = 0, framesClassicalQuality = 0;
	};

	struct Outcome
	{
		DitherNeed::Result result = DitherNeed::Result::Off;
		bool active = false;                  // the setting was on and the Original pathway was not
		unsigned missing = 0;                 // DitherNeed::Missing bits
		bool extraAllowed = false;            // result == On: the GPU block pass may run and be bound
		double qualityNeed = 1.0;             // 0..1: scales the wanted amplitude
		int tier = -1;
		DitherNeed::Params params;            // the law's parameters of this frame (aNeed scaled by qualityNeed)
		DitherNeed::Decision decision;        // the tracker's reset / off / status decision
		std::string status;                   // "" when not active; else the Ctrl+I / Config line
		FallbackCatalog::Decision fallback;   // engaged when the setting is on and an input is missing or stale
	};

	class Stage
	{
	public:
		Outcome Evaluate(bool requested, bool originalPathway, const Feed& f, const RegionEvidence::Grid& grid,
			const DitherNeed::Params& base = DitherNeed::Params())
		{
			Outcome o;
			o.fallback = FallbackCatalog::Released("dither need");
			if (!requested || originalPathway)
			{
				DitherNeed::FrameArgs off;
				off.enabled = false;
				tracker_.Decide(off);   // the next full frame resets e = target
				cache_.Reset();
				return o;
			}
			o.active = true;
			unsigned missing = 0;
			const bool stale =
				(f.lutAgeSeconds > kMaxProductAgeSeconds) || (f.evidenceAgeSeconds > kMaxProductAgeSeconds) ||
				(f.grainAgeSeconds > kMaxProductAgeSeconds);
			if (!f.lut) missing |= DitherNeed::kNoLut;
			if (!f.grain || !f.grain->CpuValid()) missing |= DitherNeed::kNoGrain;
			if (!f.evidence || !f.evidence->Valid()) missing |= DitherNeed::kNoPrecursors;
			if (!f.domainSupported) missing |= DitherNeed::kUnsupportedDomain;
			if (stale) missing |= DitherNeed::kStaleProduct;

			// Source quality decides how much of the wanted amplitude a source needs: a clean source needs none.
			bool classical = false;
			if (f.quality)
			{
				o.qualityNeed = SourceIdentity::DitherNeed(*f.quality);
				o.tier = f.quality->tier;
				classical = f.quality->fallback.engaged;
			}
			else
				classical = true;   // no view at all: the full designed amplitude, said in the status
			o.params = base;
			o.params.aNeed = base.aBase + (base.aNeed - base.aBase) * o.qualityNeed;

			const double q = DitherNeed::QOut(f.colorBits);
			bool needed = true;
			if (f.lut && !(missing & DitherNeed::kNoLut))
				needed = cache_.Get(*f.lut, q, o.params);
			if (o.qualityNeed < 1e-3) needed = false;   // a clean source: nothing to add

			DitherNeed::FrameArgs a;
			a.params = o.params;
			a.grid = grid;
			a.dtSeconds = f.dtSeconds;
			a.discontinuity = f.discontinuity;
			a.enabled = true;
			a.missing = missing;
			a.needed = needed;
			a.colorBits = f.colorBits;
			a.lutFingerprint = f.lut ? f.lut->fingerprint : std::string();
			o.decision = tracker_.Decide(a);
			o.result = o.decision.result;
			o.missing = o.decision.missing;
			o.extraAllowed = o.result == DitherNeed::Result::On;
			o.status = o.decision.status;
			if (classical && o.result == DitherNeed::Result::On)
			{
				o.status += " [" + std::string(SourceIdentity::ConsumerStatusText(SourceIdentity::QualityConsumer::DitherNeed)) + "]";
				++counters_.framesClassicalQuality;
			}
			o.fallback = FallbackCatalog::DitherNeed(true, DitherNeed::MissingText(o.missing));
			switch (o.result)
			{
			case DitherNeed::Result::On: ++counters_.framesOn; break;
			case DitherNeed::Result::NotNeeded: ++counters_.framesNotNeeded; break;
			case DitherNeed::Result::Unavailable:
				++counters_.framesUnavailable;
				if (missing & DitherNeed::kStaleProduct) ++counters_.framesStale;
				break;
			case DitherNeed::Result::Off: break;
			}
			return o;
		}

		const Counters& Count() const { return counters_; }
		void Reset() { tracker_.Invalidate(); cache_.Reset(); counters_ = Counters(); }

	private:
		DitherNeed::StateTracker tracker_;
		DitherNeed::NeedCache cache_;
		Counters counters_;
	};
}
