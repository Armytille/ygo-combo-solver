# A/B OF THE REROOTER WITH IDENTICAL ROOTS.
#
# WHY THIS SETUP REPLACES THE END-TO-END A/B
#
# --reroot and --reroot-h touch ONLY RunLevin, the finisher. Everything before
# it (greedy probe, NRPA rollouts, archive) is the same code in every arm and
# differs only by the timing of the exchanges between workers. Comparing whole
# runs therefore mostly reads the rollouts' noise; rollout counters were already
# established not to be A/B metrics.
#
# Here the finisher's roots are IMPOSED and identical in every arm: --approach
# serves the same approach to all, --finisher-min gives it most of the budget,
# and the "appr0 backtrack N" rows of the report compare one by one
# (expansions, best overlap, EXHAUSTED or budget). That is the only place the
# mechanism acts, hence the only place it should be measured.
#
# --no-plan is kept: we stay in the GOAL-ONLY regime (suffixes to rebuild from
# scratch), which is the regime where the cost forecast announced the sqrt-LTS
# gain.
#
# --no-nrpa closes the last noise channel. The policy served to the finisher is
# the MERGE of the weights learned by the NRPA workers, so it differs from one
# arm to the next by the timing of the exchanges alone. Without NRPA it is
# EMPTY in every arm, hence identical. Identical roots + identical policy = the
# A/B becomes DETERMINISTIC: whatever moves in the "appr0 backtrack N" table can
# only come from the cost function, i.e. from the rerooter. That is the price:
# the mechanism is measured on a uniform policy, which is precisely the starting
# point of goal-only mode.
param([int]$Ms = 300000,
      [int]$FinMin = 260000,
      [string]$Seed = '888',
      [string]$Approche = 's8_B_T_s888\best_approach_7of8.yrp')

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
$ref = "D:\ProjectIgnis\replay\synchron handrip 2.yrpX"

$bras = @(
    @{ nom = 'T';   arg = @() },
    @{ nom = 'R';   arg = @('--reroot') },
    @{ nom = 'H8';  arg = @('--reroot-h', '8') },
    @{ nom = 'H15'; arg = @('--reroot-h', '15') },
    @{ nom = 'H25'; arg = @('--reroot-h', '25') }
)

foreach ($b in $bras) {
    $out = "s8_F_$($b.nom)"
    Write-Output "--- finisseur bras $($b.nom) -> $out"
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
