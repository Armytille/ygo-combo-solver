# A/B DE LA SESSION 15 — etalon A but seul, bras nommes, gabarit EPINGLE.
#
# Meme montage que tools/s14_masse_ab.ps1 (dont il derive) : meme gabarit, meme
# deck, meme main, memes cibles, memes indices. Ce qui change est la liste des
# bras, et le fait que le TEMOIN est relance ici — la session 15 a rebati le
# moteur (bandit Q^, elagage de nouveaute dans les tirages, quota d'archive),
# donc aucun temoin d'une session anterieure n'est apparie.
#
# CE QU'IL FAUT LIRE, DANS L'ORDRE (tools/s14_lecture.ps1) :
#   1. la SONDE du bandit sous --qhat — n^ et Q^ par ouverture. Si la bonne
#      cible n'y ressort pas, aucun juge de recherche n'a de sens (la courbe
#      d'accord du corpus, elle, est AVEUGLE a Q^ : elle mesure la reproduction
#      d'un corpus qui ne contient que des bonnes lignes).
#   2. l'approche ECRITE (best_approach_kof4.yrp), PAS la ligne « NRPA best k/4 »
#      qui ne couvre que la phase tirages (piege d'instrument, 9.21 (d)).
#   3. >=2 / >=3 resolutions.
# BANDE DE BRUIT : a graine fixee, >=3 varie d'un facteur ~2 et la conversion
# bascule 1/4 <-> 2/4. Une lecture mono-graine ne vaut RIEN.
param([int]$Ms = 300000,
      [int]$Period = 60,
      [string[]]$Seeds = @('888'),
      [string[]]$Bras = @('temoin', 'qhat6'),
      [string]$Prefixe = 's15')

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
$repo = "D:\ProjectIgnis\repositories"
$gabarit = "D:\ProjectIgnis\replay2video\combosolver\gabarits\etalon_a_lunalight.yrp"

$defs = @{
    # --- temoins ---
    # la forme GAGNANTE de la session 14 : minage en ligne, sans garde ctx.
    'temoin'   = @('--options-online', "$Period")
    # le meme run NU (aucune option) : le temoin des bras qui n'ont rien a voir
    # avec le catalogue, et le regime ou la sonde du bandit se lit le plus net.
    'nu'       = @()

    # --- chantier 1 : la statistique de permutation Q^ ---
    'qhat6'    = @('--options-online', "$Period", '--qhat', '6')
    'qhat12'   = @('--options-online', "$Period", '--qhat', '12')
    'qhat6nu'  = @('--qhat', '6')
    'qhat6w16' = @('--options-online', "$Period", '--qhat', '6',
                   '--qhat-window', '16384')

    # --- chantier 2 : les defaillances reparees ---
    # sorties de phase retirees de l'enumeration (le prompt de bataille les
    # emettait inconditionnellement jusqu'ici : le drapeau ne fermait qu'une
    # moitie de la porte)
    'nophase'  = @('--options-online', "$Period", '--no-phase-change')
    # elagage par nouveaute DANS les tirages sous politique (le verdict y etait
    # calcule a chaque decision et jete)
    'novcut'   = @('--options-online', "$Period", '--novelty-rollout-cut')
    # quota par niveau de progres dans l'archive Go-Explore
    'spread'   = @('--options-online', "$Period", '--archive-spread')
    # zones canoniques (declarees, allumees nulle part jusqu'a la s15)
    'canon'    = @('--options-online', "$Period", '--canonical-zones')
    # les deux reparations d'enumeration ensemble
    'nophasenov' = @('--options-online', "$Period", '--no-phase-change',
                     '--novelty-rollout-cut')
    # le bandit compose a la meilleure reparation du chantier 2
    'qhat6nophase' = @('--options-online', "$Period", '--qhat', '6',
                       '--no-phase-change')
}

function Invoke-Luna {
    param([string]$Out, [string]$Seed, [int]$Budget, [string[]]$Extra)
    Write-Output "--- $Out (graine $Seed)"
    & .\bin\Release\combosolver.exe $gabarit `
        --scriptdir "D:\ProjectIgnis\replay2video\deps\compat_2026-08" `
        --scriptdir ..\deps\scripts_2026-04-13\script `
        --scriptdir "$repo\delta-bagooska\script" `
        --scriptdir "$repo\delta-puppet\script" `
        --deck "D:\ProjectIgnis\deck\Lunalight.ydk" `
        --hand "57103969|57103969|57103969" `
        --no-ref `
        --target 54701958 --target 54701958 --target 54701958 `
        --target "90590304@DEF" `
        --max-decisions 700 `
        --resolve 4731783 --resolve 2344618 --resolve 47705572 `
        --summon-min "54701958:3" `
        --hint 35618217 --hint 24550676 --hint 100460013 --hint 24094653 `
        --hint 48444114 --hint 83190280 --hint 50277355 --hint 14152693 `
        @Extra `
        --solve-ms $Budget --seed $Seed --finisher levin --archive-k 24 `
        --outdir $Out *> "$Out.log"
    Write-Output "    EXIT=$LASTEXITCODE"
}

foreach ($s in $Seeds) {
    foreach ($b in $Bras) {
        if (-not $defs.ContainsKey($b)) { Write-Output "!! bras inconnu : $b"; continue }
        Invoke-Luna "${Prefixe}_${b}_$s" $s $Ms $defs[$b]
    }
}
Write-Output "TERMINE"
