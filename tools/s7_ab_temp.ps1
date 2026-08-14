# Session 7bis — A/B de la TEMPERATURE (chantier 5quater) sur l'etalon 2.
# Le seul levier connu qui agisse sur la MASSE et non sur le classement.
# Deux valeurs pour lire le SENS de la pente, pas un point isole (le temoin
# tau=1.0 vaut 10/15, mesure deux fois).
param([double[]]$Temps = @(0.5, 0.25), [int]$FireMs = 300000, [string]$Seed = '888')

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
foreach($t in $Temps) {
    $tag = ([string]$t) -replace '[^0-9]',''
    $out = "s7_fire_t$tag"
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
    Write-Output "tau=$t -> $(Select-String -Path "$out.log" -Pattern 'verdict --fire')"
}
