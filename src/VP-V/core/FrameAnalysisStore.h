#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "FallbackStatus.h"

namespace CameraModel { struct CameraMotion; }

// THE VP FRAME ANALYSIS STORE (WP-23, owner 2026-10-24; integration-batch-plan.md WP-23 + addendum).
//
// VP's own per-source-frame analysis is computed ONCE per source frame, inside our umbrella, kept
// in a ring keyed by the source sequence and READ by every VP stage (LED look-ahead, intent,
// dither, deband, texture restore, DV reference conditions, nets, diagnostics) instead of each
// stage decoding the raw frame again. Bill's analysis (scene detection, active picture, subtitles)
// keeps its own decisions; it reads the same shared luma decode (AnalysisLumaPlane), not this store.
//
// Range model (WP-23 decision, docs/design/wp23-frame-analysis-store.md "Range model"): every
// luma value in a record is a 10-bit STUDIO code (black 64, white 940; BTB below 64 and WTW above
// 940 kept, never clipped). The producer converts the sampler's codes to that domain in ONE place
// (VpFrameAnalysis::StudioLumaMap); no consumer applies a range conversion of its own.
//
// Validity (vp-vector-validity): ONE discontinuity epoch, shared with the vectors-valid state.
// Every discontinuity (source/format/size/range change, crop or geometry generation change,
// seek/reset, dropped or duplicated frame, queue flush/backlog shed, DV metadata loss, a settings
// change that alters what is measured) calls Invalidate(cause). A record from an older epoch is
// never served for TEMPORAL use (look-ahead windows, temporal statistics); for SINGLE-FRAME use it
// is served only when its own group bits are valid. Motion summaries are cleared at every
// invalidation (a vector pair never spans a discontinuity).
//
// Missing or late records: the consumer keeps its existing computation and reports it VISIBLY
// (vp-no-silent-fallbacks): FallbackEntry(consumer, status) gives
//   "Fallbacks engaged: frame analysis (<consumer>: <why>)".
//
// Order rule (modularity sweep): a record describes the SOURCE as input.source_analysis saw it
// (order 40, before every movable module). A consumer slot that reads it must declare that it wants
// pre-module statistics in SlotContracts, so OrderValidator sees the dependency; a consumer placed
// after a module that changes the picture (re-base, deband, encoding repair) must not treat the
// record as describing its own input. Each consumer registers behind its own slot's Runs().
//
// Threads: Begin() on the arrival thread, Publish*() on the producer's worker, Read*() on any
// thread (render loop, look-ahead walks). One mutex; every critical section is a slot copy, never
// a computation, so a reader is never held up by the producer's work.
//
// The kCamera group carries the global camera model record (Plan 19) as a shared pointer only.
//
// Header-only and pure (no Windows, no clock), C++14 like the other core types.
namespace FrameAnalysis
{
	// The 10-bit studio code domain of every luma value in a record.
	constexpr int kHistogramBins = 1024;
	constexpr uint16_t kStudioBlack = 64;
	constexpr uint16_t kStudioWhite = 940;
	// Near black: within 2% of the nominal range above black (64 + 17.52 -> codes <= 81).
	constexpr uint16_t kNearBlackMax = 81;
	// Clipped highlights: the top of the 10-bit container (codes 1019..1023; WTW probe code 1019).
	constexpr uint16_t kClippedMin = 1019;

	// Record groups (valid / expected bits).
	enum Group : uint32_t
	{
		kIdentity = 1u << 0,
		kLuma = 1u << 1,      // histogram, mean/APL, percentiles
		kRange = 1u << 2,     // BTB / WTW / near-black / clipped occupancy
		kGrain = 1u << 3,     // grain / noise estimate (flat blocks); never used to remove grain
		kChroma = 1u << 4,    // optional chroma summary (placeholder: not produced yet)
		kMotion = 1u << 5,    // motion summary + vectors-valid (written by the motion segment)
		kCadence = 1u << 6,   // repeat-of-previous, cadence phase, film clock
		kScene = 1u << 7,     // cut / fade (VP's own, or copied from Bill's decision, labelled)
		kLabels = 1u << 8,    // ID-net / encoding labels v1 (placeholder: not produced yet)
		kCamera = 1u << 9,    // global camera model record (Plan 19), written by the motion segment
	};
	constexpr uint32_t kMeasuredGroups = kLuma | kRange | kGrain;

