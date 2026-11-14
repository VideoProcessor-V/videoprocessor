// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// repair.grain_split / GrainLevel: the CPU reference law "v5" of the multi-frame grain level product P10g (refs/120,
// brief-S-3c phase T2, owner decisions OD:604 / OD:605 / OD:650 and the 11-14 answers). CPU model only: no GPU kernel
// exists or is written here (design rule 11-14 14:00: no grain-separation step without its measured 4K cost and its
// budget share). lead14\design-grain-chain-cost.md (11-14 14:0x) is the source of CostRows(); every cost is an ESTIMATE, nothing
// measured. The budget there (5.4 ms 4K laptop incl. 0.4 reserve; core 5.0) is a PROPOSAL: the owner has not set it, so the
// share is computed only when a caller passes a budget (kProposedBudget4kMs is that proposal, not a decision).
//
// What is in this file (all in the unit of P10's N: codes^2 of D = Y - G(base) Y, ten-bit-equivalent via Params::split.sourceBits):
//   L1  NLF_t  temporal noise level function: the CROSS form I_r = E_r - C_r (E = <D_t,D_t>, C = <D_t, W D_t-1>) on every trusted
//              region, per-luma-bin percentile p (T1 proposal p40) of an exponentially forgotten histogram. Half-life is in SECONDS:
//              the per-frame decay is 2^(-1 / (halfLife x measured source rate)), never a frame count (rule 6 / OD general-not-fixed).
//              The warp is BICUBIC (Catmull-Rom; owner 11-14: bicubic for L1/L2).
//   L2  coherence gate g = clamp((rho - a) / (b - a)), rho = C / I per half-block region, bilinear between region centres;
//              split f' = f + g (1 - f). Retained grain share f'^2 N_b is reported per block.
//   N_b = min(N_unit4, c_t x NLF_t(L_b)) with c_t = 1.0 for both laws (Q3 a); N_unit4 already holds min(N_local, NLF_s).
//   Guards: skip-copy regions (|v| < 0.25 px and I_r < 0.25 NLF_s) are excluded from the evidence, get g = 0 and are counted;
//              untrusted cells / outside footprint are excluded; AR(1) grain can only LOWER NLF_t (min), never raise N.
//   Mirror gate (Q2): incoherent, unambiguous regions go to the grain layer. Behind its own switch, DEFAULT OFF until the detail-loss
//              metric reads ~0 on real film (brief s2 T2). The owner's 11-14 answer says the T2 film verification runs WITH the veto
//              on: turn it on by Params::mirrorGate (one line), reported to the lead.
//   Resets: P4 cut, fade start, every MotionValidity epoch cause, settings change -> the accumulator is emptied and the frame
//              is the unit-4 law EXACTLY (bit identical; Result carries copies of the unit-4 planes).
//   Fallbacks are VISIBLE data in Result (mode, reason, trusted share, counters): the T3 product turns them into the Ctrl+I line,
//              Config status and Ctrl+Alt+D (rule "no silent fallbacks"); nothing here is log-only.
//   B   neighbour-window Wiener (owner 11-14 METHOD): 8 back + 4 ahead (low latency 6 back, 10 back only once the per-pixel vector
//              product exists), leave-one-out residual weights, per-frequency 8x8 DCT Wiener on the neighbour mean (4 half-shifts),
//              PSD shape from flat tiles. A pixel with fewer than 2 usable neighbours takes the L1+L2 gains exactly.
//
// Shared-product table (N1 by signature; this file is a CONSUMER of shared products, the stand-ins are marked):
//   vectors + trust : shared P2/P7 + CellTrust memo; here BlockField, filled by the caller (stand-in; never starts a matcher)
//   epochs / cuts   : shared MotionValidity epoch + P4 judge; here Context::reset (stand-in; no private detector)
//   D band, base    : the unit-4 Split (P10); not recomputed
//   neighbour warp  : WarpBicubic is the CPU stand-in of the ONE shared skeleton neighbour-warp product (readers P10g + TD, no TD copy)
//   Wiener, gate    : outputs are the P10g product (single producer): this module only
//
// Header-only, C++14, no Windows headers, deterministic: raster loops, double sums, no threads.
#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <VP-V/modules/repair/grain_split/GrainSplit.h>

namespace GrainLevel
{
	using GrainSplit::Plane;
	using GrainSplit::Split;

	// Clamp to [0, 1] (local: this file owns no shared clamp/ramp unit).
	inline double Unit01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

	// The law of this file. Bump on ANY change of the law or of the Result layout (the T3 GPU form and its parity tests pin it).
	constexpr uint32_t kLawVersion = 5;   // "CPU law v5" of brief-S-3c T2 (L1 NLF_t + L2 gate + guards + B neighbour window)

	// ---- cost ledger (design rule: every step states its 4K cost and its budget share) --------------------------------------
	// ms ranges are ESTIMATES on the 3050 Ti laptop (idle GPU) from refs/120 s5 and study-borrowed-grain s4 / s7; NOTHING here is
	// measured (no GPU kernel exists yet). budgetShare is unset until the owner sets the single grain-separation budget
	// (lead14\design-grain-chain-cost.md s2 proposes 5.4 ms; NOT set by the owner); BudgetShare() turns a row into a fraction once a budget exists.
	struct CostRow
	{
		const char* step;
		double ms4kLo, ms4kHi;         // 4K, 3050 Ti laptop
		double ms1080Lo, ms1080Hi;     // 1080p, 3050 Ti laptop
		const char* source;            // where the estimate comes from
	};
	inline const CostRow* CostRows(int& count)
	{
		static const CostRow rows[] = {
			{ "L1 NLF_t cross sums (1/4 of blocks, fused in APPLY)", 0.04, 0.08, 0.01, 0.02, "refs/120 s5 row 1 (ESTIMATE)" },
			{ "L2 coherence gate (half-res pass)",                   0.06, 0.11, 0.02, 0.03, "refs/120 s5 row 2 (ESTIMATE)" },
			{ "NLF_t level table + decay (merged into LEVEL)",         0.02, 0.04, 0.01, 0.02, "refs/120 s5 row 3 (ESTIMATE)" },
			{ "vector field upload + extra fetch",                   0.00, 0.01, 0.00, 0.01, "refs/120 s5 row 4 (ESTIMATE)" },
			{ "B neighbour window 8+4 (K=12, DCT Wiener, shared warps)", 2.5, 3.9, 0.63, 0.97, "design-grain-chain-cost row 8: point 2.9 / 0.73 (ESTIMATE)" },
			{ "B low latency 10 back + 0 ahead (replaces the row above)", 2.2, 3.5, 0.55, 0.88, "design-grain-chain-cost row 9: point 2.6 (ESTIMATE)" },
			{ "B low latency interim back-6 (replaces the row above)", 1.6, 2.8, 0.40, 0.69, "design-grain-chain-cost row 9i: point 1.9 (ESTIMATE)" },
			{ "single-frame fallback (unit-4 law, nothing added)",   0.00, 0.00, 0.00, 0.00, "by construction" },
		};
		count = static_cast<int>(sizeof(rows) / sizeof(rows[0]));
		return rows;
	}
	constexpr double kProposedBudget4kMs = 5.4;   // PROPOSAL (design-grain-chain-cost s2), owner has not set it
	// Fraction of a budget (ms) a row's upper estimate takes at 4K; negative when no budget is set (budgetMs <= 0).
	inline double BudgetShare(const CostRow& r, double budgetMs) { return budgetMs > 0.0 ? r.ms4kHi / budgetMs : -1.0; }

