# Session 7 — A/B du rejeu d'ADAPTATION (chantier 5bis) sur l'ETALON 2 :
# les fenetres --fire OUVERTES, le mur des rips precoces.
#
# Meme corpus que le prior par POIDS de la session 6 (sF_final/), meme graine,
# meme budget, meme point d'injection : la seule difference entre les deux bras
# est la FORME du signal — prime par coup (refutee) contre gradient
# discriminatif. Les bras sont sequentiels (jamais deux mesures en parallele).
param([string]$Corpus = 'sF_final',
      [int]$Passes = 4,
      [int]$FireMs = 300000,
      [string]$Seed = '888')

Set-Location "D:\ProjectIgnis\replay2video\combosolver"

function Invoke-Fire([string]$Out, [string[]]$Extra) {
    Write-Output "=== $Out ==="
    & .\bin\Release\combosolver.exe "D:\ProjectIgnis\replay\synchron handrip 2.yrpX" `
        --scriptdir ..\deps\scripts_2026-04-13\script `
        --fire "27204311" --fire-open --fire-bake --fire-ms $FireMs `
        --fire-spare "Junk Signal" --fire-spare "Crystal Wing" --fire-spare "63436931" `
        --seed $Seed --outdir $Out `
        --guard "5:Crystal Wing|Zalen@terrain+Junk Signal@main" --guard-off "mainadv<=2" `
        --no-activate "Duel Evolution - Assault Zone" `
        --no-chain Zalen --no-chain "Crystal Wing" `
        --resolve "PSY-Framelord Omega@terrain:2" `
        --resolve "Trishula, Dragon of the Ice Barrier@terrain" `
        @Extra 2>&1 | Tee-Object -FilePath "$Out.log" |
        Select-String -Pattern 'fenetre |converties|accord du corpus' |
        Select-Object -Last 20
}

Invoke-Fire 's7_fire_base' @()
Invoke-Fire 's7_fire_adapt' @('--adapt', $Corpus, '--adapt-passes', "$Passes")
Write-Output "=== A/B fire termine ==="
