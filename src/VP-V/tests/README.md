# VP-V tests

VP-V unit tests. Each runs on the CPU unless its name says GPU.

- **Build.** `VideoProcessor-Test` compiles the files in this folder, under the filter "VP-V".
- **Run.** Run them single-threaded, with
  `vstest.console.exe x64\Release\VideoProcessor-Test.dll /TestCaseFilter:FullyQualifiedName~PipelineSkeletonTests /Logger:trx`.
- **Current file:**
  - `PipelineSkeletonTests.cpp` covers the slot table, the zero-cost rule, the entrance order
    and cache, the reorder validator (valid and invalid orders), fingerprint keys, the Original
    pathway, the output-hook order, per-axis vectors, the delay budget and slot timing.
- **Module tests.** A module's own standalone tests go in `src/VP-V/modules/<unit>/<slot>/tests/`
  as the module migrates.
- **VP hook tests moved out of Bill's LibplaceboLutParserTests.cpp** (feat/lut-tests-out):
  the VP-0173 validation filter in Bill's `docs/VP-0173-validation.md` no longer reaches them.
  To run them with that check, add `|FullyQualifiedName~HookTests` to its `/TestCaseFilter`
  (Bill's document itself stays unchanged).
