// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// M2 P5a-5: the estimate state (doc 52 section 3.1 item a), moved verbatim out of the renderer's Impl with the code
// that uses it (EngineKeeper.inl, VectorEngineEstimate.inl): B's planes, the estimate-ahead payload / planes /
// schedule, the upload layout, the matcher (`dejudder`), the estimated pair's sequence / epoch / periods, the idle
// engine keeper and its budget adapter, the live-field bookkeeping and the warm-up flags. The renderer holds ONE member
// of this type (`vectorEstimate`, right after `vectorEngine`) and a reference alias per member with the old name, so
// every reader in a zone another package still owns compiles unchanged (doc 52 1.2; HOOKS.md section 4).
//
// The template argument is the renderer's converted-payload type (LibplaceboVideoRenderer::QueuedFrame::ConvertedPayload,
// a type of the renderer's header that VP-V code does not include here; the state only holds a shared_ptr to it). The state is also the engine's view of the matcher (IMatcherView): the IVectorEngine
// answers about the live field read it, so the engine never sees the matcher's type.
//
// Threading: render thread (renderMutex held), as before the move. Teardown is unchanged: the renderer's
// ReleaseDejudder releases the matcher and the textures before the device goes (never this destructor).
// Header for the renderer's translation unit only (it includes the matcher and DXGI); C++14.
#pragma once

#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/modules/motion/estimation/VectorEngine.h>
#include <VP-V/modules/motion/estimation/MotionEngineIdle.h>
#include <VP-V/modules/motion/consumers/DejudderAhead.h>
#include <VP-V/modules/motion/estimation/matcher/DejudderWarp.h>
#include <VideoFrameEncoding.h>
#include <video_frame_formatter/IVideoFrameFormatter.h>

#include <libplacebo/gpu.h>

