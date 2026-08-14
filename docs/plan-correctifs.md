# Plan de correctifs — issu de l'audit de fin de session 8

Chaque point porte une **provenance** : `[vérifié]` = j'ai lu le code et confirmé
moi-même ; `[rapporté]` = remonté par l'audit, plausible, non revérifié ligne à
ligne — à confirmer avant d'agir. La distinction n'est pas cosmétique : deux
points `[vérifié]` invalident des conclusions déjà écrites, et il serait
malhonnête de leur donner le même statut qu'à une observation non recoupée.

Chaque chantier porte aussi sa **vérification** — comment on saura qu'il est
fait — parce qu'un correctif sans mesure de contrôle est un pari.

Ordre : les correctifs qui **invalident des mesures** d'abord ; puis ceux qui
**rendent des mesures lisibles** ; puis la vitesse. Aucune optimisation avant
que les instruments soient fiables : accélérer une recherche dont on lit mal les
résultats, c'est produire plus vite des chiffres douteux.

---

## A. Ce qui invalide des conclusions publiées — à faire avant toute nouvelle mesure

### C1. La partition par jetons de `DescendTransplant` s'auto-verrouille `[vérifié]`

`main.cpp:5637` dimensionne `claims(plan.size() + 1)` ; `search.cpp:1245-1250`
réclame une case par **hachage du digest d'état**, et **aucune case n'est jamais
relâchée**. `search.cpp:1257` (`if(c.cost > 0 && !mine) continue;`) supprime
alors toute déviation chez les workers qui n'ont pas le jeton.

Deux régimes, tous deux cassés :

- *avec plan* : après ~`plan.size()` états distincts vus au niveau de
  réclamation, tous les jetons valent 1, `mine` est faux partout, et la passe ne
  suit plus que le répertoire — sans qu'aucun compteur ne le dise ;
- *en mode BUT SEUL* : `plan` est vide, donc **`claims_size = 1`**. Un jeton
  pour seize workers. À k = 1, `claim_level = 0` : le premier worker à toucher
  la racine le prend, les quinze autres ne peuvent dévier nulle part, et le
  gagnant lui-même est bloqué dès son deuxième nœud.

**Conséquence documentaire, déjà traitée** : les « 26 états à tous les niveaux
d'écart » du §9.15, dont on avait tiré une affirmation structurelle sur le mode
but seul, sont intégralement expliqués par ce défaut. La conclusion est
rétractée dans le §9.15 ; la mesure reste à refaire.

**Correction.** Réclamer par **indice de plan**, comme le fait déjà `RunRepair`
(`search.cpp:1030`), qui est une vraie partition — un point de la ligne, un
jeton. À défaut, dimensionner `claims` sur le nombre réel de points de
réclamation et relâcher la case au retour de la récursion. Et dans tous les
cas : **compter les branches supprimées** par `!mine` et l'imprimer, sans quoi
le prochain verrouillage sera aussi invisible que celui-ci.

**Vérification.** L'étalon B en mode but seul doit rendre un nombre d'états qui
CROÎT avec le niveau d'écart (aujourd'hui : 26, constant). Santé identique avant
/ après, puisque le flux réparation n'est pas touché.

### C2. « ÉPUISÉ » n'est pas une preuve d'absence `[vérifié : `edges_skipped` n'a qu'UNE occurrence dans tout le dépôt, sa déclaration]`

Six sites coupent sur le plafond de profondeur ou d'actions
(`search.cpp:431, 855, 966, 1153, 2087` et voisins) **sans rien incrémenter**.
Puis `stats.exhausted = pq.empty() && !hit_time_limit && !hit_node_limit`. Une
recherche dont toutes les branches ont été coupées par le plafond afficherait
donc ÉPUISÉ.

C'est le mot sur lequel repose la charge de preuve de trois sections : §9.10
(« un recul ÉPUISÉ est une preuve d'absence par racine »), §9.11 (« k=2, k=3,
k=4 ÉPUISÉ ⇒ aucune ligne sous 19 brûlées à ≤ 4 déviations »), §9.15 (le
contrôle de correction à 42 expansions de cette session).

