# CHAINE DE MESURES DE FIN DE SESSION 9, sequentielle.
#
# One process at a time: the project's discipline requires it (never two
# measurements in parallel), and the file lock on the binary enforces it
# mechanically anyway.
#
# 1. HEALTH. The gate of any engine change. The "counting + recipe graph" batch
#    must change NOTHING when --recipes is absent: the diff against the baseline
#    must contain durations only.
#
# 2. COUNTING, a free validation. Benchmark A already carries
#    `--summon-min "54701958:3"` WRITTEN BY HAND. The counting derived from the
#    target board must find exactly that constraint again: 3x Liger Dancer =
#    three Fusion summon EVENTS. If it does, the first step is validated against
#    a truth written by a human; if it does not, the counting is wrong and so is
#    the graph built on it. 30 s is enough: only the report is read, not the
#    search.
#
# 3. GRAPHE DE RECETTES, A/B deterministe sur l'etalon 0 (tools/s9_recettes.ps1).
#
# 4. BENCHMARK A goal-only, re-measured (tools/s9_etalon_a.ps1).
param([switch]$SkipSante)

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
$ref = "D:\ProjectIgnis\replay\synchron handrip 2.yrpX"

if (-not $SkipSante) {
    Write-Output "=== 1. SANTE ==="
    & .\bin\Release\combosolver.exe $ref `
        --scriptdir ..\deps\scripts_2026-04-13\script `
        --solve --solve-ms 60000 --outdir s9_sante_rec `
        --no-chain Zalen --no-chain "Crystal Wing" *> s9_sante_rec.log
    Write-Output "    EXIT=$LASTEXITCODE"
}

Write-Output "=== 2. COMPTAGE (validation contre la contrainte ecrite a la main) ==="
$repo = "D:\ProjectIgnis\repositories"
& .\bin\Release\combosolver.exe "D:\ProjectIgnis\replay\_LastReplay.yrpX" `
    --scriptdir "D:\ProjectIgnis\replay2video\deps\compat_2026-08" `
    --scriptdir ..\deps\scripts_2026-04-13\script `
    --scriptdir "$repo\delta-bagooska\script" `
    --scriptdir "$repo\delta-puppet\script" `
    --deck "D:\ProjectIgnis\deck\Lunalight.ydk" `
    --hand "57103969|57103969|57103969" `
    --no-ref `
    --target 54701958 --target 54701958 --target 54701958 `
    --target "90590304@DEF" `
    --max-decisions 700 --derive-summon-min `
    --solve-ms 30000 --seed 888 --outdir s9_comptage *> s9_comptage.log
Write-Output "    EXIT=$LASTEXITCODE"

Write-Output "=== 3. GRAPHE DE RECETTES (etalon 0) ==="
& .\tools\s9_recettes.ps1

Write-Output "=== 4. ETALON A but seul, apres C1 ==="
& .\tools\s9_etalon_a.ps1

Write-Output "CHAINE TERMINEE"
