# P3: cpu-defaults golden regenerated (fingerprint order only)

Branch r1w1/p3, regenerated from a full-solution x64 Release build of 55891cf1 (VP_TEST_REGOLDEN=1).

## What changed
Only the order of the `fp[NNN]` lines. The 1673 changed lines of
src/VP-V/tests/golden/cpu-defaults.golden.txt are all fp lines. Every other line is byte-identical.

## Proof (old golden vs regenerated, per case)
| case | values old / new | multiset equal | non-fp lines identical |
|---|---|---|---|
| empty | 310 / 310 | yes | yes |
| sample-VideoProcessor.cfg | 310 / 310 | yes | yes |
| original-pathway-on | 310 / 310 | yes | yes |
| tier-Classic | 310 / 310 | yes | yes |
| tier-Low | 310 / 310 | yes | yes |
| tier-Mid | 310 / 310 | yes | yes |
| tier-Upper | 310 / 310 | yes | yes |
| tier defaults | 0 / 0 | yes | yes |
| original pathway overrides | 0 / 0 | yes | yes |

An earlier run showed 311 values (one extra empty field at the join). That run used stale binaries
(x64\Release copies of the renderer DLL and test DLL predated the trailing-'|' strip in
VpvRendererSettings::Fingerprint). On a full-solution rebuild the extra field is gone.

## Why the order changed
P3 moved the VP-V fingerprint terms out of Bill's EffectiveSettingsFingerprint into one hook appended
at the end (VpvRendererSettings::Fingerprint). The new string is Bill's text (a591ad3b order) followed
by the VP-V terms. The old golden held VP's interleaved order, which also reordered Bill's own terms:
the old golden has upscaler|chromaUpscaler|downscaler, while Bill's a591ad3b has
`settings.upscaler << '|' << settings.downscaler << '|'`. Restoring the old order would need changes
inside Bill's body, so it is not possible with call sites only.

## Doc 52
Row 101 says cpu-defaults.golden.txt is "unchanged"; row 102 says the fingerprint string order may
change and only the equivalence partition must hold. The two rows conflict for the positional fp lines.
Row 102 governs the fp order; the multiset proof above shows no value was added, dropped or altered.

## Consumers of the fingerprint string (checked 2026-11-07)
The string is built only by `EffectiveSettingsFingerprint` (LibplaceboVideoRenderer.cpp) and is never hashed or written out.
Every consumer compares two strings produced by the same running renderer DLL, so a change of layout between builds cannot
be seen by any of them.

| Consumer | Use | Memory-only? |
|---|---|---|
| `PublishSettingsState` / `GetPublishedSettingsState` (Impl, ~12088, ~12282) | stores the live and restart strings of the active settings | yes, in-process members under a mutex |
| Renderer restart decision (~28579-28589, ~30058-30068) | new restart string vs the published one; new live string vs the published one | yes |
| `shaderProcessingChanged` (~12407), transport compare (~11937) | restart string of active vs next settings | yes |
| `IsSubtitlePresentationOnlyUpdate` (SubtitleProfileUpdatePolicy.h; callers ~12302, ~12484) | equality of two strings, current vs normalised copy | yes, equality only, no parsing |
| `subtitlePipelinePreparationKey` (~12830) | key of the in-memory `NlsPipelinePreparation` state; the key is rebuilt each run | yes (no file I/O in NlsPipelinePreparation) |
| Test exports `VideoProcessorLibplaceboSettingsFingerprintForTest`, `@@fingerprint-live/-restart` dump (~4632-4658) | tests only | yes |

Nothing persisted is keyed on it: no shader or pipeline cache, calibration/LUT or state file, profile cache, crash state ring,
render-ahead or latch state, or written configuration. The host executable never reads the string (only the renderer DLL does).
Other things named "fingerprint" in the tree (light-level LUT, net fingerprints, MadVR shader state, display gamma hash,
MotionConfig/ToneMappingTuning terms) are separate values and unaffected; ToneMappingTuning and MotionConfig terms remain inside Bill's part.
Effect of the new order: none across restarts, so no one-time invalidation.
Within one session a settings change is detected exactly as before: the equality partition over configurations is unchanged
(RendererSettingsIdentityTests).
