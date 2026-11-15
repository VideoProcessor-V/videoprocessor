// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// input.source_analysis/focus_map: the per-region defocus level and focus-change flag (job #1 L-FOCUS; owner 2026-11-12 22:16 / 22:18).
// The CPU reference. Everything here is a pure function of SHARED products: P11 region block statistics (the P10 band energies E1 / E2 / E3
// and the grain level N, copied into the stats block), the P2 backward vector grid (or the P7 camera translation, visibly flagged) and the
// P11 block statistics of the previous frame. No statistic is measured here; no private band, tensor or vector copy exists.
//
// Defocus (0 = sharp, 1 = fully defocused). TWO laws, one per frame pair (never mixed across the delta):
// EDGE LAW (unit v2; owner 11-14 05:25 "focus map built ON the edge map", registered fix FOCUS-EDGEMAP-SWITCH): the sharpness input is
//   the shared kEdgeMap (3x3 Sobel |G| per source pixel, 10-bit codes). Per P11 block: peak = sum|G|^4 / sum|G|^2 (the energy-weighted
//   |G|^2: about 0.7 x the edge-crest |G|^2 for any edge, independent of how much of the block the edge covers, so the law does not
//   depend on the block size), the grain floor kSobelPeakNoiseGain x N (white noise of variance s^2: |G|^2 is exponential with mean
//   24 s^2, so sum|G|^4 / sum|G|^2 -> 48 s^2), normalised by the same P11 band energy:
//     sharp = max(peak - 48 N, 0) (no split: N = 0, kNoSplit);  ratio = sharp / (48 total + eps);  defocus = 1 - Ramp(ratio, edgeLo, edgeHi)
//   A step edge blurred by a Gaussian of sigma b gives ratio ~ 0.6 / b^2 (b = 1 px: 0.6, sharp; b = 2: 0.15; b = 3: 0.07, defocused).
//   edgeLo / edgeHi are PROVISIONAL (derived from that model, not yet fitted on real rack-focus clips: 102b FOCUS-EDGE-LAW-CALIBRATE).
// INTERIM LAW (unit v1, used while the edge map is not served on the CPU, flagged kSharpnessInterim by the fill): the share of the
//   block's band energy that sits in the fine band.
//   fine = max(E1 - N, 0)  (N = the grain floor; no split: fine = E1, flag kNoSplit),  total = fine + E2 + E3
//   ratio = fine / (total + eps);  defocus = 1 - Ramp(ratio, ratioLo, ratioHi)
// Both: confidence = Ramp(total, energyLo, energyHi) (the P11 band energy, unchanged), the same coverage gate and the same flags.
// Focus change: defocus(this block) - defocus(the block this one came from in the previous frame, found by the motion), flagged when
//   |delta| >= changeThreshold and both confidences >= minConfidence. kSharper says the region became SHARPER than before.
// HARD TD RULE (owner 11-12 22:16): detail sharper than the current frame's focus state is never imported into a region whose focus
// changed; the temporal-detail consumer row is declared only (no wiring here) and reads kChanged + the defocus level.
//
// The templates (DefocusBlock<T>, ChangeBlock<T>) are the tolerance oracle of the GLSL mirror (FocusMapGlsl.h), which mirrors the INTERIM
// law only; the edge law's GPU mirror is owed with the GPU form (102b FOCUS-EDGEMAP-SWITCH follow-up; no GPU form is registered).
#pragma once

