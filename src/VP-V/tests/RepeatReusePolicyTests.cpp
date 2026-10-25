#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// WP-24 captured-repeat render reuse (2026-10-24 draft, feat/repeat-reuse). CPU only: the upstream
// comparison rules, VP's policy (switch, Original pathway, arming window, temporal stages, hook
// state incl. opaque hooks), the visible counters, and a simulated 24-in-30 / 30-in-60 cadence.

#include <VP-V/modules/output/presentation/RepeatReuseAdapter.h>
#include <VP-V/modules/output/presentation/TemporalShaders.h>

#include <cstring>
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
				Refusal::HookStateOpaque);
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

		// Audit H1: arm only what can be reused.
		TEST_METHOD(ArmsOnlyWhenTheQueueHoldsAReusableRepeat)
		{
			Policy p = OnPolicy();
			RepeatFrameKey k = Policy::Complete(Key(1, false), VpTemporal{}, Hooks(1));
			Assert::IsFalse(p.WantArm(k, 0.0));                 // the queue holds no repeat of it
			k.nextIsRepeat = true;
			Assert::IsTrue(p.WantArm(k, 0.0));
			RepeatFrameKey repeat = k;
			repeat.cadenceRepeat = true;
			Assert::IsFalse(p.WantArm(repeat, 0.0));            // never on a repeat
			RepeatFrameKey opaque = Policy::Complete(Key(1, false), VpTemporal{}, Hooks(1, true));
			opaque.nextIsRepeat = true;
			Assert::IsFalse(p.WantArm(opaque, 0.0));            // e.g. AdaptiveDownscale active: no copy
			VpTemporal t;
			t.peakDetection = true;
			RepeatFrameKey temporal = Policy::Complete(Key(1, false), t, Hooks(1));
			temporal.nextIsRepeat = true;
			Assert::IsFalse(p.WantArm(temporal, 0.0));
		}

		TEST_METHOD(PersistentRefusalsBackOffArming)
		{
			Policy p = OnPolicy();
			VpTemporal t;
			t.crossfade = true;
			for (unsigned i = 0; i < kPersistentRefusalsToStop; ++i)
			{
				p.NoteFullRender(Policy::Complete(Key(10 + i, false), VpTemporal{}, Hooks(1)), true);
				p.Decide(Policy::Complete(Key(10 + i, true), t, Hooks(1)), true, 5.0);
			}
			RepeatFrameKey next = Policy::Complete(Key(20, false), VpTemporal{}, Hooks(1));
			next.nextIsRepeat = true;
			Assert::IsFalse(p.WantArm(next, 5.0 + kArmBackoffSeconds / 2));
			Assert::IsTrue(p.WantArm(next, 5.0 + kArmBackoffSeconds + 0.01));
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
				int armedCount = 0;
				int repeats = 0;
				double t = 0.0;
				for (int cycle = 0; cycle < 60; ++cycle)
					for (int r = 0; r < pat.refreshes; ++r)
					{
						t += 1.0 / 60.0;
						const bool repeat = r >= pat.unique;
						if (!repeat)
							++sequence;
						RepeatFrameKey k = Policy::Complete(Key(sequence, repeat), VpTemporal{}, Hooks(1));
						k.nextIsRepeat = !repeat && r + 1 < pat.refreshes && r + 1 >= pat.unique;   // the queue's view
						if (!repeat && p.WantArm(k, t))
						{
							armedFor = sequence;
							++armedCount;
						}
						const Decision d = p.Decide(k, armedFor == sequence, t);
						if (repeat)
						{
							++repeats;
							// The queue announced every repeat, so every one finds its cache.
							Assert::IsTrue(d.reuse);
						}
						else
							p.NoteFullRender(k, armedFor == sequence);
					}
				Assert::AreEqual(static_cast<uint64_t>(repeats), p.GetStats().repeats);
				Assert::AreEqual(p.GetStats().repeats, p.GetStats().reused);
				Assert::AreEqual(p.GetStats().repeats, static_cast<uint64_t>(armedCount));   // one copy per reused repeat, no waste
			}
		}

		TEST_METHOD(UpstreamDefaultNeverReuses)
		{
			NeverReuse upstream;
			std::vector<const pl_hook*> hooks;
			upstream.PrepareFirstPresent(Key(5, true), hooks);
			Assert::IsTrue(hooks.empty());   // never arms a capture
			pl_frame destination{};
			pl_render_params params = pl_render_default_params;
			Assert::IsFalse(upstream.TryDrawRepeat(Key(5, true), nullptr, destination, params));
			Assert::AreEqual(uint64_t{ 1 }, upstream.GetStats().repeats);
			Assert::AreEqual(uint64_t{ 0 }, upstream.GetStats().reused);
		}

		TEST_METHOD(KeyFromHashesImageOverlaysAndFlagsTemporalParams)
		{
			pl_frame image{}, target{};
			image.crop = { 0, 0, 3840, 2160 };
			target.crop = { 0, 0, 2560, 1600 };
			pl_render_params params = pl_render_default_params;
			params.peak_detect_params = nullptr;
			params.deband_params = nullptr;
			params.frame_mixer = nullptr;
			const RepeatFrameKey a = KeyFrom(5, true, false, 1, 1, image, target, params, 7);
			Assert::IsFalse(a.temporalRenderState);
			// A target overlay (drawn again every refresh) does not change the key.
			pl_overlay ov{};
			target.overlays = &ov;
			target.num_overlays = 1;
			Assert::AreEqual(a.overlays, KeyFrom(5, true, false, 1, 1, image, target, params, 7).overlays);
			// An image overlay (baked into the cached picture) does.
			image.overlays = &ov;
			image.num_overlays = 1;
			Assert::AreNotEqual(a.overlays, KeyFrom(5, true, false, 1, 1, image, target, params, 7).overlays);
			// The LUT by generation, not by pointer.
			Assert::AreNotEqual(a.colour, KeyFrom(5, true, false, 1, 1, pl_frame{ image }, target, params, 8).colour);
			// Temporal per-render state.
			pl_deband_params deband = pl_deband_default_params;
			deband.grain = 4.0f;
			params.deband_params = &deband;
			Assert::IsTrue(KeyFrom(5, true, false, 1, 1, image, target, params, 7).temporalRenderState);
			params.deband_params = nullptr;
			params.peak_detect_params = &pl_peak_detect_default_params;
			Assert::IsTrue(KeyFrom(5, true, false, 1, 1, image, target, params, 7).temporalRenderState);
		}

		// Audit M1: a hook with an OUTPUT pass and an earlier stage would lose its OUTPUT pass in the
		// OUTPUT-only reuse draw - it makes the key opaque (with its reason on the status line).
		TEST_METHOD(MultiStageHookWithOutputIsOpaque)
		{
			pl_hook multi{}, outputOnly{}, early{};
			multi.stages = static_cast<pl_hook_stage>(PL_HOOK_MAIN | PL_HOOK_OUTPUT);
			outputOnly.stages = PL_HOOK_OUTPUT;
			early.stages = PL_HOOK_PRE_OUTPUT;
			const std::vector<KnownHook> known = { { &early, HookKind::Static, 0, "test static" } };
			const auto infos = ClassifyHooks({ &multi, &outputOnly, &early }, known, nullptr);
			Assert::IsTrue(infos[0].kind == HookKind::Opaque);
			Assert::IsTrue(infos[0].beforeOutput);
			Assert::IsTrue(infos[1].kind == HookKind::Opaque);   // unknown native, but OUTPUT-only: outside the key
			Assert::IsFalse(infos[1].beforeOutput);
			Assert::IsTrue(infos[2].kind == HookKind::Static);
			const RepeatFrameKey k = Policy::Complete(Key(5, true), VpTemporal{}, infos);
			Assert::IsTrue(k.hookStateOpaque);
			Assert::IsTrue(k.opaqueDetail.find("OUTPUT pass") != std::string::npos);
			// Without the multi-stage hook the key is known.
			Assert::IsFalse(Policy::Complete(Key(5, true), VpTemporal{}, ClassifyHooks({ &outputOnly, &early }, known, nullptr)).hookStateOpaque);
		}

		// Audit M2: mpv shaders with per-render state are never static.
		TEST_METHOD(TemporalShaderTextIsRecognised)
		{
			auto t = [](const char* text) { return ShaderTextIsTemporal(text, std::strlen(text)); };
			Assert::IsTrue(t("//!HOOK MAIN
vec4 hook() { return HOOKED_tex(HOOKED_pos) + vec4(random); }"));
			Assert::IsTrue(t("//!HOOK MAIN
vec4 hook() { float f = float(frame % 2); return vec4(f); }"));
			Assert::IsTrue(t("//!BUFFER state
//!VAR float x
//!HOOK MAIN
vec4 hook() { return vec4(0); }"));
			Assert::IsFalse(t("//!HOOK MAIN
// frame and random only in a comment
vec4 hook() { return HOOKED_tex(HOOKED_pos); }"));
			Assert::IsFalse(t("//!HOOK MAIN
vec4 hook() { float framed = 1.0; return vec4(framed); }"));   // not the identifier
			Assert::IsFalse(t("/* random */ //!HOOK MAIN
vec4 hook() { return vec4(1); }"));
		}

		TEST_METHOD(TemporalRegistryMakesTheHookOpaqueAndMovesTheSetGeneration)
		{
			static const char text[] = "//!HOOK MAIN
vec4 hook() { return vec4(random); }";
			pl_hook mpvLike{};
			mpvLike.stages = PL_HOOK_MAIN;
			int fakeFn = 0;
			mpvLike.hook = reinterpret_cast<pl_hook_res (*)(void*, const pl_hook_params*)>(&fakeFn);
			auto& reg = TemporalShaderRegistry::Global();
			const uint64_t before = reg.Generation();
			reg.Note(&mpvLike, text, std::strlen(text));
			Assert::IsTrue(reg.Generation() > before);
			const auto infos = ClassifyHooks({ &mpvLike }, {}, reinterpret_cast<const void*>(mpvLike.hook));
			Assert::IsTrue(infos[0].kind == HookKind::Opaque);
			reg.Forget(&mpvLike);
			const auto plain = ClassifyHooks({ &mpvLike }, {}, reinterpret_cast<const void*>(mpvLike.hook));
			Assert::IsTrue(plain[0].kind == HookKind::Parameters);
			// The set generation is part of the hook state (L4).
			Assert::AreNotEqual(Policy::Complete(Key(5, true), VpTemporal{}, infos).hookState,
				Policy::Complete(Key(5, true), VpTemporal{}, plain).hookState);
		}

		// Audit H2: VP state outside the hook list joins the key.
		TEST_METHOD(VpExtensionCoversRebaseLutSubtitlesAndFallbacks)
		{
			const VpExtension base;
			VpExtension e = base;
			e.rebaseFactor = 0.5;   // a re-base pause ended on a repeat
			Assert::AreNotEqual(base.Signature(), e.Signature());
			e = base; e.rebaseKernel = 2;
			Assert::AreNotEqual(base.Signature(), e.Signature());
			e = base; e.lutSignature = 9;   // LUT reloaded in place
			Assert::AreNotEqual(base.Signature(), e.Signature());
			e = base; e.subtitleSignature = 3;   // donor flipped / glyph mapping moved
			Assert::AreNotEqual(base.Signature(), e.Signature());
			e = base; e.fallbacksSignature = 4;   // a sticky shed engaged
			Assert::AreNotEqual(base.Signature(), e.Signature());
			RepeatFrameKey cached = Key(5, false), now = Key(5, true);
			cached.extension = base.Signature();
			now.extension = e.Signature();
			Assert::IsTrue(RepeatReuseRules::Compare(cached, true, now).refusal == Refusal::ExtensionChanged);
		}

		TEST_METHOD(SignatureIsExactOnFloats)
		{
			const float a = 1.0f, b = 1.0000001f;
			Assert::AreNotEqual(Signature().Add(a).Value(), Signature().Add(b).Value());
			Assert::AreEqual(Signature().Add(a).Add(std::string("x")).Value(), Signature().Add(a).Add(std::string("x")).Value());
		}
	};
}
