// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// L-B4 step (a): the motion producer's P1 reader over Acquire (P1AcquireSource.h).
//   1. per format: the engine half read through Acquire(kP1HalfLuma, seq, Cpu) is byte-identical to the producer's own
//      HalfLuma8 for the formats whose codes are the shared decode (YUV / 10-bit), and the reader REFUSES (own decode count)
//      the named exception formats (RGB structure luma, raw full-range 4:2:x: FIX-B4-RGB), so the producer keeps its decode;
//   2. the counter test: N producer reads of one frame run the owner fill ONCE (one P1 build per frame), also when the
//      readers are on several threads;
//   3. no memo / a miss: false, how = None, counted unread (the producer keeps its decode and shows it).
// CPU only.

#include <VP-V/core/AheadCeiling.h>
#include <VP-V/core/ComputeOnce.h>
#include <VP-V/core/SkeletonAcquire.h>
#include <VP-V/core/hub/ExtensionHub.h>
#include <VP-V/modules/control/skeleton/SkeletonFacts.h>
#include <VP-V/modules/control/skeleton/SkeletonModule.h>
#include <VP-V/modules/input/source_analysis/P1AcquireSource.h>
#include <VP-V/modules/input/source_analysis/VpFrameAnalysis.h>
#include <VP-V/modules/input/source_analysis/lane/LaneCpuProducts.h>
#include <VP-V/modules/input/source_analysis/lane/P1CpuFill.h>
#include <VP-V/modules/motion/estimation/MotionAheadProducer.h>
#include <VP-V/tests/SyntheticAnalysisFrames.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace P1AcquireSourceTestsNs
{
	using namespace SyntheticAnalysisFrames;
	using namespace VpSkeleton;

	// MAP-RGB-LUMA-PROXY (796a239ea): P1AcquireSource serves EVERY capture format (including P010Full, V210Full,
	// and all RGB formats) byte-identically to MotionAhead::HalfLuma8 (only non-10-bit tap shifts are refused;
	// tested separately in AnotherTapShiftIsRefusedBeforeAcquire).

	// The lane's P1 owner fill for one frame: the lane's own CPU law over the frame's tap view; counts its runs.
	struct P1Owner
	{
		AnalysisLumaSource source;
		std::atomic<int> fills{ 0 };

		bool Register(SkeletonMemo& memo)
		{
			OwnerFill f;
			f.fill = [this](const OwnerFillRequest&, SkeletonMemo&, OwnerFillOutput& out) {
				SourceFrameView view;
				if (!VpFrameAnalysis::SourceViewOf(source, view))
					return false;
				out.bytes.assign(VpLaneProducts::HalfLumaBytes(Dims{ view.width, view.height }), 0);
				++fills;
				return VpLaneProducts::ComputeHalfLuma(view, out.bytes.data(), out.bytes.size());
			};
			return memo.RegisterOwnerFill(kP1HalfLuma, AcquireForm::Cpu, SkeletonMemo::OwnerUnitOf(kP1HalfLuma), std::move(f))
				== SkeletonMemo::Registration::Registered;
		}
	};

	TEST_CLASS(P1AcquireSourceTests)
	{
	public:
		TEST_METHOD(EngineHalfThroughAcquireMatchesTheProducerOrIsRefused)
		{
			for (int k = 0; k < static_cast<int>(Kind::Count); ++k)
			{
				const Kind kind = static_cast<Kind>(k);
				const Frame f = Make(kind, 3);
				SkeletonMemo memo;
				P1Owner owner;
				owner.source = f.source;
				Assert::IsTrue(owner.Register(memo), KindName(kind));
				VpFrameAnalysis::P1AcquireSource reader([&memo](uint64_t seq) { return memo.Acquire(kP1HalfLuma, seq, AcquireForm::Cpu); });
				std::vector<uint8_t> got, want;
				int gw = 0, gh = 0, ww = 0, wh = 0;
				P1ReadKind how = P1ReadKind::None;
				const bool read = reader.ReadEngine8(5, f.source, got, gw, gh, how);
				Assert::IsTrue(read && how != P1ReadKind::None, KindName(kind));
				Assert::IsTrue(MotionAhead::HalfLuma8(f.source, want, ww, wh), KindName(kind));
				Assert::IsTrue(gw == ww && gh == wh && got == want, KindName(kind));
				Assert::AreEqual<uint64_t>(1u, reader.Acquired(), KindName(kind));
				Assert::AreEqual<uint64_t>(0u, reader.OwnDecode(), KindName(kind));
				Assert::AreEqual(1, owner.fills.load(), KindName(kind));
			}
		}

		TEST_METHOD(OneBuildPerFrameForManyProducerReads)
		{
			const Frame f = Make(Kind::P010, 2);
			SkeletonMemo memo;
			P1Owner owner;
			owner.source = f.source;
			Assert::IsTrue(owner.Register(memo));
			VpFrameAnalysis::P1AcquireSource reader([&memo](uint64_t seq) { return memo.Acquire(kP1HalfLuma, seq, AcquireForm::Cpu); });
			std::vector<std::vector<uint8_t>> out(4);
			std::vector<std::thread> pool;
			for (size_t t = 0; t < out.size(); ++t)
				pool.emplace_back([&, t]() {
					int w = 0, h = 0;
					P1ReadKind how = P1ReadKind::None;
					for (int i = 0; i < 3; ++i)
						reader.ReadEngine8(7, f.source, out[t], w, h, how);
				});
			for (std::thread& th : pool) th.join();
			Assert::AreEqual(1, owner.fills.load(), L"one P1 build for 12 reads on 4 threads");
			for (size_t t = 1; t < out.size(); ++t)
				Assert::IsTrue(!out[0].empty() && out[t] == out[0], L"every reader got the one frame's bytes");
			Assert::AreEqual<uint64_t>(12u, reader.Acquired());
			// Another frame builds again (once per frame).
			std::vector<uint8_t> o2;
			int w = 0, h = 0;
			P1ReadKind how = P1ReadKind::None;
			Assert::IsTrue(reader.ReadEngine8(8, f.source, o2, w, h, how));
			Assert::AreEqual(2, owner.fills.load());
		}

		TEST_METHOD(NoMemoOrAMissReadsFalse)
		{
			const Frame f = Make(Kind::P010, 1);
			std::vector<uint8_t> out;
			int w = 0, h = 0;
			P1ReadKind how = P1ReadKind::LiveFill;
			VpFrameAnalysis::P1AcquireSource none;
			Assert::IsFalse(none.ReadEngine8(1, f.source, out, w, h, how));
			Assert::IsTrue(how == P1ReadKind::None);
			// A memo with no owner for P1 (the lane off, no fill): Unavailable, counted unread.
			SkeletonMemo memo;
			VpFrameAnalysis::P1AcquireSource miss([&memo](uint64_t seq) { return memo.Acquire(kP1HalfLuma, seq, AcquireForm::Cpu); });
			how = P1ReadKind::LiveFill;
			Assert::IsFalse(miss.ReadEngine8(1, f.source, out, w, h, how));
			Assert::IsTrue(how == P1ReadKind::None);
			Assert::AreEqual<uint64_t>(1u, miss.Unread());
		}
	};
}

