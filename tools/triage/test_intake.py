# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""python -I -B tools/triage/test_intake.py   (any cwd)"""
import contextlib
import io
import json
import os
import sys
import tempfile
import unittest
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import intake  # noqa: E402

BUILD = "0123456789abcdef0123456789abcdef"


def ev(sec, kind_body, minute=0):
    return "2026-10-31 12:%02d:%02d | VPEV1 t=%d.000 %s" % (minute, sec, sec * 1000, kind_body)


RESYNC = "kind=resync level=minor cause=missed_frames seq=1 mode=shadow"
GPU = "kind=gpu_error api=d3d11 code=887A0005"
GATE = "kind=gate from=a to=b reason=x"


class Base(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = self._tmp.name
        self.bugs = os.path.join(self.tmp, "bugs")
        self.n = 0

    def tearDown(self):
        self._tmp.cleanup()

    def run_main(self, *args):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = intake.main(list(args))
        return code, out.getvalue().strip()

    def write_json(self, doc):
        self.n += 1
        p = os.path.join(self.tmp, "in%d.json" % self.n)
        with open(p, "w", encoding="utf-8") as fh:
            json.dump(doc, fh)
        return p

    def soak(self, lines, check="drops", build=BUILD + "-abc", wall="2026-10-31 12:00:10", **extra):
        doc = {"source": "soak", "check": check, "verdict": "FAIL", "detail": "d%d" % (self.n + 1),
               "build_id": build, "out_dir": self.tmp, "log_path": "",
               "trigger_wall": wall, "first_events": [{"line_no": i + 1, "raw": r} for i, r in enumerate(lines)]}
        doc.update(extra)
        return self.write_json(doc)

    def ticket(self, n):
        with open(os.path.join(self.bugs, "BUG-%04d.md" % n), encoding="utf-8") as fh:
            return fh.read()

    def make_zip(self, name="vp-report-1.zip", manifest=True, log=True, config=True, tamper=""):
        p = os.path.join(self.tmp, name)
        with zipfile.ZipFile(p, "w") as zf:
            if manifest:
                zf.writestr("manifest.json", json.dumps({
                    "format": "vp-state-report", "created_local": "2026-10-31 12:00:10", "trigger": "hotkey",
                    "build_id": BUILD, "commit": "abc", "version": "1.4", "renderer": "libplacebo"}))
            if log:
                zf.writestr("log-excerpt.txt", "\n".join([ev(5, RESYNC), ev(8, GPU), "noise line",
                                                           ev(5, GATE, minute=30)]) + tamper)
            if config:
                zf.writestr("config/b.cfg", "b=2")
                zf.writestr("config/a.cfg", "a=1")
            zf.writestr("crash-state.txt", "source: 3840x2160 p24 HDR10\nrenderer: x\n")
        return p


class Tests(Base):
    def test_bundle(self):
        z = self.make_zip()
        code, out = self.run_main(z, "--bugs-dir", self.bugs)
        self.assertEqual((code, out), (0, "BUG-0001"))
        t = self.ticket(1)
        self.assertIn(z.replace("\\", "/") if False else os.path.abspath(z), t)
        self.assertIn("build_id: " + BUILD, t)
        self.assertIn("source_format: 3840x2160 p24 HDR10", t)
        self.assertIn("severity: S?", t)
        self.assertIn("unit: ?", t)
        self.assertIn("tag: Picture", t)
        self.assertIn("resync=1", t)
        self.assertNotIn("12:30:05", t)  # outside the +/-30 s window
        fp = [ln for ln in t.splitlines() if ln.startswith("config_fingerprint: ")][0].split(": ")[1]
        self.assertEqual(len(fp), 16)
        int(fp, 16)

    def test_fingerprint_order_independent_and_content_sensitive(self):
        a = intake.read_bundle(self.make_zip("a.zip"))["config_fingerprint"]
        b = intake.read_bundle(self.make_zip("b.zip"))["config_fingerprint"]
        self.assertEqual(a, b)
        p = os.path.join(self.tmp, "c.zip")
        with zipfile.ZipFile(p, "w") as zf:
            zf.writestr("config/a.cfg", "a=CHANGED")
            zf.writestr("config/b.cfg", "b=2")
        self.assertNotEqual(a, intake.read_bundle(p)["config_fingerprint"])

    def test_soak_and_looprig(self):
        code, _ = self.run_main(self.soak([ev(9, RESYNC)]), "--bugs-dir", self.bugs)
        self.assertEqual(code, 0)
        t = self.ticket(1)
        self.assertIn("input_kind: soak", t)
        self.assertIn("trigger: drops", t)
        self.assertIn("config_fingerprint: " + intake.hashlib.sha256((BUILD + "-abc" + "drops").encode()).hexdigest()[:16], t)
        lr = self.write_json({"source": "looprig", "scenario": "s1", "row": "r2", "measure": "judder",
                              "value": 3.5, "threshold": 2, "first_frames": [], "log_path": "", "build_id": "b-1"})
        code, out = self.run_main(lr, "--bugs-dir", self.bugs)
        self.assertEqual((code, out), (0, "BUG-0002"))
        t2 = self.ticket(2)
        self.assertIn("input_kind: looprig", t2)
        self.assertIn("trigger: s1/r2/judder", t2)
        self.assertIn("tag: Judder", t2)

    def test_numbering_and_gap(self):
        for i in range(3):
            code, out = self.run_main(self.soak([ev(i, RESYNC)]), "--bugs-dir", self.bugs)
            self.assertEqual(out, "BUG-%04d" % (i + 1))
        os.remove(os.path.join(self.bugs, "BUG-0002.md"))
        code, out = self.run_main(self.soak([ev(7, GPU)]), "--bugs-dir", self.bugs)
        self.assertEqual(out, "BUG-0004")

    def test_index_idempotent(self):
        self.run_main(self.soak([ev(1, RESYNC)]), "--bugs-dir", self.bugs)
        self.run_main(self.soak([ev(2, GPU)], check="gpu"), "--bugs-dir", self.bugs)
        path = os.path.join(self.bugs, "INDEX.md")
        with open(path, "rb") as fh:
            first = fh.read()
        intake.write_index(self.bugs)
        intake.write_index(self.bugs)
        with open(path, "rb") as fh:
            self.assertEqual(first, fh.read())
        text = first.decode()
        self.assertIn("| id | status | tag | trigger | build | date | title | dup-of |", text)
        self.assertIn("BUG-0001", text)
        self.assertIn("BUG-0002", text)

    def test_duplicate_detection(self):
        base = [ev(1, RESYNC), ev(2, GPU), ev(3, GATE)]
        self.run_main(self.soak(base, check="same"), "--bugs-dir", self.bugs)
        # identical signature (different input bytes) -> flagged
        code, _ = self.run_main(self.soak(base, check="same", build=BUILD + "-def"), "--bugs-dir", self.bugs)
        self.assertEqual(code, 0)
        t = self.ticket(2)
        self.assertIn("Possible duplicate of BUG-0001", t)
        self.assertIn("dup_of: BUG-0001", t)
        # clearly different events -> not flagged
        other = ["VPEV1 t=1.000 kind=cadence state=locked pattern=2:3",
                 "VPEV1 t=2.000 kind=repeat cause=pulldown count=2",
                 "VPEV1 t=3.000 kind=source_change w=1 h=1 rate_num=24 rate_den=1 transfer=pq"]
        other = [("2026-10-31 12:00:%02d | " % (i + 1)) + r for i, r in enumerate(other)]
        self.run_main(self.soak(other, check="same"), "--bugs-dir", self.bugs)
        t3 = self.ticket(3)
        self.assertNotIn("Possible duplicate", t3)
        self.assertIn("dup_of: none", t3)

    def test_closed_ticket_not_duplicate(self):
        base = [ev(1, RESYNC), ev(2, GPU)]
        self.run_main(self.soak(base), "--bugs-dir", self.bugs)
        p = os.path.join(self.bugs, "BUG-0001.md")
        with open(p, encoding="utf-8") as fh:
            body = fh.read().replace("status: open", "status: closed", 1)
        with open(p, "w", encoding="utf-8") as fh:
            fh.write(body)
        self.run_main(self.soak(base, build=BUILD + "-zzz"), "--bugs-dir", self.bugs)
        self.assertNotIn("Possible duplicate", self.ticket(2))

    def test_rerun_refused(self):
        s = self.soak([ev(1, RESYNC)])
        self.assertEqual(self.run_main(s, "--bugs-dir", self.bugs)[0], 0)
        code, out = self.run_main(s, "--bugs-dir", self.bugs)
        self.assertEqual(code, 4)
        self.assertIn("BUG-0001", out)
        self.assertEqual(sorted(f for f in os.listdir(self.bugs) if f.startswith("BUG-")), ["BUG-0001.md"])

    def test_missing_fields_unknown(self):
        z = self.make_zip(manifest=False, log=False, config=False)
        code, _ = self.run_main(z, "--bugs-dir", self.bugs)
        self.assertEqual(code, 0)
        t = self.ticket(1)
        for key in ("trigger", "build_id", "config_fingerprint"):
            self.assertIn("%s: unknown" % key, t)
        for ln in t.splitlines():
            if ln.endswith(":") and ln.split(":")[0].islower():
                self.fail("blank field: " + ln)
        s = self.write_json({"source": "soak"})
        self.assertEqual(self.run_main(s, "--bugs-dir", self.bugs)[0], 0)
        self.assertIn("build_id: unknown", self.ticket(2))
        self.assertIn("source_format: unknown", self.ticket(2))

    def test_bugs_dir_rules(self):
        s = self.soak([ev(1, RESYNC)])
        self.assertEqual(self.run_main(s)[0], 2)
        inside = os.path.join(intake._repo_root() or HERE, "_never_created_bugs")
        self.assertEqual(self.run_main(s, "--bugs-dir", inside)[0], 2)
        self.assertFalse(os.path.exists(inside))

    def test_bad_input(self):
        p = os.path.join(self.tmp, "bad.zip")
        with open(p, "wb") as fh:
            fh.write(b"not a zip")
        self.assertEqual(self.run_main(p, "--bugs-dir", self.bugs)[0], 3)

    def test_auto_tag(self):
        mk = lambda lines, hint="": intake.derive_tag(__import__("parse").parse_lines(lines), hint)
        self.assertEqual(mk([ev(1, GPU)]), "Picture")
        self.assertEqual(mk([ev(1, RESYNC)]), "Judder")
        self.assertEqual(mk(["VPEV1 t=1.000 kind=repeat cause=x count=1"]), "Judder")
        self.assertEqual(mk(["VPEV1 t=1.000 kind=cadence state=a pattern=b"]), "Judder")
        self.assertEqual(mk(["VPEV1 t=1.000 kind=source_change w=1 h=1 rate_num=24 rate_den=1 transfer=pq"]), "Colour")
        self.assertEqual(mk([], "wrong colour"), "Colour")
        self.assertEqual(mk([ev(1, GATE)]), "Sync")
        self.assertEqual(mk(["VPEV1 t=1.000 kind=resync level=full cause=renderer_reset seq=1 mode=on"]), "Sync")
        self.assertEqual(mk([]), "Other")

    def test_explicit_tag_and_title(self):
        self.run_main(self.soak([ev(1, GPU)]), "--bugs-dir", self.bugs, "--tag", "Sync", "--title", "My title")
        t = self.ticket(1)
        self.assertIn("tag: Sync", t)
        self.assertIn("title: My title", t)

    def test_build_family(self):
        self.assertEqual(intake.build_family("1.4.2-abc123"), "1.4.2")
        self.assertEqual(intake.build_family("1.4.2+abc123"), "1.4.2")
        self.assertEqual(intake.build_family(BUILD), BUILD)
        self.assertEqual(intake.build_family(""), "unknown")

    def test_excerpt_cap_and_fallback(self):
        lines = [ev(i % 60, RESYNC) for i in range(150)]
        info = {"events": intake.parse.parse_lines(lines), "lines": lines, "trigger_time": None}
        evs, ex, note = intake.window_events(info)
        self.assertEqual(len(ex), 80)
        self.assertIn("last 80", note)
        import datetime
        info["trigger_time"] = datetime.datetime(2026, 10, 31, 12, 0, 30)
        evs, ex, note = intake.window_events(info)
        self.assertLessEqual(len(ex), 80)


if __name__ == "__main__":
    unittest.main()
