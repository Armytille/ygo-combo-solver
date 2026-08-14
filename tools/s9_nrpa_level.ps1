# A/B DU NIVEAU D'IMBRICATION NRPA, A BUDGET EGAL (session 9, C15).
#
# POURQUOI CETTE MESURE MANQUAIT
#
# `nrpa_level = (budget > 180000.0) ? 3 : 2` decidait tout seul, et ce n'est pas
# un reglage fin : il change le cout d'un appel de niveau de ~576 tirages a
# ~13 824, c'est-a-dire L'ALGORITHME D'ECHANTILLONNAGE lui-meme. Aucune mesure
# ne l'adossait.
#
# Pire, le seuil tombait exactement sur la ligne de partage des commandes
# comparees aux sessions 5-7 : un run de 600 s SANS --finisher-min laisse 420 s
# aux tirages (0,7 x 600) donc niveau 3, et le MEME run AVEC
# --finisher-min 420000 en laisse ~180 donc niveau 2. Plusieurs A/B publies
# comparaient donc deux algorithmes en croyant comparer deux reglages.
#
# Le drapeau --nrpa-level rend le choix explicite et le niveau effectif est
# imprime dans tous les cas. Reste a savoir lequel vaut mieux, a budget egal.
#
# MONTAGE. Etalon B (synchron, reference NEUTRALISEE par --start sur lui-meme
# + --no-plan), deux bras, meme graine, meme budget, sequentiels. Le seul
# facteur qui change est le niveau.
#
# CE QU'IL FAUT LIRE. Le fait STRUCTUREL est le meilleur recouvrement atteint
# (best n/8) et les conversions, PAS les compteurs de tirages — piege 39 : a
# graine fixee le bruit de run domine les compteurs de tirages, et un niveau 3
# fait mecaniquement moins d'appels de niveau qu'un niveau 2 sans que cela dise
# quoi que ce soit sur la qualite.
param([int]$Ms = 600000, [string]$Seed = '888')

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
$ref = "D:\ProjectIgnis\replay\synchron handrip 2.yrpX"

foreach ($lvl in @(2, 3)) {
    $out = "s9_lvl$lvl"
    Write-Output "--- niveau NRPA $lvl -> $out"
    & .\bin\Release\combosolver.exe $ref `
        --scriptdir ..\deps\scripts_2026-04-13\script `
        --start $ref --no-plan `
        --nrpa-level $lvl `
        --solve-ms $Ms --seed $Seed `
        --no-chain Zalen --no-chain "Crystal Wing" `
        --outdir $out *> "$out.log"
    Write-Output "    EXIT=$LASTEXITCODE"
}
Write-Output "TERMINE"
