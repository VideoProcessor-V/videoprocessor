/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// P14g source_geometry, the RECORD content (SID-C2a; the skeleton's framework tests are SourceGeometryProductTests): the payload codec
// behind the 8 B header (fixed little-endian layout, 371 payload bytes of the 504 the header leaves), the header and enumeration
// refusals, the causes of an unusable record (Record::Cause over the skeleton's CauseOf) and the held-state holder (single writer,
// lag-1 reads, ring, an undecodable newest record named instead of silently skipped). CPU only, no files, no clock.
#include "pch.h"
#include "CppUnitTest.h"

#include "GeometryFixtures.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
	TEST_CLASS(SourceGeometryRecordTests)
	{
	public:
		static GeoFx::G::Record FullRecord()
		{
			using namespace GeoFx;
			G::Record r = MakeRecord(123456789012ull, -42, AppliedAxis(1080, 2160, 5), AppliedAxis(540, 2160, -1));
			r.flags = G::kFlagShadow;
			r.streamKey = 99;
			r.publishSerial = 5;
			r.maxAgeUs = 12000000;
			r.geometryGen = 11;
			r.appliedGen = 7;
			r.discontinuities = 3;
			r.costMs = 41.5f;
			r.measured[0] = MeasuredAxis(G::LatchState::Candidate, 1080, 3840);
			r.measured[1] = MeasuredAxis(G::LatchState::Hold, 0, 2160);
			r.measured[0].phase = 0.25f;
			r.measured[0].sharpen.present = true;
			r.measured[0].sharpen.amount = 0.6f;
			r.measured[0].sharpen.radius = 1.5f;
			r.measured[0].sharpen.conf = 0.7f;
			r.measured[1].dbl.present = true;
			r.measured[1].dbl.n1 = 720;
			r.measured[1].dbl.conf = 0.8f;
			r.measured[0].reasonBits = 0x1234u;
			r.applied[1].state = G::LatchState::Unstable;
			r.applied[0].kernelParamA = 0.0f;
			r.applied[0].kernelParamB = 0.5f;
			r.area.x = 0; r.area.y = 276; r.area.w = 3840; r.area.h = 1608; r.area.conf = 0.95f;
			r.codedRaster = G::CodedRaster::PlayerUpscaled;
			r.effectiveBandwidth[0] = 0.4f;
			r.effectiveBandwidth[1] = 0.45f;
			r.chroma[0].originSampling = 1; r.chroma[0].playerUpsampled = 2; r.chroma[0].kernel = 3; r.chroma[0].siting = -2; r.chroma[0].conf = 0.5f;
			r.chroma[1].conf = 0.25f;
			return r;
		}

		TEST_METHOD(TheCodecRoundTripsEveryFieldAndFitsBehindTheHeader)
		{
			using namespace GeoFx;
			const G::Record r = FullRecord();
			const G::Packed p = G::Pack(r);
			G::Record u;
			Assert::IsTrue(G::Unpack(p, u));
			// Re-packing the unpacked record must give the same bytes (every field survived, none was invented).
			const G::Packed p2 = G::Pack(u);
			Assert::AreEqual(0, std::memcmp(p.bytes, p2.bytes, sizeof(p.bytes)), L"unpack then pack is the identity");
			Assert::IsTrue(u.unitVersion == G::kUnitVersion, L"the logical version comes from the header");
			Assert::IsTrue(u.evalSeq == r.evalSeq && u.evalTimeUs == r.evalTimeUs && u.streamKey == r.streamKey);
			Assert::IsTrue(u.geometryGen == 11 && u.appliedGen == 7 && u.discontinuities == 3 && u.maxAgeUs == 12000000);
			Assert::IsTrue(u.costMs == 41.5f);
			Assert::IsTrue(u.applied[0].n == 1080 && u.applied[0].N == 2160 && u.applied[0].ratio == 0.5f);
			Assert::IsTrue(u.applied[0].kernelIndex == 5 && u.applied[0].kernelFamily == static_cast<uint8_t>(C::Kernel(5).family));
			Assert::IsTrue(u.applied[1].kernelIndex == -1 && u.applied[1].kernelFamily == G::kKernelFamilyUnknown);
			Assert::IsTrue(u.applied[1].state == G::LatchState::Unstable);
			Assert::IsTrue(u.measured[0].state == G::LatchState::Candidate && u.measured[0].phase == 0.25f);
			Assert::IsTrue(u.measured[0].sharpen.present && u.measured[0].sharpen.amount == 0.6f && u.measured[0].sharpen.radius == 1.5f);
			Assert::IsTrue(u.measured[1].dbl.present && u.measured[1].dbl.n1 == 720 && u.measured[1].dbl.conf == 0.8f);
			Assert::IsTrue(u.measured[0].reasonBits == 0x1234u);
			Assert::IsTrue(u.area.y == 276 && u.area.h == 1608 && u.area.conf == 0.95f);
			Assert::IsTrue(u.codedRaster == G::CodedRaster::PlayerUpscaled);
			Assert::IsTrue(u.effectiveBandwidth[1] == 0.45f);
			Assert::IsTrue(u.chroma[0].siting == -2 && u.chroma[0].playerUpsampled == 2 && u.chroma[1].conf == 0.25f);
			Assert::IsTrue((u.flags & G::kFlagShadow) != 0);
			// The layout: header 8 B + payload 371 B, within the skeleton's 512 B bound and 504 B payload bound.
			Assert::IsTrue(G::kRecordBytesMax == 512 && G::kHeaderBytes == 8);
			Assert::IsTrue(G::kPayloadBytes == 371 && G::kPayloadBytes <= G::kPayloadBytesMax);
			Assert::IsTrue(G::PayloadBytesUsed() == G::kPayloadBytes, L"Pack writes exactly the declared payload bytes");
			G::Header h;
			Assert::IsTrue(G::ReadHeader(p.bytes, sizeof(p.bytes), &h) == G::HeaderStatus::Ok);
			Assert::IsTrue(h.lengthBytes == G::kHeaderBytes + G::kPayloadBytes && h.unitVersion == G::kUnitVersion);
			// The unused tail is zero (a fixed, deterministic frame).
			for (size_t i = G::kHeaderBytes + G::kPayloadBytes; i < G::kRecordBytesMax; ++i)
				Assert::IsTrue(p.bytes[i] == 0);
		}

		TEST_METHOD(TheLayoutIsLittleEndianAndFixedBehindTheHeader)
		{
			GeoFx::G::Record r;
			r.evalSeq = 0x0102030405060708ull;
			const GeoFx::G::Packed p = GeoFx::G::Pack(r);
			Assert::IsTrue(p.bytes[0] == 1 && p.bytes[1] == GeoFx::G::kMagic, L"unit version 1 in the low byte, the magic above it");
			Assert::IsTrue(p.bytes[2] == 0x7B && p.bytes[3] == 0x01, L"the length (379) little endian");
			// header 8 + flags (2) + streamKey (8) = offset 18: evalSeq little endian.
			Assert::IsTrue(p.bytes[18] == 0x08 && p.bytes[19] == 0x07 && p.bytes[25] == 0x01);
			Assert::IsTrue(GeoFx::G::kUnitVersion == 1);
		}

		TEST_METHOD(AnotherHeaderOrACorruptEnumerationIsRefusedWithItsCause)
		{
			using namespace GeoFx;
			const G::Packed p = G::Pack(FullRecord());
			G::Record u;
			u.evalSeq = 77;
			unsigned cause = 0;
			G::Packed v = p;
			v.bytes[0] = 2;   // unit version 2
			Assert::IsFalse(G::Unpack(v, u, &cause), L"another version is refused");
			Assert::IsTrue(cause == G::kCauseUnitVersion);
			Assert::IsTrue(u.evalSeq == 77, L"and the output is untouched");
			G::Packed m = p;
			m.bytes[1] = 0x00;   // not our magic
			Assert::IsFalse(G::Unpack(m, u, &cause));
			Assert::IsTrue(cause == G::kCauseCorrupt);
			G::Packed l = p;
			l.bytes[2] = 0x70;   // a length that is not this unit's
			Assert::IsFalse(G::Unpack(l, u, &cause));
			Assert::IsTrue(cause == G::kCauseCorrupt);
			G::Packed e = p;
			e.bytes[74] = 200;   // the first axis block starts at 8 + 66: its state byte
			Assert::IsFalse(G::Unpack(e, u, &cause), L"an out-of-range state is refused, not guessed");
			Assert::IsTrue(cause == G::kCauseCorrupt && u.evalSeq == 77);
		}

		// W3 (round 1): the framework's SourceGeometryProductTests stay as the skeleton team wrote them (kRequestedToday); SID's request is tested here.
		TEST_METHOD(OnlyConsensusOnRequestsTheGeometry)
		{
			Assert::IsFalse(SourceGeometry::kRequestedToday, L"the framework's constant: no estimator wired, nothing requested");
			Assert::IsFalse(SourceGeometry::Requested(false), L"source_id_consensus off (or shadow, or the Original pathway) asks for nothing");
			Assert::IsTrue(SourceGeometry::Requested(true), L"source_id_consensus on is the request");
		}

		TEST_METHOD(RecordCauseIsTheSkeletonsRuleOverTheRecordsOwnEntry)
		{
			GeoFx::G::Record r;
			r.evalSeq = 10;
			r.evalTimeUs = 10000000;
			r.maxAgeUs = 5000000;
			Assert::IsTrue(r.Cause(10, 10000000) == 0u);
			Assert::IsTrue(r.Cause(11, 15000000) == 0u, L"lag: a later frame reads the same record");
			Assert::IsTrue(r.Cause(10, 15000001) == GeoFx::G::kCauseStale);
			Assert::IsTrue(r.Cause(10, 4999999) == GeoFx::G::kCauseStale, L"a record from the future beyond the bound is stale too (a time step back)");
			Assert::IsTrue(r.Cause(9, 10000000) == GeoFx::G::kCauseNotYet, L"never the future");
			r.maxAgeUs = 0;
			Assert::IsTrue(r.Cause(10, 10000000) == GeoFx::G::kCauseNotMeasured, L"no maximum age yet: said plainly, never used");
			r.maxAgeUs = GeoFx::G::MaxAgeUs(2000000);
			Assert::IsTrue(r.maxAgeUs == 4000000 && r.Cause(10, 10000000) == 0u);
			r.unitVersion = GeoFx::G::kUnitVersion + 1;
			Assert::IsTrue(r.Cause(10, 10000000) == GeoFx::G::kCauseUnitVersion);
		}

		TEST_METHOD(HeldStateReadsTheLatestSealedRecordAtOrBeforeTheSequence)
		{
			using namespace GeoFx;
			G::HeldState h;
			G::Record out;
			unsigned cause = 0;
			Assert::IsFalse(h.ReadLatest(0, out, &cause), L"nothing published yet");
			Assert::IsTrue(cause == G::kCauseAbsent);
			for (uint64_t i = 1; i <= 3; ++i)
			{
				G::Record r = MakeRecord(i * 10, static_cast<int64_t>(i) * 1000000, AppliedAxis(1080, 2160, 5), AppliedAxis(1080, 2160, 5));
				r.appliedGen = i;
				Assert::IsTrue(h.Publish(r) == i, L"the serial is stamped last and returned");
			}
			Assert::IsTrue(h.ReadLatest(0, out) && out.appliedGen == 3, L"0 = the newest");
			Assert::IsTrue(h.ReadLatest(25, out) && out.appliedGen == 2, L"lag: the record that was current for an earlier frame");
			Assert::IsTrue(h.ReadLatest(10, out) && out.appliedGen == 1);
			Assert::IsFalse(h.ReadLatest(9, out, &cause), L"nothing that old");
			Assert::IsTrue(cause == G::kCauseAbsent);
			h.Reset();
			Assert::IsFalse(h.ReadLatest(0, out));
			Assert::IsTrue(h.Serial() == 0);
		}

		TEST_METHOD(AnUndecodableNewestRecordIsNamedNotSkipped)
		{
			using namespace GeoFx;
			G::HeldState h;
			G::Record out;
			unsigned cause = 0;
			h.Publish(MakeRecord(10, 1000000, AppliedAxis(1080, 2160, 5), AppliedAxis(1080, 2160, 5)));
			G::Packed other = G::Pack(MakeRecord(20, 2000000, AppliedAxis(540, 2160, 5), AppliedAxis(540, 2160, 5)));
			other.bytes[0] = 2;   // another unit version, sealed at 20
			h.PublishPacked(other, 20);
			Assert::IsTrue(h.ReadLatest(15, out, &cause) && out.evalSeq == 10, L"a reader of frame 15 never needed the newer record");
			Assert::IsFalse(h.ReadLatest(25, out, &cause), L"a reader that needs it is told, not given an older valid one");
			Assert::IsTrue(cause == G::kCauseUnitVersion);
			Assert::IsFalse(h.ReadLatest(0, out, &cause));
			Assert::IsTrue(cause == G::kCauseUnitVersion);
			G::Packed bad = G::Pack(MakeRecord(30, 3000000, AppliedAxis(540, 2160, 5), AppliedAxis(540, 2160, 5)));
			bad.bytes[74] = 200;
			h.PublishPacked(bad, 30);
			Assert::IsFalse(h.ReadLatest(0, out, &cause));
			Assert::IsTrue(cause == G::kCauseCorrupt);
			// A good record after the bad ones is read again by everyone.
			h.Publish(MakeRecord(40, 4000000, AppliedAxis(1080, 2160, 5), AppliedAxis(1080, 2160, 5)));
			Assert::IsTrue(h.ReadLatest(0, out) && out.evalSeq == 40);
		}

		TEST_METHOD(TheRingKeepsTheLastEightRecords)
		{
			using namespace GeoFx;
			G::HeldState h;
			for (uint64_t i = 1; i <= 20; ++i)
			{
				G::Record r = MakeRecord(i, static_cast<int64_t>(i), AppliedAxis(1080, 2160, 5), AppliedAxis(1080, 2160, 5));
				r.appliedGen = i;
				h.Publish(r);
			}
			G::Record out;
			Assert::IsTrue(h.ReadLatest(13, out) && out.appliedGen == 13, L"the 8th newest is still there");
			Assert::IsFalse(h.ReadLatest(12, out), L"older ones were overwritten");
			Assert::IsTrue(h.Serial() == 20);
		}

		TEST_METHOD(AReaderNeverSeesATornRecordWhileTheWriterPublishes)
		{
			using namespace GeoFx;
			G::HeldState h;
			std::atomic<bool> stop{ false };
			std::atomic<int> torn{ 0 };
			std::thread reader([&] {
				G::Record out;
				while (!stop.load())
					if (h.ReadLatest(0, out) && (out.appliedGen != out.geometryGen || out.applied[0].n != static_cast<int32_t>(out.appliedGen)))
						++torn;
			});
			for (uint64_t i = 1; i <= 3000; ++i)
			{
				G::Record r = MakeRecord(i, static_cast<int64_t>(i), AppliedAxis(static_cast<int>(i), 5000, 5), AppliedAxis(1080, 2160, 5));
				r.appliedGen = i;
				r.geometryGen = i;
				h.Publish(r);
			}
			stop.store(true);
			reader.join();
			Assert::AreEqual(0, torn.load(), L"every record read is one whole record");
		}
	};
}
