Tu reprends `combosolver`, un solveur de combo EDOPro écrit en C++ qui tourne
sur sa propre copie d'`ocgcore`. Dépôt git autonome, racine
`d:\ProjectIgnis\replay2video\combosolver`. Lis d'abord `README.md` puis
`docs/combo-solver-design.md` — les §9.1-9.13 documentent sept sessions,
chaque choix adossé à une mesure, impasses comprises. Ne redécouvre rien de ce
qui y est chiffré.

**LA MISSION DE CETTE SESSION : le RENDEMENT, après la fermeture des deux
leviers de la session 6.** L'escalade s'est TUE (quatre graines, dont une à
1 800 s, retrouvent 19/55/259 sans le battre) et le transfert de politique par
corpus a touché son PLAFOND DE REPRÉSENTATION (accord 44 % → 66 %, palier dès
la première passe, sur un poids par `plan_key` aveugle à l'état). Les deux
faits pointent le même endroit : **il faut changer quelque chose de
structurel, pas relancer un réglage.** Trois candidats, par rendement attendu,
détaillés plus bas : le CONTEXTE dans la clé de politique (chantier 5ter),
l'escalade PAR FENÊTRE `--fire` (chantier 6bis), le partage périodique de la
politique (chantier 4). La mission de FOND (battre 19 brûlées, coût
lexicographique) reste l'étalon n°1 ; `test 3`/`test 4` restent en PAUSE
(§9.10).

## L'état acquis (session 7, §9.13) — ne pas re-dériver

- **Meilleure ligne livrée : 19/55/259**
  (`sZ6_slack255/solution_00_b19_a55.yrp`, jugée : 0 retry, garde 35/35,
  Omega@terrain 2/2, Trishula 1/1) contre la référence 19/56/273.
- **L'ESCALADE EST CLOSE À CE RÉGLAGE** (le contraire de la session 6, qui la
  disait « le geste le plus rentable du répertoire »). Enracinée sur la ligne
  259 : graines 888 / 999 / 1234 à 600 s et 4242 à 1 800 s — **aucune ne bat
  19/55/259, toutes le RETROUVENT.** La barre du piège 41 (trois graines
  muettes consécutives) est atteinte. Détail : 888 → 3 conversions / 25
  lignes ; 999 → 3/25 ; 1234 → 4/37 ; 4242 (budget TRIPLE) → 3/25, ligne
  jugée depuis zéro 259/259, 0 retry, garde 35/0 découverte, Omega 2/2,
  Trishula 1/1. Ne pas relancer une graine de plus : ce
  n'est pas 259 qui est prouvé optimal, c'est le MÉCANISME (racine = meilleure
  ligne, reculs 60-150) qui a cessé de rendre.
