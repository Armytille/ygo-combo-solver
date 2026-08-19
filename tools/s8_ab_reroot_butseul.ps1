# A/B of sqrt-LTS in the GOAL-ONLY regime.
#
# Why this setup rather than the earlier one. The previous A/B started the
# finisher from backtracks of a KNOWN solution: short suffixes, high
# probability, a regime where the rerooting has nothing to decompose. Here the
# reference is neutralised (--no-plan): the policy starts uniform and the
# finisher rebuilds lines FROM SCRATCH. That is the regime the cost forecast
# covered (monolithic bound 10^26-10^60 against 10^5.8-10^12.5 decomposed) and
# the only one where sqrt-LTS is supposed to deliver.
#
# Five arms, same seed, same budget, sequential:
#   T    control (no rerooter)
#   R    --reroot        HARD rerooter on the hints (arXiv:2412.05196)
#   H8   --reroot-h 8    soft HEURISTIC rerooter (arXiv:2605.30664 3.2)
#   H15  --reroot-h 15   alpha ~ log of the per-segment cost bound the
#   H25  --reroot-h 25   forecast announced (10^5.8 -> 13.4; 10^12.5 -> 28.8)
#
# The H8/H15/H25 dial exists so the result is readable: a MONOTONIC loss or
# gain across the dial is a real result, an isolated point is not.
param([int]$Ms = 600000, [string]$Seed = '888')

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
$ref = "D:\ProjectIgnis\replay\synchron handrip 2.yrpX"

$bras = @(
    @{ nom = 'T';   arg = @() },
    @{ nom = 'R';   arg = @('--reroot') },
    @{ nom = 'H8';  arg = @('--reroot-h', '8') },
    @{ nom = 'H15'; arg = @('--reroot-h', '15') },
    @{ nom = 'H25'; arg = @('--reroot-h', '25') }
)

foreach ($b in $bras) {
    $out = "s8_B_$($b.nom)_s$Seed"
    Write-Output "--- bras $($b.nom) -> $out"
    & .\bin\Release\combosolver.exe $ref `
        --scriptdir ..\deps\scripts_2026-04-13\script `
        --start $ref --no-plan `
        --solve-ms $Ms --seed $Seed `
        --no-chain Zalen --no-chain "Crystal Wing" `
        @($b.arg) --outdir $out *> "$out.log"
    Write-Output "    EXIT=$LASTEXITCODE"
}
Write-Output "TERMINE"
