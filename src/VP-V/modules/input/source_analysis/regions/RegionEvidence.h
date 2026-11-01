// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// input.source_analysis/regions: the SHARED REGION EVIDENCE of one decoded source frame (owner rules: every repair or
// enhancement is regionally adaptive; the grain map is computed ONCE, EARLY, on the decoded source and shared; whatever
// several adaptive processes need is precomputed once). Two layers:
//
//   Layer 1, stored precursors (skeleton products, tiered by the VRAM governor):
//     grain_split         repair.grain_split (REUSED law, not duplicated), computed on the luma CLAMPED INTO THE ACTIVE
//                         PICTURE (ExtendPicture: bars replaced by the picture mirrored about its edge, so a bar never
//                         lowers the grain minimum or smears into the bands). Besides its planes (base, f D1, f D2, f) its pass writes one
//                         block texel per region block: (E1, E2, E3, N) = mean D1^2, D2^2, D3^2 over the block's picture
//                         pixels and the grain level N (BandTexel). The split pass already forms D1..D3 and reduces
//                         sub-block energies, so the texel costs no extra read.
//     region_block_stats  per block, from the RESIDENT decoded luma plane (no extra full-resolution texture): mean luma,
//                         local min/max, max gradient, mean structure tensor (Jxx, Jyy, Jxy: magnitude and orientation),
//                         Laplacian energy, the off-edge Laplacian energy (ringing precursor), plane-fit slopes, mean nits,
//                         mean PQ value, picture coverage, off-edge share, post-tone-mapping visibility; copied in: the
//                         split's E1..E3 and N. Plus the CPU-packed aux texel (overlay flag, camera confidence, camera code).
//   Layer 2, thin per-consumer maps, NOT STORED (owner baseline 2026-10-31: memory): each consumer derives them inside its
//     own pass (DeriveBlock / GLSL region_evidence(): about 30 ALU, sqrt only, 6 texel fetches): grain level, edge
//     strength, local edge contrast, ringing (vs the grain floor), banding-prone (flat/gradient), texture energy
//     (texture-deficit reference), brightness, visibility, flags (in picture, text/overlay, grain unavailable, camera
//     tiny mover), camera confidence and label. No derived map is stored (README: numbers).
//
// Inputs reused, never recomputed: the grain split, the trusted active-picture rect (FrameAnalysis::Identity::picture,
// Bill's crop decision copied read-only), overlay rectangles (Bill's SubtitleBoxResult bounds / line panels, copied by the
// host as plain rects), an optional text-guard grid, and the camera model record (CameraModel::CameraMotion). Bars: every
// read is clamped INTO the picture rect; blocks less than half covered by picture carry no evidence (coverage stat says
// how much picture a block holds).
//
// Block size: derived from the frame (BlockSizeFor): 16 px at the grain law's 1080-line reference, scaled by the nearest
// power of two of height / 1080, clamped to 8..64 (8 at 720 and below, 32 at 2160, 64 at 4320). The block count is then
// about the same at every size, so precursor memory and derive cost are resolution-independent.
//
// Luma domain: 10-bit studio codes (black 64, white 940; BTB/WTW kept, as in FrameAnalysisStore).
//
// CPU reference = the double instantiation of the templates the float mirror uses (StatsBlock<T>, BandTexelBlock<T>,
// DeriveBlock<T>); RegionEvidenceGlsl.h holds the shader with the same arithmetic order. Raster loops, fixed reduction
// order, no threads: deterministic. Header-only, C++14, no Windows or libplacebo headers.
#pragma once

#include <VP-V/core/Ancestry.h>
#include <VP-V/core/Products.h>
#include <VP-V/modules/motion/estimation/global_model/CameraMotion.h>
#include <VP-V/modules/repair/grain_split/GrainSplit.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace GrainSplit { namespace Gpu { struct Frame; } }   // GrainSplitGpu.h (libplacebo-free forward declaration)

namespace RegionEvidence
{
	// Active picture / overlay rectangles in source-raster px (right/bottom exclusive), same layout as FrameAnalysis::Rect.
	struct Rect
	{
		int left = 0, top = 0, right = 0, bottom = 0;
		bool Empty() const { return right <= left || bottom <= top; }
		bool operator==(const Rect& o) const { return left == o.left && top == o.top && right == o.right && bottom == o.bottom; }
	};

	template <class R>
	inline Rect FromRect(const R& r) { Rect o; o.left = r.left; o.top = r.top; o.right = r.right; o.bottom = r.bottom; return o; }

	enum class Transfer : int { SdrGamma = 0, Pq = 1, Hlg = 2 };

	// Flags (layer 2, an exact small integer stored as float in the shader).
	enum Flag : uint32_t
	{
		kInPicture = 1u << 0,     // at least half of the block (inside the frame) is active picture
		kOverlay = 1u << 1,       // text / subtitle / overlay / static logo
		kNoGrain = 1u << 2,       // no grain split for this frame: grain, texture, ringing and banding unknown (0)
		kTinyMover = 1u << 3,     // camera model: a sub-block / tiny fast mover
	};

	// ---- block geometry ----------------------------------------------------------------------------------------------

	// 16 px at 1080 lines (the grain law's reference) times the nearest power of two of height / 1080, clamped to 8..64.
	inline int BlockSizeFor(int frameHeight)
	{
		if (frameHeight <= 0) return 16;
		const double e = std::floor(std::log2(static_cast<double>(frameHeight) / 1080.0) + 0.5);
		const int shift = static_cast<int>(e);
		const int b = shift >= 0 ? (16 << (std::min)(shift, 2)) : (16 >> (std::min)(-shift, 1));
		return (std::max)(8, (std::min)(64, b));
	}

	struct Grid
	{
		int w = 0, h = 0, gw = 0, gh = 0, B = 16;
		size_t Count() const { return static_cast<size_t>(gw) * static_cast<size_t>(gh); }
	};

	inline Grid MakeGrid(int w, int h, int B)
	{
		Grid g;
		g.w = w; g.h = h; g.B = B > 0 ? B : 16;
		g.gw = (w + g.B - 1) / g.B; g.gh = (h + g.B - 1) / g.B;
		return g;
	}

