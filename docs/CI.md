# CI on the fork (ci-windows)

`.github/workflows/ci-windows.yml` builds `VideoProcessor.sln` (x64 Release, without the
Magewell SDK) on GitHub's `windows-2025` runners and runs the CPU test suite and the Qt
config editor tests. It is a safety net, not the release gate: the local gates
(`tools/run_chain_gate.ps1`, the release skill's full suite) still decide deployments.

## Jobs

```
build ──┬─> cpu-tests (shard 0..N-1, N <= 6) ──> test-summary
        └─> config-tests
```

- **build**: decides the tier (below), restores the build cache, builds, lists the tests
  and plans the shards (`tools/ci/plan_test_shards.ps1`), uploads the runtime outputs.
- **cpu-tests**: each shard runs a disjoint vstest filter. The plan is verified before
  it is written: every selected test matched by exactly one shard, nothing else matched.
- **test-summary**: `tools/ci/summarize_test_shards.ps1` merges the TRX files and fails
  unless every planned test ran exactly once and passed. `SKIP-NO-HARDWARE` markers are
  listed on their own line with test names and count; every other `SKIP-*` marker is
  counted. It also uploads measured per-test seconds (`cpu-tests-weights`), the input
  for `tools/ci/test-weights.json`.
- **config-tests**: `tools/run_config_tests.ps1 -Shards 4`, off screen.

## Tiers

`tools/ci/select_ci_scope.ps1` writes the decision and its reasons to the job summary.

| Push / trigger | Tier |
|---|---|
| `fix/*`, `feat/*`, `test/*`, `research/*` | **affected**: changed projects, dependent test classes |
| `ci/*`, workflow_dispatch with `full: true`, the nightly schedule | **full** |
| any other branch | **full** |

Affected tier, conservatively:

- The base is where the branch leaves the integration branches (every non-feature remote
  branch); the changed files are those differing from it.
- Documentation only (`docs/`, `*.md`, root `*.txt`, `.agents/`, `images/`): nothing is
  built or tested.
- A source file listed in a solution project rebuilds that project and every project
  referencing it. A header rebuilds the projects of every file that includes it,
  transitively (matched by file name, so over-approximated).
- CPU tests: the classes of changed test files and of test files that include a changed
  header (transitively); for a changed library `.cpp`, the test files that include its
  same-named header. A renderer change adds `LibplaceboPluginProxyTests`, which loads the
  renderer DLL. Config editor tests run whenever `VideoProcessor-ConfigTests` is affected.
- Full suite instead whenever a change cannot be mapped: project, solution, props and
  targets files; `3rdparty/`, `shaders/`, `test-fixtures/`, `luts/`, `models/`,
  `packaging/`, `tools/`, the workflow, `VideoProcessor.cfg`; a source no project lists
  or includes; a library `.cpp` without a same-named header, or whose header no test
  includes.

Nothing is excluded by name in either tier. Tests that cannot run on the runner detect the
missing condition themselves (`SKIP-NO-HARDWARE <test>: <reason>`, `SKIP-NO-GPU`,
`SKIP-NO-FOOTAGE`) and are reported. Locally `VP_TEST_REQUIRE_HARDWARE=1` (set by
`tools/run_chain_gate.ps1` and the release skill) turns a missing hardware precondition
into a failure.

## Caching

| What | How | Key |
|---|---|---|
| Qt 6.8.3 | `install-qt-action` cache | Qt version, host, arch |
| v142 ATL/MFC (`VC\Tools\MSVC\14.29.*\atlmfc`) | `actions/cache`; installed with `vs_installer` on a miss (about 5 min) | runner image version, Visual Studio version |
| Build outputs and intermediates (`x64/Release`, `src/*/x64/Release`, `3rdparty/*/x64/Release`) | `actions/cache/restore` + `save`; newest entry with the same prefix | runner image, Visual Studio, Qt, hash of the solution, `*.vcxproj`, `*.props`, `*.targets`; then the run id |
| libplacebo, nvapi, other 3rdparty | prebuilt binaries in the repository: nothing to cache | |
| NuGet | the solution uses no NuGet packages | |

The build cache is made safe for an incremental build by
`tools/ci/prepare_incremental_build.ps1`: the cache records the commit it was built from
(`x64/Release/.ci-build-commit`); every tracked file is dated 2001-01-01 and the files
that changed since that commit are dated now, so MSBuild's own tracking rebuilds exactly
what depends on them. A missing or unknown commit deletes the restore (clean build). LTCG
state (`*.iobj`, `*.ipdb`) is dropped, so links run in full. A full build saves the cache
with its commit; an affected build keeps the restored commit (it did not rebuild other
projects) and saves nothing when it started clean.

GitHub scopes caches by branch: a run can restore caches from its own branch and from the
default branch (`v1.4.02-beta` on the fork), not from other branches. Until the workflow is
on the default branch (where the nightly schedule then keeps the caches warm), the first
push of every branch builds cold and installs ATL/MFC.

## Policy for agents

- **Push when the work is ready, not at every step.** Every push starts a run, and a newer
  push to the same branch cancels the older run (`cancel-in-progress`), so intermediate
  pushes only burn runner time and hide results. Commit locally as often as you like;
  push once the change builds locally and its targeted tests pass.
- Use a feature-branch prefix (`fix/`, `feat/`, `test/`, `research/`) for work in
  progress: it gets the affected tier. Use `ci/*`, or Actions -> ci-windows -> Run
  workflow with `full: true`, for a full run.
- Never exclude a test in the workflow to make CI green. A test that needs hardware the
  runner lacks must detect it and log `SKIP-NO-HARDWARE <test>: <reason>`
  (`src/VideoProcessor-Test/HardwarePrerequisites.h`).
- Poll runs with `gh run watch <id> --exit-status -i 120` or slower (API rate limit,
  shared by every agent on the account).
- Refresh `tools/ci/test-weights.json` from a full run's `cpu-tests-weights` artifact
  when shards drift apart (the summary shows each shard's vstest time).

## Measured run times

See `docs/agent-reports/ci-hardware-skip.md`.
