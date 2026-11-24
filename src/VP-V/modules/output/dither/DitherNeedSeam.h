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

#include <algorithm>
#include <cmath>
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

	// Owner Q9a (SD MUST-5): physics-derived quantization-error RMS ratio when a higher-bit-depth
	// source (e.g., 10-bit or 12-bit, including TD-cleaned streams) is delivered over a narrower link
	// (e.g., 8-bit). Uniform quantization noise variance of an n-bit link is Delta^2 / 12; a finer
	// (n+d)-bit source (d = sourceBits - colorBits) has quantization variance (Delta * 2^-d)^2 / 12
	// = 4^-d * (Delta^2 / 12). The additional quantization error variance introduced by truncating
	// (n+d) -> n is (1 - 4^-d) * (Delta^2 / 12), whose RMS amplitude ratio relative to a full link
	// quantization step is sqrt(1 - 4^-d) = sqrt(1 - 2^(-2d)) (sqrt(15/16) ~= 0.968246 for 10->8 bit,
	// sqrt(255/256) ~= 0.998045 for 12->8 bit, 0.0 for d <= 0). Derived strictly from quantization
	// error variance physics (Rule 1).
	inline double BitDepthFloor(int sourceBits, int colorBits)
	{
		if (sourceBits <= 0 || colorBits <= 0 || sourceBits <= colorBits)
			return 0.0;
		const int extraBits = sourceBits - colorBits;
		return std::sqrt(1.0 - std::ldexp(1.0, -2 * extraBits));
	}

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
		int sourceBits = 0;                                          // the source's colour depth (0 = unspecified / same as colorBits)
		bool domainSupported = true;                                 // the sigma-delta can serve this output domain
		double dtSeconds = 0.0;                                      // measured frame interval; <= 0 = unknown
		bool discontinuity = false;                                  // cut, resync, re-base, context change
	};

	struct Counters
	{
		uint64_t framesOn = 0, framesNotNeeded = 0, framesUnavailable = 0, framesStale = 0, framesClassicalQuality = 0,
			framesBitDepthFloor = 0;
	};

	struct Outcome
	{
		DitherNeed::Result result = DitherNeed::Result::Off;
		bool active = false;                  // the setting was on and the Original pathway was not
		unsigned missing = 0;                 // DitherNeed::Missing bits
		bool extraAllowed = false;            // result == On: the GPU block pass may run and be bound
		double qualityNeed = 1.0;             // 0..1: scales the wanted amplitude
		double bitDepthFloor = 0.0;           // Owner Q9a: quantization-loss floor for sourceBits > colorBits
		bool bitDepthFloorApplied = false;    // true when bitDepthFloor raised qualityNeed above raw source-quality need
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

			// Source quality decides how much of the wanted amplitude a source needs: a clean source needs none
			// on its own, except that a higher-bit-depth source on a narrower link retains its quantization-loss floor (Owner Q9a).
			bool classical = false;
			double rawQualityNeed = 1.0;
			if (f.quality)
			{
				rawQualityNeed = SourceIdentity::DitherNeed(*f.quality);
				o.tier = f.quality->tier;
				classical = f.quality->fallback.engaged;
			}
			else
				classical = true;   // no view at all: the full designed amplitude, said in the status
			o.bitDepthFloor = BitDepthFloor(f.sourceBits, f.colorBits);
			const bool floorDominates = o.bitDepthFloor > rawQualityNeed;
			o.qualityNeed = (std::max)(rawQualityNeed, o.bitDepthFloor);
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
			if (floorDominates && o.result == DitherNeed::Result::On)
			{
				o.bitDepthFloorApplied = true;
				++counters_.framesBitDepthFloor;
				o.status += " [bit-depth floor: " + std::to_string(f.sourceBits) + "->" + std::to_string(f.colorBits) + " bit]";
			}
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
