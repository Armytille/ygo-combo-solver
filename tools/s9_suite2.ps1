# SUITE 2: the partition fix, then the A/B of the SEEDED graph.
#
# 1. THE PARTITION FIX: the exact test, and it is cheap.
#
#    An earlier note wrote "with no plan, the bounded-discrepancy pass becomes
#    EMPTY: 26 states at every deviation level", and took that for a STRUCTURAL
#    fact of goal-only mode. The record (s8_B_noplan.log):
#
#      ecarts  solutions   etats   transpos.  coupures
#      0            0          1        0         0
#      1            0         26       16         0
#      2            0         26       16         0
#      3            0         26       16         0
#      ...          0         26       16         0     <-- CONSTANT
#
#    The defect explains that figure entirely: `claims_size = plan.size() + 1`
#    is 1 when the plan is empty, so ONE partition token for sixteen workers,
#    and every deviation is suppressed.
#
#    AFTER THE FIX, the number of states must GROW with the deviation level. If
#    it stays at 26, the fix is not in the binary. Read the `partition` column
#    too: non-zero = the work is CEDED (shared), which could not be told apart
#    from a SUPPRESSION.
#
# 2. A/B OF THE SEEDED GRAPH (tools/s9_recettes_amorce.ps1).
param([int]$Ms = 90000, [string]$Seed = '888')

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
$ref = "D:\ProjectIgnis\replay\synchron handrip 2.yrpX"

Write-Output "=== 1. VERIFICATION DE C1 (etalon B, --no-plan) ==="
& .\bin\Release\combosolver.exe $ref `
    --scriptdir ..\deps\scripts_2026-04-13\script `
    --start $ref --no-plan `
    --solve-ms $Ms --seed $Seed `
    --no-chain Zalen --no-chain "Crystal Wing" `
    --outdir s9_c1 *> s9_c1.log
Write-Output "    EXIT=$LASTEXITCODE"

Write-Output "=== 2. A/B DU GRAPHE AMORCE ==="
& .\tools\s9_recettes_amorce.ps1

Write-Output "SUITE 2 TERMINEE"
