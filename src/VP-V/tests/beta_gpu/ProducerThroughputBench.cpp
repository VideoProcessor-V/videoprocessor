// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// refs/105 phase 0 (lane PT-A): the BEFORE baseline of the optical-flow producer's throughput. Test code only (Beta test host).
// Drives the REAL MotionAhead::Producer (rule 21: no private engine) on its own D3D11 device on the NVIDIA adapter, with synthetic
// P010 frames (a fixed textured canvas panned as a triangle wave) submitted in real time at the frame rate, and measures per run:
//   - submit -> publish latency per pair (the bench polls the producer's P2 slot; resolution about 1 ms),
//   - the producer's own service time per pair (Stats.lastPairMs, read when Stats.pairs advanced by exactly one), its luma and extras
//     parts (Stats.lumaMs when Stats.frames advanced by one; Stats.extrasMs), engine_rest = service - luma - extras (upload + NVOF both
//     directions + staging copy + readback), queue wait = latency - service,
//   - publish intervals, pairs published / submitted, producer drops (queue) and pairs lost, the engine size of the published flow,
//   - the foreign CPU load during the run (GetSystemTimes minus this process): above 10 % the row is marked CONTENDED.
// Limitations (stated in every summary): no render load on the GPU (a lower bound of the live service); the frames are views into one
// immutable canvas (no per-frame upload of new capture memory, so the CPU luma step sees a warm-ish memory system); lumaMs may belong to
// the previous job when two jobs complete within one poll (same work size, so the distribution is unaffected); the stage timers are the
// producer's existing Stats (MotionAheadProducer.* is not edited in this lane).
// A determinism A/A (refs/105 risk 1): the same 24-frame sequence is run serially twice on fresh producers and the P2 payload hashes are
// compared.
// Gated: runs only with VP_PT_BENCH=1. VP_PT_BENCH_OUT = output directory (CSV + summary appended). VP_PT_BENCH_PART = A (determinism,
// A/A first row, repeats 1-2) | B (repeat 3, A/A last row) | ALL (default). VP_PT_BENCH_SECONDS = measured seconds per run (default 5).
#include <CppUnitTest.h>

#include <VP-V/modules/motion/estimation/MotionAheadProducer.h>
#include <VP-V/modules/motion/estimation/NvOpticalFlow.h>
#include <AnalysisLumaSource.h>

