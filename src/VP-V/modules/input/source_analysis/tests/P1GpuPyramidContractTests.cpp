// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// L-B4 (BBP-B B4): the matcher pyramid variant of kP1HalfLuma (lane/P1GpuFill.h). CPU only.
//   P1GpuPyramidTextPinTests    the level bodies the owner fill runs are the former matcher text, byte for byte: the 2x2 body is
//                               the old DejudderWarp.cpp DOWNSAMPLE_BODY (ab32bc33b, 897 bytes, sha256 e667dd5f6236df2c...) with
//                               exactly its two fade blocks removed, and both bodies are pinned by length + FNV-1a 64.
//   P1GpuPyramidVariantTests    the key { codeUnit 8, scale 1..6 } is canonical and the only pyramid spelling; R16F storage only
//                               for that variant; a Float16 PRECISION request is still refused; level sizes = the matcher's own.
// No CPU emulation of the 4x4 law exists (P1GpuFill.h emulates only the canonical 2x2 Unorm16 / Float32 forms), so the 4x4
// levels' values are proven by the GPU identity classes (HalfLumaPassesIdentityGpuTests, MatcherPyramidGpuIdentityTests).
#include <VP-V/modules/input/source_analysis/lane/P1GpuFill.h>
#include <VP-V/modules/input/source_analysis/lane/shared/HalfLuma.h>
#include <VP-V/core/VariantKey.h>
#include <VP-V/core/VariantShape.h>
#include <cstdint>
#include <string>
#include "CppUnitTest.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	namespace
	{
		uint64_t PyramidFnv1a64(const std::string& s)
		{
			uint64_t h = 1469598103934665603ull;
			for (unsigned char c : s)
			{
				h ^= c;
				h *= 1099511628211ull;
			}
			return h;
		}

		// The former matcher body, verbatim from ab32bc33b DejudderWarp.cpp DOWNSAMPLE_BODY (897 bytes). Reference text only:
		// nothing compiles it.
		const char* const kFormerDownsampleBody = R"GLSL(
ivec2 p = ivec2(gl_GlobalInvocationID.xy);
ivec2 size = imageSize(dst);
if (p.x < size.x && p.y < size.y) {
    float v;
#ifdef FADE_PLAIN
    // No fade active: B's uncorrected half-size value, stored by the plain
    // pass (the same value this pass would store).
    if (texelFetch(fade_state, ivec2(0), 0).z <= 0.5)
        v = texelFetch(plain, p, 0).r;
    else
#endif
    {
        // The shared U3 accessor (HalfLuma.h): the same taps, edge clamp and (a + b + c + d) * 0.25 as the
        // matcher's former private loop, then the code scale; the arithmetic order is unchanged.
        v = vpu_half_luma(src, p) * codes;
#ifdef FADE_APPLY
        // M4: B after the fade's global gain and offset (when active).
        vec4 fs = texelFetch(fade_state, ivec2(0), 0);
        if (fs.z > 0.5)
            v = v * fs.x + fs.y;
#endif
    }
    imageStore(dst, p, vec4(v, 0.0, 0.0, 1.0));
}
)GLSL";

		// The former body with its #ifdef FADE_... blocks (each through its "#endif\n") removed.
		std::string WithoutFadeBlocks(std::string s, int* removed)
		{
			*removed = 0;
			for (;;)
			{
				const size_t a = s.find("#ifdef FADE_");
				if (a == std::string::npos)
					return s;
				const size_t e = s.find("#endif\n", a);
				if (e == std::string::npos)
					return s;
				s.erase(a, e + 7 - a);
				++*removed;
			}
		}
	}

	TEST_CLASS(P1GpuPyramidTextPinTests)
	{
	public:
		TEST_METHOD(TheHalfBodyIsTheFormerMatcherBodyWithoutItsFadeBlocks)
		{
			const std::string former(kFormerDownsampleBody);
			Assert::AreEqual(size_t(897), former.size(), L"the reference is the 897-byte former DOWNSAMPLE_BODY");
			int removed = 0;
			const std::string expected = WithoutFadeBlocks(former, &removed);
			Assert::AreEqual(2, removed, L"exactly the two fade blocks (FADE_PLAIN, FADE_APPLY)");
			const std::string& body = VpFrameAnalysis::P1Gpu::MatcherHalfBody();
			Assert::IsTrue(body == expected, L"MatcherHalfBody = the former body minus its fade blocks, byte for byte");
			Assert::IsTrue(body == std::string(VpFrameAnalysis::P1Gpu::MatcherHalfHead()) + VpFrameAnalysis::P1Gpu::MatcherHalfCore()
				+ VpFrameAnalysis::P1Gpu::MatcherHalfTail(), L"head + core + tail");
			// The former text is the pieces with the fade blocks spliced between them (the matcher's own body today).
			const size_t plain = former.find("#ifdef FADE_PLAIN");
			const size_t apply = former.find("#ifdef FADE_APPLY");
			Assert::IsTrue(former.compare(0, plain, VpFrameAnalysis::P1Gpu::MatcherHalfHead()) == 0, L"the head precedes FADE_PLAIN");
			Assert::IsTrue(former.find(VpFrameAnalysis::P1Gpu::MatcherHalfCore()) < apply, L"the core precedes FADE_APPLY");
			// Pins (computed from the strings at 20a5a718c; re-pin only with a byte-identity proof against the former text).
			Assert::AreEqual(size_t(433), body.size(), L"2x2 body length");
			Assert::IsTrue(PyramidFnv1a64(body) == 0x201d1be19b422d7full, L"2x2 body FNV-1a 64");
		}

		TEST_METHOD(TheLevelBodyIsTheSharedUnitsBox4x4Body)
		{
			const std::string& body = VpFrameAnalysis::P1Gpu::MatcherPyramidLevelBody();
			Assert::IsTrue(body == VpUnit::HalfLuma::GlslBox4x4LevelBody(), L"the one copy: U3's GlslBox4x4LevelBody");
			Assert::IsTrue(&body == &VpFrameAnalysis::P1Gpu::MatcherPyramidLevelBody(), L"one string object (the matcher and the owner read it)");
			Assert::IsTrue(body.find("p * 4 + ivec2(x, y), src_size - ivec2(1)") != std::string::npos, L"min-edge 4x4 taps");
			Assert::IsTrue(body.find("sum / 16.0") != std::string::npos, L"sum / 16");
			Assert::AreEqual(size_t(387), body.size(), L"4x4 body length");
			Assert::IsTrue(PyramidFnv1a64(body) == 0x0efbc15a6cc505b5ull, L"4x4 body FNV-1a 64");
		}
	};

	TEST_CLASS(P1GpuPyramidVariantTests)
	{
	public:
		TEST_METHOD(TheKeyIsCanonicalAndTheOnlySpellingOfALevel)
		{
			using namespace VpFrameAnalysis::P1Gpu;
			using VpSkeleton::VariantKey;
			const uint32_t pyramidAxes = uint32_t(VariantKey::kAxisCodeUnit) | uint32_t(VariantKey::kAxisScale);
			for (uint8_t level = 1; level <= 6; ++level)
			{
				const VariantKey k = MatcherPyramidKey(level);
				Assert::AreEqual(unsigned(kMatcherCodeUnit), unsigned(k.codeUnit), L"codeUnit 8");
				Assert::AreEqual(unsigned(level), unsigned(k.scale), L"scale = level");
				Assert::AreEqual(pyramidAxes, k.Axes(), L"code unit and scale axes only");
				const VariantKey c = k.Canonical();
				Assert::IsTrue(c.Hash() == k.Hash() && c.Axes() == k.Axes(), L"already canonical against the owner's defaults");
				Assert::AreEqual(unsigned(level), unsigned(MatcherPyramidLevel(c)), L"the level read back");
				Assert::IsTrue((k.Axes() & ~OwnerVariantAxes()) == 0u, L"the owner declares both axes");
			}
			VariantKey k = MatcherPyramidKey(1);
			k.scale = 0;
			Assert::AreEqual(0u, unsigned(MatcherPyramidLevel(k)), L"codeUnit 8 without a level");
			k = MatcherPyramidKey(1);
			k.scale = 7;
			Assert::AreEqual(0u, unsigned(MatcherPyramidLevel(k)), L"level 7 does not exist");
			k = MatcherPyramidKey(3);
			k.codeUnit = 10;
			Assert::AreEqual(0u, unsigned(MatcherPyramidLevel(k)), L"another code unit");
			k = VariantKey();
			k.scale = 3;
			Assert::AreEqual(0u, unsigned(MatcherPyramidLevel(k)), L"a level without codeUnit 8 (the canonical forms have none)");
			k = MatcherPyramidKey(1);
			k.precision = VpSkeleton::kPrecisionFloat32;
			Assert::AreEqual(0u, unsigned(MatcherPyramidLevel(k)), L"a precision request with the pyramid key");
			k = MatcherPyramidKey(1);
			k.precision = VpSkeleton::kPrecisionFloat16;
			Assert::AreEqual(0u, unsigned(MatcherPyramidLevel(k)), L"float16 with the pyramid key");
			k = MatcherPyramidKey(1);
			k.plane = 2;
			Assert::AreEqual(0u, unsigned(MatcherPyramidLevel(k)), L"any other axis");
			Assert::AreEqual(0u, unsigned(MatcherPyramidLevel(VariantKey())), L"the canonical default form is not a pyramid level");
		}

		TEST_METHOD(EachLevelsLawAndCodes)
		{
			using namespace VpFrameAnalysis::P1Gpu;
			MatcherLevelLaw law;
			Assert::IsTrue(MatcherLevelLawOf(1, &law) && law.input == 0 && law.twoByTwo && law.rangeCodes, L"1 = half of the source, range codes");
			Assert::IsTrue(MatcherLevelLawOf(2, &law) && law.input == 1 && law.twoByTwo && !law.rangeCodes, L"2 = 2x2 of level 1, codes 1");
			for (uint8_t level = 3; level <= 6; ++level)
				Assert::IsTrue(MatcherLevelLawOf(level, &law) && law.input == level - 2 && !law.twoByTwo && !law.rangeCodes, L"3..6 = 4x4 of level - 2");
			Assert::IsFalse(MatcherLevelLawOf(0, &law), L"no level 0");
			Assert::IsFalse(MatcherLevelLawOf(7, &law), L"no level 7");
			Assert::AreEqual(255.0f, MatcherCodes(false, false), L"limited range: 255");
			Assert::AreEqual(219.0f, MatcherCodes(false, true), L"full range: 219 (limited-equivalent)");
			Assert::AreEqual(1.0f, MatcherCodes(true, false), L"input already in codes");
			Assert::AreEqual(1.0f, MatcherCodes(true, true), L"input already in codes");
		}

		TEST_METHOD(R16fOnlyForThePyramidVariantAndFloat16PrecisionStillRefused)
		{
			using namespace VpFrameAnalysis::P1Gpu;
			using namespace VpSkeleton;
			using VpUnit::HalfLuma::HalfDim;
			using VpUnit::HalfLuma::QuarterDim;
			Assert::IsTrue(MatcherStorageFormat() == TexFormat::R16F, L"the matcher's existing storage (analysis data, OD:670 (d))");
			const uint32_t sizes[][2] = { { 1920, 1080 }, { 3840, 2160 }, { 1279, 719 }, { 33, 17 }, { 7, 5 } };
			for (const auto& s : sizes)
			{
				Dims full;
				full.width = s[0];
				full.height = s[1];
				// The matcher's own sizes (DejudderWarp.cpp EnsureTextures): half = (n + 1) / 2, each 4x4 level = (n + 3) / 4; the
				// lowRes inner's luma is the outer half.
				const uint32_t want[7][2] = {
					{ 0, 0 },
					{ HalfDim(s[0]), HalfDim(s[1]) },
					{ HalfDim(HalfDim(s[0])), HalfDim(HalfDim(s[1])) },
					{ QuarterDim(HalfDim(s[0])), QuarterDim(HalfDim(s[1])) },
					{ QuarterDim(HalfDim(HalfDim(s[0]))), QuarterDim(HalfDim(HalfDim(s[1]))) },
					{ QuarterDim(QuarterDim(HalfDim(s[0]))), QuarterDim(QuarterDim(HalfDim(s[1]))) },
					{ QuarterDim(QuarterDim(HalfDim(HalfDim(s[0])))), QuarterDim(QuarterDim(HalfDim(HalfDim(s[1])))) },
				};
				for (uint8_t level = 1; level <= 6; ++level)
				{
					TexShape shape;
					Assert::IsTrue(VariantTexShape(MatcherPyramidKey(level), full, 1, MatcherStorageFormat(), &shape), L"a shape for every level");
					Assert::IsTrue(shape.fmt == TexFormat::R16F, L"stored R16F");
					Assert::AreEqual(want[level][0], shape.w, L"level width = the matcher's own");
					Assert::AreEqual(want[level][1], shape.h, L"level height = the matcher's own");
					Assert::IsTrue(VariantReserveBytes(MatcherPyramidKey(level), full, 1, MatcherStorageFormat()) == uint64_t(shape.w) * shape.h * 2u, L"2 bytes per level texel");
				}
			}
			// The canonical P1 GPU forms stay 12-bit safe; a Float16 PRECISION request is refused for every product (N2).
			Dims full;
			full.width = 64;
			full.height = 48;
			TexShape shape;
			VariantKey u16;
			u16.precision = kPrecisionUnorm16;
			Assert::IsTrue(VariantTexShape(u16, full, 1, MatcherStorageFormat(), &shape) && shape.fmt == TexFormat::R16, L"unorm16 = R16");
			VariantKey f32;
			f32.precision = kPrecisionFloat32;
			Assert::IsTrue(VariantTexShape(f32, full, 1, MatcherStorageFormat(), &shape) && shape.fmt == TexFormat::R32F, L"float32 = R32F");
			VariantKey f16;
			f16.precision = kPrecisionFloat16;
			Assert::IsFalse(VariantTexShape(f16, full, 1, MatcherStorageFormat(), &shape), L"a float16 precision request is refused");
			VariantKey f16Level = MatcherPyramidKey(1);
			f16Level.precision = kPrecisionFloat16;
			Assert::IsFalse(VariantTexShape(f16Level, full, 1, MatcherStorageFormat(), &shape), L"also with the pyramid key");
			Assert::IsFalse(PrecisionTwelveBitSafe(kPrecisionFloat16), L"float16 is not 12-bit safe");
		}
	};
}
