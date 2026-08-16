# PGO session 14 — MEME mecanique que s13_pgo.ps1, avec :
#   * temoin nomme combosolver_lto_s14b.exe (NE PAS ecraser les temoins s12/s13,
#     conserves pour l'archeologie des mesures) ;
#   * un 5e run d'entrainement AVEC minage EN LIGNE et SANS corpus externe :
#     c'est le chemin neuf de la session (releve de la ligne plate dans
#     PolicyRollout, tour de minage, rachat de catalogue), et le regime exact de
#     la mission — un run qui part nu et s'arme lui-meme.
#
# RAPPEL (9.19 (d)) : tout MSBuild ordinaire RELIE SANS /USEPROFILE et perd le
# PGO en silence. Apres tout changement de moteur : rebuild, sante, PUIS ce
# script, AVANT toute mesure.
param([string]$Seed = '888')

$msbuild = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe"
$pgort = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\pgort140.dll"
Set-Location "D:\ProjectIgnis\replay2video\combosolver"
$ref = "D:\ProjectIgnis\replay\synchron handrip 2.yrpX"

# 1. temoin LTO du code s14
Copy-Item bin\Release\combosolver.exe bin\Release\combosolver_lto_s14b.exe -Force

# 2. relink instrumente
$env:_LINK_ = "/GENPROFILE"
Remove-Item bin\Release\combosolver.exe -Force
& $msbuild build\combosolver.sln /p:Configuration=Release /p:Platform=x64 /m /nr:false /nologo /v:quiet
if ($LASTEXITCODE -ne 0) { Write-Output "!! link /GENPROFILE : EXIT=$LASTEXITCODE"; exit 1 }
Copy-Item $pgort bin\Release\ -Force
Write-Output "--- exe instrumente lie"

# 3. entrainement : les CINQ regimes (LDS, tirages, finisseur, options, en ligne)
& .\bin\Release\combosolver.exe $ref `
    --scriptdir ..\deps\scripts_2026-04-13\script `
    --solve --solve-ms 60000 `
    --outdir s14b_pgo_train1 --no-chain Zalen --no-chain "Crystal Wing" `
    *> s14b_pgo_train1.log
Write-Output "    sante d'entrainement : EXIT=$LASTEXITCODE"
& .\bin\Release\combosolver.exe $ref `
    --scriptdir ..\deps\scripts_2026-04-13\script `
    --start $ref --no-plan `
    --solve-ms 30000 --seed $Seed `
    --guard "5:Crystal Wing|Zalen@terrain+Junk Signal@main" --guard-off "mainadv<=2" `
    --no-activate "Duel Evolution - Assault Zone" `
    --no-chain Zalen --no-chain "Crystal Wing" `
    --resolve "PSY-Framelord Omega@terrain:2" `
    --resolve "Trishula, Dragon of the Ice Barrier@terrain" `
    --outdir s14b_pgo_train2 *> s14b_pgo_train2.log
Write-Output "    etalon B d'entrainement : EXIT=$LASTEXITCODE"
& .\bin\Release\combosolver.exe $ref `
    --scriptdir ..\deps\scripts_2026-04-13\script `
    --start $ref --no-plan --no-nrpa `
    --approach s8_B_T_s888\best_approach_7of8.yrp --finisher-min 20000 `
    --solve-ms 30000 --seed $Seed `
    --no-chain Zalen --no-chain "Crystal Wing" `
    --outdir s14b_pgo_train3 *> s14b_pgo_train3.log
Write-Output "    etalon 0 d'entrainement : EXIT=$LASTEXITCODE"
& .\bin\Release\combosolver.exe $ref `
    --scriptdir ..\deps\scripts_2026-04-13\script `
    --start $ref --no-plan `
    --solve-ms 30000 --seed $Seed `
    --adapt solutions --adapt-passes 4 --options 256 --options-ctx 1 `
    --guard "5:Crystal Wing|Zalen@terrain+Junk Signal@main" --guard-off "mainadv<=2" `
    --no-activate "Duel Evolution - Assault Zone" `
    --no-chain Zalen --no-chain "Crystal Wing" `
    --resolve "PSY-Framelord Omega@terrain:2" `
    --resolve "Trishula, Dragon of the Ice Barrier@terrain" `
    --outdir s14b_pgo_train4 *> s14b_pgo_train4.log
Write-Output "    options d'entrainement : EXIT=$LASTEXITCODE"
# Le regime NEUF : aucun --adapt, le catalogue nait du run lui-meme.
& .\bin\Release\combosolver.exe $ref `
    --scriptdir ..\deps\scripts_2026-04-13\script `
    --start $ref --no-plan `
    --solve-ms 40000 --seed $Seed `
    --options-online 12 --options-ctx 1 `
    --guard "5:Crystal Wing|Zalen@terrain+Junk Signal@main" --guard-off "mainadv<=2" `
    --no-activate "Duel Evolution - Assault Zone" `
    --no-chain Zalen --no-chain "Crystal Wing" `
    --resolve "PSY-Framelord Omega@terrain:2" `
    --resolve "Trishula, Dragon of the Ice Barrier@terrain" `
    --outdir s14b_pgo_train5 *> s14b_pgo_train5.log
Write-Output "    minage en ligne d'entrainement : EXIT=$LASTEXITCODE"
Get-ChildItem bin\Release\*.pgc | ForEach-Object { Write-Output "    pgc : $($_.Name) $([Math]::Round($_.Length/1MB,1)) Mo" }

# 4. relink optimise
$env:_LINK_ = "/USEPROFILE"
Remove-Item bin\Release\combosolver.exe -Force
& $msbuild build\combosolver.sln /p:Configuration=Release /p:Platform=x64 /m /nr:false /nologo /v:quiet
$code = $LASTEXITCODE
$env:_LINK_ = $null
if ($code -ne 0) { Write-Output "!! link /USEPROFILE : EXIT=$code"; exit 1 }
Remove-Item bin\Release\pgort140.dll -Force -ErrorAction SilentlyContinue
Write-Output "--- exe PGO lie (temoin : combosolver_lto_s14b.exe)"
Write-Output "TERMINE"
