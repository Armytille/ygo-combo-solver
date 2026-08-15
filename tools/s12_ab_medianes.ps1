# LES TROIS REPETITIONS ET LES MEDIANES (session 12, point 1 du prompt perf).
#
# Les gains de la session 11 (+31,8 % d'etats a temps egal) sont des runs
# UNIQUES a graine fixee — piege 39 : la dispersion domine. Ce montage refait
# l'A/B binaire s11 en trois repetitions par bras, INTERCALEES (old, new,
# newprof, old, ...) pour decorreler la derive thermique/machine, et chiffre au
# passage le cout de l'instrument (--profile) sur trois runs.
#
# Bras :
#   old     — bin\Release\combosolver_preopt.exe, sans profil (temoin s11)
#   new     — bin\Release\combosolver.exe (LTO+C20+C22+ancetre), sans profil
#   newprof — le meme, avec --profile (cout de l'instrument)
#
# CE QU'IL FAUT LIRE : la ligne « NRPA : N tirages M etats » de la section
# « tirages profonds » de chaque log ; comparer les MEDIANES par bras.
# Le montage est l'etalon B contraint (s9_etalon_b_contraint.ps1), 60 s,
# graine 888 — identique a la session 11.
#
# En queue de batterie : s10_options (30 s), la prevision du gain des OPTIONS
# (chantier 17), jamais lue — sequentielle comme le reste.
param([int]$Ms = 60000, [string]$Seed = '888')

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
$ref = "D:\ProjectIgnis\replay\synchron handrip 2.yrpX"

function Invoke-EtalonB {
    param([string]$Exe, [string]$Out, [string[]]$Extra = @())
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
        @Extra `
        --outdir $Out *> "$Out.log"
    Write-Output "    EXIT=$LASTEXITCODE"
}

foreach ($r in 1..3) {
    Invoke-EtalonB '.\bin\Release\combosolver_preopt.exe' "s12_B_old_r$r"
    Invoke-EtalonB '.\bin\Release\combosolver.exe'         "s12_B_new_r$r"
    Invoke-EtalonB '.\bin\Release\combosolver.exe'         "s12_B_newprof_r$r" @('--profile')
}

# Chantier 17 : la prevision, 30 s, sur le corpus solutions/ (voir s10_options.ps1).
Write-Output "--- s12_options"
& .\bin\Release\combosolver.exe $ref `
    --scriptdir ..\deps\scripts_2026-04-13\script `
    --solve --solve-ms 1000 --seed $Seed `
    --adapt solutions --adapt-passes 4 `
    --no-chain Zalen --no-chain "Crystal Wing" `
    --outdir s12_options *> "s12_options.log"
Write-Output "    EXIT=$LASTEXITCODE"
Write-Output "TERMINE"
