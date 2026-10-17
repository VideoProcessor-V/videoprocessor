#include "pch.h"
#include "CppUnitTest.h"

#include <filesource/RationalRate.h>
#include <filesource/FileClockMapper.h>
#include <filesource/FilePlaybackState.h>
#include <filesource/FileMediaTypeAdapter.h>
#include <VideoState.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace filesource;

// VP Renderer (file sources): CPU tests for the graph-free helpers in
// src/VideoProcessor-Lib/filesource (rational rates, PTS verification, clock
// mapping, playback state and media type adaptation / repack).
namespace FileSourceTests
{
	namespace
	{
		// Exact rational timestamps rounded to 100 ns, as a demuxer writes them.
		int64_t RoundedTimestamp(int64_t frame, int64_t num, int64_t den)
		{
			const int64_t scaled = frame * 10000000LL * den;
			return (scaled + num / 2) / num;
		}

		void AssertRate(uint32_t num, uint32_t den, const RationalRate& r, const wchar_t* what)
		{
			Assert::AreEqual(num, r.numerator, what);
			Assert::AreEqual(den, r.denominator, what);
		}

		GUID FourCcSubtype(uint32_t fourcc)
		{
			GUID g = { fourcc, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
			return g;
		}

		uint32_t Fcc(char a, char b, char c, char d)
		{
			return (uint32_t)(unsigned char)a | ((uint32_t)(unsigned char)b << 8) |
				((uint32_t)(unsigned char)c << 16) | ((uint32_t)(unsigned char)d << 24);
		}

		// Owns the format block an AM_MEDIA_TYPE points at.
		struct MediaTypeFixture
		{
			AM_MEDIA_TYPE mt;
			std::vector<BYTE> format;

			MediaTypeFixture() { std::memset(&mt, 0, sizeof(mt)); }
			MediaTypeFixture(const MediaTypeFixture&) = delete;
			MediaTypeFixture& operator=(const MediaTypeFixture&) = delete;
			// Moving keeps the vector's buffer, so pbFormat stays valid.
			MediaTypeFixture(MediaTypeFixture&& other) : mt(other.mt), format(std::move(other.format))
			{
				mt.pbFormat = format.empty() ? nullptr : format.data();
			}

			void Finish(const GUID& subtype, const GUID& formatType, const void* block, size_t size)
			{
				format.assign((const BYTE*)block, (const BYTE*)block + size);
				mt.majortype = MEDIATYPE_Video;
				mt.subtype = subtype;
				mt.formattype = formatType;
				mt.cbFormat = (ULONG)format.size();
				mt.pbFormat = format.data();
			}
		};

		MediaTypeFixture MakeVih2(const GUID& subtype, LONG biWidth, LONG biHeight, RECT rcSource,
			REFERENCE_TIME avgTimePerFrame, DWORD aspectX = 0, DWORD aspectY = 0,
			DWORD interlaceFlags = 0, DWORD controlFlags = 0)
		{
			VIDEOINFOHEADER2 vih2;
			std::memset(&vih2, 0, sizeof(vih2));
			vih2.rcSource = rcSource;
			vih2.rcTarget = rcSource;
			vih2.AvgTimePerFrame = avgTimePerFrame;
			vih2.dwInterlaceFlags = interlaceFlags;
			vih2.dwPictAspectRatioX = aspectX;
			vih2.dwPictAspectRatioY = aspectY;
			vih2.dwControlFlags = controlFlags;
			vih2.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
			vih2.bmiHeader.biWidth = biWidth;
			vih2.bmiHeader.biHeight = biHeight;
			vih2.bmiHeader.biPlanes = 1;
			MediaTypeFixture f;
			f.Finish(subtype, FORMAT_VideoInfo2, &vih2, sizeof(vih2));
			return f;
		}

		MediaTypeFixture MakeVih(const GUID& subtype, LONG biWidth, LONG biHeight, RECT rcSource,
			REFERENCE_TIME avgTimePerFrame)
		{
			VIDEOINFOHEADER vih;
			std::memset(&vih, 0, sizeof(vih));
			vih.rcSource = rcSource;
			vih.rcTarget = rcSource;
			vih.AvgTimePerFrame = avgTimePerFrame;
			vih.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
			vih.bmiHeader.biWidth = biWidth;
			vih.bmiHeader.biHeight = biHeight;
			vih.bmiHeader.biPlanes = 1;
			MediaTypeFixture f;
			f.Finish(subtype, FORMAT_VideoInfo, &vih, sizeof(vih));
			return f;
		}

		RECT Rect(LONG l, LONG t, LONG r, LONG b)
		{
			RECT rc = { l, t, r, b };
			return rc;
		}

		// Clock helpers: QPC = base + ref * ticksPer100ns.
		ClockCorrelation Pair(int64_t ref, double ticksPer100ns, int64_t base, int64_t frequency)
		{
			ClockCorrelation c;
			c.referenceTime100ns = ref;
			c.qpcTicks = base + (int64_t)std::llround((double)ref * ticksPer100ns);
			c.qpcFrequency = frequency;
			return c;
		}

		struct FakeClock
		{
			REFERENCE_TIME time;
			HRESULT result;
			HRESULT GetTime(REFERENCE_TIME* t) { *t = time; return result; }
		};
	}

