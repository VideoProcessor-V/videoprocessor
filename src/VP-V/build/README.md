<!-- Copyright(C) 2026 VP-V contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->
# VP-V MSBuild items

Each Bill project imports `build\<MSBuildProjectName>.items.props` through one tagged line placed
before `Microsoft.Cpp.targets` (skipped when the file is absent). VP-V source lists therefore live
here, not in Bill's project files.

- **Naming.** The file name is the project's `MSBuildProjectName` plus `.items.props`. For
  `VideoProcessor-Test.vcxproj.vcxproj` that name is `VideoProcessor-Test.vcxproj`, so the file is
  `VideoProcessor-Test.vcxproj.items.props`.
- **Paths.** Items use `$(MSBuildThisFileDirectory)` so they resolve for solution and standalone builds.
- **Present files.** `VideoProcessor-Test.vcxproj.items.props` (hook-layer tests) and
  `VideoProcessor-ConfigTests.items.props` (Config hook test).
- **Later.** Moving the VP-V items that still sit in Bill's project files into these props is a separate package.
