// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// P14g `source_geometry` (skeleton product id 29; doc 78 section 6, request of the source-identity lead, wave S2): the RESERVATION of the
// held-state source geometry record. This header holds what the core and every reader need before the estimator exists: the unit version,
// the size bound and byte order of the record, the staleness rule, the provisional readers, the ABSENT placeholder producer and the one
// read API with its visible fallback. The record's fields (per axis: measured and applied state, n, N, ratio, phase, kernel family and
// parameters, kernel confidence, sharpening, double scaling, confidence, LLR, reason bits; the active area; the coded-raster class; the
// reserved chroma chain; geometryGen, appliedGen, evalSeq, evalTimeUs, maxAgeUs) are laid out by the source-identity build (SID-C) inside
// kRecordBytesMax: bump kUnitVersion here AND kExpectedSourceGeometryUnit (core/ProductCatalog.h) in the same commit on ANY change of the
// layout or of the law behind a field. The shared accessor `identity/SourceGeometryView.h` (Confident() / Provisional(), type-separated) is
// that build's file; it reads through Read() below.
//
// HELD STATE, like P14 (SourceIdProduct.h): the source-resolution side is the single writer and publishes on each evaluation; a consumer
// reads the latest SEALED record with seq <= N, i.e. a lag >= 1 read of the frame it renders, never a live fill (the estimator is
// hysteretic: recomputing it on the render thread would differ from the writer's answer). A record older than its maximum age is stale.
// Absent / stale / another unit version = the reader keeps the DELIVERED raster and its classical form, visibly (no silent fallback):
//   quality_pattern, coding_evidence   grid-free evidence ("frame pattern: no source geometry")
//   temporal detail                    the delivered raster (levels 3 / 4 of 68v2)
//   adaptive scaler, chroma stage      today's behaviour, byte-identical
//   status                             "Source identity: classical only (<reason>)"
//
// "Requested": the geometry is requested while source_id_consensus is ON (Requested(consensusOn)); Shadow computes and logs the would-be
// and Off asks for nothing: every Read() is then "not requested": no status line, nothing engaged, byte-identical to not reading at all.
// SID-C2a (the source-identity build) fills the record below (Record, Pack / Unpack, HeldState) and the readers use Read() unchanged; the
// ABSENT placeholder stays as the producer of a build without the estimator wired. Pure functions over plain structs (no clock: `nowUs`
// is passed in), header-only, C++14, no Windows.
#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/core/Products.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>

namespace SourceGeometry
{
	// Bump on ANY change of the record layout or of the law behind a field; core/ProductCatalog.h holds the expected number.
	constexpr uint32_t kUnitVersion = 1;

	using VpSkeleton::ProductId;
	constexpr ProductId kProduct = VpSkeleton::kSourceGeometry;

	// The record is a fixed little-endian layout of at most this many bytes (the registry declares exactly this figure, system RAM).
	constexpr size_t kRecordBytesMax = VpSkeleton::kSourceGeometryRecordBytes;
	static_assert(kRecordBytesMax == 512, "doc 78 section 6: the record is at most 512 B");

	// Stale after maxAgeUs = kMaxAgeIntervals x the longest evaluation interval (doc 78 section 6). The interval is MEASURED by the writer
	// (never a fixed rate): the writer states the result in its record, a reader only compares. 0 = the interval is not measured yet (the
	// reader says so, see kCauseNotMeasured); the product saturates instead of overflowing.
	constexpr int64_t kMaxAgeIntervals = 2;
	inline int64_t MaxAgeUs(int64_t evaluationIntervalMaxUs)
	{
		if (evaluationIntervalMaxUs <= 0)
			return 0;
		const int64_t top = (std::numeric_limits<int64_t>::max)();
		return evaluationIntervalMaxUs > top / kMaxAgeIntervals ? top : kMaxAgeIntervals * evaluationIntervalMaxUs;
	}

	// |a - b| in microseconds for ANY two int64 values, without signed overflow (unsigned arithmetic is exact: the difference fits uint64).
	inline uint64_t AbsDiffUs(int64_t a, int64_t b)
	{
		return a >= b ? static_cast<uint64_t>(a) - static_cast<uint64_t>(b) : static_cast<uint64_t>(b) - static_cast<uint64_t>(a);
	}

