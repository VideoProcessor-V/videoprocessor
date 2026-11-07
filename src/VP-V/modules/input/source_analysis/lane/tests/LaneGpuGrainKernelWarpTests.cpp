// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// 70b G2: the P10 grain-split kernel (GrainSplitKernel) on the libplacebo WARP device (TargetLutGpuFixture; WARP unless
// VP_TEST_GPU=hardware): the five kernel steps equal Splitter::Compute byte for byte (Full and Compact), the lane form
// through the SkeletonGpuDispatcher equals the live form, the produced Frame passes / fails CheckFit as it must, and the
// Full pass text is pinned by its FNV-1a fingerprint.
#include "pch.h"
#include <VP-V/tests/VpLutGpuTestSupport.h>
#include <VP-V/core/ProductGraph.h>
#include <VP-V/modules/control/skeleton/gpu/SkeletonGpuDispatcher.h>
#include <VP-V/modules/input/source_analysis/lane/LaneGpuGrainKernel.h>

#include <libplacebo/dispatch.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace VideoProcessorTest
{
	namespace
	{
		using namespace VpSkeleton;
		using Vpv::Skeleton::SkeletonGpuDispatcher;
		using GrainSplit::Storage;

		pl_tex MakeTex(pl_gpu gpu, TexFormat f, int w, int h, bool hostWritable)
		{
			pl_tex_params tp{};
			tp.w = w;
			tp.h = h;
			tp.format = PlTexBackend::FindFormat(gpu, f);
			tp.sampleable = true;
			tp.storable = true;
			tp.host_writable = hostWritable;
			tp.host_readable = true;
			Assert::IsNotNull(tp.format, L"no texture format");
			pl_tex t = pl_tex_create(gpu, &tp);
			Assert::IsNotNull(t, L"cannot create the texture");
			return t;
		}

		std::vector<uint8_t> Bytes(pl_gpu gpu, pl_tex tex)
		{
			std::vector<uint8_t> v(size_t(tex->params.w) * tex->params.h * tex->params.format->texel_size);
			pl_tex_transfer_params d{};
			d.tex = tex;
			d.ptr = v.data();
			Assert::IsTrue(pl_tex_download(gpu, &d), L"download failed");
			return v;
		}

		std::vector<uint16_t> Codes(int w, int h, uint32_t seed)
		{
			std::vector<uint16_t> v(size_t(w) * h);
			for (size_t i = 0; i < v.size(); ++i)
			{
				seed = seed * 1664525u + 1013904223u;
				const int x = int(i % size_t(w));
				v[i] = uint16_t(200 + (x * 600) / w + int((seed >> 8) % 41u) - 20);   // a ramp with grain-like noise, inside 64..940
			}
			return v;
		}

		SourceFrameView ViewOf(const std::vector<uint16_t>& codes, int w, int h, uint64_t seq)
		{
			SourceFrameView v;
			v.seq = seq;
			v.luma = codes.data();
			v.width = uint32_t(w);
			v.height = uint32_t(h);
			v.strideSamples = uint32_t(w);
			v.msbShift = 6;
			return v;
		}

		struct Setup
		{
			Registry reg = VpLaneProducts::TapFormProducts(SkeletonProducts());
			Graph graph;
			TierPlan plan;
			Dims dims;
			explicit Setup(Dims d) : dims(d)
			{
				ConsumerDecl c;
				c.id = 1;
				AddConsume(c, kP10GrainSplit);
				GraphSettings gs;
				gs.dims = d;
				gs.queueDepth = 4;
				gs.laneLead = 1;
				graph = BuildGraph(reg, std::vector<ConsumerDecl>{ c }, gs);
				TierEntry e;
				e.id = kP10GrainSplit;
				e.backend = Backend::GpuCompute;
				e.grant = Grant::Lane;
				plan.entries.push_back(e);
			}
			const Node& P10() const
			{
				for (const Node& n : graph.nodes)
					if (n.id == kP10GrainSplit)
						return n;
				Assert::Fail(L"P10 not in the graph");
				return graph.nodes[0];
			}
		};

		// Output textures in the kernel's layout term order, host readable.
		std::vector<pl_tex> MakeOutputs(pl_gpu gpu, const ProductTexLayout& layout, Dims dims)
		{
			std::vector<pl_tex> v;
			for (const TexTerm& t : layout.terms)
			{
				const uint32_t w = t.absW ? t.absW : (dims.width + (1u << t.shift) - 1u) >> t.shift;
				const uint32_t h = t.absH ? t.absH : (dims.height + (1u << t.shift) - 1u) >> t.shift;
				v.push_back(MakeTex(gpu, t.fmt, int(w), int(h), false));
			}
			return v;
		}
	}

	TEST_CLASS(LaneGpuGrainKernelWarpTests)
	{
	public:
		// The kernel's five steps record Splitter::Compute's dispatches: every output term is byte-identical, in the Full
		// layout and in the Compact one.
		BEGIN_TEST_METHOD_ATTRIBUTE(KernelStepLoopEqualsSplitterCompute)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(KernelStepLoopEqualsSplitterCompute)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			pl_dispatch dispatch = pl_dispatch_create(gpu->log, gpu);
			const int w = 96, h = 64;
			const Dims dims{ uint32_t(w), uint32_t(h) };
			const std::vector<uint16_t> codes = Codes(w, h, 31u);
			pl_tex plane = MakeTex(gpu, TexFormat::R16, w, h, true);
			std::vector<uint16_t> scratch;
			Assert::IsTrue(VpLaneProducts::UploadTapRows(gpu, plane, ViewOf(codes, w, h, 1), 0, uint32_t(h), scratch), L"upload");

			for (Storage storage : { Storage::Full, Storage::Compact })
			{
				VpLaneProducts::GrainSplitKernel kernel(storage);
				Assert::IsTrue(kernel.Steps(dims) == 5 && kernel.NeedsTap() && kernel.Product() == kP10GrainSplit, L"kernel shape");
				Assert::IsTrue(kernel.Granted() == storage, L"R16 unorm at codeUnit 64/65535 grants either storage");
				const ProductTexLayout layout = kernel.Layout(dims);
				std::vector<pl_tex> viaKernel = MakeOutputs(gpu, layout, dims);
				std::vector<pl_tex> viaCompute = MakeOutputs(gpu, layout, dims);

				VpLaneProducts::KernelContext c;
				c.gpu = gpu;
				c.dispatch = dispatch;
				c.id = kP10GrainSplit;
				c.seq = 5;
				c.dims = dims;
				c.extended = plane;
				c.tap = plane;
				for (size_t i = 0; i < viaKernel.size(); ++i)
					c.out[i] = viaKernel[i];
				c.outCount = viaKernel.size();
				for (uint32_t s = 0; s < kernel.Steps(dims); ++s)
					Assert::IsTrue(kernel.Step(c, s), L"kernel step");

				const int B = RegionEvidence::BlockSizeFor(h);
				GrainSplit::Gpu::Options opt;
				opt.storage = storage;
				opt.seq = 5;
				opt.targets.split = viaCompute[0];
				if (storage == Storage::Compact)
				{
					opt.targets.splitB = viaCompute[1];
					opt.targets.blocks = viaCompute[2];
				}
				else
					opt.targets.blocks = viaCompute[1];
				GrainSplit::Gpu::Splitter splitter;
				Assert::IsTrue(splitter.Compute(gpu, dispatch, plane, 0, VpLaneProducts::GrainSplitKernel::CodeUnit(), double(B) / 16.0, B,
					GrainSplit::Params(), opt), L"Splitter::Compute");

				for (size_t t = 0; t < viaKernel.size(); ++t)
				{
					const std::vector<uint8_t> a = Bytes(gpu, viaKernel[t]);
					const std::vector<uint8_t> b = Bytes(gpu, viaCompute[t]);
					Assert::IsTrue(a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size()) == 0,
						L"step loop == Compute, byte for byte, every term");
				}
				const GrainSplit::Gpu::Frame& kf = kernel.LastFrame(false);
				const GrainSplit::Gpu::Frame& cf = splitter.Out();
				Assert::IsTrue(kf.w == cf.w && kf.h == cf.h && kf.B == cf.B && kf.bw == cf.bw && kf.bh == cf.bh && kf.scale == cf.scale &&
					kf.codeUnit == cf.codeUnit && kf.storage == cf.storage && kf.seq == cf.seq && kf.version == cf.version, L"same Frame stamps");
				for (pl_tex& t : viaKernel) pl_tex_destroy(gpu, &t);
				for (pl_tex& t : viaCompute) pl_tex_destroy(gpu, &t);
				kernel.Release(gpu);
				splitter.Release(gpu);
			}
			pl_tex_destroy(gpu, &plane);
			pl_dispatch_destroy(&dispatch);
		}

		// The lane form (tap bands, extend, five kernel steps, one Dispatch each) equals the live form (DispatchLive), every
		// term byte for byte; the Frame the kernel produced fits its plane and is refused when it does not.
		BEGIN_TEST_METHOD_ATTRIBUTE(LaneFormEqualsLiveFormAndFrameFits)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(LaneFormEqualsLiveFormAndFrameFits)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			SkeletonGpuDispatcher disp(gpu, true);
			VpLaneProducts::GrainSplitKernel* kernel = new VpLaneProducts::GrainSplitKernel();
			disp.AddKernel(std::unique_ptr<VpLaneProducts::IProductKernel>(kernel));
			const Dims d{ 160, 96 };
			Setup s(d);
			Assert::IsTrue(disp.Configure(s.graph, s.reg, s.plan, d), L"configure");
			const std::vector<uint16_t> codes = Codes(int(d.width), int(d.height), 77u);
			const SourceFrameView view = ViewOf(codes, int(d.width), int(d.height), 7);
			const Node& n = s.P10();

			LaneJobContext job;
			job.node = &n;
			job.seq = 7;
			job.ringSlot = 0;
			job.source = &view;
			const uint32_t steps = VpLaneProducts::StepsFor(kP10GrainSplit, d);
			Assert::IsTrue(steps == VpLaneProducts::TapBands(d) + 1u + 5u, L"bands + extend + 5");
			for (uint32_t i = 0; i < steps; ++i)
				Assert::IsTrue(disp.Dispatch(job, i, steps), L"lane chunk");
			disp.SetLiveSourceForTesting([&codes, d](uint64_t seq, SourceFrameView* out) {
				*out = ViewOf(codes, int(d.width), int(d.height), seq);
				return true;
			});
			Assert::IsTrue(disp.DispatchLive(n, 8, 0, Header(), 0u), L"live fill");

			const VpLaneProducts::GpuProductTextures* tex = disp.TexturesOf(kP10GrainSplit);
			const ProductTexLayout layout = kernel->Layout(d);
			for (size_t t = 0; t < layout.terms.size(); ++t)
			{
				const std::vector<uint8_t> lane = Bytes(gpu, tex->Ring(0, t));
				const std::vector<uint8_t> live = Bytes(gpu, tex->Live(0, t));
				Assert::IsTrue(!lane.empty() && lane.size() == live.size() && std::memcmp(lane.data(), live.data(), lane.size()) == 0,
					L"lane == live, bit for bit, every term");
			}

			const int w = int(d.width), h = int(d.height), B = RegionEvidence::BlockSizeFor(h);
			const double scale = double(B) / 16.0, cu = VpLaneProducts::GrainSplitKernel::CodeUnit();
			const GrainSplit::Gpu::Frame& lf = kernel->LastFrame(false);
			const GrainSplit::Gpu::Frame& vf = kernel->LastFrame(true);
			Assert::IsTrue(lf.seq == 7 && vf.seq == 8, L"each form stamps its own frame");
			Assert::IsTrue(lf.split == tex->Ring(0, 0) && vf.split == tex->Live(0, 0), L"the Frame points at the slot textures");
			using GrainSplit::Gpu::FitResult;
			using GrainSplit::Gpu::CheckFit;
			Assert::IsTrue(CheckFit(&lf, w, h, B, scale, cu, 0) == FitResult::Fits, L"lane Frame fits");
			Assert::IsTrue(CheckFit(&lf, w, h, B, scale, cu, 0, 7) == FitResult::Fits, L"lane Frame fits its frame");
			Assert::IsTrue(CheckFit(&vf, w, h, B, scale, cu, 0, 8) == FitResult::Fits, L"live Frame fits");
			Assert::IsTrue(CheckFit(&lf, w + 1, h, B, scale, cu, 0) == FitResult::WrongSize, L"another width: WrongSize");
			Assert::IsTrue(CheckFit(&lf, w, h, B, scale, cu, 1) == FitResult::WrongSize, L"another component: WrongSize");
			Assert::IsTrue(CheckFit(&lf, w, h, B, scale, 1.0 / 1023.0, 0) == FitResult::WrongSize, L"another code unit: WrongSize");
			Assert::IsTrue(CheckFit(&lf, w, h, B, scale, cu, 0, 99) == FitResult::Stale, L"another frame: Stale");
			const GrainSplit::Gpu::Frame none;
			Assert::IsTrue(CheckFit(&none, w, h, B, scale, cu, 0) == FitResult::Missing, L"no product: Missing");
			Assert::IsTrue(kernel->Fallbacks().size() == 1, L"the storage decision is listed");
		}

		// The Full pass text is the pre-SA2 text: its FNV-1a 64 is pinned (the value already pinned by GrainUnitGpuTests; the
		// kernel adds no GLSL of its own).
		BEGIN_TEST_METHOD_ATTRIBUTE(PassTextFingerprintIsPinned)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(PassTextFingerprintIsPinned)
		{
			constexpr uint64_t kPinned = 0x41cef8707edf809dull;
			Assert::IsTrue(GrainSplit::Gpu::FullShaderFingerprint() == kPinned, L"Full pass GLSL fingerprint");
		}
	};

	// 70b: the private working memory the governor does not see (CPU only; no device). Compact keeps rows + bands (two
	// full-size rgba32f planes), Full only the bands; the lane form doubles it (a second Splitter), the live set is always counted.
	TEST_CLASS(LaneGpuGrainPrivateBytesTests)
	{
	public:
		TEST_METHOD(WorkingSetsAreCountedPerSplitter)
		{
			const VpSkeleton::Dims d{ 3840, 2160 };
			const uint64_t px = uint64_t(d.width) * d.height;
			const uint64_t B = uint64_t(RegionEvidence::BlockSizeFor(int(d.height)));
			const uint64_t blocks = ((d.width + B - 1) / B) * ((d.height + B - 1) / B);
			VpLaneProducts::GrainSplitKernel full(GrainSplit::Storage::Full);
			VpLaneProducts::GrainSplitKernel compact(GrainSplit::Storage::Compact);
			Assert::IsTrue(full.PrivateBytes(d, false) == px * 16u + blocks * 16u, L"Full live set: bands + block grid");
			Assert::IsTrue(full.PrivateBytes(d, true) == 2u * full.PrivateBytes(d, false), L"lane form = a second Splitter");
			if (compact.Granted() == GrainSplit::Storage::Compact)
				Assert::IsTrue(compact.PrivateBytes(d, false) == 2u * px * 16u + blocks * 16u, L"Compact live set: rows + bands + grid");
		}
	};
}
