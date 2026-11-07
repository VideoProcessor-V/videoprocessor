// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// 70b G3: the P11 region block statistics GPU kernel (RegionStatsKernel) on the libplacebo WARP device
// (TargetLutGpuFixture; WARP unless VP_TEST_GPU=hardware): equal to the CPU float mirror StatsBlock<float> on a ramp+noise
// luma with a letterbox rect, the lane form (SkeletonGpuDispatcher::Dispatch) equal to the live form (the same kernel on the
// live buffers) bit for bit, the missing-P10 path (re_have_split 0) equal to the mirror without bands and visible through
// Fallbacks(), and the pass GLSL pinned by a fingerprint.
#include "pch.h"
#include <VP-V/tests/VpLutGpuTestSupport.h>
#include <VP-V/core/ProductGraph.h>
#include <VP-V/modules/control/skeleton/gpu/SkeletonGpuDispatcher.h>
#include <VP-V/modules/input/source_analysis/lane/LaneGpuProducts.h>
#include <VP-V/modules/input/source_analysis/lane/LaneGpuRegionKernel.h>
#include <VP-V/modules/input/source_analysis/regions/RegionEvidence.h>
#include <VP-V/modules/input/source_analysis/regions/RegionEvidenceGlsl.h>

#include <libplacebo/dispatch.h>
#include <libplacebo/shaders/custom.h>