	// The picture rect clipped to the frame; an empty rect means the whole raster.
	inline Rect PictureOrFrame(const Rect& picture, int w, int h)
	{
		Rect r = picture;
		r.left = (std::max)(0, r.left); r.top = (std::max)(0, r.top);
		r.right = (std::min)(w, r.right); r.bottom = (std::min)(h, r.bottom);
		if (r.Empty()) { r.left = 0; r.top = 0; r.right = w; r.bottom = h; }
		return r;
	}

	// Mirror index of i into [lo, hi) (reflection about the picture edge, then clamped for bars deeper than the picture).
	inline int ReflectInto(int i, int lo, int hi)
	{
		if (i < lo) i = lo + (lo - 1 - i);
		else if (i >= hi) i = hi - 1 - (i - hi);
		return i < lo ? lo : (i > hi - 1 ? hi - 1 : i);
	}

	// The luma the shared split is computed on: bars replaced by the picture MIRRORED about its edge, so no bar value
	// reaches the split's Gaussians, and the grain minimum near a bar sees picture-like grain (edge replication would read
	// as grain-free and lower N). Inside the picture it is the luma unchanged.
	inline GrainSplit::Plane ExtendPicture(const GrainSplit::Plane& y, const Rect& picture)
	{
		const Rect r = PictureOrFrame(picture, y.w, y.h);
		GrainSplit::Plane o(y.w, y.h);
		for (int yy = 0; yy < y.h; ++yy)
		{
			const int cy = ReflectInto(yy, r.top, r.bottom);
			for (int x = 0; x < y.w; ++x) o.at(x, yy) = y.at(ReflectInto(x, r.left, r.right), cy);
		}
		return o;
	}

	// ---- transfer functions (luma only) --------------------------------------------------------------------------------

	// ST 2084 constants; the two reciprocals are written as literals so the C++ and GLSL sides round the same decimal.
	struct Pq
	{
		static constexpr double m1 = 0.1593017578125, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
		static constexpr double invM1 = 6.277394636015326, invM2 = 0.012683313515655966, m2 = 78.84375;
	};

	template <class T> inline T Clamp01(T v) { return v < T(0) ? T(0) : (v > T(1) ? T(1) : v); }
	// Linear ramp lo..hi; a degenerate ramp (hi <= lo) is a step at hi (never NaN).
	template <class T> inline T Ramp(T v, T lo, T hi) { return hi > lo ? Clamp01((v - lo) / (hi - lo)) : (v >= hi ? T(1) : T(0)); }

	// Display luminance (nits) of a 10-bit studio luma code. SDR: white * v^2.4 (BTB -> 0, WTW above white kept);
	// PQ: ST 2084 EOTF of v clamped 0..1; HLG: BT.2100 inverse OETF and a 1000-nit luma OOTF (gamma 1.2).
	template <class T>
	inline T Nits(T code, int transfer, T sdrWhite)
	{
		const T v = (code - T(64)) / T(876);
		if (transfer == static_cast<int>(Transfer::Pq))
		{
			const T e = Clamp01(v);
			const T p = std::pow(e, T(Pq::invM2));
			const T num = (std::max)(p - T(Pq::c1), T(0));
			return T(10000) * std::pow(num / (T(Pq::c2) - T(Pq::c3) * p), T(Pq::invM1));
		}
		if (transfer == static_cast<int>(Transfer::Hlg))
		{
			const T e = Clamp01(v);
			const T s = e <= T(0.5) ? e * e / T(3) : (std::exp((e - T(0.55991073)) / T(0.17883277)) + T(0.28466892)) / T(12);
			return T(1000) * std::pow(s, T(1.2));
		}
		const T vv = (std::max)(v, T(0));
		return sdrWhite * std::pow(vv, T(2.4));
	}

	// ST 2084 inverse EOTF: nits -> 0..1 perceptual value.
	template <class T>
	inline T PqOf(T nits)
	{
		const T y = Clamp01(nits / T(10000));
		const T p = std::pow(y, T(Pq::m1));
		return std::pow((T(Pq::c1) + T(Pq::c2) * p) / (T(1) + T(Pq::c3) * p), T(Pq::m2));
	}

	// ---- layer 1: block statistics -----------------------------------------------------------------------------------

	constexpr int kStatsTextures = 5;   // rgba32f each, one texel per block (written by the stats pass)
	constexpr int kStatsChannels = kStatsTextures * 4;
	constexpr int kAuxIndex = kStatsChannels;   // the aux texel follows (packed on the CPU, uploaded)
	constexpr int kPrecursorTextures = kStatsTextures + 1;
	constexpr int kPrecursorChannels = kPrecursorTextures * 4;

	// Channel layout (index = texture * 4 + component):
	//  S0 (meanY, minY, maxY, gmax)        codes; min/max over the block's pixels and their 4-neighbours (an edge on a block
	//                                      border is seen by both blocks); gmax = max |grad| (central differences), codes/px
	//  S1 (Jxx, Jyy, Jxy, lapE)            mean structure tensor (codes^2 / px^2), mean Laplacian^2 (codes^2)
	//  S2 (E1, E2, E3, offLap)             the split's band energies (copied from its block texel); mean Laplacian^2 over
	//                                      the off-edge pixels (|grad| < gmax / 4), the ringing precursor
	//  S3 (slopeX, slopeY, meanNits, meanPq) least-squares plane slopes (codes / px); mean display nits and mean PQ value
	//  S4 (coverage, offShare, visibility, N) picture share of the block (inside the frame); off-edge pixel share;
	//                                      visibility of one code step at the mean level after tone mapping; the split's
	//                                      grain level N (copied), -1 = no split
	//  A  (overlay, camConf, camCode, 0)   the aux texel (PackAux)
	enum StatIndex
	{
		kMeanY = 0, kMinY, kMaxY, kGmax,
		kJxx, kJyy, kJxy, kLapE,
		kE1, kE2, kE3, kOffLap,
		kSlopeX, kSlopeY, kMeanNits, kMeanPq,
		kCoverage, kOffShare, kVisibilityStat, kStatN,
		kAuxOverlay, kAuxCameraConfidence, kAuxCameraCode, kAuxPad
	};

	// The tone-mapping view the visibility weight is measured under (output settings: the display's measured peak).
	struct VisibilityParams
	{
		double displayPeakNits = 1000.0; // tone-mapping target
		double kneeShare = 0.75;         // soft roll-off above kneeShare x peak
		double weber = 0.01;             // one JND = weber x (L + dark)
		double darkNits = 0.1;
	};

