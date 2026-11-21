// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "CppUnitTest.h"

// repair.grain_split: the kP10gGrainLevel OWNER FILL (GrainLevelGpu.h, P10g T3) on the CPU tier, no GPU object touched (textures
// are plain structs, the device is a test double that makes no texture):
//   1. the reserve is DERIVED from kP10gGrainLevel's declared size rule (one RGBA32F texel per 16-px block) at the source luma size;
//   2. the fill is Derived, frame-scoped, default variant; the module registers BOTH fills (the P10 variant owner once, P10g at the
//      largest live size) under the catalog's owning unit, re-registers P10g only on a size change, never the P10 owner twice;
//   3. the visible declines: no GPU context, no source plane (P13), the P10 split unavailable; each is counted for the status line;
//   4. the Fallbacks texts (the route that reaches Ctrl+I, Config status, Ctrl+Alt+D) say the single-frame law is in use, name the
//      declines with the last reason, and show a missing registration; an AlreadyRegistered P10 owner (15b A1) is benign.
// The GPU parity test (fill vs GrainSplit's CPU blocks and the f^2 block mean) is part of the T5 GPU run (not run here: CODE ONLY).

#include <VP-V/core/GpuAllocator.h>
#include <VP-V/core/Products.h>
#include <VP-V/core/SkeletonAcquire.h>
#include <VP-V/modules/input/capture/SourcePlanes.h>
#include <VP-V/modules/repair/grain_split/GrainLevelGpu.h>

