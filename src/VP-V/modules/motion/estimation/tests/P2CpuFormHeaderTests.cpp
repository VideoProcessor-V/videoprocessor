// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include <VP-V/modules/motion/estimation/P2CpuForm.h>   // FIRST include on purpose: the header must stand alone (r1 HIGH-1)
//
// B-prereq (a) A3: the P2 CPU form's owner fill (P2CpuForm.h) against FAKE engines, through a real SkeletonMemo. Built without the
// precompiled header and with P2CpuForm.h as the first include, so a missing include in its chain (the forward-declared
// MotionAhead::Flow of r1) fails here. Pins: the fill's contract (owner unit, default variant only, any thread, keepFrames 1);
// the default IVectorEngine (a fake, an older engine) declines "no pair store"; engine absent declines and counts nothing (also
// through the hub's directory when no engine is registered); served / declined counted once per fill; an off-render-thread
// Acquire is served; a lag-1 reader of frame n-1 is served after seal n and n-2 is FrameReleased; the module registers only
// with a skeleton, once, counts AlreadyRegistered as registered and registers nothing on the Original pathway.
// CPU only; no GPU object, no producer, no matcher.

#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "CppUnitTest.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	namespace
	{
		// Every pure virtual answered "absent"; the B-prereq (a) virtuals keep their defaults.
		struct NullEngine : VpMotion::IVectorEngine
		{
			uint64_t LatestVerifiedFieldSeq() const override { return 0; }
			uint64_t FieldEra() const override { return 1; }
			bool FieldOf(uint64_t, VpMotion::VerifiedField&) const override { return false; }
			float FieldScale() const override { return 1; }
			bool GlobalFromEngine() const override { return false; }
			bool EngineGlobal(uint64_t, VpMotion::GlobalVec&) const override { return false; }
			bool PairSpansSlip(uint64_t, uint64_t) const override { return false; }
			bool FlowIsCpuResident() const override { return false; }
			bool FlowOf(uint64_t, VpMotion::FlowView&) const override { return false; }
			void SetPreEstimateHook(VpMotion::IPreEstimateHook*) override {}
		};

		// Counts NoteP2CpuFormRead; P2RecordEndingAt stays the interface default ("no pair store").
		struct CountingEngine : NullEngine
		{
			mutable int served = 0;
			mutable int declined = 0;
			void NoteP2CpuFormRead(bool ok) const override { (ok ? served : declined) += 1; }
		};

		// Serves the pair (to - 1, to) for lo <= to <= hi, generation 3, a global derived from `to`.
		struct ServingEngine : CountingEngine
		{
			uint64_t lo = 0, hi = 0;
			bool P2RecordEndingAt(uint64_t to, VpMotion::P2PairRecord& out, std::string& why) const override
			{
				if (to < lo || to > hi)
				{
					why = "p2 cpu form: no pair ahead for frame " + std::to_string(to) + " (generation 3)";
					return false;
				}
				float g[2][4] = { { float(to), -1.0f, 0.5f, 2.0f }, { -float(to), 1.0f, 0.25f, 3.0f } };
				float s[2][4] = { { 0.125f, 0.0f, 0.0f, 1.0f }, { -0.125f, 0.0f, 0.0f, 1.0f } };
				out = VpMotion::MakeP2PairRecord(to - 1u, to, 3u, g, s, true, true);
				return true;
			}
		};

		VpMotion::P2EngineOf EngineIs(VpMotion::IVectorEngine* e)
		{
			return [e](const void*) { return e; };
		}

		VpSkeleton::OwnerFill FillFor(VpMotion::IVectorEngine* e)
		{
			return VpMotion::MakeP2CpuOwnerFill(EngineIs(e));
		}

		bool Register(VpSkeleton::SkeletonMemo& m, VpSkeleton::OwnerFill f)
		{
			return m.RegisterOwnerFill(VpSkeleton::kP2VectorPair, VpSkeleton::AcquireForm::Cpu, VpMotion::kP2CpuFormUnit, f)
				== VpSkeleton::SkeletonMemo::Registration::Registered;
		}

		bool Has(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

		// Forwards the module's registrations to one real memo (the catalog's ownership rules apply) and counts them.
		struct MemoDemand : Vpv::Skeleton::IExternalDemand
		{
			VpSkeleton::SkeletonMemo* memo = nullptr;
			int registers = 0;
			void SetExternalDemand(const void*, const void*, const std::vector<VpSkeleton::ConsumerDecl>&,
				const std::vector<VpSkeleton::ProductId>&) override {}
			void ClearExternalDemand(const void*, const void*) override {}
			VpSkeleton::SkeletonMemo::Registration RegisterOwnerFill(VpSkeleton::ProductId id, VpSkeleton::AcquireForm form,
				const std::string& unit, VpSkeleton::OwnerFill fill) override
			{
				++registers;
				return memo->RegisterOwnerFill(id, form, unit, fill);
			}
		};

		// A hub module standing in for the skeleton: answers the external-demand query.
		struct DemandModule : Vpv::Hub::ISkeletonModule
		{
			Vpv::Skeleton::IExternalDemand* demand = nullptr;
			explicit DemandModule(Vpv::Skeleton::IExternalDemand* d) : demand(d) {}
			const char* Name() const override { return "test.p2_cpu_form.demand"; }
			void* Query(const char* iid) override
			{
				return std::string(iid) == Vpv::Skeleton::kExternalDemandQueryId ? static_cast<void*>(demand) : nullptr;
			}
		};

		// The hub emptied around each hub test (no skeleton, no engine directory, no pathway facts).
		struct HubScope
		{
			HubScope() { Vpv::Hub::ExtensionHub::Instance().ResetModulesForTesting(); }
			~HubScope() { Vpv::Hub::ExtensionHub::Instance().ResetModulesForTesting(); }
		};
	}

	TEST_CLASS(P2CpuFormHeaderTests)
	{
	public:
		TEST_METHOD(FillContractAndCatalogOwner)
		{
			using namespace VpSkeleton;
			const OwnerFill f = VpMotion::MakeP2CpuOwnerFill();
			Assert::IsTrue(static_cast<bool>(f.fill), L"a fill");
			Assert::AreEqual(uint32_t(0), f.variantAxes, L"default variant only");
			Assert::IsFalse(f.renderThreadOnly, L"any thread (OD:657 (a), OD:670 (b)): the slot has its own mutex");
			Assert::AreEqual(int(1), int(f.keepFrames), L"a lag-1 reader (C8-10 / camera) trails the render frame by one");
			Assert::IsFalse(f.frameScoped, L"not a one-frame view");
			Assert::IsFalse(f.view, L"a CPU form, not a GPU view");
			Assert::AreEqual(std::string(VpMotion::kP2CpuFormUnit), SkeletonMemo::OwnerUnitOf(kP2VectorPair), L"the catalog's P2 owner");
			SkeletonMemo m;
			Assert::IsTrue(Register(m, f), L"registered by its unit");
			Assert::IsTrue(m.HasOwnerFill(kP2VectorPair, AcquireForm::Cpu), L"(P2, Cpu) held");
			Assert::IsFalse(m.HasOwnerFill(kP2VectorPair, AcquireForm::Gpu), L"the GPU form is not this fill's");
			Assert::IsTrue(m.RegisterOwnerFill(kP2VectorPair, AcquireForm::Cpu, "input/capture (queue + early upload)", f)
				== SkeletonMemo::Registration::NotOwner, L"another unit is refused");
		}

		TEST_METHOD(TheDefaultEngineDeclinesNoPairStoreAndCountsIt)
		{
			using namespace VpSkeleton;
			CountingEngine engine;   // P2RecordEndingAt / P2Pairs: the interface defaults (a fake, an older engine)
			Assert::IsFalse(engine.P2Pairs().Bound(), L"the default view is unbound");
			VpMotion::P2PairRecord rec;
			std::string why;
			Assert::IsFalse(engine.P2RecordEndingAt(11, rec, why), L"the default reader finds nothing");
			Assert::AreEqual(std::string("p2 cpu form: no pair store"), why, L"worded");

			SkeletonMemo m;
			Assert::IsTrue(Register(m, FillFor(&engine)));
			const AcquireResult r = m.Acquire(kP2VectorPair, 11, AcquireForm::Cpu);
			Assert::IsTrue(r.status == AcquireStatus::Unavailable, L"declined");
			Assert::IsTrue(r.cause == AcquireCause::OwnerDeclined, L"an owner decline, not NoOwnerFill");
			Assert::IsTrue(Has(m.LastCause(kP2VectorPair), "(p2 cpu form: no pair store)"), L"the reason rides the cause");
			Assert::AreEqual(1, engine.declined, L"counted declined");
			Assert::AreEqual(0, engine.served);
		}

		TEST_METHOD(EngineAbsentDeclinesAndCountsNothing)
		{
			using namespace VpSkeleton;
			CountingEngine bystander;
			SkeletonMemo m;
			Assert::IsTrue(Register(m, FillFor(nullptr)));
			const AcquireResult r = m.Acquire(kP2VectorPair, 5, AcquireForm::Cpu);
			Assert::IsTrue(r.cause == AcquireCause::OwnerDeclined, L"declined");
			Assert::IsTrue(Has(m.LastCause(kP2VectorPair), "(p2 cpu form: engine absent)"), L"worded");
			Assert::AreEqual(0, bystander.served + bystander.declined, L"no engine: nothing counted anywhere");

			// The default lookup (the hub's kVectorEngineQueryId directory) with no engine registered: the same decline.
			HubScope hub;
			int host = 0;
			const OwnerFill f = VpMotion::MakeP2CpuOwnerFill();
			OwnerFillRequest req;
			req.host = &host;
			req.id = kP2VectorPair;
			req.seq = 5;
			req.form = AcquireForm::Cpu;
			OwnerFillOutput out;
			SkeletonMemo m2;
			Assert::IsFalse(f.fill(req, m2, out), L"no directory: declined");
			Assert::AreEqual(std::string("p2 cpu form: engine absent"), out.declineReason);
			Assert::IsTrue(out.bytes.empty(), L"nothing published");
		}

		TEST_METHOD(ServedAndDeclinedAreCountedOncePerFill)
		{
			using namespace VpSkeleton;
			ServingEngine engine;
			engine.lo = 10;
			engine.hi = 11;
			SkeletonMemo m;
			Assert::IsTrue(Register(m, FillFor(&engine)));
			const AcquireResult hit = m.Acquire(kP2VectorPair, 11, AcquireForm::Cpu);
			Assert::IsTrue(hit.status == AcquireStatus::FilledAtRead, L"filled at read");
			Assert::AreEqual(VpMotion::kP2PairRecordBytes, hit.cpuBytes, L"one record");
			VpMotion::P2PairRecord rec;
			Assert::IsTrue(VpMotion::DecodeP2PairRecord(hit.cpu, hit.cpuBytes, rec), L"the ONE codec decodes it");
			Assert::AreEqual(uint64_t(10), rec.from);
			Assert::AreEqual(uint64_t(11), rec.to);
			Assert::AreEqual(uint64_t(3), rec.generation);
			Assert::AreEqual(11.0f, rec.global[0][0], L"the engine's record, unchanged");
			Assert::IsTrue(rec.GlobalSet() && rec.GlobalGeneral());
			Assert::AreEqual(1, engine.served);

			const AcquireResult again = m.Acquire(kP2VectorPair, 11, AcquireForm::Cpu);
			Assert::IsTrue(again.status == AcquireStatus::Reused, L"the memo spot: computed once");
			Assert::AreEqual(1, engine.served, L"a reuse is not a second fill");

			const AcquireResult miss = m.Acquire(kP2VectorPair, 12, AcquireForm::Cpu);
			Assert::IsTrue(miss.cause == AcquireCause::OwnerDeclined, L"no pair ending at 12");
			Assert::IsTrue(Has(m.LastCause(kP2VectorPair), "(p2 cpu form: no pair ahead for frame 12"), L"the engine's reason");
			Assert::AreEqual(1, engine.declined);
			m.Acquire(kP2VectorPair, 12, AcquireForm::Cpu);
			Assert::AreEqual(1, engine.declined, L"a declined frame is not asked twice");
		}

		TEST_METHOD(AnOffRenderThreadAcquireIsServed)
		{
			using namespace VpSkeleton;
			ServingEngine engine;
			engine.lo = 1;
			engine.hi = 100;
			int host = 0;
			SkeletonMemo m;
			m.SetHost(&host);
			m.SetRenderThread(std::this_thread::get_id());
			Assert::IsTrue(Register(m, FillFor(&engine)));
			AcquireStatus status = AcquireStatus::Unavailable;
			AcquireCause cause = AcquireCause::CauseCount;
			uint64_t to = 0;
			std::thread worker([&]() {
				const AcquireResult r = m.Acquire(kP2VectorPair, 40, AcquireForm::Cpu);
				status = r.status;
				cause = r.cause;
				VpMotion::P2PairRecord rec;
				if (r.Ok() && VpMotion::DecodeP2PairRecord(r.cpu, r.cpuBytes, rec))
					to = rec.to;
			});
			worker.join();
			Assert::IsTrue(status == AcquireStatus::FilledAtRead, L"served off the render thread (renderThreadOnly false)");
			Assert::IsTrue(cause == AcquireCause::None, L"not WrongThread");
			Assert::AreEqual(uint64_t(40), to);
			Assert::AreEqual(1, engine.served);
		}

		TEST_METHOD(ALagOneReaderIsServedAndTwoBackIsReleased)
		{
			using namespace VpSkeleton;
			ServingEngine engine;
			engine.lo = 1;
			engine.hi = 100;
			SkeletonMemo m;
			Assert::IsTrue(Register(m, FillFor(&engine)));
			Assert::IsTrue(m.Acquire(kP2VectorPair, 9, AcquireForm::Cpu).Ok(), L"frame 9 read while it renders");
			m.OnSeal(9);
			m.OnSeal(10);   // frame 10 rendered with no P2 CPU reader
			m.OnSeal(11);   // frame 11 renders: a lag-1 worker now asks for 10
			const AcquireResult lag1 = m.Acquire(kP2VectorPair, 10, AcquireForm::Cpu);
			Assert::IsTrue(lag1.status == AcquireStatus::FilledAtRead, L"n-1 inside keepFrames 1: filled once at read");
			const AcquireResult lag2 = m.Acquire(kP2VectorPair, 9, AcquireForm::Cpu);
			Assert::IsTrue(lag2.cause == AcquireCause::FrameReleased, L"n-2: released, never recomputed");

			// The same sequence with keepFrames 0 (the plan's first value) would refuse the lag-1 read: why the fill keeps 1.
			ServingEngine other;
			other.lo = 1;
			other.hi = 100;
			OwnerFill zero = FillFor(&other);
			zero.keepFrames = 0;
			SkeletonMemo m0;
			Assert::IsTrue(Register(m0, zero));
			Assert::IsTrue(m0.Acquire(kP2VectorPair, 9, AcquireForm::Cpu).Ok());
			m0.OnSeal(9);
			m0.OnSeal(10);
			m0.OnSeal(11);
			Assert::IsTrue(m0.Acquire(kP2VectorPair, 10, AcquireForm::Cpu).cause == AcquireCause::FrameReleased,
				L"keepFrames 0: the lag-1 frame is already released");
		}

		TEST_METHOD(ModuleRegistersOnlyWithASkeletonAndOnce)
		{
			using namespace VpSkeleton;
			SkeletonMemo m;
			MemoDemand demand;
			demand.memo = &m;
			HubScope hub;   // destroyed first: the hub forgets the demand module before `demand` goes

			VpMotion::P2CpuFormModule module;
			Assert::IsFalse(module.TryRegister(), L"no skeleton in the hub yet: retried later");
			Assert::IsFalse(module.Registered());
			Assert::AreEqual(0, demand.registers);

			Vpv::Hub::ExtensionHub::Instance().AddModule(std::unique_ptr<Vpv::Hub::ISkeletonModule>(new DemandModule(&demand)));
			Assert::IsTrue(module.TryRegister(), L"the skeleton is there now");
			Assert::IsTrue(module.Registered());
			Assert::AreEqual(1, demand.registers);
			Assert::IsTrue(m.HasOwnerFill(kP2VectorPair, AcquireForm::Cpu), L"installed in the memo");
			Assert::IsTrue(module.TryRegister(), L"already registered");
			Assert::AreEqual(1, demand.registers, L"registered once: one atomic load afterwards");

			VpMotion::P2CpuFormModule second;
			Assert::IsTrue(second.TryRegister(&demand), L"AlreadyRegistered counts as registered");
			Assert::AreEqual(2, demand.registers, L"asked once");
			Assert::IsTrue(second.Registered());
		}

		TEST_METHOD(ModuleRegistersNothingOnTheOriginalPathwayOrOutsideEntrance)
		{
			using namespace VpSkeleton;
			SkeletonMemo m;
			MemoDemand demand;
			demand.memo = &m;
			HubScope hub;
			Vpv::Hub::ExtensionHub::Instance().AddModule(std::unique_ptr<Vpv::Hub::ISkeletonModule>(new DemandModule(&demand)));
			int original = 0, vpv = 0;
			Assert::IsTrue(Vpv::Hub::ExtensionHub::Instance().NoteOriginalPathway(&original, true), L"Original noted");

			VpMotion::P2CpuFormModule module;
			RendererExtension::FrameContext c;
			c.host = &original;
			c.phase = RendererExtension::FramePhase::Entrance;
			Assert::IsFalse(module.OnFrame(c), L"never takes the frame");
			Assert::IsFalse(module.Registered(), L"Original == Bill: nothing registered");
			Assert::AreEqual(0, demand.registers);

			c.host = &vpv;
			c.phase = RendererExtension::FramePhase::Begin;
			Assert::IsFalse(module.OnFrame(c));
			Assert::AreEqual(0, demand.registers, L"only at Entrance");

			c.phase = RendererExtension::FramePhase::Entrance;
			Assert::IsFalse(module.OnFrame(c), L"never takes the frame");
			Assert::IsTrue(module.Registered(), L"registered at the first VP-V Entrance");
			Assert::AreEqual(1, demand.registers);
			module.OnFrame(c);
			Assert::AreEqual(1, demand.registers, L"not again");
		}
	};
}