#include <atlbase.h>
#include <d3d11.h>
#include <dxgi.h>
#include <windows.h>
#include <timeapi.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "winmm.lib")

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace ProducerThroughput
{
	using Clock = std::chrono::steady_clock;

	inline double MsBetween(Clock::time_point a, Clock::time_point b)
	{
		return std::chrono::duration<double, std::milli>(b - a).count();
	}

	inline std::string Env(const char* name, const char* fallback)
	{
		char* value = nullptr;
		size_t length = 0;
		std::string out = fallback;
		if (_dupenv_s(&value, &length, name) == 0 && value && *value)
			out = value;
		std::free(value);
		return out;
	}

	struct Dist
	{
		std::vector<double> v;
		void Add(double x) { v.push_back(x); }
		size_t Count() const { return v.size(); }
		double Pct(double p) const
		{
			if (v.empty())
				return -1.0;
			std::vector<double> s = v;
			std::sort(s.begin(), s.end());
			const size_t i = static_cast<size_t>(p * (s.size() - 1) + 0.5);
			return s[(std::min)(i, s.size() - 1)];
		}
		double Mean() const
		{
			if (v.empty())
				return -1.0;
			double sum = 0.0;
			for (double x : v)
				sum += x;
			return sum / v.size();
		}
		double Top() const { return v.empty() ? -1.0 : *std::max_element(v.begin(), v.end()); }
	};

	// The synthetic canvas: 10-bit codes << 6 (P010 words), value noise of two octaves plus a pixel hash (texture everywhere, so the
	// engine has structure to match). Wider and taller than the frame by the pan range, plus the rows the P010 chroma plane bound needs.
	struct Canvas
	{
		int frameW = 0, frameH = 0, canvasW = 0, canvasH = 0, range = 0;
		std::shared_ptr<std::vector<uint16_t>> words;
	};

	inline uint32_t Hash(uint32_t x, uint32_t y)
	{
		uint32_t h = x * 0x8da6b343u ^ y * 0xd8163841u ^ 0x9e3779b9u;
		h ^= h >> 15;
		h *= 0x2c1b3c6du;
		h ^= h >> 12;
		h *= 0x297a2d39u;
		h ^= h >> 15;
		return h;
	}

	inline double ValueNoise(int x, int y, int cell)
	{
		const int cx = x / cell, cy = y / cell;
		const double fx = static_cast<double>(x % cell) / cell, fy = static_cast<double>(y % cell) / cell;
		const double a = (Hash(cx, cy) & 1023) / 1023.0, b = (Hash(cx + 1, cy) & 1023) / 1023.0;
		const double c = (Hash(cx, cy + 1) & 1023) / 1023.0, d = (Hash(cx + 1, cy + 1) & 1023) / 1023.0;
		const double top = a + (b - a) * fx, bottom = c + (d - c) * fx;
		return top + (bottom - top) * fy;
	}

	inline Canvas MakeCanvas(int frameW, int frameH)
	{
		Canvas c;
		c.frameW = frameW;
		c.frameH = frameH;
		c.range = 192;
		c.canvasW = frameW + c.range;
		c.canvasH = frameH + c.range + frameH / 2 + 2;
		c.words = std::make_shared<std::vector<uint16_t>>(static_cast<size_t>(c.canvasW) * c.canvasH);
		std::vector<uint16_t>& w = *c.words;
		for (int y = 0; y < c.canvasH; ++y)
			for (int x = 0; x < c.canvasW; ++x)
			{
				const double v = 0.55 * ValueNoise(x, y, 24) + 0.30 * ValueNoise(x, y, 6) + 0.15 * ((Hash(x + 7, y + 3) & 255) / 255.0);
				const int code = 64 + static_cast<int>(v * (940 - 64) + 0.5);
				w[static_cast<size_t>(y) * c.canvasW + x] = static_cast<uint16_t>(code << 6);
			}
		return c;
	}

	inline int Triangle(long long t, int span)
	{
		const long long period = 2LL * span;
		long long p = t % period;
		if (p < 0)
			p += period;
		return static_cast<int>(p <= span ? p : period - p);
	}

	// Frame f's view into the canvas (no copy; the canvas is the backing owner the producer's job keeps alive).
	inline AnalysisLumaSource FrameOf(const Canvas& c, uint64_t f)
	{
		const int x0 = Triangle(static_cast<long long>(f) * 6, c.range), y0 = Triangle(static_cast<long long>(f) * 2, c.range);
		const size_t offset = (static_cast<size_t>(y0) * c.canvasW + x0) * 2;
		const size_t total = c.words->size() * 2;
		AnalysisLumaSource s;
		s.data = reinterpret_cast<const uint8_t*>(c.words->data()) + offset;
		s.dataBytes = total - offset;
		s.width = c.frameW;
		s.height = c.frameH;
		s.rowBytes = static_cast<size_t>(c.canvasW) * 2;
		s.chromaRowBytes = static_cast<size_t>(c.frameW) * 2;
		s.format = AnalysisLumaFormat::P010;
		s.backingOwner = c.words;
		return s;
	}

	inline bool NvidiaDevice(CComPtr<ID3D11Device>& device, std::string& name)
	{
		CComPtr<IDXGIFactory1> factory;
		if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))))
			return false;
		for (UINT i = 0;; ++i)
		{
			CComPtr<IDXGIAdapter1> adapter;
			if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND)
				return false;
			DXGI_ADAPTER_DESC1 desc = {};
			adapter->GetDesc1(&desc);
			if (desc.VendorId != 0x10DE)
				continue;
			const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
			if (FAILED(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &device, nullptr,
				nullptr)))
				return false;
			char buf[256] = {};
			WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, buf, sizeof(buf) - 1, nullptr, nullptr);
			name = buf;
			return true;
		}
	}

	inline unsigned long long FileTimeValue(const FILETIME& t)
	{
		return (static_cast<unsigned long long>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
	}

	// Foreign CPU load: the whole machine's busy time less this process's, as a share of all logical processors.
	struct CpuMeter
	{
		unsigned long long idle = 0, kernel = 0, user = 0, own = 0;
		void Mark()
		{
			FILETIME i, k, u, c, e, pk, pu;
			GetSystemTimes(&i, &k, &u);
			GetProcessTimes(GetCurrentProcess(), &c, &e, &pk, &pu);
			idle = FileTimeValue(i);
			kernel = FileTimeValue(k);
			user = FileTimeValue(u);
			own = FileTimeValue(pk) + FileTimeValue(pu);
		}
		double ForeignPercentSince(const CpuMeter& start) const
		{
			const double total = static_cast<double>((kernel - start.kernel) + (user - start.user));
			const double busy = total - static_cast<double>(idle - start.idle);
			const double mine = static_cast<double>(own - start.own);
			return total > 0.0 ? (std::max)(0.0, 100.0 * (busy - mine) / total) : -1.0;
		}
		double OwnPercentSince(const CpuMeter& start) const
		{
			const double total = static_cast<double>((kernel - start.kernel) + (user - start.user));
			return total > 0.0 ? 100.0 * static_cast<double>(own - start.own) / total : -1.0;
		}
	};

	struct Config
	{
		std::string res;     // "4K" | "1080p"
		bool quarter = false;
		double rate = 24.0;  // frames per second
		int depth = 8;
		std::string Label() const
		{
			char buf[96];
			std::snprintf(buf, sizeof(buf), "%s %s %.2f d=%d", res.c_str(), quarter ? "quarter" : "half", rate, depth);
			return buf;
		}
	};

	struct Row
	{
		Config cfg;
		std::string tag;     // "r1".."r3", "AA-first", "AA-last"
		int order = 0;
		unsigned long long submitted = 0, published = 0, lost = 0, droppedStat = 0, failures = 0;
		int engineW = 0, engineH = 0;
		Dist service, luma, extras, latency, queueWait, interval;
		double foreignCpu = -1.0, ownCpu = -1.0;
		std::string error;
	};

	inline double PeriodMs(double rate)
	{
		// 59.94 / 29.97 / 23.976 as 1001-based periods; others exact.
		if (rate > 59.9 && rate < 59.95) return 1001.0 / 60.0;
		if (rate > 29.9 && rate < 29.98) return 1001.0 / 30.0;
		if (rate > 23.9 && rate < 23.98) return 1001.0 / 24.0;
		return 1000.0 / rate;
	}

	inline void EngineSizeFor(const Config& cfg, int srcW, int srcH, int& w, int& h)
	{
		w = (srcW + 1) / 2;
		h = (srcH + 1) / 2;
		if (cfg.quarter)
		{
			w = (w + 1) / 2;
			h = (h + 1) / 2;
		}
	}

	// One real-time run on a fresh producer.
	inline Row RunOne(ID3D11Device* device, const Canvas& canvas, const Config& cfg, double measureSeconds)
	{
		Row row;
		row.cfg = cfg;
		std::unique_ptr<MotionAhead::Producer> producer(new MotionAhead::Producer());
		int ew = 0, eh = 0;
		EngineSizeFor(cfg, canvas.frameW, canvas.frameH, ew, eh);
		producer->SetTargetSize(ew, eh);
		producer->SetDepth(cfg.depth);
		if (!producer->Start(device, NvOpticalFlow::Preset::Slow))
		{
			row.error = "start: " + producer->Error();
			return row;
		}
		// Prime serially: frames 0 and 1, wait for the pair (the engine opens on first use).
		producer->Submit(0, 1, FrameOf(canvas, 0));
		producer->Submit(1, 1, FrameOf(canvas, 1));
		const Clock::time_point primeDeadline = Clock::now() + std::chrono::seconds(60);
		while (!producer->Take(0, 1))
		{
			if (Clock::now() > primeDeadline || !producer->Running())
			{
				row.error = "prime: no pair 0->1: " + producer->Error();
				producer->Stop();
				return row;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		const double period = PeriodMs(cfg.rate);
		const uint64_t warmFrames = static_cast<uint64_t>(1000.0 / period + 0.5);
		const uint64_t measureFrames = static_cast<uint64_t>(measureSeconds * 1000.0 / period + 0.5);
		const uint64_t firstSeq = 2, measureFrom = firstSeq + warmFrames, endSeq = measureFrom + measureFrames;
		std::map<uint64_t, Clock::time_point> submitAt;
		std::deque<uint64_t> unresolved;
		MotionAhead::Producer::Stats last = producer->GetStats(), base = last;
		bool measuring = false;
		CpuMeter cpu0, cpu1;
		Clock::time_point lastPublish;
		bool havePublish = false;
		uint64_t seq = firstSeq;
		Clock::time_point next = Clock::now();
		Clock::time_point drainUntil;
		for (;;)
		{
			Clock::time_point now = Clock::now();
			if (seq < endSeq && now >= next)
			{
				if (seq == measureFrom)
				{
					measuring = true;
					cpu0.Mark();
					base = producer->GetStats();
				}
				producer->Submit(seq, 1, FrameOf(canvas, seq));
				if (seq >= measureFrom)
				{
					submitAt[seq] = now;
					unresolved.push_back(seq);
					++row.submitted;
				}
				++seq;
				next += std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double, std::milli>(period));
				if (seq == endSeq)
					drainUntil = now + std::chrono::seconds(3);
			}
			// Producer stats: one-step advances give per-pair samples.
			const MotionAhead::Producer::Stats st = producer->GetStats();
			if (measuring)
			{
				if (st.pairs == last.pairs + 1)
				{
					row.service.Add(st.lastPairMs);
					row.extras.Add(st.extrasMs);
				}
				if (st.frames == last.frames + 1)
					row.luma.Add(st.lumaMs);
			}
			last = st;
			// Publishes: the oldest unresolved first; a later hit means the earlier ones were dropped or lost.
			const bool dropsSeen = st.dropped > base.dropped || st.failures > base.failures;
			for (size_t i = 0; i < unresolved.size(); ++i)
			{
				const uint64_t s = unresolved[i];
				MotionAhead::FlowPtr flow = producer->Take(s - 1, s);
				for (uint64_t back = 2; !flow && dropsSeen && back <= 16 && back < s; ++back)
					flow = producer->Take(s - back, s);
				if (!flow)
					continue;
				const Clock::time_point t = Clock::now();
				const double lat = MsBetween(submitAt[s], t);
				row.latency.Add(lat);
				if (havePublish)
					row.interval.Add(MsBetween(lastPublish, t));
				lastPublish = t;
				havePublish = true;
				row.engineW = flow->halfW;
				row.engineH = flow->halfH;
				++row.published;
				row.lost += i;   // the earlier unresolved ones never publish (the worker runs in order)
				unresolved.erase(unresolved.begin(), unresolved.begin() + i + 1);
				producer->Prune(s);
				break;
			}
			if (seq >= endSeq && (unresolved.empty() || now > drainUntil))
				break;
			if (!producer->Running())
			{
				row.error = "producer stopped: " + producer->Error();
				break;
			}
			const double untilNext = MsBetween(Clock::now(), next);
			if (seq >= endSeq || untilNext > 1.5)
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			else
				std::this_thread::yield();
		}
		cpu1.Mark();
		row.lost += unresolved.size();
		row.foreignCpu = cpu1.ForeignPercentSince(cpu0);
		row.ownCpu = cpu1.OwnPercentSince(cpu0);
		const MotionAhead::Producer::Stats end = producer->GetStats();
		row.droppedStat = end.dropped - base.dropped;
		row.failures = end.failures - base.failures;
		// Queue wait per pair: latency less the mean service (the per-pair service sample cannot be tied to a sequence exactly).
		const double meanService = row.service.Mean();
		for (double lat : row.latency.v)
			row.queueWait.Add((std::max)(0.0, lat - meanService));
		producer->Stop();
		return row;
	}

	inline uint64_t Fnv(uint64_t h, const void* data, size_t bytes)
	{
		const uint8_t* p = static_cast<const uint8_t*>(data);
		for (size_t i = 0; i < bytes; ++i)
		{
			h ^= p[i];
			h *= 1099511628211ull;
		}
		return h;
	}

	// Serial pass: frame f submitted only after pair f-1 -> f is published. Returns one hash per pair (vectors + costs, both directions).
	inline bool SerialHashes(ID3D11Device* device, const Canvas& canvas, bool quarter, int frames, std::vector<uint64_t>& hashes,
		std::string& why)
	{
		std::unique_ptr<MotionAhead::Producer> producer(new MotionAhead::Producer());
		Config cfg;
		cfg.quarter = quarter;
		int ew = 0, eh = 0;
		EngineSizeFor(cfg, canvas.frameW, canvas.frameH, ew, eh);
		producer->SetTargetSize(ew, eh);
		if (!producer->Start(device, NvOpticalFlow::Preset::Slow))
		{
			why = "start: " + producer->Error();
			return false;
		}
		hashes.clear();
		for (int f = 0; f < frames; ++f)
		{
			producer->Submit(static_cast<uint64_t>(f), 1, FrameOf(canvas, static_cast<uint64_t>(f)));
			if (f == 0)
				continue;
			const Clock::time_point deadline = Clock::now() + std::chrono::seconds(60);
			MotionAhead::FlowPtr flow;
			while (!(flow = producer->Take(f - 1, f)))
			{
				if (Clock::now() > deadline || !producer->Running())
				{
					why = "no pair " + std::to_string(f) + ": " + producer->Error();
					producer->Stop();
					return false;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			uint64_t h = 1469598103934665603ull;
			h = Fnv(h, flow->forward.data(), flow->forward.size() * sizeof(flow->forward[0]));
			h = Fnv(h, flow->backward.data(), flow->backward.size() * sizeof(flow->backward[0]));
			h = Fnv(h, flow->forwardCost.data(), flow->forwardCost.size() * sizeof(flow->forwardCost[0]));
			h = Fnv(h, flow->backwardCost.data(), flow->backwardCost.size() * sizeof(flow->backwardCost[0]));
			hashes.push_back(h);
		}
		producer->Stop();
		return true;
	}

	inline std::string Fmt(double v)
	{
		char buf[32];
		if (v < 0.0)
			return "-";
		std::snprintf(buf, sizeof(buf), "%.1f", v);
		return buf;
	}

	inline std::string CsvHeader()
	{
		return "tag,order,res,size,rate,depth,engine,submitted,published,lost,dropped,failures,"
			"service_p50,service_p95,service_max,luma_p50,luma_p95,extras_p50,engine_rest_mean,latency_p50,latency_p95,latency_max,"
			"queue_wait_p50,queue_wait_p95,interval_p50,interval_p95,foreign_cpu_pct,own_cpu_pct,contended,error";
	}

	inline std::string CsvLine(const Row& r)
	{
		std::ostringstream o;
		const double rest = (r.service.Count() && r.luma.Count() && r.extras.Count()) ?
			(std::max)(0.0, r.service.Mean() - r.luma.Mean() - r.extras.Mean()) : -1.0;
		o << r.tag << "," << r.order << "," << r.cfg.res << "," << (r.cfg.quarter ? "quarter" : "half") << "," << r.cfg.rate << ","
		  << r.cfg.depth << "," << r.engineW << "x" << r.engineH << "," << r.submitted << "," << r.published << "," << r.lost << ","
		  << r.droppedStat << "," << r.failures << "," << Fmt(r.service.Pct(0.5)) << "," << Fmt(r.service.Pct(0.95)) << ","
		  << Fmt(r.service.Top()) << "," << Fmt(r.luma.Pct(0.5)) << "," << Fmt(r.luma.Pct(0.95)) << "," << Fmt(r.extras.Pct(0.5)) << ","
		  << Fmt(rest) << "," << Fmt(r.latency.Pct(0.5)) << "," << Fmt(r.latency.Pct(0.95)) << "," << Fmt(r.latency.Top()) << ","
		  << Fmt(r.queueWait.Pct(0.5)) << "," << Fmt(r.queueWait.Pct(0.95)) << "," << Fmt(r.interval.Pct(0.5)) << ","
		  << Fmt(r.interval.Pct(0.95)) << "," << Fmt(r.foreignCpu) << "," << Fmt(r.ownCpu) << ","
		  << (r.foreignCpu > 10.0 ? "CONTENDED" : "idle") << "," << r.error;
		return o.str();
	}

	inline std::vector<Config> AllConfigs()
	{
		std::vector<Config> out;
		const int depths[] = { 4, 8, 13 };
		const double rates4k[] = { 24.0, 30.0, 59.94 };
		for (int q = 1; q >= 0; --q)
			for (double rate : rates4k)
				for (int d : depths)
				{
					Config c;
					c.res = "4K";
					c.quarter = q == 1;
					c.rate = rate;
					c.depth = d;
					out.push_back(c);
				}
		const double rates1080[] = { 24.0, 59.94 };
		for (double rate : rates1080)
			for (int d : depths)
			{
				Config c;
				c.res = "1080p";
				c.quarter = false;
				c.rate = rate;
				c.depth = d;
				out.push_back(c);
			}
		return out;
	}
}

TEST_CLASS(ProducerThroughputBench)
{
public:
	TEST_METHOD(ProducerThroughputBefore)
	{
		using namespace ProducerThroughput;
		if (Env("VP_PT_BENCH", "") != "1")
		{
			Logger::WriteMessage("ProducerThroughputBench: VP_PT_BENCH is not 1; nothing run.\n");
			return;
		}
		const std::string outDir = Env("VP_PT_BENCH_OUT", ".");
		const std::string part = Env("VP_PT_BENCH_PART", "ALL");
		const double seconds = std::atof(Env("VP_PT_BENCH_SECONDS", "5").c_str());
		CComPtr<ID3D11Device> device;
		std::string adapter;
		if (!NvidiaDevice(device, adapter))
		{
			Logger::WriteMessage("ProducerThroughputBench: no NVIDIA adapter; nothing run.\n");
			Assert::Fail(L"no NVIDIA D3D11 adapter");
		}
		timeBeginPeriod(1);
		std::map<std::string, Canvas> canvases;
		canvases["4K"] = MakeCanvas(3840, 2160);
		canvases["1080p"] = MakeCanvas(1920, 1080);

		std::ofstream summary(outDir + "\\pt-bench-summary.txt", std::ios::app);
		std::ofstream csv(outDir + "\\pt-bench-runs.csv", std::ios::app);
		auto say = [&](const std::string& line) {
			summary << line << "\n";
			summary.flush();
			Logger::WriteMessage((line + "\n").c_str());
		};
		say("== ProducerThroughputBench part " + part + " on " + adapter + ", " + Fmt(seconds) + " s measured per run, preset Slow, P010 synthetic pan");
		say("limits: no render load on the GPU (lower bound of live service); frames are views of one immutable canvas; stage split = "
			"producer Stats (luma, extras, rest = upload+NVOF x2+copy+readback); queue_wait = latency - mean service");
		if (part == "A" || part == "ALL")
			csv << CsvHeader() << "\n";

		if (part == "A" || part == "ALL")
		{
			// Determinism A/A (refs/105 risk 1): serial vs serial, fresh producers.
			for (int q = 0; q < 2; ++q)
			{
				const Canvas& c = q ? canvases["4K"] : canvases["1080p"];
				std::vector<uint64_t> h1, h2;
				std::string why;
				const bool ok = SerialHashes(device, c, q == 1, 24, h1, why) && SerialHashes(device, c, q == 1, 24, h2, why);
				size_t same = 0;
				for (size_t i = 0; ok && i < h1.size() && i < h2.size(); ++i)
					same += h1[i] == h2[i] ? 1 : 0;
				say(std::string("AA-determinism ") + (q ? "4K quarter" : "1080p half") + ": " +
					(ok ? std::to_string(same) + "/" + std::to_string(h1.size()) + " pair hashes identical" : "FAILED " + why));
			}
		}

		const std::vector<Config> configs = AllConfigs();
		Config aa;
		aa.res = "4K";
		aa.quarter = true;
		aa.rate = 30.0;
		aa.depth = 8;
		std::vector<std::pair<std::string, Config>> plan;
		if (part == "A" || part == "ALL")
			plan.push_back(std::make_pair(std::string("AA-first"), aa));
		const int firstRepeat = part == "B" ? 3 : 1, lastRepeat = part == "A" ? 2 : 3;
		for (int r = firstRepeat; r <= lastRepeat; ++r)
		{
			// Alternate the direction each repeat (forward, reverse, forward): drift and warm-up cancel across repeats.
			std::vector<Config> order = configs;
			if (r % 2 == 0)
				std::reverse(order.begin(), order.end());
			for (const Config& c : order)
				plan.push_back(std::make_pair("r" + std::to_string(r), c));
		}
		if (part == "B" || part == "ALL")
			plan.push_back(std::make_pair(std::string("AA-last"), aa));

		int order = 0;
		unsigned errors = 0;
		for (const auto& item : plan)
		{
			Row r = RunOne(device, canvases[item.second.res], item.second, seconds);
			r.tag = item.first;
			r.order = ++order;
			csv << CsvLine(r) << "\n";
			csv.flush();
			errors += r.error.empty() ? 0 : 1;
			const double rest = (r.service.Count() && r.luma.Count() && r.extras.Count()) ?
				(std::max)(0.0, r.service.Mean() - r.luma.Mean() - r.extras.Mean()) : -1.0;
			say(r.tag + " " + item.second.Label() + " eng " + std::to_string(r.engineW) + "x" + std::to_string(r.engineH) +
				" | service p50/p95 " + Fmt(r.service.Pct(0.5)) + "/" + Fmt(r.service.Pct(0.95)) + " (luma " + Fmt(r.luma.Pct(0.5)) +
				", extras " + Fmt(r.extras.Pct(0.5)) + ", rest " + Fmt(rest) + ") | submit->publish p50/p95 " + Fmt(r.latency.Pct(0.5)) +
				"/" + Fmt(r.latency.Pct(0.95)) + " | interval p50/p95 " + Fmt(r.interval.Pct(0.5)) + "/" + Fmt(r.interval.Pct(0.95)) +
				" | pub " + std::to_string(r.published) + "/" + std::to_string(r.submitted) + " drop " + std::to_string(r.droppedStat) +
				" lost " + std::to_string(r.lost) + " | cpu foreign " + Fmt(r.foreignCpu) + "% " + (r.foreignCpu > 10.0 ? "CONTENDED" : "idle") +
				(r.error.empty() ? "" : " | ERROR " + r.error));
		}
		timeEndPeriod(1);
		say("== part " + part + " done: " + std::to_string(plan.size()) + " runs, " + std::to_string(errors) + " with errors");
		Assert::IsTrue(errors == 0, L"a bench run failed (see pt-bench-summary.txt)");
	}
};
