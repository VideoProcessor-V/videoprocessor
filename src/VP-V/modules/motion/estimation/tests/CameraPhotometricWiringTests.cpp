// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// R3 WIRING (b9ae2c34e, 0dc594f86; audit4 group D, gate-CE-camfix-tests MUST 2): the direct photometric refinement inside the per-pair
// fit and its link surfaces. The fit side: the outcome per path (no luma, the resolution rule at two frame heights and two forms),
// a refined model replaces the vector model and the record's signals are rebuilt on it, a fall-back keeps the vector model, and a pair
// without a PhotometricInput (or with a skipped one) gives the byte-identical record. The link side: one counter per outcome, the
// "cpu reference" status line with the four GPU failure counters, the two decisions' engage rules, the line in CollectStatus, and the
// producer passing the luma pair only at its exact size. CPU only.
#include "CameraLinkTestSupport.h"
#include "PhotometricRefineScenes.h"
#include <VP-V/modules/motion/estimation/global_model/PhotometricVerdict.h>
#include <VP-V/modules/motion/estimation/global_model/PhotometricWiring.h>
#include <VP-V/modules/motion/estimation/global_model/link/CameraFitProducer.h>
#include <VP-V/modules/motion/estimation/global_model/link/CameraFitStatus.h>
#include <VP-V/modules/motion/estimation/global_model/link/CameraLinkCore.h>
#include <VP-V/modules/motion/estimation/global_model/link/CameraRecordPool.h>

