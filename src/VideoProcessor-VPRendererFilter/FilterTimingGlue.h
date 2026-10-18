#pragma once

// VP Renderer DirectShow filter: the seam between DirectShow (media types,
// REFERENCE_TIME, IReferenceClock, IMediaSideData) and VP (VideoState,
// FileFrameTiming, QPC). Integration of the graph-free helpers:
//   filesource/FileMediaTypeAdapter.h  colorimetry -> EOTF / ColorSpace
//   filesource/RationalRate.h          AvgTimePerFrame -> exact rational,
//                                      checked against the sample PTS deltas
//   filesource/FileClockMapper.h       reference clock <-> QPC (smoothed fit)
//   filesource/FilePlaybackState.h     stop/pause/run/flush/segment/step ->
//                                      discontinuity, held picture, rate
//   filesource/FileSideData.h          LAV side data -> HDRData / DV metadata
// Everything here is free of the DirectShow base classes so the CPU tests
// compile it directly (VPRendererFilterTests.cpp).

#include <ITimingClock.h>
#include <VideoState.h>
#include <FileFrameTiming.h>
#include <HDRData.h>

#include <filesource/FileClockMapper.h>
#include <filesource/FilePlaybackState.h>
#include <filesource/RationalRate.h>
#include <filesource/FileSideData.h>

#include <mutex>
#include <string>

#include "VPRendererFilterMediaTypes.h"


namespace VPRendererFilter
{
	// QueryPerformanceCounter as VP's ITimingClock: FileFrameTiming's
	// presentationQpc is in this clock's ticks.
	class QpcTimingClock final : public ITimingClock
	{
	public:
		QpcTimingClock();
		timingclocktime_t TimingClockNow() override;
		timingclocktime_t TimingClockTicksPerSecond() const override;
		const TCHAR* TimingClockDescription() override;
	private:
		timingclocktime_t m_frequency = 0;
	};


	// Colorimetry of a parsed format in VP's terms. An unsignalled colour space
	// follows the capture path's height rule (FileMediaTypeAdapter.h); an
	// unsignalled or non-display transfer is SDR (VP has no "unknown" EOTF path
	// for a live picture), reported through eotfAssumed.
	struct FileColorDecision
	{
		EOTF eotf = EOTF::SDR;
		ColorSpace colorSpace = ColorSpace::REC_709;
		bool eotfAssumed = false;
		bool colorSpaceFromHeight = false;
	};
	// hasStaticHdr: HDR10 static metadata came with the stream; an unsignalled
	// transfer is then PQ (assumed) rather than SDR (bug hunt 3 C7).
	FileColorDecision DecideColor(const VideoFormat& format, bool hasStaticHdr = false);

	// A valid VideoState for a parsed format at an exact rate. hdr: the static
	// HDR10 metadata from side data (null: none seen; an empty HDRData is sent).
	bool BuildFileVideoState(const VideoFormat& format, const RationalRate& rate,
		const HDRData* hdr, VideoStateComPtr& state, std::string& reason);


	// The filter's timing model: one lock around FilePlaybackState (graph state,
	// rate, discontinuities), FileClockMapper (reference clock -> QPC) and the
	// PTS rate verifier. Called from the streaming thread and the filter's
	// state-change methods.
	class FileTimingModel
	{
	public:
		void Stop();
		void Pause();
		void Run(int64_t tStart);
		void BeginFlush();
		void EndFlush();
		// True when the playback rate changed (a discontinuity follows anyway).
		bool NewSegment(int64_t start, int64_t stop, double rate);
		void FrameStep();
		// A format change or any other cause the graph does not signal.
		void MarkDiscontinuity();

		void SetAvOffsetMs(int32_t ms);
		int32_t AvOffsetMs() const;

		// One reference-clock/QPC correlation (filesource::SampleReferenceClockCorrelation).
		void AddCorrelation(const filesource::ClockCorrelation& correlation);

		// Feeds the PTS verifier. Returns the rate to use for this stream (the
		// snapped AvgTimePerFrame, or the measured rate when the header is 0 or
		// disagrees with the timestamps); invalid while still unknown.
		// Bug hunt 3 (C1): DirectShow sources rate-adjust their timestamps, so
		// the deltas are scaled back by the playback rate: the source rate is
		// the same at 0.5x, 1x and 2x (FileFrameTiming carries the speed).
		// `current` (the rate the renderer already has) is kept while the
		// timestamps still agree with it, so a refinement of the measurement
		// never changes the renderer's mode.
		RationalRate ObserveSample(bool hasStart, int64_t start, bool discontinuity,
			int64_t avgTimePerFrame, const RationalRate& current = RationalRate());