	// ---- vocabulary ------------------------------------------------------------------------------------------------------------
	enum class Mode : uint8_t { SingleFrame, Temporal, TemporalLagged };
	// Why a frame is not temporal (visible fallback reason). MotionEpoch carries the cause id in Context::epochCause.
	enum class Reason : uint8_t { None, Cut, FadeStart, Fading, MotionEpoch, SettingsChange, EngineOff, NoPrevious, UntrustedPair };
	// What the caller (P4 judge / MotionValidity epoch / settings) says about THIS frame.
	enum class Reset : uint8_t { None, Cut, FadeStart, MotionEpoch, SettingsChange };

	inline const char* Token(Mode m) { return m == Mode::Temporal ? "temporal" : (m == Mode::TemporalLagged ? "temporal (lagged)" : "single-frame"); }
	inline const char* Token(Reason r)
	{
		switch (r)
		{
		case Reason::Cut: return "cut";
		case Reason::FadeStart: return "fade start";
		case Reason::Fading: return "fade";
		case Reason::MotionEpoch: return "motion epoch";
		case Reason::SettingsChange: return "settings change";
		case Reason::EngineOff: return "engine off";
		case Reason::NoPrevious: return "no previous frame";
		case Reason::UntrustedPair: return "pair not trusted";
		default: return "none";
		}
	}

	struct Params
	{
		GrainSplit::Params split;            // bins / min blocks / source depth / kappa of the unit-4 law (never copied)
		// L1
		double halfLifeSeconds = 0.5;        // forgetting half-life in SECONDS (T1 proposal; the rate comes from the stream)
		int percentile = 40;                 // per-bin percentile p of I_r (T1 experiment parameter: 25 / 40 / 50)
		double ct = 1.0;                     // NLF_t -> N scale (Q3 a: 1.0 for both laws, no 1.5x)
		// L2
		double gateA = 1.0, gateB = 4.0;     // g = clamp((rho - a) / (b - a)) (T1 proposal 1, 4)
		double rhoFloorFrac = 0.01;          // I_r is floored at this fraction of the reference level in rho = C / I
		// guards
		bool skipGuard = true;               // test-only off switch (shows why the guard is essential); NOT a user setting
		double skipSpeedPx = 0.25;           // |v| below this ...
		double skipEnergyFrac = 0.25;        // ... and I_r below this fraction of NLF_s: a skip-copied static region
		// mirror gate (Q2): DEFAULT OFF
		bool mirrorGate = false;
		double mirrorRho = 0.25;             // rho below this: incoherent
		double mirrorMinEnergy = 0.5;        // I_r at least this fraction of the reference: there IS non-repeating energy (unambiguous)
		// B (owner METHOD 11-14)
		int backFrames = 8, aheadFrames = 4;            // standard
		int llBack = 6;                                  // low latency without the per-pixel vector product (option B back-6)
		int llBackPerPixel = 10;                         // low latency once the per-pixel vector product exists (+0 ahead)
		double neighbourKappa = 1.0;                     // Wiener kappa on the neighbour mean ("kappa about 1 on the harness N")
		double looGate = 4.0;                            // leave-one-out gate: boxmean5x5(r^2) <= looGate (N + V)
		// C8 repeated-frame gate (floor design s15): a neighbour that is a near-bit-identical repeat of the current frame carries the
		// SAME grain, so averaging it keeps the grain in the clean picture; it leaves B's set. Setting grain_repeat_gate (on|off).
		bool repeatGate = true;
		double repeatFrac = 0.005;                       // quiet block: mean (W_k - D_t)^2 <= this x N (measured: true repeats 0.000, 0 false fires; 0.05 false-fired)
		double repeatShare = 0.9;                        // a neighbour is a repeat when this share of its judged blocks is quiet
		int dctShifts = 4;                               // tile grids averaged: 4 = half-shifts (0,0) (4,0) (0,4) (4,4); 2 = (0,0) (4,4). MEASUREMENT knob (B-S0), default unchanged, NOT a user setting
		int minNeighbours = 2;                           // fewer usable neighbours: the L1+L2 gains exactly
		double psdFlatFactor = 1.5;                      // a tile is flat when mean d^2 <= factor N
		int psdMinTiles = 4;                             // fewer flat tiles: white PSD shape (counted, visible)
		int regionPx(int blockPx) const { return (std::max)(2, blockPx / 2); }
	};

	// ---- rate-aware forgetting (OD general-not-fixed) --------------------------------------------------------------------------
	inline double DecayPerFrame(double halfLifeSeconds, double sourceHz)
	{
		return (halfLifeSeconds > 0.0 && sourceHz > 0.0) ? std::exp2(-1.0 / (halfLifeSeconds * sourceHz)) : 0.0;
	}
	inline double HalfLifeFrames(double halfLifeSeconds, double sourceHz) { return halfLifeSeconds * sourceHz; }

	// ---- vectors ----------------------------------------------------------------------------------------------------------------
	// Block vector field of one pair, the CPU stand-in of IVectorEngine::FlowOf + CellTrust. Convention: the pixel (x, y) of the
	// CURRENT frame corresponds to (x + vx, y + vy) in the REFERENCE frame (the previous frame for L1/L2, the neighbour for B).
	struct BlockField
	{
		int B = 16, bw = 0, bh = 0;
		std::vector<double> vx, vy;
		std::vector<uint8_t> trusted;
		void Init(int w, int h, int block, double dx = 0.0, double dy = 0.0, bool trust = true)
		{
			B = block; bw = (w + block - 1) / block; bh = (h + block - 1) / block;
			const size_t n = static_cast<size_t>(bw) * bh;
			vx.assign(n, dx); vy.assign(n, dy); trusted.assign(n, trust ? 1 : 0);
		}
		bool Has() const { return bw > 0 && bh > 0 && vx.size() == static_cast<size_t>(bw) * bh; }
		size_t At(int x, int y) const { return static_cast<size_t>(y / B) * bw + static_cast<size_t>(x / B); }
	};

