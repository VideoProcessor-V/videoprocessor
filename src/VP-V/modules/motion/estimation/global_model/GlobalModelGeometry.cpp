// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Global camera model: geometry solvers. Deterministic, double precision, fixed accumulation
// order, no pixel constants (all thresholds are dimensionless or relative to the picture size).
#include "GlobalModelGeometry.h"

#include <algorithm>
#include <cmath>

namespace CameraModel
{
	namespace
	{
		const double kPivotTiny = 1e-11;      // relative pivot floor in the Gaussian eliminations
		const double kCollinearTiny = 1e-9;   // |cross| relative to the squared extent
		const double kSpreadTiny = 1e-6;      // point spread relative to the picture half diagonal
		const double kHomogeneousTiny = 1e-12;

		// Solve a (n x n, row-major, destroyed) * x = b (b overwritten with x). Partial pivoting;
		// false when a pivot is tiny relative to the largest matrix entry or the result is not finite.
		bool SolveLinear(double* a, double* b, int n, double relTiny)
		{
			double maxAbs = 0;
			for (int i = 0; i < n * n; ++i)
				maxAbs = std::max(maxAbs, std::fabs(a[i]));
			if (!(maxAbs > 0) || !std::isfinite(maxAbs))
				return false;
			for (int c = 0; c < n; ++c)
			{
				int piv = c;
				double best = std::fabs(a[c * n + c]);
				for (int r = c + 1; r < n; ++r)
				{
					const double v = std::fabs(a[r * n + c]);
					if (v > best) { best = v; piv = r; }
				}
				if (!(best > relTiny * maxAbs))
					return false;
				if (piv != c)
				{
					for (int k = 0; k < n; ++k)
						std::swap(a[c * n + k], a[piv * n + k]);
					std::swap(b[c], b[piv]);
				}
				for (int r = c + 1; r < n; ++r)
				{
					const double f = a[r * n + c] / a[c * n + c];
					if (f == 0.0)
						continue;
					for (int k = c; k < n; ++k)
						a[r * n + k] -= f * a[c * n + k];
					b[r] -= f * b[c];
				}
			}
			for (int r = n - 1; r >= 0; --r)
			{
				double s = b[r];
				for (int k = r + 1; k < n; ++k)
					s -= a[r * n + k] * b[k];
				b[r] = s / a[r * n + r];
				if (!std::isfinite(b[r]))
					return false;
			}
			return true;
		}

		void Mul3(const double* a, const double* b, double* out)
		{
			double t[9];
			for (int r = 0; r < 3; ++r)
				for (int c = 0; c < 3; ++c)
					t[r * 3 + c] = a[r * 3 + 0] * b[0 * 3 + c] + a[r * 3 + 1] * b[1 * 3 + c] + a[r * 3 + 2] * b[2 * 3 + c];
			for (int i = 0; i < 9; ++i)
				out[i] = t[i];
		}

		double MaxAbs9(const double* h)
		{
			double m = 0;
			for (int i = 0; i < 9; ++i)
				m = std::max(m, std::fabs(h[i]));
			return m;
		}

		bool AllFinite9(const double* h)
		{
			for (int i = 0; i < 9; ++i)
				if (!std::isfinite(h[i]))
					return false;
			return true;
		}

		// Divide by h[8] when it is not tiny. Returns false when h[8] is (relatively) zero.
		bool NormaliseH8(double* h)
		{
			const double m = MaxAbs9(h);
			if (!(std::fabs(h[8]) > kHomogeneousTiny * m))
				return false;
			const double inv = 1.0 / h[8];
			for (int i = 0; i < 9; ++i)
				h[i] *= inv;
			h[8] = 1.0;
			return true;
		}

		double Cross(double ax, double ay, double bx, double by) { return ax * by - ay * bx; }

