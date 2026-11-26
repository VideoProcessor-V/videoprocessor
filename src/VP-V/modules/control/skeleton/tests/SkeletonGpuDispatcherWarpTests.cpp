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
#include <VP-V/modules/control/skeleton/gpu/GpuProductCost.h>
#include <VP-V/modules/control/skeleton/gpu/SkeletonGpuDispatcher.h>
#include <VP-V/modules/input/source_analysis/lane/LaneGpuProducts.h>

#include <libplacebo/dispatch.h>
#include <libplacebo/shaders/custom.h>

#include <algorithm>
#include <cmath>
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
		// splitter's seven steps (unit 5): step 0 copies the extended plane's codes into the split texture, the others record nothing.
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
			{ const bool cfgOk = disp.Configure(s.graph, s.reg, s.plan, s.dims); std::string why = disp.LastError(); for (const FallbackCatalog::Decision& fd : disp.Fallbacks()) if (fd.engaged) why += " | " + fd.id + ": " + fd.reason; Assert::IsTrue(cfgOk, (L"configure: " + std::wstring(why.begin(), why.end())).c_str()); }
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

		// Lane (tap bands, extend, seven kernel steps, one Dispatch each) == live (DispatchLive), texel for texel.
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
			{ const bool cfgOk = disp.Configure(s.graph, s.reg, s.plan, d); std::string why = disp.LastError(); for (const FallbackCatalog::Decision& fd : disp.Fallbacks()) if (fd.engaged) why += " | " + fd.id + ": " + fd.reason; Assert::IsTrue(cfgOk, (L"configure: " + std::wstring(why.begin(), why.end())).c_str()); }
			const std::vector<uint16_t> codes = Codes(int(d.width), int(d.height), 99u);
			const SourceFrameView view = ViewOf(codes, int(d.width), int(d.height), 7);
			const Node& n = s.P10();

			LaneJobContext job;
			job.node = &n;
			job.seq = 7;
			job.ringSlot = 0;
			job.source = &view;
			const uint32_t steps = VpLaneProducts::StepsFor(kP10GrainSplit, d);
			Assert::IsTrue(steps == VpLaneProducts::TapBands(d) + 1u + 7u && VpLaneProducts::TapBands(d) == 2u, L"2 bands + extend + 7 (WAIVER-P10V5-01: unit 5 REDUCE)");
			for (uint32_t i = 0; i < steps; ++i)
				Assert::IsTrue(disp.Dispatch(job, i, steps), L"lane chunk");
			Assert::IsTrue(kernel->steps == 7 && disp.Stats().tapBands == 2 && disp.Stats().extends == 1, L"lane passes");
			Assert::IsTrue(disp.CostOf(kP10GrainSplit).frames == 1, L"the cost counter records one completed lane frame (last chunk only)");
			// S0 / FIX-S0-COST: the dispatcher's planning cost is the per-frame pass SUM (CostOf mean), never the last single pass.
			Assert::IsTrue(disp.MeasuredMs(kP10GrainSplit) == disp.CostOf(kP10GrainSplit).MeanFrameMs(), L"MeasuredMs is the per-frame mean of all passes");
			Assert::IsTrue(disp.MeasuredMs(kP10GrainSplit) + 1e-9 >= disp.LastPassMs(kP10GrainSplit), L"the frame sum is never below one pass");
			Assert::IsFalse(disp.Dispatch(job, 0, steps + 1), L"a plan that does not match the passes is refused");

			// The live form of another frame (8) with the same content: its own tap upload (all bands) and extend.
			Assert::IsFalse(disp.DispatchLive(n, 8, 0, Header(), 0u), L"no source view: refused visibly");
			Assert::IsTrue(disp.Stats().failures >= 2 && !disp.LastError().empty(), L"failures counted with a reason");
			disp.SetLiveSourceForTesting([&codes, d](uint64_t seq, SourceFrameView* out) {
				*out = ViewOf(codes, int(d.width), int(d.height), seq);
				return true;
			});
			Assert::IsTrue(disp.DispatchLive(n, 8, 0, Header(), 0u), L"live fill");
			Assert::IsTrue(kernel->steps == 14 && disp.Stats().tapBands == 4 && disp.Stats().extends == 2, L"same passes, live");
			Assert::IsTrue(disp.CostOf(kP10GrainSplit).frames == 2, L"the cost counter records the live frame too");
			Assert::IsTrue(disp.CostOf(kP11RegionBlockStats).frames == 0, L"a product without a kernel has an empty cost");

			const VpLaneProducts::GpuProductTextures* tex = disp.TexturesOf(kP10GrainSplit);
			const std::vector<float> lane = Download<float>(gpu, tex->Ring(0, 0), 4);
			const std::vector<float> live = Download<float>(gpu, tex->Live(0, 0), 4);
			Assert::IsTrue(lane.size() == live.size() && std::memcmp(lane.data(), live.data(), lane.size() * sizeof(float)) == 0,
				L"lane == live, bit for bit");
			for (size_t i = 0; i < codes.size(); ++i)
				Assert::IsTrue(lane[i * 4] == float(codes[i]), L"lane output = the codes (full picture: extend is identity)");
		}

		// CPU only: the cost record adds every pass of a frame (P10 = five passes), keeps the worst pass and the worst frame, and the
		// status text is empty until a frame has been timed.
		TEST_METHOD(ProductCostAddsEveryPassOfAFrame)
		{
			Vpv::Skeleton::GpuProductCost cost;
			Assert::IsTrue(Vpv::Skeleton::FormatCostLine("P10 grain split", cost).empty(), L"no line before the first timed frame");
			for (int f = 0; f < 4; ++f)
			{
				cost.NoteFrameDone();
				// five passes of 1.0, 0.5, 0.5, 0.5 and 0.5 ms (one of them 2.0 ms in frame 2)
				cost.NoteResult(f == 2 ? 2000000u : 1000000u);
				for (int p = 0; p < 4; ++p)
					cost.NoteResult(500000u);
				cost.NotePoll();
			}
			Assert::IsTrue(cost.frames == 4 && cost.timedFrames == 4 && cost.results == 20, L"counts written");
			Assert::IsTrue(cost.totalNs == 3u * 3000000u + 4000000u, L"sums every pass, not the last one");
			Assert::IsTrue(cost.maxPassNs == 2000000u, L"worst single pass");
			Assert::IsTrue(std::fabs(cost.MeanFrameMs() - 13.0 / 4.0) < 1.0e-9, L"mean ms per frame over all passes");
			Assert::IsTrue(std::fabs(cost.MaxPassMs() - 2.0) < 1.0e-9, L"worst pass ms");
			const std::string line = Vpv::Skeleton::FormatCostLine("P10 grain split", cost);
			Assert::IsTrue(line.find("P10 grain split") != std::string::npos && line.find("3.25 ms/frame") != std::string::npos, L"status text");
		}

		// K#12 (SK-VRAM PERF-6, GEM-179): when oldJobsJoined == true, Configure destroys old product textures BEFORE
		// allocating replacements so peak VRAM stays <= max(old, new) with zero graveyard; negative control
		// (oldJobsJoined == false) queues the old set in the graveyard and peaks at old + new.
		BEGIN_TEST_METHOD_ATTRIBUTE(ConfigureWithOldJobsJoinedReclaimsBeforeAllocatingKeepingPeakAtMax)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(ConfigureWithOldJobsJoinedReclaimsBeforeAllocatingKeepingPeakAtMax)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			SkeletonGpuDispatcher disp(fixture.Gpu(), true);
			Assert::IsTrue(disp.Valid(), L"own dispatch on the renderer's gpu");
			disp.AddKernel(std::unique_ptr<VpLaneProducts::IProductKernel>(new CopyKernel()));

			Setup s1(Dims{ 64, 48 });
			{
				const bool cfgOk = disp.Configure(s1.graph, s1.reg, s1.plan, s1.dims, 0u, false);
				std::string why = disp.LastError();
				for (const FallbackCatalog::Decision& fd : disp.Fallbacks())
					if (fd.engaged)
						why += " | " + fd.id + ": " + fd.reason;
				Assert::IsTrue(cfgOk, (L"initial configure 64x48: " + std::wstring(why.begin(), why.end())).c_str());
			}
			const uint64_t oldBytes = disp.AllocatedBytes();
			Assert::IsTrue(oldBytes > 0u, L"initial 64x48 allocation is non-zero");
			Assert::AreEqual(uint64_t(0), disp.GraveyardBytes(), L"no graveyard after initial configure");
			Assert::IsTrue(disp.HoldsVideoMemory(), L"HoldsVideoMemory true while product textures allocated");

			// Joined reconfigure to larger 96x64: old textures destroyed BEFORE allocating new -> peak <= max(old, new).
			disp.ResetPeakBytesForTesting();
			Setup s2(Dims{ 96, 64 });
			Assert::IsTrue(disp.Configure(s2.graph, s2.reg, s2.plan, s2.dims, 0u, true),
				L"K#12: reconfigure 64x48 -> 96x64 with oldJobsJoined=true succeeds");
			const uint64_t newBytes = disp.AllocatedBytes();
			Assert::IsTrue(newBytes > oldBytes, L"96x64 allocation is strictly larger than 64x48");
			Assert::AreEqual(size_t(0), disp.PendingReleases(),
				L"K#12: oldJobsJoined=true leaves zero pending graveyard releases");
			Assert::AreEqual(uint64_t(0), disp.GraveyardBytes(),
				L"K#12: oldJobsJoined=true holds zero graveyard bytes");
			Assert::IsTrue(disp.Stats().releasesDone >= 1u,
				L"K#12: old texture set destroyed immediately before allocating replacements");
			Assert::IsTrue(disp.PeakAllocatedBytes() <= (std::max)(oldBytes, newBytes),
				L"K#12: joined reconfigure peak VRAM stays <= max(oldBytes, newBytes)");
			Assert::IsTrue(disp.PeakAllocatedBytes() < oldBytes + newBytes,
				L"K#12: joined reconfigure never spikes to oldBytes + newBytes");
			Assert::AreEqual(newBytes, disp.AllocatedBytes(),
				L"K#12: live allocation equals newBytes");

			// In-test negative control (oldJobsJoined = false): reconfigure back to 64x48 keeps old set in graveyard
			// and peaks at newBytes + oldBytes.
			disp.ResetPeakBytesForTesting();
			Assert::IsTrue(disp.Configure(s1.graph, s1.reg, s1.plan, s1.dims, 0u, false),
				L"K#12 negative control: reconfigure 96x64 -> 64x48 with oldJobsJoined=false succeeds");
			Assert::IsTrue(disp.PendingReleases() >= 1u,
				L"K#12 negative control: unjoined path queues old texture set in graveyard");
			Assert::AreEqual(newBytes, disp.GraveyardBytes(),
				L"K#12 negative control: graveyard holds full previous texture set bytes");
			Assert::AreEqual(newBytes + oldBytes, disp.PeakAllocatedBytes(),
				L"K#12 negative control: unjoined path peaks at newBytes + oldBytes");
			Assert::AreEqual(oldBytes, disp.AllocatedBytes(),
				L"live allocation equals oldBytes after shrinking back to 64x48");
			Assert::IsTrue(disp.HoldsVideoMemory(), L"HoldsVideoMemory true with live + graveyard textures");

			disp.FlushReleases();
			Assert::AreEqual(size_t(0), disp.PendingReleases(), L"FlushReleases drains graveyard");
			Assert::AreEqual(uint64_t(0), disp.GraveyardBytes(), L"FlushReleases zeroes GraveyardBytes");
			disp.Release();
			Assert::AreEqual(uint64_t(0), disp.AllocatedBytes(), L"Release frees all live textures");
			Assert::IsFalse(disp.HoldsVideoMemory(), L"HoldsVideoMemory false after Release");
		}

		// W23-C2-S1-H1 (bff541a87) + MEDIUM-4 (24d188643):
		// (1) EnsureTap re-runs ExtendTap when P4 active-picture ancestry rect changes on an already-extended tap
		//     (e.g. full-frame {0,0,w,h} -> letterbox {0,12,w,52}), reflecting active row 12 into letterbox rows 0..11
		//     instead of keeping stale black bars.
		// (2) Failed ExtendTap clears t.extendedDone BEFORE ExtendTap and updates t.picture ONLY AFTER ExtendTap succeeds,
		//     so a retry with the same cropped P4 rect re-extends cleanly instead of skipping ExtendTap with stale bars.
		// (3) Changing SourceFrameView::luma or msbShift for the same seq re-uploads tap bands and re-runs ExtendTap,
		//     and DispatchLive clears t->pinned on kP10GrainSplit finish/fail so tap slots are never leaked/starved.
		BEGIN_TEST_METHOD_ATTRIBUTE(EnsureTapReExtendsOnP4CropSourceChangeAndFailedExtendRetry)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(EnsureTapReExtendsOnP4CropSourceChangeAndFailedExtendRetry)
		{
			class P11CopyExtendedKernel : public VpLaneProducts::IProductKernel
			{
			public:
				ProductId Product() const override { return kP11RegionBlockStats; }
				ProductTexLayout Layout(Dims) const override
				{
					ProductTexLayout l;
					TexTerm a;
					a.shift = 4;
					a.fmt = TexFormat::RGBA32F;
					for (int i = 0; i < 6; ++i)
						l.terms.push_back(a);
					return l;
				}
				uint32_t Steps(Dims) const override { return 1u; }
				bool NeedsTap() const override { return true; }
				bool Step(VpLaneProducts::KernelContext& c, uint32_t) override
				{
					if (!c.extended || c.outCount < 1)
						return false;
					return Pass(c.dispatch,
						"ivec2 p = ivec2(gl_GlobalInvocationID.xy);\n"
						"if (p.x >= 1 || p.y >= 1) return;\n"
						"float v = floor(texelFetch(t_src, ivec2(0, 0), 0).x * 65535.0 + 0.5) / 64.0;\n"
						"imageStore(t_dst, p, vec4(v, 0.0, 0.0, 1.0));\n",
						c.extended, c.out[0], 1, 1);
				}
				void Release(pl_gpu) override {}
			};

			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			SkeletonGpuDispatcher disp(gpu, true);
			disp.AddKernel(std::unique_ptr<VpLaneProducts::IProductKernel>(new CopyKernel()));
			disp.AddKernel(std::unique_ptr<VpLaneProducts::IProductKernel>(new P11CopyExtendedKernel()));

			const Dims d{ 64, 64 };
			Registry reg = VpLaneProducts::TapFormProducts(SkeletonProducts());
			ConsumerDecl cDecl;
			cDecl.id = 1;
			AddConsume(cDecl, kP10GrainSplit);
			AddConsume(cDecl, kP11RegionBlockStats);
			GraphSettings gs;
			gs.dims = d;
			gs.queueDepth = 4;
			gs.laneLead = 1;
			const Graph graph = BuildGraph(reg, std::vector<ConsumerDecl>{ cDecl }, gs);
			TierPlan plan;
			for (ProductId pid : { kP10GrainSplit, kP11RegionBlockStats })
			{
				TierEntry e;
				e.id = pid;
				e.backend = Backend::GpuCompute;
				e.grant = Grant::Lane;
				plan.entries.push_back(e);
			}
			Assert::IsTrue(disp.Configure(graph, reg, plan, d), L"configure P10 + P11");

			const Node* p10Node = nullptr;
			const Node* p11Node = nullptr;
			for (const Node& nd : graph.nodes)
			{
				if (nd.id == kP10GrainSplit) p10Node = &nd;
				if (nd.id == kP11RegionBlockStats) p11Node = &nd;
			}
			Assert::IsTrue(p10Node != nullptr && p11Node != nullptr, L"P10 and P11 nodes present");

			// Synthesize letterboxed frame (rows 0..11 and 52..63 = black bar code 64; active rows 12..51 = code 800).
			std::vector<uint16_t> codesA(size_t(d.width) * d.height, 64u);
			for (uint32_t y = 12; y < 52; ++y)
				for (uint32_t x = 0; x < d.width; ++x)
					codesA[size_t(y) * d.width + x] = 800u;
			SourceFrameView viewA = ViewOf(codesA, int(d.width), int(d.height), 20u);

			// 1. Run P10 for seq 20 WITHOUT P4 (full-frame rect {0,0,64,64}): row 0 stays bar code 64.
			LaneJobContext p10Job;
			p10Job.node = p10Node;
			p10Job.seq = 20u;
			p10Job.ringSlot = 0;
			p10Job.source = &viewA;
			const uint32_t p10Steps = VpLaneProducts::StepsFor(kP10GrainSplit, d);
			for (uint32_t i = 0; i < p10Steps; ++i)
				Assert::IsTrue(disp.Dispatch(p10Job, i, p10Steps), L"initial full-frame P10 dispatch");
			Assert::AreEqual(uint64_t(1), disp.Stats().extends, L"1 full-frame ExtendTap run");
			const auto fullOut = Download<float>(gpu, disp.TexturesOf(kP10GrainSplit)->Ring(0, 0), 4);
			Assert::AreEqual(64.0f, fullOut[0], 0.0f, L"negative control (uncropped P4): row 0 holds black bar code 64");

			// Prepare P11 job for the SAME seq 20 WITH P4 active-picture rect {0, 12, 64, 52} (half-plane rectY0=6, rectY1=26).
			size_t p4InputIdx = 0;
			bool foundP4 = false;
			for (size_t k = 0; k < p11Node->inputs.size(); ++k)
			{
				if (p11Node->inputs[k].product == kP4SourceStats)
				{
					p4InputIdx = k;
					foundP4 = true;
					break;
				}
			}
			Assert::IsTrue(foundP4, L"P11 consumes kP4SourceStats");

			std::vector<uint8_t> p4CropBytes(VpLaneProducts::kStatsBytes, 0u);
			{
				VpUnit::SourceStats::Record rec{};
				rec.version = VpUnit::SourceStats::kRecordVersion;
				rec.halfWidth = d.width / 2u;
				rec.halfHeight = d.height / 2u;
				rec.rectFound = 1u;
				rec.rectX0 = 0u;
				rec.rectY0 = 6u;  // full top = 12
				rec.rectX1 = d.width / 2u;
				rec.rectY1 = 26u; // full bottom = 52
				std::memcpy(p4CropBytes.data(), &rec, sizeof(rec));
			}
			SlotSet dummyP4Set(kP4SourceStats, 4u, 1u);
			LaneInputs p11Inputs;
			p11Inputs.count = p11Node->inputs.size();
			p11Inputs.sets[p4InputIdx] = &dummyP4Set;
			p11Inputs.seqs[p4InputIdx] = 20u;
			p11Inputs.liveIndex[p4InputIdx] = 0;

			LaneJobContext p11CropJob;
			p11CropJob.node = p11Node;
			p11CropJob.seq = 20u;
			p11CropJob.ringSlot = 0;
			p11CropJob.source = &viewA;
			p11CropJob.inputs = &p11Inputs;
			p11CropJob.inputData[p4InputIdx] = p4CropBytes.data();

			// 2. Force ExtendTap to fail once (temporarily mismatch extended->params.w so ExtendTap returns false at entry
			//    without touching GPU state), then restore and retry with the exact same cropped P4 rect (24d188643 MEDIUM-4)!
			pl_tex extTex = disp.TapOf(20u, true);
			Assert::IsNotNull(extTex, L"extended tap exists for seq 20");
			const int savedW = extTex->params.w;
			const_cast<pl_tex_t*>(extTex)->params.w = 0;
			const bool failedAttempt = disp.Dispatch(p11CropJob, 0, 1);
			const_cast<pl_tex_t*>(extTex)->params.w = savedW;
			Assert::IsFalse(failedAttempt, L"forced ExtendTap failure declines Dispatch");
			Assert::AreEqual(uint64_t(1), disp.Stats().extends, L"failed ExtendTap does not increment extends");

			// Retry with restored width and SAME cropped P4 rect: must re-run ExtendTap and mirror active row 12 (800) into row 0!
			Assert::IsTrue(disp.Dispatch(p11CropJob, 0, 1), L"24d188643 MEDIUM-4: retry after failed ExtendTap succeeds");
			Assert::AreEqual(uint64_t(2), disp.Stats().extends,
				L"S1-H1 + MEDIUM-4: P4 crop replacement + failed-extend retry re-ran ExtendTap");
			const auto cropOut = Download<float>(gpu, disp.TexturesOf(kP11RegionBlockStats)->Ring(0, 0), 4);
			Assert::AreEqual(800.0f, cropOut[0], 0.0f,
				L"S1-H1: ExtendTap reflected active-picture code 800 over letterbox bar row 0 (was 64 before re-extend)");

			// 3. Changing SourceFrameView::luma on the same seq 20 re-uploads tap bands and re-extends!
			std::vector<uint16_t> codesB(size_t(d.width) * d.height, 512u);
			SourceFrameView viewB = ViewOf(codesB, int(d.width), int(d.height), 20u);
			p11CropJob.source = &viewB;
			const uint64_t bandsBefore = disp.Stats().tapBands;
			Assert::IsTrue(disp.Dispatch(p11CropJob, 0, 1), L"S1-H1: source pointer replacement on seq 20 succeeds");
			Assert::IsTrue(disp.Stats().tapBands > bandsBefore && disp.Stats().extends == 3u,
				L"S1-H1: changing view.luma re-uploaded tap bands and re-ran ExtendTap");
			const auto replacedOut = Download<float>(gpu, disp.TexturesOf(kP11RegionBlockStats)->Ring(0, 0), 4);
			Assert::AreEqual(512.0f, replacedOut[0], 0.0f, L"S1-H1: updated luma codes 512 propagated through re-uploaded tap");

			// 4. DispatchLive on kP10GrainSplit clears t->pinned so >kMaxTaps (16) live frames never starve the tap ring!
			disp.SetLiveSourceForTesting([&codesB, d](uint64_t seq, SourceFrameView* out) {
				*out = ViewOf(codesB, int(d.width), int(d.height), seq);
				return true;
			});
			for (uint64_t seq = 100u; seq < 120u; ++seq)
				Assert::IsTrue(disp.DispatchLive(*p10Node, seq, 0, Header(), 0u),
					L"S1-H1: DispatchLive clears t->pinned on kP10GrainSplit so 20 consecutive frames never exhaust tap slots");
		}
	};
}
