// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// The grain split as ONE dependency unit (GrainSplit::kUnitVersion, GrainSplitGpu.h, GrainNodeGpu.h) on the libplacebo
// test device (WARP unless VP_TEST_GPU=hardware): version and source-frame stamps and the stale check, the Compact refusal
// rules (more than 12 bits of codes, float planes), the pinned Full shader bodies, and the single compute path shared by
// the lane producer and the render's live fill.
#include "pch.h"
#include "CppUnitTest.h"
#include "ExtendedRangeGpuProbe.h"

#include <VP-V/modules/repair/grain_split/GpuPassUtil.h>
#include <VP-V/modules/repair/grain_split/GrainNodeGpu.h>
#include <VP-V/modules/repair/grain_split/GrainSplit.h>
#include <VP-V/modules/repair/grain_split/GrainSplitGpu.h>

#include <libplacebo/d3d11.h>
#include <libplacebo/dispatch.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace GrainUnitGpuTestsNs
{
	using GrainSplit::Storage;

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
			log = pl_log_create(PL_API_VER, &lp);
			std::string name;
			const pl_d3d11_params params = ExtendedRangeGpuProbe::TestDeviceParams(&name);
			Logger::WriteMessage(("grain unit GPU tests on: " + name).c_str());
			d3d11 = pl_d3d11_create(log, &params);
			if (!d3d11) return false;
			dispatch = pl_dispatch_create(log, d3d11->gpu);
			return dispatch != nullptr;
		}
		pl_gpu Gpu() const { return d3d11->gpu; }
	};

	std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }

	// Grain on a gradient, in codes (10-bit scale), as unsigned 16-bit plane values code * scale16.
	std::vector<double> MakeCodes(int w, int h, uint32_t seed)
	{
		std::vector<double> v(static_cast<size_t>(w) * h);
		std::mt19937 g(seed);
		std::normal_distribution<double> noise(0.0, 2.0);
		for (int y = 0; y < h; ++y)
			for (int x = 0; x < w; ++x)
				v[static_cast<size_t>(y) * w + x] = (std::min)(1000.0, (std::max)(30.0, 200.0 + 2.0 * x + 1.5 * y + noise(g)));
		return v;
	}

	// codes (10-bit scale) -> an r16 plane whose value is code * codesToPlane / 65535.
	pl_tex UploadR16(pl_gpu gpu, const std::vector<double>& codes, int w, int h, double codesToPlane)
	{
		std::vector<uint16_t> data(codes.size());
		for (size_t i = 0; i < data.size(); ++i)
			data[i] = static_cast<uint16_t>((std::min)(65535L, (std::max)(0L, std::lround(codes[i] * codesToPlane))));
		pl_tex_params p{};
		p.w = w; p.h = h; p.format = pl_find_named_fmt(gpu, "r16"); p.sampleable = true; p.initial_data = data.data();
		return pl_tex_create(gpu, &p);
	}

	pl_tex UploadR32F(pl_gpu gpu, const std::vector<float>& values, int w, int h)
	{
		pl_tex_params p{};
		p.w = w; p.h = h; p.format = pl_find_named_fmt(gpu, "r32f"); p.sampleable = true; p.initial_data = values.data();
		return pl_tex_create(gpu, &p);
	}

	// Any sampleable texture as four floats per texel (a compute pass copies it into a host-readable rgba32f texture).
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

	bool SameBits(const std::vector<float>& x, const std::vector<float>& y)
	{
		return !x.empty() && x.size() == y.size() && std::memcmp(x.data(), y.data(), x.size() * sizeof(float)) == 0;
	}

	constexpr int kW = 256, kH = 144, kB = 16;
	constexpr double kCu10in16 = 64.0 / 65535.0;   // 10-bit codes in a 16-bit plane
}

