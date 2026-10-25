/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// DV tunnel loopback and autotest (2026-10-17), CPU tests. See
// docs/VP-dv-tunnel-autotest.md. Everything here runs VP's real tunnel
// formatter and the renderer's presentation rule on frames that went through
// the simulated loop (VPV packer -> capture-card container -> VP), and checks
// the wire bytes against the independent reference encoder.
#include "pch.h"
#include "CppUnitTest.h"
#include "DvTunnelReferenceEncoder.h"

#include <dv_loopback/DvLoopbackHarness.h>
#include <dv_loopback/DvTunnelPacker.h>
#include <dv_loopback/DvMetadataModel.h>
#include <VP-V/modules/input/signal/DolbyVisionTunnelGate.h>
#include <DolbyVisionTunnelPresentation.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace DvLoopback;

namespace Tests
{
	namespace
	{
		std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }

		void Note(const std::string& s) { Logger::WriteMessage((s + "\n").c_str()); }

		// The reference DM for the pattern's scene (the same numbers as
		// PatternSequence::SceneDm, restated through the reference's own field
		// table - values only, no layout shared).
		DvReference::Dm ReferenceSceneDm(uint32_t scene, bool rich)
		{
			DvReference::Dm dm = DvReference::Dm::Ipt();
			const unsigned s = scene;
			dm.sourceMinPq = 7 + (s % 5);
			dm.sourceMaxPq = 3079 - (s % 3) * 300;
			dm.AddL1(s % 7, 1500 + (s * 211) % 2000, 600 + (s * 97) % 800);
			if (rich)
			{
				dm.AddL2(2851, 2048 + (s % 9) * 16, 2048, 2048 - (s % 4) * 32, 2048, 2048, 2048);
				dm.AddL2(3079, 2048, 2040 + s % 16, 2048, 2048, 2048, 2048);
				dm.AddL5(0, 0, (s % 3) * 140, (s % 3) * 140);
				dm.AddL6(1000, 1, 1000 - (s % 4) * 100, 400);
				dm.AddL8(1 + s % 3, 2048, 2048, 2048, 2048, 2048 + s % 32, 2048, true);
				dm.AddL11(1 + s % 4, 0, (s & 1) != 0);
			}
			return dm;
		}

		// The independent reference wire picture for a pattern frame.
		bool ReferenceRgb8(const PatternSequence& sequence, const LoopFrame& f, std::vector<uint8_t>& rgb)
		{
			const SequenceConfig& c = sequence.Config();
			if (f.sdr)
			{
				rgb = f.rgb8;   // SDR frames carry no tunnel structure
				return true;
			}
			std::vector<uint16_t> ct(f.embedded.ct), cp(f.embedded.cp);
			DvReference::Header h;
			h.affected = f.header.affectedId;
			h.current = f.header.currentId;
			h.noMd = f.header.noMetadata;
			h.eos = f.header.eos;
			if (f.header.noMetadata)
				return DvReference::Encode(c.width, c.height, f.embedded.i, ct, cp, nullptr, h, rgb);
			const uint32_t scene = sequence.Scene(f.index) + (f.header.affectedId != f.header.currentId ? 1 : 0);
			const DvReference::Dm dm = ReferenceSceneDm(scene, c.richMetadata);
			return DvReference::Encode(c.width, c.height, f.embedded.i, ct, cp, &dm, h, rgb);
		}

		std::function<std::string(uint32_t, const std::vector<uint8_t>&)> ReferenceCheck(const SequenceConfig& config)
		{
			return [config](uint32_t index, const std::vector<uint8_t>& captured) -> std::string
			{
				const PatternSequence sequence(config);
				const LoopFrame f = sequence.Make(index);
				std::vector<uint8_t> ref;
				if (!ReferenceRgb8(sequence, f, ref))
					return "reference could not encode";
				if (ref.size() != captured.size())
					return "size differs";
				for (size_t k = 0; k < ref.size(); ++k)
					if (ref[k] != captured[k])
					{
						char text[160];
						snprintf(text, sizeof(text), "wire byte %zu (pixel %zu channel %zu): reference %u captured %u",
							k, k / 3, k % 3, ref[k], captured[k]);
						return text;
					}
				return std::string();
			};
		}

		// Runs explicit frames (frames[i].index == i) through the simulated loop.
		LoopReport RunFrames(const std::vector<LoopFrame>& frames, uint32_t width, uint32_t height,
			const LoopFaults& faults, unsigned num = 24000, unsigned den = 1001, AnalyzerOptions options = AnalyzerOptions(),
			uint8_t sequenceId = 0x5A, DvLoopbackAnalyzer** keep = nullptr)
		{
			options.sequenceId = sequenceId;
			options.expected = [&frames](uint32_t index, LoopFrame& f) {
				if (index >= frames.size()) return false;
				f = frames[index];
				return true; };
			const double period = static_cast<double>(kVirtualTicksPerSecond) * den / num;
			options.sentQpc = [period](uint32_t index) { return static_cast<int64_t>(std::llround(period * index)) + 1; };
			options.qpcFrequency = kVirtualTicksPerSecond;
			DvLoopbackAnalyzer analyzer(options);
			DvSimulatedLoop loop(analyzer, faults);
			loop.SetPeriodTicks(static_cast<uint64_t>(std::llround(period)));
			loop.Begin(width, height, num, den);
			for (const LoopFrame& f : frames)
				loop.Send(f, options.sentQpc(f.index));
			loop.End();
			(void)keep;
			return analyzer.Finish();
		}

		IptPicture SmoothPicture(uint32_t w, uint32_t h, uint32_t seed)
		{
			IptPicture p;
			p.Resize(w, h);
			for (uint32_t y = 0; y < h; ++y)
				for (uint32_t x = 0; x < w; ++x)
				{
					const size_t i = static_cast<size_t>(y) * w + x;
					p.i[i] = static_cast<uint16_t>(200 + ((x * 3000) / w + y * 7 + seed * 13) % 3500);
					p.Chroma(i) = static_cast<uint16_t>(2048 + static_cast<int>((x + y + seed) % 200) - 100);
				}
			return p;
		}

		LoopFrame DvFrame(uint32_t index, const IptPicture& picture, const DmFields& dm, const PacketHeader& header,
			uint8_t sequenceId = 0x5A, bool stamp = true)
		{
			LoopFrame f;
			f.index = index;
			IptPicture p = picture;
			if (stamp)
				WriteStamp(p.width, p.height, sequenceId, index, &p.i, nullptr);
			f.hasDm = true;
			f.effectiveDm = dm;
			f.header = header;
			f.sentBody = !header.noMetadata;
			Assert::IsTrue(BuildTunnelRgb8(p, dm, header, f.rgb8, &f.embedded), L"raster too small for the DM");
			return f;
		}

		LoopFrame SdrFrame(uint32_t index, std::vector<uint8_t> rgb, uint32_t w, uint32_t h, uint8_t sequenceId = 0x5A)
		{
			LoopFrame f;
			f.index = index;
			f.sdr = true;
			WriteStamp(w, h, sequenceId, index, nullptr, &rgb);
			f.rgb8 = std::move(rgb);
			return f;
		}