- **Chantier 5bis (rejeu d'ADAPTATION, `--adapt`/`--adapt-passes`) : VIVANT
  mais sans effet — et sa limite est MESURÉE et EXPLIQUÉE.** Le relevé
  (`LiftPolicyRun`) conserve à chaque prompt multi-choix les `plan_key` légaux
  + le choisi (28 lignes, 4 614 décisions, 4,7 s) et l'injecte par le gradient
  NRPA, au même point que le prior par poids. La **courbe d'accord** imprimée
  au relevé (probabilité moyenne du coup joué, pour 0/1/2/4/8/16 passes) dit
  tout sans dépenser un run : **44,3 % → 65,2 % en une passe → 66,2 % à
  seize** ; sur une SEULE ligne 44,9 % → 70,2 %. Le plafond n'est pas le
  désaccord du corpus, **c'est la représentation** : un poids par `plan_key`
  est aveugle à l'état, la même identité revient partout avec des choix
  différents. A/B : étalon 2 → ensembles de conversion IDENTIQUES (10/15, déc.
  113-173, cinq précoces murées) mais états atteints différents (manques et
  crêtes bougent sur 4 et 7 fenêtres) ; étalon 1 → 19/55/259, dans la
  DISPERSION du témoin. Opt-in, hors commande recommandée. CLOS avec le prior.
- **Chantier 6 (racines croisées `--fire`) : CLOS SUR STRUCTURE, sans run.**
  `FireWindow::prefix` est un préfixe de LA MÊME passe de découverte : reculer
  la ligne d'une fenêtre avant le point d'une autre redonne l'état déjà connu,
  reculer après saute son injection. Aucune racine au niveau des RÉPONSES ne
  porte la menace d'une autre fenêtre.
- Sessions 1-6, acquis inchangés : borne B&B brûlées structurellement
  INACTIVE (0 coupure, l'espace gagnant vit sous le pic 23 de la référence) ;
  partage de borne neutre ; prior par POIDS réfuté ; LuaJIT FERMÉ sur pièces ;
  `--optimize` (anytime lexicographique) et `--fire` complet ; 4 familles de
  contres ; Zalen ne chaîne QUE par-dessus JS ; 19 brûlées résiste (k≤4
  ÉPUISÉ, k=5 incomplet à 2,33 M états).

## Les chantiers, par rendement attendu

1. **Chantier 5ter : le CONTEXTE dans la clé de politique.** C'est la suite
   directe de la mesure ci-dessus, et le seul levier dont on sait d'avance
   comment vérifier s'il vaut quelque chose : élargir `plan_key` d'un
   contexte grossier (phase, nombre d'invocations faites, progrès de rips —
   quelques bits) rend la politique dépendante de l'état. **Le test coûte
   quelques secondes, pas un run** : si la courbe d'accord de `--adapt` monte
   au-dessus de 66 %, la représentation respire ; si elle ne bouge pas,
   abandonner sans A/B. Attention aux effets de bord : la clé sert AUSSI de
   répertoire (`plan_index`), de dedup et de biais `known` — élargir la clé
   de politique sans élargir celle du répertoire (deux tables, pas une).
2. **Chantier 6bis : l'escalade PAR FENÊTRE `--fire`.** Le mécanisme prouvé
   (racine = meilleure ligne + reculs profonds) n'a jamais été branché là où
   il reste de la marge : écrire la meilleure APPROCHE de chaque fenêtre (les
   crêtes 7-8/8 des cinq fenêtres précoces, déc. 58-103) puis ré-enraciner
   CETTE fenêtre sur SA propre approche. Contrairement au chantier 6, la
   racine porte bien l'injection de sa fenêtre. Étalon 2, faits structurels.
3. **Chantier 4 : partage périodique de la POLITIQUE entre workers** (le plus
   ancien non-fait, session 3) : fusion périodique des poids (moyenne des
   logits) toutes les N itérations de niveau supérieur, ou snapshot sous le
   mutex de `NrpaShared`.
4. **Chantier 7 : LTS à frontière partagée** — 16 cœurs sur UNE racine
   prometteuse (~8 ms/expansion aujourd'hui, une racine par worker).
5. **Chantier 8 : miroir des zones depuis `MSG_MOVE` (profil D'ABORD).**
   Gain plausible 10-30 % de débit — à confirmer au profil avant d'écrire une
   ligne. Seul chantier de débit encore ouvert avec le 7.

Deux murs, à garder en tête (pas prioritaires sauf demande du joueur) :
**18 brûlées** (la voie qui tranche : relaxation SMT/ILP des ressources,
§9.10 ; côté recherche : fermer k=5 — 30-40 min avec `--tt-mb` plus grand —
et les reculs > 150) ; **les rips précoces** (le verrou est de SÉQUENCE, pas
d'échantillonnage : prior par poids ET adaptation réfutés, chantiers 5ter/6bis
ci-dessus).

## Les étalons de mesure (A/B, même graine, même budget)

```powershell
# construire
& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe" `
    build\combosolver.sln /p:Configuration=Release /p:Platform=x64 /m /nologo /v:quiet

# santé — LA porte de tout changement (0 retry, 290 digests distincts / 0
# fusion, 0 écart retrouve la référence, A/B nouveauté ; l'écart de couverture
# IDLECMD #240 est PRÉEXISTANT). Un mécanisme inerte sans son drapeau doit
# rendre une santé identique AUX DURÉES PRÈS — le diff complet le vérifie.
.\bin\Release\combosolver.exe "D:\ProjectIgnis\replay\synchron handrip 2.yrpX" `
    --scriptdir ..\deps\scripts_2026-04-13\script --solve --solve-ms 60000 `
    --outdir sX_sante --no-chain Zalen --no-chain "Crystal Wing"
# NB : --outdir dédié TOUJOURS — le défaut « solutions/ » écraserait le corpus.

# Étalon 1 — même-deck enraciné sur la ligne 259. Témoins déjà mesurés :
# s7_esc888 / s7_esc999 / s7_esc1234 (600 s) et s7_esc4242 (1 800 s) — s'en
# servir comme DISPERSION plutôt que de relancer un témoin.
.\bin\Release\combosolver.exe "D:\ProjectIgnis\replay\synchron handrip 2.yrpX" `
    --scriptdir ..\deps\scripts_2026-04-13\script `
    --start "D:\ProjectIgnis\replay\synchron handrip 2.yrpX" `
    --solve-ms 600000 --seed 777 --finisher levin --optimize `
    --finisher-min 420000 --burn-limit 19 --archive-k 24 `
    --approach "sZ6_slack255/solution_00_b19_a55.yrp" `
    --outdir sX_iter `
    --guard "5:Crystal Wing|Zalen@terrain+Junk Signal@main" --guard-off "mainadv<=2" `
    --no-activate "Duel Evolution - Assault Zone" `
    --no-chain Zalen --no-chain "Crystal Wing" `
    --resolve "PSY-Framelord Omega@terrain:2" `
    --resolve "Trishula, Dragon of the Ice Barrier@terrain"
# Métriques STRUCTURELLES : coûts écrits, conversions (<-- BUT), coupures,
# lignes anytime/finisseur. Les compteurs de tirages sont du bruit (§9.12).

# Étalon 2 — les fenêtres --fire précoces (le mur des rips ; chantier 6bis).
# ~6 min de mur : les 15 fenêtres tournent EN PARALLÈLE, une par thread.
.\bin\Release\combosolver.exe "D:\ProjectIgnis\replay\synchron handrip 2.yrpX" `
    --scriptdir ..\deps\scripts_2026-04-13\script `
    --fire "27204311" --fire-open --fire-bake --fire-ms 300000 `
    --fire-spare "Junk Signal" --fire-spare "Crystal Wing" --fire-spare "63436931" `
    --seed 888 --outdir sX_fire `
    --guard "5:Crystal Wing|Zalen@terrain+Junk Signal@main" --guard-off "mainadv<=2" `
    --no-activate "Duel Evolution - Assault Zone" `
    --no-chain Zalen --no-chain "Crystal Wing" `
    --resolve "PSY-Framelord Omega@terrain:2" `
    --resolve "Trishula, Dragon of the Ice Barrier@terrain"
# Ligne de base (sessions 6 ET 7, reproduite fenêtre par fenêtre, crêtes et
# « manque : » compris) : 10/15 converties (déc. 113-173), fenêtres 0-4
# (déc. 58-103) non converties. Toute conversion 58-103 = percée.

# Étalon 3 — débit brut (chantiers 7-8) : états/s de la phase tirages de
# l'étalon 1 (~90 k états/s), ms/expansion du finisseur (~8 ms).

# Étalon 0 — la COURBE D'ACCORD (chantier 5ter) : quelques secondes, aucun
# run. `--adapt <corpus>` imprime p(coup joué) pour 0/1/2/4/8/16 passes ;
# référence à battre 44,3 % -> 66,2 %.
... --start <ref> --solve-ms 3000 --optimize --adapt sF_final --outdir sX_probe

# mode JUGE : mêmes drapeaux sans --solve, sur n'importe quel replay produit.
```

## Les pièges qui ont coûté cher — ne pas les redécouvrir

1-31 : sessions 1-4 (liste dans les prompts précédents / §9 ; les plus
mordants : `Pop()` jamais `Discard()` (22) ; `--resolve`/`--summon-min` =
événements RARES uniquement (28, 31) ; une approche se rejoue sur le duel de
SON en-tête (21) ; jamais bit-à-bit à graine fixée (24) ; équivalence de but
sans position mais avec la face (26) ; pas d'élagage sur « carte brûlée »
(27) ; `Choice::card` couvre les fenêtres de chaîne (29)).

32. **`--outdir` par défaut = `solutions/` (le corpus)** — un `--outdir`
    dédié par run, toujours.
33. **Un « résiste à N déviations » sans `--optimize` est un arrêt à 16
    variantes, pas une preuve** — seules les passes ÉPUISÉ comptent.
34. **Les brûlées ne sont pas monotones** (pic 23 → 19) — mais voir 40 : la
    borne est de toute façon inactive sur ce problème.
35. **La poursuite d'après-but est active sous `--optimize`** : les
    « solutions » à k=0 sont des ré-atteintes, pas un bug.
36. **Une réponse posée non traitée est ÉCRASÉE par le SetResponse suivant**
    — avancer au prompt avant d'injecter.
37. **`no_chain` global ≠ continuation `--fire`** : `--fire-no-chain` est le
    drapeau de mise en scène.
38. **Une contrainte `--resolve` ne dit pas OÙ résoudre** : pour imposer un
    rôle, museler les alternatives.
39. **À graine fixée, le bruit de run domine les compteurs de tirages** — et
    **(session 7) même un fait STRUCTUREL se compare à une DISPERSION, pas à
    un run** : sur l'étalon 1, « 4 conversions / 37 lignes contre 3 / 25 »
    aurait été lu comme un gain si les trois graines témoins n'avaient pas
    couvert exactement cet intervalle. Trois témoins avant tout verdict.
40. **Une borne qui ne coupe jamais est un réglage mort** — instrumenter
    AVANT de calibrer. Corollaire session 7 : **l'instrument doit appliquer
    la MÊME mise à jour que le run** (`AdaptRun` libre, partagé entre la sonde
    et les workers), sinon il mesure autre chose.
41. **Une « convergence » d'escalade n'est qu'un plateau local** — trois
    graines muettes au moins. **(Session 7) la barre est maintenant
    ATTEINTE** sur la racine 259 : ne pas relancer l'escalade telle quelle.
42. **(Session 7) Un mécanisme peut être VIVANT et sans effet.** L'adaptation
    déplace réellement les états atteints (manques et crêtes changent) et ne
    change AUCUN verdict. « Ça bouge » n'est pas « ça rend » ; seuls les
    ensembles convertis et les coûts écrits tranchent.
43. **(Session 7) Un plafond de performance peut être un plafond de
    REPRÉSENTATION.** Avant d'inventer un mode d'injection de plus, se
    demander si la structure de données peut seulement exprimer ce qu'on lui
    demande — un poids par `plan_key` ne peut pas encoder une politique
    dépendante de l'état, et aucune quantité de passes n'y changera rien.

## Discipline de vérification — non négociable

La santé passe AVANT et APRÈS chaque changement de moteur ; 0 retry /
290 distincts / 0 fusion ; **à 0 écart la référence est retrouvée** ; A/B
nouveauté automatique ; tout replay écrit rejoué depuis zéro et jugé avec les
MÊMES drapeaux ; toute amélioration de coût vérifiée sur les trois coûts ET
la discipline avant annonce ; A/B sur faits structurels uniquement, comparés à
la dispersion du témoin (pièges 39, 42) ; un A/B perdant se documente et se
désactive par défaut ; les runs séquentiels (jamais deux mesures en parallèle
sur la machine).

## Cas de test et fichiers

- LE cas : `D:\ProjectIgnis\replay\synchron handrip 2.yrpX` (référence
  19/56/273 ; **meilleure ligne connue 19/55/259**,
  `sZ6_slack255/solution_00_b19_a55.yrp`)
- Corpus historique `solutions/` (NE PAS écraser) ; variantes `sF_final/`
  (261, corpus des `--prior`/`--adapt`), `sD_corpus/`, `sG_iter/` ; session
  6 : `sZ6_*` ; session 7 : `s7_esc{888,999,1234,4242}` (témoins de
  l'escalade), `s7_fire_base`/`s7_fire_adapt` (A/B étalon 2),
  `s7_e1_adapt888` (A/B étalon 1), `s7_sante`/`s7_sante2`
- Refermetures `--fire` : `sM_fire5/`, `sN_fire_deep/` (`--opp-hand
  "27204311"` pour juger), `sP_bake300/`, `sT_zalen3/`, `sY_zalen5/`,
  `sZ6_fire_base/`, `s7_fire_*` (cuits : jugeables sans drapeau)
- Scripts de run : `tools/s7_escalade.ps1`, `tools/s7_ab_fire.ps1`,
  `tools/s7_ab_e1.ps1` (paramétrés : graine, corpus, budget)
- Logs sessions 4-7 : `mA_*`→`mO_*`, `sA_*`→`sY_*`, `sZ6_*`, `s7_*` (ignorés
  par git)
- `--workdir` par défaut `D:\ProjectIgnis` (ne jamais y écrire)

## Bibliographie (vérifiée sur arXiv)

| Levier | Papier | arXiv |
|---|---|---|
| Prior par rejeu — POIDS et ADAPTATION tous deux mesurés, plafond de représentation | Policy Learning from Solved Games | 2401.10431 |
| Coût lexicographique NRPA (implémenté) | Montparnasse / MOGNRPALR | 2505.02110, 2606.07562 |
| LTS/PHS* (implémentés ; frontière partagée = chantier 7) | Policy-Guided Heuristic Search | 2103.11505 |
| Recul principled | √LTS | 2412.05196 |
| Archive d'états (fait) | Go-Explore | 2004.12919 |
| Stabilité NRPA | Stabilized NRPA | 2101.03563 |
| Adaptation lente niveau 1 (option chantier 4) | Montparnasse | 2505.02110 |
| Rejetés sur mesure | GNRPA-LR ; prior par poids ; adaptation par corpus | 2401.10420 ; 2401.10431 |

Définition de « terminé » pour la session 8 : (a) le chantier 5ter tranché sur
la COURBE D'ACCORD (quelques secondes) — implémenté et mesuré en A/B si elle
monte, abandonné et documenté sinon ; (b) le chantier 6bis (escalade par
fenêtre `--fire`) implémenté et mesuré en A/B structurel sur l'étalon 2, OU le
chantier 4 sur l'étalon 1 ; (c) tout gain/perte chiffré, les perdants
désactivés par défaut et documentés ; (d) §9.14 documenté, ce prompt régénéré.
