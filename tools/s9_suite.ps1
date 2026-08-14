# CHAINE DE MESURES DE FIN DE SESSION 9, sequentielle.
#
# Un seul processus a la fois : la discipline du depot l'exige (jamais deux
# mesures en parallele), et le verrou de fichier sur le binaire l'applique
# mecaniquement de toute facon.
#
# 1. SANTE. La porte de tout changement de moteur. Le lot « comptage + graphe de
#    recettes » ne doit RIEN changer quand --recipes est absent : le diff contre
#    la baseline ne doit contenir que des durees.
#
# 2. COMPTAGE, validation gratuite. L'etalon A porte deja
#    `--summon-min "54701958:3"` ECRIT A LA MAIN. Le comptage derive du board
#    cible doit retrouver exactement cette contrainte — 3x Liger Dancer = trois
#    EVENEMENTS d'invocation Fusion. S'il la retrouve, le premier pas du
#    chantier 16 est valide contre une verite ecrite par un humain ; s'il ne la
#    retrouve pas, le comptage est faux et le graphe qui s'appuie dessus aussi.
#    30 s suffisent : on ne lit que le rapport, pas la recherche.
#
# 3. GRAPHE DE RECETTES, A/B deterministe sur l'etalon 0 (tools/s9_recettes.ps1).
#
# 4. ETALON A but seul, re-mesure apres C1 (tools/s9_etalon_a.ps1).
param([switch]$SkipSante)

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
$ref = "D:\ProjectIgnis\replay\synchron handrip 2.yrpX"

if (-not $SkipSante) {
    Write-Output "=== 1. SANTE ==="
    & .\bin\Release\combosolver.exe $ref `
        --scriptdir ..\deps\scripts_2026-04-13\script `
        --solve --solve-ms 60000 --outdir s9_sante_rec `
        --no-chain Zalen --no-chain "Crystal Wing" *> s9_sante_rec.log
    Write-Output "    EXIT=$LASTEXITCODE"
}

Write-Output "=== 2. COMPTAGE (validation contre la contrainte ecrite a la main) ==="
$repo = "D:\ProjectIgnis\repositories"
& .\bin\Release\combosolver.exe "D:\ProjectIgnis\replay\_LastReplay.yrpX" `
    --scriptdir "D:\ProjectIgnis\replay2video\deps\compat_2026-08" `
    --scriptdir ..\deps\scripts_2026-04-13\script `
    --scriptdir "$repo\delta-bagooska\script" `
    --scriptdir "$repo\delta-puppet\script" `
    --deck "D:\ProjectIgnis\deck\Lunalight.ydk" `
    --hand "57103969|57103969|57103969" `
    --no-ref `
    --target 54701958 --target 54701958 --target 54701958 `
    --target "90590304@DEF" `
    --max-decisions 700 --derive-summon-min `
    --solve-ms 30000 --seed 888 --outdir s9_comptage *> s9_comptage.log
Write-Output "    EXIT=$LASTEXITCODE"

Write-Output "=== 3. GRAPHE DE RECETTES (etalon 0) ==="
& .\tools\s9_recettes.ps1

Write-Output "=== 4. ETALON A but seul, apres C1 ==="
& .\tools\s9_etalon_a.ps1

Write-Output "CHAINE TERMINEE"
