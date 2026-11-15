// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// WAIVER-C5-SOFTRESET-ON (plan D:\codex\vp-handover\lead19\c5-softreset.md item 5; review batchB\review-softreset-r1.md M2 / M3):
// cut_soft_reset, the soft reset of detail pulse and texture restore on a high-confidence non-cut candidate (owner 2026-11-15 08:35
// EST, question window U7 Q4: "need the bar fairly high here. Mid scene firing is a major problem"; settings register row 9).
//   - default ON pinned: VpDetailRestore::kDefaultCutSoftReset, DetailPulse::Params().softReset, TextureRestore::Params().softReset,
//     the renderer's loaded default (RendererSettings::cutSoftReset through the settings dump) and the vpv-tiers default;
//   - off: the same input gives no soft reset and exactly the pictures of a stream without the candidate (CPU models, the modules'
//     mirrors);
//   - on: it fires only for a CONFIRMED candidate with strength >= the bar; never provisionally, on a cut, a flash, a fade frame or
//     at 0.99; never on cut-free clips (pan, flash, static) judged by the real U7 law (CutTracker on P4 records);
//   - keep is 0.5 at strength 1, and a cut still restarts;
//   - the status text: ", soft reset: off" when off, ", soft reset: on (inactive: provisional verdicts only)" when the scene groups
//     stay provisional (M3, no silent fallbacks).
// CPU only, except SettingsDefaultOnThroughTheLoader (the renderer DLL's settings exports, no device).

#include "../../../../tests/SettingsTestSupport.h"

