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

	bool Identical(Kind k)
	{
		return k == Kind::P010 || k == Kind::P210 || k == Kind::DecodedDv422Full || k == Kind::V210 || k == Kind::Uyvy ||
			k == Kind::SdrTunnel || k == Kind::PackedDvR210;
	}

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
				if (Identical(kind))
				{
					Assert::IsTrue(read && how != P1ReadKind::None, KindName(kind));
					Assert::IsTrue(MotionAhead::HalfLuma8(f.source, want, ww, wh), KindName(kind));
					Assert::IsTrue(gw == ww && gh == wh && got == want, KindName(kind));
					Assert::AreEqual<uint64_t>(1u, reader.Acquired(), KindName(kind));
				}
				else
				{
					Assert::IsFalse(read, KindName(kind));
					Assert::IsTrue(how == P1ReadKind::None, KindName(kind));
					Assert::AreEqual<uint64_t>(1u, reader.OwnDecode(), KindName(kind));
					Assert::AreEqual(0, owner.fills.load(), L"a refused format never builds P1 for the producer");
				}
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

			// Original pathway host: OfferP1CpuSource ignored and owner fill visibly refuses.
			Vpv::Hub::ExtensionHub::Instance().NoteOriginalPathway(&rig.origHost, true);
			Vpv::FrameAnalysis::OfferP1CpuSource(&rig.origHost, 9, f.source);
			SkeletonMemo origMemo;
			origMemo.SetHost(&rig.origHost);
			rig.demand->memo = &origMemo;
			Vpv::FrameAnalysis::NoteP1CpuDemand(&rig.host, true);
			const AcquireResult origRes = origMemo.Acquire(kP1HalfLuma, 9, AcquireForm::Cpu);
			rig.demand->memo = &rig.memo;
			Assert::IsTrue(origRes.status == AcquireStatus::Unavailable && origRes.cause == AcquireCause::OwnerDeclined,
				L"Original pathway host declines P1CpuFill cleanly");
		}
	};
}
