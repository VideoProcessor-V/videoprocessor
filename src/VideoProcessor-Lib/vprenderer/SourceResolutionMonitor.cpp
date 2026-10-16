#include <pch.h>
#include "SourceResolutionMonitor.h"

#include "../DebugLog.h"

#include <libplacebo/dispatch.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace SourceResolutionMonitor
{
	namespace
	{
		constexpr int kPixels = Monitor::kTile * Monitor::kTile;

		// One invocation per tile pixel: the native luma (or Rec. 709 luma of
		// an RGB plane) at the tile's origin, written as a float.
		const char* const BODY = R"GLSL(
ivec2 g = ivec2(gl_GlobalInvocationID.xy);
int tile = g.x / 256;
int x = g.x - tile * 256;
if (tile < int(tiles) && g.y < 256) {
    vec4 pair = tile < 2 ? o01 : tile < 4 ? o23 : tile < 6 ? o45 : o67;
    vec2 origin = (tile % 2) == 0 ? pair.xy : pair.zw;
    ivec2 p = clamp(ivec2(origin) + ivec2(x, g.y), ivec2(0), textureSize(src, 0) - ivec2(1));
    vec4 c = texelFetch(src, p, 0);
    values[(tile * 256 + g.y) * 256 + x] = is_rgb > 0.5 ? dot(c.rgb, vec3(0.2126, 0.7152, 0.0722)) : c.r;
}
)GLSL";

		// Local wall-clock time for the history rows.
		std::string Clock()
		{
			SYSTEMTIME t{};
			GetLocalTime(&t);
			char text[32];
			snprintf(text, sizeof(text), "%02u:%02u:%02u.%03u", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
			return text;
		}

		double Median(std::vector<double> v)
		{
			if (v.empty()) return 1.0;
			std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
			return v[v.size() / 2];
		}
	}

	Monitor::Monitor()
	{
		worker = std::thread([this] { Worker(); });
		aheadThread = std::thread([this] { AheadWorker(); });
	}

	Monitor::~Monitor()
	{
		{
			std::lock_guard<std::mutex> lock(mutex);
			stopping = true;
		}
		wake.notify_all();
		if (worker.joinable()) worker.join();
		{
			std::lock_guard<std::mutex> lock(aheadMutex);
			aheadStopping = true;
		}
		aheadWake.notify_all();
		if (aheadThread.joinable()) aheadThread.join();
		FlushHistory(true);
	}

	void Monitor::AnalyseTilesForTest(const std::vector<float>& tiles, int count, double seconds, int nativeWidth, int nativeHeight)
	{
		Job j;
		j.values = tiles;
		j.tiles = count;
		j.codeMaximum = 1023;
		j.scale = 1023.0;
		j.seconds = seconds;
		j.nativeWidth = nativeWidth;
		j.nativeHeight = nativeHeight;
		Analyse(j);
	}

	Monitor::AheadState Monitor::Ahead() const
	{
		std::lock_guard<std::mutex> lock(aheadMutex);
		return ahead;
	}

	void Monitor::LookAheadFrame(uint64_t sequence, int width, int height, const LumaSampler& luma,
		int left, int top, int right, int bottom)
	{
		if (!luma || sequence <= aheadLastSequence) return;
		aheadLastSequence = sequence;
		left = (std::max)(0, left); top = (std::max)(0, top);
		right = (std::min)(width, right); bottom = (std::min)(height, bottom);
		const int w = right - left, h = bottom - top;
		if (w < 2 * kTile || h < kTile) return;
		// The tiles: 4 x 2 on the 16-px grid inside the picture (as the GPU monitor).
		const int tilesX = 4, tilesY = 2;
		auto tileOrigin = [&](int i, int& x0, int& y0) {
			const int tx = i % tilesX, ty = i / tilesX;
			x0 = (left + (w - kTile) * (2 * tx + 1) / (2 * tilesX)) & ~15;
			y0 = (top + (h - kTile) * (2 * ty + 1) / (2 * tilesY)) & ~15;
			x0 = (std::max)(left, (std::min)(x0, right - kTile));
			y0 = (std::max)(top, (std::min)(y0, bottom - kTile));
		};
		// Cheap check: the mean and spread on a sparse 32 x 32 grid per tile.
		double sum = 0.0, spread = 0.0;
		int count = 0;
		for (int i = 0; i < tilesX * tilesY; ++i)
		{
			int x0, y0;
			tileOrigin(i, x0, y0);
			double ts = 0.0, ts2 = 0.0;
			for (int y = 0; y < kTile; y += 8)
				for (int x = 0; x < kTile; x += 8)
				{
					const double v = luma(x0 + x, y0 + y) / 1023.0;
					ts += v; ts2 += v * v;
				}
			const double n = (kTile / 8.0) * (kTile / 8.0);
			const double mean = ts / n;
			spread = (std::max)(spread, std::sqrt((std::max)(0.0, ts2 / n - mean * mean)));
			sum += mean;
			++count;
		}
		const double mean = sum / count;
		const bool cut = aheadLastMean >= 0.0 && std::fabs(mean - aheadLastMean) > SourceResolution::Tracker::Settings{}.cutLevel;
		const bool blank = spread < 3.0 / 1023.0;   // no texture anywhere (black, a fade's end, flat)
		aheadLastMean = mean;
		const bool pending = disengagePending || std::fabs(autoPending - autoApplied) > 0.01;
		bool analyse = false;
		{
			std::lock_guard<std::mutex> lock(aheadMutex);
			if (!pending) { ahead.eventSequence = 0; aheadAwaitTarget = false; return; }
			if (ahead.eventSequence == 0 && (cut || blank))
			{
				ahead = AheadState{ sequence, 0, false, false, 1.0, ahead.verified, ahead.cancelled };
				aheadAwaitTarget = blank;
				analyse = !blank;
			}
			else if (ahead.eventSequence != 0 && aheadAwaitTarget && !blank)
			{
				aheadAwaitTarget = false;
				analyse = true;
			}
			if (analyse && !aheadHaveJob)
			{
				ahead.targetSequence = sequence;
				aheadTileCount = tilesX * tilesY;
				aheadTiles.resize(static_cast<size_t>(aheadTileCount) * kTile * kTile);
				for (int i = 0; i < aheadTileCount; ++i)
				{
					int x0, y0;
					tileOrigin(i, x0, y0);
					float* t = &aheadTiles[static_cast<size_t>(i) * kTile * kTile];
					for (int y = 0; y < kTile; ++y)
						for (int x = 0; x < kTile; ++x) t[y * kTile + x] = luma(x0 + x, y0 + y) / 1023.0f;
				}
				aheadHaveJob = true;
			}
		}
		if (analyse) aheadWake.notify_all();
	}

	void Monitor::AheadWorker()
	{
		for (;;)
		{
			std::vector<float> tiles;
			int count = 0;
			uint64_t target = 0;
			{
				std::unique_lock<std::mutex> lock(aheadMutex);
				aheadWake.wait(lock, [this] { return aheadStopping || aheadHaveJob; });
				if (aheadStopping) return;
				tiles.swap(aheadTiles);
				count = aheadTileCount;
				target = ahead.targetSequence;
			}
			std::vector<SourceResolution::Spectrum> spectra(static_cast<size_t>(count));
			for (int i = 0; i < count; ++i)
				SourceResolution::Analyse(&tiles[static_cast<size_t>(i) * kTile * kTile], kTile, kTile, spectra[static_cast<size_t>(i)], 1023);
			const SourceResolution::FrameReading r = SourceResolution::ReadFrame(spectra);
			std::lock_guard<std::mutex> lock(aheadMutex);
			aheadHaveJob = false;
			if (ahead.targetSequence == target)
			{
				ahead.ready = true;
				ahead.informative = r.informative();
				ahead.ratio = r.ratio;
			}
		}
	}

	void Monitor::SetHistoryFolder(const std::string& folder)
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (folder.empty()) { historyPath.clear(); return; }
		CreateDirectoryA(folder.c_str(), nullptr);
		// The sessions kept: the newest kHistorySessions - 1 earlier files (names sort by time), plus this one.
		std::vector<std::string> earlier;
		WIN32_FIND_DATAA found{};
		HANDLE search = FindFirstFileA((folder + "\\source-resolution *.csv").c_str(), &found);
		if (search != INVALID_HANDLE_VALUE)
		{
			do earlier.push_back(found.cFileName);
			while (FindNextFileA(search, &found));
			FindClose(search);
		}
		std::sort(earlier.begin(), earlier.end());
		while (earlier.size() > kHistorySessions - 1)
		{
			DeleteFileA((folder + "\\" + earlier.front()).c_str());
			earlier.erase(earlier.begin());
		}
		SYSTEMTIME t{};
		GetLocalTime(&t);
		char name[64];
		snprintf(name, sizeof(name), "source-resolution %04u%02u%02u-%02u%02u%02u.csv", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
		historyPath = folder + "\\" + name;
		char started[96];
		snprintf(started, sizeof(started), "session,,,,,,,,,,,,,session started %04u-%02u-%02u %02u:%02u:%02u",
			t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
		history.clear();
		Record(started);
	}

	void Monitor::Record(const std::string& row)
	{
		history.push_back(Clock() + "," + row);
		while (history.size() > kHistoryRows)
		{
			// The session row stays first.
			if (history.size() > 1 && history.front().find(",session,") != std::string::npos)
				history.erase(history.begin() + 1);
			else
				history.pop_front();
		}
		historyDirty = true;
	}

	void Monitor::FlushHistory(bool force)
	{
		std::string path;
		std::string text;
		{
			std::lock_guard<std::mutex> lock(mutex);
			const auto now = std::chrono::steady_clock::now();
			if (historyPath.empty() || !historyDirty || (!force && now - lastFlush < std::chrono::seconds(5))) return;
			lastFlush = now;
			historyDirty = false;
			path = historyPath;
			text = "time,event,frame_ratio,coherence_v,coherence_h,live,declared,confidence,bitrate_quality,effective_bits,cuts,"
				"tiles,auto_ratio,note\n";
			for (const std::string& row : history) text += row + "\n";
		}
		// Temp + replace: a reader never sees a half-written file.
		const std::string temp = path + ".tmp";
		FILE* f = nullptr;
		if (fopen_s(&f, temp.c_str(), "wb") != 0 || !f) return;
		const bool written = fwrite(text.data(), 1, text.size(), f) == text.size();
		fclose(f);
		if (written) MoveFileExA(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
		else DeleteFileA(temp.c_str());
	}

	void Monitor::Release(pl_gpu g)
	{
		for (int i = 0; i < 2; ++i)
		{
			if (ring[i] && g) pl_buf_destroy(g, &ring[i]);
			ring[i] = nullptr;
			busy[i] = false;
		}
		gpu = nullptr;
	}

	void Monitor::Discontinuity(const char* cause)
	{
		discontinuityCause.store(cause ? cause : "discontinuity", std::memory_order_release);
		autoReset.store(true, std::memory_order_release);
		wake.notify_all();
	}

	Snapshot Monitor::Read() const
	{
		std::lock_guard<std::mutex> lock(mutex);
		return snapshot;
	}

	void Monitor::Frame(pl_gpu g, pl_dispatch dispatch, const pl_frame& source, uint64_t sourceSequence, double seconds)
	{
		(void)sourceSequence;
		if (!g || !dispatch) return;
		if (gpu && gpu != g) Release(gpu);
		gpu = g;

		// Collect finished copies (never waits).
		for (int i = 0; i < 2; ++i)
		{
			if (!busy[i] || pl_buf_poll(gpu, ring[i], 0)) continue;
			Job done = pendingMeta[i];
			done.values.resize(static_cast<size_t>(done.tiles) * kPixels);
			const bool ok = pl_buf_read(gpu, ring[i], 0, done.values.data(), done.values.size() * sizeof(float));
			busy[i] = false;
			if (!ok)
			{
				Fail("the readback failed", seconds);
				continue;
			}
			{
				std::lock_guard<std::mutex> lock(mutex);
				job = std::move(done);   // a newer copy replaces one the worker has not started
				haveJob = true;
			}
			wake.notify_all();
		}

		// What the source is (a change is a discontinuity) and whether it can be read.
		const pl_tex luma = source.num_planes > 0 ? source.planes[0].texture : nullptr;
		std::string notRead;
		if (!luma) notRead = "no source";
		else if (source.repr.sys == PL_COLOR_SYSTEM_DOLBYVISION) notRead = "Dolby Vision tunnel (packed signal)";
		else if (!luma->params.sampleable) notRead = "source not sampleable";
		char sig[160];
		snprintf(sig, sizeof(sig), "%dx%d sys%d lev%d bits%d/%d/%d trc%d prim%d", luma ? luma->params.w : 0, luma ? luma->params.h : 0,
			static_cast<int>(source.repr.sys), static_cast<int>(source.repr.levels), source.repr.bits.sample_depth,
			source.repr.bits.color_depth, source.repr.bits.bit_shift, static_cast<int>(source.color.transfer), static_cast<int>(source.color.primaries));
		if (signature != sig)
		{
			if (!signature.empty()) Discontinuity("source format change");
			signature = sig;
		}
		if (notRead != notReadLogged)
		{
			notReadLogged = notRead;
			std::lock_guard<std::mutex> lock(mutex);
			snapshot.notRead = notRead;
		}
		if (!notRead.empty() || seconds - lastSubmit < intervalSeconds) return;

		// Tiles inside the active picture on the frame's 16-pixel grid (the
		// coding measures need block boundaries where the encoder put them).
		const float cx0 = (std::max)(0.0f, (std::min)(source.crop.x0, source.crop.x1));
		const float cy0 = (std::max)(0.0f, (std::min)(source.crop.y0, source.crop.y1));
		const float cx1 = (std::min)(static_cast<float>(luma->params.w), (std::max)(source.crop.x0, source.crop.x1));
		const float cy1 = (std::min)(static_cast<float>(luma->params.h), (std::max)(source.crop.y0, source.crop.y1));
		const float w = cx1 > cx0 ? cx1 - cx0 : static_cast<float>(luma->params.w);
		const float h = cy1 > cy0 ? cy1 - cy0 : static_cast<float>(luma->params.h);
		const float x0 = cx1 > cx0 ? cx0 : 0.0f, y0 = cy1 > cy0 ? cy0 : 0.0f;
		const int cols = (std::min)(4, static_cast<int>((w - 16.0f) / kTile));
		const int rows = (std::min)(2, static_cast<int>((h - 16.0f) / kTile));
		if (cols <= 0 || rows <= 0) return;
		float origins[16] = {};
		int tiles = 0;
		for (int r = 0; r < rows; ++r)
			for (int c = 0; c < cols && tiles < kMaxTiles; ++c)
			{
				auto place = [](float start, float span, int index, int count) {
					const float centre = start + (index + 0.5f) * span / count;
					float o = std::floor((centre - kTile / 2) / 16.0f) * 16.0f;
					const float lo = std::ceil(start / 16.0f) * 16.0f, hi = std::floor((start + span - kTile) / 16.0f) * 16.0f;
					return (std::max)(lo, (std::min)(hi, o));
				};
				origins[tiles * 2] = place(x0, w, c, cols);
				origins[tiles * 2 + 1] = place(y0, h, r, rows);
				++tiles;
			}

		const int slot = nextSlot;
		if (busy[slot]) return;   // the oldest copy is still on its way: skip, never wait
		const size_t bytes = static_cast<size_t>(kMaxTiles) * kPixels * sizeof(float);
		if (!ring[slot])
		{
			pl_buf_params params{};
			params.size = bytes;
			params.storable = true;
			params.host_readable = true;
			ring[slot] = pl_buf_create(gpu, &params);
			if (!ring[slot])
			{
				Fail("cannot create the readback buffer", seconds);
				return;
			}
		}
		pl_buffer_var var{};
		var.var = pl_var_float("values");
		var.var.dim_a = kMaxTiles * kPixels;
		var.layout = pl_std430_layout(0, &var.var);
		pl_shader_desc descriptors[2] = {};
		descriptors[0].desc.name = "src";
		descriptors[0].desc.type = PL_DESC_SAMPLED_TEX;
		descriptors[0].binding.object = luma;
		descriptors[0].binding.address_mode = PL_TEX_ADDRESS_CLAMP;
		descriptors[0].binding.sample_mode = PL_TEX_SAMPLE_NEAREST;
		descriptors[1].desc.name = "out_values";
		descriptors[1].desc.type = PL_DESC_BUF_STORAGE;
		descriptors[1].desc.access = PL_DESC_ACCESS_WRITEONLY;
		descriptors[1].binding.object = ring[slot];
		descriptors[1].buffer_vars = &var;
		descriptors[1].num_buffer_vars = 1;
		descriptors[1].memory = PL_MEMORY_COHERENT;
		const float tileCount = static_cast<float>(tiles);
		const float rgb = luma->params.format->num_components >= 3 ? 1.0f : 0.0f;
		const pl_shader_var variables[6] = {
			{ pl_var_float("tiles"), &tileCount, true },
			{ pl_var_float("is_rgb"), &rgb, true },
			{ pl_var_vec4("o01"), &origins[0], true },
			{ pl_var_vec4("o23"), &origins[4], true },
			{ pl_var_vec4("o45"), &origins[8], true },
			{ pl_var_vec4("o67"), &origins[12], true } };
		pl_custom_shader custom{};
		custom.description = "VideoProcessor source resolution tiles";
		custom.body = BODY;
		custom.input = PL_SHADER_SIG_NONE;
		custom.output = PL_SHADER_SIG_NONE;
		custom.descriptors = descriptors;
		custom.num_descriptors = 2;
		custom.variables = variables;
		custom.num_variables = 6;
		custom.compute = true;
		custom.compute_group_size[0] = 16;
		custom.compute_group_size[1] = 16;
		pl_shader sh = pl_dispatch_begin(dispatch);
		if (!pl_shader_custom(sh, &custom))
		{
			pl_dispatch_abort(dispatch, &sh);
			Fail("the tile shader was refused", seconds);
			return;
		}
		pl_dispatch_compute_params compute{};
		compute.shader = &sh;
		compute.dispatch_size[0] = tiles * kTile / 16;
		compute.dispatch_size[1] = kTile / 16;
		compute.dispatch_size[2] = 1;
		if (!pl_dispatch_compute(dispatch, &compute))
		{
			Fail("the tile copy failed", seconds);
			return;
		}
		Job& meta = pendingMeta[slot];
		meta.values.clear();
		meta.tiles = tiles;
		const int sampleDepth = source.repr.bits.sample_depth > 0 ? source.repr.bits.sample_depth : luma->params.format->component_depth[0];
		const int colorDepth = source.repr.bits.color_depth > 0 ? source.repr.bits.color_depth : sampleDepth;
		meta.codeMaximum = (1 << (std::min)(16, (std::max)(1, colorDepth))) - 1;
		meta.scale = (std::pow(2.0, (std::min)(16, (std::max)(1, sampleDepth))) - 1.0) / std::pow(2.0, source.repr.bits.bit_shift);
		if (luma->params.format->type == PL_FMT_FLOAT) meta.scale = meta.codeMaximum;   // float planes hold normalised values
		meta.seconds = seconds;
		meta.nativeWidth = luma->params.w;
		meta.nativeHeight = luma->params.h;
		busy[slot] = true;
		nextSlot = (slot + 1) % 2;
		lastSubmit = seconds;
	}

	void Monitor::Fail(const char* what, double seconds)
	{
		// Logged once per cause; retried after a minute.
		lastSubmit = seconds + 60.0;
		std::lock_guard<std::mutex> lock(mutex);
		if (snapshot.error != what) DebugLog::Log("Source resolution: %s", what);
		snapshot.error = what;
	}

	void Monitor::Worker()
	{
		for (;;)
		{
			Job current;
			{
				std::unique_lock<std::mutex> lock(mutex);
				wake.wait(lock, [this] { return stopping || haveJob || discontinuityCause.load(std::memory_order_acquire); });
				if (stopping) return;
				const char* cause = discontinuityCause.exchange(nullptr, std::memory_order_acq_rel);
				if (cause)
				{
					tracker.Discontinuity(cause);
					live.clear();
					coherence.clear();
					lastCuts = tracker.Cuts();
					qualitySmoothed = -1.0;
					bitsSmoothed = 0.0;
					snapshot.reading = false;
					snapshot.liveRatio = snapshot.declaredRatio = snapshot.frameRatio = 1.0;
					snapshot.confidence = 0.0;
					snapshot.discontinuities = tracker.Discontinuities();
					snapshot.lastDiscontinuity = cause;
					Record(std::string("discontinuity,,,,,1,0,,,,,") + "1," + cause);
				}
				if (!haveJob) continue;
				current = std::move(job);
				haveJob = false;
			}
			Analyse(current);
			FlushHistory(false);
		}
	}

	void Monitor::Analyse(Job& j)
	{
		const auto start = std::chrono::steady_clock::now();
		std::vector<SourceResolution::Spectrum> spectra(static_cast<size_t>(j.tiles));
		std::vector<float> luma(kPixels);
		for (int t = 0; t < j.tiles; ++t)
		{
			const float* src = j.values.data() + static_cast<size_t>(t) * kPixels;
			for (int i = 0; i < kPixels; ++i)
			{
				const double code = std::round(src[i] * j.scale);
				luma[i] = static_cast<float>((std::min)(static_cast<double>(j.codeMaximum), (std::max)(0.0, code)) / j.codeMaximum);
			}
			SourceResolution::Analyse(luma.data(), kTile, kTile, spectra[static_cast<size_t>(t)], j.codeMaximum);
		}
		SourceResolution::Thresholds thresholds;
		thresholds.codeMaximum = j.codeMaximum;
		const SourceResolution::FrameReading frame = SourceResolution::ReadFrame(spectra, thresholds);

		std::lock_guard<std::mutex> lock(mutex);
		tracker.Update(frame, j.seconds);
		// The live reading (spec 1a): ~2 s of informative frames, snapped on a cut.
		if (tracker.Cuts() != lastCuts) { live.clear(); lastCuts = tracker.Cuts(); }
		while (!live.empty() && j.seconds - live.front().first > 2.0) live.pop_front();
		while (!coherence.empty() && j.seconds - coherence.front().first > 120.0) coherence.pop_front();
		if (frame.informative())
		{
			live.push_back({ j.seconds, frame.ratio });
			coherence.push_back({ j.seconds, (std::min)(frame.vertical.coherence, frame.horizontal.coherence) });
		}
		std::vector<double> ratios;
		for (const auto& l : live) ratios.push_back(l.second);
		// Confidence of the declared value: the evidence (shots, seconds) and
		// how clearly both axes were resampled across the window.
		double coherenceMean = 0.0;
		for (const auto& c : coherence) coherenceMean += c.second;
		coherenceMean = coherence.empty() ? 0.0 : coherenceMean / coherence.size();
		const SourceResolution::Tracker::Settings ts;
		const double evidence = (std::min)(1.0, tracker.Shots() / static_cast<double>(ts.minimumShots)) *
			(std::min)(1.0, tracker.EvidenceSeconds() / ts.minimumSeconds);
		const double clarity = (std::min)(1.0, (std::max)(0.0, (coherenceMean - thresholds.coherence) / 0.3));

		// Spec 1b / 2d readings, smoothed over ~2 s (4 copies), snapped on a cut.
		const bool cutNow = live.size() <= 1;
		if (qualitySmoothed < 0.0 || cutNow) qualitySmoothed = frame.bitrateQuality;
		else qualitySmoothed += 0.25 * (frame.bitrateQuality - qualitySmoothed);
		if (frame.effectiveBits > 0.0) bitsSmoothed = bitsSmoothed <= 0.0 || cutNow ? frame.effectiveBits : bitsSmoothed + 0.25 * (frame.effectiveBits - bitsSmoothed);

		snapshot.reading = true;
		snapshot.nativeWidth = j.nativeWidth;
		snapshot.nativeHeight = j.nativeHeight;
		snapshot.frameRatio = frame.ratio;
		snapshot.frameBlank = !frame.informative();
		snapshot.liveRatio = ratios.empty() ? 1.0 : Median(ratios);
		snapshot.declaredRatio = tracker.Ratio();
		snapshot.confidence = tracker.Ratio() < 1.0 ? evidence * clarity : 0.0;
		snapshot.bitrateQuality = qualitySmoothed;
		snapshot.effectiveBits = bitsSmoothed;
		snapshot.coherenceV = frame.vertical.coherence;
		snapshot.coherenceH = frame.horizontal.coherence;
		snapshot.cuts = tracker.Cuts();
		snapshot.discontinuities = tracker.Discontinuities();
		++snapshot.frames;
		snapshot.analyseMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		char row[256];
		snprintf(row, sizeof(row), "reading,%.4f,%.3f,%.3f,%.4f,%.4f,%.2f,%.2f,%.1f,%llu,%d/%d,%.4f,%.1f ms",
			frame.ratio, frame.vertical.coherence, frame.horizontal.coherence, snapshot.liveRatio, snapshot.declaredRatio,
			snapshot.confidence, snapshot.bitrateQuality, snapshot.effectiveBits, snapshot.cuts, frame.informativeTiles,
			frame.tiles, autoAppliedShared.load(std::memory_order_acquire), snapshot.analyseMs);
		Record(row);
	}

	void Monitor::SetSelection(const std::string& value)
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (selection != value)
		{
			selection = value;
			if (value != "auto") autoAppliedShared.store(1.0, std::memory_order_release);
		}
	}

	std::string Monitor::Selection() const
	{
		std::lock_guard<std::mutex> lock(mutex);
		return selection;
	}

	double Monitor::AutoFactor(int nativeHeight, double minimumConfidence, std::string& why, double seconds, uint64_t renderSequence)
	{
		why.clear();
		const double now = seconds >= 0.0 ? seconds :
			std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
		const Snapshot s = Read();
		const double before = autoApplied;
		if (autoReset.exchange(false, std::memory_order_acq_rel))
		{
			autoApplied = autoPending = 1.0;
			disengagePending = false;
			if (before < 1.0) why = "native at once (" + (s.lastDiscontinuity.empty() ? std::string("discontinuity") : s.lastDiscontinuity) + ")";
		}
		double wanted = s.reading && s.declaredRatio < 1.0 && s.confidence >= minimumConfidence ? s.declaredRatio : 1.0;
		// Lines: an even height derived from the frame's own height (general).
		if (wanted < 1.0 && nativeHeight > 0)
		{
			const int lines = 2 * static_cast<int>(std::lround(wanted * nativeHeight * 0.5));
			wanted = lines < nativeHeight ? static_cast<double>(lines) / nativeHeight : 1.0;
		}
		const double tolerance = 0.01;
		if (wanted > autoApplied + tolerance || (wanted >= 1.0 && autoApplied < 1.0))
		{
			// Up (towards native). Detail above the re-based height (the declared reading rose)
			// is the harmful direction: at once. Only the confidence fell: at the next cut,
			// at most kDisengageWaitSeconds later, so the scaler does not change mid-scene.
			const bool detailAbove = !s.reading || s.declaredRatio > autoApplied + tolerance;
			if (detailAbove)
			{
				autoApplied = autoPending = wanted;
				disengagePending = false;
				if (why.empty()) why = wanted >= 1.0 ? "native at once (detail above the re-based height)" : "raised at once (detail above the re-based height)";
			}
			else if (!disengagePending)
			{
				disengagePending = true;
				disengageSince = now;
				disengageCuts = s.cuts;
				disengageFrames = s.frames;
				why = "confidence fell: native at the next cut or black (within 5 s)";
			}
			else if (s.cuts != disengageCuts || (s.frameBlank && s.frames != disengageFrames) || now - disengageSince >= kDisengageWaitSeconds)
			{
				why = s.cuts != disengageCuts ? "native at a cut (confidence fell)" :
					s.frameBlank && s.frames != disengageFrames ? "native during a black or flat frame (confidence fell)" :
					"native after 5 s without a cut (confidence fell)";
				autoApplied = autoPending = wanted;
				disengagePending = false;
			}
		}
		else
			disengagePending = false;

		if (wanted < autoApplied - tolerance)
		{
			// Down: wait for the next cut so the picture never changes mid-shot.
			AheadState a;
			{
				std::lock_guard<std::mutex> lock(aheadMutex);
				a = ahead;
			}
			const bool aheadDue = renderSequence != 0 && a.eventSequence != 0 && a.ready && renderSequence >= a.eventSequence &&
				std::fabs(wanted - autoPending) <= tolerance;
			if (std::fabs(wanted - autoPending) > tolerance)
			{
				autoPending = wanted;
				autoPendingCuts = s.cuts;
				autoPendingFrames = s.frames;
				why = "waiting for the next cut or black";
			}
			else if (aheadDue)
			{
				// The frame after the cut / black, read ahead of display: it decides.
				const bool agrees = !a.informative || std::fabs(a.ratio - wanted) <= 0.02;
				std::lock_guard<std::mutex> lock(aheadMutex);
				if (agrees)
				{
					autoApplied = wanted;
					++ahead.verified;
					why = "engaged at the cut / black (look-ahead verified)";
				}
				else
				{
					autoPending = autoApplied;
					autoNotBeforeCuts = s.cuts + 1;   // the event's cut, when the monitor sees it, is not used
					++ahead.cancelled;
					why = "look-ahead: the next shot disagrees - staying";
				}
				ahead.eventSequence = 0;
				ahead.ready = false;
			}
			else if (s.cuts <= autoNotBeforeCuts && autoNotBeforeCuts != 0)
			{
				// After a look-ahead cancel: wait for a later cut.
			}
			else if (s.cuts != autoPendingCuts)
			{
				autoApplied = wanted;
				why = "engaged at a cut";
			}
			else if (s.frameBlank && s.frames != autoPendingFrames)
			{
				// A black or flat frame (a fade's dark end): the change cannot be seen.
				autoApplied = wanted;
				why = "engaged during a black or flat frame";
			}
		}
		autoAppliedShared.store(autoApplied, std::memory_order_release);
		if (!why.empty())
		{
			char row[160];
			snprintf(row, sizeof(row), "auto,,,,,%.4f,%.2f,,,%llu,,%.4f,", s.declaredRatio, s.confidence, s.cuts, autoApplied);
			std::lock_guard<std::mutex> lock(mutex);
			Record(row + why);
		}
		return autoApplied;
	}

	std::string Describe(const Snapshot& s, double autoRatio, bool autoSelected)
	{
		char line[400];
		if (!s.notRead.empty())
		{
			snprintf(line, sizeof(line), "Source resolution: not read (%s)", s.notRead.c_str());
			return line;
		}
		if (!s.reading && !s.error.empty())
		{
			snprintf(line, sizeof(line), "Source resolution: not read (%s)", s.error.c_str());
			return line;
		}
		if (!s.reading)
		{
			snprintf(line, sizeof(line), "Source resolution: measuring%s%s", s.lastDiscontinuity.empty() ? "" : " (after ",
				s.lastDiscontinuity.empty() ? "" : (s.lastDiscontinuity + ")").c_str());
			return line;
		}
		const std::string liveText = SourceResolution::Describe(s.liveRatio, s.nativeHeight);
		const std::string declaredText = SourceResolution::Describe(s.declaredRatio, s.nativeHeight);
		char bits[32] = "";
		if (s.effectiveBits > 0.0) snprintf(bits, sizeof(bits), ", %.1f-bit", s.effectiveBits);
		char autoText[96] = "";
		if (autoSelected)
			snprintf(autoText, sizeof(autoText), "; auto %s", autoRatio < 1.0 ? ("re-based to " + SourceResolution::Describe(autoRatio, s.nativeHeight)).c_str() : "native");
		snprintf(line, sizeof(line), "Source resolution: %s (declared %s, confidence %.2f); bitrate quality %.2f%s%s",
			liveText.c_str(), declaredText.c_str(), s.confidence, s.bitrateQuality, bits, autoText);
		return line;
	}
}
