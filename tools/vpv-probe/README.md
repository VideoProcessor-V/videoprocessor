# vpv-probe: NVIDIA experimental Dolby Vision output harness (VPV)

VPV means VP sending a TV-led Dolby Vision RGB tunnel to a display through
the NVIDIA GPU, using the experimental NVAPI mode `NV_HDR_MODE_DOLBY_VISION`
(value 7). This tool is a research harness. It is not part of VP and is not
in the solution. It does not carry a Dolby licence, so never label its output
"Dolby Vision".

**Read the safety section before the first live run.** NVIDIA's forum thread
199530 (Jan 2022, still open in Oct 2026) reports that turning the mode off
with `NV_HDR_MODE_OFF` leaves the TV black in DV mode. In that report only a
reboot fully recovered it.

## The mode's contract (from 3rdparty/nvapi/nvapi.h)

| Item | Value |
|---|---|
| Mode | `NV_HDR_MODE_DOLBY_VISION = 7`, grouped under "Experimental", described as "Experimental mode only, not for production". Only `NV_HDR_MODE_UHDA` is a production mode. |
| Source surface | RGB8: 12-bit YCbCr 4:2:2 packed into RGB8. The application encodes frames in DV format and embeds the DV dynamic metadata in the pixels. The driver gets no DM through any struct. |
| Call | `NvAPI_Disp_HdrColorControl(displayId, NV_HDR_COLOR_DATA_V2*)`, with `cmd = NV_HDR_CMD_SET` and `hdrMode = 7` |
| `NV_HDR_COLOR_DATA_V2` colour fields | `hdrColorFormat`, `hdrDynamicRange`, `hdrBpc`. In DV mode only certain combinations are valid. If the combination is invalid or missing, the driver forces **RGB, full range (VESA), 8 bpc**. That forced default is the tunnel, and it is the probe's default (`--wire rgb8`). |
| Static metadata | `mastering_display_data` (ST 2086 + MaxCLL/MaxFALL). The header does not say whether DV uses it. The probe fills DCI-P3 D65, 4000/0.005 nits. |
| Capabilities | `NV_HDR_CAPABILITIES_V2/V3.isDolbyVisionSupported` plus `dv_static_metadata`, the parsed DV VSVDB: version, DM version, 2160p60, YUV422-12, global dimming, P3 colorimetry, interface (standard/low latency), 10b/12b 444, target luminance and primaries (encoded per the DV HDMI spec). V3 adds `parity`, HDR10+ and NVIDIA-certified. |
| Introduced in | Not determinable from the header. NVIDIA's public history (R530+) already contains it. Driver support dates from GeForce 378.92 (Mar 2017, Mass Effect Andromeda "DV for games"). NVIDIA staff call it the "legacy DV" path and say newer TVs may not support it. |
| Bundled SDK | Interface "NVidia Complete Version 1.10". The test laptop's driver is 610.60 (r610_53). |

## Tunnel packing (matches VP's decoder)

`VpvTunnelPacker.h` writes exactly what VP's `CR210DolbyVisionTunnelFormatter` and
`GpuDolbyVisionTunnelUnpack` decode, without the DeckLink 10-bit container step:

- **Pixel bytes (HDMI 1.4a 6.5.4):** R = C[11:4], G = I[11:4], B = C[3:0]<<4 | I[3:0]. Cb/P is on even pixels, Cr/T on odd pixels. In B8G8R8A8 memory the byte order is B, G, R, A.
- **Packets (ETSI GS CCM 001 clause 6):**
  - Each packet is 128 bytes, with a CRC-32/MPEG-2 over bytes 0-123.
  - Each packet is sent three times, as 1024-bit copies at pixels P*3072 + copy*1024 + bit, in raster order from the top-left active pixel.
  - Each bit is XORed with the parity of chroma bits [11:1] and the 12 bits of the luma that follows it. In the RGB8 word that luma is the same pixel's I.
