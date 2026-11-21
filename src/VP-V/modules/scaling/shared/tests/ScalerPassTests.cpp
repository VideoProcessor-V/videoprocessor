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
#include <VP-V/tests/support/HardwarePrerequisites.h>

#include <VP-V/modules/scaling/rebase/KernelInverse.h>
#include <VP-V/modules/scaling/rebase/SourceRebase.h>
#include <VP-V/modules/scaling/shared/KernelTable.h>
#include <VP-V/modules/scaling/shared/ScalerGeometry.h>
#include <VP-V/modules/scaling/shared/ScalerPass.h>
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
			Assert::IsTrue(pass.Apply(gpu, dev.dispatch, s, 64, 48, st, out), Wide(pass.LastError()).c_str());
			Assert::IsNotNull(out, L"F32 output texture");
			Assert::IsTrue(pass.Note().empty(), L"f32 default: no downgrade note");
			out = nullptr;
			st.precision = ScalerPass::Precision::F16;
			Assert::IsTrue(pass.Apply(gpu, dev.dispatch, s, 64, 48, st, out), Wide(pass.LastError()).c_str());
			Assert::IsNotNull(out, L"F16 output texture");
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
			if (!HardwarePrerequisites::RequireOrSkip(HardwarePrerequisites::Kind::Gpu,
				"ScalerGpuTiming", ExtendedRangeGpuProbe::OnHardwareGpu(), "needs VP_TEST_GPU=hardware"))
				return;
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
					Assert::IsNotNull(out, L"scaler output texture must be non-null");
					pl_gpu_finish(gpu);
					ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
				}
				std::sort(ms.begin(), ms.end());
				const double medianMs = ms[ms.size() / 2];
				Assert::IsTrue(std::isfinite(medianMs) && medianMs > 0.0, L"median GPU timing must be finite and positive");
				char line[200];
				std::snprintf(line, sizeof line, "ScalerPass 1080->2160 luma (%s): median %.3f ms over %zu frames (first %.3f ms)",
					pass.Status().c_str(), medianMs, ms.size(), ms.front());
				Log(line);
				pass.Release(gpu);
			}
			pl_tex_destroy(gpu, &tex);
		}
	};
}
