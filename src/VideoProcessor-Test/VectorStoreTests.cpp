#include "pch.h"
#include "CppUnitTest.h"

#include <VectorStore.h>

#include <atomic>
#include <thread>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// docs/VP-nvof-ahead.md (owner 2026-10-16): vectors computed ahead of display,
// stored per source-frame pair; a miss means the matcher runs as today.
namespace VectorStoreTest
{
	TEST_CLASS(VectorStoreTests)
	{
	public:
		TEST_METHOD(AHitReturnsTheStoredPairAMissFallsBack)
		{
			VectorStore<int> store;
			store.SetGeneration(1);
			Assert::IsTrue(store.Put(10, 11, 1, 42));
			VectorStore<int>::Entry e;
			Assert::IsTrue(store.Get(10, 11, e));
			Assert::AreEqual(42, e.payload);
			Assert::IsFalse(store.Get(11, 12, e), L"not computed yet: a miss (the matcher runs)");
			const auto s = store.GetStats();
			Assert::AreEqual(1ull, s.hits);
			Assert::AreEqual(1ull, s.misses);
		}

		TEST_METHOD(ADiscontinuityClearsAndAStalePairIsRefused)
		{
			VectorStore<int> store;
			store.SetGeneration(1);
			store.Put(10, 11, 1, 1);
			store.Put(11, 12, 1, 2);
			store.SetGeneration(2);   // seek / resync / format change / drop
			VectorStore<int>::Entry e;
			Assert::IsFalse(store.Get(10, 11, e), L"pairs from before the discontinuity are gone");
			Assert::AreEqual(size_t{ 0 }, store.Size());
			// A pair finished after the discontinuity but computed before it.
			Assert::IsFalse(store.Put(12, 13, 1, 3), L"computed in the old generation: refused");
			Assert::IsTrue(store.Put(20, 21, 2, 4));
			Assert::AreEqual(1ull, store.GetStats().stale);
			Assert::AreEqual(1ull, store.GetStats().cleared);
		}

		TEST_METHOD(PruneAndCapacityKeepItSmall)
		{
			VectorStore<int> store(4);
			store.SetGeneration(7);
			for (uint64_t f = 1; f <= 6; ++f) store.Put(f, f + 1, 7, static_cast<int>(f));
			Assert::AreEqual(size_t{ 4 }, store.Size(), L"bounded");
			Assert::AreEqual(2ull, store.GetStats().evicted, L"oldest first out");
			Assert::IsFalse(store.Has(1, 2));
			Assert::IsTrue(store.Has(6, 7));
			store.Prune(5);   // rendering frame 5: pairs ending before it are not needed
			Assert::IsFalse(store.Has(3, 4));
			Assert::IsTrue(store.Has(4, 5), L"(prev, cur) kept for the causal consumers");
			Assert::IsTrue(store.Has(5, 6));
		}

		TEST_METHOD(PlanAsksForTheMissingPairsAheadNearestFirst)
		{
			VectorStore<int> store;
			store.SetGeneration(1);
			store.Put(100, 101, 1, 0);
			// Queued after the current frame 100: 101..106 (a cadence repeat is not queued twice).
			const std::vector<uint64_t> queued = { 101, 102, 103, 104, 105, 106 };
			auto plan = store.Plan(100, queued, 4, false);
			Assert::AreEqual(size_t{ 3 }, plan.size(), L"4 ahead, (100,101) already stored");
			Assert::IsTrue(plan[0] == std::make_pair(101ull, 102ull) && plan[2] == std::make_pair(103ull, 104ull));
			// With the two-frame span (detail accumulation v2).
			plan = store.Plan(100, queued, 4, true);
			Assert::AreEqual(size_t{ 6 }, plan.size());
			Assert::IsTrue(plan[1] == std::make_pair(100ull, 102ull));
			// A shallow queue: only what is there (no waiting, no latency added).
			plan = store.Plan(100, { 101 }, 4, true);
			Assert::AreEqual(size_t{ 0 }, plan.size());
		}

		TEST_METHOD(ProducerAndConsumerThreads)
		{
			// One producer storing ahead while the consumer renders and prunes.
			VectorStore<int> store(16);
			store.SetGeneration(1);
			std::atomic<uint64_t> produced{ 0 };
			std::thread producer([&] {
				for (uint64_t f = 1; f <= 2000; ++f) { store.Put(f, f + 1, 1, static_cast<int>(f)); produced = f; }
			});
			unsigned long long hits = 0;
			for (uint64_t f = 1; f <= 2000; ++f)
			{
				VectorStore<int>::Entry e;
				if (store.Get(f, f + 1, e)) { ++hits; Assert::AreEqual(static_cast<int>(f), e.payload); }
				store.Prune(f);
			}
			producer.join();
			Assert::IsTrue(store.Size() <= 16);
			const auto s = store.GetStats();
			Assert::AreEqual(2000ull, s.hits + s.misses, L"every request answered: a hit or a miss");
		}
	};
}

