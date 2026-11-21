#include <pch.h>
#include "VpFrameAnalysis.h"
#include "SourceFrameTap.h"
#include <VP-V/modules/input/signal/DvTunnelTapLaw.h>
#include <VP-V/modules/input/source_analysis/AnalysisAxis10.h>
#include <VP-V/modules/input/source_analysis/lane/LaneCpuProducts.h>
#include <VP-V/modules/input/source_analysis/lane/shared/SourceStats.h>
#include <VP-V/core/RobustStats.h>
#include <VP-V/core/ComputeOnce.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <map>
#include <new>

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

		// W21 FIN (rule 1): the frame's one 12-bit decode, kept for the frame's other reader (the source-resolution look-ahead walk
		// reads a queued frame some frames after its arrival). Keyed by sequence AND the frame's bytes and layout, so a recycled buffer
		// or a restarted sequence is never mistaken for the frame. Half the plane pool at most, so it cannot pin every pooled plane; a
		// reader that misses reads its own samples (counted).
		constexpr size_t kSharedViewDepth = kPlanePoolSize / 2;
		struct SharedView12
		{
			uint64_t seq = 0;
			const uint8_t* data = nullptr;
			size_t rowBytes = 0;
			int width = 0, height = 0, alignment = 0;
			VideoFrameEncoding encoding{};
			bool mapsToLimited = false;
			VpSkeleton::SourceFrameView view;
		};
		std::mutex& SharedViewMutex() { static std::mutex mutex; return mutex; }
		// One bounded set PER HOST (the renderer key of Host::Owner; nullptr = the unowned set tests and ownerless callers use), so two
		// hosts with interleaved sequences never clear or read each other's views. Guarded by SharedViewMutex.
		std::map<const void*, std::vector<SharedView12>>& SharedViewsByHost()
		{
			static std::map<const void*, std::vector<SharedView12>> views;
			return views;
		}
		bool SameFrame(const SharedView12& e, uint64_t seq, const AnalysisLumaSource& s)
		{
			return e.seq == seq && e.data == s.data && e.rowBytes == s.rowBytes && e.width == s.width && e.height == s.height &&
				e.alignment == s.rgbBitAlignment && e.encoding == s.encoding && e.mapsToLimited == s.MapsToLimitedDomain();
		}
	}

	void ReleasePlanePool(const void* host)
	{
		{
			std::lock_guard<std::mutex> lock(SharedViewMutex());
			if (host)
				SharedViewsByHost().erase(host);   // this host's kept 12-bit views hold pooled planes; another host's stay
			else
				SharedViewsByHost().clear();
		}
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
				m_buffer[static_cast<size_t>(i)] = m_source.PlaneLuma(static_cast<uint16_t>(Vpv::AnalysisDomain::Floor10FromQ6(plane[x0 + i])));
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
	}

	void Reducer::AddRow(int y, const uint16_t* codes)
	{
		if (!codes || y < m_picture.top || y >= m_picture.bottom || m_picture.Empty())
			return;
		const int width = m_picture.Width();
		for (int i = 0; i < width; ++i)
		{
			const uint16_t code = m_studio[codes[i] & 0x3ff];
			++m_histogram[code];
			m_sum += code;
		}
		m_samples += static_cast<uint64_t>(width);
	}

	void Reducer::AddRowWide(int y, const uint16_t* codes, const VpSkeleton::SourceFrameView& lane)
	{
		if (!codes || y < m_picture.top || y >= m_picture.bottom || m_picture.Empty())
			return;
		m_wide = true;
		const int width = m_picture.Width();
		for (int i = 0; i < width; ++i)
		{
			const uint32_t q6 = lane.MsbAlign(codes[i]);   // the one MSB-align law: the lane value is the Q6 axis value
			++m_histogram[Vpv::AnalysisDomain::HistBinFromQ6(q6)];
			m_sumQ6 += q6;
			if (q6 < m_minQ6) m_minQ6 = q6;
			if (q6 > m_maxQ6) m_maxQ6 = q6;
		}
		m_samples += static_cast<uint64_t>(width);
	}

	Measured Reducer::Finish()
	{
		Measured out;
		FrameAnalysis::LumaStats& luma = out.luma;
		luma.histogram = m_histogram;
		luma.samples = m_samples;
		if (m_samples > 0)
		{
			luma.mean = m_wide ?
				static_cast<double>(m_sumQ6) / static_cast<double>(1u << Vpv::AnalysisDomain::kAxisFractionBits) / static_cast<double>(m_samples) :
				static_cast<double>(m_sum) / static_cast<double>(m_samples);
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
			// Q6 companions: integer-code sources are exactly code * 64; the wide tap keeps its sub-code min / max / mean.
			luma.meanQ6 = luma.mean * static_cast<double>(1u << Vpv::AnalysisDomain::kAxisFractionBits);
			if (m_wide)
			{
				luma.minQ6 = m_minQ6;
				luma.maxQ6 = m_maxQ6;
				luma.p1Q6 = Vpv::AnalysisDomain::BinCenterQ6(luma.p1);
				luma.p50Q6 = Vpv::AnalysisDomain::BinCenterQ6(luma.p50);
				luma.p99Q6 = Vpv::AnalysisDomain::BinCenterQ6(luma.p99);
			}
			else
			{
				luma.minQ6 = static_cast<uint32_t>(luma.minimum) << Vpv::AnalysisDomain::kAxisFractionBits;
				luma.maxQ6 = static_cast<uint32_t>(luma.maximum) << Vpv::AnalysisDomain::kAxisFractionBits;
				luma.p1Q6 = static_cast<uint32_t>(luma.p1) << Vpv::AnalysisDomain::kAxisFractionBits;
				luma.p50Q6 = static_cast<uint32_t>(luma.p50) << Vpv::AnalysisDomain::kAxisFractionBits;
				luma.p99Q6 = static_cast<uint32_t>(luma.p99) << Vpv::AnalysisDomain::kAxisFractionBits;
			}
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
		return out;
	}

	namespace
	{
		bool StudioMapIsIdentity(const std::array<uint16_t, 1024>& studio)
		{
			for (size_t code = 0; code < studio.size(); ++code)
				if (studio[code] != static_cast<uint16_t>(code))
					return false;
			return true;
		}

		// The full -> limited law at 12 bits (DvTunnelTapLaw::FullLumaToLimited12, the GPU's range table), built once.
		const uint16_t* FullToLimited12()
		{
			struct Table
			{
				uint16_t codes[DvTunnelTapLaw::kLevels];
				Table() { DvTunnelTapLaw::BuildRangeTable(codes); }
			};
			static const Table table;
			return table.codes;
		}

		bool Is12BitViewOf(const VpSkeleton::SourceFrameView* view, const AnalysisLumaSource& source)
		{
			return view && view->luma && view->msbShift == DvTunnelTapLaw::kLaneShift &&
				static_cast<int>(view->width) == source.width && static_cast<int>(view->height) == source.height;
		}

		// `decodeOwn` false (Process): no 12-bit view means the frame's one decode was refused (counted in SourceViewOf12), so no
		// second attempt is made and the 10-bit read is the visible fallback. True (direct callers): decode here when not given.
		bool ReduceWith(const AnalysisLumaSource& source, const FrameAnalysis::Rect& pictureIn, Measured& out,
			const VpSkeleton::SourceFrameView* tap12, bool decodeOwn)
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
			const std::array<uint16_t, 1024> studio = StudioLumaMap(source);
			Reducer reducer(picture, studio);
			// W21-2: a valid packed DV tunnel source reads its 12-bit tap (the lane's TapRow12 law; IsValid() implies it, DvTunnelTapLaw.h),
			// so the percentiles / mean / min / max carry the sub-code part; the histogram stays on the 1024-code axis (HistBinFromQ6).
			if (source.format == AnalysisLumaFormat::PackedDolbyVision422)
			{
				VpSkeleton::SourceFrameView own;
				const VpSkeleton::SourceFrameView* view = Is12BitViewOf(tap12, source) ? tap12 : nullptr;
				assert((!tap12 || view) && "Reduce: the shared 12-bit view is not this frame's");
				if (!view && decodeOwn && SourceViewOf12(source, own))
					view = &own;
				if (view)
				{
					// The studio map at 12 bits: the tap is already limited for a frame flagged full range (TapRow12 converts it), so the
					// map is the identity; a frame whose codes are full range by its range tag alone gets the same full -> limited law
					// the 10-bit read applies, at 12 bits.
					const uint16_t* map12 = StudioMapIsIdentity(studio) ? nullptr : FullToLimited12();
					std::vector<uint16_t> mapped(map12 ? static_cast<size_t>(picture.Width()) : 0u);
					for (int y = picture.top; y < picture.bottom; ++y)
					{
						const uint16_t* row = view->luma + static_cast<size_t>(y) * view->strideSamples + picture.left;
						if (map12)
						{
							for (size_t i = 0; i < mapped.size(); ++i)
								mapped[i] = map12[row[i] & (DvTunnelTapLaw::kLevels - 1)];
							row = mapped.data();
						}
						reducer.AddRowWide(y, row, *view);
					}
					out = reducer.Finish();
					return true;
				}
				// Only a refused 12-bit decode reaches here (counted in SourceViewOf12, Ctrl+I line): the 10-bit read below.
			}
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
	}

	bool Reduce(const AnalysisLumaSource& source, const FrameAnalysis::Rect& picture, Measured& out,
		const VpSkeleton::SourceFrameView* tap12)
	{
		return ReduceWith(source, picture, out, tap12, true);
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

	namespace
	{
		std::atomic<uint64_t>& DvRefusalCounter() { static std::atomic<uint64_t> n{ 0 }; return n; }
	}

	uint64_t DvTapRefusals() { return DvRefusalCounter().load(std::memory_order_relaxed); }
	void ResetDvTapRefusals() { DvRefusalCounter().store(0, std::memory_order_relaxed); }

	void NoteDvTapRefusal() { DvRefusalCounter().fetch_add(1, std::memory_order_relaxed); }   // visible: Ctrl+I skeleton_lane line, never log-only

	bool SourceViewOf12(const AnalysisLumaSource& source, VpSkeleton::SourceFrameView& view)
	{
		if (source.format != AnalysisLumaFormat::PackedDolbyVision422 || !source.IsValid())
			return false;   // not a packed DV tunnel frame, or not a readable one: nothing to tap (not a refusal)
		// INVARIANT (DvTunnelTapLaw.h): IsValid() implies the 12-bit law's Supports() for a packed DV frame, so a valid frame is never
		// refused by the law. Should a merge of Bill's IsValid() ever widen it, a debug build stops here and a release build refuses the
		// frame through the one visible refusal (counted, Ctrl+I) instead of mis-decoding it.
		const bool lawCovers = DvTunnelTapLaw::Supports(source.encoding, source.rgbBitAlignment, source.width);
		assert(lawCovers && "AnalysisLumaSource::IsValid() must imply DvTunnelTapLaw::Supports() for packed DV");
		if (!lawCovers)
		{
			NoteDvTapRefusal();
			return false;
		}
		// Row-wise tap (the container checks hoisted out of the per-pixel law) into a pooled plane: no allocation per frame. A full pool
		// gives an unpooled plane (AcquirePlane), so the one reachable refusal is a plane that cannot be allocated at all.
		const size_t samples = static_cast<size_t>(source.width) * static_cast<size_t>(source.height);
		std::shared_ptr<AnalysisLumaPlane> plane;
		try
		{
			plane = AcquirePlane();
			plane->codes.resize(samples);
		}
		catch (const std::bad_alloc&)
		{
			NoteDvTapRefusal();   // no memory for the 12-bit plane: the caller's 10-bit view is the visible fallback
			return false;
		}
		plane->width = source.width;
		plane->height = source.height;
		for (int y = 0; y < source.height; ++y)
			DvTunnelTapLaw::SourceTapRow12(source, y, plane->codes.data() + static_cast<size_t>(y) * static_cast<size_t>(source.width));
		VPV_COMPUTE_ONCE("analysis.tap12", DvTunnelTapLaw::kBits);   // THE 12-bit decode, counted here whoever calls (rule 1)
		view = VpSkeleton::SourceFrameView();
		view.luma = plane->codes.data();
		view.width = static_cast<uint32_t>(source.width);
		view.height = static_cast<uint32_t>(source.height);
		view.strideSamples = view.width;
		view.msbShift = static_cast<uint8_t>(DvTunnelTapLaw::kLaneShift);   // MsbAlign(code12) is the Q6 axis value
		view.keepAlive = std::shared_ptr<const void>(plane);
		return true;
	}

	bool ShareViewOf12(uint64_t seq, const AnalysisLumaSource& source, VpSkeleton::SourceFrameView& view, const void* host)
	{
		if (!SourceViewOf12(source, view))
			return false;
		if (seq == 0)
			return true;   // no frame identity to key it by: not kept
		SharedView12 kept;
		kept.seq = seq;
		kept.data = source.data;
		kept.rowBytes = source.rowBytes;
		kept.width = source.width;
		kept.height = source.height;
		kept.alignment = source.rgbBitAlignment;
		kept.encoding = source.encoding;
		kept.mapsToLimited = source.MapsToLimitedDomain();
		kept.view = view;
		std::lock_guard<std::mutex> lock(SharedViewMutex());
		std::vector<SharedView12>& views = SharedViewsByHost()[host];
		if (!views.empty() && seq <= views.back().seq)
			views.clear();   // the sequence did not advance (a new session or a repeated frame): start again
		if (views.size() >= kSharedViewDepth)
			views.erase(views.begin());
		views.push_back(std::move(kept));
		return true;
	}

	bool FindSharedViewOf12(uint64_t seq, const AnalysisLumaSource& source, VpSkeleton::SourceFrameView& view, const void* host)
	{
		if (seq == 0 || source.format != AnalysisLumaFormat::PackedDolbyVision422)
			return false;
		std::lock_guard<std::mutex> lock(SharedViewMutex());
		const auto set = SharedViewsByHost().find(host);
		if (set == SharedViewsByHost().end())
			return false;
		for (const SharedView12& kept : set->second)
			if (SameFrame(kept, seq, source))
			{
				view = kept.view;
				return true;
			}
		return false;
	}

	bool TapViewOf(const AnalysisLumaSource& source, VpSkeleton::SourceFrameView& view, bool* refused)
	{
		if (refused) *refused = false;
		if (source.format == AnalysisLumaFormat::PackedDolbyVision422 && source.IsValid())
		{
			if (SourceViewOf12(source, view))
				return true;
			if (refused) *refused = true;   // refused and counted in SourceViewOf12: the 10-bit view below, visibly
		}
		return SourceViewOf(source, view);
	}

	bool P1EngineViewDiffersFromProducer(const AnalysisLumaSource& source)
	{
		// Mirrors MotionAheadProducer.cpp HalfLuma8's planar16 / rgbOwnDecode / full-range 4:2:2 split (the producer's own
		// codes): planar P010 / P210 at limited range equal the shared decode's codes; at full range they are raw words.
		// 15c C6: for these YCbCr formats the producer's raw words differ exactly when the decode owner maps the codes
		// (MapsToLimitedDomain, which for a non-RGB format is the fullRangeYuv flag): asked of the owner, not re-derived.
		const VideoFrameEncoding e = source.encoding;
		if (source.format == AnalysisLumaFormat::NativeRgb)
			return e == VideoFrameEncoding::R12B || e == VideoFrameEncoding::R12L || e == VideoFrameEncoding::R210 ||
				e == VideoFrameEncoding::R10b || e == VideoFrameEncoding::R10l || e == VideoFrameEncoding::BGRA_8BIT ||
				e == VideoFrameEncoding::ARGB_8BIT;
		if (source.format == AnalysisLumaFormat::P010 || source.format == AnalysisLumaFormat::P210 ||
			source.format == AnalysisLumaFormat::NativeYuv422)
			return source.MapsToLimitedDomain();
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
		// is decoded into a pooled plane here, only because a sink asked; the view's keepAlive blocks its recycling). A valid packed DV
		// tunnel source delivers the 12-bit tap: `view12`, the frame's single 12-bit decode made by Process, delivered as is (the same
		// law as TapViewOf). Without it (any other source, or a DV decode refused and counted in SourceViewOf12) the 10-bit view.
		void DeliverToTap(const Job& job, const VpSkeleton::SourceFrameView* view12)
		{
			VpSkeleton::SourceFrameView view;
			if (view12)
				view = *view12;
			else if (!SourceViewOf(job.source, view))
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
		// Every compute-once note below and inside (SourceViewOf12's "analysis.tap12") is on this job's frame.
		::ComputeOnce::FrameScope frame(static_cast<int64_t>(job.identity.seq), "frame-analysis", 0);
		const bool dvSource = job.source.format == AnalysisLumaFormat::PackedDolbyVision422;
		// The retained 10-bit DV decode (the shared plane Bill's readers use), its own counted unit; counted only when it really ran.
		if (job.decodePlane && DecodeSharedLuma(job.source) && dvSource)
			VPV_COMPUTE_ONCE_AT("analysis.luma10", job.identity.seq, Vpv::AnalysisDomain::kAxisBits);
		const bool wantTap = job.owner && SourceFrameTap::Wants(job.owner);
		VpSkeleton::SourceFrameView view12;
		bool have12 = false;
		// ONE 12-bit decode per frame (rule 1), shared by the tap, the measure and the look-ahead walk (ShareViewOf12). IsValid() implies
		// the 12-bit law, so the only refusal is an unallocatable plane, counted in SourceViewOf12; the 10-bit read is then its fallback.
		if (dvSource && job.source.IsValid() && (wantTap || job.measure))
			have12 = ShareViewOf12(job.identity.seq, job.source, view12, job.host);
		if (wantTap)
			DeliverToTap(job, have12 ? &view12 : nullptr);
		if (!job.measure)
			return;
		Measured measured;
		if (ReduceWith(job.source, job.identity.picture, measured, have12 ? &view12 : nullptr, false))
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
