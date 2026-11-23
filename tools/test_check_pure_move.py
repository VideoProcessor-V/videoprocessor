# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
"""TQ-RG-P1: check_pure_move.py had no tests. norm() is tested directly; the commit check runs the script in a temp git
repository (a base commit, then a move / an edit / a deletion) and asserts the exit code and the printed counts."""
import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

_dir = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(_dir, "check_pure_move.py")
_spec = importlib.util.spec_from_file_location("check_pure_move", SCRIPT)
P = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(P)   # importable: main() runs only under __main__

BODY = "int Alpha(int x)\n{\n\tint y = x * 2;\n\treturn y + 1;\n}\n"   # 3 code lines (the braces are dropped)
KEEP = "int Keep = 1;\nint Keep2 = 2;\n"   # stays in a.h so git sees a move, not a rename


class Norm(unittest.TestCase):
    def setUp(self):
        P.amap.clear()

    def test_blank_comment_and_brace_only_lines_are_dropped(self):
        for line in ("", "   ", "// a comment", "{", "}", "};", "struct Foo"):
            self.assertIsNone(P.norm(line), repr(line))

    def test_whitespace_and_trailing_comments_are_ignored(self):
        self.assertEqual(P.norm("  int  y = x*2;   // doubled"), P.norm("int y = x * 2;"))
        self.assertEqual(P.norm("int y = x * 2;"), "inty=x*2;")

    def test_impl_pointer_qualifiers_and_leading_static_are_stripped(self):
        self.assertEqual(P.norm("m_impl->count = 1;"), P.norm("count = 1;"))
        self.assertEqual(P.norm("impl.count = 1;"), P.norm("count = 1;"))
        self.assertEqual(P.norm("vectorEngine.mode = 1;"), P.norm("mode = 1;"))
        self.assertEqual(P.norm("Foo::Impl::Run();"), P.norm("Run();"))
        self.assertEqual(P.norm("static int k = 3;"), P.norm("int k = 3;"))

    def test_a_real_change_is_not_normalised_away(self):
        self.assertNotEqual(P.norm("int y = x * 2;"), P.norm("int y = x * 3;"))
        self.assertNotEqual(P.norm("return y + 1;"), P.norm("return y + 2;"))

    def test_an_alias_name_maps_back_to_its_target(self):
        P.amap["mode"] = "m"   # field -> alias name: a use of the field reads as the alias
        self.assertEqual(P.norm("use(mode);"), "use(m);")
        self.assertEqual(P.norm("use(mode);"), P.norm("use(m);"))


class CommitCheck(unittest.TestCase):
    """Needs git on PATH: without it every test errors (it is not skipped)."""

    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.dir, True)
        self.git("init", "-q")
        self.git("config", "user.email", "t@example.com")
        self.git("config", "user.name", "t")
        self.git("config", "core.autocrlf", "false")
        self.write("src/a.h", BODY + KEEP)
        self.commit("base")

    def git(self, *args):
        done = subprocess.run(["git", *args], cwd=self.dir, capture_output=True, text=True, encoding="utf-8")
        self.assertEqual(done.returncode, 0, done.stderr)
        return done.stdout.strip()

    def write(self, rel, text):
        path = os.path.join(self.dir, *rel.split("/"))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)

    def commit(self, message):
        self.git("add", "-A")
        self.git("commit", "-q", "-m", message)
        return self.git("rev-parse", "HEAD")

    def check(self, *args):
        done = subprocess.run([sys.executable, "-I", SCRIPT, *args], cwd=self.dir, capture_output=True,
                              text=True, encoding="utf-8")
        return done.returncode, done.stdout

    def test_a_pure_move_passes(self):
        self.write("src/a.h", KEEP)
        self.write("src/b.h", BODY)
        rc, out = self.check(self.commit("move"))
        self.assertEqual(rc, 0, out)
        self.assertIn("removed code lines 3 mismatches 0", out)
        self.assertIn("TOTAL mismatches 0", out)

    def test_a_move_with_re_indentation_and_a_qualifier_change_passes(self):
        self.write("src/a.h", "int Alpha(int x)\n{\n\tint y = m_impl->x * 2;\n\treturn y + 1;\n}\n" + KEEP)
        self.commit("qualified base")
        self.write("src/a.h", KEEP)
        self.write("src/b.h", "int Alpha(int x)\n  {\n    int y = x * 2;   // doubled\n    return y + 1;\n  }\n")
        rc, out = self.check(self.commit("move"))
        self.assertEqual(rc, 0, out)

    def test_an_edit_during_the_move_is_a_mismatch(self):
        self.write("src/a.h", KEEP)
        self.write("src/b.h", BODY.replace("y + 1", "y + 2"))
        rc, out = self.check(self.commit("move and edit"))
        self.assertEqual(rc, 1, out)
        self.assertIn("MISMATCH return y + 1;", out)
        self.assertIn("mismatches 1", out)

    def test_a_deletion_without_a_re_add_is_a_mismatch(self):
        self.write("src/a.h", KEEP)
        rc, out = self.check(self.commit("delete"))
        self.assertEqual(rc, 1, out)
        self.assertIn("removed code lines 3 mismatches 3", out)

    def test_a_pure_addition_passes(self):
        self.write("src/c.h", "int Added = 3;\n")
        rc, out = self.check(self.commit("add"))
        self.assertEqual(rc, 0, out)
        self.assertIn("removed code lines 0 mismatches 0", out)

    def test_only_src_is_considered(self):
        self.write("docs/note.md", "text\n")
        self.commit("doc base")
        self.git("rm", "-q", "docs/note.md")
        rc, out = self.check(self.commit("delete a doc"))
        self.assertEqual(rc, 0, out)
        self.assertIn("removed code lines 0 mismatches 0", out)

    def test_alias_deletion_is_a_mismatch_unless_allowed(self):
        self.write("src/d.h", "int& mode = vectorEngine.mode;\n" + KEEP)
        self.commit("alias base")
        self.write("src/d.h", KEEP)
        sha = self.commit("drop the alias")
        rc, out = self.check(sha)
        self.assertEqual(rc, 1, out)
        self.assertIn("MISMATCH int& mode = vectorEngine.mode;", out)
        rc, out = self.check(sha, "--allow-alias-deletion")
        self.assertEqual(rc, 0, out)

    def test_several_commits_total_their_mismatches(self):
        self.write("src/a.h", KEEP)
        first = self.commit("delete")
        self.write("src/e.h", "int K = 1;\n")
        self.commit("add e")
        self.git("rm", "-q", "src/e.h")
        second = self.commit("delete e")
        rc, out = self.check(first, second)
        self.assertEqual(rc, 1, out)
        self.assertIn("TOTAL mismatches 4", out)


if __name__ == "__main__":
    unittest.main()
