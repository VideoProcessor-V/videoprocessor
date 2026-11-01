# tools/m2: move VP-V code out of Bill's project and test files

Copyright(C) 2026 VP-V contributors; SPDX-License-Identifier: GPL-3.0-only

All scripts are stdlib-only Python (run with `python -I tools/m2/<script>.py`), take
`--bill <ref>` (default `a591ad3b`, Bill's latest beta tip) and have three modes:

| mode | effect |
| --- | --- |
| `--dry-run` (default) | prints the plan as a sorted table plus counts, writes nothing |
| `--apply` | writes the result (git mv for file moves) |
| `--verify` | exit 1 if a dry-run would still change something |

They recompute everything from the current tree and the bill ref on every run and are idempotent:
a second `--apply` on an applied tree leaves `git status` empty. Bytes outside the edited spans,
newline style (LF/CRLF) and BOM are preserved.

## move_project_items.py (P1, H7)

Moves every item element (`ClCompile`, `ClInclude`, `None`, `ProjectReference`, ...) of the Bill
projects listed in `m2common.BILL_PROJECTS` that Bill's version of the same project does not have
into `src/VP-V/build/<name>.items.props` (name = project basename without trailing `.vcxproj`).
The project gets one import line before `Microsoft.Cpp.targets`
(`$(MSBuildThisFileDirectory)..\VP-V\build\<name>.items.props`, conditioned on `Exists`). An existing
import of the same props (H7 from another branch) is reused. Matching `*.filters` lose the entries of
moved items and VP-only `<Filter>` definitions nobody references. Bill items that differ from Bill's
text are reported ("modified Bill item, stays"), as is any other Bill `*.vcxproj` under `src/` whose
items differ (not touched). Include paths are copied verbatim (MSBuild resolves them against the
importing project's directory).

## move_tests.py --phase move (P15a)

`git mv`s tracked `.cpp/.h/.hpp/.inc` files under the three test project folders that are absent at
the bill ref AND have no history in Bill's ancestry, to the destinations in `test_moves.json`
(`rules`, first match wins; `keep` is never moved). Then it fixes references deterministically:
project/props/filters `Include` values, `#include` lines (resolved before and after the move with the
origin project's include directories; rewritten to `<VP-V/tests/...>` only when the resolved file
would change; `"pch.h"` is never rewritten), and `tools/test-categories.json` `"file"` values. It
reports other textual references (docs, scripts), `__FILE__` uses and unresolved includes whose name
matches a moved file. The map old -> new is written to `test_moves.json` `"resolved"`; later runs use
it to repair references that a merge reintroduced to an old path. Requires `move_project_items.py`
to have been applied first (Bill projects must hold no moved item).

## move_tests.py --phase split (P15b)

For the CppUnitTest files in `test_moves.json` `"split"`, TEST_METHODs (with their
`BEGIN_TEST_METHOD_ATTRIBUTE` blocks and leading comments) and TEST_CLASSes that Bill's version of the
file lacks move to `src/VP-V/tests/suite/<Stem>VpvTests.cpp` (the Bill class `X` becomes `XVpv`, VP
classes keep their names). Unused helpers are pruned in both files (fixpoint, conservative), VP-added
`#include`s that the Bill file no longer needs are dropped when provably unused. The new file is added
to `VideoProcessor-Test.items.props`; `test-renames.json` lists `Class::Method -> ClassVpv::Method`
and `tools/test-categories.json` follows. A later run appends new VP methods into the existing `XVpv`
class instead of regenerating the file (helpers that those methods need are NOT copied: build and add
them by hand). A file that cannot be compiled after the split goes into `"exclude"` with the reason.

`move_tests.py --compare-lists BEFORE AFTER [test-renames.json]` compares two sorted test listings
(exit 1 if they differ by anything other than the renames).

## Re-running after integ/batch1 merges new branches in

1. `python -I tools/m2/move_project_items.py --apply` (new VP items in Bill projects go to the props).
2. `python -I tools/m2/move_tests.py --phase move --apply` (new VP-only test files, stale old paths).
3. `python -I tools/m2/move_tests.py --phase split --apply` (new VP methods inside Bill test files).
4. Run each with `--verify` (expect exit 0) and build.

## Deviations and open items

- Include resolution for files that are not in a test project (for example `src/VP-V/tests/*.h`, which the
  Test project compiles) uses the union of both test projects' include directories after the includer's
  directory, because those files include `"ChainGpuProbe.h"` and similar through the Test project's `.`.
- `__FILE__`-based repository-root lookups are reported by P15a but NOT rewritten by the script. In this
  run they were fixed by hand (commit "P15 fix"): the level counts in `ConfigFileVpvTests.cpp`,
  `LedDriveDelta1Tests.cpp`, `LedScriptCorrelationTests.cpp`, `BetaGpuTests.cpp` (3 -> 5, 2 -> 4) and the
  `\src\VideoProcessor-Test\` markers (now `\src\VP-V	ests\`). After a later re-run, new moved
  files or newly appended methods that use `__FILE__` need the same hand fix (the report lists them).
- The split does not prune standard-library `#include`s (kept when unsure): `<cstring>` stays in
  `DolbyVisionLuminancePolicyTests.cpp` (one line of residual footprint).
- `VideoProcessorConfigTests.exe -functions` RUNS the Qt tests; no ConfigTests listing was taken (the
  sources of that project are unchanged apart from P1's project items).
- `VideoProcessorBetaTestsHost.dll` is not part of the solution build; no listing was compared.
- Not split (open items): `ConfigEditorWindowTests.cpp` (Qt, H3 / plan 4.3), `BetaParametersTests.cpp`
  (no TEST_METHODs), `MagewellCaptureTests.cpp` (Magewell isolation rules).
- Not edited (reported only): docs that name moved paths, `tools/ci/select_ci_scope.ps1` (selects tests by
  the old `src\VideoProcessor-Test` folder; CI is disabled) and `tools/test_magewell_isolation.ps1`
  (Magewell files did not move).
