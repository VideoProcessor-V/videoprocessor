/*
 * Copyright(C) 2026 VP-V contributors
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
#include <VP-V/modules/input/signal/DolbyVisionTunnelGate.h>
#include <VP-V/modules/input/signal/HdrInferenceSignalBuilder.h>
#include <LldvStaticMetadata.h>

#include <algorithm>
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

		DolbyVisionTunnelGateInput Gate(bool tunnelSwitch, bool profileForcesTunnel = false, bool declineYCbCr = true)
		{
			DolbyVisionTunnelGateInput g;
			g.tunnelSwitch = tunnelSwitch || profileForcesTunnel;
			g.profileForcesTunnel = profileForcesTunnel;
			g.declineYCbCr = declineYCbCr;
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
			bool profileForcesTunnel = false;   // the active profile's DV mode is DV Tunneling
			bool declineYCbCr = true;           // dv_tunnel_handoff
			bool newlldv = true;
			uint64_t nowMs = 0;
			std::vector<std::string> events;

			Mode Apply(const IncomingSignal& s, uint64_t forMs, uint64_t stepMs = 100)
			{
				const VideoStateComPtr state = StateOf(s);
				Mode mode = Mode::Sdr;
				for (uint64_t t = 0; t <= forMs; t += stepMs, nowMs += stepMs)
				{
					const HdrInferenceSignal signal = BuildHdrInferenceSignalFromState(state.p, newlldv, s.wireDepth,
						Gate(tunnelSwitch, profileForcesTunnel, declineYCbCr), false);
					const HdrInferenceUpdate u = tracker.OnSignal(signal, nowMs);
					if (u.event != HdrInferenceEvent::None)
						events.push_back(std::string(s.name) + ": " + ToString(u.event));
					DolbyVisionTunnelGateInput gate = Gate(tunnelSwitch, profileForcesTunnel, declineYCbCr);
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

		// DV input fix 5 (2026-10-18): a profile whose DV mode is DV Tunneling
		// used to own every capture (DolbyVisionTunnelOwnsSignal), so LLDV as
		// YCbCr 4:2:2 (the Apple TV's usual 12-bit LLDV, through the Integral 2)
		// and HDR10 without its InfoFrame were never inferred as PQ while such a
		// profile matched. The tunnel only ever arrives as 8-bit RGB 4:4:4, so a
		// YCbCr capture is declined and goes to the normal LLDV / newlldv path;
		// the tunnel itself (r210 SDR) is still the profile's. dv_tunnel_handoff
		// off reproduces the old behaviour.
		TEST_METHOD(DvTunnelingProfileDeclinesYCbCrLldvAndHdr10)
		{
			const IncomingSignal lldv10{ "LLDV 10-bit 4:2:2", VideoFrameEncoding::V210, EOTF::SDR, ColorSpace::BT_2020, false, BitDepth::BITDEPTH_10BIT };
			const IncomingSignal lldv12{ "LLDV 12-bit 4:2:2", VideoFrameEncoding::V210, EOTF::SDR, ColorSpace::BT_2020, false, BitDepth::BITDEPTH_12BIT };
			const IncomingSignal hdr10x12{ "HDR10 12-bit 4:2:2 with InfoFrame", VideoFrameEncoding::V210, EOTF::PQ, ColorSpace::BT_2020, true, BitDepth::BITDEPTH_12BIT };
			const IncomingSignal hdr10x12NoIf{ "HDR10 12-bit 4:2:2 without InfoFrame", VideoFrameEncoding::V210, EOTF::SDR, ColorSpace::BT_2020, false, BitDepth::BITDEPTH_12BIT };
			const IncomingSignal uyvy{ "LLDV 8-bit UYVY (no newlldv: below 10-bit)", VideoFrameEncoding::UYVY, EOTF::SDR, ColorSpace::BT_2020, false, BitDepth::BITDEPTH_8BIT };
			struct Expect { const IncomingSignal* s; Mode fixed; Mode old; };
			const Expect cases[] = {
				{ &lldv10, Mode::InferredPq, Mode::Sdr }, { &lldv12, Mode::InferredPq, Mode::Sdr },
				{ &kHdr10NoInfoFrame, Mode::InferredPq, Mode::Sdr }, { &hdr10x12NoIf, Mode::InferredPq, Mode::Sdr },
				{ &kHdr10, Mode::Hdr10, Mode::Hdr10 }, { &hdr10x12, Mode::Hdr10, Mode::Hdr10 },
				{ &uyvy, Mode::Sdr, Mode::Sdr }, { &kTunnel, Mode::Tunnel, Mode::Tunnel } };
			for (const bool decline : { true, false })
				for (const Expect& e : cases)
				{
					ModeDriver d;
					d.tunnelSwitch = false;            // the profile alone selects the tunnel
					d.profileForcesTunnel = true;
					d.declineYCbCr = decline;
					const Mode got = d.Apply(*e.s, 3000);
					const Mode want = decline ? e.fixed : e.old;
					Logger::WriteMessage((std::string(decline ? "fixed: " : "dv_tunnel_handoff off: ") + e.s->name + " -> " +
						ModeName(got) + "\n").c_str());
					Assert::AreEqual(static_cast<int>(want), static_cast<int>(got), Wide(std::string(decline ? "fixed: " : "old: ") + e.s->name).c_str());
					// The gate never opens for YCbCr, either way.
					DolbyVisionTunnelGateInput gate = Gate(false, true, decline);
					gate.captureStateValid = true;
					gate.encoding = e.s->encoding;
					gate.eotf = e.s->eotf;
					if (e.s->encoding != VideoFrameEncoding::R210)
						Assert::IsFalse(DolbyVisionTunnelGateOpen(gate));
					const HdrInferenceSignal signal = BuildHdrInferenceSignalFromState(StateOf(*e.s).p, true, e.s->wireDepth,
						Gate(false, true, decline), false);
					Assert::AreEqual(e.s->encoding == VideoFrameEncoding::R210 || !decline, signal.dolbyVisionTunnel,
						Wide(std::string("tunnel owns: ") + e.s->name).c_str());
				}
		}

		// DV input fix 1 (2026-10-18; was FINDING A): LLDV / HDR10 sent as
		// 10-bit RGB 4:4:4 (an HDFury converting LLDV to RGB) is captured as
		// r210 and reported SDR, so with the tunnel switch on it enters the
		// tunnel formatter. It used to stay black for good (the RGB8 placement
		// never resolves on 10-bit content) with newlldv suppressed. Now the
		// hand-off probe sees 10-bit content and no tunnel header for about a
		// second, hands the signal to the normal path, and newlldv infers PQ.
		// dv_tunnel_handoff off reproduces the old behaviour.
		TEST_METHOD(TenBitRgbLldvWithTheTunnelSwitchOnIsHandedOffAndInferredPq)
		{
			const uint32_t w = 256, h = 108;
			const IncomingSignal lldvRgb{ "LLDV 10-bit RGB", VideoFrameEncoding::R210, EOTF::SDR, ColorSpace::BT_2020, false,
				BitDepth::BITDEPTH_10BIT };
			for (const bool handoff : { false, true })
			{
				VideoStateComPtr state = StateOf(lldvRgb, w, h);
				DolbyVisionTunnelGateInput gate = Gate(true);
				gate.captureStateValid = true;
				gate.encoding = state->videoFrameEncoding;
				gate.eotf = state->eotf;
				state->dolbyVisionTunnel = DolbyVisionTunnelGateOpen(gate);
				Assert::IsTrue(state->dolbyVisionTunnel, L"the gate opens (r210, SDR)");
				AnalyzerOptions options;
				options.handoff = handoff;
				DvLoopbackAnalyzer analyzer(options);
				analyzer.OnState(state);
				std::vector<uint8_t> bytes;
				for (uint32_t n = 0; n < 120; ++n)
				{
					TenBitRgbPicture(w, h, n, state->BytesPerRow(), bytes);
					VideoFrame frame(bytes.data(), n + 1, n + 1, nullptr, n + 1);
					analyzer.OnFrame(frame, 0);
				}
				const LoopReport r = analyzer.Finish();
				Logger::WriteMessage((std::string("10-bit RGB LLDV, dv_tunnel_handoff ") + (handoff ? "on: " : "off: ") +
					r.Summary() + "\n").c_str());
				Assert::AreEqual<uint64_t>(0, r.pictures, L"no false DV lock");
				gate.handedOff = analyzer.HandedOff();
				const HdrInferenceSignal signal = BuildHdrInferenceSignalFromState(state.p, true, BitDepth::BITDEPTH_10BIT, gate, false);
				if (!handoff)
				{
					Assert::AreEqual<uint64_t>(120, r.black, L"switch off: black for the whole run (the old behaviour)");
					Assert::IsTrue(signal.dolbyVisionTunnel, L"switch off: the inference stays suppressed");
					continue;
				}
				// About one second (24 pictures at 23.976 Hz) of black, then the normal path.
				const uint64_t window = analyzer.Handoff().WindowPictures();
				Assert::AreEqual<uint64_t>(24, window, L"the window is derived from the rate");
				Assert::AreEqual<uint64_t>(1, r.handoffs);
				Assert::AreEqual<uint64_t>(0, r.takeBacks);
				Assert::AreEqual<uint64_t>(window, r.black, L"black only until the hand-off");
				Assert::AreEqual<uint64_t>(120 - window, r.handedOff, L"the normal path shows the rest");
				Assert::IsTrue(r.Logged("10-bit RGB content"), L"the hand-off is logged with its reason");
				Assert::IsFalse(signal.dolbyVisionTunnel, L"the tunnel no longer owns the signal");
				Assert::IsTrue(EvaluateHdrInferenceEligibility(signal).eligible, L"newlldv may infer PQ");
				gate.captureStateValid = true;
				Assert::IsFalse(DolbyVisionTunnelGateOpen(gate), L"the gate is closed for the handed-off signal");
				// newlldv: PQ after its stabilisation time.
				HdrInferenceTracker tracker;
				for (uint64_t t = 0; t <= 2000; t += 100)
					tracker.OnSignal(signal, t);
				Assert::IsTrue(tracker.IsInferredPq(), L"PQ inferred after 1.5 s");
			}
		}

		// DV input fix 2 (2026-10-18; was FINDING B): LLDV sent as 8-bit RGB
		// with the tunnel switch on used to be shown through the tunnel's SDR
		// passthrough (BT.709 / BT.1886) with newlldv suppressed: PQ shown as
		// SDR. 8-bit LLDV decision (DolbyVisionTunnelHandoff.h): BT.2020 RGB
		// without an HDR InfoFrame and without a tunnel, on a DV input, is
		// player-led DV (LLDV) or HDR10 without its InfoFrame: PQ. It is
		// handed off, and newlldv's 10-bit floor is waived for exactly it.
		// Both RGB8 placements (DeckLink stores 8-bit sources MSB-aligned).
		TEST_METHOD(EightBitRgbLldvWithTheTunnelSwitchOnIsHandedOffAsPq)
		{
			const uint32_t w = 256, h = 108;
			for (const bool high8 : { false, true })
				for (const bool handoff : { false, true })
				{
					std::vector<LoopFrame> frames;
					for (uint32_t n = 0; n < 60; ++n)
					{
						LoopFrame f;
						f.index = n;
						f.sdr = true;
						EightBitRgbPicture(w, h, n, f.rgb8);
						WriteStamp(w, h, 0x5A, n, nullptr, &f.rgb8);
						frames.push_back(f);
					}
					AnalyzerOptions options;
					options.sequenceId = 0x5A;
					options.handoff = handoff;
					options.expected = [&frames](uint32_t i, LoopFrame& f) { if (i >= frames.size()) return false; f = frames[i]; return true; };
					DvLoopbackAnalyzer analyzer(options);
					LoopFaults faults;
					faults.cardColorspace = ColorSpace::BT_2020;
					faults.container.high8 = high8;
					DvSimulatedLoop loop(analyzer, faults);
					loop.Begin(w, h, 24000, 1001);
					for (const LoopFrame& f : frames)
						loop.Send(f, 0);
					loop.End();
					const LoopReport r = analyzer.Finish();
					Logger::WriteMessage((std::string("8-bit RGB LLDV ") + (high8 ? "high8" : "low8") + ", dv_tunnel_handoff " +
						(handoff ? "on: " : "off: ") + r.Summary() + "\n").c_str());
					Assert::AreEqual<uint64_t>(0, r.pictures, L"no false DV lock");
					DolbyVisionTunnelGateInput gate = Gate(true);
					gate.handedOff = analyzer.HandedOff();
					const HdrInferenceSignal signal = BuildHdrInferenceSignalFromState(loop.State().p, true, BitDepth::BITDEPTH_8BIT, gate, false);
					const HdrInferenceEligibility eligibility = EvaluateHdrInferenceEligibility(signal);
					if (!handoff)
					{
						Assert::IsTrue(r.sdr > 50, L"switch off: shown through the SDR passthrough (the old behaviour)");
						Assert::IsFalse(eligibility.eligible, L"switch off: newlldv suppressed");
						continue;
					}
					Assert::AreEqual<uint64_t>(1, r.handoffs);
					Assert::AreEqual<uint64_t>(24, r.sdr, L"SDR passthrough only until the hand-off");
					Assert::AreEqual<uint64_t>(36, r.handedOff);
					Assert::AreEqual<uint64_t>(0, r.wrongKindFrames);
					Assert::IsTrue(r.Logged("BT.2020 RGB without an HDR InfoFrame"));
					Assert::IsTrue(eligibility.eligible, L"8-bit BT.2020 handed off: PQ, never SDR");
					Assert::AreEqual(std::string("BT.2020, 8-bit, no HDR InfoFrame, not a DV tunnel on a DV input (handed off)"),
						std::string(eligibility.reason));
					// The newlldv floor stays for the same 8-bit signal with the tunnel switch off.
					const HdrInferenceSignal off = BuildHdrInferenceSignalFromState(loop.State().p, true, BitDepth::BITDEPTH_8BIT, Gate(false), false);
					Assert::AreEqual(std::string("source is below 10-bit"), std::string(EvaluateHdrInferenceEligibility(off).reason));
				}
		}

		// The explicit indication wins: a Dolby VSIF that says LLDV makes an
		// 8-bit BT.2020 signal PQ with or without the tunnel switch, and the
		// probe hands it off at the first picture; one that says TV-led keeps
		// the tunnel (never handed off).
		TEST_METHOD(DolbyVsifDecidesEightBitLldvExplicitly)
		{
			const uint32_t w = 256, h = 108;
			for (const DolbyVisionVsif vsif : { DolbyVisionVsif::LowLatency, DolbyVisionVsif::TvLed })
			{
				AnalyzerOptions options;
				DvLoopbackAnalyzer analyzer(options);
				LoopFaults faults;
				faults.cardColorspace = ColorSpace::BT_2020;
				faults.cardVsif = vsif;
				DvSimulatedLoop loop(analyzer, faults);
				loop.Begin(w, h, 24000, 1001);
				for (uint32_t n = 0; n < 30; ++n)
				{
					LoopFrame f;
					f.index = n;
					f.sdr = true;
					EightBitRgbPicture(w, h, n, f.rgb8);
					loop.Send(f, 0);
				}
				loop.End();
				const LoopReport r = analyzer.Finish();
				const HdrInferenceSignal noSwitch = BuildHdrInferenceSignalFromState(loop.State().p, true, BitDepth::BITDEPTH_8BIT, Gate(false), false);
				if (vsif == DolbyVisionVsif::LowLatency)
				{
					Assert::AreEqual<uint64_t>(1, r.handoffs);
					Assert::AreEqual<uint64_t>(1, r.sdr, L"handed off at the first picture");
					Assert::IsTrue(EvaluateHdrInferenceEligibility(noSwitch).eligible, L"the VSIF makes 8-bit LLDV PQ");
					Assert::AreEqual(std::string("BT.2020, 8-bit, no HDR InfoFrame, Dolby VSIF says LLDV"),
						std::string(EvaluateHdrInferenceEligibility(noSwitch).reason));
				}
				else
				{
					Assert::AreEqual<uint64_t>(0, r.handoffs, L"TV-led VSIF: the tunnel keeps the signal");
					Assert::IsFalse(EvaluateHdrInferenceEligibility(noSwitch).eligible);
				}
			}
		}

	private:
		static std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }

		// A natural-looking 10-bit RGB picture in r210 (big-endian [00 R G B]),
		// limited range: smooth gradients with sensor-like noise, so every bit
		// of the 10-bit fields is in use.
		static void TenBitRgbPicture(uint32_t w, uint32_t h, uint32_t n, uint32_t rowBytes, std::vector<uint8_t>& bytes)
		{
			bytes.assign(static_cast<size_t>(rowBytes) * h, 0);
			uint64_t s = 0x9E3779B97F4A7C15ULL ^ n;
			for (uint32_t y = 0; y < h; ++y)
				for (uint32_t x = 0; x < w; ++x)
				{
					s = s * 6364136223846793005ULL + 1442695040888963407ULL;
					const int noise = static_cast<int>((s >> 59) & 7) - 3;
					const int base = 120 + static_cast<int>((x * 640) / w) + static_cast<int>((y * 120) / h) + static_cast<int>(n % 5);
					const uint32_t r = static_cast<uint32_t>((std::min)(940, (std::max)(64, base + 30 + noise)));
					const uint32_t g = static_cast<uint32_t>((std::min)(940, (std::max)(64, base + noise / 2)));
					const uint32_t b = static_cast<uint32_t>((std::min)(940, (std::max)(64, base - 40 - noise)));
					const uint32_t word = (r << 20) | (g << 10) | b;
					uint8_t* at = bytes.data() + static_cast<size_t>(y) * rowBytes + static_cast<size_t>(x) * 4;
					at[0] = static_cast<uint8_t>(word >> 24); at[1] = static_cast<uint8_t>(word >> 16);
					at[2] = static_cast<uint8_t>(word >> 8); at[3] = static_cast<uint8_t>(word);
				}
		}

		// An 8-bit RGB picture (R, G, B per pixel): smooth, slightly warm.
		static void EightBitRgbPicture(uint32_t w, uint32_t h, uint32_t n, std::vector<uint8_t>& rgb)
		{
			rgb.resize(static_cast<size_t>(w) * h * 3);
			for (size_t p = 0; p < static_cast<size_t>(w) * h; ++p)
			{
				const uint8_t v = static_cast<uint8_t>(40 + ((p % w) * 120) / w + (n % 3));
				rgb[p * 3] = static_cast<uint8_t>(v + 10); rgb[p * 3 + 1] = v; rgb[p * 3 + 2] = static_cast<uint8_t>(v - 5);
			}
		}
	};
}