**Correction, la moins chère du plan.** Le champ `SearchStats::edges_skipped`
(`search.h:787`) existe, n'est **jamais écrit ni lu** : l'incrémenter aux six
sites, l'imprimer, et refuser d'écrire « ÉPUISÉ » quand il est non nul —
écrire « ÉPUISÉ SOUS BORNE » à la place.

**Vérification.** Le contrôle `recul 0` de l'A/B déterministe doit rester à 42
expansions et afficher `edges_skipped = 0` (sinon la preuve de correction de
cette session tombe elle aussi). Les racines profondes, elles, afficheront
probablement un compte non nul — et c'est l'information recherchée.

### C3. `h_root` ne réalise pas l'Eq. 7 de l'article `[vérifié]`

`search.cpp:1900` : `h_root = |cible| + Σ resolve_min`, une constante du
PROBLÈME, avec le commentaire « soit exactement h au premier nœud ». Faux dans
le finisseur : le premier nœud est l'état atteint après rejeu d'un préfixe,
typiquement à 7/8 cartes, donc h ≈ 1. L'article normalise par h à la **racine de
la recherche courante**. L'échelle effective est donc α/8 sur l'étalon B.

**Correction.** Évaluer `h_root` au nœud 0 de la recherche (le même `hgoal` que
les autres nœuds), avec un plancher à 1. Puis **refaire le cadran α** : les
valeurs 8/15/25/40/60 ne testent pas ce qu'on croyait.

**Vérification.** Rejouer l'A/B déterministe (`tools/s8_ab_finisseur.ps1`) et
retrouver — ou non — la monotonie. La lecture qualitative doit survivre ; c'est
l'échelle qui bouge.

---

## B. Instruments aveugles — trois `printf` pour trois mécanismes jamais chiffrés

Même piège que `reroots` cette session (piège 52), et la correction de `reroots`
n'est elle-même faite **qu'au tiers** : elle ne s'imprime que dans la table des
racines d'`--approach` (`main.cpp:5110`), pas dans la table LTS principale
(`main.cpp:5456`) ni en phase A2. Donc **en mode but seul sans `--approach` — le
mode que cette session vient de construire — l'instrument du rerooter ne
s'imprime jamais.** À corriger en même temps que le reste. `[vérifié]`

### C4. `constraint_cuts`, `guard_cuts`, `turn_cuts` `[vérifié]`

Vérification faite : `constraint_cuts` a **six incréments dans `search.cpp` et
zéro occurrence dans `main.cpp`** ; `guard_cuts` a un incrément
(`search.cpp:686`) et zéro occurrence dans `main.cpp` ; `turn_cuts` est agrégé
dans `ModeStats` (`main.cpp:4631`, champ déclaré ligne 4532) et **aucun `printf`
ne le consomme** — agrégé exprès, puis abandonné.
Ce sont les compteurs des trois mécanismes d'élagage actifs dans **tous** les
runs disciplinés depuis la session 3 :

- `guard_cuts` dirait si la garde élague utilement ou si elle rase l'espace —
  la question exacte laissée ouverte par le §9.11 sur la bande précoce murée ;
- `constraint_cuts` dirait combien de tirages `--summon-min`/`--material` tuent ;
- `turn_cuts` sépare « le budget de décisions est trop court » de « la ligne
  déborde du tour 1 ».

**Correction.** Trois colonnes sur la ligne de phase, à côté de
`coupures nouveaute`. Coût : une soirée, dont l'essentiel est le formatage.

### C5. Statut de fin de recherche incomplet `[rapporté]`

`main.cpp:5117` et `5462` : `exhausted ? "EPUISE" : hit_time_limit ? "budget" : ""`.
Quand la limite de nœuds ou le garde-fou mémoire de 4 M nœuds
(`search.cpp:1995`) mord, la colonne est **vide** — une racine tronquée par la
mémoire est typographiquement indiscernable d'une racine normale. Ajouter
`"noeuds"` et `"memoire"`.

### C6. `dead_ends` invisible dans le flux solve `[rapporté]`