#include <MotionAhead.h>

namespace MotionAheadTest
{
	TEST_CLASS(MotionAheadTests)
	{
	public:
		TEST_METHOD(DepthFollowsTheSettingTheQueueAndLowLatency)
		{
			using namespace MotionAhead;
			// Default: 4 ahead when 4 or more are queued.
			Decision d = Decide(kDefaultDepth, false, true, 6);
			Assert::AreEqual(4, d.depth);
			Assert::IsTrue(d.reason == Reason::Active);
			// A shallower queue: what is there, never waiting.
			d = Decide(4, false, true, 2);
			Assert::AreEqual(2, d.depth);
			Assert::IsTrue(d.reason == Reason::ShallowQueue);
			d = Decide(4, false, true, 0);
			Assert::AreEqual(0, d.depth);
			Assert::IsTrue(d.reason == Reason::NoFramesAhead);
			// Low latency disables; 0 is off; no engine = the matcher.
			Assert::IsTrue(Decide(4, true, true, 6).reason == Reason::LowLatency);
			Assert::AreEqual(0, Decide(4, true, true, 6).depth);
			Assert::IsTrue(Decide(0, false, true, 6).reason == Reason::Off);
			Assert::IsTrue(Decide(4, false, false, 6).reason == Reason::NoEngine);
			// Clamped to 0..4.
			Assert::AreEqual(4, Decide(9, false, true, 9).depth);
			Assert::AreEqual(0, Decide(-3, false, true, 9).depth);
			// The queue that holds the default depth: the current frame plus 4.
			Assert::AreEqual(size_t{ 5 }, QueueFramesFor(kDefaultDepth));
		}
	};
}

namespace MotionAheadSwitchTest
{
	TEST_CLASS(MotionAheadSwitchTests)
	{
	public:
		TEST_METHOD(OpticalFlowOffMeansTheMatcherOnly)
		{
			using namespace MotionAhead;
			// The engine is the default; optical_flow off disables it whatever else is set.
			Assert::IsTrue(Decide(4, false, true, 6).reason == Reason::Active, L"default on");
			const Decision off = Decide(4, false, true, 6, false);
			Assert::IsTrue(off.reason == Reason::Disabled);
			Assert::AreEqual(0, off.depth);
		}
	};
}

#include <vprenderer/MotionAheadProducer.h>
#include <memory>

