# PGO session 24 - MEME mecanique que s15_pgo.ps1 (six regimes), avec :
#   * temoin nomme combosolver_lto_s24.exe (les temoins s12-s15 sont conserves) ;
#   * un 7e regime d'entrainement : la BOUCLE INTERNE s23/s24 (--rounds 2 avec
#     --carry --archive-fin --quota-h --refine-after) - les chemins neufs
#     (semis d'archive, fusion re-enracinee, quotas au raffinement, demandes
#     transitoires) n'existaient dans aucun profil anterieur.
#
# RAPPEL (9.19 (d)) : tout MSBuild ordinaire RELIE SANS /USEPROFILE et perd le
# PGO en silence. Apres tout changement de moteur : rebuild, sante, PUIS ce
# script, AVANT toute mesure.
param([string]$Seed = '888')

$msbuild = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe"
$pgort = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\pgort140.dll"
Set-Location "D:\ProjectIgnis\replay2video\combosolver"
$ref = "D:\ProjectIgnis\replay\synchron handrip 2.yrpX"
$repo = "D:\ProjectIgnis\repositories"
$gabarit = "D:\ProjectIgnis\replay2video\combosolver\gabarits\etalon_a_lunalight.yrp"

# 1. temoin LTO du code s24
Copy-Item bin\Release\combosolver.exe bin\Release\combosolver_lto_s24.exe -Force

# 2. relink instrumente
$env:_LINK_ = "/GENPROFILE"
Remove-Item bin\Release\combosolver.exe -Force
& $msbuild build\combosolver.sln /p:Configuration=Release /p:Platform=x64 /m /nr:false /nologo /v:quiet
if ($LASTEXITCODE -ne 0) { Write-Output "!! link /GENPROFILE : EXIT=$LASTEXITCODE"; exit 1 }
Copy-Item $pgort bin\Release\ -Force
Write-Output "--- exe instrumente lie"

# 3. entrainement : les SEPT regimes
& .\bin\Release\combosolver.exe $ref `
    --scriptdir ..\deps\scripts_2026-04-13\script `
    --solve --solve-ms 60000 `
    --outdir s24_pgo_train1 --no-chain Zalen --no-chain "Crystal Wing" `
    *> s24_pgo_train1.log
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
    --outdir s24_pgo_train2 *> s24_pgo_train2.log
Write-Output "    etalon B d'entrainement : EXIT=$LASTEXITCODE"
& .\bin\Release\combosolver.exe $ref `
    --scriptdir ..\deps\scripts_2026-04-13\script `
    --start $ref --no-plan --no-nrpa `
    --approach s8_B_T_s888\best_approach_7of8.yrp --finisher-min 20000 `
    --solve-ms 30000 --seed $Seed `
    --no-chain Zalen --no-chain "Crystal Wing" `
    --outdir s24_pgo_train3 *> s24_pgo_train3.log
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
    --outdir s24_pgo_train4 *> s24_pgo_train4.log
Write-Output "    options d'entrainement : EXIT=$LASTEXITCODE"
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
    --outdir s24_pgo_train5 *> s24_pgo_train5.log
Write-Output "    minage en ligne d'entrainement : EXIT=$LASTEXITCODE"
& .\bin\Release\combosolver.exe $gabarit `
    --scriptdir "D:\ProjectIgnis\replay2video\deps\compat_2026-08" `
    --scriptdir ..\deps\scripts_2026-04-13\script `
    --scriptdir "$repo\delta-bagooska\script" `
    --scriptdir "$repo\delta-puppet\script" `
    --deck "D:\ProjectIgnis\deck\Lunalight.ydk" `
    --hand "57103969|57103969|57103969" `
    --no-ref `
    --target 54701958 --target 54701958 --target 54701958 `
    --target "90590304@DEF" --max-decisions 700 `
    --resolve 4731783 --resolve 2344618 --resolve 47705572 `
    --summon-min "54701958:3" `
    --options-online 12 --qhat 6 `
    --solve-ms 40000 --seed $Seed --finisher levin --archive-k 24 `
    --outdir s24_pgo_train6 *> s24_pgo_train6.log
Write-Output "    bandit Q^ d'entrainement : EXIT=$LASTEXITCODE"
# Le regime NEUF de la s24 : la boucle interne complete - rounds, carry,
# fusion d'archive du finisseur, quotas au raffinement.
& .\bin\Release\combosolver.exe $ref `
    --scriptdir ..\deps\scripts_2026-04-13\script `
    --deck "D:\ProjectIgnis\deck\synchron handrip 2 player00 Schisme.ydk" `
    --no-ref --target 4891376 --target 27572350 --target 9753964 `
    --target 50954680 --target "63436931@DEF" --target 29053656 `
    --opp-hand "27204311|27204311|27204311" `
    --guard "5:Crystal Wing|Zalen@terrain+Junk Signal@main" `
    --no-activate "Duel Evolution - Assault Zone" `
    --resolve "74586817@terrain:2" --resolve "52687916@terrain" `
    --no-self-negate --mp1-only --max-decisions 700 `
    --options-online 30 --op-recipes --finisher levin --archive-k 96 `
    --solve-ms 80000 --rounds 2 --refine-after 30000 `
    --quota-h --carry --archive-fin `
    --seed 8305444233615674939 --outdir s24_pgo_train7 *> s24_pgo_train7.log
Write-Output "    boucle interne d'entrainement : EXIT=$LASTEXITCODE"
Get-ChildItem bin\Release\*.pgc | ForEach-Object { Write-Output "    pgc : $($_.Name) $([Math]::Round($_.Length/1MB,1)) Mo" }

# 4. relink optimise
$env:_LINK_ = "/USEPROFILE"
Remove-Item bin\Release\combosolver.exe -Force
& $msbuild build\combosolver.sln /p:Configuration=Release /p:Platform=x64 /m /nr:false /nologo /v:quiet
$code = $LASTEXITCODE
$env:_LINK_ = $null
if ($code -ne 0) { Write-Output "!! link /USEPROFILE : EXIT=$code"; exit 1 }
Remove-Item bin\Release\pgort140.dll -Force -ErrorAction SilentlyContinue
Write-Output "--- exe PGO lie (temoin : combosolver_lto_s24.exe)"
Write-Output "TERMINE"
