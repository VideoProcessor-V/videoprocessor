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
