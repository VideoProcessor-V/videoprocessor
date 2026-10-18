#include "pch.h"
#include "CppUnitTest.h"

#include <ActivePictureEvidence.h>
#include <AnalysisLumaSource.h>
#include <ContentIntentObserver.h>
#include <QueuedFrameStatistics.h>
#include <led/LedDriveModule.h>
#include <led/LedLookahead.h>
#include <vprenderer/AlphaQueuePolicy.h>
#include <vprenderer/UploadReuse.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <map>
#include <random>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// Efficiency round 2026-10-18 (efficiency_fixes, docs/agent-reports/efficiency.md).
// (1) UploadReuse: the frame shown takes the planes the de-judder uploaded for it as the
//     next frame only when they are exactly that upload (same payload, frame, layout, route).
// (2) The look-ahead walk measures a frame's active picture only for an observer that still
//     wants it; each observer's WantsFrame mirrors its ObserveFrame's own early returns, so
//     the observers see exactly the same calls (proved on a sliding queue below) while each
//     frame is measured once instead of once per walk.
namespace
{
	// A 10-bit P010 picture (codes << 6), with a moving bright bar so frames differ.
	struct P010Picture
	{
		int width, height;
		size_t pitch;
		std::vector<uint8_t> bytes;
		P010Picture(int w, int h) : width(w), height(h), pitch(static_cast<size_t>(w) * 2),
			bytes(static_cast<size_t>(w) * 2 * h + static_cast<size_t>(w) * 2 * (h / 2), 0) {}
		void Paint(int frame)
		{
			for (int y = 0; y < height; ++y)
				for (int x = 0; x < width; ++x)
				{
					const bool bar = y < height / 8 || y >= height - height / 8;   // letterbox
					const bool bright = ((x + frame * 7) / 32) % 5 == 0;
					const int code = bar ? 64 : bright ? 800 : 200 + ((x * 3 + y + frame) % 97);
					const uint16_t packed = static_cast<uint16_t>(code << 6);
					uint8_t* p = bytes.data() + static_cast<size_t>(y) * pitch + static_cast<size_t>(x) * 2;
					p[0] = static_cast<uint8_t>(packed & 0xff);
					p[1] = static_cast<uint8_t>(packed >> 8);
				}
			for (size_t i = pitch * height; i + 1 < bytes.size(); i += 2)
			{
				const uint16_t packed = static_cast<uint16_t>(512 << 6);
				bytes[i] = static_cast<uint8_t>(packed & 0xff);
				bytes[i + 1] = static_cast<uint8_t>(packed >> 8);
			}
		}
		AnalysisLumaSource Source() const
		{
			AnalysisLumaSource s;
			s.data = bytes.data();
			s.dataBytes = bytes.size();
			s.width = width;
			s.height = height;
			s.rowBytes = pitch;
			s.chromaRowBytes = pitch;
			s.format = AnalysisLumaFormat::P010;
			s.colorspace = ColorSpace::REC_709;
			s.generation = 1;
			return s;
		}
	};

	UploadReuse::Record MatchingRecord(const void* payload)
	{
		UploadReuse::Record r;
		r.valid = true;
		r.payload = payload;
		r.sequence = 41;
		r.width = 3840;
		r.height = 2160;
		r.chromaHeight = 2160;
		r.rowBytes = 7680;
		r.raw = true;
		return r;
	}

	UploadReuse::Request MatchingRequest(const void* payload)
	{
		UploadReuse::Request q;
		q.enabled = true;
		q.payload = payload;
		q.sequence = 41;
		q.width = 3840;
		q.height = 2160;
		q.chromaHeight = 2160;
		q.rowBytes = 7680;
		q.raw = true;
		q.texturesPresent = true;
		q.uploadedSequenceMatches = true;
		return q;
	}

	// Records every call an observer receives, and counts the active-picture measurements
	// the walk makes for it (the work the fix saves).
	struct Recorder : QueuedFrameStatistics::IObserver
	{
		QueuedFrameStatistics::IObserver& inner;
		std::vector<std::string> calls;
		explicit Recorder(QueuedFrameStatistics::IObserver& in) : inner(in) {}
		void BeginWalk(size_t futureFrames) override
		{
			calls.push_back("walk " + std::to_string(futureFrames));
			inner.BeginWalk(futureFrames);
		}
		void ObserveFrame(uint64_t sequence, size_t ahead, const AnalysisLumaSource& luma,
			const QueuedFrameStatistics::PictureRect& picture) override
		{
			calls.push_back("frame " + std::to_string(sequence) + " ahead " + std::to_string(ahead) + " rect " +
				std::to_string(picture.left) + "," + std::to_string(picture.top) + "," + std::to_string(picture.right) + "," +
				std::to_string(picture.bottom));
			inner.ObserveFrame(sequence, ahead, luma, picture);
		}
		bool WantsFrame(uint64_t sequence) const override { return inner.WantsFrame(sequence); }
	};

