# Automatic display calibration

This is the user guide for VideoProcessor's (VP) automatic display
calibration. It is for home cinema owners with a projector and for people
with a TV or monitor.

## 1. What it does

You put a colour meter in front of your screen and press **Start** on the
Config > Calibration page. VP shows its own test patches on the screen,
the meter measures each one, and VP builds three 3D colour tables (LUTs)
that correct your display's white point, greyscale and colours. At the end
you see a review. Only when you press **Apply** does VP attach the new
LUTs to your live Color / Output profile.

Until you press Apply, nothing in your configuration changes. Discard and
Cancel leave `VideoProcessor.cfg` exactly as it was.

The wizard works with any ArgyllCMS-supported meter. The result is a set
of `.cube` files, a display profile, a verification report and a backup
of your configuration.

## 2. Before you start

### 2.1 Install ArgyllCMS yourself

VP measures through ArgyllCMS's `spotread` program. VP never bundles it
and never downloads it. You install it once.

1. Download the Windows 64-bit ArgyllCMS executables from the official
   site, argyllcms.com. Use the current 3.x release.
2. Unzip it to a folder of your choice, for example
   `C:\Program Files\ArgyllCMS`.
3. Check your meter (see 2.2). Most colorimeters need no driver. Some
   meters need Argyll's USB driver. Follow Argyll's "Installing the
   software on Microsoft Windows" page.
4. In Config > Calibration, set **ArgyllCMS folder** to the folder that
   holds `bin\spotread.exe`. You can leave it empty. VP then searches for
   it (see 2.3).
5. Press **Apply** in the Config app to save the setting, then press
   **Detect ArgyllCMS and meters**.

### 2.2 Which meters need a driver

| Meter | Argyll USB driver needed? |
|---|---|
| i1Display Pro / Plus, Calibrite Display, ColorChecker Display | No |
| ColorMunki Display, i1Display Studio | No |
| Spyder family | No |
| JETI | No |
| i1Pro, ColorMunki Photo / Design | Yes |
| Some older meters | Yes |

This follows the guidance text VP shows. If in doubt, read Argyll's own
Windows page for your exact meter.

### 2.3 Where VP looks for spotread

In this order:

1. The **ArgyllCMS folder** setting (`argyll_path`). It can be
   `spotread.exe` itself, its `bin` folder, or the install folder.
2. The `ARGYLL_PATH` environment variable.
3. Every folder on `PATH`.
4. The usual unzip places: Program Files, Program Files (x86),
   `%LOCALAPPDATA%\Programs`, your user profile and the drive roots. VP
   looks for a folder named `Argyll*` and takes the highest version.

### 2.4 Windows and VP

- You need Windows 10 version 1809 or newer. VP drives spotread through a
  pseudo-console, which older Windows does not have.
- VP must be running, and **any source must be playing**. The VP Renderer
  has to be running, because the patches replace the video while VP
  measures. The Config app must be connected to the running VP.
- Use an 8-bit or 10-bit output. See the troubleshooting table for FP16.

## 3. Supported meters

Choose the meter in **Meter**, or leave it on **Automatic** and VP picks
the model from spotread's instrument list.