	// Catmull-Rom weights for the fraction t at taps -1, 0, 1, 2.
	inline void CubicWeights(double t, double* w)
	{
		const double t2 = t * t, t3 = t2 * t;
		w[0] = -0.5 * t3 + t2 - 0.5 * t;
		w[1] = 1.5 * t3 - 2.5 * t2 + 1.0;
		w[2] = -1.5 * t3 + 2.0 * t2 + 0.5 * t;
		w[3] = 0.5 * t3 - 0.5 * t2;
	}

	// One bicubic sample; false when any of the 16 taps lies outside the frame (the footprint rule: no clamped taps).
	inline bool SampleBicubic(const Plane& p, double sx, double sy, double& out)
	{
		const int x0 = static_cast<int>(std::floor(sx)), y0 = static_cast<int>(std::floor(sy));
		if (x0 - 1 < 0 || y0 - 1 < 0 || x0 + 2 >= p.w || y0 + 2 >= p.h) return false;
		double wx[4], wy[4];
		CubicWeights(sx - x0, wx);
		CubicWeights(sy - y0, wy);
		double acc = 0.0;
		for (int j = 0; j < 4; ++j)
		{
			double row = 0.0;
			for (int i = 0; i < 4; ++i) row += wx[i] * p.at(x0 - 1 + i, y0 - 1 + j);
			acc += wy[j] * row;
		}
		out = acc;
		return true;
	}

	// The reference plane warped to the current frame by a block field. valid[i] = 1 where the block is trusted and the bicubic
	// footprint is inside the frame. THE CPU stand-in of the shared neighbour-warp product.
	struct Warped
	{
		Plane v;
		std::vector<uint8_t> valid;
	};
	inline Warped WarpBicubic(const Plane& ref, const BlockField& f, int w, int h)
	{
		Warped o;
		o.v = Plane(w, h, 0.0);
		o.valid.assign(static_cast<size_t>(w) * h, 0);
		for (int y = 0; y < h; ++y)
			for (int x = 0; x < w; ++x)
			{
				const size_t b = f.At(x, y);
				if (!f.trusted[b]) continue;
				double s = 0.0;
				if (!SampleBicubic(ref, x + f.vx[b], y + f.vy[b], s)) continue;
				o.v.at(x, y) = s;
				o.valid[static_cast<size_t>(y) * w + x] = 1;
			}
		return o;
	}

	// D = d1 + d2 with the unit-4 expression (the same doubles the unit-4 Wiener share reads).
	inline Plane BandD(const Split& s)
	{
		Plane d(s.w, s.h);
		for (int y = 0; y < s.h; ++y)
			for (int x = 0; x < s.w; ++x) d.at(x, y) = s.d1.at(x, y) + s.d2.at(x, y);
		return d;
	}

	// ---- L1: the forgotten per-luma-bin histogram -----------------------------------------------------------------------------
	class LevelAccumulator
	{
	public:
		void Reset(int bins) { bins_ = bins; h_.assign(static_cast<size_t>(bins) * GrainSplit::kLevelKeys, 0.0); }
		void Decay(double factor) { for (double& v : h_) v *= factor; }
		void Add(int bin, double valueTen)
		{
			if (bin < 0 || bin >= bins_) return;
			h_[static_cast<size_t>(bin) * GrainSplit::kLevelKeys + GrainSplit::LevelKey(valueTen)] += 1.0;
		}
		double Weight(int bin) const
		{
			double s = 0.0;
			for (int k = 0; k < GrainSplit::kLevelKeys; ++k) s += h_[static_cast<size_t>(bin) * GrainSplit::kLevelKeys + k];
			return s;
		}
		// The bin's percentile in ten-bit-equivalent codes^2; false (no temporal level) below minEvidence accumulated regions.
		bool Level(int bin, int percentile, double minEvidence, double& level) const
		{
			const double total = Weight(bin);
			if (!(total >= minEvidence) || !(total > 0.0)) return false;
			const double target = total * static_cast<double>(percentile) / 100.0;
			double cum = 0.0;
			for (int k = 0; k < GrainSplit::kLevelKeys; ++k)
			{
				cum += h_[static_cast<size_t>(bin) * GrainSplit::kLevelKeys + k];
				if (cum >= target && cum > 0.0) { level = GrainSplit::LevelOfKey(k); return true; }
			}
			level = GrainSplit::LevelOfKey(GrainSplit::kLevelKeys - 1);
			return true;
		}
		double TotalWeight() const { double s = 0.0; for (double v : h_) s += v; return s; }
		int Bins() const { return bins_; }
	private:
		int bins_ = 0;
		std::vector<double> h_;
	};

	// ---- region statistics (cross form) ----------------------------------------------------------------------------------------
	struct Regions
	{
		int R = 8, rw = 0, rh = 0;
		std::vector<double> E, C, I, rho, luma;   // luma: the base at the region centre, in the plane's own codes
		std::vector<double> speed;                // |v| px of the region's centre block
		std::vector<uint8_t> trusted, skip;
	};

