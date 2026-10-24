#pragma once

#include "NetRunner.h"

#include <string>

// PlNetRunner: the pl_dispatch implementation of INetRunner. A STUB until the skeleton lead wires
// it (barrier B1 groundwork, 2026-10-24): Prepare() refuses with a plain reason, so a caller that
// tries to use it bypasses the net visibly ("Fallbacks engaged: nets: bypassed (...)").
//
// It is header-only and includes no libplacebo, so it compiles everywhere. The wired version
// moves into a .cpp in the renderer project. Its shim mirrors libplacebo's custom_mpv.c
// (hook_hook / bind_pass_tex), which VP cannot use directly because those hooks only run inside
// pl_render (B1):
//
//   Prepare: for each NetPass keep the body; choose the intermediate format from
//     CheckCapabilities (rgba16f where the fingerprint allows, else rgba32f); create the SAVE
//     textures lazily per size (renderable; storable for compute passes; sampleable LINEAR).
//   Run, per pass, with the call site's pl_dispatch (front end: the renderer's; D: the dispatch in
//     pl_hook_params):
//     1. sh = pl_dispatch_begin(dispatch);
//     2. for each //!BIND name (HOOKED = the hooked stage): id = sh_bind(sh, tex, CLAMP, LINEAR,
//        "hook_tex", &rect, &pos, &pt) and the macros NAME_raw, NAME_pos, NAME_size
//        (vec2(textureSize(id, 0))), NAME_pt, NAME_mul (1.0 for float textures; the plane's
//        normalisation for UNORM), NAME_tex(pos), NAME_texOff(off); the HOOKED_* aliases;
//     3. each //!PARAM as a shader variable (labels from NetRunInputs::params; ints as int);
//     4. append the body; out size = roundf(RPN WIDTH/HEIGHT) (EvalSizeExpr);
//     5. fragment: GLSL("vec4 color = hook();") + pl_dispatch_finish(target = the pass texture);
//        compute: sh_try_compute(block), out_image as a STORAGE_IMG desc, "hook();",
//        pl_dispatch_compute(dispatch_size = ceil(out / block));
//     6. a pass without SAVE replaces the hooked stage; the last one is the net output.
//   Cost: a pl_timer per Run (or per pass) -> RecordTiming() -> SlotTiming for the net's slot.
//   Oracle: CpuNetExecutor on the same inputs (GPU test, VP_TEST_ADAPTER=NVIDIA, mains only).
//   Note: NAME_tex() on the GPU filters with reduced fractional precision (NVIDIA: 8-bit lerp
//   weights); D's source-feature passes and A's/repair's VPNNQ_tex use it. Compare with a code
//   tolerance, or switch those reads to texelFetch-based bilinear in the exporter.
namespace VpNets
{
	class PlNetRunner : public INetRunner
	{
	public:
		const char* Name() const override { return "pl_dispatch (not wired)"; }

		NetCapabilities Capabilities(const NetDeviceContext&) const override
		{
			// Wired version: from pl_gpu (glsl.compute, pl_find_fmt(... PL_FMT_CAP_RENDERABLE /
			// STORABLE / LINEAR) for rgba16f and rgba32f, limits.max_tex_2d_dim).
			NetCapabilities c;
			c.fragment = false;
			return c;
		}

		bool Prepare(const NetDeviceContext&, const NetPackage& pkg, const NetPrepareParams&, std::string* error) override
		{
			if (error) *error = pkg.name + ": the pl_dispatch net runner is not wired yet (B1 groundwork)";
			return false;
		}

		bool Run(const NetDeviceContext&, const NetRunInputs&, NetTexture&, std::string* error) override
		{
			if (error) *error = "the pl_dispatch net runner is not wired yet (B1 groundwork)";
			return false;
		}

		void Release() override {}
	};
}
