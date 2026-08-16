# TRANSPLANTATION DE LA LIGNE RESOLUE DE L'ETALON A (session 17).
#
# CE QUI CHANGE TOUT. L'operateur a fourni `2026-08-16 13-19-12.yrpX` : rejeu
# FIDELE (283/283 reponses, 0 MSG_RETRY), qui pose DEUX Lunalight Liger Dancer
# et invoque DEUX Lunalight Leo Dancer. Jusqu'ici l'etalon A n'avait AUCUN plan
# resolu — d'ou trois consequences en cascade dans le dossier : pas de corpus
# d'adaptation, pas de landmarks apprenables, et un graphe de recettes qui ne
# connaissait de Liger que l'AMORCE PAR LE TEXTE (9.24 (e) : « --backward est
# vivant mais sans matiere »).
#
# CE QUE CE SCRIPT MESURE, et c'est la question posee par l'operateur : « la
# nouvelle main est encore plus puissante, on devrait pouvoir faire le terrain
# voulu entierement ». La ligne joue l'ANCIENNE main (3 Fire Formation - Tenki).
# On demande donc au solveur de REFAIRE SON BOARD depuis la NOUVELLE main
# (1 Gold Leo + 3 Fake Trap) — c'est exactement `RunTransplant` : la reference
# n'est plus rejouable (ses reponses ne designent rien dans cet autre duel), il
# n'en reste que l'INTENTION, relevee par LiftPlan et traitee comme repertoire.
#
# LE BOARD CIBLE EST CELUI DE LA REFERENCE, pas la cible posee a la main : 2
# Liger et non 3, sans Bagooska. C'est VOULU et c'est la marche a franchir
# d'abord — le solveur n'en pose ZERO aujourd'hui. Monter a 3 Liger + Bagooska
# vient apres, et seulement si celle-ci est franchie.
#
# UN SEUL FACTEUR PAR BRAS. Le temoin est la transplantation nue.
param([int]$Ms = 90000,
      [string]$Seed = '888',
      [string]$Prefixe = 's17t',
      [string[]]$Bras = @('temoin', 'hind'))

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
$repo = "D:\ProjectIgnis\repositories"
$planA = "D:\ProjectIgnis\replay\2026-08-16 13-19-12.yrpX"

$flags = @{
    temoin = @()
    hind   = @('--hindsight', '0.5')
    # Le graphe a DESORMAIS de la matiere : les recettes OBSERVEES de Leo et de
    # Liger viennent du corpus. `--backward` etait inerte faute de cela.
    back   = @('--adapt', $planA, '--backward')
    adapt  = @('--adapt', $planA)
}

foreach ($b in $Bras) {
    if (-not $flags.ContainsKey($b)) { Write-Output "!! bras inconnu : $b"; continue }
    $out = "${Prefixe}_${b}_$Seed"
    Write-Output "--- $out ($Ms ms, graine $Seed) : $($flags[$b] -join ' ')"
    & .\bin\Release\combosolver.exe $planA `
        --scriptdir "D:\ProjectIgnis\replay2video\deps\compat_2026-08" `
        --scriptdir ..\deps\scripts_2026-04-13\script `
        --scriptdir "$repo\delta-bagooska\script" `
        --scriptdir "$repo\delta-puppet\script" `
        --deck "D:\ProjectIgnis\deck\Lunalight.ydk" `
        --hand "8379983|3027001|3027001|3027001" `
        --max-decisions 700 `
        --watch 81196066 --watch 88753594 --watch 24550676 --watch 54701958 `
        --probe-repeat `
        --options-online 60 `
        @($flags[$b]) `
        --solve-ms $Ms --seed $Seed --finisher levin --archive-k 24 `
        --outdir $out *> "$out.log"
    Write-Output "    EXIT=$LASTEXITCODE"
}
