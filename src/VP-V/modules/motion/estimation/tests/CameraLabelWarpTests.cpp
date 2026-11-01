// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Camera link K2-1 on the libplacebo WARP device: the uploaded P8 texture reads back exactly what the CPU accessor reads
// (model vector, float residual, label code), and the compute mode equals the LabelPass float mirror outside the documented
// near-threshold band. WARP unless VP_TEST_GPU=hardware.
#include "pch.h"
#include <VP-V/tests/VpLutGpuTestSupport.h>
#include <VP-V/modules/motion/estimation/global_model/link/CameraLabelProducer.h>
#include "CameraModelGpuTestSupport.h"
#include "CameraLinkTestSupport.h"

namespace VideoProcessorTest
{
	namespace
	{
		std::vector<float> Download(pl_gpu gpu, pl_tex tex, int w, int h)
		{
			std::vector<float> v(static_cast<size_t>(w) * h * 4, -1.0f);
			pl_tex_transfer_params dl{};
			dl.tex = tex;
			dl.ptr = v.data();
			Assert::IsTrue(pl_tex_download(gpu, &dl), L"download failed");
			return v;
		}
	}

	TEST_CLASS(CameraLabelWarpTests)
	{
	public:
		BEGIN_TEST_METHOD_ATTRIBUTE(UploadedTextureReadsBackExactlyAsTheCpuAccessorReads)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(UploadedTextureReadsBackExactlyAsTheCpuAccessorReads)
		{
			using namespace CameraGpuSupport;
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			const Scenario s = MakeScenario("pan+object", CameraSynth::PanModel(12.3, -4.7), 4);
			s.field->AddObject(100, 60, 6, 4, -20, 15);
			CameraModel::Fitter fitter;
			const CameraModel::CameraMotion rec = fitter.Fit(s.field->Input(PairSeconds(), 3));
			Assert::IsTrue(rec.blocks != nullptr);
			CameraLink::CameraLabelProducer prod(fixture.Log(), gpu, CameraLink::LabelMode::Upload, 3, 2, CameraModel::Config(), true);
			CameraLink::LabelTarget ring, live;
			ring.live = false; ring.index = 1;
			live.live = true; live.index = 0;
			Assert::IsTrue(prod.Produce(rec, nullptr, ring), L"upload ring slot");
			Assert::IsTrue(prod.Produce(rec, nullptr, live), L"upload live slot");
			const CameraModel::BlockData& b = *rec.blocks;
			std::vector<float> expect;
			Assert::IsTrue(CameraLink::PackLabelTexels(b, expect));
			for (CameraLink::LabelTarget t : { ring, live })
			{
				pl_tex tex = prod.TextureOf(t);
				Assert::IsNotNull(tex);
				const std::vector<float> got = Download(gpu, tex, b.gridW, b.gridH);
				Assert::AreEqual(expect.size(), got.size());
				for (size_t i = 0; i < got.size(); ++i)
					Assert::IsTrue(got[i] == expect[i], L"texel differs");
			}
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(ComputeModeMatchesTheMirrorOutsideNearThresholdBand)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(ComputeModeMatchesTheMirrorOutsideNearThresholdBand)
		{
			using namespace CameraGpuSupport;
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			const Scenario s = MakeScenario("pan", CameraSynth::PanModel(12.3, -4.7), 4);
			CameraModel::Config cfg;
			cfg.cpuLabels = false;   // labelsOnGpu: the compute path is forced
			CameraModel::Fitter fitter(cfg);
			CameraModel::PairInput in = s.field->Input(PairSeconds(), 3);
			const CameraModel::CameraMotion rec = fitter.Fit(in);
			Assert::IsTrue(rec.labelsOnGpu);
			Assert::IsTrue(CameraLink::ChooseLabelMode(false, rec.labelsOnGpu) == CameraLink::LabelMode::Compute);
			CameraLink::FieldCopy copy = *CameraLinkTest::CopyOf(*s.field, 3);
			CameraLink::CameraLabelProducer prod(fixture.Log(), gpu, CameraLink::LabelMode::Compute, 3, 2, cfg, true);
			CameraLink::LabelTarget t;
			t.live = false; t.index = 0;
			Assert::IsTrue(prod.Produce(rec, &copy, t), L"compute");
			const std::vector<float> got = Download(gpu, prod.TextureOf(t), in.gridW, in.gridH);
			const CameraModel::LabelConstants k = CameraModel::MakeLabelConstants(rec, cfg, in);
			std::vector<float> field, aux, mirror;
			CameraModel::PackLabelInputs(in, *rec.blocks, field, aux);
			CameraModel::LabelGridMirror(k, field.data(), aux.data(), cfg.overlayZeroClusterMin, mirror);
			const std::vector<uint8_t> band = NearThresholdBand(k, *s.field, mirror);
			{
				char dbg[400];
				size_t first = 0;
				for (; first < static_cast<size_t>(k.gridW) * k.gridH; ++first)
					if (CameraModel::CodeLabel(got[first * 4 + 3]) != CameraModel::CodeLabel(mirror[first * 4 + 3]))
						break;
				snprintf(dbg, sizeof(dbg), "LABELDBG first mismatch %zu got (%g %g %g %g) mirror (%g %g %g %g) pic %d %d %d %d k.c %g %g bs %g\n", first,
					got[first * 4], got[first * 4 + 1], got[first * 4 + 2], got[first * 4 + 3], mirror[first * 4], mirror[first * 4 + 1],
					mirror[first * 4 + 2], mirror[first * 4 + 3], rec.picture.left, rec.picture.top, rec.picture.right, rec.picture.bottom,
					k.cx, k.cy, k.blockSize);
				Logger::WriteMessage(dbg);
			}
			size_t outside = 0;
			for (size_t i = 0; i < static_cast<size_t>(k.gridW) * k.gridH; ++i)
				if (CameraModel::CodeLabel(got[i * 4 + 3]) != CameraModel::CodeLabel(mirror[i * 4 + 3]) && !band[i])
					++outside;
			Assert::AreEqual(size_t(0), outside, L"GPU label differs from the mirror away from a tie");
		}
	};
}
