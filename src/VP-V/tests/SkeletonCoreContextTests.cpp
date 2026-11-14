// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "CppUnitTest.h"

// L-CORE (lead8/iface-L-CORE.md): the skeleton current-frame context and B1 production readiness.
//   1. CurrentFrame: recorded by the hub from the FrameContext path per phase (render phases only), sticky per thread, read
//      torn-free from another thread, forgotten with the host;
//   2. MemoFor WITHOUT the analysis lane: the renderer's memo, module-wide owner registrations (also into memos made later),
//      sealed at every Begin, never in the Original pathway, the memo's Unavailable entries on the Fallbacks status;
//   3. lane binding by token: a newer lane's binding survives the older lane's end; module registrations stay;
//   4. VariantKey STAGE / CROP / PARAM0-1 / PRECISION: canonical, hashed, text, served or refused, ledger shapes;
//   5. thread contract: CPU forms from a producer thread (pinned, nested prerequisite published), GPU and render-bound fills
//      refused off the render thread, a lane product's published bytes copied out off the render thread, kept frames,
//      two memos on one thread are not re-entrant (G0 S2);
//   6. P13 source-plane views: offered planes served by plane, frame-scoped, declines say why;
//   7. seal == Acquire (one law) for the CPU lane products, and the view is the offered plane (lead 8 on 102b B1-SEAL-VIA-ACQUIRE);
//   8. the memo's VRAM ledger grant without a lane: granted / refused visibly (lead 8 on 102b P13-NO-LANE-GPU-ALLOC).
// CPU only (no GPU object is touched: textures are plain structs), single process, every test bounded.

