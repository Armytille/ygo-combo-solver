# AMORCE DU GRAPHE DE RECETTES — MESUREE SUR L'ETALON A, ET POURQUOI LA.
#
# LE MONTAGE DE LA SESSION 9 (tools/s9_recettes_amorce.ps1) TOURNE SUR L'ETALON
# B, ET C'EST LE MAUVAIS ENDROIT POUR CE MECANISME.
#
# L'amorce ne retient que les materiaux NOMMES ENTRE GUILLEMETS dans la ligne de
# texte. Le board de l'etalon B est fait de Synchros et de Liens — « 1 Tuner +
# 1+ non-Tuner monsters », « 2+ monsters » : aucun nom entre guillemets, donc
# aucune recette a poser. Le bras SA y mesure alors exactement le bras SN, et
# l'A/B ne tranche RIEN (piege 52 : lire d'abord « amorce par le texte : N
# recette(s) », et si N vaut zero, le mecanisme n'a pas tourne).
#
# Le cas qui commande le chantier est sur l'ETALON A : Liger Dancer exige
# NOMMEMENT « Lunalight Leo Dancer ». C'est la seule des deux lignes ou l'amorce
# a quelque chose a dire, et le §9.16 (j) l'a deja chiffree hors recherche
# (distance 2 contre un `h` plat de 1). Reste a le lire DANS le solveur, sur la
# colonne hR d'un vrai finisseur.
#
# MESURE FAITE, ET ELLE CONFIRME : sur l'etalon B, les bras SN et SA de
# tools/s9_recettes_amorce.ps1 rendent des hR IDENTIQUES au dixieme
# (1,0 / 4,1 / 5,0 / 5,1 / 5,0) et les memes best. Une seule recette y est
# amorcee. Le montage de la session 9 ne pouvait rien trancher.
#
# MONTAGE DETERMINISTE, calque sur l'etalon 0 : racines imposees (--approach),
# politique vide (--no-nrpa). Seul le graphe change entre les bras.
#
#   AN   --no-seed-recipes   observation seule — le temoin d'amorce
#   AA   --no-seed-quant     amorce des seuls materiaux NOMMES (etat session 9,
#                            plus la correction de zone : le deck et l'extra ne
#                            sont plus comptes comme materiaux disponibles)
#   AQ   (defaut)            amorce complete : nommes + exigences CARDINALES
#   AP1  --recipes 1         l'amorce complete PESE dans h
#
# CE QU'IL FAUT LIRE, DANS CET ORDRE
#  1. La table « distance amorcee » de l'en-tete : elle sort du graphe lui-meme
#     et se verifie a la main. Liger Dancer doit y valoir plus de 1.
#  2. `hR` de AQ contre AA contre AN, racine par racine. C'EST LA MESURE.
#  3. Seulement ensuite : best / cartes cumulees de AP1 contre AN.
param([int]$Ms = 300000, [int]$FinMin = 260000, [string]$Seed = '888',
      [string]$Approche = 's7_luna_v6\best_approach_2of4.yrp')

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
$repo = "D:\ProjectIgnis\repositories"
$gabarit = "D:\ProjectIgnis\replay\_LastReplay.yrpX"

$bras = @(
    @{ nom = 'AN';  arg = @('--recipes', '0', '--no-seed-recipes') },
    @{ nom = 'AA';  arg = @('--recipes', '0', '--no-seed-quant') },
    @{ nom = 'AQ';  arg = @('--recipes', '0') },
    @{ nom = 'AP1'; arg = @('--recipes', '1') }
)

foreach ($b in $bras) {
    $out = "s10_A_$($b.nom)"
    Write-Output "--- etalon A amorce, bras $($b.nom) -> $out"
    & .\bin\Release\combosolver.exe $gabarit `
        --scriptdir "D:\ProjectIgnis\replay2video\deps\compat_2026-08" `
        --scriptdir ..\deps\scripts_2026-04-13\script `
        --scriptdir "$repo\delta-bagooska\script" `
        --scriptdir "$repo\delta-puppet\script" `
        --deck "D:\ProjectIgnis\deck\Lunalight.ydk" `
        --hand "57103969|57103969|57103969" `
        --no-ref `
        --target 54701958 --target 54701958 --target 54701958 `
        --target "90590304@DEF" `
        --max-decisions 700 `
        --resolve 4731783 --resolve 2344618 --resolve 47705572 `
        --summon-min "54701958:3" `
        --hint 35618217 --hint 24550676 --hint 100460013 --hint 24094653 `
        --hint 48444114 --hint 83190280 --hint 50277355 --hint 14152693 `
        --no-nrpa --approach $Approche --finisher-min $FinMin `
        --solve-ms $Ms --seed $Seed --finisher levin --archive-k 24 `
        @($b.arg) --outdir $out *> "$out.log"
    Write-Output "    EXIT=$LASTEXITCODE"
}
Write-Output "TERMINE"
