// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// K9-2 (C6): phase interpolation of camera models. A de-judder / hiccup consumer needs the camera displacement at a PHASE t between
// two source frames. Homography entries are never interpolated linearly (a blend of two homographies is not the motion in
// between: a zoom would not stay a zoom, a rotation would shrink). Instead a model is reduced to its similarity part in
// picture-centred source px,
//     p' = exp(logScale) * R(angle) * p + (tx, ty),
// and the interpolation runs on logScale and angle (shortest arc), with the translation linear. Affine / homography models
// keep their non-similarity part as a RESIDUAL DISPLACEMENT (exact displacement minus the similarity's displacement), scaled by
// the phase linearly in displacement (not in matrix entries). At the end phase the sum reproduces the model exactly.
//
// Two forms:
//   AtPhase(p, t)         the pair model p (frame N-1 -> N) at fraction t of the pair (t = 0 identity, t = 1 the model);
//   Between(a, b, w, ...) two models of the same kind blended (w = 0 -> a, w = 1 -> b), e.g. consecutive pair models at a
//                         smoothed instant (the P9 smoother's input).
// Both return a PhaseField whose Displacement(x, y) is the per-pixel evaluation (cheap: a handful of flops, no trig per pixel)
// and EvaluateGrid() fills a per-block vector grid the same way P8 stores its model vector (block centre, source px).
//
// Degenerate models (non-finite, a corner at or behind the horizon, scale outside [1/8, 8] of the picture, an empty picture)
// are never interpolated: with one usable end the field takes that end (Source says which), with none it is the identity and
// ok = false, so the consumer falls back visibly. The strict de-judder policy constants live here too (kStrict*): 0.85 / floor
// 0.7 / width 0.05 / rate 0.2, identical to CameraModel::PolicyFor(DejudderWarp) (a test pins the equality).
//
// Pure, deterministic, header-only, C++14, no Windows headers. Coordinates: picture-centred source px, x right, y down
// (CameraMotion.h conventions).
#pragma once

