# Session PERFORMANCE : rendre une simulation moins chère

Tu reprends `combosolver` (racine `d:\ProjectIgnis\replay2video\combosolver`) pour
**une seule chose** : réduire le temps que coûte UNE décision simulée. Pas
l'algorithme, pas l'heuristique, pas la politique — le coût unitaire.

Lis d'abord `README.md`, puis `docs/combo-solver-design.md` §5 (l'arène), §7 (les
mesures du jalon 0), §9.8 (les points chauds déjà réglés) et §9.16 (i) (l'audit,
dont la section 6 est précisément ce chantier). **Ne redécouvre rien de ce qui y
est chiffré.**

---

## D'abord, ce que l'accélération peut et ne peut PAS faire

Il faut le dire avant de commencer, sinon la session se ment sur son objectif.

**Ce qu'elle ne fera pas : débloquer l'étalon A.** Le mur mesuré au §9.14 est
`0,74^160 ≈ 10⁻²¹`. Un facteur 2, 5 ou même 100 sur le débit ne déplace pas une
grandeur de vingt et un ordres de grandeur. Le §9.9 l'a d'ailleurs déjà tranché
sur pièce, et c'est le titre de la section : *la mémoire de politique bat la
vitesse brute*. Si tu finis la session en écrivant « ×3, donc on devrait trouver
plus », tu auras écrit un piège 45 de plus.

**Ce qu'elle fera, et qui vaut la session : rendre les MESURES abordables.** Le
coût réel du projet aujourd'hui est le temps d'A/B. Un cadran de neuf bras sur
l'étalon 0 coûte 45 minutes ; l'étalon A coûte 30 minutes par bras ; la session 9
a dû choisir entre prolonger un cadran et mesurer autre chose. Un ×3 sur le débit,
c'est un cadran trois fois plus fin **pour le même temps humain**, et c'est ce
qui a manqué à chaque session depuis la 5. C'est l'objectif : le débit comme
instrument, pas comme espoir.

Le compte de la session 10, puisqu'il est frais et qu'il chiffre l'argument :
**deux heures d'horloge pour trois mesures et demie.** Un run de santé (60 s),
quatre bras d'amorce sur l'étalon B (4 × 5 min), l'étalon A (30 min, dont
21 minutes de tirages avant que la table qui portait la vérification n'apparaisse),
et un A/B de quatre bras sur l'étalon A qui a dû être **abandonné en cours** parce
que la session avait déjà consommé son temps. C'est ce budget-là que ce chantier
achète, et rien d'autre.

Un corollaire pratique qui vaut d'être écrit : **fais imprimer tôt ce qui se
décide tôt.** Cette même session a répondu à sa question de fond — « la distance
amorcée dépasse-t-elle le `h` plat ? » — en **cinq secondes** de run, parce que la
table des distances s'imprime à l'en-tête, avant toute recherche. Trente minutes
de recherche n'étaient nécessaires que pour la question suivante. Une partie du
« c'est trop lent » n'est pas du débit, c'est de l'ordre d'impression.

Corollaire à tenir : **un bras plus rapide explore plus, donc trouve plus.** Cela
ne dit rien sur la correction du changement. Voir le protocole ci-dessous — c'est
la partie de ce prompt qu'il ne faut pas sauter.

---

## LE PROTOCOLE — il diffère de tous les autres A/B du dépôt

Partout ailleurs, le contrôle est « le diff du run de santé ne contient QUE des
durées ». **Ici les durées bougent par construction**, donc ce contrôle ne dit
plus rien. Il se dédouble :

1. **Contrôle de CORRECTION, à budget de NŒUDS égal.** Fixe le budget en
   expansions / décisions, pas en millisecondes, et exige l'égalité des faits
   structurels : mêmes 273 digests, 210/273 coups identifiés, référence retrouvée
   à 0 écart, 209 solutions, 19/56/272, « résiste à 12 déviations », 16 replays.
   Un changement de perf qui modifie un seul de ces nombres a changé la
   SÉMANTIQUE, pas la vitesse — il est à jeter, même s'il est plus rapide.
   L'étalon 0 (`tools/s9_ab_alpha.ps1`) donne le contrôle gratuit : la racine
   `recul 0` rend **42 expansions, `b=0`, ÉPUISÉ**, dans tous les bras.
2. **Contrôle de GAIN, à temps égal.** Même commande, même graine, même budget en
   millisecondes : ce qui doit monter, c'est le nombre d'expansions et d'états,
   et rien d'autre ne doit changer de nature.

