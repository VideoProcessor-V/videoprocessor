# Dolby Vision input: automated test system (loopback harness)

Status 2026-10-18, branch `test/dv-tunnel-auto`. This covers VP's Dolby Vision
INPUT side - capture -> tunnel detection -> 12-bit IPT 4:2:2 unpack from 8-bit RGB
4:4:4 -> metadata (DM) -> presentation decision -> render - and every other incoming
HDR mode (LLDV, HDR10 with / without InfoFrame, SDR). It is built around a
self-contained loop: VP's own DV output ("VPV") -> HDMI -> a capture input -> VP's
decode -> a bit-for-bit comparison with what was sent.

Nothing here changes VP's decoding maths. Two code moves are behaviour-preserving
extractions so the tests run VP's own rules: the tunnel gate
(`DolbyVisionTunnelGate.h`, from VideoProcessorDlg), the renderer's tunnel
presentation rule (`DolbyVisionTunnelPresentation.h`, from LibplaceboVideoRenderer),
the newlldv inference input (`HdrInferenceSignalBuilder.h`) and the LLDV static
metadata resolution (`LldvStaticMetadata.h`).

## 1. Architecture

```
 pattern + metadata sequence           VPV packer                 output
 (DvLoopbackPattern.h)        ->  (DvTunnelPacker.h)     ->  IDvLoopOutput
   stamped frame index,             ETSI GS CCM 001              |-- DvSimulatedLoop (in memory, now)
   scenes, advance DM, no_md,       packets, CRC, scrambling,    |     capture-card containers r210/R10b/R10l,
   EOS -> SDR -> DV                 HDMI 12-bit 4:2:2 in RGB8    |     high8/low8, row padding, latency,
                                                                 |     misses, repeats, wire bit errors
                                                                 '-- VPV hardware sender (VideoProcessorDvLoopback --send)
                                                                       NVIDIA NV_HDR_MODE_DOLBY_VISION -> HDMI -> capture card
 capture (VideoState + VideoFrame)
   -> DvLoopbackAnalyzer: VP's CR210DolbyVisionTunnelFormatter + the renderer's presentation rule
   -> identify each frame from its stamp in the raw wire bytes (independent of decoding)
   -> compare: wire bytes, decoded 12-bit planes, decoded metadata (DvMetadataModel.h),
      and the INDEPENDENT reference encoder (tests: DvTunnelReferenceEncoder.h)
   -> report: latency (frames, ms), dropped / repeated / out-of-order, black / wrong-kind
      frames, metadata mismatches, pixel errors with locations
```

Files (all new unless noted):

| File | Role |
|---|---|
| `src/VideoProcessor-Lib/dv_loopback/DvTunnelPacker.h` | VPV packer: IPT 12-bit 4:2:2 + DM -> RGB8 tunnel (header-only, no VP deps; the future VPV sender uses it) |
| `.../dv_loopback/DvCaptureContainer.h` | how a capture card stores RGB8 (r210/R10b/R10l, high8/low8, ARGB/BGRA) and back |
| `.../dv_loopback/DvLoopbackPattern.h` | deterministic pattern + metadata sequence, frame stamps |
| `.../dv_loopback/DvMetadataModel.h` | DM as named fields: what VP must decode from a sent DM; diff |
| `.../dv_loopback/DvLoopbackAnalyzer.{h,cpp}` | the receiving side and the report |
| `.../dv_loopback/DvSimulatedLoop.h`, `DvLoopbackHarness.h` | the in-memory loop and the harness |
| `src/VideoProcessor-Lib/RawCaptureRecorder.{h,cpp}` | raw capture recording (Tier 3, LLDV-vs-tunnel runs, hardware loop capture) |
| `src/VideoProcessor-Lib/RawCaptureCorpus.h` | reads VP recordings and StreamRecorder `--raw` folders |
| `src/VideoProcessor-DvLoopback/` | `VideoProcessorDvLoopback.exe`: `--probe` (read-only), `--send` (VPV, owner present), `--restore-sdr` |
| `src/VideoProcessor-Test/DvTunnelReferenceEncoder.h` | the independent reference encoder |
| `src/VideoProcessor-Test/DvTunnelLoopbackTests.cpp`, `DvIncomingHdrModeTests.cpp`, `DvRawCorpusTests.cpp`, `DvRpuAnchorTests.cpp` | CPU tests |
| `RendererCaptureHarnessTests.cpp` (3 tests added) | GPU tests (Claude runs them) |
| `tools/dv_compare/dv_lldv_vs_tunnel.py`, `tools/dv_lldv_vs_tunnel.ps1` | the LLDV-vs-tunnel comparison |
| `test-fixtures/dv-rpu/` | real RPU metadata (MIT, dovi_tool test assets) converted to DM fields |