- **DM body:**
  - It follows the layout that VP's `ParseMetadata` accepts: 12-bit, YCbCr or IPT, 4:2:2, full range, with an L1 extension block.
  - The presets use the commonly published profile 8.x / profile 5 matrices. **Check them against a DM captured from the Apple TV** before you trust a TV's rendering.
  - `--dm-file` sends a captured body verbatim.

`--self-test` packs a frame, decodes it with VP's own
`DolbyVisionTunnelUnpackReference`, and CRC-checks every packet copy. It
runs on the CPU only.

## Modes

```
vpv-probe --dry-run [--display \\.\DISPLAYn]   queries only (also the default)
vpv-probe --self-test [--width W --height H] [--dm-preset ipt] [--dump-frame f.bgra]
vpv-probe --i-am-present --display \\.\DISPLAYn --no-mode --seconds 30
vpv-probe --i-am-present --display \\.\DISPLAYn --seconds 10
vpv-probe --recover-only --i-am-present [--display \\.\DISPLAYn]
```

- **Dry run** calls only `NvAPI_Initialize`, interface/driver version, GPU enumeration, `GetConnectedDisplayIds`, `GetHdrCapabilities`, `GetOutputMode`, `GetDisplayIdByDisplayName`, DXGI output descriptors and `EnumDisplaySettings`. It never calls `HdrColorControl`, even with GET.
- **`--i-am-present`** is required for anything that opens a fullscreen window or changes display state. `--recover-only` refuses to run without it.
- **`--no-mode`** shows the tunnel frame as an ordinary SDR picture. It makes no NVAPI HDR call, so it carries no stuck-mode risk. It is the loopback pixel test.

## Live sequence and exit ladder

1. Run the queries. Refuse if the target is not driven by NVIDIA, the sink lacks `isDolbyVisionSupported` (unless `--ignore-caps`), Windows HDR is on, or the NVAPI HDR mode is not already OFF.
2. Take a snapshot: `HdrColorControl(GET)`, `ColorControl(GET)`, the current DEVMODE.
3. Arm the safety nets:
   - console Ctrl+C/close handler
   - unhandled-exception filter, `std::terminate` and SIGABRT handlers
   - a **watchdog thread** that restores at seconds + grace + 5 s
   - a **guardian process** (the same exe, `--guardian`). It restores when the probe dies without signalling a clean exit (kill, hard crash), or hangs 30 s past the deadline, and then terminates the probe.
4. Create a borderless window covering the output exactly, with a flip-discard B8G8R8A8 swapchain, `DXGI_SCALING_NONE` and `RGB_FULL_G22_NONE_P709`. Present the tunnel frame for 0.5 s.
5. Write the state file `%LOCALAPPDATA%\vpv-probe\active-state.bin`. Call `HdrColorControl(SET DOLBY_VISION, RGB/VESA/8)`, read the result back with GET, and read the AVI InfoFrame with GET.
6. Present for `--seconds` (1-120), logging the present count and frame statistics once a second. Esc stops early.
7. Exit: `SET OFF`, 0.5 s of black SDR frames, release the swapchain, then the ladder. Each step is logged and verified.
   1. `SET OFF` three times, 1.5 s apart, then GET to verify. This was NVIDIA's forum suggestion.
   2. `SET UHDA`, then `SET OFF`, then verify. This step is skipped with `--exit-minimal` when step 1 verified.
   3. `ColorControl(SET)` to restore the snapshot's format, range and bpc.
   4. `ChangeDisplaySettingsExW(target, saved mode, CDS_RESET)`, a forced modeset of the target only.
   5. The probe asks the owner (30 s timeout) whether the picture is normal. If the answer is no, or the driver still reports HDR on, it runs `SetDisplayConfig(SDC_APPLY | SDC_USE_DATABASE_CURRENT)` (every display blinks), then OFF and verify again.
   6. If the picture is still wrong, the probe prints the manual steps below.
