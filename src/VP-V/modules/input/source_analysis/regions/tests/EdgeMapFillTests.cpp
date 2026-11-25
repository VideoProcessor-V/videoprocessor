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
#include <VP-V/core/VramUsers.h>
#include <VP-V/modules/input/capture/SourcePlanes.h>
#include <VP-V/modules/input/source_analysis/regions/EdgeMapFill.h>

#include <cstdint>
#include <cstring>
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
				s.fmt = TexFormat::RG32F;
				Assert::AreEqual(uint64_t(d.width) * d.height * 8u, Vpv::Regions::EdgeMapReserveBytes(d), L"8 B per source pixel: the largest variant (RG32F)");
				Assert::AreEqual(ShapeBytes(s), Vpv::Regions::EdgeMapReserveBytes(d), L"the reserve equals the largest texture the fill makes");
				s.fmt = TexFormat::R32F;
				Assert::AreEqual(2u * ShapeBytes(s), Vpv::Regions::EdgeMapReserveBytes(d), L"the default R32F texture uses half");
			}
			Assert::AreEqual(uint64_t(0), Vpv::Regions::EdgeMapReserveBytes(DimsOf(0, 0)), L"no size known: no reserve");
			Assert::AreEqual(uint64_t(0), Vpv::Regions::EdgeMapReserveBytes(DimsOf(1920, 0)), L"an empty plane: no reserve");
		}

		TEST_METHOD(FillRegistersUnderTheCatalogOwner)
		{
			using namespace VpSkeleton;
			SkeletonMemo m;
			const OwnerFill f = Vpv::Regions::MakeEdgeMapOwnerFill(Vpv::Regions::EdgeMapReserveBytes(DimsOf(1920, 1080)));
			Assert::AreEqual(uint32_t(Vpv::Regions::kEdgeMapAxes), f.variantAxes, L"the default variant plus the stage variant's axes (CB-18 S2b)");
			Assert::IsTrue(f.frameScoped, L"a per-frame memo");
			Assert::AreEqual(uint64_t(1920) * 1080u * 8u, f.reserveBytes, L"the reserve given");
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
			Assert::AreEqual(uint64_t(1920) * 1080u * 8u, demand.lastReserve, L"1080p reserve (the declared size, 8 B/px, CB-18 SHOULD-2)");
			Assert::AreEqual(uint64_t(1920) * 1080u * 8u, module.RegisteredReserveBytes());
			Assert::IsTrue(m.HasOwnerFill(kEdgeMap, AcquireForm::Gpu), L"the memo holds the fill");

			Assert::IsTrue(module.Observe(&hostA, DimsOf(1920, 1080), &demand), L"same size");
			Assert::AreEqual(1, demand.registers, L"same size: no re-registration");

			Assert::IsTrue(module.Observe(&hostB, DimsOf(3840, 2160), &demand), L"a larger host");
			Assert::AreEqual(1, demand.unregisters, L"the old fill removed");
			Assert::AreEqual(2, demand.registers, L"re-registered once");
			Assert::AreEqual(uint64_t(3840) * 2160u * 8u, module.RegisteredReserveBytes(), L"the largest live size");

			Assert::IsTrue(module.Observe(&hostA, DimsOf(1280, 720), &demand), L"a smaller host below the largest");
			Assert::AreEqual(2, demand.registers, L"the largest is unchanged: no re-registration");

			module.Forget(&hostB, &demand);
			Assert::AreEqual(3, demand.registers, L"the largest host left: re-registered");
			Assert::AreEqual(uint64_t(1280) * 720u * 8u, module.RegisteredReserveBytes(), L"the remaining host's size");
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

		// Category Cpu: the name says GPU, the body is hardware-free (a refusing fake device, no pl_gpu).
		BEGIN_TEST_METHOD_ATTRIBUTE(FillDeclinesWithoutAGpuContext)
			TEST_METHOD_ATTRIBUTE(L"Category", L"Cpu")
		END_TEST_METHOD_ATTRIBUTE()
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

			// The default variant's texture: one R32F plane of the luma size (half the declared reserve since CB-18 SHOULD-2).
			const uint64_t need = uint64_t(64) * 48u * 4u;
			Assert::AreEqual(2u * need, Vpv::Regions::EdgeMapReserveBytes(DimsOf(64, 48)), L"the declared reserve holds it");
			const OwnerFill f = Vpv::Regions::MakeEdgeMapOwnerFill(Vpv::Regions::EdgeMapReserveBytes(DimsOf(64, 48)));
			OwnerFillRequest req;
			req.host = &host;
			req.id = kEdgeMap;
			req.seq = 7;
			req.form = AcquireForm::Gpu;

			// A reserve derived from a smaller source than the frame: the texture would pass it (visible decline, nothing made).
			RefusingDevice smallDev; // not "small": rpcndr.h defines small as char
			OwnerGpuContext under(&smallDev, need - 1u);
			req.gpu = &under;
			OwnerFillOutput out;
			Assert::IsFalse(f.fill(req, m, out), L"above the reserve: declined");
			Assert::IsTrue(out.declineReason.find("edge_map: ") == 0 && out.declineReason.find("reserve") != std::string::npos,
				L"worded with the reserve");
			Assert::AreEqual(0, smallDev.asked, L"the device is never asked");
			Assert::AreEqual(uint64_t(1), under.Refused(), L"counted as refused");

			// A grant of exactly the default texture fits: the device is asked for one R32F plane of the luma size.
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

		// S#4 (R4 / S#3 R3): kEdgeMap CPU owner fill + EdgeMapCpuReadback contract on the CPU tier:
		// demand-gating (0 submits / 0 VRAM without demand), VRAM declaration under ProductVramUserId(kEdgeMap)
		// (outside DeclarableVramUsers), non-latched ReadbackPending while in flight, lag-1 fill + lag-2 reuse
		// + lag-3 FrameReleased (keepFrames == 2), latched OwnerDeclined on unscheduled frames, and clean teardown.
		TEST_METHOD(EdgeMapCpuFillHonoursDemandGatePendingRetryLagOneTwoAndVram)
		{
			using namespace VpSkeleton;
			using namespace Vpv::Regions;

			struct VramResetScope
			{
				VramResetScope() { VramUsers::Instance().ResetForTesting(); }
				~VramResetScope() { VramUsers::Instance().ResetForTesting(); }
			} vramScope;

			constexpr int kW = 16, kH = 12;
			constexpr size_t kElems = size_t(kW) * size_t(kH);
			constexpr size_t kPayloadBytes = kElems * sizeof(float);
			const uint64_t expectedVram = uint64_t(kEdgeMapReadbackSlots) * uint64_t(kPayloadBytes) * 2u;

			class FakeEdgeReadbackDevice : public IReadbackDevice
			{
			public:
				std::vector<char> done;
				std::vector<std::vector<float>> slotPayloads;
				int copies = 0;
				int unmaps = 0;

				explicit FakeEdgeReadbackDevice(size_t slots)
					: done(slots, 0), slotPayloads(slots, std::vector<float>(kElems, 0.0f)) {}

				bool CopyToStaging(ProductId id, uint64_t seq, int stagingIndex) override
				{
					if (id != kEdgeMap || stagingIndex < 0 || size_t(stagingIndex) >= slotPayloads.size())
						return false;
					++copies;
					done[size_t(stagingIndex)] = 0;
					std::vector<float>& dst = slotPayloads[size_t(stagingIndex)];
					for (size_t i = 0; i < dst.size(); ++i)
						dst[i] = static_cast<float>(seq * 1000u + i);
					return true;
				}
				bool QueryDone(int stagingIndex) override
				{
					return stagingIndex >= 0 && size_t(stagingIndex) < done.size() && done[size_t(stagingIndex)] != 0;
				}
				const void* MapNoWait(int stagingIndex, size_t* bytes) override
				{
					if (stagingIndex < 0 || size_t(stagingIndex) >= slotPayloads.size())
						return nullptr;
					*bytes = slotPayloads[size_t(stagingIndex)].size() * sizeof(float);
					return slotPayloads[size_t(stagingIndex)].data();
				}
				void Unmap(int) override { ++unmaps; }
			};

			auto findVramEntry = [](uint16_t id, uint64_t* outHeld) -> bool {
				const VramUsers::Snapshot snap = VramUsers::Instance().Take();
				for (const std::pair<uint16_t, uint64_t>& h : snap.heldById)
				{
					if (h.first == id)
					{
						if (outHeld) *outHeld = h.second;
						return true;
					}
				}
				return false;
			};

			const uint16_t edgeVramId = ProductVramUserId(kEdgeMap);
			Assert::AreEqual(uint16_t(kUserProductBase + kEdgeMap), edgeVramId,
				L"ProductVramUserId(kEdgeMap) == kUserProductBase + kEdgeMap");

			// Confirm ProductVramUserId(kEdgeMap) is NOT added to DeclarableVramUsers.
			size_t declarableCount = 0;
			const uint16_t* declarableIds = DeclarableVramUsers(&declarableCount);
			for (size_t i = 0; i < declarableCount; ++i)
				Assert::IsTrue(declarableIds[i] != edgeVramId,
					L"kEdgeMap CPU readback uses kUserProductBase + kEdgeMap, never a new DeclarableVramUsers id");

			FakeEdgeReadbackDevice dev(kEdgeMapReadbackSlots);
			EdgeMapCpuReadback rb;
			rb.AttachExternalDevice(&dev, kEdgeMapReadbackSlots, expectedVram);

			SkeletonMemo memo;
			memo.SetRenderThread(std::this_thread::get_id());
			Assert::IsTrue(memo.RegisterOwnerFill(kEdgeMap, AcquireForm::Cpu, kEdgeMapUnit, MakeEdgeMapCpuOwnerFill(&rb))
				== SkeletonMemo::Registration::Registered, L"kEdgeMap CPU owner fill registers under kEdgeMapUnit");

			// Negative control #1: with demand off (default), ScheduleExternal submits nothing, declares 0 VRAM,
			// and Acquire declines visibly with "demand".
			Assert::IsFalse(rb.Demanded(), L"demand defaults to false");
			Assert::IsFalse(rb.ScheduleExternal(10u), L"negative control: ScheduleExternal returns false without demand");
			Assert::AreEqual(uint64_t(0), rb.Counters().submitted, L"negative control: 0 submits without demand");
			Assert::AreEqual(0, dev.copies, L"negative control: 0 device copies without demand");
			Assert::AreEqual(uint64_t(0), rb.DeclaredVramBytes(), L"negative control: 0 declared VRAM without demand");
			Assert::IsFalse(findVramEntry(edgeVramId, nullptr), L"negative control: VramUsers has no kEdgeMap entry without demand");

			const AcquireResult rNoDemand = memo.Acquire(kEdgeMap, 10u, AcquireForm::Cpu);
			Assert::IsTrue(rNoDemand.status == AcquireStatus::Unavailable && rNoDemand.cause == AcquireCause::OwnerDeclined,
				L"negative control: Acquire without demand returns OwnerDeclined");
			Assert::IsTrue(memo.LastCause(kEdgeMap).find("demand") != std::string::npos,
				L"negative control: LastCause visibly names missing demand");

			// Reset memo latch before demand-active lifecycle so seq 10 can be exercised cleanly.
			memo.Clear();

			// Demand active: ScheduleExternal submits frame 10 and declares VRAM under ProductVramUserId(kEdgeMap).
			rb.SetDemand(true);
			Assert::IsTrue(rb.Demanded());
			Assert::IsTrue(rb.ScheduleExternal(10u), L"demand active: ScheduleExternal(10) submits");
			Assert::AreEqual(uint64_t(1), rb.Counters().submitted, L"1 submit recorded");
			Assert::AreEqual(1, dev.copies, L"1 staging copy issued");
			Assert::AreEqual(expectedVram, rb.DeclaredVramBytes(), L"DeclaredVramBytes matches expected staging VRAM");
			uint64_t heldVram = 0;
			Assert::IsTrue(findVramEntry(edgeVramId, &heldVram), L"VramUsers contains ProductVramUserId(kEdgeMap)");
			Assert::AreEqual(expectedVram, heldVram, L"VramUsers heldBytes equals expectedVram");

			// In-flight -> ReadbackPending (NOT latched).
			memo.OnSeal(10u);
			const AcquireResult rPend = memo.Acquire(kEdgeMap, 10u, AcquireForm::Cpu);
			Assert::IsTrue(rPend.status == AcquireStatus::Unavailable && rPend.cause == AcquireCause::ReadbackPending,
				L"in-flight readback returns non-latched ReadbackPending");
			Assert::AreEqual(uint64_t(1), memo.CountersOf(kEdgeMap).unavailable[size_t(AcquireCause::ReadbackPending)],
				L"ReadbackPending counter incremented");

			// 1 frame later (lag-1: OnSeal(11)), device finishes -> retry succeeds (FilledAtRead) with exact float32 payload.
			memo.OnSeal(11u);
			dev.done[0] = 1;
			const AcquireResult rLag1 = memo.Acquire(kEdgeMap, 10u, AcquireForm::Cpu);
			Assert::IsTrue(rLag1.status == AcquireStatus::FilledAtRead && rLag1.Ok(),
				L"lag-1 retry after device ready returns FilledAtRead");
			Assert::IsNotNull(rLag1.cpu);
			Assert::AreEqual(kPayloadBytes, rLag1.cpuBytes, L"exact R32F byte count");
			for (size_t i = 0; i < kElems; ++i)
			{
				float v = 0.0f;
				std::memcpy(&v, rLag1.cpu + i * sizeof(float), sizeof(float));
				Assert::AreEqual(static_cast<float>(10u * 1000u + i), v, 0.0f, L"exact float32 texel payload");
			}
			Assert::AreEqual(1, dev.unmaps, L"staging buffer unmapped cleanly");

			// Lag-2 (OnSeal(12)): keepFrames == 2, !frameScoped -> Reused from memo!
			memo.OnSeal(12u);
			const AcquireResult rLag2 = memo.Acquire(kEdgeMap, 10u, AcquireForm::Cpu);
			Assert::IsTrue(rLag2.status == AcquireStatus::Reused && rLag2.cpu == rLag1.cpu && rLag2.cpuBytes == kPayloadBytes,
				L"lag-2 Acquire reuses kept memo spot (keepFrames == 2, !frameScoped)");

			// Lag-3 (OnSeal(13)): past keepFrames == 2 -> FrameReleased!
			memo.OnSeal(13u);
			const AcquireResult rLag3 = memo.Acquire(kEdgeMap, 10u, AcquireForm::Cpu);
			Assert::IsTrue(rLag3.status == AcquireStatus::Unavailable && rLag3.cause == AcquireCause::FrameReleased,
				L"lag-3 Acquire past keepFrames == 2 returns FrameReleased");

			// Negative control #2: unscheduled frame 99 declines visibly ("not scheduled") and latches OwnerDeclined.
			memo.OnSeal(99u);
			const uint64_t callsBefore99 = memo.CountersOf(kEdgeMap).ownerFillCalls;
			const AcquireResult rUnsched1 = memo.Acquire(kEdgeMap, 99u, AcquireForm::Cpu);
			Assert::IsTrue(rUnsched1.status == AcquireStatus::Unavailable && rUnsched1.cause == AcquireCause::OwnerDeclined,
				L"negative control: unscheduled frame returns OwnerDeclined (not ReadbackPending)");
			Assert::IsTrue(memo.LastCause(kEdgeMap).find("not scheduled") != std::string::npos,
				L"negative control: LastCause names 'not scheduled'");
			Assert::AreEqual(callsBefore99 + 1u, memo.CountersOf(kEdgeMap).ownerFillCalls);

			const AcquireResult rUnsched2 = memo.Acquire(kEdgeMap, 99u, AcquireForm::Cpu);
			Assert::IsTrue(rUnsched2.status == AcquireStatus::Unavailable && rUnsched2.cause == AcquireCause::OwnerDeclined,
				L"negative control: second Acquire on unscheduled frame stays OwnerDeclined");
			Assert::AreEqual(callsBefore99 + 1u, memo.CountersOf(kEdgeMap).ownerFillCalls,
				L"negative control: latched OwnerDeclined does not re-invoke fill");

			// Teardown withdraws VRAM cleanly.
			rb.SetDemand(false);
			Assert::AreEqual(uint64_t(0), rb.DeclaredVramBytes(), L"SetDemand(false) withdraws DeclaredVramBytes to 0");
			Assert::IsFalse(findVramEntry(edgeVramId, nullptr), L"SetDemand(false) removes kEdgeMap entry from VramUsers");
		}
	};
}
