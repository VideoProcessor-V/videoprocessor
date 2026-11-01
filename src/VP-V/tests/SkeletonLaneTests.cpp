// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// Skeleton S3 (src/VP-V/core/LaneRunner.h, SealPoint.h): the lane runner state machine on the real
// SlotTable and Indicator, driven step by step with deterministic CPU/OFA fake producers and an
// injected clock: lane on time, stored = recomputed = live fallback, chunking independence, forced
// late, render-ahead with a shallow queue, late Indicator events, stale/skip/unlaned, OFA fences,
// a two-thread stress, and the N1 bounded-wait truth table. CPU only.

#include <VP-V/core/LaneRunner.h>
#include <VP-V/core/SealPoint.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace SkeletonLaneTestsNs
{
	using namespace VpSkeleton;
	using VpIndicator::Bit;
	using VpIndicator::Indicator;
	using VpIndicator::Stamp;
	using VpResync::Cause;

	constexpr ProductId kA = 0;   // reads the source frame
	constexpr ProductId kB = 1;   // pair: A(N), A(N-1); span rule on full + continuity
	constexpr ProductId kO = 2;   // OFA-backend fake: A(N)
	constexpr uint32_t kBytes = 64;

	Registry Reg()
	{
		using namespace VpIndicator;
		const CounterMask content = Bit(kFull) | Bit(kContinuity);
		std::vector<ProductDecl> d;
		d.push_back(detail::Make(kA, "a", "a", Backend::Cpu, 0, Tier::T0, Bit(kFull), 0));
		d.back().sourceFrame = true;
		d.push_back(detail::Make(kB, "b", "b", Backend::Cpu, 0, Tier::T0, content, 0));
		detail::AddInput(d.back(), kA, 0);
		detail::AddInput(d.back(), kA, 1);
		d.back().spanMask = content;
		d.back().missingInputForm = true;
		d.push_back(detail::Make(kO, "o", "o", Backend::Ofa, 0, Tier::T0, Bit(kFull), 0));
		detail::AddInput(d.back(), kA, 0);
		for (ProductDecl& p : d)
			p.size.fixedBytes = kBytes;
		return Registry(std::move(d));
	}

	Graph MakeGraph(uint8_t R, uint8_t L, bool withB, bool withO)
	{
		ConsumerDecl c;
		c.id = 1;
		if (withB)
			AddConsume(c, kB);
		if (withO)
			AddConsume(c, kO);
		GraphSettings gs;
		gs.dims.width = 64;
		gs.dims.height = 64;
		gs.renderAhead = R;
		gs.laneLead = L;
		return BuildGraph(Reg(), { c }, gs);
	}

	uint16_t LumaAt(uint64_t seq, size_t j) { return uint16_t(seq * 131u + j * 17u); }

	// The one deterministic byte function: every path (lane chunks, live fill, reference) uses it.
	uint8_t ByteAt(ProductId id, uint64_t seq, size_t i, uint8_t a, uint8_t b, uint8_t luma)
	{
		switch (id) {
		case kA: return uint8_t(luma + seq * 5u + i);
		case kB: return uint8_t(a * 3u + b * 5u + i + 1u);
		default: return uint8_t(a * 7u + seq + i);
		}
	}

	std::vector<uint8_t> Ref(ProductId p, uint64_t seq)
	{
		std::vector<uint8_t> out(kBytes);
		if (p == kA) {
			for (size_t i = 0; i < kBytes; ++i)
				out[i] = ByteAt(kA, seq, i, 0, 0, uint8_t(LumaAt(seq, i % 64)));
			return out;
		}
		const std::vector<uint8_t> ra = Ref(kA, seq);
		const std::vector<uint8_t> rb = (p == kB && seq > 0) ? Ref(kA, seq - 1) : std::vector<uint8_t>(kBytes, 0);
		for (size_t i = 0; i < kBytes; ++i)
			out[i] = ByteAt(p, seq, i, ra[i], rb[i], 0);
		return out;
	}

	// A deterministic producer: writes bytes from the frame, its input bytes and the source luma.
	// Byte mode: `budget` bytes per chunk. Fixed-chunk mode (OFA fake): n chunks, then the fence.
	class Fake : public ILaneProducer {
	public:
		explicit Fake(ProductId id, bool fence = false, uint32_t fixedChunks = 0)
			: id_(id), fence_(fence), fixedChunks_(fixedChunks)
		{
		}
		ProductId Product() const override { return id_; }
		bool Begin(const LaneJobContext& job) override
		{
			++begins;
			if (failBegin) {
				failBegin = false;
				return false;
			}
			job_ = job;
			progress_ = 0;
			chunks_ = 0;
			finished = false;
			if (fence_)
				signal_ = ++nextFence_;
			return job.out != nullptr;
		}
		ChunkResult RunChunk(const LaneJobContext&, uint32_t budgetUs) override
		{
			++chunkCalls;
			const size_t n = job_.outBytes;
			if (fixedChunks_ != 0) {
				if (++chunks_ < fixedChunks_)
					return ChunkResult::More;
				Write(0, n);
				finished = true;
				return ChunkResult::Done;
			}
			const size_t step = budgetUs == 0 ? 1u : size_t(budgetUs);
			const size_t end = std::min(n, progress_ + step);
			Write(progress_, end);
			progress_ = end;
			if (end >= n) {
				finished = true;
				return ChunkResult::Done;
			}
			return ChunkResult::More;
		}
		uint64_t ProducerSignal() const override { return fence_ ? signal_ : 0u; }
		uint64_t ProducerCompleted() const override { return fence_ ? completed : ~uint64_t(0); }
		void Cancel(const LaneJobContext&) override { ++cancels; }

		bool failBegin = false;
		bool finished = false;
		uint64_t completed = 0;
		int begins = 0;
		int cancels = 0;
		int chunkCalls = 0;

	private:
		void Write(size_t from, size_t to)
		{
			for (size_t i = from; i < to; ++i) {
				const uint8_t a = job_.inputData[0] ? job_.inputData[0][i] : uint8_t(0);
				const uint8_t b = job_.inputData[1] ? job_.inputData[1][i] : uint8_t(0);
				const uint8_t l = job_.source ? uint8_t(job_.source->luma[i % 64]) : uint8_t(0);
				job_.out[i] = ByteAt(id_, job_.seq, i, a, b, l);
			}
		}
		ProductId id_;
		bool fence_;
		uint32_t fixedChunks_;
		LaneJobContext job_;
		size_t progress_ = 0;
		uint32_t chunks_ = 0;
		uint64_t signal_ = 0;
		uint64_t nextFence_ = 0;
	};

	bool Progress(StepOutcome o)
	{
		return o == StepOutcome::Ran || o == StepOutcome::Published || o == StepOutcome::Late
		       || o == StepOutcome::Abandoned;
	}

	// The graph, slots, indicator, CPU buffers, sources, fake producers and runner of one run.
	struct Rig {
		Graph graph;
		SlotTable table;
		Indicator ind;
		std::vector<CpuBuffers> bufs;
		std::vector<uint16_t> lumaStore;
		std::vector<SourceFrameView> views;
		std::vector<char> dropped;
		uint64_t clock = 0;
		uint64_t fills = 0;
		LaneRunner runner;
		Fake fa, fb, fo;

		Rig(uint8_t R, uint8_t L, bool withB = true, bool withO = false, size_t frames = 64)
			: graph(MakeGraph(R, L, withB, withO))
			, table(graph)
			, ind(31, 256)
			, runner(table, &ind, [this]() { clock += 3; return clock; })
			, fa(kA)
			, fb(kB)
			, fo(kO, true, 3)
		{
			Assert::IsTrue(graph.valid, L"graph valid");
			for (const Node& n : graph.nodes)
				bufs.emplace_back(n);
			lumaStore.resize(frames * 64);
			views.resize(frames);
			dropped.assign(frames, 0);
			for (size_t s = 0; s < frames; ++s) {
				for (size_t j = 0; j < 64; ++j)
					lumaStore[s * 64 + j] = LumaAt(s, j);
				views[s].seq = s;
				views[s].luma = lumaStore.data() + s * 64;
				views[s].width = 8;
				views[s].height = 8;
				views[s].strideSamples = 8;
			}
			runner.Register(&fa);
			runner.Register(&fb);
			runner.Register(&fo);
			runner.SetBuffers(
				[this](ProductId p, int slot) { return Buf(p).Ring(size_t(slot)); },
				[this](ProductId p, uint64_t seq, int live) -> const uint8_t* {
					return live >= 0 ? Buf(p).Live(size_t(live)) : Buf(p).Ring(table.Find(p)->SlotOf(seq));
				},
				[this](ProductId p) { return Buf(p).Bytes(); });
			runner.SetSources([this](uint64_t seq, SourceFrameView* out) -> bool {
				if (seq >= views.size() || dropped[size_t(seq)])
					return false;
				if (out)
					*out = views[size_t(seq)];
				return true;
			});
			runner.SetReadsSource(kA, true);
		}

		CpuBuffers& Buf(ProductId p) { return bufs[size_t(graph.IndexOf(p))]; }
		SlotSet& Set(ProductId p) { return *table.Find(p); }
		void Arrive(uint64_t seq) { runner.OnSourceFrame(seq); }

		void Pump(uint32_t budget, bool a = true, bool b = true, bool o = true)
		{
			for (int it = 0; it < 200000; ++it) {
				bool any = false;
				if (a && graph.Has(kA))
					any = Progress(runner.Step(kA, budget)) || any;
				if (b && graph.Has(kB))
					any = Progress(runner.Step(kB, budget)) || any;
				if (o && graph.Has(kO))
					any = Progress(runner.Step(kO, budget)) || any;
				if (!any)
					return;
			}
			Assert::Fail(L"pump did not settle");
		}

		bool Fill(const Node& n, uint64_t seq, int live, uint32_t missing)
		{
			const uint8_t* in[2] = { nullptr, nullptr };
			for (size_t k = 0; k < n.inputCount && k < 2; ++k) {
				if ((missing >> k) & 1u)
					continue;
				SlotSet* s = table.Find(n.inputs[k].product);
				const ReadInfo ri = s->Read(seq - n.inputs[k].lag);
				if (ri.kind == ReadInfo::Ring)
					in[k] = Buf(n.inputs[k].product).Ring(size_t(ri.index));
				else if (ri.kind == ReadInfo::Live)
					in[k] = Buf(n.inputs[k].product).Live(size_t(ri.index));
			}
			uint8_t* out = Buf(n.id).Live(size_t(live));
			for (size_t i = 0; i < kBytes; ++i) {
				const uint8_t l = (n.id == kA && seq < views.size()) ? uint8_t(views[size_t(seq)].luma[i % 64]) : uint8_t(0);
				out[i] = ByteAt(n.id, seq, i, in[0] ? in[0][i] : uint8_t(0), in[1] ? in[1][i] : uint8_t(0), l);
			}
			++fills;
			return true;
		}

		const std::vector<ProductFrameResult>& Seal(uint64_t seq)
		{
			const Stamp st = ind.Seal(seq);
			return table.Resolve(seq, st, [this](const Node& n, uint64_t s, int live, Header&, uint32_t missing) {
				return Fill(n, s, live, missing);
			});
		}
		const ProductFrameResult& Res(const std::vector<ProductFrameResult>& r, ProductId p) const
		{
			return r[size_t(graph.IndexOf(p))];
		}
		void Release(uint64_t seq) { table.ReleaseAfter(seq); }

		std::vector<uint8_t> Bytes(ProductId p, uint64_t seq)
		{
			const ReadInfo ri = Set(p).Read(seq);
			std::vector<uint8_t> v;
			if (ri.kind == ReadInfo::Ring)
				v.assign(Buf(p).Ring(size_t(ri.index)), Buf(p).Ring(size_t(ri.index)) + kBytes);
			else if (ri.kind == ReadInfo::Live)
				v.assign(Buf(p).Live(size_t(ri.index)), Buf(p).Live(size_t(ri.index)) + kBytes);
			return v;
		}
	};

	struct Collected {
		std::vector<std::vector<uint8_t>> a, b;
		std::vector<SealReason> ra, rb;
		std::vector<bool> liveB;
	};

	// Frame s: arrive s+1 (L = 1), run the lane, seal s at its render start, release.
	void RunFrames(Rig& rig, uint64_t n, uint32_t budget, bool lane, Collected* out)
	{
		for (uint64_t s = 0; s < n; ++s) {
			if (lane) {
				rig.Arrive(s + 1);
				rig.Pump(budget);
			}
			const std::vector<ProductFrameResult>& r = rig.Seal(s);
			if (out) {
				out->a.push_back(rig.Bytes(kA, s));
				out->b.push_back(rig.Bytes(kB, s));
				out->ra.push_back(rig.Res(r, kA).seal.reason);
				out->rb.push_back(rig.Res(r, kB).seal.reason);
				out->liveB.push_back(rig.Res(r, kB).liveFilled);
			}
			rig.Release(s);
		}
	}

	TEST_CLASS(SkeletonLaneTests)
	{
	public:
		// 1. The lane keeps up: every frame is accepted at the seal (B(0) has no A(-1): lane-late, live).
		TEST_METHOD(LaneOnTimeAcceptsEveryFrame)
		{
			Rig rig(0, 1);
			Collected c;
			RunFrames(rig, 24, 1000, true, &c);
			for (size_t s = 0; s < c.a.size(); ++s) {
				Assert::IsTrue(c.ra[s] == SealReason::Accepted, L"A accepted");
				if (s >= 1)
					Assert::IsTrue(c.rb[s] == SealReason::Accepted, L"B accepted");
			}
			Assert::IsTrue(c.rb[0] == SealReason::LaneLate && c.liveB[0], L"B(0): no previous frame, lane-late, live");
			Assert::IsTrue(rig.runner.Counters(kA).published.load() >= 24, L"A published");
			Assert::IsTrue(rig.runner.Counters(kB).abandoned.load() == 1, L"B(0) abandoned once");
			Assert::IsTrue(rig.runner.Counters(kB).latePublishes.load() == 0, L"no late publishes");
			Assert::IsTrue(rig.runner.Counters(kA).laneUs.load() > 0, L"lane time measured");
			Assert::IsTrue(rig.runner.IgnoredProducers() == 1, L"the OFA fake has no node: ignored and counted");
			const ReadInfo ri = rig.Set(kA).Read(23);
			Assert::IsTrue(ri.kind == ReadInfo::Ring && ri.header && ri.header->costUs == 3u, L"cost = measured chunk time");
			Assert::IsTrue(ri.header->prov == Provenance::AheadLane, L"provenance");
		}

		// 2. Stored (ring) = recomputed (one shot) = in-render fallback (live fill), byte for byte.
		TEST_METHOD(StoredEqualsRecomputedEqualsFallback)
		{
			Rig lane(0, 1), live(0, 1);
			Collected cl, cv;
			RunFrames(lane, 20, 1000, true, &cl);
			RunFrames(live, 20, 1000, false, &cv);
			for (size_t s = 0; s < 20; ++s) {
				Assert::IsTrue(cl.a[s] == Ref(kA, s), L"A ring = reference");
				Assert::IsTrue(cl.b[s] == Ref(kB, s), L"B ring = reference");
				Assert::IsTrue(cv.a[s] == cl.a[s], L"A live = ring");
				Assert::IsTrue(cv.b[s] == cl.b[s], L"B live = ring");
				Assert::IsTrue(cv.ra[s] != SealReason::Accepted, L"live-only run has nothing to accept");
			}
			Assert::IsTrue(live.fills >= 40, L"every product filled live");
			Assert::IsTrue(lane.fills < live.fills, L"the lane run needed far fewer fills");
		}

		// 3. The chunking never changes the result.
		TEST_METHOD(ChunkingIndependence)
		{
			Collected c1, c7, c1000;
			{ Rig r(0, 1); RunFrames(r, 12, 1, true, &c1); }
			{ Rig r(0, 1); RunFrames(r, 12, 7, true, &c7); }
			{ Rig r(0, 1); RunFrames(r, 12, 1000, true, &c1000); }
			for (size_t s = 0; s < 12; ++s) {
				Assert::IsTrue(c1.a[s] == c1000.a[s] && c7.a[s] == c1000.a[s], L"A identical");
				Assert::IsTrue(c1.b[s] == c1000.b[s] && c7.b[s] == c1000.b[s], L"B identical");
				Assert::IsTrue(c1000.b[s] == Ref(kB, s), L"B = reference");
				if (s >= 1)
					Assert::IsTrue(c1.rb[s] == SealReason::Accepted && c7.rb[s] == SealReason::Accepted, L"accepted at any budget");
			}
		}

		// 4. Forced late: the seal takes a running job (cancel) and a completing job (Publish is Late).
		TEST_METHOD(ForcedLateNeverOverwrites)
		{
			Rig rig(0, 1);
			RunFrames(rig, 4, 1000, true, nullptr);   // sealed 0..3; the lane has published frame 4
			// Frame 5: B begins and runs one chunk of many, then the seal arrives.
			rig.Arrive(5);
			rig.Pump(1000, true, false, false);
			Assert::IsTrue(rig.runner.Step(kB, 1) == StepOutcome::Ran, L"B(4) must be done: first Step starts B(5)");
			rig.Seal(4);
			rig.Release(4);
			Assert::IsTrue(rig.Set(kB).StateOf(5) == SlotState::Writing, L"B(5) is being written");
			const uint64_t lateBefore = rig.Set(kB).Counters().late.load();
			const std::vector<ProductFrameResult>& r5 = rig.Seal(5);
			const ProductFrameResult& b5 = rig.Res(r5, kB);
			Assert::IsTrue(b5.seal.reason == SealReason::LaneLate && b5.liveFilled, L"seal while Writing: lane-late, live fill");
			Assert::IsTrue(rig.Bytes(kB, 5) == Ref(kB, 5), L"live bytes = reference");
			Assert::IsTrue(rig.Set(kB).Counters().late.load() == lateBefore + 1, L"flagged LATE to the regulator");
			rig.Release(5);
			Assert::IsTrue(rig.runner.Step(kB, 1) == StepOutcome::Abandoned, L"a running job the seal took is cancelled");
			Assert::IsTrue(rig.fb.cancels == 1, L"producer told to stop");
			Assert::IsTrue(rig.runner.Counters(kB).cancelled.load() == 1 && rig.runner.Counters(kB).abandoned.load() >= 1, L"counted");

			// Frame 6: the job completes its last chunk, then the seal, then Publish.
			rig.Arrive(6);
			rig.Pump(1000, true, false, false);
			int guard = 0;
			rig.fb.finished = false;
			while (!rig.fb.finished && guard++ < 1000)
				rig.runner.Step(kB, 1);
			Assert::IsTrue(rig.fb.finished, L"B(6) finished its chunks");
			const std::vector<ProductFrameResult>& r6 = rig.Seal(6);
			Assert::IsTrue(rig.Res(r6, kB).seal.reason == SealReason::LaneLate && rig.Res(r6, kB).liveFilled, L"late at the seal");
			const uint64_t before = rig.Set(kB).Counters().lateDiscards.load();
			Assert::IsTrue(rig.runner.Step(kB, 1) == StepOutcome::Late, L"Publish after the seal returns Late");
			Assert::IsTrue(rig.Set(kB).Counters().lateDiscards.load() == before + 1, L"late result discarded");
			Assert::IsTrue(rig.runner.Counters(kB).latePublishes.load() == 1, L"counted");
			Assert::IsTrue(rig.Set(kB).Read(6).kind == ReadInfo::Live, L"the live result stays current");
			Assert::IsTrue(rig.Bytes(kB, 6) == Ref(kB, 6), L"never overwritten");
			Assert::IsTrue(rig.runner.Step(kB, 1) == StepOutcome::WaitingSource, L"nextSeq moved on");
			Assert::IsTrue(rig.Set(kB).Counters().orderErrors.load() == 0 && rig.Set(kB).Counters().overruns.load() == 0, L"no protocol errors");
		}

		// 5. Render-ahead R = 2, L = 1: on time with a deep queue; a shallow queue is visible and live-filled.
		TEST_METHOD(RenderAheadLeadAndShallowQueue)
		{
			constexpr uint8_t R = 2, L = 1;
			{
				Rig rig(R, L);
				Assert::IsTrue(rig.graph.Find(kA)->ring == 1 + 1 + R + L + 1, L"A ring sized for R and L");
				Assert::IsTrue(rig.graph.Find(kB)->ring == 0 + 1 + R + L + 1, L"B ring sized for R and L");
				rig.Arrive(3);
				rig.Pump(1000);
				rig.Seal(0);
				rig.Seal(1);
				for (uint64_t p = 0; p < 26; ++p) {
					LeadInput li;
					li.presentSeq = p;
					li.newestQueuedSeq = p + 3;
					li.renderAhead = R;
					li.laneLead = L;
					Assert::IsTrue(CheckLead(li) == LeadState::Ok, L"deep queue: lane on time");
					rig.Arrive(p + 3);
					rig.Pump(1000);
					const uint64_t sealSeq = RenderStartSeq(p, R);
					Assert::IsTrue(sealSeq == p + 2, L"seal at render start");
					const std::vector<ProductFrameResult>& r = rig.Seal(sealSeq);
					Assert::IsTrue(rig.Res(r, kA).seal.reason == SealReason::Accepted, L"A accepted");
					Assert::IsTrue(rig.Res(r, kB).seal.reason == SealReason::Accepted, L"B accepted");
					Assert::IsTrue(rig.Bytes(kB, sealSeq) == Ref(kB, sealSeq), L"bytes");
					rig.Release(p);
				}
				Assert::IsTrue(rig.table.LiveFills() == 2, L"only B(0), B(1)... never later");
			}
			{
				Rig rig(R, L);
				rig.Arrive(1);
				rig.Pump(1000);
				rig.Seal(0);
				rig.Seal(1);
				for (uint64_t p = 0; p < 26; ++p) {
					LeadInput li;
					li.presentSeq = p;
					li.newestQueuedSeq = p + 1;
					li.renderAhead = R;
					li.laneLead = L;
					Assert::IsTrue(CheckLead(li) == LeadState::Shallow, L"shallow queue");
					rig.Arrive(p + 1);
					rig.Pump(1000);
					const uint64_t sealSeq = RenderStartSeq(p, R);
					const std::vector<ProductFrameResult>& r = rig.Seal(sealSeq);
					Assert::IsTrue(rig.Res(r, kA).liveFilled && rig.Res(r, kB).liveFilled, L"made in the render");
					Assert::IsTrue(rig.Res(r, kA).final == SealOutcome::Sealed && rig.Res(r, kB).final == SealOutcome::Sealed, L"available");
					Assert::IsTrue(rig.Bytes(kB, sealSeq) == Ref(kB, sealSeq), L"bytes");
					rig.Release(p);
				}
				for (ProductId p : { kA, kB }) {
					const SlotCounters& sc = rig.Set(p).Counters();
					Assert::IsTrue(sc.overruns.load() == 0 && sc.orderErrors.load() == 0, L"no Overrun / OrderError");
					Assert::IsTrue(sc.ancestorGone.load() == 0, L"no ancestor gone");
				}
				Assert::IsTrue(rig.table.LiveFills() >= 50, L"live fills counted");
			}
		}

		// 6. A late Indicator event after Begin: stamp mismatch at the seal -> live; later jobs use StampOf.
		TEST_METHOD(LateIndicatorEventAfterBegin)
		{
			Rig rig(0, 1);
			RunFrames(rig, 4, 1000, true, nullptr);   // sealed 0..3; frame 4 published ahead
			rig.Seal(4);
			rig.Release(4);
			rig.Arrive(5);
			rig.Pump(1000, true, false, false);
			Assert::IsTrue(rig.runner.Step(kB, 1) == StepOutcome::Ran, L"B(5) begun");
			const uint32_t contBefore = rig.ind.StampOf(5).v[VpIndicator::kContinuity];
			rig.ind.Advance(VpIndicator::kContinuity, Cause::MissedFrames, 5);
			Assert::IsTrue(rig.ind.StampOf(5).v[VpIndicator::kContinuity] == contBefore + 1u, L"event reaches frame 5");
			int guard = 0;
			StepOutcome o = StepOutcome::Ran;
			while (o != StepOutcome::Published && guard++ < 1000)
				o = rig.runner.Step(kB, 1000);
			Assert::IsTrue(o == StepOutcome::Published, L"B(5) published with the old stamp");
			rig.Arrive(6);
			rig.Pump(1000);
			const std::vector<ProductFrameResult>& r5 = rig.Seal(5);
			Assert::IsTrue(rig.Res(r5, kA).seal.reason == SealReason::Accepted, L"A does not read continuity");
			Assert::IsTrue(rig.Res(r5, kB).seal.reason == SealReason::StampMismatch && rig.Res(r5, kB).liveFilled, L"B: stamp mismatch -> live");
			Assert::IsTrue(rig.Bytes(kB, 5) == Ref(kB, 5), L"live bytes = reference");
			rig.Release(5);
			// B(6) began after the event: it honours StampOf(6) (re-stamped, never a stored copy).
			Assert::IsTrue(rig.Set(kB).StateOf(6) == SlotState::Published, L"B(6) published ahead");
			const std::vector<ProductFrameResult>& r6 = rig.Seal(6);
			Assert::IsTrue(rig.Res(r6, kB).seal.reason == SealReason::Accepted, L"B(6) accepted under the new stamp");
			const ReadInfo r6i = rig.Set(kB).Read(6);
			Assert::IsTrue(r6i.header && r6i.header->honoured.v[VpIndicator::kContinuity] == contBefore + 1u, L"honoured = StampOf(6)");
			Assert::IsTrue(r6i.header->honoured.v[VpIndicator::kContinuity] == rig.ind.StampOf(6).v[VpIndicator::kContinuity], L"same stamp source");
		}

		// 7. Stale claim, skip-before, un-laned products, Begin failure.
		TEST_METHOD(StaleSkipBeforeAndUnlaned)
		{
			{
				Rig rig(0, 1);
				rig.Arrive(2);
				LaneTicket mine;
				Assert::IsTrue(rig.Set(kA).Claim(0, Provenance::AheadLane, ~uint64_t(0), &mine) == ClaimResult::Claimed, L"outside claim");
				Assert::IsTrue(rig.runner.Step(kA, 1000) == StepOutcome::Late, L"stale claim -> skip to N+1");
				Assert::IsTrue(rig.runner.Counters(kA).staleSkips.load() == 1, L"counted");
				Assert::IsTrue(rig.runner.Step(kA, 1000) == StepOutcome::Ran, L"frame 1 runs");
				Assert::IsTrue(rig.runner.Step(kA, 1000) == StepOutcome::Published, L"frame 1 published");
				Assert::IsTrue(rig.Set(kA).StateOf(1) == SlotState::Published, L"state");
				rig.Set(kA).Abandon(mine);
			}
			{
				Rig rig(0, 1);
				rig.Arrive(8);
				rig.runner.SetSkipBefore(2);
				Assert::IsTrue(rig.runner.Step(kA, 1000) == StepOutcome::Ran, L"starts at the skip point");
				Assert::IsTrue(rig.Set(kA).StateOf(2) == SlotState::Writing, L"frame 2 claimed");
				Assert::IsTrue(rig.Set(kA).StateOf(0) == SlotState::Empty && rig.Set(kA).StateOf(1) == SlotState::Empty, L"0 and 1 never started");
				Assert::IsTrue(rig.runner.Step(kA, 1000) == StepOutcome::Published, L"published");
			}
			{
				Rig rig(0, 1);
				rig.Arrive(4);
				Assert::IsTrue(rig.runner.Laned(kA), L"laned by default");
				Assert::IsTrue(rig.runner.Step(kA, 1) == StepOutcome::Ran, L"A(0) running");
				rig.runner.SetLaned(kA, false);
				Assert::IsTrue(!rig.runner.Laned(kA), L"switched off");
				Assert::IsTrue(rig.runner.Step(kA, 1) == StepOutcome::Abandoned, L"in-flight job abandoned at the next step");
				Assert::IsTrue(rig.fa.cancels == 1, L"cancelled");
				Assert::IsTrue(rig.runner.Step(kA, 1) == StepOutcome::NotLaned, L"NotLaned");
				Assert::IsTrue(rig.runner.Step(kA, 1) == StepOutcome::NotLaned, L"stays");
				rig.runner.SetLaned(kA, true);
				Assert::IsTrue(rig.runner.Step(kA, 1000) == StepOutcome::Ran, L"back on");
			}
			{
				Rig rig(0, 1);
				rig.Arrive(4);
				rig.fa.failBegin = true;
				Assert::IsTrue(rig.runner.Step(kA, 1000) == StepOutcome::Abandoned, L"Begin failure abandons the job");
				Assert::IsTrue(rig.runner.Counters(kA).beginFailures.load() == 1, L"counted");
				Assert::IsTrue(rig.Set(kA).StateOf(0) == SlotState::Abandoned, L"buffer returned");
				Assert::IsTrue(rig.runner.Step(kO, 1) == StepOutcome::NoProducer, L"no producer for a product outside the graph");
				rig.dropped[1] = 1;
				Assert::IsTrue(rig.runner.Step(kA, 1000) == StepOutcome::Late, L"a dropped source frame is skipped");
				Assert::IsTrue(rig.runner.Counters(kA).skippedDropped.load() == 1, L"counted");
			}
		}

		// 8. OFA fake: Publish waits for the fence; a cancel waits for it before returning the buffer.
		TEST_METHOD(OfaFenceGatesPublishAndAbandon)
		{
			Rig rig(0, 1, false, true);
			rig.Arrive(1);
			rig.Pump(1000, true, false, false);
			Assert::IsTrue(rig.runner.Step(kO, 1000) == StepOutcome::Ran, L"chunk 1");
			Assert::IsTrue(rig.runner.Step(kO, 1000) == StepOutcome::Ran, L"chunk 2");
			Assert::IsTrue(rig.runner.Step(kO, 1000) == StepOutcome::Ran, L"chunk 3 (done)");
			Assert::IsTrue(rig.runner.Step(kO, 1000) == StepOutcome::WaitingDevice, L"fence not passed");
			Assert::IsTrue(rig.runner.Step(kO, 1000) == StepOutcome::WaitingDevice, L"still waiting");
			Assert::IsTrue(rig.Set(kO).StateOf(0) == SlotState::Writing, L"nothing published (stamp last)");
			rig.fo.completed = 1;
			Assert::IsTrue(rig.runner.Step(kO, 1000) == StepOutcome::Published, L"published after the fence");
			Assert::IsTrue(rig.Set(kO).WordFor(0).prov == Provenance::AheadOfa, L"OFA provenance");
			const std::vector<ProductFrameResult>& r0 = rig.Seal(0);
			Assert::IsTrue(rig.Res(r0, kO).seal.reason == SealReason::Accepted, L"accepted");
			Assert::IsTrue(rig.Bytes(kO, 0) == Ref(kO, 0), L"bytes");
			rig.Release(0);

			// Frame 1: cancel during Running.
			Assert::IsTrue(rig.runner.Step(kO, 1000) == StepOutcome::Ran, L"O(1) chunk 1");
			rig.Seal(1);
			Assert::IsTrue(rig.Set(kO).StateOf(1) != SlotState::Writing, L"the seal took the frame");
			Assert::IsTrue(rig.runner.Step(kO, 1000) == StepOutcome::WaitingDevice, L"cancel: submitted work must land first");
			Assert::IsTrue(rig.fo.cancels == 1, L"producer cancelled once");
			Assert::IsTrue(rig.Set(kO).Counters().abandoned.load() == 0, L"buffer not returned yet");
			Assert::IsTrue(rig.runner.Step(kO, 1000) == StepOutcome::WaitingDevice, L"still waiting");
			Assert::IsTrue(rig.fo.cancels == 1, L"cancelled only once");
			rig.fo.completed = 2;
			Assert::IsTrue(rig.runner.Step(kO, 1000) == StepOutcome::Abandoned, L"abandoned after the fence");
			Assert::IsTrue(rig.Set(kO).Counters().abandoned.load() == 1, L"buffer returned");
			Assert::IsTrue(rig.runner.Counters(kO).cancelled.load() == 1, L"counted");
			Assert::IsTrue(rig.Bytes(kO, 1) == Ref(kO, 1), L"live result kept");
		}

		// 9. Two threads: the lane steps A and B, the render seals, live-fills and releases.
		TEST_METHOD(TwoThreadStress)
		{
			constexpr uint64_t N = 20000;
			Rig rig(0, 1, true, false, size_t(N) + 4);
			std::atomic<bool> stop(false);
			std::thread lane([&]() {
				while (!stop.load(std::memory_order_acquire)) {
					const bool a = Progress(rig.runner.Step(kA, 1000));
					const bool b = Progress(rig.runner.Step(kB, 1000));
					if (!a && !b)
						std::this_thread::yield();
				}
			});
			uint64_t mismatches = 0, accepted = 0;
			for (uint64_t s = 0; s < N; ++s) {
				rig.Arrive(s + 1);
				if ((s & 7u) == 0)
					std::this_thread::yield();
				const std::vector<ProductFrameResult>& r = rig.Seal(s);
				if (rig.Res(r, kA).seal.reason == SealReason::Accepted && rig.Res(r, kB).seal.reason == SealReason::Accepted)
					++accepted;
				if (rig.Res(r, kA).final != SealOutcome::Sealed || rig.Res(r, kB).final != SealOutcome::Sealed)
					++mismatches;
				if (rig.Bytes(kA, s) != Ref(kA, s) || rig.Bytes(kB, s) != Ref(kB, s))
					++mismatches;
				rig.Release(s);
			}
			stop.store(true, std::memory_order_release);
			lane.join();
			Assert::IsTrue(mismatches == 0, L"every sealed frame equals the reference");
			for (ProductId p : { kA, kB }) {
				const SlotCounters& sc = rig.Set(p).Counters();
				Assert::IsTrue(sc.overruns.load() == 0, L"no overrun");
				Assert::IsTrue(sc.orderErrors.load() == 0, L"no order error");
				Assert::IsTrue(sc.ancestorGone.load() == 0, L"no ancestor gone");
				Assert::IsTrue(sc.sealed.load() + sc.live.load() == N, L"one result per frame");
				Assert::IsTrue(rig.runner.Counters(p).published.load() == sc.published.load(), L"published agrees");
				Assert::IsTrue(rig.runner.Counters(p).latePublishes.load() == sc.lateDiscards.load(), L"late agrees");
			}
			Assert::IsTrue(accepted > 0, L"the lane produced something");
		}

		// 10. N1 bounded wait, lead check and seal point.
		TEST_METHOD(DecideWaitTruthTable)
		{
			WaitPolicy p;
			p.deterministicRun = true;
			p.presentSlackUs = 5000;
			p.guardUs = 1000;
			uint32_t more = 77;
			const Variants ni = Variants::NonIdentical;
			Assert::IsTrue(DecideWait(Variants::Identical, true, SlotState::Writing, p, 0, &more) == WaitDecision::SealNow && more == 0, L"identical variants never wait");
			WaitPolicy off = p;
			off.deterministicRun = false;
			Assert::IsTrue(DecideWait(ni, true, SlotState::Writing, off, 0, &more) == WaitDecision::SealNow, L"non-deterministic never waits");
			Assert::IsTrue(DecideWait(ni, false, SlotState::Writing, p, 0, &more) == WaitDecision::SealNow, L"no lane granted");
			Assert::IsTrue(DecideWait(ni, true, SlotState::Published, p, 0, &more) == WaitDecision::SealNow, L"already published");
			Assert::IsTrue(DecideWait(ni, true, SlotState::Sealed, p, 0, &more) == WaitDecision::SealNow, L"already sealed");
			Assert::IsTrue(DecideWait(ni, true, SlotState::Live, p, 0, &more) == WaitDecision::SealNow, L"already live");
			Assert::IsTrue(DecideWait(ni, true, SlotState::Writing, p, 0, &more) == WaitDecision::Wait && more == 500, L"writing: wait");
			Assert::IsTrue(DecideWait(ni, true, SlotState::Empty, p, 0, &more) == WaitDecision::Wait, L"claimable empty: wait");
			Assert::IsTrue(DecideWait(ni, true, SlotState::Released, p, 0, &more) == WaitDecision::Wait, L"claimable released: wait");
			// bound = slack - guard = 4000
			Assert::IsTrue(DecideWait(ni, true, SlotState::Writing, p, 3800, &more) == WaitDecision::Wait && more == 200, L"remaining < 500");
			Assert::IsTrue(DecideWait(ni, true, SlotState::Writing, p, 3999, &more) == WaitDecision::Wait && more == 1, L"one us left");
			Assert::IsTrue(DecideWait(ni, true, SlotState::Writing, p, 4000, &more) == WaitDecision::BoundMissed && more == 0, L"bound reached");
			Assert::IsTrue(DecideWait(ni, true, SlotState::Writing, p, 9000, &more) == WaitDecision::BoundMissed, L"past the bound");
			WaitPolicy tight = p;
			tight.presentSlackUs = 1000;
			Assert::IsTrue(DecideWait(ni, true, SlotState::Writing, tight, 0, &more) == WaitDecision::BoundMissed, L"slack == guard: no wait");
			tight.presentSlackUs = 500;
			Assert::IsTrue(DecideWait(ni, true, SlotState::Writing, tight, 0, nullptr) == WaitDecision::BoundMissed, L"slack < guard: no wait");
			WaitPolicy ex = p;
			ex.explicitTestWait = true;
			ex.testWaitUs = 2000;
			Assert::IsTrue(DecideWait(ni, true, SlotState::Writing, ex, 1900, &more) == WaitDecision::Wait && more == 100, L"explicit wait ignores the slack");
			Assert::IsTrue(DecideWait(ni, true, SlotState::Writing, ex, 2000, &more) == WaitDecision::BoundMissed, L"explicit bound");
			ex.deterministicRun = false;
			Assert::IsTrue(DecideWait(ni, true, SlotState::Writing, ex, 0, &more) == WaitDecision::SealNow, L"explicit wait still needs the deterministic run");

			// Lead check.
			LeadInput li;
			li.presentSeq = 10;
			li.renderAhead = 2;
			li.laneLead = 1;
			li.newestQueuedSeq = 13;
			Assert::IsTrue(CheckLead(li) == LeadState::Ok, L"ahead == L + R");
			li.newestQueuedSeq = 12;
			Assert::IsTrue(CheckLead(li) == LeadState::Shallow, L"one short");
			li.newestQueuedSeq = 5;
			Assert::IsTrue(CheckLead(li) == LeadState::Shallow, L"queue behind the present");
			li.newestQueuedSeq = 40;
			li.mode = QueueMode::Competitive;
			Assert::IsTrue(CheckLead(li) == LeadState::BackwardOnArrival, L"competitive");
			li.mode = QueueMode::Games;
			li.laneLead = 0;
			Assert::IsTrue(CheckLead(li) == LeadState::BackwardOnArrival, L"lead 0");
			Assert::IsTrue(std::strlen(LeadText(LeadState::Ok)) > 0 && std::strcmp(LeadText(LeadState::Ok), LeadText(LeadState::Shallow)) != 0, L"texts");
			Assert::IsTrue(RenderStartSeq(7, 0) == 7 && RenderStartSeq(7, 3) == 10, L"render start");
		}
	};
}