Le reste de la discipline du dépôt s'applique sans exception : **runs
séquentiels** (jamais deux mesures en parallèle, et ne pas relier le binaire
pendant qu'une mesure tourne — le lien échoue, c'est la bonne protection), un
`--outdir` dédié par run, et la santé avant/après.

Et le piège 39 vaut ici aussi : à graine fixée, deux runs divergent. **Mesure
chaque bras trois fois** et compare des médianes, pas des runs. Un « +12 % »
mesuré une fois est du bruit.

---

## ÉTAPE 1, NON NÉGOCIABLE : le profil AVANT la première optimisation

C'est le piège 40 appliqué à la performance : *une borne qui ne coupe jamais est
un réglage mort ; instrumenter AVANT de calibrer.* Aujourd'hui, personne ne sait
où part le temps. Le §9.16 avance « ~250 µs par décision, dominée par
`duel.Process()` et `arena.Restore()` », mais ce chiffre n'a jamais été imprimé
par un run — c'est une estimation, pas un instrument (piège 52).

Ce qui existe déjà, et qui est SOLIDE parce que mesuré au jalon 0 (§7) :

| grandeur | mesure |
|---|---|
| heap vif d'un duel | 4,1 Mo vifs, 7,4 Mo servis |
| pages sales par décision | 60 médianes (240 Ko), 97 en moyenne |
| restauration incrémentale | **0,05 ms** (contre 0,68 ms en image complète) |
| pas de moteur seul | 0,18 ms (mesure du jalon 0) |
| arrêt du GC Lua | **aucun gain** — les pages sales viennent des allocations neuves |

Ce qu'il faut ajouter, et imprimer par phase, par worker, agrégé en fin de run.
**Les sites sont peu nombreux et ils sont tous repérés** — c'est du travail de
placement, pas de recherche :

| sonde | où, nommément |
|---|---|
| le core lui-même | `Duel::Process()` — `duel.cpp:170`, deux lignes |
| requêtes de zone | `Duel::Query()` ×2, `Duel::QueryCodes()`, `Duel::ProcessorState()`, `Duel::Count()` — `duel.cpp:211-300` |
| arène | `Arena::Allocate/Free/Reallocate` (`arena.cpp:195-256`) et le couple instantané/restauration |
| énumération | `EnumerateInto()` / `Enumerate()` — `enumerate.cpp:628-647` |
| digest | `StateDigest()` — déclaré `search.h:173` |
| **reste** | le total de phase moins la somme ci-dessus, **imprimé explicitement** — sans ligne « reste », un profil ment par omission |

Pour `Query*`, ce n'est pas le cumul qui décidera de C19 mais le **nombre
d'appels par décision** ; pour l'arène, c'est le nombre de **pages touchées**,
que le compteur existant sait déjà donner.

**Trois exigences de méthode, et la première a déjà coûté une conclusion fausse
au dépôt.**

1. Compteurs `thread_local`, **agrégés par flush explicite** — le piège 58 dit
   qu'un `thread_local` lu depuis un autre thread mesure zéro, toujours, et c'est
   ainsi que `host_fallbacks` a imprimé « aucune » pendant seize workers. Le
   montage sûr est un objet `thread_local` à **destructeur**, qui verse ses
   compteurs dans les atomiques globaux à la mort du thread (les workers sont
   créés et joints par phase, donc le versement est garanti) ; le thread
   principal, lui, doit flusher explicitement avant impression.
2. **Ne PAS incrémenter un atomique partagé par appel.** Sur un chemin à ~10⁵–10⁶
   appels par seconde et par worker, seize workers qui tapent la même ligne de
   cache mesurent surtout leur propre contention — l'instrument fabriquerait le
   ralentissement qu'il prétend observer.
3. Il doit pouvoir **s'éteindre** (`--profile`), parce qu'un appel d'horloge sur
   un chemin à ~10⁵ appels/s se paie lui-même. **Chiffrer le coût de l'instrument
   fait partie de l'étape 1** : même commande, même graine, `--profile` puis sans,
   et l'écart d'expansions est le prix de la mesure. S'il dépasse quelques
   pour cent, passer à `__rdtsc()` avec une calibration unique.

**Note de build, pour ne pas perdre une demi-heure dessus.** `premake5.lua`
déclare `files { "*.cpp", "*.h" }` : c'est un **glob évalué à la génération**, donc
un `profile.cpp` neuf ne sera pas vu par le `.vcxproj` existant et il faudra
régénérer la solution (commande dans le README). Le contournement propre, si tu
veux éviter ce cycle : déclarer dans `arena.h` et définir dans `arena.cpp` —
`arena.h` est inclus par `duel.h`, donc visible de tout le chemin chaud.