		// True when the three points are (near) collinear: |cross| relative to the squared extent.
		bool Collinear(double x0, double y0, double x1, double y1, double x2, double y2)
		{
			const double ux = x1 - x0, uy = y1 - y0, vx = x2 - x0, vy = y2 - y0;
			const double ext2 = std::max(ux * ux + uy * uy, std::max(vx * vx + vy * vy, (ux - vx) * (ux - vx) + (uy - vy) * (uy - vy)));
			if (!(ext2 > 0))
				return true;
			return !(std::fabs(Cross(ux, uy, vx, vy)) > kCollinearTiny * ext2);
		}

		// Hartley normalisation: centroid to the origin, mean distance sqrt(2).
		struct Norm
		{
			double cx = 0, cy = 0, s = 1;
		};

		void CorrectedCorners(const Rect& r, double* cx, double* cy)
		{
			const double hw = 0.5 * r.Width(), hh = 0.5 * r.Height();
			cx[0] = -hw; cy[0] = -hh;
			cx[1] = hw;  cy[1] = -hh;
			cx[2] = hw;  cy[2] = hh;
			cx[3] = -hw; cy[3] = hh;
		}

		double Diagonal(const Rect& r)
		{
			return std::sqrt(static_cast<double>(r.Width()) * r.Width() + static_cast<double>(r.Height()) * r.Height());
		}

		// Homography through samples (exact for count == 4 minimal; weighted DLT otherwise).
		bool SolveHomography(const Sample* s, int n, bool minimal, double scale, Params& out)
		{
			std::vector<double> sx(n), sy(n), dx(n), dy(n), w(n);
			double sumW = 0, maxCoord = 0;
			for (int i = 0; i < n; ++i)
			{
				sx[i] = s[i].x;
				sy[i] = s[i].y;
				dx[i] = s[i].x + s[i].dx;
				dy[i] = s[i].y + s[i].dy;
				w[i] = minimal ? 1.0 : (s[i].w > 0 ? s[i].w : 0.0);
				if (!std::isfinite(sx[i]) || !std::isfinite(sy[i]) || !std::isfinite(dx[i]) || !std::isfinite(dy[i]) || !std::isfinite(w[i]))
					return false;
				if (w[i] > 0)
				{
					sumW += w[i];
					maxCoord = std::max(maxCoord, std::max(std::max(std::fabs(sx[i]), std::fabs(sy[i])), std::max(std::fabs(dx[i]), std::fabs(dy[i]))));
				}
			}
			if (!(sumW > 1e-300) || !std::isfinite(sumW))
				return false;

			Norm ns, nd;
			for (int i = 0; i < n; ++i)
			{
				if (!(w[i] > 0)) continue;
				ns.cx += w[i] * sx[i]; ns.cy += w[i] * sy[i];
				nd.cx += w[i] * dx[i]; nd.cy += w[i] * dy[i];
			}
			ns.cx /= sumW; ns.cy /= sumW; nd.cx /= sumW; nd.cy /= sumW;
			double ms = 0, md = 0;
			for (int i = 0; i < n; ++i)
			{
				if (!(w[i] > 0)) continue;
				ms += w[i] * std::sqrt((sx[i] - ns.cx) * (sx[i] - ns.cx) + (sy[i] - ns.cy) * (sy[i] - ns.cy));
				md += w[i] * std::sqrt((dx[i] - nd.cx) * (dx[i] - nd.cx) + (dy[i] - nd.cy) * (dy[i] - nd.cy));
			}
			ms /= sumW; md /= sumW;
			const double spreadTol = std::max(kSpreadTiny * scale, 1e-12 * maxCoord);
			if (!(ms > spreadTol) || !(md > spreadTol))
				return false;
			const double root2 = std::sqrt(2.0);
			ns.s = root2 / ms;
			nd.s = root2 / md;

			std::vector<double> X(n), Y(n), U(n), V(n);
			for (int i = 0; i < n; ++i)
			{
				X[i] = (sx[i] - ns.cx) * ns.s;
				Y[i] = (sy[i] - ns.cy) * ns.s;
				U[i] = (dx[i] - nd.cx) * nd.s;
				V[i] = (dy[i] - nd.cy) * nd.s;
			}

			double sol[8];
			if (minimal)
			{
				if (n != 4)
					return false;
				for (int i = 0; i < 4; ++i)
					for (int j = i + 1; j < 4; ++j)
						for (int k = j + 1; k < 4; ++k)
							if (Collinear(X[i], Y[i], X[j], Y[j], X[k], Y[k]) || Collinear(U[i], V[i], U[j], V[j], U[k], V[k]))
								return false;
				double a[64] = {};
				for (int i = 0; i < 4; ++i)
				{
					double* r1 = a + (2 * i) * 8;
					double* r2 = a + (2 * i + 1) * 8;
					r1[0] = X[i]; r1[1] = Y[i]; r1[2] = 1;
					r1[6] = -U[i] * X[i]; r1[7] = -U[i] * Y[i];
					r2[3] = X[i]; r2[4] = Y[i]; r2[5] = 1;
					r2[6] = -V[i] * X[i]; r2[7] = -V[i] * Y[i];
					sol[2 * i] = U[i];
					sol[2 * i + 1] = V[i];
				}
				if (!SolveLinear(a, sol, 8, kPivotTiny))
					return false;
			}
			else
			{
				double nrm[64] = {};
				double rhs[8] = {};
				for (int i = 0; i < n; ++i)
				{
					if (!(w[i] > 0)) continue;
					double r1[8] = { X[i], Y[i], 1, 0, 0, 0, -U[i] * X[i], -U[i] * Y[i] };
					double r2[8] = { 0, 0, 0, X[i], Y[i], 1, -V[i] * X[i], -V[i] * Y[i] };
					for (int p = 0; p < 8; ++p)
					{
						for (int q = 0; q < 8; ++q)
							nrm[p * 8 + q] += w[i] * (r1[p] * r1[q] + r2[p] * r2[q]);
						rhs[p] += w[i] * (r1[p] * U[i] + r2[p] * V[i]);
					}
				}
				for (int p = 0; p < 8; ++p)
					sol[p] = rhs[p];
				if (!SolveLinear(nrm, sol, 8, kPivotTiny))
					return false;
			}

			// De-normalise: H = Td^-1 * Hn * Ts.
			double hn[9] = { sol[0], sol[1], sol[2], sol[3], sol[4], sol[5], sol[6], sol[7], 1.0 };
			const double ts[9] = { ns.s, 0, -ns.s * ns.cx, 0, ns.s, -ns.s * ns.cy, 0, 0, 1 };
			const double tdi[9] = { 1.0 / nd.s, 0, nd.cx, 0, 1.0 / nd.s, nd.cy, 0, 0, 1 };
			double t[9], h[9];
			Mul3(hn, ts, t);
			Mul3(tdi, t, h);
			if (!AllFinite9(h) || !NormaliseH8(h))
				return false;
			if (!AllFinite9(h))
				return false;
			out.type = ModelType::Homography;
			for (int i = 0; i < 9; ++i)
				out.h[i] = h[i];
			return true;
		}

