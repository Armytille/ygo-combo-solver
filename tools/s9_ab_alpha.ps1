# ALPHA DIAL OF THE SOFT REROOTER, REDONE ON THE CORRECTED SCALE.
#
# WHY THE EARLIER DIAL IS TO BE THROWN AWAY
#
# inv_w = exp(alpha * h(n) / h_root), Eq. 7 of arXiv:2605.30664, where h_root
# h A LA RACINE DE LA RECHERCHE COURANTE. La session 8 y mettait
# used to be |target| + Sigma resolve_min, i.e. h at the start of the DUEL: a
# constant of the problem, worth 8 on benchmark B. But the finisher does not
# start at the duel, it starts at a backtrack state already at 7/8 cards, so
#
# h_root is 1. The earlier 8/15/25/40/60 dial was therefore swept at an
# effective alpha of 1 / 1.9 / 3.1 / 5 / 7.5. This dial covers the SAME
# effective range on the corrected scale, so the earlier result (a gain at the
# upper edge, alpha = 25, one root out of four) is comparable point by point:
#
#     s8 alpha  8   15   25   40   60
#     s9 alpha  1    2    3    5    8
#
# Setup unchanged otherwise: benchmark 0, the DETERMINISTIC A/B, with imposed
# roots (--approach), an empty policy in every arm (--no-nrpa) and goal-only
# mode (--no-plan). Whatever moves in the "appr0 backtrack N" table can only
# come from the cost function.
#
# FREE CORRECTNESS CHECK, to verify in every arm:
#   appr0 recul 0  ->  42 exp.,  b=0,  EPUISE
# 42 because a cost function changes the ORDER of the expansions, not the
# reachable set; b=0 because no ceiling bit, without which "EXHAUSTED" would not
# be a proof of absence.
#
# The h0= column prints h_root: it must be 1 on this benchmark. If it is 8, the
# fix is not in the binary.
param([int]$Ms = 300000,
      [int]$FinMin = 260000,
      [string]$Seed = '888',
      [string]$Approche = 's8_B_T_s888\best_approach_7of8.yrp')

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
$ref = "D:\ProjectIgnis\replay\synchron handrip 2.yrpX"

$bras = @(
    @{ nom = 'A1'; arg = @('--reroot-h', '1') },
    @{ nom = 'A2'; arg = @('--reroot-h', '2') },
    @{ nom = 'A3'; arg = @('--reroot-h', '3') },
    @{ nom = 'A5'; arg = @('--reroot-h', '5') },
    @{ nom = 'A8'; arg = @('--reroot-h', '8') }
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
