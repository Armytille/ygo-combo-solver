# A/B OF THE NRPA NESTING LEVEL, AT EQUAL BUDGET.
#
# WHY THIS MEASUREMENT WAS MISSING
#
# `nrpa_level = (budget > 180000.0) ? 3 : 2` used to decide on its own, and that
# is not a fine setting: it changes the cost of a level call from ~576 rollouts
# to ~13 824, i.e. THE SAMPLING ALGORITHM itself. No measurement backed it.
# ne l'adossait.
#
# Worse, the threshold fell exactly on the dividing line between the commands
# compared in several sessions: a 600 s run WITHOUT --finisher-min leaves 420 s
# to the rollouts (0.7 x 600) hence level 3, and the SAME run WITH
# --finisher-min 420000 leaves ~180 hence level 2. Several published A/Bs were
# therefore comparing two algorithms while believing they compared two settings.
#
# The --nrpa-level flag makes the choice explicit and the effective level is
# printed in every case. What is left is which one is better, at equal budget.
#
# SETUP. Benchmark B (synchron, reference NEUTRALISED by --start on itself plus
# --no-plan), two arms, same seed, same budget, sequential. The only factor that
# changes is the level.
#
# WHAT TO READ. The STRUCTURAL fact is the best overlap reached (best n/8) and
# the conversions, NOT the rollout counters: at a fixed seed the run noise
# dominates those counters, and a level 3 mechanically makes fewer level calls
# than a level 2 without that saying anything about quality.
param([int]$Ms = 600000, [string]$Seed = '888')

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
$ref = "D:\ProjectIgnis\replay\synchron handrip 2.yrpX"

foreach ($lvl in @(2, 3)) {
    $out = "s9_lvl$lvl"
    Write-Output "--- niveau NRPA $lvl -> $out"
    & .\bin\Release\combosolver.exe $ref `
        --scriptdir ..\deps\scripts_2026-04-13\script `
        --start $ref --no-plan `
        --nrpa-level $lvl `
        --solve-ms $Ms --seed $Seed `
        --no-chain Zalen --no-chain "Crystal Wing" `
        --outdir $out *> "$out.log"
    Write-Output "    EXIT=$LASTEXITCODE"
}
Write-Output "TERMINE"