namespace MotionAheadLumaTest
{
	TEST_CLASS(MotionAheadLumaTests)
	{
	public:
		TEST_METHOD(HalfLumaIsTheSameForP010V210AndUyvy)
		{
			// Owner 2026-10-16 (compare 4:4:4 / 4:2:2 / 4:2:0): the producer's luma, read
			// directly per format, is the same picture for the same codes.
			const int W = 96, H = 20;   // a multiple of 6 (v210 groups) and of 2
			auto code = [](int x, int y) { return static_cast<uint16_t>(64 + ((x * 37 + y * 101 + x * y * 7) % 877)); };
			// P010: the luma plane (16-bit, code << 6) and a chroma plane.
			auto p010 = std::make_shared<std::vector<uint16_t>>(static_cast<size_t>(W) * H * 3 / 2, static_cast<uint16_t>(512 << 6));
			for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) (*p010)[static_cast<size_t>(y) * W + x] = static_cast<uint16_t>(code(x, y) << 6);
			AnalysisLumaSource a; a.data = reinterpret_cast<const uint8_t*>(p010->data()); a.dataBytes = p010->size() * 2;
			a.width = W; a.height = H; a.rowBytes = static_cast<size_t>(W) * 2; a.chromaRowBytes = static_cast<size_t>(W) * 2;
			a.format = AnalysisLumaFormat::P010; a.backingOwner = p010;
			// v210: 6 pixels per 16 bytes (Cb Y Cr | Y Cb Y | Cr Y Cb | Y Cr Y), rows of whole groups.
			const size_t v210Row = static_cast<size_t>(W / 6) * 16;
			auto v = std::make_shared<std::vector<uint32_t>>(v210Row / 4 * H, 0u);
			for (int y = 0; y < H; ++y)
				for (int g = 0; g < W / 6; ++g)
				{
					uint32_t* word = &(*v)[(static_cast<size_t>(y) * v210Row) / 4 + static_cast<size_t>(g) * 4];
					auto Y = [&](int k) { return static_cast<uint32_t>(code(g * 6 + k, y)); };
					word[0] = 512u | (Y(0) << 10) | (512u << 20);
					word[1] = Y(1) | (512u << 10) | (Y(2) << 20);
					word[2] = 512u | (Y(3) << 10) | (512u << 20);
					word[3] = Y(4) | (512u << 10) | (Y(5) << 20);
				}
			AnalysisLumaSource b; b.data = reinterpret_cast<const uint8_t*>(v->data()); b.dataBytes = v->size() * 4;
			b.width = W; b.height = H; b.rowBytes = v210Row; b.format = AnalysisLumaFormat::NativeYuv422;
			b.encoding = VideoFrameEncoding::V210; b.backingOwner = v;
			// UYVY: 8-bit (code / 4).
			auto u = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(W) * 2 * H, static_cast<uint8_t>(128));
			for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) (*u)[static_cast<size_t>(y) * W * 2 + static_cast<size_t>(x / 2) * 4 + ((x & 1) ? 3 : 1)] = static_cast<uint8_t>(code(x, y) >> 2);
			AnalysisLumaSource c; c.data = u->data(); c.dataBytes = u->size(); c.width = W; c.height = H;
			c.rowBytes = static_cast<size_t>(W) * 2; c.format = AnalysisLumaFormat::NativeYuv422; c.encoding = VideoFrameEncoding::UYVY; c.backingOwner = u;
			std::vector<uint8_t> la, lb, lc;
			int aw = 0, ah = 0, bw = 0, bh = 0, cw = 0, ch = 0;
			Assert::IsTrue(MotionAhead::HalfLuma8(a, la, aw, ah), L"P010");
			Assert::IsTrue(MotionAhead::HalfLuma8(b, lb, bw, bh), L"v210 (is the source valid?)");
			Assert::IsTrue(MotionAhead::HalfLuma8(c, lc, cw, ch), L"UYVY");
			Assert::IsTrue(aw == bw && ah == bh && aw == cw && ah == ch && aw == W / 2 && ah == H / 2);
			int maxV = 0, maxU = 0;
			for (size_t i = 0; i < la.size(); ++i)
			{
				maxV = (std::max)(maxV, std::abs(static_cast<int>(la[i]) - static_cast<int>(lb[i])));
				maxU = (std::max)(maxU, std::abs(static_cast<int>(la[i]) - static_cast<int>(lc[i])));
			}
			Assert::AreEqual(0, maxV, L"v210 = P010 exactly (same 10-bit codes)");
			Assert::IsTrue(maxU <= 1, L"UYVY within one 8-bit step (8-bit codes)");
		}
	};
}

