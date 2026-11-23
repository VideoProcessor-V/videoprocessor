#include <pch.h>
#include "VpFrameAnalysis.h"
#include "SourceFrameTap.h"
#include <VP-V/modules/input/signal/DvTunnelTapLaw.h>
#include <VP-V/modules/input/source_analysis/AnalysisAxis10.h>
#include <VP-V/modules/input/source_analysis/lane/LaneCpuProducts.h>
#include <VP-V/modules/input/source_analysis/lane/shared/SourceStats.h>
#include <VP-V/core/RobustStats.h>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace VpFrameAnalysis
{
	namespace
	{
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

		constexpr uint32_t kQ6PerCode = 1u << Vpv::AnalysisDomain::kAxisFractionBits;    // 64
		constexpr uint32_t kQ6Values = FrameAnalysis::kHistogramBins * kQ6PerCode;       // 65536: every 16-bit value

		// A whole-code map (0..1023 -> 0..1023) carried onto the Q6 axis: value c x 64 + f maps to
		// map(c) x 64 + (map(c + 1) - map(c)) x f, i.e. linear between the whole codes and exact at each of them
		// (f = 0 gives map(c) x 64, the ten-bit result). Integer, no narrowing: the fraction f is carried.
		template <class Map>
		void BuildQ6Map(std::vector<uint16_t>& out, Map map)
		{
			out.assign(kQ6Values, 0);
			const int top = static_cast<int>(FrameAnalysis::kHistogramBins) - 1;
			for (int code = 0; code <= top; ++code)
			{
				const int lo = static_cast<int>(map(code));
				const int hi = static_cast<int>(map(code < top ? code + 1 : code));
				for (int f = 0; f < static_cast<int>(kQ6PerCode); ++f)
				{
					const int v = lo * static_cast<int>(kQ6PerCode) + (hi - lo) * f;
					out[static_cast<size_t>(code) * kQ6PerCode + static_cast<size_t>(f)] =
						static_cast<uint16_t>((std::max)(0, (std::min)(static_cast<int>(kQ6Values) - 1, v)));
				}
			}
		}
	}

	// 15c C6: the range question (CodesFullRange), the studio map, the need for a plane and the plane's decode are the
	// decode owner's (AnalysisLumaDomain.h, Vpv::AnalysisDomain); this producer only pools the planes and reads them.

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
		if (!source.sharedLuma && Vpv::AnalysisDomain::NeedsLumaPlane(source))
			source.sharedLuma = AcquirePlane();
	}

	const uint16_t* LumaRowReader::RowQ6(int y, int x0, int count)
	{
		if (count <= 0 || x0 < 0 || y < 0 || y >= m_source.height || count > m_source.width - x0)
			return nullptr;
		if (const uint16_t* plane = m_source.LumaPlaneRow(y))
		{
			// The lanes are on the 16-bit lane scale = Q6. P010 / P210 are ten-bit containers: their low six bits are padding
			// (zero by the format; gate cb-chk r1 SHOULD 1), so they are masked off and never read as a fraction. Only the
			// decoded DV plane (12-bit codes, code12 << 4) carries real fraction bits.
			const bool tenBitLanes = m_source.format != AnalysisLumaFormat::DecodedDolbyVision422;
			const uint16_t laneMask = tenBitLanes ? static_cast<uint16_t>(0xFFC0u) : static_cast<uint16_t>(0xFFFFu);
			// A limited plane that needs no decode (NeedsDecode false) with real fraction bits is read in place.
			if (!NeedsDecode(m_source) && !tenBitLanes)
				return plane + x0;
			m_buffer.resize(static_cast<size_t>(count));
			if (!NeedsDecode(m_source))
			{
				for (int i = 0; i < count; ++i)
					m_buffer[static_cast<size_t>(i)] = static_cast<uint16_t>(plane[x0 + i] & laneMask);
				return m_buffer.data();
			}
			// Full range: Sample()'s remap (PlaneLuma, the source's own law) carried onto the Q6 axis.
			if (m_planeQ6.empty())
				BuildQ6Map(m_planeQ6, [this](int code) { return m_source.PlaneLuma(static_cast<uint16_t>(code)); });
			for (int i = 0; i < count; ++i)
				m_buffer[static_cast<size_t>(i)] = m_planeQ6[plane[x0 + i] & laneMask];
			return m_buffer.data();
		}
		// The ten-bit sources (Sample().luma): the shared plane, the row decoder, else the per-pixel sampler. Carried as
		// code x 64, the exact Q6 of a whole axis code (AnalysisAxis10.h: Q6 = code << 6 for a 10-bit code).
		m_buffer.resize(static_cast<size_t>(count));
		const uint16_t* codes = m_source.SharedLumaRow(y);
		if (codes)
			codes += x0;
		else
		{
			if (!m_source.SampleLumaRow(x0, y, count, m_buffer.data()))
				for (int i = 0; i < count; ++i)
				{
					AnalysisLumaSample sample;
					if (!m_source.Sample(x0 + i, y, sample))
						return nullptr;
					m_buffer[static_cast<size_t>(i)] = sample.luma;
				}
			codes = m_buffer.data();
		}
		for (int i = 0; i < count; ++i)
			m_buffer[static_cast<size_t>(i)] = static_cast<uint16_t>((codes[i] & Vpv::AnalysisDomain::kAxisMax)
				<< Vpv::AnalysisDomain::kAxisFractionBits);
		return m_buffer.data();
	}

	Reducer::Reducer(const FrameAnalysis::Rect& picture, const std::array<uint16_t, 1024>& studio)
		: m_picture(picture)
	{
		BuildQ6Map(m_studioQ6, [&studio](int code) { return studio[static_cast<size_t>(code)]; });
		m_q6Histogram.assign(kQ6Values, 0u);
	}

	void Reducer::AddRowQ6(int y, const uint16_t* q6)
	{
		if (!q6 || y < m_picture.top || y >= m_picture.bottom || m_picture.Empty())
			return;
		const int width = m_picture.Width();
		for (int i = 0; i < width; ++i)
		{
			const uint32_t v = m_studioQ6[q6[i]];
			++m_q6Histogram[v];
			m_sumQ6 += v;
		}
		m_samples += static_cast<uint64_t>(width);
	}

	Measured Reducer::Finish()
	{
		Measured out;
		FrameAnalysis::LumaStats& luma = out.luma;
		// The Store's histogram: one bin per whole studio code, the total of its 64 Q6 values.
		std::array<uint32_t, FrameAnalysis::kHistogramBins>& histogram = luma.histogram;
		for (size_t code = 0; code < histogram.size(); ++code)
		{
			uint32_t total = 0;
			const uint32_t* q = m_q6Histogram.data() + code * kQ6PerCode;
			for (uint32_t f = 0; f < kQ6PerCode; ++f)
				total += q[f];
			histogram[code] = total;
		}
		luma.samples = m_samples;
		if (m_samples > 0)
		{
			// Exact on the Q6 axis (/ 64 is exact, so a ten-bit-origin picture gives the code-domain mean bit for bit).
			luma.mean = static_cast<double>(m_sumQ6) / static_cast<double>(m_samples) / static_cast<double>(kQ6PerCode);
			luma.apl = (luma.mean - FrameAnalysis::kStudioBlack) /
				(FrameAnalysis::kStudioWhite - FrameAnalysis::kStudioBlack);
			int first = 0, last = FrameAnalysis::kHistogramBins - 1;
			while (first < last && !histogram[first]) ++first;
			while (last > first && !histogram[last]) --last;
			luma.minimum = static_cast<uint16_t>(first);
			luma.maximum = static_cast<uint16_t>(last);
			luma.p1 = Percentile(histogram, m_samples, 0.01);
			luma.p50 = Percentile(histogram, m_samples, 0.50);
			luma.p99 = Percentile(histogram, m_samples, 0.99);
			// The shares compare the exact Q6 value with each threshold's 10-bit meaning (64.0, 940.0, 81.0, 1019.0).
			const uint32_t blackQ6 = static_cast<uint32_t>(FrameAnalysis::kStudioBlack) * kQ6PerCode;
			const uint32_t whiteQ6 = static_cast<uint32_t>(FrameAnalysis::kStudioWhite) * kQ6PerCode;
			const uint32_t nearBlackQ6 = static_cast<uint32_t>(FrameAnalysis::kNearBlackMax) * kQ6PerCode;
			const uint32_t clippedQ6 = static_cast<uint32_t>(FrameAnalysis::kClippedMin) * kQ6PerCode;
			uint64_t btb = 0, wtw = 0, nearBlack = 0, clipped = 0;
			for (uint32_t v = 0; v < kQ6Values; ++v)
			{
				const uint64_t count = m_q6Histogram[v];
				if (count == 0) continue;
				if (v < blackQ6) btb += count;
				if (v > whiteQ6) wtw += count;
				if (v <= nearBlackQ6) nearBlack += count;
				if (v >= clippedQ6) clipped += count;
			}
			const double total = static_cast<double>(m_samples);
			out.range.btb = btb / total;
			out.range.wtw = wtw / total;
			out.range.nearBlack = nearBlack / total;
			out.range.clipped = clipped / total;
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
			const uint16_t* row = reader.RowQ6(y, picture.left, picture.Width());
			if (!row) return false;
			reducer.AddRowQ6(y, row);
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

	bool SourceViewOf12(const AnalysisLumaSource& source, VpSkeleton::SourceFrameView& view)
	{
		if (source.format != AnalysisLumaFormat::PackedDolbyVision422 || !source.IsValid())
			return false;
		auto codes = std::make_shared<std::vector<uint16_t>>(static_cast<size_t>(source.width) * static_cast<size_t>(source.height));
		for (int y = 0; y < source.height; ++y)
			for (int x = 0; x < source.width; ++x)
				if (!DvTunnelTapLaw::SourceTap12(source, x, y, (*codes)[static_cast<size_t>(y) * source.width + x]))
					return false;
		view = VpSkeleton::SourceFrameView();
		view.luma = codes->data();
		view.width = static_cast<uint32_t>(source.width);
		view.height = static_cast<uint32_t>(source.height);
		view.strideSamples = view.width;
		view.msbShift = 4;   // 12-bit tap codes in the low bits of 16
		view.keepAlive = std::shared_ptr<const void>(codes);
		return true;
	}

	bool P1EngineViewDiffersFromProducer(const AnalysisLumaSource&)
	{
		// Retired (MAP-RGB-LUMA-PROXY, OD 11-15 11:11 (3)): the producer's HalfLuma8 has no private luma law any more, so no
		// format's engine view differs from the producer's. Kept (always false) only until the tests that pin the old
		// exception are rewritten by the opus lane; then this declaration goes.
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
			store.PublishMeasured(job.identity.seq, FrameAnalysis::kMeasuredGroups, measured.luma, measured.range);
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
