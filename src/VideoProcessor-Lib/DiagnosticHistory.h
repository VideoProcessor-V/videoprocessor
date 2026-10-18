#pragma once

// Diagnostic history (report-a-problem, owner 2026-10-17): the last few
// seconds of VP's state, kept in memory so the state_report hotkey can write
// it into a report bundle (StateReport.h) for problems that do not crash VP.
//
// Five lock-free seqlock rings, each slot one 128-byte cache-line pair:
//   frames    4096 x 128 B  (512 KB)  one record per frame per stage
//   decisions 1024 x 128 B  (128 KB)  holds, resets, profile and mode choices
//   led        256 x 128 B  ( 32 KB)  Dynamic Drive (RS-232) commands
//   capture    256 x 128 B  ( 32 KB)  signal / video-state / ingress events
//   stats      256 x 128 B  ( 32 KB)  one row per second (about 4 minutes)
// Total 736 KB per module. The executable and the renderer DLL each have
// their own instance (Global() is per module); the renderer contributes its
// rows through IVideoRenderer::WriteDiagnosticHistory and the C sink below.
//
// Writers never block and never allocate: a writer claims an index with one
// atomic add, marks the slot odd, stores the payload as relaxed 64-bit words
// and marks it even with release order. A reader copies a slot and accepts it
// only when the sequence was the expected even value before and after the
// copy (Boehm's seqlock with fences), so it never returns a torn record. A
// slot being written while the snapshot runs, or overwritten by a writer that
// lapped the reader, is skipped and counted. Cost: about 20-40 ns per record
// on the laptop (DiagnosticHistoryTests::RecordCostIsBounded measures it).
//
// C++14: this header is used by the Lib, the GUI, the renderer DLL and tests.

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

// The C sink a module writes its JSONL rows into (renderer DLL -> executable).
// `data` is `length` bytes of UTF-8, one or more complete lines ending in '\n'.
// Plain C types only: it crosses the plugin boundary.
extern "C" typedef void(__cdecl* VpDiagnosticHistorySink)(
	void* context, const char* data, size_t length);

namespace DiagnosticHistory
{
	constexpr size_t kPayloadBytes = 120;
	constexpr size_t kPayloadWords = kPayloadBytes / sizeof(uint64_t);

	constexpr size_t kFrameCapacity = 4096;
	constexpr size_t kDecisionCapacity = 1024;
	constexpr size_t kLedCapacity = 256;
	constexpr size_t kCaptureCapacity = 256;
	constexpr size_t kStatsCapacity = 256;

	// Frame stages. One frame may produce several records (capture ingress in
	// the executable, ingest/render/present in the renderer).
	enum FrameStage : uint32_t
	{
		StageCaptureIngress = 0, // executable: frame accepted from the card
		StageCaptureRejected = 1,// executable: ingress refused (state change)
		StageRendererIngest = 2, // renderer: frame queued for rendering
		StageRendered = 3,       // renderer: frame rendered (GPU submit)
		StagePresented = 4,      // renderer: frame presented
		StageDropped = 5,        // renderer: frame dropped
		StageRepeated = 6,       // renderer: frame repeated / held
	};

	// FrameRecord::flags.
	enum FrameFlags : uint32_t
	{
		FlagInterpolated = 1u << 0,
		FlagRenderAhead = 1u << 1,
		FlagOpenLoop = 1u << 2,
		FlagLate = 1u << 3,
		FlagSceneCut = 1u << 4,
		FlagVectorsValid = 1u << 5,
		FlagHdr = 1u << 6,
		FlagDiscontinuity = 1u << 7,
	};