Imprimé uniquement en mode `--width`. Or c'est le symptôme n°1 du jeu de scripts
décalé — la défaillance silencieuse que ce dépôt redoute le plus. Le compteur
qui la détecterait est muet exactement là où elle se produirait.

### C7. `ArenaStats::host_fallbacks` est structurellement toujours nul `[rapporté]`

`t_host_fallbacks` est `thread_local` (`arena.cpp:60`) et `Arena::Stats()` n'est
appelé que depuis le thread principal (`main.cpp:6140`). Les allocations hors
arène se produisent dans les **workers**. Le rapport imprime donc
« aucune : tout l'état est capturé » par construction.

C'est doublement grave, car un repli hors arène **corrompt silencieusement** le
duel : `arena.Restore()` ne peut pas restaurer ces objets, donc tout ce que le
worker mesure ensuite porte sur un état divergent. **Correction** : compteur
atomique membre d'`Arena`, plus un drapeau `poisoned` collant qui fait avorter
le worker bruyamment plutôt que de le laisser produire des chiffres faux.

---

## C. Défaillances silencieuses hors instruments

### C8. Douze workers peuvent avorter en silence `[rapporté]`

`if(!arena.Init(..., err)) return;` et `if(!d.Create(..., err) || !d.Setup(...)) return;`
avec `err` jamais imprimé, sur douze sites. Sous pression d'espace d'adressage
(16 workers × `--arena-mb`), **un bras d'A/B qui n'a jamais tourné est
indiscernable d'un bras qui a tourné et n'a rien trouvé** : la passe affiche
`0 solutions, 0 etats`. `BuildPriorPolicy` imprime déjà dans ce cas : c'est donc
une incohérence, pas une politique. Correction mécanique.

### C9. `ForEachSubset` n'émet que les plus petits sous-ensembles `[rapporté]`

`enumerate.cpp:70-105` : le commentaire annonce « on privilégie les tailles
extrêmes », le code énumère par taille **croissante** et s'arrête au plafond.
Avec `cap = 24` et 24 candidats, les 24 émissions sont les 24 **singletons** —
aucune paire, jamais. Sur `MSG_SELECT_SUM` (somme de niveaux, tributs), une
sélection d'une carte ne satisfait presque jamais la contrainte : le prompt
devient stérile sans que rien ne le dise.

Si c'est confirmé, c'est un **trou de complétude** qui affaiblit toutes les
preuves d'absence portant sur des invocations Synchro/tribut. Correction :
alterner `k` depuis les deux bouts, et incrémenter `edges_skipped` (C2) quand le
plafond mord. À confirmer en premier par un test ciblé sur un prompt de somme.

### C10. `ResponseForbidden` échoue OUVERT `[rapporté]`

`enumerate.cpp:688-721` : tous les chemins d'échec rendent `false` = *autorisé*,
sur une disposition de message ocgcore codée en dur (`strides[5]`). Si le core
décale un champ, `--no-activate` et `--no-chain` **cessent de filtrer** en
silence. Correction : tri-état, `Undecodable` fatal.

### C11. `AdaptCorpus` n'applique pas la distribution qu'il prétend appliquer `[rapporté]`

`search.cpp:1667-1673` passe sept arguments pour huit paramètres : `hint_bias`
forcé à `0.0f` et `temp` laissé à son défaut `1.0f`. Deux commentaires du dépôt
sont donc démentis par le code lui-même — `search.h:420-426` (« EXACTEMENT la
même mise à jour ») et `search.h:597-605` (« l'adaptation utilise la MÊME
température »). Conséquence : `--adapt` combiné à `--nrpa-temp` est incohérent,
et le gradient du corpus est calculé sous une distribution qui diffère de la
distribution tirée **précisément sur les coups indicés**, c'est-à-dire les rips.
Correction mécanique (passer les deux paramètres), mais elle **invalide
potentiellement les mesures de `--adapt` des sessions 7 et 7bis** : à re-mesurer.

### C12. `--no-ref` laisse la référence gouverner le plafond de décisions `[vérifié]`

