# Lunalight : 3x Liger Dancer + Bagooska, main 3x Fire Formation - Tenki,
# effet d'A Bao A Qu resolu.
#
# Le replay en argument positionnel n'est qu'un GABARIT de duel (en-tete :
# graine, format, LP, pioche ; et deck adverse) — l'outil n'a pas encore de
# mode « duel depuis une decklist seule ». Le board cible qu'il apporte est
# integralement efface par les huit --board-remove avant que les --board-add
# n'ecrivent l'objectif reel.
#
# NB : --scriptdir DESACTIVE le scan automatique de repositories/ (assets.cpp,
# branche else). Les depots doivent donc etre redonnes a la main, sans quoi les
# cartes qui n'existent QUE la (ici Lunalight Scarlet Tiger, dans
# delta-bagooska/script/pre-release/) tournent INERTES et la ligne cherchee
# n'existe tout simplement pas pour le moteur.
#
# Leviers donnes a la recherche, ceux prevus pour les evenements RARES :
#  --summon-min : les invocations de Liger Dancer entrent dans le GRADIENT du
#                 but (pas seulement dans le test final) — c'est le mecanisme
#                 qui avait debloque Junk Meister.
#  --hint       : prime d'echantillonnage sur les pieces de la ligne
#                 (Kaleido Chick copie un Leo Dancer envoye a l'Extra->GY,
#                 Wolf fusionne EN BANNISSANT depuis le cimetiere, Masquerade
#                 rouvre le GY comme materiau).
param([int]$Ms = 900000, [string]$Seed = '888', [string]$Out = 's7_luna_full')

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
& .\bin\Release\combosolver.exe "D:\ProjectIgnis\replay\synchron handrip 2.yrpX" `
    --scriptdir ..\deps\scripts_2026-04-13\script `
    --scriptdir "D:\ProjectIgnis\repositories\delta-bagooska\script" `
    --scriptdir "D:\ProjectIgnis\repositories\delta-puppet\script" `
    --deck "D:\ProjectIgnis\deck\Lunalight.ydk" `
    --hand "57103969|57103969|57103969" `
    --board-remove 4891376 --board-remove 27572350 --board-remove 9753964 `
    --board-remove 50954680 --board-remove 63436931 --board-remove 29053656 `
    --board-remove 26387390 --board-remove 91002901 `
    --board-add 54701958 --board-add 54701958 --board-add 54701958 `
    --board-add "90590304@DEF" `
    --resolve 4731783 `
    --summon-min "54701958:3" `
    --hint 35618217 --hint 47705572 --hint 24550676 --hint 87931906 `
    --hint 24094653 --hint 2344618 --hint 83190280 `
    --solve-ms $Ms --seed $Seed --finisher levin --archive-k 24 `
    --outdir $Out *> "$Out.log"
Write-Output "EXIT=$LASTEXITCODE"
