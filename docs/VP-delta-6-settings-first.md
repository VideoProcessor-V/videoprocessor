# Delta 6 — settings are the primary control; keys are backup only

2026-10-12. New constraint 12 for the brief; applies to everything built
and everything to come.

## Rule

Every feature's normal operation is governed by **settings** (config
keys, edited in the Config app, per profile where that is the natural
scope). Keyboard shortcuts exist only as a **backup for A/B comparison**
in the cinema; they never are the primary way a setting is chosen, and no
feature may depend on a key having been pressed to be in its intended
state.

Concretely:
- Each toggle or value that a key can change must also exist as a
  config key with a saved value that survives restart; the key only
  overrides it until the next restart or profile change, and the overlay
  shows when a key override is active.
- Profile selection by rule (`when:` on `${cadence}` etc.) is the
  primary mechanism; the manual keys (Shift+L, Shift+Q and the like) are
  the backup.
- New features ship with their Config page or section first; a shortcut
  is optional and documented as "A/B only".

## What to do now

1. Audit pass: list every behaviour today that is reachable *only* by a
   key, or whose saved state differs from what a key sets, and give each
   a config key (one commit, "Delta 6: settings-first audit").
2. Add the rule to the brief as constraint 12 and to `docs/audit/README.md`
   §4 (general checks): "no key-only behaviour; key overrides are shown on
   the overlay".

## Built (2026-10-12) — "Delta 6: settings-first audit"

Survey (read-only) and planner answers: `docs/VP-planner-2026-10-12-answers.md` §4.

- **`[general] stats_overlay`** (default false): the overlay's saved start
  state; the statistics key toggles it until restart. Config: General →
  "Show statistics overlay". Changing the saved value applies it live.
- **`[general] persist_profile_selection`** now defaults to **false**: a key's
  profile choice lasts until restart or a profile change. `true` is the earlier
  behaviour (choices in VideoProcessor.state, kept across restarts as
  fallbacks); the Config tooltip says so. Tests that are about persistence now
  set it explicitly; the shipped sample relies on the default.
- **Key choices released on a profile change:** `UnifiedProfileRuntime::Refresh`
  compares what the rules alone select for the new source with the last
  refresh; when that changes, every live key override is released (and,
  without persistence, its choice). The same source re-detected keeps it.
  Test `KeyProfileChoiceLastsUntilTheRulesChange` (cinema config: Shift+L on
  24p holds Low_Latency, a 60p source releases it, back on 24p it is Normal).
- **A Config reload keeps a live key override** while its profile still exists
  (it previously required persistence, so with the new default any Apply would
  have silently dropped a key's choice). Refinement to the planner's wording:
  a key overrides until restart or a profile change, not until any Apply.
- **"Key override" overlay line:** names each key-made change against the
  saved settings: profile groups held by a key, stats overlay, fullscreen,
  video only, video conversion, renderer, capture device, the DirectShow
  transfer box, legacy shader / display rules. Entries end when Config applies
  that setting (video conversion, transfer, capture device are replaced by an
  Apply, as before) or at restart.
- **`[general] use_frame_queue` / `queue_auto_reset`** (default true): the
  main window's two queue checks get saved values (they were always set on at
  start). Config: General.
- **`[shortcuts] global_hotkeys`** (default false), planner addition 1: the
  background keyboard observer only watches Ctrl / Alt / function-key
  shortcuts; bare keys act only while VP has focus. Refinement: **Shift+letter
  is treated as a bare key** (it is ordinary typing of a capital letter, e.g.
  Shift+L / Shift+Q in the sample); Shift+F-key stays global.
  `ConfigurationLiveApply::IsGlobalSafeShortcut`, unit-tested. Config:
  Shortcuts → "Single keys work while another application has focus".
- **DirectShow-only keys** (planner addition 2): "Automatic transfer" and "PQ
  transfer" are labelled "(DirectShow only)" on the Shortcuts page, with a note
  that such keys do nothing with VP Renderer; no VP-renderer config keys added.
  The legacy shader / display-rule keys exist only in legacy configurations
  (not on the Shortcuts page).
- **Sample config:** the comments now say the shipped `shortcut:` keys are A/B
  backups and that scope is on F2 (it was bound although the comment said
  unbound).
- `docs/audit/README.md` §4 item 7 (no key-only behaviour; overrides shown)
  landed with brief delta 7.

Not changed (survey items left as they are): the main window's other controls
(scene correction, frame offset, DirectShow colour boxes, renderer / capture
device boxes) already have config keys and behave as session edits, like the
keys; the Space key of the output sweep (diagnostic, command-line feature).

## Audit `delta6.md` fixes (2026-10-12)

- A Config Apply that changes `use_frame_queue` restarts the renderer, as the
  main window's check box does (it only ticked the box before).
- "Key override" shows only overrides still in force: fullscreen and video
  only end when toggled back; the automatic display-rule key ends the manual
  display rule. (The legacy shader-rule entry still lasts until restart.)
- Release per group: a live key override ends only when the rules' result
  for *its* group changes (a Shift+Q queue choice survives an HDR -> SDR
  change that moves only the rendering group).
- `persist_profile_selection: true` is exactly the earlier behaviour again:
  live key overrides stay in force across rules changes (test added).
- Deploy note: the default flip means an installed config without
  `persist_profile_selection` no longer keeps key choices across restarts;
  set it to true to keep the earlier behaviour.
