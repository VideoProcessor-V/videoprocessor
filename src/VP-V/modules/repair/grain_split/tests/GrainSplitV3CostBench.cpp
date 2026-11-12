// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// repair.grain_split v3 cost bench (P10 v3, refs/116 s7): the lane kernel's six passes (HALF+across, DOWN, STATS, LEVEL, NMIN, APPLY; unit 4, WAIVER-P10V4-01: the pass count pin 5 -> 6 is an intended change)
// timed per pass with one pl_timer per pass on the hardware GPU, on R16 planes of 1080p and 2160p, Compact and Full, with the
// lane kernel law (B = RegionEvidence::BlockSizeFor(h), scale B / 16): 20 warm-up and 120 timed frames. Also the dispatcher's
// live form (tap upload + extend + six passes + finish) wall time. LOGS ONLY: no timing assertion (a measurement, not a gate).
// Category GpuHardware: without VP_TEST_GPU=hardware it returns at once with a log line.
#include "pch.h"
#include "CppUnitTest.h"
#include <VP-V/tests/support/ExtendedRangeGpuProbe.h>
#include <VP-V/tests/VpLutGpuTestSupport.h>

#include <VP-V/core/ProductGraph.h>
#include <VP-V/modules/control/skeleton/gpu/SkeletonGpuDispatcher.h>
#include <VP-V/modules/input/source_analysis/lane/LaneGpuGrainKernel.h>

#include <libplacebo/dispatch.h>
#include <libplacebo/gpu.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace GrainSplitV3CostBenchNs
{
	using namespace VpSkeleton;
	using Vpv::Skeleton::SkeletonGpuDispatcher;
	using GrainSplit::Storage;

	constexpr int kWarm = 20;
	constexpr int kTimed = 120;
	constexpr int kPasses = 6;   // WAIVER-P10V4-01: unit 4 adds the LEVEL pass (was 5)

	void LogLine(const std::string& s) { Logger::WriteMessage(s.c_str()); }

	std::string Fmt(const char* fmt, double a = 0, double b = 0, double c = 0, double d = 0, double e = 0, double f = 0)
	{
		char line[512];
		std::snprintf(line, sizeof line, fmt, a, b, c, d, e, f);
		return std::string(line);
	}

	pl_tex MakeTex(pl_gpu gpu, TexFormat f, int w, int h, bool hostWritable)
	{
		pl_tex_params tp{};
		tp.w = w; tp.h = h;
		tp.format = PlTexBackend::FindFormat(gpu, f);
		tp.sampleable = true; tp.storable = true; tp.host_writable = hostWritable; tp.host_readable = false;
		Assert::IsNotNull(tp.format, L"no texture format");
		pl_tex t = pl_tex_create(gpu, &tp);
		Assert::IsNotNull(t, L"cannot create the texture");
		return t;
	}

	// 10-bit codes in the high bits of a 16-bit sample (msbShift 6): a ramp with noise.
	std::vector<uint16_t> MakeCodes(int w, int h, uint32_t seed)
	{
		std::vector<uint16_t> v(static_cast<size_t>(w) * h);
		for (size_t i = 0; i < v.size(); ++i)
		{
			seed = seed * 1664525u + 1013904223u;
			const int x = static_cast<int>(i % static_cast<size_t>(w));
			v[i] = static_cast<uint16_t>(200 + (x * 600) / w + static_cast<int>((seed >> 8) % 41u) - 20);
		}
		return v;
	}

	SourceFrameView View(const std::vector<uint16_t>& codes, int w, int h, uint64_t seq)
	{
		SourceFrameView v;
		v.seq = seq; v.luma = codes.data(); v.width = static_cast<uint32_t>(w); v.height = static_cast<uint32_t>(h);
		v.strideSamples = static_cast<uint32_t>(w); v.msbShift = 6;
		return v;
	}

	double Ms(std::chrono::steady_clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }

	struct Stat { double mean = 0, p95 = 0, peak = 0; };
	Stat StatOf(std::vector<double> v)
	{
		Stat s;
		if (v.empty()) return s;
		std::sort(v.begin(), v.end());
		double sum = 0;
		for (double x : v) sum += x;
		s.mean = sum / static_cast<double>(v.size());
		s.peak = v.back();
		s.p95 = v[(std::min)(v.size() - 1, static_cast<size_t>(static_cast<double>(v.size()) * 0.95))];
		return s;
	}
}

