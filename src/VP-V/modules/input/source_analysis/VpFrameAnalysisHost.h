#pragma once

// The renderer's ONE handle on the frame analysis store (WP-23). VP-ONLY. The host
// (LibplaceboVideoRenderer.cpp, Bill's file) holds one of these in Impl and makes one call per hook
// point (docs/design/wp23-callsites.patch):
//
//   PrepareFrameAnalysis (static)  VpFrameAnalysis::Host::AttachIfEnabled(prepared.source)
//   conversion done (arrival)      frameAnalysis.OnArrival(...)
//   RequestMotionInvalidate        frameAnalysis.OnDiscontinuity(cause)   (the shared epoch)
//   UpdateQueueDelayHold           frameAnalysis.SetRingDepth(queue frames, look-ahead frames)
//   settings apply                 frameAnalysis.Configure(settings.frameAnalysisStore && !Original)
//
// Everything else (records, epochs, the shared luma decode, the worker) lives here and in
// VpFrameAnalysis / FrameAnalysisStore, tested on the CPU.

#include "VpFrameAnalysis.h"

#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>

namespace VpFrameAnalysis
{
	class Host
	{
	public:
		Host() : m_store(FrameAnalysis::RingDepth(4, 4)), m_producer(m_store) { m_store.SetEnabled(false); }
		~Host() { m_producer.Stop(); }
		Host(const Host&) = delete;
		Host& operator=(const Host&) = delete;

		// frame_analysis_store (default off until the big test; the Original pathway never runs it).
		void Configure(bool enabled)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (enabled == m_enabled) return;
			m_enabled = enabled;
			SharedFlag().store(enabled, std::memory_order_release);
			m_store.SetEnabled(enabled);
			if (enabled) m_producer.Start();
			else m_producer.Stop();
			m_lastUnique = 0;
			m_haveUnique = false;
		}

		bool Enabled() const
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			return m_enabled;
		}

		// For the static PrepareFrameAnalysis: give a freshly prepared source its (empty) shared plane
		// before it is cached and copied out, so every reader of this frame shares one decode.
		static void AttachIfEnabled(AnalysisLumaSource& source)
		{
			if (SharedFlag().load(std::memory_order_acquire))
				AttachSharedLuma(source);
		}

		// From the delay budget (queue frames; the look-ahead window walks the queue). Resizing
		// forgets the records (a new epoch), so it happens only when the depth changes.
		void SetRingDepth(int queueFrames, int lookaheadFrames)
		{
			const size_t depth = FrameAnalysis::RingDepth(queueFrames, lookaheadFrames);
			if (depth != m_store.Depth()) m_store.Resize(depth);
		}

		// A converted frame entered the queue (the conversion thread). picture: Bill's active-picture
		// decision when known (read only), else empty = the full raster. A captured repeat points at
		// the last unique frame (no re-measure). Never waits for the worker.
		void OnArrival(uint64_t seq, const AnalysisLumaSource& source, const FrameAnalysis::Rect& picture,
			int64_t captureQpc, bool captureRepeat, bool decodePlane = true)
		{
			uint64_t original = 0;
			bool repeat = false;
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				if (!m_enabled || !source.IsValid()) return;
				repeat = captureRepeat && m_haveUnique;
				original = m_lastUnique;
				if (!captureRepeat)
				{
					m_lastUnique = seq;
					m_haveUnique = true;
				}
			}
			FrameAnalysis::Identity identity;
			identity.seq = seq;
			identity.captureQpc = captureQpc;
			identity.width = source.width;
			identity.height = source.height;
			identity.encoding = static_cast<uint32_t>(source.encoding);
			identity.colorspace = static_cast<uint32_t>(source.colorspace);
			identity.range = SamplerCodesAreFullRange(source) || source.fullRangeYuv ?
				FrameAnalysis::SourceRange::Full : FrameAnalysis::SourceRange::Limited;
			identity.picture = picture.Empty() ? FrameAnalysis::Rect{ 0, 0, source.width, source.height } : picture;
			identity.sourceGeneration = source.generation;
			if (repeat)
			{
				m_store.Begin(identity, FrameAnalysis::kMeasuredGroups);
				if (m_store.MarkRepeat(seq, original))
					return;   // the original's record serves the repeat
			}
			Job job;
			job.identity = identity;
			job.source = source;
			job.decodePlane = decodePlane;
			m_producer.Submit(std::move(job));
		}

		// MotionValidity::Cause (as its integer value): the ONE discontinuity epoch, shared with the
		// vectors-valid state (vp-vector-validity).
		void OnDiscontinuity(int motionValidityCause)
		{
			m_store.Invalidate(FromMotionCause(motionValidityCause));
		}

		static FrameAnalysis::Discontinuity FromMotionCause(int cause)
		{
			using D = FrameAnalysis::Discontinuity;
			switch (cause)   // MotionValidity::Cause order
			{
			case 0: return D::SourceChange;     // Resync
			case 1: return D::QueueFlush;       // QueueReset
			case 2: return D::DroppedFrame;     // Gap
			case 3: return D::DuplicatedFrame;  // CadenceRepeat (a hard break)
			case 4: return D::DroppedFrame;     // DriftDrop
			case 7: return D::FormatChange;     // SourceFormatChange
			case 8: return D::SeekOrReset;      // Seek
			default: return D::SettingsChange;  // SmoothMotionMix, RebaseChange, MotionSourceChange, SettingsChange
			}
		}

		// "Fallbacks engaged: frame analysis (<reason>)" while the worker drops frames; else false.
		bool FallbackReason(std::string& reason) const { return m_producer.Behind(reason); }

		std::string Summary() const
		{
			std::string text = m_store.Summary();
			if (m_producer.Running())
			{
				char tail[96] = {};
				std::snprintf(tail, sizeof(tail), ", producer %llu frames (last %.1f ms, %llu dropped)",
					static_cast<unsigned long long>(m_producer.Processed()), m_producer.LastMs(),
					static_cast<unsigned long long>(m_producer.Dropped()));
				text += tail;
			}
			return text;
		}

		FrameAnalysis::Store& Store() { return m_store; }
		const FrameAnalysis::Store& Store() const { return m_store; }
		Producer& Worker() { return m_producer; }

	private:
		static std::atomic<bool>& SharedFlag()
		{
			static std::atomic<bool> flag{ false };
			return flag;
		}

		mutable std::mutex m_mutex;
		FrameAnalysis::Store m_store;
		Producer m_producer;
		bool m_enabled = false;
		uint64_t m_lastUnique = 0;
		bool m_haveUnique = false;
	};
}