| Family | Type | Sensor calibration asked? | Correction type | Notes |
|---|---|---|---|---|
| Calibrite Display Pro / Plus (HL), i1Display Pro / Plus, ColorChecker Display | Colorimeter | No | CCSS or CCMX | The best-supported choice. Low-light floor about 0.002 cd/m2. |
| ColorMunki Display, i1Display Studio | Colorimeter | No | CCSS or CCMX | Floor about 0.005 cd/m2. |
| i1Display 2 / LT, Huey Pro | Colorimeter | No | CCMX only | Old. Floor about 0.05 cd/m2. |
| Datacolor Spyder X2 | Colorimeter | No | CCSS or CCMX | Floor about 0.01 cd/m2. |
| Datacolor Spyder X, 5, 4 | Colorimeter | No | CCSS or CCMX | Floor about 0.02 cd/m2. |
| X-Rite DTP94 | Colorimeter | No | CCMX only | Floor about 0.02 cd/m2. |
| X-Rite i1Pro 3 | Spectrophotometer | Yes | None needed (measures the spectrum) | Floor about 0.2 cd/m2. |
| X-Rite i1Pro 2 | Spectrophotometer | Yes | None needed (measures the spectrum) | Floor about 0.2 cd/m2. |
| X-Rite i1Pro | Spectrophotometer | Yes | None needed (measures the spectrum) | Floor about 0.3 cd/m2. |
| X-Rite ColorMunki Photo / Design | Spectrophotometer | Yes | None needed (measures the spectrum) | Floor about 0.5 cd/m2. |
| JETI specbos / spectraval | Spectroradiometer | No | None needed (measures the spectrum) | Floor about 0.05 cd/m2. |
| Klein K10-A | Colorimeter | No | CCMX only | Floor about 0.0005 cd/m2. |

Notes on the table:

- "Floor" is the typical low-light limit VP assumes for the meter. Below
  it, near-black readings are noisy, and VP marks those patches (see
  6.5). The values are approximate, not a guarantee.
- CCSS is a spectral sample set for a display's light source. CCMX is a
  correction matrix. A meter that takes CCSS can use either. One that
  takes only CCMX needs a matrix made for your display and meter.
  Spectrophotometers and spectroradiometers measure the spectrum
  directly, so most owners will not need a correction with them.
- A meter that is not in the list still works if ArgyllCMS supports it.
  Choose **Another ArgyllCMS instrument**. VP then drives it without any
  model-specific tuning and uses a generic low-light floor of 0.05 cd/m2.
- **Dry run (simulated meter, real patches)** lets you try the whole
  wizard without a meter. VP shows real patches, and a simulated meter
  answers. The result describes the simulation, not your display. Do not
  Apply a dry run to a display you care about.

## 4. Which meter to buy

This section is factual. It is not an endorsement.

### 4.1 Projector or TV

A **colorimeter of the i1Display Pro / Calibrite Display Pro or Plus HL
class**, used with a **spectral correction (CCSS) for your display's
light source**, is the usual choice. It reads dark patches better than a
spectrophotometer of similar price. A CCSS lets it read different light
sources accurately.

| Display | What matters |
|---|---|
| Lamp projector | A correction for the lamp, or a generic one, helps. |
| RGB laser projector | Needs a correction for an accurate white point. The narrow laser spectra fool a plain colorimeter. |
| LED / laser-phosphor projector | A correction for that light source is advised. |
| Wide-gamut LCD or QD-OLED TV | A correction for that panel type is advised. |
| White OLED TV | A correction for OLED is a good start. |

Corrections: ArgyllCMS can create a CCSS from a spectrometer reading.
Some corrections are shared by the community or come with meter
software. Check that a correction matches your display type and your
meter before you use it.

### 4.2 Spectrophotometers (i1Pro and similar)

Spectros are accurate for colour. They are poor in the dark. Their floor
is 0.2 to 0.5 cd/m2 in the table above. On a good projector or an OLED
that is too high for the lowest greyscale steps. They also ask for a
sensor calibration. You can use one to measure your display's spectrum
and create a CCSS for a colorimeter.

### 4.3 Buy returnable

One report in VP's design notes has a Calibrite Display Plus HL hanging
in ArgyllCMS 3.1.0. VP's real-meter path has also not yet been run on
real hardware (see section 12). So:

- buy from a seller that accepts returns;
- install the current ArgyllCMS release and test the meter with it;
- press **Detect ArgyllCMS and meters** and watch the live aim reading
  before you commit to a long run;
- if the meter hangs, try another Argyll release, and return the meter if
  it still hangs.

## 5. Setting up

