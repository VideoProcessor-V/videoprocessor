# VP-V unit: control

Outside the pixel path: the net runtime, status and diagnostics ("Fallbacks engaged"), and Config.

One folder per slot below. Each slot's README.md is its module doc, and its MANIFEST.json holds
its components and its live file list. The order and zones come from docs/design/slots.json;
see src/VP-V/core/README.md for how the skeleton runs them.

| Order | Slot | Status | Zone | Purpose |
|---|---|---|---|---|
| 910 | [net_runtime](net_runtime/README.md) | placeholder | host | VP-side NetRunner: executes the exporter's mpv-format passes on VP-owned textures via pl_dispatch (fp16 intermediates by default), in the front end and inside the hybrid scaler's POST_KERNEL hook; loads net packages, checks fingerprints (label schema, pass order, intermediate format, upstream versions/states, VP commit) and bypasses on mismatch with a visible entry. |
| 920 | [diagnostics](diagnostics/README.md) | partial | host | One 'Fallbacks engaged: (list)' heading in the Ctrl+I overlay, Config status and the Ctrl+Alt+D bundle; crash state ring; logs. |
| 930 | [config](config/README.md) | partial | host | Per-unit Classic/Low/Mid/Upper tier sliders + global, toggles listed beneath, unbuilt slots shown as 'not yet implemented'; settings preview exe. |
