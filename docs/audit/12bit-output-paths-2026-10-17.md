# 12-bit output paths (research, 2026-10-17)

Owner: 12-bit output will be necessary for a future display (not the FL35, whose link is 8-bit).

> **Owner decision 2026-10-17 (FINAL), overrides recommendations 3 and 4 and route 1 below:**
> no DeckLink output card. 12-bit output only via VPV (VP sends a TV-led DV tunnel through the
> NVIDIA GPU, NVAPI `NV_HDR_MODE_DOLBY_VISION`), for DV displays. FP16 half-float output is PARKED
> (rounding/dither unknowns). Non-DV displays stay on the 10-bit 4:4:4 surface + sigma-delta. The
> research below is kept as written.

## Findings

- **D3D12 and Vulkan add no 12-bit output on Windows.** Both present through the DXGI/DWM flip
  model. That model accepts only these back-buffer formats:
  - R16G16B16A16_FLOAT
  - B8G8R8A8_UNORM / R8G8B8A8_UNORM
  - R10G10B10A2_UNORM

  No API has a 12-bit integer swapchain format.
  - **Vulkan:** 16-unorm surfaces are rare on Windows (about 9.7% of reports, likely not NVIDIA),
    and composition still goes through DXGI.
  - **NVIDIA "12 bpc":** this sets the link depth only. How a 10-bit or FP16 surface fills it is
    undocumented.
- **A rebase would not pay off for 12-bit output.**
  - **D3D12:** libplacebo has no D3D12 backend, so this is unrealistic.
  - **Vulkan:** libplacebo's backend is mature, but VP's own D3D11 code would have to be ported:
    renderer host and swapchain telemetry, DejudderWarp, the motion producer, GPU timers, the NVOF
    wrapper, GPU unpack and early upload, and about 13 files that touch D3D11 directly.
  - **Benefit:** async compute and zero-copy upload. **Cost:** several months and re-qualifying the
    whole chain gate. No 12-bit benefit.

## Real routes, ranked

1. **DeckLink output card (bit-exact).** VP schedules its own R12B/R12L frames through
   IDeckLinkOutput.
   - **Keeps:** exact codes, studio range (BTB/WTW) and the sigma-delta model.
   - **Candidate:** DeckLink 8K Pro G2 (HDMI 2.1, 8/10/12-bit, 4:4:4, UHD up to 120p). Blackmagic
     should confirm 2160p60 12-bit RGB over HDMI. The UltraStudio 4K Mini manages 12-bit RGB only
     up to 2160p30 (HDMI 2.0).
   - **New work:** a render-to-texture target, readback (or DVP/GPUDirect), R12B packing (the
     inverse of the native RGB unpack), scheduled playback on the card clock, HDR metadata, and
     roughly 1-3 frames of extra latency.
   - **Scope:** weeks. The renderer core stays as it is.
2. **FP16 scRGB surface plus a linear-conversion pass.** Full range only: BTB/WTW clipped on studio
   output, and the sigma-delta loses control of the codes. FP16 holds about 11 bits near white. The
   NVIDIA FP16-to-12-bit conversion is undocumented and would have to be measured. Dropped by the
   owner on 2026-10-15; build only on request.
3. **NVIDIA professional APIs.** Not viable: they stop at 10 bits. DVP/GPUDirect only helps the
   DeckLink readback.

## Which displays benefit

- **Consumer panels** are mostly 10-bit. A 12-bit input pays off only if the display processes it
  at that depth.
- **Dolby Vision** does not need a 12-bit link. Its tunnel carries 12-bit inside 8-bit RGB 4:4:4.
  So for a DV-capable future display, the high-fidelity route is for VP to *send* a DV tunnel. That
  is a bit-exact packing problem, and DeckLink output or exact 8-bit RGB pass-through serves it
  better than a 12-bit swapchain.

## Recommendation

1. No D3D12/Vulkan rebase for 12-bit. Reconsider Vulkan only if async compute or zero-copy upload
   becomes a measured bottleneck.
2. When the display is chosen, confirm from its EDID or specs that it accepts 12-bit and processes
   it at depth. If it is a DV display, plan to send it a DV tunnel instead.
3. If 12-bit with BTB/WTW is required, the DeckLink 8K Pro G2 output module is the path, after
   Blackmagic confirms 4K60 12-bit RGB over HDMI.
4. Keep FP16 as a possible full-range fallback, built only if the owner asks.

## Sources

- https://learn.microsoft.com/en-us/windows/desktop/api/dxgi1_2/ns-dxgi1_2-dxgi_swap_chain_desc1
- https://vulkan.gpuinfo.org/listsurfaceformats.php?platform=windows
- https://www.blackmagicdesign.com/products/decklink/techspecs/W-DLK-46
- https://www.blackmagicdesign.com/developer/products/capture-and-playback/models
- https://www.cepro.com/audio-video/media/tips-to-turning-on-dolby-vision/
