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
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace
{
	using SourceResolutionMonitor::Monitor;
	using SourceResolutionMonitor::Snapshot;

	constexpr double kBlackNorm = 64.0 / 1023.0;

	// Generates 8 informative high-contrast noise tiles at the given mean (never blank/black locally).
	std::vector<float> TexturedTilesAt(unsigned seed, double mean, double stddev = 0.14)
	{
		std::mt19937 rng(seed);
		std::normal_distribution<double> dist(mean, stddev);
		std::vector<float> tiles(static_cast<size_t>(8) * Monitor::kTile * Monitor::kTile);
		for (float& v : tiles)
		{
			const double c = (std::min)(1.0, (std::max)(0.0, dist(rng)));
			v = static_cast<float>(std::round(c * 1023.0) / 1023.0);
		}
		return tiles;
	}

	std::vector<float> TexturedTiles(unsigned seed)
	{
		return TexturedTilesAt(seed, 0.50, 0.14);
	}

	struct TempHistoryDir
	{
		std::string path;
		TempHistoryDir()
		{
			char tmp[MAX_PATH] = {};
			Assert::IsTrue(GetTempPathA(ARRAYSIZE(tmp), tmp) > 0);
			char dir[MAX_PATH] = {};
			Assert::IsTrue(GetTempFileNameA(tmp, "srm", 0, dir) != 0);
			Assert::IsTrue(DeleteFileA(dir) != 0);
			Assert::IsTrue(CreateDirectoryA(dir, nullptr) != 0);
			path = dir;
		}
		~TempHistoryDir()
		{
			if (path.empty()) return;
			WIN32_FIND_DATAA found{};
			HANDLE search = FindFirstFileA((path + "\\*").c_str(), &found);
			if (search != INVALID_HANDLE_VALUE)
			{
				do
				{
					if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
						DeleteFileA((path + "\\" + found.cFileName).c_str());
				} while (FindNextFileA(search, &found));
				FindClose(search);
			}
			RemoveDirectoryA(path.c_str());
		}
	};

	std::vector<std::string> SplitCsvLine(const std::string& line)
	{
		std::vector<std::string> cols;
		std::string cur;
		bool inQuotes = false;
		for (size_t i = 0; i < line.size(); ++i)
		{
			const char c = line[i];
			if (c == '"')
				inQuotes = !inQuotes;
			else if (c == ',' && !inQuotes)
			{
				cols.push_back(cur);
				cur.clear();
			}
			else if (c != '\r')
				cur.push_back(c);
		}
		cols.push_back(cur);
		return cols;
	}

	bool ReadHistoryRowForSeq(const std::string& csvPath, uint64_t targetSeq,
		int& outBlank, int& outCut, int& outFade, double& outApl)
	{
		std::ifstream in(csvPath.c_str());
		if (!in)
			return false;
		std::string headerLine;
		if (!std::getline(in, headerLine))
			return false;
		const std::vector<std::string> header = SplitCsvLine(headerLine);
		auto colOf = [&](const char* name) -> int
		{
			for (size_t i = 0; i < header.size(); ++i)
				if (header[i] == name)
					return static_cast<int>(i);
			return -1;
		};
		const int iEvent = colOf("event");
		const int iSeq = colOf("seq");
		const int iBlank = colOf("blank");
		const int iCut = colOf("cut");
		const int iFade = colOf("fade");
		const int iApl = colOf("apl");
		if (iEvent < 0 || iSeq < 0 || iBlank < 0 || iCut < 0 || iFade < 0 || iApl < 0)
			return false;
		std::string line;
		while (std::getline(in, line))
		{
			if (line.empty()) continue;
			const std::vector<std::string> cols = SplitCsvLine(line);
			const int maxIdx = (std::max)((std::max)(iEvent, iSeq), (std::max)((std::max)(iBlank, iCut), (std::max)(iFade, iApl)));
			if (static_cast<int>(cols.size()) <= maxIdx) continue;
			if (cols[static_cast<size_t>(iEvent)] != "reading") continue;
			if (cols[static_cast<size_t>(iSeq)].empty()) continue;
			const uint64_t seq = static_cast<uint64_t>(std::strtoull(cols[static_cast<size_t>(iSeq)].c_str(), nullptr, 10));
			if (seq != targetSeq) continue;
			outBlank = std::atoi(cols[static_cast<size_t>(iBlank)].c_str());
			outCut = std::atoi(cols[static_cast<size_t>(iCut)].c_str());
			outFade = std::atoi(cols[static_cast<size_t>(iFade)].c_str());
			outApl = std::atof(cols[static_cast<size_t>(iApl)].c_str());
			return true;
		}
		return false;
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
		void SetCallMemoOnSceneOf(bool on) { callMemoOnSceneOf_ = on; }
		uint64_t SceneOfCalls() const { return sceneOfCalls_.load(std::memory_order_relaxed); }

		Vpv::Skeleton::SceneAnswer SceneOf(const void* host, uint64_t seq) const override
		{
			sceneOfCalls_.fetch_add(1, std::memory_order_relaxed);
			if (host == host_)
			{
				if (callMemoOnSceneOf_ && memo_)
					memo_->Acquire(VpSkeleton::kP4SourceStats, seq, VpSkeleton::AcquireForm::Cpu);
				return answer_;
			}
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
		bool callMemoOnSceneOf_ = false;
		mutable std::atomic<uint64_t> sceneOfCalls_{ 0 };
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

		// Pass B LOW: CutFadeJudge::BlankOf grid-flatness branch (spread > kBlackSpreadP4 with Flatness(grid) < kFlatF),
		// contrasted against a non-flat textured grid having the EXACT same p99-p01 spread.
		TEST_METHOD(CutFadeJudgeBlankOfGridFlatnessBranchDistinguishesSmoothFromTextured)
		{
			constexpr uint32_t kHw = 64, kHh = 36; // 2x2 pixels per 32x18 grid cell

			// Smooth mid-grey vertical ramp: g(gy) = 100.0 + 0.5*gy -> P4 25600..27776 (spread ~2176 >> kBlackSpreadP4=256),
			// while Flatness(grid) ~= 0.0034 << kFlatF=0.02.
			std::vector<uint16_t> smoothMid(static_cast<size_t>(kHw) * kHh);
			for (uint32_t y = 0; y < kHh; ++y)
			{
				const uint32_t gy = y / 2u; // 0..17
				const uint16_t val = static_cast<uint16_t>(25600u + gy * 128u);
				for (uint32_t x = 0; x < kHw; ++x)
					smoothMid[static_cast<size_t>(y) * kHw + x] = val;
			}
			const VpUnit::SourceStats::Record smoothMidRec = ComputeSyntheticP4(smoothMid, kHw, kHh);
			Assert::AreEqual(1u, smoothMidRec.gridFound);
			Assert::IsTrue(smoothMidRec.p99 - smoothMidRec.p01 > VpUnit::CutFadeJudge::kBlackSpreadP4,
				L"Precondition: spread must exceed kBlackSpreadP4 to exercise the grid-flatness branch");
			const VpUnit::CutFadeJudge::BlankVerdict smoothMidBv = VpUnit::CutFadeJudge::BlankOf(smoothMidRec);
			Assert::IsTrue(smoothMidBv.flat, L"Smooth mid-grey ramp with Flatness < kFlatF must classify flat=true");
			Assert::IsTrue(smoothMidBv.blank, L"Smooth mid-grey ramp must classify blank=true");
			Assert::IsFalse(smoothMidBv.black, L"Smooth mid-grey ramp (mean > kBlackMeanMaxP4) must classify black=false");

			// Smooth dark vertical ramp: P4 2560..3240 (spread ~640 > 256, mean < kBlackMeanMaxP4=12288, Flatness < 0.01).
			std::vector<uint16_t> smoothDark(static_cast<size_t>(kHw) * kHh);
			for (uint32_t y = 0; y < kHh; ++y)
			{
				const uint32_t gy = y / 2u;
				const uint16_t val = static_cast<uint16_t>(2560u + gy * 40u);
				for (uint32_t x = 0; x < kHw; ++x)
					smoothDark[static_cast<size_t>(y) * kHw + x] = val;
			}
			const VpUnit::SourceStats::Record smoothDarkRec = ComputeSyntheticP4(smoothDark, kHw, kHh);
			Assert::IsTrue(smoothDarkRec.p99 - smoothDarkRec.p01 > VpUnit::CutFadeJudge::kBlackSpreadP4);
			const VpUnit::CutFadeJudge::BlankVerdict smoothDarkBv = VpUnit::CutFadeJudge::BlankOf(smoothDarkRec);
			Assert::IsTrue(smoothDarkBv.flat && smoothDarkBv.blank && smoothDarkBv.black,
				L"Smooth dark ramp via grid-flatness branch must classify flat=true, blank=true, black=true");

			// Negative control with the EXACT same spread (25600..27776, spread=2176) arranged as a 2x2-pixel
			// cell checkerboard -> Flatness ~= 0.078 >> kFlatF=0.02 -> must classify non-flat/non-blank.
			std::vector<uint16_t> checkerSameSpread(static_cast<size_t>(kHw) * kHh);
			for (uint32_t y = 0; y < kHh; ++y)
				for (uint32_t x = 0; x < kHw; ++x)
					checkerSameSpread[static_cast<size_t>(y) * kHw + x] =
						(((x / 2u) ^ (y / 2u)) & 1u) ? 27776u : 25600u;
			const VpUnit::SourceStats::Record checkerRec = ComputeSyntheticP4(checkerSameSpread, kHw, kHh);
			Assert::AreEqual(smoothMidRec.p99 - smoothMidRec.p01, checkerRec.p99 - checkerRec.p01,
				L"Negative control must carry the exact same p99-p01 spread as the smooth ramp");
			const VpUnit::CutFadeJudge::BlankVerdict checkerBv = VpUnit::CutFadeJudge::BlankOf(checkerRec);
			Assert::IsFalse(checkerBv.flat, L"Negative control: high-frequency grid with identical spread must NOT be flat");
			Assert::IsFalse(checkerBv.blank);
			Assert::IsFalse(checkerBv.black);
		}

		// Pass B MED-3 (parts 1 & 3) + Pass A MED-3: same-input negative controls for:
		//   - Analyse() U7 cut propagation (verified via history CSV cut column)
		//   - LookAheadEvidence(from, to) consecutive U7 history coverage (and Discontinuity clear)
		//   - SRM P4 mean replacement (NormalisedMean vs private tile mean ~0.25 in history CSV apl column)
		TEST_METHOD(SrmAnalyseCutLookAheadEvidenceAndP4MeanSameInputControls)
		{
			constexpr int kW = 1024, kH = 512;
			const std::vector<float> lowMeanTiles = TexturedTilesAt(20260510u, 0.25, 0.06);
			auto staticSampler = [](int x, int y) -> int
			{
				const unsigned h = (static_cast<unsigned>(x) * 73856093u) ^ (static_cast<unsigned>(y) * 19349663u);
				return 200 + static_cast<int>((h >> 16) % 500u);
			};

			// 1. Shared U7 + P4 monitor: identical static tiles + identical static look-ahead frames;
			//    U7 says cut=true on seq 12, P4 says mean=0.75 on seq 30.
			TempHistoryDir sharedDir;
			{
				Monitor sharedMon;
				sharedMon.SetHistoryFolder(sharedDir.path);
				sharedMon.SetSharedSceneReader([](uint64_t seq, Monitor::SharedSceneInput& out, const char**) -> bool {
					out.cut = (seq == 12);
					out.fade = false;
					out.fadeKeep = 1.0f;
					return true;
				});
				sharedMon.SetSharedP4Reader([](uint64_t seq, Monitor::SharedP4Input& out, const char**) -> bool {
					out.blank = false;
					out.black = false;
					out.flat = false;
					out.mean = (seq == 30) ? 0.75 : 0.25;
					return true;
				});

				// LookAheadFrame populates consecutive U7 history for LookAheadEvidence(from, to).
				for (uint64_t s = 10; s <= 13; ++s)
					sharedMon.LookAheadFrame(s, kW, kH, staticSampler, 0, 0, kW, kH);

				const SourceResolution::FrameEvidence evWithCut = sharedMon.LookAheadEvidence(10, 13);
				Assert::IsTrue(evWithCut.covered, L"Shared U7 consecutive look-ahead frames must mark evidence covered=true");
				Assert::IsTrue(evWithCut.cut, L"Shared U7 cut on seq 12 must report LookAheadEvidence(10, 13).cut=true");
				Assert::IsFalse(evWithCut.unresolved);

				const SourceResolution::FrameEvidence evNoCut = sharedMon.LookAheadEvidence(10, 11);
				Assert::IsTrue(evNoCut.covered);
				Assert::IsFalse(evNoCut.cut, L"Interval (10, 11] without U7 cut must report cut=false");

				// Analyse() on identical static tiles: seq 11 (no cut) -> seq 12 (U7 cut) -> seq 30 (P4 mean=0.75).
				sharedMon.AnalyseTilesForTest(lowMeanTiles, 8, 1.0, kW, kH, {}, {}, 8, Monitor::kTile, kBlackNorm, 11);
				sharedMon.AnalyseTilesForTest(lowMeanTiles, 8, 1.5, kW, kH, {}, {}, 8, Monitor::kTile, kBlackNorm, 12);
				sharedMon.AnalyseTilesForTest(lowMeanTiles, 8, 2.0, kW, kH, {}, {}, 8, Monitor::kTile, kBlackNorm, 30);
				sharedMon.FlushHistoryNow();

				const std::string csvPath = sharedMon.HistoryPath();
				Assert::IsFalse(csvPath.empty());
				int blank11 = -1, cut11 = -1, fade11 = -1;
				double apl11 = -1.0;
				Assert::IsTrue(ReadHistoryRowForSeq(csvPath, 11, blank11, cut11, fade11, apl11));
				Assert::AreEqual(0, cut11, L"seq 11 without U7 cut must record cut=0");

				int blank12 = -1, cut12 = -1, fade12 = -1;
				double apl12 = -1.0;
				Assert::IsTrue(ReadHistoryRowForSeq(csvPath, 12, blank12, cut12, fade12, apl12));
				Assert::AreEqual(1, cut12, L"Analyse() must propagate shared U7 cut=1 even on identical static tiles");

				int blank30 = -1, cut30 = -1, fade30 = -1;
				double apl30 = -1.0;
				Assert::IsTrue(ReadHistoryRowForSeq(csvPath, 30, blank30, cut30, fade30, apl30));
				Assert::AreEqual(0.75, apl30, 1e-4, L"Analyse() must replace private tile mean (~0.25) with shared P4 mean (0.75)");

				// Discontinuity clears U7 ahead history so LookAheadEvidence no longer reports stale coverage.
				sharedMon.Discontinuity("test seek");
				Assert::IsFalse(sharedMon.LookAheadEvidence(10, 13).covered,
					L"Discontinuity must clear u7AheadHistory");
			}

			// 2. Negative control on the EXACT same tiles & static frames WITHOUT shared U7 / P4:
			//    LookAheadEvidence sees cut=false, Analyse() records cut=0 on seq 12, and apl stays at private tile mean (~0.25).
			TempHistoryDir naiveDir;
			{
				Monitor naiveMon;
				naiveMon.SetHistoryFolder(naiveDir.path);
				naiveMon.SetSharedSceneReader([](uint64_t, Monitor::SharedSceneInput&, const char** why) -> bool {
					if (why != nullptr) *why = "test: no u7";
					return false;
				});
				naiveMon.SetSharedP4Reader([](uint64_t, Monitor::SharedP4Input&, const char** why) -> bool {
					if (why != nullptr) *why = "test: no p4";
					return false;
				});

				for (uint64_t s = 10; s <= 13; ++s)
					naiveMon.LookAheadFrame(s, kW, kH, staticSampler, 0, 0, kW, kH);
				Assert::IsFalse(naiveMon.LookAheadEvidence(10, 13).cut,
					L"Negative control: identical static frames without U7 must NOT report LookAheadEvidence cut");

				naiveMon.AnalyseTilesForTest(lowMeanTiles, 8, 1.0, kW, kH, {}, {}, 8, Monitor::kTile, kBlackNorm, 11);
				naiveMon.AnalyseTilesForTest(lowMeanTiles, 8, 1.5, kW, kH, {}, {}, 8, Monitor::kTile, kBlackNorm, 12);
				naiveMon.AnalyseTilesForTest(lowMeanTiles, 8, 2.0, kW, kH, {}, {}, 8, Monitor::kTile, kBlackNorm, 30);
				naiveMon.FlushHistoryNow();

				const std::string csvPath = naiveMon.HistoryPath();
				int blank12 = -1, cut12 = -1, fade12 = -1;
				double apl12 = -1.0;
				Assert::IsTrue(ReadHistoryRowForSeq(csvPath, 12, blank12, cut12, fade12, apl12));
				Assert::AreEqual(0, cut12, L"Negative control: identical static tiles without U7 must record cut=0");

				int blank30 = -1, cut30 = -1, fade30 = -1;
				double apl30 = -1.0;
				Assert::IsTrue(ReadHistoryRowForSeq(csvPath, 30, blank30, cut30, fade30, apl30));
				Assert::IsTrue(apl30 < 0.35, L"Negative control: without shared P4, apl must stay near private tile mean (~0.25)");
			}
		}

		// Pass B MED-3 (part 2): LookAheadFrame P4 black/blank under BOTH SetLatchFixes(true) and SetLatchFixes(false),
		// contrasted against exact same non-black textured tiles without shared P4.
		TEST_METHOD(SrmLookAheadFrameP4BlackAndBlankUnderBothLatchFixesModes)
		{
			constexpr int kW = 1024, kH = 512;
			auto nonBlackTextured = [](int x, int y) -> int
			{
				const unsigned h = (static_cast<unsigned>(x) * 73856093u) ^ (static_cast<unsigned>(y) * 19349663u);
				return 300 + static_cast<int>((h >> 16) % 500u); // textured, mean ~0.54 (never locally blank or black)
			};

			Snapshot lower;
			lower.reading = true;
			lower.nativeWidth = kW;
			lower.nativeHeight = kH;
			lower.declaredRatio = lower.liveRatio = lower.frameRatio = 0.5;
			lower.confidence = 0.9;
			lower.frames = 10;

			auto armPending = [&](Monitor& m)
			{
				m.InjectResultForTest(lower, 1);
				std::string why;
				m.AutoFactor(kH, 0.8, why, 1.0, 1);
			};

			// Mode A: SetLatchFixes(true) -> LookAheadFrame uses sharedP4.black (not flat bright blank).
			{
				Monitor blackMon;
				blackMon.SetLatchFixes(true);
				blackMon.SetSharedSceneReader([](uint64_t, Monitor::SharedSceneInput& out, const char**) -> bool {
					out.cut = false;
					return true;
				});
				blackMon.SetSharedP4Reader([](uint64_t seq, Monitor::SharedP4Input& out, const char**) -> bool {
					out.blank = (seq == 30);
					out.black = (seq == 30);
					out.flat = (seq == 30);
					out.mean = (seq == 30) ? kBlackNorm : 0.54;
					return true;
				});
				armPending(blackMon);
				blackMon.LookAheadFrame(29, kW, kH, nonBlackTextured, 0, 0, kW, kH);
				blackMon.LookAheadFrame(30, kW, kH, nonBlackTextured, 0, 0, kW, kH);
				Assert::AreEqual<uint64_t>(30ull, blackMon.Ahead().eventSequence,
					L"LatchFixes(true): shared P4 black=true must arm LookAheadFrame even on textured non-black tiles");
				Assert::AreEqual<uint64_t>(0ull, blackMon.Ahead().targetSequence,
					L"Blank/black event must await the next non-blank picture frame as target");

				// Bright flat (blank=true, black=false) with LatchFixes(true) must NOT arm an event.
				Monitor flatBrightMon;
				flatBrightMon.SetLatchFixes(true);
				flatBrightMon.SetSharedSceneReader([](uint64_t, Monitor::SharedSceneInput& out, const char**) -> bool {
					out.cut = false;
					return true;
				});
				flatBrightMon.SetSharedP4Reader([](uint64_t seq, Monitor::SharedP4Input& out, const char**) -> bool {
					out.blank = (seq == 30);
					out.black = false;
					out.flat = (seq == 30);
					out.mean = 0.54;
					return true;
				});
				armPending(flatBrightMon);
				flatBrightMon.LookAheadFrame(29, kW, kH, nonBlackTextured, 0, 0, kW, kH);
				flatBrightMon.LookAheadFrame(30, kW, kH, nonBlackTextured, 0, 0, kW, kH);
				Assert::AreEqual<uint64_t>(0ull, flatBrightMon.Ahead().eventSequence,
					L"LatchFixes(true): bright flat non-black (blank=true, black=false) must NOT arm");

				// Negative control: no shared P4 on exact same non-black tiles -> eventSequence stays 0.
				Monitor naiveFixesOn;
				naiveFixesOn.SetLatchFixes(true);
				naiveFixesOn.SetSharedSceneReader([](uint64_t, Monitor::SharedSceneInput& out, const char**) -> bool {
					out.cut = false;
					return true;
				});
				naiveFixesOn.SetSharedP4Reader([](uint64_t, Monitor::SharedP4Input&, const char** why) -> bool {
					if (why != nullptr) *why = "test: no p4";
					return false;
				});
				armPending(naiveFixesOn);
				naiveFixesOn.LookAheadFrame(29, kW, kH, nonBlackTextured, 0, 0, kW, kH);
				naiveFixesOn.LookAheadFrame(30, kW, kH, nonBlackTextured, 0, 0, kW, kH);
				Assert::AreEqual<uint64_t>(0ull, naiveFixesOn.Ahead().eventSequence,
					L"Negative control (LatchFixes=true): textured tiles without shared P4 must NOT arm");
			}

			// Mode B: SetLatchFixes(false) -> LookAheadFrame uses sharedP4.blank (flat non-black arms).
			{
				Monitor blankMon;
				blankMon.SetLatchFixes(false);
				blankMon.SetSharedSceneReader([](uint64_t, Monitor::SharedSceneInput& out, const char**) -> bool {
					out.cut = false;
					return true;
				});
				blankMon.SetSharedP4Reader([](uint64_t seq, Monitor::SharedP4Input& out, const char**) -> bool {
					out.blank = (seq == 30);
					out.black = false;
					out.flat = (seq == 30);
					out.mean = 0.54;
					return true;
				});
				armPending(blankMon);
				blankMon.LookAheadFrame(29, kW, kH, nonBlackTextured, 0, 0, kW, kH);
				blankMon.LookAheadFrame(30, kW, kH, nonBlackTextured, 0, 0, kW, kH);
				Assert::AreEqual<uint64_t>(30ull, blankMon.Ahead().eventSequence,
					L"LatchFixes(false): shared P4 blank=true must arm LookAheadFrame even when black=false");
				Assert::AreEqual<uint64_t>(0ull, blankMon.Ahead().targetSequence);

				// Negative control: no shared P4 on exact same non-black tiles -> eventSequence stays 0.
				Monitor naiveFixesOff;
				naiveFixesOff.SetLatchFixes(false);
				naiveFixesOff.SetSharedSceneReader([](uint64_t, Monitor::SharedSceneInput& out, const char**) -> bool {
					out.cut = false;
					return true;
				});
				naiveFixesOff.SetSharedP4Reader([](uint64_t, Monitor::SharedP4Input&, const char** why) -> bool {
					if (why != nullptr) *why = "test: no p4";
					return false;
				});
				armPending(naiveFixesOff);
				naiveFixesOff.LookAheadFrame(29, kW, kH, nonBlackTextured, 0, 0, kW, kH);
				naiveFixesOff.LookAheadFrame(30, kW, kH, nonBlackTextured, 0, 0, kW, kH);
				Assert::AreEqual<uint64_t>(0ull, naiveFixesOff.Ahead().eventSequence,
					L"Negative control (LatchFixes=false): textured tiles without shared P4 must NOT arm");
			}
		}

		// Pass A HIGH-1 regression test (MUST-9): AcquireSourceStats when no lane/owner fill is active OR when called
		// off the render thread must return false with an explicit non-empty cause AND leave SkeletonMemo::Decisions()
		// completely empty (zero spurious global "skeleton acquire p4_source_stats" fallback).
		TEST_METHOD(AcquireSourceStatsObeysMust9WithZeroSpuriousMemoFallbacks)
		{
			const int ownerTag = 0;
			const void* const owner = &ownerTag;
			FrameAnalysis::Store store(8);
			SrmTapOwnerScope tap(owner, &store);

			// Subcase A: memo exists with NO registered P4 owner fill (no analysis lane).
			{
				auto emptyMemo = std::make_shared<VpSkeleton::SkeletonMemo>();
				FakeSrmSkeletonDemand demand(owner, emptyMemo);
				SrmHubScope hub(&demand);

				VpUnit::SourceStats::Record rec;
				const char* why = "";
				Assert::IsFalse(VpSkeleton::SceneRecordReader::AcquireSourceStats(50, rec, &why),
					L"AcquireSourceStats without P4 owner fill must return false");
				Assert::IsTrue(why != nullptr && why[0] != '\0', L"Must return explicit non-empty failure cause");
				Assert::AreEqual(std::string("no analysis lane"), std::string(why));
				Assert::IsTrue(emptyMemo->Decisions().empty(),
					L"MUST-9: AcquireSourceStats without owner fill must NOT record a spurious global memo Fallback decision");
			}

			// Subcase B: P4 owner fill registered, render thread pinned to main thread; worker thread calls AcquireSourceStats.
			{
				auto memo = std::make_shared<VpSkeleton::SkeletonMemo>();
				memo->SetRenderThread(std::this_thread::get_id());
				VpSkeleton::OwnerFill p4Fill;
				p4Fill.fill = [](const VpSkeleton::OwnerFillRequest&, VpSkeleton::SkeletonMemo&, VpSkeleton::OwnerFillOutput& out) -> bool {
					constexpr uint32_t kHw = 64, kHh = 36;
					const std::vector<uint16_t> flatBlack(static_cast<size_t>(kHw) * kHh, static_cast<uint16_t>(64u << 6));
					out.bytes.assign(VpUnit::SourceStats::kRecordBytes, 0);
					return VpUnit::SourceStats::ComputeRecord(flatBlack.data(), kHw, kHh, out.bytes.data(), out.bytes.size());
				};
				const std::string p4Owner = VpSkeleton::SkeletonMemo::OwnerUnitOf(VpSkeleton::kP4SourceStats);
				Assert::IsTrue(
					memo->RegisterOwnerFill(VpSkeleton::kP4SourceStats, VpSkeleton::AcquireForm::Cpu, p4Owner, std::move(p4Fill))
					== VpSkeleton::SkeletonMemo::Registration::Registered);

				FakeSrmSkeletonDemand demand(owner, memo);
				SrmHubScope hub(&demand);

				// Off-render-thread read of unpublished seq 51: returns false with explicit cause, ZERO memo Decisions().
				bool offThreadOk = true;
				std::string offThreadWhy;
				std::thread workerUnpublished([&]() {
					VpUnit::SourceStats::Record rec;
					const char* why = "";
					offThreadOk = VpSkeleton::SceneRecordReader::AcquireSourceStats(51, rec, &why);
					offThreadWhy = (why != nullptr) ? why : "";
				});
				workerUnpublished.join();
				Assert::IsFalse(offThreadOk, L"Off-render-thread AcquireSourceStats on unpublished frame must return false");
				Assert::IsFalse(offThreadWhy.empty(), L"Off-render-thread miss must report explicit non-empty cause");
				Assert::AreEqual(std::string("p4 not published (off render thread)"), offThreadWhy);
				Assert::IsTrue(memo->Decisions().empty(),
					L"MUST-9: off-render-thread AcquireSourceStats must NOT pollute SkeletonMemo::Decisions()");

				// Positive control: publish seq 52 on render thread -> off-render-thread AcquireSourceStats succeeds via CopyPublished!
				const VpSkeleton::AcquireResult pub = memo->Acquire(VpSkeleton::kP4SourceStats, 52, VpSkeleton::AcquireForm::Cpu);
				Assert::IsTrue(pub.Ok());
				bool pubReadOk = false;
				double pubReadMean = -1.0;
				std::thread workerPublished([&]() {
					VpUnit::SourceStats::Record rec;
					const char* why = "";
					pubReadOk = VpSkeleton::SceneRecordReader::AcquireSourceStats(52, rec, &why);
					if (pubReadOk)
						pubReadMean = VpUnit::SourceStats::NormalisedMean(rec);
				});
				workerPublished.join();
				Assert::IsTrue(pubReadOk, L"Off-render-thread AcquireSourceStats must succeed via CopyPublished once published");
				Assert::AreEqual(64.0 / 1023.0, pubReadMean, 1e-9);
				Assert::IsTrue(memo->Decisions().empty());
			}
		}

		// Pass A2 HIGH-1 (7e66aabb1) regression test: SceneRecordReader::AcquireScene obeys MUST-9 off the render thread:
		//   - Store miss off render thread returns false ("scene not published (off render thread)"), NEVER calls SceneOf/memo->Acquire,
		//     and leaves SkeletonMemo::Decisions() completely empty.
		//   - Store hit off render thread returns true directly via non-mutating StoreFor(owner)->PeekScene with exact verdict
		//     and SkeletonMemo::Decisions() completely empty.
		//   - Render-thread store miss delegates to SceneOf cleanly.
		TEST_METHOD(AcquireSceneObeysMust9OffRenderThreadWithStoreHitAndMissControls)
		{
			const int ownerTag = 0;
			const void* const owner = &ownerTag;
			FrameAnalysis::Store store(16);
			store.SetEnabled(true);
			SrmTapOwnerScope tap(owner, &store);

			auto memo = std::make_shared<VpSkeleton::SkeletonMemo>();
			memo->SetRenderThread(std::this_thread::get_id());
			VpSkeleton::OwnerFill p4Fill;
			p4Fill.renderThreadOnly = true;
			p4Fill.fill = [](const VpSkeleton::OwnerFillRequest&, VpSkeleton::SkeletonMemo&, VpSkeleton::OwnerFillOutput& out) -> bool {
				constexpr uint32_t kHw = 64, kHh = 36;
				const std::vector<uint16_t> flatBlack(static_cast<size_t>(kHw) * kHh, static_cast<uint16_t>(64u << 6));
				out.bytes.assign(VpUnit::SourceStats::kRecordBytes, 0);
				return VpUnit::SourceStats::ComputeRecord(flatBlack.data(), kHw, kHh, out.bytes.data(), out.bytes.size());
			};
			const std::string p4Owner = VpSkeleton::SkeletonMemo::OwnerUnitOf(VpSkeleton::kP4SourceStats);
			Assert::IsTrue(
				memo->RegisterOwnerFill(VpSkeleton::kP4SourceStats, VpSkeleton::AcquireForm::Cpu, p4Owner, std::move(p4Fill))
				== VpSkeleton::SkeletonMemo::Registration::Registered);

			FakeSrmSkeletonDemand demand(owner, memo);
			demand.SetCallMemoOnSceneOf(true); // proves hazard: any off-thread SceneOf call would trigger WrongThread in memo->Decisions()!
			Vpv::Skeleton::SceneAnswer liveAns;
			liveAns.known = true;
			liveAns.cut = false;
			liveAns.fade = true;
			liveAns.fadeKeep = 0.77f;
			liveAns.from = Vpv::Skeleton::SceneAnswer::From::Live;
			demand.SetSceneAnswer(liveAns);
			SrmHubScope hub(&demand);

			// 1. Negative control (store miss off render thread): refuses cleanly without calling SceneOf or polluting Decisions().
			{
				bool onRenderThread = true;
				bool ok = true;
				std::string whyStr;
				VpSkeleton::SceneRecordReader::SceneVerdict v;
				std::thread workerMiss([&]() {
					onRenderThread = memo->OnRenderThread();
					const char* why = "";
					ok = VpSkeleton::SceneRecordReader::AcquireScene(61, v, &why);
					whyStr = (why != nullptr) ? why : "";
				});
				workerMiss.join();
				Assert::IsFalse(onRenderThread, L"Worker thread must be off the render thread");
				Assert::IsFalse(ok, L"Off-render-thread AcquireScene on store miss must return false");
				Assert::AreEqual(std::string("scene not published (off render thread)"), whyStr);
				Assert::IsFalse(v.cut);
				Assert::IsFalse(v.fade);
				Assert::AreEqual(-1.0f, v.fadeKeep, 1e-6f);
				Assert::AreEqual<uint64_t>(0ull, demand.SceneOfCalls(),
					L"MUST-9: AcquireScene off render thread must NOT enter SceneOf live fill");
				Assert::IsTrue(memo->Decisions().empty(),
					L"MUST-9: off-render-thread AcquireScene must leave SkeletonMemo::Decisions() completely empty");
			}

			// 2. Positive control (store hit off render thread): reads published store scene directly, zero SceneOf calls, zero Decisions().
			{
				FrameAnalysis::Identity id;
				id.seq = 62;
				Assert::IsTrue(store.Begin(id, FrameAnalysis::kScene) != 0);
				FrameAnalysis::Scene published;
				published.cut = true;
				published.fade = true;
				published.fadeKeep = 0.68f;
				published.confirmed = true;
				Assert::IsTrue(store.PublishScene(62, published));

				bool onRenderThread = true;
				bool ok = false;
				std::string whyStr = "unset";
				VpSkeleton::SceneRecordReader::SceneVerdict v;
				std::thread workerHit([&]() {
					onRenderThread = memo->OnRenderThread();
					const char* why = "sentinel";
					ok = VpSkeleton::SceneRecordReader::AcquireScene(62, v, &why);
					whyStr = (why != nullptr) ? why : "";
				});
				workerHit.join();
				Assert::IsFalse(onRenderThread, L"Worker thread must be off the render thread");
				Assert::IsTrue(ok, L"Off-render-thread AcquireScene must succeed on store hit via PeekScene");
				Assert::AreEqual(std::string(""), whyStr);
				Assert::IsTrue(v.cut);
				Assert::IsTrue(v.fade);
				Assert::AreEqual(0.68f, v.fadeKeep, 1e-6f);
				Assert::AreEqual<uint64_t>(0ull, demand.SceneOfCalls(),
					L"Store hit must be served via non-mutating PeekScene without entering SceneOf");
				Assert::IsTrue(memo->Decisions().empty());
			}

			// 3. Render-thread control (store miss ON render thread): delegates to SceneOf cleanly.
			{
				Assert::IsTrue(memo->OnRenderThread());
				VpSkeleton::SceneRecordReader::SceneVerdict v;
				const char* why = "sentinel";
				Assert::IsTrue(VpSkeleton::SceneRecordReader::AcquireScene(63, v, &why));
				Assert::AreEqual(std::string(""), std::string(why));
				Assert::IsFalse(v.cut);
				Assert::IsTrue(v.fade);
				Assert::AreEqual(0.77f, v.fadeKeep, 1e-6f);
				Assert::AreEqual<uint64_t>(1ull, demand.SceneOfCalls());
				Assert::IsTrue(memo->Decisions().empty());
			}
		}
	};
}
