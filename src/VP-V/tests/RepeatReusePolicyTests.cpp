#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// WP-24 captured-repeat render reuse (2026-10-24 draft, feat/repeat-reuse). CPU only: the upstream
// comparison rules, VP's policy (switch, Original pathway, arming window, temporal stages, hook
// state incl. opaque hooks), the visible counters, and a simulated 24-in-30 / 30-in-60 cadence.

#include <VP-V/modules/output/presentation/RepeatReusePolicy.h>

#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace RepeatRenderReuse;
using namespace VpRepeatReuse;

namespace
{
	int kHookA = 0, kHookB = 0, kLed = 0, kOpaque = 0, kSigma = 0;

	std::vector<HookInfo> Hooks(uint64_t ledCompensationValue, bool opaque = false)
	{
		std::vector<HookInfo> h = {
			{ &kHookA, true, HookKind::Parameters, ledCompensationValue },   // e.g. LED compensation gamma
			{ &kHookB, true, HookKind::Static, 0 },                          // e.g. calibration LUT
			{ &kLed, true, HookKind::PictureNeutral, 0 },                    // LED measurement
			{ &kSigma, false, HookKind::Parameters, 99 },                    // sigma-delta (OUTPUT: runs again anyway)
		};
		if (opaque)
			h.push_back({ &kOpaque, true, HookKind::Opaque, 0 });
		return h;
	}

	RepeatFrameKey Key(uint64_t sequence, bool repeat)
	{
		RepeatFrameKey k;
		k.sourceSequence = sequence;
		k.cadenceRepeat = repeat;
		k.frameGeneration = 3;
		k.settingsGeneration = 7;
		k.geometry = 11;
		k.colour = 13;
		k.overlays = 17;
		return k;
	}

	Policy OnPolicy()
	{
		Policy p;
		p.SetOptions({ true, false });
		return p;
	}
}

