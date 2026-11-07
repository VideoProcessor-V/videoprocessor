// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// SID-B0 (doc 78): the source-resolution history CSV is session-long: appended, rotated to numbered files of the same session at
// a size limit, the newest sessions kept whole, never blocking the caller on the disk, failures visible. CPU only, a temp folder,
// no clock in any decision. (The replay of these files through the identity pipeline is tested in SourceIdReplayTests.)
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/modules/input/source_analysis/SourceHistoryLog.h>
#include <VP-V/modules/input/source_analysis/SourceResolutionMonitor.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
	namespace SidHistory
	{
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
		// The number of CSV fields in a line ("..." quoting, "" for a quote).
		inline int Columns(const std::string& line)
		{
			int n = 1;
			bool quoted = false;
			for (size_t i = 0; i < line.size(); ++i)
			{
				if (quoted) { if (line[i] == '"') { if (i + 1 < line.size() && line[i + 1] == '"') ++i; else quoted = false; } }
				else if (line[i] == '"') quoted = true;
				else if (line[i] == ',') ++n;
			}
			return n;
		}
	}

	TEST_CLASS(SourceHistoryLogTests)
	{
	public:
		// ---- the session-long writer ----
		TEST_METHOD(TheLogRotatesToNumberedFilesOfTheSameSession)
		{
			using namespace SidHistory;
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
		}

		TEST_METHOD(TheNewestFiveSessionsAreKeptWholeAndOlderOnesGo)
		{
			using namespace SidHistory;
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
			using namespace SidHistory;
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
			using namespace SidHistory;
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
			using namespace SidHistory;
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
			using namespace SidHistory;
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
			Assert::AreEqual(std::string("\"confirmed: waiting for a cut, a black, \"\"or\"\" a pan\""), SourceHistory::Field(note));
			Assert::AreEqual(std::string("plain"), SourceHistory::Field("plain"));
			Assert::AreEqual(std::string("\"two lines\""), SourceHistory::Field("two\nlines"), L"a newline becomes a space");
		}

		// The monitor's own rows: header first, every row full width (event rows without the identity columns leave them empty),
		// notes quoted, the session row and the readings present, and the file appended (not rewritten) by a second flush.
		TEST_METHOD(TheMonitorWritesFullWidthRowsAndAppends)
		{
			using namespace SidHistory;
			TempFolder dir;
			SourceResolutionMonitor::Monitor m;
			m.SetHistoryFolder(dir.Str());
			const std::string path = m.HistoryPath();
			Assert::IsFalse(path.empty());
			constexpr int tile = SourceResolutionMonitor::Monitor::kTile;
			std::vector<float> tiles(static_cast<size_t>(8) * tile * tile);
			unsigned x = 12345u;
			for (float& v : tiles) { x = x * 1664525u + 1013904223u; v = 0.2f + 0.5f * static_cast<float>(x >> 8) / 16777216.0f; }
			std::string why;
			double fx = 1.0, fy = 1.0;
			for (int f = 0; f < 3; ++f)
			{
				m.AnalyseTilesForTest(tiles, 8, 0.5 * f, 3840, 2160);
				m.AutoFactors(3840, 2160, 0.8, why, fx, fy, 0.5 * f + 0.1);
			}
			m.AnalyseTilesForTest(tiles, 8, 10.0, 3840, 2160);   // a gap of 9 s with no frames: a discontinuity, synchronously
			m.FlushHistoryNow();
			const std::string first = ReadAll(path);
			const std::vector<std::string> lines = Lines(first);
			Assert::IsTrue(lines.size() >= 6);
			Assert::AreEqual(std::string(SourceHistory::Header()), lines[0], L"the header");
			int readings = 0;
			bool session = false, gap = false;
			for (size_t i = 1; i < lines.size(); ++i)
			{
				Assert::AreEqual(SourceHistory::kColumns, Columns(lines[i]), L"every row is full width");
				if (lines[i].find(",reading,") != std::string::npos) ++readings;
				if (lines[i].find(",session,") != std::string::npos) session = true;
				if (lines[i].find(",discontinuity,") != std::string::npos && lines[i].find("with no frames") != std::string::npos) gap = true;
			}
			Assert::IsTrue(readings >= 4, L"a reading row per analysed frame");
			Assert::IsTrue(session, L"the session row");
			Assert::IsTrue(gap, L"the discontinuity row with its cause");
			// A second flush appends: the first file's text is a prefix of the new one.
			m.AnalyseTilesForTest(tiles, 8, 10.5, 3840, 2160);
			m.FlushHistoryNow();
			const std::string second = ReadAll(path);
			Assert::IsTrue(second.size() > first.size() && second.compare(0, first.size(), first) == 0, L"appended, never rewritten");
		}
	};
}
