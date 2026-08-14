# CADRAN ALPHA COMPLET, SUR UN SEUL BINAIRE (session 9, C3 — final).
#
# POURQUOI TOUT REJOUER PLUTOT QUE PROLONGER
#
# Le cadran bas (1/2/3/5/8) a tourne sur le binaire d'avant le lot d'audit. Le
# gating de la nouveaute dans PolicyRollout (audit 3.7 — `--novelty 0`
# n'eteignait pas la nouveaute cote NRPA) a change le DEBIT : +20 % d'expansions
# a budget egal sur le temoin. Prolonger le cadran sur le nouveau binaire
# comparerait donc des bras qui n'ont pas explore a la meme vitesse. Un cadran
# se lit d'un bloc ou pas du tout.
#
# Le temoin sur CE binaire existe deja : s9_ctrl.log (42 exp. b=0 EPUISE au
# controle, 6/5/4/5 = 20 cartes cumulees). Ce script sert les neuf bras alpha.
#
# LES DEUX QUESTIONS
#
# 1. Ou est l'optimum sur l'echelle CORRIGEE ? L'equivalence avec le cadran de
#    la session 8 est alpha_s9 = alpha_s8 * h0/8, soit x0,5 a x0,75 (h0 vaut 4 a
#    6 aux racines comparees, et non 8). L'optimum de la session 8 (alpha_s8=25)
#    tombe donc vers alpha_s9 ~ 12-19.
#
# 2. LE MECANISME S'ETEINT-IL PAR LE HAUT ? Au controle, `rr` s'effondre quand
#    alpha monte : 123, 118, 118, 83, 3 pour alpha = 1, 2, 3, 5, 8. Si `rr`
#    tombe a zero au-dela, les bras hauts ne mesurent pas un rerooter qui gagne
#    mais un rerooter INERTE — ce qui EXPLIQUERAIT le « gain » marginal de la
#    session 8 (retour au temoin) au lieu de le confirmer. Lire `rr` en meme
#    temps que le cumul, jamais le cumul seul (pieges 42 et 52).
#
# CONTROLE DE CORRECTION, dans chaque bras : « appr0 recul 0 » doit rendre
# 42 exp., b=0, EPUISE. Une fonction de cout change l'ORDRE des expansions, pas
# l'ensemble atteignable ; et b=0 dit qu'aucun plafond n'a mordu, sans quoi
# « EPUISE » ne serait pas une preuve d'absence.
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
