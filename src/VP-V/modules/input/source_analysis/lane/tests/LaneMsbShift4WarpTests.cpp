// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Slice 21 W21-1 (W7): the lane GPU kernels run on a 12-bit tap view. A 10-bit tap holds code10 << 6 (msbShift 6); a DV 12-bit tap holds
// code12 << 4 (msbShift 4); MsbAlign puts both on the same 16-bit Q6 lane scale. So the same picture given as code10 at msbShift 6 and
// as code10 * 4 at msbShift 4 (a 12-bit code with no sub-code part) must upload the identical tap plane and give identical kernel
// outputs, in the lane form and in the live form. libplacebo WARP device (TargetLutGpuFixture; WARP unless VP_TEST_GPU=hardware).
// New class in a new file (rule 28: the existing kernel tests are not edited). Style: LaneGpuGrainKernelWarpTests.
#include "pch.h"
#include <VP-V/tests/VpLutGpuTestSupport.h>
#include <VP-V/core/ProductGraph.h>
#include <VP-V/modules/control/skeleton/gpu/SkeletonGpuDispatcher.h>
#include <VP-V/modules/input/source_analysis/lane/LaneGpuGrainKernel.h>

#include <libplacebo/dispatch.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace VideoProcessorTest
{
	namespace
	{
		using namespace VpSkeleton;
		using Vpv::Skeleton::SkeletonGpuDispatcher;

		pl_tex MakeTapTex(pl_gpu gpu, int w, int h)
		{
			pl_tex_params tp{};
			tp.w = w;
			tp.h = h;
			tp.format = PlTexBackend::FindFormat(gpu, TexFormat::R16);
			tp.sampleable = true;
			tp.storable = true;
			tp.host_writable = true;
			tp.host_readable = true;
			Assert::IsNotNull(tp.format, L"no R16 format");
			pl_tex t = pl_tex_create(gpu, &tp);
			Assert::IsNotNull(t, L"cannot create the tap texture");
			return t;
		}

		std::vector<uint8_t> TexBytes(pl_gpu gpu, pl_tex tex)
		{
			std::vector<uint8_t> v(size_t(tex->params.w) * tex->params.h * tex->params.format->texel_size);
			pl_tex_transfer_params d{};
			d.tex = tex;
			d.ptr = v.data();
			Assert::IsTrue(pl_tex_download(gpu, &d), L"download failed");
			return v;
		}

		// 10-bit studio codes: a ramp with grain-like noise inside 64..940.
		std::vector<uint16_t> Codes10(int w, int h, uint32_t seed)
		{
			std::vector<uint16_t> v(size_t(w) * h);
			for (size_t i = 0; i < v.size(); ++i)
			{
				seed = seed * 1664525u + 1013904223u;
				const int x = int(i % size_t(w));
				v[i] = uint16_t(200 + (x * 600) / w + int((seed >> 8) % 41u) - 20);
			}
			return v;
		}

		SourceFrameView ViewOf(const std::vector<uint16_t>& raw, int w, int h, uint64_t seq, uint8_t msbShift)
		{
			SourceFrameView v;
			v.seq = seq;
			v.luma = raw.data();
			v.width = uint32_t(w);
			v.height = uint32_t(h);
			v.strideSamples = uint32_t(w);
			v.msbShift = msbShift;
			return v;
		}

		// The P10 grain-split kernel through the lane dispatcher, fed `raw` at `msbShift`; every ring term and every live term, in
		// bytes.
		struct KernelRun
		{
			std::vector<std::vector<uint8_t>> lane, live;
		};

		KernelRun RunGrainKernel(pl_gpu gpu, Dims d, const std::vector<uint16_t>& raw, uint8_t msbShift)
		{
			Registry reg = VpLaneProducts::TapFormProducts(SkeletonProducts());
			ConsumerDecl c;
			c.id = 1;
			AddConsume(c, kP10GrainSplit);
			GraphSettings gs;
			gs.dims = d;
			gs.queueDepth = 4;
			gs.laneLead = 1;
			Graph graph = BuildGraph(reg, std::vector<ConsumerDecl>{ c }, gs);
			TierPlan plan;
			TierEntry e;
			e.id = kP10GrainSplit;
			e.backend = Backend::GpuCompute;
			e.grant = Grant::Lane;
			plan.entries.push_back(e);

			SkeletonGpuDispatcher disp(gpu, true);
			VpLaneProducts::GrainSplitKernel* kernel = new VpLaneProducts::GrainSplitKernel();
			disp.AddKernel(std::unique_ptr<VpLaneProducts::IProductKernel>(kernel));
			const bool cfgOk = disp.Configure(graph, reg, plan, d);
			const std::string why = disp.LastError();
			Assert::IsTrue(cfgOk, (L"configure: " + std::wstring(why.begin(), why.end())).c_str());
			const Node* node = nullptr;
			for (const Node& n : graph.nodes)
				if (n.id == kP10GrainSplit)
					node = &n;
			Assert::IsNotNull(node, L"P10 in the graph");

			const SourceFrameView view = ViewOf(raw, int(d.width), int(d.height), 7, msbShift);
			LaneJobContext job;
			job.node = node;
			job.seq = 7;
			job.ringSlot = 0;
			job.source = &view;
			const uint32_t steps = VpLaneProducts::StepsFor(kP10GrainSplit, d);
			for (uint32_t i = 0; i < steps; ++i)
				Assert::IsTrue(disp.Dispatch(job, i, steps), L"lane chunk");
			disp.SetLiveSourceForTesting([&raw, d, msbShift](uint64_t seq, SourceFrameView* out) {
				*out = ViewOf(raw, int(d.width), int(d.height), seq, msbShift);
				return true;
			});
			Assert::IsTrue(disp.DispatchLive(*node, 8, 0, Header(), 0u), L"live fill");

			KernelRun r;
			const VpLaneProducts::GpuProductTextures* tex = disp.TexturesOf(kP10GrainSplit);
			Assert::IsNotNull(tex, L"textures");
			const ProductTexLayout layout = kernel->Layout(d);
			for (size_t t = 0; t < layout.terms.size(); ++t)
			{
				r.lane.push_back(TexBytes(gpu, tex->Ring(0, t)));
				r.live.push_back(TexBytes(gpu, tex->Live(0, t)));
			}
			return r;
		}
	}

	TEST_CLASS(LaneKernelsRunOnMsbShift4View)
	{
	public:
		// The tap upload is the Q6 axis for either depth: the same picture as 10-bit codes at msbShift 6 and as code * 4 at msbShift 4
		// gives byte-identical R16 tap planes, and each plane holds exactly code10 << 6 (an independent oracle, not the MsbAlign path).
		BEGIN_TEST_METHOD_ATTRIBUTE(TapUploadIsTheSameQ6PlaneAtBothDepths)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(TapUploadIsTheSameQ6PlaneAtBothDepths)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			const int w = 96, h = 64;
			const std::vector<uint16_t> code10 = Codes10(w, h, 31u);
			std::vector<uint16_t> code12(code10.size());
			for (size_t i = 0; i < code10.size(); ++i) code12[i] = uint16_t(code10[i] * 4u);
			pl_tex tap10 = MakeTapTex(gpu, w, h);
			pl_tex tap12 = MakeTapTex(gpu, w, h);
			std::vector<uint16_t> scratch;
			Assert::IsTrue(VpLaneProducts::UploadTapRows(gpu, tap10, ViewOf(code10, w, h, 1, 6), 0, uint32_t(h), scratch), L"10-bit upload");
			Assert::IsTrue(VpLaneProducts::UploadTapRows(gpu, tap12, ViewOf(code12, w, h, 1, 4), 0, uint32_t(h), scratch), L"12-bit upload");
			const std::vector<uint8_t> a = TexBytes(gpu, tap10);
			const std::vector<uint8_t> b = TexBytes(gpu, tap12);
			pl_tex_destroy(gpu, &tap10);
			pl_tex_destroy(gpu, &tap12);
			Assert::IsTrue(a.size() == b.size() && a.size() == size_t(w) * h * 2, L"two bytes a texel");
			Assert::IsTrue(std::memcmp(a.data(), b.data(), a.size()) == 0, L"the same tap plane at both depths");
			int wrong = 0;
			for (size_t i = 0; i < code10.size(); ++i)
			{
				uint16_t texel = 0;
				std::memcpy(&texel, &a[i * 2], 2);
				if (texel != uint16_t(code10[i] << 6)) ++wrong;
			}
			Assert::AreEqual(0, wrong, L"every texel is code10 << 6");
			Assert::IsTrue(code12[0] != code10[0] && code10[0] != 0, L"the two views really differ in their raw samples");
		}

		// The P10 grain-split kernel (the lane form and the live form) on a msbShift 4 view of code10 * 4 gives every term byte for byte
		// what it gives on the msbShift 6 view of code10: nothing in the kernel depends on the tap's depth, only on the Q6 axis. The
		// terms are not empty and at least one varies (the comparison is not between blank planes).
		BEGIN_TEST_METHOD_ATTRIBUTE(LaneKernelsRunOnMsbShift4View)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(LaneKernelsRunOnMsbShift4View)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			const Dims d{ 320, 768 };
			const std::vector<uint16_t> code10 = Codes10(int(d.width), int(d.height), 77u);
			std::vector<uint16_t> code12(code10.size());
			for (size_t i = 0; i < code10.size(); ++i) code12[i] = uint16_t(code10[i] * 4u);
			const KernelRun ten = RunGrainKernel(gpu, d, code10, 6);
			const KernelRun twelve = RunGrainKernel(gpu, d, code12, 4);
			Assert::IsTrue(!ten.lane.empty() && ten.lane.size() == twelve.lane.size() && ten.live.size() == twelve.live.size(), L"the same terms");
			bool anyVaried = false;
			for (size_t t = 0; t < ten.lane.size(); ++t)
			{
				Assert::IsTrue(!ten.lane[t].empty() && ten.lane[t].size() == twelve.lane[t].size() &&
					std::memcmp(ten.lane[t].data(), twelve.lane[t].data(), ten.lane[t].size()) == 0, L"lane form: msbShift 4 == msbShift 6, every term");
				Assert::IsTrue(!ten.live[t].empty() && ten.live[t].size() == twelve.live[t].size() &&
					std::memcmp(ten.live[t].data(), twelve.live[t].data(), ten.live[t].size()) == 0, L"live form: msbShift 4 == msbShift 6, every term");
				for (size_t i = 1; i < ten.lane[t].size() && !anyVaried; ++i)
					anyVaried = ten.lane[t][i] != ten.lane[t][0];
			}
			Assert::IsTrue(anyVaried, L"the kernel produced varying output (not a comparison of blank planes)");
		}
	};
}
