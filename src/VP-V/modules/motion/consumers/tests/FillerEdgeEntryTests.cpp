// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// K8-1: FillerEdgeEntry (CPU mirror). Untrusted camera blocks take the P8 model vector; a filled block whose back-projection leaves
// the active picture is new content and never filled. Pan mirror symmetry, zoom edges, phases, switch off / absent P8 / object
// blocks unchanged.

#include <VP-V/modules/motion/consumers/FillerEdgeEntry.h>

#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace FillerEdgeEntryTestsNs
{
	using namespace FillerEdgeEntry;

	constexpr int kGw = 40, kGh = 22;
	constexpr double kBlock = 16.0;

	inline void Require(bool ok, const wchar_t* what) { Assert::IsTrue(ok, what); }

	inline std::shared_ptr<CameraModel::CameraMotion> Record(const std::vector<float>& mx, const std::vector<float>& my,
		CameraModel::Label label = CameraModel::Label::Camera, double confidence = 0.95)
	{
		auto b = std::make_shared<CameraModel::BlockData>();
		b->gridW = kGw;
		b->gridH = kGh;
		b->blockSize = kBlock;
		const size_t n = static_cast<size_t>(kGw) * kGh;
		b->label.assign(n, static_cast<uint8_t>(label));
		b->resX.assign(n, 0.0f);
		b->resY.assign(n, 0.0f);
		b->modelX = mx;
		b->modelY = my;
		b->objectId.assign(n, 0);
		b->tinyMover.assign(n, 0);
		b->excluded.assign(n, 0);
		b->absThresholdPx = 0.75f;
		b->relThreshold = 0.05f;
		auto r = std::make_shared<CameraModel::CameraMotion>();
		r->valid = true;
		r->confidence = confidence;
		r->reason = CameraModel::Reason::Ok;
		r->pairSeconds = 1.0 / 24;
		r->blocks = b;
		r->picture.left = 0;
		r->picture.top = 0;
		r->picture.right = static_cast<int>(kGw * kBlock);
		r->picture.bottom = static_cast<int>(kGh * kBlock);
		return r;
	}

	inline std::vector<float> Fill(float v) { return std::vector<float>(static_cast<size_t>(kGw) * kGh, v); }

	inline CameraModel::Policy Strict(bool enabled)
	{
		CameraModel::Policy p = CameraModel::PolicyFor(CameraModel::Consumer::DejudderWarp);
		p.enabled = enabled;
		return p;
	}

	inline void Settle(const CameraModel::Policy& p, const CameraModel::CameraMotion* rec, CameraModel::BlendState& st)
	{
		for (int i = 0; i < 12; ++i)
			st.Update(p, rec);
	}

	inline size_t At(int bx, int by) { return static_cast<size_t>(by) * kGw + bx; }

	TEST_CLASS(FillerEdgeEntryTests)
	{
	public:
		TEST_METHOD(PanEntersAndLeavesAtTheEdges)
		{
			auto rec = Record(Fill(24.0f), Fill(0.0f));
			const CameraModel::Policy p = Strict(true);
			CameraModel::BlendState st;
			Settle(p, rec.get(), st);
			Require(st.Weight() == 1.0, L"settled at weight 1");
			const Geometry g = GeometryFor(*rec, kBlock, 0.5);
			const std::vector<uint8_t> untrusted(static_cast<size_t>(kGw) * kGh, 0);
			std::vector<BlockResult> out;
			const Counts n = ClassifyGrid(p, st, rec.get(), g, kGw, kGh, untrusted, Fill(0.0f), Fill(0.0f), out);
			// +24 px pan, phase 0.5: srcA = x - 12 (< 0 for the first column), srcB = x + 12 (>= 640 for the last).
			for (int by = 0; by < kGh; ++by)
			{
				const BlockResult& first = out[At(0, by)];
				Require(first.kind == Kind::NewContent && first.entry == Entry::OutsideA && !first.fill, L"left column: enters, never filled");
				const BlockResult& last = out[At(kGw - 1, by)];
				Require(last.kind == Kind::NewContent && last.entry == Entry::OutsideB && !last.fill, L"right column: leaves, never filled");
				for (int bx = 1; bx < kGw - 1; ++bx)
				{
					const BlockResult& m = out[At(bx, by)];
					Require(m.kind == Kind::Model && m.fill && m.entry == Entry::None && m.vx == 24.0f && m.vy == 0.0f, L"inside: the model vector, filled");
				}
			}
			Require(n.newContent == 2u * kGh && n.model == static_cast<uint32_t>(kGw - 2) * kGh && n.entryA == kGh && n.entryB == kGh, L"counts");
		}

		TEST_METHOD(TrustedMeasuredEdgeBlocksAreNeverNewContent)
		{
			auto rec = Record(Fill(24.0f), Fill(0.0f));
			const CameraModel::Policy p = Strict(true);
			CameraModel::BlendState st;
			Settle(p, rec.get(), st);
			const Geometry g = GeometryFor(*rec, kBlock, 0.5);
			const std::vector<uint8_t> trusted(static_cast<size_t>(kGw) * kGh, 1);
			std::vector<BlockResult> out;
			const Counts n = ClassifyGrid(p, st, rec.get(), g, kGw, kGh, trusted, Fill(24.0f), Fill(0.0f), out);
			for (int by = 0; by < kGh; ++by)
			{
				Require(out[At(0, by)].kind != Kind::NewContent && !out[At(0, by)].fill, L"trusted left edge: not NewContent, not filled");
				Require(out[At(kGw - 1, by)].kind != Kind::NewContent && !out[At(kGw - 1, by)].fill, L"trusted right edge: not NewContent, not filled");
			}
			Require(n.newContent == 0u, L"no trusted block is counted as new content");
		}

		TEST_METHOD(MirrorPanGivesMirroredBlocks)
		{
			auto right = Record(Fill(24.0f), Fill(0.0f)), left = Record(Fill(-24.0f), Fill(0.0f));
			const CameraModel::Policy p = Strict(true);
			CameraModel::BlendState sr, sl;
			Settle(p, right.get(), sr);
			Settle(p, left.get(), sl);
			const std::vector<uint8_t> untrusted(static_cast<size_t>(kGw) * kGh, 0);
			for (double phase : { 0.0, 0.25, 0.5, 0.75, 1.0 })
			{
				std::vector<BlockResult> a, b;
				ClassifyGrid(p, sr, right.get(), GeometryFor(*right, kBlock, phase), kGw, kGh, untrusted, Fill(0.0f), Fill(0.0f), a);
				ClassifyGrid(p, sl, left.get(), GeometryFor(*left, kBlock, phase), kGw, kGh, untrusted, Fill(0.0f), Fill(0.0f), b);
				for (int by = 0; by < kGh; ++by)
					for (int bx = 0; bx < kGw; ++bx)
					{
						const BlockResult& x = a[At(bx, by)];
						const BlockResult& y = b[At(kGw - 1 - bx, by)];
						Require(x.kind == y.kind && x.entry == y.entry && x.fill == y.fill && x.vx == -y.vx && x.vy == y.vy, L"mirror: same kind and entry, mirrored vector");
					}
			}
		}

		TEST_METHOD(PhaseSelectsWhichSideCanLeave)
		{
			auto rec = Record(Fill(24.0f), Fill(0.0f));
			const CameraModel::Policy p = Strict(true);
			CameraModel::BlendState st;
			Settle(p, rec.get(), st);
			// Phase 0: the block shows frame A at its own position (inside); only the B side (x + 24) can leave: last two columns.
			const BlockResult a0 = Classify(p, st, rec.get(), GeometryFor(*rec, kBlock, 0.0), 0, 5, false, 0.0f, 0.0f);
			Require(a0.entry == Entry::None && a0.fill, L"phase 0: left column is still inside");
			const BlockResult b0 = Classify(p, st, rec.get(), GeometryFor(*rec, kBlock, 0.0), kGw - 1, 5, false, 0.0f, 0.0f);
			Require(b0.entry == Entry::OutsideB && b0.kind == Kind::NewContent, L"phase 0: right column leaves");
			const BlockResult b1 = Classify(p, st, rec.get(), GeometryFor(*rec, kBlock, 1.0), kGw - 1, 5, false, 0.0f, 0.0f);
			Require(b1.entry == Entry::None && b1.fill, L"phase 1: right column is inside (B side at its own position)");
			const BlockResult a1 = Classify(p, st, rec.get(), GeometryFor(*rec, kBlock, 1.0), 0, 5, false, 0.0f, 0.0f);
			Require(a1.entry == Entry::OutsideA && a1.kind == Kind::NewContent, L"phase 1: left column entered");
			const BlockResult clamped = Classify(p, st, rec.get(), GeometryFor(*rec, kBlock, 7.0), 0, 5, false, 0.0f, 0.0f);
			Require(clamped.entry == a1.entry, L"phase is clamped to [0, 1]");
		}

		TEST_METHOD(ZoomOutEntersAndZoomInLeavesAtTheEdges)
		{
			// Content moves with x: v = k (p - centre). Zoom-out (k < 0): edge content comes from outside; zoom-in (k > 0): it leaves.
			std::vector<float> mx(static_cast<size_t>(kGw) * kGh), my(mx.size());
			const double cx = kGw * kBlock * 0.5, cy = kGh * kBlock * 0.5;
			for (int by = 0; by < kGh; ++by)
				for (int bx = 0; bx < kGw; ++bx)
				{
					mx[At(bx, by)] = static_cast<float>(-0.1 * ((bx + 0.5) * kBlock - cx));
					my[At(bx, by)] = static_cast<float>(-0.1 * ((by + 0.5) * kBlock - cy));
				}
			const CameraModel::Policy p = Strict(true);
			for (int sign = -1; sign <= 1; sign += 2)
			{
				std::vector<float> zx = mx, zy = my;
				for (size_t i = 0; i < zx.size(); ++i) { zx[i] *= static_cast<float>(-sign); zy[i] *= static_cast<float>(-sign); }
				// sign = -1: zx = +0.1 (p - c)?  mx is -0.1(p - c) (zoom-out); multiplying by -sign = +1 keeps zoom-out for sign -1.
				auto rec = Record(zx, zy);
				CameraModel::BlendState st;
				Settle(p, rec.get(), st);
				const Geometry g = GeometryFor(*rec, kBlock, 0.5);
				const Entry want = sign < 0 ? Entry::OutsideA : Entry::OutsideB;
				const BlockResult corner = Classify(p, st, rec.get(), g, 0, 0, false, 0.0f, 0.0f);
				Require(corner.entry == want && corner.kind == Kind::NewContent && !corner.fill, sign < 0 ? L"zoom-out: the corner enters" : L"zoom-in: the corner leaves");
				const BlockResult mid = Classify(p, st, rec.get(), g, kGw / 2, kGh / 2, false, 0.0f, 0.0f);
				Require(mid.entry == Entry::None && mid.fill, L"the centre block is filled");
			}
		}

		TEST_METHOD(MeasuredHeldAndSwitchOffAreToday)
		{
			auto rec = Record(Fill(24.0f), Fill(0.0f));
			CameraModel::BlendState on, off;
			const CameraModel::Policy pOn = Strict(true), pOff = Strict(false);
			Settle(pOn, rec.get(), on);
			Settle(pOff, rec.get(), off);
			const Geometry g = GeometryFor(*rec, kBlock, 0.5);
			// A trusted measured block keeps its own vector even where the model would put it outside the picture.
			const BlockResult m = Classify(pOn, on, rec.get(), g, 0, 3, true, 5.0f, -1.0f);
			Require(m.kind == Kind::Measured && m.vx == 5.0f && m.vy == -1.0f && !m.fill, L"trusted: measured, untouched");
			// Switch off: the hold value passes through unchanged, never NewContent.
			const BlockResult h = Classify(pOff, off, rec.get(), g, 0, 3, false, 2.0f, 0.0f);
			Require(h.kind == Kind::Hold && h.vx == 2.0f && h.vy == 0.0f && !h.fill, L"switch off: today's hold value");
			// P8 absent (no record): same.
			const BlockResult none = Classify(pOn, on, nullptr, g, 5, 3, false, 2.0f, 0.0f);
			Require(none.kind == Kind::Hold && none.vx == 2.0f, L"no record: today's hold value");
			// Object block: untrusted, not camera: hold, never filled from the model.
			auto objects = Record(Fill(24.0f), Fill(0.0f), CameraModel::Label::Object);
			CameraModel::BlendState so;
			Settle(pOn, objects.get(), so);
			const BlockResult o = Classify(pOn, so, objects.get(), g, 5, 3, false, 2.0f, 0.0f);
			Require(o.kind == Kind::Hold && o.vx == 2.0f && !o.fill, L"object block keeps the hold");
			// Below the strict floor (0.7): weight 0, hold.
			auto weak = Record(Fill(24.0f), Fill(0.0f), CameraModel::Label::Camera, 0.6);
			CameraModel::BlendState sw;
			Settle(pOn, weak.get(), sw);
			const BlockResult w = Classify(pOn, sw, weak.get(), g, 5, 3, false, 2.0f, 0.0f);
			Require(w.kind == Kind::Hold && w.vx == 2.0f, L"confidence under the strict floor: hold");
		}

		TEST_METHOD(PartialWeightIsABlendAndStillEdgeChecked)
		{
			auto rec = Record(Fill(24.0f), Fill(0.0f));
			const CameraModel::Policy p = Strict(true);
			CameraModel::BlendState st;
			for (int i = 0; i < 3; ++i)
				st.Update(p, rec.get());   // rate 0.2: weight 0.6
			Require(std::fabs(st.Weight() - 0.6) < 1e-9, L"rate-limited weight 0.6");
			const Geometry g = GeometryFor(*rec, kBlock, 0.5);
			const BlockResult b = Classify(p, st, rec.get(), g, 10, 4, false, 0.0f, 0.0f);
			Require(b.kind == Kind::Blend && b.fill && std::fabs(b.vx - 14.4f) < 1e-4f, L"blend of the hold value 0 and the model 24 at 0.6");
			// Blended vector 14.4 at phase 1: srcA = 8 - 14.4 < 0 at the first column: new content.
			const BlockResult e = Classify(p, st, rec.get(), GeometryFor(*rec, kBlock, 1.0), 0, 4, false, 0.0f, 0.0f);
			Require(e.kind == Kind::NewContent && e.entry == Entry::OutsideA, L"the blended vector is edge-checked");
		}

		TEST_METHOD(MarginAndDeterminism)
		{
			auto rec = Record(Fill(24.0f), Fill(0.0f));
			const CameraModel::Policy p = Strict(true);
			CameraModel::BlendState st;
			Settle(p, rec.get(), st);
			Geometry g = GeometryFor(*rec, kBlock, 0.5);
			g.margin = 8.0;   // block 1 centre 24: srcA = 12 is inside the plain picture but within 8 px of nothing; srcA 12 >= 8 stays inside
			Require(Classify(p, st, rec.get(), g, 1, 3, false, 0.0f, 0.0f).fill, L"margin 8: block 1 still inside");
			g.margin = 16.0;
			Require(Classify(p, st, rec.get(), g, 1, 3, false, 0.0f, 0.0f).kind == Kind::NewContent, L"margin 16: block 1 now enters");
			std::vector<BlockResult> a, b;
			const std::vector<uint8_t> untrusted(static_cast<size_t>(kGw) * kGh, 0);
			ClassifyGrid(p, st, rec.get(), g, kGw, kGh, untrusted, Fill(0.0f), Fill(0.0f), a);
			ClassifyGrid(p, st, rec.get(), g, kGw, kGh, untrusted, Fill(0.0f), Fill(0.0f), b);
			bool same = a.size() == b.size();
			for (size_t i = 0; same && i < a.size(); ++i)
				same = a[i].kind == b[i].kind && a[i].entry == b[i].entry && a[i].vx == b[i].vx && a[i].fill == b[i].fill;
			Require(same, L"two runs agree");
			// Short input arrays: nothing is classified.
			std::vector<BlockResult> c;
			const Counts n = ClassifyGrid(p, st, rec.get(), g, kGw, kGh, std::vector<uint8_t>(3, 0), Fill(0.0f), Fill(0.0f), c);
			Require(n.model == 0 && n.newContent == 0 && c.size() == static_cast<size_t>(kGw) * kGh, L"short inputs: empty result");
		}
	};
}
