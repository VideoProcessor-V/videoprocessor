// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// cut_soft_reset in temporal detail (C5 batch B; review D:\codex\vp-handover\batchB\review-softreset-r1.md L1, plan
// lead19\c5-softreset.md item 2 "(and TD model)"): TemporalDetail's scene on the SHARED scene product, read exactly as detail pulse and
// texture restore read it. One VpDetailRestore::SceneRestartOf per presented frame (texture_restore/SceneRestart.h: the skeleton's
// get-or-compute scene product in production; U7's delivered cut, the fade and its keep, and the soft-reset rule
// CutFadeJudge::SoftResetStrength at its high bar kSoftResetBar with the keep SoftResetKeep), copied field for field into
// TemporalDetail::SceneRestartInput for Model::FrameIn::sceneRestart / Gpu::Params::sceneRestart. Nothing here classifies or decides:
// the model and the GPU module apply the answer (a restart resets the store; a fade or a soft reset multiplies every stored slot's keep
// by historyKeep, one path). The same switch (cut_soft_reset, the consumers' Params::softReset), the same rule, bar and depth.
//
// Owner intent (2026-11-15 08:35 EST, question window U7 Q4: "need the bar fairly high here. Mid scene firing is a major problem";
// 09:20: a fade is no full restart, the history is gently reduced): below the bar nothing happens, a provisional verdict never fires, a
// confirmed cut still resets, and a fade keeps its own partial reset (not behind the switch).
//
// No silent fallbacks (review softreset r1 M3): the source keeps the consumers' VpDetailRestore::SoftResetWatch, and every answer
// carries their status text (SoftResetStatusText: ", soft reset: off", or ", soft reset: on (inactive: provisional verdicts only)"
// while the scene groups stay provisional); the model and the GPU module append it to their status. A frame without a readable scene
// group is a reset there (the consumers restart too), shown as "reset: scene (no record)" and counted in Missing().
//
// Kept apart from TemporalDetailModel.h on purpose: the model stays header-only without the renderer hub (tools/td-cli builds it alone).
// Like SharedMotionSource, no production host wiring yet (temporal detail has no renderer call site): the host passes the production
// reader (null = VpDetailRestore::DefaultSceneReader(), the skeleton's) and cut_soft_reset when it is wired, calls Read once per new
// presented frame and Forget on the module's Forget triggers. Header only, C++14. Render thread (the reader's thread).
#pragma once

#include <VP-V/modules/repair/temporal_detail/TemporalDetail.h>
#include <VP-V/modules/repair/texture_restore/SceneRestart.h>

#include <cstdint>

namespace TemporalDetail
{
	class SharedSceneSource
	{
	public:
		// reader: null = VpDetailRestore::DefaultSceneReader() (production: the skeleton's scene product); tests inject one.
		// softReset: cut_soft_reset, as the consumers' Params::softReset (built-in default on, VpDetailRestore::kDefaultCutSoftReset).
		explicit SharedSceneSource(VpDetailRestore::SceneReaderFn reader = nullptr, bool softReset = VpDetailRestore::kDefaultCutSoftReset)
			: m_reader(reader), m_softReset(softReset) {}

		void SetSoftReset(bool on) { m_softReset = on; }
		bool SoftReset() const { return m_softReset; }

		// The answer for one NEW presented frame (once per frame, never for a repeat: the watch counts scene groups).
		SceneRestartInput Read(uint64_t seq)
		{
			const VpDetailRestore::SceneRestartAnswer a = VpDetailRestore::SceneRestartOf(seq, m_reader, m_softReset);
			m_watch.Note(a);
			m_softResets += a.softPartial ? 1u : 0u;
			m_missing += a.fromRecord ? 0u : 1u;
			return From(a, VpDetailRestore::SoftResetStatusText(m_softReset, m_watch.Inactive()));
		}

		// The consumers' answer, field for field (the only conversion).
		static SceneRestartInput From(const VpDetailRestore::SceneRestartAnswer& a, const char* statusNote = "")
		{
			SceneRestartInput s;
			s.given = true;
			s.restart = a.restart;
			s.fromRecord = a.fromRecord;
			s.fadeWithoutKeep = a.fadeWithoutKeep;
			s.fadePartial = a.fadePartial;
			s.softPartial = a.softPartial;
			s.historyKeep = a.historyKeep;
			s.strength = a.softPartial ? a.strength : 0.0;
			s.statusNote = statusNote ? statusNote : "";
			return s;
		}

		// The consumers' counters: soft resets delivered, frames the rule would have fired on had their verdict been confirmed (M3),
		// frames without a readable scene group; and the M3 state (only provisional verdicts lately: the switch cannot act).
		unsigned long long SoftResets() const { return m_softResets; }
		unsigned long long SoftWaiting() const { return m_watch.waiting; }
		unsigned long long Missing() const { return m_missing; }
		bool SoftResetInactive() const { return m_watch.Inactive(); }
		// The module starting over (its Forget triggers): the provisional run starts again, as in the consumers' Module::Forget.
		void Forget() { m_watch.Forget(); }

	private:
		VpDetailRestore::SceneReaderFn m_reader = nullptr;
		bool m_softReset = VpDetailRestore::kDefaultCutSoftReset;
		VpDetailRestore::SoftResetWatch m_watch;
		unsigned long long m_softResets = 0, m_missing = 0;
	};
}
