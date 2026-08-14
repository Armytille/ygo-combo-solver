# Lunalight v3 — reglages corriges et orientation tiree du combo de reference
# (ygocombo #105, 108 etapes, Nyarla remplace par Dugares).
#
# CE QUE LA LIGNE DE REFERENCE A APPRIS, ET QUI EST ENCODE ICI
#
# Les trois Liger Dancer passent par TROIS voies distinctes, pas trois fois la
# meme :
#   1. Polymerization ordinaire, avec Kaleido Chick ayant copie le nom d'un Leo
#      Dancer envoye de l'Extra au cimetiere.
#   2. Polymerization qui BANNIT DEPUIS LE CIMETIERE — possible seulement apres
#      la seconde resolution de Lunalight Masquerade.
#   3. Lunalight WOLF en zone Pendule, qui fusionne en bannissant les materiaux
#      du cimetiere.
# Les voies 2 et 3 sont donc conditionnees a deux resolutions PRECISES et rares.
# C'est exactement le cas d'usage de --resolve : un evenement rare, controle au
# but, qui entre aussi dans le gradient et le biais d'echantillonnage.
#
# DEUX REGLAGES PRECEDENTS QUI TUAIENT LE COMBO, CORRIGES ICI
#
#  * --no-chain sur Silver Hound est RETIRE. Il visait son effet de negate, mais
#    la ligne l'utilise a l'etape 5 pour ressusciter Kaleido Chick depuis le
#    Deck — une activation en chaine, que le drapeau interdisait. Ses deux
#    effets s'activant depuis le cimetiere, aucun drapeau ne les separe : le
#    controle « ne nege pas nos propres cartes » se fera A POSTERIORI, en mode
#    juge sur la ligne trouvee.
#  * --max-decisions 700. Le plafond par defaut vaut 1,5x la reference + 32,
#    soit 399 ici : la reference ne pose qu'UN Liger en 245 decisions, la cible
#    en veut trois. La recherche etait tronquee avant la fin, sans le dire.
param([int]$Ms = 1800000, [string]$Seed = '888', [string]$Out = 's7_luna_v6')

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
$repo = "D:\ProjectIgnis\repositories"
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
    --max-decisions 700 `
    --resolve 4731783 `
    --resolve 2344618 `
    --resolve 47705572 `
    --summon-min "54701958:3" `
    --hint 35618217 --hint 24550676 --hint 100460013 --hint 24094653 `
    --hint 48444114 --hint 83190280 --hint 50277355 --hint 14152693 `
    --solve-ms $Ms --seed $Seed --finisher levin --archive-k 24 `
    --outdir $Out *> "$Out.log"
Write-Output "EXIT=$LASTEXITCODE"
