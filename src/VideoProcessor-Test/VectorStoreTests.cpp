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
			// No motion estimate for this source (60p after film): nothing submitted, whatever is queued.
			Assert::IsTrue(Decide(4, false, true, 6, true, false).reason == Reason::NoConsumer);
			Assert::AreEqual(0, Decide(4, false, true, 6, true, false).depth);
			Assert::IsTrue(Decide(4, true, true, 6, true, false).reason == Reason::LowLatency, L"low latency named first");
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
		TEST_METHOD(FlowGlobalFindsTheCameraAndTheSecondLayer)
		{
			// Planner Q1 (2026-10-16): the whole frame's motion from the engine's field, in D1's
			// format (1/8-size texels = one engine cell = 4 half-size px; S10.5 vectors).
			const int gw = 120, gh = 68;
			const size_t cells = static_cast<size_t>(gw) * gh;
			std::vector<int16_t> v(cells * 2);
			std::vector<uint8_t> cost(cells);
			unsigned seed = 7;
			auto rnd = [&seed]() { seed = seed * 1103515245u + 12345u; return (seed >> 16) & 0x7fff; };
			for (size_t i = 0; i < cells; ++i)
			{
				const unsigned r = rnd() % 100;
				int16_t x, y;
				if (r < 65) { x = 12 * 32; y = -6 * 32; }                       // the camera: (12, -6) half px
				else if (r < 85) { x = -20 * 32; y = 0; }                       // a second layer: (-20, 0)
				else { x = static_cast<int16_t>((static_cast<int>(rnd() % 200) - 100) * 32); y = static_cast<int16_t>((static_cast<int>(rnd() % 200) - 100) * 32); }
				v[i * 2] = x; v[i * 2 + 1] = y;
				cost[i] = r < 85 ? 10 : 200;   // the noise is unsure
			}
			float g[4], second[4];
			MotionAhead::FlowGlobal(v, cost, gw, gh, g, second);
			Assert::AreEqual(3.0f, g[0], 1e-6f, L"x in 1/8-size texels (12 half px / 4)");
			Assert::AreEqual(-1.5f, g[1], 1e-6f, L"y (-6 half px / 4)");
			Assert::IsTrue(g[2] < 0.9f, L"clearly the camera: usable");
			Assert::AreEqual(1.0f, g[3]);
			Assert::AreEqual(1.0f, second[3], L"the second layer found");
			Assert::AreEqual(-5.0f, second[0], 1e-6f);
			Assert::AreEqual(0.0f, second[1], 1e-6f);
			// No agreement (every cell its own vector): ambiguous, not usable.
			for (size_t i = 0; i < cells; ++i)
			{
				v[i * 2] = static_cast<int16_t>((static_cast<int>(rnd() % 300) - 150) * 32);
				v[i * 2 + 1] = static_cast<int16_t>((static_cast<int>(rnd() % 300) - 150) * 32);
				cost[i] = 10;
			}
			MotionAhead::FlowGlobal(v, cost, gw, gh, g, second);
			Assert::IsTrue(g[2] >= 0.9f, L"no agreement: ambiguous (D1's consumers ignore it)");
		}

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

#include <OpticalFlowTrust.h>

// 2026-10-17 (docs/VP-nvof-ahead.md, the engine's extras): the cost and the engine's own
// forward/backward consistency as trust weights, and the engine's whole-frame motion with a
// fallback to the global search when it is not clear. CPU maths only (OpticalFlowTrust.h).
namespace OpticalFlowTrustTest
{
	namespace
	{
		// A field of gw x gh cells with cost `base` (+-1 noise), and `high` in a cell rectangle.
		std::vector<uint8_t> Costs(int gw, int gh, int base, int high, int x0, int y0, int x1, int y1)
		{
			std::vector<uint8_t> c(static_cast<size_t>(gw) * gh);
			unsigned seed = 3;
			for (int y = 0; y < gh; ++y)
				for (int x = 0; x < gw; ++x)
				{
					seed = seed * 1103515245u + 12345u;
					const bool inside = x >= x0 && x < x1 && y >= y0 && y < y1;
					c[static_cast<size_t>(y) * gw + x] = static_cast<uint8_t>(inside ? high : base + static_cast<int>((seed >> 16) % 3) - 1);
				}
			return c;
		}
	}

