#pragma once

#include <libplacebo/dispatch.h>
#include <libplacebo/gpu.h>

#include <VideoFrameEncoding.h>

#include <string>

// dv_tunnel_gpu_unpack (owner 2026-10-16, docs/audit/dv-extension-ranking-2026-10-16.md
// item 7): the Dolby Vision RGB tunnel's picture unpacked on the GPU. The CPU
// formatter (CR210DolbyVisionTunnelFormatter::AnalyzeFrame) still finds and
// CRC-checks the DM packet, reading only the packet's pixels; the raw r210
// frame is uploaded as it is and one compute pass writes the formatter's two
// planes exactly (P216: r16 12-bit intensity << 4, rg16 alternating chroma
// (C0, C1) per pixel pair, every row). Bit-exact with the CPU unpack: the
// shader's arithmetic is transcribed in DolbyVisionTunnelUnpackReference.h
// and tested against it. The planes are storable and linear (the de-judder
// warps them), the same as GpuCaptureUnpack's P210.
class GpuDolbyVisionTunnelUnpack
{
public:
	GpuDolbyVisionTunnelUnpack(pl_log log, pl_gpu gpu);
	~GpuDolbyVisionTunnelUnpack();
	GpuDolbyVisionTunnelUnpack(const GpuDolbyVisionTunnelUnpack&) = delete;
	GpuDolbyVisionTunnelUnpack& operator=(const GpuDolbyVisionTunnelUnpack&) = delete;

	// The tunnel containers: r210, R10b, R10l (an R10 word is the r210 word
	// << 2, R10l little endian; the padding bits are ignored).
	static bool Supports(VideoFrameEncoding encoding);
	// data: the raw frame, rowBytes per row (DeckLink's stride), width x
	// height pixels (even width). highAligned: RGB8 in the high eight bits of
	// each 10-bit channel (the formatter's PlanesHighAligned()). planes[0] =
	// intensity (width x height), planes[1] = chroma (width / 2 x height);
	// created or recreated here. uploaded: the raw frame already on the GPU
	// (EarlyUpload, rgba8, rowBytes / 4 texels wide); then data is not read.
	bool Unpack(VideoFrameEncoding encoding, const void* data, size_t rowBytes, int width, int height, bool highAligned, pl_tex (&planes)[2],
		pl_tex uploaded = nullptr);
	const std::string& Error() const { return error; }
	// Tests: host-readable planes, and a GPU timer on the unpack pass.
	void SetHostReadableForTest(bool on) { hostReadable = on; }
	void SetTimerForTest(pl_timer t) { timer = t; }

private:
	bool hostReadable = false;
	pl_timer timer = nullptr;
	pl_gpu gpu = nullptr;
	pl_dispatch dispatch = nullptr;
	pl_tex raw = nullptr;
	std::string error;
};