	// Tone-mapped luminance and its slope dL'/dL (soft roll-off to the display peak above the knee).
	template <class T>
	inline void ToneMap(T L, T peak, T knee, T& outL, T& slope)
	{
		if (L <= knee || peak <= knee) { outL = L; slope = T(1); return; }
		const T span = peak - knee;
		const T e = std::exp(-(L - knee) / span);
		outL = knee + span * (T(1) - e);
		slope = e;
	}

	// One code step at `code`, after tone mapping, in Weber JNDs, clamped 0..1.
	template <class T>
	inline T Visibility(T code, int transfer, T sdrWhite, const VisibilityParams& p)
	{
		const T L = Nits<T>(code, transfer, sdrWhite);
		const T dL = Nits<T>(code + T(0.5), transfer, sdrWhite) - Nits<T>(code - T(0.5), transfer, sdrWhite);
		const T peak = T(p.displayPeakNits);
		T Lt = T(0), slopeT = T(1);
		ToneMap<T>(L, peak, T(p.kneeShare) * peak, Lt, slopeT);
		return Clamp01(slopeT * dL / (T(p.weber) * (Lt + T(p.darkNits))));
	}

	struct StatsConstants
	{
		int w = 0, h = 0, B = 16;
		Rect picture;            // clipped (PictureOrFrame)
		int transfer = 0;
		double sdrWhite = 100.0;
		int haveSplit = 0;       // the split's block texel is bound (else E1..E3 = 0, N = -1)
		VisibilityParams vis;
	};

	// Luma sources for StatsBlock.
	struct PlaneSource
	{
		const GrainSplit::Plane* y = nullptr;
		double Y(int x, int yy) const { return y->at(x, yy); }
	};

	struct PackedLuma   // float luma as the shader samples it (the float mirror)
	{
		int w = 0;
		const float* p = nullptr;
		float Y(int x, int y) const { return p[static_cast<size_t>(y) * w + x]; }
	};

	// The block's span inside the frame and inside the picture.
	struct BlockSpan
	{
		int x0 = 0, y0 = 0, x1 = 0, y1 = 0, frameArea = 0;
		bool Empty() const { return x1 <= x0 || y1 <= y0 || frameArea <= 0; }
	};

	inline BlockSpan SpanOf(int w, int h, int B, const Rect& r, int bx, int by)
	{
		BlockSpan s;
		const int fx0 = bx * B, fy0 = by * B;
		const int fx1 = (std::min)(fx0 + B, w), fy1 = (std::min)(fy0 + B, h);
		s.x0 = (std::max)(fx0, r.left); s.y0 = (std::max)(fy0, r.top);
		s.x1 = (std::min)(fx1, r.right); s.y1 = (std::min)(fy1, r.bottom);
		s.frameArea = (fx1 - fx0) * (fy1 - fy0);
		return s;
	}

	// The split's block texel (E1, E2, E3, N) of block (bx, by): band energies over the block's picture pixels, raster
	// order. On the GPU the grain split pass writes it; this is its reference. split must be on the same raster and grid.
	template <class T>
	inline void BandTexelBlock(const GrainSplit::Split& split, const Rect& picture, int bx, int by, T out[4])
	{
		out[0] = out[1] = out[2] = T(0);
		out[3] = T(split.N[static_cast<size_t>(by) * split.bw + bx]);
		const BlockSpan s = SpanOf(split.w, split.h, split.B, picture, bx, by);
		if (s.Empty()) return;
		T e1 = T(0), e2 = T(0), e3 = T(0);
		for (int y = s.y0; y < s.y1; ++y)
			for (int x = s.x0; x < s.x1; ++x)
			{
				const T a = T(split.d1.at(x, y)), b = T(split.d2.at(x, y)), c = T(split.d3.at(x, y));
				e1 += a * a; e2 += b * b; e3 += c * c;
			}
		const T inv = T(1) / T((s.x1 - s.x0) * (s.y1 - s.y0));
		out[0] = e1 * inv; out[1] = e2 * inv; out[2] = e3 * inv;
	}

