// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// F2: the camera fit reads the optical-flow SEED (IVectorEngine::SeedFieldOf), never the guided live field (FieldOf): the seed's
// conversion onto the 16-px source grid (CPU, deterministic), the provenance rule (by type: SeedField is not a VerifiedField; at
// run time: a Live copy is refused by the fit) and the camera_fit fallback text. CPU only.
#include "CameraLinkTestSupport.h"
#include <VP-V/modules/motion/estimation/VectorEngineInterface.h>
#include <VP-V/modules/motion/estimation/global_model/link/CameraFitStatus.h>
#include <VP-V/modules/motion/estimation/global_model/link/CameraLinkCore.h>
#include <VP-V/modules/motion/estimation/global_model/link/FieldCopy.h>
#include <VP-V/modules/motion/estimation/global_model/link/FieldReadbackRing.h>

#include <cmath>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace CameraSeedFieldTestsNs
{
	// Compile-time provenance: a SeedField can never be made from, converted from or assigned from a VerifiedField (or the
	// other way round), so the seed path cannot be handed the guided field.
	static_assert(!std::is_constructible<VpMotion::SeedField, const VpMotion::VerifiedField&>::value, "seed from a verified field");
	static_assert(!std::is_convertible<const VpMotion::VerifiedField&, VpMotion::SeedField>::value, "verified field converts to a seed");
	static_assert(!std::is_assignable<VpMotion::SeedField&, const VpMotion::VerifiedField&>::value, "verified field assigns to a seed");
	static_assert(!std::is_base_of<VpMotion::VerifiedField, VpMotion::SeedField>::value, "a seed is not a verified field");
	static_assert(!std::is_base_of<VpMotion::SeedField, VpMotion::VerifiedField>::value, "a verified field is not a seed");

	// Expression SFINAE: what the camera core's field request may call. Both ring overloads exist (the generic one is tagged Live).
	template<class T, class = void> struct RingTakes : std::false_type {};
	template<class T> struct RingTakes<T, decltype(void(std::declval<CameraLink::FieldReadbackRing&>().Request(std::declval<const T&>())))> : std::true_type {};
	static_assert(RingTakes<VpMotion::SeedField>::value, "the ring takes the seed");
	// The engine's seed accessor takes only a SeedField (a VerifiedField out-param does not compile).
	template<class T, class = void> struct SeedAccepts : std::false_type {};
	template<class T> struct SeedAccepts<T, decltype(void(std::declval<const VpMotion::IVectorEngine&>().SeedFieldOf(uint64_t(1), std::declval<T&>())))> : std::true_type {};
	static_assert(SeedAccepts<VpMotion::SeedField>::value, "SeedFieldOf fills a SeedField");
	static_assert(!SeedAccepts<VpMotion::VerifiedField>::value, "SeedFieldOf never fills a VerifiedField");

	// One rgba texel (vx, vy, sad, weight).
	void Put(std::vector<float>& plane, int gridW, int x, int y, float vx, float vy, float sad, float w)
	{
		float* t = &plane[(static_cast<size_t>(y) * gridW + x) * 4];
		t[0] = vx; t[1] = vy; t[2] = sad; t[3] = w;
	}

	// A uniform pan in seed px: forward v, backward -v, a good SAD, weight 1.
	void Uniform(std::vector<float>& f, std::vector<float>& b, int gridW, int gridH, float vx, float vy)
	{
		f.assign(static_cast<size_t>(gridW) * gridH * 4, 0.0f);
		b.assign(f.size(), 0.0f);
		for (int y = 0; y < gridH; ++y)
			for (int x = 0; x < gridW; ++x)
			{
				Put(f, gridW, x, y, vx, vy, 1.0f, 1.0f);
				Put(b, gridW, x, y, -vx, -vy, 1.0f, 1.0f);
			}
	}

	CameraLink::SeedLayout Layout(int gridW, int gridH, int per, int srcW, int srcH)
	{
		CameraLink::SeedLayout l;
		l.gridW = gridW;
		l.gridH = gridH;
		l.per = per;
		l.scaleToSourcePx = 2.0f * per;
		l.sourceGridW = srcW;
		l.sourceGridH = srcH;
		l.sadTrustMax = 3.0f;
		l.consistencyPx = 1.5f;
		return l;
	}
}

namespace VideoProcessorTest
{
	using namespace CameraSeedFieldTestsNs;

