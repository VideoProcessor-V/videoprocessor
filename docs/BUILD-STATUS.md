# Build status notes

Things that look like build problems but are not.

## Windows Application Control blocks a freshly linked test binary

On the development laptop, Windows Application Control (Smart App Control)
intermittently refuses to start a just-linked test binary:

- `VideoProcessorBetaTests.exe`: bash reports "Permission denied" (exit 126);
  cmd says the command "is not recognized";
- `VideoProcessor-Test.dll`: vstest reports "Failed to load the test assembly
  or its dependencies" for every test;
- `VideoProcessorConfigTests.exe`: sometimes the same.

The build is fine. Relink the project (delete the output and build it, or
`/t:<project>:Rebuild`); if it is still blocked, wait and retry later - on
2026-10-12 the Beta exe ran again after a while with nothing changed. Never
change the Smart App Control / Application Control setting (standing rule).

## Stale incremental-link files (LNK1103 / LNK1318)

`ProcessCpuUsageWindowTests.obj : fatal error LNK1103: debugging information
corrupt` or LNK1318 on an incremental build: delete
`src/VideoProcessor-Test/x64/Release/ProcessCpuUsageWindowTests.obj` and the
`*.iobj` / `*.ipdb` there, then build again.

## Shader-compiler timing notes

libplacebo logs "Spent N ms translating HLSL to DXBC (slow!)" when a shader
compiles slowly (WARP, a loaded machine). Not a failure; the GPU tests'
clean-log checks ignore it. VP keeps libplacebo's compiled-shader cache.
