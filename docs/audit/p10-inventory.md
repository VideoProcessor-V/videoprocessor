# M2 P10-1 inventory (capture upload / unpack / early upload + queue, cadence, file frames)

Cut: branch `m2/p10`, base ed1eca65 (integ 40356bed + the P0b + P2 train). Bill = a591ad3b.
LVR = `src/VideoProcessor-Lib/vprenderer/LibplaceboVideoRenderer.cpp` (35,226 lines at the cut; doc 59's numbers were
taken at 37,983 lines, so every range below is re-taken). P5b and P6 have NOT run: their parts are marked "P5b"/"P6".

Threads: CAP = capture callback thread (OnVideoFrame), FILE = DirectShow file thread (OnTimedVideoFrame),
CONV = conversion worker (ConversionLoop), RND = render thread (renderMutex), LOOP = render loop (queue side, takes
m_queueMutex), UI = settings / GUI thread, ANY = whoever holds the lock named.

| # | What | Where (cut) | Thread (lock) | Target / step | Notes |
|---|---|---|---|---|---|
| a | capture/early/unpack members, `EarlyRequested/Active`, `SyncEarlyUpload`, `EarlyTake`, `EarlyTexture`, `BookUploadTimers`, `rawBuffersPeak`, `rgbGpuFailed`, `rgbUnpack`, `dvGpuWanted/Failed`, `dvUnpack`, `dvUnrecoverableLogged` | 4889-5064 (members interleaved with Stage 12 members up to 5070), `rgbIngestLuma` 5110, `rgbGpuLogged` 5116 | atomics: CONV reads, UI writes; the rest RND | P10-4 | `dejudderRgbLuma/Bt2020/Scratch` 5114-5116 stay P5a's |
| b | `UnpackRgbPayload`, `dvFallbackPixels`, `UnpackDolbyVisionOnCpu`, `UnpackDolbyVisionPayload`, `UnpackRawPayload` | 5434-5574 | RND (UnpackDolbyVisionOnCpu also takes `formatterMutex`) | P10-4 | `UnpackRawPayload(..., sessionFallback=false)` is also called by P5a's de-judder next/ahead upload |
| c | RGB luma shared with the matcher (`rgbIngestLuma`; `dejudderRgbLuma = rgbIngestLuma` at 18871) | 5110, 18855-18872 | RND | P10-4 | the alias line belongs to P5a/P5b |
| d | GPU release of the unpackers + early upload close log | 8130-8148 | RND (Retire path, renderMutex) | P10-2: `OnLifecycle(GpuReleased)` | runs ~35 lines later than today (after the de-judder / rebase / calibration releases, still before `pl_d3d11_destroy`): see P10-2 commit |
| e | upload-route init log | 11465-11490 | RND | P10-4 (`CollectStatus` + 1 Bill line) | reads `EarlyActive()` |
| f | live capture atomics (capture_path, native_rgb_ingest, dv_tunnel_gpu_unpack, capture_upload, efficiency_fixes) | 11136-11141 (initialise), 12186-12188 + 12200-12202 (live apply) | UI / RND, release stores | P10-2: `OnLifecycle(SettingsApplied)` (raised by `UpdatePipelineSlots`, right after `activeSettings = settings` at both sites) | the DV-formatter `dv_tunnel_fixes` block 12189-12199 is not P10 (stays) |
| g | upload branches: native-RGB / tunnel SDR repack, `rgbRawUpload`, raw / DV raw / reuse / reuseNext / CPU fallback, unrecoverable DV black, ingress status texts, `rgbRawUpload` repr, `source_range` | 17775-17850, 17978-18050, 18795-19110 | RND (renderMutex) | P10-4 `TryUpload` | interleaved with Bill's two-plane upload and with P5a/P5b's `UpdateDejudderEstimate` / `TakeNextPlanes` / `vpTimelineActive` (the forwarder doc 59 meant P5b to have moved: P5b has not run) |
| h | ConversionLoop routes: `rgbRaw`, `makeRaw` (held-buffer peak log, early hand-off), `dvRaw` + DV `AnalyzeFrame`, native RGB empty payload, `captureEncoding` | 32506-32667 | CONV (`formatterMutex` inside; never `m_queueMutex`) | P10-3 `OnConvertedFrame` | `captureEncoding` (32669) is not in R18's list: stays on the payload |
| i | early hand-off after the notify | 32741-32748 | CONV, no lock | P10-3 `AfterPublish` | |
| j | OnVideoFrame VP lines: hardware arrival, stale rule, timestamp breaks, pulldown detection + film clock, lock-change wake, gap marks | 27804-28140 (minus Q1's resync cursor 27954-27956, 28019, 28306-28310) | CAP (no lock; `m_stateMutex` for the state copy, `m_queueMutex` only for the generation read) | P10-5 `DecideIngress` | |
| k | EnqueueFrame overflow successor + VP admission lines | 28144-28330 | CAP / FILE (m_queueMutex) | P10-5 | Q1's facts block 28306-28310 stays |
| l | `OnTimedVideoFrame`, `FileHeadTicksUntilDueLocked` | 28336-28470 | FILE; LOOP for FileHead (m_queueMutex) | P10-7 | P2 already moved the boundary (`vpv.timed_file_frames.v1`, `RendererAbiBridge`) |
| m | delay budget: `SetQueueDelayBudget`, `CurrentQueueHold`, `UpdateQueueDelayHold`, `HoldTicksUntilDueLocked`, `WaitForDequeueLocked` VP lines | 28474-28635 | UI (SetQueueDelayBudget); LOOP (m_queueMutex) | P10-6 `ResolveQueueTarget` | |
| n | `UpdateFrameRateAndPPM` VP lines | 28636-28680 | CAP | P10-6 `ResolveRate` | |
| o | dequeue: repeat arm, file pairing, frame times | 33480-33530 (gap/discontinuity/format events are Q1/P5a's R9 calls) | LOOP | P10-6 (repeat arm: P6) | |
| p | queue frame rate, flush / held shed | 33770-33800, 34930-34950 | LOOP | P10-6 | |
| q | `PrefillTargetLocked`, `LockedFilmQueueTarget` VP lines | 35018-35080 | ANY holding m_queueMutex | P10-6 `ResolveQueueTarget` (R19); P6 credit | reads `m_impl->renderAheadCreditFrames` / `motionAheadDepth` outside the lifetime lock (fix C, P10-8) |
| r | header VP members (payload R18 fields 210-235, file fields 307-314, queue-delay members) | LibplaceboVideoRenderer.h 200-530 | - | R18 / R20 | **LVRH zone is held by M2 for P2 until it lands**: no header edit in this run |
| s | `IRenderer::OnTimedVideoFrame` & co. | done by P2 (`vpv.timed_file_frames.v1`, `VpvHostCalls.h`, `RendererAbiBridge.h:160`) | FILE | P10-7 (implementation behind the interface only) | |

## EngageFallback / SyncFallback calls in the P10 ranges (owner rule: count unchanged after the moves)

| Call | Line | Id / catalog entry | New owner |
|---|---|---|---|
| SyncFallback x6 | 4946, 4948, 4951, 4988, 4995, 5002 | `FallbackCatalog::EarlyUpload` / `EarlyUploadRenderUploads` (early-upload guard, open failure) | P10-4 (SyncEarlyUpload) |
| SyncFallback | 5029 | `earlyRenderUploads.Note` (render uploads share) | P10-4 (EarlyTexture) |
| EngageFallback | 5469 | "native RGB GPU unpack" | P10-4 |
| EngageFallback (via DvGpuUnpackSession::Finish) | 5520 | "Dolby Vision GPU unpack" | P10-4 |
| EngageFallback | 5569 | "capture GPU unpack" | P10-4 |
| ReleaseFallback | 8134 | "Dolby Vision GPU unpack" (new device may unpack again) | P10-2 (OnLifecycle GpuReleased) |
| Engage/ReleaseFallback | 28539-28552 | "delay budget", "look-ahead" | P10-6 |
| (none) | 28345-28353 | file timing refused: counted (`m_fileTimingRejected`, dropped frames) and logged at powers of two; NOT a FallbackCatalog entry today | P10-7 records it; making it visible is a new behaviour (owner decision, not a move) |

Outside P10 but in the touched ranges (stay where they are): "Dolby Vision metadata" 17746/17756, "Dolby Vision source
range" / "scene metadata" 19169-19175 (input.signal, P13), FrameAnalysis 32771/32773 (WP-23), "display repeat" 34135 (P6).

## Original pathway check (doc 59 section 5.3)

`src/VP-V/core/OriginalPathway.h` has rows for capture_path (standard), capture_upload (render), native_rgb_ingest (off),
dv_tunnel_gpu_unpack (off), dv_tunnel_fixes (off), pulldown_fixes (off), efficiency_fixes (off). There is **no row for
pulldown detection itself** (`dejudderPulldown`, read into `m_pulldownDetection` at LVR 33776). Original therefore relies
on its default (off, VpvRendererFields.h:115). Bill-identical today; listed for the owner/coordinator.

## Source-text pins on moved LVR text

`grep` over src/VP-V/**/tests and src/VideoProcessor-Test for the moved identifiers (makeRaw, rgbRaw, SyncEarlyUpload,
Unpack*Payload, EarlyTexture, BookUploadTimers, captureGpuWanted, RunUpload) and for tests that read
LibplaceboVideoRenderer.cpp: no test pins the capture route text (EarlyUploadHandoffTests calls the class, not the LVR
text). No pin relocation, no waiver.
