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

		// Pass A regressions (MED-4, MED-6, HIGH-1, HIGH-2, HIGH-3):
		//   1. EdgeMapModule::SetCpuDemand / HasCpuDemand / CpuRegistered / OnLifecycle(GpuReleased, Shutdown) /
		//      ~EdgeMapModule() clean VRAM withdrawal + expired-module fill decline without UAF.
		//   2. 3-frame unconsumed ReadbackRing livelock regression (evicts stale Ready slot via Discard, no skippedNoStaging).
		//   3. Failed-slot reclamation via QueryFailed/Discard & zero global poisoning across 4+ consecutive failures.
		//   4. Lag-2 preservation across Take(seq-1) (keepOlder == kEdgeMapCpuKeepFrames == 2).
		TEST_METHOD(EdgeMapModuleManagesDemandRegistrationLifecycleAndReadbackRecovery)
		{
			using namespace VpSkeleton;
			using namespace Vpv::Regions;

			struct VramResetScope
			{
				VramResetScope() { VramUsers::Instance().ResetForTesting(); }
				~VramResetScope() { VramUsers::Instance().ResetForTesting(); }
			} vramScope;

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

			constexpr int kW = 16, kH = 12;
			constexpr size_t kElems = size_t(kW) * size_t(kH);
			constexpr size_t kPayloadBytes = kElems * sizeof(float);
			const uint64_t kVramBytes = uint64_t(kEdgeMapReadbackSlots) * uint64_t(kPayloadBytes) * 2u;
			const uint16_t edgeVramId = ProductVramUserId(kEdgeMap);

			class FakeRecoveryReadbackDevice : public IReadbackDevice
			{
			public:
				std::vector<char> done;
				std::vector<char> failed;
				std::vector<std::vector<float>> slotPayloads;
				std::vector<uint64_t> slotSeq;
				int copies = 0;
				int unmaps = 0;
				int discards = 0;

				explicit FakeRecoveryReadbackDevice(size_t slots)
					: done(slots, 0), failed(slots, 0),
					  slotPayloads(slots, std::vector<float>(kElems, 0.0f)), slotSeq(slots, 0) {}

				bool CopyToStaging(ProductId id, uint64_t seq, int slot) override
				{
					if (id != kEdgeMap || slot < 0 || size_t(slot) >= slotPayloads.size())
						return false;
					++copies;
					done[size_t(slot)] = 0;
					failed[size_t(slot)] = 0;
					slotSeq[size_t(slot)] = seq;
					std::vector<float>& dst = slotPayloads[size_t(slot)];
					for (size_t i = 0; i < dst.size(); ++i)
						dst[i] = static_cast<float>(seq * 1000u + i);
					return true;
				}
				bool QueryDone(int slot) override
				{
					return slot >= 0 && size_t(slot) < done.size()
						&& failed[size_t(slot)] == 0 && done[size_t(slot)] != 0;
				}
				bool QueryFailed(int slot) const override
				{
					return slot >= 0 && size_t(slot) < failed.size() && failed[size_t(slot)] != 0;
				}
				void Discard(int slot) override
				{
					++discards;
					if (slot >= 0 && size_t(slot) < done.size())
					{
						done[size_t(slot)] = 0;
						failed[size_t(slot)] = 0;
					}
				}
				const void* MapNoWait(int slot, size_t* bytes) override
				{
					if (slot < 0 || size_t(slot) >= slotPayloads.size() || failed[size_t(slot)] != 0)
						return nullptr;
					*bytes = slotPayloads[size_t(slot)].size() * sizeof(float);
					return slotPayloads[size_t(slot)].data();
				}
				void Unmap(int) override { ++unmaps; }

				int FindSlotFor(uint64_t seq) const
				{
					for (size_t i = 0; i < slotSeq.size(); ++i)
						if (slotSeq[i] == seq)
							return int(i);
					return -1;
				}
			};

			// ---- 1. EdgeMapModule demand registration, OnLifecycle(GpuReleased/Shutdown), ~EdgeMapModule() & MED-6 UAF guard ----
			{
				struct CapturingDemand : Vpv::Skeleton::IExternalDemand
				{
					SkeletonMemo* memo = nullptr;
					int registers = 0;
					int unregisters = 0;
					OwnerFill capturedCpuFill;
					void SetExternalDemand(const void*, const void*, const std::vector<ConsumerDecl>&,
						const std::vector<ProductId>&) override {}
					void ClearExternalDemand(const void*, const void*) override {}
					SkeletonMemo::Registration RegisterOwnerFill(ProductId id, AcquireForm form,
						const std::string& unit, OwnerFill fill) override
					{
						++registers;
						if (id == kEdgeMap && form == AcquireForm::Cpu)
							capturedCpuFill = fill;
						return memo->RegisterOwnerFill(id, form, unit, fill);
					}
					bool UnregisterOwnerFill(ProductId id, AcquireForm form, const std::string& unit) override
					{
						++unregisters;
						return memo->UnregisterOwnerFill(id, form, unit);
					}
				};

				SkeletonMemo memo;
				memo.SetRenderThread(std::this_thread::get_id());
				CapturingDemand demand;
				demand.memo = &memo;
				FakeRecoveryReadbackDevice devMod(kEdgeMapReadbackSlots);
				std::unique_ptr<EdgeMapModule> mod(new EdgeMapModule());
				int hostA = 0, hostB = 0, hostC = 0;

				// Negative control: before SetCpuDemand, module reports no demand, no CPU registration, and Acquire returns NoOwnerFill.
				Assert::IsFalse(mod->HasCpuDemand(&hostA), L"negative control: HasCpuDemand defaults false");
				Assert::IsFalse(mod->CpuRegistered(), L"negative control: CpuRegistered defaults false before demand");
				Assert::IsFalse(memo.HasOwnerFill(kEdgeMap, AcquireForm::Cpu), L"negative control: memo lacks CPU fill before SetCpuDemand");
				const AcquireResult rNoOwner = memo.Acquire(kEdgeMap, 1u, AcquireForm::Cpu);
				Assert::IsTrue(rNoOwner.status == AcquireStatus::Unavailable && rNoOwner.cause == AcquireCause::NoOwnerFill,
					L"negative control: Acquire before SetCpuDemand returns NoOwnerFill");
				memo.Clear();

				// SetCpuDemand(true) registers CPU fill on demand and enables HasCpuDemand.
				mod->SetCpuDemand(&hostA, true, &demand);
				Assert::IsTrue(mod->HasCpuDemand(&hostA), L"SetCpuDemand(true) sets HasCpuDemand(hostA)");
				Assert::IsFalse(mod->HasCpuDemand(&hostB), L"unrelated hostB still has no demand");
				Assert::IsTrue(mod->CpuRegistered(), L"SetCpuDemand(true) registers CPU fill on demand");
				Assert::IsTrue(memo.HasOwnerFill(kEdgeMap, AcquireForm::Cpu), L"memo now holds kEdgeMap CPU owner fill");
				Assert::AreEqual(1, demand.registers, L"CPU fill registered once");

				EdgeMapCpuReadback* rbA = mod->ReadbackFor(&hostA);
				Assert::IsNotNull(rbA);
				rbA->AttachExternalDevice(&devMod, kEdgeMapReadbackSlots, kVramBytes);
				uint64_t held = 0;
				Assert::IsTrue(findVramEntry(edgeVramId, &held) && held == kVramBytes,
					L"active demand + attached device declares VRAM under ProductVramUserId(kEdgeMap)");

				// SetCpuDemand(false) withdraws VRAM and clears demand.
				mod->SetCpuDemand(&hostA, false, &demand);
				Assert::IsFalse(mod->HasCpuDemand(&hostA), L"SetCpuDemand(false) clears HasCpuDemand(hostA)");
				Assert::AreEqual(uint64_t(0), rbA->DeclaredVramBytes(), L"SetCpuDemand(false) clears DeclaredVramBytes");
				Assert::IsFalse(findVramEntry(edgeVramId, nullptr), L"SetCpuDemand(false) withdraws VramUsers entry");

				// OnLifecycle(GpuReleased) forgets hostA and withdraws VRAM cleanly.
				mod->SetCpuDemand(&hostA, true, &demand);
				rbA->AttachExternalDevice(&devMod, kEdgeMapReadbackSlots, kVramBytes);
				Assert::IsTrue(findVramEntry(edgeVramId, &held) && held == kVramBytes);
				RendererExtension::LifecycleFacts relFacts{};
				relFacts.host = &hostA;
				relFacts.kind = RendererExtension::Lifecycle::GpuReleased;
				mod->OnLifecycle(relFacts);
				Assert::IsFalse(mod->HasCpuDemand(&hostA), L"OnLifecycle(GpuReleased) forgets hostA demand");
				Assert::IsNull(mod->ReadbackFor(&hostA), L"OnLifecycle(GpuReleased) releases hostA readback");
				Assert::IsFalse(findVramEntry(edgeVramId, nullptr), L"OnLifecycle(GpuReleased) withdraws VRAM");

				// OnLifecycle(Shutdown) forgets hostB and withdraws VRAM cleanly.
				mod->SetCpuDemand(&hostB, true, &demand);
				EdgeMapCpuReadback* rbB = mod->ReadbackFor(&hostB);
				Assert::IsNotNull(rbB);
				rbB->AttachExternalDevice(&devMod, kEdgeMapReadbackSlots, kVramBytes);
				Assert::IsTrue(findVramEntry(edgeVramId, &held) && held == kVramBytes);
				RendererExtension::LifecycleFacts shutFacts{};
				shutFacts.host = &hostB;
				shutFacts.kind = RendererExtension::Lifecycle::Shutdown;
				mod->OnLifecycle(shutFacts);
				Assert::IsFalse(mod->HasCpuDemand(&hostB), L"OnLifecycle(Shutdown) forgets hostB demand");
				Assert::IsNull(mod->ReadbackFor(&hostB), L"OnLifecycle(Shutdown) releases hostB readback");
				Assert::IsFalse(findVramEntry(edgeVramId, nullptr), L"OnLifecycle(Shutdown) withdraws VRAM");

				// Positive control before destruction + ~EdgeMapModule() VRAM withdrawal, unregister, and MED-6 expired-fill guard.
				mod->SetCpuDemand(&hostC, true, &demand);
				EdgeMapCpuReadback* rbC = mod->ReadbackFor(&hostC);
				Assert::IsNotNull(rbC);
				rbC->AttachExternalDevice(&devMod, kEdgeMapReadbackSlots, kVramBytes);
				Assert::IsTrue(rbC->ScheduleExternal(8u));
				devMod.done[0] = 1;
				Assert::IsTrue(findVramEntry(edgeVramId, &held) && held == kVramBytes);

				OwnerFillRequest reqLive;
				reqLive.host = &hostC;
				reqLive.id = kEdgeMap;
				reqLive.seq = 8u;
				reqLive.form = AcquireForm::Cpu;
				OwnerFillOutput outLive;
				Assert::IsTrue(demand.capturedCpuFill.fill(reqLive, memo, outLive),
					L"positive control: captured CPU fill serves ready frame while EdgeMapModule is alive");
				Assert::AreEqual(kPayloadBytes, outLive.bytes.size());

				// Destroy EdgeMapModule: must withdraw VRAM, unregister CPU fill, and expire weak_ptr so retained fill declines without UAF.
				mod.reset();
				Assert::IsFalse(findVramEntry(edgeVramId, nullptr), L"~EdgeMapModule() withdraws VRAM cleanly");
				Assert::IsFalse(memo.HasOwnerFill(kEdgeMap, AcquireForm::Cpu), L"~EdgeMapModule() unregisters CPU owner fill");
				Assert::AreEqual(1, demand.unregisters, L"~EdgeMapModule() calls UnregisterOwnerFill once");

				OwnerFillRequest reqExpired;
				reqExpired.host = &hostC;
				reqExpired.id = kEdgeMap;
				reqExpired.seq = 9u;
				reqExpired.form = AcquireForm::Cpu;
				OwnerFillOutput outExpired;
				Assert::IsFalse(demand.capturedCpuFill.fill(reqExpired, memo, outExpired),
					L"MED-6: invoking retained CPU fill after ~EdgeMapModule() declines cleanly without UAF");
				Assert::IsFalse(outExpired.pending, L"MED-6: expired-module fill does not report ReadbackPending");
				Assert::IsTrue(outExpired.declineReason.find("no readback state") != std::string::npos,
					L"MED-6: expired-module decline reason visibly names missing readback state");
			}

			// ---- 2. HIGH-1 regression: 3-frame unconsumed livelock recovery ----
			{
				// Mandatory same-input negative control: naive ReadbackRing without eviction hits permanent skippedNoStaging on frame 4.
				FakeRecoveryReadbackDevice naiveDev(kEdgeMapReadbackSlots);
				VpLaneProducts::ReadbackRing naiveRing(&naiveDev, kEdgeMapReadbackSlots);
				Assert::IsTrue(naiveRing.Submit(kEdgeMap, 1u));
				Assert::IsTrue(naiveRing.Submit(kEdgeMap, 2u));
				Assert::IsTrue(naiveRing.Submit(kEdgeMap, 3u));
				naiveDev.done[0] = 1;
				naiveDev.done[1] = 1;
				naiveDev.done[2] = 1;
				naiveRing.Poll();
				Assert::IsFalse(naiveRing.Submit(kEdgeMap, 4u),
					L"negative control: un-evicted 3-slot ReadbackRing rejects frame 4 when 1..3 are Ready and unconsumed");
				Assert::AreEqual(uint64_t(1), naiveRing.Counters().skippedNoStaging,
					L"negative control: un-evicted ring counts skippedNoStaging livelock");

				// Production EdgeMapCpuReadback: scheduling frame 4 evicts stale slot 0 via Discard and succeeds.
				FakeRecoveryReadbackDevice devLive(kEdgeMapReadbackSlots);
				EdgeMapCpuReadback rbLive;
				rbLive.AttachExternalDevice(&devLive, kEdgeMapReadbackSlots, 0u);
				rbLive.SetDemand(true);

				SkeletonMemo memoLive;
				memoLive.SetRenderThread(std::this_thread::get_id());
				Assert::IsTrue(memoLive.RegisterOwnerFill(kEdgeMap, AcquireForm::Cpu, kEdgeMapUnit, MakeEdgeMapCpuOwnerFill(&rbLive))
					== SkeletonMemo::Registration::Registered);

				Assert::IsTrue(rbLive.ScheduleExternal(1u));
				devLive.done[0] = 1;
				Assert::IsTrue(rbLive.ScheduleExternal(2u));
				devLive.done[1] = 1;
				Assert::IsTrue(rbLive.ScheduleExternal(3u));
				devLive.done[2] = 1;
				rbLive.Poll();
				Assert::IsTrue(rbLive.HasReady(1u) && rbLive.HasReady(2u) && rbLive.HasReady(3u),
					L"precondition: all 3 staging slots are Ready and unconsumed");

				Assert::IsTrue(rbLive.ScheduleExternal(4u),
					L"HIGH-1: ScheduleExternal(4) evicts stale slot 0 via Discard and succeeds");
				Assert::AreEqual(uint64_t(0), rbLive.Counters().skippedNoStaging,
					L"HIGH-1: zero skippedNoStaging livelock");
				Assert::IsTrue(rbLive.Counters().staleDropped >= 1u,
					L"HIGH-1: staleDropped >= 1 after evicting unconsumed frame 1");
				Assert::IsTrue(devLive.discards >= 1,
					L"HIGH-1: Discard() invoked on evicted Ready slot");

				const int slot4 = devLive.FindSlotFor(4u);
				Assert::IsTrue(slot4 >= 0, L"frame 4 occupies a staging slot");
				devLive.done[size_t(slot4)] = 1;
				memoLive.OnSeal(5u);
				const AcquireResult r4 = memoLive.Acquire(kEdgeMap, 4u, AcquireForm::Cpu);
				Assert::IsTrue(r4.Ok() && r4.status == AcquireStatus::FilledAtRead && r4.cpuBytes == kPayloadBytes,
					L"HIGH-1: Acquire(kEdgeMap, 4, Cpu) succeeds after stale-slot eviction");
				float v4 = 0.0f;
				std::memcpy(&v4, r4.cpu, sizeof(float));
				Assert::AreEqual(4000.0f, v4, 0.0f, L"HIGH-1: exact frame 4 payload read back");
				rbLive.SetDemand(false);
			}

			// ---- 3. HIGH-3 regression: failed-slot reclamation & zero global poisoning across 4+ consecutive failures ----
			{
				FakeRecoveryReadbackDevice devFail(kEdgeMapReadbackSlots);
				EdgeMapCpuReadback rbFail;
				rbFail.AttachExternalDevice(&devFail, kEdgeMapReadbackSlots, 0u);
				rbFail.SetDemand(true);

				SkeletonMemo memoFail;
				memoFail.SetRenderThread(std::this_thread::get_id());
				Assert::IsTrue(memoFail.RegisterOwnerFill(kEdgeMap, AcquireForm::Cpu, kEdgeMapUnit, MakeEdgeMapCpuOwnerFill(&rbFail))
					== SkeletonMemo::Registration::Registered);

				// Frame 20 fails asynchronously -> Poll() reclaims slot via Discard() and counts pollFailed == 1.
				Assert::IsTrue(rbFail.ScheduleExternal(20u));
				const int slot20 = devFail.FindSlotFor(20u);
				Assert::IsTrue(slot20 >= 0);
				Assert::IsTrue(rbFail.HasInFlight(20u), L"precondition: frame 20 in flight before failure");
				devFail.failed[size_t(slot20)] = 1;
				rbFail.Poll();

				Assert::AreEqual(uint64_t(1), rbFail.Counters().pollFailed, L"HIGH-3: Poll() increments pollFailed");
				Assert::AreEqual(1, devFail.discards, L"HIGH-3: Poll() calls Discard() on failed slot");
				Assert::IsFalse(rbFail.HasInFlight(20u) || rbFail.HasReady(20u), L"HIGH-3: failed slot reclaimed to Free");

				const AcquireResult r20 = memoFail.Acquire(kEdgeMap, 20u, AcquireForm::Cpu);
				Assert::IsTrue(r20.status == AcquireStatus::Unavailable && r20.cause == AcquireCause::OwnerDeclined,
					L"HIGH-3: failed frame 20 declines visibly with OwnerDeclined");
				Assert::IsTrue(memoFail.LastCause(kEdgeMap).find("failed") != std::string::npos,
					L"HIGH-3: LastCause visibly names 'failed'");

				// Subsequent healthy frame 21 is NOT globally poisoned: returns ReadbackPending in flight, succeeds when done.
				Assert::IsTrue(rbFail.ScheduleExternal(21u), L"HIGH-3: healthy frame 21 schedules after failure");
				const AcquireResult r21Pend = memoFail.Acquire(kEdgeMap, 21u, AcquireForm::Cpu);
				Assert::IsTrue(r21Pend.status == AcquireStatus::Unavailable && r21Pend.cause == AcquireCause::ReadbackPending,
					L"HIGH-3: healthy frame 21 still returns non-latched ReadbackPending while in flight (zero global poison)");
				const int slot21 = devFail.FindSlotFor(21u);
				Assert::IsTrue(slot21 >= 0);
				devFail.done[size_t(slot21)] = 1;
				const AcquireResult r21Ok = memoFail.Acquire(kEdgeMap, 21u, AcquireForm::Cpu);
				Assert::IsTrue(r21Ok.Ok() && r21Ok.status == AcquireStatus::FilledAtRead && r21Ok.cpuBytes == kPayloadBytes,
					L"HIGH-3: healthy frame 21 succeeds cleanly on retry");
				float v21 = 0.0f;
				std::memcpy(&v21, r21Ok.cpu, sizeof(float));
				Assert::AreEqual(21000.0f, v21, 0.0f);

				// 4+ consecutive failures on a 3-slot ring must reclaim every slot with zero slot leak or skippedNoStaging.
				for (uint64_t fseq = 22u; fseq < 26u; ++fseq)
				{
					Assert::IsTrue(rbFail.ScheduleExternal(fseq),
						L"HIGH-3: reclaimed failed slots allow 4+ consecutive failures on 3-slot ring without exhaustion");
					const int fslot = devFail.FindSlotFor(fseq);
					Assert::IsTrue(fslot >= 0);
					devFail.failed[size_t(fslot)] = 1;
					rbFail.Poll();
				}
				Assert::AreEqual(uint64_t(5), rbFail.Counters().pollFailed, L"HIGH-3: all 5 failures counted in pollFailed");
				Assert::AreEqual(5, devFail.discards, L"HIGH-3: Discard() called for every failed slot");
				Assert::AreEqual(uint64_t(0), rbFail.Counters().skippedNoStaging, L"HIGH-3: zero slot leak across 5 failures");

				// Healthy frame 26 still succeeds after 4 consecutive failures.
				Assert::IsTrue(rbFail.ScheduleExternal(26u));
				const int slot26 = devFail.FindSlotFor(26u);
				Assert::IsTrue(slot26 >= 0);
				devFail.done[size_t(slot26)] = 1;
				const AcquireResult r26 = memoFail.Acquire(kEdgeMap, 26u, AcquireForm::Cpu);
				Assert::IsTrue(r26.Ok() && r26.status == AcquireStatus::FilledAtRead,
					L"HIGH-3: healthy frame 26 succeeds after 4 consecutive failures");
				rbFail.SetDemand(false);
			}

			// ---- 4. HIGH-2 regression: lag-2 preservation across Take(seq-1) ----
			{
				// Mandatory same-input negative control: naive keepOlder=0 drops Ready frame 30 when frame 31 is taken first.
				FakeRecoveryReadbackDevice naiveLagDev(kEdgeMapReadbackSlots);
				VpLaneProducts::ReadbackRing naiveLagRing(&naiveLagDev, kEdgeMapReadbackSlots);
				Assert::IsTrue(naiveLagRing.Submit(kEdgeMap, 30u));
				Assert::IsTrue(naiveLagRing.Submit(kEdgeMap, 31u));
				naiveLagDev.done[0] = 1;
				naiveLagDev.done[1] = 1;
				naiveLagRing.Poll();
				Assert::IsTrue(naiveLagRing.Take(31u, nullptr, 0u), L"negative control: naive Take(31, keepOlder=0) succeeds");
				Assert::AreEqual(uint64_t(1), naiveLagRing.Counters().staleDropped,
					L"negative control: naive Take(31, keepOlder=0) stale-drops frame 30");
				Assert::IsFalse(naiveLagRing.Take(30u, nullptr, 0u),
					L"negative control: frame 30 is lost under keepOlder=0");

				// Production EdgeMapCpuReadback (keepOlder == kEdgeMapCpuKeepFrames == 2): Acquire(31) then Acquire(30) both succeed.
				FakeRecoveryReadbackDevice devLag(kEdgeMapReadbackSlots);
				EdgeMapCpuReadback rbLag;
				rbLag.AttachExternalDevice(&devLag, kEdgeMapReadbackSlots, 0u);
				rbLag.SetDemand(true);

				SkeletonMemo memoLag;
				memoLag.SetRenderThread(std::this_thread::get_id());
				Assert::IsTrue(memoLag.RegisterOwnerFill(kEdgeMap, AcquireForm::Cpu, kEdgeMapUnit, MakeEdgeMapCpuOwnerFill(&rbLag))
					== SkeletonMemo::Registration::Registered);

				Assert::IsTrue(rbLag.ScheduleExternal(30u));
				devLag.done[0] = 1;
				Assert::IsTrue(rbLag.ScheduleExternal(31u));
				devLag.done[1] = 1;
				rbLag.Poll();
				Assert::IsTrue(rbLag.HasReady(30u) && rbLag.HasReady(31u),
					L"precondition: both frame 30 and frame 31 are Ready in ring and uncached in memo");
				Assert::AreEqual(uint64_t(0), memoLag.CountersOf(kEdgeMap).ownerFillCalls);

				const AcquireResult r31 = memoLag.Acquire(kEdgeMap, 31u, AcquireForm::Cpu);
				Assert::IsTrue(r31.Ok() && r31.status == AcquireStatus::FilledAtRead && r31.cpuBytes == kPayloadBytes,
					L"HIGH-2: Acquire(kEdgeMap, 31, Cpu) succeeds");
				Assert::IsTrue(rbLag.HasReady(30u), L"HIGH-2: Ready frame 30 preserved after Take(31)");
				Assert::AreEqual(uint64_t(0), rbLag.Counters().staleDropped, L"HIGH-2: zero stale drops after Take(31)");

				const AcquireResult r30 = memoLag.Acquire(kEdgeMap, 30u, AcquireForm::Cpu);
				Assert::IsTrue(r30.Ok() && r30.status == AcquireStatus::FilledAtRead && r30.cpuBytes == kPayloadBytes,
					L"HIGH-2: Acquire(kEdgeMap, 30, Cpu) still succeeds after Acquire(31)");
				float v31 = 0.0f, v30 = 0.0f;
				std::memcpy(&v31, r31.cpu, sizeof(float));
				std::memcpy(&v30, r30.cpu, sizeof(float));
				Assert::AreEqual(31000.0f, v31, 0.0f);
				Assert::AreEqual(30000.0f, v30, 0.0f);
				Assert::AreEqual(uint64_t(2), rbLag.Counters().taken);
				Assert::AreEqual(uint64_t(0), rbLag.Counters().staleDropped);
				rbLag.SetDemand(false);
			}
		}

		// W23-CARBON-R2-REG-FIX BLOCKER-1 (24d188643):
		// (1) VpvGpu::CodeUnit returns 0.0 without crashing when tex == nullptr or tex->params.format == nullptr.
		// (2) FillEdgeMapDefault checks P13 SourcePlanes::FactsOf and VpvGpu::CodeUnit AFTER req.gpu->Allocate,
		//     declining cleanly with visible reasons ("edge_map: missing P13 source plane facts" /
		//     "edge_map: invalid source plane code unit") and ZERO access violation (0xC0000005).
		TEST_METHOD(FillDeclinesCleanlyOnNullTextureFormatAndMissingP13FactsWithoutCrash)
		{
			using namespace VpSkeleton;
			using Vpv::Capture::SourcePlanes;

			pl_bit_encoding bits10{};
			bits10.sample_depth = 16;
			bits10.color_depth = 10;
			bits10.bit_shift = 6;
			pl_tex_t nullFmtTex{};
			nullFmtTex.params.w = 64;
			nullFmtTex.params.h = 48;
			nullFmtTex.params.format = nullptr;
			Assert::AreEqual(0.0, VpvGpu::CodeUnit(nullptr, bits10, 10), 0.0, L"CodeUnit(nullptr) == 0.0");
			Assert::AreEqual(0.0, VpvGpu::CodeUnit(&nullFmtTex, bits10, 10), 0.0, L"CodeUnit(format == nullptr) == 0.0 without crash");

			// Negative control: same pl_tex_t with a valid UNORM16 format descriptor yields exact positive CodeUnit (64 / 65535).
			pl_fmt_t validFmt{};
			validFmt.type = PL_FMT_UNORM;
			validFmt.num_components = 1;
			validFmt.component_depth[0] = 16;
			pl_tex_t validFmtTex = nullFmtTex;
			validFmtTex.params.format = &validFmt;
			Assert::AreEqual(64.0 / 65535.0, VpvGpu::CodeUnit(&validFmtTex, bits10, 10), 1e-12,
				L"negative control: valid UNORM16 format yields exact CodeUnit 64/65535");

			// Dummy allocating device so FillEdgeMapDefault reaches post-Allocate P13 facts & CodeUnit guards.
			class DummyAllocDevice : public IOwnerGpuDevice
			{
			public:
				int gpuTag = 0;
				int dispatchTag = 0;
				pl_tex_t dummyOut{};
				int allocated = 0;
				void* OwnerGpu() const override { return const_cast<int*>(&gpuTag); }
				void* OwnerDispatch() const override { return const_cast<int*>(&dispatchTag); }
				bool CreateOwnerTexture(const TexShape& shape, GpuMemoHandle& out, std::string&) override
				{
					++allocated;
					dummyOut.params.w = static_cast<int>(shape.w);
					dummyOut.params.h = static_cast<int>(shape.h);
					out.resource = &dummyOut;
					out.width = shape.w;
					out.height = shape.h;
					out.format = static_cast<uint32_t>(shape.fmt);
					out.bytes = ShapeBytes(shape);
					return true;
				}
			};

			const uint64_t reserve = Vpv::Regions::EdgeMapReserveBytes(DimsOf(64, 48));
			const OwnerFill f = Vpv::Regions::MakeEdgeMapOwnerFill(reserve);

			// Case A: Offered P13 frame with luma.params.format == nullptr allocates output then declines cleanly on CodeUnit <= 0.
			{
				int host = 0;
				pl_frame image{};
				image.num_planes = 1;
				image.planes[0].texture = &nullFmtTex;
				image.planes[0].components = 1;
				image.repr.sys = PL_COLOR_SYSTEM_BT_709;
				image.repr.levels = PL_COLOR_LEVELS_LIMITED;
				image.repr.bits = bits10;

				SkeletonMemo m;
				m.SetHost(&host);
				m.SetRenderThread(std::this_thread::get_id());
				Assert::IsTrue(m.RegisterOwnerFill(kP13SourceRing, AcquireForm::Gpu, SourcePlanes::kUnit, SourcePlanes::MakeOwnerFill())
					== SkeletonMemo::Registration::Registered);
				Assert::IsTrue(SourcePlanes::OfferFrame(&host, 11, 1, image));

				DummyAllocDevice allocDev;
				OwnerGpuContext ctx(&allocDev, reserve);
				OwnerFillRequest req;
				req.host = &host;
				req.id = kEdgeMap;
				req.seq = 11;
				req.form = AcquireForm::Gpu;
				req.gpu = &ctx;
				OwnerFillOutput out;
				Assert::IsFalse(f.fill(req, m, out), L"null format must decline cleanly without crashing");
				Assert::AreEqual(1, allocDev.allocated, L"allocation ran before CodeUnit guard");
				Assert::AreEqual(std::string("edge_map: invalid source plane code unit"), out.declineReason,
					L"exact visible decline reason on null format");
				SourcePlanes::Forget(&host);
			}

			// Case B: P13 view returned without SourcePlanes::FactsOf (missing P13 facts) declines cleanly after Allocate.
			{
				int host = 0;
				SkeletonMemo m;
				m.SetHost(&host);
				m.SetRenderThread(std::this_thread::get_id());
				OwnerFill customP13;
				customP13.view = true;
				customP13.frameScoped = true;
				customP13.fill = [&validFmtTex](const OwnerFillRequest&, SkeletonMemo&, OwnerFillOutput& out) {
					out.gpu.resource = &validFmtTex;
					out.gpu.width = 64;
					out.gpu.height = 48;
					return true;
				};
				Assert::IsTrue(m.RegisterOwnerFill(kP13SourceRing, AcquireForm::Gpu, SourcePlanes::kUnit, std::move(customP13))
					== SkeletonMemo::Registration::Registered);

				DummyAllocDevice allocDev;
				OwnerGpuContext ctx(&allocDev, reserve);
				OwnerFillRequest req;
				req.host = &host;
				req.id = kEdgeMap;
				req.seq = 12;
				req.form = AcquireForm::Gpu;
				req.gpu = &ctx;
				OwnerFillOutput out;
				Assert::IsFalse(f.fill(req, m, out), L"missing P13 facts must decline cleanly");
				Assert::AreEqual(1, allocDev.allocated, L"allocation ran before P13 facts guard");
				Assert::AreEqual(std::string("edge_map: missing P13 source plane facts"), out.declineReason,
					L"exact visible decline reason on missing P13 facts");
			}
		}
	};
}