	// The statistics of block (bx, by). bands = the split's block texel (null = no split). Shader order: loop 1 (raster over
	// the block's picture pixels) accumulates every sum and the extremes; loop 2 (same raster) the off-edge Laplacian.
	// Neighbours are clamped into the picture rect.
	template <class T, class Src>
	inline void StatsBlock(const StatsConstants& k, const Src& src, const T* bands, int bx, int by, T out[kStatsChannels])
	{
		for (int i = 0; i < kStatsChannels; ++i) out[i] = T(0);
		out[kStatN] = (k.haveSplit && bands) ? bands[3] : T(-1);
		const Rect& r = k.picture;
		const BlockSpan sp = SpanOf(k.w, k.h, k.B, r, bx, by);
		if (sp.Empty()) return;
		const int x0 = sp.x0, y0 = sp.y0, x1 = sp.x1, y1 = sp.y1;
		const int nx = x1 - x0, ny = y1 - y0, n = nx * ny;
		auto Yc = [&](int x, int y) -> T {
			const int cx = x < r.left ? r.left : (x > r.right - 1 ? r.right - 1 : x);
			const int cy = y < r.top ? r.top : (y > r.bottom - 1 ? r.bottom - 1 : y);
			return T(src.Y(cx, cy));
		};
		const T cxm = T(0.5) * T(x0 + x1 - 1), cym = T(0.5) * T(y0 + y1 - 1);
		T sumY = T(0), minY = T(1e30), maxY = T(-1e30), g2max = T(0);
		T jxx = T(0), jyy = T(0), jxy = T(0), lapE = T(0);
		T sxY = T(0), syY = T(0), sumNits = T(0), sumPq = T(0);
		for (int y = y0; y < y1; ++y)
			for (int x = x0; x < x1; ++x)
			{
				const T c = Yc(x, y), l = Yc(x - 1, y), rr = Yc(x + 1, y), u = Yc(x, y - 1), d = Yc(x, y + 1);
				const T gx = T(0.5) * (rr - l), gy = T(0.5) * (d - u);
				const T g2 = gx * gx + gy * gy;
				const T lap = T(4) * c - l - rr - u - d;
				sumY += c;
				minY = (std::min)((std::min)((std::min)(minY, c), (std::min)(l, rr)), (std::min)(u, d));
				maxY = (std::max)((std::max)((std::max)(maxY, c), (std::max)(l, rr)), (std::max)(u, d));
				g2max = (std::max)(g2max, g2);
				jxx += gx * gx; jyy += gy * gy; jxy += gx * gy;
				lapE += lap * lap;
				sxY += (T(x) - cxm) * c;
				syY += (T(y) - cym) * c;
				const T nits = Nits<T>(c, k.transfer, T(k.sdrWhite));
				sumNits += nits;
				sumPq += PqOf<T>(nits);
			}
		const T thr2 = T(0.0625) * g2max;   // |grad| < gmax / 4
		T offLap = T(0);
		int offCount = 0;
		for (int y = y0; y < y1; ++y)
			for (int x = x0; x < x1; ++x)
			{
				const T c = Yc(x, y), l = Yc(x - 1, y), rr = Yc(x + 1, y), u = Yc(x, y - 1), d = Yc(x, y + 1);
				const T gx = T(0.5) * (rr - l), gy = T(0.5) * (d - u);
				if (gx * gx + gy * gy < thr2)
				{
					const T lap = T(4) * c - l - rr - u - d;
					offLap += lap * lap;
					++offCount;
				}
			}
		const T inv = T(1) / T(n);
		out[kMeanY] = sumY * inv; out[kMinY] = minY; out[kMaxY] = maxY; out[kGmax] = std::sqrt(g2max);
		out[kJxx] = jxx * inv; out[kJyy] = jyy * inv; out[kJxy] = jxy * inv; out[kLapE] = lapE * inv;
		if (k.haveSplit && bands) { out[kE1] = bands[0]; out[kE2] = bands[1]; out[kE3] = bands[2]; }
		out[kOffLap] = offCount > 0 ? offLap / T(offCount) : T(0);
		// Plane fit on a rectangle: sum (x - cx)^2 = ny * nx (nx^2 - 1) / 12 (x and y are uncorrelated).
		const T sxx = T(ny) * T(nx) * (T(nx) * T(nx) - T(1)) / T(12);
		const T syy = T(nx) * T(ny) * (T(ny) * T(ny) - T(1)) / T(12);
		out[kSlopeX] = nx > 1 ? sxY / sxx : T(0);
		out[kSlopeY] = ny > 1 ? syY / syy : T(0);
		out[kMeanNits] = sumNits * inv; out[kMeanPq] = sumPq * inv;
		out[kCoverage] = T(n) / T(sp.frameArea);
		out[kOffShare] = T(offCount) * inv;
		out[kVisibilityStat] = Visibility<T>(out[kMeanY], k.transfer, T(k.sdrWhite), k.vis);
	}

	// ---- the aux texel (packed on the CPU from data computed elsewhere) -----------------------------------------------

	// Optional text-guard grid (1 = text / overlay tile), anchored at the frame's top-left, cell px each way.
	struct MaskGrid
	{
		int w = 0, h = 0;
		double cellPx = 0;
		std::vector<uint8_t> v;
		bool At(double x, double y) const
		{
			if (w <= 0 || h <= 0 || !(cellPx > 0)) return false;
			const int cx = static_cast<int>(std::floor(x / cellPx)), cy = static_cast<int>(std::floor(y / cellPx));
			if (cx < 0 || cy < 0 || cx >= w || cy >= h) return false;
			const size_t i = static_cast<size_t>(cy) * w + cx;
			return i < v.size() && v[i] != 0;
		}
	};

	struct Externals
	{
		Rect picture;                                   // trusted active picture (empty = whole raster)
		std::vector<Rect> overlays;                     // subtitle / overlay boxes (source raster)
		const MaskGrid* textGuard = nullptr;            // optional
		const CameraModel::CameraMotion* camera = nullptr;   // optional; the record of the pair ending at this frame
		int transfer = static_cast<int>(Transfer::SdrGamma);
		double sdrWhiteNits = 100.0;
	};

	struct CameraStatus { bool record = false, labels = false; };

	// aux texel per block: (overlay 0/1, camera confidence, camera code = label + 4 * tinyMover, 0).
	inline void PackAux(const Grid& g, const Externals& ex, std::vector<float>& aux, CameraStatus* camStatus = nullptr)
	{
		aux.assign(g.Count() * 4, 0.0f);
		const CameraModel::CameraMotion* cam = ex.camera;
		const bool camValid = cam != nullptr && cam->valid;
		const CameraModel::BlockData* cb = camValid ? cam->blocks.get() : nullptr;
		const bool labels = cb != nullptr && !cam->labelsOnGpu && cb->gridW > 0 && cb->gridH > 0 && cb->blockSize > 0 &&
			cb->label.size() >= static_cast<size_t>(cb->gridW) * cb->gridH;
		if (camStatus) { camStatus->record = camValid; camStatus->labels = labels; }
		for (int by = 0; by < g.gh; ++by)
			for (int bx = 0; bx < g.gw; ++bx)
			{
				const size_t i = static_cast<size_t>(by) * g.gw + bx;
				const int px0 = bx * g.B, py0 = by * g.B;
				const int px1 = (std::min)(px0 + g.B, g.w), py1 = (std::min)(py0 + g.B, g.h);
				const double cx = 0.5 * (px0 + px1), cy = 0.5 * (py0 + py1);
				bool overlay = false;
				for (const Rect& o : ex.overlays)
					if (!o.Empty() && o.left < px1 && o.right > px0 && o.top < py1 && o.bottom > py0) { overlay = true; break; }
				if (!overlay && ex.textGuard) overlay = ex.textGuard->At(cx, cy);
				float conf = 0.0f, code = 0.0f;
				if (camValid)
				{
					conf = static_cast<float>(Clamp01(cam->confidence));
					if (labels)
					{
						const int lx = static_cast<int>(std::floor(cx / cb->blockSize)), ly = static_cast<int>(std::floor(cy / cb->blockSize));
						if (lx >= 0 && ly >= 0 && lx < cb->gridW && ly < cb->gridH)
						{
							const size_t j = static_cast<size_t>(ly) * cb->gridW + lx;
							const int label = cb->label[j] & 3;
							const int tiny = (j < cb->tinyMover.size() && cb->tinyMover[j]) ? 1 : 0;
							code = static_cast<float>(label + 4 * tiny);
							if (label == static_cast<int>(CameraModel::Label::Overlay)) overlay = true;   // static logo
						}
					}
				}
				aux[i * 4 + 0] = overlay ? 1.0f : 0.0f;
				aux[i * 4 + 1] = conf;
				aux[i * 4 + 2] = code;
				aux[i * 4 + 3] = 0.0f;
			}
	}

