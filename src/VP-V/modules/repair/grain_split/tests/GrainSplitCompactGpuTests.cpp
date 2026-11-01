// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// repair.grain_split GPU storage layouts (SA2 part B1): Full (rgba32f split + rgba32f blocks, today) against Compact (rg16f A +
// rg16 unorm B + rg32f blocks) on a libplacebo device (WARP unless VP_TEST_GPU=hardware): fidelity of the decoded product,
// bit-identity of Full with today's path (also with the scratch arena), determinism, the visible arena shortage and its
// recovery, caller-supplied target textures, and the one read accessor (GrainSplit::Gpu::ViewGlsl / BindView) run as a real
// shader in both layouts. Every measured number is written to the test log.
#include "pch.h"
#include "CppUnitTest.h"
#include "ExtendedRangeGpuProbe.h"

#include <VP-V/modules/repair/grain_split/GrainSplit.h>
#include <VP-V/modules/repair/grain_split/GrainSplitGpu.h>
#include <VP-V/modules/repair/grain_split/GpuPassUtil.h>

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

namespace GrainSplitCompactGpuTestsNs
{
	using GrainSplit::Plane;
	using GrainSplit::Storage;

	constexpr double kPi = 3.14159265358979323846;
	constexpr double kCodeUnit = 64.0 / 65535.0;   // 10-bit codes in a 16-bit plane, 1/64-code steps (as the existing grain split test)

	void LogF(const char* fmt, double a = 0, double b = 0, double c = 0, double d = 0, double e = 0, double f = 0)
	{
		char line[384];
		std::snprintf(line, sizeof line, fmt, a, b, c, d, e, f);
		Logger::WriteMessage(line);
	}
	std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }

	// The block-edge rule (TemporalDetail kBlockPx x Scale(planeLines)): 16 px at <= 2160 lines, scaled above.
	int BlockEdge(int planeLines)
	{
		const double scale = planeLines > 2160 ? planeLines / 2160.0 : 1.0;
		return (std::max)(1, static_cast<int>(std::floor(16.0 * scale + 0.5)));
	}

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
			Logger::WriteMessage(("grain split compact GPU tests on: " + name).c_str());
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

	// The product of one Compute, read back: Full: a = (s1, s2, f, base) per texel, b empty; Compact: a = (s1, s2, -, -),
	// b = (f, base x codeUnit, -, -). blocks = (N, e3, ., .) per block (four floats per texel either way).
	struct Product
	{
		bool ok = false;
		std::string error;
		bool scratchShort = false;
		bool compact = false;
		int w = 0, h = 0, bw = 0, bh = 0;
		std::vector<float> a, b, blocks;
	};

	Product Run(GpuDevice& dev, pl_tex tex, int B, Storage storage, VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend>* arena,
		const GrainSplit::Gpu::Targets* targets = nullptr, bool oldEntry = false)
	{
		Product r;
		pl_gpu gpu = dev.Gpu();
		GrainSplit::Gpu::Splitter splitter;
		bool ok;
		if (oldEntry)
			ok = splitter.Compute(gpu, dev.dispatch, tex, 0, kCodeUnit, 1.0, B);
		else
		{
			GrainSplit::Gpu::Options o;
			o.storage = storage;
			o.arena = arena;
			if (targets) o.targets = *targets;
			ok = splitter.Compute(gpu, dev.dispatch, tex, 0, kCodeUnit, 1.0, B, GrainSplit::Params(), o);
		}
		r.ok = ok;
		r.error = splitter.LastError();
		r.scratchShort = splitter.ScratchShort();
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

	// The accessor's decode on the CPU: (s1, s2, f, base) in codes of texel i.
	struct Texel { double s1, s2, f, base; };
	Texel Decode(const Product& p, size_t i)
	{
		if (!p.compact) return { p.a[i * 4], p.a[i * 4 + 1], p.a[i * 4 + 2], p.a[i * 4 + 3] };
		return { p.a[i * 4], p.a[i * 4 + 1], p.b[i * 4], p.b[i * 4 + 1] / kCodeUnit };
	}

	// std and lag-1 horizontal autocorrelation of g over the mask (pairs inside the mask in one row).
	void Moments(const std::vector<double>& g, const std::vector<char>& mask, int w, int h, double& stddev, double& r1, size_t& count)
	{
		double n = 0, s = 0, s2 = 0;
		for (size_t i = 0; i < g.size(); ++i)
			if (mask[i]) { n += 1; s += g[i]; s2 += g[i] * g[i]; }
		count = static_cast<size_t>(n);
		if (n < 2) { stddev = 0; r1 = 0; return; }
		const double mean = s / n, var = s2 / n - mean * mean;
		stddev = std::sqrt((std::max)(0.0, var));
		double cp = 0, np = 0;
		for (int y = 0; y < h; ++y)
			for (int x = 0; x + 1 < w; ++x)
			{
				const size_t i = static_cast<size_t>(y) * w + x;
				if (mask[i] && mask[i + 1]) { cp += (g[i] - mean) * (g[i + 1] - mean); np += 1; }
			}
		r1 = (np > 0 && var > 1e-12) ? (cp / np) / var : 0.0;
	}
}

