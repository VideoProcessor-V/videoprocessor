// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// S-3a S1 (lead9/brief-S-3a-scheduler.md s4 S1): job registration in graph order (core/JobRegistry.h), the P3 matcher top-up as a Core slot
// job (motion/estimation/LaneAheadSpareWork.h) with its pairs' source planes Acquired from P13, and the Z1 chain ring / Z3 feed
// (motion/estimation/LaneAheadPolicy.h). Pure CPU: the engine and the Acquire path are fakes.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

#include <VP-V/core/JobRegistry.h>
#include <VP-V/core/SchedulerContract.h>
#include <VP-V/modules/control/skeleton/SlotBudgetFeed.h>
#include <VP-V/modules/motion/estimation/LaneAheadSpareWork.h>

#include <map>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace VpSkeleton;
using namespace VpMotion::LaneAhead;

namespace SchedulerS1Test
{
	namespace
	{
		constexpr ProductId kFakeUnpack = 200;   // a test-only product for the Unpack class path (no production Unpack job, Q1)

		Node NodeOf(ProductId id, std::vector<ProductId> lag0Inputs = {})
		{
			Node n;
			n.id = id;
			n.declared = id;
			for (ProductId in : lag0Inputs)
			{
				InputRef r;
				r.product = in;
				r.lag = 0;
				n.inputs[n.inputCount++] = r;
			}
			return n;
		}

		// P1, P2, P3 (reads P2), P3r (reads P3), P13, fake unpack.
		Graph SmallGraph()
		{
			Graph g;
			g.valid = true;
			g.nodes.push_back(NodeOf(kP1HalfLuma));
			g.nodes.push_back(NodeOf(kP2VectorPair));
			g.nodes.push_back(NodeOf(kP3AnalysisField, { kP2VectorPair }));
			g.nodes.push_back(NodeOf(kRefinedField, { kP3AnalysisField }));
			g.nodes.push_back(NodeOf(kP13SourceRing));
			g.nodes.push_back(NodeOf(kFakeUnpack));
			return g;
		}

		JobDecl Decl(const Graph& g, ProductId id, JobClass cls, bool cpu = false)
		{
			JobDecl d;
			d.product = id;
			d.cls = cls;
			d.graphOrder = Jobs::GraphOrderOf(g, id);
			d.splittable = false;
			d.cpu = cpu;
			return d;
		}

		// The engine fake: frames 1..lastQueued are queued, the render frame is `render`. Every estimate is recorded per `to` frame.
		class FakeEngine : public ILaneAheadEngine
		{
		public:
			Input in;
			uint64_t render = 1;
			uint64_t lastQueued = 4;
			uint64_t epoch = 7;
			double pairMs = 1.0;
			bool fail = false;
			std::map<uint64_t, int> estimatesByTo;
			std::vector<std::pair<uint64_t, uint64_t>> planesSeen;   // (from seq, to seq) of the Acquired planes

			FakeEngine()
			{
				in.key = SpareKey::On;
				in.matcherActive = true;
			}
			Input Facts() const override
			{
				Input f = in;
				const uint64_t next = render + 1;
				f.queuedAfterNext = lastQueued > next ? int(lastQueued - next) : 0;
				return f;
			}
			bool PairFrom(uint64_t from, EntryKey* out) const override
			{
				const uint64_t a = from == 0 ? render + 1 : from;
				if (a + 1 > lastQueued) return false;
				out->from = a;
				out->to = a + 1;
				out->epoch = epoch;
				return true;
			}
			double Estimate(const EntryKey& pair, int, const AcquireResult& fromPlanes, const AcquireResult& toPlanes) override
			{
				planesSeen.push_back(std::make_pair(uint64_t(fromPlanes.cpuBytes), uint64_t(toPlanes.cpuBytes)));
				if (fail) return -1.0;
				++estimatesByTo[pair.to];
				return pairMs;
			}
			uint64_t SourceFrame() const override { return render; }
			int MaxEstimatesPerFrame() const
			{
				int m = 0;
				for (const auto& e : estimatesByTo) m = e.second > m ? e.second : m;
				return m;
			}
			int TotalEstimates() const
			{
				int t = 0;
				for (const auto& e : estimatesByTo) t += e.second;
				return t;
			}
		};

