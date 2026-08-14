# A/B sqrt-LTS dans le REGIME BUT SEUL (session 8).
#
# Pourquoi ce montage, et pas celui de la session 7ter. L'A/B precedent faisait
# partir le finisseur de reculs d'une solution CONNUE : suffixes courts, forte
# probabilite, regime ou le rerooting n'a rien a decomposer. Ici la reference
# est neutralisee (--no-plan) : la politique demarre uniforme et le finisseur
# reconstruit des lignes DE ZERO. C'est le regime sur lequel portait la
# prevision de cout (borne monolithique 10^26-10^60 contre 10^5,8-10^12,5
# decomposee) et le seul ou sqrt-LTS soit cense rendre.
#
# Cinq bras, meme graine, meme budget, sequentiels :
#   T    temoin (aucun rerooter)
#   R    --reroot        rerooter DUR sur les indices (2412.05196, session 7ter)
#   H8   --reroot-h 8    rerooter HEURISTIQUE doux (2605.30664 3.2)
#   H15  --reroot-h 15   alpha ~ log de la borne de cout par segment que la
#   H25  --reroot-h 25   prevision annoncait (10^5,8 -> 13,4 ; 10^12,5 -> 28,8)
#
# Le cadran H8/H15/H25 est la pour que le resultat soit lisible : une perte ou
# un gain MONOTONE dans le cadran est un vrai resultat, un point isole non
# (piege 46).
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
