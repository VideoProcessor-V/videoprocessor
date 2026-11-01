# GPU memory: scratch arena and product textures (`control.gpu_memory`)

Unit: **control** | order: 935 | status: **partial** (draft manifest until the renderer call site is wired; SA1: core + backend + TextureRestore adoption)

## Purpose / why

One place that owns VP's shared GPU temporaries (doc 51 section 3). Per-render temporaries are requested per scope; scopes run one after another, so the arena keeps per texture shape only as many textures as the busiest scope needs (not the sum). Product textures (ring + live pool of a skeleton product) come from a declared layout whose bytes equal the product's `SizeRule`.

## Contract

- Core (pure C++14, no GPU): `src/VP-V/core/GpuAllocator.h` (TexFormat, TexShape, `PlanArena`, `ScratchArenaUser`, `ArenaGrantFor`, `ProductTextures`, `VramLedgerLines`) and `src/VP-V/core/ScratchArena.h` (`ScratchArena<Backend>`).
- Backend: `PlTexBackend.h` (this folder, header-only, libplacebo: `pl_find_fmt` with fixed caps sampleable|storable, `pl_tex_create`/`pl_tex_destroy`).
- Governor: user `kUserScratchArena`, levels [plan(T2), plan(T1), plan(T0), 0] (equal levels dropped), tier T1, not lossless. `Residency::Transient` products are not ring products (no lane grant, no governor user); the arena user counts them.
- Determinism: the plan does not depend on request order; the k-th Acquire of a shape in a scope gets pool slot k; Acquire never allocates.
- Single render thread (another thread's Acquire returns null and is counted).

## Fallbacks (owner rule)

Acquire beyond capacity, for a tier the plan shed, outside a scope or off-thread returns a null handle. The consumer runs reduced/off for that frame; `ScratchArena::FrameDecision()` is the FallbackCatalog entry `scratch_arena` ("scratch memory short: <owner> reduced/off"), released by itself on the first frame without a shortage (recovery once a larger plan is reserved).

## Call sites for the hook layer (not wired here; zero Bill-file lines)

1. Renderer ownership: one `ScratchArena<PlTexBackend>` created with the libplacebo gpu next to the module objects in `LibplaceboVideoRenderer` (reserve after each governor update: `ArenaGrantFor(requests, grant of kUserScratchArena)` then `PlanArena(requests, tier)` then `Reserve`). Requests come from `TextureRestore::ArenaRequests` (and later modules).
2. Per frame: `arena.BeginFrame()` at render start; after the frame `Sync(registry, arena.FrameDecision())` (FallbackCatalog) in the place where `GetFallbacksStatus` (`LibplaceboVideoRenderer.cpp`, near line 33017) collects the entries.
3. Ctrl+Alt+D ledger: `VramLedgerLines(tierPlan, arena.Plan(), registry)` appended to the diagnostic history written by `LibplaceboVideoRenderer::WriteDiagnosticHistory` (near line 37965). No generic provider registry exists yet; adding one is the M2 hook.
4. `TextureRestore::Params::arena` receives the arena pointer where the renderer builds the texture restore Params.

## How to build / test

Tests: `src/VP-V/tests/GpuAllocatorTests.cpp` (CPU: plan, order independence, determinism, exhaustion + recovery, tier shed, Transient residency, ProductTextures, layout bytes, per-tier VRAM report at 1080p/4K/8K, ledger) and `src/VP-V/tests/PrivateTextureScanTests.cpp` (static scan: no private `pl_tex_create` in repair/scaling modules outside the justified allow-list). WARP: TextureRestoreGpuTests arena tests.

## Audit-1 changes (SA1/SA2)

- Governor: two arena users. `kUserScratchArena` (tier T0, levels [plan(T0), 0]) and `kUserScratchArenaExtra` (tier T1, levels [plan(T2) - plan(T0), plan(T1) - plan(T0), 0]): pressure on the optional temporaries never takes the T0 ones. `AddScratchArenaUsers`, `ArenaGrantFor(requests, t0Grant, extraGrant)`.
- Scopes: a nested `BeginScope` is refused (returns false, counted, shown by `FrameDecision`; the outer scope is untouched); a caller closes only a scope it opened.
- Usage is counted per (owner, shape), up to that owner's admitted count, from that owner's slice of the pool: one owner cannot take another's textures.
- `ProductTextures::Allocate(node, decl, layout, dims, plan)` refuses (visibly, FallbackCatalog "product textures") when the layout's bytes do not equal the declaration's SizeRule, unless the declaration is a planning-only size (`ProductDecl::planningSize`, the chroma product P10c): then the governor uses the ACTUAL bytes (`TierInput::actual`) and the layout takes the real plane dims (`TexTerm::absW/absH`), never an assumed subsampling.
- The arena path destroys the private temporaries it replaces (TextureRestore half/tmp and the foreign trio, the grain Splitter's rows/bands/quiet).
