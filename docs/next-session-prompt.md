Tu reprends `combosolver`, un solveur de combo EDOPro écrit en C++ qui tourne
sur sa propre copie d'`ocgcore`. Dépôt git autonome, racine
`d:\ProjectIgnis\replay2video\combosolver`. Lis d'abord `README.md` puis
`docs/combo-solver-design.md` — les §9.1-9.14 documentent sept sessions,
chaque choix adossé à une mesure, impasses comprises. Ne redécouvre rien de ce
qui y est chiffré.

**LA MISSION DE CETTE SESSION : exploiter le diagnostic de la 7bis — c'est la
MASSE de probabilité qui manque, pas la connaissance.** La mesure qui commande
tout (§9.14) : à politique VIERGE, le coup du corpus est déjà classé PREMIER
dans 96 % des cas — au plafond calculé (96,1 / 97,3 %). Le prior par poids,
l'adaptation par gradient et le contexte dans la clé agissaient tous les trois
sur le CLASSEMENT, la seule chose que la politique possédait déjà : d'où trois
résultats neutres ou négatifs. Ce qui est bas, c'est la probabilité moyenne du
bon coup (44 % vierge, 74 % au mieux) — et un tirage enchaîne ~160 décisions,
donc 0,74^160 ≈ 10^-21. **Tout mécanisme qui se contente de re-pondérer les
coups est prédit NEUTRE ; seuls comptent ceux qui imposent la MASSE.** Deux en
existent : la température (`--nrpa-temp`, essayée — première conversion de la
bande précoce, non reproduite) et le rejeu dur d'un préfixe (`--approach`,
qui est exactement pourquoi l'escalade a été le seul mécanisme rentable
272 → 259). La mission de FOND (battre 19 brûlées) reste l'étalon n°1 ;
`test 3`/`test 4` restent en PAUSE (§9.10).

