// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// Camera link K2-3: the per-consumer binding: off -> no define and enabled 0; one blend update per SOURCE frame; a reset
// returns the weight to 0 and the ramp is rate limited; an Ambiguous record gives 0; P8 absent -> enabled 0. CPU only.
#include <VP-V/modules/motion/estimation/global_model/link/CameraConsumerBinding.h>

#include <memory>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	namespace
	{
		std::shared_ptr<CameraModel::CameraMotion> Rec(double confidence, CameraModel::Reason reason = CameraModel::Reason::Ok)
		{
			auto r = std::make_shared<CameraModel::CameraMotion>();
			r->valid = true;
			r->confidence = confidence;
			r->reason = reason;
			auto b = std::make_shared<CameraModel::BlockData>();
			b->gridW = 4; b->gridH = 3; b->blockSize = 16;
			b->absThresholdPx = 0.5f; b->relThreshold = 0.05f;
			r->blocks = b;
			return r;
		}

		CameraLink::CameraLabelsView Labels(bool available = true)
		{
			CameraLink::CameraLabelsView v;
			v.available = available;
			v.tex = available ? reinterpret_cast<pl_tex>(static_cast<uintptr_t>(0x2000)) : nullptr;
			v.gridW = 4; v.gridH = 3;
			return v;
		}
	}

	TEST_CLASS(CameraConsumerBindingTests)
	{
	public:
		TEST_METHOD(OffMeansNoDefineNoTextureAndEnabledZero)
		{
			CameraLink::CameraConsumerBinding b(CameraLink::CameraConsumer::GrainMap, false);
			auto rec = Rec(0.95);
			for (uint64_t f = 1; f <= 5; ++f)
			{
				Assert::IsTrue(b.Update(f, rec.get(), Labels()) == CameraLink::BindOutcome::Off);
				Assert::AreEqual(0, b.Current().constants.enabled);
				Assert::IsFalse(b.Current().defineActive, L"shader text and cache key unchanged");
				Assert::IsTrue(b.Current().tex == nullptr);
			}
			Assert::AreEqual(0.0, b.Weight());
		}

		TEST_METHOD(OnBindsTheTextureTheDefineAndTheSixUniformValues)
		{
			CameraLink::CameraConsumerBinding b(CameraLink::CameraConsumer::GrainMap, true);
			b.MutablePolicy().rateLimit = 1.0;
			auto rec = Rec(0.95);
			Assert::IsTrue(b.Update(1, rec.get(), Labels()) == CameraLink::BindOutcome::Applied);
			const CameraLink::CameraBinding& c = b.Current();
			Assert::AreEqual(1, c.constants.enabled);
			Assert::AreEqual(1.0f, c.constants.weight);
			Assert::AreEqual(0.5f, c.constants.absThr);
			Assert::AreEqual(4, c.constants.gridW);
			Assert::IsTrue(c.defineActive);
			Assert::AreEqual(std::string("CAMERA_MODEL_GRAIN_MAP"), c.defineName);
			Assert::IsTrue(c.tex != nullptr);
			Assert::IsTrue(c.domain.IsIdentity());
			const pl_shader_desc d = c.Desc();
			Assert::AreEqual(std::string("camera_model"), std::string(d.desc.name));
			Assert::AreEqual(std::string("camera_enabled"), std::string(CameraLink::kUniformEnabled));
			Assert::AreEqual(std::string("camera_grid"), std::string(CameraLink::kUniformGrid));
		}

		TEST_METHOD(OneBlendUpdatePerSourceFrameCadenceRepeatsDoNotAdvanceTheRamp)
		{
			CameraLink::CameraConsumerBinding b(CameraLink::CameraConsumer::GrainMap, true);
			b.MutablePolicy().rateLimit = 0.25;
			auto rec = Rec(0.95);
			b.Update(10, rec.get(), Labels());
			const double w1 = b.Weight();
			Assert::AreEqual(0.25, w1, 1e-12);
			// Three more presentations of the SAME source frame (2:3 style repeats).
			for (int i = 0; i < 3; ++i)
				b.Update(10, rec.get(), Labels());
			Assert::AreEqual(w1, b.Weight(), L"unchanged by repeats");
			Assert::AreEqual(uint64_t(3), b.Stats().repeats);
			b.Update(11, rec.get(), Labels());
			Assert::AreEqual(0.5, b.Weight(), 1e-12, L"next source frame: one more step");
		}

		TEST_METHOD(ResetReturnsToZeroThenARateLimitedRamp)
		{
			CameraLink::CameraConsumerBinding b(CameraLink::CameraConsumer::GrainMap, true);
			b.MutablePolicy().rateLimit = 0.25;
			auto rec = Rec(0.95);
			for (uint64_t f = 1; f <= 6; ++f)
				b.Update(f, rec.get(), Labels());
			Assert::AreEqual(1.0, b.Weight(), 1e-12);
			CameraLink::CameraReaderHooks hooks = b.Hooks();
			hooks.reset();   // the Indicator reader: a resync
			Assert::AreEqual(0.0, b.Weight());
			b.Update(7, rec.get(), Labels());
			Assert::AreEqual(0.25, b.Weight(), 1e-12);
		}

		TEST_METHOD(AmbiguousRecordGivesWeightZeroAndNoModelBinding)
		{
			CameraLink::CameraConsumerBinding b(CameraLink::CameraConsumer::GrainMap, true);
			b.MutablePolicy().rateLimit = 1.0;
			auto good = Rec(0.95);
			b.Update(1, good.get(), Labels());
			Assert::AreEqual(1.0, b.Weight());
			auto amb = Rec(0.95, CameraModel::Reason::Ambiguous);
			b.Update(2, amb.get(), Labels());
			Assert::AreEqual(0.0, b.Weight());
			Assert::AreEqual(0.0, double(b.Current().constants.weight), L"enabled but weight 0: shader returns today's value");
		}

		TEST_METHOD(AbsentLabelsOrRecordMeanEnabledZeroAndAreCounted)
		{
			CameraLink::CameraConsumerBinding b(CameraLink::CameraConsumer::Debander, true);
			auto rec = Rec(0.95);
			Assert::IsTrue(b.Update(1, rec.get(), Labels(false)) == CameraLink::BindOutcome::NoLabels);
			Assert::AreEqual(0, b.Current().constants.enabled);
			Assert::IsFalse(b.Current().defineActive);
			Assert::IsTrue(b.Update(2, nullptr, Labels()) == CameraLink::BindOutcome::NoRecord);
			Assert::AreEqual(0, b.Current().constants.enabled);
			Assert::AreEqual(uint64_t(1), b.Stats().noLabels);
			Assert::AreEqual(uint64_t(1), b.Stats().noRecord);
		}
	};
}