	// ---- the record header: a fixed 8 B in front of the payload -----------------------------------------------------------------
	//   offset 0  u16  (kMagic << 8) | unit version   (the version fits the low byte)
	//   offset 2  u16  the record length in bytes, header included (kHeaderBytes .. kRecordBytesMax)
	//   offset 4  u32  reserved: written 0, ignored on read (room for flags without a version bump of the header)
	// A reader checks the header BEFORE it trusts a byte of the payload: another magic / unit version / a length beyond the buffer or the
	// 512 B bound is a visible cause, never a guess. The payload is at most kPayloadBytesMax.
	constexpr size_t kHeaderBytes = 8;
	constexpr size_t kPayloadBytesMax = kRecordBytesMax - kHeaderBytes;
	constexpr uint8_t kMagic = 0x47;   // 'G'
	static_assert(kUnitVersion <= 0xFFu, "the unit version lives in the low byte of the header's first u16");

	// ---- the byte order of the record: fixed little-endian, independent of the host --------------------------------------------

	inline void PutU16(unsigned char* at, uint16_t v)
	{
		at[0] = static_cast<unsigned char>(v & 0xFFu);
		at[1] = static_cast<unsigned char>((v >> 8) & 0xFFu);
	}
	inline void PutU32(unsigned char* at, uint32_t v)
	{
		for (unsigned i = 0; i < 4; ++i)
			at[i] = static_cast<unsigned char>((v >> (8u * i)) & 0xFFu);
	}
	inline void PutU64(unsigned char* at, uint64_t v)
	{
		for (unsigned i = 0; i < 8; ++i)
			at[i] = static_cast<unsigned char>((v >> (8u * i)) & 0xFFu);
	}
	inline uint16_t GetU16(const unsigned char* at) { return static_cast<uint16_t>(at[0] | (unsigned(at[1]) << 8)); }
	inline uint32_t GetU32(const unsigned char* at)
	{
		uint32_t v = 0;
		for (unsigned i = 0; i < 4; ++i)
			v |= static_cast<uint32_t>(at[i]) << (8u * i);
		return v;
	}
	inline uint64_t GetU64(const unsigned char* at)
	{
		uint64_t v = 0;
		for (unsigned i = 0; i < 8; ++i)
			v |= static_cast<uint64_t>(at[i]) << (8u * i);
		return v;
	}

	// Writes the 8 B header at `at` (kHeaderBytes writable). False (nothing written) for a length outside kHeaderBytes .. kRecordBytesMax.
	inline bool WriteHeader(unsigned char* at, size_t lengthBytes)
	{
		if (lengthBytes < kHeaderBytes || lengthBytes > kRecordBytesMax)
			return false;
		PutU16(at, static_cast<uint16_t>((unsigned(kMagic) << 8) | kUnitVersion));
		PutU16(at + 2, static_cast<uint16_t>(lengthBytes));
		PutU32(at + 4, 0u);
		return true;
	}

	enum class HeaderStatus : uint8_t { Ok, TooShort, BadMagic, UnitVersion, BadLength };
	struct Header
	{
		uint16_t unitVersion = 0;
		uint16_t lengthBytes = 0;
	};
	inline const char* HeaderText(HeaderStatus s)
	{
		switch (s)
		{
		case HeaderStatus::Ok: return "ok";
		case HeaderStatus::TooShort: return "buffer shorter than the 8 B header";
		case HeaderStatus::BadMagic: return "not a source geometry record";
		case HeaderStatus::UnitVersion: return "unit version";
		case HeaderStatus::BadLength: return "record length outside the header .. 512 B bound or beyond the buffer";
		}
		return "?";
	}
	// `available` = the bytes readable at `at`. On Ok `out` holds the version and the length.
	inline HeaderStatus ReadHeader(const unsigned char* at, size_t available, Header* out)
	{
		if (!at || available < kHeaderBytes)
			return HeaderStatus::TooShort;
		const uint16_t first = GetU16(at);
		if ((first >> 8) != kMagic)
			return HeaderStatus::BadMagic;
		const uint16_t version = static_cast<uint16_t>(first & 0xFFu);
		if (version != kUnitVersion)
			return HeaderStatus::UnitVersion;
		const uint16_t length = GetU16(at + 2);
		if (length < kHeaderBytes || length > kRecordBytesMax || length > available)
			return HeaderStatus::BadLength;
		if (out)
		{
			out->unitVersion = version;
			out->lengthBytes = length;
		}
		return HeaderStatus::Ok;
	}

