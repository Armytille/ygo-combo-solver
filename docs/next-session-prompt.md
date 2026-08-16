# Session 15 — CORRIGER LES IMPLÉMENTATIONS, PAS LES CONTOURNER

Tu reprends `combosolver` (racine `d:\ProjectIgnis\replay2video\combosolver`).
Lis `README.md` puis `docs/combo-solver-design.md` **§9.21** (session 14) et
§9.20. **Ne redécouvre rien de ce qui y est chiffré.**

## LA MISSION, fixée par l'opérateur en fin de session 14

> « Nos implémentations sont défaillantes ; plutôt que de fallback sur des
> solutions bricolées non optimales, il faut corriger. »

La session 14 a trouvé **six mécanismes à moitié câblés** (tableau plus bas) et
s'est fait prendre deux fois à mesurer autour d'eux au lieu de les réparer. La
session 15 répare. Le chantier principal est nommé et il est gros : implémenter
pour de vrai la **statistique de permutation `Q̂`**.

## LE RECADRAGE — MCPS n'a jamais été implémenté

`search.h` dit que la politique à deux niveaux est « inspirée de MCPS
(arXiv:2510.06381) ». **C'est trompeur et il faut corriger le commentaire.** Le
papier a été lu en séance ; voici ce qu'il fait :

