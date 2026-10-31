# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
<#
Dot-source this file to get Get-VpTestFilter: the vstest /TestCaseFilter text for a test set, produced
by tools/gen_test_filters.py from the manifest tools/test-categories.json. Scripts call it instead of
embedding a copy of a filter. There is no fallback: when python or the generator fails, or prints
nothing, this throws, so a stale hand-written filter can never be used by accident.

  . (Join-Path $PSScriptRoot 'test_filters.ps1')         # from a script in tools/
  Get-VpTestFilter -Kind cpu                              # everything that may run without a GPU
  Get-VpTestFilter -Kind gpu                              # excluded categories (hardware, on-screen, meter, projector)
  Get-VpTestFilter -Kind gpu -Categories GpuHardware,GpuOnScreen
  Get-VpTestFilter -Kind gpuwarp                          # WARP-capable GPU tests; run with VP_TEST_GPU=hardware too
  Get-VpTestFilter -Kind gpuall                           # gpu | gpuwarp
#>
function Get-VpTestFilter {
    param(
        [Parameter(Mandatory)][ValidateSet('cpu', 'gpu', 'gpuwarp', 'gpuall')][string]$Kind,
        [string[]]$Categories = @()
    )
    $generator = Join-Path $PSScriptRoot 'gen_test_filters.py'
    if (-not (Test-Path $generator)) { throw "test filter generator missing: $generator" }
    if (-not (Get-Command python -ErrorAction SilentlyContinue)) { throw "python not found: needed to generate the test filter ($generator); no fallback filter is used" }
    function Invoke-Generator([string[]]$GenArgs) {
        $text = & python -I $generator @GenArgs 2>&1 | ForEach-Object { "$_" }
        if ($LASTEXITCODE -ne 0) { throw "gen_test_filters.py $($GenArgs -join ' ') failed (exit $LASTEXITCODE): $($text -join ' ')" }
        $filter = ($text -join '').Trim()
        if (-not $filter) { throw "gen_test_filters.py $($GenArgs -join ' ') printed an empty filter" }
        $filter
    }
    $gpuArgs = @('--gpu')
    if ($Categories.Count -gt 0) { $gpuArgs += @('--categories', ($Categories -join ',')) }
    switch ($Kind) {
        'cpu'     { Invoke-Generator @('--cpu') }
        'gpu'     { Invoke-Generator $gpuArgs }
        'gpuwarp' { Invoke-Generator @('--gpuwarp') }
        'gpuall'  { (Invoke-Generator $gpuArgs) + '|' + (Invoke-Generator @('--gpuwarp')) }
    }
}