`main.cpp:4337` dérive `cfg.max_decisions = ref_decisions * 3/2 + 32` même sous
`--no-ref`. Le mode « sans référence » est calibré par la référence. Dans les
runs de cette session `--max-decisions 700` couvrait le défaut, mais la promesse
du drapeau est fausse sans lui. Correction : sous `--no-ref`, exiger
`--max-decisions` ou dériver le plafond de la decklist, et l'imprimer.

### C13. Saturations silencieuses `[rapporté]`

`search.cpp:730-737` (score d'archive : `rp` clampé à 15, `overlap` à 255),
`search.cpp:1305/1478` (histogramme `resolve_reached` clampé à 4),
`search.h:302-306` (`ContextKey` clampe à 15, ce qui borne aussi la
segmentation lue par `ForecastSearchCost` — les deux nombres qui ont décidé
d'écrire sqrt-LTS). Aucune n'avertit. Correction : vérifier au parse, comme
c'est déjà fait pour le plafond de 4 entrées `--resolve` (`main.cpp:891`).

### C14. `LiftPlan` jette sa valeur de retour dans le chemin `--fire` `[rapporté]`

`main.cpp:3411`. Les trois autres sites capturent et impriment le nombre
d'étapes non identifiées. Ici un plan à 90 % de trous est utilisé comme s'il
était complet, et l'échec des fenêtres est mis sur le compte de la recherche.

---

## D. Constantes à exposer et à mesurer

### C15. `nrpa_level = (budget > 180000.0) ? 3 : 2` `[vérifié]`

`main.cpp:4587`. Ce seuil change le coût d'un appel de niveau de `iters²` ≈ 576
tirages à `iters³` ≈ 13 824 — **l'algorithme d'échantillonnage lui-même**. Aucune
mesure ne l'adosse. Et il tombe exactement sur la ligne de partage des commandes
comparées aux sessions 5-7 : un run de 600 s sans `--finisher-min` donne 420 s
de tirages → niveau 3 ; le **même** run avec `--finisher-min 420000` donne
~180 s → niveau 2. C'est une variable cachée dans plusieurs A/B publiés.

**Correction.** `--nrpa-level <n>` explicite, valeur imprimée dans l'en-tête de
phase, et un A/B des deux valeurs à budget égal. C'est peut-être un gain gratuit.

### C16. `max_subsets = 24` écrit en dur douze fois `[rapporté]`

Le défaut de la structure (64) n'est jamais utilisé, aucun drapeau, aucune
mesure — alors que c'est ce qui plafonne le facteur de branchement de tous les
prompts de sélection. À exposer (`--max-subsets`) et à mesurer, en même temps
que C9 dont il est le paramètre.

### C17. Le partage du budget entre phases `[rapporté]`

Sept constantes au jugé (`0.7` aux tirages, `0.2`/`0.75` aux deux phases
d'approche, `0.8` et plafond `240000`, `/4` et plafond `20000`, `0.4`/`0.6` en
réparation). Ce sont elles qui décident du volume relatif des trois passes dont
le §9.11 compare les rendements. Au minimum : les imprimer.

### C18. `hint_bias = 2.0f` sans drapeau `[rapporté]`

`search.h:572`. C'est le canal par lequel la connaissance du joueur entre dans
l'échantillonnage, et les cartes `--resolve` le reçoivent d'office. Son voisin
`nrpa_bias_known` a `--nrpa-bias` ; celui-ci n'a rien. À exposer.

---

## E. Optimisations — seulement après B et C

**Calibration du chemin chaud**, relevée dans les logs : ~4 000 états/s/worker,
`arena.Restore()` = 0,36 ms pour 2,36 Mo. Une décision coûte ~250 µs, dominée
par `duel.Process()` et par `Restore()`. **Règle de priorité qui découle de la
mesure** : tout ce qui supprime une requête au core bat tout ce qui supprime un
`malloc`, parce que les requêtes allouent *dans* l'arène, salissent des pages,
et sont donc repayées par le `Restore()` suivant.

| # | Optimisation | Où | Gain attendu |
|---|---|---|---|
| C19 | Supprimer `CollectAtoms` + `Observe` du tirage NRPA quand la nouveauté est éteinte — et corriger `--novelty 0`, qui aujourd'hui ne l'éteint pas côté NRPA | `search.cpp:1524-1546` | le plus gros bloc supprimable du chemin NRPA ; supprime aussi la principale allocation NON BORNÉE du run (`seen` croît sans limite, par worker) |
| C20 | Deux jeux de drapeaux de requête au lieu d'un : `QUERY_OVERLAY_CARD\|COUNTERS\|LINK` n'a de sens que pour le terrain, pas pour HAND/GRAVE/REMOVED/EXTRA | `search.cpp:29-30` | `StateDigest` fait 12 requêtes par nœud ; chaque octet sérialisé en trop est une page salie, donc du `Restore()` payé |
| C21 | `EntryOf` : tampon de pile + tri par insertion au lieu de deux `std::vector` construits et triés **par carte, par requête** | `search.cpp:46-56` | appelé ~40 fois par `StateDigest` et une fois par carte par board key |
| C22 | Hacher 8 octets à la fois dans `StateDigest` au lieu d'octet par octet | `search.cpp:383-389` | la fonction la plus appelée du dépôt ; ~×8 sur le nombre de mélanges |
| C23 | Calculer `CommonCodes` **une fois** par décision au lieu de 4-5 (`GoalCheck`, `ArchiveObserve`, partition de nouveauté, `Heuristic`, `ctx`) ; idem pour le compte de cimetière, demandé deux fois au core | `search.cpp:474-1568` | supprime des allers-retours au core sur le chemin le plus chaud |
| C24 | `LNode` : réponse en ligne (`uint8_t resp[12]`) au lieu d'un `std::vector` par arête | `search.cpp:1875` | un `malloc` par arête générée, et 150-250 Mo par worker au plafond de 4 M nœuds |
| C25 | `path` : pile de tampons réutilisés (`assign` garde la capacité) au lieu de `vector<vector<uint8_t>>` | `search.h:1087` | un `malloc`+`free` par décision, sur cinq boucles chaudes **et** par décision rejouée dans les re-descentes de Levin |
| C26 | `GoalCheck` : hacher `path` avant de le copier, matérialiser seulement si la solution est retenue | `search.cpp:556` | en mode anytime, une politique convergée ré-atteint la cible presque à chaque décision ; chaque ré-atteinte copie ~150 vecteurs pour les jeter |
| C27 | `PolicyStep` : réutiliser les tampons au lieu de trois allocations par décision — dont deux copies de `NrpaRun` **sous le mutex partagé** | `search.cpp:1559-1573, 1737-1767` | ~450 `malloc`/`free` par tirage, et le travail d'allocateur sérialise les workers |
| C28 | Conteneurs : `no_activate` en vecteur plat, `hint_cards` idem, `try_emplace` au lieu de `count`+`emplace` (cinq sites), pile de plongée indexée | divers | chacun petit, tous sur des chemins par-choix ou par-nœud |

---

## Les cinq à faire en premier, et pourquoi

1. **C1 — la partition par jetons.** C'est le seul point qui invalide une
   conclusion publiée. Tant qu'il n'est pas corrigé, **aucune mesure du mode but
   seul n'est interprétable**, y compris celles de cette session.
2. **C2 — compter les coupures de plafond avant d'écrire « ÉPUISÉ ».** Trois
   sections du §9 tirent des preuves d'absence de ce mot. Le champ existe déjà
   et n'a jamais servi : c'est le meilleur rapport information/effort du plan.
3. **C3 — `h_root`.** Le cadran α de cette session ne teste pas ce qu'il
   annonce ; la correction est de deux lignes et le re-test est déjà scripté.
4. **C4 + le tiers manquant de `reroots`.** Trois mécanismes d'élagage utilisés
   dans tous les runs depuis la session 3, jamais chiffrés — et l'instrument
   qu'on vient d'ajouter est muet précisément dans le mode qu'on vient de créer.
5. **C15 — `nrpa_level`.** Une constante non mesurée qui change l'algorithme et
   qui sépare exactement les deux commandes comparées dans plusieurs A/B
   antérieurs. À exposer et à mesurer avant de comparer quoi que ce soit d'autre.

Rien de la section E avant que ces cinq points soient faits.