## L'état acquis (sessions 7 et 7bis, §9.13-9.14) — ne pas re-dériver

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
- **Chantiers 5bis / 5ter : implémentés, mesurés, CLOS.** `--adapt` (rejeu
  d'adaptation par gradient) : vivant, neutre sur les deux étalons.
  `--ctx-shrink` (politique à DEUX niveaux, un poids par coup ET un par
  (coup, contexte), mélange convexe `s = n/(n+k)` — d'après MCPS
  arXiv:2510.06381) : **perdant, et de façon MONOTONE dans le cadran** — étalon
  2, témoin 10/15, k=64 → 9/15, k=1 → 7/15. Cause mesurée : une recherche par
  fenêtre apprend de ses propres tirages, cette donnée est rare, et répartir
  l'évidence sur 20 contextes coûte plus que les +4 points de classement
  qu'elle achète. Les deux drapeaux restent opt-in, éteints par défaut.
- **Chantier 5quater : la TEMPÉRATURE (`--nrpa-temp`) — le seul levier de MASSE
  essayé, et le seul qui ait jamais ouvert la bande précoce.** τ=0,5 graine 888 :
  **11/15, sur-ensemble strict du témoin, dont la fenêtre déc. 89 — première
  conversion de la bande déc. 58-103, murée sur six runs antérieurs**, 16/16
  replays jugés depuis zéro (0 retry, Omega 2/2, Trishula 1/1). Mais τ=0,5 aux
  graines 999 et 1234, et τ=0,25 à 888, retombent EXACTEMENT sur le témoin. Le
  gain n'est donc pas reproductible ; ce qui l'est : la température ne perd
  jamais de fenêtre. Éteinte par défaut (τ=1,0).
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

1. **τ VARIABLE (la suite directe du seul levier qui ait ouvert la bande
   précoce).** Un τ constant à 0,5 gagne une fenêtre sur une graine et rien sur
   deux autres : le réglage est trop grossier. Les formes à essayer, toutes à
   un A/B de distance : τ décroissant le long du tirage (explorer tôt,
   s'engager tard) ; τ par CONTEXTE (concentré là où l'évidence est forte) ;
   τ décroissant avec les redémarrages, à la manière du recuit. Étalon 2, trois
   graines, faits structurels — et le témoin y est INVARIANT par graine, ce qui
   rend l'A/B lisible.
2. **Chantier 6bis : l'escalade PAR FENÊTRE `--fire`.** Le mécanisme prouvé
   (racine = meilleure ligne + reculs profonds) appartient à la famille qui
   IMPOSE la masse — la seule qui ait jamais rendu. Il n'a jamais été branché
   par fenêtre : écrire la meilleure APPROCHE de chaque fenêtre précoce (crêtes
   7-8/8, déc. 58-103) puis ré-enraciner CETTE fenêtre dessus. Contrairement au
   chantier 6 (clos), la racine porte bien l'injection de sa fenêtre.
3. **Borne inférieure LP/IP par comptage d'opérateurs (18 brûlées).** La forme
   concrète que §9.10 cherchait depuis trois sessions : variables = nombre
   d'usages de chaque action, contraintes = flux des cartes entre zones,
   objectif = minimiser les brûlées. Une borne ≥ 19 est la preuve d'absence
   sous la relaxation ; et la borne étant admissible elle sert AUSSI
   d'heuristique. Cadre : arXiv:2404.07934, approximation polynomiale
   arXiv:1605.07989. Travail hors moteur, sans risque de régression.
4. **Landmarks généralisés appris depuis les plans résolus**
   (arXiv:2508.21564) : apprendre du corpus non plus des préférences de coup
   mais un GRAPHE ORIENTÉ de sous-buts. C'est le seul transfert de corpus qui
   ne soit pas un re-classement — donc le seul non réfuté par la mesure de
   §9.14.
5. **Chantier 4 : partage périodique de la POLITIQUE entre workers** (le plus
   ancien non-fait, session 3).
6. **Chantiers 7 et 8 : LTS à frontière partagée ; miroir des zones depuis
   `MSG_MOVE` (profil D'ABORD).** Les deux seuls chantiers de débit ouverts.

Deux murs, à garder en tête (pas prioritaires sauf demande du joueur) :
**18 brûlées** (la voie qui tranche : borne LP/IP, chantier 3 ci-dessus ; côté
recherche : fermer k=5 — 30-40 min avec `--tt-mb` plus grand — et les reculs
> 150) ; **les rips précoces** — mur ENTAMÉ pour la première fois (déc. 89,
τ=0,5), le verrou est un déficit de MASSE, pas de connaissance ni de séquence
(§9.14).

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

# Étalon 0 — la SONDE DE POLITIQUE (quelques secondes, aucun run). `--adapt`
# imprime : le plafond par point de décision, puis une grille passes x alpha x
# niveau contextuel avec DEUX métriques par case — moyenne géométrique de
# p(coup joué) / fraction d'étapes où ce coup est classé PREMIER. Ne jamais
# lire la première seule (piège 44). Références à battre : plafond 96,1 %
# (sans contexte) / 97,3 % (avec) ; vierge 44 % / 96 % ; adaptée 66 % / 93 %.
... --start <ref> --solve-ms 2000 --optimize --adapt sF_final --outdir sX_probe

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
43. **(Session 7, RETIRÉ par la 7bis)** « Un plafond de performance peut être
    un plafond de REPRÉSENTATION » — c'était une conclusion tirée d'un
    instrument mal lu. Voir 44.
44. **(Session 7bis) Une moyenne GÉOMÉTRIQUE de probabilités, lue seule, ment.**
    Elle est écrasée par une poignée de termes proches de zéro : elle affichait
    44 % là où la politique classait déjà le bon coup PREMIER 96 % du temps.
    Toute métrique de politique se lit à deux colonnes (masse ET classement) et
    se compare à un PLAFOND calculé, sinon on optimise un chiffre qui ne
    mesure pas ce qu'on croit. Trois sessions de mécanismes ont été construites
    sur cette erreur.
45. **(Session 7bis) Distinguer les leviers de CLASSEMENT et les leviers de
    MASSE.** Re-pondérer les coups (prior, adaptation, contexte) ne peut rien
    quand le classement est déjà au plafond ; seul ce qui concentre la
    probabilité (température) ou l'impose (rejeu d'un préfixe, `--approach`)
    change un tirage de 160 décisions. Classer un mécanisme dans l'une des deux
    familles AVANT de l'écrire prédit son signe.
46. **(Session 7bis) Une perte MONOTONE dans un cadran est un vrai résultat.**
    Trois bras ordonnés par un seul réglage (`--ctx-shrink` 1 / 64 / éteint) et
    trois résultats ordonnés (7 / 9 / 10 fenêtres) : c'est ce qui distingue un
    mécanisme nuisible du bruit, là où un point isolé ne prouverait rien.

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
  `tools/s7_ab_e1.ps1`, `tools/s7_ab_ctx.ps1`, `tools/s7_ab_temp.ps1`,
  `tools/s7_temp_seed2.ps1` (paramétrés : graine, corpus, budget, τ, k)
- Session 7bis : `s7_fire_ctx`/`s7_fire_ctx64` (chantier 5ter, perdant),
  `s7_fire_t05` (**la bande précoce entamée** — 16 replays jugés),
  `s7_fire_t025`, `s7_fire_s999_t*`, `s7_fire_s1234_t*`, `s7_both`/`s7_zero`
  (sondes de politique)
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

Définition de « terminé » pour la session 8 : (a) le τ VARIABLE implémenté et
tranché en A/B structurel sur l'étalon 2, trois graines — la question précise
est si la conversion de la déc. 89 devient REPRODUCTIBLE ; (b) le chantier 6bis
(escalade par fenêtre) OU la borne LP/IP implémenté et mesuré ; (c) tout
gain/perte chiffré, les perdants désactivés par défaut et documentés ;
(d) §9.15 documenté, ce prompt régénéré.