TEST_CLASS(GrainSplitCompactGpuTests)
{
	using GpuDevice = GrainSplitCompactGpuTestsNs::GpuDevice;

public:
	TEST_METHOD(GrainSplitCompactMatchesFull)
	{
		using namespace GrainSplitCompactGpuTestsNs;
		GpuDevice dev;
		Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
		pl_gpu gpu = dev.Gpu();
		const int sizes[2][2] = { { 256, 144 }, { 320, 180 } };
		for (int si = 0; si < 2; ++si)
		{
			const int w = sizes[si][0], h = sizes[si][1], B = BlockEdge(h);
			const Plane y = MakePlane(w, h, 17u + static_cast<uint32_t>(si));
			pl_tex tex = UploadLuma(gpu, y);
			const Product full = Run(dev, tex, B, Storage::Full, nullptr);
			const Product comp = Run(dev, tex, B, Storage::Compact, nullptr);
			Assert::IsTrue(full.ok, Wide(full.error).c_str());
			Assert::IsTrue(comp.ok, Wide(comp.error).c_str());
			Assert::IsTrue(!full.a.empty() && !comp.a.empty() && !comp.b.empty() && !full.blocks.empty() && !comp.blocks.empty(), L"downloaded");
			const size_t n = y.v.size();
			std::vector<double> gFull(n), gComp(n);
			std::vector<char> clsGrain(n, 0), clsStruct(n, 0), clsMixed(n, 0), clsAll(n, 1);
			double worstClean = 0, worstG = 0, worstS = 0, worstBase = 0, worstF = 0;
			for (size_t i = 0; i < n; ++i)
			{
				const Texel a = Decode(full, i), b = Decode(comp, i);
				const double cleanA = a.base + a.s1 + a.s2, cleanB = b.base + b.s1 + b.s2;
				gFull[i] = y.v[i] - cleanA;
				gComp[i] = y.v[i] - cleanB;
				worstClean = (std::max)(worstClean, std::fabs(cleanB - cleanA));
				worstG = (std::max)(worstG, std::fabs(gComp[i] - gFull[i]));
				worstS = (std::max)(worstS, (std::max)(std::fabs(b.s1 - a.s1), std::fabs(b.s2 - a.s2)));
				worstBase = (std::max)(worstBase, std::fabs(b.base - a.base));
				worstF = (std::max)(worstF, std::fabs(b.f - a.f));
				if (a.f < 0.1) clsGrain[i] = 1; else if (a.f > 0.9) clsStruct[i] = 1; else clsMixed[i] = 1;
			}
			LogF("compact vs full %.0fx%.0f B %.0f: max |clean| err %.4f codes, max |G| err %.4f codes", w, h, B, worstClean, worstG);
			LogF("  max |s1,s2| err %.4f codes, |base| err %.4f codes, |f| err %.6f", worstS, worstBase, worstF);
			Assert::IsTrue(worstClean <= 0.5, L"max |clean| error within 0.5 code");
			Assert::IsTrue(worstG <= 0.5, L"max |G| error within 0.5 code");

			struct Cls { const char* name; const std::vector<char>* mask; };
			const Cls classes[4] = { { "all", &clsAll }, { "grain-only (f<0.1)", &clsGrain }, { "mixed", &clsMixed }, { "structure (f>0.9)", &clsStruct } };
			for (const Cls& c : classes)
			{
				double sdF, r1F, sdC, r1C;
				size_t cnt, cnt2;
				Moments(gFull, *c.mask, w, h, sdF, r1F, cnt);
				Moments(gComp, *c.mask, w, h, sdC, r1C, cnt2);
				char line[320];
				std::snprintf(line, sizeof line, "  class %-20s %6zu px: std(G) full %.4f compact %.4f (%.3f %%), lag-1 full %.4f compact %.4f (delta %.4f)", c.name, cnt, sdF,
					sdC, sdF > 0 ? 100.0 * std::fabs(sdC - sdF) / sdF : 0.0, r1F, r1C, std::fabs(r1C - r1F));
				Logger::WriteMessage(line);
				if (cnt < 200) { Logger::WriteMessage("    (class skipped: fewer than 200 px)"); continue; }
				Assert::IsTrue(std::fabs(sdC - sdF) <= 0.05 * sdF, Wide(std::string("std(G) within 5 % in class ") + c.name + ": " + line).c_str());
				// within 5 % of the full value (an absolute floor of 0.005 for a correlation near zero)
				Assert::IsTrue(std::fabs(r1C - r1F) <= (std::max)(0.05 * std::fabs(r1F), 0.005), Wide(std::string("lag-1 autocorrelation within 5 % in class ") + c.name + ": " + line).c_str());
			}
			// the block texels are bit-identical (N and e3: the .x and .y of the texel)
			bool blocksSame = full.blocks.size() == comp.blocks.size();
			size_t differing = 0;
			for (size_t i = 0; blocksSame && i < static_cast<size_t>(full.bw) * full.bh; ++i)
				if (full.blocks[i * 4] != comp.blocks[i * 4] || full.blocks[i * 4 + 1] != comp.blocks[i * 4 + 1]) ++differing;
			LogF("  blocks %.0f x %.0f: %.0f texels differ (N, e3 exact equality)", full.bw, full.bh, static_cast<double>(differing));
			Assert::IsTrue(blocksSame && differing == 0, L"N and e3 of Compact are bit-identical to Full's");
			pl_tex_destroy(gpu, &tex);
		}
	}

	TEST_METHOD(GrainSplitFullIsBitIdenticalToToday)
	{
		using namespace GrainSplitCompactGpuTestsNs;
		GpuDevice dev;
		Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
		pl_gpu gpu = dev.Gpu();
		VpSkeleton::PlTexBackend backend(gpu);
		VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend> arena(backend);   // destroyed before the device
		const int sizes[2][2] = { { 256, 144 }, { 320, 180 } };
		for (int si = 0; si < 2; ++si)
		{
			const int w = sizes[si][0], h = sizes[si][1], B = BlockEdge(h);
			const Plane y = MakePlane(w, h, 31u + static_cast<uint32_t>(si));
			pl_tex tex = UploadLuma(gpu, y);
			const Product today = Run(dev, tex, B, Storage::Full, nullptr, nullptr, true);
			const Product viaNew = Run(dev, tex, B, Storage::Full, nullptr);
			Assert::IsTrue(today.ok && viaNew.ok, L"Full computes through both entries");
			Assert::IsTrue(SameBits(today.a, viaNew.a) && SameBits(today.blocks, viaNew.blocks), L"default Options: split and blocks exactly equal to the 8-arg Compute");
			Assert::IsTrue(arena.Reserve(VpSkeleton::PlanArena(GrainSplit::Gpu::Splitter::ArenaRequests(gpu, w, h, B, Storage::Full), VpSkeleton::Tier::T2)), L"arena reserve");
			arena.BeginFrame();
			const uint64_t before = arena.Acquired();
			const Product viaArena = Run(dev, tex, B, Storage::Full, &arena);
			Assert::IsTrue(viaArena.ok, Wide(viaArena.error).c_str());
			LogF("Full via arena %.0fx%.0f: %.0f acquires (bands, quiet), arena %.1f MB", w, h, static_cast<double>(arena.Acquired() - before),
				static_cast<double>(arena.ReservedBytes()) / (1024.0 * 1024.0));
			Assert::AreEqual(static_cast<uint64_t>(2), arena.Acquired() - before, L"Full takes bands and quiet from the arena (rows stays the product)");
			Assert::IsTrue(SameBits(today.a, viaArena.a) && SameBits(today.blocks, viaArena.blocks), L"arena intermediates: split and blocks exactly equal");
			Assert::IsFalse(arena.FrameDecision().engaged, L"no shortage");
			pl_tex_destroy(gpu, &tex);
		}
	}

	TEST_METHOD(GrainSplitCompactIsDeterministic)
	{
		using namespace GrainSplitCompactGpuTestsNs;
		GpuDevice dev;
		Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
		pl_gpu gpu = dev.Gpu();
		const int w = 256, h = 144, B = BlockEdge(h);
		const Plane y = MakePlane(w, h, 5u);
		pl_tex tex = UploadLuma(gpu, y);
		const Product r1 = Run(dev, tex, B, Storage::Compact, nullptr);
		const Product r2 = Run(dev, tex, B, Storage::Compact, nullptr);
		Assert::IsTrue(r1.ok && r2.ok, L"Compact computes");
		Assert::IsTrue(SameBits(r1.a, r2.a), L"texture A identical");
		Assert::IsTrue(SameBits(r1.b, r2.b), L"texture B identical");
		Assert::IsTrue(SameBits(r1.blocks, r2.blocks), L"blocks identical");
		// the same through an arena (temporaries from pooled textures never change the result)
		VpSkeleton::PlTexBackend backend(gpu);
		VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend> arena(backend);
		Assert::IsTrue(arena.Reserve(VpSkeleton::PlanArena(GrainSplit::Gpu::Splitter::ArenaRequests(gpu, w, h, B, Storage::Compact), VpSkeleton::Tier::T2)), L"arena reserve");
		const uint64_t before = arena.Acquired();
		const Product r3 = Run(dev, tex, B, Storage::Compact, &arena);
		Assert::IsTrue(r3.ok, Wide(r3.error).c_str());
		Assert::AreEqual(static_cast<uint64_t>(3), arena.Acquired() - before, L"Compact takes rows, bands and quiet from the arena");
		Assert::IsTrue(SameBits(r1.a, r3.a) && SameBits(r1.b, r3.b) && SameBits(r1.blocks, r3.blocks), L"arena result equals the private result");
		Logger::WriteMessage("Compact: two private runs and one arena run are bit-identical (A, B, blocks)");
		pl_tex_destroy(gpu, &tex);
	}

	TEST_METHOD(GrainSplitArenaShortageIsVisible)
	{
		using namespace GrainSplitCompactGpuTestsNs;
		GpuDevice dev;
		Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
		pl_gpu gpu = dev.Gpu();
		const int w = 256, h = 144, B = BlockEdge(h);
		const Plane y = MakePlane(w, h, 9u);
		pl_tex tex = UploadLuma(gpu, y);
		const Product reference = Run(dev, tex, B, Storage::Compact, nullptr);
		Assert::IsTrue(reference.ok, L"private Compact computes");
		const Product referenceFull = Run(dev, tex, B, Storage::Full, nullptr);

		VpSkeleton::PlTexBackend backend(gpu);
		VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend> arena(backend);
		Assert::IsTrue(arena.Reserve(VpSkeleton::PlanArena({}, VpSkeleton::Tier::T2)), L"empty plan reserves");
		arena.BeginFrame();
		const Product shortC = Run(dev, tex, B, Storage::Compact, &arena);
		Assert::IsFalse(shortC.ok, L"Compact with an empty plan fails (no private temporaries)");
		Assert::IsTrue(shortC.scratchShort, L"ScratchShort() set");
		const std::string prefix = "scratch memory short (grain split)";
		Assert::IsTrue(shortC.error.compare(0, prefix.size(), prefix) == 0, Wide("error text: " + shortC.error).c_str());
		const FallbackCatalog::Decision dec = arena.FrameDecision();
		Assert::IsTrue(dec.engaged && dec.id == "scratch_arena", L"scratch_arena fallback engaged");
		Logger::WriteMessage(("shortage: " + shortC.error + " | " + dec.reason).c_str());
		const Product shortF = Run(dev, tex, B, Storage::Full, &arena);
		Assert::IsFalse(shortF.ok, L"Full with an empty plan fails too");
		Assert::IsTrue(shortF.scratchShort, L"ScratchShort() set (Full)");
		// ScratchShort resets on the next Compute: a private run clears it
		const Product again = Run(dev, tex, B, Storage::Compact, nullptr);
		Assert::IsTrue(again.ok && !again.scratchShort, L"ScratchShort reset by a Compute without a shortage");

		// recovery: a larger plan, a new frame
		Assert::IsTrue(arena.Reserve(VpSkeleton::PlanArena(GrainSplit::Gpu::Splitter::ArenaRequests(gpu, w, h, B, Storage::Compact), VpSkeleton::Tier::T2)), L"larger plan reserves");
		arena.BeginFrame();
		const Product ok = Run(dev, tex, B, Storage::Compact, &arena);
		Assert::IsTrue(ok.ok && !ok.scratchShort, Wide("recovery: " + ok.error).c_str());
		Assert::IsFalse(arena.FrameDecision().engaged, L"the fallback releases itself on the first frame that was not short");
		Assert::IsTrue(SameBits(reference.a, ok.a) && SameBits(reference.b, ok.b) && SameBits(reference.blocks, ok.blocks), L"recovered result equals the private result exactly");
		// Full after the Compact plan (same bands/quiet shapes are pooled): still equal to the private Full
		Assert::IsTrue(arena.Reserve(VpSkeleton::PlanArena(GrainSplit::Gpu::Splitter::ArenaRequests(gpu, w, h, B, Storage::Full), VpSkeleton::Tier::T2)), L"Full plan reserves");
		const Product okFull = Run(dev, tex, B, Storage::Full, &arena);
		Assert::IsTrue(okFull.ok && SameBits(referenceFull.a, okFull.a) && SameBits(referenceFull.blocks, okFull.blocks), L"Full recovery equals the private Full result");
		pl_tex_destroy(gpu, &tex);
	}

	TEST_METHOD(GrainSplitCompactWritesIntoTargets)
	{
		using namespace GrainSplitCompactGpuTestsNs;
		GpuDevice dev;
		Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
		pl_gpu gpu = dev.Gpu();
		const int w = 256, h = 144, B = BlockEdge(h);
		const int bw = (w + B - 1) / B, bh = (h + B - 1) / B;
		const Plane y = MakePlane(w, h, 3u);
		pl_tex tex = UploadLuma(gpu, y);
		VpSkeleton::PlTexBackend backend(gpu);
		const Product reference = Run(dev, tex, B, Storage::Compact, nullptr);
		const Product referenceFull = Run(dev, tex, B, Storage::Full, nullptr);
		Assert::IsTrue(reference.ok && referenceFull.ok, L"private runs");
		for (int mode = 0; mode < 2; ++mode)
		{
			const bool compact = mode == 0;
			VpSkeleton::TexShape a, b, blk;
			a.w = static_cast<uint32_t>(w); a.h = static_cast<uint32_t>(h); a.fmt = compact ? VpSkeleton::TexFormat::RG16F : VpSkeleton::TexFormat::RGBA32F;
			b = a; b.fmt = VpSkeleton::TexFormat::RG16;
			blk.w = static_cast<uint32_t>(bw); blk.h = static_cast<uint32_t>(bh); blk.fmt = VpSkeleton::TexFormat::RGBA32F;
			GrainSplit::Gpu::Targets t;
			t.split = backend.Create(a);
			t.splitB = compact ? backend.Create(b) : nullptr;
			t.blocks = backend.Create(blk);
			Assert::IsTrue(t.split && t.blocks && (!compact || t.splitB), L"target textures created");
			GrainSplit::Gpu::Splitter splitter;
			GrainSplit::Gpu::Options o;
			o.storage = compact ? Storage::Compact : Storage::Full;
			o.targets = t;
			Assert::IsTrue(splitter.Compute(gpu, dev.dispatch, tex, 0, kCodeUnit, 1.0, B, GrainSplit::Params(), o), Wide(splitter.LastError()).c_str());
			const GrainSplit::Gpu::Frame& f = splitter.Out();
			Assert::IsTrue(f.split == t.split && f.blocks == t.blocks && f.splitB == t.splitB, L"the frame points at the caller's targets");
			Assert::IsTrue(GrainSplit::Gpu::Fits(&f, w, h, B, 1.0, kCodeUnit, 0), L"Fits");
			const Product& ref = compact ? reference : referenceFull;
			const bool same = SameBits(ref.a, DownloadRGBA(gpu, dev.dispatch, t.split)) && SameBits(ref.blocks, DownloadRGBA(gpu, dev.dispatch, t.blocks)) &&
				(!compact || SameBits(ref.b, DownloadRGBA(gpu, dev.dispatch, t.splitB)));
			Assert::IsTrue(same, L"results in the targets equal the private result");
			// a Compact frame without B does not fit; a Full frame does not need B
			GrainSplit::Gpu::Frame noB = f;
			noB.splitB = nullptr;
			Assert::AreEqual(!compact, GrainSplit::Gpu::Fits(&noB, w, h, B, 1.0, kCodeUnit, 0), L"Fits needs splitB for Compact only");
			splitter.Release(gpu);   // never destroys the caller's targets
			Assert::IsTrue(t.split->params.w == w, L"targets survive Release");
			backend.Destroy(t.split);
			if (t.splitB) backend.Destroy(t.splitB);
			backend.Destroy(t.blocks);
		}
		pl_tex_destroy(gpu, &tex);
	}

	TEST_METHOD(GrainSplitViewGlslAndBind)
	{
		using namespace GrainSplitCompactGpuTestsNs;
		const std::string full = GrainSplit::Gpu::ViewGlsl(Storage::Full, kCodeUnit);
		const std::string comp = GrainSplit::Gpu::ViewGlsl(Storage::Compact, kCodeUnit);
		Assert::IsTrue(full.find("texelFetch(split") != std::string::npos && full.find("gblocks") != std::string::npos, L"Full accessor reads split and gblocks");
		Assert::IsTrue(full.find("gs_fetch") != std::string::npos && full.find("gs_blocks") != std::string::npos, L"Full accessor defines gs_fetch and gs_blocks");
		Assert::IsTrue(comp.find("gs_a") != std::string::npos && comp.find("gs_b") != std::string::npos && comp.find("gblocks") != std::string::npos, L"Compact accessor reads gs_a, gs_b, gblocks");
		Assert::IsTrue(comp.find(VpvGpu::Num(kCodeUnit)) != std::string::npos, L"Compact accessor carries the code unit literal");

		GpuDevice dev;
		Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
		pl_gpu gpu = dev.Gpu();
		const int sizes[2][2] = { { 256, 144 }, { 320, 180 } };
		double worstS = 0, worstSrel = 0, worstBase = 0, worstF = 0;
		for (int si = 0; si < 2; ++si)
		{
			const int w = sizes[si][0], h = sizes[si][1], B = BlockEdge(h);
			const Plane y = MakePlane(w, h, 41u + static_cast<uint32_t>(si));
			pl_tex tex = UploadLuma(gpu, y);
			std::vector<float> fetch[2], blocks[2];
			for (int mode = 0; mode < 2; ++mode)
			{
				const Storage st = mode == 0 ? Storage::Full : Storage::Compact;
				GrainSplit::Gpu::Splitter splitter;
				GrainSplit::Gpu::Options o;
				o.storage = st;
				Assert::IsTrue(splitter.Compute(gpu, dev.dispatch, tex, 0, kCodeUnit, 1.0, B, GrainSplit::Params(), o), Wide(splitter.LastError()).c_str());
				const GrainSplit::Gpu::Frame& f = splitter.Out();
				std::vector<pl_shader_desc> desc;
				GrainSplit::Gpu::BindView(f, desc);
				Assert::AreEqual(static_cast<size_t>(mode == 0 ? 2 : 3), desc.size(), L"descriptor count");
				const char* const want[2][3] = { { "split", "gblocks", "" }, { "gs_a", "gs_b", "gblocks" } };
				for (size_t i = 0; i < desc.size(); ++i)
				{
					Assert::IsTrue(std::strcmp(desc[i].desc.name, want[mode][i]) == 0, Wide(std::string("descriptor name ") + want[mode][i]).c_str());
					Assert::IsTrue(desc[i].desc.type == PL_DESC_SAMPLED_TEX, L"sampled");
				}
				pl_tex outFetch = nullptr, outBlocks = nullptr;
				const pl_fmt f4 = VpvGpu::FloatFormat(gpu, 4);
				Assert::IsTrue(VpvGpu::Ensure(gpu, outFetch, f.w, f.h, f4) && VpvGpu::Ensure(gpu, outBlocks, f.bw, f.bh, f4), L"output textures");
				desc.push_back(VpvGpu::Storage("out_fetch", outFetch));
				desc.push_back(VpvGpu::Storage("out_blocks", outBlocks));
				const std::string hdr = GrainSplit::Gpu::ViewGlsl(st, kCodeUnit) + VpvGpu::DefineInt("TW", f.w) + VpvGpu::DefineInt("TH", f.h) +
					VpvGpu::DefineInt("TBW", f.bw) + VpvGpu::DefineInt("TBH", f.bh);
				const bool ran = VpvGpu::Pass(dev.dispatch, "grain split accessor test", hdr, R"GLSL(
ivec2 p = ivec2(gl_GlobalInvocationID.xy);
if (p.x < TW && p.y < TH) imageStore(out_fetch, p, gs_fetch(p));
if (p.x < TBW && p.y < TBH) imageStore(out_blocks, p, vec4(gs_blocks(p), 0.0, 0.0));
)GLSL", desc.data(), static_cast<int>(desc.size()), nullptr, 0, (f.w + 15) / 16, (f.h + 15) / 16, 0);
				Assert::IsTrue(ran, L"accessor shader compiled and ran");
				fetch[mode] = DownloadRGBA(gpu, dev.dispatch, outFetch);
				blocks[mode] = DownloadRGBA(gpu, dev.dispatch, outBlocks);
				pl_tex_destroy(gpu, &outFetch);
				pl_tex_destroy(gpu, &outBlocks);
				// the accessor in the Full layout is the raw texel (exact)
				if (mode == 0)
				{
					const std::vector<float> raw = DownloadRGBA(gpu, dev.dispatch, f.split), rawBlocks = DownloadRGBA(gpu, dev.dispatch, f.blocks);
					Assert::IsTrue(SameBits(raw, fetch[0]), L"Full gs_fetch returns the stored texel exactly");
					bool same = raw.size() == fetch[0].size();
					for (size_t i = 0; same && i < static_cast<size_t>(f.bw) * f.bh; ++i)
						same = rawBlocks[i * 4] == blocks[0][i * 4] && rawBlocks[i * 4 + 1] == blocks[0][i * 4 + 1];
					Assert::IsTrue(same, L"Full gs_blocks returns (N, e3) exactly");
				}
				splitter.Release(gpu);
			}
			Assert::IsTrue(fetch[0].size() == fetch[1].size() && !fetch[0].empty(), L"both layouts read back");
			for (size_t i = 0; i < fetch[0].size() / 4; ++i)
			{
				for (int c = 0; c < 2; ++c)
				{
					const double a = fetch[0][i * 4 + c], b = fetch[1][i * 4 + c], d = std::fabs(b - a);
					if (std::fabs(a) <= 64.0) worstS = (std::max)(worstS, d);
					else worstSrel = (std::max)(worstSrel, d / std::fabs(a));
				}
				worstF = (std::max)(worstF, std::fabs(static_cast<double>(fetch[1][i * 4 + 2]) - fetch[0][i * 4 + 2]));
				worstBase = (std::max)(worstBase, std::fabs(static_cast<double>(fetch[1][i * 4 + 3]) - fetch[0][i * 4 + 3]));
			}
			bool blocksSame = true;
			for (size_t i = 0; i < blocks[0].size() / 4; ++i)
				blocksSame = blocksSame && blocks[0][i * 4] == blocks[1][i * 4] && blocks[0][i * 4 + 1] == blocks[1][i * 4 + 1];
			Assert::IsTrue(blocksSame, L"gs_blocks equal in both layouts");
			pl_tex_destroy(gpu, &tex);
		}
		LogF("accessor, Compact vs Full: |s| (<= 64 codes) %.5f codes, |s| relative (> 64 codes) %.2e, |base| %.5f codes, |f| %.2e", worstS, worstSrel, worstBase, worstF);
		// fp16 keeps 11 significant bits: half an ulp is 2^-11 relative (0.03 codes at 64); base is quantised to 1/65535 of the
		// plane value (about 0.0078 codes of half a step at this code unit); f to 1/65535 unorm (we allow 1/255).
		Assert::IsTrue(worstS <= 0.05, L"|ds| <= 0.05 codes for |s| <= 64");
		Assert::IsTrue(worstSrel <= 1.0 / 1024.0, L"relative error of large s within the fp16 step");
		Assert::IsTrue(worstBase <= 0.05, L"base within 0.05 codes");
		Assert::IsTrue(worstF <= 1.0 / 255.0, L"f within 1/255");
	}

	TEST_METHOD(GrainSplitCompactBlockEnergiesMatchTheModel)
	{
		// The Compact blocks carry the block mean band energies e1 = mean d1^2 and e2 = mean d2^2 (modifier fields): they match
		// GrainSplit::Compute's bands, are deterministic, and Full leaves blocks.zw at 0 (gs_energies unavailable there).
		using namespace GrainSplitCompactGpuTestsNs;
		GpuDevice dev;
		Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
		pl_gpu gpu = dev.Gpu();
		const int sizes[2][2] = { { 256, 144 }, { 320, 180 } };
		for (int si = 0; si < 2; ++si)
		{
			const int w = sizes[si][0], h = sizes[si][1], B = BlockEdge(h);
			const Plane y = MakePlane(w, h, 41u + static_cast<uint32_t>(si));
			pl_tex tex = UploadLuma(gpu, y);
			const Product comp = Run(dev, tex, B, Storage::Compact, nullptr);
			const Product again = Run(dev, tex, B, Storage::Compact, nullptr);
			const Product full = Run(dev, tex, B, Storage::Full, nullptr);
			Assert::IsTrue(comp.ok && again.ok && full.ok, L"computes");
			Assert::IsTrue(SameBits(comp.blocks, again.blocks), L"deterministic: the block texels are bit-identical across runs");
			const GrainSplit::Split model = GrainSplit::Compute(y, 1.0, B);
			double worstRel1 = 0, worstRel2 = 0;
			size_t zwZero = 0, blocksTotal = static_cast<size_t>(comp.bw) * comp.bh;
			for (int by = 0; by < comp.bh; ++by)
				for (int bx = 0; bx < comp.bw; ++bx)
				{
					double s1 = 0, s2 = 0, cnt = 0;
					for (int yy = by * B; yy < (std::min)(h, (by + 1) * B); ++yy)
						for (int xx = bx * B; xx < (std::min)(w, (bx + 1) * B); ++xx)
						{
							s1 += model.d1.at(xx, yy) * model.d1.at(xx, yy);
							s2 += model.d2.at(xx, yy) * model.d2.at(xx, yy);
							cnt += 1;
						}
					const size_t i = static_cast<size_t>(by) * comp.bw + bx;
					const double e1 = comp.blocks[i * 4 + 2], e2 = comp.blocks[i * 4 + 3];
					worstRel1 = (std::max)(worstRel1, std::fabs(e1 - s1 / cnt) / (std::fabs(s1 / cnt) + 1e-3));
					worstRel2 = (std::max)(worstRel2, std::fabs(e2 - s2 / cnt) / (std::fabs(s2 / cnt) + 1e-3));
					zwZero += (full.blocks[i * 4 + 2] == 0.0f && full.blocks[i * 4 + 3] == 0.0f) ? 1u : 0u;
				}
			LogF("block energies %.0fx%.0f: worst relative error e1 %.6f, e2 %.6f against the CPU model", w, h, worstRel1, worstRel2);
			Assert::IsTrue(worstRel1 <= 2e-3 && worstRel2 <= 2e-3, L"e1 and e2 match the model's block means of d1^2 and d2^2");
			Assert::AreEqual(blocksTotal, zwZero, L"Full leaves blocks.zw at 0");
			pl_tex_destroy(gpu, &tex);
		}
		const std::string full = GrainSplit::Gpu::ViewGlsl(Storage::Full, kCodeUnit), compact = GrainSplit::Gpu::ViewGlsl(Storage::Compact, kCodeUnit);
		Assert::IsTrue(full.find("GS_HAS_ENERGIES 0") != std::string::npos && full.find("vec2(-1.0)") != std::string::npos, L"Full: energies unavailable");
		Assert::IsTrue(compact.find("GS_HAS_ENERGIES 1") != std::string::npos && compact.find("gs_tilt") != std::string::npos && compact.find("gs_energies") != std::string::npos,
			L"Compact: energies and tilt accessors");
	}
};