	// ---- layer 2: the thin maps (derived on the fly) --------------------------------------------------------------------

	constexpr int kMapTextures = 3;
	constexpr int kMapChannels = kMapTextures * 4;

	//  M0 (grain, edge, edgeContrast, ringing)
	//  M1 (banding, texture, brightness, visibility)
	//  M2 (flags, cameraConfidence, cameraCode, 0)
	enum MapIndex
	{
		kGrain = 0, kEdge, kEdgeContrast, kRinging,
		kBanding, kTexture, kBrightness, kVisibility,
		kFlags, kCameraConfidence, kCameraCode, kMapPad
	};

	// The derive law's constants (documented; tuned on the synthetic cases, to be confirmed on real clips).
	struct DeriveParams
	{
		double lapGrain = 20.0;          // Laplacian energy of white grain = 20 x its variance (5-point stencil)
		double grainSpan = 6.0;          // grain peak-to-peak in sigmas (edge contrast)
		double edgeGateLo = 4.0, edgeGateHi = 8.0;   // ringing needs an edge above this many grain sigmas (+ offsets)
		double edgeGateOffLo = 2.0, edgeGateOffHi = 4.0;
		double ringFloor = 1.0;          // codes^2 (at least 1e-6 is used)
		double flatLo = 1.0, flatHi = 12.0;          // texture energy (codes^2) where banding-prone ends
		double slopeLo = 1.0, slopeHi = 4.0;         // plane slope (codes / px) where a gradient stops banding
	};

	// One block's maps from its precursors (stats + aux texel). Pure ALU (sqrt only): consumers run it inside their own
	// pass. Same order in the shader (RegionEvidenceGlsl.h region_evidence()).
	template <class T>
	inline void DeriveBlock(const DeriveParams& p, const T s[kPrecursorChannels], T m[kMapChannels])
	{
		for (int i = 0; i < kMapChannels; ++i) m[i] = T(0);
		const bool haveSplit = s[kStatN] >= T(0);
		const bool overlay = s[kAuxOverlay] > T(0.5);
		const bool inPicture = s[kCoverage] >= T(0.5);
		uint32_t flags = (overlay ? kOverlay : 0u) | (haveSplit ? 0u : kNoGrain);
		if (!inPicture)
		{
			m[kFlags] = T(static_cast<int>(flags));
			return;
		}
		flags |= kInPicture;
		const T N = haveSplit ? s[kStatN] : T(0);
		const T sigma = std::sqrt(N);
		const T edge = s[kGmax];
		const T range = s[kMaxY] - s[kMinY];
		m[kGrain] = sigma;
		m[kEdge] = edge;
		m[kEdgeContrast] = range / (range + T(p.grainSpan) * sigma + T(1));
		const T floorLap = T(p.lapGrain) * N;
		const T gate = Ramp(edge, T(p.edgeGateLo) * sigma + T(p.edgeGateOffLo), T(p.edgeGateHi) * sigma + T(p.edgeGateOffHi));
		const T excess = (std::max)(s[kOffLap] - floorLap, T(0));
		m[kRinging] = haveSplit ? gate * excess / (s[kOffLap] + floorLap + (std::max)(T(p.ringFloor), T(1e-6))) : T(0);
		const T texture = haveSplit ? (std::max)(s[kE1] + s[kE2] - N, T(0)) : T(0);
		m[kTexture] = texture;
		const T slope = std::sqrt(s[kSlopeX] * s[kSlopeX] + s[kSlopeY] * s[kSlopeY]);
		m[kBanding] = (haveSplit && !overlay) ? (T(1) - Ramp(texture, T(p.flatLo), T(p.flatHi))) * (T(1) - Ramp(slope, T(p.slopeLo), T(p.slopeHi))) : T(0);
		m[kBrightness] = s[kMeanY];
		m[kVisibility] = s[kVisibilityStat];
		const int code = static_cast<int>(s[kAuxCameraCode] + T(0.5));
		if ((code >> 2) & 1) flags |= kTinyMover;
		m[kFlags] = T(static_cast<int>(flags));
		m[kCameraConfidence] = s[kAuxCameraConfidence];
		m[kCameraCode] = T(code & 3);
	}

	// ---- the shared grain product (coordinator 2026-10-31: owned here, one product for every grain reader) -------------

	// The grain-free picture and the grain layer of one decoded source frame, computed once, early, on the source (picture
	// extended by mirroring: bars never enter it). The law is repair.grain_split's, reused unchanged. Readers: the camera
	// model's grain-map consumer, TemporalDetail, the chroma-from-luma grain guard, the dynamic grain-safe contrast recovery,
	// the region block stats (and through them every derived map). Nobody recomputes it.
	// The planes of the product's GPU form (luma for every reader; Cb/Cr for the chroma-from-luma grain guard).
	enum class GrainPlane : int { Luma = 0, Cb = 1, Cr = 2, Count = 3 };

	struct GrainProduct
	{
		GrainSplit::Split split;         // CPU form: base, D1/D2/D3, f, s1/s2, clean, grain, N (block grid = the region grid)
		Rect picture;                    // clipped picture the split was made for
		VpAncestry::Header header;       // stamped by the producer (StampGrain)
		// GPU form: the same law as GrainSplit::Gpu::Splitter passes (GrainSplitGpu.h), one Frame per plane, borrowed from
		// the producer and valid until its next Compute. Null = not made on the GPU for that plane. A reader checks
		// GrainSplit::Gpu::Fits against its own plane and falls back visibly (its own Splitter, named in its status).
		const GrainSplit::Gpu::Frame* gpu[static_cast<int>(GrainPlane::Count)] = { nullptr, nullptr, nullptr };
		bool Valid() const { return split.w > 0 && split.bw > 0 && split.N.size() >= static_cast<size_t>(split.bw) * split.bh; }
		bool HasGpu() const { return gpu[0] != nullptr || gpu[1] != nullptr || gpu[2] != nullptr; }
	};

