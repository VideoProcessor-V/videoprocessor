#pragma once
/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// colour.dm_curve helpers (CF-1 P4, plan-CF-dv-display-mapping P4). Header-only, C++14, pure apart from the
// libplacebo switch at the end (guarded like DvDolbyStyle's function table).
//
//   1. Contrast recovery for DV pictures follows the NEED, never a private estimate: the one P14 read API
//      (SourceIdentity::QualityView / ContrastRecoveryActive) decides whether the picture needs libplacebo's
//      local contrast recovery. A clean high-bitrate source gets 0 (get out of the way); anything else keeps the
//      configured value. No quality reading at all: the configured value stands and the status says so with the
//      existing "contrast recovery: classical quality" wording (a visible fallback, never silent).
//   2. The tone LUT size libplacebo bakes for the curve is CHOSEN FROM A MEASUREMENT: the linear interpolation of a
//      LUT of N samples against the analytic curve, evaluated on every 12-bit I code of the scene range, in 12-bit
//      output codes. The smallest candidate under kMaxErrorCodes wins; no passing candidate keeps the largest and
//      reports its measured error. Nothing is hard-coded: the candidates are a ladder, the budget is a code error.
//
// Dolby-style only (Original = Bill: libplacebo's own recovery and LUT size are untouched). Precision: all maths in
// double, no sub-12-bit intermediate; the error is reported in 12-bit codes because that is the display precision.
#include <VP-V/modules/colour/display_mapping/DvDolbyStyle.h>
#include <VP-V/modules/input/source_analysis/identity/SourceQualityView.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace DmCurve
{
	constexpr int kCodeBits = 12;                                      // the display precision the error is stated in
	constexpr double kCodeScale = (1 << kCodeBits) - 1;                // 4095: PQ 0..1 -> 12-bit code
	constexpr double kMaxErrorCodes = 0.25;                            // LUT interpolation budget (12-bit codes)

	// The LUT size ladder tried in ascending order (libplacebo's default is 256).
	inline const std::vector<size_t>& Candidates()
	{
		static const std::vector<size_t> c = { 256, 512, 1024, 2048, 4096 };
		return c;
	}

	// libplacebo bakes lut[i] = f(lutMin + (lutMax - lutMin) * i / (size - 1)) and the shader interpolates linearly.
	// Returns the largest |interpolated - analytic| over all 12-bit codes inside the scene range, in 12-bit codes.
	inline double LutInterpolationError(const DvDolbyStyle::Curve& c, size_t size)
	{
		if (c.identity || size < 2 || !(c.sMax > c.sMin))
			return 0.0;
		std::vector<double> lut(size);
		const double span = c.sMax - c.sMin;
		for (size_t i = 0; i < size; ++i)
			lut[i] = c.Map(c.sMin + span * static_cast<double>(i) / static_cast<double>(size - 1));
		const long first = static_cast<long>(std::ceil(c.sMin * kCodeScale));
		const long last = static_cast<long>(std::floor(c.sMax * kCodeScale));
		double worst = 0.0;
		for (long code = first; code <= last; ++code)
		{
			const double x = static_cast<double>(code) / kCodeScale;
			const double pos = (x - c.sMin) / span * static_cast<double>(size - 1);
			size_t i = static_cast<size_t>(pos);
			if (i >= size - 1) i = size - 2;
			const double f = pos - static_cast<double>(i);
			const double interpolated = lut[i] + (lut[i + 1] - lut[i]) * f;
			worst = (std::max)(worst, std::fabs(interpolated - c.Map(x)) * kCodeScale);
		}
		return worst;
	}

	struct LutChoice
	{
		size_t size = 0;           // 0 = nothing measured (identity curve or no scene range): leave libplacebo's size
		double errorCodes = 0.0;   // measured error at `size`
		bool withinBudget = true;  // false: even the largest candidate missed the budget (reported, never silent)
	};

	inline LutChoice ChooseLutSize(const DvDolbyStyle::Curve& c, double maxErrorCodes = kMaxErrorCodes)
	{
		LutChoice r;
		if (c.identity || !(c.sMax > c.sMin))
			return r;
		for (size_t size : Candidates())
		{
			r.size = size;
			r.errorCodes = LutInterpolationError(c, size);
			if (r.errorCodes <= maxErrorCodes)
				return r;
		}
		r.withinBudget = false;
		return r;
	}

	// ---- contrast recovery from the need --------------------------------------------------------------------------
	struct Recovery
	{
		double value = 0.0;           // the contrast_recovery to give libplacebo for this DV picture
		bool needed = true;           // the need product says the picture benefits
		std::string status;           // "" = nothing to report; else the visible classical-quality wording
	};

	// `configured` is the profile's contrast_recovery (libplacebo's own value). The need comes only from the view.
	inline Recovery RecoveryFromNeed(const SourceIdentity::QualityView& view, double configured)
	{
		Recovery r;
		r.status = SourceIdentity::ConsumerStatus(view, SourceIdentity::QualityConsumer::ContrastRecovery);
		r.needed = SourceIdentity::ContrastRecoveryActive(view);
		r.value = r.needed ? configured : 0.0;
		return r;
	}
}

#if defined(LIBPLACEBO_TONE_MAPPING_H_) && defined(LIBPLACEBO_SHADERS_COLORSPACE_H_)
namespace DmCurve
{
	// Applies the measured LUT size and the need-driven recovery to the shared colour-map parameters for DV pictures in
	// Dolby-style mode, and restores the configured values otherwise (same adopt-a-re-apply rule as DvDolbyStyle::ToneSwitch).
	class MapSwitch
	{
	public:
		void Apply(pl_color_map_params& params, bool active, size_t lutSize, bool setRecovery, double recovery)
		{
			if (!m_overridden || params.lut_size != m_appliedLut)
				m_configuredLut = params.lut_size;
			if (!m_overridden || params.contrast_recovery != m_appliedRecovery)
				m_configuredRecovery = params.contrast_recovery;
			if (active)
			{
				if (lutSize > 0)
					params.lut_size = static_cast<decltype(params.lut_size)>(lutSize);
				if (setRecovery)
					params.contrast_recovery = static_cast<decltype(params.contrast_recovery)>(recovery);
				m_appliedLut = params.lut_size;
				m_appliedRecovery = params.contrast_recovery;
				m_overridden = true;
			}
			else if (m_overridden)
			{
				params.lut_size = m_configuredLut;
				params.contrast_recovery = m_configuredRecovery;
				m_overridden = false;
			}
		}

		// The profile's own contrast_recovery: the stored one while our override is in place, else the live one.
		float ConfiguredRecovery(const pl_color_map_params& params) const
		{
			return m_overridden && params.contrast_recovery == m_appliedRecovery ? m_configuredRecovery : params.contrast_recovery;
		}

	private:
		decltype(pl_color_map_params::lut_size) m_configuredLut = 0, m_appliedLut = 0;
		decltype(pl_color_map_params::contrast_recovery) m_configuredRecovery = 0, m_appliedRecovery = 0;
		bool m_overridden = false;
	};
}
#endif
