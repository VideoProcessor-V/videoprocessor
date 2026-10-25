/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// The DV tunnel hand-off probe (DolbyVisionTunnelHandoff.h, DV input fixes
// 2026-10-18) on its own: the window comes from the measured rate and counts
// source pictures (drops included), undecided (dark) pictures decide nothing,
// BT.709 8-bit RGB stays with the tunnel's SDR passthrough while 10-bit RGB
// is handed off at any colorimetry, tunnel-like content and CRC-valid headers
// restart the window, and the formatter's static probes classify content as
// its own placement probe does.
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/modules/input/signal/DolbyVisionTunnelHandoff.h>
#include <dv_loopback/DvCaptureContainer.h>
#include <dv_loopback/DvTunnelPacker.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace DvLoopback;

namespace Tests
{
	namespace
	{
		constexpr uint32_t kW = 256, kH = 108;

		void PutWord(std::vector<uint8_t>& bytes, uint32_t x, uint32_t y, uint32_t word)
		{
			uint8_t* at = bytes.data() + (static_cast<size_t>(y) * kW + x) * 4;
			at[0] = static_cast<uint8_t>(word >> 24); at[1] = static_cast<uint8_t>(word >> 16);
			at[2] = static_cast<uint8_t>(word >> 8); at[3] = static_cast<uint8_t>(word);
		}

		std::vector<uint8_t> TenBit(uint32_t n)
		{
			std::vector<uint8_t> bytes(static_cast<size_t>(kW) * kH * 4);
			uint64_t s = 12345 + n;
			for (uint32_t y = 0; y < kH; ++y)
				for (uint32_t x = 0; x < kW; ++x)
				{
					s = s * 6364136223846793005ULL + 1442695040888963407ULL;
					const int noise = static_cast<int>((s >> 59) & 7) - 3;
					const int v = 300 + static_cast<int>(x * 2) + noise;
					PutWord(bytes, x, y, (static_cast<uint32_t>(v + 20) << 20) | (static_cast<uint32_t>(v) << 10) |
						static_cast<uint32_t>(v - 30));
				}
			return bytes;
		}

		std::vector<uint8_t> EightBit(bool high8, uint8_t level = 0)
		{
			std::vector<uint8_t> rgb(static_cast<size_t>(kW) * kH * 3);
			for (size_t p = 0; p < static_cast<size_t>(kW) * kH; ++p)
			{
				const uint8_t v = level ? level : static_cast<uint8_t>(40 + ((p % kW) * 120) / kW);
				rgb[p * 3] = static_cast<uint8_t>(v + 10); rgb[p * 3 + 1] = v; rgb[p * 3 + 2] = static_cast<uint8_t>(v - 5);
			}
			ContainerOptions o;
			o.high8 = high8;
			std::vector<uint8_t> bytes;
			WrapRgb8(rgb, kW, kH, o, bytes);
			return bytes;
		}

		std::vector<uint8_t> Tunnel(bool high8)
		{
			IptPicture p;
			p.Resize(kW, kH);
			for (uint32_t y = 0; y < kH; ++y)
				for (uint32_t x = 0; x < kW; ++x)
				{
					const size_t i = static_cast<size_t>(y) * kW + x;
					p.i[i] = static_cast<uint16_t>(200 + (x * 3000) / kW + y * 7);
					p.Chroma(i) = static_cast<uint16_t>(2048 + static_cast<int>((x + y) % 200) - 100);
				}
			DmFields dm = DmFields::Ipt();
			dm.sourceMinPq = 7;
			dm.sourceMaxPq = 3079;
			dm.ext.push_back(Ext::L1(0, 2081, 819));
			PacketHeader header;
			std::vector<uint8_t> rgb;
			Assert::IsTrue(BuildTunnelRgb8(p, dm, header, rgb, nullptr));
			ContainerOptions o;
			o.high8 = high8;
			std::vector<uint8_t> bytes;
			WrapRgb8(rgb, kW, kH, o, bytes);
			return bytes;
		}

		CR210DolbyVisionTunnelFormatter::RasterView View(const std::vector<uint8_t>& bytes)
		{
			CR210DolbyVisionTunnelFormatter::RasterView v;
			v.data = bytes.data();
			v.width = kW;
			v.height = kH;
			v.stride = kW * 4;
			v.encoding = VideoFrameEncoding::R210;
			return v;
		}

		DolbyVisionTunnelHandoffSignal Signal(ColorSpace colorspace, double rate = 24000.0 / 1001.0)
		{
			DolbyVisionTunnelHandoffSignal s;
			s.colorspace = colorspace;
			s.frameRate = rate;
			return s;
		}

		// Feeds the same picture with the given counter step; returns the
		// counter of the picture that triggered the hand-off (0 = none).
		uint64_t RunUntilHandoff(DolbyVisionTunnelHandoffProbe& probe, const std::vector<uint8_t>& bytes,
			uint32_t pictures, uint64_t step = 1, uint64_t first = 1)
		{
			for (uint32_t k = 0; k < pictures; ++k)
			{
				const uint64_t counter = first + k * step;
				if (probe.OnFrame(View(bytes), counter) == DolbyVisionTunnelHandoffEvent::HandOff)
					return counter;
			}
			return 0;
		}
	}