	// E_r, C_r, I_r per R x R region of the current band against the warped previous band. nlfRef(r) is the spatial level used by
	// the skip-copy guard and the rho floor: the unit-4 block N of the region's centre block (stand-in for the NLF_s table, which
	// the T3 product carries; Context::levelS overrides it when given).
	inline Regions CrossRegions(const Plane& dCur, const Warped& prev, const Plane& base, const BlockField& f, const std::vector<double>& blockN,
		const std::vector<double>* nlfS, const std::vector<uint8_t>* nlfSValid, const Params& p, int B)
	{
		Regions r;
		r.R = p.regionPx(B);
		const int w = dCur.w, h = dCur.h;
		r.rw = (w + r.R - 1) / r.R; r.rh = (h + r.R - 1) / r.R;
		const size_t n = static_cast<size_t>(r.rw) * r.rh;
		r.E.assign(n, 0.0); r.C.assign(n, 0.0); r.I.assign(n, 0.0); r.rho.assign(n, 0.0); r.luma.assign(n, 0.0);
		r.speed.assign(n, 0.0); r.trusted.assign(n, 0); r.skip.assign(n, 0);
		const int bw = (w + B - 1) / B;
		for (int ry = 0; ry < r.rh; ++ry)
			for (int rx = 0; rx < r.rw; ++rx)
			{
				const int x0 = rx * r.R, y0 = ry * r.R, x1 = (std::min)(w, x0 + r.R), y1 = (std::min)(h, y0 + r.R);
				double e = 0.0, c = 0.0;
				bool all = true;
				for (int y = y0; y < y1 && all; ++y)
					for (int x = x0; x < x1; ++x)
					{
						if (!prev.valid[static_cast<size_t>(y) * w + x]) { all = false; break; }
						const double dv = dCur.at(x, y);
						e += dv * dv;
						c += dv * prev.v.at(x, y);
					}
				const size_t i = static_cast<size_t>(ry) * r.rw + rx;
				const int cx = (std::min)(w - 1, x0 + (x1 - x0) / 2), cy = (std::min)(h - 1, y0 + (y1 - y0) / 2);
				r.luma[i] = base.at(cx, cy);
				const size_t b = f.At(cx, cy);
				r.speed[i] = std::sqrt(f.vx[b] * f.vx[b] + f.vy[b] * f.vy[b]);
				if (!all) continue;                      // untrusted: outside the footprint or an untrusted cell
				const double cnt = static_cast<double>((x1 - x0) * (y1 - y0));
				r.E[i] = e / cnt; r.C[i] = c / cnt; r.I[i] = r.E[i] - r.C[i];
				r.trusted[i] = 1;
				// reference spatial level for the guard and the rho floor
				double ref = blockN[static_cast<size_t>(cy / B) * bw + static_cast<size_t>(cx / B)];
				if (nlfS && nlfSValid)
				{
					// the optional NLF_s table is piecewise-constant per bin of the caller's own binning: use the nearest valid entry
					const int bins = static_cast<int>(nlfS->size());
					const double cs = GrainSplit::DepthScale(p.split.sourceBits);
					const int j = GrainSplit::LevelBin(r.luma[i] / cs, p.split);
					if (j >= 0 && j < bins && (*nlfSValid)[static_cast<size_t>(j)]) ref = (*nlfS)[static_cast<size_t>(j)] * cs * cs;
				}
				const double den = (std::max)(r.I[i], p.rhoFloorFrac * ref > 1e-12 ? p.rhoFloorFrac * ref : 1e-12);
				r.rho[i] = r.C[i] / den;
				if (p.skipGuard && r.speed[i] < p.skipSpeedPx && r.I[i] < p.skipEnergyFrac * ref) r.skip[i] = 1;
			}
		return r;
	}

	// Bilinear interpolation of a region quantity at a pixel; weights use only trusted, non-skip regions. Returns false when the
	// pixel's own region is untrusted or skipped (the caller then uses 0).
	inline bool RegionBilinear(const Regions& r, const std::vector<double>& q, int x, int y, double& out)
	{
		const int rx = (std::min)(r.rw - 1, x / r.R), ry = (std::min)(r.rh - 1, y / r.R);
		const size_t own = static_cast<size_t>(ry) * r.rw + rx;
		if (!r.trusted[own] || r.skip[own]) return false;
		const double u = (x + 0.5) / r.R - 0.5, v = (y + 0.5) / r.R - 0.5;
		const int i0 = static_cast<int>(std::floor(u)), j0 = static_cast<int>(std::floor(v));
		const double fx = u - i0, fy = v - j0;
		double sw = 0.0, sq = 0.0;
		for (int dj = 0; dj < 2; ++dj)
			for (int di = 0; di < 2; ++di)
			{
				const int ii = (std::max)(0, (std::min)(r.rw - 1, i0 + di)), jj = (std::max)(0, (std::min)(r.rh - 1, j0 + dj));
				const size_t k = static_cast<size_t>(jj) * r.rw + ii;
				if (!r.trusted[k] || r.skip[k]) continue;
				const double wgt = (di ? fx : 1.0 - fx) * (dj ? fy : 1.0 - fy);
				sw += wgt; sq += wgt * q[k];
			}
		if (!(sw > 0.0)) return false;
		out = sq / sw;
		return true;
	}

	// ---- the Result (the data the P10g product will carry; T3 fixes the GPU layout) ---------------------------------------------
	struct Result
	{
		uint32_t version = kLawVersion;
		Mode mode = Mode::SingleFrame;
		Reason reason = Reason::None;
		uint16_t epochCause = 0;
		int ageFrames = 0;                       // frames since the last reset (0 = the reset frame itself)
		double trustedShare = 0.0;               // trusted regions / regions (temporal frames; 0 otherwise)
		int regionsTotal = 0, regionsTrusted = 0, skipRegions = 0, gatedRegions = 0, mirrorRegions = 0;
		// per luma bin (ten-bit-equivalent codes^2)
		std::vector<double> levelT; std::vector<uint8_t> validT; std::vector<double> evidenceT;
		// per block
		std::vector<double> Nb;                  // N_b = min(N_unit4, c_t NLF_t(L_b)); the unit-4 N in single-frame mode
		std::vector<double> retained;            // mean f'^2 x N_b: the grain energy that stays in clean (TD's A1 correction reads it)
		// per pixel; in single-frame mode exact copies of the unit-4 planes
		Plane f, gate, s1, s2, clean, grain;
		bool nlfTLowersN = false;                // true when at least one block's N was lowered by NLF_t
	};

	// ---- the estimator ----------------------------------------------------------------------------------------------------------
	struct Context
	{
		double sourceHz = 0.0;                   // MEASURED source rate (the caller's; never assumed here); <= 0 -> single-frame
		Reset reset = Reset::None;
		uint16_t epochCause = 0;                 // MotionValidity / ResyncCause id when reset == MotionEpoch (diagnostic)
		bool fading = false;                     // a fade is running (after FadeStart): single-frame until the caller clears it
		bool engineOn = true;                    // the motion engine / vectors exist at all
		bool pairTrusted = true;                 // same epoch, not a repeat, vectors for (t-1, t) present
		bool lagged = false;                     // low latency: the pair is (t-2, t-1)
		const BlockField* field = nullptr;       // current -> previous frame
		const std::vector<double>* levelS = nullptr;       // optional NLF_s table (ten-bit-equivalent codes^2 per bin)
		const std::vector<uint8_t>* levelSValid = nullptr;
	};