#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	namespace
	{
		// Forwards the module's registrations to one real memo (the catalog's ownership rules apply) and counts them.
		struct GrainMemoDemand : Vpv::Skeleton::IExternalDemand
		{
			VpSkeleton::SkeletonMemo* memo = nullptr;
			int registers = 0;
			int unregisters = 0;
			int splitRegisters = 0;
			bool refuseSplit = false;   // answer the P10 owner NotOwner without forwarding (a refused split owner)
			VpSkeleton::OwnerFill lastLevelFill;   // a copy of the last P10g fill registered (the module's counters ride in it)
			void SetExternalDemand(const void*, const void*, const std::vector<VpSkeleton::ConsumerDecl>&,
				const std::vector<VpSkeleton::ProductId>&) override {}
			void ClearExternalDemand(const void*, const void*) override {}
			VpSkeleton::SkeletonMemo::Registration RegisterOwnerFill(VpSkeleton::ProductId id, VpSkeleton::AcquireForm form,
				const std::string& unit, VpSkeleton::OwnerFill fill) override
			{
				++registers;
				if (id == VpSkeleton::kP10gGrainLevel)
					lastLevelFill = fill;
				if (id == VpSkeleton::kP10GrainSplit)
				{
					++splitRegisters;
					if (refuseSplit)
						return VpSkeleton::SkeletonMemo::Registration::NotOwner;
				}
				return memo->RegisterOwnerFill(id, form, unit, fill);
			}
			bool UnregisterOwnerFill(VpSkeleton::ProductId id, VpSkeleton::AcquireForm form, const std::string& unit) override
			{
				++unregisters;
				return memo->UnregisterOwnerFill(id, form, unit);
			}
		};

		// Non-null opaque handles (the fill only tests them before acquiring its inputs); makes no texture.
		struct GrainRefusingDevice : VpSkeleton::IOwnerGpuDevice
		{
			int gpuTag = 0;
			int dispatchTag = 0;
			int asked = 0;
			void* OwnerGpu() const override { return const_cast<int*>(&gpuTag); }
			void* OwnerDispatch() const override { return const_cast<int*>(&dispatchTag); }
			bool CreateOwnerTexture(const VpSkeleton::TexShape&, VpSkeleton::GpuMemoHandle&, std::string& why) override
			{
				++asked;
				why = "test device makes no texture";
				return false;
			}
		};

		VpSkeleton::Dims GrainDims(uint32_t w, uint32_t h)
		{
			VpSkeleton::Dims d;
			d.width = w;
			d.height = h;
			return d;
		}
	}

	TEST_CLASS(GrainLevelFillTests)
	{
	public:
		TEST_METHOD(ReserveIsTheDeclaredBlockGridAtTheSourceLumaSize)
		{
			using namespace VpSkeleton;
			namespace L = GrainSplit::LevelGpu;
			const Dims sizes[3] = { GrainDims(3840, 2160), GrainDims(1920, 1080), GrainDims(1919, 1081) };
			for (const Dims& d : sizes)
			{
				TexShape s;
				s.w = (d.width + 15u) / 16u;
				s.h = (d.height + 15u) / 16u;
				s.fmt = TexFormat::RGBA32F;
				Assert::AreEqual(uint64_t(s.w) * s.h * 16u, L::ReserveBytes(d), L"one RGBA32F texel per 16-px block (ceil)");
				Assert::AreEqual(ShapeBytes(s), L::ReserveBytes(d), L"the reserve equals the texture the fill makes");
			}
			Assert::AreEqual(uint64_t(240) * 135u * 16u, L::ReserveBytes(GrainDims(3840, 2160)), L"518,400 B at 4K");
			Assert::AreEqual(uint64_t(0), L::ReserveBytes(GrainDims(0, 0)), L"no size known: no reserve");
			Assert::AreEqual(uint64_t(0), L::ReserveBytes(GrainDims(1920, 0)), L"an empty plane: no reserve");
		}

		TEST_METHOD(FillIsDerivedFrameScopedDefaultVariantUnderTheCatalogOwner)
		{
			using namespace VpSkeleton;
			namespace L = GrainSplit::LevelGpu;
			const OwnerFill f = L::MakeOwnerFill(L::ReserveBytes(GrainDims(1920, 1080)), std::make_shared<L::Counters>());
			Assert::AreEqual(uint32_t(0), f.variantAxes, L"default variant only");
			Assert::IsTrue(f.frameScoped, L"a per-frame memo");
			Assert::IsTrue(f.jobClass == JobClass::Derived, L"Derived (LaneSpareWork StaticClassOf), never a job");
			Assert::AreEqual(L::ReserveBytes(GrainDims(1920, 1080)), f.reserveBytes, L"the reserve given");
			Assert::AreEqual(std::string(L::kOwnerUnit), SkeletonMemo::OwnerUnitOf(kP10gGrainLevel), L"the catalog's owner unit");
			SkeletonMemo m;
			Assert::IsTrue(m.RegisterOwnerFill(kP10gGrainLevel, AcquireForm::Gpu, L::kOwnerUnit, f) == SkeletonMemo::Registration::Registered,
				L"registered by its unit");
			Assert::IsTrue(m.RegisterOwnerFill(kP10gGrainLevel, AcquireForm::Gpu, "input/source_analysis/regions", f)
				== SkeletonMemo::Registration::NotOwner, L"another unit is refused");
		}

		TEST_METHOD(ModuleRegistersBothFillsAndTracksTheLargestLiveSize)
		{
			using namespace VpSkeleton;
			namespace L = GrainSplit::LevelGpu;
			SkeletonMemo m;
			GrainMemoDemand demand;
			demand.memo = &m;
			L::GrainLevelModule module;
			int hostA = 0, hostB = 0;

			Assert::IsFalse(module.Observe(&hostA, GrainDims(0, 0), &demand), L"no size: nothing registered");
			Assert::AreEqual(0, demand.registers, L"no registration without a size");
			Assert::IsFalse(module.SplitOwnerRegistered(), L"nor the P10 owner");

			Assert::IsTrue(module.Observe(&hostA, GrainDims(1920, 1080), &demand), L"registered on the first size");
			Assert::AreEqual(2, demand.registers, L"the P10 variant owner and the P10g fill");
			Assert::AreEqual(1, demand.splitRegisters, L"the P10 owner once");
			Assert::IsTrue(module.SplitOwnerRegistered(), L"P10 variant owner registered");
			Assert::IsTrue(m.HasOwnerFill(kP10GrainSplit, AcquireForm::Gpu), L"the memo holds the P10 fill");
			Assert::IsTrue(m.HasOwnerFill(kP10gGrainLevel, AcquireForm::Gpu), L"the memo holds the P10g fill");
			Assert::AreEqual(L::ReserveBytes(GrainDims(1920, 1080)), module.RegisteredReserveBytes(), L"1080p reserve");
			Assert::IsFalse(module.SplitOwnerServedElsewhere(), L"the P10 owner is ours here");
			Assert::IsTrue(module.FallbackLines().empty(), L"both registered, no fill yet: no Fallbacks line");

			Assert::IsTrue(module.Observe(&hostA, GrainDims(1920, 1080), &demand), L"same size");
			Assert::AreEqual(2, demand.registers, L"same size: no re-registration");

			Assert::IsTrue(module.Observe(&hostB, GrainDims(3840, 2160), &demand), L"a larger host");
			Assert::AreEqual(1, demand.unregisters, L"the old P10g fill removed");
			Assert::AreEqual(3, demand.registers, L"P10g re-registered once");
			Assert::AreEqual(1, demand.splitRegisters, L"the P10 owner is never registered twice");
			Assert::AreEqual(L::ReserveBytes(GrainDims(3840, 2160)), module.RegisteredReserveBytes(), L"the largest live size");

			module.Forget(&hostB, &demand);
			Assert::AreEqual(4, demand.registers, L"the largest host left: re-registered");
			Assert::AreEqual(L::ReserveBytes(GrainDims(1920, 1080)), module.RegisteredReserveBytes(), L"the remaining host's size");
		}

		TEST_METHOD(ModuleDoesNotRetryARefusedRegistrationEveryFrame)
		{
			using namespace VpSkeleton;
			namespace L = GrainSplit::LevelGpu;
			SkeletonMemo m;
			Assert::IsTrue(m.RegisterOwnerFill(kP10gGrainLevel, AcquireForm::Gpu, L::kOwnerUnit, L::MakeOwnerFill(64, std::make_shared<L::Counters>()))
				== SkeletonMemo::Registration::Registered, L"someone registered P10g first");
			GrainMemoDemand demand;
			demand.memo = &m;
			L::GrainLevelModule module;
			int host = 0;
			Assert::IsFalse(module.Observe(&host, GrainDims(1920, 1080), &demand), L"P10g refused: not registered");
			Assert::AreEqual(2, demand.registers, L"one P10 and one P10g attempt");
			Assert::IsFalse(module.Observe(&host, GrainDims(1920, 1080), &demand), L"the same size again");
			Assert::AreEqual(2, demand.registers, L"not retried every frame");
			Assert::AreEqual(uint64_t(0), module.RegisteredReserveBytes(), L"nothing of ours registered for P10g");
		}

		TEST_METHOD(ModuleShowsARefusedRegistrationOnTheFallbacksRoute)
		{
			using namespace VpSkeleton;
			namespace L = GrainSplit::LevelGpu;
			SkeletonMemo m;
			Assert::IsTrue(m.RegisterOwnerFill(kP10gGrainLevel, AcquireForm::Gpu, L::kOwnerUnit, L::MakeOwnerFill(64, std::make_shared<L::Counters>()))
				== SkeletonMemo::Registration::Registered, L"someone registered P10g first");
			GrainMemoDemand demand;
			demand.memo = &m;
			L::GrainLevelModule module;
			int host = 0;
			Assert::IsFalse(module.Observe(&host, GrainDims(0, 0), &demand), L"no size yet");
			Assert::IsTrue(module.FallbackLines().empty(), L"no size seen: nothing owed, no line");

			Assert::IsFalse(module.Observe(&host, GrainDims(1920, 1080), &demand), L"P10g refused");
			const std::vector<std::string> lines = module.FallbackLines();
			Assert::AreEqual(size_t(1), lines.size(), L"exactly the missing P10g registration (P10 is ours; no fill, no decline)");
			const std::string want = std::string(L::kUnregisteredId) + ": P10g owner fill refused: another fill is already registered (reserve "
				+ std::to_string(L::ReserveBytes(GrainDims(1920, 1080))) + " B; retried at the next size change)";
			Assert::AreEqual(want, lines[0], L"the refusal in words, on a Fallbacks line");

			module.Forget(&host, &demand);
			Assert::IsTrue(module.FallbackLines().empty(), L"the host left: no size, nothing owed");
		}

		TEST_METHOD(ModuleShowsARefusedP10OwnerButNotAnAlreadyRegisteredOne)
		{
			using namespace VpSkeleton;
			namespace L = GrainSplit::LevelGpu;
			int host = 0;
			{
				SkeletonMemo m;
				GrainMemoDemand demand;
				demand.memo = &m;
				demand.refuseSplit = true;
				L::GrainLevelModule module;
				Assert::IsTrue(module.Observe(&host, GrainDims(1920, 1080), &demand), L"P10g registered");
				Assert::IsFalse(module.SplitOwnerRegistered(), L"the P10 owner refused");
				Assert::IsFalse(module.SplitOwnerServedElsewhere(), L"a refusal is not 'served elsewhere'");
				const std::vector<std::string> lines = module.FallbackLines();
				Assert::AreEqual(size_t(1), lines.size(), L"exactly the refused P10 owner");
				Assert::AreEqual(std::string(L::kSplitUnregisteredId) + ": P10 variant owner refused: not the catalog's owning unit"
					" (P10 variant Acquires stay NoOwnerFill; not retried)", lines[0], L"the refusal in words");
				Assert::IsTrue(module.Observe(&host, GrainDims(3840, 2160), &demand), L"a size change");
				Assert::AreEqual(1, demand.splitRegisters, L"a refused P10 owner is not retried");
			}
			{
				// 15b A1 (or anyone) registered the P10 owner first: benign, nothing shown, never retried.
				SkeletonMemo m;
				GrainSplit::Gpu::VariantOwner a1;
				Assert::IsTrue(a1.Register(m) == SkeletonMemo::Registration::Registered, L"the other P10 owner first");
				GrainMemoDemand demand;
				demand.memo = &m;
				L::GrainLevelModule module;
				Assert::IsTrue(module.Observe(&host, GrainDims(1920, 1080), &demand), L"P10g still registered");
				Assert::AreEqual(1, demand.splitRegisters, L"one P10 attempt");
				Assert::IsFalse(module.SplitOwnerRegistered(), L"not ours");
				Assert::IsTrue(module.SplitOwnerServedElsewhere(), L"served by the other registration");
				Assert::IsTrue(m.HasOwnerFill(kP10GrainSplit, AcquireForm::Gpu), L"P10 is served");
				Assert::IsTrue(module.FallbackLines().empty(), L"AlreadyRegistered is benign: no Fallbacks line");
				Assert::IsTrue(module.Observe(&host, GrainDims(3840, 2160), &demand), L"a size change");
				Assert::AreEqual(1, demand.splitRegisters, L"never retried");
			}
		}

		// Category Cpu: the name says GPU, the body is hardware-free (a refusing fake device, no pl_gpu).
		BEGIN_TEST_METHOD_ATTRIBUTE(FillDeclinesVisiblyWithoutAGpuContext)
			TEST_METHOD_ATTRIBUTE(L"Category", L"Cpu")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(FillDeclinesVisiblyWithoutAGpuContext)
		{
			using namespace VpSkeleton;
			namespace L = GrainSplit::LevelGpu;
			SkeletonMemo m;
			const std::shared_ptr<L::Counters> c = std::make_shared<L::Counters>();
			const OwnerFill f = L::MakeOwnerFill(L::ReserveBytes(GrainDims(64, 48)), c);
			OwnerFillRequest req;
			req.id = kP10gGrainLevel;
			req.seq = 7;
			req.form = AcquireForm::Gpu;
			OwnerFillOutput out;
			Assert::IsFalse(f.fill(req, m, out), L"declined");
			Assert::AreEqual(std::string("grain level: no GPU context"), out.declineReason, L"worded");
			Assert::IsFalse(out.gpu.Valid(), L"nothing made");
			Assert::AreEqual(uint64_t(1), c->declines.load(), L"counted for the status line");
			Assert::AreEqual(uint64_t(0), c->fills.load(), L"no fill");
			Assert::AreEqual(out.declineReason, c->LastDecline(), L"the last decline shown");
		}

		TEST_METHOD(FillDeclinesVisiblyWithoutASourcePlane)
		{
			using namespace VpSkeleton;
			namespace L = GrainSplit::LevelGpu;
			int host = 0;
			SkeletonMemo m;
			m.SetHost(&host);
			m.SetRenderThread(std::this_thread::get_id());
			const OwnerFill f = L::MakeOwnerFill(L::ReserveBytes(GrainDims(64, 48)), std::make_shared<L::Counters>());
			GrainRefusingDevice dev;
			OwnerGpuContext ctx(&dev, f.reserveBytes);
			OwnerFillRequest req;
			req.host = &host;
			req.id = kP10gGrainLevel;
			req.seq = 7;
			req.form = AcquireForm::Gpu;
			req.gpu = &ctx;
			OwnerFillOutput out;
			Assert::IsFalse(f.fill(req, m, out), L"no P13 owner: declined");
			Assert::IsTrue(out.declineReason.find("grain level: no source plane for the frame (P13: ") == 0, L"worded with P13's cause");
			Assert::AreEqual(0, dev.asked, L"no texture asked for");
		}

		TEST_METHOD(FillDeclinesVisiblyWhenTheGrainSplitIsUnavailable)
		{
			using namespace VpSkeleton;
			using Vpv::Capture::SourcePlanes;
			namespace L = GrainSplit::LevelGpu;
			int host = 0;
			pl_tex_t luma{}, chroma{};
			luma.params.w = 64;
			luma.params.h = 48;
			chroma.params.w = 32;
			chroma.params.h = 24;
			pl_frame image{};
			image.num_planes = 2;
			image.planes[0].texture = &luma;
			image.planes[0].components = 1;
			image.planes[1].texture = &chroma;
			image.planes[1].components = 2;
			image.repr.sys = PL_COLOR_SYSTEM_BT_2020_NC;
			image.repr.levels = PL_COLOR_LEVELS_LIMITED;

			SkeletonMemo m;
			m.SetHost(&host);
			m.SetRenderThread(std::this_thread::get_id());
			Assert::IsTrue(m.RegisterOwnerFill(kP13SourceRing, AcquireForm::Gpu, SourcePlanes::kUnit, SourcePlanes::MakeOwnerFill())
				== SkeletonMemo::Registration::Registered, L"P13 owner");
			Assert::IsTrue(SourcePlanes::OfferFrame(&host, 7, 1, image), L"the Entrance image offered");

			// No P10 variant owner registered: the nested Acquire is refused and P10g says so (never a private split).
			const std::shared_ptr<L::Counters> c = std::make_shared<L::Counters>();
			const OwnerFill f = L::MakeOwnerFill(L::ReserveBytes(GrainDims(64, 48)), c);
			GrainRefusingDevice dev;
			OwnerGpuContext ctx(&dev, f.reserveBytes);
			OwnerFillRequest req;
			req.host = &host;
			req.id = kP10gGrainLevel;
			req.seq = 7;
			req.form = AcquireForm::Gpu;
			req.gpu = &ctx;
			OwnerFillOutput out;
			Assert::IsFalse(f.fill(req, m, out), L"declined");
			Assert::IsTrue(out.declineReason.find("grain level: grain split unavailable: ") == 0, L"worded with the split's cause");
			Assert::AreEqual(0, dev.asked, L"no output texture asked for before the split fits");
			Assert::AreEqual(uint64_t(1), c->declines.load(), L"counted");

			SourcePlanes::Forget(&host);
		}

		TEST_METHOD(FallbackLinesSayTheSingleFrameLawAndTheDeclines)
		{
			using namespace VpSkeleton;
			namespace L = GrainSplit::LevelGpu;
			Assert::IsTrue(L::FallbackLine(0).empty(), L"no Fallbacks line before any fill");
			Assert::IsTrue(L::FallbackLine(1).find(std::string(L::kFallbackId) + ": ") == 0, L"the Fallbacks id once in use");
			Assert::IsTrue(L::FallbackLine(12).find("single-frame law") != std::string::npos, L"the law in use");
			Assert::IsTrue(L::DeclineLine(0, "grain level: no GPU context").empty(), L"no decline: no line");
			Assert::AreEqual(std::string("grain_level_declined: grain level: no GPU context (3 declines)"),
				L::DeclineLine(3, "grain level: no GPU context"), L"the last decline and the count");
			Assert::AreEqual(std::string("grain_level_declined: grain level: RGB source (no luma plane to split) (1 decline)"),
				L::DeclineLine(1, "grain level: RGB source (no luma plane to split)"), L"one decline");
			Assert::AreEqual(std::string("grain_level_declined: a fill declined (2 declines)"), L::DeclineLine(2, std::string()),
				L"a count without a recorded reason still shows");
			Assert::IsTrue(L::UnregisteredLine(L::kUnregisteredId, std::string()).empty(), L"no problem: no line");
			Assert::AreEqual(std::string("grain_level_unregistered: why"), L::UnregisteredLine(L::kUnregisteredId, "why"), L"P10g id");
			Assert::AreEqual(std::string("grain_split_owner_unregistered: why"), L::UnregisteredLine(L::kSplitUnregisteredId, "why"),
				L"P10 owner id");
			Assert::AreEqual(std::string("refused: another fill is already registered"),
				L::RegistrationText(SkeletonMemo::Registration::AlreadyRegistered), L"worded");
			Assert::AreEqual(std::string("refused: not in the catalog"), L::RegistrationText(SkeletonMemo::Registration::NotInCatalog), L"worded");
		}

		// The module's own counters reach its Fallbacks lines: a fill made by the module declines (no GPU context) and shows.
		TEST_METHOD(ModuleShowsItsFillsDeclineOnTheFallbacksRoute)
		{
			using namespace VpSkeleton;
			namespace L = GrainSplit::LevelGpu;
			SkeletonMemo m;
			GrainMemoDemand demand;
			demand.memo = &m;
			L::GrainLevelModule module;
			int host = 0;
			Assert::IsTrue(module.Observe(&host, GrainDims(64, 48), &demand), L"registered");
			OwnerFillRequest req;
			req.id = kP10gGrainLevel;
			req.seq = 7;
			req.form = AcquireForm::Gpu;
			OwnerFillOutput out;
			Assert::IsFalse(demand.lastLevelFill.fill(req, m, out), L"the module's fill declines without a GPU context");
			const std::vector<std::string> lines = module.FallbackLines();
			Assert::AreEqual(size_t(1), lines.size(), L"exactly the decline (no fill ran)");
			Assert::AreEqual(std::string("grain_level_declined: grain level: no GPU context (1 decline)"), lines[0], L"on the Fallbacks route");
		}
	};
}
