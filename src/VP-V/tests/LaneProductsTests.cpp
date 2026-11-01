// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// Skeleton S6/S7 lane products (src/VP-V/modules/input/source_analysis/lane/): P1 half luma, P4
// source stats and the event classifier, the P4 walk either/or, the generic GPU-compute lane job,
// the non-blocking P5 readback ring, and two S7 key tests (VRAM drop order, precomputed cut-aligned
// engage with the cascade). CPU only.

#include <VP-V/core/ProductSlots.h>
#include <VP-V/core/VramGovernor.h>
#include <VP-V/modules/input/source_analysis/lane/LaneCpuProducts.h>
#include <VP-V/modules/input/source_analysis/lane/LaneGpuJobs.h>
#include <VP-V/modules/input/source_analysis/lane/LaneReadback.h>
#include <VP-V/modules/input/source_analysis/lane/shared/HalfLuma.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace LaneProductsTestsNs
{
	using namespace VpSkeleton;
	using namespace VpLaneProducts;
	using VpIndicator::Bit;
	using VpIndicator::Indicator;
	using VpIndicator::Stamp;
	using VpResync::Cause;

	constexpr uint32_t kHuge = 0xFFFFFFFFu;

	struct Rng {
		uint32_t s;
		explicit Rng(uint32_t seed) : s(seed) {}
		uint32_t Next()
		{
			s = s * 1664525u + 1013904223u;
			return s >> 8;
		}
	};

	struct Frame {
		uint32_t w = 0, h = 0, stride = 0;
		std::vector<uint16_t> px;
		SourceFrameView View(uint8_t shift) const
		{
			SourceFrameView v;
			v.seq = 1;
			v.luma = px.data();
			v.width = w;
			v.height = h;
			v.strideSamples = stride;
			v.msbShift = shift;
			return v;
		}
	};

	Frame MakeFrame(uint32_t w, uint32_t h, uint32_t stride, uint32_t seed, uint32_t rawMax)
	{
		Frame f;
		f.w = w;
		f.h = h;
		f.stride = stride;
		f.px.resize(size_t(stride) * h);
		Rng r(seed);
		for (uint16_t& p : f.px)
			p = uint16_t(r.Next() % (rawMax + 1u));
		return f;
	}

	uint32_t Clamp(uint32_t v, uint32_t hi) { return v < hi ? v : hi - 1u; }

	std::vector<uint16_t> RefHalf(const Frame& f, uint8_t shift)
	{
		const uint32_t hw = (f.w + 1u) / 2u, hh = (f.h + 1u) / 2u;
		std::vector<uint16_t> out(size_t(hw) * hh);
		for (uint32_t y = 0; y < hh; ++y)
			for (uint32_t x = 0; x < hw; ++x) {
				uint32_t sum = 2u;
				for (uint32_t dy = 0; dy < 2u; ++dy)
					for (uint32_t dx = 0; dx < 2u; ++dx)
						sum += uint16_t(f.px[size_t(Clamp(2u * y + dy, f.h)) * f.stride + Clamp(2u * x + dx, f.w)] << shift);
				out[size_t(y) * hw + x] = uint16_t(sum >> 2);
			}
		return out;
	}

	std::vector<uint16_t> RefQuarter(const std::vector<uint16_t>& half, uint32_t hw, uint32_t hh)
	{
		const uint32_t qw = (hw + 1u) / 2u, qh = (hh + 1u) / 2u;
		std::vector<uint16_t> out(size_t(qw) * qh);
		for (uint32_t y = 0; y < qh; ++y)
			for (uint32_t x = 0; x < qw; ++x) {
				uint32_t sum = 2u;
				for (uint32_t dy = 0; dy < 2u; ++dy)
					for (uint32_t dx = 0; dx < 2u; ++dx)
						sum += half[size_t(Clamp(2u * y + dy, hh)) * hw + Clamp(2u * x + dx, hw)];
				out[size_t(y) * qw + x] = uint16_t(sum >> 2);
			}
		return out;
	}

	// Runs the P1 producer to Done; the number of chunks goes to *chunks.
	std::vector<uint8_t> RunHalf(const Frame& f, uint8_t shift, uint32_t budgetUs, std::function<uint64_t()> clock,
		int* chunks = nullptr)
	{
		const SourceFrameView view = f.View(shift);
		const Dims d{ f.w, f.h };
		std::vector<uint8_t> out(HalfLumaBytes(d), 0xCD);
		HalfLumaProducer p(clock);
		LaneJobContext job;
		job.source = &view;
		job.out = out.data();
		job.outBytes = out.size();
		Assert::IsTrue(p.Begin(job), L"begin");
		int n = 0;
		ChunkResult r = ChunkResult::More;
		while (r == ChunkResult::More && n < 100000) {
			r = p.RunChunk(job, budgetUs);
			++n;
		}
		Assert::IsTrue(r == ChunkResult::Done, L"half luma reaches Done");
		if (chunks)
			*chunks = n;
		return out;
	}

	std::vector<uint8_t> RunStats(const std::vector<uint8_t>& half, Dims d, uint32_t budgetUs,
		std::function<uint64_t()> clock)
	{
		std::vector<uint8_t> out(kStatsBytes, 0xCD);
		SourceStatsProducer p(d, clock);
		LaneJobContext job;
		job.inputData[0] = half.data();
		job.out = out.data();
		job.outBytes = out.size();
		Assert::IsTrue(p.Begin(job), L"begin");
		int n = 0;
		ChunkResult r = ChunkResult::More;
		while (r == ChunkResult::More && n < 100000) {
			r = p.RunChunk(job, budgetUs);
			++n;
		}
		Assert::IsTrue(r == ChunkResult::Done, L"stats reach Done");
		return out;
	}

	SourceStatsRecord StatsOfHalf(const std::vector<uint16_t>& half, uint32_t hw, uint32_t hh)
	{
		std::vector<uint8_t> buf(kStatsBytes);
		Assert::IsTrue(ComputeSourceStats(reinterpret_cast<const uint8_t*>(half.data()), Dims{ hw * 2u, hh * 2u },
			buf.data(), buf.size()), L"stats");
		SourceStatsRecord rec;
		Assert::IsTrue(ReadSourceStats(buf.data(), buf.size(), &rec), L"read");
		return rec;
	}

	// ---- fakes ----

	class FakeDispatcher : public IGpuDispatcher {
	public:
		std::vector<std::pair<uint32_t, uint32_t>> chunks;
		uint32_t failAt = ~0u;
		bool Dispatch(const LaneJobContext&, uint32_t chunk, uint32_t chunkCount) override
		{
			if (chunk == failAt)
				return false;
			chunks.push_back(std::make_pair(chunk, chunkCount));
			return true;
		}
		bool DispatchLive(const Node&, uint64_t, int, const Header&, uint32_t) override { return true; }
	};

	class FakeReadback : public IReadbackDevice {
	public:
		std::vector<char> done;
		std::vector<std::vector<uint8_t>> data;
		bool busy = false;
		int mapCalls = 0, unmaps = 0, copies = 0;
		explicit FakeReadback(size_t n) : done(n, 0), data(n, std::vector<uint8_t>(16, 0)) {}
		bool CopyToStaging(ProductId, uint64_t seq, int i) override
		{
			++copies;
			data[size_t(i)].assign(16, uint8_t(seq));
			done[size_t(i)] = 0;
			return true;
		}
		bool QueryDone(int i) override { return done[size_t(i)] != 0; }
		const void* MapNoWait(int i, size_t* bytes) override
		{
			++mapCalls;
			if (busy)
				return nullptr;
			*bytes = data[size_t(i)].size();
			return data[size_t(i)].data();
		}
		void Unmap(int) override { ++unmaps; }
	};

	// ---- the S7 cascade registry: P0-like (cpu, reads the re-base axis) and P6-like (reads P0) ----

	Registry CascadeRegistry()
	{
		using namespace VpIndicator;
		std::vector<ProductDecl> d;
		d.push_back(detail::Make(0, "p0like", "p0like", Backend::Cpu, 0, Tier::T0,
			Bit(kFull) | Bit(kRebaseH), Bit(kSettingsRebase)));
		d.push_back(detail::Make(1, "p6like", "p6like", Backend::GpuCompute, 0, Tier::T1,
			Bit(kFull) | Bit(kContinuity), 0));
		detail::AddInput(d[1], 0, 0);
		for (ProductDecl& p : d)
			p.size.fixedBytes = 64;
		return Registry(std::move(d));
	}

	struct Cascade {
		Graph graph;
		SlotTable table;
		Indicator ind;
		uint64_t fills = 0;

		Cascade() : graph(Build()), table(graph), ind(5, 64) {}

		static Graph Build()
		{
			ConsumerDecl c;
			c.id = 1;
			AddConsume(c, 1);
			GraphSettings gs;
			gs.dims.width = 64;
			gs.dims.height = 64;
			gs.renderAhead = 0;
			gs.laneLead = 1;
			return BuildGraph(CascadeRegistry(), { c }, gs);
		}

		PublishResult Lane(ProductId p, uint64_t seq)
		{
			SlotSet& s = *table.Find(p);
			LaneTicket t;
			Assert::IsTrue(s.Claim(seq, Provenance::AheadLane, ~uint64_t(0), &t) == ClaimResult::Claimed, L"claim");
			Header& h = s.LaneHeader(t);
			LaneInputs pins;
			Assert::IsTrue(table.LaneAncestors(p, seq, h, pins), L"ancestors");
			h.honoured = ind.StampOf(seq);
			const PublishResult r = s.Publish(t);
			pins.Unpin();
			return r;
		}

		const std::vector<ProductFrameResult>& Resolve(uint64_t seq)
		{
			const Stamp st = ind.Seal(seq);
			return table.Resolve(seq, st, [this](const Node&, uint64_t, int, Header&, uint32_t) {
				++fills;
				return true;
			});
		}
	};

	TEST_CLASS(LaneProductsTests)
	{
	public:
		TEST_METHOD(HalfLumaLayoutAndValues)
		{
			const ProductDecl& decl = FirstProducts().Get(kP1HalfLuma);
			const Dims dims[] = { { 1, 1 }, { 2, 2 }, { 3, 5 }, { 7, 9 }, { 16, 16 }, { 1920, 1080 }, { 1921, 1081 },
				{ 3840, 2160 }, { 7680, 4320 }, { 1, 4320 }, { 7680, 1 } };
			for (const Dims& d : dims)
				Assert::IsTrue(HalfLumaBytes(d) == BytesOf(decl.size, d), L"layout == BytesOf");
			Assert::IsTrue(HalfLumaBytes(Dims{ 1, 1 }) == 4u, L"1x1: one half + one quarter sample");

			const Dims small[] = { { 1, 1 }, { 2, 2 }, { 3, 5 }, { 7, 9 }, { 16, 16 }, { 33, 17 }, { 64, 3 } };
			for (const Dims& d : small)
				for (uint8_t shift : { uint8_t(0), uint8_t(6) }) {
					const Frame f = MakeFrame(d.width, d.height, d.width + 5u, d.width * 31u + d.height,
						shift == 0 ? 65535u : 1023u);
					std::vector<uint8_t> out(HalfLumaBytes(d), 0xCD);
					Assert::IsTrue(ComputeHalfLuma(f.View(shift), out.data(), out.size()), L"compute");
					const uint32_t hw = (d.width + 1u) / 2u, hh = (d.height + 1u) / 2u;
					const std::vector<uint16_t> half = RefHalf(f, shift);
					const std::vector<uint16_t> quarter = RefQuarter(half, hw, hh);
					Assert::IsTrue(std::memcmp(out.data(), half.data(), half.size() * 2u) == 0, L"half plane");
					Assert::IsTrue(std::memcmp(out.data() + half.size() * 2u, quarter.data(), quarter.size() * 2u) == 0,
						L"quarter plane");
				}
			// A short buffer and an invalid view are refused.
			const Frame f = MakeFrame(8, 8, 8, 1, 65535u);
			std::vector<uint8_t> shortBuf(HalfLumaBytes(Dims{ 8, 8 }) - 1u);
			Assert::IsFalse(ComputeHalfLuma(f.View(0), shortBuf.data(), shortBuf.size()), L"short buffer");
			Assert::IsFalse(ComputeHalfLuma(SourceFrameView(), shortBuf.data(), shortBuf.size()), L"invalid view");
		}

		TEST_METHOD(ChunkingIndependenceAndOneShotEquality)
		{
			const Frame f = MakeFrame(75, 59, 80, 77, 65535u);
			const Dims d{ f.w, f.h };
			std::vector<uint8_t> oneShot(HalfLumaBytes(d));
			Assert::IsTrue(ComputeHalfLuma(f.View(0), oneShot.data(), oneShot.size()), L"one shot");

			uint64_t t = 0;
			auto clock = [&t]() { t += 7; return t; };
			int chunksSmall = 0, chunksHuge = 0;
			const std::vector<uint8_t> a = RunHalf(f, 0, 1, nullptr, &chunksSmall);
			const std::vector<uint8_t> b = RunHalf(f, 0, 13, clock);
			const std::vector<uint8_t> c = RunHalf(f, 0, kHuge, nullptr, &chunksHuge);
			Assert::IsTrue(a == oneShot && b == oneShot && c == oneShot, L"P1 identical for any chunking");
			Assert::IsTrue(chunksSmall > chunksHuge && chunksHuge == 2, L"the budget decides only the chunk count");

			std::vector<uint8_t> statsOne(kStatsBytes);
			Assert::IsTrue(ComputeSourceStats(oneShot.data(), d, statsOne.data(), statsOne.size()), L"stats one shot");
			t = 0;
			const std::vector<uint8_t> s1 = RunStats(oneShot, d, 1, nullptr);
			const std::vector<uint8_t> s2 = RunStats(oneShot, d, 13, clock);
			const std::vector<uint8_t> s3 = RunStats(oneShot, d, kHuge, nullptr);
			Assert::IsTrue(s1 == statsOne && s2 == statsOne && s3 == statsOne, L"P4 identical for any chunking");
		}

		TEST_METHOD(SourceStatsRecordContents)
		{
			// Constant frame.
			Frame flat;
			flat.w = 64;
			flat.h = 64;
			flat.stride = 64;
			flat.px.assign(64u * 64u, 40000);
			const Dims d{ 64, 64 };
			std::vector<uint8_t> half(HalfLumaBytes(d));
			Assert::IsTrue(ComputeHalfLuma(flat.View(0), half.data(), half.size()), L"half");
			std::vector<uint8_t> a(kStatsBytes), b(kStatsBytes);
			Assert::IsTrue(ComputeSourceStats(half.data(), d, a.data(), a.size()), L"stats a");
			Assert::IsTrue(ComputeSourceStats(half.data(), d, b.data(), b.size()), L"stats b");
			Assert::IsTrue(a == b, L"deterministic");
			SourceStatsRecord rec;
			Assert::IsTrue(ReadSourceStats(a.data(), a.size(), &rec), L"read");
			Assert::IsTrue(rec.version == kStatsVersion && rec.halfWidth == 32u && rec.halfHeight == 32u, L"header");
			uint64_t sum = 0;
			for (uint32_t i = 0; i < kStatsHistBins; ++i)
				sum += rec.hist[i];
			Assert::IsTrue(sum == 32u * 32u && rec.pixelCount == 1024u, L"histogram sums to the half pixels");
			Assert::IsTrue(rec.mean == 40000u && rec.minCode == 40000u && rec.maxCode == 40000u, L"flat mean/min/max");
			const uint32_t centre = ((40000u >> 6) << 6) | 32u;
			Assert::IsTrue(rec.p01 == centre && rec.p50 == centre && rec.p99 == centre, L"flat percentiles");
			Assert::IsTrue(rec.grain == 0u, L"flat frame has no grain");
			Assert::IsTrue(rec.rectFound == 0u && rec.rectX1 == 32u && rec.rectY1 == 32u, L"no evidence: whole plane");
			for (size_t i = sizeof(SourceStatsRecord); i < a.size(); ++i)
				Assert::IsTrue(a[i] == 0, L"the rest of the buffer is zero");

			// Percentile order on a ramp: values 0..65535 across the width.
			Frame ramp;
			ramp.w = 256;
			ramp.h = 8;
			ramp.stride = 256;
			ramp.px.resize(256u * 8u);
			for (uint32_t y = 0; y < 8; ++y)
				for (uint32_t x = 0; x < 256; ++x)
					ramp.px[size_t(y) * 256 + x] = uint16_t(x * 257u);
			const Dims rd{ 256, 8 };
			std::vector<uint8_t> rh(HalfLumaBytes(rd)), rs(kStatsBytes);
			Assert::IsTrue(ComputeHalfLuma(ramp.View(0), rh.data(), rh.size()), L"ramp half");
			Assert::IsTrue(ComputeSourceStats(rh.data(), rd, rs.data(), rs.size()), L"ramp stats");
			SourceStatsRecord rr;
			Assert::IsTrue(ReadSourceStats(rs.data(), rs.size(), &rr), L"ramp read");
			Assert::IsTrue(rr.p01 < rr.p50 && rr.p50 < rr.p99, L"percentiles ordered");
			Assert::IsTrue(rr.p50 > 28000u && rr.p50 < 37000u, L"median near the middle of the ramp");
			Assert::IsTrue(rr.minCode <= rr.p01 && rr.p99 <= (rr.maxCode | 63u), L"percentiles inside min..max");

			// Letterboxed synthetic frame: picture rows 24..71 of 96, bars black.
			Frame lb;
			lb.w = 128;
			lb.h = 96;
			lb.stride = 128;
			lb.px.assign(128u * 96u, 0);
			Rng r(5);
			for (uint32_t y = 24; y < 72; ++y)
				for (uint32_t x = 0; x < 128; ++x)
					lb.px[size_t(y) * 128 + x] = uint16_t(29000u + r.Next() % 2000u);
			const Dims ld{ 128, 96 };
			std::vector<uint8_t> lh(HalfLumaBytes(ld)), ls(kStatsBytes);
			Assert::IsTrue(ComputeHalfLuma(lb.View(0), lh.data(), lh.size()), L"letterbox half");
			Assert::IsTrue(ComputeSourceStats(lh.data(), ld, ls.data(), ls.size()), L"letterbox stats");
			SourceStatsRecord lr;
			Assert::IsTrue(ReadSourceStats(ls.data(), ls.size(), &lr), L"letterbox read");
			Assert::IsTrue(lr.rectFound == 1u, L"rect found");
			Assert::IsTrue(lr.rectY0 == 12u && lr.rectY1 == 36u, L"rect rows = the picture rows on the half plane");
			Assert::IsTrue(lr.rectX0 == 0u && lr.rectX1 == 64u, L"rect cols = the full width");

			// Grain: noise above flat.
			Frame noise = MakeFrame(128, 128, 128, 9, 65535u);
			const Dims nd{ 128, 128 };
			std::vector<uint8_t> nh(HalfLumaBytes(nd)), ns(kStatsBytes);
			Assert::IsTrue(ComputeHalfLuma(noise.View(0), nh.data(), nh.size()), L"noise half");
			Assert::IsTrue(ComputeSourceStats(nh.data(), nd, ns.data(), ns.size()), L"noise stats");
			SourceStatsRecord nr;
			Assert::IsTrue(ReadSourceStats(ns.data(), ns.size(), &nr), L"noise read");
			Assert::IsTrue(nr.grain > 1000u && nr.grain > rec.grain, L"noise has more grain than flat");
		}

		TEST_METHOD(EventClassification)
		{
			const uint32_t hw = 64, hh = 64;
			Rng r(11);
			std::vector<uint16_t> A(size_t(hw) * hh), faded(A.size()), narrow(A.size()), bright(A.size());
			for (size_t i = 0; i < A.size(); ++i) {
				A[i] = uint16_t(20000u + r.Next() % 20000u);
				faded[i] = uint16_t(uint32_t(A[i]) * 95u / 100u);
				narrow[i] = uint16_t(5000u + r.Next() % 2000u);
				bright[i] = uint16_t(A[i] + 20000u);
			}
			const SourceStatsRecord a = StatsOfHalf(A, hw, hh);
			const SourceStatsRecord f = StatsOfHalf(faded, hw, hh);
			const SourceStatsRecord n = StatsOfHalf(narrow, hw, hh);
			const SourceStatsRecord s = StatsOfHalf(bright, hw, hh);

			EventSignals e = ClassifyEvents(a, a, true, false);
			Assert::IsTrue(e.cut && e.cutPossible, L"FULL between: cut");
			e = ClassifyEvents(a, a, false, true);
			Assert::IsTrue(!e.cut && e.cutPossible && !e.fade && !e.flash && e.histDistance == 0u, L"continuity slip: cutPossible");
			e = ClassifyEvents(a, a, false, false);
			Assert::IsTrue(!e.cut && !e.cutPossible && !e.fade && !e.flash, L"identical frames: nothing");
			e = ClassifyEvents(a, n, false, false);
			Assert::IsTrue(e.cut && !e.flash && !e.fade, L"histogram jump: cut");
			e = ClassifyEvents(a, f, false, false);
			Assert::IsTrue(e.fade && !e.cut && !e.flash, L"gradual mean ramp: fade");
			e = ClassifyEvents(a, s, false, false);
			Assert::IsTrue(e.flash && !e.cut, L"spike up: flash");
			e = ClassifyEvents(s, a, false, false);
			Assert::IsTrue(e.flash && !e.cut, L"spike back down: flash");
		}

		TEST_METHOD(WalkSourceEitherOr)
		{
			Assert::IsTrue(ParseWalkSource(nullptr) == WalkSource::Original, L"null");
			Assert::IsTrue(ParseWalkSource("") == WalkSource::Original, L"empty");
			Assert::IsTrue(ParseWalkSource("original") == WalkSource::Original, L"original");
			Assert::IsTrue(ParseWalkSource("lane") == WalkSource::Lane, L"lane");
			Assert::IsTrue(ParseWalkSource("LANE") == WalkSource::Lane, L"case-insensitive");
			Assert::IsTrue(ParseWalkSource("lanes") == WalkSource::Original, L"unknown stays original");
			Assert::IsTrue(ParseWalkSource("la") == WalkSource::Original, L"prefix is not lane");
			Assert::IsTrue(EffectiveWalk(WalkSource::Original, true) == WalkSource::Original, L"O,avail");
			Assert::IsTrue(EffectiveWalk(WalkSource::Original, false) == WalkSource::Original, L"O,none");
			Assert::IsTrue(EffectiveWalk(WalkSource::Lane, true) == WalkSource::Lane, L"L,avail");
			Assert::IsTrue(EffectiveWalk(WalkSource::Lane, false) == WalkSource::Original, L"L,none: falls back");
		}

		TEST_METHOD(ChunkPlanAndGpuComputeJob)
		{
			const uint32_t rowsList[] = { 0, 1, 7, 64, 1080, 4320 };
			const double predicted[] = { 0.0, 0.5, 3.0, 40.0 };
			const uint32_t caps[] = { 0, 100, 500, 2000, kHuge };
			for (uint32_t rows : rowsList)
				for (double pm : predicted)
					for (uint32_t cap : caps) {
						const ChunkPlan p = PlanChunks(rows, pm, cap);
						Assert::IsTrue(p.chunkCount >= 1u, L"at least one chunk");
						Assert::IsTrue(p.chunkCount <= (rows == 0u ? 1u : rows), L"at most rows chunks");
						if (rows > 0) {
							Assert::IsTrue(uint64_t(p.rowsPerChunk) * p.chunkCount >= rows, L"covers every row");
							Assert::IsTrue(uint64_t(p.rowsPerChunk) * (p.chunkCount - 1u) < rows, L"no empty chunk");
						}
						if (cap == kHuge || pm == 0.0)
							Assert::IsTrue(p.chunkCount == 1u, L"no reason to split");
					}
			const ChunkPlan p = PlanChunks(4320, 3.0, 500);
			Assert::IsTrue(p.chunkCount == 6u && p.rowsPerChunk == 720u, L"3 ms at a 0.5 ms cap: 6 chunks");
			const ChunkPlan q = PlanChunks(100, 100.0, 1);
			Assert::IsTrue(q.chunkCount == 100u && q.rowsPerChunk == 1u, L"clamped to one row per chunk");

			Assert::IsTrue(RowsFor(kP3AnalysisField, Dims{ 1920, 1080 }) == 135u, L"P3 rows");
			Assert::IsTrue(RowsFor(kP5MonitorTiles, Dims{ 1920, 1080 }) == 17u, L"P5 rows");
			Assert::IsTrue(RowsFor(kP6EntranceAhead, Dims{ 1920, 1080 }) == 1080u, L"P6 rows");
			Assert::IsTrue(RowsFor(kP3AnalysisField, Dims{ 7680, 4320 }) == 540u, L"P3 rows 8K");
			Assert::IsTrue(RowsFor(kP5MonitorTiles, Dims{ 7680, 4320 }) == 68u, L"P5 rows 8K");

			// The job: chunk order, Done after the last chunk.
			FakeDispatcher disp;
			GpuComputeLaneJob job(kP6EntranceAhead, &disp, []() { return 3.0; }, []() { return 500u; }, 4320);
			Assert::IsTrue(job.Product() == kP6EntranceAhead && job.ProducerSignal() == 0u, L"identity, no fence");
			LaneJobContext ctx;
			Assert::IsTrue(job.Begin(ctx), L"begin");
			int n = 0;
			ChunkResult r = ChunkResult::More;
			while (r == ChunkResult::More && n < 100) {
				r = job.RunChunk(ctx, 0);
				++n;
			}
			Assert::IsTrue(r == ChunkResult::Done && n == 6, L"Done exactly after the last chunk");
			for (size_t i = 0; i < disp.chunks.size(); ++i)
				Assert::IsTrue(disp.chunks[i].first == uint32_t(i) && disp.chunks[i].second == 6u, L"chunks in order");
			Assert::IsTrue(job.RunChunk(ctx, 0) == ChunkResult::Failed, L"no chunk past the plan");

			// Failed path.
			FakeDispatcher bad;
			bad.failAt = 2;
			GpuComputeLaneJob job2(kP3AnalysisField, &bad, []() { return 3.0; }, []() { return 500u; }, 4320);
			Assert::IsTrue(job2.Begin(ctx), L"begin 2");
			Assert::IsTrue(job2.RunChunk(ctx, 0) == ChunkResult::More, L"chunk 0");
			Assert::IsTrue(job2.RunChunk(ctx, 0) == ChunkResult::More, L"chunk 1");
			Assert::IsTrue(job2.RunChunk(ctx, 0) == ChunkResult::Failed, L"chunk 2 fails");
			Assert::IsTrue(bad.chunks.size() == 2u, L"nothing recorded for the failed chunk");
			GpuComputeLaneJob none(kP3AnalysisField, nullptr, nullptr, nullptr, 10);
			Assert::IsFalse(none.Begin(ctx), L"no dispatcher: cannot run");
		}

		// S5: Begin after an UNFINISHED job (a graph swap leaves one behind) resets all job state.
		TEST_METHOD(BeginAfterUnfinishedJobGivesCorrectBytes)
		{
			const Frame a = MakeFrame(64, 48, 64, 11u, 0xFFFFu);
			const Frame b = MakeFrame(64, 48, 64, 22u, 0xFFFFu);
			const SourceFrameView va = a.View(0);
			const SourceFrameView vb = b.View(0);
			const Dims d{ 64, 48 };
			std::vector<uint8_t> outA(HalfLumaBytes(d), 0xCD);
			std::vector<uint8_t> outB(HalfLumaBytes(d), 0xCD);
			HalfLumaProducer p;
			LaneJobContext ja;
			ja.source = &va;
			ja.out = outA.data();
			ja.outBytes = outA.size();
			Assert::IsTrue(p.Begin(ja), L"begin A");
			Assert::IsTrue(p.RunChunk(ja, 1) == ChunkResult::More, L"A unfinished");
			LaneJobContext jb;
			jb.source = &vb;
			jb.out = outB.data();
			jb.outBytes = outB.size();
			Assert::IsTrue(p.Begin(jb), L"begin B after the unfinished A");
			ChunkResult r = ChunkResult::More;
			for (int n = 0; r == ChunkResult::More && n < 100000; ++n)
				r = p.RunChunk(jb, 1);
			Assert::IsTrue(r == ChunkResult::Done, L"B done");
			std::vector<uint8_t> ref(HalfLumaBytes(d), 0);
			Assert::IsTrue(ComputeHalfLuma(vb, ref.data(), ref.size()), L"reference");
			Assert::IsTrue(outB == ref, L"B's bytes are correct");
		}

		// M3: the device-cost prediction of a GPU-compute job.
		TEST_METHOD(GpuJobPredictsChunkCost)
		{
			FakeDispatcher disp;
			double ms = 3.0;
			uint32_t cap = 1000;
			GpuComputeLaneJob job(kP3AnalysisField, &disp, [&ms]() { return ms; }, [&cap]() { return cap; }, 4320);
			Assert::IsTrue(job.PredictChunkUs() == 1000u, L"before Begin: from the plan of the current inputs (3 ms / 3 chunks)");
			cap = 500;
			Assert::IsTrue(job.PredictChunkUs() == 500u, L"a smaller cap: more chunks, each <= cap");
			LaneJobContext ctx;
			Assert::IsTrue(job.Begin(ctx), L"begin");
			Assert::IsTrue(job.Plan().chunkCount == 6u && job.PredictChunkUs() == 500u, L"in a job: its own plan");
			ms = 6.0;   // inputs change mid-job: the plan in flight is unchanged
			Assert::IsTrue(job.PredictChunkUs() == 500u, L"in-flight plan kept");
			ChunkResult r = ChunkResult::More;
			while (r == ChunkResult::More)
				r = job.RunChunk(ctx, 0);
			Assert::IsTrue(job.PredictChunkUs() == 500u, L"after the job: the next job's plan (6 ms at 0.5 ms = 12 chunks)");
			ms = 0.0;
			Assert::IsTrue(job.PredictChunkUs() == 0u, L"no prediction: 0");
			VpSkeleton::ILaneProducer& base = job;
			Assert::IsTrue(base.PredictChunkUs() == 0u, L"virtual dispatch");
			HalfLumaProducer cpu;
			Assert::IsTrue(cpu.PredictChunkUs() == 0u, L"CPU producers: no device prediction");
		}

		// A cancelled job is no longer in progress: the prediction is the next job's plan again.
		TEST_METHOD(GpuJobCancelEndsTheJob)
		{
			FakeDispatcher disp;
			double ms = 3.0;
			uint32_t cap = 1000;
			GpuComputeLaneJob job(kP3AnalysisField, &disp, [&ms]() { return ms; }, [&cap]() { return cap; }, 4320);
			LaneJobContext ctx;
			Assert::IsTrue(job.Begin(ctx), L"begin");
			Assert::IsTrue(job.PredictChunkUs() == 1000u, L"in the job's own plan");
			ms = 6.0;   // inputs change while the job is in flight
			cap = 500;
			Assert::IsTrue(job.PredictChunkUs() == 1000u, L"in-flight plan kept");
			job.Cancel(ctx);
			Assert::IsTrue(job.PredictChunkUs() == 500u, L"cancelled: the next job's plan (6 ms at 0.5 ms = 12 chunks)");
		}

		TEST_METHOD(ReadbackRingNeverBlocks)
		{
			FakeReadback dev(2);
			ReadbackRing ring(&dev, 2);
			Assert::IsTrue(ring.Submit(kP5MonitorTiles, 10), L"submit 10");
			Assert::IsTrue(ring.Submit(kP5MonitorTiles, 11), L"submit 11");
			Assert::IsFalse(ring.Submit(kP5MonitorTiles, 12), L"no staging left");
			Assert::IsTrue(ring.Counters().skippedNoStaging == 1u && dev.copies == 2, L"exhaustion counted, no copy");

			// The device is busy: Poll never maps, Take of a Copying frame never maps.
			ring.Poll();
			Assert::IsTrue(ring.StateOf(0) == ReadbackRing::State::Copying, L"still copying");
			bool called = false;
			Assert::IsFalse(ring.Take(10, [&](const void*, size_t) { called = true; }), L"not ready: nothing");
			Assert::IsTrue(!called && dev.mapCalls == 0, L"never mapped before the query is done");

			// Both done, but the map is busy (DO_NOT_WAIT): stays Ready, counted, retried.
			dev.done[0] = dev.done[1] = 1;
			ring.Poll();
			Assert::IsTrue(ring.StateOf(0) == ReadbackRing::State::Ready && ring.StateOf(1) == ReadbackRing::State::Ready, L"ready");
			dev.busy = true;
			Assert::IsFalse(ring.Take(11, [&](const void*, size_t) { called = true; }), L"map busy");
			Assert::IsTrue(!called && ring.Counters().mapBusy == 1u && dev.unmaps == 0, L"busy counted, nothing consumed");
			Assert::IsTrue(ring.StateOf(1) == ReadbackRing::State::Ready, L"stays Ready");
			dev.busy = false;
			uint8_t seen = 0;
			size_t bytes = 0;
			Assert::IsTrue(ring.Take(11, [&](const void* p, size_t n) { seen = static_cast<const uint8_t*>(p)[0]; bytes = n; }),
				L"retry succeeds");
			Assert::IsTrue(seen == 11 && bytes == 16u && dev.unmaps == 1, L"consumed seq 11, unmapped");
			Assert::IsTrue(ring.StateOf(1) == ReadbackRing::State::Free, L"taken buffer freed");
			// The older Ready result (seq 10) is stale once 11 was taken.
			Assert::IsTrue(ring.StateOf(0) == ReadbackRing::State::Free && ring.Counters().staleDropped == 1u, L"stale dropped, counted");
			Assert::IsFalse(ring.Take(10, [&](const void*, size_t) { called = true; }), L"stale frame is gone");
			Assert::IsTrue(ring.Submit(kP5MonitorTiles, 12), L"a Free buffer again");

			// Take ordering: a newer Ready taken first keeps an older Copying one.
			FakeReadback dev2(3);
			ReadbackRing ring2(&dev2, 3);
			ring2.Submit(kP5MonitorTiles, 1);
			ring2.Submit(kP5MonitorTiles, 2);
			ring2.Submit(kP5MonitorTiles, 3);
			dev2.done[2] = 1;   // only seq 3 is done
			ring2.Poll();
			Assert::IsTrue(ring2.Take(3, nullptr), L"take the done one");
			Assert::IsTrue(ring2.StateOf(0) == ReadbackRing::State::Copying && ring2.StateOf(1) == ReadbackRing::State::Copying,
				L"older copying buffers are left alone");
			Assert::IsTrue(ring2.Counters().staleDropped == 0u, L"copying is not dropped");
		}

		TEST_METHOD(VramDropMovesP6InRenderFirst)
		{
			std::vector<ProductDecl> decls = FirstProducts().All();
			decls[kP0RebaseDecision].backend = Backend::Cpu;   // P0 laneable in this test
			const Registry reg(std::move(decls));
			ConsumerDecl c;
			c.id = 1;
			AddConsume(c, kP6EntranceAhead);
			AddConsume(c, kP5MonitorTiles);
			AddConsume(c, kP4SourceStats);
			GraphSettings gs;
			gs.dims.width = 3840;
			gs.dims.height = 2160;
			gs.renderAhead = 2;
			const Graph g = BuildGraph(reg, { c }, gs);
			Assert::IsTrue(g.valid && g.Has(kP6EntranceAhead), L"graph with P6");

			constexpr uint64_t kMB = 1024ull * 1024ull;
			CardProfile big;
			big.key.luid = 3;
			big.vramBudgetBytes = 64000 * kMB;
			big.systemRamFreeBytes = 64000 * kMB;
			const TierPlan open = PlanTiers(reg, g, big, TierInput());
			Assert::IsTrue(open.Laned(kP6EntranceAhead) && open.governor.lines.empty(), L"a big card sheds nothing");
			const uint64_t total0 = open.governor.totalBytes;
			const uint64_t ring6 = g.Find(kP6EntranceAhead)->ringBytes;
			Assert::IsTrue(ring6 > 0, L"P6 has a ring");

			// A card that is short by half of P6's ring: exactly one step is needed.
			CardProfile small = big;
			small.vramBudgetBytes = uint64_t(double(total0 - ring6 / 2) / kHeadroomStart) + 1u;
			const TierPlan p = PlanTiers(reg, g, small, TierInput());
			Assert::IsTrue(p.governor.budgetBytes < total0 && p.governor.budgetBytes >= total0 - ring6, L"short by less than P6's ring");
			Assert::IsTrue(!p.Laned(kP6EntranceAhead), L"P6 is the product moved in-render");
			const TierEntry* e6 = p.Find(kP6EntranceAhead);
			Assert::IsTrue(e6 != nullptr && e6->grant == Grant::InRender && e6->why == GrantWhy::OverBudget, L"P6 over budget");
			for (const TierEntry& e : p.entries)
				if (e.tier == Tier::T0)
					Assert::IsTrue(e.grant == Grant::Lane, L"no T0 product moved before P6");
			Assert::IsTrue(p.Laned(kP5MonitorTiles), L"the cheap-per-MB T1 product keeps its lane");
			Assert::IsTrue(!p.t0OverBudget && p.governor.fits, L"nothing essential lost");
			Assert::IsTrue(p.governor.steps == 1u, L"one level step");
			bool line = false;
			for (const std::string& l : p.governor.lines)
				if (l.find("entrance output ahead") != std::string::npos
					&& l.find("computed in the render (no look-ahead)") != std::string::npos)
					line = true;
			Assert::IsTrue(line, L"the plain status line names P6 and the reduced mode");
		}

		TEST_METHOD(PrecomputedCutAlignedEngage)
		{
			const uint64_t C = 2;   // within the first claimable frames of the small rings

			// Ahead: P0 (the re-base decision) published for the cut frame C, P6 built on it.
			{
				Cascade h;
				Assert::IsTrue(h.Lane(0, C) == PublishResult::Published, L"P0 ahead");
				Assert::IsTrue(h.Lane(1, C) == PublishResult::Published, L"P6 ahead");
				const std::vector<ProductFrameResult>& r = h.Resolve(C);
				Assert::IsTrue(r[0].seal.reason == SealReason::Accepted && r[1].seal.reason == SealReason::Accepted,
					L"both sealed Accepted");
				Assert::IsTrue(!r[0].liveFilled && !r[1].liveFilled && h.fills == 0u, L"nothing live-filled");
				Assert::IsTrue(r[1].final == SealOutcome::Sealed, L"P6 at the cut frame is the ahead result");
			}

			// The render makes a different decision at C: P0 is live-filled, P6 built on the old one
			// is rejected AncestorReplaced and live-filled (the cascade).
			{
				Cascade h;
				Assert::IsTrue(h.Lane(0, C) == PublishResult::Published, L"P0 ahead");
				Assert::IsTrue(h.Lane(1, C) == PublishResult::Published, L"P6 ahead");
				h.ind.Advance(VpIndicator::kRebaseH, Cause::RebaseDecision, C);   // the decision at C changed
				const std::vector<ProductFrameResult>& r = h.Resolve(C);
				Assert::IsTrue(r[0].seal.outcome == SealOutcome::NeedsLive && r[0].seal.reason == SealReason::StampMismatch,
					L"P0 rejected: its decision changed");
				Assert::IsTrue(r[0].liveFilled, L"P0 live-filled");
				Assert::IsTrue(r[1].seal.outcome == SealOutcome::NeedsLive && r[1].seal.reason == SealReason::AncestorReplaced,
					L"P6 rejected AncestorReplaced");
				Assert::IsTrue(r[1].liveFilled && r[1].final == SealOutcome::Sealed, L"P6 live-filled");
				Assert::IsTrue(h.fills == 2u, L"exactly the cascade: two live fills");
			}
		}
	};
}

