// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// repair.grain_split v3 GPU kernel against its CPU law (P10 v3, refs/116 s5): the five-pass kernel (HALF+across, DOWN, STATS,
// NMIN, APPLY) in both layouts (Full, Compact) on a libplacebo device (WARP unless VP_TEST_GPU=hardware) against
// GrainSplit::Compute (the v3 law in doubles): odd and even plane sizes, block edges 8 / 16 / 32 with the lane law's scale B / 16.
//   V3GpuMatchesCpuReference  max |s1|, |s2|, |base| <= 0.05 code, f <= 1/255, N and e3 / e1 / e2 relative <= 1e-3.
//   V3GpuIsDeterministic      two runs: bit-identical product and blocks.
//   V3StepLoopEqualsCompute   Step(0..6) is bit-identical to Compute.
//   V5ChunkedStatsEqualsUnit4 unit 5 (STATS per 16x16 chunk + REDUCE, refs/127 A1) against the unit-4 output: the FNV-1a 64 of the
//                             product and blocks at every live block edge (8 / 16 / 32 / 64) pinned to the values the unit-4
//                             kernel wrote on the WARP device (WAIVER-P10V5-01 condition a); a hardware device logs only.
//   V3P11RegionFlagsAgree     consumer level: the P11 region classification from the v3 split against the v2 split (CPU).
// Every measured worst value is written to the test log. No assertion is loosened to fit a result (rule 28): a miss is reported
// with its numbers.
#include "pch.h"
#include "CppUnitTest.h"
#include <VP-V/tests/support/ExtendedRangeGpuProbe.h>

#include <VP-V/modules/repair/grain_split/GrainSplit.h>
#include <VP-V/modules/repair/grain_split/GrainSplitGpu.h>
#include <VP-V/modules/repair/grain_split/GpuPassUtil.h>
#include <VP-V/modules/input/source_analysis/regions/RegionEvidence.h>

