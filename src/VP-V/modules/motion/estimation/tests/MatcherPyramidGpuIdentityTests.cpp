// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// L-B4 (BBP-B B4, run in B5): the matcher reading the SHARED luma pyramid (kP1HalfLuma, Gpu, MatcherPyramidKey(L)) gives the same
// motion outputs, byte for byte and estimate by estimate, as the matcher building its own pyramid (the visible fallback, row
// product.motion_warp) and as the matcher without a seam at all (the pre-B3 behaviour).
// Wiring = production: a skeleton memo with the PRODUCTION owner fill P1Gpu::MatcherPyramidOwnerFill(reserve) (the fill the P1
// module registers; only its GPU device is the test's, in SkeletonGpuDispatcher's role), the P13 owner fill
// (SourcePlanes::MakeOwnerFill), and per render frame the production order: memo->OnSeal(render seq), SourcePlanes::BeginFrame,
// the Entrance offer of the render frame (its P13 = the Entrance image), NoteMatcherEpoch, the matcher offering B only
// (VectorEngine::OfferMatcherPlanes; the ahead arm offers A + B), the engine's read seam (VectorEngine::MatcherPyramidSource).
// So A of estimate n >= 2 is the product made when it was B (keepFrames 1, kept across the seal), the real path of 115 MOTION 1.
//   estimate 1: a visible own build with the first-pair cause; estimates 2..: PyramidFromProduct() and no fallback row.
//   forced arm: a seam answering Unavailable; no-seam arm: SetPyramidSource never called.
//   alternation (LOW-3): one MotionWarp bound -> forced -> bound, every estimate byte-equal to the no-seam arm.
//   lowRes (height >= 1528): the inner reads levels 2 / 4 / 6; a seam declining only those shows "lowRes inner: ..." (MED-1).
// Outputs compared: Forward, Backward, Field, GlobalVector, copied to RGBA32F after every estimate (the product path requires
// settings.hostReadable = false, so the matcher's own textures are not host-readable). WARP unless VP_TEST_GPU=hardware;
// category GpuHardware (the B5 gate run).
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

		struct ProductRig
		{
			int hostTag = 0;
			GpuDemand demand;
			TestOwnerDevice device;
			std::shared_ptr<SkeletonMemo> memo = std::make_shared<SkeletonMemo>();
			VpSkeleton::Dims luma;       // the source luma the module would have observed (the largest)
			VpSkeleton::OwnerFill fill;  // the production P1 GPU owner fill, as registered
			const void* Host() const { return &hostTag; }
			ProductRig(pl_gpu gpu, pl_dispatch dispatch, uint32_t w, uint32_t h, uint16_t aheadDepth = P1Gpu::kMatcherStructuralAhead)
			{
				Vpv::Hub::ExtensionHub::Instance().ResetModulesForTesting();
				device.gpu = gpu;
				device.dispatch = dispatch;
				memo->SetHost(Host());
				memo->SetRenderThread(std::this_thread::get_id());
				memo->SetGpuDispatcherPresent(true);
				memo->SetGpuDevice(&device);
				Assert::IsTrue(memo->RegisterOwnerFill(VpSkeleton::kP13SourceRing, AcquireForm::Gpu, Vpv::Capture::SourcePlanes::kUnit,
					Vpv::Capture::SourcePlanes::MakeOwnerFill()) == SkeletonMemo::Registration::Registered, L"P13 owner");
				// GF-P1 MUST-2: the P1 module sizes its fill from the largest observed source luma (reserveBytes = the per-frame
				// working set, reserveFor = the exact size of the asked level); the test registers the same fill with luma = w x h.
				luma.width = w;
				luma.height = h;
				fill = P1Gpu::MatcherPyramidOwnerFill(luma, aheadDepth);
				Assert::IsTrue(memo->RegisterOwnerFill(VpSkeleton::kP1HalfLuma, AcquireForm::Gpu, "input/source_analysis/lane",
					fill) == SkeletonMemo::Registration::Registered, L"the production P1 GPU owner fill");
				// Wire the queue window (matching SkeletonModule::DeriveMemoWindow) so GpuNeedBytes() includes the fill's ahead depth.
				VpSkeleton::QueueWindowTerms wt;
				wt.history = fill.DeclaredHistory();
				wt.lookAheadWalk = aheadDepth;
				wt.aheadNeed = fill.aheadDepth;
				wt.aheadNeedBy = "matcher";
				wt.wired = VpSkeleton::QueueWindowTerms::kHistory | VpSkeleton::QueueWindowTerms::kLookAhead
					| VpSkeleton::QueueWindowTerms::kAheadNeed;
				memo->SetWindow(VpSkeleton::DeriveWindow(wt, 0, 0));
				// The ledger grant is the memo's own planned need (what the module's grant gives the memo), not a bare 1 GiB.
				memo->SetGpuBudget(true, memo->GpuNeedBytes());
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

		const char* const kFirstPair = "first pair after a start or a gap: A has no kept product yet";
		const char* const kForced = "forced fallback (test)";
		const char* const kInnerDeclined = "inner declined (test)";

		// What one estimate of an arm does with the seam.
		enum class Mode
		{
			Engine,             // production: B offered, the engine's seam
			EngineAhead,        // the ahead pair: A and B offered, the engine's seam
			EngineInnerDecline, // the engine's seam, levels 2 / 4 / 6 (the lowRes inner's) answered Unavailable
			Forced,             // B offered, a seam answering Unavailable for every level
			NoSeam              // B offered, SetPyramidSource never called (pre-B3)
		};

		struct EstimateResult
		{
			std::vector<float> forward, backward, field, global;
			bool bound = false;
			bool innerBound = false;
			std::string why;
		};

		bool SameBytes(const std::vector<float>& a, const std::vector<float>& b)
		{
			return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);
		}

		VpMotion::MatcherPyramid::Level Declined(const char* cause)
		{
			VpMotion::MatcherPyramid::Level l;
			l.state = VpMotion::MatcherPyramid::Level::State::Unavailable;
			l.cause = cause;
			return l;
		}

		// What the P1 ledger refused over an arm (GF-P1 MUST-2): the two budget causes, read from the memo before the rig goes.
		struct ArmLedger
		{
			uint64_t overBudget = 0;
			uint64_t noLedgerGrant = 0;
			uint64_t made = 0;                 // frames the owner fill published (filled at read)
			uint64_t reserveLevel1 = 0;        // fill.reserveFor(level 1)
			uint64_t reserveLevel6 = 0;        // fill.reserveFor(level 6)
			uint64_t reserveBytes = 0;         // fill.reserveBytes
			uint64_t expectedReserveBytes = 0; // MatcherPyramidReserveBytes(luma)
		};

		std::vector<EstimateResult> RunArm(TargetLutGpuFixture& fixture, const std::vector<Mode>& modes, int w, int h, bool lowRes,
			ArmLedger* ledger = nullptr, uint16_t aheadDepth = P1Gpu::kMatcherStructuralAhead)
		{
			pl_gpu gpu = fixture.Gpu();
			struct DispatchHolder
			{
				pl_dispatch d;
				~DispatchHolder() { pl_dispatch_destroy(&d); }
			} dispatch{ pl_dispatch_create(gpu->log, gpu) };
			const size_t n = modes.size();
			std::vector<std::unique_ptr<DejudderGpuPlanes>> frames;
			for (size_t i = 0; i <= n; ++i)
			{
				const int dx = 12 * int(i), dy = 6 * int(i);
				const DejudderFrame f = MakeDejudderFrame(w, h, [dx, dy](int x, int y) { return DejudderTexture(x - dx, y - dy); },
					[dx, dy](int x, int y, int c) { return DejudderTexture((x - dx / 2) * 2, y - dy, 7 + c); });
				frames.emplace_back(new DejudderGpuPlanes());
				UploadDejudderFrame(gpu, f, *frames.back());
				frames.back()->planes.sequence = uint64_t(i + 1);
				frames.back()->planes.fullRange = false;   // YUV limited
			}
			ProductRig rig(gpu, dispatch.d, uint32_t(w), uint32_t(h), aheadDepth);
			VpMotion::VectorEngine engine(rig.Host());
			Dejudder::Settings settings = DejudderTestSettings();
			settings.hostReadable = false;   // a host-readable matcher declines the product (its own format); outputs are snapshotted
			settings.globalPrior = true;     // a global vector to compare
			settings.lowRes = lowRes;
			Dejudder::MotionWarp warp(fixture.Log(), gpu, settings);
			const VpMotion::MatcherPyramid::Source engineSeam = engine.MatcherPyramidSource();
			std::vector<EstimateResult> out;
			for (size_t i = 0; i < n; ++i)
			{
				const Dejudder::Planes& a = frames[i]->planes;   // the render frame
				const Dejudder::Planes& b = frames[i + 1]->planes;
				// The render frame's Begin: the seal and the last frame's offers dropped. Its Entrance image is offered only AFTER the
				// estimate, as in production (NotifyEntrance follows UpdateDejudderEstimate): A can never be filled from it.
				rig.memo->OnSeal(a.sequence);
				Vpv::Capture::SourcePlanes::BeginFrame(rig.Host());
				pl_frame image{};
				image.num_planes = 2;
				image.planes[0].texture = a.luma;
				image.planes[0].components = 1;
				image.planes[1].texture = a.chroma;
				image.planes[1].components = 2;
				image.repr.sys = PL_COLOR_SYSTEM_BT_709;
				image.repr.levels = PL_COLOR_LEVELS_LIMITED;
				// RunDejudderEstimate's order: the epoch, the offers, the seam.
				engine.NoteMatcherEpoch(1);
				if (modes[i] == Mode::EngineAhead)
					Assert::IsTrue(engine.OfferMatcherPlanes(a.sequence, a.luma, a.chroma, a.fullRange), L"ahead: A offered");
				Assert::IsTrue(engine.OfferMatcherPlanes(b.sequence, b.luma, b.chroma, b.fullRange), L"B offered");
				switch (modes[i])
				{
				case Mode::Engine:
				case Mode::EngineAhead:
					warp.SetPyramidSource(engineSeam);
					break;
				case Mode::EngineInnerDecline:
					warp.SetPyramidSource([engineSeam](const VpMotion::MatcherPyramid::Request& q) {
						return q.level == 2 || q.level == 4 || q.level == 6 ? Declined(kInnerDeclined) : engineSeam(q);
					});
					break;
				case Mode::Forced:
					warp.SetPyramidSource([](const VpMotion::MatcherPyramid::Request&) { return Declined(kForced); });
					break;
				case Mode::NoSeam:
					break;
				}
				Assert::IsTrue(warp.Estimate(a, b), L"estimate");
				Assert::IsTrue(Vpv::Capture::SourcePlanes::OfferFrame(rig.Host(), a.sequence, int64_t(i + 1), image), L"Entrance offer (after the estimate)");
				EstimateResult r;
				r.bound = warp.PyramidFromProduct();
				r.innerBound = warp.InnerPyramidFromProduct();
				r.why = warp.PyramidFallbackWhy();
				r.forward = Snapshot(gpu, dispatch.d, warp.Forward());
				r.backward = Snapshot(gpu, dispatch.d, warp.Backward());
				r.field = Snapshot(gpu, dispatch.d, warp.Field());
				r.global = Snapshot(gpu, dispatch.d, warp.GlobalVector());
				out.push_back(std::move(r));
			}
			if (ledger != nullptr)
			{
				const VpSkeleton::AcquireCounters c = rig.memo->CountersOf(VpSkeleton::kP1HalfLuma);
				ledger->overBudget = c.unavailable[size_t(VpSkeleton::AcquireCause::OverBudget)];
				ledger->noLedgerGrant = c.unavailable[size_t(VpSkeleton::AcquireCause::NoLedgerGrant)];
				ledger->made = c.filledAtRead;
				Assert::IsTrue(static_cast<bool>(rig.fill.reserveFor), L"the production fill declares reserveFor");
				ledger->reserveLevel1 = rig.fill.reserveFor(P1Gpu::MatcherPyramidKey(1));
				ledger->reserveLevel6 = rig.fill.reserveFor(P1Gpu::MatcherPyramidKey(6));
				ledger->reserveBytes = rig.fill.reserveBytes;
				ledger->expectedReserveBytes = P1Gpu::MatcherPyramidReserveBytes(rig.luma);
			}
			return out;
		}

		std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }

		void CheckSameOutputs(const std::vector<EstimateResult>& x, const std::vector<EstimateResult>& ref, const wchar_t* arm)
		{
			Assert::AreEqual(ref.size(), x.size(), arm);
			for (size_t i = 0; i < x.size(); ++i)
			{
				const std::wstring at = std::wstring(arm) + L", estimate " + std::to_wstring(i + 1) + L": ";
				Assert::IsFalse(ref[i].forward.empty() || ref[i].field.empty(), (at + L"non-vacuous: the matcher produced vectors").c_str());
				Assert::IsTrue(SameBytes(x[i].forward, ref[i].forward), (at + L"Forward byte-equal").c_str());
				Assert::IsTrue(SameBytes(x[i].backward, ref[i].backward), (at + L"Backward byte-equal").c_str());
				Assert::IsTrue(SameBytes(x[i].field, ref[i].field), (at + L"Field byte-equal").c_str());
				Assert::IsTrue(SameBytes(x[i].global, ref[i].global), (at + L"GlobalVector byte-equal").c_str());
			}
		}

		void CheckUsedAfterTheFirstPair(const std::vector<EstimateResult>& r, bool lowRes)
		{
			Assert::IsFalse(r[0].bound, L"estimate 1: A has no kept product: own build");
			Assert::AreEqual(std::string(kFirstPair), r[0].why, L"estimate 1: visible, with the first-pair cause");
			for (size_t i = 1; i < r.size(); ++i)
			{
				Assert::IsTrue(r[i].bound, (L"estimate " + std::to_wstring(i + 1) + L": the product was USED; fallback: " + Wide(r[i].why)).c_str());
				Assert::IsTrue(r[i].why.empty(), L"no fallback row while bound");
				if (lowRes)
					Assert::IsTrue(r[i].innerBound, L"lowRes: the inner read levels 2 / 4 / 6 from the product");
			}
		}

		void CheckArms(int w, int h, bool lowRes)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			const std::vector<EstimateResult> none = RunArm(fixture, { Mode::NoSeam, Mode::NoSeam, Mode::NoSeam }, w, h, lowRes);
			const std::vector<EstimateResult> product = RunArm(fixture, { Mode::Engine, Mode::Engine, Mode::Engine }, w, h, lowRes);
			const std::vector<EstimateResult> ahead = RunArm(fixture, { Mode::EngineAhead, Mode::EngineAhead, Mode::EngineAhead }, w, h, lowRes);
			const std::vector<EstimateResult> forced = RunArm(fixture, { Mode::Forced, Mode::Forced, Mode::Forced }, w, h, lowRes);
			for (const EstimateResult& r : none)
				Assert::IsTrue(!r.bound && r.why.empty(), L"no seam: never bound, silent (nothing to fall back from)");
			CheckUsedAfterTheFirstPair(product, lowRes);
			for (const EstimateResult& r : ahead)
				Assert::IsTrue(r.bound && r.why.empty(), (L"ahead (A offered): bound on every estimate; fallback: " + Wide(r.why)).c_str());
			for (const EstimateResult& r : forced)
				Assert::IsTrue(!r.bound && r.why == kForced, L"forced: own build, the row names the cause");
			CheckSameOutputs(product, none, L"product");
			CheckSameOutputs(ahead, none, L"ahead");
			CheckSameOutputs(forced, none, L"forced");
		}
	}

	TEST_CLASS(MatcherPyramidGpuIdentityTests)
	{
	public:
		// MUST-1: production fill and order, YUV limited: product / ahead / forced / no seam, estimate by estimate.
		BEGIN_TEST_METHOD_ATTRIBUTE(YuvLimitedProductEqualsOwnPyramid)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(YuvLimitedProductEqualsOwnPyramid)
		{
			CheckArms(512, 256, false);
		}

		// lowRes (height >= 1528: the matcher searches on its half): the inner reads levels 2 / 4 / 6 of the same frames.
		BEGIN_TEST_METHOD_ATTRIBUTE(LowResInnerProductEqualsOwnPyramid)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(LowResInnerProductEqualsOwnPyramid)
		{
			CheckArms(2048, 1536, true);
		}

		// GF-P1 MUST-2 (S14.2a): the memo's ledger grant is its OWN planned need (GpuNeedBytes, from the fill's reserveBytes), not a
		// bare 1 GiB. Over 6 frames of the production order (normal = levels 1 / 3 / 5, lowRes = adds 2 / 4 / 6, and the EstimateAhead
		// pair) no Acquire of the pyramid is refused for the budget (0 OverBudget, 0 NoLedgerGrant) and every estimate after the
		// first pair is served from the product; the per-level reserve is the exact size of the asked level (level 6 < level 1) and
		// reserveBytes is the largest luma's per-frame working set.
		BEGIN_TEST_METHOD_ATTRIBUTE(MatcherPyramidBudgetFromNeedServesEveryFrame)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(MatcherPyramidBudgetFromNeedServesEveryFrame)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			const std::vector<Mode> engine(6, Mode::Engine);
			const std::vector<Mode> ahead(6, Mode::EngineAhead);
			ArmLedger normalLedger, lowResLedger, aheadLedger, normalZeroLedger, lowResZeroLedger;
			const std::vector<EstimateResult> normal = RunArm(fixture, engine, 512, 256, false, &normalLedger);
			const std::vector<EstimateResult> lowRes = RunArm(fixture, engine, 2048, 1536, true, &lowResLedger);
			const std::vector<EstimateResult> pair = RunArm(fixture, ahead, 512, 256, false, &aheadLedger);
			// R1 fix 3: the same arms registered with aheadDepth 1 (exact 1-frame structural ahead for B = N + 1, zero extra slack
			// in the memo's need: 3 live frames {N-1, N, N+1} fill lowRes's 3 * reserveBytes to the byte; a MUST-2 revert goes over budget).
			const std::vector<EstimateResult> normalZero = RunArm(fixture, engine, 512, 256, false, &normalZeroLedger, 1);
			const std::vector<EstimateResult> lowResZero = RunArm(fixture, engine, 2048, 1536, true, &lowResZeroLedger, 1);
			CheckUsedAfterTheFirstPair(normal, false);
			CheckUsedAfterTheFirstPair(lowRes, true);
			CheckUsedAfterTheFirstPair(normalZero, false);
			CheckUsedAfterTheFirstPair(lowResZero, true);
			for (const EstimateResult& r : pair)
				Assert::IsTrue(r.bound && r.why.empty(), (L"ahead pair (A offered): served; fallback: " + Wide(r.why)).c_str());
			const ArmLedger* const ledgers[5] = { &normalLedger, &lowResLedger, &aheadLedger, &normalZeroLedger, &lowResZeroLedger };
			const wchar_t* const names[5] = { L"normal", L"lowRes", L"ahead pair", L"normal, aheadDepth 1", L"lowRes, aheadDepth 1" };
			for (int i = 0; i < 5; ++i)
			{
				const std::wstring at = std::wstring(names[i]) + L": ";
				Assert::AreEqual(uint64_t(0), ledgers[i]->overBudget, (at + L"0 OverBudget").c_str());
				Assert::AreEqual(uint64_t(0), ledgers[i]->noLedgerGrant, (at + L"0 NoLedgerGrant (the grant is the memo's need)").c_str());
				Assert::IsTrue(ledgers[i]->made > 0, (at + L"the owner fill published (non-vacuous)").c_str());
				Assert::IsTrue(ledgers[i]->reserveLevel6 < ledgers[i]->reserveLevel1, (at + L"reserveFor(level 6) < reserveFor(level 1)").c_str());
				Assert::IsTrue(ledgers[i]->expectedReserveBytes > 0, (at + L"the working set of a luma is non-zero").c_str());
				Assert::AreEqual(ledgers[i]->expectedReserveBytes, ledgers[i]->reserveBytes, (at + L"reserveBytes == MatcherPyramidReserveBytes(luma)").c_str());
			}
		}

		// SHOULD-1 (LOW-3): one MotionWarp alternating bound -> forced fallback -> bound (A's own levels and the K set freed while
		// bound, made again on the fallback): no failure, every estimate byte-equal to the no-seam arm.
		BEGIN_TEST_METHOD_ATTRIBUTE(BoundForcedBoundAlternationEqualsOwnPyramid)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(BoundForcedBoundAlternationEqualsOwnPyramid)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			const std::vector<Mode> modes = { Mode::Engine, Mode::Engine, Mode::Forced, Mode::Engine, Mode::Engine };
			const std::vector<EstimateResult> none = RunArm(fixture, std::vector<Mode>(modes.size(), Mode::NoSeam), 512, 256, false);
			const std::vector<EstimateResult> alt = RunArm(fixture, modes, 512, 256, false);
			// Estimate 4's A was the forced estimate's B, whose product was never asked: no kept product, an own build (visible),
			// then bound again at 5. The Entrance image of A comes after the estimate, so A is never filled from it.
			const bool wantBound[5] = { false, true, false, false, true };
			for (size_t i = 0; i < alt.size(); ++i)
				Assert::AreEqual(wantBound[i], alt[i].bound, (L"estimate " + std::to_wstring(i + 1) + L" bound: " + Wide(alt[i].why)).c_str());
			Assert::AreEqual(std::string(kForced), alt[2].why, L"the forced estimate is visible");
			Assert::IsFalse(alt[3].why.empty(), L"estimate 4 (no kept A after the forced estimate) builds its own, visibly");
			Assert::IsTrue(alt[4].why.empty(), L"bound again: the row released");
			CheckSameOutputs(alt, none, L"alternation");
		}

		// SHOULD-2 (MED-1): lowRes with a seam that declines only the inner's levels 2 / 4 / 6: the outer is bound, the inner builds its
		// own and says so ("lowRes inner: <cause>"); outputs byte-equal to the no-seam arm.
		BEGIN_TEST_METHOD_ATTRIBUTE(LowResInnerDeclineIsVisibleAndEqualsOwnPyramid)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(LowResInnerDeclineIsVisibleAndEqualsOwnPyramid)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			const std::vector<EstimateResult> none = RunArm(fixture, { Mode::NoSeam, Mode::NoSeam, Mode::NoSeam }, 2048, 1536, true);
			const std::vector<EstimateResult> inner = RunArm(fixture, { Mode::EngineInnerDecline, Mode::EngineInnerDecline,
				Mode::EngineInnerDecline }, 2048, 1536, true);
			Assert::AreEqual(std::string(kFirstPair), inner[0].why, L"estimate 1: the outer's first-pair cause");
			for (size_t i = 1; i < inner.size(); ++i)
			{
				Assert::IsTrue(inner[i].bound, (L"the outer is bound; fallback: " + Wide(inner[i].why)).c_str());
				Assert::IsFalse(inner[i].innerBound, L"the inner is not");
				Assert::AreEqual(std::string("lowRes inner: ") + kInnerDeclined, inner[i].why, L"the inner's decline is visible");
			}
			CheckSameOutputs(inner, none, L"lowRes inner decline");
		}
	};
}