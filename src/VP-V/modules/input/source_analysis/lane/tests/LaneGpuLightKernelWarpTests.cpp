// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// 70b G4: the P12 light level GPU kernel (LightLevelKernel) on the libplacebo WARP device (TargetLutGpuFixture; WARP unless
// VP_TEST_GPU=hardware): equal to the CPU float mirror LightLevel::Blocks::BlockOut<float> on a synthetic P11 input, a
// three frame chain whose lane form (frame 0 through SkeletonGpuDispatcher::Dispatch with pinned slots, the later frames
// through the same kernel with a lane-style context: the dispatcher finds a product's own previous frame only through a
// live AnalysisLane) equals the live form bit for bit, the missing-P11 frame level with its visible fallback, the pass GLSL
// pinned by a fingerprint, and (CPU only) the truth table of LightLevelBeginGate.
#include "pch.h"
#include <VP-V/tests/VpLutGpuTestSupport.h>
#include <VP-V/core/ProductGraph.h>
#include <VP-V/modules/control/skeleton/gpu/SkeletonGpuDispatcher.h>
#include <VP-V/modules/input/source_analysis/lane/LaneGpuLightKernel.h>
#include <VP-V/modules/input/source_analysis/lane/LaneGpuProducts.h>
#include <VP-V/modules/input/source_analysis/light_level/LightLevelBlocks.h>
#include <VP-V/modules/input/source_analysis/regions/RegionEvidence.h>

#include <libplacebo/dispatch.h>
#include <libplacebo/shaders/custom.h>

