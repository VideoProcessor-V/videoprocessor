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
	};
}
