#pragma once

#include <libplacebo/dispatch.h>
#include <libplacebo/gpu.h>

#include <VideoFrameEncoding.h>
#include <video_frame_formatter/IVideoFrameFormatter.h>

#include <string>

// Owner 2026-10-13 (docs/VP-gpu-capture-low-latency-plan.md): the capture
// path's GPU unpack. With `capture_path: gpu` the conversion thread hands
// the raw DeckLink frame on instead of converting it on the CPU; this
// uploads its bytes as they are and a compute pass writes the same two
// planes the CPU formatters produce (P210: 16-bit luma, interleaved 16-bit
// CbCr at every row, the value in the high bits), exactly:
//   V210        CV210toP210VideoFrameFormatter  (10-bit codes << 6)
//   UYVY, HDYC  CUYVYtoP210VideoFrameFormatter  (8-bit values << 8)
// The planes are storable and linear (the de-judder warps them).
class GpuCaptureUnpack
{
public:
	GpuCaptureUnpack(pl_log log, pl_gpu gpu);
	~GpuCaptureUnpack();
	GpuCaptureUnpack(const GpuCaptureUnpack&) = delete;
	GpuCaptureUnpack& operator=(const GpuCaptureUnpack&) = delete;

	static bool Supports(VideoFrameEncoding encoding);
	// The contract of the planes: as the CPU formatter's.
	static VideoFrameFormatterOutputContract Contract(VideoFrameEncoding encoding);

	// data: the raw frame, rowBytes per row (DeckLink's stride), width x
	// height pixels (even width). planes[0] = luma (width x height),
	// planes[1] = CbCr ((width + 1) / 2 x height); created or recreated here.
	bool Unpack(VideoFrameEncoding encoding, const void* data, size_t rowBytes, int width, int height, pl_tex (&planes)[2]);
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
