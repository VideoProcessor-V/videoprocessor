// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// M2 P10-5: R15 DecideIngress reaches the renderer registered under the call's host and nobody else; with none it is
// not handled (the host's pre-filled values stand); an unregistered renderer is shown under "Fallbacks engaged";
// through the hub, the decision's out fields (drop, gaps, film rate, arrival) reach the caller. CPU only.
#include "pch.h"
#include "CppUnitTest.h"
#include <VP-V/modules/input/queue_timing/IngressPolicy.h>
#include <VP-V/modules/input/queue_timing/IngressState.h>
#include <VP-V/core/FrameAnalysisStore.h>
#include <VP-V/core/RunningPathwayLatch.h>

#include <atomic>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	namespace
	{
		namespace RE = RendererExtension;

		struct FakeRenderer
		{
			int calls = 0;
			bool drop = false;
			static bool Decide(void* self, RE::IngressContext& ic)
			{
				FakeRenderer& r = *static_cast<FakeRenderer*>(self);
				++r.calls;
				ic.drop = r.drop;
				ic.filmHz = 23.976;
				ic.gapBefore = true;
				ic.gapCause = "pulldown lock change";
				ic.arrivalQpc = 1234;
				return true;
			}
		};

		struct HubScope
		{
			struct Fwd : RE::IRendererExtension
			{
				bool OnFrame(RE::FrameContext& c) override { return Vpv::Hub::ExtensionHub::Instance().OnFrame(c); }
			};
			HubScope()
			{
				RE::ResetForTesting();
				Vpv::Hub::ExtensionHub::Instance().ResetModulesForTesting();
				RE::Register(std::unique_ptr<RE::IRendererExtension>(new Fwd()));
				Vpv::Hub::ExtensionHub::Instance().AddModule(std::unique_ptr<Vpv::Hub::ISkeletonModule>(new Vpv::Ingress::IngressPolicy()));
			}
			~HubScope()
			{
				RE::ResetForTesting();
				Vpv::Hub::ExtensionHub::Instance().ResetModulesForTesting();
			}
		};

		// RAII restore of the process-wide running-pathway latch so no test leaks latch state across tests.
		struct RunningPathwayGuard
		{
			RunningPathwayGuard() { Vpv::RunningPathway::SetForTesting(Vpv::RunningPathway::kUnknown); }
			~RunningPathwayGuard() { Vpv::RunningPathway::SetForTesting(Vpv::RunningPathway::kUnknown); }
			RunningPathwayGuard(const RunningPathwayGuard&) = delete;
			RunningPathwayGuard& operator=(const RunningPathwayGuard&) = delete;
		};

		struct S1M1FakeRenderer
		{
			int decideCalls = 0;
			mutable int targetCalls = 0;
			static bool DecideDrop(void* self, RE::IngressContext& ic)
			{
				S1M1FakeRenderer& r = *static_cast<S1M1FakeRenderer*>(self);
				++r.decideCalls;
				ic.drop = true;
				ic.filmHz = 23.976;
				return true;
			}
			static void TargetSeven(const void* self, RE::QueueTargetContext& qc)
			{
				const S1M1FakeRenderer& r = *static_cast<const S1M1FakeRenderer*>(self);
				++r.targetCalls;
				qc.target = 7;
			}
		};
	}

	TEST_CLASS(IngressPolicyTests)
	{
	public:
		TEST_METHOD(NoRegisteredRendererLeavesTheHostValues)
		{
			HubScope hub;
			int key = 0;
			RE::IngressContext ic;
			ic.host = &key;
			ic.gapBefore = true;
			ic.gapCause = "missed frames";
			ic.arrivalQpc = 77;
			Assert::IsFalse(RE::DecideIngress(ic));
			Assert::IsFalse(ic.drop);
			Assert::IsTrue(ic.gapBefore);
			Assert::AreEqual(std::string("missed frames"), std::string(ic.gapCause));
			Assert::AreEqual<int64_t>(77, ic.arrivalQpc);
		}

		TEST_METHOD(TheDecisionReachesOnlyItsRendererAndComesBackToTheCaller)
		{
			HubScope hub;
			FakeRenderer a, b;
			b.drop = true;
			Vpv::Ingress::HostBinding bindA(&a, &FakeRenderer::Decide, &a);
			Vpv::Ingress::HostBinding bindB(&b, &FakeRenderer::Decide, &b);
			RE::IngressContext ic;
			ic.host = &a;
			Assert::IsTrue(RE::DecideIngress(ic));
			Assert::IsFalse(ic.drop, L"A keeps the frame");
			Assert::AreEqual(23.976, ic.filmHz);
			Assert::AreEqual<int64_t>(1234, ic.arrivalQpc);
			Assert::AreEqual(std::string("pulldown lock change"), std::string(ic.gapCause));
			RE::IngressContext id;
			id.host = &b;
			Assert::IsTrue(RE::DecideIngress(id) && id.drop, L"B drops its repeat");
			Assert::AreEqual(1, a.calls);
			Assert::AreEqual(1, b.calls);
		}

		TEST_METHOD(ARemovedRendererIsNeverReached)
		{
			HubScope hub;
			FakeRenderer a;
			{
				Vpv::Ingress::HostBinding bind(&a, &FakeRenderer::Decide, &a);
			}
			RE::IngressContext ic;
			ic.host = &a;
			Assert::IsFalse(RE::DecideIngress(ic));
			Assert::AreEqual(0, a.calls);
		}

		// R19: the renderer's VP-V target replaces Bill's for its host only; no renderer: Bill's value.
		TEST_METHOD(QueueTargetReachesOnlyItsRenderer)
		{
			HubScope hub;
			struct Target
			{
				static void Five(const void*, RE::QueueTargetContext& qc) { qc.target = 5; }
			};
			FakeRenderer a;
			int other = 0;
			Vpv::Ingress::HostBinding bind(&a, &FakeRenderer::Decide, &a, &Target::Five);
			RE::QueueTargetContext qc;
			qc.capacity = 8;
			Assert::AreEqual<size_t>(5, RE::ResolveQueueTarget(&a, qc, 4), L"the renderer's target");
			RE::QueueTargetContext qo;
			qo.capacity = 8;
			Assert::AreEqual<size_t>(4, RE::ResolveQueueTarget(&other, qo, 4), L"another host keeps Bill's value");
			Vpv::Ingress::HostBinding noTarget(&other, &FakeRenderer::Decide, &other);
			RE::QueueTargetContext qn;
			qn.capacity = 8;
			Assert::AreEqual<size_t>(4, RE::ResolveQueueTarget(&other, qn, 4), L"a binding without a target function keeps it too");
		}

		// Audit slice 3 F1: a throw in the decision is not lost: counted, shown under Fallbacks as ONE id, the frame queued as
		// a no-fill gap at its pre-decision arrival without film timing, and the renderer asked to restart its detector.
		TEST_METHOD(AThrowingDecisionIsVisibleAndQueuesTheFrameAsAGap)
		{
			HubScope hub;
			struct Throwing
			{
				int resets = 0;
				static bool Decide(void*, RE::IngressContext& ic)
				{
					ic.filmHz = 24.0;          // half-advanced state, then the throw
					ic.arrivalQpc = 999;
					ic.drop = true;
					throw std::runtime_error("detector");
				}
				static void Fault(void* self) { ++static_cast<Throwing*>(self)->resets; }
			} t;
			Vpv::Ingress::HostBinding bind(&t, &Throwing::Decide, &t, nullptr, &Throwing::Fault);
			const uint64_t before = Vpv::Ingress::IngressFaults();
			RE::IngressContext ic;
			ic.host = &t;
			ic.arrivalQpc = 100;
			Assert::IsTrue(RE::DecideIngress(ic), L"decided (by the fault path)");
			Assert::IsFalse(ic.drop, L"the frame is queued");
			Assert::IsTrue(ic.gapBefore);
			Assert::AreEqual<uint32_t>(0, ic.gapFrames);
			Assert::AreEqual(std::string("ingress policy fault"), std::string(ic.gapCause));
			Assert::AreEqual<int64_t>(100, ic.arrivalQpc, L"the pre-decision arrival");
			Assert::AreEqual(0.0, ic.filmHz);
			Assert::AreEqual(1, t.resets, L"the detector restarts");
			Assert::AreEqual(before + 1, Vpv::Ingress::IngressFaults());
			Vpv::Ingress::IngressPolicy module;
			std::vector<std::string> lines;
			module.OnStatus(RE::Context(), RE::StatusKind::Fallbacks, lines, nullptr, nullptr);
			bool found = false;
			for (const std::string& l : lines)
				if (l.find("ingress_fault (") == 0)
				{
					found = true;
					const size_t close = l.rfind(')');
					Assert::IsTrue(l.find("; ") > l.find('(') && l.find("; ") < close, L"one id: the '; ' is inside the brackets");
				}
			Assert::IsTrue(found);
			// Slice 4 F2: a host gap survives the fault (its cause and frame count are the host's).
			RE::IngressContext ih;
			ih.host = &t;
			ih.gapBefore = true;
			ih.gapFrames = 3;
			ih.gapCause = "missed frames";
			Assert::IsTrue(RE::DecideIngress(ih));
			Assert::IsTrue(ih.gapBefore);
			Assert::AreEqual<uint32_t>(3, ih.gapFrames, L"the host's lost frames kept (gap fill may pair across them)");
			Assert::AreEqual(std::string("missed frames"), std::string(ih.gapCause));
		}

		// Slice 4 F4: faults are counted per renderer: a retired renderer's faults leave with it.
		TEST_METHOD(FaultsLeaveWithTheirRenderer)
		{
			HubScope hub;
			struct Throwing { static bool Decide(void*, RE::IngressContext&) { throw 1; } };
			const uint64_t before = Vpv::Ingress::IngressFaults();
			int key = 0;
			{
				Vpv::Ingress::HostBinding bind(&key, &Throwing::Decide, &key);
				RE::IngressContext ic;
				ic.host = &key;
				RE::DecideIngress(ic);
				Assert::AreEqual(before + 1, Vpv::Ingress::IngressFaults());
			}
			Assert::AreEqual(before, Vpv::Ingress::IngressFaults(), L"a new renderer starts clean");
		}

		// W1-A4 (88 resync/flush audit, pulldownAcquiring sticks true): the capture thread's pulldown state around the renderer's
		// IngressPulldown (IngressState is the renderer's base; the .inl calls these two members). 24p in 60 Hz, a 3:2 cycle
		// (N R R N R); repeat flags as the signature compare would give them.
		TEST_METHOD(AcquiringClearsWhenDetectionStopsOrTheSignalIsLost)
		{
			Vpv::Ingress::IngressState st;
			const char* cycle = "NRRNR";
			int i = 0;
			Pulldown::Decision d;
			for (; i < 20; ++i)   // four cycles: acquiring (three periodic cycles), far short of the two-second lock
				d = st.m_pulldown.Next(cycle[i % 5] == 'R', false, 60.0);
			Assert::IsFalse(d.locked, L"not yet locked");
			st.m_pulldownAcquiring.store(!d.locked && st.m_pulldown.Acquiring());   // as IngressPulldown does per frame
			Assert::IsTrue(st.m_pulldownAcquiring.load(), L"the cadence is being acquired: the standing engine's demand");
			// Detection switched off (or the mode lost) while acquiring: no lock, cadence 0 - the old else-if skipped it.
			Assert::IsFalse(st.ClearPulldownWhenOff(), L"no lock was held: no 'pulldown off' gap");
			Assert::IsFalse(st.m_pulldownAcquiring.load(), L"the acquiring flag is cleared");
			Assert::IsFalse(st.m_pulldown.Acquiring(), L"the detector's stale history is gone: it does not re-assert acquiring on re-enable");
			Assert::AreEqual(0, st.m_pulldownCadence.load());
			Assert::IsFalse(st.ClearPulldownWhenOff(), L"idempotent: a second off frame changes nothing");
			Assert::IsFalse(st.m_pulldownAcquiring.load());
		}

		TEST_METHOD(AnIdleOrAcquiringDetectorKeepsNoStateWhenOffAndALockStillGapsAsBefore)
		{
			Vpv::Ingress::IngressState idle;
			Assert::IsFalse(idle.ClearPulldownWhenOff(), L"nothing engaged: nothing to clear, no gap");
			Assert::IsFalse(idle.m_pulldownAcquiring.load());
			Assert::AreEqual(0, idle.m_pulldownCadence.load());
			// The control (unchanged behaviour): a held lock is dropped with its cadence, clock and signature, and reports the gap.
			Vpv::Ingress::IngressState st;
			const char* cycle = "NRRNR";
			Pulldown::Decision d;
			int frames = 0;
			for (; frames < 600 && !d.locked; ++frames)
				d = st.m_pulldown.Next(cycle[frames % 5] == 'R', false, 60.0);
			Assert::IsTrue(d.locked, L"3:2 at 60 Hz locks within 600 frames");
			Assert::IsTrue(frames >= 120, L"after the two-second wait (plausibility: 120 frames at 60 Hz)");
			st.m_pulldownCadence.store((d.cycle << 16) | d.unique);
			st.m_pulldownSignature = 77;
			st.m_pulldownFilmQpc = 88;
			st.m_pulldownAcquiring.store(true);   // locked and flagged: the lock branch clears both
			Assert::IsTrue(st.ClearPulldownWhenOff(), L"a lock was held: the caller marks the gap");
			Assert::IsFalse(st.m_pulldown.Locked());
			Assert::AreEqual(0, st.m_pulldownCadence.load());
			Assert::IsFalse(st.m_pulldownAcquiring.load());
			Assert::AreEqual<uint64_t>(0, st.m_pulldownSignature);
			Assert::AreEqual<int64_t>(0, st.m_pulldownFilmQpc);
		}

		// W1-A4: the resync reset (PulldownResyncStep.reset) starts the detector over and clears the flag with it.
		TEST_METHOD(AResetClearsTheAcquiringFlagWithTheDetector)
		{
			Vpv::Ingress::IngressState st;
			const char* cycle = "NRRNR";
			for (int i = 0; i < 20; ++i)
				st.m_pulldown.Next(cycle[i % 5] == 'R', false, 60.0);
			st.m_pulldownAcquiring.store(true);
			st.m_pulldownSignature = 5;
			st.m_pulldownFilmQpc = 6;
			st.ResetPulldown();
			Assert::IsFalse(st.m_pulldownAcquiring.load());
			Assert::IsFalse(st.m_pulldown.Acquiring());
			Assert::AreEqual<uint64_t>(0, st.m_pulldownSignature);
			Assert::AreEqual<int64_t>(0, st.m_pulldownFilmQpc);
		}

		// W2-C1: a contained throw in the decision (IngressFaultThunk) asks for the reset and clears the acquiring flag at once; the
		// detector itself starts over on the next frame (the request), as before.
		TEST_METHOD(AFaultClearsTheAcquiringFlagAndRequestsTheReset)
		{
			Vpv::Ingress::IngressState st;
			const char* cycle = "NRRNR";
			Pulldown::Decision d;
			for (int i = 0; i < 20; ++i)
				d = st.m_pulldown.Next(cycle[i % 5] == 'R', false, 60.0);
			st.m_pulldownAcquiring.store(!d.locked && st.m_pulldown.Acquiring());
			Assert::IsTrue(st.m_pulldownAcquiring.load(), L"acquiring before the fault");
			Assert::IsFalse(st.m_pulldownResetRequested.load());
			st.RequestPulldownResetAfterFault();
			Assert::IsFalse(st.m_pulldownAcquiring.load(), L"the faulted frame's verdict is not a standing demand");
			Assert::IsTrue(st.m_pulldownResetRequested.load(), L"the next frame starts the detector over");
			Assert::IsTrue(st.m_pulldown.Acquiring(), L"the detector itself waits for the request (unchanged)");
		}

		// W1-A4 audit S2: a LOCK case through the reset. A resync reset on a locked detector clears the acquiring flag and the
		// detector at once, but leaves the published cadence for Next to rewrite; a frame that then arrives with detection off still
		// reports the lock that was held (the caller's "pulldown off" gap) and clears the cadence.
		TEST_METHOD(ALockCaseAfterAResetClearsAcquiringAndStillReportsThePulldownOffGap)
		{
			Vpv::Ingress::IngressState st;
			const char* cycle = "NRRNR";
			Pulldown::Decision d;
			for (int frames = 0; frames < 600 && !d.locked; ++frames)
				d = st.m_pulldown.Next(cycle[frames % 5] == 'R', false, 60.0);
			Assert::IsTrue(d.locked, L"locked");
			st.m_pulldownCadence.store((d.cycle << 16) | d.unique);
			st.m_pulldownAcquiring.store(true);   // a stale flag on top of the lock
			st.ResetPulldown();
			Assert::IsFalse(st.m_pulldownAcquiring.load(), L"the reset clears acquiring");
			Assert::IsFalse(st.m_pulldown.Locked(), L"and the detector");
			Assert::AreNotEqual(0, st.m_pulldownCadence.load(), L"the published cadence waits for Next");
			Assert::IsTrue(st.ClearPulldownWhenOff(), L"the held cadence is reported: the caller marks the 'pulldown off' gap");
			Assert::AreEqual(0, st.m_pulldownCadence.load());
			Assert::IsFalse(st.m_pulldownAcquiring.load());
			Assert::IsFalse(st.ClearPulldownWhenOff(), L"nothing left: no second gap");
		}

		// C7b cadence feed: the record carries exactly the detector's facts for the frame (CadenceFeed.h), through the real store.
		TEST_METHOD(TheCadenceRecordEqualsTheLockFacts)
		{
			Vpv::Ingress::IngressState st;
			FrameAnalysis::Store store(FrameAnalysis::RingDepth(4, 4));
			store.SetEnabled(true);
			const char* cycle = "NRRNR";
			Pulldown::Decision d;
			uint64_t seq = 0;
			for (int frames = 0; frames < 600 && !d.locked; ++frames)
				d = st.m_pulldown.Next(cycle[frames % 5] == 'R', false, 60.0);
			Assert::IsTrue(d.locked, L"3:2 at 60 Hz locks");
			for (int k = 0; k < 10; ++k)   // ten more frames at the lock
			{
				const bool repeat = cycle[k % 5] == 'R';
				d = st.m_pulldown.Next(repeat, false, 60.0);
				if (d.drop)
					continue;   // a dropped repeat is never queued: no record
				const Vpv::Ingress::CadenceFacts facts = Vpv::Ingress::FactsOf(d);
				Assert::IsTrue(facts.valid && facts.locked);
				FrameAnalysis::Identity id;
				id.seq = ++seq;
				Assert::IsTrue(store.Begin(id, 0u) != 0);
				const FrameAnalysis::Cadence record = Vpv::Ingress::ToRecord(facts);
				Assert::IsTrue(store.PublishCadence(id.seq, record));
				FrameAnalysis::Record out;
				Assert::IsTrue(store.Read(id.seq, FrameAnalysis::Use::SingleFrame, FrameAnalysis::kCadence, out) == FrameAnalysis::ReadStatus::Ok);
				Assert::AreEqual(d.filmHz, out.cadence.filmHz, L"the film rate of the lock");
				Assert::IsTrue(out.cadence.filmHz > 23.9 && out.cadence.filmHz < 24.1, L"24p film in 60 Hz");
				Assert::AreEqual(static_cast<int>(d.filmIndex % static_cast<uint64_t>(d.unique)), out.cadence.phase);
				Assert::AreEqual(2, d.unique);
				Assert::AreEqual(5, d.cycle);
				Assert::IsFalse(out.cadence.repeatOfPrevious, L"a queued frame is never a cadence repeat");
			}
			Assert::IsTrue(seq > 0, L"some frames were kept");
		}

		// C7b: an unlocked or acquiring frame, an uncertain phase and a frame the detector never saw are all "unknown".
		TEST_METHOD(AnUnlockedAcquiringOrUncertainFrameIsRecordedAsUnknown)
		{
			Vpv::Ingress::IngressState st;
			Assert::IsFalse(st.m_pulldownFrameFacts.valid, L"no detector decision: nothing to publish");
			const char* cycle = "NRRNR";
			Pulldown::Decision d;
			for (int i = 0; i < 20; ++i)
				d = st.m_pulldown.Next(cycle[i % 5] == 'R', false, 60.0);
			Assert::IsFalse(d.locked);
			Assert::IsTrue(st.m_pulldown.Acquiring(), L"acquiring, not locked");
			const FrameAnalysis::Cadence acquiring = Vpv::Ingress::ToRecord(Vpv::Ingress::FactsOf(d));
			Assert::AreEqual(-1, acquiring.phase);
			Assert::AreEqual(0.0, acquiring.filmHz, L"the signal's rate");
			Assert::IsFalse(acquiring.repeatOfPrevious);
			Vpv::Ingress::CadenceFacts uncertain;
			uncertain.valid = uncertain.locked = uncertain.phaseUncertain = true;
			uncertain.filmHz = 23.976;
			uncertain.cycle = 5;
			uncertain.unique = 2;
			uncertain.filmIndex = 7;
			const FrameAnalysis::Cadence record = Vpv::Ingress::ToRecord(uncertain);
			Assert::AreEqual(-1, record.phase, L"a re-phase is being tracked: the slot is unknown");
			Assert::AreEqual(23.976, record.filmHz, L"the lock's rate stands");
		}

		// C7b: Take hands the slot's facts over once and clears the slot, so the next frame never inherits them.
		TEST_METHOD(TheCadenceFactsAreTakenOnceAndTheSlotIsCleared)
		{
			Vpv::Ingress::CadenceFacts slot;
			slot.valid = slot.locked = true;
			slot.filmHz = 23.976;
			slot.cycle = 5;
			slot.unique = 2;
			slot.filmIndex = 9;
			const Vpv::Ingress::CadenceFacts first = Vpv::Ingress::Take(slot);
			Assert::IsTrue(first.valid && first.locked);
			Assert::AreEqual(23.976, first.filmHz);
			Assert::AreEqual(5, first.cycle);
			Assert::AreEqual(static_cast<uint64_t>(9), first.filmIndex);
			Assert::IsFalse(slot.valid, L"the slot is cleared");
			Assert::AreEqual(0.0, slot.filmHz);
			const Vpv::Ingress::CadenceFacts second = Vpv::Ingress::Take(slot);
			Assert::IsFalse(second.valid, L"a second take yields nothing");
			Assert::IsFalse(second.locked);
		}

		TEST_METHOD(AFullTableIsVisibleUnderFallbacks)
		{
			std::vector<std::unique_ptr<FakeRenderer>> renderers;
			std::vector<std::unique_ptr<Vpv::Ingress::HostBinding>> bindings;
			const size_t before = Vpv::Ingress::UnboundRenderers();
			for (int i = 0; i < 64; ++i)
			{
				renderers.emplace_back(new FakeRenderer());
				bindings.emplace_back(new Vpv::Ingress::HostBinding(renderers.back().get(), &FakeRenderer::Decide, renderers.back().get()));
				if (!bindings.back()->Registered())
					break;
			}
			Assert::IsFalse(bindings.back()->Registered(), L"the table is bounded");
			Assert::AreEqual(before + 1, Vpv::Ingress::UnboundRenderers());
			Vpv::Ingress::IngressPolicy module;
			std::vector<std::string> lines;
			module.OnStatus(RE::Context(), RE::StatusKind::Fallbacks, lines, nullptr, nullptr);
			bool unbound = false;
			for (const std::string& l : lines)
				unbound = unbound || l == "ingress policy (renderer not registered: pulldown, film clock, stale gap off)";
			Assert::IsTrue(unbound, L"one id, no top-level '; '");
			bindings.clear();
			Assert::AreEqual(before, Vpv::Ingress::UnboundRenderers(), L"released with the renderer");
		}

		// S1-M1: cold Original launch must leave QueueTarget and DecideIngress at Bill's values BEFORE frame 1
		// (without any NoteOriginalPathway call). Negative control on the exact same renderer/context inputs under
		// kVpv proves the VP-V hooks actively mutate qc.target (2 -> 7) and ic.drop (false -> true) when ungated.
		TEST_METHOD(ColdOriginalLaunchLeavesQueueTargetAndIngressBillBeforeFirstFrame)
		{
			RunningPathwayGuard latchGuard;
			Vpv::Ingress::IngressPolicy policy;

			// Negative control (VP-V launch): before any NoteOriginalPathway call, VP-V hooks run.
			Vpv::RunningPathway::SetForTesting(Vpv::RunningPathway::kVpv);
			{
				S1M1FakeRenderer vpvHost;
				Vpv::Ingress::HostBinding bind(&vpvHost, &S1M1FakeRenderer::DecideDrop, &vpvHost, &S1M1FakeRenderer::TargetSeven);
				Assert::IsTrue(bind.Registered(), L"VP-V HostBinding must register");
				Assert::IsFalse(Vpv::Ingress::OriginalPathwayOf(&vpvHost), L"negative control: kVpv starts in VP-V pathway");

				RE::FrameContext fc;
				fc.host = &vpvHost;
				RE::QueueTargetContext qc;
				qc.capacity = 8;
				qc.billTarget = 2;
				qc.target = 2;
				policy.OnQueueTarget(fc, qc);
				Assert::AreEqual<size_t>(7, qc.target, L"negative control: VP-V launch must mutate qc.target from 2 to 7");
				Assert::AreEqual(1, vpvHost.targetCalls, L"negative control: QueueTargetFn must be called under kVpv");

				RE::IngressContext ic;
				ic.host = &vpvHost;
				ic.drop = false;
				ic.arrivalQpc = 55;
				const bool decided = policy.OnDecideIngress(fc, ic);
				Assert::IsTrue(decided, L"negative control: VP-V launch must handle OnDecideIngress");
				Assert::IsTrue(ic.drop, L"negative control: VP-V DecideFn must set ic.drop = true");
				Assert::AreEqual(1, vpvHost.decideCalls, L"negative control: DecideFn must be called under kVpv");
			}

			// Fix proof (Original launch): fresh HostBinding without any NoteOriginalPathway call (before frame 1).
			Vpv::RunningPathway::SetForTesting(Vpv::RunningPathway::kOriginal);
			{
				S1M1FakeRenderer origHost;
				Vpv::Ingress::HostBinding bind(&origHost, &S1M1FakeRenderer::DecideDrop, &origHost, &S1M1FakeRenderer::TargetSeven);
				Assert::IsTrue(bind.Registered(), L"Original HostBinding must register");
				Assert::IsTrue(Vpv::Ingress::OriginalPathwayOf(&origHost), L"cold Original launch must report OriginalPathwayOf == true before frame 1");

				RE::FrameContext fc;
				fc.host = &origHost;
				RE::QueueTargetContext qc;
				qc.capacity = 8;
				qc.billTarget = 2;
				qc.target = 2;
				policy.OnQueueTarget(fc, qc);
				Assert::AreEqual<size_t>(2, qc.target, L"cold Original launch must preserve Bill's pre-filled qc.target == 2 before frame 1");
				Assert::AreEqual(0, origHost.targetCalls, L"cold Original launch must NOT invoke QueueTargetFn");

				RE::IngressContext ic;
				ic.host = &origHost;
				ic.drop = false;
				ic.gapBefore = true;
				ic.gapCause = "missed frames";
				ic.arrivalQpc = 55;
				const bool decided = policy.OnDecideIngress(fc, ic);
				Assert::IsFalse(decided, L"cold Original launch must NOT handle OnDecideIngress before frame 1");
				Assert::IsFalse(ic.drop, L"cold Original launch must leave ic.drop == false");
				Assert::AreEqual<int64_t>(55, ic.arrivalQpc, L"cold Original launch must preserve Bill's pre-filled arrivalQpc");
				Assert::AreEqual(0, origHost.decideCalls, L"cold Original launch must NOT invoke DecideFn");
			}
		}

		// S1-M1: SettingsApplied dispatched to IngressPolicy::OnLifecycle must immediately sync the bound renderer's
		// Original pathway in both directions while paused/idle (zero frames arrived), and Shutdown must unbind the
		// implHost pointer so a later SettingsApplied for the same/reused impl address cannot mutate the renderer.
		TEST_METHOD(PausedIdleSettingsAppliedSyncsOriginalPathwayBothDirectionsWithoutFrames)
		{
			RunningPathwayGuard latchGuard;
			Vpv::RunningPathway::SetForTesting(Vpv::RunningPathway::kVpv);
			Vpv::Ingress::IngressPolicy policy;

			S1M1FakeRenderer renderer;
			int impl = 0;
			Vpv::Ingress::HostBinding bind(&renderer, &S1M1FakeRenderer::DecideDrop, &renderer, &S1M1FakeRenderer::TargetSeven);
			Assert::IsTrue(bind.Registered(), L"HostBinding must register");
			Vpv::Ingress::BindImplHost(&renderer, &impl);

			RE::FrameContext fc;
			fc.host = &renderer;

			// Negative control: before SettingsApplied, the binding runs VP-V hooks.
			Assert::IsFalse(Vpv::Ingress::OriginalPathwayOf(&renderer), L"negative control: starts in VP-V pathway");
			{
				RE::QueueTargetContext qc;
				qc.capacity = 8;
				qc.billTarget = 2;
				qc.target = 2;
				policy.OnQueueTarget(fc, qc);
				Assert::AreEqual<size_t>(7, qc.target, L"negative control: before SettingsApplied, OnQueueTarget mutates 2 -> 7");
				Assert::AreEqual(1, renderer.targetCalls, L"negative control: QueueTargetFn called once before SettingsApplied");

				RE::IngressContext ic;
				ic.host = &renderer;
				ic.drop = false;
				Assert::IsTrue(policy.OnDecideIngress(fc, ic), L"negative control: before SettingsApplied, OnDecideIngress handles frame");
				Assert::IsTrue(ic.drop, L"negative control: before SettingsApplied, DecideFn sets ic.drop = true");
				Assert::AreEqual(1, renderer.decideCalls, L"negative control: DecideFn called once before SettingsApplied");
			}

			// Fix proof (VP-V -> Original while paused/idle, zero frames arrived).
			{
				RE::LifecycleFacts toOriginal;
				toOriginal.host = &impl;
				toOriginal.kind = RE::Lifecycle::SettingsApplied;
				toOriginal.code = 1;
				policy.OnLifecycle(toOriginal);

				Assert::IsTrue(Vpv::Ingress::OriginalPathwayOf(&renderer), L"SettingsApplied(code=1) must immediately set OriginalPathwayOf == true without frames");

				RE::QueueTargetContext qc;
				qc.capacity = 8;
				qc.billTarget = 2;
				qc.target = 2;
				policy.OnQueueTarget(fc, qc);
				Assert::AreEqual<size_t>(2, qc.target, L"Original pathway must preserve Bill's qc.target == 2 without frames");
				Assert::AreEqual(1, renderer.targetCalls, L"QueueTargetFn must NOT be invoked while in Original pathway");

				RE::IngressContext ic;
				ic.host = &renderer;
				ic.drop = false;
				Assert::IsFalse(policy.OnDecideIngress(fc, ic), L"Original pathway must return false from OnDecideIngress without frames");
				Assert::IsFalse(ic.drop, L"Original pathway must leave ic.drop == false");
				Assert::AreEqual(1, renderer.decideCalls, L"DecideFn must NOT be invoked while in Original pathway");
			}

			// Fix proof (Original -> VP-V while paused/idle, zero frames arrived).
			{
				RE::LifecycleFacts toVpv;
				toVpv.host = &impl;
				toVpv.kind = RE::Lifecycle::SettingsApplied;
				toVpv.code = 0;
				policy.OnLifecycle(toVpv);

				Assert::IsFalse(Vpv::Ingress::OriginalPathwayOf(&renderer), L"SettingsApplied(code=0) must immediately restore OriginalPathwayOf == false without frames");

				RE::QueueTargetContext qc;
				qc.capacity = 8;
				qc.billTarget = 2;
				qc.target = 2;
				policy.OnQueueTarget(fc, qc);
				Assert::AreEqual<size_t>(7, qc.target, L"VP-V pathway must immediately restore QueueTargetFn (2 -> 7) without frames");
				Assert::AreEqual(2, renderer.targetCalls, L"QueueTargetFn must be invoked again after returning to VP-V");

				RE::IngressContext ic;
				ic.host = &renderer;
				ic.drop = false;
				Assert::IsTrue(policy.OnDecideIngress(fc, ic), L"VP-V pathway must immediately restore OnDecideIngress without frames");
				Assert::IsTrue(ic.drop, L"VP-V pathway must set ic.drop = true again");
				Assert::AreEqual(2, renderer.decideCalls, L"DecideFn must be invoked again after returning to VP-V");
			}

			// Teardown safety: Shutdown for &impl must unbind implHost so a later SettingsApplied on the same/reused
			// address cannot mutate the unbound renderer's pathway.
			{
				RE::LifecycleFacts shutdown;
				shutdown.host = &impl;
				shutdown.kind = RE::Lifecycle::Shutdown;
				policy.OnLifecycle(shutdown);
				Assert::IsFalse(Vpv::Ingress::OriginalPathwayOf(&renderer), L"Shutdown for impl must not flip renderer pathway");

				RE::LifecycleFacts staleOriginal;
				staleOriginal.host = &impl;
				staleOriginal.kind = RE::Lifecycle::SettingsApplied;
				staleOriginal.code = 1;
				policy.OnLifecycle(staleOriginal);
				Assert::IsFalse(Vpv::Ingress::OriginalPathwayOf(&renderer), L"SettingsApplied after Shutdown on reused impl pointer must NOT mutate renderer pathway");

				RE::QueueTargetContext qc;
				qc.capacity = 8;
				qc.billTarget = 2;
				qc.target = 2;
				policy.OnQueueTarget(fc, qc);
				Assert::AreEqual<size_t>(7, qc.target, L"renderer must stay on VP-V after stale SettingsApplied on shut-down impl");
				Assert::AreEqual(3, renderer.targetCalls, L"QueueTargetFn must still run after stale SettingsApplied on shut-down impl");
			}
		}

		// S1-M1 (Pass-A MED): SyncHostFromImpl must unify capture and timed-file arrival so a cold-Original HostBinding
		// (OriginalPathwayOf == true) with a fresh IngressState (m_originalPathway == false) immediately clears to VP-V
		// when VP-V FixSwitchBits are published, binds implHost for paused/idle SettingsApplied, and re-binds after GpuReleased.
		BEGIN_TEST_METHOD_ATTRIBUTE(SyncHostFromImplClearsStaleOriginalPathwayAndRebindsAfterGpuReleased)
			TEST_METHOD_ATTRIBUTE(L"Category", L"Cpu")   // lifecycle enum only: no GPU device
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(SyncHostFromImplClearsStaleOriginalPathwayAndRebindsAfterGpuReleased)
		{
			RunningPathwayGuard latchGuard;
			Vpv::RunningPathway::SetForTesting(Vpv::RunningPathway::kOriginal);
			Vpv::Ingress::IngressPolicy policy;

			S1M1FakeRenderer renderer;
			int impl = 0;
			struct HubImplGuard
			{
				const void* host;
				explicit HubImplGuard(const void* h) : host(h) {}
				~HubImplGuard()
				{
					RE::LifecycleFacts shutdown;
					shutdown.host = host;
					shutdown.kind = RE::Lifecycle::Shutdown;
					Vpv::Hub::ExtensionHub::Instance().OnLifecycle(shutdown);
				}
				HubImplGuard(const HubImplGuard&) = delete;
				HubImplGuard& operator=(const HubImplGuard&) = delete;
			} hubGuard(&impl);

			// 1. Reproduce the exact Pass-A MED split-brain state: HostBinding initializes to Original (true) from kOriginal
			// while a fresh IngressState starts with m_originalPathway == false, and the hub publishes VP-V FixSwitchBits.
			Vpv::Ingress::HostBinding bind(&renderer, &S1M1FakeRenderer::DecideDrop, &renderer, &S1M1FakeRenderer::TargetSeven);
			Assert::IsTrue(bind.Registered(), L"HostBinding must register");
			Assert::IsTrue(Vpv::Ingress::OriginalPathwayOf(&renderer), L"HostBinding must initialize OriginalPathwayOf == true under kOriginal");

			Vpv::Ingress::IngressState state;
			Assert::IsFalse(state.m_originalPathway.load(std::memory_order_acquire),
				L"fresh IngressState starts with m_originalPathway == false");

			std::atomic<bool> queueFixes{ false };
			std::atomic<bool> motionFixes{ false };

			Vpv::Ingress::IngressState::FixSwitches vpvSwitches;
			vpvSwitches.pulldownFixes = true;
			vpvSwitches.timingContinuityFixes = true;
			vpvSwitches.cadenceClassFixes = true;
			vpvSwitches.queueTimingFixes = true;
			vpvSwitches.motionHistoryFixes = true;
			vpvSwitches.originalPathway = false;
			const uint32_t vpvBits = Vpv::Ingress::IngressState::FixSwitchBitsOf(vpvSwitches);
			Assert::IsTrue(Vpv::Hub::ExtensionHub::Instance().NoteFixSwitches(&impl, vpvBits),
				L"ExtensionHub must publish VP-V FixSwitchBits for impl");

			RE::FrameContext fc;
			fc.host = &renderer;

			// 2. In-test negative control: exact pre-fix TimedFileFrames.inl edge-triggered pattern on the exact same
			// state & fixBits skips NoteOriginalPathway because m_originalPathway was already false -> false, leaving
			// the binding stuck in Original and OnQueueTarget refusing the VP-V target.
			{
				uint32_t readBits = 0;
				Assert::IsTrue(Vpv::Hub::ExtensionHub::Instance().FixSwitchBits(&impl, readBits),
					L"negative control: ExtensionHub must return published VP-V FixSwitchBits");
				Assert::AreEqual<uint32_t>(vpvBits, readBits,
					L"negative control: published FixSwitchBits must round-trip unchanged");

				const bool preFixChanged = state.SyncIngressSwitches(
					Vpv::Ingress::IngressState::FixSwitchesFromBits(readBits), queueFixes, motionFixes);
				Assert::IsFalse(preFixChanged,
					L"negative control: pre-fix SyncIngressSwitches returns false when state.m_originalPathway is already false");
				if (preFixChanged)
					Vpv::Ingress::NoteOriginalPathway(&renderer, state.m_originalPathway.load(std::memory_order_relaxed));

				Assert::IsTrue(Vpv::Ingress::OriginalPathwayOf(&renderer),
					L"negative control: pre-fix edge-triggered pattern leaves OriginalPathwayOf(&renderer) stuck at true");

				RE::QueueTargetContext qc;
				qc.capacity = 8;
				qc.billTarget = 2;
				qc.target = 2;
				policy.OnQueueTarget(fc, qc);
				Assert::AreEqual<size_t>(2, qc.target,
					L"negative control: stuck Original binding refuses VP-V QueueTarget (preserves Bill's 2)");
				Assert::AreEqual(0, renderer.targetCalls,
					L"negative control: QueueTargetFn must NOT run while stuck in Original");
			}

			// 3. Fix proof: reset fix-switch atomics to false so SyncHostFromImpl must actively populate them, then
			// call SyncHostFromImpl on the exact same split-brain state (binding == true, state.m_originalPathway == false).
			queueFixes.store(false, std::memory_order_relaxed);
			motionFixes.store(false, std::memory_order_relaxed);
			state.m_pulldownFixes.store(false, std::memory_order_relaxed);
			state.m_timingContinuityFixes.store(false, std::memory_order_relaxed);
			state.m_cadenceClassFixes.store(false, std::memory_order_relaxed);

			Vpv::Ingress::SyncHostFromImpl(&renderer, &impl, state, queueFixes, motionFixes);

			Assert::IsFalse(Vpv::Ingress::OriginalPathwayOf(&renderer),
				L"SyncHostFromImpl must immediately clear stale OriginalPathwayOf(&renderer) to false");
			Assert::IsTrue(queueFixes.load(std::memory_order_relaxed),
				L"SyncHostFromImpl must set queueTimingFixes == true from hub FixSwitchBits");
			Assert::IsTrue(motionFixes.load(std::memory_order_relaxed),
				L"SyncHostFromImpl must set motionHistoryFixes == true from hub FixSwitchBits");
			Assert::IsTrue(state.m_pulldownFixes.load(std::memory_order_relaxed),
				L"SyncHostFromImpl must set pulldownFixes == true from hub FixSwitchBits");
			Assert::IsTrue(state.m_timingContinuityFixes.load(std::memory_order_relaxed),
				L"SyncHostFromImpl must set timingContinuityFixes == true from hub FixSwitchBits");
			Assert::IsTrue(state.m_cadenceClassFixes.load(std::memory_order_relaxed),
				L"SyncHostFromImpl must set cadenceClassFixes == true from hub FixSwitchBits");

			{
				RE::QueueTargetContext qc;
				qc.capacity = 8;
				qc.billTarget = 2;
				qc.target = 2;
				policy.OnQueueTarget(fc, qc);
				Assert::AreEqual<size_t>(7, qc.target,
					L"After SyncHostFromImpl, OnQueueTarget must execute VP-V target (2 -> 7)");
				Assert::AreEqual(1, renderer.targetCalls,
					L"After SyncHostFromImpl, QueueTargetFn must be invoked once");
			}

			{
				RE::LifecycleFacts toOriginal;
				toOriginal.host = &impl;
				toOriginal.kind = RE::Lifecycle::SettingsApplied;
				toOriginal.code = 1;
				policy.OnLifecycle(toOriginal);
				Assert::IsTrue(Vpv::Ingress::OriginalPathwayOf(&renderer),
					L"Paused/idle SettingsApplied(host=&impl, code=1) must immediately flip OriginalPathwayOf(&renderer) to true (proving implHost was bound)");
			}

			// 4. Post-GpuReleased re-bind proof: GpuReleased clears implHost and drops the hub pathway slot; a subsequent
			// SyncHostFromImpl with freshly published VP-V FixSwitchBits must clear OriginalPathwayOf(&renderer) again
			// and re-bind implHost so paused/idle SettingsApplied reaches the renderer once more.
			{
				RE::LifecycleFacts gpuReleased;
				gpuReleased.host = &impl;
				gpuReleased.kind = RE::Lifecycle::GpuReleased;
				policy.OnLifecycle(gpuReleased);
				Vpv::Hub::ExtensionHub::Instance().OnLifecycle(gpuReleased);

				// Prove GpuReleased actually cleared implHost before re-binding.
				RE::LifecycleFacts ignoredVpv;
				ignoredVpv.host = &impl;
				ignoredVpv.kind = RE::Lifecycle::SettingsApplied;
				ignoredVpv.code = 0;
				policy.OnLifecycle(ignoredVpv);
				Assert::IsTrue(Vpv::Ingress::OriginalPathwayOf(&renderer),
					L"After GpuReleased clears implHost, SettingsApplied(host=&impl, code=0) must NOT mutate unbound renderer");
			}

			Assert::IsTrue(Vpv::Hub::ExtensionHub::Instance().NoteFixSwitches(&impl, vpvBits),
				L"ExtensionHub must publish fresh VP-V FixSwitchBits for impl after GpuReleased");
			queueFixes.store(false, std::memory_order_relaxed);
			motionFixes.store(false, std::memory_order_relaxed);

			Vpv::Ingress::SyncHostFromImpl(&renderer, &impl, state, queueFixes, motionFixes);

			Assert::IsFalse(Vpv::Ingress::OriginalPathwayOf(&renderer),
				L"Post-GpuReleased SyncHostFromImpl must clear OriginalPathwayOf(&renderer) back to false");
			Assert::IsTrue(queueFixes.load(std::memory_order_relaxed),
				L"Post-GpuReleased SyncHostFromImpl must re-sync queueTimingFixes == true");
			Assert::IsTrue(motionFixes.load(std::memory_order_relaxed),
				L"Post-GpuReleased SyncHostFromImpl must re-sync motionHistoryFixes == true");

			{
				RE::QueueTargetContext qc;
				qc.capacity = 8;
				qc.billTarget = 2;
				qc.target = 2;
				policy.OnQueueTarget(fc, qc);
				Assert::AreEqual<size_t>(7, qc.target,
					L"Post-GpuReleased SyncHostFromImpl must restore VP-V QueueTarget (2 -> 7)");
				Assert::AreEqual(2, renderer.targetCalls,
					L"Post-GpuReleased QueueTargetFn must be invoked a second time");
			}

			{
				RE::LifecycleFacts toOriginalAgain;
				toOriginalAgain.host = &impl;
				toOriginalAgain.kind = RE::Lifecycle::SettingsApplied;
				toOriginalAgain.code = 1;
				policy.OnLifecycle(toOriginalAgain);
				Assert::IsTrue(Vpv::Ingress::OriginalPathwayOf(&renderer),
					L"Post-GpuReleased SettingsApplied(host=&impl, code=1) must immediately flip OriginalPathwayOf(&renderer) to true (proving implHost was re-bound)");

				RE::QueueTargetContext qc;
				qc.capacity = 8;
				qc.billTarget = 2;
				qc.target = 2;
				policy.OnQueueTarget(fc, qc);
				Assert::AreEqual<size_t>(2, qc.target,
					L"After post-GpuReleased SettingsApplied(code=1), OnQueueTarget must preserve Bill's target == 2");
				Assert::AreEqual(2, renderer.targetCalls,
					L"After post-GpuReleased SettingsApplied(code=1), QueueTargetFn must NOT run");
			}
		}

		// S1-M1 (Pass-A MED #3): SyncHostFromImpl must use a sequence-guarded CAS on pathwayState so a concurrent
		// FixSwitchBits read on the capture/file arrival thread can never clobber a newer SettingsApplied transition
		// on the render thread back to a stale pathway.
		TEST_METHOD(SyncHostFromImplConcurrentWithSettingsAppliedNeverLeavesStalePathway)
		{
			RunningPathwayGuard latchGuard;
			Vpv::RunningPathway::SetForTesting(Vpv::RunningPathway::kVpv);
			Vpv::Ingress::IngressPolicy policy;

			S1M1FakeRenderer renderer;
			int impl = 0;
			struct HubImplGuard
			{
				const void* host;
				explicit HubImplGuard(const void* h) : host(h) {}
				~HubImplGuard()
				{
					RE::LifecycleFacts shutdown;
					shutdown.host = host;
					shutdown.kind = RE::Lifecycle::Shutdown;
					Vpv::Hub::ExtensionHub::Instance().OnLifecycle(shutdown);
				}
				HubImplGuard(const HubImplGuard&) = delete;
				HubImplGuard& operator=(const HubImplGuard&) = delete;
			} hubGuard(&impl);

			Vpv::Ingress::HostBinding bind(&renderer, &S1M1FakeRenderer::DecideDrop, &renderer, &S1M1FakeRenderer::TargetSeven);
			Assert::IsTrue(bind.Registered(), L"HostBinding must register");

			Vpv::Ingress::IngressState state;
			std::atomic<bool> queueFixes{ false };
			std::atomic<bool> motionFixes{ false };

			Vpv::Ingress::IngressState::FixSwitches vpvSwitches;
			vpvSwitches.pulldownFixes = true;
			vpvSwitches.timingContinuityFixes = true;
			vpvSwitches.cadenceClassFixes = true;
			vpvSwitches.queueTimingFixes = true;
			vpvSwitches.motionHistoryFixes = true;
			vpvSwitches.originalPathway = false;
			const uint32_t vpvBits = Vpv::Ingress::IngressState::FixSwitchBitsOf(vpvSwitches);

			Vpv::Ingress::IngressState::FixSwitches origSwitches;
			origSwitches.pulldownFixes = false;
			origSwitches.timingContinuityFixes = false;
			origSwitches.cadenceClassFixes = false;
			origSwitches.queueTimingFixes = false;
			origSwitches.motionHistoryFixes = false;
			origSwitches.originalPathway = true;
			const uint32_t origBits = Vpv::Ingress::IngressState::FixSwitchBitsOf(origSwitches);

			RE::FrameContext fc;
			fc.host = &renderer;

			// 1. Negative control (sequential sync): without a racing SettingsApplied, SyncHostFromImpl actively updates
			// OriginalPathwayOf(&renderer) in both directions (VP-V -> Original -> VP-V) when NoteFixSwitches changes.
			Assert::IsTrue(Vpv::Hub::ExtensionHub::Instance().NoteFixSwitches(&impl, vpvBits),
				L"negative control: publish initial VP-V FixSwitchBits");
			Vpv::Ingress::SyncHostFromImpl(&renderer, &impl, state, queueFixes, motionFixes);
			Assert::IsFalse(Vpv::Ingress::OriginalPathwayOf(&renderer),
				L"negative control: sequential SyncHostFromImpl must reflect VP-V FixSwitchBits");

			Assert::IsTrue(Vpv::Hub::ExtensionHub::Instance().NoteFixSwitches(&impl, origBits),
				L"negative control: publish Original FixSwitchBits");
			Vpv::Ingress::SyncHostFromImpl(&renderer, &impl, state, queueFixes, motionFixes);
			Assert::IsTrue(Vpv::Ingress::OriginalPathwayOf(&renderer),
				L"negative control: sequential SyncHostFromImpl must update VP-V -> Original when NoteFixSwitches changes");

			Assert::IsTrue(Vpv::Hub::ExtensionHub::Instance().NoteFixSwitches(&impl, vpvBits),
				L"negative control: publish VP-V FixSwitchBits again");
			Vpv::Ingress::SyncHostFromImpl(&renderer, &impl, state, queueFixes, motionFixes);
			Assert::IsFalse(Vpv::Ingress::OriginalPathwayOf(&renderer),
				L"negative control: sequential SyncHostFromImpl must update Original -> VP-V when NoteFixSwitches changes");

			// 2. Negative control (same-input unprotected hazard): reading FixSwitchBits before SettingsApplied(code=1)
			// and then calling unconditional NoteOriginalPathway with the stale bits clobbers Original back to VP-V
			// and causes OnQueueTarget to execute the VP-V hook (2 -> 7).
			{
				uint32_t staleBits = 0;
				Assert::IsTrue(Vpv::Hub::ExtensionHub::Instance().FixSwitchBits(&impl, staleBits),
					L"negative control: read pre-transition VP-V FixSwitchBits snapshot");
				Assert::AreEqual<uint32_t>(vpvBits, staleBits,
					L"negative control: pre-transition snapshot must equal vpvBits");

				Assert::IsTrue(Vpv::Hub::ExtensionHub::Instance().NoteFixSwitches(&impl, origBits),
					L"negative control: render thread publishes Original FixSwitchBits");
				RE::LifecycleFacts toOrig;
				toOrig.host = &impl;
				toOrig.kind = RE::Lifecycle::SettingsApplied;
				toOrig.code = 1;
				policy.OnLifecycle(toOrig);
				Assert::IsTrue(Vpv::Ingress::OriginalPathwayOf(&renderer),
					L"negative control: SettingsApplied(code=1) sets OriginalPathwayOf(&renderer) == true");

				RE::QueueTargetContext qcOrig;
				qcOrig.capacity = 8;
				qcOrig.billTarget = 2;
				qcOrig.target = 2;
				policy.OnQueueTarget(fc, qcOrig);
				Assert::AreEqual<size_t>(2, qcOrig.target,
					L"negative control: Original pathway preserves Bill's target == 2 before stale clobber");
				Assert::AreEqual(0, renderer.targetCalls,
					L"negative control: QueueTargetFn must not run before stale clobber");

				// Unprotected pre-fix step: unconditional NoteOriginalPathway with the stale VP-V snapshot.
				Vpv::Ingress::NoteOriginalPathway(&renderer,
					Vpv::Ingress::IngressState::FixSwitchesFromBits(staleBits).originalPathway);
				Assert::IsFalse(Vpv::Ingress::OriginalPathwayOf(&renderer),
					L"negative control: unprotected NoteOriginalPathway with stale VP-V bits clobbers SettingsApplied(code=1)");

				RE::QueueTargetContext qcClobbered;
				qcClobbered.capacity = 8;
				qcClobbered.billTarget = 2;
				qcClobbered.target = 2;
				policy.OnQueueTarget(fc, qcClobbered);
				Assert::AreEqual<size_t>(7, qcClobbered.target,
					L"negative control: stale clobber breaks Original gating and mutates qc.target (2 -> 7)");
				Assert::AreEqual(1, renderer.targetCalls,
					L"negative control: stale clobber erroneously invokes QueueTargetFn");
			}

			// 3. Concurrent race proof: 2,000 synchronized rounds (1,000 VP-V -> Original and 1,000 Original -> VP-V,
			// ending deterministically on Original == true) racing Thread A (NoteFixSwitches + SettingsApplied) against
			// Thread B (SyncHostFromImpl) from a per-round barrier with deterministic phase stagger.
			std::atomic<bool> stop{ false };
			std::atomic<int> startGen{ 0 };
			std::atomic<int> doneA{ 0 };
			std::atomic<int> doneB{ 0 };
			std::atomic<bool> roundTargetOriginal{ true };
			std::atomic<int> roundStagger{ 0 };
			std::atomic<int> publishFailures{ 0 };

			auto waitUntil = [&stop](const std::atomic<int>& flag, int target) -> bool
			{
				for (unsigned spins = 0u; flag.load(std::memory_order_acquire) < target; ++spins)
				{
					if (stop.load(std::memory_order_acquire))
						return false;
					if ((spins & 63u) == 63u)
						std::this_thread::yield();
				}
				return true;
			};

			constexpr int kRounds = 2000;
			constexpr int kStaggerSteps = 32;
			constexpr int kThreadAMaxStagger = 24;

			std::thread threadA;
			std::thread threadB;
			struct ThreadJoinGuard
			{
				std::atomic<bool>& stopFlag;
				std::thread& a;
				std::thread& b;
				ThreadJoinGuard(std::atomic<bool>& s, std::thread& ta, std::thread& tb)
					: stopFlag(s), a(ta), b(tb)
				{
				}
				~ThreadJoinGuard()
				{
					stopFlag.store(true, std::memory_order_release);
					if (a.joinable())
						a.join();
					if (b.joinable())
						b.join();
				}
				ThreadJoinGuard(const ThreadJoinGuard&) = delete;
				ThreadJoinGuard& operator=(const ThreadJoinGuard&) = delete;
			} joinGuard(stop, threadA, threadB);

			threadA = std::thread([&]()
			{
				std::atomic<int> delayA{ 0 };
				for (int nextRound = 1; nextRound <= kRounds; ++nextRound)
				{
					if (!waitUntil(startGen, nextRound))
						return;
					const bool targetOrig = roundTargetOriginal.load(std::memory_order_acquire);
					const int stagger = roundStagger.load(std::memory_order_acquire);
					if (stagger < kThreadAMaxStagger)
					{
						for (int s = 0; s < stagger; ++s)
							delayA.fetch_add(1, std::memory_order_relaxed);
					}
					if (!Vpv::Hub::ExtensionHub::Instance().NoteFixSwitches(&impl, targetOrig ? origBits : vpvBits))
						publishFailures.fetch_add(1, std::memory_order_relaxed);
					RE::LifecycleFacts applied;
					applied.host = &impl;
					applied.kind = RE::Lifecycle::SettingsApplied;
					applied.code = targetOrig ? 1 : 0;
					policy.OnLifecycle(applied);
					doneA.store(nextRound, std::memory_order_release);
				}
			});

			threadB = std::thread([&]()
			{
				std::atomic<int> delayB{ 0 };
				for (int nextRound = 1; nextRound <= kRounds; ++nextRound)
				{
					if (!waitUntil(startGen, nextRound))
						return;
					const int stagger = roundStagger.load(std::memory_order_acquire);
					if (stagger >= kThreadAMaxStagger)
					{
						for (int s = 0; s < (stagger - kThreadAMaxStagger + 1); ++s)
							delayB.fetch_add(1, std::memory_order_relaxed);
					}
					Vpv::Ingress::SyncHostFromImpl(&renderer, &impl, state, queueFixes, motionFixes);
					doneB.store(nextRound, std::memory_order_release);
				}
			});

			int vpvToOrigRounds = 0;
			int origToVpvRounds = 0;
			for (int round = 0; round < kRounds; ++round)
			{
				// Even rounds: Original -> VP-V; odd rounds: VP-V -> Original (final round 1999 ends on Original == true).
				const bool targetOrig = (round % 2) == 1;
				const uint32_t initBits = targetOrig ? vpvBits : origBits;
				Assert::IsTrue(Vpv::Hub::ExtensionHub::Instance().NoteFixSwitches(&impl, initBits),
					L"round setup must publish starting FixSwitchBits");
				RE::LifecycleFacts initApplied;
				initApplied.host = &impl;
				initApplied.kind = RE::Lifecycle::SettingsApplied;
				initApplied.code = targetOrig ? 0 : 1;
				policy.OnLifecycle(initApplied);
				if (targetOrig)
				{
					Assert::IsFalse(Vpv::Ingress::OriginalPathwayOf(&renderer),
						L"VP-V -> Original round must start with OriginalPathwayOf(&renderer) == false");
				}
				else
				{
					Assert::IsTrue(Vpv::Ingress::OriginalPathwayOf(&renderer),
						L"Original -> VP-V round must start with OriginalPathwayOf(&renderer) == true");
				}

				roundTargetOriginal.store(targetOrig, std::memory_order_release);
				roundStagger.store((round / 2) % kStaggerSteps, std::memory_order_release);
				const int nextRound = round + 1;
				startGen.store(nextRound, std::memory_order_release);

				Assert::IsTrue(waitUntil(doneA, nextRound), L"Thread A must complete race round");
				Assert::IsTrue(waitUntil(doneB, nextRound), L"Thread B must complete race round");

				const bool actualOriginal = Vpv::Ingress::OriginalPathwayOf(&renderer);
				if (targetOrig)
				{
					Assert::IsTrue(actualOriginal,
						L"Concurrent SyncHostFromImpl must never clobber SettingsApplied(code=1) back to stale VP-V");
					++vpvToOrigRounds;
				}
				else
				{
					Assert::IsFalse(actualOriginal,
						L"Concurrent SyncHostFromImpl must never clobber SettingsApplied(code=0) back to stale Original");
					++origToVpvRounds;
				}
			}

			stop.store(true, std::memory_order_release);
			threadA.join();
			threadB.join();

			Assert::AreEqual(0, publishFailures.load(std::memory_order_acquire),
				L"Every NoteFixSwitches publish on Thread A must succeed");
			Assert::AreEqual(kRounds / 2, vpvToOrigRounds,
				L"All 1,000 VP-V -> Original synchronized race rounds must execute");
			Assert::AreEqual(kRounds / 2, origToVpvRounds,
				L"All 1,000 Original -> VP-V synchronized race rounds must execute");

			// Final authoritative pathway after round 1999 is Original == true; verify OnQueueTarget respects it.
			Assert::IsTrue(Vpv::Ingress::OriginalPathwayOf(&renderer),
				L"Final pathway after race rounds must remain Original == true");
			RE::QueueTargetContext qcFinal;
			qcFinal.capacity = 8;
			qcFinal.billTarget = 2;
			qcFinal.target = 2;
			policy.OnQueueTarget(fc, qcFinal);
			Assert::AreEqual<size_t>(2, qcFinal.target,
				L"After concurrent race ending on Original, OnQueueTarget must preserve Bill's target == 2");
			Assert::AreEqual(1, renderer.targetCalls,
				L"After concurrent race ending on Original, QueueTargetFn must NOT run");
		}
	};
}
