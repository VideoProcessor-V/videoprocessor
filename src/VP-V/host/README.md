# VP-V host fragments

Why this folder exists: refs/112 (minimal seams) P2 cuts the VP-V lines inside Bill's renderer
(`LibplaceboVideoRenderer.cpp`) to a few fixed entry points. Code that must be compiled inside
Bill's translation unit (it names members of his file-local `Impl`) lives here and is pulled in
by ONE tagged line at FILE scope. Nothing here is a stand-alone header: each file is included
exactly once, at the place named in its header comment.

| File | Included by | What it carries |
|---|---|---|
| `LvrHostAccess.inl` | one `// VPV-HOOK` line at file scope right after the `Impl` struct | the eleven module `.inl` includes with the out-of-line `Impl` member bodies, same order, same place in the translation unit (CC slice 12.2) |

## Design goal, philosophy and decisions
- Goal: Bill's file keeps one line per fragment, so the re-graft carries one anchor instead of
  dozens (refs/112 s2 P2; owner end-state target 20-30 renderer lines, doc 91 A/H 2026-11-11 22:44;
  owner answer Q5 2026-11-21T09:44:08Z, lead21\owner-batch-2.md:21-26).
- File scope only: no VP-V file is included inside a Bill class or function body (CC-audit-1, rule 19;
  the honest H2 metric counts any such include's lines against the includer).
- Interim, not the end state: the bodies become free functions over `Vpv::HostAccess` (refs/112 P2),
  and this fragment then goes. Its expanded lines are reported separately in every CC commit as
  "file-scope .inl lines compiled in Bill TU".
