# CURRICULUM SUR LA TAILLE DE LA CIBLE — « le but seul converge-t-il en
# quelques minutes ? », posee par l'operateur en session 15.
#
# CE QUE LA QUESTION EXIGE ET QUE LES A/B NE DONNENT PAS. Onze runs de 300 s
# (4 mecanismes x 3 tirages) rendent TOUS 2/4 et ZERO solution ecrite, et
# 9.21 (f) mesure 2/4 encore a 1 200 s. Le plafond n'est ni une affaire de
# budget ni de dispersion : c'est un MUR. Lire le meilleur etat dit lequel —
# Bagooska + UN Liger Dancer, jamais deux, alors que la cible en demande TROIS.
#
# Avant de depenser quoi que ce soit a pousser ce mur, il faut savoir s'il est
# dans la RECHERCHE ou dans le PROBLEME. Une seule facon de le savoir sans
# theoriser : faire varier la cible et regarder ou la convergence s'arrete.
#
#   n = 1 : 1 Liger + Bagooska  -> c'est deja ce que la recherche atteint. Si
#           elle n'ecrit pas de SOLUTION ici, le blocage n'est pas le board
#           mais une contrainte (--resolve, --summon-min) ou le detail du but.
#   n = 2 : 2 Liger + Bagooska  -> le premier pas jamais franchi.
#   n = 3 : la cible historique -> le temoin, connu perdant.
#
# `--summon-min` suit n : il exige que n invocations soient des Liger Dancer.
# Le reste du montage est INCHANGE (meme gabarit epingle, meme deck, meme main,
# memes --resolve, memes indices) — un seul facteur bouge.
#
# LIRE : « N replay(s) ecrits » (une SOLUTION, pas une approche) et la ligne
# « au mieux k des N cartes cibles ». Un run qui ecrit une solution repond OUI
# a la question, et son temps est la reponse quantitative.
param([int[]]$Ligers = @(1, 2, 3),
      [int]$Ms = 120000,
      [string]$Seed = '888',
      [string]$Prefixe = 's15cu')

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
$repo = "D:\ProjectIgnis\repositories"
$gabarit = "D:\ProjectIgnis\replay2video\combosolver\gabarits\etalon_a_lunalight.yrp"

foreach ($n in $Ligers) {
    $out = "${Prefixe}_L$n"
    $cibles = @()
    for ($i = 0; $i -lt $n; $i++) { $cibles += @('--target', '54701958') }
    $cibles += @('--target', '90590304@DEF')
    Write-Output "--- $out : $n Liger + Bagooska"
    & .\bin\Release\combosolver.exe $gabarit `
        --scriptdir "D:\ProjectIgnis\replay2video\deps\compat_2026-08" `
        --scriptdir ..\deps\scripts_2026-04-13\script `
        --scriptdir "$repo\delta-bagooska\script" `
        --scriptdir "$repo\delta-puppet\script" `
        --deck "D:\ProjectIgnis\deck\Lunalight.ydk" `
        --hand "57103969|57103969|57103969" `
        --no-ref @cibles `
        --max-decisions 700 `
        --resolve 4731783 --resolve 2344618 --resolve 47705572 `
        --summon-min "54701958:$n" `
        --hint 35618217 --hint 24550676 --hint 100460013 --hint 24094653 `
        --hint 48444114 --hint 83190280 --hint 50277355 --hint 14152693 `
        --options-online 30 `
        --solve-ms $Ms --seed $Seed --finisher levin --archive-k 24 `
        --outdir $out *> "$out.log"
    Write-Output "    EXIT=$LASTEXITCODE"
}
Write-Output "TERMINE"
