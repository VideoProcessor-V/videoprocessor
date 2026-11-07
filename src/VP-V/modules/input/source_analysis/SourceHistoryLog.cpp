// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// The source-resolution history CSV writer (see SourceHistoryLog.h). C++14, Win32 file operations with wide paths.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "SourceHistoryLog.h"

#include <algorithm>
#include <cstdio>

namespace SourceHistory
{
	namespace Os
	{
		namespace
		{
			std::wstring Wide(const std::string& s)
			{
				if (s.empty()) return std::wstring();
				const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
				std::wstring w(static_cast<size_t>(n > 0 ? n : 0), L'\0');
				if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
				return w;
			}

			std::string Narrow(const std::wstring& w)
			{
				if (w.empty()) return std::string();
				const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
				std::string s(static_cast<size_t>(n > 0 ? n : 0), '\0');
				if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
				return s;
			}
		}

		std::string Join(const std::string& folder, const std::string& name)
		{
			if (folder.empty()) return name;
			const char last = folder[folder.size() - 1];
			return (last == '\\' || last == '/') ? folder + name : folder + "\\" + name;
		}

		bool CreateDirectories(const std::string& folder)
		{
			if (folder.empty()) return false;
			std::wstring w = Wide(folder);
			// Each component in turn (an existing one is fine).
			for (size_t i = 1; i <= w.size(); ++i)
				if (i == w.size() || w[i] == L'\\' || w[i] == L'/')
				{
					const std::wstring part = w.substr(0, i);
					if (part.size() == 2 && part[1] == L':') continue;
					CreateDirectoryW(part.c_str(), nullptr);   // an existing one (or a UNC / share root) is fine: judged below
				}
			const DWORD attributes = GetFileAttributesW(w.c_str());
			return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
		}

		std::vector<std::string> ListNames(const std::string& folder)
		{
			std::vector<std::string> names;
			WIN32_FIND_DATAW found{};
			HANDLE search = FindFirstFileW(Wide(Join(folder, "*")).c_str(), &found);
			if (search == INVALID_HANDLE_VALUE) return names;
			do
			{
				if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
					names.push_back(Narrow(found.cFileName));
			} while (FindNextFileW(search, &found));
			FindClose(search);
			return names;
		}

		bool Remove(const std::string& path)
		{
			return DeleteFileW(Wide(path).c_str()) != 0;
		}

		bool Write(const std::string& path, const std::string& data, bool append, std::string& error, size_t* writtenOut)
		{
			if (writtenOut) *writtenOut = 0;
			const HANDLE h = CreateFileW(Wide(path).c_str(), append ? FILE_APPEND_DATA : GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
				append ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (h == INVALID_HANDLE_VALUE)
			{
				char text[64];
				snprintf(text, sizeof(text), "cannot open, error %lu", static_cast<unsigned long>(GetLastError()));
				error = text;
				return false;
			}
			size_t done = 0;
			bool ok = true;
			while (done < data.size())
			{
				DWORD wrote = 0;
				const DWORD chunk = static_cast<DWORD>((std::min<size_t>)(data.size() - done, 1u << 20));
				if (!WriteFile(h, data.data() + done, chunk, &wrote, nullptr) || wrote == 0)
				{
					char text[64];
					snprintf(text, sizeof(text), "write error %lu", static_cast<unsigned long>(GetLastError()));
					error = text;
					ok = false;
					break;
				}
				done += wrote;
			}
			CloseHandle(h);
			if (writtenOut) *writtenOut = done;
			return ok;
		}
	}

	std::string Log::FileName(const std::string& stamp, int index)
	{
		char tail[24];
		if (index <= 1) tail[0] = '\0';
		else snprintf(tail, sizeof(tail), "-%03d", index);
		return Prefix() + stamp + tail + ".csv";
	}

	std::string Log::StampOf(const std::string& fileName)
	{
		const std::string prefix = Prefix();
		if (fileName.size() < prefix.size() + kStampLength + 4 || fileName.compare(0, prefix.size(), prefix) != 0)
			return std::string();
		if (fileName.compare(fileName.size() - 4, 4, ".csv") != 0)
			return std::string();
		return fileName.substr(prefix.size(), kStampLength);
	}

	int Log::IndexOf(const std::string& fileName)
	{
		// "<prefix><stamp>.csv" = 1, "<prefix><stamp>-NNN.csv" = NNN.
		const size_t at = Prefix().size() + kStampLength;
		if (fileName.size() > at + 4 && fileName[at] == '-')
			return std::atoi(fileName.c_str() + at + 1);
		return 1;
	}