	TEST_CLASS(FileSourceRationalRateTests)
	{
	public:
		TEST_METHOD(SnapsThousandOneFamilyIncludingOffByOneRounding)
		{
			AssertRate(24000, 1001, SnapAvgTimePerFrame(417083), L"417083");
			AssertRate(24000, 1001, SnapAvgTimePerFrame(417084), L"417084");
			AssertRate(30000, 1001, SnapAvgTimePerFrame(333667), L"333667");
			AssertRate(30000, 1001, SnapAvgTimePerFrame(333666), L"333666");
			AssertRate(60000, 1001, SnapAvgTimePerFrame(166833), L"166833");
			AssertRate(60000, 1001, SnapAvgTimePerFrame(166834), L"166834");
			AssertRate(48000, 1001, SnapAvgTimePerFrame(208541), L"208541");
			AssertRate(48000, 1001, SnapAvgTimePerFrame(208542), L"208542");
			AssertRate(120000, 1001, SnapAvgTimePerFrame(83417), L"83417");
		}

		TEST_METHOD(SnapsIntegerRatesIncludingOffByOne)
		{
			AssertRate(25, 1, SnapAvgTimePerFrame(400000), L"400000");
			AssertRate(25, 1, SnapAvgTimePerFrame(400001), L"400001");
			AssertRate(25, 1, SnapAvgTimePerFrame(399999), L"399999");
			AssertRate(50, 1, SnapAvgTimePerFrame(200000), L"200000");
			AssertRate(30, 1, SnapAvgTimePerFrame(333333), L"333333");
			AssertRate(30, 1, SnapAvgTimePerFrame(333334), L"333334");
			AssertRate(240, 1, SnapAvgTimePerFrame(41667), L"41667");
			AssertRate(240000, 1001, SnapAvgTimePerFrame(41708), L"41708");
			Assert::AreEqual(25.0, SnapAvgTimePerFrame(400000).Hz(), 1e-12);
		}

		TEST_METHOD(FallsBackToSimplestRationalWithinRounding)
		{
			const RationalRate r = SnapAvgTimePerFrame(645161); // 15.5 fps
			AssertRate(31, 2, r, L"645161");
			Assert::IsTrue(PeriodMatchesRate(645161, r));
			const RationalRate r2 = SnapAvgTimePerFrame(645162);
			AssertRate(31, 2, r2, L"645162");
			// Every snap's exact period is within one 100 ns unit of the input.
			for (int64_t t = 100000; t < 1100000; t += 7919)
			{
				const RationalRate s = SnapAvgTimePerFrame(t);
				Assert::IsTrue(s.IsValid());
				Assert::IsTrue(std::fabs(s.Period100ns() - (double)t) <= 1.0);
			}
		}

		TEST_METHOD(InvalidForUnknownPeriod)
		{
			Assert::IsFalse(SnapAvgTimePerFrame(0).IsValid());
			Assert::IsFalse(SnapAvgTimePerFrame(-417083).IsValid());
			Assert::AreEqual(0.0, SnapAvgTimePerFrame(0).Hz());
		}

		TEST_METHOD(PtsVerifierAgreesWithRoundedTimestamps)
		{
			PtsRateVerifier v;
			Assert::IsTrue(v.Check(SnapAvgTimePerFrame(417083)) == PtsRateVerifier::Verdict::Insufficient);
			for (int64_t n = 0; n < 40; ++n)
				v.AddSampleStart(RoundedTimestamp(n, 24000, 1001));
			Assert::IsTrue(v.HasMeasurement());
			Assert::IsTrue(v.Check(SnapAvgTimePerFrame(417083)) == PtsRateVerifier::Verdict::Agrees);
			Assert::IsTrue(v.Check(SnapAvgTimePerFrame(417084)) == PtsRateVerifier::Verdict::Agrees);
			AssertRate(24000, 1001, v.MeasuredRate(), L"measured");
			AssertRate(24000, 1001, v.EffectiveRate(417084), L"effective");
			// A 25 fps claim disagrees with 23.976 timestamps.
			Assert::IsTrue(v.Check(SnapAvgTimePerFrame(400000)) == PtsRateVerifier::Verdict::Disagrees);
		}

		TEST_METHOD(PtsVerifierYieldsMeasuredRateWhenHeaderIsZeroOrWrong)
		{
			PtsRateVerifier v;
			for (int64_t n = 0; n < 20; ++n)
				v.AddSampleStart(1234567 + RoundedTimestamp(n, 25, 1));
			AssertRate(25, 1, v.EffectiveRate(0), L"avg 0");
			AssertRate(25, 1, v.EffectiveRate(417083), L"wrong avg");
			Assert::AreEqual<int64_t>(400000, v.MedianDelta100ns());
		}

		TEST_METHOD(PtsVerifierToleratesVariableFrameRateOutliers)
		{
			PtsRateVerifier v(15, 5);
			int64_t t = 0;
			for (int n = 0; n < 15; ++n)
			{
				// Every fifth frame is missing (a gap of two periods), one frame is late.
				t += (n % 5 == 4) ? 2 * 333667 : 333667;
				if (n == 7)
					t += 20000;
				v.AddSampleStart(t);
			}
			AssertRate(30000, 1001, v.MeasuredRate(), L"median");
			Assert::IsTrue(v.Check(SnapAvgTimePerFrame(333667)) == PtsRateVerifier::Verdict::Agrees);
		}

		TEST_METHOD(PtsVerifierResetsOnDiscontinuityAndBackwardsTime)
		{
			PtsRateVerifier v;
			for (int64_t n = 0; n < 10; ++n)
				v.AddSampleStart(RoundedTimestamp(n, 50, 1));
			Assert::IsTrue(v.HasMeasurement());
			v.OnDiscontinuity();
			Assert::IsFalse(v.HasMeasurement());
			Assert::AreEqual<size_t>(0, v.DeltaCount());
			AssertRate(24000, 1001, v.EffectiveRate(417083), L"no measurement -> header");

			for (int64_t n = 0; n < 10; ++n)
				v.AddSampleStart(RoundedTimestamp(n, 50, 1));
			v.AddSampleStart(0); // seek backwards without a flush
			Assert::AreEqual<size_t>(0, v.DeltaCount());
		}
	};

