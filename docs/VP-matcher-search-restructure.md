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
   *Owner decision 2026-10-17 (FINAL): 12-bit output only via VPV for Dolby Vision displays; FP16 output
   parked, no DeckLink output card; non-DV displays stay 10-bit + sigma-delta (docs/VP-roadmap-2026-2031.md).*

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

## Finer profile of the work-group search (2026-10-15, laptop 3050 Ti, 4K, live settings)

Block search 7.2 ms. Half-pel refinement replaced by a constant: 7.95 ms (no saving - not the cost). Every
SAD stage replaced by a constant (1/32, 1/8, half size, half-pel): 5.7 ms left. So about 80% of the search is
not SAD arithmetic: it is the thread-0 scans (1/32 pick over up to 1089 entries, four 1/8 picks over 169, the
candidate-list build, the half-size pick over up to 321) while 255 threads wait, plus the barriers and the
shared-memory footprint (~13 KB per group limits groups per SM).

## Step A - picks folded into the SAD loops (built, byte-identical, not yet timed)

Each thread keeps the best of its own candidates while it computes them (minimum SAD; ties to the serial
pick's first candidate, rank -1, then to the lowest index - a total order, so any reduction order gives the
serial pick exactly); 16 threads reduce 16 partials each, thread 0 the last 16. One extra barrier per pick
(the earlier tree reduction added ~9 and was slower). `l2_sads[1089]` and `half_sads[NHALF]` are no longer
needed (~5.4 KB less shared memory per group). Applied to the 1/32 and half-size picks; Beta suite passes and
the parallel search is byte-identical to the serial one. Timed on mains, interleaved (the laptop GPU drifted up
to 2x between runs): step A 6.65 / 11.4 / 7.15 / 9.98 ms against the old picks 12.8 / 8.18 ms (block search;
step A won 2 of 3 adjacent pairs and has both lowest readings). Kept (owner 2026-10-15: consistently better).
Later timings use an in-process alternating A/B to cancel the drift.

## Next steps, in order (each: Beta byte-identity, then timing on mains; kept only if faster)

1. Done: step A kept.
2. Done (2026-10-15): byte-identical, Beta passes; block search 6.08 ms on a first (cool) run against step A's
   first runs 6.65 / 7.15 ms, matcher 9.1 ms. Kept. The four 1/8 picks the same way (13 threads, one row of 13 each, for all four lists; thread 0
   the 13 row winners).
3. Done (2026-10-15): candidates derived from their index (half_vecs/half_starts removed, ~3.9 KB less shared
   memory); byte-identical, Beta passes; block search 5.96 ms, matcher 9.4 ms. Kept. Was: candidate-list build in parallel: each start's 7x7 window is a fixed-size slot (start k at offset
   1 + 49 x (k - skipped)), so thread t can write its own entry once the skip flags (thread 0, a few
   compares) are known; removes ~300 serial shared writes.
4. Shared-memory diet for occupancy: `half_vecs`/`half_starts` derived from (slot, index) instead of stored;
   `l1_sads_*` as one array; measure groups per SM before/after.
5. Barrier count: merge the from-window load into the 1/32 phase (it does not depend on the picks), and
   drop barriers whose only purpose was the removed scans.
6. Done (2026-10-15): 128 threads per block, byte-identical, Beta passes. Alternating runs (laptop 4K): block
   search 5.36/6.31/5.43 ms against 5.86/5.84/5.96 ms with 256 and 6.02 ms with 64; matcher 8.3-8.8 against
   9.1-9.2 ms. Kept. Was: work-group size (note 2026-10-15: the 1/8 phase uses one thread per candidate, li < 169, and the pick
   reduction assumes 256 partials - both need a stride loop / 8-per-thread reduction first): with the scans gone, try 128 threads (more groups per SM; the half-size candidates
   still fit in 3 strides) - exact by construction, timing decides.
7. Second sweep and quarter-pel (1.1 ms + small): same pick method if their thread-0 scans show up.

Target unchanged: block search ~2.5-3 ms on the laptop, identical output. Non-exact options (different float
summation order) stay measured-and-listed only.

## Added to roadmap item 6 (owner 2026-10-15)

Zero-added-delay de-judder: an experimental optional setting (off by default) that projects frame n forward with
the live n-1->n vectors instead of holding a frame for n+1. Measured and listed (forward prediction is wrong
on motion changes and occlusions, with no next frame to fall back on); the one-frame hold stays the default for
film. Live motion-smoothing options for 50/60 Hz sources to be discussed with the owner.

Owner choice (2026-10-15) for 50/60 Hz live smoothing, also item 6: interpolation for rate mismatches at the same refresh (50p on a 60 Hz output, 25/30p cadences inside 50/60 Hz streams, found by the pulldown detector) and filling dropped or repeated frames in live streams from the vectors. A higher output refresh (100/120 Hz) is not pursued for now.

