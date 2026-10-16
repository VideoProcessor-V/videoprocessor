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
	}

	Monitor::~Monitor()
	{
		{
			std::lock_guard<std::mutex> lock(mutex);
			stopping = true;
		}
		wake.notify_all();
		if (worker.joinable()) worker.join();
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
				}
				if (!haveJob) continue;
				current = std::move(job);
				haveJob = false;
			}
			Analyse(current);
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

	double Monitor::AutoFactor(int nativeHeight, double minimumConfidence, std::string& why)
	{
		why.clear();
		const Snapshot s = Read();
		const double before = autoApplied;
		if (autoReset.exchange(false, std::memory_order_acq_rel))
		{
			autoApplied = autoPending = 1.0;
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
			// Up (towards native): at once.
			autoApplied = autoPending = wanted;
			if (why.empty()) why = wanted >= 1.0 ? "native at once (detail above the re-based height, or confidence fell)" : "raised at once";
		}
		else if (wanted < autoApplied - tolerance)
		{
			// Down: wait for the next cut so the picture never changes mid-shot.
			if (std::fabs(wanted - autoPending) > tolerance)
			{
				autoPending = wanted;
				autoPendingCuts = s.cuts;
				why = "waiting for the next cut";
			}
			else if (s.cuts != autoPendingCuts)
			{
				autoApplied = wanted;
				why = "engaged at a cut";
			}
		}
		autoAppliedShared.store(autoApplied, std::memory_order_release);
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
