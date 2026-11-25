// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only

// O#5 (M1-M4): Red Team adversarial tests for SRM migration to shared U7 BlankOf/SceneOf and P4 NormalisedMean.
// Pure CPU; verifies:
//   1. CutFadeJudge::BlankOf and SourceStats::NormalisedMean on independent synthetic half-planes.
//   2. SRM worker Analyse and LookAheadFrame prefer shared U7 & P4 over private tile heuristics (with same-input negative controls).
//   3. Unavailable shared U7 / P4 fall back visibly in Snapshot fields and Describe() (zero silent fallback).
//   4. End-to-end default reader integration through ExtensionHub IExternalDemand::SceneOf + MemoFor(kP4SourceStats) + SourceFrameTap.

#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/core/FrameAnalysisStore.h>
#include <VP-V/core/Products.h>
#include <VP-V/core/SkeletonAcquire.h>
#include <VP-V/core/hub/ExtensionHub.h>
#include <VP-V/modules/control/skeleton/SceneRecordReader.h>
#include <VP-V/modules/control/skeleton/SkeletonFacts.h>
#include <VP-V/modules/input/source_analysis/SourceFrameTap.h>
#include <VP-V/modules/input/source_analysis/SourceResolutionMonitor.h>
#include <VP-V/modules/input/source_analysis/events/shared/CutFadeJudge.h>
#include <VP-V/modules/input/source_analysis/lane/shared/SourceStats.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace
{
	using SourceResolutionMonitor::Monitor;
	using SourceResolutionMonitor::Snapshot;

	constexpr double kBlackNorm = 64.0 / 1023.0;

	// Generates 8 informative high-contrast noise tiles (mean ~0.50, never blank/black locally).
	std::vector<float> TexturedTiles(unsigned seed)
	{
		std::mt19937 rng(seed);
		std::normal_distribution<double> dist(0.50, 0.14);
		std::vector<float> tiles(static_cast<size_t>(8) * Monitor::kTile * Monitor::kTile);
		for (float& v : tiles)
		{
			const double c = (std::min)(1.0, (std::max)(0.0, dist(rng)));
			v = static_cast<float>(std::round(c * 1023.0) / 1023.0);
		}
		return tiles;
	}

	VpUnit::SourceStats::Record ComputeSyntheticP4(const std::vector<uint16_t>& half, uint32_t hw, uint32_t hh)
	{
		std::vector<uint8_t> bytes(VpUnit::SourceStats::kRecordBytes, 0);
		Assert::IsTrue(VpUnit::SourceStats::ComputeRecord(half.data(), hw, hh, bytes.data(), bytes.size()));
		VpUnit::SourceStats::Record rec;
		Assert::IsTrue(VpUnit::SourceStats::ReadRecord(bytes.data(), bytes.size(), &rec));
		return rec;
	}

	class FakeSrmSkeletonDemand : public Vpv::Skeleton::IExternalDemand
	{
	public:
		FakeSrmSkeletonDemand(const void* expectedHost, std::shared_ptr<VpSkeleton::SkeletonMemo> memoIn)
			: host_(expectedHost), memo_(std::move(memoIn))
		{
		}

		void SetExternalDemand(const void*, const void*, const std::vector<VpSkeleton::ConsumerDecl>&,
			const std::vector<VpSkeleton::ProductId>&) override {}
		void ClearExternalDemand(const void*, const void*) override {}

		void SetSceneAnswer(const Vpv::Skeleton::SceneAnswer& ans) { answer_ = ans; }

		Vpv::Skeleton::SceneAnswer SceneOf(const void* host, uint64_t) const override
		{
			if (host == host_)
				return answer_;
			Vpv::Skeleton::SceneAnswer miss;
			miss.known = false;
			miss.why = "test: unknown host";
			return miss;
		}

		Vpv::Skeleton::FrameMemo MemoFor(const void* host, uint64_t seq) override
		{
			Vpv::Skeleton::FrameMemo fm;
			if (host == host_)
			{
				fm.memo = memo_;
				fm.host = host;
				fm.seq = seq;
			}
			return fm;
		}

	private:
		const void* host_ = nullptr;
		std::shared_ptr<VpSkeleton::SkeletonMemo> memo_;
		Vpv::Skeleton::SceneAnswer answer_;
	};

	class FakeSrmDemandModule : public Vpv::Hub::ISkeletonModule
	{
	public:
		explicit FakeSrmDemandModule(FakeSrmSkeletonDemand* demandIn) : demand_(demandIn) {}
		const char* Name() const override { return "test.srm_u7_migration.demand"; }
		void* Query(const char* iid) override
		{
			return iid != nullptr && std::strcmp(iid, Vpv::Skeleton::kExternalDemandQueryId) == 0
				? static_cast<Vpv::Skeleton::IExternalDemand*>(demand_) : nullptr;
		}

	private:
		FakeSrmSkeletonDemand* demand_;
	};

	struct SrmHubScope
	{
		explicit SrmHubScope(FakeSrmSkeletonDemand* demand)
		{
			Vpv::Hub::ExtensionHub::Instance().ResetModulesForTesting();
			Vpv::Hub::ExtensionHub::Instance().AddModule(
				std::unique_ptr<Vpv::Hub::ISkeletonModule>(new FakeSrmDemandModule(demand)));
		}
		~SrmHubScope()
		{
			Vpv::Hub::ExtensionHub::Instance().ResetModulesForTesting();
		}
	};

	struct SrmTapOwnerScope
	{
		const void* owner = nullptr;
		SrmTapOwnerScope(const void* ownerIn, FrameAnalysis::Store* store) : owner(ownerIn)
		{
			VpFrameAnalysis::SourceFrameTap::SetStore(owner, store);
		}
		~SrmTapOwnerScope()
		{
			Clear();
		}
		void Clear()
		{
			if (owner != nullptr)
			{
				VpFrameAnalysis::SourceFrameTap::SetStore(owner, nullptr);
				owner = nullptr;
			}
		}
	};
}