#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	namespace
	{
		using namespace CameraModel;
		using CameraSynth::Field;
		using CameraSynth::GridSpec;

		// A frame of `frameW` x `frameH` source px on the 16-px grid that covers it (the field grid rows give the rule's frame rows).
		struct Frame
		{
			int frameW, frameH;
			GridSpec Grid() const { GridSpec g; g.w = (frameW + 15) / 16; g.h = (frameH + 15) / 16; g.blockSize = 16; return g; }
			int Rows() const { return Grid().h * 16; }
		};

		// The luma pair of a pan by tx source px at form s (sourcePxPerPixel), rendered from the R3 scene waves.
		struct LumaPair
		{
			std::vector<uint8_t> a, b;
			int w = 0, h = 0;
			double s = 0;
			LumaPair(const Frame& fr, double s_, double tx, double ty, uint64_t seed = 21)
			{
				s = s_;
				w = static_cast<int>(fr.frameW / s);
				h = static_cast<int>(fr.frameH / s);
				const std::vector<R3Scenes::Wave> waves = R3Scenes::MakeWaves(seed, s, 90.0);
				a = R3Scenes::To8(R3Scenes::RenderD(w, h, s, std::vector<R3Scenes::Layer>(1, R3Scenes::Shifted(waves, 0, 0)), 1.0, 0.0, 1, 1.0));
				b = R3Scenes::To8(R3Scenes::RenderD(w, h, s, std::vector<R3Scenes::Layer>(1, R3Scenes::Shifted(waves, tx, ty)), 1.0, 0.0, 2, 1.0));
			}
			LumaPairView View() const { return R3Scenes::View8(a, b, w, h, s); }
		};

		// The vector field of the pan (tx, ty) on the frame's grid, light noise, picture = the frame.
		Field PanField(const Frame& fr, double tx, double ty)
		{
			Field f(fr.Grid(), R3Scenes::Pic(fr.frameW, fr.frameH));
			f.FromModel(CameraSynth::PanModel(tx, ty));
			f.AddNoise(0.05, 4242);
			return f;
		}

		// One fit of the field's pair, with or without a PhotometricInput (fresh fitter: no history).
		CameraMotion FitOnce(const Field& f, PhotometricInput* photo)
		{
			Fitter fitter;
			PairInput in = f.Input(1.0 / 24, 5);
			in.photometric = photo;
			return fitter.Fit(in);
		}

		std::vector<uint8_t> Pod(const CameraMotion& r)
		{
			std::vector<uint8_t> b(CameraLink::kRecordPodMaxBytes, 0);
			const size_t n = CameraLink::EncodeRecordPod(r, b.data(), b.size());
			b.resize(n);
			return b;
		}
	}

	TEST_CLASS(CameraPhotometricWiringTests)
	{
	public:
		TEST_METHOD(TheOutcomeFollowsTheLumaAndTheResolutionRulePerPath)
		{
			const Frame hd{ 1920, 1080 }, uhd{ 3840, 2160 };
			// The rule itself, from its own constants (no resolution literal in the product): s 4 fails at the 1080 grid's rows and passes at
			// 2160; s 2 passes at 1080.
			Assert::IsFalse(PhotometricFormPasses(4.0, hd.Rows()), L"premise: the quarter form is too coarse at 1080");
			Assert::IsTrue(PhotometricFormPasses(4.0, uhd.Rows()), L"premise: the quarter form is fine at 2160");
			Assert::IsTrue(PhotometricFormPasses(2.0, hd.Rows()), L"premise: the half form is fine at 1080");
			const double tx = 6.0, ty = -2.0;
			// (a) no luma: a PhotometricInput without planes.
			{
				const Field f = PanField(hd, tx, ty);
				PhotometricInput photo;
				FitOnce(f, &photo);
				Assert::IsTrue(photo.outcome == PhotometricOutcome::SkippedNoLuma, L"no luma pair: skipped (no luma)");
			}
			// (b) s 4 at 1080 rows: the resolution rule skips it.
			{
				const Field f = PanField(hd, tx, ty);
				const LumaPair lp(hd, 4.0, tx, ty);
				PhotometricInput photo;
				photo.luma = lp.View();
				FitOnce(f, &photo);
				Assert::IsTrue(photo.outcome == PhotometricOutcome::SkippedResolution, L"quarter form at 1080: skipped (resolution)");
			}
			// (c) s 4 at 2160 rows and (d) s 2 at 1080 rows: vectors biased by 0.6 px (strictly inside R3's
			// 3 * 0.25 = 0.75 px PhotometricVerdict agreement gate) must actively refine back to the true luma pan
			// while the same-input vector-only negative control remains stuck at the biased pan.
			const double bias = 0.6;
			{
				const Field f = PanField(uhd, tx + bias, ty);
				const CameraMotion vecOnly = FitOnce(f, nullptr);
				Assert::IsTrue(vecOnly.valid && std::fabs(vecOnly.params.h[2] - tx) > 0.4,
					L"same-input negative control (4K): vector-only fit stays stuck on biased pan");
				const LumaPair lp(uhd, 4.0, tx, ty);
				PhotometricInput photo;
				photo.luma = lp.View();
				const CameraMotion r = FitOnce(f, &photo);
				Assert::IsTrue(photo.outcome == PhotometricOutcome::Refined,
					(L"quarter form at 2160 must refine biased vectors, outcome " + std::wstring(1, L'0' + int(photo.outcome))).c_str());
				Assert::IsTrue(photo.result.status == RefineStatus::Refined,
					L"quarter form at 2160 must report RefineStatus::Refined");
				Assert::IsTrue(r.valid && (r.verdictFlags & kVerdictPhotometricDisagreement) == 0,
					L"quarter form at 2160 agreeing refine must not flag photometric disagreement");
				Assert::IsTrue(std::fabs(r.params.h[2] - tx) < 0.05 && std::fabs(r.params.h[5] - ty) < 0.05,
					L"quarter form at 2160 recovers true luma pan within 0.05 px");
			}
			{
				const Field f = PanField(hd, tx + bias, ty);
				const CameraMotion vecOnly = FitOnce(f, nullptr);
				Assert::IsTrue(vecOnly.valid && std::fabs(vecOnly.params.h[2] - tx) > 0.4,
					L"same-input negative control (1080): vector-only fit stays stuck on biased pan");
				const LumaPair lp(hd, 2.0, tx, ty);
				PhotometricInput photo;
				photo.luma = lp.View();
				const CameraMotion r = FitOnce(f, &photo);
				Assert::IsTrue(photo.outcome == PhotometricOutcome::Refined,
					(L"half form at 1080 must refine biased vectors, outcome " + std::wstring(1, L'0' + int(photo.outcome))).c_str());
				Assert::IsTrue(photo.result.status == RefineStatus::Refined,
					L"half form at 1080 must report RefineStatus::Refined");
				Assert::IsTrue(r.valid && (r.verdictFlags & kVerdictPhotometricDisagreement) == 0,
					L"half form at 1080 agreeing refine must not flag photometric disagreement");
				Assert::IsTrue(std::fabs(r.params.h[2] - tx) < 0.05 && std::fabs(r.params.h[5] - ty) < 0.05,
					L"half form at 1080 recovers true luma pan within 0.05 px");
			}
		}

		TEST_METHOD(ARefinedModelReplacesTheVectorModelAndTheSignalsAreRebuiltOnIt)
		{
			// The vectors say the pan is 0.6 px longer than the luma shows: the refine moves the model to the luma's pan.
			const Frame hd{ 1920, 1080 };
			const double tx = 6.0, ty = -2.0, bias = 0.6;
			const Field f = PanField(hd, tx + bias, ty);
			const CameraMotion vectorOnly = FitOnce(f, nullptr);
			Assert::IsTrue(vectorOnly.valid && std::fabs(vectorOnly.params.h[2] - (tx + bias)) < 0.1, L"premise: the vector fit takes the biased pan");
			const LumaPair lp(hd, 2.0, tx, ty);
			PhotometricInput photo;
			photo.luma = lp.View();
			const CameraMotion r = FitOnce(f, &photo);
			Assert::IsTrue(photo.outcome == PhotometricOutcome::Refined, L"the refine is applied");
			Assert::IsTrue(photo.result.status == RefineStatus::Refined);
			Assert::IsTrue(std::fabs(r.params.h[2] - tx) < 0.1 && std::fabs(r.params.h[5] - ty) < 0.1, L"the record carries the refined (luma) model");
			Assert::IsTrue(std::fabs(r.params.h[2] - photo.result.model.h[2]) < 1e-9, L"exactly the refine's model");
			// The signals are rebuilt on the refined model: against the biased vectors its residuals are larger than the vector model's, so
			// the precision signal drops (built from the vector model it would be unchanged).
			Assert::IsTrue(r.signals.precision < vectorOnly.signals.precision,
				(L"the precision signal is rebuilt on the refined model (" + std::to_wstring(r.signals.precision) + L" vs "
					+ std::to_wstring(vectorOnly.signals.precision) + L")").c_str());
		}

		TEST_METHOD(AFallBackKeepsTheVectorModelAndTheRecord)
		{
			// The luma pair moves the OTHER way (a pan the vectors do not show): the verification refuses the warp,
			// keeps the vector model bit-for-bit, and visibly demotes the record via kVerdictPhotometricDisagreement (CE-WATER-C3).
			const Frame hd{ 1920, 1080 };
			const double tx = 6.0, ty = -2.0;
			const Field f = PanField(hd, tx, ty);
			const CameraMotion vectorOnly = FitOnce(f, nullptr);
			Assert::IsTrue(vectorOnly.valid && ReasonUsable(vectorOnly.reason) && vectorOnly.confidence >= 0.5,
				L"same-input negative control: vector-only fit is usable without photometric check");
			const LumaPair lp(hd, 2.0, -tx, -ty, 33);
			PhotometricInput photo;
			photo.luma = lp.View();
			const CameraMotion r = FitOnce(f, &photo);
			Assert::IsTrue(photo.outcome == PhotometricOutcome::FellBack, (L"the refine fell back, outcome " + std::wstring(1, L'0' + int(photo.outcome))).c_str());
			Assert::IsTrue(photo.result.status != RefineStatus::Refined, L"its own status says why (not refined)");
			Assert::IsTrue(vectorOnly.params.type == r.params.type, L"the vector model type is kept");
			for (int k = 0; k < 9; ++k)
				Assert::AreEqual(vectorOnly.params.h[k], r.params.h[k], L"the vector model is kept, bit for bit");
			Assert::IsTrue(r.reason == Reason::Ambiguous, L"opposite-direction luma disagreement demotes reason to Ambiguous");
			Assert::IsTrue((r.verdictFlags & kVerdictPhotometricDisagreement) != 0, L"kVerdictPhotometricDisagreement is visibly set");
			Assert::IsFalse(ReasonUsable(r.reason), L"demoted record is not usable");
			Assert::IsTrue(r.confidence < 0.5, L"demoted confidence drops below 0.5");
		}

		TEST_METHOD(WithoutAPhotometricInputOrWithASkipTheRecordIsByteIdentical)
		{
			const Frame hd{ 1920, 1080 };
			const Field f = PanField(hd, 6.0, -2.0);
			const std::vector<uint8_t> none = Pod(FitOnce(f, nullptr));
			Assert::IsTrue(!none.empty(), L"premise: the record encodes");
			PhotometricInput noLuma;
			Assert::IsTrue(Pod(FitOnce(f, &noLuma)) == none, L"a skipped (no luma) refine changes nothing");
			const LumaPair lp(hd, 4.0, 6.0, -2.0);
			PhotometricInput coarse;
			coarse.luma = lp.View();
			Assert::IsTrue(Pod(FitOnce(f, &coarse)) == none, L"a skipped (resolution) refine changes nothing");
			Assert::IsTrue(coarse.outcome == PhotometricOutcome::SkippedResolution, L"premise: it was the resolution skip");
		}

		TEST_METHOD(TheVerdictStubReturnsTheRefinedResultUnchanged)
		{
			// 1. Agreeing sub-3-sigma refine (correctionPx = 0.3 <= 3 * 0.25 = 0.75): passes through unchanged.
			const Params vecModel = R3Scenes::Translation(1.5, -0.5);
			RefineResult in;
			in.status = RefineStatus::Refined;
			in.model = R3Scenes::Translation(1.25, -0.5);
			in.correctionPx = 0.3;
			in.residualBefore = 4.0;
			in.residualAfter = 2.0;
			bool disagreed = true;
			const RefineResult out = PhotometricVerdict(vecModel, in, 0.25, &disagreed);
			Assert::IsFalse(disagreed, L"agreeing refine does not flag disagreement");
			Assert::IsTrue(out.status == in.status);
			for (int k = 0; k < 9; ++k)
				Assert::AreEqual(in.model.h[k], out.model.h[k]);
			Assert::AreEqual(in.correctionPx, out.correctionPx);
			Assert::AreEqual(in.residualBefore, out.residualBefore);
			Assert::AreEqual(in.residualAfter, out.residualAfter);

			// 2. Live PhotometricVerdict (CE-WATER-C3): > 3-sigma geometric disagreement refuses the warp and restores vecModel.
			RefineResult farIn = in;
			farIn.model = R3Scenes::Translation(3.0, -0.5);
			farIn.correctionPx = 1.5;   // > 3 * 0.25 = 0.75 px
			bool farDisagreed = false;
			const RefineResult farOut = PhotometricVerdict(vecModel, farIn, 0.25, &farDisagreed);
			Assert::IsTrue(farDisagreed, L"> 3-sigma geometric correction flags disagreement");
			Assert::IsTrue(farOut.status == RefineStatus::FellBackTooFar, L"discordant warp demoted to FellBackTooFar");
			for (int k = 0; k < 9; ++k)
				Assert::AreEqual(vecModel.h[k], farOut.model.h[k], L"vector model restored bit-for-bit on geometric disagreement");

			// 3. Brightness-constancy disagreement on FellBackNoImprovement above gradient floor vs quantization-floor negative control.
			RefineResult brightFail;
			brightFail.status = RefineStatus::FellBackNoImprovement;
			brightFail.model = vecModel;
			brightFail.residualBefore = 12.0;
			brightFail.residualAfter = 14.0;   // > kPhotometricBrightnessFloorCodes2 (8.0)
			bool brightDisagreed = false;
			(void)PhotometricVerdict(vecModel, brightFail, 0.25, &brightDisagreed);
			Assert::IsTrue(brightDisagreed, L"brightness-constancy failure flags disagreement");

			RefineResult quantNoise = brightFail;
			quantNoise.residualBefore = 0.2;
			quantNoise.residualAfter = 0.25;   // <= kPhotometricQuantFloorCodes2 (1.125)
			bool quantDisagreed = true;
			(void)PhotometricVerdict(vecModel, quantNoise, 0.25, &quantDisagreed);
			Assert::IsFalse(quantDisagreed, L"quantization-floor rounding wobble must not false-trigger disagreement");
		}

		TEST_METHOD(EachOutcomeHasItsCounterAndTheLineAndDecisionsShowThem)
		{
			using O = PhotometricOutcome;
			CameraLink::FitCounters c;
			c.NotePhotometric(O::NotAttempted);
			Assert::AreEqual(uint64_t(0), c.photoRefined.load() + c.photoFellBack.load() + c.photoSkippedNoLuma.load()
				+ c.photoSkippedResolution.load() + c.photoSkippedOther.load(), L"not attempted counts nothing");
			const O outs[] = { O::Refined, O::FellBack, O::SkippedNoLuma, O::SkippedResolution, O::SkippedOther };
			for (O o : outs)
				c.NotePhotometric(o);
			Assert::AreEqual(uint64_t(1), c.photoRefined.load());
			Assert::AreEqual(uint64_t(1), c.photoFellBack.load());
			Assert::AreEqual(uint64_t(1), c.photoSkippedNoLuma.load());
			Assert::AreEqual(uint64_t(1), c.photoSkippedResolution.load());
			Assert::AreEqual(uint64_t(1), c.photoSkippedOther.load());
			const std::string line = CameraLink::PhotometricStatusLine(c);
			Assert::IsTrue(line.find("cpu reference") != std::string::npos, L"the line says the path is the CPU reference");
			for (const char* gpu : { "unavailable", "limit", "ensure", "readback" })
				Assert::IsTrue(line.find(gpu) != std::string::npos, L"the line shows each GPU failure counter");

			auto Engaged = [](const std::vector<FallbackCatalog::Decision>& d, const char* id) {
				for (const FallbackCatalog::Decision& x : d)
					if (x.id == id)
						return x.engaged;
				Assert::Fail(L"decision missing");
				return false;
			};
			// not_running: only when nothing ran and something was skipped, and only with P7 in the graph.
			CameraLink::FitCounters fresh;
			Assert::IsFalse(Engaged(CameraLink::PhotometricDecisions(fresh, true), CameraLink::kIdPhotometricNotRunning), L"nothing at all: released");
			CameraLink::FitCounters skipped;
			skipped.NotePhotometric(O::SkippedNoLuma);
			Assert::IsTrue(Engaged(CameraLink::PhotometricDecisions(skipped, true), CameraLink::kIdPhotometricNotRunning), L"only skips: engaged");
			Assert::IsFalse(Engaged(CameraLink::PhotometricDecisions(skipped, false), CameraLink::kIdPhotometricNotRunning), L"no P7 in the graph: released");
			skipped.NotePhotometric(O::FellBack);
			Assert::IsFalse(Engaged(CameraLink::PhotometricDecisions(skipped, true), CameraLink::kIdPhotometricNotRunning), L"one ran: released");
			// gpu_failed: when any GPU failure counter is non-zero (each one alone), with P7 in the graph.
			Assert::IsFalse(Engaged(CameraLink::PhotometricDecisions(fresh, true), CameraLink::kIdPhotometricGpuFailed));
			std::atomic<uint64_t> CameraLink::FitCounters::* const gpuCounters[] = { &CameraLink::FitCounters::photoGpuUnavailable,
				&CameraLink::FitCounters::photoGpuLimit, &CameraLink::FitCounters::photoGpuEnsure, &CameraLink::FitCounters::photoGpuReadback };
			for (auto m : gpuCounters)
			{
				CameraLink::FitCounters g;
				(g.*m).store(1);
				Assert::IsTrue(Engaged(CameraLink::PhotometricDecisions(g, true), CameraLink::kIdPhotometricGpuFailed), L"a GPU failure engages it");
				Assert::IsFalse(Engaged(CameraLink::PhotometricDecisions(g, false), CameraLink::kIdPhotometricGpuFailed), L"not without P7");
			}
		}

		TEST_METHOD(TheActiveLinkCollectsThePhotometricLine)
		{
			CameraLinkTest::Rig rig;
			FrameAnalysis::Store store;
			CameraLink::CameraLinkCore core(store);
			CameraLink::CameraLinkSettings s;
			s.p7Wanted = true;
			s.deterministic = true;
			core.OnSettings(s);
			core.AttachGraph(rig.table.get(), rig.bytes.get(), rig.indicator.get());
			std::vector<std::string> lines;
			core.CollectStatus(lines);
			const std::string expected = CameraLink::PhotometricStatusLine(core.Counters());
			bool found = false;
			for (const std::string& l : lines)
				found = found || l == expected;
			Assert::IsTrue(found, L"CollectStatus carries the photometric line while the link is active");
		}

		TEST_METHOD(TheProducerPassesTheLumaPairOnlyAtItsExactSize)
		{
			// The 4K pan job of the link tests (240 x 135 blocks, 2160 rows): vectors are biased by +0.6 px (10.6 vs true
			// luma pan 10.0, strictly inside R3's 3 * 0.25 = 0.75 px PhotometricVerdict agreement gate). A quarter-form luma
			// pair of exact size 960 x 540 must actively refine (photoRefined == 1, photoFellBack == 0, h[2] ~= 10.0); the
			// same-input vector-only control and mismatched-size / zero-step planes skip luma and stay stuck at ~10.6.
			const Frame uhd{ 3840, 2160 };
			const double trueTx = 10.0, bias = 0.6;
			const LumaPair lp(uhd, 4.0, trueTx, 0.0);
			auto from = std::make_shared<const std::vector<uint8_t>>(lp.a), to = std::make_shared<const std::vector<uint8_t>>(lp.b);
			const CameraLink::CameraFitJob baseJob = CameraLinkTest::PanJob(1, trueTx + bias);
			auto run = [&](bool attachLuma, int w, int h, double step, CameraLink::FitCounters& counters) -> CameraLink::CameraFitResult {
				CameraLink::CameraFitProducer producer(CameraModel::Config(), &counters);
				CameraLink::TimingFacts tf;
				tf.measuredSourceHz = 24.0;
				producer.SetTimingFacts(tf);
				producer.NoteTiming();
				CameraLink::CameraFitJob job = baseJob;
				if (attachLuma)
				{
					job.lumaFrom = from;
					job.lumaTo = to;
					job.lumaW = w;
					job.lumaH = h;
					job.lumaStep = step;
				}
				return producer.Fit(job);
			};
			CameraLink::FitCounters vecOnlyCounters;
			const CameraLink::CameraFitResult vecOnlyRes = run(false, 0, 0, 0.0, vecOnlyCounters);
			Assert::AreEqual(uint64_t(1), vecOnlyCounters.photoSkippedNoLuma.load(), L"no luma attached: counted as no luma");
			Assert::AreEqual(uint64_t(0), vecOnlyCounters.photoRefined.load());
			Assert::AreEqual(uint64_t(0), vecOnlyCounters.photoFellBack.load());
			Assert::IsTrue(vecOnlyRes.record != nullptr && vecOnlyRes.record->valid
				&& std::fabs(vecOnlyRes.record->params.h[2] - trueTx) > 0.4,
				L"same-input negative control: producer without luma stays stuck at biased pan ~10.6");

			CameraLink::FitCounters exact;
			const CameraLink::CameraFitResult exactRes = run(true, lp.w, lp.h, lp.s, exact);
			Assert::AreEqual(uint64_t(1), exact.photoRefined.load(), L"the exact-size biased pair must be refined");
			Assert::AreEqual(uint64_t(0), exact.photoFellBack.load(), L"the exact-size biased pair must not fall back");
			Assert::AreEqual(uint64_t(0), exact.photoSkippedNoLuma.load());
			Assert::IsTrue(exactRes.record != nullptr && exactRes.record->valid
				&& (exactRes.record->verdictFlags & kVerdictPhotometricDisagreement) == 0,
				L"exact-size agreeing refine produces a valid record without disagreement");
			Assert::IsTrue(std::fabs(exactRes.record->params.h[2] - trueTx) < 0.05
				&& std::fabs(exactRes.record->params.h[5]) < 0.05,
				L"exact-size producer fit recovers true luma pan 10.0 within 0.05 px");

			CameraLink::FitCounters wrong;
			const CameraLink::CameraFitResult wrongRes = run(true, lp.w - 1, lp.h, lp.s, wrong);
			Assert::AreEqual(uint64_t(1), wrong.photoSkippedNoLuma.load(), L"a size that is not the planes' own: no luma");
			Assert::AreEqual(uint64_t(0), wrong.photoRefined.load());
			Assert::AreEqual(uint64_t(0), wrong.photoFellBack.load());
			Assert::IsTrue(wrongRes.record != nullptr && wrongRes.record->valid
				&& std::fabs(wrongRes.record->params.h[2] - trueTx) > 0.4,
				L"mismatched-size luma is not applied: record stays at biased pan ~10.6");

			CameraLink::FitCounters noStep;
			const CameraLink::CameraFitResult noStepRes = run(true, lp.w, lp.h, 0.0, noStep);
			Assert::AreEqual(uint64_t(1), noStep.photoSkippedNoLuma.load(), L"no source step: no luma");
			Assert::AreEqual(uint64_t(0), noStep.photoRefined.load());
			Assert::AreEqual(uint64_t(0), noStep.photoFellBack.load());
			Assert::IsTrue(noStepRes.record != nullptr && noStepRes.record->valid
				&& std::fabs(noStepRes.record->params.h[2] - trueTx) > 0.4,
				L"zero-step luma is not applied: record stays at biased pan ~10.6");
		}

		TEST_METHOD(BiasedFourKPanRefinesToTrueLumaPanAcrossFitAndProducer)
		{
			// CT-M5 end-to-end proof: vectors biased by 0.6 px (tx = 10.6 vs true luma pan 10.0, strictly inside R3's
			// 3 * 0.25 = 0.75 px PhotometricVerdict agreement gate) actively refine to 10.0 (< 0.05 px) on both
			// Fitter::Fit and CameraFitProducer::Fit, while the exact same biased field without luma stays stuck at ~10.6.
			const Frame uhd{ 3840, 2160 };
			const double trueTx = 10.0, trueTy = 0.0, bias = 0.6;
			const Field biasedField = PanField(uhd, trueTx + bias, trueTy);
			const LumaPair lp(uhd, 4.0, trueTx, trueTy);

			// Same-input negative control via Fitter::Fit (no photometric input).
			const CameraMotion vecOnly = FitOnce(biasedField, nullptr);
			Assert::IsTrue(vecOnly.valid, L"negative control: vector-only fit is valid");
			Assert::IsTrue(std::fabs(vecOnly.params.h[2] - trueTx) > 0.4,
				L"same-input negative control: FitOnce(biasedField, nullptr) stays stuck at biased ~10.6");

			// Direct Fitter::Fit with quarter-form 4K luma pair.
			PhotometricInput photo;
			photo.luma = lp.View();
			const CameraMotion r = FitOnce(biasedField, &photo);
			Assert::IsTrue(photo.outcome == PhotometricOutcome::Refined, L"biased 4K pan outcome must be Refined");
			Assert::IsTrue(photo.result.status == RefineStatus::Refined, L"biased 4K pan status must be RefineStatus::Refined");
			Assert::IsTrue(photo.result.residualAfter < photo.result.residualBefore,
				L"photometric refinement strictly lowers the Huber luma residual");
			Assert::IsTrue(r.valid && (r.verdictFlags & kVerdictPhotometricDisagreement) == 0,
				L"agreeing 0.6 px refine does not trigger photometric disagreement");
			Assert::IsTrue(std::fabs(r.params.h[2] - trueTx) < 0.05 && std::fabs(r.params.h[5] - trueTy) < 0.05,
				L"refined Fitter::Fit recovers true luma pan 10.0 within 0.05 px");
			Assert::IsTrue(std::fabs(r.params.h[2] - photo.result.model.h[2]) < 1e-9,
				L"record carries exact refined model");
			Assert::IsTrue(r.signals.precision < vecOnly.signals.precision,
				L"signals are rebuilt on the refined model against the biased vectors");

			// Same biased field through CameraFitProducer counters + record output.
			CameraLink::FitCounters counters;
			CameraLink::CameraFitProducer producer(CameraModel::Config(), &counters);
			CameraLink::TimingFacts tf;
			tf.measuredSourceHz = 24.0;
			producer.SetTimingFacts(tf);
			producer.NoteTiming();
			CameraLink::CameraFitJob job;
			job.seq = 5;
			job.field = CameraLinkTest::CopyOf(biasedField, 5);
			job.facts.pairSeconds = 1.0 / 24.0;
			job.stamp = job.field->stamp;
			job.lumaFrom = std::make_shared<const std::vector<uint8_t>>(lp.a);
			job.lumaTo = std::make_shared<const std::vector<uint8_t>>(lp.b);
			job.lumaW = lp.w;
			job.lumaH = lp.h;
			job.lumaStep = lp.s;
			const CameraLink::CameraFitResult prodRes = producer.Fit(job);
			Assert::AreEqual(uint64_t(1), counters.photoRefined.load(), L"producer counts photoRefined == 1");
			Assert::AreEqual(uint64_t(0), counters.photoFellBack.load(), L"producer counts photoFellBack == 0");
			Assert::AreEqual(uint64_t(0), counters.photoSkippedNoLuma.load());
			Assert::AreEqual(uint64_t(0), counters.photoSkippedResolution.load());
			Assert::AreEqual(uint64_t(0), counters.photoSkippedOther.load());
			Assert::IsTrue(prodRes.record != nullptr && prodRes.record->valid
				&& std::fabs(prodRes.record->params.h[2] - trueTx) < 0.05
				&& std::fabs(prodRes.record->params.h[5] - trueTy) < 0.05,
				L"producer fit recovers true luma pan 10.0 within 0.05 px");
		}
	};
}
