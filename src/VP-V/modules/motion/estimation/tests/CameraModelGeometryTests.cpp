// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "pch.h"
#include <VP-V/modules/motion/estimation/global_model/GlobalModelGeometry.h>
#include "CppUnitTest.h"
#include <vector>
#include <cmath>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	namespace
	{
		using namespace CameraModel;

		const double kPi = 3.14159265358979323846;

		Rect Pic(int w, int h)
		{
			Rect r;
			r.left = 0; r.top = 0; r.right = w; r.bottom = h;
			return r;
		}

		double HalfDiag(const Rect& r)
		{
			return 0.5 * std::sqrt(static_cast<double>(r.Width()) * r.Width() + static_cast<double>(r.Height()) * r.Height());
		}

		Params MakeKnown(ModelType type, const Rect& pic)
		{
			Params p = IdentityParams(type);
			const double sx = pic.Width() / 3840.0;   // keep shifts moderate on any picture size
			const double th = 2.0 * kPi / 180.0;
			const double s = 1.03;
			switch (type)
			{
			case ModelType::Translation:
				p.h[2] = 40.0 * sx; p.h[5] = -25.0 * sx;
				break;
			case ModelType::Similarity:
				p.h[0] = s * std::cos(th); p.h[1] = -s * std::sin(th); p.h[2] = 40.0 * sx;
				p.h[3] = s * std::sin(th); p.h[4] = s * std::cos(th);  p.h[5] = -25.0 * sx;
				break;
			case ModelType::Affine:
				p.h[0] = 1.02; p.h[1] = 0.015; p.h[2] = 40.0 * sx;
				p.h[3] = -0.01; p.h[4] = 0.985; p.h[5] = -25.0 * sx;
				break;
			case ModelType::Homography:
				p.h[0] = s * std::cos(th); p.h[1] = -s * std::sin(th); p.h[2] = 40.0 * sx;
				p.h[3] = s * std::sin(th); p.h[4] = s * std::cos(th);  p.h[5] = -25.0 * sx;
				p.h[6] = 1e-5 * 3840.0 / pic.Width(); p.h[7] = -8e-6 * 3840.0 / pic.Width(); p.h[8] = 1.0;
				break;
			default:
				break;
			}
			return p;
		}

		Sample MakeSample(const Params& p, double x, double y, double w = 1.0)
		{
			double ox = 0, oy = 0;
			Apply(p, x, y, &ox, &oy);
			Sample s;
			s.x = x; s.y = y; s.dx = ox - x; s.dy = oy - y; s.w = w;
			return s;
		}

		// Largest distance between the two models' images of a probe grid across the picture.
		double ProbeError(const Params& a, const Params& b, const Rect& pic)
		{
			double worst = 0;
			for (int iy = -2; iy <= 2; ++iy)
				for (int ix = -2; ix <= 2; ++ix)
				{
					const double x = ix * 0.2 * pic.Width(), y = iy * 0.2 * pic.Height();
					double ax = 0, ay = 0, bx = 0, by = 0;
					Apply(a, x, y, &ax, &ay);
					Apply(b, x, y, &bx, &by);
					worst = std::max(worst, std::sqrt((ax - bx) * (ax - bx) + (ay - by) * (ay - by)));
				}
			return worst;
		}

		double MatrixError(const Params& a, const Params& b)
		{
			double worst = 0;
			for (int i = 0; i < 9; ++i)
				worst = std::max(worst, std::fabs(a.h[i] - b.h[i]));
			return worst;
		}

		std::vector<Sample> RandomSamples(const Params& p, const Rect& pic, int n, uint64_t seed, double noise)
		{
			SplitMix64 rng(seed);
			std::vector<Sample> v;
			for (int i = 0; i < n; ++i)
			{
				const double x = (rng.Unit() - 0.5) * pic.Width();
				const double y = (rng.Unit() - 0.5) * pic.Height();
				Sample s = MakeSample(p, x, y);
				if (noise > 0)
				{
					s.dx += noise * (2.0 * rng.Unit() - 1.0);
					s.dy += noise * (2.0 * rng.Unit() - 1.0);
				}
				v.push_back(s);
			}
			return v;
		}

		const ModelType kTypes[4] = { ModelType::Translation, ModelType::Similarity, ModelType::Affine, ModelType::Homography };

		// Well-spread sample positions for the minimal solves.
		void MinimalPositions(const Rect& pic, int count, double* xs, double* ys)
		{
			const double fx[4] = { -0.31, 0.37, 0.12, -0.22 };
			const double fy[4] = { -0.27, -0.19, 0.33, 0.29 };
			for (int i = 0; i < count; ++i)
			{
				xs[i] = fx[i] * pic.Width();
				ys[i] = fy[i] * pic.Height();
			}
		}
	}

	TEST_CLASS(CameraGeometryTests)
	{
	public:
		TEST_METHOD(SolveMinimalExactNoiseless)
		{
			const Rect pics[3] = { Pic(3840, 2160), Pic(7680, 4320), Pic(1000, 1000) };
			for (const Rect& pic : pics)
			{
				for (ModelType t : kTypes)
				{
					const Params truth = MakeKnown(t, pic);
					const int n = MinimalCount(t);
					double xs[4], ys[4];
					MinimalPositions(pic, n, xs, ys);
					Sample s[4];
					for (int i = 0; i < n; ++i)
						s[i] = MakeSample(truth, xs[i], ys[i]);
					Params out;
					Assert::IsTrue(SolveMinimal(t, s, n, out), L"minimal solve succeeds");
					Assert::IsTrue(out.type == t, L"type kept");
					Assert::AreEqual(0.0, ProbeError(truth, out, pic), 1e-6, L"minimal solve reproduces the model at probes");
				}
			}
		}

		TEST_METHOD(SolveMinimalRejectsBadInput)
		{
			const Rect pic = Pic(3840, 2160);
			Params out;
			Sample s[4];
			Assert::IsTrue(!SolveMinimal(ModelType::Translation, nullptr, 1, out), L"null");
			Assert::IsTrue(!SolveMinimal(ModelType::None, s, 1, out), L"None");
			Assert::IsTrue(!SolveMinimal(ModelType::Similarity, s, 3, out), L"wrong count");

			// Coincident points.
			s[0] = MakeSample(MakeKnown(ModelType::Similarity, pic), 100, 50);
			s[1] = s[0];
			Assert::IsTrue(!SolveMinimal(ModelType::Similarity, s, 2, out), L"similarity coincident");

			// Collinear points.
			const Params aff = MakeKnown(ModelType::Affine, pic);
			for (int i = 0; i < 3; ++i)
				s[i] = MakeSample(aff, -400.0 + 400.0 * i, -200.0 + 200.0 * i);
			Assert::IsTrue(!SolveMinimal(ModelType::Affine, s, 3, out), L"affine collinear");

			const Params hom = MakeKnown(ModelType::Homography, pic);
			s[0] = MakeSample(hom, -400, -200);
			s[1] = MakeSample(hom, 0, 0);
			s[2] = MakeSample(hom, 400, 200);
			s[3] = MakeSample(hom, 100, 600);
			Assert::IsTrue(!SolveMinimal(ModelType::Homography, s, 4, out), L"homography three collinear");
			s[2] = s[1];
			Assert::IsTrue(!SolveMinimal(ModelType::Homography, s, 4, out), L"homography coincident");
		}

		TEST_METHOD(FitWeightedNoiselessRecoversEachType)
		{
			const Rect pics[3] = { Pic(3840, 2160), Pic(7680, 4320), Pic(1000, 1000) };
			uint64_t seed = 11;
			for (const Rect& pic : pics)
			{
				for (ModelType t : kTypes)
				{
					const Params truth = MakeKnown(t, pic);
					std::vector<Sample> v = RandomSamples(truth, pic, 200, seed++, 0.0);
					Params out;
					Assert::IsTrue(FitWeighted(t, v.data(), static_cast<int>(v.size()), HalfDiag(pic), out), L"fit succeeds");
					Assert::AreEqual(0.0, ProbeError(truth, out, pic), 1e-7, L"noiseless fit recovers the model");
				}
			}
		}

		TEST_METHOD(FitWeightedZeroWeightOutlierIgnored)
		{
			const Rect pic = Pic(3840, 2160);
			uint64_t seed = 99;
			for (ModelType t : kTypes)
			{
				const Params truth = MakeKnown(t, pic);
				std::vector<Sample> v = RandomSamples(truth, pic, 200, seed++, 0.02);
				Params base;
				Assert::IsTrue(FitWeighted(t, v.data(), static_cast<int>(v.size()), HalfDiag(pic), base), L"base fit");
				Sample bad;
				bad.x = 1500; bad.y = -800; bad.dx = 1e9; bad.dy = -1e9; bad.w = 0.0;
				v.insert(v.begin() + 37, bad);
				Params withOutlier;
				Assert::IsTrue(FitWeighted(t, v.data(), static_cast<int>(v.size()), HalfDiag(pic), withOutlier), L"outlier fit");
				Assert::AreEqual(0.0, MatrixError(base, withOutlier), 1e-12, L"zero-weight sample changes nothing");
			}
		}

		TEST_METHOD(FitWeightedSmallNoiseStaysAccurate)
		{
			const Rect pic = Pic(3840, 2160);
			uint64_t seed = 500;
			for (ModelType t : kTypes)
			{
				const Params truth = MakeKnown(t, pic);
				std::vector<Sample> v = RandomSamples(truth, pic, 200, seed++, 0.05);
				Params out;
				Assert::IsTrue(FitWeighted(t, v.data(), static_cast<int>(v.size()), HalfDiag(pic), out), L"noisy fit");
				const double hw = 0.5 * pic.Width(), hh = 0.5 * pic.Height();
				const double cx[4] = { -hw, hw, hw, -hw }, cy[4] = { -hh, -hh, hh, hh };
				for (int i = 0; i < 4; ++i)
				{
					double dxT, dyT, dxO, dyO;
					Displacement(truth, cx[i], cy[i], &dxT, &dyT);
					Displacement(out, cx[i], cy[i], &dxO, &dyO);
					const double e = std::sqrt((dxT - dxO) * (dxT - dxO) + (dyT - dyO) * (dyT - dyO));
					Assert::IsTrue(e < 0.1, L"corner displacement within 0.1 px under 0.05 px noise");
				}
			}
		}

		TEST_METHOD(FitWeightedRejectsDegenerateAndShort)
		{
			const Rect pic = Pic(3840, 2160);
			Params out;
			std::vector<Sample> v;
			for (int i = 0; i < 10; ++i)
				v.push_back(MakeSample(MakeKnown(ModelType::Affine, pic), -300.0 + 60.0 * i, -150.0 + 30.0 * i));
			Assert::IsTrue(!FitWeighted(ModelType::Affine, v.data(), 10, HalfDiag(pic), out), L"collinear affine fit rejected");
			Assert::IsTrue(!FitWeighted(ModelType::Homography, v.data(), 10, HalfDiag(pic), out), L"collinear homography fit rejected");
			Assert::IsTrue(!FitWeighted(ModelType::Homography, v.data(), 3, HalfDiag(pic), out), L"too few samples");
			for (Sample& s : v) s.w = 0.0;
			Assert::IsTrue(!FitWeighted(ModelType::Translation, v.data(), 10, HalfDiag(pic), out), L"zero weights rejected");
		}

		TEST_METHOD(ComposeOfInverseIsIdentity)
		{
			const Rect pics[2] = { Pic(3840, 2160), Pic(1000, 1000) };
			for (const Rect& pic : pics)
				for (ModelType t : kTypes)
				{
					const Params p = MakeKnown(t, pic);
					bool ok = false;
					const Params inv = Invert(p, &ok);
					Assert::IsTrue(ok, L"invertible");
					const Params id = Compose(inv, p);
					Assert::IsTrue(id.type == t, L"compose keeps the type");
					Assert::AreEqual(0.0, MatrixError(id, IdentityParams(t)), 1e-9, L"inverse o p is identity");
					const Params id2 = Compose(p, inv);
					Assert::AreEqual(0.0, MatrixError(id2, IdentityParams(t)), 1e-9, L"p o inverse is identity");
				}
		}

		TEST_METHOD(InvertSingularReportsFailure)
		{
			Params p = IdentityParams(ModelType::Affine);
			p.h[0] = 1; p.h[1] = 2; p.h[3] = 2; p.h[4] = 4;   // rank 1
			bool ok = true;
			const Params inv = Invert(p, &ok);
			Assert::IsTrue(!ok, L"singular reported");
			Assert::IsTrue(inv.type == ModelType::Affine, L"type kept on failure");
			Assert::AreEqual(0.0, MatrixError(inv, IdentityParams(ModelType::Affine)), 0.0, L"identity returned on failure");
			Params n = IdentityParams(ModelType::Affine);
			n.h[0] = std::nan("");
			Invert(n, &ok);
			Assert::IsTrue(!ok, L"NaN reported");
		}

		TEST_METHOD(ComposeTypeIsHigherOfTheTwo)
		{
			const Rect pic = Pic(3840, 2160);
			Params a = MakeKnown(ModelType::Translation, pic);
			Params b = MakeKnown(ModelType::Homography, pic);
			Assert::IsTrue(Compose(a, b).type == ModelType::Homography, L"homography wins");
			Params none;   // type None, identity
			Params c = Compose(none, a);
			Assert::IsTrue(c.type == ModelType::Translation, L"None acts as identity of the other type");
			Assert::AreEqual(0.0, MatrixError(c, a), 0.0, L"None composes as identity");
		}

		TEST_METHOD(SimilarityRoundTripOverPairDurations)
		{
			const Rect pic = Pic(3840, 2160);
			SimilarityRates r;
			r.logScale = 0.02; r.angle = -0.015; r.tx = 33.0; r.ty = -12.5;
			const double secs[3] = { 1.0 / 24.0, 1.0 / 60.0, 1.0 / 30.0 };
			for (double sec : secs)
			{
				const Params p = FromSimilarity(r, sec);
				Assert::IsTrue(p.type == ModelType::Similarity, L"type");
				SimilarityRates out;
				Assert::IsTrue(ToSimilarity(p, pic, sec, out), L"decompose");
				Assert::AreEqual(r.logScale, out.logScale, 1e-9, L"logScale");
				Assert::AreEqual(r.angle, out.angle, 1e-9, L"angle");
				Assert::AreEqual(r.tx, out.tx, 1e-9, L"tx");
				Assert::AreEqual(r.ty, out.ty, 1e-9, L"ty");
			}
		}

		TEST_METHOD(SameMotionOverDifferentPairSecondsGivesSameRates)
		{
			const Rect pic = Pic(3840, 2160);
			SimilarityRates r;
			r.logScale = 0.03; r.angle = 0.02; r.tx = -40.0; r.ty = 18.0;
			const double s1 = 1.0 / 24.0, s2 = 2.5 / 24.0;   // 3:2-style pair durations
			SimilarityRates o1, o2;
			Assert::IsTrue(ToSimilarity(FromSimilarity(r, s1), pic, s1, o1), L"pair 1");
			Assert::IsTrue(ToSimilarity(FromSimilarity(r, s2), pic, s2, o2), L"pair 2");
			Assert::AreEqual(o1.logScale, o2.logScale, 1e-9, L"logScale");
			Assert::AreEqual(o1.angle, o2.angle, 1e-9, L"angle");
			Assert::AreEqual(o1.tx, o2.tx, 1e-9, L"tx");
			Assert::AreEqual(o1.ty, o2.ty, 1e-9, L"ty");
		}

		TEST_METHOD(ToSimilarityRejectsBadInput)
		{
			const Rect pic = Pic(3840, 2160);
			SimilarityRates out;
			const Params p = MakeKnown(ModelType::Similarity, pic);
			Assert::IsTrue(!ToSimilarity(p, pic, 0.0, out), L"zero seconds");
			Assert::IsTrue(!ToSimilarity(p, pic, -1.0 / 24.0, out), L"negative seconds");
			Params nan = p;
			nan.h[2] = std::nan("");
			Assert::IsTrue(!ToSimilarity(nan, pic, 1.0 / 24.0, out), L"degenerate");
		}

		TEST_METHOD(ToSimilarityOfAffineAndHomographyUsesCorners)
		{
			const Rect pic = Pic(3840, 2160);
			SimilarityRates r;
			r.logScale = 0.01; r.angle = 0.02; r.tx = 12.0; r.ty = -6.0;
			const double sec = 1.0 / 30.0;
			Params sim = FromSimilarity(r, sec);
			Params aff = sim;
			aff.type = ModelType::Affine;
			SimilarityRates out;
			Assert::IsTrue(ToSimilarity(aff, pic, sec, out), L"affine decomposes");
			Assert::AreEqual(r.logScale, out.logScale, 1e-7, L"affine logScale");
			Assert::AreEqual(r.angle, out.angle, 1e-7, L"affine angle");
			Assert::AreEqual(r.tx, out.tx, 1e-6, L"affine tx");
			Assert::AreEqual(r.ty, out.ty, 1e-6, L"affine ty");
			Params hom = MakeKnown(ModelType::Homography, pic);
			Assert::IsTrue(ToSimilarity(hom, pic, sec, out), L"homography decomposes");
			Assert::IsTrue(std::fabs(out.logScale * sec - std::log(1.03)) < 0.01, L"homography scale near 1.03");
		}

		TEST_METHOD(InterpolateEndpointsForAllTypes)
		{
			const Rect pics[2] = { Pic(3840, 2160), Pic(7680, 4320) };
			for (const Rect& pic : pics)
				for (ModelType t : kTypes)
				{
					const Params p = MakeKnown(t, pic);
					const Params i0 = Interpolate(p, 0.0, pic);
					const Params i1 = Interpolate(p, 1.0, pic);
					Assert::AreEqual(0.0, ProbeError(i0, IdentityParams(t), pic), 1e-9, L"fraction 0 is identity");
					Assert::AreEqual(0.0, ProbeError(i1, p, pic), 1e-9, L"fraction 1 is p");
					Assert::IsTrue(i0.type == t && i1.type == t, L"type kept");
				}
			const Params none;
			Assert::IsTrue(Interpolate(none, 0.5, pics[0]).type == ModelType::None, L"None returned as is");
		}

		TEST_METHOD(InterpolateHalfTwiceComposesToSimilarity)
		{
			const Rect pic = Pic(3840, 2160);
			// Zoom + rotation about the picture centre (no translation): halves compose exactly.
			// (With a translation the rule scales it linearly, which does not compose.)
			Params p = MakeKnown(ModelType::Similarity, pic);
			p.h[2] = 0.0; p.h[5] = 0.0;
			const Params h = Interpolate(p, 0.5, pic);
			const Params hh = Compose(h, h);
			Assert::AreEqual(0.0, MatrixError(hh, p), 1e-6, L"half twice equals the whole");
		}

		TEST_METHOD(InterpolateAffineHomographyScalesCornerDisplacements)
		{
			const Rect pics[2] = { Pic(3840, 2160), Pic(1000, 1000) };
			const double fractions[4] = { 0.25, 0.5, 0.8, 1.5 };
			for (const Rect& pic : pics)
				for (ModelType t : { ModelType::Affine, ModelType::Homography })
				{
					const Params p = MakeKnown(t, pic);
					const double hw = 0.5 * pic.Width(), hh = 0.5 * pic.Height();
					const double cx[4] = { -hw, hw, hw, -hw }, cy[4] = { -hh, -hh, hh, hh };
					for (double f : fractions)
					{
						const Params q = Interpolate(p, f, pic);
						for (int i = 0; i < 4; ++i)
						{
							double dx, dy, ox, oy;
							Displacement(p, cx[i], cy[i], &dx, &dy);
							Apply(q, cx[i], cy[i], &ox, &oy);
							Assert::AreEqual(cx[i] + f * dx, ox, 1e-6, L"corner x");
							Assert::AreEqual(cy[i] + f * dy, oy, 1e-6, L"corner y");
						}
					}
				}
		}

		TEST_METHOD(InterpolatePureZoomIsLogLinear)
		{
			const Rect pic = Pic(3840, 2160);
			Params zoom = IdentityParams(ModelType::Similarity);
			zoom.h[0] = 1.2; zoom.h[4] = 1.2;
			const Params half = Interpolate(zoom, 0.5, pic);
			Assert::AreEqual(std::sqrt(1.2), half.h[0], 1e-12, L"scale sqrt(1.2)");
			Assert::IsTrue(std::fabs(half.h[0] - 1.1) > 1e-3, L"not the arithmetic 1.1");
			Assert::AreEqual(std::sqrt(1.2), half.h[4], 1e-12, L"scale on the second diagonal");
			Assert::AreEqual(0.0, half.h[3], 1e-12, L"no rotation");
		}

		TEST_METHOD(TimeScaleTranslationRatios)
		{
			const Rect pic = Pic(3840, 2160);
			Params t = IdentityParams(ModelType::Translation);
			t.h[2] = 7.5; t.h[5] = -3.0;
			const Params d = TimeScale(t, 2.0, pic);
			Assert::AreEqual(15.0, d.h[2], 1e-12, L"x doubled");
			Assert::AreEqual(-6.0, d.h[5], 1e-12, L"y doubled");
			const Params same = TimeScale(t, 1.0, pic);
			Assert::AreEqual(0.0, MatrixError(same, t), 0.0, L"ratio 1 returns p");
			const Params neg = TimeScale(t, -1.0, pic);
			Params inv = Invert(t);
			Assert::AreEqual(0.0, MatrixError(neg, inv), 1e-12, L"ratio -1 is the inverse motion");
		}

		TEST_METHOD(IsDegenerateClassification)
		{
			const Rect pics[3] = { Pic(3840, 2160), Pic(7680, 4320), Pic(1000, 1000) };
			for (const Rect& pic : pics)
			{
				Assert::IsTrue(!IsDegenerate(IdentityParams(ModelType::None), pic), L"identity None");
				for (ModelType t : kTypes)
					Assert::IsTrue(!IsDegenerate(MakeKnown(t, pic), pic), L"ordinary model is fine");

				Params panZoomRot = MakeKnown(ModelType::Similarity, pic);
				panZoomRot.h[2] = 0.05 * pic.Width();
				Assert::IsTrue(!IsDegenerate(panZoomRot, pic), L"pan+zoom+rotation");

				Params behind = IdentityParams(ModelType::Homography);
				behind.h[6] = -4.0 / pic.Width();   // w = -1 at the right-hand corners
				Assert::IsTrue(IsDegenerate(behind, pic), L"corner behind the camera");

				Params nan = IdentityParams(ModelType::Affine);
				nan.h[4] = std::nan("");
				Assert::IsTrue(IsDegenerate(nan, pic), L"NaN");

				Params zoom = IdentityParams(ModelType::Similarity);
				zoom.h[0] = 20.0; zoom.h[4] = 20.0;
				Assert::IsTrue(IsDegenerate(zoom, pic), L"20x zoom");

				Params shrink = IdentityParams(ModelType::Similarity);
				shrink.h[0] = 0.05; shrink.h[4] = 0.05;
				Assert::IsTrue(IsDegenerate(shrink, pic), L"20x shrink");

				Params mirror = IdentityParams(ModelType::Affine);
				mirror.h[0] = -1.0;
				Assert::IsTrue(IsDegenerate(mirror, pic), L"mirrored");

				Params far = IdentityParams(ModelType::Translation);
				far.h[2] = 9.0 * std::sqrt(static_cast<double>(pic.Width()) * pic.Width() + static_cast<double>(pic.Height()) * pic.Height());
				Assert::IsTrue(IsDegenerate(far, pic), L"corner far away");
			}
			Assert::IsTrue(IsDegenerate(IdentityParams(ModelType::Translation), Rect()), L"empty picture");
		}

		TEST_METHOD(MaxCornerDifferenceBasics)
		{
			const Rect pic = Pic(3840, 2160);
			const Params p = MakeKnown(ModelType::Homography, pic);
			Assert::AreEqual(0.0, MaxCornerDifference(p, p, pic), 0.0, L"identical models");
			Params a = IdentityParams(ModelType::Translation), b = IdentityParams(ModelType::Translation);
			a.h[2] = 5.0; b.h[2] = 15.0;
			const double diag = std::sqrt(3840.0 * 3840.0 + 2160.0 * 2160.0);
			Assert::AreEqual(10.0 / diag, MaxCornerDifference(a, b, pic), 1e-12, L"10 px apart");
			Assert::AreEqual(0.0, MaxCornerDifference(a, b, Rect()), 0.0, L"empty picture");
		}

		TEST_METHOD(SplitMix64Determinism)
		{
			SplitMix64 a(12345), b(12345), c(12346);
			bool anyDiff = false;
			for (int i = 0; i < 8; ++i)
			{
				const uint64_t va = a.Next(), vb = b.Next(), vc = c.Next();
				Assert::IsTrue(va == vb, L"same seed, same output");
				if (va != vc) anyDiff = true;
			}
			Assert::IsTrue(anyDiff, L"different seeds differ");
			SplitMix64 u(7);
			for (int i = 0; i < 1000; ++i)
			{
				const double x = u.Unit();
				Assert::IsTrue(x >= 0.0 && x < 1.0, L"Unit in [0,1)");
			}
			SplitMix64 bl(3);
			for (int i = 0; i < 100; ++i)
				Assert::IsTrue(bl.Below(10) < 10u, L"Below range");
		}
	};
}
