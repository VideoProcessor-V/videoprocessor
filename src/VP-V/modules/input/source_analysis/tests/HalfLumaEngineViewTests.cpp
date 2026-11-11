// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only

// C9 commit A (doc 105 phase 4a, doc 93 C9P s2a/s2b): the optical-flow engine's 8-bit views of P1 and the P1 reader.
// (1) the engine-half law Engine8FromP1(FinishP1(64 S)) == FinishEngine8(S) for every S (exhaustive over 0..4092);
// (2) the engine quarter (Engine8Quarter) equals the producer's own quarter loop and is NOT derived from the P1 quarter;
// (3) per format, the reader (lane-published under the slot seqlock, live fill, lane off) against a frozen copy of the
//     producer's HalfLuma8 (MotionAheadProducer.cpp at 8630155c6): identical for the formats whose codes are the shared
//     decode, a pinned named exception for RGB structure luma and raw full-range 4:2:x codes (C9 commit B moves them);
// (4) the seqlock: a slot recycled under the copy is never returned (deterministic and threaded). CPU only.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

#include <VP-V/core/AnalysisLane.h>
#include <VP-V/core/Indicator.h>
#include <VP-V/core/ProductGraph.h>
#include <VP-V/core/ProductSlots.h>
#include <VP-V/core/Products.h>
#include <VP-V/modules/input/source_analysis/VpFrameAnalysis.h>
#include <VP-V/modules/input/source_analysis/lane/LaneCpuProducts.h>
#include <VP-V/modules/input/source_analysis/lane/shared/HalfLuma.h>
#include <VP-V/modules/motion/estimation/MotionAheadProducer.h>
#include <VP-V/tests/SyntheticAnalysisFrames.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace HalfLumaEngineViewTestsNs
{
	using namespace SyntheticAnalysisFrames;
	namespace HL = VpUnit::HalfLuma;
	using namespace VpSkeleton;

	// ---- Frozen copies (8630155c6). Never edited: they pin the producer's behaviour before commit A. ----
	uint32_t FrozenBig32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]); }
	uint32_t FrozenLittle32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
	uint16_t FrozenLumaOf(unsigned r, unsigned g, unsigned b) { return static_cast<uint16_t>((r + 2 * g + b + 2) >> 2); }

	// MotionAheadProducer.cpp HalfLuma8 (single-threaded; the row split never changed the values).
	bool FrozenHalfLuma8(const AnalysisLumaSource& source, std::vector<uint8_t>& out, int& halfW, int& halfH)
	{
		if (!source.IsValid()) return false;
		const int w = source.width, h = source.height;
		halfW = (w + 1) / 2;
		halfH = (h + 1) / 2;
		out.resize(static_cast<size_t>(halfW) * halfH);
		const AnalysisLumaFormat format = source.format;
		const VideoFrameEncoding encoding = source.encoding;
		const bool planar16 = format == AnalysisLumaFormat::P010 || format == AnalysisLumaFormat::P210;
		const bool yuv422 = format == AnalysisLumaFormat::NativeYuv422;
		const bool rgb = format == AnalysisLumaFormat::NativeRgb;
		const bool rgbOwnDecode = rgb && (encoding == VideoFrameEncoding::R12B || encoding == VideoFrameEncoding::R12L ||
			encoding == VideoFrameEncoding::R210 || encoding == VideoFrameEncoding::R10b || encoding == VideoFrameEncoding::R10l ||
			encoding == VideoFrameEncoding::BGRA_8BIT || encoding == VideoFrameEncoding::ARGB_8BIT);
		const bool sharedSameCodes = !planar16 && !rgbOwnDecode && !(yuv422 && source.fullRangeYuv);
		auto decodeRow = [&](int y, uint16_t* codes) {
			if (sharedSameCodes)
				if (const uint16_t* shared = source.SharedLumaRow(y))
				{
					std::memcpy(codes, shared, static_cast<size_t>(w) * sizeof(uint16_t));
					return;
				}
			const uint8_t* row = source.data + static_cast<size_t>(y) * source.rowBytes;
			if (planar16)
			{
				const uint16_t* words = reinterpret_cast<const uint16_t*>(row);
				for (int x = 0; x < w; ++x) codes[x] = static_cast<uint16_t>(words[x] >> 6);
				return;
			}
			if (yuv422 && encoding == VideoFrameEncoding::V210)
			{
				for (int x = 0; x < w; x += 6)
				{
					uint32_t word[4];
					std::memcpy(word, row + static_cast<size_t>(x / 6) * 16, 16);
					const uint16_t y6[6] = {
						static_cast<uint16_t>((word[0] >> 10) & 0x3ff), static_cast<uint16_t>(word[1] & 0x3ff),
						static_cast<uint16_t>((word[1] >> 20) & 0x3ff), static_cast<uint16_t>((word[2] >> 10) & 0x3ff),
						static_cast<uint16_t>(word[3] & 0x3ff), static_cast<uint16_t>((word[3] >> 20) & 0x3ff) };
					for (int k = 0; k < 6 && x + k < w; ++k) codes[x + k] = y6[k];
				}
				return;
			}
			if (yuv422)
			{
				for (int x = 0; x < w; ++x) codes[x] = static_cast<uint16_t>(row[static_cast<size_t>(x / 2) * 4 + ((x & 1) ? 3 : 1)] << 2);
				return;
			}
			if (rgb && encoding == VideoFrameEncoding::R12B)
			{
				for (int x = 0; x < w; x += 8)
				{
					const uint8_t* bk = row + static_cast<size_t>(x / 8) * 36;
					auto B = [bk](int word, int index) -> unsigned { return bk[word * 4 + index]; };
					auto L = [&B](int word, int index) -> unsigned { return B(word, index) & 0x0f; };
					auto Hh = [&B](int word, int index) -> unsigned { return B(word, index) >> 4; };
					const unsigned rgb8[8][3] = {
						{ B(0, 3) | (L(0, 2) << 8), Hh(0, 2) | (B(0, 1) << 4), B(0, 0) | (L(1, 3) << 8) },
						{ Hh(1, 3) | (B(1, 2) << 4), B(1, 1) | (L(1, 0) << 8), Hh(1, 0) | (B(2, 3) << 4) },
						{ B(2, 2) | (L(2, 1) << 8), Hh(2, 1) | (B(2, 0) << 4), B(3, 3) | (L(3, 2) << 8) },
						{ Hh(3, 2) | (B(3, 1) << 4), B(3, 0) | (L(4, 3) << 8), Hh(4, 3) | (B(4, 2) << 4) },
						{ B(4, 1) | (L(4, 0) << 8), Hh(4, 0) | (B(5, 3) << 4), B(5, 2) | (L(5, 1) << 8) },
						{ Hh(5, 1) | (B(5, 0) << 4), B(6, 3) | (L(6, 2) << 8), Hh(6, 2) | (B(6, 1) << 4) },
						{ B(6, 0) | (L(7, 3) << 8), Hh(7, 3) | (B(7, 2) << 4), B(7, 1) | (L(7, 0) << 8) },
						{ Hh(7, 0) | (B(8, 3) << 4), B(8, 2) | (L(8, 1) << 8), Hh(8, 1) | (B(8, 0) << 4) } };
					for (int k = 0; k < 8 && x + k < w; ++k)
						codes[x + k] = static_cast<uint16_t>(FrozenLumaOf(rgb8[k][0], rgb8[k][1], rgb8[k][2]) >> 2);
				}
				return;
			}
			if (rgb && encoding == VideoFrameEncoding::R12L)
			{
				for (int x = 0; x < w; x += 2)
				{
					const uint8_t* pr = row + static_cast<size_t>(x / 2) * 9;
					const unsigned r0 = pr[0] | ((pr[1] & 0x0f) << 8), g0 = (pr[1] >> 4) | (pr[2] << 4), b0 = pr[3] | ((pr[4] & 0x0f) << 8);
					const unsigned r1 = (pr[4] >> 4) | (pr[5] << 4), g1 = pr[6] | ((pr[7] & 0x0f) << 8), b1 = (pr[7] >> 4) | (pr[8] << 4);
					codes[x] = static_cast<uint16_t>(FrozenLumaOf(r0, g0, b0) >> 2);
					if (x + 1 < w) codes[x + 1] = static_cast<uint16_t>(FrozenLumaOf(r1, g1, b1) >> 2);
				}
				return;
			}
			if (rgb && (encoding == VideoFrameEncoding::R210 || encoding == VideoFrameEncoding::R10b || encoding == VideoFrameEncoding::R10l))
			{
				const int shift = encoding == VideoFrameEncoding::R210 ? 0 : 2;
				for (int x = 0; x < w; ++x)
				{
					const uint8_t* px = row + static_cast<size_t>(x) * 4;
					const uint32_t word = encoding == VideoFrameEncoding::R10l ? FrozenLittle32(px) : FrozenBig32(px);
					codes[x] = FrozenLumaOf((word >> (20 + shift)) & 0x3ff, (word >> (10 + shift)) & 0x3ff, (word >> shift) & 0x3ff);
				}
				return;
			}
			if (rgb && (encoding == VideoFrameEncoding::BGRA_8BIT || encoding == VideoFrameEncoding::ARGB_8BIT))
			{
				const bool bgra = encoding == VideoFrameEncoding::BGRA_8BIT;
				for (int x = 0; x < w; ++x)
				{
					const uint8_t* px = row + static_cast<size_t>(x) * 4;
					const unsigned r = bgra ? px[2] : px[1], g = bgra ? px[1] : px[2], b = bgra ? px[0] : px[3];
					codes[x] = static_cast<uint16_t>(FrozenLumaOf(r, g, b) << 2);
				}
				return;
			}
			AnalysisLumaSample sample;
			for (int x = 0; x < w; ++x)
				codes[x] = source.Sample(x, y, sample) ? sample.luma : 0;
		};
		std::vector<uint16_t> r0(static_cast<size_t>(w)), r1(static_cast<size_t>(w));
		for (int y = 0; y < halfH; ++y)
		{
			decodeRow(HL::Tap0(y, h), r0.data());
			decodeRow(HL::Tap1(y, h), r1.data());
			uint8_t* dst = &out[static_cast<size_t>(y) * halfW];
			for (int x = 0; x < halfW; ++x)
			{
				const int x0 = HL::Tap0(x, w), x1 = HL::Tap1(x, w);
				dst[x] = HL::FinishEngine8(HL::Box4(r0[x0], r0[x1], r1[x0], r1[x1]));
			}
		}
		return true;
	}

	// MotionAheadProducer.cpp Producer::Process quarter loop (lines 459-469 at 8630155c6).
	std::vector<uint8_t> FrozenQuarter(const std::vector<uint8_t>& lumaBuffer, int halfW, int halfH)
	{
		const int tw = (halfW + 1) / 2, th = (halfH + 1) / 2;
		std::vector<uint8_t> quarterBuffer(static_cast<size_t>(tw) * th);
		for (int y = 0; y < th; ++y)
		{
			const uint8_t* r0 = &lumaBuffer[static_cast<size_t>((std::min)(2 * y, halfH - 1)) * halfW];
			const uint8_t* r1 = &lumaBuffer[static_cast<size_t>((std::min)(2 * y + 1, halfH - 1)) * halfW];
			for (int x = 0; x < tw; ++x)
			{
				const int x0 = (std::min)(2 * x, halfW - 1), x1 = (std::min)(2 * x + 1, halfW - 1);
				quarterBuffer[static_cast<size_t>(y) * tw + x] = static_cast<uint8_t>((r0[x0] + r0[x1] + r1[x0] + r1[x1] + 2) / 4);
			}
		}
		return quarterBuffer;
	}

	std::vector<uint8_t> NewQuarter(const std::vector<uint8_t>& half8, int hw, int hh)
	{
		std::vector<uint8_t> q(static_cast<size_t>((hw + 1) / 2) * static_cast<size_t>((hh + 1) / 2));
		VpLaneProducts::Engine8Quarter(half8.data(), static_cast<uint32_t>(hw), static_cast<uint32_t>(hh), q.data());
		return q;
	}

	// Formats whose producer codes are the frame's shared decode: the reader is byte-identical. The rest (RGB structure luma
	// (R + 2G + B) / 4, raw full-range 4:2:x words) are the named exception commit B handles (doc 105 phase 4b).
	bool IdenticalKind(Kind k)
	{
		return k == Kind::P010 || k == Kind::P210 || k == Kind::DecodedDv422Full || k == Kind::V210 || k == Kind::Uyvy ||
			k == Kind::SdrTunnel || k == Kind::PackedDvR210;
	}

	std::wstring Where(Kind kind, int w, int h, const wchar_t* what)
	{
		return std::wstring(KindName(kind)) + L" " + std::to_wstring(w) + L"x" + std::to_wstring(h) + L": " + what;
	}

	size_t Differing(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
	{
		if (a.size() != b.size()) return (std::max)(a.size(), b.size());
		size_t n = 0;
		for (size_t i = 0; i < a.size(); ++i) n += a[i] != b[i] ? 1u : 0u;
		return n;
	}

	// A frame of kind `kind`, cropped to w x h (the stored frame is kWidth x kHeight; rows keep their stride).
	Frame Cropped(Kind kind, int frame, int w, int h)
	{
		Frame f = Make(kind, frame);
		f.source.width = w;
		f.source.height = h;
		return f;
	}

	// A one-product graph (P1 for a consumer), its table, the P1 byte ring and an indicator: the lane's published P1.
	struct LaneRig
	{
		Graph graph;
		std::unique_ptr<SlotTable> table;
		std::unique_ptr<CpuBuffers> bytes;
		std::unique_ptr<VpIndicator::Indicator> indicator;

		LaneRig(uint32_t w, uint32_t h)
		{
			ConsumerDecl c;
			c.id = 1;
			AddConsume(c, kP1HalfLuma);
			GraphSettings gs;
			gs.dims.width = w;
			gs.dims.height = h;
			gs.renderAhead = 0;
			gs.laneLead = 1;
			gs.queueDepth = 4;
			gs.lookAheadWalk = 0;
			gs.ofaPresent = true;
			graph = BuildGraph(FirstProducts(), { c }, gs);
			table.reset(new SlotTable(graph));
			const Node* n = table->NodeOf(kP1HalfLuma);
			Assert::IsNotNull(n, L"P1 in the graph");
			bytes.reset(new CpuBuffers(*n));
			indicator.reset(new VpIndicator::Indicator(1, 256));
			Assert::AreEqual<size_t>(VpLaneProducts::HalfLumaBytes(Dims{ w, h }), bytes->Bytes(), L"P1 buffer = the P1 layout");
		}

		SlotSet& Set() { return *table->Find(kP1HalfLuma); }
		LaneLease Lease() { return LaneLease::Borrow(table.get(), bytes.get(), indicator.get()); }

		// The lane's P1 of frame seq (the producer law on the tap view), claimed, written and published.
		void Publish(uint64_t seq, const SourceFrameView& view)
		{
			LaneTicket t;
			Assert::IsTrue(Set().Claim(seq, VpAncestry::Provenance::AheadLane, ~uint64_t(0), &t) == ClaimResult::Claimed, L"claim");
			Assert::IsTrue(VpLaneProducts::ComputeHalfLuma(view, bytes->Ring(t.slot), bytes->Bytes()), L"P1 law");
			Set().LaneHeader(t).honoured = indicator->StampOf(seq);
			Assert::IsTrue(Set().Publish(t) == PublishResult::Published, L"publish");
		}
	};

	LaneLeaseSource SourceOf(LaneRig& rig)
	{
		LaneRig* r = &rig;
		return [r]() { return r->Lease(); };
	}

	TEST_CLASS(HalfLumaEngineViewTests)
	{
	public:
		TEST_METHOD(EngineHalfLawExhaustiveOverS)
		{
			std::mt19937 rng(91u);
			uint64_t checked = 0;
			for (uint32_t s = 0; s <= 4092u; ++s)
			{
				// Several decompositions of S into four 10-bit codes: greedy, even split, reversed, and random.
				std::vector<std::array<uint32_t, 4>> parts;
				std::array<uint32_t, 4> g{};
				uint32_t left = s;
				for (int i = 0; i < 4; ++i) { g[i] = (std::min)(left, 1023u); left -= g[i]; }
				parts.push_back(g);
				parts.push_back({ { g[3], g[2], g[1], g[0] } });
				std::array<uint32_t, 4> e{ { s / 4, s / 4, s / 4, s - 3 * (s / 4) } };
				if (e[3] <= 1023u) parts.push_back(e);
				for (int r = 0; r < 3; ++r)
				{
					std::array<uint32_t, 4> p{};
					uint32_t rest = s;
					for (int i = 0; i < 3; ++i)
					{
						const uint32_t lo = rest > 1023u * (3u - i) ? rest - 1023u * (3u - i) : 0u;
						const uint32_t hi = (std::min)(rest, 1023u);
						p[i] = lo + (hi > lo ? rng() % (hi - lo + 1u) : 0u);
						rest -= p[i];
					}
					p[3] = rest;
					parts.push_back(p);
				}
				for (const auto& p : parts)
				{
					Assert::AreEqual<uint32_t>(s, p[0] + p[1] + p[2] + p[3]);
					Assert::IsTrue(p[0] <= 1023u && p[1] <= 1023u && p[2] <= 1023u && p[3] <= 1023u);
					const uint16_t p1 = HL::FinishP1(HL::Box4(p[0] << 6, p[1] << 6, p[2] << 6, p[3] << 6));
					Assert::AreEqual<uint32_t>(16u * s, p1, L"P1 half sample = 16 S exactly");
					if (HL::Engine8FromP1(p1) != HL::FinishEngine8(HL::Box4(p[0], p[1], p[2], p[3])))
						Assert::Fail((L"engine half law broken at S = " + std::to_wstring(s)).c_str());
					++checked;
				}
			}
			Logger::WriteMessage(("engine half law: decompositions checked " + std::to_string(checked)).c_str());
		}

		TEST_METHOD(EngineQuarterIsTheProducersQuarterNotP1Quarter)
		{
			std::mt19937 rng(7u);
			const int dims[][2] = { { 1, 1 }, { 2, 2 }, { 3, 5 }, { 7, 9 }, { 192, 108 }, { 191, 107 }, { 960, 540 }, { 961, 541 } };
			for (const auto& d : dims)
			{
				std::vector<uint8_t> half8(static_cast<size_t>(d[0]) * d[1]);
				for (uint8_t& v : half8) v = static_cast<uint8_t>(rng() & 0xff);
				Assert::IsTrue(FrozenQuarter(half8, d[0], d[1]) == NewQuarter(half8, d[0], d[1]),
					(std::to_wstring(d[0]) + L"x" + std::to_wstring(d[1])).c_str());
			}
			// The counterexample: half sums S = (16, 16, 0, 0). P1 half (256, 256, 0, 0) -> P1 quarter 128 -> top byte 0;
			// the engine half (1, 1, 0, 0) -> engine quarter (1 + 1 + 0 + 0 + 2) / 4 = 1. The engine quarter is its own law.
			const uint16_t h1 = HL::FinishP1(HL::Box4(4u << 6, 4u << 6, 4u << 6, 4u << 6));   // codes (4, 4, 4, 4): S = 16
			Assert::AreEqual<uint32_t>(256u, h1);
			Assert::AreEqual<uint32_t>(1u, HL::Engine8FromP1(h1));
			const uint16_t p1q = HL::FinishP1(HL::Box4(256u, 256u, 0u, 0u));
			Assert::AreEqual<uint32_t>(0u, HL::Engine8FromP1(p1q), L"the P1 quarter's top byte");
			Assert::AreEqual<uint32_t>(1u, HL::QuarterEngine8(HL::Engine8FromP1(256u), HL::Engine8FromP1(256u), 0u, 0u), L"the engine quarter");
		}

		TEST_METHOD(EngineHalfOfP1MatchesTheLawOnRandomViews)
		{
			std::mt19937 rng(23u);
			const uint32_t dims[][2] = { { 1, 1 }, { 2, 1 }, { 1, 2 }, { 3, 3 }, { 17, 9 }, { 384, 216 }, { 383, 215 } };
			for (const auto& d : dims)
			{
				const uint32_t w = d[0], h = d[1];
				std::vector<uint16_t> codes(static_cast<size_t>(w) * h);
				for (uint16_t& c : codes) c = static_cast<uint16_t>(rng() % 1024u);
				SourceFrameView v;
				v.luma = codes.data();
				v.width = w;
				v.height = h;
				v.strideSamples = w;
				v.msbShift = 6;
				std::vector<uint8_t> p1(VpLaneProducts::HalfLumaBytes(Dims{ w, h }));
				Assert::IsTrue(VpLaneProducts::ComputeHalfLuma(v, p1.data(), p1.size()));
				const uint32_t hw = VpLaneProducts::HalfDim(w), hh = VpLaneProducts::HalfDim(h);
				std::vector<uint8_t> got(static_cast<size_t>(hw) * hh), want(got.size());
				VpLaneProducts::Engine8Half(reinterpret_cast<const uint16_t*>(p1.data()), hw, hh, got.data());
				for (uint32_t y = 0; y < hh; ++y)
					for (uint32_t x = 0; x < hw; ++x)
					{
						const uint16_t* r0 = codes.data() + size_t(HL::Tap0(y, h)) * w;
						const uint16_t* r1 = codes.data() + size_t(HL::Tap1(y, h)) * w;
						const uint32_t x0 = HL::Tap0(x, w), x1 = HL::Tap1(x, w);
						want[size_t(y) * hw + x] = HL::FinishEngine8(HL::Box4(r0[x0], r0[x1], r1[x0], r1[x1]));
					}
				Assert::IsTrue(got == want, (std::to_wstring(w) + L"x" + std::to_wstring(h)).c_str());
			}
		}

		TEST_METHOD(FrozenCopyIsTheLiveProducer)
		{
			// The frozen copy is the producer as shipped (any later edit of the live producer is caught here first).
			for (int k = 0; k < static_cast<int>(Kind::Count); ++k)
			{
				const Kind kind = static_cast<Kind>(k);
				for (int c = 0; c < 2; ++c)
				{
					const int w = c ? kWidth - 8 : kWidth, h = c ? kHeight - 2 : kHeight;   // the crop stays a valid source (even, R12 x8); odd half height
					const Frame f = Cropped(kind, 3, w, h);
					std::vector<uint8_t> a, b;
					int wa = 0, ha = 0, wb = 0, hb = 0;
					Assert::IsTrue(FrozenHalfLuma8(f.source, a, wa, ha));
					Assert::IsTrue(MotionAhead::HalfLuma8(f.source, b, wb, hb));
					Assert::IsTrue(wa == wb && ha == hb && a == b, Where(kind, w, h, L"frozen vs live HalfLuma8").c_str());
				}
			}
		}

		TEST_METHOD(ReaderMatchesTheProducerPerFormat)
		{
			std::string summary;
			for (int k = 0; k < static_cast<int>(Kind::Count); ++k)
			{
				const Kind kind = static_cast<Kind>(k);
				for (int c = 0; c < 2; ++c)
				{
					const int w = c ? kWidth - 8 : kWidth, h = c ? kHeight - 2 : kHeight;   // the crop stays a valid source (even, R12 x8); odd half height
					const uint64_t seq = 1;   // inside the ring's first claim window
					const Frame plain = Cropped(kind, 3, w, h);
					std::vector<uint8_t> want;
					int ww = 0, wh = 0;
					Assert::IsTrue(FrozenHalfLuma8(plain.source, want, ww, wh));
					const std::vector<uint8_t> wantQ = FrozenQuarter(want, ww, wh);

					// Lane published: the lane's P1 of the tap view, read under the seqlock.
					LaneRig rig(static_cast<uint32_t>(w), static_cast<uint32_t>(h));
					SourceFrameView view;
					Assert::IsTrue(VpFrameAnalysis::SourceViewOf(plain.source, view), Where(kind, w, h, L"view").c_str());
					rig.Publish(seq, view);
					VpFrameAnalysis::P1Reader lane(PublishedEngine8Of(SourceOf(rig)));
					std::vector<uint8_t> got;
					int gw = 0, gh = 0;
					P1ReadKind how = P1ReadKind::None;
					Assert::IsTrue(lane.ReadEngine8(seq, plain.source, got, gw, gh, how));
					Assert::IsTrue(how == P1ReadKind::LanePublished, Where(kind, w, h, L"lane published").c_str());
					Assert::IsTrue(gw == ww && gh == wh, Where(kind, w, h, L"dims").c_str());
					const std::vector<uint8_t> published = got;

					// Not published (another seq): the live fill, counted.
					Assert::IsTrue(lane.ReadEngine8(seq + 1, plain.source, got, gw, gh, how));
					Assert::IsTrue(how == P1ReadKind::LiveFill, Where(kind, w, h, L"live fill").c_str());
					Assert::IsTrue(got == published, Where(kind, w, h, L"live fill = lane published").c_str());
					Assert::AreEqual<uint64_t>(1u, lane.LanePublished());
					Assert::AreEqual<uint64_t>(1u, lane.FilledByMotion());

					// Lane off; the shared plane attached but pending (decoded here) and already decoded.
					VpFrameAnalysis::P1Reader off;
					Frame pending = Cropped(kind, 3, w, h);
					VpFrameAnalysis::AttachSharedLuma(pending.source);
					std::vector<uint8_t> offPlain, offPending, offShared;
					Assert::IsTrue(off.ReadEngine8(seq, plain.source, offPlain, gw, gh, how) && how == P1ReadKind::LiveFill);
					const bool attached = pending.source.sharedLuma != nullptr;
					Assert::IsTrue(off.ReadEngine8(seq, pending.source, offPending, gw, gh, how) && how == P1ReadKind::LiveFill);
					Assert::IsTrue(off.ReadEngine8(seq, pending.source, offShared, gw, gh, how) && how == P1ReadKind::LiveFill);
					Assert::IsTrue(offPlain == published && offPending == published && offShared == published,
						Where(kind, w, h, attached ? L"lane off (plain / pending / decoded plane)" : L"lane off").c_str());

					const std::vector<uint8_t> gotQ = NewQuarter(published, gw, gh);
					const size_t diffHalf = Differing(published, want), diffQ = Differing(gotQ, wantQ);
					if (IdenticalKind(kind))
					{
						Assert::AreEqual<size_t>(0u, diffHalf, Where(kind, w, h, L"engine half byte-identical").c_str());
						Assert::AreEqual<size_t>(0u, diffQ, Where(kind, w, h, L"engine quarter byte-identical").c_str());
					}
					else
						Assert::IsTrue(diffHalf > 0u, Where(kind, w, h, L"named exception: expected to differ (commit B)").c_str());
					std::wstring name(KindName(kind));
					summary += std::string(name.begin(), name.end()) + " " + std::to_string(w) + "x" + std::to_string(h) +
						(IdenticalKind(kind) ? " identical" : " exception") + " half-diff " + std::to_string(diffHalf) + "/" +
						std::to_string(want.size()) + " quarter-diff " + std::to_string(diffQ) + "/" + std::to_string(wantQ.size()) + "\n";
				}
			}
			Logger::WriteMessage(summary.c_str());
		}

		TEST_METHOD(SeqlockRecycledUnderTheCopyIsNeverReturned)
		{
			const Frame f = Make(Kind::V210, 2);
			LaneRig rig(kWidth, kHeight);
			SourceFrameView view;
			Assert::IsTrue(VpFrameAnalysis::SourceViewOf(f.source, view));
			const uint64_t seq = 1;   // inside the ring's first claim window
			rig.Publish(seq, view);
			const uint64_t ring = rig.Set().Ring();
			// The pass itself recycles the slot: the render discards seq, the lane claims seq + ring and publishes it.
			auto recycle = [&rig, seq, ring, &view](const uint8_t*, size_t) {
				Assert::IsTrue(rig.Set().Discard(seq), L"discard");
				rig.Publish(seq + ring, view);
			};
			Assert::IsTrue(ReadPublishedBytes(rig.Lease(), kP1HalfLuma, seq, rig.bytes->Bytes(), recycle) == PublishedRead::Recycled);
			Assert::IsTrue(ReadPublishedBytes(rig.Lease(), kP1HalfLuma, seq, rig.bytes->Bytes(), [](const uint8_t*, size_t) {}) ==
				PublishedRead::Absent, L"seq is gone");

			// Through the reader: a recycled copy is discarded and the frame is live-filled, byte-identical.
			LaneRig rig2(kWidth, kHeight);
			rig2.Publish(seq, view);
			const uint64_t ring2 = rig2.Set().Ring();
			LaneRig* r2 = &rig2;
			PublishedEngine8Fn racing = [r2, ring2, &view](uint64_t s, uint32_t w, uint32_t h, uint8_t* out) {
				const uint32_t hw = VpLaneProducts::HalfDim(w), hh = VpLaneProducts::HalfDim(h);
				return ReadPublishedBytes(r2->Lease(), kP1HalfLuma, s, VpLaneProducts::HalfLumaBytes(Dims{ w, h }),
					[&](const uint8_t* bytes, size_t) {
						VpLaneProducts::Engine8Half(reinterpret_cast<const uint16_t*>(bytes), hw, hh, out);
						Assert::IsTrue(r2->Set().Discard(s));
						r2->Publish(s + ring2, view);
					});
			};
			VpFrameAnalysis::P1Reader reader(racing);
			std::vector<uint8_t> got, want;
			int gw = 0, gh = 0, ww = 0, wh = 0;
			P1ReadKind how = P1ReadKind::None;
			Assert::IsTrue(reader.ReadEngine8(seq, f.source, got, gw, gh, how));
			Assert::IsTrue(how == P1ReadKind::LiveFill, L"recycled -> live fill");
			Assert::AreEqual<uint64_t>(1u, reader.SeqlockRetries());
			Assert::AreEqual<uint64_t>(1u, reader.FilledByMotion());
			Assert::AreEqual<uint64_t>(0u, reader.LanePublished());
			Assert::IsTrue(FrozenHalfLuma8(f.source, want, ww, wh));
			Assert::IsTrue(got == want && gw == ww && gh == wh, L"the live fill is byte-identical");
		}

		TEST_METHOD(SeqlockThreadedNoTornCopy)
		{
			// A writer cycles claim / fill f(seq) / publish / discard on a two-slot ring while a reader copies the newest
			// published frames. Every Copied read must hold exactly f(seq); a recycled slot is Recycled, never Copied.
			SlotSet set(kP1HalfLuma, 2, 0);
			Node node;
			node.id = kP1HalfLuma;
			node.ring = 2;
			node.livePool = 0;
			node.bufferBytes = 8192;
			CpuBuffers bufs(node);
			std::atomic<uint64_t> latest{ 0 };
			std::atomic<bool> stop{ false };
			auto fill = [](uint64_t s, size_t i) { return static_cast<uint8_t>((s * 131u + i * 7u) & 0xff); };
			std::thread writer([&]() {
				const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
				for (uint64_t s = 0; std::chrono::steady_clock::now() < until; ++s)
				{
					if (s >= 2)
						set.Discard(s - 2);
					LaneTicket t;
					if (set.Claim(s, VpAncestry::Provenance::AheadLane, ~uint64_t(0), &t) != ClaimResult::Claimed)
						continue;
					uint8_t* p = bufs.Ring(t.slot);
					for (size_t i = 0; i < bufs.Bytes(); ++i) p[i] = fill(s, i);
					set.Publish(t);
					latest.store(s + 1, std::memory_order_release);   // 0 = nothing published yet
				}
				stop.store(true, std::memory_order_release);
			});
			uint64_t copied = 0, recycled = 0, absent = 0, torn = 0;
			std::vector<uint8_t> local(bufs.Bytes());
			while (!stop.load(std::memory_order_acquire))
			{
				const uint64_t top = latest.load(std::memory_order_acquire);
				if (top == 0) continue;
				for (uint64_t d = 0; d < 2 && d < top; ++d)
				{
					const uint64_t q = top - 1 - d;   // the newest published frame and the one before it
					const PublishedRead r = ReadPublishedBytes(set, bufs, q, [&local](const uint8_t* b, size_t n) {
						std::memcpy(local.data(), b, n);
					});
					if (r == PublishedRead::Copied)
					{
						++copied;
						for (size_t i = 0; i < local.size(); ++i)
							if (local[i] != fill(q, i)) { ++torn; break; }
					}
					else if (r == PublishedRead::Recycled)
						++recycled;
					else
						++absent;
				}
			}
			writer.join();
			Logger::WriteMessage(("seqlock race: copied " + std::to_string(copied) + " recycled " + std::to_string(recycled) +
				" absent " + std::to_string(absent) + " torn " + std::to_string(torn)).c_str());
			Assert::AreEqual<uint64_t>(0u, torn, L"no torn copy is ever returned");
			Assert::IsTrue(copied > 0u, L"the reader copied published frames");
		}
	};
}