	// 120 bytes. Unused fields stay 0. Times: qpc and presentQpc are
	// QueryPerformanceCounter ticks (the same clock in the executable and the
	// renderer DLL); source/capture timestamps are the capture card's 100 ns
	// units as VideoFrame carries them.
	struct FrameRecord
	{
		int64_t qpc = 0;               // filled by RecordFrame when 0
		uint64_t counter = 0;          // capture frame counter
		int64_t sourceTimestamp = 0;   // VideoFrame timing timestamp
		int64_t captureTimestamp = 0;  // VideoFrame capture timestamp
		int64_t presentQpc = 0;        // target / actual present time
		uint32_t stage = 0;            // FrameStage
		uint32_t flags = 0;            // FrameFlags
		uint32_t queueDepth = 0;       // frames queued at this stage
		uint32_t holdReason = 0;       // renderer-defined, 0 = none: the code table is
		                               // vprenderer/RendererDiagnosticCodes.h
		float renderMs = 0.0f;
		float uploadMs = 0.0f;
		float waitMs = 0.0f;
		float intervalMs = 0.0f;       // since the previous record of this stage
		float leadMs = 0.0f;           // presentation lead
		float latencyMs = 0.0f;        // capture -> this stage
		int32_t phase = 0;             // cadence phase / render-ahead slot
		int32_t repeatCount = 0;
		uint32_t width = 0;
		uint32_t height = 0;
		uint64_t droppedTotal = 0;
		uint64_t repeatedTotal = 0;
		uint32_t rendererGeneration = 0;
		uint32_t reserved = 0;
	};

	// 120 bytes. A named event: decisions, LED commands and capture events.
	struct EventRecord
	{
		int64_t qpc = 0;               // filled when 0
		int64_t a = 0;
		int64_t b = 0;
		double x = 0.0;
		double y = 0.0;
		char name[24] = {};            // short machine name, e.g. "reset-request"
		char detail[56] = {};          // short human detail (no paths, no user data)
	};

	// 120 bytes. One row per second; field names are kStatsFieldNames.
	constexpr size_t kStatsFields = 14;
	struct StatsRecord
	{
		int64_t qpc = 0;
		double values[kStatsFields] = {};
	};

	inline const char* const* StatsFieldNames()
	{
		static const char* const names[kStatsFields] = {
			"source_hz", "display_hz", "queue_frames", "entry_latency_ms",
			"exit_latency_ms", "dropped_total", "repeated_total", "cpu_percent",
			"render_load_percent", "presented_fps", "captured_fps",
			"rejected_total", "renderer_generation", "signal" };
		return names;
	}

	static_assert(sizeof(FrameRecord) == kPayloadBytes, "FrameRecord must be 120 bytes");
	static_assert(sizeof(EventRecord) == kPayloadBytes, "EventRecord must be 120 bytes");
	static_assert(sizeof(StatsRecord) == kPayloadBytes, "StatsRecord must be 120 bytes");

	inline int64_t NowQpc() noexcept
	{
		LARGE_INTEGER value;
		QueryPerformanceCounter(&value);
		return value.QuadPart;
	}

	inline int64_t QpcFrequency() noexcept
	{
		static const int64_t frequency = []
		{
			LARGE_INTEGER value;
			QueryPerformanceFrequency(&value);
			return value.QuadPart > 0 ? value.QuadPart : 1;
		}();
		return frequency;
	}

	// Copies at most size-1 bytes and terminates. Replaces control characters
	// so a record never breaks a JSON line.
	inline void CopyText(char* destination, size_t size, const char* source) noexcept
	{
		size_t index = 0;
		if (source)
			for (; index + 1 < size && source[index]; ++index)
			{
				const unsigned char c = static_cast<unsigned char>(source[index]);
				destination[index] = c < 0x20 ? ' ' : static_cast<char>(c);
			}
		for (; index < size; ++index)
			destination[index] = '\0';
	}

	struct alignas(64) Slot
	{
		std::atomic<uint64_t> sequence;            // 0 = never written
		std::atomic<uint64_t> words[kPayloadWords];
	};
	static_assert(sizeof(Slot) == 128, "a slot is two cache lines");

	struct RingCounters
	{
		uint64_t written = 0;   // records ever claimed
		uint64_t returned = 0;  // records in the snapshot
		uint64_t skipped = 0;   // in flight or overwritten during the copy
	};

