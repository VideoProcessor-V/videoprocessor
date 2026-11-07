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

		// Field-by-field, bitwise per double: Params and Signals have padding bytes, so no memcmp over the structs.
		bool SameParams(const Params& a, const Params& b)
		{
			return a.type == b.type && std::memcmp(a.h, b.h, sizeof(a.h)) == 0;
		}

		bool SameBits(double a, double b) { return std::memcmp(&a, &b, sizeof(double)) == 0; }

		bool SameSignals(const Signals& a, const Signals& b)
		{
			return SameBits(a.inlierShare, b.inlierShare) && SameBits(a.precision, b.precision) && SameBits(a.coverage, b.coverage)
				&& SameBits(a.temporal, b.temporal) && SameBits(a.conditioning, b.conditioning) && SameBits(a.engineAgreement, b.engineAgreement)
				&& SameBits(a.inverse, b.inverse) && SameBits(a.ambiguity, b.ambiguity) && SameBits(a.logit, b.logit)
				&& a.haveBackward == b.haveBackward && a.haveEngine == b.haveEngine
				&& SameBits(a.border, b.border) && SameBits(a.continuity, b.continuity);
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
				&& SameParams(withFlash.Previous()->params, beforePrev.params)
				&& withFlash.Previous()->confidence == beforePrev.confidence, "flash changed Previous()");
			const CameraMotion r1 = withFlash.Fit(a2.Input(kPairSeconds, 7));
			const CameraMotion r2 = without.Fit(a2.Input(kPairSeconds, 7));
			Require(SameParams(r1.params, r2.params) && r1.confidence == r2.confidence
				&& r1.hypotheses == r2.hypotheses && SameSignals(r1.signals, r2.signals) && SameBlocks(r1, r2),
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

		// The owner requires a pan behind many scattered movers to be FOUND (phase 1 called it "no dominant motion").
		static Config Phase1Config()
		{
			Config c;
			c.inlierShareLo = 0.4; c.inlierShareHi = 0.9;
			c.temporalNoiseFloor = false; c.verdictGatesConfidence = false;
			return c;
		}

		TEST_METHOD(CrowdUnderPanIsDominant)
		{
			Field f(Grid4K());
			const Params truth = Pan4K();
			f.FromModel(truth);
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
			const std::string msg = Fmt("crowd", r) + " scattered=" + std::to_string(scattered);
			Require(r.valid && r.reason == Reason::Ok && r.confidence >= 0.5, msg);
			Require(ModelError(r, truth, f.picture) < kTol, msg + " err=" + std::to_string(ModelError(r, truth, f.picture)));
			// Documents the phase-1 failure: the same field was "no dominant motion" (confidence could not reach 0.5).
			Fitter old(Phase1Config());
			const CameraMotion r1 = old.Fit(f.Input(kPairSeconds, 5));
			Require(r1.confidence < 0.5, Fmt("crowd phase1", r1) + " scattered=" + std::to_string(scattered));
		}

		TEST_METHOD(NoDominantMotionRandomField)
		{
			Field f(Grid4K());
			SplitMix64 rng(51);
			for (size_t i = 0; i < f.Count(); ++i)
			{
				f.fx[i] = static_cast<float>((rng.Unit() - 0.5) * 80);
				f.fy[i] = static_cast<float>((rng.Unit() - 0.5) * 80);
			}
			Fitter fitter;
			const CameraMotion r = fitter.Fit(f.Input(kPairSeconds, 5));
			Require(r.confidence < 0.5, Fmt("random field", r));
		}

		TEST_METHOD(TwoLayerHalfAndHalfIsAmbiguous)
		{
			Field f(Grid4K());
			f.FromModel(PanModel(12, 0));
			f.AddObject(f.gridW / 2, 0, f.gridW - f.gridW / 2, f.gridH, -12, 3);
			f.AddNoise(0.05, 52);
			Fitter fitter;
			const CameraMotion r = fitter.Fit(f.Input(kPairSeconds, 5));
			Require(r.confidence < 0.5 && r.reason == Reason::Ambiguous, Fmt("two layers 50/50", r));
		}

		TEST_METHOD(ObjectOverBackgroundSixtyForty)
		{
			Field f(Grid4K());
			f.FromModel(PanModel(12, 0));
			const int x0 = static_cast<int>(0.4 * f.gridW);
			f.AddObject(x0, 0, f.gridW - x0, f.gridH, -30, 8);
			f.AddNoise(0.05, 53);
			Fitter fitter;
			const CameraMotion r = fitter.Fit(f.Input(kPairSeconds, 5));
			Require(r.confidence < 0.5 && r.reason == Reason::Ambiguous, Fmt("60% object over 40% pan", r));
			// Is the verdict gate what keeps it below 0.5? (documentation: logged, not asserted either way)
			Config noGate;
			noGate.verdictGatesConfidence = false;
			Fitter ungated(noGate);
			const CameraMotion u = ungated.Fit(f.Input(kPairSeconds, 5));
			Logger::WriteMessage((Fmt("without verdict gate", u)).c_str());
			Require(u.confidence >= r.confidence, Fmt("gate must never raise confidence", u));
		}

		// Known limitation: a coherent object over ~50% of the picture, over a background of incoherent vectors, is
		// indistinguishable from a pan behind a crowd: it IS the dominant motion. The ambiguity signal only catches a
		// coherent competitor with at least about a third of the camera's support. The verdict is logged.
		TEST_METHOD(CoherentObjectOverRandomBackgroundIsFittedAsDominant)
		{
			Field f(Grid4K());
			SplitMix64 rng(71);
			for (size_t i = 0; i < f.Count(); ++i)
			{
				f.fx[i] = static_cast<float>((rng.Unit() - 0.5) * 80);
				f.fy[i] = static_cast<float>((rng.Unit() - 0.5) * 80);
			}
			const int x0 = f.gridW / 2;
			f.AddObject(x0, 0, f.gridW - x0, f.gridH, -20, 6);
			f.AddNoise(0.05, 72);
			Fitter fitter;
			const CameraMotion r = fitter.Fit(f.Input(kPairSeconds, 5));
			Logger::WriteMessage(Fmt("50% coherent object over random background", r).c_str());
			Require(r.valid && std::fabs(r.params.h[2] + 20) < 1.0 && std::fabs(r.params.h[5] - 6) < 1.0, Fmt("the object's motion is the fitted model", r));
			Require(r.reason == Reason::Ok && r.confidence >= 0.5, Fmt("documented limitation: confident", r));
		}

		TEST_METHOD(NewDefaultsAreDeterministic)
		{
			Field f(Grid4K());
			f.FromModel(Pan4K());
			SplitMix64 rng(61);
			for (int y = 0; y + 1 < f.gridH; y += 2)
				for (int x = 0; x + 1 < f.gridW; x += 2)
					if (rng.Unit() < 0.45) f.AddObject(x, y, 2, 2, (rng.Unit() - 0.5) * 80, (rng.Unit() - 0.5) * 80);
			f.AddNoise(0.05, 62);
			Fitter f1, f2;
			for (uint64_t i = 0; i < 4; ++i)
			{
				const CameraMotion a = f1.Fit(f.Input(kPairSeconds, 5 + i));
				const CameraMotion b = f2.Fit(f.Input(kPairSeconds, 5 + i));
				Require(SameParams(a.params, b.params), "params differ");
				Require(a.confidence == b.confidence && a.reason == b.reason && a.valid == b.valid, "confidence / reason differ");
				Require(SameSignals(a.signals, b.signals), "signals differ");
			}
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
				// Only a text guard excludes; an unguarded static logo is an Overlay that stays searchable.
				const BlockData& bd = *r.blocks;
				int excluded = 0;
				for (int y = 8; y < 12; ++y)
					for (int x = 10; x < 14; ++x)
						excluded += bd.excluded[static_cast<size_t>(y) * bd.gridW + x];
				Require(excluded == (guarded ? 16 : 0), Fmt(tag.c_str(), r) + " excluded flag");
				Require(bd.absThresholdPx > 0.0f && bd.relThreshold > 0.0f, Fmt(tag.c_str(), r) + " stored thresholds");
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
			{
				const BlockData& bd = *r.blocks;
				int excluded = 0;
				for (int x = 60; x < 180; ++x) excluded += bd.excluded[static_cast<size_t>(90) * bd.gridW + x];
				Require(excluded == 120, Fmt("credits", r) + " guarded tiles excluded");
			}
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
			Require(SameParams(a.params, b.params), "params differ");
			Require(a.confidence == b.confidence && a.hypotheses == b.hypotheses && a.samples == b.samples && a.reason == b.reason, "scalars differ");
			Require(SameSignals(a.signals, b.signals), "signals differ");
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

			// A deadline that has passed when the fit completes yields no model (valid=false,
			// DeadlineMissed); the hypothesis search is also truncated once the deadline has passed.
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

		// ---- model type step-down on the early-exit path ----
		TEST_METHOD(HomographyStepsDownWhenTheMotionBecomesAPurePan)
		{
			// Perspective strength fades out over a few pairs (so every pair's previous model is a good start and the
			// early-exit path is taken), then the motion is a pure pan: the model must relax by one type per pair.
			const GridSpec g = Grid4K();
			const double W = g.w * g.blockSize;
			auto model = [&](double a)
			{
				const double h[9] = { 1, 0, 3.0, 0, 1, -2.0, 0.04 / W * a, 0.02 / W * a, 1 };
				return MakeParams(a > 0 ? ModelType::Homography : ModelType::Translation, h);
			};
			const double strength[] = { 1.0, 0.6, 0.3, 0.1, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
			Fitter fitter;
			std::string types;
			int firstLow = -1, firstZero = -1, sawHomography = 0;
			for (int i = 0; i < 10; ++i)
			{
				Field f(g);
				f.FromModel(model(strength[i]));
				f.AddNoise(0.05, 300 + i);
				const CameraMotion r = fitter.Fit(f.Input(kPairSeconds, 5 + i));
				const std::string tag = "pair " + std::to_string(i);
				Require(r.valid && r.confidence >= 0.5, Fmt(tag.c_str(), r));
				Require(ModelError(r, model(strength[i]), f.picture) < kTol, Fmt(tag.c_str(), r));
				types += std::to_string(static_cast<int>(r.params.type));
				if (r.params.type == ModelType::Homography) sawHomography = 1;
				if (strength[i] == 0.0)
				{
					if (firstZero < 0) firstZero = i;
					if (firstLow < 0 && static_cast<int>(r.params.type) <= kSim) firstLow = i;
				}
			}
			Logger::WriteMessage(("CAMERA_STEPDOWN model types per pair (1 trans, 2 sim, 3 affine, 4 homography): " + types + "\n").c_str());
			Require(sawHomography == 1, "the sequence never reached a homography: " + types);
			Require(firstLow >= 0 && firstLow - firstZero <= 3, "the model did not relax to Similarity or Translation within 4 pure-pan pairs: " + types);
		}

		// ---- recycled block data while an earlier record is held ----
		struct Snapshot
		{
			double h[9];
			double confidence;
			std::vector<uint8_t> label, objectId, tinyMover, excluded;
			std::vector<float> resX, resY, modelX, modelY;
			float absThr, relThr;
			explicit Snapshot(const CameraMotion& r)
			{
				std::memcpy(h, r.params.h, sizeof(h));
				confidence = r.confidence;
				const BlockData& b = *r.blocks;
				label = b.label; objectId = b.objectId; tinyMover = b.tinyMover; excluded = b.excluded;
				resX = b.resX; resY = b.resY; modelX = b.modelX; modelY = b.modelY;
				absThr = b.absThresholdPx; relThr = b.relThreshold;
			}
			bool operator==(const Snapshot& o) const
			{
				return std::memcmp(h, o.h, sizeof(h)) == 0 && std::memcmp(&confidence, &o.confidence, sizeof(confidence)) == 0 &&
					label == o.label && objectId == o.objectId && tinyMover == o.tinyMover && excluded == o.excluded &&
					resX == o.resX && resY == o.resY && modelX == o.modelX && modelY == o.modelY &&
					std::memcmp(&absThr, &o.absThr, 4) == 0 && std::memcmp(&relThr, &o.relThr, 4) == 0;
			}
		};

		static Field PairFor(int i)
		{
			Field f(Grid4K());
			f.FromModel(PanModel(10.0 + 0.7 * i, -4.0 + 0.3 * i));
			f.AddNoise(0.05, 400 + i);
			if (i % 2 == 1) f.AddObject(100 + i, 60, 6, 4, -20, 15);
			if (i % 3 == 0) f.AddLogo(10, 8, 4, 4, false);
			return f;
		}

		TEST_METHOD(HeldRecordSurvivesPoolRecyclingAndRefitIsBitwiseEqual)
		{
			std::vector<Snapshot> first;
			Fitter fitter;
			CameraMotion held;
			std::vector<const BlockData*> addresses;
			for (int i = 0; i < 6; ++i)
			{
				Field f = PairFor(i);
				CameraMotion r = fitter.Fit(f.Input(kPairSeconds, 5 + i));
				Require(r.valid && r.blocks != nullptr, Fmt("pair", r));
				first.push_back(Snapshot(r));
				if (i == 0)
					held = r;   // HOLD pair 1's record (keeps its BlockData alive); the rest are dropped
				else
					addresses.push_back(r.blocks.get());
			}
			Require(held.blocks != nullptr, "held record has blocks");
			for (const BlockData* a : addresses)
				Require(a != held.blocks.get(), "a held BlockData was recycled");
			// Pool recycling happened for the unheld records: fewer distinct buffers than records.
			std::set<const BlockData*> distinct(addresses.begin(), addresses.end());
			Require(distinct.size() < addresses.size(), "no BlockData was recycled (" + std::to_string(distinct.size()) + " distinct of " +
				std::to_string(addresses.size()) + ")");
			// The held record is bitwise what it was right after fitting.
			Require(Snapshot(held) == first[0], "the held record's blocks changed after pool recycling");

			// A fresh Fitter over the same sequence gives bitwise-equal records for every pair.
			Fitter again;
			for (int i = 0; i < 6; ++i)
			{
				Field f = PairFor(i);
				const CameraMotion r = again.Fit(f.Input(kPairSeconds, 5 + i));
				Require(Snapshot(r) == first[static_cast<size_t>(i)], "refit differs at pair " + std::to_string(i));
			}
		}
	};

	// Two-layer foreground fix (K10a, Config::twoLayer). Every test states its Config explicitly.
	TEST_CLASS(CameraTwoLayerTests)
	{
	public:
		static Config TL() { Config c; c.twoLayer = true; return c; }

		static void RandomField(Field& f, uint64_t seed, double amplitude)
		{
			SplitMix64 rng(seed);
			for (size_t i = 0; i < f.Count(); ++i)
			{
				f.fx[i] = static_cast<float>((rng.Unit() - 0.5) * 2 * amplitude);
				f.fy[i] = static_cast<float>((rng.Unit() - 0.5) * 2 * amplitude);
			}
		}

		static std::string Flags(const CameraMotion& r)
		{
			return Describe(r) + " flags=" + std::to_string(static_cast<int>(r.layerFlags)) + " risk=" + std::to_string(r.layerRisk)
				+ " obj=" + std::to_string(r.haveObjectLayer ? r.objectLayer.id : 0);
		}

		static double ObjectError(const CameraMotion& r, const Params& truth, const Rect& pic)
		{
			Params p;
			p.type = r.objectLayer.type;
			for (int i = 0; i < 9; ++i) p.h[i] = r.objectLayer.h[i];
			return MaxCornerDifference(p, truth, pic);
		}

		// (a) twoLayer off keeps today's verdict on a coherent object over a random background (documented limitation);
		// the original test CoherentObjectOverRandomBackgroundIsFittedAsDominant holds it. (b) The same field, twoLayer on:
		// a right-half object touches three picture edges, so there is no border evidence against it. The verdict is logged.
		TEST_METHOD(RightHalfObjectOverRandomBackgroundTwoLayerVerdictIsLogged)
		{
			Field f(Grid4K());
			RandomField(f, 71, 40);
			const int x0 = f.gridW / 2;
			f.AddObject(x0, 0, f.gridW - x0, f.gridH, -20, 6);
			f.AddNoise(0.05, 72);
			Fitter off;
			const CameraMotion ro = off.Fit(f.Input(kPairSeconds, 5));
			Require(ro.layerFlags == 0 && !ro.haveObjectLayer && ro.layerRisk == 0, "twoLayer off: no layer fields " + Flags(ro));
			Fitter on(TL());
			const CameraMotion r = on.Fit(f.Input(kPairSeconds, 5));
			Logger::WriteMessage(("right-half object over random background, twoLayer on: " + Flags(r)).c_str());
			Require((r.layerFlags & 1) != 0, "the two-layer path ran " + Flags(r));
			// Known limitation (no border evidence, no camera in the field): the object is still reported as dominant. The
			// two-layer path must never be MORE confident than today.
			Require(r.confidence <= ro.confidence + 1e-12, "no more lenient than off: " + Flags(r) + " vs " + Flags(ro));
		}

		// (c) A centred object (~50 % of the picture, away from the border) over a random background: the border evidence
		// makes the verdict Ambiguous, never usable, and the record says why (ForegroundSuspect).
		TEST_METHOD(CentredObjectOverRandomBackgroundIsForegroundSuspect)
		{
			Field f(Grid4K());
			RandomField(f, 71, 40);
			f.AddObject(36, 19, 168, 97, -20, 6);   // 70 % x 72 % of the grid, inside the border ring
			f.AddNoise(0.05, 73);
			Fitter fitter(TL());
			const CameraMotion r = fitter.Fit(f.Input(kPairSeconds, 5));
			Require(r.reason == Reason::Ambiguous && r.confidence < 0.5, "centred object: " + Flags(r));
			Require((r.layerFlags & 8) != 0, "ForegroundSuspect flag: " + Flags(r));
			Require(r.haveObjectLayer && r.objectLayer.id >= 1, "the suspect layer is reported as the object layer: " + Flags(r));
			Require(r.signals.border < 0.35, "border signal " + std::to_string(r.signals.border));
			Fitter off;
			const CameraMotion ro = off.Fit(f.Input(kPairSeconds, 5));
			Logger::WriteMessage(("same field, twoLayer off: " + Flags(ro)).c_str());
		}

		// (d) A centred object over a pan background with 40 % of the background random: the object is the RANSAC dominant
		// motion but the camera is the pan (the layer touching the border).
		TEST_METHOD(CentredObjectOverPanWithRandomPartKeepsThePanAsCamera)
		{
			const Params pan = PanModel(12, -3);
			Field f(Grid4K());
			f.FromModel(pan);
			SplitMix64 rng(81);
			for (int y = 0; y < f.gridH; ++y)
				for (int x = 0; x < f.gridW; ++x)
					if (rng.Unit() < 0.4)
					{
						f.fx[f.Index(x, y)] = static_cast<float>((rng.Unit() - 0.5) * 80);
						f.fy[f.Index(x, y)] = static_cast<float>((rng.Unit() - 0.5) * 80);
					}
			f.AddObject(36, 19, 168, 97, -20, 6);
			f.AddNoise(0.05, 82);
			Fitter fitter(TL());
			const CameraMotion r = fitter.Fit(f.Input(kPairSeconds, 5));
			Require(r.valid && ModelError(r, pan, f.picture) < kTol, "camera is the pan: " + Flags(r) + " err=" + std::to_string(ModelError(r, pan, f.picture)));
			Require((r.layerFlags & 4) != 0, "camera is the RANSAC runner-up: " + Flags(r));
			Require(r.haveObjectLayer && ObjectError(r, PanModel(-20, 6), f.picture) < 2e-3, "object layer is the object motion: " + Flags(r));
			Fitter off;
			const CameraMotion ro = off.Fit(f.Input(kPairSeconds, 5));
			Logger::WriteMessage(("same field, twoLayer off: " + Flags(ro) + " err=" + std::to_string(ModelError(ro, pan, f.picture))).c_str());
		}

		// A centred coherent boat over water (small random vectors plus a weak drift): never a usable fit.
		TEST_METHOD(BoatLikeForegroundOverWaterIsNeverUsable)
		{
			for (uint64_t seed = 0; seed < 4; ++seed)
			{
				Field f(Grid4K());
				RandomField(f, 90 + seed, 3.0);
				for (size_t i = 0; i < f.Count(); ++i) { f.fx[i] += 0.3f; }
				f.AddObject(78, 44, 84, 47, 4, -1);     // ~12 % of the grid, centred
				f.AddNoise(0.05, 95 + seed);
				Fitter fitter(TL());
				const CameraMotion r = fitter.Fit(f.Input(kPairSeconds, 5 + seed));
				Require(!(r.valid && r.confidence >= 0.5), "boat over water must not be usable: " + Flags(r));
			}
		}

		// The camera follows a nearly static subject: the subject (120 x 108 of 240 x 135 blocks = 40 %, centred) is the minority layer, the panning
		// background is the camera.
		TEST_METHOD(TrackedSubjectPanningBackground)
		{
			const Params pan = PanModel(-12, 2);
			Field f(Grid4K());
			f.FromModel(pan);
			f.AddObject(60, 14, 120, 108, 0, 0);
			f.AddNoise(0.05, 101);
			Fitter fitter(TL());
			const CameraMotion r = fitter.Fit(f.Input(kPairSeconds, 5));
			Require(r.valid && ModelError(r, pan, f.picture) < kTol, "camera is the pan: " + Flags(r));
			Require(r.haveObjectLayer && ObjectError(r, PanModel(0, 0), f.picture) < 2e-3, "object layer is the static subject: " + Flags(r));
			// 60/40 stays Ambiguous exactly as with twoLayer off: continuity and border evidence choose the layer, they never
			// make a record confident.
			Fitter off;
			const CameraMotion ro = off.Fit(f.Input(kPairSeconds, 5));
			Require(r.confidence <= ro.confidence + 1e-12, "no more lenient than off: " + Flags(r) + " vs " + Flags(ro));
		}

		// An object enters from the right edge and grows to ~55 % of the picture over a pan: the camera stays the pan.
		TEST_METHOD(ObjectEntersFromEdgeCameraStaysThePan)
		{
			const Params pan = PanModel(12, -3);
			Fitter fitter(TL());
			for (int i = 0; i < 14; ++i)
			{
				Field f(Grid4K());
				f.FromModel(pan);
				const int w = std::min(static_cast<int>(0.55 * f.gridW), 4 * i * f.gridW / 100 * 2);
				if (w > 0) f.AddObject(f.gridW - w, 4, w, f.gridH - 8, -30, 8);
				f.AddNoise(0.05, 110 + i);
				const CameraMotion r = fitter.Fit(f.Input(kPairSeconds, 5 + i));
				const double e = ModelError(r, pan, f.picture);
				Logger::WriteMessage(("enter pair " + std::to_string(i) + " w=" + std::to_string(w) + ": " + Flags(r) + " err=" + std::to_string(e)).c_str());
				Require(r.valid && e < kTol, "pair " + std::to_string(i) + ": camera must stay the pan: " + Flags(r) + " err=" + std::to_string(e));
			}
		}

		// Sequence P (pan) 80 % / Q (-12, 3) 20 %, then Q 85 % / P 15 %: Q now wins the score but is not continuous with the
		// reference, P still is. P is held for M-1 pairs, Q is accepted at pair M (flags 32, layerRisk 1, decaying to 0 over M pairs).
		static void SwapHysteresisAt(double pairSeconds, uint64_t seqBase)
		{
			const Params P = PanModel(12, 0), Q = PanModel(-12, 3);
			const int M = std::max(1, static_cast<int>(std::lround(Config().layerSwapSeconds / pairSeconds)));
			Fitter fitter(TL());
			auto fieldPQ = [&](int pFraction100, uint64_t seed)
			{
				Field f(Grid4K());
				f.FromModel(P);
				const int qw = f.gridW * (100 - pFraction100) / 100;
				f.AddObject(f.gridW - qw, 0, qw, f.gridH, -12, 3);
				f.AddNoise(0.05, seed);
				return f;
			};
			uint64_t seq = seqBase;
			for (int i = 0; i < 3; ++i)
			{
				Field f = fieldPQ(80, 120 + i);
				const CameraMotion r = fitter.Fit(f.Input(pairSeconds, seq++));
				Require(r.valid && r.reason == Reason::Ok && r.confidence >= 0.5 && ModelError(r, P, f.picture) < kTol, "establish P: " + Flags(r));
			}
			double prevRisk = 2;
			for (int i = 1; i <= 2 * M + 2; ++i)
			{
				Field f = fieldPQ(15, 130 + i);
				const CameraMotion r = fitter.Fit(f.Input(pairSeconds, seq++));
				const std::string tag = "M=" + std::to_string(M) + " pair " + std::to_string(i) + ": " + Flags(r);
				Logger::WriteMessage(tag.c_str());
				if (i < M)
				{
					Require(ModelError(r, P, f.picture) < kTol, "held on P " + tag);
					Require((r.layerFlags & 16) != 0 && (r.layerFlags & 32) == 0, "SwapPending " + tag);
				}
				else if (i == M)
				{
					Require(ModelError(r, Q, f.picture) < kTol, "accepted Q " + tag);
					Require((r.layerFlags & 32) != 0 && (r.layerFlags & 16) == 0 && r.layerRisk == 1.0, "Swapped, layerRisk 1 " + tag);
					prevRisk = r.layerRisk;
				}
				else
				{
					Require(ModelError(r, Q, f.picture) < kTol, "stays Q " + tag);
					Require((r.layerFlags & 32) == 0, "swap reported once " + tag);
					if (i - M < M)
						Require(r.layerRisk < prevRisk && r.layerRisk > 0, "risk decays " + tag);
					else
						Require(r.layerRisk == 0, "risk gone " + tag);
					prevRisk = r.layerRisk;
				}
			}
		}

		TEST_METHOD(SwapHysteresisAt24PairsPerSecond) { SwapHysteresisAt(1.0 / 24, 200); }
		TEST_METHOD(SwapHysteresisAt60PairsPerSecond) { SwapHysteresisAt(1.0 / 60, 300); }

		// A single layer that changes its motion (a whip pan) is accepted at once: no hold, no risk.
		TEST_METHOD(WhipPanSingleLayerAcceptedAtOnce)
		{
			Fitter fitter(TL());
			const Params slow = PanModel(8, 0), fast = PanModel(70, -20);
			for (int i = 0; i < 4; ++i)
			{
				Field f(Grid4K());
				f.FromModel(slow);
				f.AddNoise(0.05, 140 + i);
				const CameraMotion r = fitter.Fit(f.Input(kPairSeconds, 5 + i));
				Require(r.valid && r.confidence >= 0.5, "establish: " + Flags(r));
			}
			Field w(Grid4K());
			w.FromModel(fast);
			w.AddNoise(0.05, 150);
			const CameraMotion r = fitter.Fit(w.Input(kPairSeconds, 20));
			Require(r.valid && ModelError(r, fast, w.picture) < kTol, "whip pan fitted at once: " + Flags(r));
			Require((r.layerFlags & 16) == 0 && r.layerRisk == 0, "no hold, no risk: " + Flags(r));
		}

		// Two layers present, the reference layer vanishes (both layers change motion): the new layer is accepted at once.
		TEST_METHOD(WhipPanTwoLayersReferenceVanishesAcceptedAtOnce)
		{
			Fitter fitter(TL());
			const Params P = PanModel(12, 0), P2 = PanModel(60, -15);
			for (int i = 0; i < 3; ++i)
			{
				Field f(Grid4K());
				f.FromModel(P);
				const int qw = f.gridW * 20 / 100;
				f.AddObject(f.gridW - qw, 0, qw, f.gridH, -12, 3);
				f.AddNoise(0.05, 160 + i);
				const CameraMotion r = fitter.Fit(f.Input(kPairSeconds, 5 + i));
				Require(r.valid && r.confidence >= 0.5, "establish: " + Flags(r));
			}
			Field f(Grid4K());
			f.FromModel(P2);
			const int qw = f.gridW * 20 / 100;
			f.AddObject(f.gridW - qw, 0, qw, f.gridH, -40, 20);
			f.AddNoise(0.05, 170);
			const CameraMotion r = fitter.Fit(f.Input(kPairSeconds, 20));
			Require(ModelError(r, P2, f.picture) < kTol, "new layer accepted at once: " + Flags(r));
			Require((r.layerFlags & 32) != 0 && (r.layerFlags & 16) == 0, "Swapped, not held: " + Flags(r));
		}

		// A cut clears the two-layer state (reference, hold count, risk), also with a queued next pair for the post-cut seed.
		TEST_METHOD(CutClearsTwoLayerState)
		{
			const Params P = PanModel(12, 0);
			auto fieldPQ = [&](int pPercent, uint64_t seed)
			{
				Field f(Grid4K());
				f.FromModel(P);
				const int qw = f.gridW * (100 - pPercent) / 100;
				f.AddObject(f.gridW - qw, 0, qw, f.gridH, -12, 3);
				f.AddNoise(0.05, seed);
				return f;
			};
			Fitter fitter(TL());
			uint64_t seq = 5;
			for (int i = 0; i < 3; ++i)
				fitter.Fit(fieldPQ(80, 180 + i).Input(kPairSeconds, seq++));
			const CameraMotion held = fitter.Fit(fieldPQ(15, 190).Input(kPairSeconds, seq++));
			Require((held.layerFlags & 16) != 0 && (held.layerFlags & 64) != 0, "hold in progress before the cut: " + Flags(held));
			Field after = fieldPQ(15, 191);
			PairInput cut = fieldPQ(15, 192).Input(kPairSeconds, seq++);
			cut.cut = true;
			const PairInput nextIn = after.Input(kPairSeconds, seq++);
			cut.next = &nextIn;
			const CameraMotion rc = fitter.Fit(cut);
			Require(rc.reason == Reason::PostCutSeeded || rc.reason == Reason::Cut, "cut record: " + Flags(rc));
			const CameraMotion r = fitter.Fit(after.Input(kPairSeconds, seq++));
			Require((r.layerFlags & (16 | 64)) == 0 && r.layerRisk == 0, "no hold, no continuity reference after the cut: " + Flags(r));
		}

		static std::vector<CameraMotion> Sequence(const Config& c)
		{
			Fitter fitter(c);
			std::vector<CameraMotion> out;
			for (int i = 0; i < 8; ++i)
			{
				Field f(Grid4K());
				f.FromModel(PanModel(10.0 + 0.7 * i, -4.0 + 0.3 * i));
				f.AddNoise(0.05, 400 + i);
				if (i % 2 == 1) f.AddObject(100 + i, 60, 6, 4, -20, 15);
				if (i % 3 == 0) f.AddLogo(10, 8, 4, 4, false);
				if (i >= 4) f.AddObject(36, 19, 100, 60, -25, 7);
				out.push_back(fitter.Fit(f.Input(kPairSeconds, 5 + i)));
			}
			return out;
		}

		static bool SameRecord(const CameraMotion& a, const CameraMotion& b)
		{
			return a.valid == b.valid && a.reason == b.reason && SameBits(a.confidence, b.confidence) && SameParams(a.params, b.params)
				&& SameSignals(a.signals, b.signals) && SameBlocks(a, b) && a.layerFlags == b.layerFlags
				&& a.haveObjectLayer == b.haveObjectLayer && a.objectLayer.id == b.objectLayer.id && SameBits(a.layerRisk, b.layerRisk)
				&& SameBits(a.objectLayer.h[2], b.objectLayer.h[2]) && SameBits(a.objectLayer.h[5], b.objectLayer.h[5]);
		}

		// Identity with the pre-change fitter is shown by the camf comparison of camera_eval vs camera_eval_base.exe (real clips,
		// bit-identical .camf); this test shows that, with twoLayer off, the other two-layer constants change nothing and that
		// the layer fields stay empty.
		// twoLayer = false: the records equal the default-configuration records bit for bit, whatever the other two-layer
		// constants are, and carry no layer fields.
		TEST_METHOD(TwoLayerOffIsByteIdentical)
		{
			Config odd;
			odd.twoLayer = false;
			odd.borderRingFraction = 0.3; odd.layerSwapSeconds = 2; odd.borderVetoRatio = 0.9; odd.continuityFloor = 0.9; odd.competitorMinRatio = 0.9;
			const std::vector<CameraMotion> a = Sequence(Config()), b = Sequence(odd);
			for (size_t i = 0; i < a.size(); ++i)
			{
				Require(SameRecord(a[i], b[i]), "record " + std::to_string(i) + " differs");
				Require(a[i].layerFlags == 0 && !a[i].haveObjectLayer && a[i].layerRisk == 0, "no layer fields when off: " + Flags(a[i]));
				Require(a[i].signals.border == 0.5 && a[i].signals.continuity == 0.5, "neutral diagnostics when off");
			}
		}

		TEST_METHOD(TwoLayerIsDeterministic)
		{
			const std::vector<CameraMotion> a = Sequence(TL()), b = Sequence(TL());
			for (size_t i = 0; i < a.size(); ++i)
				Require(SameRecord(a[i], b[i]), "record " + std::to_string(i) + " differs between two runs");
		}

		// The verdicts of the existing single-layer tests survive twoLayer on.
		TEST_METHOD(ExistingVerdictsSurviveTwoLayerOn)
		{
			{
				Field f(Grid4K());
				f.FromModel(PanModel(12, 0));
				f.AddObject(f.gridW / 2, 0, f.gridW - f.gridW / 2, f.gridH, -12, 3);
				f.AddNoise(0.05, 52);
				Fitter fitter(TL());
				const CameraMotion r = fitter.Fit(f.Input(kPairSeconds, 5));
				Require(r.confidence < 0.5 && r.reason == Reason::Ambiguous, Fmt("50/50", r));
			}
			{
				Field f(Grid4K());
				f.FromModel(PanModel(12, 0));
				const int x0 = static_cast<int>(0.4 * f.gridW);
				f.AddObject(x0, 0, f.gridW - x0, f.gridH, -30, 8);
				f.AddNoise(0.05, 53);
				Fitter fitter(TL());
				const CameraMotion r = fitter.Fit(f.Input(kPairSeconds, 5));
				Require(r.confidence < 0.5 && r.reason == Reason::Ambiguous, Fmt("60/40", r));
			}
			{
				Field f(Grid4K());
				f.FromModel(PanModel(12, 0));
				f.AddObject(static_cast<int>(0.3 * f.gridW), 0, f.gridW - static_cast<int>(0.3 * f.gridW), f.gridH, -30, 8);
				f.AddNoise(0.05, 43);
				Fitter fitter(TL());
				const CameraMotion r = fitter.Fit(f.Input(kPairSeconds, 5));
				Require(r.confidence < 0.5, Fmt("70% object", r));
			}
			{
				const Params truth = Pan4K();
				Field f(Grid4K());
				f.FromModel(truth);
				SplitMix64 rng(41);
				for (int y = 0; y + 1 < f.gridH; y += 2)
					for (int x = 0; x + 1 < f.gridW; x += 2)
						if (rng.Unit() < 0.45) f.AddObject(x, y, 2, 2, (rng.Unit() - 0.5) * 80, (rng.Unit() - 0.5) * 80);
				f.AddNoise(0.05, 42);
				Fitter fitter(TL());
				const CameraMotion r = fitter.Fit(f.Input(kPairSeconds, 5));
				Require(r.valid && r.reason == Reason::Ok && r.confidence >= 0.5 && ModelError(r, truth, f.picture) < kTol, Fmt("crowd under pan", r));
			}
			{
				const Params truth = Pan4K();
				Field bars(Grid4K());
				bars.FromModel(truth);
				bars.AddNoise(0.05, 53);
				bars.AddBars(17, 17);
				Fitter fitter(TL());
				const CameraMotion rb = fitter.Fit(bars.Input(kPairSeconds, 5));
				Require(rb.valid && rb.confidence >= 0.5 && ModelError(rb, truth, bars.picture) < kTol, Fmt("letterbox", rb));
			}
		}
	};

	// R1-GATES: the dynamic-texture (water-like) detector (Config::dynTexture).
	TEST_CLASS(CameraDynTextureTests)
	{
	public:
		static Config DT() { Config c; c.dynTexture = true; return c; }

		// A pan whose reliable block per cell (trust 1) follows the camera while every other block is water: random vectors
		// (amplitude 3 px) of lower trust, different in every pair. The fitter samples one block per cell, so the fit itself is
		// clean; the detector sees the whole block field.
		static Field AnchoredWater(uint64_t seed, double amplitude, double waterShareOfBlocks = 1.0)
		{
			Field f(Grid4K());
			f.FromModel(Pan4K());
			SplitMix64 rng(seed);
			for (int y = 0; y < f.gridH; ++y)
				for (int x = 0; x < f.gridW; ++x)
				{
					const size_t i = f.Index(x, y);
					const bool anchor = (x % 5 == 2) && (y % 5 == 2);
					if (anchor) continue;
					f.trust[i] = 0.9f;
					if (rng.Unit() <= waterShareOfBlocks)
					{
						f.fx[i] += static_cast<float>((rng.Unit() - 0.5) * 2 * amplitude);
						f.fy[i] += static_cast<float>((rng.Unit() - 0.5) * 2 * amplitude);
					}
				}
			return f;
		}

		static std::vector<CameraMotion> WaterSequence(const Config& c, int pairs)
		{
			Fitter fitter(c);
			std::vector<CameraMotion> out;
			for (int i = 0; i < pairs; ++i)
			{
				Field f = AnchoredWater(700 + i, 3.0);
				out.push_back(fitter.Fit(f.Input(kPairSeconds, 5 + i)));
			}
			return out;
		}

		TEST_METHOD(WaterLikeFieldIsAmbiguousWithReasonCode)
		{
			// Premise: without the detector the clean anchors make the fit usable.
			const std::vector<CameraMotion> off = WaterSequence(Config(), 3);
			for (const CameraMotion& r : off)
				Require(r.valid && r.reason == Reason::Ok && r.confidence >= 0.5 && r.verdictFlags == 0, "premise (detector off): " + Describe(r));
			const std::vector<CameraMotion> on = WaterSequence(DT(), 3);
			for (size_t i = 0; i < on.size(); ++i)
			{
				const CameraMotion& r = on[i];
				Require(r.valid && r.reason == Reason::Ambiguous && r.confidence < 0.5 && (r.verdictFlags & kVerdictDynamicTexture) != 0,
					"water-like field must be Ambiguous with the dynamic-texture flag, pair " + std::to_string(i) + ": " + Describe(r));
				Require(!ReasonUsable(r.reason), "never usable by a consumer");
				Require(std::string(ReasonTextOf(r.reason, r.verdictFlags)).find("dynamic texture") != std::string::npos, "status text names the cause");
				Require(r.signals.dynShare >= 0.5 && r.signals.dynSpatial >= 0.5, "statistics recorded: share=" + std::to_string(r.signals.dynShare)
					+ " spatial=" + std::to_string(r.signals.dynSpatial));
				if (i > 0)
					Require(r.signals.dynTemporal >= 0.6, "temporal incoherence recorded: " + std::to_string(r.signals.dynTemporal));
			}
		}

		// An invalid fit between two water pairs resets the detector history: the pair after it has no previous residual field.
		TEST_METHOD(InvalidFitResetsTheDetectorHistory)
		{
			Fitter fitter(DT());
			const CameraMotion a = fitter.Fit(AnchoredWater(900, 3.0).Input(kPairSeconds, 5));
			Require(a.valid, "premise: first water pair fits: " + Describe(a));
			Field empty(Grid4K());
			empty.FromModel(Pan4K());
			for (size_t i = 0; i < empty.trust.size(); ++i) empty.trust[i] = 0.0f;
			const CameraMotion bad = fitter.Fit(empty.Input(kPairSeconds, 6));
			Require(!bad.valid, "premise: a field without trusted blocks is not a valid fit: " + Describe(bad));
			const CameraMotion c = fitter.Fit(AnchoredWater(901, 3.0).Input(kPairSeconds, 7));
			Require(c.valid && c.signals.dynTemporal == 0.0, "no temporal term across an invalid pair, got " + std::to_string(c.signals.dynTemporal));
			Fitter control(DT());
			control.Fit(AnchoredWater(900, 3.0).Input(kPairSeconds, 5));
			const CameraMotion d = control.Fit(AnchoredWater(901, 3.0).Input(kPairSeconds, 6));
			Require(d.signals.dynTemporal > 0.0, "control: consecutive valid pairs do have a temporal term");
		}

		// Real-looking content stays usable: a pan, a pan with a coherent 40 % foreground, a pan with a small-object crowd
		// (45 % of the blocks in 2 x 2 movers with random vectors).
		TEST_METHOD(PansCrowdsAndForegroundsAreNotFlagged)
		{
			Fitter fitter(DT());
			for (int i = 0; i < 4; ++i)
			{
				Field f(Grid4K());
				f.FromModel(Pan4K());
				f.AddNoise(0.05, 800 + i);
				const CameraMotion r = fitter.Fit(f.Input(kPairSeconds, 5 + i));
				Require(r.valid && r.reason == Reason::Ok && r.confidence >= 0.5 && r.verdictFlags == 0, "pan: " + Describe(r));
			}
			{
				Fitter fc(DT());
				for (int i = 0; i < 3; ++i)
				{
					Field f(Grid4K());
					f.FromModel(Pan4K());
					SplitMix64 rng(41 + i);
					for (int y = 0; y + 1 < f.gridH; y += 2)
						for (int x = 0; x + 1 < f.gridW; x += 2)
							if (rng.Unit() < 0.45) f.AddObject(x, y, 2, 2, (rng.Unit() - 0.5) * 80, (rng.Unit() - 0.5) * 80);
					f.AddNoise(0.05, 42 + i);
					const CameraMotion r = fc.Fit(f.Input(kPairSeconds, 5 + i));
					Require(r.valid && r.reason == Reason::Ok && r.confidence >= 0.5 && r.verdictFlags == 0, "crowd under pan: " + Describe(r));
				}
			}
			{
				Fitter ff(DT());
				for (int i = 0; i < 3; ++i)
				{
					Field f(Grid4K());
					f.FromModel(Pan4K());
					f.AddObject(10, 10, 60, 60, -12, 9);
					f.AddNoise(0.05, 60 + i);
					const CameraMotion r = ff.Fit(f.Input(kPairSeconds, 5 + i));
					Require(r.valid && r.reason == Reason::Ok && r.confidence >= 0.5 && r.verdictFlags == 0, "coherent foreground: " + Describe(r));
				}
			}
		}

		// Off: the detector state is never touched, the record carries no detector fields, whatever the thresholds are.
		TEST_METHOD(DetectorOffIsBitIdentical)
		{
			Config odd;
			odd.dynTexture = false;
			odd.dynTextureCfg.minShare = 0.0; odd.dynTextureCfg.minTemporal = 0.0; odd.dynTextureCfg.minSpatial = 0.0; odd.dynTextureCfg.agreeRatio = 0.01;
			const std::vector<CameraMotion> a = WaterSequence(Config(), 4), b = WaterSequence(odd, 4);
			for (size_t i = 0; i < a.size(); ++i)
			{
				Require(a[i].valid == b[i].valid && a[i].reason == b[i].reason && SameBits(a[i].confidence, b[i].confidence)
					&& SameParams(a[i].params, b[i].params) && SameSignals(a[i].signals, b[i].signals) && SameBlocks(a[i], b[i]), "record " + std::to_string(i) + " differs");
				Require(a[i].verdictFlags == 0 && a[i].signals.dynShare == 0 && a[i].signals.dynTemporal == 0 && a[i].signals.dynSpatial == 0, "no detector fields when off");
			}
		}

		TEST_METHOD(DetectorIsDeterministic)
		{
			const std::vector<CameraMotion> a = WaterSequence(DT(), 4), b = WaterSequence(DT(), 4);
			for (size_t i = 0; i < a.size(); ++i)
				Require(a[i].valid == b[i].valid && a[i].reason == b[i].reason && SameBits(a[i].confidence, b[i].confidence) && SameParams(a[i].params, b[i].params)
					&& SameBits(a[i].signals.dynShare, b[i].signals.dynShare) && SameBits(a[i].signals.dynTemporal, b[i].signals.dynTemporal)
					&& SameBits(a[i].signals.dynSpatial, b[i].signals.dynSpatial) && a[i].verdictFlags == b[i].verdictFlags, "record " + std::to_string(i) + " differs between runs");
		}

		// A cut clears the previous residual field: the first water pair after a cut has no temporal term (detected on share and spatial alone).
		TEST_METHOD(CutClearsDetectorHistory)
		{
			Fitter fitter(DT());
			Field w = AnchoredWater(900, 3.0);
			fitter.Fit(w.Input(kPairSeconds, 5));
			PairInput cut = w.Input(kPairSeconds, 6);
			cut.cut = true;
			fitter.Fit(cut);
			Field w2 = AnchoredWater(901, 3.0);
			const CameraMotion r = fitter.Fit(w2.Input(kPairSeconds, 7));
			Require(r.reason == Reason::Ambiguous && r.signals.dynTemporal == 0.0, "after a cut: " + Describe(r) + " temporal=" + std::to_string(r.signals.dynTemporal));
		}
	};
}
