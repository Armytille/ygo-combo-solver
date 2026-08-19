# RECIPE GRAPH: the deciding measurement.
#
# THE QUESTION, and it comes before "does it win".
#
# The diagnosis says the lock is no longer the search algorithm, it is in `h`.
# Our `h` (the number of missing target cards) is zero over ~90 % of the line,
# and a decomposition mechanism can decompose nothing on a flat landscape. The
# recipe graph is meant to make `h` informative: it counts the SUMMONS left,
# intermediate materials included, so it DECREASES when an intermediate piece is
# assembled, where the flat `h` does not move.
#
# Before asking whether that `h` makes the search win, one has to know WHETHER
# IT DIFFERS FROM THE FLAT `h` (instrument before calibrating; a mechanism can
# be live and have no effect). Hence the three arms:
#
#   T    temoin, aucun graphe                       -> h plat
#   M    --recipes 0: the graph is FED and MEASURED but does NOT enter the
#        cost. The run is therefore IDENTICAL to the control by construction,
#        and the `hR` column says what the graph WOULD have said. That is the
#        measurement that decides whether it is worth wiring in.
#   P1   --recipes 1: the recipe distance weighs in h.
#   P2   --recipes 2
#
# WHAT TO READ, in this order:
#
# 1. `rec=` (summons observed). At ZERO the graph is empty and the distance is
#    exactly the flat `h`: the mechanism is INERT and the P arms measured
#    nothing. That is the first thing to check.
# 2. `hR` (mean recipe distance) against the control's |missing target|. If
#    hR ~ the number of missing cards, the graph learned nothing beyond the
#    count: the landscape stayed flat and the work failed at its declared goal,
#    whatever the result in cards.
#    If hR > that count, the graph sees INTERMEDIATE SUMMONS the flat `h`
#    ignored, and that is the signal being looked for.
# 3. Only then: the cumulated cards of the P arms against the control.
#
# Setup: benchmark 0, DETERMINISTIC A/B (roots imposed by --approach, empty
# policy by --no-nrpa, goal-only by --no-plan). Free check in every arm: "appr0
# backtrack 0" must return 42 expansions, b=0, EXHAUSTED.
param([int]$Ms = 300000,
      [int]$FinMin = 260000,
      [string]$Seed = '888',
      [string]$Approche = 's8_B_T_s888\best_approach_7of8.yrp')

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
$ref = "D:\ProjectIgnis\replay\synchron handrip 2.yrpX"

$bras = @(
    @{ nom = 'T';  arg = @() },
    @{ nom = 'M';  arg = @('--recipes', '0') },
    @{ nom = 'P1'; arg = @('--recipes', '1') },
    @{ nom = 'P2'; arg = @('--recipes', '2') }
)

foreach ($b in $bras) {
    $out = "s9r_$($b.nom)"
    Write-Output "--- recettes bras $($b.nom) -> $out"
    & .\bin\Release\combosolver.exe $ref `
        --scriptdir ..\deps\scripts_2026-04-13\script `
        --start $ref --no-plan --no-nrpa `
        --approach $Approche --finisher-min $FinMin `
        --solve-ms $Ms --seed $Seed `
        --no-chain Zalen --no-chain "Crystal Wing" `
        @($b.arg) --outdir $out *> "$out.log"
    Write-Output "    EXIT=$LASTEXITCODE"
}
Write-Output "TERMINE"
