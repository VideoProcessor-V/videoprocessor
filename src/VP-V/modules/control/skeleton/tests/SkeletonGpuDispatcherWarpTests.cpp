// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// 70b G1: the skeleton's render-thread GPU dispatcher on the libplacebo WARP device (TargetLutGpuFixture; WARP unless
// VP_TEST_GPU=hardware): the tap plane carries all 1024 10-bit codes exactly, the extend pass equals the CPU
// RegionEvidence::ExtendPicture, the product textures equal the declaration, releases are deferred and counted, and the
// lane form (tap bands, extend, kernel steps) equals the live form texel for texel.
#include "pch.h"
#include <VP-V/tests/VpLutGpuTestSupport.h>
#include <VP-V/core/ProductGraph.h>
#include <VP-V/modules/control/skeleton/gpu/SkeletonGpuDispatcher.h>
#include <VP-V/modules/input/source_analysis/lane/LaneGpuProducts.h>

#include <libplacebo/dispatch.h>
#include <libplacebo/shaders/custom.h>

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

		pl_tex ReadableR16(pl_gpu gpu, int w, int h)
		{
			pl_tex_params tp{};
			tp.w = w;
			tp.h = h;
			tp.format = PlTexBackend::FindFormat(gpu, TexFormat::R16);
			tp.sampleable = true;
			tp.storable = true;
			tp.host_writable = true;
			tp.host_readable = true;
			Assert::IsNotNull(tp.format, L"no r16 format");
			pl_tex t = pl_tex_create(gpu, &tp);
			Assert::IsNotNull(t, L"cannot create the r16 texture");
			return t;
		}

		template <class T>
		std::vector<T> Download(pl_gpu gpu, pl_tex tex, int comps)
		{
			std::vector<T> v(size_t(tex->params.w) * tex->params.h * comps);
			pl_tex_transfer_params d{};
			d.tex = tex;
			d.ptr = v.data();
			Assert::IsTrue(pl_tex_download(gpu, &d), L"download failed");
			return v;
		}

		// One compute pass: out(p) = header body over `src` (sampled) into `dst` (storage).
		bool Pass(pl_dispatch dispatch, const char* body, pl_tex src, pl_tex dst, int w, int h)
		{
			pl_shader_desc desc[2]{};
			desc[0].desc.name = "t_src";
			desc[0].desc.type = PL_DESC_SAMPLED_TEX;
			desc[0].binding.object = src;
			desc[0].binding.sample_mode = PL_TEX_SAMPLE_NEAREST;
			desc[1].desc.name = "t_dst";
			desc[1].desc.type = PL_DESC_STORAGE_IMG;
			desc[1].desc.access = PL_DESC_ACCESS_WRITEONLY;
			desc[1].binding.object = dst;
			pl_custom_shader c{};
			c.description = "skeleton dispatcher test";
			c.body = body;
			c.input = PL_SHADER_SIG_NONE;
			c.output = PL_SHADER_SIG_NONE;
			c.descriptors = desc;
			c.num_descriptors = 2;
			c.compute = true;
			c.compute_group_size[0] = 8;
			c.compute_group_size[1] = 8;
			pl_shader sh = pl_dispatch_begin(dispatch);
			if (!pl_shader_custom(sh, &c))
			{
				pl_dispatch_abort(dispatch, &sh);
				return false;
			}
			pl_dispatch_compute_params p{};
			p.shader = &sh;
			p.dispatch_size[0] = (w + 7) / 8;
			p.dispatch_size[1] = (h + 7) / 8;
			p.dispatch_size[2] = 1;
			return pl_dispatch_compute(dispatch, &p);
		}

		// A P10 stand-in with the real Full layout (split rgba32f per pixel + blocks rgba32f per 16 px cell) and the
		// splitter's five steps: step 0 copies the extended plane's codes into the split texture, the others record nothing.
		class CopyKernel : public VpLaneProducts::IProductKernel
		{
		public:
			ProductId Product() const override { return kP10GrainSplit; }
			ProductTexLayout Layout(Dims) const override
			{
				ProductTexLayout l;
				TexTerm a;
				a.shift = 0;
				a.fmt = TexFormat::RGBA32F;
				TexTerm b;
				b.shift = 4;
				b.fmt = TexFormat::RGBA32F;
				l.terms.push_back(a);
				l.terms.push_back(b);
				return l;
			}
			uint32_t Steps(Dims) const override { return kP10SplitterStepsLocal; }
			bool NeedsTap() const override { return true; }
			bool Step(VpLaneProducts::KernelContext& c, uint32_t step) override
			{
				++steps;
				if (step != 0)
					return true;
				if (!c.extended || c.outCount < 1)
					return false;
				return Pass(c.dispatch,
					"ivec2 p = ivec2(gl_GlobalInvocationID.xy);\n"
					"if (p.x >= textureSize(t_src, 0).x || p.y >= textureSize(t_src, 0).y) return;\n"
					"float v = floor(texelFetch(t_src, p, 0).x * 65535.0 + 0.5) / 64.0;\n"
					"imageStore(t_dst, p, vec4(v, 0.0, 0.0, 1.0));\n",
					c.extended, c.out[0], int(c.dims.width), int(c.dims.height));
			}
			void Release(pl_gpu) override {}
			static constexpr uint32_t kP10SplitterStepsLocal = VpLaneProducts::kP10SplitterSteps;
			int steps = 0;
		};

		std::vector<uint16_t> Codes(int w, int h, uint32_t seed)
		{
			std::vector<uint16_t> v(size_t(w) * h);
			for (size_t i = 0; i < v.size(); ++i)
			{
				seed = seed * 1664525u + 1013904223u;
				v[i] = uint16_t(64 + (seed >> 8) % 877u);   // studio codes 64..940
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
	}

	TEST_CLASS(SkeletonGpuDispatcherWarpTests)
	{
	public:
		// All 1024 10-bit codes survive the tap upload (code << 6 in R16 unorm) and the reader's law
		// floor(texel * 65535 + 0.5) / 64 gives each code back exactly.
		BEGIN_TEST_METHOD_ATTRIBUTE(TapCarriesEvery10BitCodeExactly)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(TapCarriesEvery10BitCodeExactly)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			const int w = 256, h = 4;
			std::vector<uint16_t> codes(size_t(w) * h);
			for (size_t i = 0; i < codes.size(); ++i)
				codes[i] = uint16_t(i);   // 0..1023
			pl_tex tap = ReadableR16(gpu, w, h);
			std::vector<uint16_t> scratch;
			Assert::IsTrue(VpLaneProducts::UploadTapRows(gpu, tap, ViewOf(codes, w, h, 1), 0, 2, scratch), L"band 0");
			Assert::IsTrue(VpLaneProducts::UploadTapRows(gpu, tap, ViewOf(codes, w, h, 1), 2, 4, scratch), L"band 1");
			const std::vector<uint16_t> raw = Download<uint16_t>(gpu, tap, 1);
			for (size_t i = 0; i < raw.size(); ++i)
				Assert::IsTrue(raw[i] == uint16_t(codes[i] << 6), L"tap = code << 6");

			pl_tex out = nullptr;
			{
				pl_tex_params tp{};
				tp.w = w;
				tp.h = h;
				tp.format = PlTexBackend::FindFormat(gpu, TexFormat::RGBA32F);
				tp.storable = true;
				tp.sampleable = true;
				tp.host_readable = true;
				out = pl_tex_create(gpu, &tp);
			}
			pl_dispatch dispatch = pl_dispatch_create(gpu->log, gpu);
			Assert::IsTrue(Pass(dispatch,
				"ivec2 p = ivec2(gl_GlobalInvocationID.xy);\n"
				"if (p.x >= 256 || p.y >= 4) return;\n"
				"imageStore(t_dst, p, vec4(floor(texelFetch(t_src, p, 0).x * 65535.0 + 0.5) / 64.0, 0.0, 0.0, 1.0));\n",
				tap, out, w, h), L"reader pass");
			const std::vector<float> back = Download<float>(gpu, out, 4);
			for (size_t i = 0; i < codes.size(); ++i)
				Assert::IsTrue(back[i * 4] == float(codes[i]), L"the code comes back exactly");
			pl_dispatch_destroy(&dispatch);
			pl_tex_destroy(gpu, &out);
			pl_tex_destroy(gpu, &tap);
		}

		// The extend pass equals RegionEvidence::ExtendPicture (mirror about the picture edge) texel for texel.
		BEGIN_TEST_METHOD_ATTRIBUTE(ExtendMatchesTheCpuReference)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(ExtendMatchesTheCpuReference)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			const int w = 96, h = 64;
			const std::vector<uint16_t> codes = Codes(w, h, 4242u);
			pl_tex tap = ReadableR16(gpu, w, h);
			pl_tex ext = ReadableR16(gpu, w, h);
			std::vector<uint16_t> scratch;
			Assert::IsTrue(VpLaneProducts::UploadTapRows(gpu, tap, ViewOf(codes, w, h, 1), 0, uint32_t(h), scratch), L"upload");
			pl_dispatch dispatch = pl_dispatch_create(gpu->log, gpu);
			const RegionEvidence::Rect rects[3] = { { 8, 6, 80, 50 }, { 0, 0, w, h }, { 0, 20, w, 30 } };
			for (const RegionEvidence::Rect& r : rects)
			{
				Assert::IsTrue(VpLaneProducts::ExtendTap(dispatch, tap, ext, r), L"extend pass");
				const std::vector<uint16_t> got = Download<uint16_t>(gpu, ext, 1);
				GrainSplit::Plane y(w, h);
				for (int yy = 0; yy < h; ++yy)
					for (int x = 0; x < w; ++x)
						y.at(x, yy) = double(codes[size_t(yy) * w + x]);
				const GrainSplit::Plane ref = RegionEvidence::ExtendPicture(y, r);
				for (int yy = 0; yy < h; ++yy)
					for (int x = 0; x < w; ++x)
						Assert::IsTrue(double(got[size_t(yy) * w + x] >> 6) == ref.at(x, yy) && (got[size_t(yy) * w + x] & 63) == 0,
							L"extended = CPU ExtendPicture");
			}
			pl_dispatch_destroy(&dispatch);
			pl_tex_destroy(gpu, &ext);
			pl_tex_destroy(gpu, &tap);
		}

		// The product textures are exactly the declaration (ring + live); a size change defers the old set's release to
		// FlushReleases; Release frees everything; every release is counted.
		BEGIN_TEST_METHOD_ATTRIBUTE(AllocationEqualsDeclarationAndReleasesAreDeferred)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(AllocationEqualsDeclarationAndReleasesAreDeferred)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			SkeletonGpuDispatcher disp(fixture.Gpu(), true);
			Assert::IsTrue(disp.Valid(), L"own dispatch on the renderer's gpu");
			disp.AddKernel(std::unique_ptr<VpLaneProducts::IProductKernel>(new CopyKernel()));
			Setup s(Dims{ 128, 64 });
			Assert::IsTrue(disp.Configure(s.graph, s.reg, s.plan, s.dims), L"configure");
			const Node& n = s.P10();
			const uint64_t declared = uint64_t(n.ring + n.livePool) * BytesOf(s.reg.Get(kP10GrainSplit).size, s.dims);
			Assert::IsTrue(disp.AllocatedBytes() == declared && declared > 0, L"allocated = declared (ring + live)");
			Assert::IsTrue(disp.TexturesOf(kP10GrainSplit)->RingBuffers() == n.ring, L"ring buffers");
			Assert::IsTrue(disp.Stats().allocations == 1, L"one allocation");
			Assert::IsTrue(disp.Configure(s.graph, s.reg, s.plan, s.dims) && disp.Stats().allocations == 1, L"unchanged: no work");

			Setup s2(Dims{ 64, 32 });
			Assert::IsTrue(disp.Configure(s2.graph, s2.reg, s2.plan, s2.dims), L"size change");
			Assert::IsTrue(disp.PendingReleases() == 1 && disp.Stats().releasesDone == 0, L"old set waits for a GPU-legal phase");
			disp.FlushReleases();
			Assert::IsTrue(disp.PendingReleases() == 0 && disp.Stats().releasesDone == 1, L"released at the flush");
			disp.Release();
			Assert::IsTrue(disp.Stats().releasesDone == 2 && disp.AllocatedBytes() == 0, L"everything released");
		}

		// Lane (tap bands, extend, five kernel steps, one Dispatch each) == live (DispatchLive), texel for texel.
		BEGIN_TEST_METHOD_ATTRIBUTE(LaneEqualsLive)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(LaneEqualsLive)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			SkeletonGpuDispatcher disp(gpu, true);
			CopyKernel* kernel = new CopyKernel();
			disp.AddKernel(std::unique_ptr<VpLaneProducts::IProductKernel>(kernel));
			const Dims d{ 1024, 2100 };   // two tap bands (4 MB each)
			Setup s(d);
			Assert::IsTrue(disp.Configure(s.graph, s.reg, s.plan, d), L"configure");
			const std::vector<uint16_t> codes = Codes(int(d.width), int(d.height), 99u);
			const SourceFrameView view = ViewOf(codes, int(d.width), int(d.height), 7);
			const Node& n = s.P10();

			LaneJobContext job;
			job.node = &n;
			job.seq = 7;
			job.ringSlot = 0;
			job.source = &view;
			const uint32_t steps = VpLaneProducts::StepsFor(kP10GrainSplit, d);
			Assert::IsTrue(steps == VpLaneProducts::TapBands(d) + 1u + 5u && VpLaneProducts::TapBands(d) == 2u, L"2 bands + extend + 5");
			for (uint32_t i = 0; i < steps; ++i)
				Assert::IsTrue(disp.Dispatch(job, i, steps), L"lane chunk");
			Assert::IsTrue(kernel->steps == 5 && disp.Stats().tapBands == 2 && disp.Stats().extends == 1, L"lane passes");
			Assert::IsFalse(disp.Dispatch(job, 0, steps + 1), L"a plan that does not match the passes is refused");

			// The live form of another frame (8) with the same content: its own tap upload (all bands) and extend.
			Assert::IsFalse(disp.DispatchLive(n, 8, 0, Header(), 0u), L"no source view: refused visibly");
			Assert::IsTrue(disp.Stats().failures >= 2 && !disp.LastError().empty(), L"failures counted with a reason");
			disp.SetLiveSourceForTesting([&codes, d](uint64_t seq, SourceFrameView* out) {
				*out = ViewOf(codes, int(d.width), int(d.height), seq);
				return true;
			});
			Assert::IsTrue(disp.DispatchLive(n, 8, 0, Header(), 0u), L"live fill");
			Assert::IsTrue(kernel->steps == 10 && disp.Stats().tapBands == 4 && disp.Stats().extends == 2, L"same passes, live");

			const GpuProductTextures* tex = disp.TexturesOf(kP10GrainSplit);
			const std::vector<float> lane = Download<float>(gpu, tex->Ring(0, 0), 4);
			const std::vector<float> live = Download<float>(gpu, tex->Live(0, 0), 4);
			Assert::IsTrue(lane.size() == live.size() && std::memcmp(lane.data(), live.data(), lane.size() * sizeof(float)) == 0,
				L"lane == live, bit for bit");
			for (size_t i = 0; i < codes.size(); ++i)
				Assert::IsTrue(lane[i * 4] == float(codes[i]), L"lane output = the codes (full picture: extend is identity)");
		}
	};
}
