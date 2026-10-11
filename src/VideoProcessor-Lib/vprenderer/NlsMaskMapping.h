#pragma once

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <string>

// Stage 6c-2: the NLS / NLS+ coordinate map, for the occlusion mask.
//
// The NLS shaders (shaders/NLS.glsl, shaders/NLSPlus.glsl) sample the source
// at a non-linearly mapped position, per axis, before the scaler. The mask
// hook must follow the same map from an output pixel back to the source
// block it shows. Both shaders use one per-axis curve; classic NLS is the
// one-axis case (the other axis has centre scale 1, which is the identity).
// The renderer's NLS binding log uses CenterScales too, so the two cannot
// drift apart.
namespace NlsMaskMapping
{
	struct Mapping
	{
		bool valid = false;
		double xScale = 1.0, yScale = 1.0;   // centre slopes of the inverse map
		double curve = 2.0;
		int geometry = 0;                     // 0 power curve, 1 protected centre
		double xProtect = 0.0, yProtect = 0.0;
	};

	inline bool Number(const std::map<std::string, std::string>& p, const char* key, double& value)
	{
		const auto found = p.find(key);
		if (found == p.end() || found->second.empty())
			return false;
		char* end = nullptr;
		const double parsed = std::strtod(found->second.c_str(), &end);
		if (end == found->second.c_str() || !std::isfinite(parsed))
			return false;
		value = parsed;
		return true;
	}

	// The centre scales the shader uses. plus: 1 = NLS+ (NLSPlus.glsl), 0 =
	// classic NLS, -1 = NLS+ when axis_balance is set (the binding log's rule).
	inline void CenterScales(const std::map<std::string, std::string>& p, double stretchRatio,
		bool verticalWarp, double& xScale, double& yScale,
		double* requestedBalance = nullptr, double* effectiveBalance = nullptr, double* maximumZoom = nullptr,
		int plus = -1)
	{
		double strength = 1.0;
		Number(p, "strength", strength);
		strength = std::max(0.0, std::min(1.0, strength));
		xScale = yScale = 1.0;
		double balance = 0.5, zoom = 1.08, requested = 0.0, effective = 0.0;
		if (plus == 1 || (plus < 0 && p.find("axis_balance") != p.end()))
		{
			Number(p, "axis_balance", balance);
			balance = std::max(0.0, std::min(1.0, balance));
			Number(p, "max_center_zoom", zoom);
			zoom = std::max(1.0, std::min(1.25, zoom));
			requested = balance * strength;
			effective = requested;
			if (stretchRatio > 1.000001)
				effective = std::min(effective, std::max(0.0, std::min(1.0, std::log(zoom) / std::log(stretchRatio))));
			const double q = verticalWarp ? 1.0 / stretchRatio : stretchRatio;
			xScale = std::pow(q, verticalWarp ? effective : 1.0 - effective);
			yScale = std::pow(q, verticalWarp ? effective - 1.0 : -effective);
		}
		else
		{
			const double selected = 1.0 + (stretchRatio - 1.0) * strength;
			(verticalWarp ? yScale : xScale) = selected;
		}
		if (requestedBalance) *requestedBalance = requested;
		if (effectiveBalance) *effectiveBalance = effective;
		if (maximumZoom) *maximumZoom = zoom;
	}

	// The whole map from the NLS rule's fixed parameters (after the shader
	// loader's defaults). nlsPlus: the rule's shader is NLSPlus.glsl. Invalid
	// (mask off) when a parameter the shader needs is missing.
	inline Mapping FromParameters(const std::map<std::string, std::string>& p, double stretchRatio,
		bool verticalWarp, bool nlsPlus)
	{
		Mapping m;
		double geometry = 0.0;
		if (!Number(p, "curve", m.curve) || !Number(p, "geometry", geometry))
			return m;
		m.curve = std::max(1.0, std::min(2.9, m.curve));
		m.geometry = geometry >= 0.5 ? 1 : 0;
		const bool plus = nlsPlus;
		double balance = 0.0;
		if (plus)
		{
			if (!Number(p, "axis_balance", balance) ||
				!Number(p, "horizontal_center_protection", m.xProtect) ||
				!Number(p, "vertical_center_protection", m.yProtect))
				return m;
		}
		else
		{
			double protect = 0.0;
			if (!Number(p, "center_protection", protect))
				return m;
			m.xProtect = m.yProtect = protect;
		}
		m.xProtect = std::max(0.0, std::min(0.45, m.xProtect));
		m.yProtect = std::max(0.0, std::min(0.45, m.yProtect));
		CenterScales(p, std::max(1.0, std::min(1.5, stretchRatio)), verticalWarp, m.xScale, m.yScale,
			nullptr, nullptr, nullptr, plus ? 1 : 0);
		m.valid = true;
		return m;
	}

	// CPU copy of the shaders' per-axis map (tests).
	inline double MapRadius(double radius, double scale, double curve, int geometry, double protect)
	{
		if (geometry == 0 || protect <= 0.0)
			return scale * radius - (scale - 1.0) * std::pow(radius, curve);
		if (radius <= protect)
			return radius * scale;
		const double span = 1.0 - protect;
		const double t = std::max(0.0, std::min(1.0, (radius - protect) / span));
		const double t2 = t * t, t3 = t2 * t, t4 = t3 * t, t5 = t4 * t;
		const double h00 = 1.0 - 10.0 * t3 + 15.0 * t4 - 6.0 * t5;
		const double h10 = t - 6.0 * t3 + 8.0 * t4 - 3.0 * t5;
		const double h01 = 10.0 * t3 - 15.0 * t4 + 6.0 * t5;
		const double h11 = -4.0 * t3 + 7.0 * t4 - 3.0 * t5;
		const double edgeSlope = std::max(0.20, 1.0 - 0.5 * (scale - 1.0) * curve);
		return h00 * protect * scale + h10 * span * scale + h01 + h11 * span * edgeSlope;
	}

	inline double MapCoordinate(double coordinate, double scale, double curve, int geometry, double protect)
	{
		const double centered = coordinate * 2.0 - 1.0;
		const double r = MapRadius(std::abs(centered), scale, curve, geometry, protect);
		return (centered < 0.0 ? -r : r) * 0.5 + 0.5;
	}
}
