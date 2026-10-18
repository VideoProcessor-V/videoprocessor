#include "pch.h"
#include "CppUnitTest.h"

// newlldv repurposed (owner decision 2026-10-17): BT.2020 + no HDR InfoFrame
// + 10/12-bit is treated as PQ for LLDV and HDR10, with a picture-statistics
// SDR veto. These tests own the state machine, the veto statistics and the
// cut detector. PQ material is synthetic (no PQ masters on the laptop): SDR
// scenes are re-encoded through an SDR-to-PQ expansion, the approach of the
// design study (scratchpad hdrdetect, programme_sim.py / variants.py).
// HdrInferenceRealClipVeto runs on the D:\hfr SDR clips when
// VP_TEST_REAL_FOOTAGE names that folder; otherwise it logs and passes.

#include <AviInfoFrameColorimetry.h>
#include <HdrInference.h>
#include <HdrVetoCaptureGate.h>
#include <RendererProfileConfig.h>
#include <VideoState.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace
{
	constexpr int COLUMNS = 160;
	constexpr int ROWS = 90;

	HdrInferenceSignal Signal(int bitDepth, uint64_t key = 1)
	{
		HdrInferenceSignal signal;
		signal.enabled = true;
		signal.valid = true;
		signal.colorspace = ColorSpace::BT_2020;
		signal.eotf = EOTF::SDR;
		signal.hdrInfoFrame = false;
		signal.bitDepth = bitDepth;
		signal.signalKey = key;
		return signal;
	}

	// LLDV as a capture card reports it: BT.2020 + SDR, no DRM, 12-bit.
	HdrInferenceSignal LldvSignal() { return Signal(12, 0x1111); }
	// HDR10 behind an HDFury Integral 2 with its HDR toggle off: the DRM
	// InfoFrame is dropped, the AVI InfoFrame still says BT.2020; 10-bit.
	HdrInferenceSignal Hdr10FlagDroppedSignal() { return Signal(10, 0x2222); }

	HdrInferenceTracker InferredTracker(const HdrInferenceSignal& signal,
		uint64_t& now)
	{
		HdrInferenceTracker tracker;
		tracker.OnSignal(signal, now);
		now += 1500;
		tracker.OnSignal(signal, now);
		Assert::IsTrue(tracker.IsInferredPq());
		return tracker;
	}

	HdrPictureObservation Picture(bool sdrEvidence, bool cut = false)
	{
		HdrPictureObservation picture;
		picture.measured = true;
		picture.activePicture = true;
		picture.sdrEvidence = sdrEvidence;
		picture.cut = cut;
		return picture;
	}

	// Feeds one shot of `ms` at 20 Hz; the first picture carries the cut.
	HdrInferenceEvent Shot(HdrInferenceTracker& tracker, uint64_t& now,
		uint64_t ms, bool sdrEvidence, bool leadingCut = true)
	{
		HdrInferenceEvent last = HdrInferenceEvent::None;
		for (uint64_t t = 0; t < ms; t += 50)
		{
			const HdrInferenceUpdate update = tracker.OnPicture(
				Picture(sdrEvidence, leadingCut && t == 0), now);
			if (update.event != HdrInferenceEvent::None)
				last = update.event;
			now += 50;
			if (tracker.State() == HdrInferenceState::VetoedSdr)
				break;
		}
		return last;
	}

	// --- synthetic scenes (luma only, limited 10-bit codes) ---------------

	double PqOetf(double nits)
	{
		const double m1 = 2610.0 / 16384.0, m2 = 2523.0 / 32.0;
		const double c1 = 3424.0 / 4096.0, c2 = 2413.0 / 128.0, c3 = 2392.0 / 128.0;
		const double y = std::pow(std::min(std::max(nits / 10000.0, 0.0), 1.0), m1);
		return std::pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
	}

	// SDR linear (1 = diffuse white) -> nits, stretching above a 0.7 knee so
	// SDR white reaches `peak` (variants.py expand()).
	double Expand(double linear, double whiteNits, double peakNits)
	{
		const double knee = 0.7;
		const double f1 = peakNits / whiteNits;
		const double out = linear < knee ? linear :
			knee + (linear - knee) * (f1 - knee) / (1.0 - knee);
		return out * whiteNits;
	}

	uint16_t SdrCode(double linear)
	{
		const double v = std::pow(std::min(std::max(linear, 0.0), 1.0), 1.0 / 2.4);
		return static_cast<uint16_t>(std::lround(64.0 + 876.0 * v));
	}

	uint16_t PqCode(double nits)
	{
		return static_cast<uint16_t>(std::lround(64.0 + 876.0 * PqOetf(nits)));
	}

	// Scene linear light: a smooth gradient with texture and a few small
	// specular highlights (clip to SDR white). `exposure` scales it.
	std::vector<double> SceneLinear(uint32_t seed, double exposure,
		double highlightArea, int shift = 0, int letterboxRows = 0)
	{
		std::mt19937 random(seed);
		std::uniform_real_distribution<double> unit(0.0, 1.0);
		const double base = 0.05 + 0.3 * unit(random);
		const double gx = unit(random) * 0.4, gy = unit(random) * 0.4;
		const double fx = 2.0 + 6.0 * unit(random), fy = 2.0 + 6.0 * unit(random);
		std::vector<double> linear(static_cast<size_t>(COLUMNS) * ROWS);
		for (int y = 0; y < ROWS; ++y)
			for (int x = 0; x < COLUMNS; ++x)
			{
				const double u = static_cast<double>(x + shift) / COLUMNS;
				const double v = static_cast<double>(y) / ROWS;
				double value = base + gx * u + gy * v +
					0.08 * std::sin(fx * u * 6.283) * std::cos(fy * v * 6.283);
				linear[static_cast<size_t>(y) * COLUMNS + x] = std::max(0.0, value) * exposure;
			}
		const int highlights = static_cast<int>(highlightArea * COLUMNS * ROWS);
		for (int i = 0; i < highlights; ++i)
		{
			const int x = static_cast<int>(unit(random) * COLUMNS);
			const int y = letterboxRows + static_cast<int>(unit(random) * (ROWS - 2 * letterboxRows));
			linear[static_cast<size_t>(y) * COLUMNS + std::min(x + shift, COLUMNS - 1)] =
				1.5 + unit(random);
		}
		for (int y = 0; y < letterboxRows; ++y)
			for (int x = 0; x < COLUMNS; ++x)
			{
				linear[static_cast<size_t>(y) * COLUMNS + x] = 0.0;
				linear[static_cast<size_t>(ROWS - 1 - y) * COLUMNS + x] = 0.0;
			}
		return linear;
	}

	std::vector<uint16_t> SdrGrid(const std::vector<double>& linear)
	{
		std::vector<uint16_t> grid(linear.size());
		for (size_t i = 0; i < linear.size(); ++i)
			grid[i] = SdrCode(linear[i]);
		return grid;
	}

	std::vector<uint16_t> PqGrid(const std::vector<double>& linear,
		double whiteNits = 203.0, double peakNits = 1000.0)
	{
		std::vector<uint16_t> grid(linear.size());
		for (size_t i = 0; i < linear.size(); ++i)
			grid[i] = PqCode(Expand(std::min(linear[i], 1.0), whiteNits, peakNits));
		return grid;
	}

	HdrVetoFrameStats Measure(const std::vector<uint16_t>& grid)
	{
		return MeasureHdrVetoGrid(grid.data(), COLUMNS, ROWS);
	}
}

