# Combo Solver — analyse de conception

Feature : à partir d'un `.yrpX`, explorer l'espace des lignes de jeu possibles et produire un
répertoire de `.yrpX` qui atteignent **le même board final** avec **moins de ressources et moins
d'étapes** que la ligne d'origine.

Document d'analyse préalable — aucune ligne de code écrite. Objectif : établir la solution la plus
performante et la plus élégante avant de patcher ocgcore.

---

## 1. Contrat de la feature (arbitré)

| Question | Décision |
|---|---|
| Équivalence du board final | Mêmes cartes sur le terrain par **type de zone** (MZONE / EMZ / SZONE / Pendule), même **face** (recto/verso), mêmes **matériaux** (Xyz/Fusion/Link overlay), mêmes **compteurs**. La colonne exacte est ignorée, et la **position de combat ATK/DEF aussi** (arbitrage du joueur, session 4 — le digest d'ÉTAT garde, lui, la position complète). |
| Fonction de coût | Lexicographique : **(1)** cartes consommées, **(2)** nombre d'activations/invocations, **(3)** nombre total de décisions. |
| Périmètre | Du début du duel à la fin du tour 1 du joueur ciblé. L'adversaire ne répond jamais. |
| Exploration | Complète (toutes les décisions offertes par le core) + **branch-and-bound** borné par la ligne de référence. |
| Sortie | Répertoire de `.yrpX` uniquement, top-N. |

---

## 2. Ce que dit le core (constats vérifiés)

Références dans le clone `edopro/`.

### 2.1 Le bootstrap d'un duel est déjà écrit et réutilisable

[old_replay_mode.cpp:108-166](../edopro/gframe/old_replay_mode.cpp#L108) fait exactement ce dont le
solveur a besoin : lecture du seed Xoshiro256 depuis l'en-tête étendu, `OCG_CreateDuel` avec
`duel_flags` / `start_lp` / `start_hand` / `draw_count`, puis `OCG_DuelNewCard` pour les rule cards,
le main deck et l'extra deck de chaque joueur. Rien de tout ça ne dépend du rendu — c'est
transposable tel quel dans un binaire headless.

### 2.2 Les décisions du joueur ne sont pas dans le flux yrpX

Un `.yrpX` contient le **flux de messages** (`ReplayStream`), pas les réponses. Les réponses sont
dans le `yrp1` embarqué, chargé en [replay.cpp:287](../edopro/gframe/replay.cpp#L287)
(`yrp->OpenReplayFromBuffer`). Conséquence directe : **le solveur exige un replay exportable**
(`Replay::HasPlayableYrp()`). C'est le cas des replays EDOPro standards ; il faut un message d'erreur
explicite sinon, parce que sans les réponses on ne peut ni rejouer la ligne de référence ni établir
le board cible.

### 2.3 Le point de décision est parfaitement identifiable

[processor_visit.cpp:9-31](../edopro/ocgcore/processor_visit.cpp#L9) : `field::process()` visite le
`std::variant` en tête de `core.units` et renvoie `OCG_DUEL_STATUS_AWAITING` **si et seulement si**
le `Process<NA>` en cours a `needs_answer == true`. Les 18 types de prompt sont énumérables
statiquement depuis `processor_unit.h`, et les messages correspondants sont émis par `playerop.cpp` :

```
MSG_SELECT_IDLECMD  MSG_SELECT_BATTLECMD  MSG_SELECT_CHAIN     MSG_SELECT_CARD
MSG_SELECT_UNSELECT_CARD  MSG_SELECT_SUM   MSG_SELECT_TRIBUTE   MSG_SELECT_PLACE
MSG_SELECT_POSITION MSG_SELECT_COUNTER    MSG_SELECT_OPTION    MSG_SELECT_EFFECTYN
MSG_SELECT_YESNO    MSG_SORT_CARD         MSG_ANNOUNCE_CARD    MSG_ANNOUNCE_NUMBER
MSG_ANNOUNCE_ATTRIB MSG_ANNOUNCE_RACE
```

Chaque message porte sa propre liste de choix légaux (ex. [playerop.cpp:306-317](../edopro/ocgcore/playerop.cpp#L306)
pour `MSG_SELECT_CARD` : `min`, `max`, puis `n` triplets code/location). **L'énumérateur de branches
se lit donc directement dans le message** — pas besoin de dupliquer la logique de légalité du core.

### 2.4 Il n'existe aucun moyen de sauvegarder l'état d'un duel

L'API publique ([ocgcore_functions.inl](../edopro/gframe/ocgcore_functions.inl)) n'a ni clone ni
snapshot. Et un sérialiseur écrit à la main est hors de question :

- `processor` ([field.h:196-276](../edopro/ocgcore/field.h#L196)) contient ~60 conteneurs STL de
  pointeurs bruts (`chain_list`, `card_set`, `event_list`, `effect_indexer`, `delayed_effect_collection`…),
  imbriqués et croisés.
- Surtout : **des coroutines Lua sont suspendues au moment des prompts**.
  [interpreter.cpp:504-549](../edopro/ocgcore/interpreter.cpp#L504) — `call_coroutine` crée un
  `lua_newthread` par fonction d'effet et renvoie `COROUTINE_YIELD` ; le thread reste vivant, sa pile
  d'exécution intacte, jusqu'à la réponse du joueur. Sérialiser une pile de coroutine Lua avec ses
  upvalues et ses closures, c'est réimplémenter Pluto/Eris.

C'est le constat central : **le snapshot ne peut pas être sémantique, il doit être mémoire.**

### 2.5 Le core est propre pour ce qu'on veut en faire

- **Aucun état global mutable** (grep sur `static` non-const / `thread_local` : rien). Tout est
  suspendu au `duel*`.
- `card_sort` compare `cardid` ([card.cpp:19](../edopro/ocgcore/card.cpp#L19)), pas des pointeurs →
  l'ordre des `card_set` est indépendant de l'adressage.
- `ocgcore` a déjà une cible `StaticLib` ([premake5.lua](../edopro/ocgcore/premake5.lua)) avec
  `staticruntime "on"` → linkable statiquement dans notre binaire.
- Lua **5.4.7** bundlé en source, avec un `luaconf-customize.h` déjà prévu pour les surcharges, et
  `luai_makeseed` protégé par `#if !defined(luai_makeseed)` ([lstate.c:58](../edopro/ocgcore/lua/src/lstate.c#L58)).
- `luaL_newstate()` ([interpreter.cpp:32](../edopro/ocgcore/interpreter.cpp#L32)) utilise l'allocateur
  par défaut → remplaçable par `lua_newstate(alloc, ud)` en une ligne.

---

## 3. Le vrai goulot n'est pas la vitesse

**Mesuré** sur `synchron handrip 2.yrpX` (jalon 0, rejeu fidèle : 0 `MSG_RETRY`, 290/290 réponses) :

| | |
|---|---|
| Points de décision sur la ligne | **291** |
| `SELECT_CHAIN` | 141 · branchement moyen 1,5 · max 4 |
| `SELECT_PLACE` | 35 · moyen 3,4 · max 7 |
| `SELECT_UNSELECT_CARD` | 29 · moyen 2,6 · max 5 |
| `SELECT_IDLECMD` | 28 · **moyen 13,0 · max 20** |
| `SELECT_POSITION` | 24 · moyen 2,0 |
| `SELECT_CARD` | 21 · moyen 4,6 · **max 35** |
| `SELECT_EFFECTYN` / `OPTION` / `YESNO` | 13 · moyen 2,0 |
| **Produit des branchements** | **10⁹⁷** (10⁹³ après dédup par code) |

Ce produit est un **indicateur d'échelle, pas un décompte rigoureux de feuilles** : changer un choix
précoce modifie les prompts suivants, donc les 291 facteurs ne se combinent pas librement. Il n'est
ni une borne haute ni une borne basse de la taille exacte de l'arbre. Ce qu'il établit en revanche
sans ambiguïté, c'est l'ordre de grandeur du régime dans lequel on se trouve.

Le seul énoncé qui compte est donc qualitatif, et il est désormais adossé à une mesure :
**l'arbre d'actions n'est énumérable à aucune vitesse.** C'est pourquoi il faut chercher sur le
**graphe d'états** et non sur l'arbre d'actions — activer A puis B et activer B puis A convergent sur
le même nœud.

Second enseignement de la mesure, plus dérangeant : les 28 `SELECT_IDLECMD` branchent à 13 en
moyenne. Même le niveau **macro** seul vaut ~13²⁸ ≈ 10³¹. L'espoir ne peut donc pas venir d'un
élagage marginal : il repose entièrement sur le taux de convergence du graphe d'états, qui reste
non mesuré (§3.0).

Le patch ocgcore sert donc à une chose précise, et à une seule : **rendre le coût d'une transition
d'état négligeable**. Il ne réduit pas le nombre de nœuds. Les deux leviers sont orthogonaux et tous
les deux nécessaires.

### 3.0 La quantité qui décide de tout — MESURE INVALIDÉE, À REFAIRE

> ⚠️ **Les chiffres de cette section sont faux et conservés pour mémoire.** Ils ont été mesurés avec
> un digest d'état incomplet, qui fusionne des états distincts. Contrôle a posteriori : sur les 290
> états de la ligne de référence, le digest n'en distingue que **280 — 10 fusions**, la première dès
> la décision **#27, confondue avec #24**. La recherche, arrivée au #27, y voit une transposition et
> élague : la branche du combo est coupée à son démarrage.
>
> Conséquences : les 0 solution et la « saturation » à 65 184 états sont des **artefacts**, pas des
> propriétés du problème. Le facteur de croissance ×1,25 a été mesuré sur un espace tronqué, et le
> plafond « 16–20 actions » qui en découlait **n'est pas établi**.
>
> C'est exactement le mode de défaillance annoncé au §4.2 : sous-hacher fait disparaître des
> solutions **sans rien signaler**. La leçon opérationnelle est que le contrôle de collision sur la
> ligne de référence (`combosolver --growth`, section « digest ») doit passer **avant** toute
> exploitation d'un chiffre de recherche.
>
> Ce que ça établit en revanche : le patch **C1** (exposer l'état du processeur — forme de
> `core.units`, `effect_count_code`, compteurs d'invocation) n'est pas un raffinement, c'est un
> **prérequis**. Les états confondus sont typiquement des fenêtres `MSG_SELECT_CHAIN` à zéro option
> au milieu d'une résolution : board identique, charge utile de prompt identique, pile de processeur
> différente.

### 3.0 bis Chiffres obtenus (non valides, conservés pour trace)

Le nombre d'**états distincts** atteignables est ce qui détermine si « exhaustif » est un mot
réaliste. Il ne se devine pas ; il se mesure. C'est fait (`combosolver --growth`), en développant
exhaustivement le graphe à profondeur croissante depuis le début du duel de référence :

| profondeur (décisions) | états | transpositions | durée | facteur |
|---|---|---|---|---|
| 10 | 508 | 98 | 45 ms | |
| 20 | 6 441 | 1 859 | 0,53 s | ×1,5 |
| 30 | 19 376 | 6 763 | 1,7 s | ×1,2 |
| 38 | 43 554 | 14 971 | 3,9 s | ×1,2 |

**Le facteur de croissance se stabilise autour de ×1,25 par palier de 2 décisions**, soit ~×1,12 par
décision. C'est spectaculairement bas au regard du branchement brut (13 en moyenne aux commandes
idle) : la table de transposition fusionne **34 %** des nœuds à profondeur 38. Le pari central de la
conception — chercher sur le graphe d'états et non sur l'arbre d'actions — est quantitativement
validé.

Mais l'extrapolation est sans appel :

| profondeur visée | états attendus | durée |
|---|---|---|
| 60 | ~500 k | ~45 s |
| 80 | ~4,7 M | ~7 min |
| 100 | ~44 M | ~1 h |
| **276** (la ligne de référence) | ~10²⁰ | hors de portée |

**Conclusion : l'optimisation exhaustive d'un combo de 59 actions est impossible.** La profondeur
exhaustive praticable est de l'ordre de **80 à 100 décisions, soit 16 à 20 actions** — un tiers du
combo de référence.

Ça ne condamne pas la feature, ça la recadre :

- l'exhaustivité reste acquise sur un **préfixe** de combo, ce qui suffit à répondre à « existe-t-il
  une meilleure amorce ? » ;
- au-delà, il faut une recherche **guidée vers le board cible** (best-first sur une heuristique de
  distance au board) plutôt qu'une énumération ;
- `--time-limit` n'est pas un garde-fou de confort : c'est le mode de fonctionnement normal, et le
  rapport doit toujours dire si l'espace a été épuisé ou tronqué.

### 3.1 Formalisation

- **État** `s` : l'état complet du duel à un point stable.
- **Point stable** : un prompt `MSG_SELECT_IDLECMD` ou `MSG_SELECT_BATTLECMD`. À ces instants la pile
  de coroutines est vide et l'état est sémantiquement descriptible.
- **Macro-action** `a` : la suite de micro-réponses menant d'un point stable au suivant (ex. « activer
  l'effet de la carte 3 » = choix idle + choix d'effet + choix de cible + choix de zone).
- **Coût d'arête** : `(activations, décisions)` — additif, positif.
- **Coût terminal** : `cartes consommées` = cartes qui ne sont ni en main, ni en deck, ni en extra à
  la fin. **Ce terme ne dépend que de l'état final**, pas du chemin.

Cette dernière propriété est la clé de l'algorithme : puisque le tier 1 du coût lexicographique est
une fonction de l'état terminal seul, pour chaque état intermédiaire il suffit de retenir **le
meilleur préfixe `(activations, décisions)`**, et le tier 1 se calcule au but. Le graphe est donc
explorable en recherche à coût uniforme classique, et le classement lexicographique final reste exact.

### 3.2 Bornes de branch-and-bound

On rejoue d'abord la ligne de référence pour obtenir `(C_ref, A_ref, D_ref)` et le board cible.
On élague ensuite toute branche telle que `activations ≥ A_ref` ou `décisions ≥ D_ref`. Ces deux
quantités sont **monotones croissantes** le long d'un chemin, donc l'élagage est *sound* : aucune
solution avec `A < A_ref` et `D < D_ref` n'est perdue.

Le tier 1 (ressources) n'admet **pas** de borne inférieure monotone triviale — une carte au cimetière
peut revenir en main. On n'élague donc pas dessus par défaut ; on énumère tous les états-but dans le
budget `(A_ref, D_ref)` et on classe. Un mode `--prune-resources heuristic` pourra ajouter une borne
optimiste (« une carte hors main/deck/extra et absente du board cible est perdue »), plus rapide mais
non exhaustif — à documenter comme tel.

**Limite assumée** : une ligne qui consommerait moins de ressources en *plus* d'étapes que la
référence est hors budget par construction. C'est conforme à la demande (« moins d'étapes **et** de
ressources »), et `--max-actions N` permettra de relâcher la borne.

---

## 4. Architecture du solveur

Recherche à deux niveaux — c'est ce qui rend la table de transposition fiable.

```
                 ┌─────────────────────────────────────────────┐
   NIVEAU MACRO  │ graphe d'états aux points stables           │
                 │ • table de transposition (digest sémantique)│
                 │ • coût uniforme sur (activations, décisions)│
                 │ • branch-and-bound sur (A_ref, D_ref)       │
                 │ • test de but : board == board cible        │
                 └────────────────┬────────────────────────────┘
                                  │ une macro-action
                 ┌────────────────▼────────────────────────────┐
   NIVEAU MICRO  │ DFS pur sur les prompts intermédiaires      │
                 │ • énumération lue dans le MSG_SELECT_*      │
                 │ • dédup par classes d'équivalence           │
                 │ • checkpoint/restore à chaque branche       │
                 │ • pas de transposition (état non descriptible)│
                 └─────────────────────────────────────────────┘
```

Au niveau micro on ne hache rien : l'état contient des coroutines suspendues. On y fait un DFS avec
restauration, et on ne dédoublonne qu'à la sortie, sur l'état macro atteint.

### 4.1 Classes d'équivalence des choix

Sans ça, `MSG_SELECT_CARD` avec `min=2, max=2` sur 8 cartes produit 28 branches dont la plupart sont
identiques. Réductions à appliquer avant expansion :

| Prompt | Réduction |
|---|---|
| `SELECT_CARD` / `SELECT_TRIBUTE` / `SELECT_SUM` | Dédup sur le **multiset des codes** + location. Deux copies du même code au même endroit sont interchangeables. |
| `SELECT_PLACE` | Une seule zone représentative par classe d'équivalence de flèches Link. Si aucun Link n'est en jeu et qu'aucun effet colonne-dépendant n'est actif, une seule zone libre suffit. |
| `SELECT_CHAIN` | Ordre de chaînage : ne garder que les ordres distinguables (les effets sans interaction commutent). |
| `SELECT_POSITION` | Toutes les positions offertes (elles font partie du board cible). |
| `SORT_CARD` | Non exploré — réponse par défaut. L'ordre du deck n'entre pas dans l'équivalence de board. |
| `ANNOUNCE_*` | Restreint aux valeurs présentes dans le deck / le board cible. |

C'est le levier de réduction le plus rentable après la transposition, et le plus risqué : chaque
réduction est une hypothèse. Toutes doivent être désactivables individuellement, et toutes sont
rattrapées par la vérification finale (§4.4).

### 4.2 Digest d'état (clé de transposition)

Doit capturer tout ce qui influence l'évolution future. `OCG_DuelQueryLocation` sur les 8 locations ×
2 joueurs donne les cartes (code, position, overlay, compteurs, statuts, équipements, ATK/DEF). Il
manque l'état du *processor*, qu'il faut exposer via un ajout d'API (§5.6, patch C) :

- tour, phase, joueur du tour ;
- `core.summon_count` / invocation normale déjà utilisée, `extra_p_count` ;
- `core.effect_count_code` (compteurs « once per turn » par effet) — indispensable ;
- LP des deux joueurs, `used_location` / `disabled_location` ;
- forme de la pile `core.units` (une empreinte des types de `Process` empilés).

**Sous-hacher est sans danger, sur-hacher coûte cher.** Un digest incomplet fusionne à tort deux états
distincts et produit un candidat invalide — qui sera rejeté à la vérification. Un digest trop précis
n'a aucun effet sur la correction, juste sur le taux de collapse. On commence donc large et on
resserre en mesurant.

### 4.3 Réponses de l'adversaire

Auto-pass systématique pour le joueur 1 : `SELECT_CHAIN` → aucun, `SELECT_EFFECTYN`/`SELECT_YESNO` →
non, et un `PLACE`/`POSITION` par défaut si le core le lui demande. Si la ligne de référence contient
une réponse adverse non triviale, le board cible n'est pas reproductible en solitaire — il faut le
détecter au moment de rejouer la référence et **avertir** plutôt que produire un résultat faux.

### 4.4 Vérification et écriture — la même passe

Chaque candidat retenu est **rejoué depuis zéro dans un duel neuf**, avec sa séquence de réponses.
On vérifie que le board final correspond, et on enregistre le flux de messages. Ces deux besoins sont
satisfaits par le même run, donc la vérification est gratuite. Elle donne une propriété forte :

> Une hypothèse fausse (digest incomplet, classe d'équivalence abusive) produit un candidat **rejeté**,
> jamais un `.yrpX` incorrect en sortie.

Écriture via `Replay::BeginRecord` / `WritePacket` / `EndRecord`, avec le `yrp1` des réponses embarqué
pour que les fichiers soient rejouables dans EDOPro **et** rendables par `--render-replay`.

---

## 5. Le patch ocgcore : arène à base fixe + checkpoint par pages sales

C'est la réponse à « la solution la plus performante et élégante ».

### 5.1 Principe

Puisque l'état ne peut pas être sérialisé sémantiquement (§2.4), on le sauvegarde **au niveau mémoire**.
Ça n'est possible que si *toute* la mémoire mutable du duel vit dans une région contiguë que l'on
contrôle. On restaure **à la même adresse de base**, donc tous les pointeurs absolus — C++ comme Lua —
restent valides sans aucune relocation. Le duel ne sait même pas qu'il a été restauré.

```
  VirtualAlloc(base fixe, 512 Mo, MEM_RESERVE | MEM_WRITE_WATCH, PAGE_READWRITE)
  └── mspace dlmalloc (create_mspace_with_base) — allocateur O(1) déterministe
       ├── heap Lua complet         ← lua_newstate(arena_alloc, mspace)
       ├── objet duel + field       ← operator new global redirigé
       ├── cards / effects / groups
       └── tous les nœuds de conteneurs STL
```

Deux redirections suffisent pour tout capturer :

1. **Lua** : `luaL_newstate()` → `lua_newstate(arena_alloc, mspace)` + `lua_atpanic`. Une ligne.
2. **C++** : surcharge de `operator new` / `operator delete` **globale dans le binaire du solveur**
   (pas dans ocgcore), routée vers l'arène quand un drapeau thread-local « in duel » est levé.
   `operator delete` dispatche par plage d'adresses : `ptr ∈ arène → mspace_free`, sinon `free`.
   C'est un motif classique et robuste, et il laisse le code C++ d'ocgcore **totalement inchangé** —
   pas un seul `typedef` de conteneur à toucher. C'est là que se trouve l'élégance de la solution.

Ce que ça implique : ocgcore doit être **lié statiquement** au solveur (cible `StaticLib` déjà
présente), pas chargé comme DLL. EDOPro et `replay2video.exe` continuent d'utiliser `ocgcore.dll`
normalement — aucun impact sur l'existant.

### 5.2 Checkpoint incrémental

Copier l'arène entière à chaque nœud coûterait ~1 ms (heap estimé 4–10 Mo, cf. §7). On ne copie que
les **pages modifiées**, obtenues par `GetWriteWatch(WRITE_WATCH_FLAG_RESET, …)`.

Subtilité importante : `GetWriteWatch` donne l'ensemble des pages sales, **pas leur contenu
d'avant**. Il faut donc un miroir. Le schéma correct, en LIFO strict (ce qui est exactement le cas
d'un DFS) :

- On maintient un **miroir `M`** hors arène, qui contient en permanence l'état de l'arène *au moment
  du checkpoint courant*.
- **`push()`** : `GetWriteWatch(RESET)` → ensemble `D` des pages modifiées depuis le dernier reset.
  Pour chaque page de `D` : ranger `M[page]` dans le journal d'annulation du niveau courant, puis
  `M[page] ← arène[page]`. Empiler un nouveau niveau vide.
- **`pop()`** : `GetWriteWatch(RESET)` → pages modifiées depuis le `push`. Les restaurer depuis `M`
  (qui est exactement l'état du `push`). Dépiler, puis réappliquer le journal du niveau redevenu
  sommet sur `M` pour rétablir l'invariant.

Coût par transition : O(pages touchées), soit quelques dizaines de µs au lieu de ~1 ms. Profondeur de
pile nécessaire : ~20 niveaux macro + ~10 micro, négligeable en mémoire.

*Alternative écartée* : `PAGE_READONLY` + Vectored Exception Handler (copy-on-write classique). Plus
direct — pas de miroir — mais une exception par page et par niveau (~5–20 µs chacune), et surtout une
interaction risquée avec le `longjmp`/SEH que Lua utilise pour la gestion d'erreurs. Le schéma
write-watch n'installe aucun handler et ne touche à aucun mécanisme de contrôle de flux.

### 5.3 Le GC Lua devient inutile — et c'est un gain double

Deux problèmes disparaissent en arrêtant le GC (`lua_gc(L, LUA_GCSTOP)`) pendant l'exploration :

1. Le marquage GC écrit dans l'en-tête de *tous* les objets vivants → il salirait la quasi-totalité
   des pages et anéantirait le bénéfice du checkpoint incrémental.
2. Le GC introduit une dépendance au timing dans l'ordre de collecte.

Et la mémoire n'est pas perdue : **le `pop()` récupère automatiquement tout ce qu'une branche a
alloué**, puisqu'il restaure l'état de l'allocateur lui-même. Le déchet Lua d'une branche abandonnée
disparaît avec la branche. Il faut simplement dimensionner l'arène pour la branche la plus profonde,
et `collectgarbage` est déjà neutralisé côté scripts ([interpreter.cpp:56](../edopro/ocgcore/interpreter.cpp#L56)).

### 5.4 Bonus : déterminisme bit-à-bit

`luai_makeseed` dérive la graine de hachage des chaînes Lua d'une adresse de pile et de l'horloge
([lstate.c:71](../edopro/ocgcore/lua/src/lstate.c#L71)). En la fixant à une constante dans
`luaconf-customize.h`, combiné à un allocateur d'arène qui rend les adresses reproductibles, le core
devient **déterministe bit-à-bit d'un run à l'autre**. Ce n'est pas requis pour la feature, mais ça
rend les bugs de recherche reproductibles — ce qui, sur un projet de ce type, vaut cher.

### 5.5 Parallélisme

Une arène par thread, à des bases distinctes. Les checkpoints ne sont donc pas transférables entre
threads — sans importance : chaque worker reçoit un **préfixe de réponses**, le rejoue une fois pour
se positionner, puis explore son sous-arbre en checkpoint/restore local. Répartition par vol de
travail sur la frontière macro.

### 5.6 Liste exacte des modifications ocgcore

| # | Fichier | Modification | Ampleur |
|---|---|---|---|
| A1 | `ocgcore/arena.h/.cpp` *(nouveau)* | Région `MEM_WRITE_WATCH` à base fixe + mspace dlmalloc + pile de checkpoints | ~400 l. |
| A2 | `ocgcore/interpreter.cpp` | `luaL_newstate()` → `lua_newstate(arena_alloc, …)`, `lua_atpanic`, `LUA_GCSTOP` optionnel | ~10 l. |
| A3 | `ocgcore/lua/luaconf-customize.h` | `#define luai_makeseed(L) 0` | 1 l. |
| A4 | `ocgcore/premake5.lua` | Cible `StaticLib` exposée au solveur | ~5 l. |
| C1 | `ocgcore/ocgapi.h/.cpp` | `OCG_DuelQueryProcessorState` — phase, tour, compteurs d'invocation, `effect_count_code`, forme de `core.units` | ~120 l. |
| D1 | `ocgcore/interpreter.cpp` | *(optionnel)* cache de bytecode `lua_dump` pour `load_script` | ~40 l. |

**Aucune modification** de `duel.cpp`, `field.cpp`, `card.cpp`, `effect.cpp`, `processor*.cpp`,
`operations.cpp` — soit les 24 000 lignes les plus délicates du core. La redirection d'allocateur
globale les couvre sans les toucher. C'est le point le plus important de cette conception.

Le patch D1 est de faible priorité : avec le snapshot, on ne crée **qu'un duel par worker** pour toute
la recherche. Le coût de `luaL_loadbuffer` sur les 418 Ko de `utility.lua` + `constant.lua` + `proc_*.lua`
n'est payé qu'à l'initialisation et lors des N vérifications finales.

---

## 6. Composants hors ocgcore

Nouveau binaire headless, sans Irrlicht ni FFmpeg — `tools/combosolver/` :

| Module | Rôle |
|---|---|
| `replay_source` | Ouverture yrpX, extraction du `yrp1` embarqué, decks, seed, `duel_parameters`, réponses de référence |
| `duel_host` | Callbacks `cardReader` (cards.cdb) / `scriptReader` (filesystem), bootstrap calqué sur `old_replay_mode.cpp:108` |
| `prompt_decoder` | Décodage des 18 `MSG_SELECT_*` → liste de choix légaux + encodage des réponses |
| `equivalence` | Classes d'équivalence des choix (§4.1), chacune désactivable |
| `state_digest` | Digest sémantique via `OCG_DuelQueryLocation` + `OCG_DuelQueryProcessorState` |
| `search` | Moteur deux niveaux, transposition, coût uniforme, branch-and-bound |
| `verifier` | Rejeu depuis zéro d'un candidat, contrôle du board, capture du flux |
| `replay_writer` | Écriture yrpX + yrp1 embarqué via `Replay` |

CLI envisagée :

```
combosolver.exe --replay duel.yrpX --outdir solutions/ --workdir D:\ProjectIgnis
                [--player 0|1] [--top 10] [--threads N] [--time-limit 600]
                [--max-actions N] [--prune-resources exact|heuristic]
```

---

## 6bis. Contrainte de versionnement découverte au jalon 0

Un `.yrpX` n'est fidèlement rejouable qu'avec **le core et le jeu de scripts contemporains de son
enregistrement**. Une version décalée ne plante pas : le core pose une question différente, la
réponse enregistrée devient invalide, et le duel part ailleurs. Le seul détecteur est le compteur de
`MSG_RETRY`.

Sur le replay de test (2026-04-13), les deux côtés étaient décalés :

| Composant | État | Effet |
|---|---|---|
| `edopro/ocgcore` (pin du dépôt) | `158aebe`, **2025-04-17** — un an de retard | une fenêtre `MSG_SELECT_CHAIN` de trop après activation → 269 `MSG_RETRY`/290 |
| `repositories/delta-bagooska` (scripts installés) | suivi en continu, **en avance** sur le core | `Duel.GetReasonEffect` (ajouté le 2026-06-09) → `attempt to call a nil value` |
| ocgcore `8e5f4e4` (2026-04-07) + scripts `0e90a3e8` | contemporains du replay | **0 `MSG_RETRY`, 290/290** |

Conséquences pour la feature :

- Le solveur doit accepter un **jeu de scripts et un core explicites** (`--scriptdir`, `--dll`), pas
  seulement `--workdir`.
- Il doit **compter les `MSG_RETRY` du rejeu de référence et refuser de continuer** si le compte
  n'est pas nul : sans ligne de référence fidèle, le board cible est faux et tout le reste l'est aussi.
- Idéalement il date le replay et propose l'extraction automatique des versions correspondantes
  (`git archive` depuis le dépôt de scripts et le sous-module ocgcore, vers un cache local — jamais
  vers l'installation EDOPro de l'utilisateur).
- Le sous-module `lua/src` d'ocgcore a **son propre pin**, à extraire séparément.

---

## 6ter. Elements repris d'un plan externe (bot RL mono-deck)

Un plan de bot Yu-Gi-Oh! par apprentissage a ete verse au dossier. Sa couche 1 (fork ocgcore)
converge independamment sur la meme architecture d'instantane que le §5 : arene memoire, allocateur
Lua personnalise, pages sales, restauration a la meme adresse de base, RNG dans l'arene, GC suspendu.
Cette convergence vaut confirmation. Quatre elements en sont repris, un est ecarte, un est mesure.

**Repris :**

| Idee | Ce qu'elle apporte ici |
|---|---|
| **Garde-fous d'execution** (budget de pas de processeur, longueur de chaine, activations par tour, terminaison a issue neutre) | Une recherche exhaustive **explorera** des branches qui bouclent — effets obligatoires en cycle, FTK. Sans budget, un worker se bloque et la recherche ne termine jamais. Manquait completement a la conception. |
| **Harnais de neutralite asymetrique** | Version rigoureuse de notre controle `MSG_RETRY`. Point non evident : des qu'on elide des decisions forcees, les **indices de reponses ne s'alignent plus** entre core vanilla et core patche, alors meme que la semantique est identique. Il faut journaliser les reponses auto-generees, remapper, puis comparer les **flux de messages filtres des messages cosmetiques** — pas les indices. |
| **Predicat d'equivalence conservatif** (meme code, meme zone, memes flags, meme position, memes compteurs, aucun ciblage anterieur les distinguant ; au moindre doute, ne pas fusionner) | Formulation plus precise que le §4.1. Adoptee telle quelle. |
| **Instantane naif d'abord** : `memcpy` complet de l'arene avant tout suivi de pages sales | Avec un pas de moteur mesure a **0,18 ms**, un `memcpy` d'arene de quelques Mo (~100 µs) represente ~55 % de surcout — deja utilisable. **Le jalon 0c n'a donc pas besoin du suivi de pages sales pour demarrer**, ce qui retire le morceau le plus risque du chemin critique. |

Sont aussi retenus, moins critiques : mise en cache memoire des scripts (notre `ScriptProvider` relit le
disque a chaque appel), et le dimensionnement par le cache L3 — le jeu de travail d'un duel etant de
quelques Mo, le nombre optimal de workers est probablement bien plus bas que le nombre de threads, et
l'arene doit rester **committee au minimum** plutot que large.

**Ecarte :** l'instantane par `mprotect(PROT_READ)` + handler `SIGSEGV`. C'est l'approche
copie-a-l'ecriture paresseuse, qui evite le miroir du §5.2. Elle est raisonnable sous Linux ; sous
Windows l'equivalent est un gestionnaire d'exceptions vectorise, plus couteux et en interaction avec
le SEH qu'utilise Lua pour ses erreurs. On garde `GetWriteWatch` + miroir.

**Mesure contre affirmation.** Le plan avance que « 60 a 80 % des requetes sont triviales ». Mesure
sur le replay de reference :

```
SELECT_CHAIN    93 / 141  forcees  (66%)   <-- fenetres de chaine a zero option
SELECT_CARD      8 /  21  forcees  (38%)
SELECT_PLACE     5 /  35  forcees  (14%)
TOTAL          106 / 291  forcees  (36%)
```

**36 %, pas 60–80 %.** Et surtout, la distinction que le plan ne fait pas : une decision forcee a un
facteur de branchement de 1, elle **n'ajoute aucun noeud a l'arbre**. L'elision est donc pour nous un
gain de **debit** (×1,57 sur la profondeur, donc sur le cout d'une re-simulation), pas une reduction
de l'espace de recherche — notre recherche a deux niveaux absorbe deja ces decisions au niveau micro.
Le plan la classe tres haut parce que dans un cadre RL la profondeur est aussi l'horizon
d'attribution de credit ; ce n'est pas notre cas.

**A ne pas transferer.** Le plan affirme qu'avec transpositions « un combo a 12 etapes passe de
plusieurs millions de noeuds a quelques dizaines de milliers ». C'est exactement la quantite non
mesuree du §3.0, avancee sans support. Et son hypothese de travail est un combo a 12 etapes, quand la
ligne de reference mesuree en compte **59**. Son optimisme sur le nombre de noeuds ne se transpose pas.

**Hors sujet ici :** observation incrementale sous forme de tenseur (destinee a un reseau), requete
filtree par point de vue (notre recherche est en solitaire et veut l'information complete), et toute
la pile apprentissage.

---

## 6quater. Transplantation : refaire le board depuis un autre deck

Cas d'usage distinct de l'optimisation : on garde le board cible d'un replay et
on cherche à l'atteindre depuis **une autre partie** — autre deck, autre main,
autre graine. Option `--start <replay>`.

### Pourquoi la ligne de référence ne se rejoue pas

Une réponse enregistrée dit « le troisième élément de la liste ». Dans un autre
duel, la liste n'a ni le même contenu ni le même ordre : rejouer les octets
produit du `MSG_RETRY` immédiat. Ce qui survit au changement de deck, c'est
l'**arête** que l'énumérateur associe déjà à chaque choix — elle est bâtie sur
des *codes de cartes*, pas sur des indices. `LiftPlan` relève donc la ligne sous
cette forme, en identifiant chaque décision par l'**état qu'elle atteint**
(appliquer / comparer / restaurer), puisque les octets enregistrés ne sont pas
comparables à ceux qu'énumère le solveur.

### Répertoire, pas calendrier

Premier essai : suivre le plan pas à pas, avec une fenêtre de rattrapage.
**Échec mesuré** — la descente meurt à la 4ᵉ décision, sur une question que la
référence n'a jamais eue à trancher. Deux decks ne posent pas les mêmes
questions dans le même ordre, et un alignement séquentiel décroche à la première
question inédite.

Le plan est donc traité comme un **répertoire de coups** : un coup que la
référence a joué, n'importe où dans sa ligne, est gratuit ; tout autre coup coûte
un écart. Le budget d'écarts mesure alors ce qu'il faut *inventer* en plus du
répertoire. Avec ce seul changement la descente à zéro écart passe de 4 à
60 000 états.

### Trois passes, et ce que chacune apporte

| Passe | Ce qu'elle fait | Portée mesurée |
|---|---|---|
| Faisabilité | Les cartes du board sont-elles dans le deck ? Les cartes que la ligne *empruntait* y sont-elles ? | Instantané, et décisif : une carte de board absente rend la cible inatteignable, point |
| Écarts bornés | Large et courte : épuise le répertoire à 0 écart, explose dès le 1ᵉʳ coup inventé | ~60 k états à k=0 (**épuisé**), 1,4 M à k=1 (non épuisé) |
| Tirages gloutons | Profonde : descend d'un trait jusqu'à la fin du tour, mille lignes distinctes | 8,4 M états, va **nettement plus loin** que les deux autres |

Le point structurant : le board est à ~300 décisions, alors qu'une recherche à
écarts bornés ne descend qu'aussi profond que son budget d'écarts. Il faut de la
**profondeur**, pas de la largeur — d'où les tirages, guidés par une heuristique
matérielle (corps sur le terrain, monstres au cimetière) et amorcés par le
répertoire. L'heuristique ne peut pas se contenter de compter les cartes du board
cible : ce sont des Synchro de fin de chaîne, le compte reste à zéro pendant
toute la montée et ne guide rien.

### Pièges rencontrés, tous silencieux

- **Variantes d'illustration.** `QUERY_CODE` rend le code imprimé ; deux
  exemplaires du même Quetzacoatl (29053656 / 29053657) n'y portent pas le même
  nombre. Sans résolution d'alias (`QUERY_ALIAS`, qui passe par `get_code()`), la
  carte est déclarée absente du deck et la recherche est annulée d'emblée.
- **Nom effectif ≠ carte physique.** `get_code()` reflète aussi
  `EFFECT_CHANGE_CODE` : Scarred Dragon Archfiend s'y présente comme « Red Dragon
  Archfiend ». Correct pour comparer deux terrains, faux pour compter ce qu'un
  deck doit contenir — d'où deux comptes distincts (`Code()` et `code`).
- **`Arena::Init` s'approprie le routage des libérations du thread** (`t_owner`).
  Monter une seconde arène sur le thread principal dépossède celle du duel de
  référence : plantage différé. Toute arène supplémentaire tourne sur son thread.
- **Décisions forcées facturées comme des écarts.** Un prompt à réponse unique
  n'est pas une déviation ; le facturer vidait le budget sur des non-choix.

---

## 7. Inconnues et risques

Les chiffres ci-dessous sont des **estimations à valider** — c'est l'objet du jalon 0.

Toutes les inconnues du §5 et du §3.0 ont été mesurées. Résultats :

| Inconnue | Estimation initiale | **Mesuré** |
|---|---|---|
| Taille du heap vif d'un duel | 4–10 Mo | **4,1 Mo vifs, 7,4 Mo de zone servie** |
| Pages sales par décision | « quelques dizaines de pages » | **60 pages médianes (240 Ko), 97 en moyenne** — l'estimation était bonne |
| Coût d'une restauration incrémentale | ~10 µs espéré | **0,05 ms** (contre 0,68 ms en image complète, ×13) |
| Croissance du graphe d'états | aucun a priori | **×1,25 par 2 décisions** (§3.0) |
| Effet de l'arrêt du GC Lua | gain espéré sur les pages sales | **aucun gain** : les pages sales viennent des allocations neuves, pas du marquage. Le GC reste arrêté (neutre et sûr), mais ce n'était pas le levier |
| Sorties d'arène (état non capturé) | risque | **zéro** — tout l'état du core est capturé |

Risques qualitatifs :

- **Digest incomplet** → candidats rejetés à la vérification. Détectable (taux de rejet), pas dangereux.
- **Classes d'équivalence abusives** → solutions manquées, silencieusement. Le risque le plus sournois ;
  mitigé en les rendant désactivables et en comparant les résultats avec/sans sur un cas de référence.
- **`operator new` global** → capture aussi les allocations du solveur faites pendant un appel au core
  (callbacks lecteur de script/carte). Gérable en abaissant le drapeau autour des callbacks, mais à
  tester soigneusement : c'est le point de fragilité n°1 du montage.
- **Explosion combinatoire résiduelle** malgré tout, sur un deck très ramifié. Le budget temps et le
  top-N garantissent une sortie utile même sans exhaustivité ; le log doit dire clairement si
  l'espace a été épuisé ou non.

---

## 8. Plan de livraison

| Jalon | Contenu | Sortie vérifiable |
|---|---|---|
| **0a — Rejeu instrumenté** ✅ | Spike Python (`tools/`) puis binaire C++ `combosolver`. Rejeu fidèle (0 `MSG_RETRY`), branchements et coût de référence mesurés (§3). | **Fait.** Voir aussi §6bis. |
| **0c — Arène et instantanés** ✅ | Arène à base fixe, allocateur par classes de taille sans en-tête, heap Lua branché via patch `lauxlib.c`, `operator new` global redirigé. Instantané incrémental par pages sales avec miroir et journal d'annulation. Tests : fidélité sur 290 décisions, frères successifs, imbrication à 20 niveaux. | **Fait.** Restauration 0,05 ms, zéro sortie d'arène. |
| **0b — Courbe de croissance** ✅ | Énumérateur des 19 types de prompt, digest d'état, table de transposition, DFS borné. Couverture de l'énumérateur validée **sémantiquement** contre la ligne de référence : 289/290. | **Fait — et la réponse est négative pour l'exhaustivité** (§3.0). |
| **1 — Recherche guidée** | Best-first vers le board cible plutôt qu'énumération. Heuristique de distance au board. Bornes lexicographiques (C_ref, A_ref, D_ref). | Trouver des solutions sur un combo réel |
| **2 — Sortie yrpX** | Vérification par rejeu depuis zéro + écriture des `.yrpX` du top-N. | Livrable final |
| **1 — Arène** | Patchs A1–A4. Le duel tourne entièrement dans l'arène ; `push`/`pop` testés en rejouant la référence avec des allers-retours aléatoires. | Le rejeu donne un état identique avec et sans checkpoints |
| **2 — Introspection + digest** | Patch C1, `state_digest`, mesure du taux de collapse. | Deux permutations d'une même ligne produisent le même digest |
| **3 — Recherche** | Énumérateur, équivalences, moteur deux niveaux, B&B. | Trouve les réordonnancements triviaux d'un combo simple |
| **4 — Sortie** | Vérificateur + écriture yrpX, top-N. | Les `.yrpX` produits se rejouent dans EDOPro et se rendent avec `--render-replay` |
| **5 — Passage à l'échelle** | Multithread, budget temps, réglages d'élagage. | Combo réel de 15+ actions traité dans le budget |

Le jalon 0a a déjà rapporté plus que prévu : le rejeu fidèle, les branchements réels, le coût de
référence — et une contrainte de versionnement qui n'était pas dans la conception initiale (§6bis).
Restent 0b et 0c, qui tranchent les deux questions dont dépend tout le reste : « comment croît le
nombre d'états macro » (§3.0) et « combien de pages sales par action » (§5). Tant qu'elles ne sont
pas mesurées, tout chiffrage de faisabilité relève de la conjecture.

### Outils

Prototype Python (itération rapide, sert encore d'oracle croisé) :

```
tools/inspect_replay.py   parse yrpX + yrp1 embarque, decoupage par tour, proxys de cout
tools/ocgcore.py          liaison ctypes vers ocgcore.dll x64, CardDB, ScriptProvider, queries
tools/solver_spike.py     rejeu instrumente
```

Binaire C++ (`combosolver/`), qui compile sa propre copie d'ocgcore :

```
tools/fetch_solver_deps.ps1   extrait ocgcore 8e5f4e4 + son Lua, applique les patchs d'arene
combosolver/replay.cpp        yrpX + yrp1 embarque, LZMA
combosolver/assets.cpp        CardDB (sqlite3), ScriptProvider
combosolver/arena.cpp         arene, allocateur, instantanes incrementaux
combosolver/duel.cpp          ocgcore lie statiquement
combosolver/prompt.cpp        decodage des prompts (branchement)
combosolver/enumerate.cpp     enumeration des reponses legales
combosolver/search.cpp        digest, transposition, DFS borne
```

```powershell
.\tools\fetch_solver_deps.ps1
cd combosolver ; ..\premake5\premake5.exe vs2022
MSBuild build\combosolver.sln /p:Configuration=Release /p:Platform=x64

.\combosolver\bin\Release\combosolver.exe "D:\ProjectIgnis\replay\synchron handrip 2.yrpX" `
    --scriptdir <cache>\scripts_2026-04-13\script --growth
```

Le binaire enchaîne : rejeu de référence instrumenté → mesure des pages sales → test de fidélité des
instantanés → test de stress (frères, imbrication) → couverture de l'énumérateur → courbe de
croissance. Il rend un code de sortie non nul dès qu'un de ces contrôles échoue.

---

## 9. Rendre la recherche moins gloutonne : nouveauté et politique apprise

Constat déclencheur : à un écart sur l'autre deck, 1 439 473 états développés pour 222 363 fusions —
la table de transposition ne rattrape que 15 %, parce qu'elle ne fusionne que les états *identiques*.
Deux lignes qui diffèrent d'une carte au cimetière sont distinctes et pourtant sans intérêt distinct.
Deux leviers de la littérature ont été intégrés : l'élagage par nouveauté (Iterated Width, Lipovetzky
& Geffner) et les tirages par politique apprise (NRPA, Cazenave). Chaque décision ci-dessous est
adossée à une mesure, y compris celles qui ont conclu à *ne pas* utiliser une technique.

### 9.1 La largeur effective se mesure avant de promettre (`--width`)

Un **atome** est un fait `(zone, carte, occurrence)` du joueur cible — plus les entrées fines du
terrain (position, matériaux, compteurs) et le compte du deck. Mesure le long de la ligne de
référence (276 décisions jusqu'au board) :

| | |
|---|---|
| Atomes distincts sur la ligne | **115** (428 avec sérialisation par sous-but) |
| États « muets » (aucun atome neuf) | **225 / 276 = 82 %** |
| Plus longue série muette | **17 décisions** |

Deux enseignements. D'abord le potentiel : 82 % des états de la *solution elle-même* n'apportent
aucun fait neuf — l'espace exploré autour est donc essentiellement du silence. Ensuite la contrainte :
couper au premier silence tuerait la référence ; toute coupure doit tolérer une **patience** supérieure
à la plus longue série muette. La patience est auto-calibrée à `max(12, série_max + 4)` = 21 sur ce
replay, et `--novelty <n>` / `--no-novelty` l'imposent ou la coupent.

### 9.2 L'élagage par nouveauté dans les recherches systématiques

`NoveltyCut` : à chaque nœud, collecter les atomes, les partitionner par nombre de sous-buts atteints
(cartes du board cible posées — c'est la sérialisation du but conjonctif, façon Serialized IW/BFWS),
puis couper la branche après `patience` décisions consécutives sans fait inédit. Trois garde-fous :

- **Le préfixe pur répertoire est exempt** (aucun écart pris) : l'invariant « à zéro écart la
  référence est retrouvée » est préservé par construction.
- **Deux régimes, choisis sur mesure.** Le régime *sensible à la profondeur* (refaire un fait en
  moins de décisions compte comme neuf) est sûr mais n'élague presque rien dans un DFS qui visite
  les déviations profondes d'abord : toute branche plus courte y est « nouvelle » (3 584 coupures
  sur 1 M d'états à k=1 — un élagage de façade). Le régime *strict* (seuls les faits jamais vus
  comptent) encapsule chaque invention dans une bulle de `patience` décisions ; il est réservé à la
  transplantation, où l'on cherche du matériel neuf, pas des variantes.
- **Contrôle A/B automatique** sur le cas même-deck à k=1 : même moteur, même budget, avec puis sans
  nouveauté ; le rapport imprime états, coupures, solutions et meilleur coût des deux passes. Un
  élagage qui perd des solutions le dit lui-même. Mesuré : −4 % d'états, mêmes 20 solutions, même
  meilleur coût (régime sensible à la profondeur, patience 21).

### 9.3 Rollout-IW sans arbre : l'impasse, mesurée

Couper un tirage glouton dès qu'il cesse de produire du neuf (table partagée entre tirages) semble
être « notre `Rollout()` en mieux ». **Mesuré : 2/8 cartes cibles au lieu de 6/8.** La table étant
partagée, tout tirage qui re-parcourt le même début meurt en `patience` décisions *avant d'avoir pu
dévier* — les tirages plafonnent à ~19 décisions de moyenne. C'est le mode de défaillance de
Rollout-IW privé de son arbre : la version publiée descend un arbre de nœuds déjà visités *sans* les
re-tester en nouveauté, et ne teste que la frontière. Plutôt que de payer l'arbre, le budget est allé
à NRPA (§9.4). La coupure reste dans le code (`novelty_rollout_cut`), désactivée par défaut, avec la
mesure en commentaire.

### 9.4 NRPA : la politique remplace l'évaluation des fils

`RunNrpa` implémente NRPA avec biais GNRPA : un poids par **code de coup** — nos `plan_key`, déjà
stables d'un duel à l'autre — échantillonnage softmax, adaptation vers la meilleure séquence à chaque
niveau, garde-fou de stagnation (8 itérations sans progrès → remontée), redémarrages à politique
vierge jusqu'au budget. Le répertoire entre comme **biais** (+1,5), pas comme prime additive — la
prime avait déjà été mesurée nocive. Deux détails qui comptent :

- **Les changements de phase sont exclus du biais.** « → End Phase » est au répertoire (la référence
  finit son tour) ; avec le biais, un prompt idle à ~10 choix terminait le tour une fois sur trois,
  et terminer le tour est irréversible. La politique reste libre de l'apprendre.
- **Aucune évaluation des fils.** Le tirage glouton avance/mesure/restaure chaque fils (~13 pas de
  moteur par décision aux idle) ; NRPA échantillonne et avance. Mesuré : ~2× plus d'états visités
  par seconde, et des tirages entiers plusieurs fois moins chers.
- **Coupure de tour** (toutes stratégies) : le board cible est celui de la fin du tour 1 ; passé le
  changement de tour, le board est figé et la descente est du temps perdu.

Le score d'un tirage est le **max le long de la ligne** (matériel ×1000 + nombre d'états ayant produit
un atome inédit en départage) ; atteindre le board vaut 10¹² − décisions.

**Résultat sur `test 4`** — la question ouverte du §6quater est **tranchée**. À 90 s, NRPA atteint
7/8 là où 300 s de tirages gloutons plafonnaient à 6/8. À 530 s (12 workers NRPA passés au niveau 3,
4 gloutons), **NRPA atteint le board complet, 8/8** : 210 k tirages, 23,7 M d'états, et une ligne de
**224 décisions, 49 actions, 16 cartes brûlées** — moins chère que la référence sur son propre deck
(273 décisions, 56 actions, 19 brûlées). Le replay écrit (`solution_00_b16_a49.yrp`) se rejoue depuis
zéro : 224/224 réponses, 0 `MSG_RETRY`, board identique. Le partage des workers est de 3 NRPA pour
1 glouton ; la découverte n'est pas déterministe (budget en temps réel), mais la sortie est
auto-vérifiée avant écriture.

### 9.5 Contraintes de ligne (`--summon`, `--guard`, `--no-activate`)

La recherche accepte des contraintes sur la *ligne*, pas seulement sur le board final. Trois
familles, toutes résolues par code ou fragment de nom à résolution unique (un fragment ambigu est
une erreur qui liste les candidats) :

- `--summon "n:carte[|carte...]"` (répétable) : la n-ième invocation — normales + spéciales, le
  décompte de Nibiru ; les flips n'y comptent pas — doit être l'une des cartes données.
- `--guard "n:clause[|clause...]"` (clause = `carte[@zone][+carte[@zone]...]`, zones `main terrain
  cimetiere banni extra`, défaut `terrain`) : à partir de la n-ième invocation, **à chaque fenêtre
  de réponse de l'adversaire** — là et seulement là où Nibiru peut tomber — au moins une clause doit
  tenir entièrement. Évaluer la garde aux fenêtres adverses plutôt qu'à chaque décision est ce qui
  la rend jouable : les creux transitoires en pleine résolution (le garde part en matériel pendant
  que son remplaçant arrive) sont invisibles pour l'adversaire et ne sont pas des fautes.
- `--no-activate "carte[@zone]"` (répétable, défaut `terrain`) : interdit d'activer cette carte
  depuis ces zones. Le choix n'est jamais émis par l'énumérateur — la branche n'existe plus — et la
  réponse *enregistrée* du mode réparation, qui contourne l'énumérateur, est rattrapée par
  `ResponseForbidden`. L'activation depuis la main (qui *pose* une magie) reste permise par défaut :
  c'est l'effet igné superflu (« payer 2000 LP ») qu'on bannit, pas la pose.
- `--guard-off "mainadv<=N"` : la garde s'éteint aux fenêtres où la main adverse compte au plus N
  cartes. C'est la sémantique du handrip : une fois la main adverse vidée, plus personne pour avoir
  Nibiru — exiger la garde au-delà serait une contrainte de papier. Mesuré : la référence, jugée
  violante sous garde stricte (2 fenêtres nues après l'invocation #27), est **conforme** sous
  `mainadv<=2` — ses fenêtres nues arrivent toutes après le handrip. La contrainte assouplie
  correspond donc exactement au raisonnement du joueur.
- `--resolve "carte[:n]"` (répétable, max 4) : la ligne doit résoudre l'effet de cette carte au
  moins n fois avant le board. Compté à l'activation (`MSG_CHAINING`) — en solitaire rien ne nie une
  chaîne. Contrainte de **minimum** : elle ne peut pas élaguer en cours de ligne (l'avenir peut
  encore l'accomplir), elle se contrôle **au but** — un board conforme sans les résolutions n'est
  pas une solution, et la recherche continue au-delà. Les tirages NRPA reçoivent le progrès plafonné
  comme gradient. Cas d'usage : `--resolve "PSY-Framelord Omega:2" --resolve "Trishula, Dragon of
  the Ice Barrier"` garantit le handrip de 3 cartes — celui-là même qui éteint la garde. Verdict sur
  la référence : Omega 2/2, Trishula 1/1.

**L'identité d'un choix de position inclut la carte.** Mesuré en le payant : l'arête de
`MSG_SELECT_POSITION` ne portait que la position, donc la politique NRPA apprenait UN poids global
« DEF » partagé par toutes les cartes — six monstres en défense là où la cible en veut cinq en
attaque — et le répertoire perdait l'intention par carte de la référence. Avec l'arête (carte,
position), quatre positions sur cinq sont apprises au run suivant.

**Routage réparation-d'abord.** Quand la référence viole une contrainte corrigible par réordonnement
(son seul défaut sous garde assouplie : l'invocation #5), la réparation à écarts bornés SOUS
contraintes passe en premier (40 % du budget) — les contraintes y forcent les déviations au bon
endroit — puis NRPA en secours. **État mesuré sur la discipline Nibiru assouplie** (3 runs de
480 s) : aucune ligne conforme trouvée. Réparation : rien jusqu'à k=12 (échanger les invocations
#4/#5 désaligne toute la suite enregistrée — la brittleness d'alignement du §6quater, retrouvée).
NRPA : plafonne à 3/8 = {Zalen, Junk Signal, Assault Zone} posés — la signature de la clause : ses
lignes laissent Junk Signal dormir dans le deck (ni en main, ni sur le terrain pendant la fenêtre
critique, mesuré au juge), là où la référence le tient en main dès avant le 5ᵉ summon. La question
décisive est une question de jeu, pas de recherche : Zalen peut-il encore s'invoquer APRÈS la
normal summon de Junk Synchron, et le deck peut-il chercher Junk Signal avant le 5ᵉ summon sur
d'autres ouvertures que celle de la référence ?

Cas d'usage complet — jouer sous menace Nibiru :

```
--guard  "5:Crystal Wing|Zalen@terrain+Junk Signal@main"
--guard-off "mainadv<=2"
--no-activate "Duel Evolution - Assault Zone"
```

« Dès que la 5ᵉ invocation résout — Nibiru devient actif — à toute fenêtre adverse : soit Crystal
Wing est en jeu, soit Zalen est en jeu *avec* Junk Signal encore en main pour chaîner par-dessus ;
la garde s'éteint une fois la main adverse handrippée ; et on ne paie jamais les 2000 LP du
terrain. » Sous cette discipline, la référence est **conforme** (33/33 fenêtres sous menace
couvertes — Zalen posé en 4ᵉ, donc déjà là quand la fenêtre s'ouvre).

**Leçon d'interprétation, apprise en la payant** : « Zalen en 5ᵉ summon » au sens du joueur voulait
dire « le garde est en place quand la fenêtre Nibiru s'ouvre » — la sémantique de `--guard` seule.
L'encoder en plus comme `--summon "5:Zalen|CW"` (l'identité du 5ᵉ summon) sur-contraint : la
référence pose Zalen en 4ᵉ et devient artificiellement violante ; trois runs de 480 s ont buté sur
cette sur-contrainte avant que la relecture ne la lève. Les deux drapeaux existent parce que les
deux besoins existent — mais ils ne disent pas la même chose.

**Mode juge.** Passer un replay en entrée avec les drapeaux de contraintes (sans `--solve`) imprime
son verdict — le solveur contrôle aussi bien les replays qu'il produit que ceux joués à la main.
Jugements mesurés : la solution `b16_a47` (5ᵉ summon = Zalen) *respecte* le summon mais *viole* la
garde à 29 fenêtres sur 35 et *paie deux fois* les 2000 LP du terrain. Deux leçons : l'effet
« superflu » du terrain alimentait en réalité les lignes machines les moins chères, et la garde est
la contrainte qui mord. Sous la discipline complète, 500 s de recherche plafonnent à 3/8 — aucune
ligne conforme trouvée à ce budget, ce qui n'est pas une preuve d'inexistence : l'outil dit où ça
bloque (Junk Signal ne reste pas en main entre le 5ᵉ summon et l'arrivée de Crystal Wing).

Propriétés communes aux trois familles :

- **La contrainte élague, elle ne filtre pas.** Vérifiée au fil de la descente dans toutes les
  stratégies (y compris l'évaluation des fils et les deux familles de tirages) : une branche qui
  viole meurt sur-le-champ (`constraint_cuts`, `guard_cuts`). NRPA l'apprend tout seul — un tirage
  violant meurt avec un score bas, la politique s'en détourne.
- **Sémantique conditionnelle** : une ligne qui n'atteint pas la n-ième invocation n'est pas en faute.
- **La référence est jugée d'abord.** Le rapport imprime sa séquence d'invocations et son verdict —
  y compris la garde, évaluée fenêtre adverse par fenêtre adverse pendant le rejeu instrumenté. Si
  elle satisfait tout, l'invariant « 0 écart retrouve la référence » tient toujours. Si elle viole
  quelque chose, sa ligne n'est plus une solution : le mode même-deck bascule automatiquement sur le
  moteur de transplantation (répertoire + NRPA) appliqué au même deck, et le contrôle à 0 écart est
  suspendu en le disant.
- **Vérifié avant écriture, comme tout le reste** : le rejeu depuis zéro re-contrôle summon ET garde,
  et la séquence d'invocations de la meilleure solution écrite est imprimée, contrainte pointée.

**Résultat mesuré** (même deck, `--summon "5:Zalen|Crystal Wing"`, 450 s) : la référence viole la
contrainte (sa 5ᵉ invocation était Junk Synchron) ; le moteur NRPA a produit **2 lignes complètes et
conformes** — Zalen en 5ᵉ invocation — dont la meilleure coûte **16 brûlées / 47 actions /
229 décisions contre 19 / 56 / 273 pour la référence** : strictement meilleure sur les trois étages
du coût, *avec* une contrainte en plus. Rejeu de contrôle : 229/229, 0 `MSG_RETRY`.

Ce résultat dit autre chose, et c'est important : la réparation à écarts bornés avait établi que la
référence « résiste à 7 déviations » — vrai, mais elle ne voit que les petites perturbations de la
ligne enregistrée. NRPA, libre de tout ancrage, trouve sur le même deck des lignes **hors de ce
voisinage** et strictement meilleures. Le mode même-deck gagnerait à lancer les deux moteurs
systématiquement, pas seulement quand une contrainte disqualifie la référence.

### 9.6 Départ synthétique : decklist + main, sans replay (`--deck`, `--hand`)

La transplantation exigeait un replay de départ ; `--deck <f.ydk>` la libère de cette dépendance :
le duel de départ est **construit** — mêmes paramètres et même adversaire que la référence, deck du
`.ydk`, et main de départ imposée (`--hand "carte|carte|..."`, par défaut la main de la référence).

- **La main est forcée, pas espérée** : `DUEL_PSEUDO_SHUFFLE` (le core ne mélange plus) + le
  réordonnancement du main deck. L'extrémité qui se pioche n'est pas devinée : un duel jetable
  pioche la main et on la compare — si ni la queue ni la tête de liste ne la donnent, erreur
  franche. Jamais de recherche sur une main qu'on *croit* avoir.
- Les cartes de la main se prennent dans la decklist par identité canonique (une illustration
  différente convient) ; une main infournissable est une erreur explicite qui nomme la carte.
- Le reste de la chaîne est inchangé : faisabilité, plan-répertoire relevé sur la référence,
  contraintes, tirages, finisseur, vérification avant écriture — et `WriteYrp1` sérialise depuis la
  position synthétique, donc les replays produits se rejouent dans EDOPro.

### 9.7 Édition du board cible et contrainte de matériau

- `--board-add "carte[@ATK|DEF]"` / `--board-remove "carte"` (répétables) : la cible n'est plus
  forcément le board de la référence — on l'édite. L'édition part des cartes *capturées* au board
  de référence (positions, matériaux, compteurs compris) et recompose la clé via `MakeBoardKey` ;
  les ajouts sont des monstres en MZONE (défaut ATK). Garde-fous : retirer une carte absente est
  une erreur ; plus de 6 monstres exigés est signalé inatteignable (5 zones + 1 EMZ sans Links) ;
  l'édition exige la transplantation (`--deck`/`--start`) — en réparation, la référence ne peut
  plus servir de contrôle sur une cible qu'elle n'atteint pas.
- `--material "carte:attr[,attr...]"` : l'invocation de cette carte doit consommer au moins un
  matériau d'un de ces attributs (« Chaos Angel invoqué avec un monstre LUMIÈRE »). Détection par
  les `MSG_MOVE` marqués `REASON_SYNCHRO|REASON_MATERIAL` dans la même résolution que le
  `MSG_SPSUMMONING` ; violation = branche coupée (une invocation ratée ne se défait pas) ;
  re-vérifiée au rejeu avant écriture.

Cas d'usage, décidé avec le joueur pour tenir en 6 zones (7 monstres exigés = impossible) :

```
--board-remove "Hot Red Dragon Archfiend Abyss" --board-remove "Crimson Dragon"
--board-add "Naturia Beast" --board-add "Chaos Angel"
--material "Chaos Angel:lumiere"
```

### 9.8 Points chauds réglés, et ce qui reste

- `Heuristic()` faisait deux requêtes de zone par fils évalué dont une redondante :
  `ComputeBoardKey` compte désormais les monstres au passage (`mzone_count`) et l'heuristique prend
  le board déjà calculé. Une requête de zone économisée par fils.
- Le budget de la transplantation est désormais **global** : sonde (≤ 20 s), tirages (70 % du
  restant), écarts bornés (le reste) se partagent `--solve-ms` au lieu de l'empiler.
- **Non fait, et documenté comme tel** : la table de transposition partagée entre workers
  (lazy SMP) — les workers LDS refont du travail ; IW(2) (paires d'atomes) si IW(1) sérialisé
  s'avère trop agressif ailleurs ; Rollout-IW avec arbre explicite.
  *(Session 3 : la table partagée est faite, cf. §9.9.)*

### 9.9 Session 3 : la mémoire de politique bat la vitesse brute

Quatre chantiers menés dans l'ordre de rendement attendu ; chaque affirmation ci-dessous est
adossée à une mesure A/B (même graine, même budget, même machine), les surprises comprises.

**NRPA stabilisé et partagé — le gain dominant de la session.** Trois changements conjoints dans
`RunNrpa` :

- *Persistance partielle de la politique* (`--nrpa-keep`, défaut 0,5) : au redémarrage, les poids
  sont atténués puis purgés (|w| < 0,01) au lieu de repartir de zéro — les sous-lignes apprises
  survivent, la diversité revient par l'échantillonnage.
- *Meilleure séquence GLOBALE* (`NrpaShared`, mutex pris une fois par itération de niveau
  supérieur) : chaque redémarrage s'ensemence de la meilleure ligne connue de TOUS les workers ;
  un worker publie ses améliorations et n'adopte la ligne globale qu'en stagnation (≥ 4 itérations
  sans progrès), pour ne pas écraser la diversité.
- *Graine dérivée du temps par défaut, imprimée* (`--seed` la rejoue) : l'ancienne constante
  faisait de chaque relance le même run.

Mesure sur la transplantation `test 4`, à graine et budget IDENTIQUES à la baseline
(graine 2611923443488327891, 90 s) : la baseline plafonne à **7/8, 0 ligne** ; le moteur
stabilisé atteint **8/8 avec 76 lignes complètes dans les 52,5 s de la phase tirages**, meilleure
ligne **15 brûlées / 48 actions / 221 décisions** — mieux que le 16/49/224 que la session 2 avait
payé 530 s sur la seule graine chanceuse de quatre. Contrôle sur une seconde graine (12345,
90 s) : **8/8 encore, 192 lignes, meilleure 13/45/208**. Le 8/8 n'est plus un coup de dés : c'est
la mémoire de politique qui manquait, exactement là où le §9.5 la désignait.

**Débit NRPA : l'hypothèse ×2 sur les allocations ne s'est PAS réalisée — mesuré.** Tout le
chemin chaud a pourtant été dégraissé : `EnumerateInto` + `ChoiceList` (pool de `Choice` réutilisé,
labels construits à la demande — `EnumOptions.labels`, le drapeau `Choice::phase` remplace le
préfixe `->` des labels), `QueryCodes` (les atomes et la garde ne décodent plus de `QueriedCard`),
variantes sans allocation de `Messages`/`Query`/`ComputeBoardKey`/`StateDigest`, politique passée
par référence constante (elle était COPIÉE à chaque tirage). Résultat : ~90 k états/s avant comme
après — le coût par décision est dans ocgcore (`Process` + requêtes), pas dans l'hôte. Le
nettoyage reste (il allonge les tirages à états/s constant et débarrasse le profil), mais le
levier de débit suivant est côté core, pas côté allocations.

**Table de transposition partagée (lazy SMP, `--tt-mb`, défaut 64 Mo par passe).** Slots
`atomic<uint64_t>`, tag 48 bits + budget 16 bits, écrasement lossy, minimum 1 Mo (en deçà, des
bits du digest ne participeraient ni au tag ni à l'index). Sémantique identique aux tables
privées : l'entrée s'écrit avant l'exploration (marqueur « pris »), un timeout peut donc perdre un
sous-arbre réclamé — lossiness assumée. Mesure sur la réparation même-deck (90 s) : à k=8,
**376 514 états / 38,7 s → 143 321 / 32,3 s** à couverture égale ; les « 80 solutions » par
passe (16 workers × 5 doublons) deviennent 5 solutions distinctes ; meilleur coût conservé
(19/56/272) ; contrôle A/B de la nouveauté inchangé.

**Réparation sémantique — et le diagnostic du §9.5 corrigé par la mesure.** Deux mécanismes :

- *Resynchronisation exacte* : `LiftRefLine` relève une fois le digest de l'état avant CHAQUE
  réponse de la référence (153 ms) ; pendant `DescendRepair`, un état dont le digest est celui
  d'un point PLUS LOIN de la ligne y reprend le suffixe enregistré. Toujours actif — une
  recherche de table par nœud.
- *Répertoire fenêtré* : après une première déviation, un coup que la référence joue à ± 24
  décisions du point courant est GRATUIT — une permutation locale coûte UNE déviation, pas une
  par décision réordonnée. Réservé à la réparation SOUS contraintes : sans contrainte, il dépense
  le budget en permutations de coût égal et divise par deux la profondeur k atteinte (mesuré :
  k=4 contre k=8 à 90 s) ; jamais sur le préfixe pur (l'invariant « 0 écart retrouve la
  référence » tient par construction).

La surprise : sur le cas canonique « échanger les invocations #4/#5 » (`--summon "5:Zalen|Crystal
Wing"`), même avec fenêtre et resynchronisation, k=1 s'ÉPUISE à 3 258 états et k=2 à 4 661 —
**zéro solution, et c'est une preuve d'absence, plus un échec d'alignement**. (La baseline
« s'épuisait » aussi, mais son modèle de coût ne savait même pas EXPRIMER une permutation — une
déviation par décision réordonnée ; le nouveau modèle explore l'espace des permutations fenêtrées
et l'épuise quand même.) La cause est une question de DISPONIBILITÉ, pas d'ordre : à l'invocation
#4 (décision ~52), Junk Synchron n'est pas encore en main (`summon=0` à l'idle #52) — il y arrive
par l'activation intermédiaire (idle #61, le chercheur). L'« échange adjacent » du §9.5 n'existe
pas dans le jeu ; la conformité au summon #5 exige une restructuration d'ouverture — précisément
ce que NRPA avait trouvé (229 décisions). Le répertoire fenêtré reste le bon outil pour les
permutations légales ; l'exemple qui a motivé sa construction n'en était pas une. Les runs
contraints affichent leurs resynchronisations (`N resync` sur la ligne de passe) ; zéro resync +
espace épuisé = la permutation n'existe pas.

**Vérifié au passage** : le budget de la réparation était déjà global (chaque passe reçoit
`solve_ms − dépensé`) ; rejeu de référence 0 `MSG_RETRY`, couverture 289/290, 0 fusion de digest,
largeur inchangée (115 atomes, patience 21) après tout le refactor.

**La question ouverte du §9.7 reste ouverte — mais le blocage a changé de nature.** `test 3.ydk`,
board édité (−Hot Red −Crimson, +Naturia Beast +Chaos Angel@LUMIÈRE), discipline complète,
indices : 480 s, 566 k tirages NRPA / **38,8 M états** (persistance et partage actifs, ~8× la
couverture des runs de la session 2), indices légaux dans 228 901 états et pris 36 241 fois — et
toujours **6/8, les MÊMES manquants (Naturia Beast, Chaos Angel)**. L'échantillonnage n'est plus
en cause à ce budget. La mesure neuve est ailleurs : la meilleure approche (139 décisions, garde
tenue, écrite en `best_approach_6of8.yrp`) est un état d'où **le finisseur s'épuise à 6 états en
0,0 s** — tout meurt immédiatement sous la garde à cette position. C'est la mesure qui rend le
« finisseur à K états et à recul » prioritaire pour la session suivante : fouiller depuis les K
meilleurs états DISTINCTS, et depuis `best_path` tronqué de N décisions — là où l'espace n'est
pas encore verrouillé.

**`--opp-hand` : donner un objet à la discipline sur un départ hand test.** Le juge a montré que
la ligne 13/45/208 de `test 4` ne satisfaisait la garde que PAR VACUITÉ : dans ce hand test,
l'adversaire n'a que 2 cartes (miroir du deck), donc `mainadv<=2` éteint la menace dès le départ,
et aucune fenêtre adverse ne s'ouvre. `--opp-hand "c|c|..."` AJOUTE des cartes à la main adverse
du duel de départ (via `OCG_DuelNewCard` en `LOCATION_HAND`, après le `ReloadFieldEnd` du hand
test). Deux points non négociables : il faut des cartes JOUABLES (3× Nibiru — le core n'ouvre les
fenêtres de chaîne que si l'adversaire peut répondre, et des briques inertes laisseraient la
garde invérifiée) ; et les replays produits ne se rejouent qu'avec le même `--opp-hand` (les rips
de Trishula/Omega piochent dans cette main : l'état diverge sans elle) — le mode juge l'applique
donc aussi, et la vérification avant écriture rejoue le MÊME duel augmenté. En mode `--solve`, le
duel principal (la référence enregistrée SANS ces cartes) n'est jamais augmenté.

**Première mesure sous discipline complète sur `test 4`** (garde + `mainadv<=2` + no-activate +
Omega×2 + Trishula, 3 Nibiru en main adverse via `--opp-hand`) : à 300 s, 7/8 ; à 600 s avec
`--hint` Crimson Dragon, **8/8 codes réunis mais PAS une solution** — deux positions fausses
(Dis Pater et Crystal Wing en DEF au lieu d'ATK, le choix se verrouille à l'invocation) et le
handrip incomplet au meilleur état. Le juge sur `best_approach_8of8` (241 décisions) : **36
fenêtres adverses sous menace, toutes couvertes**, zéro activation interdite — la garde a enfin
un objet sur ce départ, et la ligne la tient. Le finisseur s'épuise encore en quelques états
depuis le meilleur état : troisième cas mesuré (test 3 : 6 états ; ici : 6 états à 300 s) où le
verrou est la CONVERSION du meilleur état, pas l'échantillonnage — le finisseur à K états et à
recul est le levier suivant, trois mesures le désignent.

**`--resolve` filtre désormais la zone d'ACTIVATION (`carte[@zone][:n]`) — un faux positif payé
sur ce jugement.** Le compte affichait « Omega 1/2 » sur `best_approach_8of8` : c'était l'effet
de CIMETIÈRE d'Omega, pas le handrip — qui s'active du terrain. Le joueur l'a vu ; le compteur,
non. `MSG_CHAINING` porte la zone d'activation (`triggering_location`, offset 15,
processor.cpp:3700) : chaque `ResolveReq` porte un masque de zones (0 = toutes, l'ancien
comportement), appliqué partout où l'on compte — recherche (`StepToPrompt`), gradient
(`ResolveProgress`), verdict de la référence, juge, vérification avant écriture, préfixe du
finisseur. Re-jugé : `best_approach_8of8` tombe à **Omega@terrain 0/2, Trishula@terrain 0/1**
(le handrip n'avait pas commencé) ; la référence reste conforme (2/2, 1/1 — ses activations
sont bien du terrain). Conséquence pour les runs passés : le gradient `--resolve` du run à 600 s
récompensait le faux positif ; la discipline complète sur `test 4` est à re-mesurer avec
`Omega@terrain:2` et `Trishula@terrain`.

**Non fait, et documenté comme tel** : finisseur à K états et à recul ; partage de la POLITIQUE
entre workers (seule la meilleure séquence l'est) ; Rollout-IW avec arbre explicite ; IW(2).

**Revue de littérature (fin de session 3).** Chaque verrou mesuré a sa réponse publiée, vérifiée
sur arXiv — la table complète (12 papiers, IDs) est dans `docs/next-session-prompt.md`. Les trois
appariements décisifs : le finisseur mono-état ↔ archive Go-Explore (2004.12919, notre restauration
à 0,05 ms est le « retour » gratuit) + Levin Tree Search sur les poids NRPA (2103.11505, garanties
d'expansions ; √LTS 2412.05196 = notre « recul » avec garanties) ; nos réglages NRPA artisanaux ↔
GNRPA à répétitions limitées (2401.10420), adaptation lente niveau 1 et multicritère lexicographique
(recette Montparnasse, 2505.02110/2606.07562), prior par rejeu de solutions (2401.10431 — notre
corpus de replays vérifiés dort sur disque) ; notre Rollout-IW abandonné ↔ π-IW avec arbre
(1904.07091) et IW hiérarchique (2101.06177). Aucun solveur de combo Yu-Gi-Oh publié n'existe
(2603.02863 ne traite que l'indécidabilité du problème général) : le territoire d'application est
vierge.

### 9.10 Session 4 : le finisseur multi-états — archive Go-Explore + Levin Tree Search

**Ce qui est construit.** Le finisseur mono-état (fouille guidée du seul meilleur état) est
remplacé par un finisseur archive + LTS, en quatre pièces :

- *Archive Go-Explore* (arXiv:2004.12919, `--archive-k`, défaut 16) : pendant TOUTES les passes
  (sonde, tirages gloutons, NRPA, LDS), chaque stratégie propose ses états à une archive de K
  cellules — cellule = hash du board complet (codes, positions, matériaux, compteurs), score =
  `overlap<<40 | résolutions<<32 | ~décisions` — chemin compris. La proposition n'est faite
  qu'APRÈS les contrôles de garde et de tour (un état non conforme est un départ condamné), et le
  cas courant coûte une comparaison d'entiers (plancher de score). Les archives des workers
  fusionnent par cellule, la meilleure gagne.
- *Politique NRPA exportée et fusionnée.* Les poids `plan_key` appris mouraient avec le run ;
  chaque worker NRPA exporte désormais sa politique finale, et la moyenne des poids (logits de
  même échelle) est servie au finisseur.
- *`RunLevin`* (arXiv:2103.11505) : depuis chaque racine, recherche best-first COMPLÈTE ordonnée
  par le coût de Levin `log(d(n)+1) − log π(n)`, où π est le produit des softmax des MÊMES logits
  que les tirages (poids + biais répertoire + biais indices). Un nœud = un prompt à choix
  multiple ; les coups forcés (fenêtres adverses, sélections à candidat unique) sont joués en
  ligne et ne coûtent ni profondeur ni probabilité. Transposition par digest (en best-first, la
  première visite est la moins chère). Le rejeu d'un nœud passe par une *pile de plongée* :
  chaque nœud développé pousse un niveau d'arène ; un nœud extrait dont le parent est dans la
  pile se rejoue en une restauration + une réponse (enfants ET frères), sinon rejeu complet
  depuis la racine. Piège d'arène payé à la relecture : `Pop()` restaure PUIS dépile — jamais
  `Discard()`, qui ne verse pas les pages sales du fils dans le parent et corromprait la
  restauration suivante. Coût mesuré : ~6-12 ms par expansion sous 16 workers concurrents,
  borné par le rejeu (Process), pas par la file.
- *Racines* : archive triée par score, reculs {15, 30} des 3 meilleures entrées, meilleur chemin
  global et ses reculs {5, 10, 20, 40, 60, 90} (l'approximation praticable du re-rooting √LTS,
  arXiv:2412.05196), et les approches des sessions passées (`--approach`, reculs
  {0, 10, 30, 60, 90, 120}). `--finisher mono|levin|ab` (ab = les deux à budget égal, la mesure).

**L'A/B qui l'a jugé — et le verrou généralisé.** Sur les deux cas qui ont motivé le chantier,
même run, budget égal (`--finisher ab`, graine 777, 600 s) :

| cas | tirages (406 s) | mono | levin |
|---|---|---|---|
| `test 4` discipliné (gradient corrigé) | 7/8, 45,5 M états | **6 états, épuisé, 0 ligne** | 32 racines, ~42 k expansions (max 11 291 sur `arch00 recul 30`), 0 ligne |
| `test 3.ydk` board édité | 6/8, 45,7 M états | **6 états, épuisé, 0 ligne** | 33 racines, ~100 k expansions, 0 ligne |

Le « 6 états » du mono est tombé une quatrième et une cinquième fois — et la mesure neuve est
qu'il n'est PAS une singularité du meilleur état : dans tous les runs, TOUTES les cellules
d'archive de la crête (6-8/8) s'épuisent en 1 à 8 expansions. Le verrou se pose bien avant l'état
final ; seuls les préfixes de recul ouvrent de vrais espaces (500 à 15 000 expansions). Le
finisseur multiplie la couverture du dernier pas par ~10³-10⁴ à budget égal — et ne convertit
toujours pas ces deux cas : les cartes manquantes (Hot Red Abyss sur `test 4` à cette graine ;
NB/CA sur `test 3`) ne sont pas à un « dernier pas » de la crête, elles demandent une
restructuration que même 15 000 expansions ordonnées par la politique n'atteignent pas en ~70 s
par racine. Un recul EPUISÉ est en revanche une preuve d'absence par racine — le mono ne savait
même pas la formuler.

**`--approach` : l'archive qui persiste entre les runs — et le piège de l'en-tête.** Les
`best_approach_*.yrp` des sessions passées entrent comme racines. Mesure qui a coûté une
correction : l'approche 8/8 de la session 3 (`test 4` discipliné) se JUGE parfaitement
(239/239, 0 retry) mais meurt à 39/239 rejouée sur le duel de départ du run — l'en-tête yrp1
écrit par `WriteYrp1` (pseudo-mélange) ne reconstruit pas exactement le duel du hand test
`--start`, et la divergence est silencieuse jusqu'à ce qu'un ordre de deck compte. Sur un départ
`--deck` (synthétique des deux côtés), les approches se rejouent telles quelles — mesuré sur
`test 3`. Conséquence architecturale : les racines d'approche s'enracinent sur LEUR duel (celui
de leur en-tête, augmenté du même `--opp-hand`), et leurs solutions s'écrivent contre LEUR
en-tête — vérification avant écriture comprise. Piège n°21, en somme : *une approche se rejoue
sur le duel de son fichier, pas nécessairement sur le duel de départ qui l'a produite.*

**Verdicts des runs disciplinés (gradient `--resolve` corrigé).** `test 4`, 600 s : graine 777 —
crête 7/8 (manque Hot Red Abyss, Stardust en trop) ; graine 888 — crête 8/8 aux tirages, l'approche
8/8 rejouée sur son duel s'épuise en 10 expansions à recul 0 (verrouillée, comme prévu), ses reculs
10-120 fouillent 7-15 k expansions chacun sans convertir, et les reculs les plus profonds
redescendent à 4-6/8 dans leur budget (~70 s) : reculer plus loin exige de remonter plus haut, et
70 s de LTS ne suffisent pas à re-monter 90 décisions. Pas de ligne complète disciplinée à ce
budget ; l'échec est localisé — positions et handrip se jouent dans la fenêtre [recul 10, recul 60]
de l'approche 8/8, avec ~15 k expansions consommées sans but. `test 3`, 600 s, graine 777 : 6/8,
mêmes manquants — la réponse d'une session sur l'autre ne bouge plus : c'est une question de jeu
(routes du joueur : NB = Junk Meister + Scrap Synchron) plus qu'une question de recherche, ou un
budget d'un autre ordre de grandeur.

**GNRPA à répétitions limitées : mesuré, REJETÉ.** `--nrpa-lr` implémente l'arrêt de niveau après
R re-trouvailles de la meilleure séquence (arXiv:2401.10420), la re-trouvaille étant l'égalité du
score MATÉRIEL (la part nouveauté décroît à chaque rejeu, l'égalité stricte n'arrive jamais). A/B
sur la transplantation `test 4` sans discipline, 90 s, même binaire : graine 2611923443488327891 —
baseline (stagnation à 8) **8/8, 36 lignes, meilleure 14/46/222** ; R=2 — **7/8, 0 ligne**.
Graine 12345 : baseline 6/8, R=2 7/8, aucune conversion des deux côtés. L'arrêt précoce casse la
convergence que la stagnation laissait aboutir : défaut remis à 0, le drapeau reste pour
re-mesurer (R plus grand, niveau 1 seulement).

**La reproductibilité a une limite mesurée.** À graine FIXÉE, les runs ne sont pas bit-à-bit
reproductibles : le partage de la meilleure séquence entre workers dépend de l'ordonnancement
(mutex, budgets temps). Même graine 2611923443488327891, même budget 90 s : session 3 = 76
lignes, session 4 = 36 lignes (8/8 les deux fois) ; graine 12345 : session 3 = 8/8 et 192
lignes, session 4 = 6/8 aux tirages. Une comparaison mono-run ne tranche donc que les écarts
FRANCS (8/8 contre 7/8, 6 états contre 11 000 expansions) ; les écarts fins demanderaient des
répétitions.

**`--no-chain` — la connaissance du joueur retire des branches que rien ne mesurait.** Demande du
joueur en cours de session : les effets de Zalen et Crystal Wing ne doivent JAMAIS annuler
l'activation de nos propres cartes. En solitaire, toute fenêtre de chaîne répond à nos propres
actions (l'adversaire passe toujours) : chaque fenêtre où un garde POURRAIT chaîner sa négation
multipliait des sous-arbres inutiles par construction. `--no-chain <carte>` élague le choix à
l'ÉNUMÉRATION des fenêtres de chaîne (« ne pas chaîner » demeure, les déclencheurs FORCES sont
exempts, les commandes idle aussi), et `ResponseForbidden` rattrape la réponse enregistrée à coût
zéro du mode réparation. Invariants inchangés avec le drapeau actif (0 retry, 290/290, référence
retrouvée à 0 écart). Et la mesure est le plus gros gain de la session : `test 4` discipliné,
600 s, graine 888 — SANS `--no-chain`, crête 8/8 avec Quetzacoatl ET d'autres détails faux ; AVEC,
la meilleure approche (226 décisions, `best_approach_8of8.yrp`) colle à la cible **à UNE position
près** (Quetzacoatl en DEF au lieu d'ATK — tout le reste est exact : Hot Red Abyss posé, Dis
Pater/Crystal Wing en ATK, Junk Signal face verso). Les racines de recul touchent 8/8 partout
(recul 20 : ÉPUISÉ à 287 expansions — preuve que le flip de Quetza n'est PAS à moins de ~20
décisions de la fin ; recul 40-90 : 2 700-6 600 expansions, budget épuisé avant conversion).
L'élagage demandé par le joueur a fait plus que tous les réglages de politique de la session.

**Conversion dédiée (`--finisher-min`) : la preuve d'absence au pas près.** Le juge sur la
nouvelle approche dit ce qui manque vraiment : garde 32/32 tenue, 0 activation interdite, mais
**Omega@terrain 0/2 et Trishula@terrain 0/1** — le suffixe de conversion doit contenir les TROIS
résolutions du handrip EN PLUS du flip de Quetzacoatl. Run dédié (600 s, graine 888,
`--finisher-min 300000` : les tirages cèdent le budget au finisseur quand `--approach` fournit
déjà les racines ; les deux approches 8/8 en racines) : `appr0 recul 0/10/30` s'ÉPUISENT (1, 21,
2 402 expansions) — il n'existe AUCUNE complétion disciplinée à moins de 30 décisions de la fin
de la meilleure approche, les 30 dernières décisions sont structurellement engagées (les effets à
résoudre n'y sont plus disponibles) ; `recul 20` et les reculs 15 d'archive s'épuisent à 287 ; la
frontière est **[recul 30, recul 120]**, budget épuisé à 4 200-17 800 expansions par racine
(150 s chacune, ~8 ms/expansion). Si la ligne disciplinée complète existe, elle se joue là — et
il faut soit un ordre de grandeur d'expansions en plus, soit un meilleur ORDRE (PHS* : mettre la
distance au but — cartes manquantes, résolutions manquantes, déjà calculées à chaque expansion —
dans le coût ; arXiv:2103.11505 couvre LTS et PHS* avec les mêmes garanties), soit un prior qui
sait déjà ripper (2401.10431).

**Deux arbitrages de jeu posés par le joueur en fin de session — et leurs conséquences.**

- *La position ATK/DEF ne compte pas* : deux boards qui ne diffèrent que par une position de
  combat sont le MÊME board. L'équivalence de but (`EntryOf` en `goal_view`) ignore désormais la
  position et ne garde que la FACE (recto/verso — un Junk Signal posé face verso reste distinct) ;
  le digest d'ÉTAT garde la position complète (une position différente est un état de jeu
  différent : la fusionner ferait disparaître des lignes sans le signaler). Conséquence
  immédiate : le « 8/8 à une position près » de `mD_t4_nochain` devient un board-but — il ne
  manque plus QUE le handrip ; et les tirages n'ont plus à payer le flip de Quetzacoatl.
  Invariants re-vérifiés après le changement : 0 retry, 290 digests distincts, référence
  retrouvée à 0 écart, meilleur coût même-deck conservé (19/56/272) — et 209 solutions même-deck
  au lieu de 46, l'équivalence élargie admettant plus de lignes.
- *Une carte « brûlée » n'est pas perdue* : Omega se récupère de la zone bannie via Dis Pater
  (c'est ainsi que la référence résout Omega deux fois). Cela INVALIDE les élagages « carte cible
  au cimetière/bannie sans copie restante = branche morte » qui étaient sur la table : ils ne
  seront PAS implémentés. La détection d'états morts, si elle revient, devra passer par les
  compteurs du core (ProcessorState), pas par des règles de zones.

**PHS\* et réglages associés (implémentés dans la foulée).** `RunLevin` porte désormais le coût
PHS\* `log(d+1) + levin_h · h(n) − log π(n)` avec h = cartes cibles manquantes + résolutions
manquantes au nœud développé (`--levin-h`, défaut 1,0 ; 0 = Levin pur — le Levin pur re-montait
les reculs profonds sans préférer les branches qui ripent). Le quart de workers gloutons est
réduit à un huitième (re-mesuré sur les trois runs disciplinés : crête 2/8 pour ~6 M états
pendant que NRPA fait 7-8/8).

**Le run complet sous les nouvelles sémantiques (600 s, graine 888, toutes contraintes,
`--finisher-min 300000`, PHS\*, deux approches).** Trois faits :

- `appr0 recul 0` : **1 expansion, best 8/8, épuisé** — le board final de la meilleure approche
  EST un board-but sous la nouvelle équivalence ; il ne manque plus QUE le handrip.
- `appr0 recul 30` : épuisé à **2 402 expansions — le même compte exactement que sous l'ancienne
  équivalence** (mE). La preuve d'absence ≤ recul 30 est donc INVARIANTE au changement
  d'équivalence : la position n'a jamais été le verrou sous recul 30, le handrip est le seul
  manquant, et il n'est plus jouable dans les 30 dernières décisions.
- Pas de conversion dans [recul 30, 120] à 150 s/racine, PHS\* compris (11-16 k expansions par
  racine). La raison est structurelle : le suffixe requis n'est pas un « dernier pas » — il faut
  INVOQUER Omega (il n'est pas sur le board final), ripper, le récupérer via Dis Pater, re-ripper,
  invoquer Trishula (son rip d'invocation), puis refermer le board : c'est la fin de ligne de la
  référence elle-même, ~60-120 décisions. Les tirages NRPA ne l'échantillonnent jamais malgré le
  gradient (+100 par résolution, poids d'une carte cible) : la politique ne SAIT pas ripper —
  c'est l'argument le plus concret à ce jour pour le prior par rejeu (2401.10431, la référence et
  le corpus contiennent ces séquences), et pour des TIRAGES NRPA enracinés sur les états de recul
  (RunNrpa ne prend pas encore de compteurs initiaux — l'échantillonnage profond bat la
  recherche systématique à ces profondeurs). Effet de bord mesuré de `--finisher-min` : tirages
  plafonnés à 280 s → crête 7/8 seulement (sans conséquence ici, les approches portent le 8/8).

**La campagne de conversion (suite de session, sur demande du joueur : « jusqu'au bout »).**
Mécanismes ajoutés dans la foulée, chacun mesuré par le run suivant :

- *Faisabilité des `--resolve`* : la carte à résoudre doit EXISTER dans le deck de départ, sinon
  la contrainte est insatisfiable et la recherche s'annule avec un verdict définitif. Sur
  `test 4` : Omega ×1 copie (les 2 activations passent par la récupération — la route Dis Pater
  du joueur), Trishula ×2 — une ligne PEUT exister.
- *Tirages NRPA enracinés* : `initial_summons/turns/resolved` câblés dans
  `PolicyRollout`/`Rollout`, politique initiale (`nrpa_init` = la politique fusionnée de la
  phase tirages), meilleure séquence partagée PAR RACINE. Le finisseur devient LTS→NRPA : les
  reculs courts passent au LTS (épuisement = preuve), les profonds à l'échantillonnage.
- *Biais automatique des cartes `--resolve`* (canal `--hint`), *sérialisation de la nouveauté
  par résolutions* (la table se rouvre à chaque rip), *histogramme de résolutions par tirage*
  (LE diagnostic), *poids du gradient de résolution* (`--resolve-weight`, défaut porté à 250).
- *Correctif réel découvert en route* : `Choice::card` n'était pas renseigné pour
  `MSG_SELECT_CHAIN` — les effets RAPIDES (le rip d'Omega s'active en fenêtre de chaîne)
  échappaient au biais des indices depuis toujours ; les invocations étaient biaisées, jamais
  les activations en chaîne.

**Les mesures de la campagne, dans l'ordre.** (1) Run enraciné à gradient 100 : 850 k tirages /
27 M états depuis les reculs 50-150 de l'approche — tous à 8/8, **zéro rip** : à poids égal
(rip = une carte cible), les lignes 8/8 muettes gagnent la course d'adaptation. (2) Run pleine
ligne à gradient 250 : 2,06 M tirages / 147 M états — **130 721 tirages font ≥ 1 rip, 7 530
font ≥ 2, 2 170 font les 3 résolutions exigées**. La politique sait ripper ; aucune ligne ne
rippe ET ne referme (encore). (3) Le même run, phase enracinée : **le point de non-retour du rip
est localisé** — à recul 50 de l'approche, 0 rip sur 45 k tirages (la séquence n'est plus
jouable) ; à recul 80, ~11 500 tirages rippent et ~2 200 font les 3 — la fenêtre de conversion
est [recul 50, recul 80], décisions ~146-176 de l'approche. C'est le résultat le plus fin de la
session : le solveur dit désormais OÙ la ligne se joue, à trente décisions près.

**Fin de campagne : l'archive « résolutions d'abord », la crête rippée, et le verdict.** Deux
derniers mécanismes : sous `--resolve`, le score d'archive place les RÉSOLUTIONS avant les
cartes posées (sinon les 8/8 muets évincent tous les états rippés — mesuré) et la phase A2
s'enracine aussi sur les meilleurs états rippés de l'archive (duel de départ) ; et la mesure
`best_overlap_ripped` — la crête conditionnée aux résolutions COMPLÈTES. Verdict des deux
derniers runs (25-30 min chacun, graine 888) :

- les états d'archive à 2 rips plafonnent à 5/8 et ne produisent JAMAIS le 3e rip (0 sur
  ~500 k tirages enracinés) ;
- avec le score résolutions-d'abord, l'archive tient des états **r3 à 7/8** — les lignes qui
  font TOUT le handrip montent à UNE carte du board (`crête-rip 7/8`, 1 341 tirages complets
  côté pleine ligne) ; ~500 k tirages enracinés sur ces états (recul 0-40) ne trouvent jamais
  la 8e carte ;
- le board-BUT est atteint par ailleurs sans le handrip (approche 264 décisions, identique à la
  cible sous l'équivalence) — et aucune ligne ne fait les deux.

L'état du problème `test 4` en fin de session 4 : **les deux moitiés de la solution existent
séparément et se touchent presque** — 8/8 sans rips d'un côté, 3 rips + 7/8 de l'autre. Les
preuves : pas de complétion à ≤ 30 décisions de la fin de l'approche muette (épuisé, compte
stable ~2 400) ; point de non-retour de la séquence de rips entre recul 60 et 70 (0 ligne à
3 rips sur 49 k tirages à recul 60, ~4 000 par worker à recul 70) ; la 8e carte des lignes
rippées introuvable en ~500 k tirages depuis leurs états finaux. Les deux voies pour trancher,
par coût croissant : une RELAXATION arithmétique SMT/ILP des ressources jointes (conservation
des corps, tuners/niveaux, copies, arithmétique du handrip — son UNSAT serait une preuve
d'absence du jeu réel, la voie « SAT/SMT praticable » discutée avec le joueur : le jeu complet
n'est pas encodable, le moteur est la seule spécification) ; ou la ligne jouée à la main dans
EDOPro (elle se juge telle quelle et entre en `--approach`). Diagnostic à instrumenter au
préalable : QUELLE carte manque aux crêtes rippées (le board du meilleur état rippé n'est pas
encore capturé — seule la crête globale l'est).

**La simulation `test 3` du joueur (board NB + CA lumière + Quetza + Dis Pater + CW + Zalen +
Junk Signal posé, garde, double rip d'Omega) — et deux leçons payées comptant.** Verdict du run
(25 min, graine 888) : aucune ligne ; crête muette 6/8 (NB/CA manquants, comme depuis la
session 3) ; double rip accompli par 3 314 tirages, et **crête-rip 6/8 aussi** — sur `test 3`,
le rip n'est PAS le mur, la pose conjointe NB+CA l'est, rips ou pas. Le joueur a alors donné la
route de NB (Jet Synchron tuto Junk Meister → Starjunk Synchron invoque Scrap Synchron →
Scrap + Meister = NB) ; le run suivant l'a imposée par `--resolve` Jet + Starjunk, et :

- *Leçon 1 — la route tirait déjà* : Jet 2,44 M de fois, Jet+Starjunk 1,6 M — le verrou est le
  pas d'APRÈS : la POSE de Junk Meister (il s'invoque par son propre effet en révélant
  Stardust Dragon, Accel Synchron ou le 3e synchro — précision du joueur : l'invocation exige
  qu'un reveal soit encore disponible, un conflit séquentiel de l'extra deck).
- *Leçon 2 — ne JAMAIS mettre un pas trivial dans `--resolve`* : à +250 par unité quasi
  gratuite, la politique s'est effondrée sur la collecte de résolutions (crête 6/8 → 3/8,
  plus aucun rip). `--resolve` porte les événements RARES ; les pas que la ligne fait déjà
  détruisent le gradient. Piège n°31.

D'où `--summon-min "carte[:n]"` : la même machinerie (gate au but, gradient, biais, histogramme,
juge, vérification, préfixes) comptée sur les INVOCATIONS. Sa première mesure a d'ailleurs
retourné le diagnostic : **Meister se pose dans 1,6 M de tirages** — sa pose n'est pas le verrou
non plus (et l'événement, courant, a re-détruit le gradient : leçon 2 confirmée, crête 3/8). Le
verrou de NB est donc plus fin encore — Meister posé 1,6 M de fois, Scrap invoqué 1,6 M de fois,
NB jamais : la synchro Scrap+Meister→NB n'est probablement JAMAIS légale (coexistence ou
niveaux). Le diagnostic suivant était prêt (`--hint "Naturia Beast"` seul, lire `hint_seen`)
quand le joueur a réorienté la mission : **les questions `test 3`/`test 4` sont mises en PAUSE,
l'objectif redevient l'OPTIMISATION de la ligne de référence sur son propre deck** — cf. le
prompt de reprise régénéré.

**Non fait, et documenté comme tel** : capture du board du meilleur état RIPPÉ (la 8e carte
manquante n'est pas identifiée) ; relaxation SMT des ressources jointes ; prior par rejeu de
solutions (2401.10431 — remplacé en pratique par le trio gradient 250 + biais auto + politique
fusionnée, qui a suffi à faire ripper ; le corpus ne contient d'ailleurs AUCUNE ligne rippée
hormis la référence) ; adaptation lente niveau 1 (Montparnasse) ; partage périodique de la
POLITIQUE entre workers ; π-IW / IW(2) ; vérification du `--no-chain` par le mode juge ; LTS
parallèle à frontière partagée (les nœuds sont transportables par leur chemin de réponses — la
piste de débit du finisseur) ; LuaJIT côté core (2-10× plausible sur une charge dominée par les
scripts, risqué : luaconf porte l'allocateur d'arène et le hachage déterministe) ; A/B isolé de
`--levin-h`.

### 9.11 Session 5 : l'optimisation même-deck — coût lexicographique anytime (`--optimize`)

**La mission réorientée** (fin de session 4) : trouver une route moins chère que la référence
`synchron handrip 2` sur son PROPRE deck — même main, même board, coût lexicographique
(brûlées, puis actions, puis décisions), discipline complète tenue. La borne à battre :
**19/56/273** (le répertoire même-deck connaît 19/56/272).

**Chantier 1 — le raccourci `--start` = référence : il marche, sans une ligne de code.** Le
dispatch passait déjà `RunTransplantSolve(duel, ref, start, ...)` avec start quelconque, et le
chemin start = ref existait (secours `ref_violates`) ; l'écriture des solutions passe par le
MÊME en-tête que `RunSolve` (le hand test de la référence se reconstruit, contrairement à celui
de `test 4` — piège 21 sans objet ici). Run de validation (600 s, graine 888, discipline
complète, `--approach solution_00`) : sonde 2/8 (l'heuristique gloutonne ne monte pas sous
discipline), NRPA **8/8** (832 k tirages, 55,7 M états), finisseur : `appr0 recul 0/10` →
BUT (épuisés — l'approche EST une solution), 5 lignes à 19/56/272 écrites et vérifiées. La
mesure qui gouverne la suite : **1 tirage sur 832 000 fait les 3 résolutions exigées** — le
verrou d'échantillonnage de la discipline (Omega×2 + Trishula), le même que `test 4`, mais ici
un répertoire de lignes complètes disciplinées existe déjà : tout le levier est dans les
racines-solutions.

**La marge de récupération se mesure — nouvelle ligne du rapport de rejeu.** Le long de la
référence, les brûlées piquent à **23 en cours de ligne pour finir à 19** : marge de
récupération 4. C'est la mesure qui calibre la borne B&B (`--burn-slack`, défaut 6) — les
brûlées ne sont PAS monotones (piège 27, les récupérations jouent dans les deux sens), une
borne sans marge couperait la référence elle-même.

**Chantier 2 — `--optimize`, l'objectif de coût anytime (générique).** Sept mécanismes,
tous derrière un drapeau (comportement d'avant inchangé sans lui, invariants re-vérifiés) :

- *Score de but NRPA lexicographique* : `1e12 − brûlées·1e9 − actions·1e5 − décisions`
  (unités disjointes, entiers exacts en double) — l'adaptation tire vers la ligne la moins
  CHÈRE, plus vers la première venue (la recette Montparnasse, 2505.02110).
- *Anytime* : plus d'arrêt à `max_solutions` — l'ensemble par worker est borné par
  remplacement du PIRE (clé lexicographique empaquetée), dedup par hachage de chemin (une
  politique convergée rejoue la même ligne des milliers de fois).
- *Poursuite APRÈS le but* — tirages ET recherches systématiques : le board atteint n'est
  plus terminal, des décisions de plus peuvent RÉDUIRE les brûlées (un effet de cimetière qui
  remélange au deck ne touche pas au board) ; chaque ré-atteinte ré-enregistre si moins chère.
  Cette classe de lignes était structurellement introuvable avant.
- *Borne B&B brûlées* : un état qui brûle plus que `meilleures_brûlées + burn_slack` meurt
  (tirages seulement ; la marge absorbe les récupérations mesurées). `--burn-limit` ensemence ;
  la borne se resserre à chaque amélioration, et le finisseur hérite de la meilleure des
  tirages.
- *Archive par coût* : sous `--optimize`, les brûlées s'insèrent dans le score de cellule
  avant le chemin court (`résolutions<<48 | overlap<<40 | ~brûlées<<32 | ~décisions`) — les
  racines utiles sont « board proche + coût partiel BAS ».
- *Racines = solutions les moins chères* : les préfixes des 3 meilleures solutions entrent au
  finisseur (LTS, reculs 30-120) et les reculs profonds des 2 meilleures au NRPA enraciné
  (reculs 60-150) ; les portes `found >= 4` et « finisseur seulement si aucune solution »
  sautent — en optimisation, chaque racine restante peut porter une ligne moins chère.
- *LDS sous `--optimize`* : bornes relâchées (+8 actions, +48 décisions — les bornes
  ≤ référence interdisaient exactement les lignes qui récupèrent) et passes qui ÉPUISENT au
  lieu de s'arrêter à 16 variantes de coût égal. La mesure qui a montré la nécessité : chaque
  passe k de la santé s'arrêtait à 330 états / 0,2 s — « résiste à 12 déviations » était un
  arrêt prématuré, pas une preuve.

**Invariants après le refactor : tous verts.** 0 retry, 290/290 digests distincts, 0 fusion,
« à 0 écart la référence est retrouvée », A/B nouveauté sans perte, meilleur coût conservé.

**La preuve de résistance locale (LDS-optimize, 900 s, discipline complète, bornes relâchées
64 actions / 321 décisions).** k=0 : 5 solutions (les ré-atteintes le long de la référence —
aucune récupération gratuite sur son propre suffixe de fin de tour). Puis l'épuisement,
chiffré : **k=2 — 41 099 états, ÉPUISÉ (18 s) ; k=3 — 156 797 états, ÉPUISÉ (48 s) ; k=4 —
959 034 états, ÉPUISÉ (253 s)** ; k=5 — 2 332 513 états, budget épuisé à 550 s (incomplet).
149 solutions distinctes, toutes à 19/56/272. **Aucune ligne sous 19 brûlées à ≤ 4 déviations
de la référence** — avec répertoire fenêtré, resynchronisation et poursuite d'après-but.
(Le run de contrôle à 60 s donnait les mêmes zéros à k≤3 mais ses passes étaient tronquées
par le budget total — c'est le run à 900 s qui fait foi sur les épuisements.)

**A/B ± `--optimize` sur le flux transplantation (graine 888, 600 s chacun).** Tirages
quasi identiques (758 k contre 832 k tirages, 8/8 les deux) — mais 27 tirages à ≥3 rips contre
1, et le finisseur change de nature : les reculs PROFONDS d'approche convertissent (`appr0
recul 60` → BUT, crête-rip 8/8 ; 3 064 tirages enracinés suffisent), 25 lignes disciplinées
contre 5. Les racines rippées-tôt de l'archive plafonnent à 4-6/8 sans refermer (le motif
« deux moitiés » de test 4, en plus doux). **Toutes les lignes restent à exactement
19/56/272.**

**Le run corpus (4 approches, `--finisher-min 420000`, graine 999, 600 s) : première
amélioration réelle — 19/56/263.** Les tirages cèdent le budget (160 s, crête 7/8) ; le
finisseur porte tout : `appr0 recul 80` → BUT (crête-rip 8/8, 6 442 tirages enracinés),
`recul 110/150` touchent 8/8, les reculs courts 0/10 s'épuisent en re-trouvant l'approche.
**25 lignes disciplinées, les meilleures à 19 brûlées / 56 actions / 263 décisions — 9 de
moins que tout le répertoire connu (272), 10 de moins que la référence (273).** Jugement
indépendant depuis zéro : 263/263 rejouées, 0 retry, garde 30/30 fenêtres couvertes, 0
activation interdite, Omega@terrain 2/2, Trishula@terrain 1/1.

**L'escalade lexicographique — chaque run s'enracine sur la meilleure ligne du précédent, et
descend.** Le grand run (1 800 s, graine 888, `--finisher-min 1200000`, `--burn-limit 19`,
`--archive-k 24`, approches = la ligne 263 + deux du corpus) : tirages 8/8 (1,05 M tirages,
72,5 M états), et le finisseur convertit depuis QUATRE reculs profonds (`appr0 recul 60/90` et
`appr1 recul 60` → BUT, crête-rip 8/8 — les fenêtres de conversion mesurées sur test 4 valent
ici aussi). Résultat : **19 brûlées / 55 actions / 261 décisions — une ACTION de moins que
tout ce qui était connu.** Jugée depuis zéro : 261/261, 0 retry, garde 33/33, Omega@terrain
2/2, Trishula@terrain 1/1. La trajectoire 19/56/272 → 19/56/263 → 19/55/261 est une
hill-climb par redémarrages enracinés : le mécanisme générique (`--approach` sur la meilleure
solution + reculs 60-150 au NRPA enraciné) EST l'optimiseur. Les racines rippées-TÔT de
l'archive, elles, restent stériles (3/8 avec 3 rips, ~700 k tirages sans progrès — ripper
d'abord puis construire ne refait pas le board sur ce deck : l'ordre de la référence, rips en
FIN de ligne, semble structurel).

**L'itération de convergence (600 s, graine 777, approche = la ligne 261).** 49 lignes via
l'approche, QUATRE reculs convertissent (60/70/80/110, crête-rip 8/8) — et le meilleur coût
ne bouge plus : **19/55/261 confirmé sur une troisième graine, l'escalade a convergé** à ces
budgets.

**Le verdict de la session, en trois phrases.** (1) Une ligne strictement meilleure que la
référence au sens lexicographique existe et est LIVRÉE : **19 brûlées / 55 actions / 261
décisions** (`sF_final/solution_00_b19_a55.yrp`, écrite, vérifiée, jugée depuis zéro sous
tous les drapeaux — contre 19/56/273). (2) **19 brûlées n'a pas été battu**, et la résistance
est chiffrée : espace ÉPUISÉ à k ≤ 4 déviations de la référence (959 034 états, bornes
relâchées 64 actions / 321 décisions) ; k=5 incomplet à 2,33 M états ; ~3,5 M tirages NRPA
pleine ligne et ~6 M tirages enracinés sur trois graines (888/999/777) et quatre budgets
(600/600/900/1800 s), borne B&B armée — pas UNE ligne à 18, toutes les lignes complètes
tombent sur 19 exactement. (3) La voie restante pour trancher 18 est celle du §9.10 :
la relaxation arithmétique SMT/ILP des ressources (l'UNSAT serait une preuve d'absence),
ou une restructuration au-delà du rayon atteint (reculs > 150, budgets d'un autre ordre).

**Non fait, et documenté comme tel** (session 5) : affichage des compteurs `burn_cuts` /
`goal_hits` dans les rapports de phase (les stats existent, aucun printf ne les sert) ;
partage de la borne brûlées ENTRE workers pendant la phase tirages (chaque Search resserre la
sienne ; un atomique global la propagerait) ; racines d'approche au-delà de recul 150 ;
relaxation SMT des ressources (la voie de la preuve pour 18) ; A/B isolé de `--burn-slack`
(6 par défaut, marge mesurée 4 sur la référence — jamais stressée) ; LTS à frontière
partagée, LuaJIT, adaptation lente niveau 1 (inchangés du §9.10).

**Le test adverse (`--fire`, demande du joueur en fin de session) : la menace jouée POUR DE
VRAI.** La garde n'a jamais été qu'un proxy statique — « un contre est disponible à chaque
fenêtre ». Le mode `--fire <carte>` en fait la preuve dynamique : la carte (Nibiru) est
ajoutée à la main adverse, l'adversaire l'ACTIVE à chaque fenêtre où elle est légale (un
essai par fenêtre), et la recherche enracinée doit refermer le board depuis l'état
post-injection — board complet (contre gratuit par Crystal Wing) ou board sans la carte
sacrifiée (`--fire-spare "Junk Signal"` : contrer par Zalen consomme Junk Signal, arbitrage
du joueur ; `SearchConfig.target_alt` = second board accepté au but, `Solution.alt`).

Trois mécanismes de fiabilité, chacun payé par une mesure :

- *Rejeu par JOUEUR avec preuve d'alignement.* Ajouter une carte jouable ouvre des fenêtres
  adverses NOUVELLES (le core ne demande que si une réponse légale existe) — les réponses
  enregistrées se décalent. Le rejeu étiquette d'abord les réponses par joueur (nôtres /
  passes adverses), puis rejoue sur le duel augmenté : nos réponses dans l'ordre, le passe
  enregistré aux fenêtres ENREGISTRÉES, un passe synthétique aux fenêtres NOUVELLES (la carte
  tirée seule chaînable = exactement 2 choix). La passe de découverte doit refaire le board
  avec 0 retry AVANT toute injection — sur la référence : alignement prouvé, 212 réponses à
  nous, 64 passes adverses, 37 fenêtres où Nibiru est jouable (toutes nouvelles).
- *`no_chain` LEVÉ dans la continuation post-injection.* La règle du joueur (« Zalen/Crystal
  Wing ne chaînent jamais ») supposait le solitaire, où toute chaîne répond à nos propres
  actions ; chaîner sur la menace RÉELLE est précisément le rôle des gardes. Mesuré : avec le
  filtre hérité, le contre par Crystal Wing était interdit d'énumération — zéro conversion
  « board complet » ; levé, les fenêtres tardives convertissent en board complet.
- *Le piège de la réponse pendante — payé 3 runs.* La boucle de rejeu du préfixe sort avec la
  dernière réponse POSÉE mais non traitée (la convention d'entrée du finisseur) ;
  `SetResponse(inject)` l'ÉCRASAIT — l'injection se jouait au prompt d'AVANT, 13 fenêtres
  sur 37 se déclaraient « illégales » (artefact), et là où les octets passaient par
  coïncidence, la recherche partait d'un état décalé d'une réponse : 16/16 solutions en
  MSG_RETRY à l'écriture. Le diagnostic est venu d'un AUTO-CONTRÔLE ajouté au worker (rejouer
  le chemin assemblé sur son propre duel, imprimer l'indice de divergence et si la réponse
  figure parmi les choix énumérés à froid : « décalage de fenêtres » à +3 de l'injection).
  Le correctif : TRAITER la dernière réponse du préfixe (avancer jusqu'au prompt de la
  fenêtre de tir) avant de poser l'injection. Piège n°36, en somme : *une réponse posée non
  traitée est écrasée par le SetResponse suivant — avancer au prompt avant d'injecter.*

**Le verdict du run corrigé (30 s de recherche par fenêtre, graine 888).** 37 fenêtres
injectables, **8 converties : les deux dernières (déc. 211-212, inv. 31) en board COMPLET à
19/59 — Crystal Wing contre Nibiru sans rien consommer — et six (déc. 173-208) en board sans
Junk Signal à 20 brûlées** (JS consommé finit au cimetière : +1 brûlée, la chaîne de contre
coûte ~3 actions). 16 replays écrits, vérifiés sur le duel augmenté, et jugés depuis zéro
avec `--opp-hand Nibiru` : 279/279, 0 retry, Omega@terrain 2/2, Trishula 1/1.

**`--fire-bake` : des refermetures VISIONNABLES dans EDOPro.** Les replays du mode par défaut
ne se rejouent qu'avec `--opp-hand` (Nibiru est ajouté par `OCG_DuelNewCard` APRÈS la mise en
place — inexprimable dans un yrp1). La variante cuite insère la carte dans le DECK adverse là
où le pseudo-mélange sert la main (la queue de la liste, vérifié par la sonde `OpeningHand`) :
les fichiers produits se rejouent DEPUIS LEUR EN-TÊTE, donc EDOPro les visionne. Le prix,
`start_hand` étant partagé : la carte prend la place de la 5e carte de main adverse (déplacée
vers le deck) — le duel diffère d'une carte, la preuve d'alignement tranche (elle passe sur la
référence), et le handrip peut ripper Nibiru lui-même : les fenêtres tardives disparaissent
(29 fenêtres au lieu de 37, plus de conversions « faciles » post-rip). Verdict à 300 s/fenêtre
(graine 999) : **9/29 converties — 2 board complet (19/59), 7 via Zalen+JS (20/58-62)** — et
les fichiers écrits se rejouent depuis leur en-tête SANS drapeau (jugés : 265/265 et 280/280,
0 retry, résolutions tenues).

**La mise en scène du contreur (`--fire-no-chain`, `--fire-spare` répétable) — et ce qu'elle a
appris sur le jeu.** Demande du joueur : VOIR le negate par Zalen (Junk Signal chaîné sur
Nibiru, Zalen par-dessus). Trois faits mesurés en route : (1) le solveur ne choisit JAMAIS
cette voie spontanément — Crystal Wing et Dis Pater contrent gratuitement ; (2) forcer
`--resolve Zalen@terrain` seul est contourné — le solveur active Zalen AILLEURS (chaîne 30)
pendant que CW nège Nibiru (chaîne 15) : une contrainte de résolution ne dit pas OÙ résoudre ;
(3) muselé CW (`--fire-no-chain "Crystal Wing"` — no-chain propre à la continuation, les
`--no-chain` globaux y restant levés), la voie Zalen plafonne à 7/8 avec « manque : Crystal
Wing » ou « manque : Crimson Dragon » selon la fenêtre (diagnostic « manque : » ajouté au
rapport par fenêtre) — **le contre Zalen+JS coûte PLUS qu'une carte : Junk Signal est aussi
une pièce de construction, sa dépense casse la fin du combo.** Avec le but élargi
(`--fire-spare "Junk Signal" --fire-spare "Crystal Wing"`), la fenêtre 2 (déc. 65, inv. 7 —
un tir PRÉCOCE, le premier converti à ce jour) livre la séquence exacte demandée :
**Nibiru → Junk Signal → Zalen par-dessus → Omega**, 18 brûlées / 53 actions sur le board
amputé des deux cartes (non comparable au 19/55 du board complet), jugée depuis son en-tête
cuit : 245/245, 0 retry, Omega 2/2, Trishula 1/1, Zalen 1/1 (`sT_zalen3/`).

**Le verdict du joueur sur les premières refermetures — « frauduleuses » — et ce qu'il a fait
construire.** Toutes les conversions faciles injectaient Nibiru EN PLEINE CHAÎNE (par-dessus
Junk Speeder…) : un Nibiru chaîné se nège à bon compte et ne modèle pas l'adversaire réel —
**la vraie menace DÉMARRE une chaîne** (link 1). Trois instruments en réponse : (a)
classification des fenêtres par l'état de la chaîne (MSG_CHAINING/MSG_CHAIN_END —
« OUVERTE » / « sur \<carte\> ») et `--fire-open` qui n'injecte qu'aux chaînes vides — sur la
référence cuite : 15 fenêtres ouvertes sur 29, et TOUTES les conversions antérieures étaient
en pleine chaîne ; (b) le diagnostic « réponses à la menace » (énumération du premier prompt
post-injection, muselières marquées) ; (c) `--fire-spare` répétable → buts alternatifs = board
− chaque SOUS-ENSEMBLE des cartes sacrifiables (une ligne qui ne dépense que JS doit matcher,
comme une ligne qui perd aussi la pièce cassée par cette dépense), et le rapport « manque : »
par fenêtre.

**Ce que l'escalade de muselières a mesuré (fenêtres OUVERTES, 300 s/fenêtre).** Le deck a
QUATRE familles de réponses à un Nibiru ouvreur, que le solveur prend par coût croissant :
Crystal Wing (negate gratuit — 1 board complet converti), Bystial Dis Pater (gratuit — vole le
contre dès que CW est muselé, 5/15 converties), Junk Signal (partout : la SEULE réponse aux
fenêtres précoces, déc. 58-75), et PSY-Framelord Omega/Assault Zone ponctuellement. Zalen
n'est JAMAIS chaînable directement sur Nibiru — il ne vient QUE par-dessus Junk Signal
(link 3), exactement comme le joueur l'avait dit ; et une contrainte `--resolve Zalen` seule
se fait contourner (Zalen activé AILLEURS pendant que CW nège — mesuré). Avec CW et Dis Pater
muselés : le contre Junk Signal SAUVE la ligne aux fenêtres précoces (survivants à 7/8,
« manque : Crimson Dragon ») mais la refermeture complète bute sur le mur des rips (re-dériver
Omega×2+Trishula depuis la décision 58 — le verrou d'échantillonnage de toujours) ; les
fenêtres médianes (déc. 89-139) sont RASÉES sans les negates gratuits (~400 k tirages morts).
Une conversion authentique livrée : **fenêtre 14 (déc. 173, OUVERTE) — Nibiru ouvreur contré
par Junk Signal, board sans les sacrifiées, rips 2/2+1/1, jugée 286/286 depuis son en-tête
cuit** (`sY_zalen5/`). La séquence complète « JS depuis la MAIN + Zalen par-dessus » en
fenêtre ouverte ET refermée reste hors de portée des budgets essayés — les pièces existent
(chaînes JS→Zalen mesurées en pleine chaîne, réponse JS énumérée aux fenêtres ouvertes),
c'est la complétion des rips depuis si tôt qui manque. Levier suivant documenté : écrire la
MEILLEURE APPROCHE par fenêtre (le contre + la remontée à 7/8 se visionnent sans exiger le
but), ou des budgets d'escalade par fenêtre.

**La passe profonde (300 s par fenêtre, graine 999) — la frontière de faisabilité.**
**16 fenêtres sur 37 converties : 6 en board COMPLET (déc. 152, 157, 185, 190, 211, 212 —
19 brûlées / 58-59 actions, Crystal Wing contre et le combo se referme ENTIER, Junk Signal
posé compris) et 10 en board sans Junk Signal (20 brûlées / 58-62 actions, la voie
Zalen+JS).** La géographie : à partir de la décision ~131 (inv. 18), presque toutes les
fenêtres convertissent ; les tirs PRÉCOCES (déc. 55-128, inv. 5-17) et une bande médiane
(déc. 133-169) restent non convertis à ce budget — plusieurs touchent 8/8 sans refermer
(le mur d'échantillonnage habituel : après un Nibiru précoce, il faut re-dériver 150+
décisions AVEC les trois rips). Non converti ≠ réfuté : les refermetures écrites entrent en
`--approach` pour ensemencer les fenêtres précoces d'un futur run. La lecture de jeu : la
garde promettait un contre DISPONIBLE à 33/33 fenêtres ; le test adverse prouve un contre
QUI MARCHE ET REFERME sur 16 fenêtres, du milieu de ligne à la fin — et chiffre son prix
(board complet : +2-3 actions ; voie Zalen : +1 brûlée, Junk Signal en moins sur le board).
