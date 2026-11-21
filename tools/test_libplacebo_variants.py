"""Load both packaged VP renderers in one process, in either order (Windows x64).

As a unittest suite (tools lane PY): test_standard_first and test_beta_first each run check() in a CHILD process
(python -I <this file> --child [--beta-first]), because DLL loading is process-global: the second order would otherwise
reuse the modules the first one loaded. The build root is VP_BUILD_ROOT (default <repo>/x64/Release); a missing Release
build FAILS (it is not a skip): the lane runs after the Release build step.

Manual use is unchanged: python tools/test_libplacebo_variants.py [--build-root DIR] [--beta-first]
"""
import argparse
import ctypes
import hashlib
import os
import re
import subprocess
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
API_HEADER = REPO / "src/VideoProcessor-Lib/vprenderer/LibplaceboRendererPluginApi.h"
STANDARD = (False, "vprenderer", "VideoProcessorVPRenderer.dll", "libplacebo-360.dll")
BETA = (True, "vprenderer-beta", "VideoProcessorVPRendererBeta.dll", "libplacebo-388.dll")


def default_build_root():
    return Path(os.environ.get("VP_BUILD_ROOT") or (REPO / "x64/Release"))


def expected_api_version():
    """The renderer ABI version of the host's source header, not an old literal."""
    versions = re.findall(
        r"^\s*static\s+constexpr\s+uint32_t\s+VP_LIBPLACEBO_PLUGIN_API_VERSION\s*=\s*(\d+)\s*;",
        API_HEADER.read_text(encoding="utf-8"), re.MULTILINE)
    assert len(versions) == 1, "Expected one renderer ABI version in the host header"
    return int(versions[0])


def check(build_root, beta_first):
    """Load both renderers (and their libplacebo) in this process in the given order; print one PASS line per renderer
    and one for the shared runtimes. Raises AssertionError on any mismatch."""
    root = Path(build_root).resolve()
    expected_api = expected_api_version()
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.GetModuleHandleW.argtypes = [ctypes.c_wchar_p]
    kernel.GetModuleHandleW.restype = ctypes.c_void_p
    kernel.GetModuleFileNameW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_uint]
    kernel.GetModuleFileNameW.restype = ctypes.c_uint
    variants = [STANDARD, BETA]
    if beta_first:
        variants.reverse()
    modules = []
    for beta, directory, plugin_name, library_name in variants:
        folder = root / directory
        plugin = ctypes.CDLL(str(folder / plugin_name), winmode=0x100 | 0x800)
        modules.append(plugin)
        plugin.VideoProcessorLibplaceboIsBeta.restype = ctypes.c_bool
        plugin.VideoProcessorLibplaceboGetApiVersion.restype = ctypes.c_uint32
        assert plugin.VideoProcessorLibplaceboIsBeta() == beta, plugin_name
        assert plugin.VideoProcessorLibplaceboGetApiVersion() == expected_api, plugin_name
        handle = kernel.GetModuleHandleW(library_name)
        assert handle, library_name
        path = ctypes.create_unicode_buffer(32768)
        assert kernel.GetModuleFileNameW(handle, path, len(path))
        assert Path(path.value).resolve() == (folder / library_name).resolve(), path.value
        library = ctypes.CDLL(str(folder / library_name), winmode=0x100 | 0x800)
        library.pl_version.restype = ctypes.c_char_p
        print(f"PASS {plugin_name}: API {expected_api}; {library_name}; {library.pl_version().decode()}")
    # Windows can reuse already loaded transitive DLLs by basename. Enforce identical
    # shared runtimes instead of assuming directory separation isolates these DLLs.
    for stable in (root / "vprenderer").glob("*.dll"):
        if stable.name.startswith(("libplacebo-", "VideoProcessor")):
            continue
        other = root / "vprenderer-beta" / stable.name
        if other.exists():
            digest = lambda p: hashlib.sha256(p.read_bytes()).digest()
            assert digest(stable) == digest(other), f"Shared runtime mismatch: {stable.name}"
    print("PASS simultaneous loading and shared runtime identity")


def _child(beta_first):
    """Run this file's CLI in a fresh interpreter; (returncode, stdout, stderr)."""
    cmd = [sys.executable, "-I", str(Path(__file__).resolve()), "--child", "--build-root", str(default_build_root())]
    if beta_first:
        cmd.append("--beta-first")
    done = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    return done.returncode, done.stdout, done.stderr


class LibplaceboVariantsLoadTogether(unittest.TestCase):
    """Both orders, each in a child process. A missing Release build fails; there is no skip."""

    def _run(self, beta_first):
        root = default_build_root()
        for _beta, directory, plugin_name, library_name in (STANDARD, BETA):
            for name in (plugin_name, library_name):
                self.assertTrue((root / directory / name).is_file(),
                                f"missing {root / directory / name}: build x64 Release first (VP_BUILD_ROOT={root})")
        rc, out, err = _child(beta_first)
        self.assertEqual(rc, 0, f"child rc {rc}\n{out}\n{err}")
        api = expected_api_version()
        lines = out.splitlines()
        plugin_lines = [ln for ln in lines if ln.startswith("PASS Video") and ": API " in ln]
        order = [STANDARD, BETA]
        if beta_first:
            order.reverse()
        self.assertEqual(len(plugin_lines), 2, out)
        for line, (_beta, _dir, plugin_name, library_name) in zip(plugin_lines, order):
            self.assertTrue(line.startswith(f"PASS {plugin_name}: API {api}; {library_name}; "), line)
            self.assertGreater(len(line.split("; ", 2)[2].strip()), 0, line)   # the libplacebo version string
        self.assertIn("PASS simultaneous loading and shared runtime identity", lines)

    def test_standard_first(self):
        self._run(False)

    def test_beta_first(self):
        self._run(True)


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-root", type=Path, default=default_build_root())
    parser.add_argument("--beta-first", action="store_true")
    parser.add_argument("--child", action="store_true", help="used by the unittest suite: run check() and exit")
    args = parser.parse_args(argv)
    check(args.build_root, args.beta_first)
    return 0


if __name__ == "__main__":
    sys.exit(main())
