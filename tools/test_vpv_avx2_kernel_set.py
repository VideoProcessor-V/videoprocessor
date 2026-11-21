# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""VP-V-owned checks of the AVX2 kernel set and its boundary (standard library only).

tools/test_avx2_boundary.py is Bill's file and is not imported or edited here; its logic is read and
restated below, check for check:
  1. no project-wide AVX2 / AVX-512 (ItemDefinitionGroup settings of every .vcxproj under src);
  2. every kernel item sets AVX2, WholeProgramOptimization=false (no LTCG) and PrecompiledHeader=NotUsing, unconditionally;
  3. no baseline (non-.AVX2.cpp) source of VideoProcessor-Lib uses a _mm256_ intrinsic (comments stripped);
  4. no kernel object has a .CRT / .tls section (no startup initializer);
  5. every kernel object publishes only its allowed, anchored functions (Bill's list is one combined pattern;
     here each object has its OWN pattern, so a kernel cannot export another kernel's name).
What this file adds to Bill's checks:
  * the EXACT sorted set of the 11 AVX2 kernel .cpp files (a rename, swap or replacement at the same count
    fails; commit 741de4f4f derived the set: the old 9 + CRGBDolbyVisionTunnelFormatter.AVX2.cpp and
    magewell/MagewellRgb24Packing.AVX2.cpp);
  * the Magewell RGB24 kernel's rule: its only function is ?ExpandAvx2@MagewellRgb24Packing@@;
  * pinned probes: ?ExpandAvx2@Other@@, ?Helper@MagewellRgb24Packing@@ and the std min / max COMDATs are rejected;
  * a mutation test for every check, so each check is shown to fail on a broken input.
The object checks (4, 5) read the Release objects of VideoProcessor-Lib: pass --objects DIR, or set
VP_AVX2_OBJECTS=DIR (needed with python -m unittest). Without either, src/VideoProcessor-Lib/x64/Release is the
default and the object checks are skipped when it is missing; with one given, a missing object FAILS.

    python -I tools/test_vpv_avx2_kernel_set.py --objects <build>/src/VideoProcessor-Lib/x64/Release
    VP_AVX2_OBJECTS=<dir> python -I -m unittest discover -s tools -p test_vpv_avx2_kernel_set.py
"""

import argparse
import copy
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

MAGEWELL_KERNEL = "magewell\\MagewellRgb24Packing.AVX2.cpp"

# kernel include -> the anchored pattern of the functions its object may publish ("@@" ends the scope, so the class or
# namespace is part of the match: another scope with the same function name is rejected).
ALLOWED_SYMBOLS = {
    "CpuProbe.AVX2.cpp": r"^\?ExecuteAvx2Probe@CpuFeatures@@",
    MAGEWELL_KERNEL: r"^\?ExpandAvx2@MagewellRgb24Packing@@",
    "video_frame_formatter\\CARGBtoP010VideoFrameFormatter.AVX2.cpp": r"^\?ConvertAVX2@CARGBtoP010VideoFrameFormatter@@",
    "video_frame_formatter\\CDeckLinkRGBToP010VideoFrameFormatter.AVX2.cpp":
        r"^\?(?:ConvertLimited10RowPairsAVX2|ConvertR12RowPairsAVX2)@CDeckLinkRGBToP010VideoFrameFormatter@@",
    "video_frame_formatter\\CR12BtoRGB48VideoFrameFormatter.AVX2.cpp": r"^\?ConvertRowsAVX2@CR12BtoRGB48VideoFrameFormatter@@",
    "video_frame_formatter\\CR210toRGB48VideoFrameFormatter.AVX2.cpp": r"^\?ConvertRowsAVX2@CR210toRGB48VideoFrameFormatter@@",
    "video_frame_formatter\\CRGBDolbyVisionTunnelFormatter.AVX2.cpp": r"^\?ConvertRowsAVX2@CRGBDolbyVisionTunnelFormatter@@",
    "video_frame_formatter\\CUYVYtoP010VideoFrameFormatter.AVX2.cpp": r"^\?ConvertAVX2@CUYVYtoP010VideoFrameFormatter@@",
    "video_frame_formatter\\CUYVYtoP210VideoFrameFormatter.AVX2.cpp": r"^\?ConvertAVX2@CUYVYtoP210VideoFrameFormatter@@",
    "video_frame_formatter\\CV210toP010VideoFrameFormatter.AVX2.cpp":
        r"^(?:\?ProcessAdvancedSegmentAVX2|\?\?\$(?:ProcessLineSegmentImpl|ConvertV210ToP010_SIMDImpl)@\$0\w*@?)@CV210toP010VideoFrameFormatter@@",
    "video_frame_formatter\\CV210toP210VideoFrameFormatter.AVX2.cpp": r"^\?ConvertAVX2@CV210toP210VideoFrameFormatter@@",
}
EXPECTED_KERNELS = sorted(ALLOWED_SYMBOLS)

# Shared inline COMDAT helpers compiled for AVX2 must never be selected for baseline callers by the linker.
REJECTED_PROBES = (
    "?ExpandAvx2@Other@@YAXXZ",
    "?Helper@MagewellRgb24Packing@@YAXXZ",
    "x?ExpandAvx2@MagewellRgb24Packing@@",
    "?ExpandAvx2@MagewellRgb24PackingX@@YAXXZ",
    "?ExpandAvx2@MagewellRgb24Packing@Inner@@YAXXZ",
    "??$min@H@std@@YAAEBHAEBH0@Z",   # std::min COMDAT
    "??$max@I@std@@YAAEBIAEBI0@Z",   # std::max COMDAT
)


# ---- discovery (restated from Bill's tools/test_avx2_boundary.py) ----------------------------------------------

def project(path):
    return ET.parse(path).getroot()


def kernel_items(root):
    return [item for item in root.findall("m:ItemGroup/m:ClCompile", NS) if item.attrib["Include"].endswith(".AVX2.cpp")]


def kernel_includes():
    return [item.attrib["Include"] for item in kernel_items(project(PROJECT))]


def object_name(include):
    return Path(include.replace("\\", "/")).stem + ".obj"


def coff(path):
    """(section names, defined function names) of a native x64 COFF object; LTCG objects are rejected."""
    data = path.read_bytes()
    machine, sections, _, symbols_at, symbol_count, optional_size, _ = struct.unpack_from("<HHIIIHH", data)
    if machine != 0x8664 or optional_size != 0:
        raise AssertionError(f"{path.name}: expected native x64 COFF, not LTCG")
    strings_at = symbols_at + symbol_count * 18

    def string_at(offset):
        start = strings_at + offset
        return data[start:data.index(b"\0", start)].decode("ascii")

    names = []
    for index in range(sections):
        name = data[20 + index * 40:28 + index * 40].rstrip(b"\0").decode("ascii")
        names.append(string_at(int(name[1:])) if name.startswith("/") else name)
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
    return names, functions


# ---- the checks, as functions of their input (each returns a list of problems; [] = pass) ---------------------

def project_wide_avx2_problems(roots):
    """Check 1. roots = [(name, project root element)]."""
    problems = []
    for name, root in roots:
        for group in root.findall("m:ItemDefinitionGroup", NS):
            for setting in group.findall("m:ClCompile/m:EnableEnhancedInstructionSet", NS):
                if setting.text in ("AdvancedVectorExtensions2", "AdvancedVectorExtensions512"):
                    problems.append(f"{name}: project-wide {setting.text}")
    return problems


def kernel_setting_problems(root):
    """Check 2. Every kernel item: AVX2 on, no LTCG, no PCH, none of them conditional."""
    problems = []
    for item in kernel_items(root):
        for name, value in (("EnableEnhancedInstructionSet", "AdvancedVectorExtensions2"),
                            ("WholeProgramOptimization", "false"), ("PrecompiledHeader", "NotUsing")):
            setting = item.find("m:" + name, NS)
            if setting is None:
                problems.append(f"{item.attrib['Include']}: {name} missing")
                continue
            if setting.text != value:
                problems.append(f"{item.attrib['Include']}: {name} is {setting.text}, expected {value}")
            if "Condition" in setting.attrib:
                problems.append(f"{item.attrib['Include']}: {name} is conditional")
    return problems


def intrinsic_problems(sources):
    """Check 3. sources = [(name, text)]: a _mm256_ call outside comments."""
    problems = []
    for name, text in sources:
        text = re.sub(r"//[^\n]*|/\*.*?\*/", "", text, flags=re.S)
        if re.search(r"\b_mm256_\w+\s*\(", text):
            problems.append(f"{name}: AVX2 intrinsic in a baseline source")
    return problems


def startup_section_problems(name, sections):
    """Check 4."""
    return [f"{name}: startup section {s}" for s in sections if s.startswith(".CRT") or s.startswith(".tls")]


def symbol_problems(include, functions):
    """Check 5: the object publishes at least one function, and only its own anchored names."""
    if include not in ALLOWED_SYMBOLS:
        return [f"{include}: not a known kernel"]
    if not functions:
        return [f"{include}: publishes no function"]
    allowed = re.compile(ALLOWED_SYMBOLS[include])
    return [f"{include}: function not allowed: {f}" for f in functions if not allowed.search(f)]


def object_path(include):
    return OBJECTS / object_name(include)


class VpvAvx2KernelSetTests(unittest.TestCase):
    def need_objects(self):
        if not any(object_path(i).is_file() for i in EXPECTED_KERNELS):
            if OBJECTS_GIVEN:
                self.fail(f"no Release objects at {OBJECTS}")
            self.skipTest(f"no Release objects at {OBJECTS} (pass --objects or VP_AVX2_OBJECTS)")

    # -- the exact set --
    def test_exact_kernel_set(self):
        includes = kernel_includes()
        self.assertEqual(len(includes), len(set(includes)), "a kernel is listed twice")
        self.assertEqual(11, len(includes))
        self.assertEqual(EXPECTED_KERNELS, sorted(includes))

    def test_every_kernel_source_exists(self):
        for include in kernel_includes():
            self.assertTrue((LIB / Path(include.replace("\\", "/"))).is_file(), include)

    # -- 1. no project-wide AVX2 --
    def test_no_project_wide_avx2(self):
        roots = [(str(p), project(p)) for p in (ROOT / "src").rglob("*.vcxproj")]
        self.assertTrue(roots)
        self.assertEqual([], project_wide_avx2_problems(roots))

    def test_project_wide_avx2_check_fails_on_a_mutation(self):
        for value in ("AdvancedVectorExtensions2", "AdvancedVectorExtensions512"):
            xml = ('<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003"><ItemDefinitionGroup><ClCompile>'
                   f"<EnableEnhancedInstructionSet>{value}</EnableEnhancedInstructionSet></ClCompile></ItemDefinitionGroup></Project>")
            self.assertTrue(project_wide_avx2_problems([("mutant", ET.fromstring(xml))]), value)

    # -- 2. per-kernel settings --
    def test_kernel_settings(self):
        self.assertEqual([], kernel_setting_problems(project(PROJECT)))

    def test_kernel_settings_check_fails_on_mutations(self):
        base = project(PROJECT)
        for name, bad in (("EnableEnhancedInstructionSet", "NotSet"), ("WholeProgramOptimization", "true"),
                          ("PrecompiledHeader", "Use")):
            mutant = copy.deepcopy(base)
            kernel_items(mutant)[3].find("m:" + name, NS).text = bad
            self.assertTrue(kernel_setting_problems(mutant), name + " changed")
            mutant = copy.deepcopy(base)
            item = kernel_items(mutant)[4]
            item.remove(item.find("m:" + name, NS))
            self.assertTrue(kernel_setting_problems(mutant), name + " removed")
            mutant = copy.deepcopy(base)
            kernel_items(mutant)[5].find("m:" + name, NS).attrib["Condition"] = "'$(Platform)'=='x64'"
            self.assertTrue(kernel_setting_problems(mutant), name + " conditional")

    # -- 3. baseline sources free of intrinsics --
    def test_no_avx2_intrinsics_in_baseline_sources(self):
        sources = []
        for item in project(PROJECT).findall("m:ItemGroup/m:ClCompile", NS):
            source = LIB / Path(item.attrib["Include"].replace("\\", "/"))
            if source.name.endswith(".AVX2.cpp"):
                continue
            sources.append((str(source), source.read_text(encoding="utf-8", errors="replace")))
        self.assertTrue(sources)
        self.assertEqual([], intrinsic_problems(sources))

    def test_intrinsic_check_fails_on_a_mutation(self):
        self.assertTrue(intrinsic_problems([("m.cpp", "int f(){ return _mm256_add_epi32 (a, b); }")]))
        self.assertEqual([], intrinsic_problems([("m.cpp", "// _mm256_add_epi32(a, b)\n/* _mm256_set1_epi8( */ int x;")]))

    # -- 4. no startup sections in the kernel objects --
    def test_kernel_objects_have_no_startup_initializers(self):
        self.need_objects()
        for include in kernel_includes():
            self.assertTrue(object_path(include).is_file(), f"{object_path(include)} not found")
            sections, _ = coff(object_path(include))
            self.assertEqual([], startup_section_problems(object_name(include), sections))

    def test_startup_section_check_fails_on_a_mutation(self):
        self.assertTrue(startup_section_problems("m.obj", [".text", ".CRT$XCU"]))
        self.assertTrue(startup_section_problems("m.obj", [".tls$"]))
        self.assertEqual([], startup_section_problems("m.obj", [".text$mn", ".xdata"]))

    # -- 5. the symbol scan over ALL 11 kernel objects --
    def test_every_kernel_object_publishes_only_its_allowed_functions(self):
        self.need_objects()
        seen = 0
        for include in kernel_includes():
            self.assertTrue(object_path(include).is_file(), f"{object_path(include)} not found")
            _, functions = coff(object_path(include))
            self.assertEqual([], symbol_problems(include, functions))
            seen += 1
        self.assertEqual(11, seen)

    def test_symbol_check_fails_on_mutations(self):
        for include in ALLOWED_SYMBOLS:
            self.assertTrue(symbol_problems(include, []), include + ": no function")
            for probe in REJECTED_PROBES:
                self.assertTrue(symbol_problems(include, [probe]), f"{include}: {probe}")
        # another kernel's name is not allowed in this object
        self.assertTrue(symbol_problems(MAGEWELL_KERNEL, ["?ConvertAVX2@CUYVYtoP010VideoFrameFormatter@@AEBAXPEBEPEAG1@Z"]))
        self.assertTrue(symbol_problems("CpuProbe.AVX2.cpp", ["?ExpandAvx2@MagewellRgb24Packing@@YAXPEBEIPEAEIII@Z"]))

    # -- the Magewell rule and the pinned probes --
    def test_magewell_allowed_symbol_rule(self):
        magewell = re.compile(ALLOWED_SYMBOLS[MAGEWELL_KERNEL])
        self.assertTrue(magewell.search("?ExpandAvx2@MagewellRgb24Packing@@YAXPEBEIPEAEIII@Z"))
        for probe in REJECTED_PROBES:
            self.assertIsNone(magewell.search(probe), probe)

    def test_magewell_object_publishes_exactly_its_one_function(self):
        self.need_objects()
        path = object_path(MAGEWELL_KERNEL)
        self.assertTrue(path.is_file(), f"{path} not found")
        _, functions = coff(path)
        self.assertEqual([], symbol_problems(MAGEWELL_KERNEL, functions))
        self.assertEqual(1, len(functions), functions)

    def test_pinned_probes_are_rejected_by_every_kernel_pattern(self):
        for include, pattern in ALLOWED_SYMBOLS.items():
            compiled = re.compile(pattern)
            for probe in REJECTED_PROBES:
                self.assertIsNone(compiled.search(probe), f"{include}: {probe}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--objects", type=Path, default=None)
    options, remaining = parser.parse_known_args()
    if options.objects is not None:
        OBJECTS = options.objects
        OBJECTS_GIVEN = True
    unittest.main(argv=[__file__] + remaining)
