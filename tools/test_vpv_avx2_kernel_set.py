# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""VP-V-owned checks of the AVX2 kernel set (standard library only).

tools/test_avx2_boundary.py is Bill's file and is not imported or edited here; its discovery logic
(the project's ClCompile items that end in .AVX2.cpp, and the native-x64-COFF function list of an
object) is read and restated below. What this file adds:
  * the EXACT sorted set of the 11 AVX2 kernel .cpp files (a rename, swap or replacement at the same
    count fails; commit 741de4f4f derived the set: the old 9 + CRGBDolbyVisionTunnelFormatter.AVX2.cpp
    + magewell/MagewellRgb24Packing.AVX2.cpp);
  * the allowed-symbol rule of the Magewell RGB24 kernel: its only function is
    ?ExpandAvx2@MagewellRgb24Packing@@ (the probes ?ExpandAvx2@Other@@ and ?Helper@MagewellRgb24Packing@@
    are rejected).
The object-file checks read the Release objects of VideoProcessor-Lib: pass --objects DIR, or set
VP_AVX2_OBJECTS=DIR (needed with python -m unittest). Without either, the default
src/VideoProcessor-Lib/x64/Release is used and the object checks are skipped when it is missing.

    python -I tools/test_vpv_avx2_kernel_set.py --objects <build>/src/VideoProcessor-Lib/x64/Release
    VP_AVX2_OBJECTS=<dir> python -I -m unittest discover -s tools -p test_vpv_avx2_kernel_set.py
"""

import argparse
import os
from pathlib import Path
import re
import struct
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
NS = {"m": "http://schemas.microsoft.com/developer/msbuild/2003"}
LIB = ROOT / "src/VideoProcessor-Lib"
PROJECT = LIB / "VideoProcessor-Lib.vcxproj"
OBJECTS = Path(os.environ["VP_AVX2_OBJECTS"]) if os.environ.get("VP_AVX2_OBJECTS") else LIB / "x64/Release"
OBJECTS_GIVEN = bool(os.environ.get("VP_AVX2_OBJECTS"))

EXPECTED_KERNELS = sorted((
    "CpuProbe.AVX2.cpp",
    "magewell\\MagewellRgb24Packing.AVX2.cpp",
    "video_frame_formatter\\CARGBtoP010VideoFrameFormatter.AVX2.cpp",
    "video_frame_formatter\\CDeckLinkRGBToP010VideoFrameFormatter.AVX2.cpp",
    "video_frame_formatter\\CR12BtoRGB48VideoFrameFormatter.AVX2.cpp",
    "video_frame_formatter\\CR210toRGB48VideoFrameFormatter.AVX2.cpp",
    "video_frame_formatter\\CRGBDolbyVisionTunnelFormatter.AVX2.cpp",
    "video_frame_formatter\\CUYVYtoP010VideoFrameFormatter.AVX2.cpp",
    "video_frame_formatter\\CUYVYtoP210VideoFrameFormatter.AVX2.cpp",
    "video_frame_formatter\\CV210toP010VideoFrameFormatter.AVX2.cpp",
    "video_frame_formatter\\CV210toP210VideoFrameFormatter.AVX2.cpp",
))
MAGEWELL_KERNEL = "magewell\\MagewellRgb24Packing.AVX2.cpp"
# The one function of MagewellRgb24Packing.AVX2.obj, anchored to its namespace (the "@@" ends the scope).
MAGEWELL_ALLOWED = re.compile(r"^\?ExpandAvx2@MagewellRgb24Packing@@")


def kernel_includes():
    root = ET.parse(PROJECT).getroot()
    return [item.attrib["Include"] for item in root.findall("m:ItemGroup/m:ClCompile", NS)
            if item.attrib["Include"].endswith(".AVX2.cpp")]


def object_name(include):
    return Path(include.replace("\\", "/")).stem + ".obj"


def coff_functions(path):
    """Names of the defined functions of a native x64 COFF object (LTCG objects are rejected)."""
    data = path.read_bytes()
    _, _, _, symbols_at, symbol_count, optional_size, _ = struct.unpack_from("<HHIIIHH", data)
    machine = struct.unpack_from("<H", data)[0]
    if machine != 0x8664 or optional_size != 0:
        raise AssertionError(f"{path.name}: expected native x64 COFF, not LTCG")
    strings_at = symbols_at + symbol_count * 18

    def string_at(offset):
        start = strings_at + offset
        return data[start:data.index(b"\0", start)].decode("ascii")

    functions = []
    index = 0
    while index < symbol_count:
        offset = symbols_at + index * 18
        name_bytes = data[offset:offset + 8]
        name = (string_at(struct.unpack_from("<I", name_bytes, 4)[0])
                if name_bytes[:4] == b"\0" * 4 else name_bytes.rstrip(b"\0").decode("ascii"))
        _, section, kind, storage, auxiliary = struct.unpack_from("<IhHBB", data, offset + 8)
        if section > 0 and kind & 0x20 and storage == 2:
            functions.append(name)
        index += 1 + auxiliary
    return functions


class VpvAvx2KernelSetTests(unittest.TestCase):
    def test_exact_kernel_set(self):
        includes = kernel_includes()
        self.assertEqual(len(includes), len(set(includes)), "a kernel is listed twice")
        self.assertEqual(11, len(includes))
        self.assertEqual(EXPECTED_KERNELS, sorted(includes))

    def test_every_kernel_source_exists(self):
        for include in kernel_includes():
            self.assertTrue((LIB / Path(include.replace("\\", "/"))).is_file(), include)

    def test_magewell_allowed_symbol_rule(self):
        self.assertTrue(MAGEWELL_ALLOWED.search("?ExpandAvx2@MagewellRgb24Packing@@YAXPEBEIPEAEIII@Z"))
        for probe in ("?ExpandAvx2@Other@@YAXXZ", "?Helper@MagewellRgb24Packing@@YAXXZ",
                      "x?ExpandAvx2@MagewellRgb24Packing@@", "?ExpandAvx2@MagewellRgb24PackingX@@YAXXZ",
                      "?ExpandAvx2@MagewellRgb24Packing@Inner@@YAXXZ"):
            self.assertIsNone(MAGEWELL_ALLOWED.search(probe), probe)

    def test_magewell_object_publishes_only_its_kernel(self):
        path = OBJECTS / object_name(MAGEWELL_KERNEL)
        if not path.is_file():
            if OBJECTS_GIVEN:
                self.fail(f"{path} not found")
            self.skipTest(f"no Release objects at {OBJECTS} (pass --objects or VP_AVX2_OBJECTS)")
        functions = coff_functions(path)
        self.assertTrue(functions, path.name)
        for function in functions:
            self.assertRegex(function, MAGEWELL_ALLOWED, path.name)
        self.assertEqual(1, len(functions), functions)

    def test_every_kernel_has_a_native_object(self):
        missing = [include for include in kernel_includes() if not (OBJECTS / object_name(include)).is_file()]
        if len(missing) == len(EXPECTED_KERNELS) and not OBJECTS_GIVEN:
            self.skipTest(f"no Release objects at {OBJECTS} (pass --objects or VP_AVX2_OBJECTS)")
        self.assertEqual([], missing)
        for include in kernel_includes():
            self.assertTrue(coff_functions(OBJECTS / object_name(include)), include)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--objects", type=Path, default=None)
    options, remaining = parser.parse_known_args()
    if options.objects is not None:
        OBJECTS = options.objects
        OBJECTS_GIVEN = True
    unittest.main(argv=[__file__] + remaining)
