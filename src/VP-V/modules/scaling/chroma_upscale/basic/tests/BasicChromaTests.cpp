// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// scaling.chroma_upscale / basic: the basic polyphase lanczos chroma stand-in. BasicChromaModelTests exercise the CPU reference
// (exact phase / position logic against double-precision maths, the kernel against a double-precision reference, siting, BTB / WTW
// overshoot, bars, IPT 4:2:2, the engine read path). BasicChromaGpuTests compare the GLSL pass BYTE FOR BYTE with the CPU model
// on a libplacebo device: WARP unless VP_TEST_GPU=hardware (the chain gate runs them on the NVIDIA adapter, VP_TEST_ADAPTER=NVIDIA).
#include "pch.h"
#include "CppUnitTest.h"
#include <VP-V/tests/support/ExtendedRangeGpuProbe.h>

#include <VP-V/modules/scaling/chroma_upscale/basic/BasicChroma.h>
#include <VP-V/modules/scaling/chroma_upscale/basic/BasicChromaGpu.h>

#include <libplacebo/d3d11.h>
#include <libplacebo/dispatch.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace BasicChromaTestsNs
{
	using namespace BasicChroma;

	void LogLine(const std::string& s) { Logger::WriteMessage(s.c_str()); }

	// A plane of pseudo-random container codes in [lo, hi].
	Plane16 RandomPlane(int w, int h, uint32_t seed, int lo = 0, int hi = 65535)
	{
		std::mt19937 g(seed);
		Plane16 p(w, h);
		for (uint16_t& v : p.v) v = static_cast<uint16_t>(lo + static_cast<int>(g() % static_cast<uint32_t>(hi - lo + 1)));
		return p;
	}

	// A smooth picture with texture (codes), for the double-precision comparison.
	Plane16 SmoothPlane(int w, int h, uint32_t seed)
	{
		std::mt19937 g(seed);
		Plane16 p(w, h);
		for (int y = 0; y < h; ++y)
			for (int x = 0; x < w; ++x)
			{
				const double v = 32768.0 + 12000.0 * std::sin(0.31 * x + 0.17 * y) + 6000.0 * std::cos(0.07 * x * y * 0.1 + 1.0) +
					static_cast<double>(g() % 600);
				p.at(x, y) = static_cast<uint16_t>((std::min)(65535.0, (std::max)(0.0, std::floor(v))));
			}
		return p;
	}

	Geometry Geo(int lumaW, int lumaH, int fx, int fy, Siting s, const Rect& active = Rect())
	{
		double sx = 0, sy = 0;
		ShiftOf(s, fx, fy, sx, sy);
		std::string why;
		const Geometry g = MakeGeometry(lumaW, lumaH, fx, fy, sx, sy, active, &why);
		Assert::AreEqual(std::string(), Check(g));
		return g;
	}

	// The independent reference: double-precision Lanczos at the exact chroma coordinate, no Q14 table, no intermediate rounding,
	// taps clamped to the plane (whole-plane geometry only).
	double RefAt(const Plane16& c, const Geometry& g, int x, int y)
	{
		const double sx = g.s4x / 4.0, sy = g.s4y / 4.0;
		const double jx = (x + 0.5 - sx) / g.fx - 0.5, jy = (y + 0.5 - sy) / g.fy - 0.5;
		const int ix = static_cast<int>(std::floor(jx)), iy = static_cast<int>(std::floor(jy));
		double wx[kTaps], wy[kTaps], sumx = 0, sumy = 0;
		for (int k = 0; k < kTaps; ++k)
		{
			wx[k] = Lanczos((ix - 2 + k) - jx); sumx += wx[k];
			wy[k] = Lanczos((iy - 2 + k) - jy); sumy += wy[k];
		}
		double acc = 0;
		for (int ty = 0; ty < kTaps; ++ty)
			for (int tx = 0; tx < kTaps; ++tx)
			{
				const int kx = (std::min)((std::max)(ix - 2 + tx, 0), c.w - 1), ky = (std::min)((std::max)(iy - 2 + ty, 0), c.h - 1);
				acc += wy[ty] / sumy * wx[tx] / sumx * c.at(kx, ky);
			}
		return acc;
	}

	// ---- GPU: byte identity with the CPU model --------------------------------------------------------------------------------

	struct GpuDevice
	{
		pl_log log = nullptr;
		pl_d3d11 d3d11 = nullptr;
		pl_dispatch dispatch = nullptr;
		~GpuDevice()
		{
			if (dispatch) pl_dispatch_destroy(&dispatch);
			if (d3d11) pl_d3d11_destroy(&d3d11);
			if (log) pl_log_destroy(&log);
		}
		bool Create()
		{
			pl_log_params lp{};
			lp.log_level = PL_LOG_WARN;
			log = pl_log_create(PL_API_VER, &lp);
			std::string name;
			const pl_d3d11_params params = ExtendedRangeGpuProbe::TestDeviceParams(&name);
			Logger::WriteMessage(("basic chroma GPU tests on: " + name).c_str());
			d3d11 = pl_d3d11_create(log, &params);
			if (!d3d11) return false;
			dispatch = pl_dispatch_create(log, d3d11->gpu);
			return dispatch != nullptr;
		}
		pl_gpu Gpu() const { return d3d11->gpu; }
	};

	pl_tex UploadRg16(pl_gpu gpu, const Plane16& a, const Plane16& b)
	{
		std::vector<uint16_t> data(a.v.size() * 2);
		for (size_t i = 0; i < a.v.size(); ++i) { data[i * 2] = a.v[i]; data[i * 2 + 1] = b.v[i]; }
		pl_tex_params p{};
		p.w = a.w; p.h = a.h; p.format = pl_find_named_fmt(gpu, "rg16"); p.sampleable = true; p.initial_data = data.data();
		return pl_tex_create(gpu, &p);
	}

	bool DownloadRg16(pl_gpu gpu, pl_tex tex, Plane16& a, Plane16& b)
	{
		pl_tex_params rp{};
		rp.w = tex->params.w; rp.h = tex->params.h; rp.format = tex->params.format; rp.host_readable = true; rp.blit_dst = true;
		pl_tex readable = pl_tex_create(gpu, &rp);
		if (!readable) return false;
		pl_tex_blit_params bp{}; bp.src = tex; bp.dst = readable;
		pl_tex_blit(gpu, &bp);
		std::vector<uint16_t> raw(static_cast<size_t>(rp.w) * rp.h * 2, 0);
		pl_tex_transfer_params d{}; d.tex = readable; d.ptr = raw.data();
		const bool ok = pl_tex_download(gpu, &d);
		pl_tex_destroy(gpu, &readable);
		if (!ok) return false;
		a = Plane16(rp.w, rp.h); b = Plane16(rp.w, rp.h);
		for (size_t i = 0; i < a.v.size(); ++i) { a.v[i] = raw[i * 2]; b.v[i] = raw[i * 2 + 1]; }
		return true;
	}

	size_t Mismatches(const Plane16& a, const Plane16& b)
	{
		if (a.w != b.w || a.h != b.h) return a.v.size() + b.v.size() + 1;
		size_t n = 0;
		for (size_t i = 0; i < a.v.size(); ++i) n += a.v[i] != b.v[i];
		return n;
	}

	TEST_CLASS(BasicChromaModelTests)
	{
	public:
		TEST_METHOD(PhaseTablesSumExactlyAndStayInsideTheInt32Bound)
		{
			for (int f : { 1, 2, 4 })
			{
				const PhaseTable t = BuildPhaseTable(f);
				Assert::AreEqual(4 * f, t.phases);
				for (int p = 0; p < t.phases; ++p)
				{
					int sum = 0, pos = 0, neg = 0;
					for (int k = 0; k < kTaps; ++k) { sum += t.w[p][k]; (t.w[p][k] > 0 ? pos : neg) += std::abs(t.w[p][k]); }
					Assert::AreEqual(kWeightOne, sum, L"exactly one in Q14");
					// Worst case of the vertical sum: hv spans [-65535 N, 65535 P] (horizontal pass), so |acc| <= 65535 (P^2 + N^2) / 16384 in
					// weight units; it must stay below 2^31.
					const double P = pos, N = neg;
					Assert::IsTrue(65535.0 * (P * P + N * N) / kWeightOne < 2147483647.0, L"overflow bound of the int32 sums");
					if (p > 0)
						for (int k = 0; k < kTaps; ++k)
							Assert::IsTrue(std::abs(t.w[p][k] - t.w[t.phases - p][kTaps - 1 - k]) <= 4, L"phases mirror within the rounding remainder");
				}
				Assert::AreEqual(kWeightOne, t.w[0][kLobes - 1], L"phase 0 is the identity tap");
			}
		}

		TEST_METHOD(PositionLogicMatchesDoubleMathForEveryFactorAndSiting)
		{
			for (int f : { 1, 2, 4 })
				for (int s4 = -8; s4 <= 8; ++s4)
					for (int x = -9; x < 70; ++x)
					{
						int i0 = 0, ph = 0;
						Locate(x, f, s4, i0, ph);
						const double j = (x + 0.5 - s4 / 4.0) / f - 0.5;
						Assert::AreEqual(static_cast<int>(std::floor(j)), i0);
						Assert::AreEqual(static_cast<int>(std::floor((j - std::floor(j)) * 4 * f + 0.5)), ph);
						Assert::IsTrue(ph >= 0 && ph < 4 * f);
					}
			// The siting helper: libplacebo shifts (luma-pixel units).
			double sx = 9, sy = 9;
			ShiftOf(Siting::Left, 2, 1, sx, sy); Assert::AreEqual(-0.5, sx); Assert::AreEqual(0.0, sy);
			ShiftOf(Siting::Left, 2, 2, sx, sy); Assert::AreEqual(-0.5, sx); Assert::AreEqual(0.0, sy);
			ShiftOf(Siting::Center, 2, 2, sx, sy); Assert::AreEqual(0.0, sx); Assert::AreEqual(0.0, sy);
			ShiftOf(Siting::TopLeft, 2, 2, sx, sy); Assert::AreEqual(-0.5, sx); Assert::AreEqual(-0.5, sy);
			ShiftOf(Siting::Top, 2, 2, sx, sy); Assert::AreEqual(0.0, sx); Assert::AreEqual(-0.5, sy);
			ShiftOf(Siting::BottomLeft, 2, 2, sx, sy); Assert::AreEqual(-0.5, sx); Assert::AreEqual(0.5, sy);
			ShiftOf(Siting::Left, 4, 1, sx, sy); Assert::AreEqual(-1.5, sx, L"co-sited at factor 4");
			ShiftOf(Siting::Left, 1, 1, sx, sy); Assert::AreEqual(0.0, sx);
		}

		TEST_METHOD(CositedLeftOutputsAtTheSampleSitesAreTheInputExactly)
		{
			const Plane16 c = RandomPlane(20, 12, 7);
			const Geometry g = Geo(40, 12, 2, 1, Siting::Left);   // 4:2:2, DV-tunnel IPT siting
			const Plane16 o = UpsamplePlane(c, g);
			for (int y = 0; y < 12; ++y)
				for (int j = 0; j < 20; ++j)
					Assert::AreEqual(int(c.at(j, y)), int(o.at(2 * j, y)));
			// Co-sited vertical (4:2:0 top-left): rows 2k of the output equal the chroma rows exactly in the columns at the sites.
			const Plane16 c2 = RandomPlane(10, 10, 9);
			const Geometry g2 = Geo(20, 20, 2, 2, Siting::TopLeft);
			const Plane16 o2 = UpsamplePlane(c2, g2);
			for (int j = 0; j < 10; ++j)
				for (int i = 0; i < 10; ++i)
					Assert::AreEqual(int(c2.at(i, j)), int(o2.at(2 * i, 2 * j)));
		}

		TEST_METHOD(SitingIsHonouredOnALinearRamp)
		{
			// A linear ramp is reproduced (within ~2% of one chroma step) at the coordinate the siting says: co-sited left puts output x at
			// chroma x / 2, centred puts it at (x - 0.5) / 2. A wrong siting shifts the ramp by a quarter chroma step (160 codes).
			Plane16 c(24, 4);
			for (int y = 0; y < 4; ++y)
				for (int j = 0; j < 24; ++j) c.at(j, y) = static_cast<uint16_t>(10000 + 640 * j);
			for (Siting s : { Siting::Left, Siting::Center })
			{
				const Geometry g = Geo(48, 4, 2, 1, s);
				const Plane16 o = UpsamplePlane(c, g);
				const double sx = g.s4x / 4.0;
				for (int x = 8; x < 40; ++x)   // interior: the taps do not reach the clamped edge
				{
					const double j = (x + 0.5 - sx) / 2 - 0.5;
					Assert::AreEqual(10000.0 + 640.0 * j, double(o.at(x, 1)), 20.0);   // windowed sinc is not exact on a ramp (~13 codes); a wrong siting is 160
				}
			}
		}

		TEST_METHOD(CentredSitingIsMirrorSymmetric)
		{
			const Plane16 c = RandomPlane(16, 8, 21, 8000, 56000);
			Plane16 rev(16, 8);
			for (int y = 0; y < 8; ++y)
				for (int j = 0; j < 16; ++j) rev.at(15 - j, y) = c.at(j, y);
			const Geometry g = Geo(32, 8, 2, 1, Siting::Center);
			const Plane16 a = UpsamplePlane(c, g), b = UpsamplePlane(rev, g);
			for (int y = 0; y < 8; ++y)
				for (int x = 0; x < 32; ++x)
					Assert::IsTrue(std::abs(int(a.at(x, y)) - int(b.at(31 - x, y))) <= 4, L"mirror within the Q14 remainder");
		}

		TEST_METHOD(KernelMatchesADoublePrecisionReference)
		{
			struct Case { int w, h, fx, fy; Siting s; };
			const Case cases[] = { { 64, 48, 2, 2, Siting::Left }, { 64, 48, 2, 2, Siting::TopLeft }, { 64, 48, 2, 1, Siting::Left },
				{ 64, 48, 2, 2, Siting::Center }, { 64, 40, 4, 1, Siting::Left }, { 65, 49, 2, 2, Siting::Left } };
			for (const Case& k : cases)
			{
				const Geometry g = Geo(k.w, k.h, k.fx, k.fy, k.s);
				const Plane16 c = SmoothPlane(g.chromaW(), g.chromaH(), 5);
				const Plane16 o = UpsamplePlane(c, g);
				double worst = 0;
				for (int y = 0; y < k.h; ++y)
					for (int x = 0; x < k.w; ++x)
						worst = (std::max)(worst, std::fabs(RefAt(c, g, x, y) - o.at(x, y)));
				LogLine("basic chroma vs double reference " + std::to_string(k.w) + "x" + std::to_string(k.h) + " f" + std::to_string(k.fx) +
					"x" + std::to_string(k.fy) + " " + Token(k.s) + ": worst " + std::to_string(worst) + " codes");
				Assert::IsTrue(worst <= 4.0, L"Q14 weights + integer rounding stay within 4 codes (16-bit container) of the exact kernel");
			}
		}

		TEST_METHOD(FlatPlanesStayExactlyFlatAndOvershootIsKeptNotClippedToTheVideoRange)
		{
			const Geometry g = Geo(32, 16, 2, 2, Siting::Left);
			const Plane16 flat(16, 8, 33333);
			for (uint16_t v : UpsamplePlane(flat, g).v) Assert::AreEqual(33333, int(v));
			// A hard step from black-ish (10-bit code 64 << 6) to white-ish (940 << 6): the lanczos ring leaves the video range and the
			// container passes it (BTB / WTW preserved); the only clamp is the 16-bit container.
			Plane16 step(16, 8);
			for (int y = 0; y < 8; ++y)
				for (int j = 0; j < 16; ++j) step.at(j, y) = static_cast<uint16_t>(j < 8 ? 64 * 64 : 940 * 64);
			const Plane16 o = UpsamplePlane(step, g);
			int lo = 65535, hi = 0;
			for (uint16_t v : o.v) { lo = (std::min)(lo, int(v)); hi = (std::max)(hi, int(v)); }
			Assert::IsTrue(hi > 940 * 64, L"overshoot above the video white kept");
			Assert::IsTrue(lo < 64 * 64, L"undershoot below the video black kept");
			Assert::IsTrue(hi <= 65535 && lo >= 0);
			// Full-range extremes saturate at the container, never wrap.
			Plane16 ext(16, 8);
			for (int y = 0; y < 8; ++y)
				for (int j = 0; j < 16; ++j) ext.at(j, y) = static_cast<uint16_t>((j & 1) ? 65535 : 0);
			{
				const Plane16 eo = UpsamplePlane(ext, g);
				int sat = 0;
				for (uint16_t v : eo.v) sat += (v == 0 || v == 65535);
				Assert::IsTrue(sat > 0, L"alternating extremes overshoot and saturate at the container instead of wrapping");
			}
		}

		TEST_METHOD(BarsStayFlatAndNothingReadsAcrossThePictureBoundary)
		{
			const int W = 48, H = 64, fx = 2, fy = 2;
			const Rect active = { 0, 16, 48, 48 };   // letterbox: bars above and below (rows 0-15 and 48-63)
			const Geometry full = Geo(W, H, fx, fy, Siting::TopLeft, active);
			Plane16 c(W / fx, H / fy, 32768);          // bar chroma = a flat code
			const Plane16 pic = SmoothPlane(W / fx, (48 - 16) / fy, 3);
			for (int y = 0; y < pic.h; ++y)
				for (int x = 0; x < pic.w; ++x) c.at(x, 8 + y) = pic.at(x, y);
			const Plane16 o = UpsamplePlane(c, full);
			for (int y = 0; y < H; ++y)
				if (y < 16 || y >= 48)
					for (int x = 0; x < W; ++x) Assert::AreEqual(32768, int(o.at(x, y)), L"a flat bar stays exactly its code");
			// The picture rows equal the upsample of the picture on its own (the crop starts on an even luma row: same phases).
			const Geometry alone = Geo(W, 32, fx, fy, Siting::TopLeft);
			const Plane16 oa = UpsamplePlane(pic, alone);
			for (int y = 0; y < 32; ++y)
				for (int x = 0; x < W; ++x) Assert::AreEqual(int(oa.at(x, y)), int(o.at(x, 16 + y)));
		}

		TEST_METHOD(IptComposed12BitFourTwoTwoUpsamplesPAndTLikeAnyChromaPair)
		{
			// DV-tunnel IPT 12-bit (composed): I full size, P and T 4:2:2 with the horizontal siting co-sited (left); 12-bit codes in
			// the 16-bit container (<< 4). The filter is code-agnostic: P and T come out at I's size, exactly at the sites.
			const int W = 64, H = 16;
			Plane16 p(W / 2, H), t(W / 2, H);
			std::mt19937 g(31);
			for (size_t i = 0; i < p.v.size(); ++i)
			{
				p.v[i] = static_cast<uint16_t>((2048 + static_cast<int>(g() % 1500)) << 4);
				t.v[i] = static_cast<uint16_t>((1700 + static_cast<int>(g() % 900)) << 4);
			}
			const Geometry geo = Geo(W, H, 2, 1, Siting::Left);
			const Result r = Upsample(p, t, geo);
			Assert::AreEqual(std::string("ok"), r.status);
			Assert::AreEqual(W, r.cb.w);
			Assert::AreEqual(H, r.cb.h);
			for (int y = 0; y < H; ++y)
				for (int j = 0; j < W / 2; ++j)
				{
					Assert::AreEqual(int(p.at(j, y)), int(r.cb.at(2 * j, y)));
					Assert::AreEqual(int(t.at(j, y)), int(r.cr.at(2 * j, y)));
				}
			double worst = 0;
			for (int y = 0; y < H; ++y)
				for (int x = 0; x < W; ++x) worst = (std::max)(worst, std::fabs(RefAt(p, geo, x, y) - r.cb.at(x, y)));
			Assert::IsTrue(worst <= 5.0, L"noise of +-12000 codes: the Q14 table error is the dominant term");
		}

		TEST_METHOD(UnsupportedGeometriesAreRefusedWithAReason)
		{
			const Plane16 c(8, 8, 1000);
			Geometry g = Geo(16, 16, 2, 2, Siting::Left);
			Assert::AreEqual(std::string("chroma upscale: chroma plane size does not match the luma size and factors"),
				Upsample(Plane16(7, 8), Plane16(7, 8), g).status);
			Geometry three = g; three.fx = 3;
			Assert::AreEqual(std::string("chroma upscale: subsampling factor not 1, 2 or 4"), Upsample(c, c, three).status);
			std::string why;
			const Geometry odd = MakeGeometry(16, 16, 2, 2, -0.3, 0.0, Rect(), &why);
			const Geometry quarter = MakeGeometry(16, 16, 2, 2, -0.25, 0.75, Rect(), &why);   // a re-based shift: accepted
			Assert::AreEqual(std::string(), Check(quarter));
			Assert::AreEqual(std::string("siting shift is not a multiple of a quarter luma pixel"), why);
			Assert::AreEqual(std::string("siting shift is not a multiple of a quarter luma pixel"), Check(odd));
			Assert::AreEqual(std::string("invalid plane sizes"), Check(Geometry()));
			// 4:4:4: nothing to upsample, the planes come back untouched.
			const Geometry same = Geo(8, 8, 1, 1, Siting::Center);
			const Result r = Upsample(c, c, same);
			Assert::AreEqual(std::string("4:4:4: nothing to upsample"), r.status);
			Assert::IsTrue(r.cb.v == c.v);
			// Odd luma sizes: chroma is ceil(w / f); the last chroma column is taken as the plane edge.
			const Geometry oddG = Geo(17, 9, 2, 2, Siting::Left);
			Assert::AreEqual(9, oddG.chromaW());
			Assert::AreEqual(5, oddG.chromaH());
			const Plane16 oc = UpsamplePlane(RandomPlane(9, 5, 4), oddG);
			Assert::AreEqual(17, oc.w);
			Assert::AreEqual(9, oc.h);
			// UpsamplePlane guards the plane against the geometry (it is a public entry): wrong size or a short sample vector makes no picture.
			Assert::AreEqual(0, UpsamplePlane(RandomPlane(8, 5, 4), oddG).w);
			Assert::AreEqual(0, UpsamplePlane(RandomPlane(9, 4, 4), oddG).w);
			Assert::AreEqual(0, UpsamplePlane(Plane16(), oddG).w);
			Plane16 shortPlane = RandomPlane(9, 5, 4);
			shortPlane.v.resize(10);
			Assert::AreEqual(0, UpsamplePlane(shortPlane, oddG).w);
			const Geometry sameG = Geo(8, 8, 1, 1, Siting::Center);
			Assert::AreEqual(0, UpsamplePlane(Plane16(4, 4), sameG).w, L"4:4:4 copies only a plane of the right size");
			Assert::AreEqual(8, UpsamplePlane(Plane16(8, 8, 7), sameG).w);
		}

		TEST_METHOD(TheEngineReadPathIsInertWhileNotRequestedAndVisibleWhenRequestedWithoutProducts)
		{
			const Geometry g = Geo(32, 16, 2, 2, Siting::Left);
			const Plane16 c1 = RandomPlane(16, 8, 1), c2 = RandomPlane(16, 8, 2);
			const Result plain = Upsample(c1, c2, g);
			Assert::IsFalse(plain.td.requested || plain.td.usable || plain.td.fallback.engaged);
			Assert::AreEqual(std::string(), TdProducts::StatusLine(TdProducts::Consumer::ChromaStage, plain.td));
			const Result asked = Upsample(c1, c2, g, TdProducts::PlaceholderSnapshot(), true, 0);
			Assert::IsTrue(asked.td.fallback.engaged);
			Assert::AreEqual(TdProducts::FallbackKey(TdProducts::Consumer::ChromaStage, TdProducts::kCauseAbsent), asked.td.fallback.id, L"keyed per id + cause, not the bare id");
			Assert::IsTrue(asked.cb.v == plain.cb.v && asked.cr.v == plain.cr.v, L"the plain kernel runs either way: byte-identical");
			const Result again = Upsample(c1, c2, g);
			Assert::IsTrue(again.cb.v == plain.cb.v && again.cr.v == plain.cr.v, L"deterministic");
		}
	};

	TEST_CLASS(BasicChromaGpuTests)
	{
	public:
		TEST_METHOD(GpuPassIsByteIdenticalToTheCpuModel)
		{
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"libplacebo D3D11 device");
			pl_gpu gpu = dev.Gpu();
			struct Case { const char* name; int w, h, fx, fy; Siting s; Rect active; int lo, hi; uint32_t seed; };
			const Case cases[] = {
				{ "4:2:0 left, full-range noise", 64, 48, 2, 2, Siting::Left, Rect(), 0, 65535, 1 },
				{ "4:2:0 top-left", 64, 48, 2, 2, Siting::TopLeft, Rect(), 0, 65535, 2 },
				{ "4:2:0 centre, video range", 66, 50, 2, 2, Siting::Center, Rect(), 4096, 60160, 3 },
				{ "4:2:2 left (IPT 12-bit codes << 4)", 64, 32, 2, 1, Siting::Left, Rect(), 1600 << 4, 3300 << 4, 4 },
				{ "4:1:1-style factor 4", 64, 24, 4, 1, Siting::Left, Rect(), 0, 65535, 5 },
				{ "odd luma size 4:2:0", 33, 19, 2, 2, Siting::Left, Rect(), 0, 65535, 6 },
				{ "letterbox bars", 48, 64, 2, 2, Siting::TopLeft, { 0, 16, 48, 48 }, 2000, 62000, 7 },
				{ "extremes (0 / 65535 alternating)", 32, 16, 2, 2, Siting::Left, Rect(), -1, -1, 8 },
				{ "4:4:4 on one axis only", 32, 32, 1, 2, Siting::Left, Rect(), 0, 65535, 9 },
			};
			for (const Case& k : cases)
			{
				const Geometry g = Geo(k.w, k.h, k.fx, k.fy, k.s, k.active);
				Plane16 a, b;
				if (k.lo < 0)
				{
					a = Plane16(g.chromaW(), g.chromaH()); b = a;
					for (int y = 0; y < a.h; ++y)
						for (int x = 0; x < a.w; ++x) { a.at(x, y) = static_cast<uint16_t>(((x + y) & 1) ? 65535 : 0); b.at(x, y) = static_cast<uint16_t>(((x + y) & 1) ? 0 : 65535); }
				}
				else
				{
					a = RandomPlane(g.chromaW(), g.chromaH(), k.seed, k.lo, k.hi);
					b = RandomPlane(g.chromaW(), g.chromaH(), k.seed + 100, k.lo, k.hi);
				}
				pl_tex src = UploadRg16(gpu, a, b);
				Assert::IsNotNull(src);
				Gpu::Module module;
				pl_tex out = nullptr;
				const bool ok = module.Apply(gpu, dev.dispatch, src, g, out);
				Assert::IsTrue(ok, std::wstring(module.LastError().begin(), module.LastError().end()).c_str());
				Plane16 ga, gb;
				Assert::IsTrue(DownloadRg16(gpu, out, ga, gb));
				const Plane16 ca = UpsamplePlane(a, g), cb = UpsamplePlane(b, g);
				const size_t m0 = Mismatches(ga, ca), m1 = Mismatches(gb, cb);
				LogLine(std::string("basic chroma GPU vs CPU, ") + k.name + ": mismatching bytes " + std::to_string(m0) + " + " + std::to_string(m1) +
					" of " + std::to_string(ca.v.size() * 2));
				Assert::AreEqual(size_t(0), m0, L"first plane byte-identical");
				Assert::AreEqual(size_t(0), m1, L"second plane byte-identical");
				module.Release(gpu);
				pl_tex_destroy(gpu, &src);
			}
		}

		TEST_METHOD(GpuRefusesWhatTheCpuRefuses)
		{
			GpuDevice dev;
			Assert::IsTrue(dev.Create());
			pl_gpu gpu = dev.Gpu();
			Gpu::Module module;
			pl_tex out = nullptr;
			Geometry bad = Geo(16, 16, 2, 2, Siting::Left);
			bad.fx = 3;
			const Plane16 a(8, 8, 100);
			pl_tex src = UploadRg16(gpu, a, a);
			Assert::IsFalse(module.Apply(gpu, dev.dispatch, src, bad, out));
			Assert::AreEqual(std::string("subsampling factor not 1, 2 or 4"), module.LastError());
			const Geometry g = Geo(32, 32, 2, 2, Siting::Left);   // chroma 16 x 16, the texture is 8 x 8
			Assert::IsFalse(module.Apply(gpu, dev.dispatch, src, g, out));
			Assert::IsFalse(module.LastError().empty());
			pl_tex_destroy(gpu, &src);
		}

		TEST_METHOD(GlslArithmeticIsIntegerOnly)
		{
			const std::string header = Gpu::BuildHeader(Geo(64, 48, 2, 2, Siting::Left));
			const std::string body = Gpu::Body();
			// No float literal reaches the filter sums: the weights are int tables, the only float is the texel decode / encode.
			Assert::IsTrue(header.find("const int BCX_W[") != std::string::npos && header.find("const int BCY_W[") != std::string::npos);
			Assert::IsTrue(header.find("const float") == std::string::npos);
			Assert::IsTrue(body.find("accH") != std::string::npos && body.find(">> 14") != std::string::npos);
			Assert::IsTrue(body.find("floor(t * 65535.0 + 0.5)") != std::string::npos);
		}
	};
}
