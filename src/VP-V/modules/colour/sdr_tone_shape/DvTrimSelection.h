#pragma once
/*
 * Copyright(C) 2026 Victor McPherson
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
// Dolby Vision trims with CM v4.0 Level 8 (feat/dv-l8, 2026-10-24), behind dv_trims.
// Header-only and pure (C++14): the renderer, the tests and (later) the "display adaptation"
// module that will join the trims with Stage 13's CAM16 appearance step.
//
// NOT Dolby display management and NOT certified (the certified reference is LLDV from the
// Apple TV): the same non-certified approximation as DolbyVisionTrims.h (L2), applied in the
// same stage - in PQ relative to the display peak, AFTER libplacebo's tone mapping
// (PRE_OUTPUT, shaders/DV trims.glsl), following the tone-mapping target nits live.
//
// LEVEL ORDER: L8 when the DM carries usable L8 trims whose known targets cover the display
// target at least as well as L2's (bracketed beats one-sided, else the nearer target), else L2,
// else none. A level is never mixed with the other inside one selection.
//
// L8 TARGETS: an L8 block names a target_display_index, not a luminance. It is resolved from
//   1. an L10 block with that index (custom target display: target_max_pq), else
//   2. the predefined CM v4.0 target list. libdovi (dovi_tool, level10.rs) names the preset
//      ids that need no L10 - 1, 16, 18, 21, 27, 28, 37, 38, 42, 48, 49 - but publishes no
//      luminance for them, so their values are UNVERIFIED from public sources; only index 1
//      (the 100-nit BT.709 target Dolby's tools write by default) is built in. Other ids are
//      skipped and counted (status + "Fallbacks engaged" when nothing else applies).
// Selection and interpolation by target nits are exactly L2's (DolbyVisionTrims::Select):
// a trim at the display peak as sent; two bracketing targets interpolated linearly in PQ;
// BELOW EVERY TARGET THE LOWEST-TARGET TRIM IS USED UNCHANGED (e.g. a projector at ~48 nits
// with a 100-nit trim gets the 100-nit trim); above every target the highest-target trim
// fades to neutral at the mastering peak; at or above the mastering peak nothing applies.
//
// L8 FIELDS (12-bit codes, 2048 = neutral, decoded exactly as L2): slope, offset, power,
// chroma_weight, saturation_gain (ms_weight: Dolby's detail management, not used). The
// optional fields, each also neutral when absent:
//   target_mid_contrast  mc = code/4096 - 0.5: a mid-tone contrast bend on the trimmed PQ
//                        value v (0..1): v + mc (v - 1/2) 4 v (1 - v) - black and peak fixed,
//                        monotonic for |mc| <= 0.5;
//   clip_trim            ct = code/4096 - 0.5: a highlight bend v + ct u^2 (1 - u),
//                        u = (v - 1/2) / (1/2) on the top half - peak fixed, C1 at mid,
//                        monotonic for |ct| <= 0.5;
//   saturation vector    6 bytes (128 = neutral) for the R, Y, G, C, B, M hue centres:
//                        chroma gain 1 + (b - 128) / 128 x kSatVectorRange;
//   hue vector           6 bytes (128 = neutral): hue rotation (b - 128) / 128 x
//                        kHueVectorDegrees at each centre;
//   both interpolated linearly between the two nearest centres (60 degrees apart) of the
//   pixel's hue in the RGB hexagon of its chroma (L - Y); a rotation keeps the luminance.
// The meanings of mid contrast / clip trim / the vectors and their ranges are assumptions
// (no public Dolby definition); they are kept small and continuous, and are the first things
// to calibrate against the LLDV reference (docs/dv/DV-trims-certified-reference.md).
#include <VP-V/modules/colour/sdr_tone_shape/DolbyVisionTrims.h>
#include <VP-V/modules/input/signal/DvSignalLevels.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace DvTrimSelection
{
	using DolbyVisionTrims::Params;
	using DolbyVisionTrims::Signal;
	using Trim = DolbyVisionTunnelFrameMetadata::Trim;

	enum class Level { None, L2, L8 };

	inline const char* LevelName(Level level)
	{
		return level == Level::L8 ? "L8" : level == Level::L2 ? "L2" : "none";
	}

	constexpr double kSatVectorRange = 0.5;      // byte 0..255 -> gain 0.5..~1.5 (128 = 1)
	constexpr double kHueVectorDegrees = 30.0;   // byte 0..255 -> -30..~+30 degrees (128 = 0)
	constexpr int kHues = 6;                     // R, Y, G, C, B, M
	constexpr double kPi = 3.14159265358979323846;

	// The trim one selection applies: L2's five fields plus L8's optional ones.
	struct ExtParams
	{
		Params base;
		double midContrast = 0.0;
		double clipTrim = 0.0;
		double saturation[kHues] = { 1.0, 1.0, 1.0, 1.0, 1.0, 1.0 };
		double hueDegrees[kHues] = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
	};

	inline bool HueVectorNeutral(const ExtParams& p)
	{
		for (int k = 0; k < kHues; ++k)
			if (p.hueDegrees[k] != 0.0)
				return false;
		return true;
	}

	inline bool SatVectorNeutral(const ExtParams& p)
	{
		for (int k = 0; k < kHues; ++k)
			if (p.saturation[k] != 1.0)
				return false;
		return true;
	}

	inline bool ExtrasNeutral(const ExtParams& p)
	{
		return p.midContrast == 0.0 && p.clipTrim == 0.0 && HueVectorNeutral(p) && SatVectorNeutral(p);
	}

	inline bool Neutral(const ExtParams& p) { return DolbyVisionTrims::Neutral(p.base) && ExtrasNeutral(p); }

	// Every L8 code a transport can corrupt is a 12-bit field (the vectors are bytes).
	inline bool ValidL8(const Trim& t)
	{
		return t.level == 8 && t.slope <= 4095 && t.offset <= 4095 && t.power <= 4095 && t.chromaWeight <= 4095 &&
			t.saturationGain <= 4095 && (!t.hasMidContrastAndClip || (t.midContrast <= 4095 && t.clipTrim <= 4095));
	}

	inline ExtParams FromL2(const Trim& t)
	{
		ExtParams p;
		p.base = DolbyVisionTrims::Decode(t);
		return p;
	}

	inline ExtParams DecodeL8(const Trim& t)
	{
		ExtParams p;
		p.base = DolbyVisionTrims::Decode(t);   // the same five fields and scale as L2
		if (t.hasMidContrastAndClip)
		{
			p.midContrast = DolbyVisionTrims::Clamp((t.midContrast > 4095 ? 4095 : t.midContrast) / 4096.0 - 0.5, -0.5, 0.5);
			p.clipTrim = DolbyVisionTrims::Clamp((t.clipTrim > 4095 ? 4095 : t.clipTrim) / 4096.0 - 0.5, -0.5, 0.5);
		}
		for (int k = 0; k < kHues; ++k)
		{
			if (t.hasSaturationVector)
				p.saturation[k] = 1.0 + (static_cast<double>(t.saturationVector[k]) - 128.0) / 128.0 * kSatVectorRange;
			if (t.hasHueVector)
				p.hueDegrees[k] = (static_cast<double>(t.hueVector[k]) - 128.0) / 128.0 * kHueVectorDegrees;
		}
		return p;
	}

	inline ExtParams Lerp(const ExtParams& a, const ExtParams& b, double w)
	{
		ExtParams p;
		p.base = DolbyVisionTrims::Lerp(a.base, b.base, w);
		p.midContrast = a.midContrast + (b.midContrast - a.midContrast) * w;
		p.clipTrim = a.clipTrim + (b.clipTrim - a.clipTrim) * w;
		for (int k = 0; k < kHues; ++k)
		{
			p.saturation[k] = a.saturation[k] + (b.saturation[k] - a.saturation[k]) * w;
			p.hueDegrees[k] = a.hueDegrees[k] + (b.hueDegrees[k] - a.hueDegrees[k]) * w;
		}
		return p;
	}

	// The predefined CM v4.0 target displays VP assumes (12-bit PQ of the target peak; -1 =
	// unknown). Only index 1, the 100-nit default target, is built in (to verify on a live
	// packet: the formatter logs the first L8 block per title).
	inline int PredefinedTargetMaxPq(uint8_t index)
	{
		return index == 1 ? 2081 : -1;
	}

	// The 12-bit PQ peak an L8 block's target refers to (-1 = unknown); `from` names the source.
	inline int ResolveL8TargetPq(const DolbyVisionTunnelFrameMetadata& dm, uint8_t index, const char** from = nullptr)
	{
		if (const auto* t = DvSignalLevels::FindTarget(dm, index))
		{
			if (from) *from = "L10";
			return t->maxPq;
		}
		const int predefined = PredefinedTargetMaxPq(index);
		if (from) *from = predefined >= 0 ? "predefined" : "unknown";
		return predefined;
	}

	struct Candidate
	{
		int trimIndex = -1;         // dm.trims[] index
		int targetPq = 0;           // 12-bit PQ code of the target peak
		ExtParams params;
	};

	struct Selection
	{
		bool apply = false;          // a non-neutral trim was chosen
		Level level = Level::None;   // the level the trim came from
		ExtParams params;
		int lowerIndex = -1;         // dm.trims[] index at or below the display (-1 none)
		int upperIndex = -1;         // dm.trims[] index at or above the display
		double weight = 0.0;         // toward upper (0 = lower only)
		double displayPq = 0.0;      // 12-bit PQ code of the display peak
		int l8Present = 0;           // valid L8 blocks in the DM
		int l8Unresolved = 0;        // ... of which the target is unknown (skipped)
		int firstUnresolvedIndex = -1;
		bool l8CoveredWorse = false; // usable L8 present, but L2's targets cover the display better
		const char* reason = "";
	};

	// The usable trims of one level, in DM order.
	inline std::vector<Candidate> Candidates(const DolbyVisionTunnelFrameMetadata& dm, Level level,
		int* l8Present = nullptr, int* l8Unresolved = nullptr, int* firstUnresolved = nullptr)
	{
		std::vector<Candidate> out;
		for (int i = 0; i < dm.trimCount && i < DolbyVisionTunnelFrameMetadata::kMaxTrims; ++i)
		{
			const Trim& t = dm.trims[i];
			Candidate c;
			c.trimIndex = i;
			if (level == Level::L2)
			{
				if (!DolbyVisionTrims::Valid(t))
					continue;
				c.targetPq = t.targetMaxPq;
				c.params = FromL2(t);
			}
			else
			{
				if (!ValidL8(t))
					continue;
				if (l8Present) ++*l8Present;
				c.targetPq = ResolveL8TargetPq(dm, t.targetDisplayIndex);
				if (c.targetPq < 0)
				{
					if (l8Unresolved) ++*l8Unresolved;
					if (firstUnresolved && *firstUnresolved < 0) *firstUnresolved = t.targetDisplayIndex;
					continue;
				}
				c.params = DecodeL8(t);
			}
			out.push_back(c);
		}
		return out;
	}

	// How well a level's targets cover the display (12-bit PQ codes): 0 when some target is at or
	// below it and some at or above it (bracketed or exact), else the distance to the nearest.
	inline double Coverage(const std::vector<Candidate>& cands, double displayPq)
	{
		bool below = false, above = false;
		double nearest = 1e9;
		for (const Candidate& c : cands)
		{
			below = below || c.targetPq <= displayPq;
			above = above || c.targetPq >= displayPq;
			const double d = std::fabs(c.targetPq - displayPq);
			nearest = d < nearest ? d : nearest;
		}
		return below && above ? 0.0 : nearest;
	}

	// L2's rule over one level's candidates (identical to DolbyVisionTrims::Select for L2).
	inline void SelectAmong(const DolbyVisionTunnelFrameMetadata& dm, const std::vector<Candidate>& cands, Selection& s)
	{
		int lower = -1, upper = -1;   // indices into cands
		for (int k = 0; k < static_cast<int>(cands.size()); ++k)
		{
			const double target = cands[k].targetPq;
			if (target <= s.displayPq && (lower < 0 || target > cands[lower].targetPq))
				lower = k;
			if (target >= s.displayPq && (upper < 0 || target < cands[upper].targetPq))
				upper = k;
		}
		s.lowerIndex = lower >= 0 ? cands[lower].trimIndex : -1;
		s.upperIndex = upper >= 0 ? cands[upper].trimIndex : -1;
		const bool sourcePeak = dm.sourceMaxPq > dm.sourceMinPq && dm.sourceMaxPq <= 4095;
		if (sourcePeak && s.displayPq >= dm.sourceMaxPq)
		{
			s.lowerIndex = s.upperIndex = -1;
			s.reason = "display at or above the mastering peak (identity)";
			return;
		}
		if (lower >= 0 && upper >= 0)
		{
			const double lo = cands[lower].targetPq, hi = cands[upper].targetPq;
			s.weight = hi > lo ? (s.displayPq - lo) / (hi - lo) : 0.0;
			s.params = Lerp(cands[lower].params, cands[upper].params, s.weight);
			s.reason = lower == upper || hi <= lo ? "trim at the display peak" : "interpolated between two trims";
		}
		else if (upper >= 0)
		{
			s.params = cands[upper].params;
			s.reason = "display below every trim target: the lowest-target trim";
		}
		else
		{
			const double lo = cands[lower].targetPq;
			if (sourcePeak && dm.sourceMaxPq > lo)
			{
				s.weight = (s.displayPq - lo) / (dm.sourceMaxPq - lo);
				s.params = Lerp(cands[lower].params, ExtParams(), s.weight);
				s.reason = "above every trim target: fading to neutral at the mastering peak";
			}
			else
			{
				s.params = cands[lower].params;
				s.reason = "above every trim target: the highest-target trim";
			}
		}
		s.apply = !Neutral(s.params);
	}

	// The trim for a display peak in nits (the tone-mapping target, > 0): L8, else L2, else none.
	inline Selection Select(const DolbyVisionTunnelFrameMetadata& dm, double displayPeakNits)
	{
		Selection s;
		if (!dm.valid) { s.reason = "no DM"; return s; }
		if (!(displayPeakNits > 0.0)) { s.reason = "no display peak"; return s; }
		s.displayPq = DolbyVisionTrims::PqFromNits(displayPeakNits) * 4095.0;
		const std::vector<Candidate> l8 = Candidates(dm, Level::L8, &s.l8Present, &s.l8Unresolved, &s.firstUnresolvedIndex);
		const std::vector<Candidate> l2 = Candidates(dm, Level::L2);
		// Audit 2026-10-24: L8 only when its known targets cover the display target at least as
		// well as L2's (bracketed beats one-sided; else the nearer target in PQ). A stream with
		// L8 at 100 nits (other L8 targets unknown) and L2 at 100/600/1000 on a 600-1000-nit
		// target keeps the grader's L2 trims.
		if (!l8.empty() && (l2.empty() || Coverage(l8, s.displayPq) <= Coverage(l2, s.displayPq)))
		{
			s.level = Level::L8;
			SelectAmong(dm, l8, s);
			return s;
		}
		if (!l2.empty())
		{
			s.level = Level::L2;
			s.l8CoveredWorse = !l8.empty();
			SelectAmong(dm, l2, s);
			return s;
		}
		s.reason = s.l8Unresolved ? "no L2 trim; L8 targets unknown" : "no L2/L8 trim";
		return s;
	}

	// ------------------------------------------------------------------ the pixel

	// v + mc (v - 1/2) 4 v (1 - v) on 0..1 (outside: unchanged).
	inline double MidContrast(double v, double mc)
	{
		if (mc == 0.0 || !(v > 0.0) || !(v < 1.0))
			return v;
		return v + mc * (v - 0.5) * 4.0 * v * (1.0 - v);
	}

	// v + ct u^2 (1 - u), u = (v - 1/2) / (1/2) clamped to 0..1.
	inline double ClipTrim(double v, double ct)
	{
		if (ct == 0.0)
			return v;
		const double u = DolbyVisionTrims::Clamp((v - 0.5) / 0.5, 0.0, 1.0);
		return v + ct * u * u * (1.0 - u);
	}

	// L2's curve, then (L8) the mid-contrast and clip bends on the in-range part.
	inline double Curve(double t, const ExtParams& p, double fadeT)
	{
		const double inside = t < 1.0 ? t : 1.0;
		const double fade = fadeT > 0.0 ? DolbyVisionTrims::Clamp(inside / fadeT, 0.0, 1.0) : 1.0;
		double v = inside * p.base.slope + p.base.offset * fade;
		v = DolbyVisionTrims::Shoulder(v < 0.0 ? 0.0 : v, p.base.slope + p.base.offset);
		v = std::pow(DolbyVisionTrims::Clamp(v, 0.0, 1.0), p.base.power);
		v = ClipTrim(MidContrast(v, p.midContrast), p.clipTrim);
		return t > 1.0 ? v + (t - 1.0) : v;
	}

	// The hue-centre weights of a chroma vector (L - Y): hue in the RGB hexagon, R = 0,
	// Y = 60, G = 120, C = 180, B = 240, M = 300 degrees. Returns false for no chroma.
	inline bool HueOf(const double C[3], double& x, double& y, double& degrees)
	{
		x = C[0] - 0.5 * (C[1] + C[2]);
		y = 0.86602540378443864676 * (C[1] - C[2]);
		if (x == 0.0 && y == 0.0)
			return false;
		degrees = std::atan2(y, x) * 180.0 / kPi;
		if (degrees < 0.0)
			degrees += 360.0;
		return true;
	}

	inline double HueInterp(const double v[kHues], double degrees)
	{
		const double pos = degrees / 60.0;
		const double fl = std::floor(pos);
		const int i0 = static_cast<int>(fl) % kHues;
		const int i1 = (i0 + 1) % kHues;
		const double f = pos - fl;
		return v[i0] * (1.0 - f) + v[i1] * f;
	}

	// One pixel of the output signal (r, g, b in place). Neutral L8 extras: exactly
	// DolbyVisionTrims::ApplyPixel (the L2 path is unchanged, bit for bit).
	inline void ApplyPixel(double rgb[3], const ExtParams& p, const Signal& s)
	{
		if (ExtrasNeutral(p))
		{
			DolbyVisionTrims::ApplyPixel(rgb, p.base, s);
			return;
		}
		double L[3];
		for (int c = 0; c < 3; ++c)
			L[c] = DolbyVisionTrims::ToNits(rgb[c], s);
		const double Y = s.kr * L[0] + s.kg * L[1] + s.kb * L[2];
		if (!(Y > 0.0))
			return;
		const double peakPq = DolbyVisionTrims::PqFromNits(s.peakNits);
		const double t = DolbyVisionTrims::PqFromNits(Y) / peakPq;
		const double t2 = Curve(t, p, DolbyVisionTrims::kOffsetFadePq / peakPq);
		const double Y2 = DolbyVisionTrims::NitsFromPq(t2 * peakPq);
		const double r = Y2 / Y;
		// Chroma, its hue, the per-hue saturation and rotation (luminance kept).
		double C[3] = { L[0] - Y, L[1] - Y, L[2] - Y };
		double hueSat = 1.0;
		const bool rotate = !HueVectorNeutral(p);
		bool rotated = false;
		double x = 0.0, y = 0.0, degrees = 0.0;
		if ((rotate || !SatVectorNeutral(p)) && HueOf(C, x, y, degrees))
		{
			if (!SatVectorNeutral(p))
				hueSat = HueInterp(p.saturation, degrees);
			const double turn = rotate ? HueInterp(p.hueDegrees, degrees) : 0.0;
			if (turn != 0.0)
			{
				const double a = turn * kPi / 180.0, ca = std::cos(a), sa = std::sin(a);
				const double x2 = x * ca - y * sa, y2 = x * sa + y * ca;
				const double m = (C[0] + C[1] + C[2]) / 3.0;
				C[0] = m + 2.0 / 3.0 * x2;
				C[1] = m - x2 / 3.0 + y2 / 1.73205080756887729353;
				C[2] = m - x2 / 3.0 - y2 / 1.73205080756887729353;
				const double d = s.kr * C[0] + s.kg * C[1] + s.kb * C[2];
				for (int c = 0; c < 3; ++c)
					C[c] -= d;
				rotated = true;
			}
		}
		const double sat = DolbyVisionTrims::Clamp(p.base.saturationGain * hueSat *
			(p.base.chromaWeight == 0.0 ? 1.0 : std::pow(r > 1e-6 ? r : 1e-6, p.base.chromaWeight)), 0.0, 2.0);
		const double yNear = DolbyVisionTrims::NitsFromPq(DolbyVisionTrims::kNearBlackPq);
		const double w = DolbyVisionTrims::Clamp((Y - yNear) / yNear, 0.0, 1.0);
		const double additive = 1.0 + (sat - 1.0) * DolbyVisionTrims::Clamp(Y / yNear, 0.0, 1.0);
		const double multiplicative = sat * (r < DolbyVisionTrims::kMaxChromaRatio ? r : DolbyVisionTrims::kMaxChromaRatio);
		double gain = additive + (multiplicative - additive) * w;
		// Gamut: no channel at or above black is pushed below it. Unrotated: as L2 (a gain
		// above 1 is capped, never below 1). Rotated: the rotation itself may need a gain
		// below 1, so the cap applies at any gain.
		for (int c = 0; c < 3; ++c)
			if (L[c] >= 0.0 && C[c] < 0.0)
			{
				const double limit = Y2 / -C[c];
				if (rotated)
					gain = gain < limit ? gain : limit;
				else if (gain > 1.0)
					gain = gain < limit ? gain : (limit > 1.0 ? limit : 1.0);
			}
		for (int c = 0; c < 3; ++c)
			rgb[c] = DolbyVisionTrims::FromNits(Y2 + gain * C[c], s);
	}

	// ------------------------------------------------------------------ status and shader

	// "DV trims: L8" / "DV trims: L2" / "DV trims: none" (the Ctrl+I / status words).
	inline std::string StatusText(bool enabled, const Selection& s)
	{
		if (!enabled)
			return "DV trims: off";
		std::string text = std::string("DV trims: ") + (s.apply ? LevelName(s.level) : "none");
		if (s.l8CoveredWorse)
			text += " (L2 targets cover the display better than the known L8 targets)";
		if (s.l8Unresolved)
		{
			char extra[96];
			std::snprintf(extra, sizeof(extra), " (%d L8 target%s unknown, e.g. index %d)", s.l8Unresolved,
				s.l8Unresolved == 1 ? "" : "s", s.firstUnresolvedIndex);
			text += extra;
		}
		return text;
	}

	inline std::string Describe(const Selection& s)
	{
		char text[512];
		std::snprintf(text, sizeof(text), "%s, %s (display %.0f PQ code, slope %.3f offset %.3f power %.3f chroma_weight %.3f saturation %.3f"
			" mid_contrast %.3f clip_trim %.3f sat_vector %.2f/%.2f/%.2f/%.2f/%.2f/%.2f hue_vector %.1f/%.1f/%.1f/%.1f/%.1f/%.1f)",
			LevelName(s.level), s.reason, s.displayPq, s.params.base.slope, s.params.base.offset, s.params.base.power,
			s.params.base.chromaWeight, s.params.base.saturationGain, s.params.midContrast, s.params.clipTrim,
			s.params.saturation[0], s.params.saturation[1], s.params.saturation[2], s.params.saturation[3],
			s.params.saturation[4], s.params.saturation[5], s.params.hueDegrees[0], s.params.hueDegrees[1],
			s.params.hueDegrees[2], s.params.hueDegrees[3], s.params.hueDegrees[4], s.params.hueDegrees[5]);
		return text;
	}

	// The trim's //!PARAM values for shaders/DV trims.glsl (the signal ones - dv_signal_pq,
	// dv_gamma, dv_peak_nits, dv_kr, dv_kb - are set by the renderer). dv_ext = 0 keeps the
	// shader on the L2 arithmetic exactly.
	inline std::vector<std::pair<const char*, double>> ShaderParameters(const ExtParams& p)
	{
		static const char* const sat[kHues] = { "dv_sat_r", "dv_sat_y", "dv_sat_g", "dv_sat_c", "dv_sat_b", "dv_sat_m" };
		static const char* const hue[kHues] = { "dv_hue_r", "dv_hue_y", "dv_hue_g", "dv_hue_c", "dv_hue_b", "dv_hue_m" };
		std::vector<std::pair<const char*, double>> out = {
			{ "dv_slope", p.base.slope }, { "dv_offset", p.base.offset }, { "dv_power", p.base.power },
			{ "dv_chroma_weight", p.base.chromaWeight }, { "dv_sat_gain", p.base.saturationGain },
			{ "dv_ext", ExtrasNeutral(p) ? 0.0 : 1.0 }, { "dv_rotate", HueVectorNeutral(p) ? 0.0 : 1.0 },
			{ "dv_mid_contrast", p.midContrast }, { "dv_clip_trim", p.clipTrim } };
		for (int k = 0; k < kHues; ++k)
			out.emplace_back(sat[k], p.saturation[k]);
		for (int k = 0; k < kHues; ++k)
			out.emplace_back(hue[k], p.hueDegrees[k]);
		return out;
	}
}
