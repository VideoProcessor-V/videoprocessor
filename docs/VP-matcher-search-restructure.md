# Matcher block search restructure (plan, 2026-10-15)

Owner: the matcher restructure is the next task; the optical flow source is parked (measured slower than the
block search on the laptop and the 3090; kept off by default).

## Why

Laptop RTX 3050 Ti, 4K source, live settings (refined, half size for every pass, M6, landing check), Beta
pass timing (`VP_TEST_TIMING=1`, "live settings" section):

- matcher ~10 ms wall clock; the block search (half size) 7.2 ms of it, the second sweep 1.1 ms, every other
  pass 0.1-0.45 ms.
- A stage-skip profile of the block search: skipping the level-2, level-1 and half-size SADs together leaves
  5.0 of the 7.3 ms. The pixel comparisons are about a third; the rest is the shape of the pass: one 256-thread
  work group per 16-px block (~8,000 groups), ~10 group barriers, single-thread (lead) phases while 255 threads
  wait, and stages that use 169-321 of the 256 threads.
- The SADs themselves are small (level 2: 9 samples; level 1: 16 bilinear samples; half size: 144), so a work
  group's barriers and idle lanes dominate.
- Two exact rewrites inside that shape were byte-identical but slower (a shared-memory window of `to`; picks as
  parallel reductions, which add barriers) and were not kept.

- The serial reference (one thread per block, no barriers) is 118.7 ms on the same settings: the parallelism is
  essential. The flat passes keep a thread per (block, candidate) - hundreds of thousands of threads - and only
  the picks (a scan of at most a few hundred values) are one thread per block.
- Owner 2026-10-15: this matters beyond the laptop - motion for 50/60p in low-latency modes is estimated as each
  frame arrives (no look-ahead), so the matcher's time adds directly to that frame's latency.

## Design: flat passes, no group barriers

Split the search into simple passes, each one thread per (block, candidate) or per block, with the results in
textures between them. The candidate set, the SAD of each candidate (same samples, same order) and the picks
(strictly smaller value, scanning in order from the preferred first entry) stay exactly as today, so the result
must stay byte-identical; the existing Beta check against the serial search proves it.

1. **Level-2 SADs**: one thread per (block, candidate) for the (2r+1)^2 candidates (r = coarse range) ->
   texture `l2s` (blocks x side, r32f).
   The level-2 SADs are 3x3 samples at 1/32 and two blocks share each level-2 texel (`sad_l2(b / 2, v)`), so
   today every one is computed twice; here once per level-2 texel, read by both blocks (same values).
2. **Level-2 pick**: one thread per block; serial scan from the zero vector -> `v2` per block.
3. **Level-1 SADs**: one thread per (block, search, candidate) for the 13x13 half-texel candidates of each start
   (zero, level 2, D1 global, M2 second global) -> `l1s`.
4. **Level-1 picks and the starts**: one thread per block; the four serial picks, the start list exactly as the
   lead builds it now (skips, centres, E1 previous vector, M2) -> `starts` (centres + count per block).
5. **Half-size SADs**: one thread per (block, list index) up to the list length (<= 321; laid out 18x18 per
   block); each thread derives its candidate from `starts`, applies the repeat rule, and computes the 12x12 SAD
   -> `hs`.
6. **Half-size pick and half-pel**: one thread per block: the serial pick over the list, then the 8 half-pel
   SADs and their pick -> the search output (`vec4(v, sad/144, 1)`), as today.

E4 (static scene keeps the previous vectors) is decided in pass 1/2 and carried through; blocks that keep skip
the later passes' work.

The second sweep (11a E1, 1.1 ms) has the same shape and gets the same treatment once the search is done.

## Expected

The flat passes keep every lane busy and need no barriers; the SAD work (~2.3 ms of today's 7.2) remains, plus
~6 cheap dispatches. Target: block search ~2.5-3 ms on the laptop (matcher ~6 ms instead of ~10), identical
output. On the RTX 3090 the matcher is already ~1-2 ms; the gain matters most for the laptop.

## Tests

- Beta: "parallel block search byte-identical to the serial search" (must stay), the 6d check (rendered vs source
  0.161845), the full Beta suite and matrix.
- Pass timing (live settings) before/after; the real-clip corpus unchanged (41.3% / 0.30%).
- Daytime runs about 5 minutes; the corpus and the gate overnight.

## After this (owner 2026-10-15)

1. De-judder on RGB input (12-bit RGB 4:4:4; DeckLink has no 12-bit YCbCr 4:2:2): a luma-from-RGB pass for the
   matcher and the rendered warp allowed on RGB (it warps the RGB output picture anyway); the source warp's RGB
   planes only if wanted.
2. 12-bit output: make sure VP's own code allows it (no forced 10-bit formats, clamps or depth assumptions in its
   transports and the dither's depth handling); not chasing libplacebo's output stages (owner).

## Result of the flat-pass version (2026-10-15)

Built and measured (laptop 3050 Ti, 4K, live settings): byte-identical to the work-group search and the serial
search (Beta), but the block search took ~13.8 ms in total against 7.2 ms - half-size SADs 6.8 ms (each thread
reads its 144 samples of `from` and `to` from textures; the work-group search shares the block's `from` window
in shared memory), the 1/8 SADs 2.5 ms, and the one-thread-per-block passes (starts, final pick with the
half-pel refinement) ~2 ms each (8,000 threads cannot fill the GPU). Not kept; the work-group search stays.

Next: profile the work-group search's remaining fixed cost in finer steps (the half-pel refinement - 8 threads x
144 bilinear samples while 248 wait - the from-window load, the lead's scans) and target those inside the
existing shape. A non-exact option (partial sums in a different float order, so picks could differ on exact
ties) is measured and listed for the owner, not enabled.
