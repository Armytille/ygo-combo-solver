# A/B DU REROOTER A RACINES IDENTIQUES (session 8).
#
# POURQUOI CE MONTAGE REMPLACE L'A/B BOUT-EN-BOUT
#
# --reroot et --reroot-h ne touchent QU'A RunLevin, le finisseur. Tout ce qui
# precede — sonde gloutonne, tirages NRPA, archive — est le meme code dans tous
# les bras et ne differe que par le timing des echanges entre workers. Comparer
# des runs entiers revient donc a lire surtout le bruit des tirages : la
# session 6 avait deja etabli que les compteurs de tirages ne sont pas des
# metriques d'A/B.
#
# Ici les racines du finisseur sont IMPOSEES et identiques dans tous les bras :
# --approach sert la meme approche a tous, --finisher-min lui donne l'essentiel
# du budget, et les lignes « appr0 recul N » du rapport se comparent une a une
# (expansions, meilleur recouvrement, EPUISE ou budget). C'est le seul endroit
# ou le mecanisme agit, donc le seul endroit ou il doit se mesurer.
#
# --no-plan est conserve : on reste dans le REGIME BUT SEUL (suffixes a
# reconstruire de zero), qui est le regime ou la prevision de cout annoncait le
# gain de sqrt-LTS.
#
# --no-nrpa ferme le dernier canal de bruit. La politique servie au finisseur
# est la FUSION des poids appris par les workers NRPA ; elle differe donc d'un
# bras a l'autre par le seul timing des echanges. Sans NRPA elle est VIDE dans
# tous les bras, donc identique. Racines identiques + politique identique =
# l'A/B devient DETERMINISTE : ce qui bouge dans la table « appr0 recul N » ne
# peut venir que de la fonction de cout, c'est-a-dire du rerooter. C'est le
# prix a payer : on mesure le mecanisme sur une politique uniforme, ce qui est
# precisement le point de depart du mode but seul.
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
