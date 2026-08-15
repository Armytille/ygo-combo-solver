# SUITE DE L'AUDIT LITTERATURE (session 12) — deux A/B en une batterie.
#
# 1. SANTE stricte (tous les mecanismes nouveaux eteints par defaut).
# 2. PHS* CANONIQUE (etalon 0, deterministe) : cout (d+h)/pi du papier contre
#    notre log(d+1)+h-log pi (facteur e^h). Controle : recul 0 = 42 exp., b=0,
#    EPUISE dans les deux bras (l'ensemble atteignable ne depend pas du cout).
#    Gain : expansions a temps egal + best par racine.
# 3. OPTIONS v3 (etalon B contraint + --adapt) : selection par perte de Levin
#    (s'auto-limite : ~18 macros au lieu de 256) et fenetre de position.
#    Bras : temoin / selection seule / selection + fenetre 16.
#    Lire : triptyque prises-absorbees-avortees, >=k resolutions, best.
param([string]$Seed = '888')

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
$ref = "D:\ProjectIgnis\replay\synchron handrip 2.yrpX"

Write-Output "--- sante"
& .\bin\Release\combosolver.exe $ref `
    --scriptdir ..\deps\scripts_2026-04-13\script `
    --solve --solve-ms 60000 `
    --outdir s12_sante_v3 --no-chain Zalen --no-chain "Crystal Wing" `
    *> s12_sante_v3.log
Write-Output "    EXIT=$LASTEXITCODE"

foreach ($b in @(@{ n = 'temoin'; a = @() },
                 @{ n = 'canon';  a = @('--phs-canonical') })) {
    $out = "s12_PHS_$($b.n)"
    Write-Output "--- etalon 0 bras $($b.n) -> $out"
    & .\bin\Release\combosolver.exe $ref `
        --scriptdir ..\deps\scripts_2026-04-13\script `
        --start $ref --no-plan --no-nrpa `
        --approach s8_B_T_s888\best_approach_7of8.yrp --finisher-min 80000 `
        --solve-ms 100000 --seed $Seed `
        --no-chain Zalen --no-chain "Crystal Wing" `
        @($b.a) --outdir $out *> "$out.log"
    Write-Output "    EXIT=$LASTEXITCODE"
}

function Invoke-Bras {
    param([string]$Out, [string[]]$Extra)
    Write-Output "--- $Out"
    & .\bin\Release\combosolver.exe $ref `
        --scriptdir ..\deps\scripts_2026-04-13\script `
        --start $ref --no-plan `
        --solve-ms 60000 --seed $Seed `
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
    Invoke-Bras "s12_O3_temoin_r$r" @()
    Invoke-Bras "s12_O3_sel_r$r"    @('--options', '256')
    Invoke-Bras "s12_O3_win_r$r"    @('--options', '256', '--options-window', '16')
}
Write-Output "TERMINE"
