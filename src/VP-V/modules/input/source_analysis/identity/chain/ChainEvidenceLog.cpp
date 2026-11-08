// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// The chain estimator's evidence log (see ChainEvidenceLog.h). C++14, Win32 file operations with wide paths. Worker-side only.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "ChainEvidenceLog.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace SourceIdentity
{
	namespace Chain
	{
		// ---- level names -------------------------------------------------------------------------------------------------
		const char* EvidenceLevelText(EvidenceLevel level)
		{
			switch (level)
			{
			case EvidenceLevel::Off: return "off";
			case EvidenceLevel::Log: return "log";
			case EvidenceLevel::Lines: return "log+lines";
			case EvidenceLevel::Frames: return "log+lines+frames";
			}
			return "off";
		}

		bool ParseEvidenceLevel(const std::string& text, EvidenceLevel& out)
		{
			std::string t;
			for (char c : text)
				if (c != ' ' && c != '\t')
					t += static_cast<char>((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
			if (t == "off") { out = EvidenceLevel::Off; return true; }
			if (t == "log") { out = EvidenceLevel::Log; return true; }
			if (t == "log+lines") { out = EvidenceLevel::Lines; return true; }
			if (t == "log+lines+frames") { out = EvidenceLevel::Frames; return true; }
			return false;
		}

		// ---- the Win32 disk -----------------------------------------------------------------------------------------------
		namespace
		{
			std::wstring Wide(const std::string& s)
			{
				if (s.empty())
					return std::wstring();
				const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
				std::wstring w(static_cast<size_t>(n > 0 ? n : 0), L'\0');
				if (n > 0)
					MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
				return w;
			}

			std::string Narrow(const std::wstring& w)
			{
				if (w.empty())
					return std::string();
				const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
				std::string s(static_cast<size_t>(n > 0 ? n : 0), '\0');
				if (n > 0)
					WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
				return s;
			}

			std::string LastError(const char* what)
			{
				char b[96];
				std::snprintf(b, sizeof(b), "%s (error %lu)", what, static_cast<unsigned long>(GetLastError()));
				return b;
			}

			class DiskFs : public IEvidenceFs
			{
			public:
				bool CreateDirs(const std::string& folder, std::string& error) override
				{
					std::wstring w = Wide(folder);
					if (w.empty())
					{
						error = "no folder";
						return false;
					}
					for (size_t i = 1; i < w.size(); ++i)
						if ((w[i] == L'\\' || w[i] == L'/') && w[i - 1] != L':')
						{
							const std::wstring prefix = w.substr(0, i);
							if (!CreateDirectoryW(prefix.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
							{
								// a missing parent is created by the next prefix; a real failure shows at the last call
							}
						}
					if (!CreateDirectoryW(w.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
					{
						error = LastError("cannot create the evidence folder");
						return false;
					}
					const DWORD attrs = GetFileAttributesW(w.c_str());
					if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0)
					{
						error = "the evidence folder is not a folder";
						return false;
					}
					return true;
				}

				bool Write(const std::string& path, const std::string& data, bool append, std::string& error) override
				{
					const std::wstring w = Wide(path);
					const HANDLE h = CreateFileW(w.c_str(), append ? FILE_APPEND_DATA : GENERIC_WRITE, FILE_SHARE_READ, nullptr,
						append ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
					if (h == INVALID_HANDLE_VALUE)
					{
						error = LastError("cannot open the evidence file");
						return false;
					}
					size_t done = 0;
					bool ok = true;
					while (done < data.size())
					{
						const DWORD chunk = static_cast<DWORD>((std::min)(data.size() - done, static_cast<size_t>(8u * 1024u * 1024u)));
						DWORD wrote = 0;
						if (!WriteFile(h, data.data() + done, chunk, &wrote, nullptr) || wrote == 0)
						{
							ok = false;
							error = LastError("cannot write the evidence file");
							break;
						}
						done += wrote;
					}
					CloseHandle(h);
					return ok;
				}

				std::vector<FileInfo> List(const std::string& folder) override
				{
					std::vector<FileInfo> out;
					const std::wstring pattern = Wide(JoinPath(folder, "*"));
					WIN32_FIND_DATAW fd;
					const HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
					if (h == INVALID_HANDLE_VALUE)
						return out;
					do
					{
						if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
							continue;
						FileInfo f;
						f.name = Narrow(fd.cFileName);
						f.bytes = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | static_cast<uint64_t>(fd.nFileSizeLow);
						out.push_back(f);
					} while (FindNextFileW(h, &fd));
					FindClose(h);
					std::sort(out.begin(), out.end(), [](const FileInfo& a, const FileInfo& b) { return a.name < b.name; });
					return out;
				}

				bool Remove(const std::string& path) override { return DeleteFileW(Wide(path).c_str()) != 0; }

				bool Read(const std::string& path, std::string& data) override
				{
					const HANDLE h = CreateFileW(Wide(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
						OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
					if (h == INVALID_HANDLE_VALUE)
						return false;
					data.clear();
					char buf[8192];
					for (;;)
					{
						DWORD got = 0;
						if (!ReadFile(h, buf, sizeof(buf), &got, nullptr) || got == 0)
							break;
						data.append(buf, got);
						if (data.size() > 16u * 1024u * 1024u)
							break;
					}
					CloseHandle(h);
					return true;
				}
			};
		}

		std::shared_ptr<IEvidenceFs> MakeDiskEvidenceFs() { return std::make_shared<DiskFs>(); }

		std::string JoinPath(const std::string& folder, const std::string& name)
		{
			if (folder.empty())
				return name;
			const char last = folder[folder.size() - 1];
			return (last == '\\' || last == '/') ? folder + name : folder + "\\" + name;
		}

		std::string LocalStamp()
		{
			SYSTEMTIME t{};
			GetLocalTime(&t);
			char b[32];
			std::snprintf(b, sizeof(b), "%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
			return b;
		}

		// ---- the dump and frame formats ------------------------------------------------------------------------------------
		namespace
		{
			void PutU32(std::string& s, uint32_t v)
			{
				for (int i = 0; i < 4; ++i)
					s += static_cast<char>((v >> (8 * i)) & 0xFFu);
			}
			void PutU64(std::string& s, uint64_t v)
			{
				for (int i = 0; i < 8; ++i)
					s += static_cast<char>((v >> (8 * i)) & 0xFFu);
			}
			void PutI32(std::string& s, int32_t v) { PutU32(s, static_cast<uint32_t>(v)); }
			void PutU16Array(std::string& s, const std::vector<uint16_t>& v)
			{
				for (uint16_t x : v)
				{
					s += static_cast<char>(x & 0xFFu);
					s += static_cast<char>((x >> 8) & 0xFFu);
				}
			}

			struct Rd
			{
				const unsigned char* p;
				size_t n;
				size_t pos = 0;
				bool ok = true;
				Rd(const std::string& s) : p(reinterpret_cast<const unsigned char*>(s.data())), n(s.size()) {}
				size_t Left() const { return n - pos; }
				uint32_t U32()
				{
					if (Left() < 4) { ok = false; return 0; }
					uint32_t v = 0;
					for (int i = 0; i < 4; ++i)
						v |= static_cast<uint32_t>(p[pos + static_cast<size_t>(i)]) << (8 * i);
					pos += 4;
					return v;
				}
				uint64_t U64()
				{
					if (Left() < 8) { ok = false; return 0; }
					uint64_t v = 0;
					for (int i = 0; i < 8; ++i)
						v |= static_cast<uint64_t>(p[pos + static_cast<size_t>(i)]) << (8 * i);
					pos += 8;
					return v;
				}
				int32_t I32() { return static_cast<int32_t>(U32()); }
				void U16Array(std::vector<uint16_t>& v, size_t count)
				{
					if (Left() / 2 < count) { ok = false; return; }
					v.resize(count);
					for (size_t i = 0; i < count; ++i)
					{
						v[i] = static_cast<uint16_t>(p[pos] | (static_cast<uint16_t>(p[pos + 1]) << 8));
						pos += 2;
					}
				}
			};
		}

		std::string SerializeDump(const std::string& headerJson, const std::vector<std::shared_ptr<const LineSnapshot>>& evals)
		{
			std::string s;
			s += "SIDL";
			PutU32(s, 2);
			PutU32(s, static_cast<uint32_t>(headerJson.size()));
			s += headerJson;
			PutU32(s, static_cast<uint32_t>(evals.size()));
			for (const std::shared_ptr<const LineSnapshot>& ep : evals)
			{
				if (!ep)
					continue;
				const LineSnapshot& e = *ep;
				PutU64(s, e.seq);
				PutU64(s, static_cast<uint64_t>(e.timeUs));
				PutU64(s, e.streamKey);
				PutU32(s, e.flags);
				PutI32(s, e.cuts);
				PutU64(s, static_cast<uint64_t>(e.eventTimeUs));
				PutI32(s, e.active.x); PutI32(s, e.active.y); PutI32(s, e.active.w); PutI32(s, e.active.h);
				PutI32(s, e.inner.x); PutI32(s, e.inner.y); PutI32(s, e.inner.w); PutI32(s, e.inner.h);
				PutU32(s, static_cast<uint32_t>(e.rowCount));
				PutU32(s, static_cast<uint32_t>(e.colCount));
				for (int32_t v : e.rowY) PutI32(s, v);
				for (int32_t v : e.colX) PutI32(s, v);
				PutU16Array(s, e.rows);
				PutU16Array(s, e.cols);
			}
			return s;
		}

		bool ParseDump(const std::string& bytes, std::string& headerJson, std::vector<LineSnapshot>& evals)
		{
			evals.clear();
			headerJson.clear();
			if (bytes.size() < 16 || std::memcmp(bytes.data(), "SIDL", 4) != 0)
				return false;
			Rd r(bytes);
			r.pos = 4;
			if (r.U32() != 2u)
				return false;
			const uint32_t headerLen = r.U32();
			if (!r.ok || r.Left() < headerLen)
				return false;
			headerJson.assign(bytes, r.pos, headerLen);
			r.pos += headerLen;
			const uint32_t count = r.U32();
			if (!r.ok)
				return false;
			for (uint32_t k = 0; k < count; ++k)
			{
				LineSnapshot e;
				e.seq = r.U64();
				e.timeUs = static_cast<int64_t>(r.U64());
				e.streamKey = r.U64();
				e.flags = r.U32();
				e.cuts = r.I32();
				e.eventTimeUs = static_cast<int64_t>(r.U64());
				e.active.x = r.I32(); e.active.y = r.I32(); e.active.w = r.I32(); e.active.h = r.I32();
				e.inner.x = r.I32(); e.inner.y = r.I32(); e.inner.w = r.I32(); e.inner.h = r.I32();
				const uint32_t rc = r.U32();
				const uint32_t cc = r.U32();
				if (!r.ok || rc > static_cast<uint32_t>(kMaxLines) || cc > static_cast<uint32_t>(kMaxLines) || e.inner.w < 0 || e.inner.h < 0)
					return false;
				e.rowCount = static_cast<int>(rc);
				e.colCount = static_cast<int>(cc);
				e.rowY.resize(rc);
				e.colX.resize(cc);
				for (uint32_t i = 0; i < rc; ++i) e.rowY[i] = r.I32();
				for (uint32_t i = 0; i < cc; ++i) e.colX[i] = r.I32();
				r.U16Array(e.rows, static_cast<size_t>(rc) * static_cast<size_t>(e.inner.w));
				r.U16Array(e.cols, static_cast<size_t>(cc) * static_cast<size_t>(e.inner.h));
				if (!r.ok)
					return false;
				evals.push_back(std::move(e));
			}
			return true;
		}

		std::string SerializeFrame(const FrameRef& f)
		{
			std::string s;
			if (!f.Valid())
				return s;
			s += "SIDF";
			PutU32(s, 1);
			PutU64(s, f.seq);
			PutU32(s, f.width);
			PutU32(s, f.height);
			PutU32(s, f.msbShift);
			s.reserve(s.size() + static_cast<size_t>(f.width) * f.height * 2);
			for (uint32_t y = 0; y < f.height; ++y)
			{
				const uint16_t* row = f.luma + static_cast<size_t>(y) * f.strideSamples;
				for (uint32_t x = 0; x < f.width; ++x)
				{
					s += static_cast<char>(row[x] & 0xFFu);
					s += static_cast<char>((row[x] >> 8) & 0xFFu);
				}
			}
			return s;
		}

		// ---- a tiny JSON writer ----------------------------------------------------------------------------------------------
		namespace
		{
			class Json
			{
			public:
				std::string s;
				Json& Obj() { Sep(); s += '{'; stack_.push_back(false); return *this; }
				Json& EndObj() { s += '}'; stack_.pop_back(); return *this; }
				Json& Arr() { Sep(); s += '['; stack_.push_back(false); return *this; }
				Json& EndArr() { s += ']'; stack_.pop_back(); return *this; }
				Json& Key(const char* k) { Sep(); Quote(k); s += ':'; afterKey_ = true; return *this; }
				Json& Str(const std::string& v) { Sep(); Quote(v); return *this; }
				Json& Num(double v)
				{
					Sep();
					if (!std::isfinite(v))
						s += "null";
					else
					{
						char b[48];
						std::snprintf(b, sizeof(b), "%.6g", v);
						s += b;
					}
					return *this;
				}
				Json& Int(int64_t v)
				{
					Sep();
					char b[32];
					std::snprintf(b, sizeof(b), "%lld", static_cast<long long>(v));
					s += b;
					return *this;
				}
				Json& UInt(uint64_t v)
				{
					Sep();
					char b[32];
					std::snprintf(b, sizeof(b), "%llu", static_cast<unsigned long long>(v));
					s += b;
					return *this;
				}
				Json& Bool(bool v) { Sep(); s += v ? "true" : "false"; return *this; }
				Json& Null() { Sep(); s += "null"; return *this; }

			private:
				void Sep()
				{
					if (afterKey_)
					{
						afterKey_ = false;
						return;
					}
					if (!stack_.empty())
					{
						if (stack_.back())
							s += ',';
						stack_.back() = true;
					}
				}
				void Quote(const std::string& v)
				{
					s += '"';
					for (char c : v)
					{
						const unsigned char u = static_cast<unsigned char>(c);
						if (c == '"') s += "\\\"";
						else if (c == '\\') s += "\\\\";
						else if (u < 0x20)
						{
							char b[8];
							std::snprintf(b, sizeof(b), "\\u%04x", static_cast<unsigned>(u));
							s += b;
						}
						else s += c;
					}
					s += '"';
				}
				std::vector<bool> stack_;
				bool afterKey_ = false;
			};

			const char* AxisName(int a) { return a == 0 ? "H" : "V"; }

			std::string KernelName(int index) { return index >= 0 ? std::string(Kernel(index).name) : std::string("unsure"); }

			std::string Sanitize(const std::string& s)
			{
				std::string o;
				for (char c : s)
				{
					const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '_' || c == '-';
					o += ok ? c : '_';
					if (o.size() >= 40)
						break;
				}
				return o.empty() ? std::string("dump") : o;
			}

			void WriteAxisGeom(Json& j, const LogAxis& a, bool applied)
			{
				j.Obj();
				j.Key("state").Str(Geometry::StateText(applied ? a.appliedState : a.state));
				j.Key("n").Int(applied ? a.appliedN : a.measuredN);
				j.Key("N").Int(a.N);
				j.Key("kernel").Str(KernelName(applied ? a.appliedKernel : a.measuredKernel));
				j.Key("llr").Num(applied ? a.appliedLlr : a.measuredLlr);
				j.EndObj();
			}

			void WriteCandidates(Json& j, const LogAxis& a)
			{
				j.Arr();
				for (int i = 0; i < a.candCount && i < 3; ++i)
				{
					j.Obj();
					j.Key("n").Int(a.top[i].n);
					j.Key("stat").Num(a.top[i].stat);
					j.Key("inc").Num(a.top[i].increment);
					j.Key("kernel").Str(KernelName(a.top[i].kernel));
					j.Key("familyMargin").Num(a.top[i].familyMargin);
					j.Key("informative").Bool(a.top[i].informative);
					j.EndObj();
				}
				j.EndArr();
			}
		}

		// ---- EvidenceLog -------------------------------------------------------------------------------------------------
		EvidenceLog::EvidenceLog(const EvidenceLogSettings& settings, std::shared_ptr<IEvidenceFs> fs, NowMsFn nowMs, StampFn stamp)
			: s_(settings), fs_(std::move(fs)), nowMs_(std::move(nowMs)), stamp_(std::move(stamp))
		{
			if (!fs_)
				fs_ = MakeDiskEvidenceFs();
			if (!nowMs_)
				nowMs_ = []() { return 0.0; };
			if (!stamp_)
				stamp_ = &LocalStamp;
		}

		void EvidenceLog::Configure(const std::string& folder, EvidenceLevel level, bool active, const std::string& modeText)
		{
			const bool wantActive = active && level != EvidenceLevel::Off && !folder.empty();
			if (!wantActive)
			{
				if (active_)
					Close(haveLastEval_ ? lastEval_.timeUs : 0, "evidence log off");
				active_ = false;
				level_ = level;
				ring_.clear();
				frames_.clear();
				probes_.clear();
				noticeGiven_ = false;   // a later start announces itself again
				// A level was asked for while the estimator runs, but there is nowhere to write: said on the status line and once as a notice.
				noFolder_ = active && level != EvidenceLevel::Off && folder.empty();
				if (noFolder_ && !noFolderNoticed_)
				{
					noFolderNoticed_ = true;
					Notice(std::string("Source identity: evidence logging (") + EvidenceLevelText(level) + ") requested but no history folder is set: nothing is logged");
				}
				if (!noFolder_)
					noFolderNoticed_ = false;
				PublishStatus();
				return;
			}
			if (active_ && folder == folder_ && level == level_ && modeText == mode_)
				return;
			const bool folderChanged = folder != folder_;
			if (active_ && folderChanged)
				Close(haveLastEval_ ? lastEval_.timeUs : 0, "evidence folder changed");
			folder_ = folder;
			mode_ = modeText;
			if (!active_ || folderChanged)
			{
				session_ = stamp_();
				logName_.clear();
				logBytes_ = 0;
				logIndex_ = 0;
				dumpIndex_ = 0;
				segmentIndex_ = 0;
				folderLoaded_ = false;
				sizes_.clear();
				coverage_.clear();
				coverageDirty_ = false;
			}
			level_ = level;
			active_ = true;
			noFolder_ = false;
			noFolderNoticed_ = false;
			if (!WantsLines())
				ring_.clear();
			if (!WantsFrames())
				frames_.clear();
			if (!noticeGiven_)
			{
				noticeGiven_ = true;
				Notice("Source identity: evidence logging on (" + modeText + ")");
			}
			PublishStatus();
		}

		std::string EvidenceLog::Name(const std::string& suffix) const { return session_ + "-" + suffix; }

		void EvidenceLog::Notice(const std::string& text)
		{
			std::lock_guard<std::mutex> g(pub_);
			notices_.push_back(text);
		}

		void EvidenceLog::PostNotice(const std::string& text) { Notice(text); }

		bool EvidenceLog::TakeNotice(std::string& text)
		{
			std::lock_guard<std::mutex> g(pub_);
			if (notices_.empty())
				return false;
			text = notices_.front();
			notices_.erase(notices_.begin());
			return true;
		}

		EvidenceStatus EvidenceLog::GetStatus() const
		{
			std::lock_guard<std::mutex> g(pub_);
			return status_;
		}

		void EvidenceLog::PublishStatus()
		{
			uint64_t bytes = 0;
			for (const auto& f : sizes_)
				bytes += f.second;
			std::lock_guard<std::mutex> g(pub_);
			status_.active = active_;
			status_.level = level_;
			status_.folder = folder_;
			status_.bytes = bytes;
			status_.files = static_cast<int>(sizes_.size());
			status_.noFolder = noFolder_;
		}

		std::string EvidenceLog::StatusText() const
		{
			const EvidenceStatus st = GetStatus();
			if (!st.error.empty())
				return "evidence: write failed (" + st.error + ")";
			if (!st.active)
			{
				if (st.noFolder)
					return std::string("evidence: ") + EvidenceLevelText(st.level) + " requested, no folder (set the history folder)";
				return "evidence: off";
			}
			char b[200];
			std::snprintf(b, sizeof(b), "evidence: %s, %.1f MB in sid-evidence (cap %.0f MB), %llu dumps", EvidenceLevelText(st.level),
				static_cast<double>(st.bytes) / (1024.0 * 1024.0), static_cast<double>(s_.capBytes) / (1024.0 * 1024.0),
				static_cast<unsigned long long>(st.dumps));
			return b;
		}

		void EvidenceLog::NoteFailure(const std::string& error)
		{
			bool first = false;
			{
				std::lock_guard<std::mutex> g(pub_);
				first = status_.error.empty();
				status_.error = error;
				++status_.failedWrites;
			}
			if (first)
				Notice("Source identity: evidence log write failed (" + error + ")");
		}

		// ---- disk ---------------------------------------------------------------------------------------------------------
		void EvidenceLog::LoadFolder()
		{
			if (folderLoaded_)
				return;
			std::string err;
			if (!fs_->CreateDirs(folder_, err))
			{
				NoteFailure(err);
				return;
			}
			sizes_.clear();
			for (const FileInfo& f : fs_->List(folder_))
				if (!f.name.empty() && f.name[0] >= '0' && f.name[0] <= '9')
					sizes_[f.name] = f.bytes;
			LoadCoverage();
			folderLoaded_ = true;
			EnforceCap();
		}

		void EvidenceLog::LoadCoverage()
		{
			std::string data;
			if (!fs_->Read(JoinPath(folder_, "coverage.txt"), data))
				return;
			size_t pos = 0;
			while (pos < data.size())
			{
				size_t end = data.find('\n', pos);
				if (end == std::string::npos)
					end = data.size();
				const std::string line = data.substr(pos, end - pos);
				pos = end + 1;
				const size_t sp = line.rfind(' ');
				if (sp == std::string::npos || sp == 0)
					continue;
				const int count = std::atoi(line.c_str() + sp + 1);
				if (count > 0)
					coverage_[line.substr(0, sp)] = count;
			}
		}

		void EvidenceLog::SaveCoverage()
		{
			if (!coverageDirty_ || !folderLoaded_)
				return;
			std::string data;
			for (const auto& c : coverage_)
				data += c.first + " " + std::to_string(c.second) + "\n";
			std::string err;
			if (!fs_->Write(JoinPath(folder_, "coverage.txt"), data, false, err))
				NoteFailure(err);
			else
				coverageDirty_ = false;
		}

		bool EvidenceLog::WriteFile(const std::string& name, const std::string& data, bool append)
		{
			LoadFolder();
			if (!folderLoaded_)
				return false;
			std::string err;
			if (!fs_->Write(JoinPath(folder_, name), data, append, err))
			{
				NoteFailure(err);
				return false;
			}
			{
				std::lock_guard<std::mutex> g(pub_);
				status_.error.clear();   // a successful write ends the failure episode
			}
			if (append)
				sizes_[name] += data.size();
			else
				sizes_[name] = data.size();
			EnforceCap();
			return true;
		}

		void EvidenceLog::EnforceCap()
		{
			uint64_t total = 0;
			for (const auto& f : sizes_)
				total += f.second;
			uint64_t removed = 0;
			while (total > s_.capBytes)
			{
				auto it = sizes_.begin();
				while (it != sizes_.end() && it->first == logName_)
					++it;
				if (it == sizes_.end())
				{
					// Only the open log file is left and it alone exceeds the cap: the next row starts a new file.
					logName_.clear();
					break;
				}
				fs_->Remove(JoinPath(folder_, it->first));
				total -= it->second;
				sizes_.erase(it);
				++removed;
			}
			if (removed != 0)
			{
				std::lock_guard<std::mutex> g(pub_);
				status_.evicted += removed;
			}
		}

		void EvidenceLog::AppendRow(const std::string& json)
		{
			pending_ += json;
			pending_ += '\n';
			std::lock_guard<std::mutex> g(pub_);
			++status_.rows;
		}

		void EvidenceLog::FlushRows()
		{
			if (pending_.empty())
				return;
			if (logName_.empty() || logBytes_ >= s_.rotateBytes)
			{
				++logIndex_;
				char b[32];
				std::snprintf(b, sizeof(b), "log-%03d.jsonl", logIndex_);
				logName_ = Name(b);
				logBytes_ = 0;
			}
			if (WriteFile(logName_, pending_, true))
			{
				logBytes_ += pending_.size();
				pending_.clear();
			}
			else if (pending_.size() > 1024u * 1024u)
				pending_.clear();   // the disk is failing (visible through the status): never grow without bound
		}

		// ---- segments ------------------------------------------------------------------------------------------------------
		void EvidenceLog::OpenSegment(const LogEval& e)
		{
			seg_ = Segment();
			seg_.open = true;
			seg_.startUs = e.timeUs;
			seg_.endUs = e.timeUs;
			seg_.stamp = stamp_();
			seg_.streamKey = e.streamKey;
			seg_.active = e.active;
			seg_.format = format_;
			seg_.firstSeq = e.seq;
			seg_.repeatsAtStart = e.repeatsSkipped;
			seg_.repeatsAtEnd = e.repeatsSkipped;
			++segmentIndex_;
		}

		void EvidenceLog::AccumulateSegment(const LogEval& e)
		{
			Segment& g = seg_;
			const int64_t dt = e.timeUs - g.endUs;
			g.endUs = e.timeUs;
			g.lastSeq = e.seq;
			++g.evals;
			if (g.costs.size() < 4096)
				g.costs.push_back(e.costMs);
			g.costSum += e.costMs;
			g.repeatsAtEnd = e.repeatsSkipped;
			g.intervalSec = e.intervalSec;
			g.stepCodes = e.stepCodes;
			g.active = e.active;
			g.classicalValid = e.classicalValid;
			for (int a = 0; a < 2; ++a)
			{
				g.classicalRatio[a] = e.classicalRatio[a];
				SegAxis& A = g.axis[a];
				const LogAxis& L = e.axis[a];
				if (A.haveLast && dt > 0)
					A.stateUs[static_cast<int>(A.lastState)] += dt;
				A.lastState = L.state;
				A.haveLast = true;
				++A.evals;
				if (L.informative)
					++A.informative;
				if (L.lines > 0)
				{
					++A.gateEvals;
					A.gatedShareSum += static_cast<double>(L.gatedLines) / static_cast<double>(L.lines);
					if (L.gatedLines > 0)
						A.maskedShareSum += static_cast<double>(L.maskedLines) / static_cast<double>(L.gatedLines);
				}
				if (L.probeRan)
				{
					++A.probeRan;
					if (L.probeSeen)
						++A.probeSeen;
				}
				A.last = L;
				g.chainConfident[a] = L.appliedState == Geometry::LatchState::Latched && L.appliedN > 0 && L.N > 0;
				g.chainRatio[a] = g.chainConfident[a] ? static_cast<double>(L.appliedN) / static_cast<double>(L.N) : 1.0;
			}
		}

		void EvidenceLog::CloseSegment(int64_t timeUs, const std::string& why)
		{
			if (!seg_.open)
				return;
			Segment& g = seg_;
			g.open = false;
			Json j;
			j.Obj();
			j.Key("t").Str("segment");
			j.Key("session").Str(session_);
			j.Key("n").Int(segmentIndex_);
			j.Key("end").Str(why);
			j.Key("wall").Str(g.stamp);
			j.Key("srcStartUs").Int(g.startUs);
			j.Key("srcEndUs").Int((std::max)(timeUs, g.endUs));
			j.Key("era").UInt(g.streamKey);
			j.Key("format").Str(g.format);
			j.Key("outputStepCodes").Int(g.stepCodes);
			j.Key("active").Arr().Int(g.active.x).Int(g.active.y).Int(g.active.w).Int(g.active.h).EndArr();
			j.Key("seq").Arr().UInt(g.firstSeq).UInt(g.lastSeq).EndArr();
			j.Key("evals").Int(g.evals);
			j.Key("cadence").Obj();
			j.Key("intervalSec").Num(g.intervalSec);
			j.Key("repeatsSkipped").UInt(g.repeatsAtEnd >= g.repeatsAtStart ? g.repeatsAtEnd - g.repeatsAtStart : 0u);
			j.EndObj();
			j.Key("axis").Arr();
			for (int a = 0; a < 2; ++a)
			{
				const SegAxis& A = g.axis[a];
				j.Obj();
				j.Key("name").Str(AxisName(a));
				j.Key("stateUs").Obj();
				for (int s = 0; s < static_cast<int>(Geometry::LatchState::Count); ++s)
					j.Key(Geometry::StateText(static_cast<Geometry::LatchState>(s))).Int(A.stateUs[s]);
				j.EndObj();
				j.Key("measured");
				WriteAxisGeom(j, A.last, false);
				j.Key("applied");
				WriteAxisGeom(j, A.last, true);
				j.Key("llr").Num(A.last.measuredLlr);
				j.Key("top");
				WriteCandidates(j, A.last);
				j.Key("probeSeenShare");
				if (A.probeRan > 0) j.Num(static_cast<double>(A.probeSeen) / static_cast<double>(A.probeRan)); else j.Null();
				j.Key("lineGateShare");
				if (A.gateEvals > 0) j.Num(A.gatedShareSum / static_cast<double>(A.gateEvals)); else j.Null();
				j.Key("spatialMaskedShare");
				if (A.gateEvals > 0) j.Num(A.maskedShareSum / static_cast<double>(A.gateEvals)); else j.Null();
				j.Key("informativeShare").Num(A.evals > 0 ? static_cast<double>(A.informative) / static_cast<double>(A.evals) : 0.0);
				j.EndObj();
			}
			j.EndArr();
			bool disagree = false;
			for (int a = 0; a < 2; ++a)
				if (g.classicalValid && g.chainConfident[a])
				{
					const double c = g.classicalRatio[a];
					const double m = (std::max)(c, g.chainRatio[a]);
					if (m > 0.0 && std::fabs(c - g.chainRatio[a]) > s_.disagreeTolerance * m)
						disagree = true;
				}
			j.Key("classical").Obj();
			j.Key("ratio");
			if (g.classicalValid) j.Arr().Num(g.classicalRatio[0]).Num(g.classicalRatio[1]).EndArr(); else j.Null();
			j.Key("wouldBeRatio");
			if (g.chainConfident[0] || g.chainConfident[1]) j.Arr().Num(g.chainRatio[0]).Num(g.chainRatio[1]).EndArr(); else j.Null();
			j.Key("disagree").Bool(disagree);
			j.EndObj();
			// Not wired yet (reserved, null): the encoder tier, the ABR events, the would-be notices and the quality-pattern summary.
			j.Key("tier").Null();
			j.Key("abrEvents").Null();
			j.Key("wouldBeNotices").Null();
			j.Key("qualityPattern").Null();
			double p95 = 0.0;
			if (!g.costs.empty())
			{
				std::vector<double> c = g.costs;
				std::sort(c.begin(), c.end());
				size_t idx = static_cast<size_t>(std::ceil(0.95 * static_cast<double>(c.size())));
				idx = idx == 0 ? 0 : idx - 1;
				p95 = c[(std::min)(idx, c.size() - 1)];
			}
			j.Key("cost").Obj();
			j.Key("meanMs").Num(g.evals > 0 ? g.costSum / static_cast<double>(g.evals) : 0.0);
			j.Key("p95Ms").Num(p95);
			j.EndObj();
			j.EndObj();
			AppendRow(j.s);
		}

		void EvidenceLog::WriteStateRows(const LogEval& e)
		{
			for (int a = 0; a < 2; ++a)
			{
				const LogAxis& L = e.axis[a];
				if (!L.stateChanged && !L.appliedChanged)
					continue;
				Json j;
				j.Obj();
				j.Key("t").Str("state");
				j.Key("srcUs").Int(e.timeUs);
				j.Key("seq").UInt(e.seq);
				j.Key("axis").Str(AxisName(a));
				j.Key("state").Str(Geometry::StateText(L.state));
				j.Key("reason").Str(ReadingReasonText(static_cast<ReadingReason>(L.readingReason < 0 ? 0 : (L.readingReason >= static_cast<int>(ReadingReason::Count) ? 0 : L.readingReason))));
				j.Key("reasonBits").UInt(L.reasonBits);
				j.Key("measured");
				WriteAxisGeom(j, L, false);
				j.Key("applied");
				WriteAxisGeom(j, L, true);
				j.Key("kernelConf").Num(L.kernelConf);
				j.Key("c1").Num(e.c1);
				j.Key("candidates");
				WriteCandidates(j, L);
				j.Key("probe").Obj();
				j.Key("ran").Bool(L.probeRan);
				j.Key("seen").Bool(L.probeSeen);
				j.EndObj();
				j.Key("lines").Obj();
				j.Key("total").Int(L.lines);
				j.Key("gated").Int(L.gatedLines);
				j.Key("masked").Int(L.maskedLines);
				j.EndObj();
				j.EndObj();
				AppendRow(j.s);
			}
		}

		void EvidenceLog::MaybeAggregate(const LogEval& e)
		{
			if (!haveAgg_)
			{
				haveAgg_ = true;
				lastAggUs_ = e.timeUs;
				lastRepeats_ = e.repeatsSkipped;
				lastDropped_ = e.queueDropped;
				lastEvalCount_ = e.evalCount;
				aggEvals_ = aggInformative_ = 0;
				aggCostSum_ = 0.0;
			}
			++aggEvals_;
			if (e.axis[0].informative || e.axis[1].informative)
				++aggInformative_;
			aggCostSum_ += e.costMs;
			if (e.timeUs - lastAggUs_ < s_.aggregateEveryUs)
				return;
			Json j;
			j.Obj();
			j.Key("t").Str("agg");
			j.Key("srcUs").Int(e.timeUs);
			j.Key("windowUs").Int(e.timeUs - lastAggUs_);
			j.Key("era").UInt(e.streamKey);
			j.Key("evals").UInt(aggEvals_);
			j.Key("informative").UInt(aggInformative_);
			j.Key("repeatsSkipped").UInt(e.repeatsSkipped >= lastRepeats_ ? e.repeatsSkipped - lastRepeats_ : 0u);
			j.Key("queueDropped").UInt(e.queueDropped >= lastDropped_ ? e.queueDropped - lastDropped_ : 0u);
			j.Key("meanCostMs").Num(aggEvals_ > 0 ? aggCostSum_ / static_cast<double>(aggEvals_) : 0.0);
			j.Key("intervalSec").Num(e.intervalSec);
			j.Key("state").Arr().Str(Geometry::StateText(e.axis[0].state)).Str(Geometry::StateText(e.axis[1].state)).EndArr();
			j.Key("applied").Arr().Int(e.axis[0].appliedN).Int(e.axis[1].appliedN).EndArr();
			j.EndObj();
			AppendRow(j.s);
			lastAggUs_ = e.timeUs;
			lastRepeats_ = e.repeatsSkipped;
			lastDropped_ = e.queueDropped;
			lastEvalCount_ = e.evalCount;
			aggEvals_ = aggInformative_ = 0;
			aggCostSum_ = 0.0;
		}

		// ---- triggers --------------------------------------------------------------------------------------------------------
		std::string EvidenceLog::CoverageKey(const LogEval& e, int axis) const
		{
			const LogAxis& L = e.axis[axis];
			std::string ratio = "native";
			if (L.appliedN > 0 && L.N > 0 && L.appliedN != L.N)
			{
				static const struct { double v; const char* t; } kClasses[] = {
					{ 1.0 / 2.0, "1/2" }, { 2.0 / 3.0, "2/3" }, { 3.0 / 4.0, "3/4" }, { 1.0 / 3.0, "1/3" },
					{ 3.0 / 8.0, "3/8" }, { 5.0 / 12.0, "5/12" }, { 1.0 / 4.0, "1/4" } };
				const double r = static_cast<double>(L.appliedN) / static_cast<double>(L.N);
				ratio = "other";
				for (const auto& c : kClasses)
					if (std::fabs(r - c.v) < 0.01)
					{
						ratio = c.t;
						break;
					}
			}
			std::string family = "unsure";
			if (L.appliedKernel >= 0)
			{
				static const char* const kFamilies[] = { "bilinear", "bicubic", "lanczos", "spline" };
				const int f = static_cast<int>(Kernel(L.appliedKernel).family);
				family = (f >= 0 && f < 4) ? kFamilies[f] : "unsure";
			}
			// The content class and the sharpening are not known to this stage (C3 / the harness): fixed placeholders.
			return ratio + "|" + family + "|" + (e.stepCodes == 4 ? "8bit" : "10bit") + "|unclassified|na";
		}

		void EvidenceLog::RequestDump(const std::string& why)
		{
			std::lock_guard<std::mutex> g(pub_);
			hotkeyPending_ = true;
			hotkeyWhy_ = why.empty() ? std::string("hotkey") : why;
		}

		void EvidenceLog::Poll()
		{
			if (!active_)
			{
				// A deliberate press is never silently kept for later (it would dump an unrelated moment): it is refused, visibly.
				bool pending = false;
				{
					std::lock_guard<std::mutex> g(pub_);
					pending = hotkeyPending_;
					hotkeyPending_ = false;
				}
				if (pending)
					Notice("Source identity: evidence dump ignored (evidence logging is off or has no folder)");
				return;
			}
			if (!haveLastEval_)
				return;
			bool pending = false;
			std::string why;
			{
				std::lock_guard<std::mutex> g(pub_);
				pending = hotkeyPending_;
				why = hotkeyWhy_;
				hotkeyPending_ = false;
			}
			if (pending)
			{
				Raise(why, lastEval_, true);
				FlushRows();
				PublishStatus();
			}
		}

		void EvidenceLog::CheckTriggers(const LogEval& e, const FrameRef* /*frame*/)
		{
			std::vector<std::string> why;
			bool hotkey = false;
			{
				std::lock_guard<std::mutex> g(pub_);
				if (hotkeyPending_)
				{
					hotkey = true;
					why.push_back(hotkeyWhy_);
					hotkeyPending_ = false;
				}
			}
			bool disagree = false, hold = false, coverage = false;
			for (int a = 0; a < 2; ++a)
			{
				const LogAxis& L = e.axis[a];
				if (L.appliedChanged && L.appliedState == Geometry::LatchState::Latched)
				{
					// A CONFIDENT reading (n, N and the confident ratio come from the geometry): compare with the classical decision.
					if (e.classicalValid && L.appliedN > 0 && L.N > 0)
					{
						const double chain = static_cast<double>(L.appliedN) / static_cast<double>(L.N);
						const double c = e.classicalRatio[a];
						const double m = (std::max)(c, chain);
						if (m > 0.0 && std::fabs(c - chain) > s_.disagreeTolerance * m)
							disagree = true;
					}
					const std::string key = CoverageKey(e, a);
					int& count = coverage_[key];
					if (count < s_.coverageTarget)
						coverage = true;
					++count;
					coverageDirty_ = true;
				}
				if (L.state == Geometry::LatchState::Hold)
				{
					if (holdSinceUs_[a] < 0)
						holdSinceUs_[a] = e.timeUs;
					if (!holdRaised_[a] && e.timeUs - holdSinceUs_[a] > s_.holdTriggerUs)
					{
						hold = true;
						holdRaised_[a] = true;
					}
				}
				else
				{
					holdSinceUs_[a] = -1;
					holdRaised_[a] = false;
				}
				if (L.probeRan)
					probes_.push_back({ e.timeUs, L.probeSeen });
			}
			// The power window: unseen in more than powerUnseenShare of the probes over powerWindowUs.
			bool power = false;
			{
				const int64_t cutoff = e.timeUs - s_.powerWindowUs;
				while (probes_.size() >= 2 && probes_[1].timeUs <= cutoff)
					probes_.erase(probes_.begin());
				if (!probes_.empty() && probes_.front().timeUs <= cutoff)
				{
					size_t unseen = 0;
					for (const ProbePoint& p : probes_)
						if (!p.seen)
							++unseen;
					const double share = static_cast<double>(unseen) / static_cast<double>(probes_.size());
					if (share > s_.powerUnseenShare)
					{
						if (!powerRaised_)
							power = true;
						powerRaised_ = true;
					}
					else
						powerRaised_ = false;
				}
			}
			if (disagree) why.push_back("disagree");
			if (hold) why.push_back("hold");
			if (power) why.push_back("power");
			if (coverage) why.push_back("coverage");
			if (!why.empty())
			{
				std::string joined;
				for (size_t i = 0; i < why.size(); ++i)
					joined += (i == 0 ? "" : "+") + why[i];
				Raise(joined, e, hotkey);
			}
			SaveCoverage();
		}

		void EvidenceLog::Raise(const std::string& why, const LogEval& e, bool hotkey)
		{
			const double now = nowMs_();
			bool allowed = false;
			if (hotkey)
			{
				allowed = !haveHotkey_ || now - lastHotkeyMs_ >= s_.hotkeyDebounceMs;
				if (allowed)
				{
					haveHotkey_ = true;
					lastHotkeyMs_ = now;
				}
			}
			else
			{
				while (!dumpTimesMs_.empty() && now - dumpTimesMs_.front() > s_.dayMs)
					dumpTimesMs_.erase(dumpTimesMs_.begin());
				allowed = (!haveDump_ || now - lastDumpMs_ >= s_.triggerGapMs) && static_cast<int>(dumpTimesMs_.size()) < s_.maxPerDay;
				if (allowed)
				{
					haveDump_ = true;
					lastDumpMs_ = now;
					dumpTimesMs_.push_back(now);
				}
			}
			const bool writes = allowed && WantsLines();
			if (hotkey && !allowed)
				Notice("Source identity: evidence dump ignored (a dump was made a moment ago)");
			else if (hotkey && !writes)
				Notice("Source identity: evidence dump ignored (level log keeps no lines: use log+lines)");
			Json j;
			j.Obj();
			j.Key("t").Str("trigger");
			j.Key("why").Str(why);
			j.Key("srcUs").Int(e.timeUs);
			j.Key("seq").UInt(e.seq);
			j.Key("hotkey").Bool(hotkey);
			j.Key("dump").Str(!allowed ? "suppressed: rate limit" : (writes ? "written" : "skipped: level log"));
			j.EndObj();
			AppendRow(j.s);
			{
				std::lock_guard<std::mutex> g(pub_);
				++status_.triggers;
				if (!allowed)
					++status_.suppressed;
				status_.lastTrigger = why + " @ seq " + std::to_string(e.seq);
			}
			if (writes)
				WriteDump(why, e);
		}

		void EvidenceLog::WriteDump(const std::string& why, const LogEval& e)
		{
			++dumpIndex_;
			char nn[16];
			std::snprintf(nn, sizeof(nn), "%02d", dumpIndex_);
			const std::string base = Name(std::string("dump-") + nn + "-" + Sanitize(why));
			Json h;
			h.Obj();
			h.Key("format").Str("sid-lines-dump");
			h.Key("version").Int(1);
			h.Key("trigger").Str(why);
			h.Key("session").Str(session_);
			h.Key("seq").UInt(e.seq);
			h.Key("srcUs").Int(e.timeUs);
			h.Key("era").UInt(e.streamKey);
			h.Key("evaluations").UInt(ring_.size());
			h.Key("outputStepCodes").Int(e.stepCodes);
			h.Key("mode").Str(mode_);
			h.Key("c1").Num(e.c1);
			h.Key("format_info").Str(format_);
			h.EndObj();
			const std::string bytes = SerializeDump(h.s, ring_);
			if (WriteFile(base + ".sidl", bytes, false))
			{
				std::lock_guard<std::mutex> g(pub_);
				++status_.dumps;
			}
			if (WantsFrames())
				for (size_t k = 0; k < frames_.size(); ++k)
				{
					const std::string data = SerializeFrame(frames_[k]);
					if (!data.empty())
						WriteFile(base + "-frame" + std::to_string(k + 1) + ".u16", data, false);
				}
		}

		// ---- the worker's entry points -------------------------------------------------------------------------------------
		void EvidenceLog::OnEvaluation(const LogEval& e, const FrameRef* frame)
		{
			if (!active_)
				return;
			lastEval_ = e;
			haveLastEval_ = true;
			lastSeq_ = static_cast<int64_t>(e.seq);
			if (!seg_.open)
				OpenSegment(e);
			AccumulateSegment(e);
			if (WantsLines() && e.lines)
			{
				ring_.push_back(e.lines);
				while (ring_.size() > s_.ringEvaluations)
					ring_.erase(ring_.begin());
			}
			if (WantsFrames() && frame != nullptr && frame->Valid())
			{
				frames_.push_back(*frame);
				while (static_cast<int>(frames_.size()) > s_.framesPerDump)
					frames_.erase(frames_.begin());
			}
			WriteStateRows(e);
			CheckTriggers(e, frame);
			bool boundary = false;
			std::string boundaryWhy;
			for (int a = 0; a < 2; ++a)
			{
				// The segment closes when the CONFIDENT size changes (not when the reading drops to UNSURE and returns).
				const LogAxis& L = e.axis[a];
				if (L.appliedState != Geometry::LatchState::Latched || L.appliedN <= 0)
					continue;
				if (haveConfidentN_[a] && lastConfidentN_[a] != L.appliedN)
				{
					boundary = true;
					boundaryWhy = "confident size change";
				}
				haveConfidentN_[a] = true;
				lastConfidentN_[a] = L.appliedN;
			}
			if (boundary)
				CloseSegment(e.timeUs, boundaryWhy);
			MaybeAggregate(e);
			FlushRows();
			PublishStatus();
		}

		void EvidenceLog::OnDiscontinuity(int64_t timeUs, const std::string& cause)
		{
			if (!active_)
				return;
			CloseSegment(timeUs, "discontinuity: " + cause);
			// The evidence of the old source must not trigger against the new one.
			probes_.clear();
			powerRaised_ = false;
			for (int a = 0; a < 2; ++a)
			{
				holdSinceUs_[a] = -1;
				holdRaised_[a] = false;
				haveConfidentN_[a] = false;
			}
			FlushRows();
			PublishStatus();
		}

		void EvidenceLog::Close(int64_t timeUs, const std::string& cause)
		{
			if (active_)
			{
				CloseSegment(timeUs, cause);
				FlushRows();
				SaveCoverage();
			}
			PublishStatus();
		}
	}
}
