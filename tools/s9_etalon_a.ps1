# BENCHMARK A IN GOAL-ONLY MODE, RE-MEASURED AFTER THE PARTITION FIX.
#
# WHY THIS MEASUREMENT EXISTS
#
# An earlier note compared an arm armed with the repertoire against a goal-only
# arm and concluded the repertoire was worth "the hard part". But the two arms
# did not differ by a single factor: they differed by the repertoire AND by a
# BROKEN MECHANISM. The LDS pass, the one that produced the armed arm's best
# result (2/4, 2 038 555 states), was paralysed when the plan is empty:
# `claims_size = plan.size() + 1` was 1, i.e. ONE partition token for sixteen
# workers, and every deviation was suppressed.
#
# That is fixed (a ClaimTable with a full key, failing open, refusals counted).
# This measurement redoes the goal-only arm on an engine where LDS works.
#
# WHAT TO READ, IN THIS ORDER
#
# 1. THE FIX ITSELF, before any result: in the "bounded-discrepancy search
#    around the plan" table, the number of STATES must GROW with the deviation
#    level. Before the fix it was 26, CONSTANT at every level, and that figure
#    had been taken for a structural fact of the mode. If it is still constant,
#    the fix is not in the binary.
# 2. The `partition` column of the pruning line. Non-zero = the work is CEDED
#    to another worker (shared); that is what could not be told apart from a
#    SUPPRESSION.
# 3. Only then: the board reached, against the armed arm's 2/4.
#
# Reference WITH the repertoire (s7_luna_v6, 1800 s, seed 888):
#   tirages 1 246 s / 12,66 M tirages / best 1/4
#   LDS at 0 deviations 2 038 555 states / 294 s / best 2/4   <-- THE BEST
#   board atteint : 1 Liger + Bagooska ; MANQUE 2x Liger. 0 erreur de core.
#
# The target is REACHABLE (ygocombo #105, 108 steps), and that is what makes
# this benchmark worth having.
param([int]$Ms = 1800000, [string]$Seed = '888')

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
$repo = "D:\ProjectIgnis\repositories"
$gabarit = "D:\ProjectIgnis\replay\_LastReplay.yrpX"

$out = "s9_A_c1"
Write-Output "--- etalon A but seul, apres C1 -> $out"
& .\bin\Release\combosolver.exe $gabarit `
    --scriptdir "D:\ProjectIgnis\replay2video\deps\compat_2026-08" `
    --scriptdir ..\deps\scripts_2026-04-13\script `
    --scriptdir "$repo\delta-bagooska\script" `
    --scriptdir "$repo\delta-puppet\script" `
    --deck "D:\ProjectIgnis\deck\Lunalight.ydk" `
    --hand "57103969|57103969|57103969" `
    --no-ref `
    --target 54701958 --target 54701958 --target 54701958 `
    --target "90590304@DEF" `
    --max-decisions 700 `
    --resolve 4731783 --resolve 2344618 --resolve 47705572 `
    --summon-min "54701958:3" `
    --hint 35618217 --hint 24550676 --hint 100460013 --hint 24094653 `
    --hint 48444114 --hint 83190280 --hint 50277355 --hint 14152693 `
    --solve-ms $Ms --seed $Seed --finisher levin --archive-k 24 `
    --outdir $out *> "$out.log"
Write-Output "    EXIT=$LASTEXITCODE"
Write-Output "TERMINE"