#include <libplacebo/d3d11.h>
#include <libplacebo/dispatch.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace GrainSplitV3GpuTestsNs
{
	using GrainSplit::Plane;
	using GrainSplit::Storage;

	constexpr double kPi = 3.14159265358979323846;
	constexpr double kCodeUnit = 64.0 / 65535.0;   // 10-bit codes in a 16-bit plane, 1/64-code steps (as the sibling grain split tests)

	void LogF(const char* fmt, double a = 0, double b = 0, double c = 0, double d = 0, double e = 0, double f = 0)
	{
		char line[448];
		std::snprintf(line, sizeof line, fmt, a, b, c, d, e, f);
		Logger::WriteMessage(line);
	}
	std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }

	// Luma in codes: left third a flat gradient, middle third fine texture, right third structure (a rectangle and a diagonal
	// edge); grain (sigma 2 codes) everywhere; quantised to 1/64 code like the GPU plane.
	Plane MakePlane(int w, int h, uint32_t seed)
	{
		Plane p(w, h);
		std::mt19937 g(seed);
		std::normal_distribution<double> noise(0.0, 2.0);
		for (int y = 0; y < h; ++y)
			for (int x = 0; x < w; ++x)
			{
				double v;
				if (x < w / 3)
					v = 250.0 + 0.4 * x + 0.3 * y;
				else if (x < 2 * w / 3)
					v = 350.0 + 14.0 * std::sin(2.0 * kPi * x / 5.0) * std::cos(2.0 * kPi * y / 6.5) + 9.0 * std::sin(2.0 * kPi * (x + y) / 9.0);
				else
				{
					const double rx0 = 0.72 * w, rx1 = 0.92 * w, ry0 = 0.2 * h, ry1 = 0.8 * h;
					const double inX = 0.5 * (std::tanh((x - rx0) / 0.7) - std::tanh((x - rx1) / 0.7));
					const double inY = 0.5 * (std::tanh((y - ry0) / 0.7) - std::tanh((y - ry1) / 0.7));
					v = 300.0 + 250.0 * inX * inY + 80.0 * std::tanh((x * 0.6 + y * 0.8 - 0.70 * w) / 0.9);
				}
				v += noise(g);
				p.at(x, y) = std::round((std::min)(1023.0, (std::max)(0.0, v)) * 64.0) / 64.0;
			}
		return p;
	}

	struct GpuDevice
	{
		pl_log log = nullptr;
		pl_d3d11 d3d11 = nullptr;
		pl_dispatch dispatch = nullptr;
		std::string name;
		~GpuDevice()
		{
			if (dispatch) pl_dispatch_destroy(&dispatch);
			if (d3d11) pl_d3d11_destroy(&d3d11);
			if (log) pl_log_destroy(&log);
		}
		bool Create()
		{
			pl_log_params lp{};
			lp.log_level = PL_LOG_ERR;
			lp.log_cb = [](void*, enum pl_log_level, const char* msg) { Logger::WriteMessage((std::string("libplacebo: ") + msg).c_str()); };
			log = pl_log_create(PL_API_VER, &lp);
			const pl_d3d11_params params = ExtendedRangeGpuProbe::TestDeviceParams(&name);
			Logger::WriteMessage(("grain split v3 GPU tests on: " + name).c_str());
			d3d11 = pl_d3d11_create(log, &params);
			if (!d3d11) return false;
			dispatch = pl_dispatch_create(log, d3d11->gpu);
			return dispatch != nullptr;
		}
		pl_gpu Gpu() const { return d3d11->gpu; }
	};

	pl_tex UploadLuma(pl_gpu gpu, const Plane& y)
	{
		std::vector<uint16_t> data(y.v.size());
		for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<uint16_t>((std::min)(65535L, (std::max)(0L, std::lround(y.v[i] * 64.0))));
		pl_tex_params p{};
		p.w = y.w; p.h = y.h; p.format = pl_find_named_fmt(gpu, "r16"); p.sampleable = true; p.initial_data = data.data();
		return pl_tex_create(gpu, &p);
	}

	// RG16 unorm texture: channel 0 = noise (a deterministic pattern unrelated to the plane), channel 1 = the plane.
	pl_tex UploadLumaInChannel1(pl_gpu gpu, const Plane& y)
	{
		std::vector<uint16_t> data(y.v.size() * 2);
		for (size_t i = 0; i < y.v.size(); ++i)
		{
			data[i * 2] = static_cast<uint16_t>((i * 2654435761u) >> 16);
			data[i * 2 + 1] = static_cast<uint16_t>((std::min)(65535L, (std::max)(0L, std::lround(y.v[i] * 64.0))));
		}
		pl_tex_params p{};
		p.w = y.w; p.h = y.h; p.format = pl_find_named_fmt(gpu, "rg16"); p.sampleable = true; p.initial_data = data.data();
		return pl_tex_create(gpu, &p);
	}

	// Any sampleable texture as four floats per texel (unorm and half textures come back normalised / widened): a compute pass
	// copies it into a host-readable rgba32f storage texture.
	std::vector<float> DownloadRGBA(pl_gpu gpu, pl_dispatch dispatch, pl_tex tex)
	{
		if (!tex) return {};
		pl_tex_params rp{};
		rp.w = tex->params.w; rp.h = tex->params.h; rp.format = VpvGpu::FloatFormat(gpu, 4); rp.host_readable = true; rp.storable = true;
		pl_tex readable = pl_tex_create(gpu, &rp);
		if (!readable) return {};
		const pl_shader_desc d[2] = { VpvGpu::Sampled("src", tex), VpvGpu::Storage("dst", readable) };
		VpvGpu::Pass(dispatch, "test copy", std::string(), R"GLSL(
ivec2 p = ivec2(gl_GlobalInvocationID.xy);
if (p.x < textureSize(src, 0).x && p.y < textureSize(src, 0).y) imageStore(dst, p, texelFetch(src, p, 0));
)GLSL", d, 2, nullptr, 0, (rp.w + 15) / 16, (rp.h + 15) / 16, 0);
		std::vector<float> v(static_cast<size_t>(rp.w) * rp.h * 4);
		pl_tex_transfer_params tr{}; tr.tex = readable; tr.ptr = v.data();
		const bool ok = pl_tex_download(gpu, &tr);
		pl_tex_destroy(gpu, &readable);
		if (!ok) v.clear();
		return v;
	}

	// The product of one run, read back: Full: a = (s1, s2, f, base) per texel, b empty; Compact: a = (s1, s2, -, -),
	// b = (f, base x codeUnit, -, -). blocks = (N, e3, e1, e2) per block in Compact; Full carries (N, e3, 0, 0).
	struct Product
	{
		bool ok = false;
		std::string error;
		bool compact = false;
		int w = 0, h = 0, bw = 0, bh = 0;
		std::vector<float> a, b, blocks;
	};

	// One run on the device: Splitter::Compute, or (stepLoop) the six Step calls with the same arguments.
	Product Run(GpuDevice& dev, pl_tex tex, int B, double scale, Storage storage, bool stepLoop = false, uint32_t component = 0)
	{
		Product r;
		pl_gpu gpu = dev.Gpu();
		GrainSplit::Gpu::Splitter splitter;
		GrainSplit::Gpu::Options o;
		o.storage = storage;
		bool ok = true;
		if (stepLoop)
		{
			for (uint32_t s = 0; s < GrainSplit::Gpu::Splitter::StepCount() && ok; ++s)
				ok = splitter.Step(gpu, dev.dispatch, tex, component, kCodeUnit, scale, B, GrainSplit::Params(), o, s);
		}
		else
			ok = splitter.Compute(gpu, dev.dispatch, tex, component, kCodeUnit, scale, B, GrainSplit::Params(), o);
		r.ok = ok;
		r.error = splitter.LastError();
		if (ok)
		{
			const GrainSplit::Gpu::Frame& f = splitter.Out();
			r.compact = GrainSplit::Gpu::Compact(&f);
			r.w = f.w; r.h = f.h; r.bw = f.bw; r.bh = f.bh;
			r.a = DownloadRGBA(gpu, dev.dispatch, f.split);
			if (r.compact) r.b = DownloadRGBA(gpu, dev.dispatch, f.splitB);
			r.blocks = DownloadRGBA(gpu, dev.dispatch, f.blocks);
		}
		splitter.Release(gpu);
		return r;
	}

	bool SameBits(const std::vector<float>& x, const std::vector<float>& y)
	{
		return x.size() == y.size() && (x.empty() || std::memcmp(x.data(), y.data(), x.size() * sizeof(float)) == 0);
	}

	// (s1, s2, f, base) in codes of texel i, whichever the layout.
	struct Texel { double s1, s2, f, base; };
	Texel Decode(const Product& p, size_t i)
	{
		if (!p.compact) return { p.a[i * 4], p.a[i * 4 + 1], p.a[i * 4 + 2], p.a[i * 4 + 3] };
		return { p.a[i * 4], p.a[i * 4 + 1], p.b[i * 4], p.b[i * 4 + 1] / kCodeUnit };
	}

	// Relative error with a floor: the energies of a nearly flat block are around the plane's 1/64-code quantisation noise (1e-4
	// codes^2 and below), where a float32 kernel and a double reference cannot agree in relative terms; 1e-2 codes^2 is below
	// every grain level the test planes carry (sigma >= 1 code).
	constexpr double kRelFloor = 1e-2;
	double Rel(double got, double ref) { return std::fabs(got - ref) / (std::max)(std::fabs(ref), kRelFloor); }

	// ---- the consumer-level classes (test d): flat, gradient, dark flat, texture, edges, planted grain 1 and 3 codes -------

	enum class Cls { Flat, Gradient, DarkFlat, Texture, Edges, Grain1, Grain3, Count };
	const char* ClsName(Cls c)
	{
		static const char* n[] = { "flat", "gradient", "dark flat", "texture", "edges", "grain 1 code", "grain 3 codes" };
		return n[static_cast<int>(c)];
	}

	Plane MakeClass(Cls c, int w, int h, uint32_t seed)
	{
		Plane p(w, h);
		std::mt19937 g(seed);
		std::normal_distribution<double> unit(0.0, 1.0);
		for (int y = 0; y < h; ++y)
			for (int x = 0; x < w; ++x)
			{
				double v = 400.0;
				switch (c)
				{
				case Cls::Flat: v = 400.0; break;
				case Cls::Gradient: v = 150.0 + 0.45 * x + 0.2 * y; break;
				case Cls::DarkFlat: v = 70.0 + 0.02 * x; break;
				case Cls::Texture:
					v = 350.0 + 14.0 * std::sin(2.0 * kPi * x / 5.0) * std::cos(2.0 * kPi * y / 6.5) + 9.0 * std::sin(2.0 * kPi * (x + y) / 9.0);
					break;
				case Cls::Edges:
				{
					const bool tile = (((x / 40) + (y / 36)) & 1) != 0;
					v = 300.0 + (tile ? 220.0 : 0.0) + 60.0 * std::tanh((x * 0.6 + y * 0.8 - 0.5 * w) / 0.9);
					break;
				}
				case Cls::Grain1: v = 400.0 + 0.15 * x + 1.0 * unit(g); break;
				case Cls::Grain3: v = 400.0 + 0.15 * x + 3.0 * unit(g); break;
				default: break;
				}
				p.at(x, y) = std::round((std::min)(1023.0, (std::max)(0.0, v)) * 64.0) / 64.0;
			}
		return p;
	}

	// The discrete classifications of one block from the derive law's maps (thresholds: the law's own constants where it has
	// them, round numbers elsewhere), as one bit each.
	constexpr int kNumFlags = 8;
	const char* const kFlagName[kNumFlags] = { "banding>=0.5", "ringing>=0.25", "texture>=flatLo", "texture>=flatHi",
		"grain>=0.5", "grain>=2", "edgeContrast>=0.5", "flags word" };
	uint32_t Classify(const RegionEvidence::DeriveParams& dp, const double* precursors)
	{
		double m[RegionEvidence::kMapChannels];
		RegionEvidence::DeriveBlock<double>(dp, precursors, m);
		uint32_t bits = 0;
		bits |= (m[RegionEvidence::kBanding] >= 0.5 ? 1u : 0u) << 0;
		bits |= (m[RegionEvidence::kRinging] >= 0.25 ? 1u : 0u) << 1;
		bits |= (m[RegionEvidence::kTexture] >= dp.flatLo ? 1u : 0u) << 2;
		bits |= (m[RegionEvidence::kTexture] >= dp.flatHi ? 1u : 0u) << 3;
		bits |= (m[RegionEvidence::kGrain] >= 0.5 ? 1u : 0u) << 4;
		bits |= (m[RegionEvidence::kGrain] >= 2.0 ? 1u : 0u) << 5;
		bits |= (m[RegionEvidence::kEdgeContrast] >= 0.5 ? 1u : 0u) << 6;
		// the flags word is an exact small integer: its low bits (inPicture, overlay, noGrain, tinyMover) are folded in by value
		const uint32_t word = static_cast<uint32_t>(m[RegionEvidence::kFlags] + 0.5);
		return bits | (word << 8);
	}
}

