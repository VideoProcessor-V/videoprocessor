// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Camera link K2-1 on the libplacebo WARP device: the uploaded P8 texture reads back exactly what the CPU accessor reads
// (model vector, float residual, label code), and the compute mode equals the LabelPass float mirror outside the documented
// near-threshold band. WARP unless VP_TEST_GPU=hardware.
#include "pch.h"
#include <VP-V/tests/VpLutGpuTestSupport.h>
#include <VP-V/modules/motion/estimation/global_model/link/AheadFlow.h>
#include <VP-V/modules/motion/estimation/global_model/link/CameraLabelProducer.h>
#include "CameraModelGpuTestSupport.h"
#include "CameraLinkTestSupport.h"

#include <cmath>
#include <limits>

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

		CameraLink::FieldCopy UniformFieldCopy(int w, int h, double blockSize, float vx, float vy, uint64_t seq = 1)
		{
			CameraLink::FieldCopy f;
			f.seq = seq;
			f.pairFrom = seq > 0 ? seq - 1 : 0;
			f.pairTo = seq;
			f.gridW = w;
			f.gridH = h;
			f.blockSize = blockSize;
			const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
			f.fwdX.assign(n, vx);
			f.fwdY.assign(n, vy);
			f.fwdTrust.assign(n, 1.0f);
			return f;
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
			size_t outside = 0;
			for (size_t i = 0; i < static_cast<size_t>(k.gridW) * k.gridH; ++i)
				if (CameraModel::CodeLabel(got[i * 4 + 3]) != CameraModel::CodeLabel(mirror[i * 4 + 3]) && !band[i])
					++outside;
			Assert::AreEqual(size_t(0), outside, L"GPU label differs from the mirror away from a tie");
		}

		// GEM-129 (AB1#1): Compute mode refuses any incompatible field/blocks grid, non-integer scale,
		// upscaling ratio, or mismatched excluded mask with "camera labels: grid mismatch" and increments
		// GridMismatchCount() on every refusal, while still succeeding on a valid matching grid.
		BEGIN_TEST_METHOD_ATTRIBUTE(ComputeModeRefusesGridMismatchAndCountsEachFailure)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(ComputeModeRefusesGridMismatchAndCountsEachFailure)
		{
			using namespace CameraGpuSupport;
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();

			CameraModel::Config cfg;
			cfg.cpuLabels = false;
			CameraLink::CameraLabelProducer prod(fixture.Log(), gpu, CameraLink::LabelMode::Compute, 2, 1, cfg, true);
			Assert::AreEqual<uint64_t>(0u, prod.GridMismatchCount());
			Assert::AreEqual<uint64_t>(0u, prod.RegridCount());

			auto makeRecord = [](int gw, int gh, double bs, size_t excludedSize) {
				CameraModel::CameraMotion rec;
				rec.seq = 10;
				rec.pairFrom = 9;
				rec.pairTo = 10;
				rec.pairSeconds = PairSeconds();
				rec.valid = true;
				rec.confidence = 0.9;
				rec.reason = CameraModel::Reason::Ok;
				rec.labelsOnGpu = true;
				rec.params = CameraSynth::PanModel(8.0, -3.0);
				rec.picture.left = 0; rec.picture.top = 0; rec.picture.right = 1920; rec.picture.bottom = 1080;
				auto bd = std::make_shared<CameraModel::BlockData>();
				bd->gridW = gw;
				bd->gridH = gh;
				bd->blockSize = bs;
				bd->absThresholdPx = 0.75f;
				bd->relThreshold = 0.05f;
				bd->excluded.assign(excludedSize, 0);
				rec.blocks = bd;
				return rec;
			};

			CameraLink::LabelTarget target;
			target.live = false;
			target.index = 0;

			std::vector<float> rx, ry, rt;
			CameraModel::PairInput pairIn;
			const CameraModel::CameraMotion rec120x68 = makeRecord(120, 68, 16.0, static_cast<size_t>(120) * 68);

			auto assertRefused = [&](const CameraModel::CameraMotion& rec, const CameraLink::FieldCopy& field,
				uint64_t expectedMismatches, const wchar_t* tag)
			{
				Assert::IsFalse(CameraLink::PrepareComputePairInput(field, *rec.blocks, rec.picture, pairIn, rx, ry, rt), tag);
				Assert::IsFalse(prod.Produce(rec, &field, target), tag);
				Assert::IsTrue(prod.Error().find("camera labels: grid mismatch") != std::string::npos, tag);
				Assert::AreEqual(expectedMismatches, prod.GridMismatchCount(), tag);
				Assert::AreEqual<uint64_t>(0u, prod.RegridCount(), tag);
				Assert::IsNull(prod.TextureOf(target), tag);
			};

			// 1. Incompatible dimensions at scale=1: 100x60 field vs 120x68 rec.blocks.
			assertRefused(rec120x68, UniformFieldCopy(100, 60, 16.0, 8.0f, -3.0f), 1u, L"100x60 vs 120x68");

			// 2. Non-integer blockSize ratio (16 / 10 = 1.6).
			assertRefused(rec120x68, UniformFieldCopy(192, 108, 10.0, 8.0f, -3.0f), 2u, L"non-integer scale 1.6");

			// 3. Coarser field than consumer (ratio = 0.5 < 1.0): 120x68@16px field vs 240x135@8px blocks.
			const CameraModel::CameraMotion rec240x135 = makeRecord(240, 135, 8.0, static_cast<size_t>(240) * 135);
			assertRefused(rec240x135, UniformFieldCopy(120, 68, 16.0, 8.0f, -3.0f), 3u, L"coarser field than blocks");

			// 4. Off-by-one dimension mismatch at integer scale=2: ceil(238/2)=119 != 120.
			assertRefused(rec120x68, UniformFieldCopy(238, 135, 8.0, 8.0f, -3.0f), 4u, L"238x135@8px vs 120x68@16px");

			// 5. Same 120x68 grid & 16px blockSize, but corrupt non-empty excluded mask size (100*60 != 120*68).
			const CameraModel::CameraMotion recBadExcl = makeRecord(120, 68, 16.0, static_cast<size_t>(100) * 60);
			assertRefused(recBadExcl, UniformFieldCopy(120, 68, 16.0, 8.0f, -3.0f), 5u, L"bad excluded size");

			// Positive control on the exact same producer and target: matching 120x68 grid succeeds cleanly
			// without incrementing GridMismatchCount() or RegridCount().
			const CameraLink::FieldCopy validField = UniformFieldCopy(120, 68, 16.0, 8.0f, -3.0f);
			Assert::IsTrue(CameraLink::PrepareComputePairInput(validField, *rec120x68.blocks, rec120x68.picture, pairIn, rx, ry, rt),
				L"matching grid prepares cleanly");
			Assert::IsTrue(prod.Produce(rec120x68, &validField, target), L"matching grid dispatches cleanly after refusals");
			Assert::IsTrue(prod.Error().empty(), L"error cleared on success");
			Assert::AreEqual<uint64_t>(5u, prod.GridMismatchCount(), L"mismatch count unchanged on valid produce");
			Assert::AreEqual<uint64_t>(0u, prod.RegridCount(), L"scale=1 does not increment regrid count");
			pl_tex tex = prod.TextureOf(target);
			Assert::IsNotNull(tex);
			Assert::AreEqual(120, tex->params.w);
			Assert::AreEqual(68, tex->params.h);
		}

		// GEM-129 (AB1#1): 8-px native AheadFit field (240x135, blockSize=8) with a 16-px consumer record
		// (120x68, blockSize=16 via RegridToConsumerGrid) increments RegridCount(), dispatches at 120x68,
		// and matches CPU Labels() bit-for-bit across every block including the odd bottom row (135 -> 68).
		BEGIN_TEST_METHOD_ATTRIBUTE(ComputeModeRegrids8PxAheadFitTo16PxConsumerAndMatchesCpuBitForBit)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(ComputeModeRegrids8PxAheadFitTo16PxConsumerAndMatchesCpuBitForBit)
		{
			using namespace CameraGpuSupport;
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();

			CameraSynth::GridSpec g8;
			g8.w = 240;
			g8.h = 135;
			g8.blockSize = 8.0;
			CameraSynth::Field f(g8);
			f.FromModel(CameraSynth::PanModel(12.3, -4.7));
			f.AddNoise(0.05, 42);
			f.AddObject(60, 40, 24, 16, -20.0, 15.0);      // 12x8 consumer blocks -> Object
			f.AddLogo(16, 20, 12, 12, false);              // 6x6 consumer blocks -> Overlay via zero-cluster
			f.AddBars(16, 0);                              // top 8 consumer rows excluded; bottom odd row ty=67 stays active
			f.AddTextTiles(80, 90, 12, 8, 52);             // interior 6x4 consumer blocks excluded -> Overlay

			const CameraModel::PairInput in8 = f.Input(PairSeconds(), 5);

			CameraModel::Config cfgCpu;
			cfgCpu.cpuLabels = true;
			CameraModel::Fitter fitterCpu(cfgCpu);
			const CameraModel::CameraMotion recCpuNative = fitterCpu.Fit(in8);
			Assert::IsTrue(recCpuNative.valid && recCpuNative.blocks != nullptr, L"CPU native 8-px fit valid");
			const auto recCpu16 = CameraLink::RegridToConsumerGrid(
				std::make_shared<CameraModel::CameraMotion>(recCpuNative), 120, 68, 16.0);
			Assert::IsTrue(recCpu16 && recCpu16->blocks && recCpu16->blocks->gridW == 120 && recCpu16->blocks->gridH == 68);
			Assert::AreEqual(16.0, recCpu16->blocks->blockSize);

			CameraModel::Config cfgGpu = cfgCpu;
			cfgGpu.cpuLabels = false;
			CameraModel::Fitter fitterGpu(cfgGpu);
			const CameraModel::CameraMotion recGpuNative = fitterGpu.Fit(in8);
			Assert::IsTrue(recGpuNative.valid && recGpuNative.labelsOnGpu && recGpuNative.blocks != nullptr, L"GPU native 8-px fit valid");
			const auto recGpu16 = CameraLink::RegridToConsumerGrid(
				std::make_shared<CameraModel::CameraMotion>(recGpuNative), 120, 68, 16.0);
			Assert::IsTrue(recGpu16 && recGpu16->blocks && recGpu16->blocks->gridW == 120 && recGpu16->blocks->gridH == 68);
			Assert::AreEqual(16.0, recGpu16->blocks->blockSize);
			Assert::IsTrue(recGpu16->blocks->label.empty(), L"GPU record carries no CPU labels");

			CameraLink::FieldCopy nativeCopy = *CameraLinkTest::CopyOf(f, 5);
			nativeCopy.origin = CameraLink::FieldOrigin::Ahead;
			Assert::AreEqual(240, nativeCopy.gridW);
			Assert::AreEqual(135, nativeCopy.gridH);
			Assert::AreEqual(8.0, nativeCopy.blockSize);

			CameraLink::CameraLabelProducer prod(fixture.Log(), gpu, CameraLink::LabelMode::Compute, 2, 1, cfgGpu, true);
			CameraLink::LabelTarget t0;
			t0.live = false;
			t0.index = 0;

			Assert::IsTrue(prod.Produce(*recGpu16, &nativeCopy, t0), L"8-px native -> 16-px consumer regrid compute");
			Assert::AreEqual<uint64_t>(1u, prod.RegridCount(), L"RegridCount incremented for 8px -> 16px");
			Assert::AreEqual<uint64_t>(0u, prod.GridMismatchCount(), L"no grid mismatch on valid 2x regrid");

			pl_tex tex = prod.TextureOf(t0);
			Assert::IsNotNull(tex);
			Assert::AreEqual(120, tex->params.w, L"P8 texture dispatched at consumer gridW=120");
			Assert::AreEqual(68, tex->params.h, L"P8 texture dispatched at consumer gridH=68");

			const std::vector<float> got = Download(gpu, tex, 120, 68);
			const size_t total = static_cast<size_t>(120) * 68;
			Assert::AreEqual(total, recCpu16->blocks->label.size());

			size_t mismatches = 0, camCount = 0, objCount = 0, ovlCount = 0;
			for (size_t i = 0; i < total; ++i)
			{
				const int gpuLbl = CameraModel::CodeLabel(got[i * 4 + 3]);
				const int cpuLbl = static_cast<int>(recCpu16->blocks->label[i]);
				if (gpuLbl != cpuLbl)
					++mismatches;
				if (gpuLbl == static_cast<int>(CameraModel::Label::Camera)) ++camCount;
				else if (gpuLbl == static_cast<int>(CameraModel::Label::Object)) ++objCount;
				else if (gpuLbl == static_cast<int>(CameraModel::Label::Overlay)) ++ovlCount;
			}
			Assert::AreEqual(size_t(0), mismatches, L"regridded GPU labels match CPU Labels() bit-for-bit");
			Assert::IsTrue(camCount > 0 && objCount > 0 && ovlCount > 0, L"non-vacuous coverage across Camera, Object, and Overlay");

			// Spot-check every distinct region on the 120x68 consumer grid (including odd bottom row ty=67).
			auto lblAt = [&](int tx, int ty) { return CameraModel::CodeLabel(got[(static_cast<size_t>(ty) * 120 + tx) * 4 + 3]); };
			Assert::AreEqual(static_cast<int>(CameraModel::Label::Camera), lblAt(60, 67), L"odd bottom row ty=67 active camera block");
			Assert::AreEqual(static_cast<int>(CameraModel::Label::Object), lblAt(35, 22), L"moving object block");
			Assert::AreEqual(static_cast<int>(CameraModel::Label::Overlay), lblAt(10, 12), L"static logo zero-cluster overlay");
			Assert::AreEqual(static_cast<int>(CameraModel::Label::Overlay), lblAt(50, 3), L"top bar excluded overlay");
			Assert::AreEqual(static_cast<int>(CameraModel::Label::Overlay), lblAt(42, 46), L"interior text-guard excluded overlay");

			// Negative controls on the exact same consumer record:
			// 1) Scale=1 matching 120x68 field succeeds WITHOUT incrementing RegridCount().
			CameraModel::PairInput regriddedIn;
			std::vector<float> rx, ry, rt;
			Assert::IsTrue(CameraLink::PrepareComputePairInput(nativeCopy, *recGpu16->blocks, recGpu16->picture, regriddedIn, rx, ry, rt));
			CameraLink::FieldCopy sameGridCopy;
			sameGridCopy.seq = 5; sameGridCopy.pairFrom = 4; sameGridCopy.pairTo = 5;
			sameGridCopy.gridW = 120; sameGridCopy.gridH = 68; sameGridCopy.blockSize = 16.0;
			sameGridCopy.fwdX = rx; sameGridCopy.fwdY = ry; sameGridCopy.fwdTrust = rt;
			CameraLink::LabelTarget t1;
			t1.live = false;
			t1.index = 1;
			Assert::IsTrue(prod.Produce(*recGpu16, &sameGridCopy, t1), L"scale=1 produce succeeds");
			Assert::AreEqual<uint64_t>(1u, prod.RegridCount(), L"scale=1 does not increment RegridCount");

			// 2) Breaking the integer-regrid ceiling invariant (238x135@8px vs 120x68@16px) is refused.
			const CameraLink::FieldCopy badWidth = UniformFieldCopy(238, 135, 8.0, 12.3f, -4.7f, 5);
			Assert::IsFalse(prod.Produce(*recGpu16, &badWidth, t1), L"broken regrid width refused");
			Assert::AreEqual<uint64_t>(1u, prod.GridMismatchCount(), L"broken regrid counted as grid mismatch");
		}

		// GEM-129 (AB1#1): MakeLabelConstants honors rec.blocks->absThresholdPx (preserving R2F-raised thresholds)
		// and falls back to NVOF quantumPx / sqrt(12) before nominal precision. Same-input negative controls prove
		// that omitting rec.blocks->absThresholdPx (or quantumPx) mislabels in-band residuals as Label::Object.
		BEGIN_TEST_METHOD_ATTRIBUTE(MakeLabelConstantsHonorsR2fAndQuantumThresholdsWithNegativeControl)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(MakeLabelConstantsHonorsR2fAndQuantumThresholdsWithNegativeControl)
		{
			using namespace CameraGpuSupport;
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();

			// ---- Part A: Controlled R2F absThresholdPx = 1.25f with 0.95 px residual (between nominal 0.75 and 1.25) ----
			CameraModel::Config cfg;
			cfg.cpuLabels = false;   // default absThresholdSigmas = 3.0, relThreshold = 0.05

			CameraModel::CameraMotion recR2f;
			recR2f.seq = 20; recR2f.pairFrom = 19; recR2f.pairTo = 20;
			recR2f.pairSeconds = PairSeconds();
			recR2f.valid = true;
			recR2f.confidence = 0.9;
			recR2f.reason = CameraModel::Reason::Ok;
			recR2f.labelsOnGpu = true;
			// Small pan (2.0 px) keeps relThreshold * |model| = 0.10 px well below both 0.75 px and 1.25 px.
			recR2f.params = CameraSynth::PanModel(2.0, 0.0);
			recR2f.picture.left = 0; recR2f.picture.top = 0; recR2f.picture.right = 1920; recR2f.picture.bottom = 1080;

			auto bdR2f = std::make_shared<CameraModel::BlockData>();
			bdR2f->gridW = 120; bdR2f->gridH = 68; bdR2f->blockSize = 16.0;
			bdR2f->excluded.assign(static_cast<size_t>(120) * 68, 0);
			bdR2f->absThresholdPx = 1.25f;
			bdR2f->relThreshold = 0.05f;
			recR2f.blocks = bdR2f;

			CameraLink::FieldCopy field = UniformFieldCopy(120, 68, 16.0, 2.0f, 0.0f, 20);
			field.quantumPx = 0.0;
			// Probe patch A (0.95 px residual): above nominal 0.75 px and quantum 0.866 px, below R2F 1.25 px.
			for (int y = 10; y < 14; ++y)
				for (int x = 10; x < 14; ++x)
					field.fwdX[static_cast<size_t>(y) * 120 + x] = 2.95f;
			// Probe patch B (0.81 px residual): above nominal 0.75 px, below 4K NVOF quantum 3/sqrt(12) = 0.8660254 px.
			for (int y = 10; y < 14; ++y)
				for (int x = 20; x < 24; ++x)
					field.fwdX[static_cast<size_t>(y) * 120 + x] = 2.81f;
			// Real moving object patch (5.0 px residual): strictly above 1.25 px -> must remain Object.
			for (int y = 20; y < 24; ++y)
				for (int x = 40; x < 44; ++x)
					field.fwdX[static_cast<size_t>(y) * 120 + x] = -3.0f;

			CameraModel::PairInput in;
			in.gridW = 120; in.gridH = 68; in.blockSize = 16.0; in.picture = recR2f.picture;
			in.fwdX = field.fwdX.data(); in.fwdY = field.fwdY.data(); in.fwdTrust = field.fwdTrust.data();
			in.quantumPx = 0.0;

			// Constant derivation checks (including non-finite / custom relThreshold guards).
			const CameraModel::LabelConstants kR2f = CameraModel::MakeLabelConstants(recR2f, cfg, in);
			Assert::AreEqual(1.25f, kR2f.absThr, 1e-6f, L"MakeLabelConstants uses rec.blocks->absThresholdPx");
			Assert::AreEqual(0.05f, kR2f.rel, 1e-6f);

			CameraModel::CameraMotion recZeroThr = recR2f;
			auto bdZeroThr = std::make_shared<CameraModel::BlockData>(*bdR2f);
			bdZeroThr->absThresholdPx = 0.0f;
			recZeroThr.blocks = bdZeroThr;
			const CameraModel::LabelConstants kZeroThr = CameraModel::MakeLabelConstants(recZeroThr, cfg, in);
			Assert::AreEqual(0.75f, kZeroThr.absThr, 1e-6f, L"absThresholdPx == 0 falls back to nominal 3 * 0.25 = 0.75");

			CameraModel::CameraMotion recNoBlocks = recR2f;
			recNoBlocks.blocks.reset();
			const CameraModel::LabelConstants kNoBlocks = CameraModel::MakeLabelConstants(recNoBlocks, cfg, in);
			Assert::AreEqual(0.75f, kNoBlocks.absThr, 1e-6f, L"null blocks falls back to nominal 0.75");

			// Guard checks: NaN / +Inf / negative absThresholdPx must fall back safely; positive relThreshold is honored.
			for (float badThr : { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), -2.0f })
			{
				CameraModel::CameraMotion recBad = recR2f;
				auto bdBad = std::make_shared<CameraModel::BlockData>(*bdR2f);
				bdBad->absThresholdPx = badThr;
				bdBad->relThreshold = 0.12f;
				recBad.blocks = bdBad;
				const CameraModel::LabelConstants kBad = CameraModel::MakeLabelConstants(recBad, cfg, in);
				Assert::AreEqual(0.75f, kBad.absThr, 1e-6f, L"invalid absThresholdPx falls back to nominal 0.75");
				Assert::AreEqual(0.12f, kBad.rel, 1e-6f, L"valid positive rec.blocks->relThreshold is honored");
			}

			// Run GPU Compute on WARP for R2F record vs same-input negative control (absThresholdPx = 0).
			CameraLink::CameraLabelProducer prod(fixture.Log(), gpu, CameraLink::LabelMode::Compute, 3, 1, cfg, true);
			CameraLink::LabelTarget t0, t1, t2;
			t0.live = false; t0.index = 0;
			t1.live = false; t1.index = 1;
			t2.live = false; t2.index = 2;

			Assert::IsTrue(prod.Produce(recR2f, &field, t0), L"GPU compute with R2F absThresholdPx=1.25");
			const std::vector<float> gotR2f = Download(gpu, prod.TextureOf(t0), 120, 68);

			Assert::IsTrue(prod.Produce(recZeroThr, &field, t1), L"GPU compute negative control with absThresholdPx=0");
			const std::vector<float> gotZero = Download(gpu, prod.TextureOf(t1), 120, 68);

			// Also verify the rec.blocks == nullptr mirror negative control on the exact same field/aux.
			std::vector<float> packedField, packedAux, mirrorNoBlocks;
			CameraModel::PackLabelInputs(in, *bdZeroThr, packedField, packedAux);
			CameraModel::LabelGridMirror(kNoBlocks, packedField.data(), packedAux.data(), cfg.overlayZeroClusterMin, mirrorNoBlocks);

			for (int y = 10; y < 14; ++y)
				for (int x = 10; x < 14; ++x)
				{
					const size_t i = static_cast<size_t>(y) * 120 + x;
					Assert::AreEqual(static_cast<int>(CameraModel::Label::Camera), CameraModel::CodeLabel(gotR2f[i * 4 + 3]),
						L"0.95 px residual is Camera under R2F absThresholdPx=1.25");
					Assert::AreEqual(static_cast<int>(CameraModel::Label::Object), CameraModel::CodeLabel(gotZero[i * 4 + 3]),
						L"negative control (absThresholdPx=0): exact same 0.95 px residual is Object");
					Assert::AreEqual(static_cast<int>(CameraModel::Label::Object), CameraModel::CodeLabel(mirrorNoBlocks[i * 4 + 3]),
						L"negative control (blocks=nullptr): exact same 0.95 px residual is Object");
				}
			for (int y = 20; y < 24; ++y)
				for (int x = 40; x < 44; ++x)
				{
					const size_t i = static_cast<size_t>(y) * 120 + x;
					Assert::AreEqual(static_cast<int>(CameraModel::Label::Object), CameraModel::CodeLabel(gotR2f[i * 4 + 3]),
						L"5.0 px mover stays Object even under R2F absThresholdPx=1.25");
				}

			// ---- Part B: NVOF quantumPx fallback parity (quantumPx = 1.0 -> absThr = 3 / sqrt(12) ~= 0.8660 px) ----
			CameraLink::FieldCopy fieldQ = field;
			fieldQ.quantumPx = 1.0;
			CameraModel::PairInput inQ = in;
			inQ.quantumPx = 1.0;
			const float expectedQuantumThr = static_cast<float>(3.0 / std::sqrt(12.0));
			const CameraModel::LabelConstants kQuantum = CameraModel::MakeLabelConstants(recZeroThr, cfg, inQ);
			Assert::AreEqual(expectedQuantumThr, kQuantum.absThr, 1e-5f, L"quantumPx fallback uses 3 * quantumPx / sqrt(12)");

			Assert::IsTrue(prod.Produce(recZeroThr, &fieldQ, t2), L"GPU compute with NVOF quantumPx=1.0");
			const std::vector<float> gotQ = Download(gpu, prod.TextureOf(t2), 120, 68);
			for (int y = 10; y < 14; ++y)
				for (int x = 20; x < 24; ++x)
				{
					const size_t i = static_cast<size_t>(y) * 120 + x;
					Assert::AreEqual(static_cast<int>(CameraModel::Label::Camera), CameraModel::CodeLabel(gotQ[i * 4 + 3]),
						L"0.81 px residual is Camera under NVOF quantumPx=1.0 (0.866 px floor)");
					Assert::AreEqual(static_cast<int>(CameraModel::Label::Object), CameraModel::CodeLabel(gotZero[i * 4 + 3]),
						L"negative control (quantumPx=0): exact same 0.81 px residual is Object");
				}
			for (int y = 10; y < 14; ++y)
				for (int x = 10; x < 14; ++x)
				{
					const size_t i = static_cast<size_t>(y) * 120 + x;
					Assert::AreEqual(static_cast<int>(CameraModel::Label::Object), CameraModel::CodeLabel(gotQ[i * 4 + 3]),
						L"0.95 px residual remains Object under 0.866 px quantum floor");
				}

			// ---- Part C: Real Fitter R2F-raised record: CPU Labels() vs GPU Compute parity + same-input negative control ----
			CameraSynth::GridSpec g16;
			g16.w = 120; g16.h = 68; g16.blockSize = 16.0;
			CameraSynth::Field fR2f(g16);
			fR2f.FromModel(CameraSynth::PanModel(2.0, 0.0));
			fR2f.AddNoise(0.45, 77);   // sigma=0.45 -> MadSigma~0.58 -> R2F raises absThresholdPx above 1.2 px
			CameraModel::PairInput inR2f = fR2f.Input(PairSeconds(), 9);
			inR2f.quantumPx = 1.0;

			CameraModel::Config cfgCpu;
			cfgCpu.cpuLabels = true;
			CameraModel::Fitter fitterCpu(cfgCpu);
			const CameraModel::CameraMotion recCpu = fitterCpu.Fit(inR2f);
			Assert::IsTrue(recCpu.valid && recCpu.blocks != nullptr, L"real R2F CPU fit valid");
			Assert::IsTrue(recCpu.blocks->absThresholdPx > 1.0f, L"R2F genuinely raised absThresholdPx above quantum floor");

			CameraModel::Config cfgGpu = cfgCpu;
			cfgGpu.cpuLabels = false;
			CameraModel::Fitter fitterGpu(cfgGpu);
			const CameraModel::CameraMotion recGpu = fitterGpu.Fit(inR2f);
			Assert::IsTrue(recGpu.valid && recGpu.labelsOnGpu && recGpu.blocks != nullptr, L"real R2F GPU fit valid");
			Assert::AreEqual(recCpu.blocks->absThresholdPx, recGpu.blocks->absThresholdPx, 1e-6f);

			CameraLink::FieldCopy copyR2f = *CameraLinkTest::CopyOf(fR2f, 9);
			copyR2f.quantumPx = 1.0;

			Assert::IsTrue(prod.Produce(recGpu, &copyR2f, t0), L"GPU compute on real R2F-raised record");
			const std::vector<float> gotRealR2f = Download(gpu, prod.TextureOf(t0), 120, 68);

			const CameraModel::LabelConstants kReal = CameraModel::MakeLabelConstants(recGpu, cfgGpu, inR2f);
			std::vector<float> pfReal, paReal, mirrorReal;
			CameraModel::PackLabelInputs(inR2f, *recGpu.blocks, pfReal, paReal);
			CameraModel::LabelGridMirror(kReal, pfReal.data(), paReal.data(), cfgGpu.overlayZeroClusterMin, mirrorReal);
			const std::vector<uint8_t> bandReal = NearThresholdBand(kReal, fR2f, mirrorReal);

			// Negative control on the exact same real Fitter output: drop rec.blocks->absThresholdPx to 0.
			CameraModel::CameraMotion recGpuNoR2f = recGpu;
			auto bdNoR2f = std::make_shared<CameraModel::BlockData>(*recGpu.blocks);
			bdNoR2f->absThresholdPx = 0.0f;
			recGpuNoR2f.blocks = bdNoR2f;
			Assert::IsTrue(prod.Produce(recGpuNoR2f, &copyR2f, t1), L"GPU compute negative control without R2F absThresholdPx");
			const std::vector<float> gotRealNoR2f = Download(gpu, prod.TextureOf(t1), 120, 68);

			size_t mismatchesWithR2f = 0;
			size_t falseObjectsWithoutR2f = 0;
			const size_t total = static_cast<size_t>(120) * 68;
			for (size_t i = 0; i < total; ++i)
			{
				const int cpuLbl = static_cast<int>(recCpu.blocks->label[i]);
				const int gpuLbl = CameraModel::CodeLabel(gotRealR2f[i * 4 + 3]);
				const int noR2fLbl = CameraModel::CodeLabel(gotRealNoR2f[i * 4 + 3]);
				if (gpuLbl != cpuLbl && !bandReal[i])
					++mismatchesWithR2f;
				if (cpuLbl == static_cast<int>(CameraModel::Label::Camera) &&
					noR2fLbl == static_cast<int>(CameraModel::Label::Object))
					++falseObjectsWithoutR2f;
			}
			Assert::AreEqual(size_t(0), mismatchesWithR2f, L"GPU compute matches CPU Labels() on real R2F-raised frame");
			Assert::IsTrue(falseObjectsWithoutR2f > 50,
				L"negative control: dropping rec.blocks->absThresholdPx mislabels >50 real R2F camera blocks as Object");
		}
	};
}
