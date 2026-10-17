<#
Makes a restored build-output cache safe for an incremental MSBuild run (ci-windows.yml).

The cache holds the x64 Release outputs and every project's intermediate directory from an earlier
CI build, plus x64\Release\.ci-build-commit (the commit it was built from). A fresh checkout gives
every source file the checkout time, which is newer than the cached objects, while a restore gives
the objects their old times; neither says which sources really changed. So:

  - no marker, or a marker commit this clone does not have: the restored outputs are deleted
    (clean build);
  - otherwise every tracked file gets an old fixed time, and the files that differ between the
    cached commit and HEAD (added, modified, renamed) get the current time. MSBuild's own tracking
    (.tlog include lists, command lines) then rebuilds exactly what depends on them;
  - LTCG incremental state (*.iobj, *.ipdb) is deleted, because a stale pair breaks the link
    (LNK1103/LNK1318); the link then runs in full, the compile stays incremental.

  prepare_incremental_build.ps1 -Dirs x64\Release,src\*\x64\Release,3rdparty\*\x64\Release
Prints what it did; the workflow passes the result to the job summary.
#>
param(
    [string[]]$Dirs = @('x64\Release', 'src\*\x64\Release', '3rdparty\*\x64\Release')
)
$ErrorActionPreference = 'Stop'
$root = (git rev-parse --show-toplevel).Trim()
Set-Location $root
$marker = Join-Path $root 'x64\Release\.ci-build-commit'
$restored = @($Dirs | ForEach-Object { Get-Item -Path (Join-Path $root $_) -ErrorAction SilentlyContinue } | Where-Object { $_.PSIsContainer })

function Clear-Restored([string]$why) {
    foreach ($d in $restored) { Remove-Item -LiteralPath $d.FullName -Recurse -Force -ErrorAction SilentlyContinue }
    "Build cache: clean build ($why)."
    exit 0
}

if ($restored.Count -eq 0) { "Build cache: nothing restored (clean build)."; exit 0 }
if (-not (Test-Path $marker)) { Clear-Restored 'no commit marker in the restored outputs' }
$cached = (Get-Content $marker -Raw).Trim()
git cat-file -e "$cached^{commit}" 2>$null
if ($LASTEXITCODE -ne 0) { Clear-Restored "cached commit $cached is not in this clone" }

$old = [DateTime]::new(2001, 1, 1, 0, 0, 0, [DateTimeKind]::Utc)
$now = [DateTime]::UtcNow
$tracked = @(git ls-files)
foreach ($f in $tracked) {
    $p = Join-Path $root $f
    if (Test-Path -LiteralPath $p -PathType Leaf) { [IO.File]::SetLastWriteTimeUtc($p, $old) }
}
$changed = @(git diff --name-only --no-renames --diff-filter=ACMRT $cached HEAD | Where-Object { $_ })
foreach ($f in $changed) {
    $p = Join-Path $root $f
    if (Test-Path -LiteralPath $p -PathType Leaf) { [IO.File]::SetLastWriteTimeUtc($p, $now) }
}
$ltcg = @($restored | ForEach-Object { Get-ChildItem -LiteralPath $_.FullName -Recurse -File -Include *.iobj, *.ipdb -ErrorAction SilentlyContinue })
$ltcg | Remove-Item -Force -ErrorAction SilentlyContinue
$short = $cached.Substring(0, [Math]::Min(8, $cached.Length))
"Build cache: incremental from $short; $($tracked.Count) tracked files dated $($old.ToString('yyyy-MM-dd')), $($changed.Count) changed since then dated now; $($ltcg.Count) LTCG state files removed."
exit 0