	// ---- the field budget (doc 78 section 6) --------------------------------------------------------------------------------------
	// The CEILING the source-identity layout must stay within, one line per field of the doc 78 list with its byte width and how many
	// times it occurs: per axis (horizontal, vertical) and per side (measured, applied) the state, n, N, ratio, phase, kernel family and two
	// parameters, kernel confidence, sharpening, double scaling, confidence, LLR and reason bits; per axis the effective bandwidth; the active
	// area; the coded-raster class; the reserved chroma chain (two planes); the generations and timing. NOT the layout (SID-C lays the bytes
	// out and bumps kUnitVersion with any change); a test pins that header + budget fits kRecordBytesMax with room to grow.
	struct FieldBudget
	{
		const char* name;
		uint16_t bytes;   // one occurrence
		uint8_t count;    // occurrences (axis x side = 4, per axis = 2, ...)
	};
	inline const FieldBudget* FieldBudgets(size_t* count)
	{
		static const FieldBudget kFields[] = {
			{ "state (per axis and side)", 1, 4 },
			{ "n", 2, 4 },
			{ "N", 2, 4 },
			{ "ratio", 4, 4 },
			{ "phase", 4, 4 },
			{ "kernel family", 1, 4 },
			{ "kernel parameters (2 x f32)", 8, 4 },
			{ "kernel confidence", 4, 4 },
			{ "sharpening {present, amount, radius, confidence}", 13, 4 },
			{ "double scaling {present, n1, confidence}", 7, 4 },
			{ "confidence", 4, 4 },
			{ "LLR", 4, 4 },
			{ "reason bits", 4, 4 },
			{ "effective bandwidth per axis", 4, 2 },
			{ "active area {x, y, w, h, confidence}", 12, 1 },
			{ "coded-raster class", 1, 1 },
			{ "chroma chain per plane {origin sampling, player-upsampled, kernel, siting x, siting y, confidence} (reserved)", 9, 2 },
			{ "geometryGen", 8, 1 },
			{ "appliedGen", 8, 1 },
			{ "evalSeq", 8, 1 },
			{ "evalTimeUs", 8, 1 },
			{ "maxAgeUs", 8, 1 },
		};
		if (count)
			*count = sizeof(kFields) / sizeof(kFields[0]);
		return kFields;
	}
	inline size_t FieldBudgetBytes()
	{
		size_t n = 0;
		const FieldBudget* f = FieldBudgets(&n);
		size_t total = 0;
		for (size_t i = 0; i < n; ++i)
			total += size_t(f[i].bytes) * f[i].count;
		return total;
	}

	// ---- the readers --------------------------------------------------------------------------------------------------------------

	// The provisional readers (doc 78 section 6 table; the catalog rows in core/ProductCatalog.h state them, a test compares).
	enum class Consumer : uint8_t { QualityPattern, CodingEvidence, TemporalDetail, AdaptiveScaler, ChromaStage, Status };
	constexpr size_t kConsumerCount = 6;

	inline const char* Name(Consumer c)
	{
		switch (c)
		{
		case Consumer::QualityPattern: return "quality pattern";
		case Consumer::CodingEvidence: return "coding evidence";
		case Consumer::TemporalDetail: return "temporal detail";
		case Consumer::AdaptiveScaler: return "adaptive scaling";
		case Consumer::ChromaStage: return "chroma stage";
		case Consumer::Status: return "status";
		}
		return "?";
	}

	// What the reader does instead when the record cannot be used (its classical form, never nothing).
	inline const char* ClassicalForm(Consumer c)
	{
		switch (c)
		{
		case Consumer::QualityPattern:
		case Consumer::CodingEvidence: return "grid-free evidence (frame pattern: no source geometry)";
		case Consumer::TemporalDetail: return "the delivered raster";
		case Consumer::AdaptiveScaler:
		case Consumer::ChromaStage: return "today's behaviour";
		case Consumer::Status: return "Source identity: classical only";
		}
		return "";
	}

	// What the writer says about the record right now.
	struct Entry
	{
		bool published = false;
		uint32_t unitVersion = 0;   // the unit version that made it
		uint64_t seq = 0;           // the frame the evaluation was sealed with: visible to a reader of frame N only when seq <= N
		int64_t timeUs = 0;         // source time of the evaluation
		int64_t maxAgeUs = 0;       // stale after this age; <= 0 = the writer has not measured its interval yet: never used (kCauseNotMeasured)
	};

	// The reservation's PLACEHOLDER producer: nothing is ever published (absent).
	inline const Entry* PlaceholderEntry()
	{
		static const Entry empty{};
		return &empty;
	}

	// Whether any consumer asks for the geometry: source_id_consensus = On (the Monitor's effective mode: the Original pathway forces Off,
	// Shadow only computes and logs). `consensusOn` is the caller's state of that key; nothing else asks.
	inline bool Requested(bool consensusOn) { return consensusOn; }

