# A/B DES QUATRE LEVIERS (session 17) — ETALON A NU.
#
# LE JUGE, et il est BINAIRE ET SANS GRAINE : la ligne `POSITION` de la sonde
# d'offre. `POSITION` est la seule preuve DIRECTE qu'une carte a ete posee — la
# sonde le verifie sur Sabre Dancer, dont POSITION (320) egale exactement le
# nombre d'invocations (320). Au temoin, Leo Dancer et Liger Dancer sont a
# ZERO. La question est donc : un bras les fait-il passer a NON NUL ?
#
# Un juge binaire n'a pas besoin de graines multiples : « 0 -> non nul » n'est
# pas un ecart de mesure. Les graines redeviennent obligatoires si un bras
# convertit, pour departager combien.
#
# ORDRE DE PRECEDENCE des chantiers, fixe par la sonde et non par gout : les
# ~123 000 offres SELECT_CARD sont le MEME pool pour les quatre Fusions (l'extra
# deck lu en entier) ; IDLECMD et POSITION sont a zero pour Leo et Liger. La
# panne est donc dans l'ETAT, ce qui met --recipe-w et --backward devant.
# --assign et --hindsight sont mesures quand meme : un diagnostic n'est pas une
# preuve, et un bras qui convertit contre son pronostic est une information.
#
# UN SEUL FACTEUR PAR BRAS. `--probe-repeat` implique deja `--recipes 0`, donc
# le graphe de recettes est alimente et mesure DANS TOUS LES BRAS, temoin
# compris : seuls les drapeaux de la session 17 changent.
param([int]$Ms = 90000,
      [string]$Seed = '888',
      [string]$Prefixe = 's17ab',
      [string[]]$Bras = @('temoin', 'recw', 'back', 'assign', 'hind'))

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
$repo = "D:\ProjectIgnis\repositories"
$gabarit = "D:\ProjectIgnis\replay2video\combosolver\gabarits\etalon_a_lunalight.yrp"
# Le premier plan RESOLU de l'etalon A (operateur, session 17). Voir le bloc
# `rec` / `adapt` plus bas pour ce qu'il est et ce qu'il n'est pas.
$planA = "D:\ProjectIgnis\replay\2026-08-16 13-19-12.yrpX"

# Drapeaux propres a chaque bras. Le temoin en a ZERO.
$flags = @{
    temoin = @()
    recw   = @('--recipe-w', '1.0')
    back   = @('--backward')
    assign = @('--assign')
    hind   = @('--hindsight', '0.5')
    # COURBES DE CADRAN (le poids n'est pas un facteur nouveau : c'est le meme
    # mecanisme regle autrement, et un mecanisme mal regle se lit comme un
    # mecanisme faux). Mesure a 90 s : --recipe-w 1.0 fait CHUTER les poses de
    # Perfume Dancer de 74 788 a 8 902 alors que la distance decroit vraiment
    # (10.70 contre 13.06) — le terme domine le board au lieu de le departager.
    recw01 = @('--recipe-w', '0.1')
    recw03 = @('--recipe-w', '0.3')
    hind025 = @('--hindsight', '0.25')
    hind10  = @('--hindsight', '1.0')
    # COMBINAISON, declaree comme telle : deux facteurs a la fois, donc elle ne
    # s'interprete QUE contre les deux bras simples, jamais contre le temoin.
    hind_recw = @('--hindsight', '0.5', '--recipe-w', '0.1')
    # --- LE PREMIER PLAN RESOLU DE L'ETALON A (fourni par l'operateur) --------
    # `2026-08-16 13-19-12.yrpX` : rejeu FIDELE (283/283, 0 MSG_RETRY), pose DEUX
    # Liger Dancer et invoque DEUX Leo Dancer. C'est la matiere qui manquait a
    # tout le dossier : jusqu'ici le graphe de recettes n'avait de Liger et de Leo
    # que l'AMORCE PAR LE TEXTE, et --backward etait inerte faute de pouvoir
    # decomposer (9.24 (e)). RESERVE : la ligne joue l'ANCIENNE main (3 Tenki) et
    # ne fait que 2 Liger sur les 3 exiges, sans Bagooska — ce n'est donc pas une
    # solution de la cible, c'est un CORPUS.
    #
    # DEUX BRAS POUR UN SEUL FACTEUR CHACUN, et c'est `--adapt-passes 0` qui le
    # permet (il coupe l'adaptation SANS toucher au releve) :
    #   `rec`   : le graphe recoit les recettes OBSERVEES, la politique n'est pas
    #             touchee. Isole ce que CONNAITRE la recette apporte.
    #   `adapt` : releve + adaptation de politique, le mode normal.
    rec       = @('--adapt', $planA, '--adapt-passes', '0')
    adapt     = @('--adapt', $planA)
    # Declaree comme combinaison : ne s'interprete que contre `adapt` et `hind`.
    adapt_hind = @('--adapt', $planA, '--hindsight', '0.5')
}

foreach ($b in $Bras) {
    if (-not $flags.ContainsKey($b)) { Write-Output "!! bras inconnu : $b"; continue }
    $out = "${Prefixe}_${b}_$Seed"
    Write-Output "--- $out ($Ms ms, graine $Seed) : $($flags[$b] -join ' ')"
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
        --probe-repeat `
        --options-online 60 `
        @($flags[$b]) `
        --solve-ms $Ms --seed $Seed --finisher levin --archive-k 24 `
        --outdir $out *> "$out.log"
    Write-Output "    EXIT=$LASTEXITCODE"
}
