// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// SA0: the product catalog (src/VP-V/core/ProductCatalog.h): one node per product over the full consumer set, fallbacks,
// live-fill ownership, unit versions, VRAM per tier, the generated docs/design/product-graph.md region and the
// "no private product descriptors" scan. CPU only.

#include <VP-V/core/ProductCatalog.h>
#include <VP-V/modules/input/source_analysis/lane/LaneCpuProducts.h>
#include <VP-V/modules/input/source_analysis/light_level/LightLevelLut.h>
#include <VP-V/modules/input/source_analysis/SourceResolutionDetector.h>
#include <VP-V/modules/input/source_analysis/identity/SourceIdProduct.h>
#include <VP-V/modules/input/source_analysis/regions/RegionEvidence.h>
#include <VP-V/modules/scaling/rebase/RebaseDecision.h>
#include <VP-V/modules/repair/grain_split/GrainSplit.h>
#include <VP-V/modules/repair/temporal_detail/TemporalDetail.h>
#include <VP-V/modules/repair/temporal_detail/products/TdProductContracts.h>

#include "SettingsTestSupport.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace ProductCatalogTestsNs
{
	using namespace VpSkeleton;
	using namespace VpSkeleton::Catalog;

	GraphSettings Settings(const Dims& d, uint8_t laneLead, bool ofa = true)
	{
		GraphSettings gs;
		gs.dims = d;
		gs.laneLead = laneLead;
		gs.queueDepth = 6;
		gs.lookAheadWalk = 2;
		gs.ofaPresent = ofa;
		return gs;
	}

	std::string Normalise(std::string s)
	{
		s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
		return s;
	}

	std::string Lower(std::string s)
	{
		std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
		std::replace(s.begin(), s.end(), '/', '\\');
		return s;
	}

	TEST_CLASS(ProductCatalogTests)
	{
	public:
		TEST_METHOD(FullGraphHasExactlyOneNodePerProduct)
		{
			const Registry reg = SkeletonProducts();
			Assert::IsTrue(reg.Validate());
			const Dims d = { 3840, 2160 };
			for (int ofa = 0; ofa <= 1; ++ofa)
				for (uint8_t lane : { uint8_t(0), uint8_t(1), uint8_t(3) }) {
					const GraphSettings gs = Settings(d, lane, ofa != 0);
					const Graph g = FullGraph(gs, reg);
					Assert::IsTrue(g.valid, L"full graph valid");
					std::set<int> seen;
					for (const Node& n : g.nodes) {
						Assert::IsTrue(n.id < reg.Size(), L"no unknown ids");
						Assert::IsTrue(seen.insert(int(n.id)).second, L"one node per product");
					}
					for (const ConsumerRow& c : ConsumerRows())
						for (size_t k = 0; k < c.decl.count; ++k) {
							const ProductId asked = c.decl.consumes[k].product;
							Assert::IsTrue(reg.Has(asked), L"consumer asks for a registered product");
							bool reduced = false;
							const ProductId made = Resolve(reg, asked, gs, &reduced);
							Assert::IsTrue(g.Has(made), L"every consumer's product resolves to a node");
						}
					// Spot the substitutions.
					Assert::AreEqual(ofa != 0, g.Has(kP2VectorPair));
					Assert::AreEqual(ofa == 0, g.Has(kP2mMatcherPair));
					Assert::AreEqual(lane >= 2, g.Has(kP9CameraSmoothed));
				}
		}

		TEST_METHOD(ConsumersAreUniqueAndEachDeclaresAnExistingFallback)
		{
			std::set<int> ids;
			std::set<uint32_t> keys;
			const std::string fallbackText = SettingsTestSupport::ReadFile(SettingsTestSupport::RepoRoot()
				+ "src\\VP-V\\core\\FallbackCatalog.h");
			Assert::IsTrue(!fallbackText.empty(), L"FallbackCatalog.h readable");
			for (const ConsumerRow& c : ConsumerRows()) {
				Assert::IsTrue(ids.insert(int(c.decl.id)).second, L"unique consumer ids");
				Assert::IsTrue(keys.insert(PathwayKey(c)).second, L"unique pathway order");
				Assert::IsTrue(c.fallbackId != nullptr && c.fallbackId[0] != '\0', L"a fallback is declared");
				Assert::IsNotNull(FallbackCatalog::FindProductEntry(c.fallbackId), L"the fallback id exists in the catalog table");
				Assert::IsTrue(fallbackText.find(std::string("\"") + c.fallbackId + "\"") != std::string::npos,
					L"the fallback id appears in FallbackCatalog.h");
				Assert::IsTrue(c.decl.hasReducedMode);
				Assert::IsTrue(c.module != nullptr && c.module[0] != '\0');
			}
			Assert::IsTrue(ConsumerRows().size() >= 19 + size_t(VectorDemand::kConsumerCount) + size_t(CameraLink::kCameraConsumerCount));
		}

		TEST_METHOD(ModuleBuildersAgreeWithTheCatalog)
		{
			for (int i = 0; i < VectorDemand::kConsumerCount; ++i) {
				const ConsumerDecl want = VectorDemand::DeclarationOf(VectorDemand::ConsumerAt(i));
				bool found = false;
				for (const ConsumerRow& c : ConsumerRows())
					if (c.decl.id == want.id) {
						found = true;
						Assert::AreEqual(int(want.count), int(c.decl.count));
						for (size_t k = 0; k < want.count; ++k)
							Assert::AreEqual(int(want.consumes[k].product), int(c.decl.consumes[k].product));
						Assert::AreEqual(want.holdsSpan, c.decl.holdsSpan);
					}
				Assert::IsTrue(found, L"vector-demand consumer present");
			}
			for (int i = 0; i < CameraLink::kCameraConsumerCount; ++i) {
				const ConsumerDecl want = CameraLink::DeclarationOf(CameraLink::ConsumerAt(i));
				bool found = false;
				for (const ConsumerRow& c : ConsumerRows())
					if (c.decl.id == want.id) {
						found = true;
						Assert::AreEqual(int(want.count), int(c.decl.count));
						for (size_t k = 0; k < want.count; ++k) {
							Assert::AreEqual(int(want.consumes[k].product), int(c.decl.consumes[k].product));
							Assert::AreEqual(int(want.consumes[k].lag), int(c.decl.consumes[k].lag));
						}
					}
				Assert::IsTrue(found, L"camera consumer present");
			}
		}

		TEST_METHOD(LiveFillOwnerMatchesAnIndependentlyWrittenTable)
		{
			// Expected owners written by hand from the pathway order (input < motion < repair < scaling < ... , render
			// order within a unit) and the declared reads, NOT computed with the catalog's own functions. Only reads of
			// the same frame (lag 0) can fill a slot live, so camera measurement (P7 at lag 1) is never an owner.
			struct Expect { ProductId id; const char* family; };
			const Expect expected[] = {
				{ kP0RebaseDecision, "entrance stages" },
				{ kP1HalfLuma, "optical-flow producer" },
				{ kP2VectorPair, "optical-flow producer" },
				{ kP3AnalysisField, "entrance stages" },       // the entrance segment (input 45) reads the analysis field
				{ kP4SourceStats, "source resolution monitor" },
				{ kP5MonitorTiles, "source resolution monitor" },
				{ kP6EntranceAhead, "entrance stages" },
				{ kP7CameraFit, "temporal detail" },        // camera TD (repair 12): measurement reads P7 at lag 1 only
				{ kP8CameraLabels, "occlusion mask" },      // camera occlusion filler (motion 45) precedes the repair readers
				{ kP9CameraSmoothed, "camera object split" },
				{ kP10GrainSplit, "region evidence (derived maps)" },
				{ kP10cGrainSplitChroma, "chroma from luma" },
				{ kP11RegionBlockStats, "region evidence (derived maps)" },
				{ kP12LightLevel, "region evidence (derived maps)" },
				{ kP13SourceRing, "temporal detail" },
				{ kTdAnalysis, "temporal detail" },
				{ kTdRefstore, "temporal detail" },
				{ kTdAdmission, "temporal detail" },
				// Doc 72: p7g is read by the camera guidance row (motion 24); the engine products are read at lag 0 by the engine rows (family
				// "temporal detail", repair 13-14) which come before every Scaling / Output / deband reader, so the engine is the live-fill owner.
				{ kP7gCameraGuide, "camera guidance" },
				{ kCodingEvidence, "temporal detail" },
				{ kQualityPattern, "temporal detail" },
				{ kRefinedField, "temporal detail" },
				{ kTdAnchorRefs, "temporal detail" },
				{ kTdDelta, "temporal detail" },
				{ kTdMaps, "temporal detail" },
				{ kTdTrailSummary, "temporal detail" },
			};
			const Registry reg = SkeletonProducts();
			Assert::AreEqual(size_t(kSkeletonProductEnd) - 3u, sizeof(expected) / sizeof(expected[0]),
				L"every product but the matcher pair, the held-state source identity and the causal quality pattern has an expectation");
			// The causal quality pattern is a substitute (read as P15 by its declared id): nobody reads it by its own id.
			Assert::IsNull(LiveFillOwner(kQualityPatternCausal));
			for (const Expect& e : expected) {
				const ConsumerRow* owner = LiveFillOwner(e.id);
				Assert::IsNotNull(owner, L"an owner exists");
				Assert::AreEqual(std::string(e.family), std::string(owner->family));
			}
			// The matcher pair is read as P2 (declared id): nobody reads it by its own id, and it is lane-produced.
			Assert::IsNull(LiveFillOwner(kP2mMatcherPair));
			// P14 source identity is HELD STATE (doc 61 D-SID1): its consumers read the latest sealed record (lag 1), so no
			// consumer can fill it live; it still has readers.
			Assert::IsNull(LiveFillOwner(kSourceIdentity), L"no live fill for the held-state source identity");
			Assert::IsFalse(ConsumersOf(kSourceIdentity).empty());
			for (const ConsumerRow* c : ConsumersOf(kSourceIdentity))
				for (size_t k = 0; k < c->decl.count; ++k)
					if (c->decl.consumes[k].product == kSourceIdentity)
						Assert::IsTrue(c->decl.consumes[k].lag >= 1, L"a held-state read is lag >= 1");
			size_t unread = 0;
			for (const ProductDecl& d : reg.All())
				if (ConsumersOf(d.id).empty() && d.backend != Backend::RenderOnly)
					++unread;
			Assert::AreEqual(size_t(2), unread, L"the matcher pair and the causal quality pattern (substitutes read by the declared id)");

			// Over the ACTIVE consumers: with the temporal-detail camera row off, P7's owner is the next lag-0 reader.
			std::vector<uint16_t> active;
			for (const ConsumerRow& c : ConsumerRows())
				if (c.decl.id != CameraLink::DeclarationOf(CameraLink::CameraConsumer::TemporalDetail).id)
					active.push_back(c.decl.id);
			Assert::AreEqual(std::string("detail pulse"), std::string(LiveFillOwner(kP7CameraFit, &active)->family));
			// Only camera measurement active: it reads P7 at lag 1, so nothing can fill P7 live from it.
			std::vector<uint16_t> only(1, CameraLink::DeclarationOf(CameraLink::CameraConsumer::Measurement).id);
			Assert::IsNull(LiveFillOwner(kP7CameraFit, &only));
			Assert::IsFalse(ConsumersOf(kP7CameraFit, false, &only).empty());
			// A family with several rows is one reader.
			size_t families = 0;
			for (const ConsumerRow* c : ConsumersOf(kP8CameraLabels))
				families += std::string(c->family) == "temporal detail" ? 1u : 0u;
			Assert::IsTrue(families <= 1);
		}

		TEST_METHOD(EngineProductsAreWiredToTheFiveConsumerReadPathsAndTheEngineRows)
		{
			// The consumer rows state exactly the sets TdProducts::NeededBy states (the read paths in the modules use that function),
			// and every engine product has a reader (consumer rows or engine rows).
			auto readSet = [](uint16_t index, std::set<int>& into) {
				const uint16_t id = uint16_t(kCatalogConsumerIdBase + index);
				bool found = false;
				for (const ConsumerRow& c : ConsumerRows())
					if (c.decl.id == id) {
						found = true;
						for (size_t k = 0; k < c.decl.count; ++k)
							into.insert(int(c.decl.consumes[k].product));
					}
				Assert::IsTrue(found, L"consumer row present");
			};
			auto expectSet = [](TdProducts::Consumer who) {
				std::set<int> s2;
				for (ProductId p : TdProducts::NeededBy(who))
					s2.insert(int(p));
				return s2;
			};
			std::set<int> scaler, dither, chroma, sigma, deband, engine;
			readSet(19, scaler);
			readSet(20, dither);
			readSet(21, chroma);
			readSet(22, sigma);
			readSet(23, deband);
			readSet(24, engine);
			readSet(25, engine);
			Assert::IsTrue(scaler == expectSet(TdProducts::Consumer::AdaptiveScaler), L"adaptive scaling read path");
			Assert::IsTrue(dither == expectSet(TdProducts::Consumer::DitherNeed), L"dither need read path");
			Assert::IsTrue(chroma == expectSet(TdProducts::Consumer::ChromaStage), L"chroma read path");
			Assert::IsTrue(sigma == expectSet(TdProducts::Consumer::SigmaDelta), L"sigma-delta read path");
			Assert::IsTrue(deband == expectSet(TdProducts::Consumer::Deband), L"deband read path");
			Assert::IsTrue(engine == expectSet(TdProducts::Consumer::Engine), L"engine rows");
			// Every engine-product id (the three redefined + the appended) is a registry product with a catalog row of the right unit.
			const Registry reg = SkeletonProducts();
			size_t seen = 0;
			for (const TdProducts::Contract& c : TdProducts::Contracts()) {
				++seen;
				Assert::IsTrue(reg.Has(c.id));
				Assert::AreEqual(std::string(c.token), std::string(reg.Get(c.id).token));
				if (c.id == kQualityPatternCausal) continue;   // the substitute is read through the declared id of quality_pattern
				Assert::IsFalse(ConsumersOf(c.id).empty(), L"every engine product has a reader");
			}
			Assert::AreEqual(size_t(TdProducts::kCount), seen);
			// Each row names its consumer's fallback id (its own existing entry); the engine rows use the engine's.
			for (const ConsumerRow& c : ConsumerRows()) {
				const uint16_t index = uint16_t(c.decl.id - kCatalogConsumerIdBase);
				if (c.decl.id < kCatalogConsumerIdBase || index > 25) continue;
				if (index == 19) Assert::AreEqual(std::string(TdProducts::FallbackIdOf(TdProducts::Consumer::AdaptiveScaler)), std::string(c.fallbackId));
				if (index == 20) Assert::AreEqual(std::string(TdProducts::FallbackIdOf(TdProducts::Consumer::DitherNeed)), std::string(c.fallbackId));
				if (index == 21) Assert::AreEqual(std::string(TdProducts::FallbackIdOf(TdProducts::Consumer::ChromaStage)), std::string(c.fallbackId));
				if (index == 22) Assert::AreEqual(std::string(TdProducts::FallbackIdOf(TdProducts::Consumer::SigmaDelta)), std::string(c.fallbackId));
				if (index == 23) Assert::AreEqual(std::string(TdProducts::FallbackIdOf(TdProducts::Consumer::Deband)), std::string(c.fallbackId));
				if (index >= 24 && index <= 25) {
					Assert::AreEqual(std::string(TdProducts::FallbackIdOf(TdProducts::Consumer::Engine)), std::string(c.fallbackId));
					Assert::AreEqual(std::string("temporal detail"), std::string(c.family));
				}
			}
			// The registered p7g and its consumer row (id 20 of the registry: registered, not claimed).
			Assert::AreEqual(int(kSourceIdentity) + 1, int(kP7gCameraGuide));
			Assert::AreEqual(size_t(1), ConsumersOf(kP7gCameraGuide).size());
		}

		TEST_METHOD(EveryRegistryProductHasACatalogRow)
		{
			const Registry reg = SkeletonProducts();
			Assert::AreEqual(reg.Size(), ProductRows().size());
			for (const ProductDecl& d : reg.All()) {
				const ProductRow* row = RowOf(d.id);
				Assert::IsNotNull(row);
				Assert::IsTrue(row->unit != nullptr && row->unit[0] != '\0');
			}
			// No consumer row drops a read: every declared read count fits (MakeRow asserts it) and is non-empty.
			for (const ConsumerRow& c : ConsumerRows())
				Assert::IsTrue(c.decl.count > 0 && c.decl.count <= kMaxConsumes && !c.overflow && !c.decl.overflow);
			Assert::AreEqual(std::string(), ValidateConsumerRows(), L"Release-visible: no catalog row dropped a read");
#ifdef NDEBUG
			{   // Release has no assert: a row naming one read too many is flagged, not silently truncated.
				std::vector<ProductId> many(kMaxConsumes + 1, kP4SourceStats);
				ConsumerRow row = MakeRow(0x7f, "overflow probe", "test", PathwayUnit::Input, 1, {}, FallbackKind::OwnReduced, "product.source_id", false, {});
				for (ProductId p : many) AddConsume(row.decl, p);
				Assert::IsTrue(row.decl.overflow, L"AddConsume flags the extra read");
				Assert::AreEqual(size_t(kMaxConsumes), size_t(row.decl.count));
				const size_t n = kMaxConsumes + 1;
				ConsumerRow big = MakeRow(0x7e, "overflow probe 2", "test", PathwayUnit::Input, 2,
					{ kP4SourceStats, kP4SourceStats, kP4SourceStats, kP4SourceStats, kP4SourceStats, kP4SourceStats, kP4SourceStats, kP4SourceStats, kP4SourceStats, kP4SourceStats, kP4SourceStats },
					FallbackKind::OwnReduced, "product.source_id");
				Assert::IsTrue(n == 11 && big.overflow && big.decl.overflow, L"MakeRow flags the row (and the decl) in Release");
			}
#endif
		}

		TEST_METHOD(UnitVersionsMatchTheUnitsAndVerifyUnitEngagesAFallback)
		{
			struct Pair { ProductId id; uint32_t unit; };
			const Pair pairs[] = {
				{ kP0RebaseDecision, VpRebase::kUnitVersion }, { kP6EntranceAhead, VpRebase::kUnitVersion },
				{ kP1HalfLuma, VpLaneProducts::kHalfLumaVersion }, { kP5MonitorTiles, SourceResolution::kUnitVersion },
				{ kP2VectorPair, VectorDemand::kUnitVersion }, { kP3AnalysisField, VectorDemand::kUnitVersion },
				{ kP4SourceStats, VpLaneProducts::kStatsVersion },
				{ kP7CameraFit, CameraLink::kCameraUnitVersion }, { kP8CameraLabels, CameraLink::kCameraUnitVersion },
				{ kP9CameraSmoothed, CameraLink::kCameraUnitVersion },
				{ kP11RegionBlockStats, RegionEvidence::kUnitVersion },
				{ kP12LightLevel, LightLevel::kUnitVersion },
				{ kTdAnalysis, TdProducts::kUnitVersion }, { kTdRefstore, TdProducts::kUnitVersion },
				{ kTdAdmission, TdProducts::kUnitVersion },
				{ kSourceIdentity, SourceIdentity::kUnitVersion },
				{ kP10GrainSplit, GrainSplit::kUnitVersion }, { kP10cGrainSplitChroma, GrainSplit::kUnitVersion },
			};
			for (const Pair& p : pairs) {
				const ProductRow* row = RowOf(p.id);
				Assert::IsNotNull(row);
				Assert::AreEqual(p.unit, row->unitVersion, L"catalog expectation equals the unit constant");
				FallbackCatalog::Decision d;
				Assert::IsTrue(VerifyUnit(p.id, p.unit, &d));
				Assert::IsFalse(d.engaged);
			}
			// The temporal-detail engine products: each contract's unit version is the catalog's expectation.
			Assert::AreEqual(2u, kExpectedTemporalDetailUnit);
			Assert::AreEqual(2u, TdProducts::kUnitVersion);
			Assert::AreEqual(1u, TemporalDetail::kUnitVersion, L"the v1 module code (415 MB class store) stays v1 until the v2 engine replaces it");
			Assert::AreEqual(TdProducts::kCodingEvidenceUnit, kExpectedCodingEvidenceUnit);
			Assert::AreEqual(TdProducts::kQualityPatternUnit, kExpectedQualityPatternUnit);
			Assert::AreEqual(TdProducts::kFieldRefineUnit, kExpectedFieldRefineUnit);
			Assert::AreEqual(TdProducts::kMapsViewUnit, kExpectedTdMapsViewUnit);
			for (const TdProducts::Contract& c : TdProducts::Contracts()) {
				Assert::AreEqual(c.unitVersion, RowOf(c.id)->unitVersion, L"catalog expectation equals the unit constant");
				FallbackCatalog::Decision d;
				Assert::IsTrue(VerifyUnit(c.id, c.unitVersion, &d));
				Assert::IsFalse(d.engaged);
				Assert::IsFalse(VerifyUnit(c.id, c.unitVersion + 1, &d), L"a reader of another version engages the fallback");
				Assert::IsTrue(d.engaged);
			}
			// P10/P10c: the grain split unit's own constant (SA2, GrainSplit::kUnitVersion = 2), checked above.
			Assert::AreEqual(2u, RowOf(kP10GrainSplit)->unitVersion);
			Assert::AreEqual(2u, RowOf(kP10cGrainSplitChroma)->unitVersion);

			FallbackCatalog::Decision bad;
			Assert::IsFalse(VerifyUnit(kP7CameraFit, CameraLink::kCameraUnitVersion + 1, &bad));
			Assert::IsTrue(bad.engaged);
			Assert::AreEqual(std::string("product.unit_version"), bad.id);
			Assert::IsTrue(bad.reason.find("expects") != std::string::npos);
			FallbackCatalog::Decision unknown;
			Assert::IsFalse(VerifyUnit(ProductId(0x7000), 1, &unknown));
			Assert::IsTrue(unknown.engaged);
			// A reader that declares version 0 against a versioned unit is a mismatch; an unversioned unit (the source ring)
			// never mismatches.
			FallbackCatalog::Decision zero;
			Assert::IsFalse(VerifyUnit(kP5MonitorTiles, 0, &zero));
			Assert::IsTrue(zero.engaged);
			Assert::IsTrue(VerifyUnit(kP13SourceRing, 7, nullptr));
		}

		TEST_METHOD(DeclaredSizesAreTheCorrectedOnes)
		{
			const Registry reg = SkeletonProducts();
			const Dims d4k = { 3840, 2160 };
			// P5: the real monitor tile buffer (16 tiles of 256 x 256 f32), independent of the frame size.
			Assert::AreEqual(uint64_t(4194304), BytesOf(reg.Get(kP5MonitorTiles).size, d4k));
			Assert::AreEqual(uint64_t(4194304), BytesOf(reg.Get(kP5MonitorTiles).size, Dims{ 1920, 1080 }));
			Assert::AreEqual(8388608u, kMonitorRingBytes);
			// P1: half R16 + quarter R16 (the engine-size u8 view joins with SA4).
			Assert::AreEqual(uint64_t(4147200 + 1036800), BytesOf(reg.Get(kP1HalfLuma).size, d4k));
			// P4: 8 KB today (the producer layout) of system RAM.
			Assert::AreEqual(uint64_t(8192), BytesOf(reg.Get(kP4SourceStats).size, d4k));
			Assert::IsTrue(reg.Get(kP4SourceStats).residency == Residency::SystemRam);
			// td.memory (id 17, doc 72): the budget-governed floor, the WHOLE store: {1/4 axis: 55 B, 1/2 axis: 1 B, full: 2 B}; no history.
			Assert::AreEqual(uint64_t(960ull * 540 * 55 + 1920ull * 1080 * 1 + 3840ull * 2160 * 2), BytesOf(reg.Get(kTdMemory).size, d4k));
			Assert::AreEqual(0, int(reg.Get(kTdMemory).history));
			Assert::IsTrue(reg.Get(kTdMemory).singletonStore, L"td.memory is the one persistent store (read at lag 1, never governed as history)");
			Assert::AreEqual(std::string("td.memory"), std::string(reg.Get(kTdMemory).token));
			// P13: views only.
			Assert::AreEqual(uint64_t(0), BytesOf(reg.Get(kP13SourceRing).size, d4k));
			Assert::IsTrue(reg.Get(kP13SourceRing).backend == Backend::RenderOnly);
			// P10 reads P13; P11 reads P13 and P10; P12 reads P11.
			auto reads = [&](ProductId id, ProductId from) {
				for (size_t k = 0; k < reg.Get(id).inputCount; ++k)
					if (reg.Get(id).inputs[k].product == from)
						return true;
				return false;
			};
			Assert::IsTrue(reads(kP10GrainSplit, kP13SourceRing) && reads(kP11RegionBlockStats, kP13SourceRing));
			Assert::IsTrue(reads(kP11RegionBlockStats, kP10GrainSplit) && reads(kP12LightLevel, kP11RegionBlockStats));
			// The grain split's full form is recorded as a separate figure.
			Assert::AreEqual(32u, kGrainSplitFullBytesPerPixel);
		}

		TEST_METHOD(VramPerTierGrowsWithTheFrameAndSumsToTheGraphTotals)
		{
			const Dims dims[3] = { { 1920, 1080 }, { 3840, 2160 }, { 7680, 4320 } };
			uint64_t previous = 0;
			for (const Dims& d : dims) {
				const Graph g = FullGraph(Settings(d, 3));
				Assert::IsTrue(g.valid);
				const TierBytes t = VramByTier(g);
				Assert::AreEqual(g.totalRingBytes + g.totalLiveBytes, t.Total());
				Assert::IsTrue(t.Total() > previous, L"bigger frame, more bytes");
				previous = t.Total();
				Assert::IsTrue(t.Total(0) > 0 && t.Total(1) > 0 && t.Total(2) > 0, L"every tier holds something");
			}
			// Deterministic.
			Assert::AreEqual(VramByTier(FullGraph(Settings(dims[1], 3))).Total(), VramByTier(FullGraph(Settings(dims[1], 3))).Total());
		}

		TEST_METHOD(EveryProductRingHoldsItsHistoryAtAnyMeasuredRateAndTheCapIsVisible)
		{
			// Unmeasured (the field maximum), 24p, 60p and 240 fps; a deep render-ahead and several lane jobs stress the extra slot terms.
			const double periods[] = { 0.0, 1001.0 / 24.0, 1001.0 / 60.0, 1000.0 / 120.0, 1000.0 / 240.0 };
			for (double period : periods) {
				const Registry reg = SkeletonProducts(false, false, period);
				Assert::IsTrue(reg.Validate());
				for (uint8_t lane : { uint8_t(0), uint8_t(1), uint8_t(3) }) {
					GraphSettings gs = Settings(Dims{ 3840, 2160 }, lane);
					gs.renderAhead = 8;
					gs.laneJobsInFlight = 4;
					const Graph g = FullGraph(gs, reg);
					Assert::IsTrue(g.valid, L"the catalog graph never overflows the slot counts at any rate");
					for (const Node& n : g.nodes) {
						Assert::IsTrue(int(n.ring) >= int(n.history) + 1, L"ring >= history + 1");
						Assert::IsTrue(int(n.livePool) >= int(n.history) + 1, L"live pool >= history + 1");
					}
					const Node* qp = g.Find(kQualityPattern);
					Assert::IsTrue(qp != nullptr);
					Assert::AreEqual(int(QualityPatternHistoryFrames(period)), int(qp->history), L"the node carries the derived depth, no silent change");
				}
			}
			// The cap: 223 frames of the 2 s window = 111.5 fps. Visible above it, silent when nothing is capped.
			Assert::AreEqual(223, int(kRecordHistoryMax));
			Assert::IsTrue(RecordHistoryCapNote(0.0).empty());
			Assert::IsTrue(RecordHistoryCapNote(1001.0 / 24.0).empty());
			Assert::IsTrue(RecordHistoryCapNote(1000.0 / 100.0).empty());
			const std::string note = RecordHistoryCapNote(1000.0 / 240.0);
			Assert::IsTrue(note.find("quality pattern history capped at 223 frames (source rate above 111.5 fps)") != std::string::npos, L"the cap is named, never silent");
			Assert::AreEqual(223, int(QualityPatternHistoryFrames(1000.0 / 240.0)));
			Assert::IsFalse(FallbackCatalog::SkeletonHistoryCapped(note).reason.empty());
			Assert::IsFalse(FallbackCatalog::SkeletonHistoryCapped("").engaged);
		}

		TEST_METHOD(GeneratedDocRegionMatchesTheCatalog)
		{
			const std::string path = SettingsTestSupport::RepoRoot() + "docs\\design\\product-graph.md";
			const std::string rendered = Normalise(RenderDoc());
			std::string text = Normalise(SettingsTestSupport::ReadFile(path));
			size_t envLen = 0;
			getenv_s(&envLen, nullptr, 0, "VP_WRITE_PRODUCT_GRAPH_DOC");
			const bool write = envLen > 0;
			const size_t b = text.find(kDocBegin);
			const size_t e = text.find(kDocEnd);
			if (write) {
				if (b != std::string::npos && e != std::string::npos && e > b)
					text = text.substr(0, b) + rendered + text.substr(e + std::string(kDocEnd).size());
				else
					text += (text.empty() ? "" : "\n") + rendered + "\n";
				std::ofstream out(path, std::ios::binary);
				out << text;
				return;
			}
			Assert::IsTrue(b != std::string::npos && e != std::string::npos && e > b,
				L"docs/design/product-graph.md has the generated region markers");
			const std::string region = text.substr(b, e + std::string(kDocEnd).size() - b);
			Assert::AreEqual(rendered, region, L"regenerate with VP_WRITE_PRODUCT_GRAPH_DOC=1");
		}

		TEST_METHOD(NoModuleHeaderDefinesItsOwnProductDescriptor)
		{
			namespace fs = std::filesystem;
			const std::string root = SettingsTestSupport::RepoRoot() + "src\\VP-V\\modules";
			Assert::IsTrue(fs::exists(root));
			const std::regex def("struct\\s+(ProductDecl|ProductInfo)\\b|\\bkProductDecl\\b|\\bkProduct\\b");
			// SA2 (skeleton adoption, GrainNode) retires these two when it merges: the grain split's and the chroma
			// upscale's descriptors read the grain split and are owned by that package.
			const char* const allowed[] = { "\\repair\\grain_split\\grainsplit.h", "\\scaling\\chroma_upscale\\chromacfl.h" };
			size_t scanned = 0;
			std::string offenders;
			for (const fs::directory_entry& e : fs::recursive_directory_iterator(root)) {
				if (!e.is_regular_file())
					continue;
				const std::string ext = Lower(e.path().extension().string());
				if (ext != ".h" && ext != ".cpp")
					continue;
				const std::string path = e.path().string();
				const std::string low = Lower(path);
				if (low.find("\\tests\\") != std::string::npos)
					continue;
				bool skip = false;
				for (const char* a : allowed)
					skip = skip || low.size() >= std::strlen(a) && low.compare(low.size() - std::strlen(a), std::strlen(a), a) == 0;
				++scanned;
				if (skip)
					continue;
				std::istringstream lines(SettingsTestSupport::ReadFile(path));
				std::string line;
				int no = 0;
				while (std::getline(lines, line)) {
					++no;
					const size_t first = line.find_first_not_of(" \t");
					if (first == std::string::npos || line.compare(first, 2, "//") == 0 || line[first] == '*')
						continue;
					if (std::regex_search(line, def))
						offenders += path + ":" + std::to_string(no) + "\n";
				}
			}
			Assert::IsTrue(scanned > 50);
			Assert::AreEqual(std::string(), offenders);
		}
	};
}