	TEST_CLASS(CameraSeedFieldTests)
	{
	public:
		// 1080p: the seed grid IS the 16-px source grid; vectors x 2 (half-size px -> source px); trust 1 on a consistent pan.
		TEST_METHOD(TheSeedAtTheSourceGridIsScaledToSourcePixels)
		{
			std::vector<float> f, b;
			Uniform(f, b, 6, 4, 2.0f, -1.25f);
			CameraLink::FieldCopy c;
			Assert::IsTrue(CameraLink::SeedToSourceGrid(f.data(), b.data(), Layout(6, 4, 1, 6, 4), c));
			Assert::IsTrue(c.Valid());
			Assert::IsTrue(c.origin == CameraLink::FieldOrigin::Seed);
			Assert::AreEqual(6, c.gridW);
			Assert::AreEqual(4, c.gridH);
			Assert::AreEqual(16.0, c.blockSize);
			Assert::IsFalse(c.HaveBackward(), L"forward only, as the live field the fit read before");
			for (size_t i = 0; i < c.Blocks(); ++i)
			{
				Assert::AreEqual(4.0f, c.fwdX[i]);
				Assert::AreEqual(-2.5f, c.fwdY[i]);
				Assert::AreEqual(1.0f, c.fwdTrust[i]);
			}
		}

		// lowRes 4K: 32-px seed texels onto the 16-px grid: block t takes texel t / 2 (its centre's texel), clamped; vectors x 4.
		TEST_METHOD(AHalvedSearchsSeedMapsOntoTheSixteenPixelGridDeterministically)
		{
			const int gw = 2, gh = 2, sw = 3, sh = 3;   // ceil(3 / 2) = 2: the seed covers the 3 x 3 source grid exactly
			std::vector<float> f(static_cast<size_t>(gw) * gh * 4, 0.0f), b(f.size(), 0.0f);
			for (int y = 0; y < gh; ++y)
				for (int x = 0; x < gw; ++x)
				{
					Put(f, gw, x, y, 0.25f * (1 + x + 2 * y), 0.0f, 0.5f, 1.0f);   // small motion: stays in its own block
					Put(b, gw, x, y, -0.25f * (1 + x + 2 * y), 0.0f, 0.5f, 1.0f);
				}
			CameraLink::FieldCopy c, again;
			Assert::IsTrue(CameraLink::SeedToSourceGrid(f.data(), b.data(), Layout(gw, gh, 2, sw, sh), c));
			Assert::IsTrue(CameraLink::SeedToSourceGrid(f.data(), b.data(), Layout(gw, gh, 2, sw, sh), again));
			Assert::AreEqual(sw, c.gridW);
			Assert::AreEqual(sh, c.gridH);
			Assert::AreEqual(16.0, c.blockSize);
			for (int ty = 0; ty < sh; ++ty)
				for (int tx = 0; tx < sw; ++tx)
				{
					const int sx = tx / 2, sy = ty / 2;
					const size_t o = static_cast<size_t>(ty) * sw + tx;
					Assert::AreEqual(4.0f * 0.25f * (1 + sx + 2 * sy), c.fwdX[o], L"texel t / per, x 4");
					Assert::AreEqual(1.0f, c.fwdTrust[o]);
				}
			Assert::IsTrue(c.fwdX == again.fwdX && c.fwdY == again.fwdY && c.fwdTrust == again.fwdTrust, L"deterministic");
		}

		// The live consistency pass's rule on the seed: the reach sentinel, a disagreeing backward vector, a low engine weight, a
		// non-finite texel: untrusted (the vector itself kept, or zero for a non-finite one).
		TEST_METHOD(TheSeedTrustIsTheLiveConsistencyRule)
		{
			std::vector<float> f, b;
			Uniform(f, b, 4, 1, 1.0f, 0.0f);
			Put(f, 4, 0, 0, 0.0f, 0.0f, 1.0e4f, 1.0f);   // beyond the searches' reach
			Put(f, 4, 1, 0, 1.0f, 0.0f, 3.0f, 1.0f);     // SAD at the tolerance; lands in block 1 (8 + 4 + 1 = 13): backward -1 agrees
			Put(f, 4, 2, 0, 1.0f, 0.0f, 1.0f, 0.0f);     // weight 0: SAD + 2 x tolerance > tolerance
			Put(f, 4, 3, 0, std::numeric_limits<float>::quiet_NaN(), 0.0f, 1.0f, 1.0f);
			CameraLink::FieldCopy c;
			Assert::IsTrue(CameraLink::SeedToSourceGrid(f.data(), b.data(), Layout(4, 1, 1, 4, 1), c));
			Assert::AreEqual(0.0f, c.fwdTrust[0], L"reach sentinel");
			Assert::AreEqual(1.0f, c.fwdTrust[1], L"at the tolerance, consistent");
			Assert::AreEqual(0.0f, c.fwdTrust[2], L"engine weight 0");
			Assert::AreEqual(0.0f, c.fwdTrust[3], L"non-finite");
			Assert::AreEqual(0.0f, c.fwdX[3]);
			// A backward vector that disagrees (block 1 lands in block 1, whose backward is now +1 instead of -1).
			Put(b, 4, 1, 0, 1.0f, 0.0f, 1.0f, 1.0f);
			Assert::IsTrue(CameraLink::SeedToSourceGrid(f.data(), b.data(), Layout(4, 1, 1, 4, 1), c));
			Assert::AreEqual(0.0f, c.fwdTrust[1], L"forward + backward = 2 seed px > 1.5");
			Assert::AreEqual(2.0f, c.fwdX[1], L"the vector is kept, only untrusted");
		}

		// A layout that does not add up is refused (never guessed): the seed must cover the source grid exactly.
		TEST_METHOD(AMismatchedSeedLayoutIsRefused)
		{
			std::vector<float> f, b;
			Uniform(f, b, 2, 2, 1.0f, 0.0f);
			CameraLink::FieldCopy c;
			Assert::IsFalse(CameraLink::SeedToSourceGrid(f.data(), b.data(), Layout(2, 2, 2, 5, 3), c), L"ceil(5 / 2) = 3 != 2");
			Assert::IsFalse(CameraLink::SeedToSourceGrid(f.data(), b.data(), Layout(2, 2, 1, 3, 3), c), L"16-px texels under a 3 x 3 grid");
			Assert::IsFalse(CameraLink::SeedToSourceGrid(f.data(), nullptr, Layout(2, 2, 1, 2, 2), c), L"no backward plane");
			CameraLink::SeedLayout l = Layout(2, 2, 1, 2, 2);
			l.sadTrustMax = 0.0f;
			Assert::IsFalse(CameraLink::SeedToSourceGrid(f.data(), b.data(), l, c));
		}

		// Run-time provenance: a copy of the GUIDED live field (origin Live, what Request(VerifiedField) yields) is never fitted;
		// a seed copy (and a hand-built one) is.
		TEST_METHOD(AGuidedLiveFieldCopyCannotBeFedToTheFit)
		{
			CameraLinkTest::Rig rig;
			FrameAnalysis::Store store;
			CameraLink::CameraLinkCore core(store);
			CameraLink::CameraLinkSettings on;
			on.p7Wanted = true;
			on.deterministic = true;
			core.OnSettings(on);
			core.AttachGraph(rig.table.get(), rig.bytes.get(), rig.indicator.get());
			CameraLink::TimingFacts tf;
			tf.measuredSourceHz = 24;
			core.SetTimingFacts(tf);
			core.Producer()->NoteTiming();
			CameraLink::FramePhaseFacts f;
			f.seq = 1;
			rig.PublishInputs(1);
			std::shared_ptr<CameraLink::FieldCopy> live = std::make_shared<CameraLink::FieldCopy>(*CameraLinkTest::PanJob(1, 8).field);
			live->origin = CameraLink::FieldOrigin::Live;
			Assert::IsFalse(core.SubmitField(live, f), L"the guided field is refused");
			Assert::AreEqual(uint64_t(1), core.Counters().guidedRefused.load());
			Assert::AreEqual(uint64_t(0), core.Counters().submitted.load(), L"nothing reached the fit worker");
			Assert::AreEqual(size_t(0), core.ProcessPending());
			std::shared_ptr<CameraLink::FieldCopy> seed = std::make_shared<CameraLink::FieldCopy>(*live);
			seed->origin = CameraLink::FieldOrigin::Seed;
			Assert::IsTrue(core.SubmitField(seed, f), L"the seed is fitted");
			Assert::AreEqual(size_t(1), core.ProcessPending());
			Assert::AreEqual(uint64_t(1), core.Counters().guidedRefused.load());
		}

		TEST_METHOD(TheCameraFitFallbackTextNamesTheSeed)
		{
			Assert::AreEqual(std::string("camera fit needs the optical-flow seed (engine off or matcher-only)"),
				std::string(CameraLink::kSeedAbsentText));
		}
	};
}
