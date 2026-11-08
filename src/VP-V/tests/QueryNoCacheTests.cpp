// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// M2 P2 (D-P2c): an interface pointer lives in the renderer DLL for the renderer's lifetime, so host code never
// caches one: Vpv::Query<T> is asked on every use (src/VP-V/abi/VpvRendererAccess.h). This static checker scans the
// host sources for the ways a result could be kept (a member or static assigned from Query, a lambda capture
// initialised from it, a member or static pointer of an IVpv* interface type) and the scanner itself is tested
// on injected text. CPU only: scans source text, no device.

#include "SettingsTestSupport.h"

#include <regex>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace QueryNoCacheTestsNs
{
	// Host-side source folders (the plugin side, src/VP-V/abi/RendererAbiBridge.*, hands out interface objects and is
	// scanned too: its members are objects, never pointers to an interface).
	static const char* const kScanDirs[] = {
		"tools/vp-scale-render",
		"src/VP-V/abi",
		"src/VP-V/core",
		"src/VP-V/gui",
		"src/VP-V/modules",
		"src/VP-V/segments",
		"src/VideoProcessor-GUI",
		"src/VideoProcessor-Lib",
		"src/VideoProcessor-VPRendererFilter",
		"src/VideoProcessor-VPRenderer",
		"src/VideoProcessor-Config",
		"src/VideoProcessor-ConfigCore",
	};

	bool HasSourceExtension(const std::string& name)
	{
		for (const char* ext : { ".cpp", ".h", ".inl" })
		{
			const std::string e(ext);
			if (name.size() > e.size() && name.compare(name.size() - e.size(), e.size(), e) == 0) return true;
		}
		return false;
	}

	std::string Native(const std::string& relative)
	{
		std::string p = relative;
		for (char& c : p) if (c == '/') c = '\\';
		return SettingsTestSupport::RepoRoot() + p;
	}

	std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }
	std::string Narrow(const std::wstring& s) { std::string r; for (wchar_t c : s) r.push_back(static_cast<char>(c)); return r; }

	// Repo-relative forward-slash paths of every source file below relativeDir; "tests" folders are skipped (a test
	// may hold a Query result for the length of one check).
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

	// The line without its // comment tail (string literals are not special-cased: no source line holds a quoted //
	// before one of the patterns below).
	std::string Code(const std::string& line)
	{
		const size_t comment = line.find("//");
		return comment == std::string::npos ? line : line.substr(0, comment);
	}

	// Pure scanner: the lines (1-based numbers, with text) that keep a Query result or an interface pointer.
	std::vector<std::string> FindCachedQueries(const std::string& text)
	{
		static const std::regex memberAssigned(R"re(\b(m_\w+|\w+_)\s*=\s*(Vpv::)?Query\s*<)re");
		static const std::regex staticQuery(R"re(\bstatic\b[^;(]*=\s*(Vpv::)?Query\s*<)re");
		static const std::regex captureQuery(R"re(\[[^\]]*=\s*(Vpv::)?Query\s*<)re");
		static const std::regex memberPointer(R"re(\bIVpv\w+\s*\*\s*(m_\w+|\w+_)\s*(=|;|\{|,|\)))re");
		static const std::regex staticPointer(R"re(\bstatic\b[^;(]*\bIVpv\w+\s*\*)re");
		std::vector<std::string> bad;
		size_t start = 0, number = 0;
		while (start <= text.size())
		{
			size_t end = text.find('\n', start);
			if (end == std::string::npos) end = text.size();
			++number;
			const std::string code = Code(text.substr(start, end - start));
			if (std::regex_search(code, memberAssigned) || std::regex_search(code, staticQuery) ||
				std::regex_search(code, captureQuery) || std::regex_search(code, memberPointer) ||
				std::regex_search(code, staticPointer))
				bad.push_back(std::to_string(number) + ": " + code);
			start = end + 1;
		}
		return bad;
	}

	TEST_CLASS(QueryNoCacheTests)
	{
	public:
		TEST_METHOD(NoHostSourceKeepsAQueryResult)
		{
			std::vector<std::string> files;
			for (const char* dir : kScanDirs) Walk(dir, files);
			Assert::IsTrue(files.size() > 100, L"the scan found too few source files: repository root is wrong");
			bool sawQuery = false;
			for (const std::string& rel : files)
			{
				const std::string text = SettingsTestSupport::ReadFile(Native(rel));
				if (text.find("Query<") != std::string::npos) sawQuery = true;
				const std::vector<std::string> bad = FindCachedQueries(text);
				if (bad.empty()) continue;
				const std::wstring message = Widen(rel) + L" line " + Widen(bad.front()) +
					L": an extension interface pointer must not be cached (D-P2c): call Vpv::Query<T> at each use";
				Assert::Fail(message.c_str());
			}
			Assert::IsTrue(sawQuery, L"no file uses Vpv::Query: the scan covers the wrong folders");
		}

		// F2 (source level; the CPU bridge test covers the behaviour): LibplaceboVideoRenderer::QueryExtension serves the
		// bridge only, with no fallback to the extension registry, whose module iids must not reach the executable.
		TEST_METHOD(RendererQueryExtensionServesOnlyTheBridge)
		{
			const std::string text = SettingsTestSupport::ReadFile(Native("src/VideoProcessor-Lib/vprenderer/LibplaceboVideoRenderer.cpp"));
			const size_t at = text.find("LibplaceboVideoRenderer::QueryExtension(const char* iid)");
			Assert::IsTrue(at != std::string::npos, L"QueryExtension definition not found");
			const size_t end = text.find("InvokeModuleCommand", at);   // the next definition
			Assert::IsTrue(end != std::string::npos && end - at < 600);
			const std::string body = text.substr(at, end - at);
			Assert::IsTrue(body.find("m_abiBridge") != std::string::npos);
			Assert::IsTrue(body.find("RendererExtension::") == std::string::npos,
				L"QueryExtension must not fall back to RendererExtension::Query (module iids would reach the host)");
		}

		// F1 (source level): the factory checks the attach gate before it builds or touches anything.
		TEST_METHOD(FactoryChecksTheAttachGateBeforeBuildingARenderer)
		{
			const std::string text = SettingsTestSupport::ReadFile(Native("src/VideoProcessor-VPRenderer/LibplaceboPluginExports.cpp"));
			const size_t gate = text.find("Vpv::AttachedForCreate()");
			const size_t build = text.find("new LibplaceboVideoRenderer");
			Assert::IsTrue(gate != std::string::npos && build != std::string::npos);
			Assert::IsTrue(gate < build, L"the attach gate must come before the renderer is constructed");
			const size_t callbackCheck = text.find("if (!callback)");
			Assert::IsTrue(callbackCheck != std::string::npos && gate < callbackCheck);
		}

		TEST_METHOD(ScannerFlagsEveryWayToKeepAResult)
		{
			// A member assigned from Query, with and without the namespace.
			Assert::AreEqual(size_t(1), FindCachedQueries("m_led = Vpv::Query<Vpv::IVpvLed>(renderer);").size());
			Assert::AreEqual(size_t(1), FindCachedQueries("\tstatus_ = Query<IVpvStatus>(r);").size());
			// A static.
			Assert::AreEqual(size_t(1), FindCachedQueries("static auto* led = Vpv::Query<Vpv::IVpvLed>(r);").size());
			// A lambda capture initialised from it.
			Assert::AreEqual(size_t(1), FindCachedQueries("auto f = [led = Vpv::Query<Vpv::IVpvLed>(r)]() { return led != nullptr; };").size());
			// A member or static pointer of an interface type.
			Assert::AreEqual(size_t(1), FindCachedQueries("\tVpv::IVpvStatus* m_status = nullptr;").size());
			Assert::AreEqual(size_t(1), FindCachedQueries("\tIVpvLed* led_;").size());
			Assert::AreEqual(size_t(1), FindCachedQueries("static Vpv::IVpvModules* modules;").size());
			// More than one line: each bad line is reported with its number.
			const std::vector<std::string> two = FindCachedQueries("int a;\nm_x = Vpv::Query<Vpv::IVpvLed>(r);\nint b;\nIVpvLed* m_y;\n");
			Assert::AreEqual(size_t(2), two.size());
			Assert::IsTrue(two[0].compare(0, 2, "2:") == 0);
			Assert::IsTrue(two[1].compare(0, 2, "4:") == 0);
		}

		TEST_METHOD(ScannerAcceptsUseAtTheCallSite)
		{
			Assert::AreEqual(size_t(0), FindCachedQueries("Vpv::IVpvLed* i = Vpv::Query<Vpv::IVpvLed>(renderer);").size());
			Assert::AreEqual(size_t(0), FindCachedQueries("if (Vpv::IVpvLed* i = Vpv::Query<Vpv::IVpvLed>(renderer)) i->ResumeAfterTest();").size());
			Assert::AreEqual(size_t(0), FindCachedQueries("const bool ok = Vpv::Query<Vpv::IVpvStatus>(r) != nullptr;").size());
			Assert::AreEqual(size_t(0), FindCachedQueries("return static_cast<IVpvModules*>(&modules_);").size());
			Assert::AreEqual(size_t(0), FindCachedQueries("void* Query(const char* iid) override").size());
			Assert::AreEqual(size_t(0), FindCachedQueries("// m_led = Vpv::Query<Vpv::IVpvLed>(r);  (a comment)").size());
			Assert::AreEqual(size_t(0), FindCachedQueries("explicit ModulesImpl(Host& h) : host(h) {}").size());
			Assert::AreEqual(size_t(0), FindCachedQueries("").size());
		}
	};
}
