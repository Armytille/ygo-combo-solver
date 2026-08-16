# LECTURE DES RUNS DE LA SESSION 17 — un tableau, pas un journal.
#
# LE JUGE PRINCIPAL est la colonne POSITION : le nombre de fois qu'une carte a
# ete POSEE. C'est la seule preuve directe d'invocation (verifiee : pour Sabre
# Dancer, POSITION egale exactement le compte d'invocations). Les colonnes
# SELECT_CARD sont du BRUIT — c'est l'extra deck lu en entier par un prompt de
# selection, identique dans tous les bras.
#
# LES COLONNES DE VIE (recettes / rebours / hindsight) sont obligatoires avant
# toute conclusion : un mecanisme allume mais INERTE est indiscernable de son
# temoin, et c'est la faute que la session 16 a commise deux fois (piege 52).
param([string]$Motif = 's17ab_*',
      [switch]$Detail)

Set-Location "D:\ProjectIgnis\replay2video\combosolver"

$lignes = @()
foreach ($log in (Get-ChildItem "$Motif.log" | Sort-Object Name)) {
    $t = Get-Content $log.FullName
    $bras = $log.BaseName

    function Val([string]$motif, [int]$grp = 1) {
        $m = $t | Select-String -Pattern $motif | Select-Object -First 1
        if ($m) { return $m.Matches[0].Groups[$grp].Value } else { return '' }
    }

    # POSITION par carte : le juge. La sonde imprime « par prompt : ... POSITION n »
    # juste apres la ligne de la carte.
    $pos = @{}
    $inv = @{}
    for ($i = 0; $i -lt $t.Count; $i++) {
        if ($t[$i] -match '^\s+(Lunalight \w+ Dancer) : >=1 (\d+)') {
            $nom = $Matches[1]; $inv[$nom] = [int]$Matches[2]
            $pos[$nom] = 0
            for ($j = $i + 1; $j -lt [Math]::Min($i + 5, $t.Count); $j++) {
                if ($t[$j] -match 'POSITION (\d+)') { $pos[$nom] = [int]$Matches[1]; break }
                if ($t[$j] -match '^\s+Lunalight') { break }
            }
        }
    }

    $lignes += [pscustomobject]@{
        bras      = $bras -replace '^s17ab_', ''
        tirages   = Val 'phase TIRAGES : (\d+) tirage'
        PerfPOS   = $pos['Lunalight Perfume Dancer']
        SabrPOS   = $pos['Lunalight Sabre Dancer']
        LeoPOS    = $pos['Lunalight Leo Dancer']
        LigerPOS  = $pos['Lunalight Liger Dancer']
        recDist   = Val 'recettes : distance moyenne ([\d.]+) contre'
        recD0     = Val 'recettes : distance moyenne [\d.]+ contre ([\d.]+)'
        rebours   = Val 'rebours : ([\d.]+) sous-produit'
        rebTot    = Val 'rebours : [\d.]+ sous-produit\(s\) fabrique\(s\) en moyenne sur (\d+)'
        hsButs    = Val 'hindsight : (\d+) but'
        hsAdapt   = Val 'hindsight : \d+ but\(s\) de substitution retenu\(s\), (\d+) adaptation'
        utiles    = Val 'code\(s\) utile\(s\)' 0
        approche  = Val 'meilleure approche ecrite : \S+ \((\d+) decisions'
    }
}

Write-Output ''
Write-Output '=== JUGE : POSES (POSITION) — 0 -> non nul est le seul verdict qui compte ==='
$lignes | Format-Table bras, tirages, PerfPOS, SabrPOS, LeoPOS, LigerPOS -AutoSize

Write-Output '=== VIE DES MECANISMES (un mecanisme inerte ne prouve rien) ==='
$lignes | Format-Table bras, recDist, recD0, rebours, rebTot, hsButs, hsAdapt -AutoSize

if ($Detail) {
    Write-Output '=== instantanes du graphe ==='
    foreach ($log in (Get-ChildItem "$Motif.log" | Sort-Object Name)) {
        Write-Output "--- $($log.BaseName)"
        Select-String -Path $log.FullName -Pattern 'recettes : instantane|session 17 :' |
            ForEach-Object { '    ' + $_.Line.Trim() }
    }
}
