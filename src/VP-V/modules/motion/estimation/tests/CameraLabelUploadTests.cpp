// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// Camera link K2-1: the P8 upload texels equal the inputs of the CPU accessor (FieldVector) bit for bit, and agree with the
// LabelPass float mirror except at documented threshold ties. CPU only.
#include "CameraLinkTestSupport.h"
#include "CameraModelGpuTestSupport.h"
#include <VP-V/modules/motion/estimation/global_model/link/CameraLabelProducer.h>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	TEST_CLASS(CameraLabelUploadTests)
	{
	public:
		TEST_METHOD(TexelsEqualTheAccessorInputsBitForBit)
		{
			using namespace CameraSynth;
			Field f(Grid4K());
			f.FromModel(PanModel(12.3, -4.7));
			f.AddNoise(0.05, 9);
			f.AddObject(100, 60, 6, 4, -20, 15);
			CameraModel::Fitter fitter;
			const CameraModel::CameraMotion rec = fitter.Fit(f.Input(1.0 / 24, 3));
			Assert::IsTrue(rec.blocks != nullptr && !rec.labelsOnGpu);
			std::vector<float> t;
			Assert::IsTrue(CameraLink::PackLabelTexels(*rec.blocks, t));
			const CameraModel::BlockData& b = *rec.blocks;
			const size_t n = static_cast<size_t>(b.gridW) * b.gridH;
			Assert::AreEqual(n * 4, t.size());
			for (size_t i = 0; i < n; ++i)
			{
				Assert::IsTrue(t[i * 4] == b.modelX[i] && t[i * 4 + 1] == b.modelY[i]);
				Assert::IsTrue(t[i * 4 + 2] == std::sqrt(b.resX[i] * b.resX[i] + b.resY[i] * b.resY[i]), L"the accessor's float residual");
				Assert::IsTrue((static_cast<int>(t[i * 4 + 3] + 0.5f) & 3) == static_cast<int>(b.label[i]));
				Assert::IsTrue(((static_cast<int>(t[i * 4 + 3] + 0.5f) >> 2) & 1) == (b.tinyMover[i] ? 1 : 0));
			}
			// The CPU accessor and the texel read agree on the camera/not-camera decision for every block.
			CameraModel::Policy on;
			on.enabled = true;
			on.rateLimit = 1.0;
			on.threshold = 0.0;
			on.floor = 0.0;
			CameraModel::BlendState st;
			st.Update(on, &rec);
			Assert::IsTrue(st.Weight() > 0.0);
			for (int by = 0; by < b.gridH; ++by)
				for (int bx = 0; bx < b.gridW; ++bx)
				{
					const size_t i = static_cast<size_t>(by) * b.gridW + bx;
					float ox = 0, oy = 0;
					const CameraModel::Choice c = CameraModel::FieldVector(on, st, &rec, bx, by, false, 1.0f, 2.0f, ox, oy);
					const bool cameraCpu = c == CameraModel::Choice::Model || c == CameraModel::Choice::Blend;
					const bool cameraTexel = (static_cast<int>(t[i * 4 + 3] + 0.5f) & 3) == 1;
					Assert::AreEqual(cameraTexel, cameraCpu);
				}
		}

		TEST_METHOD(AgreesWithTheLabelPassMirrorExceptAtThresholdTies)
		{
			using namespace CameraGpuSupport;
			const std::vector<Scenario> scenarios = MakeScenarios();
			for (const Scenario& s : scenarios)
			{
				CameraModel::Config cfg;
				CameraModel::Fitter fitter(cfg);
				const CameraSynth::Field& f = *s.field;
				CameraModel::PairInput in = f.Input(PairSeconds(), 4);
				const CameraModel::CameraMotion rec = fitter.Fit(in);
				if (!rec.blocks || rec.blocks->label.empty())
					continue;
				std::vector<float> up;
				Assert::IsTrue(CameraLink::PackLabelTexels(*rec.blocks, up));
				const CameraModel::LabelConstants k = CameraModel::MakeLabelConstants(rec, cfg, in);
				std::vector<float> field, aux, mirror;
				CameraModel::PackLabelInputs(in, *rec.blocks, field, aux);
				CameraModel::LabelGridMirror(k, field.data(), aux.data(), cfg.overlayZeroClusterMin, mirror);
				const std::vector<uint8_t> band = NearThresholdBand(k, f, mirror);
				const size_t n = static_cast<size_t>(k.gridW) * k.gridH;
				size_t mismatches = 0, outside = 0;
				for (size_t i = 0; i < n; ++i)
				{
					const int lu = CameraModel::CodeLabel(up[i * 4 + 3]);
					const int lm = CameraModel::CodeLabel(mirror[i * 4 + 3]);
					if (lu != lm)
					{
						++mismatches;
						outside += band[i] ? 0 : 1;
					}
				}
				Assert::AreEqual(size_t(0), outside, L"a label difference away from a documented tie");
				Assert::IsTrue(mismatches * 200 <= n, L"ties are rare");
			}
		}

		TEST_METHOD(RecordsWithoutCpuLabelsCannotBeUploadedAndTheModeRuleIsEitherOr)
		{
			CameraModel::BlockData empty;
			empty.gridW = 4; empty.gridH = 4;
			std::vector<float> t;
			Assert::IsFalse(CameraLink::PackLabelTexels(empty, t), L"labelsOnGpu records have no CPU arrays");
			using CameraLink::LabelMode;
			Assert::IsTrue(CameraLink::ChooseLabelMode(false, false) == LabelMode::Upload, L"default");
			Assert::IsTrue(CameraLink::ChooseLabelMode(true, false) == LabelMode::Compute, L"switch");
			Assert::IsTrue(CameraLink::ChooseLabelMode(false, true) == LabelMode::Compute, L"forced by labelsOnGpu");
			Assert::IsTrue(CameraLink::ChooseLabelMode(true, true) == LabelMode::Compute);
		}
	};
}
