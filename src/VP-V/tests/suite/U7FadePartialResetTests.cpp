// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "pch.h"
#include "CppUnitTest.h"

// U7 v3, the partial reset on fades (owner 2026-11-15 09:20, refs\102b U7-FADE-NO-HARDRESTART): a fade is no full restart; the
// history is reduced by the frame's fade keep (CutFadeJudge::FrameFadeKeep), monotonically over the fade; a cut still restarts in
// full; a flash or a pan reduces nothing. CPU only: the U7 law on real P4 records (SourceStats::ComputeRecord) of synthetic half
// planes, SceneRestart.h with an injected scene reader, the detail_pulse_stabilise / texture_restore CPU models (the modules'
// mirrors) and the temporal detail CPU model.

#include <VP-V/core/FrameAnalysisStore.h>
#include <VP-V/core/hub/CurrentFrame.h>
#include <VP-V/core/hub/ExtensionHub.h>
#include <VP-V/modules/control/skeleton/SkeletonAccess.h>
#include <VP-V/modules/input/source_analysis/SourceFrameTap.h>
#include <VP-V/modules/input/source_analysis/events/shared/CutFadeJudge.h>
#include <VP-V/modules/input/source_analysis/lane/shared/SourceStats.h>
#include <VP-V/modules/repair/temporal_detail/TemporalDetailModel.h>
#include <VP-V/modules/repair/temporal_detail/TemporalDetailMotion.h>
#include <VP-V/modules/repair/texture_restore/SceneRestart.h>
#include <VP-V/tests/support/DetailPulseModel.h>
#include <VP-V/tests/support/TextureRestoreModel.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace U7FadePartialResetTestsNs
{
	using Record = VpUnit::SourceStats::Record;
	using Events = VpUnit::CutFadeJudge::EventSignals;
	using VpUnit::CutFadeJudge::FadeRun;

	constexpr uint32_t kW = 160, kH = 90;
	constexpr double kBlack = 4096.0;   // P4 codes of the 8-bit-like black 16

	inline int Noise(uint32_t x, uint32_t y, uint32_t k)
	{
		uint32_t h = (x * 73856093u) ^ (y * 19349663u) ^ (k * 83492791u);
		h ^= h >> 13;
		h *= 0x5bd1e995u;
		h ^= h >> 15;
		return static_cast<int>(h % 513u) - 256;
	}

	inline Record RecordOf(const std::vector<uint16_t>& half)
	{
		std::vector<uint8_t> buf(VpUnit::SourceStats::kRecordBytes);
		Record r;
		Assert::IsTrue(VpUnit::SourceStats::ComputeRecord(half.data(), kW, kH, buf.data(), buf.size()), L"the record");
		Assert::IsTrue(VpUnit::SourceStats::ReadRecord(buf.data(), buf.size(), &r), L"the record reads back");
		return r;
	}

	// A horizontal ramp scaled about black by `gain`, deterministic noise; pan: the ramp is twice the plane wide and moves by `shift`.
	inline Record Picture(double gain, uint32_t k, uint32_t shift = 0, bool pan = false)
	{
		std::vector<uint16_t> half(size_t(kW) * kH);
		for (uint32_t y = 0; y < kH; ++y)
			for (uint32_t x = 0; x < kW; ++x)
			{
				const int64_t base = 8192 + (int64_t(x + shift) * 48000) / (pan ? int64_t(2 * kW) : int64_t(kW - 1));
				const double v = kBlack + gain * (double(base) - kBlack) + double(Noise(x, y, k));
				half[size_t(y) * kW + x] = static_cast<uint16_t>((std::min)(65535.0, (std::max)(0.0, std::floor(v + 0.5))));
			}
		return RecordOf(half);
	}

	// Another picture altogether (a cut target): a flat mid grey with noise.
	inline Record Flat(uint32_t k)
	{
		std::vector<uint16_t> half(size_t(kW) * kH);
		for (uint32_t y = 0; y < kH; ++y)
			for (uint32_t x = 0; x < kW; ++x)
				half[size_t(y) * kW + x] = static_cast<uint16_t>(30000 + Noise(x, y, k));
		return RecordOf(half);
	}

	struct Played
	{
		std::vector<char> fade, cut, flash;
		std::vector<double> keep;   // the frame's history keep (FrameFadeKeep; 1 = no fade)
	};

	// Every pair in order through one run, exactly as the skeleton's SceneFactsFor does.
	inline Played Play(const std::vector<Record>& recs)
	{
		Played p;
		p.fade.assign(recs.size(), 0);
		p.cut.assign(recs.size(), 0);
		p.flash.assign(recs.size(), 0);
		p.keep.assign(recs.size(), 1.0);
		FadeRun run;
		for (size_t k = 1; k < recs.size(); ++k)
		{
			const Events e = VpUnit::CutFadeJudge::ClassifyEvents(recs[k - 1], recs[k], false, false);
			double runKeep = 1.0;
			const bool runFade = VpUnit::CutFadeJudge::FoldFadeRun(recs[k - 1], recs[k], e, run, &runKeep);
			p.keep[k] = VpUnit::CutFadeJudge::FrameFadeKeep(recs[k - 1], recs[k], e.fade, runFade, runKeep);
			p.fade[k] = (runFade || e.fade) ? 1 : 0;
			p.cut[k] = e.cut ? 1 : 0;
			p.flash[k] = e.flash ? 1 : 0;
		}
		return p;
	}

	// --- injected scene readers (function pointers: the state is file-static) ---
	struct ReaderPlan
	{
		uint64_t fadeFrom = 0, fadeTo = 0;   // [from, to): fade frames
		float fadeKeep = 0.9f;               // < 0: a fade group without a keep
		uint64_t cutAt = 0;                  // the delivered cut (0 = none)
		uint64_t flashAt = 0;                // a flash (not a cut, not a fade)
	};
	ReaderPlan g_plan;

	bool PlanReader(uint64_t seq, FrameAnalysis::Scene& s)
	{
		s = FrameAnalysis::Scene();
		s.origin = FrameAnalysis::SceneOrigin::Vp;
		s.fadeKeep = 1.0f;
		if (seq >= g_plan.fadeFrom && seq < g_plan.fadeTo)
		{
			s.fade = true;
			s.fadeKeep = g_plan.fadeKeep;
		}
		if (seq == g_plan.cutAt)
		{
			s.cut = true;
			s.cutHere = true;
			s.confirmed = true;
		}
		if (seq == g_plan.flashAt)
		{
			s.flash = true;
			s.cutPossible = true;
		}
		return true;
	}

	// C5 (WAIVER-C5-SCENEOF-DPTR; owner 2026-11-14 17:38 "Ask the skeleton for it same as everything else"): a skeleton module in the
	// hub whose IExternalDemand::SceneOf answers LIVE from g_plan, as the real SceneOf does with frame_analysis_store off (that live
	// path itself is pinned on the real module: SkeletonModuleTests, "2. Store off"). `unknownAt` = a frame it cannot answer.
	struct LiveSceneSkeleton : Vpv::Hub::ISkeletonModule, Vpv::Skeleton::IExternalDemand
	{
		const char* Name() const override { return "test skeleton (scene product, live)"; }
		void* Query(const char* iid) override
		{
			return iid && std::strcmp(iid, Vpv::Skeleton::kExternalDemandQueryId) == 0 ? static_cast<Vpv::Skeleton::IExternalDemand*>(this) : nullptr;
		}
		void SetExternalDemand(const void*, const void*, const std::vector<VpSkeleton::ConsumerDecl>&, const std::vector<VpSkeleton::ProductId>&) override {}
		void ClearExternalDemand(const void*, const void*) override {}
		Vpv::Skeleton::SceneAnswer SceneOf(const void* host, uint64_t seq) const override
		{
			asks.push_back(std::make_pair(host, seq));
			Vpv::Skeleton::SceneAnswer a;
			FrameAnalysis::Scene s;
			if (seq == unknownAt || !PlanReader(seq, s))
				return a;
			a.known = true;
			a.cut = s.cut; a.fade = s.fade; a.cutLate = s.cutLate; a.cutHere = s.cutHere; a.confirmed = s.confirmed;
			a.flash = s.flash; a.cutPossible = s.cutPossible; a.cutStrength = s.cutStrength; a.fadeKeep = s.fadeKeep;
			a.from = Vpv::Skeleton::SceneAnswer::From::Live;
			return a;
		}
		uint64_t unknownAt = 0;
		mutable std::vector<std::pair<const void*, uint64_t>> asks;
	};

	// A textured plane (codes of a 10-bit signal), the same picture every frame with its own fine noise.
	inline GrainModel::Image Textured(int w, int h, uint32_t k)
	{
		GrainModel::Image im(w, h);
		for (int y = 0; y < h; ++y)
			for (int x = 0; x < w; ++x)
				im.at(x, y) = 400.0 + 60.0 * std::sin(0.37 * x) * std::cos(0.23 * y) + 0.02 * Noise(uint32_t(x), uint32_t(y), k);
		return im;
	}

	TEST_CLASS(U7FadePartialResetTests)
	{
	public:
		// The law: over a fade the keeps are in [0, 1], their product never rises, the static frames keep everything, and the
		// product reaches the contrast the pre-fade picture still explains (a fade to 0.25 of the contrast: about 0.25).
		TEST_METHOD(AFadeGivesAMonotoneHistoryReductionToTheContrastLeft)
		{
			const int pre = 10, n = 120, post = 10;
			std::vector<Record> recs;
			uint32_t k = 0;
			for (int i = 0; i < pre; ++i) recs.push_back(Picture(1.0, k++));
			for (int i = 1; i <= n; ++i) recs.push_back(Picture(1.0 - 0.75 * i / n, k++));
			for (int i = 0; i < post; ++i) recs.push_back(Picture(0.25, k++));
			const Played p = Play(recs);
			double product = 1.0, previous = 1.0;
			int fades = 0;
			for (size_t f = 1; f < recs.size(); ++f)
			{
				Assert::IsTrue(p.keep[f] >= 0.0 && p.keep[f] <= 1.0, L"every keep is a share of the history");
				Assert::IsFalse(p.cut[f] != 0, L"no cut inside the fade");
				if (!p.fade[f])
					Assert::AreEqual(1.0, p.keep[f], L"a frame that is no fade keeps the whole history");
				fades += p.fade[f] ? 1 : 0;
				product *= p.keep[f];
				Assert::IsTrue(product <= previous + 1e-15, L"the reduction over the fade is monotone");
				previous = product;
			}
			for (int f = 1; f < pre; ++f)
				Assert::AreEqual(1.0, p.keep[size_t(f)], L"static frames before the fade: nothing reduced");
			char text[200];
			snprintf(text, sizeof(text), "fade 1 -> 0.25 over %d frames: %d fade frames, history product %.4f", n, fades, product);
			Logger::WriteMessage(text);
			Assert::IsTrue(fades >= n / 2, Wide(text).c_str());
			Assert::IsTrue(product < 0.35 && product > 0.15, Wide(text).c_str());
		}

		// SceneRestart.h: a fade with its keep is NO restart and hands the keep on; without a keep it restarts, flagged.
		TEST_METHOD(AFadeIsAPartialResetAndAFadeWithoutAKeepRestartsVisibly)
		{
			g_plan = ReaderPlan();
			g_plan.fadeFrom = 5; g_plan.fadeTo = 10; g_plan.fadeKeep = 0.8f;
			const VpDetailRestore::SceneRestartAnswer a = VpDetailRestore::SceneRestartOf(6, &PlanReader);
			Assert::IsFalse(a.restart, L"a fade is no full restart (owner 2026-11-15)");
			Assert::IsTrue(a.fadePartial && !a.fadeWithoutKeep);
			Assert::AreEqual(0.8f, a.historyKeep, L"the keep is handed on");
			const VpDetailRestore::SceneRestartAnswer none = VpDetailRestore::SceneRestartOf(3, &PlanReader);
			Assert::IsFalse(none.restart || none.fadePartial);
			Assert::AreEqual(1.0f, none.historyKeep, L"no fade: the whole history");
			g_plan.fadeKeep = -1.0f;
			const VpDetailRestore::SceneRestartAnswer old = VpDetailRestore::SceneRestartOf(6, &PlanReader);
			Assert::IsTrue(old.restart && old.fadeWithoutKeep && !old.fadePartial, L"a fade group without its keep: the full restart, flagged");
			Assert::IsTrue(std::string(VpDetailRestore::SceneFadeWithoutKeepWhy()).size() > 10, L"and explained");
		}

		// A cut still restarts in full, also inside a fade; the law's run ends at it.
		TEST_METHOD(ACutStillGivesAFullRestart)
		{
			g_plan = ReaderPlan();
			g_plan.fadeFrom = 5; g_plan.fadeTo = 20; g_plan.fadeKeep = 0.8f; g_plan.cutAt = 12;
			const VpDetailRestore::SceneRestartAnswer a = VpDetailRestore::SceneRestartOf(12, &PlanReader);
			Assert::IsTrue(a.restart && !a.fadePartial, L"the delivered cut restarts, fade or not");
			Assert::AreEqual(0.0f, a.historyKeep);
			Assert::AreEqual(1.0f, a.strength);
			// The law: a cut in the middle of a fade (the ramp, then another picture) ends the run and is no fade frame.
			std::vector<Record> recs;
			for (int i = 0; i < 30; ++i) recs.push_back(Picture(1.0 - 0.01 * i, uint32_t(i)));
			recs.push_back(Flat(30));
			const Played p = Play(recs);
			Assert::IsTrue(p.cut[30] != 0, L"the pair law sees the cut");
			Assert::IsFalse(p.fade[30] != 0, L"a cut is no fade frame");
			Assert::AreEqual(1.0, p.keep[30], L"the cut frame carries no partial factor: the consumer restarts on the cut instead");
		}

		// A pan (the same ramp moving) and a flash reduce nothing.
		TEST_METHOD(AFlashOrAPanGivesNoReset)
		{
			std::vector<Record> pan;
			for (uint32_t i = 0; i < 60; ++i) pan.push_back(Picture(1.0, i, 2 * i, true));
			const Played p = Play(pan);
			for (size_t f = 1; f < pan.size(); ++f)
			{
				Assert::IsFalse(p.cut[f] != 0, L"a pan is no cut");
				Assert::AreEqual(1.0, p.keep[f], L"a pan keeps the whole history");
			}
			g_plan = ReaderPlan();
			g_plan.flashAt = 7;
			const VpDetailRestore::SceneRestartAnswer a = VpDetailRestore::SceneRestartOf(7, &PlanReader);
			Assert::IsFalse(a.restart || a.fadePartial, L"a flash is no restart and no fade");
			Assert::AreEqual(1.0f, a.historyKeep, L"a flash keeps the whole history");
		}

		// detail_pulse_stabilise's model (the module's mirror): no restart on the fade frames, their keep applied (the gains never
		// above the same run with keep 1, i.e. the history only reduced), and a cut still restarts with gain 1.
		TEST_METHOD(DetailPulseModelFadeIsPartialAndCutRestarts)
		{
			const int w = 96, h = 64, frames = 30;
			DetailPulseModel::Settings s;
			s.sourceHz = 23.976;
			s.colorDepth = 10;
			s.sceneReader = &PlanReader;
			auto run = [&](float keep, uint64_t cutAt) {
				g_plan = ReaderPlan();
				g_plan.fadeFrom = 8; g_plan.fadeTo = 21; g_plan.fadeKeep = keep; g_plan.cutAt = cutAt;
				DetailPulseModel::Model m;
				std::vector<DetailPulseModel::Frame> out;
				for (int k = 0; k < frames; ++k)
				{
					// The picture's fine texture dips on every other frame (a coding pulse the stabiliser lifts).
					GrainModel::Image im = Textured(w, h, uint32_t(k));
					if (k % 2)
						for (double& v : im.v) v = 400.0 + 0.6 * (v - 400.0);
					out.push_back(m.Process(im, uint64_t(k) + 1, s));
				}
				return out;
			};
			const std::vector<DetailPulseModel::Frame> full = run(1.0f, 0), partial = run(0.7f, 0), cut = run(0.7f, 25);
			bool strictlyReduced = false;
			for (int k = 8; k < 21; ++k)
			{
				const size_t u = size_t(k) - 1;   // sequence k is frame index k - 1
				Assert::IsFalse(partial[u].restart, L"no full restart on a fade frame");
				Assert::IsTrue(partial[u].fadePartial, L"the fade frame is a partial reset");
				Assert::AreEqual(0.7, partial[u].historyKeep, 1e-6, L"its keep is applied");
				Assert::IsTrue(partial[u].g1 <= full[u].g1 + 1e-12 && partial[u].g2 <= full[u].g2 + 1e-12, L"the history is only reduced");
				if (partial[u].g1 < full[u].g1 - 1e-6 || partial[u].g2 < full[u].g2 - 1e-6)
					strictlyReduced = true;
			}
			Assert::IsTrue(strictlyReduced, L"fade keep=0.7 must strictly reduce gain on at least one lifted fade frame");
			Assert::IsTrue(cut[24].restart, L"the cut restarts");
			Assert::AreEqual(1.0, cut[24].g1);
			Assert::AreEqual(1.0, cut[24].g2);
		}

		// texture_restore's model (the module's mirror): no restart on the fade frames, their keep applied; a cut restarts.
		TEST_METHOD(TextureRestoreModelFadeIsPartialAndCutRestarts)
		{
			const int w = 96, h = 64, frames = 30;
			auto run = [&](float keep, uint64_t cutAt) {
				g_plan = ReaderPlan();
				g_plan.fadeFrom = 8; g_plan.fadeTo = 21; g_plan.fadeKeep = keep; g_plan.cutAt = cutAt;
				TextureRestoreModel::Model m;
				std::vector<std::vector<double>> out;
				for (int k = 0; k < frames; ++k)
				{
					const GrainModel::Image g = Textured(w, h, uint32_t(k));
					TextureRestoreModel::Image im(w, h);
					for (size_t i = 0; i < im.v.size(); ++i)
						im.v[i] = (k % 2) ? (400.0 + 0.6 * (g.v[i] - 400.0)) : g.v[i];
					TextureRestore::Params p;
					p.colorDepth = 10; p.sourceHz = 23.976; p.bitrateQuality = 1.0; p.sequence = uint64_t(k) + 1;
					p.sceneReader = &PlanReader;
					out.push_back(m.Frame(im, p).v);
					const uint64_t seq = p.sequence;
					if (seq >= 8 && seq < 21)
					{
						Assert::IsFalse(m.LastWasCut(), L"no full restart on a fade frame");
						Assert::IsTrue(m.LastWasFadePartial(), L"the fade frame is a partial reset");
						Assert::AreEqual(static_cast<double>(keep), m.LastHistoryKeep(), 1e-6);
					}
					else if (cutAt != 0 && seq == cutAt)
						Assert::IsTrue(m.LastWasCut(), L"the cut restarts");
					else if (seq > 1)
						Assert::IsFalse(m.LastWasCut() || m.LastWasFadePartial(), L"elsewhere: neither");
				}
				return out;
			};
			const auto outPartial = run(0.7f, 25);
			const auto outFull = run(1.0f, 25);
			double maxFadeDiff = 0.0;
			for (int k = 8; k < 21; ++k)
			{
				const size_t u = static_cast<size_t>(k) - 1;
				for (size_t i = 0; i < outPartial[u].size(); ++i)
					maxFadeDiff = (std::max)(maxFadeDiff, std::fabs(outPartial[u][i] - outFull[u][i]));
			}
			Assert::IsTrue(maxFadeDiff > 1e-3, L"partial fade keep=0.7 must visibly alter restored output vs keep=1.0");
		}

		// temporal detail's model: a fade with its keep resets nothing (the store is reduced instead), a fade without a keep and a
		// cut reset; a block that keeps transferring from the same slot sees its keep fall by the fade's keep each fade frame.
		TEST_METHOD(TemporalDetailModelFadeIsPartialAndCutResets)
		{
			const int w = 128, h = 96, frames = 34;
			TemporalDetail::FieldMotionSource motion;
			for (int t = 0; t < frames; ++t)
			{
				motion.SetBackward(uint64_t(t), TemporalDetail::MotionPair::Uniform(w, h, 16.0, 0.0, 0.0));
				motion.SetForward(uint64_t(t), TemporalDetail::MotionPair::Uniform(w, h, 16.0, 0.0, 0.0));
			}
			TemporalDetail::Settings set;
			set.mode = TemporalDetail::Mode::Steady;
			TemporalDetail::Model model(set);
			std::vector<TemporalDetail::Diagnostics> d;
			const double keep = 0.9;
			for (int t = 0; t < frames; ++t)
			{
				TemporalDetail::Model::FrameIn in;
				in.luma = TemporalDetail::Plane(w, h);
				const GrainModel::Image g = Textured(w, h, uint32_t(t));
				for (size_t i = 0; i < in.luma.v.size(); ++i) in.luma.v[i] = (t % 3 ? 0.7 : 1.0) * (g.v[i] - 400.0) + 400.0;   // an I-like frame every third
				in.seq = uint64_t(t);
				in.contentHz = 23.976;
				in.planeLines = 1080.0;
				in.motion = &motion;
				if (t >= 10 && t < 20) { in.fade = true; in.fadeKeep = keep; }
				if (t == 24) { in.fade = true; in.fadeKeep = -1.0; }   // a producer before v3
				if (t == 28) in.cut = true;
				model.Process(in);
				d.push_back(model.Diag());
			}
			int observed = 0;
			for (int t = 10; t < 20; ++t)
			{
				const size_t u = size_t(t);
				Assert::AreEqual(d[u - 1].resets, d[u].resets, L"a fade with its keep resets nothing");
				Assert::IsTrue(d[u].status.find("fade: partial reset") == 0, Wide(d[u].status).c_str());
				Assert::AreEqual(d[u - 1].fadePartials + 1, d[u].fadePartials);
				Assert::AreEqual(keep, d[u].fadeKeep);
				for (size_t b = 0; b < d[u].historyKeep.size(); ++b)
				{
					Assert::IsTrue(d[u].historyKeep[b] <= 1.0 && d[u].historyKeep[b] >= 0.0);
					if (t > 10 && d[u].slotId[b] >= 0 && d[u].slotId[b] == d[u - 1].slotId[b] && d[u].refreshed[b] == 0 &&
						d[u].refSeq[b] >= 0 && d[u - 1].refSeq[b] >= 0)
					{
						Assert::AreEqual(d[u - 1].historyKeep[b] * keep, d[u].historyKeep[b], 1e-12, L"the slot's keep falls by the fade's keep");
						++observed;
					}
				}
			}
			char text[160];
			snprintf(text, sizeof(text), "TD partial reset: %d block-frames transferred from the same slot through the fade", observed);
			Logger::WriteMessage(text);
			Assert::IsTrue(observed > 0, Wide(std::string(text) + " (the per-slot product check must see a transfer)").c_str());   // WAIVER-U7FADE-PARTIAL-2 (audit C2)
			Assert::AreEqual(d[23].resets + 1, d[24].resets, L"a fade without its keep resets (shown)");
			Assert::IsTrue(d[24].status.find("reset: fade (no keep)") == 0, Wide(d[24].status).c_str());
			Assert::AreEqual(d[27].resets + 1, d[28].resets, L"a cut resets");
		}

		// C5 (WAIVER-C5-SCENEOF-DPTR; gate C5 prequal x5dup MUST 1, extended to DP/TR by lead 18): the PRODUCTION reader of detail
		// pulse / texture restore asks the skeleton's get-or-compute reader (SceneOf) for the renderer this thread renders, never a
		// store Peek. With frame_analysis_store OFF (the Host's store registered DISABLED: the Peek finds nothing) a cut still
		// restarts in full and a fade still gets its partial reset. A frame the skeleton cannot answer, and no skeleton module, stay
		// visible restarts with their cause.
		TEST_METHOD(StoreOffACutStillRestartsThroughTheSkeleton)
		{
			struct Scope
			{
				Scope() { Vpv::Hub::ExtensionHub::Instance().ResetModulesForTesting(); Vpv::Hub::ResetThisThreadFrameForTesting(); }
				~Scope()
				{
					Vpv::Hub::ExtensionHub::Instance().ResetModulesForTesting();
					Vpv::Hub::ResetThisThreadFrameForTesting();
					VpDetailRestore::DefaultSceneReader() = &VpDetailRestore::ReadSceneFromSkeleton;
				}
			} scope;
			int renderer = 0;
			FrameAnalysis::Store offStore(FrameAnalysis::RingDepth(4, 4));
			offStore.SetEnabled(false);   // what frame_analysis_store=false leaves
			VpFrameAnalysis::SourceFrameTap::SetStore(&renderer, &offStore);
			struct StoreScope { const void* h; ~StoreScope() { VpFrameAnalysis::SourceFrameTap::SetStore(h, nullptr); } } storeScope{ &renderer };
			Assert::IsTrue(VpDetailRestore::DefaultSceneReader() == &VpDetailRestore::ReadSceneFromSkeleton,
				L"production default: the skeleton's scene product, not the store Peek");

			// No skeleton module yet: a visible restart with its cause.
			RendererExtension::FrameContext c;
			c.host = &renderer;
			c.phase = RendererExtension::FramePhase::Entrance;
			c.seq = 12;
			Vpv::Hub::RecordCurrentFrame(c);
			g_plan = ReaderPlan();
			g_plan.fadeFrom = 5; g_plan.fadeTo = 10; g_plan.fadeKeep = 0.6f; g_plan.cutAt = 12;
			const VpDetailRestore::SceneRestartAnswer noModule = VpDetailRestore::SceneRestartOf(12, nullptr);
			Assert::IsTrue(noModule.restart && !noModule.fromRecord, L"no skeleton module: restart, counted as missing");
			Assert::IsTrue(std::string(VpDetailRestore::SceneUnavailableWhy()).find("no skeleton module") != std::string::npos,
				Wide(VpDetailRestore::SceneUnavailableWhy()).c_str());

			LiveSceneSkeleton* sk = new LiveSceneSkeleton();
			sk->unknownAt = 14;
			Vpv::Hub::ExtensionHub::Instance().AddModule(std::unique_ptr<Vpv::Hub::ISkeletonModule>(sk));
			FrameAnalysis::Scene peeked;
			Assert::IsFalse(VpDetailRestore::ReadSceneFromStore(12, peeked), L"store off: a Peek finds no scene group");

			// The cut: a full restart, answered by the skeleton for THIS renderer and frame.
			const VpDetailRestore::SceneRestartAnswer cut = VpDetailRestore::SceneRestartOf(12, nullptr);
			Assert::IsTrue(cut.restart && cut.fromRecord, L"store off: the cut still restarts, from the skeleton's answer");
			Assert::AreEqual(0.0f, cut.historyKeep);
			Assert::AreEqual(size_t(1), sk->asks.size(), L"one SceneOf call per frame");
			Assert::IsTrue(sk->asks[0].first == &renderer && sk->asks[0].second == 12u, L"asked for this thread's renderer and the frame");

			// A fade: the partial reset, no restart.
			const VpDetailRestore::SceneRestartAnswer fade = VpDetailRestore::SceneRestartOf(7, nullptr);
			Assert::IsTrue(!fade.restart && fade.fadePartial && fade.fromRecord, L"store off: a fade is still a partial reset");
			Assert::AreEqual(0.6f, fade.historyKeep);
			const VpDetailRestore::SceneRestartAnswer still = VpDetailRestore::SceneRestartOf(11, nullptr);
			Assert::IsTrue(!still.restart && !still.fadePartial && still.fromRecord, L"no cut, no fade: the history is kept");

			// A frame the skeleton cannot answer: a visible restart with the skeleton's cause (a genuinely missing result).
			const VpDetailRestore::SceneRestartAnswer unknown = VpDetailRestore::SceneRestartOf(14, nullptr);
			Assert::IsTrue(unknown.restart && !unknown.fromRecord, L"not answered: restart, counted as missing");
			Assert::IsTrue(std::string(VpDetailRestore::SceneUnavailableWhy()).find("could not classify") != std::string::npos,
				Wide(VpDetailRestore::SceneUnavailableWhy()).c_str());

			// The CPU models with the production reader: the cut restarts (gain 1), the fade frames are partial.
			DetailPulseModel::Settings s;
			s.sourceHz = 23.976;
			s.colorDepth = 10;
			s.sceneReader = &VpDetailRestore::ReadSceneFromSkeleton;
			g_plan = ReaderPlan();
			g_plan.fadeFrom = 8; g_plan.fadeTo = 21; g_plan.fadeKeep = 0.7f; g_plan.cutAt = 25;
			sk->unknownAt = 0;
			DetailPulseModel::Model dp;
			TextureRestoreModel::Model tr;
			for (int k = 0; k < 30; ++k)
			{
				const uint64_t seq = uint64_t(k) + 1;
				const GrainModel::Image g = Textured(96, 64, uint32_t(k));
				const DetailPulseModel::Frame f = dp.Process(g, seq, s);
				TextureRestoreModel::Image im(96, 64);
				for (size_t i = 0; i < im.v.size(); ++i) im.v[i] = g.v[i];
				TextureRestore::Params p;
				p.colorDepth = 10; p.sourceHz = 23.976; p.bitrateQuality = 1.0; p.sequence = seq;
				p.sceneReader = &VpDetailRestore::ReadSceneFromSkeleton;
				tr.Frame(im, p);
				if (seq == 25)
				{
					Assert::IsTrue(f.restart, L"detail pulse: store off, the cut restarts");
					Assert::AreEqual(1.0, f.g1);
					Assert::AreEqual(1.0, f.g2);
					Assert::IsTrue(tr.LastWasCut(), L"texture restore: store off, the cut restarts");
				}
				else if (seq >= 8 && seq < 21)
				{
					Assert::IsTrue(!f.restart && f.fadePartial, L"detail pulse: store off, a fade frame is partial");
					Assert::IsTrue(!tr.LastWasCut() && tr.LastWasFadePartial(), L"texture restore: store off, a fade frame is partial");
				}
			}
		}

	private:
		static std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }
	};
}
