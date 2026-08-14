# CADRAN ALPHA DU REROOTER DOUX, REFAIT SUR L'ECHELLE CORRIGEE (session 9, C3).
#
# POURQUOI LE CADRAN DE LA SESSION 8 EST A JETER
#
# inv_w = exp(alpha * h(n) / h_root) — Eq. 7 de arXiv:2605.30664, ou h_root est
# h A LA RACINE DE LA RECHERCHE COURANTE. La session 8 y mettait
# |cible| + Sigma resolve_min, c'est-a-dire h au depart du DUEL : une constante
# du probleme, valant 8 sur l'etalon B. Or le finisseur ne demarre pas au duel,
# il demarre a un etat de recul deja a 7/8 cartes, donc h_root vaut 1.
#
# Le cadran 8/15/25/40/60 de la session 8 a donc ete parcouru a alpha_effectif
# = 1 / 1,9 / 3,1 / 5 / 7,5. Ce cadran-ci reprend la MEME plage effective sur
# l'echelle corrigee, pour que le resultat de la session 8 (gain au bord
# superieur, alpha = 25, une racine sur quatre) soit comparable point a point :
#
#     s8 alpha  8   15   25   40   60
#     s9 alpha  1    2    3    5    8
#
# Montage inchange par ailleurs — c'est l'etalon 0, l'A/B DETERMINISTE :
# racines imposees (--approach), politique vide dans tous les bras (--no-nrpa),
# regime but seul (--no-plan). Ce qui bouge dans la table « appr0 recul N » ne
# peut venir que de la fonction de cout.
#
# CONTROLE DE CORRECTION GRATUIT, a verifier dans chaque bras :
#   appr0 recul 0  ->  42 exp.,  b=0,  EPUISE
# 42 parce qu'une fonction de cout change l'ORDRE des expansions, pas
# l'ensemble atteignable ; b=0 parce qu'aucun plafond n'a mordu, sans quoi
# « EPUISE » ne serait pas une preuve d'absence (C2).
#
# La colonne h0= imprime h_root : elle doit valoir 1 sur cet etalon. Si elle
# vaut 8, la correction C3 n'est pas dans le binaire.
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
