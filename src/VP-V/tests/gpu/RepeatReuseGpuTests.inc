// WP-24 captured-repeat render reuse: GPU byte-identity tests. WRITTEN, NOT RUN (no GPU today; the
// renderer-level part needs the capture harness and the 2.5 h timing gates). To run: apply
// docs/design/repeat-reuse-callsites.patch, include this from src/VideoProcessor-BetaTests/BetaGpuTests.cpp
// (after its Gpu/Check helpers) and call RepeatReuseGpu::Run(gpu).
//
// WHAT "IDENTICAL" MEANS. A reused repeat must equal a full re-render of the same refresh, byte for
// byte at the output bit depth, with these and only these exceptions:
//   - the temporal dither PHASE: both paths call pl_render_image once per refresh, so libplacebo's
//     temporal dither and VP's sigma-delta advance identically; the comparison therefore pairs the
//     refresh N of the reuse run with refresh N of the full run (same dither frame index). If a
//     dither keyed on render count ever differs, compare with dither off and report the phase-only
//     difference separately (expected: identical);
//   - temporal per-RENDER state the policy refuses anyway (peak detection, frame mixing,
//     deinterlacing, crossfades): covered as "refused, rendered in full", not compared.
//
// R1 (unit, this file): one libplacebo renderer, synthetic 4K P010 source, target 2560x1600 (FL35)
//    and 3840x2160; for each case: render the first present with the cache armed, then refresh 2
//    (a) in full and (b) via VpRepeatReuse::Adapter::DrawFromCache, each into its own target texture;
//    download both; require 0 differing bytes.
//    Cases (each must be identical): SDR BT.709 limited; HDR10 PQ -> SDR tone map (peak detect OFF);
//    target LUT on (cache already carries it: the reuse clears target.lut); integer and FRACTIONAL
//    target crops (Rounded rects: the black-border edge pixels must match); NLS on (warped geometry);
//    a target overlay (OSD/subtitle overlay drawn on both); sigma-delta OUTPUT hook with
//    static_refresh = 1 on both; OutputEncodingRepair needed (7.360) and not needed (377);
//    fbo_float32 on and off (the cache format follows the FBO format); BTB/WTW probes (codes 4..64,
//    940..1019) unchanged.
// R2 (renderer harness, capture): 24-in-30 and 30-in-60 clips (Apple TV recordings), repeat_render_reuse
//    on vs off: all presented frames byte-identical; the status line shows "Repeats reused: N of M"
//    with N/M >= 0.95 on steady content (refusals only at cuts/start/overlay changes), 0 draw failures;
//    LED: the drive stays identical (the repeat refresh has no new measurement, as on the extras).
// R3 (timing gate): GPU time per repeat refresh drops by the main render's share (audit #4 estimate:
//    ~2-6 ms per repeat refresh on the laptop, 20% of refreshes at 24-in-30, 50% at 30-in-60); the
//    delay gate totals are unchanged (reuse changes no queue decision).

namespace RepeatReuseGpu
{
	struct Case
	{
		const char* name;
		int targetW, targetH;
		bool hdrSource, lut, fractionalCrop, overlay, sigmaDelta, fbo32;
	};

	inline void CompareFullVsReuse(Gpu& gpu, pl_gpu g, pl_renderer r, const Case& c)
	{
		// Source and target setup reuse BetaGpuTests' synthetic plane/target helpers (MakeP010Frame,
		// MakeTarget, Download) - names to be matched to the file when this is wired in.
		pl_frame source = MakeP010Frame(gpu, g, 3840, 2160, c.hdrSource);
		pl_frame targetFull = MakeTarget(g, c.targetW, c.targetH, c.lut, c.fbo32);
		pl_frame targetReuse = MakeTarget(g, c.targetW, c.targetH, c.lut, c.fbo32);
		if (c.fractionalCrop)
		{
			targetFull.crop = { 10.4f, 6.6f, c.targetW - 9.7f, c.targetH - 5.2f };
			targetReuse.crop = targetFull.crop;
		}
		DejudderRendered::Cache cache;
		std::vector<const pl_hook*> hooks = c.sigmaDelta ? SigmaDeltaHooks(gpu, g) : std::vector<const pl_hook*>{};
		std::vector<const pl_hook*> firstHooks = hooks;
		cache.Arm(1);
		firstHooks.push_back(cache.Hook());
		pl_render_params params = pl_render_default_params;
		params.peak_detect_params = nullptr;
		params.hooks = firstHooks.data();
		params.num_hooks = static_cast<int>(firstHooks.size());
		Check(pl_render_image(r, &source, &targetFull, &params), "R1 first present");
		Check(cache.Texture(1) != nullptr, "R1 cache captured");
		// Refresh 2, (a) full re-render without the cache copy.
		params.hooks = hooks.empty() ? nullptr : hooks.data();
		params.num_hooks = static_cast<int>(hooks.size());
		Check(pl_render_image(r, &source, &targetFull, &params), "R1 full re-render");
		// Refresh 2, (b) reuse - a second renderer at the same dither phase (one prior render each).
		pl_renderer r2 = pl_renderer_create(gpu.Log(), g);
		Check(pl_render_image(r2, &source, &targetReuse, &params), "R1 phase-aligning render");
		const DejudderRendered::Rects rounded = DejudderRendered::Rounded(source.crop, targetReuse.crop,
			static_cast<float>(c.targetW), static_cast<float>(c.targetH));
		VpRepeatReuse::Adapter adapter;
		adapter.Init(g);
		Check(adapter.DrawFromCache(r2, cache.Texture(1), cache.Repr(), cache.Color(), rounded.target, params, hooks, {},
			nullptr, targetReuse), "R1 reuse draw");
		const std::vector<uint8_t> a = Download(g, targetFull), b = Download(g, targetReuse);
		size_t differing = 0;
		for (size_t i = 0; i < a.size() && i < b.size(); ++i)
			differing += a[i] != b[i];
		std::cout << "  R1 " << c.name << ": " << differing << " differing bytes\n";
		Check(a.size() == b.size() && differing == 0, std::string("R1 reuse differs from a full re-render: ") + c.name);
		pl_renderer_destroy(&r2);
		cache.Release();
	}

	inline void Run(Gpu& gpu, pl_gpu g, pl_renderer r)
	{
		const Case cases[] = {
			{ "sdr 1600p", 2560, 1600, false, false, false, false, false, true },
			{ "hdr->sdr 1600p", 2560, 1600, true, false, false, false, false, true },
			{ "lut", 2560, 1600, false, true, false, false, false, true },
			{ "fractional crop", 2560, 1600, false, false, true, false, false, true },
			{ "overlay", 2560, 1600, false, false, false, true, false, true },
			{ "sigma-delta", 2560, 1600, false, false, false, false, true, true },
			{ "fp16 fbo", 3840, 2160, true, false, true, false, true, false },
		};
		for (const Case& c : cases)
			CompareFullVsReuse(gpu, g, r, c);
	}
}