namespace GrainUnitGpuTestsNs
{
	TEST_CLASS(GrainUnitGpuTests)
	{
	public:
		TEST_METHOD(GrainUnitStampsVersionAndSourceFrame)
		{
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = dev.Gpu();
			pl_tex plane = UploadR16(gpu, MakeCodes(kW, kH, 5u), kW, kH, 64.0);
			GrainSplit::Gpu::Splitter splitter;
			GrainSplit::Gpu::Options o;
			o.seq = 7;
			Assert::IsTrue(splitter.Compute(gpu, dev.dispatch, plane, 0, kCu10in16, 1.0, kB, GrainSplit::Params(), o), Wide(splitter.LastError()).c_str());
			const GrainSplit::Gpu::Frame f = splitter.Out();
			Assert::AreEqual(uint64_t(7), f.seq, L"the frame carries its source frame");
			Assert::AreEqual(GrainSplit::kUnitVersion, f.version, L"the frame carries the unit version");
			using GrainSplit::Gpu::CheckFit;
			using GrainSplit::Gpu::FitResult;
			Assert::IsTrue(CheckFit(&f, kW, kH, kB, 1.0, kCu10in16, 0, 7) == FitResult::Fits, L"fits the right frame");
			Assert::IsTrue(CheckFit(&f, kW, kH, kB, 1.0, kCu10in16, 0) == FitResult::Fits, L"kAnySeq: not checked");
			Assert::IsTrue(CheckFit(&f, kW, kH, kB, 1.0, kCu10in16, 0, 8) == FitResult::Stale, L"another frame is stale");
			GrainSplit::Gpu::Frame old = f;
			old.version = GrainSplit::kUnitVersion + 1;
			Assert::IsTrue(CheckFit(&old, kW, kH, kB, 1.0, kCu10in16, 0, 7) == FitResult::Stale, L"another unit version is stale");
			old.version = 0;
			Assert::IsTrue(CheckFit(&old, kW, kH, kB, 1.0, kCu10in16, 0) == FitResult::Stale, L"a frame not made by the unit is stale");
			Assert::IsTrue(CheckFit(&f, kW + 1, kH, kB, 1.0, kCu10in16, 0, 7) == FitResult::WrongSize, L"another geometry is a wrong size");
			Assert::IsTrue(CheckFit(&f, kW, kH, kB, 1.0, kCu10in16, 1, 7) == FitResult::WrongSize, L"another component is a wrong size");
			Assert::IsTrue(CheckFit(nullptr, kW, kH, kB, 1.0, kCu10in16, 0) == FitResult::Missing, L"no frame is missing");
			Assert::IsTrue(!GrainSplit::Gpu::Fits(&f, kW, kH, kB, 1.0, kCu10in16, 0, 8), L"Fits agrees");
			// A failing Compute resets Out(): the previous frame never looks valid.
			GrainSplit::Gpu::Options bad;
			Assert::IsFalse(splitter.Compute(gpu, dev.dispatch, nullptr, 0, kCu10in16, 1.0, kB, GrainSplit::Params(), bad), L"invalid input fails");
			Assert::IsTrue(splitter.Out().split == nullptr && splitter.Out().version == 0, L"Out() was reset by the failed Compute");
			splitter.Release(gpu);
			pl_tex_destroy(gpu, &plane);
		}

		TEST_METHOD(GrainUnitPinsTheFullShaderBodies)
		{
			// The Full pass bodies hash to the value of the strings at 160987e5 (git show), so the Full path cannot change
			// silently: an intended change bumps kUnitVersion AND this pin.
			Assert::AreEqual(uint64_t(0x41cef8707edf809dull), GrainSplit::Gpu::FullShaderFingerprint(), L"Full shader bodies are the pre-SA2 text");
			const std::string full = GrainSplit::Gpu::ViewGlsl(Storage::Full, kCu10in16);
			const std::string compact = GrainSplit::Gpu::ViewGlsl(Storage::Compact, kCu10in16);
			const std::string define = "#define GS_UNIT_VERSION " + std::to_string(GrainSplit::kUnitVersion);
			Assert::IsTrue(full.find(define) != std::string::npos && compact.find(define) != std::string::npos, L"the GLSL embeds the unit version");
		}

		TEST_METHOD(GrainUnitCompactIsRefusedAbove12Bits)
		{
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = dev.Gpu();
			const std::vector<double> codes = MakeCodes(kW, kH, 11u);
			// 12-bit codes in a 16-bit plane (code unit 16/65535: 1/cu = 4095.9): Compact is allowed.
			{
				pl_tex plane = UploadR16(gpu, codes, kW, kH, 16.0);
				GrainSplit::Gpu::Splitter s;
				GrainSplit::Gpu::Options o;
				o.storage = Storage::Compact;
				Assert::IsTrue(s.Compute(gpu, dev.dispatch, plane, 0, 16.0 / 65535.0, 1.0, kB, GrainSplit::Params(), o), Wide(s.LastError()).c_str());
				Assert::IsTrue(s.Out().storage == Storage::Compact, L"12-bit: Compact is used");
				Assert::IsTrue(s.StorageNote().empty() && !s.StorageDecision().engaged, L"12-bit: nothing to show");
				s.Release(gpu);
				pl_tex_destroy(gpu, &plane);
			}
			// 16-bit codes (1/cu = 65535): Compact is refused, Full is used, visibly.
			{
				pl_tex plane = UploadR16(gpu, codes, kW, kH, 64.0);
				GrainSplit::Gpu::Splitter compactAsked, fullAsked;
				GrainSplit::Gpu::Options o;
				o.storage = Storage::Compact;
				Assert::IsTrue(compactAsked.Compute(gpu, dev.dispatch, plane, 0, 1.0 / 65535.0, 1.0, kB, GrainSplit::Params(), o), Wide(compactAsked.LastError()).c_str());
				Assert::IsTrue(compactAsked.Out().storage == Storage::Full, L"16-bit: Full is used");
				Assert::IsTrue(!compactAsked.StorageNote().empty(), L"16-bit: the reason is kept");
				const FallbackCatalog::Decision d = compactAsked.StorageDecision();
				Assert::IsTrue(d.engaged && d.id == "grain split storage" && d.reason.find("12 bits") != std::string::npos, L"16-bit: visible FallbackCatalog entry");
				GrainSplit::Gpu::Options f;
				Assert::IsTrue(fullAsked.Compute(gpu, dev.dispatch, plane, 0, 1.0 / 65535.0, 1.0, kB, GrainSplit::Params(), f), L"full computes");
				Assert::IsTrue(SameBits(DownloadRGBA(gpu, dev.dispatch, compactAsked.Out().split), DownloadRGBA(gpu, dev.dispatch, fullAsked.Out().split)),
					L"the fallback is exactly the Full result");
				// Recovery: a later 12-bit frame on the same splitter is Compact again and the entry releases.
				pl_tex plane12 = UploadR16(gpu, codes, kW, kH, 16.0);
				Assert::IsTrue(compactAsked.Compute(gpu, dev.dispatch, plane12, 0, 16.0 / 65535.0, 1.0, kB, GrainSplit::Params(), o), L"12-bit again");
				Assert::IsTrue(compactAsked.Out().storage == Storage::Compact && !compactAsked.StorageDecision().engaged, L"recovered");
				compactAsked.Release(gpu);
				fullAsked.Release(gpu);
				pl_tex_destroy(gpu, &plane12);
				pl_tex_destroy(gpu, &plane);
			}
		}

		TEST_METHOD(GrainUnitCompactNeverClampsAFloatPlane)
		{
			// A float plane can carry blacker-than-black (< 0) and whiter-than-white (> 1): Compact's unorm base would clamp
			// them, so a float plane always takes Full and the base keeps its out-of-range values.
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = dev.Gpu();
			std::vector<float> v(static_cast<size_t>(kW) * kH);
			for (int y = 0; y < kH; ++y)
				for (int x = 0; x < kW; ++x)
					v[static_cast<size_t>(y) * kW + x] = x < kW / 2 ? -0.1f : 1.2f;
			pl_tex plane = UploadR32F(gpu, v, kW, kH);
			Assert::IsNotNull(plane, L"float plane");
			Assert::IsTrue(GrainSplit::Gpu::ChooseStorage(Storage::Compact, plane->params.format, 1.0 / 1023.0).downgraded, L"ChooseStorage refuses float");
			GrainSplit::Gpu::Splitter s;
			GrainSplit::Gpu::Options o;
			o.storage = Storage::Compact;
			const double cu = 1.0 / 1023.0;
			Assert::IsTrue(s.Compute(gpu, dev.dispatch, plane, 0, cu, 1.0, kB, GrainSplit::Params(), o), Wide(s.LastError()).c_str());
			Assert::IsTrue(s.Out().storage == Storage::Full, L"float plane: Full");
			Assert::IsTrue(s.StorageDecision().engaged && s.StorageNote().find("integer") != std::string::npos, L"visible, with the reason");
			const std::vector<float> split = DownloadRGBA(gpu, dev.dispatch, s.Out().split);
			// base (channel w) in codes at an interior pixel of each flat half: value / codeUnit.
			const size_t left = (static_cast<size_t>(kH / 2) * kW + kW / 4) * 4 + 3, right = (static_cast<size_t>(kH / 2) * kW + 3 * kW / 4) * 4 + 3;
			char line[160];
			std::snprintf(line, sizeof line, "float plane base: left %.3f codes (want %.3f), right %.3f (want %.3f)", split[left], -0.1 / cu, split[right], 1.2 / cu);
			Logger::WriteMessage(line);
			Assert::IsTrue(std::fabs(split[left] * cu - (-0.1)) < 1e-3, L"blacker-than-black survives in the base");
			Assert::IsTrue(std::fabs(split[right] * cu - 1.2) < 1e-3, L"whiter-than-white survives in the base");
			s.Release(gpu);
			pl_tex_destroy(gpu, &plane);
		}

		TEST_METHOD(GrainUnitLaneAndLiveFillShareOneCompute)
		{
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = dev.Gpu();
			pl_tex plane = UploadR16(gpu, MakeCodes(kW, kH, 21u), kW, kH, 64.0);
			// Reference: the plain Splitter result of the same plane.
			GrainSplit::Gpu::Splitter ref;
			Assert::IsTrue(ref.Compute(gpu, dev.dispatch, plane, 0, kCu10in16, 1.0, kB), Wide(ref.LastError()).c_str());
			const std::vector<float> refSplit = DownloadRGBA(gpu, dev.dispatch, ref.Out().split);
			const std::vector<float> refBlocks = DownloadRGBA(gpu, dev.dispatch, ref.Out().blocks);

			const VpSkeleton::Registry reg = VpSkeleton::SkeletonProducts();
			VpSkeleton::ConsumerDecl consumer;
			consumer.id = 1;
			VpSkeleton::AddConsume(consumer, VpSkeleton::kP10GrainSplit);
			VpSkeleton::GraphSettings gs;
			gs.dims.width = kW;
			gs.dims.height = kH;
			gs.renderAhead = 2;
			gs.laneLead = 1;
			const VpSkeleton::Graph graph = VpSkeleton::BuildGraph(reg, { consumer }, gs);
			Assert::IsTrue(graph.valid, L"graph");
			const VpSkeleton::Node* node = graph.Find(VpSkeleton::kP10GrainSplit);
			Assert::IsNotNull(node, L"P10 node");
			VpSkeleton::CardProfile card;
			card.vramBudgetBytes = 8ull << 30;
			card.systemRamFreeBytes = 16ull << 30;
			const VpSkeleton::TierPlan plan = VpSkeleton::PlanTiers(reg, graph, card, VpSkeleton::TierInput());

			VpSkeleton::PlTexBackend backend(gpu);
			VpSkeleton::ProductTextures<VpSkeleton::PlTexBackend> tex(backend);   // destroyed before the device
			GrainSplit::GrainDesc desc;
			desc.w = kW; desc.h = kH; desc.B = kB; desc.scale = 1.0; desc.codeUnit = kCu10in16; desc.component = 0;
			Assert::IsTrue(tex.Allocate(*node, reg.Get(VpSkeleton::kP10GrainSplit), GrainSplit::Gpu::GrainLayout(Storage::Full, desc), gs.dims, plan),
				L"the slot textures match the declaration and allocate");
			GrainSplit::GrainNode gnode(VpSkeleton::kP10GrainSplit, node->ring, node->livePool, desc);
			gnode.SetLaneGranted(true);
			GrainSplit::Gpu::Splitter splitter;   // the ONE implementation both paths call
			GrainSplit::Gpu::SplitterFill fill(splitter, gpu, dev.dispatch);
			GrainSplit::Gpu::SplitterFill::Source src;
			src.plane = plane; src.component = 0; src.codeUnit = kCu10in16; src.scale = 1.0; src.B = kB; src.storage = Storage::Full;
			fill.SetSource(src);
			fill.SetTargets([&](const GrainSplit::GrainTarget& t) { return GrainSplit::Gpu::TargetsFor(tex, t, Storage::Full); }, Storage::Full);

			const uint64_t ringN = node->ring;
			const uint64_t laneSeq = 3 * ringN + 1, liveSeq = 3 * ringN + 2;
			gnode.Slots().Discard(laneSeq - ringN);
			gnode.Slots().Discard(liveSeq - ringN);

			// Frame A: the lane precomputes before the seal; every consumer reads the ring slot.
			Assert::IsTrue(gnode.LanePrecompute(laneSeq, fill) == GrainSplit::LaneOutcome::Published, L"lane publishes");
			gnode.Seal(laneSeq);
			const GrainSplit::GrainRead a = gnode.Acquire(laneSeq, desc, "temporal_detail", fill);
			const GrainSplit::GrainRead a2 = gnode.Acquire(laneSeq, desc, "chroma_cfl", fill);
			Assert::IsTrue(a.status == GrainSplit::GrainStatus::Ring && a2.status == GrainSplit::GrainStatus::Ring && a.index == a2.index, L"ring, shared");
			Assert::AreEqual(1u, fill.Fills(), L"the lane computed once, no consumer computed");
			const GrainSplit::Gpu::Frame fa = GrainSplit::Gpu::FrameOf(tex, GrainSplit::GrainTarget{ false, a.index }, Storage::Full, desc, laneSeq);
			Assert::IsTrue(GrainSplit::Gpu::CheckFit(&fa, kW, kH, kB, 1.0, kCu10in16, 0, laneSeq) == GrainSplit::Gpu::FitResult::Fits, L"the lane frame fits its frame");
			Assert::IsTrue(GrainSplit::Gpu::CheckFit(&fa, kW, kH, kB, 1.0, kCu10in16, 0, liveSeq) == GrainSplit::Gpu::FitResult::Stale, L"and is stale for another frame");
			Assert::IsTrue(SameBits(refSplit, DownloadRGBA(gpu, dev.dispatch, fa.split)) && SameBits(refBlocks, DownloadRGBA(gpu, dev.dispatch, fa.blocks)),
				L"the lane result equals the plain Splitter result");

			// Frame B: nothing published; the first consumer fills the live buffer once, the second reuses it.
			gnode.Seal(liveSeq);
			const GrainSplit::GrainRead b = gnode.Acquire(liveSeq, desc, "temporal_detail", fill);
			const GrainSplit::GrainRead b2 = gnode.Acquire(liveSeq, desc, "chroma_cfl", fill);
			Assert::IsTrue(b.status == GrainSplit::GrainStatus::Live && b.filledHere && b2.status == GrainSplit::GrainStatus::Live && !b2.filledHere && b.index == b2.index,
				L"live fill by the first consumer, reused by the second");
			Assert::AreEqual(2u, fill.Fills(), L"exactly one more compute: the live fill");
			const GrainSplit::Gpu::Frame fb = GrainSplit::Gpu::FrameOf(tex, GrainSplit::GrainTarget{ true, b.index }, Storage::Full, desc, liveSeq);
			Assert::IsTrue(SameBits(refSplit, DownloadRGBA(gpu, dev.dispatch, fb.split)) && SameBits(refBlocks, DownloadRGBA(gpu, dev.dispatch, fb.blocks)),
				L"the live-fill result equals the lane result: one implementation");
			splitter.Release(gpu);
			ref.Release(gpu);
			pl_tex_destroy(gpu, &plane);
		}

		// Audit 2 A: one code path, one instance per thread. A lane instance (own Splitter + own pl_dispatch, on its own
		// thread, writing a ring buffer) and a render instance (own Splitter + dispatch, this thread, writing a live
		// buffer) produce bit-identical slots.
		TEST_METHOD(GrainUnitLaneAndRenderInstancesAreBitIdentical)
		{
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = dev.Gpu();
			pl_tex plane = UploadR16(gpu, MakeCodes(kW, kH, 23u), kW, kH, 64.0);
			GrainSplit::GrainDesc desc;
			desc.w = kW; desc.h = kH; desc.B = kB; desc.scale = 1.0; desc.codeUnit = kCu10in16; desc.component = 0;
			for (Storage storage : { Storage::Full, Storage::Compact })
			{
				const VpSkeleton::ProductTexLayout layout = GrainSplit::Gpu::GrainLayout(storage, desc);
				VpSkeleton::PlTexBackend backend(gpu);
				VpSkeleton::Dims dims; dims.width = kW; dims.height = kH;
				std::vector<pl_tex> laneT, liveT;
				for (size_t i = 0; i < layout.terms.size(); ++i)
				{
					laneT.push_back(backend.Create(layout.ShapeOf(i, dims)));
					liveT.push_back(backend.Create(layout.ShapeOf(i, dims)));
					Assert::IsTrue(laneT.back() && liveT.back(), L"slot textures");
				}
				auto targetsOf = [&](const std::vector<pl_tex>& v) {
					GrainSplit::Gpu::Targets t;
					t.split = v[0];
					if (storage == Storage::Compact) { t.splitB = v[1]; t.blocks = v[2]; }
					else t.blocks = v[1];
					return t;
				};
				GrainSplit::Gpu::SplitterFill::Source src;
				src.plane = plane; src.component = 0; src.codeUnit = kCu10in16; src.scale = 1.0; src.B = kB; src.storage = storage;

				// The lane instance: created and used on its own thread with its own dispatch.
				bool laneOk = false;
				std::string laneErr;
				std::thread lane([&]() {
					pl_dispatch laneDispatch = pl_dispatch_create(dev.log, gpu);
					GrainSplit::Gpu::Splitter laneSplitter;
					{
						GrainSplit::Gpu::SplitterFill laneFill(laneSplitter, gpu, laneDispatch);
						laneFill.SetSource(src);
						laneFill.SetTargets([&](const GrainSplit::GrainTarget&) { return targetsOf(laneT); }, storage);
						laneOk = laneDispatch && laneFill.Fill(7, GrainSplit::GrainTarget{ false, 0 });
						laneErr = laneFill.LastRefusal() + laneSplitter.LastError();
					}
					laneSplitter.Release(gpu);
					if (laneDispatch) pl_dispatch_destroy(&laneDispatch);
				});
				lane.join();
				Assert::IsTrue(laneOk, Wide("lane fill: " + laneErr).c_str());

				// The render instance on this thread.
				GrainSplit::Gpu::Splitter renderSplitter;
				GrainSplit::Gpu::SplitterFill renderFill(renderSplitter, gpu, dev.dispatch);
				renderFill.SetSource(src);
				renderFill.SetTargets([&](const GrainSplit::GrainTarget&) { return targetsOf(liveT); }, storage);
				Assert::IsTrue(renderFill.Fill(7, GrainSplit::GrainTarget{ true, 0 }), Wide("render fill: " + renderFill.LastRefusal() + renderSplitter.LastError()).c_str());

				for (size_t i = 0; i < layout.terms.size(); ++i)
					Assert::IsTrue(SameBits(DownloadRGBA(gpu, dev.dispatch, laneT[i]), DownloadRGBA(gpu, dev.dispatch, liveT[i])),
						storage == Storage::Full ? L"Full: the lane and render instances write identical bits" : L"Compact: the lane and render instances write identical bits");
				renderSplitter.Release(gpu);
				for (pl_tex& t : laneT) backend.Destroy(t);
				for (pl_tex& t : liveT) backend.Destroy(t);
			}
			pl_tex_destroy(gpu, &plane);
		}

		// Audit 2 A: a SplitterFill is bound to the thread of its first Fill; another thread's Fill is refused visibly and
		// does no GPU work; an arena bound to another thread is refused too. The owning thread keeps working.
		TEST_METHOD(GrainUnitFillRefusesAnotherThreadAndAnotherThreadsArena)
		{
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = dev.Gpu();
			pl_tex plane = UploadR16(gpu, MakeCodes(kW, kH, 29u), kW, kH, 64.0);
			GrainSplit::Gpu::Splitter splitter;
			GrainSplit::Gpu::SplitterFill fill(splitter, gpu, dev.dispatch);
			GrainSplit::Gpu::SplitterFill::Source src;
			src.plane = plane; src.component = 0; src.codeUnit = kCu10in16; src.scale = 1.0; src.B = kB;
			fill.SetSource(src);
			Assert::IsTrue(fill.Fill(1, GrainSplit::GrainTarget{ true, 0 }), Wide(splitter.LastError()).c_str());
			Assert::IsFalse(fill.FillDecision().engaged, L"accepted fill: nothing shown");
			bool other = true;
			std::thread t([&]() { other = fill.Fill(2, GrainSplit::GrainTarget{ true, 0 }); });
			t.join();
			Assert::IsFalse(other, L"another thread's Fill is refused");
			Assert::AreEqual(1u, fill.Fills(), L"no compute for the refused Fill");
			Assert::AreEqual(1u, fill.Refusals(), L"counted");
			Assert::IsTrue(fill.FillDecision().engaged && fill.LastRefusal().find("another thread") != std::string::npos, L"visible, with the reason");
			Assert::IsTrue(fill.Fill(3, GrainSplit::GrainTarget{ true, 0 }), L"the owning thread recovers at once");
			Assert::IsFalse(fill.FillDecision().engaged, L"and the notice is released");

			// An arena bound to another thread (reserved there) is refused on this thread's instance.
			VpSkeleton::PlTexBackend backend(gpu);
			VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend> arena(backend);
			std::thread r([&]() { arena.Reserve(VpSkeleton::ArenaPlan()); });
			r.join();
			src.arena = &arena;
			fill.SetSource(src);
			Assert::IsFalse(fill.Fill(4, GrainSplit::GrainTarget{ true, 0 }), L"another thread's arena is refused");
			Assert::IsTrue(fill.LastRefusal().find("arena") != std::string::npos, L"with the reason");
			Assert::AreEqual(2u, fill.Fills(), L"still no compute for a refused Fill");

			// Finisher MUST 3: Bind() on the owning thread before any Fill; a Fill from any other thread is then refused
			// before GPU work (no first-use race decides the owner).
			GrainSplit::Gpu::Splitter laneSplitter;
			GrainSplit::Gpu::SplitterFill laneFill(laneSplitter, gpu, dev.dispatch);
			bool bound = false;
			std::thread owner([&]() { bound = laneFill.Bind(); });
			owner.join();
			Assert::IsTrue(bound, L"the owning thread binds");
			Assert::IsFalse(laneFill.Bind(), L"another thread cannot take it");
			src.arena = nullptr;
			laneFill.SetSource(src);
			Assert::IsFalse(laneFill.Fill(5, GrainSplit::GrainTarget{ true, 0 }), L"a non-owner Fill is refused");
			Assert::AreEqual(0u, laneFill.Fills(), L"no compute");
			Assert::AreEqual(1u, laneFill.CrossThreadRefusals(), L"counted");
			Assert::IsTrue(laneFill.FillDecision().engaged, L"visible");
			laneSplitter.Release(gpu);
			splitter.Release(gpu);
			pl_tex_destroy(gpu, &plane);
		}

		// Audit 2 B: the block grid follows the node's real block edge. At 8K the edge doubles (B = 32): the layout's grid
		// is ceil(w / 32) x ceil(h / 32), it passes the declaration check (the declaration is the upper bound at 16 px), and a
		// grid larger than declared (B = 8) is refused. A fill at B = 32 into node textures succeeds.
		TEST_METHOD(GrainUnitBlockGridFollowsTheBlockEdge)
		{
			const VpSkeleton::Registry reg = VpSkeleton::SkeletonProducts();
			const VpSkeleton::ProductDecl& decl = reg.Get(VpSkeleton::kP10GrainSplit);
			VpSkeleton::Dims d8k; d8k.width = 7680; d8k.height = 4320;
			GrainSplit::GrainDesc desc;
			desc.w = 7680; desc.h = 4320; desc.B = 32; desc.scale = 2.0; desc.codeUnit = kCu10in16; desc.component = 0;
			const VpSkeleton::ProductTexLayout full32 = GrainSplit::Gpu::GrainLayout(Storage::Full, desc);
			const VpSkeleton::TexShape g = full32.ShapeOf(1, d8k);
			Assert::AreEqual(240u, g.w, L"7680 / 32");
			Assert::AreEqual(135u, g.h, L"4320 / 32");
			Assert::IsTrue(full32.MatchesDecl(decl, d8k), L"B = 32 is within the declared grid");
			desc.B = 16;
			Assert::IsTrue(GrainSplit::Gpu::GrainLayout(Storage::Full, desc).MatchesDecl(decl, d8k), L"B = 16 equals the declared grid");
			desc.B = 8;
			Assert::IsFalse(GrainSplit::Gpu::GrainLayout(Storage::Full, desc).MatchesDecl(decl, d8k), L"a grid larger than declared is refused");
			VpSkeleton::ProductTexLayout wrongFmt = full32;
			wrongFmt.terms[1].fmt = VpSkeleton::TexFormat::RG32F;
			Assert::IsFalse(wrongFmt.MatchesDecl(decl, d8k), L"an absolute term at other bytes per cell is refused");

			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = dev.Gpu();
			pl_tex plane = UploadR16(gpu, MakeCodes(kW, kH, 31u), kW, kH, 64.0);
			GrainSplit::GrainDesc desc32;
			desc32.w = kW; desc32.h = kH; desc32.B = 32; desc32.scale = 1.0; desc32.codeUnit = kCu10in16; desc32.component = 0;
			VpSkeleton::Dims dims; dims.width = kW; dims.height = kH;
			for (Storage storage : { Storage::Full, Storage::Compact })
			{
				const VpSkeleton::ProductTexLayout layout = GrainSplit::Gpu::GrainLayout(storage, desc32);
				VpSkeleton::PlTexBackend backend(gpu);
				std::vector<pl_tex> tx;
				for (size_t i = 0; i < layout.terms.size(); ++i)
					tx.push_back(backend.Create(layout.ShapeOf(i, dims)));
				GrainSplit::Gpu::Splitter splitter;
				GrainSplit::Gpu::SplitterFill fill(splitter, gpu, dev.dispatch);
				GrainSplit::Gpu::SplitterFill::Source src;
				src.plane = plane; src.component = 0; src.codeUnit = kCu10in16; src.scale = 1.0; src.B = 32; src.storage = storage;
				fill.SetSource(src);
				fill.SetTargets([&](const GrainSplit::GrainTarget&) {
					GrainSplit::Gpu::Targets t;
					t.split = tx[0];
					if (storage == Storage::Compact) { t.splitB = tx[1]; t.blocks = tx[2]; }
					else t.blocks = tx[1];
					return t;
				}, storage);
				Assert::IsTrue(fill.Fill(5, GrainSplit::GrainTarget{ true, 0 }), Wide("B = 32 fill: " + splitter.LastError()).c_str());
				Assert::AreEqual((kW + 31) / 32, splitter.Out().bw, L"block columns at B = 32");
				splitter.Release(gpu);
				for (pl_tex& t : tx) backend.Destroy(t);
			}
			pl_tex_destroy(gpu, &plane);
		}

		// Audit 2 C: Compact requested on a plane that refuses it (a float plane) is laid out as Full (ChooseNodeLayout),
		// so the fill succeeds as Full with the visible storage fallback; targets laid out for a storage the plane no longer
		// grants are refused visibly (the host lays the node out again) instead of failing inside Compute forever.
		TEST_METHOD(GrainUnitDowngradedCompactIsLaidOutAsFull)
		{
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = dev.Gpu();
			std::vector<float> v(static_cast<size_t>(kW) * kH);
			for (size_t i = 0; i < v.size(); ++i) v[i] = 0.25f + 0.001f * static_cast<float>(i % 97);
			pl_tex plane = UploadR32F(gpu, v, kW, kH);
			Assert::IsNotNull(plane, L"float plane");
			const double cu = 1.0 / 1023.0;
			GrainSplit::GrainDesc desc;
			desc.w = kW; desc.h = kH; desc.B = kB; desc.scale = 1.0; desc.codeUnit = cu; desc.component = 0;
			const GrainSplit::Gpu::NodeLayout nl = GrainSplit::Gpu::ChooseNodeLayout(Storage::Compact, plane->params.format, cu, desc);
			Assert::IsTrue(nl.choice.downgraded && nl.choice.storage == Storage::Full, L"Compact refused for a float plane");
			Assert::IsTrue(GrainSplit::Gpu::StorageFallback(nl.choice).engaged, L"visible");
			Assert::IsTrue(nl.StorageDecision().engaged && nl.StorageDecision().id == "grain split storage", L"the node layout carries the visible decision");
			Assert::AreEqual(size_t(2), nl.layout.terms.size(), L"the node is laid out as Full");
			{
				// Finisher MUST 2: the declaration follows the GRANTED storage, so the downgraded node still allocates.
				VpSkeleton::Dims dd; dd.width = kW; dd.height = kH;
				Assert::IsTrue(nl.layout.MatchesDecl(nl.decl, dd), L"the Full layout matches the granted (Full) declaration");
				Assert::IsFalse(nl.layout.MatchesDecl(GrainSplit::GrainDecl(Storage::Compact), dd), L"the requested (Compact) declaration would refuse it");
			}

			VpSkeleton::PlTexBackend backend(gpu);
			VpSkeleton::Dims dims; dims.width = kW; dims.height = kH;
			std::vector<pl_tex> tx;
			for (size_t i = 0; i < nl.layout.terms.size(); ++i)
				tx.push_back(backend.Create(nl.layout.ShapeOf(i, dims)));
			GrainSplit::Gpu::Splitter splitter;
			GrainSplit::Gpu::SplitterFill fill(splitter, gpu, dev.dispatch);
			GrainSplit::Gpu::SplitterFill::Source src;
			src.plane = plane; src.component = 0; src.codeUnit = cu; src.scale = 1.0; src.B = kB; src.storage = Storage::Compact;
			fill.SetSource(src);
			auto fullTargets = [&](const GrainSplit::GrainTarget&) { GrainSplit::Gpu::Targets t; t.split = tx[0]; t.blocks = tx[1]; return t; };
			fill.SetTargets(fullTargets, nl.choice.storage);
			Assert::IsTrue(fill.Fill(1, GrainSplit::GrainTarget{ true, 0 }), Wide("downgraded fill: " + fill.LastRefusal() + splitter.LastError()).c_str());
			Assert::IsTrue(splitter.Out().storage == Storage::Full, L"computed as Full into the Full layout");

			// Targets laid out for Compact while the plane grants only Full: refused before Compute, visibly.
			fill.SetTargets(fullTargets, Storage::Compact);
			Assert::IsFalse(fill.Fill(2, GrainSplit::GrainTarget{ true, 0 }), L"a stale layout is refused");
			Assert::IsTrue(fill.FillDecision().engaged && fill.LastRefusal().find("laid out again") != std::string::npos, L"visible, with the reason");
			Assert::AreEqual(1u, fill.Fills(), L"no compute for the refused Fill");
			fill.SetTargets(fullTargets, nl.choice.storage);
			Assert::IsTrue(fill.Fill(3, GrainSplit::GrainTarget{ true, 0 }), L"recovers once laid out again");
			splitter.Release(gpu);
			for (pl_tex& t : tx) backend.Destroy(t);
			pl_tex_destroy(gpu, &plane);
		}
	};
}
