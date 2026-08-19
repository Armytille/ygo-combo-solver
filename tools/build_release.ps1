# Builds the release binary and packages it into dist/.
#
# The binary is statically linked: it depends on KERNEL32 alone, so the .exe is
# the whole deliverable. Everything it reads at runtime comes from the EDOPro
# installation named by --workdir or COMBOSOLVER_WORKDIR; nothing is resolved
# relative to the executable.
#
# PGO is applied, and that is the reason this script exists rather than a plain
# MSBuild. MSVC applies the profile at LINK time through the _LINK_ environment
# variable, so ANY ordinary MSBuild silently relinks without /USEPROFILE and
# throws it away. A release must therefore be built here, and rebuilt here after
# every engine change.
#
# What it is worth, measured on the profile this script produces: 11.06 -> 10.55
# us per Process call, a 4.6 % median gain over three interleaved repetitions at
# an identical call count (49 280 calls in all six runs, so this is a fixed-work
# comparison), distributions disjoint. That is well under the 21.4 % recorded in
# docs/combo-solver-design.md 9.19 (d), which was trained on seven regimes
# including the benchmark it was then measured on; the narrow profile below buys
# a narrower gain. The judge is us per call, never the rollout counter, whose
# dispersion at a fixed seed swallows an effect this size.
#
# Training runs on the replay template shipped in gabarits/ and on nothing else,
# so the build is reproducible on any machine with an EDOPro installation. Three
# regimes, chosen because they exercise different code: startup and faithful
# replay with the arena's snapshot/restore and no search at all; bounded-
# discrepancy search following the recorded line; and the goal-only regime where
# the policy starts uniform and the Levin finisher rebuilds lines from scratch.
#
# Known limit of this profile. The template is a hand test, so the mechanisms
# that need a longer duel stay out of the profile — a training run reports
# `!! DEMANDE mais INERTE ici` for re-entry and for the refined quotas, and the
# internal loop (--rounds) has nothing to report on it. Those paths are
# therefore optimised on inference rather than on measurement. Widening the
# profile means shipping a longer template, not adding flags to the runs below.
#
# Toolchain is discovered through vswhere, never hard-coded.

