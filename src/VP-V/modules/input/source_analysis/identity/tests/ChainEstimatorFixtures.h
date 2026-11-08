// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Shared fixtures of the SID-C2b tests: an in-memory evidence file system, a manual / ticking clock, synthetic luma planes and the
// SourceFrameView builder. Deterministic: fixed-seed LCGs, no random device, no wall clock.
#include <VP-V/modules/input/source_analysis/SourceFrameTap.h>
#include <VP-V/modules/input/source_analysis/identity/chain/ChainEstimator.h>
#include <VP-V/modules/input/source_analysis/identity/chain/ChainEvidenceLog.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Tests
{
	namespace ChainFx
	{
		namespace C = SourceIdentity::Chain;
		namespace G = SourceIdentity::Geometry;

		// The evidence folder in memory: paths are "folder\name". Remembers which thread wrote.
		class MemFs : public C::IEvidenceFs
		{
		public:
			bool CreateDirs(const std::string& folder, std::string&) override
			{
				std::lock_guard<std::mutex> g(m);
				dirs.push_back(folder);
				return true;
			}
			bool Write(const std::string& path, const std::string& data, bool append, std::string& error) override
			{
				std::lock_guard<std::mutex> g(m);
				if (failWrites)
				{
					error = "disk full (test)";
					return false;
				}
				writerThreads.push_back(std::this_thread::get_id());
				std::string& f = files[path];
				if (append)
					f += data;
				else
					f = data;
				return true;
			}
			std::vector<C::FileInfo> List(const std::string& folder) override
			{
				std::lock_guard<std::mutex> g(m);
				std::vector<C::FileInfo> out;
				const std::string prefix = C::JoinPath(folder, "");
				for (const auto& f : files)
					if (f.first.compare(0, prefix.size(), prefix) == 0 && f.first.find('\\', prefix.size()) == std::string::npos)
					{
						C::FileInfo fi;
						fi.name = f.first.substr(prefix.size());
						fi.bytes = f.second.size();
						out.push_back(fi);
					}
				return out;
			}
			bool Remove(const std::string& path) override
			{
				std::lock_guard<std::mutex> g(m);
				return files.erase(path) > 0;
			}
			bool Read(const std::string& path, std::string& data) override
			{
				std::lock_guard<std::mutex> g(m);
				const auto it = files.find(path);
				if (it == files.end())
					return false;
				data = it->second;
				return true;
			}

			// ---- test helpers ----
			std::vector<std::string> Names(const std::string& folder)
			{
				std::vector<std::string> out;
				for (const C::FileInfo& f : List(folder))
					out.push_back(f.name);
				return out;
			}
			std::string Get(const std::string& folder, const std::string& name)
			{
				std::string d;
				Read(C::JoinPath(folder, name), d);
				return d;
			}
			// The first file whose name ends with `suffix` ("" when none).
			std::string FirstWithSuffix(const std::string& folder, const std::string& suffix)
			{
				for (const std::string& n : Names(folder))
					if (n.size() >= suffix.size() && n.compare(n.size() - suffix.size(), suffix.size(), suffix) == 0)
						return n;
				return std::string();
			}
			size_t CountWithSuffix(const std::string& folder, const std::string& suffix)
			{
				size_t k = 0;
				for (const std::string& n : Names(folder))
					if (n.size() >= suffix.size() && n.compare(n.size() - suffix.size(), suffix.size(), suffix) == 0)
						++k;
				return k;
			}
			uint64_t TotalBytes(const std::string& folder)
			{
				uint64_t t = 0;
				for (const C::FileInfo& f : List(folder))
					t += f.bytes;
				return t;
			}
			size_t FileCount()
			{
				std::lock_guard<std::mutex> g(m);
				return files.size();
			}
			bool AnyWriteFrom(std::thread::id id)
			{
				std::lock_guard<std::mutex> g(m);
				for (const std::thread::id& t : writerThreads)
					if (t == id)
						return true;
				return false;
			}
			size_t WriteCount()
			{
				std::lock_guard<std::mutex> g(m);
				return writerThreads.size();
			}

			std::mutex m;
			std::map<std::string, std::string> files;
			std::vector<std::string> dirs;
			std::vector<std::thread::id> writerThreads;
			bool failWrites = false;
		};

		// A clock the test sets (no automatic movement) and one that moves by a fixed step on every read.
		struct ManualClock
		{
			std::atomic<double> ms{ 0.0 };
			C::NowMsFn Fn()
			{
				std::atomic<double>* p = &ms;
				return [p]() { return p->load(); };
			}
			void Set(double v) { ms.store(v); }
			void Add(double d) { ms.store(ms.load() + d); }
		};
		struct TickClock
		{
			std::atomic<double> ms{ 0.0 };
			double step = 1100.0;
			C::NowMsFn Fn()
			{
				std::atomic<double>* p = &ms;
				const double s = step;
				return [p, s]() {
					double cur = p->load();
					while (!p->compare_exchange_weak(cur, cur + s))
					{
					}
					return cur;
				};
			}
		};

		inline C::StampFn FixedStamp()
		{
			return []() { return std::string("20260101-000000"); };
		}

		// A luma plane of 10-bit codes (step 4 = an 8-bit picture carried in 10-bit codes, 1 = a true 10-bit picture): a smooth
		// pattern from an 8-px grid of random values plus fine noise, fixed seed.
		struct Plane
		{
			int w = 0, h = 0;
			std::vector<uint16_t> luma;
		};

		inline Plane MakePlane(int w, int h, uint32_t seed, int step)
		{
			Plane p;
			p.w = w;
			p.h = h;
			p.luma.assign(static_cast<size_t>(w) * static_cast<size_t>(h), 0);
			uint32_t x = seed * 2654435761u + 12345u;
			auto rnd = [&x]() {
				x = x * 1664525u + 1013904223u;
				return static_cast<double>(x >> 8) / 16777216.0 - 0.5;
			};
			const int gw = w / 8 + 2, gh = h / 8 + 2;
			std::vector<double> grid(static_cast<size_t>(gw) * static_cast<size_t>(gh));
			for (size_t i = 0; i < grid.size(); ++i)
				grid[i] = rnd();
			for (int y = 0; y < h; ++y)
				for (int xx = 0; xx < w; ++xx)
				{
					const double fx = static_cast<double>(xx) / 8.0, fy = static_cast<double>(y) / 8.0;
					const int ix = static_cast<int>(fx), iy = static_cast<int>(fy);
					const double ax = fx - ix, ay = fy - iy;
					const double v00 = grid[static_cast<size_t>(iy) * gw + ix], v10 = grid[static_cast<size_t>(iy) * gw + ix + 1];
					const double v01 = grid[static_cast<size_t>(iy + 1) * gw + ix], v11 = grid[static_cast<size_t>(iy + 1) * gw + ix + 1];
					const double lf = (v00 * (1.0 - ax) + v10 * ax) * (1.0 - ay) + (v01 * (1.0 - ax) + v11 * ax) * ay;
					double v = 480.0 + 300.0 * lf + 14.0 * rnd();
					v = std::floor(v / step + 0.5) * step;
					v = v < 64.0 ? 64.0 : (v > 940.0 ? 940.0 : v);
					p.luma[static_cast<size_t>(y) * w + xx] = static_cast<uint16_t>(v);
				}
			return p;
		}

		inline VpSkeleton::SourceFrameView View(const Plane& p, uint64_t seq)
		{
			VpSkeleton::SourceFrameView v;
			v.seq = seq;
			v.luma = p.luma.data();
			v.width = static_cast<uint32_t>(p.w);
			v.height = static_cast<uint32_t>(p.h);
			v.strideSamples = static_cast<uint32_t>(p.w);
			v.msbShift = 6;
			return v;
		}

		// One frame into the estimator: its clock entry (the whole frame is the active area), then the delivery.
		inline void Feed(C::ChainEstimator& e, const Plane& p, uint64_t seq, double seconds)
		{
			e.NoteFrame(seq, seconds, 0, 0, 0, 0);
			e.Deliver(View(p, seq));
		}

		inline bool SameBytes(const G::Packed& a, const G::Packed& b)
		{
			for (size_t i = 0; i < G::kRecordBytesMax; ++i)
				if (a.bytes[i] != b.bytes[i])
					return false;
			return true;
		}
	}
}
