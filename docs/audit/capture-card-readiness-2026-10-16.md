# Capture card readiness (2026-10-16 night)

Owner: a new capture card will be integrated soon (model not chosen; cost-effective). This is what
the code assumes today, the bugs found, and the first-day test checklist.

## Adding a card

- **DeckLink / UltraStudio:** discovered automatically (IDeckLinkDiscovery); limited by the hard-coded
  mode table (`BD_DISPLAY_MODE_DATA`, BlackMagicDeckLinkTranslate.cpp) - an unknown mode throws.
- **Other Magewell families / vendors:** implement `ACaptureDevice` + `ITimingClock` + a discoverer
  (model on `magewell/*`), register in VideoProcessorDlg (~2417 create, ~14109 start, ~15225 stop) and
  the Config device-name query; frames must arrive as an existing `VideoFrameEncoding` packed exactly as
  Blackmagic packs it (`VideoFrame` has no stride; every consumer uses `VideoState::BytesPerRow()`).
- Hard-coded today: 23-120 Hz refresh (DisplayMode and Magewell), range implied by the encoding (no range
  field), Magewell Pro Capture family only (Eco/USB skipped, no hot-plug), Magewell captures ARGB /
  RGB10 / P210 only (4:4:4 YCbCr and 12-bit RGB reduced to 10-bit 4:2:2 by the card), DeckLink state
  valid only with metadata extensions, DV tunnel r210-only (never on Magewell), DeckLink EDID rewritten
  for every card at start-up, device chosen by display name (two identical cards ambiguous).

## Bugs found (capture paths)

1. DeckLink `bmdMode4kDCI2997` = 30000/1000 (should be /1001); `bmdMode4kDCI60` missing.
2. Unknown mode during a DeckLink format change throws inside the callback (swallowed) - capture stalls.
3. No recovery from capture errors (modal MessageBox over fullscreen; DeckLink resync failure leaves
   capture dead; a Magewell thread abort leaves state CAPTURING with no thread).
4. Magewell missed-frame count always 0 (DirectShow never sees drops; stats show 0).
5. DeckLink counter repeats after start/resync (first frame not counted).
6. DeckLink row size checked only in Debug - a padded row shears silently in Release.
7. DeckLink hot-unplug refcount: `Attach(new ...)` without AddRef and an uninitialised atomic
   `m_refCount` (likely use-after-free on Thunderbolt unplug).
8. Exceptions thrown out of `DeckLinkDeviceArrived` across the COM boundary.
9. `SendCardStateCallback` can throw from `StartCapture` (unknown mode / no signal).
10. DeckLink `ProfileChanging` just throws (multi-profile cards stop silently).
11. GUI null dereference of `GetTimingClock()` on a stale CAPTURING message; FAILED hits `assert(false)`.
12. Magewell HDR primaries assumed R,G,B order (ST 2086 commonly G,B,R) - assign by chromaticity.
13. Magewell 8-bit RGB forced FULL range - limited sources lose BTB/WTW (owner rule).
14. Magewell drops a frame when a notification carries both FRAME_BUFFERED and a format bit.
15. Small: `DisplayMode::NanosecondsPerFrame()` returns nothing; bits-per-pixel table lacks R10b/R10l/R12L;
    NTSC 486-line 601 maps to UNKNOWN.
Load risks: Magewell repacks 4K on its notify thread with scalar loops (latency/skips at 4K60); driver
buffers held through the queue and look-ahead ("Capture raw route: up to N capture buffers held").

## First-day checklist

1. Discovery/identity; restart with the card selected; vp.log packing and geometry lines.
2. Rates: 1080p/2160p at 23.976/24/25/29.97/30/50/59.94/60, 4K DCI 24/29.97/60, 1080i, 100/120 if
   offered, one out-of-range source (clean rejection, no modal box).
3. Formats with BTB/WTW bars and 0/1023 ramps: YCbCr 4:2:2 8/10/12, 4:4:4 10/12, 4:2:0 10 at 4K60, RGB
   8/10/12 full and limited; which encoding and whether the GPU unpack ran.
4. HDR: PQ with known mastering metadata (primaries order, min/max, MaxCLL/MaxFALL in the log), HLG,
   SDR<->PQ mid-stream, InfoFrame dropped while PQ.
5. Format switches (rate, resolution, depth, RGB<->YCbCr, same-format re-handshake): exactly one reset.
6. Signal loss / hot-plug (HDMI, source power cycle, Thunderbolt/USB unplug).
7. Missed frames under GPU load (stats vs "capture missed N frame(s)").
8. Buffer pool at 4K60 with max queue and look-ahead.
9. DV tunnel if the card gives bit-exact 8-bit RGB LSBs.
10. Latency (photodiode/flash); re-tune the frame offset per card.
11. DeckLink profiles/duplex changes while capturing.

## Generalisations to make (effort)

- Mode from `IDeckLinkDisplayMode` (width/height/GetFrameRate) instead of the table; fix DCI 29.97 (S).
- `rowBytes` on `VideoFrame`, used everywhere instead of `BytesPerRow()`; Release check meanwhile (M).
- `sampleRange` / bit depth / chroma siting on `VideoState`, set by the device (M).
- One format capability table driving BytesPerRow, Supports(), the formatter factory, the tunnel gate (M).
- Refresh limit as a policy, up to ~240 Hz (S).
- Capture error/recovery policy (log + status + automatic restart with back-off) (M).
- DeckLink refcount + callback boundary (S); Magewell missed frames from timestamps/IDs (S).
- Magewell HDR primaries by chromaticity, 4:4:4 path, repack off the notify thread / onto the GPU (M).
- DV tunnel encoding-agnostic (M); DeckLink input frame allocator (M); Magewell Eco/USB + hot-plug (L).

## DV tunnel at 60 Hz (2026-10-17)

Full DV tunnel coverage needs bit-exact 8-bit RGB 4:4:4 at the source rate. Bar for 60 Hz DV: 2160p60 8-bit RGB 4:4:4 capture, untouched (any YUV/4:2:0 conversion destroys the tunnel). DeckLink Quad HDMI Recorder: manufacturer-style specs list RGB 4:4:4 only up to 4Kp30 and YUV (possibly 8-bit 4:2:0) at 2160p50/60; some reseller listings claim RGB to 4K60 - unverified. Implication: 24/25/30p DV tunnels at 12-bit; 50/60p DV would not. Verify with Blackmagic's manual and a live test (SDK pixel format reported for 4K60 RGB input via the Vertex2).
