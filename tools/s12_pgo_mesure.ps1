# MESURE PGO (session 12) — apres tools/s12_pgo.ps1.
#
# LA METRIQUE N'EST PAS le compteur de tirages (declasse par les medianes de
# cette session : dispersion ±10-18 % a graine fixee). Le juge est le cout PAR
# APPEL de Process, releve par la meme sonde --profile des deux cotes, median
# de trois runs intercales. Lire la ligne « Process (core) » de la table de
# profil de la phase tirages (colonne µs/appel) dans chaque log.
#
# Bras : lto  = bin\Release\combosolver_lto_s12.exe (temoin, LTO seul)
#        pgo  = bin\Release\combosolver.exe (PGO par-dessus LTO)
# Montage : etalon B contraint 60 s, graine 888 — celui des medianes s12.
#
# En tete : la sante stricte du binaire PGO (diff attendu contre s12_sante.log :
# QUE des durees — PGO ne change que le codegen).
param([int]$Ms = 60000, [string]$Seed = '888')

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
$ref = "D:\ProjectIgnis\replay\synchron handrip 2.yrpX"

Write-Output "--- sante du binaire PGO"
& .\bin\Release\combosolver.exe $ref `
    --scriptdir ..\deps\scripts_2026-04-13\script `
    --solve --solve-ms 60000 `
    --outdir s12_sante_pgo --no-chain Zalen --no-chain "Crystal Wing" `
    *> s12_sante_pgo.log
Write-Output "    EXIT=$LASTEXITCODE"

function Invoke-EtalonB {
    param([string]$Exe, [string]$Out)
    Write-Output "--- $Out ($Exe)"
    & $Exe $ref `
        --scriptdir ..\deps\scripts_2026-04-13\script `
        --start $ref --no-plan `
        --solve-ms $Ms --seed $Seed `
        --guard "5:Crystal Wing|Zalen@terrain+Junk Signal@main" --guard-off "mainadv<=2" `
        --no-activate "Duel Evolution - Assault Zone" `
        --no-chain Zalen --no-chain "Crystal Wing" `
        --resolve "PSY-Framelord Omega@terrain:2" `
        --resolve "Trishula, Dragon of the Ice Barrier@terrain" `
        --profile `
        --outdir $Out *> "$Out.log"
    Write-Output "    EXIT=$LASTEXITCODE"
}

foreach ($r in 1..3) {
    Invoke-EtalonB '.\bin\Release\combosolver_lto_s12.exe' "s12_P_lto_r$r"
    Invoke-EtalonB '.\bin\Release\combosolver.exe'         "s12_P_pgo_r$r"
}
Write-Output "TERMINE"
