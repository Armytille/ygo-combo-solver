# ARETES MACRO DANS LE FINISSEUR (session 14, chantier 3) — LE CONTROLE D'ABORD.
#
# LE CONTROLE CHANGE DE FORME, ET C'EST ECRIT AVANT DE MESURER (exigence du
# prompt). L'etalon 0 se lisait « recul 0 -> 42 expansions, b=0, EPUISE » :
# c'etait une IDENTITE, valable parce que dive_full et merged_pop ne changent
# que la vitesse. Les aretes macro, elles, changent l'ARBRE — une macro
# applicable ajoute une arete, donc le denominateur du softmax grossit et TOUTES
# les probabilites d'aretes atomiques baissent. Les comptes d'expansions
# DOIVENT bouger. Ce qui doit tenir :
#
#   (1) memes `best` par racine — l'espace atteignable est inchange (les aretes
#       atomiques restent toutes la, une macro n'en retire aucune) ;
#   (2) aucune solution perdue — le nombre de lignes ecrites ne baisse pas ;
#   (3) EPUISE reste EPUISE — une racine dont la file se vidait doit toujours
#       se vider (sinon une arete macro a fabrique des noeuds inatteignables,
#       ce qui serait un bug de rejeu, pas un effet).
#
# BRAS 0 (dormance) : --finisher-options ETEINT doit rendre EXACTEMENT le
# 42/b=0/EPUISE historique. C'est le filet : si ce bras bouge, le refactor du
# rejeu de chaine a casse quelque chose et rien d'autre n'est lisible.
param([int]$Ms = 100000,
      [int]$FinMin = 80000,
      [string]$Seed = '888',
      [string]$Approche = 's8_B_T_s888\best_approach_7of8.yrp')

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
$ref = "D:\ProjectIgnis\replay\synchron handrip 2.yrpX"

# bras 0 : dormance stricte (aucun catalogue, drapeau eteint) — l'identite
$bras = @(
    @{ nom = 'dormant'; arg = @() },
    @{ nom = 'cat';     arg = @('--adapt', 'solutions', '--adapt-passes', '4',
                                '--options', '256') },
    @{ nom = 'aretes';  arg = @('--adapt', 'solutions', '--adapt-passes', '4',
                                '--options', '256', '--finisher-options') }
)
foreach ($b in $bras) {
    $out = "s14_AM_$($b.nom)"
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
Write-Output "LIRE : les lignes 'recul N ... exp. ... best k/8 ... EPUISE/budget'"
Write-Output "       de chaque bras, cote a cote — best et EPUISE, pas exp."
