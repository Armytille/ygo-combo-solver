# PGO (session 12, point 2 du prompt perf).
#
# /GL est deja pose (LTO, session 11). Le profil d'execution est extremement
# stable : cas d'ecole PGO. Mecanique : le linker MSVC lit la variable
# d'environnement _LINK_ et l'ajoute a sa ligne de commande — pas besoin de
# toucher premake ni de regenerer la solution.
#
#   1. temoin : bin\Release\combosolver.exe copie en combosolver_lto_s12.exe
#   2. /GENPROFILE : relink instrumente (suppression de l'exe pour forcer le
#      lien ; /nr:false pour que les nodes MSBuild n'heritent pas d'un
#      environnement perime), pgort140.dll copiee a cote de l'exe
#   3. entrainement : sante 60 s + etalon B contraint 30 s + etalon 0 court —
#      les trois regimes du profil (LDS, tirages, finisseur)
#   4. /USEPROFILE : relink optimise sur les .pgc fusionnes
#   5. la MESURE se fait apres, separement (sante stricte + Process µs/appel)
#
# LA METRIQUE DU GAIN N'EST PAS le compteur de tirages : la session 12 a
# montre (mediannes x3) que sa dispersion a graine fixee (±10-18 %) engloutit
# un effet de ~15 %. Le juge est le cout PAR APPEL de Process, releve par la
# meme sonde --profile des deux cotes (27,2 -> 23,5 µs pour le LTO seul).
param([string]$Seed = '888')

$msbuild = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe"
$pgort = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\pgort140.dll"
Set-Location "D:\ProjectIgnis\replay2video\combosolver"
$ref = "D:\ProjectIgnis\replay\synchron handrip 2.yrpX"

# 1. temoin
Copy-Item bin\Release\combosolver.exe bin\Release\combosolver_lto_s12.exe -Force

# 2. relink instrumente
$env:_LINK_ = "/GENPROFILE"
Remove-Item bin\Release\combosolver.exe -Force
& $msbuild build\combosolver.sln /p:Configuration=Release /p:Platform=x64 /m /nr:false /nologo /v:quiet
if ($LASTEXITCODE -ne 0) { Write-Output "!! link /GENPROFILE : EXIT=$LASTEXITCODE"; exit 1 }
Copy-Item $pgort bin\Release\ -Force
Write-Output "--- exe instrumente lie"

# 3. entrainement (les .pgc tombent a cote du .pgd, dans bin\Release)
& .\bin\Release\combosolver.exe $ref `
    --scriptdir ..\deps\scripts_2026-04-13\script `
    --solve --solve-ms 60000 `
    --outdir s12_pgo_train1 --no-chain Zalen --no-chain "Crystal Wing" `
    *> s12_pgo_train1.log
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
    --outdir s12_pgo_train2 *> s12_pgo_train2.log
Write-Output "    etalon B d'entrainement : EXIT=$LASTEXITCODE"
& .\bin\Release\combosolver.exe $ref `
    --scriptdir ..\deps\scripts_2026-04-13\script `
    --start $ref --no-plan --no-nrpa `
    --approach s8_B_T_s888\best_approach_7of8.yrp --finisher-min 20000 `
    --solve-ms 30000 --seed $Seed `
    --no-chain Zalen --no-chain "Crystal Wing" `
    --outdir s12_pgo_train3 *> s12_pgo_train3.log
Write-Output "    etalon 0 d'entrainement : EXIT=$LASTEXITCODE"
Get-ChildItem bin\Release\*.pgc | ForEach-Object { Write-Output "    pgc : $($_.Name) $([Math]::Round($_.Length/1MB,1)) Mo" }

# 4. relink optimise
$env:_LINK_ = "/USEPROFILE"
Remove-Item bin\Release\combosolver.exe -Force
& $msbuild build\combosolver.sln /p:Configuration=Release /p:Platform=x64 /m /nr:false /nologo /v:quiet
$code = $LASTEXITCODE
$env:_LINK_ = $null
if ($code -ne 0) { Write-Output "!! link /USEPROFILE : EXIT=$code"; exit 1 }
Remove-Item bin\Release\pgort140.dll -Force -ErrorAction SilentlyContinue
Write-Output "--- exe PGO lie (temoin : combosolver_lto_s12.exe)"
Write-Output "TERMINE"