		// Timing for the next delivered sample (consumes a pending
		// discontinuity). DirectShow sources rate-adjust their timestamps, so the
		// presentation reference time is tStart + rtStart (+ file_av_offset_ms).
		FileFrameTiming BuildTiming(const filesource::FileSampleTimes& sample,
			const RationalRate& rate);

		double PlaybackRate() const;
		bool MotionAllowed() const;

	private:
		mutable std::mutex m_mutex;
		filesource::FilePlaybackState m_playback;
		filesource::FileClockMapper m_mapper;
		filesource::PtsRateVerifier m_verifier;
		bool m_extraDiscontinuity = false;
		int32_t m_avOffsetMs = 0;
	};


	// Bug hunt 3 (C1): what the filter needs from its renderer to keep it
	// showing frames across media-type, rate, seek and pause changes. The
	// renderer may refuse a new state in place (a different display mode,
	// pixel format or effective profile); the filter then replaces it with a
	// fresh renderer that takes the state as its first one. Graph-free so the
	// CPU tests drive every event order against a stand-in renderer.
	class IFileVideoStateSink
	{
	public:
		virtual ~IFileVideoStateSink() = default;
		// The current renderer's OnVideoState: false = refused in place.
		virtual bool SendVideoState(VideoStateComPtr& state) = 0;
		// Destroys the current renderer and creates a fresh one (not yet built).
		// False (with reason) when no renderer could be made.
		virtual bool RestartRenderer(std::string& reason) = 0;
		// Builds and starts the fresh renderer once its first state was offered.
		virtual void StartRestartedRenderer() = 0;
	};

	enum class FileStateOutcome { Unchanged, Sent, SentAfterRestart, Deferred, Failed };
	const char* ToString(FileStateOutcome outcome);

	// The state the current renderer was given, and the send / restart rule.
	class FileVideoStateTracker
	{
	public:
		// Sends `format` at `rate` (with static HDR10 metadata when hdr is not
		// null) when the format generation, the rate or that metadata changed
		// since the last accepted state. An invalid rate keeps the rate already
		// sent (a format or HDR change is still sent); with none sent yet the
		// state is deferred. A refusal restarts the renderer once and offers
		// the state to the fresh one.
		FileStateOutcome Ensure(const VideoFormat& format, uint64_t generation, const RationalRate& rate,
			const HDRData* hdr, IFileVideoStateSink& sink, std::string& reason);
		// A new renderer that has been given nothing yet.
		void Reset();
		bool Sent() const { return m_sent; }
		const RationalRate& SentRate() const { return m_rate; }
		uint64_t SentGeneration() const { return m_generation; }
		uint32_t Restarts() const { return m_restarts; }

	private:
		bool m_sent = false;
		uint64_t m_generation = 0;
		RationalRate m_rate;
		bool m_hdrValid = false;
		HDRData m_hdr;
		uint32_t m_restarts = 0;
	};


	// Side data on one sample, reduced to what the renderer can take today:
	// a static HDR10 metadata change (mastering display, content light level)
	// triggers a new VideoState. Dolby Vision and HDR10+ are parsed and counted,
	// but ABI 37 has no per-frame entry point for them yet.
	class FileSideDataTracker
	{
	public:
		using Getter = HRESULT(*)(void* context, const GUID& type, const BYTE** data, size_t* size);
		// Reads every known side-data type through getter. True when the static
		// HDR10 metadata changed.
		bool Update(Getter getter, void* context);
		void Reset();
		bool HasStaticHdr() const;
		const HDRData& StaticHdr() const;
		uint64_t DolbyVisionFrames() const { return m_dolbyVisionFrames; }
		uint64_t Hdr10PlusFrames() const { return m_hdr10PlusFrames; }
		const FileSideData::SideDataSnapshot& Last() const { return m_last; }

	private:
		FileSideData::StaticHdrChangeDetector m_detector;
		FileSideData::SideDataSnapshot m_last;
		uint64_t m_dolbyVisionFrames = 0;
		uint64_t m_hdr10PlusFrames = 0;
	};
}
