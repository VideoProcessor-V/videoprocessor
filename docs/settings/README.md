# Settings documentation

| File | What |
|---|---|
| `MASTER-SETTINGS.md` | GENERATED master list: modules index, one table per pipeline unit, tier presets, appendices (undocumented keys, stale rows). Never edit by hand. |
| `UPSTREAM-KEYS.txt` | Keys of Bill's schema files (out of scope for the list). Rebuilt with `--write-upstream <git-ref>`. |
| `VP-settings-reference.md`, `organisation.md` | Plain-language guide and the Config page map (hand-written). |

The source of truth for each key is the `## Settings` table in its module README
(`src/VP-V/modules/<unit>/<submodule>/README.md`), in the template of the reset-1 worktree plan, section 3.
A worked example: `src/VP-V/modules/colour/extended_range/README.md`.

## Add a key

1. Code: add the key and its validator/default where the code keeps them
   (`RendererProfileConfig.h`, `MainConfigSchema.h`, a `[vpv]` key list, a `[vpv.<name>]` settings header with
   `kSection`, or a VP-V field table `VpvRenderer*.{h,cpp}` / `VpvRendererFields.h` once settings move to
   `src/VP-V/modules/control/config/`). Add it to the module `MANIFEST.json`
   `components.config_keys`.
2. README: add one row to the module's `## Settings` table. Exact header row, one key per row, key in backticks:

   `| Key | Section | Type | Range / values | Default | Config placement | Original (Bill) | Fallbacks / status | Tiers |`

   - Section: `[vpv]`, `[vpv.<name>]` (the key is then listed as `vpv.<name>.<key>`) or `profile (...)`.
   - Type: `bool`, `int`, `float`, `enum` or `string`.
   - Default: one plain token (`on`, `0.5`) is compared with the defaults golden
     (`src/VP-V/tests/golden/cpu-defaults.golden.txt`: `case empty`, then `tier defaults`); `on`/`true`/`1`
     and `off`/`false`/`0` compare equal. Text that is not a single token (a formula, "from the tier")
     is not compared.
   - Planned or declared-not-wired keys: start the status cell with `planned` or `declared`; they are then
     exempt from the "documented but not in code" check.
   - Wording: never name the owner's display; write generic text such as "output links/displays of limited
     bit depth (8- or 10-bit)". Tiers are budget-driven, never "GPU X only".
3. Regenerate: `python -I tools/gen_master_settings.py`.
4. Check: `python -I tools/check_master_settings.py --strict`. If you closed gaps, shrink the baseline:
   `python -I tools/check_master_settings.py --write-baseline` (it refuses to add a gap).

## The checker

`tools/check_master_settings.py` extracts the keys in code (validators and schema entries of the three schema
files, the `[vpv]` key lists, VP-V field tables in the old and new layout, `[vpv.*]` namespaced sections) and
compares them with the README rows.

- Always an error (exit 1): malformed or duplicate row, a row naming the owner's display, a
  `MASTER-SETTINGS.md` that is out of date.
- Gaps, ratcheted by `tools/master_settings_baseline.json`: `undocumented` (key in code, no row),
  `stale` (row, no key in code), `default_mismatch` (documented default differs from the golden).
  `--strict` fails on any gap that is not in the baseline; the baseline only shrinks.
- CI should also run `--strict --base-ref <merge-base>`: the baseline and `UPSTREAM-KEYS.txt` may not grow
  relative to it (otherwise a gap could be hidden by editing either file).
- Known limits: keys are matched by bare name; keys defined only in other validator headers
  (e.g. RendererConfigView.h, FileHostSettings.h) are not scanned; fenced example tables are ignored.
- Tests: `python -I tools/test_check_master_settings.py` (fixture trees, plus the repository's own gate).