	class Estimator
	{
	public:
		explicit Estimator(const Params& p = Params()) : p_(p) { acc_.Reset(p.split.levelBins); }

		// Drop all history (a reset the caller did not announce per frame, e.g. a source change).
		void Forget() { acc_.Reset(p_.split.levelBins); hasPrev_ = false; age_ = 0; }
		double EvidenceWeight() const { return acc_.TotalWeight(); }
		const Params& Parameters() const { return p_; }

		Result Step(const Split& u4, const Context& ctx)
		{
			Result r;
			const Plane dCur = BandD(u4);
			Reason why = Reason::None;
			bool temporal = false;
			if (ctx.reset != Reset::None)
			{
				acc_.Reset(p_.split.levelBins);
				age_ = 0;
				why = ctx.reset == Reset::Cut ? Reason::Cut : (ctx.reset == Reset::FadeStart ? Reason::FadeStart
					: (ctx.reset == Reset::MotionEpoch ? Reason::MotionEpoch : Reason::SettingsChange));
				r.epochCause = ctx.epochCause;
			}
			else
			{
				++age_;
				if (ctx.fading) why = Reason::Fading;
				else if (!ctx.engineOn) why = Reason::EngineOff;
				else if (!hasPrev_ || prevD_.w != dCur.w || prevD_.h != dCur.h) why = Reason::NoPrevious;
				else if (!ctx.pairTrusted || !(ctx.sourceHz > 0.0) || !ctx.field || !ctx.field->Has()) why = Reason::UntrustedPair;
				else temporal = true;
			}
			r.ageFrames = age_;
			if (temporal) RunTemporal(u4, dCur, ctx, r);
			else
			{
				FillSingle(u4, r, why);
				// A gap (engine off, untrusted pair, fade) still AGES the history by the frame period: the half-life is in seconds,
				// so a frame without evidence is not free. Resets already emptied it.
				if (ctx.reset == Reset::None && ctx.sourceHz > 0.0) acc_.Decay(DecayPerFrame(p_.halfLifeSeconds, ctx.sourceHz));
			}
			prevD_ = dCur;
			hasPrev_ = true;
			return r;
		}

	private:
		static void FillSingle(const Split& u4, Result& r, Reason why)
		{
			r.mode = Mode::SingleFrame;
			r.reason = why;
			r.Nb = u4.N;
			r.f = u4.f; r.s1 = u4.s1; r.s2 = u4.s2; r.clean = u4.clean; r.grain = u4.grain;
			r.gate = Plane(u4.w, u4.h, 0.0);
			Retained(u4, u4.f, r.Nb, r.retained);
		}

		static void Retained(const Split& u4, const Plane& f, const std::vector<double>& Nb, std::vector<double>& out)
		{
			out.assign(static_cast<size_t>(u4.bw) * u4.bh, 0.0);
			for (int by = 0; by < u4.bh; ++by)
				for (int bx = 0; bx < u4.bw; ++bx)
				{
					const int x1 = (std::min)(u4.w, (bx + 1) * u4.B), y1 = (std::min)(u4.h, (by + 1) * u4.B);
					double s = 0.0;
					for (int y = by * u4.B; y < y1; ++y)
						for (int x = bx * u4.B; x < x1; ++x) s += f.at(x, y) * f.at(x, y);
					const double cnt = static_cast<double>((x1 - bx * u4.B) * (y1 - by * u4.B));
					const size_t i = static_cast<size_t>(by) * u4.bw + bx;
					out[i] = s / cnt * Nb[i];
				}
		}

