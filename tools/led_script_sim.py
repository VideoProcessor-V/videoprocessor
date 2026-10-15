"""Runs the owner's LED script (main_vp.py) unchanged on a recorded brightness
timeline, on a virtual clock, for the script-vs-VP correlation
(owner 2026-10-15; LedScriptCorrelationTests).

The script's imports are replaced before it runs:
  time        a virtual clock: sleep() advances it, nothing waits
  pyautogui   screenshot() returns the timeline's frame at the clock and
              costs --shot seconds (a real screenshot + resize)
  numpy, cv2  just enough for the script's measurement on that frame
  subprocess  call('echo :lpw1NNNcr > nul') records the command; costs --cmd
  socket      VP's side of the listen socket: answers VP1 REQ from the
              timeline (the python_feed), records VP1 TICK
The script runs with VP_LED_RELAY=1 and, unless --no-feed, VP_LED_FEED=1.

timeline CSV (header): t_ms,apl,max,hl[,...]  - one row per frame, the
frame shown from t_ms until the next row.
Writes <out>_ticks.csv (t_ms,frame,mean,max,hl,tp,tc) and
<out>_commands.csv (t_ms,kind,value).
"""
import argparse
import bisect
import csv
import io
import os
import re
import sys
import types


class Finished(Exception):
    pass


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--script", required=True)
    parser.add_argument("--timeline", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--shot", type=float, default=0.15, help="seconds a real screenshot + resize takes")
    parser.add_argument("--cmd", type=float, default=0.03, help="seconds a cmd echo to the port takes")
    parser.add_argument("--no-feed", action="store_true", help="screenshots every loop (the original)")
    args = parser.parse_args()

    times, frames = [], []
    with open(args.timeline, newline="") as f:
        for row in csv.DictReader(f):
            times.append(float(row["t_ms"]) / 1000.0)
            frames.append((float(row["apl"]), float(row["max"]), int(row["hl"])))
    end = times[-1]
    clock = [0.0]
    fed_frame = [-1]

    def frame_index():
        return max(0, bisect.bisect_right(times, clock[0]) - 1)

    def advance(seconds):
        clock[0] += max(0.0, seconds)
        if clock[0] > end:
            raise Finished()

    # time
    fake_time = types.ModuleType("time")
    fake_time.time = lambda: clock[0]
    fake_time.monotonic = fake_time.perf_counter = fake_time.time
    fake_time.sleep = advance

    # the screenshot and the measurement on it
    class Picture:
        def __init__(self, index):
            self.index = index
            self.apl, self.maximum, _ = frames[index]

        def __getitem__(self, _):
            return self

    fake_gui = types.ModuleType("pyautogui")

    def screenshot():
        picture = Picture(frame_index())
        advance(args.shot)
        return picture

    fake_gui.screenshot = screenshot
    fake_np = types.ModuleType("numpy")
    fake_np.array = lambda picture: picture
    fake_np.mean = lambda picture: picture.apl
    fake_cv2 = types.ModuleType("cv2")
    fake_cv2.COLOR_BGR2GRAY = 6
    fake_cv2.INTER_AREA = 3
    fake_cv2.cvtColor = lambda picture, code: picture
    fake_cv2.resize = lambda picture, dim, interpolation=None: picture
    fake_cv2.minMaxLoc = lambda picture: (0.0, float(int(picture.maximum)), (0, 0), (0, 0))

    # the port
    commands = []
    pattern = re.compile(r":(lpw1|cntr)(\d+)cr")
    fake_sub = types.ModuleType("subprocess")

    def call(command, shell=False):
        text = command if isinstance(command, str) else " ".join(command)
        match = pattern.search(text)
        # Only what goes to the port (the script also echoes each LPW1 to its
        # console, which never reaches the projector).
        if match and ">" in text:
            commands.append((clock[0], match.group(1).upper(), int(match.group(2))))
        advance(args.cmd)
        return 0

    fake_sub.call = call
    fake_sub.Popen = lambda *a, **k: None

    # VP's end of the listen socket
    ticks = []
    fake_socket = types.ModuleType("socket")
    fake_socket.AF_INET, fake_socket.SOCK_DGRAM = 2, 2

    class Timeout(OSError):
        pass

    fake_socket.timeout = Timeout

    class Socket:
        def __init__(self, *a, **k):
            self.replies = []

        def settimeout(self, value):
            pass

        def sendto(self, data, address):
            words = data.decode("ascii").split()
            fields = dict(w.split("=", 1) for w in words[2:] if "=" in w)
            if words[1] == "REQ":
                index = frame_index()
                fed_frame[0] = index
                apl, maximum, hl = frames[index]
                self.replies.append(("VPS1 id=%s mean=%d max=%d hl=%d seq=%d" % (
                    words[2], max(0, int(apl)), int(maximum), hl, index)).encode("ascii"))
            elif words[1] == "TICK":
                ticks.append((clock[0], fed_frame[0], int(fields["mean"]), int(fields["max"]), int(fields["hl"]),
                              int(fields["tp"]), int(fields["tc"])))
                fed_frame[0] = -1
            return len(data)

        def recvfrom(self, size):
            if not self.replies:
                raise Timeout()
            return self.replies.pop(0), ("127.0.0.1", 47811)

    fake_socket.socket = Socket

    # Screenshot loops: the frame the picture came from (no REQ was answered).
    original_screenshot = fake_gui.screenshot

    def screenshot_noting():
        fed_frame[0] = frame_index()
        return original_screenshot()

    fake_gui.screenshot = screenshot_noting

    sys.modules.update({"time": fake_time, "pyautogui": fake_gui, "numpy": fake_np, "cv2": fake_cv2,
                        "subprocess": fake_sub, "socket": fake_socket})
    os.environ["VP_LED_RELAY"] = "1"
    if args.no_feed:
        os.environ.pop("VP_LED_FEED", None)
    else:
        os.environ["VP_LED_FEED"] = "1"
    with open(args.script) as f:
        code = compile(f.read(), args.script, "exec")
    saved = sys.stdout
    sys.stdout = io.StringIO()   # the script prints every loop
    try:
        exec(code, {"__name__": "__main__", "__file__": args.script})
    except Finished:
        pass
    finally:
        sys.stdout = saved

    with open(args.out + "_ticks.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["t_ms", "frame", "mean", "max", "hl", "tp", "tc"])
        for t, frame, mean, maximum, hl, tp, tc in ticks:
            w.writerow(["%.1f" % (t * 1000.0), frame, mean, maximum, hl, tp, tc])
    with open(args.out + "_commands.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["t_ms", "kind", "value"])
        for t, kind, value in commands:
            w.writerow(["%.1f" % (t * 1000.0), kind, value])
    print("ticks=%d commands=%d end=%.1fs" % (len(ticks), len(commands), end))


if __name__ == "__main__":
    main()
