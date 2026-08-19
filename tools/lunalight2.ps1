# Lunalight v2: template = _LastReplay.yrpX (the hand test the player played),
# Silver Hound muzzled.
#
# The recorded line places ONE Liger Dancer (+ Black Sheep, Kaleido Chick and
# three spells): the six --board-remove entries empty that capture and the
# --board-add ones write the objective. What is kept from the template is its
# PLAN, i.e. the repertoire of Lunalight moves the NRPA policy takes as a bias,
# and which every earlier run lacked.
#
# Why _LastReplay as the template: it only serves as a duel TEMPLATE (header +
# opponent deck). Unlike the synchron replay it comes from the same context as
# the question asked. And since its line does not cross the turn change, NO
# target board is captured there (fingerprint 0000...): the --board-add entries
# therefore build the target from NOTHING, with no need to erase eight foreign
# cards first.
#
# --no-chain rather than --guard: --guard checks that a counter is AVAILABLE at
# every opponent window, which is not the request. What is needed here is to
# forbid the player from CHAINING Silver Hound, whose effect negates the
# activation of a spell/trap on the field, ours included.
#
# Script note: --scriptdir disables the automatic scan of repositories/, so they
# must be given back. Even then, Lunalight Scarlet Tiger stays unusable: its
# pre-release script calls Effect:IsCardSetcode(), an API absent from the pinned
# ocgcore (462 core errors measured). Its 3 copies are inert until the core is
# moved up.
param([int]$Ms = 900000, [string]$Seed = '888', [string]$Out = 's7_luna_v3')

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
$repo = "D:\ProjectIgnis\repositories"
# Scarlet Tiger: made FUNCTIONAL by deps/compat_2026-08/utility.lua, which
# defines the one missing method (Effect.IsCardSetcode). Without it the card
# errored 462 times per replay, hence stayed inert, hence any combo line using
# it was invisible to the search. After the fix: 0 MSG_RETRY AND 0 core errors,
# both at once, which no other configuration gave. Must stay FIRST: it is a
# replacement of utility.lua.
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
