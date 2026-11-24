// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// input.source_analysis/regions: P16 kCodingEvidence (lead8/iface-P16.md). The moved tile measure and frame reduction, the product's
// bytes (round trip, corrupt input refused), the owner fill through a real SkeletonMemo (computed once, equal to the direct measure),
// and the unavailable paths (no memo, nothing staged) that consumers show instead of measuring privately. CPU only.
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/core/ComputeOnce.h>
#include <VP-V/modules/input/source_analysis/regions/CodingEvidence.h>
#include <VP-V/modules/input/source_analysis/SourceResolutionDetector.h>
#include <VP-V/modules/input/source_analysis/SourceResolutionMonitor.h>
#include <VP-V/modules/input/source_analysis/SourceFrameTap.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	namespace
	{
		namespace CE = VpUnit::CodingEvidence;
		constexpr int kN = 64;

		std::vector<float> Clean(unsigned seed)
		{
			std::mt19937 rng(seed);
			std::normal_distribution<float> noise(0.0f, 0.004f);
			std::vector<float> v(size_t(kN) * kN);
			for (int y = 0; y < kN; ++y)
				for (int x = 0; x < kN; ++x)
					v[size_t(y) * kN + x] = 0.3f + 0.3f * float(x + y) / float(2 * kN) + noise(rng);
			return v;
		}
		// 8x8 blocks, each its own level: a coded picture.
		std::vector<float> Blocked(unsigned seed)
		{
			std::mt19937 rng(seed);
			std::uniform_real_distribution<float> level(0.2f, 0.6f);
			std::vector<float> v(size_t(kN) * kN);
			float lv[8][8];
			for (auto& r : lv)
				for (float& c : r)
					c = level(rng);
			for (int y = 0; y < kN; ++y)
				for (int x = 0; x < kN; ++x)
					v[size_t(y) * kN + x] = lv[y / 8][x / 8];
			return v;
		}
		// The frame reduction's fixtures, at the monitor's smallest real tile (SourceResolution::TileSize: 128 at 1080). Its robust
		// grid search needs at least eight repeats of the 8 px period (a 64 px tile has 63 steps: the search never runs there), and
		// it counts blocking only from tiles with variation inside the blocks (gradient > 0.05, kept from the detector): a coded
		// picture is block levels PLUS the texture the coder kept, never pure flat blocks.
		constexpr int kFrameTile = 128;
		std::vector<float> CleanAt(unsigned seed, int n)
		{
			std::mt19937 rng(seed);
			std::normal_distribution<float> noise(0.0f, 0.004f);
			std::vector<float> v(size_t(n) * n);
			for (int y = 0; y < n; ++y)
				for (int x = 0; x < n; ++x)
					v[size_t(y) * n + x] = 0.3f + 0.3f * float(x + y) / float(2 * n) + noise(rng);
			return v;
		}
		std::vector<float> CodedAt(unsigned seed, int n)
		{
			std::mt19937 rng(seed);
			std::uniform_real_distribution<float> level(0.2f, 0.6f);
			std::normal_distribution<float> noise(0.0f, 0.004f);
			const int blocks = n / 8;
			std::vector<float> lv(size_t(blocks) * blocks);
			for (float& c : lv)
				c = level(rng);
			std::vector<float> v(size_t(n) * n);
			for (int y = 0; y < n; ++y)
				for (int x = 0; x < n; ++x)
					v[size_t(y) * n + x] = lv[size_t(y / 8) * blocks + x / 8] + noise(rng);
			return v;
		}
		std::shared_ptr<CE::TilePack> PackOf(const std::vector<std::vector<float>>& tiles)
		{
			std::shared_ptr<CE::TilePack> p = std::make_shared<CE::TilePack>();
			p->tiles = int(tiles.size());
			p->tileSize = kN;
			p->codeMaximum = 1023;
			for (const auto& t : tiles)
				p->luma.insert(p->luma.end(), t.begin(), t.end());
			return p;
		}
		bool Same(const CE::TileMeasure& a, const CE::TileMeasure& b)
		{
			return a.histogram == b.histogram && a.flatHistogram == b.flatHistogram && a.blockiness == b.blockiness
				&& a.gradient == b.gradient && a.blocks == b.blocks && a.bandEdges == b.bandEdges && a.flatBlocks == b.flatBlocks
				&& a.blockinessRobust == b.blockinessRobust && a.blockPeriod == b.blockPeriod && a.bandEdgesRobust == b.bandEdgesRobust;
		}
		struct ProviderGuard
		{
			~ProviderGuard() { CE::Hub::Instance().SetMemoProvider(nullptr); }
		};
		struct ComputeOnceScope
		{
			bool prevEnabled = false;
			ComputeOnce::ThreadFrame prevFrame;
			ComputeOnceScope()
				: prevEnabled(ComputeOnce::Enabled())
				, prevFrame(ComputeOnce::Current())
			{
				ComputeOnce::SetEnabled(true);
				ComputeOnce::Global().Reset();
				ComputeOnce::Current() = ComputeOnce::ThreadFrame();
			}
			~ComputeOnceScope()
			{
				ComputeOnce::Global().Reset();
				ComputeOnce::Current() = prevFrame;
				ComputeOnce::SetEnabled(prevEnabled);
			}
			ComputeOnceScope(const ComputeOnceScope&) = delete;
			ComputeOnceScope& operator=(const ComputeOnceScope&) = delete;
		};
	}

	TEST_CLASS(CodingEvidenceTests)
	{
	public:
		TEST_METHOD(BlockedReadsBelowClean)
		{
			const CE::TileMeasure clean = CE::MeasureTile(Clean(1).data(), kN, kN, 1023);
			const CE::TileMeasure blocked = CE::MeasureTile(Blocked(2).data(), kN, kN, 1023);
			Assert::IsTrue(blocked.blockiness > clean.blockiness, L"the 8 px grid shows in the measure");
			const std::vector<char> eligible(1, 1);
			const CE::TileMeasure cleanFrame = CE::MeasureTile(CleanAt(1, kFrameTile).data(), kFrameTile, kFrameTile, 1023);
			const CE::TileMeasure codedFrame = CE::MeasureTile(CodedAt(2, kFrameTile).data(), kFrameTile, kFrameTile, 1023);
			const CE::FrameCoding c = CE::ReduceFrame({ &cleanFrame }, eligible, true);
			const CE::FrameCoding b = CE::ReduceFrame({ &codedFrame }, eligible, true);
			Assert::IsTrue(c.bitrateQuality > b.bitrateQuality, L"a blocked frame reads below a clean one");
			const CE::FrameCoding none = CE::ReduceFrame({}, {}, true);
			Assert::AreEqual(1.0, none.bitrateQuality, 1e-12, L"no tiles: neutral");
		}

		TEST_METHOD(SerialiseRoundTripIsExact)
		{
			std::vector<CE::TileMeasure> tiles;
			tiles.push_back(CE::MeasureTile(Clean(3).data(), kN, kN, 1023));
			tiles.push_back(CE::MeasureTile(Blocked(4).data(), kN, kN, 1023));
			const std::vector<uint8_t> bytes = CE::Serialise(tiles, kN, 1023);
			std::vector<CE::TileMeasure> back;
			Assert::IsTrue(CE::Deserialise(bytes.data(), bytes.size(), back), L"reads back");
			Assert::AreEqual(size_t(2), back.size());
			Assert::IsTrue(Same(tiles[0], back[0]) && Same(tiles[1], back[1]), L"every field survives");
		}

		TEST_METHOD(CorruptBytesAreRefused)
		{
			std::vector<CE::TileMeasure> tiles(1, CE::MeasureTile(Clean(5).data(), kN, kN, 1023));
			std::vector<uint8_t> bytes = CE::Serialise(tiles, kN, 1023);
			std::vector<CE::TileMeasure> back;
			Assert::IsFalse(CE::Deserialise(bytes.data(), bytes.size() / 2, back), L"truncated");
			Assert::IsFalse(CE::Deserialise(bytes.data(), 3, back), L"too short for the header");
			bytes[0] ^= 0xFF;
			Assert::IsFalse(CE::Deserialise(bytes.data(), bytes.size(), back), L"bad magic");
			bytes[0] ^= 0xFF;
			bytes[8] = 0xFF; bytes[9] = 0xFF; bytes[10] = 0xFF; bytes[11] = 0x7F;   // a huge tile count
			Assert::IsFalse(CE::Deserialise(bytes.data(), bytes.size(), back), L"a corrupt tile count");
		}

		TEST_METHOD(OwnerFillThroughTheMemoIsComputedOnceAndEqualsTheDirectMeasure)
		{
			ProviderGuard guard;
			auto memo = std::make_shared<VpSkeleton::SkeletonMemo>();
			Assert::IsTrue(memo->RegisterOwnerFill(VpSkeleton::kCodingEvidence, VpSkeleton::AcquireForm::Cpu, CE::kUnitPath,
				CE::MakeOwnerFill(CE::Hub::Instance(), 0)) == VpSkeleton::SkeletonMemo::Registration::Registered, L"owner registered");
			CE::Hub::Instance().SetMemoProvider([memo](const void*) { return memo; });
			const std::vector<std::vector<float>> src = { Clean(6), Blocked(7) };
			std::vector<CE::TileMeasure> got;
			std::string why;
			Assert::IsTrue(CE::Hub::Instance().AcquireTiles(this, 11, PackOf(src), got, why), L"acquired");
			Assert::AreEqual(size_t(2), got.size());
			for (size_t i = 0; i < 2; ++i)
				Assert::IsTrue(Same(got[i], CE::MeasureTile(src[i].data(), kN, kN, 1023)), L"the product equals the direct measure");
			std::vector<CE::TileMeasure> again;
			Assert::IsTrue(CE::Hub::Instance().AcquireTiles(this, 11, PackOf(src), again, why), L"second reader");
			const VpSkeleton::AcquireCounters c = memo->CountersOf(VpSkeleton::kCodingEvidence);
			Assert::IsTrue(c.ownerFillCalls == 1 && c.reusedMemo == 1, L"one fill, the second reader reuses it");
			Assert::IsTrue(CE::Hub::Instance().Staged(11) == nullptr, L"nothing stays staged");
		}

		TEST_METHOD(NoMemoIsUnavailableNotMeasured)
		{
			ProviderGuard guard;
			CE::Hub::Instance().SetMemoProvider(nullptr);
			std::vector<CE::TileMeasure> got;
			std::string why;
			Assert::IsFalse(CE::Hub::Instance().AcquireTiles(this, 1, PackOf({ Clean(8) }), got, why), L"no provider");
			Assert::IsTrue(why == "no skeleton memo" && got.empty(), L"visible reason, no tiles");
			CE::Hub::Instance().SetMemoProvider([](const void*) { return std::shared_ptr<VpSkeleton::SkeletonMemo>(); });
			Assert::IsFalse(CE::Hub::Instance().AcquireTiles(this, 1, PackOf({ Clean(8) }), got, why), L"provider without a memo");
		}

		TEST_METHOD(MemoWithoutAnOwnerFillDeclinesVisibly)
		{
			ProviderGuard guard;
			auto memo = std::make_shared<VpSkeleton::SkeletonMemo>();
			CE::Hub::Instance().SetMemoProvider([memo](const void*) { return memo; });
			std::vector<CE::TileMeasure> got;
			std::string why;
			Assert::IsFalse(CE::Hub::Instance().AcquireTiles(this, 2, PackOf({ Clean(9) }), got, why), L"no owner registered");
			Assert::IsFalse(why.empty(), L"the Acquire token is the reason");
		}

		TEST_METHOD(FillDeclinesWhenNothingIsStaged)
		{
			CE::Hub hub;
			const VpSkeleton::OwnerFill f = CE::MakeOwnerFill(hub, 0);
			VpSkeleton::SkeletonMemo m;
			VpSkeleton::OwnerFillRequest req;
			req.id = VpSkeleton::kCodingEvidence;
			req.seq = 5;
			VpSkeleton::OwnerFillOutput out;
			Assert::IsFalse(f.fill(req, m, out), L"declined, never invented");
		}

		// keepFrames = the monitor's worker latency (derived from its queue depth): a frame lagging inside the window is served,
		// one past it is visibly unavailable (frame released), never recomputed and never neutral-without-a-reason.
		TEST_METHOD(LaggingMonitorFrameInsideTheWindowIsServedOutsideIsVisiblyUnavailable)
		{
			ProviderGuard guard;
			const uint8_t keep = static_cast<uint8_t>(SourceResolutionMonitor::Monitor::kLatencyFrames);
			Assert::IsTrue(keep >= SourceResolutionMonitor::Monitor::kJobQueueCap + 1, L"the window covers the queue plus the frame in analysis");
			auto memo = std::make_shared<VpSkeleton::SkeletonMemo>();
			Assert::IsTrue(memo->RegisterOwnerFill(VpSkeleton::kCodingEvidence, VpSkeleton::AcquireForm::Cpu, CE::kUnitPath,
				CE::MakeOwnerFill(CE::Hub::Instance(), keep)) == VpSkeleton::SkeletonMemo::Registration::Registered, L"owner registered");
			CE::Hub::Instance().SetMemoProvider([memo](const void*) { return memo; });
			const std::vector<std::vector<float>> src = { Blocked(21), Clean(22) };
			std::vector<CE::TileMeasure> got;
			std::string why;
			memo->OnSeal(10 + keep);   // the renderer is `keep` frames past frame 10: the monitor is exactly as late as allowed
			Assert::IsTrue(CE::Hub::Instance().AcquireTiles(this, 10, PackOf(src), got, why), L"a frame inside the window is served");
			Assert::AreEqual(size_t(2), got.size());
			memo->OnSeal(11 + keep);   // one frame later it is outside
			std::vector<CE::TileMeasure> late;
			std::string lateWhy;
			Assert::IsFalse(CE::Hub::Instance().AcquireTiles(this, 9, PackOf(src), late, lateWhy), L"a frame outside the window is refused");
			Assert::IsTrue(late.empty(), L"no tiles are invented");
			Assert::IsTrue(lateWhy == VpSkeleton::Token(VpSkeleton::AcquireCause::FrameReleased), L"the reason is the visible token, not silence");
		}

		// h3-latch distribution: with the coding evidence not acquired the distribution change is UNKNOWN, never "moved". The old
		// behaviour (an empty distribution reads as moved) is kept for a distribution that is empty for other reasons, so it is compared.
		TEST_METHOD(UnavailableDistributionIsUnknownNeverMoved)
		{
			auto pattern = [](bool ramp) {
				std::vector<double> p(16);
				for (int i = 0; i < 16; ++i)
				{
					const double u = (i - 7.5) / 7.5;
					p[size_t(i)] = ramp ? 0.3 + 0.4 * double(i) / 15.0 : 0.35 + 0.4 * u * u;   // zero linear correlation: decorrelation 1
				}
				return p;
			};
			auto frame = [&](bool ramp, const std::vector<double>& distribution, bool unavailable, double mean) {
				SourceResolution::FrameReading f;
				f.tiles = 16;
				f.pattern = pattern(ramp);
				f.patternFlat.assign(16, 0);
				f.distribution = distribution;
				f.distributionUnavailable = unavailable;
				f.mean = mean;
				f.signature = mean;
				return f;
			};
			std::vector<double> low(32, 0.0), high(32, 0.0), same(32, 1.0 / 32.0);
			low[4] = 1.0;
			high[24] = 1.0;
			auto moments = [&](const std::vector<double>& before, const std::vector<double>& after, bool unavailable) {
				SourceResolution::Tracker::Settings st;
				st.latchFixes = true;
				st.cutFixes = true;
				SourceResolution::Tracker t(st);
				t.Update(frame(true, before, unavailable, 0.5), 0.0, false);
				t.Update(frame(false, after, unavailable, 0.486), 0.04, false);
				return t.CutMoments();
			};
			Assert::AreEqual(0ULL, moments(same, same, false), L"measured, distribution unchanged: no cut (the pan guard)");
			Assert::AreEqual(1ULL, moments(low, high, false), L"measured, distribution moved: a cut");
			Assert::AreEqual(1ULL, moments({}, {}, false), L"the old reading of an empty distribution: counted as moved");
			Assert::AreEqual(0ULL, moments({}, {}, true), L"coding unavailable: unknown, never moved, no silent flip");
			Assert::AreEqual(0ULL, moments(low, high, true), L"coding unavailable on either side: unknown");
			// The tracker counts the unknown judgements and ReadFrame carries the flag.
			SourceResolution::Tracker::Settings st;
			SourceResolution::Tracker t(st);
			t.Update(frame(true, {}, true, 0.5), 0.0, false);
			t.Update(frame(false, {}, true, 0.486), 0.04, false);
			Assert::IsTrue(t.DistributionUnavailableSteps() >= 1, L"the unknown judgement is counted");
			SourceResolution::Thresholds th;
			th.codingUnavailable = true;
			Assert::IsTrue(SourceResolution::ReadFrame(std::vector<SourceResolution::Spectrum>(), th).distributionUnavailable, L"ReadFrame carries it");
			// ... and it is shown, never silent.
			SourceResolutionMonitor::Snapshot snap;
			snap.reading = true;
			snap.codingUnavailable = true;
			snap.codingWhy = "no skeleton memo";
			const std::string text = SourceResolutionMonitor::Describe(snap, 1.0, false);
			Assert::IsTrue(text.find("source latch: distribution unavailable") != std::string::npos, L"the visible flag");
		}

		TEST_METHOD(StagingIsBounded)
		{
			CE::Hub hub;
			for (uint64_t s = 1; s <= 40; ++s)
				hub.Stage(s, PackOf({ Clean(1) }));
			Assert::IsTrue(hub.Staged(1) == nullptr && hub.Staged(40) != nullptr, L"the oldest are dropped");
		}

		// audit-P16 MUST 1: the no-store fallback calls the OWNER (MeasurePack, the owner fill's own measure), is counted, and its
		// result equals the pre-P16 per-tile measure (MeasureTile per tile) and the memo product.
		TEST_METHOD(NoStoreFallbackIsTheOwnersMeasureAndIsCounted)
		{
			CE::Hub hub;   // a private hub: the process-wide one is untouched
			const std::vector<std::vector<float>> src = { Clean(12), Blocked(13), Clean(14) };
			std::vector<CE::TileMeasure> got;
			Assert::AreEqual(0ull, hub.MeasuredWithoutMemo(), L"nothing counted yet");
			Assert::IsTrue(hub.MeasureWithoutMemo(*PackOf(src), got), L"measured by the owner");
			Assert::AreEqual(size_t(3), got.size());
			for (size_t i = 0; i < src.size(); ++i)
				Assert::IsTrue(Same(got[i], CE::MeasureTile(src[i].data(), kN, kN, 1023)), L"equals the pre-P16 per-tile measure");
			Assert::AreEqual(1ull, hub.MeasuredWithoutMemo(), L"counted");
			auto memo = std::make_shared<VpSkeleton::SkeletonMemo>();
			Assert::IsTrue(memo->RegisterOwnerFill(VpSkeleton::kCodingEvidence, VpSkeleton::AcquireForm::Cpu, CE::kUnitPath,
				CE::MakeOwnerFill(hub, 0)) == VpSkeleton::SkeletonMemo::Registration::Registered, L"owner registered");
			hub.SetMemoProvider([memo](const void*) { return memo; });
			std::vector<CE::TileMeasure> viaMemo;
			std::string why;
			Assert::IsTrue(hub.AcquireTiles(this, 21, PackOf(src), viaMemo, why), L"acquired");
			Assert::AreEqual(got.size(), viaMemo.size());
			for (size_t i = 0; i < got.size(); ++i)
				Assert::IsTrue(Same(got[i], viaMemo[i]), L"the fallback equals the memo product");
			Assert::AreEqual(1ull, hub.MeasuredWithoutMemo(), L"an acquire is not a fallback");
			CE::TilePack bad;
			bad.tiles = 2;
			bad.tileSize = kN;   // no pixels
			Assert::IsFalse(hub.MeasureWithoutMemo(bad, got), L"a malformed pack is refused");
			Assert::AreEqual(1ull, hub.MeasuredWithoutMemo(), L"and not counted");
		}

		// audit-P16 MUST 1: a monitor with no renderer store takes the fallback and SHOWS it (snapshot flag, count, Describe line).
		TEST_METHOD(StandaloneMonitorShowsTheNoStoreFallback)
		{
			Assert::IsTrue(VpFrameAnalysis::SourceFrameTap::StoreOwners().empty(), L"precondition: no renderer store registered in this process");
			const unsigned long long before = CE::Hub::Instance().MeasuredWithoutMemo();
			SourceResolutionMonitor::Monitor m;
			const int T = SourceResolutionMonitor::Monitor::kTile;
			std::mt19937 rng(5);
			std::normal_distribution<double> n(0.5, 0.12);
			for (int f = 0; f < 2; ++f)
			{
				std::vector<float> tiles(size_t(8) * T * T);
				for (float& v : tiles) v = static_cast<float>(std::round((std::min)(1.0, (std::max)(0.0, n(rng))) * 1023.0) / 1023.0);
				m.AnalyseTilesForTest(tiles, 8, 0.04 * f, 1920, 1080);
			}
			const SourceResolutionMonitor::Snapshot s = m.Read();
			Assert::IsTrue(s.codingWithoutMemo, L"the latest frame took the fallback");
			Assert::AreEqual(2ull, s.codingWithoutMemoFrames, L"both frames counted");
			Assert::IsFalse(s.codingUnavailable, L"evidence present (as before P16)");
			Assert::IsTrue(CE::Hub::Instance().MeasuredWithoutMemo() >= before + 2, L"counted at the owner");
			const std::string text = SourceResolutionMonitor::Describe(s, 1.0, false);
			Assert::IsTrue(text.find("coding evidence measured by its owner without a memo") != std::string::npos, L"shown, never silent");
		}

		// audit-P16 SHOULD 2: a module's destructor clears only its own provider; a newer module's stays installed.
		TEST_METHOD(ClearingAnOlderProviderKeepsTheNewer)
		{
			CE::Hub hub;
			auto memo = std::make_shared<VpSkeleton::SkeletonMemo>();
			Assert::IsTrue(memo->RegisterOwnerFill(VpSkeleton::kCodingEvidence, VpSkeleton::AcquireForm::Cpu, CE::kUnitPath,
				CE::MakeOwnerFill(hub, 0)) == VpSkeleton::SkeletonMemo::Registration::Registered, L"owner registered");
			const uint64_t older = hub.SetMemoProvider([](const void*) { return std::shared_ptr<VpSkeleton::SkeletonMemo>(); });
			const uint64_t newer = hub.SetMemoProvider([memo](const void*) { return memo; });
			Assert::IsTrue(older != 0 && newer != 0 && older != newer, L"distinct tokens");
			hub.ClearMemoProvider(older);   // the older module's destructor
			std::vector<CE::TileMeasure> got;
			std::string why;
			Assert::IsTrue(hub.AcquireTiles(this, 3, PackOf({ Clean(15) }), got, why), L"the newer provider is kept");
			hub.ClearMemoProvider(newer);
			Assert::IsFalse(hub.AcquireTiles(this, 4, PackOf({ Clean(15) }), got, why), L"its own clear removes it");
			Assert::IsTrue(why == "no skeleton memo", L"visible reason");
			Assert::AreEqual(uint64_t(0), hub.SetMemoProvider(nullptr), L"no provider, no token");
		}

		// audit-P16 SHOULD 1: the clear (in ~SkeletonModule) waits for a provider call already running, so the lambda's module is never
		// used after its destructor has passed the clear.
		TEST_METHOD(ClearWaitsForAnInFlightProviderCall)
		{
			CE::Hub hub;
			std::atomic<bool> inCall{ false }, release{ false }, cleared{ false };
			const uint64_t token = hub.SetMemoProvider([&](const void*) {
				inCall.store(true);
				const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
				while (!release.load() && std::chrono::steady_clock::now() < until)
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				return std::shared_ptr<VpSkeleton::SkeletonMemo>();
			});
			const std::shared_ptr<CE::TilePack> pack = PackOf({ Clean(16) });
			std::thread reader([&hub, &pack, this]() {
				std::vector<CE::TileMeasure> got;
				std::string why;
				hub.AcquireTiles(this, 5, pack, got, why);
			});
			const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
			while (!inCall.load() && std::chrono::steady_clock::now() < until)
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			const bool started = inCall.load();
			std::thread clearer([&hub, &cleared, token]() {
				hub.ClearMemoProvider(token);
				cleared.store(true);
			});
			std::this_thread::sleep_for(std::chrono::milliseconds(100));   // the clear has had time to return if it did not wait
			const bool clearedDuringCall = cleared.load();
			release.store(true);
			reader.join();
			clearer.join();
			Assert::IsTrue(started, L"the provider call was running");
			Assert::IsFalse(clearedDuringCall, L"the clear waited for the running call");
			Assert::IsTrue(cleared.load(), L"and returned once it finished");
			std::vector<CE::TileMeasure> got;
			std::string why;
			Assert::IsFalse(hub.AcquireTiles(this, 6, pack, got, why), L"no call after the clear");
		}

		// 15d J2 / L#8: P16 CodingEvidence owner fill records ComputeOnce "U13" once per computed frame,
		// does not recount on memo reuse or declined fills, and catches real duplicates when memo is bypassed.
		TEST_METHOD(OwnerFillRecordsComputeOnceU13OncePerFrameAndNotOnReuseOrDecline)
		{
			ComputeOnceScope computeOnceScope;
			CE::Hub hub;
			auto memo = std::make_shared<VpSkeleton::SkeletonMemo>();
			const VpSkeleton::OwnerFill fill = CE::MakeOwnerFill(hub, 0);
			Assert::IsTrue(
				memo->RegisterOwnerFill(VpSkeleton::kCodingEvidence, VpSkeleton::AcquireForm::Cpu, CE::kUnitPath, fill)
					== VpSkeleton::SkeletonMemo::Registration::Registered,
				L"owner registered");
			hub.SetMemoProvider([memo](const void*) { return memo; });

			const std::string expectedVariant = ComputeOnce::detail::HexVariant(ComputeOnce::Variant(0));
			auto checkU13 = [&](uint64_t expectedCalls, uint64_t expectedFrames, uint64_t expectedDupFrames, uint32_t expectedMaxPerFrame, const wchar_t* phase) {
				const std::vector<ComputeOnce::UnitRow> rows = ComputeOnce::Global().Rows();
				const std::vector<ComputeOnce::GapRow> gaps = ComputeOnce::Global().Gaps();
				Assert::AreEqual<size_t>(0, gaps.size(), phase);
				size_t u13Rows = 0;
				const ComputeOnce::UnitRow* u13 = nullptr;
				for (const ComputeOnce::UnitRow& r : rows)
				{
					if (r.unit == "U13")
					{
						++u13Rows;
						if (r.variant == expectedVariant)
							u13 = &r;
					}
				}
				Assert::AreEqual<size_t>(1, u13Rows, phase);
				Assert::IsNotNull(u13, phase);
				Assert::AreEqual<uint64_t>(expectedCalls, u13->calls, phase);
				Assert::AreEqual<uint64_t>(expectedFrames, u13->frames, phase);
				Assert::AreEqual<uint64_t>(expectedDupFrames, u13->dupFrames, phase);
				Assert::AreEqual<uint32_t>(expectedMaxPerFrame, u13->maxPerFrame, phase);
				if (expectedDupFrames > 0)
					Assert::IsFalse(u13->dupCallers.empty(), phase);
				else
					Assert::IsTrue(u13->dupCallers.empty(), phase);
			};

			// 1) First acquire for frame seq = 10 computes once through SkeletonMemo -> U13 calls=1, frames=1, dupFrames=0, gaps=0.
			const std::shared_ptr<CE::TilePack> pack10 = PackOf({ Clean(30), Blocked(31) });
			std::vector<CE::TileMeasure> got10;
			std::string why;
			Assert::IsTrue(hub.AcquireTiles(this, 10, pack10, got10, why), L"first acquire for seq=10 succeeds");
			Assert::AreEqual<size_t>(2, got10.size(), L"seq=10 measured both tiles");
			{
				const VpSkeleton::AcquireCounters c = memo->CountersOf(VpSkeleton::kCodingEvidence);
				Assert::IsTrue(c.ownerFillCalls == 1 && c.reusedMemo == 0, L"seq=10 ran owner fill once");
			}
			checkU13(1, 1, 0, 1, L"after first acquire of seq=10");

			// 2) Second acquire for the SAME frame seq = 10 hits the memo cache -> U13 stays calls=1 (zero double-compute).
			std::vector<CE::TileMeasure> again10;
			Assert::IsTrue(hub.AcquireTiles(this, 10, pack10, again10, why), L"second acquire for seq=10 reuses memo");
			Assert::AreEqual<size_t>(2, again10.size(), L"seq=10 reused tiles");
			{
				const VpSkeleton::AcquireCounters c = memo->CountersOf(VpSkeleton::kCodingEvidence);
				Assert::IsTrue(c.ownerFillCalls == 1 && c.reusedMemo == 1, L"seq=10 second acquire was a memo reuse");
			}
			checkU13(1, 1, 0, 1, L"after memo cache hit on seq=10");

			// 3) Distinct frame seq = 11 computes once -> U13 advances to calls=2, frames=2, dupFrames=0.
			const std::shared_ptr<CE::TilePack> pack11 = PackOf({ Clean(32), Blocked(33) });
			std::vector<CE::TileMeasure> got11;
			Assert::IsTrue(hub.AcquireTiles(this, 11, pack11, got11, why), L"acquire for distinct seq=11 succeeds");
			Assert::AreEqual<size_t>(2, got11.size(), L"seq=11 measured both tiles");
			{
				const VpSkeleton::AcquireCounters c = memo->CountersOf(VpSkeleton::kCodingEvidence);
				Assert::IsTrue(c.ownerFillCalls == 2 && c.reusedMemo == 1, L"seq=11 ran owner fill once");
			}
			checkU13(2, 2, 0, 1, L"after acquire of distinct frame seq=11");

			// 4) Rule 3 negative control A (declined fills): unstaged seq = 12 and malformed seq = 13 decline visibly and do NOT count U13.
			Assert::IsTrue(hub.Staged(12) == nullptr, L"precondition: nothing staged for seq=12");
			const VpSkeleton::AcquireResult declined = memo->Acquire(VpSkeleton::kCodingEvidence, 12, VpSkeleton::AcquireForm::Cpu);
			Assert::IsFalse(declined.Ok(), L"seq=12 with no staged pack is unavailable");
			Assert::IsTrue(declined.status == VpSkeleton::AcquireStatus::Unavailable && declined.cause == VpSkeleton::AcquireCause::OwnerDeclined,
				L"seq=12 declined visibly as OwnerDeclined");
			checkU13(2, 2, 0, 1, L"after declined fill on unstaged seq=12");

			auto badPack = std::make_shared<CE::TilePack>();
			badPack->tiles = 2;
			badPack->tileSize = kN;   // missing pixels: MeasurePack refuses
			std::vector<CE::TileMeasure> gotBad;
			Assert::IsFalse(hub.AcquireTiles(this, 13, badPack, gotBad, why), L"malformed pack for seq=13 declines");
			Assert::IsTrue(why == VpSkeleton::Token(VpSkeleton::AcquireCause::OwnerDeclined), L"malformed pack declines visibly as OwnerDeclined");
			checkU13(2, 2, 0, 1, L"after declined fill on malformed seq=13");

			// 5) Rule 3 negative control B (memo bypass on exact same seq=10 input): direct second fill call triggers U13 duplicate detection.
			hub.Stage(10, pack10);
			VpSkeleton::OwnerFillRequest dupReq;
			dupReq.id = VpSkeleton::kCodingEvidence;
			dupReq.seq = 10;
			dupReq.form = VpSkeleton::AcquireForm::Cpu;
			VpSkeleton::OwnerFillOutput dupOut;
			const bool dupFilled = fill.fill(dupReq, *memo, dupOut);
			hub.Unstage(10);
			Assert::IsTrue(dupFilled, L"direct bypass fill for staged seq=10 succeeded");
			Assert::IsFalse(dupOut.bytes.empty(), L"direct bypass fill produced bytes");
			checkU13(3, 2, 1, 2, L"after memo-bypass duplicate fill on seq=10");
		}

		// S4-M4: GridBlockiness scales its quarter-axis-code noise floor by axisScale (1 at <= 10-bit, 4 at 12-bit)
		// so 12-bit coding grids match 10-bit scale-invariantly instead of suffering a 4x floor bias.
		TEST_METHOD(GridBlockiness12BitMatches10BitWithAxisScaleFloor)
		{
			// 1) Direct GridBlockiness scale invariance on subtle 8-px periodic steps:
			// 10-bit boundary = 0.5, inner = 0.1 -> exact analytical ratio (0.1 + 0.4 + 0.25) / (0.1 + 0.25) = 15 / 7.
			// 12-bit (axisScale = 4) boundary = 2.0, inner = 0.4 -> exact analytical ratio (0.4 + 1.6 + 1.0) / (0.4 + 1.0) = 15 / 7.
			constexpr int kAxisScale10 = 1;
			constexpr int kAxisScale12 = 4;
			const size_t stepCount = static_cast<size_t>(kFrameTile - 1);
			std::vector<double> steps10(stepCount, 0.1);
			std::vector<double> steps12(stepCount, 0.4);
			for (size_t x = 7; x < stepCount; x += 8)
			{
				steps10[x] = 0.5;
				steps12[x] = 2.0;
			}

			double period10 = 0.0;
			double period12 = 0.0;
			const double grid10 = CE::GridBlockiness(steps10, period10, kAxisScale10);
			const double grid12 = CE::GridBlockiness(steps12, period12, kAxisScale12);
			const double expectedSubtleRatio = 15.0 / 7.0;
			Assert::AreEqual(8.0, period10, 1e-12, L"10-bit subtle 8-px grid period");
			Assert::AreEqual(8.0, period12, 1e-12, L"12-bit subtle 8-px grid period");
			Assert::AreEqual(expectedSubtleRatio, grid10, 1e-12, L"10-bit GridBlockiness matches analytical quarter-code floor ratio");
			Assert::AreEqual(grid10, grid12, 1e-12, L"12-bit GridBlockiness with axisScale=4 is scale-invariant against 10-bit");

			// Contract check: default axisScale and non-positive axisScale clamp to 1.
			double periodDefault = 0.0;
			double periodClamped = 0.0;
			const double gridDefault = CE::GridBlockiness(steps10, periodDefault);
			const double gridClamped = CE::GridBlockiness(steps10, periodClamped, 0);
			Assert::AreEqual(grid10, gridDefault, 1e-12, L"default axisScale matches axisScale=1");
			Assert::AreEqual(grid10, gridClamped, 1e-12, L"non-positive axisScale clamps to 1");
			Assert::AreEqual(8.0, periodDefault, 1e-12, L"default axisScale period");
			Assert::AreEqual(8.0, periodClamped, 1e-12, L"clamped axisScale period");

			// Rule 3 same-input negative control A (subtle grid): pre-fix unscaled axisScale=1 on the exact same 12-bit steps12
			// inflates ratio to (0.4 + 1.6 + 0.25) / (0.4 + 0.25) = 45 / 13 (~3.462 vs ~2.143: +1.319 bias, >2x excess over 1.0).
			double periodNaive12 = 0.0;
			const double gridNaive12 = CE::GridBlockiness(steps12, periodNaive12, 1);
			Assert::AreEqual(8.0, periodNaive12, 1e-12, L"unscaled 12-bit still locks onto the 8-px period");
			Assert::AreEqual(45.0 / 13.0, gridNaive12, 1e-12, L"unscaled 12-bit matches pre-fix unscaled-floor bias formula");
			Assert::IsTrue(gridNaive12 - grid10 > 0.3, L"same-input negative control: unscaled 12-bit diverges > 0.3 above 10-bit");
			Assert::IsTrue((gridNaive12 - 1.0) > 2.0 * (grid10 - 1.0),
				L"same-input negative control: unscaled 12-bit inflates blockiness excess by > 2x");

			// Rule 3 same-input negative control B (quarter-code noise-floor regime): inner=0, boundary=0.25 (10-bit) / 1.0 (12-bit).
			// Scaled 12-bit matches 10-bit (2.0), whereas pre-fix axisScale=1 inflates excess by exact 4x (5.0 vs 2.0).
			std::vector<double> flatSteps10(stepCount, 0.0);
			std::vector<double> flatSteps12(stepCount, 0.0);
			for (size_t x = 7; x < stepCount; x += 8)
			{
				flatSteps10[x] = 0.25;
				flatSteps12[x] = 1.0;
			}
			double periodFlat10 = 0.0;
			double periodFlat12 = 0.0;
			double periodFlatNaive12 = 0.0;
			const double flatGrid10 = CE::GridBlockiness(flatSteps10, periodFlat10, kAxisScale10);
			const double flatGrid12 = CE::GridBlockiness(flatSteps12, periodFlat12, kAxisScale12);
			const double flatGridNaive12 = CE::GridBlockiness(flatSteps12, periodFlatNaive12, 1);
			Assert::AreEqual(8.0, periodFlat10, 1e-12, L"10-bit quarter-code boundary period");
			Assert::AreEqual(8.0, periodFlat12, 1e-12, L"12-bit quarter-axis-code boundary period");
			Assert::AreEqual(8.0, periodFlatNaive12, 1e-12, L"unscaled 12-bit quarter-axis-code boundary period");
			Assert::AreEqual(2.0, flatGrid10, 1e-12, L"10-bit quarter-code boundary reads 2.0");
			Assert::AreEqual(flatGrid10, flatGrid12, 1e-12, L"12-bit quarter-axis-code boundary matches 10-bit");
			Assert::AreEqual(5.0, flatGridNaive12, 1e-12, L"pre-fix unscaled 12-bit inflates quarter-axis-code excess by exact 4x");
			Assert::AreEqual(4.0, (flatGridNaive12 - 1.0) / (flatGrid10 - 1.0), 1e-12,
				L"same-input negative control: exact 4x floor-bias ratio in noise-floor regime");

			// 2) End-to-end MeasureTile on the exact same normalized float 128x128 luma tile at 10-bit (1023) vs 12-bit (4095).
			// Codes 420 and 421 map to exact 10-bit {420, 421} (step 1) and exact 12-bit {1681, 1685} (step 4, zero rounding jitter):
			// - Rows 0..63: alternating 8-px block offset -> boundary step 64/128 = 0.5 (10-bit) / 2.0 (12-bit).
			// - Rows 64..112: 7 inner-transition patterns x 7 contiguous rows (spacing 7 -> no vertical grid at P >= 8)
			//   hitting every inner transition u=0..6 twice per cycle -> inner step 14/128 = 7/64 (10-bit) / 28/64 (12-bit).
			// - Rows 113..127: flat base level.
			// Analytical blockinessRobust = (7/64 + 25/64 + 16/64) / (7/64 + 16/64) = 48/23 (~2.087);
			// pre-fix unscaled 12-bit would read (28/64 + 100/64 + 16/64) / (28/64 + 16/64) = 36/11 (~3.273).
			constexpr int kBaseCode10 = 420;
			std::vector<float> tile(static_cast<size_t>(kFrameTile) * kFrameTile, static_cast<float>(kBaseCode10) / 1023.0f);
			const float hiLuma = static_cast<float>(kBaseCode10 + 1) / 1023.0f;
			for (int y = 0; y < 64; ++y)
			{
				for (int x = 0; x < kFrameTile; ++x)
				{
					if (((x / 8) % 2) != 0)
						tile[static_cast<size_t>(y) * kFrameTile + static_cast<size_t>(x)] = hiLuma;
				}
			}
			for (int p = 0; p < 7; ++p)
			{
				const int yStart = 64 + p * 7;
				for (int dy = 0; dy < 7; ++dy)
				{
					const int y = yStart + dy;
					for (int x = 0; x < kFrameTile; ++x)
					{
						const int u = x % 8;
						const bool raised = (p < 6) ? (u == p + 1) : (u >= 1 && u <= 6);
						if (raised)
							tile[static_cast<size_t>(y) * kFrameTile + static_cast<size_t>(x)] = hiLuma;
					}
				}
			}

			const CE::TileMeasure m10 = CE::MeasureTile(tile.data(), kFrameTile, kFrameTile, 1023);
			const CE::TileMeasure m12 = CE::MeasureTile(tile.data(), kFrameTile, kFrameTile, 4095);
			const double expectedTileRatio = 48.0 / 23.0;
			Assert::AreEqual(8.0, m10.blockPeriod, 1e-12, L"10-bit MeasureTile detects 8-px block period");
			Assert::AreEqual(8.0, m12.blockPeriod, 1e-12, L"12-bit MeasureTile detects 8-px block period");
			Assert::AreEqual(expectedTileRatio, m10.blockinessRobust, 1e-12, L"10-bit MeasureTile blockinessRobust matches analytical value");
			Assert::AreEqual(m10.blockinessRobust, m12.blockinessRobust, 1e-12,
				L"12-bit MeasureTile blockinessRobust matches 10-bit without 4x floor bias");
			Assert::AreEqual(m10.blockiness, m12.blockiness, 1e-12, L"12-bit MeasureTile fixed blockiness matches 10-bit");

			// Rule 3 same-input negative control C (on MeasureTile's exact 12-bit horizontal steps):
			std::vector<double> tileStepsH12(stepCount, 0.0);
			for (int y = 0; y < kFrameTile; ++y)
			{
				for (int x = 0; x + 1 < kFrameTile; ++x)
				{
					const int c0 = static_cast<int>(std::lround(tile[static_cast<size_t>(y) * kFrameTile + static_cast<size_t>(x)] * 4095.0f));
					const int c1 = static_cast<int>(std::lround(tile[static_cast<size_t>(y) * kFrameTile + static_cast<size_t>(x + 1)] * 4095.0f));
					tileStepsH12[static_cast<size_t>(x)] += std::fabs(static_cast<double>(c1 - c0));
				}
			}
			for (double& s : tileStepsH12) s /= static_cast<double>(kFrameTile);
			double periodTile12Fixed = 0.0;
			double periodTile12Naive = 0.0;
			const double tileGrid12Fixed = CE::GridBlockiness(tileStepsH12, periodTile12Fixed, kAxisScale12);
			const double tileGrid12Naive = CE::GridBlockiness(tileStepsH12, periodTile12Naive, 1);
			Assert::AreEqual(8.0, periodTile12Fixed, 1e-12, L"reconstructed 12-bit tile steps period");
			Assert::AreEqual(8.0, periodTile12Naive, 1e-12, L"unscaled 12-bit tile steps period");
			Assert::AreEqual(m12.blockinessRobust, tileGrid12Fixed, 1e-12, L"reconstructed 12-bit tile steps match MeasureTile");
			Assert::AreEqual(36.0 / 11.0, tileGrid12Naive, 1e-12, L"pre-fix unscaled 12-bit tile steps read inflated 36/11");
			Assert::IsTrue(tileGrid12Naive - m10.blockinessRobust > 0.3,
				L"same-input negative control: pre-fix unscaled 12-bit MeasureTile steps diverge > 0.3 above 10-bit");
			Assert::IsTrue((tileGrid12Naive - 1.0) > 2.0 * (m10.blockinessRobust - 1.0),
				L"same-input negative control: pre-fix unscaled 12-bit MeasureTile excess is > 2x inflated");
		}
	};
}
