// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// K5-1: GrainCameraRefine (CPU reference). A synthetic grainy pan: the grain level of the content alternates in stripes that
// move with the camera; some blocks are untrusted for the matcher every frame (flat grainy areas). Today's rule carries an
// untrusted block's statistic along at the same cell, so on a pan it smears; the camera rule carries it along with the model.
// CPU only; the GLSL equals-the-mirror check is a WARP test that waits for the K5-2 wiring.

#include <VP-V/modules/repair/grain_split/GrainCameraRefine.h>

#include <cmath>
#include <memory>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace GrainCameraRefineTestsNs
{
	using namespace GrainCameraRefine;

	constexpr int kGw = 40, kGh = 24;
	constexpr double kBlock = 16.0;

	inline void Require(bool ok, const wchar_t* what) { Assert::IsTrue(ok, what); }

	// Deterministic hash in [0, 1).
	inline double Hash01(uint32_t a, uint32_t b, uint32_t c)
	{
		uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA6Bu ^ (c + 0x165667B1u) * 0xC2B2AE35u;
		h ^= h >> 15;
		h *= 0x2C1B3C6Du;
		h ^= h >> 12;
		h *= 0x297A2D39u;
		h ^= h >> 15;
		return (h & 0xFFFFFFu) / 16777216.0;
	}

	// The statistic of the content at world x: stripes of 96 px (six blocks) alternating between two levels.
	inline double Truth(double worldX)
	{
		const long long stripe = static_cast<long long>(std::floor(worldX / 96.0));
		return ((stripe % 2 + 2) % 2 == 0) ? 0.5 : 0.95;
	}

	inline std::shared_ptr<CameraModel::BlockData> Blocks(double modelX, double modelY, CameraModel::Label label)
	{
		auto b = std::make_shared<CameraModel::BlockData>();
		b->gridW = kGw;
		b->gridH = kGh;
		b->blockSize = kBlock;
		const size_t n = static_cast<size_t>(kGw) * kGh;
		b->label.assign(n, static_cast<uint8_t>(label));
		b->resX.assign(n, 0.0f);
		b->resY.assign(n, 0.0f);
		b->modelX.assign(n, static_cast<float>(modelX));
		b->modelY.assign(n, static_cast<float>(modelY));
		b->objectId.assign(n, 0);
		b->tinyMover.assign(n, 0);
		b->excluded.assign(n, 0);
		b->absThresholdPx = 0.75f;
		b->relThreshold = 0.05f;
		return b;
	}

	inline std::shared_ptr<CameraModel::CameraMotion> Record(double confidence, std::shared_ptr<CameraModel::BlockData> blocks)
	{
		auto r = std::make_shared<CameraModel::CameraMotion>();
		r->valid = true;
		r->confidence = confidence;
		r->reason = CameraModel::Reason::Ok;
		r->pairSeconds = 1.0 / 24;
		r->blocks = blocks;
		return r;
	}

	inline CameraModel::Policy GrainPolicy(bool enabled)
	{
		CameraModel::Policy p = CameraModel::PolicyFor(CameraModel::Consumer::GrainMap);
		p.enabled = enabled;
		return p;
	}

	// Settles the blend state at weight 1 (rate limit 0.25: four updates).
	inline void Settle(const CameraModel::Policy& p, const CameraModel::CameraMotion* rec, CameraModel::BlendState& st)
	{
		for (int i = 0; i < 8; ++i)
			st.Update(p, rec);
	}

	struct PanRun
	{
		double rmse = 0;          // map vs truth, frames >= warm
		double rmsMap = 0, rmsTruth = 0;
		std::vector<Grid> maps;   // every frame
		std::vector<uint32_t> stamps;
		std::vector<Variant> variants;
	};

	// One run: content moves right by vx px per frame; per frame, a block's target is the truth at its content position
	// (+/- 0.02 deterministic noise) and it is trusted with probability `trustedShare`.
	inline PanRun RunPan(bool camera, double vx, double trustedShare, int frames, int warm, bool p8Available = true,
		CameraModel::Label label = CameraModel::Label::Camera, double confidence = 0.95)
	{
		PanRun run;
		const CameraModel::Policy policy = GrainPolicy(camera);
		auto rec = Record(confidence, Blocks(vx, 0.0, label));
		CameraModel::BlendState state;
		Grid prev;
		prev.Resize(kGw, kGh);
		double sumSq = 0, sumMap = 0, sumTruth = 0;
		long long count = 0;
		for (int k = 0; k < frames; ++k)
		{
			state.Update(policy, rec.get());
			BlockInputs in;
			const size_t n = static_cast<size_t>(kGw) * kGh;
			in.target.resize(n);
			in.trusted.resize(n);
			in.vx.assign(n, static_cast<float>(vx));
			in.vy.assign(n, 0.0f);
			std::vector<double> truth(n);
			for (int by = 0; by < kGh; ++by)
				for (int bx = 0; bx < kGw; ++bx)
				{
					const size_t i = static_cast<size_t>(by) * kGw + bx;
					const double wx = (bx + 0.5) * kBlock - k * vx;
					truth[i] = Truth(wx);
					in.target[i] = static_cast<float>(truth[i] + 0.04 * (Hash01(bx, by, k) - 0.5));
					in.trusted[i] = Hash01(bx, by, k + 1000) < trustedShare ? 1 : 0;
				}
			Settings s;
			s.blockSize = kBlock;
			s.alpha = 0.5f;
			s.reset = (k == 0);
			Grid out;
			const FrameResult r = RefineFrame(policy, state, rec.get(), p8Available, prev, in, s, out);
			run.stamps.push_back(r.stamp);
			run.variants.push_back(r.variant);
			if (k >= warm)
				for (size_t i = 0; i < n; ++i)
				{
					const double d = out.value[i] - truth[i];
					sumSq += d * d;
					sumMap += static_cast<double>(out.value[i]) * out.value[i];
					sumTruth += truth[i] * truth[i];
					++count;
				}
			run.maps.push_back(out);
			prev = out;
		}
		run.rmse = std::sqrt(sumSq / static_cast<double>(count));
		run.rmsMap = std::sqrt(sumMap / static_cast<double>(count));
		run.rmsTruth = std::sqrt(sumTruth / static_cast<double>(count));
		return run;
	}

	inline bool MapsEqual(const Grid& a, const Grid& b)
	{
		return a.w == b.w && a.h == b.h && a.value.size() == b.value.size() &&
			std::memcmp(a.value.data(), b.value.data(), a.value.size() * sizeof(float)) == 0 && a.valid == b.valid;
	}

	TEST_CLASS(GrainCameraRefineTests)
	{
	public:
		TEST_METHOD(VariantStampCarriesBothVersions)
		{
			const uint32_t u = VariantStamp(Variant::Uncompensated), c = VariantStamp(Variant::Camera);
			Require(u != c, L"the two variants carry different stamps");
			Require((u >> 16) == GrainSplit::kUnitVersion && (c >> 16) == GrainSplit::kUnitVersion, L"split version in the high half");
			Require(kGrainSplitUnitVersion == GrainSplit::kUnitVersion, L"alias of the split unit version");
			Require(((c >> 1) & 0x7FFF) == kRefineUnitVersion, L"refine version in the low half");
			Require((u & 1u) == 0 && (c & 1u) == 1, L"the flag bit is the variant");
		}

		TEST_METHOD(TodaysRuleBlockArithmetic)
		{
			Grid prev;
			prev.Resize(2, 1);
			prev.value[0] = 0.4f;
			prev.valid[0] = 1;
			prev.value[1] = 0.8f;
			prev.valid[1] = 0;
			float v = 0;
			uint8_t ok = 9;
			SmoothBlock(prev, 0, true, 1.0f, 0.25f, false, v, ok);
			Require(std::fabs(v - 0.55f) < 1e-6f && ok == 1, L"trusted with a valid previous: mix(prev, target, alpha)");
			SmoothBlock(prev, 1, true, 0.5f, 0.25f, false, v, ok);
			Require(v == 0.5f && ok == 1, L"trusted with an invalid previous: the target");
			SmoothBlock(prev, 0, true, 0.5f, 0.25f, true, v, ok);
			Require(v == 0.5f && ok == 1, L"a reset takes the target");
			SmoothBlock(prev, 0, false, 0.1f, 0.25f, false, v, ok);
			Require(v == 0.4f && ok == 0, L"untrusted: the previous value, no measurement");
			SmoothBlock(prev, 0, false, 0.1f, 0.25f, true, v, ok);
			Require(v == 1.0f && ok == 0, L"untrusted after a reset: 1");
		}

		TEST_METHOD(TodaysVectorFetchHandNumbers)
		{
			// Hand-computed from MAP_BODY: trusted, v = 17 px, alpha 0.5. Row 0 previous values 0.2, 0.4, 0.6, 0.8 (valid).
			// Block 2 centre 40 - 17 = 23 -> cell 1 (0.4): out = 0.4 + (1.0 - 0.4) * 0.5 = 0.7.
			Grid prev, out;
			prev.Resize(4, 1);
			for (int x = 0; x < 4; ++x) { prev.value[x] = 0.2f + 0.2f * x; prev.valid[x] = 1; }
			BlockInputs in;
			in.target.assign(4, 1.0f);
			in.trusted.assign(4, 1);
			in.vx.assign(4, 17.0f);
			in.vy.assign(4, 0.0f);
			Settings s;
			s.alpha = 0.5f;
			CameraModel::BlendState st;
			RefineFrame(GrainPolicy(false), st, nullptr, false, prev, in, s, out);
			Require(std::fabs(out.value[2] - 0.7f) < 1e-6f && std::fabs(out.value[3] - 0.8f) < 1e-6f, L"hand numbers");
			Require(std::fabs(out.value[0] - 0.6f) < 1e-6f, L"block 0: position clamps to cell 0 (0.2): 0.2 + 0.8 * 0.5 = 0.6");
		}

		TEST_METHOD(GridMismatchIsLateAndBadShapeKeepsThePreviousMap)
		{
			const CameraModel::Policy on = GrainPolicy(true);
			auto rec = Record(0.95, Blocks(16.0, 0.0, CameraModel::Label::Camera));
			CameraModel::BlendState st;
			Settle(on, rec.get(), st);
			Grid prev, out;
			prev.Resize(kGw - 1, kGh);   // not P8's grid
			BlockInputs in;
			const size_t n = static_cast<size_t>(kGw - 1) * kGh;
			in.target.assign(n, 0.7f); in.trusted.assign(n, 1); in.vx.assign(n, 0.0f); in.vy.assign(n, 0.0f);
			Settings s;
			FrameResult r = RefineFrame(on, st, rec.get(), true, prev, in, s, out);
			Require(r.cameraLate && r.variant == Variant::Uncompensated, L"a different grid: flagged late, uncompensated");
			prev.Resize(kGw, kGh, 0.3f);
			BlockInputs bad;
			r = RefineFrame(on, st, rec.get(), true, prev, bad, s, out);
			Require(r.badShape && out.value == prev.value, L"bad inputs: the previous map is carried, flagged");
		}

		TEST_METHOD(CameraOffEqualsTodaysRule)
		{
			// Switch off: no camera blocks, no flag, and the map equals a run whose P8 never exists.
			const PanRun off = RunPan(false, 17.0, 0.3, 30, 10);
			const PanRun absent = RunPan(true, 17.0, 0.3, 30, 10, false);
			for (size_t k = 0; k < off.maps.size(); ++k)
			{
				Require(MapsEqual(off.maps[k], absent.maps[k]), L"P8 absent = today's map, every frame");
				Require(off.variants[k] == Variant::Uncompensated && absent.variants[k] == Variant::Uncompensated, L"uncompensated variant");
			}
		}

		TEST_METHOD(PanCompensationFollowsContentAndKeepsGrainRms)
		{
			const PanRun today = RunPan(false, 17.0, 0.3, 48, 12);
			const PanRun cam = RunPan(true, 17.0, 0.3, 48, 12);
			Require(cam.rmse < 0.7 * today.rmse, L"misregistration error falls by at least 30%");
			Require(std::fabs(cam.rmsMap / cam.rmsTruth - 1.0) <= 0.05, L"grain-level RMS within +-5% of the content's");
			Require(cam.variants.back() == Variant::Camera, L"camera variant on a settled pan");
			Require(cam.stamps.back() == VariantStamp(Variant::Camera), L"stamp matches the variant");
		}

		TEST_METHOD(StaticLag1AndMapUnchanged)
		{
			// Static content (vector 0): the camera rule fetches the same cell, so the map is bit-identical and so is lag-1.
			const PanRun off = RunPan(false, 0.0, 0.5, 40, 5);
			const PanRun on = RunPan(true, 0.0, 0.5, 40, 5);
			for (size_t k = 0; k < off.maps.size(); ++k)
				Require(MapsEqual(off.maps[k], on.maps[k]), L"static: identical map every frame");
			// Lag-1 autocorrelation of every block's series over frames 5.., both runs, must agree exactly.
			auto lag1 = [](const PanRun& r)
			{
				double acc = 0;
				for (size_t i = 0; i < r.maps[0].value.size(); ++i)
				{
					double m = 0;
					int n = 0;
					for (size_t k = 5; k < r.maps.size(); ++k) { m += r.maps[k].value[i]; ++n; }
					m /= n;
					double num = 0, den = 0;
					for (size_t k = 5; k < r.maps.size(); ++k)
					{
						const double a = r.maps[k].value[i] - m;
						den += a * a;
						if (k + 1 < r.maps.size())
							num += a * (r.maps[k + 1].value[i] - m);
					}
					acc += den > 0 ? num / den : 0;
				}
				return acc / static_cast<double>(r.maps[0].value.size());
			};
			Require(lag1(off) == lag1(on), L"lag-1 unchanged on static content");
		}

		TEST_METHOD(ObjectBlocksAndLowTrustKeepTodaysRule)
		{
			const PanRun today = RunPan(false, 17.0, 0.3, 30, 10);
			const PanRun objects = RunPan(true, 17.0, 0.3, 30, 10, true, CameraModel::Label::Object);
			const PanRun low = RunPan(true, 17.0, 0.3, 30, 10, true, CameraModel::Label::Camera, 0.4);   // below the 0.5 floor
			for (size_t k = 0; k < today.maps.size(); ++k)
			{
				Require(MapsEqual(today.maps[k], objects.maps[k]), L"object blocks: today's smoothing");
				Require(MapsEqual(today.maps[k], low.maps[k]), L"below the trust floor: today's smoothing");
			}
			Require(objects.variants.back() == Variant::Uncompensated && low.variants.back() == Variant::Uncompensated, L"no camera block, no variant flag");
		}

		TEST_METHOD(AbsentOrLateP8IsFlaggedUncompensated)
		{
			const CameraModel::Policy on = GrainPolicy(true), off = GrainPolicy(false);
			CameraModel::BlendState st;
			Grid prev, out;
			prev.Resize(kGw, kGh);
			BlockInputs in;
			const size_t n = static_cast<size_t>(kGw) * kGh;
			in.target.assign(n, 0.7f);
			in.trusted.assign(n, 1);
			in.vx.assign(n, 3.0f);
			in.vy.assign(n, 0.0f);
			Settings s;
			s.alpha = 0.5f;
			FrameResult r = RefineFrame(on, st, nullptr, false, prev, in, s, out);
			Require(r.cameraWanted && r.cameraLate && r.variant == Variant::Uncompensated && r.cameraBlocks == 0, L"no record: late, uncompensated");
			auto rec = Record(0.95, Blocks(3.0, 0.0, CameraModel::Label::Camera));
			Settle(on, rec.get(), st);
			r = RefineFrame(on, st, rec.get(), false, prev, in, s, out);
			Require(r.cameraLate && r.variant == Variant::Uncompensated, L"record but P8 not published (late): flagged");
			r = RefineFrame(off, st, rec.get(), true, prev, in, s, out);
			Require(!r.cameraWanted && !r.cameraLate && r.variant == Variant::Uncompensated, L"switch off: no flag, no 'camera late'");
			r = RefineFrame(on, st, rec.get(), true, prev, in, s, out);
			Require(!r.cameraLate && r.variant == Variant::Camera && r.cameraBlocks == n, L"present: every camera block compensated");
		}

		TEST_METHOD(ModelPositionIsFetchedAndUntrustedCarriesAlong)
		{
			// One row, camera moving right by exactly one block per frame: the value at block 3 moves to block 4 even when
			// block 4 is untrusted; today's rule would leave it in place (block 4 keeps its own old value).
			const CameraModel::Policy on = GrainPolicy(true);
			auto blocks = Blocks(static_cast<double>(kBlock), 0.0, CameraModel::Label::Camera);
			auto rec = Record(0.95, blocks);
			CameraModel::BlendState st;
			Settle(on, rec.get(), st);
			Grid prev, out;
			prev.Resize(kGw, kGh, 0.0f);
			for (int x = 0; x < kGw; ++x)
			{
				prev.value[prev.Index(x, 0)] = x == 3 ? 0.9f : 0.5f;
				prev.valid[prev.Index(x, 0)] = 1;
			}
			BlockInputs in;
			const size_t n = static_cast<size_t>(kGw) * kGh;
			in.target.assign(n, 0.0f);
			in.trusted.assign(n, 0);
			in.vx.assign(n, 0.0f);
			in.vy.assign(n, 0.0f);
			Settings s;
			s.blockSize = kBlock;
			RefineFrame(on, st, rec.get(), true, prev, in, s, out);
			Require(out.value[out.Index(4, 0)] == 0.9f, L"the statistic moved with the camera");
			Require(out.valid[out.Index(4, 0)] == 0, L"still marked as carried, not measured");
			Require(out.value[out.Index(3, 0)] == 0.5f, L"the cell it left takes its upstream neighbour");
		}

		TEST_METHOD(DeterministicAndGlslGatedByTheSwitch)
		{
			const PanRun a = RunPan(true, 17.0, 0.3, 20, 5), b = RunPan(true, 17.0, 0.3, 20, 5);
			for (size_t k = 0; k < a.maps.size(); ++k)
				Require(MapsEqual(a.maps[k], b.maps[k]), L"two runs, identical maps");
			Require(GlslText(false).empty(), L"consumer off: no shader text at all");
			const std::string t = GlslText(true);
			Require(t.find("camera_field_vector") != std::string::npos, L"calls the accessor's GLSL port");
			Require(t.find("VPU_GRAIN_CAMERA_REFINE_VERSION " + std::to_string(kRefineUnitVersion)) != std::string::npos, L"variant stamp define");
			Require(t == GlslText(true), L"text is constant");
		}
	};
}
