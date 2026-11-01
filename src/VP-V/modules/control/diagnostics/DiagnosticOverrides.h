// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Diagnostic overrides (bug hunt 39 #6/#7, 2026-10-31).
//
// VP reads test and debug switches from the process environment in release builds (VP_TEST_* fault
// injectors and folder redirects, VP_TEST_ADAPTER, VP_DEBUG_*, the raw / DV-compare / subtitle
// captures, VP_DISABLE_AVX2, the experimental crop switches). A stray user or system variable
// therefore changes production behaviour unseen. They stay (the harnesses need them); this header
// makes them visible and parses their numbers strictly:
//   - ActiveOverrides(): the names of the switches set in the process environment (the Win32
//     environment, which a test's SetEnvironmentVariable also changes). Logging-only switches
//     (*_TRACE*, VP_RENDERER_FILTER_LOG) are not overrides.
//   - IsTestHarnessProcess(): the process is a test host (vstest / testhost / TE), where the
//     switches are the point; outside it any active override is shown.
//   - StatusLine() / FallbackCatalog::DiagnosticOverrides(): "diagnostic override active: <names>".
//   - StrictNumber(): a whole-string, finite number in a documented range, else the fallback
//     (strtod accepted "nan", "inf", "12abc" and values whose casts were undefined).
// Header-only, C++14. The pure parts take their input as arguments (CPU-tested).
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

namespace VpDiagnostics
{
	// ---- strict numbers -------------------------------------------------------------------------

	// The whole text (surrounding blanks allowed) is one finite number: true and `out` set.
	inline bool ParseStrictNumber(const std::string& text, double& out)
	{
		size_t begin = 0, end = text.size();
		while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
		while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
		if (begin == end)
			return false;
		const std::string body = text.substr(begin, end - begin);
		// No hex floats, "inf", "nan" or "infinity": digits, sign, point and exponent only.
		for (const char c : body)
			if (!(std::isdigit(static_cast<unsigned char>(c)) || c == '+' || c == '-' || c == '.' || c == 'e' || c == 'E'))
				return false;
		char* stop = nullptr;
		errno = 0;
		const double v = std::strtod(body.c_str(), &stop);
		if (stop != body.c_str() + body.size() || errno == ERANGE || !std::isfinite(v))
			return false;
		out = v;
		return true;
	}

	// A strict number clamped to [lo, hi]; the fallback when unset (empty) or not a number.
	inline double StrictNumber(const std::string& text, double fallback, double lo, double hi)
	{
		double v = 0.0;
		if (!ParseStrictNumber(text, v))
			return fallback;
		return (std::min)(hi, (std::max)(lo, v));
	}

	// ---- which switches are overrides -----------------------------------------------------------

	inline std::string UpperAscii(std::string s)
	{
		for (char& c : s)
			c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
		return s;
	}

	// Environment names are case-insensitive on Windows.
	inline bool IsDiagnosticOverrideName(const std::string& nameIn)
	{
		const std::string name = UpperAscii(nameIn);
		if (name.find("_TRACE") != std::string::npos)
			return false;   // logging only
		static const char* const kPrefixes[] = { "VP_TEST_", "VP_DEBUG_", "VP_RAW_CAPTURE_", "VP_DV_COMPARE_",
			"VP_SUBTITLE_CAPTURE_" };
		for (const char* prefix : kPrefixes)
			if (name.compare(0, std::char_traits<char>::length(prefix), prefix) == 0)
				return true;
		static const char* const kNames[] = { "VP_DISABLE_AVX2", "VP_D3D11_DEBUG", "VP_SPARSE_BOUNDARY_CROP",
			"VP_REMEMBERED_EDGE_RETURN" };
		for (const char* exact : kNames)
			if (name == exact)
				return true;
		return false;
	}

	// The override names in a Win32 environment block ("NAME=value\0...\0\0"), sorted, unique.
	// Entries with an empty value are not set; "=C:"-style entries are skipped.
	inline std::vector<std::string> ScanEnvironmentBlock(const wchar_t* block)
	{
		std::vector<std::string> names;
		if (!block)
			return names;
		for (const wchar_t* p = block; *p; )
		{
			const std::wstring entry(p);
			p += entry.size() + 1;
			const size_t eq = entry.find(L'=', 1);
			if (eq == std::wstring::npos || eq == 0 || eq + 1 >= entry.size())
				continue;
			std::string name;
			bool ascii = true;
			for (size_t i = 0; i < eq; ++i)
			{
				if (entry[i] > 0x7f) { ascii = false; break; }
				name += static_cast<char>(entry[i]);
			}
			if (ascii && IsDiagnosticOverrideName(name))
				names.push_back(UpperAscii(name));
		}
		std::sort(names.begin(), names.end());
		names.erase(std::unique(names.begin(), names.end()), names.end());
		return names;
	}

	inline std::vector<std::string> ActiveOverrides()
	{
		wchar_t* block = GetEnvironmentStringsW();
		const std::vector<std::string> names = ScanEnvironmentBlock(block);
		if (block)
			FreeEnvironmentStringsW(block);
		return names;
	}

	// ---- the test harness -----------------------------------------------------------------------

	// The image file name (no folder) of a test host: vstest.console / vstest.executionengine,
	// testhost(.x86), TE.ProcessHost, QTAgent.
	inline bool IsTestHarnessImage(const std::string& fileNameIn)
	{
		const std::string name = UpperAscii(fileNameIn);
		static const char* const kHosts[] = { "VSTEST", "TESTHOST", "TE.PROCESSHOST", "QTAGENT" };
		for (const char* host : kHosts)
			if (name.compare(0, std::char_traits<char>::length(host), host) == 0)
				return true;
		return false;
	}

	inline bool IsTestHarnessProcess()
	{
		char path[MAX_PATH] = {};
		const DWORD length = GetModuleFileNameA(nullptr, path, MAX_PATH);
		if (length == 0 || length >= MAX_PATH)
			return false;
		const std::string full(path, length);
		const size_t slash = full.find_last_of("\\/");
		return IsTestHarnessImage(slash == std::string::npos ? full : full.substr(slash + 1));
	}

	// The overrides to show: none inside a test harness.
	inline std::vector<std::string> OverridesToShow(const std::vector<std::string>& active, bool testHarness)
	{
		return testHarness ? std::vector<std::string>() : active;
	}
	inline std::vector<std::string> OverridesToShow()
	{
		return OverridesToShow(ActiveOverrides(), IsTestHarnessProcess());
	}

	inline std::string JoinNames(const std::vector<std::string>& names)
	{
		std::string text;
		for (size_t i = 0; i < names.size(); ++i)
			text += (i ? ", " : "") + names[i];
		return text;
	}

	// "" when none; else "diagnostic override active: A, B".
	inline std::string StatusLine(const std::vector<std::string>& names)
	{
		return names.empty() ? std::string() : "diagnostic override active: " + JoinNames(names);
	}
}