#include <VP-V/core/AnalysisLane.h>
#include <VP-V/core/MemoGpuGrant.h>
#include <VP-V/core/PipelineSlots.h>
#include <VP-V/core/SkeletonAcquire.h>
#include <VP-V/core/SkeletonAcquireLane.h>
#include <VP-V/core/VariantShape.h>
#include <VP-V/core/hub/CurrentFrame.h>
#include <VP-V/core/hub/ExtensionHub.h>
#include <VP-V/modules/control/skeleton/SkeletonModule.h>
#include <VP-V/modules/input/capture/SourcePlanes.h>
#include <VP-V/modules/input/source_analysis/lane/LaneCpuProducts.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace SkeletonCoreContextTestsNs
{
	using namespace VpSkeleton;
	namespace RE = RendererExtension;

	const char* const kGrainUnit = "repair/grain_split";              // P10's owner (ProductRows)
	const char* const kRegionUnit = "input/source_analysis/regions";  // P11
	const char* const kLaneUnit = "input/source_analysis/lane";       // P1 / P4

	bool Has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

	OwnerFill CpuFill(std::atomic<int>* calls, uint8_t value, uint32_t axes = 0)
	{
		OwnerFill f;
		f.variantAxes = axes;
		f.fill = [calls, value](const OwnerFillRequest& r, SkeletonMemo&, OwnerFillOutput& out) {
			calls->fetch_add(1);
			out.bytes.assign(16, value);
			out.bytes[1] = uint8_t(r.seq);
			out.bytes[2] = uint8_t(r.variant.stage);
			return true;
		};
		return f;
	}

	template <class F>
	bool RunsWithin(F f, int ms)
	{
		std::future<void> done = std::async(std::launch::async, f);
		return done.wait_for(std::chrono::milliseconds(ms)) == std::future_status::ready;
	}

	RE::FrameContext Phase(const void* host, RE::FramePhase phase, int64_t seq, int64_t next = -1, int64_t gen = 0)
	{
		RE::FrameContext c;
		c.host = host;
		c.phase = phase;
		c.seq = seq;
		c.nextSeq = next;
		c.generation = gen;
		return c;
	}

	TEST_CLASS(SkeletonCurrentFrameTests)
	{
	public:
		TEST_METHOD(RecordedOnRenderPhasesStickyPerThreadAndReadableElsewhere)
		{
			int host = 0;
			Vpv::Hub::ResetThisThreadFrameForTesting();
			Assert::IsFalse(Vpv::Hub::ThisThreadFrame().Valid(), L"nothing rendered on this thread yet");
			Vpv::Hub::RecordCurrentFrame(Phase(&host, RE::FramePhase::Begin, 41));
			Vpv::Hub::CurrentFrame f = Vpv::Hub::ThisThreadFrame();
			Assert::IsTrue(f.Valid() && f.host == &host && f.seq == 41 && f.nextSeq == -1, L"Begin: the frame being rendered");
			const uint64_t serial = f.serial;
			Vpv::Hub::RecordCurrentFrame(Phase(&host, RE::FramePhase::Entrance, 41, 42, 7));
			f = Vpv::Hub::ThisThreadFrame();
			Assert::IsTrue(f.seq == 41 && f.nextSeq == 42 && f.generation == 7 && f.serial == serial, L"Entrance completes the frame");
			// Phases of other threads / other frames never move it.
			Vpv::Hub::RecordCurrentFrame(Phase(&host, RE::FramePhase::Convert, 50));
			Vpv::Hub::RecordCurrentFrame(Phase(&host, RE::FramePhase::CollectAhead, 51));
			Vpv::Hub::RecordCurrentFrame(Phase(&host, RE::FramePhase::Draw, -1));
			Assert::IsTrue(Vpv::Hub::ThisThreadFrame().seq == 41, L"Convert / CollectAhead / seq -1 do not record");
			// Sticky: a pass body after CollectHooks sees the frame.
			Vpv::Hub::RecordCurrentFrame(Phase(&host, RE::FramePhase::CollectHooks, 41, 42, 7));
			Assert::IsTrue(Vpv::Hub::ThisThreadFrame().nextSeq == 42, L"sticky after CollectHooks");
			// Another thread: no frame of its own, but the host's record is readable.
			Vpv::Hub::CurrentFrame other, ofHost;
			std::thread t([&]() { other = Vpv::Hub::ThisThreadFrame(); ofHost = Vpv::Hub::CurrentFrameOf(&host); });
			t.join();
			Assert::IsFalse(other.Valid(), L"a thread that never rendered has no current frame");
			Assert::IsTrue(ofHost.Valid() && ofHost.seq == 41 && ofHost.nextSeq == 42 && ofHost.generation == 7, L"the host's record from any thread");
			// A cadence repeat (Begin of the same seq) is a new serial; nextSeq resets until the Entrance.
			Vpv::Hub::RecordCurrentFrame(Phase(&host, RE::FramePhase::Begin, 41));
			f = Vpv::Hub::ThisThreadFrame();
			Assert::IsTrue(f.serial == serial + 1 && f.nextSeq == -1 && f.generation == 7, L"repeat = new serial, generation kept");
			Vpv::Hub::ForgetCurrentFrame(&host);
			Assert::IsFalse(Vpv::Hub::ThisThreadFrame().Valid() || Vpv::Hub::CurrentFrameOf(&host).Valid(), L"forgotten with the host");
		}

		TEST_METHOD(TheHubRecordsBeforeTheFanOut)
		{
			struct Probe : Vpv::Hub::ISkeletonModule
			{
				const char* Name() const override { return "probe"; }
				bool OnFrame(RE::FrameContext&) override { seen = Vpv::Hub::ThisThreadFrame(); return false; }
				Vpv::Hub::CurrentFrame seen;
			};
			// The hub records without any module too (no module list needed for the record).
			int host = 0;
			Vpv::Hub::ResetThisThreadFrameForTesting();
			RE::FrameContext c = Phase(&host, RE::FramePhase::AfterRender, 9, -1, 2);
			Vpv::Hub::ExtensionHub::Instance().OnFrame(c);
			const Vpv::Hub::CurrentFrame f = Vpv::Hub::ThisThreadFrame();
			Assert::IsTrue(f.host == &host && f.seq == 9 && f.generation == 2, L"ExtensionHub::OnFrame records the frame");
			Vpv::Hub::ForgetCurrentFrame(&host);
		}
	};

	TEST_CLASS(SkeletonMemoWithoutLaneTests)
	{
	public:
		TEST_METHOD(MemoForServesOwnerFillsWithoutTheLaneAndSealsAtBegin)
		{
			Vpv::Skeleton::SkeletonModule sk([](ConfigFile&) { return false; });   // no file: analysis_lane off (defaults)
			int host = 0;
			Assert::IsTrue(sk.MemoOf(&host) == nullptr, L"no memo before anyone asks (off = zero cost)");
			std::atomic<int> calls{ 0 };
			Assert::IsTrue(sk.RegisterOwnerFill(kP10GrainSplit, AcquireForm::Cpu, kGrainUnit, CpuFill(&calls, 0x11))
				== SkeletonMemo::Registration::Registered, L"module-wide registration");
			Assert::IsTrue(sk.RegisterOwnerFill(kP10GrainSplit, AcquireForm::Cpu, kGrainUnit, CpuFill(&calls, 0x11))
				== SkeletonMemo::Registration::AlreadyRegistered, L"one per (product, form)");
			Assert::IsTrue(sk.RegisterOwnerFill(kP10GrainSplit, AcquireForm::Cpu, "control/skeleton", CpuFill(&calls, 0x11))
				== SkeletonMemo::Registration::NotOwner, L"only the catalog's unit");
			OwnerFill staged = CpuFill(&calls, 0x11);
			staged.defaults.stage = 3;
			Assert::IsTrue(sk.RegisterOwnerFill(kP11RegionBlockStats, AcquireForm::Cpu, kRegionUnit, staged)
				== SkeletonMemo::Registration::BadDefaults, L"an owner's own form is the source planes");

			Vpv::Skeleton::FrameMemo fm = sk.MemoFor(&host, 5);
			Assert::IsTrue(fm.Valid() && fm.seq == 5 && sk.MemoOf(&host) == fm.memo.get(), L"the renderer's memo exists without a lane");
			const AcquireResult a = fm.memo->Acquire(kP10GrainSplit, 5, AcquireForm::Cpu);
			const AcquireResult b = fm.memo->Acquire(kP10GrainSplit, 5, AcquireForm::Cpu);
			Assert::IsTrue(a.status == AcquireStatus::FilledAtRead && b.status == AcquireStatus::Reused && calls.load() == 1, L"get-or-compute-and-publish, once");

			// A registration made after the memo exists reaches it.
			std::atomic<int> late{ 0 };
			Assert::IsTrue(sk.RegisterOwnerFill(kP11RegionBlockStats, AcquireForm::Cpu, kRegionUnit, CpuFill(&late, 0x22))
				== SkeletonMemo::Registration::Registered, L"late registration");
			Assert::IsTrue(fm.memo->Acquire(kP11RegionBlockStats, 5, AcquireForm::Cpu).Ok() && late.load() == 1, L"installed in the existing memo");

			// Begin (no lane) seals the memo: frame 5 is released at the seal of 6.
			RE::FrameContext begin6 = Phase(&host, RE::FramePhase::Begin, 6);
			sk.OnFrame(begin6);
			Assert::IsTrue(fm.memo->Seals() == 1 && fm.memo->Released(5), L"sealed at Begin without the lane");
			Assert::IsTrue(fm.memo->Acquire(kP10GrainSplit, 5, AcquireForm::Cpu).cause == AcquireCause::FrameReleased, L"never recomputed");
			Assert::IsTrue(a.cpu != nullptr && a.cpu[0] == 0x11, L"a pinned result outlives the release");

			// A GPU form without a lane: visible cause, on the Fallbacks status of the renderer.
			Assert::IsTrue(fm.memo->Acquire(kP12LightLevel, 6, AcquireForm::Gpu).cause == AcquireCause::NoOwnerFill, L"no owner");
			std::vector<std::string> lines;
			RE::Context ctx;
			ctx.host = &host;
			sk.OnStatus(ctx, RE::StatusKind::Fallbacks, lines, nullptr, nullptr);
			bool shown = false;
			for (const std::string& l : lines)
				shown = shown || Has(l, "skeleton acquire");
			Assert::IsTrue(shown, L"the memo's Unavailable entries are shown without the lane");

			Assert::IsTrue(sk.UnregisterOwnerFill(kP11RegionBlockStats, AcquireForm::Cpu, kRegionUnit) && !fm.memo->HasOwnerFill(kP11RegionBlockStats, AcquireForm::Cpu), L"unregistered everywhere");
			RE::LifecycleFacts rel;
			rel.host = &host;
			rel.kind = RE::Lifecycle::GpuReleased;
			sk.OnLifecycle(rel);
			Assert::IsTrue(sk.MemoOf(&host) == nullptr && fm.memo, L"dropped with the renderer; a holder keeps its copy");
		}

		TEST_METHOD(NoMemoInTheOriginalPathway)
		{
			Vpv::Skeleton::SkeletonModule sk([](ConfigFile&) { return false; });
			int host = 0;
			Vpv::Hub::ExtensionHub& hub = Vpv::Hub::ExtensionHub::Instance();
			Assert::IsTrue(hub.NoteOriginalPathway(&host, true), L"pathway fact recorded");
			Assert::IsFalse(sk.MemoFor(&host, 1).Valid(), L"Original == Bill: no memo");
			Assert::IsTrue(sk.MemoOf(&host) == nullptr, L"nothing made");
			hub.NoteOriginalPathway(&host, false);
			Assert::IsTrue(sk.MemoFor(&host, 1).Valid(), L"VP-V pathway: the memo");
			RE::LifecycleFacts rel;
			rel.host = &host;
			rel.kind = RE::Lifecycle::Shutdown;
			hub.OnLifecycle(rel);   // drops the pathway fact (the hub's bookkeeping)
			sk.OnLifecycle(rel);
		}

		TEST_METHOD(LaneBindingIsByTokenAndModuleFillsStay)
		{
			SkeletonMemo m;
			std::atomic<int> moduleCalls{ 0 }, laneACalls{ 0 }, laneBCalls{ 0 };
			m.RegisterOwnerFill(kP1HalfLuma, AcquireForm::Cpu, kLaneUnit, CpuFill(&moduleCalls, 0x01));
			int laneA = 0, laneB = 0;
			auto bind = [&m](int* token, std::atomic<int>* calls, uint8_t value) {
				LaneSlotSource src;
				src.token = token;
				src.read = [](ProductId, uint64_t, AcquireForm, LaneSlotSource::Read*) { return LaneSlotSource::State::Absent; };
				std::map<std::pair<ProductId, AcquireForm>, OwnerFill> fills;
				fills[std::make_pair(kP1HalfLuma, AcquireForm::Cpu)] = CpuFill(calls, value);
				m.BindLane(src, fills);
			};
			bind(&laneA, &laneACalls, 0xA0);
			Assert::IsTrue(m.Acquire(kP1HalfLuma, 1, AcquireForm::Cpu).cpu[0] == 0xA0, L"the bound lane's fill wins");
			bind(&laneB, &laneBCalls, 0xB0);   // a rebuild binds the new lane before the old one ends
			Assert::IsFalse(m.UnbindLane(&laneA), L"the old lane's end leaves the newer binding");
			Assert::IsTrue(m.Acquire(kP1HalfLuma, 2, AcquireForm::Cpu).cpu[0] == 0xB0, L"lane B serves");
			Assert::IsTrue(m.UnbindLane(&laneB) && !m.LaneBound(), L"lane B ends");
			Assert::IsTrue(m.Acquire(kP1HalfLuma, 3, AcquireForm::Cpu).cpu[0] == 0x01, L"the module registration serves again");
			Assert::IsTrue(laneACalls == 1 && laneBCalls == 1 && moduleCalls == 1 && m.LaneBinds() == 2, L"each fill ran once");
		}
	};

	TEST_CLASS(SkeletonVariantAxesTests)
	{
	public:
		TEST_METHOD(StageCropParamsCanonicalHashedAndText)
		{
			VariantKey d;   // the owner's defaults: a 1920x1080 frame
			d.width = 1920;
			d.height = 1080;
			VariantKey k;
			k.stage = VariantKey::StageAfter(uint8_t(PipelineSlots::Id::RepairDeband));
			k.cropX = 0;
			k.cropY = 0;
			k.cropW = 1920;
			k.cropH = 1080;   // the full frame: no crop
			k.param0 = 48;
			const VariantKey c = k.Canonical(d);
			Assert::IsTrue(c.stage == uint8_t(PipelineSlots::Id::RepairDeband) + 1u && !c.HasCrop() && c.param0 == 48, L"full-frame crop is no crop");
			Assert::IsTrue((c.Axes() & VariantKey::kAxisStage) && (c.Axes() & VariantKey::kAxisParam0) && !(c.Axes() & VariantKey::kAxisCrop), L"axes");
			uint8_t slot = 0;
			Assert::IsTrue(VariantKey::StageSlot(c.stage, &slot) && slot == uint8_t(PipelineSlots::Id::RepairDeband) && !VariantKey::StageSlot(0, nullptr), L"stage decode");
			VariantKey crop;
			crop.cropX = 100;
			crop.cropY = 50;
			crop.cropW = 640;
			crop.cropH = 360;
			crop.param1 = 7;
			crop.precision = kPrecisionFloat32;
			const VariantKey cc = crop.Canonical(d);
			Assert::IsTrue(cc.HasCrop() && (cc.Axes() & VariantKey::kAxisCrop) && (cc.Axes() & VariantKey::kAxisParam1), L"a real crop is an axis");
			Assert::IsTrue(Has(cc.Text(), "crop_w=640") && Has(cc.Text(), "p1=7") && Has(cc.Text(), "precision=float32") && Has(c.Text(), "p0=48") && Has(c.Text(), "stage="), L"text");
			Assert::IsTrue(VariantKey().Hash() == 0 && c.Hash() != 0 && cc.Hash() != 0 && c.Hash() != cc.Hash(), L"hash: 0 = default, distinct");
			VariantKey c2 = cc;
			Assert::IsTrue(c2.Hash() == cc.Hash() && c2 == cc && !(c2 < cc) && !(cc < c2), L"stable");
			c2.param1 = 8;
			Assert::IsTrue(c2 != cc && c2.Hash() != cc.Hash() && (cc < c2), L"param1 is in ==, < and the hash");
			VariantKey zeroSide = crop;
			zeroSide.cropH = 0;
			Assert::IsFalse(zeroSide.Canonical(d).HasCrop(), L"a crop with a zero side is none");
			Assert::IsTrue(PrecisionTwelveBitSafe(kPrecisionUnorm16) && PrecisionTwelveBitSafe(kPrecisionFloat32) && !PrecisionTwelveBitSafe(kPrecisionFloat16), L"12-bit rule");
		}

		TEST_METHOD(AxesServedOrRefusedByTheOwner)
		{
			SkeletonMemo m;
			std::atomic<int> calls{ 0 };
			OwnerFill f = CpuFill(&calls, 0x33, VariantKey::kAxisStage | VariantKey::kAxisCrop | VariantKey::kAxisPrecision);
			f.pixelData = true;
			m.RegisterOwnerFill(kP10GrainSplit, AcquireForm::Cpu, kGrainUnit, f);
			VariantKey stage4;
			stage4.stage = 4;
			const AcquireResult a = m.Acquire(kP10GrainSplit, 1, AcquireForm::Cpu, stage4);
			Assert::IsTrue(a.status == AcquireStatus::FilledAtRead && a.cpu[2] == 4, L"a processed-plane stage served by the owner");
			VariantKey crop;
			crop.cropX = 8;
			crop.cropY = 8;
			crop.cropW = 32;
			crop.cropH = 16;
			Assert::IsTrue(m.Acquire(kP10GrainSplit, 1, AcquireForm::Cpu, crop).Ok(), L"a TD crop request is SERVED (declared axis)");
			VariantKey half;
			half.precision = kPrecisionFloat16;
			Assert::IsTrue(m.Acquire(kP10GrainSplit, 1, AcquireForm::Cpu, half).cause == AcquireCause::VariantUndeclared, L"pixel data never at fp16");
			VariantKey p0;
			p0.param0 = 3;
			Assert::IsTrue(m.Acquire(kP10GrainSplit, 1, AcquireForm::Cpu, p0).cause == AcquireCause::VariantUndeclared, L"an undeclared axis is refused, visibly");
			Assert::IsTrue(calls.load() == 2, L"one fill per served variant");
		}

		TEST_METHOD(VariantShapesFollowTheAxes)
		{
			const Dims full{ 1920, 1080 };
			VariantKey k;
			k.cropX = 1800;
			k.cropY = 1000;
			k.cropW = 400;
			k.cropH = 400;   // clipped at the frame edge
			Dims d = VariantDims(k, full);
			Assert::IsTrue(d.width == 120 && d.height == 80, L"crop clipped to the frame");
			k.scale = 1;
			d = VariantDims(k, full);
			Assert::IsTrue(d.width == 60 && d.height == 40, L"scale halves after the crop");
			VariantKey p1;
			p1.width = 960;
			p1.height = 540;
			p1.precision = kPrecisionUnorm16;
			TexShape s;
			Assert::IsTrue(VariantTexShape(p1, full, 1, TexFormat::R16F, &s) && s.w == 960 && s.h == 540 && s.fmt == TexFormat::R16, L"unorm16 = R16");
			Assert::IsTrue(VariantReserveBytes(p1, full, 1, TexFormat::R16F) == 960u * 540u * 2u, L"ledger bytes");
			p1.precision = kPrecisionFloat32;
			Assert::IsTrue(VariantReserveBytes(p1, full, 1, TexFormat::R16F) == 960u * 540u * 4u, L"float32 = R32F");
			p1.precision = kPrecisionUnorm16;
			Assert::IsFalse(VariantTexShape(p1, full, 4, TexFormat::RGBA16F, &s), L"no 4-channel unorm16 in the ledger: refused");
		}
	};

	TEST_CLASS(SkeletonThreadContractTests)
	{
	public:
		TEST_METHOD(CpuFormsFromAProducerThreadWithNestedPrerequisite)
		{
			auto m = std::make_shared<SkeletonMemo>();
			m->SetRenderThread(std::this_thread::get_id());
			std::atomic<int> aCalls{ 0 }, bCalls{ 0 };
			std::atomic<bool> aOnProducer{ false };
			const std::thread::id render = std::this_thread::get_id();
			OwnerFill fa = CpuFill(&aCalls, 0xA5);
			auto innerA = fa.fill;
			fa.fill = [innerA, render, &aOnProducer](const OwnerFillRequest& r, SkeletonMemo& mm, OwnerFillOutput& out) {
				aOnProducer = std::this_thread::get_id() != render;
				return innerA(r, mm, out);
			};
			OwnerFill fb;
			fb.fill = [&bCalls](const OwnerFillRequest& r, SkeletonMemo& mm, OwnerFillOutput& out) {
				bCalls.fetch_add(1);
				const AcquireResult a = mm.Acquire(kP10GrainSplit, r.seq, AcquireForm::Cpu);   // the prerequisite, on the same thread
				if (!a.Ok())
					return false;
				out.bytes.assign(a.cpu, a.cpu + a.cpuBytes);
				out.bytes[0] ^= 0xFF;
				return true;
			};
			m->RegisterOwnerFill(kP10GrainSplit, AcquireForm::Cpu, kGrainUnit, fa);
			m->RegisterOwnerFill(kP11RegionBlockStats, AcquireForm::Cpu, kRegionUnit, fb);
			AcquireResult b;
			const bool ran = RunsWithin([m, &b]() { b = m->Acquire(kP11RegionBlockStats, 9, AcquireForm::Cpu); }, 5000);
			Assert::IsTrue(ran && b.status == AcquireStatus::FilledAtRead && b.cpu[0] == uint8_t(0xA5 ^ 0xFF), L"B filled on the producer thread from A");
			Assert::IsTrue(aOnProducer.load(), L"A computed on the asking producer thread");
			const AcquireResult a2 = m->Acquire(kP10GrainSplit, 9, AcquireForm::Cpu);   // the render thread reads A
			Assert::IsTrue(a2.status == AcquireStatus::Reused && aCalls == 1 && bCalls == 1, L"A published on the way, reused, computed once");
			m->OnSeal(10);
			m->OnSeal(11);
			Assert::IsTrue(b.cpu[0] == uint8_t(0xA5 ^ 0xFF), L"pinned past the release");
		}

		TEST_METHOD(GpuAndRenderBoundFillsAreRefusedOffTheRenderThread)
		{
			SkeletonMemo m;
			m.SetRenderThread(std::this_thread::get_id());
			m.SetGpuDispatcherPresent(true);
			m.SetGpuBudget(true, 1u << 20);
			int gpuCalls = 0;
			OwnerFill g;
			g.fill = [&gpuCalls](const OwnerFillRequest&, SkeletonMemo&, OwnerFillOutput& out) {
				++gpuCalls;
				static int tex = 0;
				out.gpu.resource = &tex;
				out.gpu.bytes = 64;
				return true;
			};
			m.RegisterOwnerFill(kP10GrainSplit, AcquireForm::Gpu, kGrainUnit, g);
			std::atomic<int> rcalls{ 0 };
			OwnerFill bound = CpuFill(&rcalls, 0x44);
			bound.renderThreadOnly = true;
			m.RegisterOwnerFill(kP11RegionBlockStats, AcquireForm::Cpu, kRegionUnit, bound);
			AcquireResult gOff, rOff;
			std::thread t([&]() {
				gOff = m.Acquire(kP10GrainSplit, 1, AcquireForm::Gpu);
				rOff = m.Acquire(kP11RegionBlockStats, 1, AcquireForm::Cpu);
			});
			t.join();
			Assert::IsTrue(gOff.cause == AcquireCause::WrongThread && rOff.cause == AcquireCause::WrongThread, L"refused off the render thread");
			Assert::IsTrue(Has(m.LastCause(kP11RegionBlockStats), "render thread"), L"visible cause");
			Assert::IsTrue(m.Acquire(kP10GrainSplit, 1, AcquireForm::Gpu).Ok() && m.Acquire(kP11RegionBlockStats, 1, AcquireForm::Cpu).Ok(), L"served on the render thread");
			Assert::IsTrue(gpuCalls == 1 && rcalls == 1, L"once each");
		}

		TEST_METHOD(LanePublishedBytesCopiedOutOffTheRenderThread)
		{
			SkeletonMemo m;
			m.SetRenderThread(std::this_thread::get_id());
			int token = 0;
			std::atomic<int> laneFill{ 0 }, renderReads{ 0 };
			LaneSlotSource src;
			src.token = &token;
			src.read = [&renderReads](ProductId, uint64_t, AcquireForm, LaneSlotSource::Read*) { renderReads.fetch_add(1); return LaneSlotSource::State::Absent; };
			src.copyPublished = [](ProductId id, uint64_t seq, std::vector<uint8_t>* copy) {
				if (id != kP1HalfLuma)
					return LaneSlotSource::State::NotLaneProduct;
				if (seq != 4)
					return LaneSlotSource::State::Absent;
				copy->assign(32, 0x5A);
				return LaneSlotSource::State::Present;
			};
			std::map<std::pair<ProductId, AcquireForm>, OwnerFill> fills;
			OwnerFill lf = CpuFill(&laneFill, 0x66);
			lf.renderThreadOnly = true;
			fills[std::make_pair(kP1HalfLuma, AcquireForm::Cpu)] = lf;
			m.BindLane(src, fills);
			AcquireResult published, notYet;
			std::thread t([&]() {
				published = m.Acquire(kP1HalfLuma, 4, AcquireForm::Cpu);
				notYet = m.Acquire(kP1HalfLuma, 5, AcquireForm::Cpu);
			});
			t.join();
			Assert::IsTrue(published.status == AcquireStatus::Reused && published.cpuBytes == 32 && published.cpu[0] == 0x5A, L"published bytes copied out");
			Assert::IsTrue(notYet.cause == AcquireCause::WrongThread, L"not published + render-bound fill: refused, visible");
			Assert::IsTrue(m.CountersOf(kP1HalfLuma).reusedLaneCopy == 1 && laneFill == 0 && renderReads == 0, L"no recompute, no render-state read off thread");
		}

		TEST_METHOD(KeptFramesAndTwoMemosOnOneThread)
		{
			SkeletonMemo m;
			std::atomic<int> calls{ 0 };
			OwnerFill kept = CpuFill(&calls, 0x77);
			kept.keepFrames = 1;
			m.RegisterOwnerFill(kP10GrainSplit, AcquireForm::Cpu, kGrainUnit, kept);
			m.OnSeal(5);
			Assert::IsTrue(m.Acquire(kP10GrainSplit, 5, AcquireForm::Cpu).Ok(), L"frame 5");
			m.OnSeal(6);
			Assert::IsTrue(m.Acquire(kP10GrainSplit, 5, AcquireForm::Cpu).status == AcquireStatus::Reused, L"kept one frame: A = the previous B");
			m.OnSeal(7);
			Assert::IsTrue(m.Acquire(kP10GrainSplit, 5, AcquireForm::Cpu).cause == AcquireCause::FrameReleased && calls == 1, L"then released, never recomputed");

			// G0 S2: a fill of memo 1 asking memo 2 for the same key on this thread is not re-entrant.
			SkeletonMemo m1, m2;
			std::atomic<int> c2{ 0 };
			m2.RegisterOwnerFill(kP10GrainSplit, AcquireForm::Cpu, kGrainUnit, CpuFill(&c2, 0x12));
			OwnerFill f1;
			f1.fill = [&m2](const OwnerFillRequest& r, SkeletonMemo&, OwnerFillOutput& out) {
				const AcquireResult x = m2.Acquire(kP10GrainSplit, r.seq, AcquireForm::Cpu);
				if (!x.Ok())
					return false;
				out.bytes.assign(x.cpu, x.cpu + x.cpuBytes);
				return true;
			};
			m1.RegisterOwnerFill(kP10GrainSplit, AcquireForm::Cpu, kGrainUnit, f1);
			const AcquireResult r = m1.Acquire(kP10GrainSplit, 3, AcquireForm::Cpu);
			Assert::IsTrue(r.Ok() && r.cpu[0] == 0x12 && c2 == 1, L"per-memo re-entrancy guard");
		}
	};

	TEST_CLASS(SourcePlaneProviderTests)
	{
	public:
		TEST_METHOD(OfferedPlanesServedByPlaneFrameScoped)
		{
			using Vpv::Capture::SourcePlanes;
			using Vpv::Capture::SourcePlaneSet;
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
				== SkeletonMemo::Registration::Registered, L"P13 owner = input/capture");
			Assert::IsTrue(SourcePlanes::OfferFrame(&host, 7, 1, image), L"the Entrance image offered");

			// No dispatcher, no ledger grant: a VIEW needs neither.
			const AcquireResult y = m.Acquire(kP13SourceRing, 7, AcquireForm::Gpu);
			Assert::IsTrue(y.status == AcquireStatus::FilledAtRead && y.gpu.resource == &luma && y.gpu.bytes == 0 && y.gpu.width == 64, L"luma view");
			const Vpv::Capture::SourcePlaneSet* facts = SourcePlanes::FactsOf(y);
			Assert::IsTrue(facts != nullptr && facts->seq == 7 && !facts->rgb && !facts->fullRange && facts->planes == 2, L"facts");
			VariantKey cplane;
			cplane.plane = 2;
			Assert::IsTrue(m.Acquire(kP13SourceRing, 7, AcquireForm::Gpu, cplane).gpu.resource == &chroma, L"chroma plane");
			Assert::IsTrue(m.Acquire(kP13SourceRing, 7, AcquireForm::Gpu).status == AcquireStatus::Reused && m.GpuBytes() == 0, L"reused, 0 bytes");
			VariantKey staged;
			staged.stage = 2;
			Assert::IsTrue(m.Acquire(kP13SourceRing, 7, AcquireForm::Gpu, staged).cause == AcquireCause::VariantUndeclared, L"source planes only");
			const AcquireResult absent = m.Acquire(kP13SourceRing, 8, AcquireForm::Gpu);
			Assert::IsTrue(absent.cause == AcquireCause::OwnerDeclined && Has(m.LastCause(kP13SourceRing), "seq not offered"), L"a frame not offered: says why");

			// Frame scope: the next Begin drops the offers and the next seal the memo views.
			SourcePlanes::BeginFrame(&host);
			m.OnSeal(8);
			Assert::IsTrue(m.Acquire(kP13SourceRing, 7, AcquireForm::Gpu).cause == AcquireCause::FrameReleased, L"the previous frame's views are gone");
			pl_frame rgb = image;
			rgb.num_planes = 1;
			rgb.repr.sys = PL_COLOR_SYSTEM_RGB;
			SourcePlanes::OfferFrame(&host, 8, 1, rgb);
			Assert::IsTrue(m.Acquire(kP13SourceRing, 8, AcquireForm::Gpu).cause == AcquireCause::OwnerDeclined && Has(m.LastCause(kP13SourceRing), "no luma plane"), L"RGB: no luma, said so");
			// A second uploader's offer of another frame in the same render frame (the matcher's B).
			SourcePlaneSet bset;
			bset.seq = 9;
			bset.planes = 1;
			bset.tex[0] = &luma;
			Assert::IsTrue(SourcePlanes::Offer(&host, bset) && m.Acquire(kP13SourceRing, 9, AcquireForm::Gpu).gpu.resource == &luma, L"B offered and served");
			m.OnSeal(9);   // frame 9's own seal: a kept spot of seq 9 would survive it; a FRAME-SCOPED view does not
			Assert::IsTrue(m.MemoCount() == 0, L"frame-scoped views released at the next seal whatever their seq");
			SourcePlanes::BeginFrame(&host);
			SourcePlanes::Offer(&host, bset);
			// Off the render thread: refused.
			AcquireResult off;
			std::thread t([&]() { off = m.Acquire(kP13SourceRing, 9, AcquireForm::Gpu, cplane); });
			t.join();
			Assert::IsTrue(off.cause == AcquireCause::WrongThread, L"render thread only");
			SourcePlanes::Forget(&host);
		}
	};

	// A minimal real lane (FirstProducts; CPU producers P1 + P4; one consumer of P4), as in SkeletonAcquireTests.
	constexpr uint32_t kW = 64;
	constexpr uint32_t kH = 48;
	struct MiniLane {
		Registry reg = FirstProducts();
		std::vector<ConsumerDecl> consumers;
		GraphSettings gs;
		CardProfile card;
		TierInput tier;
		std::atomic<uint64_t> clock{ 0 };
		VpIndicator::Indicator ind;
		AnalysisLane lane;
		VpLaneProducts::HalfLumaProducer p1;
		VpLaneProducts::SourceStatsProducer p4;
		std::vector<uint16_t> store;
		size_t frames = 16;

		MiniLane()
			: ind(7, 256)
			, lane(&ind, [this]() { return clock.fetch_add(3) + 3; })
			, p1([this]() { return clock.fetch_add(3) + 3; })
			, p4(Dims{ kW, kH }, [this]() { return clock.fetch_add(3) + 3; })
		{
			ConsumerDecl c;
			c.id = 1;
			AddConsume(c, kP4SourceStats);
			consumers.push_back(c);
			gs.dims.width = kW;
			gs.dims.height = kH;
			gs.renderAhead = 0;
			gs.laneLead = 1;
			gs.queueDepth = 4;
			gs.lookAheadWalk = 0;
			card.vramBudgetBytes = 1ull << 30;
			card.systemRamFreeBytes = 1ull << 30;
			card.uploadGBps = 10.0;
			card.computeScale = 1.0;
			tier.framePeriodMs = 40.0;
			store.resize(frames * kW * kH);
			uint32_t x = 9191u;
			for (size_t i = 0; i < store.size(); ++i) {
				x = x * 1664525u + 1013904223u;
				store[i] = uint16_t((x >> 12) & 0xFFFFu);
			}
			lane.RegisterProducer(&p1);
			lane.RegisterProducer(&p4);
			lane.RegisterCpuLiveCompute(kP1HalfLuma,
				[](const Node&, const SourceFrameView* v, const std::array<const uint8_t*, kMaxInputs>&, uint8_t* dst, size_t n) {
					return v != nullptr && VpLaneProducts::ComputeHalfLuma(*v, dst, n);
				});
			lane.RegisterCpuLiveCompute(kP4SourceStats,
				[](const Node&, const SourceFrameView*, const std::array<const uint8_t*, kMaxInputs>& in, uint8_t* dst, size_t n) {
					return in[0] != nullptr && VpLaneProducts::ComputeSourceStats(in[0], Dims{ kW, kH }, dst, n);
				});
			LaneSettings s;
			s.mode = LaneSettings::Mode::On;
			lane.Configure(s, reg, consumers, gs, card, tier, 0, false);
		}
		SourceFrameView View(uint64_t seq) const
		{
			SourceFrameView v;
			v.seq = seq;
			v.luma = store.data() + size_t(seq % frames) * kW * kH;
			v.width = kW;
			v.height = kH;
			v.strideSamples = kW;
			return v;
		}
		void Pump()
		{
			for (int i = 0; i < 20000; ++i)
				if (!lane.StepWorker(Backend::Cpu, 1000))
					return;
			Assert::Fail(L"pump did not settle");
		}
	};

	TEST_CLASS(SealAcquireIdentityTests)
	{
	public:
		// Lead 8 ruling on 102b B1-SEAL-VIA-ACQUIRE: the seal and Acquire share ONE law (RunCpuLive). Lane A makes and seals the
		// frames the normal way; lane B (same configuration, never pumped) gets each frame ONLY through Acquire (the owner fill):
		// the bytes are identical for P1 and P4 on every frame.
		TEST_METHOD(SealedBytesEqualAcquiredBytesCpuProducts)
		{
			MiniLane a, b;
			Assert::IsTrue(a.lane.Active() && b.lane.Active(), L"both lanes built");
			SkeletonMemo ma, mb;
			BindLaneToMemo(a.lane, ma);
			BindLaneToMemo(b.lane, mb);
			a.lane.OnSourceFrame(a.View(0));
			b.lane.OnSourceFrame(b.View(0));
			for (uint64_t s = 0; s < 12; ++s) {
				a.lane.OnSourceFrame(a.View(s + 1));
				b.lane.OnSourceFrame(b.View(s + 1));
				a.Pump();
				a.lane.OnRenderStart(s, 4000);   // A: the seal publishes
				ma.OnSeal(s);
				for (ProductId id : { kP1HalfLuma, kP4SourceStats }) {
					const AcquireResult fromB = mb.Acquire(id, s, AcquireForm::Cpu);   // B: the owner fill, before any seal of B
					const AcquireResult fromA = ma.Acquire(id, s, AcquireForm::Cpu);
					Assert::IsTrue(fromA.source == AcquireSource::LaneSlot && fromB.Ok(), L"A sealed, B acquired");
					Assert::IsTrue(fromA.cpuBytes == fromB.cpuBytes && std::memcmp(fromA.cpu, fromB.cpu, fromA.cpuBytes) == 0,
						L"seal-published bytes == Acquired bytes (one law)");
				}
				b.lane.OnRenderStart(s, 4000);   // B's seal reuses B's publication
				mb.OnSeal(s);
				a.lane.OnFrameRetired(s, 0);
				b.lane.OnFrameRetired(s, 0);
			}
			Assert::IsTrue(b.lane.SealReusedPublished() >= 12u, L"B's seal reused the Acquire publications, never computed twice");
		}

		// The view product (P13): two memos (e.g. two readers' paths) Acquiring one offered frame get the same plane texture and facts:
		// the view is the uploader's texture, never a copy.
		TEST_METHOD(ViewAcquiredIsTheOfferedPlane)
		{
			using Vpv::Capture::SourcePlanes;
			int host = 0;
			pl_tex_t luma{};
			luma.params.w = 64;
			luma.params.h = 48;
			pl_frame image{};
			image.num_planes = 1;
			image.planes[0].texture = &luma;
			image.planes[0].components = 1;
			image.repr.sys = PL_COLOR_SYSTEM_BT_709;
			Assert::IsTrue(SourcePlanes::OfferFrame(&host, 3, 0, image), L"offered");
			SkeletonMemo m1, m2;
			for (SkeletonMemo* m : { &m1, &m2 }) {
				m->SetHost(&host);
				m->RegisterOwnerFill(kP13SourceRing, AcquireForm::Gpu, SourcePlanes::kUnit, SourcePlanes::MakeOwnerFill());
			}
			const AcquireResult r1 = m1.Acquire(kP13SourceRing, 3, AcquireForm::Gpu);
			const AcquireResult r2 = m2.Acquire(kP13SourceRing, 3, AcquireForm::Gpu);
			Assert::IsTrue(r1.Ok() && r2.Ok() && r1.gpu.resource == &luma && r2.gpu.resource == r1.gpu.resource, L"the offered plane itself");
			Assert::IsTrue(SourcePlanes::FactsOf(r1) == SourcePlanes::FactsOf(r2), L"one offered set");
			SourcePlanes::Forget(&host);
		}
	};

	TEST_CLASS(MemoGpuGrantWithoutLaneTests)
	{
	public:
		TEST_METHOD(PlanGrantsOrRefusesByTheLedgerRule)
		{
			CardProfile card;
			card.vramBudgetBytes = 1ull << 30;
			card.vramUsageBytes = 200ull << 20;
			const MemoGpuGrant ok = PlanMemoGpuGrant(card, true, 100ull << 20, 16ull << 20);
			Assert::IsTrue(ok.granted && ok.bytes == (16ull << 20) && ok.budget > 0, L"granted the full need");
			CardProfile tight;
			tight.vramBudgetBytes = 100ull << 20;
			tight.vramUsageBytes = 95ull << 20;
			const MemoGpuGrant no = PlanMemoGpuGrant(tight, true, 0, 16ull << 20);
			Assert::IsTrue(!no.granted && no.bytes == 0, L"does not fit: refused (shed to 0)");
			Assert::IsFalse(PlanMemoGpuGrant(card, false, 0, 16ull << 20).granted, L"card unknown: refused");
			Assert::IsFalse(PlanMemoGpuGrant(card, true, 0, 0).granted, L"nothing to grant");
			// The other users' holdings are fixed: a card whose free memory is held by them leaves the memo nothing.
			CardProfile held;
			held.vramBudgetBytes = 1ull << 30;
			held.vramUsageBytes = 1000ull << 20;
			Assert::IsFalse(PlanMemoGpuGrant(held, true, 900ull << 20, 64ull << 20).granted, L"others do not shed for the memo");
		}

		// The module: with analysis_lane off, a GPU owner fill that ALLOCATES is granted against the ledger at Begin (reserved before the
		// fill, released at the next seal); with a card that cannot hold it, it is Unavailable(no VRAM ledger grant), visible.
		TEST_METHOD(ModuleGrantsTheNoLaneMemoAndRefusesVisibly)
		{
			for (int tightCard = 0; tightCard < 2; ++tightCard) {
				Vpv::Skeleton::SkeletonModule sk([](ConfigFile&) { return false; });
				CardProfile card;
				card.vramBudgetBytes = tightCard ? (8ull << 20) : (1ull << 30);
				card.vramUsageBytes = tightCard ? (7ull << 20) : 0;
				sk.SetCardForTesting(card);
				int host = 0;
				int fakeGpu = 0;
				RE::LifecycleFacts created;
				created.host = &host;
				created.kind = RE::Lifecycle::GpuCreated;
				created.gpu = reinterpret_cast<pl_gpu>(&fakeGpu);   // never dereferenced: presence only
				sk.OnLifecycle(created);
				int calls = 0;
				OwnerFill g;
				g.reserveBytes = 1u << 20;
				g.fill = [&calls](const OwnerFillRequest&, SkeletonMemo&, OwnerFillOutput& out) {
					++calls;
					static int tex = 0;
					out.gpu.resource = &tex;
					out.gpu.bytes = 1u << 20;
					return true;
				};
				Assert::IsTrue(sk.RegisterOwnerFill(kP10GrainSplit, AcquireForm::Gpu, kGrainUnit, g) == SkeletonMemo::Registration::Registered, L"GPU owner");
				const Vpv::Skeleton::FrameMemo fm = sk.MemoFor(&host, 2);
				Assert::IsTrue(fm.Valid() && fm.gpu == &fakeGpu, L"memo + the renderer's pl_gpu");
				Assert::IsTrue(fm.memo->GpuNeedBytes() == (1u << 20), L"declared need");
				RE::FrameContext b2 = Phase(&host, RE::FramePhase::Begin, 2);
				sk.OnFrame(b2);   // seal + no-lane grant
				const AcquireResult r = fm.memo->Acquire(kP10GrainSplit, 2, AcquireForm::Gpu);
				if (!tightCard) {
					Assert::IsTrue(fm.memo->GpuGranted() && r.status == AcquireStatus::FilledAtRead && fm.memo->GpuBytes() == (1u << 20), L"granted, reserved");
					RE::FrameContext b3 = Phase(&host, RE::FramePhase::Begin, 3);
					sk.OnFrame(b3);
					Assert::IsTrue(fm.memo->GpuBytes() == 0, L"released at the next seal");
				} else {
					Assert::IsTrue(!fm.memo->GpuGranted() && r.cause == AcquireCause::NoLedgerGrant && calls == 0, L"refused before the fill, visible");
					Assert::IsTrue(Has(fm.memo->LastCause(kP10GrainSplit), "ledger"), L"cause text");
				}
				RE::LifecycleFacts rel;
				rel.host = &host;
				rel.kind = RE::Lifecycle::GpuReleased;
				sk.OnLifecycle(rel);
			}
		}
	};
}
