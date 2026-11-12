// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Global camera model: the GPU form of the direct photometric refinement (doc 103 s5 R3-GPU, stage 1). The same
// algorithm as PhotometricRefine.cpp (the CPU reference, which stays the definition), run as D3D11 compute on the
// producer worker's own device (MotionAheadProducer.cpp CreateDevice): candidate pass + gradient histogram, the
// deterministic strongest-gradient selection (threshold bin, raster-order ties, row prefix), then per Gauss-Newton
// pass the residuals, an exact radix-select median of |r|, the Huber-weighted normal equations as a fixed-order
// reduction, and a one-thread solve / inverse-compositional update; the verification sums last. Every pass is
// dispatched up front (passes after convergence are no-ops), so a call is one submission and ONE readback, which
// can be collected later (async). The arithmetic is double (SM 5.0 double ops only: add / mul / compare; division,
// reciprocal and square root by Newton steps from float), so the result matches the CPU reference to rounding and
// repeats byte-equal on the same device. The final status decisions (degenerate, correction, gain range,
// improvement) run on the CPU from the read-back values with the CPU code's own geometry functions.
//
// Input: one interface (RefineLumaInput). Stage 1 implements the CPU-upload path only (RefineUploadInput: the
// producer's HalfLuma8 / quarter 8-bit buffer, or full-resolution 10-bit codes from AnalysisLumaSource as R16);
// the twin / fence inputs (lane\shared EngineLumaGpu) are a later swap behind the same interface. Not wired
// anywhere yet (no setting, no status line). See README.md "Photometric refinement (R3)".
#pragma once

#include <cstdint>
#include <string>

#include "PhotometricRefine.h"

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11ShaderResourceView;

namespace CameraModel
{
	// The pair as two R8_UINT or R16_UINT textures on the refiner's device (codes; same conventions as LumaPairView).
	struct RefineGpuLuma
	{
		ID3D11ShaderResourceView* from = nullptr;   // frame N-1 (template)
		ID3D11ShaderResourceView* to = nullptr;     // frame N
		int width = 0, height = 0;
		bool r16 = false;
		double sourcePxPerPixel = 0;
		double codeScale = 1;
	};

	// The input interface: produces the GPU pair for one call. Implementations own their textures.
	class RefineLumaInput
	{
	public:
		virtual ~RefineLumaInput() {}
		virtual bool Prepare(ID3D11DeviceContext* context, RefineGpuLuma* out, std::string* why) = 0;
		virtual const char* PathName() const = 0;
	};

	// Stage 1 input: uploads a CPU pair (LumaPairView: 8-bit engine luma, or R16 codes such as full-resolution 10-bit
	// luma with codeScale 255 / 1023) with UpdateSubresource. The view's planes must stay valid until Prepare returns.
	class RefineUploadInput : public RefineLumaInput
	{
	public:
		RefineUploadInput();
		~RefineUploadInput();
		bool Init(ID3D11Device* device, std::string* why);
		void Set(const LumaPairView& view) { m_view = view; }
		bool Prepare(ID3D11DeviceContext* context, RefineGpuLuma* out, std::string* why) override;
		const char* PathName() const override { return "cpu upload"; }
		double LastUploadCpuMs() const { return m_uploadMs; }   // CPU time of the UpdateSubresource calls (the GPU copy is measured by the caller)

	private:
		RefineUploadInput(const RefineUploadInput&);
		RefineUploadInput& operator=(const RefineUploadInput&);
		struct Tex;
		Tex* m_tex;
		LumaPairView m_view;
		double m_uploadMs = 0;
	};

	struct RefineGpuTiming
	{
		bool valid = false;
		double gpuMs = 0;                           // GPU time of the refinement dispatches (timestamp queries)
	};

	// One per producer worker (owns its shaders and scratch on that worker's device). NOT thread-safe; the device
	// context is the caller's (the producer worker's immediate context).
	class PhotometricRefinerGpu
	{
	public:
		explicit PhotometricRefinerGpu(const RefineOptions& options = RefineOptions());
		~PhotometricRefinerGpu();
		// Compiles the kernels and checks the device (SM 5.0 double ops). False = unavailable, with the reason.
		bool Init(ID3D11Device* device, std::string* why);
		bool Available() const { return m_ready; }
		const std::string& UnavailableReason() const { return m_why; }

		// Queues one refinement (one submission). False when nothing was queued: the call's result is then final and
		// returned by Collect at once (Skipped* statuses, exactly as the CPU reference; or the GPU is unavailable,
		// reported as SkippedNoLuma with the reason in LastError()). picture / mask / model as PhotometricRefiner::Refine.
		bool Submit(ID3D11DeviceContext* context, const Params& model, const RefineGpuLuma& luma, const Rect& picture, const InlierMask& mask);
		// The result of the last Submit. wait = false returns false while the GPU has not finished (poll again later).
		bool Collect(ID3D11DeviceContext* context, bool wait, RefineResult* out);
		// Submit + Collect(wait).
		RefineResult Refine(ID3D11DeviceContext* context, const Params& model, const RefineGpuLuma& luma, const Rect& picture, const InlierMask& mask);

		void EnableTiming(bool on) { m_timing = on; }
		const RefineGpuTiming& LastTiming() const { return m_lastTiming; }
		const std::string& LastError() const { return m_lastError; }
		const RefineCounters& Counters() const { return m_counters; }
		const RefineOptions& Options() const { return m_options; }
		void ResetCounters() { m_counters = RefineCounters(); }

	private:
		PhotometricRefinerGpu(const PhotometricRefinerGpu&);
		PhotometricRefinerGpu& operator=(const PhotometricRefinerGpu&);
		struct Impl;
		Impl* m_impl;
		RefineOptions m_options;
		RefineCounters m_counters;
		bool m_ready = false;
		bool m_timing = false;
		std::string m_why, m_lastError;
		RefineGpuTiming m_lastTiming;
	};
}
