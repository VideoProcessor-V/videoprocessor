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
	FileColorDecision DecideColor(const VideoFormat& format);

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
		RationalRate ObserveSample(bool hasStart, int64_t start, bool discontinuity,
			int64_t avgTimePerFrame);

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


	// Side data on one sample, reduced to what the renderer can take today:
	// a static HDR10 metadata change (mastering display, content light level)
	// triggers a new VideoState. Dolby Vision and HDR10+ are parsed and counted,
	// but ABI 35 has no per-frame entry point for them yet.
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
