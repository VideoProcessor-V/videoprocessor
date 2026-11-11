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

		// ---- SK-MV (MV1): the VP-side view of the matcher's field set ------------------------------------------
		// One place that reads today's dejudder-> accessors (Field / FrameTrust / SubField / Camera, and the Snap* copies the extras and
		// the entrance snapshot read while the next pair is estimated), so a renderer reader asks the view and not the matcher. The
		// renderer's reads are NOT switched to it here: M2 P5b publishes no field interface and P5c owns the warp, the interpolator and
		// the live-field bookkeeping, so the reader switch rides P5c (README, doc 83); each future switch is a one-line, byte-identical
		// call-site change. The same field set is published at every estimate through the motion segment (MotionShadowField).
		struct FieldSetView
		{
			pl_tex field = nullptr, frameTrust = nullptr, subField = nullptr, camera = nullptr;
			bool Usable() const { return field != nullptr && frameTrust != nullptr; }
		};
		FieldSetView LiveFields() const
		{
			FieldSetView v;
			if (dejudder)
			{
				v.field = dejudder->Field();
				v.frameTrust = dejudder->FrameTrust();
				v.subField = dejudder->SubField();
				v.camera = dejudder->Camera();
			}
			return v;
		}
		FieldSetView SnapshotFields() const
		{
			FieldSetView v;
			if (dejudder)
			{
				v.field = dejudder->SnapField();
				v.frameTrust = dejudder->SnapFrameTrust();
				v.subField = dejudder->SnapSubField();
				v.camera = dejudder->SnapCamera();
			}
			return v;
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
