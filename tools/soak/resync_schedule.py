# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Deterministic fault-injection schedule generator for a resync soak (stdlib only).

python -I tools/soak/resync_schedule.py --seed N --hours H [--mean-interval-s 90]
       [--scenarios seek,reset,...] [--dv] > schedule.txt

The output is the text format read by ResyncInject::LoadSchedule (src/VP-V/modules/input/signal/
ResyncInject.h): a `seed N` header and `at <seconds>s <kind> [arg]` lines in non-decreasing time.
The same seed + arguments always give the same bytes (own SplitMix64 generator, no clock, no
random module). Exit 2 on a usage error or when the schedule would exceed 100000 entries.
"""

import argparse
import math
import sys

MAX_ENTRIES = 100000
MASK = (1 << 64) - 1

# Scenario names. The default set has no Dolby Vision scenarios; --dv adds them.
BASE_SCENARIOS = ["seek", "reset", "inplace", "rebuild", "gap", "rate", "stall", "capture_restart",
                  "signal_loss", "storm", "overflow", "minor", "full"]
DV_SCENARIOS = ["dv_switch", "rpu_loss", "tunnel_handoff"]
ALL_SCENARIOS = BASE_SCENARIOS + DV_SCENARIOS
STORM_KINDS = ["seek", "reset", "inplace", "rebuild", "full", "minor", "gap 2"]
DV_TARGETS = ["tunnel", "lldv", "hdr10", "sdr"]


class Rng(object):
    """SplitMix64: identical on every platform and Python version."""

    def __init__(self, seed):
        self.s = seed & MASK

    def u64(self):
        self.s = (self.s + 0x9E3779B97F4A7C15) & MASK
        z = self.s
        z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & MASK
        z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & MASK
        return z ^ (z >> 31)

    def unit(self):
        return (self.u64() >> 11) / float(1 << 53)

    def randint(self, lo, hi):
        return lo + self.u64() % (hi - lo + 1)

    def expo_ms(self, mean_ms):
        # exponential-ish: inverse-CDF, clamped to [0.25, 4] x mean so a run has neither bursts of
        # zero gaps nor a long silence
        g = -math.log(1.0 - self.unit()) * mean_ms
        return int(min(max(g, 0.25 * mean_ms), 4.0 * mean_ms))


def fmt_time(ms):
    return "%d.%03ds" % (ms // 1000, ms % 1000)


def generate(seed, hours, mean_interval_s=90.0, scenarios=None, dv=False):
    """Returns the schedule text. Raises ValueError on bad arguments or too many entries."""
    if seed < 0 or seed > MASK:
        raise ValueError("seed must be 0..2^64-1")
    if hours <= 0:
        raise ValueError("hours must be positive")
    if mean_interval_s < 1.0:
        raise ValueError("mean interval must be at least 1 s")
    names = list(scenarios) if scenarios else list(BASE_SCENARIOS)
    if dv:
        for n in DV_SCENARIOS:
            if n not in names:
                names.append(n)
    for n in names:
        if n not in ALL_SCENARIOS:
            raise ValueError("unknown scenario %r (known: %s)" % (n, ",".join(ALL_SCENARIOS)))
    if not names:
        raise ValueError("no scenarios")
    rng = Rng(seed)
    end_ms = int(hours * 3600.0 * 1000.0)
    mean_ms = mean_interval_s * 1000.0
    events = []          # (t_ms, order, text)
    order = [0]
    dv_next = [rng.randint(0, 3)]

    def add(t, text):
        events.append((t, order[0], text))
        order[0] += 1

    t = 0
    while True:
        t += rng.expo_ms(mean_ms)
        if t >= end_ms:
            break
        sc = names[rng.randint(0, len(names) - 1)]
        if sc == "gap":
            add(t, "gap %d" % rng.randint(1, 30))
        elif sc == "rate":
            # a step that returns to 0 later (a later step may overlap; the last "rate 0" restores 1.0)
            ppm = rng.randint(300, 3000) * (1 if rng.randint(0, 1) else -1)
            add(t, "rate %d" % ppm)
            add(t + rng.randint(20, 120) * 1000, "rate 0")
        elif sc == "stall":
            add(t, "stall %d" % rng.randint(50, 2000))
        elif sc == "dv_switch":
            add(t, "dv_switch %s" % DV_TARGETS[dv_next[0] % 4])
            dv_next[0] += 1
        elif sc == "rpu_loss":
            add(t, "rpu_loss %d" % rng.randint(30, 600))
            add(t + rng.randint(1, 20) * 1000, "rpu_return")
        elif sc == "signal_loss":
            ms = rng.randint(200, 5000)
            add(t, "signal_loss %d" % ms)
            add(t + ms + rng.randint(0, 500), "signal_return")
        elif sc == "storm":
            n = rng.randint(4, 6)
            offs = sorted(rng.randint(0, 8000) for _ in range(n))
            for off in offs:
                add(t + off, STORM_KINDS[rng.randint(0, len(STORM_KINDS) - 1)])
        elif sc == "overflow":
            add(t, "overflow %d" % rng.randint(10, 500))
        elif sc == "minor":
            if rng.randint(0, 1):
                add(t, "minor %d" % rng.randint(1, 5))
            else:
                add(t, "minor")
        elif sc == "tunnel_handoff":
            add(t, "tunnel_handoff")
        else:
            add(t, sc)   # seek reset inplace rebuild capture_restart full
        if len(events) > MAX_ENTRIES:
            raise ValueError("schedule would exceed %d entries; shorten --hours or raise --mean-interval-s"
                             % MAX_ENTRIES)
    events.sort(key=lambda e: (e[0], e[1]))
    if len(events) > MAX_ENTRIES:
        raise ValueError("schedule would exceed %d entries" % MAX_ENTRIES)
    out = ["# generated by tools/soak/resync_schedule.py: hours=%s mean_interval_s=%s scenarios=%s"
           % (("%g" % hours), ("%g" % mean_interval_s), ",".join(names)),
           "seed %d" % seed]
    for t_ms, _o, text in events:
        out.append("at %s %s" % (fmt_time(t_ms), text))
    return "\n".join(out) + "\n"


def main(argv=None):
    ap = argparse.ArgumentParser(description="resync injection schedule generator")
    ap.add_argument("--seed", type=int, required=True)
    ap.add_argument("--hours", type=float, required=True)
    ap.add_argument("--mean-interval-s", type=float, default=90.0)
    ap.add_argument("--scenarios", help="comma-separated; default: %s" % ",".join(BASE_SCENARIOS))
    ap.add_argument("--dv", action="store_true", help="also add " + ",".join(DV_SCENARIOS))
    a = ap.parse_args(argv)
    sc = [s.strip() for s in a.scenarios.split(",") if s.strip()] if a.scenarios else None
    try:
        text = generate(a.seed, a.hours, a.mean_interval_s, sc, a.dv)
    except ValueError as ex:
        print("resync_schedule: %s" % ex, file=sys.stderr)
        return 2
    sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
