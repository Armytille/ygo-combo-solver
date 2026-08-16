# LECTURE D'UNE SERIE DE RUNS (session 16) — etalon B optimise.
#
# Derive de tools/s15_lecture.ps1. Les colonnes changent parce que le JUGE
# change : sur l'etalon B optimise, la conversion vaut zero partout et ne
# discrimine rien. Ce qui discrimine est la JONCTION board+rips, et elle se lit
# sur trois colonnes :
#   * `>=3`  : tirages ayant accompli les TROIS resolutions exigees. Zero chez
#              le temoin (9.22 (i)) — c'est le verrou.
#   * `crete`: jusqu'ou montent les lignes AUX resolutions completes.
#   * `best` : l'approche ECRITE, jamais la ligne « NRPA best k/8 » (9.21 (d)).
# Et une colonne de VIE DU MECANISME, sans laquelle aucun juge n'a de sens :
#   * `lm h` : le `h` de landmarks moyen. Colle au total appris = la recherche
#              n'accomplit rien ; colle a zero = les landmarks sont trop faciles
#              et ne guident pas (piege 52).
#
# Les logs PS 5.1 sont en UTF-16 : Select-String, jamais grep (piege maison).
# BANDE DE BRUIT : a graine fixee les compteurs de tirages varient d'un facteur
# ~2 (9.21 (i)). Une lecture mono-graine ne vaut RIEN pour un ecart de ce
# calibre ; elle vaut pour un zero qui devient non nul.
param([string]$Motif = 's16ab_*.log')

Set-Location "D:\ProjectIgnis\replay2video\combosolver"

$rows = foreach ($f in Get-ChildItem $Motif | Sort-Object Name) {
    $t = Get-Content $f.FullName
    $best = ($t | Select-String -Pattern 'best_approach_(\d+)of(\d+)\.yrp \((\d+) decisions').Matches
    $tir  = ($t | Select-String -Pattern 'NRPA\s+:.*best (\d+)/(\d+)').Matches
    $res  = ($t | Select-String -Pattern 'resolutions atteintes par tirage : >=1 (\d+)\s+>=2 (\d+)\s+>=3 (\d+)\s+>=4 (\d+)').Matches
    $cre  = ($t | Select-String -Pattern 'meilleure crete AUX resolutions completes : (\d+)/(\d+)').Matches
    $sol  = ($t | Select-String -Pattern '(\d+) replay\(s\) ecrits').Matches
    $nrpa = ($t | Select-String -Pattern 'NRPA\s+:\s+(\d+) tirages\s+(\d+) etats').Matches
    $lmg  = ($t | Select-String -Pattern '(\d+) landmark\(s\) sur (\d+) fait\(s\) distinct\(s\), dont (\d+) BOUCLE').Matches
    $lmh  = @($t | Select-String -Pattern 'landmarks : h moyen ([\d.]+) sur' |
              ForEach-Object { [double]$_.Matches[0].Groups[1].Value })
    $tour = ($t | Select-String -Pattern 'NRPA\s+coupures : contrainte \d+, garde \d+, tour (\d+)\s+\((\d+)%').Matches
    $exp = @($t | Select-String -Pattern '^\s+archive \d+.*?(\d+) exp\.' |
             ForEach-Object { [int]$_.Matches[0].Groups[1].Value })
    [pscustomobject]@{
        run       = $f.BaseName
        best      = if ($best) { "$($best[-1].Groups[1].Value)/$($best[-1].Groups[2].Value)" } else { '-' }
        tirages   = if ($nrpa) { [int64]$nrpa[0].Groups[1].Value } else { $null }
        'ge1'     = if ($res)  { [int]$res[0].Groups[1].Value } else { $null }
        'ge2'     = if ($res)  { [int]$res[0].Groups[2].Value } else { $null }
        'ge3'     = if ($res)  { [int]$res[0].Groups[3].Value } else { $null }
        crete     = if ($cre)  { "$($cre[0].Groups[1].Value)/$($cre[0].Groups[2].Value)" } else { '-' }
        lignes    = if ($sol)  { [int]$sol[-1].Groups[1].Value } else { 0 }
        'lm n'    = if ($lmg)  { [int]$lmg[0].Groups[1].Value } else { $null }
        'lm boucl'= if ($lmg)  { [int]$lmg[0].Groups[3].Value } else { $null }
        'lm h'    = if ($lmh.Count) { [math]::Round(($lmh | Measure-Object -Average).Average, 2) } else { $null }
        'tour%'   = if ($tour) { [int]$tour[0].Groups[2].Value } else { $null }
        racines   = $exp.Count
        'exp med' = if ($exp.Count) { ($exp | Sort-Object)[[int]($exp.Count/2)] } else { $null }
    }
}

Write-Output "`n=== juges : la JONCTION board+rips (>=3 et crete) d'abord ==="
$rows | Format-Table run, 'ge1', 'ge2', 'ge3', crete, best, lignes -AutoSize
Write-Output "=== vie du mecanisme (sans elle, aucun juge n'a de sens) ==="
$rows | Format-Table run, tirages, 'lm n', 'lm boucl', 'lm h', 'tour%' -AutoSize
Write-Output "=== finisseur : ce que l'archive lui donne a explorer ==="
$rows | Format-Table run, racines, 'exp med' -AutoSize
