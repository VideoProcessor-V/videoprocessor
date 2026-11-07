// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// repair.temporal_detail / products: the reserved engine products (R1-W1-RES). CPU only: the registry entries, the contract table,
// the VRAM budget per tier, the one read API (absent -> classical form, visible), and the three consumer read paths (adaptive
// scaling, dither need, the chroma stage) being byte-identical to today while the engine is not requested.
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/core/Products.h>
#include <VP-V/modules/output/dither/DitherNeed.h>
#include <VP-V/modules/repair/temporal_detail/products/TdProductContracts.h>
#include <VP-V/modules/repair/temporal_detail/products/TdProductReads.h>
#include <VP-V/modules/scaling/chroma_upscale/ChromaCfl.h>
#include <VP-V/modules/scaling/output/AdaptiveDownscaleStage.h>

#include <cstdint>
#include <set>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
	namespace TdProductsNs
	{
		using namespace TdProducts;
		using VpSkeleton::Dims;
		using VpSkeleton::ProductId;
		using VpSkeleton::Registry;
		using VpSkeleton::SkeletonProducts;

		Snapshot FreshAll(int64_t timeUs = 1000)
		{
			Snapshot s;
			for (Entry& e : s.entry)
			{
				e.published = true;
				e.unitVersion = kUnitVersion;
				e.timeUs = timeUs;
				e.maxAgeUs = 0;
			}
			return s;
		}

		TEST_CLASS(TdProductsTests)
		{
		public:
			TEST_METHOD(ReservedProductsAreDenseAfterTheLastSa0IdAndTheRegistryValidates)
			{
				const Registry reg = SkeletonProducts();
				const char* why = nullptr;
				Assert::IsTrue(reg.Validate(&why));
				Assert::AreEqual(15, kCount);
				Assert::AreEqual(int(VpSkeleton::kSourceIdentity) + 1, int(kFirstId), L"appended after P14");
				Assert::AreEqual(int(VpSkeleton::kSkeletonProductEnd), int(kFirstId) + kCount);
				Assert::AreEqual(size_t(VpSkeleton::kSkeletonProductEnd), reg.Size());
				for (int i = 0; i < kCount; ++i)
					Assert::IsTrue(IsReserved(ProductId(int(kFirstId) + i)));
				Assert::IsFalse(IsReserved(VpSkeleton::kSourceIdentity));
				Assert::IsFalse(IsReserved(VpSkeleton::kSkeletonProductEnd));
			}

			TEST_METHOD(ContractsCoverEveryReservedProductWithTheRegistryToken)
			{
				const Registry reg = SkeletonProducts();
				Assert::AreEqual(size_t(kCount), Contracts().size());
				std::set<std::string> tokens;
				for (const Contract& c : Contracts())
				{
					Assert::IsTrue(IsReserved(c.id));
					Assert::AreEqual(std::string(reg.Get(c.id).token), std::string(c.token));
					Assert::IsTrue(tokens.insert(c.token).second, L"unique tokens");
					Assert::IsTrue(c.layout[0] != '\0' && c.meaning[0] != '\0');
					Assert::IsTrue(ContractOf(c.id) == &c);
				}
				// Exactly one product is store-vs-recompute TBD, and it is optional.
				int tbd = 0;
				for (const Contract& c : Contracts())
					if (c.storeVsRecomputeTbd) { ++tbd; Assert::IsTrue(c.fill == FillRule::OptionalMap); Assert::IsTrue(c.id == VpSkeleton::kTdRefinePixel); }
				Assert::AreEqual(1, tbd);
				// Held records are CPU system-RAM products; anchor slots keep their slots as history.
				for (const Contract& c : Contracts())
				{
					const VpSkeleton::ProductDecl& d = reg.Get(c.id);
					if (c.fill == FillRule::HeldRecord)
					{
						Assert::IsTrue(d.backend == VpSkeleton::Backend::Cpu);
						Assert::IsTrue(d.residency == VpSkeleton::Residency::SystemRam);
					}
					else
						Assert::IsTrue(d.backend == VpSkeleton::Backend::GpuCompute);
					if (c.fill == FillRule::AnchorSlot)
						Assert::IsTrue(d.history == (c.id == VpSkeleton::kTdBaseLong ? VpSkeleton::kTdLongSlots : VpSkeleton::kTdShortSlots));
				}
				Assert::IsNull(ContractOf(VpSkeleton::kSourceIdentity));
			}

			TEST_METHOD(BaseBuffersCarryTheChroma420PlanesAtHalfTheLumaBaseResolution)
			{
				const Registry reg = SkeletonProducts();
				// Luma R16 at 1/4 per axis (2 B) + Cb and Cr at 1/8 per axis (2 x 2 B): 1/4 base of 4K = 960 x 540, chroma 480 x 270.
				const Dims d4k = { 3840, 2160 };
				Assert::AreEqual(uint64_t(960) * 540 * 2 + uint64_t(480) * 270 * 4, VpSkeleton::BytesOf(reg.Get(VpSkeleton::kTdBaseLong).size, d4k));
				Assert::AreEqual(VpSkeleton::BytesOf(reg.Get(VpSkeleton::kTdBaseLong).size, d4k), VpSkeleton::BytesOf(reg.Get(VpSkeleton::kTdBaseShort).size, d4k));
				// A stored picture is more than 10x smaller than the frame's luma at 2 B/px (4K: 1.56 MB per slot vs 16.6 MB).
				Assert::IsTrue(VpSkeleton::BytesOf(reg.Get(VpSkeleton::kTdBaseLong).size, d4k) * 10 < uint64_t(3840) * 2160 * 2);
			}

			TEST_METHOD(VramBudgetPerTierIsTheHandSummedFigure)
			{
				// Hand sums (decimal bytes, stored slots counted: base_long x 2, base_short x 6), written from the layouts in the
				// contract table, not from BytesOf. T0 holds only system-RAM records; T1 the maps and base buffers; T2 drift + offsets.
				struct Row { Dims d; uint64_t t1, t2; };
				const Row rows[] = {
					{ { 1920, 1080 }, 5152320ull, 2106240ull },
					{ { 3840, 2160 }, 20606400ull, 8424000ull },
					{ { 7680, 4320 }, 82425600ull, 33696000ull },
				};
				for (const Row& r : rows)
				{
					const TierBudget b = BudgetAt(r.d);
					Assert::AreEqual(uint64_t(0), b.vram[0], L"T0 has no GPU bytes");
					Assert::AreEqual(r.t1, b.vram[1]);
					Assert::AreEqual(r.t2, b.vram[2]);
					Assert::AreEqual(uint64_t(256 + 4096 + 2048), b.systemRam[0], L"quality pattern + motion model + mirror records");
					Assert::AreEqual(uint64_t(0), b.systemRam[1] + b.systemRam[2]);
				}
				// Even the 8K worst case (store form of the offsets included) is far below the K0 laptop figure for the whole lane.
				Assert::IsTrue(BudgetAt(Dims{ 7680, 4320 }).Vram() < 200ull * 1000 * 1000);
			}

			TEST_METHOD(EveryConsumerReadsSomethingAndTheSetsAreDistinctFromTheEngine)
			{
				for (Consumer c : { Consumer::AdaptiveScaler, Consumer::DitherNeed, Consumer::ChromaStage, Consumer::Engine })
				{
					const std::vector<ProductId> need = NeededBy(c);
					Assert::IsFalse(need.empty());
					for (ProductId p : need)
						Assert::IsTrue(IsReserved(p));
					Assert::IsTrue(FallbackCatalog::FindProductEntry(FallbackIdOf(c)) != nullptr, L"the fallback id exists in the catalog table");
				}
				// Every reserved product is read by somebody (consumers + the engine).
				std::set<int> read;
				for (Consumer c : { Consumer::AdaptiveScaler, Consumer::DitherNeed, Consumer::ChromaStage, Consumer::Engine })
					for (ProductId p : NeededBy(c))
						read.insert(int(p));
				Assert::AreEqual(size_t(kCount), read.size());
			}

			TEST_METHOD(NotRequestedIsNotAFallbackAndChangesNothing)
			{
				for (Consumer c : { Consumer::AdaptiveScaler, Consumer::DitherNeed, Consumer::ChromaStage })
				{
					const ReadResult r = Read(c, PlaceholderSnapshot(), false, 0);
					Assert::IsFalse(r.requested);
					Assert::IsFalse(r.usable);
					Assert::IsFalse(r.fallback.engaged);
					Assert::AreEqual(std::string(FallbackIdOf(c)), r.fallback.id);
					Assert::AreEqual(std::string(), r.reason);
					Assert::AreEqual(std::string(), StatusLine(c, r));
					const ReadResult t = ReadToday(c);
					Assert::IsFalse(t.requested || t.usable || t.fallback.engaged);
					Assert::AreEqual(std::string(), StatusLine(c, t));
				}
				Assert::IsFalse(kEngineRequestedToday, L"no configuration key exists in reset 1");
				for (const Entry& e : PlaceholderSnapshot()->entry)
					Assert::IsFalse(e.published);
			}

			TEST_METHOD(RequestedWithoutTheEngineIsAVisibleFallbackNamingEveryMissingProduct)
			{
				for (Consumer c : { Consumer::AdaptiveScaler, Consumer::DitherNeed, Consumer::ChromaStage })
				{
					const ReadResult placeholder = Read(c, PlaceholderSnapshot(), true, 0);
					Assert::IsTrue(placeholder.requested);
					Assert::IsFalse(placeholder.usable);
					Assert::IsTrue(placeholder.fallback.engaged);
					Assert::AreEqual(std::string(FallbackIdOf(c)), placeholder.fallback.id);
					for (ProductId p : NeededBy(c))
						Assert::IsTrue(placeholder.reason.find(ContractOf(p)->token) != std::string::npos);
					Assert::IsFalse(StatusLine(c, placeholder).empty());
					const ReadResult none = Read(c, nullptr, true, 0);
					Assert::IsTrue(none.fallback.engaged);
					Assert::AreEqual(std::string("engine not built"), none.reason);
					Assert::AreEqual(placeholder.missingMask, none.missingMask);
					Assert::IsTrue(placeholder.fallback.reason.find("classical") != std::string::npos || !placeholder.fallback.reason.empty());
				}
			}

			TEST_METHOD(ReadRecoversByItselfAndRejectsStaleOrForeignVersions)
			{
				const Consumer c = Consumer::AdaptiveScaler;
				Snapshot s = FreshAll(1000);
				Assert::IsTrue(Read(c, &s, true, 2000).usable);
				Assert::IsFalse(Read(c, &s, true, 2000).fallback.engaged);
				// One needed product absent: classical, naming it; an unneeded one absent changes nothing.
				s.entry[IndexOf(VpSkeleton::kTdStability)].published = false;
				ReadResult r = Read(c, &s, true, 2000);
				Assert::IsFalse(r.usable);
				Assert::IsTrue(r.reason.find("td.stability absent") != std::string::npos);
				Assert::IsTrue(Read(Consumer::DitherNeed, &s, true, 2000).usable == false);   // dither needs it too
				Assert::IsTrue(Read(Consumer::ChromaStage, &s, true, 2000).usable, L"chroma does not read stability");
				s.entry[IndexOf(VpSkeleton::kTdStability)].published = true;
				Assert::IsTrue(Read(c, &s, true, 2000).usable, L"recovers by itself");
				// Another unit version.
				s.entry[IndexOf(VpSkeleton::kTdRealDetail)].unitVersion = kUnitVersion + 1;
				r = Read(c, &s, true, 2000);
				Assert::IsFalse(r.usable);
				Assert::IsTrue(r.reason.find("td.real_detail unit version") != std::string::npos);
				s.entry[IndexOf(VpSkeleton::kTdRealDetail)].unitVersion = kUnitVersion;
				// Stale (age limit) and from the future.
				s.entry[IndexOf(VpSkeleton::kTdGrainLevel)].maxAgeUs = 500;
				Assert::IsTrue(Read(c, &s, true, 1400).usable);
				r = Read(c, &s, true, 1600);
				Assert::IsFalse(r.usable);
				Assert::IsTrue(r.reason.find("td.grain_level stale") != std::string::npos);
				Assert::IsFalse(Read(c, &s, true, 400).usable, L"a record newer than the reader by more than its age is not trusted");
				Assert::IsTrue(Read(c, &s, true, 1000).usable);
			}

			TEST_METHOD(DitherNeedDecisionIsUnchangedWhileTheEngineIsNotRequested)
			{
				DitherNeed::FrameArgs a;
				a.grid = RegionEvidence::MakeGrid(16, 8, 8);
				a.dtSeconds = 1.0 / 60;
				a.lutFingerprint = "fp";
				DitherNeed::StateTracker t1, t2;
				const DitherNeed::Decision plain = t1.Decide(a);
				DitherNeed::FrameArgs b = a;
				b.td = nullptr;   // even with no producer at all: not requested, so not a fallback
				const DitherNeed::Decision viaNull = t2.Decide(b);
				Assert::AreEqual(int(plain.result), int(viaNull.result));
				Assert::AreEqual(plain.status, viaNull.status);
				Assert::AreEqual(DitherNeed::StatusOf(true, 0, true), plain.status, L"the status is exactly the classical text");
				Assert::IsFalse(plain.td.fallback.engaged);
				Assert::IsFalse(plain.td.requested);
				// Requested with the placeholder: the classical law still runs (result unchanged), the status says why.
				DitherNeed::FrameArgs r = a;
				r.tdRequested = true;
				DitherNeed::StateTracker t3;
				const DitherNeed::Decision asked = t3.Decide(r);
				Assert::AreEqual(int(plain.result), int(asked.result));
				Assert::AreEqual(plain.reset, asked.reset);
				Assert::AreEqual(plain.off, asked.off);
				Assert::IsTrue(asked.td.fallback.engaged);
				Assert::AreEqual(std::string("product.td_dither"), asked.td.fallback.id);
				Assert::IsTrue(asked.status.find(plain.status) == 0 && asked.status.size() > plain.status.size());
				// Off (setting off): nothing is read, nothing is said.
				DitherNeed::FrameArgs off = r;
				off.enabled = false;
				DitherNeed::StateTracker t4;
				Assert::IsFalse(t4.Decide(off).td.fallback.engaged);
			}

			TEST_METHOD(ChromaCflResultReadsTheEngineProductsAndRunsUnchanged)
			{
				using ChromaUpscale::Plane;
				Plane y(32, 32, 512.0), cb(16, 16, 512.0), cr(16, 16, 512.0);
				for (int j = 0; j < 16; ++j)
					for (int i = 0; i < 16; ++i)
						cb.at(i, j) = 400.0 + 20.0 * i + 5.0 * j;
				const ChromaUpscale::Result r = ChromaUpscale::Upsample(y, cb, cr, -0.5, 0.0, ChromaUpscale::ActiveRect(), 10, 32.0);
				Assert::AreEqual(std::string("ok"), r.status);
				Assert::IsFalse(r.td.requested);
				Assert::IsFalse(r.td.fallback.engaged);
			}

			TEST_METHOD(AdaptiveStageReportsNothingUntilTheEngineIsRequested)
			{
				AdaptiveDownscale::Module module;
				AdaptiveDownscaleStage::Stage stage(module);
				Assert::IsTrue(stage.StatusLines().empty());
				Assert::IsFalse(stage.TemporalDetailRead().requested);
				Assert::IsFalse(stage.TemporalDetailFallback().engaged);
				Assert::AreEqual(std::string("product.td_scaler"), std::string(FallbackIdOf(Consumer::AdaptiveScaler)));
			}
		};
	}
}