		Params MakeTranslation(double tx, double ty)
		{
			Params p = IdentityParams(ModelType::Translation);
			p.h[2] = tx;
			p.h[5] = ty;
			return p;
		}

		Params MakeSimilarity(double a, double b, double tx, double ty)
		{
			Params p = IdentityParams(ModelType::Similarity);
			p.h[0] = a; p.h[1] = -b; p.h[2] = tx;
			p.h[3] = b; p.h[4] = a;  p.h[5] = ty;
			return p;
		}

		// Shared rule behind Interpolate and TimeScale.
		Params ScaleMotion(const Params& p, double f, const Rect& picture)
		{
			if (p.type == ModelType::None)
				return p;
			if (f == 1.0)
				return p;
			if (f == 0.0)
				return IdentityParams(p.type);
			if (!AllFinite9(p.h) || !std::isfinite(f))
				return IdentityParams(p.type);

			if (p.type == ModelType::Translation)
			{
				Params o = IdentityParams(ModelType::Translation);
				o.h[2] = p.h[2] * f;
				o.h[5] = p.h[5] * f;
				return o;
			}
			if (p.type == ModelType::Similarity)
			{
				const double a = p.h[0], b = p.h[3];
				const double s = std::sqrt(a * a + b * b);
				if (!(s > 0) || !std::isfinite(s))
					return IdentityParams(p.type);
				const double ls = std::log(s) * f;
				const double th = std::atan2(b, a) * f;
				const double sf = std::exp(ls);
				return MakeSimilarity(sf * std::cos(th), sf * std::sin(th), p.h[2] * f, p.h[5] * f);
			}

			if (picture.Empty())
				return IdentityParams(p.type);
			double cx[4], cy[4];
			CorrectedCorners(picture, cx, cy);
			Sample smp[4];
			for (int i = 0; i < 4; ++i)
			{
				const double w = p.h[6] * cx[i] + p.h[7] * cy[i] + p.h[8];
				if (!(w > 0))
					return IdentityParams(p.type);
				double dx = 0, dy = 0;
				Displacement(p, cx[i], cy[i], &dx, &dy);
				smp[i].x = cx[i];
				smp[i].y = cy[i];
				smp[i].dx = dx * f;
				smp[i].dy = dy * f;
				smp[i].w = 1.0;
			}
			Params o;
			bool ok;
			if (p.type == ModelType::Affine)
				ok = FitWeighted(ModelType::Affine, smp, 4, 0.5 * Diagonal(picture), o);
			else
				ok = SolveMinimal(ModelType::Homography, smp, 4, o);
			if (!ok)
				return IdentityParams(p.type);
			return o;
		}
	}

