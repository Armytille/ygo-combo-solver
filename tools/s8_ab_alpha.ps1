# Extension of the alpha DIAL of sqrt-LTS-H.
#
# The first dial (8 / 15 / 25) returned 18 / 20 / 21 cumulated cards over the
# four backtracks the budget allowed, against 20 for the control. Three arms
# ordered by a single setting giving three ordered results is the shape of a
# real result, and it calls for one more point above. The window computed by
# ForecastSearchCost stopped at 28.8, so we look at 40 and 60, i.e. OUTSIDE it,
# to find out whether the gain continues or whether the window was the right
# prediction.
#
# Same deterministic setup as tools/s8_ab_finisseur.ps1: same roots
# (--approach), same policy (--no-nrpa, hence empty everywhere), same budget.
param([int]$Ms = 300000,
      [int]$FinMin = 260000,
      [string]$Seed = '888',
      [string]$Approche = 's8_B_T_s888\best_approach_7of8.yrp')

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
$ref = "D:\ProjectIgnis\replay\synchron handrip 2.yrpX"

foreach ($a in @('40', '60')) {
    $out = "s8_F_H$a"
    Write-Output "--- finisseur bras H$a -> $out"
    & .\bin\Release\combosolver.exe $ref `
        --scriptdir ..\deps\scripts_2026-04-13\script `
        --start $ref --no-plan --no-nrpa `
        --approach $Approche --finisher-min $FinMin `
        --solve-ms $Ms --seed $Seed `
        --no-chain Zalen --no-chain "Crystal Wing" `
        --reroot-h $a --outdir $out *> "$out.log"
    Write-Output "    EXIT=$LASTEXITCODE"
}
Write-Output "ALPHA_TERMINE"
