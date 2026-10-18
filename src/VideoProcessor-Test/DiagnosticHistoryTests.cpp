#include "pch.h"
#include "CppUnitTest.h"

#include <DiagnosticHistory.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// Diagnostic history (report-a-problem, owner 2026-10-17): the seqlock rings
// and the JSONL writer behind the state report.
namespace
{
	namespace DH = DiagnosticHistory;

	// Rings and the recorder are cache-line aligned and large: allocate them
	// aligned on the heap, never on the stack.
	template <typename T>
	struct AlignedDeleter
	{
		void operator()(T* object) const
		{
			if (object)
			{
				object->~T();
				_aligned_free(object);
			}
		}
	};
	template <typename T>
	using AlignedPtr = std::unique_ptr<T, AlignedDeleter<T>>;

	template <typename T>
	AlignedPtr<T> MakeAligned()
	{
		void* memory = _aligned_malloc(sizeof(T), 64);
		if (!memory)
			throw std::bad_alloc();
		return AlignedPtr<T>(new (memory) T());
	}

	DH::EventRecord MakeEventRecord(int64_t value)
	{
		DH::EventRecord record;
		record.qpc = value + 1;
		record.a = value;
		record.b = value * 2;
		record.x = static_cast<double>(value) * 0.5;
		record.y = -static_cast<double>(value);
		std::snprintf(record.name, sizeof(record.name), "event-%lld", static_cast<long long>(value));
		std::snprintf(record.detail, sizeof(record.detail), "detail %lld", static_cast<long long>(value));
		return record;
	}

	DH::FrameRecord MakeFrameRecord(uint64_t v)
	{
		DH::FrameRecord record;
		record.qpc = static_cast<int64_t>(v + 1);
		record.counter = v;
		record.sourceTimestamp = static_cast<int64_t>(v * 3);
		record.captureTimestamp = static_cast<int64_t>(~v);
		record.width = static_cast<uint32_t>(v);
		record.height = static_cast<uint32_t>(~v);
		record.droppedTotal = v ^ 0x5555555555555555ull;
		record.repeatedTotal = v * 7;
		return record;
	}

	bool FrameConsistent(const DH::FrameRecord& r)
	{
		const uint64_t v = r.counter;
		return r.qpc == static_cast<int64_t>(v + 1) &&
			r.sourceTimestamp == static_cast<int64_t>(v * 3) &&
			r.captureTimestamp == static_cast<int64_t>(~v) &&
			r.width == static_cast<uint32_t>(v) &&
			r.height == static_cast<uint32_t>(~v) &&
			r.droppedTotal == (v ^ 0x5555555555555555ull) &&
			r.repeatedTotal == v * 7;
	}

	std::vector<std::string> Lines(const std::string& text)
	{
		std::vector<std::string> lines;
		size_t start = 0;
		while (start < text.size())
		{
			size_t end = text.find('\n', start);
			if (end == std::string::npos)
				end = text.size();
			lines.push_back(text.substr(start, end - start));
			start = end + 1;
		}
		return lines;
	}

	bool StartsWith(const std::string& text, const std::string& prefix)
	{
		return text.compare(0, prefix.size(), prefix) == 0;
	}

	bool Contains(const std::string& text, const std::string& part)
	{
		return text.find(part) != std::string::npos;
	}
}

