// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// repair.grain_split v3 (refs/116 s4-s6): the CPU reference of the v3 law (GrainSplit::Compute, low bands at half
// resolution) against the v2 law (GrainSplit::ComputeV2, the A/B baseline only). CPU only, deterministic (fixed seeds).
//
// The grain-keep metric (refs/116 s4): paired frames S (structure) and S + Gt (planted zero-mean Gaussian grain),
//   G_kept = grain(S + Gt) - grain(S);  keep = <G_kept, Gt> / <Gt, Gt>;  leak = ||Gt - G_kept||^2 / ||Gt||^2,
// evaluated on the interior (a 2-block margin dropped), per region class. Every number is logged.
//
// Owner decision D4 (near-edge acceptance, film-like grain on flat at scale 1, texture) is OPEN: those rows are only
// reported ('D4 pending'), never given a threshold here.

#include <VP-V/modules/repair/grain_split/GrainSplit.h>
#include <VP-V/modules/repair/grain_split/shared/BandFromBase.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace GrainSplitV3TestsNs
{
	using GrainSplit::Plane;

	enum class Cls { Flat, Gradient, DarkFlat, Texture, Edges };
	enum class Spec { White, Film };

	inline const char* ClsName(Cls c)
	{
		switch (c)
		{
		case Cls::Flat: return "flat";
		case Cls::Gradient: return "gradient";
		case Cls::DarkFlat: return "darkflat";
		case Cls::Texture: return "texture";
		default: return "edges";
		}
	}
	inline const char* SpecName(Spec s) { return s == Spec::White ? "white" : "film"; }

	struct Geo { int w, h; double scale; int B; };
	inline Geo GeoOf(int scaleIndex)
	{
		if (scaleIndex == 1) return Geo{ 768, 512, 2.0, 32 };
		return Geo{ 384, 256, 1.0, 16 };
	}

	inline void Log(const std::string& s) { Logger::WriteMessage(s.c_str()); }
	inline void Require(bool ok, const std::string& what)
	{
		if (!ok) Log("FAILED: " + what);
		const std::wstring w(what.begin(), what.end());
		Assert::IsTrue(ok, w.c_str());
	}

	// The structure plane S of a class (codes).
	inline Plane Structure(Cls c, const Geo& g)
	{
		Plane s(g.w, g.h);
		for (int y = 0; y < g.h; ++y)
			for (int x = 0; x < g.w; ++x)
			{
				double v = 400.0;
				switch (c)
				{
				case Cls::Flat: v = 400.0; break;
				case Cls::Gradient: v = 200.0 + 400.0 * x / (g.w - 1); break;
				case Cls::DarkFlat: v = 70.0; break;
				case Cls::Texture: v = 400.0 + 25.0 * std::sin(0.9 * x) * std::sin(0.7 * y) + 15.0 * std::sin(0.31 * x + 0.53 * y); break;
				case Cls::Edges: v = (((x / 24) + (y / 24)) & 1) ? 600.0 : 250.0; break;
				}
				s.at(x, y) = v;
			}
		return s;
	}

	// Zero-mean planted grain Gt at the target RMS (codes). White: iid Gaussian. Film-like: white blurred by sigma 0.6 px,
	// then renormalised to the target RMS (and re-centred to zero mean).
	inline Plane Grain(Spec sp, const Geo& g, double rms, uint32_t seed)
	{
		std::mt19937 rng(seed);
		std::normal_distribution<double> nd(0.0, 1.0);
		Plane n(g.w, g.h);
		for (size_t i = 0; i < n.v.size(); ++i) n.v[i] = nd(rng);
		if (sp == Spec::Film) n.v = VpUnit::BandFromBase::Blur(n.v, g.w, g.h, 0.6);
		double mean = 0.0;
		for (size_t i = 0; i < n.v.size(); ++i) mean += n.v[i];
		mean /= static_cast<double>(n.v.size());
		double ss = 0.0;
		for (size_t i = 0; i < n.v.size(); ++i) { n.v[i] -= mean; ss += n.v[i] * n.v[i]; }
		const double cur = std::sqrt(ss / static_cast<double>(n.v.size()));
		for (size_t i = 0; i < n.v.size(); ++i) n.v[i] *= rms / cur;
		return n;
	}

	struct Keep { double keep = 0.0, leak = 0.0; };

	inline GrainSplit::Split Law(bool v3, const Plane& y, const Geo& g)
	{
		return v3 ? GrainSplit::Compute(y, g.scale, g.B) : GrainSplit::ComputeV2(y, g.scale, g.B);
	}

	// keep / leak of one law on the interior (2-block margin).
	inline Keep Measure(bool v3, const Plane& s, const Plane& gt, const Geo& g)
	{
		Plane yg = s;
		for (size_t i = 0; i < yg.v.size(); ++i) yg.v[i] += gt.v[i];
		const GrainSplit::Split a = Law(v3, s, g), b = Law(v3, yg, g);
		const int m = 2 * g.B;
		double num = 0.0, den = 0.0, res = 0.0;
		for (int y = m; y < g.h - m; ++y)
			for (int x = m; x < g.w - m; ++x)
			{
				const double gk = b.grain.at(x, y) - a.grain.at(x, y);
				const double t = gt.at(x, y);
				num += gk * t;
				den += t * t;
				res += (t - gk) * (t - gk);
			}
		Keep k;
		k.keep = num / den;
		k.leak = res / den;
		return k;
	}

	// One matrix cell: both laws.
	struct Row
	{
		Cls cls; Spec spec; int scaleIndex; double rms;
		Keep v2, v3;
	};

	inline Row RunRow(Cls c, Spec sp, int scaleIndex, double rms)
	{
		const Geo g = GeoOf(scaleIndex);
		const uint32_t seed = 7u;   // one fixed seed for the metric matrix (refs/116 s4 numbers of the lead's scratch run)
		const Plane s = Structure(c, g);
		const Plane gt = Grain(sp, g, rms, seed);
		Row r;
		r.cls = c; r.spec = sp; r.scaleIndex = scaleIndex; r.rms = rms;
		r.v2 = Measure(false, s, gt, g);
		r.v3 = Measure(true, s, gt, g);
		return r;
	}

	inline std::string RowText(const Row& r)
	{
		char buf[256];
		std::snprintf(buf, sizeof(buf), "scale%d %-5s %-8s rms %.0f: keep v2 %.4f v3 %.4f | leak v2 %.4f v3 %.4f",
			r.scaleIndex + 1, SpecName(r.spec), ClsName(r.cls), r.rms, r.v2.keep, r.v3.keep, r.v2.leak, r.v3.leak);
		return buf;
	}

	TEST_CLASS(GrainSplitV3Tests)
	{
	public:
		TEST_METHOD(GrainSplitV3KeepsWhiteGrainOnFlatClasses)
		{
			const Cls cls[] = { Cls::Flat, Cls::Gradient, Cls::DarkFlat };
			for (int si = 0; si < 2; ++si)
				for (Cls c : cls)
					for (double rms : { 1.0, 3.0 })
					{
						const Row r = RunRow(c, Spec::White, si, rms);
						Log(RowText(r));
						Require(r.v3.keep >= 0.95, std::string("v3 keep >= 0.95: ") + RowText(r));
						Require(r.v3.leak <= 0.05, std::string("v3 leak <= 0.05: ") + RowText(r));
					}
		}

		TEST_METHOD(GrainSplitV3NotWorseThanV2)
		{
			const Cls cls[] = { Cls::Flat, Cls::Gradient, Cls::DarkFlat, Cls::Texture, Cls::Edges };
			for (int si = 0; si < 2; ++si)
				for (Spec sp : { Spec::White, Spec::Film })
					for (Cls c : cls)
						for (double rms : { 1.0, 3.0 })
						{
							const Row r = RunRow(c, sp, si, rms);
							Log(RowText(r));
							if (c == Cls::Edges)
							{
								// The near-edge acceptance is open owner decision D4: no threshold invented, only the 0.02 guard.
								Require(r.v3.keep >= r.v2.keep - 0.02, std::string("edges v3 keep >= v2 keep - 0.02 (D4 pending): ") + RowText(r));
							}
							else
								Require(r.v3.keep >= r.v2.keep - 0.01, std::string("v3 keep >= v2 keep - 0.01: ") + RowText(r));
						}
		}

		TEST_METHOD(GrainSplitV3ReportsOpenRows)
		{
			// No assertion: the rows the owner must decide (D4). Film-like flat and texture at scale 1, edges at both scales.
			for (double rms : { 1.0, 3.0 })
			{
				Log("D4 pending: " + RowText(RunRow(Cls::Flat, Spec::Film, 0, rms)));
				Log("D4 pending: " + RowText(RunRow(Cls::Texture, Spec::White, 0, rms)));
				Log("D4 pending: " + RowText(RunRow(Cls::Texture, Spec::Film, 0, rms)));
				for (int si = 0; si < 2; ++si)
				{
					Log("D4 pending: " + RowText(RunRow(Cls::Edges, Spec::White, si, rms)));
					Log("D4 pending: " + RowText(RunRow(Cls::Edges, Spec::Film, si, rms)));
				}
			}
		}

		TEST_METHOD(GrainSplitV3MatchesV2Closely)
		{
			const Cls cls[] = { Cls::Flat, Cls::Gradient, Cls::DarkFlat, Cls::Texture, Cls::Edges };
			for (int si = 0; si < 2; ++si)
			{
				const Geo g = GeoOf(si);
				for (Cls c : cls)
				{
					const Plane s = Structure(c, g);
					Plane y = s;
					const Plane gt = Grain(Spec::White, g, 3.0, 7u);
					for (size_t i = 0; i < y.v.size(); ++i) y.v[i] += gt.v[i];
					// refs/116b: the v3 law with the NLF cap OFF (ParamsV2) against the v2 law at the ORIGINAL limits.
					const GrainSplit::Split a = GrainSplit::Compute(y, g.scale, g.B, GrainSplit::ParamsV2());
					const GrainSplit::Split b = GrainSplit::ComputeV2(y, g.scale, g.B);
					double sum = 0.0, mx = 0.0, isum = 0.0;
					size_t icount = 0;
					const int m = 2 * g.B;
					for (int yy = 0; yy < g.h; ++yy)
						for (int xx = 0; xx < g.w; ++xx)
						{
							const double d = std::fabs(a.clean.at(xx, yy) - b.clean.at(xx, yy));
							sum += d;
							if (d > mx) mx = d;
							if (xx >= m && xx < g.w - m && yy >= m && yy < g.h - m) { isum += d; ++icount; }
						}
					const double mean = sum / static_cast<double>(a.clean.v.size());
					const double imean = isum / static_cast<double>(icount);
					char buf[240];
					std::snprintf(buf, sizeof(buf), "scale%d %-8s mean|clean_v3-clean_v2| whole %.4f, interior %.4f code, max %.4f code", si + 1, ClsName(c), mean, imean, mx);
					Log(buf);
					// The limits apply to the interior mean (the metric's own region; the clamped plane border is not graded).
					if (c == Cls::Texture) Require(imean <= 0.40, std::string("texture interior mean clean difference <= 0.40: ") + buf);
					else if (c != Cls::Edges) Require(imean <= 0.06, std::string("flat-class interior mean clean difference <= 0.06: ") + buf);

					if (c == Cls::Flat || c == Cls::Gradient || c == Cls::Texture)
					{
						// Blocks fully inside the interior (2-block margin). Limits calibrated from the scratch run (lead decision
						// P10V3-T4-CAL; the first guesses of 3 % / 10 % per block had never passed). Scratch numbers, flat/gradient:
						//   N rel: scale 1 mean 0.0065, worst 0.034; scale 2 mean 0.0005, worst 0.0021  -> assert mean <= 1 %, worst <= 5 %.
						//   e3 rel: scale 1 mean 0.133, median 0.118, p95 0.314, worst 0.469; scale 2 mean 0.056, worst 0.192.
						// On grain-only flat/gradient blocks e3 is the energy of filtered noise, a second-order quantity, so its per-block
						// relative error is noise-dominated: LOGGED only. On TEXTURE e3 carries structure: asserted, mean <= 5 %.
						double worstN = 0.0, worstE3 = 0.0, sumN = 0.0, sumE3 = 0.0, aggV3 = 0.0, aggV2 = 0.0;
						std::vector<double> relE3;
						for (int by = 2; by < a.bh - 2; ++by)
							for (int bx = 2; bx < a.bw - 2; ++bx)
							{
								const size_t i = static_cast<size_t>(by) * a.bw + bx;
								const double rn = std::fabs(a.N[i] - b.N[i]) / (std::max)(1e-9, std::fabs(b.N[i]));
								const double re = std::fabs(a.e3[i] - b.e3[i]) / (std::max)(1e-9, std::fabs(b.e3[i]));
								worstN = (std::max)(worstN, rn);
								worstE3 = (std::max)(worstE3, re);
								sumN += rn; sumE3 += re; relE3.push_back(re); aggV3 += a.e3[i]; aggV2 += b.e3[i];
							}
						std::sort(relE3.begin(), relE3.end());
						const double cnt = static_cast<double>(relE3.size());
						std::snprintf(buf, sizeof(buf), "scale%d %-8s worst N rel %.4f, worst e3 rel %.4f", si + 1, ClsName(c), worstN, worstE3);
						Log(buf);
						char buf2[200];
						std::snprintf(buf2, sizeof(buf2), "scale%d %-8s (info) mean N rel %.4f, mean e3 rel %.4f, median e3 rel %.4f, p95 e3 rel %.4f",
							si + 1, ClsName(c), sumN / cnt, sumE3 / cnt, relE3[relE3.size() / 2], relE3[static_cast<size_t>(0.95 * (cnt - 1.0))]);
						Log(buf2);
						const double R = aggV3 / aggV2;
						char buf3[160];
						std::snprintf(buf3, sizeof(buf3), "scale%d %-8s e3 aggregate ratio R = sum e3_v3 / sum e3_v2 = %.4f", si + 1, ClsName(c), R);
						Log(buf3);
						// WAIVER-P10V3-02: band d3 has one live reader (P11); TD will read the split later; re-check at the TD build.
						// e3 is asserted as the aggregate ratio R over the interior blocks, pinned to the measured value +- 0.05.
						// Measured R (v3 / v2; the v3 half-resolution low band carries less d3 energy than the full-resolution one):
						//   flat, gradient: scale 1 0.8954 (|R - 1| = 0.105 > 0.05, so pinned to measured +- 0.05, reported to the lead),
						//                   scale 2 0.9611 (|R - 1| <= 0.05 asserted);  texture: scale 1 0.7484, scale 2 0.8590.
						const double pinCentre = (c == Cls::Texture) ? (si == 0 ? 0.7484 : 0.8590) : (si == 0 ? 0.8954 : 1.0);
						Require(std::fabs(R - pinCentre) <= 0.05, std::string("e3 aggregate ratio within pin +- 0.05 (WAIVER-P10V3-02): ") + buf3);
						if (c != Cls::Texture)
						{
							Require(sumN / cnt <= 0.01, std::string("N mean relative error <= 1 %: ") + buf2);
							Require(worstN <= 0.05, std::string("N worst block relative error <= 5 %: ") + buf);
						}
					}
				}
			}
		}

		TEST_METHOD(GrainSplitV3GrainLawIdentity)
		{
			// The law identity grain == (1 - f) * (Y - base) per pixel (both laws). Independent of how clean is summed:
			// clean = base + f (D1 + D2) and D1 + D2 = Y - base, so grain = Y - clean = (1 - f) (Y - base).
			const Cls cls[] = { Cls::Flat, Cls::Gradient, Cls::DarkFlat, Cls::Texture, Cls::Edges };
			for (int si = 0; si < 2; ++si)
			{
				const Geo g = GeoOf(si);
				for (Cls c : cls)
				{
					Plane y = Structure(c, g);
					const Plane gt = Grain(Spec::Film, g, 3.0, 7u);
					for (size_t i = 0; i < y.v.size(); ++i) y.v[i] += gt.v[i];
					for (int law = 0; law < 2; ++law)
					{
						const GrainSplit::Split a = Law(law == 1, y, g);
						double worst = 0.0;
						for (size_t i = 0; i < y.v.size(); ++i)
							worst = (std::max)(worst, std::fabs(a.grain.v[i] - (1.0 - a.f.v[i]) * (y.v[i] - a.base.v[i])));
						char buf[160];
						std::snprintf(buf, sizeof(buf), "scale%d %-8s %s: max |grain - (1 - f)(Y - base)| = %.3e", si + 1, ClsName(c), law == 1 ? "v3" : "v2", worst);
						Log(buf);
						Require(worst <= 1e-9, std::string("grain == (1 - f)(Y - base) to 1e-9: ") + buf);
					}
				}
			}
		}

		TEST_METHOD(GrainSplitV3KeepsGrainInTextureAt4kLaw)
		{
			// The 4K lane law (scale 2, B 32): texture keep >= 0.90 and film-like flat keep >= 0.95, white and film, rms 1 and 3.
			// The scale-1 texture and film rows stay 'D4 pending' (GrainSplitV3ReportsOpenRows): log only.
			for (Spec sp : { Spec::White, Spec::Film })
				for (double rms : { 1.0, 3.0 })
				{
					const Row t = RunRow(Cls::Texture, sp, 1, rms);
					Log(RowText(t));
					// Owner ruling 2026-11-12 (refs/116b): in dense texture, texture wins. With the NLF cap the texture keep reads
					// about 0.05-0.15; the 90 % band applies to flat, gradient and dark. REPORT-ONLY; the texture-leak limits are
					// asserted by GrainSplitKeepsTextureInClean.
					Log("report-only (texture wins, refs/116b): " + RowText(t));
				}
			for (double rms : { 1.0, 3.0 })
			{
				const Row f = RunRow(Cls::Flat, Spec::Film, 1, rms);
				Log(RowText(f));
				Require(f.v3.keep >= 0.95, std::string("v3 film-like flat keep >= 0.95 at scale 2: ") + RowText(f));
			}
		}

		// ---- refs/116b: the NLF cap ----
		struct MixedPlanes { Plane s; Geo g; int tx0, ty0, tx1, ty1; };
		static MixedPlanes MakeMixed(int scaleIndex, bool clip)
		{
			MixedPlanes m;
			const int w = scaleIndex ? 1536 : 768, h = scaleIndex ? 1024 : 512;
			m.g = Geo{ w, h, scaleIndex ? 2.0 : 1.0, scaleIndex ? 32 : 16 };
			m.s = Plane(w, h);
			m.tx0 = w / 4; m.tx1 = 3 * w / 4; m.ty0 = h / 4; m.ty1 = 3 * h / 4;
			for (int y = 0; y < h; ++y)
				for (int x = 0; x < w; ++x)
				{
					const bool in = x >= m.tx0 && x < m.tx1 && y >= m.ty0 && y < m.ty1;
					double v = 400.0 + (in ? 25.0 * std::sin(0.9 * x) * std::sin(0.7 * y) + 15.0 * std::sin(0.31 * x + 0.53 * y) : 0.0);
					if (clip && !in && y < static_cast<int>(0.3 * h / m.g.B) * m.g.B) v = 64.0;
					m.s.at(x, y) = std::floor(v * 64.0 + 0.5) / 64.0;   // 1/64 code like the class planes
				}
			return m;
		}

		// rho: the texture core's correlation of the grain plane with the structure; keep: flat-region grain keep.
		static void MeasureMixed(const MixedPlanes& m, Spec sp, double rms, bool clip, double depthScale, int bits, double& rho, double& keep)
		{
			Plane gt = Grain(sp, m.g, rms, 7u);
			if (clip)
			{
				const int clipRows = static_cast<int>(0.3 * m.g.h / m.g.B) * m.g.B;
				for (int y = 0; y < clipRows; ++y) for (int x = 0; x < m.g.w; ++x) gt.at(x, y) = 0.0;
			}
			Plane s = m.s, yg = m.s;
			for (size_t i = 0; i < yg.v.size(); ++i) { yg.v[i] += gt.v[i]; }
			if (depthScale != 1.0) for (size_t i = 0; i < s.v.size(); ++i) { s.v[i] *= depthScale; yg.v[i] *= depthScale; gt.v[i] *= depthScale; }
			GrainSplit::Params p; p.sourceBits = bits;
			const GrainSplit::Split a = GrainSplit::Compute(s, m.g.scale, m.g.B, p), b = GrainSplit::Compute(yg, m.g.scale, m.g.B, p);
			const int B = m.g.B;
			// CORE: the texture rectangle shrunk by 3 B.
			double sx = 0.0; int n = 0;
			for (int y = m.ty0 + 3 * B; y < m.ty1 - 3 * B; ++y) for (int x = m.tx0 + 3 * B; x < m.tx1 - 3 * B; ++x) { sx += s.at(x, y); ++n; }
			const double mean = sx / n;
			double num = 0.0, den = 0.0;
			for (int y = m.ty0 + 3 * B; y < m.ty1 - 3 * B; ++y)
				for (int x = m.tx0 + 3 * B; x < m.tx1 - 3 * B; ++x)
				{
					const double sc = s.at(x, y) - mean;
					num += (yg.at(x, y) - b.clean.at(x, y)) * sc;   // G(S + g)
					den += sc * sc;
				}
			rho = den > 0.0 ? num / den : 0.0;
			// FLAT: [2B, w-2B) x [2B, h-2B) minus the texture rectangle grown by B (and the clip band + 2 B below it).
			const int clipEnd = clip ? static_cast<int>(0.3 * m.g.h / B) * B + 2 * B : 0;
			double kn = 0.0, kd = 0.0;
			for (int y = 2 * B; y < m.g.h - 2 * B; ++y)
				for (int x = 2 * B; x < m.g.w - 2 * B; ++x)
				{
					if (x >= m.tx0 - B && x < m.tx1 + B && y >= m.ty0 - B && y < m.ty1 + B) continue;
					if (y < clipEnd) continue;
					kn += (b.grain.at(x, y) - a.grain.at(x, y)) * gt.at(x, y);
					kd += gt.at(x, y) * gt.at(x, y);
				}
			keep = kd > 0.0 ? kn / kd : 0.0;
		}

		// MUST-2 (opus gate) + owner rulings 2026-11-12: a texture leak into grain is a defect; detail preservation
		// outranks grain keep; the keep band applies to flat / gradient / dark. Limits' arithmetic: refs/116b s5.1.
		TEST_METHOD(GrainSplitKeepsTextureInClean)
		{
			for (int si = 0; si < 2; ++si)
				for (int clip = 0; clip < 2; ++clip)
				{
					const MixedPlanes m = MakeMixed(si, clip != 0);
					for (Spec sp : { Spec::White, Spec::Film })
						for (double rms : { 0.0, 1.0, 3.0 })
						{
							if (rms == 0.0 && sp == Spec::Film) continue;
							double rho = 0.0, keep = 0.0;
							MeasureMixed(m, sp, rms, clip != 0, 1.0, 10, rho, keep);
							char buf[200];
							std::snprintf(buf, sizeof(buf), "mixed%s scale%d %-5s rms %.0f: texture leak rho %.4f, flat keep %.4f", clip ? "-clip" : "", si + 1, SpecName(sp), rms, rho, keep);
							Log(buf);
							const double limit = rms >= 3.0 ? 0.10 : (rms >= 1.0 ? 0.02 : 0.01);
							Require(rho <= limit, std::string("texture leak <= limit: ") + buf);
							if (rms > 0.0 && sp == Spec::White) Require(keep >= 0.95, std::string("flat keep (white) >= 0.95: ") + buf);
							else if (rms > 0.0 && sp == Spec::Film && si == 1) Require(keep >= 0.95, std::string("flat keep (film, 4K law) >= 0.95: ") + buf);
							// film at the 1080p law: REPORT (the v3 law's own limit, refs/116 D4)
						}
				}
		}

		// Hand-made quiet + luma arrays through the table law.
		TEST_METHOD(GrainLevelTableLaw)
		{
			GrainSplit::Params p;
			double level[8]; bool valid[8];
			// (a) n blocks in bin 3 (luma 448..): quiet ramps 1..n; keys / ranks exact.
			for (int n : { 8, 9, 10, 11, 100 })
			{
				std::vector<double> q, l;
				for (int i = 0; i < n; ++i) { q.push_back(1.0 + i); l.push_back(448.0); }
				GrainSplit::GrainLevelTable(q, l, p, level, valid);
				Require(valid[3], "bin 3 has a level at n >= 8");
				const int r = (10 * n + 99) / 100;
				const double expect = GrainSplit::LevelOfKey(GrainSplit::LevelKey(static_cast<double>(r)));
				Require(level[3] == expect, "rank r = ceil(10 n / 100): level = midpoint of the r-th smallest key");
			}
			// (b) 7 blocks: no level; neighbours interpolate.
			{
				std::vector<double> q, l;
				for (int i = 0; i < 20; ++i) { q.push_back(4.0); l.push_back(64.0); }     // bin 0
				for (int i = 0; i < 7; ++i)  { q.push_back(1.0); l.push_back(448.0); }    // bin 3: too few
				for (int i = 0; i < 20; ++i) { q.push_back(16.0); l.push_back(960.0); }   // bin 7
				GrainSplit::GrainLevelTable(q, l, p, level, valid);
				Require(!valid[3] && valid[0] && valid[7], "a bin with 7 blocks has no level");
				double cap = 0.0;
				Require(GrainSplit::GrainLevelCap(level, valid, p, 448.0, cap), "cap exists");
				const double centre0 = 0.5 * 128.0, centre7 = 7.5 * 128.0, t = (448.0 - centre0) / (centre7 - centre0);
				Require(std::fabs(cap - (level[0] + (level[7] - level[0]) * t)) < 1e-9, "interpolated between the valid bin centres");
				Require(GrainSplit::GrainLevelCap(level, valid, p, 10.0, cap) && cap == level[0], "constant below the first valid bin");
				// (e) level for quiet 8.7 within one class
			}
			// (c) no valid bin: no cap.
			{
				std::vector<double> q(5, 3.0), l(5, 300.0);
				GrainSplit::GrainLevelTable(q, l, p, level, valid);
				double cap = 0.0;
				Require(!GrainSplit::GrainLevelCap(level, valid, p, 300.0, cap), "no valid bin -> no cap (N = N_local)");
			}
			// (d) a grain-free bin caps only its own range.
			{
				std::vector<double> q, l;
				for (int i = 0; i < 20; ++i) { q.push_back(0.0); l.push_back(64.0); }
				for (int i = 0; i < 20; ++i) { q.push_back(9.0); l.push_back(448.0); }
				GrainSplit::GrainLevelTable(q, l, p, level, valid);
				Require(level[0] < 0.02 && level[3] > 7.0, "bin 0 level ~ 2^-6, bin 3 level unaffected");
				double cap = 0.0;
				GrainSplit::GrainLevelCap(level, valid, p, 448.0, cap);
				Require(cap == level[3], "at bin 3's centre the cap is bin 3's own level");
			}
			// (e) quantisation: level for quiet 8.7 within one 1/8-octave class.
			{
				std::vector<double> q(20, 8.7), l(20, 64.0);
				GrainSplit::GrainLevelTable(q, l, p, level, valid);
				const double r = std::pow(2.0, 1.0 / 8.0);
				Require(level[0] >= 8.7 / r && level[0] <= 8.7 * r, "level of quiet 8.7 lies in [8.7 / 2^(1/8), 8.7 x 2^(1/8)]");
			}
		}

		// 12-bit input (owner 2026-11-12): the law's limits are in ten-bit-equivalent units scaled by the source depth.
		// A plane x4 at sourceBits 12 must give the ten-bit result x4 (linear law, class keys of 10-bit-equivalent energies).
		TEST_METHOD(GrainSplitTwelveBitInputMatchesTenBitScaled)
		{
			const MixedPlanes m = MakeMixed(0, false);
			const Plane gt = Grain(Spec::White, m.g, 3.0, 7u);
			Plane y10 = m.s, y12 = m.s;
			for (size_t i = 0; i < y10.v.size(); ++i) { y10.v[i] += gt.v[i]; y12.v[i] = 4.0 * y10.v[i]; }
			GrainSplit::Params p10, p12; p12.sourceBits = 12;
			const GrainSplit::Split a = GrainSplit::Compute(y10, m.g.scale, m.g.B, p10), b = GrainSplit::Compute(y12, m.g.scale, m.g.B, p12);
			double worst = 0.0;
			for (size_t i = 0; i < a.clean.v.size(); ++i) worst = (std::max)(worst, std::fabs(4.0 * a.clean.v[i] - b.clean.v[i]));
			char buf[160];
			std::snprintf(buf, sizeof(buf), "12-bit (x4 plane) vs 10-bit: max |4 clean10 - clean12| = %.3e codes", worst);
			Log(buf);
			Require(worst < 1e-6, std::string("12-bit equals 10-bit x4: ") + buf);
			Require(GrainSplit::DepthScale(12) == 4.0 && GrainSplit::DepthScale(10) == 1.0 && GrainSplit::DepthScale(8) == 0.25, "DepthScale = 2^(bits-10)");
		}

		TEST_METHOD(GrainSplitV3HalfLawPieces)
		{
			// V3Half of a 4 x 4 ramp v = x + 4 y:  rows  0 1 2 3 / 4 5 6 7 / 8 9 10 11 / 12 13 14 15.
			// Half (0,0) = (0 + 1 + 4 + 5) / 4 = 2.5; (1,0) = (2 + 3 + 6 + 7) / 4 = 4.5;
			//      (0,1) = (8 + 9 + 12 + 13) / 4 = 10.5; (1,1) = (10 + 11 + 14 + 15) / 4 = 12.5.
			Plane ramp(4, 4);
			for (int y = 0; y < 4; ++y)
				for (int x = 0; x < 4; ++x) ramp.at(x, y) = x + 4 * y;
			const Plane h = GrainSplit::V3Half(ramp);
			Require(h.w == 2 && h.h == 2, "4x4 halves to 2x2");
			Require(h.at(0, 0) == 2.5 && h.at(1, 0) == 4.5 && h.at(0, 1) == 10.5 && h.at(1, 1) == 12.5, "2x2 box means of the ramp");

			// Odd size 3 x 3, v = x + 3 y: ceil(3 / 2) = 2; half (1,0) clamps x = 3 to 2: (2 + 2 + 5 + 5) / 4 = 3.5.
			Plane odd(3, 3);
			for (int y = 0; y < 3; ++y)
				for (int x = 0; x < 3; ++x) odd.at(x, y) = x + 3 * y;
			const Plane ho = GrainSplit::V3Half(odd);
			Require(ho.w == 2 && ho.h == 2, "3x3 halves to 2x2 (ceil)");
			Require(ho.at(1, 0) == 3.5, "clamped odd edge: (2 + 2 + 5 + 5) / 4 = 3.5");
			// (0,0) = (0 + 1 + 3 + 4) / 4 = 2.0; (1,1): rows 2 clamped to 2 both: (8 + 8 + 8 + 8) / 4 = 8.0.
			Require(ho.at(0, 0) == 2.0 && ho.at(1, 1) == 8.0, "odd plane other cells");

			// V3LowAt on h = {2.5, 4.5 / 10.5, 12.5}: full pixel p reads half position (p + 0.5) / 2 - 0.5.
			//  x = 0: -0.25 -> index -1 clamps to 0, weight 1 on h0;  x = 1: 0.25 -> 0.75 h0 + 0.25 h1;
			//  x = 2: 0.75 -> 0.25 h0 + 0.75 h1;  x = 3: 1.25 -> index 1 and 2 (clamped to 1): h1.
			const double eps = 1e-12;
			Require(std::fabs(GrainSplit::V3LowAt(h, 0, 0) - 2.5) < eps, "corner (0,0) clamps to h(0,0) = 2.5");
			Require(std::fabs(GrainSplit::V3LowAt(h, 3, 3) - 12.5) < eps, "corner (3,3) clamps to h(1,1) = 12.5");
			Require(std::fabs(GrainSplit::V3LowAt(h, 3, 0) - 4.5) < eps, "corner (3,0) clamps to h(1,0) = 4.5");
			Require(std::fabs(GrainSplit::V3LowAt(h, 0, 3) - 10.5) < eps, "corner (0,3) clamps to h(0,1) = 10.5");
			// x = 1, y = 0 : 0.75 * 2.5 + 0.25 * 4.5 = 1.875 + 1.125 = 3.0 (y clamps).
			Require(std::fabs(GrainSplit::V3LowAt(h, 1, 0) - 3.0) < eps, "weights 0.75 / 0.25 across: 3.0");
			// x = 2, y = 0 : 0.25 * 2.5 + 0.75 * 4.5 = 0.625 + 3.375 = 4.0.
			Require(std::fabs(GrainSplit::V3LowAt(h, 2, 0) - 4.0) < eps, "weights 0.25 / 0.75 across: 4.0");
			// x = 1, y = 1 : bilinear 0.75 / 0.25 on both axes of a plane that is linear: 2.5 + 2 * 0.25 + 8 * 0.25 = 5.0.
			Require(std::fabs(GrainSplit::V3LowAt(h, 1, 1) - 5.0) < eps, "interior point (1,1) = 0.5625 a + 0.1875 b + 0.1875 c + 0.0625 d = 5.0");
			// y = 2, x = 0 : hy = 0.75: 0.25 * 2.5 + 0.75 * 10.5 = 0.625 + 7.875 = 8.5.
			Require(std::fabs(GrainSplit::V3LowAt(h, 0, 2) - 8.5) < eps, "weights 0.25 / 0.75 down: 8.5");

			// HalfSigma(5) = sqrt(25 - 0.25) / 2 = sqrt(24.75) / 2 = 4.974937 / 2 = 2.487469; 3.5 * 2.487469 = 8.70614 -> ceil 9.
			Require(std::fabs(GrainSplit::V3HalfSigma(5.0) - std::sqrt(24.75) / 2.0) < eps, "V3HalfSigma(5) = sqrt(24.75) / 2");
			Require(GrainSplit::V3HalfRadius(5.0) == static_cast<int>(std::ceil(3.5 * VpUnit::BandFromBase::HalfSigma(5.0))), "V3HalfRadius(5) == ceil(3.5 HalfSigma(5))");
			Require(GrainSplit::V3HalfRadius(5.0) == 9, "V3HalfRadius(5) == 9");
			// sigma 15 (6 x 2.5 at scale 1, the low band): HalfSigma = sqrt(224.75) / 2 = 14.99166 / 2 = 7.49583; x 3.5 = 26.235 -> 27.
			Require(GrainSplit::V3HalfRadius(15.0) == 27, "V3HalfRadius(15) == 27");
			// sigma 2.5: HalfSigma = sqrt(6.25 - 0.25) / 2 = 2.449490 / 2 = 1.224745; x 3.5 = 4.2866 -> 5.
			Require(GrainSplit::V3HalfRadius(2.5) == 5, "V3HalfRadius(2.5) == 5");
			// Weights sum to one over -r..r.
			double sum = 0.0;
			const int r = GrainSplit::V3HalfRadius(5.0);
			for (int k = -r; k <= r; ++k) sum += GrainSplit::V3HalfWeight(k, 5.0);
			Require(std::fabs(sum - 1.0) < 1e-12, "half weights are normalised");
			// A constant plane stays constant through the whole half path.
			Plane flat(40, 30, 123.0);
			const Plane fh = GrainSplit::V3HalfBlur(GrainSplit::V3Half(flat), 5.0);
			Require(std::fabs(GrainSplit::V3LowAt(fh, 17, 11) - 123.0) < 1e-9, "constant plane through half blur and low read");
		}
	};
}
