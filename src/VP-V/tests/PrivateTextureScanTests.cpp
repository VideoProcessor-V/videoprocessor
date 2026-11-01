// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// Skeleton rule (owner): no private full-resolution/GPU textures in repair / scaling / input-analysis
// modules outside the shared scratch arena (src/VP-V/core/ScratchArena.h) or the product store.
// Modules that still create textures privately are on the allow-list below, each naming the README
// that justifies it. CPU only: scans source text, no device. FindFirstFileW (not std::filesystem)
// so the file builds under any LanguageStandard.

#include "SettingsTestSupport.h"

#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace PrivateTextureScanTestsNs
{
	struct Allowed { const char* file; const char* readme; };

	// Exactly the files that still contain pl_tex_create / pl_tex_recreate (tests/ excluded).
	static const Allowed kAllowList[] = {
		{ "src/VP-V/modules/repair/texture_restore/TextureRestore.cpp", "src/VP-V/modules/repair/texture_restore/README.md" },
		{ "src/VP-V/modules/repair/texture_restore/DetailPulse.cpp", "src/VP-V/modules/repair/texture_restore/README.md" },
		{ "src/VP-V/modules/repair/deband/VpDebander.cpp", "src/VP-V/modules/repair/deband/README.md" },
		{ "src/VP-V/modules/scaling/rebase/SourceRebase.cpp", "src/VP-V/modules/scaling/rebase/README.md" },
		{ "src/VP-V/modules/scaling/output/AdaptiveDownscale.cpp", "src/VP-V/modules/scaling/output/README.md" },
	};

	static const char* const kScanDirs[] = {
		"src/VP-V/modules/repair",
		"src/VP-V/modules/scaling",
		"src/VP-V/modules/input/source_analysis",
	};

	static const char* const kSentence = "migrates in SA6/SA7/SA8/SA12";

	// Pure scanner: does the text create a texture privately?
	bool CreatesTexturePrivately(const std::string& text)
	{
		return text.find("pl_tex_create") != std::string::npos
			|| text.find("pl_tex_recreate") != std::string::npos;
	}

	std::string RepoRoot()
	{
		std::string root = SettingsTestSupport::RepoRoot();   // backslashes, trailing separator
		return root;
	}

	std::string Native(const std::string& relative)
	{
		std::string p = relative;
		for (char& c : p) if (c == '/') c = '\\';
		return RepoRoot() + p;
	}

	bool HasSourceExtension(const std::string& name)
	{
		for (const char* ext : { ".cpp", ".h", ".inl" })
		{
			const std::string e(ext);
			if (name.size() > e.size() && name.compare(name.size() - e.size(), e.size(), e) == 0) return true;
		}
		return false;
	}

	std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }
	std::string Narrow(const std::wstring& s) { std::string r; for (wchar_t c : s) r.push_back(static_cast<char>(c)); return r; }

	// Appends repo-relative forward-slash paths of every source file below relativeDir; directories
	// named "tests" are skipped.
	void Walk(const std::string& relativeDir, std::vector<std::string>& out)
	{
		WIN32_FIND_DATAW data;
		const HANDLE h = FindFirstFileW(Widen(Native(relativeDir) + "\\*").c_str(), &data);
		if (h == INVALID_HANDLE_VALUE) return;
		do
		{
			const std::string name = Narrow(data.cFileName);
			if (name == "." || name == "..") continue;
			const std::string rel = relativeDir + "/" + name;
			if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			{
				if (name != "tests") Walk(rel, out);
			}
			else if (HasSourceExtension(name) && rel.find("/tests/") == std::string::npos)
			{
				out.push_back(rel);
			}
		} while (FindNextFileW(h, &data));
		FindClose(h);
	}

	bool InAllowList(const std::string& rel)
	{
		for (const Allowed& a : kAllowList) if (rel == a.file) return true;
		return false;
	}
}

namespace PrivateTextureScanTestsNs
{
	TEST_CLASS(PrivateTextureScanTests)
	{
	public:
		TEST_METHOD(NoPrivateTexturesOutsideTheAllowList)
		{
			std::vector<std::string> files;
			for (const char* dir : kScanDirs) Walk(dir, files);
			Assert::IsTrue(!files.empty(), L"the scan found no source files: repository root is wrong");
			for (const std::string& rel : files)
			{
				if (!CreatesTexturePrivately(SettingsTestSupport::ReadFile(Native(rel)))) continue;
				if (InAllowList(rel)) continue;
				const std::wstring message = Widen(rel)
					+ L": private texture creation: use the shared scratch arena (src/VP-V/core/ScratchArena.h)"
					  L" or add the file to the allow-list with a README justification";
				Assert::Fail(message.c_str());
			}
		}

		TEST_METHOD(EveryAllowListEntryIsJustified)
		{
			for (const Allowed& a : kAllowList)
			{
				const std::wstring file = Widen(a.file);
				const std::string source = SettingsTestSupport::ReadFile(Native(a.file));
				Assert::IsTrue(!source.empty(), (file + L": allow-listed file is missing").c_str());
				Assert::IsTrue(CreatesTexturePrivately(source),
					(file + L": no longer creates textures privately: remove the stale allow-list entry").c_str());
				const std::wstring readme = Widen(a.readme);
				const std::string text = SettingsTestSupport::ReadFile(Native(a.readme));
				Assert::IsTrue(!text.empty(), (readme + L": justifying README is missing").c_str());
				Assert::IsTrue(text.find(kSentence) != std::string::npos,
					(readme + L": README lacks the sentence \"migrates in SA6/SA7/SA8/SA12\"").c_str());
			}
		}

		TEST_METHOD(ScannerFlagsAnInjectedFile)
		{
			Assert::IsTrue(CreatesTexturePrivately("auto t = pl_tex_create(gpu, &params);"));
			Assert::IsTrue(CreatesTexturePrivately("pl_tex_recreate(gpu, &tex, &params);"));
			Assert::IsFalse(CreatesTexturePrivately("auto t = arena.Acquire(width, height, format);"));
			Assert::IsFalse(CreatesTexturePrivately(""));
		}
	};
}