	Params IdentityParams(ModelType type)
	{
		Params p;
		p.type = type;
		return p;
	}

	int MinimalCount(ModelType type)
	{
		switch (type)
		{
		case ModelType::Translation: return 1;
		case ModelType::Similarity: return 2;
		case ModelType::Affine: return 3;
		case ModelType::Homography: return 4;
		default: return 0;
		}
	}

	bool SolveMinimal(ModelType type, const Sample* s, int count, Params& out)
	{
		if (s == nullptr || count != MinimalCount(type) || count <= 0)
			return false;
		for (int i = 0; i < count; ++i)
			if (!std::isfinite(s[i].x) || !std::isfinite(s[i].y) || !std::isfinite(s[i].dx) || !std::isfinite(s[i].dy))
				return false;

		switch (type)
		{
		case ModelType::Translation:
			out = MakeTranslation(s[0].dx, s[0].dy);
			return true;

		case ModelType::Similarity:
		{
			const double dpx = s[1].x - s[0].x, dpy = s[1].y - s[0].y;
			const double dqx = (s[1].x + s[1].dx) - (s[0].x + s[0].dx), dqy = (s[1].y + s[1].dy) - (s[0].y + s[0].dy);
			const double d2 = dpx * dpx + dpy * dpy;
			const double ext2 = std::max(s[0].x * s[0].x + s[0].y * s[0].y, s[1].x * s[1].x + s[1].y * s[1].y);
			if (d2 == 0.0 || d2 <= 1e-18 * ext2)
				return false;
			const double a = (dqx * dpx + dqy * dpy) / d2;
			const double b = (dqy * dpx - dqx * dpy) / d2;
			const double tx = (s[0].x + s[0].dx) - (a * s[0].x - b * s[0].y);
			const double ty = (s[0].y + s[0].dy) - (b * s[0].x + a * s[0].y);
			if (!std::isfinite(a) || !std::isfinite(b) || !std::isfinite(tx) || !std::isfinite(ty))
				return false;
			out = MakeSimilarity(a, b, tx, ty);
			return true;
		}

		case ModelType::Affine:
		{
			const double ux = s[1].x - s[0].x, uy = s[1].y - s[0].y;
			const double vx = s[2].x - s[0].x, vy = s[2].y - s[0].y;
			if (Collinear(s[0].x, s[0].y, s[1].x, s[1].y, s[2].x, s[2].y))
				return false;
			const double det = Cross(ux, uy, vx, vy);
			const double Ux = ux + s[1].dx - s[0].dx, Uy = uy + s[1].dy - s[0].dy;
			const double Vx = vx + s[2].dx - s[0].dx, Vy = vy + s[2].dy - s[0].dy;
			// A = [U V] * [u v]^-1
			const double a = (Ux * vy - Vx * uy) / det;
			const double b = (-Ux * vx + Vx * ux) / det;
			const double c = (Uy * vy - Vy * uy) / det;
			const double d = (-Uy * vx + Vy * ux) / det;
			const double tx = (s[0].x + s[0].dx) - (a * s[0].x + b * s[0].y);
			const double ty = (s[0].y + s[0].dy) - (c * s[0].x + d * s[0].y);
			if (!std::isfinite(a) || !std::isfinite(b) || !std::isfinite(c) || !std::isfinite(d) || !std::isfinite(tx) || !std::isfinite(ty))
				return false;
			out = IdentityParams(ModelType::Affine);
			out.h[0] = a; out.h[1] = b; out.h[2] = tx;
			out.h[3] = c; out.h[4] = d; out.h[5] = ty;
			return true;
		}

		case ModelType::Homography:
		{
			Params tmp;
			if (!SolveHomography(s, count, true, 0.0, tmp))
				return false;
			out = tmp;
			return true;
		}

		default:
			return false;
		}
	}

