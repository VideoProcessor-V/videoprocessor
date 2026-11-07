# VP-V unit: repair

Encoding repair at the encoded resolution (net A / VP restore / off), deband, and the texture-restore add-on.

One folder per slot below. Each slot's README.md is its module doc, and its MANIFEST.json holds
its components and its live file list. The order and zones come from docs/design/slots.json;
see src/VP-V/core/README.md for how the skeleton runs them.

| Order | Slot | Status | Zone | Purpose |
|---|---|---|---|---|
| 70 | [encoding](encoding/README.md) | placeholder | front_end | Remove codec damage (blocking, ringing, mosquito, smear) at the ENCODED resolution, where the damage lives. |
| 80 | [deband](deband/README.md) | built | front_end | Remove banding at source resolution, after repair (cheaper and correct for source-resolution bands). |
| 90 | [texture_restore](texture_restore/README.md) | built | front_end | Long-memory add-on on A's output: texture restore (~2 s peak-hold fine-band reference along vectors) and breathing reduction (detail_pulse_stabilise, the GOP pulse). |
| 85 (draft) | [grain_split](grain_split/README.md) | draft | none yet | The shared grain-free picture (clean = base + f D, G = Y - clean, texture_restore's Wiener split promoted): computed once per frame, read by temporal_detail (later chroma, mosquito repair, scaling); readers add their frame's G back exactly. No slot, no call sites. |
| 90 (draft) | [temporal_detail](temporal_detail/README.md) | draft | none yet | Successor of texture_restore (reference-based structure transfer for breathing and detail, grain untouched); a draft CPU model with tests, no slot, no call sites, not in slots.json. |
| 95 (draft) | [stack](stack/README.md) | draft | none yet | The repair-stack ORDER as one explicit, tested declaration: temporal detail -> deband -> contrast recovery (WP9); deband is the fallback layer under temporal detail. Pure declaration, no pass, no call sites. |
