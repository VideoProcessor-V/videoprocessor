// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Stage B implementation: camera-stabilized window mapping, Cramer-Rao covariance propagation,
// exact Hungarian GNN association, bounce/impulse stitching, BIC 4-model trajectory classifier,
// 2.5D perspective depth check, motion-blur tangent alignment, shutter estimation, 64x36 clutter
// hotspot auto-mask, and session ball appearance prior.
#include "FastObjectTrajectory.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace FastObjects
{
	namespace
	{
		constexpr double kForbiddenCost = 1e12;
		// Clutter hotspot suppression threshold: 3 clutter rejections within decay window (matches 3-frame confirmation).
		constexpr float kHotspotSuppressThreshold = 3.0f;
		// Uniform 1-source-px quantization std floor for Cramer-Rao centroid localization.
		constexpr double kCentroidFloorPx = kInvSqrt12;

		// Analytical 2x2 Jacobian of 3x3 homography p at picture-centred (x, y).
		inline void HomographyJacobian(const CameraModel::Params& p, double x, double y, double j[4])
		{
			const double nx = p.h[0] * x + p.h[1] * y + p.h[2];
			const double ny = p.h[3] * x + p.h[4] * y + p.h[5];
			const double w = p.h[6] * x + p.h[7] * y + p.h[8];
			const double iw = (std::fabs(w) > 1e-12) ? (1.0 / w) : 0.0;
			const double u = nx * iw;
			const double v = ny * iw;
			j[0] = (p.h[0] - p.h[6] * u) * iw;
			j[1] = (p.h[1] - p.h[7] * u) * iw;
			j[2] = (p.h[3] - p.h[6] * v) * iw;
			j[3] = (p.h[4] - p.h[7] * v) * iw;
		}

		// Inverse of a 2x2 symmetric positive-definite matrix; returns false if singular.
		inline bool InvertSym2x2(double a, double b, double d, double& ia, double& ib, double& id)
		{
			const double det = a * d - b * b;
			if (!CameraModel::IsFiniteBits(det) || !(det > 1e-12))
				return false;
			const double inv = 1.0 / det;
			ia = d * inv;
			ib = -b * inv;
			id = a * inv;
			return true;
		}

		// Small dense symmetric linear solver (Gaussian elimination with partial pivoting, N <= 8).
		template <int MaxN>
		bool SolveLinearSystem(int n, const double A_in[MaxN][MaxN], const double b_in[MaxN], double x_out[MaxN], double invDiag_out[MaxN] = nullptr)
		{
			double M[MaxN][MaxN * 2];
			for (int r = 0; r < n; ++r)
			{
				for (int c = 0; c < n; ++c)
				{
					M[r][c] = A_in[r][c];
					M[r][n + c] = (r == c) ? 1.0 : 0.0;
				}
			}
			double rhs[MaxN];
			for (int r = 0; r < n; ++r)
				rhs[r] = b_in[r];

			for (int col = 0; col < n; ++col)
			{
				int pivot = col;
				double bestAbs = std::fabs(M[col][col]);
				for (int r = col + 1; r < n; ++r)
				{
					const double v = std::fabs(M[r][col]);
					if (v > bestAbs)
					{
						bestAbs = v;
						pivot = r;
					}
				}
				if (!CameraModel::IsFiniteBits(bestAbs) || !(bestAbs > 1e-12))
					return false;
				if (pivot != col)
				{
					for (int c = col; c < 2 * n; ++c)
						std::swap(M[col][c], M[pivot][c]);
					std::swap(rhs[col], rhs[pivot]);
				}
				const double diag = M[col][col];
				const double invDiag = 1.0 / diag;
				for (int c = col; c < 2 * n; ++c)
					M[col][c] *= invDiag;
				rhs[col] *= invDiag;

				for (int r = 0; r < n; ++r)
				{
					if (r == col)
						continue;
					const double f = M[r][col];
					if (f == 0.0)
						continue;
					for (int c = col; c < 2 * n; ++c)
						M[r][c] -= f * M[col][c];
					rhs[r] -= f * rhs[col];
				}
			}

			for (int r = 0; r < n; ++r)
			{
				if (!CameraModel::IsFiniteBits(rhs[r]))
					return false;
				x_out[r] = rhs[r];
				if (invDiag_out)
					invDiag_out[r] = std::fmax(0.0, M[r][n + r]);
			}
			return true;
		}

		// Accumulate quadratic Mahalanobis residual sum (p - f)^T W (p - f).
		inline double Mahalanobis2D(double rx, double ry, double wxx, double wxy, double wyy)
		{
			return wxx * rx * rx + 2.0 * wxy * rx * ry + wyy * ry * ry;
		}

		struct SampleWeights
		{
			double wxx[kWindowRingCapacity];
			double wxy[kWindowRingCapacity];
			double wyy[kWindowRingCapacity];
			double tNorm[kWindowRingCapacity];
			double tScale = 1.0;
		};

		void BuildSampleWeights(const WindowSample* samples, int n, SampleWeights& sw)
		{
			double maxAbsTau = 0.0;
			for (int i = 0; i < n; ++i)
				maxAbsTau = std::fmax(maxAbsTau, std::fabs(samples[i].tauSec));
			sw.tScale = (maxAbsTau > 1e-4) ? maxAbsTau : 1.0;
			const double invScale = 1.0 / sw.tScale;

			for (int i = 0; i < n; ++i)
			{
				sw.tNorm[i] = samples[i].tauSec * invScale;
				double ia = 1.0, ib = 0.0, id = 1.0;
				if (!InvertSym2x2(samples[i].covXX, samples[i].covXY, samples[i].covYY, ia, ib, id))
				{
					ia = 1.0;
					ib = 0.0;
					id = 1.0;
				}
				sw.wxx[i] = ia;
				sw.wxy[i] = ib;
				sw.wyy[i] = id;
			}
		}

		// Fit Static model (d=2): p(t) = p0.
		bool FitStatic(const WindowSample* s, int n, const SampleWeights& sw, double& p0x, double& p0y, double& chi2)
		{
			double A[2][2] = { { 0, 0 }, { 0, 0 } };
			double b[2] = { 0, 0 };
			for (int i = 0; i < n; ++i)
			{
				A[0][0] += sw.wxx[i];
				A[0][1] += sw.wxy[i];
				A[1][0] += sw.wxy[i];
				A[1][1] += sw.wyy[i];
				b[0] += sw.wxx[i] * s[i].x + sw.wxy[i] * s[i].y;
				b[1] += sw.wxy[i] * s[i].x + sw.wyy[i] * s[i].y;
			}
			double sol[2];
			if (!SolveLinearSystem<2>(2, A, b, sol))
				return false;
			p0x = sol[0];
			p0y = sol[1];
			chi2 = 0.0;
			for (int i = 0; i < n; ++i)
				chi2 += Mahalanobis2D(s[i].x - p0x, s[i].y - p0y, sw.wxx[i], sw.wxy[i], sw.wyy[i]);
			return true;
		}

		// Fit LinearGlide CV model (d=4): p(t) = p0 + v * t.
		bool FitLinearCV(
			const WindowSample* s, int n, const SampleWeights& sw,
			double& p0x, double& p0y, double& vxSec, double& vySec, double& chi2)
		{
			double A[4][4];
			double b[4];
			std::memset(A, 0, sizeof(A));
			std::memset(b, 0, sizeof(b));

			for (int i = 0; i < n; ++i)
			{
				const double t = sw.tNorm[i];
				const double phi[2] = { 1.0, t };
				for (int r = 0; r < 2; ++r)
				{
					const double rx = phi[r];
					b[r] += rx * (sw.wxx[i] * s[i].x + sw.wxy[i] * s[i].y);
					b[2 + r] += rx * (sw.wxy[i] * s[i].x + sw.wyy[i] * s[i].y);
					for (int c = 0; c < 2; ++c)
					{
						const double rc = rx * phi[c];
						A[r][c] += rc * sw.wxx[i];
						A[r][2 + c] += rc * sw.wxy[i];
						A[2 + r][c] += rc * sw.wxy[i];
						A[2 + r][2 + c] += rc * sw.wyy[i];
					}
				}
			}

			double sol[4];
			if (!SolveLinearSystem<4>(4, A, b, sol))
				return false;
			p0x = sol[0];
			p0y = sol[2];
			vxSec = sol[1] / sw.tScale;
			vySec = sol[3] / sw.tScale;
			chi2 = 0.0;
			for (int i = 0; i < n; ++i)
			{
				const double fx = p0x + vxSec * s[i].tauSec;
				const double fy = p0y + vySec * s[i].tauSec;
				chi2 += Mahalanobis2D(s[i].x - fx, s[i].y - fy, sw.wxx[i], sw.wxy[i], sw.wyy[i]);
			}
			return true;
		}

		// Fit LinearGlide with Tangential Drag (d=5): p(t) = p0 + v * t + 0.5 * a_par * u * t^2, a_par <= 0.
		bool FitLinearDrag(
			const WindowSample* s, int n, const SampleWeights& sw,
			double initVxSec, double initVySec,
			double& p0x, double& p0y, double& vxSec, double& vySec, double& axSec2, double& aySec2, double& chi2)
		{
			double speed = std::sqrt(initVxSec * initVxSec + initVySec * initVySec);
			if (!(speed > 1e-3))
				return false;
			double ux = initVxSec / speed;
			double uy = initVySec / speed;

			double sol[5] = { 0, 0, 0, 0, 0 };
			for (int iter = 0; iter < 2; ++iter)
			{
				double A[5][5];
				double b[5];
				std::memset(A, 0, sizeof(A));
				std::memset(b, 0, sizeof(b));

				for (int i = 0; i < n; ++i)
				{
					const double t = sw.tNorm[i];
					const double q = 0.5 * t * t;
					// Basis rows for x and y:
					// x = p0x + vxN * t + aParN * ux * q
					// y = p0y + vyN * t + aParN * uy * q
					const double jx[5] = { 1.0, t, 0.0, 0.0, ux * q };
					const double jy[5] = { 0.0, 0.0, 1.0, t, uy * q };
					for (int r = 0; r < 5; ++r)
					{
						const double wxr = sw.wxx[i] * jx[r] + sw.wxy[i] * jy[r];
						const double wyr = sw.wxy[i] * jx[r] + sw.wyy[i] * jy[r];
						b[r] += wxr * s[i].x + wyr * s[i].y;
						for (int c = 0; c < 5; ++c)
							A[r][c] += wxr * jx[c] + wyr * jy[c];
					}
				}
				if (!SolveLinearSystem<5>(5, A, b, sol))
					return false;
				const double nvx = sol[1] / sw.tScale;
				const double nvy = sol[3] / sw.tScale;
				const double nspd = std::sqrt(nvx * nvx + nvy * nvy);
				if (!(nspd > 1e-3))
					return false;
				ux = nvx / nspd;
				uy = nvy / nspd;
			}

			// Drag must decelerate (aPar <= 0 along velocity direction).
			const double aParSec2 = sol[4] / (sw.tScale * sw.tScale);
			if (aParSec2 >= 0.0)
				return false;

			p0x = sol[0];
			p0y = sol[2];
			vxSec = sol[1] / sw.tScale;
			vySec = sol[3] / sw.tScale;
			axSec2 = aParSec2 * ux;
			aySec2 = aParSec2 * uy;
			chi2 = 0.0;
			for (int i = 0; i < n; ++i)
			{
				const double t = s[i].tauSec;
				const double fx = p0x + vxSec * t + 0.5 * axSec2 * t * t;
				const double fy = p0y + vySec * t + 0.5 * aySec2 * t * t;
				chi2 += Mahalanobis2D(s[i].x - fx, s[i].y - fy, sw.wxx[i], sw.wxy[i], sw.wyy[i]);
			}
			return true;
		}

		// Fit Ballistic / Parabolic model (d=6): p(t) = p0 + v * t + 0.5 * a * t^2.
		bool FitBallistic(
			const WindowSample* s, int n, const SampleWeights& sw,
			double& p0x, double& p0y, double& vxSec, double& vySec,
			double& axSec2, double& aySec2, double& sigmaAySec2, double& chi2)
		{
			double A[6][6];
			double b[6];
			std::memset(A, 0, sizeof(A));
			std::memset(b, 0, sizeof(b));

			for (int i = 0; i < n; ++i)
			{
				const double t = sw.tNorm[i];
				const double phi[3] = { 1.0, t, 0.5 * t * t };
				for (int r = 0; r < 3; ++r)
				{
					const double rx = phi[r];
					b[r] += rx * (sw.wxx[i] * s[i].x + sw.wxy[i] * s[i].y);
					b[3 + r] += rx * (sw.wxy[i] * s[i].x + sw.wyy[i] * s[i].y);
					for (int c = 0; c < 3; ++c)
					{
						const double rc = rx * phi[c];
						A[r][c] += rc * sw.wxx[i];
						A[r][3 + c] += rc * sw.wxy[i];
						A[3 + r][c] += rc * sw.wxy[i];
						A[3 + r][3 + c] += rc * sw.wyy[i];
					}
				}
			}

			double sol[6];
			double invDiag[6];
			if (!SolveLinearSystem<6>(6, A, b, sol, invDiag))
				return false;
			const double tScale2 = sw.tScale * sw.tScale;
			p0x = sol[0];
			p0y = sol[3];
			vxSec = sol[1] / sw.tScale;
			vySec = sol[4] / sw.tScale;
			axSec2 = sol[2] / tScale2;
			aySec2 = sol[5] / tScale2;
			sigmaAySec2 = std::sqrt(invDiag[5]) / tScale2;
			chi2 = 0.0;
			for (int i = 0; i < n; ++i)
			{
				const double t = s[i].tauSec;
				const double fx = p0x + vxSec * t + 0.5 * axSec2 * t * t;
				const double fy = p0y + vySec * t + 0.5 * aySec2 * t * t;
				chi2 += Mahalanobis2D(s[i].x - fx, s[i].y - fy, sw.wxx[i], sw.wxy[i], sw.wyy[i]);
			}
			return true;
		}

		// Two-leg Impulse fit at fixed split time tauS between samples[splitK] and samples[splitK+1].
		// Solves 6 linear parameters (psX, vMinusX, vPlusX, psY, vMinusY, vPlusY) + optional shared gravity ay >= 0.
		struct ImpulseFitResult
		{
			bool ok = false;
			double tauS = 0.0;
			double psX = 0.0, psY = 0.0;
			double vMinusX = 0.0, vMinusY = 0.0;
			double vPlusX = 0.0, vPlusY = 0.0;
			double sharedAy = 0.0;
			double sigmaVMinus = 0.0;
			double sigmaVPlus = 0.0;
			double sigmaDeltaV = 0.0;
			double chi2 = 1e30;
			int dofParams = 7;
			int splitIndex = -1;
		};

		ImpulseFitResult SolveImpulseAtTau(
			const WindowSample* s, int n, const SampleWeights& sw, int splitK, double tauS, bool withGravity)
		{
			ImpulseFitResult res;
			res.tauS = tauS;
			res.splitIndex = splitK;
			res.dofParams = withGravity ? 8 : 7;

			const int dim = withGravity ? 7 : 6;
			double A[7][7];
			double b[7];
			std::memset(A, 0, sizeof(A));
			std::memset(b, 0, sizeof(b));

			const double invScale = 1.0 / sw.tScale;
			for (int i = 0; i < n; ++i)
			{
				const double dtN = (s[i].tauSec - tauS) * invScale;
				const bool isLeft = (i <= splitK);
				double jx[7] = { 1.0, isLeft ? dtN : 0.0, isLeft ? 0.0 : dtN, 0.0, 0.0, 0.0, 0.0 };
				double jy[7] = { 0.0, 0.0, 0.0, 1.0, isLeft ? dtN : 0.0, isLeft ? 0.0 : dtN, withGravity ? (0.5 * dtN * dtN) : 0.0 };
				for (int r = 0; r < dim; ++r)
				{
					const double wxr = sw.wxx[i] * jx[r] + sw.wxy[i] * jy[r];
					const double wyr = sw.wxy[i] * jx[r] + sw.wyy[i] * jy[r];
					b[r] += wxr * s[i].x + wyr * s[i].y;
					for (int c = 0; c < dim; ++c)
						A[r][c] += wxr * jx[c] + wyr * jy[c];
				}
			}

			double sol[7];
			double invDiag[7];
			if (!SolveLinearSystem<7>(dim, A, b, sol, invDiag))
				return res;

			const double tScale2 = sw.tScale * sw.tScale;
			double ay = withGravity ? (sol[6] / tScale2) : 0.0;
			if (withGravity && ay < 0.0)
				return res; // Gravity must point downward (y-down convention)

			res.ok = true;
			res.psX = sol[0];
			res.vMinusX = sol[1] / sw.tScale;
			res.vPlusX = sol[2] / sw.tScale;
			res.psY = sol[3];
			res.vMinusY = sol[4] / sw.tScale;
			res.vPlusY = sol[5] / sw.tScale;
			res.sharedAy = ay;
			res.sigmaVMinus = std::sqrt(invDiag[1] + invDiag[4]) / sw.tScale;
			res.sigmaVPlus = std::sqrt(invDiag[2] + invDiag[5]) / sw.tScale;
			res.sigmaDeltaV = std::sqrt(res.sigmaVMinus * res.sigmaVMinus + res.sigmaVPlus * res.sigmaVPlus);

			res.chi2 = 0.0;
			for (int i = 0; i < n; ++i)
			{
				const double dt = s[i].tauSec - tauS;
				const bool isLeft = (i <= splitK);
				const double vx = isLeft ? res.vMinusX : res.vPlusX;
				const double vy = isLeft ? res.vMinusY : res.vPlusY;
				const double fx = res.psX + vx * dt;
				const double fy = res.psY + vy * dt + 0.5 * res.sharedAy * dt * dt;
				res.chi2 += Mahalanobis2D(s[i].x - fx, s[i].y - fy, sw.wxx[i], sw.wxy[i], sw.wyy[i]);
			}
			return res;
		}

		// Golden-section search for optimal vertex time tauS in [tau_k, tau_{k+1}].
		ImpulseFitResult OptimizeImpulseOnInterval(
			const WindowSample* s, int n, const SampleWeights& sw, int splitK, bool withGravity)
		{
			double lo = s[splitK].tauSec;
			double hi = s[splitK + 1].tauSec;
			if (!(hi > lo + 1e-6))
				return ImpulseFitResult{};

			constexpr double kPhi = 0.6180339887498949;
			double c = hi - kPhi * (hi - lo);
			double d = lo + kPhi * (hi - lo);
			ImpulseFitResult fc = SolveImpulseAtTau(s, n, sw, splitK, c, withGravity);
			ImpulseFitResult fd = SolveImpulseAtTau(s, n, sw, splitK, d, withGravity);

			for (int iter = 0; iter < 14; ++iter)
			{
				const double vc = fc.ok ? fc.chi2 : 1e30;
				const double vd = fd.ok ? fd.chi2 : 1e30;
				if (vc < vd)
				{
					hi = d;
					d = c;
					fd = fc;
					c = hi - kPhi * (hi - lo);
					fc = SolveImpulseAtTau(s, n, sw, splitK, c, withGravity);
				}
				else
				{
					lo = c;
					c = d;
					fc = fd;
					d = lo + kPhi * (hi - lo);
					fd = SolveImpulseAtTau(s, n, sw, splitK, d, withGravity);
				}
			}

			// Also check interval midpoint & endpoints (vertex can sit right on a frame timestamp).
			ImpulseFitResult best = (fc.ok && (!fd.ok || fc.chi2 <= fd.chi2)) ? fc : fd;
			const double endpoints[3] = { s[splitK].tauSec, 0.5 * (lo + hi), s[splitK + 1].tauSec };
			for (int e = 0; e < 3; ++e)
			{
				ImpulseFitResult cand = SolveImpulseAtTau(s, n, sw, splitK, endpoints[e], withGravity);
				if (cand.ok && (!best.ok || cand.chi2 < best.chi2))
					best = cand;
			}
			return best;
		}

		// Evaluate impulse curve position & instantaneous velocity at time tau.
		inline void EvalImpulseAt(const ImpulseFitResult& imp, double tau, double& ox, double& oy, double& vx, double& vy)
		{
			const double dt = tau - imp.tauS;
			const bool isLeft = (tau <= imp.tauS);
			const double v0x = isLeft ? imp.vMinusX : imp.vPlusX;
			const double v0y = isLeft ? imp.vMinusY : imp.vPlusY;
			ox = imp.psX + v0x * dt;
			oy = imp.psY + v0y * dt + 0.5 * imp.sharedAy * dt * dt;
			vx = v0x;
			vy = v0y + imp.sharedAy * dt;
		}

		// 2.5D Pinhole Perspective Depth model (spec C.4.3):
		// Tracks 1/r(t) = c0 + c1*t and projects (x/r, y/r) linearly or parabolically.
		struct PerspectiveFitResult
		{
			bool ok = false;
			TrajectoryType type = TrajectoryType::LinearGlide;
			double p0x = 0.0, p0y = 0.0;
			double vxSec = 0.0, vySec = 0.0;
			double axSec2 = 0.0, aySec2 = 0.0;
			double nextDispX = 0.0, nextDispY = 0.0;
			double radiusAtZero = 0.0;
			double radiusRatePerSec = 0.0;
			double chi2 = 1e30;
			int dofParams = 5;
		};

		PerspectiveFitResult FitPerspectiveDepth(
			const WindowSample* s, int n, const SampleWeights& sw,
			double pairSeconds, const CameraModel::Rect& picture)
		{
			PerspectiveFitResult out;
			if (n < 4)
				return out;

			// Fit 1/r(tau) = c0 + c1 * tau_norm via weighted least squares.
			double A2[2][2] = { { 0, 0 }, { 0, 0 } };
			double b2[2] = { 0, 0 };
			for (int i = 0; i < n; ++i)
			{
				if (!(s[i].radiusPx > 0.5))
					return out;
				const double u = 1.0 / s[i].radiusPx;
				// Exact delta-method variance Var(1/r) = sigma_r^2 / r^4 from sample's own Cramer-Rao covariance.
				const double varR = std::fmax(0.5 * (s[i].covXX + s[i].covYY), kCentroidFloorPx * kCentroidFloorPx);
				const double r2 = s[i].radiusPx * s[i].radiusPx;
				const double wu = (r2 * r2) / varR;
				const double t = sw.tNorm[i];
				A2[0][0] += wu;
				A2[0][1] += wu * t;
				A2[1][0] += wu * t;
				A2[1][1] += wu * t * t;
				b2[0] += wu * u;
				b2[1] += wu * t * u;
			}

			double solU[2];
			double invDiagU[2];
			if (!SolveLinearSystem<2>(2, A2, b2, solU, invDiagU))
				return out;

			const double c0 = solU[0];
			const double c1N = solU[1];
			const double sigmaC1N = std::sqrt(invDiagU[1]);
			// Require non-trivial depth slope (|c1| > 1.5*sigma; BIC still penalizes the extra depth DOF by +ln(2N)).
			if (!(std::fabs(c1N) > 1.5 * sigmaC1N) || !(c0 > 1e-4))
				return out;

			for (int i = 0; i < n; ++i)
			{
				if (!(c0 + c1N * sw.tNorm[i] > 1e-4))
					return out;
			}

			const double picCx = picture.Empty() ? 0.0 : picture.CentreX();
			const double picCy = picture.Empty() ? 0.0 : picture.CentreY();

			// Fit 3D linear (d=5: X0, Vx, Y0, Vy, k) and 3D ballistic (d=7: + Ax, Ay) in image-plane WLS.
			// Notice x(t) - picCx = (X0 + Vx*t + 0.5*Ax*t^2) / (1 + k*t) is EXACTLY LINEAR in (X0, Vx, Ax) given k = c1/c0.
			auto evalProjPolyAtK = [&](double kVal, int order, double& outChi2, double solP[6], double invDiagP[6]) -> bool
			{
				for (int i = 0; i < n; ++i)
				{
					if (!(1.0 + kVal * sw.tNorm[i] > 0.1))
						return false;
				}
				const int dim = 2 * order;
				double A[6][6];
				double b[6];
				std::memset(A, 0, sizeof(A));
				std::memset(b, 0, sizeof(b));
				for (int i = 0; i < n; ++i)
				{
					const double t = sw.tNorm[i];
					const double invU = 1.0 / (c0 * (1.0 + kVal * t));
					const double phi[3] = { invU, t * invU, 0.5 * t * t * invU };
					const double rx = s[i].x - picCx;
					const double ry = s[i].y - picCy;
					for (int r = 0; r < order; ++r)
					{
						b[r] += phi[r] * (sw.wxx[i] * rx + sw.wxy[i] * ry);
						b[order + r] += phi[r] * (sw.wxy[i] * rx + sw.wyy[i] * ry);
						for (int c = 0; c < order; ++c)
						{
							const double rc = phi[r] * phi[c];
							A[r][c] += rc * sw.wxx[i];
							A[r][order + c] += rc * sw.wxy[i];
							A[order + r][c] += rc * sw.wxy[i];
							A[order + r][order + c] += rc * sw.wyy[i];
						}
					}
				}
				if (!SolveLinearSystem<6>(dim, A, b, solP, invDiagP))
					return false;
				outChi2 = 0.0;
				for (int i = 0; i < n; ++i)
				{
					const double t = sw.tNorm[i];
					const double invU = 1.0 / (c0 * (1.0 + kVal * t));
					double px = solP[0] + solP[1] * t;
					double py = solP[order] + solP[order + 1] * t;
					if (order == 3)
					{
						px += 0.5 * solP[2] * t * t;
						py += 0.5 * solP[order + 2] * t * t;
					}
					const double fx = picCx + px * invU;
					const double fy = picCy + py * invU;
					outChi2 += Mahalanobis2D(s[i].x - fx, s[i].y - fy, sw.wxx[i], sw.wxy[i], sw.wyy[i]);
				}
				return true;
			};

			// Refine depth rate k = c1 / c0 within [0.5 * kInit, 2.0 * kInit] (strictly preserving observed dr/dt sign)
			// to eliminate half-res Sheppard radius bias while staying anchored to the measured apparent-size change.
			const double kInit = c1N / c0;
			double kBest = kInit;
			{
				double lo = (kInit < 0.0) ? (2.0 * kInit) : (0.5 * kInit);
				double hi = (kInit < 0.0) ? (0.5 * kInit) : (2.0 * kInit);
				constexpr double kPhi = 0.6180339887498949;
				double cK = hi - kPhi * (hi - lo);
				double dK = lo + kPhi * (hi - lo);
				double tmpSol[6] = {}, tmpInv[6] = {};
				double fc = 1e30, fd = 1e30;
				if (!evalProjPolyAtK(cK, 2, fc, tmpSol, tmpInv)) fc = 1e30;
				if (!evalProjPolyAtK(dK, 2, fd, tmpSol, tmpInv)) fd = 1e30;
				for (int iter = 0; iter < 10; ++iter)
				{
					if (fc < fd)
					{
						hi = dK;
						dK = cK;
						fd = fc;
						cK = hi - kPhi * (hi - lo);
						if (!evalProjPolyAtK(cK, 2, fc, tmpSol, tmpInv)) fc = 1e30;
					}
					else
					{
						lo = cK;
						cK = dK;
						fc = fd;
						dK = lo + kPhi * (hi - lo);
						if (!evalProjPolyAtK(dK, 2, fd, tmpSol, tmpInv)) fd = 1e30;
					}
				}
				const double kMid = 0.5 * (lo + hi);
				double fInit = 1e30, fMid = 1e30;
				if (!evalProjPolyAtK(kInit, 2, fInit, tmpSol, tmpInv)) fInit = 1e30;
				if (!evalProjPolyAtK(kMid, 2, fMid, tmpSol, tmpInv)) fMid = 1e30;
				kBest = (fMid < fInit) ? kMid : kInit;
			}
			const double c1Refined = c0 * kBest;

			double chi2Lin = 1e30, chi2Bal = 1e30;
			double solLin[6] = {}, invLin[6] = {};
			double solBal[6] = {}, invBal[6] = {};
			const bool okLin = evalProjPolyAtK(kBest, 2, chi2Lin, solLin, invLin);
			const bool okBal = (n >= 4) && evalProjPolyAtK(kBest, 3, chi2Bal, solBal, invBal);
			const double log2N = std::log(static_cast<double>(2 * n));

			const double bicLin = okLin ? (chi2Lin + 5.0 * log2N) : 1e30;
			double bicBal = 1e30;
			if (okBal)
			{
				const double ay3D = solBal[5];
				const double sigmaAy3D = std::sqrt(invBal[5]);
				if (ay3D >= -3.0 * sigmaAy3D)
					bicBal = chi2Bal + 7.0 * log2N;
			}

			if (!okLin && !(bicBal < 1e29))
				return out;

			const bool pickBal = (bicBal < bicLin);
			const double* sol = pickBal ? solBal : solLin;
			const int ord = pickBal ? 3 : 2;
			const double c1Sec = c1Refined / sw.tScale;
			const double r0 = 1.0 / c0;

			auto evalImagePos = [&](double tauSec, double& ox, double& oy)
			{
				const double t = tauSec / sw.tScale;
				const double u = std::fmax(c0 + c1Refined * t, 1e-4);
				double px = sol[0] + sol[1] * t;
				double py = sol[ord] + sol[ord + 1] * t;
				if (pickBal)
				{
					px += 0.5 * sol[2] * t * t;
					py += 0.5 * sol[ord + 2] * t * t;
				}
				ox = picCx + px / u;
				oy = picCy + py / u;
			};

			double p0x = 0.0, p0y = 0.0, p1x = 0.0, p1y = 0.0;
			evalImagePos(0.0, p0x, p0y);
			evalImagePos(pairSeconds, p1x, p1y);

			// Exact analytical first/second image derivatives at tau = 0:
			// x(t) = picCx + P(t)/u(t) => x'(0) = (P'(0) - c1 * P(0)/c0) / c0.
			const double X0 = sol[0], Vx0 = sol[1] / sw.tScale, Ax0 = pickBal ? (sol[2] / (sw.tScale * sw.tScale)) : 0.0;
			const double Y0 = sol[ord], Vy0 = sol[ord + 1] / sw.tScale, Ay0 = pickBal ? (sol[ord + 2] / (sw.tScale * sw.tScale)) : 0.0;
			const double vx0 = (Vx0 - c1Sec * X0 / c0) / c0;
			const double vy0 = (Vy0 - c1Sec * Y0 / c0) / c0;
			const double ax0 = (Ax0 - 2.0 * c1Sec * vx0) / c0;
			const double ay0 = (Ay0 - 2.0 * c1Sec * vy0) / c0;

			out.ok = true;
			out.type = pickBal ? TrajectoryType::Ballistic : TrajectoryType::LinearGlide;
			out.p0x = p0x;
			out.p0y = p0y;
			out.vxSec = vx0;
			out.vySec = vy0;
			out.axSec2 = ax0;
			out.aySec2 = ay0;
			out.nextDispX = p1x - p0x;
			out.nextDispY = p1y - p0y;
			out.radiusAtZero = r0;
			out.radiusRatePerSec = -c1Sec * r0 * r0;
			out.chi2 = pickBal ? chi2Bal : chi2Lin;
			out.dofParams = pickBal ? 7 : 5;
			return out;
		}

		// Compute motion-blur tangent alignment factor g_blur and median online shutter estimate tau_s.
		void EvaluateMotionBlurCoupling(
			const WindowSample* samples, int n,
			const TrajectoryClassification& cls,
			double& outGBlur, double& outShutterEst, bool& outBlurStretched)
		{
			outGBlur = 1.0;
			outShutterEst = 0.0;
			outBlurStretched = false;

			double logGSum = 0.0;
			int elongatedCount = 0;
			double shutterVals[kWindowRingCapacity];
			int shutterCount = 0;

			for (int i = 0; i < n; ++i)
			{
				const WindowSample& s = samples[i];
				if (!(s.streakLenPx > s.radiusPx) || !(s.streakLenPx > 1e-3))
					continue;
				outBlurStretched = true;

				double vx = cls.vxSec + cls.axSec2 * s.tauSec;
				double vy = cls.vySec + cls.aySec2 * s.tauSec;
				if (cls.type == TrajectoryType::Impulse && cls.hasImpulse)
				{
					const double dt = s.tauSec - cls.impulseTauSec;
					const bool isLeft = (s.tauSec <= cls.impulseTauSec);
					vx = isLeft ? cls.vMinusX : cls.vPlusX;
					vy = (isLeft ? cls.vMinusY : cls.vPlusY) + cls.aySec2 * dt;
				}

				const double spd = std::sqrt(vx * vx + vy * vy);
				if (!(spd > 1e-3))
					continue;

				const double dot = std::fabs((vx * s.streakDirX + vy * s.streakDirY) / spd);
				const double clampedDot = std::fmin(1.0, dot);
				const double dTheta = std::acos(clampedDot);
				const double snrScale = std::fmax(1.0, s.intSnr / kGrowSigma);
				const double sigmaTheta = s.radiusPx / (s.streakLenPx * snrScale);
				const double excess = std::fmax(0.0, dTheta - sigmaTheta);
				constexpr double kInvAngleScale = 6.0 / kPi; // 1 / (pi / 6)
				const double ratio = excess * kInvAngleScale;
				logGSum += -0.5 * ratio * ratio;
				++elongatedCount;

				if (s.dtStepSec > 1e-6)
				{
					const double rawTau = s.streakLenPx / (spd * s.dtStepSec);
					shutterVals[shutterCount++] = std::fmin(std::fmax(rawTau, 0.0), 1.0);
				}
			}

			if (elongatedCount > 0)
				outGBlur = std::exp(logGSum / static_cast<double>(elongatedCount));
			if (shutterCount > 0)
			{
				std::sort(shutterVals, shutterVals + shutterCount);
				if ((shutterCount & 1) == 1)
					outShutterEst = shutterVals[shutterCount / 2];
				else
					outShutterEst = 0.5 * (shutterVals[shutterCount / 2 - 1] + shutterVals[shutterCount / 2]);
			}
		}

		// Cramer-Rao centroid localization std in source px (spec B.1).
		inline double CramerRaoCentroidSigmaPx(const BlobCandidate& b)
		{
			const double r = static_cast<double>(b.radiusPx);
			const double l = static_cast<double>(b.streakLenPx);
			const double lambda2 = 0.25 * r * r;
			const double lambda1 = lambda2 + (l * l) / 12.0;
			const double sigma = std::sqrt(std::fmax(lambda1 + lambda2, 0.0)) / std::fmax(1.0, static_cast<double>(b.intSnr));
			return std::fmax(sigma, kCentroidFloorPx);
		}
	}

	TrajectoryClassification ClassifyTrajectoryWindow(
		const Config& cfg,
		const WindowSample* samples,
		int count,
		double pairSeconds,
		const CameraModel::Rect& picture)
	{
		TrajectoryClassification out;
		if (!samples || count < 3)
			return out;
		const int n = std::min(count, kWindowRingCapacity);

		// 1. Mask overlap rejection (> 50% of window inside P8 Overlay or CE-WATER).
		int maskHits = 0;
		double meanRadius = 0.0;
		double meanSigmaCent = 0.0;
		for (int i = 0; i < n; ++i)
		{
			if (samples[i].overlapsMask)
				++maskHits;
			meanRadius += samples[i].radiusPx;
			meanSigmaCent += std::sqrt(0.5 * (samples[i].covXX + samples[i].covYY));
		}
		meanRadius /= static_cast<double>(n);
		meanSigmaCent /= static_cast<double>(n);
		if (maskHits * 2 > n)
		{
			out.type = TrajectoryType::Clutter;
			out.rejectedMaskOverlap = true;
			return out;
		}

		// 2. Oscillatory / periodic motion check: >= 2 consecutive velocity direction reversals.
		int reversals = 0;
		double prevDx = 0.0, prevDy = 0.0;
		bool havePrevStep = false;
		const double stepNoiseFloor = 2.0 * meanSigmaCent;
		for (int i = 1; i < n; ++i)
		{
			const double dx = samples[i].x - samples[i - 1].x;
			const double dy = samples[i].y - samples[i - 1].y;
			const double len = std::sqrt(dx * dx + dy * dy);
			if (len > stepNoiseFloor)
			{
				if (havePrevStep && (prevDx * dx + prevDy * dy) < 0.0)
					++reversals;
				prevDx = dx;
				prevDy = dy;
				havePrevStep = true;
			}
		}
		out.velocityReversals = reversals;
		if (reversals >= 2)
		{
			out.type = TrajectoryType::Clutter;
			out.rejectedOscillatory = true;
			return out;
		}

		// 3. Max excursion floor check (stationary/blinking rejection; handles 180-deg rebounds returning near origin).
		double maxExcursion = 0.0;
		for (int i = 1; i < n; ++i)
		{
			const double dx = samples[i].x - samples[0].x;
			const double dy = samples[i].y - samples[0].y;
			maxExcursion = std::fmax(maxExcursion, std::sqrt(dx * dx + dy * dy));
		}
		const double minNetDisp = std::fmax(3.0 * meanSigmaCent, 2.0 * meanRadius);
		if (maxExcursion < minNetDisp)
		{
			out.type = TrajectoryType::Clutter;
			out.rejectedStationary = true;
			return out;
		}

		SampleWeights sw;
		BuildSampleWeights(samples, n, sw);
		const double log2N = std::log(static_cast<double>(2 * n));
		const double picH = picture.Empty() ? 1080.0 : static_cast<double>(picture.Height());
		const double picW = picture.Empty() ? 1920.0 : static_cast<double>(picture.Width());
		const double maxSpeedPxSec = cfg.maxSpeedWidthFracPerSec * picW;
		// Physical sport acceleration envelope derived from Plan 73's 0.5 s frame-crossing timescale:
		// H_pic = 0.5 * a_max * (0.5 s)^2 => a_max = 8.0 * H_pic px/s^2.
		const double maxPhysAccelPxSec2 = 8.0 * picH;

		// 4. Null Static model (d=2).
		double stP0x = 0.0, stP0y = 0.0, chi2Static = 1e30;
		const bool okStatic = FitStatic(samples, n, sw, stP0x, stP0y, chi2Static);
		const double bicStatic = okStatic ? (chi2Static + 2.0 * log2N) : 1e30;

		// 5. Type 2 — LinearGlide: CV (d=4) + optional tangential drag (d=5, n >= 4).
		double cvP0x = 0.0, cvP0y = 0.0, cvVx = 0.0, cvVy = 0.0, chi2Cv = 1e30;
		const bool okCv = FitLinearCV(samples, n, sw, cvP0x, cvP0y, cvVx, cvVy, chi2Cv);
		double bicGlide = okCv ? (chi2Cv + 4.0 * log2N) : 1e30;
		double glP0x = cvP0x, glP0y = cvP0y, glVx = cvVx, glVy = cvVy, glAx = 0.0, glAy = 0.0, chi2Glide = chi2Cv;
		int dofGlide = 4;

		if (okCv && n >= 4)
		{
			double dP0x = 0.0, dP0y = 0.0, dVx = 0.0, dVy = 0.0, dAx = 0.0, dAy = 0.0, chi2Drag = 1e30;
			if (FitLinearDrag(samples, n, sw, cvVx, cvVy, dP0x, dP0y, dVx, dVy, dAx, dAy, chi2Drag))
			{
				const double aMag = std::sqrt(dAx * dAx + dAy * dAy);
				const double bicDrag = chi2Drag + 5.0 * log2N;
				if (aMag <= maxPhysAccelPxSec2 && bicDrag < bicGlide)
				{
					bicGlide = bicDrag;
					glP0x = dP0x;
					glP0y = dP0y;
					glVx = dVx;
					glVy = dVy;
					glAx = dAx;
					glAy = dAy;
					chi2Glide = chi2Drag;
					dofGlide = 5;
				}
			}
		}
		if (okCv && std::sqrt(glVx * glVx + glVy * glVy) > maxSpeedPxSec)
			bicGlide = 1e30;

		// 6. Type 1 — Ballistic / Parabolic Arc (d=6; strictly requires n >= 4 so remDof = 2N - 6 >= 2).
		// At n == 3, CV (d=4, remDof=2) is the clean default (Owner CV-default ruling; prevents 0-DOF random-walk fits).
		double balP0x = 0.0, balP0y = 0.0, balVx = 0.0, balVy = 0.0, balAx = 0.0, balAy = 0.0, balSigAy = 0.0, chi2Bal = 1e30;
		double bicBal = 1e30;
		bool okBal = false;
		bool unconstrainedBalPasses99 = false;
		if (n >= 4 && FitBallistic(samples, n, sw, balP0x, balP0y, balVx, balVy, balAx, balAy, balSigAy, chi2Bal))
		{
			const double aMag = std::sqrt(balAx * balAx + balAy * balAy);
			const double vMag = std::sqrt(balVx * balVx + balVy * balVy);
			// If a smooth unconstrained parabola already fits within 99% chi^2, it is a smooth curve (never an Impulse kink).
			// If such a smooth parabola has unphysical upward anti-gravity (a_y < -3*sigma), it must be rejected as Clutter.
			if (chi2Bal <= Chi2Critical99(2 * n - 6))
				unconstrainedBalPasses99 = true;
			// Downward gravity consistency (y-down): a_y >= -3 * sigma_{a_y}.
			const bool gravOk = (balAy >= -3.0 * balSigAy);
			if (gravOk && aMag <= maxPhysAccelPxSec2 && vMag <= maxSpeedPxSec)
			{
				okBal = true;
				bicBal = chi2Bal + 6.0 * log2N;
			}
		}

		// Single-leg 99% chi^2 goodness-of-fit gates (Spec B.3.3: run Impulse split search ONLY when single-leg fails).
		const bool glidePasses99 = okCv && (bicGlide < 1e29) && (2 * n - dofGlide >= 1) && (chi2Glide <= Chi2Critical99(2 * n - dofGlide));

		// 7. Type 3 — Impulse / Bounce / Strike (d=7 piecewise-linear, optional d=8 shared downward gravity).
		ImpulseFitResult bestImp;
		double bicImp = 1e30;
		if (n >= 4 && !glidePasses99 && !unconstrainedBalPasses99)
		{
			for (int splitK = 1; splitK <= n - 3; ++splitK)
			{
				const bool nearObj = samples[splitK].nearLargeObject || samples[splitK + 1].nearLargeObject;
				for (int gMode = 0; gMode < ((n >= 5) ? 2 : 1); ++gMode)
				{
					const bool withGrav = (gMode == 1);
					ImpulseFitResult cand = OptimizeImpulseOnInterval(samples, n, sw, splitK, withGrav);
					if (!cand.ok)
						continue;

					const double dvx = cand.vPlusX - cand.vMinusX;
					const double dvy = cand.vPlusY - cand.vMinusY;
					const double dvMag = std::sqrt(dvx * dvx + dvy * dvy);
					const double vMinusMag = std::sqrt(cand.vMinusX * cand.vMinusX + cand.vMinusY * cand.vMinusY);
					const double vPlusMag = std::sqrt(cand.vPlusX * cand.vPlusX + cand.vPlusY * cand.vPlusY);

					if (vMinusMag > maxSpeedPxSec || vPlusMag > maxSpeedPxSec)
						continue;
					// Impulse velocity discontinuity must be statistically significant (> 3 sigma).
					if (!(dvMag > 3.0 * cand.sigmaDeltaV))
						continue;

					// Physical energy / contact check (spec B.3.3):
					// Passive bounce: ||v+|| <= ||v-|| + 3*sigma_v; active strike: allowed near P8 Label::Object.
					const double sigmaV = std::sqrt(cand.sigmaVMinus * cand.sigmaVMinus + cand.sigmaVPlus * cand.sigmaVPlus);
					const bool passiveBounce = (vPlusMag <= vMinusMag + 3.0 * sigmaV);
					const bool activeStrike = nearObj;
					if (!passiveBounce && !activeStrike)
						continue;

					const double candBic = cand.chi2 + static_cast<double>(cand.dofParams) * log2N;
					if (candBic < bicImp)
					{
						bicImp = candBic;
						bestImp = cand;
					}
				}
			}
		}

		// 8. 2.5D Perspective Depth check (spec C.4.3).
		PerspectiveFitResult persp;
		double bicPersp = 1e30;
		if (cfg.enablePerspectiveDepth && n >= 4)
		{
			persp = FitPerspectiveDepth(samples, n, sw, pairSeconds, picture);
			if (persp.ok)
			{
				const int pDof = 2 * n - persp.dofParams;
				if (pDof >= 1 && persp.chi2 <= Chi2Critical99(pDof))
					bicPersp = persp.chi2 + static_cast<double>(persp.dofParams) * log2N;
			}
		}

		// 9. BIC Model Selection + Explicit Null Static & Chi-Squared Coherence Gates.
		struct ModelCandidate
		{
			TrajectoryType type;
			double bic;
			double chi2;
			int dofParams;
			bool isPersp;
		};
		ModelCandidate cands[4] = {
			{ TrajectoryType::LinearGlide, bicGlide, chi2Glide, dofGlide, false },
			{ TrajectoryType::Ballistic, bicBal, chi2Bal, 6, false },
			{ TrajectoryType::Impulse, bicImp, bestImp.chi2, bestImp.dofParams, false },
			{ persp.type, bicPersp, persp.chi2, persp.dofParams, true }
		};

		int bestIdx = -1;
		double bestBic = 1e30;
		double runnerUpBic = 1e30;
		for (int i = 0; i < 4; ++i)
		{
			if (!(cands[i].bic < 1e29))
				continue;
			// Goodness-of-fit check: chi^2 <= chi^2_crit(2N - d, 0.99).
			const int remDof = 2 * n - cands[i].dofParams;
			if (remDof >= 1 && cands[i].chi2 > Chi2Critical99(remDof))
				continue;
			if (cands[i].bic < bestBic)
			{
				runnerUpBic = bestBic;
				bestBic = cands[i].bic;
				bestIdx = i;
			}
			else if (cands[i].bic < runnerUpBic)
			{
				runnerUpBic = cands[i].bic;
			}
		}

		// Incoherent / random walk rejection if no model passes the 99% chi^2 gate.
		if (bestIdx < 0)
		{
			out.type = TrajectoryType::Clutter;
			out.rejectedIncoherent = true;
			return out;
		}

		// Null Stationary BIC rejection (spec B.3.4).
		if (bicStatic <= bestBic)
		{
			out.type = TrajectoryType::Clutter;
			out.rejectedStationary = true;
			return out;
		}

		out.bic = bestBic;
		out.bicMargin = (runnerUpBic < 1e29) ? (runnerUpBic - bestBic) : (bicStatic - bestBic);
		out.chi2 = cands[bestIdx].chi2;
		out.modelDof = cands[bestIdx].dofParams;

		if (bestIdx == 0)
		{
			out.type = TrajectoryType::LinearGlide;
			out.p0x = glP0x;
			out.p0y = glP0y;
			out.vxSec = glVx;
			out.vySec = glVy;
			out.axSec2 = glAx;
			out.aySec2 = glAy;
			out.nextDispX = glVx * pairSeconds + 0.5 * glAx * pairSeconds * pairSeconds;
			out.nextDispY = glVy * pairSeconds + 0.5 * glAy * pairSeconds * pairSeconds;
		}
		else if (bestIdx == 1)
		{
			out.type = TrajectoryType::Ballistic;
			out.p0x = balP0x;
			out.p0y = balP0y;
			out.vxSec = balVx;
			out.vySec = balVy;
			out.axSec2 = balAx;
			out.aySec2 = balAy;
			out.nextDispX = balVx * pairSeconds + 0.5 * balAx * pairSeconds * pairSeconds;
			out.nextDispY = balVy * pairSeconds + 0.5 * balAy * pairSeconds * pairSeconds;
		}
		else if (bestIdx == 2)
		{
			out.type = TrajectoryType::Impulse;
			out.hasImpulse = true;
			out.impulseTauSec = bestImp.tauS;
			out.impulseX = bestImp.psX;
			out.impulseY = bestImp.psY;
			out.vMinusX = bestImp.vMinusX;
			out.vMinusY = bestImp.vMinusY;
			out.vPlusX = bestImp.vPlusX;
			out.vPlusY = bestImp.vPlusY;
			out.axSec2 = 0.0;
			out.aySec2 = bestImp.sharedAy;

			double p0x = 0.0, p0y = 0.0, v0x = 0.0, v0y = 0.0;
			double p1x = 0.0, p1y = 0.0, v1x = 0.0, v1y = 0.0;
			EvalImpulseAt(bestImp, 0.0, p0x, p0y, v0x, v0y);
			EvalImpulseAt(bestImp, pairSeconds, p1x, p1y, v1x, v1y);
			out.p0x = p0x;
			out.p0y = p0y;
			out.vxSec = v0x;
			out.vySec = v0y;
			out.nextDispX = p1x - p0x;
			out.nextDispY = p1y - p0y;
		}
		else
		{
			out.type = persp.type;
			out.usedPerspectiveDepth = true;
			out.p0x = persp.p0x;
			out.p0y = persp.p0y;
			out.vxSec = persp.vxSec;
			out.vySec = persp.vySec;
			out.axSec2 = persp.axSec2;
			out.aySec2 = persp.aySec2;
			out.nextDispX = persp.nextDispX;
			out.nextDispY = persp.nextDispY;
			out.radiusAtZero = persp.radiusAtZero;
			out.radiusRatePerSec = persp.radiusRatePerSec;
		}

		EvaluateMotionBlurCoupling(samples, n, out, out.blurAlignFactor, out.shutterEst, out.blurStretched);
		return out;
	}

	// Exact O(N^3) Hungarian algorithm (Kuhn-Munkres shortest-augmenting-path potentials, zero heap allocs).
	void SolveHungarian32(
		int nRows,
		int nCols,
		const double cost[kMaxTracksHardCap][kMaxBlobs],
		double forbiddenCost,
		int outRowToCol[kMaxTracksHardCap])
	{
		for (int r = 0; r < kMaxTracksHardCap; ++r)
			outRowToCol[r] = -1;
		if (nRows <= 0 || nCols <= 0)
			return;
		const int rows = std::min(nRows, static_cast<int>(kMaxTracksHardCap));
		const int cols = std::min(nCols, static_cast<int>(kMaxBlobs));
		// Pad to square dimension N = max(rows, cols) with dummy edges of cost = forbiddenCost.
		const int dim = std::max(rows, cols);
		constexpr int kMaxDim = 33; // 1-indexed [1..32]

		double u[kMaxDim] = {};
		double v[kMaxDim] = {};
		int p[kMaxDim] = {};
		int way[kMaxDim] = {};
		double minv[kMaxDim] = {};
		bool used[kMaxDim] = {};

		for (int i = 1; i <= dim; ++i)
		{
			p[0] = i;
			int j0 = 0;
			for (int j = 0; j <= dim; ++j)
			{
				minv[j] = 1e30;
				used[j] = false;
			}
			do
			{
				used[j0] = true;
				const int i0 = p[j0];
				double delta = 1e30;
				int j1 = 0;
				for (int j = 1; j <= dim; ++j)
				{
					if (used[j])
						continue;
					const double cval = (i0 <= rows && j <= cols) ? cost[i0 - 1][j - 1] : forbiddenCost;
					const double cur = cval - u[i0] - v[j];
					if (cur < minv[j])
					{
						minv[j] = cur;
						way[j] = j0;
					}
					if (minv[j] < delta)
					{
						delta = minv[j];
						j1 = j;
					}
				}
				for (int j = 0; j <= dim; ++j)
				{
					if (used[j])
					{
						u[p[j]] += delta;
						v[j] -= delta;
					}
					else
					{
						minv[j] -= delta;
					}
				}
				j0 = j1;
			} while (p[j0] != 0);

			do
			{
				const int j1 = way[j0];
				p[j0] = p[j1];
				j0 = j1;
			} while (j0 != 0);
		}

		for (int j = 1; j <= cols; ++j)
		{
			const int r = p[j] - 1;
			if (r >= 0 && r < rows && cost[r][j - 1] < forbiddenCost)
				outRowToCol[r] = j - 1;
		}
	}

	// =========================================================================
	// TrajectoryTracker
	// =========================================================================
	TrajectoryTracker::TrajectoryTracker()
	{
		ResetSession();
	}

	void TrajectoryTracker::ResetScene()
	{
		ringCount_ = 0;
		latestFrameIndex_ = 0;
		hasLatestFrame_ = false;
		for (int i = 0; i < kWindowRingCapacity; ++i)
			ring_[i] = FrameObservation{};
		for (uint32_t i = 0; i < kMaxTracksHardCap; ++i)
			tracks_[i] = TrackRecord{};
		std::memset(hotspotHeat_, 0, sizeof(hotspotHeat_));
		lastHotspotSuppressed_ = 0;
	}

	void TrajectoryTracker::ResetSession()
	{
		ResetScene();
		sessionPrior_.Reset();
		nextTrackId_ = 1;
	}

	int TrajectoryTracker::FindRingSlot(uint64_t frameIndex) const
	{
		for (int i = 0; i < ringCount_; ++i)
		{
			if (ring_[i].valid && ring_[i].frameIndex == frameIndex)
				return i;
		}
		return -1;
	}

	bool TrajectoryTracker::ComposeHomographyBetween(
		uint64_t fromFrame, uint64_t toFrame,
		CameraModel::Params& outH, double& outAccumCamVarPx2) const
	{
		outH = IdentityHomography();
		outAccumCamVarPx2 = 0.0;
		if (fromFrame == toFrame)
			return true;

		if (fromFrame < toFrame)
		{
			for (uint64_t f = fromFrame + 1; f <= toFrame; ++f)
			{
				const int slot = FindRingSlot(f);
				if (slot < 0 || !ring_[slot].camPrevUsable)
					return false;
				bool ok = false;
				outH = ComposeHomography(ring_[slot].camPrevToCur, outH, &ok);
				if (!ok)
					return false;
				outAccumCamVarPx2 += ring_[slot].camSigmaPx * ring_[slot].camSigmaPx;
			}
			return true;
		}

		// fromFrame > toFrame: compose forward from toFrame -> fromFrame, then invert.
		CameraModel::Params fwd = IdentityHomography();
		for (uint64_t f = toFrame + 1; f <= fromFrame; ++f)
		{
			const int slot = FindRingSlot(f);
			if (slot < 0 || !ring_[slot].camPrevUsable)
				return false;
			bool ok = false;
			fwd = ComposeHomography(ring_[slot].camPrevToCur, fwd, &ok);
			if (!ok)
				return false;
			outAccumCamVarPx2 += ring_[slot].camSigmaPx * ring_[slot].camSigmaPx;
		}
		bool invOk = false;
		outH = InvertHomography(fwd, &invOk);
		return invOk;
	}

	void TrajectoryTracker::DecayHotspots(double dtSec, double decaySec)
	{
		if (!(dtSec > 0.0) || !(decaySec > 0.0))
			return;
		const float factor = static_cast<float>(std::exp(-dtSec / decaySec));
		for (int i = 0; i < kHotspotGridCells; ++i)
			hotspotHeat_[i] *= factor;
	}

	void TrajectoryTracker::AddHotspotHeat(float x, float y, const CameraModel::Rect& picture, float amount)
	{
		if (picture.Empty() || !(amount > 0.0f))
			return;
		const float fx = (x - static_cast<float>(picture.left)) / static_cast<float>(picture.Width());
		const float fy = (y - static_cast<float>(picture.top)) / static_cast<float>(picture.Height());
		const int gx = std::min(std::max(static_cast<int>(std::floor(fx * static_cast<float>(kHotspotGridW))), 0), kHotspotGridW - 1);
		const int gy = std::min(std::max(static_cast<int>(std::floor(fy * static_cast<float>(kHotspotGridH))), 0), kHotspotGridH - 1);
		hotspotHeat_[gy * kHotspotGridW + gx] += amount;
	}

	float TrajectoryTracker::HotspotHeatAt(float x, float y, const CameraModel::Rect& picture) const
	{
		if (picture.Empty())
			return 0.0f;
		const float fx = (x - static_cast<float>(picture.left)) / static_cast<float>(picture.Width());
		const float fy = (y - static_cast<float>(picture.top)) / static_cast<float>(picture.Height());
		const int gx = std::min(std::max(static_cast<int>(std::floor(fx * static_cast<float>(kHotspotGridW))), 0), kHotspotGridW - 1);
		const int gy = std::min(std::max(static_cast<int>(std::floor(fy * static_cast<float>(kHotspotGridH))), 0), kHotspotGridH - 1);
		return hotspotHeat_[gy * kHotspotGridW + gx];
	}

	bool TrajectoryTracker::IsHotspotSuppressed(float x, float y, const CameraModel::Rect& picture) const
	{
		return HotspotHeatAt(x, y, picture) >= kHotspotSuppressThreshold;
	}

	void TrajectoryTracker::UpdateSessionPrior(const TrackRecord& trk)
	{
		constexpr double kAlpha = 0.1; // Exponential smoothing rate (10 confirmed updates time constant)
		if (sessionPrior_.updates == 0)
		{
			sessionPrior_.polarityScore = static_cast<double>(trk.polarity);
			sessionPrior_.meanRadiusPx = trk.emaRadiusPx;
			sessionPrior_.minRadiusPx = trk.emaRadiusPx;
			sessionPrior_.maxRadiusPx = trk.emaRadiusPx;
			if (trk.hasChroma)
			{
				sessionPrior_.meanCb = trk.emaCb;
				sessionPrior_.meanCr = trk.emaCr;
				sessionPrior_.hasChroma = true;
			}
		}
		else
		{
			sessionPrior_.polarityScore = (1.0 - kAlpha) * sessionPrior_.polarityScore + kAlpha * static_cast<double>(trk.polarity);
			sessionPrior_.meanRadiusPx = (1.0 - kAlpha) * sessionPrior_.meanRadiusPx + kAlpha * trk.emaRadiusPx;
			sessionPrior_.minRadiusPx = std::fmin(sessionPrior_.minRadiusPx, trk.emaRadiusPx);
			sessionPrior_.maxRadiusPx = std::fmax(sessionPrior_.maxRadiusPx, trk.emaRadiusPx);
			if (trk.hasChroma)
			{
				if (!sessionPrior_.hasChroma)
				{
					sessionPrior_.meanCb = trk.emaCb;
					sessionPrior_.meanCr = trk.emaCr;
					sessionPrior_.hasChroma = true;
				}
				else
				{
					sessionPrior_.meanCb = (1.0 - kAlpha) * sessionPrior_.meanCb + kAlpha * trk.emaCb;
					sessionPrior_.meanCr = (1.0 - kAlpha) * sessionPrior_.meanCr + kAlpha * trk.emaCr;
				}
			}
		}
		++sessionPrior_.updates;
	}

	double TrajectoryTracker::SessionPriorBonus(const Config& cfg, const BlobCandidate& b) const
	{
		if (!cfg.enableSessionPrior || sessionPrior_.updates < 3)
			return 0.0;
		double score = 0.0;
		if (b.polarity != 0 && std::fabs(sessionPrior_.polarityScore) > 0.5)
		{
			if ((b.polarity > 0 && sessionPrior_.polarityScore > 0.0) ||
				(b.polarity < 0 && sessionPrior_.polarityScore < 0.0))
				score += 0.25;
		}
		if (b.radiusPx >= 0.5f * static_cast<float>(sessionPrior_.minRadiusPx) &&
			b.radiusPx <= 2.0f * static_cast<float>(sessionPrior_.maxRadiusPx))
		{
			score += 0.25;
		}
		return score; // Bounded <= 0.5 chi^2 units: soft tie-breaker only, never hard-blocks any object.
	}

	bool TrajectoryTracker::HasActiveOrCoastingTracks() const
	{
		for (uint32_t i = 0; i < kMaxTracksHardCap; ++i)
		{
			if (tracks_[i].active)
				return true;
		}
		return false;
	}

	void TrajectoryTracker::PushFrame(const Config& cfg, const FrameObservation& obs)
	{
		if (!obs.valid)
			return;
		if (hasLatestFrame_ && obs.frameIndex <= latestFrameIndex_)
		{
			// Out-of-order or reset stream.
			ResetScene();
		}

		if (cfg.enableHotspotAutoMask && obs.dtFromPrevSec > 0.0)
			DecayHotspots(obs.dtFromPrevSec, cfg.hotspotDecaySec);

		// Insert into ordered fixed-capacity ring (evicting oldest when full).
		int slot = 0;
		if (ringCount_ < kWindowRingCapacity)
		{
			slot = ringCount_++;
		}
		else
		{
			for (int i = 0; i < kWindowRingCapacity - 1; ++i)
				ring_[i] = ring_[i + 1];
			slot = kWindowRingCapacity - 1;
		}
		ring_[slot] = obs;
		latestFrameIndex_ = obs.frameIndex;
		hasLatestFrame_ = true;

		// Repeat frame (dt == 0): do not advance kinematic prediction or add duplicate zero-dt samples.
		if (ringCount_ > 1 && !(obs.dtFromPrevSec > 0.0))
			return;

		AssociateLatestFrame(cfg, slot);
		AttemptImpulseStitching(cfg, slot);
		PruneExpiredTracks(cfg);
	}

	void TrajectoryTracker::ConfirmTrackIncrementally(const Config& cfg, int latestSlot, TrackRecord& trk)
	{
		if (!trk.active || trk.historyCount < 3)
			return;
		const FrameObservation& curObs = ring_[latestSlot];
		const double curCx = curObs.picture.Empty() ? 0.0 : curObs.picture.CentreX();
		const double curCy = curObs.picture.Empty() ? 0.0 : curObs.picture.CentreY();

		WindowSample win[kWindowRingCapacity];
		int winCount = 0;
		for (int h = 0; h < trk.historyCount && winCount < kWindowRingCapacity; ++h)
		{
			const TrackPoint& pt = trk.history[h];
			CameraModel::Params hToCur;
			double camVar = 0.0;
			if (!ComposeHomographyBetween(pt.frameIndex, curObs.frameIndex, hToCur, camVar))
				continue;
			const int rSlot = FindRingSlot(pt.frameIndex);
			const double pCx = (rSlot >= 0 && !ring_[rSlot].picture.Empty()) ? ring_[rSlot].picture.CentreX() : curCx;
			const double pCy = (rSlot >= 0 && !ring_[rSlot].picture.Empty()) ? ring_[rSlot].picture.CentreY() : curCy;
			const double relX = static_cast<double>(pt.blob.cx) - pCx;
			const double relY = static_cast<double>(pt.blob.cy) - pCy;
			double mx = 0.0, my = 0.0;
			CameraModel::Apply(hToCur, relX, relY, &mx, &my);
			double jac[4];
			HomographyJacobian(hToCur, relX, relY, jac);
			const double sig = CramerRaoCentroidSigmaPx(pt.blob);
			const double c2 = sig * sig;

			WindowSample& ws = win[winCount++];
			ws.tauSec = pt.timestampSec - curObs.timestampSec;
			ws.dtStepSec = pt.dtFromPrevSec;
			ws.x = mx + curCx;
			ws.y = my + curCy;
			ws.covXX = c2 + camVar * (jac[0] * jac[0] + jac[1] * jac[1]);
			ws.covXY = camVar * (jac[0] * jac[2] + jac[1] * jac[3]);
			ws.covYY = c2 + camVar * (jac[2] * jac[2] + jac[3] * jac[3]);
			ws.radiusPx = static_cast<double>(pt.blob.radiusPx);
			ws.streakLenPx = static_cast<double>(pt.blob.streakLenPx);
			ws.streakDirX = std::cos(static_cast<double>(pt.blob.streakAngleRad));
			ws.streakDirY = std::sin(static_cast<double>(pt.blob.streakAngleRad));
			ws.intSnr = static_cast<double>(pt.blob.intSnr);
			ws.overlapsMask = pt.blob.overlapsOverlay || pt.blob.overlapsWater;
			ws.nearLargeObject = pt.blob.nearLargeObject;
		}
		if (winCount < 3)
			return;
		const double dtStep = (curObs.dtFromPrevSec > 0.0) ? curObs.dtFromPrevSec : (1.0 / 60.0);
		const TrajectoryClassification cls = ClassifyTrajectoryWindow(cfg, win, winCount, dtStep, curObs.picture);
		if (cls.type != TrajectoryType::Clutter && cls.type != TrajectoryType::Unknown)
		{
			trk.confirmed = true;
			trk.lastType = cls.type;
		}
	}

	void TrajectoryTracker::PruneExpiredTracks(const Config& cfg)
	{
		const CameraModel::Rect latestPic = (ringCount_ > 0) ? ring_[ringCount_ - 1].picture : CameraModel::Rect{};
		for (uint32_t i = 0; i < kMaxTracksHardCap; ++i)
		{
			TrackRecord& trk = tracks_[i];
			if (!trk.active || trk.coastFrames == 0)
				continue;
			// Unconfirmed 1-point seeds expire immediately on miss; unconfirmed 2-point seeds survive
			// 2 frames so AttemptImpulseStitching can bridge a bounce across frames (k, k+1);
			// confirmed tracks coast up to cfg.coastMaxSec (default 200 ms).
			const bool expire = (!trk.confirmed && trk.historyCount < 2) ||
				(!trk.confirmed && trk.coastFrames > 2) ||
				(trk.coastElapsedSec > cfg.coastMaxSec);
			if (expire)
			{
				// Repeatedly aborted unconfirmed clutter births in the same spatial cell (e.g. flashing/oscillating
				// LED ad boards) heat the 64x36 clutter-hotspot auto-mask without ever touching confirmed balls.
				if (cfg.enableHotspotAutoMask && !trk.confirmed && trk.historyCount > 0 && !latestPic.Empty())
				{
					AddHotspotHeat(trk.history[0].blob.cx, trk.history[0].blob.cy, latestPic, 0.5f);
				}
				trk = TrackRecord{};
			}
		}
	}

	void TrajectoryTracker::AssociateLatestFrame(const Config& cfg, int latestSlot)
	{
		const FrameObservation& curObs = ring_[latestSlot];
		const uint32_t blobCount = std::min(curObs.blobCount, kMaxBlobs);
		const double picW = curObs.picture.Empty() ? 1920.0 : static_cast<double>(curObs.picture.Width());
		const double picH = curObs.picture.Empty() ? 1080.0 : static_cast<double>(curObs.picture.Height());
		const double maxSpeedPxSec = cfg.maxSpeedWidthFracPerSec * picW;
		const double maneuverSigmaA = kManeuverAccelHeightFracPerSec2 * picH;
		const double curCx = curObs.picture.Empty() ? 0.0 : curObs.picture.CentreX();
		const double curCy = curObs.picture.Empty() ? 0.0 : curObs.picture.CentreY();

		// Gather active track indices.
		int activeTrackSlots[kMaxTracksHardCap];
		int activeCount = 0;
		for (uint32_t i = 0; i < kMaxTracksHardCap; ++i)
		{
			if (tracks_[i].active)
				activeTrackSlots[activeCount++] = static_cast<int>(i);
		}

		double cost[kMaxTracksHardCap][kMaxBlobs];
		double kinCost[kMaxTracksHardCap][kMaxBlobs];
		double predX[kMaxTracksHardCap] = {};
		double predY[kMaxTracksHardCap] = {};
		double predStd[kMaxTracksHardCap] = {};
		bool predValid[kMaxTracksHardCap] = {};

		for (int m = 0; m < kMaxTracksHardCap; ++m)
		{
			for (int k = 0; k < static_cast<int>(kMaxBlobs); ++k)
			{
				cost[m][k] = kForbiddenCost;
				kinCost[m][k] = kForbiddenCost;
			}
		}

		// Build predicted position & innovation covariance S for every active track in current frame coords.
		for (int m = 0; m < activeCount; ++m)
		{
			const TrackRecord& trk = tracks_[activeTrackSlots[m]];
			if (trk.historyCount <= 0)
				continue;

			const TrackPoint& lastPt = trk.history[trk.historyCount - 1];
			const double dtSinceLast = curObs.timestampSec - lastPt.timestampSec;
			if (!(dtSinceLast > 0.0))
				continue;

			CameraModel::Params hLastToCur;
			double camVarLast = 0.0;
			if (!ComposeHomographyBetween(lastPt.frameIndex, curObs.frameIndex, hLastToCur, camVarLast))
				continue;

			const int lastRingSlot = FindRingSlot(lastPt.frameIndex);
			const double lastPicCx = (lastRingSlot >= 0 && !ring_[lastRingSlot].picture.Empty()) ? ring_[lastRingSlot].picture.CentreX() : curCx;
			const double lastPicCy = (lastRingSlot >= 0 && !ring_[lastRingSlot].picture.Empty()) ? ring_[lastRingSlot].picture.CentreY() : curCy;

			double lMappedX = 0.0, lMappedY = 0.0;
			CameraModel::Apply(hLastToCur, lastPt.blob.cx - lastPicCx, lastPt.blob.cy - lastPicCy, &lMappedX, &lMappedY);
			lMappedX += curCx;
			lMappedY += curCy;

			double px = lMappedX;
			double py = lMappedY;
			double sVar = CramerRaoCentroidSigmaPx(lastPt.blob);
			sVar = sVar * sVar + camVarLast;
			bool isSeedSinglePoint = (trk.historyCount == 1);

			// Optional 2.5D pinhole projective prediction (under r(t) ~ 1/Z(t), u(t) = 1/r(t) and q(t)/r(t) are linear).
			bool hasProjPred = false;
			double projP3x = 0.0, projP3y = 0.0; // Predicted projective 3D coord q(t)/r(t) at current frame
			double uPred = 0.0;
			double uPredVar = 0.0;
			double projExtraVar = 0.0;
			double rPred = trk.emaRadiusPx;

			if (trk.historyCount >= 2)
			{
				const TrackPoint& prevPt = trk.history[trk.historyCount - 2];
				const double dtPrev = lastPt.timestampSec - prevPt.timestampSec;
				CameraModel::Params hPrevToCur;
				double camVarPrev = 0.0;
				if (dtPrev > 0.0 && ComposeHomographyBetween(prevPt.frameIndex, curObs.frameIndex, hPrevToCur, camVarPrev))
				{
					const int prevRingSlot = FindRingSlot(prevPt.frameIndex);
					const double prevPicCx = (prevRingSlot >= 0 && !ring_[prevRingSlot].picture.Empty()) ? ring_[prevRingSlot].picture.CentreX() : curCx;
					const double prevPicCy = (prevRingSlot >= 0 && !ring_[prevRingSlot].picture.Empty()) ? ring_[prevRingSlot].picture.CentreY() : curCy;
					double pMappedX = 0.0, pMappedY = 0.0;
					CameraModel::Apply(hPrevToCur, prevPt.blob.cx - prevPicCx, prevPt.blob.cy - prevPicCy, &pMappedX, &pMappedY);
					pMappedX += curCx;
					pMappedY += curCy;

					double vx = (lMappedX - pMappedX) / dtPrev;
					double vy = (lMappedY - pMappedY) / dtPrev;
					double ax = 0.0, ay = 0.0;

					// Use parabolic prediction when track has >= 3 points and was classified Ballistic.
					if (trk.historyCount >= 3 && trk.lastType == TrajectoryType::Ballistic)
					{
						const TrackPoint& prev2Pt = trk.history[trk.historyCount - 3];
						const double dtPrev2 = prevPt.timestampSec - prev2Pt.timestampSec;
						CameraModel::Params hPrev2ToCur;
						double camVarPrev2 = 0.0;
						if (dtPrev2 > 0.0 && ComposeHomographyBetween(prev2Pt.frameIndex, curObs.frameIndex, hPrev2ToCur, camVarPrev2))
						{
							const int p2Slot = FindRingSlot(prev2Pt.frameIndex);
							const double p2Cx = (p2Slot >= 0 && !ring_[p2Slot].picture.Empty()) ? ring_[p2Slot].picture.CentreX() : curCx;
							const double p2Cy = (p2Slot >= 0 && !ring_[p2Slot].picture.Empty()) ? ring_[p2Slot].picture.CentreY() : curCy;
							double p2Mx = 0.0, p2My = 0.0;
							CameraModel::Apply(hPrev2ToCur, prev2Pt.blob.cx - p2Cx, prev2Pt.blob.cy - p2Cy, &p2Mx, &p2My);
							p2Mx += curCx;
							p2My += curCy;
							const double vxOld = (pMappedX - p2Mx) / dtPrev2;
							const double vyOld = (pMappedY - p2My) / dtPrev2;
							const double dtMid = 0.5 * (dtPrev + dtPrev2);
							ax = (vx - vxOld) / dtMid;
							ay = (vy - vyOld) / dtMid;
						}
					}

					px = lMappedX + vx * dtSinceLast + 0.5 * ax * dtSinceLast * dtSinceLast;
					py = lMappedY + vy * dtSinceLast + 0.5 * ay * dtSinceLast * dtSinceLast;

					// Exact Gaussian error propagation + quadratic maneuver Taylor remainder.
					const double sigPrev = CramerRaoCentroidSigmaPx(prevPt.blob);
					const double ratio = dtSinceLast / dtPrev;
					const double velPropVar = ratio * ratio * (sVar + sigPrev * sigPrev + camVarPrev);
					const double procStd = 0.5 * maneuverSigmaA * dtSinceLast * (dtPrev + dtSinceLast);
					sVar = sVar + velPropVar + procStd * procStd;

					// 2.5D pinhole projective prediction: under r(t) ~ 1/Z(t), u(t) = 1/r(t) and P(t) = q(t)/r(t) are linear.
					if (cfg.enablePerspectiveDepth && lastPt.blob.radiusPx > 0.5f && prevPt.blob.radiusPx > 0.5f)
					{
						const double rLast = static_cast<double>(lastPt.blob.radiusPx);
						const double rPrev = static_cast<double>(prevPt.blob.radiusPx);
						const double uLast = 1.0 / rLast;
						const double uPrev = 1.0 / rPrev;
						const double uDot = (uLast - uPrev) / dtPrev;
						const double uP = uLast + uDot * dtSinceLast;
						if (uP > 0.25 * uLast && uP < 4.0 * uLast)
						{
							const double qLastX = lMappedX - curCx;
							const double qLastY = lMappedY - curCy;
							const double qPrevX = pMappedX - curCx;
							const double qPrevY = pMappedY - curCy;
							const double pLastX = qLastX * uLast;
							const double pLastY = qLastY * uLast;
							const double pPrevX = qPrevX * uPrev;
							const double pPrevY = qPrevY * uPrev;
							projP3x = pLastX + (pLastX - pPrevX) * ratio;
							projP3y = pLastY + (pLastY - pPrevY) * ratio;
							uPred = uP;
							rPred = 1.0 / uP;
							const double varULast = 0.5 / (rLast * rLast * rLast * rLast);
							const double varUPrev = 0.5 / (rPrev * rPrev * rPrev * rPrev);
							uPredVar = varULast + ratio * ratio * (varULast + varUPrev);
							// Tight projective prediction variance (only scales with local step variance, never full frame offset).
							const double stepDisp = std::hypot(lMappedX - pMappedX, lMappedY - pMappedY);
							const double relRStd = std::sqrt(0.5) / std::fmax(rLast, 1.0);
							const double projPosStd = std::fmax(1.0, relRStd * stepDisp);
							projExtraVar = projPosStd * projPosStd;
							hasProjPred = true;
						}
					}
				}
				else
				{
					isSeedSinglePoint = true;
				}
			}

			predX[m] = px;
			predY[m] = py;
			predStd[m] = std::sqrt(std::fmax(sVar, 1e-6));
			predValid[m] = true;

			for (uint32_t k = 0; k < blobCount; ++k)
			{
				const BlobCandidate& b = curObs.blobs[k];

				// Polarity veto (unless isoluminant or background luma crossed object luma).
				if (trk.polarity != 0 && b.polarity != 0 && trk.polarity != b.polarity)
				{
					const double bgShift = std::fabs(static_cast<double>(b.meanBgLuma) - trk.lastBgLuma);
					const double avgContrast = std::fabs(trk.emaFlux) / std::fmax(1.0, static_cast<double>(b.areaHalfPx));
					if (!(bgShift > 0.5 * avgContrast))
						continue;
				}

				const double sigObs = CramerRaoCentroidSigmaPx(b);
				const double varR = sigObs * sigObs + 1.0; // Half-res discretization variance
				const double dx = static_cast<double>(b.cx) - px;
				const double dy = static_cast<double>(b.cy) - py;
				const double dist2 = dx * dx + dy * dy;

				double dKin2 = 0.0;
				bool usedProjGate = false;
				if (isSeedSinglePoint)
				{
					// Birth seed pairing within derived speed bound v_max = 2.0 * W_pic px/s.
					const double maxJump = maxSpeedPxSec * dtSinceLast;
					if (dist2 > maxJump * maxJump)
						continue;

					// If either blob is motion-blur elongated, require displacement direction to align with streak axis!
					if (dist2 > 4.0)
					{
						const double dist = std::sqrt(dist2);
						const double ux = dx / dist;
						const double uy = dy / dist;
						auto checkStreakSeed = [&](const BlobCandidate& sb) -> bool
						{
							if (!(sb.streakLenPx > sb.radiusPx))
								return true;
							const double ex = std::cos(static_cast<double>(sb.streakAngleRad));
							const double ey = std::sin(static_cast<double>(sb.streakAngleRad));
							const double dot = std::fabs(ux * ex + uy * ey);
							return dot >= 0.5; // Within 60 deg (exact cos(pi/3) = 1/2)
						};
						if (!checkStreakSeed(b) || !checkStreakSeed(lastPt.blob))
							continue;
					}
					dKin2 = dist2 / std::fmax(maxJump * maxJump / kChi2Dof2_99, 1.0);
				}
				else
				{
					const double totalVar = sVar + sigObs * sigObs;
					dKin2 = dist2 / std::fmax(totalVar, 1e-6);

					// Check 2.5D pinhole projective gate when radius is evolving consistently with approaching/receding motion.
					if (hasProjPred && b.radiusPx > 0.5f)
					{
						const double rObs = static_cast<double>(b.radiusPx);
						const double uObs = 1.0 / rObs;
						const double varUObs = 0.5 / (rObs * rObs * rObs * rObs);
						const double du = uObs - uPred;
						if (du * du <= 9.0 * (uPredVar + varUObs))
						{
							// Evaluate projective prediction using candidate's own radius (and predicted radius), taking tighter fit.
							const double p1x = curCx + projP3x * rObs;
							const double p1y = curCy + projP3y * rObs;
							const double p2x = curCx + projP3x * rPred;
							const double p2y = curCy + projP3y * rPred;
							const double d1 = (static_cast<double>(b.cx) - p1x) * (static_cast<double>(b.cx) - p1x) +
								(static_cast<double>(b.cy) - p1y) * (static_cast<double>(b.cy) - p1y);
							const double d2 = (static_cast<double>(b.cx) - p2x) * (static_cast<double>(b.cx) - p2x) +
								(static_cast<double>(b.cy) - p2y) * (static_cast<double>(b.cy) - p2y);
							const double pVar = sVar + projExtraVar + sigObs * sigObs;
							const double pKin2 = std::fmin(d1, d2) / std::fmax(pVar, 1e-6);
							if (pKin2 < dKin2)
							{
								dKin2 = pKin2;
								usedProjGate = true;
							}
						}
					}
					if (dKin2 > kChi2Dof2_99)
						continue;
				}

				// Conserved flux term: luma flux for luma-dominant balls; chroma flux for isoluminant chroma balls.
				// Note: for approaching/receding 2.5D balls, flux scales with area ~ r^2, so normalize by r^2 when usedProjGate.
				double dFlux2 = 0.0;
				const bool lumaReliable = (trk.polarity != 0 && b.polarity != 0);
				double obsFluxMag = 0.0;
				double trkFluxMag = 0.0;
				if (lumaReliable)
				{
					obsFluxMag = std::fabs(static_cast<double>(b.flux));
					trkFluxMag = std::fabs(trk.emaFlux);
				}
				else if (trk.hasChroma && b.hasChroma)
				{
					obsFluxMag = static_cast<double>(b.chromaFlux);
					trkFluxMag = trk.emaChromaFlux;
				}
				if (obsFluxMag > 1e-6 && trkFluxMag > 1e-6)
				{
					if (usedProjGate && b.radiusPx > 0.5f && trk.emaRadiusPx > 0.5)
					{
						const double rScale = trk.emaRadiusPx / static_cast<double>(b.radiusPx);
						obsFluxMag *= (rScale * rScale);
					}
					const double lnDiff = std::log(obsFluxMag) - std::log(trkFluxMag);
					const double invSnr2 = 1.0 / std::fmax(1.0, static_cast<double>(b.intSnr * b.intSnr));
					const double varLnFlux = 2.0 * invSnr2 + 2.0 / std::fmax(1.0, static_cast<double>(b.areaHalfPx));
					dFlux2 = (lnDiff * lnDiff) / std::fmax(varLnFlux, 0.04);
					// Reject extreme flux mismatch (> 4 sigma in log-flux).
					if (dFlux2 > 16.0)
						continue;
				}

				// Minor-axis radius invariance term (uses projective predicted radius when depth is changing).
				const double refRadius = (hasProjPred && usedProjGate) ? rPred : trk.emaRadiusPx;
				const double dr = static_cast<double>(b.radiusPx) - refRadius;
				const double dRad2 = (dr * dr) / varR;
				if (dRad2 > 16.0)
					continue;

				// Optional chroma contrast distance.
				double dChroma2 = 0.0;
				if (trk.hasChroma && b.hasChroma && b.chromaSigma > 1e-6f)
				{
					const double dcb = static_cast<double>(b.cbContrast) - trk.emaCb;
					const double dcr = static_cast<double>(b.crContrast) - trk.emaCr;
					const double cVar = 2.0 * static_cast<double>(b.chromaSigma * b.chromaSigma);
					dChroma2 = (dcb * dcb + dcr * dcr) / cVar;
				}

				kinCost[m][k] = isSeedSinglePoint ? 0.0 : dKin2;
				cost[m][k] = std::fmax(0.0, dKin2 + dFlux2 + dRad2 + dChroma2 - SessionPriorBonus(cfg, b));
			}
		}

		// Exact Hungarian Global Nearest Neighbor assignment.
		int assignment[kMaxTracksHardCap];
		SolveHungarian32(activeCount, static_cast<int>(blobCount), cost, kForbiddenCost, assignment);

		// Crossing-ball ambiguity check (spec C.3):
		// Flag both tracks Uncertain if an assigned blob falls inside another track's gate with < 2 sigma (4.0) separation,
		// OR two confirmed tracks' predicted positions cross within r_1 + r_2 + 2*sigma_pred.
		bool crossingFlag[kMaxTracksHardCap] = {};
		for (int m1 = 0; m1 < activeCount; ++m1)
		{
			const int k1 = assignment[m1];
			if (k1 >= 0)
			{
				for (int m2 = 0; m2 < activeCount; ++m2)
				{
					if (m1 == m2)
						continue;
					if (kinCost[m2][k1] <= kChi2Dof2_99 && std::fabs(cost[m2][k1] - cost[m1][k1]) < 4.0)
					{
						crossingFlag[m1] = true;
						crossingFlag[m2] = true;
					}
				}
			}
			for (int m2 = m1 + 1; m2 < activeCount; ++m2)
			{
				if (!predValid[m1] || !predValid[m2])
					continue;
				const TrackRecord& t1 = tracks_[activeTrackSlots[m1]];
				const TrackRecord& t2 = tracks_[activeTrackSlots[m2]];
				if (t1.historyCount < 2 || t2.historyCount < 2)
					continue;
				const double dx = predX[m1] - predX[m2];
				const double dy = predY[m1] - predY[m2];
				const double sep = std::sqrt(dx * dx + dy * dy);
				const double mergeDist = t1.emaRadiusPx + t2.emaRadiusPx + 2.0 * std::fmax(predStd[m1], predStd[m2]);
				if (sep <= mergeDist)
				{
					crossingFlag[m1] = true;
					crossingFlag[m2] = true;
				}
			}
		}

		bool blobUsed[kMaxBlobs] = {};
		for (int m = 0; m < activeCount; ++m)
		{
			TrackRecord& trk = tracks_[activeTrackSlots[m]];
			const int k = assignment[m];
			if (k >= 0 && k < static_cast<int>(blobCount))
			{
				blobUsed[k] = true;
				// If a confirmed track hits a crossing-ball ambiguity (merged/overlapping blob),
				// coast cleanly across the 1 ambiguous frame so the contaminated merged centroid/flux
				// never poisons velocity or causes post-crossing track loss / ID swaps!
				if (crossingFlag[m] && trk.confirmed)
				{
					++trk.coastFrames;
					trk.coastElapsedSec += std::fmax(0.0, curObs.dtFromPrevSec);
					continue;
				}
				const BlobCandidate& b = curObs.blobs[k];
				trk.coastFrames = 0;
				trk.coastElapsedSec = 0.0;
				trk.lastNis = kinCost[m][k];

				if (trk.historyCount >= kWindowRingCapacity)
				{
					for (int h = 0; h < kWindowRingCapacity - 1; ++h)
						trk.history[h] = trk.history[h + 1];
					trk.historyCount = kWindowRingCapacity - 1;
				}
				TrackPoint& pt = trk.history[trk.historyCount++];
				pt.frameIndex = curObs.frameIndex;
				pt.timestampSec = curObs.timestampSec;
				pt.dtFromPrevSec = curObs.dtFromPrevSec;
				pt.blob = b;
				pt.nis = kinCost[m][k];
				pt.crossingAmbiguous = crossingFlag[m];
				pt.valid = true;

				// Update appearance EMAs.
				constexpr double kAppAlpha = 0.25;
				if (b.polarity != 0)
					trk.polarity = b.polarity;
				trk.emaFlux = (1.0 - kAppAlpha) * trk.emaFlux + kAppAlpha * static_cast<double>(b.flux);
				trk.emaRadiusPx = (1.0 - kAppAlpha) * trk.emaRadiusPx + kAppAlpha * static_cast<double>(b.radiusPx);
				trk.lastBgLuma = static_cast<double>(b.meanBgLuma);
				if (b.hasChroma)
				{
					if (!trk.hasChroma)
					{
						trk.emaCb = static_cast<double>(b.cbContrast);
						trk.emaCr = static_cast<double>(b.crContrast);
						trk.emaChromaFlux = static_cast<double>(b.chromaFlux);
						trk.hasChroma = true;
					}
					else
					{
						trk.emaCb = (1.0 - kAppAlpha) * trk.emaCb + kAppAlpha * static_cast<double>(b.cbContrast);
						trk.emaCr = (1.0 - kAppAlpha) * trk.emaCr + kAppAlpha * static_cast<double>(b.crContrast);
						trk.emaChromaFlux = (1.0 - kAppAlpha) * trk.emaChromaFlux + kAppAlpha * static_cast<double>(b.chromaFlux);
					}
				}

				// Incrementally confirm/classify tracks during PushFrame once historyCount >= 3
				// so valid tracks survive bounces and occlusions across look-ahead windows!
				if (trk.historyCount >= 3)
					ConfirmTrackIncrementally(cfg, latestSlot, trk);
			}
			else
			{
				// Unassociated track: increment coast counters; pruning runs AFTER AttemptImpulseStitching.
				++trk.coastFrames;
				trk.coastElapsedSec += std::fmax(0.0, curObs.dtFromPrevSec);
			}
		}

		// Spawn tentative tracks for unassociated blobs (filtered by Clutter-Hotspot Auto-Mask).
		uint32_t suppressed = 0;
		const uint32_t capTracks = std::min(std::max(cfg.maxTracks, 1u), kMaxTracksHardCap);
		// Reserve internal slots up to kMaxTracksHardCap so tentative seeds do not starve valid tracks.
		for (uint32_t k = 0; k < blobCount; ++k)
		{
			if (blobUsed[k])
				continue;
			const BlobCandidate& b = curObs.blobs[k];
			if (cfg.enableHotspotAutoMask && IsHotspotSuppressed(b.cx, b.cy, curObs.picture))
			{
				++suppressed;
				continue;
			}

			int freeSlot = -1;
			uint32_t liveCount = 0;
			for (uint32_t i = 0; i < kMaxTracksHardCap; ++i)
			{
				if (tracks_[i].active)
					++liveCount;
				else if (freeSlot < 0)
					freeSlot = static_cast<int>(i);
			}
			if (freeSlot < 0 || liveCount >= std::min(kMaxTracksHardCap, capTracks * 2u))
				break;

			TrackRecord& nt = tracks_[freeSlot];
			nt = TrackRecord{};
			nt.id = nextTrackId_++;
			nt.active = true;
			nt.confirmed = false;
			nt.priorMatched = (SessionPriorBonus(cfg, b) > 0.0);
			nt.polarity = b.polarity;
			nt.emaFlux = static_cast<double>(b.flux);
			nt.emaRadiusPx = static_cast<double>(b.radiusPx);
			nt.lastBgLuma = static_cast<double>(b.meanBgLuma);
			if (b.hasChroma)
			{
				nt.emaCb = static_cast<double>(b.cbContrast);
				nt.emaCr = static_cast<double>(b.crContrast);
				nt.emaChromaFlux = static_cast<double>(b.chromaFlux);
				nt.hasChroma = true;
			}
			nt.historyCount = 1;
			nt.history[0].frameIndex = curObs.frameIndex;
			nt.history[0].timestampSec = curObs.timestampSec;
			nt.history[0].dtFromPrevSec = curObs.dtFromPrevSec;
			nt.history[0].blob = b;
			nt.history[0].nis = 0.0;
			nt.history[0].crossingAmbiguous = false;
			nt.history[0].valid = true;
		}
		lastHotspotSuppressed_ = suppressed;
	}

	// Recovers sharp bounce / strike Impulse tracks that momentarily broke across the vertex:
	// stitches a coasting incoming track (>= 2 pts) with an outgoing track (>= 2 pts) when their
	// combined window passes exact Impulse WLS + energy/contact consistency + appearance match.
	void TrajectoryTracker::AttemptImpulseStitching(const Config& cfg, int latestSlot)
	{
		const FrameObservation& curObs = ring_[latestSlot];
		const double curCx = curObs.picture.Empty() ? 0.0 : curObs.picture.CentreX();
		const double curCy = curObs.picture.Empty() ? 0.0 : curObs.picture.CentreY();

		for (uint32_t i = 0; i < kMaxTracksHardCap; ++i)
		{
			TrackRecord& inTrk = tracks_[i];
			if (!inTrk.active || inTrk.historyCount < 2 || inTrk.coastFrames == 0)
				continue;
			const TrackPoint& inLast = inTrk.history[inTrk.historyCount - 1];

			for (uint32_t j = 0; j < kMaxTracksHardCap; ++j)
			{
				if (i == j)
					continue;
				TrackRecord& outTrk = tracks_[j];
				if (!outTrk.active || outTrk.historyCount < 2 || outTrk.coastFrames > 0)
					continue;
				const TrackPoint& outFirst = outTrk.history[0];
				if (outFirst.frameIndex <= inLast.frameIndex || outFirst.frameIndex > inLast.frameIndex + 2)
					continue;

				// Appearance compatibility check (polarity, luma/chroma flux, radius).
				if (inTrk.polarity != 0 && outTrk.polarity != 0 && inTrk.polarity != outTrk.polarity)
					continue;
				if (inTrk.polarity != 0 && outTrk.polarity != 0)
				{
					const double fluxRatio = std::fabs(inTrk.emaFlux) / std::fmax(1e-6, std::fabs(outTrk.emaFlux));
					if (fluxRatio < 0.25 || fluxRatio > 4.0)
						continue;
				}
				else if (inTrk.hasChroma && outTrk.hasChroma && inTrk.emaChromaFlux > 1e-6 && outTrk.emaChromaFlux > 1e-6)
				{
					const double fluxRatio = inTrk.emaChromaFlux / outTrk.emaChromaFlux;
					if (fluxRatio < 0.25 || fluxRatio > 4.0)
						continue;
				}
				if (std::fabs(inTrk.emaRadiusPx - outTrk.emaRadiusPx) > std::fmax(2.0, 0.5 * inTrk.emaRadiusPx))
					continue;

				// Build combined stabilized window in curObs coordinates.
				WindowSample combined[kWindowRingCapacity];
				int combCount = 0;
				auto appendTrackPts = [&](const TrackRecord& tr) -> bool
				{
					for (int h = 0; h < tr.historyCount && combCount < kWindowRingCapacity; ++h)
					{
						const TrackPoint& pt = tr.history[h];
						CameraModel::Params hToCur;
						double camVar = 0.0;
						if (!ComposeHomographyBetween(pt.frameIndex, curObs.frameIndex, hToCur, camVar))
							return false;
						const int rSlot = FindRingSlot(pt.frameIndex);
						const double pCx = (rSlot >= 0 && !ring_[rSlot].picture.Empty()) ? ring_[rSlot].picture.CentreX() : curCx;
						const double pCy = (rSlot >= 0 && !ring_[rSlot].picture.Empty()) ? ring_[rSlot].picture.CentreY() : curCy;
						double mx = 0.0, my = 0.0;
						CameraModel::Apply(hToCur, pt.blob.cx - pCx, pt.blob.cy - pCy, &mx, &my);
						WindowSample& ws = combined[combCount++];
						ws.tauSec = pt.timestampSec - curObs.timestampSec;
						ws.dtStepSec = pt.dtFromPrevSec;
						ws.x = mx + curCx;
						ws.y = my + curCy;
						const double sig = CramerRaoCentroidSigmaPx(pt.blob);
						ws.covXX = sig * sig + camVar;
						ws.covXY = 0.0;
						ws.covYY = sig * sig + camVar;
						ws.radiusPx = pt.blob.radiusPx;
						ws.streakLenPx = pt.blob.streakLenPx;
						ws.intSnr = pt.blob.intSnr;
						ws.overlapsMask = pt.blob.overlapsOverlay || pt.blob.overlapsWater;
						ws.nearLargeObject = pt.blob.nearLargeObject;
					}
					return true;
				};

				if (!appendTrackPts(inTrk) || !appendTrackPts(outTrk) || combCount < 4)
					continue;

				const TrajectoryClassification cls = ClassifyTrajectoryWindow(
					cfg, combined, combCount, std::fmax(curObs.dtFromPrevSec, 1.0 / 60.0), curObs.picture);
				if (cls.type == TrajectoryType::Impulse && cls.hasImpulse)
				{
					// Stitch outTrk onto inTrk while preserving inTrk's persistent ID!
					for (int h = 0; h < outTrk.historyCount; ++h)
					{
						if (inTrk.historyCount >= kWindowRingCapacity)
						{
							for (int s = 0; s < kWindowRingCapacity - 1; ++s)
								inTrk.history[s] = inTrk.history[s + 1];
							inTrk.historyCount = kWindowRingCapacity - 1;
						}
						inTrk.history[inTrk.historyCount++] = outTrk.history[h];
					}
					inTrk.coastFrames = 0;
					inTrk.coastElapsedSec = 0.0;
					inTrk.lastType = TrajectoryType::Impulse;
					inTrk.confirmed = true;
					outTrk = TrackRecord{};
					break;
				}
			}
		}
	}

	bool TrajectoryTracker::ResolveAt(const Config& cfg, uint64_t targetFrameIndex, FastObjectTrackSet& outSet)
	{
		const int targetSlot = FindRingSlot(targetFrameIndex);
		if (targetSlot < 0)
			return false;

		const FrameObservation& targetObs = ring_[targetSlot];
		const bool hasLookAhead = (targetSlot + 1 < ringCount_);
		const bool causalMode = !hasLookAhead || targetObs.causalMode;
		const double targetCx = targetObs.picture.Empty() ? 0.0 : targetObs.picture.CentreX();
		const double targetCy = targetObs.picture.Empty() ? 0.0 : targetObs.picture.CentreY();

		// Inter-frame duration for pair (t, t+1): exact if t+1 is in ring, else fallback to dtFromPrevSec.
		double pairSeconds = targetObs.dtFromPrevSec;
		if (targetSlot + 1 < ringCount_ && ring_[targetSlot + 1].dtFromPrevSec > 0.0)
			pairSeconds = ring_[targetSlot + 1].dtFromPrevSec;
		if (!(pairSeconds > 0.0))
			pairSeconds = 1.0 / 60.0;

		outSet.count = 0;
		outSet.diag.causalMode = causalMode;
		outSet.diag.chromaUsed = targetObs.chromaUsed;
		outSet.diag.hotspotSuppressedBlobs = lastHotspotSuppressed_;
		if (causalMode)
			outSet.abstainReason |= AbstainReason::CausalNoLookAhead;

		const uint32_t maxOutTracks = std::min(std::max(cfg.maxTracks, 1u), kMaxTracksHardCap);
		uint32_t outSrcTrackIdx[kMaxTracksHardCap] = {};

		for (uint32_t ti = 0; ti < kMaxTracksHardCap; ++ti)
		{
			TrackRecord& trk = tracks_[ti];
			if (!trk.active || trk.historyCount <= 0)
				continue;

			// Map track's window points within [target - W_b, target + W_a] into frame t's stabilized plane.
			WindowSample win[kWindowRingCapacity];
			int winCount = 0;
			int targetPtIdx = -1;
			uint64_t lastSeenFrameAtOrBeforeTarget = 0;
			bool hasAtOrBeforeTarget = false;

			for (int h = 0; h < trk.historyCount; ++h)
			{
				const TrackPoint& pt = trk.history[h];
				if (pt.frameIndex + static_cast<uint64_t>(kWindowBackMax) < targetFrameIndex)
					continue;
				if (pt.frameIndex > targetFrameIndex + static_cast<uint64_t>(kWindowAheadMax))
					continue;

				CameraModel::Params hToTarget;
				double camVar = 0.0;
				if (!ComposeHomographyBetween(pt.frameIndex, targetFrameIndex, hToTarget, camVar))
					continue;

				const int pSlot = FindRingSlot(pt.frameIndex);
				const double pCx = (pSlot >= 0 && !ring_[pSlot].picture.Empty()) ? ring_[pSlot].picture.CentreX() : targetCx;
				const double pCy = (pSlot >= 0 && !ring_[pSlot].picture.Empty()) ? ring_[pSlot].picture.CentreY() : targetCy;

				const double relX = static_cast<double>(pt.blob.cx) - pCx;
				const double relY = static_cast<double>(pt.blob.cy) - pCy;
				double mx = 0.0, my = 0.0;
				CameraModel::Apply(hToTarget, relX, relY, &mx, &my);

				double jac[4];
				HomographyJacobian(hToTarget, relX, relY, jac);
				// Transform streak unit axis through local Jacobian.
				const double ex0 = std::cos(static_cast<double>(pt.blob.streakAngleRad));
				const double ey0 = std::sin(static_cast<double>(pt.blob.streakAngleRad));
				double ex = jac[0] * ex0 + jac[1] * ey0;
				double ey = jac[2] * ex0 + jac[3] * ey0;
				const double eNorm = std::sqrt(ex * ex + ey * ey);
				if (eNorm > 1e-6)
				{
					ex /= eNorm;
					ey /= eNorm;
				}
				else
				{
					ex = 1.0;
					ey = 0.0;
				}

				// Propagated measurement covariance R_k = sigma_cent^2 * I + J_H * (camVar * I) * J_H^T.
				const double sigCent = CramerRaoCentroidSigmaPx(pt.blob);
				const double c2 = sigCent * sigCent;
				const double rXX = c2 + camVar * (jac[0] * jac[0] + jac[1] * jac[1]);
				const double rXY = camVar * (jac[0] * jac[2] + jac[1] * jac[3]);
				const double rYY = c2 + camVar * (jac[2] * jac[2] + jac[3] * jac[3]);

				WindowSample& ws = win[winCount];
				ws.tauSec = pt.timestampSec - targetObs.timestampSec;
				ws.dtStepSec = pt.dtFromPrevSec;
				ws.x = mx + targetCx;
				ws.y = my + targetCy;
				ws.covXX = rXX;
				ws.covXY = rXY;
				ws.covYY = rYY;
				ws.radiusPx = static_cast<double>(pt.blob.radiusPx);
				ws.streakLenPx = static_cast<double>(pt.blob.streakLenPx);
				ws.streakDirX = ex;
				ws.streakDirY = ey;
				ws.intSnr = static_cast<double>(pt.blob.intSnr);
				ws.overlapsMask = pt.blob.overlapsOverlay || pt.blob.overlapsWater;
				ws.nearLargeObject = pt.blob.nearLargeObject;

				if (pt.frameIndex == targetFrameIndex)
					targetPtIdx = h;
				if (pt.frameIndex <= targetFrameIndex)
				{
					hasAtOrBeforeTarget = true;
					lastSeenFrameAtOrBeforeTarget = pt.frameIndex;
				}
				++winCount;
			}

			// Ignore tracks that haven't started by targetFrameIndex.
			if (!hasAtOrBeforeTarget)
				continue;

			// Need at least 3 detections in window to confirm and classify cleanly.
			if (winCount < 3)
				continue;

			const TrajectoryClassification cls = ClassifyTrajectoryWindow(cfg, win, winCount, pairSeconds, targetObs.picture);

			if (cls.type == TrajectoryType::Clutter)
			{
				++outSet.diag.clutterRejected;
				if (cfg.enableHotspotAutoMask && (cls.rejectedStationary || cls.rejectedOscillatory || cls.rejectedIncoherent))
				{
					for (int w = 0; w < winCount; ++w)
						AddHotspotHeat(static_cast<float>(win[w].x), static_cast<float>(win[w].y), targetObs.picture, 1.0f);
				}
				// Only retire unconfirmed clutter tracks once a full window (>= 6 frames) at the leading edge fails.
				if (!trk.confirmed && !hasLookAhead && winCount >= 6)
					trk = TrackRecord{};
				continue;
			}

			trk.confirmed = true;
			trk.lastType = cls.type;

			// Is the track currently occluded/coasting at targetFrameIndex?
			const bool isCoastingAtTarget = (targetPtIdx < 0 && lastSeenFrameAtOrBeforeTarget < targetFrameIndex);

			// Transform stabilized next position p(pairSeconds) from frame t back into frame t+1 image coords!
			// Notice: in frame t's stabilized plane, the object moves from p0 to p1 = p0 + nextDisp.
			// In raw image coordinates of frame t+1, that point appears at H_{t -> t+1}(p1), so the raw
			// image-plane motion vector from frame t to frame t+1 is H_{t -> t+1}(p1) - p0!
			double rawVx = cls.nextDispX;
			double rawVy = cls.nextDispY;
			if (targetSlot + 1 < ringCount_ && ring_[targetSlot + 1].camPrevUsable)
			{
				const FrameObservation& nextObs = ring_[targetSlot + 1];
				const double nextCx = nextObs.picture.Empty() ? targetCx : nextObs.picture.CentreX();
				const double nextCy = nextObs.picture.Empty() ? targetCy : nextObs.picture.CentreY();
				const double p1xStab = cls.p0x + cls.nextDispX;
				const double p1yStab = cls.p0y + cls.nextDispY;
				double p1ImgX = 0.0, p1ImgY = 0.0;
				CameraModel::Apply(nextObs.camPrevToCur, p1xStab - targetCx, p1yStab - targetCy, &p1ImgX, &p1ImgY);
				rawVx = (p1ImgX + nextCx) - cls.p0x;
				rawVy = (p1ImgY + nextCy) - cls.p0y;
			}
			else if (targetObs.camPrevUsable)
			{
				// Causal mode (no t+1 camera yet): predict camera displacement from H_{t-1 -> t}.
				const double p1xStab = cls.p0x + cls.nextDispX;
				const double p1yStab = cls.p0y + cls.nextDispY;
				double p1ImgX = 0.0, p1ImgY = 0.0;
				CameraModel::Apply(targetObs.camPrevToCur, p1xStab - targetCx, p1yStab - targetCy, &p1ImgX, &p1ImgY);
				rawVx = (p1ImgX + targetCx) - cls.p0x;
				rawVy = (p1ImgY + targetCy) - cls.p0y;
			}

			// Derived confidence (spec C.3):
			// c = clamp(snrFactor * g_NIS * g_blur * g_age * h_agree, 0, 1).
			const double targetNis = (targetPtIdx >= 0) ? trk.history[targetPtIdx].nis : trk.lastNis;
			const bool targetCrossing = (targetPtIdx >= 0) ? trk.history[targetPtIdx].crossingAmbiguous : false;
			const double gNis = std::exp(-std::fmax(0.0, targetNis - 2.0) * 0.25);
			const double gAge = std::fmin(1.0, static_cast<double>(winCount) / 3.0);
			const double gBlur = cls.blurAlignFactor;
			const double obsIntSnr = (targetPtIdx >= 0) ? static_cast<double>(trk.history[targetPtIdx].blob.intSnr) : 6.0;
			const double snrMargin = std::fmin(1.0, std::fmax(0.0, 1.0 - cfg.growSigma / std::fmax(cfg.growSigma, obsIntSnr)));
			const float baseConfidence = static_cast<float>(std::fmin(1.0, std::fmax(0.0, snrMargin * gNis * gBlur * gAge)));

			if (outSet.count >= maxOutTracks)
				continue;

			outSrcTrackIdx[outSet.count] = ti;
			FastObjectTrack& outTrk = outSet.tracks[outSet.count++];
			outTrk = FastObjectTrack{};
			outTrk.id = trk.id;
			outTrk.type = cls.type;
			outTrk.cx = static_cast<float>(cls.p0x);
			outTrk.cy = static_cast<float>(cls.p0y);
			outTrk.vx = static_cast<float>(rawVx);
			outTrk.vy = static_cast<float>(rawVy);
			outTrk.velXPerSec = static_cast<float>(cls.vxSec);
			outTrk.velYPerSec = static_cast<float>(cls.vySec);
			outTrk.ax = static_cast<float>(cls.axSec2);
			outTrk.ay = static_cast<float>(cls.aySec2);
			outTrk.pairSeconds = static_cast<float>(pairSeconds);
			outTrk.radiusPx = static_cast<float>(cls.usedPerspectiveDepth ? cls.radiusAtZero : trk.emaRadiusPx);
			outTrk.radiusRate = static_cast<float>(cls.radiusRatePerSec);
			outTrk.shutterEst = static_cast<float>(cls.shutterEst);
			outTrk.blurAlignFactor = static_cast<float>(cls.blurAlignFactor);
			outTrk.confidence = baseConfidence;
			outTrk.nis = static_cast<float>(targetNis);
			outTrk.bicMargin = static_cast<float>(cls.bicMargin);
			outTrk.ageFrames = static_cast<uint32_t>(winCount);
			outTrk.coastFrames = isCoastingAtTarget ? static_cast<uint32_t>(targetFrameIndex - lastSeenFrameAtOrBeforeTarget) : 0u;

			if (causalMode)
				outTrk.flags |= TrackFlags::kCausalMode;
			if (cls.blurStretched)
				outTrk.flags |= TrackFlags::kBlurStretched;
			if (cls.usedPerspectiveDepth)
				outTrk.flags |= TrackFlags::kDepthPerspective;
			if (trk.hasChroma)
				outTrk.flags |= TrackFlags::kChromaAssisted;
			if (trk.hasChroma && (trk.polarity == 0 || (targetPtIdx >= 0 && trk.history[targetPtIdx].blob.polarity == 0)))
				outTrk.flags |= TrackFlags::kIsoLumaChroma;
			if (trk.priorMatched)
				outTrk.flags |= TrackFlags::kSessionPriorMatch;
			if (targetCrossing)
				outTrk.flags |= TrackFlags::kCrossingAmbiguity;

			if (targetPtIdx >= 0)
			{
				const BlobCandidate& tb = trk.history[targetPtIdx].blob;
				outTrk.streakLenPx = tb.streakLenPx;
				outTrk.streakAngleRad = tb.streakAngleRad;
				outTrk.maskBox = tb.maskBox;
				for (int bi = 0; bi < 4; ++bi)
					outTrk.maskBits[bi] = tb.maskBits[bi];
				outTrk.hasMaskBits = tb.hasMaskBits;
			}
			else
			{
				const int rInt = std::max(1, static_cast<int>(std::ceil(outTrk.radiusPx)));
				outTrk.maskBox.left = static_cast<int>(std::floor(outTrk.cx)) - rInt;
				outTrk.maskBox.top = static_cast<int>(std::floor(outTrk.cy)) - rInt;
				outTrk.maskBox.right = static_cast<int>(std::ceil(outTrk.cx)) + rInt + 1;
				outTrk.maskBox.bottom = static_cast<int>(std::ceil(outTrk.cy)) + rInt + 1;
				outTrk.hasMaskBits = false;
			}

			// Impulse vertex bracketing check for pair (t, t+1).
			if (cls.type == TrajectoryType::Impulse && cls.hasImpulse &&
				cls.impulseTauSec > 0.0 && cls.impulseTauSec < pairSeconds)
			{
				const double tauFrac = cls.impulseTauSec / pairSeconds;
				double impImgX = cls.impulseX;
				double impImgY = cls.impulseY;
				if (targetSlot + 1 < ringCount_ && ring_[targetSlot + 1].camPrevUsable)
				{
					const FrameObservation& nextObs = ring_[targetSlot + 1];
					const double nextCx = nextObs.picture.Empty() ? targetCx : nextObs.picture.CentreX();
					const double nextCy = nextObs.picture.Empty() ? targetCy : nextObs.picture.CentreY();
					double mappedX = 0.0, mappedY = 0.0;
					CameraModel::Apply(nextObs.camPrevToCur, cls.impulseX - targetCx, cls.impulseY - targetCy, &mappedX, &mappedY);
					const double endX = mappedX + nextCx;
					const double endY = mappedY + nextCy;
					impImgX = (1.0 - tauFrac) * cls.impulseX + tauFrac * endX;
					impImgY = (1.0 - tauFrac) * cls.impulseY + tauFrac * endY;
				}
				outTrk.hasImpulse = true;
				outTrk.impulseTau = static_cast<float>(tauFrac);
				outTrk.impulseX = static_cast<float>(impImgX);
				outTrk.impulseY = static_cast<float>(impImgY);
				outTrk.flags |= TrackFlags::kImpulsePair;
			}

			// Swept disocclusion capsule from frame t to frame t+1.
			outTrk.disocclusionCapsule.x0 = outTrk.cx;
			outTrk.disocclusionCapsule.y0 = outTrk.cy;
			outTrk.disocclusionCapsule.x1 = outTrk.cx + outTrk.vx;
			outTrk.disocclusionCapsule.y1 = outTrk.cy + outTrk.vy;
			outTrk.disocclusionCapsule.radiusPx = outTrk.radiusPx;

			// Initial validity state machine (spec C.3: Coasting & Uncertain emit ZERO vectors to consumers).
			if (isCoastingAtTarget)
			{
				outTrk.state = ValidityState::Coasting;
				outTrk.confidence = 0.0f;
			}
			else if (targetCrossing || baseConfidence < cfg.minConfidence)
			{
				outTrk.state = ValidityState::Uncertain;
			}
			else
			{
				outTrk.state = ValidityState::Valid;
			}
		}

		// Frame-aligned pairwise crossing-ball ambiguity check at targetFrameIndex (eliminates look-ahead skew!):
		// If two resolved tracks approach within (r_1 + r_2 + 2 px) at frame t (or cross within pair [t, t+1]),
		// flag both kCrossingAmbiguity and demote to Uncertain.
		for (uint32_t i = 0; i < outSet.count; ++i)
		{
			for (uint32_t j = i + 1; j < outSet.count; ++j)
			{
				FastObjectTrack& a = outSet.tracks[i];
				FastObjectTrack& b = outSet.tracks[j];
				const float dx0 = a.cx - b.cx;
				const float dy0 = a.cy - b.cy;
				const float dvx = a.vx - b.vx;
				const float dvy = a.vy - b.vy;
				// Minimum separation over phase phi in [0, 1] along relative segment p_a(phi) - p_b(phi).
				const float dv2 = dvx * dvx + dvy * dvy;
				float phiStar = 0.0f;
				if (dv2 > 1e-6f)
					phiStar = std::min(std::max(-(dx0 * dvx + dy0 * dvy) / dv2, 0.0f), 1.0f);
				const float mdx = dx0 + phiStar * dvx;
				const float mdy = dy0 + phiStar * dvy;
				const float minSep = std::sqrt(mdx * mdx + mdy * mdy);
				const float thresh = a.radiusPx + b.radiusPx + 2.0f;
				if (minSep <= thresh)
				{
					a.flags |= TrackFlags::kCrossingAmbiguity;
					b.flags |= TrackFlags::kCrossingAmbiguity;
					if (a.state == ValidityState::Valid || a.state == ValidityState::Coasting)
						a.state = ValidityState::Uncertain;
					if (b.state == ValidityState::Valid || b.state == ValidityState::Coasting)
						b.state = ValidityState::Uncertain;
				}
			}
		}

		// Enforce strict zero-vector emission contract for non-Valid tracks and tally final diagnostics.
		outSet.diag.validTracks = 0;
		outSet.diag.uncertainTracks = 0;
		outSet.diag.coastingTracks = 0;
		outSet.diag.ballisticTracks = 0;
		outSet.diag.linearGlideTracks = 0;
		outSet.diag.impulseTracks = 0;
		for (uint32_t i = 0; i < outSet.count; ++i)
		{
			FastObjectTrack& outTrk = outSet.tracks[i];
			if (outTrk.state == ValidityState::Valid)
			{
				++outSet.diag.validTracks;
				if (outTrk.type == TrajectoryType::Ballistic) ++outSet.diag.ballisticTracks;
				else if (outTrk.type == TrajectoryType::LinearGlide) ++outSet.diag.linearGlideTracks;
				else if (outTrk.type == TrajectoryType::Impulse) ++outSet.diag.impulseTracks;
				if (cfg.enableSessionPrior)
					UpdateSessionPrior(tracks_[outSrcTrackIdx[i]]);
			}
			else
			{
				outTrk.vx = 0.0f;
				outTrk.vy = 0.0f;
				outTrk.disocclusionCapsule = Capsule{};
				if (outTrk.state == ValidityState::Coasting)
					++outSet.diag.coastingTracks;
				else if (outTrk.state == ValidityState::Uncertain)
					++outSet.diag.uncertainTracks;
			}
		}

		return true;
	}
}
