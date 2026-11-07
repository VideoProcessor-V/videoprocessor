/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// SID-B0 (doc 78): the source-resolution history CSV is replayable. A small COMMITTED fixture (fixtures/srm-history-replay.csv:
// 58 s of readings at the 0.5 s cadence - native, a declared 1080 reading, a classical veto, a second declaration, a second veto,
// a discontinuity) is replayed through the identity pipeline; the trace is deterministic and its changes are the ones the consensus
// rules give today (dwell 3 s, hold 10 s after a change, the safety veto at once). The files of a rotated session replay as one
// input. CPU only, no clock in the decisions. (The writer itself is tested in SourceHistoryLogTests.)
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/modules/input/source_analysis/SourceHistoryLog.h>
#include <VP-V/modules/input/source_analysis/identity/SourceIdReplay.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
	namespace SidReplay
	{
		inline std::string FixtureText()
		{
			const std::string file = __FILE__;
			const std::string path = file.substr(0, file.find_last_of("\\/")) + "\\fixtures\\srm-history-replay.csv";
			std::ifstream in(path.c_str(), std::ios::binary);
			Assert::IsTrue(in.good(), L"the committed fixture opens");
			return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		}

		// The source times (microseconds) at which both axes changed.
		inline std::vector<int64_t> AxisChangeTimes(const std::vector<SourceIdentity::ReplayStep>& steps)
		{
			std::vector<int64_t> at;
			for (const SourceIdentity::ReplayStep& s : steps)
				if (!s.discontinuity && s.Changed(SourceIdentity::Field::AxisH) && s.Changed(SourceIdentity::Field::AxisV))
					at.push_back(s.timeUs);
			return at;
		}

		inline std::vector<SourceIdentity::ReplayStep> Replay(const std::string& text)
		{
			SourceIdentity::Pipeline pipeline;
			pipeline.SetConsensusOn(true);
			std::vector<SourceIdentity::ReplayStep> steps;
			std::string error;
			Assert::IsTrue(SourceIdentity::SrmReplay::Run(text, pipeline, steps, error), std::wstring(error.begin(), error.end()).c_str());
			return steps;
		}

		// The fixture with the cut column cleared.
		inline std::string WithoutCutFlags(const std::string& text)
		{
			std::string out;
			size_t cutColumn = std::string::npos;
			size_t pos = 0;
			while (pos < text.size())
			{
				size_t end = text.find('\n', pos);
				if (end == std::string::npos) end = text.size();
				std::vector<std::string> f = SourceIdentity::SplitCsvLine(text.substr(pos, end - pos));
				if (f.size() > 2 && f[0] == "time")
					for (size_t i = 0; i < f.size(); ++i)
						if (f[i] == "cut") cutColumn = i;
				if (f.size() > 2 && f[1] == "reading" && cutColumn < f.size())
					f[cutColumn] = "0";
				for (size_t i = 0; i < f.size(); ++i)
				{
					if (i) out += ',';
					out += SourceHistory::Field(f[i]);
				}
				out += '\n';
				pos = end + 1;
			}
			return out;
		}

		struct TempFolder
		{
			std::string path;
			TempFolder()
			{
				char temp[MAX_PATH] = {};
				GetTempPathA(MAX_PATH, temp);
				path = std::string(temp) + "vp-sidb-history-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount());
				Clear();
				SourceHistory::Os::CreateDirectories(path);
			}
			void Clear()
			{
				const std::vector<std::string> names = SourceHistory::Os::ListNames(path);
				for (size_t i = 0; i < names.size(); ++i) SourceHistory::Os::Remove(Join(names[i]));
			}
			~TempFolder() { Clear(); RemoveDirectoryA(path.c_str()); }
			std::string Str() const { return path; }
			std::string Join(const std::string& name) const { return SourceHistory::Os::Join(path, name); }
		};

		inline std::string ReadAll(const std::string& p)
		{
			std::ifstream in(p.c_str(), std::ios::binary);
			return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		}

		inline std::vector<std::string> Lines(const std::string& text)
		{
			std::vector<std::string> v;
			size_t pos = 0;
			while (pos < text.size())
			{
				size_t end = text.find('\n', pos);
				if (end == std::string::npos) end = text.size();
				std::string one = text.substr(pos, end - pos);
				if (!one.empty() && one.back() == '\r') one.pop_back();   // a CRLF checkout of the fixture
				v.push_back(one);
				pos = end + 1;
			}
			return v;
		}
	}

	TEST_CLASS(SourceIdReplayTests)
	{
	public:
		TEST_METHOD(TheFixtureHeaderIsTheWritersHeader)
		{
			using namespace SidReplay;
			const std::vector<std::string> lines = Lines(FixtureText());
			Assert::IsTrue(!lines.empty());
			Assert::AreEqual(std::string(SourceHistory::Header()), lines[0], L"the committed fixture follows the current columns");
			Assert::AreEqual(SourceHistory::kColumns, static_cast<int>(SourceIdentity::SplitCsvLine(lines[0]).size()));
			// Every row is full width (event rows without the identity columns leave them empty).
			for (size_t i = 1; i < lines.size(); ++i)
				Assert::AreEqual(SourceHistory::kColumns, static_cast<int>(SourceIdentity::SplitCsvLine(lines[i]).size()));
		}

		TEST_METHOD(TheReplayTraceIsDeterministic)
		{
			using namespace SidReplay;
			const std::string text = FixtureText();
			const std::string a = SourceIdentity::SrmReplay::Trace(Replay(text));
			const std::string b = SourceIdentity::SrmReplay::Trace(Replay(text));
			Assert::IsTrue(a == b, L"the same CSV gives the same trace");
			Assert::IsTrue(a.size() > 1000);
		}

		// The consensus on integ (dwell 3 s, hold 10 s after a change, the safety veto at once) on the fixture:
		//  1. the declaration from 10 s: native -> 0.5 after the 3 s dwell (13 s);
		//  2. the classical veto from 30 s (detail_above): native at once (30 s);
		//  3. the second declaration from 40 s: 0.5 after the dwell, the hold since the veto change (30 s) having elapsed (43 s);
		//  4. the veto again from 47 s: native at once (47 s); the discontinuity row (55 s) keeps it native.
		TEST_METHOD(TheReplayShowsTheConsensusRulesOnTheFixture)
		{
			using namespace SidReplay;
			const std::vector<SourceIdentity::ReplayStep> steps = Replay(FixtureText());
			const std::vector<int64_t> at = AxisChangeTimes(steps);
			Assert::AreEqual(size_t(4), at.size(), L"four changes of the axes");
			Assert::IsTrue(at[0] == 13000000 && at[1] == 30000000 && at[2] == 43000000 && at[3] == 47000000);
			for (const SourceIdentity::ReplayStep& s : steps)
			{
				if (s.discontinuity) continue;
				if (s.timeUs >= 13000000 && s.timeUs < 30000000)
					Assert::IsTrue(s.ratioH == 0.5f && s.ratioV == 0.5f, L"re-based between the first change and the veto");
				if (s.timeUs >= 30000000 && s.timeUs < 43000000)
					Assert::IsTrue(s.ratioH == 1.0f && s.ratioV == 1.0f, L"native between the veto and the second declaration");
				if (s.timeUs >= 43000000 && s.timeUs < 47000000)
					Assert::IsTrue(s.ratioH == 0.5f && s.ratioV == 0.5f, L"re-based again until the second veto");
				if (s.timeUs >= 47000000)
					Assert::IsTrue(s.ratioH == 1.0f && s.ratioV == 1.0f, L"native from the second veto on");
			}
			// The discontinuity row (55 s) returns to native and the later readings stay there.
			bool seenDiscontinuity = false;
			for (const SourceIdentity::ReplayStep& s : steps)
			{
				if (s.discontinuity) { seenDiscontinuity = true; Assert::IsTrue(s.ratioH == 1.0f && s.ratioV == 1.0f); }
				else if (seenDiscontinuity) Assert::IsTrue(s.ratioH == 1.0f && s.ratioV == 1.0f);
			}
			Assert::IsTrue(seenDiscontinuity);
		}

		// The cut / blank / fade flags are recorded (QualitySample, the ABR detector) but the consensus does not read them today:
		// clearing the cut column leaves every change where it was.
		TEST_METHOD(TheCutColumnDoesNotMoveTheConsensusToday)
		{
			using namespace SidReplay;
			const std::string text = FixtureText();
			const std::vector<int64_t> a = AxisChangeTimes(Replay(text));
			const std::vector<int64_t> b = AxisChangeTimes(Replay(WithoutCutFlags(text)));
			Assert::IsTrue(a == b, L"the same axis changes with and without the cut flags");
		}

		TEST_METHOD(AMissingColumnIsReportedNotGuessed)
		{
			SourceIdentity::Pipeline pipeline;
			std::vector<SourceIdentity::ReplayStep> steps;
			std::string error;
			Assert::IsFalse(SourceIdentity::SrmReplay::Run("time,event,frame_ratio\n12:00:00.000,reading,1.0\n", pipeline, steps, error));
			Assert::IsTrue(error.find("t_src") != std::string::npos);
		}

		// The files of a rotated session read back as one replay input (the headers repeat, session rows are skipped).
		TEST_METHOD(TheFilesOfARotatedSessionReplayAsOneInput)
		{
			using namespace SidReplay;
			TempFolder dir;
			SourceHistory::Log log;
			const uint64_t limit = 3000;
			const std::string stamp = "20261107-120000";
			Assert::IsTrue(log.Open(dir.Str(), stamp, "12:00:00.000,session" + std::string(12, ',') + "session started" + SourceHistory::EmptyExtras(),
				SourceHistory::kDefaultSessions, limit));
			// The fixture's reading rows, re-written through the log (so they rotate), as a monitor would write them.
			const std::vector<std::string> fixture = Lines(FixtureText());
			size_t rows = 0;
			for (size_t i = 1; i < fixture.size(); ++i)
				if (fixture[i].find(",reading,") != std::string::npos) { log.Append(fixture[i]); ++rows; }
			log.Flush();
			const std::vector<std::string> files = SourceHistory::Log::SessionFiles(dir.Str(), stamp);
			Assert::IsTrue(files.size() >= 4, L"the rows rotated over several files");
			std::string joined;
			for (const std::string& name : files) joined += ReadAll(dir.Join(name));
			SourceIdentity::Pipeline pipeline;
			pipeline.SetConsensusOn(true);
			std::vector<SourceIdentity::ReplayStep> steps;
			std::string error;
			Assert::IsTrue(SourceIdentity::SrmReplay::Run(joined, pipeline, steps, error), std::wstring(error.begin(), error.end()).c_str());
			Assert::AreEqual(rows, steps.size(), L"one step per reading row; the repeated headers and session rows are skipped");
			// and it is the same trace as the readings of the unrotated fixture
			SourceIdentity::Pipeline direct;
			direct.SetConsensusOn(true);
			std::vector<SourceIdentity::ReplayStep> directSteps;
			std::string fixtureReadings;
			for (size_t i = 0; i < fixture.size(); ++i)
				if (i == 0 || fixture[i].find(",reading,") != std::string::npos) fixtureReadings += fixture[i] + "\n";
			Assert::IsTrue(SourceIdentity::SrmReplay::Run(fixtureReadings, direct, directSteps, error));
			Assert::IsTrue(SourceIdentity::SrmReplay::Trace(steps) == SourceIdentity::SrmReplay::Trace(directSteps), L"rotation does not change the replay");
		}

		TEST_METHOD(AFreeTextNoteSurvivesTheReplaysCsvSplit)
		{
			const std::string note = "confirmed: waiting for a cut, a black, \"or\" a pan";
			const std::vector<std::string> parsed = SourceIdentity::SplitCsvLine("a," + SourceHistory::Field(note) + ",b");
			Assert::AreEqual(size_t(3), parsed.size());
			Assert::AreEqual(note, parsed[1]);
		}
	};
}
