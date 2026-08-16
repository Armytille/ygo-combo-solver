# LECTURE D'UNE SERIE DE RUNS (session 15) — les juges, et rien d'autre.
#
# Derive de tools/s14_lecture.ps1, augmente de trois colonnes que la session 15
# a rendues lisibles :
#   * la vie du BANDIT Q^ (decisions, recompense moyenne de la fenetre, memoire) ;
#   * les EXPANSIONS PAR RACINE du finisseur (total / mediane), qui sont le juge
#     de --archive-spread : 9.21 (f) mesurait EPUISE en 0 a 13 expansions, donc
#     une archive qui range de nouveau des etats PROMETTEURS se lit la et
#     nulle part ailleurs ;
#   * la part des tirages morts au CHANGEMENT DE TOUR, critere interne de
#     --no-phase-change.
#
# Les logs PS 5.1 sont en UTF-16 : Select-String, jamais grep (piege maison).
# LE JUGE reste l'APPROCHE ECRITE (best_approach_kof4.yrp), pas la ligne
# « NRPA ... best k/4 » qui ne couvre que la phase tirages (9.21 (d)).
# BANDE DE BRUIT : a graine fixee >=3 varie d'un facteur ~2 et la conversion
# bascule 1/4 <-> 2/4 — une lecture mono-graine ne vaut RIEN.
param([string]$Motif = 's15m_*.log')

Set-Location "D:\ProjectIgnis\replay2video\combosolver"

$rows = foreach ($f in Get-ChildItem $Motif | Sort-Object Name) {
    $t = Get-Content $f.FullName
    $best = ($t | Select-String -Pattern 'best_approach_(\d+)of(\d+)\.yrp \((\d+) decisions').Matches
    $tir  = ($t | Select-String -Pattern 'NRPA\s+:.*best (\d+)/(\d+)').Matches
    $res  = ($t | Select-String -Pattern 'resolutions atteintes par tirage : >=1 (\d+)\s+>=2 (\d+)\s+>=3 (\d+)\s+>=4 (\d+)').Matches
    $sol  = ($t | Select-String -Pattern '(\d+) replay\(s\) ecrits').Matches
    $mac  = ($t | Select-String -Pattern 'options : (\d+) prises, (\d+) decisions absorbees \(([\d.]+)/prise\), (\d+) avortees').Matches
    $cat  = ($t | Select-String -Pattern 'dernier catalogue : (\d+) macro\(s\) \(moyenne ([\d.]+), max (\d+)\)').Matches
    $tour = ($t | Select-String -Pattern 'NRPA\s+coupures : contrainte \d+, garde \d+, tour (\d+)\s+\((\d+)%').Matches
    $ban  = ($t | Select-String -Pattern 'bandit Q\^ \(--qhat (\d+)\) : (\d+) decisions dont (\d+) a la 1re, (\d+) tirages en fenetre, recompense moyenne ([\d.]+)').Matches
    $mem  = ($t | Select-String -Pattern 'arbre (\d+) noeud\(s\).*?(\d+) code\(s\) en fenetre, ([\d.]+) Mo').Matches
    # racines du finisseur : « archive NN x/4 rY   NNN exp. »
    $exp = @($t | Select-String -Pattern '^\s+archive \d+.*?(\d+) exp\.' |
             ForEach-Object { [int]$_.Matches[0].Groups[1].Value })
    [pscustomobject]@{
        run      = $f.BaseName
        best     = if ($best) { "$($best[-1].Groups[1].Value)/$($best[-1].Groups[2].Value)" } else { '-' }
        dec      = if ($best) { [int]$best[-1].Groups[3].Value } else { $null }
        tirages  = if ($tir)  { "$($tir[0].Groups[1].Value)/$($tir[0].Groups[2].Value)" } else { '-' }
        'ge2'    = if ($res)  { [int]$res[0].Groups[2].Value } else { $null }
        'ge3'    = if ($res)  { [int]$res[0].Groups[3].Value } else { $null }
        lignes   = if ($sol)  { [int]$sol[-1].Groups[1].Value } else { 0 }
        'tour%'  = if ($tour) { [int]$tour[0].Groups[2].Value } else { $null }
        macros   = if ($cat)  { [int]$cat[0].Groups[1].Value } else { $null }
        'abs/pr' = if ($mac)  { [double]$mac[0].Groups[3].Value } else { $null }
        avortees = if ($mac)  { [int]$mac[0].Groups[4].Value } else { $null }
        'q^dec'  = if ($ban)  { [int64]$ban[0].Groups[2].Value } else { $null }
        'q^rec'  = if ($ban)  { [double]$ban[0].Groups[5].Value } else { $null }
        'q^Mo'   = if ($mem)  { [double]$mem[0].Groups[3].Value } else { $null }
        racines  = $exp.Count
        'exp tot'= if ($exp.Count) { ($exp | Measure-Object -Sum).Sum } else { $null }
        'exp med'= if ($exp.Count) { ($exp | Sort-Object)[[int]($exp.Count/2)] } else { $null }
        'exp max'= if ($exp.Count) { ($exp | Measure-Object -Maximum).Maximum } else { $null }
    }
}

Write-Output "`n=== juges (approche ECRITE d'abord) ==="
$rows | Format-Table run, best, dec, tirages, ge2, ge3, lignes -AutoSize
Write-Output "=== vie des mecanismes ==="
$rows | Format-Table run, 'tour%', macros, 'abs/pr', avortees, 'q^dec', 'q^rec', 'q^Mo' -AutoSize
Write-Output "=== finisseur : ce que l'archive lui donne a explorer ==="
$rows | Format-Table run, racines, 'exp tot', 'exp med', 'exp max' -AutoSize
