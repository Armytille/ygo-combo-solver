# SUITE 2 : la verification de C1, puis l'A/B du graphe AMORCE.
#
# 1. VERIFICATION DE C1 — le test exact, et il est bon marche.
#
#    Le §9.15 ecrivait : « sans plan, la passe a ecarts bornes devient VIDE :
#    26 etats a tous les niveaux d'ecart », et en tirait un fait STRUCTUREL du
#    mode but seul. Le releve de la session 8 (s8_B_noplan.log) :
#
#      ecarts  solutions   etats   transpos.  coupures
#      0            0          1        0         0
#      1            0         26       16         0
#      2            0         26       16         0
#      3            0         26       16         0
#      ...          0         26       16         0     <-- CONSTANT
#
#    C1 explique ce chiffre entierement : `claims_size = plan.size() + 1` vaut 1
#    quand le plan est vide, donc UN jeton de partition pour seize workers, et
#    toute deviation est supprimee.
#
#    APRES CORRECTION, le nombre d'etats doit CROITRE avec le niveau d'ecart.
#    S'il reste a 26, C1 n'est pas dans le binaire. Lire aussi la colonne
#    `partition` : non nulle = le travail est CEDE (partage), ce que la session 8
#    ne pouvait pas distinguer d'une SUPPRESSION.
#
# 2. A/B DU GRAPHE AMORCE (tools/s9_recettes_amorce.ps1).
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
