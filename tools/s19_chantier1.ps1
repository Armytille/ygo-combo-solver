# CHANTIER 1 (session 19) : LE TYPE DE NŒUD MANQUANT, ET LE RE-JUGEMENT DE
# `--backward`.
#
# LA QUESTION, ET ELLE EST STRUCTURELLE. `--backward` est refute depuis 9.24 (e)
# avec un diagnostic precis : « mecanisme correct, MATIERE absente — 2
# sous-produits, 0,02 fabrique ». Le graphe rangeait `Lunalight Leo Dancer`
# comme un PRODUIT A FABRIQUER, alors que son materiau nomme est absent du deck :
# la decomposition essayait de construire une carte non constructible.
#
# `--op-recipes` pose le nœud qui manquait — « ce CODE peut etre ACQUIS » — et
# la question devient falsifiable en une mesure :
#
#     LA DECOMPOSITION DE LIGER CONTIENT-ELLE ENFIN L'OPERATEUR DE KALEIDO CHICK ?
#
# Le juge est SANS GRAINE : les compteurs `snap_backward` (profondeur de
# decomposition atteinte) et `snap_useful` (codes que le graphe designe comme
# utiles) sont imprimes au bilan, et les aretes d'acquisition sont imprimees une
# par une a l'amorce. Le mode deterministe n'est la que pour que les deux bras
# fassent le meme travail.
param([int]$Rollouts = 4000,
      [string]$Prefixe = 's19c1',
      [string]$Seed = '888')

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
$repo = "D:\ProjectIgnis\repositories"
$gabarit = "D:\ProjectIgnis\replay2video\combosolver\gabarits\etalon_a_lunalight.yrp"

$defs = @{
    # Temoin : le graphe amorce par le TEXTE, tel qu'il est depuis la s16.
    'temoin' = @('--recipes', '0', '--backward')
    # Le seul facteur qui change : l'amorce par les OPERATEURS DECLARES.
    'op'     = @('--recipes', '0', '--backward', '--op-recipes')
}

foreach ($b in @('temoin', 'op')) {
    $out = "${Prefixe}_$b"
    Write-Output "--- $out"
    & .\bin\Release\combosolver.exe $gabarit `
        --scriptdir "D:\ProjectIgnis\replay2video\deps\compat_2026-08" `
        --scriptdir ..\deps\scripts_2026-04-13\script `
        --scriptdir "$repo\delta-bagooska\script" `
        --scriptdir "$repo\delta-puppet\script" `
        --deck "D:\ProjectIgnis\deck\Lunalight.ydk" `
        --hand "8379983|3027001|3027001|3027001" `
        --no-ref `
        --target 54701958 --target 54701958 --target 54701958 `
        --target "90590304@DEF" `
        --max-decisions 700 `
        --watch 81196066 --watch 88753594 --watch 24550676 --watch 54701958 `
        @($defs[$b]) `
        --threads 1 --max-rollouts $Rollouts --max-nodes 500000 `
        --solve-ms 900000 --seed $Seed `
        --finisher levin --archive-k 24 `
        --outdir $out *> "$out.log"
    Write-Output "    EXIT=$LASTEXITCODE"
}
