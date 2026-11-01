// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// output.dither / dither need map (DRAFT, setting dither_need = off | auto, default off).
//
// The output sigma-delta (Temporal sigma-delta.glsl) dithers the 8-bit link with a fixed noise_amplitude. Where the picture
// carries enough grain, the grain itself dithers the link; where it does not (a grain-free gradient, a fade, a clean CGI
// sky) and one LINK step is a visible step, the link bands. This module derives a per-region-block EXTRA amplitude that is
// added to noise_amplitude only there:
//
//   amplitude(pixel) = noise_amplitude + extra(pixel),   extra >= 0, 0 where the grain dithers or the step is invisible.
//
// Everything is read through the shared products (never recomputed): the block grain level N through
// RegionEvidence::GrainView::Level, mean luma, banding-prone, in-picture and overlay through RegionEvidence::View, and the
// light-level LUT (LightLevel::Fetch: V = visibility of one input code in Weber JNDs, Sout = d sdrCode / d code).
//
// The law (per block, 10-bit-scale codes; q_out = 2^(10 - colorBits) is the link code step in that scale):
//
//   sigmaOut = sqrt(max(N,0)) * |Sout| / q_out                       grain rms in link codes
//   aTarget  = clamp(aNeed - sigmaOut, aBase, max(aMax, aBase))
//   gVis     = |Sout| > 1e-6 ? smoothstep(visLo, visHi, V * q_out / |Sout|) : 0        one LINK step in JND
//   g        = (inPicture && !overlay) ? banding * gVis : 0
//   target   = g * (aTarget - aBase)                                 EXTRA amplitude
//
// Temporal state per block (Map::Update): reset to the target on the first frame, a discontinuity (cut, resync, re-base,
// context change), a grid / LUT / colour-depth / aBase change; otherwise hysteresis (|target - e| <= deadband holds e) and a
// one-pole e += alpha (target - e), alpha = 1 - exp(-dt / tau) with dt the MEASURED frame interval (same time constant at
// every frame rate; computed on the CPU, a uniform in the shader).
//
// Missing input (no grain product, grain of another grid, no precursors, no LUT, unsupported domain) = target 0 for every
// block, e reset: amplitude == noise_amplitude, i.e. today's behaviour, and the status string names every missing input
// (no silent fallback). The next frame with all inputs present recovers by itself (e = target).
//
// Frame-level skip: NeededAnywhere (is any code's link step visible at all?) is evaluated once per LUT fingerprint; if not,
// the pass is skipped and the status says so.
//
// CPU reference = the double instantiation of the templates; the float instantiation is the arithmetic order of the shader
// (TargetBlock<float>, SampleExtra). Raster loops, fixed order, no threads: deterministic. Header-only, C++14; no Windows
// or libplacebo headers (the colour depth is an argument; SigmaDeltaDomain::Domain::color supplies it at integration).
#pragma once

