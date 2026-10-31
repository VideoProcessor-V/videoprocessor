// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "pch.h"
#include <VP-V/modules/motion/estimation/global_model/GlobalModelFit.h>
#include <VP-V/modules/motion/estimation/global_model/GlobalModelGeometry.h>
#include "SyntheticCameraFields.h"
#include "CppUnitTest.h"

#include <cmath>
#include <cstring>
#include <set>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	using namespace CameraModel;
	using namespace CameraSynth;

	namespace
	{
		const double kPairSeconds = 1.0 / 24;
		const double kTol = 1e-3;   // of the picture diagonal

		void Require(bool ok, const std::string& what)
		{
			if (ok) return;
			Logger::WriteMessage(what.c_str());
			Assert::Fail(std::wstring(what.begin(), what.end()).c_str());
		}

		std::string Fmt(const char* name, const CameraMotion& r)
		{
			return std::string(name) + ": " + Describe(r);
		}

		// Expect a clean, confident, correct camera fit with (almost) every block labelled Camera.
		void ExpectGoodFit(const char* name, const CameraMotion& r, const Params& truth, const Field& f, int minType)
		{
			Require(r.valid, Fmt(name, r));
			Require(r.reason == Reason::Ok, Fmt(name, r));
			Require(r.confidence >= 0.5, Fmt(name, r));
			Require(static_cast<int>(r.params.type) >= minType, Fmt(name, r));
			const double err = ModelError(r, truth, f.picture);
			Require(err < kTol, Fmt(name, r) + " corner error/diag=" + std::to_string(err));
			Require(r.blocks != nullptr, name);
			const int cam = CountLabel(r, Label::Camera);
			Require(cam >= static_cast<int>(0.95 * f.Count()), Fmt(name, r) + " camera blocks=" + std::to_string(cam));
		}

		void KnownModel(const char* name, const GridSpec& g, const Params& truth, double noise, int minType, uint64_t seed)
		{
			Field f(g);
			f.FromModel(truth);
			if (noise > 0) f.AddNoise(noise, seed);
			Fitter fitter;
			const CameraMotion r = fitter.Fit(f.Input(kPairSeconds, 5));
			ExpectGoodFit(name, r, truth, f, minType);
		}

		const int kTrans = static_cast<int>(ModelType::Translation);
		const int kSim = static_cast<int>(ModelType::Similarity);
		const int kHom = static_cast<int>(ModelType::Homography);

		Params Pan4K() { return PanModel(12.3, -4.7); }
		Params Persp4K() { return PerspectiveModel(3840.0, 3.0, -2.0); }
		Params Rot1Deg() { return SimilarityModel(1.0, 0.01745329252, 0, 0); }

		bool SameBlocks(const CameraMotion& a, const CameraMotion& b)
		{
			if (!a.blocks || !b.blocks) return a.blocks == b.blocks;
			return a.blocks->label == b.blocks->label && a.blocks->resX == b.blocks->resX && a.blocks->resY == b.blocks->resY
				&& a.blocks->modelX == b.blocks->modelX && a.blocks->modelY == b.blocks->modelY
				&& a.blocks->objectId == b.blocks->objectId && a.blocks->tinyMover == b.blocks->tinyMover;
		}

		int64_t g_fakeNow = 0;
		int64_t FakeClock() { g_fakeNow += 1000000; return g_fakeNow; }
	}

	TEST_CLASS(CameraFitTests)
	{
	public:
		// ---- known models ----
		TEST_METHOD(KnownPan) { KnownModel("pan", Grid4K(), Pan4K(), 0, kTrans, 1); }
		TEST_METHOD(KnownPanNoisy) { KnownModel("pan noisy", Grid4K(), Pan4K(), 0.05, kTrans, 2); }
		TEST_METHOD(KnownZoomIn) { KnownModel("zoom 1.02", Grid4K(), ZoomModel(1.02), 0, kSim, 3); }
		TEST_METHOD(KnownZoomInNoisy) { KnownModel("zoom 1.02 noisy", Grid4K(), ZoomModel(1.02), 0.05, kSim, 4); }
		TEST_METHOD(KnownZoomOut) { KnownModel("zoom 0.97", Grid4K(), ZoomModel(0.97), 0, kSim, 5); }
		TEST_METHOD(KnownZoomOutNoisy) { KnownModel("zoom 0.97 noisy", Grid4K(), ZoomModel(0.97), 0.05, kSim, 6); }
		TEST_METHOD(KnownRotation) { KnownModel("rotation", Grid4K(), Rot1Deg(), 0, kSim, 7); }
		TEST_METHOD(KnownRotationNoisy) { KnownModel("rotation noisy", Grid4K(), Rot1Deg(), 0.05, kSim, 8); }
		TEST_METHOD(KnownPerspective) { KnownModel("perspective", Grid4K(), Persp4K(), 0, kHom, 9); }
		TEST_METHOD(KnownPerspectiveNoisy) { KnownModel("perspective noisy", Grid4K(), Persp4K(), 0.05, kHom, 10); }

		// ---- other grids ----
		TEST_METHOD(Grid8KPan)
		{
			const GridSpec g = Grid8K();
			KnownModel("8k pan", g, PanModel(0.77 * g.blockSize, -0.29 * g.blockSize), 0.05, kTrans, 11);
			KnownModel("8k zoom", g, ZoomModel(1.02), 0.05, kSim, 12);
		}
		TEST_METHOD(Grid8KPerspective)
		{
			const GridSpec g = Grid8K();
			KnownModel("8k perspective", g, PerspectiveModel(g.w * g.blockSize, 6.0, -4.0), 0.05, kHom, 13);
		}
		TEST_METHOD(GridOddPan)
		{
			const GridSpec g = GridOdd();
			KnownModel("odd pan", g, PanModel(0.77 * g.blockSize, -0.29 * g.blockSize), 0.05, kTrans, 14);
			KnownModel("odd zoom", g, ZoomModel(0.97), 0.05, kSim, 15);
			KnownModel("odd perspective", g, PerspectiveModel(g.w * g.blockSize, 1.0, -1.0), 0.05, kHom, 16);
		}

		// ---- moving objects ----
		TEST_METHOD(ObjectBlockPatch)
		{
			Field f(Grid4K());
			const Params truth = Pan4K();
			f.FromModel(truth);
			f.AddNoise(0.05, 20);
			f.AddObject(100, 60, 6, 4, -20, 15);
			Fitter fitter;
			const CameraMotion r = fitter.Fit(f.Input(kPairSeconds, 5));
			Require(r.valid && r.confidence >= 0.5, Fmt("object patch", r));
			Require(ModelError(r, truth, f.picture) < kTol, Fmt("object patch model", r));
			Require(CountLabelInRect(r, f, 100, 60, 6, 4, Label::Object) == 24, Fmt("object patch object blocks", r));
			Require(CountLabel(r, Label::Camera) == static_cast<int>(f.Count()) - 24, Fmt("object patch camera blocks", r));
		}

		TEST_METHOD(TinyFastObjectOneAndTwoBlocks)
		{
			const Params truth = Pan4K();
			{
				Field f(Grid4K());
				f.FromModel(truth);
				f.AddNoise(0.05, 21);
				f.AddTinyObject(50, 40, 1, -90, 60);
				Fitter fitter;
				const CameraMotion r = fitter.Fit(f.Input(kPairSeconds, 5));
				Require(r.valid && ModelError(r, truth, f.picture) < kTol, Fmt("tiny one", r));
				Require(r.blocks->label[f.Index(50, 40)] == static_cast<uint8_t>(Label::Object), Fmt("tiny one block label", r));
			}
			{
				Field f(Grid4K());
				f.FromModel(truth);
				f.AddNoise(0.05, 22);
				f.AddTinyObject(50, 40, 2, -90, 60);
				Fitter fitter;
				const CameraMotion r = fitter.Fit(f.Input(kPairSeconds, 5));
				Require(r.valid && ModelError(r, truth, f.picture) < kTol, Fmt("tiny two", r));
				Require(CountLabelInRect(r, f, 50, 40, 2, 1, Label::Object) == 2, Fmt("tiny two block labels", r));
				Require(CountLabelInRect(r, f, 50, 40, 2, 1, Label::Camera) == 0, Fmt("tiny two never camera", r));
			}
		}

		TEST_METHOD(SubBlockBallThroughPixelResidual)
		{
			const Params truth = Pan4K();
			Field f(Grid4K());
			f.FromModel(truth);
			f.AddNoise(0.05, 23);
			f.SetPixelResidual(70, 70, 0.05);   // camera-like measured vector, but pixels disagree
			f.SetUntrusted(120, 100, 1, 1, 1.0);   // untrusted textured block
			f.SetPixelResidual(120, 100, 0.05);
			Fitter fitter;
			const CameraMotion r = fitter.Fit(f.Input(kPairSeconds, 5));
			Require(r.valid && ModelError(r, truth, f.picture) < kTol, Fmt("ball model", r));
			Require(r.blocks->label[f.Index(70, 70)] == static_cast<uint8_t>(Label::Object), Fmt("ball trusted label", r));
			Require(r.blocks->tinyMover[f.Index(70, 70)] == 1, Fmt("ball tinyMover", r));
			Require(r.blocks->label[f.Index(120, 100)] == static_cast<uint8_t>(Label::Object), Fmt("ball untrusted label", r));
			Require(r.blocks->label[f.Index(120, 100)] != static_cast<uint8_t>(Label::Camera), Fmt("ball untrusted not camera", r));
		}

		// ---- flat sky ----
		TEST_METHOD(FlatSkyIsFilledAsCamera)
		{
			const Params truth = Pan4K();
			Field full(Grid4K());
			full.FromModel(truth);
			full.AddNoise(0.05, 24);
			Fitter fa;
			const CameraMotion rFull = fa.Fit(full.Input(kPairSeconds, 5));

			Field sky(Grid4K());
			sky.FromModel(truth);
			sky.AddNoise(0.05, 24);
			const int skyRows = sky.gridH / 3;
			sky.SetUntrusted(0, 0, sky.gridW, skyRows, 0.05);
			Fitter fb;
			const CameraMotion rSky = fb.Fit(sky.Input(kPairSeconds, 5));
			Require(rSky.valid && rSky.confidence >= 0.5, Fmt("sky", rSky));
			Require(ModelError(rSky, truth, sky.picture) < kTol, Fmt("sky model", rSky));
			Require(CountLabelInRect(rSky, sky, 0, 0, sky.gridW, skyRows, Label::Camera) == sky.gridW * skyRows, Fmt("sky labels", rSky));
			Require(rSky.confidence <= rFull.confidence, Fmt("sky confidence", rSky) + " vs full " + std::to_string(rFull.confidence));
		}

		// ---- cut / fade / flash ----
		TEST_METHOD(CutWithoutNext)
		{
			Field a(Grid4K());
			a.FromModel(PanModel(10, 0));
			Fitter f;
			f.Fit(a.Input(kPairSeconds, 5));
			PairInput c = a.Input(kPairSeconds, 6);
			c.cut = true;
			const CameraMotion r = f.Fit(c);
			Require(r.reason == Reason::Cut && !r.valid && r.confidence == 0, Fmt("cut", r));
		}

		TEST_METHOD(CutSeededFromNext)
		{
			const Params pa = PanModel(10, 0), pb = PanModel(-8, 2);
			Field a(Grid4K()), b(Grid4K());
			a.FromModel(pa); a.AddNoise(0.05, 30);
			b.FromModel(pb); b.AddNoise(0.05, 31);
			const PairInput bin = b.Input(kPairSeconds, 7);
			Fitter f;
			f.Fit(a.Input(kPairSeconds, 5));
			PairInput c = a.Input(kPairSeconds, 6);
			c.cut = true;
			c.next = &bin;
			const CameraMotion r = f.Fit(c);
			Require(r.reason == Reason::PostCutSeeded && r.valid && r.seededFromNext, Fmt("seeded", r));
			Require(ModelError(r, pb, a.picture) < kTol, Fmt("seeded model", r));

			// The next normal pair starts from the seed: far fewer hypotheses than a cold search.
			const CameraMotion after = f.Fit(bin);
			Fitter cold;
			const CameraMotion coldRec = cold.Fit(bin);
			Logger::WriteMessage((Fmt("after seed", after) + " | cold hyp=" + std::to_string(coldRec.hypotheses)).c_str());
			Require(after.valid && ModelError(after, pb, a.picture) < kTol, Fmt("after seed", after));
			Require(after.hypotheses <= coldRec.hypotheses, Fmt("after seed hypotheses", after));
			Require(after.hypotheses <= 4, Fmt("after seed small hypotheses", after));
		}

		TEST_METHOD(FadeStaysValid)
		{
			Field a(Grid4K());
			a.FromModel(Pan4K()); a.AddNoise(0.05, 32);
			Fitter plain;
			const CameraMotion r0 = plain.Fit(a.Input(kPairSeconds, 5));
			PairInput fd = a.Input(kPairSeconds, 5);
			fd.fade = true; fd.fadeGain = 0.7; fd.fadeOffset = 0.02;
			Fitter faded;
			const CameraMotion r1 = faded.Fit(fd);
			Require(r1.valid && r1.inFade, Fmt("fade", r1));
			Require(r1.confidence > 0 && std::fabs(r1.confidence - r0.confidence) < 0.1, Fmt("fade", r1) + " vs plain " + std::to_string(r0.confidence));
		}

		TEST_METHOD(FlashLeavesStateUntouched)
		{
			Field a(Grid4K()), a2(Grid4K());
			a.FromModel(PanModel(10, 0)); a.AddNoise(0.05, 33);
			a2.FromModel(PanModel(10.4, 0.1)); a2.AddNoise(0.05, 34);
			Fitter withFlash, without;
			withFlash.Fit(a.Input(kPairSeconds, 5));
			without.Fit(a.Input(kPairSeconds, 5));
			const CameraMotion beforePrev = *withFlash.Previous();
			PairInput fl = a.Input(kPairSeconds, 6);
			fl.flash = true;
			const CameraMotion rf = withFlash.Fit(fl);
			Require(rf.reason == Reason::Flash && !rf.valid && rf.confidence == 0, Fmt("flash", rf));
			Require(withFlash.Previous() != nullptr && withFlash.Previous()->seq == beforePrev.seq
				&& std::memcmp(&withFlash.Previous()->params, &beforePrev.params, sizeof(Params)) == 0
				&& withFlash.Previous()->confidence == beforePrev.confidence, "flash changed Previous()");
			const CameraMotion r1 = withFlash.Fit(a2.Input(kPairSeconds, 7));
			const CameraMotion r2 = without.Fit(a2.Input(kPairSeconds, 7));
			Require(std::memcmp(&r1.params, &r2.params, sizeof(Params)) == 0 && r1.confidence == r2.confidence
				&& r1.hypotheses == r2.hypotheses && std::memcmp(&r1.signals, &r2.signals, sizeof(Signals)) == 0 && SameBlocks(r1, r2),
				Fmt("after flash", r1) + " vs " + Fmt("no flash", r2));
		}

		TEST_METHOD(FlashBridgedWithNext)
		{
			Field a(Grid4K()), n(Grid4K());
			a.FromModel(PanModel(10, 0)); a.AddNoise(0.05, 35);
			n.FromModel(PanModel(14, 0)); n.AddNoise(0.05, 36);
			const PairInput nin = n.Input(kPairSeconds, 7);
			Fitter f;
			f.Fit(a.Input(kPairSeconds, 5));
			PairInput fl = a.Input(kPairSeconds, 6);
			fl.flash = true;
			fl.next = &nin;
			const CameraMotion r = f.Fit(fl);
			Require(r.reason == Reason::FlashBridged && r.valid && r.bridged, Fmt("bridged", r));
			Require(r.params.h[2] > 10.2 && r.params.h[2] < 13.8, Fmt("bridged between", r) + " tx=" + std::to_string(r.params.h[2]));
			Require(f.Previous()->seq == 5, "bridged changed Previous()");
		}

		// ---- ambiguous scenes ----
		TEST_METHOD(ParallaxLowConfidence)
		{
			Field f(Grid4K());
			f.FromModel(PanModel(4, 0));
			f.AddObject(0, f.gridH / 2, f.gridW, f.gridH - f.gridH / 2, 14, 0);
			f.AddNoise(0.05, 40);
			Fitter fitter;
			const CameraMotion r = fitter.Fit(f.Input(kPairSeconds, 5));
			Require(r.confidence < 0.5, Fmt("parallax", r));
		}

		TEST_METHOD(CrowdLowConfidence)
		{
			Field f(Grid4K());
			f.FromModel(Pan4K());
			SplitMix64 rng(41);
			int scattered = 0;
			// Many small (2x2 block) movers, each with its own vector, on about 45% of the picture.
			for (int y = 0; y + 1 < f.gridH; y += 2)
				for (int x = 0; x + 1 < f.gridW; x += 2)
					if (rng.Unit() < 0.45)
					{
						f.AddObject(x, y, 2, 2, (rng.Unit() - 0.5) * 80, (rng.Unit() - 0.5) * 80);
						scattered += 4;
					}
			f.AddNoise(0.05, 42);
			Fitter fitter;
			const CameraMotion r = fitter.Fit(f.Input(kPairSeconds, 5));
			Require(r.confidence < 0.5, Fmt("crowd", r) + " scattered=" + std::to_string(scattered));
		}

		TEST_METHOD(ObjectFillingMostOfFrameIsAmbiguous)
		{
			Field f(Grid4K());
			f.FromModel(PanModel(12, 0));
			f.AddObject(static_cast<int>(0.3 * f.gridW), 0, f.gridW - static_cast<int>(0.3 * f.gridW), f.gridH, -30, 8);
			f.AddNoise(0.05, 43);
			Fitter fitter;
			const CameraMotion r = fitter.Fit(f.Input(kPairSeconds, 5));
			Require(r.confidence < 0.5, Fmt("70% object", r));
		}

		// ---- overlays ----
		TEST_METHOD(StaticLogoOverPan)
		{
			const Params truth = Pan4K();
			for (int guarded = 0; guarded < 2; ++guarded)
			{
				Field f(Grid4K());
				f.FromModel(truth);
				f.AddNoise(0.05, 50);
				f.AddLogo(10, 8, 4, 4, guarded == 1);
				Fitter fitter;
				const CameraMotion r = fitter.Fit(f.Input(kPairSeconds, 5));
				const std::string tag = guarded ? "logo guarded" : "logo unguarded";
				Require(r.valid && r.confidence >= 0.5 && ModelError(r, truth, f.picture) < kTol, Fmt(tag.c_str(), r));
				Require(CountLabelInRect(r, f, 10, 8, 4, 4, Label::Overlay) == 16, Fmt(tag.c_str(), r) + " overlay blocks");
			}
		}

		TEST_METHOD(CreditsOverStaticPicture)
		{
			Field f(Grid4K());
			f.FromModel(PanModel(0, 0));
			f.AddNoise(0.02, 51);
			f.AddTextTiles(60, 90, 120, 6, 52);
			Fitter fitter;
			const CameraMotion r = fitter.Fit(f.Input(kPairSeconds, 5));
			Require(r.valid && r.confidence >= 0.5, Fmt("credits", r));
			Require(ModelError(r, PanModel(0, 0), f.picture) < kTol, Fmt("credits model", r));
			Require(CountLabelInRect(r, f, 60, 90, 120, 6, Label::Overlay) == 120 * 6, Fmt("credits overlay", r));
		}

		TEST_METHOD(LetterboxWithPan)
		{
			const Params truth = Pan4K();
			Field plain(Grid4K());
			plain.FromModel(truth);
			plain.AddNoise(0.05, 53);
			Field bars(Grid4K());
			bars.FromModel(truth);
			bars.AddNoise(0.05, 53);
			bars.AddBars(17, 17);
			Fitter fp, fb;
			PairInput inPlain = plain.Input(kPairSeconds, 5);
			inPlain.picture = bars.picture;   // same active picture, bars carry the model's vectors
			const CameraMotion rp = fp.Fit(inPlain);
			const CameraMotion rb = fb.Fit(bars.Input(kPairSeconds, 5));
			Require(rb.valid && rb.confidence >= 0.5, Fmt("bars", rb));
			Require(ModelError(rb, truth, bars.picture) < kTol, Fmt("bars model", rb));
			Require(MaxCornerDifference(rb.params, rp.params, bars.picture) < kTol, Fmt("bars vs no bars", rb) + " / " + Fmt("plain", rp));
			Require(CountLabelInRect(rb, bars, 0, 0, bars.gridW, 17, Label::Overlay) == bars.gridW * 17, Fmt("bars top", rb));
			Require(CountLabelInRect(rb, bars, 0, bars.gridH - 17, bars.gridW, 17, Label::Overlay) == bars.gridW * 17, Fmt("bars bottom", rb));
		}

		// ---- temporal behaviour ----
		TEST_METHOD(HandheldShakeTracksEveryFrame)
		{
			const GridSpec g = Grid4K();
			SplitMix64 rng(60);
			Fitter fitter;
			for (int i = 0; i < 60; ++i)
			{
				const Params truth = ShakeModel(rng, g.w * g.blockSize);
				Field f(g);
				f.FromModel(truth);
				f.AddNoise(0.05, 100 + i);
				const CameraMotion r = fitter.Fit(f.Input(1.0 / 60, 10 + i));
				const std::string tag = "shake frame " + std::to_string(i);
				Require(r.valid, Fmt(tag.c_str(), r));
				Require(r.confidence >= 0.5, Fmt(tag.c_str(), r));
				const double err = ModelError(r, truth, f.picture);
				Require(err < kTol, Fmt(tag.c_str(), r) + " err=" + std::to_string(err));
			}
		}

		TEST_METHOD(PulldownRatesAgree)
		{
			// The same camera motion per second over a 1/24 and a 2.5/24 pair gives equal rates.
			const double logScaleRate = 0.4, angleRate = 0.12, txRate = 90, tyRate = -30;
			SimilarityRates rates;
			rates.logScale = logScaleRate; rates.angle = angleRate; rates.tx = txRate; rates.ty = tyRate;
			const double secs[2] = { 1.0 / 24, 2.5 / 24 };
			CameraMotion rec[2];
			for (int i = 0; i < 2; ++i)
			{
				Field f(Grid4K());
				Params p = FromSimilarity(rates, secs[i]);
				p.type = ModelType::Similarity;
				f.FromModel(p);
				
				Fitter fitter;
				rec[i] = fitter.Fit(f.Input(secs[i], 5));
				Require(rec[i].valid && rec[i].confidence >= 0.5, Fmt("pulldown", rec[i]));
			}
			auto rel = [](double a, double b) { return std::fabs(a - b) / std::max(1e-9, std::max(std::fabs(a), std::fabs(b))); };
			Require(rel(rec[0].rates.logScale, rec[1].rates.logScale) < kTol, "logScale rate " + std::to_string(rec[0].rates.logScale) + " vs " + std::to_string(rec[1].rates.logScale));
			Require(rel(rec[0].rates.angle, rec[1].rates.angle) < kTol, "angle rate " + std::to_string(rec[0].rates.angle) + " vs " + std::to_string(rec[1].rates.angle));
			Require(rel(rec[0].rates.tx, rec[1].rates.tx) < kTol, "tx rate " + std::to_string(rec[0].rates.tx) + " vs " + std::to_string(rec[1].rates.tx));
			Require(rel(rec[0].rates.ty, rec[1].rates.ty) < kTol, "ty rate " + std::to_string(rec[0].rates.ty) + " vs " + std::to_string(rec[1].rates.ty));
		}

		TEST_METHOD(ZeroPairSecondsIsNoInput)
		{
			Field f(Grid4K());
			f.FromModel(Pan4K());
			Fitter fitter;
			PairInput bad = f.Input(0.0, 5);
			CameraMotion r = fitter.Fit(bad);
			Require(r.reason == Reason::NoInput && !r.valid, Fmt("zero seconds", r));
			bad.pairSeconds = -1.0 / 24;
			r = fitter.Fit(bad);
			Require(r.reason == Reason::NoInput && !r.valid, Fmt("negative seconds", r));
		}

		// ---- forward / backward ----
		TEST_METHOD(BackwardConsistencyAndCorruption)
		{
			const Params truth = ZoomModel(1.02);
			Field good(Grid4K());
			good.FromModel(truth, true);
			good.AddNoise(0.05, 80);
			Fitter fg;
			const CameraMotion rg = fg.Fit(good.Input(kPairSeconds, 5));
			Require(rg.valid && rg.signals.haveBackward && rg.signals.inverse > 0.8, Fmt("backward consistent", rg));

			Field bad(Grid4K());
			bad.FromModel(truth, true);
			bad.AddNoise(0.05, 80);
			bad.CorruptBackward(30, 81);
			Fitter fb;
			const CameraMotion rb = fb.Fit(bad.Input(kPairSeconds, 5));
			Require(rb.signals.haveBackward && rb.signals.inverse < 0.4, Fmt("backward corrupt", rb));
			Require(rb.confidence < rg.confidence, Fmt("backward corrupt", rb) + " vs " + std::to_string(rg.confidence));
		}

		// ---- determinism ----
		TEST_METHOD(DeterministicRecords)
		{
			Field f(Grid4K());
			f.FromModel(PerspectiveModel(3840.0, 3.0, -2.0), true);
			f.AddNoise(0.2, 90);
			f.AddObject(100, 60, 6, 4, -20, 15);
			f.AddTinyObject(30, 30, 1, -70, 50);
			f.SetPixelResidual(70, 70, 0.05);
			Config cfg;
			cfg.objects = true;
			Fitter f1(cfg), f2(cfg);
			const PairInput in = f.Input(kPairSeconds, 5);
			const CameraMotion a = f1.Fit(in);
			const CameraMotion b = f2.Fit(in);
			Require(std::memcmp(&a.params, &b.params, sizeof(Params)) == 0, "params differ");
			Require(a.confidence == b.confidence && a.hypotheses == b.hypotheses && a.samples == b.samples && a.reason == b.reason, "scalars differ");
			Require(std::memcmp(&a.signals, &b.signals, sizeof(Signals)) == 0, "signals differ");
			Require(SameBlocks(a, b), "block data differ");
			Require(a.objects.size() == b.objects.size(), "object count differs");
			for (size_t i = 0; i < a.objects.size(); ++i)
				Require(a.objects[i].id == b.objects[i].id && std::memcmp(a.objects[i].h, b.objects[i].h, sizeof(a.objects[i].h)) == 0, "object differs");
		}

		// ---- generations ----
		static void GenerationChange(bool resync)
		{
			Field f(Grid4K());
			f.FromModel(Pan4K());
			f.AddNoise(0.05, 110);
			Fitter fitter;
			for (int i = 0; i < 3; ++i) fitter.Fit(f.Input(kPairSeconds, 5 + i));
			const CameraMotion warm = fitter.Fit(f.Input(kPairSeconds, 8));

			PairInput changed = f.Input(kPairSeconds, 9);
			if (resync) changed.resyncGeneration = 1; else changed.rebaseGeneration = 1;
			const CameraMotion after = fitter.Fit(changed);
			Fitter fresh;
			const CameraMotion cold = fresh.Fit(changed);
			Logger::WriteMessage((Fmt("warm", warm) + " | " + Fmt("after change", after) + " | " + Fmt("cold", cold)).c_str());
			Require(after.valid, Fmt("after change", after));
			Require(after.hypotheses == cold.hypotheses, Fmt("after change", after) + " cold hyp=" + std::to_string(cold.hypotheses));
			Require(warm.hypotheses <= cold.hypotheses, Fmt("warm", warm));
			Require(after.resyncGeneration == changed.resyncGeneration && after.rebaseGeneration == changed.rebaseGeneration, "generation not carried");

			// Too few samples straight after another change: GenerationReset, not TooFewSamples.
			Field sparse(Grid4K());
			sparse.FromModel(Pan4K());
			sparse.SetUntrusted(0, 0, sparse.gridW, sparse.gridH, 1.0);
			sparse.trust[sparse.Index(5, 5)] = 1.0f;
			sparse.trust[sparse.Index(50, 50)] = 1.0f;
			sparse.trust[sparse.Index(100, 100)] = 1.0f;
			PairInput again = sparse.Input(kPairSeconds, 10);
			again.resyncGeneration = changed.resyncGeneration + (resync ? 1 : 0);
			again.rebaseGeneration = changed.rebaseGeneration + (resync ? 0 : 1);
			const CameraMotion rr = fitter.Fit(again);
			Require(!rr.valid && rr.reason == Reason::GenerationReset, Fmt("sparse after change", rr));
			Require(rr.resyncGeneration == again.resyncGeneration && rr.rebaseGeneration == again.rebaseGeneration, "sparse generation not carried");
			// Same sparse field without a change: plain TooFewSamples.
			const CameraMotion plain = fitter.Fit(again);
			Require(!plain.valid && plain.reason == Reason::TooFewSamples, Fmt("sparse without change", plain));
		}
		TEST_METHOD(ResyncGenerationResetsState) { GenerationChange(true); }
		TEST_METHOD(RebaseGenerationResetsState) { GenerationChange(false); }

		// ---- deadline ----
		TEST_METHOD(DeadlineMissAndNoClock)
		{
			Field f(Grid4K());
			f.FromModel(Pan4K());
			SplitMix64 rng(119);
			for (int y = 0; y + 1 < f.gridH; y += 2)
				for (int x = 0; x + 1 < f.gridW; x += 2)
					if (rng.Unit() < 0.45)
						f.AddObject(x, y, 2, 2, (rng.Unit() - 0.5) * 80, (rng.Unit() - 0.5) * 80);
			f.AddNoise(0.3, 120);
			PairInput in = f.Input(kPairSeconds, 5);
			in.deadlineMicros = 1;

			// The fitter checks the clock inside the hypothesis loop; with a winner already found it
			// stops searching and returns the best model so far (DeadlineMissed is reserved for the case
			// where the deadline passes before any hypothesis was scored, which a Translation draw
			// always prevents on a real field). So: the search is truncated, not the answer discarded.
			Config cfg;
			cfg.clock = &FakeClock;
			g_fakeNow = 0;
			Fitter late(cfg);
			const CameraMotion r = late.Fit(in);

			Fitter noClock;   // clock == nullptr: the deadline is not enforced
			const CameraMotion ok = noClock.Fit(in);
			Logger::WriteMessage((Fmt("deadline", r) + " | " + Fmt("no clock", ok)).c_str());
			Require(ok.valid && ok.reason != Reason::DeadlineMissed, Fmt("no clock", ok));
			Require(ok.hypotheses > 100, Fmt("no clock full search", ok));
			Require(r.hypotheses < ok.hypotheses, Fmt("deadline truncated the search", r) + " vs " + std::to_string(ok.hypotheses));
			Require(!r.valid && r.reason == Reason::DeadlineMissed && r.confidence == 0, Fmt("deadline missed -> no model", r));
		}

		// ---- object tracker through the fitter ----
		TEST_METHOD(ObjectsPersistentIds)
		{
			Config cfg;
			cfg.objects = true;
			Fitter fitter(cfg);
			std::vector<uint8_t> idsPerFrame[2];
			for (int i = 0; i < 2; ++i)
			{
				Field f(Grid4K());
				f.FromModel(Pan4K());
				f.AddNoise(0.05, 130 + i);
				f.AddObject(30 + i, 20, 24, 16, -25, 14);
				f.AddObject(170 - i, 90, 24, 16, 30, -22);
				const CameraMotion r = fitter.Fit(f.Input(kPairSeconds, 5 + i));
				Require(r.valid, Fmt("objects", r));
				Require(r.objects.size() == 2, Fmt("objects", r) + " count=" + std::to_string(r.objects.size()));
				for (const ObjectModel& o : r.objects) idsPerFrame[i].push_back(o.id);
				std::sort(idsPerFrame[i].begin(), idsPerFrame[i].end());
			}
			Require(idsPerFrame[0] == idsPerFrame[1], "object ids not persistent");
			Require(idsPerFrame[0][0] != 0 && idsPerFrame[0][1] != 0, "object id zero");

			Fitter off;   // objects == false
			Field f(Grid4K());
			f.FromModel(Pan4K());
			f.AddObject(30, 20, 24, 16, -25, 14);
			const CameraMotion r = off.Fit(f.Input(kPairSeconds, 5));
			Require(r.objects.empty(), Fmt("objects off", r));
		}
	};
}
