# Lunalight: 3x Liger Dancer + Bagooska, hand 3x Fire Formation - Tenki,
# A Bao A Qu's effect resolved.
#
# The replay in positional argument is only a duel TEMPLATE (header: seed,
# format, life points, draw; and the opponent deck). The target board it brings
# is wiped entirely by the eight --board-remove entries before the --board-add
# ones write the real objective.
#
# Note: --scriptdir DISABLES the automatic scan of repositories/ (assets.cpp,
# else branch). The repositories must therefore be given back by hand, otherwise
# the cards that exist ONLY there (here Lunalight Scarlet Tiger, in
# delta-bagooska/script/pre-release/) run INERT and the line being searched for
# simply does not exist as far as the engine is concerned.
#
# Levers given to the search, the ones meant for RARE events:
#  --summon-min: Liger Dancer's summons enter the goal's GRADIENT (not only the
#                final test); it is the mechanism that unlocked Junk Meister.
#  --hint      : sampling bonus on the line's pieces (Kaleido Chick copies a Leo
#                Dancer sent from the Extra to the graveyard, Wolf fuses BY
#                BANISHING from the graveyard, Masquerade reopens the
#                graveyard as a material source).
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