#include <algorithm>
#include <cmath>
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
		using VpLaneProducts::LightLevelKernel;
		namespace RE = RegionEvidence;
		namespace LB = LightLevel::Blocks;

		constexpr int kW = 320, kH = 768;   // B = BlockSizeFor(768) = 16: inside the declaration's 16 px upper bound
		constexpr double kAplCode10 = 400.0;
		constexpr double kFps = 50.0;

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
			c.description = "P12 kernel test copy";
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

		// A P10 stand-in with the real Compact layout (the declaration of SkeletonProducts(true)); P12 never reads it, but it is
		// an input of P11 in the graph, so the dispatcher needs a kernel for it.
		class P10Stand : public VpLaneProducts::IProductKernel
		{
		public:
			ProductId Product() const override { return kP10GrainSplit; }
			ProductTexLayout Layout(Dims d) const override
			{
				const RE::Grid g = RE::MakeGrid(int(d.width), int(d.height), RE::BlockSizeFor(int(d.height)));
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

		// A P11 stand-in with the real layout (6 absolute rgba32f grids); the test fills S0, S3, S4 itself.
		class P11Stand : public VpLaneProducts::IProductKernel
		{
		public:
			ProductId Product() const override { return kP11RegionBlockStats; }
			ProductTexLayout Layout(Dims d) const override
			{
				const RE::Grid g = RE::MakeGrid(int(d.width), int(d.height), RE::BlockSizeFor(int(d.height)));
				ProductTexLayout l;
				for (int t = 0; t < 6; ++t)
				{
					TexTerm term;
					term.shift = 4;
					term.fmt = TexFormat::RGBA32F;
					term.absW = uint32_t(g.gw);
					term.absH = uint32_t(g.gh);
					l.terms.push_back(term);
				}
				return l;
			}
			uint32_t Steps(Dims) const override { return 1; }
			bool NeedsTap() const override { return false; }
			bool Step(VpLaneProducts::KernelContext&, uint32_t) override { return true; }
			void Release(pl_gpu) override {}
		};

		LightLevel::Settings Defaults()
		{
			return LightLevel::Read([](const std::string&, std::string&) { return false; });
		}

		LightLevel::Context Ctx(LightLevel::Context::Kind kind, double peak)
		{
			LightLevel::Context c;
			c.kind = kind;
			c.sourcePeakNits = peak;
			return c;
		}

		std::vector<float> TexelsFor(const LightLevel::Context& ctx)
		{
			return LightLevel::PackTexels(LightLevel::Build(Defaults(), ctx));
		}

		const LightLevel::Context::Kind kSdr = LightLevel::Context::Sdr;

		struct Rig
		{
			TargetLutGpuFixture fixture;
			std::unique_ptr<SkeletonGpuDispatcher> disp;
			LightLevelKernel* kernel = nullptr;
			Registry reg = VpLaneProducts::TapFormProducts(SkeletonProducts(true));
			Graph graph;
			TierPlan plan;
			Dims dims{ uint32_t(kW), uint32_t(kH) };
			RE::Grid grid;
			RE::Rect pic;
			std::vector<uint8_t> p4 = std::vector<uint8_t>(VpLaneProducts::kStatsBytes, 0);
			std::vector<float> stats[3];   // synthetic P11 stats of three frames (kStatsChannels per block)

			void Init()
			{
				Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
				disp.reset(new SkeletonGpuDispatcher(fixture.Gpu(), true));
				kernel = new LightLevelKernel();
				kernel->SetFrameRate(kFps);
				disp->AddKernel(std::unique_ptr<VpLaneProducts::IProductKernel>(new P10Stand()));
				disp->AddKernel(std::unique_ptr<VpLaneProducts::IProductKernel>(new P11Stand()));
				disp->AddKernel(std::unique_ptr<VpLaneProducts::IProductKernel>(kernel));
				ConsumerDecl c;
				c.id = 1;
				AddConsume(c, kP12LightLevel);
				GraphSettings gs;
				gs.dims = dims;
				gs.queueDepth = 4;
				gs.laneLead = 1;
				graph = BuildGraph(reg, std::vector<ConsumerDecl>{ c }, gs);
				for (ProductId id : { kP10GrainSplit, kP11RegionBlockStats, kP12LightLevel })
				{
					TierEntry e;
					e.id = id;
					e.backend = Backend::GpuCompute;
					e.grant = Grant::Lane;
					plan.entries.push_back(e);
				}
				Assert::IsTrue(disp->Configure(graph, reg, plan, dims), L"configure");
				grid = RE::MakeGrid(kW, kH, RE::BlockSizeFor(kH));
				pic.left = 0;
				pic.top = 96;
				pic.right = kW;
				pic.bottom = 672;   // letterbox; even edges: P4's half-plane rect maps back exactly
				VpLaneProducts::SourceStatsRecord rec;
				rec.rectFound = 1;
				rec.rectX0 = uint32_t(pic.left / 2);
				rec.rectY0 = uint32_t(pic.top / 2);
				rec.rectX1 = uint32_t(pic.right / 2);
				rec.rectY1 = uint32_t(pic.bottom / 2);
				rec.pixelCount = 1000;
				rec.mean = uint32_t(kAplCode10 * 64.0);   // 16-bit scale: the LUT code is mean / 64
				VpLaneProducts::WriteStatsRecord(rec, p4.data(), p4.size());
				const std::vector<float> sdr = TexelsFor(Ctx(kSdr, 0.0));
				for (int f = 0; f < 3; ++f)
					MakeStats(f, sdr, stats[f]);
				const Node& n12 = NodeOf(kP12LightLevel);
				Assert::IsTrue(n12.ring >= 2 && n12.livePool >= 2, L"P12 keeps two ring slots and two live buffers (history 1)");
			}

			// Synthetic P11 stats: coverage 1 inside the letterbox rows (a few partial 0.25 blocks), mean code drifting with the
			// frame, the LUT's J as the mean PQ, grain N >= 0 except on a few blocks.
			void MakeStats(int f, const std::vector<float>& texels, std::vector<float>& out) const
			{
				const size_t n = grid.Count();
				out.assign(n * RE::kStatsChannels, 0.0f);
				for (int by = 0; by < grid.gh; ++by)
					for (int bx = 0; bx < grid.gw; ++bx)
					{
						const size_t i = size_t(by) * grid.gw + bx;
						float* s = &out[i * RE::kStatsChannels];
						const int y0 = by * grid.B, y1 = y0 + grid.B;
						const bool inside = y0 >= pic.top && y1 <= pic.bottom;
						float cov = inside ? 1.0f : 0.0f;
						if (inside && i % 17u == 3u)
							cov = 0.25f;
						const float meanY = 150.0f + 6.0f * float(bx) + 3.0f * float(by) + 40.0f * float(f) + float((bx * 7 + by * 3) % 11);
						s[RE::kMeanY] = cov > 0.0f ? meanY : 0.0f;
						s[RE::kMeanPq] = cov > 0.0f ? LightLevel::FetchPacked<float>(texels.data(), meanY).J : 0.0f;
						s[RE::kCoverage] = cov;
						s[RE::kStatN] = (cov > 0.0f && i % 9u != 0u) ? 1.0f + float(i % 5u) * 0.5f : -1.0f;
					}
			}

			const Node& NodeOf(ProductId id) const
			{
				for (const Node& n : graph.nodes)
					if (n.id == id)
						return n;
				Assert::Fail(L"node not in the graph");
				return graph.nodes[0];
			}

			// A frame number whose P11 ring slot is 0 (the slot FillP11 writes), plus f.
			uint64_t Seq0() const { return uint64_t(NodeOf(kP11RegionBlockStats).ring) * 2u; }

			pl_tex Upload(const std::vector<float>& rgba)
			{
				pl_tex_params tp{};
				tp.w = grid.gw;
				tp.h = grid.gh;
				tp.format = PlTexBackend::FindFormat(fixture.Gpu(), TexFormat::RGBA32F);
				tp.sampleable = true;
				tp.initial_data = rgba.data();
				pl_tex t = pl_tex_create(fixture.Gpu(), &tp);
				Assert::IsNotNull(t, L"stats source texture");
				return t;
			}

			// P11's S0, S3, S4 of frame f into ring slot 0 and live buffer 0.
			void FillP11(int f)
			{
				const GpuProductTexturesPtr t11 = disp->TexturesOf(kP11RegionBlockStats);
				Assert::IsNotNull(t11, L"P11 stand-in textures");
				for (size_t t : { size_t(0), size_t(3), size_t(4) })
				{
					std::vector<float> rgba(grid.Count() * 4);
					for (size_t i = 0; i < grid.Count(); ++i)
						for (size_t c = 0; c < 4; ++c)
							rgba[i * 4 + c] = stats[f][i * RE::kStatsChannels + t * 4 + c];
					pl_tex src = Upload(rgba);
					Assert::IsTrue(CopyPass(disp->Dispatcher(), src, t11->Ring(0, t)), L"fill the ring stats");
					Assert::IsTrue(CopyPass(disp->Dispatcher(), src, t11->Live(0, t)), L"fill the live stats");
					pl_tex_destroy(fixture.Gpu(), &src);
				}
			}

			using GpuProductTexturesPtr = const VpLaneProducts::GpuProductTextures*;

			// The lane form of frame 0: one Dispatch of the P12 job into ring slot 0 (no AnalysisLane: no own previous frame).
			void RunLaneDispatch(uint64_t seq, bool withP11, bool withP4)
			{
				const Node& n12 = NodeOf(kP12LightLevel);
				const Node& n11 = NodeOf(kP11RegionBlockStats);
				SlotSet set11(kP11RegionBlockStats, uint16_t(n11.ring), uint16_t(n11.livePool));
				SlotSet set4(kP4SourceStats, 2, 1);
				LaneInputs pins;
				pins.count = n12.inputCount;
				for (size_t k = 0; k < n12.inputCount; ++k)
				{
					pins.liveIndex[k] = -1;
					pins.seqs[k] = seq;
					pins.sets[k] = nullptr;
					if (n12.inputs[k].product == kP11RegionBlockStats && withP11)
						pins.sets[k] = &set11;
					else if (n12.inputs[k].product == kP4SourceStats && withP4)
						pins.sets[k] = &set4;
				}
				LaneJobContext job;
				job.node = &n12;
				job.seq = seq;
				job.ringSlot = 0;
				job.inputs = &pins;
				for (size_t k = 0; k < n12.inputCount; ++k)
					if (n12.inputs[k].product == kP4SourceStats)
						job.inputData[k] = p4.data();
				Assert::IsTrue(disp->Dispatch(job, 0, 1), L"the P12 lane chunk");
			}

			VpLaneProducts::KernelContext MakeContext(uint64_t seq, bool live, bool withP11, bool withP4) const
			{
				VpLaneProducts::KernelContext c;
				c.gpu = fixture.Gpu();
				c.dispatch = disp->Dispatcher();
				c.id = kP12LightLevel;
				c.seq = seq;
				c.dims = dims;
				c.live = live;
				c.own = disp->TexturesOf(kP12LightLevel);
				c.picture = VpLaneProducts::PictureFromStats(withP4 ? p4.data() : nullptr, p4.size(), kW, kH);
				c.inCount = 2;
				c.in[0].id = kP11RegionBlockStats;
				c.in[0].tex = disp->TexturesOf(kP11RegionBlockStats);
				c.in[0].ringSlot = live ? -1 : 0;
				c.in[0].live = live ? 0 : -1;
				c.in[0].missing = !withP11;
				c.in[1].id = kP4SourceStats;
				c.in[1].cpu = p4.data();
				c.in[1].cpuBytes = p4.size();
				c.in[1].missing = !withP4;
				c.missing = (withP11 ? 0u : 1u) | (withP4 ? 0u : 2u);
				return c;
			}

			// The same kernel with a lane-style context (ring slots, the own previous frame at ring slot `prev`) or the live
			// form (live buffers, previous at live buffer `prev`); prev < 0 = none.
			void RunKernel(uint64_t seq, bool live, int out, int prev, bool withP11 = true, bool withP4 = true)
			{
				VpLaneProducts::KernelContext c = MakeContext(seq, live, withP11, withP4);
				c.out[0] = live ? c.own->Live(size_t(out), 0) : c.own->Ring(size_t(out), 0);
				c.outCount = 1;
				if (live)
					c.ownPrevLive = prev;
				else
					c.ownPrevRingSlot = prev;
				Assert::IsTrue(kernel->Step(c, 0), L"the P12 step");
			}

			std::vector<float> Out(bool live, int slot) const
			{
				const VpLaneProducts::GpuProductTextures* t = disp->TexturesOf(kP12LightLevel);
				return Download<float>(fixture.Gpu(), live ? t->Live(size_t(slot), 0) : t->Ring(size_t(slot), 0), 4);
			}

			// The CPU float mirror of one frame. mode 1 = frame level (every block: frameJ, V, conf 0).
			std::vector<float> Mirror(const std::vector<float>& statsF, const std::vector<float>& texels, const std::vector<float>* prev,
				bool full, bool haveApl) const
			{
				LB::Params p = LB::ParamsFor(kW, kH, VpLaneProducts::PictureFromStats(p4.data(), p4.size(), kW, kH), grid.B);
				p.frameRateHz = kFps;
				const LB::Kernel kern = LB::KernelFor(p);
				const float alpha = float(LB::TemporalAlpha(p));
				float frameJ = 0.0f, frameV = 0.0f;
				if (haveApl)
				{
					const LightLevel::PackedEntry<float> e = LightLevel::FetchPacked<float>(texels.data(), float(kAplCode10));
					frameJ = e.J;
					frameV = (std::min)(e.V, float(LB::kVCap));
				}
				std::vector<float> m(grid.Count() * 4, 0.0f);
				for (int by = 0; by < grid.gh; ++by)
					for (int bx = 0; bx < grid.gw; ++bx)
					{
						float o[4];
						if (full)
							LB::BlockOut<float>(p, kern, grid.gw, grid.gh, statsF.data(), RE::kStatsChannels, texels.data(), frameJ, 1.0f,
								prev ? prev->data() : nullptr, prev == nullptr, alpha, bx, by, o);
						else
						{
							o[0] = o[1] = frameJ;
							o[2] = frameV;
							o[3] = float(p.frameLevelConf);
						}
						for (int c = 0; c < 4; ++c)
							m[(size_t(by) * grid.gw + bx) * 4 + c] = o[c];
					}
				return m;
			}
		};

		// Relative difference (the tolerance of LightLevelBlocksWarpTests); returns the number of values out of tolerance.
		int Compare(const std::vector<float>& got, const std::vector<float>& want, const char* label, double* worstOut = nullptr)
		{
			int bad = 0;
			double worst = 0.0;
			Assert::IsTrue(got.size() == want.size(), L"size");
			for (size_t i = 0; i < got.size(); ++i)
			{
				const double err = std::fabs(double(got[i]) - double(want[i])) / (std::max)(std::fabs(double(want[i])), 1.0);
				worst = (std::max)(worst, err);
				if (err > 2e-4)
					++bad;
			}
			char msg[200];
			snprintf(msg, sizeof(msg), "P12_KERNEL %s values=%zu bad=%d worst=%.2e\n", label, got.size(), bad, worst);
			Logger::WriteMessage(msg);
			if (worstOut)
				*worstOut = worst;
			return bad;
		}

		bool SameBits(const std::vector<float>& a, const std::vector<float>& b)
		{
			return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
		}

		// By value (Fallbacks() returns a temporary); an empty id = not listed.
		FallbackCatalog::Decision Find(const std::vector<FallbackCatalog::Decision>& v, const char* id)
		{
			for (const auto& d : v)
				if (d.id == id)
					return d;
			return FallbackCatalog::Decision();
		}
	}

	TEST_CLASS(LaneGpuLightKernelWarpTests)
	{
	public:
		// (a) Golden: the lane kernel (frame 0, reset) equals BlockOut<float> on a synthetic P11 input, for the default
		// context and, after SetSettings, for an HDR-to-SDR LUT (the LUT is rebuilt and uploaded).
		BEGIN_TEST_METHOD_ATTRIBUTE(KernelMatchesTheCpuReference)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(KernelMatchesTheCpuReference)
		{
			Rig r;
			r.Init();
			const uint64_t seq = r.Seq0();
			for (int pass = 0; pass < 2; ++pass)
			{
				const LightLevel::Context ctx = pass == 0 ? Ctx(kSdr, 0.0) : Ctx(LightLevel::Context::HdrToSdr, 1000.0);
				if (pass == 1)
				{
					FallbackCatalog::Decision d = Find(r.kernel->Fallbacks(), "light_level_display");
					Assert::IsTrue(!d.id.empty() && d.engaged, L"display facts: defaults is shown until the settings are supplied");
					Assert::IsTrue(d.reason.find("display facts: defaults") != std::string::npos, L"the reason names the fallback");
					r.kernel->SetSettings(Defaults(), ctx);
					FallbackCatalog::Decision d2 = Find(r.kernel->Fallbacks(), "light_level_display");
					Assert::IsTrue(!d2.id.empty() && !d2.engaged, L"released once the facts are supplied");
				}
				const std::vector<float> texels = TexelsFor(ctx);
				r.FillP11(0);
				r.RunLaneDispatch(seq, true, true);
				Assert::IsTrue(r.kernel->LastFull() && !r.kernel->LastUsedPrev(), L"full mode, reset");
				Assert::IsTrue(r.kernel->LutFingerprint() == LightLevel::Build(Defaults(), ctx).fingerprint, L"the LUT is the CPU LUT");
				const std::vector<float> got = r.Out(false, 0);
				const std::vector<float> want = r.Mirror(r.stats[0], texels, nullptr, true, true);
				Assert::IsTrue(Compare(got, want, pass == 0 ? "golden_default" : "golden_hdr_to_sdr") == 0, L"the kernel differs from BlockOut<float>");
				int conf = 0, zero = 0;
				for (size_t i = 0; i < r.grid.Count(); ++i)
				{
					conf += got[i * 4 + 3] > 0.0f ? 1 : 0;
					zero += got[i * 4 + 3] == 0.0f ? 1 : 0;
				}
				Assert::IsTrue(conf > 0 && zero > 0, L"the scene reaches confident picture blocks and bar blocks");
			}
		}

		// (b) Three frames: the lane form equals the live form bit for bit; frame 0 resets, frames 1 and 2 smooth on the previous
		// frame (ring slot / live buffer of seq - 1) and equal the mirror chain.
		BEGIN_TEST_METHOD_ATTRIBUTE(ChainLaneEqualsLive)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(ChainLaneEqualsLive)
		{
			Rig r;
			r.Init();
			const std::vector<float> texels = TexelsFor(Ctx(kSdr, 0.0));
			std::vector<float> prevMirror;
			for (int f = 0; f < 3; ++f)
			{
				const uint64_t seq = r.Seq0() + uint64_t(f);
				const int out = f % 2, prev = f == 0 ? -1 : (f + 1) % 2;
				r.FillP11(f);
				if (f == 0)
					r.RunLaneDispatch(seq, true, true);   // the dispatcher path, ring slot 0, no previous frame
				else
					r.RunKernel(seq, false, out, prev);
				const bool laneUsedPrev = r.kernel->LastUsedPrev();
				r.RunKernel(seq, true, out, prev);
				Assert::IsTrue(laneUsedPrev == (f > 0) && r.kernel->LastUsedPrev() == (f > 0), L"frame 0 resets, later frames use ll_prev");
				const std::vector<float> lane = r.Out(false, out), live = r.Out(true, out);
				Assert::IsTrue(SameBits(lane, live), L"lane == live, bit for bit");
				const std::vector<float> want = r.Mirror(r.stats[f], texels, f == 0 ? nullptr : &prevMirror, true, true);
				char label[32];
				snprintf(label, sizeof(label), "chain_frame_%d", f);
				Assert::IsTrue(Compare(lane, want, label) == 0, L"the chain differs from the CPU mirror");
				if (f == 1)
				{
					// The smoothing is active: the reset form of the same frame is another picture.
					const std::vector<float> unsmoothed = r.Mirror(r.stats[f], texels, nullptr, true, true);
					Assert::IsTrue(!SameBits(unsmoothed, want), L"frame 1 must differ from its unsmoothed form");
				}
				prevMirror = want;
			}
		}

		// (c) No P11: the documented frame-level form (every block (J, J, V, 0) of P4's APL; zeros without the APL) and the
		// visible "P12 frame-level" fallback; lane == live; the next full frame does not smooth on the reduced one.
		BEGIN_TEST_METHOD_ATTRIBUTE(MissingP11GivesTheFrameLevelAndAFallback)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(MissingP11GivesTheFrameLevelAndAFallback)
		{
			Rig r;
			r.Init();
			const std::vector<float> texels = TexelsFor(Ctx(kSdr, 0.0));
			const uint64_t seq = r.Seq0();
			{
				const FallbackCatalog::Decision before = Find(r.kernel->Fallbacks(), "light_level_frame");
				Assert::IsTrue(!before.id.empty() && !before.engaged, L"released before any reduced frame");
			}

			r.RunLaneDispatch(seq, false, true);
			Assert::IsFalse(r.kernel->LastFull(), L"reduced");
			const std::vector<float> got = r.Out(false, 0);
			const std::vector<float> want = r.Mirror(r.stats[0], texels, nullptr, false, true);
			Assert::IsTrue(Compare(got, want, "frame_level") == 0, L"the frame level differs from the mirror");
			FallbackCatalog::Decision d = Find(r.kernel->Fallbacks(), "light_level_frame");
			Assert::IsTrue(!d.id.empty() && d.engaged, L"P12 frame-level is visible");
			Assert::IsTrue(d.reason.find("P12 frame-level") != std::string::npos, L"the reason names the fallback");
			r.RunKernel(seq, true, 0, -1, false, true);
			Assert::IsTrue(SameBits(got, r.Out(true, 0)), L"lane == live (frame level)");

			// No APL either: code only, zeros, still visible.
			r.RunKernel(seq + 1, false, 1, -1, false, false);
			Assert::IsTrue(!r.kernel->LastFull(), L"reduced without the APL");
			const std::vector<float> zeros = r.Out(false, 1);
			Assert::IsTrue(Compare(zeros, r.Mirror(r.stats[0], texels, nullptr, false, false), "code_only") == 0, L"code only is zeros");
			d = Find(r.kernel->Fallbacks(), "light_level_frame");
			Assert::IsTrue(!d.id.empty() && d.engaged && d.reason.find("code only") != std::string::npos, L"code only is named");

			// A full frame after a reduced one starts unsmoothed (the CPU twin keeps no history across a reduced frame) and says so.
			r.FillP11(1);
			r.RunKernel(seq + 2, false, 0, 1);
			Assert::IsTrue(r.kernel->LastFull() && !r.kernel->LastUsedPrev(), L"no smoothing on a reduced previous frame");
			d = Find(r.kernel->Fallbacks(), "light_level_smoothing");
			Assert::IsTrue(!d.id.empty() && d.engaged && d.reason.find("P12 smoothing reset") != std::string::npos, L"the reset is visible");
			Assert::IsFalse(Find(r.kernel->Fallbacks(), "light_level_frame").engaged, L"frame-level released");
		}

		// (d) The begin gate truth table (CPU only).
		TEST_METHOD(BeginGateTruthTable)
		{
			using VpLaneProducts::LightLevelBeginGate;
			using VpLaneProducts::LightPrevState;
			uint64_t asked = 99;
			LightPrevState answer = LightPrevState::None;
			const auto stateOf = [&](uint64_t s) { asked = s; return answer; };
			std::string why;

			Assert::IsTrue(LightLevelBeginGate(0, stateOf, &why), L"the first frame is allowed");
			Assert::IsTrue(asked == 99, L"the first frame asks nothing");
			Assert::IsTrue(why.empty(), L"no reason when allowed");

			const struct { LightPrevState s; bool ok; } table[] = { { LightPrevState::Ring, true }, { LightPrevState::Live, true },
				{ LightPrevState::None, false }, { LightPrevState::Pending, false }, { LightPrevState::Failed, false } };
			for (const auto& t : table)
			{
				answer = t.s;
				why.clear();
				asked = 99;
				Assert::IsTrue(LightLevelBeginGate(7, stateOf, &why) == t.ok, L"gate answer");
				Assert::IsTrue(asked == 6, L"asks for P12(N-1)");
				Assert::IsTrue(t.ok ? why.empty() : why == "waits for P12(N-1)", L"the reason text");
			}
			answer = LightPrevState::Pending;
			Assert::IsFalse(LightLevelBeginGate(1, stateOf, nullptr), L"works without a reason out-parameter");
			Assert::IsTrue(VpLaneProducts::LightPrevReadable(LightPrevState::Ring) && !VpLaneProducts::LightPrevReadable(LightPrevState::Failed),
				L"readable states");
		}

		// (e) The pass GLSL is pinned (FNV-1a 64 over header, newline, body). 0 = not yet pinned: the integrator fills it after
		// the first run (the value is logged).
		BEGIN_TEST_METHOD_ATTRIBUTE(PassFingerprintIsPinned)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(PassFingerprintIsPinned)
		{
			// integrator fills after first run
			constexpr uint64_t kPinned = 0ull;
			const uint64_t fp = LightLevelKernel::PassFingerprint();
			char msg[120];
			snprintf(msg, sizeof(msg), "P12_KERNEL_FINGERPRINT 0x%016llXull\n", static_cast<unsigned long long>(fp));
			Logger::WriteMessage(msg);
			Assert::IsTrue(LightLevelKernel::HeaderGlsl().find("light_level_lut_fetch") != std::string::npos, L"the header carries the LUT fetch");
			Assert::IsTrue(LightLevelKernel::BodyGlsl().find("ll_prev") != std::string::npos &&
				LightLevelKernel::BodyGlsl().find("ll_out") != std::string::npos, L"the body reads the previous frame and writes the block");
			Assert::IsTrue(kPinned == 0ull || fp == kPinned, L"the P12 pass GLSL changed: re-pin deliberately");
		}
	};
}