## Unread pyramid levels in the lowRes outer (2026-10-15)

With lowRes the outer matcher built the full 1/8 and 1/32 pyramids of both frames, but the inner builds its own;
the outer's levels are read only by the picture area (A's 1/8) and, when the outer runs its own frame trust
(not lowResAll) with regional hold, its histograms (both 1/32). Now built only when read: 27 passes instead of 28,
about 0.4 ms GPU less in the live settings (outer 1/8 0.36 -> 0.15 ms, 1/32 0.28 -> 0). Exact: every Beta
output line identical to the previous run (6d 0.161845), Beta passes; main tests 2606/2608 with the two known
unrelated failures (window placement, config read cache) seen in the earlier gates.

## Second sweep: spread texture reads (2026-10-15, not kept)

Every thread of the 32-thread group computed absolute differences into shared memory (11 x 144 floats), one
thread per candidate summed its 144 in the serial order (exact; Beta identical, 6d 0.161845). Slower: second
sweep 2.62 ms against 1.18 ms (6.3 KB of shared memory per 32-thread group cuts the groups per SM; the texture
reads were already well hidden by many resident groups). Reverted.

## Second sweep: two blocks per 32-thread group (2026-10-15, kept)

Threads 0-15 work on one block and 16-31 on its right-hand neighbour, so 22 of 32 threads compute candidate
SADs instead of 11 (16 of 32 for half-pel instead of 8); each block's work, order and picks unchanged.
Byte-identical (Beta passes, 6d 0.161845). One timing on a slow GPU run: second sweep 1.13 ms (earlier
1.18 ms) while the block search read 6.88 ms instead of ~5.4 ms, so likely ~20-25% faster; kept on that
evidence (owner 2026-10-15: take it, confirm the timing on the next run).

## Reusing the last pair's B planes (2026-10-15, kept)

Each pair (A, B) is followed by (B, C): A's half-size luma and pyramid are now the last pair's B planes,
kept uncorrected (the fade correction rewrites B's planes; the uncorrected copy moves to a third set first),
keyed by the frame number the renderer passes (0 = unknown, no reuse; a frame gap forgets it, a resync
destroys the matcher). Where no fade is active the corrected-B pass copies the stored uncorrected value
instead of downsampling the full picture again (the same stored value). Exact: Beta test "reused half-size
planes byte-identical to rebuilt ones" (full size, lowRes, lowResAll; a pan and a still fade with the
correction on in 6 pairs). Saves A's half-size luma and pyramid on both matcher sizes (roughly 0.4-0.7 ms
on the laptop at 4K; the live timing test passes no frame numbers, so it does not show it).

## Merged small passes and the batch timing (2026-10-15)

Exact merges, each direction's work and order unchanged: global motion D1 for both directions in four dispatches
instead of eight (both 1/8 samples taken, then chosen: D3D11 rejects an implicit-LOD sample in a branch); the two
kept searches in one copy; the two searches and two global vectors to the source grid in one dispatch. Beta
passes (parallel vs serial identical, 6d 0.161845, reused planes identical).

Live settings, laptop 4K, one run: matcher 7.85 ms wall-clock (start of day ~10 ms); passes 10.46 ms GPU. On
consecutive frames with plane reuse: 9.71 ms GPU (-0.75 ms), 8.02 ms wall-clock (run noise). Per pass: block
search 5.29 (7.2), second sweep 0.89 (1.18; the paired groups confirmed), global motion 0.38 for all four (~0.77),
grid 0.17 (0.40), keep the searches 0.09 (0.22). 18 small passes (under 0.15 ms each) total 1.88 ms - the floor
a larger GPU will not shrink much; candidates to merge next.

## Block search: comparison-stage profile and two table/row trials (2026-10-15)

Fast rotated timing (VP_TEST_MATCHER_TIMING, medians): skipping the half-size SADs removes about half of the
block search, the 1/32 + 1/8 SADs about a third, all SADs leaves about a quarter (fixed overhead) - the SAD work
now dominates. Each global-motion start adds 49 candidates per block: without M2's second start the block search
was ~20% shorter (a quality-for-speed trade; listed, not enabled). Feature-off totals include knock-on effects
(fewer starts, gate, copies); each pass's own cost is in the per-pass breakdown.

- 1/8 candidates from a per-start 19x19 table of bilinear samples (kept, ed0f67d7): exact, ~3-5% on the block
  search (order-balanced runs on a warm GPU).
- Half-size candidates four to a thread sharing each `to` row (not kept): exact, block search GPU time 4.86 vs
  4.95 ms, but the whole matcher's wall-clock (passes back to back) was 16-21 ms against 7.7 ms in two of three
  runs, with no shader recompiles - the longer per-thread work interacts badly with scheduling. Reverted.
