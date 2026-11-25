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

#include <algorithm>
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
		// Wrapper definition and wrapper callers (VpvGpu::Ensure / Ensure / EnsureFloat around pl_tex_recreate):
		{ "src/VP-V/modules/repair/grain_split/GpuPassUtil.h", "src/VP-V/modules/repair/grain_split/README.md" },
		{ "src/VP-V/modules/repair/grain_split/GrainSplitGpu.cpp", "src/VP-V/modules/repair/grain_split/README.md" },
		{ "src/VP-V/modules/repair/temporal_detail/TemporalDetailGpu.cpp", "src/VP-V/modules/repair/temporal_detail/README.md" },
		{ "src/VP-V/modules/scaling/chroma_upscale/ChromaCflGpu.cpp", "src/VP-V/modules/scaling/chroma_upscale/README.md" },
		// BasicChromaGpu::Module::m_out: the stage's OUTPUT texture, handed to the caller and kept across frames (not a per-call temporary).
		{ "src/VP-V/modules/scaling/chroma_upscale/basic/BasicChromaGpu.cpp", "src/VP-V/modules/scaling/chroma_upscale/README.md" },
		// ScalerPass::Module: the per-slot OUTPUT textures (handed to the caller, kept across frames) and the per-slot two-pass temporary.
		// EXPIRES when the first B1 caller of ScalerPass lands (102b row SCALERPASS-ARENA: caller-supplied out/temp textures from the arena or product store).
		{ "src/VP-V/modules/scaling/shared/ScalerPass.cpp", "src/VP-V/modules/scaling/shared/README.md" },
		// Route4k::Module: per-slot OUTPUT blendTex[slot] handed to the caller across frames (migrates alongside ScalerPass).
		{ "src/VP-V/modules/scaling/route_4k/Route4k.cpp", "src/VP-V/modules/scaling/route_4k/README.md" },
	};

	static const char* const kScanDirs[] = {
		"src/VP-V/modules/repair",
		"src/VP-V/modules/scaling",
		"src/VP-V/modules/input/source_analysis",
	};

	static const char* const kSentence = "migrates in SA6/SA7/SA8/SA12";

	bool IsIdentChar(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }

	// A call of the bare wrapper name `name(` (Ensure / EnsureFloat), alone or qualified (VpvGpu::Ensure), but not
	// a longer identifier (EnsureSize, MyEnsure).
	bool CallsWrapper(const std::string& text, const char* name)
	{
		const std::string call = std::string(name) + "(";
		for (size_t at = text.find(call); at != std::string::npos; at = text.find(call, at + 1))
			if (at == 0 || !IsIdentChar(text[at - 1])) return true;
		return false;
	}

	// Pure scanner: does the text create a texture privately (directly or through the Ensure / EnsureFloat wrappers
	// around pl_tex_recreate that the repair and scaling modules define)?
	bool CreatesTexturePrivately(const std::string& text)
	{
		return text.find("pl_tex_create") != std::string::npos
			|| text.find("pl_tex_recreate") != std::string::npos
			|| CallsWrapper(text, "Ensure") || CallsWrapper(text, "EnsureFloat");
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

	// C-T4 (lead 17 gate C3-testdiff item 4, 102b DISPATCH-GUARD-PER-USE): the owner-fill dispatch provenance is judged per use,
	// on the CODE. CodeOnly blanks comments, the contents of string and char literals (raw strings too; the quotes stay, so
	// `Dispatch("x")` never reads as `Dispatch()`) and the header name of an #include; every other byte keeps its offset, so the
	// line numbers hold. Other preprocessor lines stay code (a #define alias is a use).
	std::string CodeOnly(const std::string& text)
	{
		std::string out = text;
		const size_t n = text.size();
		size_t i = 0;
		bool lineStart = true;   // nothing but blanks since the last newline
		while (i < n)
		{
			const char c = text[i];
			if (c == '/' && i + 1 < n && text[i + 1] == '/')
			{
				while (i < n && text[i] != '\n')
					out[i++] = ' ';
				continue;
			}
			if (c == '/' && i + 1 < n && text[i + 1] == '*')
			{
				const size_t closeAt = text.find("*/", i + 2);
				const size_t stop = closeAt == std::string::npos ? n : closeAt + 2;
				for (; i < stop; ++i)
				{
					if (text[i] == '\n')
						lineStart = true;
					else
						out[i] = ' ';
				}
				continue;
			}
			if (lineStart && c == '#')
			{
				size_t k = i + 1;
				while (k < n && (text[k] == ' ' || text[k] == '\t'))
					++k;
				if (text.compare(k, 7, "include") == 0)
				{
					k += 7;
					while (k < n && (text[k] == ' ' || text[k] == '\t'))
						++k;
					const char closer = k < n && text[k] == '<' ? '>' : (k < n && text[k] == '"' ? '"' : '\0');
					if (closer != '\0')
					{
						size_t e = k + 1;
						while (e < n && text[e] != closer && text[e] != '\n')
							out[e++] = ' ';
						i = e < n && text[e] == closer ? e + 1 : e;
						lineStart = false;
						continue;
					}
				}
			}
			if (c == '"' || c == '\'')
			{
				size_t p = i;   // the identifier run just before the quote: an encoding or raw-string prefix, or no literal at all
				while (p > 0 && IsIdentChar(text[p - 1]))
					--p;
				const std::string prefix = text.substr(p, i - p);
				if (c == '\'' && !prefix.empty() && prefix != "L" && prefix != "u" && prefix != "U" && prefix != "u8")
				{
					++i;   // a digit separator (1'000), not a char literal
					lineStart = false;
					continue;
				}
				if (c == '"' && (prefix == "R" || prefix == "LR" || prefix == "uR" || prefix == "UR" || prefix == "u8R"))
				{
					const size_t openAt = text.find('(', i + 1);
					const std::string delimiter = openAt == std::string::npos ? std::string() : text.substr(i + 1, openAt - i - 1);
					if (openAt != std::string::npos && delimiter.size() <= 16 && delimiter.find_first_of(" \t\v\f\r\n)\\") == std::string::npos)
					{
						const std::string closeText = ")" + delimiter + "\"";
						const size_t closeAt = text.find(closeText, openAt + 1);
						const size_t last = closeAt == std::string::npos ? n : closeAt + closeText.size() - 1;   // the closing quote
						for (size_t k = i + 1; k < last; ++k)
							if (text[k] != '\n')
								out[k] = ' ';
						i = last + 1;
						lineStart = false;
						continue;
					}
				}
				size_t k = i + 1;
				while (k < n && text[k] != c && text[k] != '\n')
				{
					if (text[k] == '\\' && k + 1 < n)
					{
						out[k++] = ' ';
						if (text[k] != '\n')
							out[k] = ' ';
					}
					else
						out[k] = ' ';
					++k;
				}
				i = k < n && text[k] == c ? k + 1 : k;
				lineStart = false;
				continue;
			}
			if (c == '\n')
				lineStart = true;
			else if (c != ' ' && c != '\t' && c != '\r')
				lineStart = false;
			++i;
		}
		return out;
	}

	struct CodeToken { std::string s; size_t at; };

	// The code as tokens: identifier / number runs, the two-character operators -> :: == !=, and single characters.
	std::vector<CodeToken> CodeTokens(const std::string& code)
	{
		std::vector<CodeToken> out;
		const size_t n = code.size();
		size_t i = 0;
		while (i < n)
		{
			if (static_cast<unsigned char>(code[i]) <= ' ')
			{
				++i;
				continue;
			}
			size_t len = 1;
			if (IsIdentChar(code[i]))
			{
				while (i + len < n && IsIdentChar(code[i + len]))
					++len;
			}
			else if (i + 1 < n)
			{
				const std::string two = code.substr(i, 2);
				if (two == "->" || two == "::" || two == "==" || two == "!=")
					len = 2;
			}
			CodeToken t;
			t.s = code.substr(i, len);
			t.at = i;
			out.push_back(t);
			i += len;
		}
		return out;
	}

	// The context's dispatch (core/SkeletonAcquire.h OwnerGpuContext), and the house cast of it (EdgeMapFill.h).
	static const char* const kContextCall[] = { "req", ".", "gpu", "->", "Dispatch", "(", ")" };
	static const char* const kContextCast[] = { "static_cast", "<", "pl_dispatch", ">", "(", "req", ".", "gpu", "->", "Dispatch", "(", ")", ")" };
	static const size_t kContextCallTokens = sizeof(kContextCall) / sizeof(kContextCall[0]);
	static const size_t kContextCastTokens = sizeof(kContextCast) / sizeof(kContextCast[0]);

	template <size_t N>
	bool TokensAre(const std::vector<CodeToken>& t, size_t at, const char* const (&words)[N])
	{
		if (at > t.size() || t.size() - at < N)
			return false;
		for (size_t k = 0; k < N; ++k)
			if (t[at + k].s != words[k])
				return false;
		return true;
	}

	bool IsNameToken(const CodeToken& tok) { return !tok.s.empty() && IsIdentChar(tok.s[0]) && !(tok.s[0] >= '0' && tok.s[0] <= '9'); }

	// `const pl_dispatch <name> = static_cast<pl_dispatch>(req.gpu->Dispatch());` (or `const auto <name> = ...;`) from token `at`.
	bool ContextDispatchLocalAt(const std::vector<CodeToken>& t, size_t at)
	{
		const size_t end = at + 4 + kContextCastTokens;   // the `;`
		return end < t.size() && t[at].s == "const" && (t[at + 1].s == "pl_dispatch" || t[at + 1].s == "auto") && IsNameToken(t[at + 2])
			&& t[at + 3].s == "=" && TokensAre(t, at + 4, kContextCast) && t[end].s == ";";
	}

	// libplacebo/dispatch.h names that carry no dispatch: the parameter / info structs and the info copy helper.
	bool CarriesNoDispatch(const std::string& s)
	{
		return s == "pl_dispatch_params" || s == "pl_dispatch_compute_params" || s == "pl_dispatch_vertex_params"
			|| s == "pl_dispatch_info" || s == "pl_dispatch_info_move";
	}

	// A libplacebo dispatch function (pl_dispatch_begin / compute / finish / abort / ...): its first argument is the dispatch.
	bool IsDispatchFunction(const std::string& s)
	{
		return s.compare(0, 12, "pl_dispatch_") == 0 && s != "pl_dispatch_t" && !CarriesNoDispatch(s);
	}

	// Pure scanner: "" when every dispatch an owner-fill file's code uses is the context's, else the first fault (with its line):
	//   1. a file that names pl_dispatch (anywhere, as before) takes the context's dispatch in code: a req.gpu->Dispatch() in a
	//      comment or a string does not count;
	//   2. every `pl_dispatch` is inside `static_cast<pl_dispatch>(req.gpu->Dispatch())` or is the type of a const local of it,
	//      never a member / field, a parameter, an alias, a non-const local or a cast of anything else; pl_dispatch_t (the struct
	//      behind it) is never named;
	//   3. every pl_dispatch_* function (the parameter / info structs aside) is called, on a first argument that is such a
	//      local in its scope (declared before the call, in a block still open there), the cast itself or req.gpu->Dispatch();
	//   4. such a local appears only as that first argument: it is never declared again, assigned, captured or handed on (a
	//      helper is handed the cast itself).
	std::string OwnerFillDispatchFault(const std::string& text)
	{
		const std::string code = CodeOnly(text);
		if (text.find("pl_dispatch") != std::string::npos && code.find("req.gpu->Dispatch()") == std::string::npos)
			return "names pl_dispatch but takes no dispatch from req.gpu->Dispatch() in code (a comment or a string does not count)";
		const std::vector<CodeToken> t = CodeTokens(code);
		const auto where = [&text, &t](size_t k) {
			return "line " + std::to_string(1 + std::count(text.begin(), text.begin() + static_cast<std::string::difference_type>(t[k].at), '\n')) + ": ";
		};
		std::vector<std::string> locals;   // the context's const dispatch locals
		std::vector<size_t> localDecls;    // the token of each one's name at its declaration
		std::vector<size_t> localEnds;     // the `}` that closes the block each one is declared in (t.size(): none)
		for (size_t i = 0; i < t.size(); ++i)
			if (ContextDispatchLocalAt(t, i))
			{
				size_t end = i;
				for (int depth = 0; end < t.size(); ++end)
				{
					if (t[end].s == "{")
						++depth;
					else if (t[end].s == "}")
					{
						if (depth == 0)
							break;
						--depth;
					}
				}
				locals.push_back(t[i + 2].s);
				localDecls.push_back(i + 2);
				localEnds.push_back(end);
			}
		const auto isLocal = [&locals](const std::string& s) { return std::find(locals.begin(), locals.end(), s) != locals.end(); };
		// The local `s` is in scope at token u: one of its declarations precedes u inside a block that is still open there (the
		// same name elsewhere in the file, e.g. a member reached from another function, is not the context's).
		const auto localInScope = [&locals, &localDecls, &localEnds](const std::string& s, size_t u) {
			for (size_t k = 0; k < locals.size(); ++k)
				if (locals[k] == s && localDecls[k] < u && u < localEnds[k])
					return true;
			return false;
		};
		for (size_t i = 0; i < t.size(); ++i)
		{
			const std::string& s = t[i].s;
			if (s == "pl_dispatch")
			{
				const bool inCast = i >= 2 && TokensAre(t, i - 2, kContextCast);
				const bool localType = i >= 1 && ContextDispatchLocalAt(t, i - 1);
				if (!inCast && !localType)
					return where(i) + "a pl_dispatch that is not the context's: a member / field, a parameter, an alias, a non-const"
						" local or a cast of something other than req.gpu->Dispatch()";
			}
			else if (s == "pl_dispatch_t")
				return where(i) + "names pl_dispatch_t, the dispatch struct itself";
			else if (IsDispatchFunction(s))
			{
				if (i + 1 >= t.size() || t[i + 1].s != "(")
					return where(i) + s + " is named outside a call";
				size_t end = i + 2;   // the first argument ends at the first top-level ',' or at the call's ')'
				for (int depth = 0; end < t.size(); ++end)
				{
					const std::string& a = t[end].s;
					if (a == "(" || a == "[" || a == "{")
						++depth;
					else if (a == ")" || a == "]" || a == "}")
					{
						if (depth == 0)
							break;
						--depth;
					}
					else if (a == "," && depth == 0)
						break;
				}
				const size_t len = end - (i + 2);
				const bool context = (len == 1 && localInScope(t[i + 2].s, i + 2))
					|| (len == kContextCastTokens && TokensAre(t, i + 2, kContextCast))
					|| (len == kContextCallTokens && TokensAre(t, i + 2, kContextCall));
				if (!context)
					return where(i) + s + " runs on a dispatch that is not the context's (its first argument is not req.gpu->Dispatch(),"
						" its static_cast or a const local of it)";
			}
			else if (isLocal(s) && std::find(localDecls.begin(), localDecls.end(), i) == localDecls.end()
				&& !(i > 0 && (t[i - 1].s == "." || t[i - 1].s == "->" || t[i - 1].s == "::")))
			{
				const bool firstArgument = i >= 2 && t[i - 1].s == "(" && IsDispatchFunction(t[i - 2].s)
					&& i + 1 < t.size() && (t[i + 1].s == "," || t[i + 1].s == ")");
				if (!firstArgument)
					return where(i) + "the context's dispatch local `" + s + "` is used other than as the first argument of a"
						" pl_dispatch_* call (declared again, assigned, captured or handed on: hand on the cast itself)";
			}
		}
		return std::string();
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
			// the wrappers are caught, longer identifiers are not
			Assert::IsTrue(CreatesTexturePrivately("if (!VpvGpu::Ensure(gpu, m_rows, w, h, fmt)) return;"));
			Assert::IsTrue(CreatesTexturePrivately("Ensure(gpu, tex, 1, 1, f);"));
			Assert::IsTrue(CreatesTexturePrivately("if (!EnsureFloat(tex, w, h, 4)) return;"));
			Assert::IsFalse(CreatesTexturePrivately("bool EnsureSize(int w, int h);"));
			Assert::IsFalse(CreatesTexturePrivately("MyEnsure(x);"));
		}

		// R2 (lead 12): no owner fill captures a pl_gpu of its own. A file that defines or handles an owner fill (names
		// OwnerFillRequest) takes the device only through OwnerFillRequest::gpu (the sanctioned OwnerGpuContext): it never names
		// pl_gpu. The memo itself (core/SkeletonAcquire.h, whose comments name the type) is the one exception. The whole VP-V tree
		// is scanned (tests excluded).
		// Category Cpu: the name says GPU, the body is a source-text scan (no device).
		BEGIN_TEST_METHOD_ATTRIBUTE(OwnerFillsTakeNoPrivateGpu)
			TEST_METHOD_ATTRIBUTE(L"Category", L"Cpu")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(OwnerFillsTakeNoPrivateGpu)
		{
			std::vector<std::string> files;
			Walk("src/VP-V", files);
			Assert::IsTrue(!files.empty(), L"the scan found no source files: repository root is wrong");
			size_t fillFiles = 0;
			for (const std::string& rel : files)
			{
				const std::string text = SettingsTestSupport::ReadFile(Native(rel));
				if (text.find("OwnerFillRequest") == std::string::npos)
					continue;
				++fillFiles;
				if (rel == "src/VP-V/core/SkeletonAcquire.h")
					continue;
				const std::wstring message = Widen(rel) + L": an owner fill names pl_gpu: take the device through OwnerFillRequest::gpu"
					L" (core/SkeletonAcquire.h OwnerGpuContext), never a captured pl_gpu";
				Assert::IsTrue(text.find("pl_gpu") == std::string::npos, message.c_str());
				// Audit S4: nor a dispatch of its own (the context's Dispatch() is the one). A guard, not a proof: a stronger pattern
				// is an S-CHK checker change through the lead. C3 (lead 17 R17-22): a fill may RUN on the context's dispatch
				// (pl_dispatch_begin / compute), so the guard forbids creating or destroying one, and a fill that uses pl_dispatch_*
				// must obtain it from req.gpu->Dispatch().
				// C-T4 (102b DISPATCH-GUARD-PER-USE): that provenance is checked PER USE on the code (OwnerFillDispatchFault: comments
				// and literals never count), no longer by the presence of the text anywhere in the file.
				const std::wstring dispatchMessage = Widen(rel) + L": an owner fill owns a pl_dispatch, or uses one not taken from"
					L" req.gpu->Dispatch() (core/SkeletonAcquire.h OwnerGpuContext)";
				Assert::IsTrue(text.find("pl_dispatch_create") == std::string::npos, dispatchMessage.c_str());
				Assert::IsTrue(text.find("pl_dispatch_destroy") == std::string::npos, dispatchMessage.c_str());
				const std::string dispatchFault = OwnerFillDispatchFault(text);
				Assert::IsTrue(dispatchFault.empty(), (dispatchMessage + L": " + Widen(dispatchFault)).c_str());
			}
			Assert::IsTrue(fillFiles >= 2, L"the scan found the memo and at least one fill");
		}

		// C-T4 (gate C3-testdiff item 4): the per-use dispatch scanner on injected text, as ScannerFlagsAnInjectedFile does for the
		// texture scanner. Every red case still names req.gpu->Dispatch() and no pl_dispatch_create / _destroy, so the old
		// file-level check passed it: a member dispatch, a foreign cast, a comment- or string-only req.gpu->Dispatch(), a
		// parameter, the struct itself, a non-const or re-declared local, an alias, code that follows a literal, and the local's
		// name used out of its scope. Green: the house form (EdgeMapFill.h), the cast handed straight to a call, a const auto
		// local, no dispatch at all, and foreign forms that sit only in comments or literals.
		TEST_METHOD(DispatchProvenanceScannerFlagsAnInjectedFile)
		{
			struct Case { const wchar_t* name; std::string text; };
			const std::string house =
				"if (req.gpu == nullptr || req.gpu->Gpu() == nullptr || req.gpu->Dispatch() == nullptr) return false;\n"
				"const pl_dispatch dispatch = static_cast<pl_dispatch>(req.gpu->Dispatch());\n"
				"pl_shader sh = pl_dispatch_begin(dispatch);\n"
				"if (!pl_shader_custom(sh, &custom)) { pl_dispatch_abort(dispatch, &sh); return false; }\n"
				"pl_dispatch_compute_params compute{};\n"
				"compute.shader = &sh;\n"
				"compute.dispatch_size[0] = 1;\n"
				"return pl_dispatch_compute(dispatch, &compute);\n";
			const Case green[] = {
				{ L"the house form (EdgeMapFill.h)", house },
				{ L"the cast handed straight to the call", "pl_shader sh = pl_dispatch_begin(static_cast<pl_dispatch>(req.gpu->Dispatch()));\n" },
				{ L"a const auto local of the cast", "const auto dp = static_cast<pl_dispatch>(req.gpu->Dispatch());\npl_shader sh = pl_dispatch_begin(dp);\n" },
				{ L"no dispatch at all", "const VpSkeleton::AcquireResult src = memo.Acquire(VpSkeleton::kP13SourceRing, req.seq, VpSkeleton::AcquireForm::Gpu);\n" },
				{ L"comments and literals never count", house + "// a pl_dispatch m_dp member, pl_dispatch_begin(m_dp), would be refused\n"
					"/* static_cast<pl_dispatch>(SomeOtherDispatch()) */\nconst char* why = \"pl_dispatch_begin(m_dp)\";\n" },
				{ L"an include of the dispatch header", "#include <libplacebo/dispatch.h>\n" + house },
				{ L"a raw string naming a foreign dispatch", house + "const char* glsl = R\"GLSL(pl_dispatch_begin(m_dp); // \" x)GLSL\";\n" },
			};
			const Case red[] = {
				{ L"a member dispatch", house + "struct Kept { pl_dispatch m_dp = nullptr; };\n" },
				{ L"a member dispatch used by a call", house + "pl_dispatch_reset_frame(kept.m_dp);\n" },
				{ L"a foreign cast", "if (req.gpu == nullptr || req.gpu->Dispatch() == nullptr) return false;\n"
					"const pl_dispatch dispatch = static_cast<pl_dispatch>(SomeOtherDispatch());\npl_shader sh = pl_dispatch_begin(dispatch);\n" },
				{ L"a foreign cast handed to the call", house + "pl_dispatch_reset_frame(static_cast<pl_dispatch>(host->OtherDispatch()));\n" },
				{ L"a C-style cast of a foreign dispatch", house + "pl_dispatch_reset_frame((pl_dispatch)host->dispatch);\n" },
				{ L"a comment-only req.gpu->Dispatch()", "// the dispatch is static_cast<pl_dispatch>(req.gpu->Dispatch())\n"
					"auto dp = host->dispatch;\npl_shader sh = pl_dispatch_begin(dp);\n" },
				{ L"a string-only req.gpu->Dispatch()", "const char* note = \"req.gpu->Dispatch()\";\nauto dp = host->dispatch;\npl_shader sh = pl_dispatch_begin(dp);\n" },
				{ L"a parameter", house + "static bool Finish(pl_dispatch dp) { return pl_dispatch_finish(dp, nullptr); }\n" },
				{ L"the dispatch struct itself", house + "struct pl_dispatch_t* kept = nullptr;\n" },
				{ L"a non-const local", "pl_dispatch dispatch = static_cast<pl_dispatch>(req.gpu->Dispatch());\n"
					"dispatch = host->dispatch;\npl_shader sh = pl_dispatch_begin(dispatch);\n" },
				{ L"the local's name declared again", house + "{ auto dispatch = host->dispatch; pl_dispatch_reset_frame(dispatch); }\n" },
				{ L"the local captured", house + "auto later = [dispatch]() { return dispatch; };\n" },
				{ L"an alias of the type", house + "#define VP_DP pl_dispatch\n" },
				{ L"a dispatch function outside a call", house + "auto begin = &pl_dispatch_begin;\n" },
				{ L"a call on another local", house + "pl_dispatch_reset_frame(other);\n" },
				{ L"code after a raw string", house + "const char* glsl = R\"GLSL(x = \"y\"; // z)GLSL\"; struct K { pl_dispatch m; };\n" },
				{ L"code after a char literal and a digit separator", house + "const char q = '\"'; const int n = 1'000; struct K { pl_dispatch m; };\n" },
				{ L"code after an escaped quote", house + "const char* s = \"a \\\" b\"; struct K { pl_dispatch m; };\n" },
				{ L"a literal argument is not the context call", "if (req.gpu->Dispatch() == nullptr) return false;\n"
					"pl_shader sh = pl_dispatch_begin(static_cast<pl_dispatch>(req.gpu->Dispatch(\"x\")));\n" },
				{ L"the local's name used before its declaration", "pl_dispatch_reset_frame(dispatch);\n" + house },
				{ L"the local's name used outside its block", "{\n" + house + "}\nstruct M { void Run() { pl_dispatch_reset_frame(dispatch); } };\n" },
			};
			for (const Case& c : green)
			{
				const std::string fault = OwnerFillDispatchFault(c.text);
				Assert::IsTrue(fault.empty(), (std::wstring(c.name) + L": " + Widen(fault)).c_str());
			}
			for (const Case& c : red)
			{
				Assert::IsTrue(c.text.find("req.gpu->Dispatch()") != std::string::npos && c.text.find("pl_dispatch_create") == std::string::npos
					&& c.text.find("pl_dispatch_destroy") == std::string::npos, (std::wstring(c.name) + L": the old file-level check must pass this case").c_str());
				Assert::IsFalse(OwnerFillDispatchFault(c.text).empty(), (std::wstring(c.name) + L": not flagged").c_str());
			}
			// the fault names its line (the house form is 8 lines)
			Assert::AreEqual(std::string("line 9: "), OwnerFillDispatchFault(red[0].text).substr(0, 8));
		}
	};
}
