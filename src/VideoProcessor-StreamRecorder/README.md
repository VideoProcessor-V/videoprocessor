# VP Stream Recorder

`VideoProcessorStreamRecorder.exe` records what a DeckLink input actually
delivers, so the de-judder's handling of real streamed input (bit-exact
repeats, pulldown cadences such as 24p inside 30 Hz, missed frames, timing
jitter and drift, HDMI resyncs) can be inspected and replayed in the tests.
It does not process or show video.

Close VP first: only one program can use the card. Then, from a command
prompt in the folder that holds the exe:

```text
VideoProcessorStreamRecorder.exe --list
VideoProcessorStreamRecorder.exe --device 0 --seconds 120
```

While it records, one line a second shows the frame rate, the bit-exact
repeats in that second, missed frames, resyncs and the last 20 frames:
`.` a new picture, `R` an exact repeat of the previous frame, `G` the first
frame after missed frames. 24p carried in 30 Hz shows one `R` in every five
frames (`....R....R`); 24p in 60 Hz shows the 2:3 pattern (`.R.RR.R.RR`).

Options: `--input hdmi|sdi` (default hdmi), `--out DIR` (default
`stream-YYYYMMDD-HHMMSS` next to where it is run), `--thumb W` (thumbnail
width, default 240), `--raw N` (also save the first N full frames; about
20 MB each at 4K, for exact reproduction only).

Output folder:

- `frames.csv`: one line per frame: the hardware arrival time and duration
  (100 ns units), the stream time, the callback time (QPC), whole periods since
  the previous frame (1 = contiguous), frames buffered in the driver, the frame
  flags, no-input, a 64-bit hash of every pixel byte, exact-repeat and the
  callback's own duration.
- `thumbs.y8`: an 8-bit luma thumbnail per frame, in frame order (size in
  `events.txt`).
- `events.txt`: mode and format changes (restarted as VP does), the
  thumbnail size and raw frames saved.

The thumbnails are small, but they are still pictures of the content: keep
recordings local and do not commit them.

## Replaying a recording through VP

Copy the recording folder to the development machine and run the renderer
harness's replay test with it (hardware GPU):

```text
set VP_TEST_GPU=hardware
set VP_TEST_STREAM_DIR=C:\path\to\stream-YYYYMMDD-HHMMSS
vstest.console.exe x64\Release\VideoProcessor-Test.dll /Tests:RecordedStreamReplaysThroughTheRenderer /logger:console;verbosity=detailed
```

It replays the recorded timing, missed frames, jitter and exact-repeat cadence
through the real renderer with pulldown detection on, and prints the pulldown
lock changes, counted gaps and motion invalidations. Without
VP_TEST_STREAM_DIR it replays a synthetic 24p-in-29.97 Hz recording with a
two-frame gap (a self-check of the replay).