8. The state file is deleted only after a verified recovery. A leftover file is reported by the next dry run.

Every NVAPI call is logged with its status code, message and duration to the
console and to `--log` (default `vpv-probe.log`). The log is flushed after every line.

## Owner test procedure

The owner must be present. Close VP first.

### Stage 0: on the bench, no DV sink needed

1. Run `vpv-probe --self-test`. It must print PASS.
2. Connect the HDMI cable, then run `vpv-probe --dry-run`. Note:
   - which `\\.\DISPLAYn` has an NVIDIA display id. On this laptop (RTX 3050 Ti + Intel UHD), the internal panel has none. Whether HDMI is wired to the NVIDIA GPU shows up only once something is plugged in.
   - `DolbyVision=1` and the VSVDB fields. Interface 0 = standard (TV-led), which is the interface this tunnel needs.

### Stage 1: loopback pixel check, laptop HDMI into the UltraStudio 4K Mini

1. Set the HDMI output in Windows to the capture mode you want (for example 1080p24, 8-bit RGB full). Turn off Night light, HDR, ICC profiles and NVIDIA colour or vibrance adjustments.
2. Start VP on the DeckLink input with the DV tunnel path (r210). Run:
   `vpv-probe --i-am-present --display \\.\DISPLAYn --no-mode --seconds 60`
3. Pass when VP logs `CRC-validated r210 RGB8 alignment=...` and its periodic tunnel stats show `CRC_failed_copies=0` for the whole run. The picture must show the ramp. That proves the swapchain-to-wire path is bit-exact in SDR.
4. For a full-frame comparison, run `--self-test --width W --height H --dump-frame expected.bgra` and compare it with a raw DeckLink capture of the same mode.
5. Optional, with the DV signal itself:
   - Add `--ignore-caps` and drop `--no-mode`. The UltraStudio's EDID has no DV VSVDB, so the driver may refuse; log the return code.
   - If the driver accepts, VP must still lock with CRC-clean packets.
   - This step tests the stuck-mode recovery on a sink that cannot get stuck in DV, so it is the safe place to rehearse the exit ladder.

### Stage 2: an external DV TV

Use a TV with an HDMI DV VSVDB, standard interface.

1. Run the dry run. Confirm `DolbyVision=1` and interface 0.
2. Run `vpv-probe --i-am-present --display \\.\DISPLAYn --seconds 10 --pattern ramp`
3. Expect the TV's DV indicator and a grey ramp from black to the TV's peak. If you see black with the DV logo, the TV rejected the metadata: try `--dm-preset ipt`, or `--dm-file` with a DM captured from the Apple TV.
4. Answer the probe's question after exit honestly. Answering "n" escalates.
5. Keep the log file and send it back.

## Manual recovery if the display stays stuck

Try these in order and stop when the picture is back:

1. Run `vpv-probe --recover-only --i-am-present` (add `--display \\.\DISPLAYn` if there is no state file).
2. Press **Win+Ctrl+Shift+B** to restart the graphics stack. The screens blink.
3. Unplug the HDMI cable for 10 s, then plug it back in (hot-plug and link retrain).
4. In Device Manager, disable and then re-enable the NVIDIA display adapter. In an admin PowerShell:
   `Get-PnpDevice -Class Display -FriendlyName '*NVIDIA*' | Disable-PnpDevice -Confirm:$false`, then the same with `Enable-PnpDevice`.
   One forum user reports this recovers the desktop.
5. Unplug the TV at the mains for 30 s.
6. Reboot. This is the only fix every public report confirms.

## Not known yet

- Whether this driver (r610) still implements mode 7 on a laptop GPU.
- Whether the exit ladder beats the stuck-mode bug. Steps 2, 4 and 5 are untested anywhere public.
- The InfoFrame the driver sends. It is presumably the Dolby VSIF (OUI 0x00D046, tunnel mode), but that is undocumented.
- Whether `--wire yuv422-12` is a combination the driver accepts.
