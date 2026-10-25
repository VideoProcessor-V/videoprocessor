#include "pch.h"
#include "CppUnitTest.h"

// WP-23 addendum: ONE luma decode per frame (AnalysisLumaSource::sharedLuma, filled by the frame
// analysis producer) read by every analyser. Byte-identical DECISIONS are required for every
// consumer: each runs on the same recorded inputs (synthetic frame sequences in every format that
// needs a decode, a cut in the middle) once with the source as before and once reading the shared
// plane, and every result must be identical. Bill's analysers (SceneDetector, active picture,
// subtitles: the upstreamable SampleLuma patch) and ours (LED look-ahead peak + raw measurement,
// content intent / intent interpreter grids and decisions, HDR veto grid, NVOF half luma).

#include <ActivePictureEvidence.h>
#include <ContentIntentObserver.h>
#include <HdrInference.h>
#include <SceneDetector.h>
#include <SubtitleBoxDetector.h>
#include <VP-V/modules/drive/led_measure/LedLookahead.h>
#include <VP-V/modules/input/source_analysis/VpFrameAnalysis.h>
#include <VP-V/modules/motion/estimation/MotionAheadProducer.h>
#include "SyntheticAnalysisFrames.h"

#include <cstring>
#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace SharedLumaEquivalenceTestsNs
{
	using namespace SyntheticAnalysisFrames;

	constexpr int kFrames = 10;   // a cut at frame 6

	// Formats whose luma is decoded (a shared plane exists). P010/P210 at limited range are read in
	// place by every reader already: nothing to share.
	bool DecodedKind(Kind kind) { return kind != Kind::P010 && kind != Kind::P210; }

	struct Pair
	{
		Frame plain;    // as before WP-23
		Frame shared;   // reads the published shared plane
	};

	Pair MakePair(Kind kind, int frame)
	{
		Pair pair{ Make(kind, frame), Make(kind, frame) };
		VpFrameAnalysis::AttachSharedLuma(pair.shared.source);
		Assert::IsTrue(VpFrameAnalysis::DecodeSharedLuma(pair.shared.source), KindName(kind));
		Assert::IsNotNull(pair.shared.source.SharedLumaRow(0), KindName(kind));
		Assert::IsTrue(pair.plain.source.SharedLumaRow(0) == nullptr);
		return pair;
	}

	std::wstring Where(Kind kind, int frame, const wchar_t* what)
	{
		return std::wstring(KindName(kind)) + L" frame " + std::to_wstring(frame) + L": " + what;
	}

	void SameBounds(const ActivePictureBounds& a, const ActivePictureBounds& b, const std::wstring& where)
	{
		Assert::IsTrue(a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom &&
			a.rasterWidth == b.rasterWidth && a.rasterHeight == b.rasterHeight, where.c_str());
	}

	TEST_CLASS(SharedLumaEquivalenceTests)
	{
	public:
		// ---- Bill's analysers (upstreamable patch docs/design/wp23-bill-analysers.patch) ----

		TEST_METHOD(SceneDetectorDecisionsIdentical)
		{
			for (int k = 0; k < static_cast<int>(Kind::Count); ++k)
			{
				const Kind kind = static_cast<Kind>(k);
				if (!DecodedKind(kind)) continue;
				SceneDetector plain, shared;
				for (int f = 0; f < kFrames; ++f)
				{
					const Pair pair = MakePair(kind, f);
					SceneDetectorInput in;
					in.width = kWidth;
					in.height = kHeight;
					in.sourceSequence = static_cast<uint64_t>(f + 1);
					in.timestamp = f * 417083;
					in.generation = 1;
					in.frameDuration = 417083;
					in.enabled = true;
					in.analysisSource = &pair.plain.source;
					const SceneDetectorResult a = plain.Analyze(in);
					in.analysisSource = &pair.shared.source;
					const SceneDetectorResult b = shared.Analyze(in);
					const std::wstring where = Where(kind, f, L"scene");
					Assert::IsTrue(a.status == b.status && a.safeBoundary == b.safeBoundary && a.eventId == b.eventId &&
						a.eventFramesBack == b.eventFramesBack && a.averageLuma == b.averageLuma &&
						a.nearBlackEntry == b.nearBlackEntry && a.hardCutCandidate == b.hardCutCandidate &&
						a.hardCutConfirmed == b.hardCutConfirmed && a.differenceEvaluated == b.differenceEvaluated &&
						a.immediateAverageLumaDifference == b.immediateAverageLumaDifference &&
						a.changedSampleCount == b.changedSampleCount && a.sampleCount == b.sampleCount, where.c_str());
				}
			}
		}

		TEST_METHOD(SubtitleDetectorDecisionsIdentical)
		{
			for (int k = 0; k < static_cast<int>(Kind::Count); ++k)
			{
				const Kind kind = static_cast<Kind>(k);
				if (!DecodedKind(kind)) continue;
				SubtitleBoxDetector plain, shared;
				for (int f = 0; f < kFrames; ++f)
				{
					const Pair pair = MakePair(kind, f);
					const SubtitleBoxResult a = plain.Analyze(pair.plain.source, kBar, kHeight - kBar, f + 1, 1);
					const SubtitleBoxResult b = shared.Analyze(pair.shared.source, kBar, kHeight - kBar, f + 1, 1);
					const std::wstring where = Where(kind, f, L"subtitles");
					Assert::IsTrue(a.detected == b.detected && a.held == b.held && a.revised == b.revised &&
						a.workLimit == b.workLimit && a.lineCount == b.lineCount && a.cue == b.cue &&
						a.observations == b.observations && a.signature == b.signature &&
						a.anchorSignature == b.anchorSignature &&
						a.nearBarEligibilityMeasured == b.nearBarEligibilityMeasured &&
						a.nearBarEligible == b.nearBarEligible && a.capturePanelMeasured == b.capturePanelMeasured &&
						a.diagnosticComponentCount == b.diagnosticComponentCount &&
						a.diagnosticNearBarGap == b.diagnosticNearBarGap &&
						std::string(a.diagnosticReason ? a.diagnosticReason : "") ==
						std::string(b.diagnosticReason ? b.diagnosticReason : ""), where.c_str());
					Assert::IsTrue(std::memcmp(&a.bounds, &b.bounds, sizeof(a.bounds)) == 0 &&
						std::memcmp(&a.sourcePanel, &b.sourcePanel, sizeof(a.sourcePanel)) == 0 &&
						std::memcmp(&a.capturePanel, &b.capturePanel, sizeof(a.capturePanel)) == 0 &&
						std::memcmp(&a.anchor, &b.anchor, sizeof(a.anchor)) == 0 &&
						std::memcmp(a.lineBounds.data(), b.lineBounds.data(), sizeof(a.lineBounds)) == 0, where.c_str());
					Assert::AreEqual(plain.SharedSampleCount(), shared.SharedSampleCount(), where.c_str());
				}
			}
		}

		TEST_METHOD(ActivePictureEvidenceIdentical)
		{
			for (int k = 0; k < static_cast<int>(Kind::Count); ++k)
			{
				const Kind kind = static_cast<Kind>(k);
				if (!DecodedKind(kind)) continue;
				for (int f : { 0, 7 })
				{
					const Pair pair = MakePair(kind, f);
					const ActivePictureEvidence a = ExtractActivePictureEvidence(pair.plain.source);
					const ActivePictureEvidence b = ExtractActivePictureEvidence(pair.shared.source);
					const std::wstring where = Where(kind, f, L"active picture");
					Assert::IsTrue(a.available == b.available && a.classification == b.classification &&
						a.lumaSamples == b.lumaSamples && a.chromaSamples == b.chromaSamples && a.reason == b.reason &&
						a.authorityOrigin == b.authorityOrigin, where.c_str());
					SameBounds(a.proposedBounds, b.proposedBounds, where);
					SameBounds(a.trustedBounds, b.trustedBounds, where);
					const ActivePictureGlobalNearBlackEvidence ga = EvaluateActivePictureGlobalNearBlack(pair.plain.source);
					const ActivePictureGlobalNearBlackEvidence gb = EvaluateActivePictureGlobalNearBlack(pair.shared.source);
					Assert::IsTrue(ga.evaluated == gb.evaluated && ga.lumaP90 == gb.lumaP90 && ga.nearBlack == gb.nearBlack &&
						ga.lumaSamples == gb.lumaSamples, where.c_str());
				}
			}
		}

		// ---- Our consumers ----

		TEST_METHOD(LedLookaheadPeakAndMeasurementIdentical)
		{
			const auto transfer = LedDrive::IdentityRawTransfer();
			for (int k = 0; k < static_cast<int>(Kind::Count); ++k)
			{
				const Kind kind = static_cast<Kind>(k);
				if (!DecodedKind(kind)) continue;
				for (int f : { 1, 8 })
				{
					const Pair pair = MakePair(kind, f);
					const std::wstring where = Where(kind, f, L"LED");
					const auto table = LedDrive::PeakValueTable(pair.plain.source, transfer, -1);
					const double a = LedDrive::FullCoveragePeak(pair.plain.source, 0, kBar, kWidth, kHeight - kBar, table,
						0.0, 0.0, nullptr, 0);
					const double b = LedDrive::FullCoveragePeak(pair.shared.source, 0, kBar, kWidth, kHeight - kBar, table,
						0.0, 0.0, nullptr, 0);
					Assert::IsTrue(a == b && a >= 0.0, where.c_str());
					// With a picture-only mask too (led_picture_only).
					LedDrive::PixelRect mask;
					mask.x0 = 100; mask.y0 = 90; mask.x1 = 220; mask.y1 = 130;
					Assert::IsTrue(LedDrive::FullCoveragePeak(pair.plain.source, 0, kBar, kWidth, kHeight - kBar, table, 0.0, 0.0, &mask, 1) ==
						LedDrive::FullCoveragePeak(pair.shared.source, 0, kBar, kWidth, kHeight - kBar, table, 0.0, 0.0, &mask, 1), where.c_str());
					for (int coverage = 0; coverage < 2; ++coverage)
					{
						LedDrive::RawMeasureOptions options;
						options.peakFullCoverage = coverage != 0;
						QueuedFrameStatistics::PictureRect rect;
						rect.left = 0; rect.top = kBar; rect.right = kWidth; rect.bottom = kHeight - kBar;
						LedDrive::RawMeasurement ma{}, mb{};
						const bool oka = LedDrive::MeasureRawFrame(pair.plain.source, rect, transfer, ma, -1, &options);
						const bool okb = LedDrive::MeasureRawFrame(pair.shared.source, rect, transfer, mb, -1, &options);
						Assert::IsTrue(oka && okb && std::memcmp(&ma, &mb, sizeof(ma)) == 0, where.c_str());
					}
				}
			}
		}

		TEST_METHOD(IntentGridsAndDecisionsIdentical)
		{
			for (int k = 0; k < static_cast<int>(Kind::Count); ++k)
			{
				const Kind kind = static_cast<Kind>(k);
				if (!DecodedKind(kind)) continue;
				ContentIntent::Observer plain, shared;
				plain.Configure(true, 2.0);
				shared.Configure(true, 2.0);
				ContentIntent::FrameContext context;
				context.contentHz = 23.976;
				context.displayHz = 60.0;
				plain.SetFrameContext(context);
				shared.SetFrameContext(context);
				QueuedFrameStatistics::PictureRect picture;
				picture.left = 0; picture.top = kBar; picture.right = kWidth; picture.bottom = kHeight - kBar;
				for (int f = 0; f < kFrames; ++f)
				{
					const Pair pair = MakePair(kind, f);
					const std::wstring where = Where(kind, f, L"intent");
					// The grid both intent observers build (IntentInterpreterObserver uses the same lambda).
					ContentIntent::LumaGrid ga, gb;
					const bool ba = ContentIntent::BuildGrid([&](int x, int y, uint16_t& v)
						{ AnalysisLumaSample s; if (!pair.plain.source.Sample(x, y, s)) return false; v = s.luma; return true; },
						picture.left, picture.top, picture.right, picture.bottom, ga);
					const bool bb = ContentIntent::BuildGrid([&](int x, int y, uint16_t& v)
						{ return pair.shared.source.SampleLuma(x, y, v); },
						picture.left, picture.top, picture.right, picture.bottom, gb);
					Assert::IsTrue(ba && bb && ga.luma == gb.luma && ga.width == gb.width && ga.height == gb.height, where.c_str());
					plain.ObserveFrame(static_cast<uint64_t>(f + 1), 0, pair.plain.source, picture);
					shared.ObserveFrame(static_cast<uint64_t>(f + 1), 0, pair.shared.source, picture);
					const ContentIntentStatus sa = plain.Status(), sb = shared.Status();
					Assert::IsTrue(sa.content == sb.content && sa.source == sb.source && sa.candidate == sb.candidate &&
						sa.candidateSource == sb.candidateSource && sa.motionValid == sb.motionValid &&
						sa.panPictureHeightsPerSecond == sb.panPictureHeightsPerSecond && sa.widePanShare == sb.widePanShare &&
						sa.windowSeconds == sb.windowSeconds, where.c_str());
				}
			}
		}

		TEST_METHOD(HdrVetoGridIdentical)
		{
			for (int k = 0; k < static_cast<int>(Kind::Count); ++k)
			{
				const Kind kind = static_cast<Kind>(k);
				if (!DecodedKind(kind)) continue;
				const Pair pair = MakePair(kind, 4);
				std::vector<uint16_t> a, b;
				Assert::IsTrue(SampleHdrVetoGrid(pair.plain.source, 64, 36, a));
				Assert::IsTrue(SampleHdrVetoGrid(pair.shared.source, 64, 36, b));
				Assert::IsTrue(a == b, Where(kind, 4, L"HDR veto grid").c_str());
			}
		}

		TEST_METHOD(NvofHalfLumaIdentical)
		{
			for (int k = 0; k < static_cast<int>(Kind::Count); ++k)
			{
				const Kind kind = static_cast<Kind>(k);
				if (!DecodedKind(kind)) continue;
				const Pair pair = MakePair(kind, 5);
				std::vector<uint8_t> a, b;
				int wa = 0, ha = 0, wb = 0, hb = 0;
				Assert::IsTrue(MotionAhead::HalfLuma8(pair.plain.source, a, wa, ha));
				Assert::IsTrue(MotionAhead::HalfLuma8(pair.shared.source, b, wb, hb));
				Assert::IsTrue(wa == wb && ha == hb && a == b, Where(kind, 5, L"NVOF half luma").c_str());
			}
		}

		TEST_METHOD(UnpublishedPlaneChangesNothing)
		{
			// A plane attached but not decoded yet (the producer still busy): readers decode as before.
			Frame plain = Make(Kind::R12L, 3), pending = Make(Kind::R12L, 3);
			VpFrameAnalysis::AttachSharedLuma(pending.source);
			Assert::IsTrue(pending.source.SharedLumaRow(0) == nullptr);
			const auto table = LedDrive::PeakValueTable(plain.source, LedDrive::IdentityRawTransfer(), -1);
			Assert::IsTrue(LedDrive::FullCoveragePeak(plain.source, 0, 0, kWidth, kHeight, table, 0, 0, nullptr, 0) ==
				LedDrive::FullCoveragePeak(pending.source, 0, 0, kWidth, kHeight, table, 0, 0, nullptr, 0));
			SceneDetector a, b;
			SceneDetectorInput in;
			in.width = kWidth; in.height = kHeight; in.enabled = true; in.generation = 1;
			in.analysisSource = &plain.source;
			const auto ra = a.Analyze(in);
			in.analysisSource = &pending.source;
			const auto rb = b.Analyze(in);
			Assert::IsTrue(ra.averageLuma == rb.averageLuma && ra.status == rb.status);
		}
	};
}
