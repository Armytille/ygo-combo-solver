# GRAPHE DE RECETTES AMORCE PAR LE TEXTE (session 9, chantier 16 — second temps).
#
# CE QUE LE PREMIER A/B A ETABLI, et qui commande celui-ci
#
# Bras `--recipes 0` (graphe alimente, mesure, sans peser dans le cout) :
#   appr0 recul 0    rec=121    hR=1.0    (h plat au meme endroit : 1)
#   appr0 recul 10   rec=22240  hR=4.1    (h plat : 4)
#   appr0 recul 20   rec=24518  hR=5.0    (h plat : 5)
#   appr0 recul 30   rec=44324  hR=5.1    (h plat : 6)
#   appr0 recul 45   rec=14885  hR=5.0    (h plat : 5)
#
# Le graphe OBSERVE beaucoup (des dizaines de milliers d'invocations) et
# n'apporte que ~0,1 de gradient. La raison n'est pas un manque de donnees, elle
# est STRUCTURELLE : un graphe observationnel n'apprend que des invocations
# REUSSIES, et la carte qu'on cherche est celle qu'aucune ligne n'a jamais
# posee. Sa recette est donc inconnue, `Distance` rend son plancher, et `h`
# reste plat exactement la ou il faudrait qu'il renseigne.
#
# CE QUE CE SECOND A/B TESTE
#
# La regle 3 du chantier dit « le texte n'est qu'une AMORCE ; la verite vient de
# l'observation ». Le premier temps n'implementait que l'observation. L'amorce
# lit la ligne de materiaux du texte de carte —
#     "Lunalight Leo Dancer" + 3 "Lunalight" monsters
# — et n'en retient que les materiaux NOMMES entre guillemets. Liger Dancer
# obtient ainsi une recette exigeant NOMMEMENT Leo Dancer, qui est lui-meme une
# Fusion : la distance a Liger cesse d'etre le plancher.
#
# CE QU'IL FAUT LIRE, dans cet ordre :
#
# 1. `amorce par le texte : N recette(s) posee(s)` a l'en-tete. A zero, rien
#    n'est amorce et les bras ne mesurent rien (piege 52).
# 2. `hR` du bras SA contre `hR` du bras M ci-dessus. C'EST LA MESURE : si hR ne
#    monte pas, l'amorce n'a rien apporte et le chantier 16 est refute dans
#    cette forme. Si hR monte, le paysage s'est creuse — et seulement alors la
#    question « est-ce que ca fait gagner » a un sens.
# 3. Cartes cumulees des bras ponderes contre le temoin (20).
#
# Bras :
#   SA   --recipes 0            : amorce + observation, MESURE sans peser
#   SN   --recipes 0 --no-seed-recipes : temoin d'amorce (= bras M precedent)
#   SP1  --recipes 1            : l'amorce pese dans h
#   SP2  --recipes 2
param([int]$Ms = 300000,
      [int]$FinMin = 260000,
      [string]$Seed = '888',
      [string]$Approche = 's8_B_T_s888\best_approach_7of8.yrp')

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
$ref = "D:\ProjectIgnis\replay\synchron handrip 2.yrpX"

# SN est le TEMOIN D'AMORCE, et il n'est pas optionnel : le bras M de la session
# 9 a ete mesure sur un binaire ANTERIEUR a l'amorce. Comparer SA a ce M-la
# ferait varier deux choses (l'amorce et le binaire) — exactement ce que le
# cadran alpha a du refaire pour cette raison (§9.16 (d)). SN rejoue donc le
# temoin sur LE binaire courant.
$bras = @(
    @{ nom = 'SN';  arg = @('--recipes', '0', '--no-seed-recipes') },
    @{ nom = 'SA';  arg = @('--recipes', '0') },
    @{ nom = 'SP1'; arg = @('--recipes', '1') },
    @{ nom = 'SP2'; arg = @('--recipes', '2') }
)

foreach ($b in $bras) {
    $out = "s9s_$($b.nom)"
    Write-Output "--- amorce bras $($b.nom) -> $out"
    & .\bin\Release\combosolver.exe $ref `
        --scriptdir ..\deps\scripts_2026-04-13\script `
        --start $ref --no-plan --no-nrpa `
        --approach $Approche --finisher-min $FinMin `
        --solve-ms $Ms --seed $Seed `
        --no-chain Zalen --no-chain "Crystal Wing" `
        @($b.arg) --outdir $out *> "$out.log"
    Write-Output "    EXIT=$LASTEXITCODE"
}
Write-Output "TERMINE"
