# MESURE PERF s24 - A/B de deux binaires sur le cout PAR APPEL de Process.
#
# LE JUGE N'EST PAS le compteur de tirages (declasse en s12 : dispersion
# +-10-18 % a graine fixee). C'est la ligne « Process (core) » de la table
# --profile de la phase tirages (colonne us/appel), MEDIANE de trois runs
# INTERCALES par bras (A,B,A,B,A,B - l'intercalage neutralise la derive
# thermique et le cache disque).
#
# Montage : etalon B contraint 60 s, graine 888 - celui des medianes s12/s15,
# conserve a l'identique pour la comparabilite historique (27,2 -> 23,6 LTO
# -> 18,6 us/appel PGO).
param([int]$Ms = 60000, [string]$Seed = '888',
      [string]$ExeA = '.\bin\Release\combosolver_s24base.exe',
      [string]$ExeB = '.\bin\Release\combosolver.exe',
      [string]$Tag = 's24_P')

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
$ref = "D:\ProjectIgnis\replay\synchron handrip 2.yrpX"

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
    Invoke-EtalonB $ExeA "${Tag}_A_r$r"
    Invoke-EtalonB $ExeB "${Tag}_B_r$r"
}
Write-Output "TERMINE - lire la ligne Process de la table de profil de la"
Write-Output "phase tirages dans ${Tag}_A_r*.log / ${Tag}_B_r*.log"
