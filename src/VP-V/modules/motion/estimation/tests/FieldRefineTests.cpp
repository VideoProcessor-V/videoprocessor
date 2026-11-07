// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// U15 FieldRefine v1 CPU reference: hand-computed cases, parabolic sub-pel exactness, flags, the placeholder producer.
#include <VP-V/modules/motion/estimation/refine/FieldRefineRef.h>
#include <VP-V/modules/motion/estimation/refine/FieldRefineProducer.h>

#include <cmath>
#include <limits>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	namespace
	{
		using namespace VpUnit::FieldRefine;

		double Texture(double x, double y)
		{
			return 120.0 + 50.0 * std::sin(0.7 * x + 0.3) + 40.0 * std::cos(0.5 * y + 0.2 * x) + 25.0 * std::sin(0.31 * x * y * 0.1 + y);
		}

		struct Scene
		{
			int w = 24, h = 24, gw = 3, gh = 3;
			std::vector<float> a, b, p3, cam;
			std::vector<uint8_t> camMask, occ;
			// B(q) = A(q - m): content moves by m half px from A to B.
			void Build(double mx, double my, bool linear)
			{
				a.resize(static_cast<size_t>(w) * h);
				b.resize(a.size());
				for (int y = 0; y < h; ++y)
					for (int x = 0; x < w; ++x)
					{
						auto f = [&](double fx, double fy) { return linear ? 10.0 * fx + 3.5 * fy + 20.0 : Texture(fx, fy); };
						a[static_cast<size_t>(y) * w + x] = static_cast<float>(f(x, y));
						b[static_cast<size_t>(y) * w + x] = static_cast<float>(f(x - mx, y - my));
					}
				p3.assign(static_cast<size_t>(gw) * gh * 2, 0.0f);
				cam.assign(p3.size(), 0.0f);
				camMask.assign(static_cast<size_t>(gw) * gh, 0);
				occ.assign(camMask.size(), 0);
			}
			Input In() const
			{
				Input in;
				in.lumaA = a.data(); in.lumaB = b.data(); in.w = w; in.h = h; in.gridW = gw; in.gridH = gh;
				in.p3 = p3.data(); in.camera = cam.data(); in.cameraMask = camMask.data(); in.occluded = occ.data();
				return in;
			}
			void SetP3(float vx, float vy) { for (size_t i = 0; i < p3.size(); i += 2) { p3[i] = vx; p3[i + 1] = vy; } }
		};
	}

	TEST_CLASS(FieldRefineTests)
	{
	public:
		TEST_METHOD(ParabolicOffsetIsExactOnAnExactParabola)
		{
			for (double x0 : { 0.3, -0.25, 0.0, 0.49, -0.4 })
			{
				auto f = [&](double x) { return static_cast<float>(3.0 * (x - x0) * (x - x0) + 7.0); };
				Assert::AreEqual(x0, static_cast<double>(ParabolicOffset(f(-1), f(0), f(1))), 2e-5);
			}
			Assert::AreEqual(0.0, static_cast<double>(ParabolicOffset(5.0f, 5.0f, 5.0f)), 0.0, L"flat: no sub-pel");
			Assert::AreEqual(0.0, static_cast<double>(ParabolicOffset(1.0f, 5.0f, 1.0f)), 0.0, L"concave: no sub-pel");
			Assert::AreEqual(0.5, static_cast<double>(ParabolicOffset(0.0f, 0.001f, 4.0f)), 0.0, L"clamped to half a pixel");
		}

		TEST_METHOD(HalfFloatRoundTripAndKnownCodes)
		{
			Assert::AreEqual(static_cast<uint16_t>(0x3C00), FloatToHalf(1.0f));
			Assert::AreEqual(static_cast<uint16_t>(0xC000), FloatToHalf(-2.0f));
			Assert::AreEqual(static_cast<uint16_t>(0x3800), FloatToHalf(0.5f));
			Assert::AreEqual(static_cast<uint16_t>(0x7BFF), FloatToHalf(65504.0f));
			Assert::AreEqual(static_cast<uint16_t>(0x0000), FloatToHalf(0.0f));
			Assert::AreEqual(1.0f, HalfToFloat(0x3C00));
			Assert::AreEqual(-2.0f, HalfToFloat(0xC000));
			for (float v : { 0.1f, -3.7f, 12.34f, 0.015625f, 100.5f })
				Assert::AreEqual(static_cast<double>(v), static_cast<double>(HalfToFloat(FloatToHalf(v))), std::fabs(v) * 1.0 / 1024.0);
			Block b;
			b.vx = -1.5f; b.vy = 6.25f; b.confidence = 200; b.flags = kRefined | kBoundary;
			const Packed p = Pack(b);
			Assert::AreEqual(static_cast<size_t>(8), sizeof(p));
			const Block u = Unpack(p);
			Assert::AreEqual(-1.5f, u.vx); Assert::AreEqual(6.25f, u.vy);
			Assert::AreEqual(static_cast<int>(200), static_cast<int>(u.confidence));
			Assert::AreEqual(static_cast<int>(kRefined | kBoundary), static_cast<int>(u.flags));
		}

		TEST_METHOD(IntegerShiftIsFoundWithZeroCostAndRefinedFlag)
		{
			Scene s;
			s.Build(1.0, -1.0, false);   // half-px motion (1, -1) = source (2, -2)
			s.SetP3(0.0f, 0.0f);
			const Input in = s.In();
			Surface surf;
			SearchSurface(in, 1, 1, 4, 2, 0.0f, 0.0f, surf);
			Assert::AreEqual(1, surf.bestX); Assert::AreEqual(-1, surf.bestY);
			Assert::AreEqual(0.0, static_cast<double>(surf.best), 0.0, L"integer shifts sample B exactly");
			Params p;
			const Block r = RefineBlock(in, p, 1, 1);
			Assert::IsTrue(std::fabs(r.vx - 2.0f) < 0.6f && std::fabs(r.vy + 2.0f) < 0.6f);
			Assert::IsTrue((r.flags & kRefined) != 0 && (r.flags & kKept) == 0);
			Assert::IsTrue(r.confidence > 230);
		}

		TEST_METHOD(CorrectSeedOnALinearRampIsKeptExactly)
		{
			Scene s;
			s.Build(2.0, -1.0, true);   // source (4, -2)
			s.SetP3(4.0f, -2.0f);
			Params p;
			const Block r = RefineBlock(s.In(), p, 1, 1);
			Assert::AreEqual(4.0f, r.vx); Assert::AreEqual(-2.0f, r.vy);
			Assert::AreEqual(static_cast<int>(kKept), static_cast<int>(r.flags));
			Assert::AreEqual(255, static_cast<int>(r.confidence));
		}

		TEST_METHOD(SaturatedSearchFlagsTheBlockAsBoundary)
		{
			Scene s;
			s.Build(2.0, 0.0, false);   // best at +2 on the range edge
			s.SetP3(0.0f, 0.0f);
			Params p;
			const Block r = RefineBlock(s.In(), p, 1, 1);
			Assert::IsTrue((r.flags & kBoundary) != 0, L"range edge = boundary");
		}

		TEST_METHOD(NeighbourDiscontinuityFlagsBoundaryOnlyAboveTheThreshold)
		{
			Scene s;
			s.Build(0.0, 0.0, false);
			s.SetP3(0.0f, 0.0f);
			Params p;
			Assert::IsTrue((RefineBlock(s.In(), p, 1, 1).flags & kBoundary) == 0);
			s.p3[(1 * 3 + 2) * 2] = 0.9f;   // below 1.0 source px
			Assert::IsTrue((RefineBlock(s.In(), p, 1, 1).flags & kBoundary) == 0);
			s.p3[(1 * 3 + 2) * 2] = 5.0f;
			Assert::IsTrue((RefineBlock(s.In(), p, 1, 1).flags & kBoundary) != 0);
		}

		TEST_METHOD(CameraSeedWinsOnlyOnCameraBlocksAndOnlyWhenCheaper)
		{
			Scene s;
			s.Build(3.0, 0.0, false);   // half px (3, 0): outside the +-2 range of the zero P3 seed
			s.SetP3(0.0f, 0.0f);
			for (size_t i = 0; i < s.cam.size(); i += 2) { s.cam[i] = 6.0f; s.cam[i + 1] = 0.0f; }
			Params p;
			Block off = RefineBlock(s.In(), p, 1, 1);
			Assert::IsTrue(std::fabs(off.vx - 6.0f) > 1.0f, L"not a camera block: P3 seed only");
			s.camMask[1 * 3 + 1] = 1;
			const Block on = RefineBlock(s.In(), p, 1, 1);
			Assert::IsTrue(std::fabs(on.vx - 6.0f) <= 1.01f && std::fabs(on.vy) <= 1.01f, L"camera seed reaches the true motion");
			Assert::IsTrue((on.flags & kRefined) != 0);
		}

		TEST_METHOD(InvalidAndOccludedBlocksAreNotSearched)
		{
			Scene s;
			s.Build(1.0, 0.0, false);
			s.SetP3(2.0f, 0.0f);
			s.p3[(1 * 3 + 1) * 2] = std::numeric_limits<float>::quiet_NaN();
			s.occ[0] = 1;
			Params p;
			const Block inv = RefineBlock(s.In(), p, 1, 1);
			Assert::AreEqual(0.0f, inv.vx); Assert::AreEqual(0, static_cast<int>(inv.confidence));
			Assert::AreEqual(static_cast<int>(kKept), static_cast<int>(inv.flags));
			const Block occ = RefineBlock(s.In(), p, 0, 0);
			Assert::AreEqual(2.0f, occ.vx);
			Assert::AreEqual(static_cast<int>(kOccluded | kKept), static_cast<int>(occ.flags));
		}

		TEST_METHOD(FlatPictureHasZeroConfidence)
		{
			Scene s;
			s.Build(0.0, 0.0, false);
			std::fill(s.a.begin(), s.a.end(), 77.0f);
			std::fill(s.b.begin(), s.b.end(), 77.0f);
			s.SetP3(0.0f, 0.0f);
			Params p;
			Assert::AreEqual(0, static_cast<int>(RefineBlock(s.In(), p, 1, 1).confidence));
		}

		TEST_METHOD(RefineFieldIsDeterministicAndFillsTheGrid)
		{
			Scene s;
			s.Build(1.0, 1.0, false);
			s.SetP3(1.0f, 1.0f);
			Params p;
			std::vector<Block> a, b;
			RefineField(s.In(), p, a);
			RefineField(s.In(), p, b);
			Assert::AreEqual(static_cast<size_t>(9), a.size());
			for (size_t i = 0; i < a.size(); ++i)
				Assert::IsTrue(a[i].vx == b[i].vx && a[i].vy == b[i].vy && a[i].flags == b[i].flags && a[i].confidence == b[i].confidence);
		}

		TEST_METHOD(OffsetAccessorHandComputedAndStopsAtBoundaries)
		{
			std::vector<Block> f(3);
			f[0].vx = 0.0f; f[1].vx = 8.0f; f[2].vx = 16.0f;
			for (auto& b : f) b.flags = kKept;
			float ox = 9, oy = 9;
			Offset(f.data(), 3, 1, 8, 12.0f, 4.0f, ox, oy);
			Assert::AreEqual(0.0f, ox); Assert::AreEqual(0.0f, oy);
			Offset(f.data(), 3, 1, 8, 8.0f, 4.0f, ox, oy);
			Assert::AreEqual(-4.0f, ox);
			Offset(f.data(), 3, 1, 8, 16.0f, 4.0f, ox, oy);
			Assert::AreEqual(-4.0f, ox);
			f[0].flags = kKept | kBoundary;   // neighbour excluded: its vector is replaced by the own one
			Offset(f.data(), 3, 1, 8, 8.0f, 4.0f, ox, oy);
			Assert::AreEqual(0.0f, ox);
			f[2].flags = kKept | kBoundary;   // own block boundary: no offset
			Offset(f.data(), 3, 1, 8, 20.0f, 4.0f, ox, oy);
			Assert::AreEqual(0.0f, ox); Assert::AreEqual(0.0f, oy);
		}

		TEST_METHOD(PlaceholderProducerPublishesAbsentAndTheFallbackIsVisible)
		{
			RefinedFieldProducer prod;
			Assert::IsTrue(prod.Status() == Availability::Absent);
			Assert::IsFalse(prod.Bound());
			const auto asked = prod.Decision(true);
			Assert::IsTrue(asked.engaged);
			Assert::IsTrue(asked.reason.find("unrefined P3") != std::string::npos);
			Assert::IsFalse(prod.Decision(false).engaged);
			Assert::IsTrue(prod.StatusLine().find("motion:") == 0);
		}
	};
}