		DmFields RichDm()
		{
			DmFields dm = DmFields::Ipt();
			dm.sourceMinPq = 7;
			dm.sourceMaxPq = 3079;
			dm.ext.push_back(Ext::L1(0, 2081, 819));
			dm.ext.push_back(Ext::L2(2851, 2100, 2000, 2048, 2048, 2048, 2048));
			dm.ext.push_back(Ext::L5(0, 0, 280, 280));
			dm.ext.push_back(Ext::L6(1000, 1, 1000, 400));
			dm.ext.push_back(Ext::L8(1, 2048, 2048, 2048, 2048, 2048, 2048, 1));
			dm.ext.push_back(Ext::L11(1, 0, true));
			return dm;
		}
	}

	TEST_CLASS(DvTunnelLoopbackTests)
	{
	public:
		// The CRC check value of ISO/IEC 13818-1 CRC-32 ("123456789" ->
		// 0x0376E6E7) anchors both encoders' CRC to the published standard.
		TEST_METHOD(CrcMatchesTheStandardCheckValueInBothEncoders)
		{
			const char* text = "123456789";
			const uint32_t packer = Crc32Mpeg2(reinterpret_cast<const uint8_t*>(text), 9);
			std::vector<int> bits;
			for (int k = 0; k < 9; ++k)
				for (int m = 7; m >= 0; --m)
					bits.push_back((text[k] >> m) & 1);
			Assert::AreEqual(0x0376E6E7u, packer, L"VPV packer CRC");
			Assert::AreEqual(0x0376E6E7u, DvReference::Crc(bits), L"reference encoder CRC");
		}

		// ETSI Table 3 with its default values: 71 bytes, the reserved
		// constants in place (reference encoder vs the VPV packer).
		TEST_METHOD(EtsiDefaultDmBodyMatchesTable3InBothEncoders)
		{
			DvReference::Dm ref;
			ref.yccToRgb = { 9575, 0, 14742, 9575, -1754, -4383, 9575, 17372, 0 };
			ref.offsets = { 67108864u, 536870912u, 536870912u };
			ref.rgbToLms = { 5845, 9702, 837, 2568, 12256, 1561, 0, 679, 15705 };
			ref.signalColorSpace = 0;
			const std::vector<uint8_t> body = DvReference::Body(ref);
			Assert::AreEqual(size_t(71), body.size());
			Assert::AreEqual(0xFF, int(body[50]));
			Assert::AreEqual(0xFF, int(body[51]));
			Assert::AreEqual(0x0C, int(body[60]));
			Assert::AreEqual(0x3E, int(body[65]));
			Assert::AreEqual(0x0E, int(body[66]));
			Assert::AreEqual(0x70, int(body[67]));
			Assert::AreEqual(0x2A, int(body[69]));
			DmFields dm;
			const int16_t ycc[9] = { 9575, 0, 14742, 9575, -1754, -4383, 9575, 17372, 0 };
			const int16_t lms[9] = { 5845, 9702, 837, 2568, 12256, 1561, 0, 679, 15705 };
			for (int k = 0; k < 9; ++k) { dm.yccToRgb[k] = ycc[k]; dm.rgbToLms[k] = lms[k]; }
			dm.yccToRgbOffset[0] = 67108864u; dm.yccToRgbOffset[1] = dm.yccToRgbOffset[2] = 536870912u;
			dm.signalColorSpace = 0;
			Assert::IsTrue(body == SerializeDm(dm), L"packer and reference DM bodies differ");
		}

		// Common-mode guard: VP's packer and the independent reference produce
		// the same wire bytes for every frame of a rich sequence (multi-packet
		// DM, advance metadata, no_md, EOS, SDR) and for random pictures/DMs.
		TEST_METHOD(ReferenceEncoderAgreesWithTheVpvPackerByteForByte)
		{
			SequenceConfig config;
			config.width = 320;
			config.height = 108;
			config.sceneFrames = 5;
			config.noMetadataEvery = 3;
			config.sdrStart = 22;
			config.sdrLength = 4;
			const PatternSequence sequence(config);
			for (uint32_t index = 0; index < 40; ++index)
			{
				const LoopFrame f = sequence.Make(index);
				std::vector<uint8_t> ref;
				Assert::IsTrue(ReferenceRgb8(sequence, f, ref));
				Assert::IsTrue(ref == f.rgb8, (L"frame " + std::to_wstring(index) + L": packer and reference differ").c_str());
			}
			// Random pictures and DMs (every 12-bit code, signed extremes, long DMs).
			uint64_t rng = 0xC0FFEEULL;
			auto next = [&rng]() { rng = rng * 6364136223846793005ULL + 1442695040888963407ULL; return static_cast<uint32_t>(rng >> 33); };
			for (int round = 0; round < 12; ++round)
			{
				const uint32_t w = 128 + (round % 4) * 34 * 2, h = 104 + round * 4;
				IptPicture p;
				p.Resize(w, h);
				for (size_t k = 0; k < p.i.size(); ++k) p.i[k] = static_cast<uint16_t>(next() & 4095);
				for (size_t k = 0; k < p.ct.size(); ++k) { p.ct[k] = static_cast<uint16_t>(next() & 4095); p.cp[k] = static_cast<uint16_t>(next() & 4095); }
				DmFields dm = DmFields::Ipt();
				DvReference::Dm rd = DvReference::Dm::Ipt();
				for (int k = 0; k < 9; ++k)
				{
					const int16_t a = static_cast<int16_t>(next()), b = static_cast<int16_t>(next());
					dm.yccToRgb[k] = a; rd.yccToRgb[k] = a;
					dm.rgbToLms[k] = b; rd.rgbToLms[k] = b;
				}
				for (int k = 0; k < 3; ++k) { const uint32_t o = next() * 2u + (next() & 1u); dm.yccToRgbOffset[k] = o; rd.offsets[k] = o; }
				dm.sourceMinPq = rd.sourceMinPq = next() & 4095;
				dm.sourceMaxPq = rd.sourceMaxPq = next() & 4095;
				const int blocks = 1 + round * 2;
				for (int b = 0; b < blocks; ++b)
				{
					const uint16_t v1 = next() & 4095, v2 = next() & 4095, v3 = next() & 4095;
					dm.ext.push_back(Ext::L1(v1, v2, v3));
					rd.AddL1(v1, v2, v3);
					dm.ext.push_back(Ext::L2(v1, v2, v3, v1, v2, v3, static_cast<int16_t>(next())));
					rd.AddL2(v1, v2, v3, v1, v2, v3, static_cast<int16_t>(Be16(dm.ext.back().payload, 12)));
				}
				PacketHeader hdr;
				hdr.currentId = next() & 15;
				hdr.affectedId = (next() & 1) ? hdr.currentId : static_cast<uint8_t>((hdr.currentId + 1) & 15);
				hdr.eos = round == 5;
				std::vector<uint8_t> packed, ref;
				const bool fits = BuildTunnelRgb8(p, dm, hdr, packed);
				DvReference::Header rh{ hdr.affectedId, hdr.currentId, false, hdr.eos };
				const bool refFits = DvReference::Encode(w, h, p.i, p.ct, p.cp, &rd, rh, ref);
				Assert::AreEqual(fits, refFits);
				if (fits)
					Assert::IsTrue(packed == ref, (L"random round " + std::to_wstring(round) + L" differs").c_str());
			}
		}

		// Every container and placement VP accepts, with row padding (width not
		// a multiple of 64): bit-exact pictures, metadata and wire bytes, and
		// the reference check passes on every frame.
		TEST_METHOD(SimulatedLoopIsBitExactForEveryContainerAndPlacement)
		{
			for (const VideoFrameEncoding encoding : { VideoFrameEncoding::R210, VideoFrameEncoding::R10b, VideoFrameEncoding::R10l })
				for (const bool high8 : { false, true })
				{
					HarnessConfig config;
					config.sequence.width = 200;
					config.sequence.height = 108;
					config.sequence.sceneFrames = 6;
					config.frames = 30;
					config.faults.container.encoding = encoding;
					config.faults.container.high8 = high8;
					config.faults.container.r10Padding = 3;
					config.faults.forceTunnelFlag = encoding != VideoFrameEncoding::R210;
					config.analyzer.reference = ReferenceCheck(config.sequence);
					const LoopReport r = RunSimulatedLoop(config);
					const std::wstring name = Wide(std::string(CStringA(ToString(encoding)).GetString())) + (high8 ? L" high8" : L" low8");
					Note(std::string(CStringA(name.c_str()).GetString()) + ": " + r.Summary());
					Assert::IsTrue(r.BitExact(), (name + L": " + Wide(r.Summary())).c_str());
					Assert::AreEqual<uint64_t>(30, r.captured, name.c_str());
					Assert::AreEqual<uint64_t>(30, r.identified, name.c_str());
					Assert::AreEqual<uint64_t>(30, r.pictures, (name + L": every frame shown as a DV picture").c_str());
					Assert::AreEqual<uint64_t>(0, r.dropped);
					Assert::AreEqual<uint64_t>(0, r.repeated);
					Assert::IsTrue(r.Logged("CRC-validated r210 RGB8 alignment"), name.c_str());
				}
		}

		// Latency, capture misses and output repeats are measured from the
		// stamps; the pictures stay bit-exact.
		TEST_METHOD(LoopMeasuresLatencyDropsAndRepeats)
		{
			for (const auto rate : { std::pair<unsigned, unsigned>(24000, 1001), std::pair<unsigned, unsigned>(60, 1) })
			{
				HarnessConfig config;
				config.sequence.width = 192;
				config.sequence.height = 108;
				config.sequence.richMetadata = false;
				config.frames = 48;
				config.rateNumerator = rate.first;
				config.rateDenominator = rate.second;
				config.faults.latencyFrames = 3;
				config.faults.captureMisses = { 10, 11 };
				config.faults.outputRepeats = { 20 };
				const LoopReport r = RunSimulatedLoop(config);
				Note(r.Summary());
				Assert::IsTrue(r.BitExact(), Wide(r.Summary()).c_str());
				Assert::AreEqual<uint64_t>(2, r.dropped, L"two capture misses");
				Assert::AreEqual(10u, r.droppedIndices.at(0));
				Assert::AreEqual(11u, r.droppedIndices.at(1));
				Assert::AreEqual<uint64_t>(1, r.repeated, L"one output repeat");
				// Latency: 3 pictures in flight, measured against the sent time
				// (a repeat delays every later picture by one more period).
				Assert::IsTrue(std::fabs(r.latencyFrames - 3.0) < 1.01, Wide(r.Summary()).c_str());
				Assert::IsTrue(r.latencyMsMin > 0, L"latency measured");
				// The miss is a counter gap, not a decode problem: no black.
				Assert::AreEqual<uint64_t>(0, r.black);
			}
		}

		// A wire bit error in the picture is reported at its exact location and
		// changes exactly one decoded sample; in one metadata copy it is
		// recovered by the next copy; in the same bit of all three copies the
		// header is lost and the picture goes black (never packed pixels).
		TEST_METHOD(WireBitErrorsAreLocatedAndMetadataErrorsFallBackSafely)
		{
			const uint32_t w = 192, h = 108;
			HarnessConfig config;
			config.sequence.width = w;
			config.sequence.height = h;
			config.sequence.richMetadata = false;   // one packet: pixels 0..3071
			config.sequence.noMetadataEvery = 0;
			config.frames = 12;
			const size_t picturePixel = 5000, pictureByte = picturePixel * 3 + 1;   // G = I[11:4]
			config.faults.bitErrors.push_back({ 4, pictureByte, 0x10 });            // I bit 8
			config.faults.bitErrors.push_back({ 6, 17 * 3 + 2, 0x10 });              // copy 0, bit 17: C bit 0
			for (size_t copy = 0; copy < 3; ++copy)                                    // frame 8: all copies, bit 3
				config.faults.bitErrors.push_back({ 8, (copy * 1024 + 3) * 3 + 2, 0x10 });
			const LoopReport r = RunSimulatedLoop(config);
			Note(r.Summary());
			auto frameOf = [&r](uint32_t index) -> const FrameRecord& {
				for (const FrameRecord& f : r.frames)
					if (f.identified && f.sentIndex == index) return f;
				Assert::Fail(L"frame not captured");
				return r.frames.front(); };
			Assert::AreEqual<uint64_t>(1, frameOf(4).transportErrors);
			Assert::AreEqual<uint64_t>(1, frameOf(4).pixelMismatches, L"exactly one decoded sample changes");
			Assert::AreEqual<size_t>(0, frameOf(4).metadataMismatches);
			Assert::IsTrue(frameOf(6).shown == Shown::Picture, L"copy 1 recovers the packet");
			Assert::AreEqual<uint64_t>(1, frameOf(6).pixelMismatches, L"the flipped chroma LSB is a picture change");
			Assert::AreEqual<size_t>(0, frameOf(6).metadataMismatches);
			Assert::IsTrue(frameOf(8).shown == Shown::Black, L"no trusted header: black");
			Assert::IsTrue(frameOf(9).shown == Shown::Picture, L"the next good header restores the picture");
			Assert::AreEqual<uint64_t>(0, r.wrongKindFrames, L"never packed pixels as SDR");
			bool located = false;
			for (const std::string& issue : r.issues)
				located |= issue.find("x=" + std::to_string(picturePixel % w) + " y=" + std::to_string(picturePixel / w)) != std::string::npos;
			Assert::IsTrue(located, L"the report names the error location");
		}

		// The common-mode trap: a packer bug the decoder shares (the low
		// nibbles of I and C swapped in B on send and swapped back after
		// decode; parity and CRC are unaffected) passes the loop - pixels,
		// metadata and wire all agree - and only the independent reference
		// catches it.
		TEST_METHOD(CommonModePackerAndDecoderFaultIsCaughtOnlyByTheReference)
		{
			HarnessConfig config;
			config.sequence.width = 192;
			config.sequence.height = 108;
			config.frames = 10;
			config.packerFault = [](LoopFrame& f) {
				if (f.sdr) return;
				for (size_t p = 0; p * 3 < f.rgb8.size(); ++p)
				{
					uint8_t& b = f.rgb8[p * 3 + 2];
					b = static_cast<uint8_t>((b << 4) | (b >> 4));
				}
			};
			config.analyzer.decodedFault = [](std::vector<uint16_t>& planes, uint32_t w, uint32_t h) {
				const size_t n = static_cast<size_t>(w) * h;
				for (size_t p = 0; p < n; ++p)
				{
					const uint16_t i = planes[p], c = planes[n + p];
					planes[p] = static_cast<uint16_t>((i & 0xFF00) | (c & 0x00F0));
					planes[n + p] = static_cast<uint16_t>((c & 0xFF00) | (i & 0x00F0));
				}
			};
			// The loop alone: passes.
			LoopReport loopOnly = RunSimulatedLoop(config);
			Note("loop only: " + loopOnly.Summary());
			Assert::AreEqual<uint64_t>(0, loopOnly.pixelMismatchSamples, L"the shared bug cancels in the loop");
			Assert::AreEqual<uint64_t>(0, loopOnly.metadataMismatchFrames);
			Assert::AreEqual<uint64_t>(0, loopOnly.transportErrorBytes);
			Assert::AreEqual<uint64_t>(10, loopOnly.pictures);
			// With the independent reference: caught on every frame.
			config.analyzer.reference = ReferenceCheck(config.sequence);
			const LoopReport guarded = RunSimulatedLoop(config);
			Note("with reference: " + guarded.Summary());
			Assert::AreEqual<uint64_t>(10, guarded.referenceMismatchFrames);
			Assert::IsFalse(guarded.BitExact());
		}

		// Scenes, advance metadata (affected = current + 1) and no_md pictures:
		// the decoded metadata matches the metadata in force on every frame.
		TEST_METHOD(MetadataSequenceIsTrackedAcrossScenesAdvanceAndNoMetadata)
		{
			HarnessConfig config;
			config.sequence.width = 256;
			config.sequence.height = 108;
			config.sequence.sceneFrames = 4;
			config.sequence.noMetadataEvery = 3;
			config.frames = 64;
			config.analyzer.reference = ReferenceCheck(config.sequence);
			const LoopReport r = RunSimulatedLoop(config);
			Note(r.Summary());
			Assert::IsTrue(r.BitExact(), Wide(r.Summary()).c_str());
			Assert::AreEqual<uint64_t>(64, r.pictures);
			Assert::IsTrue(r.Logged("complete DM bytes="), L"multi-packet DM logged");
			Assert::IsTrue(r.Logged("title DM extension levels (first complete DM)"));
		}

		// DV -> SDR -> DV with EOS: SDR shows at once, DV again with fresh DM.
		// Without EOS: black for the rate-derived window (2 s), then SDR; at 24p
		// and 60p cadence (no fixed frame counts).
		TEST_METHOD(DvSdrDvTransitionsWithAndWithoutEosAt24pAnd60p)
		{
			for (const auto rate : { std::pair<unsigned, unsigned>(24000, 1001), std::pair<unsigned, unsigned>(60000, 1001),
				std::pair<unsigned, unsigned>(24, 1), std::pair<unsigned, unsigned>(60, 1) })
				for (const bool eos : { true, false })
				{
					const double hz = static_cast<double>(rate.first) / rate.second;
					const uint32_t window = static_cast<uint32_t>(std::ceil(hz * 2.0 - 1e-9));
					HarnessConfig config;
					config.sequence.width = 192;
					config.sequence.height = 108;
					config.sequence.richMetadata = false;
					config.sequence.sdrStart = 20;
					config.sequence.sdrLength = window + 30;
					config.sequence.sdrEos = eos;
					config.frames = config.sequence.sdrStart + config.sequence.sdrLength + 20;
					config.rateNumerator = rate.first;
					config.rateDenominator = rate.second;
					const LoopReport r = RunSimulatedLoop(config);
					const std::string name = std::to_string(hz) + (eos ? " Hz with EOS" : " Hz without EOS");
					Note(name + ": " + r.Summary());
					Assert::AreEqual<uint64_t>(0, r.wrongKindFrames, Wide(name).c_str());
					Assert::AreEqual<uint64_t>(0, r.pixelMismatchSamples, Wide(name).c_str());
					Assert::AreEqual<uint64_t>(0, r.metadataMismatchFrames, Wide(name).c_str());
					Assert::AreEqual<uint64_t>(0, r.blackWhereDvExpected, Wide(name + ": DV after SDR shown at once").c_str());
					if (eos)
					{
						Assert::AreEqual<uint64_t>(0, r.blackWhereSdrExpected, Wide(name).c_str());
						Assert::IsTrue(r.Logged("EOS boundary; allowing SDR RGB passthrough"), Wide(name).c_str());
					}
					else
					{
						// Black from the first SDR picture until the window (in
						// source pictures, inclusive) has passed.
						Assert::AreEqual<uint64_t>(window - 1, r.blackWhereSdrExpected, Wide(name).c_str());
						Assert::IsTrue(r.Logged("treating source as SDR"), Wide(name).c_str());
					}
					Assert::AreEqual<uint64_t>(config.sequence.sdrLength - r.blackWhereSdrExpected, r.sdr, Wide(name).c_str());
				}
		}

		// The metadata-loss limit and the SDR window follow the signal's rate.
		TEST_METHOD(TimeLimitsAreDerivedFromTheSignalRate)
		{
			for (const auto rate : { std::pair<unsigned, unsigned>(24000, 1001), std::pair<unsigned, unsigned>(25, 1),
				std::pair<unsigned, unsigned>(50, 1), std::pair<unsigned, unsigned>(60000, 1001), std::pair<unsigned, unsigned>(120, 1) })
			{
				const double hz = static_cast<double>(rate.first) / rate.second;
				const unsigned loss = static_cast<unsigned>((std::max)(1.0, std::floor(hz * 24.0 * 1001.0 / 24000.0 + 0.5)));
				const unsigned long long sdr = static_cast<unsigned long long>((std::max)(2.0, std::ceil(hz * 2.0 - 1e-9)));
				HarnessConfig config;
				config.sequence.width = 192;
				config.sequence.height = 108;
				config.sequence.richMetadata = false;
				config.frames = 2;
				config.rateNumerator = rate.first;
				config.rateDenominator = rate.second;
				const LoopReport r = RunSimulatedLoop(config);
				char want[160];
				snprintf(want, sizeof(want), "loss limit=%u pictures; SDR exit without EOS after %llu header-less", loss, sdr);
				Assert::IsTrue(r.Logged(want), Wide(std::string(want)).c_str());
			}
		}

		// Ordinary 8-bit RGB never locks DV: noise, black, white, gradients,
		// colour bars, graphics/text-like content and mid grey, for 120
		// pictures each, through the tunnel path with the switch on. It is
		// shown as SDR passthrough (black only while the placement is unknown).
		TEST_METHOD(NoFalseDetectionOnOrdinaryRgb)
		{
			const uint32_t w = 256, h = 108;
			struct Content { const char* name; std::function<void(std::vector<uint8_t>&, uint32_t)> fill; };
			const std::vector<Content> contents = {
				{ "noise", [](std::vector<uint8_t>& rgb, uint32_t n) { uint64_t s = 99 + n; for (uint8_t& b : rgb) { s = s * 6364136223846793005ULL + 1; b = static_cast<uint8_t>(s >> 56); } } },
				{ "black", [](std::vector<uint8_t>& rgb, uint32_t) { std::fill(rgb.begin(), rgb.end(), uint8_t(0)); } },
				{ "white", [](std::vector<uint8_t>& rgb, uint32_t) { std::fill(rgb.begin(), rgb.end(), uint8_t(255)); } },
				{ "limited black", [](std::vector<uint8_t>& rgb, uint32_t) { std::fill(rgb.begin(), rgb.end(), uint8_t(16)); } },
				{ "gradient", [w](std::vector<uint8_t>& rgb, uint32_t n) { for (size_t p = 0; p * 3 < rgb.size(); ++p) { const uint8_t v = static_cast<uint8_t>(((p % w) * 255 / (w - 1) + n) & 255); rgb[p * 3] = v; rgb[p * 3 + 1] = static_cast<uint8_t>(255 - v); rgb[p * 3 + 2] = static_cast<uint8_t>(v / 2); } } },
				{ "colour bars", [w](std::vector<uint8_t>& rgb, uint32_t) { static const uint8_t bars[8][3] = { {235,235,235},{235,235,16},{16,235,235},{16,235,16},{235,16,235},{235,16,16},{16,16,235},{16,16,16} }; for (size_t p = 0; p * 3 < rgb.size(); ++p) { const size_t b = (p % w) * 8 / w; for (int c = 0; c < 3; ++c) rgb[p * 3 + c] = bars[b][c]; } } },
				{ "graphics", [w](std::vector<uint8_t>& rgb, uint32_t n) { for (size_t p = 0; p * 3 < rgb.size(); ++p) { const size_t x = p % w, y = p / w; const bool glyph = ((x / 3 + y / 5 + n) % 7) == 0 || ((x ^ y) & 8); rgb[p * 3] = glyph ? 250 : 20; rgb[p * 3 + 1] = glyph ? 250 : 30; rgb[p * 3 + 2] = glyph ? 250 : 90; } } },
				{ "mid grey", [](std::vector<uint8_t>& rgb, uint32_t n) { std::fill(rgb.begin(), rgb.end(), static_cast<uint8_t>(128 + (n & 1))); } },
			};
			for (const Content& content : contents)
			{
				std::vector<LoopFrame> frames;
				for (uint32_t n = 0; n < 120; ++n)
				{
					std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3);
					content.fill(rgb, n);
					frames.push_back(SdrFrame(n, rgb, w, h));
				}
				for (const bool high8 : { false, true })
				{
					LoopFaults faults;
					faults.container.high8 = high8;
					const LoopReport r = RunFrames(frames, w, h, faults);
					const std::string name = std::string(content.name) + (high8 ? " high8" : " low8");
					Note(name + ": " + r.Summary());
					Assert::AreEqual<uint64_t>(0, r.pictures, Wide(name + ": no DV picture").c_str());
					Assert::IsFalse(r.Logged("CRC-validated"), Wide(name + ": no DV lock").c_str());
					Assert::AreEqual<uint64_t>(0, r.wrongKindFrames, Wide(name).c_str());
				}
			}
		}

		// With the tunnel switch off (or a non-r210 / HDR capture state) the
		// gate stays closed and the frames never reach the tunnel formatter.
		TEST_METHOD(DetectionOffLeavesTheSignalToTheNormalPath)
		{
			DolbyVisionTunnelGateInput in;
			in.tunnelSwitch = true;
			in.libplaceboRenderer = true;
			in.captureStateValid = true;
			in.encoding = VideoFrameEncoding::R210;
			in.eotf = EOTF::SDR;
			Assert::IsTrue(DolbyVisionTunnelGateOpen(in));
			auto closed = [](DolbyVisionTunnelGateInput g) { return !DolbyVisionTunnelGateOpen(g); };
			{ auto g = in; g.tunnelSwitch = false; Assert::IsTrue(closed(g), L"switch off"); }
			{ auto g = in; g.libplaceboRenderer = false; Assert::IsTrue(closed(g), L"other renderer"); }
			{ auto g = in; g.captureStateValid = false; Assert::IsTrue(closed(g), L"invalid state"); }
			{ auto g = in; g.eotf = EOTF::PQ; Assert::IsTrue(closed(g), L"PQ capture (HDR10)"); }
			for (const VideoFrameEncoding e : { VideoFrameEncoding::V210, VideoFrameEncoding::UYVY, VideoFrameEncoding::R12B,
				VideoFrameEncoding::R10b, VideoFrameEncoding::R10l, VideoFrameEncoding::ARGB_8BIT })
			{
				auto g = in; g.encoding = e;
				Assert::IsTrue(closed(g), L"non-r210 container");
			}
			Assert::IsTrue(DolbyVisionTunnelOwnsSignal([] { DolbyVisionTunnelGateInput g; g.profileForcesTunnel = true; return g; }()),
				L"a profile forcing the tunnel owns the signal");
			HarnessConfig config;
			config.sequence.width = 192;
			config.sequence.height = 108;
			config.frames = 8;
			config.faults.tunnelSwitch = false;
			const LoopReport r = RunSimulatedLoop(config);
			Assert::AreEqual<uint64_t>(8, r.tunnelOff);
			Assert::AreEqual<uint64_t>(8, r.identified, L"frames still identified from the wire");
			Assert::AreEqual<uint64_t>(0, r.pictures);
		}

		// Corrupted / partial tunnel frames: a missing continuation packet, a
		// malformed body (L1 too short), a declared length beyond the raster,
		// and pictures whose metadata area is garbage after lock. None replaces
		// the metadata in force or shows packed pixels; each is logged.
		TEST_METHOD(CorruptedAndPartialTunnelFramesFallBackSafely)
		{
			const uint32_t w = 256, h = 108;
			const DmFields good = RichDm();   // two packets
			PacketHeader hdr; hdr.currentId = hdr.affectedId = 3;
			std::vector<LoopFrame> frames;
			uint32_t n = 0;
			for (; n < 4; ++n) frames.push_back(DvFrame(n, SmoothPicture(w, h, n), good, hdr));
			// 4: second packet missing (first packet only embedded).
			{
				DmFields other = good;
				other.sourceMaxPq = 2000;
				LoopFrame f = DvFrame(n, SmoothPicture(w, h, n), good, hdr);
				IptPicture p = SmoothPicture(w, h, n);
				WriteStamp(w, h, 0x5A, n, &p.i, nullptr);
				const std::vector<Packet> packets = Packetize(SerializeDm(other), hdr);
				Assert::IsTrue(packets.size() >= 2);
				EmbedPackets(p, { packets[0] });
				PackRgb8(p, f.rgb8);
				f.embedded = p;
				f.effectiveDm = good;   // the metadata in force must not change
				frames.push_back(f);
				++n;
			}
			// 5: malformed body (L1 block of 5 bytes): DM rejected, previous kept.
			{
				DmFields bad = good;
				bad.ext[0].payload.pop_back();
				LoopFrame f = DvFrame(n, SmoothPicture(w, h, n), bad, hdr);
				f.effectiveDm = good;
				frames.push_back(f);
				++n;
			}
			// 6..(6+limit): garbage in the metadata area (random chroma LSBs).
			const uint32_t lossLimit = 24;
			for (uint32_t k = 0; k < lossLimit + 4; ++k, ++n)
			{
				LoopFrame f = DvFrame(n, SmoothPicture(w, h, n), good, hdr);
				uint64_t s = n * 7919ULL;
				for (size_t p = 0; p < 3072 * 2; ++p)
				{
					s = s * 6364136223846793005ULL + 1;
					f.embedded.Chroma(p) = static_cast<uint16_t>((f.embedded.Chroma(p) & 0xFFE) | ((s >> 60) & 1));
				}
				PackRgb8(f.embedded, f.rgb8);
				frames.push_back(f);
			}
			// Then good frames again.
			for (uint32_t k = 0; k < 3; ++k, ++n) frames.push_back(DvFrame(n, SmoothPicture(w, h, n), good, hdr));
			const LoopReport r = RunFrames(frames, w, h, LoopFaults());
			Note(r.Summary());
			Assert::AreEqual<uint64_t>(0, r.wrongKindFrames, L"never packed pixels as SDR");
			Assert::AreEqual<uint64_t>(0, r.metadataMismatchFrames, L"a damaged DM never replaces the active one");
			auto shownOf = [&r](uint32_t index) {
				for (const FrameRecord& f : r.frames) if (f.identified && f.sentIndex == index) return f.shown;
				return Shown::Rejected; };
			Assert::IsTrue(shownOf(4) == Shown::Picture, L"valid header, incomplete DM: the active DM stays");
			Assert::IsTrue(shownOf(5) == Shown::Picture, L"malformed body: the active DM stays");
			for (uint32_t k = 6; k < 6 + lossLimit + 4; ++k)
				Assert::IsTrue(shownOf(k) == Shown::Black, (L"garbage metadata: black at " + std::to_wstring(k)).c_str());
			Assert::IsTrue(shownOf(n - 1) == Shown::Picture, L"fresh DM restores the picture");
			Assert::IsTrue(r.Logged("missing/invalid continuation packet") || r.Logged("pictures without usable metadata"));
			Assert::IsTrue(r.Logged("pictures without usable metadata"), L"the loss is logged");
			Assert::IsTrue(r.Logged("metadata transport recovered"), L"the recovery is logged");
		}

		// DV input fix 3 (2026-10-18; was FINDING C): a frame cut short (the
		// lower part never written, e.g. a truncated DMA) with intact metadata
		// at the top used to be decoded as a picture: zero IPT is extreme
		// chroma, so it would show coloured garbage. Now it is rejected (the
		// renderer keeps the previous picture) and logged, the CRC-valid
		// metadata it carried is kept, and valid frames - including black and
		// letterboxed tunnel pictures, whose bottom rows are I near 0 with
		// chroma near 2048 - are never thrown away. dv_tunnel_fixes off
		// reproduces the old behaviour.
		TEST_METHOD(TruncatedFrameWithIntactMetadataIsRejectedNotDecoded)
		{
			const uint32_t w = 256, h = 108;
			PacketHeader hdr; hdr.currentId = hdr.affectedId = 1;
			const DmFields dm = RichDm();
			// Black picture with letterbox bars: tunnel black, not zero words.
			IptPicture black;
			black.Resize(w, h);
			for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i)
			{
				black.i[i] = 0;
				black.Chroma(i) = 2048;
			}
			for (const bool fixes : { false, true })
			{
				std::vector<LoopFrame> frames;
				frames.push_back(DvFrame(0, SmoothPicture(w, h, 0), dm, hdr));
				LoopFrame cut = DvFrame(1, SmoothPicture(w, h, 1), dm, hdr);
				for (size_t k = cut.rgb8.size() / 2; k < cut.rgb8.size(); ++k)
					cut.rgb8[k] = 0;
				frames.push_back(cut);
				// A truncated frame cut only in its last row is caught too.
				LoopFrame lastRow = DvFrame(2, SmoothPicture(w, h, 2), dm, hdr);
				for (size_t k = lastRow.rgb8.size() - static_cast<size_t>(w) * 3; k < lastRow.rgb8.size(); ++k)
					lastRow.rgb8[k] = 0;
				frames.push_back(lastRow);
				frames.push_back(DvFrame(3, black, dm, hdr));
				frames.push_back(DvFrame(4, SmoothPicture(w, h, 4), dm, hdr));
				AnalyzerOptions options;
				options.bugHuntFixes = fixes;
				const LoopReport r = RunFrames(frames, w, h, LoopFaults(), 24000, 1001, options);
				Note(std::string("truncated frames, dv_tunnel_fixes ") + (fixes ? "on: " : "off: ") + r.Summary());
				Assert::AreEqual<uint64_t>(5, r.captured);
				if (!fixes)
				{
					Assert::AreEqual<uint64_t>(5, r.pictures, L"fixes off: the truncated frames are decoded (the old behaviour)");
					continue;
				}
				Assert::AreEqual<uint64_t>(2, r.rejected, L"both truncated frames rejected");
				Assert::AreEqual<uint64_t>(3, r.pictures, L"the valid frames (black included) are decoded");
				Assert::AreEqual<uint64_t>(0, r.metadataMismatchFrames);
				Assert::AreEqual<uint64_t>(0, r.pixelMismatchFrames, L"the valid frames are bit-exact");
				Assert::IsTrue(r.Logged("incomplete frame (last row blank"), L"the rejection is logged");
				auto shownOf = [&r](uint32_t index) {
					for (const FrameRecord& f : r.frames) if (f.identified && f.sentIndex == index) return f.shown;
					return Shown::Rejected; };
				Assert::IsTrue(shownOf(3) == Shown::Picture, L"a black tunnel picture is never thrown away");
				Assert::IsTrue(shownOf(4) == Shown::Picture, L"the next frame decodes with the metadata kept");
			}
		}

		// The hand-off never takes a tunnel (DV input fixes 2026-10-18): a
		// healthy tunnel, a tunnel whose metadata is garbage for longer than
		// the hand-off window (black, never packed pixels), and a tunnel a card
		// range-converted (10-bit values, no CRC: tunnel-like content keeps it
		// with the tunnel path) all stay with the tunnel formatter.
		TEST_METHOD(HandoffNeverTakesATunnel)
		{
			const uint32_t w = 256, h = 108;
			HarnessConfig config;
			config.sequence.width = w;
			config.sequence.height = h;
			config.frames = 96;
			config.faults.cardColorspace = ColorSpace::BT_2020;   // even with BT.2020 reported
			LoopReport healthy = RunSimulatedLoop(config);
			Note("healthy tunnel: " + healthy.Summary());
			Assert::AreEqual<uint64_t>(0, healthy.handoffs);
			Assert::AreEqual<uint64_t>(0, healthy.wrongKindFrames);
			Assert::IsTrue(healthy.pictures > 80);

			HarnessConfig converted = config;
			converted.faults.container.rangeConvert = true;
			LoopReport rc = RunSimulatedLoop(converted);
			Note("range-converted tunnel: " + rc.Summary());
			Assert::AreEqual<uint64_t>(0, rc.handoffs, L"a converted tunnel is never handed to the normal path");
			Assert::AreEqual<uint64_t>(0, rc.handedOff);
			Assert::AreEqual<uint64_t>(0, rc.sdr, L"never packed pixels as RGB");

			// Garbage metadata for 48 pictures (twice the window) after lock.
			PacketHeader hdr; hdr.currentId = hdr.affectedId = 3;
			std::vector<LoopFrame> frames;
			uint32_t n = 0;
			for (; n < 4; ++n) frames.push_back(DvFrame(n, SmoothPicture(w, h, n), RichDm(), hdr));
			for (uint32_t k = 0; k < 48; ++k, ++n)
			{
				LoopFrame f = DvFrame(n, SmoothPicture(w, h, n), RichDm(), hdr);
				uint64_t s = n * 7919ULL;
				for (size_t p = 0; p < 3072 * 2; ++p)
				{
					s = s * 6364136223846793005ULL + 1;
					f.embedded.Chroma(p) = static_cast<uint16_t>((f.embedded.Chroma(p) & 0xFFE) | ((s >> 60) & 1));
				}
				PackRgb8(f.embedded, f.rgb8);
				frames.push_back(f);
			}
			for (uint32_t k = 0; k < 3; ++k, ++n) frames.push_back(DvFrame(n, SmoothPicture(w, h, n), RichDm(), hdr));
			LoopFaults bt2020;
			bt2020.cardColorspace = ColorSpace::BT_2020;
			const LoopReport damaged = RunFrames(frames, w, h, bt2020);
			Note("damaged tunnel: " + damaged.Summary());
			Assert::AreEqual<uint64_t>(0, damaged.handoffs, L"a damaged tunnel stays with the tunnel path (black)");
			Assert::AreEqual<uint64_t>(0, damaged.wrongKindFrames);
		}

		// A tunnel arriving after a hand-off (the source switched from LLDV
		// RGB to the tunnel without a capture-state change) takes the signal
		// back at its first CRC-valid header; the formatter starts fresh and
		// the DV pictures decode bit-exact.
		TEST_METHOD(TunnelTakesAHandedOffSignalBack)
		{
			const uint32_t w = 256, h = 108;
			PacketHeader hdr; hdr.currentId = hdr.affectedId = 2;
			std::vector<LoopFrame> frames;
			uint32_t n = 0;
			for (; n < 40; ++n)
			{
				LoopFrame f;
				f.index = n;
				f.sdr = true;
				f.rgb8.resize(static_cast<size_t>(w) * h * 3);
				for (size_t p = 0; p < static_cast<size_t>(w) * h; ++p)
				{
					const uint8_t v = static_cast<uint8_t>(40 + ((p % w) * 120) / w + (n % 3));
					f.rgb8[p * 3] = static_cast<uint8_t>(v + 10); f.rgb8[p * 3 + 1] = v; f.rgb8[p * 3 + 2] = static_cast<uint8_t>(v - 5);
				}
				WriteStamp(w, h, 0x5A, n, nullptr, &f.rgb8);
				frames.push_back(f);
			}
			for (uint32_t k = 0; k < 20; ++k, ++n)
				frames.push_back(DvFrame(n, SmoothPicture(w, h, n), RichDm(), hdr));
			LoopFaults faults;
			faults.cardColorspace = ColorSpace::BT_2020;
			faults.container.high8 = true;
			const LoopReport r = RunFrames(frames, w, h, faults);
			Note("LLDV RGB then tunnel: " + r.Summary());
			Assert::AreEqual<uint64_t>(1, r.handoffs);
			Assert::AreEqual<uint64_t>(1, r.takeBacks);
			Assert::IsTrue(r.Logged("taken back by the tunnel"));
			// The picture with the first header is held back (never shown through
			// the normal path); the rest decode.
			Assert::AreEqual<uint64_t>(1, r.rejected);
			Assert::AreEqual<uint64_t>(19, r.pictures);
			Assert::AreEqual<uint64_t>(0, r.wrongKindFrames, L"no tunnel picture through the normal path");
			Assert::AreEqual<uint64_t>(0, r.pixelMismatchFrames);
			Assert::AreEqual<uint64_t>(0, r.metadataMismatchFrames);
		}

		// Single-field metadata perturbations: exactly the named decoded field
		// changes, by the expected amount; out-of-range values are rejected
		// (the previous DM stays) or dropped per block, as VP documents.
		TEST_METHOD(MetadataFieldPerturbationsChangeExactlyThatField)
		{
			const uint32_t w = 256, h = 108;
			struct Case
			{
				std::string name;
				std::function<void(DmFields&)> mutate;
				std::set<std::string> keys;   // decoded fields that must change
				bool rejected;                 // the whole DM must be rejected
			};
			auto l = [](size_t block, size_t byte, uint16_t value) {
				return [block, byte, value](DmFields& d) {
					d.ext[block].payload[byte] = static_cast<uint8_t>(value >> 8);
					d.ext[block].payload[byte + 1] = static_cast<uint8_t>(value & 0xFF); }; };
			// RichDm blocks: 0 L1, 1 L2, 2 L5, 3 L6, 4 L8, 5 L11.
			std::vector<Case> cases;
			for (int r = 0; r < 3; ++r)
				for (int c = 0; c < 3; ++c)
				{
					const int k = r * 3 + c;
					for (const int v : { -32768, 32767, 1, -1 })
						cases.push_back({ "YCCtoRGB[" + std::to_string(k) + "]=" + std::to_string(v),
							[k, v](DmFields& d) { d.yccToRgb[k] = static_cast<int16_t>(v); }, { Key("nonlinear", r, c) }, false });
					cases.push_back({ "RGBtoLMS[" + std::to_string(k) + "]+1",
						[k](DmFields& d) { d.rgbToLms[k] = static_cast<int16_t>(d.rgbToLms[k] + 1); }, { Key("linear", r, c) }, false });
				}
			for (int k = 0; k < 3; ++k)
			{
				cases.push_back({ "offset[" + std::to_string(k) + "]=max", [k](DmFields& d) { d.yccToRgbOffset[k] = 0xFFFFFFFFu; }, { Key("nonlinearOffset", k) }, false });
				cases.push_back({ "offset[" + std::to_string(k) + "]+256", [k](DmFields& d) { d.yccToRgbOffset[k] += 256; }, { Key("nonlinearOffset", k) }, false });
			}
			// +1 LSB below float precision at 2^27: VP's float offset cannot show it.
			cases.push_back({ "offset[1]+1 (below float precision)", [](DmFields& d) { d.yccToRgbOffset[1] += 1; }, {}, false });
			cases.push_back({ "full range 0", [](DmFields& d) { d.signalFullRange = 0; }, { "fullRange" }, false });
			cases.push_back({ "source min PQ 0", [](DmFields& d) { d.sourceMinPq = 0; }, { "sourceMinPq" }, false });
			cases.push_back({ "source max PQ 4095", [](DmFields& d) { d.sourceMaxPq = 4095; }, { "sourceMaxPq" }, false });
			cases.push_back({ "source max PQ -1", [](DmFields& d) { d.sourceMaxPq -= 1; }, { "sourceMaxPq" }, false });
			cases.push_back({ "scene refresh 0 (not decoded)", [](DmFields& d) { d.sceneRefresh = 0; }, {}, false });
			cases.push_back({ "colour space YCbCr (0)", [](DmFields& d) { d.signalColorSpace = 0; }, {}, false });
			cases.push_back({ "colour space 1 (reserved)", [](DmFields& d) { d.signalColorSpace = 1; }, {}, true });
			cases.push_back({ "colour space 3 (reserved)", [](DmFields& d) { d.signalColorSpace = 3; }, {}, true });
			cases.push_back({ "bit depth 10", [](DmFields& d) { d.signalBitDepth = 10; }, {}, true });
			const char* l1[] = { "L1.min", "L1.max", "L1.avg" };
			for (int f = 0; f < 3; ++f)
			{
				cases.push_back({ std::string(l1[f]) + "=4095", l(0, f * 2, 4095), { l1[f] }, false });
				cases.push_back({ std::string(l1[f]) + "+1", [f](DmFields& d) { const uint16_t v = Be16(d.ext[0].payload, f * 2); d.ext[0].payload[f * 2] = static_cast<uint8_t>((v + 1) >> 8); d.ext[0].payload[f * 2 + 1] = static_cast<uint8_t>((v + 1) & 0xFF); }, { l1[f] }, false });
				cases.push_back({ std::string(l1[f]) + "=4096", l(0, f * 2, 4096), {}, true });
			}
			cases.push_back({ "L1 length 7", [](DmFields& d) { d.ext[0].payload.push_back(0); }, {}, true });
			const char* l2[] = { "targetMaxPq", "slope", "offset", "power", "chromaWeight", "saturationGain", "msWeight" };
			for (int f = 0; f < 7; ++f)
			{
				cases.push_back({ std::string("L2.") + l2[f] + "=0", l(1, f * 2, 0), { std::string("trim0.") + l2[f] }, false });
				cases.push_back({ std::string("L2.") + l2[f] + "=0xFFFF", l(1, f * 2, 0xFFFF), { std::string("trim0.") + l2[f] }, false });
			}
			// The short L2 is skipped, so the L8 block moves into slot 0 and slot 1
			// empties: slot 0 changes in the fields where the L2 (2851, slope
			// 2100, offset 2000) and the L8 (index 1, 2048s, mid/clip) differ.
			cases.push_back({ "L2 short (13 bytes): skipped", [](DmFields& d) { d.ext[1].payload.pop_back(); },
				{ "trimCount", "trim0.level", "trim0.targetIndex", "trim0.targetMaxPq", "trim0.slope", "trim0.offset",
				  "trim0.hasMidContrastAndClip", "trim0.midContrast", "trim0.clipTrim",
				  "trim1.level", "trim1.targetIndex", "trim1.targetMaxPq", "trim1.slope", "trim1.offset", "trim1.power",
				  "trim1.chromaWeight", "trim1.saturationGain", "trim1.msWeight", "trim1.hasMidContrastAndClip", "trim1.midContrast",
				  "trim1.clipTrim", "trim1.hasSaturationVector", "trim1.hasHueVector" }, false });
			const char* l5[] = { "L5.left", "L5.right", "L5.top", "L5.bottom" };
			for (int f = 0; f < 4; ++f)
			{
				cases.push_back({ std::string(l5[f]) + "=8191", l(2, f * 2, 8191), { l5[f] }, false });
				cases.push_back({ std::string(l5[f]) + "=8192 (block dropped)", l(2, f * 2, 8192),
					{ "L5.present", "L5.top", "L5.bottom" }, false });
			}
			cases.push_back({ "L6 max mastering 10000", l(3, 0, 10000), { "L6.maxMastering" }, false });
			cases.push_back({ "L6 max mastering 10001 (block dropped)", l(3, 0, 10001),
				{ "L6.present", "L6.maxMastering", "L6.minMastering", "L6.contentLight", "L6.maxCll", "L6.maxFall" }, false });
			cases.push_back({ "L6 min mastering 50", l(3, 2, 50), { "L6.minMastering" }, false });
			cases.push_back({ "L6 MaxCLL 0 (content light unknown)", l(3, 4, 0), { "L6.contentLight", "L6.maxCll", "L6.maxFall" }, false });
			cases.push_back({ "L6 MaxFALL > MaxCLL", l(3, 6, 1001), { "L6.contentLight", "L6.maxCll", "L6.maxFall" }, false });
			cases.push_back({ "L6 MaxCLL 1200", l(3, 4, 1200), { "L6.maxCll" }, false });
			cases.push_back({ "L8 target index 7", [](DmFields& d) { d.ext[4].payload[0] = 7; }, { "trim1.targetIndex" }, false });
			const char* l8[] = { "slope", "offset", "power", "chromaWeight", "saturationGain", "msWeight", "midContrast", "clipTrim" };
			for (int f = 0; f < 8; ++f)
				cases.push_back({ std::string("L8.") + l8[f] + "=1234", l(4, 1 + f * 2, 1234), { std::string("trim1.") + l8[f] }, false });
			cases.push_back({ "L11 content type 3", [](DmFields& d) { d.ext[5].payload[0] = 3; }, { "L11.contentType" }, false });
			cases.push_back({ "L11 white point 5", [](DmFields& d) { d.ext[5].payload[1] = static_cast<uint8_t>((d.ext[5].payload[1] & 0x10) | 5); }, { "L11.whitepoint" }, false });
			cases.push_back({ "L11 reference mode off", [](DmFields& d) { d.ext[5].payload[1] &= 0x0F; }, { "L11.referenceMode" }, false });
			cases.push_back({ "unknown level 200 added (skipped)", [](DmFields& d) { d.ext.push_back(Ext::Raw(200, { 1, 2, 3 })); }, {}, false });

			const DmFields base = RichDm();
			bool baseAccepted = false;
			const FlatMetadata baseWant = ExpectedDecode(base, baseAccepted);
			Assert::IsTrue(baseAccepted);
			int checked = 0;
			for (const Case& c : cases)
			{
				DmFields perturbed = base;
				c.mutate(perturbed);
				PacketHeader h0; h0.currentId = h0.affectedId = 4;
				PacketHeader h1; h1.currentId = h1.affectedId = 5;
				std::vector<LoopFrame> frames = { DvFrame(0, SmoothPicture(w, h, 0), base, h0), DvFrame(1, SmoothPicture(w, h, 1), perturbed, h1) };
				// The metadata the decoder must hold after frame 1.
				bool accepted = false;
				const FlatMetadata want = ExpectedDecode(perturbed, accepted);
				frames[1].effectiveDm = accepted ? perturbed : base;
				if (!accepted)
				{
					// A new current id with a rejected body: VP drops the old
					// DM (id changed) and shows black. Keep the id to test that
					// a rejected body leaves the metadata in force untouched.
					frames[1] = DvFrame(1, SmoothPicture(w, h, 1), perturbed, h0);
					frames[1].effectiveDm = base;
				}
				const LoopReport r = RunFrames(frames, w, h, LoopFaults());
				const std::wstring name = Wide(c.name);
				Assert::AreEqual(c.rejected, !accepted, (name + L": model and table agree on rejection").c_str());
				Assert::AreEqual<uint64_t>(0, r.metadataMismatchFrames, (name + L": " + Wide(r.Summary())).c_str());
				Assert::AreEqual<uint64_t>(2, r.pictures, (name + L": both frames shown").c_str());
				// Exactly the named fields changed between the two decodes.
				const std::vector<std::string> diff = DiffMetadata(baseWant, accepted ? want : baseWant);
				std::set<std::string> changed;
				for (const std::string& line : diff)
					changed.insert(line.substr(0, line.find(':')));
				if (changed != c.keys)
				{
					std::string text = c.name + ": changed {";
					for (const std::string& k : changed) text += k + " ";
					text += "} want {";
					for (const std::string& k : c.keys) text += k + " ";
					Assert::Fail(Wide(text + "}").c_str());
				}
				++checked;
			}
			Note("metadata perturbations checked: " + std::to_string(checked));
		}

		// Single-sample and single-bit picture perturbations come back at the
		// same location with the same value; inside the metadata area a luma
		// or chroma change toggles that pixel's chroma LSB exactly when ETSI's
		// scrambling parity changes (computed here from the spec rule).
		TEST_METHOD(PicturePerturbationsComeBackAtTheSameSampleAndValue)
		{
			const uint32_t w = 256, h = 108;
			const DmFields dm = RichDm();
			PacketHeader hdr; hdr.currentId = hdr.affectedId = 2;
			const IptPicture basePicture = SmoothPicture(w, h, 9);
			const LoopFrame baseFrame = DvFrame(0, basePicture, dm, hdr);
			const size_t pixels = static_cast<size_t>(w) * h;
			const size_t metadataPixels = Packetize(SerializeDm(dm), hdr).size() * kPixelsPerPacket;
			struct Change { size_t pixel; bool chroma; uint16_t value; };
			std::vector<Change> changes;
			for (const size_t p : { size_t(0), size_t(1), size_t(w - 1), size_t(w), size_t(777), metadataPixels - 1, metadataPixels,
				metadataPixels + 1, pixels - 64 * 5 - 1, size_t(9000) })
			{
				for (int bit = 0; bit < 12; ++bit)
				{
					changes.push_back({ p, false, static_cast<uint16_t>(basePicture.i[p] ^ (1u << bit)) });
					if (bit > 0 || p >= metadataPixels)
						changes.push_back({ p, true, static_cast<uint16_t>(basePicture.Chroma(p) ^ (1u << bit)) });
				}
				changes.push_back({ p, false, 0 });
				changes.push_back({ p, false, 4095 });
			}
			int checked = 0;
			for (const Change& c : changes)
			{
				IptPicture picture = basePicture;
				if (c.chroma) picture.Chroma(c.pixel) = c.value; else picture.i[c.pixel] = c.value;
				const LoopFrame frame = DvFrame(1, picture, dm, hdr);
				// Expected change set, from the spec rule (not from the packer).
				const bool inMetadata = c.pixel < metadataPixels;
				std::set<std::pair<int, size_t>> want;   // (plane 0 = I / 1 = C, pixel)
				const uint16_t oldI = baseFrame.embedded.i[c.pixel], oldC = baseFrame.embedded.Chroma(c.pixel);
				if (!c.chroma && c.value != basePicture.i[c.pixel]) want.insert({ 0, c.pixel });
				if (c.chroma && ((c.value ^ basePicture.Chroma(c.pixel)) & (inMetadata ? 0xFFEu : 0xFFFu))) want.insert({ 1, c.pixel });
				if (inMetadata)
				{
					const uint16_t newI = c.chroma ? oldI : c.value;
					const uint16_t newCHigh = static_cast<uint16_t>((c.chroma ? c.value : oldC) & 0xFFE);
					const unsigned toggle = Parity12((newI ^ oldI) & 0xFFF) ^ Parity12((newCHigh ^ (oldC & 0xFFE)) & 0xFFE);
					if (toggle) want.insert({ 1, c.pixel });
				}
				// Stamp rows: the stamp overwrites the perturbation there.
				if (c.pixel >= static_cast<size_t>(StampFirstRow(h)) * w && (c.pixel % w) < kStampBits && !c.chroma)
					continue;
				std::vector<LoopFrame> frames = { baseFrame, frame };
				frames[1].index = 1;
				LoopReport r;
				{
					AnalyzerOptions options;
					options.keepFrames = true;
					// Compare frame 1's decode against frame 0's decode directly.
					std::vector<uint16_t> planes0, planes1;
					options.decodedFault = [&planes0, &planes1](std::vector<uint16_t>& planes, uint32_t, uint32_t) {
						(planes0.empty() ? planes0 : planes1) = planes; };
					r = RunFrames(frames, w, h, LoopFaults(), 24000, 1001, options);
					Assert::AreEqual<uint64_t>(2, r.pictures);
					Assert::AreEqual<uint64_t>(0, r.pixelMismatchSamples, Wide(r.Summary()).c_str());
					std::set<std::pair<int, size_t>> got;
					for (size_t k = 0; k < pixels; ++k)
					{
						if (planes0[k] != planes1[k]) got.insert({ 0, k });
						if (planes0[pixels + k] != planes1[pixels + k]) got.insert({ 1, k });
					}
					// The stamp differs between frame 0 and frame 1: ignore it.
					for (auto it = got.begin(); it != got.end();)
					{
						const bool stamp = it->second >= static_cast<size_t>(StampFirstRow(h)) * w && (it->second % w) < kStampBits;
						it = stamp && !want.count(*it) ? got.erase(it) : std::next(it);
					}
					if (got != want)
						Assert::Fail(Wide("pixel " + std::to_string(c.pixel) + (c.chroma ? " chroma" : " luma") + " value " +
							std::to_string(c.value) + ": changed samples differ from the expected set").c_str());
					if (!c.chroma && want.count({ 0, c.pixel }))
						Assert::AreEqual(static_cast<unsigned>(c.value) << 4, static_cast<unsigned>(planes1[c.pixel]), L"same value");
					if (c.chroma && want.count({ 1, c.pixel }) && !inMetadata)
						Assert::AreEqual(static_cast<unsigned>(c.value) << 4, static_cast<unsigned>(planes1[pixels + c.pixel]), L"same value");
				}
				// And the wire bytes still agree with the independent reference.
				std::vector<uint8_t> ref;
				DvReference::Dm rd = DvReference::Dm::Ipt();
				rd.sourceMinPq = 7; rd.sourceMaxPq = 3079;
				rd.AddL1(0, 2081, 819); rd.AddL2(2851, 2100, 2000, 2048, 2048, 2048, 2048); rd.AddL5(0, 0, 280, 280);
				rd.AddL6(1000, 1, 1000, 400); rd.AddL8(1, 2048, 2048, 2048, 2048, 2048, 2048, true); rd.AddL11(1, 0, true);
				IptPicture stamped = picture;
				WriteStamp(w, h, 0x5A, 1, &stamped.i, nullptr);
				Assert::IsTrue(DvReference::Encode(w, h, stamped.i, stamped.ct, stamped.cp, &rd, { 2, 2, false, false }, ref));
				Assert::IsTrue(ref == frame.rgb8, L"reference and packer agree on the perturbed picture");
				++checked;
			}
			Note("picture perturbations checked: " + std::to_string(checked));
		}

		// The renderer's presentation rule, as moved to its own header.
		TEST_METHOD(PresentationRuleMatchesTheRendererContract)
		{
			DolbyVisionTunnelFrameMetadata md;
			md.allowSdrFallback = true;
			md.rgbBitAlignment = 2;
			Assert::IsTrue(DecideDolbyVisionTunnelPresentation(VideoFrameEncoding::R210, md) == DolbyVisionTunnelPresentation::SdrPassthrough);
			Assert::IsTrue(DecideDolbyVisionTunnelPresentation(VideoFrameEncoding::R10b, md) == DolbyVisionTunnelPresentation::Black,
				L"R10b/R10l never take the SDR passthrough");
			md.rgbBitAlignment = 0;
			Assert::IsTrue(DecideDolbyVisionTunnelPresentation(VideoFrameEncoding::R210, md) == DolbyVisionTunnelPresentation::Black,
				L"unknown placement: black");
			md.valid = true;
			md.allowSdrFallback = false;
			Assert::IsTrue(DecideDolbyVisionTunnelPresentation(VideoFrameEncoding::R210, md) == DolbyVisionTunnelPresentation::Picture);
			md.requiresBlackFrame = true;
			Assert::IsTrue(DecideDolbyVisionTunnelPresentation(VideoFrameEncoding::R210, md) == DolbyVisionTunnelPresentation::Black);
		}

		// 1080p and 2160p single pictures, the full-size rasters VP sees: a
		// bit-exact round trip of the every-code pattern (scalar and AVX2).
		TEST_METHOD(FullSizeRastersRoundTripBitExact)
		{
			for (const auto size : { std::pair<uint32_t, uint32_t>(1920, 1080), std::pair<uint32_t, uint32_t>(3840, 2160) })
				for (const bool scalar : { true, false })
				{
					HarnessConfig config;
					config.sequence.width = size.first;
					config.sequence.height = size.second;
					config.frames = 2;
					config.cacheFrames = true;
					config.analyzer.scalarUnpack = scalar;
					config.analyzer.reference = ReferenceCheck(config.sequence);
					const LoopReport r = RunSimulatedLoop(config);
					Note(std::to_string(size.first) + "x" + std::to_string(size.second) + (scalar ? " scalar: " : " auto: ") + r.Summary());
					Assert::IsTrue(r.BitExact(), Wide(r.Summary()).c_str());
					Assert::AreEqual<uint64_t>(2, r.pictures);
				}
		}
	};
}