		// The Acquire fake: P13 is served (the frame number is reported in cpuBytes so the test sees which frame was asked), or not.
		struct FakeAcquire
		{
			bool serve = true;
			int calls = 0;
			std::vector<ProductId> ids;
			AcquireFn Fn()
			{
				return [this](ProductId id, uint64_t seq, AcquireForm) {
					++calls;
					ids.push_back(id);
					AcquireResult r;
					if (serve)
					{
						r.status = AcquireStatus::Reused;
						r.source = AcquireSource::Memo;
						r.cpuBytes = size_t(seq);
					}
					else
						r.cause = AcquireCause::FrameReleased;
					return r;
				};
			}
		};

		class FakeCpuSink : public Jobs::ICpuJobSink
		{
		public:
			bool accept = true;
			std::vector<JobDecl> accepted;
			bool Accept(const JobDecl& d, std::string* why) override
			{
				if (!accept) { if (why) *why = "no laned producer"; return false; }
				accepted.push_back(d);
				return true;
			}
		};

		SpareReport Present(SpareHost& host, uint64_t seq, bool repeat, double budgetMs)
		{
			host.NotePresented(seq, repeat);
			host.SetBudget(budgetMs);
			return host.Fire(SpareTrigger::PostPresentNoExtras);
		}
	}