	template <typename Record, size_t Capacity>
	class SeqlockRing
	{
		static_assert((Capacity & (Capacity - 1)) == 0, "capacity must be a power of two");
		static_assert(std::is_trivially_copyable<Record>::value, "records are copied as words");
		static_assert(sizeof(Record) == kPayloadBytes, "records are 120 bytes");

	public:
		struct Entry
		{
			uint64_t index;
			Record record;
		};

		SeqlockRing() noexcept
		{
			head.store(0, std::memory_order_relaxed);
			for (Slot& slot : slots)
			{
				slot.sequence.store(0, std::memory_order_relaxed);
				for (auto& word : slot.words)
					word.store(0, std::memory_order_relaxed);
			}
		}
		SeqlockRing(const SeqlockRing&) = delete;
		SeqlockRing& operator=(const SeqlockRing&) = delete;

		static constexpr size_t capacity() { return Capacity; }

		// Lock-free, wait-free for the writer; any number of writers. A writer
		// lapped by Capacity others while it writes one slot can collide with
		// them; the reader then rejects the slot (sequence mismatch) unless the
		// slower writer finishes last, which needs Capacity writes during one
		// 16-store write - not a case VP's rates reach.
		void Push(const Record& record) noexcept
		{
			const uint64_t index = head.fetch_add(1, std::memory_order_relaxed);
			Slot& slot = slots[index & (Capacity - 1)];
			slot.sequence.store(index * 2 + 1, std::memory_order_relaxed);
			std::atomic_thread_fence(std::memory_order_release);
			uint64_t words[kPayloadWords];
			std::memcpy(words, &record, kPayloadBytes);
			for (size_t word = 0; word < kPayloadWords; ++word)
				slot.words[word].store(words[word], std::memory_order_relaxed);
			slot.sequence.store(index * 2 + 2, std::memory_order_release);
		}

		uint64_t Written() const noexcept { return head.load(std::memory_order_acquire); }

		// Reads one record by absolute index. False when it is not (or no
		// longer, or not yet completely) that record.
		bool TryRead(uint64_t index, Record& out) const noexcept
		{
			const Slot& slot = slots[index & (Capacity - 1)];
			const uint64_t expected = index * 2 + 2;
			const uint64_t before = slot.sequence.load(std::memory_order_acquire);
			if (before != expected)
				return false;
			uint64_t words[kPayloadWords];
			for (size_t word = 0; word < kPayloadWords; ++word)
				words[word] = slot.words[word].load(std::memory_order_relaxed);
			std::atomic_thread_fence(std::memory_order_acquire);
			const uint64_t after = slot.sequence.load(std::memory_order_relaxed);
			if (after != before)
				return false;
			std::memcpy(&out, words, kPayloadBytes);
			return true;
		}

		// The newest records (at most `maxRecords`), oldest first.
		std::vector<Entry> Snapshot(RingCounters* counters = nullptr,
			size_t maxRecords = Capacity) const
		{
			const uint64_t end = head.load(std::memory_order_acquire);
			const uint64_t span = maxRecords < Capacity ? maxRecords : Capacity;
			const uint64_t begin = end > span ? end - span : 0;
			std::vector<Entry> entries;
			entries.reserve(static_cast<size_t>(end - begin));
			uint64_t skipped = 0;
			for (uint64_t index = begin; index < end; ++index)
			{
				Entry entry;
				entry.index = index;
				if (TryRead(index, entry.record))
					entries.push_back(entry);
				else
					++skipped;
			}
			if (counters)
			{
				counters->written = end;
				counters->returned = entries.size();
				counters->skipped = skipped;
			}
			return entries;
		}

	private:
		alignas(64) std::atomic<uint64_t> head;
		Slot slots[Capacity];
	};

	class Recorder
	{
	public:
		Recorder() noexcept = default;
		Recorder(const Recorder&) = delete;
		Recorder& operator=(const Recorder&) = delete;

		void RecordFrame(const FrameRecord& record) noexcept
		{
			if (record.qpc != 0)
			{
				frames.Push(record);
				return;
			}
			FrameRecord stamped = record;
			stamped.qpc = NowQpc();
			frames.Push(stamped);
		}

