// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// input.source_analysis/regions: shared region evidence (precursors + on-the-fly maps). CPU only: block size, the shared
// grain split reused, letterbox/pillarbox exclusion, synthetic ringing/banding/text cases, visibility, camera labels,
// determinism, the float mirror against the double reference, skeleton products and the VRAM/cost figures.
#include "pch.h"
#include <VP-V/modules/input/source_analysis/regions/RegionEvidence.h>
#include <VP-V/modules/input/source_analysis/regions/RegionEvidenceGlsl.h>
#include <VP-V/core/VramGovernor.h>
#include "RegionEvidenceSynthetic.h"

#include <cstring>
#include <memory>
#include <string>
#include "CppUnitTest.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	namespace
	{
		using namespace RegionEvidence;
		using namespace RegionSynth;

		constexpr int kW = 256, kH = 144, kB = 16;

		void Require(bool ok, const std::string& what)
		{
			if (ok) return;
			Logger::WriteMessage((what + "\n").c_str());
			Assert::Fail(std::wstring(what.begin(), what.end()).c_str());
		}

		struct Frame
		{
			Plane y;
			GrainSplit::Split split;
			Precursors pre;
		};

		Frame Make(const Plane& y, const Externals& ex, bool withSplit = true, const FrameParams& fp = FrameParams())
		{
			Frame f;
			f.y = y;
			FrameParams p = fp;
			if (p.blockOverride == 0) p.blockOverride = kB;
			if (withSplit) f.pre = ComputeFrame(f.y, ex, p, &f.split);
			else f.pre = Compute(f.y, nullptr, ex, p);
			return f;
		}

		Rect R(int l, int t, int r, int b) { Rect x; x.left = l; x.top = t; x.right = r; x.bottom = b; return x; }
	}

	TEST_CLASS(RegionEvidenceTests)
	{
	public:
		TEST_METHOD(BlockSizeIsDerivedFromTheFrame)
		{
			Assert::AreEqual(8, BlockSizeFor(480));
			Assert::AreEqual(8, BlockSizeFor(720));
			Assert::AreEqual(16, BlockSizeFor(1080));
			Assert::AreEqual(16, BlockSizeFor(1440));
			Assert::AreEqual(32, BlockSizeFor(2160));
			Assert::AreEqual(64, BlockSizeFor(4320));
			// About the same block count at every size (resolution-independent precursor memory).
			Assert::AreEqual(CostAt({ 1920, 1080 }).blocks, CostAt({ 3840, 2160 }).blocks);
			Assert::AreEqual(CostAt({ 3840, 2160 }).blocks, CostAt({ 7680, 4320 }).blocks);
		}

		TEST_METHOD(GrainLevelIsTheSharedSplitNotARecomputation)
		{
			Plane y = Flat(kW, kH, 400.0);
			AddGrain(y, 2.0, 11);
			Externals ex;
			const Frame f = Make(y, ex);
			const View v(&f.pre);
			Require(v.Valid() && f.pre.HaveSplit() && v.Status().empty() == false, "precursors valid with a split (no camera: reduced)");
			for (int by = 0; by < f.pre.grid.gh; ++by)
				for (int bx = 0; bx < f.pre.grid.gw; ++bx)
				{
					const double N = f.split.N[static_cast<size_t>(by) * f.split.bw + bx];
					Require(v.Stat(bx, by, kStatN) == N, "N is the split's N");
					Require(std::fabs(v.At(bx, by).grain - static_cast<float>(std::sqrt(N))) < 1e-5f, "grain = sqrt(N)");
				}
			// E1/E2/E3 are the split's own bands (block means of D1^2 ...), not a second filter.
			double e1 = 0.0;
			for (int yy = 0; yy < kB; ++yy)
				for (int xx = 0; xx < kB; ++xx) e1 += f.split.d1.at(xx, yy) * f.split.d1.at(xx, yy);
			Require(std::fabs(v.Stat(0, 0, kE1) - e1 / (kB * kB)) < 1e-9, "E1 = mean D1^2 of the split");
			// Without the split: visible reduced status, kNoGrain, grain/texture/ringing/banding 0.
			const Frame g = Make(y, ex, false);
			const View w(&g.pre);
			Require(std::string(w.Status()).find("no grain split") != std::string::npos, "status names the missing split");
			const Sample s = w.At(3, 3);
			Require((s.flags & kNoGrain) && s.grain == 0 && s.texture == 0 && s.ringing == 0 && s.banding == 0, "reduced form");
			Require(w.Status().find("no camera record") != std::string::npos, "every reduction is named, not only the first");
			// A split of another grid is refused visibly, never read at the wrong place.
			const GrainSplit::Split other = GrainSplit::Compute(y, 0.5, 8);
			FrameParams fp;
			fp.blockOverride = kB;
			const Precursors mis = Compute(y, &other, ex, fp);
			Require(mis.split == SplitState::Mismatched && View(&mis).Status().find("another size or grid") != std::string::npos &&
				(View(&mis).At(2, 2).flags & kNoGrain), "mismatched split: reduced and named");
			// Degenerate consumer parameters never give NaN.
			DeriveParams odd;
			odd.flatLo = odd.flatHi = 5.0;
			odd.ringFloor = 0.0;
			odd.edgeGateLo = odd.edgeGateHi = 0.0;
			odd.edgeGateOffLo = odd.edgeGateOffHi = 0.0;
			Plane flat = Flat(kW, kH, 400.0);
			const Frame z = Make(flat, ex);
			const Sample q = View(&z.pre, odd).At(4, 4);
			Require(q.ringing == q.ringing && q.banding == q.banding && q.banding >= 0.0f && q.banding <= 1.0f, "no NaN on degenerate ramps");
		}

		TEST_METHOD(LetterboxAndPillarboxBarsAreExcluded)
		{
			struct Case { const char* name; Rect pic; };
			const Case cases[] = { { "letterbox", R(0, 32, kW, 112) }, { "pillarbox", R(48, 0, 208, kH) },
				{ "letterbox off-grid", R(0, 40, kW, 104) } };
			for (const Case& c : cases)
			{
				Plane y = Flat(kW, kH, 500.0);
				AddGrain(y, 2.0, 21);
				Bars(y, c.pic, 64.0);
				Externals ex;
				ex.picture = c.pic;
				const Frame f = Make(y, ex);
				const View v(&f.pre);
				Externals whole;
				const Frame g = Make(y, whole);
				const View vw(&g.pre);
				double maxInsideEdge = 0.0, maxBoundaryEdge = 0.0, maxBoundaryEdgeNoRect = 0.0;
				double insideGrain = 0.0, boundaryGrain = 0.0, maxInsideTexture = 0.0, maxBoundaryTexture = 0.0;
				int insideCount = 0, boundaryCount = 0;
				int bars = 0, picture = 0;
				for (int by = 0; by < f.pre.grid.gh; ++by)
					for (int bx = 0; bx < f.pre.grid.gw; ++bx)
					{
						const Sample s = v.At(bx, by);
						const int x0 = bx * kB, y0 = by * kB, x1 = x0 + kB, y1 = y0 + kB;
						const int ix0 = std::max(x0, c.pic.left), iy0 = std::max(y0, c.pic.top);
						const int ix1 = std::min(x1, c.pic.right), iy1 = std::min(y1, c.pic.bottom);
						const int inside = (ix1 > ix0 && iy1 > iy0) ? (ix1 - ix0) * (iy1 - iy0) : 0;
						const bool expectIn = 2 * inside >= kB * kB;
						Require(s.InPicture() == expectIn, std::string(c.name) + ": in-picture flag follows the rect");
						if (!expectIn)
						{
							++bars;
							Require(s.edge == 0 && s.texture == 0 && s.ringing == 0 && s.banding == 0 && s.grain == 0 && s.visibility == 0,
								std::string(c.name) + ": a bar block carries no evidence");
							continue;
						}
						++picture;
						const bool boundary = inside < kB * kB || x0 <= c.pic.left || x1 >= c.pic.right || y0 <= c.pic.top || y1 >= c.pic.bottom;
						const bool frameEdge = (c.pic.left == 0 && x0 == 0) || (c.pic.right == kW && x1 == kW) || (c.pic.top == 0 && y0 == 0) || (c.pic.bottom == kH && y1 == kH);
						if (boundary && !frameEdge)
						{
							maxBoundaryEdge = std::max(maxBoundaryEdge, static_cast<double>(s.edge));
							maxBoundaryEdgeNoRect = std::max(maxBoundaryEdgeNoRect, static_cast<double>(vw.At(bx, by).edge));
							boundaryGrain += s.grain;
							++boundaryCount;
							maxBoundaryTexture = std::max(maxBoundaryTexture, static_cast<double>(s.texture));
						}
						else if (!boundary)
						{
							maxInsideEdge = std::max(maxInsideEdge, static_cast<double>(s.edge));
							insideGrain += s.grain;
							++insideCount;
							maxInsideTexture = std::max(maxInsideTexture, static_cast<double>(s.texture));
						}
					}
				char msg[200];
				snprintf(msg, sizeof(msg), "REGION_BARS %s bars=%d picture=%d edge inside=%.2f boundary=%.2f boundary_without_rect=%.2f\n",
					c.name, bars, picture, maxInsideEdge, maxBoundaryEdge, maxBoundaryEdgeNoRect);
				Logger::WriteMessage(msg);
				Require(bars > 0 && picture > 0, std::string(c.name) + ": both kinds of block");
				Require(maxBoundaryEdge <= 1.5 * maxInsideEdge + 1.0, std::string(c.name) + ": the bar edge does not read as an edge");
				Require(maxBoundaryEdgeNoRect > 100.0, std::string(c.name) + ": without the rect the bar edge would read (test is live)");
				const double gi = insideGrain / std::max(insideCount, 1), gb = boundaryGrain / std::max(boundaryCount, 1);
				snprintf(msg, sizeof(msg), "REGION_BARS %s grain inside=%.3f boundary=%.3f texture inside=%.2f boundary=%.2f\n", c.name, gi, gb,
					maxInsideTexture, maxBoundaryTexture);
				Logger::WriteMessage(msg);
				Require(insideCount > 0 && boundaryCount > 0 && std::fabs(gb - gi) <= 0.2 * gi, std::string(c.name) + ": bars do not lower the grain level");
				Require(maxBoundaryTexture <= maxInsideTexture + 1.0, std::string(c.name) + ": bars do not smear into the band energies");
			}
		}

		TEST_METHOD(RingingNearEdgesStandsAboveTheGrainFloor)
		{
			Externals ex;
			Plane clean = Step(kW, kH, 128, 300.0, 600.0, 0.0);
			AddGrain(clean, 1.5, 31);
			Plane ringing = Step(kW, kH, 128, 300.0, 600.0, 30.0);
			AddGrain(ringing, 1.5, 31);
			Plane flat = Flat(kW, kH, 300.0);
			AddGrain(flat, 1.5, 31);
			const Frame a = Make(clean, ex), b = Make(ringing, ex), c = Make(flat, ex);
			const View va(&a.pre), vb(&b.pre), vc(&c.pre);
			const int by = 4;
			const float ra = std::max(va.At(7, by).ringing, va.At(8, by).ringing);
			const float rb = std::max(vb.At(7, by).ringing, vb.At(8, by).ringing);
			const float rc = vc.At(7, by).ringing;
			char msg[160];
			snprintf(msg, sizeof(msg), "REGION_RINGING clean=%.3f ringing=%.3f flat=%.3f\n", ra, rb, rc);
			Logger::WriteMessage(msg);
			Require(rb > 0.5f, "ringing beside the edge is detected");
			Require(ra < 0.15f, "a clean step with grain reads little ringing");
			Require(rc == 0.0f, "grain alone (no edge) reads no ringing");
			Require(va.At(8, by).edge > 100.0f && va.At(8, by).edgeContrast > 0.9f, "edge strength and contrast of the step");
			Require(vc.At(8, by).edgeContrast < 0.6f, "grain alone has low edge contrast");
		}

		TEST_METHOD(BandingProneMaskFollowsFlatGradients)
		{
			Plane y = Ramp(kW, kH, 200.0, 0.3);
			AddTexture(y, 0, 96, 128, kH, 40.0, 4.0);                 // textured bottom-left
			for (int yy = 96; yy < kH; ++yy)                           // steep ramp bottom-right
				for (int x = 128; x < kW; ++x) y.at(x, yy) = 200.0 + 8.0 * x - 900.0;
			AddGrain(y, 0.5, 41);
			Externals ex;
			ex.overlays.push_back(R(160, 16, 224, 48));                 // a subtitle box over the smooth ramp
			const Frame f = Make(y, ex);
			const View v(&f.pre);
			const Sample smooth = v.At(4, 3), textured = v.At(3, 7), steep = v.At(12, 7), text = v.At(11, 2);
			char msg[200];
			snprintf(msg, sizeof(msg), "REGION_BANDING smooth=%.3f textured=%.3f (T %.1f) steep=%.3f text=%.3f\n", smooth.banding,
				textured.banding, textured.texture, steep.banding, text.banding);
			Logger::WriteMessage(msg);
			Require(smooth.banding > 0.7f, "a smooth gradient is banding-prone");
			Require(textured.banding < 0.1f && textured.texture > 50.0f, "texture is not banding-prone and reads texture energy");
			Require(steep.banding < 0.1f, "a steep ramp is not banding-prone");
			Require(text.Overlay() && text.banding == 0.0f, "under a subtitle box: overlay, no banding evidence");
			Require(smooth.texture < 1.0f, "grain is not texture (the split's N is subtracted)");
		}

		TEST_METHOD(TextOverlayMaskReusesTheExistingDetections)
		{
			Plane y = Flat(kW, kH, 400.0);
			AddGrain(y, 1.0, 51);
			MaskGrid guard;
			guard.w = 2; guard.h = 2; guard.cellPx = 128;
			guard.v = { 0, 0, 0, 1 };                                   // text-guard tile bottom-right
			auto blocks = std::make_shared<CameraModel::BlockData>();
			blocks->gridW = kW / 32; blocks->gridH = (kH + 31) / 32; blocks->blockSize = 32;
			blocks->label.assign(static_cast<size_t>(blocks->gridW) * blocks->gridH, static_cast<uint8_t>(CameraModel::Label::Camera));
			blocks->tinyMover.assign(blocks->label.size(), 0);
			blocks->label[0] = static_cast<uint8_t>(CameraModel::Label::Overlay);     // static logo top-left
			blocks->label[2] = static_cast<uint8_t>(CameraModel::Label::Object);
			blocks->tinyMover[2] = 1;
			CameraModel::CameraMotion cam;
			cam.valid = true;
			cam.confidence = 0.8;
			cam.blocks = blocks;
			Externals ex;
			ex.overlays.push_back(R(100, 60, 120, 70));
			ex.textGuard = &guard;
			ex.camera = &cam;
			const Frame f = Make(y, ex);
			const View v(&f.pre);
			Require(std::string(v.Status()).empty(), "complete evidence: empty status");
			Require(v.At(6, 3).Overlay() && v.At(7, 4).Overlay(), "subtitle box blocks");
			Require(v.At(12, 8).Overlay() && !v.At(12, 6).Overlay(), "text-guard tile (block centre lookup)");
			Require(v.At(0, 0).Overlay() && v.At(1, 1).Overlay(), "camera static-logo label");
			Require(!v.At(3, 0).Overlay() && !v.At(10, 1).Overlay(), "plain picture is not overlay");
			const Sample obj = v.At(4, 0);
			Require(obj.cameraLabel == static_cast<int>(CameraModel::Label::Object) && (obj.flags & kTinyMover), "camera label and tiny mover");
			Require(std::fabs(v.At(8, 4).cameraConfidence - 0.8f) < 1e-6f && v.At(8, 4).cameraLabel == 1, "camera confidence and Camera label");
			cam.labelsOnGpu = true;
			const Frame g = Make(y, ex);
			Require(std::string(View(&g.pre).Status()).find("labels only on the GPU") != std::string::npos, "GPU-only labels are named");
			cam.valid = false;
			const Frame h = Make(y, ex);
			Require(View(&h.pre).At(8, 4).cameraConfidence == 0.0f, "an invalid record gives confidence 0");
			Require(std::string(View(&h.pre).Status()).find("no camera record") != std::string::npos, "and says so");
		}

		TEST_METHOD(VisibilityIsHdrAwareAfterToneMapping)
		{
			Plane y(kW, kH, 0.0);
			for (int yy = 0; yy < kH; ++yy)
				for (int x = 0; x < kW; ++x) y.at(x, yy) = x < 64 ? 40.0 : (x < 128 ? 500.0 : (x < 192 ? 700.0 : 900.0));
			Externals sdr;
			const Frame s = Make(y, sdr);
			Externals pq;
			pq.transfer = static_cast<int>(Transfer::Pq);
			const Frame p = Make(y, pq);
			const View vs(&s.pre), vp(&p.pre);
			char msg[200];
			snprintf(msg, sizeof(msg), "REGION_VISIBILITY sdr btb=%.3f 500=%.3f 900=%.3f | pq 500=%.3f 700=%.3f 900=%.3f\n", vs.At(1, 2).visibility,
				vs.At(5, 2).visibility, vs.At(14, 2).visibility, vp.At(5, 2).visibility, vp.At(10, 2).visibility, vp.At(14, 2).visibility);
			Logger::WriteMessage(msg);
			Require(vs.At(1, 2).visibility == 0.0f, "BTB carries no visible step");
			Require(vp.At(14, 2).visibility < vp.At(10, 2).visibility && vp.At(14, 2).visibility < 0.5f * vp.At(5, 2).visibility,
				"PQ highlights above the display peak are compressed: less visible");
			Require(std::fabs(vp.Stat(14, 2, kMeanPq) - (900.0 - 64.0) / 876.0) < 1e-6, "PQ perceptual mean of a PQ source is its code");
			Require(vs.Stat(5, 2, kMeanNits) > 10.0 && vs.Stat(5, 2, kMeanNits) < 50.0, "SDR mid code in nits");
		}

		TEST_METHOD(DeterministicAndTheFloatMirrorMatchesTheReference)
		{
			Plane y = Step(kW, kH, 100, 250.0, 700.0, 20.0);
			AddTexture(y, 150, 20, 250, 120, 25.0, 5.0);
			AddGrain(y, 1.5, 61);
			Externals ex;
			ex.picture = R(0, 16, kW, 128);
			Bars(y, ex.picture);
			const Frame a = Make(y, ex), b = Make(y, ex);
			Require(a.pre.data.size() == b.pre.data.size() &&
				std::memcmp(a.pre.data.data(), b.pre.data.data(), a.pre.data.size() * sizeof(double)) == 0, "bit-identical precursors");
			std::vector<float> luma, bands, mirror;
			PackLuma(a.y, luma);
			BandTexelGrid(a.split, a.pre.picture, bands);
			const StatsConstants k = MakeStatsConstants(kW, kH, kB, a.pre.picture, ex, true, VisibilityParams());
			StatsGridMirror(k, luma.data(), bands.data(), mirror);
			int bad = 0, offFlips = 0;
			for (size_t i = 0; i < a.pre.grid.Count(); ++i)
				for (int c = 0; c < kStatsChannels; ++c)
				{
					const double ref = a.pre.data[i * kPrecursorChannels + c], got = mirror[i * kStatsChannels + c];
					if (std::fabs(got - ref) > 1e-3 * std::max(std::fabs(ref), 1.0))
					{
						// A pixel within an ulp of the off-edge threshold may flip (one pixel of the block): tolerated twice.
						if (c == kOffLap || c == kOffShare) ++offFlips;
						else ++bad;
					}
				}
			Require(bad == 0 && offFlips <= 4, "float mirror vs double reference: " + std::to_string(bad) + " channels differ, " +
				std::to_string(offFlips) + " off-edge flips");
		}

		TEST_METHOD(AccessorIsNeutralOutsideAndMapsPixelsToBlocks)
		{
			Plane y = Flat(kW, kH, 400.0);
			AddGrain(y, 1.0, 71);
			Externals ex;
			const Frame f = Make(y, ex);
			const View v(&f.pre);
			Require(!v.At(-1, 0).valid && !v.At(f.pre.grid.gw, 0).valid && !View().At(0, 0).valid, "neutral outside the grid");
			Require(v.AtPixel(33.0, 17.5).grain == v.At(2, 1).grain, "pixel -> floor(px / B)");
			Require(Smooth(1.0f, 3.0f, 0.25f, false) == 1.5f && Smooth(1.0f, 3.0f, 0.25f, true) == 3.0f, "smoothing resets at a discontinuity");
			Precursors p = f.pre;
			VpAncestry::AncestorRef anc[2];
			anc[0].productId = 8; anc[0].seq = 5; anc[0].cgen = 3;
			anc[1].productId = VpSkeleton::kP4SourceStats; anc[1].seq = 5; anc[1].cgen = 2;
			Stamp(p, 5, 7, VpAncestry::Provenance::AheadLane, 3, anc, 2);
			Require(p.header.seq == 5 && p.header.cgen == 7 && p.header.ancestorCount == 2 && p.header.ancestors[1].cgen == 2, "ancestry stamp");
		}

		TEST_METHOD(PrecursorProductsRegisterWithTheGraphAndTheVramGovernor)
		{
			using namespace VpSkeleton;
			for (const Dims d : { Dims{ 3840, 2160 }, Dims{ 7680, 4320 } })
			{
				std::vector<ProductDecl> decls = FirstProducts().All();
				const ProductIds ids = AppendProducts(decls, d);
				const Registry reg(decls);
				const char* why = "";
				Require(reg.Validate(&why), std::string("registry: ") + why);
				ConsumerDecl deband;
				deband.id = 40;
				AddConsume(deband, ids.blockStats);
				AddConsume(deband, ids.grainSplit);
				GraphSettings gs;
				gs.dims = d;
				const Graph g = BuildGraph(reg, { deband }, gs);
				Require(g.valid && g.Has(ids.grainSplit) && g.Has(ids.blockStats) && g.Has(kP4SourceStats), "graph closure");
				Require(g.IndexOf(ids.grainSplit) < g.IndexOf(ids.blockStats), "split before the block stats");
				CardProfile card;
				card.key.luid = 1; card.key.driverVersion = 1;
				card.vramBudgetBytes = 3500ull << 20; card.vramUsageBytes = 1000ull << 20; card.systemRamFreeBytes = 8000ull << 20;
				card.uploadGBps = 12.0; card.copyGBps = 150.0; card.computeScale = 1.0; card.ofaPresent = true; card.ofaMsPerPairRef = 11.0;
				card.measured = true;
				const TierPlan plan = PlanTiers(reg, g, card, TierInput());
				const TierEntry* es = plan.Find(ids.grainSplit);
				const TierEntry* eb = plan.Find(ids.blockStats);
				Require(es && eb && es->tier == Tier::T0 && eb->tier == Tier::T0, "both precursors are T0 tier entries");
				Require(plan.governor.Find(uint16_t(kUserProductBase + ids.grainSplit)) != nullptr &&
					plan.governor.Find(uint16_t(kUserProductBase + ids.blockStats)) != nullptr, "registered as VRAM governor users");
				const Cost c = CostAt(d);
				const Node* ns = g.Find(ids.grainSplit);
				const Node* nb = g.Find(ids.blockStats);
				char msg[320];
				snprintf(msg, sizeof(msg), "REGION_VRAM %ux%u B=%d blocks=%zu split=%.1f MB/buffer (ring %.1f + live %.1f MB) stats=%.0f KB/buffer (ring %.0f + live %.0f KB) derived-maps-if-stored=%.0f KB grant split=%s stats=%s\n",
					d.width, d.height, c.B, c.blocks, c.grainSplitBytes / 1048576.0, ns->ringBytes / 1048576.0, ns->liveBytes / 1048576.0,
					c.blockStatsBytes / 1024.0, nb->ringBytes / 1024.0, nb->liveBytes / 1024.0, c.derivedMapBytes / 1024.0,
					Token(es->why), Token(eb->why));
				Logger::WriteMessage(msg);
				Require(ns->bufferBytes == BytesOf(reg.Get(ids.grainSplit).size, d) && nb->bufferBytes == BytesOf(reg.Get(ids.blockStats).size, d),
					"graph buffer sizes are the core declarations (SA0: the declarations live once, in core/Products.h)");
			}
			Assert::AreEqual(static_cast<uint64_t>(45749760), CostAt({ 3840, 2160 }).grainSplitBytes);
			Assert::AreEqual(static_cast<uint64_t>(182607360), CostAt({ 7680, 4320 }).grainSplitBytes);
			Assert::AreEqual(static_cast<uint64_t>(8160 * 96), CostAt({ 3840, 2160 }).blockStatsBytes);
			Assert::AreEqual(static_cast<uint64_t>(8160 * 96), CostAt({ 7680, 4320 }).blockStatsBytes);
		}

		TEST_METHOD(GrainProductIsOneSharedProductForEveryGrainReader)
		{
			using namespace VpSkeleton;
			Plane y = Step(kW, kH, 120, 250.0, 650.0, 0.0);
			AddGrain(y, 2.0, 81);
			Externals ex;
			ex.picture = R(0, 16, kW, 128);
			Bars(y, ex.picture);
			const GrainProduct g = ComputeGrain(y, ex.picture, kB);
			const GrainView gv(&g);
			Require(gv.Valid() && gv.BlockSize() == kB, "grain product valid on the region grid");
			for (int yy = 16; yy < 128; ++yy)
				for (int x = 0; x < kW; ++x)
					Require(gv.Clean(x, yy) + gv.Grain(x, yy) == y.at(x, yy), "Y = clean + grain exactly inside the picture");
			// The precursors read the same product (no second split).
			FrameParams fp;
			fp.blockOverride = kB;
			GrainSplit::Split fromFrame;
			const Precursors p = ComputeFrame(y, ex, fp, &fromFrame);
			Require(fromFrame.N == g.split.N && fromFrame.clean.v == g.split.clean.v, "ComputeFrame uses ComputeGrain's product");
			Require(View(&p).Stat(3, 3, kStatN) == gv.Level(3, 3), "block stats carry the product's N");
			Require(gv.LevelAtPixel(3 * kB + 5, 3 * kB + 7) == gv.Level(3, 3) && !GrainView().Valid() && GrainView().Clean(1, 1) == 0.0,
				"level lookup; an absent product is invalid");
			GrainProduct stamped = g;
			VpAncestry::AncestorRef anc;
			anc.productId = kP4SourceStats; anc.seq = 9; anc.cgen = 4;
			StampGrain(stamped, 9, 2, VpAncestry::Provenance::AheadLane, 3, &anc, 1);
			Require(stamped.header.seq == 9 && stamped.header.cgen == 2 && stamped.header.ancestorCount == 1, "grain ancestry stamp");
			// Four grain readers plus the region stats: one grain_split node in the graph.
			std::vector<ProductDecl> decls = FirstProducts().All();
			const ProductIds ids = AppendProducts(decls, { 3840, 2160 });
			const Registry reg(decls);
			std::vector<ConsumerDecl> consumers = GrainConsumers(ids, 200);
			ConsumerDecl stats;
			stats.id = 210;
			AddConsume(stats, ids.blockStats);
			consumers.push_back(stats);
			GraphSettings gs;
			gs.dims = { 3840, 2160 };
			const Graph graph = BuildGraph(reg, consumers, gs);
			int splitNodes = 0;
			for (const Node& n : graph.nodes) splitNodes += n.id == ids.grainSplit ? 1 : 0;
			Require(graph.valid && consumers.size() == 5 && splitNodes == 1, "one grain_split node for five readers");
		}
	};
}