#include <VP-V/core/FrameAnalysisStore.h>
#include <VP-V/modules/control/config/config_pages/VpvTierTable.generated.h>
#include <VP-V/modules/input/source_analysis/events/shared/CutFadeJudge.h>
#include <VP-V/modules/input/source_analysis/lane/shared/SourceStats.h>
#include <VP-V/modules/repair/texture_restore/DetailPulse.h>
#include <VP-V/modules/repair/texture_restore/SceneRestart.h>
#include <VP-V/modules/repair/texture_restore/TextureRestore.h>
#include <VP-V/tests/support/DetailPulseModel.h>
#include <VP-V/tests/support/TextureRestoreModel.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace SoftResetTestsNs
{
	using Record = VpUnit::SourceStats::Record;

	std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }

	// --- an injected scene reader (a function pointer: the plan is file-static) ---
	// Every frame's verdict is CONFIRMED (an ordinary lane) unless `allProvisional`; the soft candidate is a confirmed candidate
	// that is neither cut nor flash, with `softStrength`.
	struct Plan
	{
		uint64_t softAt = 0;          // the candidate (0 = none)
		float softStrength = 1.0f;
		bool softConfirmed = true;
		uint64_t cutAt = 0;           // the delivered cut (cut + cutHere)
		uint64_t cutHereAt = 0;       // the frame's own cut verdict, not yet delivered (cutHere only)
		uint64_t flashAt = 0;         // a flash candidate
		uint64_t fadeFrom = 0, fadeTo = 0;   // [from, to): fade frames
		float fadeKeep = 0.8f;
		bool allProvisional = false;  // every group provisional (low latency, a thin lane)
		uint64_t missingAt = 0;       // a frame without a group
	};
	Plan g_plan;

	bool PlanReader(uint64_t seq, FrameAnalysis::Scene& s)
	{
		s = FrameAnalysis::Scene();
		if (seq == g_plan.missingAt)
			return false;
		s.origin = FrameAnalysis::SceneOrigin::Vp;
		s.fadeKeep = 1.0f;
		s.confirmed = !g_plan.allProvisional;
		if (seq >= g_plan.fadeFrom && seq < g_plan.fadeTo)
		{
			s.fade = true;
			s.fadeKeep = g_plan.fadeKeep;
		}
		if (seq == g_plan.softAt)
		{
			s.cutPossible = true;
			s.cutStrength = g_plan.softStrength;
			s.confirmed = g_plan.softConfirmed && !g_plan.allProvisional;
		}
		if (seq == g_plan.cutAt)
		{
			s.cut = true;
			s.cutHere = true;
			s.cutPossible = true;
			s.cutStrength = 1.0f;
		}
		if (seq == g_plan.cutHereAt)
		{
			s.cutHere = true;
			s.cutPossible = true;
			s.cutStrength = 1.0f;
		}
		if (seq == g_plan.flashAt)
		{
			s.flash = true;
			s.cutPossible = true;
			s.cutStrength = 1.0f;
		}
		return true;
	}

	// --- real U7 verdicts on synthetic P4 records (the shape of U7FadePartialResetTests' pictures) ---
	constexpr uint32_t kW = 160, kH = 90;
	constexpr double kBlack = 4096.0;

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

	// The scene groups the real law gives a clip (confirmed verdicts only, the cut delivered at its frame as with its successor
	// present), indexed by seq - 1; known[i] = a confirmed verdict exists.
	std::vector<FrameAnalysis::Scene> g_scenes;
	std::vector<char> g_known;

	bool ClipReader(uint64_t seq, FrameAnalysis::Scene& s)
	{
		if (seq == 0 || seq > g_scenes.size() || !g_known[size_t(seq - 1)])
			return false;
		s = g_scenes[size_t(seq - 1)];
		return true;
	}

	struct ClipCounts { int verdicts = 0, candidates = 0, cuts = 0, flashes = 0, fades = 0; };

	ClipCounts JudgeClip(const std::vector<Record>& recs)
	{
		ClipCounts c;
		g_scenes.assign(recs.size(), FrameAnalysis::Scene());
		g_known.assign(recs.size(), 0);
		std::vector<char> fade(recs.size(), 0);
		std::vector<float> keep(recs.size(), 1.0f);
		VpUnit::CutFadeJudge::FadeRun run;
		for (size_t k = 1; k < recs.size(); ++k)
		{
			const VpUnit::CutFadeJudge::EventSignals e = VpUnit::CutFadeJudge::ClassifyEvents(recs[k - 1], recs[k], false, false);
			double runKeep = 1.0;
			const bool runFade = VpUnit::CutFadeJudge::FoldFadeRun(recs[k - 1], recs[k], e, run, &runKeep);
			keep[k] = static_cast<float>(VpUnit::CutFadeJudge::FrameFadeKeep(recs[k - 1], recs[k], e.fade, runFade, runKeep));
			fade[k] = (runFade || e.fade) ? 1 : 0;
		}
		VpUnit::CutFadeJudge::CutTracker tracker;
		for (size_t k = 0; k < recs.size(); ++k)
		{
			const VpUnit::CutFadeJudge::CutVerdict v = tracker.Push(uint64_t(k) + 1, recs[k]);
			if (!v.known || v.seq == 0 || v.seq > recs.size())
				continue;
			const size_t i = size_t(v.seq - 1);
			FrameAnalysis::Scene& s = g_scenes[i];
			s.origin = FrameAnalysis::SceneOrigin::Vp;
			s.confirmed = v.confirmed;
			s.cut = v.cut;
			s.cutHere = v.cut;
			s.flash = v.flash;
			s.cutPossible = v.candidate;
			s.cutStrength = v.strength;
			s.fade = fade[i] != 0;
			s.fadeKeep = keep[i];
			g_known[i] = 1;
			++c.verdicts;
			c.candidates += v.candidate ? 1 : 0;
			c.cuts += v.cut ? 1 : 0;
			c.flashes += v.flash ? 1 : 0;
			c.fades += s.fade ? 1 : 0;
		}
		return c;
	}

	// A cut-free clip judged by the real law: no frame fires, and switch on and off give the same answers.
	void AssertNoSoftReset(const std::vector<Record>& recs, const char* name)
	{
		const ClipCounts c = JudgeClip(recs);
		char text[240];
		snprintf(text, sizeof(text), "%s: %d confirmed verdicts, %d candidates, %d cuts, %d flashes, %d fade frames", name, c.verdicts,
			c.candidates, c.cuts, c.flashes, c.fades);
		Logger::WriteMessage(text);
		Assert::IsTrue(c.verdicts >= int(recs.size()) - 2, Wide(text).c_str());
		for (size_t i = 0; i < recs.size(); ++i)
		{
			if (!g_known[i])
				continue;
			const FrameAnalysis::Scene& s = g_scenes[i];
			const std::string where = std::string(text) + ", frame " + std::to_string(i + 1);
			Assert::AreEqual(0.0f, VpUnit::CutFadeJudge::SoftResetStrength(s.confirmed, s.cutPossible, s.cut || s.cutHere, s.flash, s.fade,
				s.cutStrength), Wide("the rule never fires on a cut-free clip: " + where).c_str());
			const VpDetailRestore::SceneRestartAnswer on = VpDetailRestore::SceneRestartOf(uint64_t(i) + 1, &ClipReader, true);
			const VpDetailRestore::SceneRestartAnswer off = VpDetailRestore::SceneRestartOf(uint64_t(i) + 1, &ClipReader, false);
			Assert::IsFalse(on.softPartial, Wide("no soft reset on a cut-free clip: " + where).c_str());
			Assert::AreEqual(off.restart, on.restart, Wide(where).c_str());
			Assert::AreEqual(off.fadePartial, on.fadePartial, Wide(where).c_str());
			Assert::AreEqual(off.historyKeep, on.historyKeep, Wide(where).c_str());
		}
	}

	// A textured plane (codes of a 10-bit signal), the same picture every frame with its own fine noise.
	inline GrainModel::Image Textured(int w, int h, uint32_t k)
	{
		GrainModel::Image im(w, h);
		for (int y = 0; y < h; ++y)
			for (int x = 0; x < w; ++x)
				im.at(x, y) = 400.0 + 60.0 * std::sin(0.37 * x) * std::cos(0.23 * y) + 0.02 * Noise(uint32_t(x), uint32_t(y), k);
		return im;
	}

	TEST_CLASS(SoftResetTests)
	{
	public:
		// Default ON (owner U7 Q4 / register row 9) wherever the default lives.
		TEST_METHOD(DefaultOnIsPinned)
		{
			Assert::IsTrue(VpDetailRestore::kDefaultCutSoftReset, L"kDefaultCutSoftReset is on");
			Assert::IsTrue(DetailPulse::Params().softReset, L"detail pulse: Params().softReset defaults on");
			Assert::IsTrue(TextureRestore::Params().softReset, L"texture restore: Params().softReset defaults on");
			Assert::IsTrue(DetailPulseModel::Settings().softReset, L"the detail pulse CPU model mirrors the module's default");

			std::string tierDefault = "(missing)";
			for (const Vpv::KeyValue& kv : Vpv::TierTable().defaults)
				if (std::string(kv.key) == "cut_soft_reset")
					tierDefault = kv.value;
			Assert::AreEqual(std::string("on"), tierDefault, L"the vpv-tiers default (VpvTierTable.generated.h) is on");
			const std::string json = SettingsTestSupport::ReadFile(SettingsTestSupport::RepoRoot() + "docs\\design\\vpv-tiers.json");
			Assert::IsTrue(json.find("\"cut_soft_reset\": \"on\"") != std::string::npos, L"docs/design/vpv-tiers.json defaults it on");

			bool toggle = false;
			for (const Vpv::Unit& unit : Vpv::TierTable().units)
				for (const Vpv::Toggle& t : unit.toggles)
					if (std::string(t.key) == "cut_soft_reset")
					{
						toggle = true;
						Assert::AreEqual(size_t(2), t.values.size(), L"the toggle is on / off");
						Assert::AreEqual(std::string("on"), std::string(t.values[0]));
						Assert::AreEqual(std::string("off"), std::string(t.values[1]));
					}
			Assert::IsTrue(toggle, L"the VP-V Motion toggle exists");
		}

		// RendererSettings::cutSoftReset through the renderer's own loader: absent = on; "off" is read.
		TEST_METHOD(SettingsDefaultOnThroughTheLoader)
		{
			SettingsTestSupport::Renderer renderer;
			Assert::IsTrue(renderer.Ready(), L"the renderer DLL and its settings exports");
			const SettingsTestSupport::Evaluation empty = renderer.Evaluate(SettingsTestSupport::Base());
			Assert::IsTrue(empty.ok, L"the base configuration loads");
			bool present = false;
			Assert::AreEqual(std::string("on"), SettingsTestSupport::DescribeValue(empty.describe, "cut_soft_reset", &present),
				L"RendererSettings().cutSoftReset: on when the key is absent");
			Assert::IsTrue(present, L"the settings dump has the key");
			const SettingsTestSupport::Evaluation off = renderer.Evaluate(SettingsTestSupport::Base() + "cut_soft_reset: off\n");
			Assert::IsTrue(off.ok, L"the configuration with the key loads");
			Assert::AreEqual(std::string("off"), SettingsTestSupport::DescribeValue(off.describe, "cut_soft_reset"), L"off is read");
			Assert::IsTrue(off.fingerprint != empty.fingerprint, L"the switch is part of the effective-settings fingerprint");
		}

		// The rule (U7, CutFadeJudge): only a confirmed candidate that is neither cut nor flash nor a fade frame, at strength >= the bar.
		TEST_METHOD(TheRuleFiresOnlyOnAConfirmedCandidateAtTheBar)
		{
			using VpUnit::CutFadeJudge::SoftResetStrength;
			Assert::AreEqual(1.0f, SoftResetStrength(true, true, false, false, false, 1.0f), L"a confirmed candidate at strength 1 fires");
			Assert::AreEqual(0.0f, SoftResetStrength(false, true, false, false, false, 1.0f), L"never provisionally");
			Assert::AreEqual(0.0f, SoftResetStrength(true, false, false, false, false, 1.0f), L"never without a candidate");
			Assert::AreEqual(0.0f, SoftResetStrength(true, true, true, false, false, 1.0f), L"never on a cut (its restart owns it)");
			Assert::AreEqual(0.0f, SoftResetStrength(true, true, false, true, false, 1.0f), L"never on a flash");
			Assert::AreEqual(0.0f, SoftResetStrength(true, true, false, false, true, 1.0f), L"never on a fade frame (the fade keep owns it)");
			Assert::AreEqual(0.0f, SoftResetStrength(true, true, false, false, false, 0.99f), L"never at 0.99: below the bar nothing happens");
			Assert::AreEqual(0.0f, SoftResetStrength(true, true, false, false, false, 0.0f));
			Assert::AreEqual(0.0f, SoftResetStrength(true, true, false, false, false, std::nanf("")), L"a NaN strength never fires");
			Assert::IsTrue(VpUnit::CutFadeJudge::kSoftResetBar > 0.99f, L"the bar stays high (owner 08:35)");
			Assert::AreEqual(0.5f, VpUnit::CutFadeJudge::SoftResetKeep(1.0f), L"keep is 0.5 at strength 1: never a restart");
			Assert::AreEqual(1.0f, VpUnit::CutFadeJudge::SoftResetKeep(0.0f), L"strength 0 keeps everything");
			Assert::AreEqual(0.5f, VpUnit::CutFadeJudge::SoftResetKeep(2.0f), L"the strength is clamped to 1");
		}

		// SceneRestart.h, the consumers' answer: on, a confirmed candidate is a soft partial reset (keep 0.5); off, the same input
		// gives exactly the answer of a plain frame; provisional, a cut, a cutHere, a flash, a fade frame and 0.99 never fire.
		TEST_METHOD(SceneRestartOfGivesTheSoftResetOnlyWhereTheRuleFires)
		{
			g_plan = Plan();
			g_plan.softAt = 7;
			const VpDetailRestore::SceneRestartAnswer on = VpDetailRestore::SceneRestartOf(7, &PlanReader, true);
			Assert::IsTrue(on.fromRecord && !on.restart && !on.fadePartial, L"a soft reset is no restart and no fade");
			Assert::IsTrue(on.softPartial, L"on: the confirmed candidate is a soft reset");
			Assert::AreEqual(0.5f, on.historyKeep, L"keep 0.5 at strength 1");
			Assert::AreEqual(1.0f, on.strength);
			Assert::IsFalse(on.provisional || on.softWaiting);

			const VpDetailRestore::SceneRestartAnswer off = VpDetailRestore::SceneRestartOf(7, &PlanReader, false);
			const VpDetailRestore::SceneRestartAnswer plain = VpDetailRestore::SceneRestartOf(6, &PlanReader, true);
			Assert::IsTrue(off.fromRecord && !off.restart && !off.fadePartial && !off.softPartial && !off.softWaiting, L"off: no soft reset");
			Assert::AreEqual(1.0f, off.historyKeep, L"off: the whole history, as before");
			Assert::AreEqual(0.0f, off.strength, L"off: strength 0 on a non-restart, as before");
			Assert::IsTrue(!plain.softPartial && plain.historyKeep == 1.0f && plain.strength == 0.0f, L"a plain frame: nothing");
			Assert::IsTrue(off.restart == plain.restart && off.historyKeep == plain.historyKeep && off.strength == plain.strength &&
				off.fadePartial == plain.fadePartial, L"off: the candidate frame answers exactly as a plain frame");
			Assert::IsTrue(VpDetailRestore::SceneRestartOf(7, &PlanReader).historyKeep == 1.0f, L"the argument's default stays off");

			g_plan.softStrength = 0.99f;
			const VpDetailRestore::SceneRestartAnswer low = VpDetailRestore::SceneRestartOf(7, &PlanReader, true);
			Assert::IsTrue(!low.softPartial && !low.restart && low.historyKeep == 1.0f && low.strength == 0.0f, L"0.99: below the bar, nothing");

			g_plan = Plan();
			g_plan.softAt = 7;
			g_plan.softConfirmed = false;
			const VpDetailRestore::SceneRestartAnswer provisional = VpDetailRestore::SceneRestartOf(7, &PlanReader, true);
			Assert::IsTrue(!provisional.softPartial && !provisional.restart && provisional.historyKeep == 1.0f, L"never provisionally");
			Assert::IsTrue(provisional.provisional && provisional.softWaiting, L"a provisional eligible frame is counted as waiting (M3)");
			Assert::IsFalse(VpDetailRestore::SceneRestartOf(7, &PlanReader, false).softWaiting, L"off: nothing waits");

			g_plan = Plan();
			g_plan.cutAt = 9;
			const VpDetailRestore::SceneRestartAnswer cut = VpDetailRestore::SceneRestartOf(9, &PlanReader, true);
			Assert::IsTrue(cut.restart && !cut.softPartial && cut.historyKeep == 0.0f && cut.strength == 1.0f, L"a cut still restarts in full");
			g_plan = Plan();
			g_plan.cutHereAt = 9;
			const VpDetailRestore::SceneRestartAnswer here = VpDetailRestore::SceneRestartOf(9, &PlanReader, true);
			Assert::IsTrue(!here.softPartial && here.historyKeep == 1.0f, L"the frame's own cut verdict (delivered later) is no soft reset");
			g_plan = Plan();
			g_plan.flashAt = 9;
			const VpDetailRestore::SceneRestartAnswer flash = VpDetailRestore::SceneRestartOf(9, &PlanReader, true);
			Assert::IsTrue(!flash.restart && !flash.softPartial && flash.historyKeep == 1.0f, L"a flash: nothing");
			g_plan = Plan();
			g_plan.fadeFrom = 5; g_plan.fadeTo = 12; g_plan.fadeKeep = 0.8f; g_plan.softAt = 8;
			const VpDetailRestore::SceneRestartAnswer fade = VpDetailRestore::SceneRestartOf(8, &PlanReader, true);
			Assert::IsTrue(!fade.restart && fade.fadePartial && !fade.softPartial, L"a fade frame stays the fade's partial reset");
			Assert::AreEqual(0.8f, fade.historyKeep, L"with the fade's keep, not the soft keep");
			g_plan = Plan();
			g_plan.softAt = 7;
			g_plan.missingAt = 7;
			const VpDetailRestore::SceneRestartAnswer missing = VpDetailRestore::SceneRestartOf(7, &PlanReader, true);
			Assert::IsTrue(missing.restart && !missing.fromRecord && !missing.softPartial, L"no group: the restart, as before");
		}

		// The real U7 law on cut-free clips (pan, flash, static): no soft reset, and the switch changes no answer.
		TEST_METHOD(NeverOnCutFreeClips)
		{
			std::vector<Record> still, pan, flash;
			for (uint32_t i = 0; i < 48; ++i)
			{
				still.push_back(Picture(1.0, i));
				pan.push_back(Picture(1.0, i, 2 * i, true));
				flash.push_back(Picture(i == 20 || i == 33 ? 1.2 : 1.0, i));   // two one-frame lighting flashes
			}
			AssertNoSoftReset(still, "static");
			AssertNoSoftReset(pan, "pan");
			AssertNoSoftReset(flash, "flash");
		}

		// detail_pulse_stabilise's CPU model (the module's mirror): off = the pictures of a stream without the candidate and no soft
		// reset; on = one soft reset at the candidate (keep 0.5, no restart, the history only reduced); a cut still restarts.
		TEST_METHOD(DetailPulseModelOffIsUnchangedOnSoftResetsAndCutRestarts)
		{
			const int w = 96, h = 64, frames = 26;
			auto run = [&](uint64_t softAt, bool softReset) {
				g_plan = Plan();
				g_plan.softAt = softAt;
				g_plan.cutAt = 20;
				DetailPulseModel::Settings s;
				s.sourceHz = 23.976;
				s.colorDepth = 10;
				s.sceneReader = &PlanReader;
				s.softReset = softReset;
				DetailPulseModel::Model m;
				std::vector<DetailPulseModel::Frame> out;
				for (int k = 0; k < frames; ++k)
				{
					GrainModel::Image im = Textured(w, h, uint32_t(k));
					if (k % 2)
						for (double& v : im.v) v = 400.0 + 0.6 * (v - 400.0);   // a coding pulse the stabiliser lifts
					out.push_back(m.Process(im, uint64_t(k) + 1, s));
				}
				return out;
			};
			const std::vector<DetailPulseModel::Frame> none = run(0, true), off = run(12, false), on = run(12, true);
			int softs = 0;
			for (size_t u = 0; u < size_t(frames); ++u)
			{
				Assert::IsFalse(off[u].softPartial, L"off: no soft reset");
				Assert::AreEqual(none[u].g1, off[u].g1, L"off: the gains of the stream without the candidate");
				Assert::AreEqual(none[u].g2, off[u].g2);
				Assert::AreEqual(none[u].restart, off[u].restart);
				Assert::IsTrue(none[u].out.v == off[u].out.v, L"off: the same picture");
				softs += on[u].softPartial ? 1 : 0;
				if (u < 11)
					Assert::IsTrue(none[u].out.v == on[u].out.v, L"on: nothing changes before the candidate");
			}
			Assert::AreEqual(1, softs, L"on: exactly one soft reset");
			const size_t c = 11;   // sequence 12
			Assert::IsTrue(on[c].softPartial && !on[c].restart && !on[c].fadePartial, L"on: the candidate is a soft partial reset");
			Assert::AreEqual(0.5, on[c].historyKeep, 1e-6, L"keep 0.5 at strength 1");
			Assert::IsTrue(on[c].g1 <= none[c].g1 + 1e-12 && on[c].g2 <= none[c].g2 + 1e-12, L"the history is only reduced");
			Assert::IsTrue(on[19].restart, L"a cut still restarts");
			Assert::AreEqual(1.0, on[19].g1);
			Assert::AreEqual(1.0, on[19].g2);
		}

		// texture_restore's CPU model (the module's mirror): off = no soft reset and the pictures of a stream without the candidate;
		// on = one soft reset (keep 0.5, no restart); a cut still restarts.
		TEST_METHOD(TextureRestoreModelOffIsUnchangedOnSoftResetsAndCutRestarts)
		{
			const int w = 96, h = 64, frames = 26;
			struct Run { std::vector<std::vector<double>> pictures; std::vector<char> soft, cut; std::vector<double> keep; unsigned long long count = 0; };
			auto run = [&](uint64_t softAt, bool softReset) {
				g_plan = Plan();
				g_plan.softAt = softAt;
				g_plan.cutAt = 20;
				TextureRestoreModel::Model m;
				Run r;
				for (int k = 0; k < frames; ++k)
				{
					const GrainModel::Image g = Textured(w, h, uint32_t(k));
					TextureRestoreModel::Image im(w, h);
					for (size_t i = 0; i < im.v.size(); ++i) im.v[i] = g.v[i];
					TextureRestore::Params p;
					p.colorDepth = 10; p.sourceHz = 23.976; p.bitrateQuality = 1.0; p.sequence = uint64_t(k) + 1;
					p.sceneReader = &PlanReader;
					p.softReset = softReset;
					r.pictures.push_back(m.Frame(im, p).v);
					r.soft.push_back(m.LastWasSoftPartial() ? 1 : 0);
					r.cut.push_back(m.LastWasCut() ? 1 : 0);
					r.keep.push_back(m.LastHistoryKeep());
				}
				r.count = m.SoftResets();
				return r;
			};
			const Run none = run(0, true), off = run(12, false), on = run(12, true);
			Assert::AreEqual(0ull, off.count, L"off: SceneSoftResets() == 0");
			Assert::AreEqual(0ull, none.count);
			Assert::AreEqual(1ull, on.count, L"on: exactly one soft reset");
			for (size_t u = 0; u < size_t(frames); ++u)
			{
				Assert::IsFalse(off.soft[u] != 0, L"off: no soft reset");
				Assert::IsTrue(none.pictures[u] == off.pictures[u], L"off: the same picture as without the candidate");
				Assert::IsTrue(none.cut[u] == off.cut[u], L"off: the same restarts");
				if (u < 11)
					Assert::IsTrue(none.pictures[u] == on.pictures[u], L"on: nothing changes before the candidate");
			}
			const size_t c = 11;   // sequence 12
			Assert::IsTrue(on.soft[c] != 0 && on.cut[c] == 0, L"on: the candidate is a soft reset, no restart");
			Assert::AreEqual(0.5, on.keep[c], 1e-6, L"keep 0.5 at strength 1");
			Assert::IsTrue(on.cut[19] != 0, L"a cut still restarts");
		}

		// The status text (Ctrl+I processing list): off as before; on but inert while the groups stay provisional (M3).
		TEST_METHOD(StatusTextShowsOffAndInactive)
		{
			Assert::AreEqual(std::string(", soft reset: off"), std::string(VpDetailRestore::SoftResetStatusText(false, false)));
			Assert::AreEqual(std::string(", soft reset: off"), std::string(VpDetailRestore::SoftResetStatusText(false, true)));
			Assert::AreEqual(std::string(""), std::string(VpDetailRestore::SoftResetStatusText(true, false)), L"on and able to act: no text");
			Assert::AreEqual(std::string(", soft reset: on (inactive: provisional verdicts only)"),
				std::string(VpDetailRestore::SoftResetStatusText(true, true)));
			char line[200];
			snprintf(line, sizeof(line), "Detail pulse stabilise: on (cap %.2f%s)", 1.35, VpDetailRestore::SoftResetStatusText(false, false));
			Assert::AreEqual(std::string("Detail pulse stabilise: on (cap 1.35, soft reset: off)"), std::string(line), L"the off line as before");

			// The watch: kSoftResetInactiveGroups provisional groups enter "inactive", a confirmed one leaves it, a missing group is neutral.
			g_plan = Plan();
			g_plan.allProvisional = true;
			g_plan.softAt = 3;
			VpDetailRestore::SoftResetWatch watch;
			for (int k = 1; k < VpDetailRestore::kSoftResetInactiveGroups; ++k)
			{
				watch.Note(VpDetailRestore::SceneRestartOf(uint64_t(k), &PlanReader, true));
				Assert::IsFalse(watch.Inactive(), L"a short provisional run does not flip the line");
			}
			g_plan.missingAt = 50;
			watch.Note(VpDetailRestore::SceneRestartOf(50, &PlanReader, true));
			Assert::IsFalse(watch.Inactive(), L"a missing group says nothing about confirmation");
			watch.Note(VpDetailRestore::SceneRestartOf(uint64_t(VpDetailRestore::kSoftResetInactiveGroups), &PlanReader, true));
			Assert::IsTrue(watch.Inactive(), L"only provisional verdicts: inactive, shown");
			Assert::AreEqual(1ull, watch.waiting, L"the candidate that would have fired is counted");
			Assert::AreEqual(std::string(", soft reset: on (inactive: provisional verdicts only)"),
				std::string(VpDetailRestore::SoftResetStatusText(true, watch.Inactive())));
			g_plan.allProvisional = false;
			watch.Note(VpDetailRestore::SceneRestartOf(60, &PlanReader, true));
			Assert::IsFalse(watch.Inactive(), L"the first confirmed group leaves the state");
			for (int k = 0; k < VpDetailRestore::kSoftResetInactiveGroups; ++k)
				watch.Note(VpDetailRestore::SceneRestartOf(uint64_t(70 + k), &PlanReader, true));
			Assert::IsFalse(watch.Inactive(), L"an ordinary (confirmed) lane is never inactive");
			g_plan.allProvisional = true;
			for (int k = 0; k < VpDetailRestore::kSoftResetInactiveGroups; ++k)
				watch.Note(VpDetailRestore::SceneRestartOf(uint64_t(90 + k), &PlanReader, true));
			Assert::IsTrue(watch.Inactive());
			watch.Forget();
			Assert::IsFalse(watch.Inactive(), L"Forget (the stage starting over) clears the run");

			// The modules start able to act.
			Assert::IsFalse(DetailPulse::Module().SceneSoftResetInactive());
			Assert::IsFalse(TextureRestore::Module().SceneSoftResetInactive());
			Assert::AreEqual(0ull, DetailPulse::Module().SceneSoftResets());
			Assert::AreEqual(0ull, TextureRestore::Module().SceneSoftResets());
		}
	};
}
