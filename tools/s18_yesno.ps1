# LA SONDE OUI/NON (session 18ter) — LE PROMPT QUI DEBLOQUE LE CIMETIERE.
#
# CE QUE LE LUA A REVELE. Sur l'etalon A, la decision qui ouvre l'acces aux
# materiaux du CIMETIERE est un `Duel.SelectYesNo` : l'effet e2 de
# `Lunalight Masquerade` (2344618) se declenche sur une Invocation-Fusion
# Lunalight, recupere Polymerization, PUIS propose de defausser une carte —
# et c'est cette defausse FACULTATIVE qui enregistre
# EFFECT_EXTRA_FUSION_MATERIAL jusqu'a la End Phase. Sans elle, les Fusions
# suivantes ne peuvent pas bannir depuis le cimetiere, ou vit le materiau nomme.
#
# LE DEFAUT QU'ELLE MESURE. L'enumerateur emettait `EdgeOf(message, {1})` pour
# tous les oui de la partie : UN SEUL poids de politique pour huit decisions du
# plan resolu, dont le pivot du combo. `--yn-identity` donne au prompt son
# identite (carte, effet) et le rend visible aux sondes.
#
# LES JUGES, DANS L'ORDRE :
#   1. la ligne « OUI/NON » de Masquerade : offres, et taux de OUI. C'est le
#      chiffre neuf. A « JAMAIS OUI », le combo est referme a la premiere porte.
#   2. la colonne OUI/NON de « par prompt » : le prompt existe-t-il seulement ?
#   3. les invocations de Liger (54701958), inchangees si rien ne bouge.
#
# UN SEUL FACTEUR entre les deux bras.
param([int]$Ms = 90000,
      [string[]]$Seeds = @('888'),
      [string[]]$Bras = @('temoin', 'yn'),
      [string]$Prefixe = 's18y')

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
$repo = "D:\ProjectIgnis\repositories"
$gabarit = "D:\ProjectIgnis\replay2video\combosolver\gabarits\etalon_a_lunalight.yrp"

$defs = @{
    'temoin' = @()
    'yn'     = @('--yn-identity')
    # LES DEUX INSTRUMENTS ALLUMES. `--card-on-select` n'est pas ici un
    # mecanisme mais une CONDITION D'INSTRUMENT : sans lui `Choice::card` est nul
    # sur les prompts de SELECTION, donc « CHOISIE quand offerte » vaut ZERO par
    # construction — et se lit « JAMAIS RETENUE », ce qui est faux. Le relevé le
    # dit desormais de lui-meme, mais il faut quand meme le mesurer allume.
    'full'   = @('--yn-identity', '--card-on-select')
}

foreach ($s in $Seeds) {
  foreach ($b in $Bras) {
    $out = "${Prefixe}_${b}_$s"
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
        --watch 2344618 --watch 47705572 --watch 24550676 --watch 54701958 `
        --probe-repeat `
        --options-online 60 `
        --elide-forced --hindsight 0.5 --adapt-to-peak `
        @($defs[$b]) `
        --solve-ms $Ms --seed $s --finisher levin --archive-k 24 `
        --outdir $out *> "$out.log"
    Write-Output "    EXIT=$LASTEXITCODE"
  }
}
