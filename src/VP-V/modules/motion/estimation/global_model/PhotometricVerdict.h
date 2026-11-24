// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Global camera model: the verdict on a photometric refinement (plan lead20 W3, CE-WATER-C3).
// The fit calls PhotometricVerdict() ONCE at the point where the R3 result is applied to the chosen model.
// Evaluates pixel-domain brightness-constancy and geometric agreement with the vector fit using derived
// contract constants (zero invented magic numbers). Pure C++14, no Windows headers.
#pragma once

#include "CameraTestSeams.h"
#include "PhotometricRefine.h"

namespace CameraModel
{
	// Derived photometric-disagreement verdict constants (Rule 1):
	//  * kPhotometricDisagreeSigmas = 3.0: matches the fitter's 3-sigma agreement contract
	//    (Config::absThresholdSigmas = 3.0). A photometric correction exceeding 3 * precisionPx
	//    disagrees with the vector fit beyond 3-sigma vector noise.
	//  * kPhotometricQuantFloorCodes2 = 0.5 * (kPhotometricDisagreeSigmas * RefineOptions().minScaleCodes)^2:
	//    derived directly from RefineOptions::minScaleCodes (default 0.5 in 8-bit codes) and the
	//    3-sigma contract — the quadratic Huber cost (0.5 * r^2) of a 3-sigma quantization-floor
	//    residual (1.125 codes^2 at default). Prevents an already-exact clean vector fit (sitting at
	//    ~0.1-0.3 codes^2) from false-triggering if a Gauss-Newton step wobbles by rounding noise on
	//    FellBackNoImprovement.
	//  * kPhotometricBrightnessFloorCodes2 = 0.5 * RefineOptions().minGradientCodes^2:
	//    derived directly from RefineOptions::minGradientCodes (default 4.0 in 8-bit codes) — the
	//    quadratic Huber cost (0.5 * r^2) of a pixel residual equal to the minimum accepted gradient
	//    magnitude (8.0 codes^2 at default; failing brightness constancy by at least one minimum-gradient step).
	constexpr double kPhotometricDisagreeSigmas = 3.0;
	constexpr double kPhotometricQuantFloorCodes2 =
		0.5 * (kPhotometricDisagreeSigmas * RefineOptions().minScaleCodes)
		* (kPhotometricDisagreeSigmas * RefineOptions().minScaleCodes);
	constexpr double kPhotometricBrightnessFloorCodes2 =
		0.5 * RefineOptions().minGradientCodes * RefineOptions().minGradientCodes;

	// Pure predicate: true when an attempted R3 refine indicates genuine pixel-domain disagreement
	// with the vector model (brightness-constancy failure or > 3-sigma geometric disagreement).
	inline bool PhotometricDisagreed(const RefineResult& refined, double precisionPx)
	{
		const bool attempted = refined.status == RefineStatus::Refined || RefineFellBack(refined.status);
		if (!attempted)
			return false;
		// (a) Brightness-constancy failure on FellBackNoImprovement: residual strictly worsened above the
		// 3-sigma quantization floor, OR failed to improve while remaining above the gradient-derived
		// brightness-constancy floor.
		if (refined.status == RefineStatus::FellBackNoImprovement)
		{
			const bool worsened = refined.residualAfter > refined.residualBefore
				&& refined.residualAfter > kPhotometricQuantFloorCodes2;
			const bool elevated = refined.residualAfter >= refined.residualBefore
				&& refined.residualAfter > kPhotometricBrightnessFloorCodes2;
			if (worsened || elevated)
				return true;
		}
		// (b) Strong geometric disagreement: candidate correction exceeds 3 * precisionPx (3-sigma contract).
		if (precisionPx > 0.0 && IsFiniteBits(precisionPx) && IsFiniteBits(refined.correctionPx)
			&& refined.correctionPx > kPhotometricDisagreeSigmas * precisionPx)
			return true;
		// (c) Basin departure or out-of-range photometric gain already refused by R3's own verification gate.
		if (refined.status == RefineStatus::FellBackTooFar || refined.status == RefineStatus::FellBackPhotometric)
			return true;
		return false;
	}

	// vectorModel: the vector fit's chosen model before the refine; refined: R3's answer; precisionPx: the pair's final vector
	// precision (source px); disagreedOut: optional flag set true when pixel-domain verification disagrees with the vector fit.
	// Returns the result the fit must apply (status Refined = use result.model).
	inline RefineResult PhotometricVerdict(const Params& vectorModel, const RefineResult& refined, double precisionPx,
		bool* disagreedOut = nullptr)
	{
		CameraTestSeams::NotePhotometricVerdictCall();   // test-only seam: unarmed = one relaxed load, zero behaviour change
		const bool disagreed = PhotometricDisagreed(refined, precisionPx);
		if (disagreedOut != nullptr)
			*disagreedOut = disagreed;
		if (!disagreed)
			return refined;
		RefineResult out = refined;
		if (out.status == RefineStatus::Refined)
		{
			// Refined warp departed beyond 3 * precisionPx from the vector fit: refuse to apply the
			// discordant warp and restore the vector model visibly as FellBackTooFar.
			out.status = RefineStatus::FellBackTooFar;
			out.model = vectorModel;
			out.model.type = vectorModel.type;
			out.gain = 1.0;
			out.offset = 0.0;
		}
		return out;
	}
}