	// Why the record is not usable: one bit per cause.
	enum : unsigned { kCauseAbsent = 1u, kCauseUnitVersion = 2u, kCauseStale = 4u, kCauseNotYet = 8u, kCauseNoProducer = 16u, kCauseNotMeasured = 32u,
	                  kCauseCorrupt = 64u };   // kCauseCorrupt: the bytes are not a record of this unit (SID-C2a: Unpack)

	inline unsigned CauseOf(const Entry* e, uint64_t frameSeq, int64_t nowUs)
	{
		if (!e)
			return kCauseNoProducer;
		if (!e->published)
			return kCauseAbsent;
		if (e->unitVersion != kUnitVersion)
			return kCauseUnitVersion;
		if (e->seq > frameSeq)
			return kCauseNotYet;   // sealed after the frame being rendered: a reader never reads the future
		// The writer states no maximum age until it has MEASURED its evaluation interval: say so plainly (not "stale"), and never use the record.
		if (e->maxAgeUs <= 0)
			return kCauseNotMeasured;
		if (AbsDiffUs(nowUs, e->timeUs) > static_cast<uint64_t>(e->maxAgeUs))
			return kCauseStale;
		return 0;
	}

	inline const char* CauseWord(unsigned cause)
	{
		switch (cause)
		{
		case kCauseAbsent: return "absent";
		case kCauseUnitVersion: return "unit version";
		case kCauseStale: return "stale";
		case kCauseNotYet: return "not sealed yet";
		case kCauseNoProducer: return "no producer";
		case kCauseNotMeasured: return "evaluation interval not measured yet";
		case kCauseCorrupt: return "corrupt record";
		}
		return "";
	}

	struct ReadResult
	{
		bool requested = false;
		bool usable = false;          // the consumer may use the record
		unsigned cause = 0;           // kCause*; 0 unless requested && !usable
		std::string reason;           // "" unless the classical form stands in for a requested record
		// Engaged exactly when requested && !usable: id "product.source_geometry" (FallbackCatalog product entry) with the reason, which
		// names the consumer and the cause.
		FallbackCatalog::Decision fallback;
	};

	// `entry` may be null (no producer at all). Not requested: nothing is engaged, no string is built (byte-identical to not reading).
	inline ReadResult Read(Consumer consumer, const Entry* entry, bool requested, uint64_t frameSeq, int64_t nowUs)
	{
		ReadResult r;
		r.requested = requested;
		if (!requested)
			return r;
		r.cause = CauseOf(entry, frameSeq, nowUs);
		if (r.cause == 0)
		{
			r.usable = true;
			return r;
		}
		r.reason = std::string(Name(consumer)) + ": source geometry " + CauseWord(r.cause) + ", " + ClassicalForm(consumer);
		r.fallback = FallbackCatalog::ProductFallback("product.source_geometry", r.reason);
		return r;
	}

	// The read of a consumer in a build without the estimator wired: the placeholder producer, nothing requested. Byte-identical to not
	// reading at all.
	inline ReadResult ReadToday(Consumer consumer) { return Read(consumer, PlaceholderEntry(), Requested(false), 0, 0); }

	// The ONE decision of `product.source_geometry` over the reads of every consumer (one id, so one consumer's recovery cannot release
	// another's entry): engaged while any requested read is not usable, the reason naming each consumer. Released otherwise.
	inline FallbackCatalog::Decision Combine(const ReadResult* reads, size_t count)
	{
		std::string why;
		for (size_t i = 0; i < count; ++i)
			if (reads[i].fallback.engaged)
				why += (why.empty() ? "" : ", ") + reads[i].reason;
		return why.empty() ? FallbackCatalog::Released("product.source_geometry")
		                   : FallbackCatalog::ProductFallback("product.source_geometry", why);
	}

	// The status line of a consumer under a read: its fallback wording while the classical form stands in, "" otherwise.
	inline std::string StatusLine(Consumer consumer, const ReadResult& r)
	{
		return r.fallback.engaged ? std::string(Name(consumer)) + ": classical form (source geometry: " + CauseWord(r.cause) + ")"
		                          : std::string();
	}

	// ================================================================================================================================
	// The RECORD (SID-C2a, the source-identity build): the content of the payload behind the 8 B header. The framework above (header,
	// age, causes, readers) is the skeleton's; the fields and their layout below are SID's. Per axis a MEASURED geometry (the current
	// best hypothesis, diagnostic, whatever its strength) and an APPLIED geometry (the CONFIDENT reading: written only while the evidence is above
	// c1, otherwise the axis is UNSURE and nothing is applied; not sticky), plus the active area, the
	// coded-raster class and reserved effective bandwidth / chroma chain fields (SID-C3 / SID-F). Axis convention as the Indicator
	// port: 0 = horizontal, 1 = vertical. The unit version is the header's (kUnitVersion above); the logical Record carries it so a
	// reader can test another version through the same Entry.
	// ================================================================================================================================

