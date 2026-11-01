// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Light-level block product P12 (`input.light_level`, WP2, unit U9): the CPU reference. One rgba texel per region block
// (the P11 grid, B = RegionEvidence::BlockSizeFor): (J_surround, J_local, V, conf).
//
//   J_local     the mean J (PQ of the luminance the viewer sees) over the block's PICTURE pixels. P11's stats pass takes it
//               from the light-level LUT (StatsConstants::lutTexels -> kMeanPq), so the stats and this product agree. A block
//               outside the picture carries J_surround (never bar data).
//   J_surround  the adaptation level: a Gaussian over J_local, weighted by picture COVERAGE (kCoverage), so letterbox bars,
//               pillar bars and any bar level never enter it: a block with no picture has weight 0, the window of a picture
//               block is normalised by the picture weight inside it. Sigma comes from the geometry (1 degree of visual
//               angle from the picture height and the viewing distance; fallback picture height / 40), in blocks, so the
//               kernel covers the same share of the picture at 1080p, 4K and 8K. Then a one-pole on J_surround
//               (tau 0.3 s, GeneralRate::PerFrameWeight) that RESETS on a cut, resync, re-base, geometry or context change.
//   V           the LUT visibility (Weber units) of one code step at the block's mean code, capped at kVCap (storage as half
//               floats); 0 outside the picture. Unclamped, unlike P11's kVisibilityStat (clamped 0..1 for its readers).
//   conf        1 inside the picture with a grain split (P11 N >= 0), else 0; scaled by the frame gate (0 when the stats were
//               made without this LUT: a context mismatch). Consumers with conf 0 go light-neutral and the status says why.
//
// Fill: the lane precomputes it after P11 (one pass), or the first consumer fills it live into the same slot with the SAME
// pass (LightLevelBlocksGlsl.h). The previous frame's slot is the one-pole state (no private history). Fallbacks, all visible
// in Status (see Mode): no P11 -> frame level from P4's APL; no P4 either -> code only (LUT per pixel, conf 0); no LUT/context
// -> neutral (light term 1).
//
// Determinism: fixed loop order (rows, then columns), no threads. The templated block functions are the float mirror of the
// GLSL (same arithmetic order); the double instantiation is the reference. Header-only, C++14.
#pragma once

#include "LightLevelLut.h"

#include <VP-V/core/Ancestry.h>
#include <VP-V/modules/input/source_analysis/regions/RegionEvidence.h>

