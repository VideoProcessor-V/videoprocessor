// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// D5-QK step K1 (design-check-P13-MOVE s3, refs/118 Phase B as amended by the queue-keyed design): the P13 UNPACK
// kernel. It is unpack ONLY: it reads one raw early-upload frame (an rgba8 texture, rowBytes / 4 texels per row, one
// texture row per frame row) and writes ONE luma plane, R16 unorm, MSB-aligned, so that P13 is a real shared plane
// that every reader (the P10 grain-split tap, P11, the S-3a look-ahead spare work, later TD) gets from the one
// computation. It never extends (the P10 reflect law is P10's own, a reader of this plane) and never fuses.
//
// Law (nothing re-implemented; each line is the existing CPU decode, pinned by SourceUnpackGpuTests):
//   Planar16  P010 / P210 luma plane: code = word16 >> 6 (AnalysisLumaSource P010Code), out = code << 6   (10 bit)
//   V210      the 10-bit Y field of the six-pixel group (AnalysisLumaSource ReadNativeYuv422), out = code << 6 (10 bit)
//   Uyvy8     UYVY / HDYC luma byte, out = byte << 8                                                        (8 bit)
//   DvTunnel  the DV tunnel's 12-bit intensity: DolbyVisionTunnelUnpackReference (the shared
//             DvTunnelGlsl::kHeader helpers, one GLSL copy with GpuDolbyVisionTunnelUnpack), tap12 = DvTunnelTapLaw::TapCode, out = tap12 << 4 (12 bit);
//             full range goes through DvTunnelTapLaw::BuildRangeTable (4096 entries, built by the CPU law, exact)
// SourceBits(packing) gives the plane's sourceBits (10 / 8 / 12); the owner fill (Track Q) stores it with the result,
// so P10 / P11 set their Params::sourceBits from it, never assume 10.
//
// Every refusal is a Cause (CauseName for the status line, the notice and the dump); the caller shows it, never only
// logs it (principles 5). The kernel owns no queue, no ring and no memo: the P13 owner fill (SourcePlanes.h) decides
// which seq and plane is demanded and where the owned plane lives.
//
// Threading: one SourceUnpackGpu per pl_gpu, used on the thread that owns that pl_gpu (the render thread).

#include <libplacebo/dispatch.h>
#include <libplacebo/gpu.h>

#include <VideoFrameEncoding.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace SourceUnpack
{
	// Bump on ANY change of a decode law or the output format (the tap-form unit version follows it at wiring).
	constexpr uint32_t kUnitVersion = 1;

	enum class Packing : uint32_t
	{
		Planar16 = 0,   // P010 / P210: the luma plane, 16-bit little-endian words
		V210 = 1,       // 10-bit 4:2:2, six pixels per 16 bytes
		Uyvy8 = 2,      // UYVY / HDYC, 8-bit
		DvTunnel = 3    // r210 / R10b / R10l carrying the DV tunnel (4 B per pixel)
	};

	// The significant source bits of a packing (the P13 plane's `sourceBits`).
	constexpr int SourceBits(Packing packing)
	{
		return packing == Packing::Uyvy8 ? 8 : (packing == Packing::DvTunnel ? 12 : 10);
	}

	// Why a plane did NOT come from the kernel (None: it did).
	enum class Cause : uint32_t
	{
		None = 0,
		NoGpu,              // no pl_gpu / dispatch
		BadGeometry,        // size, stride or width the packing cannot take
		DvContainer,        // a tunnel container outside DolbyVisionTunnelUnpackReference::SupportsContainer
		DvAlignment,        // RGB8 alignment is neither 1 (high) nor 2 (low)
		FullRangeYuv,       // W23-NEON-L2: fullRange is DvTunnel-only (YUV full range stays on the CPU path)
		NoFormat,           // no rgba8 / storable r16 format on this device
		RawUploadFailed,    // the host-bytes path could not create or upload the raw texture
		UploadedSizeDiffers,// the early-upload texture is not rowBytes / 4 x height
		OutputCreateFailed, // the R16 plane could not be created
		RangeTableFailed,   // the DV full-range table could not be created or uploaded
		ShaderRejected,     // libplacebo refused the shader
		DispatchFailed,     // the compute dispatch failed
		Count
	};
	// A short ASCII name ("geometry", "dv container", "dispatch", ...).
	const char* CauseName(Cause cause);

	struct Request
	{
		Packing packing = Packing::Planar16;
		int width = 0;
		int height = 0;
		size_t rowBytes = 0;                                        // the raw row stride (a multiple of 4)
		VideoFrameEncoding container = VideoFrameEncoding::UNKNOWN; // DvTunnel only: R210 / R10b / R10l
		int rgbBitAlignment = 1;                                    // DvTunnel only: 1 = high eight bits, 2 = low
		bool fullRange = false;                                     // DvTunnel only (YUV full range stays on the CPU path)
		int chromaSubsampleY = 1;                                   // Planar16: vertical chroma subsampling (P010 = 2, P210 = 1); the height must be a multiple
	};

	// Horizontal chroma subsampling factor of a packing's luma geometry (the pixel-pair grouping the CPU reference
	// AnalysisLumaSource::IsValid requires); 1 when chroma is not horizontally subsampled (the DV tunnel RGB container).
	constexpr int ChromaSubsampleX(Packing packing)
	{
		return packing == Packing::DvTunnel ? 1 : 2;
	}

	// The bytes one row of `width` pixels needs (rowBytes >= this; the raw texture row is rowBytes / 4 texels).
	size_t MinRowBytes(Packing packing, int width);

	// The cause a request would be refused with before any GPU work (None when the geometry and format are accepted).
	Cause Validate(const Request& request);

	class SourceUnpackGpu
	{
	public:
		SourceUnpackGpu(pl_log log, pl_gpu gpu);
		~SourceUnpackGpu();
		SourceUnpackGpu(const SourceUnpackGpu&) = delete;
		SourceUnpackGpu& operator=(const SourceUnpackGpu&) = delete;

		// Writes the luma plane of the raw frame into `out` (created or recreated: R16 unorm, width x height, sampleable
		// and storable). `uploaded` is the raw frame already on the GPU (an EarlyUpload slot, rgba8, rowBytes / 4 texels
		// wide, at least `height` rows); when null, `data` (rowBytes per row) is uploaded here. False with LastCause().
		bool Unpack(const Request& request, const void* data, pl_tex uploaded, pl_tex* out);

		Cause LastCause() const { return cause; }
		const std::string& LastError() const { return error; }
		// Tests: a host-readable plane, and a GPU timer on the unpack pass (the C' timing).
		void SetHostReadableForTest(bool on) { hostReadable = on; }
		void SetTimerForTest(pl_timer t) { timer = t; }

	private:
		bool Fail(Cause c, const char* message);
		bool EnsureRangeTable();

		pl_gpu gpu = nullptr;
		pl_dispatch dispatch = nullptr;
		pl_tex raw = nullptr;
		pl_tex rangeTable = nullptr;   // 4096 x 1 R16: entry c = DvTunnelTapLaw::FullLumaToLimited12(c) << 4
		bool hostReadable = false;
		pl_timer timer = nullptr;
		Cause cause = Cause::None;
		std::string error;
	};
}
