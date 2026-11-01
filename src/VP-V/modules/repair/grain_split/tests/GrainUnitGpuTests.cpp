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
			Assert::IsTrue(tex.Allocate(*node, reg.Get(VpSkeleton::kP10GrainSplit), GrainSplit::Gpu::GrainLayout(Storage::Full), gs.dims, plan),
				L"the slot textures match the declaration and allocate");

			GrainSplit::GrainDesc desc;
			desc.w = kW; desc.h = kH; desc.B = kB; desc.scale = 1.0; desc.codeUnit = kCu10in16; desc.component = 0;
			GrainSplit::GrainNode gnode(VpSkeleton::kP10GrainSplit, node->ring, node->livePool, desc);
			gnode.SetLaneGranted(true);
			GrainSplit::Gpu::Splitter splitter;   // the ONE implementation both paths call
			GrainSplit::Gpu::SplitterFill fill(splitter, gpu, dev.dispatch);
			GrainSplit::Gpu::SplitterFill::Source src;
			src.plane = plane; src.component = 0; src.codeUnit = kCu10in16; src.scale = 1.0; src.B = kB; src.storage = Storage::Full;
			fill.SetSource(src);
			fill.SetTargets([&](const GrainSplit::GrainTarget& t) { return GrainSplit::Gpu::TargetsFor(tex, t, Storage::Full); });

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
	};
}
