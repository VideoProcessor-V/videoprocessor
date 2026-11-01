// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// TemporalDetail CPU reference model (README.md steps 1-10). One luma plane in codes (doubles),
// positions in plane pixels with pixel i at index coordinate i, edges clamped. Deterministic: raster
// loops, double sums, no threads, no unordered containers, no random numbers. Header-only, C++14-compatible.
//
// Differences from the shader plan that the model deliberately keeps simple: the base band is held at
// full resolution (the shader holds it at half resolution), and the 4-tap Catmull-Rom is separable.
//
// Usage: one Model per stream. Process() per presented source frame with its sequence, content rate,
// plane lines, indicator stamps, cut/fade flags, a motion source and (queued modes) the future frames;
// Reset() at discontinuities; UpdateSettings() for a settings change (it knows which fields changed).
#pragma once

#include <VP-V/modules/repair/grain_split/GrainSplit.h>
#include <VP-V/modules/repair/temporal_detail/TemporalDetail.h>
#include <VP-V/modules/repair/temporal_detail/TemporalDetailMotion.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace TemporalDetail
{
	// The shared grain-free picture's plane type (repair.grain_split).
	using Plane = GrainSplit::Plane;

	struct Diagnostics
	{
		std::string status = "off";    // why the module is (not) acting: off, ok, reset: <reason>, ...
		int bw = 0, bh = 0, blockPx = 0;
		double scale = 1.0;
		int resets = 0, refreshes = 0, retires = 0, liveSlots = 0, frames = 0;
		// per block, raster
		std::vector<double> weight, boost, target, ageSeconds, uncertainty, baseMatch, corr, corrGate, stronger,
			highBand, focus, referee, evidence, rebounds, q, qref, hard;
		// grain-free transfer: per-band transfer share (1 - lambda) x coherence, template members, template grain level
		std::vector<double> tau1, tau2, templateGrain;
		std::vector<int> templateSlots;
		// output-vs-input grain guard: factor applied to the weight, measured violation, current grain added back
		std::vector<double> guard, guardViolation, addBack;
		std::vector<uint8_t> guardOn;
		int guardTrips = 0, guardActive = 0, addBackBlocks = 0;
		std::vector<int> slotId;                  // -1 none
		std::vector<uint8_t> retired, refreshed, usedFuture, camera;
		// input grain G (the shared split), input structure S, output structure, output grain (G plus any guard add-back), base band
		Plane grain, structure, structureOut, grainOut, base;

		bool operator==(const Diagnostics& o) const
		{
			return status == o.status && bw == o.bw && bh == o.bh && blockPx == o.blockPx && resets == o.resets &&
				refreshes == o.refreshes && retires == o.retires && liveSlots == o.liveSlots && frames == o.frames &&
				weight == o.weight && boost == o.boost && target == o.target && ageSeconds == o.ageSeconds &&
				uncertainty == o.uncertainty && baseMatch == o.baseMatch && corr == o.corr && corrGate == o.corrGate &&
				stronger == o.stronger && highBand == o.highBand && focus == o.focus && referee == o.referee &&
				evidence == o.evidence && rebounds == o.rebounds && q == o.q && qref == o.qref && hard == o.hard &&
				slotId == o.slotId && retired == o.retired && refreshed == o.refreshed && usedFuture == o.usedFuture &&
				camera == o.camera && grain.v == o.grain.v && structure.v == o.structure.v &&
				structureOut.v == o.structureOut.v && grainOut.v == o.grainOut.v && base.v == o.base.v &&
				tau1 == o.tau1 && tau2 == o.tau2 && templateGrain == o.templateGrain && templateSlots == o.templateSlots &&
				guard == o.guard && guardViolation == o.guardViolation && addBack == o.addBack && guardOn == o.guardOn &&
				guardTrips == o.guardTrips && guardActive == o.guardActive && addBackBlocks == o.addBackBlocks;
		}
	};

	namespace Detail
	{
		using GrainSplit::Gauss;

		// The 4-tap Catmull-Rom of README step 5, separable.
		struct Taps
		{
			int x0 = 0, y0 = 0;
			double wx[4] = {}, wy[4] = {};
		};

		inline void CatmullRom(double t, double* wt)
		{
			const double t2 = t * t, t3 = t2 * t;
			wt[0] = -0.5 * t3 + t2 - 0.5 * t;
			wt[1] = 1.5 * t3 - 2.5 * t2 + 1.0;
			wt[2] = -1.5 * t3 + 2.0 * t2 + 0.5 * t;
			wt[3] = 0.5 * t3 - 0.5 * t2;
		}

		inline Taps MakeTaps(double x, double y)
		{
			Taps t;
			const double fx = std::floor(x), fy = std::floor(y);
			t.x0 = static_cast<int>(fx) - 1;
			t.y0 = static_cast<int>(fy) - 1;
			CatmullRom(x - fx, t.wx);
			CatmullRom(y - fy, t.wy);
			return t;
		}

		inline double SampleTaps(const Plane& p, const Taps& t)
		{
			double a = 0.0;
			if (t.x0 >= 0 && t.y0 >= 0 && t.x0 + 3 < p.w && t.y0 + 3 < p.h)
			{
				for (int j = 0; j < 4; ++j)
				{
					const double* r = &p.v[static_cast<size_t>(t.y0 + j) * p.w + t.x0];
					a += t.wy[j] * (t.wx[0] * r[0] + t.wx[1] * r[1] + t.wx[2] * r[2] + t.wx[3] * r[3]);
				}
				return a;
			}
			for (int j = 0; j < 4; ++j)
			{
				double row = 0.0;
				for (int i = 0; i < 4; ++i) row += t.wx[i] * p.get(t.x0 + i, t.y0 + j);
				a += t.wy[j] * row;
			}
			return a;
		}

		// One frame's analysis (README steps 1-2) on the shared grain split.
		struct Analysis
		{
			int w = 0, h = 0, bw = 0, bh = 0, B = 16;
			Plane y, base, s1, s2, s, grain, f;   // s = s1 + s2 = f D (the grain-free structure), grain = Y - clean
			std::vector<double> e1, e2, e3, F, q, N;
		};

		inline GrainSplit::Params SplitParams()
		{
			GrainSplit::Params p;
			p.sigmaFine = kSigmaFine; p.sigmaBase = kSigmaBase; p.sigmaLow = kSigmaLow;
			p.subBlocks = kSubBlocks; p.quietSubBlocks = kQuietSubBlocks; p.neighbourhood = kGrainNeighbourhood;
			p.grainCal = kGrainCal; p.kappa = kWienerKappa; p.energyFloor = kEnergyFloor;
			return p;
		}

		inline Analysis Analyse(const Plane& y, double scale, int B)
		{
			GrainSplit::Split sp = GrainSplit::Compute(y, scale, B, SplitParams());
			Analysis a;
			a.w = y.w; a.h = y.h; a.B = B; a.bw = sp.bw; a.bh = sp.bh;
			a.y = y;
			a.base = std::move(sp.base);
			a.s1 = std::move(sp.s1);
			a.s2 = std::move(sp.s2);
			a.grain = std::move(sp.grain);
			a.f = std::move(sp.f);
			a.N = std::move(sp.N);
			a.s = Plane(y.w, y.h);
			for (size_t i = 0; i < a.s.v.size(); ++i) a.s.v[i] = a.s1.v[i] + a.s2.v[i];
			const size_t nb = static_cast<size_t>(a.bw) * a.bh;
			a.e1.assign(nb, 0.0); a.e2.assign(nb, 0.0); a.e3.assign(nb, 0.0); a.F.assign(nb, 0.0); a.q.assign(nb, 0.0);
			for (int by = 0; by < a.bh; ++by)
				for (int bx = 0; bx < a.bw; ++bx)
				{
					double s1 = 0.0, s2 = 0.0, s3 = 0.0;
					int n = 0;
					for (int yy = by * B; yy < (std::min)(y.h, (by + 1) * B); ++yy)
						for (int xx = bx * B; xx < (std::min)(y.w, (bx + 1) * B); ++xx)
						{
							s1 += a.s1.at(xx, yy) * a.s1.at(xx, yy);
							s2 += a.s2.at(xx, yy) * a.s2.at(xx, yy);
							s3 += sp.d3.at(xx, yy) * sp.d3.at(xx, yy);
							++n;
						}
					const size_t i = static_cast<size_t>(by) * a.bw + bx;
					a.e1[i] = s1 / n; a.e2[i] = s2 / n; a.e3[i] = s3 / n;
					a.F[i] = a.e2[i] / (a.e2[i] + a.e3[i] + kEnergyFloor);
					a.q[i] = a.e1[i] + kEnergyFloor;
				}
			return a;
		}

		// A stored (past slot) or per-frame (future) reference: structure planes and the composed motion
		// from the current frame to it, on the current frame's block grid (block-centre samples).
		struct Source
		{
			int id = -1;
			uint64_t seq = 0;
			bool future = false;
			Plane s1, s2, s, base, grain;   // grain-free structure bands, base, and the frame's grain (test damage only)
			Plane c1, c2;                   // the cleaned bands (multi-frame cleaner, step 4a): what the template transfers
			std::vector<double> rho2;       // per own block: residual grain energy of c1 + c2 over one full frame's grain (1 = uncleaned)
			bool cleaned = false;
			std::vector<double> N;          // the source frame's block grain level
			std::vector<double> F, q;       // the source frame's own block maps
			std::vector<double> cx, cy, u;  // composed displacement (px) and uncertainty (px) per current-frame block
			double hc[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };   // composed homography current -> source (picture-centred)
			bool camChain = true;           // every hop since the source had a usable camera model
		};

		inline bool Invert3(const double* m, double* o)
		{
			const double a = m[0], b = m[1], c = m[2], d = m[3], e = m[4], f = m[5], g = m[6], h = m[7], i = m[8];
			const double A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
			const double det = a * A + b * B + c * C;
			if (std::fabs(det) < 1e-12) return false;
			const double id = 1.0 / det;
			o[0] = A * id; o[1] = -(b * i - c * h) * id; o[2] = (b * f - c * e) * id;
			o[3] = B * id; o[4] = (a * i - c * g) * id; o[5] = -(a * f - c * d) * id;
			o[6] = C * id; o[7] = -(a * h - b * g) * id; o[8] = (a * e - b * d) * id;
			return true;
		}

		inline void Mul3(const double* a, const double* b, double* o)
		{
			double t[9];
			for (int r = 0; r < 3; ++r)
				for (int c = 0; c < 3; ++c) t[r * 3 + c] = a[r * 3 + 0] * b[0 * 3 + c] + a[r * 3 + 1] * b[1 * 3 + c] + a[r * 3 + 2] * b[2 * 3 + c];
			for (int i = 0; i < 9; ++i) o[i] = t[i];
		}

		// Bilinear read of a block-grid array at pixel position (x, y) (block i's centre at (i + 0.5) B - 0.5).
		inline double GridAt(const std::vector<double>& g, int bw, int bh, int B, double x, double y)
		{
			const double gx = (x + 0.5) / B - 0.5, gy = (y + 0.5) / B - 0.5;
			const double fx = std::floor(gx), fy = std::floor(gy);
			const double tx = gx - fx, ty = gy - fy;
			auto at = [&](int i, int j) { return g[static_cast<size_t>((std::max)(0, (std::min)(bh - 1, j))) * bw + (std::max)(0, (std::min)(bw - 1, i))]; };
			const int i0 = static_cast<int>(fx), j0 = static_cast<int>(fy);
			return (at(i0, j0) * (1 - tx) + at(i0 + 1, j0) * tx) * (1 - ty) + (at(i0, j0 + 1) * (1 - tx) + at(i0 + 1, j0 + 1) * tx) * ty;
		}

		// Both displacement components at once (same bilinear weights).
		inline void GridAt2(const std::vector<double>& gx0, const std::vector<double>& gy0, int bw, int bh, int B, double x, double y, double& ox, double& oy)
		{
			const double gx = (x + 0.5) / B - 0.5, gy = (y + 0.5) / B - 0.5;
			const double fx = std::floor(gx), fy = std::floor(gy);
			const double tx = gx - fx, ty = gy - fy;
			const int i0 = static_cast<int>(fx), j0 = static_cast<int>(fy);
			const int ia = (std::max)(0, (std::min)(bw - 1, i0)), ib = (std::max)(0, (std::min)(bw - 1, i0 + 1));
			const int ja = (std::max)(0, (std::min)(bh - 1, j0)), jb = (std::max)(0, (std::min)(bh - 1, j0 + 1));
			const size_t a = static_cast<size_t>(ja) * bw + ia, b = static_cast<size_t>(ja) * bw + ib, c = static_cast<size_t>(jb) * bw + ia, d = static_cast<size_t>(jb) * bw + ib;
			const double w00 = (1 - tx) * (1 - ty), w10 = tx * (1 - ty), w01 = (1 - tx) * ty, w11 = tx * ty;
			ox = gx0[a] * w00 + gx0[b] * w10 + gx0[c] * w01 + gx0[d] * w11;
			oy = gy0[a] * w00 + gy0[b] * w10 + gy0[c] * w01 + gy0[d] * w11;
		}

		inline double PairAt(const MotionPair& m, const std::vector<float>& g, double x, double y)
		{
			const double gx = (x + m.originX + 0.5) / m.blockPx - 0.5, gy = (y + m.originY + 0.5) / m.blockPx - 0.5;
			const double fx = std::floor(gx), fy = std::floor(gy);
			const double tx = gx - fx, ty = gy - fy;
			auto at = [&](int i, int j) { return static_cast<double>(g[static_cast<size_t>((std::max)(0, (std::min)(m.gridH - 1, j))) * m.gridW + (std::max)(0, (std::min)(m.gridW - 1, i))]); };
			const int i0 = static_cast<int>(fx), j0 = static_cast<int>(fy);
			return (at(i0, j0) * (1 - tx) + at(i0 + 1, j0) * tx) * (1 - ty) + (at(i0, j0 + 1) * (1 - tx) + at(i0 + 1, j0 + 1) * tx) * ty;
		}

		inline bool PairUsable(const MotionPair& m)
		{
			const size_t n = static_cast<size_t>((std::max)(0, m.gridW)) * static_cast<size_t>((std::max)(0, m.gridH));
			return m.valid && n > 0 && m.vx.size() >= n && m.vy.size() >= n && m.trust.size() >= n && m.blockPx > 0.0;
		}

		struct BlockState
		{
			int slot = -1, prevSlot = -1;
			double xf = 1.0;
			double qref = 0.0;
			double wLim = 0.0, hSm = 0.0, tHold = 0.0;
			bool on = false;
			double bLim = 0.0;
			bool bOn = false;
			bool have = false;
			double qPrev = 0.0, e2Prev = 0.0;
			int rstate = 0;
			double qHigh = 0.0, qLow = 0.0, rebounds = 0.0;
			int softRun = 0, vetoRun = 0;
			bool retired = false;
			double qPeak = 0.0, e2Peak = 0.0, fPeak = 0.0;
			// output-vs-input grain guard: factor, hysteresis flag, EMA of the tile sums
			// (Ein, Eout, cross, Lin, Lout, Pin, Pout: see Model::Process step 10)
			double guard = 1.0;
			bool guardOn = false;
			double ema[7] = { 0, 0, 0, 0, 0, 0, 0 };
		};

		struct Eval
		{
			bool valid = false;
			double ageSeconds = 0, ageGate = 0, u = 0, uncGate = 0, baseDiff = 0, baseGate = 0, corr = 0, corrGate = 0,
				strongerGate = 0, harmGate = 0, hbGate = 0, focusGate = 0, e1w = 0, e1c = 0;
			double hard = 0;   // unc * base * corr * harm * hb * focus (instant fall); strongerGate is soft
		};

		// One template member: a source and its coefficient (README step 8).
		struct Member
		{
			const Source* s = nullptr;
			double coef = 0.0;
			double rho2 = 1.0;               // the member's residual grain share at the block (1 = uncleaned)
		};

		// The multi-frame cleaner's transform (step 4a): orthonormal P x P DCT-II, sine window (w^2 sums to 1 at a half-patch
		// step, so analysis x synthesis windows reconstruct exactly with unit gains), and the grain spectrum shape of the
		// detail band D = Y - G(sigmaBase) Y: hs = |1 - G^(w)|^2 / mean |1 - G^|^2 over the band, so a coefficient's grain
		// variance is N hs mean(window^2 f^2) (N: the split's block grain level, the energy of D for pure grain).
		struct CleanBasis
		{
			int P = 0;
			double sigma = 0.0;
			std::vector<double> C, win, hs;
		};

		inline CleanBasis MakeCleanBasis(int P, double sigma)
		{
			CleanBasis b;
			b.P = P; b.sigma = sigma;
			b.C.assign(static_cast<size_t>(P) * P, 0.0);
			b.win.assign(static_cast<size_t>(P), 0.0);
			b.hs.assign(static_cast<size_t>(P) * P, 0.0);
			const double pi = 3.14159265358979323846;
			for (int k = 0; k < P; ++k)
				for (int x = 0; x < P; ++x)
					b.C[static_cast<size_t>(k) * P + x] = std::sqrt((k ? 2.0 : 1.0) / P) * std::cos(pi * (2 * x + 1) * k / (2.0 * P));
			for (int x = 0; x < P; ++x) b.win[static_cast<size_t>(x)] = std::sin(pi * (x + 0.5) / P);
			auto H2 = [&](double wx, double wy) { const double h = 1.0 - std::exp(-0.5 * sigma * sigma * (wx * wx + wy * wy)); return h * h; };
			const int G = 256;
			double mean = 0.0;
			for (int j = 0; j < G; ++j)
				for (int i = 0; i < G; ++i) mean += H2(pi * (i + 0.5) / G, pi * (j + 0.5) / G);
			mean /= static_cast<double>(G) * G;
			for (int ky = 0; ky < P; ++ky)
				for (int kx = 0; kx < P; ++kx)
					b.hs[static_cast<size_t>(ky) * P + kx] = H2(pi * kx / P, pi * ky / P) / mean;
			return b;
		}

		// Separable 2-D DCT (forward: inv = false) of a P x P block, fixed summation order.
		inline void Dct2(const CleanBasis& b, const double* in, double* out, double* tmp, bool inv)
		{
			const int P = b.P;
			const double* C = b.C.data();
			for (int y = 0; y < P; ++y)
				for (int k = 0; k < P; ++k)
				{
					double a = 0.0;
					if (!inv) for (int x = 0; x < P; ++x) a += C[static_cast<size_t>(k) * P + x] * in[y * P + x];
					else for (int x = 0; x < P; ++x) a += C[static_cast<size_t>(x) * P + k] * in[y * P + x];
					tmp[y * P + k] = a;
				}
			for (int k = 0; k < P; ++k)
				for (int x = 0; x < P; ++x)
				{
					double a = 0.0;
					if (!inv) for (int y = 0; y < P; ++y) a += C[static_cast<size_t>(k) * P + y] * tmp[y * P + x];
					else for (int y = 0; y < P; ++y) a += C[static_cast<size_t>(y) * P + k] * tmp[y * P + x];
					out[k * P + x] = a;
				}
		}

		struct Contribution
		{
			const Source* a = nullptr;       // the block's selected reference
			const Source* prev = nullptr;
			double xf = 1.0, w = 0.0, boost = 0.0;
			bool cam = false;
			Member mem[kSlots + 1];          // the template: every valid slot (or the future source), warped once each
			int nm = 0;
			double tau1 = 0.0, tau2 = 0.0;   // per-band (1 - lambda) x coherence
			double lam1 = 0.0, lam2 = 0.0;   // per-band projection of the current grain-free structure onto the template
			double nT = 0.0;                 // the template's relative grain energy: sum coef_k^2 (1 = one member frame's grain)
		};
	}

	class Model
	{
	public:
		struct FutureFrame { const Plane* luma = nullptr; uint64_t seq = 0; };

		struct FrameIn
		{
			Plane luma;
			uint64_t seq = 0;
			double contentHz = 0.0;       // <= 0: the reference rate
			double planeLines = 1080.0;   // sets Scale independent of the image size (tests simulate 8K on small planes)
			StampView stamps;
			bool cut = false, fade = false;
			const IMotionSource* motion = nullptr;
			std::vector<FutureFrame> future;   // queued modes: seq + 1, seq + 2, ... (the referee and future slot)
			std::vector<double> quality;       // optional per-block bitrate quality 0..1 on the active picture's block grid (empty = neutral 1)
			ActiveRect active;                 // the active picture (empty = the whole plane)
			bool autoEngaged = false;          // temporal_detail = auto: the host's AutoGate decision for this frame
			// Regionally adaptive gates (owner 2026-10-31): per block of the active picture's grid, 4 factors on the tunables -
			// [0] reference-stronger margin, [1] base-match allowance, [2] motion-uncertainty limit, [3] strength. A rule
			// computed from the local maps (texture deficit, vector / camera confidence, grain level, brightness, I/B phase;
			// see README "Regionally adaptive gates") fills it; empty or short = 1 (the global tunables).
			std::vector<float> blockGates;
		};

		// Model-only knobs for measurement and the guard's own test (not user settings, never in the config):
		// the template grain budget, the output-vs-input grain guard switch, and deliberate damage that the guard
		// must catch (forceDrop removes that share of the current frame's grain, forceFreeze adds that share of the
		// selected reference's warped grain, both scaled by the block weight).
		struct Tuning
		{
			double grainBudget = kGrainBudget;   // <= 0: no budget (the transfer at its designed strength)
			bool templateClean = true;           // the multi-frame template cleaner (false: the single-frame split's bands, for measurement)
			double cleanKappa = kCleanKappa;
			double guardTemplateShare = kGuardTemplateShare;   // <= 0: the guard reads every low-f pixel (no structure mask)
			bool guard = true;
			// > 0: every block with a selected reference transfers at this weight, gates and limiter bypassed (the transfer's
			// ceiling, for measurement only - test/temporal-detail-real; the projection, coherence, budget and guard still apply)
			double forceWeight = 0.0;
			double forceDrop = 0.0, forceFreeze = 0.0;
			int slots = kSlots;                  // reference-store tier: past slots kept (1 .. kSlots; small cards keep fewer)
			GateTuning gates;                    // the gate thresholds (re-tuned on real B-frames)
		};

		explicit Model(const Settings& settings) : m_set(settings) {}

		// A change of the slot tier restarts the store (as the GPU module does).
		void SetTuning(const Tuning& t)
		{
			const bool reset = t.slots != m_tune.slots;
			m_tune = t;
			if (reset) Reset("tier");
		}

		const Settings& GetSettings() const { return m_set; }

		// A mode or max_age change resets; strength and boost_max change the target only.
		void UpdateSettings(const Settings& s)
		{
			const bool reset = s.mode != m_set.mode || s.maxAgeSeconds != m_set.maxAgeSeconds;
			m_set = s;
			if (reset) Reset("settings");
		}

		void Reset(const char* reason)
		{
			m_slots.clear();
			m_blocks.clear();
			m_nextId = 0;
			m_haveFrame = false;
			m_havePrevG = false;
			++m_diag.resets;
			m_diag.status = std::string("reset: ") + (reason ? reason : "");
		}

		const Diagnostics& Diag() const { return m_diag; }

		// Processes the active picture (in.active) as the whole plane: its border is the plane edge for every filter,
		// statistic and warp; pixels outside it are copied unchanged (bit-exact).
		Plane Process(const FrameIn& in);

	private:
		using Source = Detail::Source;
		using Analysis = Detail::Analysis;
		using BlockState = Detail::BlockState;
		using Eval = Detail::Eval;
		using Contribution = Detail::Contribution;

		Source* FindSlot(int id)
		{
			for (Source& s : m_slots) if (s.id == id) return &s;
			return nullptr;
		}

		Eval EvaluateBlock(int bx, int by, const Analysis& A, const Source& src, bool cam, double hz) const;
		void Compose(Source& s, const MotionPair& bwd, const MotionPair* fwd, const CameraPairView* cam, const Analysis& A, const std::vector<uint8_t>& camBlock);
		void Position(const Source& s, bool cam, int px, int py, double& x, double& y) const;
		void TemplateAt(const Contribution& c, int x, int y, double baseCur, double& t1, double& t2) const;
		void CleanSlot(Source& s, const Analysis& A, const std::vector<const Source*>& mem, const std::vector<uint8_t>& memValid, const std::vector<uint8_t>& camBlock);
		double Rho2At(const Source& s, bool cam, int bx, int by) const;
		Plane ProcessActive(const FrameIn& in);

		Settings m_set;
		Detail::CleanBasis m_basis;
		Tuning m_tune;
		Diagnostics m_diag;
		Plane m_prevGin, m_prevGout;   // the previous frame's input and output grain on the shared map (guard lag-1)
		bool m_havePrevG = false;
		Plane m_lastOut;
		std::vector<Source> m_slots;
		std::vector<BlockState> m_blocks;
		int m_nextId = 0;
		bool m_haveFrame = false;
		uint64_t m_prevSeq = 0;
		StampView m_stamps;
		int m_w = 0, m_h = 0, m_B = 16;
		double m_scale = 1.0;
		double m_camCx = 0.0, m_camCy = 0.0;
		std::vector<float> m_blockGates;  // this frame's per-block gate factors (FrameIn::blockGates)
		double Bg(size_t bi, int k) const
		{
			const size_t i = bi * 4 + static_cast<size_t>(k);
			return i < m_blockGates.size() ? SanitizeBlockGate(static_cast<double>(m_blockGates[i]), k) : 1.0;
		}
		ActiveRect m_rect;                // the last frame's resolved active picture (plane coordinates)
		bool m_rectMoved = false;         // its origin moved at equal size: a geometry reset
		Plane m_lastFull;                 // the last full-plane output (repeats)
	};

	inline void Model::Position(const Source& s, bool cam, int px, int py, double& x, double& y) const
	{
		if (cam && s.camChain)
		{
			const double X = px + 0.5 - m_camCx, Y = py + 0.5 - m_camCy;
			const double a = s.hc[0] * X + s.hc[1] * Y + s.hc[2];
			const double b = s.hc[3] * X + s.hc[4] * Y + s.hc[5];
			const double c = s.hc[6] * X + s.hc[7] * Y + s.hc[8];
			const double ic = (c != 0.0) ? 1.0 / c : 0.0;
			x = a * ic + m_camCx - 0.5;
			y = b * ic + m_camCy - 0.5;
			return;
		}
		const int bw = (m_w + m_B - 1) / m_B, bh = (m_h + m_B - 1) / m_B;
		double dx, dy;
		Detail::GridAt2(s.cx, s.cy, bw, bh, m_B, px, py, dx, dy);
		x = px + dx;
		y = py + dy;
	}

	// The block's reference template at pixel (x, y), per band: T_b = sum_k coef_k g_k S_b,k(p + C_k(p)), each member
	// sampled once from its unwarped slot (warp once), g_k = base / base_w (lighting).
	inline void Model::TemplateAt(const Contribution& c, int x, int y, double baseCur, double& t1, double& t2) const
	{
		t1 = 0.0; t2 = 0.0;
		for (int k = 0; k < c.nm; ++k)
		{
			const Source& s = *c.mem[k].s;
			double sx, sy;
			Position(s, c.cam && !s.future, x, y, sx, sy);
			const Detail::Taps t = Detail::MakeTaps(sx, sy);
			const double bwv = Detail::SampleTaps(s.base, t);
			const double g = (bwv > 1e-3 && baseCur > 1e-3) ? Clamp(baseCur / bwv, kGainLo, kGainHi) : 1.0;
			const double cg = c.mem[k].coef * g;
			const bool clean = m_tune.templateClean && s.cleaned;
			t1 += cg * Detail::SampleTaps(clean ? s.c1 : s.s1, t);
			t2 += cg * Detail::SampleTaps(clean ? s.c2 : s.s2, t);
		}
	}

	// A source's residual grain share at the current block (its own block under the composed block centre).
	inline double Model::Rho2At(const Source& s, bool cam, int bx, int by) const
	{
		if (!(m_tune.templateClean && s.cleaned) || s.rho2.empty()) return 1.0;
		const int bw = (m_w + m_B - 1) / m_B, bh = (m_h + m_B - 1) / m_B;
		double sx, sy;
		Position(s, cam, static_cast<int>((bx + 0.5) * m_B - 0.5), static_cast<int>((by + 0.5) * m_B - 0.5), sx, sy);
		const int sbx = (std::max)(0, (std::min)(bw - 1, static_cast<int>(std::floor((sx + 0.5) / m_B))));
		const int sby = (std::max)(0, (std::min)(bh - 1, static_cast<int>(std::floor((sy + 0.5) / m_B))));
		return s.rho2[static_cast<size_t>(sby) * bw + sbx];
	}

	// The multi-frame template cleaner (README step 4a), run once when the current frame is admitted as slot s (its own
	// coordinates = the current frame's). Overlapping P x P DCT patches (sine window, half-patch step). Per coefficient:
	// X0 = the new slot's grain-free structure, Xm = each other valid slot's structure as the template already samples it
	// (warped once to the current frame, lighting gain), evidence only: nothing warped is stored. Grain is independent
	// between frames, structure coherent, so the cross terms X0 Xm estimate the structure power without grain bias:
	//   P = max(0, (X0^2 - V0 + sum_m X0 Xm) / (1 + M)),  W = P / (P + kappa V0),  V0 = N hs mean(window^2 f^2).
	// The cleaned bands c_b = sum_patches window IDCT(W DCT(window s_b)) are stored beside the raw bands (the gates keep
	// reading the raw bands). rho2 per block = sum W^2 V0 / sum N hs mean(window^2): the residual grain energy over one
	// full frame's grain, which the template grain budget uses in place of 1.
	inline void Model::CleanSlot(Source& s, const Analysis& A, const std::vector<const Source*>& mem, const std::vector<uint8_t>& memValid, const std::vector<uint8_t>& camBlock)
	{
		const int w = A.w, h = A.h, B = m_B, bw = A.bw, bh = A.bh;
		const int P = (std::max)(4, 2 * static_cast<int>(std::floor(0.5 * kCleanPatchPx * m_scale + 0.5)));
		const double sigma = kSigmaBase * m_scale;
		if (m_basis.P != P || m_basis.sigma != sigma) m_basis = Detail::MakeCleanBasis(P, sigma);
		const Detail::CleanBasis& cb = m_basis;
		const size_t PP = static_cast<size_t>(P) * P, nb = static_cast<size_t>(bw) * bh;
		s.c1 = Plane(w, h); s.c2 = Plane(w, h);
		std::vector<double> rg(nb, 0.0), rv(nb, 0.0);
		std::vector<double> x1(PP), x2(PP), xm(PP), X1(PP), X2(PP), Xm(PP), cross(PP), tmp(PP), wgt(PP);
		const int half = P / 2;
		for (int oy = -half; oy < h; oy += half)
			for (int ox = -half; ox < w; ox += half)
			{
				const int cbx = (std::max)(0, (std::min)(bw - 1, (ox + half) / B)), cby = (std::max)(0, (std::min)(bh - 1, (oy + half) / B));
				const size_t bi = static_cast<size_t>(cby) * bw + cbx;
				double wf2 = 0.0, w2 = 0.0;
				// Outside the plane the patch is zero (no replicated edge pixels: replication would turn the edge's grain
				// into column/row patterns that read as structure); the grain variance counts in-plane pixels only.
				for (int j = 0; j < P; ++j)
					for (int i = 0; i < P; ++i)
					{
						const int xx = ox + i, yy = oy + j;
						const size_t k = static_cast<size_t>(j) * P + i;
						if (xx < 0 || yy < 0 || xx >= w || yy >= h) { x1[k] = 0.0; x2[k] = 0.0; continue; }
						const double wv = cb.win[static_cast<size_t>(i)] * cb.win[static_cast<size_t>(j)];
						const double f = A.f.at(xx, yy);
						x1[k] = wv * A.s1.at(xx, yy);
						x2[k] = wv * A.s2.at(xx, yy);
						wf2 += wv * wv * f * f; w2 += wv * wv;
					}
				wf2 /= static_cast<double>(PP); w2 /= static_cast<double>(PP);
				Detail::Dct2(cb, x1.data(), X1.data(), tmp.data(), false);
				Detail::Dct2(cb, x2.data(), X2.data(), tmp.data(), false);
				std::fill(cross.begin(), cross.end(), 0.0);
				int M = 0;
				for (size_t m = 0; m < mem.size(); ++m)
				{
					if (!memValid[m * nb + bi]) continue;
					const Source& ms = *mem[m];
					const bool cam = camBlock[bi] != 0;
					for (int j = 0; j < P; ++j)
						for (int i = 0; i < P; ++i)
						{
							const int xx = ox + i, yy = oy + j;
							if (xx < 0 || yy < 0 || xx >= w || yy >= h) { xm[static_cast<size_t>(j) * P + i] = 0.0; continue; }
							double sx, sy;
							Position(ms, cam, xx, yy, sx, sy);
							const Detail::Taps t = Detail::MakeTaps(sx, sy);
							const double bwv = Detail::SampleTaps(ms.base, t), bc = A.base.at(xx, yy);
							const double g = (bwv > 1e-3 && bc > 1e-3) ? Clamp(bc / bwv, kGainLo, kGainHi) : 1.0;
							xm[static_cast<size_t>(j) * P + i] = cb.win[static_cast<size_t>(i)] * cb.win[static_cast<size_t>(j)] * g * Detail::SampleTaps(ms.s, t);
						}
					Detail::Dct2(cb, xm.data(), Xm.data(), tmp.data(), false);
					for (size_t k = 0; k < PP; ++k) cross[k] += (X1[k] + X2[k]) * Xm[k];
					++M;
				}
				const double N = (std::max)(0.0, A.N[bi]);
				for (size_t k = 0; k < PP; ++k)
				{
					const double X0 = X1[k] + X2[k], V0 = N * cb.hs[k] * wf2;
					double W = 1.0;
					if (V0 > 0.0)
					{
						const double Pw = (std::max)(0.0, (X0 * X0 - V0 + cross[k]) / (1.0 + M));
						W = Pw / (Pw + m_tune.cleanKappa * V0);
					}
					X1[k] *= W; X2[k] *= W;
					rg[bi] += W * W * V0;
					rv[bi] += N * cb.hs[k] * w2;
				}
				Detail::Dct2(cb, X1.data(), x1.data(), tmp.data(), true);
				Detail::Dct2(cb, X2.data(), x2.data(), tmp.data(), true);
				for (int j = 0; j < P; ++j)
				{
					const int yy = oy + j;
					if (yy < 0 || yy >= h) continue;
					for (int i = 0; i < P; ++i)
					{
						const int xx = ox + i;
						if (xx < 0 || xx >= w) continue;
						const double wv = cb.win[static_cast<size_t>(i)] * cb.win[static_cast<size_t>(j)];
						s.c1.at(xx, yy) += wv * x1[static_cast<size_t>(j) * P + i];
						s.c2.at(xx, yy) += wv * x2[static_cast<size_t>(j) * P + i];
					}
				}
			}
		s.rho2.assign(nb, 1.0);
		for (size_t i = 0; i < nb; ++i)
			if (rv[i] > 0.0) s.rho2[i] = (std::min)(1.0, rg[i] / rv[i]);
		s.cleaned = true;
	}

	// One backward hop (frame n -> n-1) for a stored source: C_n(p) = Vb(p) + C_{n-1}(p + Vb(p)),
	// u_n(p) = u_{n-1}(p + Vb(p)) + (1 - trust) block + |Vf + Vb|; the camera chain composes homographies.
	inline void Model::Compose(Source& s, const MotionPair& bwd, const MotionPair* fwd, const CameraPairView* cam, const Analysis& A, const std::vector<uint8_t>& camBlock)
	{
		const int bw = A.bw, bh = A.bh, B = m_B;
		std::vector<double> ncx(s.cx.size()), ncy(s.cy.size()), nu(s.u.size());
		for (int by = 0; by < bh; ++by)
			for (int bx = 0; bx < bw; ++bx)
			{
				const size_t i = static_cast<size_t>(by) * bw + bx;
				const double px = (bx + 0.5) * B - 0.5, py = (by + 0.5) * B - 0.5;
				const double vx = Detail::PairAt(bwd, bwd.vx, px, py), vy = Detail::PairAt(bwd, bwd.vy, px, py);
				const double tr = Detail::PairAt(bwd, bwd.trust, px, py);
				double hop = (1.0 - tr) * bwd.blockPx;
				if (cam && cam->usable && camBlock[i]) hop = (1.0 - cam->confidence) * cam->blockSize;
				if (fwd)
				{
					const double fx = Detail::PairAt(*fwd, fwd->vx, px + vx, py + vy), fy = Detail::PairAt(*fwd, fwd->vy, px + vx, py + vy);
					hop += std::sqrt((fx + vx) * (fx + vx) + (fy + vy) * (fy + vy));
				}
				ncx[i] = vx + Detail::GridAt(s.cx, bw, bh, B, px + vx, py + vy);
				ncy[i] = vy + Detail::GridAt(s.cy, bw, bh, B, px + vx, py + vy);
				nu[i] = Detail::GridAt(s.u, bw, bh, B, px + vx, py + vy) + hop;
			}
		s.cx = std::move(ncx); s.cy = std::move(ncy); s.u = std::move(nu);
		if (cam && cam->usable)
		{
			double inv[9];
			if (s.camChain && Detail::Invert3(cam->h, inv)) Detail::Mul3(s.hc, inv, s.hc);
			else s.camChain = false;
		}
		else s.camChain = false;
	}

	// The per-block validation of README step 6 (and the guard's measured parts) against one source.
	inline Detail::Eval Model::EvaluateBlock(int bx, int by, const Analysis& A, const Source& src, bool cam, double hz) const
	{
		Eval e;
		const int B = m_B, bw = A.bw, bh = A.bh;
		const size_t bi = static_cast<size_t>(by) * bw + bx;
		double c1 = 0, c2 = 0, w1 = 0, w2 = 0, cs = 0, ws = 0, dot = 0, bd = 0;
		int n = 0;
		for (int y = by * B; y < (std::min)(A.h, (by + 1) * B); ++y)
			for (int x = bx * B; x < (std::min)(A.w, (bx + 1) * B); ++x)
			{
				double sx, sy;
				Position(src, cam, x, y, sx, sy);
				const Detail::Taps t = Detail::MakeTaps(sx, sy);
				const double s1w = Detail::SampleTaps(src.s1, t), s2w = Detail::SampleTaps(src.s2, t), bwv = Detail::SampleTaps(src.base, t);
				const double s1 = A.s1.at(x, y), s2 = A.s2.at(x, y), sc = s1 + s2, sw = s1w + s2w;
				c1 += s1 * s1; c2 += s2 * s2; w1 += s1w * s1w; w2 += s2w * s2w;
				cs += sc * sc; ws += sw * sw; dot += sc * sw;
				bd += std::fabs(A.base.at(x, y) - bwv);
				++n;
			}
		const double fl = kEnergyFloor;
		e.e1c = c1 / n; e.e1w = w1 / n;
		const double e2c = c2 / n, e2w = w2 / n;
		e.corr = dot / std::sqrt(cs * ws + 1e-12);
		e.corrGate = Smoothstep(0.5 * kCorrMin, kCorrMin, e.corr);
		const GateTuning& gt = m_tune.gates;
		const double margin = gt.strongerMargin * Bg(bi, 0);
		e.strongerGate = Smoothstep(1.0 + margin, 1.0 + 3.0 * margin, (e.e1w + fl) / (e.e1c + fl)) *
			Smoothstep(gt.significanceLo, gt.significanceHi, e.e1w / (A.N[bi] + fl));
		// A reference clearly weaker than the current block would soften it: that veto is instant.
		e.harmGate = Smoothstep(1.0 - 3.0 * margin, 1.0 - margin, (e.e1w + fl) / (e.e1c + fl));
		e.hbGate = Smoothstep(kRho2Lo, kRho2Hi, (e2c + fl) / (e2w + fl));
		// Focus: the source block's F at the composed block centre.
		{
			const double cx = (bx + 0.5) * B - 0.5, cy = (by + 0.5) * B - 0.5;
			double sx, sy;
			Position(src, cam, static_cast<int>(cx), static_cast<int>(cy), sx, sy);
			const int sbx = (std::max)(0, (std::min)(bw - 1, static_cast<int>(std::floor((sx + 0.5) / B)))), sby = (std::max)(0, (std::min)(bh - 1, static_cast<int>(std::floor((sy + 0.5) / B))));
			const double Fw = src.F[static_cast<size_t>(sby) * bw + sbx];
			e.focusGate = Smoothstep(kPullLo, kPullHi, A.F[bi] / (Fw + 1e-6));
			e.u = Detail::GridAt(src.u, bw, bh, B, cx, cy);
		}
		const double uMax = gt.uncertaintyPx * m_scale * Bg(bi, 2);
		e.uncGate = 1.0 - Smoothstep(0.3 * uMax, uMax, e.u);
		e.baseDiff = bd / n;
		const double allow = (gt.baseNoiseCodes + gt.baseGrainSigmas * std::sqrt((std::max)(0.0, A.N[bi])) / 4.0) * Bg(bi, 1);
		e.baseGate = 1.0 - Smoothstep(allow, 2.0 * allow, e.baseDiff);
		const double hzv = hz > 0.0 ? hz : kReferenceHz;
		const double ageFrames = src.future ? static_cast<double>(src.seq) - static_cast<double>(m_prevSeq + 1) : static_cast<double>(m_prevSeq + 1) - static_cast<double>(src.seq);
		e.ageSeconds = ageFrames / hzv;
		e.ageGate = Clamp((m_set.maxAgeSeconds - e.ageSeconds) / (m_set.maxAgeSeconds * kAgeFadeShare), 0.0, 1.0);
		e.hard = e.uncGate * e.baseGate * e.corrGate * e.harmGate * e.hbGate * e.focusGate;
		e.valid = true;
		return e;
	}

	namespace Detail
	{
		// The motion source seen from an active-picture crop at (ox, oy): same pairs, grids read at the plane position.
		class OffsetMotionSource : public IMotionSource
		{
		public:
			OffsetMotionSource(const IMotionSource* inner, double ox, double oy) : m_inner(inner), m_ox(ox), m_oy(oy) {}
			bool Backward(uint64_t seq, MotionPair& out) const override { return Shift(m_inner && m_inner->Backward(seq, out), out); }
			bool Forward(uint64_t seq, MotionPair& out) const override { return Shift(m_inner && m_inner->Forward(seq, out), out); }
			bool Camera(uint64_t seq, CameraPairView& out) const override
			{
				if (!m_inner || !m_inner->Camera(seq, out)) return false;
				out.pictureCx -= m_ox; out.pictureCy -= m_oy;   // picture-centred homographies: the centre in crop pixels
				out.originX += m_ox; out.originY += m_oy;
				return true;
			}

		private:
			bool Shift(bool ok, MotionPair& m) const
			{
				if (ok) { m.originX += m_ox; m.originY += m_oy; }
				return ok;
			}
			const IMotionSource* m_inner;
			double m_ox, m_oy;
		};

		inline Plane Crop(const Plane& p, const ActiveRect& r)
		{
			Plane c(r.Width(), r.Height());
			for (int y = 0; y < c.h; ++y)
				for (int x = 0; x < c.w; ++x) c.at(x, y) = p.at(r.left + x, r.top + y);
			return c;
		}
	}

	inline Plane Model::Process(const FrameIn& in)
	{
		if (m_set.mode == Mode::Off)
		{
			m_diag.status = "off";
			return in.luma;
		}
		if (m_set.mode == Mode::Auto && !in.autoEngaged)
		{
			// auto, idle: the input itself; the store restarts when it engages again (the sequence breaks meanwhile).
			if (m_haveFrame || !m_slots.empty()) Reset("auto idle");   // the store restarts on engagement (as the GPU)
			m_diag.status = "auto: idle";
			return in.luma;
		}
		const ActiveRect r = in.active.Resolve(in.luma.w, in.luma.h);
		const bool whole = r.left == 0 && r.top == 0 && r.right == in.luma.w && r.bottom == in.luma.h;
		// A repeat (same sequence, plane and picture): the last output again, no state advanced.
		if (m_haveFrame && in.seq == m_prevSeq && r == m_rect && m_lastFull.w == in.luma.w && m_lastFull.h == in.luma.h) return m_lastFull;
		m_rectMoved = m_haveFrame && r != m_rect && r.Width() == m_rect.Width() && r.Height() == m_rect.Height();
		m_rect = r;
		if (whole)
		{
			m_lastFull = ProcessActive(in);
			return m_lastFull;
		}
		FrameIn c;
		c.luma = Detail::Crop(in.luma, r);
		c.seq = in.seq; c.contentHz = in.contentHz; c.planeLines = in.planeLines; c.stamps = in.stamps;
		c.cut = in.cut; c.fade = in.fade; c.quality = in.quality; c.blockGates = in.blockGates;
		const Detail::OffsetMotionSource shifted(in.motion, r.left, r.top);
		c.motion = in.motion ? &shifted : nullptr;
		std::vector<Plane> futures;
		futures.reserve(in.future.size());
		for (const FutureFrame& f : in.future)
		{
			if (!f.luma || f.luma->w != in.luma.w || f.luma->h != in.luma.h) break;
			futures.push_back(Detail::Crop(*f.luma, r));
		}
		for (size_t k = 0; k < futures.size(); ++k)
		{
			FutureFrame ff;
			ff.luma = &futures[k];
			ff.seq = in.future[k].seq;
			c.future.push_back(ff);
		}
		const Plane o = ProcessActive(c);
		Plane full = in.luma;   // bars: the input, bit-exact
		for (int y = 0; y < o.h; ++y)
			for (int x = 0; x < o.w; ++x) full.at(r.left + x, r.top + y) = o.at(x, y);
		m_lastFull = full;
		return full;
	}

	inline Plane Model::ProcessActive(const FrameIn& in)
	{
		if (m_set.mode == Mode::Off)
		{
			m_diag.status = "off";
			return in.luma;
		}
		m_blockGates = in.blockGates;
		const int w = in.luma.w, h = in.luma.h;
		// A repeat (same sequence, same size): the last output again, no state advanced.
		if (m_haveFrame && !m_rectMoved && in.seq == m_prevSeq && w == m_w && h == m_h && m_lastOut.w == w && m_lastOut.h == h) return m_lastOut;
		const double scale = Scale(in.planeLines);
		const int B = (std::max)(1, static_cast<int>(std::floor(kBlockPx * scale + 0.5)));
		const double hz = in.contentHz > 0.0 ? in.contentHz : kReferenceHz;

		// --- Resets: dimension/scale change, sequence break, stamps with the Reset policy, cut, fade ---
		const char* reason = nullptr;
		if (m_haveFrame)
		{
			if (w != m_w || h != m_h || B != m_B || m_rectMoved) reason = "geometry";
			else if (in.seq != m_prevSeq + 1) reason = "sequence";
			else if (in.stamps.full != m_stamps.full && PolicyOf(Counter::Full) == ReaderPolicy::Reset) reason = "stamp full";
			else if (in.stamps.continuity != m_stamps.continuity && PolicyOf(Counter::Continuity) == ReaderPolicy::Reset) reason = "stamp continuity";
			else if (in.stamps.rebaseH != m_stamps.rebaseH && PolicyOf(Counter::RebaseH) == ReaderPolicy::Reset) reason = "stamp rebaseH";
			else if (in.stamps.rebaseV != m_stamps.rebaseV && PolicyOf(Counter::RebaseV) == ReaderPolicy::Reset) reason = "stamp rebaseV";
		}
		if (in.cut) reason = "cut";
		else if (in.fade) reason = "fade";
		if (reason) Reset(reason);
		std::string status = reason ? std::string("reset: ") + reason : std::string("ok");
		m_w = w; m_h = h; m_B = B; m_scale = scale;

		const int bw = (w + B - 1) / B, bh = (h + B - 1) / B, nb = bw * bh;
		if (m_blocks.size() != static_cast<size_t>(nb)) m_blocks.assign(static_cast<size_t>(nb), BlockState());
		// Block gates must cover this grid exactly (4 factors per block), else they are dropped visibly (merge audit 2026-11-01).
		std::string gateNote;
		if (!m_blockGates.empty())
		{
			if (m_blockGates.size() != static_cast<size_t>(nb) * 4) { m_blockGates.clear(); gateNote = "; block gates rejected (size/format)"; }
			else gateNote = "; block gates";
		}
		if (!m_haveFrame) { m_camCx = 0.5 * w; m_camCy = 0.5 * h; }

		const int fRise = Frames(kRiseSeconds, hz), fFall = Frames(kFallSeconds, hz), fXf = Frames(kCrossfadeSeconds, hz);
		const int fRun = Frames(kSoftRunSeconds, hz);
		const double reboundDecay = std::pow(0.5, 1.0 / Frames(m_set.maxAgeSeconds * 0.5, hz));
		const double peakDecay = std::pow(0.5, 1.0 / Frames(m_set.maxAgeSeconds, hz));
		const double rise = 1.0 / fRise, fall = 1.0 / fFall;

		const Analysis A = Detail::Analyse(in.luma, scale, B);

		// --- Camera view and the per-block camera labels of this pair ---
		CameraPairView camView;
		bool haveCam = false;
		std::vector<uint8_t> camBlock(static_cast<size_t>(nb), 0);
		MotionPair bwd;
		bool haveBwd = in.motion && in.motion->Backward(in.seq, bwd) && Detail::PairUsable(bwd);
		if (in.motion && m_haveFrame)
		{
			haveCam = in.motion->Camera(in.seq, camView) && camView.usable && camView.confidence >= kCameraMinConfidence;
			if (haveCam)
			{
				m_camCx = camView.pictureCx; m_camCy = camView.pictureCy;
				if (camView.label && camView.gridW > 0 && camView.gridH > 0 && camView.blockSize > 0.0)
					for (int by = 0; by < bh; ++by)
						for (int bx = 0; bx < bw; ++bx)
						{
							const int cbx = (std::min)(camView.gridW - 1, static_cast<int>(((bx + 0.5) * B + camView.originX) / camView.blockSize));
							const int cby = (std::min)(camView.gridH - 1, static_cast<int>(((by + 0.5) * B + camView.originY) / camView.blockSize));
							camBlock[static_cast<size_t>(by) * bw + bx] = camView.label[static_cast<size_t>(cby) * camView.gridW + cbx] == 1 ? 1 : 0;
						}
			}
		}

		// --- Compose every stored slot with this pair; motion lost = the store cannot be carried ---
		if (m_haveFrame && !m_slots.empty())
		{
			if (!haveBwd)
			{
				m_slots.clear();
				for (BlockState& st : m_blocks) { st.slot = -1; st.prevSlot = -1; st.xf = 1.0; st.qref = 0.0; }
				status = "motion lost";
			}
			else
			{
				MotionPair fwd;
				const bool haveFwd = in.motion->Forward(in.seq, fwd) && Detail::PairUsable(fwd);
				for (Source& s : m_slots) Compose(s, bwd, haveFwd ? &fwd : nullptr, haveCam ? &camView : nullptr, A, camBlock);
			}
		}

		// --- Future sources (queued modes): composed forward vectors, warped once ---
		std::vector<Source> futures;
		std::vector<Analysis> futureA;
		if (in.motion && !in.future.empty())
		{
			futureA.reserve(in.future.size());
			uint64_t expect = in.seq + 1;
			std::vector<double> px(static_cast<size_t>(nb)), py(static_cast<size_t>(nb)), pu(static_cast<size_t>(nb), 0.0);
			for (int by = 0; by < bh; ++by)
				for (int bx = 0; bx < bw; ++bx)
				{
					px[static_cast<size_t>(by) * bw + bx] = (bx + 0.5) * B - 0.5;
					py[static_cast<size_t>(by) * bw + bx] = (by + 0.5) * B - 0.5;
				}
			std::vector<double> ox = px, oy = py;
			for (size_t k = 0; k < in.future.size() && static_cast<int>(k) < kMaxFutureFrames; ++k)
			{
				const FutureFrame& ff = in.future[k];
				if (!ff.luma || ff.seq != expect || ff.luma->w != w || ff.luma->h != h) break;
				MotionPair f, b;
				if (!in.motion->Forward(ff.seq, f) || !Detail::PairUsable(f)) break;
				const bool haveB = in.motion->Backward(ff.seq, b) && Detail::PairUsable(b);
				for (size_t i = 0; i < px.size(); ++i)
				{
					const double vx = Detail::PairAt(f, f.vx, px[i], py[i]), vy = Detail::PairAt(f, f.vy, px[i], py[i]);
					double hop = (1.0 - Detail::PairAt(f, f.trust, px[i], py[i])) * f.blockPx;
					if (haveB)
					{
						const double bx2 = Detail::PairAt(b, b.vx, px[i] + vx, py[i] + vy), by2 = Detail::PairAt(b, b.vy, px[i] + vx, py[i] + vy);
						hop += std::sqrt((bx2 + vx) * (bx2 + vx) + (by2 + vy) * (by2 + vy));
					}
					px[i] += vx; py[i] += vy; pu[i] += hop;
				}
				futureA.push_back(Detail::Analyse(*ff.luma, scale, B));
				const Analysis& fa = futureA.back();
				Source s;
				s.id = -2 - static_cast<int>(k);
				s.seq = ff.seq; s.future = true;
				s.s1 = fa.s1; s.s2 = fa.s2; s.s = fa.s; s.grain = fa.grain; s.N = fa.N; s.base = fa.base; s.F = fa.F; s.q = fa.q;
				s.cx.resize(px.size()); s.cy.resize(px.size()); s.u = pu;
				for (size_t i = 0; i < px.size(); ++i) { s.cx[i] = px[i] - ox[i]; s.cy[i] = py[i] - oy[i]; }
				s.camChain = false;
				futures.push_back(std::move(s));
				++expect;
			}
		}

		// --- Per-block pass 1: trackers, validation against the held reference (and the future), refresh request ---
		struct Work
		{
			Eval pe, fe;
			int pastId = -1;
			const Source* fut = nullptr;
			double referee = 1.0, futQ = -1.0;
			bool refresh = false;
			double evidence = 0.0;
		};
		std::vector<Work> work(static_cast<size_t>(nb));
		int refreshes = 0;
		for (int by = 0; by < bh; ++by)
			for (int bx = 0; bx < bw; ++bx)
			{
				const size_t i = static_cast<size_t>(by) * bw + bx;
				BlockState& st = m_blocks[i];
				Work& wk = work[i];
				const double q = A.q[i], e2 = A.e2[i], F = A.F[i];
				// trackers (rebound counter, softening run, decayed peak)
				if (!st.have)
				{
					st.have = true; st.qPrev = q; st.e2Prev = e2; st.qHigh = q; st.qLow = q; st.rstate = 0;
					st.qPeak = q; st.e2Peak = e2; st.fPeak = F;
				}
				else
				{
					st.rebounds *= reboundDecay;
					if (st.rstate == 0)
					{
						if (q > st.qHigh) st.qHigh = q;
						else if (q < st.qHigh * (1.0 - kReboundTau) && st.qHigh - q >= kReboundMinEnergy) { st.rstate = 1; st.qLow = q; }
					}
					else
					{
						if (q < st.qLow) st.qLow = q;
						else if (q > st.qLow * (1.0 + kReboundTau) && q - st.qLow >= kReboundMinEnergy) { st.rebounds += 1.0; st.rstate = 0; st.qHigh = q; }
					}
					const bool falling = q < st.qPrev * (1.0 - kSoftStep) && e2 < st.e2Prev * (1.0 - kSoftStep) && st.qPrev >= kRetireMinEnergy;
					st.softRun = falling ? (std::min)(st.softRun + 1, kRunSaturate) : 0;
					st.qPeak *= peakDecay;
					if (q >= st.qPeak) { st.qPeak = q; st.e2Peak = e2; st.fPeak = F; }
					st.qPrev = q; st.e2Prev = e2;
				}
				// crossfade progress
				if (st.prevSlot >= 0)
				{
					st.xf = (std::min)(1.0, st.xf + 1.0 / fXf);
					if (st.xf >= 1.0) st.prevSlot = -1;
				}
				const double hint = (i < in.quality.size()) ? Clamp(in.quality[i], 0.0, 1.0) : 1.0;
				wk.evidence = (std::max)(Smoothstep(kEvidenceLo, kEvidenceHi, st.rebounds), 1.0 - hint);
				const bool cam = camBlock[i] != 0;
				Source* ps = st.slot >= 0 ? FindSlot(st.slot) : nullptr;
				if (st.slot >= 0 && !ps) { st.slot = -1; st.prevSlot = -1; st.xf = 1.0; st.qref = 0.0; }
				if (ps && m_haveFrame)
				{
					wk.pastId = ps->id;
					wk.pe = EvaluateBlock(bx, by, A, *ps, cam, hz);
				}
				// the best future frame for this block (highest q at the composed centre)
				if (!futures.empty())
				{
					const double cx = (bx + 0.5) * B - 0.5, cy = (by + 0.5) * B - 0.5;
					double bestQ = -1.0;
					for (const Source& fs : futures)
					{
						const double sx = cx + Detail::GridAt(fs.cx, bw, bh, B, cx, cy), sy = cy + Detail::GridAt(fs.cy, bw, bh, B, cx, cy);
						const int sbx = (std::max)(0, (std::min)(bw - 1, static_cast<int>(std::floor((sx + 0.5) / B)))), sby = (std::max)(0, (std::min)(bh - 1, static_cast<int>(std::floor((sy + 0.5) / B))));
						const double fq = fs.q[static_cast<size_t>(sby) * bw + sbx];
						if (fq > bestQ) { bestQ = fq; wk.fut = &fs; }
					}
					wk.futQ = bestQ;
					if (wk.fut)
					{
						wk.fe = EvaluateBlock(bx, by, A, *wk.fut, false, hz);
						if (wk.pastId >= 0)
						{
							const double ratio = bestQ / (wk.pe.e1w + kEnergyFloor);
							wk.referee = Smoothstep(0.8 * kSoftFuture, 1.2 * kSoftFuture, ratio);
						}
					}
				}
				// refresh request: no reference, or q has caught up with the decayed reference level and the block is not soft by the guard
				if (!st.retired)
				{
					if (wk.pastId < 0) wk.refresh = true;
					else
					{
						const double decay = std::pow(0.5, wk.pe.ageSeconds / m_set.maxAgeSeconds);
						if (q >= st.qref * decay && wk.pe.hbGate * wk.pe.focusGate >= 0.5) wk.refresh = true;
					}
				}
				if (wk.refresh) ++refreshes;
			}

		// --- Admission of the current frame as a slot (eviction: no live blocks, else the oldest that has faded) ---
		bool admitted = false;
		int curId = -1;
		if (refreshes > 0)
		{
			auto referenced = [&](int id, bool onlyFaded)
			{
				for (const BlockState& st : m_blocks)
					if ((st.slot == id || st.prevSlot == id) && (!onlyFaded || st.wLim * st.hSm >= kEvictWeight)) return true;
				return false;
			};
			const int slotCap = (std::max)(1, (std::min)(kSlots, m_tune.slots));
			if (static_cast<int>(m_slots.size()) >= slotCap)
			{
				int victim = -1;
				for (size_t k = 0; k < m_slots.size(); ++k)
					if (!referenced(m_slots[k].id, false)) { victim = static_cast<int>(k); break; }
				if (victim < 0)
				{
					uint64_t oldest = ~0ull;
					for (size_t k = 0; k < m_slots.size(); ++k)
						if (!referenced(m_slots[k].id, true) && m_slots[k].seq < oldest) { oldest = m_slots[k].seq; victim = static_cast<int>(k); }
				}
				if (victim >= 0)
				{
					const int vid = m_slots[static_cast<size_t>(victim)].id;
					for (BlockState& st : m_blocks)
					{
						if (st.slot == vid) { st.slot = -1; st.qref = 0.0; }
						if (st.prevSlot == vid) st.prevSlot = -1;
					}
					for (Work& wk : work)
						if (wk.pastId == vid) wk.pastId = -1;
					m_slots.erase(m_slots.begin() + victim);
				}
			}
			if (static_cast<int>(m_slots.size()) < slotCap)
			{
				Source s;
				s.id = curId = m_nextId++;
				s.seq = in.seq;
				s.s1 = A.s1; s.s2 = A.s2; s.s = A.s; s.grain = A.grain; s.N = A.N; s.base = A.base; s.F = A.F; s.q = A.q;
				s.cx.assign(static_cast<size_t>(nb), 0.0); s.cy.assign(static_cast<size_t>(nb), 0.0); s.u.assign(static_cast<size_t>(nb), 0.0);
				m_slots.push_back(std::move(s));
				admitted = true;
				// Step 4a: clean the new slot against the other slots that align with it here (evidence only).
				if (m_tune.templateClean)
				{
					std::vector<const Source*> mem;
					for (const Source& sl : m_slots)
						if (sl.id != curId) mem.push_back(&sl);
					std::vector<uint8_t> valid(mem.size() * static_cast<size_t>(nb), 0);
					for (size_t m = 0; m < mem.size(); ++m)
						for (int by = 0; by < bh; ++by)
							for (int bx = 0; bx < bw; ++bx)
							{
								const size_t bi = static_cast<size_t>(by) * bw + bx;
								const Eval e = EvaluateBlock(bx, by, A, *mem[m], camBlock[bi] != 0, hz);
								valid[m * static_cast<size_t>(nb) + bi] = (e.uncGate * e.baseGate * e.corrGate >= kCleanMemberMin && e.ageGate > 0.0) ? 1 : 0;
							}
					CleanSlot(m_slots.back(), A, mem, valid, camBlock);
				}
			}
		}

		// --- Pass 2: weights (limiter), retire, boost, contributions ---
		Diagnostics d;
		d.status = status + gateNote;
		d.bw = bw; d.bh = bh; d.blockPx = B; d.scale = scale;
		d.resets = m_diag.resets; d.refreshes = m_diag.refreshes; d.retires = m_diag.retires; d.frames = m_diag.frames + 1;
		auto sized = [&](std::vector<double>& v) { v.assign(static_cast<size_t>(nb), 0.0); };
		sized(d.weight); sized(d.boost); sized(d.target); sized(d.ageSeconds); sized(d.uncertainty); sized(d.baseMatch); sized(d.corr);
		sized(d.corrGate); sized(d.stronger); sized(d.highBand); sized(d.focus); sized(d.referee); sized(d.evidence); sized(d.rebounds);
		sized(d.q); sized(d.qref); sized(d.hard);
		sized(d.tau1); sized(d.tau2); sized(d.templateGrain); sized(d.guard); sized(d.guardViolation); sized(d.addBack);
		d.templateSlots.assign(static_cast<size_t>(nb), 0);
		d.guardOn.assign(static_cast<size_t>(nb), 0);
		d.guardTrips = m_diag.guardTrips;
		d.slotId.assign(static_cast<size_t>(nb), -1);
		d.retired.assign(static_cast<size_t>(nb), 0); d.refreshed.assign(static_cast<size_t>(nb), 0);
		d.usedFuture.assign(static_cast<size_t>(nb), 0); d.camera.assign(static_cast<size_t>(nb), 0);

		std::vector<Contribution> contrib(static_cast<size_t>(nb));
		for (int i = 0; i < nb; ++i)
		{
			BlockState& st = m_blocks[static_cast<size_t>(i)];
			Work& wk = work[static_cast<size_t>(i)];
			const bool refreshNow = wk.refresh && admitted;
			double soft = 0.0, hardTarget = 0.0;
			const Eval* chosen = nullptr;
			bool useFuture = false;
			if (refreshNow)
			{
				// The current frame becomes the reference: nothing to transfer this frame, the weight holds.
				if (wk.pastId >= 0) { st.prevSlot = st.slot; st.xf = 0.0; }
				else { st.prevSlot = -1; st.xf = 1.0; }
				st.slot = curId;
				st.qref = A.q[static_cast<size_t>(i)];
				soft = st.tHold;
				hardTarget = st.hSm;
				++d.refreshes;
				d.refreshed[static_cast<size_t>(i)] = 1;
			}
			else
			{
				if (wk.pastId >= 0) chosen = &wk.pe;
				const double pastScore = wk.pastId >= 0 ? wk.pe.hard * wk.referee * wk.pe.ageGate * wk.pe.strongerGate : 0.0;
				const double futScore = wk.fut ? wk.fe.hard * wk.fe.ageGate * wk.fe.strongerGate : 0.0;
				if (wk.fut && futScore > pastScore + 1e-12) { chosen = &wk.fe; useFuture = true; }
				if (chosen)
				{
					const double refereeGate = useFuture ? 1.0 : wk.referee;
					hardTarget = chosen->hard * refereeGate;
					soft = m_set.strength * Bg(static_cast<size_t>(i), 3) * wk.evidence * chosen->ageGate * chosen->strongerGate * (st.retired ? 0.0 : 1.0);
					st.tHold = soft;
					// the guard's veto run (the focus / high-band-loss guard against a past reference)
					if (!useFuture && wk.pastId >= 0 && chosen->hbGate * chosen->focusGate < 0.1) st.vetoRun = (std::min)(st.vetoRun + 1, kRunSaturate); else st.vetoRun = 0;
					d.referee[static_cast<size_t>(i)] = refereeGate;
				}
				else { soft = 0.0; hardTarget = 0.0; st.vetoRun = 0; }
			}
			// retire: monotonic softening, or the guard vetoing the reference for Frames(0.2 s)
			if (!st.retired && wk.pastId >= 0 && (st.softRun >= fRun || st.vetoRun >= fRun))
			{
				st.retired = true;
				++d.retires;
			}
			if (st.retired) soft = 0.0;
			// hysteresis + rate limiter (age-out and retire fade through it; never a step)
			double t = soft;
			if (!st.on && t >= kEnterWeight) st.on = true;
			else if (st.on && t < kLeaveWeight) st.on = false;
			const double tEff = st.on ? t : 0.0;
			st.wLim += Clamp(tEff - st.wLim, -fall, rise);
			if (hardTarget < st.hSm) st.hSm = hardTarget; else st.hSm = (std::min)(hardTarget, st.hSm + rise);
			const double weight = st.wLim * st.hSm;
			// a retired reference leaves once its weight has faded
			if (st.retired && weight < kLeaveWeight * 0.5 && st.wLim <= 0.0)
			{
				st.slot = -1; st.prevSlot = -1; st.xf = 1.0; st.qref = 0.0; st.hSm = 0.0;
			}
			if (st.retired && st.softRun < fRun && st.vetoRun < fRun && st.slot < 0) st.retired = false;
			// fallback boost (structure only): no valid reference, the guard holds against the decayed peak
			double boost = 0.0;
			{
				const size_t u = static_cast<size_t>(i);
				const double q = A.q[u];
				const double raw = std::sqrt((std::max)(st.qPeak, q) / q) - 1.0;
				const double boostMax = m_set.boost ? m_set.boostMax : 0.0;   // the boost switch (off by default)
				double bn = boostMax > 0.0 ? Clamp(raw / boostMax, 0.0, 1.0) : 0.0;
				const double hbP = Smoothstep(kRho2Lo, kRho2Hi, (A.e2[u] + kEnergyFloor) / (st.e2Peak + kEnergyFloor));
				const double fP = Smoothstep(kPullLo, kPullHi, A.F[u] / (st.fPeak + 1e-6));
				const double noRef = 1.0 - Smoothstep(0.0, 0.5, weight);
				const double refB = wk.futQ >= 0.0 ? Smoothstep(0.8 * kSoftFuture, 1.2 * kSoftFuture, wk.futQ / (st.qPeak + kEnergyFloor)) : 1.0;
				bn *= hbP * fP * wk.evidence * noRef * refB * m_set.strength * Bg(u, 3);
				bn = (st.retired ? 0.0 : bn);
				if (!st.bOn && bn >= kEnterWeight) st.bOn = true;
				else if (st.bOn && bn < kLeaveWeight) st.bOn = false;
				st.bLim += Clamp((st.bOn ? bn : 0.0) - st.bLim, -fall, rise);
				boost = boostMax * st.bLim;
			}
			Contribution& c = contrib[static_cast<size_t>(i)];
			// the output-vs-input grain guard (step 10) scales what this block may contribute
			const double guard = m_tune.guard ? st.guard : 1.0;
			c.w = refreshNow ? 0.0 : ((m_tune.forceWeight > 0.0 && chosen) ? m_tune.forceWeight : weight) * guard;
			c.boost = boost * guard;
			if (!refreshNow && chosen)
			{
				if (useFuture) { c.a = wk.fut; c.prev = nullptr; c.xf = 1.0; }
				else
				{
					c.a = FindSlot(st.slot);
					c.prev = st.prevSlot >= 0 ? FindSlot(st.prevSlot) : nullptr;
					c.xf = c.prev ? st.xf : 1.0;
				}
				c.cam = camBlock[static_cast<size_t>(i)] != 0 && !useFuture;
				if (!c.a) c.w = 0.0;
			}
			// The reference template (step 8): every valid stored slot's grain-free structure (or the future source alone),
			// q-weighted (best linear estimate of a common structure under independent grain), scaled to the selected
			// reference's amplitude. Then the current block's per-band projection onto it: lambda_b = <S_b, T_b> / <T_b, T_b>.
			if (c.w > 0.0 && c.a)
			{
				const int bx = i % bw, by = i / bw;
				double aw[kSlots + 1], om[kSlots + 1];
				const Eval& selE = useFuture ? wk.fe : wk.pe;
				const double aSel = std::sqrt(selE.e1w + kEnergyFloor);
				c.nm = 0;
				auto add = [&](const Source* src, double a, double omega)
				{
					if (c.nm >= kSlots + 1 || !(omega > 0.0)) return;
					c.mem[c.nm].s = src; aw[c.nm] = a; om[c.nm] = omega; ++c.nm;
				};
				if (useFuture) add(c.a, aSel, 1.0);
				else
				{
					for (const Source& sl : m_slots)
					{
						if (sl.id == c.a->id) { add(&sl, aSel, c.prev ? c.xf : 1.0); continue; }
						if (sl.id == curId) continue;   // the current frame (admitted this frame) never serves itself
						const Eval e = EvaluateBlock(bx, by, A, sl, c.cam, hz);
						const bool valid = e.hard >= kTemplateMinHard && e.ageGate > 0.0;
						const double omega = (c.prev && sl.id == c.prev->id) ? (std::max)(1.0 - c.xf, valid ? 1.0 : 0.0) : (valid ? 1.0 : 0.0);
						add(&sl, std::sqrt(e.e1w + kEnergyFloor), omega);
					}
				}
				double den = 0.0;
				for (int k = 0; k < c.nm; ++k) den += om[k] * aw[k] * aw[k];
				c.nT = 0.0;
				for (int k = 0; k < c.nm; ++k)
				{
					c.mem[k].coef = den > 0.0 ? aSel * om[k] * aw[k] / den : 0.0;
					c.mem[k].rho2 = Rho2At(*c.mem[k].s, c.cam && !c.mem[k].s->future, bx, by);
					c.nT += c.mem[k].coef * c.mem[k].coef * c.mem[k].rho2;
				}
				double st1 = 0, tt1 = 0, ss1 = 0, st2 = 0, tt2 = 0, ss2 = 0;
				for (int y = by * B; y < (std::min)(h, (by + 1) * B); ++y)
					for (int x = bx * B; x < (std::min)(w, (bx + 1) * B); ++x)
					{
						double t1, t2;
						TemplateAt(c, x, y, A.base.at(x, y), t1, t2);
						const double s1 = A.s1.at(x, y), s2 = A.s2.at(x, y);
						st1 += s1 * t1; tt1 += t1 * t1; ss1 += s1 * s1;
						st2 += s2 * t2; tt2 += t2 * t2; ss2 += s2 * s2;
					}
				auto share = [](double sxt, double txt, double sxs, double& lambda)
				{
					lambda = txt > 1e-9 ? Clamp(sxt / txt, 0.0, 1.0) : 1.0;
					if (!(txt > 1e-9)) return 0.0;
					const double rho = sxt / std::sqrt(txt * sxs + 1e-12);
					return (1.0 - lambda) * Smoothstep(kCoherenceLo, kCoherenceHi, rho);
				};
				c.tau1 = share(st1, tt1, ss1, c.lam1);
				c.tau2 = share(st2, tt2, ss2, c.lam2);
				if (c.tau1 <= 0.0 && c.tau2 <= 0.0) c.w = 0.0;
			}
			const size_t u = static_cast<size_t>(i);
			d.weight[u] = weight; d.boost[u] = boost; d.target[u] = soft; d.hard[u] = hardTarget;
			d.rebounds[u] = st.rebounds; d.evidence[u] = wk.evidence; d.q[u] = A.q[u]; d.qref[u] = st.qref;
			d.slotId[u] = st.slot; d.retired[u] = st.retired ? 1 : 0; d.usedFuture[u] = useFuture ? 1 : 0; d.camera[u] = camBlock[u];
			d.tau1[u] = c.tau1; d.tau2[u] = c.tau2; d.templateGrain[u] = c.nT; d.templateSlots[u] = c.nm; d.guard[u] = guard;
			if (chosen)
			{
				d.ageSeconds[u] = chosen->ageSeconds; d.uncertainty[u] = chosen->u; d.baseMatch[u] = chosen->baseGate; d.corr[u] = chosen->corr;
				d.corrGate[u] = chosen->corrGate; d.stronger[u] = chosen->strongerGate; d.highBand[u] = chosen->hbGate; d.focus[u] = chosen->focusGate;
			}
		}

		// --- Transfer on the grain-free picture, then the current grain added back exactly (steps 8-9) ---
		// TD(clean) = clean + delta, Y_out = TD(clean) + G = Y + delta. delta = sum over the four nearest blocks of
		// om w (tau1 T1 + tau2 T2): only the template (other frames' grain-free structure) moves; the current frame enters
		// through its block projections lambda_b only, so its own grain is never averaged, scaled or replaced.
		Plane out = in.luma;
		d.structure = A.s;
		d.structureOut = A.s;
		d.base = A.base;
		d.grain = A.grain;
		d.grainOut = A.grain;
		const size_t np = static_cast<size_t>(w) * static_cast<size_t>(h);
		// gdel: what the output-vs-input grain guard reads: delta, less the template's structure transfer at pixels where the
		// (cleaned) template carries structure (T^2 > guardTemplateShare N). The map's G holds some structure at low-f pixels
		// and restoring it there is not grain damage (it tripped the guard on legitimate transfer); everything else (boost,
		// add-back, deliberate damage, and all of delta elsewhere) stays visible to the guard.
		std::vector<double> delta(np, 0.0), gdel(np, 0.0);
		for (int y = 0; y < h; ++y)
			for (int x = 0; x < w; ++x)
			{
				const double gx = (x + 0.5) / B - 0.5, gy = (y + 0.5) / B - 0.5;
				const double fx = std::floor(gx), fy = std::floor(gy);
				const double tx = gx - fx, ty = gy - fy;
				const int i0 = static_cast<int>(fx), j0 = static_cast<int>(fy);
				const size_t pi = static_cast<size_t>(y) * w + x;
				double raw = 0.0, pred = 0.0, amp = 0.0, refMax = 0.0, bsum = 0.0, wpix = 0.0, frozen = 0.0, tv = 0.0;
				const double S = A.s.at(x, y), bs = A.base.at(x, y);
				for (int k = 0; k < 4; ++k)
				{
					const int ii = (std::max)(0, (std::min)(bw - 1, i0 + (k & 1))), jj = (std::max)(0, (std::min)(bh - 1, j0 + (k >> 1)));
					const double om = ((k & 1) ? tx : 1.0 - tx) * ((k >> 1) ? ty : 1.0 - ty);
					const Contribution& c = contrib[static_cast<size_t>(jj) * bw + ii];
					if (om <= 0.0 || (c.w <= 0.0 && c.boost <= 0.0)) continue;
					if (c.w > 0.0 && c.nm > 0)
					{
						double t1, t2;
						TemplateAt(c, x, y, bs, t1, t2);
						raw += om * c.w * (c.tau1 * t1 + c.tau2 * t2);
						pred += om * c.w * (c.lam1 * t1 + c.lam2 * t2);
						amp += om * c.w * (std::max)(c.tau1, c.tau2) * std::sqrt((std::max)(0.0, c.nT));
						refMax = (std::max)(refMax, std::fabs(t1 + t2));
						tv += om * c.w * (t1 + t2);
						wpix += om * c.w;
						if (m_tune.forceFreeze > 0.0 && c.a)
						{
							double sx, sy;
							Position(*c.a, c.cam && !c.a->future, x, y, sx, sy);
							frozen += om * c.w * Detail::SampleTaps(c.a->grain, Detail::MakeTaps(sx, sy));
						}
					}
					bsum += om * c.boost;
				}
				if (wpix <= 0.0 && bsum <= 0.0) continue;
				// Template grain budget: the template's residual grain (sum coef^2 of one member frame's grain, the same grain
				// level as the current frame within a shot) carried into the output stays <= grainBudget x the current grain
				// RMS. Block quantities only (no noisy per-frame level estimate), bilinear. grainBudget <= 0: designed strength.
				double beta = 1.0;
				if (m_tune.grainBudget > 0.0 && amp > 0.0) beta = (std::min)(1.0, m_tune.grainBudget / amp);
				// Fallback boost on the current structure: it scales that pixel's grain share f by at most kBoostGrainShare.
				// Amplitude limit on the grain-free prediction: the current structure's projection P (lambda T) plus the
				// transfer stays within kPixelCap x max(|P|, |T|). The current pixel's own value (its grain) is never clamped.
				double dv = beta * raw;
				if (wpix > 0.0 && dv != 0.0)
				{
					const double P = pred / wpix, t = dv / wpix;
					const double cap = kPixelCap * (std::max)(std::fabs(P), refMax);
					dv = (Clamp(P + t, -cap, cap) - P) * wpix;
				}
				const double dT = dv;   // the template's structure transfer
				// Fallback boost on the current structure: it scales that pixel's grain share f by at most kBoostGrainShare.
				const double f = A.f.at(x, y);
				const double bEff = (std::min)(bsum, kBoostGrainShare / (std::max)(f, 1e-6));
				if (bEff > 0.0)
				{
					const double cap = (1.0 + bsum) * std::fabs(S);
					dv += Clamp(S * (1.0 + bEff), -cap, cap) - S;
				}
				// deliberate damage (Tuning, the guard's own test only), after the cap so nothing hides it
				dv += -m_tune.forceDrop * wpix * A.grain.at(x, y) + m_tune.forceFreeze * frozen;
				delta[pi] = dv;
				const double tval = wpix > 0.0 ? tv / wpix : 0.0;
				const bool structural = m_tune.guardTemplateShare > 0.0 && tval * tval > m_tune.guardTemplateShare * A.N[static_cast<size_t>(y / B) * bw + (x / B)];
				gdel[pi] = structural ? dv - dT : dv;
			}

		// --- Step 10: output-vs-input grain guard on the shared grain map (tile statistics, pooled over 3 x 3 blocks) ---
		// G_in = Y - clean (the split); G_out = the same map applied to the output = G_in + (1 - f) delta. Per block:
		// energies, the cross term <(1 - f) delta, G_in> (current grain removed when negative) and the lag-1 sums against
		// the previous frame's G_in / G_out warped one hop along the backward vectors (measurement only, never output).
		{
			const bool lagOk = m_havePrevG && haveBwd && !reason && m_prevGin.w == w && m_prevGin.h == h;
			std::vector<double> sums(static_cast<size_t>(nb) * 7, 0.0);
			for (int y = 0; y < h; ++y)
				for (int x = 0; x < w; ++x)
				{
					const size_t pi = static_cast<size_t>(y) * w + x;
					// grain-dominated pixels of the map only (f < kGuardMaxShare): structure the transfer adds legitimately lives where f is high
					if (A.f.v[pi] >= kGuardMaxShare) continue;
					const double gin = A.grain.v[pi], dd = (1.0 - A.f.v[pi]) * gdel[pi], gout = gin + dd;
					double* sb = &sums[(static_cast<size_t>(y / B) * bw + (x / B)) * 7];
					sb[0] += gin * gin; sb[1] += gout * gout; sb[2] += dd * gin;
					if (lagOk)
					{
						const double vx = Detail::PairAt(bwd, bwd.vx, x, y), vy = Detail::PairAt(bwd, bwd.vy, x, y);
						const Detail::Taps t = Detail::MakeTaps(x + vx, y + vy);
						const double pin = Detail::SampleTaps(m_prevGin, t), pout = Detail::SampleTaps(m_prevGout, t);
						sb[3] += gin * pin; sb[4] += gout * pout; sb[5] += pin * pin; sb[6] += pout * pout;
					}
				}
			auto pooled = [&](const std::vector<double>& v, int bx, int by, double* o)
			{
				for (int q = 0; q < 7; ++q) o[q] = 0.0;
				for (int j = -1; j <= 1; ++j)
					for (int ii = -1; ii <= 1; ++ii)
					{
						const int xx = (std::max)(0, (std::min)(bw - 1, bx + ii)), yy = (std::max)(0, (std::min)(bh - 1, by + j));
						const double* sb = &v[(static_cast<size_t>(yy) * bw + xx) * 7];
						for (int q = 0; q < 7; ++q) o[q] += sb[q];
					}
			};
			// (a) immediate: add back the measured missing current-frame grain (this frame's pooled projection)
			std::vector<double> kBack(static_cast<size_t>(nb), 0.0);
			bool anyBack = false;
			for (int by = 0; by < bh; ++by)
				for (int bx = 0; bx < bw; ++bx)
				{
					double o[7];
					pooled(sums, bx, by, o);
					const double k = o[0] > 1e-9 ? -o[2] / o[0] : 0.0;
					const size_t u = static_cast<size_t>(by) * bw + bx;
					if (m_tune.guard && k > kGuardAddBackMin) { kBack[u] = (std::min)(1.0, k); anyBack = true; ++d.addBackBlocks; }
					d.addBack[u] = kBack[u];
				}
			if (anyBack)
				for (int y = 0; y < h; ++y)
					for (int x = 0; x < w; ++x)
					{
						const double k = Detail::GridAt(kBack, bw, bh, B, x, y);
						if (k <= 0.0) continue;
						const size_t pi = static_cast<size_t>(y) * w + x;
						delta[pi] += k * A.grain.v[pi];
						gdel[pi] += k * A.grain.v[pi];
						d.grainOut.v[pi] = A.grain.v[pi] + k * A.grain.v[pi];
					}
			// (b) the weight: EMA of the sums, pooled; trip / release with hysteresis; fast fall, slow rise
			const double alpha = 1.0 - 1.0 / Frames(kGuardStatsSeconds, hz);
			const double gFall = 1.0 / Frames(kGuardFallSeconds, hz), gRise = 1.0 / Frames(kGuardRiseSeconds, hz);
			for (int u = 0; u < nb; ++u)
			{
				BlockState& st = m_blocks[static_cast<size_t>(u)];
				const double* sb = &sums[static_cast<size_t>(u) * 7];
				for (int q = 0; q < 7; ++q) st.ema[q] = alpha * st.ema[q] + sb[q];
			}
			std::vector<double> emaAll(static_cast<size_t>(nb) * 7);
			for (int u = 0; u < nb; ++u)
				for (int q = 0; q < 7; ++q) emaAll[static_cast<size_t>(u) * 7 + q] = m_blocks[static_cast<size_t>(u)].ema[q];
			for (int by = 0; by < bh; ++by)
				for (int bx = 0; bx < bw; ++bx)
				{
					const size_t u = static_cast<size_t>(by) * bw + bx;
					BlockState& st = m_blocks[u];
					double o[7];
					pooled(emaAll, bx, by, o);
					double v = 0.0;
					if (o[0] > 1e-9)
					{
						const double L = std::sqrt(o[1] / o[0]);
						const double drop = (std::max)(1.0 - L, -o[2] / o[0]);
						v = (std::max)(drop / kGuardDropTrip, (L - 1.0) / kGuardRiseTrip);
						if (o[5] > 1e-9 && o[6] > 1e-9 && o[1] > 1e-9)
						{
							const double freeze = o[4] / std::sqrt(o[1] * o[6]) - o[3] / std::sqrt(o[0] * o[5]);
							v = (std::max)(v, freeze / kGuardFreezeTrip);
						}
					}
					d.guardViolation[u] = v;
					if (!m_tune.guard) continue;
					if (!st.guardOn && v > 1.0) { st.guardOn = true; ++d.guardTrips; }
					else if (st.guardOn && v < kGuardRelease) st.guardOn = false;
					const double target = st.guardOn ? (v > 1.0 ? st.guard / v : st.guard) : 1.0;
					st.guard += Clamp(target - st.guard, -gFall, gRise);
					d.guardOn[u] = st.guardOn ? 1 : 0;
					if (st.guardOn) ++d.guardActive;
				}
			if (d.guardActive > 0 || d.addBackBlocks > 0)
				d.status += "; grain guard: " + std::to_string(d.guardActive) + " blocks, add-back " + std::to_string(d.addBackBlocks);
		}

		// --- Recompose: a pixel with nothing to add is copied unchanged (bit-exact) ---
		m_prevGin = A.grain;
		m_prevGout = A.grain;
		for (size_t pi = 0; pi < np; ++pi)
		{
			if (delta[pi] == 0.0) continue;
			out.v[pi] = in.luma.v[pi] + delta[pi];
			d.structureOut.v[pi] = A.s.v[pi] + delta[pi] - (d.grainOut.v[pi] - A.grain.v[pi]);
		}
		// the guard's previous output grain, on the same definition as its statistics (gdel)
		for (size_t pi = 0; pi < np; ++pi)
			if (gdel[pi] != 0.0) m_prevGout.v[pi] = A.grain.v[pi] + (1.0 - A.f.v[pi]) * gdel[pi];
		m_havePrevG = true;

		// --- Finish ---
		d.liveSlots = static_cast<int>(m_slots.size());
		m_diag = std::move(d);
		m_prevSeq = in.seq;
		m_stamps = in.stamps;
		m_haveFrame = true;
		m_lastOut = out;
		return out;
	}
}