	TEST_CLASS(FileSourceClockMapperTests)
	{
	public:
		TEST_METHOD(MapsIdealClockWithNonDecimalFrequency)
		{
			const int64_t freq = 24000000; // 2.4 ticks per 100 ns
			FileClockMapper m;
			Assert::IsFalse(m.Map(ClockMapRequest()).ok);
			for (int k = 0; k < 10; ++k)
				Assert::IsTrue(m.AddCorrelation(Pair(10000000LL * k, 2.4, 1000000, freq)));
			Assert::IsTrue(m.HasFit());
			ClockMapRequest r;
			r.streamStartTime100ns = 50000000;
			r.sampleStart100ns = 10000000;
			const ClockMapResult res = m.Map(r);
			Assert::IsTrue(res.ok);
			Assert::AreEqual<int64_t>(60000000, res.referenceTime100ns);
			Assert::AreEqual<int64_t>(1000000 + 144000000, res.presentationQpc);
			Assert::AreEqual(0.0, m.DriftPpm(), 0.01);
		}

		TEST_METHOD(FollowsClockDrift)
		{
			const int64_t freq = 10000000;
			const double slope = 1.0 * (1.0 + 200e-6); // reference clock 200 ppm slow
			FileClockMapper m(16);
			for (int k = 0; k < 40; ++k)
				m.AddCorrelation(Pair(5000000LL * k, slope, 777, freq));
			Assert::AreEqual<size_t>(16, m.CorrelationCount());
			Assert::AreEqual(200.0, m.DriftPpm(), 1.0);
			int64_t qpc = 0;
			const int64_t future = 5000000LL * 50;
			Assert::IsTrue(m.MapReferenceToQpc(future, qpc));
			const double expected = 777.0 + (double)future * slope;
			Assert::IsTrue(std::fabs((double)qpc - expected) <= 2.0);
		}

		TEST_METHOD(DriftIsBoundedAndJumpsRestartTheFit)
		{
			const int64_t freq = 10000000;
			FileClockMapper m(16, 100.0, 5.0);
			// 300 ppm exceeds the 100 ppm bound: slope is clamped.
			for (int k = 0; k < 10; ++k)
				m.AddCorrelation(Pair(10000000LL * k, 1.0003, 1000, freq));
			Assert::AreEqual(100.0, m.DriftPpm(), 0.5);

			FileClockMapper j;
			for (int k = 0; k < 5; ++k)
				j.AddCorrelation(Pair(10000000LL * k, 1.0, 1000, freq));
			Assert::AreEqual<size_t>(5, j.CorrelationCount());
			// 100 ms off the fit: clock change, restart.
			j.AddCorrelation(Pair(10000000LL * 5, 1.0, 1000 + 1000000, freq));
			Assert::AreEqual<size_t>(1, j.CorrelationCount());
			// Invalid pair ignored.
			Assert::IsFalse(j.AddCorrelation(ClockCorrelation()));
		}

		TEST_METHOD(PauseAndResumeUseTheNewStartTime)
		{
			const int64_t freq = 10000000;
			FileClockMapper m;
			for (int k = 0; k < 4; ++k)
				m.AddCorrelation(Pair(10000000LL * k, 1.0, 5000, freq));
			ClockMapRequest r;
			r.streamStartTime100ns = 10000000;
			r.sampleStart100ns = 20000000;
			const ClockMapResult before = m.Map(r);
			// Paused for 3 s: Run() supplies tStart + 3 s for the same stream position.
			r.streamStartTime100ns += 30000000;
			const ClockMapResult after = m.Map(r);
			Assert::IsTrue(before.ok && after.ok);
			Assert::AreEqual<int64_t>(30000000, after.presentationQpc - before.presentationQpc);
		}

		TEST_METHOD(ScalesStreamTimeByPlaybackRate)
		{
			const int64_t freq = 10000000;
			FileClockMapper m;
			for (int k = 0; k < 4; ++k)
				m.AddCorrelation(Pair(10000000LL * k, 1.0, 0 + 1, freq));
			ClockMapRequest r;
			r.streamStartTime100ns = 0;
			r.sampleStart100ns = 20000000;
			r.playbackRate = 2.0;
			Assert::AreEqual<int64_t>(10000000, m.Map(r).referenceTime100ns);
			r.playbackRate = 0.5;
			Assert::AreEqual<int64_t>(40000000, m.Map(r).referenceTime100ns);
			r.playbackRate = 2.0;
			r.sampleTimesAlreadyRateScaled = true;
			Assert::AreEqual<int64_t>(20000000, m.Map(r).referenceTime100ns);
			r.sampleTimesAlreadyRateScaled = false;
			r.playbackRate = 0.0;
			Assert::IsFalse(m.Map(r).ok);
			r.playbackRate = -1.0;
			Assert::IsFalse(m.Map(r).ok);
			r.playbackRate = std::numeric_limits<double>::quiet_NaN();
			Assert::IsFalse(m.Map(r).ok);
		}

		TEST_METHOD(AppliesSignedAvOffsetAndNegativeSampleTimes)
		{
			const int64_t freq = 24000000;
			FileClockMapper m;
			for (int k = 0; k < 4; ++k)
				m.AddCorrelation(Pair(10000000LL * k, 2.4, 100000000, freq));
			ClockMapRequest r;
			r.streamStartTime100ns = 20000000;
			r.sampleStart100ns = 0;
			const int64_t base = m.Map(r).presentationQpc;
			r.avOffsetMs = -40;
			Assert::AreEqual<int64_t>(base - 960000, m.Map(r).presentationQpc);
			r.avOffsetMs = 25;
			Assert::AreEqual<int64_t>(base + 600000, m.Map(r).presentationQpc);
			r.avOffsetMs = 0;
			r.sampleStart100ns = -1000000; // pre-roll sample before the segment start
			Assert::AreEqual<int64_t>(base - 2400000, m.Map(r).presentationQpc);
		}

		TEST_METHOD(RejectsOverflowAndNonPositiveResults)
		{
			const int64_t freq = 10000000;
			FileClockMapper m;
			for (int k = 1; k < 4; ++k)
				m.AddCorrelation(Pair(10000000LL * k, 1.0, 10, freq));
			ClockMapRequest r;
			r.streamStartTime100ns = (std::numeric_limits<int64_t>::max)() - 10;
			r.sampleStart100ns = (std::numeric_limits<int64_t>::max)() - 10;
			Assert::IsFalse(m.Map(r).ok);
			r.streamStartTime100ns = 0;
			r.sampleStart100ns = -1000000000LL; // maps before QPC zero
			const ClockMapResult res = m.Map(r);
			Assert::IsFalse(res.ok);
			Assert::AreEqual<int64_t>(0, res.presentationQpc);
		}

		TEST_METHOD(AdapterTakesQpcMidpointAroundGetTime)
		{
			FakeClock clock = { 123456789, S_OK };
			int64_t counter = 1000;
			auto readQpc = [&counter](int64_t* t) { *t = counter; counter += 10; return true; };
			auto readFreq = [](int64_t* f) { *f = 10000000; return true; };
			ClockCorrelation c;
			Assert::IsTrue(SampleClockCorrelation(&clock, readQpc, readFreq, c));
			Assert::AreEqual<int64_t>(123456789, c.referenceTime100ns);
			Assert::AreEqual<int64_t>(1005, c.qpcTicks);
			Assert::AreEqual<int64_t>(10000000, c.qpcFrequency);

			FakeClock failing = { 1, E_FAIL };
			Assert::IsFalse(SampleClockCorrelation(&failing, readQpc, readFreq, c));
			FakeClock* none = nullptr;
			Assert::IsFalse(SampleClockCorrelation(none, readQpc, readFreq, c));
			Assert::IsFalse(SampleReferenceClockCorrelation(nullptr, c));
		}
	};

