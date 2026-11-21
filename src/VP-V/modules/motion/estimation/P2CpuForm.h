// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// motion/estimation: the OWNER FILL of kP2VectorPair for AcquireForm::Cpu (B-prereq (a); lead18/coreplan-B-prereq-P2global-pyramid.md
// section (a)): the pair that ENDS at the asked frame, as a P2PairRecord (identity + generation + the producer's global / second,
// D1 format). The skeleton lane's slot is header-only (a CPU read of an Ofa product is Absent there), so this unit serves the form.
// It reads the engine's P2 slot through IVectorEngine::P2RecordEndingAt() (the engine of the asking renderer, found through the hub's
// kVectorEngineQueryId directory; the engine applies the generation check and owns the Flow layout, so this header never sees
// MotionAhead::Flow); it never reads the producer and never starts the producer or the matcher (doc 91 D).
// Any thread (the slot has its own mutex; the fill is not renderThreadOnly: the C8-10 / camera workers read it, ruling 2026-11-14 04:26).
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

	// `engineOf` answers the engine of a host (null = absent): a TEST seam (the test owns the engine's lifetime). The default goes
	// through the hub's directory with its locked read (P2DefaultRead).
	using P2EngineOf = std::function<IVectorEngine*(const void* host)>;

	// GF-P4 S7 (a): the production read. The directory runs the whole read (record + count) under its lock
	// (IVectorEngineDirectory::ReadP2Record), so no raw engine pointer is used outside it on this off-render-thread fill.
	inline bool P2DefaultRead(const void* host, uint64_t to, P2PairRecord& out, std::string& why)
	{
		IVectorEngineDirectory* dir =
			static_cast<IVectorEngineDirectory*>(Vpv::Hub::ExtensionHub::Instance().Query(kVectorEngineQueryId));
		if (dir == nullptr)
		{
			why = "p2 cpu form: engine absent";
			return false;
		}
		return dir->ReadP2Record(host, to, out, why);
	}

	inline VpSkeleton::OwnerFill MakeP2CpuOwnerFill(P2EngineOf engineOf = P2EngineOf())
	{
		VpSkeleton::OwnerFill f;
		f.variantAxes = 0;
		f.renderThreadOnly = false;   // the slot's own mutex: any thread may read (pinned by a test)
		f.keepFrames = 1;             // a lag-1 reader (C8-10 / camera) trails the render frame by a frame
		f.fill = [engineOf](const VpSkeleton::OwnerFillRequest& req, VpSkeleton::SkeletonMemo&, VpSkeleton::OwnerFillOutput& out) {
			P2PairRecord rec;
			std::string why;
			bool read = false;
			if (engineOf)
			{
				IVectorEngine* engine = engineOf(req.host);
				if (engine == nullptr)
					why = "p2 cpu form: engine absent";
				else
					read = ReadP2RecordOf(*engine, req.seq, rec, why);   // the same read + count as the directory's
			}
			else
				read = P2DefaultRead(req.host, req.seq, rec, why);
			if (!read)
			{
				out.declineReason = why;
				return false;
			}
			// The read is counted served (the engine's count, under the directory's lock); the buffer is sized to the record, so the
			// encode cannot refuse it. Kept as a visible decline should the codec ever change.
			out.bytes.resize(kP2PairRecordBytes);
			if (EncodeP2PairRecord(rec, out.bytes.data(), out.bytes.size()) != kP2PairRecordBytes)
			{
				out.bytes.clear();
				out.declineReason = "p2 cpu form: record not encoded";
				return false;
			}
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
