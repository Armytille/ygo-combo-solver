# RECIPE GRAPH SEEDED FROM CARD TEXT (second step).
#
# WHAT THE FIRST A/B ESTABLISHED, and what it commands here
#
# Arm `--recipes 0` (graph fed and measured, not weighing in the cost):
#   appr0 backtrack 0    rec=121    hR=1.0    (flat h at the same place: 1)
#   appr0 recul 10   rec=22240  hR=4.1    (h plat : 4)
#   appr0 recul 20   rec=24518  hR=5.0    (h plat : 5)
#   appr0 recul 30   rec=44324  hR=5.1    (h plat : 6)
#   appr0 recul 45   rec=14885  hR=5.0    (h plat : 5)
#
# The graph OBSERVES a great deal (tens of thousands of summons) and brings only
# ~0.1 of gradient. The reason is not a lack of data, it is STRUCTURAL: an
# observational graph only learns from SUCCESSFUL summons, and the card being
# sought is the one no line has ever placed. Its recipe is therefore unknown,
# `Distance` returns its floor, and `h` stays flat exactly where it should be
# informative.
#
# WHAT THIS SECOND A/B TESTS
#
# Rule 3 says "the text is only a SEED; the truth comes from observation". The
# first step implemented observation alone. The seeding reads the materials line
# of the card text
#     "Lunalight Leo Dancer" + 3 "Lunalight" monsters
# and keeps only the materials NAMED in quotes. Liger Dancer thereby gets a
# recipe requiring Leo Dancer BY NAME, which is itself a Fusion: the distance to
# Liger stops being the floor.
#
# WHAT TO READ, in this order:
#
# 1. `seeded from text: N recipe(s) posted` in the header. At zero, nothing is
#    seeded and the arms measure nothing.
# 2. `hR` of arm SA against `hR` of arm M above. THAT IS THE MEASUREMENT: if hR
#    does not rise, the seeding brought nothing and this form of the work is
#    refuted. If hR rises, the landscape got deeper, and only then does "does it
#    win" mean anything.
# 3. Cumulated cards of the weighted arms against the control (20).
#
# Bras :
#   SA   --recipes 0            : seeding + observation, MEASURED without weight
#   SN   --recipes 0 --no-seed-recipes : temoin d'amorce (= bras M precedent)
#   SP1  --recipes 1            : the seeding weighs in h
#   SP2  --recipes 2
param([int]$Ms = 300000,
      [int]$FinMin = 260000,
      [string]$Seed = '888',
      [string]$Approche = 's8_B_T_s888\best_approach_7of8.yrp')

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
$ref = "D:\ProjectIgnis\replay\synchron handrip 2.yrpX"

# SN is the SEEDING CONTROL, and it is not optional: arm M above was measured on
# a binary from BEFORE the seeding. Comparing SA against that M would vary two
# things (the seeding and the binary), exactly what the alpha dial had to be
# redone for. SN therefore replays the control on THE current binary.
$bras = @(
    @{ nom = 'SN';  arg = @('--recipes', '0', '--no-seed-recipes') },
    @{ nom = 'SA';  arg = @('--recipes', '0') },
    @{ nom = 'SP1'; arg = @('--recipes', '1') },
    @{ nom = 'SP2'; arg = @('--recipes', '2') }
)

foreach ($b in $bras) {
    $out = "s9s_$($b.nom)"
    Write-Output "--- amorce bras $($b.nom) -> $out"
    & .\bin\Release\combosolver.exe $ref `
        --scriptdir ..\deps\scripts_2026-04-13\script `
        --start $ref --no-plan --no-nrpa `
        --approach $Approche --finisher-min $FinMin `
        --solve-ms $Ms --seed $Seed `
        --no-chain Zalen --no-chain "Crystal Wing" `
        @($b.arg) --outdir $out *> "$out.log"
    Write-Output "    EXIT=$LASTEXITCODE"
}
Write-Output "TERMINE"
