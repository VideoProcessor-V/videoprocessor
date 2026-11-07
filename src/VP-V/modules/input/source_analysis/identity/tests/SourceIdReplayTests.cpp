/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// SID-B (doc 78): the source-resolution history CSV is replayable and session-long. A small COMMITTED fixture
// (fixtures/srm-history-replay.csv: 58 s of readings at the 0.5 s cadence - native, a declared 1080 reading, a sustained-native
// veto, a second declaration, a flagged regime boundary, a discontinuity) is replayed through the identity pipeline; the
// trace is deterministic and its changes are the ones the consensus rules give (dwell 3 s, T_min 10 s, regime boundary after a
// cut). Then the writer: append, rotate to numbered files of the same session at the size limit, keep the newest sessions.
// CPU only, a temp folder, no clock in the decisions.
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/modules/input/source_analysis/SourceHistoryLog.h>
#include <VP-V/modules/input/source_analysis/identity/SourceIdReplay.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
	namespace SidReplay
	{
		namespace fs = std::filesystem;

		inline std::string FixtureText()
		{
			const fs::path path = fs::path(__FILE__).parent_path() / "fixtures" / "srm-history-replay.csv";
			std::ifstream in(path, std::ios::binary);
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

		// The fixture with the cut column cleared: no flagged regime boundary.
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
			fs::path path;
			TempFolder()
			{
				path = fs::temp_directory_path() / ("vp-sidb-history-" + std::to_string(reinterpret_cast<uintptr_t>(this)));
				std::error_code ec;
				fs::remove_all(path, ec);
				fs::create_directories(path, ec);
			}
			~TempFolder() { std::error_code ec; fs::remove_all(path, ec); }
			std::string Str() const { return path.string(); }
		};

		inline std::string ReadAll(const fs::path& p)
		{
			std::ifstream in(p, std::ios::binary);
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

		TEST_METHOD(TheReplayShowsTheConsensusRulesOnTheFixture)
		{
			using namespace SidReplay;
			const std::vector<SourceIdentity::ReplayStep> steps = Replay(FixtureText());
			const std::vector<int64_t> at = AxisChangeTimes(steps);
			// 1. the declaration from 10 s: native -> 0.5 after the 3 s dwell;
			// 2. the sustained-native veto from 30 s: native at once (T_min since the first change has elapsed);
			// 3. the second declaration from 40 s: 0.5 after the dwell, T_min after the veto change;
			// 4. a cut flagged at 47 s with the veto persisting 3 evaluations: native at 48 s, inside T_min (a regime boundary).
			Assert::AreEqual(size_t(4), at.size(), L"four changes of the axes");
			Assert::IsTrue(at[0] == 13000000 && at[1] == 30000000 && at[2] == 43000000 && at[3] == 48000000);
			for (const SourceIdentity::ReplayStep& s : steps)
			{
				if (s.discontinuity) continue;
				if (s.timeUs >= 13000000 && s.timeUs < 30000000)
					Assert::IsTrue(s.ratioH == 0.5f && s.ratioV == 0.5f, L"re-based between the first change and the veto");
				if (s.timeUs >= 30000000 && s.timeUs < 43000000)
					Assert::IsTrue(s.ratioH == 1.0f && s.ratioV == 1.0f, L"native between the veto and the second declaration");
				if (s.timeUs >= 43000000 && s.timeUs < 48000000)
					Assert::IsTrue(s.ratioH == 0.5f && s.ratioV == 0.5f);
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

		TEST_METHOD(WithoutTheCutFlagTheLastChangeWaitsForTMin)
		{
			using namespace SidReplay;
			const std::vector<int64_t> at = AxisChangeTimes(Replay(WithoutCutFlags(FixtureText())));
			Assert::AreEqual(size_t(4), at.size());
			// The veto still comes (sustained native), but with no regime boundary T_min (10 s after the change at 43 s) holds it.
			Assert::IsTrue(at[3] == 53000000, L"no flag: the fourth change comes at 43 s + T_min");
		}

		TEST_METHOD(AMissingColumnIsReportedNotGuessed)
		{
			SourceIdentity::Pipeline pipeline;
			std::vector<SourceIdentity::ReplayStep> steps;
			std::string error;
			Assert::IsFalse(SourceIdentity::SrmReplay::Run("time,event,frame_ratio\n12:00:00.000,reading,1.0\n", pipeline, steps, error));
			Assert::IsTrue(error.find("t_src") != std::string::npos);
		}

		// ---- the session-long writer ----
		TEST_METHOD(TheLogRotatesToNumberedFilesOfTheSameSession)
		{
			using namespace SidReplay;
			TempFolder dir;
			SourceHistory::Log log;
			const uint64_t limit = 3000;   // bytes (a header is ~450)
			const std::string stamp = "20261107-120000";
			const std::string first = "12:00:00.000,session" + std::string(12, ',') + "session started" + SourceHistory::EmptyExtras();
			Assert::IsTrue(log.Open(dir.Str(), stamp, first, SourceHistory::kDefaultSessions, limit));
			Assert::AreEqual(1, log.FileIndex());
			std::vector<std::string> rows;
			for (int i = 0; i < 120; ++i)
			{
				char row[200];
				snprintf(row, sizeof(row), "12:00:%02d.000,reading,1.0000,0.620,0.610,1.0000,1.0000,0.00,0.95,8.0,%d,8/8,1.0000,120.0 ms", i % 60, i);
				rows.push_back(std::string(row) + SourceHistory::EmptyExtras());
				log.Append(rows.back());
				if (i % 17 == 16) log.Flush();   // appended in several flushes, as the worker does
			}
			log.Flush();
			Assert::IsFalse(log.Dirty());
			const std::vector<std::string> files = SourceHistory::Log::SessionFiles(dir.Str(), stamp);
			Assert::IsTrue(files.size() >= 4, L"several files for 120 rows at a 3000-byte limit");
			Assert::AreEqual(SourceHistory::Log::FileName(stamp, 1), files[0]);
			Assert::AreEqual(SourceHistory::Log::FileName(stamp, 2), files[1], L"the continuation is numbered -002");
			Assert::AreEqual(static_cast<int>(files.size()), log.FileIndex());
			std::vector<std::string> collected;
			for (const std::string& name : files)
			{
				const std::string text = ReadAll(dir.path / name);
				Assert::IsTrue(text.size() <= limit, L"no file exceeds the limit (a row never straddles files)");
				const std::vector<std::string> lines = Lines(text);
				Assert::AreEqual(std::string(SourceHistory::Header()), lines[0], L"every file starts with the header");
				for (size_t i = 1; i < lines.size(); ++i)
					if (lines[i].find(",reading,") != std::string::npos) collected.push_back(lines[i]);
					else Assert::IsTrue(lines[i].find(",session,") != std::string::npos, L"the only other rows are session rows");
			}
			Assert::AreEqual(rows.size(), collected.size(), L"every row is in exactly one file");
			for (size_t i = 0; i < rows.size(); ++i)
				Assert::IsTrue(rows[i] == collected[i], L"in order");
			// The files of a rotated session read back as one replay input (the headers repeat).
			std::string joined;
			for (const std::string& name : files) joined += ReadAll(dir.path / name);
			SourceIdentity::Pipeline pipeline;
			std::vector<SourceIdentity::ReplayStep> steps;
			std::string error;
			Assert::IsTrue(SourceIdentity::SrmReplay::Run(joined, pipeline, steps, error), L"the files of a rotated session read back as one input");
			Assert::AreEqual(size_t(120), steps.size(), L"one step per reading row; the repeated headers and session rows are skipped");
		}

		TEST_METHOD(TheNewestFiveSessionsAreKeptWholeAndOlderOnesGo)
		{
			using namespace SidReplay;
			TempFolder dir;
			const uint64_t limit = 1500;
			for (int s = 1; s <= 7; ++s)
			{
				SourceHistory::Log log;
				char stamp[32];
				snprintf(stamp, sizeof(stamp), "2026110%d-120000", s);
				Assert::IsTrue(log.Open(dir.Str(), stamp, "12:00:00.000,session" + std::string(12, ',') + "started" + SourceHistory::EmptyExtras(),
					SourceHistory::kDefaultSessions, limit));
				for (int i = 0; i < 40; ++i)
					log.Append("12:00:00.000,reading,1.0000" + std::string(100, 'x') + SourceHistory::EmptyExtras());
				log.Flush();
				Assert::IsTrue(SourceHistory::Log::SessionFiles(dir.Str(), stamp).size() >= 2, L"each session rotated");
			}
			std::vector<std::string> stamps;
			for (const fs::directory_entry& e : fs::directory_iterator(dir.path))
			{
				const std::string stamp = SourceHistory::Log::StampOf(e.path().filename().string());
				Assert::IsFalse(stamp.empty());
				if (std::find(stamps.begin(), stamps.end(), stamp) == stamps.end()) stamps.push_back(stamp);
			}
			std::sort(stamps.begin(), stamps.end());
			Assert::AreEqual(size_t(5), stamps.size(), L"five sessions kept");
			Assert::AreEqual(std::string("20261103-120000"), stamps.front(), L"the two oldest sessions went, with all their files");
			Assert::AreEqual(std::string("20261107-120000"), stamps.back());
			// A session's files are never split by the pruning: the base file and its continuations are all there.
			for (const std::string& stamp : stamps)
			{
				const std::vector<std::string> files = SourceHistory::Log::SessionFiles(dir.Str(), stamp);
				for (size_t i = 0; i < files.size(); ++i)
					Assert::AreEqual(SourceHistory::Log::FileName(stamp, static_cast<int>(i) + 1), files[i]);
			}
		}

		TEST_METHOD(AFreeTextNoteIsQuotedSoTheColumnsStayAligned)
		{
			const std::string note = "confirmed: waiting for a cut, a black, \"or\" a pan";
			const std::string field = SourceHistory::Field(note);
			const std::vector<std::string> parsed = SourceIdentity::SplitCsvLine("a," + field + ",b");
			Assert::AreEqual(size_t(3), parsed.size());
			Assert::AreEqual(note, parsed[1]);
			Assert::AreEqual(std::string("plain"), SourceHistory::Field("plain"));
		}
	};
}