## 2. The tiers

### Tier 1 - synthetic tunnel encoder and the simulated loop (built)

- **Two encoders, on purpose.** VP's VPV packer (Lib) and an independent reference
  encoder (tests) are both written from ETSI GS CCM 001 V1.1.1 (sections 6.2-6.4,
  Tables 3-14) and the HDMI 12-bit 4:2:2 channel mapping, without calling VP's
  decoder or the older test sender. The reference is built differently (field table,
  bit-string packets, bit-serial CRC LFSR, the 4:2:2 sample stream of ETSI Table 14,
  an explicit bit-position table). Both reproduce the standard CRC check value
  (`"123456789"` -> `0x0376E6E7`) and ETSI Table 3's default body.
- **The loop** (`RunSimulatedLoop`) runs VP's real formatter and the renderer's
  presentation rule on frames stored the way a capture card stores them, through
  every container/placement and row padding, at any raster and rate.
- **Tests**: bit-exact IPT round trip (every 12-bit code; 1080p and 2160p; scalar and
  AVX2); detection on/off (the gate); no false detection on noise, black, white,
  limited black, gradients, colour bars, graphics/text and mid grey (both
  placements, 120 frames each); DV -> SDR -> DV with and without EOS; scenes,
  advance metadata and no_md; corrupted / partial frames (missing continuation
  packet, malformed body, garbage metadata area, metadata loss limit, recovery,
  all logged, never packed pixels); 23.976 / 24 / 25 / 50 / 59.94 / 60 / 120 Hz
  (limits derived from the measured rate); latency, capture misses and output
  repeats measured from the stamps; wire bit errors located.
- **Perturbation (differential) tests**: every DM field is changed alone - matrices
  (signed extremes and +-1), offsets, range, source PQ, L1/L2/L5/L6/L8/L11 fields
  at extremes and just outside their limits - and the decoder must report exactly
  that change (by named field) and nothing else, or reject/drop exactly as VP
  documents. Pictures: single-sample and single-bit changes of I and of Ct/Cp at
  raster edges, row boundaries and inside/outside the metadata area must come
  back at the same sample with the same value; inside the metadata area the chroma
  LSB toggles exactly when ETSI's scrambling parity changes.

### Tier 1b - synthetic corpus through the whole renderer (built 2026-10-24, test/dv-synthetic)

