// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// TemporalDetail CPU reference model (README.md steps 1-9). One luma plane in codes (doubles),
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

#include <VP-V/modules/repair/temporal_detail/TemporalDetail.h>
#include <VP-V/modules/repair/temporal_detail/TemporalDetailMotion.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace TemporalDetail
{
	struct Plane
	{
		int w = 0, h = 0;
		std::vector<double> v;
		Plane() = default;
		Plane(int width, int height, double fill = 0.0) : w(width), h(height), v(static_cast<size_t>(width) * static_cast<size_t>(height), fill) {}
		double& at(int x, int y) { return v[static_cast<size_t>(y) * w + x]; }
		double at(int x, int y) const { return v[static_cast<size_t>(y) * w + x]; }
		double get(int x, int y) const { return at((std::max)(0, (std::min)(w - 1, x)), (std::max)(0, (std::min)(h - 1, y))); }
	};

	struct Diagnostics
	{
		std::string status = "off";    // why the module is (not) acting: off, ok, reset: <reason>, ...
		int bw = 0, bh = 0, blockPx = 0;
		double scale = 1.0;
		int resets = 0, refreshes = 0, retires = 0, liveSlots = 0, frames = 0;
		// per block, raster
		std::vector<double> weight, boost, target, ageSeconds, uncertainty, baseMatch, corr, corrGate, stronger,
			highBand, focus, referee, evidence, rebounds, q, qref, hard;
		std::vector<int> slotId;                  // -1 none
		std::vector<uint8_t> retired, refreshed, usedFuture, camera;
		Plane grain, structure, structureOut, base;   // input grain G, input structure S, output structure, base band

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
				structureOut.v == o.structureOut.v && base.v == o.base.v;
		}
	};

	namespace Detail
	{
		inline Plane Gauss(const Plane& in, double sigma)
		{
			if (!(sigma > 0.0)) return in;
			const int r = (std::max)(1, static_cast<int>(std::ceil(3.0 * sigma)));
			std::vector<double> k(static_cast<size_t>(2 * r + 1));
			double sum = 0.0;
			for (int i = -r; i <= r; ++i) sum += k[static_cast<size_t>(i + r)] = std::exp(-0.5 * i * i / (sigma * sigma));
			for (double& x : k) x /= sum;
			// Horizontal pass on a row padded by edge replication, then a vertical pass accumulating whole rows
			// (the same sums, in the same order, as a clamped per-tap read).
			Plane t(in.w, in.h), o(in.w, in.h);
			std::vector<double> pad(static_cast<size_t>(in.w + 2 * r));
			for (int y = 0; y < in.h; ++y)
			{
				for (int x = -r; x < in.w + r; ++x) pad[static_cast<size_t>(x + r)] = in.get(x, y);
				for (int x = 0; x < in.w; ++x)
				{
					double a = 0.0;
					for (int i = -r; i <= r; ++i) a += k[static_cast<size_t>(i + r)] * pad[static_cast<size_t>(x + r + i)];
					t.at(x, y) = a;
				}
			}
			for (int y = 0; y < in.h; ++y)
			{
				double* dst = &o.v[static_cast<size_t>(y) * in.w];
				for (int i = -r; i <= r; ++i)
				{
					const double* src = &t.v[static_cast<size_t>((std::max)(0, (std::min)(in.h - 1, y + i))) * in.w];
					const double kk = k[static_cast<size_t>(i + r)];
					for (int x = 0; x < in.w; ++x) dst[x] += kk * src[x];
				}
			}
			return o;
		}

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

		// One frame's analysis (README steps 1-2).
		struct Analysis
		{
			int w = 0, h = 0, bw = 0, bh = 0, B = 16;
			Plane y, base, s1, s2, s, st, d;   // st = f_t D: the strict structure that is transferred / boosted
			std::vector<double> e1, e2, e3, F, q, N;
		};

		inline Analysis Analyse(const Plane& y, double scale, int B)
		{
			Analysis a;
			a.w = y.w; a.h = y.h; a.B = B;
			a.bw = (y.w + B - 1) / B; a.bh = (y.h + B - 1) / B;
			a.y = y;
			const Plane g1 = Gauss(y, kSigmaFine * scale), gb = Gauss(y, kSigmaBase * scale), g6 = Gauss(y, kSigmaLow * scale);
			a.base = gb;
			a.d = Plane(y.w, y.h);
			Plane d1(y.w, y.h), d2(y.w, y.h), d3(y.w, y.h);
			for (size_t i = 0; i < y.v.size(); ++i)
			{
				d1.v[i] = y.v[i] - g1.v[i];
				d2.v[i] = g1.v[i] - gb.v[i];
				d3.v[i] = gb.v[i] - g6.v[i];
				a.d.v[i] = d1.v[i] + d2.v[i];
			}
			const size_t nb = static_cast<size_t>(a.bw) * a.bh;
			// Grain level N per block: the quietest sub-blocks of D, minimum over the neighbourhood.
			std::vector<double> quiet(nb, 0.0);
			for (int by = 0; by < a.bh; ++by)
				for (int bx = 0; bx < a.bw; ++bx)
				{
					std::vector<double> e;
					for (int sj = 0; sj < kSubBlocks; ++sj)
						for (int si = 0; si < kSubBlocks; ++si)
						{
							const int x0 = bx * B + si * B / kSubBlocks, x1 = (std::min)(y.w, bx * B + (si + 1) * B / kSubBlocks);
							const int y0 = by * B + sj * B / kSubBlocks, y1 = (std::min)(y.h, by * B + (sj + 1) * B / kSubBlocks);
							if (x1 <= x0 || y1 <= y0) continue;
							double sum = 0.0;
							for (int yy = y0; yy < y1; ++yy)
								for (int xx = x0; xx < x1; ++xx) sum += a.d.at(xx, yy) * a.d.at(xx, yy);
							e.push_back(sum / ((x1 - x0) * (y1 - y0)));
						}
					std::sort(e.begin(), e.end());
					const size_t n = (std::min)(e.size(), static_cast<size_t>(kQuietSubBlocks));
					double m = 0.0;
					for (size_t i = 0; i < n; ++i) m += e[i];
					quiet[static_cast<size_t>(by) * a.bw + bx] = n ? kGrainCal * m / static_cast<double>(n) : 0.0;
				}
			a.N.assign(nb, 0.0);
			for (int by = 0; by < a.bh; ++by)
				for (int bx = 0; bx < a.bw; ++bx)
				{
					double m = 1e300;
					for (int j = -kGrainNeighbourhood; j <= kGrainNeighbourhood; ++j)
						for (int i = -kGrainNeighbourhood; i <= kGrainNeighbourhood; ++i)
							m = (std::min)(m, quiet[static_cast<size_t>((std::max)(0, (std::min)(a.bh - 1, by + j))) * a.bw + (std::max)(0, (std::min)(a.bw - 1, bx + i))]);
					a.N[static_cast<size_t>(by) * a.bw + bx] = m;
				}
			// Wiener share per pixel and the structure bands.
			a.s1 = Plane(y.w, y.h);
			a.s2 = Plane(y.w, y.h);
			a.s = Plane(y.w, y.h);
			a.st = Plane(y.w, y.h);
			for (int yy = 0; yy < y.h; ++yy)
				for (int xx = 0; xx < y.w; ++xx)
				{
					double e = 0.0;
					for (int j = -1; j <= 1; ++j)
						for (int i = -1; i <= 1; ++i) { const double dv = a.d.get(xx + i, yy + j); e += dv * dv; }
					e /= 9.0;
					const double N = a.N[static_cast<size_t>(yy / B) * a.bw + (xx / B)];
					const double f = Clamp(1.0 - kWienerKappa * N / (std::max)(e, kEnergyFloor), 0.0, 1.0);
					a.s1.at(xx, yy) = f * d1.at(xx, yy);
					a.s2.at(xx, yy) = f * d2.at(xx, yy);
					a.s.at(xx, yy) = a.s1.at(xx, yy) + a.s2.at(xx, yy);
					const double ft = Clamp(1.0 - kKappaTransfer * N / (std::max)(e, kEnergyFloor), 0.0, 1.0);
					a.st.at(xx, yy) = ft * a.d.at(xx, yy);
				}
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
							s3 += d3.at(xx, yy) * d3.at(xx, yy);
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
			Plane s1, s2, s, st, base;
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
			const double gx = (x + 0.5) / m.blockPx - 0.5, gy = (y + 0.5) / m.blockPx - 0.5;
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
		};

		struct Eval
		{
			bool valid = false;
			double ageSeconds = 0, ageGate = 0, u = 0, uncGate = 0, baseDiff = 0, baseGate = 0, corr = 0, corrGate = 0,
				strongerGate = 0, harmGate = 0, hbGate = 0, focusGate = 0, e1w = 0, e1c = 0;
			double hard = 0;   // unc * base * corr * harm * hb * focus (instant fall); strongerGate is soft
		};

		struct Contribution
		{
			const Source* a = nullptr;
			const Source* prev = nullptr;
			double xf = 1.0, w = 0.0, boost = 0.0;
			bool cam = false;
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
			std::vector<double> quality;       // optional per-block bitrate quality 0..1 (empty = neutral 1)
		};

		explicit Model(const Settings& settings) : m_set(settings) {}

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
			++m_diag.resets;
			m_diag.status = std::string("reset: ") + (reason ? reason : "");
		}

		const Diagnostics& Diag() const { return m_diag; }

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

		Settings m_set;
		Diagnostics m_diag;
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
		e.strongerGate = Smoothstep(1.0 + kStrongerMargin, 1.0 + 3.0 * kStrongerMargin, (e.e1w + fl) / (e.e1c + fl)) *
			Smoothstep(kSignificanceLo, kSignificanceHi, e.e1w / (A.N[bi] + fl));
		// A reference clearly weaker than the current block would soften it: that veto is instant.
		e.harmGate = Smoothstep(1.0 - 3.0 * kStrongerMargin, 1.0 - kStrongerMargin, (e.e1w + fl) / (e.e1c + fl));
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
		const double uMax = kUncertaintyPx * m_scale;
		e.uncGate = 1.0 - Smoothstep(0.3 * uMax, uMax, e.u);
		e.baseDiff = bd / n;
		const double allow = kBaseNoiseCodes + kBaseGrainSigmas * std::sqrt((std::max)(0.0, A.N[bi])) / 4.0;
		e.baseGate = 1.0 - Smoothstep(allow, 2.0 * allow, e.baseDiff);
		const double hzv = hz > 0.0 ? hz : kReferenceHz;
		const double ageFrames = src.future ? static_cast<double>(src.seq) - static_cast<double>(m_prevSeq + 1) : static_cast<double>(m_prevSeq + 1) - static_cast<double>(src.seq);
		e.ageSeconds = ageFrames / hzv;
		e.ageGate = Clamp((m_set.maxAgeSeconds - e.ageSeconds) / (m_set.maxAgeSeconds * kAgeFadeShare), 0.0, 1.0);
		e.hard = e.uncGate * e.baseGate * e.corrGate * e.harmGate * e.hbGate * e.focusGate;
		e.valid = true;
		return e;
	}

	inline Plane Model::Process(const FrameIn& in)
	{
		if (m_set.mode == Mode::Off)
		{
			m_diag.status = "off";
			return in.luma;
		}
		const int w = in.luma.w, h = in.luma.h;
		// A repeat (same sequence, same size): the last output again, no state advanced.
		if (m_haveFrame && in.seq == m_prevSeq && w == m_w && h == m_h && m_lastOut.w == w && m_lastOut.h == h) return m_lastOut;
		const double scale = Scale(in.planeLines);
		const int B = (std::max)(1, static_cast<int>(std::floor(kBlockPx * scale + 0.5)));
		const double hz = in.contentHz > 0.0 ? in.contentHz : kReferenceHz;

		// --- Resets: dimension/scale change, sequence break, stamps with the Reset policy, cut, fade ---
		const char* reason = nullptr;
		if (m_haveFrame)
		{
			if (w != m_w || h != m_h || B != m_B) reason = "geometry";
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
							const int cbx = (std::min)(camView.gridW - 1, static_cast<int>(((bx + 0.5) * B) / camView.blockSize));
							const int cby = (std::min)(camView.gridH - 1, static_cast<int>(((by + 0.5) * B) / camView.blockSize));
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
				s.s1 = fa.s1; s.s2 = fa.s2; s.s = fa.s; s.st = fa.st; s.N = fa.N; s.base = fa.base; s.F = fa.F; s.q = fa.q;
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
					st.softRun = falling ? st.softRun + 1 : 0;
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
			if (static_cast<int>(m_slots.size()) >= kSlots)
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
			if (static_cast<int>(m_slots.size()) < kSlots)
			{
				Source s;
				s.id = curId = m_nextId++;
				s.seq = in.seq;
				s.s1 = A.s1; s.s2 = A.s2; s.s = A.s; s.st = A.st; s.N = A.N; s.base = A.base; s.F = A.F; s.q = A.q;
				s.cx.assign(static_cast<size_t>(nb), 0.0); s.cy.assign(static_cast<size_t>(nb), 0.0); s.u.assign(static_cast<size_t>(nb), 0.0);
				m_slots.push_back(std::move(s));
				admitted = true;
			}
		}

		// --- Pass 2: weights (limiter), retire, boost, contributions ---
		Diagnostics d;
		d.status = status;
		d.bw = bw; d.bh = bh; d.blockPx = B; d.scale = scale;
		d.resets = m_diag.resets; d.refreshes = m_diag.refreshes; d.retires = m_diag.retires; d.frames = m_diag.frames + 1;
		auto sized = [&](std::vector<double>& v) { v.assign(static_cast<size_t>(nb), 0.0); };
		sized(d.weight); sized(d.boost); sized(d.target); sized(d.ageSeconds); sized(d.uncertainty); sized(d.baseMatch); sized(d.corr);
		sized(d.corrGate); sized(d.stronger); sized(d.highBand); sized(d.focus); sized(d.referee); sized(d.evidence); sized(d.rebounds);
		sized(d.q); sized(d.qref); sized(d.hard);
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
					soft = m_set.strength * wk.evidence * chosen->ageGate * chosen->strongerGate * (st.retired ? 0.0 : 1.0);
					st.tHold = soft;
					// the guard's veto run (the focus / high-band-loss guard against a past reference)
					if (!useFuture && wk.pastId >= 0 && chosen->hbGate * chosen->focusGate < 0.1) ++st.vetoRun; else st.vetoRun = 0;
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
				double bn = m_set.boostMax > 0.0 ? Clamp(raw / m_set.boostMax, 0.0, 1.0) : 0.0;
				const double hbP = Smoothstep(kRho2Lo, kRho2Hi, (A.e2[u] + kEnergyFloor) / (st.e2Peak + kEnergyFloor));
				const double fP = Smoothstep(kPullLo, kPullHi, A.F[u] / (st.fPeak + 1e-6));
				const double noRef = 1.0 - Smoothstep(0.0, 0.5, weight);
				const double refB = wk.futQ >= 0.0 ? Smoothstep(0.8 * kSoftFuture, 1.2 * kSoftFuture, wk.futQ / (st.qPeak + kEnergyFloor)) : 1.0;
				bn *= hbP * fP * wk.evidence * noRef * refB * m_set.strength;
				bn = (st.retired ? 0.0 : bn);
				if (!st.bOn && bn >= kEnterWeight) st.bOn = true;
				else if (st.bOn && bn < kLeaveWeight) st.bOn = false;
				st.bLim += Clamp((st.bOn ? bn : 0.0) - st.bLim, -fall, rise);
				boost = m_set.boostMax * st.bLim;
			}
			Contribution& c = contrib[static_cast<size_t>(i)];
			c.w = refreshNow ? 0.0 : weight;
			c.boost = boost;
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
			const size_t u = static_cast<size_t>(i);
			d.weight[u] = weight; d.boost[u] = boost; d.target[u] = soft; d.hard[u] = hardTarget;
			d.rebounds[u] = st.rebounds; d.evidence[u] = wk.evidence; d.q[u] = A.q[u]; d.qref[u] = st.qref;
			d.slotId[u] = st.slot; d.retired[u] = st.retired ? 1 : 0; d.usedFuture[u] = useFuture ? 1 : 0; d.camera[u] = camBlock[u];
			if (chosen)
			{
				d.ageSeconds[u] = chosen->ageSeconds; d.uncertainty[u] = chosen->u; d.baseMatch[u] = chosen->baseGate; d.corr[u] = chosen->corr;
				d.corrGate[u] = chosen->corrGate; d.stronger[u] = chosen->strongerGate; d.highBand[u] = chosen->hbGate; d.focus[u] = chosen->focusGate;
			}
		}

		// --- Transfer and recompose: Y_out = Y + (S_out - S); weights bilinear between block centres ---
		Plane out = in.luma;
		d.structure = A.s;
		d.structureOut = A.s;
		d.base = A.base;
		d.grain = Plane(w, h);
		for (size_t i = 0; i < d.grain.v.size(); ++i) d.grain.v[i] = A.d.v[i] - A.s.v[i];
		// Pass A: per pixel the weight-normalised raw delta d = g S_t,w - S (strict reference structure), the
		// reference's grain level, and the boost on the strict structure. Pass B: the delta Wiener and the cap.
		const size_t np = static_cast<size_t>(w) * static_cast<size_t>(h);
		std::vector<double> dRaw(np, 0.0), wSum(np, 0.0), nRef(np, 0.0), refMax(np, 0.0), bSum(np, 0.0), bDelta(np, 0.0);
		for (int y = 0; y < h; ++y)
			for (int x = 0; x < w; ++x)
			{
				const double gx = (x + 0.5) / B - 0.5, gy = (y + 0.5) / B - 0.5;
				const double fx = std::floor(gx), fy = std::floor(gy);
				const double tx = gx - fx, ty = gy - fy;
				const int i0 = static_cast<int>(fx), j0 = static_cast<int>(fy);
				const size_t pi = static_cast<size_t>(y) * w + x;
				double sumGs = 0.0, sumN = 0.0, ws = 0.0, rm = 0.0, bs2 = 0.0, bd = 0.0;
				const double S = A.s.at(x, y), St = A.st.at(x, y), bs = A.base.at(x, y);
				for (int k = 0; k < 4; ++k)
				{
					const int ii = (std::max)(0, (std::min)(bw - 1, i0 + (k & 1))), jj = (std::max)(0, (std::min)(bh - 1, j0 + (k >> 1)));
					const double om = ((k & 1) ? tx : 1.0 - tx) * ((k >> 1) ? ty : 1.0 - ty);
					const Contribution& c = contrib[static_cast<size_t>(jj) * bw + ii];
					if (om <= 0.0 || (c.w <= 0.0 && c.boost <= 0.0)) continue;
					if (c.w > 0.0 && c.a)
					{
						auto warp = [&](const Source& s, double& sw, double& bwv, double& nv)
						{
							double sx, sy;
							Position(s, c.cam, x, y, sx, sy);
							const Detail::Taps t = Detail::MakeTaps(sx, sy);
							sw = Detail::SampleTaps(s.st, t);
							bwv = Detail::SampleTaps(s.base, t);
							const int sbx = (std::max)(0, (std::min)(bw - 1, static_cast<int>(std::floor((sx + 0.5) / B)))), sby = (std::max)(0, (std::min)(bh - 1, static_cast<int>(std::floor((sy + 0.5) / B))));
							nv = s.N[static_cast<size_t>(sby) * bw + sbx];
						};
						double sw, bwv, nv;
						warp(*c.a, sw, bwv, nv);
						if (c.prev)
						{
							double sp, bp, np2;
							warp(*c.prev, sp, bp, np2);
							sw = c.xf * sw + (1.0 - c.xf) * sp;
							bwv = c.xf * bwv + (1.0 - c.xf) * bp;
							nv = c.xf * nv + (1.0 - c.xf) * np2;
						}
						const double g = (bwv > 1e-3 && bs > 1e-3) ? Clamp(bs / bwv, kGainLo, kGainHi) : 1.0;
						const double gs = g * sw;
						sumGs += om * c.w * gs;
						sumN += om * c.w * nv;
						rm = (std::max)(rm, std::fabs(gs));
						ws += om * c.w;
					}
					bd += om * c.boost * St;
					bs2 += om * c.boost;
				}
				wSum[pi] = ws; refMax[pi] = rm; bSum[pi] = bs2; bDelta[pi] = bd;
				if (ws > 0.0) { dRaw[pi] = sumGs / ws - S; nRef[pi] = sumN / ws; }
			}
		for (int y = 0; y < h; ++y)
			for (int x = 0; x < w; ++x)
			{
				const size_t pi = static_cast<size_t>(y) * w + x;
				double delta = bDelta[pi];
				if (wSum[pi] > 0.0)
				{
					double e = 0.0;
					for (int j = -1; j <= 1; ++j)
						for (int i = -1; i <= 1; ++i)
						{
							const int xx = (std::max)(0, (std::min)(w - 1, x + i)), yy = (std::max)(0, (std::min)(h - 1, y + j));
							const double dv = dRaw[static_cast<size_t>(yy) * w + xx];
							e += dv * dv;
						}
					e /= 9.0;
					const double Ncur = Detail::GridAt(A.N, bw, bh, B, x, y);
					const double phi = Clamp(1.0 - kKappaDelta * (Ncur + nRef[pi]) / (std::max)(e, kEnergyFloor), 0.0, 1.0);
					delta += wSum[pi] * phi * dRaw[pi];
				}
				if (delta != 0.0)
				{
					const double S = A.s.at(x, y);
					double sOut = S + delta;
					const double k = (std::max)(kPixelCap, 1.0 + bSum[pi]);
					const double cap = k * (std::max)(std::fabs(S), refMax[pi]);
					sOut = Clamp(sOut, -cap, cap);
					out.at(x, y) = in.luma.at(x, y) + (sOut - S);
					d.structureOut.at(x, y) = sOut;
				}
			}

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
