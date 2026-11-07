// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// repair.temporal_detail / products (doc 72, R1-W1-RES round 2). CPU only: the registry entries (ids appended after p7g in doc 72's
// order, ids 16-18 redefined), the contract table and the intended inputs, the budget-governed stages, the U19 TdMapsView accessor,
// the one read API (absent -> classical form, visible) and the five consumer read paths (adaptive scaling, dither need, sigma-delta,
// chroma, deband) being byte-identical to today while the engine is not requested.
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/core/Products.h>
#include <VP-V/modules/output/dither/DitherNeed.h>
#include <VP-V/modules/output/dither/SigmaDeltaVectors.h>
#include <VP-V/modules/repair/deband/VpDebander.h>
#include <VP-V/modules/repair/temporal_detail/products/TdProductContracts.h>
#include <VP-V/modules/repair/temporal_detail/products/TdProductReads.h>
#include <VP-V/modules/repair/temporal_detail/shared/TdMapsView.h>
#include <VP-V/modules/scaling/chroma_upscale/ChromaCfl.h>
#include <VP-V/modules/scaling/output/AdaptiveDownscaleStage.h>

#include <cstdint>
#include <cstring>
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
		using VpSkeleton::kQualityPatternHistorySeconds;
		using VpSkeleton::kQualityPatternMinFrames;
		using VpSkeleton::kRecordHistoryMax;
		using VpSkeleton::kTdTrailSummaryHistorySeconds;
		using VpSkeleton::kTdTrailSummaryMinFrames;
		using VpSkeleton::QualityPatternHistoryFrames;
		using VpSkeleton::TdTrailSummaryHistoryFrames;

		Snapshot FreshAll(int64_t timeUs = 1000)
		{
			Snapshot s;
			for (ProductId p = VpSkeleton::kTdBlocks; p <= VpSkeleton::kTdTrailSummary; ++p)
			{
				if (IndexOf(p) < 0) continue;
				Entry& e = s.entry[IndexOf(p)];
				e.published = true;
				e.unitVersion = ContractOf(p)->unitVersion;
				e.timeUs = timeUs;
			}
			return s;
		}

		constexpr size_t kDeclaredInputsToday = 4;   // the registry's declared input prefix (the contract table has the full lists)

		const Consumer kAllConsumers[] = { Consumer::AdaptiveScaler, Consumer::DitherNeed, Consumer::SigmaDelta, Consumer::ChromaStage, Consumer::Deband };

		TEST_CLASS(TdProductsTests)
		{
		public:
			TEST_METHOD(IdsFollowDoc72AndTheRegistryValidates)
			{
				using namespace VpSkeleton;
				const Registry reg = SkeletonProducts();
				const char* why = nullptr;
				Assert::IsTrue(reg.Validate(&why), L"dense ids, inputs known, no lag-0 cycle, look-ahead rules (P15 has the P15c substitute)");
				// The append order of doc 72 section 1.1, after P14; ids 16-18 keep their numbers.
				Assert::AreEqual(16, int(kTdBlocks));
				Assert::AreEqual(17, int(kTdMemory));
				Assert::AreEqual(18, int(kTdAnchorPass));
				Assert::AreEqual(19, int(kSourceIdentity));
				Assert::AreEqual(20, int(kP7gCameraGuide));
				Assert::AreEqual(21, int(kCodingEvidence));
				Assert::AreEqual(22, int(kQualityPattern));
				Assert::AreEqual(23, int(kQualityPatternCausal));
				Assert::AreEqual(24, int(kRefinedField));
				Assert::AreEqual(25, int(kTdAnchorRefs));
				Assert::AreEqual(26, int(kTdDelta));
				Assert::AreEqual(27, int(kTdMaps));
				Assert::AreEqual(28, int(kTdTrailSummary));
				Assert::AreEqual(29, int(kSourceGeometry), L"wave S2: P14g source_geometry is appended after the doc 72 list");
				Assert::AreEqual(30, int(kSkeletonProductEnd));
				Assert::AreEqual(size_t(kSkeletonProductEnd), reg.Size());
				// The v1 names are aliases of the v2 ids.
				Assert::AreEqual(int(kTdBlocks), int(kTdAnalysis));
				Assert::AreEqual(int(kTdMemory), int(kTdRefstore));
				Assert::AreEqual(int(kTdAnchorPass), int(kTdAdmission));
				// The dropped products are gone: no per-pixel offsets, no per-map reservations.
				for (const ProductDecl& d : reg.All())
				{
					const std::string t = d.token;
					Assert::IsTrue(t != "td.refine_pixel" && t != "td.trail_confidence" && t != "td.grain_level" && t != "td.stability" && t != "td.residual");
				}
				// P15 is a per-frame lane product with leads and the causal substitute; it is NOT P14's held state.
				const ProductDecl& qp = reg.Get(kQualityPattern);
				Assert::IsTrue(qp.shortQueueSubstitute == kQualityPatternCausal);
				Assert::IsTrue(qp.backend == Backend::Cpu && qp.residency == Residency::SystemRam);
				bool lead1 = false, lead2 = false;
				for (size_t k = 0; k < qp.inputCount; ++k)
				{
					Assert::IsTrue(qp.inputs[k].product != kSourceIdentity, L"quality_pattern does not read P14");
					lead1 = lead1 || qp.inputs[k].lead == 1;
					lead2 = lead2 || qp.inputs[k].lead == 2;
				}
				Assert::IsTrue(lead1 && lead2, L"role at lead 2 from coding_evidence(N+1), (N+2)");
				Assert::AreEqual(int(kRecordHistoryMax), int(qp.history), L"unmeasured rate: the field maximum, never a hard-coded rate");
				for (size_t k = 0; k < reg.Get(kQualityPatternCausal).inputCount; ++k)
					Assert::IsTrue(reg.Get(kQualityPatternCausal).inputs[k].lead == 0 && reg.Get(kQualityPatternCausal).inputs[k].product != kQualityPattern);
				// td.anchor_pass is Transient (the arena); td.memory is the whole store with no history.
				Assert::IsTrue(reg.Get(kTdAnchorPass).residency == Residency::Transient);
				Assert::AreEqual(0, int(reg.Get(kTdMemory).history));
			}

			TEST_METHOD(RecordHistoriesAreSecondsOfTheMeasuredSourceRate)
			{
				// 24000/1001 p, 25 p, 30 p, 59.94 p, 120 p: frames = ceil(seconds / period), within [structural floor, kRecordHistoryMax (223)].
				const double periods[] = { 1001.0 / 24.0, 40.0, 1001.0 / 30.0, 1001.0 / 60.0, 1000.0 / 120.0 };
				for (double period : periods)
				{
					const Registry reg = VpSkeleton::SkeletonProducts(false, false, period);
					const int qp = int(reg.Get(VpSkeleton::kQualityPattern).history);
					const int ts = int(reg.Get(VpSkeleton::kTdTrailSummary).history);
					Assert::AreEqual(int(QualityPatternHistoryFrames(period)), qp);
					Assert::AreEqual(int(TdTrailSummaryHistoryFrames(period)), ts);
					Assert::IsTrue(qp >= int(kQualityPatternMinFrames) && qp <= int(kRecordHistoryMax));
					Assert::IsTrue(ts >= int(kTdTrailSummaryMinFrames) && ts <= int(kRecordHistoryMax));
					if (qp < int(kRecordHistoryMax))
						Assert::IsTrue(qp * period >= kQualityPatternHistorySeconds * 1000.0 - 1e-6 && (qp - 1) * period < kQualityPatternHistorySeconds * 1000.0 + 1e-6,
							L"the window is the seconds, to within one frame");
					if (ts > int(kTdTrailSummaryMinFrames) && ts < int(kRecordHistoryMax))
						Assert::IsTrue(ts * period >= kTdTrailSummaryHistorySeconds * 1000.0 - 1e-6 && (ts - 1) * period < kTdTrailSummaryHistorySeconds * 1000.0 + 1e-6);
				}
				// A faster source needs more frames for the same seconds; a slower one fewer.
				Assert::IsTrue(QualityPatternHistoryFrames(1000.0 / 60) > QualityPatternHistoryFrames(1000.0 / 24));
				Assert::IsTrue(TdTrailSummaryHistoryFrames(1000.0 / 60) > TdTrailSummaryHistoryFrames(1000.0 / 24));
				// Unmeasured (<= 0) declares the maximum; the structural floor holds for an absurdly slow source.
				Assert::AreEqual(int(kRecordHistoryMax), int(QualityPatternHistoryFrames(0.0)));
				Assert::AreEqual(int(kRecordHistoryMax), int(TdTrailSummaryHistoryFrames(-1.0)));
				Assert::AreEqual(int(kQualityPatternMinFrames), int(QualityPatternHistoryFrames(60000.0)));
				Assert::AreEqual(int(kTdTrailSummaryMinFrames), int(TdTrailSummaryHistoryFrames(60000.0)));
			}

			TEST_METHOD(ContractsCoverEveryEngineProductAndTheRegistryIsAPrefixOfTheIntendedInputs)
			{
				const Registry reg = SkeletonProducts();
				Assert::AreEqual(size_t(kCount), Contracts().size());
				std::set<std::string> tokens;
				for (const Contract& c : Contracts())
				{
					Assert::IsTrue(IsEngineProduct(c.id));
					Assert::AreEqual(std::string(reg.Get(c.id).token), std::string(c.token));
					Assert::IsTrue(tokens.insert(c.token).second, L"unique tokens");
					Assert::IsTrue(c.layout[0] != '\0' && c.fillLane[0] != '\0' && c.fillLive[0] != '\0' && c.fillReduced[0] != '\0');
					Assert::IsTrue(ContractOf(c.id) == &c);
					const VpSkeleton::ProductDecl& d = reg.Get(c.id);
					// The registry never silently drops an input: it declares the first kDeclaredInputsToday of the intended list, in order.
					// W1-SKG raised the framework limit to kMaxInputs = 16, but the declarations have NOT caught up yet (open item of the
					// hand-back: the lead / short-queue edges of the longer lists need their own decisions); the first four are declared.
					const size_t want = c.intended.size() < kDeclaredInputsToday ? c.intended.size() : kDeclaredInputsToday;
					Assert::AreEqual(want, size_t(d.inputCount), (std::wstring(L"declared inputs of ") + std::wstring(c.token, c.token + std::strlen(c.token))).c_str());
					for (size_t k = 0; k < want; ++k)
					{
						Assert::AreEqual(int(c.intended[k].product), int(d.inputs[k].product));
						Assert::AreEqual(int(c.intended[k].lag), int(d.inputs[k].lag));
						Assert::AreEqual(int(c.intended[k].lead), int(d.inputs[k].lead));
					}
				}
				// Recorded, not hidden: these three intended lists exceed the declared prefix today (the catch-up is an open item).
				using namespace VpSkeleton;
				Assert::IsTrue(ContractOf(kTdBlocks)->intended.size() > kDeclaredInputsToday && size_t(reg.Get(kTdBlocks).inputCount) == kDeclaredInputsToday);
				Assert::IsTrue(ContractOf(kCodingEvidence)->intended.size() > kDeclaredInputsToday);
				Assert::IsTrue(ContractOf(kTdAnchorPass)->intended.size() > kDeclaredInputsToday);
				Assert::IsNull(ContractOf(kSourceIdentity));
				Assert::AreEqual(2u, kUnitVersion);
			}

			TEST_METHOD(CodingEvidenceAndQualityPatternRecordTheProvisionalSourceGeometryRead)
			{
				// Wave S2 (doc 78 section 6): P14g source_geometry is a HELD-STATE record read at lag 1 by coding_evidence and quality_pattern
				// (grid placement, sharpening discount). The contract table records it (the registry still declares the first four inputs).
				using namespace VpSkeleton;
				const Registry reg = SkeletonProducts();
				for (ProductId id : { kCodingEvidence, kQualityPattern })
				{
					const Contract* c = ContractOf(id);
					Assert::IsNotNull(c);
					bool found = false;
					for (const Input& in : c->intended)
						if (in.product == kSourceGeometry)
						{
							found = true;
							Assert::AreEqual(1, int(in.lag), L"a held-state read is lag 1");
							Assert::AreEqual(0, int(in.lead));
						}
					Assert::IsTrue(found, L"recorded in the intended inputs");
					for (size_t k = 0; k < reg.Get(id).inputCount; ++k)
						Assert::IsTrue(reg.Get(id).inputs[k].product != kSourceGeometry, L"not (yet) a declared registry input: the catalog row carries the read");
				}
				// It is not an engine product: no contract of its own, and the engine's read sets are unchanged.
				Assert::IsNull(ContractOf(kSourceGeometry));
				Assert::IsFalse(IsEngineProduct(kSourceGeometry));
			}

			TEST_METHOD(GovernedStagesAreWholeBuffersFloorFirstAndTheRegistryDeclaresTheFloor)
			{
				const Registry reg = SkeletonProducts();
				const Dims d4k = { 3840, 2160 };
				for (const Contract& c : Contracts())
				{
					if (c.stages.empty()) continue;
					Assert::AreEqual(VpSkeleton::BytesOf(reg.Get(c.id).size, d4k), VpSkeleton::BytesOf(c.stages[0].size, d4k), L"the registry declares stage 0");
					for (size_t i = 1; i < c.stages.size(); ++i)
						Assert::IsTrue(VpSkeleton::BytesOf(c.stages[i].size, d4k) >= VpSkeleton::BytesOf(c.stages[i - 1].size, d4k), L"stages only add");
				}
				// Only td.memory, td.anchor_refs and td.delta are governed (the large buffers).
				size_t governed = 0;
				for (const Contract& c : Contracts()) governed += c.stages.empty() ? 0u : 1u;
				Assert::AreEqual(size_t(3), governed);
				// The memory floor is the budget-governed stage 0: + still/dark floor + full-resolution mid band (2 B/px at full resolution).
				Assert::AreEqual(uint64_t(960) * 540 * 55 + uint64_t(1920) * 1080 + uint64_t(3840) * 2160 * 2, BytesAt(reg, VpSkeleton::kTdMemory, d4k, 0));
				Assert::AreEqual(uint64_t(0), BytesAt(reg, VpSkeleton::kTdAnchorRefs, d4k, 0));
				// No fixed per-GPU tier wording anywhere in the table.
				for (const Contract& c : Contracts())
					for (const Stage& s : c.stages)
					{
						const std::string n = s.name;
						Assert::IsTrue(n.find("3090") == std::string::npos && n.find("laptop") == std::string::npos);
					}
			}

			TEST_METHOD(BudgetAtThe4KFrameIsTheHandSummedFigure)
			{
				// Hand sums (decimal bytes) from the layouts, not from BytesOf. VRAM floor: td.blocks 1,684,800 + td.memory 47,174,400 +
				// td.delta 3,240,000 + td.maps 777,600 + coding_evidence 1,037,056 + refined_field 1,036,800 (anchor_refs 0).
				const Dims d4k = { 3840, 2160 };
				const Budget floor = BudgetAt(d4k, false);
				Assert::AreEqual(uint64_t(54950656), floor.vram);
				Assert::AreEqual(uint64_t(24883200), floor.transient, L"anchor pass arena peak: 3 B/px");
				Assert::AreEqual(uint64_t((64 + 64 + 256) * int(kRecordHistoryMax)), floor.systemRam, L"quality pattern, causal, trail summary at the unmeasured bound (history kRecordHistoryMax counted)");
				{   // Measured 24000/1001 p: quality pattern + causal 48 frames, trail summary 6 (2 s and 0.25 s of source time).
					const Budget at24 = BudgetAt(VpSkeleton::SkeletonProducts(false, false, 1001.0 / 24.0), d4k, false);
					Assert::AreEqual(uint64_t(64 * 48 * 2 + 256 * 6), at24.systemRam);
					Assert::AreEqual(floor.vram, at24.vram, L"the GPU figures do not depend on the rate");
				}
				// Full stages: td.memory 175,737,600, anchor_refs 49,766,400, delta 11,534,400.
				const Budget full = BudgetAt(d4k, true);
				Assert::AreEqual(uint64_t(241574656), full.vram);
				Assert::AreEqual(floor.systemRam, full.systemRam);
				Assert::IsTrue(full.vram > floor.vram);
				// The floor at 1080p is smaller and at 8K larger (no hidden fixed tier).
				Assert::IsTrue(BudgetAt(Dims{ 1920, 1080 }, false).vram < floor.vram);
				Assert::IsTrue(BudgetAt(Dims{ 7680, 4320 }, false).vram > floor.vram);
			}

			TEST_METHOD(TdMapsViewTwelveNamedFieldsInThreePlanesWithOneAccessor)
			{
				using namespace TdMapsView;
				Assert::AreEqual(1u, TdMapsView::kUnitVersion);
				Assert::IsTrue(VersionMatches(1) && !VersionMatches(2) && !VersionMatches(0));
				std::set<std::string> names;
				for (int f = 0; f < kFields; ++f)
				{
					Assert::IsTrue(names.insert(Name(f)).second);
					Assert::AreEqual(f / 4, PlaneOf(f));
					Assert::AreEqual(f % 4, ComponentOf(f));
				}
				Block b;
				for (int f = 0; f < kFields; ++f) b.f[f] = 0.01f * (f + 1);
				float planes[kPlanes][4];
				Pack(b, planes);
				const Block r = Unpack(planes);
				for (int f = 0; f < kFields; ++f) Assert::AreEqual(b.f[f], Fetch(r, Field(f)));
				// Each reader reads only its fields (doc 72 section 1.2 card 10).
				Assert::AreEqual(Bit(kRealDetail) | Bit(kStability) | Bit(kCodingNoise), FieldsOf(Reader::AdaptiveScaler));
				Assert::AreEqual(Bit(kOwnGrain) | Bit(kStability), FieldsOf(Reader::DitherNeed));
				Assert::AreEqual(Bit(kStability), FieldsOf(Reader::SigmaDelta));
				Assert::AreEqual(Bit(kValidity) | Bit(kTrailConfidence0), FieldsOf(Reader::ChromaStage));
				Assert::AreEqual(Bit(kValidity) | Bit(kStability), FieldsOf(Reader::Deband));
				for (Consumer c : kAllConsumers)
					Assert::IsTrue(FieldsOf(MapsReaderOf(c)) != 0u);
				const std::string glsl = Glsl();
				for (int f = 0; f < kFields; ++f)
					Assert::IsTrue(glsl.find(std::string("vpu_td_maps_") + Name(f) + "(ivec2 b)") != std::string::npos);
				Assert::IsTrue(glsl.find("2 * VPU_TD_MAPS_GH") != std::string::npos, L"plane 2 sits two block-heights down");
			}

			TEST_METHOD(EveryConsumerReadsTdMapsExceptTheEngineAndEveryEngineProductHasAReader)
			{
				for (Consumer c : kAllConsumers)
				{
					const std::vector<ProductId> need = NeededBy(c);
					bool maps = false;
					for (ProductId p : need) { Assert::IsTrue(IsEngineProduct(p)); maps = maps || p == VpSkeleton::kTdMaps; }
					Assert::IsTrue(maps, L"every consumer reads td.maps through U19");
					Assert::IsTrue(FallbackCatalog::FindProductEntry(FallbackIdOf(c)) != nullptr, L"the fallback id exists in the catalog table");
				}
				Assert::AreEqual(std::string("product.adaptive_downscale"), std::string(FallbackIdOf(Consumer::AdaptiveScaler)));
				Assert::AreEqual(std::string("product.debander"), std::string(FallbackIdOf(Consumer::Deband)));
				Assert::IsTrue(FallbackCatalog::FindProductEntry(FallbackIdOf(Consumer::Engine)) != nullptr);
				std::set<int> read;
				for (Consumer c : { Consumer::AdaptiveScaler, Consumer::DitherNeed, Consumer::SigmaDelta, Consumer::ChromaStage, Consumer::Deband, Consumer::Engine })
					for (ProductId p : NeededBy(c)) read.insert(int(p));
				// td.blocks / td.memory / td.anchor_pass and the causal pattern are read by the module row / as the substitute.
				for (const Contract& c : Contracts())
				{
					if (c.id == VpSkeleton::kTdBlocks || c.id == VpSkeleton::kTdMemory || c.id == VpSkeleton::kTdAnchorPass || c.id == VpSkeleton::kQualityPatternCausal)
						continue;
					Assert::IsTrue(read.count(int(c.id)) == 1, (std::wstring(L"no reader: ") + std::wstring(c.token, c.token + std::strlen(c.token))).c_str());
				}
			}

			TEST_METHOD(NotRequestedIsNotAFallbackAndChangesNothing)
			{
				for (Consumer c : kAllConsumers)
				{
					const ReadResult r = Read(c, PlaceholderSnapshot(), false, 0);
					Assert::IsFalse(r.requested || r.usable || r.fallback.engaged);
					Assert::AreEqual(std::string(), r.fallback.id, L"not requested: no id built (allocation-free per-frame read)");
					Assert::AreEqual(0u, r.causeMask);
					Assert::AreEqual(std::string(), r.reason);
					Assert::AreEqual(std::string(), StatusLine(c, r));
					const ReadResult t = ReadToday(c);
					Assert::IsFalse(t.requested || t.usable || t.fallback.engaged);
					// The per-frame read builds no heap string: the ids / reason stay in the small-string buffer, and the stored lists and keys are reused.
					Assert::IsTrue(t.fallback.id.empty() && t.fallback.reason.empty() && t.reason.empty());
					Assert::IsTrue(t.fallback.id.capacity() == std::string().capacity() && t.reason.capacity() == std::string().capacity());
					Assert::IsTrue(&NeededBy(c) == &NeededBy(c), L"NeededBy returns the stored list");
					Assert::IsTrue(&FallbackKey(c, kCauseAbsent) == &FallbackKey(c, kCauseAbsent), L"keys are built once");
				}
				Assert::IsFalse(kEngineRequestedToday, L"no configuration key exists in reset 1");
				for (const Entry& e : PlaceholderSnapshot()->entry)
					Assert::IsFalse(e.published);
			}

			TEST_METHOD(RequestedWithoutTheEngineIsAVisibleFallbackNamingEveryMissingProduct)
			{
				for (Consumer c : kAllConsumers)
				{
					const ReadResult placeholder = Read(c, PlaceholderSnapshot(), true, 0);
					Assert::IsTrue(placeholder.requested && !placeholder.usable && placeholder.fallback.engaged);
					Assert::AreEqual(FallbackKey(c, kCauseAbsent), placeholder.fallback.id, L"keyed by consumer + cause");
					Assert::IsTrue(placeholder.fallback.id != std::string(FallbackIdOf(c)), L"never the consumer's bare id");
					for (ProductId p : NeededBy(c))
						Assert::IsTrue(placeholder.reason.find(ContractOf(p)->token) != std::string::npos);
					Assert::IsFalse(StatusLine(c, placeholder).empty());
					const ReadResult none = Read(c, nullptr, true, 0);
					Assert::IsTrue(none.fallback.engaged);
					Assert::AreEqual(std::string("engine not built"), none.reason);
					Assert::AreEqual(FallbackKey(c, kCauseNoEngine), none.fallback.id);
					Assert::AreEqual(placeholder.missingMask, none.missingMask);
				}
			}

			TEST_METHOD(FallbackKeysArePerIdAndCauseAndNeverTheConsumersBareId)
			{
				const Consumer c = Consumer::ChromaStage;
				Snapshot s = FreshAll(1000);
				s.entry[IndexOf(VpSkeleton::kTdMaps)].published = false;
				const ReadResult absent = Read(c, &s, true, 2000);
				Snapshot t = FreshAll(1000);
				t.entry[IndexOf(VpSkeleton::kTdDelta)].maxAgeUs = 100;
				const ReadResult stale = Read(c, &t, true, 2000);
				const ReadResult nothing = Read(c, nullptr, true, 2000);
				Assert::AreEqual(unsigned(kCauseAbsent), absent.causeMask);
				Assert::AreEqual(unsigned(kCauseStale), stale.causeMask);
				Assert::AreEqual(unsigned(kCauseNoEngine), nothing.causeMask);
				Assert::IsTrue(absent.fallback.id != stale.fallback.id && stale.fallback.id != nothing.fallback.id && absent.fallback.id != nothing.fallback.id);
				for (const ReadResult* r : { &absent, &stale, &nothing })
					Assert::IsTrue(r->fallback.id.compare(0, std::strlen(FallbackIdOf(c)), FallbackIdOf(c)) == 0 && r->fallback.id != FallbackIdOf(c));
				// Every (consumer, cause set) has its own key; the keys carry no heading separator.
				std::set<std::string> all;
				for (Consumer k : { Consumer::AdaptiveScaler, Consumer::DitherNeed, Consumer::SigmaDelta, Consumer::ChromaStage, Consumer::Deband, Consumer::Engine })
					for (unsigned m = 1; m <= kCauseAll; ++m)
					{
						const std::string& key = FallbackKey(k, m);
						Assert::IsTrue(!key.empty() && key.find('(') == std::string::npos && key.find("; ") == std::string::npos);
						Assert::IsTrue(all.insert(key).second, L"keys are unique");
					}
				Assert::IsTrue(FallbackKey(c, 0).empty());
				// Registry: the consumer's own classical cause (bare id) and the engine's two causes are three entries, none overwrites another.
				FallbackStatus::Registry reg;
				FallbackCatalog::Sync(reg, FallbackCatalog::ProductFallback(FallbackIdOf(c), "chroma split unavailable"));
				Assert::IsTrue(SyncRead(reg, c, absent));
				Assert::AreEqual(size_t(2), reg.Entries().size());
				Assert::IsFalse(SyncRead(reg, c, absent), L"unchanged read: heading unchanged");
				Assert::IsTrue(SyncRead(reg, c, stale), L"another cause replaces the first");
				Assert::AreEqual(size_t(2), reg.Entries().size());
				Assert::IsFalse(reg.Engaged(absent.fallback.id));
				Assert::IsTrue(reg.Engaged(stale.fallback.id));
				Assert::IsTrue(reg.Engaged(FallbackIdOf(c)), L"the consumer's own cause is untouched");
				// The GUI notice sees two distinct ids.
				FallbackStatus::Notices notices;
				Assert::AreEqual(size_t(2), notices.Take(reg.Heading()).size());
				Assert::IsTrue(SyncRead(reg, c, absent));
				Assert::AreEqual(size_t(1), notices.Take(reg.Heading()).size(), L"a new cause is announced once");
				// Recovery releases the cause key only.
				Assert::IsTrue(SyncRead(reg, c, ReadResult()));
				Assert::AreEqual(size_t(1), reg.Entries().size());
				Assert::IsTrue(reg.Engaged(FallbackIdOf(c)));
				// A mixed read has its own key (both causes named).
				Snapshot m2 = FreshAll(1000);
				m2.entry[IndexOf(VpSkeleton::kTdMaps)].published = false;
				m2.entry[IndexOf(VpSkeleton::kTdDelta)].maxAgeUs = 100;
				const ReadResult mixed = Read(c, &m2, true, 2000);
				Assert::AreEqual(unsigned(kCauseAbsent | kCauseStale), mixed.causeMask);
				Assert::IsTrue(mixed.fallback.id.find("absent+stale") != std::string::npos);
			}

			TEST_METHOD(AnEngagedReadBuildsNoStringWhileItsCausesAreUnchanged)
			{
				const Consumer c = Consumer::ChromaStage;   // td.maps, td.delta, td.memory, refined_field
				Snapshot s = FreshAll(1000);
				s.entry[IndexOf(VpSkeleton::kTdMaps)].published = false;
				const ReadResult& first = Read(c, &s, true, 2000);
				Assert::IsTrue(first.fallback.engaged && first.reason.find("td.maps absent") != std::string::npos);
				const char* reasonText = first.reason.c_str();
				const char* idText = first.fallback.id.c_str();
				const char* whyText = first.fallback.reason.c_str();
				for (int frame = 0; frame < 5; ++frame)
				{
					const ReadResult& again = Read(c, &s, true, 2000 + frame);
					Assert::IsTrue(&again == &first, L"one cached result per consumer and thread");
					Assert::IsTrue(again.reason.c_str() == reasonText && again.fallback.id.c_str() == idText && again.fallback.reason.c_str() == whyText,
						L"the strings are not rebuilt while the causes are unchanged");
					Assert::IsTrue(again.fallback.engaged && again.causeMask == kCauseAbsent);
				}
				// The SAME cause mask for another product is another text: the cache keys on the products and their causes, not the mask alone.
				Snapshot t = FreshAll(1000);
				t.entry[IndexOf(VpSkeleton::kTdDelta)].published = false;
				const ReadResult& other = Read(c, &t, true, 2000);
				Assert::AreEqual(unsigned(kCauseAbsent), other.causeMask);
				Assert::IsTrue(other.reason.find("td.delta absent") != std::string::npos && other.reason.find("td.maps") == std::string::npos);
				Assert::IsTrue(other.fallback.reason.find("td.delta absent") != std::string::npos);
				// A cause change rebuilds; recovery clears the strings; the next engagement is correct again.
				Snapshot st = FreshAll(1000);
				st.entry[IndexOf(VpSkeleton::kTdDelta)].maxAgeUs = 100;
				const ReadResult& stale = Read(c, &st, true, 2000);
				Assert::AreEqual(unsigned(kCauseStale), stale.causeMask);
				Assert::IsTrue(stale.reason.find("td.delta stale") != std::string::npos);
				const Snapshot fresh = FreshAll(1000);
				const ReadResult& ok = Read(c, &fresh, true, 2000);
				Assert::IsTrue(ok.usable && !ok.fallback.engaged && ok.reason.empty() && ok.fallback.id.empty());
				const ReadResult& again = Read(c, &s, true, 2000);
				Assert::IsTrue(again.fallback.engaged && again.reason.find("td.maps absent") != std::string::npos);
				Assert::AreEqual(FallbackKey(c, kCauseAbsent), again.fallback.id);
				// No producer at all, then not requested.
				Assert::AreEqual(std::string("engine not built"), Read(c, nullptr, true, 0).reason);
				Assert::IsTrue(Read(c, nullptr, false, 0).reason.empty());
			}

			TEST_METHOD(ReadRecoversByItselfAndRejectsStaleOrForeignVersions)
			{
				const Consumer c = Consumer::AdaptiveScaler;   // td.maps + refined_field
				Snapshot s = FreshAll(1000);
				Assert::IsTrue(Read(c, &s, true, 2000).usable);
				Assert::IsFalse(Read(c, &s, true, 2000).fallback.engaged);
				s.entry[IndexOf(VpSkeleton::kTdMaps)].published = false;
				ReadResult r = Read(c, &s, true, 2000);
				Assert::IsFalse(r.usable);
				Assert::IsTrue(r.reason.find("td.maps absent") != std::string::npos);
				Assert::IsTrue(r.fallback.reason.find("temporal-detail products unavailable") != std::string::npos, L"the wording is the engine's, not another cause's");
				Assert::IsFalse(Read(Consumer::DitherNeed, &s, true, 2000).usable);
				Assert::IsFalse(Read(Consumer::Deband, &s, true, 2000).usable, L"deband reads td.maps too");
				s.entry[IndexOf(VpSkeleton::kTdMaps)].published = true;
				Assert::IsTrue(Read(c, &s, true, 2000).usable, L"recovers by itself");
				// Another unit version of the map's accessor unit (U19).
				s.entry[IndexOf(VpSkeleton::kTdMaps)].unitVersion = TdMapsView::kUnitVersion + 1;
				r = Read(c, &s, true, 2000);
				Assert::IsFalse(r.usable);
				Assert::IsTrue(r.reason.find("td.maps unit version") != std::string::npos);
				s.entry[IndexOf(VpSkeleton::kTdMaps)].unitVersion = TdMapsView::kUnitVersion;
				// refined_field belongs to U15: its own version; a stale record; one from the future.
				s.entry[IndexOf(VpSkeleton::kRefinedField)].unitVersion = kFieldRefineUnit + 1;
				Assert::IsTrue(Read(c, &s, true, 2000).reason.find("refined_field unit version") != std::string::npos);
				s.entry[IndexOf(VpSkeleton::kRefinedField)].unitVersion = kFieldRefineUnit;
				s.entry[IndexOf(VpSkeleton::kRefinedField)].maxAgeUs = 500;
				Assert::IsTrue(Read(c, &s, true, 1400).usable);
				Assert::IsTrue(Read(c, &s, true, 1600).reason.find("refined_field stale") != std::string::npos);
				Assert::IsFalse(Read(c, &s, true, 400).usable, L"newer than the reader by more than its age is not trusted");
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
				b.td = nullptr;
				const DitherNeed::Decision viaNull = t2.Decide(b);
				Assert::AreEqual(int(plain.result), int(viaNull.result));
				Assert::AreEqual(plain.status, viaNull.status);
				Assert::AreEqual(DitherNeed::StatusOf(true, 0, true), plain.status, L"the status is exactly the classical text");
				Assert::IsFalse(plain.td.fallback.engaged || plain.td.requested);
				DitherNeed::FrameArgs r = a;
				r.tdRequested = true;
				DitherNeed::StateTracker t3;
				const DitherNeed::Decision asked = t3.Decide(r);
				Assert::AreEqual(int(plain.result), int(asked.result));
				Assert::AreEqual(plain.reset, asked.reset);
				Assert::AreEqual(plain.off, asked.off);
				Assert::IsTrue(asked.td.fallback.engaged);
				Assert::AreEqual(FallbackKey(Consumer::DitherNeed, kCauseAbsent), asked.td.fallback.id);
				Assert::AreEqual(std::string("product.dither_need [td: absent]"), asked.td.fallback.id);
				Assert::IsTrue(asked.status.find(plain.status) == 0 && asked.status.size() > plain.status.size());
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
				Assert::IsFalse(r.td.requested || r.td.fallback.engaged);
				Assert::AreEqual(std::string(), r.td.fallback.id, L"not requested: no id string is built per frame");
			}

			TEST_METHOD(AdaptiveStageSigmaDeltaAndDebanderReportNothingUntilTheEngineIsRequested)
			{
				AdaptiveDownscale::Module module;
				AdaptiveDownscaleStage::Stage stage(module);
				Assert::IsTrue(stage.StatusLines().empty());
				Assert::IsFalse(stage.TemporalDetailRead().requested);
				Assert::IsFalse(stage.TemporalDetailFallback().engaged);
				SigmaDeltaVectors::NextVectorGuard guard;
				Assert::IsFalse(guard.Apply(nullptr, 0, nullptr), L"no hooks: nothing applied");
				Assert::IsFalse(guard.TemporalDetailRead().requested || guard.TemporalDetailRead().fallback.engaged);
				Assert::AreEqual(std::string(), guard.TemporalDetailRead().fallback.id);
				VpDebander::Module deband;
				pl_frame src{}, dst{};
				Assert::IsFalse(deband.Apply(nullptr, nullptr, src, VpDebander::Params(), dst), L"nothing to deband");
				Assert::IsFalse(deband.TemporalDetailRead().requested || deband.TemporalDetailRead().fallback.engaged);
				Assert::AreEqual(std::string(), deband.TemporalDetailRead().fallback.id);
			}
		};
	}
}