// U3 (shared half-luma unit): the lane's P1 producer now calls VpUnit::HalfLuma. The frozen copies below are the private
// implementations it replaced; the product bytes must be identical.
namespace HalfLumaUnitTestsNs
{
	using namespace VpSkeleton;
	using LaneProductsTestsNs::Frame;
	using LaneProductsTestsNs::MakeFrame;

	inline void OldHalfLumaRows(const SourceFrameView& v, uint16_t* half, uint32_t rowBegin, uint32_t rowEnd)
	{
		const uint32_t hw = (v.width + 1u) / 2u;
		const uint32_t hh = (v.height + 1u) / 2u;
		if (rowEnd > hh)
			rowEnd = hh;
		for (uint32_t y = rowBegin; y < rowEnd; ++y) {
			const uint32_t y0 = 2u * y;
			const uint32_t y1 = std::min(y0 + 1u, v.height - 1u);
			const uint16_t* r0 = v.luma + size_t(y0) * v.strideSamples;
			const uint16_t* r1 = v.luma + size_t(y1) * v.strideSamples;
			uint16_t* dst = half + size_t(y) * hw;
			for (uint32_t x = 0; x < hw; ++x) {
				const uint32_t x0 = 2u * x;
				const uint32_t x1 = std::min(x0 + 1u, v.width - 1u);
				const uint32_t a = uint32_t(uint16_t(r0[x0] << v.msbShift));
				const uint32_t b = uint32_t(uint16_t(r0[x1] << v.msbShift));
				const uint32_t c = uint32_t(uint16_t(r1[x0] << v.msbShift));
				const uint32_t d = uint32_t(uint16_t(r1[x1] << v.msbShift));
				dst[x] = uint16_t((a + b + c + d + 2u) >> 2);
			}
		}
	}

