# Vector delivery (`motion.vector_delivery`) - folded stub

Unit: **motion** | side: processing | order: 130 | status: **partial** | hook point: host

Consolidation W0-1 (2026-11-10) retired this folder's code: `DejudderPolicy.h` is now in `../consumers/`, `DejudderWarp.h` (and `DejudderWarp.cpp`) in `../estimation/matcher/`, `MotionSegment.h` in `../estimation/`. The full former README (API, data flow, threading, config keys, fallbacks, tests, open items) is folded into `../consumers/README.md`, section "Folded in: the former motion/vector_delivery folder". This folder stays only because the slot `motion.vector_delivery` is still in `docs/design/slots.json` / `PipelineSlots.h` (retiring the slot itself is a separate decision), and it holds one forwarder, `MotionSegment.h`, so `LibplaceboVideoRenderer.cpp:13` takes no hunk; the forwarder is removed at the next Bill-merge window (listed in `../estimation/MANIFEST.json` `forwarding_headers`).

The authoritative file list is `live_files` in [MANIFEST.json](MANIFEST.json).
