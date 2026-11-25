// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// scaling.shared: ScalerPass, the ONE scaler unit of the hybrid upscaler rebuild (refs/123 N1 / N4 B0).
// A function-only unit (no ProductId, no config key, nothing reads vectors, no private motion / grain / edge
// code). One call resamples one texture, one plane kind, by libplacebo's orthogonal (1-D) or polar sampler.
//
// Axes: filter (KernelTable.h), axis {X, Y, Both}, ratio per axis DERIVED from the geometry (source span vs
// destination size; two different ratios are normal, e.g. an SD picture with a non-square pixel aspect), plane
// {Luma, Chroma} (+ siting phase as a source-texel shift of the span), anti-ring, light, precision
// (f32 default; f16 only visibly: Note()).
//
// Both axes with a separable kernel = two 1-D passes (Order picks which axis goes first: the re-base chroma
// passes go across first, the faithful ortho branch goes down first); with ewa_lanczossharp = one polar pass.
// The call sequence is libplacebo's exactly as today's users make it, so the migration identity tests can
// compare bit for bit. No existing user calls this yet (B5).
#pragma once

#include <VP-V/modules/scaling/shared/KernelTable.h>
#include <VP-V/modules/scaling/shared/ScalerGeometry.h>

#include <libplacebo/dispatch.h>
#include <libplacebo/gpu.h>
#include <libplacebo/shaders.h>
#include <libplacebo/shaders/sampling.h>

#include <string>

namespace ScalerPass
{
	enum class Axis { X, Y, Both };
	enum class Plane { Luma, Chroma };          // a tag in B0 (chroma differs only by the siting phase the caller passes)
	enum class Light { Signal, Linear };        // Linear is declared, not built in B0: Apply refuses it visibly
	enum class Precision { F32, F16, Target };  // F32 default; F16 is announced by Note(); Target = the target format
	enum class Order { AcrossFirst, DownFirst };

	struct Settings
	{
		ScalerKernels::Spec filter;
		Axis axis = Axis::Both;
		Plane plane = Plane::Luma;
		Light light = Light::Signal;
		Precision precision = Precision::F32;
		Order order = Order::AcrossFirst;
		float antiring = 0.0f;
		// Chroma siting phase: source texels added to the span of each SCALED axis.
		float sitingX = 0.0f;
		float sitingY = 0.0f;
		// Precision::Target: the output (and intermediate) format; null = the source texture's own format.
		pl_fmt targetFormat = nullptr;
	};

	// The source texture and the span to sample (all four 0 = the whole texture).
	struct Source
	{
		pl_tex tex = nullptr;
		float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
	};

	// libplacebo's filter configuration for a table spec (exposed for the tests).
	pl_filter_config FilterConfig(const ScalerKernels::Spec& spec);
	const char* Name(Axis a);

	// Threading and lifetime: one Module is used from ONE thread, the thread that owns the pl_gpu / pl_dispatch it is
	// given (the members are plain, not locked). An output texture is valid only until the next Apply of the same
	// slot or Release. There is no destructor: the owner MUST call Release(gpu) before the pl_gpu is destroyed, or
	// the textures and LUT objects leak.
	class Module
	{
	public:
		Module() = default;
		Module(const Module&) = delete;
		Module& operator=(const Module&) = delete;

		// Resample `src` to dstW x dstH into this module's texture `slot` (valid until the next Apply of that
		// slot or Release). For a single axis the unscaled axis keeps its whole-texel window: pass 0 for its
		// destination size to derive it, or the window size. False (out untouched) on any failure, with
		// LastError(); nothing falls back silently.
		bool Apply(pl_gpu gpu, pl_dispatch dispatch, const Source& src, int dstW, int dstH, const Settings& settings,
			pl_tex& out, int slot = 0);
		void Release(pl_gpu gpu);

		const std::string& LastError() const { return error; }
		// A visible-downgrade note for the last successful Apply (empty: none): the half-float intermediate.
		const std::string& Note() const { return note; }
		// One line describing the last successful Apply.
		const std::string& Status() const { return status; }
		unsigned long long Failures() const { return failures; } size_t TextureBytes() const;
		// POD2-M6: float format selection order: F32+storable -> F32+renderable -> F16+storable -> F16+renderable.
		static pl_fmt SelectFloatFormat(pl_gpu gpu, int comps, Precision precision, bool preferStorable, bool& fellBackTo16);

	private:
		static constexpr int kSlots = 3;
		pl_tex textures[kSlots] = {};
		pl_tex temps[kSlots] = {};
		pl_shader_obj luts[kSlots][2] = {};
		std::string error;
		std::string note;
		std::string status;
		unsigned long long failures = 0;
	};
}
