# Prolongement du CADRAN alpha de sqrt-LTS-H (session 8).
#
# Le premier cadran (8 / 15 / 25) a rendu 18 / 20 / 21 cartes cumulees sur les
# quatre reculs limites par le budget, le temoin valant 20. Trois bras ordonnes
# par un seul reglage donnent trois resultats ordonnes : c'est la forme d'un
# vrai resultat (piege 46), et elle appelle un point de plus au-dessus. La
# fenetre calculee par ForecastSearchCost s'arretait a 28,8 ; on regarde donc
# 40 et 60, c'est-a-dire DEHORS, pour savoir si le gain continue ou si la
# fenetre etait la bonne prediction.
#
# Meme montage deterministe que tools/s8_ab_finisseur.ps1 : memes racines
# (--approach), meme politique (--no-nrpa, donc vide partout), meme budget.
param([int]$Ms = 300000,
      [int]$FinMin = 260000,
      [string]$Seed = '888',
      [string]$Approche = 's8_B_T_s888\best_approach_7of8.yrp')

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
$ref = "D:\ProjectIgnis\replay\synchron handrip 2.yrpX"

foreach ($a in @('40', '60')) {
    $out = "s8_F_H$a"
    Write-Output "--- finisseur bras H$a -> $out"
    & .\bin\Release\combosolver.exe $ref `
        --scriptdir ..\deps\scripts_2026-04-13\script `
        --start $ref --no-plan --no-nrpa `
        --approach $Approche --finisher-min $FinMin `
        --solve-ms $Ms --seed $Seed `
        --no-chain Zalen --no-chain "Crystal Wing" `
        --reroot-h $a --outdir $out *> "$out.log"
    Write-Output "    EXIT=$LASTEXITCODE"
}
Write-Output "ALPHA_TERMINE"
