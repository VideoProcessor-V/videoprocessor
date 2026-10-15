# LED Python source: VP feeds the script its measurement (python_feed)

Owner 2026-10-15: the Python script (command_source: python) takes the
brightness values it needs from VP instead of a screenshot - more
efficient, and VP's measurement path is exercised from the start.

Owner decisions (2026-10-15):
- The script gets the **frame on screen**, never an upcoming one: faithful
  to the original, which screenshots what is displayed. The `lookahead`
  switch does not apply to the Python source.
- VP's own comparison (the listen log's TICK rows and accuracy) uses that
  same frame: when VP fed the loop, exactly the frame it fed, so only the
  rules can differ.
- The loop is padded to the script's own screenshot time, so its pace and
  ramps stay as tuned.

## What the script measured

`main_vp.py` per loop: `pyautogui.screenshot()`, BGR2GRAY on the RGB array
(effective weights 0.114 R / 0.587 G / 0.299 B), INTER_AREA resize to
854x480, `max_val` over the whole frame (highlight >= 215), mean over rows
57..423 -> `mean2 = int(mean)`. Everything after that uses only `mean2`,
`max_val` and `highlightdetect`.

## Built

- `[led_drive.*] python_feed` (default on); Config: a check box on the
  Python script card.
- VP starts the script with `VP_LED_FEED=1` (a change restarts it).
- The script sends `VP1 REQ <n>` on the listen socket (47811); VP answers
  only the script it runs (the relay's sender check) with
  `VPS1 id=<n> mean=<apl> max=<brightest block> hl=<0|1> seq=<frame>` -
  VP's exact measurement of the latest rendered frame, the integers the
  internal script copy uses (`apl_weights`, `script_highlight_code`) - or
  `VPS1 id=<n> none` before any frame.
- Script addendum (marked; the rest of the script unchanged): the first
  three loops take real screenshots to time them; then each loop asks VP,
  waits up to 0.15 s, uses VP's values and sleeps out the rest of the
  screenshot time. No answer: that loop takes its screenshot.
- Listen log: the 10 s summary line counts `fed`, `fed_none`,
  `fed_refused`.

The script is a seed (installed only when absent): an existing deployed
`scripts\main_vp.py` must be replaced by hand to use the feed; the old
copy simply never asks, and VP behaves as before.

## Tests

`FeedAnswersTheScriptWithTheFrameOnScreen`: a stand-in script told
VP_LED_FEED=1 gets `none` before any frame, then the frame on screen (not
the queued brighter frames), the next frame after it changes, with
lookahead off too; a stranger's request is refused.
