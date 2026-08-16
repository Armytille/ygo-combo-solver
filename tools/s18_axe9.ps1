# AXE 9 DE L'AUDIT (session 18) — LE JUGE DE L'ETALON B EST-IL BIMODAL, ET UN
# MODE STRICTEMENT DETERMINISTE EST-IL ATTEIGNABLE ?
#
# CE QUE L'AXE 1 A ETABLI. Sous `--hint-bias 0`, `--card-on-select` ne peut plus
# rien changer : le seul canal par lequel `Choice::card` entre dans un logit est
# `hinted * cfg.hint_bias` (search.cpp:3052 et 4323), `--assign-bias` etant a
# zero et `--qhat` eteint. Le drapeau est alors SEMANTIQUEMENT INERTE. Or les
# deux bras rendent 2 172 et 0. La difference n'est donc pas dans le drapeau :
# elle est dans le TEMPS (le compteur `hint_seen` fait un `std::find` de plus par
# choix), et le temps decide quel worker publie sa meilleure sequence en premier.
#
# CE QUE CETTE MESURE TRANCHE, en deux volets independants :
#
#   VOLET CENSUS (`m1..m3`) — la configuration PAR DEFAUT, repetee a graine
#   IDENTIQUE. Avec le temoin de la s17 (2 239, approche 7/8) et celui de l'axe 1
#   (0, approche 3/8), cela porte l'echantillon a cinq. Si les deux modes
#   coexistent, aucun A/B a un tirage par bras sur cet etalon n'a jamais rien
#   etabli — ni dans un sens ni dans l'autre.
#
#   VOLET DETERMINISME (`t1a`, `t1b`, `t1cos`) — `--threads 1`. Un seul worker :
#   plus d'echange asynchrone de meilleure sequence (NrpaShared), plus de table
#   partagee entre workers, plus de course sur le corpus vivant. Si `t1a` et
#   `t1b` sont IDENTIQUES ligne pour ligne, un mode reproductible existe et le
#   cout en debit se lit dans le meme relevé. `t1cos` verifie qu'un drapeau
#   inerte redevient alors inerte POUR DE BON.
#
# Le juge structurel (l'approche ECRITE : 3/8 a 434 decisions contre 7/8 a
# ~190-250) separe les deux modes sans ambiguite et ne depend d'aucun compteur.
param([int]$Ms = 90000,
      [string]$Seed = '888',
      [string[]]$Bras = @('m1', 'm2', 'm3', 't1a', 't1b', 't1cos'),
      [string]$Prefixe = 's18n')

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
$bopt = "D:\ProjectIgnis\replay\synchron handrip optimized.yrpX"

$defs = @{
    'm1'    = @()
    'm2'    = @()
    'm3'    = @()
    't1a'   = @('--threads', '1')
    't1b'   = @('--threads', '1')
    't1cos' = @('--threads', '1', '--card-on-select')
}

foreach ($b in $Bras) {
    $out = "${Prefixe}_${b}_$Seed"
    Write-Output "--- $out"
    & .\bin\Release\combosolver.exe $bopt `
        --scriptdir ..\deps\scripts_2026-04-13\script `
        --start $bopt --no-plan --solve-ms $Ms --seed $Seed `
        --guard "5:Crystal Wing|Zalen@terrain+Junk Signal@main" `
        --no-activate "Duel Evolution - Assault Zone" `
        --no-chain Zalen --no-chain "Crystal Wing" `
        --resolve "PSY-Framelord Omega@terrain:2" `
        --resolve "Trishula, Dragon of the Ice Barrier@terrain" `
        @($defs[$b]) `
        --finisher levin --archive-k 24 --outdir $out *> "$out.log"
    Write-Output "    EXIT=$LASTEXITCODE"
}