namespace VpvTests
{
	TEST_CLASS(RepeatReusePolicyTests)
	{
	public:
		TEST_METHOD(RepeatOfTheCachedFrameIsReused)
		{
			Policy p = OnPolicy();
			const RepeatFrameKey first = Policy::Complete(Key(5, false), VpTemporal{}, Hooks(1));
			p.NoteFullRender(first, true);
			const RepeatFrameKey repeat = Policy::Complete(Key(5, true), VpTemporal{}, Hooks(1));
			Assert::IsTrue(p.Decide(repeat, true, 1.0).reuse);
			Assert::AreEqual(uint64_t{ 1 }, p.GetStats().reused);
		}

		TEST_METHOD(EveryUpstreamChangeRefusesWithItsReason)
		{
			struct Case { void (*change)(RepeatFrameKey&); Refusal want; };
			const Case cases[] = {
				{ [](RepeatFrameKey& k) { k.cadenceRepeat = false; }, Refusal::NotRepeat },
				{ [](RepeatFrameKey& k) { k.sourceSequence = 6; }, Refusal::OtherSource },
				{ [](RepeatFrameKey& k) { k.frameGeneration = 4; }, Refusal::StreamChanged },
				{ [](RepeatFrameKey& k) { k.settingsGeneration = 8; }, Refusal::SettingsChanged },
				{ [](RepeatFrameKey& k) { k.geometry = 12; }, Refusal::GeometryChanged },      // crop / placement moved
				{ [](RepeatFrameKey& k) { k.colour = 14; }, Refusal::ColourChanged },          // metadata / LUT
				{ [](RepeatFrameKey& k) { k.overlays = 18; }, Refusal::OverlaysChanged },      // subtitle / OSD changed
				{ [](RepeatFrameKey& k) { k.hookState = 1; }, Refusal::HookStateChanged },
				{ [](RepeatFrameKey& k) { k.extension = 1; }, Refusal::ExtensionChanged },
				{ [](RepeatFrameKey& k) { k.temporalRenderState = true; }, Refusal::TemporalState },
			};
			for (const Case& c : cases)
			{
				RepeatFrameKey cached = Key(5, false), now = Key(5, true);
				c.change(now);
				const Decision d = RepeatReuseRules::Compare(cached, true, now);
				Assert::IsFalse(d.reuse);
				Assert::IsTrue(d.refusal == c.want, (std::wstring(L"case: ") + std::to_wstring(static_cast<int>(c.want))).c_str());
			}
			Assert::IsTrue(RepeatReuseRules::Compare(Key(5, false), false, Key(5, true)).refusal == Refusal::NoCache);
		}

		TEST_METHOD(PreOutputParameterChangeRefusesOutputHooksDoNot)
		{
			Policy p = OnPolicy();
			p.NoteFullRender(Policy::Complete(Key(5, false), VpTemporal{}, Hooks(1)), true);
			// LED compensation ramping on the repeat refresh: the cached picture is stale.
			Assert::IsTrue(p.Decide(Policy::Complete(Key(5, true), VpTemporal{}, Hooks(2)), true, 1.0).refusal ==
				Refusal::HookStateChanged);
			// An OUTPUT-stage parameter (the sigma-delta's static_refresh) changes every refresh anyway.
			std::vector<HookInfo> h = Hooks(1);
			h[3].value = 100;
			Assert::IsTrue(p.Decide(Policy::Complete(Key(5, true), VpTemporal{}, h), true, 1.0).reuse);
			// The LED measurement (picture-neutral) present on the first present only: no effect.
			std::vector<HookInfo> noLed = Hooks(1);
			noLed.erase(noLed.begin() + 2);
			Assert::IsTrue(p.Decide(Policy::Complete(Key(5, true), VpTemporal{}, noLed), true, 1.0).reuse);
		}

		TEST_METHOD(OpaqueHookNeverReuses)
		{
			Policy p = OnPolicy();
			p.NoteFullRender(Policy::Complete(Key(5, false), VpTemporal{}, Hooks(1, true)), true);
			Assert::IsTrue(p.Decide(Policy::Complete(Key(5, true), VpTemporal{}, Hooks(1, true)), true, 1.0).refusal ==
				Refusal::HookStateChanged);
		}

		TEST_METHOD(VpTemporalStagesRefuse)
		{
			for (int i = 0; i < 8; ++i)
			{
				VpTemporal t;
				bool* flags[] = { &t.phasePicture, &t.dejudderExtras, &t.crossfade, &t.startupHoldBlack, &t.calibrationPattern,
					&t.deinterlacing, &t.frameMixing, &t.peakDetection };
				*flags[i] = true;
				Policy p = OnPolicy();
				p.NoteFullRender(Policy::Complete(Key(5, false), VpTemporal{}, Hooks(1)), true);
				Assert::IsTrue(p.Decide(Policy::Complete(Key(5, true), t, Hooks(1)), true, 1.0).refusal == Refusal::TemporalState);
			}
		}

		TEST_METHOD(OffAndOriginalPathwayNeverReuse)
		{
			for (const Options o : { Options{ false, false }, Options{ true, true } })
			{
				Policy p;
				p.SetOptions(o);
				p.NoteFullRender(Policy::Complete(Key(5, false), VpTemporal{}, Hooks(1)), true);
				Assert::IsTrue(p.Decide(Policy::Complete(Key(5, true), VpTemporal{}, Hooks(1)), true, 1.0).refusal == Refusal::Disabled);
				Assert::IsFalse(p.WantArm(Key(6, false), 1.0));
			}
		}

		TEST_METHOD(ArmsOnlyWhileRepeatsAreSeen)
		{
			Policy p = OnPolicy();
			Assert::IsFalse(p.WantArm(Key(1, false), 0.0));   // no repeat seen yet: no copy cost
			p.Decide(Key(1, true), false, 10.0);               // a repeat seen at t = 10 s
			Assert::IsTrue(p.WantArm(Key(2, false), 10.5));
			Assert::IsFalse(p.WantArm(Key(2, true), 10.5));    // never on a repeat
			Assert::IsFalse(p.WantArm(Key(3, false), 10.0 + kArmWindowSeconds + 0.01));   // cadence gone (e.g. 24p on 24 Hz)
			RepeatFrameKey temporal = Key(4, false);
			temporal.temporalRenderState = true;
			Assert::IsFalse(p.WantArm(temporal, 10.5));
		}

		TEST_METHOD(RefusedRepeatDoesNotReplaceTheReference)
		{
			Policy p = OnPolicy();
			p.NoteFullRender(Policy::Complete(Key(5, false), VpTemporal{}, Hooks(1)), true);
			p.NoteFullRender(Policy::Complete(Key(5, true), VpTemporal{}, Hooks(2)), false);   // a refused repeat, rendered in full
			Assert::IsTrue(p.Decide(Policy::Complete(Key(5, true), VpTemporal{}, Hooks(1)), true, 1.0).reuse);
		}

		TEST_METHOD(DrawFailureIsCountedAndVisible)
		{
			Policy p = OnPolicy();
			p.NoteFullRender(Policy::Complete(Key(5, false), VpTemporal{}, Hooks(1)), true);
			Assert::IsTrue(p.Decide(Policy::Complete(Key(5, true), VpTemporal{}, Hooks(1)), true, 1.0).reuse);
			p.NoteDrawFailed();
			Assert::AreEqual(uint64_t{ 0 }, p.GetStats().reused);
			Assert::IsTrue(p.GetStats().Line().find("reuse draw failed") != std::string::npos);
		}

		TEST_METHOD(CadenceSimulationCountsRepeats)
		{
			// 24-in-30 (2:2:2:2:... with one repeat in 5 refreshes) and 30-in-60 (every second refresh a
			// repeat), derived from the pattern - no fixed rate lists in the policy itself.
			struct Pattern { int refreshes; int unique; };
			for (const Pattern pat : { Pattern{ 5, 4 }, Pattern{ 2, 1 } })
			{
				Policy p = OnPolicy();
				uint64_t sequence = 0, armedFor = 0;
				int repeats = 0;
				double t = 0.0;
				for (int cycle = 0; cycle < 60; ++cycle)
					for (int r = 0; r < pat.refreshes; ++r)
					{
						t += 1.0 / 60.0;
						const bool repeat = r >= pat.unique;
						if (!repeat)
							++sequence;
						const RepeatFrameKey k = Policy::Complete(Key(sequence, repeat), VpTemporal{}, Hooks(1));
						if (!repeat && p.WantArm(k, t))
							armedFor = sequence;
						const Decision d = p.Decide(k, armedFor == sequence, t);
						if (repeat)
						{
							++repeats;
							// Only the very first repeat finds no cache (nothing was armed before a repeat was seen).
							Assert::IsTrue(d.reuse || repeats == 1);
						}
						else
							p.NoteFullRender(k, armedFor == sequence);
					}
				Assert::AreEqual(static_cast<uint64_t>(repeats), p.GetStats().repeats);
				Assert::IsTrue(p.GetStats().reused + 1 >= p.GetStats().repeats);
			}
		}

		TEST_METHOD(UpstreamDefaultNeverReuses)
		{
			struct FakeFrame {};
			NeverReuse<FakeFrame> upstream;
			FakeFrame f;
			Assert::IsFalse(upstream.TryDrawRepeat(Key(5, true), f));
			Assert::AreEqual(uint64_t{ 1 }, upstream.GetStats().repeats);
			Assert::AreEqual(uint64_t{ 0 }, upstream.GetStats().reused);
		}

		TEST_METHOD(SignatureIsExactOnFloats)
		{
			const float a = 1.0f, b = 1.0000001f;
			Assert::AreNotEqual(Signature().Add(a).Value(), Signature().Add(b).Value());
			Assert::AreEqual(Signature().Add(a).Add(std::string("x")).Value(), Signature().Add(a).Add(std::string("x")).Value());
		}
	};
}
