# A/B DES OPTIONS (chantier 17, session 12) — premiere mesure du mecanisme.
#
# Deux bras sur LE MEME binaire, etalon B contraint + corpus --adapt (les deux
# bras adaptent pareil ; seul --options differe) :
#   temoin : --options 0    (mecanisme absent, comportement d'avant)
#   opt    : --options 256  (la seule forme que la prevision justifie —
#                            support 2, longueur <= 8, 93 % d'absorption)
#
# CE QU'IL FAUT LIRE, dans l'ordre (piege 52 puis piege 39) :
#   1. la ligne « options : N prises, M decisions absorbees (x/prise),
#      K avortees » — le mecanisme est-il VIVANT, et absorbe-t-il ~sa longueur
#      moyenne par prise ?
#   2. l'histogramme des resolutions atteintes (rips 0/0/0) et le best/8 —
#      les sorties STRUCTURELLES, comparees entre bras.
#   3. les compteurs tirages/etats sont DECLASSES (dispersion ±10-18 %) :
#      ne conclure sur eux qu'a ecart massif et coherent sur les 3 paires.
#
# La sante stricte passe en tete (options eteintes par defaut : le moteur par
# defaut doit rendre les MEMES nombres que s12_sante.log).
param([int]$Ms = 60000, [string]$Seed = '888')

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
$ref = "D:\ProjectIgnis\replay\synchron handrip 2.yrpX"

Write-Output "--- sante (mecanisme eteint par defaut)"
& .\bin\Release\combosolver.exe $ref `
    --scriptdir ..\deps\scripts_2026-04-13\script `
    --solve --solve-ms 60000 `
    --outdir s12_sante_opt --no-chain Zalen --no-chain "Crystal Wing" `
    *> s12_sante_opt.log
Write-Output "    EXIT=$LASTEXITCODE"

function Invoke-Bras {
    param([string]$Out, [string[]]$Extra)
    Write-Output "--- $Out"
    & .\bin\Release\combosolver.exe $ref `
        --scriptdir ..\deps\scripts_2026-04-13\script `
        --start $ref --no-plan `
        --solve-ms $Ms --seed $Seed `
        --adapt solutions --adapt-passes 4 `
        --guard "5:Crystal Wing|Zalen@terrain+Junk Signal@main" --guard-off "mainadv<=2" `
        --no-activate "Duel Evolution - Assault Zone" `
        --no-chain Zalen --no-chain "Crystal Wing" `
        --resolve "PSY-Framelord Omega@terrain:2" `
        --resolve "Trishula, Dragon of the Ice Barrier@terrain" `
        @Extra `
        --outdir $Out *> "$Out.log"
    Write-Output "    EXIT=$LASTEXITCODE"
}

foreach ($r in 1..3) {
    Invoke-Bras "s12_O_temoin_r$r" @()
    Invoke-Bras "s12_O_opt_r$r" @('--options', '256')
}
Write-Output "TERMINE"
