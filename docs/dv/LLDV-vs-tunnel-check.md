# LLDV vs tunnel check - owner steps (one page)

Goal: play the same scene twice from the Apple TV - once as **LLDV** (the Apple TV's
own certified Dolby mapping to your EDID's target) and once as the **DV tunnel** (VP
gets the raw 12-bit picture and the per-scene metadata and maps it itself) - and let
VP compare its mapping with Dolby's. About 20 minutes plus copying time.

## Before you start

- A drive with **80 GB free** (one 20-second 4K run is about 16 GB). Not a source folder.
- In PowerShell, start VP from the same window:
  ```
  $env:VP_RAW_CAPTURE_PATH = "E:\vp-capture"
  cd C:\Videoprocessor\vp ; .\VideoProcessor.exe
  ```
- Apple TV: Match Dynamic Range **on**, Match Frame Rate **on**, menus in SDR.
- Integral 2: scaling off; no HDR injection; no colour, range or chroma conversion
  (keep YCbCr 4:2:2 as YCbCr 4:2:2: it keeps the Apple TV's 12 bits. An RGB conversion of
  LLDV no longer turns VP's picture black - VP hands it off after about a second - but it
  is not what this check compares).
- Pick a title and a scene (1-2 minutes in, ideally with bright and dark shots). Note
  the chapter and the time.

## Run 1 - LLDV

1. Integral 2: load the **LLDV EDID** - a Dolby Vision block with the *low-latency*
   interface only, and the target luminance set to your screen (write down the
   maximum and minimum nits it declares, e.g. 100 and 0.005).
2. Start the title; skip to about 5 seconds before your scene.
3. When the picture is on, press **Ctrl+Alt+W** in VP (the raw-capture key, Config >
   Shortcuts > raw_capture; Ctrl+Alt+D saves a problem report instead). The screen shows
   "Raw capture: recording started ...".
4. Let it play **20 seconds** (it stops by itself after 30 s), or press Ctrl+Alt+W to stop
   ("recording stopped ..." appears).

## Run 2 - tunnel

1. Integral 2: load the **TV-led DV EDID** (the Vertex2's working DV EDID).
2. In VP the DV tunnel switch must be on (DV page: Input mode "DV Tunneling").
3. Play the same title from the same point; the DV card should show **DM**.
4. Ctrl+Alt+W, 20 seconds, Ctrl+Alt+W - as in run 1. Small differences in where you
   start or a pause do not matter: VP lines the two runs up by picture content and
   compares only what both contain.

## Afterwards

- Each run is a folder under `E:\vp-capture` (newest last). Send Claude the two folder
  names, the two EDID luminance values, and the vp.log. Claude runs:
  ```
  powershell -File tools\dv_lldv_vs_tunnel.ps1 -Lldv E:\vp-capture\<run 1> -Tunnel E:\vp-capture\<run 2> -TargetNits 100 -BlackNits 0.005
  ```
  (the rendering step uses the GPU, so Claude runs it), and sends back `report.md`:
  Delta E ITP per scene (about 1 = just noticeable), where VP's mapping departs from
  Dolby's, and whether the Apple TV's brightness follows the per-scene metadata.
- Delete the recordings when done (or keep one pair as a regression set). Nothing is
  uploaded or committed.

## Which mode for the projector chain?

- **Tunnel (recommended):** VP receives 12-bit IPT and the per-scene DM (L1, trims) and
  does the mapping to the FL35 itself, feeding its sigma-delta at full precision.
- **LLDV:** the Apple TV maps to the EDID target with Dolby's certified engine, but VP
  receives an already-mapped picture at the capture card's 10-bit YCbCr depth and only
  static metadata; the result depends on how well the EDID target matches the screen.
- This check tells us how close VP's tunnel mapping is to Dolby's; differences in
  highlights near the target peak are expected (different curves by design).
