# VP-V unit: motion

Motion estimation on the original frame at source size, vector delivery per axis, and the motion consumers.

One folder per slot below. Each slot's README.md is its module doc, and its MANIFEST.json holds
its components and its live file list. The order and zones come from docs/design/slots.json;
see src/VP-V/core/README.md for how the skeleton runs them.

| Order | Slot | Status | Zone | Purpose |
|---|---|---|---|---|
| 50 | [estimation](estimation/README.md) | built | front_end | Estimate motion on the ORIGINAL frame at SOURCE size (NVOF seed + VP matcher) with a time-based look-ahead, and keep ONE vectors-valid state that every consumer obeys (invalidated at cuts, drops, repeats, drift, resyncs). |
| 130 | [vector_delivery](vector_delivery/README.md) | partial | host | Hand source-size vectors to consumers at the processing size: PER-AXIS scaling (re-base/4K route give different x/y factors), mapping through crop/A-lens/NLS, and an optional +-1 px local refinement for precision consumers. |
| 140 | [consumers](consumers/README.md) | built | front_end | De-judder, hiccup repair, optional interpolation (exclusive with libplacebo smooth motion), and the temporal consumers (dither vectors, scaler refinements). |