	// The calls that reach an observer's state: the walks and each frame's first observation (the
	// observers ignore any repeat of a frame they have seen, which the fix no longer offers).
	std::vector<std::string> FirstObservations(const std::vector<std::string>& calls)
	{
		std::vector<std::string> kept;
		std::map<std::string, bool> seen;
		for (const std::string& c : calls)
		{
			if (c.rfind("frame", 0) != 0) { kept.push_back(c); continue; }
			const std::string key = c.substr(0, c.find(" ahead"));
			if (!seen[key]) { seen[key] = true; kept.push_back(c); }
		}
		return kept;
	}

	// The renderer's statistics-only walk (LibplaceboVideoRenderer AnalyzeActivePictureLookahead,
	// observeStatisticsOnly) over a stream: each dequeue walks the current frame and the queued
	// frames after it (AlphaQueuePolicy::SelectLookaheadWalk, whole queue), measures the frame's
	// active picture and hands it to the observer - with efficiency_fixes only when it wants it.
	struct WalkEntry
	{
		bool cadenceRepeat = false;
		int frame = 0;
	};

	struct WalkResult
	{
		std::map<uint64_t, int> measurements;   // per frame: active-picture extractions
		int total = 0;
	};

	WalkResult RunWalk(QueuedFrameStatistics::IObserver& observer, bool efficiencyFixes, int frames, size_t queued,
		std::vector<P010Picture>& pictures)
	{
		WalkResult result;
		for (int current = 1; current <= frames; ++current)
		{
			std::vector<WalkEntry> queue;
			for (int f = current; f < current + static_cast<int>(queued) && f <= frames; ++f)
			{
				WalkEntry entry;
				entry.frame = f;
				queue.push_back(entry);
			}
			const auto walk = AlphaQueuePolicy::SelectLookaheadWalk(queue, 0, LedDrive::LedDriveModule::WHOLE_QUEUE, 8);
			observer.BeginWalk(walk.window.indices.empty() ? 0 : walk.window.indices.size() - 1);
			for (size_t index = 0; index < walk.window.indices.size(); ++index)
			{
				const int frame = queue[walk.window.indices[index]].frame;
				const uint64_t sequence = static_cast<uint64_t>(frame);
				if (efficiencyFixes && !observer.WantsFrame(sequence))
					continue;
				const AnalysisLumaSource source = pictures[static_cast<size_t>(frame) % pictures.size()].Source();
				const ActivePictureEvidence evidence = ExtractActivePictureEvidence(source);
				++result.measurements[sequence];
				++result.total;
				QueuedFrameStatistics::PictureRect rect;
				rect.right = source.width;
				rect.bottom = source.height;
				if (evidence.available && evidence.classification == ActivePictureClassification::BAR_CROP_TRUSTED)
				{
					rect.left = evidence.trustedBounds.left;
					rect.top = evidence.trustedBounds.top;
					rect.right = evidence.trustedBounds.right;
					rect.bottom = evidence.trustedBounds.bottom;
				}
				observer.ObserveFrame(sequence, index, source, rect);
			}
		}
		return result;
	}
}