// L-B4 audit SHOULDs 1-3 (P1AcquireSource guards):
//   1. a reader whose P1 is built from another tap depth (msbShift 4, SourceViewOf12) refuses every read before any Acquire;
//   2. a P1 result whose size is not exactly HalfLumaBytes(dims) (the half plane alone, or a larger buffer) is refused (Unread);
//   3. memo reads of one frame are counted apart: one FilledAtRead, the rest MemoReused (labelled LiveFill, not LanePublished).
namespace P1AcquireSourceTestsNs
{
	TEST_CLASS(P1AcquireSourceGuardTests)
	{
	public:
		TEST_METHOD(AnotherTapShiftIsRefusedBeforeAcquire)
		{
			const Frame f = Make(Kind::P010, 4);
			SkeletonMemo memo;
			P1Owner owner;
			owner.source = f.source;
			Assert::IsTrue(owner.Register(memo));
			int acquires = 0;
			VpFrameAnalysis::P1AcquireSource reader([&](uint64_t seq) {
				++acquires;
				return memo.Acquire(kP1HalfLuma, seq, AcquireForm::Cpu);
			}, 4);
			std::vector<uint8_t> out;
			int w = 0, h = 0;
			P1ReadKind how = P1ReadKind::LiveFill;
			Assert::IsFalse(reader.ReadEngine8(3, f.source, out, w, h, how), L"msbShift 4: Engine8Half is not exact");
			Assert::IsTrue(how == P1ReadKind::None);
			Assert::AreEqual<uint64_t>(1u, reader.OwnDecode());
			Assert::AreEqual<uint64_t>(0u, reader.Acquired());
			Assert::AreEqual(0, acquires, L"no Acquire for a refused depth");
			Assert::AreEqual(0, owner.fills.load(), L"no P1 build for a refused depth");
			// The same frame through a shift-6 reader reads (the guard is the shift, not the frame).
			VpFrameAnalysis::P1AcquireSource ten([&memo](uint64_t seq) { return memo.Acquire(kP1HalfLuma, seq, AcquireForm::Cpu); });
			Assert::IsTrue(ten.ReadEngine8(3, f.source, out, w, h, how));
		}

		TEST_METHOD(AP1ResultOfAnotherSizeIsRefused)
		{
			const Frame f = Make(Kind::P010, 5);
			const Dims dims{ static_cast<uint32_t>(f.source.width), static_cast<uint32_t>(f.source.height) };
			const size_t sizes[] = { VpLaneProducts::HalfLumaHalfBytes(dims), VpLaneProducts::HalfLumaBytes(dims) + 2u };
			for (size_t n : sizes)
			{
				SkeletonMemo memo;
				OwnerFill fill;
				fill.fill = [n](const OwnerFillRequest&, SkeletonMemo&, OwnerFillOutput& o) {
					o.bytes.assign(n, 0x40);
					return true;
				};
				Assert::IsTrue(memo.RegisterOwnerFill(kP1HalfLuma, AcquireForm::Cpu, SkeletonMemo::OwnerUnitOf(kP1HalfLuma),
					std::move(fill)) == SkeletonMemo::Registration::Registered);
				VpFrameAnalysis::P1AcquireSource reader([&memo](uint64_t seq) { return memo.Acquire(kP1HalfLuma, seq, AcquireForm::Cpu); });
				std::vector<uint8_t> out;
				int w = 0, h = 0;
				P1ReadKind how = P1ReadKind::LiveFill;
				Assert::IsFalse(reader.ReadEngine8(9, f.source, out, w, h, how), L"size != HalfLumaBytes(dims)");
				Assert::IsTrue(how == P1ReadKind::None);
				Assert::AreEqual<uint64_t>(1u, reader.Unread());
				Assert::AreEqual<uint64_t>(0u, reader.Acquired());
			}
		}

		TEST_METHOD(MemoFillAndReuseAreCountedApart)
		{
			const Frame f = Make(Kind::P010, 6);
			SkeletonMemo memo;
			P1Owner owner;
			owner.source = f.source;
			Assert::IsTrue(owner.Register(memo));
			VpFrameAnalysis::P1AcquireSource reader([&memo](uint64_t seq) { return memo.Acquire(kP1HalfLuma, seq, AcquireForm::Cpu); });
			std::vector<uint8_t> out;
			int w = 0, h = 0;
			for (int i = 0; i < 12; ++i)
			{
				P1ReadKind how = P1ReadKind::None;
				Assert::IsTrue(reader.ReadEngine8(11, f.source, out, w, h, how));
				Assert::IsTrue(how == P1ReadKind::LiveFill, L"a memo result is never labelled LanePublished");
			}
			Assert::AreEqual(1, owner.fills.load());
			Assert::AreEqual<uint64_t>(1u, reader.FilledAtRead(), L"the first read ran the one fill");
			Assert::AreEqual<uint64_t>(11u, reader.MemoReused(), L"the other eleven reused it");
			Assert::AreEqual<uint64_t>(12u, reader.Acquired());
		}
	};

