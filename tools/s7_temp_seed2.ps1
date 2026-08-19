# Confirmation of the temperature on a SECOND seed. Two sequential arms
# (control tau=1.0 and tau=0.5) at seed 999: a structural fact is compared
# against a dispersion, not against one run.
param([string]$Seed = '999', [int]$FireMs = 300000)

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
foreach($t in @(1.0, 0.5)) {
    $tag = ([string]$t) -replace '[^0-9]',''
    $out = "s7_fire_s${Seed}_t$tag"
    & .\bin\Release\combosolver.exe "D:\ProjectIgnis\replay\synchron handrip 2.yrpX" `
        --scriptdir ..\deps\scripts_2026-04-13\script `
        --fire "27204311" --fire-open --fire-bake --fire-ms $FireMs `
        --fire-spare "Junk Signal" --fire-spare "Crystal Wing" --fire-spare "63436931" `
        --seed $Seed --outdir $out --nrpa-temp $t `
        --guard "5:Crystal Wing|Zalen@terrain+Junk Signal@main" --guard-off "mainadv<=2" `
        --no-activate "Duel Evolution - Assault Zone" `
        --no-chain Zalen --no-chain "Crystal Wing" `
        --resolve "PSY-Framelord Omega@terrain:2" `
        --resolve "Trishula, Dragon of the Ice Barrier@terrain" *> "$out.log"
    Write-Output "graine $Seed tau=$t -> $((Select-String -Path "$out.log" -Pattern 'verdict --fire').Line)"
}