	std::vector<std::string> Log::SessionFiles(const std::string& folder, const std::string& stamp)
	{
		std::vector<std::string> names;
		const std::vector<std::string> all = Os::ListNames(folder);
		for (size_t i = 0; i < all.size(); ++i)
			if (StampOf(all[i]) == stamp) names.push_back(all[i]);
		// "<stamp>.csv" sorts after "<stamp>-002.csv" ('.' > '-'): order by the rotation index instead.
		std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) { return IndexOf(a) < IndexOf(b); });
		return names;
	}

	int Log::Prune(const std::string& folder, size_t keep)
	{
		std::vector<std::string> stamps;
		const std::vector<std::string> all = Os::ListNames(folder);
		for (size_t i = 0; i < all.size(); ++i)
		{
			const std::string stamp = StampOf(all[i]);
			if (!stamp.empty() && std::find(stamps.begin(), stamps.end(), stamp) == stamps.end()) stamps.push_back(stamp);
		}
		std::sort(stamps.begin(), stamps.end());   // stamps sort by time
		int deleted = 0;
		while (stamps.size() > keep)
		{
			const std::vector<std::string> files = SessionFiles(folder, stamps.front());
			for (size_t i = 0; i < files.size(); ++i)
				Os::Remove(Os::Join(folder, files[i]));
			stamps.erase(stamps.begin());
			++deleted;
		}
		return deleted;
	}

	bool Log::Open(const std::string& folder, const std::string& stamp, const std::string& firstRow, size_t sessions, uint64_t rotateBytes,
		size_t queueRows)
	{
		// No disk work here (the render thread may call this): only the session is recorded, under the short queue lock.
		std::lock_guard<std::mutex> q(q_);
		++gen_;
		pending_.clear();
		pathCopy_.clear();
		indexCopy_ = 1;
		open_ = false;
		initPending_ = false;
		dropped_ = droppedReported_ = failed_ = 0;
		failing_ = failureReported_ = false;
		failureText_.clear();
		if (folder.empty()) return false;
		initFolder_ = folder;
		initStamp_ = stamp;
		initFirstRow_ = firstRow;
		initSessions_ = sessions;
		initRotate_ = rotateBytes > 0 ? rotateBytes : kDefaultRotateBytes;
		queueRows_ = queueRows > 0 ? queueRows : kDefaultQueueRows;
		pathCopy_ = Os::Join(folder, FileName(stamp, 1));
		initPending_ = true;
		open_ = true;
		return true;
	}

	bool Log::IsOpen() const { std::lock_guard<std::mutex> lock(q_); return open_; }
	std::string Log::CurrentPath() const { std::lock_guard<std::mutex> lock(q_); return pathCopy_; }
	int Log::FileIndex() const { std::lock_guard<std::mutex> lock(q_); return indexCopy_; }
	bool Log::Dirty() const { std::lock_guard<std::mutex> lock(q_); return initPending_ || !pending_.empty(); }
	uint64_t Log::DroppedRows() const { std::lock_guard<std::mutex> lock(q_); return dropped_; }
	uint64_t Log::FailedWrites() const { std::lock_guard<std::mutex> lock(q_); return failed_; }

	void Log::Append(const std::string& row)
	{
		std::lock_guard<std::mutex> lock(q_);
		if (!open_) return;
		if (pending_.size() >= queueRows_)
		{
			// Bounded: drop the OLDEST rows (a tenth of the bound at once) and count them; reported by TakeNotice.
			const size_t drop = (std::min)(pending_.size(), queueRows_ / 10 + 1);
			pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(drop));
			dropped_ += drop;
		}
		pending_.push_back(row);
	}

	bool Log::TakeNotice(std::string& text)
	{
		std::lock_guard<std::mutex> lock(q_);
		if (failing_ && !failureReported_)
		{
			failureReported_ = true;
			text = "source history CSV: write failed (" + failureText_ + ")";
			return true;
		}
		if (dropped_ > droppedReported_)
		{
			text = "source history CSV: rows dropped: " + std::to_string(dropped_ - droppedReported_);
			droppedReported_ = dropped_;
			return true;
		}
		return false;
	}

	std::string Log::Status() const
	{
		std::lock_guard<std::mutex> lock(q_);
		std::string s;
		if (failing_) s = "source history CSV: write failed (" + failureText_ + ")";
		if (dropped_ > 0) s += std::string(s.empty() ? "" : "; ") + "source history CSV: rows dropped: " + std::to_string(dropped_);
		return s;
	}

	size_t Log::Flush()
	{
		std::lock_guard<std::mutex> io(io_);
		std::vector<std::string> rows;
		bool doInit = false;
		uint64_t gen = 0;
		std::string folder, stamp, firstRow;
		size_t sessions = kDefaultSessions;
		uint64_t rotate = kDefaultRotateBytes;
		{
			std::lock_guard<std::mutex> q(q_);
			if (!open_ || (!initPending_ && pending_.empty())) return 0;
			gen = gen_;
			if (initPending_)
			{
				doInit = true;
				folder = initFolder_; stamp = initStamp_; firstRow = initFirstRow_; sessions = initSessions_; rotate = initRotate_;
			}
			rows.swap(pending_);
		}
		size_t written = 0;
		size_t i = 0;
		bool failed = false;
		bool initFailed = false;
		std::string error;
		if (doInit)
		{
			// The session start Open recorded: folder, retention, then the base file with the header and the first row.
			if (!Os::CreateDirectories(folder))
			{
				failed = initFailed = true;
				error = "cannot create the folder " + folder;
			}
			else
			{
				const std::vector<std::string> same = SessionFiles(folder, stamp);
				for (size_t k = 0; k < same.size(); ++k)
					Os::Remove(Os::Join(folder, same[k]));   // a restart inside the same second reuses the stamp
				Prune(folder, sessions > 0 ? sessions - 1 : 0);
				const std::string head = std::string(Header()) + "\n" + firstRow + "\n";
				const std::string path = Os::Join(folder, FileName(stamp, 1));
				if (!Os::Write(path, head, false, error))
					failed = initFailed = true;
				else
				{
					folder_ = folder;
					stamp_ = stamp;
					rotateBytes_ = rotate;
					index_ = 1;
					path_ = path;
					bytes_ = head.size();
					freshBytes_ = bytes_;
				}
			}
		}
		while (!failed && i < rows.size())
		{
			// As many rows as fit below the limit, in one write.
			std::string chunk;
			size_t count = 0;
			while (i + count < rows.size() && bytes_ + chunk.size() + rows[i + count].size() + 1 <= rotateBytes_)
			{
				chunk += rows[i + count];
				chunk += '\n';
				++count;
			}
			if (count == 0)
			{
				// The next row does not fit: continue in the next numbered file (a row never straddles files). A row
				// larger than the whole limit still goes into a fresh file, alone.
				if (bytes_ > freshBytes_)
				{
					if (!Rotate()) { failed = true; error = "cannot start the next file"; break; }
					continue;
				}
				chunk = rows[i] + '\n';
				count = 1;
			}
			size_t wrote = 0;
			if (!Os::Write(path_, chunk, true, error, &wrote))
			{
				// A partial write: the rows that reached the file whole are done (no duplicate); the cut row is retried whole.
				size_t whole = 0, at = 0;
				for (size_t k = 0; k < count; ++k)
				{
					const size_t end = at + rows[i + k].size() + 1;
					if (end > wrote) break;
					at = end;
					++whole;
				}
				bytes_ += wrote;
				i += whole;
				written += whole;
				failed = true;
				break;
			}
			bytes_ += chunk.size();
			i += count;
			written += count;
		}
		std::lock_guard<std::mutex> q(q_);
		if (gen != gen_) return written;   // Open started another session meanwhile: its state is not touched
		indexCopy_ = index_;
		if (!path_.empty() && !initPending_) pathCopy_ = path_;
		if (doInit && !initFailed) initPending_ = false;   // (Open has not run again: gen matches)
		if (failed)
		{
			++failed_;
			if (!failing_) failureReported_ = false;   // a new failure episode is reported once
			failing_ = true;
			failureText_ = error;
			// The unwritten rows go back in front of the newer ones, within the bound (the oldest are dropped, counted).
			pending_.insert(pending_.begin(), rows.begin() + static_cast<std::ptrdiff_t>(i), rows.end());
			if (pending_.size() > queueRows_)
			{
				const size_t drop = pending_.size() - queueRows_;
				pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(drop));
				dropped_ += drop;
			}
		}
		else
			failing_ = false;
		return written;
	}

	// Caller holds io_. The next numbered file of this session, with the header and a continuation row.
	bool Log::Rotate()
	{
		const int next = index_ + 1;
		const std::string path = Os::Join(folder_, FileName(stamp_, next));
		std::string head = std::string(Header()) + "\n";
		head += ",session" + std::string(12, ',') + "session continued in file " + std::to_string(next) + std::string(static_cast<size_t>(kExtraColumns), ',') + "\n";
		std::string error;
		if (!Os::Write(path, head, false, error)) return false;
		index_ = next;
		path_ = path;
		bytes_ = head.size();
		freshBytes_ = bytes_;
		return true;
	}
}
