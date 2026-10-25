/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// feat/dv-l8 (2026-10-24): L9/L10 parsing, the bounded DM hold (held -> static -> black,
// counted and visible), end to end through the real tunnel formatter with synthetic DMs
// (DvTunnelPacker: L8 with every optional field + L10), and the DV comparison capture's
// metadata lines and clip recording.
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/modules/input/signal/DvSignalLevels.h>
#include <VP-V/modules/input/signal/DvMetadataHold.h>
#include <VP-V/modules/input/signal/DvCompareCapture.h>
#include <VP-V/modules/input/signal/DvTunnelPolicy.h>
#include <VP-V/modules/colour/sdr_tone_shape/DvTrimSelection.h>
#include <dv_loopback/DvLoopbackHarness.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace DvLoopback;

namespace Tests
{
	namespace DvL8Signal
	{
		constexpr uint32_t kW = 1920, kH = 1080;

		inline std::vector<uint8_t> L10Container(uint8_t index, uint16_t maxPq, uint16_t minPq, uint8_t primary)
		{
			std::vector<uint8_t> b;
			b.push_back(index);
			PutBe16(b, maxPq);
			PutBe16(b, minPq);
			b.push_back(primary);
			return b;
		}

		inline std::vector<uint8_t> L10Packed(uint8_t index, uint16_t maxPq, uint16_t minPq, uint8_t primary)
		{
			const uint64_t bits = (static_cast<uint64_t>(index) << 32) | (static_cast<uint64_t>(maxPq & 0xFFF) << 20) |
				(static_cast<uint64_t>(minPq & 0xFFF) << 8) | primary;
			std::vector<uint8_t> b;
			for (int k = 4; k >= 0; --k)
				b.push_back(static_cast<uint8_t>((bits >> (8 * k)) & 0xFF));
			return b;
		}

		// A full-HD tunnel picture with the given DM (r210, low8).
		inline std::vector<uint8_t> TunnelFrame(const DmFields& dm, uint8_t id)
		{
			IptPicture picture;
			picture.Resize(kW, kH);
			for (uint32_t y = 0; y < kH; ++y)
				for (uint32_t x = 0; x < kW; ++x)
				{
					const size_t p = static_cast<size_t>(y) * kW + x;
					picture.i[p] = static_cast<uint16_t>(300 + (x * 7 + y * 3) % 3000);
					picture.Chroma(p) = static_cast<uint16_t>(1500 + (x + 2 * y) % 1100);
				}
			PacketHeader header;
			header.currentId = header.affectedId = static_cast<uint8_t>(id & 15);
			std::vector<uint8_t> rgb;
			Assert::IsTrue(BuildTunnelRgb8(picture, dm, header, rgb, nullptr));
			ContainerOptions o;
			o.encoding = VideoFrameEncoding::R210;
			o.high8 = false;
			o.rowBytes = ((kW + 63) / 64) * 256;
			std::vector<uint8_t> bytes;
			WrapRgb8(rgb, kW, kH, o, bytes);
			return bytes;
		}

		inline VideoStateComPtr State(uint32_t rateNum = 24000, uint32_t rateDen = 1001)
		{
			VideoStateComPtr state = new VideoState();
			state->valid = true;
			state->displayMode = std::make_shared<DisplayMode>(kW, kH, false, rateNum, rateDen);
			state->videoFrameEncoding = VideoFrameEncoding::R210;
			state->dolbyVisionTunnel = true;
			return state;
		}

		inline DmFields GoodDm()
		{
			DmFields dm = DmFields::Ipt();
			dm.sourceMinPq = 62;
			dm.sourceMaxPq = 3079;
			dm.ext.push_back(Ext::L1(0, 3079, 1200));
			dm.ext.push_back(Ext::L2(2081, 2300, 2048, 2048, 2048, 2048, 0));
			const uint8_t sat[6] = { 140, 128, 128, 128, 128, 128 }, hue[6] = { 128, 128, 120, 128, 128, 128 };
			dm.ext.push_back(Ext::L8(1, 2200, 2048, 2048, 2048, 2048, 0, 3, 2100, 2000, sat, hue));
			dm.ext.push_back(Ext::L8(9, 2100, 2048, 2048, 2048, 2048, 0));
			dm.ext.push_back(Ext::Raw(10, L10Container(9, 2851, 62, 2)));   // 600 nits custom target
			dm.ext.push_back(Ext::Raw(9, { 2 }));
			return dm;
		}