	TEST_CLASS(SchedulerS1Tests)
	{
	public:
		// S1-T1: graphOrder = the index in Graph::nodes; a hand-set order is refused; Derived must follow its inputs; one job per product.
		TEST_METHOD(JobRegistryOrderIsGraphOrder)
		{
			const Graph g = SmallGraph();
			Assert::AreEqual(2u, Jobs::GraphOrderOf(g, kP3AnalysisField));
			Assert::AreEqual(Jobs::kNoGraphOrder, Jobs::GraphOrderOf(g, kP10GrainSplit));

			SpareHost host;
			Jobs::JobRegistry reg;
			FakeEngine eng;
			FakeAcquire acq;
			AheadRing ring;
			LaneAheadSpareWork p3(&eng, acq.Fn(), Jobs::GraphOrderOf(g, kP3AnalysisField), ring);
			std::string why;
			Assert::IsTrue(reg.Register(host, g, Decl(g, kP3AnalysisField, JobClass::Core), &p3, &why), std::wstring(why.begin(), why.end()).c_str());
			Assert::AreEqual(size_t(1), host.Slot().WorkCount());

			// hand-set order
			JobDecl hand = Decl(g, kRefinedField, JobClass::Derived);
			hand.graphOrder = 9;
			FunctionSpareWork w9(kRefinedField, SpareClass::Core, 9);
			Assert::IsFalse(reg.Register(host, g, hand, &w9, &why));
			Assert::IsTrue(why.find("hand-set order") != std::string::npos);

			// derived after its input: accepted
			FunctionSpareWork p3r(kRefinedField, SpareClass::Core, 3);
			Assert::IsTrue(reg.Register(host, g, Decl(g, kRefinedField, JobClass::Derived), &p3r, &why));

			// derived BEFORE its input (a graph where the reader sits first): refused
			Graph bad;
			bad.valid = true;
			bad.nodes.push_back(NodeOf(kRefinedField, { kP3AnalysisField }));
			bad.nodes.push_back(NodeOf(kP3AnalysisField));
			SpareHost host2;
			FunctionSpareWork p3rBad(kRefinedField, SpareClass::Core, 0);
			Assert::IsFalse(Jobs::Register(host2, bad, Decl(bad, kRefinedField, JobClass::Derived), &p3rBad, &why));
			Assert::IsTrue(why.find("not after its input") != std::string::npos);

			// one job per product (compute once)
			AheadRing p3AgainRing(RingCapacity());
			LaneAheadSpareWork p3Again(&eng, acq.Fn(), 2, p3AgainRing);
			Assert::IsFalse(reg.Register(host, g, Decl(g, kP3AnalysisField, JobClass::Core), &p3Again, &why));
			Assert::IsTrue(why.find("already has a job") != std::string::npos);

			// a work whose class differs from the declaration
			FunctionSpareWork wrong(kP1HalfLuma, SpareClass::Fill, 0);
			Assert::IsFalse(reg.Register(host, g, Decl(g, kP1HalfLuma, JobClass::Core), &wrong, &why));

			// shared_gap is S5's
			reg.SetMode(ScheduleMode::SharedGap);
			FunctionSpareWork p1(kP1HalfLuma, SpareClass::Core, 0);
			Assert::IsFalse(reg.Register(host, g, Decl(g, kP1HalfLuma, JobClass::Core), &p1, &why));
			Assert::AreEqual(4ull, reg.Refused());
			Assert::IsTrue(reg.StatusLine().find("refused") != std::string::npos);
		}

		// Q1: the Unpack class path works with a fake product; P13 itself is refused as an Unpack job and the status line names the absence.
		TEST_METHOD(UnpackClassPathWithAFakeAndNoProductionUnpack)
		{
			const Graph g = SmallGraph();
			SpareHost host;
			Jobs::JobRegistry reg;
			std::string why;
			FunctionSpareWork p13(kP13SourceRing, SpareClass::Unpack, Jobs::GraphOrderOf(g, kP13SourceRing));
			Assert::IsFalse(reg.Register(host, g, Decl(g, kP13SourceRing, JobClass::Unpack), &p13, &why));
			Assert::IsTrue(why.find("S3A-UNPACK-SLOT") != std::string::npos);

			int unpackRuns = 0, coreRuns = 0;
			std::vector<int> order;
			FunctionSpareWork unpack(kFakeUnpack, SpareClass::Unpack, Jobs::GraphOrderOf(g, kFakeUnpack));
			unpack.hasWork = [] { return true; };
			unpack.run = [&](uint32_t) { ++unpackRuns; order.push_back(0); return 0.5; };
			FunctionSpareWork core(kP1HalfLuma, SpareClass::Core, Jobs::GraphOrderOf(g, kP1HalfLuma));
			core.hasWork = [] { return true; };
			core.run = [&](uint32_t) { ++coreRuns; order.push_back(1); return 0.5; };
			Assert::IsTrue(reg.Register(host, g, Decl(g, kP1HalfLuma, JobClass::Core), &core, &why));
			Assert::IsTrue(reg.Register(host, g, Decl(g, kFakeUnpack, JobClass::Unpack), &unpack, &why));
			Present(host, 1, false, 10.0);   // both unmeasured: only the first (unpack) runs; core is promoted first next slot
			Present(host, 2, false, 10.0);
			Present(host, 3, false, 10.0);
			Assert::AreEqual(3, unpackRuns);
			Assert::AreEqual(2, coreRuns);
			Assert::AreEqual(0, order[0], L"unpack runs before core (class order)");
			Assert::IsTrue(reg.StatusLine().find("unpack: none") != std::string::npos, L"the absent production Unpack job is visible");
		}

		// S1-T2: 24-in-60 (2-3 presents per film frame): one Run per film frame, AlreadyRan after; each frame's P3 estimated once.
		TEST_METHOD(LaneAheadSpareWorkRunsOncePerSourceFrame)
		{
			FakeEngine eng;
			FakeAcquire acq;
			AheadRing p3Ring(2);
			LaneAheadSpareWork p3(&eng, acq.Fn(), 2, p3Ring);
			SpareHost host;
			host.Slot().Register(&p3);
			const int pattern[] = { 3, 2, 3, 2, 3, 2 };
			uint64_t ran = 0, already = 0;
			for (int f = 0; f < 6; ++f)
			{
				eng.render = uint64_t(f + 1);
				eng.lastQueued = eng.render + 3;
				for (int k = 0; k < pattern[f]; ++k)
				{
					const SpareReport r = Present(host, eng.render, k > 0, 20.0);
					if (r.outcome == SpareOutcome::Ran) ++ran;
					if (r.outcome == SpareOutcome::AlreadyRan) ++already;
				}
				// the render frame's own pair is consumed at its estimate (from the previous slot), as the renderer will
				p3.ConsumeAtRender(eng.render + 1, eng.render + 2, eng.epoch);
			}
			Assert::AreEqual(uint64_t(6), ran);
			Assert::AreEqual(uint64_t(15 - 6), already);
			Assert::AreEqual(1, eng.MaxEstimatesPerFrame(), L"ComputeOnce: <= 1 estimate per source frame");
			// every estimate read its two frames from P13 through Acquire
			for (ProductId id : acq.ids) Assert::AreEqual(int(kP13SourceRing), int(id));
			Assert::AreEqual(size_t(eng.TotalEstimates()) * 2u, acq.ids.size());
		}

		// S1-T3: on frames the slot ran, the render takes the ring entry (render-time P3 = 0); a deferred pair falls back at render, counted,
		// with the slot's reason on the status line.
		TEST_METHOD(P3NotMadeInRenderWhenSlotRan)
		{
			FakeEngine eng;
			FakeAcquire acq;
			AheadRing p3Ring(2);
			LaneAheadSpareWork p3(&eng, acq.Fn(), 2, p3Ring);
			SpareHost host;
			host.Slot().Register(&p3);
			eng.render = 1;
			eng.lastQueued = 4;
			Present(host, 1, false, 20.0);   // estimates (2,3), (3,4)
			Assert::AreEqual(2, p3.Ring().Held());
			eng.render = 2;
			Assert::IsTrue(p3.ConsumeAtRender(2, 3, eng.epoch), L"frame 2's pair comes from the slot");
			Assert::AreEqual(0ull, p3.RenderFallback());
			Assert::AreEqual(1ull, p3.RenderSaved());

			// no budget for the next pair: the slot does not run it; the render estimates it, counted and visible
			eng.render = 3;
			eng.lastQueued = 6;
			Present(host, 3, false, 0.0);   // NoBudget
			Assert::IsTrue(p3.ConsumeAtRender(3, 4, eng.epoch), L"(3,4) was made two slots ago");
			Assert::IsFalse(p3.ConsumeAtRender(4, 5, eng.epoch));
			Assert::AreEqual(1ull, p3.RenderFallback());
			const std::string s = p3.StatusLine();
			Assert::IsTrue(s.find("render 1") != std::string::npos);
		}

		// Ruling 2026-11-14 (lead 10): the pair's planes come from P13 through Acquire; Unavailable = not estimated, counted with the cause.
		TEST_METHOD(SourcePlanesAreAcquiredFromP13NeverCopied)
		{
			FakeEngine eng;
			FakeAcquire acq;
			acq.serve = false;
			AheadRing p3Ring(2);
			LaneAheadSpareWork p3(&eng, acq.Fn(), 2, p3Ring);
			SpareHost host;
			host.Slot().Register(&p3);
			Present(host, 1, false, 20.0);
			Assert::AreEqual(0, eng.TotalEstimates(), L"no estimate without the P13 view");
			Assert::AreEqual(1ull, p3.SourceNotServed());
			Assert::IsTrue(p3.LastSourceCause() == AcquireCause::FrameReleased);
			Assert::IsTrue(p3.StatusLine().find("source ring not served") != std::string::npos);

			acq.serve = true;
			eng.render = 2;
			eng.lastQueued = 5;
			Present(host, 2, false, 20.0);
			Assert::AreEqual(2, eng.TotalEstimates());
			Assert::AreEqual(uint64_t(3), eng.planesSeen[0].first, L"the A frame asked is the pair's from (render + 1)");
			Assert::AreEqual(uint64_t(4), eng.planesSeen[0].second, L"the B frame asked is the pair's to (render + 2)");
		}

		// S1-T4: a Core job that does not fit is DEFERRED (counted) and runs first in the next slot that has budget; its cost is never decayed.
		TEST_METHOD(DeferredCoreIsFirstNextSlotNeverCostDecayed)
		{
			FakeEngine eng;
			FakeAcquire acq;
			eng.pairMs = 5.0;
			AheadRing p3Ring(1);
			LaneAheadSpareWork p3(&eng, acq.Fn(), 2, p3Ring);   // depth 1: one unsplittable pair per slot
			std::vector<int> order;
			FunctionSpareWork fill(kP5MonitorTiles, SpareClass::Fill, 9);
			fill.hasWork = [] { return true; };
			fill.run = [&](uint32_t) { order.push_back(5); return 0.1; };
			SpareHost host;
			host.Slot().Register(&p3);
			host.Slot().Register(&fill);

			eng.render = 1; eng.lastQueued = 3;
			Present(host, 1, false, 20.0);   // measures the pair (5 ms)
			p3.ConsumeAtRender(2, 3, eng.epoch);
			const double predicted = p3.PredictMs();
			Assert::AreEqual(5.0, predicted, 1e-9);

			eng.render = 2; eng.lastQueued = 4;
			const SpareReport tight = Present(host, 2, false, 3.0);
			Assert::AreEqual(1u, tight.coreDeferred, L"core deferred, never cap-skipped");
			Assert::AreEqual(predicted, p3.PredictMs(), 1e-12, L"R4: a deferral never decays the cost");

			order.clear();
			eng.render = 3; eng.lastQueued = 5;
			const SpareReport wide = Present(host, 3, false, 20.0);
			Assert::AreEqual(0u, wide.coreDeferred);
			Assert::IsTrue(eng.estimatesByTo.count(5) == 1, L"the deferred core job ran in the next slot");
			Assert::AreEqual(size_t(1), order.size());
			Assert::AreEqual(1ull, p3.Reasons().Count(Reason::NoSpare), L"the deferral is the slot's NoSpare verdict, counted");
		}

		// S1-T5: games (1 queued) = no field ahead (backward only); competitive = no slot work; the status line names the reason.
		TEST_METHOD(LowLatencyGamesBackwardOnlyCompetitiveNoSlotWork)
		{
			FakeEngine eng;
			FakeAcquire acq;
			AheadRing p3Ring(RingCapacity());
			LaneAheadSpareWork p3(&eng, acq.Fn(), 2, p3Ring);
			SpareHost host;
			host.Slot().Register(&p3);
			eng.in.lowLatency = true;
			Assert::IsTrue(Present(host, 1, false, 20.0).outcome == SpareOutcome::NothingToDo);
			Assert::IsTrue(p3.StatusLine().find(ReasonName(Reason::LowLatency)) != std::string::npos);
			eng.in.lowLatency = false;
			eng.in.competitive = true;
			eng.render = 2;
			Assert::IsTrue(Present(host, 2, false, 20.0).outcome == SpareOutcome::NothingToDo);
			Assert::IsTrue(p3.StatusLine().find(ReasonName(Reason::Competitive)) != std::string::npos);
			Assert::AreEqual(0, eng.TotalEstimates());
			Assert::AreEqual(0, acq.calls, L"nothing Acquired in low latency");
		}

		// S1-T6: without the optical-flow producer (P2m path, doc 91 C :78) the policy never waits for P2 and the slot still estimates.
		TEST_METHOD(DegradedNoNvofStillEstimates)
		{
			FakeEngine eng;
			FakeAcquire acq;
			eng.in.producerRunning = false;
			eng.in.nextPairP2Ready = false;
			AheadRing p3Ring(2);
			LaneAheadSpareWork p3(&eng, acq.Fn(), 2, p3Ring);
			SpareHost host;
			host.Slot().Register(&p3);
			Present(host, 1, false, 20.0);
			Assert::AreEqual(2, eng.TotalEstimates());
			// with the producer running and its P2 missing: P2NotReady, nothing estimated
			eng.in.producerRunning = true;
			eng.render = 2; eng.lastQueued = 6;
			p3.ConsumeAtRender(2, 3, eng.epoch);
			Present(host, 2, false, 20.0);
			Assert::AreEqual(2, eng.TotalEstimates());
			Assert::IsTrue(p3.Reasons().Last() == Reason::P2NotReady);
		}

		// S1-T7: the Original pathway registers nothing and the budget is never fed (byte-identical, IC-2 R9).
		TEST_METHOD(OriginalRegistersNothing)
		{
			const Graph g = SmallGraph();
			SpareHost host;
			Jobs::JobRegistry reg;
			reg.SetOriginal(true);
			FakeEngine eng;
			FakeAcquire acq;
			AheadRing p3Ring(RingCapacity());
			LaneAheadSpareWork p3(&eng, acq.Fn(), 2, p3Ring);
			Assert::IsFalse(reg.Register(host, g, Decl(g, kP3AnalysisField, JobClass::Core), &p3));
			Assert::AreEqual(size_t(0), host.Slot().WorkCount());
			Assert::AreEqual(1ull, reg.OriginalRefused());
			Assert::IsTrue(reg.StatusLine().find("Original pathway") != std::string::npos);

			Vpv::Skeleton::SlotBudgetFeed feed;
			RenderAhead::Presenter presenter;
			presenter.originalPathway = true;
			Vpv::Skeleton::SlotPresentFacts f;
			f.sourceSequence = 1;
			f.trigger = SpareTrigger::PostPresentNoExtras;
			f.rendered = true;
			f.renderMs = 5.0;
			f.captureRateHz = 60.0;
			const SpareReport r = feed.OnAfterPresent(host, f, &presenter);
			Assert::IsTrue(r.outcome == SpareOutcome::Off);
			Assert::AreEqual(uint64_t(0), feed.BudgetFeeds(), L"SetBudget never called");
			Assert::AreEqual(0.0, presenter.spareSlotMs.load());
			Assert::AreEqual(0, acq.calls);
		}

		// S1-T8: a cpu job goes to the CPU lane under the same graph order, never into the render-device slot.
		TEST_METHOD(CpuJobUsesLaneRunner)
		{
			const Graph g = SmallGraph();
			SpareHost host;
			Jobs::JobRegistry reg;
			std::string why;
			Assert::IsFalse(reg.Register(host, g, Decl(g, kP1HalfLuma, JobClass::Core, true), nullptr, &why));
			Assert::IsTrue(why.find("no CPU lane") != std::string::npos);
			FakeCpuSink sink;
			reg.SetCpuSink(&sink);
			Assert::IsTrue(reg.Register(host, g, Decl(g, kP1HalfLuma, JobClass::Core, true), nullptr, &why));
			Assert::AreEqual(size_t(1), sink.accepted.size());
			Assert::AreEqual(0u, sink.accepted[0].graphOrder);
			Assert::AreEqual(size_t(0), host.Slot().WorkCount(), L"not render-device work");
			Assert::AreEqual(size_t(1), reg.CpuJobs());
			sink.accept = false;
			Assert::IsFalse(reg.Register(host, g, Decl(g, kP2VectorPair, JobClass::Core, true), nullptr, &why));
			Assert::IsTrue(why.find("no laned producer") != std::string::npos);
		}

		// Z1: the chain ring holds strictly chained pairs once each; a consume hit needs pair + epoch; misses discard by cause.
		TEST_METHOD(AheadRingChainsAndDiscardsByCause)
		{
			AheadRing ring(4);
			EntryKey a{ 2, 3, 7 }, b{ 3, 4, 7 }, c{ 5, 6, 7 };
			Assert::IsTrue(ring.Push(a) == PushResult::Held);
			Assert::IsTrue(ring.Push(a) == PushResult::AlreadyHeld);
			Assert::IsTrue(ring.Push(c) == PushResult::NotChained);
			Assert::IsTrue(ring.Push(b) == PushResult::Held);
			Assert::AreEqual(uint64_t(4), ring.BackTo());

			Assert::IsTrue(ring.Consume(2, 3, 7));
			Assert::IsFalse(ring.Consume(3, 4, 8), L"another epoch");
			Assert::AreEqual(1ull, ring.Discarded(DiscardCause::EpochMoved));
			Assert::IsTrue(ring.Empty());

			ring.Push(EntryKey{ 10, 11, 7 });
			ring.Push(EntryKey{ 11, 12, 7 });
			Assert::IsFalse(ring.Consume(10, 13, 7), L"next differs");
			Assert::AreEqual(2ull, ring.Discarded(DiscardCause::NextDiffers));

			ring.Push(EntryKey{ 20, 21, 7 });
			ring.Push(EntryKey{ 21, 22, 7 });
			Assert::IsTrue(ring.Consume(21, 22, 7), L"a passed entry is dropped, the chained one still hits");
			Assert::AreEqual(1ull, ring.Discarded(DiscardCause::Drop));

			ring.Push(EntryKey{ 30, 31, 7 });
			ring.Reset();
			Assert::AreEqual(1ull, ring.Discarded(DiscardCause::Resync));
			ring.SetCapacity(1);
			ring.Push(EntryKey{ 40, 41, 7 });
			Assert::IsTrue(ring.Push(EntryKey{ 41, 42, 7 }) == PushResult::Full);
		}

		// PREC2-S3A MUST 3: an entry keeps the host slot it was estimated into across consumes (the textures never move); MUST 4: the ring's
		// capacity is the owner's ahead ceiling, never the P2 ring's 13.
		TEST_METHOD(AheadRingSlotFollowsTheEntryAcrossConsume)
		{
			AheadRing ring(2);
			const EntryKey a{ 2, 3, 7 }, b{ 3, 4, 7 }, c{ 4, 5, 7 };
			Assert::AreEqual(0, ring.FreeSlot());
			Assert::IsTrue(ring.Push(a) == PushResult::Held);
			Assert::IsTrue(ring.Push(b) == PushResult::Held);
			Assert::AreEqual(-1, ring.FreeSlot(), L"both slots held");
			int slot = -2;
			Assert::IsTrue(ring.Consume(2, 3, 7, &slot));
			Assert::AreEqual(0, slot, L"A was estimated into slot 0");
			Assert::AreEqual(1, ring.SlotAt(0), L"B keeps slot 1 after A is consumed");
			Assert::AreEqual(0, ring.FreeSlot(), L"the next pair goes to A's freed slot, never over B");
			Assert::IsTrue(ring.Push(c, 1) == PushResult::BadSlot, L"a held pair's slot is refused");
			Assert::IsTrue(ring.Push(c) == PushResult::Held);
			Assert::AreEqual(0, ring.SlotAt(1));
			Assert::IsTrue(ring.Consume(3, 4, 7, &slot));
			Assert::AreEqual(1, slot, L"B is read from the slot it was estimated into");
			Assert::IsTrue(ring.Consume(4, 5, 7, &slot));
			Assert::AreEqual(0, slot, L"C is read from the slot it was estimated into");

			// a smaller capacity keeps only a chained prefix whose slots still exist (each dropped entry counted)
			AheadRing r4(4);
			r4.Push(EntryKey{ 10, 11, 7 });
			r4.Push(EntryKey{ 11, 12, 7 });
			r4.Push(EntryKey{ 12, 13, 7 });
			Assert::IsTrue(r4.Consume(10, 11, 7));
			r4.SetCapacity(2);
			Assert::AreEqual(1, r4.Held(), L"(12,13) sat in slot 2, beyond the new capacity");
			Assert::AreEqual(1, r4.SlotAt(0));
			Assert::AreEqual(1ull, r4.Discarded(DiscardCause::Drop));

			Assert::IsTrue(AheadRing::kCapacityMax == kAheadCeilingLead, L"capacity max = the owner's ahead cap");
			AheadRing big(kStructuralMaxLead);
			Assert::AreEqual(kAheadCeilingLead, big.Capacity(), L"held to the owner's ahead cap");
		}

		// PREC2-S3A MUST 3 through the adapter: push, consume, push at capacity 2 leaves each held pair reading the slot it was estimated into.
		TEST_METHOD(LaneAheadSpareWorkEstimatesIntoAFreeSlot)
		{
			struct SlotEngine : FakeEngine
			{
				std::map<uint64_t, int> slotByTo;
				double Estimate(const EntryKey& pair, int slot, const AcquireResult& fromPlanes, const AcquireResult& toPlanes) override
				{
					slotByTo[pair.to] = slot;
					return FakeEngine::Estimate(pair, slot, fromPlanes, toPlanes);
				}
			};
			SlotEngine eng;
			FakeAcquire acq;
			AheadRing p3Ring(2);
			LaneAheadSpareWork p3(&eng, acq.Fn(), 2, p3Ring);
			SpareHost host;
			host.Slot().Register(&p3);
			eng.render = 1;
			eng.lastQueued = 4;
			Present(host, 1, false, 20.0);   // (2,3) -> slot 0, (3,4) -> slot 1
			Assert::AreEqual(0, eng.slotByTo[3]);
			Assert::AreEqual(1, eng.slotByTo[4]);
			int slot = -2;
			eng.render = 2;
			Assert::IsTrue(p3.ConsumeAtRender(2, 3, eng.epoch, &slot));
			Assert::AreEqual(0, slot);
			eng.lastQueued = 5;
			Present(host, 2, false, 20.0);   // (4,5) -> the freed slot 0, never (3,4)'s slot 1
			Assert::AreEqual(0, eng.slotByTo[5]);
			eng.render = 3;
			Assert::IsTrue(p3.ConsumeAtRender(3, 4, eng.epoch, &slot));
			Assert::AreEqual(eng.slotByTo[4], slot, L"(3,4) is read from the slot it was estimated into");
			eng.render = 4;
			Assert::IsTrue(p3.ConsumeAtRender(4, 5, eng.epoch, &slot));
			Assert::AreEqual(eng.slotByTo[5], slot);

			// SHOULD 4: a lost lease is on the status line, not only in a counter
			p3Ring.Push(EntryKey{ 5, 6, eng.epoch });
			p3Ring.Discard(DiscardCause::LeaseLost);
			Assert::IsTrue(p3.StatusLine().find("lease lost 1") != std::string::npos);
		}

		// Z3: the feed publishes every held entry once, ascending.
		TEST_METHOD(AheadFeedPublishesEachEntryOnceAscending)
		{
			AheadRing ring(4);
			ring.Push(EntryKey{ 2, 3, 1 });
			ring.Push(EntryKey{ 3, 4, 1 });
			AheadFeed feed;
			uint64_t out[8] = {};
			Assert::AreEqual(2, feed.Pending(ring, out, 8));
			Assert::AreEqual(uint64_t(3), out[0]);
			Assert::AreEqual(uint64_t(4), out[1]);
			feed.MarkPublished(3);
			Assert::AreEqual(1, feed.Pending(ring, out, 8));
			feed.MarkPublished(4);
			feed.MarkPublished(3);
			Assert::AreEqual(0, feed.Pending(ring, out, 8));
			Assert::AreEqual(2ull, feed.Published());
		}

		// Q5: auto = on only once the S1 acceptance row is recorded; the status says which.
		TEST_METHOD(AutoKeyFollowsTheAcceptanceRow)
		{
			Assert::IsFalse(KeyOn(SpareKey::Auto));
			Assert::IsFalse(KeyOn(SpareKey::Auto, false));
			Assert::IsTrue(KeyOn(SpareKey::Auto, true));
			Assert::IsFalse(KeyOn(SpareKey::Off, true));
			Input in;
			in.key = SpareKey::Auto;
			in.matcherActive = true;
			in.queuedAfterNext = 3;
			Assert::IsTrue(Decide(in).reason == Reason::Off);
			in.autoAccepted = true;
			Assert::IsTrue(Decide(in).reason == Reason::Ahead);
			Assert::IsTrue(std::string(KeyStateText(SpareKey::Auto, false)).find("off until") != std::string::npos);
		}

		// VRAM per tier: the ring holds only the matcher's outputs (the planes are P13's), derived from the dimensions.
		TEST_METHOD(RingVramIsFieldsOnlyAndScalesWithGeometry)
		{
			const uint64_t hd = EntryFieldBytes(1920, 1080), uhd = EntryFieldBytes(3840, 2160);
			Assert::IsTrue(hd > 1000000u && hd < 2500000u);
			Assert::IsTrue(uhd > 3 * hd && uhd < 5 * hd);
			Assert::AreEqual(4 * uhd, RingVramBytes(3840, 2160, 4));
			Assert::AreEqual(uint64_t(0), RingVramBytes(3840, 2160, 0));
		}
	};
}
