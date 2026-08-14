# CHIFFRER LES OPTIONS AVANT DE LES ECRIRE (chantier 17).
#
# L'argument des options est le seul de la revue a toucher l'EXPOSANT : le mur
# du §9.14 est 0,74^160, mais 0,74^20 vaut 2.10^-3. Si une macro remplace huit
# decisions par une, la ligne passe de 160 a ~20 et on quitte « jamais » pour
# « deux tirages sur mille ».
#
# Le critere de selection n'est pas a inventer : Alikhasi & Lelis
# (arXiv:2410.11262) choisissent leurs options en MINIMISANT la perte de Levin,
# et cette perte est ce que le rapport calcule deja sur le corpus. Le gain est
# donc chiffrable AVANT d'ecrire le mecanisme — exactement comme alpha l'a ete
# pour sqrt-LTS (piege 40).
#
# LE RUN NE CHERCHE RIEN : --solve-ms 1000. Tout ce qui nous interesse est
# imprime par le rejeu d'adaptation, avant la recherche.
#
# CE QU'IL FAUT LIRE : la table « prevision du gain des OPTIONS ». La colonne
# `log10 d/pi` donne la borne de Levin avant -> apres. Un ecart de trois ordres
# de grandeur ou plus justifie d'ouvrir le chantier ; un ecart d'un demi-ordre
# ne le justifie pas, et le chiffre aura coute 30 secondes au lieu d'une
# session. La colonne `absorbe` dit quelle fraction des decisions du corpus
# tombe dans une macro : a 5 %, le catalogue ne mord sur rien.
param([string]$Corpus = 'solutions', [string]$Seed = '888')

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
$ref = "D:\ProjectIgnis\replay\synchron handrip 2.yrpX"

$out = "s10_options"
& .\bin\Release\combosolver.exe $ref `
    --scriptdir ..\deps\scripts_2026-04-13\script `
    --solve --solve-ms 1000 --seed $Seed `
    --adapt $Corpus --adapt-passes 4 `
    --no-chain Zalen --no-chain "Crystal Wing" `
    --outdir $out *> "$out.log"
Write-Output "EXIT=$LASTEXITCODE"
Write-Output "TERMINE"
