// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// scaling.shared: ScalerPass. ScalerModelTests exercise the table, the geometry and the CPU reference sampler
// (no GPU). ScalerPassGpuTests compare the GPU pass with that reference and with today's users' libplacebo call
// sequences (migration identity) on a libplacebo device: WARP (software, CPU tier) unless VP_TEST_GPU=hardware.
// The timing test is hardware only (category GpuHardware).
#include "pch.h"
#include "CppUnitTest.h"
#include <VP-V/tests/support/ExtendedRangeGpuProbe.h>

#include <VP-V/modules/scaling/rebase/KernelInverse.h>
#include <VP-V/modules/scaling/rebase/SourceRebase.h>
#include <VP-V/modules/scaling/chroma_upscale/ChromaCfl.h>
#include <VP-V/modules/scaling/shared/KernelTable.h>
#include <VP-V/modules/scaling/shared/ScalerGeometry.h>
#include <VP-V/modules/scaling/shared/ScalerPass.h>
#include <VP-V/modules/scaling/shared/ScalerPassFixed.h>
#include <VP-V/modules/scaling/shared/ScalerReference.h>

#include <libplacebo/d3d11.h>
#include <libplacebo/dispatch.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace ScalerPassTestsNs
{
	using ScalerKernels::Kind;
	using ScalerKernels::Spec;
	using ScalerReference::Image;

	// The brief's ratios (upscale factors), incl. unequal axes.
	const double kFactors[5] = { 1.48, 2.0, 2.22, 3.0, 3.75 };

	void Log(const std::string& s) { Logger::WriteMessage(s.c_str()); }
	std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }

	Spec Lanczos(int r) { Spec s; s.kind = Kind::Lanczos; s.radius = r; return s; }
	Spec Plain(Kind k) { Spec s; s.kind = k; return s; }

	// A textured picture (edges + ripple), values 0..1.
	Image MakeImage(int w, int h)
	{
		Image im(w, h);
		for (int y = 0; y < h; ++y)
			for (int x = 0; x < w; ++x)
			{
				const double ripple = 0.15 * std::sin(0.9 * x + 0.3) * std::cos(0.7 * y);
				const double edge = ((x / 7 + y / 5) & 1) ? 0.62 : 0.28;
				im.at(x, y) = (std::min)(1.0, (std::max)(0.0, edge + ripple));
			}
		return im;
	}

	double MaxDiff(const Image& a, const Image& b)
	{
		double m = 0.0;
		for (size_t i = 0; i < a.v.size(); ++i) m = (std::max)(m, std::fabs(a.v[i] - b.v[i]));
		return m;
	}

	TEST_CLASS(ScalerModelTests)
	{
	public:
		TEST_METHOD(ScalerKernelTableMatchesKernelInverseTaps)
		{
			const auto& inv = KernelInverse::Candidates();
			size_t seen = 0;
			for (const KernelInverse::Candidate& c : inv)
			{
				const int idx = ScalerKernels::Find(c.name);
				Assert::IsTrue(idx >= 0, Wide("table lacks KernelInverse candidate " + c.name).c_str());
				const ScalerKernels::Entry& e = ScalerKernels::Table()[static_cast<size_t>(idx)];
				Assert::IsTrue(e.inKernelInverse, L"flagged as a KernelInverse entry");
				for (int p = 0; p < 2; ++p)
				{
					const ScalerKernels::PhaseTaps t = ScalerKernels::Phase(e.spec, p == 0 ? -0.25 : 0.25);
					Assert::AreEqual(c.phaseFirst, t.first, Wide(c.name + " first tap").c_str());
					Assert::AreEqual(c.phase[p].size(), t.taps.size(), Wide(c.name + " tap count").c_str());
					for (size_t i = 0; i < t.taps.size(); ++i)
						Assert::IsTrue(c.phase[p][i] == t.taps[i], Wide(c.name + " tap value (bit for bit)").c_str());
				}
				++seen;
			}
			Assert::AreEqual(static_cast<size_t>(7), seen);
		}

		TEST_METHOD(ScalerKernelTableOrderAndExtras)
		{
			const auto& t = ScalerKernels::Table();
			const auto& inv = KernelInverse::Candidates();
			Assert::IsTrue(t.size() >= inv.size() + 2);
			for (size_t i = 0; i < inv.size(); ++i) Assert::AreEqual(inv[i].name, t[i].name);
			Assert::IsTrue(ScalerKernels::Find("hermite") >= 0);
			Assert::IsTrue(ScalerKernels::IsPolar(t[static_cast<size_t>(ScalerKernels::Find("ewa_lanczossharp"))].spec.kind));
			Assert::AreEqual(-1, ScalerKernels::Find("no such kernel"));
		}

		TEST_METHOD(ScalerCatmullRomTapWeightsSharedWithChromaCfl)
		{
			for (int i = 0; i <= 16; ++i)
			{
				const double t = i / 16.0;
				double a[4], b[4];
				ScalerKernels::CatmullRomTapWeights(t, a);
				ChromaUpscale::CatmullRomWeights(t, b);
				double sum = 0.0;
				for (int k = 0; k < 4; ++k)
				{
					Assert::IsTrue(a[k] == b[k], L"chroma CfL weights are the shared table weights (bit for bit)");
					sum += a[k];
					const double viaKernel = ScalerKernels::Cubic(0.0, 0.5, std::fabs(t - (k - 1)));
					Assert::IsTrue(std::fabs(a[k] - viaKernel) < 1e-12, L"tap weight equals Cubic(0,0.5) at its distance");
				}
				Assert::IsTrue(std::fabs(sum - 1.0) < 1e-12, L"Catmull-Rom taps sum to one");
			}
		}

		TEST_METHOD(ScalerGeometryDerivesRatiosPerAxis)
		{
			ScalerGeometry::Span x{ 0.0f, 720.0f, 1920 }, y{ 0.0f, 480.0f, 1080 };   // SD, non-square pixels
			Assert::AreEqual(0.375, x.Ratio(), 1e-12);
			Assert::AreEqual(480.0 / 1080.0, y.Ratio(), 1e-12);
			Assert::IsTrue(x.Scales() && y.Scales());
			Assert::IsTrue(ScalerGeometry::Unequal(x.Ratio(), y.Ratio()));
			ScalerGeometry::Span same{ 10.0f, 30.0f, 20 };
			Assert::IsFalse(same.Scales());
			Assert::IsFalse(ScalerGeometry::Unequal(0.5, 0.52));
		}

		TEST_METHOD(ScalerGeometryIntegerWindow)
		{
			const ScalerGeometry::Window w = ScalerGeometry::IntegerWindow(2.5f, 98.5f);
			Assert::AreEqual(2, w.origin);
			Assert::AreEqual(97, w.size);
			const ScalerGeometry::Window whole = ScalerGeometry::IntegerWindow(0.0f, 144.0f);
			Assert::AreEqual(0, whole.origin);
			Assert::AreEqual(144, whole.size);
		}

		TEST_METHOD(ScalerReferenceWeightsSumToOneAndFlatStaysFlat)
		{
			const Spec specs[5] = { Lanczos(3), Plain(Kind::CatmullRom), Plain(Kind::Hermite), Plain(Kind::Bilinear), Lanczos(2) };
			const Image flat(50, 40, 0.4375);
			for (const Spec& s : specs)
				for (double fx : kFactors)
					for (double fy : kFactors)   // includes every unequal pair
					{
						const int dw = static_cast<int>(std::lround(50 * fx)), dh = static_cast<int>(std::lround(40 * fy));
						const Image o = ScalerReference::Resample(flat, dw, dh, s);
						for (double v : o.v) Assert::AreEqual(0.4375, v, 1e-12);
					}
			std::vector<double> w;
			int first = 0;
			ScalerReference::Taps(Lanczos(3), 0.0, 1.0 / 2.22, 17, w, first);
			double sum = 0.0;
			for (double v : w) sum += v;
			Assert::AreEqual(1.0, sum, 1e-12);
			ScalerReference::Taps(Lanczos(3), 0.0, 3.75, 4, w, first);   // downscale: support stretched by the ratio
			Assert::IsTrue(w.size() >= static_cast<size_t>(2 * 3 * 3.75) - 1);
		}

		TEST_METHOD(ScalerReferenceUnityRatioIsIdentity)
		{
			const Image im = MakeImage(40, 30);
			const Spec specs[3] = { Lanczos(3), Plain(Kind::CatmullRom), Plain(Kind::Hermite) };
			for (const Spec& s : specs)
				Assert::IsTrue(MaxDiff(im, ScalerReference::Resample(im, 40, 30, s)) < 1e-9);
		}

		TEST_METHOD(ScalerReferenceBilinearStaysWithinInputRange)
		{
			Image step(16, 1, 0.1);
			for (int x = 8; x < 16; ++x) step.at(x, 0) = 0.9;
			const Image o = ScalerReference::Resample(step, 48, 1, Plain(Kind::Bilinear));
			for (double v : o.v) Assert::IsTrue(v >= 0.1 - 1e-12 && v <= 0.9 + 1e-12);
			Assert::AreEqual(0.1, o.at(0, 0), 1e-12);   // clamp to edge
			Assert::AreEqual(0.9, o.at(47, 0), 1e-12);
		}

		TEST_METHOD(ScalerReferenceSitingPhaseShiftsTheSpan)
		{
			const Image im = MakeImage(20, 1);
			const Image o = ScalerReference::ResampleAxis(im, true, 0.0, 20.0, 20, Lanczos(3), 1.0);   // one texel
			for (int x = 0; x < 19; ++x) Assert::AreEqual(im.at(x + 1, 0), o.at(x, 0), 1e-9);
			Assert::AreEqual(im.at(19, 0), o.at(19, 0), 1e-9);   // clamp to edge
		}

		TEST_METHOD(ScalerReferenceAxisOrderAgrees)
		{
			const Image im = MakeImage(37, 29);
			const Image a = ScalerReference::Resample(im, 82, 107, Lanczos(3), false);
			const Image b = ScalerReference::Resample(im, 82, 107, Lanczos(3), true);
			Assert::IsTrue(MaxDiff(a, b) < 1e-9);
		}

		// Category Cpu: the body is the CPU ScalerReference model only (no device); the GPU section follows the class.
		BEGIN_TEST_METHOD_ATTRIBUTE(ScalerReferenceDeterministic)
			TEST_METHOD_ATTRIBUTE(L"Category", L"Cpu")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(ScalerReferenceDeterministic)
		{
			const Image im = MakeImage(33, 21);
			const Image a = ScalerReference::Resample(im, 70, 50, Lanczos(4));
			const Image b = ScalerReference::Resample(im, 70, 50, Lanczos(4));
			Assert::IsTrue(a.v == b.v);
		}

		// ---- CF-2 B5a-2 step 2: the Q14 fixed-point mode (ScalerPassFixed.h) --------------------------------------------------
		// GPU rows the W4 build must prove: BasicChromaGpuTests' CPU == GPU byte comparison (unchanged) now runs the GLSL this
		// mode generates ("BC" prefix); the text equality to the pre-step-2 GLSL was checked by emulation at authoring time.

		// The pre-step-2 BasicChroma loops, written out independently (6 taps a = 3, int32 horizontal intermediate unclamped, the
		// container clamp at the end), as the reference the shared mode must reproduce exactly.
		// Gate CF2 S3: the phase tables here come from F::BuildPhaseTable, so this reference is independent for the integer
		// loops only. The tables' independence rests on BasicChromaTests PhaseTablesFromTheSharedKernelTableEqualTheLegacyQ14Tables
		// (the kept private Lanczos(d) against the shared KernelTable).
		static std::vector<uint16_t> LegacyQ14(const std::vector<uint16_t>& c, const ScalerPass::FixedQ14::Axis& ax,
			const ScalerPass::FixedQ14::Axis& ay)
		{
			namespace F = ScalerPass::FixedQ14;
			const F::PhaseTable tx = F::BuildPhaseTable(Lanczos(3), ax.factor), ty = F::BuildPhaseTable(Lanczos(3), ay.factor);
			const int W = ax.out, H = ay.out, cw = ax.in, ch = ay.in;
			std::vector<int> hrow(static_cast<size_t>(W) * ch);
			for (int x = 0; x < W; ++x)
			{
				const int n = 4 * x + 2 - ax.s4 - 2 * ax.factor, den = 4 * ax.factor;
				const int i0 = static_cast<int>(std::floor(static_cast<double>(n) / den)), ph = n - i0 * den;
				int lo, hi;
				if (x >= ax.L && x < ax.R) { lo = ax.cL; hi = ax.cR - 1; }
				else if (x < ax.L) { lo = 0; hi = (std::max)(ax.cL - 1, 0); }
				else { lo = (std::min)(ax.cR, cw - 1); hi = cw - 1; }
				for (int y = 0; y < ch; ++y)
				{
					int acc = 0;
					for (int k = 0; k < 6; ++k)
						acc += static_cast<int>(c[static_cast<size_t>(y) * cw + (std::min)((std::max)(i0 - 2 + k, lo), hi)]) * tx.w[ph][k];
					hrow[static_cast<size_t>(y) * W + x] = (acc + 8192) >> 14;
				}
			}
			std::vector<uint16_t> out(static_cast<size_t>(W) * H);
			for (int y = 0; y < H; ++y)
			{
				const int n = 4 * y + 2 - ay.s4 - 2 * ay.factor, den = 4 * ay.factor;
				const int j0 = static_cast<int>(std::floor(static_cast<double>(n) / den)), ph = n - j0 * den;
				int lo, hi;
				if (y >= ay.L && y < ay.R) { lo = ay.cL; hi = ay.cR - 1; }
				else if (y < ay.L) { lo = 0; hi = (std::max)(ay.cL - 1, 0); }
				else { lo = (std::min)(ay.cR, ch - 1); hi = ch - 1; }
				for (int x = 0; x < W; ++x)
				{
					int acc = 0;
					for (int k = 0; k < 6; ++k)
						acc += hrow[static_cast<size_t>((std::min)((std::max)(j0 - 2 + k, lo), hi)) * W + x] * ty.w[ph][k];
					out[static_cast<size_t>(y) * W + x] = static_cast<uint16_t>((std::min)((std::max)((acc + 8192) >> 14, 0), 65535));
				}
			}
			return out;
		}

		static ScalerPass::FixedQ14::Axis FixedAxis(int out, int factor, int s4, int L, int R)
		{
			ScalerPass::FixedQ14::Axis a;
			a.out = out; a.factor = factor; a.in = (out + factor - 1) / factor; a.s4 = s4;
			a.L = L; a.R = R;
			a.cL = (std::max)(0, (std::min)(a.in, L / factor));
			a.cR = (std::max)(0, (std::min)(a.in, (R + factor - 1) / factor));
			return a;
		}

		// A hard-edged plane with BTB / WTW codes and the container extremes (overshoot reaches both container ends).
		static std::vector<uint16_t> FixedPlane(int w, int h)
		{
			std::vector<uint16_t> v(static_cast<size_t>(w) * h);
			const uint16_t codes[6] = { 0, 1000, 4096, 40000, 63000, 65535 };   // 0, BTB, 10-bit black << 6, mid, WTW, container top
			for (int y = 0; y < h; ++y)
				for (int x = 0; x < w; ++x)
					v[static_cast<size_t>(y) * w + x] = codes[((x / 2) * 7 + (y / 3) * 5 + x * y) % 6];
			return v;
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(FixedQ14ReproducesTheBasicChromaIntegerLoops)
			TEST_METHOD_ATTRIBUTE(L"Category", L"Cpu")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(FixedQ14ReproducesTheBasicChromaIntegerLoops)
		{
			// Factors 1 / 2 / 4 incl. unequal axes, co-sited / centred / bottom shifts, bars on both axes and none.
			struct Case { int W, H, fx, fy, s4x, s4y, ax0, ax1, ay0, ay1; };
			const Case cases[] = {
				{ 37, 23, 2, 2, -2, -2, 0, 37, 0, 23 },
				{ 37, 23, 2, 2, 0, 0, 5, 31, 3, 20 },
				{ 40, 24, 2, 1, -2, 0, 4, 36, 0, 24 },
				{ 41, 18, 4, 1, -6, 0, 6, 35, 2, 16 },
				{ 33, 29, 4, 2, 0, 2, 0, 33, 4, 25 },
				{ 19, 34, 1, 4, 0, -6, 0, 19, 0, 34 },
			};
			bool hitFloor = false, hitTop = false;
			for (const Case& k : cases)
			{
				const ScalerPass::FixedQ14::Axis ax = FixedAxis(k.W, k.fx, k.s4x, k.ax0, k.ax1);
				const ScalerPass::FixedQ14::Axis ay = FixedAxis(k.H, k.fy, k.s4y, k.ay0, k.ay1);
				const std::vector<uint16_t> src = FixedPlane(ax.in, ay.in);
				std::vector<uint16_t> got;
				Assert::IsTrue(ScalerPass::FixedQ14::Upsample(src.data(), src.size(), ax, ay, Lanczos(3), got));
				Assert::IsTrue(got == LegacyQ14(src, ax, ay), Wide("fixed mode == legacy loops, " + std::to_string(k.W) + "x" +
					std::to_string(k.H) + " f " + std::to_string(k.fx) + "," + std::to_string(k.fy)).c_str());
				hitFloor = hitFloor || std::find(got.begin(), got.end(), uint16_t(0)) != got.end();
				hitTop = hitTop || std::find(got.begin(), got.end(), uint16_t(65535)) != got.end();
			}
			Assert::IsTrue(hitFloor && hitTop, L"overshoot is clamped to the 16-bit container only");
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(FixedQ14FlatBtbWtwCodesPassExactly)
			TEST_METHOD_ATTRIBUTE(L"Category", L"Cpu")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(FixedQ14FlatBtbWtwCodesPassExactly)
		{
			// Every phase sums to exactly 16384: a flat plane at any code (BTB, WTW, 12-bit << 4) comes back unchanged, no
			// video-range clamp, for each separable kernel the mode takes.
			const Spec kernels[4] = { Lanczos(3), Lanczos(2), Plain(Kind::CatmullRom), Plain(Kind::Bilinear) };
			const uint16_t codes[5] = { 0, 1000, 3 << 4, 63000, 65535 };
			for (const Spec& sp : kernels)
				for (int f = 1; f <= 4; f *= 2)
				{
					const ScalerPass::FixedQ14::PhaseTable t = ScalerPass::FixedQ14::BuildPhaseTable(sp, f);
					Assert::AreEqual(ScalerPass::FixedQ14::TapsOf(sp), t.taps);
					for (int p = 0; p < t.phases; ++p)
					{
						int sum = 0;
						for (int k = 0; k < t.taps; ++k) sum += t.w[p][k];
						Assert::AreEqual(16384, sum, L"each phase sums to exactly 16384");
					}
					for (uint16_t code : codes)
					{
						const ScalerPass::FixedQ14::Axis ax = FixedAxis(21, f, 0, 0, 21), ay = FixedAxis(13, 2, -2, 0, 13);
						const std::vector<uint16_t> src(static_cast<size_t>(ax.in) * ay.in, code);
						std::vector<uint16_t> got;
						Assert::IsTrue(ScalerPass::FixedQ14::Upsample(src.data(), src.size(), ax, ay, sp, got));
						for (uint16_t v : got) Assert::AreEqual(static_cast<int>(code), static_cast<int>(v), L"flat stays flat at every code");
					}
				}
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(FixedQ14RefusesWhatItCannotRunVisibly)
			TEST_METHOD_ATTRIBUTE(L"Category", L"Cpu")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(FixedQ14RefusesWhatItCannotRunVisibly)
		{
			namespace F = ScalerPass::FixedQ14;
			const F::Axis ok = FixedAxis(20, 2, 0, 0, 20);
			const std::vector<uint16_t> src(10 * 10, 500);
			std::vector<uint16_t> got(3, 1);
			Assert::IsFalse(F::Upsample(src.data(), src.size(), ok, ok, Plain(Kind::EwaLanczosSharp), got), L"polar refused");
			Assert::IsTrue(got.empty());
			Assert::IsFalse(F::Upsample(src.data(), src.size(), ok, ok, Lanczos(4), got), L"8 taps refused (max 6)");
			Assert::IsFalse(F::Upsample(src.data(), src.size(), FixedAxis(20, 3, 0, 0, 20), ok, Lanczos(3), got), L"factor 3 refused");
			Assert::IsFalse(F::Upsample(src.data(), 99, ok, ok, Lanczos(3), got), L"short source refused");
			Assert::IsTrue(F::GlslHeader("VP", "vp", ok, ok, Plain(Kind::EwaLanczosSharp)).empty());
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(FixedQ14GlslIsIntegerOnlyForEveryTapCount)
			TEST_METHOD_ATTRIBUTE(L"Category", L"Cpu")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(FixedQ14GlslIsIntegerOnlyForEveryTapCount)
		{
			namespace F = ScalerPass::FixedQ14;
			const F::Axis ax = FixedAxis(64, 2, -2, 0, 64), ay = FixedAxis(48, 2, 0, 0, 48);
			const std::string h3 = F::GlslHeader("VP", "vp", ax, ay, Lanczos(3)), b3 = F::GlslBody("VP", "vp", Lanczos(3));
			const std::string h2 = F::GlslHeader("VP", "vp", ax, ay, Lanczos(2)), b2 = F::GlslBody("VP", "vp", Lanczos(2));
			Assert::IsTrue(h3.find("const int VPX_W[96]") != std::string::npos && h3.find("const int VPY_W[96]") != std::string::npos);
			Assert::IsTrue(h2.find("const int VPX_W[64]") != std::string::npos);
			Assert::IsTrue(h3.find("#define VP_S4X -2\n") != std::string::npos && h3.find("void vp_range(") != std::string::npos);
			for (const std::string* s : { &h3, &b3, &h2, &b2 })
			{
				Assert::IsTrue(s->find("const float") == std::string::npos, L"no float constant in the arithmetic");
				Assert::IsTrue(s->find('@') == std::string::npos, L"every template token filled");
			}
			Assert::IsTrue(b3.find("VPX_W[phx * 6 + tx]") != std::string::npos && b3.find("iy0 - 2 + ty") != std::string::npos);
			Assert::IsTrue(b2.find("VPX_W[phx * 4 + tx]") != std::string::npos && b2.find("iy0 - 1 + ty") != std::string::npos);
			Assert::IsTrue(b3.find(">> 14") != std::string::npos && b3.find("floor(t * 65535.0 + 0.5)") != std::string::npos &&
				b3.find("texelFetch(vp_src") != std::string::npos && b3.find("imageStore(vp_dst") != std::string::npos);
		}
	};

	// --- GPU side ---
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
			Log("scaler pass GPU tests on: " + name);
			d3d11 = pl_d3d11_create(log, &params);
			if (!d3d11) return false;
			dispatch = pl_dispatch_create(log, d3d11->gpu);
			return dispatch != nullptr;
		}
		pl_gpu Gpu() const { return d3d11->gpu; }
	};

	pl_tex UploadRaw(pl_gpu gpu, int w, int h, const char* format, const void* data)
	{
		pl_tex_params p{};
		p.w = w; p.h = h; p.format = pl_find_named_fmt(gpu, format); p.sampleable = true; p.initial_data = data;
		return pl_tex_create(gpu, &p);
	}

	// An image (0..1) as a 16-bit unorm single-channel texture, normalised raw / 65535 (what the sampler returns).
	pl_tex UploadR16(pl_gpu gpu, const Image& im, std::vector<uint16_t>* rawOut = nullptr)
	{
		std::vector<uint16_t> raw(im.v.size());
		for (size_t i = 0; i < raw.size(); ++i) raw[i] = static_cast<uint16_t>(std::lround((std::min)(1.0, (std::max)(0.0, im.v[i])) * 65535.0));
		if (rawOut) *rawOut = raw;
		return UploadRaw(gpu, im.w, im.h, "r16", raw.data());
	}

	// The image the GPU actually samples (the quantised raw values), as the reference input.
	Image FromRaw(const std::vector<uint16_t>& raw, int w, int h)
	{
		Image im(w, h);
		for (size_t i = 0; i < raw.size(); ++i) im.v[i] = raw[i] / 65535.0;
		return im;
	}

	// A texture read back as raw bytes (any host-readable format the pass produces).
	bool DownloadBytes(pl_gpu gpu, pl_tex tex, std::vector<uint8_t>& bytes)
	{
		const pl_fmt fmt = tex->params.format;
		pl_tex_params rp{};
		rp.w = tex->params.w; rp.h = tex->params.h; rp.format = fmt; rp.host_readable = true; rp.blit_dst = true;
		pl_tex readable = pl_tex_create(gpu, &rp);
		if (!readable) return false;
		pl_tex_blit_params bp{}; bp.src = tex; bp.dst = readable;
		pl_tex_blit(gpu, &bp);
		bytes.assign(static_cast<size_t>(rp.w) * rp.h * fmt->texel_size, 0);
		pl_tex_transfer_params d{}; d.tex = readable; d.ptr = bytes.data();
		const bool ok = pl_tex_download(gpu, &d);
		pl_tex_destroy(gpu, &readable);
		return ok;
	}

	// A one-component 32-bit float texture as an image.
	bool DownloadFloatImage(pl_gpu gpu, pl_tex tex, Image& im)
	{
		const pl_fmt fmt = tex->params.format;
		if (fmt->type != PL_FMT_FLOAT || fmt->component_depth[0] != 32 || fmt->num_components != 1) return false;
		std::vector<uint8_t> bytes;
		if (!DownloadBytes(gpu, tex, bytes)) return false;
		im = Image(tex->params.w, tex->params.h);
		for (size_t i = 0; i < im.v.size(); ++i)
		{
			float f;
			std::memcpy(&f, bytes.data() + i * sizeof(float), sizeof f);
			im.v[i] = f;
		}
		return true;
	}

	void SetRepr(pl_frame& f)
	{
		f.repr.sys = PL_COLOR_SYSTEM_BT_709; f.repr.levels = PL_COLOR_LEVELS_LIMITED;
		f.repr.bits.sample_depth = 16; f.repr.bits.color_depth = 10; f.repr.bits.bit_shift = 6;
		f.color = pl_color_space_bt709;
	}

	struct GpuCase
	{
		int sw, sh, dw, dh;
		ScalerPass::Axis axis;
	};

	TEST_CLASS(ScalerPassGpuTests)
	{
		static void CompareWithReference(GpuDevice& dev, const GpuCase& c, const Spec& spec, double tolerance, const char* name)
		{
			pl_gpu gpu = dev.Gpu();
			std::vector<uint16_t> raw;
			const Image src = MakeImage(c.sw, c.sh);
			pl_tex tex = UploadR16(gpu, src, &raw);
			Assert::IsNotNull(tex, L"source upload");
			const Image in = FromRaw(raw, c.sw, c.sh);
			ScalerPass::Module pass;
			ScalerPass::Settings st;
			st.filter = spec;
			st.axis = c.axis;
			st.precision = ScalerPass::Precision::F32;
			ScalerPass::Source s;
			s.tex = tex;
			pl_tex out = nullptr;
			const int argW = c.axis == ScalerPass::Axis::Y ? 0 : c.dw, argH = c.axis == ScalerPass::Axis::X ? 0 : c.dh;
			Assert::IsTrue(pass.Apply(gpu, dev.dispatch, s, argW, argH, st, out), Wide(pass.LastError()).c_str());
			Image got;
			Assert::IsTrue(DownloadFloatImage(gpu, out, got), L"readable f32 output");
			Image want;
			if (c.axis == ScalerPass::Axis::X) want = ScalerReference::ResampleAxis(in, true, 0.0, c.sw, c.dw, spec);
			else if (c.axis == ScalerPass::Axis::Y) want = ScalerReference::ResampleAxis(in, false, 0.0, c.sh, c.dh, spec);
			else want = ScalerReference::Resample(in, c.dw, c.dh, spec);
			Assert::AreEqual(want.w, got.w);
			Assert::AreEqual(want.h, got.h);
			const double worst = MaxDiff(want, got) * 1023.0;   // codes at 10-bit
			char line[200];
			std::snprintf(line, sizeof line, "%s: %dx%d -> %dx%d worst %.4f codes (10-bit)", name, c.sw, c.sh, got.w, got.h, worst);
			Log(line);
			Assert::IsTrue(worst <= tolerance, Wide(line).c_str());
			pass.Release(gpu);
			pl_tex_destroy(gpu, &tex);
		}

	public:
		TEST_METHOD(ScalerGpuParityAtTheBriefsRatios)
		{
			// CPU reference vs GPU within 1 code at 10-bit: each ratio on both axes, then unequal pairs, then downscales.
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			for (double f : kFactors)
			{
				const int dw = static_cast<int>(std::lround(48 * f)), dh = static_cast<int>(std::lround(32 * f));
				CompareWithReference(dev, { 48, 32, dw, dh, ScalerPass::Axis::Both }, Lanczos(3), 1.0, "lanczos3 both");
			}
			const double pairs[3][2] = { { 1.48, 3.75 }, { 2.0, 2.22 }, { 3.0, 1.48 } };
			for (const auto& p : pairs)
				CompareWithReference(dev, { 48, 32, static_cast<int>(std::lround(48 * p[0])), static_cast<int>(std::lround(32 * p[1])), ScalerPass::Axis::Both },
					Lanczos(3), 1.0, "lanczos3 unequal");
			CompareWithReference(dev, { 48, 32, 107, 32, ScalerPass::Axis::X }, Lanczos(3), 1.0, "lanczos3 x only");
			CompareWithReference(dev, { 48, 32, 48, 71, ScalerPass::Axis::Y }, Lanczos(3), 1.0, "lanczos3 y only");
			CompareWithReference(dev, { 96, 64, 48, 32, ScalerPass::Axis::Both }, Lanczos(3), 1.0, "lanczos3 down 2x");
			CompareWithReference(dev, { 96, 64, 26, 17, ScalerPass::Axis::Both }, Lanczos(3), 1.0, "lanczos3 down 3.7x");
			Spec cubic; cubic.kind = Kind::Bicubic; cubic.B = 0.3; cubic.C = 0.35;
			const Spec others[5] = { Plain(Kind::CatmullRom), cubic, Plain(Kind::Hermite), Plain(Kind::Bilinear), Lanczos(2) };
			for (const Spec& s : others)
				CompareWithReference(dev, { 48, 32, 107, 71, ScalerPass::Axis::Both }, s, 1.0, "other kernel 2.22x");
		}

		TEST_METHOD(ScalerGpu12BitRampWithinOneLsbOfTheF32Reference)
		{
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = dev.Gpu();
			const int w = 256, h = 8;
			std::vector<uint16_t> raw(static_cast<size_t>(w) * h);
			for (int y = 0; y < h; ++y)
				for (int x = 0; x < w; ++x) raw[static_cast<size_t>(y) * w + x] = static_cast<uint16_t>((x * 16) << 4);   // 12-bit codes in 16 bits
			pl_tex tex = UploadRaw(gpu, w, h, "r16", raw.data());
			Assert::IsNotNull(tex);
			const Image in = FromRaw(raw, w, h);
			ScalerPass::Module pass;
			ScalerPass::Settings st;
			st.filter = Lanczos(3);
			st.axis = ScalerPass::Axis::X;
			ScalerPass::Source s;
			s.tex = tex;
			pl_tex out = nullptr;
			const int dw = static_cast<int>(std::lround(w * 3.75));
			Assert::IsTrue(pass.Apply(gpu, dev.dispatch, s, dw, 0, st, out), Wide(pass.LastError()).c_str());
			Image got;
			Assert::IsTrue(DownloadFloatImage(gpu, out, got));
			const Image want = ScalerReference::ResampleAxis(in, true, 0.0, w, dw, Lanczos(3));
			const double lsb12 = 1.0 / 4095.0;
			double worst = 0.0;
			for (int x = 0; x < dw; ++x) worst = (std::max)(worst, std::fabs(got.at(x, 3) - want.at(x, 3)));
			Log("12-bit ramp 3.75x: worst " + std::to_string(worst / lsb12) + " LSB");
			Assert::IsTrue(worst <= lsb12, L"within 1 LSB at 12 bits of the f32 reference");
			pass.Release(gpu);
			pl_tex_destroy(gpu, &tex);
		}

		TEST_METHOD(ScalerGpuMigrationIdentityRebaseChromaPasses)
		{
			// ScalerPass(lanczos, ortho, antiring 0.5, chroma, across first, target format) == the chroma planes of
			// SourceRebase::Module::Apply (default separable path), bit for bit.
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = dev.Gpu();
			const int w = 256, h = 144, cw = 128, ch = 72;
			const Image luma = MakeImage(w, h), cbi = MakeImage(cw, ch);
			std::vector<uint16_t> craw(static_cast<size_t>(cw) * ch * 2);
			for (size_t i = 0; i < cbi.v.size(); ++i)
			{
				craw[i * 2] = static_cast<uint16_t>(std::lround(cbi.v[i] * 65535.0));
				craw[i * 2 + 1] = static_cast<uint16_t>(std::lround((1.0 - cbi.v[i] * 0.8) * 65535.0));
			}
			pl_tex ytex = UploadR16(gpu, luma), ctex = UploadRaw(gpu, cw, ch, "rg16", craw.data());
			Assert::IsTrue(ytex && ctex);
			pl_frame src{};
			src.num_planes = 2;
			src.planes[0].texture = ytex; src.planes[0].components = 1; src.planes[0].component_mapping[0] = PL_CHANNEL_Y;
			src.planes[1].texture = ctex; src.planes[1].components = 2;
			src.planes[1].component_mapping[0] = PL_CHANNEL_CB; src.planes[1].component_mapping[1] = PL_CHANNEL_CR;
			src.planes[1].shift_x = -0.5f;
			SetRepr(src);
			SourceRebase::Module rebase;
			pl_frame rebased{};
			Assert::IsTrue(rebase.Apply(gpu, dev.dispatch, src, 0.5, 0.5, rebased), Wide(rebase.LastError()).c_str());
			pl_tex want = rebased.planes[1].texture;
			Assert::IsNotNull(want);

			ScalerPass::Module pass;
			ScalerPass::Settings st;
			st.filter = Lanczos(3);
			st.axis = ScalerPass::Axis::Both;
			st.plane = ScalerPass::Plane::Chroma;
			st.order = ScalerPass::Order::AcrossFirst;
			st.antiring = rebase.antiring;
			st.precision = ScalerPass::Precision::Target;
			st.targetFormat = want->params.format;
			ScalerPass::Source s;
			s.tex = ctex;
			pl_tex got = nullptr;
			Assert::IsTrue(pass.Apply(gpu, dev.dispatch, s, want->params.w, want->params.h, st, got), Wide(pass.LastError()).c_str());
			Assert::AreEqual(want->params.w, got->params.w);
			Assert::AreEqual(want->params.h, got->params.h);
			std::vector<uint8_t> a, b;
			Assert::IsTrue(DownloadBytes(gpu, want, a) && DownloadBytes(gpu, got, b));
			Assert::IsTrue(a == b, L"ScalerPass chroma == re-base chroma passes, bit for bit");
			pass.Release(gpu);
			rebase.Release(gpu);
			pl_tex_destroy(gpu, &ytex);
			pl_tex_destroy(gpu, &ctex);
		}

		TEST_METHOD(ScalerGpuMigrationIdentityFaithfulOrtho)
		{
			// ScalerPass(ortho, down first, integer window, antiring) == the faithful ortho branch's libplacebo call
			// sequence as AdaptiveDownscale makes it (quoted here: vertical pass over whole columns ox..ox+w, then
			// the offset rect), rgba32f picture, bit for bit.
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = dev.Gpu();
			const int sw = 100, sh = 60, dw = 48, dh = 27;
			const float sx0 = 2.5f, sy0 = 1.5f, sx1 = 98.5f, sy1 = 54.5f;
			std::vector<float> px(static_cast<size_t>(sw) * sh * 4);
			const Image base = MakeImage(sw, sh);
			for (size_t i = 0; i < base.v.size(); ++i)
			{
				px[i * 4] = static_cast<float>(base.v[i]);
				px[i * 4 + 1] = static_cast<float>(1.0 - base.v[i]);
				px[i * 4 + 2] = static_cast<float>(base.v[i] * base.v[i]);
				px[i * 4 + 3] = 1.0f;
			}
			pl_tex tex = UploadRaw(gpu, sw, sh, "rgba32f", px.data());
			Assert::IsNotNull(tex);
			const float antiring = 0.5f;

			// the quoted sequence
			const int ox = static_cast<int>(std::floor(sx0));
			const int w = (std::max)(1, static_cast<int>(std::ceil(sx1)) - ox);
			const auto caps = static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_LINEAR);
			pl_fmt f32 = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32, caps);
			Assert::IsNotNull(f32);
			pl_tex mid = nullptr, want = nullptr;
			pl_tex_params tp{};
			tp.format = f32; tp.sampleable = true; tp.renderable = true; tp.blit_src = true;
			tp.w = w; tp.h = dh;
			Assert::IsTrue(pl_tex_recreate(gpu, &mid, &tp));
			tp.w = dw; tp.h = dh;
			Assert::IsTrue(pl_tex_recreate(gpu, &want, &tp));
			pl_shader_obj lut[2] = {};
			for (int pass = 0; pass < 2; ++pass)
			{
				pl_shader sh = pl_dispatch_begin(dev.dispatch);
				pl_sample_src src{};
				src.components = 4;
				if (pass == 0)
				{
					src.tex = tex;
					src.rect = { static_cast<float>(ox), sy0, static_cast<float>(ox + w), sy1 };
					src.new_w = w;
					src.new_h = dh;
				}
				else
				{
					src.tex = mid;
					src.rect = { sx0 - ox, 0.0f, sx1 - ox, static_cast<float>(dh) };
					src.new_w = dw;
					src.new_h = dh;
				}
				pl_sample_filter_params filter{};
				filter.filter = pl_filter_lanczos;
				filter.antiring = antiring;
				filter.lut = &lut[pass];
				pl_dispatch_params dp{};
				dp.shader = &sh;
				dp.target = pass == 0 ? mid : want;
				Assert::IsTrue(pl_shader_sample_ortho2(sh, &src, &filter) && pl_dispatch_finish(dev.dispatch, &dp));
			}

			ScalerPass::Module pass;
			ScalerPass::Settings st;
			st.filter = Lanczos(3);
			st.axis = ScalerPass::Axis::Both;
			st.order = ScalerPass::Order::DownFirst;
			st.antiring = antiring;
			st.precision = ScalerPass::Precision::F32;
			ScalerPass::Source s;
			s.tex = tex; s.x0 = sx0; s.y0 = sy0; s.x1 = sx1; s.y1 = sy1;
			pl_tex got = nullptr;
			Assert::IsTrue(pass.Apply(gpu, dev.dispatch, s, dw, dh, st, got), Wide(pass.LastError()).c_str());
			std::vector<uint8_t> a, b;
			Assert::IsTrue(DownloadBytes(gpu, want, a) && DownloadBytes(gpu, got, b));
			Assert::IsTrue(a == b, L"ScalerPass == the faithful ortho call sequence, bit for bit");
			pass.Release(gpu);
			pl_shader_obj_destroy(&lut[0]);
			pl_shader_obj_destroy(&lut[1]);
			pl_tex_destroy(gpu, &mid);
			pl_tex_destroy(gpu, &want);
			pl_tex_destroy(gpu, &tex);
		}

		TEST_METHOD(ScalerGpuPolarFlatStaysFlatAndIsDeterministic)
		{
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = dev.Gpu();
			const Image flat(40, 30, 0.5);
			pl_tex tex = UploadR16(gpu, flat);
			ScalerPass::Module pass;
			ScalerPass::Settings st;
			st.filter = Plain(Kind::EwaLanczosSharp);
			st.axis = ScalerPass::Axis::Both;
			ScalerPass::Source s;
			s.tex = tex;
			pl_tex o1 = nullptr;
			Assert::IsTrue(pass.Apply(gpu, dev.dispatch, s, 89, 67, st, o1), Wide(pass.LastError()).c_str());
			Image a, b;
			Assert::IsTrue(DownloadFloatImage(gpu, o1, a));
			for (double v : a.v) Assert::AreEqual(0.5, v, 2e-3);
			pl_tex o2 = nullptr;
			Assert::IsTrue(pass.Apply(gpu, dev.dispatch, s, 89, 67, st, o2, 1), Wide(pass.LastError()).c_str());
			Assert::IsTrue(DownloadFloatImage(gpu, o2, b));
			Assert::IsTrue(a.v == b.v, L"deterministic");
			// a polar filter on a single axis is refused visibly
			st.axis = ScalerPass::Axis::X;
			Assert::IsFalse(pass.Apply(gpu, dev.dispatch, s, 89, 0, st, o2));
			Assert::IsFalse(pass.LastError().empty());
			pass.Release(gpu);
			pl_tex_destroy(gpu, &tex);
		}

		TEST_METHOD(ScalerGpuHalfFloatIsAnnouncedAndLinearLightRefused)
		{
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = dev.Gpu();
			const Image im = MakeImage(32, 24);
			pl_tex tex = UploadR16(gpu, im);
			ScalerPass::Module pass;
			ScalerPass::Settings st;
			st.filter = Lanczos(3);
			ScalerPass::Source s;
			s.tex = tex;
			pl_tex out = nullptr;
			if (pass.Apply(gpu, dev.dispatch, s, 64, 48, st, out))
				Assert::IsTrue(pass.Note().empty(), L"f32 default: no downgrade note");
			st.precision = ScalerPass::Precision::F16;
			if (pass.Apply(gpu, dev.dispatch, s, 64, 48, st, out))
				Assert::IsFalse(pass.Note().empty(), L"f16 is visible (no silent fallback)");
			st.precision = ScalerPass::Precision::F32;
			st.light = ScalerPass::Light::Linear;
			Assert::IsFalse(pass.Apply(gpu, dev.dispatch, s, 64, 48, st, out));
			Assert::IsFalse(pass.LastError().empty());
			pass.Release(gpu);
			pl_tex_destroy(gpu, &tex);
		}

		TEST_METHOD(ScalerGpuSingleAxisKeepsItsWindow)
		{
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = dev.Gpu();
			pl_tex tex = UploadR16(gpu, MakeImage(32, 24));
			ScalerPass::Module pass;
			ScalerPass::Settings st;
			st.filter = Lanczos(3);
			st.axis = ScalerPass::Axis::X;
			ScalerPass::Source s;
			s.tex = tex;
			pl_tex out = nullptr;
			Assert::IsTrue(pass.Apply(gpu, dev.dispatch, s, 70, 0, st, out), Wide(pass.LastError()).c_str());
			Assert::AreEqual(24, out->params.h);
			Assert::IsFalse(pass.Apply(gpu, dev.dispatch, s, 70, 25, st, out), L"a mismatched unscaled axis is refused");
			pass.Release(gpu);
			pl_tex_destroy(gpu, &tex);
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(ScalerGpuTiming)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(ScalerGpuTiming)
		{
			// Hardware only (VP_TEST_GPU=hardware): 1920x1080 -> 3840x2160 luma, one ScalerPass call, median over 40
			// frames with pl_gpu_finish per frame. Logged, not asserted (the brief expects ~1.5 ms on the laptop).
			if (!ExtendedRangeGpuProbe::OnHardwareGpu())
			{
				Log("ScalerGpuTiming: VP_TEST_GPU=hardware only; not run");
				return;
			}
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = dev.Gpu();
			std::vector<uint16_t> raw(1920u * 1080u);
			for (int y = 0; y < 1080; ++y)
				for (int x = 0; x < 1920; ++x) raw[static_cast<size_t>(y) * 1920 + x] = static_cast<uint16_t>((((x >> 5) + (y >> 5)) & 1 ? 40000 : 20000) + ((x * 7 + y * 13) & 63));
			pl_tex tex = UploadRaw(gpu, 1920, 1080, "r16", raw.data());
			const Spec specs[2] = { Lanczos(3), Plain(Kind::EwaLanczosSharp) };
			for (const Spec& spec : specs)
			{
				ScalerPass::Module pass;
				ScalerPass::Settings st;
				st.filter = spec;
				st.axis = ScalerPass::Axis::Both;
				st.precision = ScalerPass::Precision::Target;
				ScalerPass::Source s;
				s.tex = tex;
				std::vector<double> ms;
				for (int k = 0; k < 40; ++k)
				{
					pl_tex out = nullptr;
					pl_gpu_finish(gpu);
					const auto t0 = std::chrono::steady_clock::now();
					Assert::IsTrue(pass.Apply(gpu, dev.dispatch, s, 3840, 2160, st, out), Wide(pass.LastError()).c_str());
					pl_gpu_finish(gpu);
					ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
				}
				std::sort(ms.begin(), ms.end());
				char line[200];
				std::snprintf(line, sizeof line, "ScalerPass 1080->2160 luma (%s): median %.3f ms over %zu frames (first %.3f ms)",
					pass.Status().c_str(), ms[ms.size() / 2], ms.size(), ms.front());
				Log(line);
				pass.Release(gpu);
			}
			pl_tex_destroy(gpu, &tex);
		}
	};
}
