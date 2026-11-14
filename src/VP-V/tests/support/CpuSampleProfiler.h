// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// COMPUTE-ONCE detector 3, CPU side (iface-L-DET section 3): a test-only sampling profiler that names the hot renderer
// functions no unit owns. Started around a harness run (ComputeOnceRealClipAcceptance with VP_TEST_CPU_SAMPLE=1, or
// tools/compute_once/run_cpu_sample.ps1). Every ~1 ms it suspends each other thread of the process for a few
// microseconds, records the instruction pointer and the first stack words that point into the watched module (a
// heuristic caller chain: no unwinder runs while a thread is suspended, so no lock can be held against us), and
// resumes. Nothing is allocated while a thread is suspended.
// Afterwards each sample is charged to the first frame inside the module; the frame's source file gives the owner:
//   src/VP-V/modules/<unit>/<slot>/...  -> "<unit>.<slot>"         src/VP-V/core/...      -> "core"
//   src/VP-V/<other>/...                -> "vp:<other>"            libplacebo sources     -> "pathway:libplacebo"
//   src/VideoProcessor-Lib/...          -> "pathway:renderer"      (Bill's renderer, the pathway's owner)
// A module frame with no source file (no PDB line, an inlined CRT copy...) is UNATTRIBUTED; functions with at least
// `minPercent` of the module samples are listed:
//   Compute-once cpu owner <owner> samples <n> pct <p>
//   Compute-once cpu UNATTRIBUTED <symbol> samples <n> pct <p>
// A thread stopped in ntdll (a wait or syscall stub) is idle and not sampled.
//   Compute-once cpu summary samples <n> in-module <n> owners <n> unattributed-hot <n>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "dbghelp.lib")

namespace VpTest
{
	class CpuSampleProfiler
	{
	public:
		static constexpr int kFrames = 4;

		explicit CpuSampleProfiler(const char* moduleName) : moduleName_(moduleName) {}
		~CpuSampleProfiler() { Stop(); }

		void Start()
		{
			if (running_.exchange(true))
				return;
			samples_.reserve(1u << 20);
			thread_ = std::thread([this] { Loop(); });
		}
		void Stop()
		{
			if (!running_.exchange(false))
				return;
			if (thread_.joinable())
				thread_.join();
		}

