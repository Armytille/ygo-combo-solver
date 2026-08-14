# Session 7 — A/B de la politique a DEUX NIVEAUX (chantier 5ter) sur l'etalon 2.
# Le mecanisme est teste SEUL (sans --adapt) : il agit sur l'apprentissage
# propre de chaque fenetre, pas sur un ensemencement par corpus. Bras temoin
# deja mesure deux fois (session 6 et s7_fire_base) : 10/15, dec. 113-173.
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
