# SESSION 10, second temps : ce qui suit la compilation du nœud d'exigence.
#
#  0. SANTE APRES. La porte. Le diff avec s10_sante.log (santé AVANT, meme
#     session, binaire precedent) ne doit contenir QUE des durees : le run de
#     sante n'active pas --recipes, donc rien de ce qui a ete ecrit ne doit s'y
#     voir. Si quelque chose bouge, c'est une regression, pas un resultat.
#  1. AMORCE SUR L'ETALON A. La mesure qui decide du chantier 16, sur le cas ou
#     l'amorce a quelque chose a dire. Quatre bras deterministes.
#  2. OPTIONS. Le chiffrage du chantier 17 sur le corpus, avant d'ecrire quoi
#     que ce soit — 30 secondes contre une session.
param([switch]$SkipSante)

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
$ref = "D:\ProjectIgnis\replay\synchron handrip 2.yrpX"

if (-not $SkipSante) {
    Write-Output "=== 0/2 SANTE APRES -> s10_sante_apres.log"
    & .\bin\Release\combosolver.exe $ref `
        --scriptdir ..\deps\scripts_2026-04-13\script --solve --solve-ms 60000 `
        --outdir s10_sante_apres --no-chain Zalen --no-chain "Crystal Wing" `
        *> "s10_sante_apres.log"
    Write-Output "    EXIT=$LASTEXITCODE"
}

Write-Output "=== 1/2 AMORCE, ETALON A"
& .\tools\s10_amorce_etalon_a.ps1

Write-Output "=== 2/2 CHIFFRAGE DES OPTIONS"
& .\tools\s10_options.ps1

Write-Output "=== SUITE 2 TERMINEE"
