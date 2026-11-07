# Repair stack order (`repair.stack`, DRAFT)

The order of the repair stack as one explicit, tested declaration: **temporal detail -> deband -> contrast recovery** (WP9). Deband is the
fallback layer under temporal detail. Pure declaration (`RepairStackOrder.h`): no pass, no slot, no key.
Unit: repair/stack | draft, no slot, no call sites in the renderer | hook: n/a (it orders existing stages).

## Settings

| Key | Section | Type | Range / values | Default | Config placement | Original (Bill) | Fallbacks / status | Tiers |
|---|---|---|---|---|---|---|---|---|
| (none) | | | | | | | | |

## Environment and diagnostics

| Name | Purpose | Default | Scope (test/debug/runtime) |
|---|---|---|---|
| (none) | | | |

## Products

None made. The order is read against the catalog's pathway order (`core/ProductCatalog.h`: temporal detail repair 10, debander repair 40,
contrast recovery need repair 55; a test compares the rows) and the slot table (`core/PipelineSlots.h`: repair.deband 80,
repair.texture_restore 90; temporal detail and contrast recovery are not slots).

## The order and why

1. **Temporal detail first.** It rebuilds structure from the anchors; everything after works on the repaired picture.
2. **Deband second, as the fallback layer.** Where temporal detail covered a block (`td.maps` validity / stability) the debander does not
   deband it a second time; where temporal detail is absent it debands everywhere, exactly as it does today.
3. **Contrast recovery last**, on the debanded picture.

The earlier text (doc 62 section 3.4, Stack A) read deband -> temporal detail -> contrast recovery (`LegacyOrder()`); only the position of
temporal detail moved. Deband and contrast recovery keep their relative order, which is why today's behaviour (temporal detail not built)
is byte-identical: the layers that run are {deband, contrast recovery} in both declarations.

## Status and fallbacks

`Decide(tdRequested, tdUsable, tdReason)` gives the plan for a frame.

| State | Layers that run | Status |
|---|---|---|
| temporal detail not requested (today: no key asks for it) | deband, contrast recovery | none (not a fallback; byte-identical) |
| requested and usable | temporal detail, deband, contrast recovery | none |
| requested, products absent / stale / of another unit version | deband (the fallback layer, everywhere), contrast recovery | "Repair stack: temporal detail -> deband -> contrast recovery; temporal detail unavailable (<reason>), deband runs everywhere as the fallback layer" |

The debander exposes it as `VpDebander::Module::StackPlan()` / `StackStatusLine()` (the plan from its own temporal-detail read, which also
engages `product.debander [td: <cause>]` through `TdProducts::SyncRead`). The host appends `StackStatusLine()` to the "VP deband" status
line when the engine becomes requestable; that one-line call site is in the renderer's status zone and is listed as an open item of the
wave-S2 skeleton package (it is a no-op today: the line is empty while nothing is requested). Recovery is by itself.

## Not in this unit

WP4b (`DebandRef.h`), WP5b and WP8 are deferred (coordinator, 2026-11-07): the deband and contrast-recovery reference rewrites are not
touched here, only the order is declared.

## Tests

`tests/RepairStackOrderTests.cpp` (VideoProcessor-Test, CPU only): the order table (the three layers once each, ranks, precedes), the legacy
order differs only in the position of temporal detail, today's layers equal in both orders, the plan for every temporal-detail state, the
catalog pathway order agrees, the debander's plan is today's (nothing requested, no line). Command:
`vstest.console.exe x64\Release\VideoProcessor-Test.dll /Tests:RepairStackOrderTests /Logger:trx`.
