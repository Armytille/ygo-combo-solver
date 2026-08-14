# SESSION 10 — les mesures de « A FAIRE D'ABORD », enchainees SEQUENTIELLEMENT
# sur UN SEUL binaire (celui construit au debut de la session).
#
# ORDRE, et pourquoi celui-la :
#
#  0. SANTE. La porte de tout. Elle etablit la ligne de base du binaire courant
#     AVANT la premiere mesure ; le diff avec le run de santé de la session 9 ne
#     doit contenir que des durees.
#  1. AMORCE (mesure 4 du prompt). C'est la question qui decide du chantier 16 :
#     `hR` monte-t-il au-dessus du `h` plat une fois le graphe amorce par le
#     texte ? Passe en premier parce qu'elle est la plus decisive et la moins
#     chere (4 x 300 s). Le bras SN est le temoin d'amorce sur CE binaire.
#  2. ETALON A but seul (mesure 1). Le run de la session 9 a ete INTERROMPU
#     avant la table LDS — c'est-a-dire avant la verification de C1 elle-meme.
#     A relancer en entier (1800 s).
#  3. NRPA-LEVEL (mesure 3). A/B a budget egal de la constante exposee par C15.
#
# Une mesure a la fois : deux runs en parallele se disputent seize workers et
# rendent tous les compteurs de tirages incomparables.
param([switch]$SkipSante)

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
$ref = "D:\ProjectIgnis\replay\synchron handrip 2.yrpX"

if (-not $SkipSante) {
    Write-Output "=== 0/3 SANTE -> s10_sante.log"
    & .\bin\Release\combosolver.exe $ref `
        --scriptdir ..\deps\scripts_2026-04-13\script --solve --solve-ms 60000 `
        --outdir s10_sante --no-chain Zalen --no-chain "Crystal Wing" `
        *> "s10_sante.log"
    Write-Output "    EXIT=$LASTEXITCODE"
}

Write-Output "=== 1/3 AMORCE DU GRAPHE DE RECETTES"
& .\tools\s9_recettes_amorce.ps1

Write-Output "=== 2/3 ETALON A BUT SEUL (verification de C1)"
& .\tools\s9_etalon_a.ps1

Write-Output "=== 3/3 A/B NRPA-LEVEL"
& .\tools\s9_nrpa_level.ps1

Write-Output "=== SUITE TERMINEE"
