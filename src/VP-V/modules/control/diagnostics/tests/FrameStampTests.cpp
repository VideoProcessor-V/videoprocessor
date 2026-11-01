// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/core/OutputHookOrder.h>
#include <VP-V/modules/control/diagnostics/FrameStamp.h>
#include <VP-V/modules/control/diagnostics/FrameStampShader.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// vp-stamp/1 (FrameStamp.h is the format's single source of truth; tools/framestamp/framestamp.py
// mirrors it). The golden literals below are IDENTICAL to test_framestamp.py's.
namespace
{
	namespace FS = VpFrameStamp;
	using FS::Corner;

	struct GoldenWord { int kind; int aux; uint32_t index; uint64_t word; };
	const GoldenWord kGolden[] = {
		{ 0, 0, 0u, 0xA50000000000AC39ull },
		{ 0, 4, 1u, 0xA50400000001351Eull },
		{ 1, 5, 0xFFFFFFFFu, 0xA545FFFFFFFF07C9ull },
		{ 1, 63, 0x12345678u, 0xA57F12345678606Aull },
		{ 0, 63, 0xDEADBEEFu, 0xA53FDEADBEEF0179ull },
		{ 1, 0, 1u, 0xA54000000001AD70ull },
	};
	// FNV-1a 64 of the little-endian uint16 bytes: 480x270 10-bit limited, flat 64, source stamp (0, 3, 0x01020304).
	const uint64_t kGoldenFrameHash = 0x2A42BB6007F3FDB5ull;

	const int kHeights[] = { 270, 720, 1080, 2160, 541, 999 };

	int WidthFor(int h) { return (h * 16 + 8) / 9; }

	struct Frame
	{
		int w = 0, h = 0;
		std::vector<uint16_t> p;
		Frame(int width, int height, uint16_t fill) : w(width), h(height), p(size_t(width) * height, fill) {}
		uint16_t& at(int x, int y) { return p[size_t(y) * w + x]; }
		uint16_t at(int x, int y) const { return p[size_t(y) * w + x]; }
	};

	Frame MakeFrame(int h, int depth, bool full, Corner corner, uint64_t word, int w = 0, int bg = -1)
	{
		if (w == 0) w = WidthFor(h);
		uint32_t black, white;
		FS::Levels(depth, full, black, white);
		Frame f(w, h, static_cast<uint16_t>(bg < 0 ? black : bg));
		FS::Write(f.p.data(), w, h, w, depth, full, corner, word);
		return f;
	}

	FS::Result Decode(const Frame& f, int depth, Corner corner, double radius = FS::kDefaultRadius)
	{
		return FS::Decode(f.p.data(), f.w, f.h, f.w, depth, corner, radius);
	}

	Frame BilinearUp2(const Frame& s)
	{
		Frame o(s.w * 2, s.h * 2, 0);
		for (int y = 0; y < o.h; ++y)
			for (int x = 0; x < o.w; ++x)
			{
				const double sy = (y + 0.5) / 2 - 0.5, sx = (x + 0.5) / 2 - 0.5;
				const int y0 = std::max(0, std::min(s.h - 1, static_cast<int>(std::floor(sy))));
				const int x0 = std::max(0, std::min(s.w - 1, static_cast<int>(std::floor(sx))));
				const int y1 = std::min(s.h - 1, y0 + 1), x1 = std::min(s.w - 1, x0 + 1);
				const double fy = std::max(0.0, std::min(1.0, sy - std::floor(sy)));
				const double fx = std::max(0.0, std::min(1.0, sx - std::floor(sx)));
				const double top = s.at(x0, y0) * (1 - fx) + s.at(x1, y0) * fx;
				const double bot = s.at(x0, y1) * (1 - fx) + s.at(x1, y1) * fx;
				o.at(x, y) = static_cast<uint16_t>(std::floor(top * (1 - fy) + bot * fy + 0.5));
			}
		return o;
	}

