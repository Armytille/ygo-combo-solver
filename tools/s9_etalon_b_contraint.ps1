# ETALON B AVEC SES CONTRAINTES DE LIGNE (session 9).
#
# POURQUOI CE MONTAGE MANQUAIT, ET CE QU'IL CORRIGE
#
# Tous les montages de l'etalon B — ceux de la session 8 comme les miens —
# tournaient SANS AUCUNE contrainte : ni --resolve, ni --summon-min, ni --guard.
# Le solveur y etait donc libre d'atteindre les huit codes du board par
# n'importe quel raccourci, et c'est exactement ce qu'il a fait : un run de 90 s
# a rendu dix lignes a 8/8, cout 16/47/226 contre 19/56/276 pour la reference —
# MOINS CHER parce qu'il fait MOINS. Ces lignes ne rippent pas et ne gardent pas.
#
# « 8/8 » sans contraintes veut dire « les huit codes sont sur le terrain », pas
# « la ligne marche ». Comme metrique d'A/B c'est valide (la meme dans tous les
# bras) ; comme jugement de qualite, non (pieges 66 et 67).
#
# LES CONTRAINTES, reprises telles quelles des montages de la session 7 qui
# faisaient tourner ce deck (tools/s7_ab_reroot.ps1) :
#
#   --guard "5:Crystal Wing|Zalen@terrain+Junk Signal@main"
#       des la 5e invocation, la ligne doit tenir de quoi repondre a Nibiru :
#       Crystal Wing ou Zalen au terrain, PLUS Junk Signal en main.
#   --guard-off "mainadv<=2"
#       la garde s'eteint quand la main adverse est descendue a 2 cartes : un
#       deck handrip a alors deja retire la menace.
#   --resolve "PSY-Framelord Omega@terrain:2"
#   --resolve "Trishula, Dragon of the Ice Barrier@terrain"
#       LES RIPS. C'est la raison d'etre de cette ligne, et c'est precisement ce
#       que le montage sans contraintes n'exigeait pas. Filtres par zone
#       d'ACTIVATION : l'effet de cimetiere d'Omega ne compte pas pour le
#       handrip (faux positif mesure, piege 38).
#   --no-activate "Duel Evolution - Assault Zone"
#       effet igne superflu, ecarte a l'enumeration.
#
# CE QU'IL FAUT LIRE
#
# La colonne `rips` des lignes de tirages, et le compte de lignes atteignant le
# board. Une ligne qui atteint 8/8 AVEC 3 rips et la garde tenue est le combo ;
# une ligne a 8/8 sans rip est le raccourci que le montage precedent recompensait.
param([int]$Ms = 90000, [string]$Seed = '888')

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
$ref = "D:\ProjectIgnis\replay\synchron handrip 2.yrpX"

& .\bin\Release\combosolver.exe $ref `
    --scriptdir ..\deps\scripts_2026-04-13\script `
    --start $ref --no-plan `
    --solve-ms $Ms --seed $Seed `
    --guard "5:Crystal Wing|Zalen@terrain+Junk Signal@main" --guard-off "mainadv<=2" `
    --no-activate "Duel Evolution - Assault Zone" `
    --no-chain Zalen --no-chain "Crystal Wing" `
    --resolve "PSY-Framelord Omega@terrain:2" `
    --resolve "Trishula, Dragon of the Ice Barrier@terrain" `
    --outdir s9_B_cons *> s9_B_cons.log
Write-Output "EXIT=$LASTEXITCODE"
Write-Output "TERMINE"