	// The kernel family codes = SourceIdentity::Chain::Family (chain/KernelBank.h): bilinear, bicubic, lanczos, spline; 255 = none.
	constexpr uint8_t kKernelFamilyUnknown = 255;

	// The fallback id of this product (the skeleton's catalog row).
	constexpr const char* kFallbackId = "product.source_geometry";

	// The per-axis reading state (doc 78 section 5). identity/chain/ChainReading.h produces it per axis. The numeric values are the unit's
	// layout (no unit bump): Candidate and Unstable belonged to the PARKED latch state machine and are never written; Latched = CONFIDENT.
	enum class LatchState : uint8_t
	{
		Unknown = 0,       // nothing measured (start, discontinuity, active-area change)
		Provisional = 1,   // a best hypothesis exists below c1 (UNSURE); published, no action
		Candidate = 2,     // reserved (parked latch design), never written
		Latched = 3,       // CONFIDENT: the best hypothesis is above c1 (the name is the reserved numeric; every text says "confident")
		Hold = 4,          // mixed picture / overlay: UNSURE, nothing applied
		Abstain = 5,       // no power (dark, flat, soft, low detail): UNSURE, nothing applied
		Unstable = 6,      // reserved (parked latch design), never written
		Count = 7
	};

	inline const char* StateText(LatchState s)
	{
		switch (s)
		{
		case LatchState::Unknown: return "unknown";
		case LatchState::Provisional: return "provisional";
		case LatchState::Candidate: return "candidate (reserved)";
		case LatchState::Latched: return "confident";
		case LatchState::Hold: return "hold";
		case LatchState::Abstain: return "abstain";
		case LatchState::Unstable: return "unstable (reserved)";
		default: return "?";
		}
	}

	enum class CodedRaster : uint8_t { Unknown = 0, PlayerUpscaled = 1, NativeCoded = 2, Count = 3 };

	struct SharpenGeom
	{
		bool present = false;
		float amount = 0.0f, radius = 0.0f, conf = 0.0f;
	};

	struct DoubleGeom
	{
		bool present = false;
		int32_t n1 = 0;                 // the first-stage size (0 = unknown)
		float conf = 0.0f;
	};

	// One axis of one geometry (measured or applied).
	struct AxisGeom
	{
		LatchState state = LatchState::Unknown;
		int32_t n = 0;                  // original (latent) size along the axis; 0 = unknown
		int32_t N = 0;                  // delivered size along the axis (active area); 0 = unknown
		float ratio = 1.0f;             // n / N (1 = native, no upscale)
		float phase = 0.0f;             // sub-sample phase of the scaler, latent samples
		uint8_t kernelFamily = kKernelFamilyUnknown;
		int8_t kernelIndex = -1;        // KernelBank index of a kernel with a classical equivalent, -1 = unknown ("n known, kernel unsure")
		float kernelParamA = 0.0f;      // B (cubic) or lobes (lanczos)
		float kernelParamB = 0.0f;      // C (cubic)
		float kernelConf = 0.0f;        // 0..1
		SharpenGeom sharpen;
		DoubleGeom dbl;
		float conf = 0.0f;              // 0..1 summary confidence of the field
		float llr = 0.0f;               // evidence, nats
		uint32_t reasonBits = 0;        // Chain::kReason* of the latest evaluation
	};

	struct ActiveArea
	{
		int32_t x = 0, y = 0, w = 0, h = 0;
		float conf = 0.0f;              // 0 = not measured
	};

	// Reserved for SID-F: the chroma chain of one chroma plane.
	struct ChromaChain
	{
		uint8_t originSampling = 0;     // 0 unknown (reserved)
		uint8_t playerUpsampled = 0;    // 0 unknown, 1 no, 2 yes
		int8_t kernel = -1;
		int8_t siting = -1;
		float conf = 0.0f;
	};

	enum RecordFlags : uint16_t
	{
		kFlagShadow = 1u << 0           // SHADOW mode: published and logged, the classical decision stands
	};

