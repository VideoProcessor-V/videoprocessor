// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// M2 P5a-5: the matcher estimate's K4 pre-estimate hook (VectorEngine::Estimate) and IVectorEngine
// (vpv.vector_engine.v1) answered from today's state. Pure CPU: a fake matcher records every call the engine makes
// on it, so the tests pin that with no hook - and with a hook that binds nothing (camera guidance all off) - the
// matcher sees exactly the call sequence it saw before P5a-5 (its Estimate, nothing bound before it). No GPU.
// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

#include <VP-V/modules/motion/estimation/VectorEngine.h>
#include <VP-V/modules/motion/estimation/VectorEngineExtension.h>
#include <VP-V/modules/motion/estimation/VectorEngineInterface.h>

#include <memory>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace VpMotion;

namespace VectorEngineHookTest
{
	namespace
	{
		// The matcher surface the engine's Estimate touches (MotionWarp's names and shapes). `calls` records the calls
		// that change what the matcher does (bind, estimate); `reads` the read-only queries.
		struct FakeMatcher
		{
			struct CameraGuidanceUse
			{
				bool modelGlobal = false, staticFromModel = false, matcherSkipped = false, narrowPass = false, e1Pass = false,
					fadeNormalised = false;
				bool narrowDroppedEngine = false;
				const char* rejected = "";
				const char* gpuNotApplied = "";
			};
			std::vector<std::string>* calls = nullptr;
			int reads = 0;
			CameraGuidanceUse use;
			pl_tex counts = reinterpret_cast<pl_tex>(0x1);   // never dereferenced
			void SetCameraGuidance(std::shared_ptr<const CameraModel::GuidanceFrame> g) { calls->push_back(g ? "bind" : "bind-null"); }
			CameraGuidanceUse CameraGuidanceUsed() const { ++const_cast<FakeMatcher*>(this)->reads; return use; }
			pl_tex CameraGuidanceCounts() const { ++const_cast<FakeMatcher*>(this)->reads; return counts; }
		};

		struct RecordingHook : IPreEstimateHook
		{
			std::vector<std::string>* calls = nullptr;
			bool bind = false;
			EstimateContext ctx;
			uint64_t afterSeq = 0;
			bool afterUsed = true;
			pl_tex afterCounts = reinterpret_cast<pl_tex>(0x2);
			void BeforeEstimate(const EstimateContext& c, IGuidanceSink& sink) override
			{
				calls->push_back("before");
				ctx = c;
				if (bind)
					sink.SetCameraGuidance(std::make_shared<CameraModel::GuidanceFrame>());
			}
			void AfterEstimate(uint64_t pairSeq, bool used, pl_tex countsTex) override
			{
				calls->push_back("after");
				afterSeq = pairSeq;
				afterUsed = used;
				afterCounts = countsTex;
			}
		};

		struct FakeView : IMatcherView
		{
			pl_tex field = nullptr;
			uint64_t from = 0;
			bool global = false;
			pl_tex LiveField() const override { return field; }
			uint64_t LiveFieldFrom() const override { return from; }
			bool MatcherGlobalFromEngine() const override { return global; }
		};

		// Runs one estimate the way RunDejudderEstimate does and returns the matcher-side call sequence.
		std::vector<std::string> RunOne(VectorEngine& engine, FakeMatcher& matcher, bool hookable, bool result = true,
			uint64_t a = 5, uint64_t b = 6, bool ahead = false)
		{
			std::vector<std::string> calls;
			matcher.calls = &calls;
			const bool done = engine.Estimate(a, b, ahead, 24.0, hookable, matcher, [&]() { calls.push_back("estimate"); return result; });
			Assert::AreEqual(result, done, L"the estimate's result passes through unchanged");
			return calls;
		}
	}

