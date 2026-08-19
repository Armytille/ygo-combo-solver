# EXTENSION OF THE ALPHA DIAL UPWARDS.
#
# WHY THIS SECOND DIAL EXISTS
#
# The first one (tools/s9_ab_alpha.ps1) was centred on wrong arithmetic. It
# assumed h_root = 1 everywhere after the fix, because the finisher starts from
# a backtrack state "already at 7/8 cards". The h0= column, added by the same
# fix, shows that is false: h_root is 1 at the "backtrack 0" root (the one that
# EXHAUSTS in 42 expansions and weighs nothing in the comparison) but 4, 5 and 6
# at backtracks 10, 20/45 and 30, which are the four that count.
# racines effectivement comparees.
#
# So the correct equivalence between the two scales is:
#
#     inv_w = exp(alpha * h(n) / h_root)
#     session 8 : h_root = 8 (constante)   ->  coefficient effectif alpha_s8 / 8
#     session 9 : h_root = h(racine)       ->  coefficient effectif alpha_s9 / h0
#     equivalence :  alpha_s9 = alpha_s8 * h0 / 8,  soit x0,5 a x0,75
#
# The 1/2/3/5/8 dial therefore covers old-scale equivalents of ~1.5 to ~13: it
# is ENTIRELY BELOW the point where the earlier session saw its gain (alpha_old
# = 25, new-scale equivalent ~12.5 to 18.75). It measures the low tail, not the
#
# optimum. This dial goes looking for the optimum where it should be, and one
# notch past it so that it is BRACKETED rather than merely reached (a dial is
# extended, and an isolated point proves nothing):
#
#     alpha_s9   12    16    20    28
#     equiv. s8  ~16   ~21   ~27   ~37
#
# Setup identical to the first dial: benchmark 0, DETERMINISTIC A/B. Same free
# check: "appr0 backtrack 0" must return 42 expansions, b=0, EXHAUSTED in
# chaque bras.
param([int]$Ms = 300000,
      [int]$FinMin = 260000,
      [string]$Seed = '888',
      [string]$Approche = 's8_B_T_s888\best_approach_7of8.yrp')

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
$ref = "D:\ProjectIgnis\replay\synchron handrip 2.yrpX"

$bras = @(
    @{ nom = 'A12'; arg = @('--reroot-h', '12') },
    @{ nom = 'A16'; arg = @('--reroot-h', '16') },
    @{ nom = 'A20'; arg = @('--reroot-h', '20') },
    @{ nom = 'A28'; arg = @('--reroot-h', '28') }
)

foreach ($b in $bras) {
    $out = "s9_F_$($b.nom)"
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
