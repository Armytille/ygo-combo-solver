# Enchainement SEQUENTIEL des mesures de la session 8.
# Un seul run a la fois : deux mesures en parallele se volent les coeurs et
# rendent les durees — donc les bras limites par le BUDGET — incomparables.
Set-Location "D:\ProjectIgnis\replay2video\combosolver"

Write-Output "=== 1. sante finale (porte) ==="
.\bin\Release\combosolver.exe "D:\ProjectIgnis\replay\synchron handrip 2.yrpX" `
    --scriptdir ..\deps\scripts_2026-04-13\script --solve --solve-ms 60000 `
    --outdir s8_sante_final --no-chain Zalen --no-chain "Crystal Wing" `
    *> s8_sante_final.log
Write-Output "    EXIT=$LASTEXITCODE"

Write-Output "=== 2. A/B deterministe du finisseur ==="
.\tools\s8_ab_finisseur.ps1

Write-Output "=== 3. etalon A (Lunalight) en mode but seul ==="
.\tools\lunalight_butseul.ps1 -Ms 1800000 -Seed 888 -Bras base

Write-Output "PIPELINE_TERMINE"
