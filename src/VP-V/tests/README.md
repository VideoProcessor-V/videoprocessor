# VP-V tests

VP-V unit tests. Each runs on the CPU unless its name says GPU.

- **Build.** `VideoProcessor-Test` compiles the files in this folder, under the filter "VP-V".
- **Run.** Run them single-threaded, with
  `vstest.console.exe x64\Release\VideoProcessor-Test.dll /TestCaseFilter:FullyQualifiedName~PipelineSkeletonTests /Logger:trx`.
- **Current file:**
  - `PipelineSkeletonTests.cpp` covers the slot table, the zero-cost rule, the entrance order
    and cache, the reorder validator (valid and invalid orders), fingerprint keys, the Original
    pathway, the output-hook order, per-axis vectors, the delay budget and slot timing.
- **Module tests.** A module's own standalone tests go in `src/VP-V/modules/<unit>/<slot>/tests/`
  as the module migrates.
- **VP hook tests moved out of Bill's LibplaceboLutParserTests.cpp** (feat/lut-tests-out):
  the VP-0173 validation filter in Bill's `docs/VP-0173-validation.md` no longer reaches them.
  To run them with that check, add `|FullyQualifiedName~HookTests` to its `/TestCaseFilter`
  (Bill's document itself stays unchanged).

## CPU defaults golden

`SettingsDefaultsGoldenTests` (CPU tier) evaluates fixed configurations with the renderer's own
loader (the settings exports of `vprenderer\VideoProcessorVPRenderer.dll`, no device): the empty
config, the repository sample `VideoProcessor.cfg`, `[vpv] pipeline_original_pathway: on`, and each
global tier Classic / Low / Mid / Upper (every tiered unit's preset keys). For each it records the
settings dump and the effective-settings fingerprint as `fp[NNN]=value` lines (the identity and path
fields are stripped), then the tier defaults and the Original pathway overrides. The text is compared
with `golden/cpu-defaults.golden.txt` (checked in).

- A mismatch fails with the first 20 differing lines.
- **Review rule:** a differing line is either a bug or an intended default change. Keep it only if the
  owner-visible behaviour is meant, then regenerate with `VP_TEST_REGOLDEN=1` (the test rewrites the
  source-tree golden, logs `REGOLDEN wrote <path>` and passes), look at `git diff` of the golden, and
  commit it with the change that caused it.
- Not covered: no input-profile mode is selectable for an unknown source (the loader takes the first
  profile), so there is no per-mode case.

`SettingsFingerprintStructureTests` (same tier) checks that every validated key (and every `[vpv]` key)
changes the fingerprint, that the table defaults are valid and match the renderer, and that every VP-V
key is in `docs/settings/VP-settings-reference.md`. Its allow-lists carry a reason per entry (`GAP:` =
a real gap, reported); stale entries fail.

## Frozen references (r6, Bill v1.4.02, B3-on)

The GPU comparison tests need renderer DLLs built from frozen commits. References are never
committed, never built inside a source tree you are working in, and never deployed.

1. Make a separate clean worktree under `D:\codex\videoprocessor\` for each reference (never in a
   shared or existing checkout):
   - **r6:** branch `fix/r6-baseline` (existing worktree `fix-r6`; confirm the commit with
     `git rev-parse fix/r6-baseline`).
   - **Bill v1.4.02:** Bill's `v1.4.02-beta` (`origin/v1.4.02-beta`; the repository has no tag of that
     name, so record the commit you picked; the existing `bill-v1.4.02` worktree sits on another commit).
2. Build x64 Release (`MSBuild.exe VideoProcessor.sln /t:Build /m:4 /p:Configuration=Release /p:Platform=x64
   /p:MagewellSdkRoot=<SDK> /v:minimal`, with the build-slot rules).
3. Copy the whole `vprenderer` folder (renderer DLL and its dependencies) out of the source tree to a
   reference folder, for example `D:\codex\references\<name>\vprenderer\`.
4. Record internally (not in the repository): the commit, and the SHA256 of
   `VideoProcessorVPRenderer.dll`.
5. Environment variables for the GPU tests (run on the GPU machine only):
   - `VP_TEST_BASELINE_RENDERER_DLL` = the r6 renderer DLL.
   - `VP_TEST_BILL_RENDERER_DLL` = the Bill v1.4.02 renderer DLL.
   - `VP_TEST_ENTRANCE_ONCE=1` = run the harness test with `[vpv] pipeline_entrance_once: on` (B3-on);
     the B3-on reference is the picture hash set recorded with it.
   - `VP_TEST_DETERMINISM_SUBSET=1` = per-merge subset of the determinism run-twice test.
6. Regenerate a reference only deliberately: r6 and Bill never (they are frozen); the B3-on reference
   only after an owner-accepted picture change. Without a variable the matching test logs
   `SKIP-NO-BASELINE` and passes.
