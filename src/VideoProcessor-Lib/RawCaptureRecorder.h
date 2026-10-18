#pragma once
/*
 * Copyright(C) 2026 Bill Slack
 * SPDX-License-Identifier: GPL-3.0-only
 */
// Raw capture recording (DV tunnel autotest Tier 3 and the LLDV-vs-tunnel
// check, 2026-10-17). Opt-in per process, modelled on VP_SUBTITLE_CAPTURE_PATH:
// nothing happens unless VP_RAW_CAPTURE_PATH names a folder (put it on a large
// drive, never inside a source checkout - captured pixels are not committed).
//
//   VP_RAW_CAPTURE_PATH        root folder; enables the recorder
//   VP_RAW_CAPTURE_BURST=N     also record N consecutive frames automatically
//                              after each new capture state (first 4 states),
//                              VP_RAW_CAPTURE_SKIP frames (default: 1 s worth)
//                              after the state starts - the regression corpus
//   VP_RAW_CAPTURE_EVERY=k     continuous recording keeps every k-th frame (1)
//   VP_RAW_CAPTURE_MAX_SECONDS continuous recording stops by itself after this
//                              much capture time (default 30)
//   VP_RAW_CAPTURE_MIN_FREE_GB stop when the drive has less free space (30)
//   VP_RAW_CAPTURE_QUEUE_MB    frames waiting for the disk (default 2048); a
//                              frame that does not fit is skipped and counted
//   VP_RAW_CAPTURE_NOTE        free text stored in every sidecar (e.g. "Integral
//                              2 EDID: LLDV 1000 nits" or "TV-led DV EDID")
//
// Continuous recording ("record until stopped") is toggled with the
// raw_capture_toggle shortcut (default Ctrl+Alt+D) or by creating the file
// RECORD in the root folder (deleting it, or a file STOP, stops it). Start and
// stop are logged ("Raw capture: recording started ...") and shown on screen.
//
// Layout: <root>\<yyyymmdd-hhmmss>-<kind>\segNN\ with capture.txt (format,
// card, card-reported input encoding / bit depth, capture state: raster, rate,
// encoding, row bytes, EOTF, colorimetry, HDR InfoFrame values, the DV tunnel
// switch, note), frames.csv (sequence, file, source counter, timing / capture
// timestamps, arrival QPC) and frame_NNNNNN.bin (BytesPerFrame raw bytes). A
// capture-state change starts the next segment. Frames are copied on the
// capture thread into pooled buffers and written by one background thread.
#include <VideoFrame.h>
#include <VideoState.h>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct RawCaptureOptions
{
	std::string root;
	uint32_t burstFrames = 0;
	int64_t burstSkipFrames = -1;     // -1 = one second worth at the state's rate
	uint32_t burstStates = 4;
	uint32_t every = 1;
	double maxSeconds = 30.0;
	double minFreeGb = 30.0;
	size_t queueBytes = size_t(2048) << 20;
	std::string note;
	bool watchFiles = true;           // RECORD / STOP files in the root

	// From the environment (see above); root empty when not enabled.
	static RawCaptureOptions FromEnvironment();
};

class RawCaptureRecorder
{
public:
	explicit RawCaptureRecorder(const RawCaptureOptions& options);
	~RawCaptureRecorder();
	RawCaptureRecorder(const RawCaptureRecorder&) = delete;
	RawCaptureRecorder& operator=(const RawCaptureRecorder&) = delete;

	// The process-wide recorder (options from the environment).
	static RawCaptureRecorder& Instance();

	bool Enabled() const { return !m_options.root.empty(); }
	bool Recording() const { return m_continuous.load(); }

	// Capture-thread callbacks. Cheap no-ops when disabled.
	void OnCardState(const std::string& card, const std::string& inputEncoding,
		const std::string& inputBitDepth, const std::vector<std::string>& other);
	void OnVideoState(const VideoState& state);
	void OnFrame(const VideoFrame& frame);
	// What VP built from the capture state (the DV tunnel switch, the newlldv /
	// LLDV decision), recorded in the next sidecar.
	void SetProcessingNote(const std::string& text);

	// Continuous recording on / off; returns the status line to show.
	std::string Toggle();
	std::string Start(const char* reason);
	std::string Stop(const char* reason);

	// Written so far (tests).
	uint64_t FramesWritten() const { return m_framesWritten.load(); }
	uint64_t FramesSkipped() const { return m_framesSkipped.load(); }
	std::string SessionFolder() const;
	// Blocks until the queue is written (tests, shutdown).
	void Flush();

private:
	struct Job
	{
		std::string folder;
		std::string file;
		std::vector<uint8_t> bytes;
		std::string csvLine;
		bool sidecarOnly = false;
		std::string sidecar;
	};
	void WriterThread();
	void BeginSegmentLocked(const char* kind);
	std::string SidecarLocked() const;
	bool QueueFrameLocked(const VideoFrame& frame, const char* kind);
	void CheckFiles();

	RawCaptureOptions m_options;
	mutable std::mutex m_mutex;
	VideoStateComPtr m_state;
	std::string m_card, m_cardEncoding, m_cardBitDepth, m_processingNote;
	std::vector<std::string> m_cardOther;
	std::atomic<bool> m_continuous{ false };
	std::string m_session, m_segmentFolder;
	uint32_t m_segment = 0;
	uint64_t m_segmentFrames = 0, m_sequence = 0;
	uint64_t m_frameInState = 0;
	uint64_t m_continuousStartCounter = 0;
	bool m_continuousHaveStart = false;
	uint32_t m_statesSeen = 0;
	bool m_burstActive = false;
	uint64_t m_burstWritten = 0;
	uint64_t m_filesCheckedAt = 0;
	std::atomic<bool> m_startedByFile{ false }, m_recordArmed{ true };
	std::atomic<uint64_t> m_framesWritten{ 0 }, m_framesSkipped{ 0 };
	std::deque<Job> m_queue;
	size_t m_queuedBytes = 0;
	std::vector<std::vector<uint8_t>> m_pool;
	std::condition_variable m_wake, m_drained;
	bool m_stopThread = false;
	bool m_writing = false;
	std::thread m_thread;
};
