# LE TEST REINE — la mission, en UNE SEULE TRAITE (session 14, chantier 1 (c))
#
# Un run complet, budget long, ZERO corpus externe, ZERO --approach herite,
# ZERO relance : `--no-ref --target ... --options-online`. C'est la forme que
# l'operateur a fixee en fin de session 13. La session 13 avait prouve la
# FAISABILITE de la boucle en plusieurs runs (gen1 nue -> macros -> gen2 armee
# -> premier 2/4) ; ce script teste son INTERNALISATION.
#
# DEUX BRAS, un par arme, meme graine et meme budget — le temoin nu est relance
# ICI et non repris de 9.20 (a) : a 240-300 s le nu rend 1/4 sur trois graines,
# mais personne n'a jamais fait tourner un nu 1200 s, et la lecture « le budget
# convertit » de 9.20 (d) interdit de le supposer.
#
# LIRE : best k/4 d'abord (la barre), puis >=2 / >=3 resolutions, le nombre de
# lignes ecrites, et la ligne « options en ligne » — combien de tours de minage
# le budget a payes et comment le catalogue a evolue.
# $Ctx : tolerance de la garde semantique, -1 = garde eteinte. Le bras arme du
# test reine doit etre la forme GAGNANTE de l'A/B a 300 s — pas la forme
# recommandee a priori. On ne fige donc pas la garde ici.
param([int]$Ms = 1200000, [int]$Period = 90, [string]$Seed = '4242',
      [int]$Ctx = -1)

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
$repo = "D:\ProjectIgnis\repositories"
$gabarit = "D:\ProjectIgnis\replay\_LastReplay.yrpX"

function Invoke-Luna {
    param([string]$Out, [string]$S, [int]$Budget, [string[]]$Extra)
    Write-Output "--- $Out (graine $S, $([Math]::Round($Budget/1000)) s)"
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
        --solve-ms $Budget --seed $S --finisher levin --archive-k 24 `
        --outdir $Out *> "$Out.log"
    Write-Output "    EXIT=$LASTEXITCODE"
}

$arme = @('--options-online', "$Period")
if ($Ctx -ge 0) { $arme += @('--options-ctx', "$Ctx") }

Invoke-Luna "s14_reine_nu_$Seed" $Seed $Ms @()
Invoke-Luna "s14_reine_on_$Seed" $Seed $Ms $arme
Write-Output "TERMINE"
