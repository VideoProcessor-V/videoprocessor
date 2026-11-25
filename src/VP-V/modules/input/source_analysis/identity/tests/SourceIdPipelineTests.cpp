/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// P14 source identity: the record codec and held-state holder, the shared quality view (consensus off = the snapshot bit for bit;
// absent / stale / unlatched = classical, visibly), the frame-level quality signal, the worker-side pipeline (ABR event, latch,
// one-time notice, deferred rebase applied once, determinism) and the monitor's auto-engage gate. CPU only, no files, no clock.
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/core/SkeletonAcquire.h>
#include <VP-V/modules/input/source_analysis/SourceResolutionMonitor.h>
#include <VP-V/modules/input/source_analysis/identity/FrameQuality.h>
#include <VP-V/modules/input/source_analysis/identity/SourceIdPipeline.h>
#include <VP-V/modules/input/source_analysis/identity/SourceIdProduct.h>
#include <VP-V/modules/input/source_analysis/identity/SourceQualityView.h>
#include <VP-V/modules/repair/grain_split/BandMemoFill.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
	namespace SidSynth
	{
		using namespace SourceIdentity;

		struct Lcg
		{
			uint32_t x;
			explicit Lcg(uint32_t seed) : x(seed) {}
			double Next() { x = x * 1664525u + 1013904223u; return static_cast<double>(x >> 8) / 16777216.0; }
			double Noise(double a) { return (Next() * 2.0 - 1.0) * a; }
		};

		// A T x T tile: a gentle gradient plus uniform noise of the given amplitude (8-bit code units), normalised 0..1.
		inline std::vector<float> NoisyTile(int T, double amplitude8, uint32_t seed)
		{
			Lcg r(seed);
			std::vector<float> t(static_cast<size_t>(T) * T);
			for (int y = 0; y < T; ++y)
				for (int x = 0; x < T; ++x)
				{
					const double v = 100.0 + 0.2 * x + 0.1 * y + r.Noise(amplitude8);
					t[static_cast<size_t>(y) * T + x] = static_cast<float>(v / 255.0);
				}
			return t;
		}

		constexpr int64_t kFrameUs = 500000;   // the monitor analyses about 2 frames per second

		struct Clip
		{
			// Frames 0..n-1: native 1080p-like (ratio 1, noise `noiseHigh`) until `switchAt`, then ratio `lowRatio`, noise `noiseLow`.
			int frames = 80;
			int switchAt = 40;
			double lowRatio = 0.667;
			double noiseHigh = 3.0, noiseLow = 0.9;
			bool classicalFollows = true;     // the classical tracker declares the lower ratio a few frames after the switch
			int classicalDelay = 4;
		};

		inline FrameInput Frame(const Clip& c, int i, uint64_t streamKey = 1)
		{
			FrameInput in;
			in.seq = static_cast<uint64_t>(i + 1);
			in.timeUs = static_cast<int64_t>(i + 1) * kFrameUs;
			in.streamKey = streamKey;
			const bool low = i >= c.switchAt;
			in.deliveredRatio = low ? c.lowRatio : 1.0;
			const int T = 128;
			const std::vector<float> tile = NoisyTile(T, low ? c.noiseLow : c.noiseHigh, 1000u + static_cast<uint32_t>(i));
			in.noise.push_back(FrameQuality::MeasureNoise(tile.data(), T));
			in.apl = 0.4;
			in.nativeHeight = 1080;
			in.classical.reading = true;
			in.classical.bitrateQuality = 0.95;
			in.classical.kernel = -1;
			const bool declared = c.classicalFollows && i >= c.switchAt + c.classicalDelay;
			in.classical.declaredRatio = declared ? c.lowRatio : 1.0;
			in.classical.confidence = declared ? 0.9 : 0.0;
			in.classical.axisRatioX = in.classical.axisRatioY = 1.0;
			in.classical.axisConfidence = 0.0;
			return in;
		}

		inline std::string Trace(const Clip& c)
		{
			Pipeline p;
			p.SetConsensusOn(true);
			std::string t;
			for (int i = 0; i < c.frames; ++i)
			{
				const Record r = p.Push(Frame(c, i));
				t += std::to_string(r.tier) + "," + std::to_string(r.ratioV) + "," + std::to_string(r.consensusGen) + "," +
					std::to_string(r.abrDirection) + "," + std::to_string(r.level) + "," + std::to_string(p.LastFrameQuality()) + ";";
			}
			return t;
		}
	}

	TEST_CLASS(SourceIdProductTests)
	{
	public:
		TEST_METHOD(RecordCodecRoundTripsAndFitsTheDeclaredBytes)
		{
			using namespace SourceIdentity;
			Record r;
			r.flags = kFlagDropEventSeen | kFlagConsensusOn;
			r.seq = 123456789012ull; r.timeUs = -42; r.consensusGen = 7; r.streamKey = 99; r.maxAgeUs = 12000000;
			r.ratioH = 0.667f; r.ratioV = 0.5f; r.kernel = 3; r.codec = static_cast<uint8_t>(CodecClass::Hevc); r.tier = 2;
			r.quality = 0.75f; r.level = 0.4f; r.latched = 0x13;
			for (int i = 0; i < kFieldCount; ++i) { r.reason[i] = static_cast<uint8_t>(i); r.confidence[i] = 0.1f * i; }
			r.abrDirection = 1; r.abrTimeUs = 5000000; r.abrMagnitude = 0.3f; r.abrRatioStep = -0.33f; r.abrConfidence = 0.8f;
			r.noticeSerial = 2; r.rebaseGen[0] = 4; r.rebaseGen[1] = 5; r.rebaseCause[0] = 1; r.rebaseCause[1] = 3; r.discontinuities = 6;
			const Packed p = Pack(r);
			Assert::AreEqual(kRecordBytes, sizeof(p.bytes));
			Record u;
			Assert::IsTrue(Unpack(p, u));
			const Packed q = Pack(u);
			Assert::AreEqual(0, std::memcmp(p.bytes, q.bytes, sizeof(p.bytes)), L"round trip is byte exact");
			Assert::AreEqual(r.seq, u.seq);
			Assert::AreEqual(-42ll, static_cast<long long>(u.timeUs));
			Assert::AreEqual(r.ratioH, u.ratioH);
			Assert::AreEqual(2, static_cast<int>(u.tier));
			Assert::IsTrue(u.Latched(Field::AxisH) && u.Latched(Field::AxisV) && !u.Latched(Field::Kernel) && u.Latched(Field::Quality));
		}

		TEST_METHOD(AnotherUnitVersionIsRefused)
		{
			using namespace SourceIdentity;
			Record r;
			Packed p = Pack(r);
			p.bytes[0] = static_cast<uint8_t>(kUnitVersion + 1);   // little endian low byte of unitVersion
			Record u;
			Assert::IsFalse(Unpack(p, u), L"a record of another unit version is not read (visible fallback)");
		}

		TEST_METHOD(HeldStateReturnsTheNewestRecordNotNewerThanTheAskedFrame)
		{
			using namespace SourceIdentity;
			HeldState h;
			Record out;
			Assert::IsFalse(h.ReadLatest(0, out), L"nothing published yet");
			for (uint64_t s = 10; s <= 50; s += 10)
			{
				Record r;
				r.seq = s;
				r.tier = static_cast<int8_t>(s / 10);
				h.Publish(r);
			}
			Assert::IsTrue(h.ReadLatest(0, out));
			Assert::AreEqual(50ull, out.seq);
			Assert::IsTrue(h.ReadLatest(35, out));
			Assert::AreEqual(30ull, out.seq, L"the record that was current for an earlier frame");
			Assert::IsFalse(h.ReadLatest(5, out), L"nothing that old");
			Assert::AreEqual(5ull, h.Serial());
			h.Reset();
			Assert::IsFalse(h.ReadLatest(0, out));
		}

		TEST_METHOD(RebaseRequestsAreAppliedExactlyOncePerGeneration)
		{
			using namespace SourceIdentity;
			struct Port : IndicatorPort
			{
				std::vector<std::pair<int, std::string>> calls;
				void AdvanceRebase(int axis, uint64_t, const char* cause) override { calls.push_back({ axis, cause }); }
			} port;
			RebaseApplier a;
			Record r;
			r.streamKey = 1;
			Assert::AreEqual(0, a.Poll(r, port));
			r.rebaseGen[1] = 1; r.rebaseCause[1] = 1;
			Assert::AreEqual(1, a.Poll(r, port));
			Assert::AreEqual(0, a.Poll(r, port), L"the same record again");
			Record newer = r;
			newer.seq = 99;
			Assert::AreEqual(0, a.Poll(newer, port), L"a newer record carrying the same generation");
			newer.rebaseGen[0] = 1; newer.rebaseGen[1] = 2; newer.rebaseCause[0] = 2; newer.rebaseCause[1] = 3;
			Assert::AreEqual(2, a.Poll(newer, port));
			Assert::AreEqual(3, static_cast<int>(port.calls.size()));
			Assert::AreEqual(std::string("kernel"), port.calls[1].second);
			// A new title: generations start again, old requests are not replayed, new ones are applied.
			Record title2;
			title2.streamKey = 2;
			Assert::AreEqual(0, a.Poll(title2, port));
			title2.rebaseGen[0] = 1;
			Assert::AreEqual(1, a.Poll(title2, port));
		}

		TEST_METHOD(ConcurrentPublishAndReadNeverTearsARecord)
		{
			using namespace SourceIdentity;
			HeldState h;
			std::atomic<bool> stop{ false };
			std::atomic<int> torn{ 0 };
			std::thread reader([&] {
				while (!stop.load())
				{
					Record r;
					if (h.ReadLatest(0, r) && (static_cast<uint64_t>(r.tier) != (r.seq & 0x7Fu) || r.timeUs != static_cast<int64_t>(r.seq) * 3))
						torn++;
				}
			});
			for (uint64_t s = 1; s <= 20000; ++s)
			{
				Record r;
				r.seq = s;
				r.tier = static_cast<int8_t>(s & 0x7Fu);
				r.timeUs = static_cast<int64_t>(s) * 3;
				h.Publish(r);
			}
			stop = true;
			reader.join();
			Assert::AreEqual(0, torn.load());
			Assert::AreEqual(20000ull, h.Serial());
		}
	};

	TEST_CLASS(SourceQualityViewTests)
	{
	public:
		TEST_METHOD(ConsensusOffIsExactlyTheSnapshotAndTodaysTiers)
		{
			using namespace SourceIdentity;
			for (int i = 0; i <= 100; ++i)
			{
				ClassicalQuality c;
				c.reading = true;
				c.bitrateQuality = i / 100.0;
				int expect = 0;
				for (double e : VoteSettings().qualityTierEdges) expect += c.bitrateQuality >= e ? 1 : 0;
				Record junk;                       // a published record must make no difference while consensus is off
				junk.tier = 0; junk.level = 0.0f; junk.latched = 0xFF;
				const QualityView a = View(nullptr, c, false, 0);
				const QualityView b = View(&junk, c, false, 1000);
				Assert::AreEqual(c.bitrateQuality, a.quality);
				Assert::AreEqual(c.bitrateQuality, b.quality);
				Assert::AreEqual(expect, a.tier);
				Assert::AreEqual(expect, b.tier);
				Assert::IsFalse(a.fallback.engaged, L"off is not a fallback");
				Assert::IsTrue(a.origin == QualityOrigin::Classical);
			}
		}

		TEST_METHOD(AbsentStaleAndUnlatchedRecordsFallBackToClassicalVisibly)
		{
			using namespace SourceIdentity;
			ClassicalQuality c;
			c.reading = true;
			c.bitrateQuality = 0.8;
			// Absent.
			QualityView v = View(nullptr, c, true, 5000000);
			Assert::AreEqual(0.8, v.quality);
			Assert::IsTrue(v.fallback.engaged);
			Assert::AreEqual(std::string("no record"), v.reason);
			Assert::AreEqual(std::string("product.source_quality"), v.fallback.id);
			// Fresh and latched: consensus.
			Record r;
			r.timeUs = 5000000; r.maxAgeUs = 12000000; r.tier = 1; r.level = 0.4f; r.latched = 1u << static_cast<unsigned>(Field::Quality);
			v = View(&r, c, true, 6000000);
			Assert::IsTrue(v.origin == QualityOrigin::Consensus);
			Assert::IsFalse(v.fallback.engaged);
			Assert::AreEqual(0.4f, static_cast<float>(v.quality), L"min(classical, event-anchored level)");
			Assert::AreEqual(1, v.tier);
			// Stale: recovers by itself when a fresh record returns.
			v = View(&r, c, true, 5000000 + 12000001);
			Assert::IsTrue(v.fallback.engaged);
			Assert::AreEqual(std::string("stale"), v.reason);
			Assert::AreEqual(0.8, v.quality);
			Record fresh = r;
			fresh.timeUs = 5000000 + 12000000;
			v = View(&fresh, c, true, 5000000 + 12000001);
			Assert::IsTrue(v.origin == QualityOrigin::Consensus, L"recovers");
			// Not latched yet.
			Record unlatched = r;
			unlatched.latched = 0;
			v = View(&unlatched, c, true, 6000000);
			Assert::IsTrue(v.fallback.engaged);
			Assert::AreEqual(std::string("quality not latched yet"), v.reason);
			// A quiet stream (level 1) reads exactly as before.
			Record quiet = r;
			quiet.level = 1.0f;
			v = View(&quiet, c, true, 6000000);
			Assert::AreEqual(0.8, v.quality);
		}

		// SID-B0: a record flagged SHADOW is for logs only; even a caller that passes consensusOn = true gets the classical value, visibly.
		TEST_METHOD(AShadowRecordIsNeverUsedAsAnInput)
		{
			using namespace SourceIdentity;
			ClassicalQuality c;
			c.reading = true;
			c.bitrateQuality = 0.8;
			Record r;
			r.timeUs = 5000000; r.maxAgeUs = 12000000; r.tier = 1; r.level = 0.4f; r.latched = 1u << static_cast<unsigned>(Field::Quality);
			QualityView v = View(&r, c, true, 6000000);
			Assert::IsTrue(v.origin == QualityOrigin::Consensus, L"the same record without the flag is used");
			r.flags = static_cast<uint16_t>(r.flags | kFlagShadow);
			v = View(&r, c, true, 6000000);
			Assert::IsTrue(v.origin == QualityOrigin::Classical);
			Assert::IsTrue(v.fallback.engaged);
			Assert::AreEqual(std::string("shadow record"), v.reason);
			Assert::AreEqual(0.8, v.quality);
		}

		TEST_METHOD(EveryConsumerHasAnAbsentProductPath)
		{
			using namespace SourceIdentity;
			ClassicalQuality c;
			c.reading = true;
			c.bitrateQuality = 0.95;   // a clean source
			const QualityView absent = View(nullptr, c, true, 0);
			// Each consumer's helper runs on the classical value and states its fallback wording.
			Assert::AreEqual(DitherNeed(View(nullptr, c, false, 0)), DitherNeed(absent), L"dither need: same input, same answer");
			Assert::IsFalse(ContrastRecoveryActive(absent), L"contrast recovery stays off on a clean source");
			ClassicalQuality poor = c;
			poor.bitrateQuality = 0.2;
			Assert::IsTrue(ContrastRecoveryActive(View(nullptr, poor, true, 0)));
			const QualityConsumer all[] = { QualityConsumer::TemporalDetailAuto, QualityConsumer::DitherNeed, QualityConsumer::Deband,
				QualityConsumer::ContrastRecovery, QualityConsumer::AdaptiveScaling, QualityConsumer::DetailPulse,
				QualityConsumer::TextureRestore, QualityConsumer::Status };
			for (QualityConsumer q : all)
			{
				const std::string s = ConsumerStatus(absent, q);
				Assert::IsTrue(s.find("classical") != std::string::npos || s.find("unknown") != std::string::npos, L"visible wording");
				Assert::IsTrue(ConsumerStatus(View(nullptr, c, false, 0), q).empty(), L"nothing to report while off");
			}
			Assert::AreEqual(std::string("Source identity: classical only (no record)"), StatusLine(absent, nullptr));
		}
	};

	TEST_CLASS(FrameQualityTests)
	{
	public:
		TEST_METHOD(NoiseMeasureFollowsTheNoiseAndIsDeterministic)
		{
			using namespace SourceIdentity;
			const std::vector<float> lo = SidSynth::NoisyTile(128, 1.0, 5), hi = SidSynth::NoisyTile(128, 4.0, 5);
			const FrameQuality::TileNoise a = FrameQuality::MeasureNoise(lo.data(), 128);
			const FrameQuality::TileNoise b = FrameQuality::MeasureNoise(hi.data(), 128);
			const FrameQuality::TileNoise b2 = FrameQuality::MeasureNoise(hi.data(), 128);
			Assert::IsTrue(a.valid && b.valid);
			Assert::AreEqual(b.sigma, b2.sigma, L"deterministic");
			const double ratio = b.sigma / a.sigma;
			Assert::IsTrue(ratio > 3.0 && ratio < 5.0, L"4x the amplitude reads about 4x the sigma");
			Assert::IsFalse(FrameQuality::MeasureNoise(nullptr, 128).valid);
			Assert::IsFalse(FrameQuality::MeasureNoise(lo.data(), 16).valid, L"a tile too small to judge abstains");
		}

		TEST_METHOD(QualityIsRelativeToTheStreamsOwnNoiseLevel)
		{
			using namespace SourceIdentity;
			FrameQuality::Signal s;
			double qHigh = 0.0;
			for (int i = 0; i < 20; ++i)
			{
				const std::vector<float> t = SidSynth::NoisyTile(128, 3.0, 10u + i);
				qHigh = s.Push(1.0, { FrameQuality::MeasureNoise(t.data(), 128) });
			}
			Assert::IsTrue(qHigh > 0.8, L"the stream's own level reads high");
			double qLow = 0.0;
			for (int i = 0; i < 6; ++i)
			{
				const std::vector<float> t = SidSynth::NoisyTile(128, 0.8, 100u + i);
				qLow = s.Push(1.0, { FrameQuality::MeasureNoise(t.data(), 128) });
			}
			Assert::IsTrue(qLow < qHigh - 0.4, L"a denoised rung reads clearly lower than the stream's baseline");
			// The resolution term bounds the quality.
			const std::vector<float> t = SidSynth::NoisyTile(128, 3.0, 77);
			Assert::IsTrue(s.Push(0.5, { FrameQuality::MeasureNoise(t.data(), 128) }) <= 0.5 + 1e-12);
			// A picture with no noise to lose abstains (the term stays 1), never a false drop.
			FrameQuality::Signal clean;
			double q = 0.0;
			for (int i = 0; i < 20; ++i)
			{
				const std::vector<float> c = SidSynth::NoisyTile(128, 0.0, 1u);
				q = clean.Push(1.0, { FrameQuality::MeasureNoise(c.data(), 128) });
			}
			Assert::AreEqual(1.0, q);
		}
	};

	TEST_CLASS(SourceIdPipelineTests)
	{
	public:
		TEST_METHOD(ACleanStreamProducesNoEventNoNoticeAndStaysNative)
		{
			using namespace SourceIdentity;
			SidSynth::Clip c;
			c.frames = 120;
			c.switchAt = 1000;   // never
			Pipeline p;
			p.SetConsensusOn(true);
			uint64_t seen = 0;
			std::string text;
			Record last;
			for (int i = 0; i < c.frames; ++i)
				last = p.Push(SidSynth::Frame(c, i));
			Assert::AreEqual(0ull, p.Events(), L"no ABR event on a steady stream");
			Assert::IsFalse(p.TakeNotice(seen, text));
			Assert::AreEqual(1.0f, last.ratioV);
			Assert::AreEqual(1.0f, last.level);
			Assert::AreEqual(0u, last.rebaseGen[0] + last.rebaseGen[1]);
			Assert::IsTrue(last.Latched(Field::Quality) || last.tier >= 0, L"the tier latched from the classical voter");
		}

		TEST_METHOD(ARungDropIsDetectedLatchedAndNoticedOnceWithTheRebaseDeferred)
		{
			using namespace SourceIdentity;
			SidSynth::Clip c;
			Pipeline p;
			p.SetConsensusOn(true);
			uint64_t seen = 0;
			std::string text;
			Record r;
			int firstDropFrame = -1;
			for (int i = 0; i < c.frames; ++i)
			{
				r = p.Push(SidSynth::Frame(c, i));
				if (firstDropFrame < 0 && r.abrDirection == 1)
					firstDropFrame = i;
			}
			Assert::IsTrue(firstDropFrame >= c.switchAt, L"the drop is not reported before the switch");
			Assert::IsTrue(firstDropFrame <= c.switchAt + 8, L"detected within a few seconds (latency)");
			Assert::IsTrue(p.Events() >= 1ull);
			Assert::IsTrue(r.ratioV < 0.7f && r.ratioV > 0.6f, L"consensus latched the lower ratio");
			Assert::IsTrue(r.Latched(Field::AxisV));
			Assert::IsTrue(r.level < 0.9f, L"the event-anchored level dropped");
			Assert::IsTrue(r.rebaseGen[0] == 1u && r.rebaseGen[1] == 1u, L"one rebase request per axis, deferred into the record");
			Assert::IsTrue(p.TakeNotice(seen, text), L"the one-time notice fired");
			Assert::IsTrue(text.find("Stream quality dropped") != std::string::npos);
			Assert::IsFalse(p.TakeNotice(seen, text), L"once per title");
			Assert::AreEqual(1ull, r.noticeSerial);
			// The held record is readable from another frame's point of view.
			Record held;
			Assert::IsTrue(p.Held().ReadLatest(0, held));
			Assert::AreEqual(r.seq, held.seq);
		}

		TEST_METHOD(ADiscontinuityStartsAgainAndReArmsTheNotice)
		{
			using namespace SourceIdentity;
			SidSynth::Clip c;
			Pipeline p;
			p.SetConsensusOn(true);
			for (int i = 0; i < c.frames; ++i)
				p.Push(SidSynth::Frame(c, i));
			uint64_t seen = 0;
			std::string text;
			Assert::IsTrue(p.TakeNotice(seen, text));
			p.Discontinuity(c.frames + 1, 2);
			Record held;
			Assert::IsFalse(p.Held().ReadLatest(0, held), L"the old title's records are dropped");
			for (int i = 0; i < c.frames; ++i)
			{
				SidSynth::FrameInput in = SidSynth::Frame(c, i, 2);
				in.seq += 1000;
				in.timeUs += 1000ll * 1000000ll;
				p.Push(in);
			}
			Assert::IsTrue(p.TakeNotice(seen, text), L"a new title may be told again");
		}

		TEST_METHOD(NoticeCanBeSwitchedOff)
		{
			using namespace SourceIdentity;
			SidSynth::Clip c;
			Pipeline p;
			p.SetConsensusOn(true);
			p.SetNoticeEnabled(false);
			for (int i = 0; i < c.frames; ++i)
				p.Push(SidSynth::Frame(c, i));
			uint64_t seen = 0;
			std::string text;
			Assert::IsFalse(p.TakeNotice(seen, text));
		}

		TEST_METHOD(TheTraceIsIdenticalOnEveryRun)
		{
			SidSynth::Clip c;
			const std::string a = SidSynth::Trace(c);
			Assert::AreEqual(a, SidSynth::Trace(c));
			Assert::IsTrue(a.size() > 100);
		}
	};

	TEST_CLASS(SourceIdMonitorGateTests)
	{
	public:
		TEST_METHOD(ConsensusOffTheViewIsTheSnapshotAndNothingRuns)
		{
			SourceResolutionMonitor::Monitor m;
			Assert::IsFalse(m.SourceIdConsensus());
			const SourceResolutionMonitor::Snapshot s = m.Read();
			const SourceIdentity::QualityView v = m.QualityView(10.0);
			Assert::AreEqual(s.bitrateQuality, v.quality);
			Assert::IsFalse(v.fallback.engaged);
			SourceIdentity::Record r;
			Assert::IsFalse(m.SourceIdRecord(r), L"no worker ran: nothing published");
			Assert::AreEqual(std::string("Source identity: off"), m.SourceIdStatusLine(10.0));
			struct Port : SourceIdentity::IndicatorPort { int n = 0; void AdvanceRebase(int, uint64_t, const char*) override { ++n; } } port;
			Assert::AreEqual(0, m.ApplySourceIdRebase(port));
			Assert::AreEqual(0, port.n);
			Assert::AreEqual(1.0, s.frameQuality);
		}

		TEST_METHOD(ConsensusOnWithoutARecordFallsBackToClassicalVisibly)
		{
			SourceResolutionMonitor::Monitor m;
			m.SetSourceIdConsensus(true);
			const SourceIdentity::QualityView v = m.QualityView(10.0);
			Assert::IsTrue(v.fallback.engaged);
			Assert::AreEqual(m.Read().bitrateQuality, v.quality);
			Assert::AreEqual(std::string("Source identity: classical only (no record)"), m.SourceIdStatusLine(10.0));
			// The auto-engage gate: no record, the classical decision stands and the fallback is stated.
			std::string why;
			double fx = 1.0, fy = 1.0;
			m.AutoFactors(1920, 1080, 0.8, why, fx, fy, 10.0, 1);
			Assert::AreEqual(1.0, fx);
			Assert::AreEqual(1.0, fy);
			Assert::IsTrue(m.SourceIdGateFallback().engaged);
			Assert::IsTrue(m.SourceIdGateFallback().reason.find("no record") != std::string::npos);
			m.SetSourceIdConsensus(false);
			m.AutoFactors(1920, 1080, 0.8, why, fx, fy, 11.0, 2);
			Assert::IsFalse(m.SourceIdGateFallback().engaged, L"off releases the gate's fallback");
			Assert::IsFalse(m.QualityView(11.0).fallback.engaged);
		}

		// SID-B: Shadow runs the pipeline and publishes P14, but the classical decision stands: AutoFactors, the quality view and
		// the re-base port are byte-identical to Off. The Off and Shadow monitors see the same synthetic 2x-upscaled tiles
		// (separable bilinear of noise, a level jump every 6 s = a shot) and every frame's decision is compared.
		TEST_METHOD(ShadowDecidesLikeOffForAutoFactorsAndPublishes)
		{
			constexpr int T = 128, tiles = 8;
			auto frameTiles = [&](int frame) {
				std::vector<float> all(static_cast<size_t>(T) * T * tiles);
				const int shot = frame / 12;
				for (int t = 0; t < tiles; ++t)
				{
					SidSynth::Lcg rng(7000u + static_cast<uint32_t>(shot * 31 + t));
					const int h = T / 2;
					std::vector<double> low(static_cast<size_t>(h) * h);
					for (double& v : low) v = 0.25 + 0.5 * rng.Next();
					auto at = [&](int x, int y) { x = (std::min)((std::max)(x, 0), h - 1); y = (std::min)((std::max)(y, 0), h - 1); return low[static_cast<size_t>(y) * h + x]; };
					const double level = (shot % 2 ? 0.12 : -0.08) + 0.0007 * (frame % 12);   // a jump per shot, a drift within it
					for (int y = 0; y < T; ++y)
						for (int x = 0; x < T; ++x)
						{
							const int sx = x / 2, sy = y / 2;
							const int dx = x % 2 ? 1 : -1, dy = y % 2 ? 1 : -1;
							const double v = 0.5625 * at(sx, sy) + 0.1875 * at(sx + dx, sy) + 0.1875 * at(sx, sy + dy) + 0.0625 * at(sx + dx, sy + dy);
							all[static_cast<size_t>(t) * T * T + static_cast<size_t>(y) * T + x] = static_cast<float>((std::min)(1.0, (std::max)(0.0, v + level)));
						}
				}
				return all;
			};
			SourceResolutionMonitor::Monitor off, shadow;
			off.SetSourceIdMode(SourceResolutionMonitor::Monitor::SourceIdMode::Off);
			shadow.SetSourceIdMode(SourceResolutionMonitor::Monitor::SourceIdMode::Shadow);
			Assert::IsFalse(shadow.SourceIdConsensus(), L"shadow does not decide");
			Assert::IsTrue(shadow.SourceIdRunning());
			Assert::AreEqual(std::string("Source identity: shadow (classical decides)"), shadow.SourceIdStatusLine(1.0));
			const std::vector<float> none;
			int differences = 0, engagedFrames = 0, shadowEngagedFrames = 0;
			for (int f = 0; f < 120; ++f)
			{
				const double seconds = 0.5 * f;
				const std::vector<float> data = frameTiles(f);
				off.AnalyseTilesForTest(data, tiles, seconds, 1920, 1080, none, none, tiles, T, 64.0 / 1023.0, static_cast<uint64_t>(f + 1));
				shadow.AnalyseTilesForTest(data, tiles, seconds, 1920, 1080, none, none, tiles, T, 64.0 / 1023.0, static_cast<uint64_t>(f + 1));
				std::string whyOff, whyShadow;
				double ox = 1.0, oy = 1.0, sx = 1.0, sy = 1.0;
				off.AutoFactors(1920, 1080, 0.8, whyOff, ox, oy, seconds + 0.1, static_cast<uint64_t>(f + 1));
				shadow.AutoFactors(1920, 1080, 0.8, whyShadow, sx, sy, seconds + 0.1, static_cast<uint64_t>(f + 1));
				if (ox != sx || oy != sy || whyOff != whyShadow) ++differences;
				if (ox < 1.0 || oy < 1.0) ++engagedFrames;
				if (sx < 1.0 || sy < 1.0) ++shadowEngagedFrames;
				const SourceResolutionMonitor::Snapshot a = off.Read(), b = shadow.Read();
				if (a.declaredRatio != b.declaredRatio || a.confidence != b.confidence || a.bitrateQuality != b.bitrateQuality) ++differences;
				const SourceIdentity::QualityView qa = off.QualityView(seconds), qb = shadow.QualityView(seconds);
				if (qa.quality != qb.quality || qb.fallback.engaged || qb.origin != qa.origin) ++differences;
			}
			char line[160];
			snprintf(line, sizeof(line), "shadow vs off: %d differing decisions; engaged frames %d (off) %d (shadow)\n", differences, engagedFrames, shadowEngagedFrames);
			Logger::WriteMessage(line);
			Assert::AreEqual(0, differences, L"AutoFactors, the snapshot and the quality view are byte-identical to Off");
			// Not vacuous: the classical path engages on this script (an earlier run: 96 of 120 frames), and both modes engage on the same frames.
			Assert::IsTrue(engagedFrames > 0, L"the script makes the classical path engage, so the comparison covers a real decision");
			Assert::AreEqual(engagedFrames, shadowEngagedFrames, L"shadow engages on exactly the frames Off does");
			SourceIdentity::Record rec;
			Assert::IsTrue(shadow.SourceIdRecord(rec), L"the record is published in shadow");
			Assert::IsTrue((rec.flags & SourceIdentity::kFlagShadow) != 0, L"and flagged as shadow");
			Assert::IsTrue((rec.flags & SourceIdentity::kFlagConsensusOn) == 0);
			Assert::IsFalse(off.SourceIdRecord(rec), L"off publishes nothing");
			struct Port : SourceIdentity::IndicatorPort { int n = 0; void AdvanceRebase(int, uint64_t, const char*) override { ++n; } } port;
			Assert::AreEqual(0, shadow.ApplySourceIdRebase(port), L"shadow never moves a re-base stamp");
			Assert::AreEqual(0, port.n);
		}

		TEST_METHOD(WouldBeLinesNameTheFieldTheChangeAndTheReason)
		{
			SourceIdentity::Consensus c;
			SourceIdentity::Vote v;
			v.voter = SourceIdentity::VoterId::ClassicalResolution;
			v.h.ratioH = v.h.ratioV = 0.5;
			SourceIdentity::SetField(v, SourceIdentity::Field::AxisH, 0.9);
			SourceIdentity::SetField(v, SourceIdentity::Field::AxisV, 0.9);
			std::vector<std::string> lines;
			for (int64_t t = 0; t <= 3000000; t += 500000)
			{
				v.evidenceTimeUs = t;
				c.Submit(v);
				const SourceIdentity::Result r = c.Evaluate(static_cast<uint64_t>(t / 1000 + 1), t);
				for (const std::string& l : SourceIdentity::WouldBeLines(r)) lines.push_back(l);
			}
			Assert::AreEqual(size_t(2), lines.size(), L"one line per changed axis, once");
			Assert::IsTrue(lines[0].rfind("source_id would-be: ratioH 1.000 -> 0.500 (changed", 0) == 0, L"format: field from -> to (reason)");
			Assert::IsTrue(lines[1].rfind("source_id would-be: ratioV 1.000 -> 0.500 (changed", 0) == 0);
		}

		// W23-B3 M2#2 (U2 B4 CPU BandMemo): FrameQuality::MeasureNoise acquires sigma 1 & 2 (Reflect) from CPU kBandMemo,
		// matches independent memo=nullptr reference, reuses memo on repeat, visibly reports fallback when memo is absent or
		// unregistered, and clearly separates synthetic noisy vs noiseless tiles.
		TEST_METHOD(FrameQualityBandMemoIdentity)
		{
			namespace GB = Vpv::GrainBand;
			using namespace SourceIdentity;
			using Reg = VpSkeleton::SkeletonMemo::Registration;

			const int T = 64;
			std::vector<float> noisyTile = SidSynth::NoisyTile(T, 3.0, 4242u);
			std::vector<float> cleanTile = SidSynth::NoisyTile(T, 0.0, 4242u);
			// Plant a steep edge quadrant on both tiles so gradient masking (< 3.0) is genuinely exercised.
			for (int y = 0; y < 16; ++y)
				for (int x = 0; x < 16; ++x)
				{
					const float step = (x >= 8) ? (180.0f / 255.0f) : (40.0f / 255.0f);
					noisyTile[static_cast<size_t>(y) * T + x] = step;
					cleanTile[static_cast<size_t>(y) * T + x] = step;
				}

			VpSkeleton::SkeletonMemo memo;
			const uint64_t kSeqNoisy = 77u;
			const uint64_t kSeqClean = 78u;

			VpSkeleton::OwnerFill p13Cpu;
			p13Cpu.frameScoped = true;
			p13Cpu.renderThreadOnly = false;
			p13Cpu.fill = [&noisyTile, &cleanTile, kSeqNoisy, kSeqClean](const VpSkeleton::OwnerFillRequest& req, VpSkeleton::SkeletonMemo&, VpSkeleton::OwnerFillOutput& out) {
				const std::vector<float>* src = (req.seq == kSeqNoisy) ? &noisyTile : (req.seq == kSeqClean ? &cleanTile : nullptr);
				if (!src)
					return false;
				out.bytes.resize(src->size() * sizeof(float));
				std::memcpy(out.bytes.data(), src->data(), out.bytes.size());
				return true;
			};
			Assert::IsTrue(memo.RegisterOwnerFill(VpSkeleton::kP13SourceRing, VpSkeleton::AcquireForm::Cpu,
				VpSkeleton::SkeletonMemo::OwnerUnitOf(VpSkeleton::kP13SourceRing), std::move(p13Cpu)) == Reg::Registered);

			const std::shared_ptr<GB::Counters> counters = std::make_shared<GB::Counters>();
			Assert::IsTrue(memo.RegisterOwnerFill(VpSkeleton::kBandMemo, VpSkeleton::AcquireForm::Cpu,
				GB::kBandMemoUnit, GB::MakeBandMemoCpuOwnerFill(counters)) == Reg::Registered);

			const FrameQuality::TileNoise res = FrameQuality::MeasureNoise(noisyTile.data(), T, &memo, kSeqNoisy);
			Assert::IsTrue(res.valid, L"MeasureNoise with kBandMemo produces valid measurement");
			Assert::IsTrue(res.usedBandMemo, L"MeasureNoise reports usedBandMemo == true");
			Assert::IsTrue(res.bandMemoFallback.empty(), L"MeasureNoise has empty fallback reason on kBandMemo hit");
			Assert::AreEqual(uint64_t(2), counters->fills.load(), L"MeasureNoise computes sigma 1 & 2 once each (fills == 2)");

			// Repeated MeasureNoise for the same seq reuses both cached Reflect bands with zero extra fills.
			const FrameQuality::TileNoise resRepeat = FrameQuality::MeasureNoise(noisyTile.data(), T, &memo, kSeqNoisy);
			Assert::AreEqual(uint64_t(2), counters->fills.load(), L"repeat MeasureNoise reuses memo without extra fills");
			Assert::IsTrue(resRepeat.usedBandMemo && resRepeat.bandMemoFallback.empty());
			Assert::AreEqual(res.sigma, resRepeat.sigma, 0.0, L"repeat sigma is exact");
			Assert::AreEqual(res.coverage, resRepeat.coverage, 0.0, L"repeat coverage is exact");

			// Independent reference (memo = nullptr): verifies tight numeric identity + visible fallback.
			const FrameQuality::TileNoise ref = FrameQuality::MeasureNoise(noisyTile.data(), T);
			Assert::IsTrue(ref.valid, L"reference MeasureNoise is valid");
			Assert::IsFalse(ref.usedBandMemo, L"negative control: memo=nullptr reports usedBandMemo == false");
			Assert::IsFalse(ref.bandMemoFallback.empty(), L"negative control: memo=nullptr populates visible fallback reason");
			Assert::AreEqual(ref.sigma, res.sigma, 1e-3, L"kBandMemo sigma matches independent reference within 1e-3 8-bit codes");
			Assert::AreEqual(ref.coverage, res.coverage, 1e-3, L"kBandMemo coverage matches independent reference");

			// Negative control: unregistered memo visibly reports fallback and still matches reference.
			VpSkeleton::SkeletonMemo emptyMemo;
			const FrameQuality::TileNoise unreg = FrameQuality::MeasureNoise(noisyTile.data(), T, &emptyMemo, kSeqNoisy);
			Assert::IsTrue(unreg.valid, L"unregistered memo falls back gracefully");
			Assert::IsFalse(unreg.usedBandMemo, L"unregistered memo reports usedBandMemo == false");
			Assert::IsFalse(unreg.bandMemoFallback.empty(), L"unregistered memo populates visible fallback reason");
			Assert::AreEqual(ref.sigma, unreg.sigma, 0.0, L"unregistered fallback sigma matches reference bit-for-bit");

			// Negative control: seq = 0 with valid memo visibly reports unkeyed fallback.
			const FrameQuality::TileNoise unkeyed = FrameQuality::MeasureNoise(noisyTile.data(), T, &memo, 0u);
			Assert::IsTrue(unkeyed.valid);
			Assert::IsFalse(unkeyed.usedBandMemo, L"seq=0 reports usedBandMemo == false");
			Assert::IsFalse(unkeyed.bandMemoFallback.empty(), L"seq=0 populates visible fallback reason");

			// Negative control: noiseless tile vs synthetic noisy tile clearly separate in measured sigma.
			const FrameQuality::TileNoise cleanRes = FrameQuality::MeasureNoise(cleanTile.data(), T, &memo, kSeqClean);
			Assert::IsTrue(cleanRes.valid && cleanRes.usedBandMemo && cleanRes.bandMemoFallback.empty(),
				L"noiseless tile uses kBandMemo cleanly");
			Assert::IsTrue(cleanRes.sigma < 0.05, L"noiseless flat+sloped tile reads near-zero sigma (< 0.05)");
			Assert::IsTrue(res.sigma > 1.0, L"noisy tile reads real noise sigma (> 1.0 8-bit codes)");
		}

		// W23-Fix2 Carbon HIGH-1 / MED-4 regression:
		// Two distinct T x T tiles on the EXACT SAME (memo, seq) inside a full W x H CPU frame
		// key kBandMemo by (frameW, frameH, cropX, cropY, T, T) so Tile A and Tile B NEVER alias
		// and each matches its independent memo=nullptr computation.
		TEST_METHOD(FrameQualityBandMemoDistinctTilesSameSeqNeverAlias)
		{
			namespace BM = VpUnit::BandMemo;
			namespace GB = Vpv::GrainBand;
			using namespace SourceIdentity;
			using Reg = VpSkeleton::SkeletonMemo::Registration;

			const int T = 64;
			const int W = 2 * T, H = T;
			const std::vector<float> tileA = SidSynth::NoisyTile(T, 3.5, 1111u);
			const std::vector<float> tileB = SidSynth::NoisyTile(T, 0.25, 2222u);

			// Pack Tile A at (0, 0) and Tile B at (T, 0) inside a single real W x H frame.
			std::vector<float> fullFrame(static_cast<size_t>(W) * H, 0.0f);
			for (int y = 0; y < T; ++y)
			{
				std::memcpy(fullFrame.data() + static_cast<size_t>(y) * W, tileA.data() + static_cast<size_t>(y) * T, size_t(T) * sizeof(float));
				std::memcpy(fullFrame.data() + static_cast<size_t>(y) * W + T, tileB.data() + static_cast<size_t>(y) * T, size_t(T) * sizeof(float));
			}

			VpSkeleton::SkeletonMemo memo;
			const uint64_t kSeq = 88u;
			VpSkeleton::OwnerFill p13Cpu;
			p13Cpu.frameScoped = true;
			p13Cpu.renderThreadOnly = false;
			p13Cpu.fill = [&fullFrame, kSeq](const VpSkeleton::OwnerFillRequest& req, VpSkeleton::SkeletonMemo&, VpSkeleton::OwnerFillOutput& out) {
				if (req.seq != kSeq)
					return false;
				out.bytes.resize(fullFrame.size() * sizeof(float));
				std::memcpy(out.bytes.data(), fullFrame.data(), out.bytes.size());
				return true;
			};
			Assert::IsTrue(memo.RegisterOwnerFill(VpSkeleton::kP13SourceRing, VpSkeleton::AcquireForm::Cpu,
				VpSkeleton::SkeletonMemo::OwnerUnitOf(VpSkeleton::kP13SourceRing), std::move(p13Cpu)) == Reg::Registered);

			const std::shared_ptr<GB::Counters> counters = std::make_shared<GB::Counters>();
			Assert::IsTrue(memo.RegisterOwnerFill(VpSkeleton::kBandMemo, VpSkeleton::AcquireForm::Cpu,
				GB::kBandMemoUnit, GB::MakeBandMemoCpuOwnerFill(counters)) == Reg::Registered);

			FrameQuality::TileOrigin originA{ 0u, 0u, static_cast<uint32_t>(W), static_cast<uint32_t>(H) };
			FrameQuality::TileOrigin originB{ static_cast<uint32_t>(T), 0u, static_cast<uint32_t>(W), static_cast<uint32_t>(H) };

			const FrameQuality::TileNoise resA = FrameQuality::MeasureNoise(tileA.data(), T, &memo, kSeq, originA);
			const FrameQuality::TileNoise resB = FrameQuality::MeasureNoise(tileB.data(), T, &memo, kSeq, originB);
			const FrameQuality::TileNoise refA = FrameQuality::MeasureNoise(tileA.data(), T);
			const FrameQuality::TileNoise refB = FrameQuality::MeasureNoise(tileB.data(), T);

			Assert::IsTrue(resA.valid && resA.usedBandMemo && resA.bandMemoFallback.empty(), L"Tile A hit crop-keyed kBandMemo");
			Assert::IsTrue(resB.valid && resB.usedBandMemo && resB.bandMemoFallback.empty(), L"Tile B hit crop-keyed kBandMemo");
			Assert::AreEqual(uint64_t(4), counters->fills.load(), L"two distinct tiles x two sigmas execute 4 fills on same seq");
			Assert::AreEqual(refA.sigma, resA.sigma, 1e-3, L"Tile A sigma matches independent tile computation");
			Assert::AreEqual(refB.sigma, resB.sigma, 1e-3, L"Tile B sigma matches independent tile computation");
			Assert::IsTrue(resA.sigma > 2.0 && resB.sigma < 0.6, L"Tile A (noisy) and Tile B (quiet) stay cleanly separated on same seq");

			// Repeat Tile B reuses cached bands without a 5th fill.
			const FrameQuality::TileNoise resB2 = FrameQuality::MeasureNoise(tileB.data(), T, &memo, kSeq, originB);
			Assert::AreEqual(uint64_t(4), counters->fills.load(), L"repeat Tile B reuses cached kBandMemo bands");
			Assert::AreEqual(resB.sigma, resB2.sigma, 0.0, L"repeat Tile B sigma is exact");

			// Negative control (Rule 3): prove the unkeyed aliasing hazard is real — omitting crop origin maps both
			// tiles onto one identical VariantKey, whereas crop-keyed variants are strictly distinct, and passing a
			// non-zero origin without frame dimensions visibly declines rather than aliasing onto (0, 0).
			BM::BandKey naiveKey;
			naiveKey.resolution = BM::Stage::Full;
			naiveKey.width = static_cast<uint32_t>(T);
			naiveKey.height = static_cast<uint32_t>(T);
			naiveKey.sigmaPx = 1.0;
			naiveKey.law = BM::Law::Taps;
			naiveKey.edge = VpUnit::BandFromBase::EdgeMode::Reflect;
			naiveKey.minOne = false;

			BM::BandKey keyA = naiveKey;
			keyA.width = static_cast<uint32_t>(W);
			keyA.height = static_cast<uint32_t>(H);
			keyA.cropX = 0u; keyA.cropY = 0u; keyA.cropW = static_cast<uint32_t>(T); keyA.cropH = static_cast<uint32_t>(T);
			BM::BandKey keyB = keyA;
			keyB.cropX = static_cast<uint32_t>(T);

			Assert::IsTrue(BM::ToVariant(keyA) != BM::ToVariant(keyB), L"crop-keyed VariantKeys for Tile A and Tile B are distinct");
			Assert::AreNotEqual(BM::ToVariant(keyA).Hash(), BM::ToVariant(keyB).Hash(), L"crop-keyed hashes for Tile A and Tile B are distinct");

			FrameQuality::TileOrigin missingFrameOrigin{ static_cast<uint32_t>(T), 0u, 0u, 0u };
			const FrameQuality::TileNoise declined = FrameQuality::MeasureNoise(tileB.data(), T, &memo, kSeq, missingFrameOrigin);
			Assert::IsTrue(declined.valid, L"declined tile still falls back cleanly to U2 CPU blur");
			Assert::IsFalse(declined.usedBandMemo, L"non-zero tile origin without frame dims refuses kBandMemo rather than aliasing (0,0)");
			Assert::IsFalse(declined.bandMemoFallback.empty(), L"declined tile records visible fallback reason");
			Assert::AreEqual(refB.sigma, declined.sigma, 1e-3, L"fallback sigma still matches Tile B reference");
		}
	};
}
