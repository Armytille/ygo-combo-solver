# CHANTIER REJEUX DU FINISSEUR (session 12, point 3 du prompt perf).
#
# Le profil (9.18 (c)) a montre que le cout de RunLevin est le REJEU du chemin
# a chaque saut de la file : ~80 Process par expansion. Deux mecanismes
# l'attaquent, flag-gates et ETEINTS par defaut :
#   --lifo-ties  a cout de Levin egal, extraire le dernier enfile (localite)
#   --dive-full  empiler un niveau d'arene a chaque noeud de chaine rejoue
#                (la pile detient la branche entiere, l'ancetre partage trouve
#                est le vrai point de branchement)
#
# SEQUENCE :
#   0. sante stricte, flags eteints — le chemin par defaut doit rendre les
#      MEMES nombres que s11_sante_apres.log (seules les durees et la nouvelle
#      colonne rj= peuvent differer).
#   1. etalon 0 (montage s9_ab_alpha sans cadran), QUATRE bras sous --profile.
#
# CE QU'IL FAUT LIRE, par bras :
#   - CONTROLE : « appr0 recul 0 -> 42 exp., b=0, EPUISE » — un ordre
#     d'extraction change l'ordre, jamais l'ensemble atteignable.
#   - GAIN : expansions a temps egal sur les reculs 10/20/30/45, la colonne
#     rj=rejouees/chaine (l'instrument nouveau), et Process/expansion dans la
#     table de profil de la phase « finisseur approches ».
param([int]$Ms = 100000,
      [int]$FinMin = 80000,
      [string]$Seed = '888',
      [string]$Approche = 's8_B_T_s888\best_approach_7of8.yrp')

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
$ref = "D:\ProjectIgnis\replay\synchron handrip 2.yrpX"

Write-Output "--- sante (flags eteints)"
& .\bin\Release\combosolver.exe $ref `
    --scriptdir ..\deps\scripts_2026-04-13\script `
    --solve --solve-ms 60000 `
    --outdir s12_sante --no-chain Zalen --no-chain "Crystal Wing" `
    *> s12_sante.log
Write-Output "    EXIT=$LASTEXITCODE"

$bras = @(
    @{ nom = 'temoin'; arg = @() },
    @{ nom = 'lifo';   arg = @('--lifo-ties') },
    @{ nom = 'dive';   arg = @('--dive-full') },
    @{ nom = 'both';   arg = @('--lifo-ties', '--dive-full') }
)
foreach ($b in $bras) {
    $out = "s12_F_$($b.nom)"
    Write-Output "--- etalon 0 bras $($b.nom) -> $out"
    & .\bin\Release\combosolver.exe $ref `
        --scriptdir ..\deps\scripts_2026-04-13\script `
        --start $ref --no-plan --no-nrpa `
        --approach $Approche --finisher-min $FinMin `
        --solve-ms $Ms --seed $Seed `
        --no-chain Zalen --no-chain "Crystal Wing" `
        --profile @($b.arg) --outdir $out *> "$out.log"
    Write-Output "    EXIT=$LASTEXITCODE"
}
Write-Output "TERMINE"
