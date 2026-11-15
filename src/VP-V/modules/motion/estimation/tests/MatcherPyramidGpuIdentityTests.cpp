// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// L-B4 (BBP-B B4, run in B5): the matcher reading the SHARED luma pyramid (kP1HalfLuma, Gpu, MatcherPyramidKey(L)) gives the same
// motion outputs, byte for byte, as the matcher building its own pyramid (the visible fallback, row product.motion_warp) and as
// the matcher without a seam at all (the pre-B3 behaviour). Each arm runs two real MotionWarp Estimates (A = 1, B = 2, then
// A = 2, B = 3: the second A is the product's level of seq 2, the former reuse) on the real wiring: a skeleton memo, the P13 owner
// fill (SourcePlanes::MakeOwnerFill), the matcher's offers (VectorEngine::OfferMatcherPlanes) and the engine's read seam
// (VectorEngine::MatcherPyramidSource). Product arm: PyramidFromProduct() on every estimate and PyramidFallbackWhy() empty (USED,
// not falling back); lowRes: InnerPyramidFromProduct() (levels 2 / 4 / 6). Forced arm: a seam answering Unavailable ->
// PyramidFallbackWhy() names it. Outputs compared: Forward, Backward, Field, GlobalVector (copied to RGBA32F: the product path
// requires settings.hostReadable = false, so the matcher's own textures are not host-readable).
// STAND-IN (B5 must replace it): the (kP1HalfLuma, Gpu) owner fill here is the test's copy of P1GpuFill.cpp MakeP1GpuOwnerFill's
// body (the owner's law function P1Gpu::BuildPyramidInto, MatcherLevelLawOf, MatcherCodes, VariantTexShape + MatcherStorageFormat,
// one test dispatch runner). The real fill is reachable only through RegisterP1GpuModule, whose module the hub owns and
// ResetModulesForTesting deletes while RegisteredP1GpuModule() keeps the pointer (a later NoteP1GpuSourceLuma would use it):
// unsafe in a test process. WARP unless VP_TEST_GPU=hardware; category GpuHardware (the B5 gate run).
#include "pch.h"
#include <VP-V/tests/VpLutGpuTestSupport.h>
#include <VP-V/modules/motion/estimation/VectorEngine.h>
#include <VP-V/modules/motion/estimation/matcher/MatcherPyramidSource.h>
#include <VP-V/modules/input/capture/SourcePlanes.h>
#include <VP-V/modules/input/source_analysis/lane/P1GpuFill.h>
#include <VP-V/modules/control/skeleton/SkeletonFacts.h>
#include <VP-V/core/SkeletonAcquire.h>
#include <VP-V/core/VariantShape.h>
#include <VP-V/core/hub/ExtensionHub.h>

#include <libplacebo/dispatch.h>
#include <libplacebo/shaders/custom.h>

