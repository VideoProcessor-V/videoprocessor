# TV-led Dolby Vision HDMI RGB tunnel

This experimental path accepts an Apple TV or other TV-led Dolby Vision
source through an HDFury Vertex2 and a DeckLink device that captures the
4K23.976 RGB 8-bit tunnel as r210. Enable it in the source configuration:

    [general]
    renderer: VP Renderer (Beta)
    dolby_vision_tunnel: true

Restart VP after changing this startup setting. Keep the Vertex2 on a
**real Dolby Vision** custom EDID and **No Scaling** for the DeckLink output.
Use the normal HDR10/LLDV configuration when the tunnel switch is off.

The formatter accepts either placement of RGB8 in DeckLink's 10-bit r210
channels, but recognizes Dolby Vision only after an embedded
display-management packet passes its MPEG-2 CRC-32. It reconstructs the
original 12-bit intensity and alternating chroma samples in two 16-bit
planes at full 4:2:2 resolution. It does not convert the tunnel to P010.
The packet's opponent-color and LMS matrices are passed to libplacebo's
Dolby Vision decoder; Level 1 scene peak/average PQ values are passed to
its tone mapper after complete metadata reassembly, including L1 blocks that
cross packet boundaries. Each 128-byte packet is read at its raster position,
with up to three CRC-checked copies. Declared lengths are bounded at 0x2F00
bytes and by the available picture pixels; packet types, IDs and header flags
must match. Incomplete structures never replace the active metadata.
Ordinary RGB8 with no Dolby packet uses SDR RGB passthrough before DV lock
once byte alignment is known, or after a CRC-validated EOS boundary.

The HDMI signal is already composed by the playback device. This path
does not decode or combine a Dolby Vision FEL itself. libplacebo does not
apply Dolby Level 2/8 display trims; it tone maps the decoded signal to
VP's configured output target. The formatter keeps active metadata when
a CRC-valid packet has no_md set. Advance metadata is associated with
the immediate next captured picture or with a later packet that names
the affected metadata ID; EOS separately ends the HDR stream. The
renderer bounds its metadata-matrix cache at 512 entries.

After DV lock, a picture without a CRC-valid supported header is presented as
black immediately, before upload, tone mapping or image analysis. Cached DM
is retained for brief loss, but it cannot authorize displaying untrusted pixels.
A valid `no_md` header with the current ID resumes normal rendering using that
cache. Temporal interpolation history is cleared while black is presented.

After 24 consecutive pictures without a complete usable metadata transaction,
cached active/pending metadata is discarded. Black continues until fresh complete
DM is available; a valid `no_md` header alone cannot rebuild discarded matrices.
CRC loss does not authorize SDR passthrough: a damaged/scaled tunnel is
indistinguishable from a same-mode SDR switch without EOS. An EOS boundary or
capture video-state reset permits SDR again. A same-mode SDR switch that omits
EOS may therefore remain black until Restart capture.

DeckLink reports the transport container as RGB 10-bit 4:4:4 (r210).
The DV tunnel inside that container is 12-bit 4:2:2, which the VP OSD
reports after unpacking. This does not add 4:4:4 movie chroma. The
previous measured LLDV input was also 4:2:2, at 10-bit capture depth,
so this path changes code precision and Dolby display-management
information, not the number of chroma samples. It cannot establish
whether a disc player used a Profile 7 FEL before HDMI output.

For Apple TV, choose 4K SDR as the normal menu format and enable Match
Dynamic Range and Match Frame Rate. This lets 23.976 Hz Dolby Vision
movies use the supported RGB tunnel while keeping the 59.94 Hz menu in
ordinary SDR. The current decoder only accepts the r210 RGB tunnel;
do not interpret a 59.94 Hz v210 menu as Dolby Vision.

The log reports complete DM length/packet count/IDs when the layout changes,
newly encountered extension levels (L1 is applied; other levels are logged
but not applied), loss/recovery events, and aggregate counters every 600
pictures. CRC-failed-copy counts include attempts at an alternate RGB8
alignment, so a nonzero count alone is not proof of HDMI corruption.
The log also reports "CRC-validated r210 RGB8 alignment" on first detection,
"new display-management matrix" when it changes, and "DV 12-bit 4:2:2"
for the formatter contract. If the CRC never validates, the path cannot
interpret that signal as Dolby Vision; check Vertex2 EDID/scaling, DeckLink
packing, and whether the player actually sends TV-led DV. The previous single-packet decoder has produced a correct image on the
user's DeckLink/Vertex2/Apple TV and Sony setup. Multi-packet reception and
loss/recovery changes still need live source confirmation.

