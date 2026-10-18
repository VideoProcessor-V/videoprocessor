#include <pch.h>

#include "FilterTimingGlue.h"

#include <DisplayMode.h>
#include <filesource/FileMediaTypeAdapter.h>

#include <cmath>
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

	FileColorDecision DecideColor(const VideoFormat& format, bool hasStaticHdr)
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
			// Bug hunt 3 (C7): an unsignalled transfer that carries HDR10 static
			// metadata (mastering display / content light level side data) is
			// PQ, not SDR - only HDR10 carries that metadata.
			d.eotf = hasStaticHdr ? EOTF::PQ : EOTF::SDR;
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

			const FileColorDecision color = DecideColor(format, hdr != nullptr);
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
		bool discontinuity, int64_t avgTimePerFrame, const RationalRate& current)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (discontinuity)
			m_verifier.OnDiscontinuity();
		if (hasStart)
		{
			// Source time: the rate-adjusted timestamp times the playback rate
			// (a new segment resets the verifier, so one rate per history).
			const double rate = m_playback.PlaybackRate();
			m_verifier.AddSampleStart(rate > 0.0 && std::isfinite(rate) ?
				static_cast<int64_t>(std::llround(static_cast<double>(start) * rate)) : start);
		}
		if (current.IsValid() &&
			m_verifier.Check(current) != filesource::PtsRateVerifier::Verdict::Disagrees)
		{
			// Nothing measured yet (a new segment) keeps the current rate unless
			// the header names another; a measurement keeps it while it agrees.
			const RationalRate header = filesource::SnapAvgTimePerFrame(avgTimePerFrame);
			if (m_verifier.HasMeasurement() || !header.IsValid() || header == current)
				return current;
		}
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
	// Renderer state across stream changes
	//

	const char* ToString(FileStateOutcome outcome)
	{
		switch (outcome)
		{
		case FileStateOutcome::Unchanged: return "unchanged";
		case FileStateOutcome::Sent: return "sent";
		case FileStateOutcome::SentAfterRestart: return "sent after a renderer restart";
		case FileStateOutcome::Deferred: return "deferred";
		case FileStateOutcome::Failed: return "failed";
		}
		return "?";
	}

	void FileVideoStateTracker::Reset()
	{
		m_sent = false;
		m_generation = 0;
		m_rate = RationalRate();
		m_hdrValid = false;
		m_hdr = HDRData();
	}

	FileStateOutcome FileVideoStateTracker::Ensure(const VideoFormat& format, uint64_t generation,
		const RationalRate& rate, const HDRData* hdr, IFileVideoStateSink& sink, std::string& reason)
	{
		const bool hasHdr = hdr != nullptr;
		const bool hdrChanged = hasHdr != m_hdrValid || (hasHdr && *hdr != m_hdr);
		if (m_sent && generation == m_generation && (!rate.IsValid() || rate == m_rate) && !hdrChanged)
			return FileStateOutcome::Unchanged;
		// No rate from this sample (a new segment of a header-less stream): the
		// rate already sent stays; a format or HDR change still goes out.
		const RationalRate sendRate = rate.IsValid() ? rate : m_rate;
		if (!sendRate.IsValid())
		{
			reason = "frame rate unknown (no AvgTimePerFrame and too few timestamps yet)";
			return FileStateOutcome::Deferred;
		}

		VideoStateComPtr state;
		if (!BuildFileVideoState(format, sendRate, hdr, state, reason))
			return FileStateOutcome::Failed;
		FileStateOutcome outcome = FileStateOutcome::Sent;
		if (!sink.SendVideoState(state))
		{
			// Refused in place: a fresh renderer takes it as its first state.
			Reset();
			if (!sink.RestartRenderer(reason))
			{
				if (reason.empty())
					reason = "the renderer refused the video state and could not be restarted";
				return FileStateOutcome::Failed;
			}
			++m_restarts;
			VideoStateComPtr fresh;
			const bool built = BuildFileVideoState(format, sendRate, hdr, fresh, reason);
			const bool accepted = built && sink.SendVideoState(fresh);
			sink.StartRestartedRenderer();
			if (!accepted)
			{
				if (built)
					reason = "the restarted renderer refused the video state";
				return FileStateOutcome::Failed;
			}
			outcome = FileStateOutcome::SentAfterRestart;
		}
		m_sent = true;
		m_generation = generation;
		m_rate = sendRate;
		m_hdrValid = hasHdr;
		m_hdr = hasHdr ? *hdr : HDRData();
		return outcome;
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