[CmdletBinding()]
param(
    # EDOPro installation. Falls back to COMBOSOLVER_WORKDIR.
    [string]$Workdir = $env:COMBOSOLVER_WORKDIR,
    # Skip the instrument/train/relink cycle: a plain LTO build, ~6 minutes faster
    # and ~21 % slower per call. For iteration, not for publishing.
    [switch]$NoPgo
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

if (-not $Workdir) {
    Write-Output "!! no EDOPro installation: pass -Workdir <dir>, or set COMBOSOLVER_WORKDIR"
    exit 1
}
# Same rule as CardDB::Load: a root cards.cdb if there is one, plus every .cdb
# found under expansions/ and repositories/. A stock install has no root
# cards.cdb at all, so requiring one would reject a working installation.
$cdb = @('cards.cdb', 'expansions', 'repositories') |
    ForEach-Object { Join-Path $Workdir $_ } |
    Where-Object { Test-Path $_ }
if (-not $cdb) {
    Write-Output "!! $Workdir holds no card database (cards.cdb, expansions/ or repositories/)"
    exit 1
}
$env:COMBOSOLVER_WORKDIR = $Workdir

# --- toolchain ---------------------------------------------------------------

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { Write-Output "!! vswhere not found: is Visual Studio installed?"; exit 1 }
$vsroot = & $vswhere -products * -requires Microsoft.Component.MSBuild -property installationPath -latest
if (-not $vsroot) { Write-Output "!! no Visual Studio installation carrying MSBuild"; exit 1 }

$msbuild = Join-Path $vsroot 'MSBuild\Current\Bin\amd64\MSBuild.exe'
if (-not (Test-Path $msbuild)) { Write-Output "!! MSBuild not found under $vsroot"; exit 1 }

# pgort140.dll must sit next to the instrumented exe for the training runs.
$pgort = Get-ChildItem (Join-Path $vsroot 'VC\Tools\MSVC') -Directory |
    Sort-Object Name -Descending |
    ForEach-Object { Join-Path $_.FullName 'bin\Hostx64\x64\pgort140.dll' } |
    Where-Object { Test-Path $_ } |
    Select-Object -First 1

$exe = 'bin\Release\combosolver.exe'
$gabarit = 'gabarits\etalon_a_lunalight.yrp'
$train = 'obj\pgo-train'

function Invoke-Build([string]$linkFlag, [string]$label) {
    $env:_LINK_ = $linkFlag
    if (Test-Path $exe) { Remove-Item $exe -Force }
    # /nr:false so the MSBuild node processes do not outlive the run holding a
    # stale _LINK_ from a previous invocation.
    & $msbuild build\combosolver.sln /p:Configuration=Release /p:Platform=x64 /m /nr:false /nologo /v:quiet
    $code = $LASTEXITCODE
    $env:_LINK_ = $null
    if ($code -ne 0) { Write-Output "!! build ${label}: EXIT=$code"; exit 1 }
    Write-Output "  $label linked"
}

# --- build -------------------------------------------------------------------

if ($NoPgo) {
    Write-Output "== building (LTO only, no PGO) =="
    Invoke-Build $null 'release'
} else {
    if (-not $pgort) { Write-Output "!! pgort140.dll not found: cannot train"; exit 1 }

    Write-Output "== 1/3 instrumented build =="
    Invoke-Build '/GENPROFILE' 'instrumented'
    Copy-Item $pgort 'bin\Release\' -Force

    Write-Output "== 2/3 training (three regimes, ~3 min) =="
    if (Test-Path $train) { Remove-Item $train -Recurse -Force }

    # a. loading, faithful replay, snapshot/restore stress. No search at all:
    #    this is the only regime that exercises the startup and the self-checks.
    & ".\$exe" $gabarit *> "$train.a.log"
    Write-Output "  a. replay and restore            EXIT=$LASTEXITCODE"

    # b. bounded-discrepancy search around the recorded line, and the writing of
    #    the produced replays.
    & ".\$exe" $gabarit --solve --solve-ms 60000 --outdir "$train\b" *> "$train.b.log"
    Write-Output "  b. bounded-discrepancy search    EXIT=$LASTEXITCODE"

    # c. goal-only regime: the reference's repertoire is set aside, so the policy
    #    starts uniform and the Levin finisher rebuilds lines from scratch. This
    #    is where the search actually spends its time in a real run.
    & ".\$exe" $gabarit --solve --no-plan --finisher levin --finisher-min 40000 `
        --archive-k 24 --solve-ms 90000 --outdir "$train\c" *> "$train.c.log"
    Write-Output "  c. goal-only finisher            EXIT=$LASTEXITCODE"

    Get-ChildItem bin\Release\*.pgc -ErrorAction SilentlyContinue |
        ForEach-Object { Write-Output ("  profile: {0} {1:N1} MB" -f $_.Name, ($_.Length / 1MB)) }

    Write-Output "== 3/3 optimised build =="
    Invoke-Build '/USEPROFILE' 'release (PGO)'
    Remove-Item 'bin\Release\pgort140.dll' -Force -ErrorAction SilentlyContinue
    Remove-Item 'bin\Release\*.pgc' -Force -ErrorAction SilentlyContinue
}

# --- check -------------------------------------------------------------------

Write-Output "== checking the binary is self-contained =="
$dumpbin = Get-ChildItem (Join-Path $vsroot 'VC\Tools\MSVC') -Directory |
    Sort-Object Name -Descending |
    ForEach-Object { Join-Path $_.FullName 'bin\Hostx64\x64\dumpbin.exe' } |
    Where-Object { Test-Path $_ } |
    Select-Object -First 1
if ($dumpbin) {
    $dlls = (& $dumpbin /dependents $exe |
             Select-String -Pattern '^\s+(\S+\.dll)$' |
             ForEach-Object { $_.Matches[0].Groups[1].Value }) | Sort-Object
    Write-Output "  imports: $($dlls -join ', ')"
    $extra = $dlls | Where-Object { $_ -notmatch '^(KERNEL32|ADVAPI32|WS2_32)\.dll$' }
    if ($extra) { Write-Output "  !! unexpected dependency: $($extra -join ', ')"; exit 1 }
}

# --- package -----------------------------------------------------------------

$version = (& git -C $root describe --tags --always --dirty 2>$null)
if (-not $version) { $version = 'dev' }

$dist = Join-Path $root 'dist'
if (Test-Path $dist) { Remove-Item $dist -Recurse -Force }
New-Item -ItemType Directory -Force $dist | Out-Null
Copy-Item $exe $dist
Copy-Item 'README.md' $dist
New-Item -ItemType Directory -Force (Join-Path $dist 'gabarits') | Out-Null
Copy-Item "$gabarit" (Join-Path $dist 'gabarits')

$zip = Join-Path $root "combosolver-$version.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $dist '*') -DestinationPath $zip

Write-Output "== packaged =="
Write-Output ("  {0}  {1:N1} MB" -f $exe, ((Get-Item $exe).Length / 1MB))
Write-Output ("  {0}  {1:N1} MB" -f $zip, ((Get-Item $zip).Length / 1MB))
