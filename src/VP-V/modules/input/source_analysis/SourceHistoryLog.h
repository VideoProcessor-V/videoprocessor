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
// Not thread-safe by itself except Append / Flush / the accessors, which share one mutex.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
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

	// `n` empty trailing fields (a row of another event than "reading"): ",,,,".
	inline std::string EmptyExtras() { return std::string(static_cast<size_t>(kExtraColumns), ','); }

	// A free-text field made safe for the CSV (commas, quotes and newlines in a note).
	inline std::string Field(const std::string& text)
	{
		if (text.find_first_of(",\"\r\n") == std::string::npos)
			return text;
		std::string out = "\"";
		for (char c : text)
		{
			if (c == '"') out += "\"\"";
			else if (c == '\r' || c == '\n') out += ' ';
			else out += c;
		}
		out += '"';
		return out;
	}

	constexpr uint64_t kDefaultRotateBytes = 20ull * 1024ull * 1024ull;
	constexpr size_t kDefaultSessions = 5;

	class Log
	{
	public:
		static std::string Prefix() { return "source-resolution "; }
		// The session stamp is the 15 characters "yyyymmdd-hhmmss" after the prefix.
		static constexpr size_t kStampLength = 15;

		static std::string FileName(const std::string& stamp, int index)
		{
			char tail[24];
			if (index <= 1) tail[0] = '\0';
			else std::snprintf(tail, sizeof(tail), "-%03d", index);
			return Prefix() + stamp + tail + ".csv";
		}

		// The session stamp of a file name of this family ("" = not one).
		static std::string StampOf(const std::string& fileName)
		{
			const std::string prefix = Prefix();
			if (fileName.size() < prefix.size() + kStampLength + 4 || fileName.compare(0, prefix.size(), prefix) != 0)
				return std::string();
			if (fileName.compare(fileName.size() - 4, 4, ".csv") != 0)
				return std::string();
			return fileName.substr(prefix.size(), kStampLength);
		}

		// The files of one session in write order (base file first, then -002, -003, ...).
		static std::vector<std::string> SessionFiles(const std::string& folder, const std::string& stamp)
		{
			std::vector<std::string> names;
			std::error_code ec;
			for (std::filesystem::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
			{
				const std::string name = it->path().filename().string();
				if (StampOf(name) == stamp) names.push_back(name);
			}
			// "<stamp>.csv" sorts after "<stamp>-002.csv" ('.' > '-'): order by the rotation index instead.
			std::sort(names.begin(), names.end(), [&](const std::string& a, const std::string& b) { return IndexOf(a) < IndexOf(b); });
			return names;
		}

		// Deletes the oldest sessions so that at most `keep` remain. Returns the number of sessions deleted.
		static int Prune(const std::string& folder, size_t keep)
		{
			std::vector<std::string> stamps;
			std::error_code ec;
			for (std::filesystem::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
			{
				const std::string stamp = StampOf(it->path().filename().string());
				if (!stamp.empty() && std::find(stamps.begin(), stamps.end(), stamp) == stamps.end()) stamps.push_back(stamp);
			}
			std::sort(stamps.begin(), stamps.end());   // stamps sort by time
			int deleted = 0;
			while (stamps.size() > keep)
			{
				for (const std::string& name : SessionFiles(folder, stamps.front()))
					std::filesystem::remove(std::filesystem::path(folder) / name, ec);
				stamps.erase(stamps.begin());
				++deleted;
			}
			return deleted;
		}

		// Starts a session: the earlier sessions beyond `sessions - 1` are deleted, the base file is created with the
		// header and `firstRow` (a complete row, no newline). An empty folder turns the log off.
		bool Open(const std::string& folder, const std::string& stamp, const std::string& firstRow, size_t sessions = kDefaultSessions,
			uint64_t rotateBytes = kDefaultRotateBytes)
		{
			std::lock_guard<std::mutex> lock(m_);
			pending_.clear();
			folder_.clear();
			path_.clear();
			if (folder.empty()) return false;
			std::error_code ec;
			std::filesystem::create_directories(folder, ec);
			// A restart inside the same second reuses the stamp: its earlier files go first.
			for (const std::string& name : SessionFiles(folder, stamp))
				std::filesystem::remove(std::filesystem::path(folder) / name, ec);
			Prune(folder, sessions > 0 ? sessions - 1 : 0);
			folder_ = folder;
			stamp_ = stamp;
			rotateBytes_ = rotateBytes > 0 ? rotateBytes : kDefaultRotateBytes;
			index_ = 1;
			bytes_ = 0;
			path_ = (std::filesystem::path(folder_) / FileName(stamp_, 1)).string();
			dirty_ = false;
			// A fresh file: the header and the first row.
			std::FILE* f = nullptr;
			if (fopen_s(&f, path_.c_str(), "wb") != 0 || !f) { path_.clear(); return false; }
			const std::string head = std::string(Header()) + "\n" + firstRow + "\n";
			std::fwrite(head.data(), 1, head.size(), f);
			std::fclose(f);
			bytes_ = head.size();
			freshBytes_ = bytes_;
			return true;
		}

		bool IsOpen() const { std::lock_guard<std::mutex> lock(m_); return !path_.empty(); }
		std::string CurrentPath() const { std::lock_guard<std::mutex> lock(m_); return path_; }
		int FileIndex() const { std::lock_guard<std::mutex> lock(m_); return index_; }
		bool Dirty() const { std::lock_guard<std::mutex> lock(m_); return dirty_; }

		// Queues one row (without the newline).
		void Append(const std::string& row)
		{
			std::lock_guard<std::mutex> lock(m_);
			if (path_.empty()) return;
			pending_.push_back(row);
			dirty_ = true;
		}

		// Appends the queued rows to the current file (one write per file), rotating to the next numbered file of the
		// session whenever the size limit is reached. Returns the rows written.
		size_t Flush()
		{
			std::lock_guard<std::mutex> lock(m_);
			if (path_.empty() || pending_.empty()) { dirty_ = false; return 0; }
			size_t written = 0;
			size_t i = 0;
			while (i < pending_.size())
			{
				// As many rows as fit below the limit, in one write.
				std::string chunk;
				size_t count = 0;
				while (i + count < pending_.size() && bytes_ + chunk.size() + pending_[i + count].size() + 1 <= rotateBytes_)
				{
					chunk += pending_[i + count];
					chunk += '\n';
					++count;
				}
				if (count == 0)
				{
					// The next row does not fit: continue in the next numbered file (a row never straddles files). A row
					// larger than the whole limit still goes into a fresh file, alone.
					if (bytes_ > freshBytes_)
					{
						if (!Rotate()) break;
						continue;
					}
					chunk = pending_[i] + '\n';
					count = 1;
				}
				std::FILE* f = nullptr;
				if (fopen_s(&f, path_.c_str(), "ab") != 0 || !f) break;
				const bool ok = std::fwrite(chunk.data(), 1, chunk.size(), f) == chunk.size();
				std::fclose(f);
				if (!ok) break;
				bytes_ += chunk.size();
				i += count;
				written += count;
			}
			pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(i));
			dirty_ = !pending_.empty();
			return written;
		}

	private:
		static int IndexOf(const std::string& fileName)
		{
			// "<prefix><stamp>.csv" = 1, "<prefix><stamp>-NNN.csv" = NNN.
			const size_t at = Prefix().size() + kStampLength;
			if (fileName.size() > at + 4 && fileName[at] == '-')
				return std::atoi(fileName.c_str() + at + 1);
			return 1;
		}

		// Caller holds m_. The next numbered file of this session, with the header and a continuation row.
		bool Rotate()
		{
			const int next = index_ + 1;
			const std::string path = (std::filesystem::path(folder_) / FileName(stamp_, next)).string();
			std::FILE* f = nullptr;
			if (fopen_s(&f, path.c_str(), "wb") != 0 || !f) return false;
			std::string head = std::string(Header()) + "\n";
			head += ",session" + std::string(12, ',') + "session continued in file " + std::to_string(next) + std::string(static_cast<size_t>(kExtraColumns), ',') + "\n";
			std::fwrite(head.data(), 1, head.size(), f);
			std::fclose(f);
			index_ = next;
			path_ = path;
			bytes_ = head.size();
			freshBytes_ = bytes_;
			return true;
		}

		mutable std::mutex m_;
		std::string folder_, stamp_, path_;
		uint64_t rotateBytes_ = kDefaultRotateBytes;
		uint64_t bytes_ = 0;
		uint64_t freshBytes_ = 0;   // the size right after a file was created (header + first / continuation row)
		int index_ = 1;
		bool dirty_ = false;
		std::vector<std::string> pending_;
	};
}
