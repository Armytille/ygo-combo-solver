# A/B of sqrt-LTS (--reroot) on BENCHMARK 1. BOTH arms run on the current
# binary: the control of the climb runs dates from an earlier binary, and an
# A/B is judged on contemporary arms even when the mechanism is supposed to be
# inert.
param([string]$Seed = '888', [int]$Ms = 600000)

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
foreach($arm in @(@{tag='base'; flag=@()}, @{tag='reroot'; flag=@('--reroot')})) {
    $out = "s7_rr_$($arm.tag)_$Seed"
    & .\bin\Release\combosolver.exe "D:\ProjectIgnis\replay\synchron handrip 2.yrpX" `
        --scriptdir ..\deps\scripts_2026-04-13\script `
        --start "D:\ProjectIgnis\replay\synchron handrip 2.yrpX" `
        --solve-ms $Ms --seed $Seed --finisher levin --optimize `
        --finisher-min 420000 --burn-limit 19 --archive-k 24 `
        --approach "sZ6_slack255/solution_00_b19_a55.yrp" `
        --outdir $out @($arm.flag) `
        --guard "5:Crystal Wing|Zalen@terrain+Junk Signal@main" --guard-off "mainadv<=2" `
        --no-activate "Duel Evolution - Assault Zone" `
        --no-chain Zalen --no-chain "Crystal Wing" `
        --resolve "PSY-Framelord Omega@terrain:2" `
        --resolve "Trishula, Dragon of the Ice Barrier@terrain" *> "$out.log"
    $best = (Select-String -Path "$out.log" -Pattern '^\s+0\s+\d+\s+\d+\s+\d+' | Select-Object -First 1).Line
    $conv = (Select-String -Path "$out.log" -Pattern '<-- BUT').Count
    Write-Output "$($arm.tag) : best=[$($best.Trim())] conversions=$conv"
}
