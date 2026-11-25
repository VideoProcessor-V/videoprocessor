// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The compute-once report (iface-L-DET section 3): detector 2 (ComputeOnce.h unit counters, the skeleton Acquire
// fold-in) and detector 3 (GpuAttribution.h pass owners) as text lines, one row per line, in the fixed format the
// acceptance harness parses:
//   Compute-once unit <unit> variant <v> frames <n> calls <n> max/frame <n> dup-frames <n> callers <a> | <b> ...
//   Compute-once DUPLICATE <unit> variant <v> dup-frames <n> max/frame <n> callers <a> | <b> ...
//   Compute-once HIDDEN-COPY <unit> variant private:<site> frames <n> calls <n> callers <a> | ...
//   Compute-once gap <unit> calls <n> callers <a> | ...
//   Compute-once gpu owner <owner> passes <n> ms <avg per pass>
//   Compute-once gpu UNATTRIBUTED "<step>" passes <n> ms <avg> in "<description>"
//   Compute-once gpu HIDDEN-COPY "<step>" unit <unit> owner <owner> passes <n> ms <avg>
//   Compute-once gpu rule-unmatched "<pattern>" owner <owner> [unit <unit>]   (a seeded rule no pass matched this run)
//   Compute-once not-instrumented <unit> <name>: <why> (trigger: <when it gains a counter>)
//   Compute-once summary units <n> duplicates <n> hidden-copies <n> gaps <n> gpu-frames <n> gpu-passes <n> gpu-owners <n> gpu-unattributed <n>
//     gpu-hidden-copies <n> gpu-rules <n> gpu-rules-matched <n> gpu-private-rules-matched <n> not-instrumented <n>
// A caller reads "<symbol>+0x<off> [<file>:<line>] @<context>"; symbols come from DbgHelp when the PDB is beside the
// module, else "<module>+0x<offset>". Built only on demand (status command, teardown log, the 30 s log while on).

#include <VP-V/core/ComputeOnce.h>
#include <VP-V/core/GpuAttribution.h>
#include <VP-V/core/ProductCatalog.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>