	bool FitWeighted(ModelType type, const Sample* s, int count, double scale, Params& out)
	{
		if (s == nullptr || count < MinimalCount(type) || count <= 0 || MinimalCount(type) == 0)
			return false;
		if (type == ModelType::Homography)
		{
			Params tmp;
			if (!SolveHomography(s, count, false, scale, tmp))
				return false;
			out = tmp;
			return true;
		}

		double sumW = 0;
		double pcx = 0, pcy = 0, dcx = 0, dcy = 0;   // weighted centroid of positions / displacements
		for (int i = 0; i < count; ++i)
		{
			if (!(s[i].w > 0))
				continue;
			if (!std::isfinite(s[i].x) || !std::isfinite(s[i].y) || !std::isfinite(s[i].dx) || !std::isfinite(s[i].dy) || !std::isfinite(s[i].w))
				return false;
			sumW += s[i].w;
			pcx += s[i].w * s[i].x; pcy += s[i].w * s[i].y;
			dcx += s[i].w * s[i].dx; dcy += s[i].w * s[i].dy;
		}
		if (!(sumW > 1e-300) || !std::isfinite(sumW))
			return false;
		pcx /= sumW; pcy /= sumW; dcx /= sumW; dcy /= sumW;

		if (type == ModelType::Translation)
		{
			out = MakeTranslation(dcx, dcy);
			return std::isfinite(dcx) && std::isfinite(dcy);
		}

		// Centred second moments. Positions P = p - pc; displacements D = d - dc (the target
		// offsets Q = P + D, since the centroid of q is pc + dc).
		double sxx = 0, sxy = 0, syy = 0;
		double cxx = 0, cxy = 0, cyx = 0, cyy = 0;   // sum w * P * Q^T  (Q = P + D)
		double sSimA = 0, sSimB = 0;
		for (int i = 0; i < count; ++i)
		{
			if (!(s[i].w > 0))
				continue;
			const double px = s[i].x - pcx, py = s[i].y - pcy;
			const double qx = px + (s[i].dx - dcx), qy = py + (s[i].dy - dcy);
			const double w = s[i].w;
			sxx += w * px * px; sxy += w * px * py; syy += w * py * py;
			cxx += w * px * qx; cxy += w * px * qy;
			cyx += w * py * qx; cyy += w * py * qy;
			sSimA += w * (px * qx + py * qy);
			sSimB += w * (px * qy - py * qx);
		}
		const double trace = sxx + syy;
		const double spreadTol = kSpreadTiny * scale;
		if (!(trace > spreadTol * spreadTol * sumW) || !(trace > 0))
			return false;

		if (type == ModelType::Similarity)
		{
			const double a = sSimA / trace, b = sSimB / trace;
			const double tx = (pcx + dcx) - (a * pcx - b * pcy);
			const double ty = (pcy + dcy) - (b * pcx + a * pcy);
			if (!std::isfinite(a) || !std::isfinite(b) || !std::isfinite(tx) || !std::isfinite(ty))
				return false;
			out = MakeSimilarity(a, b, tx, ty);
			return true;
		}

		// Affine: [a b] = S^-1 * (sum w P Qx), [c d] = S^-1 * (sum w P Qy)
		const double det = sxx * syy - sxy * sxy;
		if (!(det > 1e-10 * trace * trace))
			return false;
		const double a = (syy * cxx - sxy * cyx) / det;
		const double b = (-sxy * cxx + sxx * cyx) / det;
		const double c = (syy * cxy - sxy * cyy) / det;
		const double d = (-sxy * cxy + sxx * cyy) / det;
		const double tx = (pcx + dcx) - (a * pcx + b * pcy);
		const double ty = (pcy + dcy) - (c * pcx + d * pcy);
		if (!std::isfinite(a) || !std::isfinite(b) || !std::isfinite(c) || !std::isfinite(d) || !std::isfinite(tx) || !std::isfinite(ty))
			return false;
		out = IdentityParams(ModelType::Affine);
		out.h[0] = a; out.h[1] = b; out.h[2] = tx;
		out.h[3] = c; out.h[4] = d; out.h[5] = ty;
		return true;
	}

