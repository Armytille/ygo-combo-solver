# A/B DES OPTIONS, deuxieme iteration (session 12) — les deux gardes posees :
# une macro par premiere cle, aucun biais herite. Voir s12_options_ab.ps1 pour
# le montage et la lecture ; seuls les outdirs changent.
param([int]$Ms = 60000, [string]$Seed = '888')

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
$ref = "D:\ProjectIgnis\replay\synchron handrip 2.yrpX"

function Invoke-Bras {
    param([string]$Out, [string[]]$Extra)
    Write-Output "--- $Out"
    & .\bin\Release\combosolver.exe $ref `
        --scriptdir ..\deps\scripts_2026-04-13\script `
        --start $ref --no-plan `
        --solve-ms $Ms --seed $Seed `
        --adapt solutions --adapt-passes 4 `
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
    Invoke-Bras "s12_O2_temoin_r$r" @()
    Invoke-Bras "s12_O2_opt_r$r" @('--options', '256')
}
Write-Output "TERMINE"