Open the Config app, connect it to the running VP and go to the
**Calibration** page. It has four cards: Meter, Display and patches,
Target and LUT, and Calibrate. Changes need **Apply** in the Config app
before **Start** uses them, because the wizard uses the saved settings.

All values are stored in the `[calibration]` section of
`VideoProcessor.cfg`.

### 5.1 Meter card

| Key | Control label | Default | Meaning |
|---|---|---|---|
| `argyll_path` | ArgyllCMS folder | empty (search) | Folder with `bin\spotread.exe`, `bin`, or `spotread.exe`. |
| `meter_model` | Meter | Automatic | The meter model (section 3). `simulated` is the dry run. |
| `meter_port` | Meter number | 0 (first found) | spotread's `-c` number, from the Detect list. Range 0 to 64. |
| `display_type` | Display type | empty (the meter's default) | spotread's `-y` selector. Up to 8 letters or digits. Pick the one nearest your light source. |
| `spectral_correction` | Spectral correction | none | Path to a `.ccss` or `.ccmx` file. Passed to spotread with `-X`. |
| `refresh_mode` | Refresh mode | Meter decides | `auto`, `refresh` (`-Y r`) or `non_refresh` (`-Y n`). |
| `read_timeout_s` | Reading time limit | 60 s | A meter that does not answer in this time is given up on. Range 5 to 600. |

VP always passes `-e` and `-x` to spotread. It adds `-c`, `-y`, `-Y` and
`-X` only when you set them.

### 5.2 Display and patches card

| Key | Control label | Default | Meaning |
|---|---|---|---|
| `display_kind` | Display | Projector | Projector, LCD TV / monitor, OLED TV / monitor, Other. Sets the automatic window and settle time. |
| `display_name` | Display name | the output monitor's name | Names the run folder. |
| `patch_window_percent` | Patch window | Automatic (0) | Share of the screen area the patch covers. 0 to 100. |
| `background_percent` | Window surround | 0 | Grey level around a windowed patch, as a percentage of signal. 0 to 100. |
| `settle_ms` | Settle time | Automatic (0) | Wait after each patch change before reading. 0 to 30000 ms. |
| `near_black_max_reads` | Near-black readings | 4 | Up to this many readings are averaged near black. 1 to 16. |
| `repeat_tolerance_percent` | Agreement | 1.0 % | Two readings that agree within this are accepted. 0.1 to 20. |
| `warmup_minutes` | Warm-up | 30 min | Warm-up before measuring. 0 to 180. |
| `drift_interval` | Drift check every | 50 patches | White is measured again every this many patches. 0 to 1000. |

### 5.3 Projector, LCD and OLED defaults

When the window and settle are left on Automatic, VP picks them from the
display kind.

| Display kind | Patch window | Base settle | Extra settle after a big drop to dark |
|---|---|---|---|
| Projector | 100 % (full field) | 0.5 s | 0.5 s |
| LCD | 18 % | 1.0 s | 1.0 s |
| OLED | 10 % | 0.8 s | 0.5 s |
| Other | 18 % | 1.0 s | 1.0 s |

Why a window? An OLED limits its light when a lot of the screen is
bright (ABL). An LCD with local dimming changes its backlight with the
picture. A small window keeps the patch light the same whatever the
patch size. A projector has neither problem, so it shows full field.

If you set `settle_ms` yourself, VP adds half of it again after a large
drop in light. VP also adds the display's measured latency when it is
known.

### 5.4 Target and LUT card

| Key | Control label | Default | Meaning |
|---|---|---|---|
| `plan` | Measurements | Standard (9^3 cube) | How many patches. See section 6.2. |
| `target_transfer` | Target response | BT.1886 | `bt1886` (with the measured black) or `gamma` (pure gamma). |
| `target_gamma` | Gamma | 2.4 | Used with pure gamma. 1.6 to 3.0. |
| `target_white_percent` | White level | 100 % | Share of the display's brightest D65 white to aim for. 25 to 100. |
| `lut_size` | LUT size | 33 | 17, 33 or 65. 33 is recommended. |
| `verify_after` | Verify through the new LUT | on | Measures about 60 more patches through the new LUT. |

### 5.5 Meter placement

**Projector.** Put the meter on a tripod 30 to 50 cm in front of the
screen centre. Face it at the screen. Keep it out of the projector's
beam. The white patch is in the centre of the screen.

**TV or monitor.** Lay the meter flat against the centre of the screen,
on the patch. Weight the cable so the meter does not tilt.

### 5.6 Display and room

- Turn off dynamic contrast, eco or energy saving, auto brightness, ABL
  style limiters where the display allows it, and similar modes. They
  change the light while the meter reads.
- Darken the room. Stray light spoils near-black readings.
- Let the display warm up. The wizard has a warm-up step for this.
- Use the picture mode and the lamp, laser or backlight setting you watch
  in.

## 6. Running the wizard

Press **Start** in the Calibrate card. The wizard shows the step, an
instruction, a progress bar and the last reading. The buttons are Start,
Continue, Skip warm-up, Apply, Discard and Cancel. Only the ones that make
sense in the current step are enabled.

### 6.1 The steps

| Step | What happens | What you do |
|---|---|---|
| Connecting | VP starts spotread and opens the meter. | Wait. |
| Meter calibration (only if asked) | Some meters ask for a dark or white-tile calibration. | Do what the text says, then press Continue. |
| Place the meter | A white patch is shown. The meter reads it live, and the page shows Y in cd/m2 and x, y. | Place and aim the meter for the highest steady reading, then press Continue. |
| Warm-up | A mid-grey patch is shown for the warm-up time. | Wait, or press **Skip warm-up** if the display has been on long enough. |
| Measuring | Patches are shown and read one by one. A progress bar and the time left are shown. | Keep the room dark and the meter still. |
| Building the LUTs | VP fits a display model and builds three LUTs. | Wait. |
| Verifying | VP measures colours through the new LUT. These are not used for the fit. | Wait. |
| Review | The result is shown (section 7). | Apply or Discard. |

If the meter stops answering during the run, VP tries to reconnect. If it
needs you (for example to put the meter back on its tile), it tells you
and waits for Continue.

### 6.2 How long it takes

These are the estimates on the Config page. They are for a projector. A
dim display takes longer, because dark patches need more settle time and
more readings.

| Plan | Estimate |
|---|---|
| Quick (greyscale + colours) | About 10 minutes |
| 5^3 cube | About 15 minutes |
| Standard (9^3 cube) | About 35 to 45 minutes |
| 13^3 cube | About 1.5 hours |
| Thorough (17^3 cube) | About 2.5 hours |

Add the warm-up time (30 minutes by default) and the verification pass.

Patch order: a white reference first, then the greyscale from white down
with the near-black codes last, then primaries and secondaries, then the
cube. White is measured again at the drift interval and at the end.

### 6.3 Near-black strategy

A colorimeter's count is noisy near black. Near black, VP averages up to
**Near-black readings** readings. It steps down from brighter patches, so
the meter's dark integration adapts. The black patch is measured last of
the greyscale ramp, and again at the end to check drift. Elsewhere, VP
reads a patch up to three times and stops when two readings agree within
**Agreement**.

### 6.4 Drift

Displays drift as they warm. White is measured at the start, every
**Drift check every** patches and at the end. VP corrects the
measurements for slow drift. The Review shows the drift it saw.

### 6.5 Patches below the meter's floor

A patch whose reading is below the meter's low-light floor is marked
floor-limited. The Review reports how many there were. Many
floor-limited patches mean the meter is too weak for your display's
blacks. A better meter or a darker room helps.

## 7. Review, Apply and Discard

### 7.1 What Review shows

- The display's white level (cd/m2) with its x, y, the black level and the
  contrast ratio.
- The effective greyscale gamma and the additivity error.
- The LUTs written, with the number of gamut-clipped nodes in each.
- The verification result: dE2000 mean and max, with the worst patch. VP
  calls it a pass when the mean is under 1.5 and the max is under 3.
- How many patches were measured, skipped and floor-limited, and any
  display drift (corrected).
- The exact configuration edits Apply would make. If Apply is not
  possible, the Review says why.

### 7.2 What Apply does

1. Copies `VideoProcessor.cfg` byte for byte to
   `VideoProcessor.cfg.before-calibration.<yyyyMMdd-HHmmss>.bak` beside
   it.
2. Copies it again into the run's `before\` folder.
3. Sets only these keys, in the live **Color / Output** profile:
   - `calibration_lut_enabled`
   - `calibration_lut_bt709`
   - `calibration_lut_p3_d65`
   - `calibration_lut_bt2020`
4. Checks the new file and saves it in one step. Comments, other keys and
   their order are kept.
5. VP applies the change at once. No restart is needed.

VP uses the active Color / Output profile. If the file does not have it,
VP uses the first `[vprenderer.color.*]` section. If there is no Color /
Output profile at all, Apply is not possible, and the Review says so.

One run writes all three LUTs (BT.709, P3-D65 and BT.2020), so the right
one is used for each source gamut.

### 7.3 Discard

Discard changes nothing in your configuration. The measurements stay in
the run folder, so you can still read the report.

### 7.4 Undo

Either:

- copy the `.bak` file back over `VideoProcessor.cfg`; or
- in the Config app, turn `calibration_lut_enabled` off or set the
  `calibration_lut_*` keys back to what they were.

## 8. The run folder

Each run is stored in the VP folder (the one with `VideoProcessor.cfg`):

```
luts\calibration\index.json
luts\calibration\<display>\<timestamp>\
```

`<display>` is the display name with unsafe characters changed to `_`.
`<timestamp>` is `yyyyMMdd-HHmmss`.

| File | What it is |
|---|---|
| `raw.jsonl` | Every reading, one line each: patch, codes, XYZ, number of reads, spread, status, notes. |
| `calibration_bt709.cube` | The LUT for BT.709 sources. |
| `calibration_p3_d65.cube` | The LUT for P3-D65 sources. |
| `calibration_bt2020.cube` | The LUT for BT.2020 sources. |
| `display_profile.cfg` | What VP learned about the display: peak, black, response and gamut. |
| `verify.json` | The verification result (when verification ran). |
| `report.html` | A readable report. Open it in a browser. |
| `before\` | A byte copy of the configuration before Apply. |

`luts\calibration\index.json` lists every run: display, timestamp, run
folder, status (measured, applied, discarded, cancelled or failed) and
the verification mean and max. A run that is cancelled or fails still
gets an entry.

## 9. Faithful image notes

- **BTB and WTW are measured and kept.** Below-black and above-white
  codes in the plan are shown as they are. On a limited-range output the
  legal codes are 1 to 254 (8-bit) and 4 to 1019 (10-bit), so these
  codes are reachable and the LUTs keep them.
- **Exact codes in the active output.** Patches are written straight to
  VP's own swapchain as raw integers in its format: 8-bit, 8-in-10 (an
  8-bit code in a 10-bit container) or 10-bit, limited or full range.
  The output pipeline stays as VP configured it for video.
- **Sigma-delta and dither are bypassed for the patch frames only.** Tone
  mapping, gamut mapping, the calibration LUT, deband, the sigma-delta
  and dither are not applied to a patch. The sigma-delta itself is not
  changed. It does not run for the patch frames and runs again from the
  first video frame afterwards.
- **The recorded signal is the exact one.** A patch lands on a whole code.
  VP records the exact signal of that code, not the one it asked for.
- **Nothing assumes a refresh rate or a resolution.** The window is a
  share of the screen area. Settle times are display physics, in
  seconds. The meter measures the display's refresh unless you set it.

## 10. Safety

- **Cancel at any step** returns the display to video. The meter is
  closed. Your configuration is not changed.
- **Dynamic Drive (the LED drive) is held at full light** while patches
  show, so the light does not change during measuring. VP restarts it
  when the patches end.
- **A renderer restart cancels the run.** A new signal or VP closing does
  the same. The renderer is returned to video.
- **Config silent for 60 seconds cancels the run** during a measuring
  step. Keep the Config app open and connected.
- **A hung meter times out.** After the reading time limit, VP gives up on
  the reading and ends spotread.
- Video is paused while patches are shown. This is expected.

## 11. Troubleshooting

| Message or symptom | Likely cause | What to do |
|---|---|---|
| ArgyllCMS was not found | Argyll is not installed, or not where VP looks. | Install it (section 2.1). Set **ArgyllCMS folder**, Apply, press Detect. The message lists what VP searched. |
| No meter found | Meter not plugged in, driver missing, wrong meter number. | Plug it in directly, not through a hub. Install Argyll's USB driver for i1Pro, ColorMunki Photo and older meters. Press Detect and check **Meter number**. |
| "No pseudo-console is available" | Windows older than 10 version 1809. | Update Windows. |
| The meter needs calibration | Spectros and some others need a sensor calibration. | Follow the instruction (dark position or white tile), then press Continue. |
| Readings too dark, or near-black patches marked below the meter floor | Room not dark, meter not aimed, or the meter's floor is above the display's black. | Darken the room. Re-aim in the Place step. Raise **Near-black readings**. Consider a more sensitive meter (section 4). |
| "VP cannot show calibration patches... Start VP's renderer with any source" | No source is playing, or the Config app is not connected to VP. | Start VP, play any source, connect the Config app, then press Start again. |
| "...floating-point (FP16) swapchain: exact patch codes need an 8- or 10-bit output" | VP's output is FP16. | Switch VP's output to 8-bit or 10-bit and run again. FP16 is not supported. |
| BTB / WTW patches are skipped | The output is full range, which has no codes outside black to white. | Expected. To measure BTB / WTW, use a limited-range output. |
| spotread rejected an option (display type or correction file) | The meter does not accept that `-y` type or `-X` file. For example a CCSS was given to a meter that takes only CCMX. | Detect lists the meter's display types. Clear **Display type** or **Spectral correction**, or use a file the meter accepts (section 3). |
| The meter stopped answering | Cable, hub or a hung Argyll. | VP tries to reconnect. Otherwise unplug and replug, and try another Argyll release. |
| Apply is not possible | The configuration has no Color / Output profile. | Create or select a Color / Output profile in Config, then run again. The measurements stay in the run folder. |
| Run cancelled by itself | The renderer restarted, or Config was silent for 60 s. | Keep Config open. Do not change VP's output while measuring. |
| Start gives old settings | Settings changed but not applied. | Press Apply in the Config app first. Calibration uses the saved settings. |

VP's runtime log has more detail, for example a line starting
"Calibration patterns: started".

## 12. Current limits

Be aware of these:

- **LED characterisation is not in the wizard yet.** The flux table, the
  CNTR curve and the LED latency are not measured by the wizard. They
  stay as they are today.
- **Ambient light checks are not wired.** The wizard does not check or
  report room light. Darken the room yourself.
- **HDR (PQ) output measurement is implemented, but SDR is the tested
  path.** Treat an HDR run as experimental.
- **The real-meter path has not been run on real hardware.** No meter was
  available while the feature was built. It is covered by a hardware test
  that is off by default and is switched on with `VP_TEST_METER=1`. The
  rest was tested with a fake spotread program and a simulated display.
  Expect rough edges with your first real meter. Use the Dry run first,
  then the Quick plan, before a long run.