	// Logical form; Pack / Unpack are the codec.
	struct Record
	{
		uint32_t unitVersion = kUnitVersion;   // not packed: it is the header's
		uint16_t flags = 0;
		uint64_t streamKey = 0;          // the Indicator's FULL/era counter (title)
		uint64_t evalSeq = 0;            // newest source sequence behind this evaluation (the ReadLatest key)
		int64_t evalTimeUs = 0;          // source time of the evaluation
		uint64_t publishSerial = 0;      // 1, 2, ... per Publish since the holder's reset
		int64_t maxAgeUs = 0;            // stale after this (MaxAgeUs(longest PLANNED evaluation interval)); 0 = not measured yet
		uint64_t geometryGen = 0;        // bumps whenever a MEASURED field changed
		uint64_t appliedGen = 0;         // bumps whenever an APPLIED (confident) field changed
		uint32_t discontinuities = 0;    // bumps at every reset (the consumers restart their generations)
		float costMs = 0.0f;             // wall time of this evaluation (the CPU-share governor, Ctrl+Alt+D)
		AxisGeom measured[2];            // the current best (provisional)
		AxisGeom applied[2];             // the CONFIDENT reading (state Unknown while the axis is UNSURE)
		ActiveArea area;
		CodedRaster codedRaster = CodedRaster::Unknown;
		float effectiveBandwidth[2] = { 0.0f, 0.0f };   // cycles / px per axis (reserved, SID-C3)
		ChromaChain chroma[2];                          // Cb, Cr (reserved, SID-F)

		// What the writer says about this record (the skeleton's Entry): the one input of Read() / CauseOf().
		Entry AsEntry() const
		{
			Entry e;
			e.published = true;
			e.unitVersion = unitVersion;
			e.seq = evalSeq;
			e.timeUs = evalTimeUs;
			e.maxAgeUs = maxAgeUs;
			return e;
		}
		// 0 = usable by the reader of frame `frameSeq` at `nowUs`, else the kCause* (absent, version, not yet, not measured, stale).
		unsigned Cause(uint64_t frameSeq, int64_t nowUs) const
		{
			const Entry e = AsEntry();
			return CauseOf(&e, frameSeq, nowUs);
		}
	};

	// ---- byte codec (little endian, fixed layout behind the 8 B header) ----------------------------------------------------------
	namespace detail
	{
		// p == nullptr: a counting writer (no bytes written). A Put beyond `cap` is not written but still counted.
		struct Writer
		{
			unsigned char* p = nullptr;
			size_t cap = 0;
			size_t n = 0;
			template <class T> void Put(T v)
			{
				uint64_t u = 0;
				std::memcpy(&u, &v, sizeof(T));
				for (size_t i = 0; i < sizeof(T); ++i)
				{
					if (p != nullptr && n < cap)
						p[n] = static_cast<unsigned char>((u >> (8 * i)) & 0xFFu);
					++n;
				}
			}
		};
		struct Reader
		{
			const unsigned char* p;
			size_t n = 0;
			template <class T> T Get()
			{
				uint64_t u = 0;
				for (size_t i = 0; i < sizeof(T); ++i)
					u |= static_cast<uint64_t>(p[n++]) << (8 * i);
				T v;
				std::memcpy(&v, &u, sizeof(T));
				return v;
			}
		};

		inline void PutAxis(Writer& w, const AxisGeom& a)
		{
			w.Put(static_cast<uint8_t>(a.state));
			w.Put(a.n); w.Put(a.N); w.Put(a.ratio); w.Put(a.phase);
			w.Put(a.kernelFamily); w.Put(a.kernelIndex); w.Put(a.kernelParamA); w.Put(a.kernelParamB); w.Put(a.kernelConf);
			w.Put(static_cast<uint8_t>(a.sharpen.present ? 1 : 0)); w.Put(a.sharpen.amount); w.Put(a.sharpen.radius); w.Put(a.sharpen.conf);
			w.Put(static_cast<uint8_t>(a.dbl.present ? 1 : 0)); w.Put(a.dbl.n1); w.Put(a.dbl.conf);
			w.Put(a.conf); w.Put(a.llr); w.Put(a.reasonBits);
		}
		// False on an out-of-range enumeration (a corrupt record is refused, never guessed).
		inline bool GetAxis(Reader& r, AxisGeom& a)
		{
			const uint8_t st = r.Get<uint8_t>();
			a.n = r.Get<int32_t>(); a.N = r.Get<int32_t>(); a.ratio = r.Get<float>(); a.phase = r.Get<float>();
			a.kernelFamily = r.Get<uint8_t>(); a.kernelIndex = r.Get<int8_t>(); a.kernelParamA = r.Get<float>();
			a.kernelParamB = r.Get<float>(); a.kernelConf = r.Get<float>();
			a.sharpen.present = r.Get<uint8_t>() != 0; a.sharpen.amount = r.Get<float>(); a.sharpen.radius = r.Get<float>();
			a.sharpen.conf = r.Get<float>();
			a.dbl.present = r.Get<uint8_t>() != 0; a.dbl.n1 = r.Get<int32_t>(); a.dbl.conf = r.Get<float>();
			a.conf = r.Get<float>(); a.llr = r.Get<float>(); a.reasonBits = r.Get<uint32_t>();
			if (st >= static_cast<uint8_t>(LatchState::Count))
				return false;
			a.state = static_cast<LatchState>(st);
			return true;
		}

