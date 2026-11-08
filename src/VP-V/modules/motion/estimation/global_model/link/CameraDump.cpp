// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "CameraDump.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace CameraLink
{
	namespace
	{
		std::string EnvValue(const char* name)
		{
			char* v = nullptr;
			size_t n = 0;
			std::string s;
			if (_dupenv_s(&v, &n, name) == 0 && v)
			{
				s = v;
				free(v);
			}
			return s;
		}

		// A clip / mode name becomes one safe path component (no separators, no "..").
		std::string SafeComponent(const std::string& in, const char* fallback)
		{
			std::string out;
			for (char c : in)
				out += ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.') ? c : '_';
			if (out.empty() || out == "." || out == "..")
				return fallback;
			return out;
		}

		// Creates `path` and its parents (a parent that already exists, or a drive root, may refuse: only the end result counts).
		bool MakeDirectories(const std::string& path)
		{
			if (path.empty())
				return false;
			for (size_t i = 0; i < path.size(); ++i)
				if ((path[i] == '\\' || path[i] == '/') && i > 1)
					CreateDirectoryA(path.substr(0, i).c_str(), nullptr);
			CreateDirectoryA(path.c_str(), nullptr);
			const DWORD attrs = GetFileAttributesA(path.c_str());
			return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
		}

		// Whole file or nothing: a temporary name, then a rename over the final one.
		bool WriteFileAtomic(const std::string& path, const void* data, size_t size)
		{
			const std::string temp = path + ".tmp";
			{
				std::ofstream out(temp.c_str(), std::ios::binary | std::ios::trunc);
				if (!out.is_open())
					return false;
				out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
				out.flush();
				if (!out.good())
					return false;
			}
			if (!MoveFileExA(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
			{
				DeleteFileA(temp.c_str());
				return false;
			}
			return true;
		}

		void AppendF(std::string& s, const char* format, ...)
		{
			char b[512];
			va_list args;
			va_start(args, format);
			std::vsnprintf(b, sizeof(b), format, args);
			va_end(args);
			s += b;
		}
	}

	std::string FlowFileName(uint64_t from, uint64_t to)
	{
		char b[64];
		std::snprintf(b, sizeof(b), "flow_%06llu_%06llu.npy", static_cast<unsigned long long>(from), static_cast<unsigned long long>(to));
		return b;
	}

	std::string CameraFileName(uint64_t to)
	{
		char b[48];
		std::snprintf(b, sizeof(b), "camera_%06llu.json", static_cast<unsigned long long>(to));
		return b;
	}

	std::vector<uint8_t> BuildNpy(const std::vector<float>& x, const std::vector<float>& y, int w, int h)
	{
		std::vector<uint8_t> out;
		if (w <= 0 || h <= 0)
			return out;
		const size_t count = static_cast<size_t>(w) * static_cast<size_t>(h);
		if (x.size() != count || y.size() != count)
			return out;
		char dict[96];
		std::snprintf(dict, sizeof(dict), "{'descr': '<f4', 'fortran_order': False, 'shape': (%d, %d, 2), }", h, w);
		std::string header = dict;
		// 10 = magic (6) + version (2) + header length (2); the whole prefix is a multiple of 64 and the header ends with '\n'.
		const size_t unpadded = 10 + header.size() + 1;
		header.append((64 - unpadded % 64) % 64, ' ');
		header += '\n';
		const uint16_t headerLength = static_cast<uint16_t>(header.size());
		static const uint8_t magic[8] = { 0x93, 'N', 'U', 'M', 'P', 'Y', 1, 0 };
		out.assign(magic, magic + 8);
		out.push_back(static_cast<uint8_t>(headerLength & 0xFF));
		out.push_back(static_cast<uint8_t>(headerLength >> 8));
		out.insert(out.end(), header.begin(), header.end());
		const size_t base = out.size();
		out.resize(base + count * 2 * sizeof(float));
		for (size_t i = 0; i < count; ++i)
		{
			std::memcpy(&out[base + i * 8], &x[i], sizeof(float));
			std::memcpy(&out[base + i * 8 + 4], &y[i], sizeof(float));
		}
		return out;
	}

	std::string CameraJson(const CameraModel::CameraMotion* rec, const FieldCopy& field, const std::string& mode)
	{
		std::string s = "{\n";
		AppendF(s, "  \"mode\": \"%s\",\n", SafeComponent(mode, "engine").c_str());
		AppendF(s, "  \"seq\": %llu,\n  \"pairFrom\": %llu,\n  \"pairTo\": %llu,\n", static_cast<unsigned long long>(field.seq),
			static_cast<unsigned long long>(field.pairFrom), static_cast<unsigned long long>(field.pairTo));
		AppendF(s, "  \"grid\": { \"w\": %d, \"h\": %d, \"blockSize\": %.9g, \"backward\": %s },\n", field.gridW, field.gridH,
			field.blockSize, field.HaveBackward() ? "true" : "false");
		if (!rec)
		{
			s += "  \"record\": false\n}\n";
			return s;
		}
		s += "  \"record\": true,\n";
		AppendF(s, "  \"valid\": %s,\n  \"reason\": \"%s\",\n  \"confidence\": %.9g,\n", rec->valid ? "true" : "false",
			CameraModel::ReasonTextOf(rec->reason, rec->verdictFlags), rec->confidence);
		AppendF(s, "  \"modelType\": %d,\n  \"pairSeconds\": %.9g,\n", static_cast<int>(rec->params.type), rec->pairSeconds);
		AppendF(s, "  \"params\": [%.9g, %.9g, %.9g, %.9g, %.9g, %.9g, %.9g, %.9g, %.9g],\n", rec->params.h[0], rec->params.h[1],
			rec->params.h[2], rec->params.h[3], rec->params.h[4], rec->params.h[5], rec->params.h[6], rec->params.h[7], rec->params.h[8]);
		AppendF(s, "  \"rates\": { \"logScale\": %.9g, \"angle\": %.9g, \"tx\": %.9g, \"ty\": %.9g },\n", rec->rates.logScale,
			rec->rates.angle, rec->rates.tx, rec->rates.ty);
		AppendF(s, "  \"samples\": %u,\n  \"hypotheses\": %u,\n  \"costMicros\": %lld,\n", rec->samples, rec->hypotheses,
			static_cast<long long>(rec->costMicros));
		AppendF(s, "  \"picture\": { \"left\": %d, \"top\": %d, \"right\": %d, \"bottom\": %d }\n}\n", rec->picture.left, rec->picture.top,
			rec->picture.right, rec->picture.bottom);
		return s;
	}

	CameraDump::CameraDump(const DumpConfig& config, bool startThread) : m_config(config)
	{
		m_config.clip = SafeComponent(m_config.clip, "clip");
		m_config.mode = SafeComponent(m_config.mode, "engine");
		if (m_config.queueCapacity == 0)
			m_config.queueCapacity = 1;
		if (startThread)
			m_thread = std::thread([this]() { Run(); });
	}

	CameraDump::~CameraDump()
	{
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_stop = true;
		}
		m_cv.notify_all();
		if (m_thread.joinable())
			m_thread.join();   // the thread writes what is queued before it returns
	}

	std::string CameraDump::DirectoryFromEnvironment()
	{
		const std::string v = EnvValue("VP_CAMERA_DUMP");
		return v == "1" ? std::string() : v;
	}

	std::string CameraDump::ClipFromEnvironment()
	{
		return SafeComponent(EnvValue("VP_CAMERA_DUMP_CLIP"), "clip");
	}

	std::string CameraDump::ClipDirectory() const
	{
		std::string d = m_config.directory;
		if (!d.empty() && d.back() != '\\' && d.back() != '/')
			d += '\\';
		return d + m_config.clip;
	}

	bool CameraDump::Offer(std::shared_ptr<const FieldCopy> field, std::shared_ptr<const CameraModel::CameraMotion> record)
	{
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			++m_stats.offered;
			if (!field || m_stop || m_queue.size() >= m_config.queueCapacity)
			{
				++m_stats.dropped;
				return false;
			}
			Item item;
			item.field = std::move(field);
			item.record = std::move(record);
			m_queue.push_back(std::move(item));
		}
		m_cv.notify_one();
		return true;
	}

	DumpStats CameraDump::Stats() const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		DumpStats s = m_stats;
		s.queued = m_queue.size();
		return s;
	}

	size_t CameraDump::DrainOnce()
	{
		size_t n = 0;
		for (;;)
		{
			Item item;
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				if (m_queue.empty())
					return n;
				item = std::move(m_queue.front());
				m_queue.pop_front();
			}
			const bool ok = Write(item);
			std::lock_guard<std::mutex> lock(m_mutex);
			++(ok ? m_stats.written : m_stats.failed);
			++n;
		}
	}

	void CameraDump::Run()
	{
		for (;;)
		{
			Item item;
			{
				std::unique_lock<std::mutex> lock(m_mutex);
				m_cv.wait(lock, [this]() { return m_stop || !m_queue.empty(); });
				if (m_queue.empty())
					return;   // stop requested and nothing left
				item = std::move(m_queue.front());
				m_queue.pop_front();
			}
			const bool ok = Write(item);
			std::lock_guard<std::mutex> lock(m_mutex);
			++(ok ? m_stats.written : m_stats.failed);
		}
	}

	bool CameraDump::Write(const Item& item)
	{
		const FieldCopy& f = *item.field;
		if (!f.Valid() || m_config.directory.empty())
			return false;
		const std::string dir = ClipDirectory();
		if (!m_dirReady)
		{
			if (!MakeDirectories(dir))
				return false;
			m_dirReady = true;   // only the writing thread (or DrainOnce, never both) touches this
		}
		const std::string base = dir + "\\";
		bool ok = true;
		const std::vector<uint8_t> fwd = BuildNpy(f.fwdX, f.fwdY, f.gridW, f.gridH);
		ok = ok && !fwd.empty() && WriteFileAtomic(base + FlowFileName(f.pairFrom, f.pairTo), fwd.data(), fwd.size());
		if (f.HaveBackward())
		{
			const std::vector<uint8_t> bwd = BuildNpy(f.bwdX, f.bwdY, f.gridW, f.gridH);
			ok = ok && !bwd.empty() && WriteFileAtomic(base + FlowFileName(f.pairTo, f.pairFrom), bwd.data(), bwd.size());
		}
		const std::string json = CameraJson(item.record.get(), f, m_config.mode);
		ok = ok && WriteFileAtomic(base + CameraFileName(f.pairTo), json.data(), json.size());
		return ok;
	}
}
