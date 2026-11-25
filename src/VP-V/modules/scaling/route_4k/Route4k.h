// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// scaling.route_4k (Q2-B2, refs/123 N1.2 / N4): pre-render 4K/processing-resolution upscaler built
// on the single ScalerPass unit and the single HybridBlend law (zero duplicate kernel or blend maths).
//
// Processing target geometry is ALWAYS derived from delivered raster + output target + SourceGeometrySet
// (never fixed x2 or hard-coded resolution literals):
//   targetW = max(deliveredW, outputW), targetH = max(deliveredH, outputH)
//   dstW    = max(inputW, targetW),     dstH    = max(inputH, targetH)   (never downscales an axis)
//
// Modes (upscale_route):
//   off          - bypassed: zero GPU passes, zero allocations, byte-identical output.
//   rebased_only - runs only when the frame was rebased and target exceeds input on at least one axis.
//   always_4k    - runs whenever target exceeds input on at least one axis.
//
// Threading and lifetime: render thread only (same as SourceRebase / ScalerPass). Call Release(gpu)
// before the pl_gpu is destroyed so all ScalerPass textures, LUT objects, and blend textures are freed.
#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/modules/scaling/geometry/SourceGeometrySet.h>
#include <VP-V/modules/scaling/shared/HybridBlend.h>
#include <VP-V/modules/scaling/shared/KernelTable.h>
#include <VP-V/modules/scaling/shared/ScalerPass.h>

#include <libplacebo/dispatch.h>
#include <libplacebo/gpu.h>
#include <libplacebo/renderer.h>

#include <string>

namespace Route4k
{
	enum class Mode
	{
		Off,
		RebasedOnly,
		Always4k
	};

	// Case-insensitive parser for upscale_route ("off" | "rebased_only" | "always_4k").
	// Unknown values map to Mode::Off with *ok = false (when ok != nullptr).
	Mode ParseMode(const std::string& value, bool* ok = nullptr);
	const char* ModeName(Mode mode);

	// Maps config kernel tokens ("lanczos3", "lanczos2", "lanczos4", "ewa_lanczossharp",
	// "catmull_rom", "bicubic", "hermite", "bilinear") to ScalerKernels::Spec drawn from
	// ScalerKernels::Table(). Returns false on unknown token without modifying `out`.
	bool ParseKernel(const std::string& token, ScalerKernels::Spec& out);

	// True when two kernel specs select the same resampling kernel and parameters.
	bool SameKernel(const ScalerKernels::Spec& a, const ScalerKernels::Spec& b);

	inline ScalerKernels::Spec DefaultSharpFilter()
	{
		ScalerKernels::Spec s;
		s.kind = ScalerKernels::Kind::Lanczos;
		s.radius = 3;
		s.B = 0.0;
		s.C = 0.0;
		return s;
	}

	inline ScalerKernels::Spec DefaultSafeFilter()
	{
		ScalerKernels::Spec s;
		s.kind = ScalerKernels::Kind::Hermite;
		s.radius = 2;
		s.B = 0.0;
		s.C = 0.0;
		return s;
	}

	struct Config
	{
		Mode mode = Mode::Off;
		ScalerKernels::Spec sharpFilter = DefaultSharpFilter();
		ScalerKernels::Spec safeFilter = DefaultSafeFilter();
		float sharpAntiring = 0.5f;
		float safeAntiring = 0.5f;
		float wBase = 1.0f;
		bool preferFloat32 = true;
	};

	// Derived ratio summary consulting Scaling::GeometrySet::RatioForms / ConventionalPair
	// against the active input and target raster geometry.
	struct DerivedGeometry
	{
		bool conventional = false;     // matches a listed GeometrySet entry on the target raster
		bool parked = false;           // matched a detect-only (parked) GeometrySet entry
		const char* name = nullptr;    // GeometrySet entry name when conventional, else nullptr
		double ratioX = 1.0;           // dstW / inputW (>= 1.0)
		double ratioY = 1.0;           // dstH / inputH (>= 1.0)
		double sourceRatioX = 1.0;     // inputW / dstW (GeometrySet ratio form)
		double sourceRatioY = 1.0;     // inputH / dstH (GeometrySet ratio form)
	};

	DerivedGeometry DerivedGeometryRatios(int inputW, int inputH, int targetW, int targetH);

	struct TargetGeometry
	{
		int targetW = 0;               // max(deliveredW, outputW)
		int targetH = 0;               // max(deliveredH, outputH)
		int dstW = 0;                  // max(inputW, targetW) - never downscales X
		int dstH = 0;                  // max(inputH, targetH) - never downscales Y
		double factorX = 1.0;          // dstW / inputW (>= 1.0)
		double factorY = 1.0;          // dstH / inputH (>= 1.0)
		DerivedGeometry derived{};
	};

	struct PlanResult
	{
		bool shouldRun = false;
		bool oneAxis = false;          // true when exactly one axis upscales
		TargetGeometry geometry{};
		std::string reason;
	};

	// Pure geometry and gating planner (zero GPU dependencies).
	PlanResult Plan(int inputW, int inputH, int deliveredW, int deliveredH, int outputW, int outputH,
		bool wasRebased, bool slotRuns, bool originalPathway, const Config& cfg);

	// Pure check: would AdaptiveDownscale::UpscaleAxesOf still detect an upscale downstream
	// after Route4k? False whenever Route4k runs to target >= output (proves no double upscale).
	bool WillDownstreamUpscale(const PlanResult& plan, int inputW, int inputH, int outputW, int outputH);

	// Pure CPU reference forwarding directly to HybridBlend::Weight + HybridBlend::BlendLuma
	// (single source of truth; no duplicate blend formula).
	inline float HybridBlendLumaReference(float sharpY, float safeY, float wBase, float edge, float grain, float motion)
	{
		HybridBlend::Terms t;
		t.wBase = wBase;
		t.edge = edge;
		t.grain = grain;
		t.motion = motion;
		return HybridBlend::BlendLuma(sharpY, safeY, HybridBlend::Weight(t));
	}

	class Module
	{
	public:
		Module() = default;
		Module(const Module&) = delete;
		Module& operator=(const Module&) = delete;
		~Module();

		// When !plan.shouldRun: copies `out = source` with ZERO GPU passes and ZERO allocations.
		// When plan.shouldRun: scales luma via ScalerPass (+ HybridBlend compute pass when both
		// sharp and safe branches contribute) and chroma planes via ScalerPass preserving subsampling,
		// siting shifts (ChromaSiting::ShiftAfterResample) and crop. Returns false (leaving `out`
		// untouched) on any failure and records the reason in LastError() / Fallback().
		bool Apply(pl_gpu gpu, pl_dispatch dispatch, const pl_frame& source, const PlanResult& plan,
			const Config& cfg, pl_frame& out, int slot = 0);

		void Release(pl_gpu gpu);

		const std::string& LastError() const { return error; }
		const std::string& FormatNote() const { return formatNote; }
		const std::string& Status() const { return status; }
		unsigned long long Failures() const { return failures; }
		FallbackCatalog::Decision Fallback() const;

	private:
		static constexpr int kSlots = 3;
		ScalerPass::Module sharpPass;
		ScalerPass::Module safePass;
		ScalerPass::Module chromaPasses[PL_MAX_PLANES];
		pl_tex blendTex[kSlots] = {};
		std::string error;
		std::string formatNote;
		std::string status;
		unsigned long long failures = 0;
	};
}