		// The report lines (call after Stop, while the module is still loaded).
		std::vector<std::string> Report(double minPercent = 1.0)
		{
			std::vector<std::string> out;
			SymSetOptions(SymGetOptions() | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
			if (!SymInitialize(GetCurrentProcess(), nullptr, TRUE))
				SymRefreshModuleList(GetCurrentProcess());
			std::map<std::string, uint64_t> owners;
			std::map<std::string, uint64_t> unattributed;
			std::map<uint64_t, std::pair<std::string, std::string>> cache;   // address -> (symbol, owner)
			uint64_t inModule = 0;
			for (const Sample& s : samples_)
			{
				if (!s.frames[0])
					continue;
				++inModule;
				// The first frame with an owner pays (inlined STL / CRT code charges its VP caller); none = the first frame
				// is an unattributed hot function.
				std::string owner, first;
				for (int i = 0; i < kFrames && s.frames[i] && owner.empty(); ++i)
				{
					auto it = cache.find(s.frames[i]);
					if (it == cache.end())
						it = cache.emplace(s.frames[i], Resolve(s.frames[i])).first;
					if (i == 0)
						first = it->second.first;
					owner = it->second.second;
				}
				if (owner.empty())
					++unattributed[first];
				else
					++owners[owner];
			}
			auto pct = [inModule](uint64_t n) { return inModule ? 100.0 * static_cast<double>(n) / static_cast<double>(inModule) : 0.0; };
			char line[1024];
			for (const auto& kv : owners)
			{
				sprintf_s(line, "Compute-once cpu owner %s samples %llu pct %.1f", kv.first.c_str(), static_cast<unsigned long long>(kv.second), pct(kv.second));
				out.push_back(line);
			}
			size_t hot = 0;
			for (const auto& kv : unattributed)
				if (pct(kv.second) >= minPercent)
				{
					++hot;
					sprintf_s(line, "Compute-once cpu UNATTRIBUTED %s samples %llu pct %.1f", kv.first.c_str(), static_cast<unsigned long long>(kv.second), pct(kv.second));
					out.push_back(line);
				}
			sprintf_s(line, "Compute-once cpu summary samples %llu in-module %llu owners %zu unattributed-hot %zu",
				static_cast<unsigned long long>(samples_.size()), static_cast<unsigned long long>(inModule), owners.size(), hot);
			out.push_back(line);
			return out;
		}

		// The owner of a source file (see the header comment); "" = unattributed.
		static std::string OwnerOfFile(std::string file)
		{
			for (char& c : file)
				if (c == '\\')
					c = '/';
			std::string lower = file;
			for (char& c : lower)
				c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
			const std::string mods = "/src/vp-v/modules/";
			size_t at = lower.find(mods);
			if (at != std::string::npos)
			{
				const size_t u = at + mods.size();
				const size_t s1 = lower.find('/', u);
				const size_t s2 = s1 == std::string::npos ? std::string::npos : lower.find('/', s1 + 1);
				if (s2 != std::string::npos)
					return file.substr(u, s1 - u) + "." + file.substr(s1 + 1, s2 - s1 - 1);
			}
			if (lower.find("/src/vp-v/core/") != std::string::npos)
				return "core";
			const std::string vpv = "/src/vp-v/";
			at = lower.find(vpv);
			if (at != std::string::npos)
			{
				const size_t u = at + vpv.size();
				const size_t s1 = lower.find('/', u);
				if (s1 != std::string::npos)
					return "vp:" + file.substr(u, s1 - u);
			}
			if (lower.find("libplacebo") != std::string::npos)
				return "pathway:libplacebo";
			if (lower.find("/src/videoprocessor-lib/") != std::string::npos)
				return "pathway:renderer";
			return std::string();
		}

	private:
		struct Sample
		{
			uint64_t frames[kFrames];
		};

		void Loop()
		{
			const DWORD self = GetCurrentThreadId();
			const DWORD pid = GetCurrentProcessId();
			uint64_t ntdllBase = 0, ntdllEnd = 0;
			std::vector<DWORD> ids;
			ids.reserve(256);
			while (running_.load())
			{
				// Outside any suspension: the module range (the loader lock may be taken here) and the thread list.
				uint64_t base = 0, end = 0;
				CodeRange(GetModuleHandleA(moduleName_), base, end);
				if (!ntdllEnd)
					CodeRange(GetModuleHandleA("ntdll.dll"), ntdllBase, ntdllEnd);
				ids.clear();
				if (base)
				{
					HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
					if (snap != INVALID_HANDLE_VALUE)
					{
						THREADENTRY32 te = {};
						te.dwSize = sizeof(te);
						for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
							if (te.th32OwnerProcessID == pid && te.th32ThreadID != self && ids.size() < ids.capacity())
								ids.push_back(te.th32ThreadID);
						CloseHandle(snap);
					}
				}
				for (DWORD id : ids)
				{
					HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, id);
					if (!t)
						continue;
					Sample s = {};
					bool any = false;
					if (SuspendThread(t) != static_cast<DWORD>(-1))
					{
						CONTEXT c = {};
						c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
						if (GetThreadContext(t, &c))
						{
							int n = 0;
							const bool waiting = c.Rip >= ntdllBase && c.Rip < ntdllEnd;   // a wait / syscall stub: idle, not cost
							if (waiting)
								n = kFrames;
							else if (c.Rip >= base && c.Rip < end)
								s.frames[n++] = c.Rip;
							MEMORY_BASIC_INFORMATION mb = {};
							if (n < kFrames && VirtualQuery(reinterpret_cast<const void*>(c.Rsp), &mb, sizeof(mb)) && mb.State == MEM_COMMIT)
							{
								const uint64_t regionEnd = reinterpret_cast<uint64_t>(mb.BaseAddress) + mb.RegionSize;
								const uint64_t limit = (std::min)(regionEnd, static_cast<uint64_t>(c.Rsp) + 16384);
								for (uint64_t p = c.Rsp & ~7ull; p + 8 <= limit && n < kFrames; p += 8)
								{
									const uint64_t v = *reinterpret_cast<const uint64_t*>(p);
									if (v >= base && v < end)
										s.frames[n++] = v - 1;   // inside the call instruction: the caller's line
								}
							}
							any = !waiting && n > 0;
						}
						ResumeThread(t);
					}
					CloseHandle(t);
					if (any && samples_.size() < samples_.capacity())
						samples_.push_back(s);
				}
				Sleep(1);
			}
		}

		// The module's code section (from its PE header: no loader call).
		static void CodeRange(HMODULE m, uint64_t& base, uint64_t& end)
		{
			base = end = 0;
			if (!m)
				return;
			const uint8_t* image = reinterpret_cast<const uint8_t*>(m);
			const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
			if (dos->e_magic != IMAGE_DOS_SIGNATURE)
				return;
			const IMAGE_NT_HEADERS64* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image + dos->e_lfanew);
			if (nt->Signature != IMAGE_NT_SIGNATURE)
				return;
			base = reinterpret_cast<uint64_t>(image) + nt->OptionalHeader.BaseOfCode;
			end = base + nt->OptionalHeader.SizeOfCode;
		}

		static std::pair<std::string, std::string> Resolve(uint64_t a)
		{
			char buffer[sizeof(SYMBOL_INFO) + 512] = {};
			SYMBOL_INFO* info = reinterpret_cast<SYMBOL_INFO*>(buffer);
			info->SizeOfStruct = sizeof(SYMBOL_INFO);
			info->MaxNameLen = 511;
			DWORD64 displacement = 0;
			std::string symbol;
			if (SymFromAddr(GetCurrentProcess(), a, &displacement, info))
				symbol = info->Name;
			else
			{
				char t[32];
				sprintf_s(t, "0x%llx", static_cast<unsigned long long>(a));
				symbol = t;
			}
			IMAGEHLP_LINE64 line = {};
			line.SizeOfStruct = sizeof(line);
			DWORD lineDisp = 0;
			std::string owner;
			if (SymGetLineFromAddr64(GetCurrentProcess(), a, &lineDisp, &line) && line.FileName)
			{
				owner = OwnerOfFile(line.FileName);
				if (owner.empty())
					symbol += std::string(" [") + line.FileName + "]";
			}
			return std::make_pair(symbol, owner);
		}

		const char* moduleName_;
		std::atomic<bool> running_{ false };
		std::thread thread_;
		std::vector<Sample> samples_;
	};
}