	Params Invert(const Params& p, bool* ok)
	{
		if (ok) *ok = false;
		const double* h = p.h;
		if (!AllFinite9(h))
			return IdentityParams(p.type);
		const double c00 = h[4] * h[8] - h[5] * h[7];
		const double c01 = h[5] * h[6] - h[3] * h[8];
		const double c02 = h[3] * h[7] - h[4] * h[6];
		const double det = h[0] * c00 + h[1] * c01 + h[2] * c02;
		const double m = MaxAbs9(h);
		if (!std::isfinite(det) || !(std::fabs(det) > 1e-14 * m * m * m))
			return IdentityParams(p.type);
		const double id = 1.0 / det;
		Params o;
		o.type = p.type;
		o.h[0] = c00 * id;
		o.h[1] = (h[2] * h[7] - h[1] * h[8]) * id;
		o.h[2] = (h[1] * h[5] - h[2] * h[4]) * id;
		o.h[3] = c01 * id;
		o.h[4] = (h[0] * h[8] - h[2] * h[6]) * id;
		o.h[5] = (h[2] * h[3] - h[0] * h[5]) * id;
		o.h[6] = c02 * id;
		o.h[7] = (h[1] * h[6] - h[0] * h[7]) * id;
		o.h[8] = (h[0] * h[4] - h[1] * h[3]) * id;
		if (!AllFinite9(o.h))
			return IdentityParams(p.type);
		if (p.type == ModelType::Homography)
		{
			if (!NormaliseH8(o.h))
				return IdentityParams(p.type);
		}
		else
		{
			o.h[6] = 0; o.h[7] = 0; o.h[8] = 1;
		}
		if (ok) *ok = true;
		return o;
	}

