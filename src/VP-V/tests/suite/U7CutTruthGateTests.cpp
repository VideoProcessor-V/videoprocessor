// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// WAIVER-U7ACC-GATE: the permanent U7 cut truth gate (U7-accuracy, rule 28: the gate is never lowered). The shared scene/cut law
// (VpUnit::CutFadeJudge::CutTracker, U7 v2, the T2 sequence law) is run over the P4 record sequence of real clips, exactly as the
// renderer feeds it: decode (RealClipLibrary, 8-bit gray), the 10-bit view (msbShift 6), P1 half luma + P4 source stats at the clip
// size (the lane's own functions), CutTracker::Push per frame in order, one continuous part per clip. Only CONFIRMED verdicts count
// (the delivered cut, owner ruling Q3: one frame late is allowed); the last frame of a clip has no successor and is not judged.
//
// Truth (owner ruling Q1: the COMBINED truth): frame labels of tos_24p_3min, cosmos_23976p_3min and meridian_2997p_3min made by
// an opus agent's frame review, not by a person (vp-handover lead17, folder u7acc-truth; candidate frames partly from the prototype law;
// for tos the manifest cuts_s list is wrong: 1315 missing, 1778 and 1946 not cuts). A truth EVENT is a group of frames; it is found when any frame of the group is called a cut. A false cut
// is a cut on a frame outside every event and outside the dont-care frames (fades, dissolves, ambiguous frames).
// Gate on the combined set: recall >= 99.5 % and precision >= 99.5 %. Zero cuts on the cut-free clips: pan_480pxs_23976p,
// pan_1440pxs_5994p, pan_480pxs_fade_23976p, rate_24, and the synthetic flash and fade (rate_24 picture, the D8 harness's rows).
// False cuts per minute and per hour are logged for both sets.
// Frame numbers below are 0-based decoder frame indices. No pixels are stored; the clips come from VP_TEST_CLIP_MANIFEST.
// Skips VISIBLY (SKIP-NO-FOOTAGE, a failure under VP_TEST_REQUIRE_FOOTAGE / _ALL) when the manifest, ffmpeg or a clip is missing.
// CPU only (no GPU, no store).

#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/tests/support/HardwarePrerequisites.h>
#include <VP-V/tests/support/RealClipLibrary.h>

