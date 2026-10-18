/*
 * Copyright(C) 2026 Bill Slack
 * SPDX-License-Identifier: GPL-3.0-only
 */
// Every incoming HDR mode the DV harness covers (2026-10-17): the DV tunnel
// (TV-led), LLDV (player-led: PQ BT.2020 10/12-bit, tone-mapped by the
// source, no HDR InfoFrame - a Dolby VSIF instead, which capture cards do not
// report), HDR10 with and without its InfoFrame (the Integral 2 case), and
// SDR. The tests drive VP's own pieces - the tunnel gate
// (DolbyVisionTunnelGate.h), the newlldv inference input and tracker
// (HdrInferenceSignalBuilder.h, HdrInference), the LLDV static metadata
// resolution (LldvStaticMetadata.h) and the tunnel formatter through the
// loop analyzer - in the order VideoProcessorDlg applies them.
#include "pch.h"
#include "CppUnitTest.h"

#include <dv_loopback/DvLoopbackHarness.h>
#include <DolbyVisionTunnelGate.h>
#include <HdrInferenceSignalBuilder.h>
#include <LldvStaticMetadata.h>

#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace DvLoopback;

namespace Tests
{
	namespace
	{
		struct IncomingSignal
		{
			const char* name;
			VideoFrameEncoding encoding;
			EOTF eotf;
			ColorSpace colorspace;
			bool infoFrame;
			BitDepth wireDepth;
		};

		// How each mode reaches VP from a DeckLink card (what the card reports).
		const IncomingSignal kTunnel{ "DV tunnel (RGB8 in r210)", VideoFrameEncoding::R210, EOTF::SDR, ColorSpace::REC_709, false, BitDepth::BITDEPTH_8BIT };
		const IncomingSignal kLldv{ "LLDV 12-bit 4:2:2", VideoFrameEncoding::V210, EOTF::SDR, ColorSpace::BT_2020, false, BitDepth::BITDEPTH_12BIT };
		const IncomingSignal kHdr10{ "HDR10 with InfoFrame", VideoFrameEncoding::V210, EOTF::PQ, ColorSpace::BT_2020, true, BitDepth::BITDEPTH_10BIT };
		const IncomingSignal kHdr10NoInfoFrame{ "HDR10 without InfoFrame", VideoFrameEncoding::V210, EOTF::SDR, ColorSpace::BT_2020, false, BitDepth::BITDEPTH_10BIT };
		const IncomingSignal kSdr{ "SDR BT.709", VideoFrameEncoding::V210, EOTF::SDR, ColorSpace::REC_709, false, BitDepth::BITDEPTH_10BIT };

		VideoStateComPtr StateOf(const IncomingSignal& s, uint32_t w = 3840, uint32_t h = 2160, unsigned num = 24000, unsigned den = 1001)
		{
			VideoStateComPtr state = new VideoState();
			state->valid = true;
			state->displayMode = std::make_shared<DisplayMode>(w, h, false, num, den);
			state->videoFrameEncoding = s.encoding;
			state->eotf = s.eotf;
			state->colorspace = s.colorspace;
			if (s.infoFrame)
			{
				state->hdrData = std::make_shared<HDRData>();
				state->hdrData->masteringDisplayMaxLuminance = 1000;
				state->hdrData->maxCll = 1000;
			}
			return state;
		}

		DolbyVisionTunnelGateInput Gate(bool tunnelSwitch)
		{
			DolbyVisionTunnelGateInput g;
			g.tunnelSwitch = tunnelSwitch;
			g.libplaceboRenderer = true;
			return g;
		}

		enum class Mode { Tunnel, InferredPq, Hdr10, Sdr, Waiting };

		const char* ModeName(Mode m)
		{
			switch (m)
			{
			case Mode::Tunnel: return "DV tunnel";
			case Mode::InferredPq: return "PQ inferred (LLDV / HDR10 without InfoFrame)";
			case Mode::Hdr10: return "HDR10 (InfoFrame)";
			case Mode::Waiting: return "SDR while newlldv stabilises";
			default: return "SDR";
			}
		}

		// VideoProcessorDlg's order: tunnel gate -> HDR InfoFrame -> newlldv.
		struct ModeDriver
		{
			HdrInferenceTracker tracker;
			bool tunnelSwitch = true;
			bool newlldv = true;
			uint64_t nowMs = 0;
			std::vector<std::string> events;

