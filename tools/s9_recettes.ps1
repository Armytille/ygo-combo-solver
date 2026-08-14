# GRAPHE DE RECETTES — la mesure qui decide (session 9, chantier 16).
#
# LA QUESTION, et elle est anterieure a « est-ce que ca gagne ».
#
# Le diagnostic de la session 8 dit : le verrou n'est plus l'algorithme de
# recherche, il est dans `h`. Notre `h` — le nombre de cartes cibles manquantes
# — vaut zero sur ~90 % de la ligne, et un mecanisme de decomposition ne peut
# rien decomposer sur un paysage plat. Le graphe de recettes est cense rendre
# `h` informatif : il compte les INVOCATIONS restantes, materiaux
# intermediaires compris, donc il DECROIT quand on assemble une piece
# intermediaire — alors que le `h` plat ne bouge pas.
#
# Avant de demander si ce `h` fait gagner la recherche, il faut savoir S'IL
# DIFFERE DU `h` PLAT. C'est le piege 40 (instrumenter avant de calibrer) et le
# piege 42 (un mecanisme peut etre vivant et sans effet). D'ou les trois bras :
#
#   T    temoin, aucun graphe                       -> h plat
#   M    --recipes 0 : le graphe est ALIMENTE et MESURE, mais n'entre PAS dans
#        le cout. Le run est donc IDENTIQUE au temoin par construction, et la
#        colonne `hR` dit ce que le graphe AURAIT dit. C'est la mesure qui
#        decide s'il vaut la peine d'etre branche.
#   P1   --recipes 1 : la distance de recettes pese dans h.
#   P2   --recipes 2
#
# CE QU'IL FAUT LIRE, dans cet ordre :
#
# 1. `rec=` (invocations observees). A ZERO, le graphe est vide et la distance
#    vaut exactement le `h` plat : le mecanisme est INERTE et les bras P
#    n'auront rien mesure. C'est la premiere chose a verifier — piege 52.
# 2. `hR` (distance de recettes moyenne) contre |cible manquante| du temoin.
#    Si hR ~ le nombre de cartes manquantes, le graphe n'a rien appris de plus
#    que le compte : le paysage est reste plat, et le chantier a echoue a son
#    but declare, quel que soit le resultat en cartes.
#    Si hR > ce compte, le graphe voit des INVOCATIONS INTERMEDIAIRES que le
#    `h` plat ignorait — c'est le signal recherche.
# 3. Seulement ensuite : les cartes cumulees des bras P contre le temoin.
#
# Montage : etalon 0, A/B DETERMINISTE (racines imposees par --approach,
# politique vide par --no-nrpa, regime but seul par --no-plan). Controle gratuit
# dans chaque bras : « appr0 recul 0 » doit rendre 42 exp., b=0, EPUISE.
param([int]$Ms = 300000,
      [int]$FinMin = 260000,
      [string]$Seed = '888',
      [string]$Approche = 's8_B_T_s888\best_approach_7of8.yrp')

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
$ref = "D:\ProjectIgnis\replay\synchron handrip 2.yrpX"

$bras = @(
    @{ nom = 'T';  arg = @() },
    @{ nom = 'M';  arg = @('--recipes', '0') },
    @{ nom = 'P1'; arg = @('--recipes', '1') },
    @{ nom = 'P2'; arg = @('--recipes', '2') }
)

foreach ($b in $bras) {
    $out = "s9r_$($b.nom)"
    Write-Output "--- recettes bras $($b.nom) -> $out"
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