	Params Compose(const Params& a, const Params& b)
	{
		static const double kIdentity[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
		const double* ha = (a.type == ModelType::None) ? kIdentity : a.h;
		const double* hb = (b.type == ModelType::None) ? kIdentity : b.h;
		Params o;
		o.type = (static_cast<int>(a.type) >= static_cast<int>(b.type)) ? a.type : b.type;
		Mul3(ha, hb, o.h);
		NormaliseH8(o.h);
		return o;
	}

	bool IsDegenerate(const Params& p, const Rect& picture)
	{
		if (!AllFinite9(p.h) || picture.Empty())
			return true;
		const double diag = Diagonal(picture);
		const double picArea = static_cast<double>(picture.Width()) * picture.Height();
		double cx[4], cy[4], mx[4], my[4];
		CorrectedCorners(picture, cx, cy);
		for (int i = 0; i < 4; ++i)
		{
			const double w = p.h[6] * cx[i] + p.h[7] * cy[i] + p.h[8];
			if (!(w > 0) || !std::isfinite(w))
				return true;
			mx[i] = (p.h[0] * cx[i] + p.h[1] * cy[i] + p.h[2]) / w;
			my[i] = (p.h[3] * cx[i] + p.h[4] * cy[i] + p.h[5]) / w;
			if (!std::isfinite(mx[i]) || !std::isfinite(my[i]))
				return true;
			if (std::sqrt(mx[i] * mx[i] + my[i] * my[i]) > 8.0 * diag)
				return true;
		}
		// Convex with the original (positive) orientation at every vertex.
		for (int i = 0; i < 4; ++i)
		{
			const int j = (i + 1) & 3, k = (i + 2) & 3;
			if (!(Cross(mx[j] - mx[i], my[j] - my[i], mx[k] - mx[j], my[k] - my[j]) > 0))
				return true;
		}
		double area2 = 0;
		for (int i = 0; i < 4; ++i)
		{
			const int j = (i + 1) & 3;
			area2 += mx[i] * my[j] - mx[j] * my[i];
		}
		const double ratio = 0.5 * area2 / picArea;
		if (!(ratio >= 1.0 / 64.0) || !(ratio <= 64.0))
			return true;
		return false;
	}

	bool ToSimilarity(const Params& p, const Rect& picture, double pairSeconds, SimilarityRates& out)
	{
		if (!(pairSeconds > 0) || !std::isfinite(pairSeconds) || IsDegenerate(p, picture))
			return false;
		double a = 1, b = 0, tx = 0, ty = 0;
		if (p.type == ModelType::None)
		{
		}
		else if (p.type == ModelType::Translation)
		{
			tx = p.h[2];
			ty = p.h[5];
		}
		else if (p.type == ModelType::Similarity)
		{
			a = p.h[0]; b = p.h[3]; tx = p.h[2]; ty = p.h[5];
		}
		else
		{
			double cx[4], cy[4];
			CorrectedCorners(picture, cx, cy);
			Sample smp[4];
			for (int i = 0; i < 4; ++i)
			{
				double dx = 0, dy = 0;
				Displacement(p, cx[i], cy[i], &dx, &dy);
				smp[i].x = cx[i]; smp[i].y = cy[i]; smp[i].dx = dx; smp[i].dy = dy; smp[i].w = 1.0;
			}
			Params sim;
			if (!FitWeighted(ModelType::Similarity, smp, 4, 0.5 * Diagonal(picture), sim))
				return false;
			a = sim.h[0]; b = sim.h[3]; tx = sim.h[2]; ty = sim.h[5];
		}
		const double s = std::sqrt(a * a + b * b);
		if (!(s > 0) || !std::isfinite(s))
			return false;
		out.logScale = std::log(s) / pairSeconds;
		out.angle = std::atan2(b, a) / pairSeconds;
		out.tx = tx / pairSeconds;
		out.ty = ty / pairSeconds;
		return true;
	}

	Params FromSimilarity(const SimilarityRates& r, double seconds)
	{
		const double s = std::exp(r.logScale * seconds);
		const double th = r.angle * seconds;
		return MakeSimilarity(s * std::cos(th), s * std::sin(th), r.tx * seconds, r.ty * seconds);
	}

	Params Interpolate(const Params& p, double fraction, const Rect& picture)
	{
		return ScaleMotion(p, fraction, picture);
	}

	Params TimeScale(const Params& p, double ratio, const Rect& picture)
	{
		return ScaleMotion(p, ratio, picture);
	}

	double MaxCornerDifference(const Params& a, const Params& b, const Rect& picture)
	{
		if (picture.Empty())
			return 0.0;
		double cx[5], cy[5];
		CorrectedCorners(picture, cx, cy);
		cx[4] = 0; cy[4] = 0;
		double worst = 0;
		for (int i = 0; i < 5; ++i)
		{
			double ax = 0, ay = 0, bx = 0, by = 0;
			Displacement(a, cx[i], cy[i], &ax, &ay);
			Displacement(b, cx[i], cy[i], &bx, &by);
			const double d = std::sqrt((ax - bx) * (ax - bx) + (ay - by) * (ay - by));
			worst = std::max(worst, d);
		}
		return worst / Diagonal(picture);
	}
}
