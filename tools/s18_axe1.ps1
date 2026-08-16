# AXE 1 DE L'AUDIT (session 18) — ATTRIBUER LE COUPLAGE FANTOME DE `--card-on-select`.
#
# LE FAIT A EXPLIQUER (9.24 (o)) : sur l'etalon B, `--card-on-select` SEUL fait
# tomber les resolutions de 2 239 a ZERO. La session 17 a cru corriger la cause
# (`Choice::card_lossy`, pour que le biais d'INDICES cesse de s'appliquer a une
# identite approximative), a re-mesure zero, et a conclu « le couplage est
# ailleurs et reste non identifie ».
#
# L'HYPOTHESE DE L'AUDIT : le correctif n'a jamais ete BRANCHE. `card_lossy` est
# declare (enumerate.h:53), remis a zero dans Emit() (enumerate.h:82) et lu une
# fois (search.cpp:3032) — il n'est JAMAIS ASSIGNE `true`. Le garde est donc
# inerte, et `--card-on-select` etend toujours `hint_bias` (2,0 par defaut) aux
# prompts de SELECTION. Sur l'etalon B, `hint_cards` n'est pas vide : les deux
# `--resolve` y versent Omega et Trishula d'office (main.cpp:6763).
#
# L'EXPERIENCE, ET POURQUOI ELLE NE TOUCHE PAS AU CODE. `--hint-bias 0` annule
# le seul canal soupconne sans rien recompiler. Si l'attribution est juste :
#   temoin  >> cos          (l'effondrement connu)
#   hb0     ~= cos_hb0      (le couplage a DISPARU quand le canal est coupe)
# Si `cos_hb0` s'effondre AUSSI par rapport a `hb0`, le canal est ailleurs et
# l'hypothese est refutee. Le bras `hb0` seul est le temoin APPARIE obligatoire :
# sans lui on comparerait `cos_hb0` a un temoin qui, lui, a garde ses indices.
#
# Juge : « resolutions atteintes par tirage » >=1 (main.cpp:7494) et l'approche
# ECRITE (best_approach_kof8.yrp), les deux juges de 9.24 (o), a l'identique.
param([int]$Ms = 90000,
      [string]$Seed = '888',
      [string[]]$Bras = @('temoin', 'cos', 'hb0', 'cos_hb0'),
      [string]$Prefixe = 's18a')

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
$bopt = "D:\ProjectIgnis\replay\synchron handrip optimized.yrpX"

$defs = @{
    'temoin'  = @()
    'cos'     = @('--card-on-select')
    'hb0'     = @('--hint-bias', '0')
    'cos_hb0' = @('--card-on-select', '--hint-bias', '0')
}

foreach ($b in $Bras) {
    $out = "${Prefixe}_${b}_$Seed"
    Write-Output "--- $out"
    & .\bin\Release\combosolver.exe $bopt `
        --scriptdir ..\deps\scripts_2026-04-13\script `
        --start $bopt --no-plan --solve-ms $Ms --seed $Seed `
        --guard "5:Crystal Wing|Zalen@terrain+Junk Signal@main" `
        --no-activate "Duel Evolution - Assault Zone" `
        --no-chain Zalen --no-chain "Crystal Wing" `
        --resolve "PSY-Framelord Omega@terrain:2" `
        --resolve "Trishula, Dragon of the Ice Barrier@terrain" `
        @($defs[$b]) `
        --finisher levin --archive-k 24 --outdir $out *> "$out.log"
    Write-Output "    EXIT=$LASTEXITCODE"
}