		void RunTemporal(const Split& u4, const Plane& dCur, const Context& ctx, Result& r)
		{
			const int w = u4.w, h = u4.h, B = u4.B, bins = p_.split.levelBins;
			const double cs = GrainSplit::DepthScale(p_.split.sourceBits), es = cs * cs;
			r.mode = ctx.lagged ? Mode::TemporalLagged : Mode::Temporal;
			r.reason = Reason::None;

			const Warped prev = WarpBicubic(prevD_, *ctx.field, w, h);
			Regions reg = CrossRegions(dCur, prev, u4.base, *ctx.field, u4.N, ctx.levelS, ctx.levelSValid, p_, B);
			r.regionsTotal = reg.rw * reg.rh;

			// L1: forget (rate-aware), then add the trusted, non-skip regions' I_r to their luma bin.
			acc_.Decay(DecayPerFrame(p_.halfLifeSeconds, ctx.sourceHz));
			for (size_t i = 0; i < reg.I.size(); ++i)
			{
				if (reg.trusted[i]) ++r.regionsTrusted;
				if (reg.skip[i]) ++r.skipRegions;
				if (!reg.trusted[i] || reg.skip[i]) continue;
				acc_.Add(GrainSplit::LevelBin(reg.luma[i] / cs, p_.split), reg.I[i] / es);
			}
			r.trustedShare = r.regionsTotal ? static_cast<double>(r.regionsTrusted) / static_cast<double>(r.regionsTotal) : 0.0;

			std::vector<double> level(static_cast<size_t>(bins), 0.0);
			std::vector<uint8_t> validT(static_cast<size_t>(bins), 0);
			std::unique_ptr<bool[]> validB(new bool[static_cast<size_t>(bins)]);
			r.evidenceT.assign(static_cast<size_t>(bins), 0.0);
			for (int j = 0; j < bins; ++j)
			{
				double lv = 0.0;
				const bool ok = acc_.Level(j, p_.percentile, static_cast<double>(p_.split.levelMinBlocks), lv);
				level[static_cast<size_t>(j)] = ok ? lv : 0.0;
				validT[static_cast<size_t>(j)] = ok ? 1 : 0;
				validB[static_cast<size_t>(j)] = ok;
				r.evidenceT[static_cast<size_t>(j)] = acc_.Weight(j);
			}
			r.levelT = level; r.validT = validT;

			// N_b = min(N_unit4, c_t NLF_t(L_b)): the unit-4 N already holds min(N_local, NLF_s).
			r.Nb = u4.N;
			for (int by = 0; by < u4.bh; ++by)
				for (int bx = 0; bx < u4.bw; ++bx)
				{
					const size_t i = static_cast<size_t>(by) * u4.bw + bx;
					const double luma = u4.base.at((std::min)(bx * B + B / 2, w - 1), (std::min)(by * B + B / 2, h - 1));
					double cap = 0.0;
					if (GrainSplit::GrainLevelCap(level.data(), validB.get(), p_.split, luma / cs, cap))
					{
						const double nt = p_.ct * cap * es;
						if (nt < r.Nb[i]) { r.Nb[i] = nt; r.nlfTLowersN = true; }
					}
				}

			// L2 gate (and the optional mirror gate) per region, then per pixel.
			std::vector<double> gReg(reg.I.size(), 0.0), hReg(reg.I.size(), 0.0);
			const int bwB = u4.bw;
			for (int ry = 0; ry < reg.rh; ++ry)
				for (int rx = 0; rx < reg.rw; ++rx)
				{
					const size_t i = static_cast<size_t>(ry) * reg.rw + rx;
					if (!reg.trusted[i] || reg.skip[i]) continue;
					const double g = (std::max)(0.0, (std::min)(1.0, (reg.rho[i] - p_.gateA) / (p_.gateB - p_.gateA)));
					gReg[i] = g;
					if (g > 0.0) ++r.gatedRegions;
					if (p_.mirrorGate)
					{
						const int cx = (std::min)(w - 1, rx * reg.R + reg.R / 2), cy = (std::min)(h - 1, ry * reg.R + reg.R / 2);
						const double ref = r.Nb[static_cast<size_t>(cy / B) * bwB + static_cast<size_t>(cx / B)];
						if (reg.rho[i] < p_.mirrorRho && reg.I[i] >= p_.mirrorMinEnergy * ref && g == 0.0)
						{
							hReg[i] = (std::min)(1.0, (std::max)(0.0, 1.0 - reg.rho[i] / p_.mirrorRho));
							if (hReg[i] > 0.0) ++r.mirrorRegions;
						}
					}
				}

			const double floorE = p_.split.energyFloor * es;
			r.f = Plane(w, h); r.gate = Plane(w, h, 0.0);
			r.s1 = Plane(w, h); r.s2 = Plane(w, h); r.clean = Plane(w, h); r.grain = Plane(w, h);
			for (int y = 0; y < h; ++y)
				for (int x = 0; x < w; ++x)
				{
					double e = 0.0;
					for (int j = -1; j <= 1; ++j)
						for (int i = -1; i <= 1; ++i) { const double dv = dCur.get(x + i, y + j); e += dv * dv; }
					e /= 9.0;
					const double N = r.Nb[static_cast<size_t>(y / B) * bwB + static_cast<size_t>(x / B)];
					const double f = Unit01(1.0 - p_.split.kappa * N / (std::max)(e, floorE));
					double g = 0.0, hh = 0.0;
					RegionBilinear(reg, gReg, x, y, g);
					double fp = f + g * (1.0 - f);
					if (p_.mirrorGate && RegionBilinear(reg, hReg, x, y, hh) && hh > 0.0) fp = fp * (1.0 - hh);
					r.gate.at(x, y) = g;
					r.f.at(x, y) = fp;
					r.s1.at(x, y) = fp * u4.d1.at(x, y);
					r.s2.at(x, y) = fp * u4.d2.at(x, y);
					r.clean.at(x, y) = u4.base.at(x, y) + r.s1.at(x, y) + r.s2.at(x, y);
					r.grain.at(x, y) = (u4.clean.at(x, y) + u4.grain.at(x, y)) - r.clean.at(x, y);   // Y - clean, Y = clean + grain of unit 4
				}
			Retained(u4, r.f, r.Nb, r.retained);
		}

		Params p_;
		LevelAccumulator acc_;
		Plane prevD_;
		bool hasPrev_ = false;
		int age_ = 0;
	};

	// ---- B: the neighbour window (owner METHOD 11-14: 8 back + 4 ahead) ----------------------------------------------------------
	struct Window { int back = 0, ahead = 0; };
	// The window the law asks for. availableAhead = the queue's real look-ahead (the ahead count never exceeds it). Low latency:
	// 0 ahead and 6 back (option B back-6), or 10 back only once the per-pixel vector product exists.
	inline Window WindowFor(const Params& p, bool lowLatency, bool perPixelVectors, int availableAhead)
	{
		Window wd;
		if (lowLatency) { wd.back = perPixelVectors ? p.llBackPerPixel : p.llBack; wd.ahead = 0; return wd; }
		wd.back = p.backFrames;
		wd.ahead = (std::max)(0, (std::min)(p.aheadFrames, availableAhead));
		return wd;
	}

	// One neighbour: its band plane D_k, the field current -> that frame, its epoch id and its signed offset (negative = behind).
	struct Neighbour
	{
		const Plane* d = nullptr;
		BlockField field;
		int epoch = 0;
		int offset = 0;
	};

	struct NeighbourResult
	{
		Plane cleanD, grainD;       // cleanD + grainD = D_t exactly
		Plane keff;                 // neighbours kept per pixel (0 where the L1+L2 gains were used)
		int pixels = 0, fallbackPixels = 0, usedNeighbours = 0, droppedCrossEpoch = 0;
		int droppedRepeat = 0;      // C8: repeated frames that left the set (visible: FallbackCatalog::GrainRepeatGate)
		bool psdWhite = false;      // no flat tiles: the white PSD shape was used (visible)
		double meanKeff = 0.0;      // over the non-fallback pixels
	};

	// Orthonormal 8x8 DCT-II basis, basis[k][n].
	inline const double* DctBasis()
	{
		struct Table
		{
			double v[64];
			Table()
			{
				const double pi = 3.14159265358979323846;
				for (int k = 0; k < 8; ++k)
					for (int n = 0; n < 8; ++n)
						v[k * 8 + n] = (k == 0 ? std::sqrt(1.0 / 8.0) : std::sqrt(2.0 / 8.0)) * std::cos(pi * (2 * n + 1) * k / 16.0);
			}
		};
		static const Table table;   // C++11 magic static: thread-safe one-time init
		return table.v;
	}
	inline void Dct8x8(const double* in, double* out)
	{
		const double* t = DctBasis();
		double tmp[64];
		for (int y = 0; y < 8; ++y)           // rows
			for (int k = 0; k < 8; ++k)
			{
				double a = 0.0;
				for (int n = 0; n < 8; ++n) a += t[k * 8 + n] * in[y * 8 + n];
				tmp[y * 8 + k] = a;
			}
		for (int x = 0; x < 8; ++x)           // columns
			for (int k = 0; k < 8; ++k)
			{
				double a = 0.0;
				for (int n = 0; n < 8; ++n) a += t[k * 8 + n] * tmp[n * 8 + x];
				out[k * 8 + x] = a;
			}
	}
	inline void Idct8x8(const double* in, double* out)
	{
		const double* t = DctBasis();
		double tmp[64];
		for (int x = 0; x < 8; ++x)
			for (int n = 0; n < 8; ++n)
			{
				double a = 0.0;
				for (int k = 0; k < 8; ++k) a += t[k * 8 + n] * in[k * 8 + x];
				tmp[n * 8 + x] = a;
			}
		for (int y = 0; y < 8; ++y)
			for (int n = 0; n < 8; ++n)
			{
				double a = 0.0;
				for (int k = 0; k < 8; ++k) a += t[k * 8 + n] * tmp[y * 8 + k];
				out[y * 8 + n] = a;
			}
	}

