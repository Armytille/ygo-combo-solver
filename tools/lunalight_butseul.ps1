# BENCHMARK A in GOAL-ONLY MODE: Lunalight, with no reference replay.
#
# WHAT CHANGES FROM tools/lunalight3.ps1, AND NOTHING ELSE
#
#  * --no-ref: _LastReplay.yrpX is only a duel TEMPLATE now (flags, life points,
#    hand size, opponent deck). Its line, its end-of-turn board and its
#    REPERTOIRE are all set aside. The report prints the template so that this
#    is verifiable rather than promised.
#  * --target: the target board is POSTED, no longer edited. lunalight3 started
#    from the reference's capture and emptied it with six --board-remove; those
#    six codes were information about what the reference had placed.
#
# WHAT REMAINS, AND WHY IT IS NOT THE REFERENCE IN DISGUISE
#
#  --resolve / --summon-min / --hint / --max-decisions are DOMAIN KNOWLEDGE
#  written by hand: "the line must resolve Masquerade twice", "three Ligers are
#  needed". In Bonet & Geffner's sense that is a SKETCH, not a demonstration
#  (see docs/etat-de-lart-but-seul.md, direction 3). The --hint entries stay the
#  most debatable part: they were chosen by looking at the reference combo. The
#  HINTLESS arm removes them to quantify what they are worth.
#
# GROUND TRUTH, AND WHAT HAS TO BE BEATEN
#
#  The deck reaches 3x Liger Dancer in 108 steps (ygocombo #105): the target is
#  REACHABLE, and that is what makes this benchmark worth having.
#
#  Best known result, WITH the repertoire (s7_luna_v6, 1800 s, seed 888):
#    tirages   1 246 s, 12,66 M tirages, best 1/4
#    finisher best 1/4 (one root climbs to 2/4 at backtrack 40)
#    LDS at 0 deviations  2 038 555 states, 294 s, best 2/4  <-- THE BEST
#    board atteint : 1 Liger + Bagooska ; MANQUE 2x Liger. 0 erreur de core.
#
#  So the best came from the pass that FOLLOWS THE PLAN. Under --no-plan that
#  pass becomes structurally EMPTY (measured on benchmark B: 26 states at every
#  deviation level): goal-only mode loses exactly the mechanism that produced
#  the 2/4. That is to be quantified, not worked around.
#
#  And propositional HER (arXiv:2512.19355, the Delivery domain) predicts that
#  "n copies of the same thing" is precisely the goal shape where these methods
#  learn to place ONE and stop there.
param([int]$Ms = 1800000, [string]$Seed = '888', [string]$Bras = 'tous')

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
$repo = "D:\ProjectIgnis\repositories"
$gabarit = "D:\ProjectIgnis\replay\_LastReplay.yrpX"

function Invoke-Bras([string]$nom, [string[]]$extra, [bool]$hints) {
    $out = "s8_A_$nom"
    Write-Output "--- etalon A bras $nom -> $out"
    $hintArgs = @()
    if ($hints) {
        $hintArgs = @('--hint','35618217','--hint','24550676','--hint','100460013',
                      '--hint','24094653','--hint','48444114','--hint','83190280',
                      '--hint','50277355','--hint','14152693')
    }
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
        @hintArgs @extra `
        --solve-ms $Ms --seed $Seed --finisher levin --archive-k 24 `
        --outdir $out *> "$out.log"
    Write-Output "    EXIT=$LASTEXITCODE"
}

if ($Bras -eq 'tous' -or $Bras -eq 'base')  { Invoke-Bras 'base'  @() $true }
if ($Bras -eq 'tous' -or $Bras -eq 'rh')    { Invoke-Bras 'rh15'  @('--reroot-h','15') $true }
Write-Output "TERMINE"
