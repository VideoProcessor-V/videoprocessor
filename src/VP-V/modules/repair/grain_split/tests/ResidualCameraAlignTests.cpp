// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

#include <VP-V/modules/repair/grain_split/ResidualCameraAlign.h>

#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	namespace
	{
		// Deterministic high-frequency residual texture so alignment errors show up clearly.
		float PatternAt(double x, double y)
		{
			return static_cast<float>(0.5 * std::sin(0.37 * x + 0.19 * y) + 0.5 * std::cos(0.29 * y - 0.13 * x));
		}

		CameraModel::CameraMotion MakeZoomRecord(int w, int h, int blockSize, double zoomScale, double tx, double ty, double conf)
		{
			CameraModel::CameraMotion m;
			m.seq = 10;
			m.valid = true;
			m.reason = CameraModel::Reason::Ok;
			m.confidence = conf;
			m.picture = { 0, 0, w, h };
			m.params.type = CameraModel::ModelType::Similarity;
			m.params.h[0] = zoomScale; m.params.h[1] = 0.0;       m.params.h[2] = tx;
			m.params.h[3] = 0.0;       m.params.h[4] = zoomScale; m.params.h[5] = ty;
			m.params.h[6] = 0.0;       m.params.h[7] = 0.0;       m.params.h[8] = 1.0;
			auto bd = std::make_shared<CameraModel::BlockData>();
			bd->gridW = w / blockSize;
			bd->gridH = h / blockSize;
			bd->blockSize = static_cast<double>(blockSize);
			bd->label.assign(static_cast<size_t>(bd->gridW) * static_cast<size_t>(bd->gridH),
				static_cast<uint8_t>(CameraModel::Label::Camera));
			m.blocks = bd;
			return m;
		}
	}

	TEST_CLASS(ResidualCameraAlignTests)
	{
	public:
		TEST_METHOD(OffSwitchIsByteIdenticalAndEmitsNoGlsl)
		{
			Assert::AreEqual(std::string(), ResidualCameraAlign::GlslText(false), L"off emits no GLSL text or define");
			const std::string onGlsl = ResidualCameraAlign::GlslText(true);
			Assert::IsTrue(onGlsl.find("VPU_RESIDUAL_CAMERA_ALIGN_VERSION") != std::string::npos);
			Assert::IsTrue(onGlsl.find("rca_prev_pixel") != std::string::npos);

			const int w = 64, h = 64, bs = 16;
			ResidualCameraAlign::Plane prev, cur, outOff, outAbsent;
			prev.Resize(w, h); cur.Resize(w, h);
			for (int y = 0; y < h; ++y)
				for (int x = 0; x < w; ++x)
				{
					prev.data[size_t(y) * w + x] = PatternAt(x + 0.5, y + 0.5);
					cur.data[size_t(y) * w + x] = PatternAt(x + 0.5 - 2.0, y + 0.5);
				}
			ResidualCameraAlign::BlockField bf;
			bf.gridW = w / bs; bf.gridH = h / bs; bf.blockSize = bs;
			bf.vx.assign(size_t(bf.gridW) * bf.gridH, 2.0f);
			bf.vy.assign(size_t(bf.gridW) * bf.gridH, 0.0f);
			bf.trusted.assign(size_t(bf.gridW) * bf.gridH, 1);

			CameraModel::CameraMotion rec = MakeZoomRecord(w, h, bs, 1.08, 3.0, -1.0, 0.95);
			CameraModel::Policy pol = CameraModel::PolicyFor(CameraModel::Consumer::ResidualStabilise);
			Assert::IsFalse(pol.enabled, L"ResidualStabilise preset defaults to OFF");
			CameraModel::BlendState st;
			st.Update(pol, &rec);

			ResidualCameraAlign::Settings s;
			s.alpha = 0.25f;
			const auto rOff = ResidualCameraAlign::AlignResidualFrame(pol, st, &rec, true, prev, cur, bf, s, outOff);
			const auto rNone = ResidualCameraAlign::AlignResidualFrame(pol, st, nullptr, false, prev, cur, bf, s, outAbsent);
			Assert::IsFalse(rOff.cameraWanted);
			Assert::IsFalse(rOff.cameraLate);
			Assert::AreEqual(int(ResidualCameraAlign::Variant::Uncompensated), int(rOff.variant));
			Assert::AreEqual(uint64_t(0), rOff.cameraPixels);
			Assert::AreEqual(0, std::memcmp(outOff.data.data(), outAbsent.data.data(), outOff.data.size() * sizeof(float)),
				L"off with record is byte-identical to absent P8");
		}

		TEST_METHOD(AbsentOrLateP8IsByteIdenticalAndFlagsCameraLate)
		{
			const int w = 64, h = 64, bs = 16;
			ResidualCameraAlign::Plane prev, cur, outRef, outLate;
			prev.Resize(w, h); cur.Resize(w, h);
			for (int y = 0; y < h; ++y)
				for (int x = 0; x < w; ++x)
				{
					prev.data[size_t(y) * w + x] = PatternAt(x + 0.5, y + 0.5);
					cur.data[size_t(y) * w + x] = PatternAt(x + 0.5, y + 0.5);
				}
			ResidualCameraAlign::BlockField bf;
			bf.gridW = w / bs; bf.gridH = h / bs; bf.blockSize = bs;
			bf.vx.assign(size_t(bf.gridW) * bf.gridH, 1.0f);
			bf.vy.assign(size_t(bf.gridW) * bf.gridH, -0.5f);
			bf.trusted.assign(size_t(bf.gridW) * bf.gridH, 1);

			CameraModel::Policy offPol = CameraModel::PolicyFor(CameraModel::Consumer::ResidualStabilise);
			CameraModel::Policy onPol = offPol;
			onPol.enabled = true;
			CameraModel::BlendState stOff, stOn;
			CameraModel::CameraMotion rec = MakeZoomRecord(w, h, bs, 1.05, 0.0, 0.0, 0.95);
			for (int i = 0; i < 6; ++i)
				stOn.Update(onPol, &rec);

			ResidualCameraAlign::Settings s;
			s.alpha = 0.2f;
			ResidualCameraAlign::AlignResidualFrame(offPol, stOff, nullptr, false, prev, cur, bf, s, outRef);
			const auto rLate = ResidualCameraAlign::AlignResidualFrame(onPol, stOn, &rec, false, prev, cur, bf, s, outLate);
			Assert::IsTrue(rLate.cameraWanted);
			Assert::IsTrue(rLate.cameraLate, L"late/absent P8 is visibly flagged");
			Assert::AreEqual(int(ResidualCameraAlign::Variant::Uncompensated), int(rLate.variant));
			Assert::AreEqual(0, std::memcmp(outRef.data.data(), outLate.data.data(), outRef.data.size() * sizeof(float)),
				L"absent P8 produces byte-identical output to uncompensated alignment");
		}

		TEST_METHOD(ZoomAlignmentTracksEveryPixelExactlyAndOutperformsBlockTranslation)
		{
			// Synthetic 1.15x zoom centred on a 128x128 frame: cur(p) = prev(H^{-1}(p)).
			// Block-constant translation (measured block-centre vector) has radial error across each 16x16 block;
			// ResidualCameraAlign aligns every pixel with exact H^{-1}(p), so alpha=0 reproduces cur accurately.
			const int w = 128, h = 128, bs = 16;
			const double zoom = 1.15;
			CameraModel::CameraMotion rec = MakeZoomRecord(w, h, bs, zoom, 0.0, 0.0, 0.96);
			const auto inv = ResidualCameraAlign::InvertModel(rec.params);
			Assert::IsTrue(inv.valid);

			ResidualCameraAlign::Plane prev, cur, outBlock, outCam;
			prev.Resize(w, h); cur.Resize(w, h);
			for (int y = 0; y < h; ++y)
				for (int x = 0; x < w; ++x)
				{
					const double fx = x + 0.5, fy = y + 0.5;
					prev.data[size_t(y) * w + x] = PatternAt(fx, fy);
					double px = 0.0, py = 0.0;
					Assert::IsTrue(ResidualCameraAlign::PrevPixelExact(inv, rec.picture, fx, fy, px, py));
					cur.data[size_t(y) * w + x] = PatternAt(px, py);
				}

			// Block-constant field: exact forward vector at each block centre, constant across the 16x16 block.
			ResidualCameraAlign::BlockField bf;
			bf.gridW = w / bs; bf.gridH = h / bs; bf.blockSize = bs;
			bf.vx.resize(size_t(bf.gridW) * bf.gridH);
			bf.vy.resize(size_t(bf.gridW) * bf.gridH);
			bf.trusted.assign(size_t(bf.gridW) * bf.gridH, 1);
			for (int by = 0; by < bf.gridH; ++by)
				for (int bx = 0; bx < bf.gridW; ++bx)
				{
					double cx = 0.0, cy = 0.0, dx = 0.0, dy = 0.0;
					CameraModel::BlockCentre(bx, by, bs, rec.picture, &cx, &cy);
					CameraModel::Displacement(rec.params, cx, cy, &dx, &dy);
					bf.vx[size_t(by) * bf.gridW + bx] = static_cast<float>(dx);
					bf.vy[size_t(by) * bf.gridW + bx] = static_cast<float>(dy);
				}

			CameraModel::Policy offPol = CameraModel::PolicyFor(CameraModel::Consumer::ResidualStabilise);
			CameraModel::Policy onPol = offPol;
			onPol.enabled = true;
			CameraModel::BlendState stOff, stOn;
			for (int i = 0; i < 6; ++i)
				stOn.Update(onPol, &rec);
			Assert::AreEqual(1.0, stOn.Weight(), 1e-9);

			ResidualCameraAlign::Settings s;
			s.alpha = 0.0f;               // pure warped previous residual vs true current residual
			s.grainGuardNearest = false;  // bilinear to isolate geometric alignment error
			ResidualCameraAlign::AlignResidualFrame(offPol, stOff, &rec, true, prev, cur, bf, s, outBlock);
			const auto rCam = ResidualCameraAlign::AlignResidualFrame(onPol, stOn, &rec, true, prev, cur, bf, s, outCam);
			Assert::AreEqual(int(ResidualCameraAlign::Variant::Camera), int(rCam.variant));
			Assert::AreEqual(uint64_t(w) * uint64_t(h), rCam.cameraPixels);

			// Measure interior RMS error (excluding outer border clamped taps).
			double errBlock = 0.0, errCam = 0.0;
			int count = 0;
			for (int y = 16; y < h - 16; ++y)
				for (int x = 16; x < w - 16; ++x)
				{
					const size_t i = size_t(y) * w + x;
					const double db = outBlock.data[i] - cur.data[i];
					const double dc = outCam.data[i] - cur.data[i];
					errBlock += db * db;
					errCam += dc * dc;
					++count;
				}
			const double rmsBlock = std::sqrt(errBlock / count);
			const double rmsCam = std::sqrt(errCam / count);
			Assert::IsTrue(rmsCam < 0.015, L"exact inverse zoom alignment tracks every pixel cleanly");
			Assert::IsTrue(rmsCam < rmsBlock * 0.25, L"exact zoom alignment substantially outperforms block-constant translation");
		}

		TEST_METHOD(ObjectAndOverlayBlocksKeepMeasuredResidualAlignment)
		{
			const int w = 64, h = 64, bs = 16;
			CameraModel::CameraMotion rec = MakeZoomRecord(w, h, bs, 1.10, 4.0, 0.0, 0.96);
			auto bd = std::make_shared<CameraModel::BlockData>(*rec.blocks);
			bd->label[0] = static_cast<uint8_t>(CameraModel::Label::Object);
			bd->label[1] = static_cast<uint8_t>(CameraModel::Label::Overlay);
			rec.blocks = bd;

			ResidualCameraAlign::Plane prev, cur, outRef, outCam;
			prev.Resize(w, h); cur.Resize(w, h);
			for (int y = 0; y < h; ++y)
				for (int x = 0; x < w; ++x)
				{
					prev.data[size_t(y) * w + x] = PatternAt(x + 0.5, y + 0.5);
					cur.data[size_t(y) * w + x] = 0.0f;
				}
			ResidualCameraAlign::BlockField bf;
			bf.gridW = w / bs; bf.gridH = h / bs; bf.blockSize = bs;
			bf.vx.assign(size_t(bf.gridW) * bf.gridH, 0.0f);
			bf.vy.assign(size_t(bf.gridW) * bf.gridH, 0.0f);
			bf.trusted.assign(size_t(bf.gridW) * bf.gridH, 1);

			CameraModel::Policy offPol = CameraModel::PolicyFor(CameraModel::Consumer::ResidualStabilise);
			CameraModel::Policy onPol = offPol;
			onPol.enabled = true;
			CameraModel::BlendState stOff, stOn;
			for (int i = 0; i < 6; ++i)
				stOn.Update(onPol, &rec);

			ResidualCameraAlign::Settings s;
			s.alpha = 0.0f;
			ResidualCameraAlign::AlignResidualFrame(offPol, stOff, &rec, true, prev, cur, bf, s, outRef);
			const auto r = ResidualCameraAlign::AlignResidualFrame(onPol, stOn, &rec, true, prev, cur, bf, s, outCam);
			Assert::AreEqual(uint64_t(2 * bs * bs), r.measuredPixels, L"Object and Overlay blocks keep measured path");
			for (int y = 0; y < bs; ++y)
				for (int x = 0; x < 2 * bs; ++x)
					Assert::AreEqual(outRef.data[size_t(y) * w + x], outCam.data[size_t(y) * w + x],
						L"Object and Overlay pixels are byte-identical to measured alignment");
		}

		TEST_METHOD(DegenerateOrSingularModelFallsBackVisiblyToUncompensated)
		{
			CameraModel::CameraMotion singular = MakeZoomRecord(64, 64, 16, 0.0, 0.0, 0.0, 0.96);
			Assert::IsFalse(ResidualCameraAlign::InvertModel(singular.params).valid, L"singular matrix rejected");

			ResidualCameraAlign::Plane prev, cur, out;
			prev.Resize(64, 64, 0.25f); cur.Resize(64, 64, 0.75f);
			ResidualCameraAlign::BlockField bf;
			bf.gridW = 4; bf.gridH = 4; bf.blockSize = 16.0;
			bf.vx.assign(16, 0.0f); bf.vy.assign(16, 0.0f); bf.trusted.assign(16, 1);
			CameraModel::Policy pol = CameraModel::PolicyFor(CameraModel::Consumer::ResidualStabilise);
			pol.enabled = true;
			CameraModel::BlendState st;
			for (int i = 0; i < 6; ++i) st.Update(pol, &singular);
			ResidualCameraAlign::Settings s;
			const auto r = ResidualCameraAlign::AlignResidualFrame(pol, st, &singular, true, prev, cur, bf, s, out);
			Assert::IsTrue(r.cameraLate, L"singular model triggers visible fallback");
			Assert::AreEqual(int(ResidualCameraAlign::Variant::Uncompensated), int(r.variant));
		}
	};
}
