// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// motion/estimation: the OWNER FILL of kP3AnalysisField for AcquireForm::Gpu (M1#1 / B0#2): a zero-copy GPU VIEW of the
// vector engine's verified analysis field (IVectorEngine::FieldOf) for the current render frame.
// Contract (matches SourcePlanes.h view idiom and P2CpuForm.h engine seam):
//   - Owner unit: "motion/estimation" (ProductCatalog.h kP3AnalysisField row).
//   - Default variant only (variantAxes = 0); any non-default axis declines visibly as VariantUndeclared.
//   - Zero-copy view (view = true, gpu.bytes = 0): no texture allocated, no ledger grant or GPU dispatcher needed.
//   - Frame-scoped (frameScoped = true, OwnerFillOutput::Scope::Frame): released at the next OnSeal so the matcher-owned
//     pl_tex view never outlives the render frame it was acquired in.
//   - Render thread only (renderThreadOnly = true, matching IVectorEngine::FieldOf); off-render-thread Acquire declines
//     visibly as WrongThread.
//   - Read-only: calls IVectorEngine::FieldOf(req.seq, vf) only; never starts the matcher or the NVOF producer.
//   - Declines are VISIBLE: out.declineReason names the absence ("p3 analysis field: engine absent",
//     "p3 analysis field: no verified field for seq") and rides Acquire's OwnerDeclined cause / LastCause / Decisions.
// Header-only, standalone C++14 (registered by VectorEngineExtension::OnFrame on the first VP-V Entrance).
#include <VP-V/core/Products.h>
#include <VP-V/core/SkeletonAcquire.h>
#include <VP-V/core/hub/ExtensionHub.h>
#include <VP-V/modules/control/skeleton/SkeletonFacts.h>
#include <VP-V/modules/motion/estimation/VectorEngineInterface.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace VpMotion
{
	static constexpr const char* kP3FieldUnit = "motion/estimation";   // the catalog's kP3AnalysisField owner (ProductCatalog.h)

	// Resolves the host's IVectorEngine* (null = absent): test seam matching P2EngineOf. Default queries the hub directory.
	using P3EngineOf = std::function<IVectorEngine*(const void* host)>;

	// Production render-thread lookup through the hub's IVectorEngineDirectory (null when no directory or host engine exists).
	inline IVectorEngine* P3DefaultEngineOf(const void* host)
	{
		IVectorEngineDirectory* dir =
			static_cast<IVectorEngineDirectory*>(Vpv::Hub::ExtensionHub::Instance().Query(kVectorEngineQueryId));
		return dir != nullptr ? dir->EngineFor(host) : nullptr;
	}

	// Tag stored in GpuMemoHandle::textureSet to identify a P3FieldFill view whose keepAlive carries a const VerifiedField.
	inline const void* P3FieldFactsTag()
	{
		static const char tag = 0;
		return &tag;
	}

	// The VerifiedField metadata of a kP3AnalysisField GPU AcquireResult produced by MakeP3GpuOwnerFill (null for anything else).
	// Valid while `r` (which holds `r.gpu.keepAlive`) stays alive; texture handles inside remain matcher-owned for this render frame.
	inline const VerifiedField* P3FieldFactsOf(const VpSkeleton::AcquireResult& r)
	{
		return r.Ok() && r.gpu.textureSet == P3FieldFactsTag()
			? static_cast<const VerifiedField*>(r.gpu.keepAlive.get())
			: nullptr;
	}

	inline VpSkeleton::OwnerFill MakeP3GpuOwnerFill(P3EngineOf engineOf = P3EngineOf())
	{
		VpSkeleton::OwnerFill f;
		f.variantAxes = 0;
		f.view = true;               // zero-copy view of VerifiedField::fwd owned by the matcher (0 extra VRAM bytes)
		f.frameScoped = true;        // valid only for the current render frame (released at the next OnSeal)
		f.renderThreadOnly = true;   // IVectorEngine::FieldOf is render-thread only
		f.fill = [engineOf](const VpSkeleton::OwnerFillRequest& req, VpSkeleton::SkeletonMemo&, VpSkeleton::OwnerFillOutput& out) {
			IVectorEngine* engine = engineOf ? engineOf(req.host) : P3DefaultEngineOf(req.host);
			if (engine == nullptr)
			{
				out.declineReason = "p3 analysis field: engine absent";
				return false;
			}
			VerifiedField vf;
			if (!engine->FieldOf(req.seq, vf) || !vf.fwd)
			{
				out.declineReason = "p3 analysis field: no verified field for seq";
				return false;
			}
			const std::shared_ptr<const VerifiedField> facts = std::make_shared<const VerifiedField>(vf);
			out.gpu.resource = const_cast<void*>(static_cast<const void*>(vf.fwd));
			out.gpu.textureSet = P3FieldFactsTag();
			out.gpu.index = 0;
			out.gpu.width = vf.gridW > 0 ? static_cast<uint32_t>(vf.gridW) : 0u;
			out.gpu.height = vf.gridH > 0 ? static_cast<uint32_t>(vf.gridH) : 0u;
			out.gpu.bytes = 0;   // view: nothing allocated
			out.gpu.keepAlive = std::const_pointer_cast<void>(std::static_pointer_cast<const void>(facts));
			out.scope = VpSkeleton::OwnerFillOutput::Scope::Frame;
			return true;
		};
		return f;
	}

	// Registers the kP3AnalysisField GPU owner fill with the skeleton found via the hub (or an injected demand in tests).
	// True = registered (or already registered). False = no skeleton yet (retried) or registration refused.
	inline bool TryRegisterP3FieldFill(Vpv::Skeleton::IExternalDemand* demand = nullptr, P3EngineOf engineOf = P3EngineOf())
	{
		if (demand == nullptr)
			demand = static_cast<Vpv::Skeleton::IExternalDemand*>(
				Vpv::Hub::ExtensionHub::Instance().Query(Vpv::Skeleton::kExternalDemandQueryId));
		if (demand == nullptr)
			return false;
		const VpSkeleton::SkeletonMemo::Registration r = demand->RegisterOwnerFill(
			VpSkeleton::kP3AnalysisField, VpSkeleton::AcquireForm::Gpu, kP3FieldUnit, MakeP3GpuOwnerFill(std::move(engineOf)));
		return r == VpSkeleton::SkeletonMemo::Registration::Registered
			|| r == VpSkeleton::SkeletonMemo::Registration::AlreadyRegistered;
	}

	// Consumer helper: acquires kP3AnalysisField (GPU) through SkeletonMemo::Acquire and extracts its VerifiedField facts
	// (gridW/gridH/blockSize/scaleToSourcePx/fwd/bwd/trust/stamp) without bypassing the skeleton or taking a private copy.
	// If `outResult` is non-null, it receives the AcquireResult (pin, status, cause, gpu handle).
	inline bool AcquireP3Field(VpSkeleton::SkeletonMemo& memo, uint64_t seq, VerifiedField& outField,
		VpSkeleton::AcquireResult* outResult = nullptr)
	{
		outField = VerifiedField();
		VpSkeleton::AcquireResult r = memo.Acquire(VpSkeleton::kP3AnalysisField, seq, VpSkeleton::AcquireForm::Gpu);
		const bool ok = r.Ok() && r.gpu.Valid() && r.gpu.resource != nullptr;
		if (ok)
		{
			if (const VerifiedField* facts = P3FieldFactsOf(r))
			{
				outField = *facts;
			}
			else
			{
				outField.seq = seq;
				outField.pairTo = seq;
				outField.fwd = static_cast<pl_tex>(r.gpu.resource);
				outField.gridW = static_cast<int>(r.gpu.width);
				outField.gridH = static_cast<int>(r.gpu.height);
			}
		}
		if (outResult != nullptr)
			*outResult = std::move(r);
		return ok && outField.fwd != nullptr;
	}
}
