// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// The grain split as ONE dependency unit (GrainSplit::kUnitVersion, GrainSplitGpu.h, GrainNodeGpu.h) on the libplacebo
// test device (WARP unless VP_TEST_GPU=hardware): version and source-frame stamps and the stale check, the Compact refusal
// rules (more than 12 bits of codes, float planes), the pinned Full shader bodies, and the single compute path shared by
// the lane producer and the render's live fill.
#include "pch.h"
#include "CppUnitTest.h"
#include <VP-V/tests/support/ExtendedRangeGpuProbe.h>

#include <VP-V/modules/repair/grain_split/GpuPassUtil.h>
#include <VP-V/modules/repair/grain_split/GrainNodeGpu.h>
#include <VP-V/modules/repair/grain_split/GrainSplit.h>
#include <VP-V/modules/repair/grain_split/GrainSplitGpu.h>
#include <VP-V/modules/repair/grain_split/GrainSplitVariantOwner.h>
#include <VP-V/modules/input/capture/SourcePlanes.h>
#include <VP-V/core/ComputeOnce.h>
#include <VP-V/core/GpuAttribution.h>
#include <VP-V/core/SkeletonAcquire.h>
#include <VP-V/modules/control/skeleton/gpu/OwnerGpuDevice.h>

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

	// WAIVER-C5-GF-T3: the skeleton side of the grain split's memo variant (as TemporalDetailGpuTests' MemoRig): the real P13
	// source-plane owner, the skeleton's owner GPU device and repair.grain_split's variant owner on a memo with a GPU dispatcher
	// and a roomy ledger grant. Begin offers the frame's luma, End seals the frame and frees what it released.
	struct VariantRig
	{
		Vpv::Skeleton::OwnerGpuDevice device;
		GrainSplit::Gpu::VariantOwner owner;
		VpSkeleton::SkeletonMemo memo;
		VariantRig(pl_gpu gpu, int w, int h) : device(gpu)
		{
			memo.SetHost(this);
			memo.SetGpuDispatcherPresent(true);
			memo.SetGpuBudget(true, uint64_t(1) << 32);
			memo.SetGpuDevice(&device);
			Assert::IsTrue(memo.RegisterOwnerFill(VpSkeleton::kP13SourceRing, VpSkeleton::AcquireForm::Gpu, Vpv::Capture::SourcePlanes::kUnit,
				Vpv::Capture::SourcePlanes::MakeOwnerFill()) == VpSkeleton::SkeletonMemo::Registration::Registered, L"the P13 source-plane owner registers its fill");
			Assert::IsTrue(owner.Register(memo, w, h) == VpSkeleton::SkeletonMemo::Registration::Registered, L"the grain split owner registers its fill");
		}
		~VariantRig()
		{
			memo.ReleaseGpuSpots();
			device.Release();
			Vpv::Capture::SourcePlanes::Forget(this);
		}
		VariantRig(const VariantRig&) = delete;
		VariantRig& operator=(const VariantRig&) = delete;
		void Begin(uint64_t seq, pl_tex tex)
		{
			Vpv::Capture::SourcePlaneSet set;
			set.seq = seq;
			set.planes = 1;
			set.tex[0] = tex;
			set.components[0] = 1;
			set.repr.sys = PL_COLOR_SYSTEM_BT_709;
			set.repr.levels = PL_COLOR_LEVELS_LIMITED;
			set.repr.bits.sample_depth = 16;
			set.repr.bits.color_depth = 10;
			set.repr.bits.bit_shift = 6;
			set.origin = "grain unit GPU test rig";
			Assert::IsTrue(Vpv::Capture::SourcePlanes::Offer(this, set), L"the frame's luma is offered to P13");
		}
		void End(uint64_t seq)
		{
			memo.OnSeal(seq);
			Vpv::Capture::SourcePlanes::BeginFrame(this);
			device.FlushReleases();
		}
	};

	// Compute-once detector 3 on for one test (the ComputeOnceTests EnabledScope): both registries reset around it, the flag off after.
	struct ComputeOnceOn
	{
		ComputeOnceOn()
		{
			ComputeOnce::SetEnabled(true);
			ComputeOnce::Global().Reset();
			GpuAttribution::Global().Reset();
			ComputeOnce::Current() = ComputeOnce::ThreadFrame();
		}
		~ComputeOnceOn()
		{
			ComputeOnce::Global().Reset();
			GpuAttribution::Global().Reset();
			ComputeOnce::Current() = ComputeOnce::ThreadFrame();
			ComputeOnce::SetEnabled(false);
		}
	};

	// `passes` one-texel-grid compute passes ("VP-V owner gpu probe": no attribution rule matches it) into the storable texture `dst`
	// through `dispatch`, the GPU finished after each so the pass timer's callback has been delivered.
	bool RunProbePasses(pl_gpu gpu, pl_dispatch dispatch, pl_tex dst, int passes)
	{
		for (int i = 0; i < passes; ++i)
		{
			const pl_shader_desc d[1] = { VpvGpu::Storage("dst", dst) };
			if (!VpvGpu::Pass(dispatch, "VP-V owner gpu probe", std::string(),
				"imageStore(dst, ivec2(gl_GlobalInvocationID.xy), vec4(1.0));\n", d, 1, nullptr, 0,
				(dst->params.w + 15) / 16, (dst->params.h + 15) / 16, 0))
				return false;
			pl_gpu_finish(gpu);
		}
		return true;
	}

	const GpuAttribution::OwnerRow* OwnerRowOf(const std::vector<GpuAttribution::OwnerRow>& rows, const char* owner)
	{
		for (const GpuAttribution::OwnerRow& r : rows)
			if (r.owner == owner)
				return &r;
		return nullptr;
	}
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
			// The pass texts (v3: half, down, low_s, stats, minimum, apply) hash to the pinned value, so the path cannot change
			// silently: an intended change bumps kUnitVersion AND this pin.
			// WAIVER-P10V3-01 (approved v3 law, refs/116): old pin 0x41cef8707edf809d (v2 pre-SA2 rows/columns/quiet/minimum/Wiener
			// text) -> new 0x8949becef0aab5c9 = FNV-1a 64 of the ten v3 GLSL literals (carriage returns skipped), recomputed offline.
			// WAIVER-P10V4-01 (refs/116b, unit 4: LEVEL pass, NLF cap, depth scale): 0x8949becef0aab5c9 -> 0x7fe213d86466cc12, recomputed offline.
			// WAIVER-P10V5-01 (refs/127 A1, unit 5: STATS per 16x16 chunk + REDUCE): 0x7fe213d86466cc12 -> 0xf33e0215cda77a66, recomputed offline.
			// WAIVER-GS-155 (L#3, commit 6bfb1d462: LEVEL_BODY boundary-bin zero-energy exclusion q <= 1/12 code^2, BTB/WTW preserved): 0xf33e0215cda77a66 -> 0x63b5ed5b2aa6ca3b, recomputed offline.
			Assert::AreEqual(uint64_t(0x63b5ed5b2aa6ca3bull), GrainSplit::Gpu::FullShaderFingerprint(), L"pass shader texts are the unit-5 + GS-155 text");
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
			Assert::IsTrue(fill.FillDecision().engaged, L"the cross-thread notice stays for the rest of the frame");
			fill.BeginFrame();
			Assert::IsFalse(fill.FillDecision().engaged, L"and is released on the next frame");

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

		// C5 MUST-2 / R19-27: the context entry Splitter::Compute(OwnerGpuContext&, ...) (the variant owner's path: no device handle,
		// every texture ctx.Allocate'd, the context's dispatch and shared-memory limit) writes the SAME bytes as the plain
		// Compute(gpu, dispatch, ..., Options{seq}) on the same plane: split and blocks bit-identical, the same Frame stamps. Two
		// variants: the default law at B 16 and an eighth scale (1.5) at its block edge 24. It makes exactly the ContextShapesOf
		// textures (the reserve = what is made); one byte under that reserve, or a closed context, refuses visibly and keeps nothing.
		TEST_METHOD(GrainUnitContextEntryEqualsCompute)
		{
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = dev.Gpu();
			pl_tex plane = UploadR16(gpu, MakeCodes(kW, kH, 23u), kW, kH, 64.0);
			Assert::IsNotNull(plane, L"plane");
			Vpv::Skeleton::OwnerGpuDevice device(gpu);
			const GrainSplit::Params law = GrainSplit::DefaultParams();
			struct Variant { double scale; int B; };
			const Variant variants[2] = { { 1.0, 16 }, { 1.5, 24 } };
			for (const Variant& va : variants)
			{
				const std::wstring tag = L"scale " + std::to_wstring(va.scale) + L", B " + std::to_wstring(va.B) + L": ";
				GrainSplit::Gpu::Splitter reference;
				GrainSplit::Gpu::Options o;
				o.seq = 41;
				Assert::IsTrue(reference.Compute(gpu, dev.dispatch, plane, 0, kCu10in16, va.scale, va.B, law, o), (tag + Wide(reference.LastError())).c_str());
				const GrainSplit::Gpu::Frame rf = reference.Out();
				const std::vector<float> refSplit = DownloadRGBA(gpu, dev.dispatch, rf.split);
				const std::vector<float> refBlocks = DownloadRGBA(gpu, dev.dispatch, rf.blocks);
				Assert::IsTrue(!refSplit.empty() && !refBlocks.empty(), (tag + L"reference downloaded").c_str());

				const GrainSplit::Gpu::ContextShapes shapes = GrainSplit::Gpu::ContextShapesOf(kW, kH, va.B, law);
				Assert::IsTrue(shapes.Valid() && shapes.Bytes() > 0, (tag + L"the size law gives the shapes").c_str());
				{
					VpSkeleton::OwnerGpuContext ctx(&device, shapes.Bytes());
					GrainSplit::Gpu::Splitter splitter;
					GrainSplit::Gpu::ContextHold hold;
					Assert::IsTrue(splitter.Compute(ctx, plane, 0, kCu10in16, va.scale, va.B, law, 41, hold), (tag + Wide(splitter.LastError())).c_str());
					// WAIVER-C5-GF-T3 (GF-P1 S6): after a successful Compute the hold keeps only the two outputs; the five scratch
					// textures are let go after the last pass (the device retires and frees them), so the expected keep-alive count
					// is 2, not 7. The bytes kept are the outputs' bytes; the context still accounted and made all seven (below).
					Assert::AreEqual(size_t(2), hold.keep.size(), (tag + L"two textures kept: split and blocks (the scratch is let go)").c_str());
					Assert::AreEqual(VpSkeleton::ShapeBytes(shapes.split) + VpSkeleton::ShapeBytes(shapes.blocks), hold.bytes,
						(tag + L"the bytes kept are the two outputs'").c_str());
					Assert::AreEqual(shapes.Bytes(), ctx.Used(), (tag + L"all accounted against the context's grant").c_str());
					Assert::AreEqual(uint64_t(7), ctx.Made(), (tag + L"made through the context").c_str());
					const GrainSplit::Gpu::Frame f = splitter.Out();
					Assert::IsTrue(f.w == rf.w && f.h == rf.h && f.bw == rf.bw && f.bh == rf.bh && f.B == rf.B && f.scale == rf.scale &&
						f.codeUnit == rf.codeUnit && f.component == rf.component && f.storage == Storage::Full && f.seq == 41u &&
						f.version == GrainSplit::kUnitVersion && f.splitB == nullptr, (tag + L"the same Frame stamps").c_str());
					Assert::IsTrue(SameBits(DownloadRGBA(gpu, dev.dispatch, f.split), refSplit), (tag + L"split bit-identical to Compute").c_str());
					Assert::IsTrue(SameBits(DownloadRGBA(gpu, dev.dispatch, f.blocks), refBlocks), (tag + L"blocks bit-identical to Compute").c_str());
					ctx.Close();
					GrainSplit::Gpu::Splitter late;
					GrainSplit::Gpu::ContextHold none;
					Assert::IsFalse(late.Compute(ctx, plane, 0, kCu10in16, va.scale, va.B, law, 42, none), (tag + L"a closed context refuses").c_str());
					Assert::IsTrue(none.keep.empty() && late.LastError().find("no GPU context") != std::string::npos, (tag + Wide(late.LastError())).c_str());
				}
				{
					VpSkeleton::OwnerGpuContext under(&device, shapes.Bytes() - 1u);
					GrainSplit::Gpu::Splitter splitter;
					GrainSplit::Gpu::ContextHold hold;
					Assert::IsFalse(splitter.Compute(under, plane, 0, kCu10in16, va.scale, va.B, law, 43, hold), (tag + L"one byte under the reserve refuses").c_str());
					Assert::IsTrue(hold.keep.empty() && hold.bytes == 0u, (tag + L"nothing kept on a refusal").c_str());
					Assert::IsTrue(under.Refused() == 1u && splitter.LastError().find("refused") != std::string::npos, (tag + Wide(splitter.LastError())).c_str());
					Assert::IsTrue(splitter.Out().split == nullptr, (tag + L"no frame on a refusal").c_str());
				}
				reference.Release(gpu);
				device.FlushReleases();
			}
			const Vpv::Skeleton::OwnerGpuDevice::Stats st = device.GetStats();
			Assert::AreEqual(uint64_t(0), st.live, L"every context texture was handed back");
			device.Release();
			pl_tex_destroy(gpu, &plane);
		}

		// GF-P1 S6 (MUST-6 measurement): a 4K luma at B 16 through the memo variant. The context is granted every texture the fill
		// makes (ContextShapesOf: the peak), but the memo's spot keeps only the two outputs once the fill returns, so the memo's
		// GPU bytes after the fill and the seal are split + blocks, not the seven-texture total. Logs the before and after in MB.
		TEST_METHOD(GrainVariantSpotHoldsOnlyTheOutputs4K)
		{
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = dev.Gpu();
			const int w = 3840, h = 2160;
			pl_tex plane = UploadR16(gpu, MakeCodes(w, h, 29u), w, h, 64.0);
			Assert::IsNotNull(plane, L"the 4K plane");
			const GrainSplit::Gpu::ContextShapes shapes = GrainSplit::Gpu::ContextShapesOf(w, h, kB, GrainSplit::DefaultParams());
			Assert::IsTrue(shapes.Valid() && shapes.Bytes() > 0, L"the size law gives the shapes");
			const uint64_t outputs = VpSkeleton::ShapeBytes(shapes.split) + VpSkeleton::ShapeBytes(shapes.blocks);
			{
				VariantRig rig(gpu, w, h);
				rig.Begin(7, plane);
				{
					const VpSkeleton::AcquireResult res = rig.memo.Acquire(VpSkeleton::kP10GrainSplit, 7, VpSkeleton::AcquireForm::Gpu,
						GrainSplit::Gpu::VariantFor(w, h, 1.0, 10));
					Assert::IsTrue(res.Ok(), Wide(rig.owner.LastError()).c_str());
					Assert::IsTrue(res.status == VpSkeleton::AcquireStatus::FilledAtRead, L"the variant was filled at read");
					Assert::IsNotNull(GrainSplit::Gpu::FrameOf(res), L"a published frame");
					Assert::AreEqual(outputs, rig.memo.GpuBytes(), L"after the fill: the spot is the two outputs");
				}
				rig.End(7);
				const uint64_t after = rig.memo.GpuBytes();
				char line[220];
				std::snprintf(line, sizeof line, "4K grain variant spot: before (all seven textures) %.2f MB, after (split + blocks) %.2f MB",
					static_cast<double>(shapes.Bytes()) / 1048576.0, static_cast<double>(after) / 1048576.0);
				Logger::WriteMessage(line);
				Assert::AreEqual(outputs, after, L"after the fill and the seal: the memo holds split + blocks only");
				Assert::AreEqual(uint64_t(133228800), after, L"3840x2160 at B 16: 133,228,800 B (127.06 MiB)");
				Assert::IsTrue(after < shapes.Bytes(), L"smaller than the seven-texture total");
			}
			pl_tex_destroy(gpu, &plane);
		}

		// GF-P1 MUST-6: the variant fill's law is depth-scaled to the variant's code unit (GrainSplit.h DepthScale): the 8-bit and
		// the 12-bit variants equal the CPU GrainSplit::Compute with Params.sourceBits = 8 / 12 on the same codes, within the
		// unit's tolerance (bands 0.002 codes, Wiener share 1e-3, grain level 1e-4 relative: GrainSplitGpuMatchesModel).
		// R1 fix 4 (the control): on a quiet plane (grain energy near the depth-scaled energy floor) the 10-bit law
		// (DefaultParams: the behaviour before MUST-6) DIFFERS from the 8-bit and 12-bit model beyond the same tolerance, so
		// a revert of MUST-6 (the fill ignoring the code unit) fails this test on the model comparison.
		struct LawWorst
		{
			double s = 0.0, f = 0.0, base = 0.0, n = 0.0;
			bool Within() const { return s < 2e-3 && base < 2e-3 && f < 1e-3 && n < 1e-4; }
		};

		static LawWorst CompareToModel(const std::vector<float>& s, const std::vector<float>& b, const GrainSplit::Split& m, const GrainSplit::Plane& y)
		{
			LawWorst w;
			for (size_t i = 0; i < y.v.size(); ++i)
			{
				w.s = (std::max)(w.s, (std::max)(std::fabs(s[i * 4] - m.s1.v[i]), std::fabs(s[i * 4 + 1] - m.s2.v[i])));
				w.f = (std::max)(w.f, std::fabs(s[i * 4 + 2] - m.f.v[i]));
				w.base = (std::max)(w.base, std::fabs(s[i * 4 + 3] - m.base.v[i]));
			}
			for (size_t i = 0; i < m.N.size(); ++i)
				w.n = (std::max)(w.n, std::fabs(b[i * 4] - m.N[i]) / (std::fabs(m.N[i]) + 1e-3));
			return w;
		}

		TEST_METHOD(GrainVariantLawFollowsTheCodeUnit)
		{
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = dev.Gpu();
			// Plane 1: the usual grain on a gradient. Plane 2 (the control): a quiet plane at a low pedestal (10 codes, avoiding
			// float32 cancellation on 300.0f) with sigma = 0.015 codes so 2*N ~ 5e-4 lies between floorE(8-bit) and floorE(10-bit) = 1e-3,
			// making both 8-bit and 12-bit genuinely diverge from the 10-bit law.
			std::vector<double> quiet(static_cast<size_t>(kW) * kH);
			{
				std::mt19937 g(5u);
				std::normal_distribution<double> noise(0.0, 0.015);
				for (int yy = 0; yy < kH; ++yy)
					for (int xx = 0; xx < kW; ++xx)
						quiet[static_cast<size_t>(yy) * kW + xx] = 10.0 + 0.01 * xx + noise(g);
			}
			const std::vector<double> planes[2] = { MakeCodes(kW, kH, 31u), quiet };
			pl_tex tex[2] = { UploadR16(gpu, planes[0], kW, kH, 64.0), UploadR16(gpu, planes[1], kW, kH, 64.0) };
			Assert::IsNotNull(tex[0], L"plane");
			Assert::IsNotNull(tex[1], L"quiet plane");
			const int depths[2] = { 8, 12 };
			{
				VariantRig rig(gpu, kW, kH);
				for (int pi = 0; pi < 2; ++pi)
				{
					// The CPU plane is the codes the texture holds (the 16-bit value is round(code * 64); one code = 64 values).
					GrainSplit::Plane y(kW, kH);
					for (size_t i = 0; i < planes[pi].size(); ++i)
						y.v[i] = static_cast<double>((std::min)(65535L, (std::max)(0L, std::lround(planes[pi][i] * 64.0)))) / 64.0;
					const uint64_t seq = static_cast<uint64_t>(3 + pi);
					rig.Begin(seq, tex[pi]);
					for (int bits : depths)
					{
						const std::wstring tag = std::wstring(pi == 0 ? L"grain plane, " : L"quiet plane, ") + std::to_wstring(bits) + L"-bit variant: ";
						GrainSplit::Params law = GrainSplit::DefaultParams();
						law.sourceBits = bits;
						const GrainSplit::Split m = GrainSplit::Compute(y, 1.0, kB, law);
						const GrainSplit::Split m10 = GrainSplit::Compute(y, 1.0, kB, GrainSplit::DefaultParams());
						const VpSkeleton::AcquireResult res = rig.memo.Acquire(VpSkeleton::kP10GrainSplit, seq, VpSkeleton::AcquireForm::Gpu,
							GrainSplit::Gpu::VariantFor(kW, kH, 1.0, bits));
						Assert::IsTrue(res.Ok(), (tag + Wide(rig.owner.LastError())).c_str());
						const GrainSplit::Gpu::Frame* f = GrainSplit::Gpu::FrameOf(res);
						Assert::IsNotNull(f, (tag + L"a published frame").c_str());
						const std::vector<float> s = DownloadRGBA(gpu, dev.dispatch, f->split);
						const std::vector<float> b = DownloadRGBA(gpu, dev.dispatch, f->blocks);
						Assert::IsTrue(!s.empty() && !b.empty() && s.size() >= y.v.size() * 4u, (tag + L"downloaded").c_str());
						Assert::IsTrue(b.size() >= m.N.size() * 4u, (tag + L"one grain level per block").c_str());
						const LawWorst own = CompareToModel(s, b, m, y);
						const LawWorst ten = CompareToModel(s, b, m10, y);
						char line[260];
						std::snprintf(line, sizeof line, "%s %d-bit variant: vs its own model |s| %.2e |f| %.2e |base| %.2e N rel %.2e; vs the 10-bit model |s| %.2e |f| %.2e |base| %.2e N rel %.2e",
							pi == 0 ? "grain plane," : "quiet plane,", bits, own.s, own.f, own.base, own.n, ten.s, ten.f, ten.base, ten.n);
						Logger::WriteMessage(line);
						Assert::IsTrue(own.Within(), (tag + L"within tolerance of the depth-scaled model (sourceBits = the code unit)").c_str());
						if (pi == 1)
							Assert::IsFalse(ten.Within(), (tag + L"control: the 10-bit law is NOT within tolerance of this variant, so a revert of the depth scaling goes red").c_str());
					}
					rig.End(seq);
				}
			}
			pl_tex_destroy(gpu, &tex[0]);
			pl_tex_destroy(gpu, &tex[1]);
		}

		// GF-P5 S11: the owner GPU device's dispatch is watched by compute-once detector 3: with the detector ON when
		// OwnerDispatch() creates it, a pass run through a variant owner's context (ctx.Dispatch()) arrives as an "owner_gpu" row and
		// is not unattributed; a dispatch created while the detector is OFF installs no callback (the off cost is one flag load), so
		// the same pass leaves no "owner_gpu" row. Asserts on what the callback delivered.
		BEGIN_TEST_METHOD_ATTRIBUTE(OwnerGpuDeviceDispatchIsWatched)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(OwnerGpuDeviceDispatchIsWatched)
		{
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = dev.Gpu();
			VpSkeleton::TexShape shape;
			shape.w = 16;
			shape.h = 16;
			shape.fmt = VpSkeleton::TexFormat::RGBA32F;
			const uint64_t bytes = VpSkeleton::ShapeBytes(shape);
			{
				// (a) ON when the dispatch is created.
				ComputeOnceOn on;
				Vpv::Skeleton::OwnerGpuDevice device(gpu);
				{
					VpSkeleton::OwnerGpuContext ctx(&device, bytes);
					VpSkeleton::GpuMemoHandle made;
					std::string why;
					Assert::IsTrue(ctx.Allocate(shape, made, why), Wide(why).c_str());
					Assert::IsNotNull(made.resource, L"an owner texture");
					pl_dispatch dispatch = static_cast<pl_dispatch>(ctx.Dispatch());
					Assert::IsNotNull(dispatch, L"the context's dispatch");
					Assert::IsTrue(RunProbePasses(gpu, dispatch, static_cast<pl_tex>(made.resource), 3), L"the probe passes ran");
					const std::vector<GpuAttribution::OwnerRow> owners = GpuAttribution::Global().Owners();
					const GpuAttribution::OwnerRow* row = OwnerRowOf(owners, "owner_gpu");
					Assert::IsNotNull(row, L"a pass through the owner device is charged to owner_gpu");
					Assert::IsTrue(row->passes >= 1u, L"at least one pass delivered to the detector");
					Assert::AreEqual(uint64_t(0), GpuAttribution::Global().UnattributedPasses(), L"and none is unattributed");
					made.keepAlive.reset();
					ctx.Close();
				}
				device.FlushReleases();
				device.Release();
			}
			{
				// (b) OFF when the dispatch is created: nothing is installed, so nothing arrives, though the detector is ON afterwards.
				Vpv::Skeleton::OwnerGpuDevice device(gpu);
				ComputeOnce::SetEnabled(false);
				Assert::IsNotNull(device.OwnerDispatch(), L"the dispatch is made with the detector off");
				ComputeOnceOn on;   // ON from here, registries reset
				VpSkeleton::OwnerGpuContext ctx(&device, bytes);
				VpSkeleton::GpuMemoHandle made;
				std::string why;
				Assert::IsTrue(ctx.Allocate(shape, made, why), Wide(why).c_str());
				Assert::IsTrue(RunProbePasses(gpu, static_cast<pl_dispatch>(ctx.Dispatch()), static_cast<pl_tex>(made.resource), 3),
					L"the probe passes ran");
				Assert::IsNull(OwnerRowOf(GpuAttribution::Global().Owners(), "owner_gpu"), L"a dispatch made while the detector was off is not watched");
				made.keepAlive.reset();
				ctx.Close();
				device.FlushReleases();
				device.Release();
			}
		}

		// GS-155 (L#3): CPU-vs-GPU parity for boundary-bin zero-energy exclusion (q <= 1/12 code^2) with BTB/WTW preserved.
		// Synthetic 10-bit plane (256x384, B=16 -> 16x24 blocks) with four bands:
		//   by =  0.. 4: crushed zero-energy black (luma = 0.0, flat, quiet == 0 <= 1/12) -> bin 0
		//   by =  5..11: grainy BTB (luma = 32.0 < 64, sigma = 2.0, quiet ~ 4.0 > 1/12)   -> bin 0
		//   by = 12..18: grainy WTW (luma = 980.0 > 940, sigma = 2.0, quiet ~ 4.0 > 1/12) -> top bin (levelBins - 1)
		//   by = 19..23: clipped zero-energy white (luma = 1023.0, flat, quiet <= 1/12)   -> top bin (levelBins - 1)
		// Same-input negative control (Rule 3): pre-fix naive percentile over ALL bin-0 / top-bin blocks collapses both
		// boundary levels to ~2^-6 (< 0.05) and crushes interior BTB/WTW block N; protected CPU and GPU both exclude only
		// zero-energy boundary blocks, recover bin 0 and top-bin levels (> 1.0), preserve interior BTB/WTW N (> 1.0), and match.
		BEGIN_TEST_METHOD_ATTRIBUTE(Gs155ZeroEnergyBoundaryExclusionMatchesCpuAndPreservesBtbWtw)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(Gs155ZeroEnergyBoundaryExclusionMatchesCpuAndPreservesBtbWtw)
		{
			GpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = dev.Gpu();

			const int w = 256, h = 384, B = 16;
			const int bw = w / B, bh = h / B;   // 16 x 24 blocks
			const GrainSplit::Params params = GrainSplit::DefaultParams();
			const int topBin = params.levelBins - 1;

			// Build exact 1/64-code-quantized 10-bit plane so CPU and GPU receive bit-identical code values.
			std::vector<double> rawCodes(static_cast<size_t>(w) * h, 0.0);
			GrainSplit::Plane y(w, h, 0.0);
			std::mt19937 rng(155u);
			std::normal_distribution<double> grain(0.0, 2.0);
			for (int py = 0; py < h; ++py)
			{
				const int by = py / B;
				for (int px = 0; px < w; ++px)
				{
					double v = 0.0;
					if (by <= 4)
						v = 0.0;   // crushed zero-energy black
					else if (by <= 11)
						v = (std::min)(63.0, (std::max)(1.0, 32.0 + grain(rng)));     // grainy BTB (< 64)
					else if (by <= 18)
						v = (std::min)(1022.0, (std::max)(941.0, 980.0 + grain(rng))); // grainy WTW (> 940)
					else
						v = 1023.0;   // clipped zero-energy white
					const long q16 = (std::min)(65535L, (std::max)(0L, std::lround(v * 64.0)));
					const double exactCode = static_cast<double>(q16) / 64.0;
					const size_t idx = static_cast<size_t>(py) * w + px;
					rawCodes[idx] = exactCode;
					y.v[idx] = exactCode;
				}
			}

			// Independent CPU reference: full Compute plus per-block quiet / blockLuma to inspect CPU NLF table and run
			// the same-input pre-fix naive negative control.
			const GrainSplit::Split cpuSplit = GrainSplit::Compute(y, 1.0, B, params);
			const size_t nb = static_cast<size_t>(bw) * bh;
			std::vector<double> cpuQuiet(nb, 0.0), cpuBlockLuma(nb, 0.0);
			{
				const GrainSplit::Plane g1 = GrainSplit::Gauss(y, params.sigmaFine);
				const GrainSplit::Plane yh = GrainSplit::V3Half(y);
				const GrainSplit::Plane gbh = GrainSplit::V3HalfBlur(yh, params.sigmaBase);
				GrainSplit::Plane d(w, h);
				for (int py = 0; py < h; ++py)
					for (int px = 0; px < w; ++px)
					{
						const double gb = GrainSplit::V3LowAt(gbh, px, py);
						const double d1 = y.at(px, py) - g1.at(px, py);
						const double d2 = g1.at(px, py) - gb;
						d.at(px, py) = d1 + d2;
					}
				for (int by = 0; by < bh; ++by)
					for (int bx = 0; bx < bw; ++bx)
					{
						std::vector<double> e;
						for (int sj = 0; sj < params.subBlocks; ++sj)
							for (int si = 0; si < params.subBlocks; ++si)
							{
								const int x0 = bx * B + si * B / params.subBlocks, x1 = (std::min)(w, bx * B + (si + 1) * B / params.subBlocks);
								const int y0 = by * B + sj * B / params.subBlocks, y1 = (std::min)(h, by * B + (sj + 1) * B / params.subBlocks);
								if (x1 <= x0 || y1 <= y0) continue;
								double sum = 0.0;
								for (int yy = y0; yy < y1; ++yy)
									for (int xx = x0; xx < x1; ++xx) sum += d.at(xx, yy) * d.at(xx, yy);
								e.push_back(sum / static_cast<double>((x1 - x0) * (y1 - y0)));
							}
						std::sort(e.begin(), e.end());
						const size_t nq = (std::min)(e.size(), static_cast<size_t>((std::max)(1, params.quietSubBlocks)));
						double m = 0.0;
						for (size_t i = 0; i < nq; ++i) m += e[i];
						const size_t bi = static_cast<size_t>(by) * bw + bx;
						cpuQuiet[bi] = nq ? params.grainCal * m / static_cast<double>(nq) : 0.0;
						cpuBlockLuma[bi] = GrainSplit::V3LowAt(gbh, (std::min)(bx * B + B / 2, w - 1), (std::min)(by * B + B / 2, h - 1));
					}
			}

			// Same-input negative control (Rule 3): pre-fix naive 10th percentile WITHOUT GS-155 boundary zero-energy exclusion.
			std::vector<double> naiveLevel(static_cast<size_t>(params.levelBins), 0.0);
			std::vector<bool> naiveValid(static_cast<size_t>(params.levelBins), false);
			{
				const double cs = GrainSplit::DepthScale(params.sourceBits), es = cs * cs;
				std::vector<std::vector<int>> keys(static_cast<size_t>(params.levelBins));
				for (size_t i = 0; i < cpuQuiet.size(); ++i)
				{
					const int bin = GrainSplit::LevelBin(cpuBlockLuma[i] / cs, params);
					keys[static_cast<size_t>(bin)].push_back(GrainSplit::LevelKey(cpuQuiet[i] / es));
				}
				for (int j = 0; j < params.levelBins; ++j)
				{
					std::vector<int>& k = keys[static_cast<size_t>(j)];
					const int n = static_cast<int>(k.size());
					naiveValid[static_cast<size_t>(j)] = n >= params.levelMinBlocks && n > 0;
					if (!naiveValid[static_cast<size_t>(j)]) continue;
					std::sort(k.begin(), k.end());
					const int r = (params.levelPercentile * n + 99) / 100;
					naiveLevel[static_cast<size_t>(j)] = GrainSplit::LevelOfKey(k[static_cast<size_t>((std::max)(1, r) - 1)]);
				}
			}
			Assert::IsTrue(naiveValid[0] && naiveLevel[0] < 0.05,
				L"negative control: pre-fix naive bin-0 percentile collapses toward zero under crushed black blocks");
			Assert::IsTrue(naiveValid[static_cast<size_t>(topBin)] && naiveLevel[static_cast<size_t>(topBin)] < 0.05,
				L"negative control: pre-fix naive top-bin percentile collapses toward zero under clipped white blocks");

			// Protected CPU NLF table on the exact same plane.
			std::vector<double> cpuLevel(static_cast<size_t>(params.levelBins), 0.0);
			std::unique_ptr<bool[]> cpuValid(new bool[static_cast<size_t>(params.levelBins)]);
			GrainSplit::GrainLevelTable(cpuQuiet, cpuBlockLuma, params, cpuLevel.data(), cpuValid.get());
			Assert::IsTrue(cpuValid[0] && cpuLevel[0] > 1.0,
				L"CPU GS-155: bin 0 recovers grainy BTB level (> 1.0) despite crushed flat black blocks");
			Assert::IsTrue(cpuValid[topBin] && cpuLevel[static_cast<size_t>(topBin)] > 1.0,
				L"CPU GS-155: top bin recovers grainy WTW level (> 1.0) despite clipped flat white blocks");

			// GPU execution with caller-supplied scratch textures (including `level`) so we inspect the GPU NLF table directly.
			struct GpuGuard
			{
				pl_gpu gpu = nullptr;
				pl_tex plane = nullptr;
				GrainSplit::Gpu::Splitter splitter;
				GrainSplit::Gpu::Scratch scratch;
				explicit GpuGuard(pl_gpu devGpu) : gpu(devGpu) {}
				~GpuGuard()
				{
					splitter.Release(gpu);
					for (pl_tex* t : { &scratch.tmp, &scratch.low, &scratch.part, &scratch.quiet, &scratch.level, &plane })
						if (gpu && *t) { pl_tex_destroy(gpu, t); *t = nullptr; }
				}
			};
			GpuGuard g(gpu);

			g.plane = UploadR16(gpu, rawCodes, w, h, 64.0);
			Assert::IsNotNull(g.plane, L"upload synthetic GS-155 10-bit plane");
			const GrainSplit::Gpu::ContextShapes shapes = GrainSplit::Gpu::ContextShapesOf(w, h, B, params);
			Assert::IsTrue(shapes.Valid(), L"valid scratch shapes");
			VpSkeleton::PlTexBackend backend(gpu);
			g.scratch.tmp = backend.Create(shapes.tmp);
			g.scratch.low = backend.Create(shapes.low);
			g.scratch.part = backend.Create(shapes.part);
			g.scratch.quiet = backend.Create(shapes.quiet);
			g.scratch.level = backend.Create(shapes.level);
			Assert::IsTrue(g.scratch.All(), L"allocated GPU scratch textures including level table");

			GrainSplit::Gpu::Options opts;
			opts.storage = Storage::Full;
			opts.scratch = g.scratch;
			Assert::IsTrue(g.splitter.Compute(gpu, dev.dispatch, g.plane, 0, kCu10in16, 1.0, B, params, opts),
				Wide(g.splitter.LastError()).c_str());

			const std::vector<float> gpuLevel = DownloadRGBA(gpu, dev.dispatch, g.scratch.level);
			const std::vector<float> gpuBlocks = DownloadRGBA(gpu, dev.dispatch, g.splitter.Out().blocks);
			const std::vector<float> gpuSplit = DownloadRGBA(gpu, dev.dispatch, g.splitter.Out().split);
			Assert::AreEqual(static_cast<size_t>(params.levelBins) * 4u, gpuLevel.size(), L"downloaded GPU level table");
			Assert::AreEqual(nb * 4u, gpuBlocks.size(), L"downloaded GPU blocks");
			Assert::AreEqual(static_cast<size_t>(w) * h * 4u, gpuSplit.size(), L"downloaded GPU split");

			char msg[320];
			std::snprintf(msg, sizeof msg,
				"GS-155 bin0: naive %.4f | CPU %.4f (valid %d) | GPU %.4f (valid %.0f) || topBin(%d): naive %.4f | CPU %.4f (valid %d) | GPU %.4f (valid %.0f)",
				naiveLevel[0], cpuLevel[0], cpuValid[0] ? 1 : 0, gpuLevel[0], gpuLevel[1],
				topBin, naiveLevel[static_cast<size_t>(topBin)], cpuLevel[static_cast<size_t>(topBin)], cpuValid[topBin] ? 1 : 0,
				gpuLevel[static_cast<size_t>(topBin) * 4u], gpuLevel[static_cast<size_t>(topBin) * 4u + 1u]);
			Logger::WriteMessage(msg);

			// Verify GPU boundary-bin NLF recovery and full-table parity with CPU.
			const auto relErr = [](double got, double ref) { return std::fabs(got - ref) / (std::max)(std::fabs(ref), 1e-2); };
			Assert::IsTrue(gpuLevel[1] > 0.5f && gpuLevel[0] > 1.0f,
				L"GPU GS-155: bin 0 is valid and recovers grainy BTB level (> 1.0, not collapsed by crushed black)");
			Assert::IsTrue(gpuLevel[static_cast<size_t>(topBin) * 4u + 1u] > 0.5f && gpuLevel[static_cast<size_t>(topBin) * 4u] > 1.0f,
				L"GPU GS-155: top bin is valid and recovers grainy WTW level (> 1.0, not collapsed by clipped white)");
			for (int j = 0; j < params.levelBins; ++j)
			{
				const bool gpuVal = gpuLevel[static_cast<size_t>(j) * 4u + 1u] > 0.5f;
				Assert::AreEqual(cpuValid[j], gpuVal, (L"bin " + std::to_wstring(j) + L": GPU and CPU valid flags agree").c_str());
				if (cpuValid[j])
					Assert::IsTrue(relErr(gpuLevel[static_cast<size_t>(j) * 4u], cpuLevel[static_cast<size_t>(j)]) <= 1e-3,
						(L"bin " + std::to_wstring(j) + L": GPU and CPU NLF level agree within 1e-3 rel").c_str());
			}

			// Verify interior grainy BTB (by = 8, >= 3 blocks from flat black and WTW) and interior grainy WTW (by = 15)
			// retain non-collapsed block grain level N (> 1.0) on both CPU and GPU.
			for (int bx = 0; bx < bw; ++bx)
			{
				const size_t iBtb = static_cast<size_t>(8) * bw + bx;
				const size_t iWtw = static_cast<size_t>(15) * bw + bx;
				Assert::IsTrue(cpuSplit.N[iBtb] > 1.0 && gpuBlocks[iBtb * 4u] > 1.0f,
					L"interior grainy BTB block retains grain level N > 1.0 on both CPU and GPU");
				Assert::IsTrue(cpuSplit.N[iWtw] > 1.0 && gpuBlocks[iWtw * 4u] > 1.0f,
					L"interior grainy WTW block retains grain level N > 1.0 on both CPU and GPU");
			}

			// Full-plane CPU-vs-GPU parity across all blocks and pixels (GrainSplitV3GpuTests contract tolerances).
			double worstN = 0.0, worstS = 0.0, worstSrel = 0.0, worstBase = 0.0, worstF = 0.0;
			for (size_t i = 0; i < nb; ++i)
				worstN = (std::max)(worstN, relErr(gpuBlocks[i * 4u], cpuSplit.N[i]));
			for (size_t i = 0; i < y.v.size(); ++i)
			{
				const double r1 = cpuSplit.s1.v[i], r2 = cpuSplit.s2.v[i];
				const double e1 = std::fabs(gpuSplit[i * 4u] - r1), e2 = std::fabs(gpuSplit[i * 4u + 1u] - r2);
				if (std::fabs(r1) <= 64.0) worstS = (std::max)(worstS, e1); else worstSrel = (std::max)(worstSrel, e1 / std::fabs(r1));
				if (std::fabs(r2) <= 64.0) worstS = (std::max)(worstS, e2); else worstSrel = (std::max)(worstSrel, e2 / std::fabs(r2));
				worstF = (std::max)(worstF, std::fabs(gpuSplit[i * 4u + 2u] - cpuSplit.f.v[i]));
				worstBase = (std::max)(worstBase, std::fabs(gpuSplit[i * 4u + 3u] - cpuSplit.base.v[i]));
			}
			std::snprintf(msg, sizeof msg,
				"GS-155 CPU-vs-GPU parity: worst N rel %.2e, |s1,s2| %.5f (rel %.2e), |base| %.5f, |f| %.6f",
				worstN, worstS, worstSrel, worstBase, worstF);
			Logger::WriteMessage(msg);
			Assert::IsTrue(worstN <= 1e-3, L"GS-155: block N CPU-vs-GPU relative error <= 1e-3");
			Assert::IsTrue(worstS <= 0.05, L"GS-155: |s1|,|s2| CPU-vs-GPU error <= 0.05 code for |s| <= 64");
			Assert::IsTrue(worstSrel <= 1.0 / 1024.0, L"GS-155: |s1|,|s2| CPU-vs-GPU relative error <= 2^-10 for |s| > 64");
			Assert::IsTrue(worstBase <= 0.05, L"GS-155: |base| CPU-vs-GPU error <= 0.05 code");
			Assert::IsTrue(worstF <= 1.0 / 255.0, L"GS-155: |f| CPU-vs-GPU error <= 1/255");
		}
	};
}