	// The grain PSD SHAPE per DCT coefficient (mean 1) from the flat 8x8 tiles of the current band (the P10 grain monitor's
	// per-coefficient field in the product; here derived from the same flat-tile rule). white=true when too few flat tiles.
	inline void PsdShape(const Plane& d, const std::vector<double>& Nb, int B, int bw, const Params& p, double* shape, bool& white)
	{
		double acc[64] = {};
		int tiles = 0;
		for (int ty = 0; ty + 8 <= d.h; ty += 8)
			for (int tx = 0; tx + 8 <= d.w; tx += 8)
			{
				double in[64], out[64], e = 0.0;
				for (int y = 0; y < 8; ++y)
					for (int x = 0; x < 8; ++x) { in[y * 8 + x] = d.at(tx + x, ty + y); e += in[y * 8 + x] * in[y * 8 + x]; }
				e /= 64.0;
				const double N = Nb[static_cast<size_t>((ty + 4) / B) * bw + static_cast<size_t>((tx + 4) / B)];
				if (!(e <= p.psdFlatFactor * N)) continue;
				Dct8x8(in, out);
				for (int k = 0; k < 64; ++k) acc[k] += out[k] * out[k];
				++tiles;
			}
		white = tiles < p.psdMinTiles;
		double mean = 0.0;
		if (!white) for (int k = 0; k < 64; ++k) mean += acc[k];
		mean /= 64.0;
		for (int k = 0; k < 64; ++k) shape[k] = (white || !(mean > 0.0)) ? 1.0 : acc[k] / mean;
		if (!(mean > 0.0)) white = true;
	}

	// The neighbour-window Wiener of the current band. u4 = the unit-4 split (provides D, N via t1.Nb and the shares), t1 = the
	// L1+L2 Result of the same frame (its f gives the fallback; its s1 + s2 are used verbatim at fallback pixels), nb = the
	// candidate neighbours (only those of the current frame's epoch and inside the window count). curEpoch: the current epoch id.
	// Parses grain_repeat_gate (on|off). Unknown text keeps the default (on) and says so; nothing is silent.
	inline bool ParseRepeatGate(const std::string& raw, bool& value, std::string& problem)
	{
		std::string v;
		for (char ch : raw) if (ch != ' ' && ch != '\t') v.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
		if (v == "on") { value = true; return true; }
		if (v == "off") { value = false; return true; }
		problem = "grain_repeat_gate '" + raw + "' is not valid (on|off); kept on";
		value = true;
		return false;
	}

	// C8: is the warped neighbour a near-bit-identical repeat of the current band? Per block (needs at least a quarter of its
	// pixels valid) the mean squared residual against N; the neighbour is a repeat when repeatShare of the judged blocks are quiet.
	inline bool IsRepeat(const Warped& wk, const Plane& dCur, const std::vector<double>& Nb, int B, int bw, const Params& p)
	{
		const int w = dCur.w, h = dCur.h, bh = (h + B - 1) / B;
		std::vector<double> sum(static_cast<size_t>(bw) * bh, 0.0);
		std::vector<int> cnt(static_cast<size_t>(bw) * bh, 0);
		for (int y = 0; y < h; ++y)
			for (int x = 0; x < w; ++x)
			{
				const size_t i = static_cast<size_t>(y) * w + x;
				if (!wk.valid[i]) continue;
				const double r = wk.v.v[i] - dCur.v[i];
				const size_t b = static_cast<size_t>(y / B) * bw + static_cast<size_t>(x / B);
				sum[b] += r * r; ++cnt[b];
			}
		int judged = 0, quiet = 0;
		for (size_t b = 0; b < sum.size() && b < Nb.size(); ++b)
		{
			if (cnt[b] < (B * B) / 4) continue;
			++judged;
			if (sum[b] / cnt[b] <= p.repeatFrac * Nb[b]) ++quiet;
		}
		return judged > 0 && quiet >= p.repeatShare * judged;
	}