#include <GeneralRate.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace LightLevel
{
	namespace Blocks
	{
		enum Channel { kJSurround = 0, kJLocal = 1, kV = 2, kConf = 3 };
		constexpr int kChannels = 4;
		constexpr int kBytesPerBlock = 16;         // rgba32f in the slot: the one-pole reads J_surround back, a half float would stall it (core/Products.h)
		constexpr double kVCap = 64.0;             // half-float storage and a sane consumer range
		constexpr double kSupportEpsilon = 1e-6;   // picture weight below which the surround falls back to the frame level

		// What the pass does this frame; the status text names every reduction (no silent fallbacks).
		enum class Mode : uint8_t
		{
			Full,         // per-block J_local / J_surround / V / conf from P11 and the LUT
			FrameLevel,   // no usable P11: one level for the whole frame from P4's APL, conf = Params::frameLevelConf
			CodeOnly,     // no P11 and no APL: no block data (conf 0); consumers use the LUT per pixel only
			Neutral       // no LUT / no display context: the light term is 1 (consumers ignore the product)
		};

		enum class Reset : uint8_t { None, First, Cut, Resync, Rebase, Geometry, Context, NoHistory };

		inline const char* Token(Reset r)
		{
			switch (r)
			{
			case Reset::None: return "none";
			case Reset::First: return "first frame";
			case Reset::Cut: return "cut";
			case Reset::Resync: return "resync";
			case Reset::Rebase: return "re-base";
			case Reset::Geometry: return "geometry change";
			case Reset::Context: return "context change";
			case Reset::NoHistory: return "no full previous frame";
			}
			return "?";
		}

		// Tunables. The kernel and time constant are provisional (tuned in WP9); nothing is a resolution or a rate.
		struct Params
		{
			int w = 0, h = 0, B = 16;                    // frame raster and region block (RegionEvidence grid)
			RegionEvidence::Rect picture;                // trusted active picture (empty = whole raster)
			double viewingDistancePictureHeights = 0.0;  // display settings; 0 = unknown -> the fallback sigma
			double sigmaDegrees = 1.0;                   // visual angle of the surround sigma
			double fallbackSigmaDivisor = 40.0;          // unknown viewing distance: sigma = picture height / this
			double kernelRadiusSigmas = 3.0;
			int maxRadiusBlocks = 32;                    // window cap; reaching it is reported
			double tauSeconds = 0.3;                     // one-pole time constant (per-frame weight via GeneralRate)
			double frameRateHz = 0.0;                    // measured; 0 = unknown (the 60 Hz reference weight)
			double surroundMix = 0.5;                    // J_eff = mix(J_p, J_s, surroundMix * conf), tuned by the eval
			double frameLevelConf = 0.0;                 // conf of the frame-level fallback (design: 0 without stats; consumers go light-neutral)
		};

		struct Kernel
		{
			double sigmaBlocks = 1.0;
			int radius = 1;
			bool capped = false;
			bool fromViewing = false;
		};

		inline Params ParamsFor(int w, int h, const RegionEvidence::Rect& picture, int blockOverride = 0)
		{
			Params p;
			p.w = w; p.h = h;
			p.B = blockOverride > 0 ? blockOverride : RegionEvidence::BlockSizeFor(h);
			p.picture = picture;
			return p;
		}

		inline Kernel KernelFor(const Params& p)
		{
			Kernel k;
			const RegionEvidence::Rect pic = RegionEvidence::PictureOrFrame(p.picture, p.w, p.h);
			const double picH = static_cast<double>(pic.bottom - pic.top);
			double sigmaPx;
			if (p.viewingDistancePictureHeights > 0.0)
			{
				sigmaPx = std::tan(p.sigmaDegrees * 3.14159265358979323846 / 180.0) * p.viewingDistancePictureHeights * picH;
				k.fromViewing = true;
			}
			else
				sigmaPx = picH / p.fallbackSigmaDivisor;
			k.sigmaBlocks = std::max(sigmaPx / static_cast<double>(p.B), 0.25);
			int radius = static_cast<int>(std::ceil(p.kernelRadiusSigmas * k.sigmaBlocks));
			if (radius > p.maxRadiusBlocks) { radius = p.maxRadiusBlocks; k.capped = true; }
			k.radius = std::max(radius, 1);
			return k;
		}

		// The one-pole weight of one frame: tau 0.3 s at the 60 Hz reference, held in time above it (the repo's rate rule).
		inline double TemporalAlpha(const Params& p)
		{
			const double alpha60 = 1.0 - std::exp(-1.0 / (p.tauSeconds * GeneralRate::kFrameCountReferenceHz));
			return GeneralRate::PerFrameWeight(alpha60, p.frameRateHz);
		}

		// ---- the per-block law (T = double reference, T = float GLSL mirror) ----------------------------------------------

		// stats = Count() blocks of `stride` values each, the first kStatsChannels being P11's S0..S4 (stride kStatsChannels for the
		// GPU mirror's stats grid, kPrecursorChannels for Precursors::data), lut = PackTexels, prev = Count() x 4 values of
		// the previous frame's slot (null or reset: no smoothing), frameJ = the coverage-weighted frame level, gate = the
		// frame confidence gate. Loop order: rows then columns, as the shader.
		template <class T>
		inline void BlockOut(const Params& p, const Kernel& k, int gw, int gh, const T* stats, size_t stride, const float* lut,
			T frameJ, T gate, const T* prev, bool reset, T alpha, int bx, int by, T out[kChannels])
		{
			using namespace RegionEvidence;
			const size_t i = static_cast<size_t>(by) * gw + bx;
			const T* own = stats + i * stride;
			const bool inPicture = own[kCoverage] >= T(0.5);
			const bool haveGrain = own[kStatN] >= T(0);
			T sw = T(0), sj = T(0);
			const int y0 = std::max(0, by - k.radius), y1 = std::min(gh - 1, by + k.radius);
			const int x0 = std::max(0, bx - k.radius), x1 = std::min(gw - 1, bx + k.radius);
			const T sigma = T(k.sigmaBlocks);
			for (int y = y0; y <= y1; ++y)
			{
				const T dy = T(y - by) / sigma;
				const T wy = std::exp(T(-0.5) * dy * dy);
				for (int x = x0; x <= x1; ++x)
				{
					const T dx = T(x - bx) / sigma;
					const T* s = stats + (static_cast<size_t>(y) * gw + x) * stride;
					const T wc = std::exp(T(-0.5) * dx * dx) * wy * s[kCoverage];
					sw += wc;
					sj += wc * s[kMeanPq];
				}
			}
			T js = sw > T(kSupportEpsilon) ? sj / sw : frameJ;
			if (!reset && prev)
			{
				const T pj = prev[i * kChannels + kJSurround];
				js = pj + alpha * (js - pj);
			}
			out[kJSurround] = js;
			out[kJLocal] = inPicture ? own[kMeanPq] : js;
			out[kV] = inPicture ? std::min(FetchPacked<T>(lut, own[kMeanY]).V, T(kVCap)) : T(0);
			out[kConf] = (inPicture && haveGrain) ? gate : T(0);
			(void)p;
		}

		// ---- consumer read (the mirror of vpu_light_jeff) -----------------------------------------------------------------

		template <class T>
		struct Eff { T jEff, vPixel, conf; };

		// J_eff at pixel (px, py) whose own low-band code is code10: J_s bilinear over the block grid (block centres), J_p
		// a LUT fetch, J_eff = J_p + (J_s - J_p) * (surroundMix * conf). conf 0 (reduced / neutral blocks): J_eff = J_p.
		template <class T>
		inline Eff<T> EffectiveAt(int gw, int gh, int B, const T* blocks, const float* lut, T px, T py, T code10, T surroundMix)
		{
			const T fx = px / T(B) - T(0.5), fy = py / T(B) - T(0.5);
			const T flx = std::floor(fx), fly = std::floor(fy);
			const int xa = static_cast<int>(flx), ya = static_cast<int>(fly);
			const T tx = fx - flx, ty = fy - fly;
			const int xs[2] = { std::min(std::max(xa, 0), gw - 1), std::min(std::max(xa + 1, 0), gw - 1) };
			const int ys[2] = { std::min(std::max(ya, 0), gh - 1), std::min(std::max(ya + 1, 0), gh - 1) };
			auto at = [&](int x, int y, int c) { return blocks[(static_cast<size_t>(y) * gw + x) * kChannels + c]; };
			auto bil = [&](int c) {
				const T top = at(xs[0], ys[0], c) + (at(xs[1], ys[0], c) - at(xs[0], ys[0], c)) * tx;
				const T bot = at(xs[0], ys[1], c) + (at(xs[1], ys[1], c) - at(xs[0], ys[1], c)) * tx;
				return top + (bot - top) * ty;
			};
			const PackedEntry<T> e = FetchPacked<T>(lut, code10);
			Eff<T> r;
			r.conf = bil(kConf);
			const T js = bil(kJSurround);
			r.jEff = e.J + (js - e.J) * (surroundMix * r.conf);
			r.vPixel = e.V;
			return r;
		}

		// ---- the CPU product ----------------------------------------------------------------------------------------------

		struct In
		{
			const RegionEvidence::Precursors* stats = nullptr;   // P11 (null = absent)
			double frameAplCode = -1.0;                          // P4's mean picture code (10-bit); < 0 = unknown
		};

		struct Events
		{
			bool cut = false, resync = false, rebase = false;    // discontinuities (U7 cut/fade, queue resync, re-base)
		};

		struct Out
		{
			uint32_t unitVersion = kUnitVersion;
			Mode mode = Mode::Neutral;
			Reset reset = Reset::None;
			RegionEvidence::Grid grid;
			std::vector<double> data;     // Count() x kChannels
			std::string status;           // "" = complete and exact
			double frameJ = 0.0;
			double gate = 1.0;
			Kernel kernel;
			VpAncestry::Header header;    // stamped by the producer (the slot protocol fills cgen/stamp)

			bool Valid() const { return grid.gw > 0 && data.size() == grid.Count() * kChannels; }
			const double* Block(int bx, int by) const { return &data[(static_cast<size_t>(by) * grid.gw + bx) * kChannels]; }
		};

		// The coverage-weighted mean of J_local over the grid (row-major): the frame level used when a block has no picture
		// support in its window. Mirror input of the pass (the host passes it as ll_frame_j).
		template <class T>
		inline T FrameLevelJ(int gw, int gh, const T* stats, size_t stride)
		{
			using namespace RegionEvidence;
			T sw = T(0), sj = T(0);
			for (size_t i = 0; i < static_cast<size_t>(gw) * gh; ++i)
			{
				const T* s = stats + i * stride;
				sw += s[kCoverage];
				sj += s[kCoverage] * s[kMeanPq];
			}
			return sw > T(kSupportEpsilon) ? sj / sw : T(0);
		}

		inline std::string Reasons(const std::vector<std::string>& r)
		{
			std::string s;
			for (size_t i = 0; i < r.size(); ++i) s += (i ? "; " : "") + r[i];
			return s;
		}

		// One frame, no temporal state (prev = the previous slot or null). reset forces the unsmoothed value.
		inline Out Compute(const In& in, const Params& p, const Lut* lut, const double* prev, bool reset, Reset resetReason)
		{
			using namespace RegionEvidence;
			Out out;
			out.reset = reset ? resetReason : Reset::None;
			out.grid = MakeGrid(p.w, p.h, p.B);
			out.kernel = KernelFor(p);
			const size_t n = out.grid.Count();
			out.data.assign(n * kChannels, 0.0);
			if (lut == nullptr || out.grid.gw <= 0)
			{
				out.mode = Mode::Neutral;
				out.status = "Light level: neutral (no display context: light term 1)";
				return out;
			}
			const std::vector<float> texels = PackTexels(*lut);
			std::vector<std::string> why;
			if (lut->exactness == Exactness::Approx) why.push_back(lut->status);

			const bool statsOk = in.stats != nullptr && in.stats->Valid() && in.stats->grid.w == p.w && in.stats->grid.h == p.h &&
				in.stats->grid.B == p.B && in.stats->picture == RegionEvidence::PictureOrFrame(p.picture, p.w, p.h);
			if (!statsOk)
			{
				const char* reason = in.stats == nullptr ? "no region statistics" : "region statistics of another size or grid";
				if (in.frameAplCode >= 0.0)
				{
					out.mode = Mode::FrameLevel;
					const PackedEntry<double> e = FetchPacked<double>(texels.data(), in.frameAplCode);
					out.frameJ = e.J;
					for (size_t i = 0; i < n; ++i)
					{
						double* o = &out.data[i * kChannels];
						o[kJSurround] = e.J; o[kJLocal] = e.J; o[kV] = std::min(e.V, kVCap); o[kConf] = p.frameLevelConf;
					}
					out.status = std::string("Light level: reduced (") + reason + "; frame level from the source APL)";
				}
				else
				{
					out.mode = Mode::CodeOnly;
					out.status = std::string("Light level: code only (") + reason + ", no source APL; LUT per pixel)";
				}
				return out;
			}

			out.mode = Mode::Full;
			const Precursors& pre = *in.stats;
			out.gate = (pre.lightFingerprint == lut->fingerprint) ? 1.0 : 0.0;
			if (pre.lightFingerprint.empty())
				why.push_back("region statistics made without the LUT: confidence 0");
			else if (out.gate == 0.0)
				why.push_back("region statistics made for another display context: confidence 0");
			if (!pre.HaveSplit()) why.push_back("no grain split: confidence 0");
			if (out.kernel.capped) why.push_back("surround window capped");
			if (!(p.viewingDistancePictureHeights > 0.0)) why.push_back("viewing distance unknown: sigma = picture height / " +
				std::to_string(static_cast<int>(p.fallbackSigmaDivisor + 0.5)));

			out.frameJ = FrameLevelJ<double>(pre.grid.gw, pre.grid.gh, pre.data.data(), kPrecursorChannels);
			const double alpha = TemporalAlpha(p);
			for (int by = 0; by < out.grid.gh; ++by)
				for (int bx = 0; bx < out.grid.gw; ++bx)
				{
					double o[kChannels];
					BlockOut<double>(p, out.kernel, out.grid.gw, out.grid.gh, pre.data.data(), kPrecursorChannels, texels.data(),
						out.frameJ, out.gate, prev, reset, alpha, bx, by, o);
					for (int c = 0; c < kChannels; ++c) out.data[(static_cast<size_t>(by) * out.grid.gw + bx) * kChannels + c] = o[c];
				}
			out.status = why.empty() ? std::string() : "Light level: reduced (" + Reasons(why) + ")";
			return out;
		}

		// The reset decision and the one-pole state of one stream (the lane's CPU twin; the GPU reads the previous slot).
		class State
		{
		public:
			Out Step(const In& in, const Params& p, const Lut* lut, const Events& ev)
			{
				const std::string ctx = lut ? lut->fingerprint : std::string();
				const std::string geo = GeometryKey(p);
				Reset why = Reset::None;
				if (!valid_) why = Reset::First;
				else if (geo != geometry_) why = Reset::Geometry;
				else if (ctx != context_) why = Reset::Context;
				else if (prev_.empty()) why = Reset::NoHistory;
				else if (ev.cut) why = Reset::Cut;
				else if (ev.resync) why = Reset::Resync;
				else if (ev.rebase) why = Reset::Rebase;
				const bool reset = why != Reset::None;
				Out o = Compute(in, p, lut, reset || prev_.empty() ? nullptr : prev_.data(), reset, why);
				if (reset && why != Reset::First)
					o.status += (o.status.empty() ? std::string() : std::string(" | ")) + "Light level: reset (" + Token(why) + ")";
				prev_ = o.data;
				if (o.mode != Mode::Full) prev_.clear();   // a reduced frame is no smoothing history
				geometry_ = geo; context_ = ctx; valid_ = true;
				return o;
			}
			void Invalidate() { valid_ = false; prev_.clear(); }
			const std::vector<double>& Previous() const { return prev_; }

		private:
			static std::string GeometryKey(const Params& p)
			{
				const RegionEvidence::Rect r = RegionEvidence::PictureOrFrame(p.picture, p.w, p.h);
				return std::to_string(p.w) + "x" + std::to_string(p.h) + "/" + std::to_string(p.B) + "/" + std::to_string(r.left) +
					"," + std::to_string(r.top) + "," + std::to_string(r.right) + "," + std::to_string(r.bottom);
			}
			bool valid_ = false;
			std::string geometry_, context_;
			std::vector<double> prev_;
		};

		// Ancestry stamp (the slot protocol fills cgen/stamp; this records what the product was made from).
		inline void Stamp(Out& o, uint64_t seq, uint32_t cgen, VpAncestry::Provenance prov, VpIndicator::CounterMask reads,
			const VpAncestry::AncestorRef* ancestors, size_t count)
		{
			o.header.seq = seq;
			o.header.cgen = cgen;
			o.header.prov = prov;
			o.header.reads = reads;
			o.header.ancestorCount = static_cast<uint8_t>((std::min)(count, VpAncestry::kMaxAncestors));
			for (size_t i = 0; i < o.header.ancestorCount; ++i) o.header.ancestors[i] = ancestors[i];
		}

		// Bytes of one frame's block product (excluding the 16 KB LUT): the same number at 1080p, 4K and 8K (B scales).
		inline uint64_t BlockBytes(int w, int h) { return static_cast<uint64_t>(RegionEvidence::MakeGrid(w, h, RegionEvidence::BlockSizeFor(h)).Count()) * kBytesPerBlock; }
	}
}