	struct Rect
	{
		int left = 0, top = 0, right = 0, bottom = 0;
		int Width() const { return right - left; }
		int Height() const { return bottom - top; }
		bool Empty() const { return right <= left || bottom <= top; }
		bool operator==(const Rect& o) const { return left == o.left && top == o.top && right == o.right && bottom == o.bottom; }
	};

	enum class SourceRange : uint8_t { Unknown, Limited, Full };

	struct Identity
	{
		uint64_t seq = 0;               // source sequence (the ring key)
		int64_t captureQpc = 0;         // capture/arrival time (frame start), 0 = unknown
		int width = 0, height = 0;      // delivered size
		uint32_t encoding = 0;          // VideoFrameEncoding of the delivered frame
		uint32_t colorspace = 0;        // ColorSpace
		uint32_t transfer = 0;          // EOTF key
		SourceRange range = SourceRange::Unknown;
		// The active-picture rectangle the record measured: copied from Bill's crop decision (read
		// only), or the full raster when none is known. The record says which crop it measured.
		Rect picture;
		uint64_t geometryGeneration = 0;
		uint64_t sourceGeneration = 0;  // AnalysisLumaSource::generation of the measured view
	};

	struct LumaStats
	{
		std::array<uint32_t, kHistogramBins> histogram{};   // full coverage of `picture`, studio codes
		uint64_t samples = 0;
		double mean = 0.0;              // mean studio code
		double apl = 0.0;               // average picture level: (mean - 64) / 876, BTB/WTW not clipped
		uint16_t minimum = 0, p1 = 0, p50 = 0, p99 = 0, maximum = 0;
	};

	struct RangeOccupancy
	{
		double btb = 0.0;               // share of samples below black (code < 64)
		double wtw = 0.0;               // share above white (code > 940)
		double nearBlack = 0.0;         // share at or below kNearBlackMax (BTB included)
		// Share of LUMA at or above kClippedMin. Luma only: an RGB source (native RGB, the DV tunnel's
		// r210 container) can clip one channel, or reach RGB 1019 at a luma below 1019 (r210's
		// 64..960 scaling), without counting here. A consumer that needs "the picture clips" for RGB
		// needs a max(R, G, B) group (not built; audit L1).
		double clipped = 0.0;
	};

	struct GrainEstimate
	{
		double rms = 0.0;               // noise/grain RMS in studio codes (flat-block method)
		double confidence = 0.0;        // 0..1: share of blocks flat enough to measure
		uint32_t blocks = 0, flatBlocks = 0;
	};

	struct ChromaSummary
	{
		double meanSaturation = 0.0, peakSaturation = 0.0;
	};

	enum class VectorSource : uint8_t { None, EngineAhead, AtRender, Matcher };

	struct MotionSummary
	{
		bool vectorsValid = false;      // MotionSegment::Validity() for the pair ending at this frame
		uint64_t pairFrom = 0;          // the earlier frame of the pair
		double globalX = 0.0, globalY = 0.0;   // global motion, source pixels per frame
		float magnitudeP50 = 0.0f, magnitudeP95 = 0.0f;
		VectorSource source = VectorSource::None;
	};

	struct Cadence
	{
		bool repeatOfPrevious = false;  // a captured repeat: the record points at the original
		uint64_t repeatOf = 0;
		int phase = -1;                 // cadence phase (-1 = unknown)
		double filmHz = 0.0;            // film clock when pulldown is locked (0 = the signal's rate)
	};

	enum class SceneOrigin : uint8_t { None, Vp, BillCopied };

	struct Scene
	{
		bool cut = false;
		bool fade = false;
		SceneOrigin origin = SceneOrigin::None;   // consumers needing a cut read one value
	};

	struct Labels
	{
		uint32_t version = 0;           // 0 = none yet; 1 = label schema v1
		int codecClass = -1, bppTier = -1, bitDepth = 0, playerKernel = -1;
		double deliveredRatio = 0.0;
	};