	TEST_CLASS(FileSourcePlaybackStateTests)
	{
	public:
		static FileClockMapper IdentityMapper()
		{
			FileClockMapper m;
			for (int k = 1; k < 5; ++k)
				m.AddCorrelation(Pair(10000000LL * k, 1.0, 0, 10000000));
			return m;
		}

		static FileSampleTimes Sample(int64_t start, int64_t duration = 400000, bool flag = false)
		{
			FileSampleTimes s;
			s.hasStart = true;
			s.hasStop = true;
			s.start100ns = start;
			s.stop100ns = start + duration;
			s.discontinuityFlag = flag;
			return s;
		}

		TEST_METHOD(FirstSampleAfterStartIsDiscontinuousOnlyOnce)
		{
			FilePlaybackState p;
			const FileClockMapper m = IdentityMapper();
			Assert::IsFalse(p.AcceptsSamples());
			p.Run(50000000);
			Assert::IsTrue(p.AcceptsSamples());
			const RationalRate rate = SnapAvgTimePerFrame(417083);
			FileFrameTiming t1 = p.BuildTiming(Sample(0), rate, 0.0, m, 0);
			FileFrameTiming t2 = p.BuildTiming(Sample(417083), rate, 0.0, m, 0);
			Assert::IsTrue(t1.discontinuity);
			Assert::IsFalse(t2.discontinuity);
			Assert::IsFalse(t1.paused);
			Assert::AreEqual<uint32_t>((uint32_t)sizeof(FileFrameTiming), t1.structSize);
			Assert::IsTrue(t1.isFileSource);
			Assert::AreEqual<uint32_t>(24000, t1.sourceRateNumerator);
			Assert::AreEqual<uint32_t>(1001, t1.sourceRateDenominator);
			Assert::AreEqual(24000.0 / 1001.0, t1.sourceRateHz, 1e-12);
			Assert::AreEqual<int64_t>(50000000, t1.presentationQpc);
			Assert::AreEqual<int64_t>(50000000 + 417083, t2.presentationQpc);
			Assert::AreEqual<int64_t>(417083, t2.sampleStart100ns);
			Assert::AreEqual<int64_t>(417083 + 400000, t2.sampleStop100ns);
			Assert::IsTrue(p.MotionAllowed());
		}

		TEST_METHOD(PrerollIsPausedHeldAndUntimed)
		{
			FilePlaybackState p;
			const FileClockMapper m = IdentityMapper();
			p.Pause();
			Assert::IsTrue(p.AcceptsSamples());
			FileFrameTiming t = p.BuildTiming(Sample(0), RationalRate(), 25.0, m, 0);
			Assert::IsTrue(t.paused);
			Assert::IsTrue(t.discontinuity);
			Assert::AreEqual<int64_t>(0, t.presentationQpc);
			Assert::IsFalse(p.MotionAllowed());
			// Unknown rational: the measured rate is reported.
			Assert::AreEqual<uint32_t>(0, t.sourceRateNumerator);
			Assert::AreEqual(25.0, t.sourceRateHz);
		}

		TEST_METHOD(ResumeAfterPauseIsContinuousWithNewStartTime)
		{
			FilePlaybackState p;
			const FileClockMapper m = IdentityMapper();
			const RationalRate rate = SnapAvgTimePerFrame(400000);
			p.Run(10000000);
			p.BuildTiming(Sample(0), rate, 0.0, m, 0);
			p.Pause();
			FileFrameTiming held = p.BuildTiming(Sample(400000), rate, 0.0, m, 0);
			Assert::IsTrue(held.paused);
			Assert::IsFalse(held.discontinuity);
			p.Run(40000000);
			FileFrameTiming t = p.BuildTiming(Sample(800000), rate, 0.0, m, 0);
			Assert::IsFalse(t.discontinuity);
			Assert::IsFalse(t.paused);
			Assert::AreEqual<int64_t>(40000000, p.StreamStartTime());
			Assert::AreEqual<int64_t>(40800000, t.presentationQpc);
		}

		TEST_METHOD(FlushAndSeekMarkTheNextSample)
		{
			FilePlaybackState p;
			const FileClockMapper m = IdentityMapper();
			p.Run(10000000);
			p.BuildTiming(Sample(0), RationalRate(), 0.0, m, 0);
			p.BeginFlush();
			Assert::IsTrue(p.IsFlushing());
			Assert::IsFalse(p.AcceptsSamples());
			p.EndFlush();
			Assert::IsTrue(p.AcceptsSamples());
			Assert::IsFalse(p.NewSegment(0, 1000000000, 1.0));
			Assert::IsTrue(p.BuildTiming(Sample(0), RationalRate(), 0.0, m, 0).discontinuity);
			Assert::IsFalse(p.BuildTiming(Sample(400000), RationalRate(), 0.0, m, 0).discontinuity);
			// A sample flagged discontinuous by the source.
			Assert::IsTrue(p.BuildTiming(Sample(800000, 400000, true), RationalRate(), 0.0, m, 0).discontinuity);
		}

		TEST_METHOD(RateChangeTurnsMotionOffAndScalesTheClock)
		{
			Assert::IsTrue(MotionAllowedAtRate(1.0));
			Assert::IsFalse(MotionAllowedAtRate(2.0));
			Assert::IsFalse(MotionAllowedAtRate(0.5));

			FilePlaybackState p;
			const FileClockMapper m = IdentityMapper();
			p.Run(10000000);
			p.BuildTiming(Sample(0), RationalRate(), 0.0, m, 0);
			Assert::IsTrue(p.NewSegment(0, 1000000000, 2.0));
			Assert::IsFalse(p.MotionAllowed());
			FileFrameTiming t = p.BuildTiming(Sample(20000000), RationalRate(), 0.0, m, 0);
			Assert::IsTrue(t.discontinuity);
			Assert::AreEqual(2.0, t.playbackRate);
			Assert::AreEqual<int64_t>(20000000, t.presentationQpc);

			p.SetRate(0.5);
			FileFrameTiming slow = p.BuildTiming(Sample(4000000), RationalRate(), 0.0, m, 0);
			Assert::IsTrue(slow.discontinuity);
			Assert::AreEqual(0.5, slow.playbackRate);
			Assert::AreEqual<int64_t>(18000000, slow.presentationQpc);
			// Same rate again: no new discontinuity; invalid rates ignored.
			p.SetRate(0.5);
			p.SetRate(-1.0);
			Assert::IsFalse(p.BuildTiming(Sample(4400000), RationalRate(), 0.0, m, 0).discontinuity);
			Assert::AreEqual(0.5, p.PlaybackRate());
		}

		TEST_METHOD(FrameStepHoldsTheSteppedFrame)
		{
			FilePlaybackState p;
			const FileClockMapper m = IdentityMapper();
			p.Run(10000000);
			p.BuildTiming(Sample(0), RationalRate(), 0.0, m, 0);
			p.FrameStep();
			Assert::IsTrue(p.StepPending());
			Assert::IsFalse(p.MotionAllowed());
			FileFrameTiming stepped = p.BuildTiming(Sample(400000), RationalRate(), 0.0, m, 0);
			Assert::IsTrue(stepped.discontinuity);
			Assert::IsTrue(stepped.paused);
			Assert::AreEqual<int64_t>(0, stepped.presentationQpc);
			Assert::IsFalse(p.StepPending());
		}

		TEST_METHOD(StopThenRunRestartsWithDiscontinuity)
		{
			FilePlaybackState p;
			const FileClockMapper m = IdentityMapper();
			p.Run(10000000);
			p.BuildTiming(Sample(0), RationalRate(), 0.0, m, 0);
			p.Stop();
			Assert::IsFalse(p.AcceptsSamples());
			p.Run(90000000);
			FileFrameTiming t = p.BuildTiming(Sample(0), RationalRate(), 0.0, m, -10);
			Assert::IsTrue(t.discontinuity);
			Assert::AreEqual<int64_t>(90000000 - 100000, t.presentationQpc);
			// No correlation yet: presentation time unknown.
			FilePlaybackState q;
			q.Run(0);
			Assert::AreEqual<int64_t>(0, q.BuildTiming(Sample(0), RationalRate(), 0.0, FileClockMapper(), 0).presentationQpc);
		}
	};

