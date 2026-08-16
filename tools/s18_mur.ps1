# L'ATTAQUE DU MUR — ETALON A NU (session 18).
#
# LE MUR, tel que trois sondes successives l'ont localise :
#   - `Lunalight Leo Dancer` exige un materiau NOMME absent du deck : il n'est
#     JAMAIS invocable par la voie normale. Il doit atteindre le CIMETIERE, puis
#     etre banni comme materiau par `Wolf` ou `Masquerade` (9.24 (l)).
#   - la PORTE est empruntee : Masquerade s'active dans 21,4 % des tirages.
#   - le MATERIAU n'arrive pas : Leo au cimetiere dans 0,035 % des tirages ;
#     `--assign-bias 3` porte ce chiffre a 0,33 % (x27, chaine causale verifiee).
#   - et Liger reste a ZERO : ses offres sont TOUTES sur des prompts de
#     selection, aucune sur IDLECMD ni POSITION — il n'est jamais PAYABLE.
#
# CE QUE CE BANC AJOUTE, et c'est le seul facteur neuf : `--adapt-to-peak`. Le
# score d'un tirage est un MAX sur les prefixes, mais le gradient renforcait
# TOUS les pas — y compris ceux d'apres le pic, c'est-a-dire ceux qui ont
# DEFAIT le board. Le mecanisme qui construit Leo puis le consomme mal etait
# donc appris exactement aussi fort que celui qui le construit.
#
# LE RUN EST NU : aucun `--hint`, aucun `--resolve` (indice deguise), aucun
# `--summon-min`, aucune reference. `--watch` ne fait que COMPTER.
#
# LE JUGE EST BINAIRE ET SANS GRAINE : la colonne « invoquee » de Liger Dancer
# (54701958) passe-t-elle de ZERO a non nul ? Tout le reste est secondaire.
param([int]$Ms = 90000,
      [string[]]$Seeds = @('888'),
      [string[]]$Bras = @('temoin', 'pile', 'pilepeak'),
      [string]$Prefixe = 's18w')

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
$repo = "D:\ProjectIgnis\repositories"
$gabarit = "D:\ProjectIgnis\replay2video\combosolver\gabarits\etalon_a_lunalight.yrp"

$defs = @{
    # Nu strict : le repere de reference du dossier.
    'temoin'   = @()
    # LA PILE, c'est-a-dire l'etat de l'art mesure de la s17 : le seul couple
    # coherent sur deux decks (--elide-forced --hindsight) plus le seul
    # mecanisme qui deplace le goulot mesure (--assign-bias, dont la
    # « refutation » sur l'etalon B est retiree par 9.25 (b)).
    'pile'     = @('--elide-forced', '--hindsight', '0.5', '--assign-bias', '3')
    # LA PILE + LE CORRECTIF DE CREDIT. Un seul facteur change.
    'pilepeak' = @('--elide-forced', '--hindsight', '0.5', '--assign-bias', '3',
                   '--adapt-to-peak')
    # Le correctif SEUL, pour l'attribuer si la pile bouge.
    'peak'     = @('--adapt-to-peak')
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
        --watch 81196066 --watch 88753594 --watch 24550676 --watch 54701958 `
        --probe-repeat `
        --options-online 60 `
        @($defs[$b]) `
        --solve-ms $Ms --seed $s --finisher levin --archive-k 24 `
        --outdir $out *> "$out.log"
    Write-Output "    EXIT=$LASTEXITCODE"
  }
}
