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
