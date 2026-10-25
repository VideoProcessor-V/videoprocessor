#pragma once

#include <libplacebo/renderer.h>
#include <libplacebo/shaders/custom.h>
#include <libplacebo/shaders/lut.h>

// The projector calibration LUT applied BY VP as its last colour op (design freeze 2026-10-24;
// ADR-0013; barrier R1; slot output.calibration_lut).
//
// Why: libplacebo applies a normalized target LUT during colour management, BEFORE its
// PRE_OUTPUT hooks (renderer.c pass_output_target), so VP's LED measurement and compensation see
// projector codes instead of target light. The agreed order is ... LED measurement -> LED
// compensation -> black level -> calibration LUT -> dither. With calibration_lut_stage = vp the
// renderer stops handing the LUT to libplacebo and appends this hook as the last colour hook at
// PRE_OUTPUT. Its place after the user shaders and before the de-judder rendered-picture cache
// (so the de-judder extras show the LUT-mapped picture too) comes from the order the render path
// pushes the hooks; the registry's OrderSlotsInPlace only orders the slot hooks among themselves,
// so a pipeline_order that moves this (movable) slot can put it ahead of LED compensation.
//
// The LUT itself is applied with libplacebo's own pl_shader_custom_lut (the same sampling as
// libplacebo's target-LUT path). Values outside 0..1 (BTB/WTW carried by ExtendedRange, owner
// rule ADR-0002) are LUT-mapped at the clamped point and the excess is carried on unchanged in
// code space (slope 1). libplacebo's path extrapolates along the LUT's edge slope instead
// (ExtendedRange late restore); the difference is limited to BTB/WTW codes and is documented.
//
// The default stays calibration_lut_stage = libplacebo (byte-identical to before) until the
// owner switches it after measurement.
namespace CalibrationLutStage
{
	class Hook
	{
	public:
		Hook();
		~Hook();
		Hook(const Hook&) = delete;
		Hook& operator=(const Hook&) = delete;

		// The LUT for this frame (nullptr = the hook does nothing and is not submitted).
		void SetLut(const pl_custom_lut* value) { lut = value; }
		const pl_custom_lut* Lut() const { return lut; }
		const pl_hook* Get() const { return &hook; }
		unsigned long long Applied() const { return applied; }
		unsigned long long Failures() const { return failures; }
		// Frees the LUT texture (with the renderer's other GPU resources, before the GPU goes).
		void Release();

	private:
		static pl_hook_res Run(void* priv, const pl_hook_params* params);
		pl_hook_res Apply(const pl_hook_params* params);

		pl_hook hook{};
		const pl_custom_lut* lut = nullptr;
		pl_shader_obj state = nullptr;
		unsigned long long applied = 0;
		unsigned long long failures = 0;
	};
}