#include <VP-V/modules/input/source_analysis/events/shared/CutFadeJudge.h>
#include <VP-V/modules/input/source_analysis/lane/LaneCpuProducts.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace U7CutTruthGateTest
{
	namespace HP = HardwarePrerequisites;

	constexpr double kRecallGatePct = 99.5;      // owner's value; never lowered (rule 28)
	constexpr double kPrecisionGatePct = 99.5;   // owner's value; never lowered (rule 28)
	constexpr int kSyntheticFrames = 240;

	enum class Synthetic { None, Fade, Flash };

	inline std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }

	// The D8 harness's synthetic rows (D8SceneJudgeAgreementTests.cpp), same pictures: fade to black and back, single bright frames.
	inline double FadeGain(int k)
	{
		if (k < 40) return 1.0;
		if (k < 88) return 1.0 - (k - 40 + 1) / 48.0;
		if (k < 112) return 0.0;
		if (k < 160) return (k - 112 + 1) / 48.0;
		return 1.0;
	}
	inline bool FlashFrame(int k) { return k % 24 == 12; }

	inline void ApplySynthetic(Synthetic kind, int k, std::vector<uint8_t>& luma)
	{
		if (kind == Synthetic::Fade)
		{
			const double g = FadeGain(k);
			for (uint8_t& p : luma)
			{
				const double v = 16.0 + (static_cast<double>(p) - 16.0) * g;
				p = static_cast<uint8_t>((std::max)(0.0, (std::min)(255.0, std::floor(v + 0.5))));
			}
		}
		else if (kind == Synthetic::Flash && FlashFrame(k))
		{
			for (uint8_t& p : luma)
				p = static_cast<uint8_t>((std::min)(255, static_cast<int>(p) + 100));
		}
	}

	struct Truth
	{
		std::vector<std::vector<int>> events;   // each event: the frames any of which may carry the cut
		std::set<int> dontCare;
		void Singles(const std::vector<int>& frames) { for (int f : frames) events.push_back({ f }); }
		void Range(int a, int b) { for (int k = a; k <= b; ++k) dontCare.insert(k); }
	};

	inline Truth TosTruth()
	{
		Truth t;
		t.Singles({ 24, 178, 269, 425, 461, 556, 788, 884, 1063, 1315, 1632 });
		t.events.push_back({ 2324, 2325 });
		t.events.push_back({ 2418, 2419 });
		t.Range(1785, 1788);
		t.Range(2550, 2625);
		for (int f : { 1246, 1251, 1252, 1665, 1666, 1667, 1863, 1864, 2029, 2183 }) t.dontCare.insert(f);
		const int fades[][2] = { { 186, 200 }, { 236, 268 }, { 1655, 1660 }, { 2034, 2039 }, { 2040, 2045 }, { 2171, 2178 },
			{ 2318, 2323 }, { 2411, 2417 } };
		for (const auto& r : fades) t.Range(r[0], r[1]);
		return t;
	}

	inline Truth CosmosTruth()
	{
		Truth t;
		t.Singles({ 101, 137, 187, 246, 288, 375, 407, 492, 582, 661, 695, 736, 843, 1009, 1101, 1179, 1392, 1493, 1634, 1735, 1970,
			2023, 2093, 2368, 2597, 2629, 2718, 2771, 2809, 3232, 3299, 3428, 3533, 3635, 3683, 3830, 3899, 3982, 4107, 4165, 4284 });
		t.Range(3713, 3728);
		return t;
	}

	inline Truth MeridianTruth()
	{
		Truth t;
		t.Singles({ 296, 452, 558, 619, 871, 937, 1301, 1419, 1535, 1668, 2267, 2348, 2566, 2874, 3030, 3239, 3435, 4445, 4508, 4901,
			4979, 5096, 5269 });
		return t;
	}

	// WAIVER-U7ACC-GATE-LOG (lead 18 orders A4 + C, logging only: no assertion changes). The per-frame evidence of the confirmed
	// verdicts (for the firing margins) and the v1 pair law (ClassifyEvents) over the same consecutive records (the before/after).
	struct FrameEvidence
	{
		int frame = 0;
		bool cut = false, candidate = false, gridLaw = false;
		double L = 0.0, bar = 0.0, H = 0.0, G = 0.0;
		// How close the frame came to the candidate rule: the spike tier needs L >= kSpikeL and L >= bar, the low tier L >= kLowTierL
		// and L >= bar (with S < kLowTierS and no flat frame, not checked here: an upper bound), the hard tier H > kHardH and G >= kHardG;
		// 1 = at the bar of the nearest tier. It does not see the veto stage (flash / lighting): `vetoed` counts those candidates.
		double Fire() const
		{
			using namespace VpUnit::CutFadeJudge;
			const double spikeBar = bar > 0.0 ? L / bar : 0.0;
			const double spikeL = L / kLowTierL;
			const double spike = spikeL < spikeBar ? spikeL : spikeBar;
			const double hardH = H / kHardH, hardG = G / kHardG;
			const double hard = hardH < hardG ? hardH : hardG;
			return spike > hard ? spike : hard;
		}
	};

	struct ClipRun
	{
		bool ran = false;
		int frames = 0;
		double seconds = 0.0;
		std::vector<int> cuts, flashes;   // confirmed verdicts, 0-based frame
		std::vector<FrameEvidence> evidence;   // every confirmed grid-law verdict
		std::vector<int> v1Cuts, v1Flashes, fades;   // the v1 pair law on (k-1, k); fades = the pair fade (unchanged in v2)
		int noGrid = 0;   // confirmed verdicts given by the no-grid fallback (gridLaw false, not a part start)
	};

	// Runs the clip (or its synthetic row) through P1 -> P4 -> CutTracker. ran = false: skipped visibly.
	inline ClipRun RunClip(const char* test, const char* clipId, Synthetic synthetic)
	{
		ClipRun run;
		const RealClip::Manifest manifest = RealClip::Manifest::Load();
		if (!HP::RequireOrSkip(HP::Kind::Footage, test, manifest.Loaded(), "clip manifest not loaded: " + manifest.error))
			return run;
		if (!HP::RequireOrSkip(HP::Kind::Footage, test, RealClip::FileExists(manifest.ffmpeg), "ffmpeg not found at " + manifest.ffmpeg))
			return run;
		const RealClip::Clip* clip = manifest.Find(clipId);
		if (!HP::RequireOrSkip(HP::Kind::Footage, test, clip != nullptr && RealClip::FileExists(manifest.FullPath(*clip)),
				std::string("clip ") + clipId + " not in the manifest or its file is missing (" + manifest.path + ")"))
			return run;
		const double hz = clip->RateHz();
		Assert::IsTrue(hz > 0.0, Wide(std::string("U7 gate: clip ") + clipId + " has no rate").c_str());
		const int total = synthetic == Synthetic::None ? clip->frames : (std::min)(kSyntheticFrames, clip->frames);
		Assert::IsTrue(total >= 2, L"U7 gate: the clip has frames");
		const int W = clip->width, H = clip->height;

		RealClip::Decoder decoder;
		std::string error;
		Assert::IsTrue(decoder.Open(manifest, *clip, "", error), Wide("U7 gate decode: " + error).c_str());

		const VpLaneProducts::Dims dims{ static_cast<uint32_t>(W), static_cast<uint32_t>(H) };
		std::vector<uint8_t> luma, half(VpLaneProducts::HalfLumaBytes(dims)), stats(VpLaneProducts::kStatsBytes);
		std::vector<uint16_t> view(static_cast<size_t>(W) * H);
		VpLaneProducts::SourceStatsRecord cur, prev;
		bool havePrev = false;
		VpUnit::CutFadeJudge::CutTracker tracker;
		tracker.Reset();
		for (int k = 0; k < total; ++k)
		{
			Assert::IsTrue(decoder.Next(luma), Wide("U7 gate: the clip ended early at frame " + std::to_string(k) + " of " + clipId).c_str());
			ApplySynthetic(synthetic, k, luma);
			for (size_t i = 0; i < view.size(); ++i) view[i] = static_cast<uint16_t>(luma[i] * 4u);
			VpSkeleton::SourceFrameView v;
			v.seq = static_cast<uint64_t>(k) + 1;
			v.luma = view.data(); v.width = dims.width; v.height = dims.height; v.strideSamples = dims.width; v.msbShift = 6;
			Assert::IsTrue(VpLaneProducts::ComputeHalfLuma(v, half.data(), half.size()), L"U7 gate: P1");
			Assert::IsTrue(VpLaneProducts::ComputeSourceStats(half.data(), dims, stats.data(), stats.size()), L"U7 gate: P4");
			Assert::IsTrue(VpLaneProducts::ReadSourceStats(stats.data(), stats.size(), &cur), L"U7 gate: P4 read back");
			if (havePrev)
			{
				const VpLaneProducts::EventSignals e = VpLaneProducts::ClassifyEvents(prev, cur, false, false);   // WAIVER-U7ACC-GATE-LOG
				if (e.cut) run.v1Cuts.push_back(k);
				if (e.flash) run.v1Flashes.push_back(k);
				if (e.fade) run.fades.push_back(k);
			}
			prev = cur;
			havePrev = true;
			const VpUnit::CutFadeJudge::CutVerdict verdict = tracker.Push(v.seq, cur);
			if (!verdict.known) continue;
			Assert::IsTrue(verdict.confirmed && verdict.seq + 1u == v.seq, L"U7 gate: Push returns the confirmed verdict of the previous frame");
			const int frame = static_cast<int>(verdict.seq) - 1;
			if (verdict.cut) run.cuts.push_back(frame);
			if (verdict.flash) run.flashes.push_back(frame);
			if (verdict.gridLaw)
			{
				FrameEvidence ev;   // WAIVER-U7ACC-GATE-LOG
				ev.frame = frame; ev.cut = verdict.cut; ev.candidate = verdict.candidate; ev.gridLaw = true;
				ev.L = verdict.score.L; ev.bar = verdict.bar; ev.H = verdict.score.H; ev.G = verdict.score.G;
				run.evidence.push_back(ev);
			}
			else if (!verdict.partStart)
				++run.noGrid;
		}
		decoder.Close();
		run.ran = true;
		run.frames = total;
		run.seconds = total / hz;
		return run;
	}

	struct Score
	{
		int events = 0, found = 0, falseCuts = 0, frames = 0;
		double seconds = 0.0;
		std::string missed, falses;
		void Add(const ClipRun& run, const Truth& truth)
		{
			frames += run.frames;
			seconds += run.seconds;
			const std::set<int> cuts(run.cuts.begin(), run.cuts.end());
			std::set<int> eventFrames;
			for (const std::vector<int>& e : truth.events)
			{
				++events;
				bool hit = false;
				for (int f : e)
				{
					eventFrames.insert(f);
					hit = hit || cuts.count(f) != 0;
				}
				if (hit) ++found;
				else missed += " " + std::to_string(e.front());
			}
			for (int c : run.cuts)
				if (!eventFrames.count(c) && !truth.dontCare.count(c))
				{
					++falseCuts;
					falses += " " + std::to_string(c);
				}
		}
		double RecallPct() const { return events > 0 ? 100.0 * found / events : 0.0; }
		double PrecisionPct() const { return found + falseCuts > 0 ? 100.0 * found / (found + falseCuts) : 0.0; }
		double PerMinute() const { return seconds > 0.0 ? falseCuts * 60.0 / seconds : 0.0; }
		double PerHour() const { return seconds > 0.0 ? falseCuts * 3600.0 / seconds : 0.0; }
	};

	inline std::string Line(const char* name, const Score& s)
	{
		char text[512];
		snprintf(text, sizeof(text), "%s: %d frames (%.1f s), events %d, found %d, recall %.3f %%, false cuts %d, precision %.3f %%, "
			"false cuts %.4f / min, %.3f / h", name, s.frames, s.seconds, s.events, s.found, s.RecallPct(), s.falseCuts,
			s.PrecisionPct(), s.PerMinute(), s.PerHour());
		return std::string(text) + (s.missed.empty() ? "" : "; missed:" + s.missed) + (s.falses.empty() ? "" : "; false:" + s.falses);
	}

	// WAIVER-U7ACC-GATE-LOG: the owner's before/after (v1 pair law vs the v2 sequence law), the firing margins (lead 18 order A4) and the
	// per-consumer reset counts (order C), one line per row. Logging only. Resets: DetailPulse / TextureRestore / temporal detail restart
	// on cut or fade (SceneRestart.h, TemporalDetailModel.h); the camera model and the classifier on the delivered cut; one vector pair
	// is refused per confirmed cut (MotionValidity, v2 only).
	inline std::string Diagnostics(const char* name, const ClipRun& run, const Truth& truth)
	{
		ClipRun v1 = run;
		v1.cuts = run.v1Cuts;
		Score s1, s2;
		s1.Add(v1, truth);
		s2.Add(run, truth);
		std::set<int> eventFrames;
		for (const std::vector<int>& e : truth.events)
			for (int f : e)
				eventFrames.insert(f);
		double highestNonCut = 0.0, weakestCut = 1e9;
		int highestFrame = -1, weakestFrame = -1, vetoed = 0;
		for (const FrameEvidence& ev : run.evidence)
		{
			const bool inTruth = eventFrames.count(ev.frame) != 0, dontCare = truth.dontCare.count(ev.frame) != 0;
			if (ev.cut)
			{
				if (inTruth && ev.Fire() < weakestCut) { weakestCut = ev.Fire(); weakestFrame = ev.frame; }
				continue;
			}
			if (inTruth || dontCare)
				continue;
			if (ev.candidate) ++vetoed;
			if (ev.Fire() > highestNonCut) { highestNonCut = ev.Fire(); highestFrame = ev.frame; }
		}
		std::set<int> v2Restart(run.cuts.begin(), run.cuts.end()), v1Restart(run.v1Cuts.begin(), run.v1Cuts.end());
		for (int f : run.fades) { v2Restart.insert(f); v1Restart.insert(f); }
		const double hours = run.seconds / 3600.0;
		char text[1024];
		snprintf(text, sizeof(text), "U7DIAG %s: v1 found %d/%d false %d (%.1f / h) flashes %d | v2 found %d/%d false %d (%.1f / h) flashes %d | "
			"fades %d | margin: highest non-cut fire %.3f at %d (%d vetoed candidates), weakest real cut fire %.3f at %d | "
			"resets v1 -> v2: DP/TR/TD %d -> %d, camera/classifier %d -> %d, refused vector pairs 0 -> %d | no-grid verdicts %d",
			name, s1.found, s1.events, s1.falseCuts, hours > 0.0 ? s1.falseCuts / hours : 0.0, int(run.v1Flashes.size()),
			s2.found, s2.events, s2.falseCuts, hours > 0.0 ? s2.falseCuts / hours : 0.0, int(run.flashes.size()), int(run.fades.size()),
			highestNonCut, highestFrame, vetoed, weakestFrame >= 0 ? weakestCut : 0.0, weakestFrame,
			int(v1Restart.size()), int(v2Restart.size()), int(run.v1Cuts.size()), int(run.cuts.size()), int(run.cuts.size()), run.noGrid);
		return std::string(text);
	}

	TEST_CLASS(U7CutTruthGateTests)
	{
	public:
		// The combined truth set (Q1): recall and precision >= 99.5 %.
		TEST_METHOD(U7Gate_CombinedTruthRecallAndPrecision)
		{
			const char* test = "U7Gate_CombinedTruthRecallAndPrecision";
			struct Row { const char* id; Truth truth; };
			const Row rows[] = { { "tos_24p_3min", TosTruth() }, { "cosmos_23976p_3min", CosmosTruth() },
				{ "meridian_2997p_3min", MeridianTruth() } };
			Score combined;
			for (const Row& row : rows)
			{
				const ClipRun run = RunClip(test, row.id, Synthetic::None);
				if (!run.ran) return;   // skipped visibly (SKIP-NO-FOOTAGE)
				Score own;
				own.Add(run, row.truth);
				combined.Add(run, row.truth);
				Logger::WriteMessage(("U7GATE " + Line(row.id, own) + "\n").c_str());
				Logger::WriteMessage((Diagnostics(row.id, run, row.truth) + "\n").c_str());   // WAIVER-U7ACC-GATE-LOG
			}
			const std::string text = "U7GATE " + Line("combined truth", combined);
			Logger::WriteMessage((text + "\n").c_str());
			Assert::IsTrue(combined.events > 0, L"U7 gate: the truth has events");
			Assert::IsTrue(combined.RecallPct() >= kRecallGatePct, Wide(text + " (gate: recall >= 99.5 %)").c_str());
			Assert::IsTrue(combined.PrecisionPct() >= kPrecisionGatePct, Wide(text + " (gate: precision >= 99.5 %)").c_str());
		}

		// Pan, flash and fade: ZERO cuts.
		TEST_METHOD(U7Gate_ZeroCutsOnPanFlashFade)
		{
			const char* test = "U7Gate_ZeroCutsOnPanFlashFade";
			struct Row { const char* name; const char* id; Synthetic synthetic; };
			const Row rows[] = { { "flash_synthetic", "rate_24", Synthetic::Flash }, { "fade_synthetic", "rate_24", Synthetic::Fade },
				{ "rate_24", "rate_24", Synthetic::None }, { "pan_480pxs_23976p", "pan_480pxs_23976p", Synthetic::None },
				{ "pan_1440pxs_5994p", "pan_1440pxs_5994p", Synthetic::None },
				{ "pan_480pxs_fade_23976p", "pan_480pxs_fade_23976p", Synthetic::None } };
			Score all;
			std::string red;
			for (const Row& row : rows)
			{
				const ClipRun run = RunClip(test, row.id, row.synthetic);
				if (!run.ran)
				{
					// Skipped visibly (SKIP-NO-FOOTAGE); cuts already seen on earlier rows still fail the gate.
					Assert::AreEqual(0, all.falseCuts, Wide("U7GATE " + Line("cut-free set (partial)", all) + " (gate: zero cuts; red rows:" +
						red + ")").c_str());
					return;
				}
				Score own;
				own.Add(run, Truth());
				all.Add(run, Truth());
				Logger::WriteMessage(("U7GATE " + Line(row.name, own) + ", flashes " + std::to_string(run.flashes.size()) + "\n").c_str());
				Logger::WriteMessage((Diagnostics(row.name, run, Truth()) + "\n").c_str());   // WAIVER-U7ACC-GATE-LOG
				if (own.falseCuts != 0) red += std::string(" ") + row.name;
			}
			const std::string text = "U7GATE " + Line("cut-free set", all);
			Logger::WriteMessage((text + "\n").c_str());
			Assert::AreEqual(0, all.falseCuts, Wide(text + " (gate: zero cuts; red rows:" + red + ")").c_str());
		}
	};
}
