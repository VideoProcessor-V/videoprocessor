# Upstream pull request plan (FL35 work)

Status: not submitted. Nothing has been pushed anywhere; all commits and tags
exist only in the local checkout `C:\dev\videoprocessor`, branch
`stage2-edge-blanking` (based on tag `1.4-beta-rc1`).

Target: `billslack2/videoprocessor`, only after the work is verified on the
projector (brief stage 3). Bill owes nothing; offer, expect nothing.

## Tested snapshots

| Tag | Commit | Contents |
|---|---|---|
| `fl35-blanking` | `84883ae5` | Edge blanking + blanking-stage test config |
| `fl35-stage2` | `5dcfc088` | Stage 2 complete |

Builds of both: `C:\dev\deploy\` (deploy folders + `archive\` ZIPs).

## Split into two independent PRs

**PR 1 – viewport edge blanking**
- `c9ddc15f` Add viewport edge blanking (blank_left/right/top/bottom)
  — self-contained: `EdgeBlanking.h`, renderer, RendererProfileConfig,
  Config app fields, tests.

**PR 2 – custom GLSL hook chain**
- `8f9be0d1` Ship SSimDownscaler, CfL chroma and sigma-delta GLSL shaders
  (shader files, vcxproj copy items, release manifest, shader count 7→11).
- `5dcfc088` VP Renderer: custom GLSL hook chain, dither suppression,
  saturation — drop its `docs/cinema/VideoProcessor-test.cfg` change.
  Optionally split `saturation` into its own commit/PR if Bill prefers.

**Never upstream:** `0f7143d9` (project brief + shader draft as a docs
commit), `84883ae5` (personal test config), anything under `docs/cinema/`.

## Steps when ready

1. Discover Bill's current beta branch and default branch (see `AGENTS.md`:
   `gh repo view billslack2/videoprocessor --json defaultBranchRef`), fetch it.
2. New branch per PR from the beta tip; cherry-pick the commits above;
   resolve conflicts.
3. Full x64 Release rebuild (`/t:Rebuild`) and all native tests; compare
   against the known environmental failures only.
4. Licence question for Bill: igv SSimDownscaler is LGPL-3.0 (header kept),
   Artoriuz CfL is MIT (licence file shipped). Sigma-delta is
   GPL-3.0-or-later, same as the fork.
5. Ask Victor before each outward step: creating a GitHub fork, pushing,
   opening each PR.
