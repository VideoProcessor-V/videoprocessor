// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// U15 FieldRefine v1, CPU reference (doc 72 section 1 product 4 `refined_field`, P3r). Header-only, pure C++14,
// no Windows / libplacebo headers. FieldRefineGlsl.h is the shader twin; the WARP test holds Ref == Glsl.
//
// Law (per 8x8 SOURCE-pixel block = 4x4 pixels of the half luma):
//  * Seeds: the P3 vector (source px, over the pair A -> B) and, on blocks the P8 labels call camera, the camera-model
//    vector. A non-finite / invalid P3 vector gives no search (zero vector, confidence 0, flag kept).
//  * Search: DMVR-style, +-kRange (2) integer half-luma px around the seed, window = block + `margin` px on every
//    side (default 2 -> 8x8 half px), cost = SAD of A(window, integer indices, edge clamp) against B bilinearly sampled
//    at the same position + seed + offset (float arithmetic, fixed scan order dy outer / dx inner, strict < so the first
//    lowest wins). The camera seed replaces the P3 seed only when its best cost is strictly lower.
//  * Sub-pel: per axis, parabola through the three costs around the best (only when the best is strictly inside the
//    +-2 range and the curvature > 1e-6); offset clamped to +-0.5 half px. Output vector = (seed + d + sub) * 2 source px.
//  * Confidence u8 = 255 * clamp(1 - best / mean(25 costs)); 0 when the mean is ~0 (flat: no information).
//  * Flags: kRefined (output differs from P3 by > keepEps), kKept (otherwise, also invalid/occluded blocks),
//    kBoundary (a finite 4-neighbour P3 vector differs by > boundaryThr source px, or the best sits on the edge of the
//    search range on either axis: per-pixel checks needed), kOccluded (input occlusion flag: no search).
//  * Never starts the matcher: pure function of its inputs. BDOF/PROF-style per-pixel offsets are NEVER stored:
//    Offset() (and vpu_field_refine_offset in the GLSL) is an on-the-fly accessor for the consumer's own band.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace VpUnit
{
	namespace FieldRefine
	{
		constexpr int kRange = 2;                 // +-2 half-luma px
		constexpr int kSide = 2 * kRange + 1;     // 5
		constexpr int kCandidates = kSide * kSide;
		constexpr uint8_t kRefined = 1, kKept = 2, kBoundary = 4, kOccluded = 8;

		struct Params
		{
			int blockSource = 8;          // block edge in source px (half-luma block = blockSource / 2)
			int margin = 2;               // window margin in half-luma px
			float boundaryThr = 1.0f;     // source px: neighbour P3 vector difference that flags a boundary block
			float keepEps = 1.0f / 64.0f; // source px: output closer than this to P3 = kept
		};

		struct Block
		{
			float vx = 0.0f, vy = 0.0f;   // source px
			uint8_t confidence = 0;
			uint8_t flags = 0;
		};

		// One refined block as the product stores it: 8 bytes (vector RG16F, confidence, flags, padding).
		struct Packed
		{
			uint16_t vx = 0, vy = 0;
			uint8_t confidence = 0, flags = 0;
			uint16_t padding = 0;
		};
		static_assert(sizeof(Packed) == 8, "refined_field stores 8 B per block");

		struct Input
		{
			const float* lumaA = nullptr;       // luma at half resolution, 0..255 floats, w x h
			const float* lumaB = nullptr;
			int w = 0, h = 0;
			int gridW = 0, gridH = 0;           // blocks
			const float* p3 = nullptr;          // gridW*gridH*2 source px; non-finite = invalid
			const float* camera = nullptr;      // gridW*gridH*2 source px (optional)
			const uint8_t* cameraMask = nullptr;// nonzero = P8 camera block (optional)
			const uint8_t* occluded = nullptr;  // nonzero = occluded / disoccluded (optional)
		};

		inline float Sample(const float* plane, int w, int h, float x, float y)
		{
			const float fx = std::floor(x), fy = std::floor(y);
			const float tx = x - fx, ty = y - fy;
			const int ix = static_cast<int>(fx), iy = static_cast<int>(fy);
			const int x0 = std::min(std::max(ix, 0), w - 1), x1 = std::min(std::max(ix + 1, 0), w - 1);
			const int y0 = std::min(std::max(iy, 0), h - 1), y1 = std::min(std::max(iy + 1, 0), h - 1);
			const float a = plane[static_cast<size_t>(y0) * w + x0], b = plane[static_cast<size_t>(y0) * w + x1];
			const float c = plane[static_cast<size_t>(y1) * w + x0], d = plane[static_cast<size_t>(y1) * w + x1];
			const float top = a * (1.0f - tx) + b * tx;
			const float bot = c * (1.0f - tx) + d * tx;
			return top * (1.0f - ty) + bot * ty;
		}

		// SAD of the window with origin (ox, oy) in A against B shifted by (bx, by) half px.
		inline float WindowCost(const Input& in, int ox, int oy, int win, float bx, float by)
		{
			float s = 0.0f;
			for (int j = 0; j < win; ++j)
				for (int i = 0; i < win; ++i)
				{
					const int ax = std::min(std::max(ox + i, 0), in.w - 1), ay = std::min(std::max(oy + j, 0), in.h - 1);
					const float a = in.lumaA[static_cast<size_t>(ay) * in.w + ax];
					const float b = Sample(in.lumaB, in.w, in.h, static_cast<float>(ox + i) + bx, static_cast<float>(oy + j) + by);
					s += std::fabs(a - b);
				}
			return s;
		}

		// Parabola through (-1, cm), (0, c0), (+1, cp): the minimum's offset, 0 when not convex; clamped to +-0.5.
		inline float ParabolicOffset(float cm, float c0, float cp)
		{
			const float den = cm - 2.0f * c0 + cp;
			if (!(den > 1e-6f))
				return 0.0f;
			const float off = (cm - cp) / (2.0f * den);
			return std::min(std::max(off, -0.5f), 0.5f);
		}

		struct Surface
		{
			float cost[kCandidates];
			int bestX = 0, bestY = 0;   // offsets in -2..2
			float best = 0.0f, mean = 0.0f;
		};

		inline void SearchSurface(const Input& in, int bx, int by, int blockHalf, int margin, float seedHx, float seedHy, Surface& s)
		{
			const int win = blockHalf + 2 * margin;
			const int ox = bx * blockHalf - margin, oy = by * blockHalf - margin;
			float best = 0.0f, sum = 0.0f;
			int bestIdx = -1;
			for (int dy = -kRange; dy <= kRange; ++dy)
				for (int dx = -kRange; dx <= kRange; ++dx)
				{
					const int idx = (dy + kRange) * kSide + (dx + kRange);
					const float c = WindowCost(in, ox, oy, win, seedHx + static_cast<float>(dx), seedHy + static_cast<float>(dy));
					s.cost[idx] = c;
					sum += c;
					if (bestIdx < 0 || c < best)
					{
						best = c;
						bestIdx = idx;
					}
				}
			s.bestX = bestIdx % kSide - kRange;
			s.bestY = bestIdx / kSide - kRange;
			s.best = best;
			s.mean = sum / static_cast<float>(kCandidates);
		}

		inline bool Finite2(const float* v) { return std::isfinite(v[0]) && std::isfinite(v[1]); }

		inline Block RefineBlock(const Input& in, const Params& p, int bx, int by)
		{
			Block out;
			const size_t i = static_cast<size_t>(by) * in.gridW + bx;
			const float* seed = in.p3 + i * 2;
			if (!Finite2(seed))
			{
				out.flags = kKept;
				return out;
			}
			if (in.occluded && in.occluded[i])
			{
				out.vx = seed[0];
				out.vy = seed[1];
				out.flags = static_cast<uint8_t>(kOccluded | kKept);
				return out;
			}
			const int blockHalf = p.blockSource / 2;
			Surface s3;
			SearchSurface(in, bx, by, blockHalf, p.margin, seed[0] * 0.5f, seed[1] * 0.5f, s3);
			const Surface* win = &s3;
			float sx = seed[0] * 0.5f, sy = seed[1] * 0.5f;
			Surface sc;
			if (in.camera && in.cameraMask && in.cameraMask[i] && Finite2(in.camera + i * 2))
			{
				const float cx = in.camera[i * 2] * 0.5f, cy = in.camera[i * 2 + 1] * 0.5f;
				SearchSurface(in, bx, by, blockHalf, p.margin, cx, cy, sc);
				if (sc.best < s3.best)
				{
					win = &sc;
					sx = cx;
					sy = cy;
				}
			}
			float subX = 0.0f, subY = 0.0f;
			const int ix = win->bestX + kRange, iy = win->bestY + kRange;
			bool saturated = false;
			if (win->bestX > -kRange && win->bestX < kRange)
				subX = ParabolicOffset(win->cost[iy * kSide + ix - 1], win->cost[iy * kSide + ix], win->cost[iy * kSide + ix + 1]);
			else
				saturated = true;
			if (win->bestY > -kRange && win->bestY < kRange)
				subY = ParabolicOffset(win->cost[(iy - 1) * kSide + ix], win->cost[iy * kSide + ix], win->cost[(iy + 1) * kSide + ix]);
			else
				saturated = true;
			out.vx = (sx + static_cast<float>(win->bestX) + subX) * 2.0f;
			out.vy = (sy + static_cast<float>(win->bestY) + subY) * 2.0f;
			if (win->mean > 1e-6f)
			{
				float c = 1.0f - win->best / win->mean;
				c = std::min(std::max(c, 0.0f), 1.0f);
				out.confidence = static_cast<uint8_t>(std::floor(255.0f * c + 0.5f));
			}
			bool boundary = saturated;
			static const int nb[4][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } };
			for (const auto& n : nb)
			{
				const int nx = bx + n[0], ny = by + n[1];
				if (nx < 0 || ny < 0 || nx >= in.gridW || ny >= in.gridH)
					continue;
				const float* v = in.p3 + (static_cast<size_t>(ny) * in.gridW + nx) * 2;
				if (!Finite2(v))
					continue;
				if (std::fabs(v[0] - seed[0]) > p.boundaryThr || std::fabs(v[1] - seed[1]) > p.boundaryThr)
					boundary = true;
			}
			const bool moved = std::fabs(out.vx - seed[0]) > p.keepEps || std::fabs(out.vy - seed[1]) > p.keepEps;
			out.flags = static_cast<uint8_t>((moved ? kRefined : kKept) | (boundary ? kBoundary : 0));
			return out;
		}

		inline void RefineField(const Input& in, const Params& p, std::vector<Block>& out)
		{
			out.assign(static_cast<size_t>(in.gridW) * in.gridH, Block());
			for (int by = 0; by < in.gridH; ++by)
				for (int bx = 0; bx < in.gridW; ++bx)
					out[static_cast<size_t>(by) * in.gridW + bx] = RefineBlock(in, p, bx, by);
		}

		// IEEE binary16, round to nearest even (the product's RG16F).
		inline uint16_t FloatToHalf(float f)
		{
			uint32_t u;
			std::memcpy(&u, &f, 4);
			const uint32_t sign = (u >> 16) & 0x8000u;
			int32_t exp = static_cast<int32_t>((u >> 23) & 0xFF) - 127 + 15;
			uint32_t man = u & 0x7FFFFFu;
			if (((u >> 23) & 0xFF) == 0xFF)
				return static_cast<uint16_t>(sign | 0x7C00u | (man ? 0x200u : 0u));
			if (exp >= 31)
				return static_cast<uint16_t>(sign | 0x7C00u);
			if (exp <= 0)
			{
				if (exp < -10)
					return static_cast<uint16_t>(sign);
				man |= 0x800000u;
				const int shift = 14 - exp;
				uint32_t h = man >> shift;
				const uint32_t rem = man & ((1u << shift) - 1u), halfway = 1u << (shift - 1);
				if (rem > halfway || (rem == halfway && (h & 1u)))
					++h;
				return static_cast<uint16_t>(sign | h);
			}
			uint32_t h = (static_cast<uint32_t>(exp) << 10) | (man >> 13);
			const uint32_t rem = man & 0x1FFFu;
			if (rem > 0x1000u || (rem == 0x1000u && (h & 1u)))
				++h;
			return static_cast<uint16_t>(sign | h);
		}

		inline float HalfToFloat(uint16_t h)
		{
			const uint32_t sign = (h & 0x8000u) << 16;
			const uint32_t exp = (h >> 10) & 0x1Fu, man = h & 0x3FFu;
			uint32_t u;
			if (exp == 0)
			{
				if (man == 0)
					u = sign;
				else
				{
					int e = -1;
					uint32_t m = man;
					do { ++e; m <<= 1; } while (!(m & 0x400u));
					u = sign | (static_cast<uint32_t>(127 - 15 - e) << 23) | ((m & 0x3FFu) << 13);
				}
			}
			else if (exp == 31)
				u = sign | 0x7F800000u | (man << 13);
			else
				u = sign | ((exp + 127 - 15) << 23) | (man << 13);
			float f;
			std::memcpy(&f, &u, 4);
			return f;
		}

		inline Packed Pack(const Block& b)
		{
			Packed p;
			p.vx = FloatToHalf(b.vx);
			p.vy = FloatToHalf(b.vy);
			p.confidence = b.confidence;
			p.flags = b.flags;
			return p;
		}

		inline Block Unpack(const Packed& p)
		{
			Block b;
			b.vx = HalfToFloat(p.vx);
			b.vy = HalfToFloat(p.vy);
			b.confidence = p.confidence;
			b.flags = p.flags;
			return b;
		}

		// On-the-fly PROF-style per-pixel offset (source px) at source pixel (px, py): the bilinear blend of the four
		// nearest block-centre vectors minus the containing block's own vector, so block vector + offset is a smooth
		// field inside a motion-coherent region. Neighbours flagged boundary / occluded contribute the own vector (no
		// smearing across a discontinuity); a boundary / occluded own block gets 0. Never stored.
		inline void Offset(const Block* field, int gridW, int gridH, int blockSource, float px, float py, float& ox, float& oy)
		{
			ox = oy = 0.0f;
			const float bs = static_cast<float>(blockSource);
			const int cx = std::min(std::max(static_cast<int>(std::floor(px / bs)), 0), gridW - 1);
			const int cy = std::min(std::max(static_cast<int>(std::floor(py / bs)), 0), gridH - 1);
			const Block& own = field[static_cast<size_t>(cy) * gridW + cx];
			if (own.flags & (kBoundary | kOccluded))
				return;
			const float u = px / bs - 0.5f, v = py / bs - 0.5f;
			const float fu = std::floor(u), fv = std::floor(v);
			const float tx = u - fu, ty = v - fv;
			const int i0 = static_cast<int>(fu), j0 = static_cast<int>(fv);
			float vx[4], vy[4];
			for (int k = 0; k < 4; ++k)
			{
				const int ii = std::min(std::max(i0 + (k & 1), 0), gridW - 1);
				const int jj = std::min(std::max(j0 + (k >> 1), 0), gridH - 1);
				const Block& b = field[static_cast<size_t>(jj) * gridW + ii];
				const bool use = !(b.flags & (kBoundary | kOccluded));
				vx[k] = use ? b.vx : own.vx;
				vy[k] = use ? b.vy : own.vy;
			}
			const float ax = (vx[0] * (1.0f - tx) + vx[1] * tx) * (1.0f - ty) + (vx[2] * (1.0f - tx) + vx[3] * tx) * ty;
			const float ay = (vy[0] * (1.0f - tx) + vy[1] * tx) * (1.0f - ty) + (vy[2] * (1.0f - tx) + vy[3] * tx) * ty;
			ox = ax - own.vx;
			oy = ay - own.vy;
		}
	}
}