	inline void OldQuarterFromHalf(const uint16_t* half, uint32_t hw, uint32_t hh, uint16_t* quarter, uint32_t rowBegin, uint32_t rowEnd)
	{
		const uint32_t qw = (hw + 1u) / 2u;
		const uint32_t qh = (hh + 1u) / 2u;
		if (rowEnd > qh)
			rowEnd = qh;
		for (uint32_t y = rowBegin; y < rowEnd; ++y) {
			const uint32_t y0 = 2u * y;
			const uint32_t y1 = std::min(y0 + 1u, hh - 1u);
			const uint16_t* r0 = half + size_t(y0) * hw;
			const uint16_t* r1 = half + size_t(y1) * hw;
			uint16_t* dst = quarter + size_t(y) * qw;
			for (uint32_t x = 0; x < qw; ++x) {
				const uint32_t x0 = 2u * x;
				const uint32_t x1 = std::min(x0 + 1u, hw - 1u);
				dst[x] = uint16_t((uint32_t(r0[x0]) + r0[x1] + r1[x0] + r1[x1] + 2u) >> 2);
			}
		}
	}

	TEST_CLASS(HalfLumaUnitTests)
	{
	public:
		TEST_METHOD(UnitVersionAndGridMatchTheLaneProducts)
		{
			Assert::AreEqual(uint32_t(1), VpUnit::HalfLuma::kUnitVersion);
			Assert::AreEqual(VpUnit::HalfLuma::kUnitVersion, VpLaneProducts::kHalfLumaVersion, L"the lane's version is the unit's");
			for (uint32_t v : { 1u, 2u, 3u, 4u, 5u, 7u, 1080u, 1081u, 3840u, 7680u })
			{
				Assert::AreEqual((v + 1u) / 2u, VpUnit::HalfLuma::HalfDim(v));
				Assert::AreEqual((v + 3u) / 4u, VpUnit::HalfLuma::QuarterDim(v));
				Assert::AreEqual(VpUnit::HalfLuma::QuarterDim(v), VpUnit::HalfLuma::HalfDim(VpUnit::HalfLuma::HalfDim(v)), L"quarter = half of half");
			}
			Assert::IsTrue(VpUnit::HalfLuma::TotalBytes(1921, 1081) == VpLaneProducts::HalfLumaBytes(Dims{ 1921, 1081 }));
		}

		TEST_METHOD(ProductBytesAreIdenticalToTheFrozenPrivateCopy)
		{
			const Dims dims[] = { { 1, 1 }, { 2, 2 }, { 3, 5 }, { 5, 3 }, { 7, 9 }, { 16, 16 }, { 33, 17 }, { 64, 3 }, { 1, 40 }, { 41, 1 }, { 255, 129 } };
			for (const Dims& d : dims)
				for (uint8_t shift : { uint8_t(0), uint8_t(6) })
					for (uint32_t rawMax : { 1023u, 65535u })
					{
						if (shift == 6 && rawMax > 1023u) continue;
						const Frame f = MakeFrame(d.width, d.height, d.width + 3u, d.width * 17u + d.height + shift, rawMax);
						// A fully saturated frame as well as noise.
						Frame sat = f;
						std::fill(sat.px.begin(), sat.px.end(), uint16_t(shift == 6 ? 1023u : 65535u));
						for (const Frame* fr : { &f, &sat })
						{
							const uint32_t hw = (d.width + 1u) / 2u, hh = (d.height + 1u) / 2u;
							const uint32_t qw = (hw + 1u) / 2u, qh = (hh + 1u) / 2u;
							std::vector<uint16_t> half(size_t(hw) * hh), quarter(size_t(qw) * qh);
							OldHalfLumaRows(fr->View(shift), half.data(), 0, hh);
							OldQuarterFromHalf(half.data(), hw, hh, quarter.data(), 0, qh);
							std::vector<uint8_t> out(VpLaneProducts::HalfLumaBytes(d), 0xCD);
							Assert::IsTrue(VpLaneProducts::ComputeHalfLuma(fr->View(shift), out.data(), out.size()));
							Assert::IsTrue(std::memcmp(out.data(), half.data(), half.size() * 2u) == 0, L"half plane bytes");
							Assert::IsTrue(std::memcmp(out.data() + half.size() * 2u, quarter.data(), quarter.size() * 2u) == 0, L"quarter plane bytes");
						}
					}
		}

		TEST_METHOD(EngineEightBitFinishSaturatesAndTheGlslNamesAreFixed)
		{
			using namespace VpUnit::HalfLuma;
			Assert::AreEqual(uint8_t(0), FinishEngine8(Box4(0, 0, 0, 0)));
			Assert::AreEqual(uint8_t(255), FinishEngine8(Box4(1023, 1023, 1023, 1023)), L"4092 >> 4 = 255");
			Assert::AreEqual(uint8_t(255), FinishEngine8(Box4(1023, 1023, 1023, 1023) + 100), L"clamped at 255");
			Assert::AreEqual(uint8_t(64), FinishEngine8(Box4(256, 256, 256, 256)));
			Assert::AreEqual(uint16_t(3), FinishP1(Box4(2, 3, 3, 3)));
			Assert::AreEqual(uint16_t(2), FinishP1(Box4(1, 2, 2, 2)), L"(7 + 2) >> 2");
			Assert::AreEqual(0, Tap0(0, 3));
			Assert::AreEqual(1, Tap1(0, 3));
			Assert::AreEqual(2, Tap0(1, 3));
			Assert::AreEqual(2, Tap1(1, 3), L"odd edge replicates: the second tap of the last pair is the last sample");
			Assert::AreEqual(5, Tap1(2, 6));
			const std::string g = Glsl();
			Assert::IsTrue(g.find("vpu_half_luma(sampler2D src, ivec2 q)") != std::string::npos);
			Assert::IsTrue(g.find("vpu_quarter_luma(sampler2D halfSrc, ivec2 q)") != std::string::npos);
			Assert::IsTrue(g.find("VPU_HALF_LUMA_VERSION 1") != std::string::npos);
		}
	};
}