#include <VP-V/modules/motion/estimation/global_model/CameraAccessor.h>
#include <VP-V/modules/motion/estimation/global_model/CameraMotion.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace CameraPhase
{
	// Strictest camera policy (de-judder: a wrong vector tears the picture).
	constexpr double kStrictThreshold = 0.85;
	constexpr double kStrictFloor = 0.7;
	constexpr double kStrictBlendWidth = 0.05;
	constexpr double kStrictRateLimit = 0.2;

	inline CameraModel::Policy StrictPolicy(bool enabled)
	{
		CameraModel::Policy p;
		p.threshold = kStrictThreshold;
		p.floor = kStrictFloor;
		p.blendWidth = kStrictBlendWidth;
		p.rateLimit = kStrictRateLimit;
		p.enabled = enabled;
		return p;
	}

	// Similarity about the picture centre.
	struct Sim
	{
		double logScale = 0, angle = 0, tx = 0, ty = 0;
	};

	enum class Source : uint8_t { Both, OnlyA, OnlyB, Identity };

	namespace detail
	{
		constexpr double kPi = 3.14159265358979323846;
		constexpr double kMaxLogScale = 2.0794415416798357;   // ln 8

		inline bool Finite(double v) { return CameraModel::IsFiniteBits(v); }

		// Shortest signed arc a -> b, in (-pi, pi].
		inline double WrapArc(double d)
		{
			while (d > kPi) d -= 2.0 * kPi;
			while (d <= -kPi) d += 2.0 * kPi;
			return d;
		}

		inline void SimApply(const Sim& s, double x, double y, double& ox, double& oy)
		{
			const double k = std::exp(s.logScale), c = std::cos(s.angle), sn = std::sin(s.angle);
			ox = k * (c * x - sn * y) + s.tx;
			oy = k * (sn * x + c * y) + s.ty;
		}

		// Precomputed form of a Sim for per-pixel use (no trig, no exp per pixel).
		struct SimK
		{
			double a = 1, b = 0, tx = 0, ty = 0;   // x' = a x - b y + tx, y' = b x + a y + ty
			static SimK From(const Sim& s)
			{
				SimK k;
				const double e = std::exp(s.logScale);
				k.a = e * std::cos(s.angle);
				k.b = e * std::sin(s.angle);
				k.tx = s.tx;
				k.ty = s.ty;
				return k;
			}
			void Disp(double x, double y, double& dx, double& dy) const
			{
				dx = (a - 1.0) * x - b * y + tx;
				dy = b * x + (a - 1.0) * y + ty;
			}
		};

		// Exact displacement of a model (false = at/behind the horizon).
		inline bool ModelDisp(const CameraModel::Params& p, double x, double y, double& dx, double& dy)
		{
			const double w = p.h[6] * x + p.h[7] * y + p.h[8];
			if (!(w > 1e-9))
				return false;
			dx = (p.h[0] * x + p.h[1] * y + p.h[2]) / w - x;
			dy = (p.h[3] * x + p.h[4] * y + p.h[5]) / w - y;
			return Finite(dx) && Finite(dy);
		}

		// Least-squares similarity through the five picture probes (corners and centre). False when the model is degenerate.
		inline bool Decompose(const CameraModel::Params& p, const CameraModel::Rect& pic, Sim& out)
		{
			out = Sim();
			if (pic.Empty())
				return false;
			for (int i = 0; i < 9; ++i)
				if (!Finite(p.h[i]))
					return false;
			if (p.type == CameraModel::ModelType::None)
				return true;
			const double hw = 0.5 * pic.Width(), hh = 0.5 * pic.Height();
			const double px[5] = { -hw, hw, hw, -hw, 0.0 }, py[5] = { -hh, -hh, hh, hh, 0.0 };
			double qx[5], qy[5];
			for (int i = 0; i < 5; ++i)
			{
				double dx = 0, dy = 0;
				if (!ModelDisp(p, px[i], py[i], dx, dy))
					return false;
				qx[i] = px[i] + dx;
				qy[i] = py[i] + dy;
			}
			double pbx = 0, pby = 0, qbx = 0, qby = 0;
			for (int i = 0; i < 5; ++i)
			{
				pbx += px[i]; pby += py[i]; qbx += qx[i]; qby += qy[i];
			}
			pbx *= 0.2; pby *= 0.2; qbx *= 0.2; qby *= 0.2;
			double re = 0, im = 0, den = 0;
			for (int i = 0; i < 5; ++i)
			{
				const double ax = px[i] - pbx, ay = py[i] - pby, bx = qx[i] - qbx, by = qy[i] - qby;
				re += ax * bx + ay * by;        // conj(a) * b
				im += ax * by - ay * bx;
				den += ax * ax + ay * ay;
			}
			if (!(den > 0.0))
				return false;
			const double cr = re / den, ci = im / den;
			const double scale = std::sqrt(cr * cr + ci * ci);
			if (!(scale > 0.0) || !Finite(scale))
				return false;
			out.logScale = std::log(scale);
			if (std::fabs(out.logScale) > kMaxLogScale)
				return false;
			out.angle = std::atan2(ci, cr);
			out.tx = qbx - (cr * pbx - ci * pby);
			out.ty = qby - (ci * pbx + cr * pby);
			return Finite(out.tx) && Finite(out.ty) && Finite(out.angle);
		}

		inline bool HasResidual(const CameraModel::Params& p)
		{
			return p.type == CameraModel::ModelType::Affine || p.type == CameraModel::ModelType::Homography;
		}
	}

	// A model at a phase, ready for per-pixel evaluation.
	class PhaseField
	{
	public:
		bool Ok() const { return m_ok; }
		Source Which() const { return m_source; }
		const Sim& Similarity() const { return m_sim; }

		// Displacement (source px) the camera produces at the picture-centred point (x, y).
		void Displacement(double x, double y, double& dx, double& dy) const
		{
			m_k.Disp(x, y, dx, dy);
			for (int i = 0; i < 2; ++i)
			{
				if (m_resWeight[i] == 0.0)
					continue;
				double mx = 0, my = 0;
				if (!detail::ModelDisp(m_full[i], x, y, mx, my))
					continue;   // beyond the horizon at this point (outside the picture): similarity only
				double sx = 0, sy = 0;
				m_fullK[i].Disp(x, y, sx, sy);
				dx += m_resWeight[i] * (mx - sx);
				dy += m_resWeight[i] * (my - sy);
			}
		}

		// Fills one vector per block (block centre, as P8's modelX / modelY: the forward displacement), row-major.
		void EvaluateGrid(const CameraModel::Rect& picture, int gridW, int gridH, double blockSize,
			std::vector<float>& vx, std::vector<float>& vy) const
		{
			const size_t n = static_cast<size_t>(gridW > 0 ? gridW : 0) * static_cast<size_t>(gridH > 0 ? gridH : 0);
			vx.assign(n, 0.0f);
			vy.assign(n, 0.0f);
			for (int by = 0; by < gridH; ++by)
				for (int bx = 0; bx < gridW; ++bx)
				{
					double cx = 0, cy = 0, dx = 0, dy = 0;
					CameraModel::BlockCentre(bx, by, blockSize, picture, &cx, &cy);
					Displacement(cx, cy, dx, dy);
					const size_t i = static_cast<size_t>(by) * static_cast<size_t>(gridW) + static_cast<size_t>(bx);
					vx[i] = static_cast<float>(dx);
					vy[i] = static_cast<float>(dy);
				}
		}

	private:
		friend PhaseField AtPhase(const CameraModel::Params&, const CameraModel::Rect&, double);
		friend PhaseField Between(const CameraModel::Params&, const CameraModel::Params&, const CameraModel::Rect&, double);

		static PhaseField Identity()
		{
			PhaseField f;
			f.m_ok = false;
			f.m_source = Source::Identity;
			return f;
		}

		void SetSim(const Sim& s)
		{
			m_sim = s;
			m_k = detail::SimK::From(s);
		}

		void SetResidual(int slot, const CameraModel::Params& full, const Sim& fullSim, double weight)
		{
			if (!detail::HasResidual(full) || weight == 0.0)
				return;
			m_full[slot] = full;
			m_fullK[slot] = detail::SimK::From(fullSim);
			m_resWeight[slot] = weight;
		}

		bool m_ok = true;
		Source m_source = Source::Both;
		Sim m_sim;
		detail::SimK m_k;
		CameraModel::Params m_full[2];
		detail::SimK m_fullK[2];
		double m_resWeight[2] = { 0.0, 0.0 };
	};

	// The pair model p at fraction t of the pair (t is clamped to [-1, 2]: a small extrapolation is allowed for a phase just
	// outside the pair, never more).
	inline PhaseField AtPhase(const CameraModel::Params& p, const CameraModel::Rect& pic, double t)
	{
		Sim s;
		if (!detail::Finite(t) || !detail::Decompose(p, pic, s))
			return PhaseField::Identity();
		if (t < -1.0) t = -1.0;
		if (t > 2.0) t = 2.0;
		PhaseField f;
		Sim st;
		st.logScale = s.logScale * t;
		st.angle = s.angle * t;
		st.tx = s.tx * t;
		st.ty = s.ty * t;
		f.SetSim(st);
		f.SetResidual(0, p, s, t);
		f.m_source = Source::Both;
		return f;
	}

	// Two models blended at weight w in [0, 1] (clamped): log-scale and shortest-arc angle, linear translation, residuals linear in
	// displacement.
	inline PhaseField Between(const CameraModel::Params& a, const CameraModel::Params& b, const CameraModel::Rect& pic, double w)
	{
		Sim sa, sb;
		const bool okA = detail::Decompose(a, pic, sa), okB = detail::Decompose(b, pic, sb);
		if (!detail::Finite(w))
			return PhaseField::Identity();
		if (w < 0.0) w = 0.0;
		if (w > 1.0) w = 1.0;
		if (!okA && !okB)
			return PhaseField::Identity();
		PhaseField f;
		if (okA && !okB)
		{
			f.SetSim(sa);
			f.SetResidual(0, a, sa, 1.0);
			f.m_source = Source::OnlyA;
			return f;
		}
		if (!okA && okB)
		{
			f.SetSim(sb);
			f.SetResidual(1, b, sb, 1.0);
			f.m_source = Source::OnlyB;
			return f;
		}
		Sim s;
		s.logScale = (1.0 - w) * sa.logScale + w * sb.logScale;
		s.angle = sa.angle + w * detail::WrapArc(sb.angle - sa.angle);
		s.tx = (1.0 - w) * sa.tx + w * sb.tx;
		s.ty = (1.0 - w) * sa.ty + w * sb.ty;
		f.SetSim(s);
		f.SetResidual(0, a, sa, 1.0 - w);
		f.SetResidual(1, b, sb, w);
		f.m_source = Source::Both;
		return f;
	}
}