	TEST_CLASS(DvTunnelHandoffTests)
	{
	public:
		TEST_METHOD(StaticProbesClassifyAsTheFormatterDoes)
		{
			using C = CR210DolbyVisionTunnelFormatter;
			Assert::IsTrue(C::ClassifyRgbContent(View(TenBit(0))) == C::RgbContent::TEN_BIT);
			Assert::IsTrue(C::ClassifyRgbContent(View(EightBit(true))) == C::RgbContent::HIGH8);
			Assert::IsTrue(C::ClassifyRgbContent(View(EightBit(false))) == C::RgbContent::LOW8);
			Assert::IsTrue(C::ClassifyRgbContent(View(std::vector<uint8_t>(kW * kH * 4, 0))) == C::RgbContent::UNKNOWN, L"black decides nothing");
			for (const bool high8 : { false, true })
			{
				bool foundHigh = !high8;
				Assert::IsTrue(C::HasTunnelHeader(View(Tunnel(high8)), &foundHigh));
				Assert::AreEqual(high8, foundHigh, L"the placement the header was found in");
				Assert::IsTrue(C::LooksLikeTunnelContent(View(Tunnel(high8))));
			}
			Assert::IsFalse(C::HasTunnelHeader(View(TenBit(0))));
			Assert::IsFalse(C::HasTunnelHeader(View(EightBit(true))));
			Assert::IsFalse(C::LooksLikeTunnelContent(View(TenBit(0))), L"natural 10-bit RGB is not tunnel-like");
			Assert::IsFalse(C::LooksLikeTunnelContent(View(EightBit(false))));
			// Truncation check: only an all-zero last row.
			std::vector<uint8_t> cut = Tunnel(false);
			Assert::IsFalse(C::LastRowBlank(View(cut)));
			std::fill(cut.end() - kW * 4, cut.end(), 0);
			Assert::IsTrue(C::LastRowBlank(View(cut)));
			cut[cut.size() - 1] = 1;   // one plausible sample keeps the frame
			Assert::IsFalse(C::LastRowBlank(View(cut)));
		}

		TEST_METHOD(WindowComesFromTheRateAndCountsSourcePictures)
		{
			DolbyVisionTunnelHandoffProbe probe;
			probe.Reset(Signal(ColorSpace::BT_2020));
			Assert::AreEqual<uint64_t>(24, probe.WindowPictures(), L"about 1 s at 23.976 Hz");
			Assert::AreEqual<uint64_t>(24, RunUntilHandoff(probe, TenBit(0), 100));
			probe.Reset(Signal(ColorSpace::BT_2020, 60000.0 / 1001.0));
			Assert::AreEqual<uint64_t>(60, probe.WindowPictures(), L"about 1 s at 59.94 Hz");
			Assert::AreEqual<uint64_t>(60, RunUntilHandoff(probe, TenBit(0), 100));
			probe.Reset(Signal(ColorSpace::BT_2020, 50.0));
			Assert::AreEqual<uint64_t>(50, probe.WindowPictures());
			// Dropped pictures count: every other picture delivered.
			Assert::AreEqual<uint64_t>(51, RunUntilHandoff(probe, TenBit(0), 100, 2), L"26 analysed pictures span 51 source pictures");
		}

		TEST_METHOD(OnlyNonTunnelSignalsAreHandedOff)
		{
			DolbyVisionTunnelHandoffProbe probe;
			// BT.709 8-bit RGB: the tunnel's SDR passthrough keeps it (menus).
			for (const bool high8 : { false, true })
			{
				probe.Reset(Signal(ColorSpace::REC_709));
				Assert::AreEqual<uint64_t>(0, RunUntilHandoff(probe, EightBit(high8), 200));
			}
			// BT.709 10-bit RGB (SDR from a 10-bit source): handed off (it was black).
			probe.Reset(Signal(ColorSpace::REC_709));
			Assert::AreEqual<uint64_t>(24, RunUntilHandoff(probe, TenBit(0), 100));
			// Dark pictures decide nothing; the window starts at the first decided one.
			probe.Reset(Signal(ColorSpace::BT_2020));
			Assert::AreEqual<uint64_t>(0, RunUntilHandoff(probe, std::vector<uint8_t>(kW * kH * 4, 0), 100));
			Assert::AreEqual<uint64_t>(124, RunUntilHandoff(probe, EightBit(true), 100, 1, 101));
			// A tunnel is never handed off; its headers restart the window.
			for (const bool high8 : { false, true })
			{
				probe.Reset(Signal(ColorSpace::BT_2020));
				Assert::AreEqual<uint64_t>(0, RunUntilHandoff(probe, Tunnel(high8), 100));
			}
			probe.Reset(Signal(ColorSpace::BT_2020));
			Assert::AreEqual<uint64_t>(0, RunUntilHandoff(probe, TenBit(0), 20));
			Assert::AreEqual<uint64_t>(0, RunUntilHandoff(probe, Tunnel(false), 1, 1, 21));
			Assert::AreEqual<uint64_t>(45, RunUntilHandoff(probe, TenBit(0), 100, 1, 22), L"the window restarts after the header");
		}

		TEST_METHOD(HandedOffSignalIsTakenBackOnlyByATunnelHeader)
		{
			DolbyVisionTunnelHandoffProbe probe;
			probe.Reset(Signal(ColorSpace::BT_2020));
			Assert::AreEqual<uint64_t>(24, RunUntilHandoff(probe, TenBit(0), 100));
			Assert::IsTrue(probe.HandedOff());
			// Black, 8-bit RGB, more 10-bit: no change.
			Assert::IsTrue(probe.OnFrame(View(std::vector<uint8_t>(kW * kH * 4, 0)), 30) == DolbyVisionTunnelHandoffEvent::None);
			Assert::IsTrue(probe.OnFrame(View(EightBit(true)), 31) == DolbyVisionTunnelHandoffEvent::None);
			Assert::IsTrue(probe.HandedOff());
			Assert::IsTrue(probe.OnFrame(View(Tunnel(true)), 32) == DolbyVisionTunnelHandoffEvent::TakeBack);
			Assert::IsFalse(probe.HandedOff());
			Assert::AreEqual(std::string("CRC-valid tunnel header after the hand-off"), std::string(probe.Reason()));
		}
	};
}
