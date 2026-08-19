# A/B of the TWO-LEVEL policy on benchmark 2. The mechanism is tested ALONE
# (without --adapt): it acts on each window's own learning, not on a corpus
# seeding. The control arm has already been measured twice (10/15,
# decisions 113-173).
param([double]$Shrink = 1.0, [int]$FireMs = 300000, [string]$Seed = '888')

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
$out = "s7_fire_ctx"
& .\bin\Release\combosolver.exe "D:\ProjectIgnis\replay\synchron handrip 2.yrpX" `
    --scriptdir ..\deps\scripts_2026-04-13\script `
    --fire "27204311" --fire-open --fire-bake --fire-ms $FireMs `
    --fire-spare "Junk Signal" --fire-spare "Crystal Wing" --fire-spare "63436931" `
    --seed $Seed --outdir $out --ctx-shrink $Shrink `
    --guard "5:Crystal Wing|Zalen@terrain+Junk Signal@main" --guard-off "mainadv<=2" `
    --no-activate "Duel Evolution - Assault Zone" `
    --no-chain Zalen --no-chain "Crystal Wing" `
    --resolve "PSY-Framelord Omega@terrain:2" `
    --resolve "Trishula, Dragon of the Ice Barrier@terrain" *> "$out.log"
Write-Output "EXIT=$LASTEXITCODE"
