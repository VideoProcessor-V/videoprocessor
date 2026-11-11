// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// I-to-I long-pair service, CPU only: S10.5 + hint resampling, the seeded search law, the service paths with the NVOF
// session OFF (no GPU touched) and the visibility of the fallback. The hinted NVOF session itself is GPU-only
// (LongPairBenchDump / hardware).
#include <VP-V/core/PassTimerInbox.h>
#include <VP-V/core/ProductCatalog.h>
#include <VP-V/core/VramUsers.h>
#include <VP-V/modules/motion/estimation/MotionValidity.h>
#include <VP-V/modules/motion/estimation/VectorEngineInterface.h>
#include <VP-V/modules/motion/estimation/longpair/LongPairService.h>
#include <VP-V/modules/motion/estimation/longpair/NvOpticalFlowHinted.h>

#include <atlbase.h>
#include <d3d11.h>
#include <dxgi1_4.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

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

	namespace
	{
		CComPtr<IDXGIAdapter> WarpAdapter()
		{
			CComPtr<IDXGIFactory4> factory;
			CComPtr<IDXGIAdapter> adapter;
			if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
				factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter));
			return adapter;
		}

		bool WaitStored(Service& svc, uint64_t n)
		{
			for (int i = 0; i < 400; ++i)
			{
				if (svc.GetStats().stored >= n)
					return true;
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
			return false;
		}

		// A backend that stays inside Run until released (or 5 s), to prove SetBackend waits for it.
		struct BlockingBackend : ISeededBackend
		{
			std::mutex m;
			std::condition_variable cv;
			bool entered = false, release = false;
			std::atomic<bool> finished{ false };
			const char* Name() const override { return "blocking"; }
			bool Run(const uint8_t*, const uint8_t*, int w, int h, const HintField*, Field& out, std::string&) override
			{
				{
					std::unique_lock<std::mutex> lock(m);
					entered = true;
					cv.notify_all();
					cv.wait_for(lock, std::chrono::seconds(5), [&] { return release; });
				}
				out.gridW = GridOf(w);
				out.gridH = GridOf(h);
				out.forward.assign(static_cast<size_t>(out.gridW) * out.gridH * 2, 0);
				out.backward = out.forward;
				out.forwardCost.assign(static_cast<size_t>(out.gridW) * out.gridH, 0);
				out.backwardCost = out.forwardCost;
				finished = true;
				return true;
			}
		};
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

		// MV4: the service is catalogued with its three cost routes before it is wired, and a finished pair reaches the pass-timer inbox.
		TEST_METHOD(ServiceProductIsCataloguedWithItsVramUserAndPassTimerRoute)
		{
			const std::vector<VpSkeleton::Catalog::ServiceProductRow>& rows = VpSkeleton::Catalog::ServiceProductRows();
			Assert::AreEqual(size_t(1), rows.size());
			const VpSkeleton::Catalog::ServiceProductRow& row = rows[0];
			Assert::AreEqual(std::string("long_pair_field"), std::string(row.token));
			Assert::IsTrue(row.tier == VpSkeleton::Tier::T2, L"optional, shed first");
			Assert::AreEqual(std::string(VpMotion::kLongPairFallbackId), std::string(row.fallbackId), L"the visible absence entry is the service's own");
			size_t count = 0;
			const uint16_t* declarable = VpSkeleton::DeclarableVramUsers(&count);
			bool declared = false;
			for (size_t i = 0; i < count; ++i) declared = declared || declarable[i] == row.vramUser;
			Assert::IsTrue(declared, L"its VRAM user is one the governor's population guard knows");
			Assert::AreEqual(std::string(PassTimers::Rows::kLongPairNvof), std::string(row.passTimerRow));
			Assert::IsTrue(std::string(row.passTimerRow).compare(0, 8, "motion: ") == 0, L"a motion row");
		}

		TEST_METHOD(AFinishedLongPairPostsItsCostToThePassTimerInbox)
		{
			const int w = 48, h = 32;
			std::vector<uint8_t> a, b;
			Frames(w, h, 2.0, 1.0, a, b);
			Service svc(CpuOnly());
			const uint64_t before = PassTimers::Inbox::Motion().Posted();
			const Result r = svc.RunNow(a.data(), b.data(), w, h, nullptr);
			Assert::IsTrue(r.ok && r.path == Path::SeededSearch);
			Assert::AreEqual(before + 1, PassTimers::Inbox::Motion().Posted(), L"one post per finished pair");
			bool seen = false;
			for (const std::string& line : PassTimers::Inbox::Motion().Lines())
				seen = seen || line.compare(0, std::string(PassTimers::Rows::kLongPairSeeded).size(), PassTimers::Rows::kLongPairSeeded) == 0;
			Assert::IsTrue(seen, L"the seeded-search row is in the readout");
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

		// W1-C2a A7 (doc 88 gap 6): the resync contract the wiring must keep (README "Resync contract").
		TEST_METHOD(EpochSourceRefusesAPairAcrossAnEpochMove)
		{
			const int w = 48, h = 32;
			std::vector<uint8_t> a, b;
			Frames(w, h, 4.0, 0.0, a, b);
			Service svc(CpuOnly());
			Assert::IsFalse(svc.EpochSourceBound());
			MotionValidity::State validity;
			svc.SetEpochSource(&validity);
			Assert::IsTrue(svc.EpochSourceBound());
			auto waitStored = [&](uint64_t n) {
				for (int i = 0; i < 400 && svc.GetStats().stored < n; ++i)
					std::this_thread::sleep_for(std::chrono::milliseconds(25));
				Assert::AreEqual(n, svc.GetStats().stored, L"the worker finished the pair");
			};
			const uint64_t t1 = svc.SubmitLongPair(a.data(), b.data(), w, h, nullptr);
			Assert::IsTrue(t1 > 0);
			waitStored(1);
			validity.Invalidate(MotionValidity::Cause::Seek);   // unbound: the epoch moves at once
			Result r;
			Assert::IsFalse(svc.TryTake(t1, r), L"a pair submitted before the epoch moved is refused");
			Assert::AreEqual(1ull, svc.GetStats().staleEpoch);
			Assert::IsFalse(svc.TryTake(t1, r), L"and dropped, not kept");
			const uint64_t t2 = svc.SubmitLongPair(a.data(), b.data(), w, h, nullptr);
			waitStored(2);
			Assert::IsTrue(svc.TryTake(t2, r), L"a pair of the current epoch is served");
			Assert::IsTrue(r.ok && r.epoch == validity.Epoch());
			Assert::AreEqual(0ull, svc.GetStats().withoutEpochSource);
			svc.SetEpochSource(nullptr);
			Assert::IsTrue(svc.SubmitLongPair(a.data(), b.data(), w, h, nullptr) > 0);
			Assert::AreEqual(1ull, svc.GetStats().withoutEpochSource, L"an unbound submit is counted");
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

	// The five pre-P5a fixes (opus gate): backend lifetime, Map-failure latch, device-removed recovery, SetAdapter required,
	// eviction count. CPU / WARP only: no NVIDIA hardware needed.
	TEST_CLASS(LongPairFixTests)
	{
	public:
		// Fix 1: SetBackend returned while a pair was still inside the old backend (Process copied the raw pointer out of the
		// lock), so the caller could destroy a backend that was in use.
		TEST_METHOD(SetBackendWaitsForARunInFlightSoTheOldBackendCanBeDestroyed)
		{
			const int w = 48, h = 32;
			std::vector<uint8_t> a, b;
			Frames(w, h, 1.0, 1.0, a, b);
			Service svc(CpuOnly());
			BlockingBackend be;
			svc.SetBackend(&be);
			std::thread runner([&] { svc.RunNow(a.data(), b.data(), w, h, nullptr); });
			{
				std::unique_lock<std::mutex> lock(be.m);
				Assert::IsTrue(be.cv.wait_for(lock, std::chrono::seconds(5), [&] { return be.entered; }), L"the backend was entered");
			}
			std::atomic<bool> setReturned{ false };
			std::thread swapper([&] { svc.SetBackend(nullptr); setReturned = true; });
			std::this_thread::sleep_for(std::chrono::milliseconds(150));
			const bool returnedWhileInside = setReturned.load() && !be.finished.load();
			{
				std::lock_guard<std::mutex> lock(be.m);
				be.release = true;
			}
			be.cv.notify_all();
			runner.join();
			swapper.join();
			Assert::IsFalse(returnedWhileInside, L"SetBackend must not return while the old backend is still running");
			Assert::IsTrue(be.finished.load());
		}

		TEST_METHOD(NoCallReachesTheOldBackendAfterSetBackendReturned)
		{
			const int w = 48, h = 32;
			std::vector<uint8_t> a, b;
			Frames(w, h, 1.0, 1.0, a, b);
			Service svc(CpuOnly());
			BlockingBackend be;
			{
				std::lock_guard<std::mutex> lock(be.m);
				be.release = true;   // never blocks
			}
			svc.SetBackend(&be);
			Assert::IsTrue(svc.RunNow(a.data(), b.data(), w, h, nullptr).ok);
			Assert::IsTrue(be.finished.load());
			be.finished = false;
			svc.SetBackend(nullptr);
			const Result r = svc.RunNow(a.data(), b.data(), w, h, nullptr);
			Assert::IsTrue(r.ok && r.path == Path::SeededSearch);
			Assert::IsFalse(be.finished.load(), L"the unbound backend is never called again");
		}

		TEST_METHOD(AFailedBackendDoesNotReturnTheFieldOfAnEarlierAttempt)
		{
			struct Failing : ISeededBackend
			{
				const char* Name() const override { return "failing"; }
				bool Run(const uint8_t*, const uint8_t*, int, int, const HintField*, Field&, std::string& error) override
				{
					error = "backend down";
					return false;
				}
			} failing;
			const int w = 48, h = 32;
			std::vector<uint8_t> a, b;
			Frames(w, h, 1.0, 1.0, a, b);
			Service svc(CpuOnly());
			svc.SetBackend(&failing);
			const Result r = svc.RunNow(a.data(), b.data(), w, h, nullptr);
			Assert::IsFalse(r.ok);
			Assert::IsTrue(r.path == Path::None);
			Assert::IsTrue(r.field.forward.empty() && r.field.gridW == 0, L"a failed pair carries no field");
			Assert::IsTrue(r.error == "backend down");
			Assert::IsTrue(svc.StatusLine().find("long-pair flow failed (backend down)") != std::string::npos);
		}

		// Fix 2: a failed Map must latch a visible state and never read stale data. A DEFAULT texture without CPU access cannot
		// be mapped, which is a real Map failure on WARP.
		BEGIN_TEST_METHOD_ATTRIBUTE(AFailedMapReturnsFalseWithAnHresultAndLeavesTheDestinationUntouched)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(AFailedMapReturnsFalseWithAnHresultAndLeavesTheDestinationUntouched)
		{
			CComPtr<ID3D11Device> device;
			CComPtr<ID3D11DeviceContext> context;
			Assert::IsTrue(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)));
			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = 8; desc.Height = 4; desc.MipLevels = 1; desc.ArraySize = 1; desc.Format = DXGI_FORMAT_R8_UNORM;
			desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			CComPtr<ID3D11Texture2D> unmappable;
			Assert::IsTrue(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &unmappable)));
			std::vector<uint8_t> dst(32, 0xAB);
			HRESULT hr = S_OK;
			Assert::IsFalse(NvOpticalFlow::ReadStagingRows(context, unmappable, 4, 8, dst.data(), hr));
			Assert::IsTrue(FAILED(hr), L"the Map failure is reported with its HRESULT");
			for (uint8_t v : dst)
				Assert::AreEqual(0xAB, static_cast<int>(v), L"no stale or partial data is written");
			// Null arguments are failures, not crashes.
			Assert::IsFalse(NvOpticalFlow::ReadStagingRows(nullptr, unmappable, 4, 8, dst.data(), hr));
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(AMappableStagingTextureIsReadRowByRow)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(AMappableStagingTextureIsReadRowByRow)
		{
			CComPtr<ID3D11Device> device;
			CComPtr<ID3D11DeviceContext> context;
			Assert::IsTrue(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)));
			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = 6; desc.Height = 3; desc.MipLevels = 1; desc.ArraySize = 1; desc.Format = DXGI_FORMAT_R8_UNORM;
			desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			CComPtr<ID3D11Texture2D> src, staging;
			Assert::IsTrue(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &src)));
			desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			Assert::IsTrue(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging)));
			std::vector<uint8_t> pattern(18);
			for (size_t i = 0; i < pattern.size(); ++i)
				pattern[i] = static_cast<uint8_t>(10 + i);
			context->UpdateSubresource(src, 0, nullptr, pattern.data(), 6, 0);
			context->CopyResource(staging, src);
			std::vector<uint8_t> dst(18, 0);
			HRESULT hr = E_FAIL;
			Assert::IsTrue(NvOpticalFlow::ReadStagingRows(context, staging, 3, 6, dst.data(), hr));
			Assert::IsTrue(dst == pattern, L"rows are copied by RowPitch into a tight buffer");
		}

		TEST_METHOD(TheSessionLatchHoldsThirtySecondsThenRetriesAndCountsExecuteFailures)
		{
			SessionLatch latch;
			const SessionLatch::Time t0 = std::chrono::steady_clock::now();
			std::string why;
			Assert::IsFalse(latch.Blocked(t0, why));
			Assert::IsFalse(latch.NoteExecuteFailure("e1", t0));
			Assert::IsFalse(latch.NoteExecuteFailure("e2", t0));
			Assert::IsFalse(latch.Blocked(t0, why), L"two failures do not latch");
			Assert::IsTrue(latch.NoteExecuteFailure("e3", t0), L"the third in a row latches");
			Assert::IsTrue(latch.Blocked(t0 + std::chrono::seconds(29), why));
			Assert::IsTrue(why == "e3");
			Assert::IsFalse(latch.Blocked(t0 + std::chrono::seconds(31), why), L"retried after 30 s");
			latch.NoteExecuteFailure("x", t0);
			latch.NoteSuccess();
			Assert::IsFalse(latch.NoteExecuteFailure("y", t0), L"a success resets the run of failures");
			latch.Latch("map failed", t0);
			Assert::IsTrue(latch.Blocked(t0 + std::chrono::seconds(1), why) && why == "map failed", L"an immediate latch (Map failure) holds at once");
			latch.Clear();
			Assert::IsFalse(latch.Latched());
		}

		// Fix 3: a removed device is released, reported with its HRESULT and latched; the pair still gets the visible fallback.
		BEGIN_TEST_METHOD_ATTRIBUTE(ARemovedDeviceIsReleasedReportedAndLatched)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(ARemovedDeviceIsReleasedReportedAndLatched)
		{
			CComPtr<IDXGIAdapter> warp = WarpAdapter();
			Assert::IsTrue(warp.p != nullptr, L"WARP adapter");
			const int w = 64, h = 48;
			std::vector<uint8_t> a, b;
			Frames(w, h, 2.0, 1.0, a, b);
			Config c;
			c.useNvof = true;
			Service svc(c);
			svc.SetAdapter(warp);
			svc.InjectDeviceRemovedForTest(static_cast<long>(DXGI_ERROR_DEVICE_REMOVED));
			const Result r = svc.RunNow(a.data(), b.data(), w, h, nullptr);
			Assert::IsTrue(r.ok && r.path == Path::SeededSearch, L"the pair still gets a field");
			Assert::IsTrue(r.why.reason.find("GPU device removed or reset (0x887A0005)") != std::string::npos, L"the removal is visible with its HRESULT");
			Assert::IsTrue(svc.CurrentDecision().engaged);
			Assert::IsTrue(svc.StatusLine().find("device removed") != std::string::npos);
			// Latched: the next pair does not try the GPU again and keeps the same visible reason.
			const Result r2 = svc.RunNow(a.data(), b.data(), w, h, nullptr);
			Assert::IsTrue(r2.path == Path::SeededSearch);
			Assert::IsTrue(r2.why.reason == r.why.reason);
			Assert::IsTrue(DeviceRemovedText(static_cast<long>(DXGI_ERROR_DEVICE_RESET), "x").find("0x887A0007") != std::string::npos);
		}

		// Fix 4: no SetAdapter, no GPU session, and it says so (the old code created a default hardware device).
		TEST_METHOD(WithoutSetAdapterTheNvofSessionRefusesVisibly)
		{
			const int w = 64, h = 48;
			std::vector<uint8_t> a, b;
			Frames(w, h, 2.0, 1.0, a, b);
			Config c;
			c.useNvof = true;
			Service svc(c);
			const HintField hf = ConstHint(w, h, 2.0f, 1.0f);
			const Result r = svc.RunNow(a.data(), b.data(), w, h, &hf);
			Assert::IsTrue(r.ok && r.path == Path::SeededSearch);
			Assert::IsTrue(r.why.reason.find("SetAdapter was not called") != std::string::npos);
			Assert::IsTrue(svc.CurrentDecision().engaged);
			Assert::IsTrue(svc.StatusLine().find("SetAdapter was not called") != std::string::npos);
			svc.SetAdapter(nullptr);   // null is not an adapter
			const Result r2 = svc.RunNow(a.data(), b.data(), w, h, &hf);
			Assert::IsTrue(r2.why.reason.find("SetAdapter was not called") != std::string::npos);
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(SetAdapterLiftsTheRefusalImmediately)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(SetAdapterLiftsTheRefusalImmediately)
		{
			CComPtr<IDXGIAdapter> warp = WarpAdapter();
			Assert::IsTrue(warp.p != nullptr);
			const int w = 64, h = 48;
			std::vector<uint8_t> a, b;
			Frames(w, h, 2.0, 1.0, a, b);
			Config c;
			c.useNvof = true;
			Service svc(c);
			Assert::IsTrue(svc.RunNow(a.data(), b.data(), w, h, nullptr).why.reason.find("SetAdapter was not called") != std::string::npos);
			svc.SetAdapter(warp);
			const Result r = svc.RunNow(a.data(), b.data(), w, h, nullptr);
			Assert::IsTrue(r.path == Path::SeededSearch);
			Assert::IsTrue(r.why.reason.find("SetAdapter was not called") == std::string::npos, L"a different reason now: the session was attempted on the given adapter");
		}

		// Fix 5: each evicted result is counted once, and only results that are really waiting are evicted.
		TEST_METHOD(EvictionCountsEachDroppedResultOnce)
		{
			const int w = 48, h = 32;
			std::vector<uint8_t> a, b;
			Frames(w, h, 1.0, 0.0, a, b);
			Config c = CpuOnly();
			c.maxResults = 2;
			Service svc(c);
			std::vector<uint64_t> tickets;
			for (int i = 1; i <= 4; ++i)
			{
				const uint64_t t = svc.SubmitLongPair(a.data(), b.data(), w, h, nullptr);
				Assert::IsTrue(t > 0);
				tickets.push_back(t);
				Assert::IsTrue(WaitStored(svc, static_cast<uint64_t>(i)));
			}
			Assert::AreEqual(2ull, svc.GetStats().evicted);
			Result r;
			Assert::IsFalse(svc.TryTake(tickets[0], r));
			Assert::IsFalse(svc.TryTake(tickets[1], r));
			Assert::IsTrue(svc.TryTake(tickets[2], r));
			Assert::IsTrue(svc.TryTake(tickets[3], r));
			Assert::AreEqual(2ull, svc.GetStats().evicted, L"taking results is not an eviction");
		}

		TEST_METHOD(ATakenResultFreesItsSlotSoAnUntakenOneIsNotEvictedEarly)
		{
			const int w = 48, h = 32;
			std::vector<uint8_t> a, b;
			Frames(w, h, 1.0, 0.0, a, b);
			Config c = CpuOnly();
			c.maxResults = 3;
			Service svc(c);
			std::vector<uint64_t> t;
			for (int i = 1; i <= 3; ++i)
			{
				t.push_back(svc.SubmitLongPair(a.data(), b.data(), w, h, nullptr));
				Assert::IsTrue(WaitStored(svc, static_cast<uint64_t>(i)));
			}
			Result r;
			Assert::IsTrue(svc.TryTake(t[1], r));
			Assert::IsTrue(svc.TryTake(t[2], r));
			t.push_back(svc.SubmitLongPair(a.data(), b.data(), w, h, nullptr));
			Assert::IsTrue(WaitStored(svc, 4));
			Assert::AreEqual(0ull, svc.GetStats().evicted, L"two results are waiting, the bound is three");
			Assert::IsTrue(svc.TryTake(t[0], r), L"the oldest untaken result is still there");
			Assert::IsTrue(svc.TryTake(t[3], r));
		}
	};
}