References: [HDMI 1.4a RGB tunnel packing](https://felloff.net/doc/comp/spec/HDMISpecification14a.pdf),
[ETSI GS CCM 001 display-management packet format](https://www.etsi.org/deliver/etsi_gs/CCM/001_099/001/01.01.01_60/gs_ccm001v010101p.pdf),
[libplacebo Dolby color representation](https://github.com/haasn/libplacebo/blob/master/src/include/libplacebo/colorspace.h).

The independent sender reference is
[CroqueMr/libreelec-x86-DV transport contract](https://github.com/CroqueMr/libreelec-x86-DV/blob/d9ad989ee92846afc32957cbeab6d5e2a5cb49b4/CB1/docs/SOURCE-TRANSPORT.md)
and its `CB1/src/dvbridge_render.c` RGB8 packer at the same pinned revision.
Its 12-bit intensity/chroma packing is the inverse of this unpacker. The
reference's FEL composer operates upstream of that HDMI packing step.

## GPU tunnel unpack (under test)

`dv_tunnel_gpu_unpack: on` (renderer profile key, Config: Fixes under test;
default `off` until validated live) moves the picture unpack to the GPU. The
formatter still finds, CRC-checks and parses the DM packet on the CPU, reading
only the packet's pixels (and, before lock, the 4096-pixel alignment probe);
every lock, black, loss and EOS decision is unchanged. A picture that would be
shown goes on as the raw frame (r210, or R10b/R10l: the r210 word << 2,
R10l little endian; early upload when active) and
`GpuDolbyVisionTunnelUnpack` writes the formatter's two planes exactly (r16
intensity, rg16 alternating chroma, codes << 4). The shader arithmetic is
transcribed in `vprenderer/DolbyVisionTunnelUnpackReference.h` and tested
bit-exact against the scalar and AVX2 CPU unpack; the GPU test
(`GpuDolbyVisionTunnelUnpackMatchesTheCpuUnpack`, `VP_TEST_GPU=hardware`, or
WARP with `VP_TEST_DV_WARP=1`) compares the GPU planes to the CPU's. Scene,
subtitle and active-picture analysis sample the same codes from the raw words
(`PackedDolbyVision422`). If the GPU unpack fails, that picture is unpacked on
the CPU at the render and the session returns to the CPU unpack (logged once);
`VP_TEST_DV_UNPACK_FAIL=1` forces this for tests. After a failure the payloads
already queued go straight to the CPU fallback (no GPU retry); a new render
device may try the GPU again. A picture neither can unpack (e.g. the mode
changed under queued payloads) is presented black. Decoded DV has one analysis
domain key on both routes (placement and strides are left out of it).

## L2 trims (under test, non-certified)

`dv_trims: on` (renderer profile key, Config: Fixes under test; default `off`)
applies the Dolby Vision Level 2 trim for the display's peak after
libplacebo's tone mapping (`shaders/DV trims.glsl`, PRE_OUTPUT, before the LED
stages). This is NOT Dolby display management and is not certified: it is an
independent reimplementation from the published L2 fields (12-bit codes,
2048 = neutral) applied to VP's own tone-mapped output.

- Selection: the L2 targets (PQ codes) against the display peak in PQ
  (libplacebo's inferred target peak). A trim at the peak is used as sent, two
  bracketing trims are interpolated linearly in PQ, below every target the
  lowest-target trim is used, above every target the highest-target trim fades
  to neutral at the mastering peak, and at or above the mastering peak nothing
  is applied.
- Per pixel: luminance Y in nits from the output signal (PQ, or gamma x the
  display peak), with the output primaries' own luminance coefficients. A CDL
  `x = t * slope + offset` on `t = PQ(Y) / PQ(peak)` follows, then
  `shoulder(x) ^ power`:
  - the offset fades in over the first 16 PQ codes, so the curve is
    continuous through black and below-black stays below;
  - when slope + offset exceeds 1, a C1 shoulder reaches 1 exactly at the
    peak instead of hard-clipping highlights below it;
  - above the peak the curve continues with slope 1, so the kept above-white
    is not clipped; Y <= 0 is untouched.
- The pixel's luminance becomes Y'. Its chroma (L - Y) is scaled by
  `saturation_gain x (Y'/Y) ^ chroma_weight` (0..2) times Y'/Y, with Y'/Y
  capped at 4:
  - near black (below the luminance of 64 PQ codes) the change is additive,
    with the gain fading to 1 at Y = 0, so tiny or mixed-sign pixels cannot
    become bright specks;
  - a gain above 1 never pushes a channel that was at or above black below it
    (gamut).
- L2 blocks with any field above 12 bits are skipped; decoded values are
  clamped to the spec ranges.
- Off with a calibration 3D LUT, for outputs other than PQ, a power-law gamma,
  BT.1886 or sRGB (e.g. linear, HLG), and for pictures without DM. The log
  reports the reason on change and the parameters at most every 10 s
  ("Dolby Vision trims").
  The reference maths is `vprenderer/DolbyVisionTrims.h`. Tests:
  `DolbyVisionTrimsTests` (CPU) and `DolbyVisionTrimShaderMatchesItsReference`
  (GPU).

## Source resolution on DV

Source-resolution detection no longer skips the tunnel as a "packed
signal". It measures the decoded intensity (I) plane like YCbCr luma. A
re-base, manual or auto, resamples the IPT planes before libplacebo's Dolby
Vision decode, just as YCbCr planes are resampled before their matrix. The
DV representation (DM matrices) is kept.

The L1 scene minimum is shown on the DV luminance card ("Scene avg / min")
and in the capture JSON (`scene_min_present`, `scene_min_nits`). It is not
passed to the tone mapper, because libplacebo has no input for it. The
renderer API is 32.

## Luminance status

The Modern view shows a **DV luminance (nits)** card while the tunnel is active:
source minimum/maximum from recovered DM, and scene peak/average from L1.
The card marks a recovered source range as **DM**, or **Fallback** when a
valid source range is absent and VP's effective static range is used. Missing
scene metadata appears as `---`; a present black scene appears as zero.
Before metadata is available or after metadata loss the values are hidden.
SDR RGB passthrough is labelled **SDR**. HDR10 and LLDV retain their static
HDR luminance card. The legacy HDR controls still represent effective static
metadata, rather than this per-frame DV snapshot.

For DV, libplacebo receives zero (unknown) MaxCLL/MaxFALL instead of the
synthetic static values; recovered DM source range and L1 PQ fields are
supplied separately. L1 peak/average are not relabelled as HDR10 MaxCLL/FALL.
The output target brightness remains the configured projector/display target.
The shared renderer API is bumped to 26 so older host/plugin pairs are rejected.
## UHD disc player qualification

A UHD player can use this same tunnel path when it sends compatible TV-led
Dolby Vision. The player must reconstruct any Profile 7 enhancement layer
before HDMI output. VP preserves and decodes the composed tunnel; it cannot
recover an EL discarded by the source, or determine FEL/MEL from this signal.
Use the [staged UHD test protocol](VP_DV_UHD_QUALIFICATION.md) before claiming
hardware qualification. Dune is a transport test until its exact edition's
FEL/MEL type and the player's behavior have been independently established.

Rendered-output captures (`Ctrl+Alt+S`) now record the source representation,
sample depth, color depth and bit shift, plus a `dolby_vision` object identifying
active DV decoding, DM versus fallback source range, L1 presence and the PQ
scene values supplied for that frame. `fel_status` remains `unknown` on DV HDMI
input. These are additive fields in the existing capture JSON schema; the PNG
still contains the final 10-bit output expanded losslessly to 16 bits, not the
original 12-bit tunnel samples. See [capture interpretation](VP-0133_RENDERED_OUTPUT_CAPTURE.md).

The reviewed Beta library is the bundled API 377 source at
`billslack2/libplacebo` commit `8bccd6578c9e845f77205cec9e85fdb97bf36ed7`.
Its Dolby decoder already normalizes the recovered codes and uses the supplied
matrices. No second reshaping/FEL pass is appropriate for this HDMI route.
With peak detection enabled, L1 PQ fields remain supplied, while measured
maxRGB/average fields can also influence libplacebo's adaptive target. L1 is
therefore an input to VP's selected tone mapping, not a complete Dolby display
mapping implementation.

## VP fork additions (2026-10-16)

These extend the behaviour described above; tests in `DolbyVisionTunnelFormatterTests.cpp`.

- **Limits by time, not frames:** the metadata-loss limit is about 1 s converted at the signal's rate
  (24 pictures at 23.976, 60 at 59.94); no fixed timing remains (raster and rate from the display mode).
- **SDR switch without EOS:** while locked, if no CRC-valid header arrives and the pictures stop looking
  like a tunnel for about 2 s (counted in source pictures), the formatter unlocks and allows SDR (logged).
  Black is shown during that window; any valid header re-locks. This replaces "remain black until
  Restart capture" for that case.
- **Metadata ID wrap:** after 16 or more header-less pictures an ID match is ambiguous; cached DM is
  dropped (black until fresh DM) instead of resuming stale matrices.
- **Containers:** R10b and R10l DeckLink packings are accepted besides r210 (R10 word = r210 word << 2;
  R10l little-endian); only a CRC-valid packet proves the bytes are exact. The GUI gate still enables the
  tunnel for r210 only until widened.
- **Packets:** a bitwise 2-of-3 majority vote recovers a packet when all three copies fail CRC (the CRC
  stays the authority); multi-line DM up to 100 packets is tested.
- **Interlaced modes** never lock DV (SDR passthrough only).
- **Metadata:** the L1 scene minimum is carried (libplacebo has no field for it; for a future VP curve);
  L2 and L8 trims are parsed but not applied; one log line per title lists which extension levels arrive
  (and the first L2/L8 block decoded plus raw hex, to verify the layout on the first live run).
