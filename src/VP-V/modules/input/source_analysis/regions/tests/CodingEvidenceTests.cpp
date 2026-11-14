// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// input.source_analysis/regions: P16 kCodingEvidence (lead8/iface-P16.md). The moved tile measure and frame reduction, the product's
// bytes (round trip, corrupt input refused), the owner fill through a real SkeletonMemo (computed once, equal to the direct measure),
// and the unavailable paths (no memo, nothing staged) that consumers show instead of measuring privately. CPU only.
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/modules/input/source_analysis/regions/CodingEvidence.h>

#include <cmath>
#include <memory>
#include <random>
#include <string>
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
			const CE::FrameCoding c = CE::ReduceFrame({ &clean }, eligible, true);
			const CE::FrameCoding b = CE::ReduceFrame({ &blocked }, eligible, true);
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
				CE::MakeOwnerFill(CE::Hub::Instance())) == VpSkeleton::SkeletonMemo::Registration::Registered, L"owner registered");
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
			const VpSkeleton::OwnerFill f = CE::MakeOwnerFill(hub);
			VpSkeleton::SkeletonMemo m;
			VpSkeleton::OwnerFillRequest req;
			req.id = VpSkeleton::kCodingEvidence;
			req.seq = 5;
			VpSkeleton::OwnerFillOutput out;
			Assert::IsFalse(f.fill(req, m, out), L"declined, never invented");
		}

		TEST_METHOD(StagingIsBounded)
		{
			CE::Hub hub;
			for (uint64_t s = 1; s <= 40; ++s)
				hub.Stage(s, PackOf({ Clean(1) }));
			Assert::IsTrue(hub.Staged(1) == nullptr && hub.Staged(40) != nullptr, L"the oldest are dropped");
		}
	};
}
