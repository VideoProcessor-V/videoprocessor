# Review package — what to produce for the external design review

After each implementation step is committed (and after any audit-fix
commit), produce a zip the owner can hand to the design reviewer, who does
not have the checkout and cannot build. The reviewer checks the code
against the design intent; the Code-session auditor checks build, tests
and line-level conformance. Both happen; this file is for the first.

Save as `C:\dev\handoff\stage<S>-step<N>-review.zip`. Keep paths inside the
zip repo-relative so the reviewer sees where each file lives.

## Contents (all required unless marked optional)

```
REVIEW.md                         written by the implementer — see below
diff.patch                        git diff <previous step commit>..<this step commit>
                                  (use `git diff --stat` output at the top of REVIEW.md;
                                  generate the patch with new files included:
                                  git diff <prev>..<this> --binary > diff.patch)
commits.txt                       git log --oneline <baseline tag>..<this step commit>
src/…                             every NEW or CHANGED source file in FULL, at its
                                  repo-relative path (not just the diff) — including
                                  headers, tests, and any shader (.glsl) files
VideoProcessor-test.cfg           the current test config in full
test-output.txt                   the full test-suite output from the final run
                                  (pass/fail counts and any failures verbatim)
build-output.txt                  warnings and errors from the final build, not the
                                  whole log — just the compiler's diagnostic lines
docs/audit/stage<S>-step<N>-*.md  the Code-session audit file(s) for this step,
                                  if an audit has run (optional)
docs/*-answers-*.md               any answers file you were given for this step
                                  (optional; the reviewer usually has it already)
```

Do not include: build artifacts, third-party source, the whole repo, or
files unchanged by the step.

## REVIEW.md — written by the implementer, in plain language

Keep it under one page. Sections, in order:

1. **Step and scope.** Which stage and step; the commits included.
2. **What was built.** Three to six sentences on what the code does, as you
   would explain it to a non-developer.
3. **Decisions I had to make** that the spec did not settle, with what you
   chose and why — one line each. This is the most useful section; if it
   is empty you probably made decisions without noticing.
4. **Deviations from the spec**, if any, each with the reason. Include
   anything the Code-session audit disputed and how it was resolved.
5. **Known gaps.** What is stubbed, deferred, or not yet tested.
6. **How to try it.** The exact config lines to enable this step's
   behaviour on the projector, and what the owner should expect to see.
7. **File map.** One line per new or changed file: path — purpose.

## Size guidance

A step's diff should be readable in one sitting: low thousands of lines at
most. If it is larger, split the zip by sub-area (for example protocol
layer vs its tests) and say so in REVIEW.md. Never summarise code in place
of including it.

## Prompt to produce the package

> Produce the review package for Stage <S> step <N> exactly as described in
> docs\REVIEW-PACKAGE.md: diff.patch between the previous step's commit and
> this one, commits.txt, every new or changed source file in full at its
> repo-relative path, VideoProcessor-test.cfg, test-output.txt,
> build-output.txt, any audit file for this step, and a REVIEW.md written
> for a non-developer with all seven sections. Zip it to
> C:\dev\handoff\stage<S>-step<N>-review.zip and tell me the file list and
> the total line count of the diff.

## Producing packages for several already-completed steps

If more than one step is done before any package has been made, produce
one zip per step, retroactively, from the commit history:

1. `git log --oneline <baseline tag>..HEAD` and identify the commit that
   ended each step (the "Stage S step N: …" commits, plus any "audit
   fixes" commit that belongs to that step).
2. For each step, the diff is from the end of the previous step to the end
   of this step (including its audit-fix commit if any). Step 1's previous
   commit is the end of the last stage's work, or the baseline tag.
3. Build each package exactly as above. REVIEW.md section 1 must list the
   commits included; section 3 ("decisions I had to make") must be written
   per step, not shared.
4. If the work was not committed per step and cannot be separated by
   commit, split by file area instead (protocol layer, controller,
   measurement, hook/compensate), say so at the top of each REVIEW.md, and
   adopt the per-step commit convention from now on.
5. Name the zips `stage<S>-step<N>-review.zip` and list them, with the diff
   line count of each, when done.
