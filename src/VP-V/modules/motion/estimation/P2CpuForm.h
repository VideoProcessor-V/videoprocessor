// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// motion/estimation: the OWNER FILL of kP2VectorPair for AcquireForm::Cpu (B-prereq (a); lead18/coreplan-B-prereq-P2global-pyramid.md
// section (a)): the pair that ENDS at the asked frame, as a P2PairRecord (identity + generation + the producer's global / second,
// D1 format). The skeleton lane's slot is header-only (a CPU read of an Ofa product is Absent there), so this unit serves the form.
// It reads the engine's P2 slot through IVectorEngine::P2Pairs() (the engine of the asking renderer, found through the hub's
// kVectorEngineQueryId directory); it never reads the producer and never starts the producer or the matcher (doc 91 D).
// Any thread (the slot has its own mutex; the fill is not renderThreadOnly: the camera / pyramid workers read it, ruling 2026-11-14 04:26).
// Declines are VISIBLE: the reason text rides the Acquire's OwnerDeclined cause and the engine's debug line counts them
// (NoteP2CpuFormRead); a consumer's absent pair feeds the existing no-pair-ahead path (NoPairTracker -> FallbackCatalog::Motion).
// Header-only, C++14 (the EdgeMapFill.h pattern; HubBootstrap registers the module with one call).
#include <VP-V/core/SkeletonAcquire.h>
#include <VP-V/core/Products.h>
#include <VP-V/core/hub/ExtensionHub.h>
#include <VP-V/modules/control/skeleton/SkeletonFacts.h>
#include <VP-V/modules/motion/estimation/P2PairRecord.h>
#include <VP-V/modules/motion/estimation/VectorEngineInterface.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace VpMotion
{
	static constexpr const char* kP2CpuFormUnit = "motion/estimation";   // the catalog's kP2VectorPair owner

	// Builds the record of the pair ending at `to` from a view. False (with `why`) = declined: no bound view or no pair for `to`
	// in the current generation. Pure over the view (the unit tests call it without a skeleton).
	inline bool ReadP2PairRecord(const P2View& view, uint64_t to, P2PairRecord& out, std::string& why)
	{
		if (!view.Bound())
		{
			why = "p2 cpu form: no pair store";
			return false;
		}
		uint64_t from = 0, generation = 0;
		const MotionAhead::FlowPtr flow = view.FindTo(to, from, generation);
		if (!flow)
		{
			why = "p2 cpu form: no pair ahead for frame " + std::to_string(to) + " (generation " + std::to_string(view.Generation()) + ")";
			return false;
		}
		out = MakeP2PairRecord(from, to, generation, flow->global, flow->second, flow->globalSet, flow->globalGeneral);
		return true;
	}

	// `engineOf` answers the engine of a host (null = absent); the default goes through the hub's directory.
	using P2EngineOf = std::function<IVectorEngine*(const void* host)>;

	inline IVectorEngine* P2DefaultEngineOf(const void* host)
	{
		IVectorEngineDirectory* dir =
			static_cast<IVectorEngineDirectory*>(Vpv::Hub::ExtensionHub::Instance().Query(kVectorEngineQueryId));
		return dir != nullptr ? dir->EngineFor(host) : nullptr;
	}

	inline VpSkeleton::OwnerFill MakeP2CpuOwnerFill(P2EngineOf engineOf = P2EngineOf())
	{
		VpSkeleton::OwnerFill f;
		f.variantAxes = 0;
		f.renderThreadOnly = false;   // the slot's own mutex: any thread may read (pinned by a test)
		f.keepFrames = 1;             // the camera / pyramid reader lags the render frame by a frame
		f.fill = [engineOf](const VpSkeleton::OwnerFillRequest& req, VpSkeleton::SkeletonMemo&, VpSkeleton::OwnerFillOutput& out) {
			IVectorEngine* engine = engineOf ? engineOf(req.host) : P2DefaultEngineOf(req.host);
			if (engine == nullptr)
			{
				out.declineReason = "p2 cpu form: engine absent";
				return false;
			}
			P2PairRecord rec;
			std::string why;
			if (!ReadP2PairRecord(engine->P2Pairs(), req.seq, rec, why))
			{
				engine->NoteP2CpuFormRead(false);
				out.declineReason = why;
				return false;
			}
			out.bytes.resize(kP2PairRecordBytes);
			if (EncodeP2PairRecord(rec, out.bytes.data(), out.bytes.size()) != kP2PairRecordBytes)
			{
				engine->NoteP2CpuFormRead(false);
				out.bytes.clear();
				out.declineReason = "p2 cpu form: record not encoded";
				return false;
			}
			engine->NoteP2CpuFormRead(true);
			return true;
		};
		return f;
	}

	// Registers the fill with the skeleton found by the hub (tests pass their own demand). Registered once; an absent skeleton is
	// retried at the next Entrance (the EdgeMapModule pattern). Cost per frame once registered: one atomic load.
	class P2CpuFormModule : public Vpv::Hub::ISkeletonModule
	{
	public:
		const char* Name() const override { return "motion.estimation.p2_cpu_form"; }
		bool OnFrame(RendererExtension::FrameContext& c) override
		{
			if (c.phase != RendererExtension::FramePhase::Entrance || registered_.load(std::memory_order_acquire)
				|| Vpv::Hub::ExtensionHub::Instance().OriginalPathway(c.host))
				return false;
			TryRegister();
			return false;
		}
		// True = the fill is registered. False = no skeleton yet (retried) or the registration was refused.
		bool TryRegister(Vpv::Skeleton::IExternalDemand* demand = nullptr, P2EngineOf engineOf = P2EngineOf())
		{
			if (registered_.load(std::memory_order_acquire))
				return true;
			if (demand == nullptr)
				demand = static_cast<Vpv::Skeleton::IExternalDemand*>(
					Vpv::Hub::ExtensionHub::Instance().Query(Vpv::Skeleton::kExternalDemandQueryId));
			if (demand == nullptr)
				return false;   // no skeleton yet: retried at the next Entrance
			const VpSkeleton::SkeletonMemo::Registration r = demand->RegisterOwnerFill(
				VpSkeleton::kP2VectorPair, VpSkeleton::AcquireForm::Cpu, kP2CpuFormUnit, MakeP2CpuOwnerFill(std::move(engineOf)));
			const bool ok = r == VpSkeleton::SkeletonMemo::Registration::Registered
				|| r == VpSkeleton::SkeletonMemo::Registration::AlreadyRegistered;
			registered_.store(ok, std::memory_order_release);
			return ok;
		}
		bool Registered() const { return registered_.load(std::memory_order_acquire); }
	private:
		std::atomic<bool> registered_{ false };
	};

	// Start-up only (HubBootstrap), after the skeleton module; idempotent.
	inline void RegisterP2CpuFormModule()
	{
		static bool registered = false;
		if (registered)
			return;
		registered = true;
		Vpv::Hub::ExtensionHub::Instance().AddModule(std::unique_ptr<Vpv::Hub::ISkeletonModule>(new P2CpuFormModule()));
	}
}