	inline GrainProduct ComputeGrain(const GrainSplit::Plane& luma, const Rect& picture, int B)
	{
		GrainProduct g;
		g.picture = PictureOrFrame(picture, luma.w, luma.h);
		g.split = GrainSplit::Compute(ExtendPicture(luma, g.picture), B / 16.0, B);
		return g;
	}

	inline void StampGrain(GrainProduct& g, uint64_t seq, uint32_t cgen, VpAncestry::Provenance prov, VpIndicator::CounterMask reads,
		const VpAncestry::AncestorRef* ancestors, size_t count)
	{
		g.header.seq = seq;
		g.header.cgen = cgen;
		g.header.prov = prov;
		g.header.reads = reads;
		g.header.ancestorCount = static_cast<uint8_t>((std::min)(count, VpAncestry::kMaxAncestors));
		for (size_t i = 0; i < g.header.ancestorCount; ++i) g.header.ancestors[i] = ancestors[i];
	}

	// The one read API of the grain product (CPU; the GPU form binds the split's textures: base R16F half, s1/s2 RG16F,
	// f R8, block texel rgba32f (E1, E2, E3, N)). Reads outside the frame clamp. An invalid product reads 0: the caller
	// runs without its grain-free path and shows the product's absence in its status (no silent fallback).
	class GrainView
	{
	public:
		GrainView() = default;
		explicit GrainView(const GrainProduct* g)
			: m_g(g && (g->Valid() || g->HasGpu()) ? g : nullptr), m_cpu(g && g->Valid() ? g : nullptr) {}
		bool Valid() const { return m_g != nullptr; }        // either form present
		bool CpuValid() const { return m_cpu != nullptr; }   // the CPU form (the per-pixel reads below)
		// The GPU form of one plane (GrainSplit::Gpu::Frame), or null: GPU readers (temporal_detail, CfL) bind its textures.
		const GrainSplit::Gpu::Frame* Gpu(GrainPlane p = GrainPlane::Luma) const
		{
			const int i = static_cast<int>(p);
			return m_g && i >= 0 && i < static_cast<int>(GrainPlane::Count) ? m_g->gpu[i] : nullptr;
		}
		double Clean(int x, int y) const { return m_cpu ? m_cpu->split.clean.get(x, y) : 0.0; }   // grain-free picture
		double Grain(int x, int y) const { return m_cpu ? m_cpu->split.grain.get(x, y) : 0.0; }   // Y - clean
		double Share(int x, int y) const { return m_cpu ? m_cpu->split.f.get(x, y) : 0.0; }       // Wiener share f
		double Level(int bx, int by) const                                                          // block N (codes^2)
		{
			if (!m_cpu) return 0.0;
			const int x = (std::max)(0, (std::min)(m_cpu->split.bw - 1, bx)), y = (std::max)(0, (std::min)(m_cpu->split.bh - 1, by));
			return m_cpu->split.N[static_cast<size_t>(y) * m_cpu->split.bw + x];
		}
		double LevelAtPixel(int x, int y) const { return m_cpu ? Level(x / m_cpu->split.B, y / m_cpu->split.B) : 0.0; }
		int BlockSize() const { return m_cpu ? m_cpu->split.B : 0; }
		const GrainSplit::Split* Split() const { return m_cpu ? &m_cpu->split : nullptr; }

	private:
		const GrainProduct* m_g = nullptr;     // either form
		const GrainProduct* m_cpu = nullptr;   // the CPU form valid
	};

	// ---- the stored product of one frame: the precursors ---------------------------------------------------------------

	enum class SplitState : uint8_t { Present, Missing, Mismatched };

	struct Precursors
	{
		Grid grid;
		Rect picture;                    // clipped
		SplitState split = SplitState::Missing;
		CameraStatus camera;
		std::vector<double> data;        // Count() x kPrecursorChannels: S0..S4 then the aux texel
		VpAncestry::Header header;       // stamped by the producer (lane or in-render), see Stamp()

		bool Valid() const { return grid.gw > 0 && data.size() == grid.Count() * kPrecursorChannels; }
		bool HaveSplit() const { return split == SplitState::Present; }
		const double* Block(int bx, int by) const { return &data[(static_cast<size_t>(by) * grid.gw + bx) * kPrecursorChannels]; }
	};

	// The status line a consumer shows when the evidence is reduced (no silent fallbacks); every reduction is named.
	// "" = complete.
	inline std::string StatusOf(const Precursors& m)
	{
		if (!m.Valid()) return "Region evidence: unavailable (not computed for this frame)";
		std::string why;
		auto add = [&why](const char* s) { why += why.empty() ? s : (std::string("; ") + s); };
		if (m.split == SplitState::Missing) add("no grain split: grain, texture, ringing and banding unknown");
		if (m.split == SplitState::Mismatched) add("grain split of another size or grid: grain, texture, ringing and banding unknown");
		if (!m.camera.record) add("no camera record: camera confidence 0");
		else if (!m.camera.labels) add("camera labels only on the GPU: CPU labels unknown");
		return why.empty() ? std::string() : "Region evidence: reduced (" + why + ")";
	}

	struct FrameParams
	{
		int blockOverride = 0;           // 0 = BlockSizeFor(height)
		VisibilityParams vis;
	};

	inline StatsConstants MakeStatsConstants(int w, int h, int B, const Rect& picture, const Externals& ex, bool haveSplit,
		const VisibilityParams& vis)
	{
		StatsConstants sk;
		sk.w = w; sk.h = h; sk.B = B; sk.picture = picture;
		sk.transfer = ex.transfer; sk.sdrWhite = ex.sdrWhiteNits; sk.haveSplit = haveSplit ? 1 : 0;
		sk.vis = vis;
		return sk;
	}

	// Whether `split` belongs to this frame and grid (same raster, same block size, complete N).
	inline bool SplitMatches(const GrainSplit::Split& s, const Grid& g)
	{
		return s.w == g.w && s.h == g.h && s.B == g.B && s.bw == g.gw && s.bh == g.gh && s.N.size() >= g.Count() &&
			s.d1.v.size() == static_cast<size_t>(g.w) * g.h && s.d2.v.size() == s.d1.v.size() && s.d3.v.size() == s.d1.v.size();
	}