			Mode Apply(const IncomingSignal& s, uint64_t forMs, uint64_t stepMs = 100)
			{
				const VideoStateComPtr state = StateOf(s);
				Mode mode = Mode::Sdr;
				for (uint64_t t = 0; t <= forMs; t += stepMs, nowMs += stepMs)
				{
					const HdrInferenceSignal signal = BuildHdrInferenceSignalFromState(state.p, newlldv, s.wireDepth, Gate(tunnelSwitch), false);
					const HdrInferenceUpdate u = tracker.OnSignal(signal, nowMs);
					if (u.event != HdrInferenceEvent::None)
						events.push_back(std::string(s.name) + ": " + ToString(u.event));
					DolbyVisionTunnelGateInput gate = Gate(tunnelSwitch);
					gate.captureStateValid = true;
					gate.encoding = s.encoding;
					gate.eotf = s.eotf;
					if (DolbyVisionTunnelGateOpen(gate)) mode = Mode::Tunnel;
					else if (s.eotf == EOTF::PQ && s.infoFrame) mode = Mode::Hdr10;
					else if (tracker.IsInferredPq()) mode = Mode::InferredPq;
					else if (tracker.State() == HdrInferenceState::Waiting) mode = Mode::Waiting;
					else mode = Mode::Sdr;
				}
				return mode;
			}
		};
	}

