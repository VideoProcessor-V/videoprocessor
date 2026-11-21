# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Unittest form of Bill's tools/test_libplacebo_variants.py (VP-V file; Bill's script stays byte-identical).

Both packaged VP renderers must load together in one process, in either order. Bill's script does that check and
prints one "PASS <plugin>: API <n>; <libplacebo>; <version>" line per renderer, then "PASS simultaneous loading and
shared runtime identity". Its own CLI selects one load order (--beta-first) and the build folder (--build-root), so
each test below runs Bill's script as a CHILD process (python -I tools/test_libplacebo_variants.py ...): DLL loading is
process-global, so the second order needs a fresh process, and no copy of Bill's loading logic exists here.

The build root is VP_BUILD_ROOT (default <repo>/x64/Release). A missing Release build FAILS; it is never a skip (the PY
lane runs after the Release build step). Executed: 2 (test_standard_first, test_beta_first).
"""
import os
import re
import subprocess
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
BILL_SCRIPT = REPO / "tools" / "test_libplacebo_variants.py"
API_HEADER = REPO / "src/VideoProcessor-Lib/vprenderer/LibplaceboRendererPluginApi.h"
STANDARD = ("vprenderer", "VideoProcessorVPRenderer.dll", "libplacebo-360.dll")
BETA = ("vprenderer-beta", "VideoProcessorVPRendererBeta.dll", "libplacebo-388.dll")
SHARED_LINE = "PASS simultaneous loading and shared runtime identity"


def build_root():
    return Path(os.environ.get("VP_BUILD_ROOT") or (REPO / "x64" / "Release"))


def expected_api_version():
    """The renderer ABI version from the host's source header (the value Bill's script asserts against)."""
    versions = re.findall(
        r"^\s*static\s+constexpr\s+uint32_t\s+VP_LIBPLACEBO_PLUGIN_API_VERSION\s*=\s*(\d+)\s*;",
        API_HEADER.read_text(encoding="utf-8"), re.MULTILINE)
    assert len(versions) == 1, "Expected one renderer ABI version in the host header"
    return int(versions[0])


class LibplaceboVariantsLoadTogether(unittest.TestCase):
    """Bill's check in each load order, each in its own child process. A missing build fails; there is no skip."""

    def _run(self, beta_first):
        root = build_root()
        for directory, plugin_name, library_name in (STANDARD, BETA):
            for name in (plugin_name, library_name):
                self.assertTrue((root / directory / name).is_file(),
                                f"missing {root / directory / name}: build x64 Release first (VP_BUILD_ROOT={root})")
        self.assertTrue(BILL_SCRIPT.is_file(), f"missing {BILL_SCRIPT}")
        cmd = [sys.executable, "-I", str(BILL_SCRIPT), "--build-root", str(root)]
        if beta_first:
            cmd.append("--beta-first")
        done = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
        out = done.stdout
        self.assertEqual(done.returncode, 0, f"child rc {done.returncode}\n{out}\n{done.stderr}")
        api = expected_api_version()
        lines = out.splitlines()
        plugin_lines = [ln for ln in lines if ln.startswith("PASS Video") and ": API " in ln]
        order = [STANDARD, BETA]
        if beta_first:
            order.reverse()
        self.assertEqual(len(plugin_lines), 2, out)
        for line, (_directory, plugin_name, library_name) in zip(plugin_lines, order):
            self.assertTrue(line.startswith(f"PASS {plugin_name}: API {api}; {library_name}; "), line)
            self.assertGreater(len(line.split("; ", 2)[2].strip()), 0, line)   # the libplacebo version string
        self.assertIn(SHARED_LINE, lines)
        # Order: both plugin lines come before the shared-runtime line (Bill's script prints them in that order).
        self.assertLess(max(lines.index(ln) for ln in plugin_lines), lines.index(SHARED_LINE), out)

    def test_standard_first(self):
        self._run(False)

    def test_beta_first(self):
        self._run(True)


if __name__ == "__main__":
    unittest.main()
