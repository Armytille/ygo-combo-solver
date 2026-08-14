# ETALON A EN MODE BUT SEUL, RE-MESURE APRES C1 (session 9).
#
# POURQUOI CETTE MESURE EXISTE
#
# Le §9.15 (e) comparait un bras arme du repertoire a un bras but seul, et
# concluait que le repertoire valait « la partie difficile ». Mais les deux bras
# ne differaient pas d'un seul facteur : ils differaient du repertoire ET d'un
# MECANISME CASSE. La passe LDS — celle qui avait produit le meilleur resultat
# du bras arme (2/4, 2 038 555 etats) — etait paralysee par le defaut C1 quand
# le plan est vide : `claims_size = plan.size() + 1` valait 1, soit UN jeton de
# partition pour seize workers, et toute deviation etait supprimee.
#
# C1 est corrige (ClaimTable a cle entiere, echec ouvert, refus comptes). Cette
# mesure refait donc le bras but seul sur un moteur ou la passe LDS fonctionne.
#
# CE QU'IL FAUT LIRE, ET DANS CET ORDRE
#
# 1. LA VERIFICATION DE C1, avant tout resultat : dans la table « recherche a
#    ecarts bornes autour du plan », le nombre d'ETATS doit CROITRE avec le
#    niveau d'ecart. Avant correction il valait 26, CONSTANT a tous les niveaux
#    — c'est ce chiffre que le §9.15 avait pris pour un fait structurel du mode.
#    S'il est encore constant, C1 n'est pas dans le binaire.
# 2. La colonne `partition` de la ligne d'elagage. Non nulle = le travail est
#    CEDE a un autre worker (partage) ; c'est ce que la session 8 ne pouvait pas
#    distinguer d'une SUPPRESSION.
# 3. Seulement ensuite : le board atteint, contre le 2/4 du bras arme.
#
# Reference AVEC repertoire (s7_luna_v6, 1800 s, graine 888) :
#   tirages 1 246 s / 12,66 M tirages / best 1/4
#   LDS a 0 ecart 2 038 555 etats / 294 s / best 2/4   <-- LE MEILLEUR
#   board atteint : 1 Liger + Bagooska ; MANQUE 2x Liger. 0 erreur de core.
#
# La cible est ATTEIGNABLE (ygocombo #105, 108 etapes) : c'est ce qui fait le
# prix de cet etalon.
param([int]$Ms = 1800000, [string]$Seed = '888')

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
$repo = "D:\ProjectIgnis\repositories"
$gabarit = "D:\ProjectIgnis\replay\_LastReplay.yrpX"

$out = "s9_A_c1"
Write-Output "--- etalon A but seul, apres C1 -> $out"
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
    --solve-ms $Ms --seed $Seed --finisher levin --archive-k 24 `
    --outdir $out *> "$out.log"
Write-Output "    EXIT=$LASTEXITCODE"
Write-Output "TERMINE"
