// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// COMPUTE-ONCE detector 2 (refs/129 addendum 2026-11-13; interface D:\codex\vp-handover\lead7\iface-L-DET.md section 1):
// every shared-unit compute entry point counts its own invocations per (unit, variant, source frame), whoever calls it,
// and records the caller. More than one call for one (unit, variant, frame) is a duplicate, reported with every caller.
//
//   VPV_COMPUTE_ONCE("U1", variant);              frame = the calling thread's frame scope
//   VPV_COMPUTE_ONCE_AT("U3", seq, variant);       frame given by the caller
//   VPV_COMPUTE_ONCE_PRIVATE("U3", "matcher");     a private copy: same unit, variant tag "private:<site>"
// The unit is the bare tools/check_shared_units.py id ("U1".."U22"; the checker's names would trip its own signatures),
// or "<kind>.<name>" for a shared step with no U id (e.g. "nvof.pair").
//
// Frame scope: FrameScope (RAII, the lane runner per job) or SetThreadFrame (sticky, the hub per frame phase).
// A call with no frame known is a coverage gap (counted with its caller), never dropped.
//
// Off (the default) every entry costs one relaxed atomic load and a branch: ComputeOnceTests::CostWhenOff measures it.
// On: env VP_DEBUG_COMPUTE_ONCE=1 (a VP_DEBUG_ diagnostic override, so it is shown), or SetEnabled(true).
// Nothing here changes a pixel or a decision. Pure C++14: no Windows headers (callers are symbolised by
// ComputeOnceReport.h, in the diagnostics module, only when a report is built).

#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#pragma intrinsic(_ReturnAddress)

namespace ComputeOnce
{
	constexpr const char* kEnvName = "VP_DEBUG_COMPUTE_ONCE";
	constexpr size_t kMaxCallersPerRow = 8;
	constexpr size_t kPruneAbove = 8192;      // live (unit, variant, frame) cells before old frames are dropped
	constexpr int64_t kKeepFrames = 64;       // frames kept behind the newest seen

	namespace detail
	{
		// A header-defined global (C++14: a template's static member), constant-initialised: -1 = not read yet.
		template<class T> struct Flag { static std::atomic<int> value; };
		template<class T> std::atomic<int> Flag<T>::value{ -1 };

		__declspec(noinline) inline int ReadEnvironment()
		{
			char* v = nullptr;
			size_t n = 0;
			const bool on = _dupenv_s(&v, &n, kEnvName) == 0 && v && v[0] == '1';
			std::free(v);
			int expected = -1;
			Flag<void>::value.compare_exchange_strong(expected, on ? 1 : 0, std::memory_order_relaxed);
			return Flag<void>::value.load(std::memory_order_relaxed);
		}
	}

	inline bool Enabled()
	{
		const int v = detail::Flag<void>::value.load(std::memory_order_relaxed);
		if (v >= 0)
			return v != 0;
		return detail::ReadEnvironment() != 0;
	}
	inline void SetEnabled(bool on) { detail::Flag<void>::value.store(on ? 1 : 0, std::memory_order_relaxed); }

	// A variant key from up to four integers (FNV-1a over their bytes); 0 only for "no variant".
	inline uint64_t Variant(int64_t a, int64_t b = 0, int64_t c = 0, int64_t d = 0)
	{
		if (a == 0 && b == 0 && c == 0 && d == 0)
			return 0;
		uint64_t h = 1469598103934665603ull;
		const int64_t parts[4] = { a, b, c, d };
		for (int i = 0; i < 4; ++i)
			for (int k = 0; k < 8; ++k)
			{
				h ^= static_cast<uint64_t>((parts[i] >> (8 * k)) & 0xff);
				h *= 1099511628211ull;
			}
		return h == 0 ? 1 : h;
	}

