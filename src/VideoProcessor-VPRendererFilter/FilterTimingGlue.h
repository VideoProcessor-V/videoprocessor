#pragma once

// VP Renderer DirectShow filter: the seam between DirectShow (media types,
// REFERENCE_TIME, IReferenceClock) and VP (VideoState, FileFrameTiming, QPC).
//
// The filter only talks to IFilterFormatAdapter / IFilterClockAdapter. The
// Local* implementations here are deliberately minimal stand-ins.
//
// TODO(VP Renderer integration, agent C): swap the Local* adapters for the
// pure helpers in src/VideoProcessor-Lib/filesource/:
//   FileMediaTypeAdapter.h  media type -> encoding/size/colorimetry (VideoState)
//   FileClockMapper.h       REFERENCE_TIME + IReferenceClock correlation -> QPC
//   RationalRate.h          AvgTimePerFrame -> exact rational, PTS-checked
// Integration is a swap of CreateLocalFormatAdapter / CreateLocalClockAdapter.
// TODO(VP Renderer integration, agent D): file_av_offset_ms and the renderer
// settings feed IFilterClockAdapter::SetAvOffset100ns.
// TODO(VP Renderer integration, agent F): HDR mastering metadata / MaxCLL and
// Dolby Vision side data fill VideoState::hdrData (empty HDRData until then).

#include <ITimingClock.h>
#include <VideoState.h>

#include <memory>
#include <string>

#include "VPRendererFilterMediaTypes.h"

struct IReferenceClock;


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

	class IFilterFormatAdapter
	{
	public:
		virtual ~IFilterFormatAdapter() {}
		// Builds a valid VideoState for a parsed format. frameDuration100ns is
		// the format's AvgTimePerFrame, or a measured sample duration when the
		// format has none. rate receives the exact source rate.
		virtual bool BuildVideoState(const VideoFormat& format, int64_t frameDuration100ns,
			VideoStateComPtr& state, RationalRate& rate, std::string& reason) = 0;
	};

	class IFilterClockAdapter
	{
	public:
		virtual ~IFilterClockAdapter() {}
		// Flush, seek, stop or a new segment: forget any correlation history.
		virtual void Reset() = 0;
		virtual void SetAvOffset100ns(int64_t offset) = 0;
		// QPC tick at which a sample is due. runStart is the filter's tStart,
		// sampleStart the stream-relative start time. 0 = unknown (present as
		// soon as possible), e.g. without a clock.
		virtual int64_t PresentationQpc(IReferenceClock* clock, int64_t runStart,
			int64_t sampleStart) = 0;
	};

	std::unique_ptr<IFilterFormatAdapter> CreateLocalFormatAdapter();
	std::unique_ptr<IFilterClockAdapter> CreateLocalClockAdapter();
}
