// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "CppUnitTest.h"

// input.source_analysis/regions: the kEdgeMap OWNER FILL (EdgeMapFill.h) on the CPU tier, no GPU object touched (textures are plain
// structs, the device is a test double that makes no texture):
//   1. the reserve is DERIVED from kEdgeMap's declared size rule at the source luma size (rule 6), equal to the R32F texture it makes;
//   2. the fill registers under the catalog's owning unit; the module registers on the first observed size, re-registers only when
//      the largest live size changes, forgets a host, never registers without a size and never retries a refused size every frame;
//   3. the visible declines: no GPU context, no source luma view, a texture above the reserve, a device that refuses the texture.
// The GPU parity test (fill vs BusyEdge::SobelMagnitude, GPU-QUIET) is owed before the first kEdgeMap reader row (102b KEDGEMAP-RULE3).

#include <VP-V/core/GpuAllocator.h>
#include <VP-V/core/Products.h>
#include <VP-V/core/SkeletonAcquire.h>
#include <VP-V/modules/input/capture/SourcePlanes.h>
#include <VP-V/modules/input/source_analysis/regions/EdgeMapFill.h>

#include <cstdint>
#include <string>
#include <thread>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	namespace
	{
		// Forwards the module's registrations to one real memo (the catalog's ownership rules apply) and counts them.
		struct MemoDemand : Vpv::Skeleton::IExternalDemand
		{
			VpSkeleton::SkeletonMemo* memo = nullptr;
			int registers = 0;
			int unregisters = 0;
			uint64_t lastReserve = 0;
			void SetExternalDemand(const void*, const void*, const std::vector<VpSkeleton::ConsumerDecl>&,
				const std::vector<VpSkeleton::ProductId>&) override {}
			void ClearExternalDemand(const void*, const void*) override {}
			VpSkeleton::SkeletonMemo::Registration RegisterOwnerFill(VpSkeleton::ProductId id, VpSkeleton::AcquireForm form,
				const std::string& unit, VpSkeleton::OwnerFill fill) override
			{
				++registers;
				lastReserve = fill.reserveBytes;
				return memo->RegisterOwnerFill(id, form, unit, fill);
			}
			bool UnregisterOwnerFill(VpSkeleton::ProductId id, VpSkeleton::AcquireForm form, const std::string& unit) override
			{
				++unregisters;
				return memo->UnregisterOwnerFill(id, form, unit);
			}
		};

		// Non-null opaque handles (the fill only tests them before acquiring its input); makes no texture.
		struct RefusingDevice : VpSkeleton::IOwnerGpuDevice
		{
			int gpuTag = 0;
			int dispatchTag = 0;
			int asked = 0;
			VpSkeleton::TexShape lastShape;
			void* OwnerGpu() const override { return const_cast<int*>(&gpuTag); }
			void* OwnerDispatch() const override { return const_cast<int*>(&dispatchTag); }
			bool CreateOwnerTexture(const VpSkeleton::TexShape& shape, VpSkeleton::GpuMemoHandle&, std::string& why) override
			{
				++asked;
				lastShape = shape;
				why = "test device makes no texture";
				return false;
			}
		};

		VpSkeleton::Dims DimsOf(uint32_t w, uint32_t h)
		{
			VpSkeleton::Dims d;
			d.width = w;
			d.height = h;
			return d;
		}
	}

	TEST_CLASS(EdgeMapFillTests)
	{
	public:
		TEST_METHOD(ReserveIsTheDeclaredSizeAtTheSourceLumaSize)
		{
			using namespace VpSkeleton;
			const Dims sizes[3] = { DimsOf(3840, 2160), DimsOf(1920, 1080), DimsOf(1919, 1081) };
			for (const Dims& d : sizes)
			{
				TexShape s;
				s.w = d.width;
				s.h = d.height;
				s.fmt = TexFormat::R32F;
				Assert::AreEqual(uint64_t(d.width) * d.height * 4u, Vpv::Regions::EdgeMapReserveBytes(d), L"one R32F sample per source pixel");
				Assert::AreEqual(ShapeBytes(s), Vpv::Regions::EdgeMapReserveBytes(d), L"the reserve equals the texture the fill makes");
			}
			Assert::AreEqual(uint64_t(0), Vpv::Regions::EdgeMapReserveBytes(DimsOf(0, 0)), L"no size known: no reserve");
			Assert::AreEqual(uint64_t(0), Vpv::Regions::EdgeMapReserveBytes(DimsOf(1920, 0)), L"an empty plane: no reserve");
		}

		TEST_METHOD(FillRegistersUnderTheCatalogOwner)
		{
			using namespace VpSkeleton;
			SkeletonMemo m;
			const OwnerFill f = Vpv::Regions::MakeEdgeMapOwnerFill(Vpv::Regions::EdgeMapReserveBytes(DimsOf(1920, 1080)));
			Assert::AreEqual(uint32_t(0), f.variantAxes, L"default variant only");
			Assert::IsTrue(f.frameScoped, L"a per-frame memo");
			Assert::AreEqual(uint64_t(1920) * 1080u * 4u, f.reserveBytes, L"the reserve given");
			Assert::AreEqual(std::string(Vpv::Regions::kEdgeMapUnit), SkeletonMemo::OwnerUnitOf(kEdgeMap), L"the catalog's owner unit");
			Assert::IsTrue(m.RegisterOwnerFill(kEdgeMap, AcquireForm::Gpu, Vpv::Regions::kEdgeMapUnit, f) == SkeletonMemo::Registration::Registered,
				L"registered by its unit");
			Assert::IsTrue(m.HasOwnerFill(kEdgeMap, AcquireForm::Gpu), L"the memo holds it");
			Assert::IsTrue(m.RegisterOwnerFill(kEdgeMap, AcquireForm::Gpu, "input/capture (queue + early upload)", f)
				== SkeletonMemo::Registration::NotOwner, L"another unit is refused");
		}

		TEST_METHOD(ModuleDerivesAndTracksTheLargestLiveSize)
		{
			using namespace VpSkeleton;
			SkeletonMemo m;
			MemoDemand demand;
			demand.memo = &m;
			Vpv::Regions::EdgeMapModule module;
			int hostA = 0, hostB = 0;

			Assert::IsFalse(module.Observe(&hostA, DimsOf(0, 0), &demand), L"no size: nothing registered");
			Assert::AreEqual(0, demand.registers, L"no registration without a size");

			Assert::IsTrue(module.Observe(&hostA, DimsOf(1920, 1080), &demand), L"registered on the first size");
			Assert::AreEqual(1, demand.registers);
			Assert::AreEqual(uint64_t(1920) * 1080u * 4u, demand.lastReserve, L"1080p reserve");
			Assert::AreEqual(uint64_t(1920) * 1080u * 4u, module.RegisteredReserveBytes());
			Assert::IsTrue(m.HasOwnerFill(kEdgeMap, AcquireForm::Gpu), L"the memo holds the fill");

			Assert::IsTrue(module.Observe(&hostA, DimsOf(1920, 1080), &demand), L"same size");
			Assert::AreEqual(1, demand.registers, L"same size: no re-registration");

			Assert::IsTrue(module.Observe(&hostB, DimsOf(3840, 2160), &demand), L"a larger host");
			Assert::AreEqual(1, demand.unregisters, L"the old fill removed");
			Assert::AreEqual(2, demand.registers, L"re-registered once");
			Assert::AreEqual(uint64_t(3840) * 2160u * 4u, module.RegisteredReserveBytes(), L"the largest live size");

			Assert::IsTrue(module.Observe(&hostA, DimsOf(1280, 720), &demand), L"a smaller host below the largest");
			Assert::AreEqual(2, demand.registers, L"the largest is unchanged: no re-registration");

			module.Forget(&hostB, &demand);
			Assert::AreEqual(3, demand.registers, L"the largest host left: re-registered");
			Assert::AreEqual(uint64_t(1280) * 720u * 4u, module.RegisteredReserveBytes(), L"the remaining host's size");
			Assert::IsTrue(m.HasOwnerFill(kEdgeMap, AcquireForm::Gpu), L"still held");

			module.Forget(&hostA, &demand);
			Assert::AreEqual(3, demand.registers, L"no host left: the registration is kept");
			Assert::IsTrue(module.Registered(), L"kept");
		}

		TEST_METHOD(ModuleDoesNotRetryARefusedSizeEveryFrame)
		{
			using namespace VpSkeleton;
			SkeletonMemo m;
			Assert::IsTrue(m.RegisterOwnerFill(kEdgeMap, AcquireForm::Gpu, Vpv::Regions::kEdgeMapUnit, Vpv::Regions::MakeEdgeMapOwnerFill(64))
				== SkeletonMemo::Registration::Registered, L"someone registered first");
			MemoDemand demand;
			demand.memo = &m;
			Vpv::Regions::EdgeMapModule module;
			int host = 0;
			Assert::IsFalse(module.Observe(&host, DimsOf(1920, 1080), &demand), L"refused (already registered): not registered");
			Assert::AreEqual(1, demand.registers);
			Assert::IsFalse(module.Observe(&host, DimsOf(1920, 1080), &demand), L"the same size again");
			Assert::AreEqual(1, demand.registers, L"not retried every frame");
			Assert::AreEqual(uint64_t(0), module.RegisteredReserveBytes(), L"nothing of ours registered");
		}

		TEST_METHOD(FillDeclinesWithoutAGpuContext)
		{
			using namespace VpSkeleton;
			SkeletonMemo m;
			const OwnerFill f = Vpv::Regions::MakeEdgeMapOwnerFill(Vpv::Regions::EdgeMapReserveBytes(DimsOf(64, 48)));
			OwnerFillRequest req;
			req.id = kEdgeMap;
			req.seq = 7;
			req.form = AcquireForm::Gpu;
			OwnerFillOutput out;
			Assert::IsFalse(f.fill(req, m, out), L"declined");
			Assert::AreEqual(std::string("edge_map: no GPU context"), out.declineReason, L"worded");
			Assert::IsFalse(out.gpu.Valid(), L"nothing made");

			RefusingDevice dev;
			OwnerGpuContext closed(&dev, f.reserveBytes);
			closed.Close();
			req.gpu = &closed;
			OwnerFillOutput out2;
			Assert::IsFalse(f.fill(req, m, out2), L"a closed context declines");
			Assert::AreEqual(std::string("edge_map: no GPU context"), out2.declineReason, L"worded");
		}

		TEST_METHOD(FillDeclinesWithoutASourceLumaView)
		{
			using namespace VpSkeleton;
			int host = 0;
			SkeletonMemo m;
			m.SetHost(&host);
			m.SetRenderThread(std::this_thread::get_id());
			const OwnerFill f = Vpv::Regions::MakeEdgeMapOwnerFill(Vpv::Regions::EdgeMapReserveBytes(DimsOf(64, 48)));
			RefusingDevice dev;
			OwnerGpuContext ctx(&dev, f.reserveBytes);
			OwnerFillRequest req;
			req.host = &host;
			req.id = kEdgeMap;
			req.seq = 7;
			req.form = AcquireForm::Gpu;
			req.gpu = &ctx;
			OwnerFillOutput out;
			Assert::IsFalse(f.fill(req, m, out), L"no P13 owner: declined");
			Assert::AreEqual(std::string("edge_map: no source luma view"), out.declineReason, L"worded");
			Assert::AreEqual(0, dev.asked, L"no texture asked for");
		}

		TEST_METHOD(FillDeclinesAboveTheReserveAndOnARefusedTexture)
		{
			using namespace VpSkeleton;
			using Vpv::Capture::SourcePlanes;
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

			const uint64_t need = Vpv::Regions::EdgeMapReserveBytes(DimsOf(64, 48));
			const OwnerFill f = Vpv::Regions::MakeEdgeMapOwnerFill(need);
			OwnerFillRequest req;
			req.host = &host;
			req.id = kEdgeMap;
			req.seq = 7;
			req.form = AcquireForm::Gpu;

			// A reserve derived from a smaller source than the frame: the texture would pass it (visible decline, nothing made).
			RefusingDevice small;
			OwnerGpuContext under(&small, need - 1u);
			req.gpu = &under;
			OwnerFillOutput out;
			Assert::IsFalse(f.fill(req, m, out), L"above the reserve: declined");
			Assert::IsTrue(out.declineReason.find("edge_map: ") == 0 && out.declineReason.find("reserve") != std::string::npos,
				L"worded with the reserve");
			Assert::AreEqual(0, small.asked, L"the device is never asked");
			Assert::AreEqual(uint64_t(1), under.Refused(), L"counted as refused");

			// The derived reserve fits exactly: the device is asked for one R32F plane of the luma size.
			RefusingDevice dev;
			OwnerGpuContext ctx(&dev, need);
			req.gpu = &ctx;
			OwnerFillOutput out2;
			Assert::IsFalse(f.fill(req, m, out2), L"the device refused: declined");
			Assert::AreEqual(std::string("edge_map: test device makes no texture"), out2.declineReason, L"the device's reason");
			Assert::AreEqual(1, dev.asked, L"asked once");
			Assert::AreEqual(uint32_t(64), dev.lastShape.w, L"luma width");
			Assert::AreEqual(uint32_t(48), dev.lastShape.h, L"luma height");
			Assert::IsTrue(dev.lastShape.fmt == TexFormat::R32F, L"R32F (12-bit exact)");

			SourcePlanes::Forget(&host);
		}
	};
}
