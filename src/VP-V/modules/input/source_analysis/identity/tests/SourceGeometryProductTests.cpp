/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// P14g source_geometry (wave S2, doc 78 section 6): the reservation. Id 29 appended after the doc 72 list, the held-state declaration (Cpu, T0,
// no live fill, lag-1 readers, single writer), the catalog row and readers, the ABSENT placeholder and the absent-tested read of every
// provisional consumer (not requested = byte-identical, requested = classical form visibly), the staleness rule, the byte order. CPU only.
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/core/ProductCatalog.h>
#include <VP-V/modules/input/source_analysis/identity/SourceGeometryProduct.h>

#include <cstdint>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
	namespace SourceGeometryNs
	{
		using namespace SourceGeometry;
		using VpSkeleton::Registry;
		using VpSkeleton::SkeletonProducts;

		const Consumer kAll[] = { Consumer::QualityPattern, Consumer::CodingEvidence, Consumer::TemporalDetail,
		                          Consumer::AdaptiveScaler, Consumer::ChromaStage, Consumer::Status };

		Entry Fresh(uint64_t seq = 10, int64_t timeUs = 1000000, int64_t maxAgeUs = 500000)
		{
			Entry e;
			e.published = true;
			e.unitVersion = kUnitVersion;
			e.seq = seq;
			e.timeUs = timeUs;
			e.maxAgeUs = maxAgeUs;
			return e;
		}

		TEST_CLASS(SourceGeometryProductTests)
		{
		public:
			TEST_METHOD(TheIdIsAppendedAfterTheDoc72ListAndTheRegistryValidates)
			{
				using namespace VpSkeleton;
				const Registry reg = SkeletonProducts();
				const char* why = nullptr;
				Assert::IsTrue(reg.Validate(&why), L"dense ids, inputs known");
				Assert::AreEqual(28, int(kTdTrailSummary), L"the doc 72 list ends at 28");
				Assert::AreEqual(29, int(kSourceGeometry), L"the new id follows it");
				Assert::AreEqual(30, int(kSkeletonProductEnd));
				Assert::AreEqual(size_t(kSkeletonProductEnd), reg.Size());
				Assert::AreEqual(int(kProduct), int(kSourceGeometry));
				Assert::AreEqual(std::string("source_geometry"), std::string(reg.Get(kSourceGeometry).token));
			}

			TEST_METHOD(TheDeclarationIsHeldStateCpuT0AndAtMost512Bytes)
			{
				using namespace VpSkeleton;
				const Registry reg = SkeletonProducts();
				const ProductDecl& p = reg.Get(kSourceGeometry);
				Assert::IsTrue(p.backend == Backend::Cpu, L"Cpu");
				Assert::IsTrue(p.tier == Tier::T0, L"T0");
				Assert::IsTrue(p.residency == Residency::SystemRam);
				Assert::AreEqual(1, int(p.history), L"one record, the latest sealed one (readers at lag 1)");
				Assert::AreEqual(uint64_t(512), BytesOf(p.size, Dims{ 1920, 1080 }));
				Assert::AreEqual(BytesOf(p.size, Dims{ 1920, 1080 }), BytesOf(p.size, Dims{ 7680, 4320 }), L"independent of the frame size");
				Assert::IsTrue(BytesOf(p.size, Dims{ 3840, 2160 }) <= kRecordBytesMax);
				Assert::AreEqual(size_t(512), kRecordBytesMax);
				Assert::IsFalse(p.orderedChain || p.singletonStore, L"a plain record product");
				Assert::IsTrue(p.shortQueueSubstitute == kNoProduct && p.noOfaSubstitute == kNoProduct);
				// The same single writer family as P14: it follows the monitor evaluation (P5), stamps = content + the analysis settings.
				Assert::AreEqual(1, int(p.inputCount));
				Assert::AreEqual(int(kP5MonitorTiles), int(p.inputs[0].product));
				Assert::AreEqual(0, int(p.inputs[0].lag) + int(p.inputs[0].lead));
				Assert::IsTrue((p.settings & VpIndicator::Bit(VpIndicator::kSettingsAnalysis)) != 0);
				// P14 itself is untouched (id, size).
				Assert::AreEqual(19, int(kSourceIdentity));
				Assert::AreEqual(uint32_t(256), reg.Get(kSourceIdentity).size.fixedBytes);
			}

			TEST_METHOD(TheCatalogRowIsHeldStateWithNoLaneProducerAndNoLiveFill)
			{
				using namespace VpSkeleton;
				using namespace VpSkeleton::Catalog;
				const ProductRow* row = RowOf(kSourceGeometry);
				Assert::IsNotNull(row);
				Assert::AreEqual(std::string("input/source_analysis/identity"), std::string(row->unit));
				Assert::AreEqual(kUnitVersion, row->unitVersion, L"catalog expectation equals the unit constant");
				Assert::AreEqual(kExpectedSourceGeometryUnit, kUnitVersion);
				Assert::AreEqual(std::string(), std::string(row->laneProducer), L"held state: no lane producer");
				Assert::IsNull(LiveFillOwner(kSourceGeometry), L"no consumer can fill it live: every read is lag >= 1");
				// The provisional readers: temporal detail (with quality_pattern and coding_evidence), the adaptive scaler, the chroma stage, status.
				const std::vector<const ConsumerRow*> readers = ConsumersOf(kSourceGeometry);
				Assert::AreEqual(size_t(4), readers.size());
				std::vector<std::string> families;
				for (const ConsumerRow* c : readers)
				{
					families.push_back(c->family);
					bool read = false;
					for (const ConsumerRow& r : ConsumerRows())
						if (std::string(r.family) == c->family)
							for (size_t k = 0; k < r.decl.count; ++k)
								if (r.decl.consumes[k].product == kSourceGeometry)
								{
									read = true;
									Assert::IsTrue(r.decl.consumes[k].lag >= 1, L"a held-state read is lag >= 1");
								}
					Assert::IsTrue(read);
				}
				auto has = [&](const char* f) { for (const std::string& x : families) if (x == f) return true; return false; };
				Assert::IsTrue(has("temporal detail"));
				Assert::IsTrue(has("adaptive downscale (grain map)"));
				Assert::IsTrue(has("chroma from luma"));
				Assert::IsTrue(has("source geometry status"));
				// The unit version check engages a visible fallback for a reader built against another number.
				FallbackCatalog::Decision d;
				Assert::IsTrue(VerifyUnit(kSourceGeometry, kUnitVersion, &d));
				Assert::IsFalse(d.engaged);
				Assert::IsFalse(VerifyUnit(kSourceGeometry, kUnitVersion + 1, &d));
				Assert::IsTrue(d.engaged);
				Assert::IsNotNull(FallbackCatalog::FindProductEntry("product.source_geometry"));
			}

			TEST_METHOD(EveryProvisionalConsumerReadsAbsentAsItsClassicalFormAndNotRequestedIsByteIdentical)
			{
				// This build: the placeholder producer, nothing requested. No line, nothing engaged, no string built.
				for (Consumer c : kAll)
				{
					const ReadResult r = ReadToday(c);
					Assert::IsFalse(r.requested);
					Assert::IsFalse(r.usable, L"the classical form runs exactly as before");
					Assert::AreEqual(0u, r.cause);
					Assert::AreEqual(std::string(), r.reason);
					Assert::IsFalse(r.fallback.engaged);
					Assert::AreEqual(std::string(), r.fallback.id);
					Assert::AreEqual(std::string(), StatusLine(c, r));
				}
				Assert::IsFalse(kRequestedToday);
				Assert::IsFalse(PlaceholderEntry()->published, L"the placeholder publishes absent");
				// Requested with the placeholder (absent): every consumer takes its classical form VISIBLY.
				for (Consumer c : kAll)
				{
					const ReadResult r = Read(c, PlaceholderEntry(), true, 5, 100);
					Assert::IsTrue(r.requested);
					Assert::IsFalse(r.usable);
					Assert::AreEqual(unsigned(kCauseAbsent), r.cause);
					Assert::IsTrue(r.fallback.engaged);
					Assert::AreEqual(std::string("product.source_geometry"), r.fallback.id);
					Assert::IsTrue(r.fallback.reason.find(Name(c)) != std::string::npos, L"names the consumer");
					Assert::IsTrue(r.fallback.reason.find("absent") != std::string::npos, L"names the cause");
					Assert::IsTrue(r.fallback.reason.find(ClassicalForm(c)) != std::string::npos, L"says what runs instead");
					Assert::IsFalse(StatusLine(c, r).empty());
				}
				// No producer at all (a null entry) is its own cause.
				const ReadResult none = Read(Consumer::Status, nullptr, true, 5, 100);
				Assert::AreEqual(unsigned(kCauseNoProducer), none.cause);
				Assert::IsTrue(none.fallback.engaged);
			}

			TEST_METHOD(AFreshRecordIsUsableAndEveryOtherStateIsAVisibleCause)
			{
				const Entry fresh = Fresh();
				for (Consumer c : kAll)
				{
					const ReadResult r = Read(c, &fresh, true, 10, 1000100);
					Assert::IsTrue(r.usable && r.cause == 0u);
					Assert::IsFalse(r.fallback.engaged);
					Assert::AreEqual(std::string(), r.reason);
				}
				// Lag 1: a record sealed with seq <= N is visible to frame N (a reader of N + 1 sees the same one); a later one is not yet.
				Assert::AreEqual(0u, CauseOf(&fresh, 10, 1000100));
				Assert::AreEqual(0u, CauseOf(&fresh, 11, 1000100));
				Assert::AreEqual(unsigned(kCauseNotYet), CauseOf(&fresh, 9, 1000100), L"never the future");
				// Stale after maxAgeUs, in either direction of the clock.
				Assert::AreEqual(0u, CauseOf(&fresh, 10, 1000000 + 500000));
				Assert::AreEqual(unsigned(kCauseStale), CauseOf(&fresh, 10, 1000000 + 500001));
				Assert::AreEqual(unsigned(kCauseStale), CauseOf(&fresh, 10, 1000000 - 500001));
				// A record that states no maximum age is treated as stale (fail closed).
				Entry noAge = Fresh();
				noAge.maxAgeUs = 0;
				Assert::AreEqual(unsigned(kCauseStale), CauseOf(&noAge, 10, 1000000));
				// Another unit version.
				Entry other = Fresh();
				other.unitVersion = kUnitVersion + 1;
				const ReadResult r = Read(Consumer::TemporalDetail, &other, true, 10, 1000000);
				Assert::IsFalse(r.usable);
				Assert::AreEqual(unsigned(kCauseUnitVersion), r.cause);
				Assert::IsTrue(r.fallback.engaged && r.fallback.reason.find("unit version") != std::string::npos);
				// It recovers by itself when a fresh record returns.
				Assert::IsTrue(Read(Consumer::TemporalDetail, &fresh, true, 10, 1000000).usable);
				Assert::IsFalse(Read(Consumer::TemporalDetail, &fresh, true, 10, 1000000).fallback.engaged);
			}

			TEST_METHOD(OneDecisionCoversEveryConsumerSoOneRecoveryCannotReleaseAnotherEntry)
			{
				const Entry fresh = Fresh();
				ReadResult reads[kConsumerCount];
				for (size_t i = 0; i < kConsumerCount; ++i)
					reads[i] = Read(kAll[i], &fresh, true, 10, 1000000);
				Assert::IsFalse(Combine(reads, kConsumerCount).engaged, L"all usable: released");
				Assert::AreEqual(std::string("product.source_geometry"), Combine(reads, kConsumerCount).id);
				reads[2] = Read(Consumer::TemporalDetail, PlaceholderEntry(), true, 10, 1000000);
				reads[4] = Read(Consumer::ChromaStage, PlaceholderEntry(), true, 10, 1000000);
				const FallbackCatalog::Decision d = Combine(reads, kConsumerCount);
				Assert::IsTrue(d.engaged);
				Assert::IsTrue(d.reason.find("temporal detail") != std::string::npos && d.reason.find("chroma stage") != std::string::npos);
				Assert::IsTrue(d.reason.find("adaptive scaling") == std::string::npos, L"only the consumers that fell back are named");
				FallbackStatus::Registry registry;
				Assert::IsTrue(FallbackCatalog::Sync(registry, d), L"engaged: the heading changes");
				reads[2] = Read(Consumer::TemporalDetail, &fresh, true, 10, 1000000);
				Assert::IsTrue(Combine(reads, kConsumerCount).engaged, L"the chroma stage still says it");
				reads[4] = Read(Consumer::ChromaStage, &fresh, true, 10, 1000000);
				Assert::IsFalse(Combine(reads, kConsumerCount).engaged);
				Assert::IsTrue(FallbackCatalog::Sync(registry, Combine(reads, kConsumerCount)), L"released");
			}

			TEST_METHOD(TheMaximumAgeIsTwiceTheMeasuredEvaluationIntervalNeverAFixedRate)
			{
				Assert::AreEqual(int64_t(2), kMaxAgeIntervals);
				Assert::AreEqual(int64_t(0), MaxAgeUs(0), L"no measurement: no age (the writer must state one)");
				Assert::AreEqual(int64_t(0), MaxAgeUs(-5));
				Assert::AreEqual(int64_t(2 * 41708), MaxAgeUs(41708));
				Assert::AreEqual(int64_t(2 * 1000000), MaxAgeUs(1000000));
				Assert::IsTrue(MaxAgeUs(2000000) > MaxAgeUs(1000000), L"follows the interval");
			}

			TEST_METHOD(TheRecordByteOrderIsLittleEndianWhateverTheHost)
			{
				unsigned char b[16] = {};
				PutU16(b, 0x0102u);
				Assert::AreEqual(0x02, int(b[0]));
				Assert::AreEqual(0x01, int(b[1]));
				PutU32(b + 2, 0x01020304u);
				Assert::AreEqual(0x04, int(b[2]));
				Assert::AreEqual(0x01, int(b[5]));
				PutU64(b + 6, 0x0102030405060708ull);
				Assert::AreEqual(0x08, int(b[6]));
				Assert::AreEqual(0x01, int(b[13]));
				Assert::AreEqual(uint16_t(0x0102u), GetU16(b));
				Assert::AreEqual(uint32_t(0x01020304u), GetU32(b + 2));
				Assert::AreEqual(uint64_t(0x0102030405060708ull), GetU64(b + 6));
				unsigned char edge[8] = {};
				PutU64(edge, ~uint64_t(0));
				Assert::AreEqual(~uint64_t(0), GetU64(edge));
				PutU64(edge, 0);
				Assert::AreEqual(uint64_t(0), GetU64(edge));
			}

			TEST_METHOD(TheStatusWordingNamesNoDisplayAndEveryConsumerHasAClassicalForm)
			{
				for (Consumer c : kAll)
				{
					Assert::IsTrue(std::string(Name(c)) != "?" && !std::string(Name(c)).empty());
					Assert::IsFalse(std::string(ClassicalForm(c)).empty());
				}
				Assert::AreEqual(kConsumerCount, sizeof(kAll) / sizeof(kAll[0]));
				const std::string text = FallbackCatalog::FindProductEntry("product.source_geometry")->text;
				Assert::IsTrue(text.find("FL35") == std::string::npos);
			}
		};
	}
}