namespace Tests
{
	TEST_CLASS(HdrInferenceTests)
	{
	public:
		TEST_METHOD(EligibilityLldvAndHdr10FlagDroppedQualify)
		{
			Assert::IsTrue(EvaluateHdrInferenceEligibility(LldvSignal()).eligible);
			Assert::IsTrue(EvaluateHdrInferenceEligibility(Hdr10FlagDroppedSignal()).eligible);
		}

		TEST_METHOD(EligibilityNeverForTunnelBt709UnknownRealInfoFrameOr8Bit)
		{
			auto rejected = [](HdrInferenceSignal signal)
			{
				Assert::IsFalse(EvaluateHdrInferenceEligibility(signal).eligible);
				HdrInferenceTracker tracker;
				for (uint64_t now = 0; now <= 10000; now += 500)
					tracker.OnSignal(signal, now);
				Assert::IsFalse(tracker.IsInferredPq());
				Assert::IsFalse(tracker.IsCandidate());
			};
			HdrInferenceSignal s = Hdr10FlagDroppedSignal();
			s.dolbyVisionTunnel = true; rejected(s);
			s = Hdr10FlagDroppedSignal(); s.colorspace = ColorSpace::REC_709; rejected(s);
			s = Hdr10FlagDroppedSignal(); s.colorspace = ColorSpace::UNKNOWN; rejected(s);
			s = Hdr10FlagDroppedSignal(); s.colorspace = ColorSpace::P3_D65; rejected(s);
			s = Hdr10FlagDroppedSignal(); s.hdrInfoFrame = true; rejected(s);
			s = Hdr10FlagDroppedSignal(); s.eotf = EOTF::PQ; rejected(s);
			s = Hdr10FlagDroppedSignal(); s.eotf = EOTF::HLG; rejected(s);
			s = Hdr10FlagDroppedSignal(); s.bitDepth = 8; rejected(s);
			s = Hdr10FlagDroppedSignal(); s.bitDepth = 0; rejected(s);
			s = Hdr10FlagDroppedSignal(); s.valid = false; rejected(s);
			s = Hdr10FlagDroppedSignal(); s.enabled = false; rejected(s);
		}

		TEST_METHOD(PrecedenceExplicitSettingsOverrideInference)
		{
			// Explicit hdr_colorspace REC709 forbids the HDR10 reading.
			HdrInferenceSignal signal = Hdr10FlagDroppedSignal();
			signal.explicitSettingsForbid = true;
			const auto eligibility = EvaluateHdrInferenceEligibility(signal);
			Assert::IsFalse(eligibility.eligible);
			Assert::IsTrue(std::string(eligibility.reason).find("explicit") != std::string::npos);
			// Turning the explicit setting off returns to inference (a new epoch).
			HdrInferenceTracker tracker;
			tracker.OnSignal(signal, 0);
			Assert::IsFalse(tracker.IsCandidate());
			signal.explicitSettingsForbid = false;
			tracker.OnSignal(signal, 100);
			tracker.OnSignal(signal, 1600);
			Assert::IsTrue(tracker.IsInferredPq());
			// Profile metadata defaults used for the inferred path are sensible
			// for HDR10 tone mapping: libplacebo ignores MaxCLL/MaxFALL and
			// bounds by the mastering range, so 0.001..4000 never clips a
			// 1000- or 4000-nit grade and peak detection takes the scene level.
			const auto defaults = RendererProfileConfig::DefaultLldvMetadata(true);
			Assert::IsTrue(defaults.masteringMaxLuminance >= 4000.0);
			Assert::IsTrue(defaults.masteringMinLuminance > 0.0 &&
				defaults.masteringMinLuminance <= 0.005);
			Assert::IsTrue(defaults.maxCll >= 1000.0);
		}

		TEST_METHOD(FlagMissingSequencesInferPqForLldvAndHdr10)
		{
			for (const HdrInferenceSignal& signal :
				{ LldvSignal(), Hdr10FlagDroppedSignal() })
			{
				HdrInferenceTracker tracker;
				auto update = tracker.OnSignal(signal, 10000);
				Assert::IsTrue(update.event == HdrInferenceEvent::CandidateSeen);
				Assert::IsTrue(tracker.State() == HdrInferenceState::Waiting);
				Assert::IsFalse(update.effectiveChanged);
				update = tracker.OnTick(11499);
				Assert::IsFalse(tracker.IsInferredPq());
				update = tracker.OnTick(11500);
				Assert::IsTrue(update.event == HdrInferenceEvent::Inferred);
				Assert::IsTrue(update.effectiveChanged);
				Assert::IsTrue(tracker.IsInferredPq());
				Assert::IsTrue(tracker.WantsPictureStatistics());
				// The flag arriving later (a real HDR InfoFrame) ends inference.
				HdrInferenceSignal flagged = signal;
				flagged.hdrInfoFrame = true;
				flagged.eotf = EOTF::PQ;
				update = tracker.OnSignal(flagged, 12000);
				Assert::IsTrue(update.event == HdrInferenceEvent::Ended);
				Assert::IsTrue(update.effectiveChanged);
				Assert::IsFalse(tracker.IsInferredPq());
			}
		}

		TEST_METHOD(CandidateLostBeforeConfirmationNeverInfers)
		{
			HdrInferenceTracker tracker;
			const HdrInferenceSignal signal = Hdr10FlagDroppedSignal();
			tracker.OnSignal(signal, 0);
			HdrInferenceSignal menu = signal;
			menu.colorspace = ColorSpace::REC_709;
			const auto update = tracker.OnSignal(menu, 1000);
			Assert::IsTrue(update.event == HdrInferenceEvent::CandidateCleared);
			Assert::IsFalse(update.effectiveChanged);
			tracker.OnSignal(signal, 1200);
			tracker.OnSignal(signal, 2600);
			Assert::IsFalse(tracker.IsInferredPq());   // 1.4 s of the new candidate
			tracker.OnSignal(signal, 2700);
			Assert::IsTrue(tracker.IsInferredPq());
		}

		TEST_METHOD(SdrVetoReturnsToSdrOnlyAtACutAfterTwoShots)
		{
			uint64_t now = 0;
			HdrInferenceTracker tracker = InferredTracker(Hdr10FlagDroppedSignal(), now);
			// Shot 1: one second of SDR evidence. Counted, not armed.
			Assert::IsTrue(Shot(tracker, now, 1000, true) == HdrInferenceEvent::ShotEvidence);
			Assert::AreEqual(1u, tracker.ShotsWithEvidence());
			Assert::IsFalse(tracker.VetoArmed());
			// Shot 2: evidence arms the veto mid-shot, but PQ stays until a cut.
			Assert::IsTrue(Shot(tracker, now, 3000, true) == HdrInferenceEvent::VetoArmed);
			Assert::IsTrue(tracker.VetoArmed());
			Assert::IsTrue(tracker.IsInferredPq());
			// More pictures inside the same shot still do not flip.
			Shot(tracker, now, 2000, true, false);
			Assert::IsTrue(tracker.IsInferredPq());
			// The next cut flips.
			const auto update = tracker.OnPicture(Picture(false, true), now);
			Assert::IsTrue(update.event == HdrInferenceEvent::VetoAtCut);
			Assert::IsTrue(update.effectiveChanged);
			Assert::IsTrue(tracker.State() == HdrInferenceState::VetoedSdr);
			Assert::IsFalse(tracker.WantsPictureStatistics());
		}

		TEST_METHOD(SdrEvidenceInOneShotOrShortBurstsDoesNotVeto)
		{
			uint64_t now = 0;
			HdrInferenceTracker tracker = InferredTracker(LldvSignal(), now);
			// One long shot with evidence (under the no-cut interval).
			Shot(tracker, now, 15000, true);
			Assert::AreEqual(1u, tracker.ShotsWithEvidence());
			// Many shots with only 0.3 s of evidence each.
			for (int i = 0; i < 20; ++i)
			{
				Shot(tracker, now, 300, true);
				Shot(tracker, now, 2000, false, false);
			}
			Assert::AreEqual(1u, tracker.ShotsWithEvidence());
			Assert::IsTrue(tracker.IsInferredPq());
		}

		TEST_METHOD(PqProgrammeWithCutsNeverVetoes)
		{
			uint64_t now = 0;
			HdrInferenceTracker tracker = InferredTracker(Hdr10FlagDroppedSignal(), now);
			for (int shot = 0; shot < 100; ++shot)
				Shot(tracker, now, 3000, false);
			Assert::IsTrue(tracker.IsInferredPq());
			Assert::AreEqual(0u, tracker.ShotsWithEvidence());
		}

		TEST_METHOD(ArmedVetoWithoutACutSwitchesAfterTheFallbackInterval)
		{
			uint64_t now = 0;
			HdrInferenceTracker tracker = InferredTracker(Hdr10FlagDroppedSignal(), now);
			Shot(tracker, now, 1000, true);
			Shot(tracker, now, 1000, true);
			Assert::IsTrue(tracker.VetoArmed());
			const uint64_t armed = now - 500;   // armed 0.5 s into shot 2
			// No pictures and no cut (e.g. a frozen picture): the timer acts.
			Assert::IsTrue(tracker.OnTick(armed + 19000).event == HdrInferenceEvent::None);
			const auto update = tracker.OnTick(armed + 20100);
			Assert::IsTrue(update.event == HdrInferenceEvent::VetoWithoutCut);
			Assert::IsTrue(update.effectiveChanged);
			Assert::IsTrue(tracker.State() == HdrInferenceState::VetoedSdr);
		}

		TEST_METHOD(StaticSdrShotArmsAndSwitchesWithoutACut)
		{
			// A static SDR menu sent as BT.2020: one uncut shot of evidence.
			uint64_t now = 0;
			HdrInferenceTracker tracker = InferredTracker(LldvSignal(), now);
			Shot(tracker, now, 20400, true);
			Assert::IsFalse(tracker.VetoArmed());
			Shot(tracker, now, 400, true, false);
			Assert::IsTrue(tracker.VetoArmed());
			Assert::IsTrue(tracker.IsInferredPq());
			Shot(tracker, now, 21000, true, false);
			Assert::IsTrue(tracker.State() == HdrInferenceState::VetoedSdr);
		}

		TEST_METHOD(AtMostOneFlipPerEpochAndANewEpochInfersAgain)
		{
			uint64_t now = 0;
			const HdrInferenceSignal signal = Hdr10FlagDroppedSignal();
			HdrInferenceTracker tracker = InferredTracker(signal, now);
			Shot(tracker, now, 1000, true);
			Shot(tracker, now, 1000, true);
			Shot(tracker, now, 1000, false);   // the cut flips
			Assert::IsTrue(tracker.State() == HdrInferenceState::VetoedSdr);
			Assert::AreEqual(1u, tracker.EpochFlips());
			const uint64_t epoch = tracker.Epoch();
			// Same signal: stays SDR; PQ-looking pictures cannot flip it back.
			for (int i = 0; i < 10; ++i)
			{
				tracker.OnSignal(signal, now);
				Shot(tracker, now, 3000, false);
				tracker.OnTick(now);
			}
			Assert::IsTrue(tracker.State() == HdrInferenceState::VetoedSdr);
			Assert::AreEqual(1u, tracker.EpochFlips());
			Assert::AreEqual(epoch, tracker.Epoch());
			// A mode change while eligible is a new epoch: stabilise, then PQ.
			HdrInferenceSignal changed = signal;
			changed.signalKey = 0x3333;
			auto update = tracker.OnSignal(changed, now);
			Assert::IsTrue(update.event == HdrInferenceEvent::NewEpoch);
			Assert::AreEqual(epoch + 1, tracker.Epoch());
			Assert::IsTrue(tracker.State() == HdrInferenceState::Waiting);
			Assert::AreEqual(0u, tracker.EpochFlips());
			update = tracker.OnSignal(changed, now + 1500);
			Assert::IsTrue(update.effectiveChanged);
			Assert::IsTrue(tracker.IsInferredPq());
			// The signal leaving and returning is a new epoch too.
			HdrInferenceSignal invalid = changed;
			invalid.valid = false;
			tracker.OnSignal(invalid, now + 2000);
			tracker.OnSignal(changed, now + 2500);
			Assert::AreEqual(epoch + 2, tracker.Epoch());
		}

		TEST_METHOD(ModeChangeDuringPqKeepsPqAndResetsVetoEvidence)
		{
			uint64_t now = 0;
			HdrInferenceSignal signal = LldvSignal();
			HdrInferenceTracker tracker = InferredTracker(signal, now);
			Shot(tracker, now, 1000, true);
			Assert::AreEqual(1u, tracker.ShotsWithEvidence());
			signal.signalKey = 0x4444;
			const auto update = tracker.OnSignal(signal, now);
			Assert::IsTrue(update.event == HdrInferenceEvent::NewEpoch);
			Assert::IsFalse(update.effectiveChanged);
			Assert::IsTrue(tracker.IsInferredPq());
			Assert::AreEqual(0u, tracker.ShotsWithEvidence());
		}

		TEST_METHOD(VetoSubSwitchOffKeepsPq)
		{
			uint64_t now = 0;
			HdrInferenceTracker tracker;
			HdrInferenceOptions options;
			options.sdrVeto = false;
			tracker.SetOptions(options);
			const HdrInferenceSignal signal = Hdr10FlagDroppedSignal();
			tracker.OnSignal(signal, now);
			now += 1500;
			tracker.OnSignal(signal, now);
			Assert::IsTrue(tracker.IsInferredPq());
			Assert::IsFalse(tracker.WantsPictureStatistics());
			for (int i = 0; i < 10; ++i)
				Shot(tracker, now, 2000, true);
			tracker.OnTick(now + 60000);
			Assert::IsTrue(tracker.IsInferredPq());
		}

		TEST_METHOD(VetoStatisticsSeparateSdrFromSyntheticPq)
		{
			int sdrEvidence = 0, pqEvidence = 0, scenes = 0;
			for (uint32_t seed = 1; seed <= 60; ++seed)
			{
				const auto linear = SceneLinear(seed, 1.0 + (seed % 3) * 0.6, 0.01);
				const auto sdr = Measure(SdrGrid(linear));
				Assert::IsTrue(sdr.activePicture);
				sdrEvidence += sdr.sdrEvidence ? 1 : 0;
				for (const auto& pq : { Measure(PqGrid(linear, 203.0, 1000.0)),
					Measure(PqGrid(linear, 100.0, 1000.0)),
					Measure(PqGrid(linear, 203.0, 4000.0)),
					Measure(PqGrid(linear, 400.0, 1000.0)) })
				{
					pqEvidence += pq.sdrEvidence ? 1 : 0;
					Assert::IsTrue(pq.p999 < 870);
				}
				++scenes;
			}
			// Every SDR scene with 1% clipped highlights is SDR evidence; no
			// PQ re-encoding of the same scenes is.
			Assert::AreEqual(scenes, sdrEvidence);
			Assert::AreEqual(0, pqEvidence);
		}

		TEST_METHOD(VetoStatisticsThresholdsAndLetterboxCrop)
		{
			// 0.2% of the active area at 940 is evidence; 0.1% is not.
			std::vector<uint16_t> grid(COLUMNS * ROWS, 500);
			for (int i = 0; i < COLUMNS * ROWS / 1000; ++i)
				grid[i * 997 % grid.size()] = 940;
			auto stats = Measure(grid);
			Assert::IsTrue(stats.activePicture);
			Assert::IsFalse(stats.sdrEvidence);
			for (int i = 0; i < COLUMNS * ROWS / 400; ++i)
				grid[(i * 7919 + 13) % grid.size()] = 940;
			stats = Measure(grid);
			Assert::IsTrue(stats.sdrEvidence);
			// Letterbox: the same highlights over a 2.39 picture inside black
			// bars count against the active rows only.
			const auto boxed = SdrGrid(SceneLinear(7, 1.0, 0.002, 0, 12));
			const auto boxedStats = Measure(boxed);
			Assert::IsTrue(boxedStats.activePicture);
			Assert::IsTrue(boxedStats.activeSamples < boxedStats.samples);
			Assert::IsTrue(boxedStats.fractionAtHigh >= 0.002);
			// Black: not judged.
			const std::vector<uint16_t> black(COLUMNS * ROWS, 64);
			const auto blackStats = Measure(black);
			Assert::IsTrue(blackStats.measured);
			Assert::IsFalse(blackStats.activePicture);
			Assert::IsFalse(blackStats.sdrEvidence);
		}

		TEST_METHOD(CutDetectorSeparatesShotsFromMotion)
		{
			HdrVetoCutDetector detector;
			Assert::IsFalse(detector.Observe(Measure(SdrGrid(SceneLinear(3, 1.0, 0.01, 0)))));
			for (int shift = 1; shift <= 20; ++shift)
				Assert::IsFalse(detector.Observe(Measure(SdrGrid(SceneLinear(3, 1.0, 0.01, shift)))));
			int cuts = 0;
			for (uint32_t seed = 10; seed < 40; ++seed)
			{
				const double exposure = (seed % 2) ? 0.5 : 2.0;
				cuts += detector.Observe(Measure(SdrGrid(SceneLinear(seed, exposure, 0.01)))) ? 1 : 0;
			}
			Assert::IsTrue(cuts >= 25, std::to_wstring(cuts).c_str());
			// Entering and leaving black are boundaries.
			const std::vector<uint16_t> black(COLUMNS * ROWS, 64);
			Assert::IsTrue(detector.Observe(Measure(black)));
			Assert::IsFalse(detector.Observe(Measure(black)));
			Assert::IsTrue(detector.Observe(Measure(SdrGrid(SceneLinear(5, 1.0, 0.01)))));
		}

		TEST_METHOD(SamplerReadsP010GridInTheLimitedDomain)
		{
			const int width = 320, height = 180;
			std::vector<uint16_t> pixels(static_cast<size_t>(width) * height * 2, 512 << 6);
			for (int y = 0; y < height; ++y)
				for (int x = 0; x < width; ++x)
					pixels[static_cast<size_t>(y) * width + x] =
						static_cast<uint16_t>((x < width / 2 ? 300 : 940) << 6);
			AnalysisLumaSource source;
			source.data = reinterpret_cast<const uint8_t*>(pixels.data());
			source.dataBytes = pixels.size() * sizeof(uint16_t);
			source.width = width;
			source.height = height;
			source.rowBytes = width * sizeof(uint16_t);
			source.chromaRowBytes = width * sizeof(uint16_t);
			source.format = AnalysisLumaFormat::P010;
			std::vector<uint16_t> grid;
			Assert::IsTrue(SampleHdrVetoGrid(source, COLUMNS, ROWS, grid));
			Assert::AreEqual(static_cast<size_t>(COLUMNS * ROWS), grid.size());
			Assert::AreEqual(static_cast<uint16_t>(300), grid[0]);
			Assert::AreEqual(static_cast<uint16_t>(940), grid[COLUMNS - 1]);
			Assert::IsTrue(Measure(grid).sdrEvidence);
		}

		// Bug hunt 3 (C2): with the veto armed, the cut frame itself is the
		// first one queued as SDR - at every source rate, never 1-5 frames
		// after it. The capture path asks the gate for each frame before the
		// renderer has it (WantsSample, Observe); a returned state is pushed
		// before that frame is queued.
		TEST_METHOD(ArmedVetoSwitchesOnTheCutFrameAtEveryRate)
		{
			const double rates[] = { 24000.0 / 1001.0, 24.0, 25.0, 30000.0 / 1001.0, 50.0, 60000.0 / 1001.0, 120.0 };
			for (const double rate : rates)
				for (const int cutFrame : { 1, 2, 3, 7, 13 })
				{
					HdrVetoCaptureGate gate;
					VideoStateComPtr sdr(new VideoState());
					sdr->valid = true;
					sdr->eotf = EOTF::SDR;
					const uint64_t generation = 7;
					// Shot A with a slow pan before the veto arms (measured at 20 Hz).
					int frame = 0;
					auto nowMs = [&](int n) { return static_cast<uint64_t>(std::floor(1000.0 + n * 1000.0 / rate)); };
					for (; frame < 30; ++frame)
						if (gate.WantsSample(generation, nowMs(frame)))
							Assert::IsFalse(gate.Observe(generation, Measure(SdrGrid(SceneLinear(3, 1.0, 0.01, frame / 3)))).cut);
					gate.Arm(generation, sdr);
					int switched = -1;
					for (int n = 0; n < 40; ++n, ++frame)
					{
						if (switched < 0)
							Assert::IsTrue(gate.WantsSample(generation, nowMs(frame)), L"armed: every frame is measured");
						else if (!gate.WantsSample(generation, nowMs(frame)))
							continue;   // spent: back to the 20 Hz cadence
						const std::vector<double> scene = n < cutFrame ? SceneLinear(3, 1.0, 0.01, frame / 3) :
							SceneLinear(29, 2.0, 0.01, (n - cutFrame) / 3);
						const HdrVetoCaptureGate::Observation observed =
							gate.Observe(generation, Measure(SdrGrid(scene)));
						if (observed.switchState)
						{
							Assert::AreEqual(-1, switched, L"one switch per arming");
							Assert::IsTrue(observed.cut);
							Assert::IsTrue(observed.switchState->eotf == EOTF::SDR);
							switched = n;
						}
					}
					Assert::AreEqual(cutFrame, switched, (L"switch frame at " + std::to_wstring(rate) + L" Hz").c_str());
					Assert::IsFalse(gate.Armed(generation), L"spent");
				}
		}

		TEST_METHOD(VetoGateNeverSwitchesUnarmedDisarmedOrForAnotherGeneration)
		{
			const auto cutBetween = [](HdrVetoCaptureGate& gate, uint64_t generation)
			{
				gate.WantsSample(generation, 1000);
				gate.Observe(generation, Measure(SdrGrid(SceneLinear(3, 1.0, 0.01))));
				gate.WantsSample(generation, 2000);
				return gate.Observe(generation, Measure(SdrGrid(SceneLinear(29, 2.0, 0.01))));
			};
			VideoStateComPtr sdr(new VideoState());
			{
				HdrVetoCaptureGate gate;   // never armed
				const HdrVetoCaptureGate::Observation o = cutBetween(gate, 1);
				Assert::IsTrue(o.cut);
				Assert::IsFalse(o.switchState != nullptr);
			}
			{
				HdrVetoCaptureGate gate;
				gate.Arm(1, sdr);
				gate.Disarm();
				Assert::IsFalse(cutBetween(gate, 1).switchState != nullptr, L"disarmed");
			}
			{
				HdrVetoCaptureGate gate;
				gate.Arm(1, sdr);
				Assert::IsFalse(cutBetween(gate, 2).switchState != nullptr, L"another measuring period");
				Assert::IsFalse(gate.Armed(2));
			}
			{
				HdrVetoCaptureGate gate;
				gate.Arm(1, nullptr);
				Assert::IsFalse(gate.Armed(1), L"no state: not armed");
			}
			// Not armed: the 20 Hz cadence for the shot evidence, as before.
			HdrVetoCaptureGate gate;
			Assert::IsTrue(gate.WantsSample(3, 1000));
			Assert::IsFalse(gate.WantsSample(3, 1016));
			Assert::IsFalse(gate.WantsSample(3, 1033));
			Assert::IsTrue(gate.WantsSample(3, 1050));
		}

		// Bug hunt 3 (C6): a container override to a non-BT.2020 colorspace
		// stops the inference, as an explicit Rec.709 hdr_colorspace does.
		TEST_METHOD(ContainerOverrideToRec709ForbidsTheInference)
		{
			Assert::IsTrue(HdrSettingsPermitInference(false, ColorSpace::UNKNOWN), L"follow input");
			Assert::IsTrue(HdrSettingsPermitInference(false, ColorSpace::BT_2020));
			Assert::IsFalse(HdrSettingsPermitInference(false, ColorSpace::REC_709), L"container Rec.709");
			Assert::IsFalse(HdrSettingsPermitInference(false, ColorSpace::P3_D65));
			Assert::IsFalse(HdrSettingsPermitInference(true, ColorSpace::UNKNOWN), L"hdr_colorspace Rec.709");
			HdrInferenceSignal signal = LldvSignal();
			signal.explicitSettingsForbid = !HdrSettingsPermitInference(false, ColorSpace::REC_709);
			HdrInferenceTracker tracker;
			uint64_t now = 0;
			tracker.OnSignal(signal, now);
			now += 5000;
			tracker.OnSignal(signal, now);
			Assert::IsFalse(tracker.IsInferredPq(), L"never PQ under a Rec.709 container");
		}

		TEST_METHOD(AviInfoFrameBytesDecodeBt2020)
		{
			// PB1: Y=0 (RGB); PB2: C=3 (extended); PB3: EC=6 (BT.2020 RGB/YCC).
			const uint8_t rgb2020[3] = { 0x00, 0xC0, 0x60 };
			auto avi = DecodeAviInfoFrameBytes(rgb2020, 3);
			Assert::IsTrue(avi.valid && avi.bt2020);
			Assert::AreEqual(0, avi.pixelEncoding);
			const uint8_t ycc2020cl[3] = { 0x40, 0xC8, 0x50 };   // Y=2, EC=5
			avi = DecodeAviInfoFrameBytes(ycc2020cl, 3);
			Assert::IsTrue(avi.bt2020);
			Assert::AreEqual(2, avi.pixelEncoding);
			const uint8_t bt709[3] = { 0x00, 0x80, 0x00 };       // C=2
			Assert::IsFalse(DecodeAviInfoFrameBytes(bt709, 3).bt2020);
			const uint8_t xvycc[3] = { 0x00, 0xC0, 0x10 };       // EC=1
			Assert::IsFalse(DecodeAviInfoFrameBytes(xvycc, 3).bt2020);
			Assert::IsFalse(DecodeAviInfoFrameBytes(rgb2020, 2).valid);
		}

		// Env-gated: VP_TEST_REAL_FOOTAGE=<folder with the D:\hfr clips>.
		TEST_METHOD(HdrInferenceRealClipVeto)
		{
			char* folderValue = nullptr;
			size_t folderLength = 0;
			std::string folder;
			if (_dupenv_s(&folderValue, &folderLength, "VP_TEST_REAL_FOOTAGE") == 0 &&
				folderValue)
				folder = folderValue;
			free(folderValue);
			if (folder.empty())
			{
				Logger::WriteMessage("HdrInferenceRealClipVeto: VP_TEST_REAL_FOOTAGE not set; skipped");
				return;
			}
			struct Clip { std::string file; bool y4m; double fps; };
			const std::vector<Clip> clips = {
				{ "Bosphorus_3840x2160_120fps_420_10bit_YUV.yuv", false, 120.0 },
				{ "crowd_run_2160p50.y4m", true, 50.0 },
				{ "ReadySetGo_3840x2160_120fps_420_10bit_YUV.yuv", false, 120.0 },
				{ "Jockey_3840x2160_120fps_420_10bit_YUV.yuv", false, 120.0 },
				{ "YachtRide_3840x2160_120fps_420_10bit_YUV.yuv", false, 120.0 },
				{ "park_joy_2160p50.y4m", true, 50.0 },
				{ "ShakeNDry_3840x2160_120fps_420_10bit_YUV.yuv", false, 120.0 },
			};
			const int W = 3840, H = 2160;
			// Sampled luma grid of one frame, read row by row from the file.
			auto readGrid = [&](const Clip& clip, std::ifstream& file,
				std::streamoff headerBytes, int frame, std::vector<uint16_t>& grid)
			{
				grid.assign(COLUMNS * ROWS, 0);
				const std::streamoff frameBytes = clip.y4m ?
					static_cast<std::streamoff>(W) * H * 3 / 2 + 6 :
					static_cast<std::streamoff>(W) * H * 3;
				const std::streamoff lumaStart = headerBytes + frame * frameBytes +
					(clip.y4m ? 6 : 0);
				const int bytesPerSample = clip.y4m ? 1 : 2;
				std::vector<uint8_t> row(static_cast<size_t>(W) * bytesPerSample);
				for (int gy = 0; gy < ROWS; ++gy)
				{
					const int y = (2 * gy + 1) * H / (2 * ROWS);
					file.clear();
					file.seekg(lumaStart + static_cast<std::streamoff>(y) * W * bytesPerSample);
					if (!file.read(reinterpret_cast<char*>(row.data()), row.size()))
						return false;
					for (int gx = 0; gx < COLUMNS; ++gx)
					{
						const int x = (2 * gx + 1) * W / (2 * COLUMNS);
						grid[static_cast<size_t>(gy) * COLUMNS + gx] = clip.y4m ?
							static_cast<uint16_t>(row[x] << 2) :
							static_cast<uint16_t>(row[2 * x] | (row[2 * x + 1] << 8));
					}
				}
				return true;
			};
			// Luma-only synthetic PQ of the same frame (SDR white at 203 nits,
			// highlights stretched to 1000).
			auto toPq = [](const std::vector<uint16_t>& sdr)
			{
				std::vector<uint16_t> pq(sdr.size());
				for (size_t i = 0; i < sdr.size(); ++i)
				{
					const double v = std::min(std::max((sdr[i] - 64.0) / 876.0, 0.0), 1.0);
					pq[i] = PqCode(Expand(std::pow(v, 2.4), 203.0, 1000.0));
				}
				return pq;
			};

			for (const bool pqProgramme : { false, true })
			{
				uint64_t now = 0;
				HdrInferenceTracker tracker = InferredTracker(Hdr10FlagDroppedSignal(), now);
				HdrVetoCutDetector detector;
				int shots = 0, cuts = 0, evidenceFrames = 0, frames = 0;
				uint64_t vetoMs = 0;
				for (int round = 0; round < 2 && tracker.IsInferredPq(); ++round)
					for (size_t c = 0; c < clips.size() && tracker.IsInferredPq(); ++c)
					{
						const Clip& clip = clips[c];
						std::ifstream file(folder + "\\" + clip.file, std::ios::binary);
						if (!file)
						{
							Logger::WriteMessage(("HdrInferenceRealClipVeto: missing " + clip.file).c_str());
							continue;
						}
						std::streamoff header = 0;
						if (clip.y4m)
						{
							std::string line;
							std::getline(file, line);
							header = static_cast<std::streamoff>(line.size() + 1);
						}
						++shots;
						// A 3 s shot at 20 Hz, starting at a different point each round.
						for (int i = 0; i < 60 && tracker.IsInferredPq(); ++i)
						{
							const int frame = static_cast<int>(
								(round * 3.0 + i * 0.05) * clip.fps);
							std::vector<uint16_t> grid;
							if (!readGrid(clip, file, header, frame, grid))
								break;
							if (pqProgramme)
								grid = toPq(grid);
							const HdrVetoFrameStats stats = Measure(grid);
							HdrPictureObservation picture;
							picture.measured = stats.measured;
							picture.activePicture = stats.activePicture;
							picture.sdrEvidence = stats.sdrEvidence;
							picture.cut = detector.Observe(stats);
							cuts += picture.cut ? 1 : 0;
							evidenceFrames += stats.sdrEvidence ? 1 : 0;
							++frames;
							tracker.OnPicture(picture, now);
							if (!tracker.IsInferredPq() && vetoMs == 0)
								vetoMs = now;
							now += 50;
						}
					}
				char line[256];
				_snprintf_s(line, _TRUNCATE,
					"HdrInferenceRealClipVeto %s: shots=%d cuts=%d frames=%d sdr_evidence_frames=%d shots_with_evidence=%u state=%s veto_at_ms=%llu",
					pqProgramme ? "synthetic PQ" : "SDR", shots, cuts, frames,
					evidenceFrames, tracker.ShotsWithEvidence(), ToString(tracker.State()),
					static_cast<unsigned long long>(vetoMs));
				Logger::WriteMessage(line);
				if (shots == 0)
					return;
				// Within-shot motion must not register as cuts.
				Assert::IsTrue(cuts <= shots + 1, L"false cuts inside shots");
				if (pqProgramme)
				{
					Assert::IsTrue(tracker.IsInferredPq(), L"synthetic PQ was vetoed");
					Assert::AreEqual(0, evidenceFrames);
				}
				else
					Assert::IsTrue(tracker.State() == HdrInferenceState::VetoedSdr,
						L"SDR programme was not vetoed");
			}
		}
	};
}