namespace VideoProcessorTest
{
	TEST_CLASS(EfficiencyTests)
	{
	public:
		TEST_METHOD(UploadReuseOnlyForExactlyTheSameUpload)
		{
			int payloadA = 0, payloadB = 0;
			const UploadReuse::Record r = MatchingRecord(&payloadA);
			Assert::IsTrue(UploadReuse::CanReuse(r, MatchingRequest(&payloadA)), L"the same payload, frame and layout");
			// Every difference refuses (the frame is then uploaded as before).
			auto refuses = [&](const wchar_t* what, auto change) {
				UploadReuse::Record rr = r;
				UploadReuse::Request q = MatchingRequest(&payloadA);
				change(rr, q);
				Assert::IsFalse(UploadReuse::CanReuse(rr, q), what);
			};
			refuses(L"efficiency_fixes off", [](auto&, auto& q) { q.enabled = false; });
			refuses(L"nothing recorded", [](auto& rr, auto&) { rr.valid = false; });
			refuses(L"another payload", [&](auto&, auto& q) { q.payload = &payloadB; });
			refuses(L"the recorded payload freed", [](auto& rr, auto&) { rr.payload = nullptr; });
			refuses(L"no payload", [](auto& rr, auto& q) { rr.payload = nullptr; q.payload = nullptr; });
			refuses(L"another frame", [](auto&, auto& q) { q.sequence = 42; });
			refuses(L"frame 0 (unknown)", [](auto& rr, auto& q) { rr.sequence = 0; q.sequence = 0; });
			refuses(L"width", [](auto&, auto& q) { q.width = 1920; });
			refuses(L"height", [](auto&, auto& q) { q.height = 1080; });
			refuses(L"chroma height (4:2:0 vs 4:2:2)", [](auto&, auto& q) { q.chromaHeight = 1080; });
			refuses(L"row bytes", [](auto&, auto& q) { q.rowBytes = 7936; });
			refuses(L"route (CPU vs GPU unpack)", [](auto&, auto& q) { q.raw = false; });
			refuses(L"the RGB luma-only route", [](auto& rr, auto&) { rr.rgbLumaOnly = true; });
			refuses(L"planes released", [](auto&, auto& q) { q.texturesPresent = false; });
			refuses(L"the pair's frame no longer recorded", [](auto&, auto& q) { q.uploadedSequenceMatches = false; });
			// The CPU route as well.
			UploadReuse::Record cpu = r;
			cpu.raw = false;
			UploadReuse::Request cpuRequest = MatchingRequest(&payloadA);
			cpuRequest.raw = false;
			Assert::IsTrue(UploadReuse::CanReuse(cpu, cpuRequest), L"the CPU (P010 / P210) route");
		}

		TEST_METHOD(ContentIntentWantsFrameMirrorsObserveFrame)
		{
			ContentIntent::Observer observer;
			Assert::IsFalse(observer.WantsFrame(1), L"off: wants nothing");
			observer.Configure(true, 2.0);
			Assert::IsTrue(observer.WantsFrame(1), L"on, nothing seen: wants the frame");
			P010Picture picture(256, 144);
			picture.Paint(0);
			QueuedFrameStatistics::PictureRect rect;
			rect.right = picture.width;
			rect.bottom = picture.height;
			observer.ObserveFrame(5, 0, picture.Source(), rect);
			Assert::IsFalse(observer.WantsFrame(5), L"seen: not again");
			Assert::IsFalse(observer.WantsFrame(4), L"older: not again");
			Assert::IsTrue(observer.WantsFrame(6), L"the next frame");
			observer.Configure(false, 2.0);
			Assert::IsFalse(observer.WantsFrame(6), L"switched off");
		}

		TEST_METHOD(LedLookaheadWantsFrameMirrorsObserveFrame)
		{
			LedDrive::LedDriveConfig config;
			config.mode = LedDrive::Mode::Compensate;
			LedDrive::LedDriveModule module(LedDrive::ProjectorConfig(), config);
			LedDrive::LookaheadObserver observer(module);
			Assert::IsTrue(module.WantsPredictions());
			Assert::IsTrue(observer.WantsFrame(10), L"nothing measured: wants the frame");
			P010Picture picture(256, 144);
			picture.Paint(0);
			QueuedFrameStatistics::PictureRect rect;
			rect.right = picture.width;
			rect.bottom = picture.height;
			observer.ObserveFrame(10, 0, picture.Source(), rect);
			Assert::IsFalse(observer.WantsFrame(10), L"measured: not again");
			Assert::IsFalse(observer.WantsFrame(9), L"older: not again");
			Assert::IsTrue(observer.WantsFrame(11), L"the next frame");
			Assert::IsTrue(observer.WantsFrame(10 + 2000), L"far ahead (a new stream) is wanted");
			// Without look-ahead the LED wants no predictions: nothing is measured for it.
			LedDrive::LedDriveConfig off;
			off.mode = LedDrive::Mode::Script;
			off.scriptLookahead = false;
			LedDrive::LedDriveModule quiet(LedDrive::ProjectorConfig(), off);
			LedDrive::LookaheadObserver quietObserver(quiet);
			Assert::IsFalse(quietObserver.WantsFrame(1), L"no predictions wanted");
		}

		// The walk with and without the fix on a sliding Normal queue (5 frames): the observers
		// receive exactly the same calls (same frames, same order, same picture rectangles) and
		// end in the same state, while each frame's active picture is measured once instead of
		// once per walk that reaches it.
		TEST_METHOD(WalkSkipsOnlyFramesNoObserverWants)
		{
			std::vector<P010Picture> pictures;
			for (int i = 0; i < 6; ++i)
			{
				pictures.emplace_back(320, 180);
				pictures.back().Paint(i);
			}
			const int frames = 40;
			const size_t queued = 5;
			// Content intent.
			{
				ContentIntent::Observer before, after;
				before.Configure(true, 2.0);
				after.Configure(true, 2.0);
				Recorder recordBefore(before), recordAfter(after);
				const WalkResult old = RunWalk(recordBefore, false, frames, queued, pictures);
				const WalkResult now = RunWalk(recordAfter, true, frames, queued, pictures);
				Assert::IsTrue(FirstObservations(recordBefore.calls) == FirstObservations(recordAfter.calls),
					L"content intent: the same first observation of every frame");
				const ContentIntentStatus a = before.Status(), b = after.Status();
				Assert::IsTrue(a.content == b.content && a.candidate == b.candidate && a.source == b.source,
					L"content intent: the same classification");
				for (const auto& m : now.measurements)
					Assert::AreEqual(1, m.second, L"content intent: each frame measured once");
				Assert::AreEqual(frames, now.total);
				Assert::IsTrue(old.total > 4 * frames - 12, L"before: measured once per walk (about 5 per frame)");
				char line[160];
				snprintf(line, sizeof(line), "content intent walk, %d frames, queue %zu: %d measurements before, %d after\n",
					frames, queued, old.total, now.total);
				Logger::WriteMessage(line);
			}
			// The LED's look-ahead (compensate).
			{
				LedDrive::LedDriveConfig config;
				config.mode = LedDrive::Mode::Compensate;
				LedDrive::LedDriveModule moduleBefore(LedDrive::ProjectorConfig(), config);
				LedDrive::LedDriveModule moduleAfter(LedDrive::ProjectorConfig(), config);
				LedDrive::LookaheadObserver before(moduleBefore), after(moduleAfter);
				Recorder recordBefore(before), recordAfter(after);
				const WalkResult old = RunWalk(recordBefore, false, frames, queued, pictures);
				const WalkResult now = RunWalk(recordAfter, true, frames, queued, pictures);
				// The observer sees the walks (BeginWalk) and every frame it measures alike; the
				// frames it ignored before are simply not offered now.
				std::vector<std::string> measuredBefore, measuredAfter;
				for (int f = 1; f <= frames; ++f)
					Assert::IsFalse(recordAfter.WantsFrame(static_cast<uint64_t>(f)), L"LED: every frame measured");
				for (const std::string& c : recordAfter.calls) if (c.rfind("frame", 0) == 0) measuredAfter.push_back(c);
				std::map<std::string, bool> firstSeen;
				for (const std::string& c : recordBefore.calls)
					if (c.rfind("frame", 0) == 0)
					{
						const std::string key = c.substr(0, c.find(" ahead"));
						if (!firstSeen[key]) { firstSeen[key] = true; measuredBefore.push_back(c); }
					}
				Assert::IsTrue(measuredBefore == measuredAfter, L"LED: the same first observation of every frame");
				Assert::AreEqual(moduleBefore.LookaheadDepth(), moduleAfter.LookaheadDepth(), L"LED: the same depth");
				for (const auto& m : now.measurements)
					Assert::AreEqual(1, m.second, L"LED: each frame measured once");
				char line[160];
				snprintf(line, sizeof(line), "LED walk, %d frames, queue %zu: %d measurements before, %d after\n",
					frames, queued, old.total, now.total);
				Logger::WriteMessage(line);
			}
		}

		// Report only: what one saved active-picture measurement costs on this CPU (the render
		// loop's thread), at 4K and 1080p, letterboxed like a scope film.
		TEST_METHOD(ActivePictureMeasurementCpuCost)
		{
			for (const auto size : { std::make_pair(3840, 2160), std::make_pair(1920, 1080) })
			{
				P010Picture picture(size.first, size.second);
				picture.Paint(3);
				const AnalysisLumaSource source = picture.Source();
				(void)ExtractActivePictureEvidence(source);   // warm-up
				const int runs = 10;
				const auto start = std::chrono::steady_clock::now();
				int available = 0;
				for (int i = 0; i < runs; ++i)
					available += ExtractActivePictureEvidence(source).available ? 1 : 0;
				const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / runs;
				char line[200];
				snprintf(line, sizeof(line), "ExtractActivePictureEvidence %dx%d: %.3f ms per frame (%d/%d available); "
					"saved per source frame with a 5-frame queue: about %.3f ms\n",
					size.first, size.second, ms, available, runs, 4.0 * ms);
				Logger::WriteMessage(line);
				Assert::IsTrue(ms >= 0.0);
			}
		}
	};
}