	inline NeighbourResult NeighbourWiener(const Split& u4, const Result& t1, const std::vector<Neighbour>& nb, int curEpoch, const Window& win, const Params& p)
	{
		const int w = u4.w, h = u4.h, B = u4.B, bw = u4.bw;
		const size_t npx = static_cast<size_t>(w) * h;
		NeighbourResult out;
		out.cleanD = Plane(w, h, 0.0); out.grainD = Plane(w, h, 0.0); out.keff = Plane(w, h, 0.0);
		out.pixels = static_cast<int>(npx);
		const Plane dCur = BandD(u4);
		// Fallback frame: the L1+L2 gains (s1 + s2 verbatim).
		Plane t1D(w, h);
		for (int y = 0; y < h; ++y)
			for (int x = 0; x < w; ++x) t1D.at(x, y) = t1.s1.at(x, y) + t1.s2.at(x, y);

		std::vector<Warped> warps;
		for (const Neighbour& n : nb)
		{
			if (!n.d || !n.field.Has()) continue;
			if (n.epoch != curEpoch) { ++out.droppedCrossEpoch; continue; }
			const bool behind = n.offset < 0;
			if (n.offset == 0 || (behind ? -n.offset > win.back : n.offset > win.ahead)) continue;
			Warped wk = WarpBicubic(*n.d, n.field, w, h);
			if (p.repeatGate && IsRepeat(wk, dCur, t1.Nb, B, bw, p)) { ++out.droppedRepeat; continue; }
			warps.push_back(std::move(wk));
		}
		out.usedNeighbours = static_cast<int>(warps.size());
		const int K = static_cast<int>(warps.size());

		// per-pixel candidates
		std::vector<int> cand(npx, 0);
		std::vector<double> sum(npx, 0.0);
		for (int k = 0; k < K; ++k)
			for (size_t i = 0; i < npx; ++i)
				if (warps[static_cast<size_t>(k)].valid[i]) { ++cand[i]; sum[i] += warps[static_cast<size_t>(k)].v.v[i]; }

		// leave-one-out weights: keep_k(x) = boxmean5x5(r_k^2) <= looGate (N + V), r_k = W_k - mean of the OTHER candidates, V = N / (K' - 1)
		std::vector<uint8_t> keep(static_cast<size_t>(K) * npx, 0);
		for (int k = 0; k < K; ++k)
		{
			std::vector<double> r2(npx, 0.0);
			std::vector<uint8_t> has(npx, 0);
			for (size_t i = 0; i < npx; ++i)
			{
				if (!warps[static_cast<size_t>(k)].valid[i] || cand[i] < 2) continue;
				const double others = (sum[i] - warps[static_cast<size_t>(k)].v.v[i]) / static_cast<double>(cand[i] - 1);
				const double rr = warps[static_cast<size_t>(k)].v.v[i] - others;
				r2[i] = rr * rr; has[i] = 1;
			}
			for (int y = 0; y < h; ++y)
				for (int x = 0; x < w; ++x)
				{
					const size_t i = static_cast<size_t>(y) * w + x;
					if (!has[i]) continue;
					double s = 0.0; int c = 0;
					for (int j = -2; j <= 2; ++j)
						for (int ii = -2; ii <= 2; ++ii)
						{
							const int xx = x + ii, yy = y + j;
							if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
							const size_t q = static_cast<size_t>(yy) * w + xx;
							if (has[q]) { s += r2[q]; ++c; }
						}
					const double N = t1.Nb[static_cast<size_t>(y / B) * bw + static_cast<size_t>(x / B)];
					const double V = N / static_cast<double>(cand[i] - 1);
					keep[static_cast<size_t>(k) * npx + i] = (c > 0 && s / c <= p.looGate * (N + V)) ? 1 : 0;
				}
		}

		// neighbour mean of the kept
		Plane m(w, h, 0.0);
		std::vector<int> keffI(npx, 0);
		for (size_t i = 0; i < npx; ++i)
		{
			double s = 0.0; int c = 0;
			for (int k = 0; k < K; ++k)
				if (keep[static_cast<size_t>(k) * npx + i]) { s += warps[static_cast<size_t>(k)].v.v[i]; ++c; }
			keffI[i] = c;
			m.v[i] = c >= p.minNeighbours ? s / c : t1D.v[i];   // undefined pixels: the L1+L2 clean stands in inside the DCT tile
		}

		double shape[64];
		PsdShape(dCur, t1.Nb, B, bw, p, shape, out.psdWhite);

		// per-frequency Wiener, 4 half-shifted tile grids, each pixel covered once per grid, averaged
		Plane acc(w, h, 0.0);
		const int shiftTable[4][2] = { {0, 0}, {4, 0}, {0, 4}, {4, 4} };
		const int shiftTable2[2][2] = { {0, 0}, {4, 4} };
		const bool two = p.dctShifts == 2;
		const int nShift = two ? 2 : 4;
		const double gridW = 1.0 / nShift;
		for (int sidx = 0; sidx < nShift; ++sidx)
			for (int ty = -8 + (two ? shiftTable2[sidx][1] : shiftTable[sidx][1]); ty < h; ty += 8)
				for (int tx = -8 + (two ? shiftTable2[sidx][0] : shiftTable[sidx][0]); tx < w; tx += 8)
				{
					double in[64], co[64], rec[64];
					double kt = 0.0, nt = 0.0; int kc = 0;
					for (int y = 0; y < 8; ++y)
						for (int x = 0; x < 8; ++x)
						{
							const int xx = (std::max)(0, (std::min)(w - 1, tx + x)), yy = (std::max)(0, (std::min)(h - 1, ty + y));
							in[y * 8 + x] = m.at(xx, yy);
							const size_t i = static_cast<size_t>(yy) * w + xx;
							if (keffI[i] >= p.minNeighbours) { kt += keffI[i]; ++kc; }
							nt += t1.Nb[static_cast<size_t>(yy / B) * bw + static_cast<size_t>(xx / B)];
						}
					if (kc == 0) { for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x) { const int xx = tx + x, yy = ty + y; if (xx >= 0 && yy >= 0 && xx < w && yy < h) acc.at(xx, yy) += gridW * m.at(xx, yy); } continue; }
					kt /= kc; nt /= 64.0;
					Dct8x8(in, co);
					for (int k = 0; k < 64; ++k)
					{
						const double c2 = co[k] * co[k];
						const double V = shape[k] * nt / kt;
						const double g = c2 > 0.0 ? Unit01(1.0 - p.neighbourKappa * V / c2) : 0.0;
						co[k] *= g;
					}
					Idct8x8(co, rec);
					for (int y = 0; y < 8; ++y)
						for (int x = 0; x < 8; ++x)
						{
							const int xx = tx + x, yy = ty + y;
							if (xx >= 0 && yy >= 0 && xx < w && yy < h) acc.at(xx, yy) += gridW * rec[y * 8 + x];
						}
				}

		double kSum = 0.0; int kN = 0;
		for (int y = 0; y < h; ++y)
			for (int x = 0; x < w; ++x)
			{
				const size_t i = static_cast<size_t>(y) * w + x;
				if (keffI[i] >= p.minNeighbours)
				{
					out.cleanD.at(x, y) = acc.at(x, y);
					out.keff.at(x, y) = static_cast<double>(keffI[i]);
					kSum += keffI[i]; ++kN;
				}
				else
				{
					out.cleanD.at(x, y) = t1D.at(x, y);
					++out.fallbackPixels;
				}
				out.grainD.at(x, y) = dCur.at(x, y) - out.cleanD.at(x, y);
			}
		out.meanKeff = kN ? kSum / kN : 0.0;
		return out;
	}

	// ---- metrics used by the tests and by the T5 replay ------------------------------------------------------------------------
	// Detail loss with FROZEN gains (study proof method): the share of the true structure band energy that ends up in the grain
	// layer, leak = sum (1 - f)^2 s^2 / sum s^2 over the interior (margin px dropped). s = the structure-only band D.
	inline double FrozenLeak(const Plane& f, const Plane& s, int margin)
	{
		double num = 0.0, den = 0.0;
		for (int y = margin; y < s.h - margin; ++y)
			for (int x = margin; x < s.w - margin; ++x)
			{
				const double a = 1.0 - f.at(x, y);
				num += a * a * s.at(x, y) * s.at(x, y);
				den += s.at(x, y) * s.at(x, y);
			}
		return den > 0.0 ? num / den : 0.0;
	}
}
