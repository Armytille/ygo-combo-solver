# COMPLETE ALPHA DIAL, ON A SINGLE BINARY.
#
# WHY REPLAY EVERYTHING RATHER THAN EXTEND
#
# The low dial (1/2/3/5/8) ran on the binary from before the audit batch. Gating
# the novelty in PolicyRollout (`--novelty 0` did not turn novelty off on the
# NRPA side) changed the THROUGHPUT: +20 % expansions at equal budget on the
# control. Extending the dial on the new binary would therefore compare arms
# that did not explore at the same speed. A dial is read as a block or not at
# all.
#
# The control on THIS binary already exists: s9_ctrl.log (42 expansions, b=0,
# EXHAUSTED on the control; 6/5/4/5 = 20 cumulated cards). This script serves
# the nine alpha arms.
# THE TWO QUESTIONS
#
# 1. Where is the optimum on the CORRECTED scale? The equivalence with the
#    earlier dial is alpha_new = alpha_old * h0/8, i.e. x0.5 to x0.75 (h0 is 4
#    6 aux racines comparees, et non 8). L'optimum de la session 8 (alpha_s8=25)
#    to 6 on the roots that weigh), so the optimum should fall near 12-19.
#
# 2. DOES THE MECHANISM SWITCH ITSELF OFF FROM ABOVE? On the control, `rr`
#    alpha monte : 123, 118, 118, 83, 3 pour alpha = 1, 2, 3, 5, 8. Si `rr`
#    collapses when alpha grows; if it reaches zero beyond a point, the high
#    arms measure an INERT rerooter rather than a winning one, which would
#    EXPLAIN the earlier marginal "gain" (a return to the control) instead of
#    confirming it. Read `rr` together with the cumulative, never alone.
#
# CORRECTNESS CHECK, in every arm: "appr0 backtrack 0" must return 42
# expansions, b=0, EXHAUSTED. A cost function changes the ORDER of the
# expansions, not the reachable set; and b=0 says no ceiling bit, without which
# "EXHAUSTED" would not be a proof of absence.
param([int]$Ms = 300000,
      [int]$FinMin = 260000,
      [string]$Seed = '888',
      [string]$Approche = 's8_B_T_s888\best_approach_7of8.yrp')

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
$ref = "D:\ProjectIgnis\replay\synchron handrip 2.yrpX"

foreach ($a in @(1, 2, 3, 5, 8, 12, 16, 20, 28)) {
    $out = "s9b_F_A$a"
    Write-Output "--- bras alpha=$a -> $out"
    & .\bin\Release\combosolver.exe $ref `
        --scriptdir ..\deps\scripts_2026-04-13\script `
        --start $ref --no-plan --no-nrpa `
        --approach $Approche --finisher-min $FinMin `
        --solve-ms $Ms --seed $Seed `
        --no-chain Zalen --no-chain "Crystal Wing" `
        --reroot-h $a --outdir $out *> "$out.log"
    Write-Output "    EXIT=$LASTEXITCODE"
}
Write-Output "TERMINE"
