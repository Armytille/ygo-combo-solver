# CADRAN DU NOMBRE DE WORKERS (session 12, suite) — jamais mesure.
#
# Le 6ter le predit depuis le debut : le jeu de travail d'un duel tient en
# quelques Mo, le nombre optimal de workers est probablement SOUS le nombre de
# threads materiels (contention L3 : seize arenes de ~8 Mo se disputent le
# cache). L'instrument est le µs/appel de Process (phase tirages, --profile) :
# la contention s'y lit directement, la ou le compteur de tirages est declasse.
#
# LIRE : « Process (core) » de la phase tirages (colonne µs/appel) par bras,
# ET le debit par worker (tirages / N). Deux repetitions intercalees.
param([int]$Ms = 60000, [string]$Seed = '888')

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
$ref = "D:\ProjectIgnis\replay\synchron handrip 2.yrpX"

function Invoke-Bras {
    param([int]$N, [int]$Rep)
    $out = "s12_T${N}_r$Rep"
    Write-Output "--- $out (threads=$N)"
    & .\bin\Release\combosolver.exe $ref `
        --scriptdir ..\deps\scripts_2026-04-13\script `
        --start $ref --no-plan `
        --solve-ms $Ms --seed $Seed --threads $N `
        --guard "5:Crystal Wing|Zalen@terrain+Junk Signal@main" --guard-off "mainadv<=2" `
        --no-activate "Duel Evolution - Assault Zone" `
        --no-chain Zalen --no-chain "Crystal Wing" `
        --resolve "PSY-Framelord Omega@terrain:2" `
        --resolve "Trishula, Dragon of the Ice Barrier@terrain" `
        --profile `
        --outdir $out *> "$out.log"
    Write-Output "    EXIT=$LASTEXITCODE"
}

foreach ($r in 1..2) {
    foreach ($n in @(8, 12, 16, 24)) { Invoke-Bras $n $r }
}
Write-Output "TERMINE"