**Rends le profil sur les deux régimes**, ils n'ont pas le même profil :
les TIRAGES (`RunNrpa`, ~10⁷ décisions par run) et le FINISSEUR (`RunLevin`, des
expansions qui développent tous les fils). Optimiser l'un sans regarder l'autre,
c'est optimiser à moitié. **Et l'écart entre les deux est déjà mesurable
aujourd'hui, il est énorme, et il n'est pas expliqué** — c'est la première chose
que le profil doit trancher :

| régime | mesure brute (session 10) | coût unitaire déduit |
|---|---|---|
| tirages, étalon A | 12,2 M tirages / **393 M états** en 1 246 s sur 16 workers | **~51 µs par état** |
| finisseur, étalon A | 10 900 à 17 500 expansions en 52 s, par racine | **~3 à 5 ms par expansion** |

Un facteur ~70 sépare les deux. Il s'explique *peut-être* entièrement par le
facteur de branchement — une expansion développe tous les fils, et à b ≈ 20 les
~4 ms retombent sur ~200 µs par fils, ce qui recolle avec l'estimation de 250 µs
du §9.16. **Mais personne ne l'a vérifié**, et si l'explication est ailleurs
(requêtes de zone refaites par fils, digest recalculé, restauration par fils au
lieu d'une par expansion), c'est là que se trouve le gain le plus gros de tout ce
chantier. Commence par là.

---

## ÉTAPE 2 : les cibles, dans l'ordre où le profil les désignera probablement

Ce sont des hypothèses. **Le profil décide de l'ordre, pas cette liste.**

### (a) Ce que l'audit avait déjà nommé (section 6, C19–C28)

Aucun n'est fait, sauf le gating de la nouveauté — fait en session 9 parce qu'il
corrigeait aussi `--novelty 0`, et **mesuré à +20 % d'expansions à budget égal**.
C'est le seul point de comparaison disponible sur ce que rend ce genre de travail.

- **C19 — jeux de drapeaux de requête séparés.** Chaque `Query` traverse le core
  et sérialise un flux qu'il faut ensuite parser. Demander `QUERY_CODE|QUERY_ALIAS`
  là où seul le code sert, c'est payer le reste pour rien.
- **C20 — `EntryOf` sur la pile**, **C21 — hachage 8 octets**, **C22 —
  `CommonCodes` calculé une fois**, **C23 — `LNode` en ligne**, **C24 — `path` en
  pile de tampons**. Tous de la même famille : allocations et copies dans une
  boucle qui tourne des millions de fois.
- Cible concrète et vérifiable tout de suite : `Search::RecipeDistance` appelle
  `duel.Query(con, loc, flags)` — la surcharge qui **retourne un `std::vector`**,
  donc alloue — cinq fois par nœud développé, alors que la surcharge à tampon
  réutilisable (`Query(con, loc, flags, out)`) existe déjà juste à côté.
  Cherche les autres sites du même motif : c'est le gain le plus bête à prendre.

### (b) Les allocations, et le fait qu'elles passent par l'ARÈNE

Attention, c'est la particularité du montage et elle se retourne contre l'intuition
habituelle : le heap du duel (C++ **et** Lua) vit dans l'arène, et **toute
allocation neuve salit une page**, donc renchérit la restauration suivante. Le
jalon 0 l'a mesuré : arrêter le GC Lua n'a rien donné *parce que les pages sales
viennent des allocations neuves, pas du marquage*.

Conséquence directe : **réduire les allocations faites pendant un pas de moteur
réduit deux coûts à la fois** — l'allocation et les pages à restaurer. C'est la
piste avec le meilleur rapport, et c'est aussi la plus facile à vérifier : le
compteur de pages touchées existe déjà, il suffit de le lire avant/après.

### (c) La compilation, qui est gratuite et qu'on n'a jamais essayée

`premake5.lua` construit tout en `optimize "Speed"` (/O2), sans plus. Trois
leviers, tous à mesurer et à garder seulement s'ils tiennent le contrôle de
correction :

- **LTO** (`/GL` + `/LTCG`) — le core, Lua et le solveur sont trois bibliothèques
  statiques distinctes (`solver_ocgcore`, `solver_lua`, `solver_lzma`), donc
  l'inlining s'arrête aujourd'hui à leurs frontières, y compris sur
  `OCG_DuelProcess` et sur l'allocateur d'arène branché dans Lua — c'est-à-dire
  exactement sur le chemin chaud. C'est la configuration où LTO rend le plus.
  Côté premake : `flags { "LinkTimeOptimization" }` dans le filtre Release, puis
  **régénérer la solution** (la commande est dans le README) — un simple MSBuild
  ne suffira pas.
- **PGO** — le profil d'exécution de ce programme est extrêmement stable (le même
  chemin chaud, des millions de fois). C'est le cas d'école.