A third, independent implementation of the tunnel in Python (`tools/dv_synth/`,
no shared code with VP's decoder, the VPV packer or the C++ reference encoder)
writes a deterministic on-disk corpus in VP's raw-capture format: known pictures
(wedge 0..1000 nits, colour patches, I and Ct ramps, BTB/WTW-like codes, 1-px
detail, a frame/scene stamp) with authored DM, packed as a DeckLink delivers the
RGB8 tunnel (r210 / R10b / R10l; low8, high8, high8 with replicated bits;
1080p and 2160p; 24/30/60 Hz), plus the exact expected planes, an independent
reference decode (IPT/YCbCr -> LMS -> BT.2020, HPE matrix derived, not copied)
and a documented NON-Dolby display-management model (`dm_model.py`: BT.2390 on
ICtCp intensity + the published L2 trim form) used only to compare policies.

Run: `powershell -File tools\dv_synth\run_dv_synth.ps1 -Corpus D:\codex\dv-synth-corpus -Out D:\codex\dv-synth-out`
(about 5.5 GB, 4 min to generate; CPU tests 1.5 min; GPU tests and jobs about
15 min on the laptop, mains power, `VP_TEST_ADAPTER=NVIDIA`).

| Test | What |
|---|---|
| `DvSyntheticCorpusTests` (CPU) | decode bit-exact (AVX2, scalar, the GPU shader's transcription); every authored DM field; robustness (missing / damaged / truncated / malformed / extreme DM, EOS -> SDR -> DV, id wrap, long loss); L1 alignment at cuts (same-frame and advance DM); 4K CPU cost; `FlatHeaderRowsDoNotLockTheWrongPlacement` (no corpus) |
| `GpuDolbyVisionTunnelUnpackMatchesTheSyntheticCorpus` | the GPU unpack on the GPU vs the generator's planes, every container, 1080p/2160p |
| `DvSynthGpuRendersJobs` | renders the jobs `run_dv_synth.py jobs` lists (accuracy, BTB/WTW, policy, LLDV-like, SDR passthrough); `run_dv_synth.py analyze` measures them |
| `DvSynthGpuAlignmentAtCuts` | streamed cut segments with captures; each capture's stamp names its frame; its brightness must be its own scene's |
| `DvSynthGpuRobustnessThroughTheRenderer` | the robustness segment streamed (CPU, GPU, GPU failing): no libplacebo error, black on DM loss, restored, listed under "Fallbacks engaged" |
| `DvSynthGpuUnpackPerformance4K` | 2160p60/24 streamed with each unpack route |

Fixed here (with tests): a low8 capture with flat header rows (black bars) locked
the high8 placement (the header CRC is valid under both placements: correct DM,
garbage picture); `DV trims.glsl` was missing from the build output and the
release manifest (dv_trims never acted outside a source checkout); the tunnel's
SDR passthrough (r210) failed every picture in libplacebo ("Failed swapping
endianness"), now repacked to BGRA8; the harness rendered the tunnel as SDR/
Rec.709 (now the app's PQ/BT.2020 state, `DolbyVisionTunnelSourceState.h`); DM
loss, an invalid source range, missing L1 and dv_trims unable to act are listed
under "Fallbacks engaged".

### Every incoming HDR mode (built)

`DvIncomingHdrModeTests` drives VP's own tunnel gate, newlldv inference input and
tracker, and LLDV static metadata resolution in VideoProcessorDlg's order:

| Mode (as a DeckLink reports it) | VP's reading | Checked |
|---|---|---|
| DV tunnel: RGB 8-bit in r210, "SDR / Rec.709" | tunnel formatter (switch on) | gate, decode, inference suppressed |
| LLDV: YCbCr 4:2:2 (V210), BT.2020, no HDR InfoFrame | newlldv: PQ after 1.5 s, `[lldv.*]` static metadata | stabilisation, profile > default, command line wins |
| HDR10 with InfoFrame | PQ from the InfoFrame | never inferred, tunnel never engaged |
| HDR10 without InfoFrame (Integral 2 HDR toggle off) | same as LLDV (indistinguishable to a capture card) | inferred like LLDV |
| SDR BT.709 | SDR | never inferred |
| LLDV / HDR10 as 10-bit RGB in r210, tunnel switch on | tunnel formatter for ~1 s (black), then handed off: normal path, newlldv PQ | `TenBitRgbLldvWithTheTunnelSwitchOnIsHandedOffAndInferredPq` |
| LLDV as 8-bit RGB, BT.2020, tunnel switch on | SDR passthrough for ~1 s, then handed off: normal path, PQ (8-bit decision below) | `EightBitRgbLldvWithTheTunnelSwitchOnIsHandedOffAsPq` |
| 8-bit RGB BT.709 (SDR menu), tunnel switch on | tunnel SDR passthrough (never handed off) | `OnlyNonTunnelSignalsAreHandedOff` |
| LLDV / HDR10 YCbCr 4:2:2 (10/12-bit) under a DV Tunneling profile | normal path: newlldv PQ (or HDR10 from the InfoFrame); YCbCr is never a tunnel | `DvTunnelingProfileDeclinesYCbCrLldvAndHdr10` |

Transitions LLDV -> tunnel -> HDR10 -> SDR -> LLDV -> HDR10 without InfoFrame -> tunnel
-> LLDV follow the tracker's epochs (the tunnel ends a PQ epoch; LLDV -> HDR10 without
InfoFrame at the same raster keeps it). The LLDV Dolby VSIF is not reported by capture
cards, so VP cannot tell LLDV from HDR10 without InfoFrame. The two RGB-LLDV cases (section 4,
fixes 1 and 2, 2026-10-18) are handed off by the tunnel to the normal path: How the owner selects LLDV or the tunnel (Integral 2 EDID:
low-latency-only DV block vs TV-led DV block) and which is better for the projector:
`docs/dv/LLDV-vs-tunnel-check.md`.

### Tier 2 - real RPU metadata (built, secondary)

- **Legal material**: dovi_tool's own test assets (MIT; quietvoid/dovi_tool at commit
  614c816b): 37 RPU files - profiles 4, 5, 7 (FEL/MEL), 8.1, 8.4 and an Apple
  profile-5 sample; 259-frame sequences; levels L1-L6, L8-L11, L254, L253. Installed
  under `D:\testclips\dv\` with a README (source, licence, SHA-256, commands);
  dovi_tool 2.3.4 under `D:\tools\dovi_tool`. No Dolby-published or Creative-Commons
  DV *stream* of real length was found under a licence that clearly permits test
  use (Dolby's developer content needs an account/click-through licence; community
  sample sets have unclear licences).
- **Use**: `tools/dv_rpu_to_dm.py` converts dovi_tool's JSON to per-frame DM fields;
  `test-fixtures/dv-rpu/` holds a small converted subset plus four RPU binaries
  (metadata only, MIT). `DvRpuAnchorTests` sends those real DMs through the loop
  (multi-packet, real trim sets, real L5/L6/L11) and checks VP's decode against
  dovi_tool's values directly (not through VP's own metadata model), and verifies each
  RPU's CRC-32 with the standard ISO/IEC 13818-1 algorithm over the payload after the
  0x19 prefix (emulation prevention removed): 1222 of 1225 RPUs in the corpus match;
  the 3 that do not are dovi_tool's deliberately malformed `p8_001_end_crc32` fixture.
- **Real metadata facts this confirmed**: real profile-5 RPUs carry exactly the IPTPQc2
  matrices the VPV packer uses, `signal_color_space` 2 (ETSI Table 6's ICtCp index) and
  offsets `[0, 2^27, 2^27]` (VP's 2^28 scale gives the expected 0.5 chroma offset);
  profile 8 carries YCbCr (space 0) with `[2^24, 2^27, 2^27]`; profile 4 is 14-bit, which
  VP rejects as documented (12-bit opponent colour only).
- **Direction "our serialise -> their parse"**: not applicable to the tunnel. dovi_tool
  parses RPUs; VP never parses or writes RPUs, and the HDMI tunnel carries ETSI's
  dm_metadata(), not an RPU. Anchored instead by the independent encoder (layout) and
  dovi_tool's reading (values).

### Tier 3 - real-capture corpus (built: recorder + replay)

- `VP_RAW_CAPTURE_PATH` (a folder on a large drive) arms `RawCaptureRecorder`;
  `[shortcuts] raw_capture` (default **Ctrl+Alt+W**, Config > Shortcuts; Ctrl+Alt+D is the
  report-a-problem shortcut) or a `RECORD`
  file in that folder starts recording, the key again / deleting `RECORD` / a `STOP`
  file stops it; start and stop are logged and shown on screen. `VP_RAW_CAPTURE_BURST=N`
  instead records N frames after each new capture state (the regression corpus).
  Each segment has `capture.txt` (card, card-reported input encoding and bit depth,
  raster, rate, encoding, row bytes, EOTF, colorimetry, HDR InfoFrame values, VP's
  LLDV/newlldv decision, the tunnel switch, a free note), `frames.csv` (counter,
  timestamps, arrival QPC) and `frame_NNNNNN.bin`. Frames are copied on the capture
  thread and written by a background thread; when the disk falls behind frames are
  skipped and counted, never stalling capture; recording stops below a free-space
  floor and after a time limit.
- `RecordedCorpusReplaysThroughTheTunnelPath` (`VP_TEST_RAW_CORPUS_DIR`) replays any
  recording through VP's tunnel path, writes `vp-decoded.csv` per segment, checks
  scalar vs AVX2, compares with `vp-expected.txt` (bless with
  `VP_TEST_RAW_CORPUS_BLESS=1`), and - when the owner drops dovi_tool's export of the
  same title as `rpu.json` in the segment - requires every decoded L1 tuple to occur
  in dovi_tool's reading (the conformance anchor). StreamRecorder `--raw` folders
  (8-bit ARGB) are read too and re-contained losslessly as r210.
- Owner steps: section 6.

### Tier 4 - the hardware loop (designed; sender built, untested on hardware)

See section 5. `VideoProcessorDvLoopback.exe --probe` is read-only and was run on the
laptop; `--send` changes the display's HDR mode and is for owner-present runs only.

## 3. What each check proves

| Bug class | Caught by the loop + perturbation tests | Needs an external anchor |
|---|---|---|
| Unpack bit errors, nibble/byte/channel order, placement, container endianness, row padding | yes (bit-exact vs the sent IPT, every code, every container) | - |
| Swapped DM fields, offset errors, bit order, truncation (VP's reading vs VP's writing) | yes (each field alone, named) | - |
| Packer and decoder sharing one wrong reading of the format | NO in the loop alone (demonstrated: `CommonModePackerAndDecoderFaultIsCaughtOnlyByTheReference`) | the independent reference encoder (same spec, independent code) |
| VP's and the reference's shared reading of the spec being wrong | no | real captures (Apple TV tunnel corpus), dovi_tool's reading of the same title |
| Tunnel containers for levels ETSI V1.1.1 does not define (L6, L8, L9, L10, L11, L254 layouts) | no (both encoders follow VP's assumption) | a real capture (the title log prints raw L2/L8/L11 bytes) |
| Real metadata ranges and combinations (many trims, long DMs, real L5/L6) | partly (synthetic) | Tier 2 real RPUs via dovi_tool |
| Mapping (tone/trim) vs Dolby's own display management | no | the Apple TV LLDV-vs-tunnel check (section 7) |
| Transport faults (HDFury/scaler/range conversion, link errors, drops) | modelled only | the hardware loop and real captures |

## 4. Decoder discrepancies and findings

- **No decoding bug was found.** VP's unpack, CRC, scrambling, packet syntax and the
  ETSI-defined DM fields agree byte for byte with the independent encoder; every
  perturbation comes back exactly.
- **Spec note (not changed): YCCtoRGB offset scale.** ETSI's default offsets
  `[67108864, 536870912, 536870912]` are 64/1024 and 512/1024 at a 2^30 scale; VP
  divides by 2^28 (libdovi/libplacebo convention for RPUs, where real profile-5/8
  RPUs carry `[0 or 2^24, 2^27, 2^27]`). For IPT tunnels VP's scale matches the live
  Apple TV picture; a YCbCr-signalled tunnel carrying ETSI's defaults would be
  mis-offset by 4x. Resolve with the first real capture's DM (Tier 3 logs it).
- **Precision note (not changed):** VP converts the 32-bit offset to float before
  scaling; changes below ~2^-24 relative (e.g. +1 at 2^27) are invisible. This is
  far below a 12-bit code step and libplacebo takes a float anyway.
- **Fix 1 (2026-10-18; was Finding A): 10-bit RGB LLDV/HDR10 with the tunnel switch on
  no longer stays black.** The gate still opens for any r210 capture reported as SDR (a
  bit-depth condition would break 8-bit tunnels padded to 10 bits, "high8"). Instead the
  GUI's hand-off probe (`DolbyVisionTunnelHandoff.h`, `[general] dv_tunnel_handoff`,
  default on, Config > DV > DV Tunneling) watches the captured pictures with the
  formatter's own rules: a CRC-valid header or tunnel-like content is tunnel evidence;
  with none for about one second (counted in source pictures, derived from the measured
  rate) and 10-bit content (both ends of the 10-bit fields in use - never RGB8 stored
  untouched), the signal is handed to the normal r210 path and the tunnel stops owning it,
  so newlldv infers PQ after its 1.5 s. A CRC-valid header later takes the signal back
  (that picture and the following ones are held back from the renderer until the tunnel
  state is in place, so no packed pixels are shown). Tests:
  `TenBitRgbLldvWithTheTunnelSwitchOnIsHandedOffAndInferredPq`, `DvTunnelHandoffTests`,
  `HandoffNeverTakesATunnel`, `TunnelTakesAHandedOffSignalBack`.
- **Fix 2 (2026-10-18; was Finding B): 8-bit RGB LLDV with the tunnel switch on is no
  longer shown as SDR.** Decision: capture cards report neither the Dolby VSIF (DeckLink)
  nor anything else that separates 8-bit LLDV from 8-bit SDR sent as BT.2020. What is
  known: RGB 8-bit, BT.2020 AVI colorimetry, no HDR InfoFrame, and - proven by the probe -
  no tunnel. A TV-led tunnel has CRC-protected metadata in every picture and RGB-default
  (not BT.2020) colorimetry, so on an input where the DV tunnel is enabled, BT.2020 RGB
  without a tunnel is player-led DV (LLDV) or HDR10 without its InfoFrame: PQ either way.
  It is handed off, and newlldv's 10-bit floor is waived for exactly this signal. The
  explicit indication wins: `VideoState::dolbyVisionVsif` (filled by a capture device that
  reports the VSIF; DeckLink cannot) set to LowLatency makes an 8-bit BT.2020 signal PQ
  with or without the tunnel switch and hands it off at the first picture; TvLed never
  hands off. Real 8-bit SDR sent as BT.2020 stays protected by newlldv's SDR veto; 8-bit
  BT.709 RGB (SDR menus) stays on the tunnel's SDR passthrough; with the tunnel switch off
  nothing changes. Tests: `EightBitRgbLldvWithTheTunnelSwitchOnIsHandedOffAsPq`,
  `DolbyVsifDecidesEightBitLldvExplicitly`.
- **Fix 3 (2026-10-18; was Finding C): a truncated frame with intact metadata is rejected,
  not decoded.** A frame whose tail was never written leaves the last row all zero, which
  valid tunnel content never is (an all-zero word is I = 0 with the most negative chroma;
  tunnel black is I near 0, chroma near 2048). A picture that would be decoded (CRC-valid
  header, valid DM) with an all-zero last row is rejected (the renderer keeps the previous
  picture), logged ("incomplete frame (last row blank, truncated transfer)"), and its
  CRC-verified metadata kept. Under `dv_tunnel_fixes` (default on). Not detectable: a
  truncation that leaves the previous frame's bytes in the tail. Tests:
  `TruncatedFrameWithIntactMetadataIsRejectedNotDecoded`, and the proof that no legitimate
  frame trips it, `TruncationCheckNeverRejectsALegitimateFrame` (a zero word decodes to
  negative light under all 1374 real RPU DMs; PQ black, letterboxed and fading frames packed
  by the reference encoder under every DM are all decoded).
- **Fix 5 (2026-10-18): a DV Tunneling profile no longer claims YCbCr input.** The tunnel is
  only ever 8-bit RGB 4:4:4; LLDV / HDR10 as YCbCr 4:2:2 goes to the normal LLDV / newlldv
  path even while such a profile matches (`dv_tunnel_handoff`).

## 5. Tier 4: the hardware loop

### 5.1 The loop

```
 VideoProcessorDvLoopback --send  (or, later, VP's own VPV output)
   NVIDIA GPU, NV_HDR_MODE_DOLBY_VISION, RGB 8 bpc full range, 8-bit swapchain
   -> HDMI (TV-led DV signalling; pixels = RGB8 tunnel bytes)
   -> HDFury (Integral 2 / Vertex2) presenting a TV-led DV EDID to the GPU,
      passing the pixels unchanged (no scaling, no colour/range/chroma conversion)
   -> capture input (UltraStudio 4K Mini on the laptop, or the DeckLink Quad HDMI
      Recorder in the cinema PC), RGB 4:4:4 8-bit, captured as r210 (VP) or ARGB
      (StreamRecorder)
   -> VP records (VP_RAW_CAPTURE_PATH + Ctrl+Alt+W) or StreamRecorder --raw N
   -> HardwareLoopbackRecordingAnalyses: regenerate the pattern from sent.csv,
      identify, compare bit for bit, latency from QPC (same PC) or frame offsets
```

The sender writes `sent.csv` (pattern parameters, every picture's index and present
QPC; DXGI sync QPC when available). The same harness then checks the four incoming
modes the loop can produce: `--mode tunnel` (VPV), `--mode hdr10` (UHDA passthrough:
PQ with the HDR InfoFrame), `--mode sdr`, and "HDR10 without InfoFrame / LLDV-like"
(PQ pixels in SDR mode with BT.2020 AVI colorimetry - needs the AVI override VP's
`NvidiaBt2020Reporter` already uses; not wired into the sender yet). A real LLDV VSIF
cannot be produced by the GPU.

### 5.2 This laptop (probe results, read-only, 2026-10-18)

- Machine: Dell XPS 15 9510. GPUs: Intel UHD Graphics (Tiger Lake-H, 0x9A60) and
  NVIDIA GeForce RTX 3050 Ti Laptop GPU (0x25A0); Thunderbolt 4 controller 9A21
  (CPU-integrated) and a UCSI USB-C connector manager.
- DXGI: the Intel adapter owns the only output (`\\.\DISPLAY1`, the internal panel,
  1920x1200, SDR); the NVIDIA adapter has **no outputs**.
- NVAPI: `NvAPI_GPU_GetAllDisplayIds` reports **0 display connectors** for the RTX
  3050 Ti - connected or not. The GPU has no display output of its own: every port,
  including the Thunderbolt/USB-C DisplayPort alt-mode ports (wired to the CPU's
  Thunderbolt controller), is scanned out by the Intel GPU (Optimus, no MUX).
- Consequence: NVAPI's DV mode applies only to NVIDIA-driven displays, so **VPV
  cannot run on this laptop**, with any adapter or dock. Intel has no Windows DV
  output API.
- The laptop is still the ideal capture side: the UltraStudio 4K Mini (Thunderbolt)
  records up to 2160p30 RGB 4:4:4.

### 5.3 Will NVIDIA's DV mode engage on a capture card?

The driver engages `NV_HDR_MODE_DOLBY_VISION` only when the sink's EDID carries a
Dolby VSVDB (`NV_HDR_CAPABILITIES_V3.isDolbyVisionSupported`). Capture cards'
EDIDs (UltraStudio 4K Mini, Quad HDMI Recorder) advertise HDR10 at most, not Dolby
Vision. So an HDFury must present a **TV-led DV EDID** to the GPU (custom EDID bank,
copied from the Vertex2's working DV EDID or a DV TV), while passing the pixels to
the capture card untouched. `--send` refuses to start when the sink has no VSVDB
(override `--force` for experiments). Known risk: a public report of a TV left
black in DV mode after the mode was turned off; the sender restores the previous
mode on every exit path, and `--restore-sdr --display \\.\DISPLAYn` is the recovery.

### 5.4 Chains

1. **Cinema PC alone (recommended):** RTX 3090 HDMI -> Integral 2 (input EDID: TV-led
   DV, 4K24 RGB 8-bit; scaling off; no HDR/colour/range conversion) -> DeckLink Quad
   HDMI Recorder input. Sender and VP on one PC: latency in ms from QPC. 2160p24/25/30
   RGB 4:4:4 8-bit fits the Quad's RGB limit (4K30).
2. **Cinema PC -> laptop:** RTX 3090 HDMI -> Integral 2 -> UltraStudio 4K Mini (laptop,
   Thunderbolt). Two machines: latency only as frame offset, drops/repeats and
   bit-exactness fully. UltraStudio input limit 2160p30.
3. **Laptop alone: not possible** for VPV (Intel scans out every port). A desktop
   NVIDIA GPU (or a laptop with an NVIDIA-wired HDMI/USB-C port or a MUX) is needed.

Cables: HDMI 2.0 18 Gbps is enough for 2160p24-30 RGB 8-bit. On a laptop, a USB-C
(DP alt mode) -> HDMI 2.0 adapter is an active DP-to-HDMI converter: it must pass
RGB 8-bit 4:4:4 unchanged (most do; avoid adapters that force YCbCr).

### 5.5 Owner steps (hardware loop, about 30 minutes, owner present)

1. Build x64 Release; copy `VideoProcessorDvLoopback.exe` next to VP or run from `x64\Release`.
2. Chain 1 above; Integral 2: custom EDID = the Vertex2's TV-led DV EDID; scaling off.
3. `VideoProcessorDvLoopback --probe` - the 3090 HDMI display must show "driven by
   NVIDIA" and `DolbyVision=1`.
4. Start VP with `VP_RAW_CAPTURE_PATH=D:\dvloop\run1` and the DV tunnel switch on;
   press Ctrl+Alt+W (recording started line on screen).
5. `VideoProcessorDvLoopback --send --display \\.\DISPLAYn --out D:\dvloop\run1 --seconds 20 --i-am-present`
   (the screen on that output shows the pattern; Ctrl+C stops and restores).
6. Ctrl+Alt+W in VP to stop. Repeat with `--mode hdr10` and `--mode sdr` into run2/run3.
7. Claude: `VP_TEST_DV_LOOPBACK_DIR=D:\dvloop\run1` and run `HardwareLoopbackRecordingAnalyses`.

## 6. Owner steps: the Tier-3 recording in the cinema session

1. Before starting VP (PowerShell): `$env:VP_RAW_CAPTURE_PATH = "E:\vp-capture"`
   (a drive with 100+ GB free; never a source checkout), optionally
   `$env:VP_RAW_CAPTURE_NOTE = "Vertex2 TV-led DV EDID"`, then start VP from that window.
2. Play a DV title; after the picture settles press **Ctrl+Alt+W**; let it run 10-20 s
   (stops by itself after 30 s); press Ctrl+Alt+W again to stop early. (The key is
   `[shortcuts] raw_capture`, Config > Shortcuts; Ctrl+Alt+D saves a problem report.)
3. Repeat for: an Apple TV DV title, a UHD profile 7 and 8.1 title if available, an
   HDR10 title, an SDR menu, and one "stop to the SDR menu" transition.
4. Send Claude the folder path and the vp.log. Nothing is committed; the folder becomes
   the permanent regression corpus (`VP_TEST_RAW_CORPUS_DIR`).
5. Optional anchor: extract the same title's RPU with dovi_tool and drop the export as
   `rpu.json` into the segment folder.

A 4K r210 frame is 33 MB: 20 s at 24p is about 16 GB. `VP_RAW_CAPTURE_EVERY=2` halves it.

## 7. The Apple TV reference (LLDV vs tunnel)

The Apple TV's certified Dolby display management (LLDV, mapped to the EDID's
target) is the external anchor for VP's **mapping**, not just its decode. The owner
records the same segment twice (LLDV, then tunnel); `tools/dv_lldv_vs_tunnel.ps1`
aligns them by content (different start points, pauses, seeks; only overlapping
frames are compared), renders the tunnel frames through VP at the LLDV target (GPU
test), and reports Delta E ITP per frame and per scene and whether the LLDV
brightness follows the DM L1 values. One page for the owner:
`docs/dv/LLDV-vs-tunnel-check.md`.

Expected accuracy: the decode is exact, so differences are mapping differences.
Inherent ones: Dolby's DM version (DM 2.9/4.x curves, its own trims interpolation,
chroma handling) vs libplacebo's tone mapping with VP's L1 and (optional,
non-certified) L2 trims; L8 trims are not applied by VP; the Apple TV may dither its
12-bit output to the link; the comparison converts VP's gamma output to absolute
luminance with a pure power law. Expect a few dE ITP in mid-tones and larger
differences in highlights near the target peak; the report says where VP departs.

## 8. Running the tests

CPU (single-threaded, TRX):

```
vstest.console.exe x64\Release\VideoProcessor-Test.dll /TestCaseFilter:"FullyQualifiedName~DvTunnelLoopbackTests|FullyQualifiedName~DvIncomingHdrModeTests|FullyQualifiedName~DvRawCorpusTests|FullyQualifiedName~DvRpuAnchorTests|FullyQualifiedName~DvTunnelHandoffTests|FullyQualifiedName~ShortcutDefaultsTests" /Logger:trx;LogFileName=dv.trx
```

GPU (Claude only; `VP_TEST_GPU=hardware`, optionally `VP_TEST_REQUIRE_GPU=1`):

```
/TestCaseFilter:"FullyQualifiedName~DvAutotestGpu|FullyQualifiedName~DolbyVisionGpuUnpackMatchesTheCpuUnpackThroughTheRenderer|FullyQualifiedName~GpuDolbyVisionTunnelUnpackMatchesTheCpuUnpack"
```
