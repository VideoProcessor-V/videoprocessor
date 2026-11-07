// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// K9-2: CameraPhaseInterp (CPU). Interpolation at a phase through log-scale and angle (never homography entries), per-pixel
// evaluation, the strict policy constants, mirror symmetry, continuity (including across the angle wrap), determinism and
// degenerate models.

#include <VP-V/modules/motion/estimation/global_model/link/CameraPhaseInterp.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace CameraPhaseInterpTestsNs
{
	using namespace CameraPhase;
	using CameraModel::ModelType;
	using CameraModel::Params;
	using CameraModel::Rect;

	inline void Require(bool ok, const wchar_t* what) { Assert::IsTrue(ok, what); }
	inline bool Near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

	inline Rect Pic() { Rect r; r.left = 0; r.top = 0; r.right = 1920; r.bottom = 1080; return r; }

	inline Params Similarity(double logScale, double angle, double tx, double ty)
	{
		Params p;
		p.type = ModelType::Similarity;
		const double k = std::exp(logScale), c = std::cos(angle), s = std::sin(angle);
		p.h[0] = k * c; p.h[1] = -k * s; p.h[2] = tx;
		p.h[3] = k * s; p.h[4] = k * c;  p.h[5] = ty;
		p.h[6] = 0; p.h[7] = 0; p.h[8] = 1;
		return p;
	}

	// A mild perspective on top of a similarity.
	inline Params Homography(double logScale, double angle, double tx, double ty, double h6, double h7)
	{
		Params p = Similarity(logScale, angle, tx, ty);
		p.type = ModelType::Homography;
		p.h[6] = h6;
		p.h[7] = h7;
		return p;
	}

	// F H F with F = diag(-1, 1, 1): the model seen in a horizontal mirror.
	inline Params Mirror(const Params& p)
	{
		Params m = p;
		m.h[1] = -p.h[1]; m.h[2] = -p.h[2];
		m.h[3] = -p.h[3];
		m.h[6] = -p.h[6];
		return m;
	}

	struct Pt { double x, y; };
	inline std::vector<Pt> Probes()
	{
		return { { -900, -500 }, { 0, 0 }, { 900, 500 }, { 600, -300 }, { -250, 420 }, { 15, 7 }, { 950, 0 }, { 0, -530 } };
	}

	inline void ModelDisp(const Params& p, double x, double y, double& dx, double& dy)
	{
		CameraModel::Displacement(p, x, y, &dx, &dy);
	}

	inline bool Bits(double a, double b) { return std::memcmp(&a, &b, sizeof(double)) == 0; }

	TEST_CLASS(CameraPhaseInterpTests)
	{
	public:
		TEST_METHOD(StrictPolicyConstantsAreTheDejudderPreset)
		{
			const CameraModel::Policy p = StrictPolicy(true), q = CameraModel::PolicyFor(CameraModel::Consumer::DejudderWarp);
			Require(p.threshold == 0.85 && p.floor == 0.7 && p.blendWidth == 0.05 && p.rateLimit == 0.2, L"0.85 / 0.7 / 0.05 / 0.2");
			Require(p.threshold == q.threshold && p.floor == q.floor && p.blendWidth == q.blendWidth && p.rateLimit == q.rateLimit &&
				p.hysteresis == q.hysteresis && p.preferModelWhenAgrees == q.preferModelWhenAgrees && p.curve == q.curve,
				L"equals PolicyFor(DejudderWarp)");
			Require(p.enabled && !StrictPolicy(false).enabled, L"enabled is the caller's switch");
		}

		TEST_METHOD(EndpointsAreExact)
		{
			const Rect pic = Pic();
			const Params models[3] = { Similarity(0.04, 0.02, 12.0, -5.0), Homography(-0.03, -0.015, -20.0, 8.0, 2.0e-5, -1.0e-5),
				Similarity(0.0, 0.0, 0.0, 0.0) };
			for (const Params& m : models)
			{
				const PhaseField f0 = AtPhase(m, pic, 0.0), f1 = AtPhase(m, pic, 1.0);
				Require(f0.Ok() && f1.Ok(), L"usable");
				for (const Pt& q : Probes())
				{
					double dx, dy, ex, ey;
					f0.Displacement(q.x, q.y, dx, dy);
					Require(Near(dx, 0, 1e-9) && Near(dy, 0, 1e-9), L"phase 0 is the identity");
					f1.Displacement(q.x, q.y, dx, dy);
					ModelDisp(m, q.x, q.y, ex, ey);
					Require(Near(dx, ex, 1e-7) && Near(dy, ey, 1e-7), L"phase 1 is the model, per pixel");
				}
			}
		}

		TEST_METHOD(LogScaleAndAngleNotMatrixEntries)
		{
			const Rect pic = Pic();
			const double ls = std::log(1.2);
			const PhaseField f = AtPhase(Similarity(ls, 0.0, 0.0, 0.0), pic, 0.5);
			double dx, dy;
			f.Displacement(500.0, 0.0, dx, dy);
			Require(Near(dx, 500.0 * (std::sqrt(1.2) - 1.0), 1e-9) && Near(dy, 0.0, 1e-9), L"half a zoom is sqrt(1.2), not 1.1");
			Require(std::fabs(dx - 500.0 * 0.1) > 0.5, L"differs from a linear interpolation of the matrix entry");
			// A rotation keeps its radius at every phase (a linear blend of the entries would shrink it).
			const PhaseField r = AtPhase(Similarity(0.0, 0.8, 0.0, 0.0), pic, 0.5);
			r.Displacement(300.0, 0.0, dx, dy);
			Require(Near(std::hypot(300.0 + dx, dy), 300.0, 1e-9), L"a half rotation stays on the circle");
			Require(Near(r.Similarity().angle, 0.4, 1e-12) && Near(r.Similarity().logScale, 0.0, 1e-12), L"angle scales linearly in angle");
		}

		TEST_METHOD(BetweenIsLogScaleAndShortestArc)
		{
			const Rect pic = Pic();
			const Params a = Similarity(std::log(1.1), 0.1, 4.0, 0.0), b = Similarity(std::log(1.3), 0.3, 8.0, 2.0);
			const PhaseField m = Between(a, b, pic, 0.5);
			Require(Near(m.Similarity().logScale, 0.5 * (std::log(1.1) + std::log(1.3)), 1e-9), L"mid scale = geometric mean");
			Require(Near(m.Similarity().angle, 0.2, 1e-9) && Near(m.Similarity().tx, 6.0, 1e-9) && Near(m.Similarity().ty, 1.0, 1e-9), L"angle and translation");
			// Across the +-pi seam the arc goes through pi, not through zero.
			const PhaseField w = Between(Similarity(0.0, 3.1, 0.0, 0.0), Similarity(0.0, -3.1, 0.0, 0.0), pic, 0.5);
			double dx, dy;
			w.Displacement(100.0, 0.0, dx, dy);
			Require(Near(dx, -200.0, 1.0) && Near(dy, 0.0, 1.0), L"mid of 3.1 and -3.1 is a half turn");
		}

		TEST_METHOD(BlendEndsAreTheEndModels)
		{
			const Rect pic = Pic();
			const Params a = Homography(0.03, 0.01, 6.0, -2.0, 1.5e-5, 0.0), b = Homography(-0.02, 0.02, -9.0, 3.0, 0.0, 2.0e-5);
			const PhaseField f0 = Between(a, b, pic, 0.0), f1 = Between(a, b, pic, 1.0);
			for (const Pt& q : Probes())
			{
				double dx, dy, ex, ey;
				f0.Displacement(q.x, q.y, dx, dy);
				ModelDisp(a, q.x, q.y, ex, ey);
				Require(Near(dx, ex, 1e-7) && Near(dy, ey, 1e-7), L"w = 0 is model a");
				f1.Displacement(q.x, q.y, dx, dy);
				ModelDisp(b, q.x, q.y, ex, ey);
				Require(Near(dx, ex, 1e-7) && Near(dy, ey, 1e-7), L"w = 1 is model b");
			}
		}

		TEST_METHOD(ContinuousInPhaseAndAcrossTheAngleWrap)
		{
			const Rect pic = Pic();
			struct Case { Params a, b; } cases[2] = {
				{ Homography(0.03, 0.01, 6.0, -2.0, 1.5e-5, 0.0), Homography(-0.02, 0.02, -9.0, 3.0, 0.0, 2.0e-5) },
				{ Similarity(0.0, 3.13, 0.0, 0.0), Similarity(0.0, -3.13, 1.0, 0.0) } };
			const int steps = 400;
			for (const Case& c : cases)
			{
				for (const Pt& q : Probes())
				{
					double pdx = 0, pdy = 0, ax, ay, bx, by;
					ModelDisp(c.a, q.x, q.y, ax, ay);
					ModelDisp(c.b, q.x, q.y, bx, by);
					const double chord = std::hypot(ax - bx, ay - by);
					// The chord of a rotation through the seam is a short arc: bound by the arc length 0.06 * radius too.
					const double bound = 4.0 * (chord + 0.06 * std::hypot(q.x, q.y) + 2.0) / steps;
					for (int i = 0; i <= steps; ++i)
					{
						double dx, dy;
						Between(c.a, c.b, pic, static_cast<double>(i) / steps).Displacement(q.x, q.y, dx, dy);
						if (i > 0)
							Require(std::hypot(dx - pdx, dy - pdy) <= bound, L"no jump between neighbouring phases");
						pdx = dx;
						pdy = dy;
					}
				}
			}
		}

		TEST_METHOD(MirrorSymmetry)
		{
			const Rect pic = Pic();
			const Params models[2] = { Similarity(0.05, 0.03, 14.0, -6.0), Homography(0.02, -0.04, -11.0, 5.0, 1.0e-5, -2.0e-5) };
			for (const Params& m : models)
			{
				const Params mm = Mirror(m);
				for (double t : { 0.0, 0.37, 1.0 })
				{
					const PhaseField a = AtPhase(m, pic, t), b = AtPhase(mm, pic, t);
					for (const Pt& q : Probes())
					{
						double dx, dy, mx, my;
						a.Displacement(q.x, q.y, dx, dy);
						b.Displacement(-q.x, q.y, mx, my);
						Require(Near(mx, -dx, 1e-7) && Near(my, dy, 1e-7), L"mirrored model, mirrored point: mirrored displacement");
					}
				}
				const PhaseField a = Between(m, models[0], pic, 0.3), b = Between(Mirror(m), Mirror(models[0]), pic, 0.3);
				for (const Pt& q : Probes())
				{
					double dx, dy, mx, my;
					a.Displacement(q.x, q.y, dx, dy);
					b.Displacement(-q.x, q.y, mx, my);
					Require(Near(mx, -dx, 1e-7) && Near(my, dy, 1e-7), L"blend is mirror symmetric too");
				}
			}
		}

		TEST_METHOD(GridMatchesTheModelVectorConvention)
		{
			const Rect pic = Pic();
			const Params m = Homography(0.02, 0.01, 7.0, -3.0, 1.0e-5, 0.0);
			std::vector<float> vx, vy;
			AtPhase(m, pic, 1.0).EvaluateGrid(pic, 120, 68, 16.0, vx, vy);
			Require(vx.size() == 120u * 68u && vy.size() == vx.size(), L"one vector per block");
			for (int by : { 0, 33, 67 })
				for (int bx : { 0, 59, 119 })
				{
					double cx, cy, ex, ey;
					CameraModel::BlockCentre(bx, by, 16.0, pic, &cx, &cy);
					ModelDisp(m, cx, cy, ex, ey);
					const size_t i = static_cast<size_t>(by) * 120 + bx;
					Require(Near(vx[i], ex, 1e-3) && Near(vy[i], ey, 1e-3), L"block vector = the model's displacement at the centre");
				}
		}

		TEST_METHOD(Deterministic)
		{
			const Rect pic = Pic();
			const Params a = Homography(0.03, 0.01, 6.0, -2.0, 1.5e-5, 0.0), b = Similarity(-0.02, 0.02, -9.0, 3.0);
			std::vector<float> v1x, v1y, v2x, v2y;
			Between(a, b, pic, 0.41).EvaluateGrid(pic, 120, 68, 16.0, v1x, v1y);
			Between(a, b, pic, 0.41).EvaluateGrid(pic, 120, 68, 16.0, v2x, v2y);
			Require(std::memcmp(v1x.data(), v2x.data(), v1x.size() * sizeof(float)) == 0 &&
				std::memcmp(v1y.data(), v2y.data(), v1y.size() * sizeof(float)) == 0, L"bit-identical grids");
			double d1, d2, e1, e2;
			AtPhase(a, pic, 0.6).Displacement(123.4, -56.7, d1, d2);
			AtPhase(a, pic, 0.6).Displacement(123.4, -56.7, e1, e2);
			Require(Bits(d1, e1) && Bits(d2, e2), L"bit-identical pixel");
		}

		TEST_METHOD(DegenerateModelsNeverInterpolate)
		{
			const Rect pic = Pic();
			const Params good = Similarity(0.03, 0.01, 5.0, -2.0);
			Params nan = good;
			nan.h[2] = std::nan("");
			const Params huge = Similarity(std::log(20.0), 0.0, 0.0, 0.0);               // outside [1/8, 8]
			const Params fold = Homography(0.0, 0.0, 0.0, 0.0, 0.01, 0.0);              // w = 1 + 0.01 x: a corner is behind the horizon
			Params flat = good;
			flat.h[8] = 0.0;                                                             // w = 0 everywhere
			const Params* bad[4] = { &nan, &huge, &fold, &flat };
			for (const Params* b : bad)
			{
				const PhaseField id = AtPhase(*b, pic, 0.5);
				Require(!id.Ok() && id.Which() == Source::Identity, L"AtPhase: identity, ok = false");
				double dx, dy;
				id.Displacement(300.0, 200.0, dx, dy);
				Require(dx == 0.0 && dy == 0.0, L"zero displacement");
				const PhaseField onlyA = Between(good, *b, pic, 0.5);
				const PhaseField onlyB = Between(*b, good, pic, 0.5);
				Require(onlyA.Ok() && onlyA.Which() == Source::OnlyA && onlyB.Ok() && onlyB.Which() == Source::OnlyB, L"one usable end: that end");
				double ex, ey;
				onlyA.Displacement(300.0, 200.0, dx, dy);
				ModelDisp(good, 300.0, 200.0, ex, ey);
				Require(Near(dx, ex, 1e-7) && Near(dy, ey, 1e-7), L"the usable end, whole");
				const PhaseField both = Between(*b, *b, pic, 0.5);
				Require(!both.Ok() && both.Which() == Source::Identity, L"both bad: identity, ok = false");
			}
			Rect empty;
			Require(!AtPhase(good, empty, 0.5).Ok(), L"empty picture");
			Require(!AtPhase(good, pic, std::nan("")).Ok(), L"non-finite phase");
			Require(AtPhase(Params(), pic, 0.5).Ok(), L"model None is the identity and usable");
		}
	};
}
