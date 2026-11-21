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

		bool Attempted(PhotometricOutcome o) { return o == PhotometricOutcome::Refined || o == PhotometricOutcome::FellBack; }
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
			// (c) s 4 at 2160 rows and (d) s 2 at 1080 rows: attempted.
			{
				const Field f = PanField(uhd, tx, ty);
				const LumaPair lp(uhd, 4.0, tx, ty);
				PhotometricInput photo;
				photo.luma = lp.View();
				FitOnce(f, &photo);
				Assert::IsTrue(Attempted(photo.outcome), (L"quarter form at 2160 is attempted, outcome " + std::wstring(1, L'0' + int(photo.outcome))).c_str());
			}
			{
				const Field f = PanField(hd, tx, ty);
				const LumaPair lp(hd, 2.0, tx, ty);
				PhotometricInput photo;
				photo.luma = lp.View();
				FitOnce(f, &photo);
				Assert::IsTrue(Attempted(photo.outcome), (L"half form at 1080 is attempted, outcome " + std::wstring(1, L'0' + int(photo.outcome))).c_str());
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
			// The luma pair moves the OTHER way (a pan the vectors do not show): the verification refuses it and the vector model stays.
			const Frame hd{ 1920, 1080 };
			const double tx = 6.0, ty = -2.0;
			const Field f = PanField(hd, tx, ty);
			const CameraMotion vectorOnly = FitOnce(f, nullptr);
			const LumaPair lp(hd, 2.0, -tx, -ty, 33);
			PhotometricInput photo;
			photo.luma = lp.View();
			const CameraMotion r = FitOnce(f, &photo);
			Assert::IsTrue(photo.outcome == PhotometricOutcome::FellBack, (L"the refine fell back, outcome " + std::wstring(1, L'0' + int(photo.outcome))).c_str());
			Assert::IsTrue(photo.result.status != RefineStatus::Refined, L"its own status says why (not refined)");
			for (int k = 0; k < 9; ++k)
				Assert::AreEqual(vectorOnly.params.h[k], r.params.h[k], L"the vector model is kept, bit for bit");
			Assert::IsTrue(Pod(vectorOnly) == Pod(r), L"and so is the whole record");
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
			RefineResult in;
			in.status = RefineStatus::Refined;
			in.model = R3Scenes::Translation(1.25, -0.5);
			in.correctionPx = 0.3;
			in.residualBefore = 4.0;
			in.residualAfter = 2.0;
			const RefineResult out = PhotometricVerdict(R3Scenes::Translation(1.5, -0.5), in, 0.25);
			Assert::IsTrue(out.status == in.status);
			for (int k = 0; k < 9; ++k)
				Assert::AreEqual(in.model.h[k], out.model.h[k]);
			Assert::AreEqual(in.correctionPx, out.correctionPx);
			Assert::AreEqual(in.residualBefore, out.residualBefore);
			Assert::AreEqual(in.residualAfter, out.residualAfter);
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
			// The 4K pan job of the link tests (240 x 135 blocks, 2160 rows): a quarter-form luma pair of 960 x 540 is attempted; the same
			// planes with a declared size they do not have are not used (counted as no luma).
			const Frame uhd{ 3840, 2160 };
			const LumaPair lp(uhd, 4.0, 10.0, 0.0);
			auto from = std::make_shared<const std::vector<uint8_t>>(lp.a), to = std::make_shared<const std::vector<uint8_t>>(lp.b);
			auto run = [&](int w, int h, double step, CameraLink::FitCounters& counters) {
				CameraLink::CameraFitProducer producer(CameraModel::Config(), &counters);
				CameraLink::TimingFacts tf;
				tf.measuredSourceHz = 24.0;
				producer.SetTimingFacts(tf);
				producer.NoteTiming();
				CameraLink::CameraFitJob job = CameraLinkTest::PanJob(1, 10.0);
				job.lumaFrom = from;
				job.lumaTo = to;
				job.lumaW = w;
				job.lumaH = h;
				job.lumaStep = step;
				producer.Fit(job);
			};
			CameraLink::FitCounters exact;
			run(lp.w, lp.h, lp.s, exact);
			Assert::AreEqual(uint64_t(1), exact.photoRefined.load() + exact.photoFellBack.load(), L"the exact-size pair is attempted");
			Assert::AreEqual(uint64_t(0), exact.photoSkippedNoLuma.load());
			CameraLink::FitCounters wrong;
			run(lp.w - 1, lp.h, lp.s, wrong);
			Assert::AreEqual(uint64_t(1), wrong.photoSkippedNoLuma.load(), L"a size that is not the planes' own: no luma");
			Assert::AreEqual(uint64_t(0), wrong.photoRefined.load() + wrong.photoFellBack.load());
			CameraLink::FitCounters noStep;
			run(lp.w, lp.h, 0.0, noStep);
			Assert::AreEqual(uint64_t(1), noStep.photoSkippedNoLuma.load(), L"no source step: no luma");
		}
	};
}