	// The CPU reference of one frame's precursors (double). `split` must be the shared split of the picture-extended (mirrored) luma
	// on the same grid: GrainSplit::Compute(ExtendPicture(luma, picture), B / 16.0, B). Null or mismatched = reduced and
	// named in StatusOf.
	inline Precursors Compute(const GrainSplit::Plane& luma, const GrainSplit::Split* split, const Externals& ex,
		const FrameParams& fp = FrameParams())
	{
		Precursors out;
		const int B = fp.blockOverride > 0 ? fp.blockOverride : BlockSizeFor(luma.h);
		out.grid = MakeGrid(luma.w, luma.h, B);
		out.picture = PictureOrFrame(ex.picture, luma.w, luma.h);
		out.split = split == nullptr ? SplitState::Missing : (SplitMatches(*split, out.grid) ? SplitState::Present : SplitState::Mismatched);
		const bool have = out.HaveSplit();
		const StatsConstants sk = MakeStatsConstants(luma.w, luma.h, B, out.picture, ex, have, fp.vis);
		PlaneSource src;
		src.y = &luma;
		std::vector<float> aux;
		PackAux(out.grid, ex, aux, &out.camera);
		const size_t n = out.grid.Count();
		out.data.assign(n * kPrecursorChannels, 0.0);
		for (int by = 0; by < out.grid.gh; ++by)
			for (int bx = 0; bx < out.grid.gw; ++bx)
			{
				const size_t i = static_cast<size_t>(by) * out.grid.gw + bx;
				double* s = &out.data[i * kPrecursorChannels];
				double bands[4] = { 0, 0, 0, 0 };
				if (have) BandTexelBlock<double>(*split, out.picture, bx, by, bands);
				StatsBlock<double>(sk, src, have ? bands : nullptr, bx, by, s);
				for (int c = 0; c < 4; ++c) s[kAuxIndex + c] = aux[i * 4 + c];
			}
		return out;
	}

	// The whole CPU path of one frame (the lane's CPU twin and the tests): clamp, split once, precursors.
	inline Precursors ComputeFrame(const GrainSplit::Plane& luma, const Externals& ex, const FrameParams& fp = FrameParams(),
		GrainSplit::Split* splitOut = nullptr)
	{
		const int B = fp.blockOverride > 0 ? fp.blockOverride : BlockSizeFor(luma.h);
		GrainProduct g = ComputeGrain(luma, ex.picture, B);
		Precursors p = Compute(luma, &g.split, ex, fp);
		if (splitOut) *splitOut = std::move(g.split);
		return p;
	}

	// Ancestry stamp (the slot protocol fills cgen/stamp; this records what the precursors were made from).
	inline void Stamp(Precursors& m, uint64_t seq, uint32_t cgen, VpAncestry::Provenance prov, VpIndicator::CounterMask reads,
		const VpAncestry::AncestorRef* ancestors, size_t count)
	{
		m.header.seq = seq;
		m.header.cgen = cgen;
		m.header.prov = prov;
		m.header.reads = reads;
		m.header.ancestorCount = static_cast<uint8_t>((std::min)(count, VpAncestry::kMaxAncestors));
		for (size_t i = 0; i < m.header.ancestorCount; ++i) m.header.ancestors[i] = ancestors[i];
	}

	// ---- the single accessor (CPU; RegionEvidenceGlsl.h region_evidence() is the shader mirror) -----------------------

	struct Sample
	{
		bool valid = false;              // false = out of the grid or no precursors: neutral, act as without evidence
		float grain = 0, edge = 0, edgeContrast = 0, ringing = 0;
		float banding = 0, texture = 0, brightness = 0, visibility = 0;
		uint32_t flags = 0;
		float cameraConfidence = 0;
		int cameraLabel = 0;             // CameraModel::Label
		bool InPicture() const { return (flags & kInPicture) != 0; }
		bool Overlay() const { return (flags & kOverlay) != 0; }
	};

	inline Sample ToSample(const double m[kMapChannels])
	{
		Sample s;
		s.valid = true;
		s.grain = static_cast<float>(m[kGrain]); s.edge = static_cast<float>(m[kEdge]);
		s.edgeContrast = static_cast<float>(m[kEdgeContrast]); s.ringing = static_cast<float>(m[kRinging]);
		s.banding = static_cast<float>(m[kBanding]); s.texture = static_cast<float>(m[kTexture]);
		s.brightness = static_cast<float>(m[kBrightness]); s.visibility = static_cast<float>(m[kVisibility]);
		s.flags = static_cast<uint32_t>(m[kFlags] + 0.5);
		s.cameraConfidence = static_cast<float>(m[kCameraConfidence]);
		s.cameraLabel = static_cast<int>(m[kCameraCode] + 0.5);
		return s;
	}

	// Derives on read (nothing per consumer is stored). A consumer owns its DeriveParams (defaults = the shared law).
	class View
	{
	public:
		View() = default;
		explicit View(const Precursors* p, const DeriveParams& params = DeriveParams())
			: m_p(p && p->Valid() ? p : nullptr), m_params(params) {}
		bool Valid() const { return m_p != nullptr; }
		int BlockSize() const { return m_p ? m_p->grid.B : 0; }
		std::string Status() const { return m_p ? StatusOf(*m_p) : std::string("Region evidence: unavailable (not computed for this frame)"); }

		Sample At(int bx, int by) const
		{
			if (!m_p || bx < 0 || by < 0 || bx >= m_p->grid.gw || by >= m_p->grid.gh) return Sample();
			double m[kMapChannels];
			DeriveBlock<double>(m_params, m_p->Block(bx, by), m);
			return ToSample(m);
		}

		// Source-raster pixel -> its block (floor(px / B)).
		Sample AtPixel(double x, double y) const
		{
			if (!m_p) return Sample();
			const double B = m_p->grid.B;
			return At(static_cast<int>(std::floor(x / B)), static_cast<int>(std::floor(y / B)));
		}

		// Layer-1 precursor of a block (components that make their own decision).
		double Stat(int bx, int by, StatIndex index) const
		{
			if (!m_p || bx < 0 || by < 0 || bx >= m_p->grid.gw || by >= m_p->grid.gh) return 0.0;
			return m_p->Block(bx, by)[index];
		}

