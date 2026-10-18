# Real Dolby Vision RPU metadata fixtures (DV autotest Tier 2)

Metadata only - no pictures. Source: the test assets of quietvoid/dovi_tool
(libdovi), commit 614c816b6446dcd1dbaf433403d499a6026fbb5a, `assets/tests/`.
Licence: MIT, Copyright (c) quietvoid - see `LICENSE.dovi_tool.txt`.

| file | what |
|---|---|
| `cmv40_full_rpu.bin` | profile 8, every level (L1-L6, L8-L11, L254) |
| `profile5.bin` | profile 5 (IPTPQc2) |
| `profile20_apple.bin` | Apple profile-5 RPU (L1, L3, L8-L11, L254) |
| `cmv4_2_510_xml_rpu.bin` | profile 8, 24 frames, CM v4.2 |
| `*.dm.txt` | `tools/dv_rpu_to_dm.py` output of dovi_tool's JSON export (`dovi_tool export -i X.bin -d all=X.json`, dovi_tool 2.3.4) for the files above plus `profile5`, `mel_variable_l8_length13`, `unordered_l8_blocks`, `level253-ext-metadata`, and the first 48 frames of `cmv4_0_2_xml_rpu`, `hevc_tests__regular_rpu` and `cmv2_9_xml_with_l5_rpu` |

SHA-256: cmv40_full_rpu.bin 0123fb22...d6b457, cmv4_2_510_xml_rpu.bin b5dbcdfe...c109e,
profile20_apple.bin bc6d6261...f242, profile5.bin f2e6a33c...13f2.

The full set (37 files, with JSON and summaries) is on the test PC under
`D:\testclips\dv\` (README there); `DvRpuAnchorTests` uses it when present
(`VP_TEST_DV_RPU_DIR` overrides the location).
