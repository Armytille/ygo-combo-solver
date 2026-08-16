# LA PROMOTION EN DEFAUT DE `--elide-forced`, `--hindsight` ET `--adapt-to-peak`
# (session 19, dette 1).
#
# CE QUI MANQUE A CES TROIS MECANISMES, ET RIEN D'AUTRE. Ils sont mesures BONS
# sur l'etalon A — +61 % de debit et x2,1 de boards (9.24 (k)), x20,6 sur
# l'arite 3 (9.24 (c)), x2,6 sur deux paires (9.26 (f)) — et ils sont ETEINTS.
# Le solveur nu n'en beneficie d'aucun. La regle 2 du README dit qu'un mecanisme
# passe sur les DEUX etalons devient le defaut ; il leur manque l'etalon B.
#
# ET L'ETALON B NE SE MESURE PAS A UN RUN PAR BRAS. Son juge rend
# `0, 0, 0, 89, 2 239` sur cinq executions de la MEME commande a la MEME graine
# (9.25 (b)) : ce n'est pas du bruit, c'est un EVENEMENT RARE. La lecture porte
# donc sur la PROPORTION d'executions qui aboutissent, sur N runs, a graines
# DIFFERENTES — une graine fixe ne ferait qu'echantillonner une fois de plus.
#
# LE JUGE : la proportion de runs qui ecrivent au moins une solution, et la
# proportion qui atteint les deux resolutions exigees. Deux bras, un seul
# facteur : la pile.
param([int]$N = 10,
      [int]$Ms = 60000,
      [string]$Prefixe = 's19p')

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
$bopt = "D:\ProjectIgnis\replay\synchron handrip optimized.yrpX"

$defs = @{
    'nu'   = @()
    'pile' = @('--elide-forced', '--hindsight', '0.5', '--adapt-to-peak')
}

foreach ($b in @('nu', 'pile')) {
    for ($i = 0; $i -lt $N; ++$i) {
        $seed = 1000 + $i
        $out = "${Prefixe}_${b}_$seed"
        & .\bin\Release\combosolver.exe $bopt `
            --scriptdir ..\deps\scripts_2026-04-13\script `
            --start $bopt --no-plan --solve-ms $Ms --seed $seed `
            --guard "5:Crystal Wing|Zalen@terrain+Junk Signal@main" `
            --no-activate "Duel Evolution - Assault Zone" `
            --no-chain Zalen --no-chain "Crystal Wing" `
            --resolve "PSY-Framelord Omega@terrain:2" `
            --resolve "Trishula, Dragon of the Ice Barrier@terrain" `
            @($defs[$b]) `
            --finisher levin --archive-k 24 --outdir $out *> "$out.log"
        Write-Output "$out EXIT=$LASTEXITCODE"
    }
}

# --- LECTURE EN PROPORTION ---------------------------------------------------
#
# LE JUGE EST `>=1` ET `>=2`, PAS « une solution ecrite ». Mesure : AUCUN des
# vingt runs n'ecrit de solution a 60 s — ce juge-la est constant, donc muet. Le
# compteur de resolutions, lui, est bimodal exactement comme 9.25 (b) l'annonce
# (cinq zeros et cinq valeurs a quatre chiffres dans le bras nu), et c'est la
# PROPORTION de runs non nuls qui porte l'information.
foreach ($b in @('nu', 'pile')) {
    $c1 = 0; $c2 = 0; $tot = 0
    for ($i = 0; $i -lt $N; ++$i) {
        $seed = 1000 + $i
        $f = "${Prefixe}_${b}_$seed.log"
        if (-not (Test-Path $f)) { continue }
        ++$tot
        $m = Select-String -Path $f -Pattern 'resolutions atteintes par tirage : >=1 (\d+)\s+>=2 (\d+)'
        if (-not $m) { continue }
        if ([int]$m.Matches[0].Groups[1].Value -gt 0) { ++$c1 }
        if ([int]$m.Matches[0].Groups[2].Value -gt 0) { ++$c2 }
    }
    Write-Output "BRAS $b : >=1 dans $c1 / $tot run(s)  |  >=2 dans $c2 / $tot run(s)"
}