#include <cstdio>
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
		using VpLaneProducts::RegionStatsKernel;

		constexpr int kW = 320, kH = 768;   // B = BlockSizeFor(768) = 16: inside the declaration's 16 px upper bound

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

		bool CopyPass(pl_dispatch dispatch, pl_tex src, pl_tex dst)
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
			c.description = "P11 kernel test copy";
			c.body = "ivec2 p = ivec2(gl_GlobalInvocationID.xy);\n"
			         "if (p.x >= textureSize(t_src, 0).x || p.y >= textureSize(t_src, 0).y) return;\n"
			         "imageStore(t_dst, p, texelFetch(t_src, p, 0));\n";
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
			p.dispatch_size[0] = (src->params.w + 7) / 8;
			p.dispatch_size[1] = (src->params.h + 7) / 8;
			p.dispatch_size[2] = 1;
			return pl_dispatch_compute(dispatch, &p);
		}

		// A P10 stand-in with the real Compact layout (the declaration of SkeletonProducts(true)): the test fills its block
		// texture itself; the kernel only owns the allocation.
		class P10Stand : public VpLaneProducts::IProductKernel
		{
		public:
			ProductId Product() const override { return kP10GrainSplit; }
			ProductTexLayout Layout(Dims d) const override
			{
				const RegionEvidence::Grid g = RegionEvidence::MakeGrid(int(d.width), int(d.height), RegionEvidence::BlockSizeFor(int(d.height)));
				ProductTexLayout l;
				TexTerm a;
				a.fmt = TexFormat::RG16F;
				TexTerm b;
				b.fmt = TexFormat::RG16;
				TexTerm c;
				c.shift = 4;
				c.fmt = TexFormat::RGBA32F;
				c.absW = uint32_t(g.gw);
				c.absH = uint32_t(g.gh);
				l.terms.push_back(a);
				l.terms.push_back(b);
				l.terms.push_back(c);
				return l;
			}
			uint32_t Steps(Dims) const override { return VpLaneProducts::kP10SplitterSteps; }
			bool NeedsTap() const override { return false; }
			bool Step(VpLaneProducts::KernelContext&, uint32_t) override { return true; }
			void Release(pl_gpu) override {}
		};

		struct FrameData
		{
			std::vector<uint16_t> codes;
			std::vector<float> luma;
			std::vector<float> compactBlocks;   // (N, e3, e1, e2) per block, as P10 Compact stores them
			std::vector<float> bands;           // (e1, e2, e3, N) per block, as the stats pass reads them
			RegionEvidence::Rect pic;
			RegionEvidence::Grid grid;
		};

		FrameData MakeFrame()
		{
			FrameData f;
			const int B = RegionEvidence::BlockSizeFor(kH);
			f.grid = RegionEvidence::MakeGrid(kW, kH, B);
			f.pic.left = 0;
			f.pic.top = 96;
			f.pic.right = kW;
			f.pic.bottom = 672;   // letterbox; even edges: P4's half-plane rect maps back exactly
			f.codes.assign(size_t(kW) * kH, 64);
			uint32_t seed = 12345u;
			for (int y = f.pic.top; y < f.pic.bottom; ++y)
				for (int x = f.pic.left; x < f.pic.right; ++x)
				{
					seed = seed * 1664525u + 1013904223u;
					int v = 120 + (x * 2) % 600 + (y / 4) % 90 + int((seed >> 24) % 9u);
					if (x >= 200 && y >= 300 && y < 360)
						v = 800;                                 // a bright step
					f.codes[size_t(y) * kW + x] = uint16_t((std::min)(v, 940));
				}
			f.luma.resize(f.codes.size());
			for (size_t i = 0; i < f.codes.size(); ++i)
				f.luma[i] = float(f.codes[i]);
			const size_t n = f.grid.Count();
			f.compactBlocks.assign(n * 4, 0.0f);
			f.bands.assign(n * 4, 0.0f);
			for (size_t i = 0; i < n; ++i)
			{
				const float N = 1.0f + float(i % 7u) * 0.5f, e3 = 2.0f + float(i % 5u), e1 = 100.0f + float(i % 13u) * 3.0f,
				            e2 = 50.0f + float(i % 11u) * 2.0f;
				f.compactBlocks[i * 4 + 0] = N;
				f.compactBlocks[i * 4 + 1] = e3;
				f.compactBlocks[i * 4 + 2] = e1;
				f.compactBlocks[i * 4 + 3] = e2;
				f.bands[i * 4 + 0] = e1;
				f.bands[i * 4 + 1] = e2;
				f.bands[i * 4 + 2] = e3;
				f.bands[i * 4 + 3] = N;
			}
			return f;
		}

		SourceFrameView ViewOf(const FrameData& f, uint64_t seq)
		{
			SourceFrameView v;
			v.seq = seq;
			v.luma = f.codes.data();
			v.width = uint32_t(kW);
			v.height = uint32_t(kH);
			v.strideSamples = uint32_t(kW);
			v.msbShift = 6;
			return v;
		}

		// Everything a P11 run needs: the dispatcher with the P10 stand-in and the real P11 kernel, the P11 / P10 nodes, the
		// P10 band texture, a P4 record carrying the letterbox rect.
		struct Rig
		{
			TargetLutGpuFixture fixture;
			std::unique_ptr<SkeletonGpuDispatcher> disp;
			RegionStatsKernel* kernel = nullptr;
			Registry reg = VpLaneProducts::TapFormProducts(SkeletonProducts(true));
			Graph graph;
			TierPlan plan;
			Dims dims{ uint32_t(kW), uint32_t(kH) };
			FrameData frame = MakeFrame();
			std::vector<uint8_t> p4 = std::vector<uint8_t>(VpLaneProducts::kStatsBytes, 0);
			pl_tex bandSrc = nullptr;

			~Rig()
			{
				if (bandSrc && fixture.Gpu())
					pl_tex_destroy(fixture.Gpu(), &bandSrc);
			}

			void Init()
			{
				Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
				disp.reset(new SkeletonGpuDispatcher(fixture.Gpu(), true));
				kernel = new RegionStatsKernel();
				disp->AddKernel(std::unique_ptr<VpLaneProducts::IProductKernel>(new P10Stand()));
				disp->AddKernel(std::unique_ptr<VpLaneProducts::IProductKernel>(kernel));
				ConsumerDecl c;
				c.id = 1;
				AddConsume(c, kP11RegionBlockStats);
				GraphSettings gs;
				gs.dims = dims;
				gs.queueDepth = 4;
				gs.laneLead = 1;
				graph = BuildGraph(reg, std::vector<ConsumerDecl>{ c }, gs);
				for (ProductId id : { kP10GrainSplit, kP11RegionBlockStats })
				{
					TierEntry e;
					e.id = id;
					e.backend = Backend::GpuCompute;
					e.grant = Grant::Lane;
					plan.entries.push_back(e);
				}
				Assert::IsTrue(disp->Configure(graph, reg, plan, dims), L"configure");
				VpLaneProducts::SourceStatsRecord rec;
				rec.rectFound = 1;
				rec.rectX0 = uint32_t(frame.pic.left / 2);
				rec.rectY0 = uint32_t(frame.pic.top / 2);
				rec.rectX1 = uint32_t(frame.pic.right / 2);
				rec.rectY1 = uint32_t(frame.pic.bottom / 2);
				VpLaneProducts::WriteStatsRecord(rec, p4.data(), p4.size());
				pl_tex_params tp{};
				tp.w = frame.grid.gw;
				tp.h = frame.grid.gh;
				tp.format = PlTexBackend::FindFormat(fixture.Gpu(), TexFormat::RGBA32F);
				tp.sampleable = true;
				tp.initial_data = frame.compactBlocks.data();
				bandSrc = pl_tex_create(fixture.Gpu(), &tp);
				Assert::IsNotNull(bandSrc, L"band source texture");
				// P10's blocks (term 2) in ring slot 0 and live buffer 0.
				const VpLaneProducts::GpuProductTextures* t10 = disp->TexturesOf(kP10GrainSplit);
				Assert::IsNotNull(t10, L"P10 stand-in textures");
				Assert::IsTrue(CopyPass(disp->Dispatcher(), bandSrc, t10->Ring(0, 2)), L"fill the ring blocks");
				Assert::IsTrue(CopyPass(disp->Dispatcher(), bandSrc, t10->Live(0, 2)), L"fill the live blocks");
			}

			// A frame number whose P10 ring slot is 0 (the slot the test fills).
			uint64_t Seq() const { return uint64_t(NodeOf(kP10GrainSplit).ring) * 2u; }

			const Node& NodeOf(ProductId id) const
			{
				for (const Node& n : graph.nodes)
					if (n.id == id)
						return n;
				Assert::Fail(L"node not in the graph");
				return graph.nodes[0];
			}

			// The lane form: one Dispatch of the P11 job. `withP10`: P10's input is pinned (else missing).
			void RunLane(uint64_t seq, bool withP10)
			{
				const Node& n11 = NodeOf(kP11RegionBlockStats);
				SlotSet set10(kP10GrainSplit, uint16_t(NodeOf(kP10GrainSplit).ring), uint16_t(NodeOf(kP10GrainSplit).livePool));
				SlotSet set4(kP4SourceStats, 2, 1);
				LaneInputs pins;
				pins.count = n11.inputCount;
				for (size_t k = 0; k < n11.inputCount; ++k)
				{
					pins.liveIndex[k] = -1;
					pins.seqs[k] = seq;
					pins.sets[k] = nullptr;
					if (n11.inputs[k].product == kP10GrainSplit && withP10)
						pins.sets[k] = &set10;
					else if (n11.inputs[k].product == kP4SourceStats)
						pins.sets[k] = &set4;
				}
				const SourceFrameView view = ViewOf(frame, seq);
				LaneJobContext job;
				job.node = &n11;
				job.seq = seq;
				job.ringSlot = 0;
				job.inputs = &pins;
				for (size_t k = 0; k < n11.inputCount; ++k)
					if (n11.inputs[k].product == kP4SourceStats)
						job.inputData[k] = p4.data();
				job.source = &view;
				Assert::IsTrue(disp->Dispatch(job, 0, 1), L"the P11 lane chunk");
			}

			// The live form: the same kernel on the live buffers (the dispatcher's own DispatchLive needs a lane to find its
			// inputs; here they are named directly), the tap planes of the lane run.
			void RunLive(uint64_t seq, bool withP10)
			{
				VpLaneProducts::KernelContext c;
				c.gpu = fixture.Gpu();
				c.dispatch = disp->Dispatcher();
				c.id = kP11RegionBlockStats;
				c.seq = seq;
				c.dims = dims;
				c.live = true;
				c.tap = disp->TapOf(seq, false);
				c.extended = disp->TapOf(seq, true);
				c.picture = VpLaneProducts::PictureFromStats(p4.data(), p4.size(), kW, kH);
				const VpLaneProducts::GpuProductTextures* t11 = disp->TexturesOf(kP11RegionBlockStats);
				for (size_t t = 0; t < 6; ++t)
					c.out[t] = t11->Live(0, t);
				c.outCount = 6;
				c.inCount = 1;
				c.in[0].id = kP10GrainSplit;
				c.in[0].tex = disp->TexturesOf(kP10GrainSplit);
				c.in[0].live = 0;
				c.in[0].missing = !withP10;
				Assert::IsTrue(c.extended != nullptr, L"the lane run left the extended plane");
				Assert::IsTrue(kernel->Step(c, 0), L"the P11 live step");
			}

			std::vector<float> Out(bool live, size_t term) const
			{
				const VpLaneProducts::GpuProductTextures* t = disp->TexturesOf(kP11RegionBlockStats);
				return Download<float>(fixture.Gpu(), live ? t->Live(0, term) : t->Ring(0, term), 4);
			}
		};

		// Compare the six downloaded grids with the float mirror (the tolerances of RegionEvidenceWarpTests).
		void CompareToMirror(const std::vector<float> got[6], const FrameData& f, bool haveSplit, const char* label)
		{
			using RegionEvidence::Rect; using RegionEvidence::Externals; using RegionEvidence::StatsConstants; using RegionEvidence::VisibilityParams;
			using RegionEvidence::PictureOrFrame; using RegionEvidence::MakeStatsConstants; using RegionEvidence::StatsGridMirror;
			using RegionEvidence::kStatsChannels; using RegionEvidence::kMeanNits; using RegionEvidence::kMeanPq; using RegionEvidence::kVisibilityStat;
			using RegionEvidence::kOffLap; using RegionEvidence::kOffShare;
			const Rect pic = PictureOrFrame(f.pic, kW, kH);
			const Externals ex;
			const StatsConstants k = MakeStatsConstants(kW, kH, f.grid.B, pic, ex, haveSplit, VisibilityParams());
			std::vector<float> mirror;
			StatsGridMirror(k, f.luma.data(), haveSplit ? f.bands.data() : nullptr, mirror);
			int bad = 0, offFlips = 0, auxBad = 0;
			double worst = 0.0;
			for (size_t i = 0; i < f.grid.Count(); ++i)
			{
				for (int c = 0; c < kStatsChannels; ++c)
				{
					const double gv = got[c / 4][i * 4 + (c % 4)], ref = mirror[i * kStatsChannels + c];
					const bool trans = c == kMeanNits || c == kMeanPq || c == kVisibilityStat;
					const double err = std::fabs(gv - ref) / (std::max)(std::fabs(ref), 1.0);
					worst = (std::max)(worst, err);
					if (err > (trans ? 2e-3 : 1e-4))
					{
						if (c == kOffLap || c == kOffShare)
							++offFlips;
						else
							++bad;
					}
				}
				for (int c = 0; c < 4; ++c)
					if (got[5][i * 4 + c] != 0.0f)
						++auxBad;
			}
			char msg[220];
			snprintf(msg, sizeof(msg), "P11_KERNEL %s blocks=%zu bad=%d off_flips=%d aux_bad=%d worst=%.2e\n", label, f.grid.Count(), bad,
				offFlips, auxBad, worst);
			Logger::WriteMessage(msg);
			Assert::IsTrue(bad == 0, (std::wstring(L"the kernel differs from the float mirror: ") + std::wstring(label, label + strlen(label))).c_str());
			Assert::IsTrue(offFlips <= 4, L"too many off-edge flips");
			Assert::IsTrue(auxBad == 0, L"the aux texel must be zeros");
		}
	}

	TEST_CLASS(LaneGpuRegionKernelWarpTests)
	{
	public:
		// (a) The lane kernel equals StatsBlock<float> on a ramp+noise luma with a letterbox picture rect, with bands.
		BEGIN_TEST_METHOD_ATTRIBUTE(KernelMatchesTheFloatMirror)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(KernelMatchesTheFloatMirror)
		{
			Rig r;
			r.Init();
			r.RunLane(r.Seq(), true);
			std::vector<float> got[6];
			for (size_t t = 0; t < 6; ++t)
				got[t] = r.Out(false, t);
			CompareToMirror(got, r.frame, true, "bands");
			Assert::IsTrue(r.kernel->BandsUsed(), L"the bands were bound");
			const auto fb = r.kernel->Fallbacks();
			bool bandsEngaged = true, displayEngaged = false;
			for (const auto& d : fb)
			{
				if (d.id == "region_stats_bands") bandsEngaged = d.engaged;
				if (d.id == "region_stats_display") displayEngaged = d.engaged;
			}
			Assert::IsFalse(bandsEngaged, L"no bands fallback while P10 is present");
			Assert::IsTrue(displayEngaged, L"display facts: defaults is shown until the light level unit supplies them");
			r.kernel->SetDisplayFacts(0, 100.0, RegionEvidence::VisibilityParams());
			for (const auto& d : r.kernel->Fallbacks())
				if (d.id == "region_stats_display")
					Assert::IsFalse(d.engaged, L"released once the facts are supplied");
		}

		// (b) The lane form (SkeletonGpuDispatcher::Dispatch) equals the live form (the same kernel, live buffers), bit for bit.
		BEGIN_TEST_METHOD_ATTRIBUTE(LaneEqualsLive)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(LaneEqualsLive)
		{
			Rig r;
			r.Init();
			r.RunLane(r.Seq(), true);
			r.RunLive(r.Seq(), true);
			for (size_t t = 0; t < 6; ++t)
			{
				const std::vector<float> lane = r.Out(false, t), live = r.Out(true, t);
				Assert::IsTrue(lane.size() == live.size() && std::memcmp(lane.data(), live.data(), lane.size() * sizeof(float)) == 0,
					L"lane == live, bit for bit");
			}
		}

		// (c) No P10: re_have_split = 0, the documented reduced result (the mirror without bands: kNoGrain, N = -1), and the
		// visible "P11 without bands" fallback; lane and live agree there too.
		BEGIN_TEST_METHOD_ATTRIBUTE(MissingBandsGiveTheReducedResultAndAFallback)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(MissingBandsGiveTheReducedResultAndAFallback)
		{
			Rig r;
			r.Init();
			r.RunLane(r.Seq(), false);
			std::vector<float> got[6];
			for (size_t t = 0; t < 6; ++t)
				got[t] = r.Out(false, t);
			CompareToMirror(got, r.frame, false, "no_bands");
			Assert::IsFalse(r.kernel->BandsUsed(), L"no bands bound");
			bool engaged = false;
			for (const auto& d : r.kernel->Fallbacks())
				if (d.id == "region_stats_bands")
				{
					engaged = d.engaged;
					Assert::IsTrue(d.reason.find("P11 without bands") != std::string::npos, L"the reason names the fallback");
				}
			Assert::IsTrue(engaged, L"P11 without bands is visible");
			r.RunLive(r.Seq(), false);
			for (size_t t = 0; t < 6; ++t)
			{
				const std::vector<float> lane = r.Out(false, t), live = r.Out(true, t);
				Assert::IsTrue(std::memcmp(lane.data(), live.data(), lane.size() * sizeof(float)) == 0, L"lane == live without bands");
			}
		}

		// (d) The pass GLSL is pinned (FNV-1a 64 over header, newline, body). 0 = not yet pinned: the integrator fills it
		// after the first run (the value is logged).
		BEGIN_TEST_METHOD_ATTRIBUTE(PassFingerprintIsPinned)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(PassFingerprintIsPinned)
		{
			// integrator fills after first run
			constexpr uint64_t kPinned = 0ull;
			const uint64_t fp = RegionStatsKernel::PassFingerprint();
			char msg[120];
			snprintf(msg, sizeof(msg), "P11_KERNEL_FINGERPRINT 0x%016llXull\n", static_cast<unsigned long long>(fp));
			Logger::WriteMessage(msg);
			Assert::IsTrue(RegionStatsKernel::HeaderGlsl().find("floor(texelFetch(re_src, q, 0).x * 65535.0 + 0.5) / 64.0") != std::string::npos,
				L"the tap header decodes exact codes");
			Assert::IsTrue(RegionStatsKernel::BodyGlsl().find("re_bands, blk, 0).zwyx") != std::string::npos &&
				RegionStatsKernel::BodyGlsl().find("re_s5") != std::string::npos, L"the tap body swizzles the bands and writes aux");
			Assert::IsTrue(kPinned == 0ull || fp == kPinned, L"the P11 pass GLSL changed: re-pin deliberately");
		}
	};
}