#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace VideoProcessorTest
{
	namespace
	{
		namespace P1Gpu = VpFrameAnalysis::P1Gpu;
		using VpSkeleton::AcquireForm;
		using VpSkeleton::SkeletonMemo;

		struct GpuDemand : Vpv::Skeleton::IExternalDemand
		{
			std::shared_ptr<SkeletonMemo> memo;
			const void* host = nullptr;
			void SetExternalDemand(const void*, const void*, const std::vector<VpSkeleton::ConsumerDecl>&,
				const std::vector<VpSkeleton::ProductId>&) override {}
			void ClearExternalDemand(const void*, const void*) override {}
			Vpv::Skeleton::FrameMemo MemoFor(const void* h, uint64_t seq) override
			{
				Vpv::Skeleton::FrameMemo fm;
				if (!memo || h != host)
					return fm;
				fm.memo = memo;
				fm.host = h;
				fm.seq = seq;
				return fm;
			}
		};

		struct GpuSkeleton : Vpv::Hub::ISkeletonModule
		{
			GpuDemand* demand;
			explicit GpuSkeleton(GpuDemand* d) : demand(d) {}
			const char* Name() const override { return "test.matcher_pyramid_gpu.fake_skeleton"; }
			void* Query(const char* id) override
			{
				return id != nullptr && std::string(id) == Vpv::Skeleton::kExternalDemandQueryId
					? static_cast<Vpv::Skeleton::IExternalDemand*>(demand) : nullptr;
			}
		};

		// One pass of the owner's law on the test's dispatch (the descriptors of P1GpuFill.cpp DispatchPass / MotionWarp::Dispatch).
		bool RunPyramidPass(pl_dispatch dispatch, const P1Gpu::PyramidPass& p)
		{
			pl_shader_desc d[2]{};
			d[0].desc.name = "src";
			d[0].desc.type = PL_DESC_SAMPLED_TEX;
			d[0].binding.object = p.src;
			d[0].binding.address_mode = PL_TEX_ADDRESS_CLAMP;
			d[0].binding.sample_mode = PL_TEX_SAMPLE_NEAREST;
			d[1].desc.name = "dst";
			d[1].desc.type = PL_DESC_STORAGE_IMG;
			d[1].desc.access = PL_DESC_ACCESS_WRITEONLY;
			d[1].binding.object = p.dst;
			pl_shader_var v{ pl_var_float("codes"), &p.codes, true };
			pl_custom_shader custom{};
			custom.description = p.description;
			custom.header = p.header.empty() ? nullptr : p.header.c_str();
			custom.body = p.body->c_str();
			custom.input = PL_SHADER_SIG_NONE;
			custom.output = PL_SHADER_SIG_NONE;
			custom.descriptors = d;
			custom.num_descriptors = 2;
			custom.variables = p.hasCodes ? &v : nullptr;
			custom.num_variables = p.hasCodes ? 1 : 0;
			custom.compute = true;
			custom.compute_group_size[0] = p.group;
			custom.compute_group_size[1] = p.group;
			pl_shader sh = pl_dispatch_begin(dispatch);
			if (!pl_shader_custom(sh, &custom))
			{
				pl_dispatch_abort(dispatch, &sh);
				return false;
			}
			pl_dispatch_compute_params compute{};
			compute.shader = &sh;
			compute.dispatch_size[0] = (p.width + p.group - 1) / p.group;
			compute.dispatch_size[1] = (p.height + p.group - 1) / p.group;
			compute.dispatch_size[2] = 1;
			return pl_dispatch_compute(dispatch, &compute);
		}

		// The sanctioned GPU context's device side (SkeletonGpuDispatcher's role): R16F level textures on the test gpu.
		struct TestOwnerDevice : VpSkeleton::IOwnerGpuDevice
		{
			pl_gpu gpu = nullptr;
			pl_dispatch dispatch = nullptr;
			void* OwnerGpu() const override { return const_cast<void*>(static_cast<const void*>(gpu)); }
			void* OwnerDispatch() const override { return dispatch; }
			bool CreateOwnerTexture(const VpSkeleton::TexShape& shape, VpSkeleton::GpuMemoHandle& out, std::string& why) override
			{
				if (shape.fmt != VpSkeleton::TexFormat::R16F)
				{
					why = "test device: R16F only";
					return false;
				}
				const enum pl_fmt_caps caps = static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_STORABLE | PL_FMT_CAP_LINEAR);
				pl_tex_params tp{};
				tp.w = int(shape.w);
				tp.h = int(shape.h);
				tp.format = pl_find_fmt(gpu, PL_FMT_FLOAT, 1, 16, 16, caps);
				tp.sampleable = true;
				tp.storable = true;
				pl_tex tex = tp.format ? pl_tex_create(gpu, &tp) : nullptr;
				if (tex == nullptr)
				{
					why = "test device: no R16F texture";
					return false;
				}
				const pl_gpu g = gpu;
				out.resource = const_cast<void*>(static_cast<const void*>(tex));
				out.width = shape.w;
				out.height = shape.h;
				out.keepAlive = std::shared_ptr<void>(const_cast<void*>(static_cast<const void*>(tex)), [g](void* t) {
					pl_tex x = static_cast<pl_tex>(t);
					pl_tex_destroy(g, &x);
				});
				return true;
			}
		};

		// STAND-IN for P1GpuFill.cpp MakeP1GpuOwnerFill (see the file header): the same declaration and body.
		VpSkeleton::OwnerFill StandInP1Fill(uint64_t reserveBytes)
		{
			VpSkeleton::OwnerFill f;
			f.variantAxes = P1Gpu::OwnerVariantAxes();
			f.reserveBytes = reserveBytes;
			f.pixelData = false;
			f.keepFrames = 1;
			f.historyFrames = 1;
			f.fill = [](const VpSkeleton::OwnerFillRequest& req, SkeletonMemo& memo, VpSkeleton::OwnerFillOutput& out) {
				if (req.gpu == nullptr || req.gpu->Dispatch() == nullptr)
					return false;
				const uint8_t level = P1Gpu::MatcherPyramidLevel(req.variant);
				P1Gpu::MatcherLevelLaw law;
				if (level == 0 || !P1Gpu::MatcherLevelLawOf(level, &law))
					return false;
				const VpSkeleton::AcquireResult src = memo.Acquire(VpSkeleton::kP13SourceRing, req.seq, AcquireForm::Gpu);
				if (!src.Ok() || src.gpu.resource == nullptr)
					return false;
				const pl_tex luma = static_cast<pl_tex>(src.gpu.resource);
				VpSkeleton::Dims full;
				full.width = uint32_t(luma->params.w);
				full.height = uint32_t(luma->params.h);
				pl_tex in = luma;
				VpSkeleton::AcquireResult above;
				if (law.input != 0)
				{
					above = memo.Acquire(VpSkeleton::kP1HalfLuma, req.seq, AcquireForm::Gpu, P1Gpu::MatcherPyramidKey(law.input));
					if (!above.Ok() || above.gpu.resource == nullptr)
						return false;
					in = static_cast<pl_tex>(above.gpu.resource);
				}
				const Vpv::Capture::SourcePlaneSet* facts = Vpv::Capture::SourcePlanes::FactsOf(src);
				const bool fullRange = facts != nullptr && facts->fullRange;
				VpSkeleton::TexShape shape;
				std::string why;
				if (!VpSkeleton::VariantTexShape(req.variant, full, 1, P1Gpu::MatcherStorageFormat(), &shape) || !req.gpu->Allocate(shape, out.gpu, why))
					return false;
				const pl_tex dst = static_cast<pl_tex>(out.gpu.resource);
				const pl_dispatch dispatch = static_cast<pl_dispatch>(req.gpu->Dispatch());
				const P1Gpu::PyramidPassRunner run = [dispatch](const P1Gpu::PyramidPass& p) { return RunPyramidPass(dispatch, p); };
				if (law.twoByTwo)
					return P1Gpu::BuildPyramidInto(run, in, P1Gpu::MatcherCodes(!law.rangeCodes, fullRange), dst, nullptr, nullptr, -1, 0);
				if (level == 3 || level == 4)
					return P1Gpu::BuildPyramidInto(run, nullptr, 0.0f, in, dst, nullptr, 0, 1);
				return P1Gpu::BuildPyramidInto(run, nullptr, 0.0f, nullptr, in, dst, 1, 2);
			};
			return f;
		}

		struct ProductRig
		{
			int hostTag = 0;
			GpuDemand demand;
			TestOwnerDevice device;
			std::shared_ptr<SkeletonMemo> memo = std::make_shared<SkeletonMemo>();
			const void* Host() const { return &hostTag; }
			ProductRig(pl_gpu gpu, pl_dispatch dispatch, uint32_t w, uint32_t h)
			{
				Vpv::Hub::ExtensionHub::Instance().ResetModulesForTesting();
				device.gpu = gpu;
				device.dispatch = dispatch;
				memo->SetHost(Host());
				memo->SetRenderThread(std::this_thread::get_id());
				memo->SetGpuDispatcherPresent(true);
				memo->SetGpuDevice(&device);
				memo->SetGpuBudget(true, 1ull << 30);
				Assert::IsTrue(memo->RegisterOwnerFill(VpSkeleton::kP13SourceRing, AcquireForm::Gpu, Vpv::Capture::SourcePlanes::kUnit,
					Vpv::Capture::SourcePlanes::MakeOwnerFill()) == SkeletonMemo::Registration::Registered, L"P13 owner");
				VpSkeleton::Dims luma;
				luma.width = w;
				luma.height = h;
				const uint64_t reserve = VpSkeleton::VariantReserveBytes(P1Gpu::MatcherPyramidKey(1), luma, 1, P1Gpu::MatcherStorageFormat());
				Assert::IsTrue(memo->RegisterOwnerFill(VpSkeleton::kP1HalfLuma, AcquireForm::Gpu, "input/source_analysis/lane", StandInP1Fill(reserve))
					== SkeletonMemo::Registration::Registered, L"P1 GPU owner (stand-in)");
				demand.memo = memo;
				demand.host = Host();
				Vpv::Hub::ExtensionHub::Instance().AddModule(std::unique_ptr<Vpv::Hub::ISkeletonModule>(new GpuSkeleton(&demand)));
			}
			~ProductRig()
			{
				Vpv::Capture::SourcePlanes::Forget(Host());
				Vpv::Hub::ExtensionHub::Instance().ResetModulesForTesting();
			}
		};

		// Any sampleable output, copied texel for texel into RGBA32F (exact for the matcher's float / unorm outputs).
		std::vector<float> Snapshot(pl_gpu gpu, pl_dispatch dispatch, pl_tex src)
		{
			if (src == nullptr)
				return std::vector<float>();
			const enum pl_fmt_caps caps = static_cast<enum pl_fmt_caps>(PL_FMT_CAP_STORABLE | PL_FMT_CAP_HOST_READABLE);
			pl_tex_params tp{};
			tp.w = src->params.w;
			tp.h = src->params.h;
			tp.format = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32, caps);
			tp.storable = true;
			tp.host_readable = true;
			Assert::IsNotNull(tp.format, L"no RGBA32F storable host-readable format");
			pl_tex dst = pl_tex_create(gpu, &tp);
			Assert::IsNotNull(dst, L"snapshot texture");
			pl_shader_desc d[2]{};
			d[0].desc.name = "src";
			d[0].desc.type = PL_DESC_SAMPLED_TEX;
			d[0].binding.object = src;
			d[0].binding.address_mode = PL_TEX_ADDRESS_CLAMP;
			d[0].binding.sample_mode = PL_TEX_SAMPLE_NEAREST;
			d[1].desc.name = "dst";
			d[1].desc.type = PL_DESC_STORAGE_IMG;
			d[1].desc.access = PL_DESC_ACCESS_WRITEONLY;
			d[1].binding.object = dst;
			pl_custom_shader custom{};
			custom.description = "VideoProcessor matcher pyramid identity test: snapshot";
			custom.body = "ivec2 p = ivec2(gl_GlobalInvocationID.xy);\n"
				"ivec2 size = imageSize(dst);\n"
				"if (p.x < size.x && p.y < size.y)\n"
				"    imageStore(dst, p, texelFetch(src, p, 0));\n";
			custom.input = PL_SHADER_SIG_NONE;
			custom.output = PL_SHADER_SIG_NONE;
			custom.descriptors = d;
			custom.num_descriptors = 2;
			custom.compute = true;
			custom.compute_group_size[0] = 8;
			custom.compute_group_size[1] = 8;
			pl_shader sh = pl_dispatch_begin(dispatch);
			Assert::IsTrue(pl_shader_custom(sh, &custom), L"snapshot shader");
			pl_dispatch_compute_params compute{};
			compute.shader = &sh;
			compute.dispatch_size[0] = (tp.w + 7) / 8;
			compute.dispatch_size[1] = (tp.h + 7) / 8;
			compute.dispatch_size[2] = 1;
			Assert::IsTrue(pl_dispatch_compute(dispatch, &compute), L"snapshot dispatch");
			std::vector<float> v(size_t(tp.w) * tp.h * 4);
			pl_tex_transfer_params dl{};
			dl.tex = dst;
			dl.ptr = v.data();
			Assert::IsTrue(pl_tex_download(gpu, &dl), L"snapshot download");
			pl_tex_destroy(gpu, &dst);
			return v;
		}

		enum class Arm { Product, Forced, NoSeam };

		struct ArmResult
		{
			std::vector<float> forward, backward, field, global;
			int productEstimates = 0;
			int innerProductEstimates = 0;
			std::string why;
		};

		bool SameBytes(const std::vector<float>& a, const std::vector<float>& b)
		{
			return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);
		}

		ArmResult RunArm(TargetLutGpuFixture& fixture, Arm arm, int w, int h, bool lowRes)
		{
			pl_gpu gpu = fixture.Gpu();
			struct DispatchHolder
			{
				pl_dispatch d;
				~DispatchHolder() { pl_dispatch_destroy(&d); }
			} dispatch{ pl_dispatch_create(gpu->log, gpu) };
			DejudderGpuPlanes frames[3];
			for (int i = 0; i < 3; ++i)
			{
				const int dx = 12 * i, dy = 6 * i;
				const DejudderFrame f = MakeDejudderFrame(w, h, [dx, dy](int x, int y) { return DejudderTexture(x - dx, y - dy); },
					[dx, dy](int x, int y, int c) { return DejudderTexture((x - dx / 2) * 2, y - dy, 7 + c); });
				UploadDejudderFrame(gpu, f, frames[i]);
				frames[i].planes.sequence = uint64_t(i + 1);
				frames[i].planes.fullRange = false;   // YUV limited
			}
			ProductRig rig(gpu, dispatch.d, uint32_t(w), uint32_t(h));
			VpMotion::VectorEngine engine(rig.Host());
			Dejudder::Settings settings = DejudderTestSettings();
			settings.hostReadable = false;   // a host-readable matcher declines the product (its own format); outputs are snapshotted
			settings.globalPrior = true;     // a global vector to compare
			settings.lowRes = lowRes;
			Dejudder::MotionWarp warp(fixture.Log(), gpu, settings);
			ArmResult out;
			for (int i = 0; i < 2; ++i)
			{
				const Dejudder::Planes& a = frames[i].planes;
				const Dejudder::Planes& b = frames[i + 1].planes;
				Vpv::Capture::SourcePlanes::BeginFrame(rig.Host());
				if (arm == Arm::Product)
				{
					Assert::IsTrue(engine.OfferMatcherPlanes(a.sequence, a.luma, a.chroma, a.fullRange), L"A offered");
					Assert::IsTrue(engine.OfferMatcherPlanes(b.sequence, b.luma, b.chroma, b.fullRange), L"B offered");
					warp.SetPyramidSource(engine.MatcherPyramidSource());
				}
				else if (arm == Arm::Forced)
				{
					warp.SetPyramidSource([](const VpMotion::MatcherPyramid::Request&) {
						VpMotion::MatcherPyramid::Level l;
						l.state = VpMotion::MatcherPyramid::Level::State::Unavailable;
						l.cause = "forced fallback (test)";
						return l;
					});
				}
				Assert::IsTrue(warp.Estimate(a, b), L"estimate");
				out.productEstimates += warp.PyramidFromProduct() ? 1 : 0;
				out.innerProductEstimates += warp.InnerPyramidFromProduct() ? 1 : 0;
				out.why = warp.PyramidFallbackWhy();
			}
			out.forward = Snapshot(gpu, dispatch.d, warp.Forward());
			out.backward = Snapshot(gpu, dispatch.d, warp.Backward());
			out.field = Snapshot(gpu, dispatch.d, warp.Field());
			out.global = Snapshot(gpu, dispatch.d, warp.GlobalVector());
			return out;
		}

		void CheckIdentity(int w, int h, bool lowRes)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			const ArmResult product = RunArm(fixture, Arm::Product, w, h, lowRes);
			const ArmResult forced = RunArm(fixture, Arm::Forced, w, h, lowRes);
			const ArmResult none = RunArm(fixture, Arm::NoSeam, w, h, lowRes);
			Assert::AreEqual(2, product.productEstimates, (L"the product was USED on both estimates; fallback: "
				+ std::wstring(product.why.begin(), product.why.end())).c_str());
			Assert::IsTrue(product.why.empty(), L"product arm: no fallback row");
			if (lowRes)
				Assert::AreEqual(2, product.innerProductEstimates, L"lowRes: the inner read levels 2 / 4 / 6 from the product");
			Assert::AreEqual(0, forced.productEstimates, L"forced arm: never bound");
			Assert::AreEqual(std::string("forced fallback (test)"), forced.why, L"forced arm: the row names the cause");
			Assert::AreEqual(0, none.productEstimates, L"no seam: never bound");
			Assert::IsTrue(none.why.empty(), L"no seam: silent (nothing to fall back from)");
			Assert::IsFalse(product.forward.empty() || product.field.empty(), L"non-vacuous: the matcher produced vectors");
			Assert::IsTrue(SameBytes(product.forward, forced.forward) && SameBytes(product.forward, none.forward), L"Forward byte-equal");
			Assert::IsTrue(SameBytes(product.backward, forced.backward) && SameBytes(product.backward, none.backward), L"Backward byte-equal");
			Assert::IsTrue(SameBytes(product.field, forced.field) && SameBytes(product.field, none.field), L"Field byte-equal");
			Assert::IsTrue(SameBytes(product.global, forced.global) && SameBytes(product.global, none.global), L"GlobalVector byte-equal");
		}
	}

	TEST_CLASS(MatcherPyramidGpuIdentityTests)
	{
	public:
		BEGIN_TEST_METHOD_ATTRIBUTE(YuvLimitedProductEqualsOwnPyramid)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(YuvLimitedProductEqualsOwnPyramid)
		{
			CheckIdentity(512, 256, false);
		}

		// lowRes (height >= 1528: the matcher searches on its half): the inner reads levels 2 / 4 / 6 of the same frames.
		BEGIN_TEST_METHOD_ATTRIBUTE(LowResInnerProductEqualsOwnPyramid)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(LowResInnerProductEqualsOwnPyramid)
		{
			CheckIdentity(2048, 1536, true);
		}
	};
}
