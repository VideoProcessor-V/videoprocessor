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
				const std::string text = ReadAll(dir.Join(name));
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
			for (const std::string& name : files) joined += ReadAll(dir.Join(name));
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
			const std::vector<std::string> present = SourceHistory::Os::ListNames(dir.Str());
			for (size_t n = 0; n < present.size(); ++n)
			{
				const std::string stamp = SourceHistory::Log::StampOf(present[n]);
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

		// Append never waits on the disk and never grows without bound: past the bound the OLDEST rows are dropped, counted and reported.
		TEST_METHOD(AppendIsBoundedDropsTheOldestAndReportsIt)
		{
			using namespace SidReplay;
			TempFolder dir;
			SourceHistory::Log log;
			Assert::IsTrue(log.Open(dir.Str(), "20261107-130000", "12:00:00.000,session" + std::string(12, ',') + "x" + SourceHistory::EmptyExtras(),
				SourceHistory::kDefaultSessions, SourceHistory::kDefaultRotateBytes, 20));
			for (int i = 0; i < 100; ++i)
				log.Append("row-" + std::to_string(i));   // no Flush: nothing touches the disk
			Assert::IsTrue(log.DroppedRows() >= 80, L"rows beyond the bound were dropped");
			std::string notice;
			Assert::IsTrue(log.TakeNotice(notice));
			Assert::IsTrue(notice.find("rows dropped") != std::string::npos, L"visibly");
			Assert::IsFalse(log.TakeNotice(notice), L"once");
			Assert::IsTrue(log.Status().find("rows dropped") != std::string::npos);
			log.Flush();
			const std::string text = ReadAll(dir.Join(SourceHistory::Log::FileName("20261107-130000", 1)));
			Assert::IsTrue(text.find("row-99") != std::string::npos, L"the newest row is kept");
			Assert::IsTrue(text.find("row-0\n") == std::string::npos, L"the oldest was dropped");
		}

		// A failed write is counted, reported once, keeps the rows, and the log recovers when the file can be written again.
		TEST_METHOD(AFailedWriteIsReportedOnceKeepsTheRowsAndRecovers)
		{
			using namespace SidReplay;
			TempFolder dir;
			SourceHistory::Log log;
			const std::string stamp = "20261107-140000";
			Assert::IsTrue(log.Open(dir.Str(), stamp, "12:00:00.000,session" + std::string(12, ',') + "x" + SourceHistory::EmptyExtras()));
			const std::string path = dir.Join(SourceHistory::Log::FileName(stamp, 1));
			log.Append("before-lock");
			Assert::AreEqual(size_t(1), log.Flush(), L"the first flush starts the session and writes");
			HANDLE lockHandle = CreateFileA(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);   // exclusive: no share
			Assert::IsTrue(lockHandle != INVALID_HANDLE_VALUE);
			for (int i = 0; i < 5; ++i) log.Append("held-" + std::to_string(i));
			Assert::AreEqual(size_t(0), log.Flush());
			Assert::IsTrue(log.FailedWrites() >= 1);
			Assert::IsTrue(log.Dirty(), L"the unwritten rows are kept");
			std::string notice;
			Assert::IsTrue(log.TakeNotice(notice));
			Assert::IsTrue(notice.find("write failed") != std::string::npos);
			Assert::IsFalse(log.TakeNotice(notice), L"reported once per failure episode");
			log.Flush();
			Assert::IsFalse(log.TakeNotice(notice), L"a second failed Flush in the same episode is not reported again");
			Assert::IsTrue(log.Status().find("write failed") != std::string::npos, L"but the standing status stays");
			CloseHandle(lockHandle);
			Assert::AreEqual(size_t(5), log.Flush(), L"recovered: the kept rows are written");
			Assert::IsTrue(log.Status().empty());
			Assert::IsTrue(ReadAll(path).find("held-4") != std::string::npos);
		}

		// Open does no disk work (the render thread calls it): nothing exists until the first Flush, which does the start.
		TEST_METHOD(OpenDoesNoDiskWorkTheFirstFlushStartsTheSession)
		{
			using namespace SidReplay;
			TempFolder dir;
			const std::string sub = dir.Join("later");
			SourceHistory::Log log;
			Assert::IsTrue(log.Open(sub, "20261107-150000", "12:00:00.000,session" + std::string(12, ',') + "x" + SourceHistory::EmptyExtras()));
			Assert::IsTrue(SourceHistory::Os::ListNames(sub).empty(), L"no folder, no file yet");
			Assert::IsTrue(log.CurrentPath().find("20261107-150000") != std::string::npos, L"the path is known at once");
			Assert::IsTrue(log.Dirty(), L"a pending session start counts as work for the worker");
			log.Append("row-a");
			Assert::AreEqual(size_t(1), log.Flush());
			Assert::AreEqual(size_t(1), SourceHistory::Os::ListNames(sub).size());
			Assert::IsTrue(ReadAll(log.CurrentPath()).find("row-a") != std::string::npos);
			Assert::IsFalse(log.Dirty());
			SourceHistory::Os::Remove(log.CurrentPath());
			RemoveDirectoryA(sub.c_str());
		}

		// A session that cannot be started (the folder cannot be created) is reported once like a failed write, and retried.
		TEST_METHOD(AFailedSessionStartIsReportedOnceAndRetried)
		{
			using namespace SidReplay;
			TempFolder dir;
			// A file where the folder should go: the folder cannot be created under it.
			const std::string blocker = dir.Join("blocker");
			std::string writeError;
			Assert::IsTrue(SourceHistory::Os::Write(blocker, "x", false, writeError));
			SourceHistory::Log log;
			Assert::IsTrue(log.Open(SourceHistory::Os::Join(blocker, "sub"), "20261107-160000", "first"));
			log.Append("row");
			Assert::AreEqual(size_t(0), log.Flush());
			std::string notice;
			Assert::IsTrue(log.TakeNotice(notice));
			Assert::IsTrue(notice.find("write failed") != std::string::npos && notice.find("cannot create") != std::string::npos, L"visible");
			Assert::IsFalse(log.TakeNotice(notice), L"once");
			Assert::IsTrue(log.Dirty(), L"the start and the row are kept for the retry");
			SourceHistory::Os::Remove(blocker);
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