	TEST_CLASS(FileSourceMediaTypeTests)
	{
	public:
		TEST_METHOD(LiteralGuidsMatchTheSdk)
		{
			Assert::IsTrue(filesource::detail::SameGuid(filesource::detail::MediaTypeVideo(), MEDIATYPE_Video));
			Assert::IsTrue(filesource::detail::SameGuid(filesource::detail::SubtypeRgb32(), MEDIASUBTYPE_RGB32));
			Assert::IsTrue(filesource::detail::SameGuid(filesource::detail::FormatVideoInfo(), FORMAT_VideoInfo));
			Assert::IsTrue(filesource::detail::SameGuid(filesource::detail::FormatVideoInfo2(), FORMAT_VideoInfo2));
			Assert::IsTrue(filesource::detail::IsFourCcGuid(MEDIASUBTYPE_YUY2, Fcc('Y', 'U', 'Y', '2')));
		}

		TEST_METHOD(Nv12Vih2BasicFormatAndDisplayMode)
		{
			MediaTypeFixture mt = MakeVih2(FourCcSubtype(Fcc('N', 'V', '1', '2')), 1920, 1080,
				Rect(0, 0, 0, 0), 417083, 16, 9);
			const FileVideoFormat f = ParseFileMediaType(mt.mt);
			Assert::IsTrue(f.ok);
			Assert::IsTrue(f.encoding == VideoFrameEncoding::NV12);
			Assert::IsTrue(f.isVideoInfoHeader2);
			Assert::AreEqual<uint32_t>(1920, f.width);
			Assert::AreEqual<uint32_t>(1080, f.height);
			Assert::AreEqual<uint32_t>(1920, f.sourceStrideBytes);
			Assert::AreEqual<uint32_t>(16, f.pictureAspectX);
			Assert::AreEqual<uint32_t>(9, f.pictureAspectY);
			Assert::IsFalse(f.bottomUp);
			AssertRate(24000, 1001, f.rate, L"rate");
			Assert::IsFalse(f.color.present);
			Assert::IsTrue(f.color.colorSpaceFromHeight);
			Assert::IsTrue(f.color.colorSpace == ColorSpace::REC_709);
			Assert::IsTrue(f.color.eotf == EOTF::UNKNOWN);

			const char* why = nullptr;
			DisplayModeSharedPtr mode = BuildFileDisplayMode(f, f.rate, &why);
			Assert::IsTrue(mode != nullptr);
			Assert::IsNull(why);
			Assert::AreEqual(1920u, mode->FrameWidth());
			Assert::AreEqual(1080u, mode->FrameHeight());
			Assert::AreEqual(24000u, mode->TimeScale());
			Assert::AreEqual(1001u, mode->FrameDuration());
			Assert::IsFalse(mode->IsInterlaced());
			Assert::AreEqual<size_t>((size_t)1920 * (1080 + 540), FilePackedFrameBytes(f.encoding, f.width, f.height));
		}

		TEST_METHOD(PaddedStrideRepacksOnlyThePicture)
		{
			// 8-byte stride, 5x3 picture (odd width and height) in a 3-row buffer.
			MediaTypeFixture mt = MakeVih2(FourCcSubtype(Fcc('N', 'V', '1', '2')), 8, 3,
				Rect(0, 0, 5, 3), 400000);
			const FileVideoFormat f = ParseFileMediaType(mt.mt);
			Assert::IsTrue(f.ok);
			Assert::AreEqual<uint32_t>(5, f.width);
			Assert::AreEqual<uint32_t>(3, f.height);
			Assert::AreEqual<uint32_t>(8, f.sourceStrideBytes);
			Assert::AreEqual<size_t>(8 * 3 + 8 * 2, FileRequiredSourceBytes(f));

			std::vector<uint8_t> src(40);
			for (int y = 0; y < 3; ++y)
				for (int x = 0; x < 8; ++x)
					src[y * 8 + x] = (uint8_t)(y * 16 + x);
			for (int y = 0; y < 2; ++y)
				for (int x = 0; x < 8; ++x)
					src[24 + y * 8 + x] = (uint8_t)(200 + y * 16 + x);

			const size_t row = FileRowBytes(f.encoding, f.width); // 6 (even width)
			Assert::AreEqual<size_t>(6, row);
			std::vector<uint8_t> dst(FilePackedFrameBytes(f.encoding, f.width, f.height), 0xEE);
			Assert::AreEqual<size_t>(6 * 5, dst.size());
			Assert::IsTrue(RepackToVpLayout(f, src.data(), src.size(), dst.data(), dst.size()));
			for (int y = 0; y < 3; ++y)
				for (int x = 0; x < 6; ++x)
					Assert::AreEqual<int>(y * 16 + x, dst[y * 6 + x]);
			for (int y = 0; y < 2; ++y)
				for (int x = 0; x < 6; ++x)
					Assert::AreEqual<int>(200 + y * 16 + x, dst[18 + y * 6 + x]);

			// Short buffers are refused.
			Assert::IsFalse(RepackToVpLayout(f, src.data(), src.size() - 1, dst.data(), dst.size()));
			Assert::IsFalse(RepackToVpLayout(f, src.data(), src.size(), dst.data(), dst.size() - 1));
		}

		TEST_METHOD(OddWidthWithoutPaddingReplicatesTheLastSample)
		{
			MediaTypeFixture mt = MakeVih2(FourCcSubtype(Fcc('N', 'V', '1', '2')), 5, 3,
				Rect(0, 0, 0, 0), 400000);
			const FileVideoFormat f = ParseFileMediaType(mt.mt);
			Assert::IsTrue(f.ok);
			std::vector<uint8_t> src(5 * 3 + 5 * 2);
			for (size_t i = 0; i < src.size(); ++i)
				src[i] = (uint8_t)i;
			std::vector<uint8_t> dst(FilePackedFrameBytes(f.encoding, f.width, f.height));
			Assert::IsTrue(RepackToVpLayout(f, src.data(), src.size(), dst.data(), dst.size()));
			for (int y = 0; y < 3; ++y)
			{
				for (int x = 0; x < 5; ++x)
					Assert::AreEqual<int>(y * 5 + x, dst[y * 6 + x]);
				Assert::AreEqual<int>(y * 5 + 4, dst[y * 6 + 5]);
			}
		}

		TEST_METHOD(P010SourceRectangleOffset)
		{
			// 16-bit samples, stride 8 pixels (16 bytes), picture 4x2 at (2, 2) in a 4-row buffer.
			MediaTypeFixture mt = MakeVih2(FourCcSubtype(Fcc('P', '0', '1', '0')), 8, 4,
				Rect(2, 2, 6, 4), 166833);
			const FileVideoFormat f = ParseFileMediaType(mt.mt);
			Assert::IsTrue(f.ok);
			Assert::IsTrue(f.encoding == VideoFrameEncoding::P010);
			AssertRate(60000, 1001, f.rate, L"rate");
			Assert::AreEqual<uint32_t>(16, f.sourceStrideBytes);
			std::vector<uint16_t> src(8 * 4 + 8 * 2);
			for (int y = 0; y < 4; ++y)
				for (int x = 0; x < 8; ++x)
					src[y * 8 + x] = (uint16_t)(1000 + y * 100 + x);
			for (int y = 0; y < 2; ++y)
				for (int x = 0; x < 8; ++x)
					src[32 + y * 8 + x] = (uint16_t)(5000 + y * 100 + x);
			std::vector<uint16_t> dst(4 * 2 + 4 * 1);
			Assert::IsTrue(RepackToVpLayout(f, (const uint8_t*)src.data(), src.size() * 2,
				(uint8_t*)dst.data(), dst.size() * 2));
			for (int y = 0; y < 2; ++y)
				for (int x = 0; x < 4; ++x)
					Assert::AreEqual<int>(1000 + (y + 2) * 100 + (x + 2), dst[y * 4 + x]);
			// Chroma row 1 (= luma rows 2-3), samples 2..5 (pairs 1 and 2).
			for (int x = 0; x < 4; ++x)
				Assert::AreEqual<int>(5000 + 100 + (x + 2), dst[8 + x]);

			// Odd 4:2:0 offsets are rejected.
			MediaTypeFixture odd = MakeVih2(FourCcSubtype(Fcc('P', '0', '1', '0')), 8, 4,
				Rect(1, 0, 5, 4), 166833);
			const FileVideoFormat bad = ParseFileMediaType(odd.mt);
			Assert::IsFalse(bad.ok);
			Assert::IsNotNull(bad.rejection);
		}

		TEST_METHOD(BottomUpRgb32IsFlippedAndTopDownIsNot)
		{
			MediaTypeFixture mt = MakeVih(MEDIASUBTYPE_RGB32, 4, 3, Rect(0, 0, 0, 0), 400000);
			const FileVideoFormat f = ParseFileMediaType(mt.mt);
			Assert::IsTrue(f.ok);
			Assert::IsTrue(f.encoding == VideoFrameEncoding::BGRA_8BIT);
			Assert::IsFalse(f.isVideoInfoHeader2);
			Assert::IsTrue(f.bottomUp);
			Assert::AreEqual<uint32_t>(4, f.pictureAspectX); // square pixels: 4:3
			Assert::AreEqual<uint32_t>(3, f.pictureAspectY);
			Assert::AreEqual<uint32_t>(16, f.sourceStrideBytes);

			std::vector<uint32_t> src(12);
			for (int y = 0; y < 3; ++y)
				for (int x = 0; x < 4; ++x)
					src[y * 4 + x] = (uint32_t)(y * 10 + x);
			std::vector<uint32_t> dst(12);
			Assert::IsTrue(RepackToVpLayout(f, (const uint8_t*)src.data(), 48, (uint8_t*)dst.data(), 48));
			for (int y = 0; y < 3; ++y)
				for (int x = 0; x < 4; ++x)
					Assert::AreEqual<uint32_t>((uint32_t)((2 - y) * 10 + x), dst[y * 4 + x]);

			MediaTypeFixture td = MakeVih(MEDIASUBTYPE_RGB32, 4, -3, Rect(0, 0, 0, 0), 400000);
			const FileVideoFormat g = ParseFileMediaType(td.mt);
			Assert::IsTrue(g.ok);
			Assert::IsFalse(g.bottomUp);
			Assert::AreEqual<uint32_t>(3, g.height);
			Assert::IsTrue(RepackToVpLayout(g, (const uint8_t*)src.data(), 48, (uint8_t*)dst.data(), 48));
			for (int i = 0; i < 12; ++i)
				Assert::AreEqual(src[i], dst[i]);

			// YUV with a positive height stays top-down.
			MediaTypeFixture yuv = MakeVih(FourCcSubtype(Fcc('N', 'V', '1', '2')), 4, 4, Rect(0, 0, 0, 0), 400000);
			Assert::IsFalse(ParseFileMediaType(yuv.mt).bottomUp);
		}

		TEST_METHOD(VihAndVih2InterlaceAndFieldOrder)
		{
			const GUID nv12 = FourCcSubtype(Fcc('N', 'V', '1', '2'));
			MediaTypeFixture vih = MakeVih(nv12, 720, 576, Rect(0, 0, 0, 0), 400000);
			const FileVideoFormat a = ParseFileMediaType(vih.mt);
			Assert::IsTrue(a.ok);
			Assert::IsFalse(a.interlaced);
			Assert::IsFalse(a.color.present);
			Assert::IsTrue(a.color.colorSpace == ColorSpace::REC_601_625);
			Assert::AreEqual<uint32_t>(5, a.pictureAspectX); // 720:576 square pixels
			Assert::AreEqual<uint32_t>(4, a.pictureAspectY);

			MediaTypeFixture tff = MakeVih2(nv12, 720, 576, Rect(0, 0, 0, 0), 400000, 4, 3,
				AMINTERLACE_IsInterlaced | AMINTERLACE_Field1First);
			const FileVideoFormat b = ParseFileMediaType(tff.mt);
			Assert::IsTrue(b.interlaced);
			Assert::IsFalse(b.oneFieldPerSample);
			Assert::IsTrue(b.fieldOrder == DisplayMode::FieldOrder::TopFirst);
			Assert::AreEqual<uint32_t>(4, b.pictureAspectX);
			DisplayModeSharedPtr mode = BuildFileDisplayMode(b, b.rate);
			Assert::IsTrue(mode != nullptr);
			Assert::IsTrue(mode->IsInterlaced());
			Assert::IsTrue(mode->FirstFieldOrder() == DisplayMode::FieldOrder::TopFirst);

			MediaTypeFixture bff = MakeVih2(nv12, 720, 480, Rect(0, 0, 0, 0), 333667, 4, 3,
				AMINTERLACE_IsInterlaced | AMINTERLACE_1FieldPerSample);
			const FileVideoFormat c = ParseFileMediaType(bff.mt);
			Assert::IsTrue(c.fieldOrder == DisplayMode::FieldOrder::BottomFirst);
			Assert::IsTrue(c.oneFieldPerSample);
			Assert::IsTrue(c.color.colorSpace == ColorSpace::REC_601_525);
			DisplayModeSharedPtr fieldMode = BuildFileDisplayMode(c, c.rate);
			Assert::IsTrue(fieldMode != nullptr);
			Assert::IsFalse(fieldMode->IsInterlaced());
		}

		TEST_METHOD(V210AndP016AreAccepted)
		{
			MediaTypeFixture v = MakeVih2(FourCcSubtype(Fcc('v', '2', '1', '0')), 1920, 1080,
				Rect(0, 0, 0, 0), 200000);
			const FileVideoFormat f = ParseFileMediaType(v.mt);
			Assert::IsTrue(f.ok);
			Assert::IsTrue(f.encoding == VideoFrameEncoding::V210);
			Assert::AreEqual<uint32_t>(5120, f.sourceStrideBytes);
			AssertRate(50, 1, f.rate, L"rate");

			MediaTypeFixture p = MakeVih2(FourCcSubtype(Fcc('P', '0', '1', '6')), 3840, 2160,
				Rect(0, 0, 0, 0), 417083);
			const FileVideoFormat g = ParseFileMediaType(p.mt);
			Assert::IsTrue(g.ok);
			Assert::IsTrue(g.encoding == VideoFrameEncoding::P016);
			Assert::AreEqual<uint32_t>(7680, g.sourceStrideBytes);
		}

		TEST_METHOD(PackedRowBytesMatchVideoState)
		{
			const VideoFrameEncoding encodings[] = { VideoFrameEncoding::NV12, VideoFrameEncoding::P010,
				VideoFrameEncoding::P016, VideoFrameEncoding::BGRA_8BIT, VideoFrameEncoding::V210 };
			const unsigned int widths[] = { 1920, 1921, 3840, 719 };
			for (VideoFrameEncoding e : encodings)
			{
				for (unsigned int w : widths)
				{
					VideoState vs;
					vs.displayMode = std::make_shared<DisplayMode>(w, 1081, false, 24000, 1001);
					vs.videoFrameEncoding = e;
					Assert::AreEqual<uint32_t>(vs.BytesPerRow(), FileRowBytes(e, w));
					Assert::AreEqual<size_t>(vs.BytesPerFrame(), FilePackedFrameBytes(e, w, 1081));
				}
			}
		}

		TEST_METHOD(RejectsUnsupportedTypesWithAReason)
		{
			MediaTypeFixture yuy2 = MakeVih2(MEDIASUBTYPE_YUY2, 1920, 1080, Rect(0, 0, 0, 0), 400000);
			FileVideoFormat f = ParseFileMediaType(yuy2.mt);
			Assert::IsFalse(f.ok);
			Assert::IsNotNull(f.rejection);
			Assert::IsTrue(std::strstr(f.rejection, "subtype") != nullptr);

			MediaTypeFixture wrongFormat = MakeVih2(FourCcSubtype(Fcc('N', 'V', '1', '2')), 1920, 1080,
				Rect(0, 0, 0, 0), 400000);
			const GUID nullGuid = {};
			wrongFormat.mt.formattype = nullGuid;
			Assert::IsFalse(ParseFileMediaType(wrongFormat.mt).ok);

			MediaTypeFixture tooSmall = MakeVih2(FourCcSubtype(Fcc('N', 'V', '1', '2')), 1920, 1080,
				Rect(0, 0, 0, 0), 400000);
			tooSmall.mt.cbFormat = (ULONG)sizeof(VIDEOINFOHEADER);
			Assert::IsFalse(ParseFileMediaType(tooSmall.mt).ok);

			MediaTypeFixture audio = MakeVih2(FourCcSubtype(Fcc('N', 'V', '1', '2')), 1920, 1080,
				Rect(0, 0, 0, 0), 400000);
			audio.mt.majortype = MEDIATYPE_Audio;
			Assert::IsFalse(ParseFileMediaType(audio.mt).ok);

			MediaTypeFixture outside = MakeVih2(FourCcSubtype(Fcc('N', 'V', '1', '2')), 1920, 1080,
				Rect(0, 0, 1922, 1080), 400000);
			Assert::IsFalse(ParseFileMediaType(outside.mt).ok);

			MediaTypeFixture noFormat = MakeVih2(FourCcSubtype(Fcc('N', 'V', '1', '2')), 1920, 1080,
				Rect(0, 0, 0, 0), 400000);
			noFormat.mt.pbFormat = nullptr;
			Assert::IsFalse(ParseFileMediaType(noFormat.mt).ok);

			// Unknown rate and tiny pictures do not build a DisplayMode.
			MediaTypeFixture noRate = MakeVih2(FourCcSubtype(Fcc('N', 'V', '1', '2')), 1920, 1080,
				Rect(0, 0, 0, 0), 0);
			const FileVideoFormat nr = ParseFileMediaType(noRate.mt);
			Assert::IsTrue(nr.ok);
			const char* why = nullptr;
			Assert::IsTrue(BuildFileDisplayMode(nr, nr.rate, &why) == nullptr);
			Assert::IsNotNull(why);
			MediaTypeFixture tiny = MakeVih2(FourCcSubtype(Fcc('N', 'V', '1', '2')), 64, 64,
				Rect(0, 0, 0, 0), 400000);
			const FileVideoFormat t = ParseFileMediaType(tiny.mt);
			why = nullptr;
			Assert::IsTrue(BuildFileDisplayMode(t, t.rate, &why) == nullptr);
			Assert::IsNotNull(why);
		}

		TEST_METHOD(ColorimetryPqHlgAndBt2020)
		{
			const GUID p010 = FourCcSubtype(Fcc('P', '0', '1', '0'));
			const DWORD pq = dxva::PackControlFlags(dxva::Range16_235, dxva::MatrixBT2020_10,
				dxva::PrimariesBT2020, dxva::Transfer2084);
			MediaTypeFixture a = MakeVih2(p010, 3840, 2160, Rect(0, 0, 0, 0), 417083, 16, 9, 0, pq);
			const FileVideoFormat f = ParseFileMediaType(a.mt);
			Assert::IsTrue(f.ok);
			Assert::IsTrue(f.color.present);
			Assert::IsTrue(f.color.eotf == EOTF::PQ);
			Assert::IsTrue(f.color.colorSpace == ColorSpace::BT_2020);
			Assert::IsTrue(f.color.nominalRange == FileNominalRange::Limited);
			Assert::IsFalse(f.color.colorSpaceFromHeight);
			Assert::AreEqual<uint32_t>(15, f.color.transferFunctionCode);

			const FileColorimetry hlg = DecodeColorimetry(dxva::PackControlFlags(dxva::Range16_235,
				dxva::MatrixBT2020_10, dxva::PrimariesBT2020, dxva::TransferHLG), 2160);
			Assert::IsTrue(hlg.eotf == EOTF::HLG);
			Assert::IsTrue(hlg.colorSpace == ColorSpace::BT_2020);

			const FileColorimetry sdr = DecodeColorimetry(dxva::PackControlFlags(dxva::Range0_255,
				dxva::MatrixBT709, dxva::PrimariesBT709, dxva::Transfer709), 1080);
			Assert::IsTrue(sdr.eotf == EOTF::SDR);
			Assert::IsTrue(sdr.colorSpace == ColorSpace::REC_709);
			Assert::IsTrue(sdr.nominalRange == FileNominalRange::Full);

			// Matrix only: BT.2020 matrix -> BT.2020; BT.601 split by height.
			Assert::IsTrue(DecodeColorimetry(dxva::PackControlFlags(0, dxva::MatrixBT2020_12, 0, 0), 2160)
				.colorSpace == ColorSpace::BT_2020);
			Assert::IsTrue(DecodeColorimetry(dxva::PackControlFlags(0, dxva::MatrixBT601, 0, 0), 576)
				.colorSpace == ColorSpace::REC_601_625);
			Assert::IsTrue(DecodeColorimetry(dxva::PackControlFlags(0, dxva::MatrixBT601, 0, 0), 480)
				.colorSpace == ColorSpace::REC_601_525);

			// PQ with nothing else signalled: BT.2020 by the shared fallback rule.
			const FileColorimetry pqOnly = DecodeColorimetry(dxva::PackControlFlags(0, 0, 0,
				dxva::Transfer2084), 1080);
			Assert::IsTrue(pqOnly.colorSpace == ColorSpace::BT_2020);
			Assert::IsTrue(pqOnly.colorSpaceFromHeight);

			// Without COLORINFO_PRESENT the bits are ignored.
			const FileColorimetry ignored = DecodeColorimetry(pq & ~(uint32_t)AMCONTROL_COLORINFO_PRESENT, 1080);
			Assert::IsFalse(ignored.present);
			Assert::IsTrue(ignored.eotf == EOTF::UNKNOWN);
			Assert::IsTrue(ignored.colorSpace == ColorSpace::REC_709);

			// Linear / unknown transfer stays UNKNOWN; DCI-P3 primaries map to P3_DCI.
			const FileColorimetry p3 = DecodeColorimetry(dxva::PackControlFlags(0, 0,
				dxva::PrimariesDCI_P3, dxva::Transfer10), 2160);
			Assert::IsTrue(p3.eotf == EOTF::UNKNOWN);
			Assert::IsTrue(p3.colorSpace == ColorSpace::P3_DCI);
		}
	};
}