		void RecordDecision(const char* name, const char* detail = nullptr,
			int64_t a = 0, int64_t b = 0, double x = 0.0, double y = 0.0) noexcept
		{
			decisions.Push(MakeEvent(name, detail, a, b, x, y));
		}
		void RecordLed(const char* name, const char* detail = nullptr,
			int64_t a = 0, int64_t b = 0, double x = 0.0, double y = 0.0) noexcept
		{
			led.Push(MakeEvent(name, detail, a, b, x, y));
		}
		void RecordCapture(const char* name, const char* detail = nullptr,
			int64_t a = 0, int64_t b = 0, double x = 0.0, double y = 0.0) noexcept
		{
			capture.Push(MakeEvent(name, detail, a, b, x, y));
		}
		void RecordStats(const StatsRecord& record) noexcept
		{
			StatsRecord stamped = record;
			if (stamped.qpc == 0)
				stamped.qpc = NowQpc();
			stats.Push(stamped);
		}

		static EventRecord MakeEvent(const char* name, const char* detail,
			int64_t a, int64_t b, double x, double y) noexcept
		{
			EventRecord event;
			event.qpc = NowQpc();
			event.a = a;
			event.b = b;
			event.x = x;
			event.y = y;
			CopyText(event.name, sizeof(event.name), name);
			CopyText(event.detail, sizeof(event.detail), detail);
			return event;
		}

		SeqlockRing<FrameRecord, kFrameCapacity> frames;
		SeqlockRing<EventRecord, kDecisionCapacity> decisions;
		SeqlockRing<EventRecord, kLedCapacity> led;
		SeqlockRing<EventRecord, kCaptureCapacity> capture;
		SeqlockRing<StatsRecord, kStatsCapacity> stats;
	};

	constexpr size_t kTotalBytes =
		(kFrameCapacity + kDecisionCapacity + kLedCapacity + kCaptureCapacity + kStatsCapacity) *
		sizeof(Slot);

	// The module's recorder (one in the executable, one in the renderer DLL).
	inline Recorder& Global() noexcept
	{
		static Recorder instance;
		return instance;
	}

	// ---- JSONL ---------------------------------------------------------------
	//
	// One JSON object per line. Every row has "src" (exe / renderer), "ring"
	// and "t_ms": milliseconds relative to the report moment (negative = before
	// the hotkey). A "ring_meta" row per ring gives capacity and counters.

	namespace Detail
	{
		inline void AppendEscaped(std::string& out, const char* text, size_t maxLength)
		{
			out.push_back('"');
			for (size_t index = 0; index < maxLength && text[index]; ++index)
			{
				const char c = text[index];
				if (c == '"' || c == '\\') { out.push_back('\\'); out.push_back(c); }
				else if (static_cast<unsigned char>(c) < 0x20) out.push_back(' ');
				else out.push_back(c);
			}
			out.push_back('"');
		}

		inline void AppendNumber(std::string& out, double value)
		{
			char buffer[40];
			if (value != value || value > 1e300 || value < -1e300)
			{
				out += "null";
				return;
			}
			std::snprintf(buffer, sizeof(buffer), "%.6g", value);
			out += buffer;
		}

		inline void AppendInteger(std::string& out, long long value)
		{
			char buffer[32];
			std::snprintf(buffer, sizeof(buffer), "%lld", value);
			out += buffer;
		}

		inline void AppendUnsigned(std::string& out, unsigned long long value)
		{
			char buffer[32];
			std::snprintf(buffer, sizeof(buffer), "%llu", value);
			out += buffer;
		}

		inline double RelativeMs(int64_t qpc, int64_t referenceQpc)
		{
			if (qpc == 0)
				return 0.0;
			return static_cast<double>(qpc - referenceQpc) * 1000.0 /
				static_cast<double>(QpcFrequency());
		}

