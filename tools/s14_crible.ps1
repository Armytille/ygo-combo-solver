# CRIBLAGE — eliminer un bras en 90 s au lieu de 300 (session 14, suite)
#
# POURQUOI. Un run coute EXACTEMENT son budget : mesure du 16/08/2026, preambule
# complet (chargement, rejeu, tests de fidelite et de stress, couverture de
# l'enumerateur, largeur effective) = ~80 ms, run reel = 287 s pour un budget de
# 300 s. Il n'y a donc RIEN a recuperer dans l'outillage : le cout d'un A/B est
# son plan d'experience, bras x graines x budget. Le seul levier est de ne pas
# payer un run de resultat pour un bras que le MECANISME condamne deja.
#
# LA PREUVE QUE CA MARCHE, sur nos propres donnees. Le pilote des leviers de
# masse a refute --options-len 16 et 24 par la PERTE DE LEVIN DU MINEUR LUI-MEME
# (28,6 au temoin contre 35,4 et 32,0) et par le catalogue tombant a 1 macro.
# Ces deux lectures sont imprimees AU PREMIER TOUR DE MINAGE, vers 60-90 s. Trois
# runs de 287 s ont ete payes pour ce que trois runs de 90 s montraient.
#
# LA REGLE, ET ELLE EST STRICTE. Le criblage juge le MECANISME, JAMAIS le
# resultat. A 90 s, >=2 / >=3 / l'approche ecrite sont du bruit — les lire ici
# transformerait un criblage sain en mauvaise mesure bon marche. Ce qu'on lit :
#   * catalogue : nombre de macros, longueur moyenne, perte modele avant->apres
#     (le critere du mineur : s'il EMPIRE, le bras est mort) ;
#   * options : prises, absorbees/prise, avortees (un mecanisme qui n'est jamais
#     pris ou qui avorte toujours ne fera pas mieux a 300 s) ;
#   * niveau contextuel : cases occupees (a ~0 il ne distingue rien ; au plafond
#     il a cesse d'apprendre) ;
#   * duree du minage (il court dans le budget du run).
# Un bras qui degrade le critere interne est ECARTE sans run de resultat. Un
# bras qui ne le degrade pas est PROMU a l'A/B complet — le criblage ne
# promeut jamais, il elimine.
param([int]$Ms = 90000,
      [string]$Seed = '888',
      [string[]]$Bras = @('temoin', 'mcps6', 'mcps12'),
      [int]$Period = 60)

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
Remove-Item "s14_cr_*" -Recurse -Force -ErrorAction SilentlyContinue

foreach ($b in $Bras) {
    # Le criblage reutilise le montage de l'A/B : meme commande, meme gabarit
    # epingle, seul le budget et le prefixe d'outdir changent. Un criblage qui
    # ne serait pas la meme commande ne cribleraiT rien.
    & "$PSScriptRoot\s14_masse_ab.ps1" -Ms $Ms -Period $Period -Seeds @($Seed) `
        -Bras @($b) | Out-Null
    if (Test-Path "s14_ms_${b}_$Seed.log") {
        Move-Item "s14_ms_${b}_$Seed.log" "s14_cr_${b}_$Seed.log" -Force
        Move-Item "s14_ms_${b}_$Seed" "s14_cr_${b}_$Seed" -Force -ErrorAction SilentlyContinue
    }
    Write-Output "--- crible $b : fait"
}

Write-Output ""
Write-Output "=== CRIBLAGE ($Ms ms, graine $Seed) — LIRE LE MECANISME, PAS LE RESULTAT ==="
& "$PSScriptRoot\s14_lecture.ps1" -Motif 's14_cr_*.log'
Write-Output "Rappel : >=2/>=3 et l'approche ecrite ne sont PAS des juges a ce budget."