- **`/arch:AVX2`** — à ne tenter qu'après les deux autres, et à vérifier : le
  gain sera probablement nul (le chemin chaud est du pointeur et du branchement,
  pas du calcul), et il rend le binaire non portable.

Ordre de travail : mesurer LTO seul, puis PGO seul, puis les deux. Trois runs de
chaque, médianes.

### (d) Ce qu'il ne faut PAS ouvrir sans une raison mesurée

- **Le digest et la table de transposition.** Un digest plus rapide qui perd un
  bit de discrimination fait disparaître des solutions **en silence** — le mode de
  défaillance n°1 du projet (README, « le sous-hachage du digest »). Si tu y
  touches, le contrôle de correction est obligatoire ET la ligne de référence doit
  garder ses 273 digests deux à deux distincts.
- **Le schéma de l'arène.** Il est mesuré, il marche, et l'alternative
  (`PAGE_READONLY` + VEH) a été écartée pour une raison qui n'a pas bougé :
  interaction risquée avec le `longjmp`/SEH de Lua (§5.2).
- **Le parallélisme.** Seize workers tournent déjà. Ajouter des threads sur une
  machine à seize cœurs ne fera que déplacer la contention — et la variance de
  graine domine déjà tout (piège 39).

---

## Le montage de mesure

- **Santé** — la porte, avant et après : voir la commande dans
  `docs/next-session-prompt.md`. Ici, le diff attendu n'est PAS « seulement des
  durées » : c'est « exactement les mêmes nombres, avec des durées plus basses ».
- **Étalon 0 déterministe** (`tools/s9_ab_alpha.ps1`) : racines imposées
  (`--approach`), politique vide (`--no-nrpa`). C'est le seul montage du dépôt où
  deux bras se comparent sans bruit de graine — utilise-le pour la correction.
- **Débit des tirages** : un run de tirages purs sur l'étalon B, budget en temps,
  et on lit `tirages` et `etats`. Trois répétitions.

- **Le run le moins cher qui dise quelque chose** : `--solve-ms 5000` sur l'étalon
  A. Le chargement, la lecture du deck, le comptage dérivé et l'amorce du graphe
  s'impriment AVANT toute recherche — c'est cinq secondes pour vérifier qu'un
  binaire n'est pas cassé, contre trente minutes pour un run complet. Utilise-le
  comme test de fumée après chaque changement de perf.

Les ordres de grandeur de départ sont dans le tableau de l'étape 1. Ils viennent
de logs, pas d'un instrument dédié : **ils sont à re-mesurer proprement**, et
c'est précisément l'objet de l'étape 1.

Deux chiffres de cadrage supplémentaires, pour dimensionner les attentes : le run
de santé complet coûte **60 s** et un bras d'A/B sur l'étalon A en régime
déterministe (`--no-nrpa` + `--approach`) coûte **~90 s**. Ce sont eux qu'on
répète le plus souvent — un gain qui ne se voit pas sur ces deux-là ne changera
pas le rythme de travail, même s'il est réel ailleurs.

---

## Définition de « terminé »

(a) Le profil existe, il est imprimé, il s'éteint, et son propre coût est chiffré.
(b) Les deux régimes (tirages, finisseur) ont leur décomposition, somme vérifiée
avec une ligne « reste ». (c) Au moins un gain mesuré à temps égal ET vérifié à
budget de nœuds égal, avec ses trois répétitions et sa médiane. (d) Les
optimisations tentées et PERDANTES sont documentées avec leur chiffre — c'est la
règle du dépôt et c'est ce qui empêche de les retenter. (e) §9.18 documenté, ce
prompt régénéré.

**Une dernière chose.** Si le profil montre que le temps part majoritairement dans
`OCG_DuelProcess` lui-même — c'est-à-dire dans le code d'ocgcore et les scripts
Lua — alors le levier n'est plus dans notre code mais dans le **nombre d'appels**,
et la session doit le dire clairement plutôt que de gratter des pourcentages en
périphérie. Ce serait un résultat, pas un échec : il renverrait au vrai levier
identifié par la revue, les OPTIONS (chantier 17), qui divisent le nombre de
décisions au lieu d'en accélérer chacune.