	TEST_CLASS(OpticalFlowTrustTests)
	{
	public:
		TEST_METHOD(CostWeightsAreNormalisedPerBlockAndGrid)
		{
			// The engine's 4-px grid under the de-judder's 8-px blocks: 2 x 2 cells per block.
			const int gw = 96, gh = 48, bx = 48, by = 24;
			const std::vector<uint8_t> costs = Costs(gw, gh, 10, 250, 40, 16, 56, 32);
			std::vector<float> w;
			Assert::IsTrue(OpticalFlowTrust::CostWeights(costs, gw, gh, 4, 8, bx, by, w));
			Assert::AreEqual(static_cast<size_t>(bx * by), w.size());
			Assert::AreEqual(0.0f, w[static_cast<size_t>(10) * bx + 24], 1e-6f, L"an unsure block: no trust from the engine");
			Assert::AreEqual(1.0f, w[static_cast<size_t>(2) * bx + 2], 1e-6f, L"a usual block: unchanged");
			// The same field with every cost raised (another driver, another preset): the same weights.
			std::vector<uint8_t> raised(costs);
			for (uint8_t& c : raised) c = static_cast<uint8_t>((std::min)(255, c + 4));
			std::vector<float> w2;
			Assert::IsTrue(OpticalFlowTrust::CostWeights(raised, gw, gh, 4, 8, bx, by, w2));
			for (size_t i = 0; i < w.size(); ++i) Assert::AreEqual(w[i], w2[i], 1e-6f, L"relative to the field");
			// Larger blocks (16 px: 4 x 4 cells).
			Assert::IsTrue(OpticalFlowTrust::CostWeights(costs, gw, gh, 4, 16, bx / 2, by / 2, w2));
			Assert::AreEqual(0.0f, w2[static_cast<size_t>(5) * (bx / 2) + 12], 1e-6f);
			Assert::AreEqual(1.0f, w2[0], 1e-6f);
			Assert::AreEqual(2, OpticalFlowTrust::CellsPerBlock(4, 8));
			Assert::AreEqual(1, OpticalFlowTrust::CellsPerBlock(8, 8), L"a coarser grid: one cell per block");
			Assert::AreEqual(1, OpticalFlowTrust::CellsPerBlock(8, 4), L"a block smaller than a cell: its cell");
			// A block half over a moderately unsure region (the mean of its cells): partly trusted.
			const std::vector<uint8_t> edge = Costs(gw, gh, 10, 40, 41, 16, 56, 32);   // block 20 = cells 40 (usual), 41 (unsure)
			Assert::IsTrue(OpticalFlowTrust::CostWeights(edge, gw, gh, 4, 8, bx, by, w2));
			const float half = w2[static_cast<size_t>(10) * bx + 20];
			Assert::IsTrue(half > 0.0f && half < 1.0f, L"half its cells unsure: a partial weight");
			// An edge block with cells missing (odd grid): the cells that exist.
			const std::vector<uint8_t> odd = Costs(95, 47, 10, 250, 94, 0, 95, 47);
			Assert::IsTrue(OpticalFlowTrust::CostWeights(odd, 95, 47, 4, 8, bx, by, w2));
			Assert::AreEqual(0.0f, w2[static_cast<size_t>(3) * bx + 47], 1e-6f, L"the last column's one cell");
		}

		TEST_METHOD(CostStatisticsAndAUniformFieldChangeNothing)
		{
			float median = 0.0f, spread = 0.0f;
			const std::vector<uint8_t> c = { 1, 2, 3, 4, 100 };
			Assert::IsTrue(OpticalFlowTrust::CostStatistics(c, c.size(), median, spread));
			Assert::AreEqual(3.0f, median);
			Assert::AreEqual(4.0f, spread, 1e-6f, L"MAD 1 x 1.4826, below the floor of 4 codes");
			// A uniform field (a still picture, any level): every block fully trusted.
			const std::vector<uint8_t> flat(96 * 48, 200);
			std::vector<float> w;
			Assert::IsTrue(OpticalFlowTrust::CostWeights(flat, 96, 48, 4, 8, 48, 24, w));
			for (float v : w) Assert::AreEqual(1.0f, v);
			Assert::AreEqual(1.0f, OpticalFlowTrust::FallOff(2.0f, 4.0f, 1.0f));
			Assert::AreEqual(0.5f, OpticalFlowTrust::FallOff(2.0f, 4.0f, 3.0f), 1e-6f);
			Assert::AreEqual(0.0f, OpticalFlowTrust::FallOff(2.0f, 4.0f, 9.0f));
		}

		TEST_METHOD(MissingCostsGiveNoWeights)
		{
			// An older driver or a field without costs: no weights at all (the de-judder runs as before).
			std::vector<float> w(3, 0.5f);
			Assert::IsFalse(OpticalFlowTrust::CostWeights(std::vector<uint8_t>(), 96, 48, 4, 8, 48, 24, w));
			Assert::IsTrue(w.empty());
			Assert::IsFalse(OpticalFlowTrust::CostWeights(std::vector<uint8_t>(10, 1), 96, 48, 4, 8, 48, 24, w), L"too few cells");
			Assert::IsFalse(OpticalFlowTrust::Combine(nullptr, nullptr, 48 * 24, w), L"both off: nothing applied");
			Assert::IsTrue(w.empty());
			const std::vector<float> wrongSize(5, 0.0f);
			Assert::IsFalse(OpticalFlowTrust::Combine(&wrongSize, nullptr, 48 * 24, w), L"another block layout: nothing applied");
			MotionAhead::Flow flow;
			flow.halfW = 384; flow.halfH = 192; flow.gridW = 96; flow.gridH = 48;
			flow.forward.assign(96 * 48 * 2, 0); flow.backward.assign(96 * 48 * 2, 0);
			MotionAhead::FlowExtras(flow);
			Assert::AreEqual(48, flow.blocksX);
			Assert::AreEqual(24, flow.blocksY);
			Assert::IsTrue(flow.costWeight.empty(), L"no costs: no cost weights");
			Assert::AreEqual(static_cast<size_t>(96 * 48), flow.fbError.size(), L"the consistency needs only the vectors");
		}

		TEST_METHOD(CombineMultipliesTheEnabledWeights)
		{
			const std::vector<float> cost = { 1.0f, 0.5f, 0.0f, 1.0f }, fb = { 0.5f, 0.5f, 1.0f, 1.0f };
			std::vector<float> w;
			Assert::IsTrue(OpticalFlowTrust::Combine(&cost, &fb, 4, w));
			Assert::AreEqual(0.5f, w[0]);
			Assert::AreEqual(0.25f, w[1]);
			Assert::AreEqual(0.0f, w[2]);
			Assert::AreEqual(1.0f, w[3]);
			Assert::IsTrue(OpticalFlowTrust::Combine(&cost, nullptr, 4, w));
			Assert::AreEqual(0.5f, w[1], L"the consistency off: the cost alone");
			Assert::IsTrue(OpticalFlowTrust::Combine(nullptr, &fb, 4, w));
			Assert::AreEqual(1.0f, w[2], L"the cost off: the consistency alone");
		}

		TEST_METHOD(EngineForwardBackwardConsistency)
		{
			// A pan of (+6, -2) half-size px: the backward field cancels it everywhere but in an
			// occluded patch (cells 22-30 x 8-15 of B), where the backward vectors point elsewhere.
			const int gw = 64, gh = 32, bx = 32, by = 16;
			std::vector<int16_t> fwd(static_cast<size_t>(gw) * gh * 2), bwd(fwd.size());
			for (int y = 0; y < gh; ++y)
				for (int x = 0; x < gw; ++x)
				{
					const size_t i = static_cast<size_t>(y) * gw + x;
					fwd[i * 2] = 6 * 32;
					fwd[i * 2 + 1] = -2 * 32;
					const bool occluded = x >= 22 && x < 31 && y >= 8 && y < 16;
					bwd[i * 2] = static_cast<int16_t>(occluded ? 10 * 32 : -6 * 32);
					bwd[i * 2 + 1] = static_cast<int16_t>(occluded ? 0 : 2 * 32);
				}
			std::vector<float> err, w;
			Assert::IsTrue(OpticalFlowTrust::FbError(fwd, bwd, gw, gh, 4, err));
			Assert::AreEqual(0.0f, err[static_cast<size_t>(2) * gw + 2], 1e-6f, L"consistent");
			// Cell (20, 10): centre (82, 42) lands at (88, 40) = cell (22, 10), occluded.
			const float e = err[static_cast<size_t>(10) * gw + 20];
			Assert::AreEqual(std::sqrt(16.0f * 16.0f + 2.0f * 2.0f), e, 1e-4f, L"|f + b| in half-size px");
			Assert::IsTrue(OpticalFlowTrust::FbWeights(err, gw, gh, 4, 8, bx, by, 1.0f, w));
			Assert::AreEqual(1.0f, w[0], L"a consistent block");
			// Block (10, 5) = cells 20-21 x 10-11: they land in 22-23 x 10, occluded.
			Assert::AreEqual(0.0f, w[static_cast<size_t>(5) * bx + 10], L"an occluded block");
			// Block (9, 5) = cells 18-19: they land in 20-21 (consistent).
			Assert::AreEqual(1.0f, w[static_cast<size_t>(5) * bx + 9]);
			// One of four cells inconsistent: a partial weight.
			std::vector<float> one(err.size(), 0.0f);
			one[static_cast<size_t>(2) * gw + 3] = 10.0f;
			Assert::IsTrue(OpticalFlowTrust::FbWeights(one, gw, gh, 4, 8, bx, by, 1.0f, w));
			Assert::AreEqual(0.75f, w[static_cast<size_t>(1) * bx + 1], 1e-6f);
			// Leaving the picture: the landing cell is clamped (as the matcher's test does).
			fwd[0] = -200 * 32;
			bwd[0] = 200 * 32;
			Assert::IsTrue(OpticalFlowTrust::FbError(fwd, bwd, gw, gh, 4, err));
			Assert::AreEqual(0.0f, err[0], 1e-4f);
			Assert::IsFalse(OpticalFlowTrust::FbError(fwd, std::vector<int16_t>(), gw, gh, 4, err), L"no backward field: none");
			Assert::IsFalse(OpticalFlowTrust::FbWeights(one, gw, gh, 4, 8, bx, by, 0.0f, w), L"no tolerance: none");
		}

		TEST_METHOD(GlobalFitWithOutliersAndTheFallback)
		{
			// The engine's whole-frame motion (D1 format) on synthetic fields: the camera's share
			// of the cells, the rest outliers in any direction.
			const int gw = 120, gh = 68;
			const size_t cells = static_cast<size_t>(gw) * gh;
			std::vector<int16_t> v(cells * 2);
			std::vector<uint8_t> cost(cells, 10);
			auto field = [&](unsigned share, int cx, int cy, unsigned seed) {
				for (size_t i = 0; i < cells; ++i)
				{
					seed = seed * 1103515245u + 12345u;
					const bool camera = (seed >> 16) % 100 < share;
					seed = seed * 1103515245u + 12345u;
					const int ox = static_cast<int>((seed >> 16) % 300) - 150;
					seed = seed * 1103515245u + 12345u;
					const int oy = static_cast<int>((seed >> 16) % 300) - 150;
					v[i * 2] = static_cast<int16_t>(camera ? cx : ox * 32);
					v[i * 2 + 1] = static_cast<int16_t>(camera ? cy : oy * 32);
				}
			};
			float g[4], second[4];
			// 60 % outliers: the camera (14, -10) half px = (3.5, -2.5) 1/8-size texels, exactly.
			field(40, 14 * 32, -10 * 32, 11);
			MotionAhead::FlowGlobal(v, cost, gw, gh, g, second);
			Assert::AreEqual(3.5f, g[0], 1e-6f);
			Assert::AreEqual(-2.5f, g[1], 1e-6f);
			Assert::IsTrue(OpticalFlowTrust::GlobalUsable(g), L"40 % agree: used");
			// 5 % agree: not clear - the global search runs instead (the fallback).
			field(5, 14 * 32, -10 * 32, 12);
			MotionAhead::FlowGlobal(v, cost, gw, gh, g, second);
			Assert::IsFalse(OpticalFlowTrust::GlobalUsable(g), L"too few agree: the search");
			// Sub-cell motion: refined by the mean of the agreeing cells, then on D1's half-texel steps.
			field(70, 13 * 32 + 16, 0, 13);   // 13.5 half px = 3.375 texels -> 3.5
			MotionAhead::FlowGlobal(v, cost, gw, gh, g, second);
			Assert::AreEqual(3.5f, g[0], 1e-6f);
			Assert::AreEqual(0.0f, g[1], 1e-6f);
			// Unsure cells (high cost) do not vote: 40 % share one wrong vector but are unsure; the
			// camera's 30 % win over them (the other 30 % are sure noise).
			unsigned noise = 21;
			for (size_t i = 0; i < cells; ++i)
			{
				const size_t kind = i % 10;
				noise = noise * 1103515245u + 12345u;
				const int nx = static_cast<int>((noise >> 16) % 300) - 150;
				noise = noise * 1103515245u + 12345u;
				const int ny = static_cast<int>((noise >> 16) % 300) - 150;
				v[i * 2] = static_cast<int16_t>(kind < 3 ? 8 * 32 : kind < 7 ? -40 * 32 : nx * 32);
				v[i * 2 + 1] = static_cast<int16_t>(kind < 3 ? 4 * 32 : kind < 7 ? 20 * 32 : ny * 32);
				cost[i] = static_cast<uint8_t>(kind >= 3 && kind < 7 ? 200 : 10);
			}
			MotionAhead::FlowGlobal(v, cost, gw, gh, g, second);
			Assert::AreEqual(2.0f, g[0], 1e-6f, L"the sure cells' camera, not the unsure majority's vector");
			Assert::AreEqual(1.0f, g[1], 1e-6f);
			// No field (no engine): never used.
			MotionAhead::FlowGlobal(std::vector<int16_t>(), std::vector<uint8_t>(), 0, 0, g, second);
			Assert::IsFalse(OpticalFlowTrust::GlobalUsable(g), L"no field: ambiguous, the search");
			const float invalid[4] = { 0.0f, 0.0f, 0.5f, 0.0f };
			Assert::IsFalse(OpticalFlowTrust::GlobalUsable(invalid), L"not valid: the search");
		}

		TEST_METHOD(VectorStoreCarriesTheExtrasAndForgetsThemAtADiscontinuity)
		{
			auto make = [](int unsureCost) {
				auto flow = std::make_shared<MotionAhead::Flow>();
				flow->halfW = 384; flow->halfH = 192; flow->gridW = 96; flow->gridH = 48;
				flow->forward.assign(96 * 48 * 2, 0);
				flow->backward.assign(96 * 48 * 2, 0);
				for (size_t i = 0; i < 96 * 48; ++i) { flow->forward[i * 2] = 4 * 32; flow->backward[i * 2] = -4 * 32; }
				flow->forwardCost = Costs(96, 48, 10, unsureCost, 0, 0, 8, 8);
				flow->backwardCost = flow->forwardCost;
				MotionAhead::FlowExtras(*flow);
				return MotionAhead::FlowPtr(flow);
			};
			VectorStore<MotionAhead::FlowPtr> store;
			store.SetGeneration(5);
			const MotionAhead::FlowPtr flow = make(250);
			Assert::AreEqual(static_cast<size_t>(48 * 24), flow->costWeight.size());
			Assert::AreEqual(0.0f, flow->costWeight[0], L"the unsure corner");
			Assert::IsTrue(store.Put(100, 101, 5, flow));
			VectorStore<MotionAhead::FlowPtr>::Entry e;
			Assert::IsTrue(store.Get(100, 101, e));
			Assert::IsTrue(e.payload == flow, L"the same field, its weights with it");
			Assert::AreEqual(0.0f, e.payload->costWeight[0]);
			Assert::AreEqual(1.0f, e.payload->costWeight[30]);
			Assert::AreEqual(0.0f, e.payload->fbError[100], 1e-6f, L"a consistent pan");
			// A discontinuity: the pair and its weights are gone; one computed before it is refused.
			store.SetGeneration(6);
			Assert::IsFalse(store.Get(100, 101, e), L"cleared: the matcher runs (no stale weights)");
			Assert::IsFalse(store.Put(101, 102, 5, make(250)), L"computed before the discontinuity: refused");
			Assert::IsTrue(store.Put(101, 102, 6, make(10)));
			Assert::IsTrue(store.Get(101, 102, e));
			for (float w : e.payload->costWeight) Assert::AreEqual(1.0f, w, L"the new pair's own weights");
		}
	};
}
