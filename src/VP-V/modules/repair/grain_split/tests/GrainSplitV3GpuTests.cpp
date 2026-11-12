// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// repair.grain_split v3 GPU kernel against its CPU law (P10 v3, refs/116 s5): the five-pass kernel (HALF+across, DOWN, STATS,
// NMIN, APPLY) in both layouts (Full, Compact) on a libplacebo device (WARP unless VP_TEST_GPU=hardware) against
// GrainSplit::Compute (the v3 law in doubles): odd and even plane sizes, block edges 8 / 16 / 32 with the lane law's scale B / 16.
//   V3GpuMatchesCpuReference  max |s1|, |s2|, |base| <= 0.05 code, f <= 1/255, N and e3 / e1 / e2 relative <= 1e-3.
//   V3GpuIsDeterministic      two runs: bit-identical product and blocks.
//   V3StepLoopEqualsCompute   Step(0..4) is bit-identical to Compute.
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
			std::string name;
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

	// One run on the device: Splitter::Compute, or (stepLoop) the five Step calls with the same arguments.
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

	// (c) Step(0..4) with the same arguments records exactly Compute's dispatches: bit-identical output.
	TEST_METHOD(V3StepLoopEqualsCompute)
	{
		using namespace GrainSplitV3GpuTestsNs;
		GpuDevice dev;
		Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
		pl_gpu gpu = dev.Gpu();
		Assert::AreEqual(static_cast<uint32_t>(5), GrainSplit::Gpu::Splitter::StepCount(), L"five steps");
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
				Assert::IsTrue(same, L"Step(0..4) is bit-identical to Compute");
			}
			pl_tex_destroy(gpu, &tex);
		}
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
};