#include <atlbase.h>
#include <dxgi1_4.h>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace VpMotion
{
	// P3 field view (M2 P5c): one pair's matcher outputs as the renderer reads them. valid = the accessor's validity rule held and a
	// matcher exists (a field may still be null: the matcher has not estimated, or a feature's output is off).
	struct P3FieldSet
	{
		bool valid = false;
		pl_tex field = nullptr, frameTrust = nullptr, subField = nullptr, grainError = nullptr;
		pl_tex camera = nullptr, pixelMask = nullptr, vectorOk = nullptr;
		bool Usable() const { return field != nullptr && frameTrust != nullptr; }
	};
	// The renderer's validity rules for the matcher's live outputs, as pure functions (render-thread state passed in; CPU-testable).
	struct P3Rules
	{
		// R1 (DejudderLiveFieldFor): the matcher's live outputs are this source frame's (sequence, sequence + 1) pair of the current
		// motion epoch; with motion_history_fixes also the live-field sequence (an ahead estimate moved the live outputs to B).
		static bool LiveFor(bool matcher, uint64_t sequence, uint64_t estimatedSequence, bool epochCurrent, bool motionHistoryFixes,
			uint64_t liveFieldFrom)
		{
			return matcher && sequence != 0 && estimatedSequence == sequence && epochCurrent && (!motionHistoryFixes || liveFieldFrom == sequence);
		}
		// R2 (the entrance segment's FieldOf): the live outputs are the pair that starts at `sequence`, current epoch.
		static bool LiveFrom(bool matcher, uint64_t sequence, uint64_t liveFieldFrom, bool epochCurrent)
		{
			return matcher && liveFieldFrom == sequence && epochCurrent;
		}
	};

	template<class Payload>
	struct EstimateState : IMatcherView, Dejudder::IFieldSource
	{
		explicit EstimateState(VectorEngine& engine) : engine_(engine) { engine_.BindMatcher(this); }
		~EstimateState() override { engine_.BindMatcher(nullptr); }
		EstimateState(const EstimateState&) = delete;
		EstimateState& operator=(const EstimateState&) = delete;

		// ---- IMatcherView (render thread) ---------------------------------------------------------------------
		pl_tex LiveField() const override { return dejudder ? dejudder->Field() : nullptr; }
		uint64_t LiveFieldFrom() const override { return dejudderLiveFieldFrom; }
		bool MatcherGlobalFromEngine() const override { return dejudder && dejudder->GlobalFromEngine(); }
		bool LiveSeed(Seed& out) const override
		{
			Dejudder::MotionWarp::SeedState s;
			out = Seed();
			const pl_tex live = dejudder ? dejudder->Field() : nullptr;
			if (!live || !dejudder->LastSeed(s))
			{
				out.skipped = live && s.skipped;
				return false;
			}
			out.fwd = s.fwd;
			out.bwd = s.bwd;
			out.gridW = s.gridW;
			out.gridH = s.gridH;
			out.halvings = s.halvings;
			out.tolerance = s.tolerance;
			out.weightPenalty = s.weightPenalty;
			out.consistencyPx = s.consistencyPx;
			out.liveGridW = live->params.w;
			out.liveGridH = live->params.h;
			return true;
		}

		// ---- SK-MV pass-through (TEMPORARY until P5c): Dejudder::IFieldSource ----------------------------------
		// ONE source for the renderer's motion reads when the matcher is bound (RunDejudderEstimate binds it). Live: the motion segment's
		// published set of the last estimated pair - field, trust, sub-field AND the grain map, one pair id - valid while the epoch is
		// current and the segment has not been retracted by the next estimate. Snapshot: the shared view captured when the matcher took its
		// snapshot (the live pair's id with the snapshot textures), held by the matcher's snapshot lease generation: it stays that pair
		// while the live view advances, and a read after the matcher replaced or invalidated the copies is a LOST lease, which is counted
		// and shown (FallbackCatalog::MotionSnapshotLease), answered by the matcher's member exactly as before. Otherwise false / None
		// and the accessor returns its member.
		bool LiveFieldSet(Dejudder::IFieldSource::Set& set) const override
		{
			if (dejudderLiveFieldFrom == 0)
				return false;
			const auto d = engine_.motionSegment.PeekFrom(VpMotion::Direction::Forward, dejudderLiveFieldFrom, 0);
			if (!d || d.field == nullptr || d.field->field == nullptr)
				return false;
			set.field = d.field->field;
			set.frameTrust = d.field->frameTrust;
			set.subField = d.field->subField;
			set.grainError = d.field->grainError;
			set.from = d.field->from;
			set.to = d.field->to;
			return true;
		}
		void CaptureSnapshot(const Dejudder::IFieldSource::Set& copies, uint64_t generation) override
		{
			snapshotSet_ = copies;
			snapshotSet_.grainError = nullptr;   // the snapshot has no grain map
			Dejudder::IFieldSource::Set live;
			if (LiveFieldSet(live))
			{
				snapshotSet_.from = live.from;   // the snapshot copied the live pair: its id
				snapshotSet_.to = live.to;
			}
			else
			{
				snapshotSet_.from = dejudderLiveFieldFrom;
				snapshotSet_.to = 0;
			}
			snapshotGeneration_ = generation;
			snapshotLeaseLost_ = false;   // a complete snapshot again: the loss (if any) is over
		}
		Dejudder::IFieldSource::SnapshotState SnapshotFieldSet(uint64_t currentGeneration, Dejudder::IFieldSource::Set& set) const override
		{
			if (snapshotGeneration_ == 0)
				return Dejudder::IFieldSource::SnapshotState::None;
			if (currentGeneration != snapshotGeneration_)
				return Dejudder::IFieldSource::SnapshotState::Lost;
			set = snapshotSet_;
			return Dejudder::IFieldSource::SnapshotState::Valid;
		}
		void NoteSnapshotLeaseLost() override
		{
			++snapshotLeaseLostReads_;
			snapshotLeaseLost_ = true;
		}
		void ResetSnapshot() override
		{
			snapshotSet_ = Dejudder::IFieldSource::Set();
			snapshotGeneration_ = 0;
			snapshotLeaseLost_ = false;
		}
		// W1-C2a A2 (owner E3, 2026-11-11): the snapshot lease ends with every epoch move, on both sides (the matcher's
		// copies and this source's set), in every resync mode. The renderer's apply point (ApplyMotionInvalidate) calls it
		// as a VPV-HOOK; the ORIGINAL pathway never runs it (Bill's pathway has no matcher and no lease: byte-identical),
		// which tools/check_hook_gating.py verifies through the early return below. Returns true when the lease was ended.
		bool EndSnapshotLease(bool originalPathway)   // VPV-GATED
		{
			if (originalPathway) return false;
			if (dejudder) dejudder->InvalidateSnapshot();
			ResetSnapshot();
			return true;
		}
		// The loss is visible (FallbackCatalog::MotionSnapshotLease): latched until the next complete snapshot; the count is reads.
		bool SnapshotLeaseLost() const { return snapshotLeaseLost_; }
		unsigned long long SnapshotLeaseLostReads() const { return snapshotLeaseLostReads_; }
		// The "motion snapshot" entry for the host to sync (shown while a lost read is recent, then released).
		FallbackCatalog::Decision SnapshotLeaseDecision(unsigned long long nowMs)
		{
			return snapshotLeaseNotes_.Note(nowMs, snapshotLeaseLostReads_);
		}
		// The captured snapshot's pair id (0, 0 = none captured), for the status line and the tests.
		void SnapshotPair(uint64_t& from, uint64_t& to) const { from = snapshotSet_.from; to = snapshotSet_.to; }

		// ---- P3 field view (M2 P5c, MV1 reader switch): the ONLY place outside motion\estimation that reads the matcher's outputs ----
		// The renderer and its segments ask this view, never `dejudder->`. Each accessor carries ONE of the renderer's validity rules
		// (P3Rules below, kept exactly as the inlined conditions they replace). The answers are the matcher's accessors, which (until the
		// shim removal, P5c-4) read the motion segment's published pair through the IFieldSource pass-through above: the same pl_tex pointers
		// as the direct reads they replace, so every consumer's input is byte-identical. The one allowed direct producer read is the idle
		// engine keeper's warm-up (EngineKeeper.inl, a dummy A -> A estimate with the live field retracted: the view would answer "none").
		// R0 Live(): unconditional (the extras while the de-judder is active; the sigma-delta behind its own R1 gate).
		// R1 LiveFor(seq, motionHistoryFixes): this source frame's live field of the current motion epoch (DejudderLiveFieldFor).
		// R2 LiveFrom(seq): the live outputs are the pair that starts at seq, current epoch (the entrance segment's FieldOf).
		// R4 Snapshot(): the (A, B) copies taken before an estimate ahead; the host keeps its own snapFor / epoch test.
		P3FieldSet Live() const
		{
			P3FieldSet s;
			if (!dejudder)
				return s;
			s.valid = true;
			s.field = dejudder->Field();
			s.frameTrust = dejudder->FrameTrust();
			s.subField = dejudder->SubField();
			s.grainError = dejudder->GrainError();
			s.camera = dejudder->Camera();
			s.pixelMask = dejudder->PixelMask();
			s.vectorOk = dejudder->VectorOk();
			return s;
		}
		bool LiveValidFor(uint64_t sequence, bool motionHistoryFixes) const
		{
			return P3Rules::LiveFor(dejudder != nullptr, sequence, dejudderEstimatedSequence, engine_.motionValidity.Current(dejudderEstimatedEpoch),
				motionHistoryFixes, dejudderLiveFieldFrom);
		}
		P3FieldSet LiveFor(uint64_t sequence, bool motionHistoryFixes) const
		{
			return LiveValidFor(sequence, motionHistoryFixes) ? Live() : P3FieldSet();
		}
		P3FieldSet LiveFrom(uint64_t sequence) const
		{
			return P3Rules::LiveFrom(dejudder != nullptr, sequence, dejudderLiveFieldFrom, engine_.motionValidity.Current(dejudderEstimatedEpoch)) ?
				Live() : P3FieldSet();
		}
		// The snapshot is read per accessor (lazily, in the caller's order): each Snap* read is one lease check, and a lost lease is
		// counted per read (FallbackCatalog::MotionSnapshotLease), so the count stays what it was with the direct reads.
		struct P3Snapshot
		{
			const Dejudder::MotionWarp* matcher = nullptr;
			pl_tex Field() const { return matcher ? matcher->SnapField() : nullptr; }
			pl_tex FrameTrust() const { return matcher ? matcher->SnapFrameTrust() : nullptr; }
			pl_tex SubField() const { return matcher ? matcher->SnapSubField() : nullptr; }
			pl_tex Camera() const { return matcher ? matcher->SnapCamera() : nullptr; }
			pl_tex PixelMask() const { return matcher ? matcher->SnapPixelMask() : nullptr; }
			pl_tex VectorOk() const { return matcher ? matcher->SnapVectorOk() : nullptr; }
			bool Usable() const { return Field() && FrameTrust(); }
		};
		P3Snapshot Snapshot() const { P3Snapshot s; s.matcher = dejudder.get(); return s; }
		// Planner Q7: the snapshot trust rolled off toward the next pair's (dispatches; render thread).
		pl_tex LookAheadTrust() { return dejudder ? dejudder->LookAheadTrust() : nullptr; }
		// B's occlusion mask in a slot; the caller keeps its own slot / sequence / epoch test.
		pl_tex Occlusion(int slot) const { return dejudder ? dejudder->Occlusion(slot) : nullptr; }
		float MinTrustedShare() const { return dejudder ? dejudder->GetSettings().minTrustedShare : 0.0f; }
		// P5c rule 32 (HOOK FREEZE): the renderer's stage reads of the view as ONE gated call each. The ORIGINAL pathway never
		// reads the view here (Bill's pathway has no matcher: byte-identical), which tools/check_hook_gating.py verifies through
		// the early returns. VP-V: exactly LiveFor / Occlusion above.
		// R1 into a stage's params: writes field and grain error only when the live field is this source frame's (else untouched).
		bool LiveFieldInto(bool originalPathway, uint64_t sequence, bool motionHistoryFixes, pl_tex& field, pl_tex& grainError) const   // VPV-GATED
		{
			if (originalPathway) return false;
			const P3FieldSet live = LiveFor(sequence, motionHistoryFixes);
			if (!live.valid) return false;
			field = live.field;
			grainError = live.grainError;
			return true;
		}
		// R1, the field alone (nullptr when not this source frame's).
		pl_tex LiveFieldOnVp(bool originalPathway, uint64_t sequence, bool motionHistoryFixes) const   // VPV-GATED
		{
			if (originalPathway) return nullptr;
			return LiveFor(sequence, motionHistoryFixes).field;
		}
		// B's occlusion mask for the renderer's mask hook; the caller keeps its own slot / sequence / epoch test.
		pl_tex OcclusionOnVp(bool originalPathway, int slot) const   // VPV-GATED
		{
			if (originalPathway) return nullptr;
			return Occlusion(slot);
		}

		// ---- moved members (verbatim, with their comments) ----------------------------------------------------
		// Stage 6 de-judder: B's planes (the next source frame) and the matcher.
		// Created only while the de-judder is on; freed when it goes off.
		// (A later optimisation could reuse B's upload when B becomes current.)
		pl_tex dejudderNextTextures[2] = { nullptr, nullptr };
		// Owner 2026-10-13 (estimate ahead): C, the frame after B, when queued
		// and converted; its planes; the pair estimated ahead (B, C) during A's
		// extras; B's upload layout (C must match it).
		std::shared_ptr<const Payload> dejudderAheadPayload;
		uint64_t dejudderAheadPayloadSequence = 0;
		pl_tex dejudderAheadTextures[2] = { nullptr, nullptr };
		Dejudder::AheadSchedule dejudderAhead;
		struct DejudderUploadLayout
		{
			bool valid = false;
			int width = 0, height = 0, chromaHeight = 0;
			size_t rowBytes = 0, pixelBytes = 0;
			bool lossless422 = false, fullRange = false;
			bool raw = false;   // the raw (GPU unpack) route, with its encoding
			bool rgb = false, bt2020 = false;   // an RGB raw route: luma only, its matrix
			VideoFrameEncoding rawEncoding = VideoFrameEncoding::UNKNOWN;
			bool dvTunnelRaw = false;   // the Dolby Vision tunnel's GPU unpack route
			VideoFrameFormatterOutputContract contract{};
		} dejudderLayout;
		std::unique_ptr<Dejudder::MotionWarp> dejudder;
		uint64_t dejudderEstimatedSequence = 0;
		uint64_t dejudderEstimatedEpoch = 0;   // the motion epoch the estimate was made in (MotionValidity)
		// Bug hunt 3 F10: the source periods the estimated pair's field spans (a gap pair: k + 1).
		int dejudderEstimatedPeriods = 1;
		// Bug hunt 3 F4: the idle motion engine kept for hiccup repair, and its low-memory fallback.
		MotionEngineIdle::Keeper motionEngineIdle;
		CComPtr<IDXGIAdapter3> motionEngineBudgetAdapter;
		unsigned long long dejudderCreates = 0;
		// H3 F4 follow-up: load the idle engine (and the interpolator) once at a source's start, so
		// the first hiccup does not pay its creation on the render thread (about 100 ms here, 9.7 s
		// under CPU load: a backlog flush that swallowed the next gap). Set at renderer start and at
		// a resync / format change; cleared once loaded or when low memory released it.
		bool motionEngineWarmPending = true;
		// Hunt 2 04 M2: the A of the pair whose field the matcher's live outputs hold (0 = none).
		// After an ahead estimate it is B while A is still showing, so A's repeat draws must not
		// read the live field as A's (the extras read their snapshot).
		uint64_t dejudderLiveFieldFrom = 0;
		// Hunt 2 04 L3: the motion source last estimated with (0 matcher only, 1 render-time
		// engine, 2 engine with the producer ahead); -1 = none yet.
		int dejudderMotionSourceKey = -1;
		// r6 fix 1: WarmIdleMotionEngine's dummy A -> A estimate. It loads the engine only: no
		// estimate count, no "motion estimated"/motion-source logs, no vectors to the consumers.
		bool motionEngineWarming = false;

	private:
		VectorEngine& engine_;
		Dejudder::IFieldSource::Set snapshotSet_;   // render thread
		uint64_t snapshotGeneration_ = 0;           // the matcher's snapshot lease generation it was captured under (0 = none)
		bool snapshotLeaseLost_ = false;
		unsigned long long snapshotLeaseLostReads_ = 0;
		FallbackCatalog::SnapshotLeaseNotes snapshotLeaseNotes_;
	};
}
