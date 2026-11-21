#!/usr/bin/env python3
# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""VP-V AVX2 boundary regression tests (RG-SP5; complements Bill's untouched tools/test_avx2_boundary.py).

Checks src/VP-V/build/*.props and every baseline source compiled via src/VP-V/build/*.items.props
so AVX2 compiler flags and AVX2 intrinsics cannot slip into AVX1 baseline translation units.
"""

from pathlib import Path
import re
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
NS = {"m": "http://schemas.microsoft.com/developer/msbuild/2003"}
PROPS_DIR = ROOT / "src/VP-V/build"

# VideoProcessor-Lib baseline is /arch:AVX (AVX1); pure AVX1 float/double intrinsics guarded by
# #if defined(__AVX__) are valid on AVX1 CPUs, whereas any other _mm256_* intrinsic is AVX2-only.
AVX1_PS_PD_ALLOWLIST = re.compile(
    r"^_mm256_(?:loadu?|storeu?|set(?:1|zero|r)?|add|sub|mul|div|hadd|hsub|sqrt|rcp|rsqrt|"
    r"min|max|and|andnot|or|xor|cmp|blendv?|shuffle|permute(?:2f128)?|unpacklo|unpackhi|"
    r"movemask|testz|testc|testnzc|round|floor|ceil|dp|broadcast|extractf128|insertf128)_p[sd]$"
)


def _project_dir_for_props(props_path: Path) -> Path:
    stem = props_path.name.replace(".items.props", "")
    mapping = {
        "VideoProcessor-Lib": ROOT / "src/VideoProcessor-Lib",
        "VideoProcessor-Test": ROOT / "src/VideoProcessor-Test",
        "VideoProcessor-VPRenderer": ROOT / "src/VideoProcessor-VPRenderer",
        "VideoProcessor-VPRendererBeta": ROOT / "src/VideoProcessor-VPRenderer",
        "VideoProcessor-BetaTests": ROOT / "src/VideoProcessor-BetaTests",
        "VideoProcessor-BetaTestsHost": ROOT / "src/VideoProcessor-BetaTests",
        "VideoProcessor-ConfigTests": ROOT / "src/VideoProcessor-ConfigTests",
    }
    return mapping.get(stem, props_path.parent)


def _strip_comments(text: str) -> str:
    return re.sub(r"//[^\n]*|/\*.*?\*/", "", text, flags=re.S)


def _has_avx_guard(text: str, pos: int) -> bool:
    before = text[:pos]
    depth = 0
    for m in re.finditer(r"^\s*#\s*(if\b[^\n]*|ifdef\b[^\n]*|ifndef\b[^\n]*|endif\b)", before, flags=re.M):
        tok = m.group(1)
        if tok.startswith("endif"):
            depth = max(0, depth - 1)
        elif "__AVX__" in tok and not tok.startswith("ifndef"):
            depth += 1
        elif depth > 0:
            depth += 1
    return depth > 0


class VpvAvx2BoundaryTests(unittest.TestCase):
    def test_no_project_wide_or_baseline_avx2_in_vpv_props(self):
        props_files = sorted(PROPS_DIR.glob("*.props"))
        self.assertTrue(props_files, "expected src/VP-V/build/*.props")
        for path in props_files:
            root = ET.parse(path).getroot()
            for group in root.findall("m:ItemDefinitionGroup", NS):
                for setting in group.findall("m:ClCompile/m:EnableEnhancedInstructionSet", NS):
                    self.assertNotIn(setting.text, ("AdvancedVectorExtensions2", "AdvancedVectorExtensions512"), str(path))
            for item in root.findall("m:ItemGroup/m:ClCompile", NS):
                inc = item.attrib.get("Include", "")
                if inc.endswith(".AVX2.cpp"):
                    continue
                for setting in item.findall("m:EnableEnhancedInstructionSet", NS):
                    self.assertNotIn(setting.text, ("AdvancedVectorExtensions2", "AdvancedVectorExtensions512"),
                                     f"{path}: {inc}")

    def test_no_avx2_intrinsics_in_vpv_lib_baseline_sources(self):
        lib_props = PROPS_DIR / "VideoProcessor-Lib.items.props"
        self.assertTrue(lib_props.is_file(), str(lib_props))
        proj_dir = _project_dir_for_props(lib_props)
        sources = []
        for item in ET.parse(lib_props).getroot().findall("m:ItemGroup/m:ClCompile", NS):
            raw_inc = item.attrib["Include"]
            if "$(MSBuildThisFileDirectory)" in raw_inc:
                inc = raw_inc.replace("$(MSBuildThisFileDirectory)", "").replace("\\", "/")
                sources.append((lib_props.parent / inc).resolve())
            else:
                sources.append((proj_dir / Path(raw_inc.replace("\\", "/"))).resolve())
        self.assertGreater(len(sources), 0)
        for source in sources:
            if source.name.endswith(".AVX2.cpp"):
                continue
            self.assertTrue(source.is_file(), f"missing source listed in props: {source}")
            text = _strip_comments(source.read_text(encoding="utf-8", errors="replace"))
            for m in re.finditer(r"\b(_mm256_\w+)\s*\(", text):
                name = m.group(1)
                self.assertRegex(name, AVX1_PS_PD_ALLOWLIST, f"AVX2 intrinsic {name} in baseline source {source}")
                self.assertTrue(_has_avx_guard(text, m.start()),
                                f"AVX1 intrinsic {name} without #if defined(__AVX__) guard in {source}")


if __name__ == "__main__":
    unittest.main()
