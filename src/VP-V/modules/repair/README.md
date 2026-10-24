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
