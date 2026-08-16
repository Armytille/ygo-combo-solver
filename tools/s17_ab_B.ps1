# A/B DE LA SESSION 17 — ETALON B OPTIMISE, le seul juge ou UNE SOLUTION EXISTE.
#
# POURQUOI CE CAS EN PLUS DE L'ETALON A. L'etalon A juge l'ARITE (la colonne
# POSITION de la sonde d'offre) mais aucune solution ne s'y est jamais ecrite :
# un bras qui y gagne peut gagner sur un chemin qui ne mene nulle part. L'etalon
# B optimise est le cas ou la ligne de l'operateur satisfait la discipline
# complete (9.22 (i)) : tout echec y est imputable au solveur. C'est donc lui
# qui dit si `--hindsight` PAIE ou seulement AGITE.
#
# LES JUGES, DANS L'ORDRE (identiques a la s16, pour rester comparable) :
#   1. « resolutions atteintes par tirage » >=1 / >=2 / >=3. Le verrou de
#      l'etalon B est la JONCTION board+rips : >=3 vaut ZERO chez le temoin.
#   2. « meilleure crete AUX resolutions completes ».
#   3. l'approche ECRITE (best_approach_kof8.yrp), jamais la ligne « NRPA best ».
#   4. la vie du mecanisme : buts de substitution et adaptations. A zero but,
#      `--hindsight` est INERTE et aucun juge de recherche n'a de sens.
#
# UN SEUL FACTEUR : le temoin est la commande de 9.22 (i) a l'octet pres.
# BINAIRE : un seul, non-PGO, identique pour tous les bras — donc les DEBITS ne
# se comparent pas a ceux de la session 15.
param([int]$Ms = 300000,
      [string[]]$Seeds = @('888'),
      [string[]]$Bras = @('temoin', 'hind'),
      [string]$Prefixe = 's17B')

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
$bopt = "D:\ProjectIgnis\replay\synchron handrip optimized.yrpX"

$defs = @{
    'temoin'  = @()
    # 0.5 : l'optimum mesure sur l'etalon A (courbe 0.25 / 0.5 / 1.0, monotone
    # jusqu'a 0.5 puis plate). Reporte tel quel — le regler a nouveau ici ferait
    # de ce cas un banc de reglage au lieu d'un juge.
    'hind'    = @('--hindsight', '0.5')
    'hind10'  = @('--hindsight', '1.0')
    # Les trois autres leviers, pour memoire : refutes ou inertes sur l'etalon A,
    # mesures ici seulement si le temps le permet.
    'recw01'  = @('--recipe-w', '0.1')
    'back'    = @('--backward')
    'assign'  = @('--assign')
}

function Invoke-B {
    param([string]$Out, [string]$Seed, [int]$Budget, [string[]]$Extra)
    Write-Output "--- $Out (graine $Seed)"
    & .\bin\Release\combosolver.exe $bopt `
        --scriptdir ..\deps\scripts_2026-04-13\script `
        --start $bopt --no-plan --solve-ms $Budget --seed $Seed `
        --guard "5:Crystal Wing|Zalen@terrain+Junk Signal@main" `
        --no-activate "Duel Evolution - Assault Zone" `
        --no-chain Zalen --no-chain "Crystal Wing" `
        --resolve "PSY-Framelord Omega@terrain:2" `
        --resolve "Trishula, Dragon of the Ice Barrier@terrain" `
        @Extra `
        --finisher levin --archive-k 24 --outdir $Out *> "$Out.log"
    Write-Output "    EXIT=$LASTEXITCODE"
}

foreach ($s in $Seeds) {
    foreach ($b in $Bras) {
        if (-not $defs.ContainsKey($b)) { Write-Output "!! bras inconnu : $b"; continue }
        Invoke-B "${Prefixe}_${b}_$s" $s $Ms $defs[$b]
    }
}
Write-Output "TERMINE"
