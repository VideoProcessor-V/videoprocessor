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
#include <VP-V/modules/input/source_analysis/regions/RegionEvidence.h>
#include <VP-V/modules/repair/temporal_detail/TemporalDetail.h>

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
			Assert::IsTrue(ConsumerRows().size() >= 17 + size_t(VectorDemand::kConsumerCount) + size_t(CameraLink::kCameraConsumerCount));
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

		TEST_METHOD(LiveFillOwnerIsTheFirstReaderInPathwayOrder)
		{
			const Registry reg = SkeletonProducts();
			std::vector<ProductId> noConsumers;
			for (const ProductDecl& d : reg.All()) {
				const std::vector<const ConsumerRow*> cs = ConsumersOf(d.id);
				if (cs.empty()) {
					if (d.backend != Backend::RenderOnly)
						noConsumers.push_back(d.id);
					continue;
				}
				const ConsumerRow* owner = LiveFillOwner(d.id);
				Assert::IsNotNull(owner);
				for (size_t i = 1; i < cs.size(); ++i)
					Assert::IsTrue(PathwayKey(*owner) < PathwayKey(*cs[i]), L"the owner precedes every other reader");
				Assert::IsTrue(owner == cs.front());
			}
			// A lane-produced product nobody reads is flagged: only the matcher pair (read as P2 on cards without an engine).
			Assert::AreEqual(size_t(1), noConsumers.size());
			Assert::AreEqual(int(kP2mMatcherPair), int(noConsumers[0]));
			// A few owners by name (input < motion < repair < scaling ...).
			Assert::AreEqual(std::string("region evidence (derived maps)"), std::string(LiveFillOwner(kP10GrainSplit)->decl.name));
			Assert::AreEqual(std::string("temporal detail"), std::string(LiveFillOwner(kTdAdmission)->decl.name));
			Assert::AreEqual(std::string("source resolution monitor"), std::string(LiveFillOwner(kP5MonitorTiles)->decl.name));
		}

		TEST_METHOD(UnitVersionsMatchTheUnitsAndVerifyUnitEngagesAFallback)
		{
			struct Pair { ProductId id; uint32_t unit; };
			const Pair pairs[] = {
				{ kP2VectorPair, VectorDemand::kUnitVersion }, { kP3AnalysisField, VectorDemand::kUnitVersion },
				{ kP4SourceStats, VpLaneProducts::kStatsVersion },
				{ kP7CameraFit, CameraLink::kCameraUnitVersion }, { kP8CameraLabels, CameraLink::kCameraUnitVersion },
				{ kP9CameraSmoothed, CameraLink::kCameraUnitVersion },
				{ kP11RegionBlockStats, RegionEvidence::kUnitVersion },
				{ kP12LightLevel, LightLevel::kUnitVersion },
				{ kTdAnalysis, TemporalDetail::kUnitVersion }, { kTdRefstore, TemporalDetail::kUnitVersion },
				{ kTdAdmission, TemporalDetail::kUnitVersion },
			};
			for (const Pair& p : pairs) {
				const ProductRow* row = RowOf(p.id);
				Assert::IsNotNull(row);
				Assert::AreEqual(p.unit, row->unitVersion, L"catalog expectation equals the unit constant");
				FallbackCatalog::Decision d;
				Assert::IsTrue(VerifyUnit(p.id, p.unit, &d));
				Assert::IsFalse(d.engaged);
			}
			// P10/P10c: the grain split unit's own constant arrives with SA2 (GrainSplit::kUnitVersion = 2).
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
			// An unversioned unit (0) never mismatches.
			Assert::IsTrue(VerifyUnit(kP5MonitorTiles, 7, nullptr));
		}

		TEST_METHOD(DeclaredSizesAreTheCorrectedOnes)
		{
			const Registry reg = SkeletonProducts();
			const Dims d4k = { 3840, 2160 };
			// P5: the real monitor tile buffer (16 tiles of 256 x 256 f32), independent of the frame size.
			Assert::AreEqual(uint64_t(4194304), BytesOf(reg.Get(kP5MonitorTiles).size, d4k));
			Assert::AreEqual(uint64_t(4194304), BytesOf(reg.Get(kP5MonitorTiles).size, Dims{ 1920, 1080 }));
			Assert::AreEqual(8388608u, kMonitorRingBytes);
			// P1: half R16 + quarter R16 + engine-size u8 half view.
			Assert::AreEqual(uint64_t(4147200 + 1036800 + 2073600), BytesOf(reg.Get(kP1HalfLuma).size, d4k));
			// P4: 16 KB of system RAM.
			Assert::AreEqual(uint64_t(16384), BytesOf(reg.Get(kP4SourceStats).size, d4k));
			Assert::IsTrue(reg.Get(kP4SourceStats).residency == Residency::SystemRam);
			// td.refstore: 4.5 B/px per slot declared (4 B/px + a 0.5 B/px half-size base), history = the slots.
			Assert::AreEqual(uint64_t(3840ull * 2160 * 4 + 1920ull * 1080 * 2), BytesOf(reg.Get(kTdRefstore).size, d4k));
			Assert::AreEqual(int(kRefSlots), int(reg.Get(kTdRefstore).history));
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

		TEST_METHOD(GeneratedDocRegionMatchesTheCatalog)
		{
			const std::string path = SettingsTestSupport::RepoRoot() + "docs\\design\\product-graph.md";
			const std::string rendered = Normalise(RenderDoc());
			std::string text = Normalise(SettingsTestSupport::ReadFile(path));
			const bool write = std::getenv("VP_WRITE_PRODUCT_GRAPH_DOC") != nullptr;
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
