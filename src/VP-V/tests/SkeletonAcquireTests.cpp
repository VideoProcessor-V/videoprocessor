// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "CppUnitTest.h"

// B1 (refs/126 s1.2): GET-OR-COMPUTE-AND-PUBLISH (src/VP-V/core/SkeletonAcquire.h, SkeletonAcquireLane.h).
//   1. computed once with two readers (sequential and concurrent): the owner fill runs once, the second reader reuses it;
//   2. a variant published by the owner and reused, canonical keys; an undeclared variant axis is refused (no private copy);
//   3. lifetime: memo spots released at the next seal, a released frame is refused (never recomputed), pins outlive the
//      release, GPU spots under a read fence are retired only when it completes, budget reserved before the fill, new epoch;
//   4. no GPU dispatcher / no ledger grant: the GPU form is Unavailable, counted, on the status and the Fallbacks decisions;
//      ownership from the catalog (wrong unit, not in the catalog, double registration refused, explicit replace);
//   5. a throwing fill is a decline (waiters woken, no stuck entry); re-entrancy is refused; lane Pending / Failed / NoHandle;
//   6. identity: lane products read through Acquire are the lane's own bytes (bit-identical, same buffer); a frame an Acquire
//      reader published before the seal is reused by the seal; a missing-input seal is NOT served from the publication.
// CPU only, single process, every test bounded.