		inline bool FileExistsA(const std::string& p)
		{
			const DWORD a = GetFileAttributesA(p.c_str());
			return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
		}
	}

	TEST_CLASS(DvSignalL8HoldTests)
	{
	public:
		// The full-pipeline loose end (DvTunnelPolicy.h): libplacebo's plane deband never runs on the
		// raw tunnel planes; other pictures keep it. (The GPU check of the 2020-green patch is queued.)
		TEST_METHOD(NoPlaneDebandOnTheTunnel)
		{
			Assert::IsTrue(DvTunnelPolicy::SkipPlaneDeband(true));
			Assert::IsFalse(DvTunnelPolicy::SkipPlaneDeband(false));
			// VP's own deband: skipped on the tunnel only when the user has it on (then also a fallback entry).
			Assert::IsTrue(DvTunnelPolicy::SkipVpDeband(true, true));
			Assert::IsFalse(DvTunnelPolicy::SkipVpDeband(true, false));
			Assert::IsFalse(DvTunnelPolicy::SkipVpDeband(false, true));
			Assert::AreEqual(std::string(), DvTunnelPolicy::DebandStatus(false, false));
			Assert::AreEqual(std::string("; libplacebo plane deband off on the tunnel (12-bit IPT; no deband after decode yet)"),
				DvTunnelPolicy::DebandStatus(true, false));
			Assert::AreEqual(std::string("; libplacebo and VP deband off on the tunnel (12-bit IPT; no deband after decode yet)"),
				DvTunnelPolicy::DebandStatus(true, true));
			Assert::AreEqual(std::string("; VP deband off on the tunnel (12-bit IPT; no deband after decode yet)"),
				DvTunnelPolicy::DebandStatus(false, true));
		}

		TEST_METHOD(L10ParsesBothFormsAndRejectsBadBlocks)
		{
			using namespace DvL8Signal;
			DolbyVisionTunnelFrameMetadata md;
			const std::vector<uint8_t> a = L10Container(27, 2851, 62, 2), b = L10Packed(48, 3079, 7, 0);
			Assert::IsTrue(DvSignalLevels::ParseL10(a.data(), a.size(), md));
			Assert::IsTrue(DvSignalLevels::ParseL10(b.data(), b.size(), md));
			Assert::AreEqual<int>(2, md.targetDisplayCount);
			const auto* t27 = DvSignalLevels::FindTarget(md, 27);
			const auto* t48 = DvSignalLevels::FindTarget(md, 48);
			Assert::IsTrue(t27 && t27->maxPq == 2851 && t27->minPq == 62 && t27->primaryIndex == 2);
			Assert::IsTrue(t48 && t48->maxPq == 3079 && t48->minPq == 7 && t48->primaryIndex == 0);
			const std::vector<uint8_t> dup = L10Container(27, 3000, 0, 0), bad = L10Container(5, 100, 200, 0),
				big = L10Container(6, 5000, 0, 0), odd = { 1, 2, 3 };
			Assert::IsFalse(DvSignalLevels::ParseL10(dup.data(), dup.size(), md), L"the first block per index wins");
			Assert::IsFalse(DvSignalLevels::ParseL10(bad.data(), bad.size(), md), L"min above max");
			Assert::IsFalse(DvSignalLevels::ParseL10(big.data(), big.size(), md), L"beyond 12 bits");
			Assert::IsFalse(DvSignalLevels::ParseL10(odd.data(), odd.size(), md), L"unknown length");
			Assert::AreEqual<int>(2, md.targetDisplayCount);
			const uint8_t l9[1] = { 2 };
			Assert::IsTrue(DvSignalLevels::ParseL9(l9, 1, md));
			Assert::IsTrue(md.hasL9 && md.l9SourcePrimaryIndex == 2);
			Assert::IsFalse(DvSignalLevels::ParseL9(l9, 1, md), L"first L9 wins");
		}

		TEST_METHOD(HoldIsATimeAtTheMeasuredRateAndCountsSourcePictures)
		{
			DvMetadataHold::Tracker t;
			t.SetRate(24000.0 / 1001.0, 2.0);
			Assert::AreEqual<uint32_t>(6, t.HoldPictures());
			Assert::AreEqual<uint32_t>(48, t.StaticCapPictures(), L"the static cap: 2 s at the measured rate");
			t.SetRate(60000.0 / 1001.0, 2.0);
			Assert::AreEqual<uint32_t>(15, t.HoldPictures());
			Assert::AreEqual<uint32_t>(120, t.StaticCapPictures());
			t.SetRate(0.0, 2.0);   // no valid rate: nothing changes (no fixed rate in the tracker)
			Assert::AreEqual<uint32_t>(15, t.HoldPictures());
			t.SetRate(24000.0 / 1001.0, 2.0);
			t.Reset();
			using P = DvMetadataHold::Phase;
			Assert::IsTrue(t.OnPicture(true, true, 1) == P::Own);
			Assert::IsTrue(t.OnPicture(false, true, 2) == P::Held);
			Assert::IsTrue(t.OnPicture(false, true, 4) == P::Held, L"a drop counts as a source picture");
			Assert::IsTrue(t.OnPicture(false, true, 7) == P::Held);   // span 6
			Assert::IsTrue(t.OnPicture(false, true, 8) == P::Static);  // span 7
			Assert::IsTrue(t.OnPicture(false, false, 9) == P::Own, L"no DM held: black, not a hold");
			Assert::IsTrue(t.OnPicture(true, true, 10) == P::Own);
			Assert::IsTrue(t.OnPicture(false, true, 11) == P::Held, L"a new run starts a new hold");
			Assert::AreEqual<uint32_t>(6, t.RejectedPictures());
			Assert::AreEqual<uint32_t>(2, t.Runs());
		}

		TEST_METHOD(StaticKeepsTheTitleAndDropsTheShot)
		{
			DolbyVisionTunnelFrameMetadata md;
			md.valid = true;
			md.sourceMaxPq = 3079;
			md.nonlinear[0][0] = 1.0f;
			md.hasSceneMetadata = true;
			md.sceneMaxPq = 3000;
			md.trimCount = 2;
			md.trims[0].level = 2;
			md.hasL6 = true;
			md.targetDisplayCount = 1;
			md.targetDisplays[0].index = 9;
			md.targetDisplays[0].maxPq = 2851;
			DvMetadataHold::StripToStatic(md);
			Assert::IsTrue(md.valid && md.sourceMaxPq == 3079 && md.nonlinear[0][0] == 1.0f && md.hasL6 && md.targetDisplayCount == 1);
			Assert::IsFalse(md.hasSceneMetadata);
			Assert::AreEqual<int>(0, md.trimCount);
			Assert::AreEqual<int>(0, md.trims[0].level);
			std::string reason;
			md.reusedPreviousDm = true;
			md.dmHoldPhase = 2;
			md.dmRejectedPictures = 9;
			Assert::IsTrue(DvMetadataHold::FallbackReason(md, reason));
			Assert::IsTrue(reason.find("static") != std::string::npos);
			Assert::AreEqual(std::string("; DV metadata: 9 rejected pictures (static now)"), DvMetadataHold::StatusSuffix(md));
			md.reusedPreviousDm = false;
			Assert::IsFalse(DvMetadataHold::FallbackReason(md, reason), L"its own DM: no fallback");
			Assert::AreEqual(std::string("; DV metadata: 9 rejected pictures"), DvMetadataHold::StatusSuffix(md));
		}

		// End to end: the formatter parses L8 (all optional fields) and L10, the trim choice is
		// L8; transport damage is held, then static (past the loss limit) up to the cap, then
		// black; an unsupported body (a title change) is held only, then black; no header: black
		// at the loss limit; every rejected picture counted; fresh DM recovers everything.
		TEST_METHOD(FormatterParsesL8AndL10AndHoldsThenStatics)
		{
			using namespace DvL8Signal;
			AnalyzerOptions options;
			options.keepFrames = false;
			options.collectLog = false;
			DvLoopbackAnalyzer analyzer(options);
			analyzer.OnState(State());
			const DmFields good = GoodDm();
			DmFields bad = good;
			bad.signalBitDepth = 14;   // CRC-valid packets, a body VP must reject
			const std::vector<uint8_t> goodBytes = TunnelFrame(good, 0), badBytes = TunnelFrame(bad, 0);
			uint64_t counter = 0;
			auto feed = [&](const std::vector<uint8_t>& bytes)
			{
				++counter;
				analyzer.OnFrame(VideoFrame(bytes.data(), counter, 0, nullptr), static_cast<int64_t>(counter));
				return analyzer.LastMetadata();
			};
			for (int k = 0; k < 3; ++k)
				feed(goodBytes);
			DolbyVisionTunnelFrameMetadata md = analyzer.LastMetadata();
			Assert::IsTrue(analyzer.LastShown() == Shown::Picture);
			Assert::AreEqual<int>(3, md.trimCount, L"one L2 and two L8 blocks");
			Assert::AreEqual<int>(1, md.targetDisplayCount);
			Assert::IsTrue(md.targetDisplays[0].index == 9 && md.targetDisplays[0].maxPq == 2851);
			Assert::IsTrue(md.hasL9 && md.l9SourcePrimaryIndex == 2);
			const auto& l8 = md.trims[1];
			Assert::IsTrue(l8.level == 8 && l8.targetDisplayIndex == 1 && l8.hasMidContrastAndClip && l8.midContrast == 2100 &&
				l8.clipTrim == 2000 && l8.hasSaturationVector && l8.saturationVector[0] == 140 && l8.hasHueVector && l8.hueVector[2] == 120);
			const DvTrimSelection::Selection s = DvTrimSelection::Select(md, 300.0);
			Assert::IsTrue(s.level == DvTrimSelection::Level::L8 && s.apply);
			Assert::IsTrue(s.lowerIndex == 1 && s.upperIndex == 2, L"100-nit (predefined) and 600-nit (L10) L8 trims bracket 300 nits");
			Assert::AreEqual<int>(0, md.dmHoldPhase);
			Assert::AreEqual<uint32_t>(0, md.dmRejectedPictures);
			// Transport damage (the continuation packet lost in all three copies: CRC-valid header,
			// the title's own body damaged on the way): 6 held (0.25 s at 23.976), then static up to
			// the cap (the SDR-exit window, 2 s = 48 source pictures), then black.
			std::vector<uint8_t> damaged = goodBytes;
			const size_t rowBytes = ((kW + 63) / 64) * 256;
			auto at = [&](size_t pixel) { return (pixel / kW) * rowBytes + (pixel % kW) * 4; };
			for (size_t p = 3072; p < 2 * 3072; ++p)
				std::copy(goodBytes.begin() + at(p + 500 * kW), goodBytes.begin() + at(p + 500 * kW) + 4, damaged.begin() + at(p));
			uint32_t rejected = 0;
			for (int k = 1; k <= 6; ++k)
			{
				md = feed(damaged);
				++rejected;
				Assert::IsTrue(analyzer.LastShown() == Shown::Picture, L"held: the picture shows");
				Assert::IsTrue(md.reusedPreviousDm && md.dmHoldPhase == 1 && md.hasSceneMetadata && md.trimCount == 3);
			}
			md = feed(damaged);
			++rejected;
			Assert::IsTrue(analyzer.LastShown() == Shown::Picture, L"static: the picture still shows");
			Assert::IsTrue(md.reusedPreviousDm && md.dmHoldPhase == 2);
			Assert::IsFalse(md.hasSceneMetadata, L"static: no scene L1");
			Assert::AreEqual<int>(0, md.trimCount, L"static: no trims");
			Assert::AreEqual<int>(1, md.targetDisplayCount, L"static: L10 kept");
			Assert::AreEqual(rejected, md.dmRejectedPictures);
			std::string reason;
			Assert::IsTrue(DvMetadataHold::FallbackReason(md, reason));
			Assert::IsFalse(DvTrimSelection::Select(md, 300.0).apply, L"static: trims neutral");
			// Past the ~1 s loss limit the decodable picture stays (static, flagged, counted) ...
			for (int span = 8; span <= 48; ++span)
			{
				md = feed(damaged);
				++rejected;
				Assert::IsTrue(analyzer.LastShown() == Shown::Picture, L"transport damage: static picture up to the cap");
			}
			Assert::IsTrue(md.valid && md.reusedPreviousDm && md.dmHoldPhase == 2 && !md.hasSceneMetadata);
			Assert::AreEqual("; DV metadata: " + std::to_string(rejected) + " rejected pictures (static now)", DvMetadataHold::StatusSuffix(md));
			// ... and the cap ends it: black until fresh DM.
			md = feed(damaged);
			++rejected;
			Assert::IsTrue(analyzer.LastShown() == Shown::Black, L"black past the static-metadata cap");
			Assert::IsFalse(md.valid);
			md = feed(goodBytes);
			Assert::IsTrue(analyzer.LastShown() == Shown::Picture && !md.reusedPreviousDm && md.dmHoldPhase == 0 &&
				md.hasSceneMetadata && md.trimCount == 3);
			Assert::AreEqual(rejected, md.dmRejectedPictures, L"the count stays (since the signal started)");
			// A same-mode title change without EOS to a DM VP cannot decode (signal_bit_depth != 12,
			// other matrices): held for the hold only, then black - never the old title's matrices.
			DmFields other = DmFields::Ipt();
			other.signalBitDepth = 10;
			other.yccToRgb[1] = 1000;
			other.ext.push_back(Ext::L1(0, 2000, 900));
			const std::vector<uint8_t> titleB = TunnelFrame(other, 0);
			for (int k = 1; k <= 6; ++k)
			{
				md = feed(titleB);
				++rejected;
				Assert::IsTrue(analyzer.LastShown() == Shown::Picture && md.dmHoldPhase == 1, L"held briefly");
			}
			md = feed(titleB);
			++rejected;
			Assert::IsTrue(analyzer.LastShown() == Shown::Black, L"unsupported body past the hold: black, not the old matrices");
			for (int k = 0; k < 60; ++k)
			{
				md = feed(titleB);
				++rejected;
				Assert::IsTrue(analyzer.LastShown() == Shown::Black && !md.valid);
			}
			md = feed(goodBytes);
			Assert::IsTrue(analyzer.LastShown() == Shown::Picture && md.hasSceneMetadata && md.trimCount == 3);
			// Pictures with no valid header past the loss limit: black until fresh DM (unchanged).
			std::vector<uint8_t> noHeader = goodBytes;
			std::copy(goodBytes.begin() + 500 * rowBytes, goodBytes.begin() + 503 * rowBytes, noHeader.begin());
			for (int k = 0; k < 30; ++k)
			{
				feed(noHeader);
				++rejected;
			}
			Assert::IsTrue(analyzer.LastShown() == Shown::Black, L"black after the loss limit without a valid header");
			Assert::IsFalse(analyzer.LastMetadata().valid);
			md = feed(goodBytes);
			Assert::IsTrue(analyzer.LastShown() == Shown::Picture && md.hasSceneMetadata && md.trimCount == 3);
			Assert::AreEqual(rejected, md.dmRejectedPictures);
		}

		// The hold off with dv_tunnel_fixes off (the old behaviour, kept as the switch's meaning).
		TEST_METHOD(HoldFollowsTheTunnelFixesSwitch)
		{
			using namespace DvL8Signal;
			AnalyzerOptions options;
			options.keepFrames = false;
			options.collectLog = false;
			options.bugHuntFixes = false;
			DvLoopbackAnalyzer analyzer(options);
			analyzer.OnState(State());
			DmFields bad = GoodDm();
			bad.signalBitDepth = 14;
			const std::vector<uint8_t> goodBytes = TunnelFrame(GoodDm(), 0), badBytes = TunnelFrame(bad, 0);
			uint64_t counter = 0;
			for (int k = 0; k < 3; ++k) { ++counter; analyzer.OnFrame(VideoFrame(goodBytes.data(), counter, 0, nullptr), 0); }
			for (int k = 0; k < 10; ++k) { ++counter; analyzer.OnFrame(VideoFrame(badBytes.data(), counter, 0, nullptr), 0); }
			const auto& md = analyzer.LastMetadata();
			Assert::IsTrue(md.reusedPreviousDm && md.hasSceneMetadata, L"dv_tunnel_fixes off: the previous DM kept whole");
		}

		TEST_METHOD(ComparisonCaptureWritesLabelledMetadataLines)
		{
			using namespace DvL8Signal;
			DolbyVisionTunnelFrameMetadata md;
			md.valid = true;
			md.sourceMinPq = 62;
			md.sourceMaxPq = 3079;
			md.hasSceneMetadata = true;
			md.sceneMinPq = 0; md.sceneMaxPq = 3000; md.sceneAvgPq = 1200;
			md.trimCount = 1;
			md.trims[0].level = 8;
			md.trims[0].targetDisplayIndex = 1;
			md.trims[0].slope = md.trims[0].offset = md.trims[0].power = 2048;
			md.trims[0].chromaWeight = md.trims[0].saturationGain = 2048;
			md.trims[0].slope = 2300;
			const DvTrimSelection::Selection s = DvTrimSelection::Select(md, 100.0);
			DvCompareCapture::FrameInfo info;
			info.counter = 42;
			info.mode = "tunnel";
			info.dm = &md;
			info.trimsEnabled = true;
			info.trims = &s;
			info.targetNits = 100.0;
			const std::string line = DvCompareCapture::FrameJson(info);
			for (const char* want : { "\"counter\":42", "\"mode\":\"tunnel\"", "\"level\":\"L8\"", "\"l1\":[0,3000,1200]",
				"\"target_index\":1", "\"hold_phase\":\"own\"", "\"slope\":" })
				Assert::IsTrue(line.find(want) != std::string::npos, (L"missing " + std::wstring(want, want + strlen(want))).c_str());
			// Balanced braces / brackets (a cheap JSON sanity check; the Python tool parses it).
			int braces = 0, brackets = 0;
			for (char c : line) { braces += c == '{' ? 1 : c == '}' ? -1 : 0; brackets += c == '[' ? 1 : c == ']' ? -1 : 0; }
			Assert::AreEqual(0, braces);
			Assert::AreEqual(0, brackets);

			// A clip: RECORD in the folder starts it; frames + dv-frames.jsonl are written.
			char temp[MAX_PATH];
			GetTempPathA(MAX_PATH, temp);
			const std::string root = std::string(temp) + "vp-dv-compare-test-" + std::to_string(GetCurrentProcessId());
			CreateDirectoryA(root.c_str(), nullptr);
			{ std::ofstream rec(root + "\\RECORD"); rec << "go"; }
			DvCompareCapture::Options o;
			o.root = root;
			o.seconds = 1.0;
			o.minFreeGb = 0.0;
			o.note = "unit test";
			std::string session;
			{
				DvCompareCapture::Recorder recorder(o);
				Assert::IsTrue(recorder.Enabled());
				VideoStateComPtr state = State();
				const std::vector<uint8_t> bytes = TunnelFrame(GoodDm(), 0);
				for (uint64_t c = 1; c <= 40; ++c)
				{
					VideoFrame frame(bytes.data(), c, 0, nullptr);
					recorder.OnRender(*state, &frame, c, &md, true, &s, 100.0);
					if (session.empty() && recorder.Recording())
						session = "x";
				}
				Assert::IsFalse(recorder.Recording(), L"the clip stops by itself after VP_DV_COMPARE_SECONDS");
			}   // the recorder's writer thread drains on destruction
			WIN32_FIND_DATAA fd;
			HANDLE h = FindFirstFileA((root + "\\*-rec").c_str(), &fd);
			Assert::IsTrue(h != INVALID_HANDLE_VALUE, L"a clip session folder");
			const std::string clip = root + "\\" + fd.cFileName;
			FindClose(h);
			std::string first, l;
			size_t frames = 0;
			{
				std::ifstream in(clip + "\\dv-frames.jsonl");
				std::getline(in, first);
				while (std::getline(in, l)) ++frames;
			}
			Assert::IsTrue(first.find("\"type\":\"clip\"") != std::string::npos && first.find("unit test") != std::string::npos);
			Assert::IsTrue(frames >= 20 && frames <= 26, (L"about one second of frame lines: " + std::to_wstring(frames)).c_str());
			Assert::IsTrue(FileExistsA(clip + "\\seg01\\capture.txt"));
			Assert::IsTrue(FileExistsA(clip + "\\seg01\\frame_000001.bin"));
			Logger::WriteMessage(("DV compare clip: " + clip + ", " + std::to_string(frames) + " metadata lines\n").c_str());
			// Clean up the scratch (synthetic pixels only).
			std::string cmd = "cmd /c rd /s /q \"" + root + "\"";
			system(cmd.c_str());
		}
	};
}
