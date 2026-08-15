# A/B DE LA GARDE SEMANTIQUE DES OPTIONS (--options-ctx, session 13).
#
# La fenetre POSITIONNELLE est refutee (9.19 (g) : elle reduit les avortements
# mais tue les >=3 resolutions — les tirages ne s'alignent pas en indice avec
# le corpus). La forme designee est SEMANTIQUE : ne proposer une macro que si
# le contexte courant (cartes cibles posees EXACTES, main a ±tol) est
# compatible avec une occurrence du corpus. Le meme test entre dans le modele
# de selection par perte de Levin — le catalogue retenu peut donc CHANGER.
#
# Deux montages :
#   1. etalon B contraint (exploitation, l'axe "posees" va de 0 a 8) :
#      temoin sel / ctx 1 (strict) / ctx 15 (posees seules) — 60 s x3.
#   2. etalon A bootstrap (decouverte, l'axe "posees" vaut 0-1 : la garde
#      mord peu par construction — c'est le controle de non-degradation) :
#      memes bras, 300 s x2 graines, corpus parametrable.
#
# LIRE : >=k resolutions, best, avortees/prises, taille du catalogue et perte
# modele (elle change avec la garde). Compteur de tirages declasse.
param([string]$CorpusA = 's12_boot_corpus', [int]$MsB = 60000, [int]$MsA = 300000)

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
$ref = "D:\ProjectIgnis\replay\synchron handrip 2.yrpX"
$repo = "D:\ProjectIgnis\repositories"
$gabarit = "D:\ProjectIgnis\replay\_LastReplay.yrpX"

function Invoke-BrasB {
    param([string]$Out, [string]$Seed, [string[]]$Extra)
    Write-Output "--- $Out"
    & .\bin\Release\combosolver.exe $ref `
        --scriptdir ..\deps\scripts_2026-04-13\script `
        --start $ref --no-plan `
        --solve-ms $MsB --seed $Seed `
        --adapt solutions --adapt-passes 4 `
        --guard "5:Crystal Wing|Zalen@terrain+Junk Signal@main" --guard-off "mainadv<=2" `
        --no-activate "Duel Evolution - Assault Zone" `
        --no-chain Zalen --no-chain "Crystal Wing" `
        --resolve "PSY-Framelord Omega@terrain:2" `
        --resolve "Trishula, Dragon of the Ice Barrier@terrain" `
        --options 256 `
        @Extra `
        --outdir $Out *> "$Out.log"
    Write-Output "    EXIT=$LASTEXITCODE"
}
foreach ($r in 1..3) {
    Invoke-BrasB "s13_ctxB_off_r$r"   '888' @()
    Invoke-BrasB "s13_ctxB_t1_r$r"    '888' @('--options-ctx', '1')
    Invoke-BrasB "s13_ctxB_t15_r$r"   '888' @('--options-ctx', '15')
}

function Invoke-BrasA {
    param([string]$Out, [string]$Seed, [string[]]$Extra)
    Write-Output "--- $Out (graine $Seed)"
    & .\bin\Release\combosolver.exe $gabarit `
        --scriptdir "D:\ProjectIgnis\replay2video\deps\compat_2026-08" `
        --scriptdir ..\deps\scripts_2026-04-13\script `
        --scriptdir "$repo\delta-bagooska\script" `
        --scriptdir "$repo\delta-puppet\script" `
        --deck "D:\ProjectIgnis\deck\Lunalight.ydk" `
        --hand "57103969|57103969|57103969" `
        --no-ref `
        --target 54701958 --target 54701958 --target 54701958 `
        --target "90590304@DEF" `
        --max-decisions 700 `
        --resolve 4731783 --resolve 2344618 --resolve 47705572 `
        --summon-min "54701958:3" `
        --hint 35618217 --hint 24550676 --hint 100460013 --hint 24094653 `
        --hint 48444114 --hint 83190280 --hint 50277355 --hint 14152693 `
        --adapt $CorpusA --adapt-passes 4 --options 256 `
        @Extra `
        --solve-ms $MsA --seed $Seed --finisher levin --archive-k 24 `
        --outdir $Out *> "$Out.log"
    Write-Output "    EXIT=$LASTEXITCODE"
}
foreach ($s in @('888', '1234')) {
    Invoke-BrasA "s13_ctxA_off_$s" $s @()
    Invoke-BrasA "s13_ctxA_t1_$s"  $s @('--options-ctx', '1')
    Invoke-BrasA "s13_ctxA_t15_$s" $s @('--options-ctx', '15')
}
Write-Output "TERMINE"