		inline void BeginRow(std::string& out, const char* source, const char* ring,
			uint64_t index, int64_t qpc, int64_t referenceQpc)
		{
			out += "{\"src\":\"";
			out += source;
			out += "\",\"ring\":\"";
			out += ring;
			out += "\",\"i\":";
			AppendUnsigned(out, index);
			out += ",\"t_ms\":";
			char buffer[40];
			std::snprintf(buffer, sizeof(buffer), "%.3f", RelativeMs(qpc, referenceQpc));
			out += buffer;
		}

		inline void AppendMeta(std::string& out, const char* source, const char* ring,
			size_t capacity, const RingCounters& counters)
		{
			out += "{\"src\":\"";
			out += source;
			out += "\",\"ring\":\"ring_meta\",\"name\":\"";
			out += ring;
			out += "\",\"capacity\":";
			AppendUnsigned(out, capacity);
			out += ",\"written\":";
			AppendUnsigned(out, counters.written);
			out += ",\"returned\":";
			AppendUnsigned(out, counters.returned);
			out += ",\"skipped\":";
			AppendUnsigned(out, counters.skipped);
			out += "}\n";
		}

		inline void AppendFrame(std::string& out, const char* source, uint64_t index,
			const FrameRecord& r, int64_t referenceQpc)
		{
			BeginRow(out, source, "frame", index, r.qpc, referenceQpc);
			static const char* const stages[] = { "capture_ingress", "capture_rejected",
				"renderer_ingest", "rendered", "presented", "dropped", "repeated" };
			out += ",\"stage\":\"";
			out += r.stage < 7 ? stages[r.stage] : "unknown";
			out += "\",\"counter\":";
			AppendUnsigned(out, r.counter);
			if (r.sourceTimestamp) { out += ",\"source_ts\":"; AppendInteger(out, r.sourceTimestamp); }
			if (r.captureTimestamp) { out += ",\"capture_ts\":"; AppendInteger(out, r.captureTimestamp); }
			if (r.presentQpc)
			{
				out += ",\"present_t_ms\":";
				char buffer[40];
				std::snprintf(buffer, sizeof(buffer), "%.3f", RelativeMs(r.presentQpc, referenceQpc));
				out += buffer;
			}
			if (r.flags) { out += ",\"flags\":"; AppendUnsigned(out, r.flags); }
			out += ",\"queue\":";
			AppendUnsigned(out, r.queueDepth);
			if (r.holdReason) { out += ",\"hold\":"; AppendUnsigned(out, r.holdReason); }
			const struct { const char* name; float value; } floats[] = {
				{ "render_ms", r.renderMs }, { "upload_ms", r.uploadMs }, { "wait_ms", r.waitMs },
				{ "interval_ms", r.intervalMs }, { "lead_ms", r.leadMs }, { "latency_ms", r.latencyMs } };
			for (const auto& field : floats)
				if (field.value != 0.0f)
				{
					out += ",\"";
					out += field.name;
					out += "\":";
					AppendNumber(out, field.value);
				}
			if (r.phase) { out += ",\"phase\":"; AppendInteger(out, r.phase); }
			if (r.repeatCount) { out += ",\"repeat\":"; AppendInteger(out, r.repeatCount); }
			if (r.width || r.height)
			{
				out += ",\"w\":"; AppendUnsigned(out, r.width);
				out += ",\"h\":"; AppendUnsigned(out, r.height);
			}
			if (r.droppedTotal) { out += ",\"dropped_total\":"; AppendUnsigned(out, r.droppedTotal); }
			if (r.repeatedTotal) { out += ",\"repeated_total\":"; AppendUnsigned(out, r.repeatedTotal); }
			if (r.rendererGeneration) { out += ",\"gen\":"; AppendUnsigned(out, r.rendererGeneration); }
			out += "}\n";
		}

		inline void AppendEvent(std::string& out, const char* source, const char* ring,
			uint64_t index, const EventRecord& r, int64_t referenceQpc)
		{
			BeginRow(out, source, ring, index, r.qpc, referenceQpc);
			out += ",\"name\":";
			AppendEscaped(out, r.name, sizeof(r.name));
			if (r.detail[0]) { out += ",\"detail\":"; AppendEscaped(out, r.detail, sizeof(r.detail)); }
			if (r.a) { out += ",\"a\":"; AppendInteger(out, r.a); }
			if (r.b) { out += ",\"b\":"; AppendInteger(out, r.b); }
			if (r.x != 0.0) { out += ",\"x\":"; AppendNumber(out, r.x); }
			if (r.y != 0.0) { out += ",\"y\":"; AppendNumber(out, r.y); }
			out += "}\n";
		}