TEST_CLASS(GrainSplitV3GpuTests)
{
	using GpuDevice = GrainSplitV3GpuTestsNs::GpuDevice;

public:
	// (a) the GPU kernel against the CPU v3 law: planes, block edges and layouts.
	TEST_METHOD(V3GpuMatchesCpuReference)
	{
		using namespace GrainSplitV3GpuTestsNs;
		GpuDevice dev;
		Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
		pl_gpu gpu = dev.Gpu();
		const int sizes[3][2] = { { 257, 143 }, { 320, 180 }, { 251, 139 } };
		const int edges[5] = { 8, 9, 16, 17, 32 };
		for (int si = 0; si < 3; ++si)
			for (int bi = 0; bi < 5; ++bi)
			{
				const int w = sizes[si][0], h = sizes[si][1], B = edges[bi];
				const double scale = B / 16.0;
				const Plane y = MakePlane(w, h, 101u + static_cast<uint32_t>(si * 5 + bi));
				const GrainSplit::Split ref = GrainSplit::Compute(y, scale, B);
				pl_tex tex = UploadLuma(gpu, y);
				Assert::IsNotNull(tex, L"upload");
				for (int st = 0; st < 2; ++st)
				{
					const Storage storage = st == 0 ? Storage::Full : Storage::Compact;
					const Product p = Run(dev, tex, B, scale, storage);
					Assert::IsTrue(p.ok, Wide(p.error).c_str());
					Assert::IsTrue(!p.a.empty() && !p.blocks.empty() && (storage == Storage::Full || !p.b.empty()), L"downloaded");
					Assert::AreEqual(ref.bw, p.bw, L"block grid width");
					Assert::AreEqual(ref.bh, p.bh, L"block grid height");
					double worstS = 0, worstSrel = 0, worstBase = 0, worstF = 0;
					for (int py = 0; py < h; ++py)
						for (int px = 0; px < w; ++px)
						{
							const size_t i = static_cast<size_t>(py) * w + px;
							const Texel t = Decode(p, i);
							// fp16 keeps 11 significant bits (Compact): absolute up to |s| = 64 codes, relative above (the bound of
							// GrainSplitCompactGpuTests; opus gate SHOULD 1 - an absolute 0.05 fails from 128 codes on).
							const double r1 = ref.s1.at(px, py), r2 = ref.s2.at(px, py);
							const double e1 = std::fabs(t.s1 - r1), e2 = std::fabs(t.s2 - r2);
							if (std::fabs(r1) <= 64.0) worstS = (std::max)(worstS, e1); else worstSrel = (std::max)(worstSrel, e1 / std::fabs(r1));
							if (std::fabs(r2) <= 64.0) worstS = (std::max)(worstS, e2); else worstSrel = (std::max)(worstSrel, e2 / std::fabs(r2));
							worstBase = (std::max)(worstBase, std::fabs(t.base - ref.base.at(px, py)));
							worstF = (std::max)(worstF, std::fabs(t.f - ref.f.at(px, py)));
						}
					double worstN = 0, worstE3 = 0, worstE1 = 0, worstE2 = 0;
					for (size_t i = 0; i < static_cast<size_t>(ref.bw) * ref.bh; ++i)
					{
						worstN = (std::max)(worstN, Rel(p.blocks[i * 4], ref.N[i]));
						worstE3 = (std::max)(worstE3, Rel(p.blocks[i * 4 + 1], ref.e3[i]));
						if (storage == Storage::Compact)
						{
							worstE1 = (std::max)(worstE1, Rel(p.blocks[i * 4 + 2], ref.e1[i]));
							worstE2 = (std::max)(worstE2, Rel(p.blocks[i * 4 + 3], ref.e2[i]));
						}
					}
					char head[160];
					std::snprintf(head, sizeof head, "v3 GPU %s vs CPU %dx%d B %d scale %.3f", GrainSplit::Token(storage), w, h, B, scale);
					Logger::WriteMessage(head);
					LogF("  max |s1,s2| err %.5f codes (|s| <= 64), rel %.2e (|s| > 64), |base| err %.5f codes, |f| err %.6f", worstS, worstSrel, worstBase, worstF);
					LogF("  block rel err: N %.2e, e3 %.2e, e1 %.2e, e2 %.2e (floor %.0e codes^2)", worstN, worstE3, worstE1, worstE2, kRelFloor);
					const std::wstring tag = Wide(head);
					Assert::IsTrue(worstS <= 0.05, (tag + L": |s1|,|s2| error within 0.05 code for |s| <= 64").c_str());
					Assert::IsTrue(worstSrel <= 1.0 / 1024.0, (tag + L": |s1|,|s2| relative error within 2^-10 above 64 codes").c_str());
					Assert::IsTrue(worstBase <= 0.05, (tag + L": max |base| error within 0.05 code").c_str());
					Assert::IsTrue(worstF <= 1.0 / 255.0, (tag + L": max |f| error within 1/255").c_str());
					Assert::IsTrue(worstN <= 1e-3, (tag + L": N relative error within 1e-3").c_str());
					Assert::IsTrue(worstE3 <= 1e-3, (tag + L": e3 relative error within 1e-3").c_str());
					if (storage == Storage::Compact)
					{
						Assert::IsTrue(worstE1 <= 1e-3, (tag + L": e1 relative error within 1e-3").c_str());
						Assert::IsTrue(worstE2 <= 1e-3, (tag + L": e2 relative error within 1e-3").c_str());
					}
				}
				pl_tex_destroy(gpu, &tex);
			}
	}

	// (a2) component 1 of an RG16 texture (plane in channel 1, noise in channel 0) equals component 0 of an R16 texture of the plane.
	TEST_METHOD(V3ComponentOneEqualsComponentZero)
	{
		using namespace GrainSplitV3GpuTestsNs;
		GpuDevice dev;
		Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
		pl_gpu gpu = dev.Gpu();
		const int cases[3][3] = { { 320, 180, 16 }, { 251, 139, 9 }, { 257, 143, 32 } };
		for (const auto& cs : cases)
		{
			const int w = cs[0], h = cs[1], B = cs[2];
			const Plane y = MakePlane(w, h, 401u + static_cast<uint32_t>(B));
			pl_tex rg = UploadLumaInChannel1(gpu, y);
			pl_tex r = UploadLuma(gpu, y);
			Assert::IsNotNull(rg, L"rg16 upload");
			Assert::IsNotNull(r, L"r16 upload");
			for (int st = 0; st < 2; ++st)
			{
				const Storage storage = st == 0 ? Storage::Full : Storage::Compact;
				const Product a = Run(dev, rg, B, B / 16.0, storage, false, 1);
				const Product b = Run(dev, r, B, B / 16.0, storage, false, 0);
				Assert::IsTrue(a.ok, Wide(a.error).c_str());
				Assert::IsTrue(b.ok, Wide(b.error).c_str());
				Assert::IsTrue(!a.a.empty() && !a.blocks.empty(), L"downloaded");
				const bool same = SameBits(a.a, b.a) && SameBits(a.b, b.b) && SameBits(a.blocks, b.blocks);
				LogF("v3 component 1 (RG16) vs component 0 (R16) %.0fx%.0f B %.0f: %.0f (0 = bit-identical)", w, h, B, same ? 0.0 : 1.0);
				Assert::IsTrue(same, L"component 1 of RG16 is bit-identical to component 0 of R16 (split and blocks)");
			}
			pl_tex_destroy(gpu, &rg);
			pl_tex_destroy(gpu, &r);
		}
	}

	// (b) two runs on the same plane give a bit-identical product and blocks.
	TEST_METHOD(V3GpuIsDeterministic)
	{
		using namespace GrainSplitV3GpuTestsNs;
		GpuDevice dev;
		Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
		pl_gpu gpu = dev.Gpu();
		const int cases[3][3] = { { 320, 180, 16 }, { 257, 143, 8 }, { 257, 143, 32 } };
		for (const auto& cs : cases)
		{
			const int w = cs[0], h = cs[1], B = cs[2];
			const Plane y = MakePlane(w, h, 211u + static_cast<uint32_t>(B));
			pl_tex tex = UploadLuma(gpu, y);
			Assert::IsNotNull(tex, L"upload");
			for (int st = 0; st < 2; ++st)
			{
				const Storage storage = st == 0 ? Storage::Full : Storage::Compact;
				const Product p1 = Run(dev, tex, B, B / 16.0, storage);
				const Product p2 = Run(dev, tex, B, B / 16.0, storage);
				Assert::IsTrue(p1.ok && p2.ok, Wide(p1.error + p2.error).c_str());
				Assert::IsTrue(!p1.a.empty() && !p1.blocks.empty(), L"downloaded");
				const bool same = SameBits(p1.a, p2.a) && SameBits(p1.b, p2.b) && SameBits(p1.blocks, p2.blocks);
				LogF("v3 GPU determinism %.0fx%.0f B %.0f: %.0f (0 = bit-identical)", w, h, B, same ? 0.0 : 1.0);
				Assert::IsTrue(same, L"two runs are bit-identical (split and blocks)");
			}
			pl_tex_destroy(gpu, &tex);
		}
	}

	// (c) Step(0..6) with the same arguments records exactly Compute's dispatches: bit-identical output.
	TEST_METHOD(V3StepLoopEqualsCompute)
	{
		using namespace GrainSplitV3GpuTestsNs;
		GpuDevice dev;
		Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
		pl_gpu gpu = dev.Gpu();
		Assert::AreEqual(static_cast<uint32_t>(7), GrainSplit::Gpu::Splitter::StepCount(), L"seven steps (unit 5, WAIVER-P10V5-01: was 6)");
		const int cases[3][3] = { { 320, 180, 16 }, { 257, 143, 8 }, { 257, 143, 32 } };
		for (const auto& cs : cases)
		{
			const int w = cs[0], h = cs[1], B = cs[2];
			const Plane y = MakePlane(w, h, 307u + static_cast<uint32_t>(B));
			pl_tex tex = UploadLuma(gpu, y);
			Assert::IsNotNull(tex, L"upload");
			for (int st = 0; st < 2; ++st)
			{
				const Storage storage = st == 0 ? Storage::Full : Storage::Compact;
				const Product whole = Run(dev, tex, B, B / 16.0, storage, false);
				const Product steps = Run(dev, tex, B, B / 16.0, storage, true);
				Assert::IsTrue(whole.ok, Wide(whole.error).c_str());
				Assert::IsTrue(steps.ok, Wide(steps.error).c_str());
				Assert::IsTrue(!whole.a.empty() && !whole.blocks.empty(), L"downloaded");
				const bool same = SameBits(whole.a, steps.a) && SameBits(whole.b, steps.b) && SameBits(whole.blocks, steps.blocks);
				LogF("v3 step loop %.0fx%.0f B %.0f: %.0f (0 = bit-identical to Compute)", w, h, B, same ? 0.0 : 1.0);
				Assert::IsTrue(same, L"Step(0..6) is bit-identical to Compute");
			}
			pl_tex_destroy(gpu, &tex);
		}
	}

	// (e) unit 5 against unit 4 (WAIVER-P10V5-01 condition a): the chunked STATS + REDUCE leave every live block edge
	// bit-identical. The pins are the FNV-1a 64 of (product, blocks) the unit-4 kernel (one workgroup per block) wrote for
	// these planes on the WARP device, recorded at r2/job1-base f61d18823 with this test before the restructure.
	TEST_METHOD(V5ChunkedStatsEqualsUnit4)
	{
		using namespace GrainSplitV3GpuTestsNs;
		GpuDevice dev;
		Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
		pl_gpu gpu = dev.Gpu();
		const bool warp = dev.name.find("WARP") != std::string::npos || dev.name.find("Basic Render") != std::string::npos;
		struct Case { int w, h, B; uint64_t full, compact; };
		const Case cases[] = {
			{ 1280, 720, 8, 0x308ec19d2183c7b1ull, 0xefe7a232b85c1a98ull },
			{ 1920, 1080, 16, 0xcfa64dbed42d3583ull, 0x485bcd46682e3108ull },
			{ 3840, 2160, 32, 0x23ca19cdabd353b1ull, 0x30b5d6e1685ac00cull },
			{ 1031, 587, 64, 0xdceedb61daa8de8dull, 0x18b58ec0492b7f28ull },
			{ 257, 143, 32, 0xbb99a79cc34ce9d6ull, 0x13a4b5dae2aa48b5ull },
		};
		auto fnv = [](uint64_t h, const std::vector<float>& v) {
			const unsigned char* b = reinterpret_cast<const unsigned char*>(v.data());
			for (size_t i = 0; i < v.size() * sizeof(float); ++i) { h ^= b[i]; h *= 1099511628211ull; }
			return h;
		};
		bool all = true;
		for (const Case& cs : cases)
		{
			const Plane y = MakePlane(cs.w, cs.h, 401u + static_cast<uint32_t>(cs.B));
			pl_tex tex = UploadLuma(gpu, y);
			Assert::IsNotNull(tex, L"upload");
			for (int st = 0; st < 2; ++st)
			{
				const Storage storage = st == 0 ? Storage::Full : Storage::Compact;
				const Product p = Run(dev, tex, cs.B, cs.B / 16.0, storage, false);
				Assert::IsTrue(p.ok, Wide(p.error).c_str());
				const uint64_t h = fnv(fnv(fnv(14695981039346656037ull, p.a), p.b), p.blocks);
				const uint64_t pin = st == 0 ? cs.full : cs.compact;
				if (warp)
				{
					char line[256];
					std::snprintf(line, sizeof line, "v5 vs unit 4 %dx%d B %d %s: hash 0x%016llx pin 0x%016llx (WARP pinned)", cs.w, cs.h, cs.B,
						st == 0 ? "full" : "compact", static_cast<unsigned long long>(h), static_cast<unsigned long long>(pin));
					Logger::WriteMessage(line);
					if (h != pin) all = false;
				}
				else
				{
					// SR-S14: on hardware assert run-to-run bit-identical determinism (WARP stays the pinned reference).
					const Product p2 = Run(dev, tex, cs.B, cs.B / 16.0, storage, false);
					Assert::IsTrue(p2.ok, Wide(p2.error).c_str());
					const uint64_t h2 = fnv(fnv(fnv(14695981039346656037ull, p2.a), p2.b), p2.blocks);
					char line[256];
					std::snprintf(line, sizeof line, "v5 hw determinism %dx%d B %d %s: run1 0x%016llx run2 0x%016llx (WARP pin 0x%016llx)", cs.w, cs.h, cs.B,
						st == 0 ? "full" : "compact", static_cast<unsigned long long>(h), static_cast<unsigned long long>(h2), static_cast<unsigned long long>(pin));
					Logger::WriteMessage(line);
					if (h != h2) all = false;
				}
			}
			pl_tex_destroy(gpu, &tex);
		}
		Assert::IsTrue(all, L"unit 5 output is bit-identical to the unit-4 output at every live block edge");
	}

	// (d) consumer-level equivalence: the P11 region classification from the v3 split against the v2 split.
	// WAIVER-P10V3-02: band d3 has one live reader (P11); TD will read the split later; re-check at the TD build.
	// The v2 GPU kernel no longer exists, so the v2 split is GrainSplit::ComputeV2 (CPU, doubles) and the v3 split is
	// GrainSplit::Compute (the law the GPU kernel is held to in V3GpuMatchesCpuReference). Both feed the CPU region path
	// (RegionEvidence::Compute with a Split, then DeriveBlock): the classifications of the two must agree on >= 99.5 % of blocks.
	TEST_METHOD(V3P11RegionFlagsAgree)
	{
		using namespace GrainSplitV3GpuTestsNs;
		const int w = 512, h = 288;
		const int edges[2] = { 16, 32 };
		for (int bi = 0; bi < 2; ++bi)
		{
			const int B = edges[bi];
			const double scale = B / 16.0;
			RegionEvidence::FrameParams fp;
			fp.blockOverride = B;
			RegionEvidence::Externals ex;
			const RegionEvidence::DeriveParams dp;
			size_t blocks = 0, agree = 0;
			size_t flagDiff[kNumFlags] = {};
			for (int ci = 0; ci < static_cast<int>(Cls::Count); ++ci)
			{
				const Cls cls = static_cast<Cls>(ci);
				const Plane y = MakeClass(cls, w, h, 401u + static_cast<uint32_t>(ci));
				const GrainSplit::Split v3 = GrainSplit::Compute(y, scale, B);
				const GrainSplit::Split v2 = GrainSplit::ComputeV2(y, scale, B);
				const RegionEvidence::Precursors p3 = RegionEvidence::Compute(y, &v3, ex, fp);
				const RegionEvidence::Precursors p2 = RegionEvidence::Compute(y, &v2, ex, fp);
				Assert::IsTrue(p3.HaveSplit() && p2.HaveSplit(), L"both splits are present for the region path");
				Assert::AreEqual(p3.grid.Count(), p2.grid.Count(), L"same grid");
				size_t clsBlocks = 0, clsAgree = 0;
				size_t clsDiff[kNumFlags] = {};
				for (int by = 0; by < p3.grid.gh; ++by)
					for (int bx = 0; bx < p3.grid.gw; ++bx)
					{
						const uint32_t c3 = Classify(dp, p3.Block(bx, by)), c2 = Classify(dp, p2.Block(bx, by));
						++clsBlocks;
						if (c3 == c2) ++clsAgree;
						for (int f = 0; f < kNumFlags - 1; ++f)
							if (((c3 ^ c2) >> f) & 1u) ++clsDiff[f];
						if ((c3 >> 8) != (c2 >> 8)) ++clsDiff[kNumFlags - 1];
					}
				blocks += clsBlocks; agree += clsAgree;
				for (int f = 0; f < kNumFlags; ++f) flagDiff[f] += clsDiff[f];
				std::string line = std::string("  B ") + std::to_string(B) + " class " + ClsName(cls) + ": " + std::to_string(clsAgree) + " / " +
					std::to_string(clsBlocks) + " blocks agree; disagreements per flag:";
				for (int f = 0; f < kNumFlags; ++f) line += std::string(" ") + kFlagName[f] + "=" + std::to_string(clsDiff[f]);
				Logger::WriteMessage(line.c_str());
			}
			const double pct = blocks ? 100.0 * static_cast<double>(agree) / static_cast<double>(blocks) : 0.0;
			std::string total = std::string("v3 vs v2 P11 region classes, B ") + std::to_string(B) + ": " + std::to_string(agree) + " / " + std::to_string(blocks) +
				" blocks agree (" + std::to_string(pct) + " %); disagreements per flag:";
			for (int f = 0; f < kNumFlags; ++f) total += std::string(" ") + kFlagName[f] + "=" + std::to_string(flagDiff[f]);
			Logger::WriteMessage(total.c_str());
			Assert::IsTrue(blocks > 0, L"blocks were compared");
			Assert::IsTrue(static_cast<double>(agree) >= 0.995 * static_cast<double>(blocks), Wide(total).c_str());
		}
	}

	// S2-H7: Splitter::Release(nullptr) (detached/lost GPU) must null all 9 private texture handles AND reset the active
	// stepped run (m_run), so a mid-step Release(nullptr) cannot continue via Step(...) and reusing the same Splitter on a
	// fresh WARP device succeeds cleanly with bit-identical output for both Full and Compact layouts.
	BEGIN_TEST_METHOD_ATTRIBUTE(V3ReleaseNullClearsActiveStepAndRecoversOnFreshDevice)
		TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
	END_TEST_METHOD_ATTRIBUTE()
	TEST_METHOD(V3ReleaseNullClearsActiveStepAndRecoversOnFreshDevice)
	{
		using namespace GrainSplitV3GpuTestsNs;
		struct TexGuard
		{
			pl_gpu gpu = nullptr;
			pl_tex tex = nullptr;
			TexGuard(pl_gpu g, pl_tex t) : gpu(g), tex(t) {}
			~TexGuard() { if (gpu && tex) pl_tex_destroy(gpu, &tex); }
			TexGuard(const TexGuard&) = delete;
			TexGuard& operator=(const TexGuard&) = delete;
		};
		struct SplitterGuard
		{
			pl_gpu gpu = nullptr;
			GrainSplit::Gpu::Splitter* s = nullptr;
			SplitterGuard(pl_gpu g, GrainSplit::Gpu::Splitter* sp) : gpu(g), s(sp) {}
			~SplitterGuard() { if (gpu && s) s->Release(gpu); }
			SplitterGuard(const SplitterGuard&) = delete;
			SplitterGuard& operator=(const SplitterGuard&) = delete;
		};

		const int w = 128, h = 96, B = 16;
		const double scale = B / 16.0;
		const Plane y = MakePlane(w, h, 709u);
		const GrainSplit::Params params;
		GrainSplit::Gpu::Options fullOpts;
		fullOpts.storage = Storage::Full;
		GrainSplit::Gpu::Options compactOpts;
		compactOpts.storage = Storage::Compact;

		Product refFull, refCompact;
		GrainSplit::Gpu::Splitter splitter;   // survives dev1 teardown to verify fresh-device recovery on dev2

		// Phase 1: first WARP device (negative controls, mid-step Release(nullptr), and handle-forgetting verification).
		{
			GpuDevice dev1;
			Assert::IsTrue(dev1.Create(), L"Could not create first libplacebo WARP test device");
			pl_gpu gpu1 = dev1.Gpu();
			TexGuard tex1{ gpu1, UploadLuma(gpu1, y) };
			Assert::IsNotNull(tex1.tex, L"upload plane on dev1");

			// (1) Same-input negative control: without Release(nullptr), Step(0), Step(1), Step(2)..Step(6) succeed
			// continuously and produce a valid Frame and reference outputs for both Full and Compact layouts.
			{
				GrainSplit::Gpu::Splitter control;
				SplitterGuard controlGuard{ gpu1, &control };
				Assert::IsTrue(control.Step(gpu1, dev1.dispatch, tex1.tex, 0, kCodeUnit, scale, B, params, fullOpts, 0u),
					Wide("control Step(0): " + control.LastError()).c_str());
				Assert::IsTrue(control.Step(gpu1, dev1.dispatch, tex1.tex, 0, kCodeUnit, scale, B, params, fullOpts, 1u),
					Wide("control Step(1): " + control.LastError()).c_str());
				Assert::IsTrue(control.Step(gpu1, dev1.dispatch, tex1.tex, 0, kCodeUnit, scale, B, params, fullOpts, 2u),
					Wide("negative control: without Release(nullptr), Step(2) must continue the active run: " + control.LastError()).c_str());
				for (uint32_t s = 3u; s < GrainSplit::Gpu::Splitter::StepCount(); ++s)
				{
					Assert::IsTrue(control.Step(gpu1, dev1.dispatch, tex1.tex, 0, kCodeUnit, scale, B, params, fullOpts, s),
						Wide("control Step(" + std::to_string(s) + "): " + control.LastError()).c_str());
				}
				const GrainSplit::Gpu::Frame& cf = control.Out();
				Assert::IsNotNull(cf.split, L"control Full Out().split");
				Assert::IsNotNull(cf.blocks, L"control Full Out().blocks");
				Assert::AreEqual(w, cf.w, L"control Full Out().w");
				Assert::AreEqual(h, cf.h, L"control Full Out().h");
				Assert::AreEqual(GrainSplit::kUnitVersion, cf.version, L"control Full Out().version");
				refFull.a = DownloadRGBA(gpu1, dev1.dispatch, cf.split);
				refFull.blocks = DownloadRGBA(gpu1, dev1.dispatch, cf.blocks);
				Assert::IsTrue(!refFull.a.empty() && !refFull.blocks.empty(), L"control Full download");

				Assert::IsTrue(control.Compute(gpu1, dev1.dispatch, tex1.tex, 0, kCodeUnit, scale, B, params, compactOpts),
					Wide("control Compact Compute: " + control.LastError()).c_str());
				const GrainSplit::Gpu::Frame& cc = control.Out();
				Assert::IsNotNull(cc.split, L"control Compact Out().split");
				Assert::IsNotNull(cc.splitB, L"control Compact Out().splitB");
				Assert::IsNotNull(cc.blocks, L"control Compact Out().blocks");
				refCompact.a = DownloadRGBA(gpu1, dev1.dispatch, cc.split);
				refCompact.b = DownloadRGBA(gpu1, dev1.dispatch, cc.splitB);
				refCompact.blocks = DownloadRGBA(gpu1, dev1.dispatch, cc.blocks);
				Assert::IsTrue(!refCompact.a.empty() && !refCompact.b.empty() && !refCompact.blocks.empty(), L"control Compact download");
			}

			// (2) Populate all 9 private texture handles (Compact allocates m_tmp..m_level, m_blocks, m_outA, m_outB;
			// starting a Full stepped run also allocates m_outFull while leaving m_run active mid-step at next = 2).
			Assert::IsTrue(splitter.Compute(gpu1, dev1.dispatch, tex1.tex, 0, kCodeUnit, scale, B, params, compactOpts),
				Wide("splitter Compact Compute on dev1: " + splitter.LastError()).c_str());
			Assert::IsNotNull(splitter.Out().split, L"pre-release Compact Out().split is live");
			Assert::IsNotNull(splitter.Out().splitB, L"pre-release Compact Out().splitB is live");
			Assert::IsNotNull(splitter.Out().blocks, L"pre-release Compact Out().blocks is live");
			Assert::AreEqual(w, splitter.Out().w, L"pre-release Out().w");
			Assert::AreEqual(h, splitter.Out().h, L"pre-release Out().h");
			Assert::AreEqual(GrainSplit::kUnitVersion, splitter.Out().version, L"pre-release Out().version");

			Assert::IsTrue(splitter.Step(gpu1, dev1.dispatch, tex1.tex, 0, kCodeUnit, scale, B, params, fullOpts, 0u),
				Wide("splitter Step(0) on dev1: " + splitter.LastError()).c_str());
			Assert::IsTrue(splitter.Step(gpu1, dev1.dispatch, tex1.tex, 0, kCodeUnit, scale, B, params, fullOpts, 1u),
				Wide("splitter Step(1) confirms active run with live textures before Release(nullptr): " + splitter.LastError()).c_str());

			// Mid-step Release(nullptr): simulates lost/detached GPU where pl_tex_destroy cannot be called.
			splitter.Release(nullptr);
			Assert::IsNull(splitter.Out().split, L"Release(nullptr) clears Out().split");
			Assert::IsNull(splitter.Out().splitB, L"Release(nullptr) clears Out().splitB");
			Assert::IsNull(splitter.Out().blocks, L"Release(nullptr) clears Out().blocks");
			Assert::AreEqual(0, splitter.Out().w, L"Release(nullptr) clears Out().w");
			Assert::AreEqual(0, splitter.Out().h, L"Release(nullptr) clears Out().h");
			Assert::AreEqual(static_cast<uint32_t>(0), splitter.Out().version, L"Release(nullptr) clears Out().version");

			// Pre-fix bug #1: Release(nullptr) left m_run.active = true and m_run.next = 2, so Step(2) succeeded
			// using stale handles instead of failing cleanly with "step out of order".
			const bool continuedAfterNullRelease = splitter.Step(gpu1, dev1.dispatch, tex1.tex, 0, kCodeUnit, scale, B, params, fullOpts, 2u);
			Assert::IsFalse(continuedAfterNullRelease,
				L"Step(2) after mid-step Release(nullptr) must fail rather than continuing the abandoned run");
			Assert::IsTrue(splitter.LastError().find("step out of order") != std::string::npos,
				Wide("expected 'step out of order' after Release(nullptr), got: " + splitter.LastError()).c_str());
			Assert::IsNull(splitter.Out().split, L"failed Step(2) leaves Out().split null");
			Assert::IsNull(splitter.Out().splitB, L"failed Step(2) leaves Out().splitB null");
			Assert::IsNull(splitter.Out().blocks, L"failed Step(2) leaves Out().blocks null");

			// (3) Pre-fix bug #2 deterministic pointer-identity proof (zero UB): without Release(nullptr), pl_tex_recreate
			// reuses cached non-null output handles; after Release(nullptr), all cached handles must be forgotten so
			// subsequent Compute allocates fresh pl_tex handles at distinct addresses while the abandoned ones remain live.
			{
				GrainSplit::Gpu::Splitter ptrProbe;
				Assert::IsTrue(ptrProbe.Compute(gpu1, dev1.dispatch, tex1.tex, 0, kCodeUnit, scale, B, params, fullOpts),
					Wide("ptrProbe Full Compute 1: " + ptrProbe.LastError()).c_str());
				pl_tex oldFullSplit = ptrProbe.Out().split;
				pl_tex oldFullBlocks = ptrProbe.Out().blocks;
				Assert::IsNotNull(oldFullSplit, L"ptrProbe oldFullSplit");
				Assert::IsNotNull(oldFullBlocks, L"ptrProbe oldFullBlocks");

				Assert::IsTrue(ptrProbe.Compute(gpu1, dev1.dispatch, tex1.tex, 0, kCodeUnit, scale, B, params, compactOpts),
					Wide("ptrProbe Compact Compute 1: " + ptrProbe.LastError()).c_str());
				pl_tex oldCompactA = ptrProbe.Out().split;
				pl_tex oldCompactB = ptrProbe.Out().splitB;
				Assert::IsNotNull(oldCompactA, L"ptrProbe oldCompactA");
				Assert::IsNotNull(oldCompactB, L"ptrProbe oldCompactB");

				// Negative control: without Release(nullptr), cached handles are reused identically by pl_tex_recreate.
				Assert::IsTrue(ptrProbe.Compute(gpu1, dev1.dispatch, tex1.tex, 0, kCodeUnit, scale, B, params, compactOpts),
					Wide("ptrProbe Compact Compute negative control: " + ptrProbe.LastError()).c_str());
				Assert::IsTrue(ptrProbe.Out().split == oldCompactA,
					L"negative control: without Release(nullptr), Splitter reuses cached m_outA handle");
				Assert::IsTrue(ptrProbe.Out().splitB == oldCompactB,
					L"negative control: without Release(nullptr), Splitter reuses cached m_outB handle");
				Assert::IsTrue(ptrProbe.Out().blocks == oldFullBlocks,
					L"negative control: without Release(nullptr), Splitter reuses cached m_blocks handle");

				ptrProbe.Release(nullptr);
				Assert::IsNull(ptrProbe.Out().split, L"ptrProbe Release(nullptr) clears Out().split");
				Assert::IsNull(ptrProbe.Out().splitB, L"ptrProbe Release(nullptr) clears Out().splitB");
				Assert::IsNull(ptrProbe.Out().blocks, L"ptrProbe Release(nullptr) clears Out().blocks");

				// After Release(nullptr), private pointers must be nulled so pl_tex_recreate allocates new handles
				// rather than reusing the old ones (which were not destroyed because gpu was nullptr).
				Assert::IsTrue(ptrProbe.Compute(gpu1, dev1.dispatch, tex1.tex, 0, kCodeUnit, scale, B, params, fullOpts),
					Wide("ptrProbe Full Compute after Release(nullptr): " + ptrProbe.LastError()).c_str());
				Assert::IsNotNull(ptrProbe.Out().split, L"ptrProbe newFullSplit");
				Assert::IsNotNull(ptrProbe.Out().blocks, L"ptrProbe newFullBlocks");
				Assert::IsTrue(ptrProbe.Out().split != oldFullSplit,
					L"Release(nullptr) must null m_outFull so a new texture is allocated instead of reusing the old handle");
				Assert::IsTrue(ptrProbe.Out().blocks != oldFullBlocks,
					L"Release(nullptr) must null m_blocks so a new texture is allocated instead of reusing the old handle");

				Assert::IsTrue(ptrProbe.Compute(gpu1, dev1.dispatch, tex1.tex, 0, kCodeUnit, scale, B, params, compactOpts),
					Wide("ptrProbe Compact Compute after Release(nullptr): " + ptrProbe.LastError()).c_str());
				Assert::IsNotNull(ptrProbe.Out().split, L"ptrProbe newCompactA");
				Assert::IsNotNull(ptrProbe.Out().splitB, L"ptrProbe newCompactB");
				Assert::IsTrue(ptrProbe.Out().split != oldCompactA,
					L"Release(nullptr) must null m_outA so a new texture is allocated instead of reusing the old handle");
				Assert::IsTrue(ptrProbe.Out().splitB != oldCompactB,
					L"Release(nullptr) must null m_outB so a new texture is allocated instead of reusing the old handle");

				// Clean up both the newly created textures and the four saved old output textures (proven distinct above).
				ptrProbe.Release(gpu1);
				pl_tex_destroy(gpu1, &oldFullSplit);
				pl_tex_destroy(gpu1, &oldFullBlocks);
				pl_tex_destroy(gpu1, &oldCompactA);
				pl_tex_destroy(gpu1, &oldCompactB);
			}
		}   // dev1 is now completely destroyed (simulating real device loss)

		// Phase 2: create a brand-new WARP device and reuse the SAME Splitter instance across all 9 texture slots.
		{
			GpuDevice dev2;
			Assert::IsTrue(dev2.Create(), L"Could not create second libplacebo WARP test device");
			pl_gpu gpu2 = dev2.Gpu();
			SplitterGuard splitterGuard{ gpu2, &splitter };
			TexGuard tex2{ gpu2, UploadLuma(gpu2, y) };
			Assert::IsNotNull(tex2.tex, L"upload plane on dev2");

			// Full Compute on dev2 exercises Ensure on m_tmp, m_low, m_part, m_quiet, m_level, m_blocks, m_outFull.
			Assert::IsTrue(splitter.Compute(gpu2, dev2.dispatch, tex2.tex, 0, kCodeUnit, scale, B, params, fullOpts),
				Wide("Full Compute on fresh device after Release(nullptr) failed: " + splitter.LastError()).c_str());
			const GrainSplit::Gpu::Frame& f2 = splitter.Out();
			Assert::IsNotNull(f2.split, L"dev2 Full Out().split");
			Assert::IsNull(f2.splitB, L"dev2 Full Out().splitB is null");
			Assert::IsNotNull(f2.blocks, L"dev2 Full Out().blocks");
			Assert::AreEqual(w, f2.w, L"dev2 Full Out().w");
			Assert::AreEqual(h, f2.h, L"dev2 Full Out().h");
			Assert::AreEqual(GrainSplit::kUnitVersion, f2.version, L"dev2 Full Out().version");
			const std::vector<float> gotFullA = DownloadRGBA(gpu2, dev2.dispatch, f2.split);
			const std::vector<float> gotFullBlocks = DownloadRGBA(gpu2, dev2.dispatch, f2.blocks);
			Assert::IsTrue(SameBits(refFull.a, gotFullA) && SameBits(refFull.blocks, gotFullBlocks),
				L"Full output on fresh device after Release(nullptr) must be bit-identical to dev1 reference");

			// Compact Compute on dev2 also exercises Ensure on m_outA and m_outB (covering all 9 m_* handles).
			Assert::IsTrue(splitter.Compute(gpu2, dev2.dispatch, tex2.tex, 0, kCodeUnit, scale, B, params, compactOpts),
				Wide("Compact Compute on fresh device after Release(nullptr) failed: " + splitter.LastError()).c_str());
			const GrainSplit::Gpu::Frame& c2 = splitter.Out();
			Assert::IsNotNull(c2.split, L"dev2 Compact Out().split");
			Assert::IsNotNull(c2.splitB, L"dev2 Compact Out().splitB");
			Assert::IsNotNull(c2.blocks, L"dev2 Compact Out().blocks");
			const std::vector<float> gotCompactA = DownloadRGBA(gpu2, dev2.dispatch, c2.split);
			const std::vector<float> gotCompactB = DownloadRGBA(gpu2, dev2.dispatch, c2.splitB);
			const std::vector<float> gotCompactBlocks = DownloadRGBA(gpu2, dev2.dispatch, c2.blocks);
			Assert::IsTrue(SameBits(refCompact.a, gotCompactA) && SameBits(refCompact.b, gotCompactB) && SameBits(refCompact.blocks, gotCompactBlocks),
				L"Compact output on fresh device after Release(nullptr) must be bit-identical to dev1 reference");

			// Stepped loop on dev2 also recovers cleanly and matches Full reference bit-for-bit.
			for (uint32_t s = 0u; s < GrainSplit::Gpu::Splitter::StepCount(); ++s)
			{
				Assert::IsTrue(splitter.Step(gpu2, dev2.dispatch, tex2.tex, 0, kCodeUnit, scale, B, params, fullOpts, s),
					Wide("dev2 Step(" + std::to_string(s) + "): " + splitter.LastError()).c_str());
			}
			const std::vector<float> gotSteppedA = DownloadRGBA(gpu2, dev2.dispatch, splitter.Out().split);
			const std::vector<float> gotSteppedBlocks = DownloadRGBA(gpu2, dev2.dispatch, splitter.Out().blocks);
			Assert::IsTrue(SameBits(refFull.a, gotSteppedA) && SameBits(refFull.blocks, gotSteppedBlocks),
				L"stepped run on fresh device after Release(nullptr) must match dev1 reference bit-for-bit");

			// Mid-step Release(validGpu) must also reset m_run so Step(2) fails cleanly instead of touching freed handles.
			Assert::IsTrue(splitter.Step(gpu2, dev2.dispatch, tex2.tex, 0, kCodeUnit, scale, B, params, fullOpts, 0u),
				Wide("dev2 pre-Release(gpu2) Step(0): " + splitter.LastError()).c_str());
			Assert::IsTrue(splitter.Step(gpu2, dev2.dispatch, tex2.tex, 0, kCodeUnit, scale, B, params, fullOpts, 1u),
				Wide("dev2 pre-Release(gpu2) Step(1): " + splitter.LastError()).c_str());
			splitter.Release(gpu2);
			Assert::IsNull(splitter.Out().split, L"Release(gpu2) clears Out().split");
			Assert::IsNull(splitter.Out().blocks, L"Release(gpu2) clears Out().blocks");
			Assert::AreEqual(static_cast<uint32_t>(0), splitter.Out().version, L"Release(gpu2) clears Out().version");
			Assert::IsFalse(splitter.Step(gpu2, dev2.dispatch, tex2.tex, 0, kCodeUnit, scale, B, params, fullOpts, 2u),
				L"Step(2) after mid-step Release(gpu2) must fail cleanly");
			Assert::IsTrue(splitter.LastError().find("step out of order") != std::string::npos,
				Wide("expected 'step out of order' after Release(gpu2), got: " + splitter.LastError()).c_str());

			Logger::WriteMessage("S2-H7: mid-step Release(nullptr) cleared active run + all 9 handles and recovered bit-identically on fresh WARP device");
		}
	}

	// S2-M2: Splitter::Begin and StepOn must visibly reject non-finite (+Inf/-Inf/NaN and overflowing reciprocals/products)
	// scale, codeUnit, and Params fields before any GPU pass or GaussRadius cast, clear Out(), report "non-finite" in
	// LastError(), and recover cleanly on the same Splitter instance with valid finite arguments.
	BEGIN_TEST_METHOD_ATTRIBUTE(SplitterRejectsInfiniteScaleCodeUnitAndSigmasVisibly)
		TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
	END_TEST_METHOD_ATTRIBUTE()
	TEST_METHOD(SplitterRejectsInfiniteScaleCodeUnitAndSigmasVisibly)
	{
		using namespace GrainSplitV3GpuTestsNs;
		struct TexGuard
		{
			pl_gpu gpu = nullptr;
			pl_tex tex = nullptr;
			TexGuard(pl_gpu g, pl_tex t) : gpu(g), tex(t) {}
			~TexGuard() { if (gpu && tex) pl_tex_destroy(gpu, &tex); }
			TexGuard(const TexGuard&) = delete;
			TexGuard& operator=(const TexGuard&) = delete;
		};
		struct SplitterGuard
		{
			pl_gpu gpu = nullptr;
			GrainSplit::Gpu::Splitter* s = nullptr;
			SplitterGuard(pl_gpu g, GrainSplit::Gpu::Splitter* sp) : gpu(g), s(sp) {}
			~SplitterGuard() { if (gpu && s) s->Release(gpu); }
			SplitterGuard(const SplitterGuard&) = delete;
			SplitterGuard& operator=(const SplitterGuard&) = delete;
		};

		GpuDevice dev;
		Assert::IsTrue(dev.Create(), L"Could not create the libplacebo WARP test device");
		pl_gpu gpu = dev.Gpu();

		const int w = 64, h = 64, B = 16;
		const double validScale = 1.0;
		const double validCodeUnit = 1.0 / 1023.0;
		const GrainSplit::Params validParams{};
		const Plane y = MakePlane(w, h, 823u);
		TexGuard tex{ gpu, UploadLuma(gpu, y) };
		Assert::IsNotNull(tex.tex, L"upload plane");

		GrainSplit::Gpu::Splitter splitter;
		SplitterGuard splitterGuard{ gpu, &splitter };

		// (1) Positive control on the exact same texture: valid finite arguments succeed and populate Out().
		Assert::IsTrue(splitter.Compute(gpu, dev.dispatch, tex.tex, 0, validCodeUnit, validScale, B, validParams),
			Wide("positive control Compute failed: " + splitter.LastError()).c_str());
		Assert::IsTrue(splitter.LastError().empty(),
			Wide("positive control must clear LastError, got: " + splitter.LastError()).c_str());
		Assert::IsNotNull(splitter.Out().split, L"positive control Out().split");
		Assert::IsNotNull(splitter.Out().blocks, L"positive control Out().blocks");
		Assert::AreEqual(w, splitter.Out().w, L"positive control Out().w");
		Assert::AreEqual(h, splitter.Out().h, L"positive control Out().h");
		Assert::AreEqual(GrainSplit::kUnitVersion, splitter.Out().version, L"positive control Out().version");
		const std::vector<float> refSplit = DownloadRGBA(gpu, dev.dispatch, splitter.Out().split);
		const std::vector<float> refBlocks = DownloadRGBA(gpu, dev.dispatch, splitter.Out().blocks);
		Assert::IsTrue(!refSplit.empty() && !refBlocks.empty(), L"positive control download");

		// (2) Non-finite adversarial inputs on the exact same texture and Splitter instance.
		const double kInf = std::numeric_limits<double>::infinity();
		const double kNan = std::numeric_limits<double>::quiet_NaN();
		auto withParam = [&](auto mutator) {
			GrainSplit::Params p = validParams;
			mutator(p);
			return p;
		};
		struct BadCase
		{
			const char* label;
			double codeUnit;
			double scale;
			GrainSplit::Params params;
		};
		const BadCase badCases[] = {
			{ "scale=+Inf", validCodeUnit, kInf, validParams },
			{ "scale=-Inf", validCodeUnit, -kInf, validParams },
			{ "scale=NaN", validCodeUnit, kNan, validParams },
			{ "codeUnit=+Inf", kInf, validScale, validParams },
			{ "codeUnit=1e-320 (1/codeUnit overflows to +Inf)", 1e-320, validScale, validParams },
			{ "params.sigmaFine=+Inf", validCodeUnit, validScale, withParam([&](GrainSplit::Params& p) { p.sigmaFine = kInf; }) },
			{ "params.sigmaBase=+Inf", validCodeUnit, validScale, withParam([&](GrainSplit::Params& p) { p.sigmaBase = kInf; }) },
			{ "params.sigmaLow=+Inf", validCodeUnit, validScale, withParam([&](GrainSplit::Params& p) { p.sigmaLow = kInf; }) },
			{ "params.grainCal=+Inf", validCodeUnit, validScale, withParam([&](GrainSplit::Params& p) { p.grainCal = kInf; }) },
			{ "params.kappa=NaN", validCodeUnit, validScale, withParam([&](GrainSplit::Params& p) { p.kappa = kNan; }) },
			{ "params.energyFloor=+Inf", validCodeUnit, validScale, withParam([&](GrainSplit::Params& p) { p.energyFloor = kInf; }) },
			{ "params.sigmaFine=1e200 (s1*s1 overflows to +Inf)", validCodeUnit, validScale, withParam([&](GrainSplit::Params& p) { p.sigmaFine = 1e200; }) },
		};
		for (const BadCase& c : badCases)
		{
			const std::wstring tag = Wide(std::string(c.label));
			const bool ok = splitter.Compute(gpu, dev.dispatch, tex.tex, 0, c.codeUnit, c.scale, B, c.params);
			Assert::IsFalse(ok, (tag + L": non-finite input must be refused").c_str());
			Assert::IsNull(splitter.Out().split, (tag + L": refused Compute must leave Out().split null").c_str());
			Assert::IsNull(splitter.Out().splitB, (tag + L": refused Compute must leave Out().splitB null").c_str());
			Assert::IsNull(splitter.Out().blocks, (tag + L": refused Compute must leave Out().blocks null").c_str());
			Assert::AreEqual(static_cast<uint32_t>(0), splitter.Out().version, (tag + L": refused Compute must clear Out().version").c_str());
			Assert::IsFalse(splitter.LastError().empty(), (tag + L": refused Compute must populate LastError()").c_str());
			Assert::IsTrue(splitter.LastError().find("non-finite") != std::string::npos,
				Wide(std::string(c.label) + ": expected LastError() to name 'non-finite', got: " + splitter.LastError()).c_str());
		}

		// Mid-step non-finite rejection: Step(0) succeeds validly, then Step(1) with scale=+Inf aborts the active run visibly.
		const GrainSplit::Gpu::Options defaultOpts{};
		Assert::IsTrue(splitter.Step(gpu, dev.dispatch, tex.tex, 0, validCodeUnit, validScale, B, validParams, defaultOpts, 0u),
			Wide("mid-step setup Step(0) failed: " + splitter.LastError()).c_str());
		Assert::IsFalse(splitter.Step(gpu, dev.dispatch, tex.tex, 0, validCodeUnit, kInf, B, validParams, defaultOpts, 1u),
			L"mid-step Step(1) with scale=+Inf must be refused");
		Assert::IsNull(splitter.Out().split, L"mid-step non-finite refusal leaves Out().split null");
		Assert::IsNull(splitter.Out().blocks, L"mid-step non-finite refusal leaves Out().blocks null");
		Assert::IsTrue(splitter.LastError().find("non-finite") != std::string::npos,
			Wide("mid-step Step(1) with scale=+Inf expected 'non-finite', got: " + splitter.LastError()).c_str());
		Assert::IsFalse(splitter.Step(gpu, dev.dispatch, tex.tex, 0, validCodeUnit, validScale, B, validParams, defaultOpts, 2u),
			L"aborted stepped run must not allow Step(2) to continue");

		// (3) Recovery check: the same Splitter instance succeeds cleanly on valid finite input and matches the baseline bit-for-bit.
		Assert::IsTrue(splitter.Compute(gpu, dev.dispatch, tex.tex, 0, validCodeUnit, validScale, B, validParams),
			Wide("recovery Compute after non-finite refusals failed: " + splitter.LastError()).c_str());
		Assert::IsTrue(splitter.LastError().empty(),
			Wide("recovery Compute must clear LastError, got: " + splitter.LastError()).c_str());
		Assert::IsNotNull(splitter.Out().split, L"recovery Out().split");
		Assert::IsNotNull(splitter.Out().blocks, L"recovery Out().blocks");
		Assert::AreEqual(w, splitter.Out().w, L"recovery Out().w");
		Assert::AreEqual(h, splitter.Out().h, L"recovery Out().h");
		Assert::AreEqual(GrainSplit::kUnitVersion, splitter.Out().version, L"recovery Out().version");
		const std::vector<float> recSplit = DownloadRGBA(gpu, dev.dispatch, splitter.Out().split);
		const std::vector<float> recBlocks = DownloadRGBA(gpu, dev.dispatch, splitter.Out().blocks);
		Assert::IsTrue(SameBits(refSplit, recSplit) && SameBits(refBlocks, recBlocks),
			L"recovery Compute after non-finite refusals must match positive-control output bit-for-bit");

		Logger::WriteMessage("S2-M2: Splitter visibly rejected all 12 non-finite Compute cases + mid-step +Inf and recovered bit-identically");
	}
};
