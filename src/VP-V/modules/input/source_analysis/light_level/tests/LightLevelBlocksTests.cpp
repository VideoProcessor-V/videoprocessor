// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Tests for the light-level block product P12 (LightLevelBlocks.h, WP2) and for the LUT-driven stats pass of P11. CPU only:
// the stats law against the LUT, the coverage-weighted surround (bars never change it), the geometry-derived sigma at
// 1080p / 4K / 8K, the temporal one-pole and its resets (cut, resync, re-base, geometry, context), every fallback mode and its
// status text, the consumer accessor, BTB/WTW, determinism, storage per tier against the product declaration and the catalog
// version check. The shader mirror is in LightLevelBlocksWarpTests.cpp.
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/core/ProductCatalog.h>
#include <VP-V/modules/input/source_analysis/light_level/LightLevelBlocks.h>
#include <VP-V/modules/input/source_analysis/light_level/LightLevelBlocksGlsl.h>
#include <VP-V/modules/input/source_analysis/regions/RegionEvidence.h>
#include "../../regions/tests/RegionEvidenceSynthetic.h"
#include "LightLevelBlocksGolden.generated.h"

#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	namespace
	{
		using namespace RegionSynth;
		namespace RE = RegionEvidence;
		namespace LB = LightLevel::Blocks;

		constexpr int kW = 256, kB = 16;

		void Require(bool ok, const std::string& what)
		{
			if (ok) return;
			Logger::WriteMessage((what + "\n").c_str());
			Assert::Fail(std::wstring(what.begin(), what.end()).c_str());
		}

		bool Has(const std::string& text, const char* needle) { return text.find(needle) != std::string::npos; }

		RE::Rect R(int l, int t, int r, int b) { RE::Rect x; x.left = l; x.top = t; x.right = r; x.bottom = b; return x; }

		LightLevel::Settings Defaults()
		{
			return LightLevel::Read([](const std::string&, std::string&) { return false; });
		}

		LightLevel::Context Ctx(LightLevel::Context::Kind kind, double srcPeak = 0.0)
		{
			LightLevel::Context c;
			c.kind = kind;
			c.sourcePeakNits = srcPeak;
			return c;
		}

		// Picture content without any bar: a bright/dark layout with a ramp, texture and seeded grain (10-bit studio codes).
		Plane Content(int w, int h, double level = 0.0)
		{
			Plane p(w, h);
			for (int y = 0; y < h; ++y)
				for (int x = 0; x < w; ++x)
				{
					const double base = x < w / 2 ? 150.0 + 2.2 * x : 520.0 + 1.1 * (x - w / 2);
					p.at(x, y) = base + level + (y < h / 2 ? 0.0 : 60.0);
				}
			AddTexture(p, 20, 10, 90, 60, 18.0, 6.0);
			AddGrain(p, 1.2, 77);
			return p;
		}

		// The content inside bars: `top` rows above, `bottom` rows below, value `bar`.
		Plane Letterbox(const Plane& c, int top, int bottom, double bar)
		{
			Plane p(c.w, c.h + top + bottom, bar);
			for (int y = 0; y < c.h; ++y)
				for (int x = 0; x < c.w; ++x) p.at(x, y + top) = c.at(x, y);
			return p;
		}

		RE::Precursors Stats(const Plane& y, const RE::Rect& picture, const LightLevel::Lut* lut, bool withSplit = true)
		{
			RE::Externals ex;
			ex.picture = picture;
			RE::FrameParams fp;
			fp.blockOverride = kB;
			fp.lut = lut;
			if (withSplit) return RE::ComputeFrame(y, ex, fp);
			return RE::Compute(y, nullptr, ex, fp);
		}

		LB::Params Pr(const Plane& y, const RE::Rect& picture)
		{
			return LB::ParamsFor(y.w, y.h, picture, kB);
		}

		LB::Out Product(const Plane& y, const RE::Rect& picture, const LightLevel::Lut& lut, bool withSplit = true)
		{
			const RE::Precursors pre = Stats(y, picture, &lut, withSplit);
			LB::In in;
			in.stats = &pre;
			return LB::Compute(in, Pr(y, picture), &lut, nullptr, true, LB::Reset::First);
		}
	}

	TEST_CLASS(LightLevelBlocksTests)
	{
	public:
		TEST_METHOD(StatsPassTakesJLAndVFromTheLutAndKeepsTheGenericPathWithout)
		{
			const LightLevel::Lut lut = LightLevel::Build(Defaults(), Ctx(LightLevel::Context::Sdr));
			const std::vector<float> texels = LightLevel::PackTexels(lut);
			const Plane y = Content(kW, 96);
			const RE::Precursors withLut = Stats(y, R(0, 0, kW, 96), &lut);
			const RE::Precursors generic = Stats(y, R(0, 0, kW, 96), nullptr);
			Require(withLut.lightFingerprint == lut.fingerprint && generic.lightFingerprint.empty(), "the stats record which law made them");
			Require(Has(RE::StatusOf(generic), "no light-level LUT") && !Has(RE::StatusOf(withLut), "no light-level LUT"),
				"the generic path is named in the region status");
			int checked = 0;
			for (int by = 0; by < withLut.grid.gh; ++by)
				for (int bx = 0; bx < withLut.grid.gw; ++bx)
				{
					const double* s = withLut.Block(bx, by);
					double sumJ = 0.0, sumL = 0.0;
					int n = 0;
					for (int yy = by * kB; yy < by * kB + kB && yy < y.h; ++yy)
						for (int xx = bx * kB; xx < bx * kB + kB && xx < y.w; ++xx)
						{
							const auto e = LightLevel::FetchPacked<double>(texels.data(), y.at(xx, yy));
							sumJ += e.J; sumL += e.L10k * 10000.0; ++n;
						}
					Require(std::fabs(s[RE::kMeanPq] - sumJ / n) < 1e-9 && std::fabs(s[RE::kMeanNits] - sumL / n) < 1e-6 * std::max(1.0, sumL / n),
						"mean J / L differ from the per-pixel LUT mean");
					const double v = LightLevel::FetchPacked<double>(texels.data(), s[RE::kMeanY]).V;
					Require(std::fabs(s[RE::kVisibilityStat] - std::min(std::max(v, 0.0), 1.0)) < 1e-12, "the visibility stat is not the clamped LUT V");
					++checked;
				}
			Require(checked > 0, "no blocks");
			// The generic path is unchanged: its visibility is the old knee law at the mean code.
			const double* g = generic.Block(2, 2);
			RE::VisibilityParams vp;
			Require(std::fabs(g[RE::kVisibilityStat] - RE::Visibility<double>(g[RE::kMeanY], 0, 100.0, vp)) < 1e-12, "the generic law changed");
		}

		TEST_METHOD(BarsNeverChangeTheSurroundAndACropEqualsTheLetterbox)
		{
			const LightLevel::Lut lut = LightLevel::Build(Defaults(), Ctx(LightLevel::Context::Sdr));
			const Plane c = Content(kW, 96);
			const RE::Rect pic = R(0, 32, kW, 128);
			const LB::Out black = Product(Letterbox(c, 32, 32, 64.0), pic, lut);
			const LB::Out white = Product(Letterbox(c, 32, 32, 900.0), pic, lut);
			const LB::Out btb = Product(Letterbox(c, 32, 32, 0.0), pic, lut);
			const LB::Out crop = Product(c, R(0, 0, kW, 96), lut);
			Require(black.mode == LB::Mode::Full && crop.mode == LB::Mode::Full, "full mode expected");
			Require(black.data.size() == white.data.size() && black.data.size() == btb.data.size(), "grids differ");
			for (size_t i = 0; i < black.data.size(); ++i)
			{
				Require(black.data[i] == white.data[i] && black.data[i] == btb.data[i], "bar level changed the product at " + std::to_string(i));
			}
			Require(crop.grid.gw == black.grid.gw && crop.grid.gh == 6 && black.grid.gh == 10, "grid sizes");
			double worst = 0.0;
			for (int by = 0; by < crop.grid.gh; ++by)
				for (int bx = 0; bx < crop.grid.gw; ++bx)
				{
					const double* a = crop.Block(bx, by);
					const double* b = black.Block(bx, by + 2);
					for (int ch = 0; ch < 3; ++ch) worst = std::max(worst, std::fabs(a[ch] - b[ch]));
				}
			Require(worst < 1e-12, "a letterboxed picture differs from the same picture cropped: " + std::to_string(worst));
			// Bar blocks: no picture, no confidence, no visibility, the surround continues the picture's.
			for (int bx = 0; bx < black.grid.gw; ++bx)
			{
				const double* bar = black.Block(bx, 0);
				Require(bar[LB::kConf] == 0.0 && bar[LB::kV] == 0.0 && bar[LB::kJLocal] == bar[LB::kJSurround], "bar block carries data");
				Require(bar[LB::kJSurround] > 0.0, "the surround of a bar block must come from the picture");
			}
		}

		TEST_METHOD(SurroundMatchesAnIndependentGaussianAndSmoothsAStep)
		{
			const LightLevel::Lut lut = LightLevel::Build(Defaults(), Ctx(LightLevel::Context::Sdr));
			const Plane c = Content(kW, 96);
			const RE::Rect pic = R(0, 0, kW, 96);
			const RE::Precursors pre = Stats(c, pic, &lut);
			LB::In in; in.stats = &pre;
			const LB::Params p = Pr(c, pic);
			const LB::Out out = LB::Compute(in, p, &lut, nullptr, true, LB::Reset::First);
			const LB::Kernel k = LB::KernelFor(p);
			for (int by = 0; by < out.grid.gh; ++by)
				for (int bx = 0; bx < out.grid.gw; ++bx)
				{
					double sw = 0, sj = 0;
					for (int y = 0; y < out.grid.gh; ++y)
						for (int x = 0; x < out.grid.gw; ++x)
						{
							if (std::abs(x - bx) > k.radius || std::abs(y - by) > k.radius) continue;
							const double w = std::exp(-0.5 * (std::pow((x - bx) / k.sigmaBlocks, 2) + std::pow((y - by) / k.sigmaBlocks, 2)));
							sw += w; sj += w * pre.Block(x, y)[RE::kMeanPq];
						}
					Require(std::fabs(out.Block(bx, by)[LB::kJSurround] - sj / sw) < 1e-9, "surround differs from the independent Gaussian");
				}
			// The dark half is lifted and the bright half lowered, never beyond the extremes of J_local.
			double lo = 1, hi = 0;
			for (int i = 0; i < out.grid.gw * out.grid.gh; ++i) { lo = std::min(lo, out.data[i * 4 + LB::kJLocal]); hi = std::max(hi, out.data[i * 4 + LB::kJLocal]); }
			for (int i = 0; i < out.grid.gw * out.grid.gh; ++i)
				Require(out.data[i * 4 + LB::kJSurround] >= lo - 1e-12 && out.data[i * 4 + LB::kJSurround] <= hi + 1e-12, "surround outside the range of J_local");
			const double dark = out.Block(2, 1)[LB::kJSurround], darkLocal = out.Block(2, 1)[LB::kJLocal];
			Require(dark > darkLocal || std::fabs(dark - darkLocal) < 0.05, "surround of a dark block must not be darker than itself");
		}

		TEST_METHOD(SigmaIsDerivedFromTheGeometryAtEveryResolution)
		{
			const LB::Params p1080 = LB::ParamsFor(1920, 1080, R(0, 0, 0, 0));
			const LB::Params p4k = LB::ParamsFor(3840, 2160, R(0, 0, 0, 0));
			const LB::Params p8k = LB::ParamsFor(7680, 4320, R(0, 0, 0, 0));
			Require(p1080.B == 16 && p4k.B == 32 && p8k.B == 64, "block sizes");
			const LB::Kernel k1 = LB::KernelFor(p1080), k2 = LB::KernelFor(p4k), k3 = LB::KernelFor(p8k);
			Require(std::fabs(k1.sigmaBlocks - k2.sigmaBlocks) < 1e-9 && std::fabs(k2.sigmaBlocks - k3.sigmaBlocks) < 1e-9 &&
				k1.radius == k2.radius && k2.radius == k3.radius, "the kernel is not the same share of the picture at 1080p / 4K / 8K");
			Require(!k1.fromViewing && !k1.capped, "fallback sigma expected");
			// Letterbox: the picture height, not the frame height.
			LB::Params box = LB::ParamsFor(1920, 1080, R(0, 140, 1920, 940));
			Require(LB::KernelFor(box).sigmaBlocks < k1.sigmaBlocks, "sigma must follow the picture height");
			// A known viewing distance: 1 degree at 3 picture heights.
			LB::Params view = p1080;
			view.viewingDistancePictureHeights = 3.0;
			const LB::Kernel kv = LB::KernelFor(view);
			const double expectPx = std::tan(1.0 * 3.14159265358979323846 / 180.0) * 3.0 * 1080.0;
			Require(kv.fromViewing && std::fabs(kv.sigmaBlocks * 16.0 - expectPx) < 1e-9, "viewing-distance sigma");
			// The window cap is reported.
			LB::Params huge = p1080;
			huge.viewingDistancePictureHeights = 200.0;
			Require(LB::KernelFor(huge).capped && LB::KernelFor(huge).radius == huge.maxRadiusBlocks, "the cap must apply and be flagged");
		}

		TEST_METHOD(TemporalOnePoleUsesTheRateRuleAndResetsOnEveryDiscontinuity)
		{
			const LightLevel::Lut lut = LightLevel::Build(Defaults(), Ctx(LightLevel::Context::Sdr));
			const RE::Rect pic = R(0, 0, kW, 96);
			const Plane dim = Content(kW, 96, -60.0), bright = Content(kW, 96, 200.0);
			const RE::Precursors sd = Stats(dim, pic, &lut), sb = Stats(bright, pic, &lut);
			const LB::Params p = Pr(dim, pic);
			// alpha: tau 0.3 s at 60 Hz; held in time above it; as tuned in frames at or below it.
			const double a60 = 1.0 - std::exp(-1.0 / (0.3 * 60.0));
			Require(std::fabs(LB::TemporalAlpha(p) - a60) < 1e-12, "alpha at the unknown/60 Hz reference");
			LB::Params p24 = p; p24.frameRateHz = 24.0;
			Require(std::fabs(LB::TemporalAlpha(p24) - a60) < 1e-12, "alpha at 24 Hz is the frame-tuned value");
			LB::Params p120 = p; p120.frameRateHz = 120.0;
			Require(std::fabs(LB::TemporalAlpha(p120) - (1.0 - std::sqrt(1.0 - a60))) < 1e-12, "alpha at 120 Hz holds the time constant");

			LB::State st;
			LB::In ind; ind.stats = &sd;
			LB::In inb; inb.stats = &sb;
			const LB::Out f0 = st.Step(ind, p, &lut, LB::Events());
			Require(f0.reset == LB::Reset::First, "first frame resets");
			const LB::Out target = LB::Compute(inb, p, &lut, nullptr, true, LB::Reset::First);
			const double s0 = f0.Block(8, 3)[LB::kJSurround], t = target.Block(8, 3)[LB::kJSurround];
			Require(t > s0 + 0.01, "the bright frame must raise the surround");
			const LB::Out f1 = st.Step(inb, p, &lut, LB::Events());
			Require(f1.reset == LB::Reset::None, "no reset on an ordinary frame");
			Require(std::fabs(f1.Block(8, 3)[LB::kJSurround] - (s0 + a60 * (t - s0))) < 1e-12, "one-pole step");
			Require(std::fabs(f1.Block(8, 3)[LB::kJLocal] - target.Block(8, 3)[LB::kJLocal]) < 1e-12, "J_local is not smoothed");
			// Convergence at the time constant (120 Hz: 0.3 s = 36 frames to 63 %).
			LB::State st2;
			st2.Step(ind, p120, &lut, LB::Events());
			double v = 0;
			for (int i = 0; i < 36; ++i) v = st2.Step(inb, p120, &lut, LB::Events()).Block(8, 3)[LB::kJSurround];
			const double frac = (v - s0) / (t - s0);
			Require(std::fabs(frac - (1.0 - std::exp(-1.0))) < 0.02, "63 % in 0.3 s at 120 Hz: " + std::to_string(frac));
			// Resets: each discontinuity gives the unsmoothed value and names its reason.
			struct Case { const char* name; LB::Events ev; LB::Reset want; };
			LB::Events cut, resync, rebase;
			cut.cut = true; resync.resync = true; rebase.rebase = true;
			const Case cases[3] = { { "cut", cut, LB::Reset::Cut }, { "resync", resync, LB::Reset::Resync }, { "re-base", rebase, LB::Reset::Rebase } };
			for (const Case& cs : cases)
			{
				LB::State s;
				s.Step(ind, p, &lut, LB::Events());
				const LB::Out o = s.Step(inb, p, &lut, cs.ev);
				Require(o.reset == cs.want, std::string("reset reason for ") + cs.name);
				Require(std::fabs(o.Block(8, 3)[LB::kJSurround] - t) < 1e-12, std::string("unsmoothed value after ") + cs.name);
				Require(Has(o.status, "reset") && Has(o.status, LB::Token(cs.want)), std::string("status names the reset: ") + cs.name);
			}
			{   // geometry
				LB::State s;
				s.Step(ind, p, &lut, LB::Events());
				const Plane other = Content(kW, 112, 200.0);
				const RE::Precursors so = Stats(other, R(0, 0, kW, 112), &lut);
				LB::In ino; ino.stats = &so;
				Require(s.Step(ino, Pr(other, R(0, 0, kW, 112)), &lut, LB::Events()).reset == LB::Reset::Geometry, "geometry reset");
				// and a picture-rect change on the same raster
				LB::State s2;
				s2.Step(ind, p, &lut, LB::Events());
				const RE::Precursors sp = Stats(bright, R(0, 16, kW, 80), &lut);
				LB::In inp; inp.stats = &sp;
				Require(s2.Step(inp, Pr(bright, R(0, 16, kW, 80)), &lut, LB::Events()).reset == LB::Reset::Geometry, "picture-rect reset");
			}
			{   // context
				LB::State s;
				s.Step(ind, p, &lut, LB::Events());
				const LightLevel::Lut hdr = LightLevel::Build(Defaults(), Ctx(LightLevel::Context::HdrToSdr, 1000.0));
				const RE::Precursors sh = Stats(bright, pic, &hdr);
				LB::In inh; inh.stats = &sh;
				Require(s.Step(inh, p, &hdr, LB::Events()).reset == LB::Reset::Context, "context reset");
			}
		}

		TEST_METHOD(EveryFallbackIsNamedAndRecovers)
		{
			const LightLevel::Lut lut = LightLevel::Build(Defaults(), Ctx(LightLevel::Context::Sdr));
			const Plane c = Content(kW, 96);
			const RE::Rect pic = R(0, 0, kW, 96);
			const LB::Params p = Pr(c, pic);
			const RE::Precursors good = Stats(c, pic, &lut);
			const size_t n = good.grid.Count();
			LB::In in;
			// no display context
			in.stats = &good;
			LB::Out o = LB::Compute(in, p, nullptr, nullptr, true, LB::Reset::First);
			Require(o.mode == LB::Mode::Neutral && Has(o.status, "neutral") && Has(o.status, "light term 1"), "neutral");
			for (double v : o.data) Require(v == 0.0, "neutral data must be empty");
			// no P11 but P4's APL
			in.stats = nullptr; in.frameAplCode = 400.0;
			o = LB::Compute(in, p, &lut, nullptr, true, LB::Reset::First);
			const auto e = LightLevel::FetchPacked<double>(LightLevel::PackTexels(lut).data(), 400.0);
			Require(o.mode == LB::Mode::FrameLevel && Has(o.status, "no region statistics") && Has(o.status, "APL"), "frame level status");
			for (size_t i = 0; i < n; ++i)
				Require(std::fabs(o.data[i * 4 + LB::kJSurround] - e.J) < 1e-12 && o.data[i * 4 + LB::kConf] == p.frameLevelConf, "frame level data");
			// no P11 and no APL
			in.frameAplCode = -1.0;
			o = LB::Compute(in, p, &lut, nullptr, true, LB::Reset::First);
			Require(o.mode == LB::Mode::CodeOnly && Has(o.status, "code only") && Has(o.status, "no source APL"), "code only status");
			for (size_t i = 0; i < n; ++i) Require(o.data[i * 4 + LB::kConf] == 0.0, "code only conf");
			// stats of another grid
			const Plane other = Content(kW, 112);
			const RE::Precursors wrong = Stats(other, R(0, 0, kW, 112), &lut);
			in.stats = &wrong; in.frameAplCode = 300.0;
			o = LB::Compute(in, p, &lut, nullptr, true, LB::Reset::First);
			Require(o.mode == LB::Mode::FrameLevel && Has(o.status, "another size or grid"), "mismatched grid");
			// stats made without the LUT: values kept, confidence 0, named
			const RE::Precursors generic = Stats(c, pic, nullptr);
			in.stats = &generic;
			o = LB::Compute(in, p, &lut, nullptr, true, LB::Reset::First);
			Require(o.mode == LB::Mode::Full && o.gate == 0.0 && Has(o.status, "without the LUT"), "generic stats");
			for (size_t i = 0; i < n; ++i) Require(o.data[i * 4 + LB::kConf] == 0.0, "gated conf");
			// stats made for another context
			const LightLevel::Lut hdr = LightLevel::Build(Defaults(), Ctx(LightLevel::Context::HdrToSdr, 1000.0));
			const RE::Precursors other_ctx = Stats(c, pic, &hdr);
			in.stats = &other_ctx;
			o = LB::Compute(in, p, &lut, nullptr, true, LB::Reset::First);
			Require(o.gate == 0.0 && Has(o.status, "another display context"), "context mismatch");
			// no split: confidence 0, J and V kept
			const RE::Precursors nosplit = Stats(c, pic, &lut, false);
			in.stats = &nosplit;
			o = LB::Compute(in, p, &lut, nullptr, true, LB::Reset::First);
			Require(o.mode == LB::Mode::Full && Has(o.status, "no grain split"), "no split status");
			for (size_t i = 0; i < n; ++i) Require(o.data[i * 4 + LB::kConf] == 0.0 && o.data[i * 4 + LB::kV] >= 0.0, "no-split conf");
			// recovery: the next frame with everything present is complete again, with no stale state
			LB::State st;
			st.Step(in, p, &lut, LB::Events());
			in.stats = &good;
			o = st.Step(in, p, &lut, LB::Events());
			Require(o.mode == LB::Mode::Full && o.gate == 1.0, "recovery");
			bool anyConf = false;
			for (size_t i = 0; i < n; ++i) anyConf = anyConf || o.data[i * 4 + LB::kConf] == 1.0;
			Require(anyConf, "confidence returns with the products");
			Require(Has(o.status, "viewing distance unknown"), "the fallback sigma is reported");
			{   // a reduced frame is no smoothing history: the next full frame is unsmoothed and says so
				LB::State s2;
				LB::In none; none.frameAplCode = 300.0;
				s2.Step(none, p, &lut, LB::Events());
				in.stats = &good;
				const LB::Out full = s2.Step(in, p, &lut, LB::Events());
				Require(full.mode == LB::Mode::Full && full.reset == LB::Reset::NoHistory, "no-history reset");
			}
		}

		TEST_METHOD(BlockLawMatchesThePythonMirror)
		{
			namespace G = LightLevelBlocksGolden;
			const LightLevel::Lut lut = LightLevel::Build(Defaults(), Ctx(LightLevel::Context::Sdr));
			const std::vector<float> texels = LightLevel::PackTexels(lut);
			const size_t n = static_cast<size_t>(G::kGw) * G::kGh;
			std::vector<double> stats(n * RE::kStatsChannels, 0.0), prev(n * 4, 0.0);
			for (size_t i = 0; i < n; ++i)
			{
				double* s = &stats[i * RE::kStatsChannels];
				s[RE::kMeanPq] = G::kJLocal[i]; s[RE::kCoverage] = G::kCoverage[i]; s[RE::kMeanY] = G::kMeanY[i]; s[RE::kStatN] = G::kGrainN[i];
				prev[i * 4 + LB::kJSurround] = G::kPrev[i];
			}
			LB::Kernel k;
			k.sigmaBlocks = G::kSigma; k.radius = G::kRadius;
			const LB::Params p = LB::ParamsFor(G::kGw * kB, G::kGh * kB, R(0, 0, 0, 0), kB);
			Require(std::fabs(LB::FrameLevelJ<double>(G::kGw, G::kGh, stats.data(), RE::kStatsChannels) - G::kFrameJ) < 1e-12, "frame level");
			double worst = 0.0;
			for (int pass = 0; pass < 2; ++pass)
			{
				const double* gold = pass == 0 ? G::kReset : G::kSmooth;
				for (int by = 0; by < G::kGh; ++by)
					for (int bx = 0; bx < G::kGw; ++bx)
					{
						double o[4];
						LB::BlockOut<double>(p, k, G::kGw, G::kGh, stats.data(), RE::kStatsChannels, texels.data(), G::kFrameJ, 1.0,
							prev.data(), pass == 0, G::kAlpha, bx, by, o);
						for (int c = 0; c < 4; ++c)
						{
							const double ref = gold[(static_cast<size_t>(by) * G::kGw + bx) * 4 + c];
							const double err = std::fabs(o[c] - ref) / std::max(1.0, std::fabs(ref));
							worst = std::max(worst, err);
							Require(err < 2e-6, "block " + std::to_string(bx) + "," + std::to_string(by) + " channel " + std::to_string(c) + " pass " +
								std::to_string(pass) + " differs from the python mirror: " + std::to_string(err));
						}
					}
			}
			Logger::WriteMessage(("LIGHT_BLOCKS_GOLDEN worst=" + std::to_string(worst) + "
").c_str());
		}

		TEST_METHOD(BelowBlackAndAboveWhiteStayInTheDataAndAreLookedUpNotClamped)
		{
			const LightLevel::Lut lut = LightLevel::Build(Defaults(), Ctx(LightLevel::Context::Sdr));
			const std::vector<float> texels = LightLevel::PackTexels(lut);
			Plane btb(kW, 64, 40.0);          // below black (limited range)
			Plane wtw(kW, 64, 1000.0);        // above white
			const LB::Out a = Product(btb, R(0, 0, kW, 64), lut), b = Product(wtw, R(0, 0, kW, 64), lut);
			const auto jb = LightLevel::FetchPacked<double>(texels.data(), 40.0);
			const auto jw = LightLevel::FetchPacked<double>(texels.data(), 1000.0);
			Require(std::fabs(a.Block(4, 1)[LB::kJLocal] - jb.J) < 1e-9 && a.Block(4, 1)[LB::kV] == 0.0, "BTB: J of the display black, V 0");
			Require(std::fabs(b.Block(4, 1)[LB::kJLocal] - jw.J) < 1e-9, "WTW: looked up as written");
			Require(b.Block(4, 1)[LB::kJLocal] > a.Block(4, 1)[LB::kJLocal], "white above black");
		}

		TEST_METHOD(AccessorBlendsSurroundAndPixelByConfidenceAndDegradesToThePixelLevel)
		{
			const LightLevel::Lut lut = LightLevel::Build(Defaults(), Ctx(LightLevel::Context::Sdr));
			const std::vector<float> texels = LightLevel::PackTexels(lut);
			const Plane c = Content(kW, 96);
			const LB::Out o = Product(c, R(0, 0, kW, 96), lut);
			const int gw = o.grid.gw, gh = o.grid.gh;
			// at a block centre the bilinear read is that block
			const double px = 5 * kB + 8.0, py = 2 * kB + 8.0;
			const auto e = LightLevel::FetchPacked<double>(texels.data(), 300.0);
			const LB::Eff<double> r = LB::EffectiveAt<double>(gw, gh, kB, o.data.data(), texels.data(), px, py, 300.0, 0.5);
			const double* blk = o.Block(5, 2);
			Require(std::fabs(r.conf - blk[LB::kConf]) < 1e-12, "conf at the centre");
			Require(std::fabs(r.jEff - (e.J + (blk[LB::kJSurround] - e.J) * (0.5 * blk[LB::kConf]))) < 1e-12, "J_eff blend");
			Require(r.vPixel == e.V, "V at the pixel's own code");
			// product absent (code only): J_eff is the pixel's own level
			const std::vector<double> zero(static_cast<size_t>(gw) * gh * 4, 0.0);
			const LB::Eff<double> z = LB::EffectiveAt<double>(gw, gh, kB, zero.data(), texels.data(), px, py, 300.0, 0.5);
			Require(z.conf == 0.0 && z.jEff == e.J, "absent product must give the pixel level");
			// outside the grid reads clamp to the edge block
			const LB::Eff<double> out = LB::EffectiveAt<double>(gw, gh, kB, o.data.data(), texels.data(), -50.0, 9000.0, 300.0, 0.5);
			Require(std::isfinite(out.jEff), "out-of-range read");
			// the GLSL names exist
			Require(Has(LB::LightLevelBlocksAccessorGlsl(), "vpu_light_jeff") && Has(LB::LightLevelBlocksAccessorGlsl(), "vpu_light_block"), "accessor names");
		}

		TEST_METHOD(DeterministicAndStoredSizesFitTheDeclarationAtEveryTier)
		{
			const LightLevel::Lut lut = LightLevel::Build(Defaults(), Ctx(LightLevel::Context::Sdr));
			const Plane c = Content(kW, 96);
			const LB::Out a = Product(c, R(0, 0, kW, 96), lut), b = Product(c, R(0, 0, kW, 96), lut);
			Require(a.data.size() == b.data.size() && std::memcmp(a.data.data(), b.data.data(), a.data.size() * sizeof(double)) == 0, "not deterministic");
			// 65 KB per frame at 1080p, 4K and 8K (the block grid is the same size); within the declared P12 size.
			const VpSkeleton::Registry reg = VpSkeleton::SkeletonProducts();
			const VpSkeleton::ProductDecl& d = reg.Get(VpSkeleton::kP12LightLevel);
			const VpSkeleton::Dims dims[3] = { { 1920, 1080 }, { 3840, 2160 }, { 7680, 4320 } };
			for (const VpSkeleton::Dims& dm : dims)
			{
				const uint64_t bytes = LB::BlockBytes(static_cast<int>(dm.width), static_cast<int>(dm.height));
				Require(bytes == 8160u * 16u, "block bytes at " + std::to_string(dm.width));
				Require(bytes + 1024u * 16u <= VpSkeleton::BytesOf(d.size, dm), "declared size too small at " + std::to_string(dm.width));
			}
			Require(VpSkeleton::BytesOf(d.size, dims[0]) == 16384u + 8160u * 16u, "declaration at 1080p is exact (LUT + blocks)");
			Require(d.inputCount >= 1, "P12 reads P11");
		}

		TEST_METHOD(CatalogVersionsAgreeAndAStaleReaderGetsAVisibleFallback)
		{
			Assert::AreEqual(2u, static_cast<unsigned>(LightLevel::kUnitVersion));
			Assert::AreEqual(2u, static_cast<unsigned>(RE::kUnitVersion));
			FallbackCatalog::Decision d;
			Require(VpSkeleton::Catalog::VerifyUnit(VpSkeleton::kP12LightLevel, LightLevel::kUnitVersion, &d), "current reader");
			Require(!VpSkeleton::Catalog::VerifyUnit(VpSkeleton::kP12LightLevel, 1, &d), "a version-1 reader must not mix laws");
			Require(!VpSkeleton::Catalog::VerifyUnit(VpSkeleton::kP11RegionBlockStats, 1, &d), "a version-1 stats reader must not mix laws");
			const LightLevel::Lut lut = LightLevel::Build(Defaults(), Ctx(LightLevel::Context::Sdr));
			const LB::Out o = Product(Content(kW, 96), R(0, 0, kW, 96), lut);
			Require(o.unitVersion == LightLevel::kUnitVersion, "slots carry the unit version");
		}
	};
}