	private:
		const Precursors* m_p = nullptr;
		DeriveParams m_params;
	};

	// Temporal smoothing a consumer applies to a value it turns into a strength (owner: smoothed over time). One-pole,
	// reset at a discontinuity (the consumer passes the frame analysis epoch change). Shader mirror: region_smooth().
	inline float Smooth(float previous, float current, float alpha, bool discontinuity)
	{
		if (discontinuity) return current;
		return previous + alpha * (current - previous);
	}

	// ---- skeleton products (design doc 25 B1) and cost -----------------------------------------------------------------

	struct ProductIds
	{
		VpSkeleton::ProductId grainSplit = VpSkeleton::kNoProduct;
		VpSkeleton::ProductId blockStats = VpSkeleton::kNoProduct;
	};

	// The grain product's declared readers as skeleton consumers (ids firstId .. firstId + 3, assigned at integration):
	// camera-model grain map, TemporalDetail, chroma-from-luma grain guard, dynamic grain-safe contrast recovery. The
	// graph keeps ONE grain_split node however many of them are active.
	inline std::vector<VpSkeleton::ConsumerDecl> GrainConsumers(const ProductIds& ids, uint16_t firstId)
	{
		static const char* const kNames[4] = { "camera model grain map", "temporal detail", "chroma-from-luma grain guard",
			"grain-safe contrast recovery" };
		std::vector<VpSkeleton::ConsumerDecl> out;
		for (uint16_t i = 0; i < 4; ++i)
		{
			VpSkeleton::ConsumerDecl c;
			c.id = static_cast<uint16_t>(firstId + i);
			c.name = kNames[i];
			VpSkeleton::AddConsume(c, ids.grainSplit, 0);
			out.push_back(c);
		}
		return out;
	}

	// Per-frame buffer bytes and reference-card render estimates (3050 Ti laptop = computeScale 1.0, at 1080p).
	struct Cost
	{
		int B = 16;
		size_t blocks = 0;
		uint64_t grainSplitBytes = 0;    // base half R16F (0.5 B/px), s1/s2 RG16F (4 B/px), f R8 (1 B/px), block texel 16 B
		uint64_t blockStatsBytes = 0;    // 6 x rgba32f per block (5 stats + aux)
		uint64_t derivedMapBytes = 0;    // what ONE stored derived map set would cost (3 x rgba32f per block): not stored
		double refMsSplit = 0, refMsStats = 0;
		double deriveOpsPerEval = 0;     // ALU per on-the-fly derivation (+ 6 texel fetches)
	};

	inline Cost CostAt(VpSkeleton::Dims d)
	{
		Cost c;
		c.B = BlockSizeFor(static_cast<int>(d.height));
		const Grid g = MakeGrid(static_cast<int>(d.width), static_cast<int>(d.height), c.B);
		c.blocks = g.Count();
		c.grainSplitBytes = VpSkeleton::CellsAt(d, 0) * 5 + VpSkeleton::CellsAt(d, 1) * 2 + c.blocks * 16;
		c.blockStatsBytes = static_cast<uint64_t>(c.blocks) * kPrecursorTextures * 16;
		c.derivedMapBytes = static_cast<uint64_t>(c.blocks) * kMapTextures * 16;
		c.refMsSplit = 1.2;     // three separable Gaussians + 3x3 energy + block reductions (grain_split README)
		c.refMsStats = 0.35;    // one read of the resident luma (5-point stencil, cache-resident), ~60 ALU and up to
		                        // 4 pow per pixel (PQ), two passes over each block
		c.deriveOpsPerEval = 30;
		return c;
	}

	// Appends the two stored precursor products with the next dense ids. Both read the decoded source frame before any
	// picture change, and P4 (source stats: the active-picture rect, cut/fade) at lag 0; block stats also read the split.
	// missingInputForm on block stats: without the split (or with a mismatched one) it is still made, with N = -1 and
	// E = 0 (kNoGrain, named in StatusOf). The camera record rides FrameAnalysisStore kCamera (not a skeleton product);
	// without it the aux texel carries confidence 0 (named in StatusOf). The visibility term reads the output settings
	// (display peak). Both are T0: every regional repair depends on them.
	inline ProductIds AppendProducts(std::vector<VpSkeleton::ProductDecl>& decls, VpSkeleton::Dims frame)
	{
		using namespace VpSkeleton;
		using namespace VpIndicator;
		const CounterMask content = Bit(kFull) | Bit(kContinuity);
		const Cost c = CostAt(frame);
		ProductIds ids;

		// SA2: when the registry already carries the core P10 (SkeletonProducts), that ONE declaration is the grain
		// split node; the private one below is only the standalone form (a registry built without it).
		if (decls.size() > VpSkeleton::kP10GrainSplit && std::string(decls[VpSkeleton::kP10GrainSplit].token) == kTokenGrainSplit)
		{
			ids.grainSplit = VpSkeleton::kP10GrainSplit;
		}
		else
		{
			ProductDecl gs = detail::Make(static_cast<ProductId>(decls.size()), kTokenGrainSplit, "grain split (shared)",
				Backend::GpuCompute, 1, Tier::T0, content, Bit(kSettingsAnalysis));
			gs.sourceFrame = true;
			detail::AddInput(gs, kP4SourceStats, 0);
			detail::AddTerm(gs, 0, 5);
			detail::AddTerm(gs, 1, 2);
			gs.size.fixedBytes = static_cast<uint32_t>(c.blocks * 16);
			gs.refRenderMs = static_cast<float>(c.refMsSplit);
			ids.grainSplit = gs.id;
			decls.push_back(gs);
		}

		ProductDecl bs = detail::Make(static_cast<ProductId>(decls.size()), "region_block_stats", "region block statistics",
			Backend::GpuCompute, 1, Tier::T0, content, Bit(kSettingsAnalysis) | Bit(kSettingsMotion) | Bit(kSettingsOutput));
		bs.sourceFrame = true;
		detail::AddInput(bs, ids.grainSplit, 0);
		detail::AddInput(bs, kP4SourceStats, 0);
		bs.size.fixedBytes = static_cast<uint32_t>(c.blockStatsBytes);
		bs.missingInputForm = true;
		bs.refRenderMs = static_cast<float>(c.refMsStats);
		ids.blockStats = bs.id;
		decls.push_back(bs);
		return ids;
	}
}
