# Lunalight v3: corrected settings, and the orientation taken from the reference
# combo (ygocombo #105, 108 steps, Nyarla replaced by Dugares).
#
# WHAT THE REFERENCE LINE TAUGHT, AND WHAT IS ENCODED HERE
#
# The three Liger Dancer come through THREE distinct routes, not the same one
# three times:
#   1. an ordinary Polymerization, with Kaleido Chick having copied the name of a
#      Leo Dancer sent from the Extra to the graveyard.
#   2. a Polymerization that BANISHES FROM THE GRAVEYARD, only possible after
#      Lunalight Masquerade's second resolution.
#   3. Lunalight WOLF in the Pendulum Zone, fusing by banishing the materials
#      from the graveyard.
# Routes 2 and 3 are therefore conditioned on two PRECISE and rare resolutions.
# That is exactly --resolve's use case: a rare event, checked at the goal, which
# also enters the gradient and the sampling bias.
#
# TWO EARLIER SETTINGS THAT KILLED THE COMBO, FIXED HERE
#
#  * --no-chain on Silver Hound is REMOVED. It targeted its negation effect, but
#    the line uses it at step 5 to revive Kaleido Chick from the Deck, a chained
#    activation the flag forbade. Both of its effects activate from the
#    graveyard, so no flag separates them: the "do not negate our own cards"
#    check is done AFTERWARDS, in judge mode on the line found.
#  * --max-decisions 700. The default ceiling is 1.5x the reference + 32, i.e.
#    399 here: the reference places only ONE Liger in 245 decisions, and the
#    target wants three. The search was truncated before the end, silently.
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