	TEST_CLASS(VectorEngineHookTests)
	{
	public:
		// Today (no hook registered; always so with camera guidance off): the matcher's Estimate only.
		TEST_METHOD(NullHookLeavesTheMatcherCallSequenceUnchanged)
		{
			VectorEngine engine;
			FakeMatcher matcher;
			Assert::IsFalse(engine.PreEstimateHooked());
			const std::vector<std::string> calls = RunOne(engine, matcher, true);
			Assert::AreEqual<size_t>(1, calls.size());
			Assert::AreEqual(std::string("estimate"), calls[0]);
			Assert::AreEqual(0, matcher.reads, L"no guidance query without a hook");
			Assert::AreEqual<size_t>(1, RunOne(engine, matcher, true, false).size(), L"a failed estimate: the same");
		}

		// A hook that binds nothing (camera guidance all off): the matcher still sees only its Estimate, in the same
		// place; the hook is told nothing was used and gets no counts texture.
		TEST_METHOD(AllOffHookLeavesTheMatcherCallSequenceUnchanged)
		{
			VectorEngine engine;
			FakeMatcher matcher;
			RecordingHook hook;
			std::vector<std::string> hookCalls;
			hook.calls = &hookCalls;
			engine.SetPreEstimateHook(&hook);
			Assert::IsTrue(engine.PreEstimateHooked());
			const std::vector<std::string> calls = RunOne(engine, matcher, true);
			Assert::AreEqual<size_t>(1, calls.size());
			Assert::AreEqual(std::string("estimate"), calls[0], L"nothing bound before the estimate");
			Assert::AreEqual<size_t>(2, hookCalls.size());
			Assert::AreEqual(std::string("before"), hookCalls[0]);
			Assert::AreEqual(std::string("after"), hookCalls[1]);
			Assert::IsFalse(hook.afterUsed);
			Assert::IsTrue(hook.afterCounts == nullptr, L"no counts texture when no guidance ran");
			Assert::AreEqual<uint64_t>(6, hook.afterSeq);
			engine.SetPreEstimateHook(nullptr);
			Assert::IsFalse(engine.PreEstimateHooked());
		}

		// The warm-up's dummy A -> A estimate never reaches the hook.
		TEST_METHOD(TheWarmUpEstimateIsNeverHooked)
		{
			VectorEngine engine;
			FakeMatcher matcher;
			RecordingHook hook;
			std::vector<std::string> hookCalls;
			hook.calls = &hookCalls;
			hook.bind = true;
			engine.SetPreEstimateHook(&hook);
			const std::vector<std::string> calls = RunOne(engine, matcher, false, true, 0, 0);
			Assert::AreEqual<size_t>(1, calls.size());
			Assert::IsTrue(hookCalls.empty());
			Assert::AreEqual(0, matcher.reads);
		}

		// The guidance the hook binds reaches the matcher (1:1 SetCameraGuidance) right before its Estimate, with the
		// pair's context; what the guided Estimate did comes back right after.
		TEST_METHOD(BoundGuidanceReachesTheMatcherRightBeforeItsEstimate)
		{
			VectorEngine engine;
			FakeMatcher matcher;
			matcher.use.narrowPass = true;
			RecordingHook hook;
			std::vector<std::string> hookCalls;
			hook.calls = &hookCalls;
			hook.bind = true;
			engine.SetPreEstimateHook(&hook);
			const std::vector<std::string> calls = RunOne(engine, matcher, true, true, 10, 12, true);
			Assert::AreEqual<size_t>(2, calls.size());
			Assert::AreEqual(std::string("bind"), calls[0]);
			Assert::AreEqual(std::string("estimate"), calls[1]);
			Assert::AreEqual<uint64_t>(12, hook.ctx.pairSeq, L"pairSeq = the pair's later frame");
			Assert::IsTrue(hook.ctx.ahead);
			Assert::AreEqual(2.0 / 24.0, hook.ctx.nextPairSeconds, 1e-12, L"two source periods at 24 Hz");
			Assert::IsTrue(hook.afterUsed);
			Assert::IsTrue(hook.afterCounts == matcher.counts, L"the counts texture of the guided passes");
			matcher.use.rejected = "stale guide";
			RunOne(engine, matcher, true);
			Assert::IsFalse(hook.afterUsed, L"rejected guidance is not used");
			Assert::IsTrue(hook.afterCounts == nullptr);
		}

		// IVectorEngine with nothing estimated, or no matcher bound: every answer is the documented "absent" value.
		TEST_METHOD(TheEngineAnswersAbsentWithoutALiveField)
		{
			VectorEngine engine;
			VerifiedField field;
			GlobalVec global;
			FlowView flow;
			Assert::AreEqual<uint64_t>(0, engine.LatestVerifiedFieldSeq());
			Assert::IsFalse(engine.FieldOf(6, field));
			Assert::AreEqual(1.0f, engine.FieldScale());
			Assert::IsFalse(engine.GlobalFromEngine());
			Assert::IsFalse(engine.EngineGlobal(6, global));
			Assert::IsTrue(engine.PairSpansSlip(5, 6), L"unknown = a slip (conservative)");
			Assert::IsFalse(engine.FlowIsCpuResident());
			Assert::IsFalse(engine.FlowOf(6, flow));
			Assert::IsFalse(engine.RefinedFieldAvailable());
			uint64_t ticket = 7;
			Assert::IsTrue(engine.SubmitLongPair(nullptr, nullptr, nullptr, 1, 9, ticket) == LongPairResult::Unavailable);
			FakeMatcher matcher;
			RunOne(engine, matcher, true);   // estimated, but no matcher view bound
			Assert::IsFalse(engine.FieldOf(6, field));
		}

		// The live matcher field of the last estimate, while the live outputs hold that pair in the current epoch.
		TEST_METHOD(TheLiveFieldIsTheVerifiedFieldUntilItIsReplacedOrInvalidated)
		{
			VectorEngine engine;
			FakeView view;
			pl_tex_t tex{};
			tex.params.w = 120;
			tex.params.h = 68;
			view.field = &tex;
			engine.BindMatcher(&view);
			FakeMatcher matcher;
			RunOne(engine, matcher, true, true, 5, 6);
			view.from = 5;
			VerifiedField field;
			Assert::IsTrue(engine.FieldOf(6, field));
			Assert::AreEqual<uint64_t>(6, engine.LatestVerifiedFieldSeq());
			Assert::AreEqual<uint64_t>(5, field.pairFrom);
			Assert::AreEqual<uint64_t>(6, field.pairTo);
			Assert::IsTrue(field.fwd == &tex);
			Assert::IsTrue(field.bwd == nullptr);
			Assert::AreEqual(120, field.gridW);
			Assert::AreEqual(68, field.gridH);
			Assert::AreEqual(16, field.blockSize);
			Assert::IsFalse(engine.FieldOf(5, field), L"only the live pair");
			Assert::IsFalse(engine.PairSpansSlip(5, 6));
			Assert::IsTrue(engine.PairSpansSlip(4, 6), L"before the first estimate of this epoch: unknown");
			view.from = 0;   // the live outputs are being replaced (or the matcher was released)
			Assert::IsFalse(engine.FieldOf(6, field));
			Assert::AreEqual<uint64_t>(0, engine.LatestVerifiedFieldSeq());
			view.from = 5;
			const uint64_t era = engine.FieldEra();
			engine.motionValidity.Invalidate(MotionValidity::Cause::Gap);
			Assert::AreNotEqual(era, engine.FieldEra(), L"an invalidation starts a new field era");
			Assert::IsFalse(engine.FieldOf(6, field), L"vectors of an older epoch are never served");
			Assert::IsTrue(engine.PairSpansSlip(5, 6));
			engine.BindMatcher(nullptr);
		}

		// The optical-flow engine's whole-frame motion of the pair, in source px; unusable (z >= 0.9) = absent.
		TEST_METHOD(TheEngineGlobalIsTheProducersWholeFrameMotionInSourcePixels)
		{
			VectorEngine engine;
			const float usable[4] = { 1.5f, -0.5f, 0.2f, 1.0f };
			engine.NoteEngineGlobal(5, 6, usable, 1);   // one halving: 16 source px per engine cell
			GlobalVec g;
			Assert::IsTrue(engine.EngineGlobal(6, g));
			Assert::IsTrue(g.valid);
			Assert::AreEqual(24.0, g.x, 1e-9);
			Assert::AreEqual(-8.0, g.y, 1e-9);
			Assert::IsFalse(engine.EngineGlobal(7, g), L"another pair");
			const float unclear[4] = { 1.5f, -0.5f, 0.95f, 1.0f };
			engine.NoteEngineGlobal(6, 7, unclear, 1);
			Assert::IsFalse(engine.EngineGlobal(7, g));
			engine.NoteEngineGlobal(7, 8, nullptr, 1);
			Assert::IsFalse(engine.EngineGlobal(8, g), L"no producer pair");
		}

		// Query("vpv.vector_engine.v1") answers the directory; a renderer's engine is found under its host key while it
		// exists.
		TEST_METHOD(TheDirectoryFindsEachRenderersEngineByHost)
		{
			int hostA = 0, hostB = 0;
			VectorEngineExtension extension;
			IVectorEngineDirectory* directory = static_cast<IVectorEngineDirectory*>(extension.Query(kVectorEngineQueryId));
			Assert::IsTrue(directory == &VectorEngine::Directory());
			Assert::IsTrue(extension.Query("vpv.camera.v1") == nullptr);
			Assert::IsTrue(extension.Query(nullptr) == nullptr);
			{
				VectorEngine a(&hostA);
				Assert::IsTrue(directory->EngineFor(&hostA) == static_cast<IVectorEngine*>(&a));
				Assert::IsTrue(directory->EngineFor(&hostB) == nullptr);
				{
					VectorEngine b(&hostB);
					Assert::IsTrue(directory->EngineFor(&hostB) == static_cast<IVectorEngine*>(&b));
				}
				Assert::IsTrue(directory->EngineFor(&hostB) == nullptr, L"unregistered at destruction");
			}
			Assert::IsTrue(directory->EngineFor(&hostA) == nullptr);
			VectorEngine unbound;
			Assert::IsTrue(directory->EngineFor(nullptr) == nullptr, L"a default-constructed engine is not registered");
		}
	};
}