	// ---- frame scope ---------------------------------------------------------------------------------------------
	struct ThreadFrame
	{
		int64_t seq = -1;
		const char* context = nullptr;   // a string literal: "lane", "hub" ...
		int64_t contextId = -1;          // the lane product id or the hub phase number
	};
	inline ThreadFrame& Current()
	{
		static thread_local ThreadFrame t;
		return t;
	}
	// Sticky: stays until the next call on this thread (the hub sets it on every frame phase).
	inline void SetThreadFrame(int64_t seq, const char* context, int64_t contextId)
	{
		if (!Enabled())
			return;
		ThreadFrame& t = Current();
		t.seq = seq;
		t.context = context;
		t.contextId = contextId;
	}
	// RAII: the lane runner around one job's Begin / RunChunk.
	class FrameScope
	{
	public:
		FrameScope(int64_t seq, const char* context, int64_t contextId) : active_(Enabled())
		{
			if (!active_)
				return;
			saved_ = Current();
			ThreadFrame& t = Current();
			t.seq = seq;
			t.context = context;
			t.contextId = contextId;
		}
		~FrameScope()
		{
			if (active_)
				Current() = saved_;
		}
		FrameScope(const FrameScope&) = delete;
		FrameScope& operator=(const FrameScope&) = delete;
	private:
		bool active_;
		ThreadFrame saved_;
	};

	// ---- the registry --------------------------------------------------------------------------------------------
	struct Caller
	{
		const void* address = nullptr;   // the entry point's return address (in the caller)
		const char* context = nullptr;
		int64_t contextId = -1;
		bool operator==(const Caller& o) const
		{
			return address == o.address && contextId == o.contextId
				&& (context == o.context || (context && o.context && std::strcmp(context, o.context) == 0));
		}
	};

	// One (unit, variant) row of the report.
	struct UnitRow
	{
		std::string unit;
		std::string variant;             // hex key, or "private:<site>"
		uint64_t calls = 0;
		uint64_t frames = 0;             // distinct source frames seen
		uint64_t dupFrames = 0;          // frames with more than one call
		uint32_t maxPerFrame = 0;
		std::vector<Caller> callers;     // every caller seen (bounded)
		std::vector<Caller> dupCallers;  // the callers of the duplicate frames (bounded)
	};
	struct GapRow
	{
		std::string unit;
		uint64_t calls = 0;
		std::vector<Caller> callers;
	};

	namespace detail
	{
		inline void AddCaller(std::vector<Caller>& v, const Caller& c)
		{
			if (v.size() >= kMaxCallersPerRow)
				return;
			for (const Caller& x : v)
				if (x == c)
					return;
			v.push_back(c);
		}
		inline std::string HexVariant(uint64_t v)
		{
			static const char* const kHex = "0123456789abcdef";
			std::string s(16, '0');
			for (int i = 15; i >= 0; --i)
			{
				s[static_cast<size_t>(i)] = kHex[v & 0xf];
				v >>= 4;
			}
			return s;
		}
	}

	class Registry
	{
	public:
		void Note(const std::string& unit, const std::string& variant, int64_t seq, const Caller& caller)
		{
			std::lock_guard<std::mutex> lock(mutex_);
			if (seq < 0) { GapRow& g = gaps_[unit]; g.unit = unit; ++g.calls; detail::AddCaller(g.callers, caller); return; }
			const RowKey rk(unit, variant);
			UnitRow& row = rows_[rk];
			if (row.unit.empty()) { row.unit = unit; row.variant = variant; }
			auto rnIt = rowNewest_.find(rk);
			if (rnIt != rowNewest_.end() && seq + kKeepFrames < rnIt->second)
			{
				for (auto it = cells_.lower_bound(CellKey(rk, INT64_MIN)); it != cells_.end() && it->first.first == rk;)
					it = cells_.erase(it);
				rnIt->second = seq;
			}
			else if (rnIt == rowNewest_.end() || seq > rnIt->second) rowNewest_[rk] = seq;
			if (seq > newest_) newest_ = seq;
			++row.calls;
			detail::AddCaller(row.callers, caller);
			Cell& cell = cells_[CellKey(rk, seq)];
			if (++cell.calls == 1) { ++row.frames; cell.first = caller; }
			else { if (cell.calls == 2) { ++row.dupFrames; detail::AddCaller(row.dupCallers, cell.first); } detail::AddCaller(row.dupCallers, caller); }
			row.maxPerFrame = (std::max)(row.maxPerFrame, cell.calls);
			if (cells_.size() > kPruneAbove) Prune();
		}

