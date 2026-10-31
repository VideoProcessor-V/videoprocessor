// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Global camera model (Plan 19): consumer accessor and search-hints data API. CPU only.
#include "pch.h"
#include <VP-V/modules/motion/estimation/global_model/CameraMotion.h>
#include <VP-V/modules/motion/estimation/global_model/CameraAccessor.h>
#include <VP-V/modules/motion/estimation/global_model/GlobalModelSearchHints.h>
#include <cstring>
#include <memory>
#include "CppUnitTest.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	namespace CameraTestSupport
	{
		using namespace CameraModel;

		constexpr int kGridW = 48, kGridH = 27;

		// A pan record: every block Camera, model (-20, 0), measured residual 0.
		inline std::shared_ptr<BlockData> PanBlocks()
		{
			auto b = std::make_shared<BlockData>();
			b->gridW = kGridW;
			b->gridH = kGridH;
			b->blockSize = 16;
			const size_t n = static_cast<size_t>(kGridW) * kGridH;
			b->label.assign(n, static_cast<uint8_t>(Label::Camera));
			b->resX.assign(n, 0.0f);
			b->resY.assign(n, 0.0f);
			b->modelX.assign(n, -20.0f);
			b->modelY.assign(n, 0.0f);
			b->objectId.assign(n, 0);
			b->tinyMover.assign(n, 0);
			b->excluded.assign(n, 0);
			b->absThresholdPx = 0.75f;   // the fit's own agreement threshold: max(abs, rel * |model|)
			b->relThreshold = 0.05f;
			return b;
		}

		inline std::shared_ptr<CameraMotion> PanRecord(double confidence, std::shared_ptr<BlockData> blocks = nullptr)
		{
			auto r = std::make_shared<CameraMotion>();
			r->valid = true;
			r->confidence = confidence;
			r->reason = Reason::Ok;
			r->pairSeconds = 1.0 / 24;
			r->blocks = blocks ? blocks : PanBlocks();
			return r;
		}

		inline Policy On()
		{
			Policy p;
			p.enabled = true;
			return p;
		}

		inline bool SameBits(float a, float b)
		{
			return std::memcmp(&a, &b, sizeof(float)) == 0;
		}

		// Drives the weight to its target for a record.
		inline void Settle(BlendState& s, const Policy& p, const CameraMotion* r)
		{
			for (int i = 0; i < 20; ++i) s.Update(p, r);
		}
	}

	TEST_CLASS(CameraAccessorTests)
	{
	public:
		TEST_METHOD(DisabledPolicyReturnsInputsBitIdentical)
		{
			using namespace CameraTestSupport;
			auto rec = PanRecord(0.95);
			Policy on = On();
			BlendState state;
			Settle(state, on, rec.get());
			Assert::IsTrue(state.Weight() == 1.0, L"state has weight");
			Policy off;   // default: disabled
			Assert::IsFalse(off.enabled, L"default policy off");
			const float inputs[][2] = { { 1.5f, -2.25f }, { 0.0f, 0.0f }, { -0.0f, 3.0f }, { 123.456f, 7.0f } };
			for (const auto& in : inputs)
				for (int trusted = 0; trusted < 2; ++trusted)
				{
					float ox = 99, oy = 99;
					const Choice c = FieldVector(off, state, rec.get(), 3, 4, trusted != 0, in[0], in[1], ox, oy);
					Assert::IsTrue(SameBits(ox, in[0]) && SameBits(oy, in[1]), L"bit-identical output");
					Assert::IsTrue(c == (trusted ? Choice::Measured : Choice::Hold), L"choice");
				}
			Assert::AreEqual(0.0, state.Update(off, rec.get()), L"update off returns 0");
			Assert::AreEqual(0.0, state.Weight(), L"weight reset when off");
		}

		TEST_METHOD(NullInvalidOrBlocklessRecordIsIdentity)
		{
			using namespace CameraTestSupport;
			auto good = PanRecord(0.95);
			BlendState state;
			Policy on = On();
			Settle(state, on, good.get());
			auto invalid = PanRecord(0.95);
			invalid->valid = false;
			auto noBlocks = PanRecord(0.95);
			noBlocks->blocks.reset();
			const CameraMotion* records[] = { nullptr, invalid.get(), noBlocks.get() };
			for (const CameraMotion* r : records)
			{
				float ox = 0, oy = 0;
				Choice c = FieldVector(on, state, r, 2, 2, false, 1.25f, -0.5f, ox, oy);
				Assert::IsTrue(c == Choice::Hold && SameBits(ox, 1.25f) && SameBits(oy, -0.5f), L"untrusted unchanged");
				c = FieldVector(on, state, r, 2, 2, true, 1.25f, -0.5f, ox, oy);
				Assert::IsTrue(c == Choice::Measured && SameBits(ox, 1.25f) && SameBits(oy, -0.5f), L"trusted unchanged");
			}
			float ox = 0, oy = 0;
			Assert::IsTrue(FieldVector(on, state, good.get(), -1, 0, false, 1.0f, 1.0f, ox, oy) == Choice::Hold, L"outside grid (negative)");
			Assert::IsTrue(FieldVector(on, state, good.get(), kGridW, 0, false, 1.0f, 1.0f, ox, oy) == Choice::Hold, L"outside grid (x)");
			Assert::IsTrue(FieldVector(on, state, good.get(), 0, kGridH, false, 1.0f, 1.0f, ox, oy) == Choice::Hold, L"outside grid (y)");
			Assert::IsTrue(ox == 1.0f && oy == 1.0f, L"passthrough outside grid");
			Assert::IsFalse(ModelUsable(Policy(), good.get()), L"not usable when off");
			Assert::IsTrue(ModelUsable(on, good.get()), L"usable");
			Assert::IsFalse(ModelUsable(on, noBlocks.get()), L"not usable without blocks");
		}

		TEST_METHOD(ConfidentCameraBlockUntrustedGetsModelAtFullWeight)
		{
			using namespace CameraTestSupport;
			auto rec = PanRecord(0.95);
			Policy on = On();
			BlendState state;
			float ox = 0, oy = 0;
			// Weight 0 initially: today's behaviour.
			Assert::IsTrue(FieldVector(on, state, rec.get(), 5, 5, false, 1.0f, 2.0f, ox, oy) == Choice::Hold, L"weight 0 holds");
			Settle(state, on, rec.get());
			Assert::AreEqual(1.0, state.Weight(), L"weight reached 1");
			const Choice c = FieldVector(on, state, rec.get(), 5, 5, false, 1.0f, 2.0f, ox, oy);
			Assert::IsTrue(c == Choice::Model, L"model chosen");
			Assert::IsTrue(ox == -20.0f && oy == 0.0f, L"model vector");
		}

		TEST_METHOD(WeightRisesMonotonicallyAndIsRateLimited)
		{
			using namespace CameraTestSupport;
			Policy on = On();
			BlendState state;
			auto low = PanRecord(0.2);
			auto high = PanRecord(0.95);
			state.Update(on, low.get());
			Assert::AreEqual(0.0, state.Weight(), L"low confidence weight 0");
			double prev = state.Weight();
			bool reachedOne = false;
			for (int i = 0; i < 12; ++i)
			{
				const double w = state.Update(on, high.get());
				Assert::IsTrue(w >= prev, L"monotonic rise");
				Assert::IsTrue(w - prev <= on.rateLimit + 1e-12, L"rate limited");
				prev = w;
				if (w == 1.0) reachedOne = true;
			}
			Assert::IsTrue(reachedOne, L"reaches 1");
			// And falling: confidence moves to just inside the ramp, no pop.
			auto mid = PanRecord(0.75);
			const double before = state.Weight();
			const double after = state.Update(on, mid.get());
			Assert::IsTrue(before - after <= on.rateLimit + 1e-12 && after <= before, L"falls at most rateLimit");
		}

		TEST_METHOD(ConfidenceBelowFloorDropsWeightImmediately)
		{
			using namespace CameraTestSupport;
			Policy on = On();
			BlendState state;
			auto high = PanRecord(0.95);
			Settle(state, on, high.get());
			Assert::AreEqual(1.0, state.Weight(), L"full weight");
			auto low = PanRecord(0.4);
			Assert::AreEqual(0.0, state.Update(on, low.get()), L"hard fallback in one update");
		}

		TEST_METHOD(HysteresisKeepsTheModelJustBelowThreshold)
		{
			using namespace CameraTestSupport;
			Policy on = On();
			auto high = PanRecord(0.95);
			// Below the threshold but within the hysteresis: stays in use.
			{
				BlendState state;
				Settle(state, on, high.get());
				auto slightlyUnder = PanRecord(on.threshold - 0.5 * on.hysteresis);
				Settle(state, on, slightlyUnder.get());
				Assert::IsTrue(state.Weight() > 0.0, L"model kept inside the hysteresis band");
			}
			// Below threshold - hysteresis: dropped.
			{
				BlendState state;
				Settle(state, on, high.get());
				auto under = PanRecord(on.threshold - on.hysteresis - 0.02);
				Settle(state, on, under.get());
				Assert::AreEqual(0.0, state.Weight(), L"model dropped below the hysteresis band");
			}
			// Never used: slightly under the threshold does not start it.
			{
				BlendState state;
				auto slightlyUnder = PanRecord(on.threshold - 0.5 * on.hysteresis);
				Settle(state, on, slightlyUnder.get());
				Assert::AreEqual(0.0, state.Weight(), L"not started under the threshold");
			}
		}

		TEST_METHOD(ObjectLabelledUntrustedBlockNeverGetsTheModel)
		{
			using namespace CameraTestSupport;
			auto blocks = PanBlocks();
			const size_t i = static_cast<size_t>(3) * kGridW + 7;
			blocks->label[i] = static_cast<uint8_t>(Label::Object);
			blocks->label[i + 1] = static_cast<uint8_t>(Label::Overlay);
			blocks->label[i + 2] = static_cast<uint8_t>(Label::Unknown);
			auto rec = PanRecord(0.95, blocks);
			Policy on = On();
			BlendState state;
			Settle(state, on, rec.get());
			for (int k = 0; k < 3; ++k)
			{
				float ox = 0, oy = 0;
				const Choice c = FieldVector(on, state, rec.get(), 7 + k, 3, false, 0.5f, 0.25f, ox, oy);
				Assert::IsTrue(c == Choice::Hold && ox == 0.5f && oy == 0.25f, L"non-camera label keeps the fallback");
			}
		}

		TEST_METHOD(TrustedMeasuredVectorUnchangedUnlessPreferredAndAgreeing)
		{
			using namespace CameraTestSupport;
			auto blocks = PanBlocks();
			// Block (1,0) disagrees: residual 8 px.
			blocks->resX[1] = 8.0f;
			// Block (2,0) is an Object block with zero residual.
			blocks->label[2] = static_cast<uint8_t>(Label::Object);
			auto rec = PanRecord(0.95, blocks);
			Policy on = On();
			BlendState state;
			Settle(state, on, rec.get());
			float ox = 0, oy = 0;
			// Not preferred: measured stays.
			Assert::IsTrue(FieldVector(on, state, rec.get(), 0, 0, true, -19.5f, 0.25f, ox, oy) == Choice::Measured, L"measured");
			Assert::IsTrue(ox == -19.5f && oy == 0.25f, L"measured unchanged");
			on.preferModelWhenAgrees = true;
			Assert::IsTrue(FieldVector(on, state, rec.get(), 0, 0, true, -19.5f, 0.25f, ox, oy) == Choice::Model, L"agreeing -> model");
			Assert::IsTrue(ox == -20.0f && oy == 0.0f, L"model vector");
			Assert::IsTrue(FieldVector(on, state, rec.get(), 1, 0, true, -12.0f, 0.0f, ox, oy) == Choice::Measured, L"disagreeing stays measured");
			Assert::IsTrue(ox == -12.0f && oy == 0.0f, L"disagreeing unchanged");
			Assert::IsTrue(FieldVector(on, state, rec.get(), 2, 0, true, -20.0f, 0.0f, ox, oy) == Choice::Measured, L"object stays measured");
		}

		// The agreement test uses the fit's own stored threshold max(abs, rel * |model|), not constants.
		TEST_METHOD(AgreementUsesTheStoredThreshold)
		{
			using namespace CameraTestSupport;
			auto tight = PanBlocks();
			tight->absThresholdPx = 0.2f;
			tight->relThreshold = 0.0f;
			tight->resX[0] = 0.5f;   // |res| 0.5
			auto loose = PanBlocks();
			loose->absThresholdPx = 0.1f;
			loose->relThreshold = 0.1f;   // 0.1 * |20| = 2.0
			loose->resX[0] = 0.5f;
			Policy on = On();
			on.preferModelWhenAgrees = true;
			float ox = 0, oy = 0;
			auto r1 = PanRecord(0.95, tight);
			BlendState s1;
			Settle(s1, on, r1.get());
			Assert::IsTrue(FieldVector(on, s1, r1.get(), 0, 0, true, -19.5f, 0.0f, ox, oy) == Choice::Measured, L"tight threshold: disagrees");
			auto r2 = PanRecord(0.95, loose);
			BlendState s2;
			Settle(s2, on, r2.get());
			Assert::IsTrue(FieldVector(on, s2, r2.get(), 0, 0, true, -19.5f, 0.0f, ox, oy) == Choice::Model, L"relative term: agrees");
		}

		TEST_METHOD(ReasonsCutFlashDeadlineGiveTargetZero)
		{
			using namespace CameraTestSupport;
			Policy on = On();
			const Reason reasons[] = { Reason::Cut, Reason::Flash, Reason::DeadlineMissed, Reason::NoInput,
				Reason::GenerationReset, Reason::Disabled };
			for (Reason reason : reasons)
			{
				BlendState state;
				auto good = PanRecord(0.95);
				Settle(state, on, good.get());
				Assert::AreEqual(1.0, state.Weight(), L"full weight first");
				auto bad = PanRecord(0.95);
				bad->reason = reason;
				const double w1 = state.Update(on, bad.get());
				Assert::IsTrue(w1 < 1.0 && 1.0 - w1 <= on.rateLimit + 1e-12, L"first update falls by at most rateLimit");
				Settle(state, on, bad.get());
				Assert::AreEqual(0.0, state.Weight(), L"target 0 for the reason");
			}
			// Ok-like reasons (LowConfidence is judged by confidence, not forced).
			BlendState state;
			auto seeded = PanRecord(0.95);
			seeded->reason = Reason::PostCutSeeded;
			Settle(state, on, seeded.get());
			Assert::AreEqual(1.0, state.Weight(), L"seeded reason allowed");
		}

		TEST_METHOD(BlendIsHalfwayAtHalfWeight)
		{
			using namespace CameraTestSupport;
			Policy on = On();
			on.rateLimit = 0.5;
			auto rec = PanRecord(0.95);
			BlendState state;
			Assert::AreEqual(0.5, state.Update(on, rec.get()), L"one step of 0.5");
			float ox = 0, oy = 0;
			const Choice c = FieldVector(on, state, rec.get(), 4, 4, false, 10.0f, 4.0f, ox, oy);
			Assert::IsTrue(c == Choice::Blend, L"blend");
			Assert::AreEqual(0.5f * 10.0f + 0.5f * -20.0f, ox, L"x halfway");
			Assert::AreEqual(0.5f * 4.0f, oy, L"y halfway");
		}

		TEST_METHOD(FieldVectorIsAPureFunction)
		{
			using namespace CameraTestSupport;
			Policy on = On();
			on.preferModelWhenAgrees = true;
			on.rateLimit = 0.3;
			auto rec = PanRecord(0.95);
			BlendState state;
			state.Update(on, rec.get());
			for (int trusted = 0; trusted < 2; ++trusted)
			{
				float ax = 0, ay = 0, bx = 1, by = 1;
				const Choice a = FieldVector(on, state, rec.get(), 9, 9, trusted != 0, 3.0f, -1.0f, ax, ay);
				const Choice b = FieldVector(on, state, rec.get(), 9, 9, trusted != 0, 3.0f, -1.0f, bx, by);
				Assert::IsTrue(a == b && SameBits(ax, bx) && SameBits(ay, by), L"same inputs, same outputs");
			}
			Assert::IsTrue(state.Weight() == 0.3, L"FieldVector does not change the state");
		}

		TEST_METHOD(ResetClearsTheWeight)
		{
			using namespace CameraTestSupport;
			Policy on = On();
			auto rec = PanRecord(0.95);
			BlendState state;
			Settle(state, on, rec.get());
			state.Reset();
			Assert::AreEqual(0.0, state.Weight(), L"reset");
		}
	};

	TEST_CLASS(CameraHintsTests)
	{
	public:
		static CameraModel::SearchHints Build(const CameraModel::HintConfig& cfg, const CameraModel::CameraMotion* prev,
			const float* fx, const float* fy, const float* nx, const float* ny,
			bool cut = false, bool fade = false, bool flash = false, double precision = 0.5, double nextSeconds = 0.0)
		{
			return CameraModel::BuildHints(cfg, prev, fx, fy, nx, ny, cut, fade, flash, 0.8, 0.02, precision, nextSeconds);
		}

		struct Field
		{
			std::vector<float> x, y;
			Field() : x(static_cast<size_t>(CameraTestSupport::kGridW) * CameraTestSupport::kGridH, -20.0f),
				y(x.size(), 0.0f) {}
		};

		TEST_METHOD(PanRecordGivesNarrowEverywhere)
		{
			using namespace CameraTestSupport;
			auto rec = PanRecord(0.95);
			Field f;
			const CameraModel::SearchHints h = Build(CameraModel::HintConfig(), rec.get(), f.x.data(), f.y.data(), f.x.data(), f.y.data());
			const uint32_t n = kGridW * kGridH;
			Assert::AreEqual(n, h.narrowBlocks, L"all narrow");
			Assert::AreEqual(0u, h.fullBlocks, L"no full");
			Assert::AreEqual(4.0 * 0.5, h.narrowRadiusPx, L"radius = 4 sigma");
			Assert::IsTrue(h.gridW == kGridW && h.gridH == kGridH && h.hint.size() == n, L"grid");
			Assert::IsTrue(h.seedX[10] == -20.0f && h.seedY[10] == 0.0f, L"seed");
			Assert::IsTrue(h.scene == CameraModel::SceneAction::Normal, L"scene normal");
			Assert::IsTrue(h.fadeGain == 0.8 && h.fadeOffset == 0.02, L"fade values copied");
		}

		TEST_METHOD(FiveSigmaDisagreementIsFullSearch)
		{
			using namespace CameraTestSupport;
			auto rec = PanRecord(0.95);
			Field f, nv;
			const size_t i = static_cast<size_t>(5) * kGridW + 9;
			nv.x[i] += 5.0f * 0.5f;
			const CameraModel::SearchHints h = Build(CameraModel::HintConfig(), rec.get(), f.x.data(), f.y.data(), nv.x.data(), nv.y.data());
			Assert::IsTrue(h.hint[i] == static_cast<uint8_t>(CameraModel::BlockHint::FullSearch), L"disagreeing block");
			Assert::AreEqual(1u, h.fullBlocks, L"one full");
			Assert::AreEqual(static_cast<uint32_t>(kGridW * kGridH - 1), h.narrowBlocks, L"rest narrow");
		}

		TEST_METHOD(TinyMoverBlocksGiveFinerGridAndRois)
		{
			using namespace CameraTestSupport;
			using namespace CameraModel;
			auto blocks = PanBlocks();
			auto flag = [&](int bx, int by)
			{
				const size_t i = static_cast<size_t>(by) * kGridW + bx;
				blocks->label[i] = static_cast<uint8_t>(Label::Object);
				blocks->tinyMover[i] = 1;
			};
			// One alone; a pair side by side; one far away (raster order by first block).
			flag(5, 2);
			flag(20, 10); flag(21, 10);
			flag(40, 20);
			auto rec = PanRecord(0.95, blocks);
			Field f;
			const SearchHints h = Build(HintConfig(), rec.get(), f.x.data(), f.y.data(), f.x.data(), f.y.data());
			Assert::AreEqual(4u, h.finerBlocks, L"finer count");
			Assert::AreEqual(static_cast<size_t>(3), h.rois.size(), L"three ROIs");
			Assert::IsTrue(h.rois[0].bx0 == 5 && h.rois[0].by0 == 2 && h.rois[0].bx1 == 6 && h.rois[0].by1 == 3, L"roi 0");
			Assert::IsTrue(h.rois[1].bx0 == 20 && h.rois[1].by0 == 10 && h.rois[1].bx1 == 22 && h.rois[1].by1 == 11, L"roi 1 spans two blocks");
			Assert::IsTrue(h.rois[2].bx0 == 40 && h.rois[2].by0 == 20 && h.rois[2].bx1 == 41 && h.rois[2].by1 == 21, L"roi 2");
			// The single-flag case alone gives exactly one ROI.
			auto one = PanBlocks();
			one->label[static_cast<size_t>(7) * kGridW + 7] = static_cast<uint8_t>(Label::Object);
			one->tinyMover[static_cast<size_t>(7) * kGridW + 7] = 1;
			auto rec1 = PanRecord(0.95, one);
			const SearchHints h1 = Build(HintConfig(), rec1.get(), f.x.data(), f.y.data(), f.x.data(), f.y.data());
			Assert::AreEqual(static_cast<size_t>(1), h1.rois.size(), L"one ROI");
			// Switch off: no finer grid, no ROIs.
			HintConfig cfg;
			cfg.tinyMoverRoi = false;
			const SearchHints h2 = Build(cfg, rec1.get(), f.x.data(), f.y.data(), f.x.data(), f.y.data());
			Assert::IsTrue(h2.rois.empty() && h2.finerBlocks == 0, L"tinyMoverRoi off");
		}

		TEST_METHOD(OverlayBlocksAreExcluded)
		{
			using namespace CameraTestSupport;
			using namespace CameraModel;
			auto blocks = PanBlocks();
			for (int bx = 0; bx < kGridW; ++bx)
			{
				blocks->label[bx] = static_cast<uint8_t>(Label::Overlay);
				blocks->excluded[bx] = 1;   // bars / text guard
			}
			auto rec = PanRecord(0.95, blocks);
			Field f;
			const SearchHints h = Build(HintConfig(), rec.get(), f.x.data(), f.y.data(), f.x.data(), f.y.data());
			Assert::IsTrue(h.hint[0] == static_cast<uint8_t>(BlockHint::Excluded), L"excluded");
			Assert::AreEqual(static_cast<uint32_t>(kGridW * (kGridH - 1)), h.narrowBlocks, L"others narrow");
			Assert::AreEqual(0u, h.fullBlocks, L"excluded counted nowhere");
		}

		// A static logo (zero-motion-cluster Overlay, NOT bars / text guard) must stay searchable.
		TEST_METHOD(StaticLogoOverlayStaysSearchable)
		{
			using namespace CameraTestSupport;
			using namespace CameraModel;
			auto blocks = PanBlocks();
			blocks->label[5] = static_cast<uint8_t>(Label::Overlay);   // logo block, excluded stays 0
			blocks->label[6] = static_cast<uint8_t>(Label::Overlay);
			blocks->excluded[6] = 1;                                   // a text-guard block next to it
			auto rec = PanRecord(0.95, blocks);
			Field f;
			const SearchHints h = Build(HintConfig(), rec.get(), f.x.data(), f.y.data(), f.x.data(), f.y.data());
			Assert::IsTrue(h.hint[5] == static_cast<uint8_t>(BlockHint::FullSearch), L"logo searchable");
			Assert::IsTrue(h.hint[6] == static_cast<uint8_t>(BlockHint::Excluded), L"text guard excluded");
		}

		// 3:2 pulldown: the next pair lasts 2.5x as long, so the seeds scale and still agree with the engine.
		TEST_METHOD(SeedsAreTimeScaledForAnUnevenNextPair)
		{
			using namespace CameraTestSupport;
			using namespace CameraModel;
			auto rec = PanRecord(0.95);   // pairSeconds 1/24, model (-20, 0)
			const size_t n = static_cast<size_t>(kGridW) * kGridH;
			std::vector<float> nvx(n, -50.0f), nvy(n, 0.0f);   // the engine measured the longer pair
			std::vector<float> fx(n, -20.0f), fy(n, 0.0f);     // previous pair's forward vectors
			const SearchHints scaled = Build(HintConfig(), rec.get(), fx.data(), fy.data(), nvx.data(), nvy.data(),
				false, false, false, 0.5, 2.5 / 24);
			Assert::AreEqual(static_cast<uint32_t>(n), scaled.narrowBlocks, L"scaled seeds agree");
			Assert::AreEqual(-50.0, static_cast<double>(scaled.seedX[0]), 1e-4, L"seed scaled");
			const SearchHints plain = Build(HintConfig(), rec.get(), fx.data(), fy.data(), nvx.data(), nvy.data(),
				false, false, false, 0.5, 0.0);
			Assert::AreEqual(static_cast<uint32_t>(n), plain.fullBlocks, L"unscaled seeds disagree -> full search");
			// The model-only path (no engine vectors) scales too.
			const SearchHints modelOnly = Build(HintConfig(), rec.get(), nullptr, nullptr, nullptr, nullptr,
				false, false, false, 0.5, 2.5 / 24);
			Assert::AreEqual(-50.0, static_cast<double>(modelOnly.seedX[0]), 1e-4, L"model seed scaled");
		}

		TEST_METHOD(SceneActionPrecedence)
		{
			using namespace CameraModel;
			HintConfig cfg;
			Assert::IsTrue(Build(cfg, nullptr, nullptr, nullptr, nullptr, nullptr, true, true, true).scene == SceneAction::SkipAcrossCut, L"cut first");
			Assert::IsTrue(Build(cfg, nullptr, nullptr, nullptr, nullptr, nullptr, false, true, true).scene == SceneAction::BridgeFlash, L"flash over fade");
			Assert::IsTrue(Build(cfg, nullptr, nullptr, nullptr, nullptr, nullptr, false, true, false).scene == SceneAction::FadeNormalised, L"fade");
			Assert::IsTrue(Build(cfg, nullptr, nullptr, nullptr, nullptr, nullptr).scene == SceneAction::Normal, L"normal");
			cfg.cutSkip = false;
			Assert::IsTrue(Build(cfg, nullptr, nullptr, nullptr, nullptr, nullptr, true, false, false).scene == SceneAction::Normal, L"cutSkip off");
			cfg = HintConfig();
			cfg.flashBridge = false;
			Assert::IsTrue(Build(cfg, nullptr, nullptr, nullptr, nullptr, nullptr, false, false, true).scene == SceneAction::Normal, L"flashBridge off");
			cfg = HintConfig();
			cfg.fadeNormalised = false;
			Assert::IsTrue(Build(cfg, nullptr, nullptr, nullptr, nullptr, nullptr, false, true, false).scene == SceneAction::Normal, L"fade off");
		}

		TEST_METHOD(NullPrevGivesEmptyHintsWithSceneSet)
		{
			using namespace CameraModel;
			const SearchHints h = Build(HintConfig(), nullptr, nullptr, nullptr, nullptr, nullptr, true);
			Assert::IsTrue(h.scene == SceneAction::SkipAcrossCut, L"scene still set");
			Assert::IsTrue(h.hint.empty() && h.gridW == 0 && h.gridH == 0, L"empty");
			Assert::IsTrue(h.fullBlocks == 0 && h.narrowBlocks == 0 && h.finerBlocks == 0, L"counts zero");
			// A record without blocks is empty too.
			auto rec = CameraTestSupport::PanRecord(0.95);
			rec->blocks.reset();
			Assert::IsTrue(Build(HintConfig(), rec.get(), nullptr, nullptr, nullptr, nullptr).hint.empty(), L"no blocks, empty");
		}

		TEST_METHOD(InvalidPrevWithBlocksIsAllFullSearch)
		{
			using namespace CameraTestSupport;
			using namespace CameraModel;
			auto rec = PanRecord(0.95);
			rec->valid = false;
			Field f;
			const SearchHints h = Build(HintConfig(), rec.get(), f.x.data(), f.y.data(), f.x.data(), f.y.data());
			Assert::AreEqual(static_cast<size_t>(kGridW) * kGridH, h.hint.size(), L"grid kept");
			Assert::AreEqual(static_cast<uint32_t>(kGridW * kGridH), h.fullBlocks, L"all full");
			Assert::AreEqual(0u, h.narrowBlocks, L"no narrow");
		}

		TEST_METHOD(NullNvofAgreesWithTheModelVectorInstead)
		{
			using namespace CameraTestSupport;
			using namespace CameraModel;
			auto rec = PanRecord(0.95);
			Field f;   // seed = model: agrees
			SearchHints h = Build(HintConfig(), rec.get(), f.x.data(), f.y.data(), nullptr, nullptr);
			Assert::AreEqual(static_cast<uint32_t>(kGridW * kGridH), h.narrowBlocks, L"agrees with model -> narrow");
			// A temporal seed 5 sigma away from the model is rejected.
			const size_t i = static_cast<size_t>(2) * kGridW + 3;
			f.x[i] += 5.0f * 0.5f;
			h = Build(HintConfig(), rec.get(), f.x.data(), f.y.data(), nullptr, nullptr);
			Assert::IsTrue(h.hint[i] == static_cast<uint8_t>(BlockHint::FullSearch), L"disagrees with model -> full");
			Assert::AreEqual(1u, h.fullBlocks, L"only that block");
		}

		TEST_METHOD(LowConfidenceIsAllFullSearch)
		{
			using namespace CameraTestSupport;
			using namespace CameraModel;
			auto rec = PanRecord(0.6);
			Field f;
			const SearchHints h = Build(HintConfig(), rec.get(), f.x.data(), f.y.data(), f.x.data(), f.y.data());
			Assert::AreEqual(static_cast<uint32_t>(kGridW * kGridH), h.fullBlocks, L"all full");
			Assert::AreEqual(0u, h.narrowBlocks, L"no narrow");
		}

		TEST_METHOD(TemporalSeedOffUsesTheModelVector)
		{
			using namespace CameraTestSupport;
			using namespace CameraModel;
			auto rec = PanRecord(0.95);
			Field f, nv;
			// Previous forward vectors are wild, the engine agrees with the model.
			for (float& v : f.x) v = 100.0f;
			HintConfig cfg;
			SearchHints h = Build(cfg, rec.get(), f.x.data(), f.y.data(), nv.x.data(), nv.y.data());
			Assert::AreEqual(0u, h.narrowBlocks, L"wild temporal seed rejected");
			cfg.temporalSeed = false;
			h = Build(cfg, rec.get(), f.x.data(), f.y.data(), nv.x.data(), nv.y.data());
			Assert::AreEqual(static_cast<uint32_t>(kGridW * kGridH), h.narrowBlocks, L"model vector used as the seed");
			Assert::IsTrue(h.seedX[0] == -20.0f, L"seed is the model");
		}

		TEST_METHOD(ObjectBlocksNeedAKnownObject)
		{
			using namespace CameraTestSupport;
			using namespace CameraModel;
			auto blocks = PanBlocks();
			blocks->label[0] = static_cast<uint8_t>(Label::Object);
			blocks->objectId[0] = 3;
			blocks->label[1] = static_cast<uint8_t>(Label::Object);
			blocks->objectId[1] = 4;
			auto rec = PanRecord(0.95, blocks);
			ObjectModel obj;
			obj.id = 3;
			rec->objects.push_back(obj);
			Field f;
			const SearchHints h = Build(HintConfig(), rec.get(), f.x.data(), f.y.data(), f.x.data(), f.y.data());
			Assert::IsTrue(h.hint[0] == static_cast<uint8_t>(BlockHint::NarrowAroundPrediction), L"known object narrow");
			Assert::IsTrue(h.hint[1] == static_cast<uint8_t>(BlockHint::FullSearch), L"unknown object full");
		}

		TEST_METHOD(DefaultPrecisionScalesWithTheBlockSize)
		{
			using namespace CameraTestSupport;
			using namespace CameraModel;
			auto rec = PanRecord(0.95);
			Field f;
			const SearchHints h = BuildHints(HintConfig(), rec.get(), f.x.data(), f.y.data(), f.x.data(), f.y.data(),
				false, false, false, 1.0, 0.0, 0.0);
			Assert::AreEqual(4.0 * 0.25, h.narrowRadiusPx, L"default precision 0.25 for 16 px blocks");
		}
	};
}
