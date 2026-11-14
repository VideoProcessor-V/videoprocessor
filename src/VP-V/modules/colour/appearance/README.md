# Stage 13 appearance (CAM16) (`colour.appearance`)

Unit: **colour** | side: output | order: 210 | status: **placeholder** | hook point: PRE_OUTPUT

## Purpose / why

CAM16 Stevens/Hunt appearance matching from reference to dim-screen conditions (static per viewing setup), replacing the manual gamma boost + saturation. Off by default; on after the i1Pro screen-white measurement.

## Contract

- Input: linear target RGB at output size
- Output: same domain; BTB/WTW codes pass through unchanged
- Geometry: output size
- Delay share: 0 frames; a few dozen ALU ops per pixel
- Look-ahead: none

## Settings

| Key | Section | Type | Range / values | Default | Config placement | Original (Bill) | Fallbacks / status | Tiers |
|---|---|---|---|---|---|---|---|---|
| `mode` | [ambient] | enum | off, manual, sensor | off | Config > General page > Room lights card > Room lights | n/a (VP-only) | off: rules see no room state; manual: the room set by room; sensor: a serial light sensor decides, falling back to room without readings. | not tiered |
| `room` | [ambient] | enum | dark, lit | dark | Config > General page > Room lights card > Room | n/a (VP-only); feeds the profile rules' ambient variable, not a renderer setting | The room in manual mode and the fallback when the sensor is not reporting. | not tiered |
| `lux_lit` | [ambient] | float | 0 to 100000 | 20 | Config > General page > Room lights card > Lit above (lux) | n/a (VP-only); feeds the profile rules' ambient variable, not a renderer setting | Sensor mode: above this the room is lit. | not tiered |
| `lux_dark` | [ambient] | float | 0 to 100000 | 8 | Config > General page > Room lights card > Dark below (lux) | n/a (VP-only); feeds the profile rules' ambient variable, not a renderer setting | Sensor mode: below this the room is dark, in between it stays as it was; clamped so it never exceeds lux_lit. | not tiered |
| `hold_s` | [ambient] | float | 0 to 600 | 5 | Config > General page > Room lights card > Hold (seconds) | n/a (VP-only); feeds the profile rules' ambient variable, not a renderer setting | Sensor mode: a new room state must last this many seconds before it is taken. | not tiered |
| `port` | [ambient] | string | COM1 to COM256 (sensor mode) | empty | Config > General page > Room lights card > Sensor port | n/a (VP-only); feeds the profile rules' ambient variable, not a renderer setting | The light sensor's own serial port; refused when equal to the projector's port. | not tiered |
| `baud` | [ambient] | int | 1200 to 921600 | 115200 | Config > General page > Room lights card > Sensor speed (baud) | n/a (VP-only); feeds the profile rules' ambient variable, not a renderer setting | Serial speed of the light sensor. | not tiered |
| `ambient_adapt` | profile ([vprenderer.Default] and matching profiles) | bool | on, off | off | VP-V > Colour > Main > Room-light adaptation (experimental); also VP Renderer > Rendering > Experimental content-adaptive settings (upstream copy greyed while VP-V is on) | Original pathway sets `off` (ADR-0019) | Experimental ambient adaptation: renders to the black the room light leaves visible (screen reflection of the room light adds to the display's black), using the [ambient] light sensor (or manual lux_lit), applied at cuts with slow time constants; never moves code 0 or below-black. status line: Appearance: off (shader failed) | same on all (not tiered) |
| `ambient_adapt_strength` | profile ([vprenderer.Default] and matching profiles) | float | 0.0 - 1.0 | 1 | VP-V > Colour > Advanced > Room-light adaptation: values | n/a (VP-only): unchanged by the Original pathway | How much of the room light's veil is rendered into the black (0 changes nothing) | same on all (not tiered) |
| `ambient_max_black_nits` | profile ([vprenderer.Default] and matching profiles) | float | 0.0 - 10.0 | 1 | VP-V > Colour > Advanced > Room-light adaptation: values | n/a (VP-only): unchanged by the Original pathway | The black level (nits) the adaptation never exceeds | same on all (not tiered) |
| `ambient_screen_reflect` | profile ([vprenderer.Default] and matching profiles) | float | 0.0 - 1.0 | 0.01 | VP-V > Colour > Advanced > Room-light adaptation: values | n/a (VP-only): unchanged by the Original pathway | Light the screen reflects back, cd/m2 per lux of room light; effective black = display black + strength x lux x this | same on all (not tiered) |
| `ambient_tau_down_s` | profile ([vprenderer.Default] and matching profiles) | float | 1.0 - 1800.0 | 90 | VP-V > Colour > Advanced > Room-light adaptation: values | n/a (VP-only): unchanged by the Original pathway | Seconds the target takes to follow a falling room light | same on all (not tiered) |
| `ambient_tau_up_s` | profile ([vprenderer.Default] and matching profiles) | float | 1.0 - 600.0 | 15 | VP-V > Colour > Advanced > Room-light adaptation: values | n/a (VP-only): unchanged by the Original pathway | Seconds the target takes to follow a rising room light | same on all (not tiered) |


- Existing keys: (none)
- Planned keys: `appearance_adaptation`, `appearance_strength`, `appearance_reference_white_nits`, `appearance_reference_surround`, `appearance_screen_white_nits`, `appearance_screen_surround`
- Tiers: every tier when on (cost ~0)

## Fallbacks

- "Fallbacks engaged" entry: Appearance: off (shader failed)
- Behaviour: Failure -> off, previous gamma boost/saturation restored.

## Net fingerprint / upstream

- Fingerprint: (none)
- Upstream modules: `colour.sdr_tone_shape`

## How to build / test

Build the solution via the shared build-slot runner (see AGENTS.md); tests single-threaded; GPU tests only on mains with `VP_TEST_ADAPTER=NVIDIA`.

Placeholder tests:
- byte-identical with the slot off
- zero hooks when off

Isolation tests:
- identity at equal conditions; reference CAM16 grey ramp / primaries / skin; monotonic J; hue within 1 degree

Contract tests:
- BTB/WTW probe codes identical on and off

Chain tests:
- constant output across synthetic LED dimming with compensation on

## Known issues / open items

- one owner per correction with the ambient light sensor (no double correction)

## Where the code lives (live file list)

Code location: **module (ambient adaptation config/logic, SdrAmbientLift; slot placeholder)**. The authoritative list is `live_files` in [MANIFEST.json](MANIFEST.json).

- `src/VP-V/modules/colour/appearance/SdrAmbientLift.h` (M2 step 7: the SDR ambient black-lift `SdrAmbientLift` / `ApplySdrAmbientLift`, extracted unchanged from Bill-owned `LibplaceboRenderParameters.*`, which now forwards to it; pure double math, namespace `SdrAmbientLiftModule`)
- `src/VP-V/modules/colour/appearance/AmbientConfig.h` (M2 item 11: ambient-adaptation configuration keys, moved from `src/VideoProcessor-Lib/`)
- `src/VP-V/modules/colour/display_mapping/AmbientAdaptation.h` (M2 item 11: ambient adaptation logic, moved from `src/VideoProcessor-Lib/`; still includes `AmbientState.h` from the Lib folder)

Forwarding headers stay at `src/VideoProcessor-Lib/AmbientConfig.h` and `AmbientAdaptation.h` for Bill's files that include the old paths (`VideoProcessorDlg.cpp`, `MainConfigSchema.h`, `LibplaceboVideoRenderer.cpp`, `ConfigFileTests.cpp`); listed in the MANIFEST.

## Migration directive

Code moves into this folder only when this slot is touched (opportunistic until M1, a dedicated phase after M1, finished by M2). A move = `git mv` + project/include updates + this module's `live_files` + `docs/design/components-master.json` (regenerate with `python tools/check_module_manifests.py --write`) in the SAME commit, with the build, this module's tests and the chain-gate subset passing. Never leave a copy behind; one module per change; never during a release/deploy window. Update this README in the same commit as any slot change.

## Links

- Design: [VP-pipeline-final.md](../../../../../docs/design/VP-pipeline-final.md)
- Slot data: [slots.json](../../../../../docs/design/slots.json)
- Decisions: ADR-0014 in [docs/decisions](../../../../../docs/decisions/README.md)

<!-- vpv-settings-tests:begin (generated by tools/vpv_tests/seed_module_readmes.py) -->

## Settings

- Keys: none yet.
- Planned keys (not yet implemented): `appearance_adaptation`, `appearance_strength`, `appearance_reference_white_nits`, `appearance_reference_surround`, `appearance_screen_white_nits`, `appearance_screen_surround`.
- Tiers: every tier when on (cost ~0).
- VP-V page, Colour tab: not tiered (libplacebo does the colour in every tier; the cheaper combined HDR->SDR LUT for Classic/Low is not yet implemented.).
- Listed under the tier: `ambient_adapt`.
- Plain-language guide: [docs/settings/VP-settings-reference.md#6-colour](../../../../../docs/settings/VP-settings-reference.md#6-colour).
- Fallback entry ("Fallbacks engaged"): `Appearance: off (shader failed)`.

## Tests

- Placeholder (byte-identity on/off, zero cost): byte-identical with the slot off; zero hooks when off.
- Isolation (fixed inputs, expected outputs, cost): identity at equal conditions; reference CAM16 grey ramp / primaries / skin; monotonic J; hue within 1 degree.
- Contract (formats, geometry, fallback entry): BTB/WTW probe codes identical on and off.
- Chain (total delay at the 4/3/2-frame budgets): constant output across synthetic LED dimming with compensation on.
- Combinations: pairwise / high-risk / random lines in `tests/vpv/` that set this slot's unit (`tools/run_combinations.ps1`); matrix: `docs/design/slot-test-matrix.md`.

<!-- vpv-settings-tests:end -->
