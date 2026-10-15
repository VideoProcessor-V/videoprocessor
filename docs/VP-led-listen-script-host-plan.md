# LED listen: VP runs the owner's script itself (plan, not built)

> **Retired 2026-10-15:** VP no longer runs the Python script; see docs/VP-led-core-sources.md. Kept as history.

Owner, 2026-10-13: "is there the possibility of directly running the native
python script from inside VP instead of running it externally and
listening. Would be easier for you to track and follow."

## Chosen shape: VP launches and supervises the script

- New `[led_drive.*]` keys: `listen_script` (path to the .py, e.g.
  main_vp.py; empty = today: the owner starts it) and `python` (the
  interpreter; empty = `python` on PATH).
- In `mode: listen` VP starts `python <listen_script>` as a child process
  (no console window, working directory = the script's folder), stops it
  when the mode changes or VP exits (CTRL_BREAK, then terminate after a
  timeout), and restarts it after an exit with a back-off (2, 4, 8 ... 60 s;
  logged).
- The script is unchanged: it screenshots, drives COM1 and sends its UDP
  messages to VP as now. VP still never opens the port.
- Its stdout/stderr go to `logs\led-script.log` (rotated like
  led-listen.csv), next to vp.log and led-listen.csv: one folder holds the
  script's prints, its commands and measurements, and VP's own view.
- Config LED page: the two keys, and the listen monitor shows
  "script running (pid N) / stopped / restarting in N s".

## Not chosen

- Embedding CPython in VP: it would ship a Python runtime with numpy and
  OpenCV, a script crash could take VP down, and the script's screenshot of
  VP's own window becomes awkward.
- VP's C++ copy of the rules is `mode: script`; listen exists to compare it
  with the real script, so it does not replace this.

## Tests

A dummy script (prints, sends a few UDP messages, exits or hangs on
request): start with the mode, output in led-script.log, restart after an
exit with the back-off, stop on mode change and on VP shutdown (no orphan
process), no start when `listen_script` is empty.
