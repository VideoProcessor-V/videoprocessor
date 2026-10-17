# Dolby Vision live verification - cinema session checklist (about 40 minutes)

Planner item (a), before the deploy. Goal: learn what really arrives and whether the tunnel survives
the chain, before building more on it. Nothing here changes settings permanently; back up
`C:\Videoprocessor\vp\VideoProcessor.cfg` first if anything is edited.

## Before you start (5 min)

- Build in place: the latest x64 Release pair (VideoProcessor.exe + vprenderer DLL; plugin API 33 -
  both from the same build).
- Chain: Apple TV / UHD player -> HDFury Vertex2 (DV EDID, No Scaling) -> DeckLink (r210 capture).
- Apple TV: Match Dynamic Range ON, Match Frame Rate ON, menus in SDR.
- VP: `[general] dolby_vision_tunnel: true`; DV page Input mode "DV Tunneling" on the active profile.
- Keep `dv_tunnel_gpu_unpack: off` and `dv_trims: off` for the first pass.
- Open the log: `C:\Videoprocessor\vp\logs\vp.log` (check its time matches the running VP).

## Titles (one each; 2-3 minutes of play per title)

1. Apple TV DV title (profile 5 / 8.4) - e.g. any Apple TV+ DV series.
2. UHD disc, profile 7 (FEL) - if a player is available.
3. UHD disc, profile 8.1 - if available.
4. An HDR10 title, an HDR10+ title (Apple TV recent tvOS), an SDR title with Match Dynamic Range off.
5. Where the player offers both: TV-led tunnel AND LLDV for the same title.

## For each title, note (from the log and the DV card / Ctrl+Alt+S)

- **Levels that arrive:** the line `title DM extension levels (first complete DM): L1=... L2=... L8=...`
  and, if present, the first L2 / L8 decoded + raw hex line (sends this to Claude - it verifies the
  trim layout).
- **CRC / lock:** DV locks (DV card shows DM, not Fallback); count of "black" / "lost" lines; any
  majority-vote recoveries or ID-wrap resets in the diagnostics counters.
- **SDR exit:** stop playback to the SDR menu without EOS - the picture goes black for about 2 s and
  then shows the menu (logged "SDR exit"); it must never show coloured garbage.
- **L1 minimum:** DV card "Scene avg / min" values change per scene.
- **Picture:** nothing visibly wrong vs your memory of the title (colour, black level, highlights).

## Second pass (10 min) - GPU unpack

- Set `dv_tunnel_gpu_unpack: on` (Config: Fixes under test), Apply.
- Replay 1-2 titles; the ingress status shows "(12-bit 4:2:2, GPU unpack)".
- Check: same levels line, no new black fallbacks, CPU usage lower, picture identical.
- (Optional) Turn on the stream recorder for 30 s of each title - recordings become test fixtures.

## Optional third pass - trims (only on a title whose levels line shows L2)

- `dv_trims: on`, side by side vs off on highlights, skin, shadows. Your eye decides; default stays off
  unless you prefer it on all three.

## Afterwards

- Set the switches back as you prefer; send the log (or the levels lines + any oddities) and the
  recordings location. Claude turns them into a chain-matrix "DV tunnel on" axis and the next steps
  (metadata layer, L1 into the LED look-ahead, a VP curve only if a shortfall shows).

## Optional - HDFury Integral 2 in place of the Vertex2

- Load a custom EDID with a full TV-led Dolby Vision block (copied from the Vertex2's working DV EDID, or from a DV TV) into one of the Integral 2's custom banks. Not an LLDV-only EDID (that makes the player send LLDV, not the tunnel). Claude can verify the EDID .bin first (Dolby VSVDB OUI 00-D0-46, TV-led; 4K60 RGB 8-bit; HDR10; checksums).
- Scaling off, no custom HDR injection, no colour/range/chroma conversion.
- Play a DV title: the levels line and DV card showing DM = tunnel intact; black/Fallback = bits altered.
- Also note the DeckLink pixel format at 4K24/30/60 (Quad HDMI Recorder: RGB 4:4:4 likely only to 4K30).
