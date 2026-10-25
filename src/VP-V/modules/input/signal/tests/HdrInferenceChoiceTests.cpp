/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// F-8 (2026-10-25): the either/or between VP's HDR inference and Bill's original newlldv heuristic.
// The original rule (BT.2020 + SDR + no HDR InfoFrame, nothing else), the tracker in original
// mode (1.5 s confirmation, no signal epochs, no SDR veto), the key's parsing, and the selection
// gated through the slot registry (the Original pathway selects Bill's heuristic).
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/modules/input/signal/HdrInference.h>
#include <VP-V/modules/input/signal/HdrInferenceChoice.h>

#include <Windows.h>

#include <cstdio>
#include <fstream>
#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
	namespace HdrChoice
	{
		// An 8-bit BT.2020 / SDR signal without an InfoFrame: Bill's candidate, not VP's (no LLDV
		// indication at 8-bit).
		inline HdrInferenceSignal Candidate()
		{
			HdrInferenceSignal s;
			s.enabled = true;
			s.valid = true;
			s.colorspace = ColorSpace::BT_2020;
			s.eotf = EOTF::SDR;
			s.hdrInfoFrame = false;
			s.bitDepth = 8;
			s.signalKey = 1;
			s.lldvModesSelected = true;   // both HDR modes Follow input (LLDV)
			return s;
		}

		inline HdrInferenceChoice::Resolved FromText(const std::string& text)
		{
			char dir[MAX_PATH] = {};
			GetTempPathA(MAX_PATH, dir);
			const std::string path = std::string(dir) + "vp-hdr-choice-test-" + std::to_string(GetCurrentProcessId()) + ".cfg";
			{
				std::ofstream out(path, std::ios::binary);
				out << text;
			}
			ConfigFile config;
			Assert::IsTrue(config.Load(path), L"temporary configuration loads");
			const HdrInferenceChoice::Resolved r = HdrInferenceChoice::FromConfig(config);
			std::remove(path.c_str());
			return r;
		}
	}

	TEST_CLASS(HdrInferenceChoiceTests)
	{
	public:
		TEST_METHOD(OriginalRuleIsBt2020SdrWithoutAnInfoFrameOnly)
		{
			using namespace HdrChoice;
			HdrInferenceSignal s = Candidate();
			Assert::IsTrue(EvaluateOriginalNewLldvEligibility(s).eligible);
			Assert::IsFalse(EvaluateHdrInferenceEligibility(s).eligible, L"VP's inference needs 10-bit or an LLDV indication");
			s.dolbyVisionTunnel = true;
			s.explicitSettingsForbid = true;
			Assert::IsTrue(EvaluateOriginalNewLldvEligibility(s).eligible, L"no DV-tunnel or explicit-settings gate upstream");
			HdrInferenceSignal t = Candidate(); t.colorspace = ColorSpace::REC_709;
			Assert::IsFalse(EvaluateOriginalNewLldvEligibility(t).eligible);
			t = Candidate(); t.eotf = EOTF::PQ;
			Assert::IsFalse(EvaluateOriginalNewLldvEligibility(t).eligible);
			t = Candidate(); t.hdrInfoFrame = true;
			Assert::IsFalse(EvaluateOriginalNewLldvEligibility(t).eligible);
			t = Candidate(); t.valid = false;
			Assert::IsFalse(EvaluateOriginalNewLldvEligibility(t).eligible);
			t = Candidate(); t.lldvModesSelected = false;
			Assert::IsFalse(EvaluateOriginalNewLldvEligibility(t).eligible, L"an explicit hdr_colorspace / hdr_luminance rejects, as upstream");
			t = Candidate(); t.enabled = false;
			Assert::IsFalse(EvaluateOriginalNewLldvEligibility(t).eligible, L"newlldv stays the master switch");
		}

		TEST_METHOD(OriginalModeConfirmsAfterTheStabilisationWithoutEpochsOrVeto)
		{
			using namespace HdrChoice;
			HdrInferenceTracker tracker;
			HdrInferenceOptions options;
			options.originalHeuristic = true;
			tracker.SetOptions(options);
			HdrInferenceSignal s = Candidate();
			Assert::IsTrue(tracker.OnSignal(s, 0).event == HdrInferenceEvent::CandidateSeen);
			Assert::IsFalse(tracker.IsInferredPq());
			tracker.OnSignal(s, 1499);
			Assert::IsFalse(tracker.IsInferredPq(), L"not before 1500 ms");
			const HdrInferenceUpdate confirmed = tracker.OnSignal(s, 1500);
			Assert::IsTrue(confirmed.event == HdrInferenceEvent::Inferred && confirmed.effectiveChanged);
			Assert::IsTrue(tracker.IsInferredPq());
			// A new signal key while still a candidate: no new epoch upstream.
			s.signalKey = 2;
			Assert::IsTrue(tracker.OnSignal(s, 2000).event == HdrInferenceEvent::None);
			Assert::IsTrue(tracker.IsInferredPq());
			// No SDR veto: no statistics wanted, pictures change nothing.
			Assert::IsFalse(tracker.WantsPictureStatistics());
			HdrPictureObservation picture;
			picture.measured = true;
			picture.activePicture = true;
			picture.sdrEvidence = true;
			for (uint64_t t = 2000; t < 60000; t += 100)
			{
				picture.cut = (t % 3000) == 0;
				tracker.OnPicture(picture, t);
				tracker.OnSignal(s, t);
			}
			Assert::IsTrue(tracker.IsInferredPq(), L"no veto in the original heuristic");
			// Losing the candidate ends it, as upstream.
			s.eotf = EOTF::PQ;
			const HdrInferenceUpdate ended = tracker.OnSignal(s, 61000);
			Assert::IsTrue(ended.event == HdrInferenceEvent::Ended && ended.effectiveChanged);
			Assert::IsFalse(tracker.IsCandidate());
		}

		TEST_METHOD(VpModeIsUnchangedByDefault)
		{
			using namespace HdrChoice;
			HdrInferenceTracker tracker;
			Assert::IsFalse(tracker.Options().originalHeuristic, L"default: VP's inference");
			HdrInferenceSignal s = Candidate();
			for (uint64_t t = 0; t <= 5000; t += 500)
				tracker.OnSignal(s, t);
			Assert::IsFalse(tracker.IsInferredPq(), L"VP: an 8-bit candidate without LLDV indication is never inferred");
			s.bitDepth = 10;
			for (uint64_t t = 6000; t <= 9000; t += 500)
				tracker.OnSignal(s, t);
			Assert::IsTrue(tracker.IsInferredPq());
			Assert::IsTrue(tracker.WantsPictureStatistics(), L"VP's veto measures pictures");
		}

		TEST_METHOD(KeyParsesAndTheOriginalPathwaySelectsBillsHeuristic)
		{
			using namespace HdrChoice;
			HdrInferenceChoice::Mode mode = HdrInferenceChoice::Mode::Vp;
			Assert::IsTrue(HdrInferenceChoice::Parse("original", mode) && mode == HdrInferenceChoice::Mode::Original);
			Assert::IsTrue(HdrInferenceChoice::Parse("VP", mode) && mode == HdrInferenceChoice::Mode::Vp);
			Assert::IsFalse(HdrInferenceChoice::Parse("bill", mode));

			PipelineSlots::Registry registry;
			Assert::IsTrue(HdrInferenceChoice::Effective(HdrInferenceChoice::Mode::Vp, registry) == HdrInferenceChoice::Mode::Vp);
			Assert::IsTrue(HdrInferenceChoice::Effective(HdrInferenceChoice::Mode::Original, registry) == HdrInferenceChoice::Mode::Original);
			registry.ApplyOriginalPathway();
			Assert::IsTrue(HdrInferenceChoice::Effective(HdrInferenceChoice::Mode::Vp, registry) == HdrInferenceChoice::Mode::Original,
				L"the Original pathway puts input.signal in its upstream form");

			HdrInferenceChoice::Resolved r = FromText("[general]\nnewlldv: true\n");
			Assert::IsTrue(r.setting == HdrInferenceChoice::Mode::Vp && r.effective == HdrInferenceChoice::Mode::Vp, L"default vp");
			r = FromText("[vpv]\nhdr_inference: original\n");
			Assert::IsTrue(r.effective == HdrInferenceChoice::Mode::Original);
			r = FromText("[vpv]\nhdr_inference: vp\npipeline_original_pathway: on\n");
			Assert::IsTrue(r.setting == HdrInferenceChoice::Mode::Vp && r.effective == HdrInferenceChoice::Mode::Original && r.originalPathway);
			Assert::IsTrue(HdrInferenceChoice::Describe(r).find("Original pathway") != std::string::npos);
			Assert::IsTrue(HdrInferenceChoice::StartupLine(true, r).find("Original pathway") != std::string::npos,
				L"the override is logged at start-up");
			Assert::IsTrue(HdrInferenceChoice::StartupLine(false, r).find("command line") != std::string::npos);
			r = FromText("[vpv]\nhdr_inference: maybe\n");
			Assert::IsTrue(r.effective == HdrInferenceChoice::Mode::Vp && !r.problem.empty(), L"invalid: kept vp, reported");
		}
	};
}
