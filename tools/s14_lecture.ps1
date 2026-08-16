# LECTURE D'UNE SERIE DE RUNS (session 14) — les juges, et rien d'autre.
#
# Les logs PS 5.1 sont en UTF-16 : Select-String, jamais grep (piege maison).
# Les colonnes sont, dans l'ordre de decision : best k/4 (LA barre), >=2 et >=3
# resolutions, lignes ecrites, puis la vie des options. Le compteur de tirages
# n'est PAS imprime : il est declasse comme instrument d'A/B (9.19 (a)).
param([string]$Motif = 's14_*.log')

Set-Location "D:\ProjectIgnis\replay2video\combosolver"

$rows = foreach ($f in Get-ChildItem $Motif | Sort-Object Name) {
    $t = Get-Content $f.FullName
    # LE juge : l'APPROCHE ECRITE, c'est-a-dire le meilleur etat que le run
    # entier a su atteindre — pas la ligne « NRPA ... best k/4 », qui ne resume
    # que la phase tirages et ignore ce que le finisseur enracine trouve. C'est
    # la colonne que 9.20 (a) lit (« approche ecrite gen1 -> gen2 »).
    $best = ($t | Select-String -Pattern 'best_approach_(\d+)of(\d+)\.yrp \((\d+) decisions').Matches
    $tir  = ($t | Select-String -Pattern 'NRPA\s+:.*best (\d+)/(\d+)').Matches
    $res  = ($t | Select-String -Pattern 'resolutions atteintes par tirage : >=1 (\d+)\s+>=2 (\d+)\s+>=3 (\d+)\s+>=4 (\d+)').Matches
    $sol  = ($t | Select-String -Pattern '(\d+) replay\(s\) ecrits').Matches
    $mac  = ($t | Select-String -Pattern 'options : (\d+) prises, (\d+) decisions absorbees \(([\d.]+)/prise\), (\d+) avortees').Matches
    $onl  = ($t | Select-String -Pattern 'options en ligne : (\d+) tour').Matches
    $cat  = ($t | Select-String -Pattern 'dernier catalogue : (\d+) macro\(s\) \(moyenne ([\d.]+), max (\d+)\) sur (\d+) ligne\(s\), perte modele ([\d.]+) -> ([\d.]+) log10 ; minage ([\d.]+) ms en moyenne, ([\d.]+) ms').Matches
    # Catalogue STATIQUE (mine une fois au demarrage) : meme lecture, autre ligne.
    $sta  = ($t | Select-String -Pattern 'options : (\d+) macro\(s\) retenue\(s\).*moyenne ([\d.]+),.*perte modele ([\d.]+) -> ([\d.]+) log10').Matches
    $ctxl = ($t | Select-String -Pattern 'niveau contextuel : (\d+) case\(s\).*conditionnement : ([^)]+\))').Matches
    [pscustomobject]@{
        run      = $f.BaseName
        best     = if ($best) { "$($best[-1].Groups[1].Value)/$($best[-1].Groups[2].Value)" } else { '-' }
        dec      = if ($best) { [int]$best[-1].Groups[3].Value } else { $null }
        tirages  = if ($tir)  { "$($tir[0].Groups[1].Value)/$($tir[0].Groups[2].Value)" } else { '-' }
        'ge2'    = if ($res)  { [int]$res[0].Groups[2].Value } else { $null }
        'ge3'    = if ($res)  { [int]$res[0].Groups[3].Value } else { $null }
        lignes   = if ($sol)  { [int]$sol[-1].Groups[1].Value } else { 0 }
        prises   = if ($mac)  { [int]$mac[0].Groups[1].Value } else { $null }
        'abs/pr' = if ($mac)  { [double]$mac[0].Groups[3].Value } else { $null }
        avortees = if ($mac)  { [int]$mac[0].Groups[4].Value } else { $null }
        tours    = if ($onl)  { [int]$onl[0].Groups[1].Value } else { $null }
        macros   = if ($cat)  { [int]$cat[0].Groups[1].Value }
                   elseif ($sta) { [int]$sta[0].Groups[1].Value } else { $null }
        'lg moy' = if ($cat)  { [double]$cat[0].Groups[2].Value }
                   elseif ($sta) { [double]$sta[0].Groups[2].Value } else { $null }
        perte    = if ($cat)  { "$($cat[0].Groups[5].Value)->$($cat[0].Groups[6].Value)" }
                   elseif ($sta) { "$($sta[0].Groups[3].Value)->$($sta[0].Groups[4].Value)" } else { $null }
        'mine ms'= if ($cat)  { [double]$cat[0].Groups[8].Value } else { $null }
        'ctx'    = if ($ctxl) { [int]$ctxl[0].Groups[1].Value } else { $null }
        'cond'   = if ($ctxl) { $ctxl[0].Groups[2].Value } else { $null }
    }
}
# Deux tables : les JUGES d'abord (la seule lecture qui tranche), la vie du
# mecanisme ensuite. En une seule, Format-Table coupe les dernieres colonnes en
# silence — et ce sont justement celles qui disent si le minage a vecu.
Write-Output "`n=== juges ==="
$rows | Format-Table run, best, dec, tirages, ge2, ge3, lignes -AutoSize
Write-Output "=== vie du mecanisme (c'est CE tableau que le criblage lit) ==="
$rows | Format-Table run, prises, 'abs/pr', avortees, tours, macros, 'lg moy', perte, 'mine ms', ctx, cond -AutoSize
