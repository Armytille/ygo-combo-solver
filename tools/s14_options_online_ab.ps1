# MINAGE EN LIGNE — L'A/B DE LA MISSION (session 14, chantier 1)
#
# UN SEUL RUN, une seule traite : zero corpus externe, zero --approach herite,
# zero relance. C'est exactement la commande de generation du bootstrap de la
# session 13 (tools/s13_bootstrap_gen2.ps1) DEBARRASSEE de ses --adapt : le
# temoin est donc le run NU de la gen1 (9.20 (a) : 1/4 sur les trois graines),
# et les bras en face doivent monter SEULS.
#
# TROIS BRAS, intercales par graine (l'ordre des bras change le cache et la
# thermique, pas la conclusion — mais l'intercalage est la discipline maison) :
#   nu     : rien (le temoin ; c'est la gen1 de 9.20 (a))
#   on     : --options-online 60 (minage en ligne, garde semantique ETEINTE)
#   onctx  : --options-online 60 --options-ctx 1 (la forme recommandee par
#            9.20 (g), dont la session 13 disait « son vrai test est DANS la
#            boucle »)
#
# LIRE, dans cet ordre : best k/4 (la barre : >=2/4 la ou le nu fait 1/4),
# >=2 et >=3 resolutions, puis le triptyque des options et la ligne « options
# en ligne » (tours de minage, corpus vivant, duree du minage — au-dessus de la
# seconde le mecanisme mange son propre budget).
# JAMAIS le compteur de tirages (declasse, 9.19 (a)).
param([int]$Ms = 300000, [int]$Period = 60, [string[]]$Seeds = @('888','1234','4242'))

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
$repo = "D:\ProjectIgnis\repositories"
# GABARIT EPINGLE (session 14). Il pointait sur le _LastReplay.yrpX de
# l'installation EDOPro — un fichier VIVANT, reecrit des que quelqu'un joue un
# duel. C'est arrive EN PLEIN PILOTE le 15/08/2026 a 23:42 : deux runs sont
# morts avec « aucun yrp1 embarque ». Bruyamment, pas en silence — et l'en-tete
# de gabarit imprime par le rapport a permis de VERIFIER que les runs anterieurs
# portaient tous le meme duel, au lieu de le supposer. Meme famille que la .cdb
# vivante non epinglee (9.20). Le gabarit vit desormais DANS le depot ; sous
# --no-ref seuls ses parametres de duel comptent (drapeaux, LP, taille de main,
# deck adverse), sa ligne est ecartee — c'est pourquoi n'importe quel replay du
# meme duel fait l'affaire, y compris une approche produite par le solveur.
$gabarit = "D:\ProjectIgnis\replay2video\combosolver\gabarits\etalon_a_lunalight.yrp"

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
    Invoke-Luna "s14_on_nu_$s"    $s $Ms @()
    Invoke-Luna "s14_on_on_$s"    $s $Ms @('--options-online', "$Period")
    Invoke-Luna "s14_on_onctx_$s" $s $Ms @('--options-online', "$Period",
                                           '--options-ctx', '1')
}
Write-Output "TERMINE"
