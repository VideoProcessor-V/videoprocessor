# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""Independent adversarial unittest suite for N5 (check_public_hygiene.py),
N8 (check_readme_vs_code.py), and their registration in run_compliance.py.

Runs each checker against isolated temporary synthetic git mini-repos via
`python -I` using the public CLI contract (--root, --strict, --selftest;
exit 0 clean / 1 violation / 2 error). Every negative control mutates a
verified-clean baseline mini-repo to prove the hazard is genuinely detected.
"""
import contextlib
import importlib.util
import json
import os
import stat
import subprocess
import sys
import tempfile
import unittest

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
N5_SCRIPT = os.path.join(TOOLS_DIR, "check_public_hygiene.py")
N8_SCRIPT = os.path.join(TOOLS_DIR, "check_readme_vs_code.py")
RUN_COMPLIANCE_SCRIPT = os.path.join(TOOLS_DIR, "run_compliance.py")


def _make_writable(root):
    for dp, dn, fn in os.walk(root):
        for name in dn + fn:
            try:
                os.chmod(os.path.join(dp, name), stat.S_IWRITE | stat.S_IREAD)
            except OSError:
                pass


@contextlib.contextmanager
def _temp_git_repo():
    with tempfile.TemporaryDirectory(prefix="vpv_n5n8_") as tmp:
        try:
            _git(tmp, "init", "-q")
            yield tmp
        finally:
            _make_writable(tmp)


def _git(root, *args):
    subprocess.run(
        ["git", "-C", root, "-c", "user.name=test", "-c", "user.email=test@example.invalid"] + list(args),
        check=True,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )


def _write_text(root, rel, text):
    full = os.path.join(root, rel)
    os.makedirs(os.path.dirname(full), exist_ok=True)
    with open(full, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)
    return full


def _commit_all(root, message="update"):
    _git(root, "add", "-A")
    _git(root, "commit", "-qm", message)


def _run_tool(script, *args):
    proc = subprocess.run(
        [sys.executable, "-I", script] + list(args),
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=60,
    )
    return proc.returncode, (proc.stdout or "") + (proc.stderr or "")


def _seed_clean_n5_repo(root):
    _write_text(
        root,
        "README.md",
        "# Synthetic Demo Repo\n\nSee `src/VP-V/modules/colour/demo/Demo.h` and `docs/overview.md`.\n",
    )
    _write_text(
        root,
        "docs/overview.md",
        "# Overview\n\nAll references use repository-relative paths such as `tools/run_compliance.py`.\n",
    )
    _write_text(
        root,
        "src/VP-V/modules/colour/demo/Demo.h",
        (
            "// Copyright(C) 2026 VP-V contributors\n"
            "// SPDX-License-Identifier: GPL-3.0-only\n"
            "#pragma once\n"
            "namespace Demo {\n"
            "inline const char* DocPath() { return \"docs/overview.md\"; }\n"
            "}\n"
        ),
    )
    _commit_all(root, "clean base")


def _build_n8_manifest(status="built", live_files=None):
    if live_files is None:
        live_files = ["src/VP-V/modules/colour/demo/Demo.h"]
    return {
        "schema": "vp-module-manifest/1",
        "slot": "colour.demo",
        "unit": "colour",
        "order": 99,
        "status": status,
        "code_location": "src/VP-V/modules/colour/demo/Demo.h",
        "live_files": live_files,
        "components": {
            "files": {"required": [], "optional": []},
            "hardware": {"required": [], "optional": []},
            "tools": [],
            "config_keys": [],
            "upstream_modules": [],
        },
        "fallback_when_missing": "Slot off = bypass.",
        "fallback_entry": None,
    }


def _build_n8_readme(status="built"):
    return (
        "# Demo module (`colour.demo`)\n\n"
        f"Unit: **colour** | side: processing | order: 99 | status: **{status}** | hook point: LIBPLACEBO\n\n"
        "## Purpose / why\n\n"
        "Synthetic module for N8 README-vs-code compliance verification.\n"
    )


def _seed_clean_n8_repo(root):
    _write_text(
        root,
        "src/VP-V/modules/colour/demo/Demo.h",
        (
            "// Copyright(C) 2026 VP-V contributors\n"
            "// SPDX-License-Identifier: GPL-3.0-only\n"
            "#pragma once\n"
            "namespace Demo { inline int Value() { return 1; } }\n"
        ),
    )
    _write_text(
        root,
        "src/VP-V/modules/colour/demo/MANIFEST.json",
        json.dumps(_build_n8_manifest(), indent=2) + "\n",
    )
    _write_text(
        root,
        "src/VP-V/modules/colour/demo/README.md",
        _build_n8_readme(status="built"),
    )
    _commit_all(root, "clean module base")


class N5PublicHygieneTests(unittest.TestCase):
    def test_n5_script_exists(self):
        self.assertTrue(os.path.isfile(N5_SCRIPT), f"missing N5 checker: {N5_SCRIPT}")

    def test_n5_selftest_exits_zero(self):
        rc, out = _run_tool(N5_SCRIPT, "--selftest")
        self.assertEqual(rc, 0, f"N5 --selftest failed (rc={rc}):\n{out}")

    def test_n5_clean_repo_exits_zero(self):
        with _temp_git_repo() as root:
            _seed_clean_n5_repo(root)
            rc, out = _run_tool(N5_SCRIPT, "--root", root, "--strict")
            self.assertEqual(rc, 0, f"clean mini-repo should pass N5 (rc={rc}):\n{out}")

    def test_n5_rejects_private_d_drive_path(self):
        # Built dynamically so this test source file never contains a literal private path.
        private_d_path = "D" + ":\\private\\secret.txt"
        with _temp_git_repo() as root:
            _seed_clean_n5_repo(root)
            # Positive control on the exact same repo before injecting the violation.
            rc_clean, out_clean = _run_tool(N5_SCRIPT, "--root", root, "--strict")
            self.assertEqual(rc_clean, 0, f"pre-injection baseline must pass (rc={rc_clean}):\n{out_clean}")

            # Negative control: inject a private D-drive backslash path into tracked source.
            _write_text(
                root,
                "src/VP-V/modules/colour/demo/Demo.h",
                (
                    "#pragma once\n"
                    f'namespace Demo {{ inline const char* Leak() {{ return R"({private_d_path})"; }} }}\n'
                ),
            )
            _commit_all(root, "inject private d path")
            rc_bad, out_bad = _run_tool(N5_SCRIPT, "--root", root, "--strict")
            self.assertEqual(rc_bad, 1, f"private D-drive path must fail N5 with exit 1 (rc={rc_bad}):\n{out_bad}")

    def test_n5_rejects_private_user_profile_path(self):
        # Built dynamically so this test source file never contains a literal user-profile path.
        private_user_path = "C" + ":\\Users\\alice\\notes.txt"
        with _temp_git_repo() as root:
            _seed_clean_n5_repo(root)
            rc_clean, out_clean = _run_tool(N5_SCRIPT, "--root", root, "--strict")
            self.assertEqual(rc_clean, 0, f"pre-injection baseline must pass (rc={rc_clean}):\n{out_clean}")

            # Negative control: inject a private user-profile backslash path into tracked source.
            _write_text(
                root,
                "src/VP-V/modules/colour/demo/Demo.h",
                (
                    "#pragma once\n"
                    f'namespace Demo {{ inline const char* Leak() {{ return R"({private_user_path})"; }} }}\n'
                ),
            )
            _commit_all(root, "inject private user path")
            rc_bad, out_bad = _run_tool(N5_SCRIPT, "--root", root, "--strict")
            self.assertEqual(rc_bad, 1, f"private user-profile path must fail N5 with exit 1 (rc={rc_bad}):\n{out_bad}")


class N8ReadmeVsCodeTests(unittest.TestCase):
    def test_n8_script_exists(self):
        self.assertTrue(os.path.isfile(N8_SCRIPT), f"missing N8 checker: {N8_SCRIPT}")

    def test_n8_selftest_exits_zero(self):
        rc, out = _run_tool(N8_SCRIPT, "--selftest")
        self.assertEqual(rc, 0, f"N8 --selftest failed (rc={rc}):\n{out}")

    def test_n8_matching_module_exits_zero(self):
        with _temp_git_repo() as root:
            _seed_clean_n8_repo(root)
            rc, out = _run_tool(N8_SCRIPT, "--root", root, "--strict")
            self.assertEqual(rc, 0, f"matching MANIFEST/README/live_files must pass N8 (rc={rc}):\n{out}")

    def test_n8_rejects_readme_status_drift(self):
        with _temp_git_repo() as root:
            _seed_clean_n8_repo(root)
            rc_clean, out_clean = _run_tool(N8_SCRIPT, "--root", root, "--strict")
            self.assertEqual(rc_clean, 0, f"pre-mutation baseline must pass (rc={rc_clean}):\n{out_clean}")

            # Negative control: README says placeholder while MANIFEST says built.
            _write_text(
                root,
                "src/VP-V/modules/colour/demo/README.md",
                _build_n8_readme(status="placeholder"),
            )
            _commit_all(root, "drift readme status")
            rc_bad, out_bad = _run_tool(N8_SCRIPT, "--root", root, "--strict")
            self.assertEqual(rc_bad, 1, f"README status drift must fail N8 with exit 1 (rc={rc_bad}):\n{out_bad}")

    def test_n8_rejects_missing_live_file(self):
        with _temp_git_repo() as root:
            _seed_clean_n8_repo(root)
            rc_clean, out_clean = _run_tool(N8_SCRIPT, "--root", root, "--strict")
            self.assertEqual(rc_clean, 0, f"pre-mutation baseline must pass (rc={rc_clean}):\n{out_clean}")

            # Negative control: MANIFEST lists a nonexistent live_files entry.
            manifest = _build_n8_manifest(
                status="built",
                live_files=[
                    "src/VP-V/modules/colour/demo/Demo.h",
                    "src/VP-V/modules/colour/demo/NonExistent.h",
                ],
            )
            _write_text(
                root,
                "src/VP-V/modules/colour/demo/MANIFEST.json",
                json.dumps(manifest, indent=2) + "\n",
            )
            _commit_all(root, "add missing live file")
            rc_bad, out_bad = _run_tool(N8_SCRIPT, "--root", root, "--strict")
            self.assertEqual(rc_bad, 1, f"missing live_files entry must fail N8 with exit 1 (rc={rc_bad}):\n{out_bad}")

    def test_n8_rejects_missing_readme(self):
        with _temp_git_repo() as root:
            _seed_clean_n8_repo(root)
            rc_clean, out_clean = _run_tool(N8_SCRIPT, "--root", root, "--strict")
            self.assertEqual(rc_clean, 0, f"pre-mutation baseline must pass (rc={rc_clean}):\n{out_clean}")

            # Negative control: delete README.md from the module folder.
            os.remove(os.path.join(root, "src/VP-V/modules/colour/demo/README.md"))
            _commit_all(root, "remove readme")
            rc_bad, out_bad = _run_tool(N8_SCRIPT, "--root", root, "--strict")
            self.assertEqual(rc_bad, 1, f"missing README.md must fail N8 with exit 1 (rc={rc_bad}):\n{out_bad}")


class RunComplianceRegistrationTests(unittest.TestCase):
    def test_n5_and_n8_registered_in_tools_and_list(self):
        spec = importlib.util.spec_from_file_location("run_compliance_n5n8", RUN_COMPLIANCE_SCRIPT)
        rc_mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(rc_mod)

        by_name = {entry[0]: entry for entry in rc_mod.TOOLS}
        self.assertIn("N5_public_hygiene", by_name, "N5_public_hygiene missing from run_compliance.TOOLS")
        self.assertIn("N8_readme_vs_code", by_name, "N8_readme_vs_code missing from run_compliance.TOOLS")
        self.assertEqual(by_name["N5_public_hygiene"][1], "check_public_hygiene.py")
        self.assertEqual(by_name["N8_readme_vs_code"][1], "check_readme_vs_code.py")

        rc, out = _run_tool(RUN_COMPLIANCE_SCRIPT, "--list")
        self.assertEqual(rc, 0, f"run_compliance.py --list failed (rc={rc}):\n{out}")
        self.assertIn("N5_public_hygiene", out, f"N5_public_hygiene missing from --list output:\n{out}")
        self.assertIn("N8_readme_vs_code", out, f"N8_readme_vs_code missing from --list output:\n{out}")


if __name__ == "__main__":
    unittest.main()