namespace VideoProcessorTest
{
	TEST_CLASS(SourceResolutionU7MigrationTests)
	{
	public:
		// Test 1: CutFadeJudge::BlankOf and SourceStats::NormalisedMean on independent synthetic half-planes.
		TEST_METHOD(CutFadeJudgeBlankOfAndP4NormalisedMeanOnSyntheticHalfPlanes)
		{
			constexpr uint32_t kHw = 64, kHh = 36; // >= 32x18 so P4 cell grid is populated (gridFound == 1)

			// 1. Flat studio black (64 << 6).
			const std::vector<uint16_t> flatBlack(static_cast<size_t>(kHw) * kHh, static_cast<uint16_t>(64u << 6));
			const VpUnit::SourceStats::Record blackRec = ComputeSyntheticP4(flatBlack, kHw, kHh);
			Assert::AreEqual(1u, blackRec.gridFound);
			const VpUnit::CutFadeJudge::BlankVerdict blackBv = VpUnit::CutFadeJudge::BlankOf(blackRec);
			Assert::IsTrue(blackBv.flat, L"Flat studio black must be flat");
			Assert::IsTrue(blackBv.black, L"Flat studio black must be black");
			Assert::IsTrue(blackBv.blank, L"Flat studio black must be blank");
			Assert::AreEqual(64.0 / 1023.0, VpUnit::SourceStats::NormalisedMean(blackRec), 1e-9);

			// 2. Flat mid-grey (512 << 6).
			const std::vector<uint16_t> flatGrey(static_cast<size_t>(kHw) * kHh, static_cast<uint16_t>(512u << 6));
			const VpUnit::SourceStats::Record greyRec = ComputeSyntheticP4(flatGrey, kHw, kHh);
			Assert::AreEqual(1u, greyRec.gridFound);
			const VpUnit::CutFadeJudge::BlankVerdict greyBv = VpUnit::CutFadeJudge::BlankOf(greyRec);
			Assert::IsTrue(greyBv.flat, L"Flat mid-grey must be flat");
			Assert::IsFalse(greyBv.black, L"Flat mid-grey must NOT be black");
			Assert::IsTrue(greyBv.blank, L"Flat mid-grey must be blank");
			Assert::AreEqual(512.0 / 1023.0, VpUnit::SourceStats::NormalisedMean(greyRec), 1e-9);

			// 3. Textured high-contrast 4x4 checkerboard (alternating 128<<6 and 832<<6; mean = 480<<6).
			std::vector<uint16_t> checker(static_cast<size_t>(kHw) * kHh);
			for (uint32_t y = 0; y < kHh; ++y)
				for (uint32_t x = 0; x < kHw; ++x)
					checker[static_cast<size_t>(y) * kHw + x] =
						(((x / 4u) ^ (y / 4u)) & 1u) ? static_cast<uint16_t>(832u << 6) : static_cast<uint16_t>(128u << 6);
			const VpUnit::SourceStats::Record texRec = ComputeSyntheticP4(checker, kHw, kHh);
			Assert::AreEqual(1u, texRec.gridFound);
			const VpUnit::CutFadeJudge::BlankVerdict texBv = VpUnit::CutFadeJudge::BlankOf(texRec);
			Assert::IsFalse(texBv.flat, L"High-contrast checkerboard must NOT be flat");
			Assert::IsFalse(texBv.black, L"High-contrast checkerboard must NOT be black");
			Assert::IsFalse(texBv.blank, L"High-contrast checkerboard must NOT be blank");
			Assert::AreEqual(480.0 / 1023.0, VpUnit::SourceStats::NormalisedMean(texRec), 1e-9);

			// 4. Empty record guard.
			const VpUnit::SourceStats::Record emptyRec;
			const VpUnit::CutFadeJudge::BlankVerdict emptyBv = VpUnit::CutFadeJudge::BlankOf(emptyRec);
			Assert::IsFalse(emptyBv.blank);
			Assert::IsFalse(emptyBv.black);
			Assert::IsFalse(emptyBv.flat);
			Assert::AreEqual(0.0, VpUnit::SourceStats::NormalisedMean(emptyRec), 1e-12);
		}

		// Test 2: SRM worker Analyse & LookAheadFrame prefer shared U7 & P4 over private tile heuristics
		// with same-input negative controls proving the local fallback would disagree.
		TEST_METHOD(SrmUsesSharedU7AndP4OverPrivateTileHeuristics)
		{
			const std::vector<float> tiles = TexturedTiles(20260501u);

			// Negative control A: textured tiles (locally non-blank, mean ~0.50) paired with shared P4 black/blank.
			{
				Monitor sharedMon;
				sharedMon.SetSharedP4Reader([](uint64_t, Monitor::SharedP4Input& out, const char**) -> bool {
					out.blank = true;
					out.black = true;
					out.flat = true;
					out.mean = kBlackNorm;
					return true;
				});
				sharedMon.SetSharedSceneReader([](uint64_t, Monitor::SharedSceneInput& out, const char**) -> bool {
					out.cut = false;
					out.fade = false;
					out.fadeKeep = -1.0f;
					return true;
				});
				sharedMon.AnalyseTilesForTest(tiles, 8, 1.0, 1920, 1080, {}, {}, 8, Monitor::kTile, kBlackNorm, 10);
				const Snapshot sharedSnap = sharedMon.Read();
				Assert::IsTrue(sharedSnap.frameBlank, L"Shared P4 blank=true must override textured tiles");
				Assert::IsTrue(sharedSnap.frameBlack, L"Shared P4 black=true must override textured tiles");
				Assert::AreEqual<unsigned long long>(1, sharedSnap.p4StatsShared);
				Assert::AreEqual<unsigned long long>(0, sharedSnap.p4StatsFallback);
				Assert::AreEqual<unsigned long long>(1, sharedSnap.u7SceneShared);
				Assert::AreEqual<unsigned long long>(0, sharedSnap.u7SceneFallback);

				// Naive control on the EXACT same textured tiles without shared P4: local tile check says non-black/non-blank.
				Monitor naiveMon;
				naiveMon.SetSharedP4Reader([](uint64_t, Monitor::SharedP4Input&, const char** why) -> bool {
					if (why != nullptr) *why = "test: no shared p4";
					return false;
				});
				naiveMon.SetSharedSceneReader([](uint64_t, Monitor::SharedSceneInput&, const char** why) -> bool {
					if (why != nullptr) *why = "test: no shared u7";
					return false;
				});
				naiveMon.AnalyseTilesForTest(tiles, 8, 1.0, 1920, 1080, {}, {}, 8, Monitor::kTile, kBlackNorm, 10);
				const Snapshot naiveSnap = naiveMon.Read();
				Assert::IsFalse(naiveSnap.frameBlack, L"Negative control: naive local tile check must see textured tiles as non-black");
				Assert::IsFalse(naiveSnap.frameBlank, L"Negative control: naive local tile check must see textured tiles as non-blank");
				Assert::AreEqual<unsigned long long>(0, naiveSnap.p4StatsShared);
				Assert::IsTrue(naiveSnap.p4StatsFallback > 0);
			}

			// Negative control B: identical static textured frames paired with shared U7 cut=true, fade=true.
			{
				constexpr int kW = 1024, kH = 512;
				auto staticSampler = [](int x, int y) -> int
				{
					// High-contrast deterministic texture, 100% identical across frames (zero local change).
					const unsigned h = (static_cast<unsigned>(x) * 73856093u) ^ (static_cast<unsigned>(y) * 19349663u);
					return 200 + static_cast<int>((h >> 16) % 600u);
				};

				Snapshot lower;
				lower.reading = true;
				lower.nativeWidth = kW;
				lower.nativeHeight = kH;
				lower.declaredRatio = lower.liveRatio = lower.frameRatio = 0.5;
				lower.confidence = 0.9;
				lower.frames = 10;

				// Shared U7 monitor: static frame at seq 25 reports cut=true, fade=true via shared U7.
				Monitor u7Mon;
				u7Mon.SetSharedSceneReader([](uint64_t seq, Monitor::SharedSceneInput& out, const char**) -> bool {
					out.cut = (seq == 25);
					out.fade = (seq == 25);
					out.fadeKeep = (seq == 25) ? 0.80f : 1.0f;
					return true;
				});
				u7Mon.SetSharedP4Reader([](uint64_t, Monitor::SharedP4Input& out, const char**) -> bool {
					out.blank = false;
					out.black = false;
					out.flat = false;
					out.mean = 0.50;
					return true;
				});

				// Worker path: identical tiles at seq 24 and 25 -> frameFade comes from shared U7.
				u7Mon.AnalyseTilesForTest(tiles, 8, 1.0, kW, kH, {}, {}, 8, Monitor::kTile, kBlackNorm, 24);
				Assert::IsFalse(u7Mon.Read().frameFade);
				u7Mon.AnalyseTilesForTest(tiles, 8, 1.5, kW, kH, {}, {}, 8, Monitor::kTile, kBlackNorm, 25);
				const Snapshot fadeSnap = u7Mon.Read();
				Assert::IsTrue(fadeSnap.frameFade, L"Shared U7 fade=true must populate Snapshot::frameFade");
				Assert::AreEqual<unsigned long long>(2, fadeSnap.u7SceneShared);
				Assert::AreEqual<unsigned long long>(0, fadeSnap.u7SceneFallback);

				// LookAheadFrame path: put auto into pending state, then feed identical static frames 24 -> 25.
				u7Mon.InjectResultForTest(lower, 1);
				std::string why;
				u7Mon.AutoFactor(kH, 0.8, why, 2.0, 1);
				u7Mon.LookAheadFrame(24, kW, kH, staticSampler, 0, 0, kW, kH);
				u7Mon.LookAheadFrame(25, kW, kH, staticSampler, 0, 0, kW, kH);
				Assert::AreEqual<uint64_t>(25ull, u7Mon.Ahead().eventSequence,
					L"Shared U7 cut on seq 25 must arm LookAheadFrame eventSequence even on static pixels");

				// Naive negative control on the EXACT same static frames without shared U7: local look-ahead never arms an event!
				Monitor naiveMon;
				naiveMon.SetSharedSceneReader([](uint64_t, Monitor::SharedSceneInput&, const char** whyOut) -> bool {
					if (whyOut != nullptr) *whyOut = "test: no u7";
					return false;
				});
				naiveMon.SetSharedP4Reader([](uint64_t, Monitor::SharedP4Input&, const char** whyOut) -> bool {
					if (whyOut != nullptr) *whyOut = "test: no p4";
					return false;
				});
				naiveMon.InjectResultForTest(lower, 1);
				naiveMon.AutoFactor(kH, 0.8, why, 2.0, 1);
				naiveMon.LookAheadFrame(24, kW, kH, staticSampler, 0, 0, kW, kH);
				naiveMon.LookAheadFrame(25, kW, kH, staticSampler, 0, 0, kW, kH);
				Assert::AreEqual<uint64_t>(0ull, naiveMon.Ahead().eventSequence,
					L"Negative control: naive local look-ahead on identical static frames must NOT arm an event");
				Assert::IsTrue(naiveMon.Read().u7SceneFallback >= 2);
			}
		}

		// Test 3: Unavailable shared U7 / P4 fall back visibly in Snapshot and Describe() (zero silent fallback).
		TEST_METHOD(SrmSurfacesVisibleFallbackWhenSharedU7OrP4Unavailable)
		{
			const std::vector<float> tiles = TexturedTiles(20260502u);

			Monitor degraded;
			degraded.SetSharedSceneReader([](uint64_t, Monitor::SharedSceneInput&, const char** why) -> bool {
				if (why != nullptr) *why = "test: skeleton unavailable";
				return false;
			});
			degraded.SetSharedP4Reader([](uint64_t, Monitor::SharedP4Input&, const char** why) -> bool {
				if (why != nullptr) *why = "test: skeleton unavailable";
				return false;
			});
			degraded.AnalyseTilesForTest(tiles, 8, 1.0, 1920, 1080, {}, {}, 8, Monitor::kTile, kBlackNorm, 15);
			const Snapshot s = degraded.Read();
			Assert::AreEqual<unsigned long long>(1, s.u7SceneFallback);
			Assert::AreEqual<unsigned long long>(0, s.u7SceneShared);
			Assert::AreEqual(std::string("test: skeleton unavailable"), s.u7SceneFallbackWhy);
			Assert::AreEqual<unsigned long long>(1, s.p4StatsFallback);
			Assert::AreEqual<unsigned long long>(0, s.p4StatsShared);
			Assert::AreEqual(std::string("test: skeleton unavailable"), s.p4StatsFallbackWhy);
			Assert::IsFalse(s.frameFade);

			const std::string desc = SourceResolutionMonitor::Describe(s, 1.0, true);
			Assert::IsTrue(desc.find("U7 scene fallback 1 (test: skeleton unavailable)") != std::string::npos,
				L"Describe() must visibly surface U7 fallback count and cause");
			Assert::IsTrue(desc.find("P4 stats fallback 1 (test: skeleton unavailable)") != std::string::npos,
				L"Describe() must visibly surface P4 fallback count and cause");

			// Opposing control: when shared U7 & P4 succeed on the exact same tiles, Describe() shows zero fallback notes.
			Monitor healthy;
			healthy.SetSharedSceneReader([](uint64_t, Monitor::SharedSceneInput& out, const char**) -> bool {
				out.cut = false;
				out.fade = false;
				return true;
			});
			healthy.SetSharedP4Reader([](uint64_t, Monitor::SharedP4Input& out, const char**) -> bool {
				out.blank = false;
				out.black = false;
				out.flat = false;
				out.mean = 0.50;
				return true;
			});
			healthy.AnalyseTilesForTest(tiles, 8, 1.0, 1920, 1080, {}, {}, 8, Monitor::kTile, kBlackNorm, 15);
			const std::string healthyDesc = SourceResolutionMonitor::Describe(healthy.Read(), 1.0, true);
			Assert::IsTrue(healthyDesc.find("U7 scene fallback") == std::string::npos);
			Assert::IsTrue(healthyDesc.find("P4 stats fallback") == std::string::npos);
		}

		// Test 4: Default uninjected SRM readers integrate end-to-end with ExtensionHub IExternalDemand::SceneOf + MemoFor(kP4SourceStats).
		TEST_METHOD(SrmDefaultReaderIntegratesWithSkeletonSceneOfAndP4Memo)
		{
			const int ownerTag = 0;
			const void* const owner = &ownerTag;

			auto memo = std::make_shared<VpSkeleton::SkeletonMemo>();
			VpSkeleton::OwnerFill p4Fill;
			p4Fill.fill = [](const VpSkeleton::OwnerFillRequest&, VpSkeleton::SkeletonMemo&, VpSkeleton::OwnerFillOutput& out) -> bool {
				constexpr uint32_t kHw = 64, kHh = 36;
				const std::vector<uint16_t> flatBlack(static_cast<size_t>(kHw) * kHh, static_cast<uint16_t>(64u << 6));
				out.bytes.assign(VpUnit::SourceStats::kRecordBytes, 0);
				return VpUnit::SourceStats::ComputeRecord(flatBlack.data(), kHw, kHh, out.bytes.data(), out.bytes.size());
			};
			const std::string p4Owner = VpSkeleton::SkeletonMemo::OwnerUnitOf(VpSkeleton::kP4SourceStats);
			Assert::IsFalse(p4Owner.empty(), L"kP4SourceStats must have a catalog owner unit");
			Assert::IsTrue(
				memo->RegisterOwnerFill(VpSkeleton::kP4SourceStats, VpSkeleton::AcquireForm::Cpu, p4Owner, std::move(p4Fill))
				== VpSkeleton::SkeletonMemo::Registration::Registered);

			FakeSrmSkeletonDemand demand(owner, memo);
			Vpv::Skeleton::SceneAnswer sceneAns;
			sceneAns.known = true;
			sceneAns.cut = false;
			sceneAns.fade = true;
			sceneAns.fadeKeep = 0.72f;
			sceneAns.from = Vpv::Skeleton::SceneAnswer::From::Live;
			demand.SetSceneAnswer(sceneAns);

			SrmHubScope hub(&demand);
			FrameAnalysis::Store store(8);
			SrmTapOwnerScope tap(owner, &store);

			// Verify direct SceneRecordReader facades first.
			{
				VpUnit::SourceStats::Record p4Rec;
				const char* p4Why = "";
				Assert::IsTrue(VpSkeleton::SceneRecordReader::AcquireSourceStats(40, p4Rec, &p4Why));
				Assert::IsTrue(VpUnit::CutFadeJudge::BlankOf(p4Rec).black);
				Assert::AreEqual(64.0 / 1023.0, VpUnit::SourceStats::NormalisedMean(p4Rec), 1e-9);

				VpSkeleton::SceneRecordReader::SceneVerdict sv;
				const char* u7Why = "";
				Assert::IsTrue(VpSkeleton::SceneRecordReader::AcquireScene(40, sv, &u7Why));
				Assert::IsTrue(sv.fade);
				Assert::AreEqual(0.72f, sv.fadeKeep, 1e-6f);
			}

			// Default (uninjected!) SRM Monitor fed textured tiles at seq 40 reads real P4 flat-black + real U7 fade from skeleton.
			const std::vector<float> tiles = TexturedTiles(20260503u);
			Monitor integrated;
			integrated.AnalyseTilesForTest(tiles, 8, 1.0, 1920, 1080, {}, {}, 8, Monitor::kTile, kBlackNorm, 40);
			const Snapshot s = integrated.Read();
			Assert::AreEqual<unsigned long long>(1, s.u7SceneShared);
			Assert::AreEqual<unsigned long long>(0, s.u7SceneFallback);
			Assert::AreEqual<unsigned long long>(1, s.p4StatsShared);
			Assert::AreEqual<unsigned long long>(0, s.p4StatsFallback);
			Assert::IsTrue(s.frameFade, L"Default SRM reader must consume U7 fade via AcquireScene");
			Assert::IsTrue(s.frameBlank, L"Default SRM reader must consume P4 BlankOf via AcquireSourceStats");
			Assert::IsTrue(s.frameBlack, L"Default SRM reader must consume P4 BlankOf.black via AcquireSourceStats");

			// Negative control: clear tap owner so default readers cannot find a store owner -> falls back visibly to textured tiles.
			tap.Clear();
			Monitor detached;
			detached.AnalyseTilesForTest(tiles, 8, 1.0, 1920, 1080, {}, {}, 8, Monitor::kTile, kBlackNorm, 41);
			const Snapshot d = detached.Read();
			Assert::AreEqual<unsigned long long>(0, d.u7SceneShared);
			Assert::AreEqual<unsigned long long>(1, d.u7SceneFallback);
			Assert::AreEqual(std::string("no store owner"), d.u7SceneFallbackWhy);
			Assert::AreEqual<unsigned long long>(0, d.p4StatsShared);
			Assert::AreEqual<unsigned long long>(1, d.p4StatsFallback);
			Assert::AreEqual(std::string("no store owner"), d.p4StatsFallbackWhy);
			Assert::IsFalse(d.frameFade);
			Assert::IsFalse(d.frameBlack, L"Negative control: without skeleton owner, local tiles read non-black");
		}
	};
}
