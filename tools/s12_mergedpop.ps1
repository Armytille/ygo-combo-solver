# A/B DU DEPILAGE FUSIONNE (Arena::PopToAndRestore, session 12 suite).
#
# Equivalence exacte attendue avec k x Pop + Restore : l'etalon 0 doit rendre
# 42 exp./b=0/EPUISE et les MEMES best par racine dans les deux bras — seule
# la vitesse change (le profil du bras fusionne doit montrer Restore+Pop en
# baisse). Sante stricte en tete (flag eteint par defaut).
param([string]$Seed = '888')

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
$ref = "D:\ProjectIgnis\replay\synchron handrip 2.yrpX"

Write-Output "--- sante"
& .\bin\Release\combosolver.exe $ref `
    --scriptdir ..\deps\scripts_2026-04-13\script `
    --solve --solve-ms 60000 `
    --outdir s12_sante_mp --no-chain Zalen --no-chain "Crystal Wing" `
    *> s12_sante_mp.log
Write-Output "    EXIT=$LASTEXITCODE"

foreach ($b in @(@{ n = 'temoin'; a = @() },
                 @{ n = 'fusion'; a = @('--merged-pop') })) {
    $out = "s12_MP_$($b.n)"
    Write-Output "--- etalon 0 bras $($b.n) -> $out"
    & .\bin\Release\combosolver.exe $ref `
        --scriptdir ..\deps\scripts_2026-04-13\script `
        --start $ref --no-plan --no-nrpa `
        --approach s8_B_T_s888\best_approach_7of8.yrp --finisher-min 80000 `
        --solve-ms 100000 --seed $Seed `
        --no-chain Zalen --no-chain "Crystal Wing" `
        --profile @($b.a) --outdir $out *> "$out.log"
    Write-Output "    EXIT=$LASTEXITCODE"
}
Write-Output "TERMINE"