		inline void PutPayload(Writer& w, const Record& r)
		{
			w.Put(r.flags);
			w.Put(r.streamKey); w.Put(r.evalSeq); w.Put(r.evalTimeUs); w.Put(r.publishSerial); w.Put(r.maxAgeUs);
			w.Put(r.geometryGen); w.Put(r.appliedGen); w.Put(r.discontinuities); w.Put(r.costMs);
			for (int i = 0; i < 2; ++i) PutAxis(w, r.measured[i]);
			for (int i = 0; i < 2; ++i) PutAxis(w, r.applied[i]);
			w.Put(r.area.x); w.Put(r.area.y); w.Put(r.area.w); w.Put(r.area.h); w.Put(r.area.conf);
			w.Put(static_cast<uint8_t>(r.codedRaster));
			for (int i = 0; i < 2; ++i) w.Put(r.effectiveBandwidth[i]);
			for (int i = 0; i < 2; ++i)
			{
				w.Put(r.chroma[i].originSampling); w.Put(r.chroma[i].playerUpsampled); w.Put(r.chroma[i].kernel);
				w.Put(r.chroma[i].siting); w.Put(r.chroma[i].conf);
			}
		}
	}

	struct Packed
	{
		unsigned char bytes[kRecordBytesMax];
	};

	// The payload byte count: 2 + 8 * 5 + 8 * 2 + 4 + 4 + 4 axis blocks of 65 + area 20 + class 1 + 8 + 2 * 8 = 371 (header 8: 379 of 512).
	constexpr size_t kAxisBytes = 1 + 4 * 4 + 1 + 1 + 4 * 3 + (1 + 4 * 3) + (1 + 4 + 4) + 4 + 4 + 4;   // = 65
	constexpr size_t kPayloadBytes = 2 + 8 * 5 + 8 * 2 + 4 + 4 + 4 * kAxisBytes + 5 * 4 + 1 + 2 * 4 + 2 * 8;
	static_assert(kPayloadBytes <= kPayloadBytesMax, "P14g payload exceeds the 504 B the header leaves");

	inline Packed Pack(const Record& r)
	{
		Packed out;
		std::memset(out.bytes, 0, sizeof(out.bytes));
		WriteHeader(out.bytes, kHeaderBytes + kPayloadBytes);
		detail::Writer w;
		w.p = out.bytes + kHeaderBytes;
		w.cap = kPayloadBytesMax;
		detail::PutPayload(w, r);
		return out;
	}

	// The number of payload bytes Pack writes (a counting pass; the tests pin it to kPayloadBytes).
	inline size_t PayloadBytesUsed()
	{
		detail::Writer w;
		detail::PutPayload(w, Record());
		return w.n;
	}

	// False when the header is not ours (another magic / unit version / length: `cause` = kCauseUnitVersion for a version, else
	// kCauseCorrupt) or an enumeration is out of range (kCauseCorrupt). The reader falls back, visibly. `r` is untouched on failure.
	inline bool Unpack(const Packed& in, Record& r, unsigned* cause = nullptr)
	{
		Header h;
		const HeaderStatus hs = ReadHeader(in.bytes, sizeof(in.bytes), &h);
		if (hs != HeaderStatus::Ok)
		{
			if (cause)
				*cause = hs == HeaderStatus::UnitVersion ? kCauseUnitVersion : kCauseCorrupt;
			return false;
		}
		if (h.lengthBytes != kHeaderBytes + kPayloadBytes)
		{
			if (cause)
				*cause = kCauseCorrupt;
			return false;
		}
		detail::Reader rd{ in.bytes + kHeaderBytes };
		Record t;
		t.unitVersion = h.unitVersion;
		t.flags = rd.Get<uint16_t>();
		t.streamKey = rd.Get<uint64_t>(); t.evalSeq = rd.Get<uint64_t>(); t.evalTimeUs = rd.Get<int64_t>();
		t.publishSerial = rd.Get<uint64_t>(); t.maxAgeUs = rd.Get<int64_t>();
		t.geometryGen = rd.Get<uint64_t>(); t.appliedGen = rd.Get<uint64_t>(); t.discontinuities = rd.Get<uint32_t>();
		t.costMs = rd.Get<float>();
		bool ok = true;
		for (int i = 0; i < 2; ++i)
			ok = detail::GetAxis(rd, t.measured[i]) && ok;
		for (int i = 0; i < 2; ++i)
			ok = detail::GetAxis(rd, t.applied[i]) && ok;
		t.area.x = rd.Get<int32_t>(); t.area.y = rd.Get<int32_t>(); t.area.w = rd.Get<int32_t>(); t.area.h = rd.Get<int32_t>();
		t.area.conf = rd.Get<float>();
		const uint8_t cr = rd.Get<uint8_t>();
		ok = ok && cr < static_cast<uint8_t>(CodedRaster::Count);
		t.codedRaster = ok ? static_cast<CodedRaster>(cr) : CodedRaster::Unknown;
		for (int i = 0; i < 2; ++i) t.effectiveBandwidth[i] = rd.Get<float>();
		for (int i = 0; i < 2; ++i)
		{
			t.chroma[i].originSampling = rd.Get<uint8_t>(); t.chroma[i].playerUpsampled = rd.Get<uint8_t>();
			t.chroma[i].kernel = rd.Get<int8_t>(); t.chroma[i].siting = rd.Get<int8_t>(); t.chroma[i].conf = rd.Get<float>();
		}
		if (!ok)
		{
			if (cause)
				*cause = kCauseCorrupt;
			return false;
		}
		r = t;
		return true;
	}

