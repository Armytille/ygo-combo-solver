# LE CONTROLE DU MODE DETERMINISTE (audit 18, correctif 1).
#
# CE QU'IL DOIT MONTRER. Deux executions de la MEME commande, a `--threads 1` et
# a budget en COMPTE (tirages + noeuds), doivent rendre des relevés identiques
# hors lignes de duree. Le repere d'avant le correctif est mesure et il est
# NEGATIF : a `--threads 1` et budget en MILLISECONDES, `t1a` et `t1b` faisaient
# 41 232 et 42 179 tirages, soit 38 lignes de diff sur 392.
#
# POURQUOI `--solve-ms` reste grand. Il ne doit JAMAIS mordre : s'il mord, le
# temps redevient la borne et le determinisme disparait. Il n'est la que comme
# garde-fou contre une boucle.
param([int]$Rollouts = 20000,
      [int]$Nodes = 500000,
      [string]$Seed = '888',
      [string]$Prefixe = 's18d')

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
$bopt = "D:\ProjectIgnis\replay\synchron handrip optimized.yrpX"

foreach ($r in @('a', 'b')) {
    $out = "${Prefixe}_$r"
    Write-Output "--- $out"
    & .\bin\Release\combosolver.exe $bopt `
        --scriptdir ..\deps\scripts_2026-04-13\script `
        --start $bopt --no-plan --solve-ms 900000 --seed $Seed `
        --threads 1 --max-rollouts $Rollouts --max-nodes $Nodes `
        --guard "5:Crystal Wing|Zalen@terrain+Junk Signal@main" `
        --no-activate "Duel Evolution - Assault Zone" `
        --no-chain Zalen --no-chain "Crystal Wing" `
        --resolve "PSY-Framelord Omega@terrain:2" `
        --resolve "Trishula, Dragon of the Ice Barrier@terrain" `
        --finisher levin --archive-k 24 --outdir $out *> "$out.log"
    Write-Output "    EXIT=$LASTEXITCODE"
}

# Le juge : tout ecart QUI N'EST PAS UNE DUREE. Les lignes de temps sont
# attendues differentes — c'est le TRAVAIL qui doit etre identique.
$a = Get-Content "${Prefixe}_a.log"
$b = Get-Content "${Prefixe}_b.log"
$d = Compare-Object $a $b
Write-Output "ECARTS BRUTS : $($d.Count) sur $($a.Count) lignes"
$dur = '\d+([.,]\d+)?\s*(ms|s)\b|duree|EPUISE|budget|\(\d+ ms\)'
$sig = $d | Where-Object { $_.InputObject -notmatch $dur }
Write-Output "ECARTS HORS DUREE : $($sig.Count)"
$sig | ForEach-Object { "$($_.SideIndicator) $($_.InputObject)" }
