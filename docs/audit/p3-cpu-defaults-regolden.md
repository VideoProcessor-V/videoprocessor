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
