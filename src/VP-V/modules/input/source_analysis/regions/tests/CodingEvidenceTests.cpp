// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// input.source_analysis/regions: P16 kCodingEvidence (lead8/iface-P16.md). The moved tile measure and frame reduction, the product's
// bytes (round trip, corrupt input refused), the owner fill through a real SkeletonMemo (computed once, equal to the direct measure),
// and the unavailable paths (no memo, nothing staged) that consumers show instead of measuring privately. CPU only.
#include "pch.h"
#include "CppUnitTest.h"

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
	};
}