namespace VideoProcessorTest
{
	TEST_CLASS(DiagnosticHistoryTests)
	{
	public:
		TEST_METHOD(WraparoundKeepsTheNewestRecordsOldestFirst)
		{
			DH::SeqlockRing<DH::EventRecord, 8> ring;
			for (int64_t value = 0; value < 20; ++value)
				ring.Push(MakeEventRecord(value));
			Assert::AreEqual(static_cast<uint64_t>(20), ring.Written());

			DH::RingCounters counters;
			const auto entries = ring.Snapshot(&counters);
			Assert::AreEqual(static_cast<size_t>(8), entries.size());
			Assert::AreEqual(static_cast<uint64_t>(20), counters.written);
			Assert::AreEqual(static_cast<uint64_t>(8), counters.returned);
			Assert::AreEqual(static_cast<uint64_t>(0), counters.skipped);
			for (size_t i = 0; i < entries.size(); ++i)
			{
				const uint64_t expected = 12 + i;
				Assert::AreEqual(expected, entries[i].index);
				const DH::EventRecord want = MakeEventRecord(static_cast<int64_t>(expected));
				Assert::IsTrue(std::memcmp(&entries[i].record, &want, sizeof(want)) == 0,
					L"record content matches what was pushed");
			}

			DH::EventRecord out;
			Assert::IsFalse(ring.TryRead(0, out), L"overwritten index");
			Assert::IsFalse(ring.TryRead(11, out), L"overwritten index");
			Assert::IsTrue(ring.TryRead(12, out));
			Assert::IsTrue(ring.TryRead(19, out));
			Assert::IsFalse(ring.TryRead(20, out), L"not written yet");
		}

		TEST_METHOD(MaxRecordsLimitsTheSnapshot)
		{
			DH::SeqlockRing<DH::EventRecord, 8> ring;
			for (int64_t value = 0; value < 6; ++value)
				ring.Push(MakeEventRecord(value));
			const auto entries = ring.Snapshot(nullptr, 3);
			Assert::AreEqual(static_cast<size_t>(3), entries.size());
			Assert::AreEqual(static_cast<uint64_t>(3), entries.front().index);
			Assert::AreEqual(static_cast<uint64_t>(5), entries.back().index);
		}

		TEST_METHOD(EmptyRingSnapshotIsEmpty)
		{
			DH::SeqlockRing<DH::EventRecord, 8> ring;
			DH::RingCounters counters;
			counters.written = 99;
			counters.returned = 99;
			counters.skipped = 99;
			Assert::IsTrue(ring.Snapshot(&counters).empty());
			Assert::AreEqual(static_cast<uint64_t>(0), counters.written);
			Assert::AreEqual(static_cast<uint64_t>(0), counters.returned);
			Assert::AreEqual(static_cast<uint64_t>(0), counters.skipped);
			Assert::AreEqual(static_cast<uint64_t>(0), ring.Written());
			DH::EventRecord out;
			Assert::IsFalse(ring.TryRead(0, out));
		}

		TEST_METHOD(ReadBackIsBitIdenticalAndWrongIndexFails)
		{
			DH::SeqlockRing<DH::FrameRecord, 8> ring;
			DH::FrameRecord pushed = MakeFrameRecord(0x123456789abcdefull);
			pushed.renderMs = 1.25f;
			pushed.holdReason = 17;
			pushed.phase = -3;
			ring.Push(pushed);
			DH::FrameRecord out;
			Assert::IsTrue(ring.TryRead(0, out));
			Assert::IsTrue(std::memcmp(&pushed, &out, sizeof(pushed)) == 0);
			Assert::IsFalse(ring.TryRead(0 + ring.capacity(), out), L"same slot, wrong lap");
			Assert::IsFalse(ring.TryRead(1, out), L"empty slot");
		}

		TEST_METHOD(ConcurrentWritersNeverYieldATornRecord)
		{
			constexpr size_t kWriters = 4;
			constexpr uint64_t kPerWriter = 200000;
			AlignedPtr<DH::SeqlockRing<DH::FrameRecord, 4096>> ring =
				MakeAligned<DH::SeqlockRing<DH::FrameRecord, 4096>>();
			std::atomic<bool> start(false);
			std::atomic<size_t> finished(0);

			std::vector<std::thread> writers;
			for (size_t writer = 0; writer < kWriters; ++writer)
				writers.emplace_back([&, writer]
				{
					while (!start.load())
						std::this_thread::yield();
					for (uint64_t i = 0; i < kPerWriter; ++i)
						ring->Push(MakeFrameRecord((static_cast<uint64_t>(writer + 1) << 40) | (i + 1)));
					finished.fetch_add(1);
				});

			uint64_t torn = 0;
			uint64_t unordered = 0;
			uint64_t snapshots = 0;
			uint64_t accepted = 0;
			std::thread reader([&]
			{
				while (!start.load())
					std::this_thread::yield();
				while (finished.load() < kWriters)
				{
					const auto entries = ring->Snapshot();
					++snapshots;
					uint64_t previous = 0;
					bool first = true;
					for (const auto& entry : entries)
					{
						++accepted;
						if (!FrameConsistent(entry.record))
							++torn;
						if (!first && entry.index <= previous)
							++unordered;
						previous = entry.index;
						first = false;
					}
				}
			});
			start.store(true);
			for (auto& writer : writers)
				writer.join();
			reader.join();

			Assert::AreEqual(static_cast<uint64_t>(0), torn, L"torn records accepted");
			Assert::AreEqual(static_cast<uint64_t>(0), unordered, L"indices strictly increasing");
			Assert::IsTrue(snapshots > 0 && accepted > 0, L"the reader ran");
			Assert::AreEqual(static_cast<uint64_t>(kWriters) * kPerWriter, ring->Written());

			// After the writers stop, the final snapshot is complete and consistent.
			DH::RingCounters counters;
			const auto last = ring->Snapshot(&counters);
			Assert::AreEqual(static_cast<size_t>(4096), last.size());
			for (const auto& entry : last)
				Assert::IsTrue(FrameConsistent(entry.record));
		}

		TEST_METHOD(DecisionTruncatesAndCleansText)
		{
			AlignedPtr<DH::Recorder> recorder = MakeAligned<DH::Recorder>();
			const std::string longName(40, 'n');
			const std::string longDetail(100, 'd');
			recorder->RecordDecision(longName.c_str(), longDetail.c_str(), 5, 6, 1.5, 2.5);
			recorder->RecordDecision("a\tb\nc", "x\ry\x01z");
			recorder->RecordDecision(nullptr, nullptr);

			const auto entries = recorder->decisions.Snapshot();
			Assert::AreEqual(static_cast<size_t>(3), entries.size());
			const DH::EventRecord& first = entries[0].record;
			Assert::AreEqual(static_cast<size_t>(23), std::strlen(first.name));
			Assert::AreEqual(static_cast<size_t>(55), std::strlen(first.detail));
			Assert::AreEqual(static_cast<int64_t>(5), first.a);
			Assert::AreEqual(static_cast<int64_t>(6), first.b);
			Assert::AreEqual(1.5, first.x);
			Assert::AreEqual(2.5, first.y);
			Assert::IsTrue(first.qpc > 0, L"stamped with the QPC");
			Assert::AreEqual("a b c", entries[1].record.name);
			Assert::AreEqual("x y z", entries[1].record.detail);
			Assert::AreEqual(static_cast<size_t>(0), std::strlen(entries[2].record.name));
		}

		// Bug hunt 3 (C8): a cut never splits a UTF-8 character.
		TEST_METHOD(TruncationNeverSplitsAUtf8Character)
		{
			char out[8];
			// 6 ASCII + a 2-byte character: only 7 bytes fit; the character goes whole.
			DH::CopyText(out, sizeof(out), "abcdef\xC3\xA9xyz");
			Assert::AreEqual("abcdef", out);
			// A 3-byte character cut after one or two of its bytes.
			DH::CopyText(out, sizeof(out), "abcde\xE2\x82\xAC!");
			Assert::AreEqual("abcde", out);
			DH::CopyText(out, sizeof(out), "abcd\xE2\x82\xAC!");
			Assert::AreEqual("abcd\xE2\x82\xAC", out, L"a whole character that fits is kept");
			// A 4-byte character cut.
			DH::CopyText(out, sizeof(out), "abcd\xF0\x9F\x8E\xA5");
			Assert::AreEqual("abcd", out);
			// No cut: unchanged.
			DH::CopyText(out, sizeof(out), "ab\xC3\xA9");
			Assert::AreEqual("ab\xC3\xA9", out);
		}

		TEST_METHOD(FrameKeepsAGivenQpcAndStampsZero)
		{
			AlignedPtr<DH::Recorder> recorder = MakeAligned<DH::Recorder>();
			DH::FrameRecord given;
			given.qpc = 12345;
			given.counter = 1;
			recorder->RecordFrame(given);
			DH::FrameRecord unstamped;
			unstamped.counter = 2;
			const int64_t before = DH::NowQpc();
			recorder->RecordFrame(unstamped);
			const int64_t after = DH::NowQpc();
			const auto entries = recorder->frames.Snapshot();
			Assert::AreEqual(static_cast<size_t>(2), entries.size());
			Assert::AreEqual(static_cast<int64_t>(12345), entries[0].record.qpc);
			Assert::IsTrue(entries[1].record.qpc >= before && entries[1].record.qpc <= after);
		}

		TEST_METHOD(JsonlRowsAreWellFormedLines)
		{
			AlignedPtr<DH::Recorder> recorder = MakeAligned<DH::Recorder>();
			DH::FrameRecord frame;
			frame.counter = 7;
			frame.stage = DH::StagePresented;
			frame.queueDepth = 3;
			frame.renderMs = 2.5f;
			frame.width = 3840;
			frame.height = 2160;
			recorder->RecordFrame(frame);
			frame.stage = DH::StageDropped;
			recorder->RecordFrame(frame);
			recorder->RecordDecision("reset-request", "detail with \"quotes\" and \\ slash", 4);
			recorder->RecordLed("led-set", "level", 12);
			recorder->RecordCapture("signal", "locked");
			DH::StatsRecord stats;
			stats.values[0] = 23.976;
			stats.values[2] = 48;
			recorder->RecordStats(stats);

			const std::string text = DH::ToJsonl(*recorder, "exe", DH::NowQpc());
			Assert::IsFalse(text.empty());
			Assert::AreEqual('\n', text.back());
			size_t meta = 0;
			bool sawPresented = false, sawDropped = false, sawEscaped = false;
			for (const std::string& line : Lines(text))
			{
				Assert::IsTrue(StartsWith(line, "{\"src\":\"exe\""), L"row starts with src exe");
				Assert::AreEqual('}', line.back());
				if (Contains(line, "\"ring\":\"ring_meta\""))
					++meta;
				if (Contains(line, "\"stage\":\"presented\""))
					sawPresented = true;
				if (Contains(line, "\"stage\":\"dropped\""))
					sawDropped = true;
				if (Contains(line, "\"detail\":\"detail with \\\"quotes\\\" and \\\\ slash\""))
					sawEscaped = true;
			}
			Assert::AreEqual(static_cast<size_t>(5), meta);
			Assert::IsTrue(sawPresented && sawDropped, L"frame stages are named");
			Assert::IsTrue(sawEscaped, L"quote and backslash are escaped");
			Assert::IsTrue(Contains(text, "\"name\":\"frame\",\"capacity\":4096,\"written\":2"));
			Assert::IsTrue(Contains(text, "\"source_hz\":23.976"));
			Assert::IsTrue(Contains(text, "\"w\":3840,\"h\":2160"));
		}

		TEST_METHOD(SinkOutputEqualsToJsonl)
		{
			AlignedPtr<DH::Recorder> recorder = MakeAligned<DH::Recorder>();
			// Enough rows that the sink needs several 64 KB chunks.
			for (uint64_t i = 0; i < 3000; ++i)
			{
				DH::FrameRecord frame = MakeFrameRecord(i + 1);
				frame.stage = DH::StageRendered;
				frame.renderMs = 1.5f;
				recorder->RecordFrame(frame);
			}
			recorder->RecordDecision("x", "y");
			const int64_t reference = DH::NowQpc();
			const std::string direct = DH::ToJsonl(*recorder, "renderer", reference);
			Assert::IsTrue(direct.size() > 128 * 1024, L"several chunks");
			std::string viaSink;
			Assert::IsTrue(DH::WriteJsonlToSink(*recorder, "renderer", reference,
				&DH::AppendToStringSink, &viaSink));
			Assert::IsTrue(direct == viaSink);
			Assert::IsFalse(DH::WriteJsonlToSink(*recorder, "renderer", reference, nullptr, nullptr));
		}

		TEST_METHOD(MemoryBudget)
		{
			Assert::IsTrue(DH::kTotalBytes >= 700u * 1024u);
			Assert::IsTrue(DH::kTotalBytes <= 800u * 1024u);
			Assert::AreEqual(static_cast<size_t>(128), sizeof(DH::Slot));
		}

		TEST_METHOD(RecordCostIsBounded)
		{
			DH::FrameRecord record;
			record.counter = 1;
			record.stage = DH::StagePresented;
			record.queueDepth = 4;
			record.renderMs = 3.0f;
			record.width = 3840;
			record.height = 2160;
			record.qpc = 0; // includes the QueryPerformanceCounter stamp

			for (int i = 0; i < 100000; ++i)
				DH::Global().RecordFrame(record);

			LARGE_INTEGER frequency;
			QueryPerformanceFrequency(&frequency);
			constexpr int kCalls = 1000000;
			double best = 1e30;
			for (int run = 0; run < 3; ++run)
			{
				LARGE_INTEGER begin, end;
				QueryPerformanceCounter(&begin);
				for (int i = 0; i < kCalls; ++i)
				{
					record.counter = static_cast<uint64_t>(i);
					DH::Global().RecordFrame(record);
				}
				QueryPerformanceCounter(&end);
				const double ns = static_cast<double>(end.QuadPart - begin.QuadPart) * 1e9 /
					static_cast<double>(frequency.QuadPart) / kCalls;
				if (ns < best)
					best = ns;
			}
			char message[128];
			std::snprintf(message, sizeof(message), "DiagnosticHistory RecordFrame: %.1f ns per frame", best);
			Logger::WriteMessage(message);
			Assert::IsTrue(best <= 100.0, L"RecordFrame costs at most 100 ns");
		}
	};
}
