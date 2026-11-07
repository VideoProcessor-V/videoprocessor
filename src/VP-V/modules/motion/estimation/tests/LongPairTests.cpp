// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// I-to-I long-pair service, CPU only: S10.5 + hint resampling, the seeded search law, the service paths with the NVOF
// session OFF (no GPU touched) and the visibility of the fallback. The hinted NVOF session itself is GPU-only
// (LongPairBenchDump / hardware).
#include <VP-V/modules/motion/estimation/longpair/LongPairService.h>

#include <chrono>
#include <cmath>
#include <thread>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	namespace
	{
		using namespace VpLongPair;

		double T(double x, double y)
		{
			return 128.0 + 60.0 * std::sin(0.37 * x) + 50.0 * std::cos(0.29 * y + 0.11 * x) + 20.0 * std::sin(0.071 * x * y * 0.1 + x);
		}

		uint8_t U8(double v) { return static_cast<uint8_t>(std::min(255.0, std::max(0.0, std::floor(v + 0.5)))); }

		// B(q) = A(q - m): content moves by m from A to B.
		void Frames(int w, int h, double mx, double my, std::vector<uint8_t>& a, std::vector<uint8_t>& b)
		{
			a.resize(static_cast<size_t>(w) * h);
			b.resize(a.size());
			for (int y = 0; y < h; ++y)
				for (int x = 0; x < w; ++x)
				{
					a[static_cast<size_t>(y) * w + x] = U8(T(x, y));
					b[static_cast<size_t>(y) * w + x] = U8(T(x - mx, y - my));
				}
		}

		HintField ConstHint(int w, int h, float vx, float vy)
		{
			HintField hf;
			hf.cellPx = 8;
			hf.gridW = (w + 7) / 8;
			hf.gridH = (h + 7) / 8;
			hf.vx.assign(static_cast<size_t>(hf.gridW) * hf.gridH, vx);
			hf.vy.assign(hf.vx.size(), vy);
			return hf;
		}

		Config CpuOnly()
		{
			Config c;
			c.useNvof = false;
			return c;
		}
	}

	TEST_CLASS(LongPairTests)
	{
	public:
		TEST_METHOD(S105RoundTripClampAndNonFinite)
		{
			Assert::AreEqual(static_cast<int>(80), static_cast<int>(ToS105(2.5f)));
			Assert::AreEqual(static_cast<int>(-40), static_cast<int>(ToS105(-1.25f)));
			Assert::AreEqual(2.5f, FromS105(80));
			Assert::AreEqual(32767, static_cast<int>(ToS105(5000.0f)));
			Assert::AreEqual(-32767, static_cast<int>(ToS105(-5000.0f)));
			Assert::AreEqual(0, static_cast<int>(ToS105(std::nanf(""))));
			Assert::AreEqual(1, static_cast<int>(ToS105(1.0f / 32.0f)));
		}

		TEST_METHOD(ResampleHintMatchesTheHandComputedRamp)
		{
			HintField hf;
			hf.gridW = 2; hf.gridH = 1; hf.cellPx = 8;
			hf.vx = { 0.0f, 8.0f };
			hf.vy = { 1.0f, 1.0f };
			std::vector<int16_t> out;
			Assert::IsTrue(ResampleHint(hf, 16, 8, out));
			Assert::AreEqual(static_cast<size_t>(4 * 2 * 2), out.size());
			const int expectX[4] = { 0, 64, 192, 256 };   // 0, 2, 6, 8 half px in S10.5 (hand computed, section "u = (4c+2)/8 - 0.5")
			for (int cy = 0; cy < 2; ++cy)
				for (int cx = 0; cx < 4; ++cx)
				{
					Assert::AreEqual(expectX[cx], static_cast<int>(out[(static_cast<size_t>(cy) * 4 + cx) * 2]));
					Assert::AreEqual(32, static_cast<int>(out[(static_cast<size_t>(cy) * 4 + cx) * 2 + 1]));
				}
			HintField bad;
			Assert::IsFalse(ResampleHint(bad, 16, 8, out));
		}

		TEST_METHOD(NonFiniteHintCellsActAsZero)
		{
			HintField hf = ConstHint(16, 8, 3.0f, 0.0f);
			hf.vx[0] = std::nanf("");
			std::vector<int16_t> out;
			Assert::IsTrue(ResampleHint(hf, 16, 8, out));
			Assert::AreEqual(0, static_cast<int>(out[0]), L"cell 0 reads the zero cell only");
		}

		TEST_METHOD(SubPelIsExactOnAnExactParabola)
		{
			Assert::AreEqual(4.0 / 28.0, static_cast<double>(SubPel(13, 4, 9)), 1e-6);
			Assert::AreEqual(0.0, static_cast<double>(SubPel(5, 5, 5)), 0.0);
			Assert::AreEqual(-0.5, static_cast<double>(SubPel(0, 1, 40)), 0.0, L"clamped");
		}

		TEST_METHOD(SeededSearchFindsAShiftBeyondTheRangeOnlyWithTheHint)
		{
			const int w = 64, h = 48;
			std::vector<uint8_t> a, b;
			Frames(w, h, 10.0, -7.0, a, b);
			Field seeded, zero;
			const HintField hf = ConstHint(w, h, 10.0f, -7.0f);
			Assert::IsTrue(SeededSearchPair(a.data(), b.data(), w, h, &hf, seeded));
			Assert::IsTrue(SeededSearchPair(a.data(), b.data(), w, h, nullptr, zero));
			Assert::AreEqual(16, seeded.gridW); Assert::AreEqual(12, seeded.gridH);
			int good = 0, goodZero = 0, n = 0, backGood = 0, backN = 0, zeroCost = 0;
			for (int cy = 3; cy <= 10; ++cy)
				for (int cx = 1; cx <= 11; ++cx)
				{
					const size_t i = static_cast<size_t>(cy) * seeded.gridW + cx;
					++n;
					if (std::fabs(FromS105(seeded.forward[i * 2]) - 10.0f) <= 0.5f && std::fabs(FromS105(seeded.forward[i * 2 + 1]) + 7.0f) <= 0.5f)
						++good;
					if (std::fabs(FromS105(zero.forward[i * 2]) - 10.0f) <= 1.0f && std::fabs(FromS105(zero.forward[i * 2 + 1]) + 7.0f) <= 1.0f)
						++goodZero;
					if (cx >= 3 && cy <= 8)   // the backward window (shifted by -hint) stays inside the frame
					{
						++backN;
						if (std::fabs(FromS105(seeded.backward[i * 2]) + 10.0f) <= 0.5f && std::fabs(FromS105(seeded.backward[i * 2 + 1]) - 7.0f) <= 0.5f)
							++backGood;
					}
					if (seeded.forwardCost[i] <= 2)
						++zeroCost;
				}
			char msg[160];
			snprintf(msg, sizeof(msg), "LONGPAIR_SEEDED cells=%d seeded_good=%d zero_seed_good=%d backward_good=%d low_cost=%d\n", n, good, goodZero, backGood, zeroCost);
			Logger::WriteMessage(msg);
			Assert::IsTrue(good >= n * 9 / 10, L"the hint seed recovers the long shift");
			Assert::IsTrue(goodZero < n / 4, L"a zero seed cannot reach it (range 6)");
			Assert::IsTrue(backGood >= backN * 8 / 10, L"backward uses -hint");
			Assert::IsTrue(zeroCost >= n * 8 / 10, L"a true match has a near-zero cost");
		}

		TEST_METHOD(HintOutsideRangeOfTheTruthStillSearchesAroundIt)
		{
			const int w = 64, h = 48;
			std::vector<uint8_t> a, b;
			Frames(w, h, 10.0, -7.0, a, b);
			Field f;
			const HintField off = ConstHint(w, h, 12.0f, -5.0f);   // 2 px off: inside +-6 of the seed
			Assert::IsTrue(SeededSearchPair(a.data(), b.data(), w, h, &off, f));
			const size_t i = static_cast<size_t>(6) * f.gridW + 6;
			Assert::AreEqual(10.0, static_cast<double>(FromS105(f.forward[i * 2])), 0.5);
			Assert::AreEqual(-7.0, static_cast<double>(FromS105(f.forward[i * 2 + 1])), 0.5);
		}

		TEST_METHOD(ServiceWithoutNvofTakesTheVisibleSeededPathWhenAHintIsSupplied)
		{
			const int w = 48, h = 32;
			std::vector<uint8_t> a, b;
			Frames(w, h, 6.0, 2.0, a, b);
			Service svc(CpuOnly());
			const HintField hf = ConstHint(w, h, 6.0f, 2.0f);
			const Result r = svc.RunNow(a.data(), b.data(), w, h, &hf);
			Assert::IsTrue(r.ok);
			Assert::IsTrue(r.path == Path::SeededSearch);
			Assert::IsTrue(r.why.hintSupplied);
			const auto d = svc.CurrentDecision();
			Assert::IsTrue(d.engaged, L"a hinted pair that did not get the hinted session must be visible");
			Assert::IsTrue(d.reason.find("long-pair seeded search (NVOF with hints unavailable: long-pair NVOF off)") != std::string::npos);
			Assert::IsTrue(svc.StatusLine().find("seeded search") != std::string::npos);
			Assert::AreEqual(1ull, svc.GetStats().seeded);
		}

		TEST_METHOD(ServiceWithoutAHintStillShowsTheNvofDowngrade)
		{
			const int w = 48, h = 32;
			std::vector<uint8_t> a, b;
			Frames(w, h, 2.0, 1.0, a, b);
			Service svc(CpuOnly());
			const Result r = svc.RunNow(a.data(), b.data(), w, h, nullptr);
			Assert::IsTrue(r.ok && r.path == Path::SeededSearch && !r.why.hintSupplied);
			Assert::IsTrue(svc.CurrentDecision().engaged, L"NVOF down is visible even without a hint");
			Assert::IsTrue(svc.StatusLine() == "motion: long-pair seeded search (NVOF unavailable: long-pair NVOF off)");
		}

		TEST_METHOD(DecisionIsReleasedForTheNvofPaths)
		{
			Why w;
			w.hintSupplied = true;
			Assert::IsFalse(Decision(Path::NvofHinted, w).engaged);
			Assert::IsFalse(Decision(Path::NvofUnhinted, w).engaged);
			Assert::IsTrue(Decision(Path::SeededSearch, w).engaged);
			Assert::IsTrue(StatusLine(Path::NvofHinted, w).find("NVOF + hints") != std::string::npos);
			Assert::IsTrue(StatusLine(Path::SeededSearch, w).find("not opened") != std::string::npos, L"an empty reason is still worded");
		}

		TEST_METHOD(AsyncSubmitReturnsATicketAndTheResultArrives)
		{
			const int w = 48, h = 32;
			std::vector<uint8_t> a, b;
			Frames(w, h, 4.0, 0.0, a, b);
			Service svc(CpuOnly());
			const HintField hf = ConstHint(w, h, 4.0f, 0.0f);
			const uint64_t t = svc.SubmitLongPair(a.data(), b.data(), w, h, &hf);
			Assert::IsTrue(t > 0);
			Result r;
			bool got = false;
			for (int i = 0; i < 400 && !got; ++i)
			{
				got = svc.TryTake(t, r);
				if (!got)
					std::this_thread::sleep_for(std::chrono::milliseconds(25));
			}
			Assert::IsTrue(got, L"the worker finished the pair");
			Assert::IsTrue(r.ok && r.ticket == t && r.path == Path::SeededSearch);
			Assert::IsFalse(svc.TryTake(t, r), L"a result is taken once");
		}

		TEST_METHOD(BadInputsAreRefusedAndCounted)
		{
			Service svc(CpuOnly());
			std::vector<uint8_t> a(10, 0);
			Assert::AreEqual(0ull, svc.SubmitLongPair(a.data(), a.data(), 4, 4, nullptr));
			Assert::AreEqual(0ull, svc.SubmitLongPair(nullptr, nullptr, 64, 64, nullptr));
			Assert::AreEqual(2ull, svc.GetStats().refused);
			Assert::IsFalse(svc.RunNow(nullptr, nullptr, 64, 64, nullptr).ok);
		}

		TEST_METHOD(ABoundBackendReplacesTheCpuReference)
		{
			struct Fake : ISeededBackend
			{
				int calls = 0;
				bool sawHint = false;
				const char* Name() const override { return "fake"; }
				bool Run(const uint8_t*, const uint8_t*, int w, int h, const HintField* hint, Field& out, std::string&) override
				{
					++calls;
					sawHint = hint != nullptr;
					out.gridW = GridOf(w);
					out.gridH = GridOf(h);
					out.forward.assign(static_cast<size_t>(out.gridW) * out.gridH * 2, 7);
					out.backward = out.forward;
					out.forwardCost.assign(static_cast<size_t>(out.gridW) * out.gridH, 1);
					out.backwardCost = out.forwardCost;
					return true;
				}
			} fake;
			const int w = 48, h = 32;
			std::vector<uint8_t> a, b;
			Frames(w, h, 1.0, 1.0, a, b);
			Service svc(CpuOnly());
			svc.SetBackend(&fake);
			const HintField hf = ConstHint(w, h, 1.0f, 1.0f);
			const Result r = svc.RunNow(a.data(), b.data(), w, h, &hf);
			Assert::AreEqual(1, fake.calls);
			Assert::IsTrue(fake.sawHint);
			Assert::IsTrue(r.ok && r.path == Path::SeededSearch);
			Assert::AreEqual(7, static_cast<int>(r.field.forward[0]));
		}
	};
}
