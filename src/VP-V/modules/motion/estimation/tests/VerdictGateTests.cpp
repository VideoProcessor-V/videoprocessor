// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// Camera link K1-3 (D-K4): the verdict gate. The accessor weight (BlendState::ForcesZero) and the guidance gate share
// ONE usable set; a 60/40 two-layer field gets weight 0 and its guidance is refused. CPU only.
#include "CameraLinkTestSupport.h"
#include <VP-V/modules/motion/estimation/global_model/CameraAccessor.h>
#include <VP-V/modules/motion/estimation/global_model/GlobalModelMeasurement.h>
#include <VP-V/modules/motion/estimation/global_model/link/CameraUsable.h>
#include <VP-V/modules/motion/estimation/global_model/link/PairAssembly.h>
#include <VP-V/modules/motion/estimation/global_model/link/GuidanceScheduler.h>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	TEST_CLASS(VerdictGateTests)
	{
	public:
		static std::shared_ptr<CameraModel::CameraMotion> Record(CameraModel::Reason reason, double confidence)
		{
			auto r = std::make_shared<CameraModel::CameraMotion>();
			r->valid = true;
			r->confidence = confidence;
			r->reason = reason;
			r->params.type = CameraModel::ModelType::Translation;
			r->params.h[2] = 10;
			auto b = std::make_shared<CameraModel::BlockData>();
			b->gridW = 2; b->gridH = 2; b->blockSize = 16;
			b->label.assign(4, uint8_t(CameraModel::Label::Camera));
			b->modelX.assign(4, 10.0f); b->modelY.assign(4, 0.0f);
			b->resX.assign(4, 0.0f); b->resY.assign(4, 0.0f);
			r->blocks = b;
			return r;
		}

		TEST_METHOD(BothGatesArePinnedToTheSameUsableSet)
		{
			using namespace CameraModel;
			CameraModel::Policy on;
			on.enabled = true;
			on.rateLimit = 1.0;
			for (int i = 0; i <= int(Reason::Disabled); ++i)
			{
				const Reason reason = static_cast<Reason>(i);
				const bool expected = reason == Reason::Ok || reason == Reason::PostCutSeeded || reason == Reason::FlashBridged;
				Assert::AreEqual(expected, ReasonUsable(reason), L"the set itself");
				auto rec = Record(reason, 0.95);
				BlendState state;
				state.Update(on, rec.get());
				Assert::AreEqual(expected, state.Weight() > 0.0, L"accessor gate (ForcesZero)");
				Assert::AreEqual(expected, CameraLink::Usable(*rec), L"link gate");
			}
		}

		TEST_METHOD(UsableAlsoNeedsHalfConfidenceAndValid)
		{
			auto ok = Record(CameraModel::Reason::Ok, 0.5);
			Assert::IsTrue(CameraLink::Usable(*ok));
			auto low = Record(CameraModel::Reason::Ok, 0.49);
			Assert::IsFalse(CameraLink::Usable(*low));
			auto invalid = Record(CameraModel::Reason::Ok, 0.95);
			invalid->valid = false;
			Assert::IsFalse(CameraLink::Usable(*invalid));
			Assert::IsFalse(CameraLink::Usable(static_cast<const CameraModel::CameraMotion*>(nullptr)));
			Assert::IsNull(CameraLink::NotUsedText(*ok));
			auto amb = Record(CameraModel::Reason::Ambiguous, 0.3);
			Assert::IsNotNull(CameraLink::NotUsedText(*amb));
		}

		TEST_METHOD(SixtyFortyFieldIsAmbiguousWeightZeroAndGuidanceRefused)
		{
			using namespace CameraSynth;
			Field f(Grid4K());
			f.FromModel(PanModel(12, 0));
			const int x0 = static_cast<int>(0.4 * f.gridW);
			f.AddObject(x0, 0, f.gridW - x0, f.gridH, -30, 8);
			f.AddNoise(0.05, 53);
			auto copy = CameraLinkTest::CopyOf(f, 5);
			CameraLink::PairFacts facts;
			facts.pairSeconds = 1.0 / 24;
			CameraModel::PairInput in;
			Assert::IsTrue(CameraLink::AssemblePair(*copy, facts, copy->stamp, in) == CameraLink::AssembleStatus::Ready);
			CameraModel::Fitter fitter;
			auto rec = std::make_shared<CameraModel::CameraMotion>(fitter.Fit(in));
			Assert::IsTrue(rec->reason == CameraModel::Reason::Ambiguous, L"the verdict");
			Assert::IsFalse(CameraLink::Usable(*rec));
			// The accessor: weight 0 even when the consumer would accept the confidence.
			CameraModel::Policy on;
			on.enabled = true;
			on.threshold = 0.0;
			on.floor = 0.0;
			on.rateLimit = 1.0;
			auto forced = std::make_shared<CameraModel::CameraMotion>(*rec);
			forced->confidence = 0.95;   // even a high confidence cannot override the verdict
			CameraModel::BlendState state;
			state.Update(on, forced.get());
			Assert::AreEqual(0.0, state.Weight());
			// The measurement plan: every model-driven saving refused.
			CameraModel::MeasurementSwitches sw;
			sw.narrowWindow = true;
			sw.e1Skip = true;
			sw.modelGlobal = true;
			CameraModel::MeasurementInput mi;
			mi.prev = rec.get();
			mi.nextPairSeconds = 1.0 / 24;
			const CameraModel::MeasurementPlan plan = CameraModel::BuildMeasurementPlan(sw, CameraModel::MeasurementConfig(), mi);
			Assert::IsTrue(plan.hint.empty() && !plan.useModelGlobal && std::string(plan.fallbackReason) == "low confidence",
				L"the plan builder gives no per-block guidance from an ambiguous record");
			// ... and the guidance gate refuses it before a plan is even built.
			CameraLink::GuideInputs gi;
			gi.record = rec.get();
			const CameraLink::GuideResult gr = CameraLink::BuildGuide(sw, CameraModel::MeasurementConfig(), gi);
			Assert::IsTrue(gr.frame == nullptr && gr.refusal == CameraLink::Refusal::NotUsable, L"no guidance from an ambiguous record");
		}
	};
}