#include <VP-V/core/AnalysisLane.h>
#include <VP-V/core/SkeletonAcquire.h>
#include <VP-V/core/SkeletonAcquireLane.h>
#include <VP-V/modules/control/skeleton/SkeletonModule.h>
#include <VP-V/modules/input/source_analysis/lane/LaneCpuProducts.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace SkeletonAcquireTestsNs
{
	using namespace VpSkeleton;

	constexpr uint32_t kW = 64;
	constexpr uint32_t kH = 48;
	const char* const kGrainUnit = "repair/grain_split";              // ProductRows: P10's owner
	const char* const kRegionUnit = "input/source_analysis/regions";  // P11
	const char* const kLaneUnit = "input/source_analysis/lane";       // P1 / P4

	bool Has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }
	bool AnyLine(const std::vector<std::string>& lines, const char* part)
	{
		for (const std::string& l : lines)
			if (Has(l, part))
				return true;
		return false;
	}
	bool AnyDecision(const std::vector<FallbackCatalog::Decision>& ds, const char* id, const char* reason)
	{
		for (const FallbackCatalog::Decision& d : ds)
			if (d.engaged && d.id == id && Has(d.reason, reason))
				return true;
		return false;
	}

	// An owner CPU fill that counts its calls and writes a pattern of (seq, variant scale).
	OwnerFill CountingFill(std::atomic<int>* calls, uint32_t axes = 0, int sleepMs = 0, bool decline = false, bool throws = false)
	{
		OwnerFill f;
		f.variantAxes = axes;
		f.fill = [calls, sleepMs, decline, throws](const OwnerFillRequest& r, SkeletonMemo&, OwnerFillOutput& out) {
			calls->fetch_add(1);
			if (sleepMs > 0)
				std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
			if (throws)
				throw std::runtime_error("owner fill failure");
			if (decline)
				return false;
			out.bytes.assign(256, 0);
			for (size_t i = 0; i < out.bytes.size(); ++i)
				out.bytes[i] = uint8_t(i + r.seq * 7u + r.variant.scale * 31u);
			return true;
		};
		return f;
	}

	OwnerFill GpuFill(int* calls, uint64_t bytes, uint64_t reserve = 0)
	{
		OwnerFill f;
		f.reserveBytes = reserve;
		f.fill = [calls, bytes](const OwnerFillRequest&, SkeletonMemo&, OwnerFillOutput& out) {
			++*calls;
			static int texture = 0;
			out.gpu.resource = &texture;
			out.gpu.bytes = bytes;
			out.gpu.keepAlive = std::make_shared<int>(7);
			return true;
		};
		return f;
	}

	// A minimal lane rig on the real lane (FirstProducts, CPU producers P1 + P4, one consumer of P4).
	struct LaneRig {
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
		size_t frames = 64;

		LaneRig()
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
			uint32_t x = 4242u;
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
		}
		// A CPU product with a lag-1 input (P5 id, declared like P4 plus P1 lag 1, missing-input form): frame 0 seals with input 1
		// missing. Its law marks byte 0 with whether the previous half luma was present.
		void AddLaggedCpuProduct()
		{
			std::vector<ProductDecl> d = FirstProducts().All();
			ProductDecl p = d[kP4SourceStats];
			p.id = kP5MonitorTiles;
			p.token = "p5_test_lagged";
			p.name = "test lagged stats";
			detail::AddInput(p, kP1HalfLuma, 1);
			p.missingInputForm = true;
			d[kP5MonitorTiles] = p;
			reg = Registry(d);
			consumers.clear();
			ConsumerDecl c;
			c.id = 2;
			AddConsume(c, kP5MonitorTiles);
			consumers.push_back(c);
			lane.RegisterCpuLiveCompute(kP5MonitorTiles,
				[](const Node&, const SourceFrameView*, const std::array<const uint8_t*, kMaxInputs>& in, uint8_t* dst, size_t n) {
					if (in[0] == nullptr || !VpLaneProducts::ComputeSourceStats(in[0], Dims{ kW, kH }, dst, n))
						return false;
					dst[0] = in[1] != nullptr ? uint8_t(0x55) : uint8_t(0xAA);
					return true;
				});
		}
		bool Configure()
		{
			LaneSettings s;
			s.mode = LaneSettings::Mode::On;
			lane.Configure(s, reg, consumers, gs, card, tier, 0, false);
			return lane.Active();
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
		std::vector<uint8_t> SlotBytes(ProductId id, uint64_t s) const
		{
			const AnalysisLane::ProductRead r = lane.ReadOf(id, s);
			return r.cpu ? std::vector<uint8_t>(r.cpu, r.cpu + r.cpuBytes) : std::vector<uint8_t>();
		}
		std::vector<uint8_t> ExpectedHalf(uint64_t s) const
		{
			std::vector<uint8_t> out(VpLaneProducts::HalfLumaBytes(Dims{ kW, kH }));
			Assert::IsTrue(VpLaneProducts::ComputeHalfLuma(View(s), out.data(), out.size()), L"reference half luma");
			return out;
		}
		std::vector<uint8_t> ExpectedStats(uint64_t s) const
		{
			const std::vector<uint8_t> half = ExpectedHalf(s);
			std::vector<uint8_t> out(size_t(VpLaneProducts::kStatsBytes), 0);
			Assert::IsTrue(VpLaneProducts::ComputeSourceStats(half.data(), Dims{ kW, kH }, out.data(), out.size()), L"reference stats");
			return out;
		}
	};

	TEST_CLASS(SkeletonAcquireTests)
	{
	public:
		// 1a. Two readers of one frame: the owner computes once and publishes; the second reader reuses the same spot.
		TEST_METHOD(ComputedOnceWithTwoReaders)
		{
			SkeletonMemo m;
			std::atomic<int> calls{ 0 };
			Assert::IsTrue(m.RegisterOwnerFill(kP10GrainSplit, AcquireForm::Cpu, kGrainUnit, CountingFill(&calls))
				== SkeletonMemo::Registration::Registered, L"owner registered");
			const AcquireResult a = m.Acquire(kP10GrainSplit, 3, AcquireForm::Cpu);
			const AcquireResult b = m.Acquire(kP10GrainSplit, 3, AcquireForm::Cpu);
			Assert::IsTrue(a.status == AcquireStatus::FilledAtRead && a.source == AcquireSource::Memo, L"first reader: filled at read");
			Assert::IsTrue(b.status == AcquireStatus::Reused && b.source == AcquireSource::Memo, L"second reader: reused");
			Assert::IsTrue(a.cpu == b.cpu && a.cpuBytes == 256u && a.pin && b.pin, L"the same published, pinned spot");
			Assert::AreEqual(1, calls.load(), L"owner fill ran once");
			const AcquireCounters c = m.CountersOf(kP10GrainSplit);
			Assert::IsTrue(c.ownerFillCalls == 1 && c.filledAtRead == 1 && c.reusedMemo == 1 && c.Unavailable() == 0, L"counted");
			Assert::IsTrue(m.Acquire(kP10GrainSplit, 4, AcquireForm::Cpu).status == AcquireStatus::FilledAtRead, L"another frame fills");
			Assert::AreEqual(2, calls.load(), L"once per frame");
			Assert::IsTrue(AnyLine(m.StatusLines(), "2 filled at read"), L"on the status");
		}

		// 1b. Concurrent readers: the second waits for the fill in flight instead of computing a copy.
		TEST_METHOD(ConcurrentReadersWaitForTheOneFill)
		{
			SkeletonMemo m;
			std::atomic<int> calls{ 0 };
			m.RegisterOwnerFill(kP10GrainSplit, AcquireForm::Cpu, kGrainUnit, CountingFill(&calls, 0, 40));
			AcquireResult ra, rb;
			std::thread ta([&]() { ra = m.Acquire(kP10GrainSplit, 9, AcquireForm::Cpu); });
			std::thread tb([&]() { rb = m.Acquire(kP10GrainSplit, 9, AcquireForm::Cpu); });
			ta.join();
			tb.join();
			Assert::AreEqual(1, calls.load(), L"one fill for two concurrent readers");
			Assert::IsTrue(ra.Ok() && rb.Ok() && ra.cpu == rb.cpu, L"both read the one spot");
			Assert::IsTrue((ra.status == AcquireStatus::FilledAtRead) != (rb.status == AcquireStatus::FilledAtRead), L"exactly one filled");
		}

		// 2. G4: the owner publishes a variant under its canonical key; readers reuse it; an undeclared axis is refused.
		TEST_METHOD(VariantPublishedByTheOwnerAndReused)
		{
			SkeletonMemo m;
			std::atomic<int> calls{ 0 };
			OwnerFill f = CountingFill(&calls, VariantKey::kAxisScale | VariantKey::kAxisSize);
			f.defaults.width = kW;   // the owner's own size: a key naming it is the default variant
			f.defaults.height = kH;
			m.RegisterOwnerFill(kP10GrainSplit, AcquireForm::Cpu, kGrainUnit, f);
			VariantKey half;
			half.scale = 2;
			half.width = kW / 2;
			half.height = kH / 2;
			const AcquireResult d = m.Acquire(kP10GrainSplit, 1, AcquireForm::Cpu);
			const AcquireResult v1 = m.Acquire(kP10GrainSplit, 1, AcquireForm::Cpu, half);
			const AcquireResult v2 = m.Acquire(kP10GrainSplit, 1, AcquireForm::Cpu, half);
			Assert::IsTrue(d.status == AcquireStatus::FilledAtRead && v1.status == AcquireStatus::FilledAtRead, L"default and variant are two spots");
			Assert::IsTrue(v2.status == AcquireStatus::Reused && v2.cpu == v1.cpu, L"the variant is reused");
			Assert::IsTrue(d.cpu != v1.cpu && d.cpu[1] != v1.cpu[1], L"the variant is the owner's variant, not the default");
			// Canonical keys: the owner's own size, or a one-sided size, is the default spot (no second computation).
			VariantKey own;
			own.width = kW;
			own.height = kH;
			VariantKey oneSided;
			oneSided.width = 17;
			Assert::IsTrue(m.Acquire(kP10GrainSplit, 1, AcquireForm::Cpu, own).cpu == d.cpu, L"own size = default spot");
			Assert::IsTrue(m.Acquire(kP10GrainSplit, 1, AcquireForm::Cpu, oneSided).cpu == d.cpu, L"one-sided size = default spot");
			Assert::AreEqual(2, calls.load(), L"one fill per (frame, canonical variant)");
			VariantKey sigma;
			sigma.sigmaMilli = 1500;
			const AcquireResult u = m.Acquire(kP10GrainSplit, 1, AcquireForm::Cpu, sigma);
			Assert::IsTrue(u.status == AcquireStatus::Unavailable && u.cause == AcquireCause::VariantUndeclared, L"undeclared axis refused");
			Assert::AreEqual(2, calls.load(), L"the owner is not asked for an axis it does not serve");
			Assert::IsTrue(Has(m.LastCause(kP10GrainSplit), "variant not served"), L"visible cause");
			Assert::IsTrue(Has(half.Text(), "scale=2") && VariantKey().Text() == "default", L"variant text");
		}

		// 3a. Lifetime: per-frame memo, released at the next seal; a released frame is refused, never recomputed; pins outlive it.
		TEST_METHOD(LifetimeEvictionPinsAndEpoch)
		{
			SkeletonMemo m;
			std::atomic<int> calls{ 0 };
			m.RegisterOwnerFill(kP10GrainSplit, AcquireForm::Cpu, kGrainUnit, CountingFill(&calls));
			m.OnSeal(5);
			const AcquireResult held = m.Acquire(kP10GrainSplit, 5, AcquireForm::Cpu);
			Assert::IsTrue(held.status == AcquireStatus::FilledAtRead, L"frame 5 filled");
			const std::vector<uint8_t> copy(held.cpu, held.cpu + held.cpuBytes);
			Assert::IsTrue(m.MemoCount() == 1 && m.CpuBytes() == 256u, L"one spot, its bytes counted");
			m.OnSeal(6);
			Assert::IsTrue(m.MemoCount() == 0 && m.CpuBytes() == 0u, L"released at the next seal");
			Assert::IsTrue(m.CountersOf(kP10GrainSplit).released == 1, L"release counted");
			Assert::IsTrue(std::vector<uint8_t>(held.cpu, held.cpu + held.cpuBytes) == copy, L"the pinned result is still valid");
			const AcquireResult late = m.Acquire(kP10GrainSplit, 5, AcquireForm::Cpu);
			Assert::IsTrue(late.status == AcquireStatus::Unavailable && late.cause == AcquireCause::FrameReleased, L"released frame refused");
			Assert::AreEqual(1, calls.load(), L"never recomputed");
			Assert::IsTrue(m.Acquire(kP10GrainSplit, 6, AcquireForm::Cpu).Ok(), L"the current frame fills");

			// keepFrames 1: the previous frame stays readable for one more seal.
			m.SetKeepFrames(1);
			m.OnSeal(7);
			Assert::IsTrue(m.Acquire(kP10GrainSplit, 6, AcquireForm::Cpu).status == AcquireStatus::Reused, L"kept one frame");
			m.OnSeal(8);
			Assert::IsTrue(m.MemoCount() == 0, L"then released");
			m.SetKeepFrames(0);

			// A new epoch (frame numbers go back): the old numbering is forgotten, frames fill again.
			const int before = calls.load();
			m.Acquire(kP10GrainSplit, 8, AcquireForm::Cpu);
			m.OnSeal(2);
			Assert::IsTrue(m.MemoCount() == 0, L"the old epoch's spots released");
			Assert::IsTrue(m.Acquire(kP10GrainSplit, 2, AcquireForm::Cpu).status == AcquireStatus::FilledAtRead, L"new epoch fills");
			Assert::AreEqual(before + 2, calls.load());
			m.Clear();
			Assert::IsTrue(m.MemoCount() == 0 && m.Acquire(kP10GrainSplit, 1, AcquireForm::Cpu).Ok(), L"Clear forgets the numbering");
		}

		// 3b. GPU budget (reserved before the fill), ledger grant, and the fence-aware retire of a GPU spot under a read.
		TEST_METHOD(GpuBudgetGrantAndFenceRetire)
		{
			SkeletonMemo m;
			m.SetNames(nullptr);
			m.SetGpuDispatcherPresent(true);
			int calls = 0;
			m.RegisterOwnerFill(kP10GrainSplit, AcquireForm::Gpu, kGrainUnit, GpuFill(&calls, 1000, 1000));
			Assert::IsTrue(m.Acquire(kP10GrainSplit, 1, AcquireForm::Gpu).cause == AcquireCause::NoLedgerGrant, L"no ledger grant");
			Assert::AreEqual(0, calls, L"nothing allocated outside the ledger");
			m.SetGpuBudget(true, 1500);
			const AcquireResult a = m.Acquire(kP10GrainSplit, 1, AcquireForm::Gpu);
			Assert::IsTrue(a.status == AcquireStatus::FilledAtRead && a.gpu.Valid() && m.GpuBytes() == 1000u, L"budgeted");
			Assert::IsTrue(m.Acquire(kP10GrainSplit, 1, AcquireForm::Gpu).gpu.resource == a.gpu.resource, L"GPU form reused");
			VariantKey other;
			other.scale = 1;
			// A second spot does not fit (1000 + 1000 > 1500): refused BEFORE the fill runs.
			OwnerFill f2 = GpuFill(&calls, 1000, 1000);
			f2.variantAxes = VariantKey::kAxisScale;
			m.ReplaceOwnerFill(kP10GrainSplit, AcquireForm::Gpu, kGrainUnit, f2);
			Assert::IsTrue(m.Acquire(kP10GrainSplit, 1, AcquireForm::Gpu, other).cause == AcquireCause::OverBudget, L"over budget");
			Assert::AreEqual(1, calls, L"the fill did not run");
			Assert::IsTrue(m.Acquire(kP10GrainSplit, 1, AcquireForm::Gpu, other).cause == AcquireCause::OverBudget, L"the frame's decline is kept");
			Assert::AreEqual(1, calls, L"the kept decline did not ask the owner again");
			Assert::IsTrue(AnyDecision(m.Decisions(), ("skeleton acquire product " + std::to_string(kP10GrainSplit)).c_str(), "VRAM ledger grant exceeded"), L"Fallbacks decision");

			// A reader recorded a GPU read up to fence 50: the released spot is held until the fence completes.
			uint64_t completed = 10;
			m.SetFenceCompleted([&completed]() { return completed; });
			m.NoteGpuRead(a, 50);
			std::weak_ptr<void> alive = a.gpu.keepAlive;
			m.OnSeal(2);
			Assert::IsTrue(m.MemoCount() == 0 && m.RetiredPending() == 1 && m.GpuBytes() == 0u, L"retired, not dropped");
			Assert::IsTrue(AnyLine(m.StatusLines(), "held for a pending read fence"), L"visible");
			completed = 60;
			m.OnSeal(3);
			Assert::IsTrue(m.RetiredPending() == 0, L"dropped once the read fence completed");
			Assert::IsFalse(alive.expired(), L"the reader's own pin still holds it");
		}

		// 4. No GPU dispatcher (the SkeletonModule wiring that declines every GPU live fill): visible, counted, on the Fallbacks
		//    decisions; recovers when the dispatcher exists. Ownership is the catalog's.
		TEST_METHOD(NoDispatcherOwnershipAndCausesAreVisible)
		{
			SkeletonMemo m;
			m.SetNames(nullptr);
			int gpuCalls = 0;
			Assert::IsTrue(SkeletonMemo::OwnerUnitOf(kP1HalfLuma) == kLaneUnit, L"P1 is owned by the lane unit (catalog)");
			m.RegisterOwnerFill(kP1HalfLuma, AcquireForm::Gpu, kLaneUnit, GpuFill(&gpuCalls, 64));
			m.SetGpuBudget(true, 1u << 20);
			const AcquireResult a = m.Acquire(kP1HalfLuma, 1, AcquireForm::Gpu);
			Assert::IsTrue(a.status == AcquireStatus::Unavailable && a.cause == AcquireCause::NoGpuDispatcher, L"no dispatcher");
			Assert::AreEqual(0, gpuCalls, L"no GPU work without a dispatcher");
			Assert::IsTrue(m.CountersOf(kP1HalfLuma).unavailable[size_t(AcquireCause::NoGpuDispatcher)] == 1, L"counted");
			Assert::IsTrue(AnyLine(m.StatusLines(), "no GPU dispatcher 1"), L"on the status lines");
			Assert::IsTrue(AnyDecision(m.Decisions(), "skeleton acquire product 1", "no GPU dispatcher"), L"Fallbacks decision");
			Assert::IsTrue(m.Acquire(kP1HalfLuma, 1, AcquireForm::Cpu).cause == AcquireCause::NoOwnerFill, L"(id, form): no CPU owner");

			m.SetGpuDispatcherPresent(true);
			const AcquireResult b = m.Acquire(kP1HalfLuma, 2, AcquireForm::Gpu);
			Assert::IsTrue(b.status == AcquireStatus::FilledAtRead && b.gpu.Valid() && b.gpu.bytes == 64u, L"GPU form published");
			Assert::AreEqual(1, gpuCalls, L"one GPU fill");
			Assert::IsTrue(m.Decisions().empty(), L"released after the recovery");

			std::atomic<int> calls{ 0 };
			Assert::IsTrue(m.RegisterOwnerFill(kP1HalfLuma, AcquireForm::Cpu, "control/skeleton", CountingFill(&calls))
				== SkeletonMemo::Registration::NotOwner, L"a unit that does not own P1 is refused");
			Assert::IsTrue(m.RegisterOwnerFill(kSkeletonProductEnd, AcquireForm::Cpu, kLaneUnit, CountingFill(&calls))
				== SkeletonMemo::Registration::NotInCatalog, L"not in the catalog");
			Assert::IsTrue(m.RegisterOwnerFill(kP1HalfLuma, AcquireForm::Gpu, kLaneUnit, GpuFill(&gpuCalls, 64))
				== SkeletonMemo::Registration::AlreadyRegistered, L"no silent replace");
			Assert::IsTrue(m.ReplaceOwnerFill(kP1HalfLuma, AcquireForm::Gpu, kLaneUnit, GpuFill(&gpuCalls, 64))
				== SkeletonMemo::Registration::Replaced, L"explicit replace");
			Assert::IsTrue(m.Refusals() == 3 && AnyLine(m.StatusLines(), "owner registrations refused 3"), L"refusals shown");

			// An owner decline is a visible cause, asked once per frame.
			std::atomic<int> declines{ 0 };
			m.RegisterOwnerFill(kP11RegionBlockStats, AcquireForm::Cpu, kRegionUnit, CountingFill(&declines, 0, 0, true));
			Assert::IsTrue(m.Acquire(kP11RegionBlockStats, 3, AcquireForm::Cpu).cause == AcquireCause::OwnerDeclined, L"declined");
			Assert::IsTrue(m.Acquire(kP11RegionBlockStats, 3, AcquireForm::Cpu).cause == AcquireCause::OwnerDeclined, L"again");
			Assert::AreEqual(1, declines.load(), L"the owner is asked once per frame");
		}

		// 5a. A throwing fill is a decline: the entry is not left Filling, a waiting reader is woken, the next frame fills again.
		TEST_METHOD(ThrowingFillIsADeclineAndWakesWaiters)
		{
			SkeletonMemo m;
			std::atomic<int> calls{ 0 };
			m.RegisterOwnerFill(kP10GrainSplit, AcquireForm::Cpu, kGrainUnit, CountingFill(&calls, 0, 40, false, true));
			AcquireResult ra, rb;
			std::thread ta([&]() { ra = m.Acquire(kP10GrainSplit, 4, AcquireForm::Cpu); });
			std::thread tb([&]() { rb = m.Acquire(kP10GrainSplit, 4, AcquireForm::Cpu); });
			ta.join();
			tb.join();
			Assert::AreEqual(1, calls.load(), L"one (throwing) fill");
			Assert::IsTrue(ra.cause == AcquireCause::OwnerDeclined && rb.cause == AcquireCause::OwnerDeclined, L"both see the decline");
			Assert::IsTrue(m.CountersOf(kP10GrainSplit).unavailable[size_t(AcquireCause::OwnerDeclined)] == 2, L"counted");
		}

		// 5b. Re-entrancy, and the lane causes Pending / Failed / NoHandle through a lane source.
		TEST_METHOD(ReentrancyAndLaneCauses)
		{
			SkeletonMemo m;
			AcquireCause inner = AcquireCause::None;
			OwnerFill f;
			f.fill = [&inner](const OwnerFillRequest& r, SkeletonMemo& memo, OwnerFillOutput& out) {
				inner = memo.Acquire(r.id, r.seq, r.form).cause;   // its own key: would wait for itself
				out.bytes.assign(8, 1);
				return true;
			};
			m.RegisterOwnerFill(kP10GrainSplit, AcquireForm::Cpu, kGrainUnit, f);
			Assert::IsTrue(m.Acquire(kP10GrainSplit, 1, AcquireForm::Cpu).Ok(), L"the outer fill completes");
			Assert::IsTrue(inner == AcquireCause::Reentrant, L"the inner request is refused, not a deadlock");

			LaneSlotSource src;
			src.read = [](ProductId id, uint64_t seq, AcquireForm, LaneSlotSource::Read*) {
				if (id != kP12LightLevel)
					return LaneSlotSource::State::NotLaneProduct;
				return seq == 1 ? LaneSlotSource::State::Pending : seq == 2 ? LaneSlotSource::State::Failed : LaneSlotSource::State::NoHandle;
			};
			m.SetLaneSource(src);
			Assert::IsTrue(m.Acquire(kP12LightLevel, 1, AcquireForm::Gpu).cause == AcquireCause::LanePending, L"pending");
			Assert::IsTrue(m.Acquire(kP12LightLevel, 2, AcquireForm::Gpu).cause == AcquireCause::LaneFailed, L"failed");
			Assert::IsTrue(m.Acquire(kP12LightLevel, 3, AcquireForm::Gpu).cause == AcquireCause::NoGpuHandle, L"no handle");
			Assert::IsTrue(AnyLine(m.StatusLines(), "lane live fill pending 1") && AnyLine(m.StatusLines(), "lane live fill failed 1"), L"shown");
		}

		// 6a. Identity: lane products read through Acquire are the lane's own buffers (bit-identical by construction).
		TEST_METHOD(LaneProductsViaAcquireAreBitIdentical)
		{
			LaneRig r;
			Assert::IsTrue(r.Configure(), L"built");
			SkeletonMemo m;
			m.SetNames(&r.reg);
			BindLaneToMemo(r.lane, m);
			Assert::IsTrue(m.HasOwnerFill(kP1HalfLuma, AcquireForm::Cpu) && m.HasOwnerFill(kP4SourceStats, AcquireForm::Cpu) && m.Refusals() == 0,
				L"the lane computes are the owner fills, under the catalog unit");
			r.lane.OnSourceFrame(r.View(0));
			for (uint64_t s = 0; s < 40; ++s) {
				r.lane.OnSourceFrame(r.View(s + 1));
				r.Pump();
				r.lane.OnRenderStart(s, 4000);
				m.OnSeal(s);
				for (ProductId id : { kP1HalfLuma, kP4SourceStats }) {
					const AcquireResult a = m.Acquire(id, s, AcquireForm::Cpu);
					const AnalysisLane::ProductRead lr = r.lane.ReadOf(id, s);
					Assert::IsTrue(a.status == AcquireStatus::Reused && a.source == AcquireSource::LaneSlot && a.pin, L"reused from the lane slot");
					Assert::IsTrue(a.cpu == lr.cpu && a.cpuBytes == lr.cpuBytes, L"the lane's own buffer");
				}
				const AcquireResult h = m.Acquire(kP1HalfLuma, s, AcquireForm::Cpu);
				const AcquireResult st = m.Acquire(kP4SourceStats, s, AcquireForm::Cpu);
				Assert::IsTrue(std::vector<uint8_t>(h.cpu, h.cpu + h.cpuBytes) == r.ExpectedHalf(s), L"P1 = the one-shot law");
				Assert::IsTrue(std::vector<uint8_t>(st.cpu, st.cpu + st.cpuBytes) == r.ExpectedStats(s), L"P4 = the one-shot law");
				r.lane.OnFrameRetired(s, 0);
				r.lane.OnFrameTiming(s, 10.0, 1.0, false);
			}
			const AcquireCounters c = m.CountersOf(kP4SourceStats);
			Assert::IsTrue(c.ownerFillCalls == 0 && c.reusedLane == 80u, L"nothing recomputed: 2 reads x 40 frames from the lane");
			Assert::IsTrue(m.MemoCount() == 0, L"no memo spot for products the lane made");
			Assert::IsTrue(m.Acquire(kP1HalfLuma, 39, AcquireForm::Gpu).cause == AcquireCause::NoOwnerFill, L"GPU form of P1 not served yet");
		}

		// 6b. Get-or-compute before the seal: an Acquire of a frame the lane has not made runs the lane's own law (P4 through P1 via the
		//     same chain), publishes it, and the seal then REUSES the publication: bit-identical and never computed twice.
		TEST_METHOD(PublishedBeforeTheSealIsReusedByTheSeal)
		{
			LaneRig r;
			Assert::IsTrue(r.Configure(), L"built");
			SkeletonMemo m;
			BindLaneToMemo(r.lane, m);
			r.lane.OnSourceFrame(r.View(0));   // the lane has not run (no pump)
			const AcquireResult a = m.Acquire(kP4SourceStats, 0, AcquireForm::Cpu);
			Assert::IsTrue(a.status == AcquireStatus::FilledAtRead && a.source == AcquireSource::Memo, L"filled at read by the owner");
			Assert::IsTrue(std::vector<uint8_t>(a.cpu, a.cpu + a.cpuBytes) == r.ExpectedStats(0), L"the lane's law");
			Assert::IsTrue(m.CountersOf(kP1HalfLuma).filledAtRead == 1, L"its input P1 got-or-computed through the same path");
			const std::vector<ProductFrameResult>& res = r.lane.OnRenderStart(0, 4000);
			bool p1Live = false, p4Live = false;
			for (const ProductFrameResult& x : res) {
				p1Live = p1Live || (x.id == kP1HalfLuma && x.liveFilled);
				p4Live = p4Live || (x.id == kP4SourceStats && x.liveFilled);
			}
			Assert::IsTrue(p1Live && p4Live, L"the seal live-filled both");
			Assert::IsTrue(r.lane.SealReusedPublished() == 2u, L"from the publication, not recomputed");
			Assert::IsTrue(r.SlotBytes(kP4SourceStats, 0) == r.ExpectedStats(0) && r.SlotBytes(kP1HalfLuma, 0) == r.ExpectedHalf(0),
				L"slot bytes bit-identical to the law");
			const AcquireResult after = m.Acquire(kP4SourceStats, 0, AcquireForm::Cpu);
			Assert::IsTrue(after.status == AcquireStatus::Reused && after.source == AcquireSource::LaneSlot, L"now read from the slot");
			Assert::IsTrue(m.CountersOf(kP4SourceStats).ownerFillCalls == 1, L"computed once in all");
		}

		// 6c. A seal with a MISSING input (lag-1 input at the first frame) computes its missing-input form by the law and is not
		//     served from an Acquire publication.
		TEST_METHOD(MissingInputSealIsNotServedFromThePublication)
		{
			LaneRig r;
			r.AddLaggedCpuProduct();
			Assert::IsTrue(r.Configure(), L"built");
			SkeletonMemo m;
			BindLaneToMemo(r.lane, m);
			r.lane.OnSourceFrame(r.View(0));
			const AcquireResult a = m.Acquire(kP5MonitorTiles, 0, AcquireForm::Cpu);
			Assert::IsTrue(a.status == AcquireStatus::FilledAtRead, L"published before the seal");
			const std::vector<ProductFrameResult>& res = r.lane.OnRenderStart(0, 4000);
			bool p5Live = false;
			uint32_t p5Missing = 0;
			for (const ProductFrameResult& x : res)
				if (x.id == kP5MonitorTiles) {
					p5Live = x.liveFilled;
					p5Missing = x.missingInputs;
				}
			Assert::IsTrue(p5Live && p5Missing != 0, L"sealed live with its lag-1 input missing");
			Assert::IsTrue(r.lane.SealReusedPublished() == 1u, L"only P1 (no missing input) came from the publication");
			const std::vector<uint8_t> slot = r.SlotBytes(kP5MonitorTiles, 0);
			Assert::IsTrue(!slot.empty() && slot[0] == 0xAA, L"the missing-input form, computed by the law");
		}

		// WAIVER-C5-GF-T5 (GF-P1 MUST-3, GV AMEND, binder side): a lane with NO GPU dispatcher (a shadow or raised lane) bound to a memo
		// that has an owner GPU device attached keeps GPU presence: the non-view GPU owner fill is served, not refused
		// NoGpuDispatcher. The memo's device is attached BEFORE the bind (the attach-after-bind order is the SkeletonModule's: P2b).
		// Category Cpu: hardware-free (a fake IOwnerGpuDevice; no device).
		BEGIN_TEST_METHOD_ATTRIBUTE(BindKeepsPresenceWithAnOwnerDevice)
			TEST_METHOD_ATTRIBUTE(L"Category", L"Cpu")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(BindKeepsPresenceWithAnOwnerDevice)
		{
			struct FakeDevice : IOwnerGpuDevice
			{
				int gpu = 0, dispatch = 0, texture = 0;
				void* OwnerGpu() const override { return const_cast<int*>(&gpu); }
				void* OwnerDispatch() const override { return const_cast<int*>(&dispatch); }
				bool CreateOwnerTexture(const TexShape& shape, GpuMemoHandle& out, std::string&) override
				{
					out.resource = &texture;
					out.width = shape.w;
					out.height = shape.h;
					out.keepAlive = std::make_shared<int>(1);
					return true;
				}
			} device;
			LaneRig r;
			Assert::IsTrue(r.Configure(), L"built");
			Assert::IsFalse(r.lane.HasGpuDispatcher(), L"the lane has no GPU dispatcher");
			SkeletonMemo m;
			m.SetGpuDevice(&device);
			Assert::IsTrue(m.HasGpuDevice(), L"the owner device is attached");
			m.SetGpuBudget(true, 1u << 20);
			int gpuCalls = 0;
			m.RegisterOwnerFill(kP1HalfLuma, AcquireForm::Gpu, kLaneUnit, GpuFill(&gpuCalls, 64, 64));
			BindLaneToMemo(r.lane, m);
			const AcquireResult a = m.Acquire(kP1HalfLuma, 1, AcquireForm::Gpu);
			Assert::IsTrue(a.cause != AcquireCause::NoGpuDispatcher, L"presence is kept: not refused for want of a dispatcher");
			Assert::IsTrue(a.Ok() && a.status == AcquireStatus::FilledAtRead && a.gpu.Valid(), L"the non-view GPU fill is served");
			Assert::AreEqual(1, gpuCalls, L"the owner's GPU fill ran");
			m.SetGpuDevice(nullptr);
		}

		// WAIVER-C5-GF-T5: and without an owner device the bind does not invent presence: the same fill is refused NoGpuDispatcher.
		TEST_METHOD(BindWithoutDeviceStaysAbsent)
		{
			LaneRig r;
			Assert::IsTrue(r.Configure(), L"built");
			Assert::IsFalse(r.lane.HasGpuDispatcher(), L"the lane has no GPU dispatcher");
			SkeletonMemo m;
			Assert::IsFalse(m.HasGpuDevice(), L"no owner device");
			m.SetGpuBudget(true, 1u << 20);
			int gpuCalls = 0;
			m.RegisterOwnerFill(kP1HalfLuma, AcquireForm::Gpu, kLaneUnit, GpuFill(&gpuCalls, 64));
			BindLaneToMemo(r.lane, m);
			const AcquireResult a = m.Acquire(kP1HalfLuma, 1, AcquireForm::Gpu);
			Assert::IsTrue(a.status == AcquireStatus::Unavailable && a.cause == AcquireCause::NoGpuDispatcher, L"no device, no dispatcher: NoGpuDispatcher");
			Assert::AreEqual(0, gpuCalls, L"no GPU work");
		}

		// R21 Fix 2a (adversarial + in-test negative controls): a lazily-created no-lane memo from MemoFor()
		// immediately plans GPU dispatcher presence (and ledger grant) without waiting for a prior Begin(),
		// and immediately replans on GPU attach/detach lifecycle flips without waiting 240 frames.
		// Category Cpu: hardware-free (fake pl_gpu pointer used for presence only, never dereferenced).
		BEGIN_TEST_METHOD_ATTRIBUTE(LazyNoLaneMemoForPlansGpuDispatcherImmediatelyAndTracksGpuLifecycle)
			TEST_METHOD_ATTRIBUTE(L"Category", L"Cpu")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(LazyNoLaneMemoForPlansGpuDispatcherImmediatelyAndTracksGpuLifecycle)
		{
			namespace RE = RendererExtension;

			Vpv::Skeleton::SkeletonModule sk([](ConfigFile&) { return false; });   // analysis_lane off (no-lane memo path)
			CardProfile card;
			card.vramBudgetBytes = 1ull << 30;
			card.vramUsageBytes = 0;
			sk.SetCardForTesting(card);

			int gpuCalls = 0;
			Assert::IsTrue(
				sk.RegisterOwnerFill(kP1HalfLuma, AcquireForm::Gpu, kLaneUnit, GpuFill(&gpuCalls, 1u << 20, 1u << 20))
					== SkeletonMemo::Registration::Registered,
				L"P1 GPU owner fill registered");

			auto sendLifecycle = [&sk](const void* host, RE::Lifecycle kind, pl_gpu gpu = nullptr) {
				RE::LifecycleFacts f;
				f.host = host;
				f.kind = kind;
				f.gpu = gpu;
				sk.OnLifecycle(f);
			};

			// ---- Negative Control #1: CPU-only host (NO GpuCreated, NO Begin) -> MUST fail with NoGpuDispatcher ----
			{
				int cpuOnlyHost = 0;
				Assert::IsNull(sk.MemoOf(&cpuOnlyHost), L"no memo before lazy MemoFor");
				const Vpv::Skeleton::FrameMemo fmCpu = sk.MemoFor(&cpuOnlyHost, 2);
				Assert::IsTrue(fmCpu.Valid() && fmCpu.gpu == nullptr, L"CPU-only lazy memo created without pl_gpu");
				Assert::IsFalse(fmCpu.memo->LaneBound(), L"no-lane memo");
				Assert::IsFalse(fmCpu.memo->HasGpuDevice(), L"no owner GPU device attached");
				const AcquireResult rCpu = fmCpu.memo->Acquire(kP1HalfLuma, 2, AcquireForm::Gpu);
				Assert::IsTrue(rCpu.status == AcquireStatus::Unavailable && rCpu.cause == AcquireCause::NoGpuDispatcher,
					L"negative control: CPU-only host MUST return AcquireCause::NoGpuDispatcher");
				Assert::AreEqual(0, gpuCalls, L"negative control: GPU owner fill must not run without GPU");
				Assert::AreEqual<uint64_t>(1, fmCpu.memo->CountersOf(kP1HalfLuma).unavailable[size_t(AcquireCause::NoGpuDispatcher)],
					L"negative control: NoGpuDispatcher counted");
				Assert::IsTrue(AnyLine(fmCpu.memo->StatusLines(), "no GPU dispatcher 1"),
					L"negative control: NoGpuDispatcher visible on status lines");
				sendLifecycle(&cpuOnlyHost, RE::Lifecycle::Shutdown);
			}

			// ---- Real GPU-attached host path (matching R21 frame 1): GpuCreated BEFORE MemoFor, ZERO prior Begin ----
			{
				int gpuHost = 0;
				int fakeGpu = 0;
				sendLifecycle(&gpuHost, RE::Lifecycle::GpuCreated, reinterpret_cast<pl_gpu>(&fakeGpu));   // never dereferenced
				Assert::IsNull(sk.MemoOf(&gpuHost), L"memo is still lazily created on MemoFor");

				const Vpv::Skeleton::FrameMemo fmGpu = sk.MemoFor(&gpuHost, 2);   // no prior OnFrame(Begin)!
				Assert::IsTrue(fmGpu.Valid() && fmGpu.gpu == &fakeGpu, L"lazy memo carries the renderer's pl_gpu");
				Assert::IsFalse(fmGpu.memo->LaneBound(), L"no-lane memo");
				Assert::IsTrue(fmGpu.memo->HasGpuDevice(), L"owner GPU device attached on lazy create");

				const AcquireResult rGpu = fmGpu.memo->Acquire(kP1HalfLuma, 2, AcquireForm::Gpu);
				Assert::IsTrue(rGpu.cause != AcquireCause::NoGpuDispatcher,
					L"R21 frame 1: lazy MemoFor after GpuCreated MUST plan GPU dispatcher immediately without Begin");
				Assert::IsTrue(rGpu.status == AcquireStatus::FilledAtRead && rGpu.gpu.Valid(),
					L"R21 frame 1: GPU owner fill is served immediately on lazy MemoFor");
				Assert::AreEqual(1, gpuCalls, L"GPU owner fill ran once");
				Assert::AreEqual<uint64_t>(0, fmGpu.memo->CountersOf(kP1HalfLuma).unavailable[size_t(AcquireCause::NoGpuDispatcher)],
					L"zero spurious NoGpuDispatcher on frame 1");

				// Adversarial teardown check: after GpuReleased, a fresh MemoFor without GpuCreated MUST drop back to NoGpuDispatcher.
				sendLifecycle(&gpuHost, RE::Lifecycle::GpuReleased);
				Assert::IsNull(sk.MemoOf(&gpuHost), L"GpuReleased dropped the memo");
				const Vpv::Skeleton::FrameMemo fmAfterRel = sk.MemoFor(&gpuHost, 3);
				Assert::IsTrue(fmAfterRel.Valid() && fmAfterRel.gpu == nullptr && !fmAfterRel.memo->HasGpuDevice(),
					L"post-GpuReleased memo has no pl_gpu");
				const AcquireResult rAfterRel = fmAfterRel.memo->Acquire(kP1HalfLuma, 3, AcquireForm::Gpu);
				Assert::IsTrue(rAfterRel.status == AcquireStatus::Unavailable && rAfterRel.cause == AcquireCause::NoGpuDispatcher,
					L"post-GpuReleased memo without new GpuCreated MUST return NoGpuDispatcher");
				Assert::AreEqual(1, gpuCalls, L"no GPU work after GpuReleased");
				sendLifecycle(&gpuHost, RE::Lifecycle::Shutdown);
			}

			// ---- Adversarial Lifecycle Case: MemoFor BEFORE GpuCreated, THEN GpuCreated, THEN immediate MemoFor(frame 2) ----
			// Also tests the reverse flip (GpuCreated with nullptr -> immediate MemoFor(frame 3)) without waiting 240 frames.
			{
				int lateHost = 0;
				int lateFakeGpu = 0;

				// Frame 1 (before GpuCreated): memo is planned with plannedGpuPresent == false -> NoGpuDispatcher.
				const Vpv::Skeleton::FrameMemo fm1 = sk.MemoFor(&lateHost, 1);
				Assert::IsTrue(fm1.Valid() && fm1.gpu == nullptr && !fm1.memo->HasGpuDevice(),
					L"pre-GpuCreated memo has no GPU");
				const AcquireResult r1 = fm1.memo->Acquire(kP1HalfLuma, 1, AcquireForm::Gpu);
				Assert::IsTrue(r1.status == AcquireStatus::Unavailable && r1.cause == AcquireCause::NoGpuDispatcher,
					L"frame 1 before GpuCreated MUST fail with NoGpuDispatcher");
				Assert::AreEqual(1, gpuCalls, L"frame 1 before GpuCreated ran no GPU work");

				// GPU arrives after the memo was already planned!
				sendLifecycle(&lateHost, RE::Lifecycle::GpuCreated, reinterpret_cast<pl_gpu>(&lateFakeGpu));
				Assert::IsTrue(sk.MemoOf(&lateHost) == fm1.memo.get(),
					L"same MemoHost survives GpuCreated (already planned with plannedGpuPresent=false)");

				// Frame 2 (IMMEDIATELY after GpuCreated, NOT 240 frames later, NO Begin!): plannedGpuPresent flip MUST replan now!
				const Vpv::Skeleton::FrameMemo fm2 = sk.MemoFor(&lateHost, 2);
				Assert::IsTrue(fm2.Valid() && fm2.memo.get() == fm1.memo.get() && fm2.gpu == &lateFakeGpu && fm2.memo->HasGpuDevice(),
					L"frame 2 MemoFor sees the newly attached pl_gpu on the same memo");
				const AcquireResult r2 = fm2.memo->Acquire(kP1HalfLuma, 2, AcquireForm::Gpu);
				Assert::IsTrue(r2.cause != AcquireCause::NoGpuDispatcher,
					L"frame 2 after GpuCreated MUST immediately replan (not stuck for 240 frames)");
				Assert::IsTrue(r2.status == AcquireStatus::FilledAtRead && r2.gpu.Valid(),
					L"frame 2 GPU owner fill succeeds immediately after GpuCreated");
				Assert::AreEqual(2, gpuCalls, L"frame 2 GPU owner fill ran");

				// Frame 3 (reverse flip: GpuCreated with nullptr detaches GPU on the same MemoHost): MUST immediately replan to absent!
				sendLifecycle(&lateHost, RE::Lifecycle::GpuCreated, nullptr);
				const Vpv::Skeleton::FrameMemo fm3 = sk.MemoFor(&lateHost, 3);
				Assert::IsTrue(fm3.Valid() && fm3.memo.get() == fm1.memo.get() && fm3.gpu == nullptr && !fm3.memo->HasGpuDevice(),
					L"frame 3 MemoFor sees GPU detached on the same memo");
				const AcquireResult r3 = fm3.memo->Acquire(kP1HalfLuma, 3, AcquireForm::Gpu);
				Assert::IsTrue(r3.status == AcquireStatus::Unavailable && r3.cause == AcquireCause::NoGpuDispatcher,
					L"frame 3 after null GpuCreated MUST immediately replan back to NoGpuDispatcher");
				Assert::AreEqual(2, gpuCalls, L"frame 3 ran no GPU work after GPU detached");

				sendLifecycle(&lateHost, RE::Lifecycle::GpuReleased);
			}
		}
	};

	// G0 gate (owner rule 2026-11-13, refs/129 COMMON RULE): RECURSIVE get-or-compute-and-publish. A fill that needs a
	// prerequisite asks for it through Acquire; the prerequisite's OWNER computes it once and PUBLISHES it; later readers reuse
	// it. The re-entrancy guard is per key, cycles are detected by key (same thread and across threads), no lock is held across
	// a nested fill. Every scenario runs bounded on its own thread: a deadlock fails the test instead of hanging the run.
	template <class F>
	bool RunsWithin(F f, int ms)
	{
		auto done = std::make_shared<std::promise<void>>();
		std::future<void> fut = done->get_future();
		std::thread t([f, done]() mutable {
			f();
			done->set_value();
		});
		if (fut.wait_for(std::chrono::milliseconds(ms)) == std::future_status::ready) {
			t.join();
			return true;
		}
		t.detach();   // deadlocked: the state is shared_ptr-owned by the thread; the test fails
		return false;
	}

	void Fill8(OwnerFillOutput& out, uint8_t v) { out.bytes.assign(8, v); }

	bool WaitFlag(const std::atomic<bool>& f)
	{
		for (int i = 0; i < 5000 && !f.load(); ++i)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		return f.load();
	}

	TEST_CLASS(SkeletonAcquireNestedTests)
	{
	public:
		// B's owner fill needs A: it calls Acquire(A); A is computed by A's owner and PUBLISHED; a second reader of A gets
		// Reused (the same spot); A computed exactly once for the frame. While A's fill runs (nested inside B's), another
		// thread's Acquire of a different key completes: no lock is held across the nested fill.
		TEST_METHOD(NestedFillPublishesThePrerequisite)
		{
			struct S {
				SkeletonMemo m;
				std::atomic<int> aCalls{ 0 }, aOther{ 0 }, bCalls{ 0 };
				AcquireStatus inner = AcquireStatus::Unavailable;
				const uint8_t* innerCpu = nullptr;
				bool otherKeyProceeded = false;
				AcquireResult a2, b1, b2;
			};
			auto st = std::make_shared<S>();
			S* p = st.get();
			OwnerFill fa;
			fa.fill = [p](const OwnerFillRequest& r, SkeletonMemo& memo, OwnerFillOutput& out) {
				if (r.seq != 5) {
					p->aOther.fetch_add(1);
					Fill8(out, 0x22);
					return true;
				}
				p->aCalls.fetch_add(1);
				// A different key (A, frame 6) from another thread while this nested fill runs: must not block.
				SkeletonMemo* mm = &memo;
				std::future<AcquireResult> other = std::async(std::launch::async, [mm]() { return mm->Acquire(kP10GrainSplit, 6, AcquireForm::Cpu); });
				p->otherKeyProceeded = other.wait_for(std::chrono::seconds(3)) == std::future_status::ready && other.get().Ok();
				Fill8(out, 0xA5);
				return true;
			};
			OwnerFill fb;
			fb.fill = [p](const OwnerFillRequest& r, SkeletonMemo& memo, OwnerFillOutput& out) {
				p->bCalls.fetch_add(1);
				const AcquireResult a = memo.Acquire(kP10GrainSplit, r.seq, AcquireForm::Cpu);   // the prerequisite, via Acquire
				p->inner = a.status;
				p->innerCpu = a.cpu;
				if (!a.Ok() || a.cpuBytes != 8u)
					return false;
				out.bytes.assign(a.cpu, a.cpu + a.cpuBytes);
				out.bytes[0] ^= 0xFF;   // B derived from A
				return true;
			};
			Assert::IsTrue(st->m.RegisterOwnerFill(kP10GrainSplit, AcquireForm::Cpu, kGrainUnit, fa) == SkeletonMemo::Registration::Registered, L"A owner");
			Assert::IsTrue(st->m.RegisterOwnerFill(kP11RegionBlockStats, AcquireForm::Cpu, kRegionUnit, fb) == SkeletonMemo::Registration::Registered, L"B owner");
			const bool ran = RunsWithin([st]() {
				st->b1 = st->m.Acquire(kP11RegionBlockStats, 5, AcquireForm::Cpu);
				st->a2 = st->m.Acquire(kP10GrainSplit, 5, AcquireForm::Cpu);   // a later reader of the prerequisite
				st->b2 = st->m.Acquire(kP11RegionBlockStats, 5, AcquireForm::Cpu);
			}, 10000);
			Assert::IsTrue(ran, L"nested Acquire completes (no lock across the nested fill, no global guard)");
			Assert::IsTrue(st->b1.status == AcquireStatus::FilledAtRead && st->b1.cpuBytes == 8u && st->b1.cpu[0] == uint8_t(0xA5 ^ 0xFF), L"B filled from A");
			Assert::IsTrue(st->inner == AcquireStatus::FilledAtRead, L"inside B's fill, A was computed by ITS owner (filled at read)");
			Assert::IsTrue(st->a2.status == AcquireStatus::Reused && st->a2.source == AcquireSource::Memo, L"the second reader of A reuses the publication");
			Assert::IsTrue(st->a2.cpu == st->innerCpu && st->a2.cpu[0] == 0xA5, L"the same published spot");
			Assert::IsTrue(st->b2.status == AcquireStatus::Reused, L"B reused");
			Assert::AreEqual(1, st->aCalls.load(), L"A computed exactly once for the frame");
			Assert::AreEqual(1, st->bCalls.load(), L"B computed exactly once for the frame");
			Assert::IsTrue(st->otherKeyProceeded && st->aOther.load() == 1, L"another key proceeded during the nested fill");
			const AcquireCounters ca = st->m.CountersOf(kP10GrainSplit);
			Assert::IsTrue(ca.ownerFillCalls == 2 && ca.filledAtRead == 2 && ca.reusedMemo == 1 && ca.Unavailable() == 0, L"A counted: frames 5 and 6 once each, one reuse");
		}

		// A -> B -> A on one thread: the inner A is refused (Reentrant, by key), visible; nothing deadlocks; each fill ran once.
		TEST_METHOD(CycleOnOneThreadIsDetectedByKey)
		{
			struct S {
				SkeletonMemo m;
				std::atomic<int> aCalls{ 0 }, bCalls{ 0 };
				AcquireCause innerA = AcquireCause::None;
				AcquireStatus innerB = AcquireStatus::Unavailable;
				AcquireResult outer;
			};
			auto st = std::make_shared<S>();
			S* p = st.get();
			OwnerFill fa;
			fa.fill = [p](const OwnerFillRequest& r, SkeletonMemo& memo, OwnerFillOutput& out) {
				p->aCalls.fetch_add(1);
				p->innerB = memo.Acquire(kP11RegionBlockStats, r.seq, AcquireForm::Cpu).status;
				Fill8(out, 0x11);
				return true;
			};
			OwnerFill fb;
			fb.fill = [p](const OwnerFillRequest& r, SkeletonMemo& memo, OwnerFillOutput& out) {
				p->bCalls.fetch_add(1);
				p->innerA = memo.Acquire(kP10GrainSplit, r.seq, AcquireForm::Cpu).cause;   // closes the cycle
				Fill8(out, 0x33);
				return true;
			};
			st->m.RegisterOwnerFill(kP10GrainSplit, AcquireForm::Cpu, kGrainUnit, fa);
			st->m.RegisterOwnerFill(kP11RegionBlockStats, AcquireForm::Cpu, kRegionUnit, fb);
			const bool ran = RunsWithin([st]() { st->outer = st->m.Acquire(kP10GrainSplit, 2, AcquireForm::Cpu); }, 10000);
			Assert::IsTrue(ran, L"A -> B -> A does not deadlock");
			Assert::IsTrue(st->innerA == AcquireCause::Reentrant, L"the cycle is refused by key");
			Assert::IsTrue(st->innerB == AcquireStatus::FilledAtRead && st->outer.status == AcquireStatus::FilledAtRead, L"B and A complete");
			Assert::IsTrue(st->aCalls.load() == 1 && st->bCalls.load() == 1, L"each fill once");
			Assert::IsTrue(st->m.CountersOf(kP10GrainSplit).unavailable[size_t(AcquireCause::Reentrant)] == 1, L"counted");
			Assert::IsTrue(AnyLine(st->m.StatusLines(), "owner fill asked for its own product 1"), L"shown");
		}

		// A -> B on thread 1 while B -> A on thread 2 (both fills in flight): the second waiter sees the cycle by key and is refused
		// (Reentrant); the first waiter reuses the other's publication. Without by-key cycle detection this deadlocks.
		TEST_METHOD(CycleAcrossThreadsIsDetectedNotDeadlocked)
		{
			struct S {
				SkeletonMemo m;
				std::atomic<bool> aStarted{ false }, bStarted{ false };
				std::atomic<int> aCalls{ 0 }, bCalls{ 0 };
				AcquireResult innerB, innerA, outerA, outerB;
			};
			auto st = std::make_shared<S>();
			S* p = st.get();
			OwnerFill fa;
			fa.fill = [p](const OwnerFillRequest& r, SkeletonMemo& memo, OwnerFillOutput& out) {
				p->aCalls.fetch_add(1);
				p->aStarted = true;
				WaitFlag(p->bStarted);
				p->innerB = memo.Acquire(kP11RegionBlockStats, r.seq, AcquireForm::Cpu);
				Fill8(out, 0x44);
				return true;
			};
			OwnerFill fb;
			fb.fill = [p](const OwnerFillRequest& r, SkeletonMemo& memo, OwnerFillOutput& out) {
				p->bCalls.fetch_add(1);
				p->bStarted = true;
				WaitFlag(p->aStarted);
				p->innerA = memo.Acquire(kP10GrainSplit, r.seq, AcquireForm::Cpu);
				Fill8(out, 0x55);
				return true;
			};
			st->m.RegisterOwnerFill(kP10GrainSplit, AcquireForm::Cpu, kGrainUnit, fa);
			st->m.RegisterOwnerFill(kP11RegionBlockStats, AcquireForm::Cpu, kRegionUnit, fb);
			const bool ran = RunsWithin([st]() {
				std::thread t1([st]() { st->outerA = st->m.Acquire(kP10GrainSplit, 3, AcquireForm::Cpu); });
				std::thread t2([st]() { st->outerB = st->m.Acquire(kP11RegionBlockStats, 3, AcquireForm::Cpu); });
				t1.join();
				t2.join();
			}, 15000);
			Assert::IsTrue(ran, L"a cross-thread cycle of fills does not deadlock");
			const bool aRefused = st->innerA.cause == AcquireCause::Reentrant;
			const bool bRefused = st->innerB.cause == AcquireCause::Reentrant;
			Assert::IsTrue(aRefused != bRefused, L"exactly one inner request closes the cycle and is refused by key");
			const AcquireResult& waited = aRefused ? st->innerB : st->innerA;
			Assert::IsTrue(waited.status == AcquireStatus::Reused && waited.source == AcquireSource::Memo, L"the other waited for the one fill and reused it");
			Assert::IsTrue(st->outerA.status == AcquireStatus::FilledAtRead && st->outerB.status == AcquireStatus::FilledAtRead, L"both outer fills complete");
			Assert::IsTrue(st->aCalls.load() == 1 && st->bCalls.load() == 1, L"each computed once");
		}

		// Three threads: T0's fill of A is in flight (held open); T1 and T2 both wait on that one in-flight key. Two waiters on one
		// key is not a cycle: neither is refused (no false Reentrant), both reuse the single fill.
		TEST_METHOD(TwoWaitersOnOneInFlightKeyAreNotACycle)
		{
			struct S {
				SkeletonMemo m;
				std::atomic<bool> aStarted{ false }, release{ false };
				std::atomic<int> aCalls{ 0 };
				AcquireResult r0, r1, r2;
			};
			auto st = std::make_shared<S>();
			S* p = st.get();
			OwnerFill fa;
			fa.fill = [p](const OwnerFillRequest&, SkeletonMemo&, OwnerFillOutput& out) {
				p->aCalls.fetch_add(1);
				p->aStarted = true;
				WaitFlag(p->release);
				Fill8(out, 0x66);
				return true;
			};
			st->m.RegisterOwnerFill(kP10GrainSplit, AcquireForm::Cpu, kGrainUnit, fa);
			const bool ran = RunsWithin([st]() {
				std::thread t0([st]() { st->r0 = st->m.Acquire(kP10GrainSplit, 4, AcquireForm::Cpu); });
				WaitFlag(st->aStarted);
				std::thread t1([st]() { st->r1 = st->m.Acquire(kP10GrainSplit, 4, AcquireForm::Cpu); });
				std::thread t2([st]() { st->r2 = st->m.Acquire(kP10GrainSplit, 4, AcquireForm::Cpu); });
				std::this_thread::sleep_for(std::chrono::milliseconds(100));   // let both waiters block on the in-flight fill
				st->release = true;
				t0.join();
				t1.join();
				t2.join();
			}, 15000);
			Assert::IsTrue(ran, L"two waiters on one in-flight key complete");
			Assert::IsTrue(st->r0.status == AcquireStatus::FilledAtRead, L"the filler filled");
			Assert::IsTrue(st->r1.status == AcquireStatus::Reused && st->r2.status == AcquireStatus::Reused, L"both waiters reuse the one fill");
			Assert::IsTrue(st->r1.cause != AcquireCause::Reentrant && st->r2.cause != AcquireCause::Reentrant, L"no false Reentrant");
			Assert::AreEqual(1, st->aCalls.load(), L"computed once");
			Assert::AreEqual(size_t(0), size_t(st->m.CountersOf(kP10GrainSplit).unavailable[size_t(AcquireCause::Reentrant)]), L"no Reentrant counted");
		}

		// The cycle of CycleAcrossThreads, but the filler whose inner request is refused DECLINES (returns false): the other waiter
		// (blocked on that filler's key) is woken with the cause (OwnerDeclined) and does not hang.
		TEST_METHOD(CycleWhereTheRefusedFillerDeclinesWakesTheOtherWaiter)
		{
			struct S {
				SkeletonMemo m;
				std::atomic<bool> aStarted{ false }, bStarted{ false };
				AcquireResult innerB, innerA, outerA, outerB;
			};
			auto st = std::make_shared<S>();
			S* p = st.get();
			OwnerFill fa;
			fa.fill = [p](const OwnerFillRequest& r, SkeletonMemo& memo, OwnerFillOutput& out) {
				p->aStarted = true;
				WaitFlag(p->bStarted);
				p->innerB = memo.Acquire(kP11RegionBlockStats, r.seq, AcquireForm::Cpu);
				if (!p->innerB.Ok())
					return false;   // the refused filler declines
				Fill8(out, 0x77);
				return true;
			};
			OwnerFill fb;
			fb.fill = [p](const OwnerFillRequest& r, SkeletonMemo& memo, OwnerFillOutput& out) {
				p->bStarted = true;
				WaitFlag(p->aStarted);
				p->innerA = memo.Acquire(kP10GrainSplit, r.seq, AcquireForm::Cpu);
				if (!p->innerA.Ok())
					return false;   // the refused filler declines
				Fill8(out, 0x88);
				return true;
			};
			st->m.RegisterOwnerFill(kP10GrainSplit, AcquireForm::Cpu, kGrainUnit, fa);
			st->m.RegisterOwnerFill(kP11RegionBlockStats, AcquireForm::Cpu, kRegionUnit, fb);
			const bool ran = RunsWithin([st]() {
				std::thread t1([st]() { st->outerA = st->m.Acquire(kP10GrainSplit, 7, AcquireForm::Cpu); });
				std::thread t2([st]() { st->outerB = st->m.Acquire(kP11RegionBlockStats, 7, AcquireForm::Cpu); });
				t1.join();
				t2.join();
			}, 15000);
			Assert::IsTrue(ran, L"the cycle with a declining refused filler does not deadlock");
			const bool aRefused = st->innerA.cause == AcquireCause::Reentrant;
			const bool bRefused = st->innerB.cause == AcquireCause::Reentrant;
			Assert::IsTrue(aRefused != bRefused, L"exactly one inner request closes the cycle");
			const AcquireResult& woken = aRefused ? st->innerB : st->innerA;
			Assert::IsTrue(woken.status == AcquireStatus::Unavailable && woken.cause == AcquireCause::OwnerDeclined, L"the other waiter wakes with the cause (OwnerDeclined)");
			Assert::IsTrue(st->outerA.cause == AcquireCause::OwnerDeclined && st->outerB.cause == AcquireCause::OwnerDeclined, L"both outer requests see the decline");
		}

		// R2 (lead 12): THE sanctioned GPU context. A non-view GPU fill gets it (and only it: no pl_gpu of its own), CPU and view fills
		// get none; its textures are accounted against the fill's reserve (refused above it, counted, worded with the decline); it is
		// worded; without a device it allocates nothing and says so.
		// Category Cpu: the name says GPU, the body is hardware-free (a fake IOwnerGpuDevice; no device).
		BEGIN_TEST_METHOD_ATTRIBUTE(OwnerGpuContextIsSanctionedAndAccounted)
			TEST_METHOD_ATTRIBUTE(L"Category", L"Cpu")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(OwnerGpuContextIsSanctionedAndAccounted)
		{
			struct FakeDevice : IOwnerGpuDevice
			{
				int gpu = 0, dispatch = 0, texture = 0;
				int created = 0;
				std::vector<TexShape> shapes;
				void* OwnerGpu() const override { return const_cast<int*>(&gpu); }
				void* OwnerDispatch() const override { return const_cast<int*>(&dispatch); }
				bool CreateOwnerTexture(const TexShape& shape, GpuMemoHandle& out, std::string&) override
				{
					++created;
					shapes.push_back(shape);
					out.resource = &texture;
					out.width = shape.w;
					out.height = shape.h;
					out.keepAlive = std::make_shared<int>(created);
					return true;
				}
			} device;
			const TexShape plane{ 16, 8, TexFormat::R16 };   // 256 bytes
			const uint64_t planeBytes = ShapeBytes(plane);
			Assert::AreEqual<uint64_t>(256, planeBytes);

			SkeletonMemo m;
			m.SetNames(nullptr);
			m.SetGpuDispatcherPresent(true);
			m.SetGpuBudget(true, 1u << 20);
			m.SetGpuDevice(&device);

			// A non-view GPU fill: the context is there, names the device's handles, allocates within the reserve.
			int ran = 0;
			OwnerFill made;
			made.reserveBytes = planeBytes;
			made.fill = [&](const OwnerFillRequest& r, SkeletonMemo&, OwnerFillOutput& out) {
				++ran;
				Assert::IsNotNull(r.gpu, L"a non-view GPU fill gets the sanctioned context");
				Assert::IsTrue(r.gpu->Gpu() == &device.gpu && r.gpu->Dispatch() == &device.dispatch, L"the device's pl_gpu and pl_dispatch");
				Assert::AreEqual<uint64_t>(planeBytes, r.gpu->Granted(), L"granted exactly the fill's reserve");
				std::string why;
				if (!r.gpu->Allocate(plane, out.gpu, why))
					return false;
				GpuMemoHandle second;
				Assert::IsFalse(r.gpu->Allocate(plane, second, why), L"a second texture would pass the reserve: refused");
				Assert::IsTrue(why.find("reserve") != std::string::npos, L"and worded");
				Assert::IsFalse(second.Valid(), L"nothing handed out");
				return true;
			};
			m.RegisterOwnerFill(kP10GrainSplit, AcquireForm::Gpu, kGrainUnit, made);
			const AcquireResult a = m.Acquire(kP10GrainSplit, 1, AcquireForm::Gpu);
			Assert::IsTrue(a.status == AcquireStatus::FilledAtRead && a.gpu.Valid(), L"published");
			Assert::AreEqual<uint64_t>(planeBytes, a.gpu.bytes, L"the texture's bytes are the ledger's ShapeBytes");
			Assert::AreEqual(1, device.created, L"one texture made through the device");
			Assert::AreEqual<uint64_t>(planeBytes, m.GpuBytes(), L"budgeted against the grant");
			const AcquireCounters c = m.CountersOf(kP10GrainSplit);
			Assert::IsTrue(c.gpuAllocMade == 1 && c.gpuAllocBytes == planeBytes && c.gpuAllocRefused == 1, L"made / refused counted");
			Assert::IsTrue(AnyLine(m.StatusLines(), "gpu textures made 1"), L"on the status lines");
			Assert::AreEqual(1, ran);

			// A fill whose texture is above its reserve declines with the context's reason (closing: OwnerGpuContextClosesAndAccumulates).
			OwnerFill greedy;
			greedy.reserveBytes = planeBytes - 1u;
			greedy.fill = [&](const OwnerFillRequest& r, SkeletonMemo&, OwnerFillOutput& out) {
				std::string why;
				return r.gpu != nullptr && r.gpu->Allocate(plane, out.gpu, why);
			};
			m.ReplaceOwnerFill(kP10GrainSplit, AcquireForm::Gpu, kGrainUnit, greedy);
			const AcquireResult g = m.Acquire(kP10GrainSplit, 2, AcquireForm::Gpu);
			Assert::IsTrue(g.cause == AcquireCause::OwnerDeclined, L"a texture above the reserve: the fill declines");
			Assert::AreEqual(1, device.created, L"the device was never asked");
			Assert::IsTrue(m.LastCause(kP10GrainSplit).find("reserve") != std::string::npos, L"the decline names the reserve");

			// A view fill and a CPU fill get no context.
			int viewSeen = -1, cpuSeen = -1;
			OwnerFill view;
			view.view = true;
			view.fill = [&](const OwnerFillRequest& r, SkeletonMemo&, OwnerFillOutput& out) {
				viewSeen = r.gpu != nullptr ? 1 : 0;
				static int other = 0;
				out.gpu.view = &other;
				return true;
			};
			m.RegisterOwnerFill(kP1HalfLuma, AcquireForm::Gpu, kLaneUnit, view);
			Assert::IsTrue(m.Acquire(kP1HalfLuma, 3, AcquireForm::Gpu).Ok());
			Assert::AreEqual(0, viewSeen, L"a view allocates nothing: no context");
			OwnerFill cpu;
			cpu.fill = [&](const OwnerFillRequest& r, SkeletonMemo&, OwnerFillOutput& out) {
				cpuSeen = r.gpu != nullptr ? 1 : 0;
				out.bytes.assign(4, 1);
				return true;
			};
			m.RegisterOwnerFill(kP10GrainSplit, AcquireForm::Cpu, kGrainUnit, cpu);
			Assert::IsTrue(m.Acquire(kP10GrainSplit, 3, AcquireForm::Cpu).Ok());
			Assert::AreEqual(0, cpuSeen, L"a CPU fill: no context");

			// No device: the context exists (the fill is non-view) but hands out nothing and says why.
			m.SetGpuDevice(nullptr);
			std::string noDeviceWhy;
			bool sawNullGpu = false;
			OwnerFill orphan;
			orphan.reserveBytes = planeBytes;
			orphan.fill = [&](const OwnerFillRequest& r, SkeletonMemo&, OwnerFillOutput& out) {
				sawNullGpu = r.gpu != nullptr && r.gpu->Gpu() == nullptr && r.gpu->Dispatch() == nullptr;
				return r.gpu != nullptr && r.gpu->Allocate(plane, out.gpu, noDeviceWhy);
			};
			m.ReplaceOwnerFill(kP10GrainSplit, AcquireForm::Gpu, kGrainUnit, orphan);
			Assert::IsTrue(m.Acquire(kP10GrainSplit, 4, AcquireForm::Gpu).cause == AcquireCause::OwnerDeclined, L"declined");
			Assert::IsTrue(sawNullGpu && noDeviceWhy.find("no skeleton GPU device") != std::string::npos, L"no device: nothing, worded");
			Assert::AreEqual(1, device.created);
		}

		// R2: the context on its own: closed = nothing; an empty shape is refused; the grant is cumulative.
		// Category Cpu: the name says GPU, the body is hardware-free (a counting fake IOwnerGpuDevice; no device).
		BEGIN_TEST_METHOD_ATTRIBUTE(OwnerGpuContextClosesAndAccumulates)
			TEST_METHOD_ATTRIBUTE(L"Category", L"Cpu")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(OwnerGpuContextClosesAndAccumulates)
		{
			struct CountingDevice : IOwnerGpuDevice
			{
				int handle = 0, created = 0;
				void* OwnerGpu() const override { return const_cast<int*>(&handle); }
				void* OwnerDispatch() const override { return const_cast<int*>(&handle); }
				bool CreateOwnerTexture(const TexShape&, GpuMemoHandle& out, std::string&) override
				{
					++created;
					out.resource = &handle;
					return true;
				}
			} device;
			const TexShape small{ 4, 4, TexFormat::R8 };   // 16 bytes
			OwnerGpuContext c(&device, 3u * ShapeBytes(small));
			GpuMemoHandle h;
			std::string why;
			for (int i = 0; i < 3; ++i)
				Assert::IsTrue(c.Allocate(small, h, why), L"three fit the grant");
			Assert::IsFalse(c.Allocate(small, h, why), L"the fourth does not: the grant is cumulative");
			Assert::IsFalse(c.Allocate(TexShape{ 0, 4, TexFormat::R8 }, h, why), L"an empty shape is refused");
			Assert::IsTrue(c.Used() == 3u * ShapeBytes(small) && c.Made() == 3u && c.Refused() == 2u);
			c.Close();
			Assert::IsTrue(c.Gpu() == nullptr && c.Dispatch() == nullptr, L"closed: no device handles");
			OwnerGpuContext wide(&device, 1u << 20);
			wide.Close();
			Assert::IsFalse(wide.Allocate(small, h, why), L"closed: nothing allocated");
			Assert::IsTrue(why.find("after its fill returned") != std::string::npos, L"worded");
			Assert::AreEqual(3, device.created, L"the device was asked only within the grant and while open");
		}

		// R19-27 (C5 MUST-2): the context carries the device's compute shared-memory limit, so a fill's kernels check their need
		// without naming the device. Open: the device's figure; closed or no device: 0; a device that does not say: 0 (the default).
		// Category Cpu: hardware-free (fake IOwnerGpuDevice implementations; no device).
		BEGIN_TEST_METHOD_ATTRIBUTE(OwnerGpuContextCarriesTheSharedMemoryLimit)
			TEST_METHOD_ATTRIBUTE(L"Category", L"Cpu")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(OwnerGpuContextCarriesTheSharedMemoryLimit)
		{
			struct SayingDevice : IOwnerGpuDevice
			{
				int handle = 0;
				void* OwnerGpu() const override { return const_cast<int*>(&handle); }
				void* OwnerDispatch() const override { return const_cast<int*>(&handle); }
				bool CreateOwnerTexture(const TexShape&, GpuMemoHandle&, std::string&) override { return false; }
				uint64_t OwnerMaxSharedMemory() const override { return 49152u; }
			} saying;
			struct SilentDevice : IOwnerGpuDevice
			{
				int handle = 0;
				void* OwnerGpu() const override { return const_cast<int*>(&handle); }
				void* OwnerDispatch() const override { return const_cast<int*>(&handle); }
				bool CreateOwnerTexture(const TexShape&, GpuMemoHandle&, std::string&) override { return false; }
			} silent;
			OwnerGpuContext live(&saying, 0);
			Assert::AreEqual(uint64_t(49152), live.MaxSharedMemory(), L"open: the device's limit");
			live.Close();
			Assert::AreEqual(uint64_t(0), live.MaxSharedMemory(), L"closed: 0, like Gpu() / Dispatch()");
			OwnerGpuContext none(nullptr, 0);
			Assert::AreEqual(uint64_t(0), none.MaxSharedMemory(), L"no device: 0");
			OwnerGpuContext quiet(&silent, 0);
			Assert::AreEqual(uint64_t(0), quiet.MaxSharedMemory(), L"a device that does not say: 0 (not known)");
		}
	};
}