#include <VP-V/core/Ramp.h>
#include <VP-V/modules/input/source_analysis/regions/RegionEvidence.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace FocusMap
{
	constexpr uint32_t kUnitVersion = 2;   // == VpSkeleton::kExpectedFocusMapUnit (v2: the edge law, FOCUS-EDGEMAP-SWITCH)
	constexpr int kBytesPerBlock = 8;      // u16 defocus (unorm), s16 delta (s1.15), u16 confidence (unorm), u16 flags

	enum Flag : uint16_t
	{
		kValid = 1,            // the block carries evidence (coverage, energy)
		kNoSplit = 2,          // no P10 grain split: fine = E1 (visible: P11 without bands)
		kNoMotion = 4,         // neither P2 nor P7 was available: zero motion assumed (visible)
		kCameraMotion = 8,     // the P7 camera translation stood in for the P2 vector grid (visible)
		kNoPrevious = 16,      // no previous frame statistics (first frame, a cut, a released frame): no change flag
		kChanged = 32,         // the focus state changed along the motion (the TD rule reads this)
		kSharper = 64,         // ... and the region became sharper (delta < 0)
		kLowConfidence = 128,  // below minConfidence: never flagged changed
		// Set by the owner fill (FocusMapFill.h), never by ComputeGrid or the GLSL mirror:
		kNoPreviousRetained = 256,  // with kNoPrevious: the previous frame's P11 (or, under the edge law, edge map) spot was RELEASED
		                            // (retention missing), not merely absent
		kSharpnessInterim = 512     // the INTERIM law was used: the edge map was not served on the CPU for this frame (visible)
	};

	struct Params
	{
		double ratioLo = 0.35, ratioHi = 0.75;        // fine-band share mapped to defocus 1 .. 0
		double eps = 1e-3;
		double energyLo = 2.0, energyHi = 30.0;       // total band energy (codes^2) mapped to confidence 0 .. 1
		double changeThreshold = 0.25;                // |defocus delta| that is a focus change
		double minConfidence = 0.3;
		double minCoverage = 0.5;
		// Edge law (v2): the normalised peak gradient energy mapped to defocus 1 .. 0. PROVISIONAL (step-edge model, see the header;
		// 102b FOCUS-EDGE-LAW-CALIBRATE fits them on real rack-focus clips).
		double edgeLo = 0.05, edgeHi = 0.6;
	};

	// White noise of variance s^2 through the 3x3 Sobel: gx, gy uncorrelated, each of variance 12 s^2 (weights 1, 2, 1 twice), so |G|^2 is
	// exponential with mean 24 s^2 and E|G|^4 / E|G|^2 = 48 s^2: the grain floor of the edge law's peak per unit of N.
	constexpr double kSobelPeakNoiseGain = 48.0;

	// The ONE linear ramp (core/Ramp.h, U25; rule 20): the private copy that stood here is removed. Bit-identical for every ramp of this
	// law (each has hi > lo: RampMath::Ramp is then Clamp01((x - lo) / (hi - lo)), the removed copy's arithmetic, NaN passing through).
	using RampMath::Ramp;

	// s = one P11 block (RegionEvidence::kStatsChannels floats / doubles). flags gets kValid / kNoSplit.
	template<class T> inline void DefocusBlock(const Params& p, const T* s, T& defocus, T& confidence, uint32_t& flags)
	{
		using namespace RegionEvidence;
		flags = 0;
		defocus = T(0);
		confidence = T(0);
		if (!(s[kCoverage] >= T(p.minCoverage))) return;
		const T n = s[kStatN];
		const bool haveSplit = n >= T(0);
		if (!haveSplit) flags |= kNoSplit;
		const T e1 = s[kE1] > T(0) ? s[kE1] : T(0);
		const T e2 = s[kE2] > T(0) ? s[kE2] : T(0);
		const T e3 = s[kE3] > T(0) ? s[kE3] : T(0);
		T fine = haveSplit ? e1 - n : e1;
		if (fine < T(0)) fine = T(0);
		const T total = fine + e2 + e3;
		const T ratio = fine / (total + T(p.eps));
		defocus = T(1) - Ramp<T>(ratio, T(p.ratioLo), T(p.ratioHi));
		confidence = Ramp<T>(total, T(p.energyLo), T(p.energyHi));
		flags |= kValid;
	}

	// The EDGE LAW (v2) of one block: s = the P11 block (coverage, N, the band energies for the normaliser and the confidence, exactly as
	// the interim law reads them); sum2 / sum4 = the block's sums of |G|^2 / |G|^4 from the shared edge map (EdgeSums). Same gate, same
	// confidence and the same flags as DefocusBlock; only the sharpness term differs.
	template<class T> inline void DefocusBlockEdge(const Params& p, const T* s, T sum2, T sum4, T& defocus, T& confidence, uint32_t& flags)
	{
		using namespace RegionEvidence;
		flags = 0;
		defocus = T(0);
		confidence = T(0);
		if (!(s[kCoverage] >= T(p.minCoverage))) return;
		const T n = s[kStatN];
		const bool haveSplit = n >= T(0);
		if (!haveSplit) flags |= kNoSplit;
		const T e1 = s[kE1] > T(0) ? s[kE1] : T(0);
		const T e2 = s[kE2] > T(0) ? s[kE2] : T(0);
		const T e3 = s[kE3] > T(0) ? s[kE3] : T(0);
		T fine = haveSplit ? e1 - n : e1;
		if (fine < T(0)) fine = T(0);
		const T total = fine + e2 + e3;
		const T peak = sum2 > T(0) ? sum4 / sum2 : T(0);
		T sharp = peak - (haveSplit ? T(kSobelPeakNoiseGain) * n : T(0));
		if (sharp < T(0)) sharp = T(0);
		const T ratio = sharp / (T(kSobelPeakNoiseGain) * total + T(p.eps));
		defocus = T(1) - Ramp<T>(ratio, T(p.edgeLo), T(p.edgeHi));
		confidence = Ramp<T>(total, T(p.energyLo), T(p.energyHi));
		flags |= kValid;
	}

	// The change flag of one block against its source in the previous frame (prevValid = false: no reference).
	template<class T> inline void ChangeBlock(const Params& p, T defocus, T confidence, uint32_t flags, bool prevValid, T prevDefocus, T prevConfidence,
		T& delta, uint32_t& outFlags)
	{
		outFlags = flags;
		delta = T(0);
		if (!(flags & kValid)) return;
		if (!prevValid) { outFlags |= kNoPrevious; return; }
		delta = defocus - prevDefocus;
		const T c = confidence < prevConfidence ? confidence : prevConfidence;
		if (c < T(p.minConfidence)) { outFlags |= kLowConfidence; return; }
		const T a = delta < T(0) ? -delta : delta;
		if (a >= T(p.changeThreshold))
		{
			outFlags |= kChanged;
			if (delta < T(0)) outFlags |= kSharper;
		}
	}

	struct Block
	{
		float defocus = 0, delta = 0, confidence = 0;
		uint16_t flags = 0;
	};

	// ---- 8-byte packed form (the CPU form of the product) ----
	inline void Pack(const Block& b, uint8_t* out)
	{
		auto u16 = [](float v) { return static_cast<uint16_t>(std::lround((std::min)((std::max)(v, 0.0f), 1.0f) * 65535.0f)); };
		const uint16_t d = u16(b.defocus), c = u16(b.confidence);
		const int16_t dl = static_cast<int16_t>(std::lround((std::min)((std::max)(b.delta, -1.0f), 1.0f) * 32767.0f));
		std::memcpy(out, &d, 2);
		std::memcpy(out + 2, &dl, 2);
		std::memcpy(out + 4, &c, 2);
		std::memcpy(out + 6, &b.flags, 2);
	}
	inline Block Unpack(const uint8_t* in)
	{
		uint16_t d, c, f;
		int16_t dl;
		std::memcpy(&d, in, 2);
		std::memcpy(&dl, in + 2, 2);
		std::memcpy(&c, in + 4, 2);
		std::memcpy(&f, in + 6, 2);
		Block b;
		b.defocus = d / 65535.0f;
		b.delta = dl / 32767.0f;
		b.confidence = c / 65535.0f;
		b.flags = f;
		return b;
	}

	// ---- geometry and motion ----
	struct Geometry
	{
		int width = 0, height = 0;
		int block = 0, gw = 0, gh = 0;   // the P11 grid
		int p2Cell = 8;                  // source px per P2 cell edge
		int p2gw = 0, p2gh = 0;
		static Geometry Make(int w, int h, int p2CellPx = 8)
		{
			Geometry g;
			g.width = w; g.height = h;
			g.block = RegionEvidence::BlockSizeFor(h);
			g.gw = (w + g.block - 1) / g.block;
			g.gh = (h + g.block - 1) / g.block;
			g.p2Cell = p2CellPx;
			g.p2gw = (w + p2CellPx - 1) / p2CellPx;
			g.p2gh = (h + p2CellPx - 1) / p2CellPx;
			return g;
		}
		size_t Blocks() const { return static_cast<size_t>(gw) * gh; }
	};

	// Per P11 block: the sums of |G|^2 and |G|^4 over the block's pixels of the edge map (the kEdgeMap CPU form: width x height float32
	// |G| in 10-bit codes, row-major, tightly packed; kEdgeMap's declared size term is 4 B per source pixel). A pure reduction of the
	// shared product, accumulated in double: nothing is measured from the picture here (no gradient of its own). `edgeBytes` is the
	// published byte payload (width x height x 4 bytes); each row is copied out with memcpy (no aliasing of the byte buffer as floats).
	inline void EdgeSums(const Geometry& g, const uint8_t* edgeBytes, std::vector<float>& sum2, std::vector<float>& sum4)
	{
		const size_t n = g.Blocks();
		std::vector<double> a2(n, 0.0), a4(n, 0.0);
		std::vector<float> line(static_cast<size_t>(g.width > 0 ? g.width : 0));
		for (int y = 0; y < g.height; ++y)
		{
			const size_t row = static_cast<size_t>(y / g.block) * g.gw;
			std::memcpy(line.data(), edgeBytes + static_cast<size_t>(y) * g.width * sizeof(float), line.size() * sizeof(float));
			for (int x = 0; x < g.width; ++x)
			{
				const double v = line[x];
				const double v2 = v * v;
				const size_t i = row + static_cast<size_t>(x / g.block);
				a2[i] += v2;
				a4[i] += v2 * v2;
			}
		}
		sum2.resize(n);
		sum4.resize(n);
		for (size_t i = 0; i < n; ++i)
		{
			sum2[i] = static_cast<float>(a2[i]);
			sum4[i] = static_cast<float>(a4[i]);
		}
	}

	// P2 CPU form: per cell fwdX, fwdY, bwdX, bwdY (int16, S10.5 of half-size px: 1/16 source px) then fwdCost, bwdCost (u8) = 10 B.
	constexpr int kP2BytesPerCell = 10;
	// P7 CPU form: float tx, ty (source px the content moved from the previous frame to this one), confidence, valid (0 / 1) = 16 B.
	constexpr int kP7Bytes = 16;

	enum class MotionSource : uint8_t { None = 0, Field = 1, Camera = 2 };

	struct BlockMotion
	{
		float dx = 0, dy = 0;   // source px from this block's centre to where the same content was in the previous frame
		MotionSource source = MotionSource::None;
	};

	// Mean backward vector of the P2 cells inside the block (the cost byte weights: low cost = more trusted).
	inline BlockMotion FieldMotion(const Geometry& g, const uint8_t* p2, int bx, int by)
	{
		double sx = 0, sy = 0, sw = 0;
		const int x0 = bx * g.block, y0 = by * g.block;
		const int cx0 = x0 / g.p2Cell, cy0 = y0 / g.p2Cell;
		const int cx1 = (std::min)((x0 + g.block + g.p2Cell - 1) / g.p2Cell, g.p2gw), cy1 = (std::min)((y0 + g.block + g.p2Cell - 1) / g.p2Cell, g.p2gh);
		for (int cy = cy0; cy < cy1; ++cy)
			for (int cx = cx0; cx < cx1; ++cx)
			{
				const uint8_t* c = p2 + (static_cast<size_t>(cy) * g.p2gw + cx) * kP2BytesPerCell;
				int16_t bxv, byv;
				std::memcpy(&bxv, c + 4, 2);
				std::memcpy(&byv, c + 6, 2);
				const double w = 1.0 - c[9] / 255.0 + 1e-3;
				sx += w * bxv; sy += w * byv; sw += w;
			}
		BlockMotion m;
		if (sw <= 0) return m;
		m.dx = static_cast<float>(sx / sw / 16.0);   // S10.5 of half-size px -> source px
		m.dy = static_cast<float>(sy / sw / 16.0);
		m.source = MotionSource::Field;
		return m;
	}

	inline BlockMotion CameraMotionOf(const float* p7)
	{
		BlockMotion m;
		if (p7[3] < 0.5f) return m;
		m.dx = -p7[0];   // content moved by +t from the previous frame: it was at p - t
		m.dy = -p7[1];
		m.source = MotionSource::Camera;
		return m;
	}

	// The edge-map sums of one frame (EdgeSums), the edge law's sharpness input. Empty = the edge map was not served (interim law).
	struct EdgeInput
	{
		const float* sum2 = nullptr;
		const float* sum4 = nullptr;
		bool Served() const { return sum2 != nullptr && sum4 != nullptr; }
	};

	// The whole grid. stats / prevStats: gw x gh blocks of kPrecursorChannels floats (the P11 CPU form); prevStats may be null (no
	// reference); p2 / p7 may be null (visible flags). forceNoSplit: P10 itself was unavailable.
	// edge / prevEdge (the edge law, v2): when `edge` is served, this frame uses the edge law and the previous frame is a reference ONLY
	// under the same law (prevEdge served too), else it is no reference (kNoPrevious): the delta never mixes two laws. When `edge` is not
	// served, the interim law runs exactly as before the switch (prevEdge ignored), bit for bit.
	inline void ComputeGrid(const Params& p, const Geometry& g, const float* stats, const float* prevStats, const uint8_t* p2, const float* p7,
		bool forceNoSplit, std::vector<Block>& out, EdgeInput edge = EdgeInput(), EdgeInput prevEdge = EdgeInput())
	{
		using namespace RegionEvidence;
		const size_t n = g.Blocks();
		out.assign(n, Block());
		std::vector<float> pd(n, 0.0f), pc(n, 0.0f);
		std::vector<uint8_t> pv(n, 0);
		const bool edgeLaw = edge.Served();
		if (edgeLaw && !prevEdge.Served()) prevStats = nullptr;   // no reference under the edge law: never compare across laws
		if (prevStats)
			for (size_t i = 0; i < n; ++i)
			{
				float s[kStatsChannels];
				std::memcpy(s, prevStats + i * kPrecursorChannels, sizeof(s));
				if (forceNoSplit) s[kStatN] = -1.0f;
				float d = 0, c = 0;
				uint32_t f = 0;
				if (edgeLaw) DefocusBlockEdge<float>(p, s, prevEdge.sum2[i], prevEdge.sum4[i], d, c, f);
				else DefocusBlock<float>(p, s, d, c, f);
				pd[i] = d; pc[i] = c; pv[i] = (f & kValid) ? 1 : 0;
			}
		for (int by = 0; by < g.gh; ++by)
			for (int bx = 0; bx < g.gw; ++bx)
			{
				const size_t i = static_cast<size_t>(by) * g.gw + bx;
				float s[kStatsChannels];
				std::memcpy(s, stats + i * kPrecursorChannels, sizeof(s));
				if (forceNoSplit) s[kStatN] = -1.0f;
				float d, c;
				uint32_t f;
				if (edgeLaw) DefocusBlockEdge<float>(p, s, edge.sum2[i], edge.sum4[i], d, c, f);
				else DefocusBlock<float>(p, s, d, c, f);
				BlockMotion m;
				if (p2) m = FieldMotion(g, p2, bx, by);
				if (m.source == MotionSource::None && p7) m = CameraMotionOf(p7);
				if (m.source == MotionSource::None) f |= kNoMotion;
				else if (m.source == MotionSource::Camera) f |= kCameraMotion;
				const float px = (bx + 0.5f) * g.block + m.dx, py = (by + 0.5f) * g.block + m.dy;
				const int sx = static_cast<int>(std::floor(px / g.block)), sy = static_cast<int>(std::floor(py / g.block));
				bool pok = prevStats && sx >= 0 && sy >= 0 && sx < g.gw && sy < g.gh;
				const size_t j = pok ? static_cast<size_t>(sy) * g.gw + sx : 0;
				pok = pok && pv[j];
				float delta;
				uint32_t fo;
				ChangeBlock<float>(p, d, c, f, pok, pok ? pd[j] : 0.0f, pok ? pc[j] : 0.0f, delta, fo);
				Block& b = out[i];
				b.defocus = d; b.delta = delta; b.confidence = c; b.flags = static_cast<uint16_t>(fo);
			}
	}
}