		std::vector<UnitRow> Rows() const
		{
			std::lock_guard<std::mutex> lock(mutex_);
			std::vector<UnitRow> out;
			out.reserve(rows_.size());
			for (const auto& kv : rows_)
				out.push_back(kv.second);
			return out;
		}
		std::vector<GapRow> Gaps() const
		{
			std::lock_guard<std::mutex> lock(mutex_);
			std::vector<GapRow> out;
			for (const auto& kv : gaps_)
				out.push_back(kv.second);
			return out;
		}
		void Reset()
		{
			std::lock_guard<std::mutex> lock(mutex_);
			rows_.clear();
			cells_.clear();
			rowNewest_.clear();
			gaps_.clear();
			newest_ = -1;
		}
		size_t LiveCells() const
		{
			std::lock_guard<std::mutex> lock(mutex_);
			return cells_.size();
		}

	private:
		using RowKey = std::pair<std::string, std::string>;
		using CellKey = std::pair<RowKey, int64_t>;
		struct Cell
		{
			uint32_t calls = 0;
			Caller first;
		};
		void Prune()
		{
			for (auto it = cells_.begin(); it != cells_.end();)
			{
				const auto rnIt = rowNewest_.find(it->first.first);
				const int64_t rowHead = rnIt != rowNewest_.end() ? rnIt->second : newest_;
				it = it->first.second < (rowHead - kKeepFrames) ? cells_.erase(it) : std::next(it);
			}
		}

		mutable std::mutex mutex_;
		std::map<RowKey, UnitRow> rows_;
		std::map<CellKey, Cell> cells_;
		std::map<RowKey, int64_t> rowNewest_;
		std::map<std::string, GapRow> gaps_;
		int64_t newest_ = -1;
	};

	namespace detail
	{
		template<class T> struct Global { static Registry registry; };
		template<class T> Registry Global<T>::registry;
	}
	inline Registry& Global() { return detail::Global<void>::registry; }

	// ---- the entry-point calls (only reached when Enabled()) ---------------------------------------------------------
	// Kept out of line: inlined, its std::string temporaries give every caller a frame, a security cookie and spills even when off.
	__declspec(noinline) inline void Note(const char* unit, int64_t seq, uint64_t variant, const char* privateSite, const void* returnAddress)
	{
		Caller c;
		c.address = returnAddress;
		const ThreadFrame& t = Current();
		c.context = t.context;
		c.contextId = t.contextId;
		const std::string v = privateSite ? std::string("private:") + privateSite : detail::HexVariant(variant);
		Global().Note(unit ? unit : "?", v, seq, c);
	}
	inline int64_t CurrentSeq() { return Current().seq; }
}

#define VPV_COMPUTE_ONCE(unit, variant) \
	do { if (::ComputeOnce::Enabled()) ::ComputeOnce::Note((unit), ::ComputeOnce::CurrentSeq(), (variant), nullptr, _ReturnAddress()); } while (0)
#define VPV_COMPUTE_ONCE_AT(unit, seq, variant) \
	do { if (::ComputeOnce::Enabled()) ::ComputeOnce::Note((unit), static_cast<int64_t>(seq), (variant), nullptr, _ReturnAddress()); } while (0)
#define VPV_COMPUTE_ONCE_PRIVATE(unit, site) \
	do { if (::ComputeOnce::Enabled()) ::ComputeOnce::Note((unit), ::ComputeOnce::CurrentSeq(), 0, (site), _ReturnAddress()); } while (0)
#define VPV_COMPUTE_ONCE_PRIVATE_AT(unit, seq, site) \
	do { if (::ComputeOnce::Enabled()) ::ComputeOnce::Note((unit), static_cast<int64_t>(seq), 0, (site), _ReturnAddress()); } while (0)