	struct Record
	{
		Identity id;
		LumaStats luma;
		RangeOccupancy range;
		GrainEstimate grain;
		ChromaSummary chroma;
		MotionSummary motion;
		Cadence cadence;
		Scene scene;
		Labels labels;
		std::shared_ptr<const CameraModel::CameraMotion> camera;
		uint32_t valid = 0;             // Group bits written
		uint32_t expected = 0;          // Group bits the producer was asked for
		uint64_t epoch = 0;             // the discontinuity epoch at Begin()
		bool Complete() const { return (valid & expected) == expected; }
	};

	enum class Use { SingleFrame, Temporal };

	enum class ReadStatus
	{
		Ok,
		Missing,        // no record for this sequence (never begun, or evicted / released)
		Late,           // begun, but a requested group has not been published yet
		StaleEpoch,     // temporal use of a record from before the last discontinuity
		GroupInvalid,   // a requested group was not asked of the producer (or was invalidated)
		Disabled,       // the store is off (frame_analysis_store)
	};

	inline const char* StatusText(ReadStatus status)
	{
		switch (status)
		{
		case ReadStatus::Ok: return "ok";
		case ReadStatus::Missing: return "no record";
		case ReadStatus::Late: return "record late";
		case ReadStatus::StaleEpoch: return "record from before a discontinuity";
		case ReadStatus::GroupInvalid: return "not measured";
		case ReadStatus::Disabled: return "store off";
		}
		return "unknown";
	}

	enum class Discontinuity
	{
		SourceChange,
		FormatChange,       // signal format / size / range
		GeometryChange,     // crop or geometry generation
		SeekOrReset,
		DroppedFrame,
		DuplicatedFrame,
		QueueFlush,         // flush / backlog shed
		DvMetadataLoss,
		SettingsChange,     // a setting that alters what is measured
		Count
	};

	inline const char* DiscontinuityName(Discontinuity cause)
	{
		switch (cause)
		{
		case Discontinuity::SourceChange: return "source change";
		case Discontinuity::FormatChange: return "format change";
		case Discontinuity::GeometryChange: return "geometry change";
		case Discontinuity::SeekOrReset: return "seek or reset";
		case Discontinuity::DroppedFrame: return "dropped frame";
		case Discontinuity::DuplicatedFrame: return "duplicated frame";
		case Discontinuity::QueueFlush: return "queue flush";
		case Discontinuity::DvMetadataLoss: return "DV metadata loss";
		case Discontinuity::SettingsChange: return "settings change";
		default: return "unknown";
		}
	}

	// Ring depth = queue depth + look-ahead depth + 2, both taken from the delay budget
	// (DelayBudget::Plan: queue frames; the look-ahead window the consumers walk). Never a fixed
	// frame count (vp-general-not-fixed); at least 3 so a record outlives its display.
	inline size_t RingDepth(int queueFrames, int lookaheadFrames)
	{
		const int depth = (std::max)(0, queueFrames) + (std::max)(0, lookaheadFrames) + 2;
		return static_cast<size_t>((std::max)(3, depth));
	}

	// The visible fallback entry (one id; the reason names the consumer and why).
	constexpr const char* kFallbackId = "frame analysis";

	inline FallbackStatus::Entry FallbackEntry(const std::string& consumer, ReadStatus status)
	{
		return FallbackStatus::Entry{ kFallbackId, consumer + ": " + StatusText(status) };
	}

	struct Counters
	{
		uint64_t begun = 0, published = 0, served = 0, missing = 0, late = 0, stale = 0, notMeasured = 0;
		uint64_t evicted = 0;           // a slot reused while its record was never released
		uint64_t repeats = 0;           // captured repeats recorded without a re-measure
		std::array<uint64_t, static_cast<size_t>(Discontinuity::Count)> invalidations{};
	};

	class Store
	{
	public:
		explicit Store(size_t depth = RingDepth(4, 4)) { Resize(depth); }

		// A new depth (from the delay budget) forgets every record; the epoch moves on.
		void Resize(size_t depth)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_slots.assign((std::max)(static_cast<size_t>(3), depth), Slot{});
			++m_epoch;
		}

