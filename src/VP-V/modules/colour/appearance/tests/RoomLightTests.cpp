// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "pch.h"
#include "CppUnitTest.h"
#include <VP-V/modules/colour/appearance/RoomLight.h>

#include <cmath>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// CF-1 CAM-2: the room_light held state and its reader. Pure CPU.
namespace Tests
{
	TEST_CLASS(RoomLightTests)
	{
	public:
		TEST_METHOD(SurroundIsDarkDimAverageAtTheAnchors)
		{
			RoomLight::Anchors a;
			const Cam16::Surround dark = RoomLight::SurroundForLux(a.luxDark, a);
			const Cam16::Surround lit = RoomLight::SurroundForLux(a.luxLit, a);
			const Cam16::Surround mid = RoomLight::SurroundForLux(std::sqrt((a.luxDark + 0.5) * (a.luxLit + 0.5)) - 0.5, a);
			Assert::AreEqual(Cam16::kSurroundDark.c, dark.c, 1e-9);
			Assert::AreEqual(Cam16::kSurroundAverage.c, lit.c, 1e-9);
			Assert::AreEqual(Cam16::kSurroundDim.c, mid.c, 1e-6);
			Assert::AreEqual(Cam16::kSurroundDark.F, RoomLight::SurroundForLux(0.0, a).F, 1e-9);
		}

		TEST_METHOD(SurroundIsMonotoneAndContinuous)
		{
			RoomLight::Anchors a;
			double prev = -1.0;
			for (double lux = 0.0; lux <= 40.0; lux += 0.25)
			{
				const double c = RoomLight::SurroundForLux(lux, a).c;
				Assert::IsTrue(c >= prev - 1e-12);
				if (prev >= 0.0)
					Assert::IsTrue(c - prev < 0.02);
				prev = c;
			}
		}

		TEST_METHOD(DegenerateAnchorsAreAStepNotACrash)
		{
			RoomLight::Anchors a;
			a.luxDark = 10.0;
			a.luxLit = 10.0;
			Assert::AreEqual(Cam16::kSurroundDark.c, RoomLight::SurroundForLux(9.0, a).c, 1e-9);
			Assert::AreEqual(Cam16::kSurroundAverage.c, RoomLight::SurroundForLux(10.0, a).c, 1e-9);
			Assert::AreEqual(Cam16::kSurroundDark.c, RoomLight::SurroundForLux(std::nan(""), a).c, 1e-9);
		}

		TEST_METHOD(VeilIsReflectedLuxCappedAndZeroAtZero)
		{
			Assert::AreEqual(0.0, RoomLight::VeilNits(0.0, 0.01, 1.0), 0.0);
			Assert::AreEqual(0.05, RoomLight::VeilNits(5.0, 0.01, 1.0), 1e-12);
			Assert::AreEqual(1.0, RoomLight::VeilNits(500.0, 0.01, 1.0), 1e-12);
			Assert::AreEqual(0.0, RoomLight::VeilNits(5.0, 0.0, 1.0), 0.0);
		}

		TEST_METHOD(PresetsIgnoreTheSensor)
		{
			RoomLight::Smoother s;
			RoomLight::Record rec;
			rec.smoothedLux = 500.0;
			rec.valid = true;
			RoomLight::Params p;
			p.room = RoomLight::Room::Cinema;
			Assert::AreEqual(Cam16::kSurroundDark.c, RoomLight::Resolve(p, rec, s, false, 1.0).surround.c, 1e-12);
			p.room = RoomLight::Room::LivingLow;
			Assert::AreEqual(Cam16::kSurroundDim.c, RoomLight::Resolve(p, rec, s, false, 2.0).surround.c, 1e-12);
			p.room = RoomLight::Room::LivingLit;
			const RoomLight::Resolved r = RoomLight::Resolve(p, rec, s, false, 3.0);
			Assert::AreEqual(Cam16::kSurroundAverage.c, r.surround.c, 1e-12);
			Assert::AreEqual(0.0, r.veilNits, 0.0);
			Assert::IsFalse(r.fromSensor);
		}

		TEST_METHOD(AutoStartsFromTheLiveValueThenAdaptsInMinutes)
		{
			RoomLight::Smoother s;
			RoomLight::Record rec;
			rec.valid = true;
			rec.smoothedLux = 2.0;
			RoomLight::Params p;
			p.room = RoomLight::Room::Auto;
			double t = 0.0;
			RoomLight::Resolved r = RoomLight::Resolve(p, rec, s, false, t);
			Assert::AreEqual(2.0, r.appliedLux, 1e-6);
			rec.smoothedLux = 200.0;
			for (int i = 0; i < 10; ++i)   // 10 s: far from converged (tau up 60 s)
			{
				t += 1.0;
				r = RoomLight::Resolve(p, rec, s, true, t);
			}
			Assert::IsTrue(r.appliedLux > 2.0);
			Assert::IsTrue(r.appliedLux < 100.0);
			for (int i = 0; i < 600; ++i)  // 10 minutes: converged
			{
				t += 1.0;
				r = RoomLight::Resolve(p, rec, s, true, t);
			}
			Assert::IsTrue(std::fabs(r.appliedLux - 200.0) < 2.0);
			Assert::AreEqual(Cam16::kSurroundAverage.c, r.surround.c, 1e-6);
		}

		TEST_METHOD(DownIsSlowerThanUp)
		{
			RoomLight::Params p;
			p.room = RoomLight::Room::Auto;
			RoomLight::Record rec;
			rec.valid = true;
			RoomLight::Smoother up, down;
			rec.smoothedLux = 2.0;
			RoomLight::Resolve(p, rec, up, true, 0.0);
			rec.smoothedLux = 200.0;
			RoomLight::Resolve(p, rec, down, true, 0.0);
			RoomLight::Record hi = rec, lo = rec;
			lo.smoothedLux = 2.0;
			double ru = 0.0, rd = 0.0;
			for (int i = 1; i <= 30; ++i)
			{
				ru = up.Step(hi.smoothedLux, true, i);
				rd = down.Step(lo.smoothedLux, true, i);
			}
			// log-distance covered after 30 s: the rise covers more of its gap than the fall.
			const double riseFrac = (std::log2(ru + 0.5) - std::log2(2.5)) / (std::log2(200.5) - std::log2(2.5));
			const double fallFrac = (std::log2(200.5) - std::log2(rd + 0.5)) / (std::log2(200.5) - std::log2(2.5));
			Assert::IsTrue(riseFrac > fallFrac);
		}

		TEST_METHOD(StaticPictureSlewsBelowOnePercentPerSecond)
		{
			RoomLight::Smoother s;
			s.Step(2.0, false, 0.0);
			double prev = s.Lux();
			for (int i = 1; i <= 20; ++i)
			{
				const double v = s.Step(200.0, false, i);
				Assert::IsTrue(v >= prev - 1e-12);
				Assert::IsTrue(v - prev <= (prev + 0.5) * 0.0125 + 1e-9);   // below ~1 %/s of the value (log2 step)
				prev = v;
			}
		}

		TEST_METHOD(SensorLossIsNamedNotSilent)
		{
			RoomLight::Params p;
			p.room = RoomLight::Room::Auto;
			RoomLight::Smoother s;
			RoomLight::Record rec;
			rec.valid = true;
			rec.holding = true;
			rec.smoothedLux = 20.0;
			Assert::IsTrue(RoomLight::Resolve(p, rec, s, false, 0.0).status.find("held") != std::string::npos);
			rec.holding = false;
			rec.decaying = true;
			Assert::IsTrue(RoomLight::Resolve(p, rec, s, false, 1.0).status.find("decaying") != std::string::npos);
			rec = RoomLight::Record();
			Assert::IsTrue(RoomLight::Resolve(p, rec, s, false, 2.0).status.find("no reading") != std::string::npos);
		}

		TEST_METHOD(HeldStateIsSingleWriterLatestSealed)
		{
			RoomLight::HeldState h;
			RoomLight::Record out;
			Assert::IsFalse(h.ReadLatest(out));
			RoomLight::Record r;
			r.valid = true;
			r.smoothedLux = 12.0;
			Assert::AreEqual<uint64_t>(1, h.Publish(r));
			r.smoothedLux = 30.0;
			Assert::AreEqual<uint64_t>(2, h.Publish(r));
			Assert::IsTrue(h.ReadLatest(out));
			Assert::AreEqual(30.0, out.smoothedLux, 0.0);
			Assert::AreEqual<uint64_t>(2, out.seq);
			h.Reset();
			Assert::IsFalse(h.ReadLatest(out));
		}

		TEST_METHOD(MakeRecordCopiesTheFilterWithoutEstimatingAnything)
		{
			AmbientAdapt::Filter f;
			const RoomLight::Record empty = RoomLight::MakeRecord(f, 5);
			Assert::IsFalse(empty.valid);
			Assert::AreEqual(0.0, empty.smoothedLux, 0.0);
			Assert::AreEqual<uint64_t>(5, empty.seq);
		}
	};
}
