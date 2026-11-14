// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// B-S0 MEASUREMENT (dispatcher R14-27 d): (1) 2 versus 4 DCT half-shift grids in the B neighbour Wiener, (2) 8x8 sub-block versus
// 16x16 block warp quality of the shared bicubic neighbour warp. CPU only, deterministic (fixed seeds), SYNTHETIC frames (the test
// project has no decoder; real-clip rows are a T5 replay item). Every assertion is an analytic or structural identity; the measured
// numbers are only logged. No behaviour change: dctShifts defaults to 4 (bit-identical to the previous loop, 0.25 == 1/4).

#include <VP-V/modules/repair/grain_split/GrainLevel.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace GrainLevelShiftBenchNs
{
	using GrainSplit::Plane;

	inline void Log(const std::string& s) { Logger::WriteMessage(s.c_str()); }
	inline void Require(bool ok, const std::string& what)
	{
		if (!ok) Log("FAILED: " + what);
		const std::wstring w(what.begin(), what.end());
		Assert::IsTrue(ok, w.c_str());
	}

	constexpr int kW = 192, kH = 128, kB = 16;

	// Band-limited texture with a coarse and a mid frequency so the DCT tiles see structure, not a flat field.
	inline double Tex(double x, double y)
	{
		return 400.0 + 25.0 * std::sin(0.30 * x + 0.20 * y) + 15.0 * std::sin(0.17 * x - 0.23 * y) + 8.0 * std::sin(0.55 * x + 0.41 * y);
	}
	inline Plane Frame(double shiftX, double sigma, uint32_t seed)
	{
		Plane p(kW, kH);
		std::mt19937 rng(seed);
		std::normal_distribution<double> nd(0.0, 1.0);
		for (int y = 0; y < kH; ++y)
			for (int x = 0; x < kW; ++x) p.at(x, y) = Tex(x - shiftX, y) + (sigma > 0.0 ? sigma * nd(rng) : 0.0);
		return p;
	}
	inline GrainSplit::Split Split(const Plane& y) { return GrainSplit::Compute(y, 1.0, kB); }

	struct Row { double err = 0.0, kept = 0.0, ms = 0.0, meanK = 0.0; int fallback = 0, used = 0; bool sumOk = true; };

	// One neighbour Wiener run with K planted-grain neighbours (static field), the given grid count; error against the noise-free band.
	inline Row Run(int K, double sigma, int shifts)
	{
		const GrainSplit::Split cur = Split(Frame(0.0, sigma, 700));
		const Plane truth = GrainLevel::BandD(Split(Frame(0.0, 0.0, 0)));
		GrainLevel::BlockField f; f.Init(kW, kH, kB, 0.0, 0.0);
		std::vector<Plane> bands;
		for (int k = 1; k <= K; ++k) bands.push_back(GrainLevel::BandD(Split(Frame(0.0, sigma, 700 + static_cast<uint32_t>(k)))));
		std::vector<GrainLevel::Neighbour> nb(static_cast<size_t>(K));
		for (int k = 0; k < K; ++k) { nb[static_cast<size_t>(k)].d = &bands[static_cast<size_t>(k)]; nb[static_cast<size_t>(k)].field = f; nb[static_cast<size_t>(k)].epoch = 0; nb[static_cast<size_t>(k)].offset = -(k + 1); }
		GrainLevel::Estimator est; GrainLevel::Context c; c.sourceHz = 24.0; c.field = &f;
		est.Step(Split(Frame(0.0, sigma, 699)), c);
		const GrainLevel::Result t1 = est.Step(cur, c);
		GrainLevel::Params p; p.dctShifts = shifts;
		GrainLevel::Window win; win.back = 8; win.ahead = 4;
		const auto t0 = std::chrono::steady_clock::now();
		const GrainLevel::NeighbourResult r = GrainLevel::NeighbourWiener(cur, t1, nb, 0, win, p);
		const auto t2 = std::chrono::steady_clock::now();
		Row o;
		o.ms = std::chrono::duration<double, std::milli>(t2 - t0).count();
		o.fallback = r.fallbackPixels; o.used = r.usedNeighbours; o.meanK = r.meanKeff;
		const Plane raw = GrainLevel::BandD(cur);
		double e = 0.0, eT = 0.0, eC = 0.0; long n = 0;
		for (int y = 2 * kB; y < kH - 2 * kB; ++y)
			for (int x = 2 * kB; x < kW - 2 * kB; ++x)
			{
				const double b = r.cleanD.at(x, y) - truth.at(x, y);
				e += b * b; eT += truth.at(x, y) * truth.at(x, y); eC += r.cleanD.at(x, y) * r.cleanD.at(x, y); ++n;
				if (std::fabs(r.cleanD.at(x, y) + r.grainD.at(x, y) - raw.at(x, y)) > 1e-9) o.sumOk = false;
			}
		o.err = e / n; o.kept = eT > 0.0 ? eC / eT : 0.0;
		return o;
	}

	TEST_CLASS(GrainLevelShiftBench)
	{
	public:
		// 2 versus 4 half-shifted tile grids over the neighbour counts of the B window (2, 4, 8, 12) and three grain levels.
		TEST_METHOD(TwoVersusFourDctShifts)
		{
			Require(GrainLevel::Params().dctShifts == 4, "default is the 4 half-shift grids");
			const int Ks[] = { 2, 4, 8, 12 };
			const double sigmas[] = { 1.0, 2.0, 4.0 };
			for (double sg : sigmas)
				for (int K : Ks)
				{
					const Row a = Run(K, sg, 4), b = Run(K, sg, 2);
					Log("sigma " + std::to_string(sg) + " K " + std::to_string(K) + " | 4 shifts: err " + std::to_string(a.err) + " kept " + std::to_string(a.kept)
						+ " ms " + std::to_string(a.ms) + " | 2 shifts: err " + std::to_string(b.err) + " kept " + std::to_string(b.kept) + " ms " + std::to_string(b.ms)
						+ " | err ratio 2/4 " + std::to_string(a.err > 0.0 ? b.err / a.err : 0.0) + " time ratio 2/4 " + std::to_string(a.ms > 0.0 ? b.ms / a.ms : 0.0));
					Require(a.sumOk && b.sumOk, "clean + grain = D exactly for both grid counts");
					Require(a.used == b.used && a.fallback == b.fallback, "the grid count never changes which pixels take the neighbour path");
					Require(std::isfinite(a.err) && std::isfinite(b.err), "finite errors");
				}
		}

		// 8x8 sub-block versus 16x16 block fields on a smooth affine motion (zoom + rotation + translation): warp error against the
		// analytic truth. A constant motion must warp identically in both (structural identity).
		TEST_METHOD(SubBlockWarpQuality)
		{
			struct Motion { double a, b, c, d, tx, ty; const char* name; };
			const Motion ms[] = { { 0, 0, 0, 0, 1.3, -0.7, "translation" }, { 0.004, 0.0, 0.0, 0.004, 0.0, 0.0, "zoom 0.4%" }, { 0.0, -0.006, 0.006, 0.0, 0.0, 0.0, "rotation 0.6%" }, { 0.01, 0.004, -0.004, 0.01, 0.5, 0.2, "zoom 1% + rotation + shift" } };
			for (const Motion& m : ms)
			{
				// v(x) = A x + t, the vector the warp applies at the current-frame position x (ref sampled at x + v(x)).
				auto vx = [&](double x, double y) { return m.a * (x - kW / 2.0) + m.b * (y - kH / 2.0) + m.tx; };
				auto vy = [&](double x, double y) { return m.c * (x - kW / 2.0) + m.d * (y - kH / 2.0) + m.ty; };
				// ref(u) must equal Tex(x) at u = x + v(x): build ref by fixed-point inversion of u -> x per ref pixel.
				Plane ref(kW, kH);
				for (int y = 0; y < kH; ++y)
					for (int x = 0; x < kW; ++x)
					{
						double xs = x, ys = y;
						for (int it = 0; it < 8; ++it) { const double nx = x - vx(xs, ys), ny = y - vy(xs, ys); xs = nx; ys = ny; }
						ref.at(x, y) = Tex(xs, ys);
					}
				double err[2] = { 0.0, 0.0 };
				Plane wp[2];
				const int blocks[2] = { 16, 8 };
				for (int bi = 0; bi < 2; ++bi)
				{
					GrainLevel::BlockField f; f.Init(kW, kH, blocks[bi], 0.0, 0.0);
					for (int by = 0; by < f.bh; ++by)
						for (int bx = 0; bx < f.bw; ++bx)
						{
							const double cx = bx * blocks[bi] + 0.5 * blocks[bi] - 0.5, cy = by * blocks[bi] + 0.5 * blocks[bi] - 0.5;
							f.vx[static_cast<size_t>(by) * f.bw + bx] = vx(cx, cy); f.vy[static_cast<size_t>(by) * f.bw + bx] = vy(cx, cy);
						}
					const GrainLevel::Warped w = GrainLevel::WarpBicubic(ref, f, kW, kH);
					wp[bi] = w.v;
					double e = 0.0; long n = 0;
					for (int y = 8; y < kH - 8; ++y)
						for (int x = 8; x < kW - 8; ++x)
							if (w.valid[static_cast<size_t>(y) * kW + x]) { const double d = w.v.at(x, y) - Tex(x, y); e += d * d; ++n; }
					err[bi] = n ? e / n : 0.0;
				}
				Log(std::string("warp ") + m.name + ": MSE 16x16 " + std::to_string(err[0]) + " 8x8 " + std::to_string(err[1]) + " ratio 8/16 " + std::to_string(err[0] > 0.0 ? err[1] / err[0] : 0.0));
				Require(std::isfinite(err[0]) && std::isfinite(err[1]), "finite warp errors");
				if (m.a == 0 && m.b == 0 && m.c == 0 && m.d == 0)
				{
					bool same = true;
					for (size_t i = 0; i < wp[0].v.size(); ++i) if (wp[0].v[i] != wp[1].v[i]) same = false;
					Require(same, "a constant motion warps identically with 16x16 and 8x8 blocks");
				}
			}
		}
	};
}
