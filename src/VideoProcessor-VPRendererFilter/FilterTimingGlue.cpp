#include <pch.h>

#include "FilterTimingGlue.h"
#include "FilterTimingGlueMath.h"

#include <DebugLog.h>
#include <DisplayMode.h>
#include <HDRData.h>

#include <atomic>
#include <exception>


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


	namespace
	{
		// TODO(VP Renderer integration, agent C): FileMediaTypeAdapter.h.
		class LocalFormatAdapter final : public IFilterFormatAdapter
		{
		public:
			bool BuildVideoState(const VideoFormat& format, int64_t frameDuration100ns,
				VideoStateComPtr& state, RationalRate& rate, std::string& reason) override
			{
				rate = RationalFromFrameDuration(frameDuration100ns);
				if (rate.numerator == 0 || rate.denominator == 0)
				{
					reason = "frame rate unknown (no AvgTimePerFrame and no sample duration yet)";
					return false;
				}
				try
				{
					auto displayMode = std::make_shared<DisplayMode>(format.width, format.height,
						format.interlaced, rate.numerator, rate.denominator,
						format.interlaced ? (format.topFieldFirst ?
							DisplayMode::FieldOrder::TopFirst : DisplayMode::FieldOrder::BottomFirst) :
							DisplayMode::FieldOrder::Unknown);

					VideoStateComPtr result(new VideoState());
					result->valid = true;
					result->displayMode = displayMode;
					result->videoFrameEncoding = format.encoding;
					result->colorspace = ColorSpaceFromColorimetry(format.colorimetry);
					result->eotf = EotfFromColorimetry(format.colorimetry);
					if (result->colorspace == ColorSpace::UNKNOWN || result->eotf == EOTF::UNKNOWN)
					{
						// Local stand-in policy until FileMediaTypeAdapter.h: an
						// unsignalled stream is treated as BT.709 SDR (logged).
						if (!m_loggedDefaultColorimetry.exchange(true))
							DebugLog::Log("VP Renderer filter: colorimetry not signalled "
								"(present=%d primaries=%u transfer=%u); using BT.709 SDR",
								format.colorimetry.present ? 1 : 0,
								format.colorimetry.primaries, format.colorimetry.transferFunction);
						if (result->colorspace == ColorSpace::UNKNOWN)
							result->colorspace = ColorSpace::REC_709;
						if (result->eotf == EOTF::UNKNOWN)
							result->eotf = EOTF::SDR;
					}
					result->invertedVertical = false;   // the repack flips bottom-up RGB
					result->dolbyVisionTunnel = false;
					// TODO(VP Renderer integration, agent F): side-data HDR metadata.
					result->hdrData = std::make_shared<HDRData>();
					state = result;
					return true;
				}
				catch (const std::exception& e)
				{
					reason = e.what();
					return false;
				}
			}

		private:
			std::atomic<bool> m_loggedDefaultColorimetry{ false };
		};


		// TODO(VP Renderer integration, agent C): FileClockMapper.h (smoothed
		// reference clock <-> QPC correlation instead of one sample per frame).
		class LocalClockAdapter final : public IFilterClockAdapter
		{
		public:
			LocalClockAdapter()
			{
				LARGE_INTEGER frequency = {};
				QueryPerformanceFrequency(&frequency);
				m_frequency = frequency.QuadPart;
			}

			void Reset() override {}

			void SetAvOffset100ns(int64_t offset) override
			{
				m_avOffset.store(offset, std::memory_order_relaxed);
			}

			int64_t PresentationQpc(IReferenceClock* clock, int64_t runStart,
				int64_t sampleStart) override
			{
				if (!clock)
					return 0;
				REFERENCE_TIME referenceNow = 0;
				LARGE_INTEGER qpcNow = {};
				QueryPerformanceCounter(&qpcNow);
				if (FAILED(clock->GetTime(&referenceNow)))
					return 0;
				const int64_t due = runStart + sampleStart +
					m_avOffset.load(std::memory_order_relaxed);
				return ReferenceTimeToQpc(due, referenceNow, qpcNow.QuadPart, m_frequency);
			}

		private:
			int64_t m_frequency = 0;
			std::atomic<int64_t> m_avOffset{ 0 };
		};
	}


	std::unique_ptr<IFilterFormatAdapter> CreateLocalFormatAdapter()
	{
		return std::unique_ptr<IFilterFormatAdapter>(new LocalFormatAdapter());
	}

	std::unique_ptr<IFilterClockAdapter> CreateLocalClockAdapter()
	{
		return std::unique_ptr<IFilterClockAdapter>(new LocalClockAdapter());
	}
}
