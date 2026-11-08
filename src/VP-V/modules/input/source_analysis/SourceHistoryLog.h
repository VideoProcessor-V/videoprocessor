// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The source-resolution history CSV (input.source_analysis): one SESSION-LONG file family per session,
// "source-resolution <yyyymmdd-hhmmss>.csv", appended to (never rewritten). When a file reaches the size limit
// the log rotates to the next numbered file of the SAME session, "source-resolution <stamp>-002.csv", "-003", ...;
// each continuation starts with the header and a "session,...,continued" row. The newest N sessions are kept
// (all files of a session go together), older sessions are deleted when a session starts.
//
// The columns hold what the source-identity consensus needs to be replayed from the file alone (doc 78 SID-B):
// the per-axis frame readings, detail_above, mixed, the blank / cut / fade flags, the 2:1 kernel, the classical
// snapshot behind the votes and the source time of every analysed frame (tools/eval/source_id/replay.py and
// SourceIdReplayTests read it).
//
// Threading: Append (any thread, including the render thread) only pushes into a bounded in-memory queue under a short
// lock and never touches the disk. Flush (the analysis worker, or a test) swaps the queue out under that lock and writes
// OUTSIDE it. When the queue bound is exceeded the OLDEST rows are dropped and counted; a failed write is counted and
// reported (TakeNotice), never silent. C++14, no <filesystem>: the file operations are Win32 in SourceHistoryLog.cpp.
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

namespace SourceHistory
{
	// The columns (one header for every row; rows of events other than "reading" leave the later columns empty).
	inline const char* Header()
	{
		return "time,event,frame_ratio,coherence_v,coherence_h,live,declared,confidence,bitrate_quality,effective_bits,cuts,"
			"tiles,auto_ratio,note,"
			"mode,t_src,seq,frame_ratio_v,frame_ratio_h,detail_above,mixed,blank,cut,fade,apl,axis_x,axis_y,axis_conf,"
			"kernel_id,kernel,kernel_votes,kernel_agree,frame_quality,frame_noise,sid_tier,sid_level,sid_ratio_h,sid_ratio_v,sid_reason";
	}
	constexpr int kBaseColumns = 14;       // time .. note (the columns of the pre-SID-B file)
	constexpr int kExtraColumns = 25;      // the replay / identity columns after note
	constexpr int kColumns = kBaseColumns + kExtraColumns;

	// `kExtraColumns` empty trailing fields (a row of another event than "reading").
	inline std::string EmptyExtras() { return std::string(static_cast<size_t>(kExtraColumns), ','); }

	// A free-text field made safe for the CSV (commas, quotes and newlines in a note).
	inline std::string Field(const std::string& text)
	{
		if (text.find_first_of(",\"\r\n") == std::string::npos)
			return text;
		std::string out = "\"";
		for (size_t i = 0; i < text.size(); ++i)
		{
			const char c = text[i];
			if (c == '"') out += "\"\"";
			else if (c == '\r' || c == '\n') out += ' ';
			else out += c;
		}
		out += '"';
		return out;
	}

	constexpr uint64_t kDefaultRotateBytes = 20ull * 1024ull * 1024ull;
	constexpr size_t kDefaultSessions = 5;
	constexpr size_t kDefaultQueueRows = 20000;   // the bound of the in-memory queue between two Flush calls

	// The disk operations (Win32, wide paths; paths are UTF-8 here). Implemented in SourceHistoryLog.cpp.
	namespace Os
	{
		bool CreateDirectories(const std::string& folder);
		std::vector<std::string> ListNames(const std::string& folder);                          // file names only
		bool Remove(const std::string& path);
		// Creates (append = false) or appends; false + `error` on failure. `written` (optional) = the bytes that did reach the file.
		bool Write(const std::string& path, const std::string& data, bool append, std::string& error, size_t* written = nullptr);
		std::string Join(const std::string& folder, const std::string& name);
	}

	class Log
	{
	public:
		static std::string Prefix() { return "source-resolution "; }
		// The session stamp is the 15 characters "yyyymmdd-hhmmss" after the prefix.
		static constexpr size_t kStampLength = 15;

		static std::string FileName(const std::string& stamp, int index);
		// The session stamp of a file name of this family ("" = not one).
		static std::string StampOf(const std::string& fileName);
		// The files of one session in write order (base file first, then -002, -003, ...).
		static std::vector<std::string> SessionFiles(const std::string& folder, const std::string& stamp);
		// Deletes the oldest sessions so that at most `keep` remain. Returns the number of sessions deleted.
		static int Prune(const std::string& folder, size_t keep);

		// Starts a session. Does NO disk work (it may run on the render thread): it only records the session under a short
		// lock. The first Flush (the worker) creates the folder, deletes the earlier sessions beyond `sessions - 1` and
		// creates the base file with the header and `firstRow` (a complete row, no newline); a failure there is counted,
		// reported once and retried at the next Flush. An empty folder turns the log off.
		bool Open(const std::string& folder, const std::string& stamp, const std::string& firstRow, size_t sessions = kDefaultSessions,
			uint64_t rotateBytes = kDefaultRotateBytes, size_t queueRows = kDefaultQueueRows);

		bool IsOpen() const;
		std::string CurrentPath() const;
		int FileIndex() const;
		bool Dirty() const;

		// Queues one row (without the newline). Never waits for the disk.
		void Append(const std::string& row);

		// Writes the queued rows (the caller is the worker, never the render thread), rotating to the next numbered file of
		// the session whenever the size limit is reached. Returns the rows written.
		size_t Flush();

		// Rows dropped (queue bound) and failed writes, since Open.
		uint64_t DroppedRows() const;
		uint64_t FailedWrites() const;
		// One-time messages: "source history CSV: write failed (<reason>)" once per failure episode, "source history CSV: rows
		// dropped: N" when rows were dropped since the last notice. False when there is nothing new.
		bool TakeNotice(std::string& text);
		// The standing status ("" = fine): a failure is cleared by the next successful write.
		std::string Status() const;

	private:
		static int IndexOf(const std::string& fileName);
		bool Rotate();   // caller holds io_

		mutable std::mutex q_;    // the queue, its counters and the pending session start: short holds only
		mutable std::mutex io_;   // the files: folder_, stamp_, path_, bytes_, index_ ... (never taken by Append)
		std::string folder_, stamp_, path_;
		uint64_t rotateBytes_ = kDefaultRotateBytes;
		uint64_t bytes_ = 0;
		uint64_t freshBytes_ = 0;   // the size right after a file was created (header + first / continuation row)
		int index_ = 1;
		bool open_ = false;          // guarded by q_
		size_t queueRows_ = kDefaultQueueRows;
		std::string pathCopy_;       // path_ for CurrentPath(), guarded by q_
		int indexCopy_ = 1;          // index_ for FileIndex(), guarded by q_
		std::vector<std::string> pending_;   // guarded by q_
		uint64_t dropped_ = 0, droppedReported_ = 0, failed_ = 0;   // guarded by q_
		bool failing_ = false, failureReported_ = false;            // guarded by q_
		std::string failureText_;                                    // guarded by q_
		// The session start Open recorded and the first Flush performs (all guarded by q_).
		bool initPending_ = false;
		uint64_t gen_ = 0;           // bumped by every Open: a Flush of an older session never touches the new one
		std::string initFolder_, initStamp_, initFirstRow_;
		size_t initSessions_ = kDefaultSessions;
		uint64_t initRotate_ = kDefaultRotateBytes;
	};
}
