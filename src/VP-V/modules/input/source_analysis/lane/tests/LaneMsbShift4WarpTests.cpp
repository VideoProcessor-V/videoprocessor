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
#include <VP-V/modules/input/capture/SourceUnpackGpu.h>
#include <VP-V/modules/input/source_analysis/lane/LaneGpuGrainKernel.h>
#include <VP-V/modules/input/source_analysis/lane/LaneGpuLightKernel.h>
#include <VP-V/modules/input/source_analysis/lane/LaneGpuProducts.h>
#include <VP-V/modules/input/source_analysis/lane/LaneGpuRegionKernel.h>

#include <libplacebo/dispatch.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <utility>
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

		// ---- Slice 21 W21-4 (U#3) P13 GPU tap helpers (RAII, independent HDMI 1.4a 6.5.4 packer) ----

		struct ScopedTex
		{
			pl_gpu gpu = nullptr;
			pl_tex tex = nullptr;
			ScopedTex() = default;
			ScopedTex(pl_gpu g, pl_tex t) : gpu(g), tex(t) {}
			~ScopedTex() { Reset(); }
			ScopedTex(const ScopedTex&) = delete;
			ScopedTex& operator=(const ScopedTex&) = delete;
			void Reset(pl_gpu g = nullptr, pl_tex t = nullptr)
			{
				if (gpu && tex)
					pl_tex_destroy(gpu, &tex);
				gpu = g;
				tex = t;
			}
			pl_tex Get() const { return tex; }
		};

		struct ScopedDispatch
		{
			pl_dispatch d = nullptr;
			explicit ScopedDispatch(pl_gpu gpu) : d(gpu ? pl_dispatch_create(gpu->log, gpu) : nullptr) {}
			~ScopedDispatch() { if (d) pl_dispatch_destroy(&d); }
			ScopedDispatch(const ScopedDispatch&) = delete;
			ScopedDispatch& operator=(const ScopedDispatch&) = delete;
			pl_dispatch Get() const { return d; }
		};

		class TestStaticStore : public VpLaneProducts::IStaticTextures
		{
		public:
			explicit TestStaticStore(pl_gpu gpu) : m_gpu(gpu) {}
			~TestStaticStore() override { Clear(); }
			TestStaticStore(const TestStaticStore&) = delete;
			TestStaticStore& operator=(const TestStaticStore&) = delete;

			pl_tex Rgba32f(const void* owner, int slot, int w, int h, const float* rgba, bool replace) override
			{
				m_error.clear();
				if (!m_gpu) { m_error = "no GPU device"; return nullptr; }
				const std::pair<const void*, int> key = std::make_pair(owner, slot);
				auto it = m_tex.find(key);
				if (it != m_tex.end())
				{
					if (!replace)
						return it->second;
					pl_tex_destroy(m_gpu, &it->second);
					m_tex.erase(it);
				}
				const pl_fmt fmt = PlTexBackend::FindFormat(m_gpu, TexFormat::RGBA32F);
				if (!fmt) { m_error = "no rgba32f format"; return nullptr; }
				pl_tex_params tp{};
				tp.w = w;
				tp.h = h;
				tp.format = fmt;
				tp.sampleable = true;
				tp.initial_data = rgba;
				pl_tex t = pl_tex_create(m_gpu, &tp);
				if (!t) { m_error = "rgba32f create failed"; return nullptr; }
				m_tex[key] = t;
				return t;
			}

			void Forget(const void* owner) override
			{
				for (auto it = m_tex.begin(); it != m_tex.end();)
				{
					if (it->first.first == owner)
					{
						if (m_gpu && it->second)
							pl_tex_destroy(m_gpu, &it->second);
						it = m_tex.erase(it);
					}
					else
						++it;
				}
			}

			std::string LastError() const override { return m_error; }

			void Clear()
			{
				for (auto& kv : m_tex)
					if (m_gpu && kv.second)
						pl_tex_destroy(m_gpu, &kv.second);
				m_tex.clear();
			}

		private:
			pl_gpu m_gpu = nullptr;
			std::map<std::pair<const void*, int>, pl_tex> m_tex;
			std::string m_error;
		};

		struct ScopedKernelRelease
		{
			pl_gpu gpu = nullptr;
			VpLaneProducts::IProductKernel* kernel = nullptr;
			ScopedKernelRelease(pl_gpu g, VpLaneProducts::IProductKernel* k) : gpu(g), kernel(k) {}
			~ScopedKernelRelease() { if (gpu && kernel) kernel->Release(gpu); }
			ScopedKernelRelease(const ScopedKernelRelease&) = delete;
			ScopedKernelRelease& operator=(const ScopedKernelRelease&) = delete;
		};

		// Independent HDMI 1.4a section 6.5.4 DV tunnel word packer (zero self-reference to production unpackers):
		//   G8 = I[11:4], R8 = C[11:4], B8 = (C[3:0] << 4) | I[3:0].
		// Packs non-zero chroma and non-zero unused/padding bits so any bit-mask leakage fails immediately.
		void PackDvTunnelWord(uint16_t intensity12, uint16_t chroma12, bool r210Layout, bool highAligned, bool bigEndian,
			uint32_t junkBits, uint8_t out[4])
		{
			const uint32_t g8 = uint32_t(intensity12 >> 4) & 0xFFu;
			const uint32_t r8 = uint32_t(chroma12 >> 4) & 0xFFu;
			const uint32_t b8 = ((uint32_t(chroma12) & 0xFu) << 4) | (uint32_t(intensity12) & 0xFu);
			const uint32_t jR = junkBits & 3u;
			const uint32_t jG = (junkBits >> 2) & 3u;
			const uint32_t jB = (junkBits >> 4) & 3u;
			const uint32_t jPad = (junkBits >> 6) & 3u;
			const uint32_t r10 = highAligned ? ((r8 << 2) | jR) : (r8 | (jR << 8));
			const uint32_t g10 = highAligned ? ((g8 << 2) | jG) : (g8 | (jG << 8));
			const uint32_t b10 = highAligned ? ((b8 << 2) | jB) : (b8 | (jB << 8));
			const uint32_t word = r210Layout
				? ((jPad << 30) | (r10 << 20) | (g10 << 10) | b10)
				: ((r10 << 22) | (g10 << 12) | (b10 << 2) | jPad);
			if (bigEndian)
			{
				out[0] = uint8_t((word >> 24) & 0xFFu);
				out[1] = uint8_t((word >> 16) & 0xFFu);
				out[2] = uint8_t((word >> 8) & 0xFFu);
				out[3] = uint8_t(word & 0xFFu);
			}
			else
			{
				out[0] = uint8_t(word & 0xFFu);
				out[1] = uint8_t((word >> 8) & 0xFFu);
				out[2] = uint8_t((word >> 16) & 0xFFu);
				out[3] = uint8_t((word >> 24) & 0xFFu);
			}
		}

		std::vector<uint8_t> PackDvTunnelFrame(const std::vector<uint16_t>& codes12, bool r210Layout, bool highAligned, bool bigEndian)
		{
			std::vector<uint8_t> raw(codes12.size() * 4u);
			for (size_t i = 0; i < codes12.size(); ++i)
			{
				const uint16_t chroma12 = uint16_t(((i * 257u) ^ (i >> 3) ^ 0xA5Au) & 0xFFFu);
				const uint32_t junk = uint32_t(((i * 73u) + 0x5Du) & 0xFFu);
				PackDvTunnelWord(codes12[i], chroma12, r210Layout, highAligned, bigEndian, junk, &raw[i * 4u]);
			}
			return raw;
		}

		void UploadRawRgba8(pl_gpu gpu, int w, int h, const std::vector<uint8_t>& raw, ScopedTex& out)
		{
			Assert::AreEqual(size_t(w) * size_t(h) * 4u, raw.size(), L"raw rgba8 byte count");
			const pl_fmt rgba8 = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 8, 8, PL_FMT_CAP_SAMPLEABLE);
			Assert::IsNotNull(rgba8, L"no rgba8 format");
			pl_tex_params rp{};
			rp.w = w;
			rp.h = h;
			rp.format = rgba8;
			rp.sampleable = true;
			rp.host_writable = true;
			pl_tex t = pl_tex_create(gpu, &rp);
			Assert::IsNotNull(t, L"cannot create raw rgba8 texture");
			out.Reset(gpu, t);
			pl_tex_transfer_params up{};
			up.tex = out.Get();
			up.ptr = const_cast<uint8_t*>(raw.data());
			up.row_pitch = size_t(w) * 4u;
			Assert::IsTrue(pl_tex_upload(gpu, &up), L"raw rgba8 upload failed");
		}

		std::vector<uint16_t> TexU16(pl_gpu gpu, pl_tex tex)
		{
			const std::vector<uint8_t> bytes = TexBytes(gpu, tex);
			const size_t n = size_t(tex->params.w) * size_t(tex->params.h);
			Assert::AreEqual(n * 2u, bytes.size(), L"two bytes per R16 texel");
			std::vector<uint16_t> out(n);
			std::memcpy(out.data(), bytes.data(), bytes.size());
			return out;
		}

		void DiffU16(const std::vector<uint16_t>& a, const std::vector<uint16_t>& b, size_t& diffCount, uint32_t& maxDiff)
		{
			Assert::AreEqual(a.size(), b.size(), L"R16 plane size mismatch");
			diffCount = 0;
			maxDiff = 0;
			for (size_t i = 0; i < a.size(); ++i)
			{
				const uint32_t d = a[i] >= b[i] ? uint32_t(a[i] - b[i]) : uint32_t(b[i] - a[i]);
				if (d != 0)
				{
					++diffCount;
					if (d > maxDiff)
						maxDiff = d;
				}
			}
		}

		float MaxFloatAbsDiff(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
		{
			Assert::AreEqual(a.size(), b.size(), L"float texture byte size mismatch");
			Assert::AreEqual(size_t(0), a.size() % sizeof(float), L"float texture alignment");
			const size_t n = a.size() / sizeof(float);
			float maxDiff = 0.0f;
			for (size_t i = 0; i < n; ++i)
			{
				float fa = 0.0f, fb = 0.0f;
				std::memcpy(&fa, a.data() + i * sizeof(float), sizeof(float));
				std::memcpy(&fb, b.data() + i * sizeof(float), sizeof(float));
				const float d = std::fabs(fa - fb);
				if (d > maxDiff)
					maxDiff = d;
			}
			return maxDiff;
		}

		// Synthesizes a realistic 12-bit DV PQ frame with letterbox bars at studio black (256) and active picture
		// carrying ramps, grain-like noise, and active low 2 bits I[1:0] (sub-10-bit Q6 fractional bits).
		std::vector<uint16_t> DvStudioFrame12(int w, int h, const RegionEvidence::Rect& pic, uint32_t seed)
		{
			std::vector<uint16_t> v(size_t(w) * size_t(h), 256u);
			for (int y = pic.top; y < pic.bottom; ++y)
			{
				for (int x = pic.left; x < pic.right; ++x)
				{
					seed = seed * 1664525u + 1013904223u;
					const int base = 520 + (x * 2200) / w + ((y - pic.top) * 520) / (pic.bottom - pic.top);
					const int grain = int((seed >> 16) % 81u) - 40;
					const int sub12 = int(((x + y) & 3) == 0 ? 1 : (((x * 3 + y) & 3) == 0 ? 2 : 3));
					int code = ((base + grain) & ~3) | sub12;
					if (code < 260) code = 263;
					if (code > 3755) code = 3755;
					v[size_t(y) * size_t(w) + size_t(x)] = uint16_t(code);
				}
			}
			return v;
		}

		struct FullLaneRun
		{
			std::vector<uint8_t> extended;
			std::vector<std::vector<uint8_t>> p10;   // Compact: split, splitB, blocks
			std::vector<std::vector<uint8_t>> p11;   // S0..S5
			std::vector<uint8_t> p12;                // (J_surround, J_local, V, conf)
		};

		// Runs ExtendTap + real P10 (GrainSplitKernel Compact) -> P11 (RegionStatsKernel) -> P12 (LightLevelKernel)
		// on a supplied tap plane and downloads every output texture.
		FullLaneRun RunFullLaneKernelsOnTap(pl_gpu gpu, pl_dispatch dispatch, Dims d, pl_tex tap,
			const RegionEvidence::Rect& pic, uint64_t seq)
		{
			const Registry reg = VpLaneProducts::TapFormProducts(SkeletonProducts(true));
			ConsumerDecl c;
			c.id = 1;
			AddConsume(c, kP12LightLevel);
			GraphSettings gs;
			gs.dims = d;
			gs.queueDepth = 4;
			gs.laneLead = 1;
			const Graph graph = BuildGraph(reg, std::vector<ConsumerDecl>{ c }, gs);
			TierPlan plan;
			for (ProductId id : { kP10GrainSplit, kP11RegionBlockStats, kP12LightLevel })
			{
				TierEntry e;
				e.id = id;
				e.backend = Backend::GpuCompute;
				e.grant = Grant::Lane;
				plan.entries.push_back(e);
			}
			const Node* nodeP10 = nullptr;
			const Node* nodeP11 = nullptr;
			const Node* nodeP12 = nullptr;
			for (const Node& n : graph.nodes)
			{
				if (n.id == kP10GrainSplit) nodeP10 = &n;
				else if (n.id == kP11RegionBlockStats) nodeP11 = &n;
				else if (n.id == kP12LightLevel) nodeP12 = &n;
			}
			Assert::IsNotNull(nodeP10, L"P10 node missing from graph");
			Assert::IsNotNull(nodeP11, L"P11 node missing from graph");
			Assert::IsNotNull(nodeP12, L"P12 node missing from graph");

			ScopedTex extended(gpu, MakeTapTex(gpu, int(d.width), int(d.height)));
			Assert::IsTrue(VpLaneProducts::ExtendTap(dispatch, tap, extended.Get(), pic), L"ExtendTap failed");

			TestStaticStore store(gpu);
			VpLaneProducts::GpuTexBackend backend(gpu, true);
			VpLaneProducts::GpuProductTextures p10Tex(backend), p11Tex(backend), p12Tex(backend);

			VpLaneProducts::GrainSplitKernel grain(GrainSplit::Storage::Compact);
			ScopedKernelRelease releaseGrain(gpu, &grain);
			Assert::IsTrue(grain.Granted() == GrainSplit::Storage::Compact, L"P10 Compact storage must be granted on R16");

			VpLaneProducts::RegionStatsKernel region;
			region.SetStaticTextures(&store);
			region.SetDisplayFacts(1, 100.0, RegionEvidence::VisibilityParams());   // PQ transfer for DV 12-bit
			ScopedKernelRelease releaseRegion(gpu, &region);

			VpLaneProducts::LightLevelKernel light;
			light.SetStaticTextures(&store);
			light.SetFrameRate(24.0);
			LightLevel::Context llCtx;
			llCtx.kind = LightLevel::Context::HdrPassthrough;
			llCtx.sourcePeakNits = 1000.0;
			light.SetSettings(LightLevel::Read([](const std::string&, std::string&) { return false; }), llCtx);
			ScopedKernelRelease releaseLight(gpu, &light);

			const ProductTexLayout l10 = grain.Layout(d);
			const ProductTexLayout l11 = region.Layout(d);
			const ProductTexLayout l12 = light.Layout(d);
			Assert::IsTrue(p10Tex.Allocate(*nodeP10, reg.Get(kP10GrainSplit), l10, d, plan), L"P10 texture allocate failed");
			Assert::IsTrue(p11Tex.Allocate(*nodeP11, reg.Get(kP11RegionBlockStats), l11, d, plan), L"P11 texture allocate failed");
			Assert::IsTrue(p12Tex.Allocate(*nodeP12, reg.Get(kP12LightLevel), l12, d, plan), L"P12 texture allocate failed");

			Assert::IsTrue(grain.Prepare(gpu, dispatch, d), L"P10 Prepare failed");
			Assert::IsTrue(region.Prepare(gpu, dispatch, d), L"P11 Prepare failed");
			Assert::IsTrue(light.Prepare(gpu, dispatch, d), L"P12 Prepare failed");

			// 1. P10: GrainSplitKernel (7 steps)
			VpLaneProducts::KernelContext c10;
			c10.gpu = gpu;
			c10.dispatch = dispatch;
			c10.id = kP10GrainSplit;
			c10.seq = seq;
			c10.dims = d;
			c10.tap = tap;
			c10.extended = extended.Get();
			c10.picture = pic;
			c10.outCount = l10.terms.size();
			for (size_t t = 0; t < l10.terms.size(); ++t)
				c10.out[t] = p10Tex.Ring(0, t);
			for (uint32_t s = 0; s < grain.Steps(d); ++s)
				Assert::IsTrue(grain.Step(c10, s), L"P10 Step failed");

			// 2. P11: RegionStatsKernel (reads extended tap + P10 Compact blocks)
			VpLaneProducts::KernelContext c11;
			c11.gpu = gpu;
			c11.dispatch = dispatch;
			c11.id = kP11RegionBlockStats;
			c11.seq = seq;
			c11.dims = d;
			c11.tap = tap;
			c11.extended = extended.Get();
			c11.picture = pic;
			c11.outCount = l11.terms.size();
			for (size_t t = 0; t < l11.terms.size(); ++t)
				c11.out[t] = p11Tex.Ring(0, t);
			c11.in[0].id = kP10GrainSplit;
			c11.in[0].tex = &p10Tex;
			c11.in[0].ringSlot = 0;
			c11.in[0].missing = false;
			c11.inCount = 1;
			Assert::IsTrue(region.Step(c11, 0), L"P11 Step failed");
			Assert::IsTrue(region.BandsUsed(), L"P11 must consume P10 Compact bands (not dummy fallback)");

			// 3. P12: LightLevelKernel (reads P11 S0/S3/S4 in full per-block mode)
			VpLaneProducts::KernelContext c12;
			c12.gpu = gpu;
			c12.dispatch = dispatch;
			c12.id = kP12LightLevel;
			c12.seq = seq;
			c12.dims = d;
			c12.picture = pic;
			c12.outCount = 1;
			c12.out[0] = p12Tex.Ring(0, 0);
			c12.own = &p12Tex;
			c12.in[0].id = kP11RegionBlockStats;
			c12.in[0].tex = &p11Tex;
			c12.in[0].ringSlot = 0;
			c12.in[0].missing = false;
			c12.inCount = 1;
			Assert::IsTrue(light.Step(c12, 0), L"P12 Step failed");
			Assert::IsTrue(light.LastFull(), L"P12 must run full per-block mode from P11 (not frame-level fallback)");

			FullLaneRun out;
			out.extended = TexBytes(gpu, extended.Get());
			for (size_t t = 0; t < l10.terms.size(); ++t)
				out.p10.push_back(TexBytes(gpu, p10Tex.Ring(0, t)));
			for (size_t t = 0; t < l11.terms.size(); ++t)
				out.p11.push_back(TexBytes(gpu, p11Tex.Ring(0, t)));
			out.p12 = TexBytes(gpu, p12Tex.Ring(0, 0));
			return out;
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
		BEGIN_TEST_METHOD_ATTRIBUTE(GrainKernelSameOnMsbShift4AndShift6)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(GrainKernelSameOnMsbShift4AndShift6)
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

		// Slice 21 W21-4 (U#3): BuildDvTapFromP13Raw unpacks raw DV tunnel rgba8 pixels (big-endian r210/R10b and
		// little-endian R10l, high/low RGB8 alignment) directly on GPU into R16 unorm (code12 << 4 == Q6) with 0 texel
		// difference against UploadTapRows(msbShift=4) across all 4096 12-bit codes (including every I[3:0] nibble in B8),
		// BuildTapFromP13Luma matches from real SourceUnpackGpu P13 R16 luma, StepsForP13Tap saves CPU band steps, and
		// in-test negative controls (wrong endianness, wrong bitShift, dropped I[3:0] nibble) measurably fail.
		BEGIN_TEST_METHOD_ATTRIBUTE(P13TapEqualsCpuTap12)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(P13TapEqualsCpuTap12)
		{
			// 1. StepsForP13Tap contract: 1 GPU tap pass + 1 extend pass + kP10SplitterSteps (fewer than CPU bands at UHD).
			const Dims smallDims{ 128u, 96u };
			const Dims uhdDims{ 3840u, 2160u };
			Assert::AreEqual(1u + 1u + VpLaneProducts::kP10SplitterSteps,
				VpLaneProducts::StepsForP13Tap(kP10GrainSplit, smallDims), L"StepsForP13Tap P10 small");
			Assert::AreEqual(1u + 1u + VpLaneProducts::kP10SplitterSteps,
				VpLaneProducts::StepsForP13Tap(kP10GrainSplit, uhdDims), L"StepsForP13Tap P10 UHD");
			Assert::AreEqual(1u, VpLaneProducts::StepsForP13Tap(kP11RegionBlockStats, uhdDims), L"StepsForP13Tap P11");
			Assert::AreEqual(1u, VpLaneProducts::StepsForP13Tap(kP12LightLevel, uhdDims), L"StepsForP13Tap P12");
			Assert::AreEqual(0u, VpLaneProducts::StepsForP13Tap(kP10GrainSplit, Dims{ 0u, 0u }), L"StepsForP13Tap empty");
			Assert::IsTrue(VpLaneProducts::TapBands(uhdDims) > 1u &&
				VpLaneProducts::StepsForP13Tap(kP10GrainSplit, uhdDims) < VpLaneProducts::StepsFor(kP10GrainSplit, uhdDims),
				L"P13 GPU tap replaces multi-band CPU upload with 1 GPU tap pass at UHD");

			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			ScopedDispatch dispatch(gpu);
			Assert::IsNotNull(dispatch.Get(), L"pl_dispatch_create failed");

			// 2. Synthesize 128x96 (12,288 pixels) spanning ALL 4096 12-bit intensity codes I[11:0] (every I[11:4] and I[3:0]).
			const int w = 128, h = 96;
			std::vector<uint16_t> codes12(size_t(w) * size_t(h));
			for (size_t i = 0; i < codes12.size(); ++i)
			{
				if (i < 4096u)
					codes12[i] = uint16_t(i);   // exhaustive 0..4095 coverage (every high byte & low nibble)
				else
					codes12[i] = uint16_t(((i * 37u) ^ (i >> 4)) & 0xFFFu);
			}

			// 3. CPU reference tap via UploadTapRows(msbShift=4) + independent ground-truth check (code12 << 4).
			ScopedTex tapCpu(gpu, MakeTapTex(gpu, w, h));
			std::vector<uint16_t> scratch;
			Assert::IsTrue(VpLaneProducts::UploadTapRows(gpu, tapCpu.Get(), ViewOf(codes12, w, h, 1, 4), 0, uint32_t(h), scratch),
				L"CPU 12-bit tap upload failed");
			const std::vector<uint16_t> cpuSamples = TexU16(gpu, tapCpu.Get());
			for (size_t i = 0; i < codes12.size(); ++i)
				Assert::AreEqual(uint32_t(uint16_t(codes12[i] << 4)), uint32_t(cpuSamples[i]), L"CPU tap must hold code12 << 4");

			// 4. Verify BuildDvTapFromP13Raw across r210/R10b (big-endian) and R10l (little-endian), high & low RGB8 alignment.
			struct DvCase
			{
				const wchar_t* name;
				VideoFrameEncoding encoding;
				bool r210Layout;
				bool highAligned;
				bool bigEndian;
				uint32_t bitShift;
			};
			const DvCase cases[] = {
				{ L"r210 BE high", VideoFrameEncoding::R210, true,  true,  true,  2u },
				{ L"r210 BE low",  VideoFrameEncoding::R210, true,  false, true,  0u },
				{ L"R10b BE high", VideoFrameEncoding::R10b, false, true,  true,  4u },
				{ L"R10l LE high", VideoFrameEncoding::R10l, false, true,  false, 4u },
				{ L"R10l LE low",  VideoFrameEncoding::R10l, false, false, false, 2u },
			};

			ScopedTex tapGpu(gpu, MakeTapTex(gpu, w, h));
			ScopedTex tapFromLuma(gpu, MakeTapTex(gpu, w, h));
			ScopedTex tapNeg(gpu, MakeTapTex(gpu, w, h));
			SourceUnpack::SourceUnpackGpu unpack(fixture.Log(), gpu);
			unpack.SetHostReadableForTest(true);

			for (const DvCase& c : cases)
			{
				const std::vector<uint8_t> raw = PackDvTunnelFrame(codes12, c.r210Layout, c.highAligned, c.bigEndian);
				ScopedTex rawTex;
				UploadRawRgba8(gpu, w, h, raw, rawTex);

				Assert::IsTrue(VpLaneProducts::BuildDvTapFromP13Raw(dispatch.Get(), rawTex.Get(), tapGpu.Get(),
					c.bigEndian, c.bitShift), c.name);
				const std::vector<uint16_t> gpuSamples = TexU16(gpu, tapGpu.Get());
				size_t diffCount = 0;
				uint32_t maxDiff = 0;
				DiffU16(gpuSamples, cpuSamples, diffCount, maxDiff);
				Assert::AreEqual(size_t(0), diffCount, c.name);
				Assert::AreEqual(uint32_t(0), maxDiff, c.name);

				// Also verify BuildTapFromP13Luma fed by real upstream SourceUnpackGpu P13 R16 luma (code12 << 4 == Q6).
				if (c.encoding == VideoFrameEncoding::R10b || (c.encoding == VideoFrameEncoding::R10l && c.highAligned))
				{
					SourceUnpack::Request req;
					req.packing = SourceUnpack::Packing::DvTunnel;
					req.width = w;
					req.height = h;
					req.rowBytes = size_t(w) * 4u;
					req.container = c.encoding;
					req.rgbBitAlignment = c.highAligned ? 1 : 2;
					req.fullRange = false;
					pl_tex p13LumaRaw = nullptr;
					const bool unpackOk = unpack.Unpack(req, nullptr, rawTex.Get(), &p13LumaRaw);
					ScopedTex p13Luma(gpu, p13LumaRaw);
					Assert::IsTrue(unpackOk, L"SourceUnpackGpu DvTunnel unpack failed");

					Assert::IsTrue(VpLaneProducts::BuildTapFromP13Luma(dispatch.Get(), p13Luma.Get(), tapFromLuma.Get(), 0u),
						L"BuildTapFromP13Luma failed");
					const std::vector<uint16_t> lumaTapSamples = TexU16(gpu, tapFromLuma.Get());
					size_t lumaDiff = 0;
					uint32_t lumaMaxDiff = 0;
					DiffU16(lumaTapSamples, cpuSamples, lumaDiff, lumaMaxDiff);
					Assert::AreEqual(size_t(0), lumaDiff, L"BuildTapFromP13Luma must have 0 texel diff vs CPU tap");
					Assert::AreEqual(uint32_t(0), lumaMaxDiff, L"BuildTapFromP13Luma maxDiff must be 0");
				}

				// Mandatory in-test negative control A (exact same DV tunnel words): flipped endianness MUST fail parity.
				Assert::IsTrue(VpLaneProducts::BuildDvTapFromP13Raw(dispatch.Get(), rawTex.Get(), tapNeg.Get(),
					!c.bigEndian, c.bitShift), L"wrong-endian negative control dispatch");
				const std::vector<uint16_t> wrongEndianSamples = TexU16(gpu, tapNeg.Get());
				size_t wrongEndianDiff = 0;
				uint32_t wrongEndianMax = 0;
				DiffU16(wrongEndianSamples, cpuSamples, wrongEndianDiff, wrongEndianMax);
				Assert::IsTrue(wrongEndianDiff > codes12.size() / 2u && wrongEndianMax > 0u,
					L"Negative control: flipped endianness on exact same DV tunnel words must differ on most texels");
			}

			// Mandatory in-test negative control B (exact same frame): wrong bitShift on R10l high-aligned MUST fail parity.
			{
				const std::vector<uint8_t> rawR10l = PackDvTunnelFrame(codes12, false, true, false);
				ScopedTex rawTex;
				UploadRawRgba8(gpu, w, h, rawR10l, rawTex);
				Assert::IsTrue(VpLaneProducts::BuildDvTapFromP13Raw(dispatch.Get(), rawTex.Get(), tapGpu.Get(), false, 4u),
					L"R10l exact dispatch");
				const std::vector<uint16_t> exactSamples = TexU16(gpu, tapGpu.Get());

				Assert::IsTrue(VpLaneProducts::BuildDvTapFromP13Raw(dispatch.Get(), rawTex.Get(), tapNeg.Get(), false, 2u),
					L"wrong-bitShift negative control dispatch");
				const std::vector<uint16_t> wrongShiftSamples = TexU16(gpu, tapNeg.Get());
				size_t shiftDiff = 0;
				uint32_t shiftMax = 0;
				DiffU16(wrongShiftSamples, exactSamples, shiftDiff, shiftMax);
				Assert::IsTrue(shiftDiff > codes12.size() / 2u && shiftMax > 0u,
					L"Negative control: wrong bitShift on exact same R10l words must differ on most texels");

				// Mandatory in-test negative control C: dropping low 4-bit nibble I[3:0] (B8[3:0]) MUST fail parity.
				std::vector<uint16_t> lossyNoLowNibble(codes12.size());
				for (size_t i = 0; i < codes12.size(); ++i)
					lossyNoLowNibble[i] = uint16_t(codes12[i] & 0xFFF0u);
				Assert::IsTrue(VpLaneProducts::UploadTapRows(gpu, tapNeg.Get(), ViewOf(lossyNoLowNibble, w, h, 2, 4),
					0, uint32_t(h), scratch), L"lossy nibble upload");
				const std::vector<uint16_t> lossySamples = TexU16(gpu, tapNeg.Get());
				size_t lossyDiff = 0;
				uint32_t lossyMax = 0;
				DiffU16(lossySamples, exactSamples, lossyDiff, lossyMax);
				Assert::IsTrue(lossyDiff > codes12.size() / 2u,
					L"Negative control: dropping I[3:0] nibble in B8 must differ on most texels");
				Assert::AreEqual(uint32_t(0xFu << 4), lossyMax,
					L"Negative control: dropping I[3:0] must show exact max diff 15 << 4 (240)");
			}

			// 5. Verify BuildTapFromP13Luma left-shift path (msbShift = 4 on unshifted 12-bit R16) + negative control (msbShift = 0).
			{
				ScopedTex unshifted12(gpu, MakeTapTex(gpu, w, h));
				Assert::IsTrue(VpLaneProducts::UploadTapRows(gpu, unshifted12.Get(), ViewOf(codes12, w, h, 3, 0),
					0, uint32_t(h), scratch), L"unshifted 12-bit upload");
				Assert::IsTrue(VpLaneProducts::BuildTapFromP13Luma(dispatch.Get(), unshifted12.Get(), tapFromLuma.Get(), 4u),
					L"BuildTapFromP13Luma msbShift=4");
				const std::vector<uint16_t> shiftedSamples = TexU16(gpu, tapFromLuma.Get());
				size_t shiftDiff = 0;
				uint32_t shiftMax = 0;
				DiffU16(shiftedSamples, cpuSamples, shiftDiff, shiftMax);
				Assert::AreEqual(size_t(0), shiftDiff, L"BuildTapFromP13Luma msbShift=4 must equal CPU tap exactly");
				Assert::AreEqual(uint32_t(0), shiftMax, L"BuildTapFromP13Luma msbShift=4 maxDiff must be 0");

				Assert::IsTrue(VpLaneProducts::BuildTapFromP13Luma(dispatch.Get(), unshifted12.Get(), tapNeg.Get(), 0u),
					L"BuildTapFromP13Luma negative control msbShift=0 on unshifted 12-bit");
				const std::vector<uint16_t> unshiftedOut = TexU16(gpu, tapNeg.Get());
				size_t negShiftDiff = 0;
				uint32_t negShiftMax = 0;
				DiffU16(unshiftedOut, cpuSamples, negShiftDiff, negShiftMax);
				Assert::IsTrue(negShiftDiff > codes12.size() / 2u && negShiftMax > 0u,
					L"Negative control: omitting msbShift on unshifted 12-bit R16 must differ on most texels");
			}
		}

		// Slice 21 W21-4 (U#3): ExtendTap + real P10 (GrainSplitKernel Compact) + P11 (RegionStatsKernel) +
		// P12 (LightLevelKernel) produce BIT-EXACT (0-diff) outputs when fed by GPU P13 tap (BuildDvTapFromP13Raw) vs
		// CPU-uploaded tap (UploadTapRows, msbShift=4) on the exact same 12-bit DV PQ frame; negative control dropping
		// the low 2 bits of 12-bit I measurably alters every kernel's output.
		BEGIN_TEST_METHOD_ATTRIBUTE(LaneKernelsSameOnP13AndCpuTap)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(LaneKernelsSameOnP13AndCpuTap)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			ScopedDispatch dispatch(gpu);
			Assert::IsNotNull(dispatch.Get(), L"pl_dispatch_create failed");

			const Dims d{ 320u, 768u };
			const int w = int(d.width), h = int(d.height);
			RegionEvidence::Rect pic;
			pic.left = 0;
			pic.top = 64;
			pic.right = w;
			pic.bottom = h - 64;

			const std::vector<uint16_t> codes12 = DvStudioFrame12(w, h, pic, 2026u);
			const std::vector<uint8_t> rawR10l = PackDvTunnelFrame(codes12, false, true, false);
			ScopedTex rawTex;
			UploadRawRgba8(gpu, w, h, rawR10l, rawTex);

			// Run A: CPU-uploaded tap (UploadTapRows, msbShift = 4).
			ScopedTex tapCpu(gpu, MakeTapTex(gpu, w, h));
			std::vector<uint16_t> scratch;
			Assert::IsTrue(VpLaneProducts::UploadTapRows(gpu, tapCpu.Get(), ViewOf(codes12, w, h, 1, 4), 0, uint32_t(h), scratch),
				L"Run A: CPU tap upload failed");
			const FullLaneRun runCpu = RunFullLaneKernelsOnTap(gpu, dispatch.Get(), d, tapCpu.Get(), pic, 101u);

			// Run B: GPU P13 tap directly from raw DV tunnel rgba8 (BuildDvTapFromP13Raw, zero CPU upload of tap).
			ScopedTex tapGpu(gpu, MakeTapTex(gpu, w, h));
			Assert::IsTrue(VpLaneProducts::BuildDvTapFromP13Raw(dispatch.Get(), rawTex.Get(), tapGpu.Get(), false, 4u),
				L"Run B: BuildDvTapFromP13Raw failed");
			const FullLaneRun runGpu = RunFullLaneKernelsOnTap(gpu, dispatch.Get(), d, tapGpu.Get(), pic, 102u);

			// Assert BIT-EXACT / 0-DIFF PARITY across ExtendTap + P10 + P11 + P12.
			Assert::IsTrue(!runCpu.extended.empty() && runCpu.extended.size() == runGpu.extended.size() &&
				std::memcmp(runCpu.extended.data(), runGpu.extended.data(), runCpu.extended.size()) == 0,
				L"ExtendTap output must be bit-exact between CPU tap and P13 GPU tap");

			Assert::AreEqual(size_t(3), runCpu.p10.size(), L"P10 Compact has 3 terms");
			Assert::AreEqual(runCpu.p10.size(), runGpu.p10.size(), L"P10 term count parity");
			for (size_t t = 0; t < runCpu.p10.size(); ++t)
				Assert::IsTrue(!runCpu.p10[t].empty() && runCpu.p10[t].size() == runGpu.p10[t].size() &&
					std::memcmp(runCpu.p10[t].data(), runGpu.p10[t].data(), runCpu.p10[t].size()) == 0,
					L"P10 GrainSplitKernel output must be bit-exact between CPU tap and P13 GPU tap");

			Assert::AreEqual(size_t(6), runCpu.p11.size(), L"P11 has 6 terms (S0..S5)");
			Assert::AreEqual(runCpu.p11.size(), runGpu.p11.size(), L"P11 term count parity");
			for (size_t t = 0; t < runCpu.p11.size(); ++t)
				Assert::IsTrue(!runCpu.p11[t].empty() && runCpu.p11[t].size() == runGpu.p11[t].size() &&
					std::memcmp(runCpu.p11[t].data(), runGpu.p11[t].data(), runCpu.p11[t].size()) == 0,
					L"P11 RegionStatsKernel output must be bit-exact between CPU tap and P13 GPU tap");

			Assert::IsTrue(!runCpu.p12.empty() && runCpu.p12.size() == runGpu.p12.size() &&
				std::memcmp(runCpu.p12.data(), runGpu.p12.data(), runCpu.p12.size()) == 0,
				L"P12 LightLevelKernel output must be bit-exact between CPU tap and P13 GPU tap");

			// Non-triviality check: outputs actually vary across space (not comparing blank planes).
			bool p10Varied = false;
			for (size_t i = 1; i < runGpu.p10[0].size() && !p10Varied; ++i)
				p10Varied = runGpu.p10[0][i] != runGpu.p10[0][0];
			Assert::IsTrue(p10Varied, L"P10 output must be non-blank and spatially varying");

			// Mandatory in-test negative control (exact same kernels): drop low 2 bits of 12-bit I (10-bit truncated tap)
			// and prove EVERY kernel stage (ExtendTap, P10, P11, P12) measurably differs.
			std::vector<uint16_t> codes10Trunc(codes12.size());
			for (size_t i = 0; i < codes12.size(); ++i)
				codes10Trunc[i] = uint16_t(codes12[i] & 0xFFFCu);
			ScopedTex tapTrunc(gpu, MakeTapTex(gpu, w, h));
			Assert::IsTrue(VpLaneProducts::UploadTapRows(gpu, tapTrunc.Get(), ViewOf(codes10Trunc, w, h, 3, 4),
				0, uint32_t(h), scratch), L"Negative control: 10-bit truncated tap upload failed");
			const FullLaneRun runTrunc = RunFullLaneKernelsOnTap(gpu, dispatch.Get(), d, tapTrunc.Get(), pic, 103u);

			Assert::IsTrue(std::memcmp(runTrunc.extended.data(), runGpu.extended.data(), runGpu.extended.size()) != 0,
				L"Negative control: 10-bit-truncated tap must change ExtendTap output");
			Assert::IsTrue(std::memcmp(runTrunc.p10[0].data(), runGpu.p10[0].data(), runGpu.p10[0].size()) != 0 &&
				MaxFloatAbsDiff(runTrunc.p10[2], runGpu.p10[2]) > 0.0f,
				L"Negative control: dropping low 2 bits of 12-bit I must measurably change P10 GrainSplitKernel output");
			Assert::IsTrue(MaxFloatAbsDiff(runTrunc.p11[0], runGpu.p11[0]) > 0.0f &&
				MaxFloatAbsDiff(runTrunc.p11[3], runGpu.p11[3]) > 0.0f,
				L"Negative control: dropping low 2 bits of 12-bit I must measurably change P11 RegionStatsKernel S0 & S3");
			Assert::IsTrue(MaxFloatAbsDiff(runTrunc.p12, runGpu.p12) > 0.0f,
				L"Negative control: dropping low 2 bits of 12-bit I must measurably change P12 LightLevelKernel output");
		}
	};
}
