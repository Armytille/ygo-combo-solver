# Lunalight v2 — temoin = _LastReplay.yrpX (le hand test joue par le joueur),
# Silver Hound musele.
#
# La ligne enregistree pose UN Liger Dancer (+ Black Sheep, Kaleido Chick et
# trois Sorts) : les six --board-remove vident cette capture, les --board-add
# ecrivent l'objectif vise. Ce qu'on garde du temoin, c'est son PLAN — le
# repertoire de coups Lunalight que la politique NRPA prend en biais, et qui
# manquait a tous les runs precedents.
#
# Pourquoi _LastReplay comme temoin : il ne sert que de GABARIT de duel
# (en-tete + deck adverse). Contrairement au replay synchron, il vient du meme
# contexte que la question posee. Et comme sa ligne ne franchit pas le
# changement de tour, AUCUN board cible n'y est capture (empreinte 0000...) :
# les --board-add construisent donc la cible a partir de RIEN, sans avoir a
# effacer huit cartes etrangeres au prealable.
#
# --no-chain plutot que --guard : --guard verifie qu'un contre est DISPONIBLE a
# chaque fenetre adverse, ce n'est pas la demande. Ce qu'il faut ici, c'est
# interdire au joueur de CHAINER Silver Hound — dont l'effet negate
# l'activation d'un Sort/Piege sur le terrain, y compris les notres.
#
# NB scripts : --scriptdir desactive le scan automatique de repositories/, il
# faut donc les redonner. Meme ainsi, Lunalight Scarlet Tiger reste inutilisable
# — son script pre-release appelle Effect:IsCardSetcode(), API absente de
# l'ocgcore epingle (462 erreurs de core mesurees). Ses 3 exemplaires sont
# inertes tant que le core n'est pas remonte.
param([int]$Ms = 900000, [string]$Seed = '888', [string]$Out = 's7_luna_v3')

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
$repo = "D:\ProjectIgnis\repositories"
# Scarlet Tiger : rendue FONCTIONNELLE par deps/compat_2026-08/utility.lua, qui
# definit la seule methode manquante (Effect.IsCardSetcode). Sans elle la carte
# errait 462 fois par rejeu, donc restait inerte, donc toute ligne de combo qui
# l'emploie etait invisible a la recherche. Apres correctif : 0 MSG_RETRY ET
# 0 erreur de core -- les deux a la fois, ce qu'aucune autre configuration ne
# donnait. Doit rester EN TETE : c'est un remplacement de utility.lua.
& .\bin\Release\combosolver.exe "D:\ProjectIgnis\replay\_LastReplay.yrpX" `
    --scriptdir "D:\ProjectIgnis\replay2video\deps\compat_2026-08" `
    --scriptdir ..\deps\scripts_2026-04-13\script `
    --scriptdir "$repo\delta-bagooska\script" `
    --scriptdir "$repo\delta-puppet\script" `
    --deck "D:\ProjectIgnis\deck\Lunalight.ydk" `
    --hand "57103969|57103969|57103969" `
    --board-remove 54701958 --board-remove 11317977 --board-remove 35618217 `
    --board-remove 57103969 --board-remove 2344618 --board-remove 83190280 `
    --board-add 54701958 --board-add 54701958 --board-add 54701958 `
    --board-add "90590304@DEF" `
    --resolve 4731783 `
    --summon-min "54701958:3" `
    --no-chain 35763582 `
    --hint 35618217 --hint 47705572 --hint 24550676 --hint 87931906 `
    --hint 24094653 --hint 2344618 --hint 83190280 `
    --solve-ms $Ms --seed $Seed --finisher levin --archive-k 24 `
    --outdir $Out *> "$Out.log"
Write-Output "EXIT=$LASTEXITCODE"
