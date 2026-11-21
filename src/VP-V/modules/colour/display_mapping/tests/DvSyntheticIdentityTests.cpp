/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// CF-1 P6 (refs/125 s6): the synthetic + identity suite for the DV display-mapping slots. CPU only; added beside the
// existing display_mapping tests, none of which is edited. The rows those files already pin (L2 only, L8 preferred,
// interpolation, slot off, calibration LUT, DV -> non-DV clear) are not repeated; this file adds the metadata mix,
// the trim targets at / around the screen peak, L6 / L0 / L9 / L11 in the reference record, the rejected-DM hold
// (visible), DV -> HDR10 -> DV, an unsupported output transfer, and the 12-bit ramp row (CPU model of the W4 GPU row).
// The deband rows wait for slice 17 (P5); there are no room-freeze rows (removed from the plan).
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/modules/colour/display_mapping/DvHostStage.h>
#include <VP-V/modules/colour/display_mapping/DmCurve.h>
#include <VP-V/modules/colour/display_mapping/DvDolbyStyle.h>
#include <VP-V/modules/colour/display_mapping/DvMath.h>
#include <VP-V/modules/colour/display_mapping/DvTrimSelection.h>
#include <VP-V/modules/input/signal/DvMetadataHold.h>
#include <VP-V/segments/OutputSegments.h>
#include <VP-V/modules/output/calibration_lut/CalibrationLutStage.h>
#include <VP-V/modules/drive/led_measure/LedMeasurementHook.h>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <map>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
	namespace DvP6
	{
		using Trim = DolbyVisionTunnelFrameMetadata::Trim;

		inline Trim L2(uint16_t target, uint16_t slope = 2048)
		{
			Trim t;
			t.level = 2; t.targetMaxPq = target; t.slope = slope; t.offset = 2048; t.power = 2048;
			t.chromaWeight = 2048; t.saturationGain = 2048;
			return t;
		}

		inline Trim L8(uint8_t index, uint16_t slope = 2048)
		{
			Trim t = L2(0, slope);
			t.level = 8;
			t.targetDisplayIndex = index;
			return t;
		}

		inline DolbyVisionTunnelFrameMetadata Dm(std::initializer_list<Trim> trims, uint16_t sourceMax = 3696)
		{
			DolbyVisionTunnelFrameMetadata dm;
			dm.valid = true;
			dm.sourceMinPq = 62;
			dm.sourceMaxPq = sourceMax;
			for (const Trim& t : trims)
				dm.trims[dm.trimCount++] = t;
			return dm;
		}

		inline void WithL1(DolbyVisionTunnelFrameMetadata& dm)
		{
			dm.hasSceneMetadata = true;
			dm.sceneMinPq = 62; dm.sceneAvgPq = 1500; dm.sceneMaxPq = 3000;
		}

		inline uint16_t Pq(double nits) { return static_cast<uint16_t>(std::lround(DolbyVisionTrims::PqFromNits(nits) * 4095.0)); }

		// The fallback registry as the host sees it.
		struct Registry
		{
			std::map<std::string, std::string> engaged;
			DvHostStage::Engage Engage() { return [this](const char* id, const std::string& r) { engaged[id] = r; }; }
			DvHostStage::Release Release() { return [this](const char* id) { engaged.erase(id); }; }
			bool Has(const char* id) const { return engaged.count(id) != 0; }
		};

		inline CustomHookChain::Loader NoShaders()
		{
			return {
				[](const std::string&, std::string&, std::string& reason) { reason = "not found (test)"; return false; },
				[](const std::string&) -> const pl_hook* { return nullptr; },
				[](const pl_hook*& hook) { hook = nullptr; } };
		}

		inline pl_frame Picture(bool dolbyVision)
		{
			pl_frame f{};
			f.repr = pl_color_repr_unknown;
			f.repr.sys = dolbyVision ? PL_COLOR_SYSTEM_DOLBYVISION : PL_COLOR_SYSTEM_BT_2020_NC;
			f.color = pl_color_space_hdr10;
			return f;
		}

		inline pl_frame Target(enum pl_color_transfer trc, float peakNits)
		{
			pl_frame f{};
			f.color.primaries = PL_COLOR_PRIM_BT_2020;
			f.color.transfer = trc;
			f.color.hdr.max_luma = peakNits;
			return f;
		}

		// One render frame as the host runs it (phase 1, the S2 segment collecting, phase 2).
		inline void Frame(DvHostStage& stage, const pl_frame& image, const pl_frame& target,
			const DolbyVisionTunnelFrameMetadata* dm, bool trims = true)
		{
			pl_color_map_params params = pl_color_map_default_params;
			std::vector<const pl_hook*> hooks;
			VideoState state;
			CustomHookChain::TemplateValues templates;
			OutputSegments::ColourFrame colourFrame;
			std::vector<const pl_hook*> user;
			colourFrame.userPreOutput = &user;
			stage.PrepareToneAndTrims({ "libplacebo", trims, "bt2390", std::string(), true, image, target, dm, 2.4, state, nullptr, 1, true },
				params, templates, colourFrame.dvTrims, colourFrame.dv);
			CustomHookChain::Chain tone(NoShaders()), comp(NoShaders());
			LedDrive::MeasurementHook measure;
			CalibrationLutStage::Hook lut;
			OutputSegments::OutputColourSegment s2(tone, stage.TrimsHook(), measure, comp, lut);
			PipelineSlots::Registry registry;
			registry.SetEnabled(PipelineSlots::Id::ColourDmTrims, true);
			registry.SetEnabled(PipelineSlots::Id::ColourDmCurve, true);
			const OutputSegments::ColourCounts counts = s2.Collect(colourFrame, registry, hooks, [](size_t, PipelineSlots::Id) {});
			stage.FinishTrims(counts.dvTrims);
		}
	}

	// Metadata mix: which level answers for each combination of L1, L2 and L8.
	TEST_CLASS(DvMetadataMixTests)
	{
	public:
		TEST_METHOD(L1PresentOrAbsentDoesNotChangeTheTrimChoice)
		{
			using namespace DvP6;
			DolbyVisionTunnelFrameMetadata with = Dm({ L2(Pq(100.0), 2400) });
			WithL1(with);
			const DolbyVisionTunnelFrameMetadata without = Dm({ L2(Pq(100.0), 2400) });
			const DvTrimSelection::Selection a = DvTrimSelection::Select(with, 100.0);
			const DvTrimSelection::Selection b = DvTrimSelection::Select(without, 100.0);
			Assert::IsTrue(a.level == DvTrimSelection::Level::L2 && b.level == DvTrimSelection::Level::L2);
			Assert::IsTrue(a.apply && b.apply);
			Assert::AreEqual(a.params.base.slope, b.params.base.slope);
		}

		TEST_METHOD(L2OnlyAndL8OnlyEachAnswerAlone)
		{
			using namespace DvP6;
			const DvTrimSelection::Selection l2 = DvTrimSelection::Select(Dm({ L2(Pq(100.0), 2400) }), 100.0);
			Assert::IsTrue(l2.level == DvTrimSelection::Level::L2 && l2.apply);
			const DvTrimSelection::Selection l8 = DvTrimSelection::Select(Dm({ L8(1, 2400) }), 100.0);   // predefined index 1 = 100 nits
			Assert::IsTrue(l8.level == DvTrimSelection::Level::L8 && l8.apply);
			Assert::AreEqual(1, l8.l8Present);
			Assert::AreEqual(0, l8.l8Unresolved);
		}

		TEST_METHOD(L8WithUnknownTargetsAndNoL2AppliesNothingAndSaysWhy)
		{
			using namespace DvP6;
			const DolbyVisionTunnelFrameMetadata dm = Dm({ L8(77, 2400) });
			const DvTrimSelection::Selection s = DvTrimSelection::Select(dm, 100.0);
			Assert::IsTrue(s.level == DvTrimSelection::Level::None && !s.apply);
			Assert::AreEqual(1, s.l8Unresolved);
			Assert::AreEqual(std::string("no L2 trim; L8 targets unknown"), std::string(s.reason));
		}

		TEST_METHOD(L2AndL8TogetherTheBetterCoveringLevelWins)
		{
			using namespace DvP6;
			// L8 at 100 nits and L2 at 100 nits cover a 100-nit screen equally: L8 is preferred.
			DvTrimSelection::Selection s = DvTrimSelection::Select(Dm({ L2(Pq(100.0), 2300), L8(1, 2400) }), 100.0);
			Assert::IsTrue(s.level == DvTrimSelection::Level::L8);
			// L8 only at 100 nits; L2 brackets an 800-nit screen (600 and 1000): the grader's L2 keeps the screen.
			s = DvTrimSelection::Select(Dm({ L2(Pq(600.0), 2300), L2(Pq(1000.0), 2200), L8(1, 2400) }), 800.0);
			Assert::IsTrue(s.level == DvTrimSelection::Level::L2);
			Assert::IsTrue(s.l8CoveredWorse);
		}

		TEST_METHOD(NoMetadataAtAllIsNoneAndAnInvalidDmIsNotUsed)
		{
			using namespace DvP6;
			Assert::IsTrue(DvTrimSelection::Select(Dm({}), 100.0).level == DvTrimSelection::Level::None);
			DolbyVisionTunnelFrameMetadata bad = Dm({ L2(Pq(100.0), 2400) });
			bad.valid = false;
			const DvTrimSelection::Selection s = DvTrimSelection::Select(bad, 100.0);
			Assert::IsTrue(!s.apply && s.level == DvTrimSelection::Level::None);
		}
	};

	// Trim targets below / at / above the screen peak, and the interpolation between two.
	TEST_CLASS(DvTrimTargetPositionTests)
	{
	public:
		TEST_METHOD(ScreenBelowAtAndAboveTheTrimTargets)
		{
			using namespace DvP6;
			const DolbyVisionTunnelFrameMetadata dm = Dm({ L2(Pq(100.0), 2500), L2(Pq(600.0), 2300) });
			const DolbyVisionTrims::Params lowTrim = DolbyVisionTrims::Decode(dm.trims[0]);
			const DolbyVisionTrims::Params highTrim = DolbyVisionTrims::Decode(dm.trims[1]);
			// Below every target: the lowest-target trim, unchanged.
			DvTrimSelection::Selection s = DvTrimSelection::Select(dm, 50.0);
			Assert::IsTrue(s.apply);
			Assert::AreEqual(lowTrim.slope, s.params.base.slope);
			Assert::IsTrue(std::string(s.reason).find("below every trim target") != std::string::npos);
			// At a target: that trim exactly.
			s = DvTrimSelection::Select(dm, 100.0);
			Assert::AreEqual(lowTrim.slope, s.params.base.slope);
			s = DvTrimSelection::Select(dm, 600.0);
			Assert::AreEqual(highTrim.slope, s.params.base.slope);
			// Above every target (below the mastering peak): fading toward neutral.
			s = DvTrimSelection::Select(dm, 1000.0);
			Assert::IsTrue(std::string(s.reason).find("above every trim target") != std::string::npos);
			Assert::IsTrue(s.weight > 0.0 && s.weight < 1.0);
		}

		TEST_METHOD(InterpolationLiesBetweenTheTwoTrimsAndFollowsTheTargetInPq)
		{
			using namespace DvP6;
			const DolbyVisionTunnelFrameMetadata dm = Dm({ L2(Pq(100.0), 2500), L2(Pq(600.0), 2300) });
			const double lo = DolbyVisionTrims::Decode(dm.trims[0]).slope, hi = DolbyVisionTrims::Decode(dm.trims[1]).slope;
			double previous = lo;
			for (double nits : { 120.0, 200.0, 300.0, 450.0, 580.0 })
			{
				const DvTrimSelection::Selection s = DvTrimSelection::Select(dm, nits);
				Assert::IsTrue(s.weight > 0.0 && s.weight < 1.0);
				const double slope = s.params.base.slope;
				Assert::IsTrue(slope <= (std::max)(lo, hi) + 1e-12 && slope >= (std::min)(lo, hi) - 1e-12);
				Assert::IsTrue((hi >= lo && slope >= previous - 1e-12) || (hi < lo && slope <= previous + 1e-12), L"monotonic in the target");
				previous = slope;
			}
		}

		TEST_METHOD(AScreenAtOrAboveTheMasteringPeakIsIdentity)
		{
			using namespace DvP6;
			const DolbyVisionTunnelFrameMetadata dm = Dm({ L2(Pq(100.0), 2500) }, Pq(1000.0));
			const DvTrimSelection::Selection s = DvTrimSelection::Select(dm, 1000.0);
			Assert::IsFalse(s.apply);
			Assert::IsTrue(std::string(s.reason).find("identity") != std::string::npos);
		}
	};

	// L6 / L0 / L9 / L11 in the reference record: what the CAM16 stage must not compensate twice.
	TEST_CLASS(DvReferenceRecordTests)
	{
	public:
		TEST_METHOD(L6IsPreferredOverL0ForTheMasteringDisplay)
		{
			using namespace DvP6;
			DolbyVisionTunnelFrameMetadata dm = Dm({ L2(Pq(100.0), 2400) });
			DvDolbyStyle::ReferenceConditions r = DvDolbyStyle::Conditions(dm, DvTrimSelection::Select(dm, 100.0), false, "bt2390");
			Assert::IsTrue(r.valid);
			Assert::IsFalse(r.masteringFromL6, L"no L6: the DM's source range (L0)");
			Assert::IsTrue(r.masteringPeakNits > 1000.0);
			dm.hasL6 = true;
			dm.l6MaxDisplayMasteringLuminance = 1000;
			dm.l6MinDisplayMasteringLuminance = 50;
			r = DvDolbyStyle::Conditions(dm, DvTrimSelection::Select(dm, 100.0), false, "bt2390");
			Assert::IsTrue(r.masteringFromL6);
			Assert::AreEqual(1000.0, r.masteringPeakNits);
			Assert::AreEqual(0.005, r.masteringMinNits, 1e-9);
		}

		TEST_METHOD(L9SourcePrimariesAreRecordedOrAbsent)
		{
			using namespace DvP6;
			DolbyVisionTunnelFrameMetadata dm = Dm({ L2(Pq(100.0), 2400) });
			Assert::AreEqual(-1, DvDolbyStyle::Conditions(dm, DvTrimSelection::Select(dm, 100.0), false, "bt2390").sourcePrimaryIndex);
			dm.hasL9 = true;
			dm.l9SourcePrimaryIndex = 2;
			Assert::AreEqual(2, DvDolbyStyle::Conditions(dm, DvTrimSelection::Select(dm, 100.0), false, "bt2390").sourcePrimaryIndex);
		}

		// L11 is recorded for the appearance stage (reference mode, content type); room freezing is not part of this slot.
		TEST_METHOD(L11ContentTypeAndReferenceModeAreRecorded)
		{
			using namespace DvP6;
			DolbyVisionTunnelFrameMetadata dm = Dm({ L2(Pq(100.0), 2400) });
			DvDolbyStyle::ReferenceConditions r = DvDolbyStyle::Conditions(dm, DvTrimSelection::Select(dm, 100.0), false, "bt2390");
			Assert::AreEqual<int>(0, r.contentType);
			Assert::IsFalse(r.referenceMode);
			dm.hasL11 = true;
			dm.l11ContentType = 1;
			dm.l11ReferenceMode = true;
			r = DvDolbyStyle::Conditions(dm, DvTrimSelection::Select(dm, 100.0), false, "bt2390");
			Assert::AreEqual<int>(1, r.contentType);
			Assert::IsTrue(r.referenceMode);
		}

		TEST_METHOD(AnInvalidDmGivesNoRecord)
		{
			using namespace DvP6;
			DolbyVisionTunnelFrameMetadata dm = Dm({ L2(Pq(100.0), 2400) });
			dm.valid = false;
			Assert::IsFalse(DvDolbyStyle::Conditions(dm, DvTrimSelection::Select(dm, 100.0), false, "bt2390").valid);
		}
	};

	// Malformed / truncated RPU: the bounded hold, and its visible entry.
	TEST_CLASS(DvRejectedDmHoldTests)
	{
	public:
		TEST_METHOD(RejectedPicturesHoldThenGoStaticAndAnAcceptedOneEndsTheRun)
		{
			DvMetadataHold::Tracker t;
			t.SetRate(60.0, 2.0);
			const uint32_t hold = t.HoldPictures();
			Assert::IsTrue(hold >= 1);
			uint64_t counter = 100;
			for (uint32_t k = 0; k < hold; ++k)
				Assert::IsTrue(t.OnPicture(false, true, counter++) == DvMetadataHold::Phase::Held);
			Assert::IsTrue(t.OnPicture(false, true, counter++) == DvMetadataHold::Phase::Static);
			Assert::IsTrue(t.EnteredStatic());
			Assert::AreEqual<uint32_t>(hold + 1, t.RejectedPictures());
			Assert::IsTrue(t.OnPicture(true, true, counter++) == DvMetadataHold::Phase::Own);
			Assert::AreEqual<uint32_t>(1, t.Runs());
		}

		TEST_METHOD(TheStaticPhaseDropsL1AndTheTrimsButKeepsTheTitleLevels)
		{
			using namespace DvP6;
			DolbyVisionTunnelFrameMetadata dm = Dm({ L2(Pq(100.0), 2400) });
			WithL1(dm);
			dm.hasL6 = true; dm.l6MaxDisplayMasteringLuminance = 1000;
			dm.hasL9 = true; dm.hasL11 = true; dm.l11ContentType = 1;
			DvMetadataHold::StripToStatic(dm);
			Assert::IsFalse(dm.hasSceneMetadata);
			Assert::AreEqual<int>(0, dm.trimCount);
			Assert::IsTrue(dm.hasL6 && dm.hasL9 && dm.hasL11 && dm.valid, L"the title's levels stay");
			Assert::IsFalse(DvTrimSelection::Select(dm, 100.0).apply, L"no trims: neutral");
		}

		TEST_METHOD(HeldAndStaticAreListedUnderFallbacksEngagedAndReleaseWhenClean)
		{
			using namespace DvP6;
			Registry reg;
			DvHostStage stage(NoShaders(), reg.Engage(), reg.Release());
			DolbyVisionTunnelFrameMetadata dm = Dm({ L2(Pq(100.0), 2400) });
			dm.reusedPreviousDm = true;
			dm.dmHoldPhase = static_cast<uint8_t>(DvMetadataHold::Phase::Static);
			stage.OnTunnelState(true, &dm);
			Assert::IsTrue(reg.Has("DV metadata"));
			Assert::AreEqual(std::string("packets rejected beyond the hold: static metadata (no scene L1, no trims)"), reg.engaged["DV metadata"]);
			dm.reusedPreviousDm = false;
			dm.dmHoldPhase = 0;
			stage.OnTunnelState(true, &dm);
			Assert::IsFalse(reg.Has("DV metadata"));
		}
	};

	// DV -> HDR10 -> DV through the host stage; an unsupported output transfer.
	TEST_CLASS(DvHostStageIdentityTests)
	{
	public:
		TEST_METHOD(DvThenHdr10ThenDvClearsAndRebuildsTheRecord)
		{
			using namespace DvP6;
			Registry reg;
			DvHostStage stage(NoShaders(), reg.Engage(), reg.Release());
			DolbyVisionTunnelFrameMetadata dm = Dm({ L2(Pq(100.0), 2400) });
			WithL1(dm);
			dm.hasL6 = true; dm.l6MaxDisplayMasteringLuminance = 4000;
			dm.hasL11 = true; dm.l11ContentType = 1; dm.l11ReferenceMode = true;
			Frame(stage, Picture(true), Target(PL_COLOR_TRC_PQ, 100.0f), &dm);
			Assert::IsTrue(stage.ReferenceConditions().valid);
			Assert::AreEqual(4000.0, stage.ReferenceConditions().masteringPeakNits);
			Frame(stage, Picture(false), Target(PL_COLOR_TRC_PQ, 100.0f), &dm);
			Assert::IsFalse(stage.ReferenceConditions().valid, L"HDR10: no DV reference");
			Assert::AreEqual<int>(0, stage.ReferenceConditions().contentType);
			Assert::IsFalse(stage.ReferenceConditions().masteringFromL6);
			DolbyVisionTunnelFrameMetadata second = Dm({ L2(Pq(100.0), 2400) });   // a new title: no L6, no L11
			Frame(stage, Picture(true), Target(PL_COLOR_TRC_PQ, 100.0f), &second);
			Assert::IsTrue(stage.ReferenceConditions().valid);
			Assert::IsFalse(stage.ReferenceConditions().masteringFromL6, L"nothing carried over from the first title");
			Assert::AreEqual<int>(0, stage.ReferenceConditions().contentType);
		}

		TEST_METHOD(AnUnsupportedOutputTransferIsListedNotSilent)
		{
			using namespace DvP6;
			Registry reg;
			DvHostStage stage(NoShaders(), reg.Engage(), reg.Release());
			const DolbyVisionTunnelFrameMetadata dm = Dm({ L2(Pq(100.0), 2400) });
			Frame(stage, Picture(true), Target(PL_COLOR_TRC_HLG, 100.0f), &dm);
			Assert::IsTrue(reg.Has("Dolby Vision trims"), L"dv_trims on, a DV picture, an output the trims cannot act on");
			Frame(stage, Picture(true), Target(PL_COLOR_TRC_PQ, 100.0f), &dm, false);
			Assert::IsFalse(reg.Has("Dolby Vision trims"), L"trims switched off by the user: nothing listed");
		}
	};

	// The 12-bit ramp row. The W4 GPU form (all 4096 I codes through PRE_OUTPUT with fbo_float32; no step > 1 12-bit code)
	// needs a device and runs in the W4 GPU round; this is its CPU model: the baked tone LUT the host would build for the
	// chosen size, interpolated in float32 per code as the shader does, over every 12-bit code of the scene range.
	TEST_CLASS(DvTwelveBitRampTests)
	{
	public:
		TEST_METHOD(W4Row_AllTwelveBitCodesStayMonotonicWithNoStepOverOneCode)
		{
			for (double target : { 120.0, 300.0, 600.0, 1000.0 })
			{
				const DvDolbyStyle::Curve c = DvDolbyStyle::Build(DvMath::PqFromNits(0.005), DvMath::PqFromNits(150.0),
					DvMath::PqFromNits(4000.0), DvMath::PqFromNits(0.05), DvMath::PqFromNits(target));
				const DmCurve::LutChoice choice = DmCurve::ChooseLutSize(c);
				Assert::IsTrue(choice.size >= 2);
				const double span = c.sMax - c.sMin;
				std::vector<float> lut(choice.size);
				for (size_t i = 0; i < choice.size; ++i)
					lut[i] = static_cast<float>(c.Map(c.sMin + span * static_cast<double>(i) / static_cast<double>(choice.size - 1)));
				const long first = static_cast<long>(std::ceil(c.sMin * DmCurve::kCodeScale));
				const long last = static_cast<long>(std::floor(c.sMax * DmCurve::kCodeScale));
				double previous = -1.0, previousAnalytic = -1.0;
				for (long code = first; code <= last; ++code)
				{
					const double x = static_cast<double>(code) / DmCurve::kCodeScale;
					const float pos = static_cast<float>((x - c.sMin) / span * static_cast<double>(choice.size - 1));
					size_t i = static_cast<size_t>(pos);
					if (i >= choice.size - 1) i = choice.size - 2;
					const float f = pos - static_cast<float>(i);
					const double out = (lut[i] + (lut[i + 1] - lut[i]) * f) * DmCurve::kCodeScale;
					const double analytic = c.Map(x) * DmCurve::kCodeScale;
					Assert::IsTrue(std::fabs(out - analytic) <= (choice.withinBudget ? DmCurve::kMaxErrorCodes : 2.0) + 1e-3, L"on the analytic curve");
					if (previous >= 0.0)
					{
						Assert::IsTrue(out >= previous - 2.0 * DmCurve::kMaxErrorCodes, L"monotonic");
						Assert::IsTrue(out - previous <= (analytic - previousAnalytic) + 2.0 * DmCurve::kMaxErrorCodes + 1e-3,
							L"no step larger than the analytic step plus the LUT budget");
					}
					previous = out;
					previousAnalytic = analytic;
				}
			}
		}
	};
}