	TEST_CLASS(DvIncomingHdrModeTests)
	{
	public:
		// Each mode on its own, tunnel switch on (the cinema configuration).
		TEST_METHOD(EachIncomingModeIsClassifiedAsVpDoes)
		{
			struct Expect { const IncomingSignal* s; Mode mode; };
			for (const Expect e : { Expect{ &kTunnel, Mode::Tunnel }, Expect{ &kLldv, Mode::InferredPq }, Expect{ &kHdr10, Mode::Hdr10 },
				Expect{ &kHdr10NoInfoFrame, Mode::InferredPq }, Expect{ &kSdr, Mode::Sdr } })
			{
				ModeDriver d;
				const Mode got = d.Apply(*e.s, 3000);
				Logger::WriteMessage((std::string(e.s->name) + " -> " + ModeName(got) + "\n").c_str());
				Assert::AreEqual(static_cast<int>(e.mode), static_cast<int>(got), Wide(e.s->name).c_str());
			}
		}

		// LLDV: PQ is inferred only after the stabilisation time, and the
		// static metadata comes from the active [lldv.*] profile, else VP's
		// newlldv defaults; the command line wins over both.
		TEST_METHOD(LldvIsInferredAfterStabilisationAndGetsTheLldvProfileMetadata)
		{
			ModeDriver d;
			Assert::AreEqual(static_cast<int>(Mode::Waiting), static_cast<int>(d.Apply(kLldv, 1000)), L"still stabilising at 1 s");
			Assert::AreEqual(static_cast<int>(Mode::InferredPq), static_cast<int>(d.Apply(kLldv, 600)), L"inferred after 1.5 s");
			RendererProfileConfig::ResolvedLldv profile;
			profile.profile = "projector";
			profile.hasMaxCll = true; profile.maxCll = 600;
			profile.hasMasteringMaxLuminance = true; profile.masteringMaxLuminance = 650;
			const auto withProfile = ResolveLldvStaticMetadata(&profile, true, LldvCommandLineMetadata());
			Assert::AreEqual(600.0, withProfile.maxCll);
			Assert::AreEqual(650.0, withProfile.masteringMaxLuminance);
			Assert::AreEqual(401.0, withProfile.maxFall, L"unset profile field: newlldv default");
			Assert::AreEqual(0.001, withProfile.masteringMinLuminance);
			const auto defaults = ResolveLldvStaticMetadata(nullptr, true, LldvCommandLineMetadata());
			Assert::AreEqual(1000.0, defaults.maxCll);
			Assert::AreEqual(4000.0, defaults.masteringMaxLuminance);
			const auto legacy = ResolveLldvStaticMetadata(nullptr, false, LldvCommandLineMetadata());
			Assert::AreEqual(1000.0, legacy.masteringMaxLuminance, L"legacy LLDV defaults");
			LldvCommandLineMetadata command;
			command.maxCll = 0;   // 0 is a value for MaxCLL ...
			command.masteringMaxLuminance = 0;   // ... but not for the mastering maximum
			const auto overridden = ResolveLldvStaticMetadata(&profile, true, command);
			Assert::AreEqual(0.0, overridden.maxCll);
			Assert::AreEqual(650.0, overridden.masteringMaxLuminance);
		}

		// The tunnel owns its signal: the inference never fires on it, and
		// HDR10 with an InfoFrame and SDR BT.709 are never inferred either.
		TEST_METHOD(InferenceNeverFiresOnTheTunnelHdr10OrSdr)
		{
			for (const IncomingSignal* s : { &kTunnel, &kHdr10, &kSdr })
			{
				const HdrInferenceSignal signal = BuildHdrInferenceSignalFromState(StateOf(*s).p, true, s->wireDepth, Gate(true), false);
				Assert::IsFalse(EvaluateHdrInferenceEligibility(signal).eligible, Wide(s->name).c_str());
			}
			const HdrInferenceSignal tunnel = BuildHdrInferenceSignalFromState(StateOf(kTunnel).p, true, BitDepth::BITDEPTH_8BIT, Gate(true), false);
			Assert::AreEqual(std::string("Dolby Vision tunnel owns the signal"), std::string(EvaluateHdrInferenceEligibility(tunnel).reason));
		}

		// The tunnel detector does not fire on LLDV / HDR10 / SDR content:
		// the gate keeps those captures away from the tunnel formatter (not
		// r210, or PQ), so no tunnel decode runs at all.
		TEST_METHOD(TunnelDetectorDoesNotFireOnLldvHdr10OrSdrCaptures)
		{
			for (const IncomingSignal* s : { &kLldv, &kHdr10, &kHdr10NoInfoFrame, &kSdr })
			{
				DolbyVisionTunnelGateInput gate = Gate(true);
				gate.captureStateValid = true;
				gate.encoding = s->encoding;
				gate.eotf = s->eotf;
				Assert::IsFalse(DolbyVisionTunnelGateOpen(gate), Wide(s->name).c_str());
			}
			// HDR10 as 10/12-bit RGB: R12B, or r210 reported as PQ: closed too.
			DolbyVisionTunnelGateInput rgbHdr10 = Gate(true);
			rgbHdr10.captureStateValid = true;
			rgbHdr10.encoding = VideoFrameEncoding::R210;
			rgbHdr10.eotf = EOTF::PQ;
			Assert::IsFalse(DolbyVisionTunnelGateOpen(rgbHdr10));
		}

		// Transitions in every direction: LLDV -> tunnel -> HDR10 -> SDR ->
		// LLDV -> HDR10 without InfoFrame -> tunnel -> LLDV. Each change ends
		// or restarts the newlldv epoch as VP's tracker defines; PQ is never
		// inferred while the tunnel owns the signal.
		TEST_METHOD(TransitionsBetweenLldvTunnelHdr10AndSdr)
		{
			ModeDriver d;
			struct Step { const IncomingSignal* s; uint64_t ms; Mode want; };
			const Step steps[] = {
				{ &kLldv, 2000, Mode::InferredPq }, { &kTunnel, 2000, Mode::Tunnel }, { &kHdr10, 2000, Mode::Hdr10 },
				{ &kSdr, 2000, Mode::Sdr }, { &kLldv, 600, Mode::Waiting }, { &kLldv, 1500, Mode::InferredPq },
				{ &kHdr10NoInfoFrame, 300, Mode::InferredPq }, { &kTunnel, 500, Mode::Tunnel }, { &kLldv, 2000, Mode::InferredPq } };
			for (const Step& step : steps)
			{
				const Mode got = d.Apply(*step.s, step.ms);
				Assert::AreEqual(static_cast<int>(step.want), static_cast<int>(got),
					Wide(std::string(step.s->name) + " -> " + ModeName(got)).c_str());
			}
			std::string log;
			for (const std::string& e : d.events) log += e + "\n";
			Logger::WriteMessage(log.c_str());
			// LLDV -> HDR10 without InfoFrame at the same raster: same signal
			// key, so the PQ epoch continues (no flicker through SDR).
			Assert::IsTrue(log.find("HDR10 without InfoFrame: new-epoch") == std::string::npos);
			Assert::IsTrue(log.find("DV tunnel (RGB8 in r210): ended") != std::string::npos, L"tunnel ends the PQ epoch");
		}

		// With the tunnel switch off, the r210 tunnel signal is ordinary SDR
		// RGB to VP (no decode); LLDV is unaffected.
		TEST_METHOD(TunnelSwitchOffLeavesLldvAndTunnelToTheNormalPaths)
		{
			ModeDriver d;
			d.tunnelSwitch = false;
			Assert::AreEqual(static_cast<int>(Mode::Sdr), static_cast<int>(d.Apply(kTunnel, 2000)));
			Assert::AreEqual(static_cast<int>(Mode::InferredPq), static_cast<int>(d.Apply(kLldv, 2000)));
		}

		// FINDING (documented, not changed): LLDV / HDR10 sent as 10-bit RGB
		// 4:4:4 (e.g. an HDFury converting to RGB) is captured as r210 and
		// reported SDR, so with the tunnel switch on it enters the tunnel
		// formatter: the placement probe never resolves on 10-bit content, the
		// picture stays black, and the newlldv inference is suppressed. A
		// wire-bit-depth condition is not a safe fix (an 8-bit tunnel padded to
		// 10 bits arrives "high8" and is valid).
		TEST_METHOD(FindingTenBitRgbLldvWithTheTunnelSwitchOnStaysBlack)
		{
			const uint32_t w = 256, h = 108;
			VideoStateComPtr state = StateOf({ "LLDV 10-bit RGB", VideoFrameEncoding::R210, EOTF::SDR, ColorSpace::BT_2020, false,
				BitDepth::BITDEPTH_10BIT }, w, h);
			DolbyVisionTunnelGateInput gate = Gate(true);
			gate.captureStateValid = true;
			gate.encoding = state->videoFrameEncoding;
			gate.eotf = state->eotf;
			state->dolbyVisionTunnel = DolbyVisionTunnelGateOpen(gate);
			Assert::IsTrue(state->dolbyVisionTunnel, L"the gate opens (r210, SDR)");
			AnalyzerOptions options;
			DvLoopbackAnalyzer analyzer(options);
			analyzer.OnState(state);
			std::vector<uint8_t> bytes(state->BytesPerFrame());
			for (uint32_t n = 0; n < 120; ++n)
			{
				for (uint32_t y = 0; y < h; ++y)
					for (uint32_t x = 0; x < w; ++x)
					{
						// 10-bit PQ-like RGB: every bit of each field in use.
						const uint32_t v = (64 + (x * 3 + y * 5 + n) % 877) & 0x3FF;
						const uint32_t word = (v << 20) | (((v * 7) & 0x3FF) << 10) | ((v * 13 + 1) & 0x3FF);
						uint8_t* at = bytes.data() + static_cast<size_t>(y) * state->BytesPerRow() + x * 4;
						at[0] = static_cast<uint8_t>(word >> 24); at[1] = static_cast<uint8_t>(word >> 16);
						at[2] = static_cast<uint8_t>(word >> 8); at[3] = static_cast<uint8_t>(word);
					}
				VideoFrame frame(bytes.data(), n + 1, n + 1, nullptr, n + 1);
				analyzer.OnFrame(frame, 0);
			}
			const LoopReport r = analyzer.Finish();
			Logger::WriteMessage(("FINDING: 10-bit RGB LLDV with the tunnel switch on: " + r.Summary() + "\n").c_str());
			Assert::AreEqual<uint64_t>(120, r.black, L"black for the whole run (current behaviour)");
			Assert::AreEqual<uint64_t>(0, r.pictures, L"no false DV lock");
			const HdrInferenceSignal signal = BuildHdrInferenceSignalFromState(state.p, true, BitDepth::BITDEPTH_10BIT, Gate(true), false);
			Assert::IsTrue(signal.dolbyVisionTunnel, L"the inference is suppressed (current behaviour)");
		}

		// FINDING (documented, not changed): LLDV sent as 8-bit RGB (some
		// sources dither LLDV to RGB 8-bit) with the tunnel switch on: no
		// false DV lock (no CRC), but VP shows it through the SDR RGB
		// passthrough (BT.709 / BT.1886), and the newlldv inference is
		// suppressed, so a PQ picture is shown as SDR.
		TEST_METHOD(FindingEightBitRgbLldvWithTheTunnelSwitchOnIsShownAsSdr)
		{
			const uint32_t w = 256, h = 108;
			std::vector<LoopFrame> frames;
			for (uint32_t n = 0; n < 60; ++n)
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
			AnalyzerOptions options;
			options.sequenceId = 0x5A;
			options.expected = [&frames](uint32_t i, LoopFrame& f) { if (i >= frames.size()) return false; f = frames[i]; return true; };
			DvLoopbackAnalyzer analyzer(options);
			LoopFaults faults;
			faults.cardColorspace = ColorSpace::BT_2020;
			DvSimulatedLoop loop(analyzer, faults);
			loop.Begin(w, h, 24000, 1001);
			for (const LoopFrame& f : frames)
				loop.Send(f, 0);
			loop.End();
			const LoopReport r = analyzer.Finish();
			Logger::WriteMessage(("FINDING: 8-bit RGB LLDV with the tunnel switch on: " + r.Summary() + "\n").c_str());
			Assert::AreEqual<uint64_t>(0, r.pictures, L"no false DV lock");
			Assert::IsTrue(r.sdr > 50, L"shown through the SDR passthrough (current behaviour)");
			const HdrInferenceSignal signal = BuildHdrInferenceSignalFromState(loop.State().p, true, BitDepth::BITDEPTH_8BIT, Gate(true), false);
			Assert::IsFalse(EvaluateHdrInferenceEligibility(signal).eligible, L"newlldv suppressed");
		}

	private:
		static std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }
	};
}
