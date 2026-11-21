// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// B-prereq (a) A3: the ONE codec of the P2 CPU form (P2PairRecord.h). Round trip exact; the 96-byte layout and the "P2R1" magic
// pinned; refusal (size, magic, null) leaves the output untouched; a short buffer encodes nothing; the flag bits.
// CPU only, header-only code (built without the precompiled header: the record header must stand alone).
#include <VP-V/modules/motion/estimation/P2PairRecord.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "CppUnitTest.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	namespace
	{
		VpMotion::P2PairRecord SampleRecord()
		{
			float g[2][4] = { { 1.25f, -2.5f, 0.125f, 3.0f }, { -1.25f, 2.5f, 0.25f, 4.0f } };
			float s[2][4] = { { 0.5f, 0.75f, 0.0625f, 1.0f }, { -0.5f, -0.75f, 0.03125f, 2.0f } };
			return VpMotion::MakeP2PairRecord(1000000000123ull, 1000000000124ull, 7u, g, s, true, false);
		}

		bool SameRecord(const VpMotion::P2PairRecord& a, const VpMotion::P2PairRecord& b)
		{
			if (a.magic != b.magic || a.flags != b.flags || a.from != b.from || a.to != b.to || a.generation != b.generation)
				return false;
			return std::memcmp(a.global, b.global, sizeof(a.global)) == 0 && std::memcmp(a.second, b.second, sizeof(a.second)) == 0;
		}
	}

	TEST_CLASS(P2PairRecordCodecTests)
	{
	public:
		TEST_METHOD(RoundTripIsExact)
		{
			const VpMotion::P2PairRecord in = SampleRecord();
			std::vector<uint8_t> bytes(VpMotion::kP2PairRecordBytes, 0);
			Assert::AreEqual(VpMotion::kP2PairRecordBytes, VpMotion::EncodeP2PairRecord(in, bytes.data(), bytes.size()), L"the whole record");
			VpMotion::P2PairRecord out;
			Assert::IsTrue(VpMotion::DecodeP2PairRecord(bytes.data(), bytes.size(), out), L"decoded");
			Assert::IsTrue(SameRecord(in, out), L"every field bit for bit");
			Assert::AreEqual(uint64_t(1000000000123ull), out.from, L"64-bit from");
			Assert::AreEqual(uint64_t(1000000000124ull), out.to, L"64-bit to");
			Assert::AreEqual(uint64_t(7), out.generation);
			Assert::AreEqual(1.25f, out.global[0][0]);
			Assert::AreEqual(4.0f, out.global[1][3]);
			Assert::AreEqual(0.03125f, out.second[1][2]);
		}

		TEST_METHOD(SizeMagicAndLayoutArePinned)
		{
			Assert::AreEqual(size_t(96), VpMotion::kP2PairRecordBytes, L"4 + 4 + 3 x 8 + 2 x 32 (the plan's 112 was an erratum)");
			const VpMotion::P2PairRecord in = SampleRecord();
			std::vector<uint8_t> bytes(VpMotion::kP2PairRecordBytes, 0);
			Assert::AreEqual(size_t(96), VpMotion::EncodeP2PairRecord(in, bytes.data(), bytes.size()));
			Assert::AreEqual(uint8_t('P'), bytes[0], L"magic byte 0");
			Assert::AreEqual(uint8_t('2'), bytes[1], L"magic byte 1");
			Assert::AreEqual(uint8_t('R'), bytes[2], L"magic byte 2");
			Assert::AreEqual(uint8_t('1'), bytes[3], L"magic byte 3");
			uint32_t flags = 0;
			uint64_t from = 0, to = 0, generation = 0;
			float g10 = 0.0f, s00 = 0.0f, s13 = 0.0f;
			std::memcpy(&flags, bytes.data() + 4, 4);
			std::memcpy(&from, bytes.data() + 8, 8);
			std::memcpy(&to, bytes.data() + 16, 8);
			std::memcpy(&generation, bytes.data() + 24, 8);
			std::memcpy(&g10, bytes.data() + 32 + 4 * 4, 4);   // global[1][0]
			std::memcpy(&s00, bytes.data() + 64, 4);           // second[0][0]
			std::memcpy(&s13, bytes.data() + 64 + 7 * 4, 4);   // second[1][3]
			Assert::AreEqual(in.flags, flags, L"flags at 4");
			Assert::AreEqual(in.from, from, L"from at 8");
			Assert::AreEqual(in.to, to, L"to at 16");
			Assert::AreEqual(in.generation, generation, L"generation at 24");
			Assert::AreEqual(in.global[1][0], g10, L"global[2][4] at 32, fwd then bwd");
			Assert::AreEqual(in.second[0][0], s00, L"second[2][4] at 64");
			Assert::AreEqual(in.second[1][3], s13, L"the last float ends at 96");
		}

		TEST_METHOD(RefusalLeavesTheOutputUntouched)
		{
			const VpMotion::P2PairRecord in = SampleRecord();
			std::vector<uint8_t> bytes(VpMotion::kP2PairRecordBytes + 1, 0);
			Assert::AreEqual(VpMotion::kP2PairRecordBytes, VpMotion::EncodeP2PairRecord(in, bytes.data(), bytes.size()), L"a larger cap writes 96");

			float g[2][4] = { { 9.0f, 9.0f, 9.0f, 9.0f }, { 9.0f, 9.0f, 9.0f, 9.0f } };
			const VpMotion::P2PairRecord sentinel = VpMotion::MakeP2PairRecord(55, 56, 99, g, g, false, true);
			VpMotion::P2PairRecord out = sentinel;
			Assert::IsFalse(VpMotion::DecodeP2PairRecord(bytes.data(), VpMotion::kP2PairRecordBytes - 1, out), L"one byte short");
			Assert::IsTrue(SameRecord(sentinel, out), L"untouched (short)");
			Assert::IsFalse(VpMotion::DecodeP2PairRecord(bytes.data(), VpMotion::kP2PairRecordBytes + 1, out), L"one byte long");
			Assert::IsTrue(SameRecord(sentinel, out), L"untouched (long)");
			Assert::IsFalse(VpMotion::DecodeP2PairRecord(nullptr, VpMotion::kP2PairRecordBytes, out), L"no bytes");
			Assert::IsTrue(SameRecord(sentinel, out), L"untouched (null)");
			std::vector<uint8_t> bad(bytes.begin(), bytes.begin() + VpMotion::kP2PairRecordBytes);
			bad[3] = uint8_t('2');   // "P2R2"
			Assert::IsFalse(VpMotion::DecodeP2PairRecord(bad.data(), bad.size(), out), L"another magic");
			Assert::IsTrue(SameRecord(sentinel, out), L"untouched (magic)");
			Assert::IsTrue(VpMotion::DecodeP2PairRecord(bytes.data(), VpMotion::kP2PairRecordBytes, out), L"the exact size decodes");
			Assert::IsTrue(SameRecord(in, out));
		}

		TEST_METHOD(AShortBufferEncodesNothing)
		{
			const VpMotion::P2PairRecord in = SampleRecord();
			std::vector<uint8_t> bytes(VpMotion::kP2PairRecordBytes, 0xA5u);
			Assert::AreEqual(size_t(0), VpMotion::EncodeP2PairRecord(in, bytes.data(), VpMotion::kP2PairRecordBytes - 1), L"cap 95");
			for (uint8_t b : bytes)
				Assert::AreEqual(uint8_t(0xA5u), b, L"nothing written");
			Assert::AreEqual(size_t(0), VpMotion::EncodeP2PairRecord(in, nullptr, VpMotion::kP2PairRecordBytes), L"no buffer");
		}

		TEST_METHOD(FlagBitsFollowGlobalSetAndGeneral)
		{
			float z[2][4] = {};
			const VpMotion::P2PairRecord none = VpMotion::MakeP2PairRecord(1, 2, 1, z, z, false, false);
			const VpMotion::P2PairRecord set = VpMotion::MakeP2PairRecord(1, 2, 1, z, z, true, false);
			const VpMotion::P2PairRecord general = VpMotion::MakeP2PairRecord(1, 2, 1, z, z, false, true);
			const VpMotion::P2PairRecord both = VpMotion::MakeP2PairRecord(1, 2, 1, z, z, true, true);
			Assert::AreEqual(0u, none.flags);
			Assert::AreEqual(VpMotion::kP2PairFlagGlobalSet, set.flags, L"bit 0");
			Assert::AreEqual(VpMotion::kP2PairFlagGlobalGeneral, general.flags, L"bit 1");
			Assert::AreEqual(3u, both.flags);
			Assert::IsTrue(set.GlobalSet() && !set.GlobalGeneral());
			Assert::IsTrue(!general.GlobalSet() && general.GlobalGeneral());
			Assert::AreEqual(VpMotion::kP2PairRecordMagic, none.magic, L"a made record carries the magic");
		}
	};
}