	// D3 / FIX3-D6: zero-wait no-lane P1CpuFill module (RegisterP1CpuModule + NoteP1CpuDemand + OfferP1CpuSource)
	// serving kP1HalfLuma via Acquire on a worker thread with zero render-thread waits and zero private U3 copies.
	TEST_CLASS(P1CpuFillTests)
	{
	public:
		TEST_METHOD(RegisterP1CpuFillServesAcquireWithZeroWaitsZeroPrivateU3AndNegativeControl)
		{
			struct ComputeOnceScope {
				bool prevEnabled = false;
				ComputeOnce::ThreadFrame prevFrame;
				ComputeOnceScope()
					: prevEnabled(ComputeOnce::Enabled())
					, prevFrame(ComputeOnce::Current())
				{
					ComputeOnce::SetEnabled(true);
					ComputeOnce::Global().Reset();
					ComputeOnce::Current() = ComputeOnce::ThreadFrame();
				}
				~ComputeOnceScope()
				{
					ComputeOnce::Global().Reset();
					ComputeOnce::Current() = prevFrame;
					ComputeOnce::SetEnabled(prevEnabled);
				}
			};

			struct MemoBackedDemand : Vpv::Skeleton::IExternalDemand {
				SkeletonMemo* memo = nullptr;
				int registerCalls = 0;
				int unregisterCalls = 0;
				bool lastRenderThreadOnly = true;
				bool lastFrameScoped = true;
				uint16_t lastKeepFrames = 0;
				uint16_t lastAheadDepth = 0;

				void SetExternalDemand(const void*, const void*, const std::vector<ConsumerDecl>&,
					const std::vector<ProductId>&) override {}
				void ClearExternalDemand(const void*, const void*) override {}
				SkeletonMemo::Registration RegisterOwnerFill(ProductId id, AcquireForm form,
					const std::string& unit, OwnerFill fill) override
				{
					++registerCalls;
					lastRenderThreadOnly = fill.renderThreadOnly;
					lastFrameScoped = fill.frameScoped;
					lastKeepFrames = fill.keepFrames;
					lastAheadDepth = fill.aheadDepth;
					return memo ? memo->RegisterOwnerFill(id, form, unit, std::move(fill))
						: SkeletonMemo::Registration::Registered;
				}
				bool UnregisterOwnerFill(ProductId id, AcquireForm form, const std::string& unit) override
				{
					++unregisterCalls;
					return memo ? memo->UnregisterOwnerFill(id, form, unit) : true;
				}
			};

			struct FakeSkeleton : Vpv::Hub::ISkeletonModule {
				MemoBackedDemand demand;
				const char* Name() const override { return "test.p1_cpu_fill.fake_skeleton"; }
				void* Query(const char* id) override
				{
					return id != nullptr && std::strcmp(id, Vpv::Skeleton::kExternalDemandQueryId) == 0
						? static_cast<Vpv::Skeleton::IExternalDemand*>(&demand) : nullptr;
				}
			};

			struct HubRig {
				int host = 0;
				int origHost = 0;
				SkeletonMemo memo;
				MemoBackedDemand* demand = nullptr;

				HubRig()
				{
					memo.SetHost(&host);
					Vpv::Hub::ExtensionHub::Instance().ResetModulesForTesting();
					std::unique_ptr<FakeSkeleton> sk(new FakeSkeleton());
					sk->demand.memo = &memo;
					demand = &sk->demand;
					Vpv::Hub::ExtensionHub::Instance().AddModule(std::move(sk));
					Vpv::FrameAnalysis::RegisterP1CpuModule();
				}
				~HubRig()
				{
					Vpv::Hub::ExtensionHub::Instance().NoteOriginalPathway(&origHost, false);
					demand = nullptr;
					Vpv::Hub::ExtensionHub::Instance().ResetModulesForTesting();
				}
				HubRig(const HubRig&) = delete;
				HubRig& operator=(const HubRig&) = delete;
			};

			ComputeOnceScope co;
			HubRig rig;

			// Activate demand -> registers (kP1HalfLuma, Cpu) with renderThreadOnly=false, keepFrames=2, aheadDepth=kAheadCeiling.
			Vpv::FrameAnalysis::NoteP1CpuDemand(&rig.host, true);
			Assert::IsTrue(rig.demand->registerCalls >= 1, L"NoteP1CpuDemand registered P1CpuFill owner fill");
			Assert::IsFalse(rig.demand->lastRenderThreadOnly, L"zero render-thread wait: renderThreadOnly == false");
			Assert::IsFalse(rig.demand->lastFrameScoped, L"frameScoped == false");
			Assert::AreEqual<uint32_t>(2u, rig.demand->lastKeepFrames, L"keepFrames == 2 for pair-start look-back");
			Assert::AreEqual<uint32_t>(VpSkeleton::kAheadCeiling, rig.demand->lastAheadDepth,
				L"aheadDepth == kAheadCeiling while demand is active");

			const Frame f = Make(Kind::P010, 7);

			// Compute independent mathematical ground truth (ComputeOnce paused so it doesn't add rows).
			std::vector<uint8_t> refEngine8;
			int refW = 0, refH = 0;
			{
				ComputeOnce::SetEnabled(false);
				Assert::IsTrue(MotionAhead::HalfLuma8(f.source, refEngine8, refW, refH));
				ComputeOnce::SetEnabled(true);
			}

			// ---- 1. In-Test Negative Control: omitting OfferP1CpuSource declines cleanly without hanging ----
			{
				VpFrameAnalysis::P1AcquireSource noOfferReader(
					[&rig](uint64_t seq) { return rig.memo.Acquire(kP1HalfLuma, seq, AcquireForm::Cpu); });
				bool ok = true;
				std::vector<uint8_t> got;
				int gw = 0, gh = 0;
				P1ReadKind how = P1ReadKind::LiveFill;
				std::thread worker([&]() {
					ok = noOfferReader.ReadEngine8(5, f.source, got, gw, gh, how);
				});
				worker.join();
				Assert::IsFalse(ok, L"negative control: without OfferP1CpuSource, ReadEngine8 declines cleanly");
				Assert::IsTrue(how == P1ReadKind::None);
				Assert::AreEqual<uint64_t>(1u, noOfferReader.Unread());
				Assert::AreEqual<uint64_t>(1u,
					rig.memo.CountersOf(kP1HalfLuma).unavailable[size_t(AcquireCause::OwnerDeclined)],
					L"negative control: missing source offer recorded visibly as OwnerDeclined");
				const std::string noOfferCause = rig.memo.LastCause(kP1HalfLuma);
				Assert::IsTrue(noOfferCause.find("p1_cpu: no source offered for frame") != std::string::npos,
					L"negative control: missing source reports exact no-source decline reason");
				Assert::IsTrue(noOfferCause.find("p1_cpu: original pathway") == std::string::npos,
					L"negative control: non-original host never reports original pathway decline");
			}

			// ---- 2. Protected Path: OfferP1CpuSource serves Acquire on worker thread with 0 waits & 0 private U3 ----
			{
				ComputeOnce::Global().Reset();
				VpFrameAnalysis::P1AcquireSource reader(
					[&rig](uint64_t seq) { return rig.memo.Acquire(kP1HalfLuma, seq, AcquireForm::Cpu); },
					VpFrameAnalysis::P1AcquireSource::kExactP1TapMsbShift,
					[&rig](uint64_t seq, const AnalysisLumaSource& src) {
						Vpv::FrameAnalysis::OfferP1CpuSource(&rig.host, seq, src);
					});

				bool ok1 = false, ok2 = false;
				std::vector<uint8_t> got1, got2;
				int gw1 = 0, gh1 = 0, gw2 = 0, gh2 = 0;
				P1ReadKind how1 = P1ReadKind::None, how2 = P1ReadKind::None;
				std::thread worker([&]() {
					ok1 = reader.ReadEngine8(6, f.source, got1, gw1, gh1, how1);
					ok2 = reader.ReadEngine8(6, f.source, got2, gw2, gh2, how2);
				});
				worker.join();

				Assert::IsTrue(ok1 && how1 == P1ReadKind::LiveFill, L"worker Acquire filled kP1HalfLuma with zero render wait");
				Assert::IsTrue(ok2 && how2 == P1ReadKind::LiveFill, L"second read of same frame reused memo");
				Assert::AreEqual(refW, gw1);
				Assert::AreEqual(refH, gh1);
				Assert::IsTrue(got1 == refEngine8, L"P1CpuFill output is byte-identical to independent HalfLuma8 ground truth");
				Assert::IsTrue(got2 == refEngine8);
				Assert::AreEqual<uint64_t>(1u, reader.FilledAtRead());
				Assert::AreEqual<uint64_t>(1u, reader.MemoReused());
				Assert::AreEqual<uint64_t>(0u, reader.Unread());

				// ComputeOnce proof: U3 counted ONCE with 0 duplicates, 0 gaps, and 0 private U3 hidden copies.
				uint64_t u3Calls = 0, u3Dups = 0, u3PrivateRows = 0;
				for (const ComputeOnce::UnitRow& row : ComputeOnce::Global().Rows()) {
					if (row.unit == "U3") {
						if (row.variant.compare(0, 8, "private:") == 0)
							++u3PrivateRows;
						else {
							u3Calls += row.calls;
							u3Dups += row.dupFrames;
						}
					}
				}
				Assert::AreEqual<uint64_t>(1u, u3Calls, L"shared U3 owner law executed exactly once for frame 6");
				Assert::AreEqual<uint64_t>(0u, u3Dups, L"zero U3 duplicates");
				Assert::AreEqual<uint64_t>(0u, u3PrivateRows, L"zero private U3 hidden copies");
				Assert::AreEqual<size_t>(0u, ComputeOnce::Global().Gaps().size(),
					L"FrameScope inside P1CpuFill prevents seq < 0 ComputeOnce gaps");
			}

			// ---- 3. Lifecycle & Original-pathway boundaries ----
			// Offer frame 8 with a tracked backingOwner, then fire GpuReleased -> backingOwner released and Acquire declines.
			{
				AnalysisLumaSource tracked = f.source;
				std::shared_ptr<int> sentinel = std::make_shared<int>(99);
				std::weak_ptr<int> weakSentinel = sentinel;
				tracked.backingOwner = sentinel;
				sentinel.reset();
				Vpv::FrameAnalysis::OfferP1CpuSource(&rig.host, 8, tracked);
				tracked.backingOwner.reset();
				Assert::IsFalse(weakSentinel.expired(), L"offer ring holds source backingOwner before GpuReleased");

				RendererExtension::LifecycleFacts lf;
				lf.host = &rig.host;
				lf.kind = RendererExtension::Lifecycle::GpuReleased;
				Vpv::Hub::ExtensionHub::Instance().OnLifecycle(lf);
				Assert::IsTrue(weakSentinel.expired(), L"GpuReleased purged host source ring and released backingOwner");

				const AcquireResult afterRel = rig.memo.Acquire(kP1HalfLuma, 8, AcquireForm::Cpu);
				Assert::IsTrue(afterRel.status == AcquireStatus::Unavailable && afterRel.cause == AcquireCause::OwnerDeclined,
					L"post-GpuReleased Acquire declines cleanly");
			}

			// NoteP1CpuDemand(false) drops aheadDepth back to 0.
			Vpv::FrameAnalysis::NoteP1CpuDemand(&rig.host, false);
			Assert::AreEqual<uint32_t>(0u, rig.demand->lastAheadDepth, L"stopping demand drops aheadDepth to 0");

			// S1-L1: Original pathway host refuses with the explicit "p1_cpu: original pathway" reason (even when a
			// source offer was already present before switching to Original), and ignores new OfferP1CpuSource calls.
			SkeletonMemo origMemo;
			origMemo.SetHost(&rig.origHost);
			rig.demand->memo = &origMemo;
			// OfferP1CpuSource(&rig.origHost, 9, ...) transitions desiredDepth_ 0 -> kAheadCeiling so SyncRegistration
			// installs P1CpuFill on origMemo before NoteOriginalPathway(&rig.origHost, true) is toggled on.
			Vpv::FrameAnalysis::OfferP1CpuSource(&rig.origHost, 9, f.source);
			Assert::IsTrue(origMemo.HasOwnerFill(kP1HalfLuma, AcquireForm::Cpu),
				L"precondition: P1CpuFill owner fill must be registered on origMemo before Original-pathway Acquire");
			Vpv::Hub::ExtensionHub::Instance().NoteOriginalPathway(&rig.origHost, true);
			{
				AnalysisLumaSource origTracked = f.source;
				std::shared_ptr<int> origSentinel = std::make_shared<int>(77);
				std::weak_ptr<int> weakOrig = origSentinel;
				origTracked.backingOwner = origSentinel;
				origSentinel.reset();
				Vpv::FrameAnalysis::OfferP1CpuSource(&rig.origHost, 10, origTracked);
				origTracked.backingOwner.reset();
				Assert::IsTrue(weakOrig.expired(), L"Original pathway OfferP1CpuSource must not retain frame backingOwner");
			}
			const AcquireResult origRes = origMemo.Acquire(kP1HalfLuma, 9, AcquireForm::Cpu);
			rig.demand->memo = &rig.memo;
			Assert::IsTrue(origRes.status == AcquireStatus::Unavailable && origRes.cause == AcquireCause::OwnerDeclined,
				L"Original pathway host declines P1CpuFill cleanly");
			const std::string origCause = origMemo.LastCause(kP1HalfLuma);
			Assert::IsTrue(origCause.find("p1_cpu: original pathway") != std::string::npos,
				L"S1-L1: Original pathway refusal must explicitly report \"p1_cpu: original pathway\"");
		}

		// S1-H1: OfferP1CpuSource must preserve out-of-order (N+1, N) pair offers (matching MotionAheadProducer's
		// (rq.to, rq.from) acquisition order) and multi-frame in-window reorders, while still resetting cleanly on
		// a true large backwards seek (seq + kMaxSourceOffers < back.seq).
		TEST_METHOD(OutOfOrderPairOffersPreserveBothFramesAndLargeBackSeekResets)
		{
			struct RealSkeletonRig {
				int host = 0;
				Vpv::Skeleton::SkeletonModule* sk = nullptr;
				RealSkeletonRig()
				{
					Vpv::Hub::ExtensionHub::Instance().ResetModulesForTesting();
					std::unique_ptr<Vpv::Skeleton::SkeletonModule> mod(
						new Vpv::Skeleton::SkeletonModule([](ConfigFile&) { return false; }));
					sk = mod.get();
					Vpv::Hub::ExtensionHub::Instance().AddModule(std::move(mod));
					Vpv::FrameAnalysis::RegisterP1CpuModule();
				}
				~RealSkeletonRig()
				{
					sk = nullptr;
					Vpv::Hub::ExtensionHub::Instance().ResetModulesForTesting();
				}
			};

			RealSkeletonRig rig;
			Vpv::Skeleton::FrameMemo fm = rig.sk->MemoFor(&rig.host, 20);
			Assert::IsTrue(fm.Valid(), L"real SkeletonModule MemoFor must provide a valid SkeletonMemo");
			Vpv::FrameAnalysis::NoteP1CpuDemand(&rig.host, true);

			// Distinct synthetic frames so any ring wipe or mis-indexing is immediately caught byte-for-byte.
			const Frame f20 = Make(Kind::P010, 2);
			const Frame f21 = Make(Kind::P010, 7);
			const Frame f22 = Make(Kind::P010, 3);
			const Frame f23 = Make(Kind::P010, 4);
			const Frame f24 = Make(Kind::P010, 5);
			const Frame f25 = Make(Kind::P010, 8);

			auto truthOf = [](const Frame& fr) {
				std::vector<uint8_t> want;
				int w = 0, h = 0;
				Assert::IsTrue(MotionAhead::HalfLuma8(fr.source, want, w, h));
				return want;
			};
			const std::vector<uint8_t> want20 = truthOf(f20);
			const std::vector<uint8_t> want21 = truthOf(f21);
			const std::vector<uint8_t> want22 = truthOf(f22);
			const std::vector<uint8_t> want23 = truthOf(f23);
			const std::vector<uint8_t> want24 = truthOf(f24);
			const std::vector<uint8_t> want25 = truthOf(f25);
			Assert::IsTrue(want20 != want21, L"precondition: frames 20 and 21 must have distinct luma pixels");

			// 1. Out-of-order (N+1, N) pair offers (exact MotionAheadProducer (rq.to, rq.from) order):
			// Pre-fix P1CpuFill.cpp:127 wiped the entire ring when seq 20 < ring.back().seq (21), losing 21!
			Vpv::FrameAnalysis::OfferP1CpuSource(&rig.host, 21, f21.source);
			Vpv::FrameAnalysis::OfferP1CpuSource(&rig.host, 20, f20.source);

			// Read without re-offering so BOTH 20 and 21 must still be present in P1CpuFill's source ring.
			VpFrameAnalysis::P1AcquireSource reader(
				[&fm](uint64_t seq) { return fm.memo->Acquire(kP1HalfLuma, seq, AcquireForm::Cpu); });
			auto readAndCheck = [&](uint64_t seq, const Frame& fr, const std::vector<uint8_t>& want, const wchar_t* msg) {
				std::vector<uint8_t> got;
				int gw = 0, gh = 0;
				P1ReadKind how = P1ReadKind::None;
				Assert::IsTrue(reader.ReadEngine8(seq, fr.source, got, gw, gh, how), msg);
				Assert::IsTrue(how == P1ReadKind::LiveFill, msg);
				Assert::IsTrue(got == want, msg);
			};
			readAndCheck(20, f20, want20, L"S1-H1: frame N (20) must be acquirable after out-of-order (21, 20) offer");
			readAndCheck(21, f21, want21, L"S1-H1: frame N+1 (21) must NOT be wiped when earlier frame N (20) is offered");

			// 2. Multi-frame scrambled offers within kMaxSourceOffers (kAheadCeiling + 2): all 4 survive in order.
			Vpv::FrameAnalysis::OfferP1CpuSource(&rig.host, 25, f25.source);
			Vpv::FrameAnalysis::OfferP1CpuSource(&rig.host, 23, f23.source);
			Vpv::FrameAnalysis::OfferP1CpuSource(&rig.host, 24, f24.source);
			Vpv::FrameAnalysis::OfferP1CpuSource(&rig.host, 22, f22.source);
			readAndCheck(22, f22, want22, L"S1-H1: scrambled in-window frame 22 preserved");
			readAndCheck(23, f23, want23, L"S1-H1: scrambled in-window frame 23 preserved");
			readAndCheck(24, f24, want24, L"S1-H1: scrambled in-window frame 24 preserved");
			readAndCheck(25, f25, want25, L"S1-H1: scrambled in-window frame 25 preserved");

			// 3. True large backwards seek (seq + kMaxSourceOffers < back.seq): stale ring MUST reset and release backingOwner.
			const uint64_t backSeq = static_cast<uint64_t>(VpSkeleton::kAheadCeiling) + 100u;
			const uint64_t seekSeq = 1u;   // seekSeq + (kAheadCeiling + 2) < backSeq
			AnalysisLumaSource trackedBack = f21.source;
			std::shared_ptr<int> sentinel = std::make_shared<int>(42);
			std::weak_ptr<int> weakSentinel = sentinel;
			trackedBack.backingOwner = sentinel;
			sentinel.reset();
			Vpv::FrameAnalysis::OfferP1CpuSource(&rig.host, backSeq, trackedBack);
			trackedBack.backingOwner.reset();
			Assert::IsFalse(weakSentinel.expired(), L"precondition: backSeq offer holds backingOwner before seek");

			Vpv::FrameAnalysis::OfferP1CpuSource(&rig.host, seekSeq, f20.source);
			Assert::IsTrue(weakSentinel.expired(), L"S1-H1: large backwards seek must purge stale pre-seek offers");
			const AcquireResult staleRes = fm.memo->Acquire(kP1HalfLuma, backSeq, AcquireForm::Cpu);
			Assert::IsTrue(staleRes.status == AcquireStatus::Unavailable && staleRes.cause == AcquireCause::OwnerDeclined,
				L"S1-H1: purged pre-seek frame declines cleanly");
			readAndCheck(seekSeq, f20, want20, L"S1-H1: post-seek frame 1 is acquirable cleanly");
		}

		// S1-H2: Concurrent NoteP1CpuDemand / SyncRegistration depth transitions (0 <-> kAheadCeiling) through the
		// real SkeletonModule must NEVER leave a window where Acquire(kP1HalfLuma, Cpu) sees AcquireCause::NoOwnerFill
		// or HasOwnerFill() drops to false.
		TEST_METHOD(ConcurrentDepthTransitionsHaveZeroNoOwnerFillRace)
		{
			// In-test negative control: prove an unregistered memo genuinely produces AcquireCause::NoOwnerFill.
			{
				SkeletonMemo emptyMemo;
				const AcquireResult neg = emptyMemo.Acquire(kP1HalfLuma, 1, AcquireForm::Cpu);
				Assert::IsTrue(neg.status == AcquireStatus::Unavailable && neg.cause == AcquireCause::NoOwnerFill,
					L"negative control: absent owner fill must report AcquireCause::NoOwnerFill");
				Assert::IsFalse(emptyMemo.HasOwnerFill(kP1HalfLuma, AcquireForm::Cpu));
			}

			struct RealSkeletonRig {
				int host = 0;
				Vpv::Skeleton::SkeletonModule* sk = nullptr;
				RealSkeletonRig()
				{
					Vpv::Hub::ExtensionHub::Instance().ResetModulesForTesting();
					std::unique_ptr<Vpv::Skeleton::SkeletonModule> mod(
						new Vpv::Skeleton::SkeletonModule([](ConfigFile&) { return false; }));
					sk = mod.get();
					Vpv::Hub::ExtensionHub::Instance().AddModule(std::move(mod));
					Vpv::FrameAnalysis::RegisterP1CpuModule();
				}
				~RealSkeletonRig()
				{
					sk = nullptr;
					Vpv::Hub::ExtensionHub::Instance().ResetModulesForTesting();
				}
			};

			RealSkeletonRig rig;
			Vpv::Skeleton::FrameMemo fm = rig.sk->MemoFor(&rig.host, 1);
			Assert::IsTrue(fm.Valid());
			// Do NOT call OfferP1CpuSource(&rig.host, ...) here: an active source offer keeps HostEntry::Active()
			// permanently true (entrances - lastOfferAt <= kAheadCeiling), which would prevent NoteP1CpuDemand(false)
			// from dropping desiredDepth_ to 0. With offered == false, every false/true toggle genuinely transitions
			// wantDepth between kAheadCeiling and 0 through SyncRegistration on the real SkeletonModule.
			Vpv::FrameAnalysis::NoteP1CpuDemand(&rig.host, true);
			Assert::IsTrue(fm.memo->HasOwnerFill(kP1HalfLuma, AcquireForm::Cpu),
				L"precondition: P1CpuFill registered before concurrent transition test");
			const AcquireResult baseline = fm.memo->Acquire(kP1HalfLuma, 1, AcquireForm::Cpu);
			Assert::IsTrue(baseline.status == AcquireStatus::Unavailable && baseline.cause == AcquireCause::OwnerDeclined,
				L"precondition: registered P1CpuFill runs and declines missing frame with OwnerDeclined (not NoOwnerFill)");

			std::atomic<int> ready{ 0 };
			std::atomic<bool> stop{ false };
			std::atomic<uint64_t> noOwnerFillHits{ 0 };
			std::atomic<uint64_t> missingOwnerProbes{ 0 };
			std::atomic<uint64_t> totalProbes{ 0 };
			std::atomic<uint64_t> totalAcquires{ 0 };

			auto readerLoop = [&](uint64_t seqBase) {
				uint64_t seq = seqBase;
				uint64_t acquires = 0;
				ready.fetch_add(1, std::memory_order_release);
				while (!stop.load(std::memory_order_acquire))
				{
					if (!fm.memo->HasOwnerFill(kP1HalfLuma, AcquireForm::Cpu))
						missingOwnerProbes.fetch_add(1, std::memory_order_relaxed);
					totalProbes.fetch_add(1, std::memory_order_relaxed);
					if (acquires < 8000u)
					{
						const AcquireResult r = fm.memo->Acquire(kP1HalfLuma, seq++, AcquireForm::Cpu);
						if (r.status == AcquireStatus::Unavailable && r.cause == AcquireCause::NoOwnerFill)
							noOwnerFillHits.fetch_add(1, std::memory_order_relaxed);
						++acquires;
						totalAcquires.fetch_add(1, std::memory_order_relaxed);
					}
				}
			};

			std::thread readerA(readerLoop, 1000u);
			std::thread readerB(readerLoop, 1000000u);
			while (ready.load(std::memory_order_acquire) < 2)
				std::this_thread::yield();

			// Hammer 3,000 real depth transitions (kAheadCeiling -> 0 -> kAheadCeiling) while both readers probe continuously.
			for (int i = 0; i < 1500; ++i)
			{
				Vpv::FrameAnalysis::NoteP1CpuDemand(&rig.host, false);
				Vpv::FrameAnalysis::NoteP1CpuDemand(&rig.host, true);
			}

			stop.store(true, std::memory_order_release);
			readerA.join();
			readerB.join();

			Assert::IsTrue(totalProbes.load() > 0u && totalAcquires.load() > 0u,
				L"readers must execute concurrently during depth transitions");
			Assert::AreEqual<uint64_t>(0u, noOwnerFillHits.load(),
				L"S1-H2: concurrent depth transitions must produce ZERO AcquireCause::NoOwnerFill gaps");
			Assert::AreEqual<uint64_t>(0u,
				fm.memo->CountersOf(kP1HalfLuma).unavailable[size_t(AcquireCause::NoOwnerFill)],
				L"S1-H2: SkeletonMemo NoOwnerFill counter must remain 0 across all transitions");
			Assert::AreEqual<uint64_t>(0u, missingOwnerProbes.load(),
				L"S1-H2: HasOwnerFill(kP1HalfLuma, Cpu) must NEVER drop to false during atomic ReplaceOwnerFill");
		}

		// S1-M3: Concurrent OfferP1CpuSource / NoteP1CpuDemand across GpuReleased / Shutdown lifecycle teardowns
		// and post-module-destroy calls must stay crash-free and leak-free.
		TEST_METHOD(ConcurrentLifecycleTeardownAndPostDestroyCallsAreSafe)
		{
			struct HubResetGuard
			{
				~HubResetGuard()
				{
					Vpv::Hub::ExtensionHub::Instance().ResetModulesForTesting();
				}
			} hubGuard;
			Vpv::Hub::ExtensionHub::Instance().ResetModulesForTesting();
			{
				std::unique_ptr<Vpv::Skeleton::SkeletonModule> mod(
					new Vpv::Skeleton::SkeletonModule([](ConfigFile&) { return false; }));
				Vpv::Hub::ExtensionHub::Instance().AddModule(std::move(mod));
				Vpv::FrameAnalysis::RegisterP1CpuModule();
			}

			int host = 0;
			const Frame f = Make(Kind::P010, 3);
			std::atomic<int> ready{ 0 };
			std::atomic<bool> stop{ false };
			std::atomic<uint64_t> offers{ 0 };
			std::atomic<uint64_t> demands{ 0 };

			struct ThreadJoinGuard
			{
				std::atomic<bool>& stopFlag;
				std::thread offerWorker;
				std::thread demandWorker;
				explicit ThreadJoinGuard(std::atomic<bool>& s) : stopFlag(s) {}
				~ThreadJoinGuard()
				{
					stopFlag.store(true, std::memory_order_release);
					if (offerWorker.joinable())
						offerWorker.join();
					if (demandWorker.joinable())
						demandWorker.join();
				}
			} workers(stop);

			workers.offerWorker = std::thread([&]() {
				uint64_t seq = 1;
				ready.fetch_add(1, std::memory_order_release);
				while (!stop.load(std::memory_order_acquire))
				{
					Vpv::FrameAnalysis::OfferP1CpuSource(&host, seq++, f.source);
					offers.fetch_add(1, std::memory_order_relaxed);
				}
			});
			workers.demandWorker = std::thread([&]() {
				bool active = true;
				ready.fetch_add(1, std::memory_order_release);
				while (!stop.load(std::memory_order_acquire))
				{
					Vpv::FrameAnalysis::NoteP1CpuDemand(&host, active);
					active = !active;
					demands.fetch_add(1, std::memory_order_relaxed);
				}
			});
			while (ready.load(std::memory_order_acquire) < 2)
				std::this_thread::yield();

			for (int i = 0; i < 200; ++i)
			{
				RendererExtension::LifecycleFacts lf;
				lf.host = &host;
				lf.kind = (i & 1) == 0 ? RendererExtension::Lifecycle::GpuReleased
					: RendererExtension::Lifecycle::Shutdown;
				Vpv::Hub::ExtensionHub::Instance().OnLifecycle(lf);
			}

			stop.store(true, std::memory_order_release);
			if (workers.offerWorker.joinable())
				workers.offerWorker.join();
			if (workers.demandWorker.joinable())
				workers.demandWorker.join();

			Assert::IsTrue(offers.load() > 0u && demands.load() > 0u,
				L"workers must exercise OfferP1CpuSource and NoteP1CpuDemand concurrently with teardown");

			// Destroy P1CpuModule and verify post-teardown calls are safe no-ops and do not retain backingOwner.
			Vpv::Hub::ExtensionHub::Instance().ResetModulesForTesting();
			Vpv::FrameAnalysis::NoteP1CpuDemand(&host, true);
			AnalysisLumaSource afterDestroy = f.source;
			std::shared_ptr<int> sentinel = std::make_shared<int>(55);
			std::weak_ptr<int> weakSentinel = sentinel;
			afterDestroy.backingOwner = sentinel;
			sentinel.reset();
			Vpv::FrameAnalysis::OfferP1CpuSource(&host, 999, afterDestroy);
			afterDestroy.backingOwner.reset();
			Assert::IsTrue(weakSentinel.expired(),
				L"S1-M3: post-destroy OfferP1CpuSource must be a clean no-op that releases backingOwner");
		}
	};
}
