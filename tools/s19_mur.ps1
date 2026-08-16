# L'ETALON A NU, APRES LA SESSION 19 — la cascade re-mesuree.
#
# CE QUI A CHANGE DEPUIS 9.27 (c), ET QUI N'A JAMAIS ETE MESURE ICI :
#   1. `aux.Stringid` corrige : `Choice::card` est enfin renseigne sur les
#      prompts OUI/NON. Le pivot du combo (la defausse de Masquerade) etait
#      invisible au biais d'indices et aux sondes — en silence.
#   2. `--elide-forced` est CABLE : il etait inerte dans toute recherche.
#   3. `--hindsight 0.5` et `--adapt-to-peak` sont le DEFAUT.
#   4. `--op-recipes` pose le nœud « code ACQUERABLE » et `--op-bias` biaise
#      « que jouer » vers les operateurs que la decomposition designe.
#
# LE JUGE EST LA CASCADE MESUREE (9.27 (c)), et le goulot est l'OFFRE :
#     Masquerade actif           34,7 %
#     Wolf active                 4,5 %
#     Kaleido Chick — renommage   0,14 %   <-- LE GOULOT
#     Leo choisi quand offert     1,16 %
#     Leo au cimetiere            6 tirages
#     Liger invoque               0
# La question, et elle est binaire : la colonne « invoquee » de Liger passe-t-elle
# de zero a non nul ? Tout le reste est secondaire.
param([int]$Ms = 90000,
      [string]$Seed = '888',
      [string[]]$Bras = @('defaut', 'op'),
      [string]$Prefixe = 's19m')

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
$repo = "D:\ProjectIgnis\repositories"
$gabarit = "D:\ProjectIgnis\replay2video\combosolver\gabarits\etalon_a_lunalight.yrp"

foreach ($b in $Bras) {
    $out = "${Prefixe}_${b}_$Seed"
    Write-Output "--- $out"
    if ($b -eq 'defaut') {
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
            --watch 35618217 --watch 24550676 --watch 54701958 --watch 47705572 `
            --probe-repeat `
            --options-online 60 `
            --solve-ms $Ms --seed $Seed --finisher levin --archive-k 24 `
            --outdir $out *> "$out.log"
    } else {
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
            --watch 35618217 --watch 24550676 --watch 54701958 --watch 47705572 `
            --probe-repeat `
            --options-online 60 `
            --op-recipes --op-bias 3 `
            --solve-ms $Ms --seed $Seed --finisher levin --archive-k 24 `
            --outdir $out *> "$out.log"
    }
    Write-Output "    EXIT=$LASTEXITCODE"
}
