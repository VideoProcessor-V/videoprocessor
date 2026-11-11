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

#include <memory>
#include <stdexcept>
#include <string>
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
	};
}
