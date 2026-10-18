#include <pch.h>

#include "FilterTimingGlue.h"

#include <DisplayMode.h>
#include <filesource/FileMediaTypeAdapter.h>

#include <exception>
#include <memory>


namespace VPRendererFilter
{
	QpcTimingClock::QpcTimingClock()
	{
		LARGE_INTEGER frequency = {};
		QueryPerformanceFrequency(&frequency);
		m_frequency = frequency.QuadPart;
	}

	timingclocktime_t QpcTimingClock::TimingClockNow()
	{
		LARGE_INTEGER now = {};
		QueryPerformanceCounter(&now);
		return now.QuadPart;
	}

	timingclocktime_t QpcTimingClock::TimingClockTicksPerSecond() const
	{
		return m_frequency;
	}

	const TCHAR* QpcTimingClock::TimingClockDescription()
	{
		return TEXT("QueryPerformanceCounter (VP Renderer filter)");
	}


	//
	// Format -> VideoState
	//

	FileColorDecision DecideColor(const VideoFormat& format)
	{
		// The pin parsed dwControlFlags into fields; FileMediaTypeAdapter decodes
		// the packed DXVA_ExtendedFormat, so re-pack them (lossless).
		const uint32_t flags = format.colorimetry.present ?
			filesource::dxva::PackControlFlags(format.colorimetry.nominalRange,
				format.colorimetry.transferMatrix, format.colorimetry.primaries,
				format.colorimetry.transferFunction) : 0u;
		const filesource::FileColorimetry c = filesource::DecodeColorimetry(flags, format.height);

		FileColorDecision d;
		d.eotf = c.eotf;
		d.colorSpace = c.colorSpace;
		d.colorSpaceFromHeight = c.colorSpaceFromHeight;
		if (d.eotf == EOTF::UNKNOWN)
		{
			d.eotf = EOTF::SDR;
			d.eotfAssumed = true;
		}
		if (d.colorSpace == ColorSpace::UNKNOWN)
		{
			d.colorSpace = filesource::ColorSpaceFromHeight(format.height, d.eotf);
			d.colorSpaceFromHeight = true;
		}
		return d;
	}


	bool BuildFileVideoState(const VideoFormat& format, const RationalRate& rate,
		const HDRData* hdr, VideoStateComPtr& state, std::string& reason)
	{
		if (!rate.IsValid())
		{
			reason = "frame rate unknown (no AvgTimePerFrame and too few timestamps yet)";
			return false;
		}
		try
		{
			const DisplayMode::FieldOrder order = !format.interlaced ?
				DisplayMode::FieldOrder::Unknown : (format.topFieldFirst ?
					DisplayMode::FieldOrder::TopFirst : DisplayMode::FieldOrder::BottomFirst);
			auto displayMode = std::make_shared<DisplayMode>(format.width, format.height,
				format.interlaced, rate.numerator, rate.denominator, order);

			const FileColorDecision color = DecideColor(format);
			VideoStateComPtr result(new VideoState());
			result->valid = true;
			result->displayMode = displayMode;
			result->videoFrameEncoding = format.encoding;
			result->colorspace = color.colorSpace;
			result->eotf = color.eotf;
			result->invertedVertical = false;   // the repack flips bottom-up RGB
			result->dolbyVisionTunnel = false;
			result->hdrData = hdr ? std::make_shared<HDRData>(*hdr) : std::make_shared<HDRData>();
			state = result;
			return true;
		}
		catch (const std::exception& e)
		{
			reason = std::string("display mode rejected: ") + e.what();
			return false;
		}
	}


	//
	// Timing model
	//

	void FileTimingModel::Stop()
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_playback.Stop();
		m_mapper.Reset();
		m_verifier.OnDiscontinuity();
	}

	void FileTimingModel::Pause()
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_playback.Pause();
	}

	void FileTimingModel::Run(int64_t tStart)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_playback.Run(tStart);
	}

	void FileTimingModel::BeginFlush()
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_playback.BeginFlush();
		m_verifier.OnDiscontinuity();
	}

	void FileTimingModel::EndFlush()
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_playback.EndFlush();
	}

	bool FileTimingModel::NewSegment(int64_t start, int64_t stop, double rate)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_verifier.OnDiscontinuity();
		return m_playback.NewSegment(start, stop, rate);
	}

	void FileTimingModel::FrameStep()
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_playback.FrameStep();
	}

	void FileTimingModel::MarkDiscontinuity()
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_extraDiscontinuity = true;
	}

	void FileTimingModel::SetAvOffsetMs(int32_t ms)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_avOffsetMs = ms;
	}

	int32_t FileTimingModel::AvOffsetMs() const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_avOffsetMs;
	}

	void FileTimingModel::AddCorrelation(const filesource::ClockCorrelation& correlation)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_mapper.AddCorrelation(correlation);
	}

	RationalRate FileTimingModel::ObserveSample(bool hasStart, int64_t start,
		bool discontinuity, int64_t avgTimePerFrame)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (discontinuity)
			m_verifier.OnDiscontinuity();
		if (hasStart)
			m_verifier.AddSampleStart(start);
		return m_verifier.EffectiveRate(avgTimePerFrame);
	}

	FileFrameTiming FileTimingModel::BuildTiming(const filesource::FileSampleTimes& sample,
		const RationalRate& rate)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		FileFrameTiming timing = m_playback.BuildTiming(sample, rate, 0.0, m_mapper,
			m_avOffsetMs, true);
		if (m_extraDiscontinuity)
		{
			timing.discontinuity = true;
			m_extraDiscontinuity = false;
		}
		return timing;
	}

	double FileTimingModel::PlaybackRate() const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_playback.PlaybackRate();
	}

	bool FileTimingModel::MotionAllowed() const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_playback.MotionAllowed();
	}


	//
	// Side data
	//

	bool FileSideDataTracker::Update(Getter getter, void* context)
	{
		FileSideData::SideDataSnapshot snapshot;
		if (getter)
		{
			const size_t count = FileSideData::KnownSideDataTypeCount();
			for (size_t i = 0; i < count; ++i)
			{
				const GUID& type = FileSideData::KnownSideDataType(i);
				const BYTE* data = nullptr;
				size_t size = 0;
				if (SUCCEEDED(getter(context, type, &data, &size)) && data && size)
					FileSideData::AddToSnapshot(snapshot, type, data, size);
			}
		}
		if (snapshot.hasDolbyVision)
			++m_dolbyVisionFrames;
		if (snapshot.hasHdr10Plus)
			++m_hdr10PlusFrames;
		m_last = snapshot;
		return m_detector.Update(snapshot);
	}

	void FileSideDataTracker::Reset()
	{
		m_detector.Reset();
		m_last = FileSideData::SideDataSnapshot();
		m_dolbyVisionFrames = 0;
		m_hdr10PlusFrames = 0;
	}

	bool FileSideDataTracker::HasStaticHdr() const
	{
		return m_detector.HasMasteringDisplay() || m_detector.HasContentLight();
	}

	const HDRData& FileSideDataTracker::StaticHdr() const
	{
		return m_detector.Current();
	}
}