	static_assert(sizeof(Packed) == kRecordBytesMax, "P14g record bytes");

	// ---- the held-state holder --------------------------------------------------------------------------------------------------
	// SINGLE WRITER (the chain estimator worker: claim, write, stamp last = publishSerial, publish); any thread reads the latest record
	// with evalSeq <= N. A small ring keeps the last kRing records so a read for an earlier frame (look-ahead, render-ahead) still
	// finds the record that was current for it. Mutex-guarded: ~380 B, read once per frame. The writer's evalSeq of every slot is kept
	// beside the bytes so a reader can stop at an UNDECODABLE record that is newer than its sequence and say so (another unit version,
	// corruption) instead of silently reading an older valid one.
	class HeldState
	{
	public:
		static constexpr size_t kRing = 8;

		void Reset()
		{
			std::lock_guard<std::mutex> g(m_);
			count_ = 0;
			head_ = 0;
			serial_ = 0;
		}

		// Worker thread. Stamps the serial last and returns it.
		uint64_t Publish(Record r)
		{
			std::lock_guard<std::mutex> g(m_);
			r.publishSerial = ++serial_;
			Store(Pack(r), r.evalSeq);
			return r.publishSerial;
		}

		// Tests and replay: stores already packed bytes (e.g. another unit version) under the sequence the writer claims for them.
		uint64_t PublishPacked(const Packed& p, uint64_t evalSeq)
		{
			std::lock_guard<std::mutex> g(m_);
			++serial_;
			Store(p, evalSeq);
			return serial_;
		}

		// Any thread: the newest record whose evalSeq is <= `seq` (0 = the newest of all). False: nothing published yet / nothing that
		// old (`cause` = kCauseAbsent), or the newest such record cannot be decoded (`cause` = kCauseUnitVersion / kCauseCorrupt): the
		// caller falls back to the delivered geometry, visibly.
		bool ReadLatest(uint64_t seq, Record& out, unsigned* cause = nullptr) const
		{
			std::lock_guard<std::mutex> g(m_);
			for (size_t i = 0; i < count_; ++i)
			{
				const size_t idx = (head_ + kRing - 1 - i) % kRing;
				if (seq != 0 && seqOf_[idx] > seq)
					continue;
				unsigned why = 0;
				if (Unpack(ring_[idx], out, &why))
					return true;
				if (cause)
					*cause = why;
				return false;   // the newest record for this reader cannot be read: never fall back to an older valid one
			}
			if (cause)
				*cause = kCauseAbsent;
			return false;
		}

		uint64_t Serial() const
		{
			std::lock_guard<std::mutex> g(m_);
			return serial_;
		}

	private:
		void Store(const Packed& p, uint64_t evalSeq)
		{
			ring_[head_] = p;
			seqOf_[head_] = evalSeq;
			head_ = (head_ + 1) % kRing;
			if (count_ < kRing)
				++count_;
		}

		mutable std::mutex m_;
		Packed ring_[kRing];
		uint64_t seqOf_[kRing] = {};
		size_t count_ = 0, head_ = 0;
		uint64_t serial_ = 0;
	};
}

// The chain units and the consensus call the product's types through the source-identity namespace (the record content is SID's).
namespace SourceIdentity
{
	namespace Geometry = ::SourceGeometry;
}
