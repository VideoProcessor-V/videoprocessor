#pragma once

// Real-footage clip library (test support, 2026-10-18): the clip manifest and an ffmpeg
// decoder, so tests feed VP's CPU-side components real decoded pictures at any resolution
// and rate, with the clips' own timestamps. Nothing about a clip is fixed here: the
// manifest (tools/testclips/make_clip_manifest.py, generated from the library's README)
// gives each clip's path, rate, cadence and expected events, and the decoder reports the
// size it decoded.
//
// Environment:
//   VP_TEST_CLIP_MANIFEST   the manifest (default D:\testclips\manifest.json; missing = skip)
//   VP_TEST_FFMPEG          ffmpeg.exe (default: the manifest's "ffmpeg", then D:\tools\ffmpeg\bin)
//   VP_TEST_FFMPEG_THREADS  decoder threads (default 3)
//   VP_TEST_REQUIRE_FOOTAGE 1 = a missing library fails instead of skipping
//
// C++14 (this project), Win32 only (CreateProcess pipes).

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace RealClip
{
	inline std::string Env(const char* name)
	{
		char* v = nullptr;
		size_t n = 0;
		std::string value;
		if (_dupenv_s(&v, &n, name) == 0 && v)
			value = v;
		std::free(v);
		return value;
	}

	inline bool FileExists(const std::string& path)
	{
		const DWORD a = GetFileAttributesA(path.c_str());
		return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
	}

	// ---- a minimal JSON reader (objects, arrays, strings, numbers, true/false/null) ----
	struct Json
	{
		enum Type { Null, Bool, Number, String, Array, Object } type = Null;
		bool b = false;
		double n = 0.0;
		std::string s;
		std::vector<Json> a;
		std::vector<std::pair<std::string, Json>> o;

		const Json* Find(const std::string& key) const
		{
			for (const auto& kv : o)
				if (kv.first == key)
					return &kv.second;
			return nullptr;
		}
		double Num(const std::string& key, double fallback = 0.0) const
		{
			const Json* j = Find(key);
			return j && j->type == Number ? j->n : fallback;
		}
		int Int(const std::string& key, int fallback = 0) const
		{
			return static_cast<int>(std::lround(Num(key, fallback)));
		}
		std::string Str(const std::string& key, const std::string& fallback = std::string()) const
		{
			const Json* j = Find(key);
			return j && j->type == String ? j->s : fallback;
		}
		bool Flag(const std::string& key, bool fallback = false) const
		{
			const Json* j = Find(key);
			return j && j->type == Bool ? j->b : fallback;
		}
		static Json Parse(const std::string& text)
		{
			size_t i = 0;
			Json j = ParseValue(text, i);
			return j;
		}

	private:
		static void Ws(const std::string& t, size_t& i)
		{
			while (i < t.size() && (t[i] == ' ' || t[i] == '\n' || t[i] == '\r' || t[i] == '\t'))
				++i;
		}
		static std::string ParseString(const std::string& t, size_t& i)
		{
			std::string out;
			++i;   // opening quote
			while (i < t.size() && t[i] != '"')
			{
				char c = t[i++];
				if (c == '\\' && i < t.size())
				{
					const char e = t[i++];
					switch (e)
					{
					case 'n': out += '\n'; break;
					case 't': out += '\t'; break;
					case 'r': out += '\r'; break;
					case 'b': out += '\b'; break;
					case 'f': out += '\f'; break;
					case 'u':
					{
						const unsigned code = static_cast<unsigned>(std::strtoul(t.substr(i, 4).c_str(), nullptr, 16));
						i += 4;
						if (code < 0x80)
							out += static_cast<char>(code);
						else if (code < 0x800)
						{
							out += static_cast<char>(0xC0 | (code >> 6));
							out += static_cast<char>(0x80 | (code & 0x3F));
						}
						else
						{
							out += static_cast<char>(0xE0 | (code >> 12));
							out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
							out += static_cast<char>(0x80 | (code & 0x3F));
						}
						break;
					}
					default: out += e; break;
					}
				}
				else
					out += c;
			}
			++i;   // closing quote
			return out;
		}
		static Json ParseValue(const std::string& t, size_t& i)
		{
			Ws(t, i);
			Json j;
			if (i >= t.size())
				return j;
			const char c = t[i];
			if (c == '{')
			{
				j.type = Object;
				++i;
				Ws(t, i);
				if (i < t.size() && t[i] == '}') { ++i; return j; }
				while (i < t.size())
				{
					Ws(t, i);
					std::string key = ParseString(t, i);
					Ws(t, i);
					++i;   // ':'
					Json v = ParseValue(t, i);
					j.o.emplace_back(std::move(key), std::move(v));
					Ws(t, i);
					if (i < t.size() && t[i] == ',') { ++i; continue; }
					if (i < t.size() && t[i] == '}') { ++i; break; }
					throw std::runtime_error("manifest: bad object");
				}
				return j;
			}
			if (c == '[')
			{
				j.type = Array;
				++i;
				Ws(t, i);
				if (i < t.size() && t[i] == ']') { ++i; return j; }
				while (i < t.size())
				{
					j.a.push_back(ParseValue(t, i));
					Ws(t, i);
					if (i < t.size() && t[i] == ',') { ++i; continue; }
					if (i < t.size() && t[i] == ']') { ++i; break; }
					throw std::runtime_error("manifest: bad array");
				}
				return j;
			}
			if (c == '"')
			{
				j.type = String;
				j.s = ParseString(t, i);
				return j;
			}
			if (t.compare(i, 4, "true") == 0) { j.type = Bool; j.b = true; i += 4; return j; }
			if (t.compare(i, 5, "false") == 0) { j.type = Bool; j.b = false; i += 5; return j; }
			if (t.compare(i, 4, "null") == 0) { i += 4; return j; }
			char* end = nullptr;
			j.type = Number;
			j.n = std::strtod(t.c_str() + i, &end);
			i = static_cast<size_t>(end - t.c_str());
			return j;
		}
	};

	// ---- the manifest ----
	struct Event
	{
		std::string type;
		const Json* json = nullptr;   // the full record (owned by Manifest::doc)
		int Frame() const { return json ? json->Int("frame", -1) : -1; }
		int First() const { return json ? json->Int("first", -1) : -1; }
		int Last() const { return json ? json->Int("last", -1) : -1; }
	};

	struct Clip
	{
		std::string id, path, group, cadenceKind, content, use;
		int rateNum = 0, rateDen = 1, frames = 0, width = 0, height = 0;
		double seconds = 0.0;
		bool interlaced = false, vfr = false, cutFree = false, longTake = false, dark = false;
		std::vector<int> holds;            // pulldown holds per film frame (e.g. 2,3), empty = none
		int pictureNum = 0, pictureDen = 1;   // the content's own picture rate
		std::vector<Event> events;
		std::vector<double> cuts;          // scene-cut ground truth (clip seconds)
		bool hasCuts = false;
		const Json* json = nullptr;

		double RateHz() const { return rateDen > 0 ? static_cast<double>(rateNum) / rateDen : 0.0; }
		double PictureHz() const { return pictureDen > 0 && pictureNum > 0 ? static_cast<double>(pictureNum) / pictureDen : RateHz(); }
		std::vector<Event> EventsOf(const std::string& type) const
		{
			std::vector<Event> out;
			for (const Event& e : events)
				if (e.type == type)
					out.push_back(e);
			return out;
		}
	};

	struct Manifest
	{
		std::string path, root, ffmpeg, error;
		std::vector<Clip> clips;
		std::shared_ptr<Json> doc;

		bool Loaded() const { return !clips.empty(); }
		const Clip* Find(const std::string& id) const
		{
			for (const Clip& c : clips)
				if (c.id == id)
					return &c;
			return nullptr;
		}
		std::string FullPath(const Clip& c) const { return root + "\\" + c.path; }
		std::string FfprobePath() const
		{
			std::string dir = ffmpeg.substr(0, ffmpeg.find_last_of("\\/"));
			return dir + "\\ffprobe.exe";
		}

		static std::string DefaultPath()
		{
			const std::string env = Env("VP_TEST_CLIP_MANIFEST");
			return env.empty() ? std::string("D:\\testclips\\manifest.json") : env;
		}

		static Manifest Load(const std::string& path = DefaultPath())
		{
			Manifest m;
			m.path = path;
			FILE* f = nullptr;
			if (fopen_s(&f, path.c_str(), "rb") != 0 || !f)
			{
				m.error = "no manifest at " + path;
				return m;
			}
			std::string text;
			char buffer[65536];
			size_t got = 0;
			while ((got = fread(buffer, 1, sizeof(buffer), f)) > 0)
				text.append(buffer, got);
			fclose(f);
			m.doc = std::make_shared<Json>(Json::Parse(text));
			const Json& d = *m.doc;
			m.root = d.Str("root");
			if (m.root.empty())
				m.root = path.substr(0, path.find_last_of("\\/"));
			m.ffmpeg = Env("VP_TEST_FFMPEG");
			if (m.ffmpeg.empty())
				m.ffmpeg = d.Str("ffmpeg", "D:\\tools\\ffmpeg\\bin\\ffmpeg.exe");
			const Json* clips = d.Find("clips");
			if (!clips || clips->type != Json::Array)
			{
				m.error = "manifest has no clips";
				return m;
			}
			for (const Json& j : clips->a)
			{
				Clip c;
				c.json = &j;
				c.id = j.Str("id");
				c.path = j.Str("path");
				c.group = j.Str("group");
				c.content = j.Str("content");
				c.use = j.Str("use");
				c.rateNum = j.Int("rate_num");
				c.rateDen = (std::max)(1, j.Int("rate_den", 1));
				c.frames = j.Int("frames");
				c.width = j.Int("width");
				c.height = j.Int("height");
				c.seconds = j.Num("seconds");
				c.interlaced = j.Flag("interlaced");
				c.vfr = j.Flag("vfr");
				c.cutFree = j.Flag("cut_free");
				c.longTake = j.Flag("long_take");
				c.dark = j.Flag("dark");
				if (const Json* cad = j.Find("cadence"))
				{
					c.cadenceKind = cad->Str("kind");
					if (const Json* h = cad->Find("holds"))
						for (const Json& x : h->a)
							c.holds.push_back(static_cast<int>(x.n));
				}
				if (const Json* p = j.Find("picture_rate"))
					if (p->type == Json::Array && p->a.size() == 2)
					{
						c.pictureNum = static_cast<int>(p->a[0].n);
						c.pictureDen = static_cast<int>(p->a[1].n);
					}
				if (const Json* ev = j.Find("events"))
					for (const Json& e : ev->a)
					{
						Event x;
						x.type = e.Str("type");
						x.json = &e;
						c.events.push_back(x);
					}
				if (const Json* cuts = j.Find("cuts_s"))
				{
					c.hasCuts = true;
					for (const Json& x : cuts->a)
						c.cuts.push_back(x.n);
				}
				m.clips.push_back(std::move(c));
			}
			return m;
		}
	};

	// ---- a child process with its stdout on a pipe ----
	class Pipe
	{
	public:
		Pipe() = default;
		Pipe(const Pipe&) = delete;
		Pipe& operator=(const Pipe&) = delete;
		~Pipe() { Close(); }

		bool Start(const std::string& commandLine, std::string& error)
		{
			SECURITY_ATTRIBUTES sa{};
			sa.nLength = sizeof(sa);
			sa.bInheritHandle = TRUE;
			HANDLE writeEnd = nullptr;
			if (!CreatePipe(&m_read, &writeEnd, &sa, 1 << 22))
			{
				error = "CreatePipe failed";
				return false;
			}
			SetHandleInformation(m_read, HANDLE_FLAG_INHERIT, 0);
			HANDLE nul = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
			STARTUPINFOA si{};
			si.cb = sizeof(si);
			si.dwFlags = STARTF_USESTDHANDLES;
			si.hStdOutput = writeEnd;
			si.hStdError = nul;
			si.hStdInput = nullptr;
			std::vector<char> cmd(commandLine.begin(), commandLine.end());
			cmd.push_back('\0');
			const BOOL ok = CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE,
				CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr, nullptr, &si, &m_process);
			CloseHandle(writeEnd);
			if (nul != INVALID_HANDLE_VALUE)
				CloseHandle(nul);
			if (!ok)
			{
				error = "CreateProcess failed: " + commandLine;
				CloseHandle(m_read);
				m_read = nullptr;
				return false;
			}
			return true;
		}

		// Reads exactly `bytes` (false at the end of the stream).
		bool ReadExact(uint8_t* dst, size_t bytes)
		{
			size_t done = 0;
			while (done < bytes)
			{
				DWORD got = 0;
				const DWORD want = static_cast<DWORD>((std::min)(bytes - done, size_t{ 1 } << 24));
				if (!ReadFile(m_read, dst + done, want, &got, nullptr) || got == 0)
					return false;
				done += got;
			}
			return true;
		}

		std::string ReadAll()
		{
			std::string out;
			char buffer[65536];
			DWORD got = 0;
			while (ReadFile(m_read, buffer, sizeof(buffer), &got, nullptr) && got > 0)
				out.append(buffer, got);
			return out;
		}

		void Close()
		{
			if (m_read)
			{
				CloseHandle(m_read);
				m_read = nullptr;
			}
			if (m_process.hProcess)
			{
				// Only the child this object started: waited for, terminated if it outlives its pipe.
				if (WaitForSingleObject(m_process.hProcess, 5000) == WAIT_TIMEOUT)
					TerminateProcess(m_process.hProcess, 1);
				CloseHandle(m_process.hProcess);
				CloseHandle(m_process.hThread);
				m_process = PROCESS_INFORMATION{};
			}
		}

	private:
		HANDLE m_read = nullptr;
		PROCESS_INFORMATION m_process{};
	};

	inline std::string Quote(const std::string& s) { return "\"" + s + "\""; }

	// Presentation timestamps (seconds) of every video frame, in display order: the packets'
	// pts sorted (no decode). Empty on failure.
	inline std::vector<double> ProbeTimestamps(const Manifest& m, const Clip& c, std::string& error)
	{
		std::vector<double> pts;
		Pipe p;
		const std::string cmd = Quote(m.FfprobePath()) + " -v error -select_streams v:0 -show_entries packet=pts_time -of csv=p=0 " +
			Quote(m.FullPath(c));
		if (!p.Start(cmd, error))
			return pts;
		std::istringstream in(p.ReadAll());
		std::string line;
		while (std::getline(in, line))
		{
			if (line.empty() || line[0] == 'N')
				continue;
			pts.push_back(std::strtod(line.c_str(), nullptr));
		}
		std::sort(pts.begin(), pts.end());
		return pts;
	}

	// Decoded 8-bit luma frames, one at a time, at the clip's size (or a filter's output
	// size). `filter` is an optional ffmpeg -vf chain that keeps the frame size (a synthetic
	// upscale, a flash); the frames come out 1:1 with the clip's frames (passthrough timing).
	class Decoder
	{
	public:
		bool Open(const Manifest& m, const Clip& c, const std::string& filter, std::string& error)
		{
			width = c.width;
			height = c.height;
			if (width <= 0 || height <= 0)
			{
				error = "clip size unknown (run the manifest generator with --probe): " + c.id;
				return false;
			}
			std::string threads = Env("VP_TEST_FFMPEG_THREADS");
			if (threads.empty())
				threads = "3";
			std::string cmd = Quote(m.ffmpeg) + " -v error -nostdin -threads " + threads + " -i " + Quote(m.FullPath(c));
			if (!filter.empty())
				cmd += " -vf " + Quote(filter);
			cmd += " -fps_mode passthrough -an -f rawvideo -pix_fmt gray -";
			return m_pipe.Start(cmd, error);
		}
		bool Next(std::vector<uint8_t>& luma)
		{
			luma.resize(static_cast<size_t>(width) * static_cast<size_t>(height));
			return m_pipe.ReadExact(luma.data(), luma.size());
		}
		void Close() { m_pipe.Close(); }
		int width = 0, height = 0;

		// B3 acceptance (2026-10-24): whole frames in `pixFmt` (e.g. yuv422p10le) at the filter's
		// output size outW x outH (the caller's filter must produce it); NextRaw reads frameBytes.
		bool OpenRaw(const Manifest& m, const Clip& c, const std::string& filter, const std::string& pixFmt, std::string& error)
		{
			std::string threads = Env("VP_TEST_FFMPEG_THREADS");
			if (threads.empty())
				threads = "3";
			std::string cmd = Quote(m.ffmpeg) + " -v error -nostdin -threads " + threads + " -i " + Quote(m.FullPath(c));
			if (!filter.empty())
				cmd += " -vf " + Quote(filter);
			cmd += " -fps_mode passthrough -an -f rawvideo -pix_fmt " + pixFmt + " -";
			return m_pipe.Start(cmd, error);
		}
		bool NextRaw(std::vector<uint8_t>& frame, size_t frameBytes)
		{
			frame.resize(frameBytes);
			return m_pipe.ReadExact(frame.data(), frame.size());
		}

	private:
		Pipe m_pipe;
	};
}