	Frame BoxDown2(const Frame& s)
	{
		Frame o(s.w / 2, s.h / 2, 0);
		for (int y = 0; y < o.h; ++y)
			for (int x = 0; x < o.w; ++x)
				o.at(x, y) = static_cast<uint16_t>((s.at(2 * x, 2 * y) + s.at(2 * x + 1, 2 * y) +
					s.at(2 * x, 2 * y + 1) + s.at(2 * x + 1, 2 * y + 1) + 2) / 4);
		return o;
	}

	struct Hook { int id; };
}

namespace FrameStampTests_ns
{
	TEST_CLASS(FrameStampTests)
	{
	public:
		TEST_METHOD(CrcCheckValue)
		{
			const uint8_t text[] = { '1', '2', '3', '4', '5', '6', '7', '8', '9' };
			Assert::IsTrue(FS::Crc16(text, 9) == 0x29B1);
		}

		TEST_METHOD(GoldenWords)
		{
			for (const GoldenWord& g : kGolden)
				Assert::IsTrue(FS::Word(g.kind, g.aux, g.index) == g.word, L"golden word");
		}

		TEST_METHOD(GoldenFrameHash)
		{
			Frame f(480, 270, 64);
			FS::Write(f.p.data(), 480, 270, 480, 10, false, Corner::Source, FS::Word(0, 3, 0x01020304u));
			const uint64_t hash = FS::Fnv1a64(reinterpret_cast<const uint8_t*>(f.p.data()), f.p.size() * 2);
			Assert::IsTrue(hash == kGoldenFrameHash, L"golden frame hash");
		}

		TEST_METHOD(WordFieldsAndHiLo)
		{
			const uint64_t w = FS::Word(1, 5, 0xFFFFFFFFu);
			Assert::IsTrue(FS::WordHi(w) == 0xA545FFFFu);
			Assert::IsTrue(FS::WordLo(w) == 0xFFFF07C9u);
		}

		TEST_METHOD(RoundTripAllHeightsBothCorners)
		{
			for (int h : kHeights)
				for (int c = 0; c < 2; ++c)
				{
					const Corner corner = c == 0 ? Corner::Source : Corner::Output;
					const Frame f = MakeFrame(h, 10, false, corner, FS::Word(c, 17, 0xA5C3F00Du));
					const FS::Result r = Decode(f, 10, corner);
					Assert::IsTrue(r.found, L"found");
					Assert::IsTrue(r.kind == c && r.aux == 17 && r.index == 0xA5C3F00Du, L"fields");
					Assert::IsTrue(r.offsetX == 0.0 && r.offsetY == 0.0, L"no shift");
				}
		}

		TEST_METHOD(DepthsAndRanges)
		{
			for (int depth : { 8, 10, 12, 16 })
				for (int full = 0; full < 2; ++full)
				{
					const Frame f = MakeFrame(540, depth, full != 0, Corner::Source, FS::Word(0, 3, 12345u));
					const FS::Result r = Decode(f, depth, Corner::Source);
					Assert::IsTrue(r.found && r.index == 12345u, L"depth/range");
				}
			// 8-bit samples through the uint8_t writer.
			std::vector<uint8_t> y(960 * 540, 16);
			FS::Write(y.data(), 960, 540, 960, 8, false, Corner::Output, FS::Word(1, 1, 99u));
			const FS::Result r = FS::Decode(y.data(), 960, 540, 960, 8, Corner::Output);
			Assert::IsTrue(r.found && r.kind == 1 && r.index == 99u, L"uint8 writer");
		}

		TEST_METHOD(LimitedLevels)
		{
			uint32_t b, w;
			FS::Levels(10, false, b, w); Assert::IsTrue(b == 64 && w == 940);
			FS::Levels(8, false, b, w); Assert::IsTrue(b == 16 && w == 235);
			FS::Levels(12, false, b, w); Assert::IsTrue(b == 256 && w == 3760);
			FS::Levels(10, true, b, w); Assert::IsTrue(b == 0 && w == 1023);
		}

		TEST_METHOD(FieldExtremes)
		{
			for (int kind = 0; kind < 4; ++kind)
				for (int aux : { 0, 63 })
					for (uint32_t index : { 0u, 0xFFFFFFFFu })
					{
						const Frame f = MakeFrame(540, 10, false, Corner::Source, FS::Word(kind, aux, index));
						const FS::Result r = Decode(f, 10, Corner::Source);
						Assert::IsTrue(r.found && r.kind == kind && r.aux == aux && r.index == index, L"extremes");
					}
		}

		TEST_METHOD(ScaledUpAndDownStillDecodes)
		{
			const Frame src = MakeFrame(1080, 10, false, Corner::Output, FS::Word(1, 9, 777u));
			const Frame up = BilinearUp2(src);
			Assert::IsTrue(up.h == 2160);
			FS::Result r = Decode(up, 10, Corner::Output);
			Assert::IsTrue(r.found && r.aux == 9 && r.index == 777u, L"1080 -> 2160");
			const Frame big = MakeFrame(2160, 10, false, Corner::Source, FS::Word(0, 2, 4242u), 3840);
			r = Decode(BoxDown2(big), 10, Corner::Source);
			Assert::IsTrue(r.found && r.aux == 2 && r.index == 4242u, L"2160 -> 1080");
		}

		TEST_METHOD(ChromaIsIrrelevantPackedUyvy)
		{
			// UYVY: luma at odd bytes, pseudo-random chroma at even bytes: the decoder reads luma only.
			const Frame luma = MakeFrame(540, 8, false, Corner::Source, FS::Word(0, 3, 12345u), 960);
			std::vector<uint8_t> packed(size_t(960) * 2 * 540);
			uint32_t s = 12345u;
			for (int y = 0; y < 540; ++y)
				for (int x = 0; x < 960; ++x)
				{
					s = s * 1664525u + 1013904223u;
					packed[(size_t(y) * 960 + x) * 2] = static_cast<uint8_t>(s >> 24);
					packed[(size_t(y) * 960 + x) * 2 + 1] = static_cast<uint8_t>(luma.at(x, y));
				}
			const FS::Result r = FS::DecodeWith([&packed](int x, int y) { return static_cast<uint32_t>(packed[(size_t(y) * 960 + x) * 2 + 1]); },
				960, 540, 8, Corner::Source);
			Assert::IsTrue(r.found && r.index == 12345u, L"luma-only decode");
		}

		TEST_METHOD(CrcRejectsOneFlippedDataBit)
		{
			const Frame f = MakeFrame(540, 10, false, Corner::Source, FS::Word(0, 3, 12345u) ^ (1ull << 20));
			Assert::IsFalse(Decode(f, 10, Corner::Source).found);
		}

		TEST_METHOD(LowContrastIsRejected)
		{
			Frame f = MakeFrame(540, 10, false, Corner::Source, FS::Word(0, 3, 12345u));
			for (uint16_t& v : f.p)
				v = static_cast<uint16_t>(std::floor(64 + (v - 64) * 0.2 + 0.5));   // contrast 175 < 256
			Assert::IsFalse(Decode(f, 10, Corner::Source).found);
		}

		TEST_METHOD(ChangedLevelsStillDecode)
		{
			Frame f = MakeFrame(720, 10, false, Corner::Source, FS::Word(0, 3, 12345u));
			for (uint16_t& v : f.p)
				v = static_cast<uint16_t>(std::floor(v * 0.8 + 40 + 0.5));   // tone-mapped style scale + offset
			const FS::Result r = Decode(f, 10, Corner::Source);
			Assert::IsTrue(r.found && r.index == 12345u, L"scaled levels");
		}

		TEST_METHOD(SmallShiftIsFoundBySearchOnly)
		{
			const Frame f = MakeFrame(1080, 10, false, Corner::Source, FS::Word(0, 3, 12345u));
			Frame shifted(f.w, f.h, 64);
			for (int y = 0; y < f.h; ++y)
				for (int x = 0; x < f.w; ++x)
					shifted.at((x + 5) % f.w, (y + 3) % f.h) = f.at(x, y);
			const FS::Result r = Decode(shifted, 10, Corner::Source);
			Assert::IsTrue(r.found && r.index == 12345u, L"search finds the shift");
			Assert::IsTrue(r.offsetX != 0.0 || r.offsetY != 0.0);
			Assert::IsFalse(Decode(shifted, 10, Corner::Source, 0.0).found);
		}

		TEST_METHOD(PlainPictureHasNoStamp)
		{
			const Frame f(960, 540, 400);
			Assert::IsFalse(Decode(f, 10, Corner::Source).found);
		}

		TEST_METHOD(ShaderCpuReferenceEqualsWrite)
		{
			for (int h : { 270, 541, 720, 999, 1080 })
				for (int c = 0; c < 2; ++c)
				{
					const Corner corner = c == 0 ? Corner::Source : Corner::Output;
					const uint64_t word = FS::Word(1, 21, 0xDEADBEEFu);
					const int w = WidthFor(h);
					std::vector<uint8_t> plane(size_t(w) * h, 0);
					FS::Write(plane.data(), w, h, w, 8, true, corner, word);   // full range 8 bit: 0 / 255
					int x0, x1, y0, y1;
					FS::BlockRect(w, h, corner, x0, x1, y0, y1);
					for (int y = 0; y < h; ++y)
						for (int x = 0; x < w; ++x)
						{
							const double ref = FS::CpuReference(x, y, w, h, corner, word);
							const bool inBlock = x >= x0 && x < x1 && y >= y0 && y < y1;
							if (ref < 0.0)
								Assert::IsFalse(inBlock, L"reference says outside, writer painted");
							else
								Assert::IsTrue(plane[size_t(y) * w + x] == (ref == 1.0 ? 255 : 0), L"pixel differs");
						}
				}
		}

		TEST_METHOD(ShaderTextHasTheTwoDynamicUintParams)
		{
			const std::string text = FS::BuildShaderText();
			Assert::IsTrue(text.find("//!PARAM stamp_hi") != std::string::npos);
			Assert::IsTrue(text.find("//!PARAM stamp_lo") != std::string::npos);
			Assert::IsTrue(text.find("//!TYPE DYNAMIC uint") != std::string::npos);
			Assert::IsTrue(text.find("//!HOOK OUTPUT") != std::string::npos);
		}

		TEST_METHOD(OffMeansNoHookText)
		{
			Assert::IsTrue(FS::HookTextIfEnabled(false).empty());
			Assert::IsTrue(FS::HookTextIfEnabled(true) == FS::BuildShaderText());
		}

		TEST_METHOD(DebugSwitchIsTheEnvironmentVariableEqualToOne)
		{
			char* old = nullptr;
			size_t len = 0;
			_dupenv_s(&old, &len, "VP_DEBUG_OUTPUT_STAMP");
			const std::string saved = old ? old : "";
			const bool had = old != nullptr;
			if (old) std::free(old);
			_putenv_s("VP_DEBUG_OUTPUT_STAMP", "");   // removes it
			Assert::IsFalse(FS::OutputStampEnabled());
			_putenv_s("VP_DEBUG_OUTPUT_STAMP", "0");
			Assert::IsFalse(FS::OutputStampEnabled());
			_putenv_s("VP_DEBUG_OUTPUT_STAMP", "1");
			Assert::IsTrue(FS::OutputStampEnabled());
			_putenv_s("VP_DEBUG_OUTPUT_STAMP", had ? saved.c_str() : "");
		}

		// ---- OutputHookOrder::Kind::DebugStamp -------------------------------------------------------

		TEST_METHOD(DebugStampSortsAfterTheQuantiser)
		{
			using OutputHookOrder::Kind;
			using T = OutputHookOrder::Tagged<Hook>;
			std::vector<T> tagged = { { { 3 }, Kind::DebugStamp }, { { 2 }, Kind::Quantiser }, { { 1 }, Kind::OutputStage },
				{ { 0 }, Kind::EncodingRepair } };
			const std::vector<Hook> ordered = OutputHookOrder::Order(tagged);
			const int expected[] = { 0, 1, 2, 3 };
			for (size_t i = 0; i < 4; ++i)
				Assert::AreEqual(expected[i], ordered[i].id);
			// Even a quantiser-rank slot (the dither) stays before it.
			Assert::IsTrue(OutputHookOrder::Rank(T{ { 0 }, Kind::DebugStamp }) > OutputHookOrder::Rank(T{ { 0 }, Kind::Slot, PipelineSlots::Id::OutputDither }));
		}

		TEST_METHOD(NoOtherKindsRankChanged)
		{
			using OutputHookOrder::Kind;
			using T = OutputHookOrder::Tagged<Hook>;
			const auto rank = [](Kind k) { return OutputHookOrder::Rank(T{ { 0 }, k }); };
			Assert::AreEqual(0, rank(Kind::Other));
			Assert::AreEqual(100, rank(Kind::EncodingRepair));
			Assert::AreEqual(245, rank(Kind::UserPreOutput));
			Assert::AreEqual(248, rank(Kind::DejudderCache));
			Assert::AreEqual(300, rank(Kind::OutputStage));
			Assert::AreEqual(400, rank(Kind::Quantiser));
			Assert::AreEqual(500, rank(Kind::DebugStamp));
			Assert::AreEqual(247, OutputHookOrder::Rank(T{ { 0 }, Kind::Slot, PipelineSlots::Id::OutputCalibrationLut }));
			Assert::AreEqual(400, OutputHookOrder::Rank(T{ { 0 }, Kind::Slot, PipelineSlots::Id::OutputDither }));
		}

		TEST_METHOD(OrderWithoutDebugStampIsUnchanged)
		{
			using OutputHookOrder::Kind;
			using T = OutputHookOrder::Tagged<Hook>;
			// The same list and expectation as PipelineSkeletonTests.OutputHooksTakeTheCorrectedOrder.
			std::vector<T> tagged = {
				{ { 9 }, Kind::Quantiser },
				{ { 1 }, Kind::Slot, PipelineSlots::Id::ColourSdrToneShape },
				{ { 5 }, Kind::Slot, PipelineSlots::Id::OutputCalibrationLut },
				{ { 3 }, Kind::Slot, PipelineSlots::Id::DriveLedMeasure },
				{ { 4 }, Kind::Slot, PipelineSlots::Id::DriveLedCompensation },
				{ { 6 }, Kind::UserPreOutput },
				{ { 2 }, Kind::Slot, PipelineSlots::Id::ColourAppearance },
				{ { 7 }, Kind::DejudderCache },
				{ { 41 }, Kind::Slot, PipelineSlots::Id::DriveBlackLevel },
				{ { 0 }, Kind::EncodingRepair },
				{ { 99 }, Kind::Other },
				{ { 8 }, Kind::OutputStage },
			};
			const std::vector<Hook> ordered = OutputHookOrder::Order(tagged);
			const std::vector<int> expected = { 99, 0, 1, 2, 3, 4, 41, 6, 5, 7, 8, 9 };
			Assert::AreEqual(expected.size(), ordered.size());
			for (size_t i = 0; i < expected.size(); ++i)
				Assert::AreEqual(expected[i], ordered[i].id);
			// Adding a DebugStamp anywhere only appends it: the rest keeps exactly that order.
			tagged.insert(tagged.begin() + 3, T{ { 500 }, Kind::DebugStamp });
			const std::vector<Hook> withStamp = OutputHookOrder::Order(tagged);
			Assert::AreEqual(expected.size() + 1, withStamp.size());
			for (size_t i = 0; i < expected.size(); ++i)
				Assert::AreEqual(expected[i], withStamp[i].id);
			Assert::AreEqual(500, withStamp.back().id);
		}
	};
}
