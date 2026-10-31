// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "pch.h"
#include <VP-V/modules/motion/estimation/global_model/GlobalModelTemporal.h>
#include <VP-V/modules/motion/estimation/global_model/GlobalModelObjects.h>
#include <VP-V/modules/motion/estimation/global_model/PixelResidualReference.h>
#include <VP-V/modules/motion/estimation/global_model/GlobalModelGeometry.h>
#include "CppUnitTest.h"

#include <cmath>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	using namespace CameraModel;

	namespace
	{
		SimilarityRates Rates(double logScale, double angle, double tx, double ty)
		{
			SimilarityRates r;
			r.logScale = logScale; r.angle = angle; r.tx = tx; r.ty = ty;
			return r;
		}

		CameraMotion SimRecord(uint64_t seq, const SimilarityRates& rates, double sec,
			Reason reason = Reason::Ok, int w = 1920, int h = 1080)
		{
			CameraMotion m;
			m.seq = seq; m.pairFrom = seq - 1; m.pairTo = seq;
			m.pairSeconds = sec;
			m.valid = true;
			m.reason = reason;
			m.rates = rates;
			m.params = FromSimilarity(rates, sec);
			m.params.type = ModelType::Similarity;
			m.picture.left = 0; m.picture.top = 0; m.picture.right = w; m.picture.bottom = h;
			return m;
		}

		CameraMotion TransRecord(uint64_t seq, double tx, double ty, double sec)
		{
			CameraMotion m = SimRecord(seq, Rates(0, 0, tx, ty), sec);
			m.params = Params();
			m.params.type = ModelType::Translation;
			m.params.h[2] = tx * sec; m.params.h[5] = ty * sec;
			return m;
		}

		// Zoom + pan with per-second displacement independent of the pair duration.
		CameraMotion AffRecord(uint64_t seq, double zoomPerSec, double txs, double tys, double sec)
		{
			CameraMotion m = SimRecord(seq, Rates(0, 0, txs, tys), sec);
			m.params = Params();
			m.params.type = ModelType::Affine;
			const double s = 1.0 + zoomPerSec * sec;
			m.params.h[0] = s; m.params.h[4] = s;
			m.params.h[2] = txs * sec; m.params.h[5] = tys * sec;
			return m;
		}

		double RelErr(double a, double b)
		{
			return std::fabs(a - b) / (std::fabs(b) + 1e-12);
		}

		double MaxHDiff(const Params& a, const Params& b)
		{
			double d = 0;
			for (int i = 0; i < 9; ++i) d = std::fmax(d, std::fabs(a.h[i] - b.h[i]));
			return d;
		}
	}

	TEST_CLASS(CameraTemporalTests)
	{
	public:
		TEST_METHOD(ConstantPanConverges)
		{
			TemporalFilter f;
			const double dt = 1.0 / 24.0;
			const SimilarityRates m = Rates(0.01, 0.02, 120.0, -30.0);
			for (int i = 0; i < 10; ++i) f.Update(dt, m);
			SimilarityRates p;
			Assert::IsTrue(f.Predict(dt, p), L"predict has state");
			Assert::IsTrue(RelErr(p.tx, m.tx) < 0.01, L"tx within 1%");
			Assert::IsTrue(RelErr(p.ty, m.ty) < 0.01, L"ty within 1%");
			Assert::IsTrue(RelErr(p.logScale, m.logScale) < 0.01, L"scale within 1%");
			Assert::IsTrue(RelErr(p.angle, m.angle) < 0.01, L"angle within 1%");
			Assert::IsTrue(f.Consistency(dt, m) > 0.9, L"consistent pan");
		}

		TEST_METHOD(SuddenSpeedChangeIsInconsistentThenFollows)
		{
			TemporalFilter f;
			const double dt = 1.0 / 24.0;
			for (int i = 0; i < 10; ++i) f.Update(dt, Rates(0, 0, 120.0, 0));
			Assert::IsTrue(f.Consistency(dt, Rates(0, 0, 1200.0, 0)) < 0.2, L"10x change flagged");
			for (int i = 0; i < 10; ++i) f.Update(dt, Rates(0, 0, 1200.0, 0));
			SimilarityRates p;
			Assert::IsTrue(f.Predict(dt, p), L"state");
			Assert::IsTrue(RelErr(p.tx, 1200.0) < 0.02, L"follows the new speed");
			Assert::IsTrue(f.Consistency(dt, Rates(0, 0, 1200.0, 0)) > 0.8, L"consistent again");
		}

		TEST_METHOD(NoStateIsNeutral)
		{
			TemporalFilter f;
			SimilarityRates p;
			Assert::IsFalse(f.Predict(0.04, p), L"no state");
			Assert::AreEqual(0.5, f.Consistency(0.04, Rates(0, 0, 10, 10)), 1e-12, L"neutral");
			Assert::IsFalse(f.Valid(), L"invalid");
		}

		TEST_METHOD(VariableDtGivesSamePrediction)
		{
			TemporalFilter a, b;
			const SimilarityRates m = Rates(0.005, -0.01, 80.0, 25.0);
			for (int i = 0; i < 12; ++i)
			{
				a.Update(1.0 / 24.0, m);
				b.Update((i & 1) ? 2.5 / 24.0 : 1.0 / 24.0, m);
			}
			SimilarityRates pa, pb;
			Assert::IsTrue(a.Predict(1.0 / 24.0, pa) && b.Predict(2.5 / 24.0, pb), L"state");
			Assert::IsTrue(RelErr(pa.tx, pb.tx) < 1e-6, L"tx same");
			Assert::IsTrue(RelErr(pa.ty, pb.ty) < 1e-6, L"ty same");
			Assert::IsTrue(RelErr(pa.angle, pb.angle) < 1e-6, L"angle same");
			Assert::IsTrue(RelErr(pb.tx, m.tx) < 1e-6, L"matches the per-second motion");
		}

		TEST_METHOD(ZeroDtBlends)
		{
			TemporalFilter f;
			f.Update(0.0, Rates(0, 0, 100, 0));
			f.Update(0.0, Rates(0, 0, 100, 0));
			SimilarityRates p;
			Assert::IsTrue(f.Predict(-1.0, p), L"state");
			Assert::AreEqual(100.0, p.tx, 1e-9, L"blend keeps the value");
		}

		TEST_METHOD(ResetClears)
		{
			TemporalFilter f;
			f.Update(0.04, Rates(0, 0, 5, 5));
			Assert::IsTrue(f.Valid(), L"valid");
			f.Reset();
			SimilarityRates p;
			Assert::IsFalse(f.Valid(), L"reset");
			Assert::IsFalse(f.Predict(0.04, p), L"no state after reset");
			Assert::AreEqual(0.5, f.Consistency(0.04, Rates(0, 0, 5, 5)), 1e-12, L"neutral after reset");
		}

		TEST_METHOD(SmootherLagZeroReturnsRecord)
		{
			FixedLagSmoother s(0);
			CameraMotion m = SimRecord(5, Rates(0.01, 0.02, 50, -10), 1.0 / 24.0);
			s.Push(m);
			Params out;
			Assert::IsTrue(s.Smoothed(5, out), L"known");
			Assert::AreEqual(0.0, MaxHDiff(out, m.params), 0.0, L"identical");
			Assert::IsFalse(s.Smoothed(6, out), L"unknown seq");
		}

		TEST_METHOD(SmootherConstantVelocityEqualsRecord)
		{
			FixedLagSmoother s(2);
			const SimilarityRates r = Rates(0.03, 0.02, 60, -25);
			for (uint64_t q = 10; q <= 20; ++q) s.Push(SimRecord(q, r, 1.0 / 24.0));
			Params out;
			Assert::IsTrue(s.Smoothed(15, out), L"smoothed");
			Assert::AreEqual(0.0, MaxHDiff(out, FromSimilarity(r, 1.0 / 24.0)), 1e-9, L"equals centre");
		}

		TEST_METHOD(SmootherReducesNoisyFrame)
		{
			FixedLagSmoother s(2);
			const double sec = 1.0 / 24.0;
			for (uint64_t q = 10; q <= 20; ++q)
				s.Push(SimRecord(q, Rates(0, 0, q == 15 ? 200.0 : 100.0, 0), sec));
			Params out;
			Assert::IsTrue(s.Smoothed(15, out), L"smoothed");
			const double rawDev = std::fabs(200.0 * sec - 100.0 * sec);
			const double dev = std::fabs(out.h[2] - 100.0 * sec);
			Assert::IsTrue(dev < 0.5 * rawDev, L"deviation reduced");
			Params nb;
			Assert::IsTrue(s.Smoothed(16, nb), L"neighbour smoothed");
			// Triangular weights bound the outlier's influence at a neighbour (weight 2 of 9).
			Assert::IsTrue(std::fabs(nb.h[2] - 100.0 * sec) <= rawDev * 2.0 / 9.0 + 1e-12, L"bounded influence");
		}

		TEST_METHOD(SmootherCutBreaksWindow)
		{
			FixedLagSmoother s(2);
			const double sec = 1.0 / 24.0;
			for (uint64_t q = 10; q <= 20; ++q)
			{
				if (q == 15) s.Push(SimRecord(q, Rates(0, 0, 900, 0), sec, Reason::Cut));
				else s.Push(SimRecord(q, Rates(0, 0, q < 15 ? 100.0 : 300.0, 0), sec));
			}
			Params out;
			Assert::IsTrue(s.Smoothed(17, out), L"post-cut");
			Assert::AreEqual(300.0 * sec, out.h[2], 1e-9, L"post-cut pan uncontaminated");
			Assert::IsTrue(s.Smoothed(16, out), L"first post-cut");
			Assert::AreEqual(300.0 * sec, out.h[2], 1e-9, L"first post-cut uncontaminated");
			Assert::IsTrue(s.Smoothed(14, out), L"pre-cut");
			Assert::AreEqual(100.0 * sec, out.h[2], 1e-9, L"pre-cut uncontaminated");
		}

		TEST_METHOD(SmootherFlashNeverContributes)
		{
			FixedLagSmoother s(2);
			const double sec = 1.0 / 24.0;
			for (uint64_t q = 10; q <= 20; ++q)
				s.Push(SimRecord(q, Rates(0, 0, q == 15 ? 900.0 : 100.0, 0), sec, q == 15 ? Reason::Flash : Reason::Ok));
			Params out;
			for (uint64_t q = 14; q <= 17; ++q)
			{
				if (q == 15) continue;
				Assert::IsTrue(s.Smoothed(q, out), L"smoothed");
				Assert::AreEqual(100.0 * sec, out.h[2], 1e-9, L"flash excluded");
			}
		}

		TEST_METHOD(SmootherFutureNotYetPushed)
		{
			FixedLagSmoother s(2);
			for (uint64_t q = 10; q <= 15; ++q) s.Push(SimRecord(q, Rates(0, 0, 100, 0), 1.0 / 24.0));
			Params out;
			Assert::IsFalse(s.Smoothed(15, out), L"no future");
			Assert::IsFalse(s.Smoothed(14, out), L"one future missing");
			Assert::IsTrue(s.Smoothed(13, out), L"complete window");
			s.Push(SimRecord(16, Rates(0, 0, 100, 0), 1.0 / 24.0));
			s.Push(SimRecord(17, Rates(0, 0, 100, 0), 1.0 / 24.0));
			Assert::IsTrue(s.Smoothed(15, out), L"now available");
		}

		TEST_METHOD(SmootherGenerationChangeBreaksWindow)
		{
			FixedLagSmoother s(2);
			const double sec = 1.0 / 24.0;
			for (uint64_t q = 10; q <= 20; ++q)
			{
				CameraMotion m = SimRecord(q, Rates(0, 0, q < 16 ? 100.0 : 300.0, 0), sec);
				if (q >= 16) m.resyncGeneration = 1;
				s.Push(m);
			}
			Params out;
			Assert::IsTrue(s.Smoothed(17, out), L"new generation");
			Assert::AreEqual(300.0 * sec, out.h[2], 1e-9, L"no old-generation contamination");
			Assert::IsTrue(s.Smoothed(14, out), L"old generation");
			Assert::AreEqual(100.0 * sec, out.h[2], 1e-9, L"no new-generation contamination");
			// Rebase generation too.
			FixedLagSmoother t(2);
			for (uint64_t q = 10; q <= 20; ++q)
			{
				CameraMotion m = SimRecord(q, Rates(0, 0, q < 16 ? 100.0 : 300.0, 0), sec);
				if (q >= 16) m.rebaseGeneration = 1;
				t.Push(m);
			}
			Assert::IsTrue(t.Smoothed(16, out), L"rebase");
			Assert::AreEqual(300.0 * sec, out.h[2], 1e-9, L"rebase breaks");
		}

		TEST_METHOD(SmootherDifferentPairSecondsSameMotion)
		{
			FixedLagSmoother s(2);
			const SimilarityRates r = Rates(0.02, -0.015, 70, 35);
			const double secs[] = { 1.0 / 24.0, 2.5 / 24.0, 1.0 / 24.0, 1.5 / 24.0, 1.0 / 24.0, 2.0 / 24.0, 1.0 / 24.0, 1.0 / 24.0 };
			for (uint64_t i = 0; i < 8; ++i) s.Push(SimRecord(20 + i, r, secs[i]));
			Params out;
			for (uint64_t i = 2; i <= 5; ++i)
			{
				Assert::IsTrue(s.Smoothed(20 + i, out), L"smoothed");
				Assert::AreEqual(0.0, MaxHDiff(out, FromSimilarity(r, secs[i])), 1e-9, L"centre's own params");
			}
		}

		TEST_METHOD(SmootherTranslationOnly)
		{
			FixedLagSmoother s(2);
			for (uint64_t q = 1; q <= 9; ++q) s.Push(TransRecord(q, 40, -8, q % 2 ? 1.0 / 24.0 : 2.0 / 24.0));
			Params out;
			Assert::IsTrue(s.Smoothed(5, out), L"smoothed");
			Assert::IsTrue(out.type == ModelType::Translation, L"stays translation");
			Assert::AreEqual(40.0 * 1.0 / 24.0, out.h[2], 1e-9, L"tx");
			Assert::AreEqual(-8.0 / 24.0, out.h[5], 1e-9, L"ty");
		}

		TEST_METHOD(SmootherSeqGapsUseSeqMembership)
		{
			FixedLagSmoother s(2);
			const SimilarityRates r = Rates(0, 0, 50, 0), odd = Rates(0, 0, 500, 0);
			// seq 10, 11, 12 (centre), 14 present; 13 missing; 15 present (odd) beyond lag? 12+2=14 only.
			s.Push(SimRecord(10, r, 0.04));
			s.Push(SimRecord(11, r, 0.04));
			s.Push(SimRecord(12, r, 0.04));
			s.Push(SimRecord(14, r, 0.04));
			s.Push(SimRecord(15, odd, 0.04));
			Params out;
			Assert::IsTrue(s.Smoothed(12, out), L"gap tolerated");
			Assert::AreEqual(50.0 * 0.04, out.h[2], 1e-9, L"seq 15 is outside the window");
		}

		TEST_METHOD(SmootherAffineCornerMethod)
		{
			FixedLagSmoother s(2);
			const double secs[] = { 1.0 / 24.0, 2.0 / 24.0, 1.0 / 24.0, 1.5 / 24.0, 1.0 / 24.0, 1.0 / 24.0, 2.5 / 24.0, 1.0 / 24.0 };
			for (uint64_t i = 0; i < 8; ++i) s.Push(AffRecord(30 + i, 0.3, 50.0, -20.0, secs[i]));
			Params out;
			for (uint64_t i = 2; i <= 5; ++i)
			{
				Assert::IsTrue(s.Smoothed(30 + i, out), L"smoothed");
				Assert::IsTrue(out.type == ModelType::Affine, L"affine");
				Assert::AreEqual(0.0, MaxHDiff(out, AffRecord(30 + i, 0.3, 50.0, -20.0, secs[i]).params), 1e-9, L"centre params");
			}
		}

		TEST_METHOD(SmootherMixedTypesUseHighest)
		{
			FixedLagSmoother s(1);
			const double sec = 1.0 / 24.0;
			s.Push(SimRecord(1, Rates(0, 0, 50, -20), sec));
			s.Push(AffRecord(2, 0.0, 50.0, -20.0, sec));
			s.Push(SimRecord(3, Rates(0, 0, 50, -20), sec));
			Params out;
			Assert::IsTrue(s.Smoothed(2, out), L"smoothed");
			Assert::IsTrue(out.type == ModelType::Affine, L"highest type");
			Assert::AreEqual(50.0 * sec, out.h[2], 1e-9, L"tx");
			Assert::AreEqual(1.0, out.h[0], 1e-9, L"unit scale");
		}
	};

	namespace
	{
		struct Grid
		{
			int gw, gh;
			double bs;
			Rect pic;
			std::vector<float> fx, fy, trust;
			std::vector<uint8_t> label;

			Grid(int w, int h, double blockSize)
				: gw(w), gh(h), bs(blockSize), fx(static_cast<size_t>(w) * h, 0.f), fy(static_cast<size_t>(w) * h, 0.f),
				trust(static_cast<size_t>(w) * h, 1.f), label(static_cast<size_t>(w) * h, 0)
			{
				pic.left = 0; pic.top = 0;
				pic.right = static_cast<int>(w * blockSize);
				pic.bottom = static_cast<int>(h * blockSize);
			}

			void Patch(int x0, int y0, int w, int h, float dx, float dy)
			{
				for (int y = y0; y < y0 + h; ++y)
					for (int x = x0; x < x0 + w; ++x)
					{
						const size_t i = static_cast<size_t>(y) * gw + x;
						label[i] = 2; fx[i] = dx; fy[i] = dy;
					}
			}

			ObjectInput Input(uint64_t seed = 1234) const
			{
				ObjectInput in;
				in.gridW = gw; in.gridH = gh; in.blockSize = bs; in.picture = pic;
				in.fwdX = fx.data(); in.fwdY = fy.data(); in.trust = trust.data(); in.label = label.data();
				in.precisionPx = 1.0; in.seed = seed;
				return in;
			}
		};

		int CountNonZero(const std::vector<uint8_t>& v)
		{
			int n = 0;
			for (size_t i = 0; i < v.size(); ++i) if (v[i]) ++n;
			return n;
		}
	}

	TEST_CLASS(CameraObjectsTests)
	{
	public:
		TEST_METHOD(SinglePatchTranslation)
		{
			Grid g(48, 27, 16.0);
			g.Patch(10, 5, 5, 4, 12.f, -3.f);
			ObjectTracker t;
			std::vector<uint8_t> ids;
			const std::vector<ObjectModel> objs = t.Update(g.Input(), ObjectConfig(), ids);
			Assert::AreEqual(size_t(1), objs.size(), L"one object");
			Assert::IsTrue(objs[0].type == ModelType::Translation, L"translation");
			Assert::AreEqual(12.0, objs[0].h[2], 0.1, L"tx");
			Assert::AreEqual(-3.0, objs[0].h[5], 0.1, L"ty");
			Assert::AreEqual(20u, objs[0].blocks, L"blocks");
			Assert::AreEqual(size_t(48 * 27), ids.size(), L"id grid size");
			Assert::AreEqual(20, CountNonZero(ids), L"exactly the patch");
			for (int y = 5; y < 9; ++y)
				for (int x = 10; x < 15; ++x)
					Assert::AreEqual(int(objs[0].id), int(ids[y * 48 + x]), L"patch block id");
			// centroid: block centres (x+0.5)*16 - 384 over x=10..14 -> 12.5*16-384 = -184
			Assert::AreEqual(12.5 * 16.0 - 384.0, objs[0].cx, 1e-6, L"cx");
			Assert::AreEqual(7.0 * 16.0 - 216.0, objs[0].cy, 1e-6, L"cy");
		}

		TEST_METHOD(TwoPatchesTwoIdsPersistent)
		{
			Grid g(48, 27, 16.0);
			g.Patch(8, 4, 5, 4, 12.f, -3.f);
			g.Patch(30, 15, 5, 4, -20.f, 6.f);
			ObjectTracker t;
			std::vector<uint8_t> ids;
			std::vector<ObjectModel> objs = t.Update(g.Input(), ObjectConfig(), ids);
			Assert::AreEqual(size_t(2), objs.size(), L"two objects");
			bool have1 = false, have2 = false;
			uint8_t idA = 0, idB = 0;
			for (size_t i = 0; i < objs.size(); ++i)
			{
				if (objs[i].id == 1) have1 = true;
				if (objs[i].id == 2) have2 = true;
				if (std::fabs(objs[i].h[2] - 12.0) < 0.5) idA = objs[i].id; else idB = objs[i].id;
			}
			Assert::IsTrue(have1 && have2, L"ids 1 and 2");

			// Next call: patches moved by (about) their motion, one block over.
			Grid g2(48, 27, 16.0);
			g2.Patch(9, 4, 5, 4, 12.f, -3.f);
			g2.Patch(29, 15, 5, 4, -20.f, 6.f);
			objs = t.Update(g2.Input(), ObjectConfig(), ids);
			Assert::AreEqual(size_t(2), objs.size(), L"two objects again");
			for (size_t i = 0; i < objs.size(); ++i)
			{
				const uint8_t expect = (std::fabs(objs[i].h[2] - 12.0) < 0.5) ? idA : idB;
				Assert::AreEqual(int(expect), int(objs[i].id), L"id persists");
			}
			Assert::AreEqual(40, CountNonZero(ids), L"both patches labelled");
		}

		TEST_METHOD(IsolatedBlockBelowMinSupport)
		{
			Grid g(48, 27, 16.0);
			g.Patch(20, 10, 1, 1, 9.f, 9.f);
			ObjectTracker t;
			std::vector<uint8_t> ids;
			const std::vector<ObjectModel> objs = t.Update(g.Input(), ObjectConfig(), ids);
			Assert::AreEqual(size_t(0), objs.size(), L"none");
			Assert::AreEqual(0, CountNonZero(ids), L"no ids");
		}

		TEST_METHOD(ScatteredSetRejected)
		{
			Grid g(48, 27, 16.0);
			for (int j = 0; j < 4; ++j)
				for (int i = 0; i < 5; ++i)
					g.Patch(2 + i * 9, 2 + j * 6, 1, 1, 12.f, -3.f);
			ObjectTracker t;
			std::vector<uint8_t> ids;
			const std::vector<ObjectModel> objs = t.Update(g.Input(), ObjectConfig(), ids);
			Assert::AreEqual(size_t(0), objs.size(), L"non-compact rejected");
			Assert::AreEqual(0, CountNonZero(ids), L"no ids");
		}

		TEST_METHOD(MaxObjectsLimits)
		{
			Grid g(48, 27, 16.0);
			g.Patch(8, 4, 5, 4, 12.f, -3.f);
			g.Patch(30, 15, 5, 4, -20.f, 6.f);
			ObjectConfig cfg;
			cfg.maxObjects = 1;
			ObjectTracker t;
			std::vector<uint8_t> ids;
			const std::vector<ObjectModel> objs = t.Update(g.Input(), cfg, ids);
			Assert::AreEqual(size_t(1), objs.size(), L"limited");
			Assert::AreEqual(20, CountNonZero(ids), L"one patch only");
		}

		TEST_METHOD(UntrustedAndOutsideBlocksIgnored)
		{
			Grid g(48, 27, 16.0);
			g.Patch(10, 5, 5, 4, 12.f, -3.f);
			for (int y = 5; y < 9; ++y)
				for (int x = 10; x < 15; ++x)
					g.trust[y * 48 + x] = 0.f;
			ObjectTracker t;
			std::vector<uint8_t> ids;
			Assert::AreEqual(size_t(0), t.Update(g.Input(), ObjectConfig(), ids).size(), L"zero trust ignored");
		}

		TEST_METHOD(Deterministic)
		{
			Grid g(48, 27, 16.0);
			g.Patch(8, 4, 5, 4, 12.f, -3.f);
			g.Patch(30, 15, 6, 5, -20.f, 6.f);
			ObjectTracker a, b;
			std::vector<uint8_t> ia, ib;
			const std::vector<ObjectModel> oa = a.Update(g.Input(77), ObjectConfig(), ia);
			const std::vector<ObjectModel> ob = b.Update(g.Input(77), ObjectConfig(), ib);
			Assert::IsTrue(ia == ib, L"same ids");
			Assert::AreEqual(oa.size(), ob.size(), L"same count");
			for (size_t i = 0; i < oa.size(); ++i)
			{
				Assert::AreEqual(int(oa[i].id), int(ob[i].id), L"same id");
				Assert::AreEqual(oa[i].h[2], ob[i].h[2], 0.0, L"same tx");
				Assert::AreEqual(oa[i].h[5], ob[i].h[5], 0.0, L"same ty");
			}
		}

		TEST_METHOD(LargerGridWorks)
		{
			Grid g(480, 270, 16.0);
			Assert::AreEqual(7680, g.pic.Width(), L"picture width");
			g.Patch(100, 60, 40, 35, 12.f, -3.f);   // 1400 blocks > min support (1296)
			ObjectTracker t;
			std::vector<uint8_t> ids;
			const std::vector<ObjectModel> objs = t.Update(g.Input(), ObjectConfig(), ids);
			Assert::AreEqual(size_t(1), objs.size(), L"one object");
			Assert::AreEqual(12.0, objs[0].h[2], 0.1, L"tx");
			Assert::AreEqual(1400u, objs[0].blocks, L"blocks");
			Assert::AreEqual(1400, CountNonZero(ids), L"ids");
			// An 800-block patch is below the share-derived minimum for this grid.
			Grid g2(480, 270, 16.0);
			g2.Patch(100, 60, 40, 20, 12.f, -3.f);
			ObjectTracker t2;
			Assert::AreEqual(size_t(0), t2.Update(g2.Input(), ObjectConfig(), ids).size(), L"below share");
		}

		TEST_METHOD(AffineObjectDetected)
		{
			Grid g(48, 27, 16.0);
			// Expanding object: displacement grows with distance from the patch centre.
			for (int y = 5; y < 15; ++y)
				for (int x = 10; x < 20; ++x)
				{
					const size_t i = static_cast<size_t>(y) * 48 + x;
					g.label[i] = 2;
					g.fx[i] = static_cast<float>((x - 14.5) * 3.0);
					g.fy[i] = static_cast<float>((y - 9.5) * 3.0);
				}
			ObjectConfig cfg;
			cfg.relThreshold = 1.0;   // let the translation hypothesis accept the whole patch
			ObjectTracker t;
			std::vector<uint8_t> ids;
			const std::vector<ObjectModel> objs = t.Update(g.Input(), cfg, ids);
			Assert::AreEqual(size_t(1), objs.size(), L"one object");
			Assert::IsTrue(objs[0].type == ModelType::Affine, L"affine chosen");
		}

		TEST_METHOD(ResetRestartsIds)
		{
			Grid g(48, 27, 16.0);
			g.Patch(8, 4, 5, 4, 12.f, -3.f);
			ObjectTracker t;
			std::vector<uint8_t> ids;
			t.Update(g.Input(), ObjectConfig(), ids);
			t.Reset();
			const std::vector<ObjectModel> objs = t.Update(g.Input(), ObjectConfig(), ids);
			Assert::AreEqual(size_t(1), objs.size(), L"object");
			Assert::AreEqual(1, int(objs[0].id), L"ids restart at 1");
		}
	};

	namespace
	{
		float Tex(int x, int y)
		{
			uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u + 1442695041u;
			h = (h ^ (h >> 13)) * 1274126177u;
			h ^= h >> 16;
			h *= 2246822519u;
			h ^= h >> 13;
			return static_cast<float>(h & 0xFFFFu) * (255.0f / 65536.0f);
		}

		struct ImagePair
		{
			int w, h;
			std::vector<float> prev, cur;
			explicit ImagePair(int width = 640, int height = 360, int sx = 7, int sy = -3)
				: w(width), h(height), prev(static_cast<size_t>(width) * height), cur(static_cast<size_t>(width) * height)
			{
				for (int y = 0; y < h; ++y)
					for (int x = 0; x < w; ++x)
					{
						prev[static_cast<size_t>(y) * w + x] = Tex(x, y);
						cur[static_cast<size_t>(y) * w + x] = Tex(x - sx, y - sy);
					}
			}
			Rect Picture() const
			{
				Rect r; r.left = 0; r.top = 0; r.right = w; r.bottom = h;
				return r;
			}
			void Fill(std::vector<float>& img, int x0, int y0, int n, float v)
			{
				for (int y = y0; y < y0 + n; ++y)
					for (int x = x0; x < x0 + n; ++x)
						img[static_cast<size_t>(y) * w + x] = v;
			}
		};

		Params Translate(double tx, double ty)
		{
			Params p;
			p.type = ModelType::Translation;
			p.h[2] = tx; p.h[5] = ty;
			return p;
		}
	}

	TEST_CLASS(CameraResidualTests)
	{
	public:
		TEST_METHOD(PureTranslationHasNoResidualInside)
		{
			ImagePair im;
			std::vector<float> share;
			const int gw = 40, gh = 23;
			Assert::IsTrue(PixelResidualReference(im.prev.data(), im.cur.data(), im.w, im.h, Translate(7, -3),
				im.Picture(), gw, gh, 16.0, 3.0, 1.0, share), L"ok");
			Assert::AreEqual(size_t(gw * gh), share.size(), L"size");
			for (int by = 0; by < gh - 1; ++by)
				for (int bx = 1; bx < gw; ++bx)
					Assert::IsTrue(share[by * gw + bx] < 0.02f, L"interior block clean");
		}

		TEST_METHOD(TinyFastMoverIsFlagged)
		{
			ImagePair im;
			im.Fill(im.prev, 304, 176, 6, 1000.f);
			im.Fill(im.cur, 306, 178, 6, 1000.f);
			std::vector<float> share;
			const int gw = 40, gh = 23;
			Assert::IsTrue(PixelResidualReference(im.prev.data(), im.cur.data(), im.w, im.h, Translate(7, -3),
				im.Picture(), gw, gh, 16.0, 3.0, 1.0, share), L"ok");
			Assert::IsTrue(share[11 * gw + 19] >= 0.1f, L"patch block flagged");
			Assert::IsTrue(share[5 * gw + 5] < 0.02f, L"far block clean");
			Assert::IsTrue(share[17 * gw + 30] < 0.02f, L"another far block clean");
		}

		TEST_METHOD(WrongCameraGivesLargeShares)
		{
			ImagePair im;
			std::vector<float> share;
			const int gw = 40, gh = 23;
			Assert::IsTrue(PixelResidualReference(im.prev.data(), im.cur.data(), im.w, im.h, Translate(0, 0),
				im.Picture(), gw, gh, 16.0, 3.0, 1.0, share), L"ok");
			int big = 0;
			for (size_t i = 0; i < share.size(); ++i) if (share[i] > 0.5f) ++big;
			Assert::IsTrue(big > static_cast<int>(share.size()) * 9 / 10, L"most blocks differ");
		}

		TEST_METHOD(HugeThresholdGivesZeros)
		{
			ImagePair im;
			std::vector<float> share;
			Assert::IsTrue(PixelResidualReference(im.prev.data(), im.cur.data(), im.w, im.h, Translate(0, 0),
				im.Picture(), 40, 23, 16.0, 1e9, 1.0, share), L"ok");
			for (size_t i = 0; i < share.size(); ++i) Assert::AreEqual(0.0f, share[i], 0.0f, L"zero");
		}

		TEST_METHOD(SingularCameraFails)
		{
			ImagePair im;
			Params bad;
			for (int i = 0; i < 9; ++i) bad.h[i] = 0.0;
			std::vector<float> share(5, 1.0f);
			Assert::IsFalse(PixelResidualReference(im.prev.data(), im.cur.data(), im.w, im.h, bad,
				im.Picture(), 40, 23, 16.0, 3.0, 1.0, share), L"fails");
			Assert::AreEqual(size_t(40 * 23), share.size(), L"sized");
			for (size_t i = 0; i < share.size(); ++i) Assert::AreEqual(0.0f, share[i], 0.0f, L"zero");
		}

		TEST_METHOD(NonIntegerBlockSizeAndPartialBlocks)
		{
			ImagePair im;
			std::vector<float> share;
			const double bs = 20.5;
			const int gw = static_cast<int>(std::ceil(640 / bs)), gh = static_cast<int>(std::ceil(360 / bs));
			Assert::IsTrue(PixelResidualReference(im.prev.data(), im.cur.data(), im.w, im.h, Translate(7, -3),
				im.Picture(), gw, gh, bs, 3.0, 1.0, share), L"ok");
			Assert::IsTrue(share[5 * gw + 5] < 0.02f, L"interior clean");
			for (size_t i = 0; i < share.size(); ++i) Assert::IsTrue(share[i] >= 0.0f && share[i] <= 1.0f, L"in range");
		}
	};
}
