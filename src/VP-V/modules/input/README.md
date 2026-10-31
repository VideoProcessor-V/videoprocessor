# VP-V unit: input

Capture, signal, the queue and timing (the one total delay), and source analysis on the original frame.

One folder per slot below. Each slot's README.md is its module doc, and its MANIFEST.json holds
its components and its live file list. The order and zones come from docs/design/slots.json;
see src/VP-V/core/README.md for how the skeleton runs them.

| Order | Slot | Status | Zone | Purpose |
|---|---|---|---|---|
| 10 | [capture](capture/README.md) | built | host | Bring frames in from DeckLink/Magewell (or a file source) without losing precision: GPU unpack of v210/UYVY to P210 4:2:2, native RGB ingest for r210/R12B/R12L, upload reuse and early upload. |
| 20 | [signal](signal/README.md) | built | host | Identify the signal (SDR / HDR10 / LLDV / DV tunnel), its metadata and its colorimetry, and unpack the DV tunnel, so every later stage knows what the pixels mean. |
| 30 | [queue_timing](queue_timing/README.md) | built | host | Hold ONE user-set total delay (lip sync with the AVR's fixed audio delay), the SAME at every frame rate (padded); 4 queued frames recommended (~230-250 ms at 24p) with a graceful 4/3/2 ladder; cadence/pulldown detection and the film clock; stale-burst handling. |
| 40 | [source_analysis](source_analysis/README.md) | partial | host | Analyse the ORIGINAL delivered frame before anything changes it: source resolution + scaler kernel pre-analysis (per axis), active picture/crop, cut/fade/black, subtitle detection, and the labels net A needs (bit depth, ratio, kernel, bitrate tier, codec family). |
| 40 | [source_analysis/identity](source_analysis/identity/README.md) | draft (part of source_analysis) | host | Combine the classical readings (and, later, the ID net) into ONE hysteretic answer to "what is this source": per-axis ratio, kernel, codec class, quality tier; detect ABR rendition switches; tell the user once when the stream quality dropped. Header-only, not wired. |