TEST_CLASS(GrainSplitV3CostBench)
{
public:
	BEGIN_TEST_METHOD_ATTRIBUTE(V3PassCosts)
		TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
	END_TEST_METHOD_ATTRIBUTE()
	TEST_METHOD(V3PassCosts)
	{
		using namespace GrainSplitV3CostBenchNs;
		if (!ExtendedRangeGpuProbe::OnHardwareGpu())
		{
			Logger::WriteMessage("GrainSplitV3CostBench: not on a hardware GPU (VP_TEST_GPU=hardware): the cost bench returns without a measurement");
			return;
		}
		TargetLutGpuFixture fixture;
		Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
		pl_gpu gpu = fixture.Gpu();
		pl_dispatch dispatch = pl_dispatch_create(gpu->log, gpu);
		Assert::IsNotNull(dispatch, L"dispatch");

		const int sizes[2][2] = { { 1920, 1080 }, { 3840, 2160 } };
		for (const auto& sz : sizes)
		{
			const int w = sz[0], h = sz[1];
			const Dims dims{ static_cast<uint32_t>(w), static_cast<uint32_t>(h) };
			const int B = RegionEvidence::BlockSizeFor(h);
			const std::vector<uint16_t> codes = MakeCodes(w, h, 7u);
			pl_tex plane = MakeTex(gpu, TexFormat::R16, w, h, true);
			std::vector<uint16_t> scratch;
			Assert::IsTrue(VpLaneProducts::UploadTapRows(gpu, plane, View(codes, w, h, 1), 0, static_cast<uint32_t>(h), scratch), L"tap upload");

			for (Storage storage : { Storage::Compact, Storage::Full })
			{
				VpLaneProducts::GrainSplitKernel kernel(storage);
				Assert::AreEqual(static_cast<uint32_t>(kPasses), static_cast<uint32_t>(kernel.Steps(dims)), L"six steps");
				const ProductTexLayout layout = kernel.Layout(dims);
				std::vector<pl_tex> outs;
				for (const TexTerm& t : layout.terms)
				{
					const uint32_t tw = t.absW ? t.absW : (dims.width + (1u << t.shift) - 1u) >> t.shift;
					const uint32_t th = t.absH ? t.absH : (dims.height + (1u << t.shift) - 1u) >> t.shift;
					outs.push_back(MakeTex(gpu, t.fmt, static_cast<int>(tw), static_cast<int>(th), false));
				}
				pl_timer timers[kPasses] = {};
				for (int s = 0; s < kPasses; ++s) timers[s] = pl_timer_create(gpu);

				VpLaneProducts::KernelContext c;
				c.gpu = gpu; c.dispatch = dispatch; c.id = kP10GrainSplit; c.dims = dims; c.extended = plane; c.tap = plane;
				for (size_t i = 0; i < outs.size(); ++i) c.out[i] = outs[i];
				c.outCount = outs.size();

				std::vector<double> perPass[kPasses], total, wall;
				for (int f = -kWarm; f < kTimed; ++f)
				{
					c.seq = static_cast<uint64_t>(f + kWarm + 10);
					const auto t0 = std::chrono::steady_clock::now();
					for (uint32_t s = 0; s < static_cast<uint32_t>(kPasses); ++s)
					{
						c.timer = timers[s];
						Assert::IsTrue(kernel.Step(c, s), L"kernel step");
					}
					pl_gpu_finish(gpu);
					const double wms = Ms(std::chrono::steady_clock::now() - t0);
					double sumMs = 0;
					double passMs[kPasses] = {};
					for (int s = 0; s < kPasses; ++s)
					{
						uint64_t ns, sum = 0;
						while ((ns = pl_timer_query(gpu, timers[s])) != 0) sum += ns;
						passMs[s] = static_cast<double>(sum) / 1.0e6;
						sumMs += passMs[s];
					}
					if (f >= 0)
					{
						for (int s = 0; s < kPasses; ++s) perPass[s].push_back(passMs[s]);
						total.push_back(sumMs);
						wall.push_back(wms);
					}
				}
				const char* name = storage == Storage::Full ? "Full" : "Compact";
				static const char* kPassName[kPasses] = { "HALF+across", "DOWN", "STATS", "LEVEL", "NMIN", "APPLY" };
				for (int s = 0; s < kPasses; ++s)
				{
					const Stat st = StatOf(perPass[s]);
					LogLine(Fmt("V3 cost %.0fx%.0f B %.0f ", w, h, B) + name + " pass " + std::to_string(s) + " " + kPassName[s] +
						Fmt(": gpu ms mean %.4f p95 %.4f max %.4f", st.mean, st.p95, st.peak));
				}
				const Stat tt = StatOf(total), ww = StatOf(wall);
				LogLine(Fmt("V3 cost %.0fx%.0f B %.0f ", w, h, B) + name + Fmt(" TOTAL (%.0f frames): gpu ms mean %.4f p95 %.4f max %.4f", kTimed, tt.mean, tt.p95, tt.peak) +
					Fmt("; wall ms (6 passes + finish) mean %.4f p95 %.4f max %.4f", ww.mean, ww.p95, ww.peak));

				for (int s = 0; s < kPasses; ++s) pl_timer_destroy(gpu, &timers[s]);
				for (pl_tex& t : outs) pl_tex_destroy(gpu, &t);
				kernel.Release(gpu);
			}

			// The dispatcher's live form: tap upload + extend + six passes + finish, wall time per frame (Compact: the default).
			{
				SkeletonGpuDispatcher disp(gpu, false);
				disp.AddKernel(std::unique_ptr<VpLaneProducts::IProductKernel>(new VpLaneProducts::GrainSplitKernel(Storage::Compact)));
				Registry reg = VpLaneProducts::TapFormProducts(SkeletonProducts());
				ConsumerDecl cd;
				cd.id = 1;
				AddConsume(cd, kP10GrainSplit);
				GraphSettings gs;
				gs.dims = dims; gs.queueDepth = 4; gs.laneLead = 1;
				Graph graph = BuildGraph(reg, std::vector<ConsumerDecl>{ cd }, gs);
				TierPlan plan;
				TierEntry e;
				e.id = kP10GrainSplit; e.backend = Backend::GpuCompute; e.grant = Grant::Lane;
				plan.entries.push_back(e);
				const bool configured = disp.Configure(graph, reg, plan, dims);
				Assert::IsTrue(configured, L"dispatcher configure (live form)");
				if (configured)
				{
					const Node* node = nullptr;
					for (const Node& n : graph.nodes)
						if (n.id == kP10GrainSplit) node = &n;
					Assert::IsNotNull(node, L"graph node");
					disp.SetLiveSourceForTesting([&codes, w, h](uint64_t seq, SourceFrameView* o) { *o = View(codes, w, h, seq); return true; });
					std::vector<double> wall;
					for (int f = -kWarm; f < kTimed; ++f)
					{
						const auto t0 = std::chrono::steady_clock::now();
						const bool ok = disp.DispatchLive(*node, static_cast<uint64_t>(f + kWarm + 20), 0, Header(), 0u);
						pl_gpu_finish(gpu);
						const double wms = Ms(std::chrono::steady_clock::now() - t0);
						Assert::IsTrue(ok, L"DispatchLive");
						if (f >= 0) wall.push_back(wms);
					}
					const Stat st = StatOf(wall);
					LogLine(Fmt("V3 cost %.0fx%.0f B %.0f ", w, h, B) + "Compact live form (tap upload + extend + 6 passes + finish): wall ms" +
						Fmt(" mean %.4f p95 %.4f max %.4f; dispatcher MeasuredMs (last pass) %.4f", st.mean, st.p95, st.peak, disp.MeasuredMs(kP10GrainSplit)));
				}
				disp.Release();
			}
			pl_tex_destroy(gpu, &plane);
		}
		pl_dispatch_destroy(&dispatch);
	}
};
