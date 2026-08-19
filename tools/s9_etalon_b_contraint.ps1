# BENCHMARK B WITH ITS LINE CONSTRAINTS.
#
# WHY THIS SETUP WAS MISSING, AND WHAT IT FIXES
#
# Every earlier benchmark B setup ran WITHOUT ANY constraint: no --resolve, no
# --summon-min, no --guard. The solver was therefore free to reach the board's
# eight codes by any shortcut, and that is exactly what it did: a 90 s run found
# lines CHEAPER than the reference because they do LESS. Those lines neither rip
# a rendu dix lignes a 8/8, cout 16/47/226 contre 19/56/276 pour la reference —
# nor guard.
#
# "8/8" with no constraints means "the eight codes are on the field", not "the
# line works". As an A/B metric it is valid (the same in every arm), but it is
# bras) ; comme jugement de qualite, non (pieges 66 et 67).
#
# not the combo. THE CONSTRAINTS, taken as they stand from the earlier setups
# that ran this deck (tools/s7_ab_reroot.ps1):
#
#   --guard "5:Crystal Wing|Zalen@terrain+Junk Signal@main"
#       from the 5th summon on, the line must hold an answer to Nibiru: Crystal
#       Wing or Zalen on the field, PLUS Junk Signal in hand.
#   --guard-off "mainadv<=2"
#       the guard goes off once the opponent's hand is down to 2 cards: a
#       handrip deck has removed the threat by then.
#   --resolve "PSY-Framelord Omega@terrain:2"
#   --resolve "Trishula, Dragon of the Ice Barrier@terrain"
#       THE RIPS. That is this line's reason for existing, and precisely what
#       the unconstrained setup did not require. Filtered by ACTIVATION zone:
#       Omega's graveyard effect does not count towards the handrip.
#       handrip (faux positif mesure, piege 38).
#   --no-activate "Duel Evolution - Assault Zone"
#       effet igne superflu, ecarte a l'enumeration.
#
# WHAT TO READ
#
# The `rips` column of the rollout lines, and the count of lines reaching the
# board. A line that reaches 8/8 WITH 3 rips and the guard held is the combo; a
# line at 8/8 with no rip is the shortcut the previous setup rewarded.
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
