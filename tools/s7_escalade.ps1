# Session 7 — escalade enracinee sur la meilleure ligne connue (19/55/259).
# Trois graines sequentielles, budgets 600 s. Aucune mesure en parallele.
param([string[]]$Seeds = @('888','999','1234'),
      [string]$Approach = 'sZ6_slack255/solution_00_b19_a55.yrp',
      [int]$Ms = 600000)

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
foreach($s in $Seeds) {
    $out = "s7_esc$s"
    Write-Output "=== escalade graine $s -> $out ==="
    & .\bin\Release\combosolver.exe "D:\ProjectIgnis\replay\synchron handrip 2.yrpX" `
        --scriptdir ..\deps\scripts_2026-04-13\script `
        --start "D:\ProjectIgnis\replay\synchron handrip 2.yrpX" `
        --solve-ms $Ms --seed $s --finisher levin --optimize `
        --finisher-min 420000 --burn-limit 19 --archive-k 24 `
        --approach $Approach `
        --outdir $out `
        --guard "5:Crystal Wing|Zalen@terrain+Junk Signal@main" --guard-off "mainadv<=2" `
        --no-activate "Duel Evolution - Assault Zone" `
        --no-chain Zalen --no-chain "Crystal Wing" `
        --resolve "PSY-Framelord Omega@terrain:2" `
        --resolve "Trishula, Dragon of the Ice Barrier@terrain" 2>&1 |
        Tee-Object -FilePath "$out.log" | Select-String -Pattern 'brulees|BUT|anytime|solution' |
        Select-Object -Last 3
}
Write-Output "=== escalade terminee ==="
