# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Tests for resync_schedule.py. Run: python -I -B tools/soak/test_resync_schedule.py (any cwd).

parse() below is an independent, tiny re-implementation of the ResyncInject::LoadSchedule grammar
(src/VP-V/modules/input/signal/ResyncInject.h); keep both in step.
"""

import io
import os
import re
import sys
import unittest
from contextlib import redirect_stdout

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, _HERE)
import resync_schedule as rs  # noqa: E402

NOARG = {"seek", "reset", "inplace", "rebuild", "capture_restart", "rpu_return", "tunnel_handoff",
         "signal_return", "full"}
CAPS = {"gap": 1000, "stall": 60000, "rpu_loss": 100000, "signal_loss": 600000, "overflow": 1000,
        "minor": 1000}


def parse(text):
    """Returns (seed or None, [(clock, time, kind, arg)]). Raises ValueError("line N: ...")."""
    seed = None
    entries = []
    last = {"s": None, "q": None}
    for no, raw in enumerate(text.split("\n"), 1):
        line = raw.rstrip("\r")
        # a '#' starts a comment except the "#<digits>" time token directly after "at"
        toks = []
        for t in re.split(r"[ \t]+", line.strip()) if line.strip() else []:
            if t.startswith("#") and not (len(toks) == 1 and toks[0] == "at" and re.match(r"^#[0-9]", t)):
                break
            toks.append(t)
        if not toks:
            continue
        if toks[0] == "seed":
            if seed is not None or entries or len(toks) != 2 or not re.match(r"^[0-9]{1,20}$", toks[1]) \
                    or int(toks[1]) >= 1 << 64:
                raise ValueError("line %d: bad seed" % no)
            seed = int(toks[1])
            continue
        if toks[0] != "at" or len(toks) < 3:
            raise ValueError("line %d: expected at" % no)
        if toks[1] == "seq":
            clock, k = "q", 3
            if len(toks) < 4 or not re.match(r"^[0-9]{1,18}$", toks[2]):
                raise ValueError("line %d: bad seq" % no)
            t = int(toks[2])
        elif toks[1].startswith("#"):
            clock, k = "q", 2
            if not re.match(r"^#[0-9]{1,18}$", toks[1]):
                raise ValueError("line %d: bad seq" % no)
            t = int(toks[1][1:])
        else:
            clock, k = "s", 2
            if not re.match(r"^[0-9]{1,9}(\.[0-9]{1,6})?s$", toks[1]):
                raise ValueError("line %d: bad time" % no)
            t = float(toks[1][:-1])
        if last[clock] is not None and t < last[clock]:
            raise ValueError("line %d: time goes backwards" % no)
        last[clock] = t
        kind, rest = toks[k], toks[k + 1:]
        if len(rest) > 1:
            raise ValueError("line %d: too many arguments" % no)
        arg = rest[0] if rest else None
        if kind in NOARG:
            if arg is not None:
                raise ValueError("line %d: takes no argument" % no)
        elif kind == "dv_switch":
            if arg not in ("tunnel", "lldv", "hdr10", "sdr"):
                raise ValueError("line %d: bad dv target" % no)
        elif kind == "rate":
            if arg is None or not re.match(r"^-?[0-9]{1,9}$", arg) or abs(int(arg)) > 1000000:
                raise ValueError("line %d: bad rate" % no)
        elif kind in CAPS:
            if arg is None:
                if kind != "minor":
                    raise ValueError("line %d: needs a number" % no)
            elif not re.match(r"^[0-9]{1,9}$", arg) or not 1 <= int(arg) <= CAPS[kind]:
                raise ValueError("line %d: out of range" % no)
        else:
            raise ValueError("line %d: unknown kind" % no)
        entries.append((clock, t, kind, arg))
        if len(entries) > rs.MAX_ENTRIES:
            raise ValueError("too many entries")
    return seed, entries


class ScheduleTests(unittest.TestCase):
    def gen(self, **kw):
        a = dict(seed=11, hours=2.0, mean_interval_s=30.0, scenarios=None, dv=True)
        a.update(kw)
        return rs.generate(**a)

    def test_deterministic_per_seed(self):
        self.assertEqual(self.gen(), self.gen())
        self.assertNotEqual(self.gen(seed=11), self.gen(seed=12))

    def test_header_ordering_and_caps(self):
        text = self.gen()
        self.assertEqual(text.split("\n")[1], "seed 11")
        seed, entries = parse(text)
        self.assertEqual(seed, 11)
        self.assertGreater(len(entries), 50)
        times = [e[1] for e in entries]
        self.assertEqual(times, sorted(times))
        self.assertLess(times[-1], 2 * 3600 + 130 + 6)   # paired returns may trail the end slightly

    def test_pairs_and_dv_rotation(self):
        text = self.gen(hours=6.0, seed=3)
        _s, entries = parse(text)
        kinds = [e[2] for e in entries]
        self.assertGreater(kinds.count("rpu_loss"), 3)
        self.assertEqual(kinds.count("rpu_loss"), kinds.count("rpu_return"))
        self.assertEqual(kinds.count("signal_loss"), kinds.count("signal_return"))
        self.assertEqual(kinds.count("rate") % 2, 0)
        rates = [e for e in entries if e[2] == "rate"]
        for e in rates:
            if e[3] != "0":
                self.assertTrue(300 <= abs(int(e[3])) <= 3000)
        self.assertEqual(rates[-1][3], "0")
        targets = [e[3] for e in entries if e[2] == "dv_switch"]
        self.assertGreater(len(targets), 8)
        order = ["tunnel", "lldv", "hdr10", "sdr"]
        for a, b in zip(targets, targets[1:]):
            self.assertEqual(order[(order.index(a) + 1) % 4], b)

    def test_storms_are_short_and_dense(self):
        text = self.gen(scenarios=["storm"], dv=False, hours=3.0, mean_interval_s=120.0)
        _s, entries = parse(text)
        ts = [e[1] for e in entries]
        self.assertTrue(all(e[2] in ("seek", "reset", "inplace", "rebuild", "full", "minor", "gap") for e in entries))
        # every storm: 4-6 events within 8 s; storms are >= 30 s apart, so cluster by gaps < 9 s
        clusters, cur = [], [ts[0]]
        for t in ts[1:]:
            if t - cur[-1] < 9.0:
                cur.append(t)
            else:
                clusters.append(cur)
                cur = [t]
        clusters.append(cur)
        for c in clusters:
            self.assertTrue(4 <= len(c) <= 12, len(c))   # neighbouring storms may merge, never split below 4
            if len(c) <= 6:
                self.assertLessEqual(c[-1] - c[0], 8.0)

    def test_default_has_no_dv_kinds(self):
        _s, entries = parse(self.gen(dv=False, hours=4.0))
        self.assertTrue({"dv_switch", "rpu_loss", "rpu_return", "tunnel_handoff"}.isdisjoint(e[2] for e in entries))

    def test_scenarios_filter_and_errors(self):
        _s, entries = parse(self.gen(scenarios=["seek", "gap"], dv=False))
        self.assertEqual({e[2] for e in entries}, {"seek", "gap"})
        with self.assertRaises(ValueError):
            self.gen(scenarios=["nope"])
        with self.assertRaises(ValueError):
            self.gen(hours=0)
        with self.assertRaises(ValueError):
            rs.generate(1, 5000.0, 1.0, ["seek"])   # > 100000 entries

    def test_cli(self):
        buf = io.StringIO()
        with redirect_stdout(buf):
            code = rs.main(["--seed", "5", "--hours", "0.5", "--dv"])
        self.assertEqual(code, 0)
        self.assertEqual(buf.getvalue(), rs.generate(5, 0.5, 90.0, None, True))
        self.assertEqual(rs.main(["--seed", "5", "--hours", "-1"]), 2)

    def test_reference_parser_grammar(self):
        seed, e = parse("# c\n\nseed 9\nat 1.5s seek # trailing\nat #10 gap 3\nat seq 11 dv_switch lldv\nat 2s minor\n")
        self.assertEqual(seed, 9)
        self.assertEqual([x[2] for x in e], ["seek", "gap", "dv_switch", "minor"])
        for bad in ["at 1s seek 3", "at 1s gap", "at 1s gap 1001", "at 2s seek\nat 1s seek", "at 1s dv_switch x",
                    "at 1s rpu_loss 100001", "at 1s signal_loss 600001", "at 1s overflow 1001", "at 1 seek",
                    "seek", "at #5 seek\nat #4 seek", "at 1s rate 1000001", "seed 1\nseed 2"]:
            with self.assertRaises(ValueError, msg=bad):
                parse(bad)


if __name__ == "__main__":
    unittest.main(verbosity=1)
