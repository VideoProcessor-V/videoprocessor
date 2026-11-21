// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// GF-P4 (test spec lead20\testspec-P4.md): pins of the P4 production fixes that are source facts or seams of their own.
// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <VP-V/modules/motion/estimation/P2CpuForm.h>   // FIRST VP include on purpose: the header must stand alone
#include <VP-V/modules/motion/estimation/MotionValidity.h>

#include "CppUnitTest.h"

#include "../../../../tests/SettingsTestSupport.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace GfP4MotionTest
{
	namespace
	{
		// A source file of the tree, found from __FILE__ (SettingsTestSupport::RepoRoot), line ends normalised to "\n".
		std::string ReadSource(const char* relativePath)
		{
			std::string text = SettingsTestSupport::ReadFile(SettingsTestSupport::RepoRoot() + relativePath);
			std::string out;
			out.reserve(text.size());
			for (const char c : text)
				if (c != '\r')
					out.push_back(c);
			return out;
		}

		// From `startMarker` to the next line that is exactly `endLine` (the body of a function whose closing line has no indent).
		// Empty when a marker is not found.
		std::string BodyBetween(const std::string& text, const std::string& startMarker, const std::string& endLine)
		{
			const size_t begin = text.find(startMarker);
			if (begin == std::string::npos)
				return std::string();
			const std::string needle = "\n" + endLine + "\n";
			const size_t end = text.find(needle, begin);
			if (end == std::string::npos)
				return std::string();
			return text.substr(begin, end - begin);
		}

		size_t CountOf(const std::string& text, const std::string& part)
		{
			size_t n = 0;
			for (size_t at = text.find(part); at != std::string::npos; at = text.find(part, at + part.size()))
				++n;
			return n;
		}

		// The same text with every run of white space collapsed to one space (a statement broken over lines reads as one).
		std::string Collapsed(const std::string& text)
		{
			std::string out;
			bool space = false;
			for (const char c : text)
			{
				if (c == ' ' || c == '\t' || c == '\n')
				{
					space = true;
					continue;
				}
				if (space && !out.empty())
					out.push_back(' ');
				space = false;
				out.push_back(c);
			}
			return out;
		}

		// The process-wide confirmed-cut seam, set for one scope and restored (the seam is shared by every test).
		struct ConfirmedCutScope
		{
			::MotionValidity::State::ConfirmedCutFn before;
			explicit ConfirmedCutScope(::MotionValidity::State::ConfirmedCutFn fn)
				: before(::MotionValidity::State::ConfirmedCutReader().load())
			{
				::MotionValidity::State::ConfirmedCutReader().store(fn);
			}
			~ConfirmedCutScope() { ::MotionValidity::State::ConfirmedCutReader().store(before); }
			ConfirmedCutScope(const ConfirmedCutScope&) = delete;
			ConfirmedCutScope& operator=(const ConfirmedCutScope&) = delete;
		};

		bool CutAtTen(uint64_t seq) { return seq == 10; }

		// Every pure virtual answered "absent"; P2RecordEndingAt declines with a fixed reason and counts its calls, and the
		// P2 CPU form's read count is kept (the same fake shape as P2CpuFormHeaderTests).
		struct CountingEngine : VpMotion::IVectorEngine
		{
			mutable int p2Calls = 0;
			mutable int served = 0;
			mutable int declined = 0;
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
			bool P2RecordEndingAt(uint64_t, VpMotion::P2PairRecord&, std::string& why) const override
			{
				++p2Calls;
				why = "p2 cpu form: fake decline";
				return false;
			}
			void NoteP2CpuFormRead(bool ok) const override { (ok ? served : declined) += 1; }
		};

		// A directory that only knows EngineFor: ReadP2Record is the interface's default.
		struct FakeDirectory : VpMotion::IVectorEngineDirectory
		{
			VpMotion::IVectorEngine* engine = nullptr;
			mutable int asked = 0;
			VpMotion::IVectorEngine* EngineFor(const void*) override
			{
				++asked;
				return engine;
			}
		};
	}

	TEST_CLASS(GfP4MotionTests)
	{
	public:
		// GF-P4 MUST-11 (source pin): RunDejudderEstimate asks the pair test ONCE (PairServes counts the confirmed-cut refusal and
		// the unjudged pair there), and the on-demand gate reuses that answer; no second PairValidAt in the function.
		TEST_METHOD(RunDejudderEstimateAsksPairServesOnce)
		{
			const std::string text = ReadSource("src\\VP-V\\modules\\motion\\estimation\\VectorEngineEstimate.inl");
			Assert::IsFalse(text.empty(), L"VectorEngineEstimate.inl found from __FILE__");
			const std::string body = BodyBetween(text, "bool LibplaceboVideoRenderer::Impl::RunDejudderEstimate(", "}");
			Assert::IsFalse(body.empty(), L"the body of RunDejudderEstimate was found");
			Assert::AreEqual<size_t>(1, CountOf(body, "PairServes("), L"exactly one PairServes( ask per estimate");
			Assert::AreEqual<size_t>(0, CountOf(body, "PairValidAt("), L"no separate PairValidAt( ask");

			const size_t gate = body.find("const bool onDemandAllowed");
			Assert::IsTrue(gate != std::string::npos, L"the on-demand gate is there");
			const size_t semicolon = body.find(';', gate);
			Assert::IsTrue(semicolon != std::string::npos);
			const std::string statement = Collapsed(body.substr(gate, semicolon - gate));
			const std::string tail = ": pairServes";
			Assert::IsTrue(statement.size() > tail.size() && statement.compare(statement.size() - tail.size(), tail.size(), tail) == 0,
				L"the adjacent branch of onDemandAllowed is the answer already asked (pairServes)");
		}

		// GF-P4 MUST-11 (behaviour): one PairServes ask over a pair whose end is a confirmed cut is refused and counted ONCE.
		TEST_METHOD(PairServesCountsOneRefusalPerAsk)
		{
			const ConfirmedCutScope scope(&CutAtTen);
			::MotionValidity::State state;
			state.NoteKept(9);
			state.NoteKept(10);
			Assert::AreEqual(0ull, static_cast<unsigned long long>(state.ConfirmedCutRefusals()));
			const bool served = state.PairServes(9, 10, state.EpochOfLastKept(), state.Epoch());
			Assert::IsFalse(served, L"the pair ending on a confirmed cut is not served");
			Assert::AreEqual(1ull, static_cast<unsigned long long>(state.ConfirmedCutRefusals()), L"one ask, one counted refusal");
		}

		// GF-P4 MUST-12 (source pin): the U3 private-copy count is made in ONE place, inside Producer::LumaAtTarget after the
		// producer's own HalfLuma8 decode (a shared P1 read is not a private copy), and every LumaAtTarget caller names its site.
		// The behavioural U3 count is covered by the compute-once detectors in the union / real-clip run.
		TEST_METHOD(U3PrivateCountOnlyInsideTheHalfLuma8Branch)
		{
			const std::string text = ReadSource("src\\VP-V\\modules\\motion\\estimation\\MotionAheadProducer.cpp");
			Assert::IsFalse(text.empty(), L"MotionAheadProducer.cpp found from __FILE__");
			Assert::AreEqual<size_t>(1, CountOf(text, "VPV_COMPUTE_ONCE_PRIVATE_AT(\"U3\","), L"one U3 private count in the file");

			const std::string body = BodyBetween(text, "bool Producer::LumaAtTarget(", "\t}");
			Assert::IsFalse(body.empty(), L"the body of Producer::LumaAtTarget was found");
			const size_t count = body.find("VPV_COMPUTE_ONCE_PRIVATE_AT(\"U3\",");
			Assert::IsTrue(count != std::string::npos, L"the one U3 count lies inside LumaAtTarget");
			const size_t decode = body.find("HalfLuma8(luma, buffer");
			Assert::IsTrue(decode != std::string::npos, L"the producer's own decode is in LumaAtTarget");
			Assert::IsTrue(decode < count, L"the count comes after the HalfLuma8 decode");

			// The calls (not the definition): in the file's order the job, then the two request calls.
			std::vector<std::string> calls;
			size_t at = 0;
			while ((at = text.find("LumaAtTarget(", at)) != std::string::npos)
			{
				const size_t lineBegin = text.rfind('\n', at) == std::string::npos ? 0 : text.rfind('\n', at) + 1;
				const size_t lineEnd = text.find('\n', at);
				const std::string line = text.substr(lineBegin, lineEnd == std::string::npos ? std::string::npos : lineEnd - lineBegin);
				if (line.find("Producer::LumaAtTarget(") == std::string::npos)
					calls.push_back(line);
				at += 13;
			}
			Assert::AreEqual<size_t>(3, calls.size(), L"three LumaAtTarget calls");
			Assert::IsTrue(calls[0].find("\"motion_ahead\")") != std::string::npos, L"the job path names motion_ahead");
			Assert::IsTrue(calls[1].find("\"motion_ahead_request\")") != std::string::npos, L"the request's to-frame names motion_ahead_request");
			Assert::IsTrue(calls[2].find("\"motion_ahead_request\")") != std::string::npos, L"the request's from-frame names motion_ahead_request");
		}

		// GF-P4 S4 (source pin): the product.motion_warp fallback row is released on the settled-off branch of the de-judder policy
		// and in ReleaseDejudder, never left engaged when no estimate (and so no matcher pyramid of its own) runs.
		TEST_METHOD(MotionWarpReleasedOnSettledOffAndRelease)
		{
			const char* released = "Released(\"product.motion_warp\")";

			const std::string runner = ReadSource("src\\VP-V\\modules\\motion\\consumers\\DejudderPolicyRunner.inl");
			Assert::IsFalse(runner.empty(), L"DejudderPolicyRunner.inl found from __FILE__");
			const size_t branch = runner.find("outcome == VpMotion::DecisionOutcome::ReleaseBranch");
			Assert::IsTrue(branch != std::string::npos, L"the ReleaseBranch outcome is tested");
			const size_t release = runner.find(released, branch);
			Assert::IsTrue(release != std::string::npos, L"the row is released after the ReleaseBranch test");
			const size_t pairCheck = runner.find("outcome != VpMotion::DecisionOutcome::PairCheck", branch);
			Assert::IsTrue(pairCheck != std::string::npos, L"the PairCheck early return follows");
			Assert::IsTrue(release < pairCheck, L"released BEFORE the early return for every outcome but PairCheck");

			const std::string timeline = ReadSource("src\\VP-V\\modules\\motion\\consumers\\PhaseTimeline.inl");
			Assert::IsFalse(timeline.empty(), L"PhaseTimeline.inl found from __FILE__");
			const std::string body = BodyBetween(timeline, "void LibplaceboVideoRenderer::Impl::ReleaseDejudder()", "}");
			Assert::IsFalse(body.empty(), L"the body of ReleaseDejudder was found");
			const size_t reset = body.find("dejudder.reset();");
			Assert::IsTrue(reset != std::string::npos, L"the matcher is reset in ReleaseDejudder");
			Assert::IsTrue(body.find(released, reset) != std::string::npos, L"the row is released after dejudder.reset();");
		}

		// GF-P4 S4 (GC S-3), SOURCE SCAN ONLY (the renderer's Impl cannot be driven in a unit test): with de-judder off the policy
		// decision is the ReleaseBranch outcome on every frame, the row is released there UNCONDITIONALLY (nothing between the
		// outcome test and the release), and the only site that can (re-)engage product.motion_warp is RunDejudderEstimate, which the
		// policy runner never reaches after a non-PairCheck outcome. So the row stays released over the following frames.
		TEST_METHOD(MotionWarpStaysReleasedAfterDejudderOff)
		{
			const std::string runner = ReadSource("src\\VP-V\\modules\\motion\\consumers\\DejudderPolicyRunner.inl");
			const std::string timeline = ReadSource("src\\VP-V\\modules\\motion\\consumers\\PhaseTimeline.inl");
			const std::string estimate = ReadSource("src\\VP-V\\modules\\motion\\estimation\\VectorEngineEstimate.inl");
			Assert::IsFalse(runner.empty() || timeline.empty() || estimate.empty(), L"the three sources are found from __FILE__");

			// The release is the very statement the ReleaseBranch test guards (no condition or block in between).
			const std::string guard = "outcome == VpMotion::DecisionOutcome::ReleaseBranch";
			const size_t branch = runner.find(guard);
			Assert::IsTrue(branch != std::string::npos, L"the ReleaseBranch outcome is tested");
			const size_t release = runner.find("Released(\"product.motion_warp\")", branch);
			Assert::IsTrue(release != std::string::npos);
			Assert::AreEqual(std::string(guard + ") SyncFallback(FallbackCatalog::"), Collapsed(runner.substr(branch, release - branch)),
				L"released unconditionally on the ReleaseBranch outcome");

			// Every non-PairCheck outcome returns, so RunDejudderEstimate (the only engage site) is not reached.
			const size_t pairCheck = runner.find("outcome != VpMotion::DecisionOutcome::PairCheck", release);
			Assert::IsTrue(pairCheck != std::string::npos);
			const size_t early = runner.find("return;", pairCheck);
			Assert::IsTrue(early != std::string::npos);
			Assert::AreEqual(std::string("outcome != VpMotion::DecisionOutcome::PairCheck)"), Collapsed(runner.substr(pairCheck, early - pairCheck)),
				L"a non-PairCheck outcome returns at once");

			// The engage form, ProductFallback("product.motion_warp", ...), exists only in RunDejudderEstimate.
			const std::string engage = "ProductFallback(\"product.motion_warp\"";
			Assert::AreEqual<size_t>(0, CountOf(runner, engage), L"the policy runner never engages the row");
			Assert::AreEqual<size_t>(0, CountOf(timeline, engage), L"ReleaseDejudder never engages the row");
			const std::string body = BodyBetween(estimate, "bool LibplaceboVideoRenderer::Impl::RunDejudderEstimate(", "}");
			Assert::AreEqual<size_t>(1, CountOf(body, engage), L"the one engage site is inside RunDejudderEstimate");
			Assert::AreEqual<size_t>(1, CountOf(estimate, engage), L"and nowhere else in the estimate file");
		}

		// GF-P4 S7 (a): the interface's DEFAULT ReadP2Record (a fake directory that only knows EngineFor) forwards to the engine's
		// P2RecordEndingAt once and counts the one decline; with no engine it answers "engine absent" and counts nothing.
		TEST_METHOD(FakeDirectoryDefaultReadUsesEngineFor)
		{
			int host = 0;
			CountingEngine engine;
			FakeDirectory dir;
			dir.engine = &engine;
			VpMotion::P2PairRecord rec;
			std::string why;
			Assert::IsFalse(dir.ReadP2Record(&host, 11, rec, why), L"the fake engine declines");
			Assert::AreEqual(1, dir.asked, L"the engine is found through EngineFor once");
			Assert::AreEqual(1, engine.p2Calls, L"P2RecordEndingAt is asked once");
			Assert::AreEqual(std::string("p2 cpu form: fake decline"), why, L"the engine's reason is returned");
			Assert::AreEqual(1, engine.declined, L"one decline counted");
			Assert::AreEqual(0, engine.served, L"nothing counted served");

			dir.engine = nullptr;
			std::string absentWhy;
			Assert::IsFalse(dir.ReadP2Record(&host, 11, rec, absentWhy), L"no engine");
			Assert::AreEqual(std::string("p2 cpu form: engine absent"), absentWhy);
			Assert::AreEqual(1, engine.p2Calls, L"no further engine call");
			Assert::AreEqual(1, engine.declined, L"no count when the engine is absent");
		}
	};
}
