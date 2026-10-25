#include "pch.h"
#include "CppUnitTest.h"

// WP-23 producer (src/VP-V/modules/input/source_analysis/VpFrameAnalysis.*): the single shared
// luma decode equals Sample().luma at every pixel for every format; the reductions equal a plain
// reference over Sample() (histogram exact, means within 1e-6 relative) incl. BTB/WTW codes; the
// range model (one studio mapping); the grain estimate on known noise; the async producer (records
// late until published, drop-oldest when behind with a visible reason, repeats). CPU only.

#include <VP-V/modules/input/source_analysis/VpFrameAnalysis.h>
#include <VP-V/modules/input/source_analysis/VpFrameAnalysisHost.h>
#include "SyntheticAnalysisFrames.h"

#include <chrono>
#include <cmath>
#include <thread>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace FrameAnalysisProducerTestsNs
{
	using namespace SyntheticAnalysisFrames;
	using FrameAnalysis::Rect;

	bool PlaneKind(Kind kind)
	{
		return kind == Kind::P010 || kind == Kind::P210;
	}

	// The reference: every pixel of `picture` through Sample(), mapped with the studio map.
	VpFrameAnalysis::Measured Reference(const AnalysisLumaSource& source, const Rect& picture)
	{
		VpFrameAnalysis::Measured out;
		const auto studio = VpFrameAnalysis::StudioLumaMap(source);
		double sum = 0.0;
		uint64_t n = 0, btb = 0, wtw = 0;
		for (int y = picture.top; y < picture.bottom; ++y)
			for (int x = picture.left; x < picture.right; ++x)
			{
				AnalysisLumaSample sample;
				Assert::IsTrue(source.Sample(x, y, sample));
				const uint16_t code = studio[sample.luma];
				++out.luma.histogram[code];
				sum += code;
				++n;
				btb += code < 64 ? 1 : 0;
				wtw += code > 940 ? 1 : 0;
			}
		out.luma.samples = n;
		out.luma.mean = sum / n;
		out.range.btb = static_cast<double>(btb) / n;
		out.range.wtw = static_cast<double>(wtw) / n;
		return out;
	}

	TEST_CLASS(FrameAnalysisProducerTests)
	{
	public:
		TEST_METHOD(SharedDecodeEqualsSamplePerPixelForEveryFormat)
		{
			for (int k = 0; k < static_cast<int>(Kind::Count); ++k)
			{
				const Kind kind = static_cast<Kind>(k);
				Frame frame = Make(kind, 2);
				Assert::IsTrue(frame.source.IsValid(), KindName(kind));
				VpFrameAnalysis::AttachSharedLuma(frame.source);
				if (PlaneKind(kind))
				{
					// Already a readable plane: no copy, no decode.
					Assert::IsFalse(VpFrameAnalysis::NeedsDecode(frame.source), KindName(kind));
					Assert::IsTrue(frame.source.sharedLuma == nullptr, KindName(kind));
					continue;
				}
				Assert::IsTrue(VpFrameAnalysis::NeedsDecode(frame.source), KindName(kind));
				const AnalysisLumaSource copy = frame.source;   // copies share the plane
				Assert::IsTrue(copy.SharedLumaRow(0) == nullptr, L"not published yet");
				Assert::IsTrue(VpFrameAnalysis::DecodeSharedLuma(frame.source), KindName(kind));
				Assert::IsFalse(VpFrameAnalysis::DecodeSharedLuma(copy), L"decoded once");
				for (int y = 0; y < kHeight; ++y)
				{
					const uint16_t* row = copy.SharedLumaRow(y);
					Assert::IsNotNull(row, KindName(kind));
					for (int x = 0; x < kWidth; ++x)
					{
						AnalysisLumaSample sample;
						Assert::IsTrue(copy.Sample(x, y, sample));
						if (row[x] != sample.luma)
							Assert::Fail((std::wstring(KindName(kind)) + L": shared plane != Sample().luma at " +
								std::to_wstring(x) + L"," + std::to_wstring(y)).c_str());
						uint16_t luma = 0;
						Assert::IsTrue(copy.SampleLuma(x, y, luma));
						Assert::AreEqual(sample.luma, luma);
					}
				}
				uint16_t luma = 0;
				Assert::IsFalse(copy.SampleLuma(-1, 0, luma), L"same failures as Sample()");
				Assert::IsFalse(copy.SampleLuma(0, kHeight, luma));
				Assert::IsTrue(copy.SharedLumaRow(kHeight) == nullptr);
			}
		}

		TEST_METHOD(PlaneOfAnotherSizeIsIgnored)
		{
			Frame frame = Make(Kind::R210, 0);
			VpFrameAnalysis::AttachSharedLuma(frame.source);
			Assert::IsTrue(VpFrameAnalysis::DecodeSharedLuma(frame.source));
			AnalysisLumaSource cropped = frame.source;
			cropped.height = kHeight / 2;   // a view of another size must not read the plane
			Assert::IsTrue(cropped.SharedLumaRow(0) == nullptr);
		}

		TEST_METHOD(RangeModelIsOneStudioMapping)
		{
			// Our sampler already emits studio codes: the VP map is the identity (no second
			// conversion; the forecast's double-conversion clash cannot happen).
			const Frame frame = Make(Kind::Bgra, 0);
			Assert::IsTrue(kAnalysisLumaSampleLimitedDomain);
			Assert::IsFalse(VpFrameAnalysis::SamplerCodesAreFullRange(frame.source));
			const auto map = VpFrameAnalysis::StudioLumaMap(frame.source);
			for (int code = 0; code < 1024; ++code)
				Assert::AreEqual(static_cast<uint16_t>(code), map[code]);
		}

		TEST_METHOD(ReductionsMatchTheReferenceIncludingBtbWtw)
		{
			const Rect picture{ 0, kBar, kWidth, kHeight - kBar };
			for (int k = 0; k < static_cast<int>(Kind::Count); ++k)
			{
				const Kind kind = static_cast<Kind>(k);
				for (int shared = 0; shared < 2; ++shared)
				{
					Frame frame = Make(kind, 3);
					if (shared)
					{
						VpFrameAnalysis::AttachSharedLuma(frame.source);
						VpFrameAnalysis::DecodeSharedLuma(frame.source);
					}
					VpFrameAnalysis::Measured measured;
					Assert::IsTrue(VpFrameAnalysis::Reduce(frame.source, picture, measured), KindName(kind));
					const VpFrameAnalysis::Measured reference = Reference(frame.source, picture);
					Assert::AreEqual(reference.luma.samples, measured.luma.samples);
					Assert::IsTrue(reference.luma.histogram == measured.luma.histogram, KindName(kind));
					Assert::IsTrue(std::abs(reference.luma.mean - measured.luma.mean) <= 1e-6 * reference.luma.mean,
						KindName(kind));
					Assert::AreEqual(reference.range.btb, measured.range.btb, 1e-12);
					Assert::AreEqual(reference.range.wtw, measured.range.wtw, 1e-12);
					Assert::IsTrue(measured.luma.minimum <= measured.luma.p1 && measured.luma.p1 <= measured.luma.p50 &&
						measured.luma.p50 <= measured.luma.p99 && measured.luma.p99 <= measured.luma.maximum);
				}
			}
			// The limited sources keep their BTB (code 4) and WTW (code 1019) probes: never clipped.
			for (Kind kind : { Kind::P010, Kind::P210, Kind::V210, Kind::R210 })
			{
				const Frame frame = Make(kind, 3);
				VpFrameAnalysis::Measured measured;
				Assert::IsTrue(VpFrameAnalysis::Reduce(frame.source, picture, measured));
				Assert::IsTrue(measured.range.btb > 0.0, KindName(kind));
				Assert::IsTrue(measured.range.wtw > 0.0, KindName(kind));
				Assert::IsTrue(measured.luma.minimum < 64 && measured.luma.maximum > 940, KindName(kind));
				// The YCbCr probes reach the container top; r210's 64..960 RGB scaling maps RGB 1019
				// below 1019 luma (still WTW).
				if (kind != Kind::R210)
					Assert::IsTrue(measured.range.clipped > 0.0, KindName(kind));
			}
		}

		TEST_METHOD(GrainEstimateOnKnownNoise)
		{
			// A flat P010 picture at code 500 with uniform noise in [-12, 12]: sigma = sqrt((25^2-1)/12) = 7.21.
			Frame frame = Make(Kind::P010, 0);
			std::vector<uint8_t>& data = *frame.bytes;
			for (int y = 0; y < kHeight; ++y)
				for (int x = 0; x < kWidth; ++x)
					Put16(&data[static_cast<size_t>(y) * frame.source.rowBytes + static_cast<size_t>(x) * 2],
						static_cast<uint16_t>((500 + Noise(x, y, 7, 12)) << 6));
			VpFrameAnalysis::Measured measured;
			Assert::IsTrue(VpFrameAnalysis::Reduce(frame.source, Rect{ 0, 0, kWidth, kHeight }, measured));
			// The flattest decile of 8x8 sample variances sits a little below sigma.
			Assert::IsTrue(measured.grain.rms > 5.5 && measured.grain.rms < 7.5, std::to_wstring(measured.grain.rms).c_str());
			Assert::IsTrue(measured.grain.confidence > 0.9);
			Assert::AreEqual(uint32_t((kWidth / 8) * (kHeight / 8)), measured.grain.blocks);
			// Noise-free: rms 0. Near-black blocks are left out (crushed noise is not grain).
			for (int y = 0; y < kHeight; ++y)
				for (int x = 0; x < kWidth; ++x)
					Put16(&data[static_cast<size_t>(y) * frame.source.rowBytes + static_cast<size_t>(x) * 2],
						static_cast<uint16_t>((x < kWidth / 2 ? 70 : 500) << 6));
			Assert::IsTrue(VpFrameAnalysis::Reduce(frame.source, Rect{ 0, 0, kWidth, kHeight }, measured));
			Assert::AreEqual(0.0, measured.grain.rms, 1e-9);
			Assert::AreEqual(uint32_t((kWidth / 16) * (kHeight / 8)), measured.grain.flatBlocks);
		}

		TEST_METHOD(ProducerPublishesAsynchronously)
		{
			FrameAnalysis::Store store(FrameAnalysis::RingDepth(4, 4));
			VpFrameAnalysis::Producer producer(store);
			producer.Start();
			std::vector<Frame> frames;
			for (uint64_t seq = 1; seq <= 6; ++seq)
			{
				frames.push_back(Make(Kind::V210, static_cast<int>(seq)));
				VpFrameAnalysis::AttachSharedLuma(frames.back().source);
				VpFrameAnalysis::Job job;
				job.identity.seq = seq;
				job.identity.picture = { 0, kBar, kWidth, kHeight - kBar };
				job.source = frames.back().source;
				producer.Submit(job);
			}
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
			while (producer.Processed() < 6 && std::chrono::steady_clock::now() < deadline)
				std::this_thread::sleep_for(std::chrono::milliseconds(2));
			producer.Stop();
			Assert::AreEqual(uint64_t(6), producer.Processed());
			for (uint64_t seq = 1; seq <= 6; ++seq)
			{
				FrameAnalysis::Record record;
				Assert::IsTrue(store.Read(seq, FrameAnalysis::Use::Temporal, FrameAnalysis::kMeasuredGroups, record) ==
					FrameAnalysis::ReadStatus::Ok);
				Assert::AreEqual(uint64_t(kWidth) * (kHeight - 2 * kBar), record.luma.samples);
				Assert::IsNotNull(frames[seq - 1].source.SharedLumaRow(0), L"the plane was published");
			}
			std::string why;
			Assert::IsFalse(producer.Behind(why));
		}

		TEST_METHOD(ProducerDropsOldestWhenBehindAndSaysSo)
		{
			FrameAnalysis::Store store(4);
			VpFrameAnalysis::Producer producer(store);   // not started: the worker is "behind"
			std::vector<Frame> frames;
			for (uint64_t seq = 1; seq <= 6; ++seq)
			{
				frames.push_back(Make(Kind::P010, static_cast<int>(seq)));
				VpFrameAnalysis::Job job;
				job.identity.seq = seq;
				job.source = frames.back().source;
				producer.Submit(job);   // returns at once
			}
			Assert::AreEqual(uint64_t(2), producer.Dropped());
			std::string why;
			Assert::IsTrue(producer.Behind(why));
			Assert::IsTrue(why.find("producer behind: 2 frame(s)") == 0, std::wstring(why.begin(), why.end()).c_str());
			FrameAnalysis::Record record;
			Assert::IsTrue(store.Read(4, FrameAnalysis::Use::SingleFrame, FrameAnalysis::kLuma, record) ==
				FrameAnalysis::ReadStatus::Late);
			producer.Drain();
			Assert::IsTrue(store.Read(6, FrameAnalysis::Use::SingleFrame, FrameAnalysis::kLuma, record) ==
				FrameAnalysis::ReadStatus::Ok);
			// Seqs 1-2 were evicted from the 4-slot ring by 5-6, 3-6 measured.
			Assert::IsTrue(store.Read(3, FrameAnalysis::Use::SingleFrame, FrameAnalysis::kLuma, record) ==
				FrameAnalysis::ReadStatus::Ok);
			// The visible entry the host shows for it.
			Assert::AreEqual(std::string("Fallbacks engaged: frame analysis (LED look-ahead: record late)"),
				FallbackStatus::Format({ FrameAnalysis::FallbackEntry("LED look-ahead", FrameAnalysis::ReadStatus::Late) }));
		}

		TEST_METHOD(HostAdapterSwitchRepeatsAndSharedEpoch)
		{
			VpFrameAnalysis::Host host;
			Frame frame = Make(Kind::V210, 1);
			// Off (the default, and the Original pathway): no plane attached, nothing recorded.
			VpFrameAnalysis::Host::AttachIfEnabled(frame.source);
			Assert::IsTrue(frame.source.sharedLuma == nullptr);
			host.OnArrival(1, frame.source, FrameAnalysis::Rect{}, 0, false);
			FrameAnalysis::Record record;
			Assert::IsTrue(host.Store().Read(1, FrameAnalysis::Use::SingleFrame, FrameAnalysis::kIdentity, record) ==
				FrameAnalysis::ReadStatus::Disabled);
			host.Configure(true);
			host.SetRingDepth(4, 4);
			Assert::AreEqual(size_t(10), host.Store().Depth());
			VpFrameAnalysis::Host::AttachIfEnabled(frame.source);
			Assert::IsTrue(frame.source.sharedLuma != nullptr);
			host.Worker().Stop();   // deterministic: process on this thread
			host.OnArrival(1, frame.source, FrameAnalysis::Rect{ 0, kBar, kWidth, kHeight - kBar }, 123, false);
			host.Worker().Drain();
			Assert::IsTrue(host.Store().Read(1, FrameAnalysis::Use::Temporal, FrameAnalysis::kLuma, record) ==
				FrameAnalysis::ReadStatus::Ok);
			Assert::AreEqual(int(kBar), record.id.picture.top);
			Assert::AreEqual(int64_t(123), record.id.captureQpc);
			Assert::IsNotNull(frame.source.SharedLumaRow(0));
			// A captured repeat: the record points at the original, no job queued.
			host.OnArrival(2, frame.source, FrameAnalysis::Rect{ 0, kBar, kWidth, kHeight - kBar }, 124, true);
			Assert::IsTrue(host.Store().Read(2, FrameAnalysis::Use::Temporal, FrameAnalysis::kLuma | FrameAnalysis::kCadence,
				record) == FrameAnalysis::ReadStatus::Ok);
			Assert::AreEqual(uint64_t(1), record.cadence.repeatOf);
			Assert::AreEqual(uint64_t(1), host.Worker().Processed());
			// The shared epoch: a motion Gap (MotionValidity::Cause 2) is a dropped frame here.
			host.OnDiscontinuity(2);
			Assert::IsTrue(host.Store().Read(1, FrameAnalysis::Use::Temporal, FrameAnalysis::kLuma, record) ==
				FrameAnalysis::ReadStatus::StaleEpoch);
			FrameAnalysis::Discontinuity cause;
			Assert::IsTrue(host.Store().LastCause(cause) && cause == FrameAnalysis::Discontinuity::DroppedFrame);
			Assert::IsTrue(VpFrameAnalysis::Host::FromMotionCause(7) == FrameAnalysis::Discontinuity::FormatChange);
			Assert::IsTrue(VpFrameAnalysis::Host::FromMotionCause(8) == FrameAnalysis::Discontinuity::SeekOrReset);
			Assert::IsTrue(host.Summary().find("Frame analysis: ring 10") == 0);
			host.Configure(false);
			Frame other = Make(Kind::V210, 2);
			VpFrameAnalysis::Host::AttachIfEnabled(other.source);
			Assert::IsTrue(other.source.sharedLuma == nullptr);
		}

		TEST_METHOD(CapturedRepeatReusesTheOriginalRecord)
		{
			FrameAnalysis::Store store(FrameAnalysis::RingDepth(4, 4));
			const Frame frame = Make(Kind::R210, 1);
			VpFrameAnalysis::Job job;
			job.identity.seq = 10;
			job.source = frame.source;
			VpFrameAnalysis::Producer producer(store);
			producer.Submit(job);
			producer.Drain();
			FrameAnalysis::Identity repeat;
			repeat.seq = 11;
			store.Begin(repeat, FrameAnalysis::kMeasuredGroups);
			Assert::IsTrue(store.MarkRepeat(11, 10));
			FrameAnalysis::Record a, b;
			store.Read(10, FrameAnalysis::Use::Temporal, FrameAnalysis::kLuma, a);
			Assert::IsTrue(store.Read(11, FrameAnalysis::Use::Temporal, FrameAnalysis::kLuma, b) ==
				FrameAnalysis::ReadStatus::Ok);
			Assert::IsTrue(a.luma.histogram == b.luma.histogram);
			Assert::AreEqual(uint64_t(10), b.cadence.repeatOf);
		}
	};
}