namespace MotionAheadRgbLumaTest
{
	TEST_CLASS(MotionAheadRgbLumaTests)
	{
	public:
		TEST_METHOD(HalfLumaIsTheSameForR12BR12LAndR210)
		{
			// 4:4:4 capture (owner 2026-10-16): the RGB packings decoded directly agree.
			const int W = 64, H = 16;   // a multiple of 8 (R12B blocks)
			auto rgb12 = [](int x, int y, int c) { return static_cast<unsigned>((x * 53 + y * 97 + c * 301 + x * y * 5) % 4096); };
			// R12B: per 8 pixels nine 32-bit words; the bit layout from AnalysisLumaSource's reader.
			auto r12b = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(W / 8) * 36 * H, 0);
			for (int y = 0; y < H; ++y)
				for (int g = 0; g < W / 8; ++g)
				{
					uint8_t* bk = &(*r12b)[(static_cast<size_t>(y) * (W / 8) + g) * 36];
					auto set = [bk](int word, int index, unsigned v) { bk[word * 4 + index] = static_cast<uint8_t>(v); };
					auto orLo = [bk](int word, int index, unsigned v) { bk[word * 4 + index] = static_cast<uint8_t>((bk[word * 4 + index] & 0xf0) | (v & 0x0f)); };
					auto orHi = [bk](int word, int index, unsigned v) { bk[word * 4 + index] = static_cast<uint8_t>((bk[word * 4 + index] & 0x0f) | ((v & 0x0f) << 4)); };
					unsigned c[8][3];
					for (int k = 0; k < 8; ++k) for (int ch = 0; ch < 3; ++ch) c[k][ch] = rgb12(g * 8 + k, y, ch);
					// Pixel k: (low byte position, high nibble) pairs mirroring ReadR12BPixel.
					set(0, 3, c[0][0] & 0xff); orLo(0, 2, c[0][0] >> 8); orHi(0, 2, c[0][1] & 0xf); set(0, 1, c[0][1] >> 4); set(0, 0, c[0][2] & 0xff); orLo(1, 3, c[0][2] >> 8);
					orHi(1, 3, c[1][0] & 0xf); set(1, 2, c[1][0] >> 4); set(1, 1, c[1][1] & 0xff); orLo(1, 0, c[1][1] >> 8); orHi(1, 0, c[1][2] & 0xf); set(2, 3, c[1][2] >> 4);
					set(2, 2, c[2][0] & 0xff); orLo(2, 1, c[2][0] >> 8); orHi(2, 1, c[2][1] & 0xf); set(2, 0, c[2][1] >> 4); set(3, 3, c[2][2] & 0xff); orLo(3, 2, c[2][2] >> 8);
					orHi(3, 2, c[3][0] & 0xf); set(3, 1, c[3][0] >> 4); set(3, 0, c[3][1] & 0xff); orLo(4, 3, c[3][1] >> 8); orHi(4, 3, c[3][2] & 0xf); set(4, 2, c[3][2] >> 4);
					set(4, 1, c[4][0] & 0xff); orLo(4, 0, c[4][0] >> 8); orHi(4, 0, c[4][1] & 0xf); set(5, 3, c[4][1] >> 4); set(5, 2, c[4][2] & 0xff); orLo(5, 1, c[4][2] >> 8);
					orHi(5, 1, c[5][0] & 0xf); set(5, 0, c[5][0] >> 4); set(6, 3, c[5][1] & 0xff); orLo(6, 2, c[5][1] >> 8); orHi(6, 2, c[5][2] & 0xf); set(6, 1, c[5][2] >> 4);
					set(6, 0, c[6][0] & 0xff); orLo(7, 3, c[6][0] >> 8); orHi(7, 3, c[6][1] & 0xf); set(7, 2, c[6][1] >> 4); set(7, 1, c[6][2] & 0xff); orLo(7, 0, c[6][2] >> 8);
					orHi(7, 0, c[7][0] & 0xf); set(8, 3, c[7][0] >> 4); set(8, 2, c[7][1] & 0xff); orLo(8, 1, c[7][1] >> 8); orHi(8, 1, c[7][2] & 0xf); set(8, 0, c[7][2] >> 4);
				}
			// R12L: per 2 pixels nine bytes.
			auto r12l = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(W / 2) * 9 * H, 0);
			for (int y = 0; y < H; ++y)
				for (int x = 0; x < W; x += 2)
				{
					uint8_t* pr = &(*r12l)[static_cast<size_t>(y) * (W / 2) * 9 + static_cast<size_t>(x / 2) * 9];
					const unsigned r0 = rgb12(x, y, 0), g0 = rgb12(x, y, 1), b0 = rgb12(x, y, 2);
					const unsigned r1 = rgb12(x + 1, y, 0), g1 = rgb12(x + 1, y, 1), b1 = rgb12(x + 1, y, 2);
					pr[0] = static_cast<uint8_t>(r0); pr[1] = static_cast<uint8_t>((r0 >> 8) | ((g0 & 0xf) << 4)); pr[2] = static_cast<uint8_t>(g0 >> 4);
					pr[3] = static_cast<uint8_t>(b0); pr[4] = static_cast<uint8_t>((b0 >> 8) | ((r1 & 0xf) << 4)); pr[5] = static_cast<uint8_t>(r1 >> 4);
					pr[6] = static_cast<uint8_t>(g1); pr[7] = static_cast<uint8_t>((g1 >> 8) | ((b1 & 0xf) << 4)); pr[8] = static_cast<uint8_t>(b1 >> 4);
				}
			// r210: big-endian words, 10-bit RGB (the 12-bit values / 4).
			auto r210 = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(W) * 4 * H, 0);
			for (int y = 0; y < H; ++y)
				for (int x = 0; x < W; ++x)
				{
					const uint32_t word = ((rgb12(x, y, 0) >> 2) << 20) | ((rgb12(x, y, 1) >> 2) << 10) | (rgb12(x, y, 2) >> 2);
					uint8_t* px = &(*r210)[(static_cast<size_t>(y) * W + x) * 4];
					px[0] = static_cast<uint8_t>(word >> 24); px[1] = static_cast<uint8_t>(word >> 16); px[2] = static_cast<uint8_t>(word >> 8); px[3] = static_cast<uint8_t>(word);
				}
			auto source = [&](const std::shared_ptr<std::vector<uint8_t>>& bytes, size_t rowBytes, VideoFrameEncoding encoding) {
				AnalysisLumaSource s;
				s.data = bytes->data(); s.dataBytes = bytes->size(); s.width = W; s.height = H; s.rowBytes = rowBytes;
				s.format = AnalysisLumaFormat::NativeRgb; s.encoding = encoding; s.backingOwner = bytes;
				return s;
			};
			std::vector<uint8_t> a, b, c;
			int w1 = 0, h1 = 0, w2 = 0, h2 = 0, w3 = 0, h3 = 0;
			Assert::IsTrue(MotionAhead::HalfLuma8(source(r12b, static_cast<size_t>(W / 8) * 36, VideoFrameEncoding::R12B), a, w1, h1), L"R12B");
			Assert::IsTrue(MotionAhead::HalfLuma8(source(r12l, static_cast<size_t>(W / 2) * 9, VideoFrameEncoding::R12L), b, w2, h2), L"R12L");
			Assert::IsTrue(MotionAhead::HalfLuma8(source(r210, static_cast<size_t>(W) * 4, VideoFrameEncoding::R210), c, w3, h3), L"r210");
			int maxAB = 0, maxAC = 0, maxRef = 0;
			for (int y = 0; y < h1; ++y)
				for (int x = 0; x < w1; ++x)
				{
					const size_t i = static_cast<size_t>(y) * w1 + x;
					maxAB = (std::max)(maxAB, std::abs(a[i] - b[i]));
					maxAC = (std::max)(maxAC, std::abs(a[i] - c[i]));
					// The reference: (R + 2G + B) / 4 of the 2 x 2 pixels, in 8-bit.
					double ref = 0;
					for (int k = 0; k < 4; ++k)
					{
						const int px = 2 * x + (k & 1), py = 2 * y + (k >> 1);
						ref += (rgb12(px, py, 0) + 2.0 * rgb12(px, py, 1) + rgb12(px, py, 2)) / 4.0;
					}
					maxRef = (std::max)(maxRef, std::abs(static_cast<int>(a[i]) - static_cast<int>(std::lround(ref / 4.0 / 16.0))));
				}
			Assert::AreEqual(0, maxAB, L"R12B = R12L");
			Assert::IsTrue(maxAC <= 1, L"r210 (10-bit) within one 8-bit step");
			Assert::IsTrue(maxRef <= 1, L"the luma is (R + 2G + B) / 4 of the 2 x 2 pixels");
		}
	};
}
