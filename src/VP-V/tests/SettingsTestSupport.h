// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Shared helpers of the CPU settings tests (SettingsFingerprintStructureTests, SettingsDefaultsGoldenTests):
// the repository root from __FILE__, whole-file reads, and the renderer DLL's settings exports
// (Describe: key=value lines; Fingerprint: '|'-separated EffectiveSettingsFingerprint) loaded WITHOUT
// creating a device, as IntentInterpreterTests / HdrInferenceChoiceWiringTests do.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <regex>
#include <string>
#include <vector>

namespace SettingsTestSupport
{
	// The repository root with a trailing backslash (the part of this file's path before "\src\").
	inline std::string RepoRoot()
	{
		std::string path = __FILE__;
		std::replace(path.begin(), path.end(), '/', '\\');
		path.resize(path.rfind("\\src\\") + 1);
		return path;
	}

	inline std::string ReadFile(const std::string& path)
	{
		std::ifstream file(path, std::ios::binary);
		return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	}

	inline std::string Here()
	{
		char here[MAX_PATH] = {};
		HMODULE self = nullptr;
		GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCSTR>(&Here), &self);
		GetModuleFileNameA(self, here, MAX_PATH);
		const std::string binary(here);
		return binary.substr(0, binary.find_last_of("\\/") + 1);
	}

	// Splits on a single character, keeping empty fields.
	inline std::vector<std::string> Split(const std::string& text, char sep)
	{
		std::vector<std::string> parts;
		size_t start = 0;
		for (;;)
		{
			const size_t at = text.find(sep, start);
			if (at == std::string::npos) { parts.push_back(text.substr(start)); break; }
			parts.push_back(text.substr(start, at - start));
			start = at + 1;
		}
		return parts;
	}

	// One evaluation of a configuration text by the renderer's own loader.
	struct Evaluation
	{
		bool ok = false;
		std::string describe;                    // key=value lines, '\n' only
		std::vector<std::string> fingerprint;    // fields without configurationIdentity / configurationPath
	};

	class Renderer
	{
	public:
		Renderer()
		{
			const std::string dll = Here() + "vprenderer\\VideoProcessorVPRenderer.dll";
			module_ = LoadLibraryExA(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
			if (!module_) return;
			describe_ = reinterpret_cast<Export>(GetProcAddress(module_, "VideoProcessorLibplaceboDescribeSettingsForTest"));
			fingerprint_ = reinterpret_cast<Export>(GetProcAddress(module_, "VideoProcessorLibplaceboSettingsFingerprintForTest"));
			char temp[MAX_PATH] = {};
			GetTempPathA(MAX_PATH, temp);
			folder_ = std::string(temp) + "vp-settings-tests-" + std::to_string(GetCurrentProcessId()) + "\\";
			CreateDirectoryA(folder_.c_str(), nullptr);
		}
		~Renderer()
		{
			for (const std::string& path : written_) DeleteFileA(path.c_str());
			if (!folder_.empty()) RemoveDirectoryA(folder_.c_str());
		}
		Renderer(const Renderer&) = delete;
		Renderer& operator=(const Renderer&) = delete;

		bool Ready() const { return module_ && describe_ && fingerprint_; }

		// Writes `text` to a NEW file (the loader's cache keys on path, size and mtime, so a rewritten
		// file could read a cached copy) and asks the renderer for its settings.
		Evaluation Evaluate(const std::string& text)
		{
			Evaluation e;
			const std::string path = folder_ + "c" + std::to_string(counter_++) + ".cfg";
			{
				std::ofstream out(path, std::ios::trunc | std::ios::binary);
				out << text;
			}
			written_.push_back(path);
			std::vector<char> buffer(1 << 20);
			const int described = describe_(path.c_str(), buffer.data(), static_cast<int>(buffer.size()));
			if (described <= 0) return e;
			e.describe = buffer.data();
			e.describe.erase(std::remove(e.describe.begin(), e.describe.end(), '\r'), e.describe.end());
			std::fill(buffer.begin(), buffer.end(), '\0');
			const int fingerprinted = fingerprint_(path.c_str(), buffer.data(), static_cast<int>(buffer.size()));
			if (fingerprinted <= 0) return e;
			std::vector<std::string> fields = Split(buffer.data(), '|');
			// The first two fields are configurationIdentity and configurationPath (machine / path dependent).
			if (fields.size() > 2) fields.erase(fields.begin(), fields.begin() + 2);
			e.fingerprint = fields;
			e.ok = true;
			return e;
		}

	private:
		using Export = int(__cdecl*)(const char*, char*, int);
		HMODULE module_ = nullptr;
		Export describe_ = nullptr;
		Export fingerprint_ = nullptr;
		std::string folder_;
		std::vector<std::string> written_;
		int counter_ = 0;
	};

	// The value of `key=` in a Describe dump, or "(missing)".
	inline std::string DescribeValue(const std::string& dump, const std::string& key, bool* found = nullptr)
	{
		const std::string needle = "\n" + key + "=";
		const size_t at = ("\n" + dump).find(needle);
		if (found) *found = at != std::string::npos;
		if (at == std::string::npos) return "(missing)";
		const std::string padded = "\n" + dump;
		const size_t start = at + needle.size();
		return padded.substr(start, padded.find('\n', start) - start);
	}

	// The [vpv] keys: the validator list of the configuration loader (static const char* const vpvKeys[]).
	inline std::vector<std::string> VpvKeys(const std::string& text)
	{
		std::vector<std::string> keys;
		const size_t at = text.find("vpvKeys[] = {");
		if (at == std::string::npos) return keys;
		const size_t end = text.find("};", at);
		const std::string body = text.substr(at, end - at);
		static const std::regex quoted(R"re("([^"]+)")re");
		for (std::sregex_iterator it(body.begin(), body.end(), quoted), last; it != last; ++it)
			keys.push_back((*it)[1].str());
		return keys;
	}

	inline std::string Base()
	{
		return "[general]\npersist_profile_selection: false\n[vprenderer.Default]\nquality: high\n";
	}
}
