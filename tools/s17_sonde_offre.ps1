# SONDE D'OFFRE (session 17) — LA DECOMPOSITION DE LA LOI D'ARITE.
#
# CE QU'ELLE TRANCHE. La session 16 a mesure la loi (frequence d'une invocation
# ÷20 par materiau supplementaire, ZERO des qu'un materiau est NOMME) mais pas
# son LIEU. « Jamais invoquee » recouvre deux pannes opposees :
#   - JAMAIS PROPOSEE : le core ne liste une invocation que si elle est payable
#     a cet instant. La panne est dans l'ETAT -> chantiers 3 (--recipe-w) et
#     4 (--backward).
#   - PROPOSEE ET JAMAIS PRISE : troncature des sous-ensembles, poids de
#     politique. La panne est dans l'ECHANTILLONNAGE -> chantiers 1 (--assign)
#     et 2 (--hindsight).
# `--goal-bias` (session 16) a echoue faute exactement de cette lecture : il
# biaisait un choix qui n'existait pas.
#
# LE RUN EST NU. Aucun `--hint`, aucun `--resolve` (INDICE DEGUISE : biais
# d'office + gradient + exigence au but), aucun `--summon-min`, aucune
# reference. `--watch` ne fait que COMPTER. C'est la seule facon de mesurer
# « le solveur trouve-t-il SEUL ».
#
# CE N'EST PAS UN A/B : aucun bras temoin, aucune graine multiple a lire. Une
# sonde se lit sur ses propres compteurs. Le juge est le bloc « OFFRE ».
#
# BINAIRE : non-PGO assume — la sonde rend des FRACTIONS, pas des debits.
param([int]$Ms = 90000,
      [string]$Seed = '888',
      [string]$Prefixe = 's17')

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
$repo = "D:\ProjectIgnis\repositories"
$gabarit = "D:\ProjectIgnis\replay2video\combosolver\gabarits\etalon_a_lunalight.yrp"

# ETALON A NU. CODES ET JAMAIS NOMS, y compris pour `--watch` : « Lunalight
# Liger Dancer » est AMBIGU (54701958 ET 101301030) et l'outil SORT sur un nom
# ambigu — le run de la session 16 qui les passait par nom a donc surveille une
# liste TRONQUEE, ce que sa lecture n'a pas releve.
#   8379983  = Lunalight Gold Leo        3027001  = Fake Trap
#   54701958 = Lunalight Liger Dancer    24550676 = Lunalight Leo Dancer
#   88753594 = Lunalight Sabre Dancer    81196066 = Lunalight Perfume Dancer
#   90590304 = Bagooska
# L'ORDRE EST L'AXE D'ARITE, du moins cher au plus cher :
#   Perfume 2 materiaux -> Sabre 3 -> Leo 1 NOMME + 2 -> Liger 1 NOMME + 3.
$out = "${Prefixe}_offre_$Seed"
Write-Output "--- $out (etalon A NU, graine $Seed, $Ms ms)"
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
    --solve-ms $Ms --seed $Seed --finisher levin --archive-k 24 `
    --outdir $out *> "$out.log"
Write-Output "    EXIT=$LASTEXITCODE"

Write-Output ""
Write-Output "=== LECTURE : offre contre invocation ==="
Select-String -Path "$out.log" -Pattern 'sonde de repetition|Dancer :|OFFRE|JAMAIS PROPOSEE|PROPOSEE ET JAMAIS|conversion offre' |
    ForEach-Object { $_.Line }