		size_t Depth() const
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			return m_slots.size();
		}

		void SetEnabled(bool enabled)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (m_enabled == enabled) return;
			m_enabled = enabled;
			for (Slot& slot : m_slots) slot = Slot{};
			++m_epoch;
		}

		bool Enabled() const
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			return m_enabled;
		}

		uint64_t Epoch() const
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			return m_epoch;
		}

		// A discontinuity: older records stop serving temporal use; motion summaries are cleared.
		uint64_t Invalidate(Discontinuity cause)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			++m_epoch;
			const size_t index = static_cast<size_t>(cause);
			if (index < m_counters.invalidations.size()) ++m_counters.invalidations[index];
			m_lastCause = cause;
			m_haveCause = true;
			for (Slot& slot : m_slots)
				if (slot.used)
				{
					// Not "late": the pair can no longer arrive (reads report "not measured").
					slot.record.valid &= ~static_cast<uint32_t>(kMotion);
					slot.record.expected &= ~static_cast<uint32_t>(kMotion);
					slot.record.motion = MotionSummary{};
					slot.record.valid &= ~static_cast<uint32_t>(kCamera);
					slot.record.expected &= ~static_cast<uint32_t>(kCamera);
					slot.record.camera.reset();
				}
			return m_epoch;
		}

		bool LastCause(Discontinuity& cause) const
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			cause = m_lastCause;
			return m_haveCause;
		}

		// Arrival: open the frame's record (identity valid, the other groups Late until published).
		// Returns the record's epoch, 0 when the store is off.
		uint64_t Begin(const Identity& identity, uint32_t expectedGroups)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (!m_enabled || m_slots.empty()) return 0;
			Slot& slot = m_slots[identity.seq % m_slots.size()];
			if (slot.used && slot.record.id.seq != identity.seq) ++m_counters.evicted;
			slot = Slot{};
			slot.used = true;
			slot.record.id = identity;
			slot.record.expected = expectedGroups | kIdentity;
			slot.record.valid = kIdentity;
			slot.record.epoch = m_epoch;
			++m_counters.begun;
			return m_epoch;
		}

		// The producer's measured groups (luma, range, grain). False when the record is gone.
		bool PublishMeasured(uint64_t seq, uint32_t groups, const LumaStats& luma, const RangeOccupancy& range,
			const GrainEstimate& grain)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			Slot* slot = FindLocked(seq);
			if (!slot) return false;
			if (groups & kLuma) slot->record.luma = luma;
			if (groups & kRange) slot->record.range = range;
			if (groups & kGrain) slot->record.grain = grain;
			slot->record.valid |= groups & kMeasuredGroups;
			++m_counters.published;
			return true;
		}

		bool PublishMotion(uint64_t seq, const MotionSummary& motion)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			Slot* slot = FindLocked(seq);
			// A pair that started before the record's epoch spans a discontinuity: not written.
			if (!slot || slot->record.epoch != m_epoch) return false;
			slot->record.motion = motion;
			slot->record.valid |= kMotion;
			return true;
		}

		bool PublishCamera(uint64_t seq, std::shared_ptr<const CameraModel::CameraMotion> camera)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			Slot* slot = FindLocked(seq);
			if (!slot || slot->record.epoch != m_epoch) return false;
			slot->record.camera = std::move(camera);
			slot->record.valid |= kCamera;
			return true;
		}

		bool PublishCadence(uint64_t seq, const Cadence& cadence)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			Slot* slot = FindLocked(seq);
			if (!slot) return false;
			slot->record.cadence = cadence;
			slot->record.valid |= kCadence;
			return true;
		}

		bool PublishScene(uint64_t seq, const Scene& scene)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			Slot* slot = FindLocked(seq);
			if (!slot) return false;
			slot->record.scene = scene;
			slot->record.valid |= kScene;
			return true;
		}

		bool PublishLabels(uint64_t seq, const Labels& labels)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			Slot* slot = FindLocked(seq);
			if (!slot) return false;
			slot->record.labels = labels;
			slot->record.valid |= kLabels;
			return true;
		}

		// A captured repeat: the record points at the original and copies its measured groups (no
		// re-measure; fixes the LED re-measure of identical pictures). False when the original is
		// gone, from another epoch or not measured yet: the caller measures the repeat as a frame.
		bool MarkRepeat(uint64_t seq, uint64_t original)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			Slot* slot = FindLocked(seq);
			const Slot* source = FindLocked(original);
			if (!slot || !source || seq == original || source->record.epoch != slot->record.epoch ||
				(source->record.valid & kMeasuredGroups) == 0)
				return false;
			const uint32_t measured = source->record.valid & kMeasuredGroups;
			slot->record.luma = source->record.luma;
			slot->record.range = source->record.range;
			slot->record.grain = source->record.grain;
			slot->record.valid |= measured | kCadence;
			slot->record.cadence.repeatOfPrevious = true;
			slot->record.cadence.repeatOf = source->record.cadence.repeatOfPrevious ?
				source->record.cadence.repeatOf : original;
			++m_counters.repeats;
			return true;
		}

		ReadStatus Read(uint64_t seq, Use use, uint32_t groups, Record& out) const
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			return ReadLocked(seq, use, groups, out);
		}

		// A temporal window (look-ahead): every record present, complete for `groups` and from the
		// CURRENT epoch, or the first failing status (out is then cleared).
		ReadStatus ReadWindow(const std::vector<uint64_t>& seqs, uint32_t groups, std::vector<Record>& out) const
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			out.clear();
			out.reserve(seqs.size());
			for (uint64_t seq : seqs)
			{
				Record record;
				const ReadStatus status = ReadLocked(seq, Use::Temporal, groups, record);
				if (status != ReadStatus::Ok)
				{
					out.clear();
					return status;
				}
				out.push_back(record);
			}
			return ReadStatus::Ok;
		}

		// Frames dropped from the queue release their slot.
		void Release(uint64_t seq)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (Slot* slot = FindLocked(seq)) *slot = Slot{};
		}

		Counters Snapshot() const
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			return m_counters;
		}

		// One status line for Ctrl+I / Ctrl+Alt+D (diagnostics summary section, WP-11 / WP-17).
		std::string Summary() const
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (!m_enabled) return "Frame analysis: off";
			std::string text = "Frame analysis: ring " + std::to_string(m_slots.size()) +
				", epoch " + std::to_string(m_epoch) +
				", served " + std::to_string(m_counters.served) +
				", late " + std::to_string(m_counters.late) +
				", missing " + std::to_string(m_counters.missing) +
				", stale " + std::to_string(m_counters.stale) +
				", repeats reused " + std::to_string(m_counters.repeats);
			if (m_haveCause) text += std::string(", last discontinuity ") + DiscontinuityName(m_lastCause);
			return text;
		}

	private:
		struct Slot
		{
			bool used = false;
			Record record;
		};

		Slot* FindLocked(uint64_t seq)
		{
			if (m_slots.empty()) return nullptr;
			Slot& slot = m_slots[seq % m_slots.size()];
			return slot.used && slot.record.id.seq == seq ? &slot : nullptr;
		}

		const Slot* FindLocked(uint64_t seq) const
		{
			if (m_slots.empty()) return nullptr;
			const Slot& slot = m_slots[seq % m_slots.size()];
			return slot.used && slot.record.id.seq == seq ? &slot : nullptr;
		}

		ReadStatus ReadLocked(uint64_t seq, Use use, uint32_t groups, Record& out) const
		{
			if (!m_enabled) return ReadStatus::Disabled;
			const Slot* slot = FindLocked(seq);
			if (!slot)
			{
				++m_counters.missing;
				return ReadStatus::Missing;
			}
			const Record& record = slot->record;
			if (use == Use::Temporal && record.epoch != m_epoch)
			{
				++m_counters.stale;
				return ReadStatus::StaleEpoch;
			}
			// Asked of the producer but not written yet: late. Never asked (or invalidated, like
			// motion at a discontinuity): not measured.
			const uint32_t missing = groups & ~record.valid;
			if (missing & record.expected)
			{
				++m_counters.late;
				return ReadStatus::Late;
			}
			if (missing)
			{
				++m_counters.notMeasured;
				return ReadStatus::GroupInvalid;
			}
			out = record;
			++m_counters.served;
			return ReadStatus::Ok;
		}

		mutable std::mutex m_mutex;
		std::vector<Slot> m_slots;
		uint64_t m_epoch = 0;
		bool m_enabled = true;
		bool m_haveCause = false;
		Discontinuity m_lastCause = Discontinuity::SourceChange;
		mutable Counters m_counters;
	};
}