#include <VP-V/core/Products.h>
#include <VP-V/modules/input/source_analysis/light_level/LightLevelLut.h>
#include <VP-V/modules/input/source_analysis/regions/RegionEvidence.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace DitherNeed
{
	constexpr int kVersion = 1;

	// Skeleton token of the product (Products.h registers tokens only for products declared outside FirstProducts by a
	// comment block; this module keeps its own constant instead of editing Products.h).
	constexpr const char* kTokenDitherNeed = "dither.need";

	struct Params
	{
		double aBase = 0.15;       // the shader's noise_amplitude (passed in)
		double aNeed = 0.55;       // total amplitude wanted with no grain
		double aMax = 0.5;         // ceiling (the quantiser's exact-code safety limit)
		double visLo = 0.25;       // one link step in JND: invisible below, fully visible above visHi
		double visHi = 0.75;
		double deadband = 0.02;    // hysteresis on the extra amplitude
		double tauSeconds = 0.25;  // smoothing time constant
	};

	template <class T> inline T Smoothstep(T e0, T e1, T x)
	{
		if (!(e1 > e0)) return x >= e0 ? T(1) : T(0);
		T t = (x - e0) / (e1 - e0);
		t = t < T(0) ? T(0) : (t > T(1) ? T(1) : t);
		return t * t * (T(3) - T(2) * t);
	}

	// One link code step in the 10-bit code scale: 2^(10 - colorBits) (8 -> 4, 10 -> 1).
	inline double QOut(int colorBits)
	{
		const int bits = (std::min)((std::max)(colorBits, 1), 16);
		return std::ldexp(1.0, 10 - bits);
	}

	// Smoothing coefficient of one frame interval; dt <= 0 (unknown) = no smoothing.
	inline double AlphaFor(double dtSeconds, double tauSeconds)
	{
		if (!(dtSeconds > 0.0) || !(tauSeconds > 0.0)) return 1.0;
		return 1.0 - std::exp(-dtSeconds / tauSeconds);
	}

	// Visibility gate of one LINK step at an input level (V, Sout from the LUT).
	template <class T> inline T GateVis(const Params& p, T V, T Sout, T qOut)
	{
		const T aS = Sout < T(0) ? -Sout : Sout;
		return aS > T(1e-6) ? Smoothstep<T>(T(p.visLo), T(p.visHi), V * qOut / aS) : T(0);
	}

	// The law for one block (see the header comment). N, V, Sout, banding are the already-read scalars.
	template <class T>
	inline T TargetBlock(const Params& p, T N, T V, T Sout, T banding, bool inPicture, bool overlay, T qOut)
	{
		const T aS = Sout < T(0) ? -Sout : Sout;
		const T sigmaOut = std::sqrt(N > T(0) ? N : T(0)) * aS / qOut;
		const T aBase = T(p.aBase);
		const T hi = (std::max)(T(p.aMax), aBase);
		const T aTarget = (std::min)((std::max)(T(p.aNeed) - sigmaOut, aBase), hi);
		const T gVis = GateVis<T>(p, V, Sout, qOut);
		const T g = (inPicture && !overlay) ? banding * gVis : T(0);
		return g * (aTarget - aBase);
	}

	// ---- frame-level gate -----------------------------------------------------------------------------------------------

	struct LutGate
	{
		bool needed = false;
		double maxGate = 0.0;     // max over codes 64..1023 of gVis
		int worstCode = 0;
	};

	inline LutGate Gate(const LightLevel::Lut& lut, double qOut, const Params& p)
	{
		LutGate g;
		for (int c = 64; c < LightLevel::kLutSize; ++c)
		{
			const LightLevel::Entry& en = lut.e[static_cast<size_t>(c)];
			const double v = GateVis<double>(p, en.V, en.Sout, qOut);
			if (v > g.maxGate) { g.maxGate = v; g.worstCode = c; }
		}
		g.needed = g.maxGate > 0.0;
		return g;
	}

	// Is the link step visible at any code? false = the pass is skipped (cost ~0).
	inline bool NeededAnywhere(const LightLevel::Lut& lut, double qOut, const Params& p) { return Gate(lut, qOut, p).needed; }

	// NeededAnywhere once per (LUT fingerprint, qOut, visLo, visHi).
	class NeedCache
	{
	public:
		bool Get(const LightLevel::Lut& lut, double qOut, const Params& p)
		{
			rebuilt_ = !valid_ || fp_ != lut.fingerprint || qOut != q_ || p.visLo != lo_ || p.visHi != hi_;
			if (rebuilt_)
			{
				needed_ = NeededAnywhere(lut, qOut, p);
				fp_ = lut.fingerprint; q_ = qOut; lo_ = p.visLo; hi_ = p.visHi;
				valid_ = true;
			}
			return needed_;
		}
		bool LastRebuilt() const { return rebuilt_; }
		void Reset() { valid_ = false; }
	private:
		std::string fp_;
		double q_ = 0.0, lo_ = 0.0, hi_ = 0.0;
		bool needed_ = false, valid_ = false, rebuilt_ = false;
	};

	// ---- inputs ---------------------------------------------------------------------------------------------------------

	enum Missing : unsigned
	{
		kNoGrain = 1u << 0,                 // no CPU grain product
		kGrainGridMismatch = 1u << 1,       // grain product on another block grid / raster
		kNoPrecursors = 1u << 2,            // no region precursors (block statistics)
		kNoLut = 1u << 3,                   // no light-level table
		kUnsupportedDomain = 1u << 4,       // output code domain the sigma-delta cannot serve (float / 16-bit target)
		kEvidenceGridMismatch = 1u << 5     // precursors on another block grid than the grain product's
	};

	inline std::string MissingText(unsigned mask)
	{
		static const struct { unsigned bit; const char* text; } kNames[] = {
			{ kNoGrain, "no grain product" },
			{ kGrainGridMismatch, "grain product on another grid" },
			{ kNoPrecursors, "no region precursors" },
			{ kEvidenceGridMismatch, "region precursors on another grid" },
			{ kNoLut, "no light-level table" },
			{ kUnsupportedDomain, "output domain not supported" } };
		std::string s;
		for (const auto& n : kNames)
			if (mask & n.bit) s += (s.empty() ? "" : "; ") + std::string(n.text);
		return s;
	}

	// engaged = the setting is on. raised/total only matter when on.
	inline std::string StatusOf(bool enabled, unsigned missingMask, bool needed, size_t raised = 0, size_t total = 0)
	{
		if (!enabled) return "dither need: off";
		if (missingMask)
			return "dither need: unavailable (" + MissingText(missingMask) + "); sigma-delta at its base amplitude";
		if (!needed) return "dither need: not needed (link step invisible)";
		return "dither need: on (" + std::to_string(raised) + " of " + std::to_string(total) + " blocks raised)";
	}

	struct Inputs
	{
		const RegionEvidence::GrainView* grain = nullptr;
		const RegionEvidence::View* evidence = nullptr;
		const LightLevel::Lut* lut = nullptr;
		int colorBits = 8;
		bool domainSupported = true;
	};

	template <class T> struct BlockInT
	{
		T N = T(0), meanY = T(0), banding = T(0), V = T(0), Sout = T(1);
		bool inPicture = false, overlay = false;
	};
	using BlockIn = BlockInT<double>;

	// Fills `out` (raster order over `grid`) from the shared products and returns the missing mask (0 = complete; `out` is
	// then empty when anything is missing). Checks: grain CpuValid and on exactly this grid and raster; precursors valid,
	// same block size and same grid dimensions (probed at the grid's corners); a LUT; a supported domain. Block values:
	// N = GrainView::Level; meanY = View::Stat(kMeanY); banding / InPicture / Overlay = View::At; V, Sout = Fetch at meanY.
	inline unsigned Gather(const Inputs& in, const RegionEvidence::Grid& grid, std::vector<BlockIn>& out)
	{
		out.clear();
		unsigned miss = 0;
		if (!in.lut) miss |= kNoLut;
		if (!in.domainSupported) miss |= kUnsupportedDomain;
		if (!in.grain || !in.grain->CpuValid()) miss |= kNoGrain;
		else
		{
			const GrainSplit::Split* s = in.grain->Split();
			if (!s || s->B != grid.B || s->bw != grid.gw || s->bh != grid.gh || (grid.w > 0 && s->w != grid.w) ||
				(grid.h > 0 && s->h != grid.h))
				miss |= kGrainGridMismatch;
		}
		if (!in.evidence || !in.evidence->Valid()) miss |= kNoPrecursors;
		else if (in.evidence->BlockSize() != grid.B || grid.gw <= 0 || grid.gh <= 0 ||
			!in.evidence->At(grid.gw - 1, grid.gh - 1).valid || in.evidence->At(grid.gw, 0).valid || in.evidence->At(0, grid.gh).valid)
			miss |= kEvidenceGridMismatch;
		if (miss) return miss;

		out.resize(grid.Count());
		for (int by = 0; by < grid.gh; ++by)
			for (int bx = 0; bx < grid.gw; ++bx)
			{
				BlockIn& b = out[static_cast<size_t>(by) * grid.gw + bx];
				const RegionEvidence::Sample s = in.evidence->At(bx, by);
				b.N = in.grain->Level(bx, by);
				b.meanY = in.evidence->Stat(bx, by, RegionEvidence::kMeanY);
				b.banding = static_cast<double>(s.banding);
				b.inPicture = s.InPicture();
				b.overlay = s.Overlay();
				const LightLevel::Entry en = LightLevel::Fetch(*in.lut, b.meanY);
				b.V = en.V;
				b.Sout = en.Sout;
			}
		return 0;
	}

	// ---- temporal state ---------------------------------------------------------------------------------------------------

	enum class Result : int { Off, Unavailable, NotNeeded, On };

	struct FrameArgs
	{
		Params params;
		RegionEvidence::Grid grid;
		double dtSeconds = 0.0;          // measured frame interval; <= 0 = unknown (no smoothing)
		bool discontinuity = false;      // cut, resync, re-base, context change (the caller's epoch change)
		bool enabled = true;             // dither_need = auto
		unsigned missing = 0;            // Gather's mask (or the caller's own)
		bool needed = true;              // NeededAnywhere / NeedCache
		int colorBits = 8;
		std::string lutFingerprint;
	};

	template <class T>
	class MapT
	{
	public:
		// One frame. blocks = Gather's output (ignored unless on). Returns the result; Extra() is the per-block extra amplitude.
		Result Update(const FrameArgs& a, const std::vector<BlockInT<T>>& blocks)
		{
			const size_t n = a.grid.Count();
			unsigned miss = a.missing;
			if (a.enabled && miss == 0 && blocks.size() != n) miss |= kGrainGridMismatch;   // defensive: inconsistent caller
			missing_ = a.enabled ? miss : 0u;
			if (!a.enabled) return Finish(Result::Off, a, true);
			if (miss) return Finish(Result::Unavailable, a, true);
			if (!a.needed) return Finish(Result::NotNeeded, a, true);

			const bool reset = !valid_ || a.discontinuity || a.grid.gw != gw_ || a.grid.gh != gh_ || a.grid.B != B_ ||
				a.lutFingerprint != fp_ || a.colorBits != colorBits_ || a.params.aBase != aBase_ || e_.size() != n;
			const T q = T(QOut(a.colorBits));
			const T alpha = T(AlphaFor(a.dtSeconds, a.params.tauSeconds));
			const T dead = T(a.params.deadband);
			if (reset) e_.assign(n, T(0));
			for (size_t i = 0; i < n; ++i)
			{
				const BlockInT<T>& b = blocks[i];
				const T target = TargetBlock<T>(a.params, b.N, b.V, b.Sout, b.banding, b.inPicture, b.overlay, q);
				if (reset) { e_[i] = target; continue; }
				const T diff = target - e_[i];
				if ((diff < T(0) ? -diff : diff) <= dead) continue;   // hysteresis: hold
				e_[i] += alpha * diff;
			}
			Remember(a);
			valid_ = true;
			result_ = Result::On;
			raised_ = 0;
			for (size_t i = 0; i < n; ++i) if (e_[i] > T(1e-3)) ++raised_;
			total_ = n;
			return result_;
		}

		const std::vector<T>& Extra() const { return e_; }
		std::vector<float> ExtraFloat() const   // the GPU upload (one float per block, R32F)
		{
			std::vector<float> f(e_.size());
			for (size_t i = 0; i < e_.size(); ++i) f[i] = static_cast<float>(e_[i]);
			return f;
		}
		Result LastResult() const { return result_; }
		unsigned LastMissing() const { return missing_; }
		size_t Raised() const { return raised_; }
		bool Valid() const { return valid_; }
		std::string Status() const
		{
			return StatusOf(result_ != Result::Off, missing_, result_ != Result::NotNeeded, raised_, total_);
		}

	private:
		// Every non-On outcome: no extra, no state (the next full frame resets e = target).
		Result Finish(Result r, const FrameArgs& a, bool zero)
		{
			if (zero) e_.assign(a.grid.Count(), T(0));
			Remember(a);
			valid_ = false;
			result_ = r;
			raised_ = 0;
			total_ = a.grid.Count();
			return r;
		}
		void Remember(const FrameArgs& a)
		{
			gw_ = a.grid.gw; gh_ = a.grid.gh; B_ = a.grid.B;
			fp_ = a.lutFingerprint; colorBits_ = a.colorBits; aBase_ = a.params.aBase;
		}

		std::vector<T> e_;
		int gw_ = 0, gh_ = 0, B_ = 0, colorBits_ = 0;
		std::string fp_;
		double aBase_ = 0.0;
		bool valid_ = false;
		Result result_ = Result::Off;
		unsigned missing_ = 0;
		size_t raised_ = 0, total_ = 0;
	};
	using Map = MapT<double>;
	using MapF = MapT<float>;

	// ---- per-pixel sampling (the output raster) ---------------------------------------------------------------------------

	// Bilinear over the block centres, 4 fetches with indices CLAMPED to the grid (filter independent). The host supplies
	// the output -> source mapping: src = (pos + 0.5) * scale + off. float, the order of the GLSL mirror.
	inline float SampleExtra(const float* map, int gw, int gh, int blockPx, float scaleX, float scaleY, float offX, float offY,
		int posX, int posY)
	{
		if (!map || gw <= 0 || gh <= 0 || blockPx <= 0) return 0.0f;
		const float srcX = (static_cast<float>(posX) + 0.5f) * scaleX + offX;
		const float srcY = (static_cast<float>(posY) + 0.5f) * scaleY + offY;
		const float u = srcX / static_cast<float>(blockPx) - 0.5f;
		const float v = srcY / static_cast<float>(blockPx) - 0.5f;
		const float fx0 = std::floor(u), fy0 = std::floor(v);
		const float fx = u - fx0, fy = v - fy0;
		const int x0 = static_cast<int>(fx0), y0 = static_cast<int>(fy0);
		const int i0 = (std::min)((std::max)(x0, 0), gw - 1), i1 = (std::min)((std::max)(x0 + 1, 0), gw - 1);
		const int j0 = (std::min)((std::max)(y0, 0), gh - 1), j1 = (std::min)((std::max)(y0 + 1, 0), gh - 1);
		const float a = map[static_cast<size_t>(j0) * gw + i0], b = map[static_cast<size_t>(j0) * gw + i1];
		const float c = map[static_cast<size_t>(j1) * gw + i0], d = map[static_cast<size_t>(j1) * gw + i1];
		const float top = a + (b - a) * fx, bot = c + (d - c) * fx;
		return top + (bot - top) * fy;
	}

	// ---- setting ----------------------------------------------------------------------------------------------------------

	struct Settings
	{
		bool on = false;
	};

	// Key dither_need: off | auto (case-insensitive, surrounding blanks ignored). Absent or empty = off. Any other value:
	// an error line and off.
	inline bool Parse(const std::function<bool(const std::string&, std::string&)>& getKey, Settings& out,
		std::vector<std::string>& errors)
	{
		out.on = false;
		std::string raw;
		if (!getKey || !getKey("dither_need", raw)) return true;
		size_t b = 0, e = raw.size();
		while (b < e && std::isspace(static_cast<unsigned char>(raw[b]))) ++b;
		while (e > b && std::isspace(static_cast<unsigned char>(raw[e - 1]))) --e;
		std::string v = raw.substr(b, e - b), low = v;
		for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		if (low.empty() || low == "off") return true;
		if (low == "auto") { out.on = true; return true; }
		errors.push_back("dither_need: invalid value '" + v + "' (expected off or auto); using off");
		return false;
	}

	// ---- skeleton product -------------------------------------------------------------------------------------------------

	// One product node "dither.need": GPU compute, history 1 (the previous frame's smoothed state, ping-pong), inputs the
	// grain split and the region block statistics at lag 0. Block-grid rgba32f state texture, ping-pong x2 = blocks * 16 * 2
	// bytes. missingInputForm: without its inputs it is still filled (all zero = today's amplitude, named in the status).
	// Filled live by its only consumer (output.dither, before the sigma-delta); no lane precompute (sourceFrame = false).
	inline VpSkeleton::ProductId AppendProduct(std::vector<VpSkeleton::ProductDecl>& decls, VpSkeleton::ProductId grainSplitId,
		VpSkeleton::ProductId blockStatsId, VpSkeleton::Dims frame)
	{
		using namespace VpSkeleton;
		using namespace VpIndicator;
		const RegionEvidence::Cost c = RegionEvidence::CostAt(frame);
		ProductDecl d = detail::Make(static_cast<ProductId>(decls.size()), kTokenDitherNeed, "dither need map",
			Backend::GpuCompute, 1, Tier::T1, Bit(kFull) | Bit(kContinuity), Bit(kSettingsOutput));
		detail::AddInput(d, grainSplitId, 0);
		detail::AddInput(d, blockStatsId, 0);
		d.size.fixedBytes = static_cast<uint32_t>(c.blocks * 16 * 2);
		d.missingInputForm = true;
		d.refRenderMs = 0.05f;   // estimate: one tiny pass over the block grid
		decls.push_back(d);
		return d.id;
	}

	// The product's only consumer: output.dither (id assigned at integration).
	inline VpSkeleton::ConsumerDecl Consumer(VpSkeleton::ProductId needId, uint16_t id)
	{
		VpSkeleton::ConsumerDecl c;
		c.id = id;
		c.name = "output.dither (dither need map)";
		VpSkeleton::AddConsume(c, needId, 0);
		return c;
	}
}