#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace ComputeOnceReport
{
	// DbgHelp is loaded on demand (as CrashState.cpp's minidump writer does): the renderer has no load-time dbghelp
	// import. <dbghelp.h> is included for the types only. Loaded once on first use while the detector is on and kept
	// for the process lifetime (never freed: SymInitialize state lives in it). Missing -> callers print as
	// "<module>+0x<offset>" and the report and the Fallbacks line say so (no silent downgrade).
	struct DbgHelpApi
	{
		typedef DWORD(WINAPI* GetOptionsFn)();
		typedef DWORD(WINAPI* SetOptionsFn)(DWORD);
		typedef BOOL(WINAPI* InitializeFn)(HANDLE, PCSTR, BOOL);
		typedef BOOL(WINAPI* FromAddrFn)(HANDLE, DWORD64, PDWORD64, PSYMBOL_INFO);
		typedef BOOL(WINAPI* LineFromAddrFn)(HANDLE, DWORD64, PDWORD, PIMAGEHLP_LINE64);
		GetOptionsFn getOptions = nullptr;
		SetOptionsFn setOptions = nullptr;
		InitializeFn initialize = nullptr;
		FromAddrFn fromAddr = nullptr;
		LineFromAddrFn lineFromAddr = nullptr;
		const char* missing = nullptr;   // why symbols are unavailable; null when all entry points resolved
	};

	inline const DbgHelpApi& DbgHelp()
	{
		static const DbgHelpApi api = []() {
			DbgHelpApi a;
			const HMODULE dll = LoadLibraryW(L"dbghelp.dll");
			if (!dll)
			{
				a.missing = "dbghelp.dll not loadable";
				return a;
			}
			a.getOptions = reinterpret_cast<DbgHelpApi::GetOptionsFn>(GetProcAddress(dll, "SymGetOptions"));
			a.setOptions = reinterpret_cast<DbgHelpApi::SetOptionsFn>(GetProcAddress(dll, "SymSetOptions"));
			a.initialize = reinterpret_cast<DbgHelpApi::InitializeFn>(GetProcAddress(dll, "SymInitialize"));
			a.fromAddr = reinterpret_cast<DbgHelpApi::FromAddrFn>(GetProcAddress(dll, "SymFromAddr"));
			a.lineFromAddr = reinterpret_cast<DbgHelpApi::LineFromAddrFn>(GetProcAddress(dll, "SymGetLineFromAddr64"));
			if (!a.getOptions || !a.setOptions || !a.initialize || !a.fromAddr || !a.lineFromAddr)
			{
				a.missing = "dbghelp.dll lacks a Sym* entry point";
				a.fromAddr = nullptr;
				a.lineFromAddr = nullptr;
			}
			return a;
		}();
		return api;
	}

	// The visible note when symbols are unavailable; empty when DbgHelp resolved.
	inline std::string SymbolsUnavailableText()
	{
		const DbgHelpApi& api = DbgHelp();
		return api.missing ? std::string("symbols unavailable (") + api.missing + "): callers shown as <module>+0x<offset>"
			: std::string();
	}

	inline const char* PhaseName(int64_t phase)
	{
		static const char* const kNames[] = { "Convert", "AfterPublish", "CollectAhead", "QueueTarget", "Begin", "PlanOutputs",
			"Entrance", "Upload", "CollectHooks", "Draw", "AfterRender", "PlanPresent", "AfterPresent", "PresentSample",
			"ResolveTime", "DecideIngress" };
		return phase >= 0 && phase < static_cast<int64_t>(sizeof(kNames) / sizeof(kNames[0])) ? kNames[phase] : "?";
	}

	inline std::string ContextText(const ComputeOnce::Caller& c)
	{
		if (!c.context)
			return "@no-scope";
		const std::string ctx = c.context;
		if (ctx == "hub")
			return std::string("@hub:") + PhaseName(c.contextId);
		if (ctx == "lane")
		{
			const VpSkeleton::Catalog::ProductRow* row = VpSkeleton::Catalog::RowOf(static_cast<VpSkeleton::ProductId>(c.contextId));
			const std::string name = row && row->laneProducer && row->laneProducer[0] ? row->laneProducer
				: (row && row->unit ? row->unit : "");
			return "@lane:" + std::to_string(c.contextId) + (name.empty() ? std::string() : "(" + name + ")");
		}
		return "@" + ctx + ":" + std::to_string(c.contextId);
	}

	// "<symbol>+0x<off> [file:line]" or "<module>+0x<offset>".
	inline std::string Symbol(const void* address)
	{
		if (!address)
			return "?";
		static std::mutex m;   // DbgHelp is single-threaded
		std::lock_guard<std::mutex> lock(m);
		const DbgHelpApi& api = DbgHelp();
		static const bool ready = [&api]() {
			if (api.missing)
				return false;
			api.setOptions(api.getOptions() | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
			// A second SymInitialize of this process (another DbgHelp user) fails; the symbols already loaded still serve.
			api.initialize(GetCurrentProcess(), nullptr, TRUE);
			return true;
		}();
		const DWORD64 a = reinterpret_cast<DWORD64>(address);
		char buffer[sizeof(SYMBOL_INFO) + 512] = {};
		SYMBOL_INFO* info = reinterpret_cast<SYMBOL_INFO*>(buffer);
		info->SizeOfStruct = sizeof(SYMBOL_INFO);
		info->MaxNameLen = 511;
		DWORD64 displacement = 0;
		char text[1024] = {};
		if (ready && api.fromAddr(GetCurrentProcess(), a, &displacement, info))
		{
			IMAGEHLP_LINE64 line = {};
			line.SizeOfStruct = sizeof(line);
			DWORD lineDisp = 0;
			if (api.lineFromAddr(GetCurrentProcess(), a, &lineDisp, &line) && line.FileName)
			{
				const char* file = line.FileName;
				for (const char* p = line.FileName; *p; ++p)
					if (*p == '\\' || *p == '/')
						file = p + 1;
				sprintf_s(text, "%s+0x%llx [%s:%lu]", info->Name, static_cast<unsigned long long>(displacement), file,
					static_cast<unsigned long>(line.LineNumber));
			}
			else
				sprintf_s(text, "%s+0x%llx", info->Name, static_cast<unsigned long long>(displacement));
			return text;
		}
		HMODULE module = nullptr;
		if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			static_cast<const char*>(address), &module) && module)
		{
			char path[MAX_PATH] = {};
			GetModuleFileNameA(module, path, MAX_PATH);
			const char* name = path;
			for (const char* p = path; *p; ++p)
				if (*p == '\\' || *p == '/')
					name = p + 1;
			sprintf_s(text, "%s+0x%llx", name, static_cast<unsigned long long>(a - reinterpret_cast<DWORD64>(module)));
			return text;
		}
		sprintf_s(text, "0x%llx", static_cast<unsigned long long>(a));
		return text;
	}

	inline std::string Callers(const std::vector<ComputeOnce::Caller>& callers)
	{
		std::string s;
		for (const ComputeOnce::Caller& c : callers)
		{
			if (!s.empty())
				s += " | ";
			s += Symbol(c.address) + " " + ContextText(c);
		}
		return s.empty() ? std::string("-") : s;
	}

	// The shared units WITHOUT a counter (audit L-DET MUST 5, rule 5: absence is printed, not silent). Each has a 102b row
	// (refs/102b, "L-DET uninstrumented units"); a unit leaves this list in the commit that gives it a counter.
	struct Uncounted { const char* unit; const char* name; const char* why; const char* trigger; };
	inline const std::vector<Uncounted>& NotInstrumented()
	{
		static const std::vector<Uncounted> list = {
			{ "U14", "trail_compose", "no implementation (TD shared function, not built)", "the TD build package that adds it" },
			{ "U15", "field_refine", "RefinedFieldProducer unbound (Bound() false), no compute", "the producer binding commit" },
			{ "U16", "lighting_fit", "no implementation (TD shared function, not built)", "the TD build package that adds it" },
			{ "U18", "codec_model", "no implementation (TD shared function, not built)", "the TD build package that adds it" },
			{ "U19", "td_maps_view", "read-only accessor, computes nothing", "a compute added behind the view" },
			{ "U21", "source_geometry", "read-only view of P14g, computes nothing", "a compute added behind the view" },
			{ "U22", "motion_engine_instance", "an instance, not a per-frame compute: the SK-MV one-engine guard (NvOpticalFlow::Sessions) counts it",
				"a second engine type outside the guard" },
		};
		return list;
	}

	inline bool IsPrivate(const ComputeOnce::UnitRow& r) { return r.variant.compare(0, 8, "private:") == 0; }

	struct Totals
	{
		size_t units = 0, duplicates = 0, hiddenCopies = 0, gaps = 0;
		uint64_t gpuFrames = 0, gpuPasses = 0;
		size_t gpuOwners = 0, gpuUnattributed = 0, gpuHiddenCopies = 0;
		size_t gpuRules = 0, gpuRulesMatched = 0, gpuPrivateRulesMatched = 0;
	};

	inline Totals Count()
	{
		Totals t;
		for (const ComputeOnce::UnitRow& r : ComputeOnce::Global().Rows())
		{
			++t.units;
			if (r.maxPerFrame > 1)
				++t.duplicates;
			if (IsPrivate(r))
				++t.hiddenCopies;
		}
		t.gaps = ComputeOnce::Global().Gaps().size();
		const GpuAttribution::Registry& g = GpuAttribution::Global();
		t.gpuFrames = g.Frames();
		t.gpuPasses = g.Passes();
		t.gpuOwners = g.Owners().size();
		t.gpuUnattributed = g.Unattributed().size();
		t.gpuHiddenCopies = g.PrivateCopies().size();
		for (const GpuAttribution::Rule& r : g.Rules())
		{
			++t.gpuRules;
			if (r.hits > 0)
			{
				++t.gpuRulesMatched;
				if (r.kind == GpuAttribution::Kind::PrivateCopy)
					++t.gpuPrivateRulesMatched;
			}
		}
		return t;
	}

	inline std::string Ms(uint64_t ns, uint64_t passes)
	{
		char b[32];
		sprintf_s(b, "%.3f", passes ? static_cast<double>(ns) / static_cast<double>(passes) / 1e6 : 0.0);
		return b;
	}

	// Every row; the summary last. Empty while the detector is off.
	inline std::vector<std::string> Lines()
	{
		std::vector<std::string> out;
		if (!ComputeOnce::Enabled())
			return out;
		for (const ComputeOnce::UnitRow& r : ComputeOnce::Global().Rows())
		{
			out.push_back("Compute-once unit " + r.unit + " variant " + r.variant + " frames " + std::to_string(r.frames) + " calls "
				+ std::to_string(r.calls) + " max/frame " + std::to_string(r.maxPerFrame) + " dup-frames " + std::to_string(r.dupFrames)
				+ " callers " + Callers(r.callers));
			if (r.maxPerFrame > 1)
				out.push_back("Compute-once DUPLICATE " + r.unit + " variant " + r.variant + " dup-frames " + std::to_string(r.dupFrames)
					+ " max/frame " + std::to_string(r.maxPerFrame) + " callers " + Callers(r.dupCallers));
			if (IsPrivate(r))
				out.push_back("Compute-once HIDDEN-COPY " + r.unit + " variant " + r.variant + " frames " + std::to_string(r.frames)
					+ " calls " + std::to_string(r.calls) + " callers " + Callers(r.callers));
		}
		for (const ComputeOnce::GapRow& g : ComputeOnce::Global().Gaps())
			out.push_back("Compute-once gap " + g.unit + " calls " + std::to_string(g.calls) + " callers " + Callers(g.callers));
		const GpuAttribution::Registry& gpu = GpuAttribution::Global();
		for (const GpuAttribution::OwnerRow& o : gpu.Owners())
			out.push_back("Compute-once gpu owner " + o.owner + " passes " + std::to_string(o.passes) + " ms " + Ms(o.ns, o.passes));
		for (const GpuAttribution::StepRow& s : gpu.Unattributed())
			out.push_back("Compute-once gpu UNATTRIBUTED \"" + s.step + "\" passes " + std::to_string(s.passes) + " ms " + Ms(s.ns, s.passes)
				+ " in \"" + s.example + "\"");
		for (const GpuAttribution::StepRow& s : gpu.PrivateCopies())
			out.push_back("Compute-once gpu HIDDEN-COPY \"" + s.step + "\" unit " + s.unit + " owner " + s.owner + " passes "
				+ std::to_string(s.passes) + " ms " + Ms(s.ns, s.passes));
		for (const GpuAttribution::Rule& rule : gpu.Rules())
			if (rule.hits == 0)
				out.push_back("Compute-once gpu rule-unmatched \"" + rule.pattern + "\" owner " + rule.owner
					+ (rule.unit.empty() ? std::string() : " unit " + rule.unit));
		for (const Uncounted& u : NotInstrumented())
			out.push_back(std::string("Compute-once not-instrumented ") + u.unit + " " + u.name + ": " + u.why + " (trigger: " + u.trigger + ")");
		const std::string noSymbols = SymbolsUnavailableText();
		if (!noSymbols.empty())
			out.push_back("Compute-once " + noSymbols);
		const Totals t = Count();
		out.push_back("Compute-once summary units " + std::to_string(t.units) + " duplicates " + std::to_string(t.duplicates)
			+ " hidden-copies " + std::to_string(t.hiddenCopies) + " gaps " + std::to_string(t.gaps) + " gpu-frames "
			+ std::to_string(t.gpuFrames) + " gpu-passes " + std::to_string(t.gpuPasses) + " gpu-owners " + std::to_string(t.gpuOwners)
			+ " gpu-unattributed " + std::to_string(t.gpuUnattributed) + " gpu-hidden-copies " + std::to_string(t.gpuHiddenCopies)
			+ " gpu-rules " + std::to_string(t.gpuRules) + " gpu-rules-matched " + std::to_string(t.gpuRulesMatched)
			+ " gpu-private-rules-matched " + std::to_string(t.gpuPrivateRulesMatched)
			+ " not-instrumented " + std::to_string(NotInstrumented().size()));
		return out;
	}

	// The one Fallbacks line (Ctrl+I fallbacks row, Config status, Ctrl+Alt+D) while on and something is wrong.
	inline std::string StatusLine()
	{
		if (!ComputeOnce::Enabled())
			return std::string();
		const Totals t = Count();
		const std::string noSymbols = SymbolsUnavailableText();
		if (t.duplicates == 0 && t.gpuUnattributed == 0 && t.gaps == 0 && t.hiddenCopies == 0 && t.gpuHiddenCopies == 0)
			return noSymbols.empty() ? std::string() : "compute-once: " + noSymbols;
		return "compute-once: " + std::to_string(t.duplicates) + " duplicate units, " + std::to_string(t.hiddenCopies + t.gpuHiddenCopies)
			+ " hidden copies, " + std::to_string(t.gpuUnattributed) + " unattributed passes, " + std::to_string(t.gaps) + " coverage gaps"
			+ (noSymbols.empty() ? std::string() : ", " + noSymbols);
	}
}
