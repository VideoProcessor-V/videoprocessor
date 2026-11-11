#include <pch.h>
#include "VpFrameAnalysis.h"
#include "SourceFrameTap.h"
#include <VP-V/modules/input/source_analysis/lane/LaneCpuProducts.h>
#include <VP-V/modules/input/source_analysis/lane/shared/SourceStats.h>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace VpFrameAnalysis
{
	namespace
	{
		bool PlaneFormat(const AnalysisLumaSource& source)
		{
			return source.format == AnalysisLumaFormat::P010 || source.format == AnalysisLumaFormat::P210 ||
				source.format == AnalysisLumaFormat::DecodedDolbyVision422;
		}

		// Symmetric round-half-away-from-zero, as AnalysisLumaSource's FullLumaToLimited.
		int RoundDivide(int64_t numerator, int64_t denominator)
		{
			return static_cast<int>(numerator >= 0 ? (numerator + denominator / 2) / denominator :
				-((-numerator + denominator / 2) / denominator));
		}

		uint16_t Percentile(const std::array<uint32_t, FrameAnalysis::kHistogramBins>& histogram, uint64_t samples,
			double q)
		{
			// The smallest code whose cumulative count reaches q of the samples (at least one): the shared law of
			// U6 (VpUnit::SourceStats::FirstBinReaching).
			const double target = (std::max)(1.0, q * static_cast<double>(samples));
			const int code = VpUnit::SourceStats::FirstBinReaching<double>(histogram.data(), FrameAnalysis::kHistogramBins, target);
			if (code >= 0)
				return static_cast<uint16_t>(code);
			return static_cast<uint16_t>(FrameAnalysis::kHistogramBins - 1);
		}
	}

	bool SamplerCodesAreFullRange(const AnalysisLumaSource& source)
	{
		// WP-21 (merge 2026-10-31): the source says which domain its codes are in. VP-V
		// pathway (limitedDomain, DIV-AN-01): the sampler already mapped full-range levels to
		// studio codes; Original pathway (Bill's model): delivered full-range codes.
		return source.CodesFullRange();
	}

	std::array<uint16_t, 1024> StudioLumaMap(const AnalysisLumaSource& source)
	{
		std::array<uint16_t, 1024> map{};
		const bool full = SamplerCodesAreFullRange(source);
		for (int code = 0; code < 1024; ++code)
			map[code] = static_cast<uint16_t>(full ?
				(std::max)(0, (std::min)(1023, 64 + RoundDivide(static_cast<int64_t>(code) * 876, 1023))) : code);
		return map;
	}

	bool NeedsDecode(const AnalysisLumaSource& source)
	{
		return source.IsValid() && !(PlaneFormat(source) && !source.fullRangeYuv);
	}

	namespace
	{
		// Recycled planes (audit M1): a plane nobody else holds any more is reused, so a frame's
		// decode neither allocates nor zero-fills 2 bytes per pixel. Bounded; emptied when the store
		// is switched off (ReleasePlanePool).
		constexpr size_t kPlanePoolSize = 12;
		std::mutex& PoolMutex() { static std::mutex mutex; return mutex; }
		std::vector<std::shared_ptr<AnalysisLumaPlane>>& Pool()
		{
			static std::vector<std::shared_ptr<AnalysisLumaPlane>> pool;
			return pool;
		}

		std::shared_ptr<AnalysisLumaPlane> AcquirePlane()
		{
			std::lock_guard<std::mutex> lock(PoolMutex());
			for (const auto& plane : Pool())
				if (plane.use_count() == 1)
				{
					// Only the pool holds it: every reader has let go (their reads happen before
					// their release of the last reference).
					std::atomic_thread_fence(std::memory_order_acquire);
					plane->Recycle();
					return plane;
				}
			auto plane = std::make_shared<AnalysisLumaPlane>();
			if (Pool().size() < kPlanePoolSize)
				Pool().push_back(plane);
			return plane;
		}
	}

	void ReleasePlanePool()
	{
		std::lock_guard<std::mutex> lock(PoolMutex());
		Pool().clear();
	}

	size_t PlanePoolSize()
	{
		std::lock_guard<std::mutex> lock(PoolMutex());
		return Pool().size();
	}

	void AttachSharedLuma(AnalysisLumaSource& source)
	{
		if (!source.sharedLuma && NeedsDecode(source))
			source.sharedLuma = AcquirePlane();
	}

	bool DecodeSharedLuma(const AnalysisLumaSource& source)
	{
		AnalysisLumaPlane* plane = source.sharedLuma.get();
		if (!plane || !source.IsValid() || plane->Ready() || !plane->TryClaim())
			return false;
		const int width = source.width, height = source.height;
		plane->Bind(source);
		// resize, not assign: a recycled plane of the same size keeps its storage and is not
		// zero-filled (every element is written below).
		plane->codes.resize(static_cast<size_t>(width) * static_cast<size_t>(height));
		for (int y = 0; y < height; ++y)
		{
			uint16_t* row = plane->codes.data() + static_cast<size_t>(y) * static_cast<size_t>(width);
			if (source.SampleLumaRow(0, y, width, row))
				continue;
			for (int x = 0; x < width; ++x)
			{
				AnalysisLumaSample sample;
				if (!source.Sample(x, y, sample))
				{
					// Unreadable: never published; every reader keeps decoding for itself.
					plane->codes.clear();
					plane->codes.shrink_to_fit();
					return false;
				}
				row[x] = sample.luma;
			}
		}
		plane->Publish();
		return true;
	}

	const uint16_t* LumaRowReader::Row(int y, int x0, int count)
	{
		if (count <= 0 || x0 < 0 || y < 0 || y >= m_source.height || count > m_source.width - x0)
			return nullptr;
		if (const uint16_t* shared = m_source.SharedLumaRow(y))
			return shared + x0;
		m_buffer.resize(static_cast<size_t>(count));
		if (const uint16_t* plane = m_source.LumaPlaneRow(y))
		{
			// The lanes' upper ten bits, mapped as Sample() maps them (PlaneLuma).
			for (int i = 0; i < count; ++i)
				m_buffer[static_cast<size_t>(i)] = m_source.PlaneLuma(static_cast<uint16_t>(plane[x0 + i] >> 6));
			return m_buffer.data();
		}
		if (m_source.SampleLumaRow(x0, y, count, m_buffer.data()))
			return m_buffer.data();
		for (int i = 0; i < count; ++i)
		{
			AnalysisLumaSample sample;
			if (!m_source.Sample(x0 + i, y, sample))
				return nullptr;
			m_buffer[static_cast<size_t>(i)] = sample.luma;
		}
		return m_buffer.data();
	}

	Reducer::Reducer(const FrameAnalysis::Rect& picture, const std::array<uint16_t, 1024>& studio)
		: m_picture(picture), m_studio(studio)
	{
		const int blocks = (std::max)(0, picture.Width() / kGrainBlock);
		m_blockSum.assign(static_cast<size_t>(blocks), 0);
		m_blockSumSq.assign(static_cast<size_t>(blocks), 0);
		m_blockExcluded.assign(static_cast<size_t>(blocks), 0);
	}

	void Reducer::AddRow(int y, const uint16_t* codes)
	{
		if (!codes || y < m_picture.top || y >= m_picture.bottom || m_picture.Empty())
			return;
		const int width = m_picture.Width();
		const int blocks = static_cast<int>(m_blockSum.size());
		// The grain bands start at the picture's top; a last partial band is left out.
		const bool inBand = (y - m_picture.top) < (m_picture.Height() / kGrainBlock) * kGrainBlock;
		for (int i = 0; i < width; ++i)
		{
			const uint16_t code = m_studio[codes[i] & 0x3ff];
			++m_histogram[code];
			m_sum += code;
			const int block = i / kGrainBlock;
			if (inBand && block < blocks)
			{
				m_blockSum[static_cast<size_t>(block)] += code;
				m_blockSumSq[static_cast<size_t>(block)] += static_cast<uint64_t>(code) * code;
				if (code <= FrameAnalysis::kNearBlackMax || code >= FrameAnalysis::kClippedMin)
					m_blockExcluded[static_cast<size_t>(block)] = 1;
			}
		}
		m_samples += static_cast<uint64_t>(width);
		if (inBand && ++m_bandRows == kGrainBlock)
			FlushBand();
	}

	void Reducer::FlushBand()
	{
		constexpr double n = kGrainBlock * kGrainBlock;
		for (size_t b = 0; b < m_blockSum.size(); ++b)
		{
			++m_blocks;
			if (!m_blockExcluded[b])
			{
				const double mean = m_blockSum[b] / n;
				m_blockVariance.push_back(static_cast<float>((std::max)(0.0, m_blockSumSq[b] / n - mean * mean)));
			}
			m_blockSum[b] = m_blockSumSq[b] = 0;
			m_blockExcluded[b] = 0;
		}
		m_bandRows = 0;
	}

	Measured Reducer::Finish()
	{
		Measured out;
		FrameAnalysis::LumaStats& luma = out.luma;
		luma.histogram = m_histogram;
		luma.samples = m_samples;
		if (m_samples > 0)
		{
			luma.mean = static_cast<double>(m_sum) / static_cast<double>(m_samples);
			luma.apl = (luma.mean - FrameAnalysis::kStudioBlack) /
				(FrameAnalysis::kStudioWhite - FrameAnalysis::kStudioBlack);
			int first = 0, last = FrameAnalysis::kHistogramBins - 1;
			while (first < last && !m_histogram[first]) ++first;
			while (last > first && !m_histogram[last]) --last;
			luma.minimum = static_cast<uint16_t>(first);
			luma.maximum = static_cast<uint16_t>(last);
			luma.p1 = Percentile(m_histogram, m_samples, 0.01);
			luma.p50 = Percentile(m_histogram, m_samples, 0.50);
			luma.p99 = Percentile(m_histogram, m_samples, 0.99);
			uint64_t btb = 0, wtw = 0, nearBlack = 0, clipped = 0;
			for (int code = 0; code < FrameAnalysis::kHistogramBins; ++code)
			{
				const uint64_t count = m_histogram[code];
				if (code < FrameAnalysis::kStudioBlack) btb += count;
				if (code > FrameAnalysis::kStudioWhite) wtw += count;
				if (code <= FrameAnalysis::kNearBlackMax) nearBlack += count;
				if (code >= FrameAnalysis::kClippedMin) clipped += count;
			}
			const double total = static_cast<double>(m_samples);
			out.range.btb = btb / total;
			out.range.wtw = wtw / total;
			out.range.nearBlack = nearBlack / total;
			out.range.clipped = clipped / total;
		}
		out.grain.blocks = m_blocks;
		if (!m_blockVariance.empty())
		{
			std::vector<float> variance = m_blockVariance;
			const size_t decile = variance.size() / 10;
			std::nth_element(variance.begin(), variance.begin() + decile, variance.end());
			const double floor = variance[decile];
			out.grain.rms = std::sqrt(floor);
			// "Equally flat": within twice the flattest decile's variance (plus a quarter code^2 so a
			// clean synthetic picture is not all-or-nothing); 20% of the blocks gives full confidence.
			const double limit = 2.0 * floor + 0.25;
			uint32_t flat = 0;
			for (float v : m_blockVariance)
				flat += v <= limit ? 1u : 0u;
			out.grain.flatBlocks = flat;
			out.grain.confidence = m_blocks ? (std::min)(1.0, flat / (0.2 * m_blocks)) : 0.0;
		}
		return out;
	}

	bool Reduce(const AnalysisLumaSource& source, const FrameAnalysis::Rect& pictureIn, Measured& out)
	{
		out = Measured{};
		if (!source.IsValid()) return false;
		FrameAnalysis::Rect picture = pictureIn;
		if (picture.Empty()) picture = { 0, 0, source.width, source.height };
		picture.left = (std::max)(0, picture.left);
		picture.top = (std::max)(0, picture.top);
		picture.right = (std::min)(source.width, picture.right);
		picture.bottom = (std::min)(source.height, picture.bottom);
		if (picture.Empty()) return false;
		Reducer reducer(picture, StudioLumaMap(source));
		LumaRowReader reader(source);
		for (int y = picture.top; y < picture.bottom; ++y)
		{
			const uint16_t* row = reader.Row(y, picture.left, picture.Width());
			if (!row) return false;
			reducer.AddRow(y, row);
		}
		out = reducer.Finish();
		return true;
	}

	bool SourceViewOf(const AnalysisLumaSource& source, VpSkeleton::SourceFrameView& view)
	{
		std::shared_ptr<AnalysisLumaPlane> plane = source.sharedLuma;
		if (!plane)
		{
			AnalysisLumaSource own = source;
			own.sharedLuma = AcquirePlane();   // pooled like every plane; the view's keepAlive blocks its recycling
			DecodeSharedLuma(own);
			plane = own.sharedLuma;
		}
		const size_t samples = static_cast<size_t>((std::max)(0, source.width)) * static_cast<size_t>((std::max)(0, source.height));
		if (!plane || !plane->Ready() || samples == 0 || plane->codes.size() != samples)
			return false;
		view = VpSkeleton::SourceFrameView();
		view.luma = plane->codes.data();
		view.width = static_cast<uint32_t>(source.width);
		view.height = static_cast<uint32_t>(source.height);
		view.strideSamples = view.width;
		view.msbShift = 6;   // 10-bit sampler codes in the low bits of 16
		view.keepAlive = std::shared_ptr<const void>(plane);
		return true;
	}

	bool P1EngineViewDiffersFromProducer(const AnalysisLumaSource& source)
	{
		// Mirrors MotionAheadProducer.cpp HalfLuma8's planar16 / rgbOwnDecode / full-range 4:2:2 split (the producer's own
		// codes): planar P010 / P210 at limited range equal the shared decode's codes; at full range they are raw words.
		const VideoFrameEncoding e = source.encoding;
		if (source.format == AnalysisLumaFormat::NativeRgb)
			return e == VideoFrameEncoding::R12B || e == VideoFrameEncoding::R12L || e == VideoFrameEncoding::R210 ||
				e == VideoFrameEncoding::R10b || e == VideoFrameEncoding::R10l || e == VideoFrameEncoding::BGRA_8BIT ||
				e == VideoFrameEncoding::ARGB_8BIT;
		if (source.format == AnalysisLumaFormat::P010 || source.format == AnalysisLumaFormat::P210 ||
			source.format == AnalysisLumaFormat::NativeYuv422)
			return source.fullRangeYuv;
		return false;
	}

	bool LiveEngine8(const AnalysisLumaSource& source, std::vector<uint8_t>& out, int& hw, int& hh)
	{
		DecodeSharedLuma(source);   // the frame's one shared decode when nobody has done it (no-op when published / claimed / none)
		VpSkeleton::SourceFrameView view;
		if (!SourceViewOf(source, view) || view.msbShift != 6)   // Engine8Half is exact for 10-bit views only
			return false;
		const uint32_t w = VpLaneProducts::HalfDim(view.width);
		const uint32_t h = VpLaneProducts::HalfDim(view.height);
		std::vector<uint16_t> half(static_cast<size_t>(w) * h);
		VpLaneProducts::HalfLumaRows(view, half.data(), 0, h);
		out.resize(half.size());
		VpLaneProducts::Engine8Half(half.data(), w, h, out.data());
		hw = static_cast<int>(w);
		hh = static_cast<int>(h);
		return true;
	}

	bool P1Reader::ReadEngine8(uint64_t seq, const AnalysisLumaSource& source, std::vector<uint8_t>& out, int& hw, int& hh,
		VpSkeleton::P1ReadKind& how)
	{
		how = VpSkeleton::P1ReadKind::None;
		if (m_published && source.width > 0 && source.height > 0)
		{
			const uint32_t w = VpLaneProducts::HalfDim(static_cast<uint32_t>(source.width));
			const uint32_t h = VpLaneProducts::HalfDim(static_cast<uint32_t>(source.height));
			out.resize(static_cast<size_t>(w) * h);
			const VpSkeleton::PublishedRead r = m_published(seq, static_cast<uint32_t>(source.width), static_cast<uint32_t>(source.height), out.data());
			if (r == VpSkeleton::PublishedRead::Copied)
			{
				hw = static_cast<int>(w);
				hh = static_cast<int>(h);
				how = VpSkeleton::P1ReadKind::LanePublished;
				m_lanePublished.fetch_add(1, std::memory_order_relaxed);
				return true;
			}
			if (r == VpSkeleton::PublishedRead::Recycled)
				m_seqlockRetries.fetch_add(1, std::memory_order_relaxed);
		}
		if (LiveEngine8(source, out, hw, hh))
		{
			how = VpSkeleton::P1ReadKind::LiveFill;
			m_filledByMotion.fetch_add(1, std::memory_order_relaxed);
			return true;
		}
		m_unread.fetch_add(1, std::memory_order_relaxed);
		return false;
	}

	namespace
	{
		// Hands the decoded luma plane to the skeleton's sink (SourceFrameTap.h): SourceViewOf's view (a format read in place
		// is decoded into a pooled plane here, only because a sink asked; the view's keepAlive blocks its recycling).
		void DeliverToTap(const Job& job)
		{
			VpSkeleton::SourceFrameView view;
			if (!SourceViewOf(job.source, view))
			{
				SourceFrameTap::NoteSkipped(job.owner);
				return;
			}
			view.seq = job.identity.seq;
			SourceFrameTap::Deliver(job.owner, view);
		}
	}

	void Process(FrameAnalysis::Store& store, const Job& job)
	{
		if (job.decodePlane)
			DecodeSharedLuma(job.source);
		if (job.owner && SourceFrameTap::Wants(job.owner))
			DeliverToTap(job);
		if (!job.measure)
			return;
		Measured measured;
		if (Reduce(job.source, job.identity.picture, measured))
			store.PublishMeasured(job.identity.seq, FrameAnalysis::kMeasuredGroups, measured.luma, measured.range,
				measured.grain);
	}

	void Producer::Start()
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_thread.joinable()) return;
		m_stop = false;
		m_running.store(true, std::memory_order_release);
		m_thread = std::thread([this] { Run(); });
	}

	void Producer::Stop()
	{
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_stop = true;
		}
		m_wake.notify_all();
		if (m_thread.joinable()) m_thread.join();
		m_running.store(false, std::memory_order_release);
		std::lock_guard<std::mutex> lock(m_mutex);
		m_jobs.clear();   // queued frames release their buffers; their records stay late (visible)
	}

	void Producer::Submit(Job job)
	{
		const uint32_t groups = job.measure ? FrameAnalysis::kMeasuredGroups : 0u;
		if (!m_store.Begin(job.identity, groups))
			return;   // store off
		const size_t capacity = m_store.Depth();
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (m_jobs.size() >= capacity)
			{
				m_jobs.pop_front();
				m_dropped.fetch_add(1, std::memory_order_relaxed);
				m_cleanSinceDrop = 0;
			}
			else
				++m_cleanSinceDrop;
			m_jobs.push_back(std::move(job));
		}
		m_wake.notify_one();
	}

	void Producer::Drain()
	{
		for (;;)
		{
			Job job;
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				if (m_jobs.empty()) return;
				job = std::move(m_jobs.front());
				m_jobs.pop_front();
			}
			Process(m_store, job);
			m_processed.fetch_add(1, std::memory_order_relaxed);
		}
	}

	bool Producer::Behind(std::string& reason) const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		const uint64_t dropped = m_dropped.load(std::memory_order_relaxed);
		// Behind until a ring's worth of frames went through without a drop.
		if (dropped == 0 || m_cleanSinceDrop >= m_store.Depth())
			return false;
		reason = "producer behind: " + std::to_string(dropped) + " frame(s) not analysed, stages measure themselves";
		return true;
	}

	void Producer::Run()
	{
		for (;;)
		{
			Job job;
			{
				std::unique_lock<std::mutex> lock(m_mutex);
				m_wake.wait(lock, [this] { return m_stop || !m_jobs.empty(); });
				if (m_stop) return;
				job = std::move(m_jobs.front());
				m_jobs.pop_front();
			}
			const auto started = std::chrono::steady_clock::now();
			Process(m_store, job);
			m_lastMs.store(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count(),
				std::memory_order_relaxed);
			m_processed.fetch_add(1, std::memory_order_relaxed);
		}
	}
}