		inline void AppendStats(std::string& out, const char* source, uint64_t index,
			const StatsRecord& r, int64_t referenceQpc)
		{
			BeginRow(out, source, "stats", index, r.qpc, referenceQpc);
			const char* const* names = StatsFieldNames();
			for (size_t field = 0; field < kStatsFields; ++field)
			{
				out += ",\"";
				out += names[field];
				out += "\":";
				AppendNumber(out, r.values[field]);
			}
			out += "}\n";
		}
	}

	// Snapshots every ring of `recorder` and returns JSONL rows tagged `source`.
	inline std::string ToJsonl(const Recorder& recorder, const char* source, int64_t referenceQpc)
	{
		std::string out;
		RingCounters counters;
		const auto frames = recorder.frames.Snapshot(&counters);
		const RingCounters frameCounters = counters;
		const auto decisions = recorder.decisions.Snapshot(&counters);
		const RingCounters decisionCounters = counters;
		const auto led = recorder.led.Snapshot(&counters);
		const RingCounters ledCounters = counters;
		const auto capture = recorder.capture.Snapshot(&counters);
		const RingCounters captureCounters = counters;
		const auto stats = recorder.stats.Snapshot(&counters);
		const RingCounters statsCounters = counters;

		out.reserve(frames.size() * 220 + (decisions.size() + led.size() + capture.size()) * 160 +
			stats.size() * 400 + 1024);
		Detail::AppendMeta(out, source, "frame", kFrameCapacity, frameCounters);
		Detail::AppendMeta(out, source, "decision", kDecisionCapacity, decisionCounters);
		Detail::AppendMeta(out, source, "led", kLedCapacity, ledCounters);
		Detail::AppendMeta(out, source, "capture", kCaptureCapacity, captureCounters);
		Detail::AppendMeta(out, source, "stats", kStatsCapacity, statsCounters);
		for (const auto& entry : capture)
			Detail::AppendEvent(out, source, "capture", entry.index, entry.record, referenceQpc);
		for (const auto& entry : decisions)
			Detail::AppendEvent(out, source, "decision", entry.index, entry.record, referenceQpc);
		for (const auto& entry : led)
			Detail::AppendEvent(out, source, "led", entry.index, entry.record, referenceQpc);
		for (const auto& entry : stats)
			Detail::AppendStats(out, source, entry.index, entry.record, referenceQpc);
		for (const auto& entry : frames)
			Detail::AppendFrame(out, source, entry.index, entry.record, referenceQpc);
		return out;
	}

	// Writes the JSONL rows of `recorder` into a C sink in bounded chunks of
	// whole lines. Used by the renderer DLL for WriteDiagnosticHistory.
	inline bool WriteJsonlToSink(const Recorder& recorder, const char* source,
		int64_t referenceQpc, VpDiagnosticHistorySink sink, void* context)
	{
		if (!sink)
			return false;
		const std::string text = ToJsonl(recorder, source, referenceQpc);
		constexpr size_t kChunk = 64 * 1024;
		size_t start = 0;
		while (start < text.size())
		{
			size_t end = start + kChunk < text.size() ? start + kChunk : text.size();
			if (end < text.size())
			{
				const size_t newline = text.rfind('\n', end - 1);
				end = newline != std::string::npos && newline >= start ? newline + 1 : end;
			}
			sink(context, text.data() + start, end - start);
			start = end;
		}
		return true;
	}

	// A sink context appending into a std::string (the executable's side).
	inline void __cdecl AppendToStringSink(void* context, const char* data, size_t length)
	{
		if (context && data && length)
			static_cast<std::string*>(context)->append(data, length);
	}
}