- MCPS est un **MCTS** : arbre, compteurs de visites par nœud, et **trois
  estimateurs qui sont des MOYENNES DE RÉCOMPENSE** —
  `Q(s,a)` (parties commençant par le chemin puis `a`), `Q̃(s_r,a)` (AMAF chez
  l'ancêtre de référence), et **`Q̂(s_r,a)` : les parties contenant `a` ET tous
  les coups de `s`, dans n'importe quel ordre, n'importe où** (la contribution
  propre du papier, rattachée par lui à Virtual Global Search).
- Combinaison : `val = (n·Q + ñ·Q̃ + n̂·Q̂) / (n + ñ + n̂)` — poids
  **proportionnels aux effectifs**, combinaison convexe qui minimise la variance
  sous indépendance. C'est ce qui **supprime l'hyperparamètre de biais** de GRAVE.
- Machinerie : **un bitset par code de coup** sur une fenêtre glissante de
  W = 10 000 parties récentes + le tableau des récompenses ; `Q̂` se calcule par
  `popcount` sur l'intersection. Les stats de permutation d'un nœud sont gelées
  quand il atteint ρ visites et propagées à son sous-arbre.

Notre `EffectiveWeight` fait `(1−s)·w_global + s·w_ctx` sur des **logits**, mis à
jour par le **gradient NRPA de la seule meilleure séquence**. Ni arbre, ni
compteur de visites, ni moyenne de récompense, ni bitset. Une combinaison convexe
de deux logits n'est pas une combinaison convexe de deux taux de victoire.

**`--mcps` (session 14) est un conditionnement ajouté au mauvais objet.** Il est
RÉFUTÉ deux fois : (a) en recherche — 0 comparaison gagnée sur 4 contre le simple
fait d'allumer l'ancien niveau, plus un **effondrement total** à k=6 (`best 0/4`,
99 % de morts au tour, 27 lignes distinctes retenues sur 8 751 offertes :
verrouillage de bassin, le gradient pousse +α sans le contrepoids qu'une moyenne
sur toutes les parties apporterait) ; (b) en représentation — la courbe d'accord
donne le même palier que l'ancien conditionnement (59 contre 59). **Le désarmer
ou le marquer explicitement comme tel ; ne pas le régler.**

## CHANTIER 1 — `Q̂` POUR DE VRAI

C'est la seule partie de MCPS transportable dans une architecture **sans arbre**,
et c'est exactement la mémoire d'état que le diagnostic de l'opérateur réclame :
*si Tenki cherche autre chose que Gold Leo, la ligne meurt en ~20 décisions ; le
solveur ne devrait pas avoir besoin d'indice et devrait le trouver seul et vite.*
À la première décision `s = {}`, donc `Q̂({}, a)` est la récompense moyenne des
lignes ayant joué `a` — **moyennée sur TOUS les tirages, y compris les mauvais**.

Ce qu'il faut construire :
- un **bitset par code de coup** sur une fenêtre glissante des W derniers tirages
  (par worker ; W à calibrer, 10 000 chez le papier) ;
- le tableau parallèle des **récompenses** de ces W tirages ;
- `Q̂(s,a)` et `n̂(s,a)` par `popcount` sur l'intersection des bitsets de
  `{a} ∪ coups(s)` ;
- la règle de combinaison **à poids proportionnels aux effectifs** — et non un
  biais additif calibré à la main, sinon on réintroduit exactement
  l'hyperparamètre que le papier supprime.

Deux difficultés de conception à trancher, et elles sont réelles :
1. **Moyennes contre logits.** NRPA échantillonne un softmax de logits ; MCPS
  choisit l'argmax de moyennes de récompense. Les composer naïvement rendrait un
  hyperparamètre. Piste : appliquer la règle de sélection MCPS **aux k premières
  décisions seulement** (un vrai bandit en haut de l'arbre, là où le branchement
  est étroit — mesuré : **207 cases** couvrent les 6 premières décisions) et
  laisser NRPA échantillonner au-delà.
2. **Quelle récompense ?** Notre score de tirage est
  `matériel×1000 + nouveauté`, non borné et non comparable d'un run à l'autre.
  `Q̂` a besoin d'une récompense normalisée. À définir explicitement AVANT de
  coder (la normaliser par le meilleur score courant est un choix, pas une
  évidence).

**COMMENT LE JUGER — et le piège d'instrument à connaître d'avance.** La courbe
d'accord du corpus (`--adapt`, déterministe, quelques secondes) **NE PEUT PAS**
juger `Q̂` : elle mesure la reproduction d'un corpus qui ne contient QUE des
bonnes lignes, alors que `Q̂` tire son signal des ÉCHECS. C'est un instrument
gratuit et aveugle à cette question précise. Le juge est donc en recherche :
≥2/≥3 et l'approche écrite, appariés multi-graines.
**Sonde préalable obligatoire** (elle, gratuite) : imprimer pour chaque cible de
Tenki les `n̂` et `Q̂` accumulés. Si Gold Leo n'y ressort pas nettement au bout de
quelques milliers de tirages, le mécanisme ne marche pas et aucun A/B n'est utile.

## CHANTIER 2 — LES CINQ AUTRES DÉFAILLANCES CONFIRMÉES

| mécanisme | état réel, vérifié session 14 |
|---|---|
| `novelty_rollout_cut` | câblé dans `Rollout()` (tirage GLOUTON) seulement — **jamais dans `PolicyRollout`**, qui fait tout le travail. Le verdict de nouveauté y est pourtant CALCULÉ à chaque décision (4 requêtes core, ~60-80 sondes) et jeté après un simple départage |
| `allow_phase_change` | lu au prompt *idle* ([enumerate.cpp:225](enumerate.cpp#L225)), **pas** au prompt *battle* où les phases sont émises inconditionnellement ; aucun drapeau CLI |
| `canonical_zones` | déclaré, documenté, **allumé nulle part** (9.20 (e)) |
| archive Go-Explore | clé de tri `résolutions<<44 \| overlap<<36 \| ~décisions` : les résolutions SATURENT sur étalon A (tout à `r4`), donc l'archive range des **fins de ligne**. Conséquence mesurée : les 37 racines du finisseur rendent `ÉPUISÉ` en **0 à 13 expansions** |
| `--finisher-options` | écrit et compilant (arêtes macro dans `RunLevin`), **jamais jugé**. Contrôle REDÉFINI d'avance dans `tools/s14_aretes_macro.ps1` : mêmes best par racine / aucune solution perdue / ÉPUISÉ toujours ÉPUISÉ — **pas** le compte d'expansions. Sans objet sur étalon A (rien à compresser dans 3 expansions) ; à juger sur **étalon B contraint**, où 9.20 (d) mesure 32 lignes arrivées au board par le finisseur |

Le sixième était la politique à deux niveaux elle-même : **éteinte par défaut
depuis la session 7** (`ctx_shrink = -1`) et jamais mesurée dans ce régime. Elle
l'a été (bras `ctx8`) : **×4,5 et ×1,5 sur ≥3, 2 graines sur 2**. À confirmer sur
plus de graines, puis à passer par défaut si ça tient.

## CE QUI EST ACQUIS — ne pas re-mesurer (détail §9.21)

- **`--options-online` tient la mission** : étalon A but seul, 300 s, zéro
  corpus / zéro `--approach` / une traite → **2/4 sur 3 graines sur 3** contre
  1/4 sur 0/3 pour le run nu ; ≥3 de 0/0/25 à 76 k-116 k. À 1 200 s le nu y
  arrive aussi : le gain est en BUDGET, pas en plafond.
- **RÉFUTÉS** : `--options-ctx` dans la boucle en ligne (elle AUGMENTE les
  avortements, 0,25→0,38) ; `--options-len 16/24` (le catalogue s'effondre à
  1 macro et la perte de Levin EMPIRE : 28,6 → 32,0/35,4) ; `--options-pool 24` ;
  `--mcps` (ci-dessus). La saturation des catalogues à `max=8` était un symptôme
  de la sélection gloutonne, pas un manque.
- **`--nrpa-lr 4`** : positif sur ≥3 aux 4 paires (dont 0 → 701 et 0 → 1 536 sur
  les bras nus), neutre sur la conversion, et il **divise les avortements de
  macros par 2-3**. R=2 reste réfuté (s4). Opt-in.
- **Plafond de représentation** : accord du corpus, palier 52 (niveau éteint) →
  59 (contextuel) → **64 (mémorisation pure)**. Cinq points de marge : aucun
  meilleur descripteur de contexte ne sauvera la masse.

## MÉTHODE — ce que la session 14 a appris sur ses propres mesures

- **Un run coûte EXACTEMENT son budget** : préambule complet ~80 ms, run réel
  287 s pour `--solve-ms 300000`. Il n'y a RIEN à récupérer dans l'outillage ; le
  coût d'un A/B est son plan d'expérience.
- **Cribler sur le critère INTERNE du mécanisme** (`tools/s14_crible.ps1`, 90 s) :
  `--options-len` était réfutable par la perte de Levin du mineur dès le premier
  tour de minage. Le criblage ÉLIMINE, il ne promeut jamais ; à 90 s, ≥2/≥3 et
  l'approche écrite ne sont PAS des juges.
- **Avant tout A/B qui touche la représentation de la politique**, faire tourner
  la courbe d'accord (`tools/s14_accord_mcps.ps1`) : déterministe, quelques
  secondes, et elle dit si le mécanisme change ce que la politique PEUT
  représenter avant de dépenser des runs. (Mais elle est aveugle à `Q̂`, cf.
  chantier 1.)
- **BANDE DE BRUIT** : à graine fixée, ≥3 varie d'un facteur **~2**, et la
  conversion bascule 1/4 ↔ 2/4. Une conversion mono-graine ne vaut RIEN ; un
  écart de moins de 2× sur ≥3 non plus.
- **Le juge est l'APPROCHE ÉCRITE** (`best_approach_kof4.yrp`), pas la ligne
  `NRPA … best k/4` qui ne couvre que la phase tirages — elle aurait compté
  1 conversion sur 3 au lieu de 3 sur 3. `tools/s14_lecture.ps1` lit les deux.
- **Jamais mesuré, scripts prêts** : `tools/s14_parallelisme.ps1` (2 processus à
  8 threads contre 1 à 16 — si la contention est intra-processus et non la bande
  passante, c'est ~1,6× sur tous les A/B futurs) et le budget de criblage à 120 s.

## PIÈGES DE BUILD ET D'ENVIRONNEMENT

- Un `MSBuild` ordinaire RELIE SANS `/USEPROFILE` : après tout changement de
  moteur → rebuild, santé stricte, puis un pipeline PGO **propre à la session**
  (`tools/s14_pgo_b.ps1` comme modèle — ne jamais écraser le témoin précédent).
  Témoins : `combosolver_preopt`, `_lto_s12`, `_lto_s13`, `_lto_s14`, `_lto_s14b`.
  NB : les `.pgc` des sessions passées ne sont pas effacés avant l'entraînement
  (procédure telle qu'elle tourne depuis la s12, conservée pour comparabilité).
- **Le gabarit est désormais ÉPINGLÉ** (`gabarits/etalon_a_lunalight.yrp`). Il
  pointait sur le `_LastReplay.yrpX` d'EDOPro — un fichier VIVANT, réécrit en
  plein pilote le 15/08/2026 à 23:42. L'outil a échoué bruyamment et l'en-tête de
  gabarit imprimé a permis de vérifier que les runs antérieurs portaient le même
  duel. **La `.cdb` reste non épinglée** : à vérifier d'abord si un A/B
  inter-jours diverge sur l'énumération.
- Runs séquentiels, un `--outdir` par run, jamais de relink pendant une mesure.
  Logs PS 5.1 en UTF-16 : `Select-String`/`pwsh`, jamais `grep`.

## Montage de mesure

```powershell
# construire
& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe" `
    build\combosolver.sln /p:Configuration=Release /p:Platform=x64 /m /nologo /v:quiet

# sante — LA porte de tout changement de moteur, avant ET apres
.\bin\Release\combosolver.exe "D:\ProjectIgnis\replay\synchron handrip 2.yrpX" `
    --scriptdir ..\deps\scripts_2026-04-13\script --solve --solve-ms 60000 `
    --outdir s15_sante --no-chain Zalen --no-chain "Crystal Wing"
# 273 digests, 210/273, 209 candidates, 16 replays sur 209, 19/56/272.
```

Outils : `s14_crible.ps1` (criblage 90 s), `s14_accord_mcps.ps1` (courbe
d'accord), `s14_masse_ab.ps1` (A/B étalon A, bras nommés), `s14_lecture.ps1`
(juges + vie du mécanisme), `s14_aretes_macro.ps1` (contrôle du finisseur),
`s14_parallelisme.ps1`, `s13_exploitation_longue.ps1` (étalon B contraint).

## Définition de « terminé »

(a) `Q̂` implémenté fidèlement — bitsets, fenêtre glissante, poids proportionnels
aux effectifs — avec la sonde `n̂`/`Q̂` par cible de Tenki lue AVANT tout A/B.
(b) `--mcps` désarmé ou explicitement marqué comme conditionnement au mauvais
objet, et le commentaire MCPS de `search.h` corrigé. (c) Au moins deux des cinq
défaillances du chantier 2 réparées et jugées. (d) §9.22 documenté, ce prompt
régénéré.
