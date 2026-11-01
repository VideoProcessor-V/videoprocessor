#include <pch.h>
#include "SourceResolutionMonitor.h"
#include <VP-V/modules/scaling/rebase/KernelInverse.h>

#include <DebugLog.h>
#include <GeneralRateLog.h>

#include <VP-V/modules/drive/led_measure/NativeReadback.h>

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
		// an RGB plane) at the tile's origin, written as a float. Tiles of
		// tile_size px (at most Monitor::kTile), packed one after another.
		const char* const BODY = R"GLSL(
ivec2 g = ivec2(gl_GlobalInvocationID.xy);
int ts = int(tile_size);
int tile = g.x / ts;
int x = g.x - tile * ts;
if (tile < int(tiles) && g.y < ts) {
    vec4 pair = tile < 2 ? o01 : tile < 4 ? o23 : tile < 6 ? o45 : tile < 8 ? o67 :
                tile < 10 ? o89 : tile < 12 ? oab : tile < 14 ? ocd : oef;
    vec2 origin = (tile % 2) == 0 ? pair.xy : pair.zw;
    ivec2 p = clamp(ivec2(origin) + ivec2(x, g.y), ivec2(0), textureSize(src, 0) - ivec2(1));
    vec4 c = texelFetch(src, p, 0);
    float v = is_rgb > 0.5 ? dot(c.rgb, vec3(0.2126, 0.7152, 0.0722)) : c.r;
    int idx = (tile * ts + g.y) * ts + x;
#ifdef SR_IMG
    imageStore(out_img, ivec2(idx % SR_IMG, idx / SR_IMG), vec4(v));
#else
    values[idx] = v;
#endif
}
)GLSL";

		LedDrive::NativeTexReadback* Native(const std::shared_ptr<void>& holder)
		{
			return static_cast<LedDrive::NativeTexReadback*>(holder.get());
		}

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

	void Monitor::AnalyseTilesForTest(const std::vector<float>& tiles, int count, double seconds, int nativeWidth, int nativeHeight,
		const std::vector<float>& tileX, const std::vector<float>& tileY, int baseTiles, int tileSize, double blackLevel)
	{
		AnalyseTilesForTest(tiles, count, seconds, nativeWidth, nativeHeight, tileX, tileY, baseTiles, tileSize, blackLevel, 0);
	}

	void Monitor::AnalyseTilesForTest(const std::vector<float>& tiles, int count, double seconds, int nativeWidth, int nativeHeight,
		const std::vector<float>& tileX, const std::vector<float>& tileY, int baseTiles, int tileSize, double blackLevel, uint64_t sequence)
	{
		Job j;
		j.sequence = sequence;
		j.values = tiles;
		j.tiles = count;
		j.codeMaximum = 1023;
		j.scale = 1023.0;
		j.seconds = seconds;
		j.nativeWidth = nativeWidth;
		j.nativeHeight = nativeHeight;
		j.tileX = tileX;
		j.tileY = tileY;
		j.baseTiles = (std::min)(baseTiles, count);
		j.tileSize = tileSize;
		j.blackLevel = blackLevel;
		Analyse(j);
	}

	void Monitor::AnalyseTilesForTest(const std::vector<float>& tiles, int count, double seconds, int nativeWidth, int nativeHeight,
		const std::vector<float>& tileX, const std::vector<float>& tileY, int baseTiles)
	{
		Job j;
		j.values = tiles;
		j.tiles = count;
		j.codeMaximum = 1023;
		j.scale = 1023.0;
		j.seconds = seconds;
		j.nativeWidth = nativeWidth;
		j.nativeHeight = nativeHeight;
		j.tileX = tileX;
		j.tileY = tileY;
		j.baseTiles = (std::min)(baseTiles, count);
		Analyse(j);
	}

	void (*Monitor::kernelFitProbeForTest)(Monitor&) = nullptr;

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

	SourceResolution::FrameEvidence Monitor::LookAheadEvidence(uint64_t from, uint64_t to) const
	{
		std::lock_guard<std::mutex> lock(frameJudgeMutex);
		return frameJudge.Evidence(from, to);
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
		const bool fixes = LatchFixes();
		const int tileSize = TileFor(h);
		if (w < 2 * tileSize || h < tileSize) return;
		// The tiles: 4 x 2 on the 16-px grid inside the picture (as the GPU monitor).
		const int tilesX = 4, tilesY = 2;
		auto tileOrigin = [&](int i, int& x0, int& y0) {
			const int tx = i % tilesX, ty = i / tilesX;
			x0 = (left + (w - tileSize) * (2 * tx + 1) / (2 * tilesX)) & ~15;
			y0 = (top + (h - tileSize) * (2 * ty + 1) / (2 * tilesY)) & ~15;
			x0 = (std::max)(left, (std::min)(x0, right - tileSize));
			y0 = (std::max)(top, (std::min)(y0, bottom - tileSize));
		};
		// Cheap check: the mean and spread on a sparse 32 x 32 grid per tile.
		double sum = 0.0, spread = 0.0;
		int count = 0;
		std::vector<double> pattern;
		// The sparse grid: 32 x 32 samples per tile whatever its size.
		const int step = (std::max)(4, tileSize / 32);
		const int half = tileSize / 2;
		for (int i = 0; i < tilesX * tilesY; ++i)
		{
			int x0, y0;
			tileOrigin(i, x0, y0);
			double ts = 0.0, ts2 = 0.0, q[4] = {};
			double samples = 0.0;
			for (int y = 0; y < tileSize; y += step)
				for (int x = 0; x < tileSize; x += step)
				{
					const double v = luma(x0 + x, y0 + y) / 1023.0;
					ts += v; ts2 += v * v;
					q[(y >= half ? 2 : 0) + (x >= half ? 1 : 0)] += v;
					samples += 1.0;
				}
			const double mean = ts / samples;
			spread = (std::max)(spread, std::sqrt((std::max)(0.0, ts2 / samples - mean * mean)));
			for (int k = 0; k < 4; ++k) pattern.push_back(q[k] / (samples / 4.0));
			sum += mean;
			++count;
		}
		const double mean = sum / count;
		bool cut = false, blank = false;
		if (fixes)
		{
			// Latch fixes: the tracker's shape-aware cut (a fade or a flash is not a cut) and a
			// real black (flat and at black) - the look-ahead's events are cuts and blacks only.
			const SourceResolution::Tracker::Settings ts;
			const SourceResolution::Thresholds th;
			const std::vector<char> none(pattern.size(), 0);
			const double noise = th.flatSpreadCodes / 1023.0;
			double decorrelation = 0.0, share = 0.0, jump = 0.0;
			if (CutFixes())
			{
				// Cut fixes: the per-frame judge with the dark and flash rules; its candidates are the
				// look-ahead's cut events, and a candidate found to be a flash cancels its event below.
				std::lock_guard<std::mutex> lock(frameJudgeMutex);
				cut = frameJudge.Frame(sequence, pattern, blackLevelSeen, noise, secondsPerFrame, ts);
			}
			else if (SourceResolution::PatternChange(aheadLastPattern, none, pattern, none, blackLevelSeen, noise, decorrelation, share, &jump))
			{
				// Consecutive frames ahead: the motion level per frame interval (the frame rate measured).
				static GeneralRate::FallbackNotice fallback;   // fixed-constant batch B7 2026-10-18: logged
				const bool measured = GeneralRate::NoteFallback(fallback, secondsPerFrame > 0.0, "source-resolution cut judge", 24.0);
				cut = aheadCutJudge.Judge(decorrelation, jump, measured ? secondsPerFrame : 1.0 / 24.0, ts);
				aheadLastPattern = pattern;
			}
			else if (SourceResolution::PatternChange(pattern, none, pattern, none, blackLevelSeen, noise, decorrelation, share))
				aheadLastPattern = pattern;
			blank = spread < 3.0 / 1023.0 && mean <= blackLevelSeen + th.nearBlackShare * (1.0 - blackLevelSeen);
		}
		else
		{
			cut = aheadLastMean >= 0.0 && std::fabs(mean - aheadLastMean) > SourceResolution::Tracker::Settings{}.cutLevel;
			blank = spread < 3.0 / 1023.0;   // no texture anywhere (black, a fade's end, flat)
		}
		aheadLastMean = mean;
		const bool pending = disengagePending || std::fabs(autoPending - autoApplied) > 0.01;
		bool analyse = false;
		{
			std::lock_guard<std::mutex> lock(aheadMutex);
			if (!pending) { ahead.eventSequence = 0; aheadAwaitTarget = false; return; }
			if (fixes && CutFixes() && ahead.eventSequence != 0)
			{
				// A flash (back to the shape before within the flash window): no switch at it.
				bool flash = false;
				{
					std::lock_guard<std::mutex> judgeLock(frameJudgeMutex);
					flash = frameJudge.Cancelled(ahead.eventSequence);
				}
				if (flash) { ahead.eventSequence = 0; ahead.ready = false; aheadAwaitTarget = false; }
			}
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
				aheadTileSize = tileSize;
				aheadTiles.resize(static_cast<size_t>(aheadTileCount) * tileSize * tileSize);
				for (int i = 0; i < aheadTileCount; ++i)
				{
					int x0, y0;
					tileOrigin(i, x0, y0);
					float* t = &aheadTiles[static_cast<size_t>(i) * tileSize * tileSize];
					for (int y = 0; y < tileSize; ++y)
						for (int x = 0; x < tileSize; ++x) t[y * tileSize + x] = luma(x0 + x, y0 + y) / 1023.0f;
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
			int count = 0, size = kTile;
			uint64_t target = 0;
			{
				std::unique_lock<std::mutex> lock(aheadMutex);
				aheadWake.wait(lock, [this] { return aheadStopping || aheadHaveJob; });
				if (aheadStopping) return;
				tiles.swap(aheadTiles);
				count = aheadTileCount;
				size = aheadTileSize;
				target = ahead.targetSequence;
			}
			std::vector<SourceResolution::Spectrum> spectra(static_cast<size_t>(count));
			for (int i = 0; i < count; ++i)
				SourceResolution::Analyse(&tiles[static_cast<size_t>(i) * size * size], size, size, spectra[static_cast<size_t>(i)], 1023);
			SourceResolution::Thresholds thresholds;
			thresholds.latchFixes = LatchFixes();
			if (!textGuard.load(std::memory_order_acquire)) thresholds.textBand = 0.0;
			thresholds.mixedGuard = mixedGuard.load(std::memory_order_acquire);
			thresholds.codingFixes = codingFixes.load(std::memory_order_acquire);
			const SourceResolution::FrameReading r = SourceResolution::ReadFrame(spectra, thresholds);
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
		if (LedDrive::NativeTexReadback* native = Native(nativeHolder))
			native->Release(g);
		nativeHolder.reset();
		nativeTried = false;
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


	// ---- source identity (doc 61, P14) -------------------------------------------------------------------------------
	void Monitor::SetSourceIdConsensus(bool on)
	{
		if (sourceIdOn.exchange(on, std::memory_order_acq_rel) != on && on)
		{
			sourceId.Held().Reset();   // records from before an off period must not read as fresh (HeldState is mutex-guarded)
			sourceIdResetPending.store(true, std::memory_order_release);
		}
	}

	namespace
	{
		double ClockNow(double seconds)
		{
			return seconds >= 0.0 ? seconds : std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
		}
	}

	SourceIdentity::QualityView Monitor::QualityView(double seconds) const
	{
		const Snapshot s = Read();
		SourceIdentity::ClassicalQuality c;
		c.reading = s.reading;
		c.bitrateQuality = s.bitrateQuality;
		c.effectiveBits = s.effectiveBits;
		const bool on = SourceIdConsensus();
		if (!on)
			return SourceIdentity::View(nullptr, c, false, 0);
		SourceIdentity::Record rec;
		const bool have = sourceId.Held().ReadLatest(0, rec);
		return SourceIdentity::View(have ? &rec : nullptr, c, true, static_cast<int64_t>(std::llround(ClockNow(seconds) * 1e6)));
	}

	std::string Monitor::SourceIdStatusLine(double seconds) const
	{
		const SourceIdentity::QualityView v = QualityView(seconds);
		SourceIdentity::Record rec;
		const bool have = sourceId.Held().ReadLatest(0, rec);
		return SourceIdentity::StatusLine(v, have ? &rec : nullptr);
	}

	FallbackCatalog::Decision Monitor::SourceIdGateFallback() const
	{
		std::lock_guard<std::mutex> lock(gateMutex);
		return gateDecision;
	}

	int Monitor::ApplySourceIdRebase(SourceIdentity::IndicatorPort& port)
	{
		SourceIdentity::Record rec;
		if (!SourceIdConsensus() || !sourceId.Held().ReadLatest(0, rec))
			return 0;
		return sourceIdRebase.Poll(rec, port);
	}

	Monitor::IdGate Monitor::SourceIdGate(double now)
	{
		IdGate g;
		g.on = SourceIdConsensus();
		if (!g.on)
		{
			std::lock_guard<std::mutex> lock(gateMutex);
			if (gateDecision.engaged || gateDecision.id.empty())
				gateDecision = FallbackCatalog::Released("product.source_quality");
			return g;
		}
		SourceIdentity::Record rec;
		const char* why = nullptr;
		if (!sourceId.Held().ReadLatest(0, rec))
			why = "no record";
		else if (rec.unitVersion != SourceIdentity::kUnitVersion)
			why = "unit version";
		else
		{
			const int64_t nowUs = static_cast<int64_t>(std::llround(now * 1e6));
			if (rec.maxAgeUs > 0 && (nowUs - rec.timeUs > rec.maxAgeUs || nowUs < rec.timeUs - rec.maxAgeUs))
				why = "stale";
		}
		{
			std::lock_guard<std::mutex> lock(gateMutex);
			gateDecision = why ? FallbackCatalog::ProductFallback("product.source_quality", std::string("auto engage: ") + why)
				: FallbackCatalog::Released("product.source_quality");
		}
		if (why)
			return g;
		g.fresh = true;
		g.latchedH = rec.Latched(SourceIdentity::Field::AxisH);
		g.latchedV = rec.Latched(SourceIdentity::Field::AxisV);
		g.ratioH = g.latchedH ? rec.ratioH : 1.0;
		g.ratioV = g.latchedV ? rec.ratioV : 1.0;
		return g;
	}

	std::string Monitor::NotReadReason(const pl_frame& source)
	{
		const pl_tex luma = source.num_planes > 0 ? source.planes[0].texture : nullptr;
		if (!luma) return "no source";
		// Dolby Vision used to be skipped as a "packed signal"; its planes are the
		// unpacked 12-bit IPT (I in planes[0]), so it is read like YCbCr luma.
		if (!luma->params.sampleable) return "source not sampleable";
		return std::string();
	}

	void Monitor::Frame(pl_gpu g, pl_dispatch dispatch, const pl_frame& source, uint64_t sourceSequence, double seconds)
	{
		(void)sourceSequence;
		if (!g || !dispatch) return;
		if (gpu && gpu != g) Release(gpu);
		gpu = g;
		if (!nativeTried)
		{
			// D3D11: two native staging slots of the whole tile buffer (once per GPU).
			nativeTried = true;
			std::shared_ptr<LedDrive::NativeTexReadback> created = std::make_shared<LedDrive::NativeTexReadback>();
			const int rows = (kMaxTiles * kPixels + kReadbackWidth - 1) / kReadbackWidth;
			if (created->Create(gpu, 2, kReadbackWidth, rows, DXGI_FORMAT_R32_FLOAT))
				nativeHolder = created;
			else
				DebugLog::Log("Source resolution: the non-blocking tile readback is unavailable (not D3D11 or no staging); libplacebo's readback");
		}
		LedDrive::NativeTexReadback* const native = Native(nativeHolder);

		// Collect finished copies (never waits: D3D11, the copy's event query and a DO_NOT_WAIT
		// map; elsewhere libplacebo's poll).
		for (int i = 0; i < 2; ++i)
		{
			if (!busy[i]) continue;
			if (native ? !native->Ready(i) : pl_buf_poll(gpu, ring[i], 0)) continue;
			Job done = pendingMeta[i];
			const size_t count = static_cast<size_t>(done.tiles) * done.tileSize * done.tileSize;
			bool ok = false;
			if (native)
			{
				// Whole rows of the R32F image, then trimmed to the tiles' samples.
				const int rows = static_cast<int>((count + kReadbackWidth - 1) / kReadbackWidth);
				done.values.resize(static_cast<size_t>(rows) * kReadbackWidth);
				if (!native->Read(i, done.values.data(), kReadbackWidth * sizeof(float), rows))
					continue;   // still drawing after all: the next frame takes it
				done.values.resize(count);
				ok = true;
			}
			else
			{
				done.values.resize(count);
				ok = pl_buf_read(gpu, ring[i], 0, done.values.data(), done.values.size() * sizeof(float));
			}
			busy[i] = false;
			if (!ok)
			{
				Fail("the readback failed", seconds);
				continue;
			}
			// R3 (resync_event on): a copy of a frame from before a FULL resync is not evidence of
			// the new source. Keys are 0 when not driven: never dropped.
			if (done.streamKey != streamKey.load(std::memory_order_acquire))
			{
				staleJobsDropped.fetch_add(1, std::memory_order_relaxed);
				continue;
			}
			// Tests only (gate r3 2026-10-18): VP_TEST_SOURCE_MONITOR_DIGEST=1 logs a digest of the
			// tiles the monitor read, so a GPU test can show its input is bit-identical whatever
			// runs after it (the detail stages). Off in ordinary runs; read per readback (a few a
			// second) so a test process that keeps the renderer DLL loaded can switch it.
			char digestValue[8] = {};
			if (GetEnvironmentVariableA("VP_TEST_SOURCE_MONITOR_DIGEST", digestValue, sizeof(digestValue)) > 0 && digestValue[0] == '1')
			{
				uint64_t fnv = 1469598103934665603ull;
				const unsigned char* bytes = reinterpret_cast<const unsigned char*>(done.values.data());
				for (size_t b = 0; b < done.values.size() * sizeof(float); ++b) { fnv ^= bytes[b]; fnv *= 1099511628211ull; }
				DebugLog::Log("Source resolution input digest: tiles %d, tile %d, fnv %016llx", done.tiles, done.tileSize,
					static_cast<unsigned long long>(fnv));
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
		const std::string notRead = NotReadReason(source);
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
		// The base 4 x 2 grid first (unchanged, so frame signatures compare across a switch),
		// then the dense rows while a lower reading is declared or gathered.
		int baseTiles = 0;
		const int tileSize = TileFor(h);
		const std::vector<SourceResolution::TilePlace> places = SourceResolution::PlaceTiles(x0, y0, w, h, tileSize, denseGrid.load(std::memory_order_acquire), &baseTiles);
		if (places.empty()) return;
		float origins[2 * kMaxTiles] = {};
		const int tiles = (std::min)(kMaxTiles, static_cast<int>(places.size()));
		for (int i = 0; i < tiles; ++i)
		{
			origins[i * 2] = static_cast<float>(places[static_cast<size_t>(i)].x0);
			origins[i * 2 + 1] = static_cast<float>(places[static_cast<size_t>(i)].y0);
		}

		const int slot = nextSlot;
		if (busy[slot]) return;   // the oldest copy is still on its way: skip, never wait
		const size_t bytes = static_cast<size_t>(kMaxTiles) * kPixels * sizeof(float);
		if (!native && !ring[slot])
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
		if (native)
		{
			descriptors[1].desc.name = "out_img";
			descriptors[1].desc.type = PL_DESC_STORAGE_IMG;
			descriptors[1].desc.access = PL_DESC_ACCESS_WRITEONLY;
			descriptors[1].binding.object = native->Target(slot);
		}
		else
		{
			descriptors[1].desc.name = "out_values";
			descriptors[1].desc.type = PL_DESC_BUF_STORAGE;
			descriptors[1].desc.access = PL_DESC_ACCESS_WRITEONLY;
			descriptors[1].binding.object = ring[slot];
			descriptors[1].buffer_vars = &var;
			descriptors[1].num_buffer_vars = 1;
			descriptors[1].memory = PL_MEMORY_COHERENT;
		}
		const float tileCount = static_cast<float>(tiles);
		const float tileSizeVar = static_cast<float>(tileSize);
		const float rgb = luma->params.format->num_components >= 3 ? 1.0f : 0.0f;
		const pl_shader_var variables[11] = {
			{ pl_var_float("tiles"), &tileCount, true },
			{ pl_var_float("tile_size"), &tileSizeVar, true },
			{ pl_var_float("is_rgb"), &rgb, true },
			{ pl_var_vec4("o01"), &origins[0], true },
			{ pl_var_vec4("o23"), &origins[4], true },
			{ pl_var_vec4("o45"), &origins[8], true },
			{ pl_var_vec4("o67"), &origins[12], true },
			{ pl_var_vec4("o89"), &origins[16], true },
			{ pl_var_vec4("oab"), &origins[20], true },
			{ pl_var_vec4("ocd"), &origins[24], true },
			{ pl_var_vec4("oef"), &origins[28], true } };
		pl_custom_shader custom{};
		static const std::string nativeHeader = "#define SR_IMG " + std::to_string(kReadbackWidth) + "\n";
		custom.description = "VideoProcessor source resolution tiles";
		custom.header = native ? nativeHeader.c_str() : nullptr;
		custom.body = BODY;
		custom.input = PL_SHADER_SIG_NONE;
		custom.output = PL_SHADER_SIG_NONE;
		custom.descriptors = descriptors;
		custom.num_descriptors = 2;
		custom.variables = variables;
		custom.num_variables = 11;
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
		compute.dispatch_size[0] = tiles * tileSize / 16;
		compute.dispatch_size[1] = tileSize / 16;
		compute.dispatch_size[2] = 1;
		if (!pl_dispatch_compute(dispatch, &compute))
		{
			Fail("the tile copy failed", seconds);
			return;
		}
		if (native)
			native->Queue(slot);
		Job& meta = pendingMeta[slot];
		meta.values.clear();
		meta.streamKey = streamKey.load(std::memory_order_acquire);   // R3 (0 when not driven)
		meta.tiles = tiles;
		meta.tileSize = tileSize;
		meta.baseTiles = (std::min)(baseTiles, tiles);
		meta.tileX.clear();
		meta.tileY.clear();
		for (int i = 0; i < tiles; ++i)
		{
			meta.tileX.push_back(places[static_cast<size_t>(i)].centreX);
			meta.tileY.push_back(places[static_cast<size_t>(i)].centreY);
		}
		const int sampleDepth = source.repr.bits.sample_depth > 0 ? source.repr.bits.sample_depth : luma->params.format->component_depth[0];
		const int colorDepth = source.repr.bits.color_depth > 0 ? source.repr.bits.color_depth : sampleDepth;
		meta.codeMaximum = (1 << (std::min)(16, (std::max)(1, colorDepth))) - 1;
		meta.scale = (std::pow(2.0, (std::min)(16, (std::max)(1, sampleDepth))) - 1.0) / std::pow(2.0, source.repr.bits.bit_shift);
		if (luma->params.format->type == PL_FMT_FLOAT) meta.scale = meta.codeMaximum;   // float planes hold normalised values
		// The signal's black (normalised): full range 0, limited 16 << (bits - 8).
		meta.blackLevel = source.repr.levels == PL_COLOR_LEVELS_FULL ? 0.0 : 16.0 * (meta.codeMaximum + 1) / 256.0 / meta.codeMaximum;
		blackLevelSeen = meta.blackLevel;
		meta.seconds = seconds;
		meta.sequence = sourceSequence;
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
			const char* cause = nullptr;
			bool have = false;
			{
				std::unique_lock<std::mutex> lock(mutex);
				wake.wait(lock, [this] { return stopping || haveJob || discontinuityCause.load(std::memory_order_acquire); });
				if (stopping) return;
				cause = discontinuityCause.exchange(nullptr, std::memory_order_acq_rel);
				have = haveJob;
				if (have)
				{
					current = std::move(job);
					haveJob = false;
				}
			}
			// Efficiency 2026-10-24: evidenceMutex (then mutex) serialises a reset with an analysis
			// whose kernel fit runs with `mutex` released (tests analyse on their own thread).
			if (cause)
			{
				std::lock_guard<std::mutex> evidenceGuard(evidenceMutex);
				std::lock_guard<std::mutex> lock(mutex);
				ResetEvidence(cause);
			}
			if (!have) continue;
			// R3: a job queued before a FULL resync whose reset ran first (keys 0 when not driven).
			if (current.streamKey != streamKey.load(std::memory_order_acquire))
			{
				staleJobsDropped.fetch_add(1, std::memory_order_relaxed);
				continue;
			}
			Analyse(current);
			FlushHistory(false);
		}
	}

	void Monitor::ResetEvidence(const char* cause)
	{
		// Every piece of evidence: the trackers, the kernel votes, the coherence / live history.
		tracker.Discontinuity(cause);
		trackerV.Discontinuity(cause);
		trackerH.Discontinuity(cause);
		denseGrid.store(false, std::memory_order_release);
		coherenceV.clear();
		coherenceH.clear();
		kernelVotes.clear();
		snapshot.kernel = -1;
		snapshot.kernelVotes = snapshot.kernelAgree = 0;
		snapshot.axisRatioX = snapshot.axisRatioY = 1.0;
		snapshot.axisConfidence = 0.0;
		live.clear();
		coherence.clear();
		lastCuts = tracker.Cuts();
		qualitySmoothed = -1.0;
		bitsSmoothed = 0.0;
		lastAnalysedSeconds = -1.0;
		// Source identity (worker thread): a new title/source starts from native/unknown (no stamp: the Indicator's FULL
		// already bumps both axes).
		sourceId.Discontinuity(snapshot.frames, streamKey.load(std::memory_order_acquire));
		sourceIdLastCuts = tracker.Cuts();
		snapshot.frameQuality = 1.0;
		snapshot.frameNoise = -1.0;
		snapshot.reading = false;
		snapshot.liveRatio = snapshot.declaredRatio = snapshot.frameRatio = 1.0;
		snapshot.confidence = 0.0;
		snapshot.discontinuities = tracker.Discontinuities();
		snapshot.lastDiscontinuity = cause;
		Record(std::string("discontinuity,,,,,1,0,,,,,") + "1," + cause);
	}

	void Monitor::Analyse(Job& j)
	{
		std::lock_guard<std::mutex> evidenceGuard(evidenceMutex);   // efficiency 2026-10-24: order evidenceMutex -> mutex
		const auto start = std::chrono::steady_clock::now();
		std::vector<SourceResolution::Spectrum> spectra(static_cast<size_t>(j.tiles));
		const int T = j.tileSize > 0 ? j.tileSize : kTile;
		const int pixels = T * T;
		std::vector<float> luma(static_cast<size_t>(pixels));
		std::vector<std::vector<float>> codeTiles(static_cast<size_t>(j.tiles));   // for the kernel fit
		for (int t = 0; t < j.tiles; ++t)
		{
			const float* src = j.values.data() + static_cast<size_t>(t) * pixels;
			for (int i = 0; i < pixels; ++i)
			{
				const double code = std::round(src[i] * j.scale);
				luma[i] = static_cast<float>((std::min)(static_cast<double>(j.codeMaximum), (std::max)(0.0, code)) / j.codeMaximum);
			}
			SourceResolution::Analyse(luma.data(), T, T, spectra[static_cast<size_t>(t)], j.codeMaximum);
			codeTiles[static_cast<size_t>(t)] = luma;
		}
		SourceResolution::Thresholds thresholds;
		thresholds.codeMaximum = j.codeMaximum;
		if (!textGuard.load(std::memory_order_acquire)) thresholds.textBand = 0.0;
		thresholds.mixedGuard = mixedGuard.load(std::memory_order_acquire);
		thresholds.codingFixes = codingFixes.load(std::memory_order_acquire);
		const bool fixes = LatchFixes();
		thresholds.latchFixes = fixes;
		thresholds.blackLevel = j.blackLevel;
		// The tiles' positions (the overlay spread) and the base grid for the frame signature:
		// switching between the base and the dense grid is neither a cut nor a new shot.
		if (j.tileX.size() == static_cast<size_t>(j.tiles) && j.tileY.size() == static_cast<size_t>(j.tiles))
		{
			thresholds.tileX = j.tileX;
			thresholds.tileY = j.tileY;
		}
		thresholds.signatureTiles = j.baseTiles;
		const SourceResolution::FrameReading frame = SourceResolution::ReadFrame(spectra, thresholds);

		// Source identity (doc 61 SID-2): the relative-noise measurement of the frame's informative tiles, taken BEFORE `mutex`
		// is held (the render thread's Read() never waits for it). Off (the default): nothing is measured.
		const bool idOn = sourceIdOn.load(std::memory_order_acquire);
		if (sourceIdResetPending.exchange(false, std::memory_order_acq_rel))
			sourceId.Discontinuity(snapshot.frames, streamKey.load(std::memory_order_acquire));   // switched on: a clean start
		sourceId.SetConsensusOn(idOn);
		std::vector<SourceIdentity::FrameQuality::TileNoise> idNoise;
		if (idOn)
		{
			constexpr size_t kMaxNoiseTiles = 4;   // bounded cost: the first informative tiles in tile order (deterministic)
			for (int t = 0; t < j.tiles && idNoise.size() < kMaxNoiseTiles; ++t)
				if (SourceResolution::Informative(spectra[static_cast<size_t>(t)], thresholds))
					idNoise.push_back(SourceIdentity::FrameQuality::MeasureNoise(codeTiles[static_cast<size_t>(t)].data(), T));
		}

		// Efficiency 2026-10-24: a unique_lock so the kernel fit (below) runs unlocked.
		std::unique_lock<std::mutex> lock(mutex);
		{
			const int shots = evidenceShots.load(std::memory_order_acquire);
			const double seconds = evidenceSeconds.load(std::memory_order_acquire);
			tracker.SetEvidence(shots, seconds);
			trackerV.SetEvidence(2 * shots, 2.0 * seconds);
			trackerH.SetEvidence(2 * shots, 2.0 * seconds);
			tracker.SetLatchFixes(fixes);
			const bool cut = CutFixes();
			tracker.SetCutFixes(cut);
			trackerV.SetCutFixes(cut);
			trackerH.SetCutFixes(cut);
			trackerV.SetLatchFixes(fixes);
			trackerH.SetLatchFixes(fixes);
		}
		// Latch fixes: a gap with no frames (or time going backwards) is a discontinuity of the whole
		// monitor with its own reason - not of the tracker alone (the kernel votes, the coherence
		// history and the per-axis state went on, and auto blamed "detail above the re-based height").
		if (fixes && lastAnalysedSeconds >= 0.0)
		{
			const double gap = j.seconds - lastAnalysedSeconds;
			if (gap < 0.0 || gap > tracker.GetSettings().maximumGapSeconds)
			{
				char text[96];
				if (gap < 0.0) snprintf(text, sizeof(text), "the source clock went backwards");
				else snprintf(text, sizeof(text), "a gap of %.1f s with no frames", gap);
				gapReason = text;
				ResetEvidence(gapReason.c_str());
				autoReset.store(true, std::memory_order_release);
			}
		}
		lastAnalysedSeconds = j.seconds;
		const unsigned long long trackerResets = tracker.Discontinuities();
		// Cut fixes: what the look-ahead saw between the tracker's analysed frames.
		const SourceResolution::EvidenceSource look = [this](uint64_t from, uint64_t to) { return LookAheadEvidence(from, to); };
		const SourceResolution::EvidenceSource* lookAhead = fixes && CutFixes() && j.sequence != 0 ? &look : nullptr;
		tracker.Update(frame, j.seconds, false, j.sequence, lookAhead);
		trackerV.Update(SourceResolution::AxisFrame(frame, true), j.seconds, false, j.sequence, lookAhead);
		trackerH.Update(SourceResolution::AxisFrame(frame, false), j.seconds, false, j.sequence, lookAhead);
		// Latch fixes: the tracker reset itself for a cause the check above did not see: the rest too.
		if (fixes && tracker.Discontinuities() != trackerResets)
		{
			gapReason = tracker.LastDiscontinuity();
			kernelVotes.clear();
			coherence.clear();
			coherenceV.clear();
			coherenceH.clear();
			live.clear();
			trackerV.Discontinuity(gapReason.c_str());
			trackerH.Discontinuity(gapReason.c_str());
			snapshot.lastDiscontinuity = gapReason;
			autoReset.store(true, std::memory_order_release);
		}
		// The dense grid while any tracker has a lower reading declared or in its window (from the
		// next copy on); back to the base grid once all are native again.
		denseGrid.store(tracker.Ratio() < 1.0 || tracker.Candidate() < 1.0 || trackerV.Ratio() < 1.0 || trackerV.Candidate() < 1.0 ||
			trackerH.Ratio() < 1.0 || trackerH.Candidate() < 1.0, std::memory_order_release);
		// The live reading (spec 1a): ~2 s of informative frames, snapped on a cut.
		if (tracker.Cuts() != lastCuts) { live.clear(); lastCuts = tracker.Cuts(); }
		while (!live.empty() && j.seconds - live.front().first > 2.0) live.pop_front();
		while (!coherence.empty() && j.seconds - coherence.front().first > 120.0) coherence.pop_front();
		while (!coherenceV.empty() && j.seconds - coherenceV.front().first > 120.0) coherenceV.pop_front();
		while (!coherenceH.empty() && j.seconds - coherenceH.front().first > 120.0) coherenceH.pop_front();
		// Latch fixes: the clarity behind a declaration comes from the frames since the last native
		// evidence (where the tracker's window starts again), not from 120 s holding a native shot.
		if (fixes)
		{
			auto since = [](std::deque<std::pair<double, double>>& c, const SourceResolution::Tracker& t) {
				if (t.Ratio() >= 1.0 && t.Candidate() >= 1.0) c.clear();
			};
			since(coherence, tracker);
			since(coherenceV, trackerV);
			since(coherenceH, trackerH);
		}
		if (frame.informative())
		{
			live.push_back({ j.seconds, frame.ratio });
			coherence.push_back({ j.seconds, (std::min)(frame.vertical.coherence, frame.horizontal.coherence) });
			coherenceV.push_back({ j.seconds, frame.vertical.coherence });
			coherenceH.push_back({ j.seconds, frame.horizontal.coherence });
		}
		std::vector<double> ratios;
		for (const auto& l : live) ratios.push_back(l.second);
		// Confidence of the declared value: the evidence (shots, seconds) and
		// how clearly both axes were resampled across the window.
		double coherenceMean = 0.0;
		for (const auto& c : coherence) coherenceMean += c.second;
		coherenceMean = coherence.empty() ? 0.0 : coherenceMean / coherence.size();
		// The evidence behind a tracker's declared value: its shots and seconds against its
		// standard - or, for a rendition change (F12, mid-shot, no shots), its seconds against
		// renditionSeconds alone, as the tracker itself declared it.
		auto evidenceOf = [](const SourceResolution::Tracker& t) {
			const SourceResolution::Tracker::Settings& s = t.GetSettings();
			if (t.DeclaredByRendition()) return (std::min)(1.0, t.EvidenceSeconds() / (std::max)(1e-9, s.renditionSeconds));
			// Latch fixes (owner 2026-10-17): the shots OR the time, whichever is met first.
			if (s.latchFixes)
				return (std::min)(1.0, (std::max)(t.Shots() / static_cast<double>((std::max)(1, s.minimumShots)),
					t.EvidenceSeconds() / (std::max)(1e-9, s.minimumSeconds)));
			return (std::min)(1.0, t.Shots() / static_cast<double>((std::max)(1, s.minimumShots))) *
				(std::min)(1.0, t.EvidenceSeconds() / (std::max)(1e-9, s.minimumSeconds));
		};
		const double evidence = evidenceOf(tracker);
		const double clarity = (std::min)(1.0, (std::max)(0.0, (coherenceMean - thresholds.coherence) / 0.3));

		// Spec 1b / 2d readings, smoothed over ~2 s (4 copies), snapped on a cut.
		const bool cutNow = live.size() <= 1;
		if (qualitySmoothed < 0.0 || cutNow) qualitySmoothed = frame.bitrateQuality;
		else qualitySmoothed += 0.25 * (frame.bitrateQuality - qualitySmoothed);
		if (frame.effectiveBits > 0.0) bitsSmoothed = bitsSmoothed <= 0.0 || cutNow ? frame.effectiveBits : bitsSmoothed + 0.25 * (frame.effectiveBits - bitsSmoothed);

		// Kernel-matched re-base: at a 2:1 reading (declared, or the one being gathered),
		// two informative tiles per analysed frame fitted against the candidate kernels;
		// accepted fits vote (docs/VP-stage12-build-plan.md). The kernel stands with 6
		// votes over 2 shots, 80% agreeing; a discontinuity clears it (below).
		{
			const double r = tracker.Ratio() < 1.0 ? tracker.Ratio() : tracker.Candidate();
			while (!kernelVotes.empty() && j.seconds - kernelVotes.front().seconds > 120.0) kernelVotes.pop_front();
			if (std::fabs(r - 0.5) <= 0.02 && frame.informative() && j.tiles > 0)
			{
				const int signalBits = static_cast<int>(std::lround(std::log2(j.codeMaximum + 1.0)));
				const double floorCodes = KernelInverse::FloorCodes(bitsSmoothed > 0.0 ? bitsSmoothed : signalBits, signalBits);
				// Efficiency 2026-10-24 (module pass): the fits (~25 ms each at 2:1) used to run with
				// `mutex` held, so the render thread's Read() (AutoFactor, the overlay) stalled ~50 ms
				// twice a second. The inputs are chosen under the lock (the counter, the informative
				// tiles, the shot), fitted unlocked, and the accepted votes published under the lock
				// in the same order. Every piece of evidence (trackers, votes, counter, bitsSmoothed)
				// is written only by this worker thread (ResetEvidence runs in Worker(); Discontinuity()
				// only sets an atomic), so the unlocked window cannot change a decision.
				int fitTiles[2] = { -1, -1 };
				int fitCount = 0;
				for (int k = 0; k < 2; ++k)
				{
					const int t = static_cast<int>((kernelFitCounter++) % static_cast<unsigned long long>(j.tiles));
					if (!SourceResolution::Informative(spectra[static_cast<size_t>(t)], thresholds)) continue;
					fitTiles[fitCount++] = t;
				}
				if (fitCount > 0)
				{
					const unsigned long long shot = tracker.Cuts();
					KernelInverse::TileFit fits[2];
					lock.unlock();
					if (kernelFitProbeForTest) kernelFitProbeForTest(*this);
					for (int k = 0; k < fitCount; ++k)
						fits[k] = KernelInverse::FitTile(codeTiles[static_cast<size_t>(fitTiles[k])].data(), T, T, j.codeMaximum);
					lock.lock();
					for (int k = 0; k < fitCount; ++k)
						if (KernelInverse::Accept(fits[k], floorCodes))
							kernelVotes.push_back({ j.seconds, fits[k].best, shot });
				}
			}
			int best = -1, bestCount = 0;
			std::vector<int> counts(KernelInverse::Candidates().size(), 0);
			unsigned long long firstShot = ~0ull, lastShot = 0;
			for (const KernelVote& v : kernelVotes)
			{
				if (v.candidate < 0 || v.candidate >= static_cast<int>(counts.size())) continue;
				if (++counts[static_cast<size_t>(v.candidate)] > bestCount) { bestCount = counts[static_cast<size_t>(v.candidate)]; best = v.candidate; }
				firstShot = (std::min)(firstShot, v.shot);
				lastShot = (std::max)(lastShot, v.shot);
			}
			const int votes = static_cast<int>(kernelVotes.size());
			const bool stands = votes >= 6 && lastShot > firstShot && bestCount * 5 >= votes * 4;
			snapshot.kernel = stands ? best : -1;
			snapshot.kernelVotes = votes;
			snapshot.kernelAgree = bestCount;
		}

		snapshot.reading = true;
		snapshot.nativeWidth = j.nativeWidth;
		snapshot.nativeHeight = j.nativeHeight;
		snapshot.frameRatio = frame.ratio;
		snapshot.frameBlank = !frame.informative();
		snapshot.frameBlack = frame.black;
		snapshot.liveRatio = ratios.empty() ? 1.0 : Median(ratios);
		// Latch precision (2026-10-17): the declared rate as the exact rendition behind it - an even
		// size of the frame within SourceResolution::kSnapBins of this tile's sweep, the simplest ratio (SnapRatio):
		// 128-px tiles at 1080 read a 540-line source as ~542, re-based 0.4 % off.
		snapshot.declaredRatio = SourceResolution::SnapRatio(tracker.Ratio(), j.nativeHeight, T);
		snapshot.confidence = tracker.Ratio() < 1.0 ? evidence * clarity : 0.0;
		// Per axis, only while the both-axes reading is native: each declared axis's
		// evidence (the stricter standard) times how clearly that axis was resampled.
		{
			auto axisConfidence = [&](const SourceResolution::Tracker& t, const std::deque<std::pair<double, double>>& c) {
				double mean = 0.0;
				for (const auto& e : c) mean += e.second;
				mean = c.empty() ? 0.0 : mean / c.size();
				const double ev = evidenceOf(t);
				return ev * (std::min)(1.0, (std::max)(0.0, (mean - thresholds.singleAxisCoherence) / 0.3));
			};
			snapshot.axisRatioX = snapshot.axisRatioY = 1.0;
			snapshot.axisConfidence = 0.0;
			if (tracker.Ratio() >= 1.0 && (trackerV.Ratio() < 1.0 || trackerH.Ratio() < 1.0))
			{
				snapshot.axisRatioY = SourceResolution::SnapRatio(trackerV.Ratio(), j.nativeHeight, T);
				snapshot.axisRatioX = SourceResolution::SnapRatio(trackerH.Ratio(), j.nativeWidth, T);
				double c = 1.0;
				if (trackerV.Ratio() < 1.0) c = (std::min)(c, axisConfidence(trackerV, coherenceV));
				if (trackerH.Ratio() < 1.0) c = (std::min)(c, axisConfidence(trackerH, coherenceH));
				snapshot.axisConfidence = c;
			}
		}
		snapshot.bitrateQuality = qualitySmoothed;
		snapshot.effectiveBits = bitsSmoothed;
		snapshot.coherenceV = frame.vertical.coherence;
		snapshot.coherenceH = frame.horizontal.coherence;
		snapshot.cuts = tracker.Cuts();
		snapshot.cutSpan = tracker.LastCutSpan();
		snapshot.moments = tracker.CutMoments();
		snapshot.momentSpan = tracker.LastMomentSpan();
		snapshot.lastCutSeconds = tracker.LastCutSeconds();
		snapshot.shots = tracker.Shots();
		snapshot.evidenceSeconds = tracker.EvidenceSeconds();
		snapshot.flashes = tracker.Flashes();
		snapshot.coveredSteps = tracker.CoveredSteps();
		snapshot.discontinuities = tracker.Discontinuities();
		++snapshot.frames;
		snapshot.analyseMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		snapshot.tiles = j.tiles;
		if (sourceIdNoticeDirty.exchange(false, std::memory_order_acq_rel))
			sourceId.SetNoticeEnabled(sourceIdNotice.load(std::memory_order_acquire));
		if (idOn)
		{
			SourceIdentity::FrameInput in;
			in.seq = j.sequence != 0 ? j.sequence : snapshot.frames;
			in.timeUs = static_cast<int64_t>(std::llround(j.seconds * 1e6));
			in.streamKey = j.streamKey;
			in.classical.reading = snapshot.reading;
			in.classical.declaredRatio = snapshot.declaredRatio;
			in.classical.confidence = snapshot.confidence;
			in.classical.axisRatioX = snapshot.axisRatioX;
			in.classical.axisRatioY = snapshot.axisRatioY;
			in.classical.axisConfidence = snapshot.axisConfidence;
			in.classical.kernel = snapshot.kernel;
			in.classical.kernelVotes = snapshot.kernelVotes;
			in.classical.kernelAgree = snapshot.kernelAgree;
			in.classical.bitrateQuality = snapshot.bitrateQuality;
			// "Detail above the re-based size": every tracker is back to native with nothing gathered below it, on an
			// informative frame (the classical safety veto; consensus may not hold a re-base the evidence no longer has).
			in.classical.detailAbove = frame.informative() && tracker.Ratio() >= 1.0 && tracker.Candidate() >= 1.0 &&
				trackerV.Ratio() >= 1.0 && trackerV.Candidate() >= 1.0 && trackerH.Ratio() >= 1.0 && trackerH.Candidate() >= 1.0;
			in.deliveredRatio = frame.informative() ? frame.ratio : 0.0;
			in.noise = idNoise;
			in.apl = frame.mean;
			in.blank = !frame.informative();
			in.cut = snapshot.cuts != sourceIdLastCuts;
			sourceIdLastCuts = snapshot.cuts;
			in.nativeHeight = j.nativeHeight;
			const unsigned long long eventsBefore = sourceId.Events();
			sourceId.Push(in);
			if (sourceId.Events() != eventsBefore)
				sourceIdEvents.fetch_add(sourceId.Events() - eventsBefore, std::memory_order_relaxed);
			snapshot.frameQuality = sourceId.LastFrameQuality();
			snapshot.frameNoise = sourceId.LastNoise();
		}
		char row[256];
		snprintf(row, sizeof(row), "reading,%.4f,%.3f,%.3f,%.4f,%.4f,%.2f,%.2f,%.1f,%llu,%d/%d,%.4f,%.1f ms",
			frame.ratio, frame.vertical.coherence, frame.horizontal.coherence, snapshot.liveRatio, snapshot.declaredRatio,
			snapshot.confidence, snapshot.bitrateQuality, snapshot.effectiveBits, snapshot.cuts, frame.informativeTiles,
			frame.tiles, autoAppliedShared.load(std::memory_order_acquire), snapshot.analyseMs);
		if (idOn)
		{
			SourceIdentity::Record rec;
			if (sourceId.Held().ReadLatest(0, rec))
			{
				char cols[200];
				snprintf(cols, sizeof(cols), ",sid,%.4f,%.2f,%d,%.4f,%.4f,%.4f,%s", snapshot.frameQuality, snapshot.frameNoise, rec.tier,
					rec.level, rec.ratioH, rec.ratioV, SourceIdentity::ReasonText(static_cast<SourceIdentity::ReasonCode>(rec.reason[static_cast<int>(SourceIdentity::Field::Quality)])));
				Record(std::string(row) + cols);
			}
			else
				Record(row);
		}
		else
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
		const bool fixes = LatchFixes();
		// The source frame interval, measured (the pan speed's time base).
		if (renderSequence > clockSequence && clockSequence != 0)
		{
			const double per = (now - clockSeconds) / static_cast<double>(renderSequence - clockSequence);
			if (per > 0.0 && per < 1.0) secondsPerFrame = secondsPerFrame > 0.0 ? secondsPerFrame + 0.1 * (per - secondsPerFrame) : per;
		}
		if (renderSequence > clockSequence) { clockSequence = renderSequence; clockSeconds = now; }
		if (autoReset.exchange(false, std::memory_order_acq_rel))
		{
			autoApplied = autoPending = 1.0;
			disengagePending = false;
			nextFadeSeconds = 0.0;   // a discontinuity: the old picture is not faded from
			if (before < 1.0) why = "native at once (" + (s.lastDiscontinuity.empty() ? std::string("discontinuity") : s.lastDiscontinuity) + ")";
		}
		double wanted = s.reading && s.declaredRatio < 1.0 && s.confidence >= minimumConfidence ? s.declaredRatio : 1.0;
		// Source identity (doc 61, auto engage): with source_id_consensus on and a fresh record, the classical reading may only
		// engage on a LATCHED field, and the target is the consensus's ratio. The classical confidence minimum still applies
		// (consensus can delay, veto or refine an engage, never add one). A stale or absent record: the classical value above,
		// visibly (SourceIdGateFallback).
		{
			const IdGate gate = SourceIdGate(now);
			if (gate.fresh && wanted < 1.0)
			{
				const double tol = 0.03;
				const bool uniform = gate.latchedH && gate.latchedV && std::fabs(gate.ratioH - gate.ratioV) <= tol * (std::max)(gate.ratioH, gate.ratioV);
				// Consensus moves only TOWARDS native: never a deeper re-base than the classical reading wants (a rise
				// in detail must not wait for the consensus dwell).
				const double target = uniform && gate.ratioH < 1.0 ? 0.5 * (gate.ratioH + gate.ratioV) : 1.0;
				wanted = (std::max)(wanted, target);
			}
		}
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
				nextFadeSeconds = fixes ? ReleaseFade() : 0.0;
				if (why.empty()) why = wanted >= 1.0 ? "native at once (detail above the re-based height)" : "raised at once (detail above the re-based height)";
			}
			else if (!disengagePending)
			{
				disengagePending = true;
				disengageSince = now;
				disengageCuts = s.moments;
				disengageFrames = s.frames;
				why = "confidence fell: native at the next cut or black (within 5 s)";
			}
			else if (s.moments != disengageCuts || (s.frameBlank && s.frames != disengageFrames) || now - disengageSince >= kDisengageWaitSeconds)
			{
				why = s.moments != disengageCuts ? "native at a cut (confidence fell)" :
					s.frameBlank && s.frames != disengageFrames ? "native during a black or flat frame (confidence fell)" :
					"native after 5 s without a cut (confidence fell)";
				autoApplied = autoPending = wanted;
				disengagePending = false;
				nextFadeSeconds = fixes ? ReleaseFade() : 0.0;
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
				autoPendingCuts = s.moments;
				autoPendingFrames = s.frames;
				autoPendingSince = now;
				why = fixes ? "confirmed: waiting for a cut, a black, a fast pan or a low-detail moment (else a crossfade)" :
					"waiting for the next cut or black";
			}
			else if (aheadDue)
			{
				// The frame after the cut / black, read ahead of display: it decides.
				const bool agrees = !a.informative || std::fabs(a.ratio - wanted) <= 0.02;
				std::lock_guard<std::mutex> lock(aheadMutex);
				if (agrees)
				{
					autoApplied = wanted;
					nextFadeSeconds = 0.0;
					++ahead.verified;
					why = "engaged at the cut / black (look-ahead verified)";
				}
				else
				{
					autoPending = autoApplied;
					autoNotBeforeCuts = s.moments + 1;   // the event's cut, when the monitor sees it, is not used
					++ahead.cancelled;
					why = "look-ahead: the next shot disagrees - staying";
				}
				ahead.eventSequence = 0;
				ahead.ready = false;
			}
			else if (fixes)
			{
				// Latch fixes (owner 2026-10-17): switch at the first cut, real black, fast pan or
				// low-detail moment; after the longest wait, a crossfade.
				const bool cutAllowed = !(s.moments <= autoNotBeforeCuts && autoNotBeforeCuts != 0);
				const int trigger = EngageTrigger(s, autoPendingCuts, autoPendingFrames, autoPendingSince, now, renderSequence, cutAllowed, why);
				if (trigger != 0)
				{
					autoApplied = wanted;
					nextFadeSeconds = trigger == 2 ? CrossfadeSeconds() : 0.0;
				}
			}
			else if (s.moments <= autoNotBeforeCuts && autoNotBeforeCuts != 0)
			{
				// After a look-ahead cancel: wait for a later cut.
			}
			else if (s.moments != autoPendingCuts)
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

	void Monitor::AutoFactors(int nativeWidth, int nativeHeight, double minimumConfidence, std::string& why,
		double& factorX, double& factorY, double seconds, uint64_t renderSequence)
	{
		const double uniform = AutoFactor(nativeHeight, minimumConfidence, why, seconds, renderSequence);
		const double now = seconds >= 0.0 ? seconds :
			std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
		const Snapshot s = Read();
		// A discontinuity: the per-axis state back to native too.
		if (s.discontinuities != axisDiscontinuities)
		{
			axisDiscontinuities = s.discontinuities;
			axisApplied[0] = axisApplied[1] = axisPending[0] = axisPending[1] = 1.0;
		}
		if (uniform < 1.0)
		{
			factorX = factorY = uniform;
			axisApplied[0] = axisApplied[1] = axisPending[0] = axisPending[1] = 1.0;
			Transit(factorX, factorY, now);
			return;
		}
		// The wanted per-axis factors: even sizes from the frame's own (general).
		double wanted[2] = { 1.0, 1.0 };
		if (s.reading && s.axisConfidence >= minimumConfidence)
		{
			const int sizes[2] = { nativeWidth, nativeHeight };
			double ratios[2] = { s.axisRatioX, s.axisRatioY };
			// Source identity: only latched axes engage, at the consensus's ratio (see AutoFactor).
			const IdGate gate = SourceIdGate(now);
			if (gate.fresh)
			{
				ratios[0] = ratios[0] < 1.0 && gate.latchedH ? (std::max)(ratios[0], gate.ratioH) : 1.0;
				ratios[1] = ratios[1] < 1.0 && gate.latchedV ? (std::max)(ratios[1], gate.ratioV) : 1.0;
			}
			for (int a = 0; a < 2; ++a)
				if (ratios[a] < 1.0 && sizes[a] > 0)
				{
					const int px = 2 * static_cast<int>(std::lround(ratios[a] * sizes[a] * 0.5));
					wanted[a] = px < sizes[a] ? static_cast<double>(px) / sizes[a] : 1.0;
				}
		}
		const double tolerance = 0.01;
		std::string axisWhy;
		const bool up = wanted[0] > axisApplied[0] + tolerance || wanted[1] > axisApplied[1] + tolerance;
		if (up)
		{
			// Detail returned on an axis, or the confidence fell: native at once (the
			// conservative direction; the per-axis path is the stricter one).
			axisApplied[0] = (std::max)(axisApplied[0], wanted[0]);
			axisApplied[1] = (std::max)(axisApplied[1], wanted[1]);
			if (wanted[0] >= 1.0) axisApplied[0] = 1.0;
			if (wanted[1] >= 1.0) axisApplied[1] = 1.0;
			axisPending[0] = axisApplied[0];
			axisPending[1] = axisApplied[1];
			axisWhy = "per axis: back towards native at once";
			nextFadeSeconds = LatchFixes() ? ReleaseFade() : 0.0;
		}
		else if (wanted[0] < axisApplied[0] - tolerance || wanted[1] < axisApplied[1] - tolerance)
		{
			// Down: at the next cut or black frame, as the uniform path.
			if (std::fabs(wanted[0] - axisPending[0]) > tolerance || std::fabs(wanted[1] - axisPending[1]) > tolerance)
			{
				axisPending[0] = wanted[0];
				axisPending[1] = wanted[1];
				axisPendingCuts = s.moments;
				axisPendingFrames = s.frames;
				axisPendingSince = now;
				axisWhy = "per axis: waiting for the next cut or black";
			}
			else if (LatchFixes())
			{
				std::string triggerWhy;
				const int trigger = EngageTrigger(s, axisPendingCuts, axisPendingFrames, axisPendingSince, now, renderSequence, true, triggerWhy);
				if (trigger != 0)
				{
					axisApplied[0] = wanted[0];
					axisApplied[1] = wanted[1];
					nextFadeSeconds = trigger == 2 ? CrossfadeSeconds() : 0.0;
					axisWhy = "per axis: " + triggerWhy;
				}
			}
			else if (s.moments != axisPendingCuts || (s.frameBlank && s.frames != axisPendingFrames))
			{
				axisApplied[0] = wanted[0];
				axisApplied[1] = wanted[1];
				axisWhy = s.moments != axisPendingCuts ? "per axis: engaged at a cut" : "per axis: engaged during a black or flat frame";
			}
		}
		if (!axisWhy.empty())
		{
			char row[200];
			snprintf(row, sizeof(row), "auto,,,,,%.4f,%.2f,,,%llu,,%.4f x %.4f,", s.axisRatioY, s.axisConfidence, s.cuts, axisApplied[0], axisApplied[1]);
			{
				std::lock_guard<std::mutex> lock(mutex);
				Record(row + axisWhy);
			}
			why = why.empty() ? axisWhy : why + "; " + axisWhy;
		}
		factorX = axisApplied[0];
		factorY = axisApplied[1];
		Transit(factorX, factorY, now);
	}

	int Monitor::EngageTrigger(const Snapshot& s, unsigned long long pendingCuts, unsigned long long pendingFrames, double pendingSince,
		double now, uint64_t renderSequence, bool cutAllowed, std::string& why)
	{
		// The look-ahead, while it runs, finds the cut on the frame itself: the tracker's (a tile copy
		// later) is not used then.
		const bool aheadLive = renderSequence != 0 && aheadLastSequence > renderSequence;
		const bool newFrame = s.frames != pendingFrames;
		// A cut found across a lost tile copy happened somewhere in a longer span: switching on it could
		// land mid-shot, so it waits for the next moment.
		if (cutAllowed && !aheadLive && s.moments != pendingCuts && s.moments != cutsNotUsable)
		{
			if (s.momentSpan <= 1.5 * intervalSeconds + 1e-6) { why = "engaged at a cut"; return 1; }
			cutsNotUsable = s.moments;
		}
		if (s.frameBlack && newFrame) { why = "engaged during a real black"; return 1; }
		// A fast pan: motion blur hides the change (MotionValidity's pair, at most two frames old).
		if (panSequence != 0 && renderSequence != 0 && renderSequence - (std::min)(renderSequence, panSequence) <= 2 &&
			secondsPerFrame > 0.0 && s.nativeHeight > 0)
		{
			const double heightsPerSecond = panPixelsPerFrame / secondsPerFrame / s.nativeHeight;
			if (heightsPerSecond >= kPanHeightsPerSecond) { why = "engaged during a fast pan"; return 1; }
		}
		// A moment of very low detail: no tile above the floor (nothing the change could show on).
		if (s.frameBlank && newFrame) { why = "engaged during a low-detail moment"; return 1; }
		if (now - pendingSince >= EngageMaxWait()) { why = "engaged by a crossfade (no cut within the wait)"; return 2; }
		return 0;
	}

	void Monitor::Transit(double factorX, double factorY, double now)
	{
		if (!LatchFixes())
		{
			fadeFromX = lastFactorX = factorX;
			fadeFromY = lastFactorY = factorY;
			fadeDuration = 0.0;
			nextFadeSeconds = 0.0;
			return;
		}
		if (std::fabs(factorX - lastFactorX) > 1e-9 || std::fabs(factorY - lastFactorY) > 1e-9)
		{
			// From what is on screen now (mid-fade: the path being left stays the 'from').
			const double w = FadeWeight(now - fadeStart, fadeDuration);
			if (w >= 1.0) { fadeFromX = lastFactorX; fadeFromY = lastFactorY; }
			fadeStart = now;
			// Back to what is being faded from (a reversal mid-fade): nothing left to fade.
			const bool same = std::fabs(fadeFromX - factorX) <= 1e-9 && std::fabs(fadeFromY - factorY) <= 1e-9;
			fadeDuration = same ? 0.0 : nextFadeSeconds;
			lastFactorX = factorX;
			lastFactorY = factorY;
		}
		nextFadeSeconds = 0.0;
	}

	Monitor::Transition Monitor::AutoTransition(double seconds) const
	{
		const double now = seconds >= 0.0 ? seconds :
			std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
		Transition t;
		t.toX = lastFactorX;
		t.toY = lastFactorY;
		t.weight = FadeWeight(now - fadeStart, fadeDuration);
		t.fromX = t.weight < 1.0 ? fadeFromX : lastFactorX;
		t.fromY = t.weight < 1.0 ? fadeFromY : lastFactorY;
		t.duration = fadeDuration;
		return t;
	}

	double Monitor::FadeWeight(double elapsed, double duration)
	{
		if (!(duration > 0.0) || elapsed >= duration) return 1.0;
		if (elapsed <= 0.0) return 0.0;
		return elapsed / duration;
	}

	void Monitor::FadeMixTimestamps(double weight, float timestamps[2])
	{
		// libplacebo's frame mixer with the triangle filter (radius 1) samples each frame's weight
		// at its timestamp: the old path at -w, the new at 1 - w give (1 - w, w); at w = 0 the new
		// path is outside the radius and at w = 1 the old one is, so the endpoints are each path alone.
		const double w = (std::min)(1.0, (std::max)(0.0, weight));
		timestamps[0] = static_cast<float>(-w);
		timestamps[1] = static_cast<float>(1.0 - w);
	}

	float Monitor::FadeMixValue(float oldValue, float newValue, double weight)
	{
		// libplacebo's pl_render_image_mix on two frames (renderer.c): weights from the triangle
		// filter, frames outside its radius skipped, contributions at or below 1e-3 culled (never
		// the frame nearest to 0), normalised by their sum, accumulated in float.
		float ts[2];
		FadeMixTimestamps(weight, ts);
		const int nearest = std::fabs(ts[0]) <= std::fabs(ts[1]) ? 0 : 1;
		float weights[2] = { 0.0f, 0.0f };
		float sum = 0.0f;
		for (int i = 0; i < 2; ++i)
		{
			if (std::fabs(ts[i]) >= 1.0f) continue;
			const float wi = 1.0f - std::fabs(ts[i]);
			if (std::fabs(wi) <= 1e-3f && i != nearest) continue;
			weights[i] = wi;
			sum += wi;
		}
		if (sum <= 0.0f) return nearest == 0 ? oldValue : newValue;
		float out = 0.0f;
		out += (weights[0] / sum) * oldValue;
		out += (weights[1] / sum) * newValue;
		return out;
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
