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
#include <VP-V/modules/input/source_analysis/identity/SourceGeometryProduct.h>
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
			Assert::AreEqual(size_t(kSkeletonProductEnd) - 4u, sizeof(expected) / sizeof(expected[0]),
				L"every product but the matcher pair, the two held-state records (source identity, source geometry) and the causal quality pattern has an expectation");
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
			// P14g source_geometry is HELD STATE too (wave S2, doc 78 section 6): readers at lag >= 1 only, so no live fill; it has readers.
			Assert::IsNull(LiveFillOwner(kSourceGeometry), L"no live fill for the held-state source geometry");
			Assert::IsFalse(ConsumersOf(kSourceGeometry).empty());
			for (const ConsumerRow& c : ConsumerRows())
				for (size_t k = 0; k < c.decl.count; ++k)
					if (c.decl.consumes[k].product == kSourceGeometry)
						Assert::IsTrue(c.decl.consumes[k].lag >= 1, L"a held-state read is lag >= 1");
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

		TEST_METHOD(HeldStateProductsAreReadAtLagOneOrMoreOnlyAndHaveNoLivePool)
		{
			// P14 source identity and P14g source geometry are the held-state products: the writer outside the frame loop publishes, every reader
			// takes the latest sealed record. Enforced: no lag-0 / look-ahead read (Registry::Validate, BuildGraph), no live pool.
			const Registry reg = SkeletonProducts();
			size_t held = 0;
			for (const ProductDecl& d : reg.All())
				if (d.heldState) {
					++held;
					Assert::IsTrue(d.id == kSourceIdentity || d.id == kSourceGeometry, L"only the two records");
					// Every catalog reader reads it at lag >= 1 (a lag-0 reader would have been refused by the graph).
					for (const ConsumerRow& c : ConsumerRows())
						for (size_t k = 0; k < c.decl.count; ++k)
							if (c.decl.consumes[k].product == d.id)
								Assert::IsTrue(c.decl.consumes[k].lag >= 1, L"a held-state read is lag >= 1");
					// No product declares a lag-0 / lead input on it either.
					for (const ProductDecl& o : reg.All())
						for (size_t k = 0; k < o.inputCount; ++k)
							if (o.inputs[k].product == d.id)
								Assert::IsTrue(o.inputs[k].lag >= 1 && o.inputs[k].lead == 0);
				}
			Assert::AreEqual(size_t(2), held);
			Assert::IsTrue(reg.Get(kSourceIdentity).heldState && reg.Get(kSourceGeometry).heldState);
			for (uint8_t lane : { uint8_t(0), uint8_t(1), uint8_t(3) }) {
				const Graph g = FullGraph(Settings(Dims{ 3840, 2160 }, lane), reg);
				Assert::IsTrue(g.valid, L"the full catalog graph still builds with the rule enforced");
				for (ProductId id : { kSourceIdentity, kSourceGeometry }) {
					const Node* n = g.Find(id);
					Assert::IsNotNull(n);
					Assert::IsTrue(n->heldState);
					Assert::AreEqual(0, int(n->livePool), L"no live pool");
					Assert::AreEqual(uint64_t(0), n->liveBytes);
					Assert::IsTrue(n->ring >= n->history + 1, L"the ring keeps its history");
				}
			}
			// A consumer at lag 0 on either record is refused, visibly.
			for (ProductId id : { kSourceIdentity, kSourceGeometry }) {
				ConsumerDecl c;
				c.id = 1;
				c.name = "lag-zero reader";
				AddConsume(c, id, 0);
				const Graph bad = BuildGraph(reg, std::vector<ConsumerDecl>(1, c), Settings(Dims{ 1920, 1080 }, 1));
				Assert::IsFalse(bad.valid);
				Assert::IsTrue(std::string(bad.error).find("held-state") != std::string::npos);
				Assert::AreEqual(int(id), int(bad.errorProduct));
			}
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
				{ kSourceGeometry, SourceGeometry::kUnitVersion },
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

		TEST_METHOD(TdMemoryIsATwoBufferPingPongInTheVramTable)
		{
			const Registry reg = SkeletonProducts();
			const Dims d4k = { 3840, 2160 };
			const uint64_t store = BytesOf(reg.Get(kTdMemory).size, d4k);
			for (uint8_t lane : { uint8_t(1), uint8_t(3), uint8_t(6) })
				for (uint8_t renderAhead : { uint8_t(0), uint8_t(4) }) {
					GraphSettings gs = Settings(d4k, lane);
					gs.renderAhead = renderAhead;
					const Graph g = FullGraph(gs, reg);
					Assert::IsTrue(g.valid);
					const Node* n = g.Find(kTdMemory);
					Assert::IsNotNull(n);
					Assert::IsTrue(n->singletonStore);
					Assert::AreEqual(2, int(n->ring), L"ring = 2 whatever R and L are");
					Assert::AreEqual(0, int(n->livePool), L"no live pool");
					Assert::AreEqual(store, n->bufferBytes);
					Assert::AreEqual(2u * store, n->ringBytes, L"the VRAM table figure is 2x the store, not a ring of history + R + L slots");
					Assert::AreEqual(uint64_t(0), n->liveBytes);
					Assert::AreEqual(g.totalRingBytes + g.totalLiveBytes, VramByTier(g).Total(), L"the table sums the node figures");
				}
		}

		TEST_METHOD(EveryProductRingHoldsItsHistoryAtAnyMeasuredRateAndTheCapIsVisible)
		{
			// Unmeasured (the field maximum), 24p, 60p and 240 fps; a deep render-ahead and several lane jobs stress the extra slot terms.
			const double periods[] = { 0.0, 1001.0 / 24.0, 1001.0 / 60.0, 1000.0 / 120.0, 1000.0 / 240.0 };
			for (double period : periods) {
				const Registry reg = SkeletonProducts(false, false, period);
				Assert::IsTrue(reg.Validate());
				for (uint8_t lane : { uint8_t(0), uint8_t(1), uint8_t(2), uint8_t(3) }) {
					GraphSettings gs = Settings(Dims{ 3840, 2160 }, lane);
					gs.renderAhead = 8;
					gs.laneJobsInFlight = 4;
					const Graph g = FullGraph(gs, reg);
					Assert::IsTrue(g.valid, L"the catalog graph never overflows the slot counts at any rate");
					for (const Node& n : g.nodes) {
						Assert::IsTrue(int(n.ring) >= int(n.history) + 1, L"ring >= history + 1");
						if (n.singletonStore)   // the ping-pong store: two buffers, lane-only (no live fill)
							Assert::IsTrue(int(n.ring) == 2 && n.livePool == 0, L"a single store is two buffers and has no live pool");
						else if (n.heldState)   // held state (P14, P14g): lane-only, read at lag >= 1; no live fill, so no live pool
							Assert::IsTrue(n.livePool == 0, L"a held-state product has no live pool");
						else
							Assert::IsTrue(int(n.livePool) >= int(n.history) + 1, L"live pool >= history + 1");
					}
					// The look-ahead form (quality_pattern) stays in the graph while the queue supplies any lead: effective lead = min(declared 2,
					// lane lead - 1), so lane 2 gives effective lead 1 (still quality_pattern, a shorter look-ahead) and lane >= 3 gives 2. Only a
					// lane lead of 0 or 1 gives no lead and resolves it to the causal substitute (a visible reduced mode). Both carry the same derived history depth.
					const Node* qp = g.Find(kQualityPattern);
					const Node* qc = g.Find(kQualityPatternCausal);
					Assert::IsTrue((qp != nullptr) == (lane >= 2), L"quality_pattern is in the graph exactly when the queue supplies some lead (lane lead >= 2)");
					Assert::IsTrue(lane >= 2 || qc != nullptr, L"otherwise the causal substitute stands in");
					if (qp) {
						Assert::AreEqual(int(lane == 2 ? 1 : 2), int(qp->lead), L"effective lead = min(declared 2, lane lead - 1): lane 2 gives 1, lane 3 gives 2");
						Assert::AreEqual(2, int(qp->declaredLead));
						Assert::IsFalse(qp->shortQueue, L"a shortened look-ahead is not the causal substitute");
					}
					const Node* record = qp ? qp : qc;
					Assert::IsNotNull(record);
					Assert::AreEqual(int(QualityPatternHistoryFrames(period)), int(record->history), L"the node carries the derived depth, no silent change");
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

		TEST_METHOD(RecordHistoriesAreDerivedFromTheConfiguredQueue)
		{
			// history_seconds = max(floor, queue multiple x (R + L + look-ahead) x period); frames = ceil(seconds / period), capped at kRecordHistoryMax.
			const double p24 = 1001.0 / 24.0;
			const double p60 = 1001.0 / 60.0;
			Assert::AreEqual(unsigned(100), QueueFramesFor(8, 4, 88));
			// The floors hold on a short queue. At 24 p (41.708 ms) the floors are 2 s = ceil(47.95) = 48 frames of quality pattern and
			// 0.25 s = ceil(5.994) = 6 frames of trail summary. A queue of 3 frames (0.125 s) is under both floors; a queue of 7 frames
			// (7 x 41.708 = 291.96 ms) is far under the 2 s floor but ABOVE the 0.25 s floor, so the trail summary follows it: 7 frames.
			const unsigned shortQ = QueueFramesFor(2, 3, 2);
			const unsigned tinyQ = QueueFramesFor(0, 1, 2);
			Assert::AreEqual(unsigned(7), shortQ);
			Assert::AreEqual(48, int(QualityPatternHistoryFrames(p24, shortQ)));
			Assert::AreEqual(int(QualityPatternHistoryFrames(p24)), int(QualityPatternHistoryFrames(p24, shortQ)));
			Assert::AreEqual(6, int(TdTrailSummaryHistoryFrames(p24, tinyQ)));
			Assert::AreEqual(int(TdTrailSummaryHistoryFrames(p24)), int(TdTrailSummaryHistoryFrames(p24, tinyQ)));
			Assert::AreEqual(7, int(TdTrailSummaryHistoryFrames(p24, shortQ)), L"7 queue frames exceed the 0.25 s floor (6 frames): the queue sets the depth");
			// A long queue gives a longer history: 100 frames = 4.17 s at 24 p, above both floors. The same 100 frames are only 1.67 s at 60 p,
			// below the 2 s floor, so the quality pattern keeps its floor there (120 frames) while the trail summary follows the queue.
			Assert::AreEqual(100, int(QualityPatternHistoryFrames(p24, 100)));
			Assert::AreEqual(100, int(TdTrailSummaryHistoryFrames(p24, 100)));
			Assert::AreEqual(int(QualityPatternHistoryFrames(p60)), int(QualityPatternHistoryFrames(p60, 100)));
			Assert::AreEqual(100, int(TdTrailSummaryHistoryFrames(p60, 100)));
			// The registry carries the derived depths (the quality pattern, its causal substitute and the trail summary).
			const Registry reg100 = SkeletonProducts(false, false, p24, 100);
			Assert::IsTrue(reg100.Validate());
			Assert::AreEqual(100, int(reg100.Get(kQualityPattern).history));
			Assert::AreEqual(100, int(reg100.Get(kQualityPatternCausal).history));
			Assert::AreEqual(100, int(reg100.Get(kTdTrailSummary).history));
			Assert::AreEqual(48, int(SkeletonProducts(false, false, p24, 0).Get(kQualityPattern).history));
			// Unmeasured period: today's behaviour (the field maximum), whatever the queue.
			Assert::AreEqual(int(kRecordHistoryMax), int(QualityPatternHistoryFrames(0.0, 300)));
			Assert::IsTrue(RecordHistoryCapNote(0.0, true, true, 300).empty());
			// The cap and its visible note: exactly kRecordHistoryMax frames of queue is not capped, one more is, and the note names the queue.
			Assert::IsTrue(RecordHistoryCapNote(p24, true, true, kRecordHistoryMax).empty());
			const std::string over = RecordHistoryCapNote(p24, true, true, unsigned(kRecordHistoryMax) + 1u);
			Assert::IsTrue(over.find("quality pattern history capped at 223 frames (the configured queue asks for 224 frames)") != std::string::npos, L"the queue cap is named");
			Assert::IsTrue(over.find("td trail summary history capped at 223 frames (the configured queue asks for 224 frames)") != std::string::npos);
			Assert::AreEqual(int(kRecordHistoryMax), int(QualityPatternHistoryFrames(p24, 300)));
			// A rate-driven cap keeps its own wording, whatever a short queue adds.
			Assert::IsTrue(RecordHistoryCapNote(1000.0 / 240.0, true, true, 7).find("quality pattern history capped at 223 frames (source rate above 111.5 fps)") != std::string::npos);
			// Through a built graph: the note reads the graph's own queue (R + L + look-ahead walk), and the capped node holds the cap.
			GraphSettings gs = Settings(Dims{ 3840, 2160 }, 1);
			gs.renderAhead = 0;
			gs.lookAheadWalk = 300;
			const Registry regCap = SkeletonProducts(false, false, p24, QueueFrames(gs));
			const Graph g = FullGraph(gs, regCap);
			Assert::IsTrue(g.valid, L"a queue-driven cap still fits the slot counts at a lane lead of 1");
			const Node* causal = g.Find(kQualityPatternCausal);
			Assert::IsNotNull(causal);
			Assert::AreEqual(int(kRecordHistoryMax), int(causal->history));
			Assert::IsTrue(GraphHistoryCapNote(g, p24).find("(the configured queue asks for 301 frames)") != std::string::npos);
		}

		// C17 (doc 93 W0-4, the seed of the conformance checker): every declared input is either READ as declared or carries an explicit flag.
		// Declarations stay as they are (graph order, live-fill owner unchanged); the flags say where the code differs today.
		TEST_METHOD(EveryDeclaredInputIsReadOrCarriesAFlag)
		{
			const Registry reg = SkeletonProducts();

			// 1. Product-level flags: each names a real product; NotYetConsumed / ViewIsActual name an input the product DECLARES (a flag on
			// a declaration that is gone is stale); ExternalView names an input it does NOT declare (else it is just a declaration).
			size_t notYet = 0, views = 0, ext = 0, reserved = 0;
			for (const ProductFlag& f : ProductFlags()) {
				Assert::IsTrue(reg.Has(f.product), L"a product flag names a registry product");
				Assert::IsTrue(f.item != nullptr && f.item[0] == 'C', L"a flag names the consolidation item that makes it true (C<n>)");
				Assert::IsTrue(f.note != nullptr && f.note[0] != '\0');
				bool declared = false;
				if (f.input != kNoProduct) {
					Assert::IsTrue(reg.Has(f.input));
					const ProductDecl& d = reg.Get(f.product);
					for (size_t i = 0; i < d.inputCount; ++i)
						declared = declared || d.inputs[i].product == f.input;
				}
				switch (f.kind) {
				case FlagKind::NotYetConsumed:
					++notYet;
					Assert::IsTrue(declared, L"declared, not yet consumed: the input is declared");
					Assert::AreEqual(std::string("declared, not yet consumed"), std::string(Token(f.kind)));
					break;
				case FlagKind::ViewIsActual:
					++views;
					Assert::IsTrue(declared, L"view flag: the declared input exists");
					break;
				case FlagKind::ExternalView:
					++ext;
					Assert::IsFalse(declared, L"external view: the input is NOT a declaration");
					break;
				case FlagKind::Reserved:
					++reserved;
					Assert::IsTrue(f.input == kNoProduct, L"reserved applies to the whole product");
					break;
				case FlagKind::NotRead:
					Assert::Fail(L"NotRead is a consumer-row flag");
					break;
				case FlagKind::DeclaredNotAllocated:
					Assert::Fail(L"DeclaredNotAllocated is a reserve-row flag (core/ReserveRows.h), not a product flag");
					break;
				}
			}
			// The exact set (a new unread declaration must add its flag here AND in ProductFlags()).
			Assert::AreEqual(size_t(3), notYet, L"P2, P2m, P3 declare P1 and read private copies (C9)");
			Assert::AreEqual(size_t(4), views, L"P10 -> tap view, P14g -> source-frame tap view, P15 / P15c -> FrameQuality view of the P5 tiles (WAIVER-P15DECL-01: 2 + 2)");
			for (ProductId p : { kQualityPattern, kQualityPatternCausal }) {
				bool flagged = false;
				for (const ProductFlag& f : ProductFlags())
					flagged = flagged || (f.product == p && f.input == kP5MonitorTiles && f.kind == FlagKind::ViewIsActual && std::string(f.note).find("identity/FrameQuality.h") != std::string::npos);
				Assert::IsTrue(flagged, L"P15 / P15c carry the ViewIsActual flag that names identity/FrameQuality.h");
			}
			Assert::AreEqual(size_t(1), ext, L"P7 reads P2 via FlowOf");
			Assert::AreEqual(size_t(1), reserved, L"P10c has no producer");

			// 2. The P2 / P2m / P3 declarations of P1 are KEPT (graph order unchanged), flagged rather than deleted.
			for (ProductId p : { kP2VectorPair, kP2mMatcherPair, kP3AnalysisField }) {
				bool flagged = false;
				for (const ProductFlag& f : ProductFlags())
					flagged = flagged || (f.product == p && f.input == kP1HalfLuma && f.kind == FlagKind::NotYetConsumed);
				const ProductDecl& d = reg.Get(p);
				bool declaresP1 = false;
				for (size_t i = 0; i < d.inputCount; ++i)
					declaresP1 = declaresP1 || d.inputs[i].product == kP1HalfLuma;
				Assert::IsTrue(declaresP1, L"the P1 declaration is kept");
				Assert::IsTrue(flagged, L"and carries the not-yet-consumed (C9) flag");
			}

			// 3. P10c: reserved, no producer: its catalog row says so and no consumer row other than chroma-from-luma reads it.
			const ProductRow* c = RowOf(kP10cGrainSplitChroma);
			Assert::IsNotNull(c);
			Assert::IsTrue(std::string(c->laneProducer).find("reserved") == 0, L"P10c is marked reserved / no producer in the product row");

			// 4. Consumer-level flags: each names a catalog consumer row; NotRead / ViewIsActual name a DECLARED read of the row,
			// ExternalView names a read the row does NOT declare (B3-Z Z-DECL, lead 2026-11-11; expectation change = WAIVER-B3Z-01, coordinator approved 2026-11-12).
			// AUDIT-R2 F1 (B3-Z Z2): +4 NotRead (782 P1, 781 P1 / P4, 776 P4), +1 view (776 P5), DP/TR P4 item C7 -> D10
			// (expectation change = WAIVER-B3Z-02 (coordinator approved 2026-11-12)).
			size_t cNotRead = 0, cViews = 0, cExt = 0;
			for (const ConsumerFlag& f : ConsumerFlags()) {
				const ConsumerRow* row = nullptr;
				for (const ConsumerRow& r : ConsumerRows())
					if (r.decl.id == f.consumerId)
						row = &r;
				Assert::IsNotNull(row, L"a consumer flag names a catalog consumer row");
				bool declared = false;
				for (size_t k = 0; k < row->decl.count; ++k)
					declared = declared || row->decl.consumes[k].product == f.input;
				Assert::IsTrue(f.item != nullptr);
				// WAIVER-B3Z-02 (coordinator approved 2026-11-12, condition: exact item set, not a C/D prefix): every consumer flag names one of these register items.
				{
					const std::string item(f.item);
					Assert::IsTrue(item == "C9" || item == "C9b" || item == "C10" || item == "C2-cam" || item == "D6-D8" || item == "D8" || item == "D10",
						L"consumer flag item is in the exact register set {C9, C9b, C10, C2-cam, D6-D8, D8, D10}");
				}
				Assert::IsTrue(f.note != nullptr && f.note[0] != '\0');
				switch (f.kind) {
				case FlagKind::NotRead:
					++cNotRead;
					Assert::IsTrue(declared, L"the flagged read is a declared read of the row");
					break;
				case FlagKind::ViewIsActual:
					++cViews;
					Assert::IsTrue(declared, L"view flag: the declared read exists");
					break;
				case FlagKind::ExternalView:
					++cExt;
					Assert::IsFalse(declared, L"external view: the read is NOT a declaration of the row");
					break;
				default:
					Assert::Fail(L"NotYetConsumed / Reserved are product flags");
				}
			}
			// Detail pulse (id 0) and texture restore (id 1): all three declared reads (P10, P11, P4) are flagged, the held identity read is not.
			size_t dpTr = 0;
			for (const ConsumerFlag& f : ConsumerFlags())
				if ((f.consumerId == kCatalogConsumerIdBase + 0 || f.consumerId == kCatalogConsumerIdBase + 1) && f.kind == FlagKind::NotRead)
					++dpTr;
			Assert::AreEqual(size_t(6), dpTr, L"DP and TR each: P10, P11, P4 not read today");
			// WAIVER-B3Z-02 (coordinator approved 2026-11-12): NotRead = DP / TR 6 + producer P1 + matcher P1, P4 + identification P4 = 10.
			Assert::AreEqual(size_t(10), cNotRead, L"DP / TR's six + the F1 rows (782 P1, 781 P1 / P4, 776 P4)");
			const uint16_t idProducer = uint16_t(kCatalogConsumerIdBase + 14), idMatcher = uint16_t(kCatalogConsumerIdBase + 13),
				idIdentity = uint16_t(kCatalogConsumerIdBase + 8);
			for (const ProductId p : { kP1HalfLuma })
				Assert::IsTrue(ConsumerFlagOf(idProducer, p) && ConsumerFlagOf(idProducer, p)->kind == FlagKind::NotRead, L"782 P1 not read");
			for (const ProductId p : { kP1HalfLuma, kP4SourceStats })
				Assert::IsTrue(ConsumerFlagOf(idMatcher, p) && ConsumerFlagOf(idMatcher, p)->kind == FlagKind::NotRead, L"781 P1 / P4 not read");
			Assert::IsTrue(ConsumerFlagOf(idIdentity, kP4SourceStats) && ConsumerFlagOf(idIdentity, kP4SourceStats)->kind == FlagKind::NotRead);
			Assert::AreEqual(std::string("D6-D8"), std::string(ConsumerFlagOf(idIdentity, kP4SourceStats)->item), L"776 P4: register item D6-D8");
			Assert::AreEqual(std::string("D8"), std::string(ConsumerFlagOf(idMatcher, kP4SourceStats)->item), L"781 P4: register item D8");
			Assert::IsTrue(ConsumerFlagOf(idIdentity, kP5MonitorTiles) && ConsumerFlagOf(idIdentity, kP5MonitorTiles)->kind == FlagKind::ViewIsActual);
			for (const ConsumerFlag& f : ConsumerFlags())
				if (f.input == kP4SourceStats && (f.consumerId == kCatalogConsumerIdBase + 0 || f.consumerId == kCatalogConsumerIdBase + 1))
					Assert::AreEqual(std::string("D10"), std::string(f.item), L"DP / TR P4: the D10 reader switch (C7 landed without it)");
			// The exact P3 set: 4 synthesis consumers + DP, TR, adaptive downscale read the live matcher field at render (external view);
			// the camera fit reads P3 through SeedFieldOf (view); identification reads P5 through the tap (view). 10 + 7 + 2 = 19.
			Assert::AreEqual(size_t(7), cExt, L"synthesis x4 + DP + TR + adaptive downscale: P3 read at render");
			Assert::AreEqual(size_t(2), cViews, L"camera fit: P3 via SeedFieldOf; identification: P5 via the tap");   // WAIVER-B3Z-02 (coordinator approved 2026-11-12)
			Assert::AreEqual(size_t(19), ConsumerFlags().size());   // WAIVER-B3Z-02 (coordinator approved 2026-11-12)
			for (const ConsumerFlag& f : ConsumerFlags())
				if (f.kind != FlagKind::NotRead && !(f.consumerId == idIdentity && f.input == kP5MonitorTiles))
					Assert::IsTrue(f.input == kP3AnalysisField, L"the view / external-view consumer flags are the P3 render-time reads (and 776 P5)");

			// 4b. AUDIT-R2 F1 rule: a read of a product without a producer is never actual, and an unwired row reads nothing.
			const Registry regF1 = SkeletonProducts();
			Assert::IsNotNull(NoProducerReason(kP10cGrainSplitChroma), L"P10c: reserved");
			Assert::IsNotNull(NoProducerReason(kP13SourceRing), L"P13: no producer");
			Assert::IsNotNull(NoProducerReason(kTdMemory), L"td.memory: no producer");
			Assert::IsNull(NoProducerReason(kP1HalfLuma), L"P1 has a producer");
			Assert::IsNull(NoProducerReason(kSourceIdentity), L"held state is produced");
			for (const ConsumerRow& r : ConsumerRows()) {
				const std::string actual = ActualReads(r, regF1);
				const size_t notRead = actual.find("not read: ");
				const std::string readPart = actual.substr(0, notRead == std::string::npos ? actual.size() : notRead);
				for (size_t k = 0; k < r.decl.count; ++k) {
					const ProductId p = r.decl.consumes[k].product;
					if (!NoProducerReason(p) && !UnwiredConsumerOf(r.decl.id))
						continue;
					const std::string token = regF1.Get(p).token;
					bool listedAsRead = false;
					for (size_t at = readPart.find(token); at != std::string::npos; at = readPart.find(token, at + 1)) {
						const size_t end = at + token.size();
						const bool boundL = at == 0 || readPart[at - 1] == ' ';
						const bool boundR = end == readPart.size() || readPart[end] == ',' || readPart[end] == ';' || readPart[end] == '@' || readPart[end] == ' ';
						listedAsRead = listedAsRead || (boundL && boundR);
					}
					Assert::IsFalse(listedAsRead, L"a read of a producer-less product / of an unwired row is never actual");
				}
			}
			size_t unwiredFound = 0;
			for (const UnwiredConsumer& u : UnwiredConsumers())
				for (const ConsumerRow& r : ConsumerRows())
					if (r.decl.id == u.consumerId) {
						++unwiredFound;
						Assert::IsTrue(r.family != nullptr && std::string(r.family).find("temporal detail") != std::string::npos,
							L"the unwired rows are the temporal-detail family");
					}
			Assert::AreEqual(UnwiredConsumers().size(), unwiredFound, L"every unwired id names a catalog row");
			const ConsumerFlag* cam = ConsumerFlagOf(uint16_t(VectorDemand::kConsumerIdBase + int(VectorDemand::Consumer::CameraFit)), kP3AnalysisField);
			Assert::IsNotNull(cam);
			Assert::IsTrue(cam->kind == FlagKind::ViewIsActual && std::string(cam->note).find("SeedFieldOf") != std::string::npos);

			// 5. The flags carry teeth: the NotRead claim is checked against the sources. If DetailPulse / TextureRestore start to reference a
			// skeleton product (any product alias, the product header, the analysis store or the lane producers), the flag is stale and must be
			// removed (C10 / D10 made it true). The other flags (P2 / P2m / P3 -> P1 not yet consumed, P10 -> P13 view, P14g -> P5 view, P7 -> P2
			// external view) stay human-pinned: they are counted and cross-checked against the registry above, but no scan proves them.
			namespace fs = std::filesystem;
			const std::string dir = SettingsTestSupport::RepoRoot() + "src\\VP-V\\modules\\repair\\texture_restore\\";
			const std::regex reads("\\bk(P10|P11|P4)\\w*|\\bGrainSplit::|RegionEvidence|SourceStats\\.h|LaneGpuProducts|LaneCpuProducts|LaneProducts|Products\\.h|FrameAnalysisStore|\\bProductId\\b");
			for (const char* name : { "DetailPulse.h", "DetailPulse.cpp", "TextureRestore.h", "TextureRestore.cpp" }) {
				const std::string path = dir + name;
				Assert::IsTrue(fs::exists(path), L"the DetailPulse / TextureRestore sources exist where the flag says");
				const std::string text = SettingsTestSupport::ReadFile(path);
				Assert::IsFalse(std::regex_search(text, reads), L"stale NotRead flag: the file now reads a skeleton product (remove the flag)");
			}

			// 6. The document prints the flags and the actual reads.
			const std::string doc = RenderDoc();
			Assert::IsTrue(doc.find("reads (actual today)") != std::string::npos);
			Assert::IsTrue(doc.find("declared, not yet consumed") != std::string::npos);
			Assert::IsTrue(doc.find("reserved, no producer") != std::string::npos);
			Assert::IsTrue(doc.find("external view (not declared)") != std::string::npos);
			Assert::IsTrue(doc.find("not read: ") != std::string::npos);
			Assert::IsTrue(doc.find("external view: ") != std::string::npos, L"the consumer table prints the undeclared render-time reads");
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
