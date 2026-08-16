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
  arXiv:2412.05196), et les approches des sessions passées (`--approach`). `--finisher
  mono|levin|ab` (ab = les deux à budget égal, la mesure).

  > **Reculs d'`--approach` — valeurs à jour (corrigé session 9, audit 7.6).** Ce paragraphe
  > annonçait {0, 10, 30, 60, 90, 120} ; le code sert **{0, 10, 20, 30, 45}** au finisseur LTS
  > et **{60, 70, 80, 90, 110, 150}** en phase A2 (NRPA). Les reculs d'archive {15, 30} et de
  > meilleur chemin {5, 10, 20, 40, 60, 90} correspondent, eux. Le §9.15 raisonne encore sur
  > les valeurs documentées : ses tables « appr0 recul N » sont à lire avec les vraies.

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

### 9.12 Session 6 : la borne instrumentée se révèle vacueuse, le prior par coup est réfuté — et l'escalade descend à 259

**La mission** : le rendement d'abord (chantiers 1-3 : borne B&B instrumentée, partagée,
calibrée ; chantier 5 : prior par rejeu de solutions visé sur les rips), le débit ensuite
(LuaJIT en chantier isolé si tenté). Santé verte AVANT et APRÈS chaque changement de moteur
(0 retry, 290 digests distincts / 0 fusion, référence retrouvée à 0 écart, A/B nouveauté
sans perte — l'écart de couverture IDLECMD #240 est préexistant, identique session 5).

**Chantier 1 — l'instrumentation, une matinée de code, LE résultat de la session.** Les
compteurs `burn_cuts`/`goal_hits` existaient sans un printf ; ils sortent désormais dans
les trois rapports (phase tirages : ligne `anytime`, finisseur : bilan agrégé, LDS :
colonne `atteinte(s)`), plus le pic de brûlées des lignes SANS board cible (une solution
s'arrête au tour 1 sans capture — son pic calibre pourtant la marge). Ce que les compteurs
révèlent aussitôt, sur 7 runs d'escalade de 600 s (26-33 M états chacun, graine 777) :
**la borne B&B ne coupe JAMAIS en phase tirages — 0 coupure, 0 atteinte, à toute marge de
4 à 255.** Deux causes lisibles : aucun tirage pleine ligne n'atteint le but sur ce flux
(les conversions viennent du finisseur, on le savait) ; et aucun état de tirage ne dépasse
23 brûlées au tour 1 (0 coupure même à borne 19+4=23). Les pics mesurés ferment la
question : référence 23 (marge 4), ligne 55 actions pic 23, ligne 56 actions pic 22 —
**l'espace des lignes gagnantes vit sous le pic de la référence, la borne brûlées est
structurellement inactive sur ce problème.** Elle ne mord que dans le finisseur (préfixes
déjà chargés à ~19-22 brûlées), et seulement à marge 4 : 201 coupures.

**Chantier 2 — le partage de la borne entre workers : NEUTRE, mécanisme expliqué.**
`SearchConfig::shared_burn` (atomique, CAS min à la publication dans GoalCheck, charge
relaxed au test — négligeable devant les deux Count() du test), câblé phase tirages et
finisseur, `--no-burn-share` pour l'A/B. Verdict : rien à partager — aucun but ne tombe en
phase tirages, et le finisseur hérite déjà de la meilleure borne via `best_known_burn`.
Défaut ON (gratuit, seul actif si un but tombait en cours de phase), documenté neutre.

**Chantier 3 — `--burn-slack` 4/6/8/255 à budget égal : 6/8/255 indiscernables (0 coupure
partout), 4 seul actif (201 coupures au finisseur) sans avantage** (3 conversions contre
4-5, mélange de coûts le plus pauvre — du bruit, ses propres lignes piquent à 22-23 donc
la borne 23 ne les coupe pas). Défaut 6 inchangé. Le chantier est CLOS : aucune marge ne
peut rendre du rendement quand la borne n'a rien à couper.

**La mesure transversale qui recadre tous les A/B futurs : à graine FIXÉE, le bruit de
run domine les compteurs de tirages.** Entre deux bras dont le mécanisme n'a RIEN fait
(0 coupure des deux côtés), les tirages rippants ≥1 vont de 98 à 9 402 et la crête de 6/8
à 8/8 — l'ordre des échanges inter-workers dépend du timing (piège 24 à l'échelle du run).
Conséquence de discipline : seuls les faits STRUCTURELS tranchent un A/B (coupures,
conversions, coûts écrits, ensembles de fenêtres converties) — jamais les compteurs
d'échantillonnage.

**Chantier 5 — le prior par rejeu de solutions (arXiv:2401.10431) : implémenté, mesuré
sur les DEUX étalons, NEUTRE — l'hypothèse « déficit de poids par coup » est réfutée.**
`--prior <fichier|dossier>` (répétable) relève les plan_key de chaque ligne du corpus par
LiftPlan sur le duel de SON en-tête (piège 21 respecté : on ne rejoue rien sur le duel de
départ, seules les identités sémantiques traversent), pondère par la fréquence dans le
corpus (`--prior-weight`, défaut 2.0 pour un coup présent partout), et sert le tout en
poids INITIAUX de politique aux tirages ET aux fenêtres `--fire`. Le relevé est bon marché
et propre : 28 lignes de `sF_final/` en 5,2 s, 101 coups distincts, 1 seule étape non
identifiée par ligne. Mais : étalon 1 — mêmes coûts retrouvés, conversions 5 contre 4
(bruit), rips du même ordre ; étalon 2 (300 s/fenêtre, graine 888, `--fire-open`) —
**ensembles de conversion IDENTIQUES avec et sans prior : 10/15 fenêtres (déc. 113-173),
les cinq fenêtres précoces (déc. 58-103) murées des deux côtés**, mêmes manques (Crimson
Dragon / Crystal Wing), mêmes crêtes 7-8/8. La conclusion de fond : le verrou des rips
précoces n'est PAS un déficit de prior par coup — +2 logit sur chaque coup de rip ne
change rien — c'est un verrou de SÉQUENCE/ressources (re-dériver ~150 décisions dans le
bon ordre), cohérent avec « rips en fin de ligne, structurel » (§9.11). Les leviers
restants pour ce mur : l'adaptation explicite vers les séquences du corpus (rejeu
d'adaptation, pas seulement des poids), ou les racines croisées entre fenêtres (chantier
6, jamais essayé). Le drapeau reste disponible (opt-in), hors de la commande recommandée.

**Chantier 9 — LuaJIT : FERMÉ sur pièces, sans branche ni run.** L'étude à froid tue le
chantier avant la batterie d'invariants : (1) le core embarque Lua 5.4.8 et l'export
épinglé utilise la syntaxe 5.3+ (`|`, `&`, `<<`, `//`) dans les deux socles
(`constant.lua` 25 usages, `utility.lua` 78) et **1 862 des 2 503 scripts de cartes** —
erreurs de syntaxe pour LuaJIT (langage 5.1) dès le chargement ; (2) LuaJIT 64-bit ne
supporte pas `lua_newstate` à allocateur custom — or TOUT le solveur repose sur le heap
Lua dans l'arène via ce point d'accroche ; (3) le mcode et les traces JIT vivent hors de
l'arène et retiennent des références GC vers le heap : chaque Restore() (des dizaines de
millions par run) exigerait un flush des traces, qui détruit le gain ; (4) l'API C est
5.4. **Le « seul 2-10× plausible » n'existe pas sous cette forme.** Le débit de fond se
jouera côté hôte : miroir des zones depuis MSG_MOVE (chantier 8, profil d'abord), LTS à
frontière partagée (chantier 7).

**Le gain de la session est venu d'où les cinq sessions le prédisaient : l'escalade.**
Deux des sept runs de 600 s ont battu l'acquis — 19/55/260 (slack8) et **19 brûlées / 55
actions / 259 décisions** (slack255, `sZ6_slack255/solution_00_b19_a55.yrp`), jugée depuis
zéro : 259/259, 0 retry, garde 35/35 fenêtres couvertes, 0 activation interdite,
Omega@terrain 2/2, Trishula 1/1. Leurs réglages propres étant inertes (0 coupure), c'est
la divergence de graine × l'anytime enraciné qui a trouvé — la trajectoire 272 → 263 →
261 → **259** est une hill-climb par redémarrages, et elle n'avait PAS convergé : chaque
relance de l'escalade sur la meilleure ligne reste le geste le plus rentable du répertoire.

**Non fait, et documenté comme tel** (session 6) : le rejeu d'ADAPTATION du corpus
(l'alternative au prior par poids, la voie restante du chantier 5) ; les racines croisées
`--fire` (chantier 6) ; le LTS à frontière partagée (chantier 7) ; le miroir de zones
(chantier 8, profil d'abord) ; le partage périodique de la POLITIQUE entre workers
(chantier 4, le plus ancien non-fait) ; fermer k=5 et la relaxation SMT/ILP pour 18
brûlées (§9.10-9.11).

### 9.13 Session 7 : l'escalade se tait sur quatre graines, et le transfert de politique touche son plafond de représentation

**La mission** : les deux leviers de rendement laissés debout par la session 6 — l'escalade
(le seul mécanisme qui avait encore progressé) et la mémoire de politique (chantier 5bis :
le rejeu d'ADAPTATION, la voie restante de arXiv:2401.10431 après la réfutation du prior par
POIDS). Santé verte avant et après (0 retry, 290 digests distincts / 0 fusion, référence
retrouvée à 0 écart, A/B nouveauté sans perte ; l'écart de couverture IDLECMD #240 reste
préexistant) — et la santé d'après est IDENTIQUE ligne à ligne à celle d'avant (aux mesures
de temps et au nom du `--outdir` près : 22 lignes de diff, toutes des durées), le mécanisme
ajouté étant inerte sans son drapeau.

**(a) L'escalade : trois graines muettes à 600 s, une quatrième à 1 800 s.** Enracinée sur la
ligne 259 (`--approach sZ6_slack255/solution_00_b19_a55.yrp`, `--finisher-min 420000`,
`--burn-limit 19`, `--archive-k 24`), la commande de l'étalon 1 a tourné sur les graines
888, 999, 1234 (600 s) puis 4242 (1 800 s). **Aucune ne bat 19/55/259 ; toutes les quatre le
RETROUVENT.** Le détail dit la même chose quatre fois : 888 → 3 conversions, 25 lignes,
second coût 19/56/272 ; 999 → 3 conversions, 25 lignes, second 19/55/260 ; 1234 → 4
conversions, 37 lignes, les trois premières à 19/55/259 ; 4242 (budget TRIPLE) → 3
conversions, 25 lignes, les quatre premières à 19/55/259. La ligne écrite par le run 4242 a
été jugée depuis zéro, indépendamment : **259/259 réponses, 0 retry, garde 35 fenêtres
sous menace / 0 découverte, 0 activation interdite, Omega@terrain 2/2, Trishula@terrain 1/1**
— « retrouver 259 » désigne bien une ligne réelle et rejouable, pas une écriture comptable.
La barre de la session 6 (piège 41 :
au moins trois graines muettes consécutives avant de conclure) est donc ATTEINTE pour la
première fois, à ce budget et sur cette racine — la trajectoire 272 → 263 → 261 → 259
s'arrête ici. Ce que la mesure ne dit PAS : que 259 soit optimal. Elle dit que le mécanisme
d'escalade tel qu'il est réglé (racine = meilleure ligne, reculs 60-150, 600-1 800 s) a cessé
de rendre — ce qui déplace la charge sur un mécanisme d'un autre ordre, pas sur une graine
de plus.

**Le sous-produit méthodologique de ces trois graines : une mesure de la DISPERSION du bras
témoin.** Sans elle, l'A/B de l'étalon 1 se serait lu à l'envers (cf. plus bas) — trois runs
témoins coûtent 30 minutes et valent mieux qu'un chiffre isolé, y compris sur des faits
réputés structurels.

**(b) Chantier 5bis — le rejeu d'ADAPTATION : implémenté, instrumenté, mesuré sur les deux
étalons. VIVANT (contrairement au prior par poids) mais SANS effet sur les verdicts.** Le
prior de la session 6 disait « ce coup existe dans les solutions » et le primait PARTOUT ;
l'adaptation dit « à CE carrefour, la solution prenait celui-ci contre ceux-là ». Même
corpus, même point d'injection (politique initiale des tirages et de chaque fenêtre `--fire`),
même relevé sur le duel de l'en-tête de chaque ligne (piège 21) : l'A/B isole donc la FORME
du signal, prime par coup contre gradient discriminatif.

- *Le relevé* (`LiftPolicyRun`) reprend l'appariement de `LiftPlan` — la réponse enregistrée
  est identifiée par l'ÉTAT qu'elle atteint, jamais octet par octet — mais conserve à chaque
  prompt multi-choix l'ensemble des `plan_key` LÉGAUX et l'indice du choisi, c'est-à-dire
  exactement le `PolicyStep` que consomme `Adapt()`. Le biais `known` du répertoire y est
  reproduit, sans quoi le gradient serait calculé sous une distribution qui n'est pas celle
  de l'échantillonnage. Coût : **28 lignes de `sF_final/`, 4 614 décisions multi-choix,
  1 étape non identifiée par ligne, 4,7 s.**
- *`Adapt()` devient libre* (`AdaptRun`/`AdaptCorpus`) pour que l'instrument et les workers
  appliquent la MÊME mise à jour — un instrument qui mesure autre chose que ce que le run
  subit ne mesure rien. Le refactor est bit-à-bit neutre (santé identique).
- *L'INSTRUMENT, avant tout run* (piège 40) : la courbe de saturation de l'« accord du
  corpus » — la probabilité moyenne que la politique donne au coup que les solutions ont
  joué. Elle s'imprime au relevé, en microsecondes, pour 0/1/2/4/8/16 passes.

**Ce que la courbe a dit, et qui a réglé le drapeau sans dépenser un run.** Sur les 28 lignes :
**44,3 % à politique vierge → 65,2 % en UNE passe → 66,2 % à seize.** Le mécanisme MORD (le
prior, lui, ne déplaçait rien d'observable) mais il sature immédiatement : au-delà d'une
passe, rien. Sur une SEULE ligne (la 259) : 44,9 % → 58,1 / 62,2 / 65,9 / 68,6 / **70,2 %** —
plus lent, et il finit au même palier.

> **RECTIFICATION (session 7bis, §9.14) — la lecture ci-dessus était fausse, l'instrument
> mesurait autre chose que ce qu'on lui faisait dire.** Cet « accord » est une moyenne
> GÉOMÉTRIQUE de p(coup joué). Une poignée d'étapes à p ≈ 0 l'écrase, et elle fait passer
> pour médiocre une politique qui classe en réalité le bon coup PREMIER dans 93 % des cas.
> La conclusion « le plafond est la REPRÉSENTATION » qui suivait ici est donc retirée : voir
> §9.14, où la mesure comparable (fraction d'étapes où le coup du corpus est premier) est
> confrontée à un plafond calculé. Ce qui reste vrai de ce paragraphe : le prior par poids
> et l'adaptation sont tous deux neutres sur les deux étalons. Ce qui est faux : la raison
> qu'on en donnait.

**Étalon 2 (les fenêtres `--fire` ouvertes, graine 888, 300 s/fenêtre, corpus `sF_final`,
4 passes) — ensembles de conversion IDENTIQUES, mais la recherche BOUGE.** Bras témoin :
10/15 converties, déc. 113-173, les cinq fenêtres précoces (déc. 58-103) murées —
**identique fenêtre par fenêtre à la base de la session 6, crêtes et diagnostics « manque : »
compris.** C'est cette reproductibilité du témoin qui autorise à attribuer au mécanisme ce
qui change dans l'autre bras. Bras adaptation : **mêmes 10/15, mêmes déc. 113-173, mêmes
cinq murées** — mais les états atteints diffèrent : les manques des fenêtres précoces passent
de Crimson Dragon / Crystal Wing / Crystal Wing à Hot Red Dragon Archfiend Abyss / Zalen /
Junk Signal / Crimson Dragon, et les crêtes bougent sur 7 fenêtres (9-13 : 6/8 → 7/8 ;
4 et 5 : 8/8 → 7/8 ; 6 : 7/8 → 8/8). Le verdict est donc plus informatif qu'une neutralité :
**l'adaptation déplace réellement l'échantillonnage et ne franchit toujours pas le mur des
rips précoces.** Le verrou n'est pas un déficit de guidage par coup — c'est bien un verrou de
SÉQUENCE (re-dériver ~150 décisions dans le bon ordre), et un guide state-blind plafonné à
66 % d'accord ne l'ouvre pas.

**Étalon 1 (même-deck, graine 888, 600 s, même racine) — NEUTRE, et c'est la dispersion du
témoin qui le prouve.** Le bras adaptation rend 19/55/259, 4 conversions, 37 lignes, second
coût 19/55/260 — lu seul contre le témoin de MÊME graine (19/55/259, 3 conversions, 25
lignes, second 19/56/272), il aurait l'air d'un gain. Mais les trois graines témoins couvrent
exactement cet intervalle : 888 → 3/25, 999 → 3/25, **1234 → 4/37**. Le bras adaptation tombe
DANS la dispersion de son propre témoin, et le meilleur coût est 19/55/259 dans les quatre
runs. **Neutre.** Corollaire de discipline, à ajouter au piège 39 : les conversions et le
nombre de lignes sont bien des faits structurels, mais un fait structurel se compare à une
DISPERSION, pas à un run.

**Verdict et réglage.** `--adapt <f|dossier>` (répétable) et `--adapt-passes <n>` (défaut 4 —
sur le palier mesuré ; 0 coupe le mécanisme en gardant le relevé, c'est le bras témoin).
Comme `--prior`, le drapeau reste OPT-IN et HORS de la commande recommandée : il est vivant,
il ne rend rien sur les deux étalons. (La *raison* qu'on en donnait ici — « la politique NRPA
est un sac de poids par coup » — est corrigée au §9.14.)

**(c) Chantier 6 — les racines croisées `--fire` : CLOS SUR STRUCTURE, sans run.** L'idée
(« les lignes converties d'une fenêtre servent d'`--approach` aux fenêtres voisines ») bute
sur la construction même des fenêtres, vérifiée dans le code : `FireWindow::prefix` est un
préfixe de LA MÊME passe de découverte — les fenêtres sont des troncatures emboîtées d'une
seule ligne de base. Une ligne convertie de la fenêtre B vaut donc `préfixe_base(B) +
injection_B + suffixe_B`. Pour une fenêtre A plus précoce : reculer cette ligne avant
`our_at(A)` redonne exactement l'état d'où A part déjà (aucune information nouvelle), et
reculer après `our_at(A)` saute l'injection de A — la fenêtre n'est plus tirée, le résultat
ne répond plus à la question posée. Pour une fenêtre C plus tardive : la ligne de B diverge
de la base dès `our_at(B) < our_at(C)`, elle ne contient pas non plus le préfixe de C.
**Aucune racine au niveau des RÉPONSES ne peut porter la menace d'une autre fenêtre** ; le
seul transfert inter-fenêtres possible est au niveau de la POLITIQUE — c'est-à-dire `--prior`
(réfuté) et `--adapt` (ci-dessus, plafonné). La variante qui reste vivante est l'escalade
transposée PAR fenêtre : écrire la meilleure APPROCHE de chaque fenêtre (les crêtes 7-8/8) et
ré-enraciner CETTE fenêtre sur SA propre approche avec des reculs profonds — le mécanisme
prouvé du répertoire, appliqué là où il n'a jamais été branché (chantier 6bis).

**Non fait, et documenté comme tel** (session 7) : le contexte dans la `plan_key` (la suite
du chantier 5, changement de représentation) ; l'escalade par fenêtre `--fire` (chantier
6bis) ; le partage périodique de la POLITIQUE entre workers (chantier 4, toujours le plus
ancien non-fait) ; le LTS à frontière partagée (chantier 7) ; le miroir de zones (chantier 8,
profil d'abord) ; fermer k=5 et la relaxation SMT/ILP pour 18 brûlées (§9.10-9.11).

### 9.14 Session 7bis : la politique savait déjà — ce qui manquait était la MASSE, pas la connaissance

**Le point de départ** : une étude arXiv du corpus Cazenave, lancée pour trouver comment
franchir le « plafond de représentation » que le §9.13 venait de diagnostiquer. Elle a livré
un mécanisme (MCPS, arXiv:2510.06381 — combiner plusieurs estimateurs d'un même coup pondérés
par leur évidence, au lieu d'en choisir un). Ce mécanisme a été implémenté, mesuré — **et il
perd**. Mais l'instrumentation qu'il a fallu construire pour le mesurer a montré que le
diagnostic du §9.13 était faux, et a chiffré le vrai.

**Chantier 5ter — la politique à DEUX NIVEAUX : implémentée, mesurée, PERDANTE, désactivée.**
`w_eff(coup, ctx) = (1-s)·w_global + s·w_ctx` avec `s = n/(n+k)`, n l'évidence accumulée par
la case contextuelle et `--ctx-shrink k` le cadran (k < 0 = éteint, comportement d'avant bit
pour bit — santé identique vérifiée). Le contexte est sémantique et calculé à l'identique au
tirage et au relevé : cartes du board cible posées × cartes restant en main (20 valeurs
distinctes sur le corpus). L'écart assumé avec MCPS est documenté dans le code : ses
estimateurs sont indépendants et pondérés par les effectifs bruts, les nôtres sont EMBOÎTÉS
(le global agrège tous les contextes, son effectif domine toujours), d'où la retenue par un k
calibré. **A/B étalon 2, trois bras, même graine, même budget : témoin 10/15 converties,
k=64 → 9/15, k=1 → 7/15.** La perte est MONOTONE dans le cadran — c'est ce qui la rend
crédible malgré le piège 39 : trois bras ordonnés par un seul réglage donnent trois résultats
ordonnés. Mécanisme : dans une recherche par fenêtre, la politique apprend de ses propres
tirages et cette donnée est RARE ; répartir l'évidence sur 20 contextes coûte plus que la
précision de classement qu'elle achète. Défaut `--ctx-shrink -1` (éteint).

**L'instrument qui a tout retourné : deux métriques au lieu d'une.** L'« accord du corpus »
du §9.13 est une moyenne GÉOMÉTRIQUE de p(coup joué). Elle est écrasée par une poignée
d'étapes à p ≈ 0, et lue seule elle fait passer pour médiocre une politique qui a raison
presque partout. On lui a adjoint (a) la **fraction d'étapes où le coup du corpus est classé
PREMIER** — la seule des deux qui se compare à quelque chose — et (b) un **plafond calculé** :
en groupant les étapes par point de décision (contexte + ensemble des coups légaux) et en
prenant le coup majoritaire de chaque groupe, on obtient ce qu'atteindrait une table parfaite,
soit **96,1 % sans contexte (139 points distincts) et 97,3 % avec (243)**. Le corpus ne se
contredit donc quasiment pas : l'écart au plafond, s'il y en a un, est un défaut
d'apprentissage, pas une fatalité.

**Ce que les deux métriques disent, et qui referme trois sessions de résultats neutres.**

| politique | moy. géom. de p | coup du corpus classé 1er |
|---|---|---|
| **vierge** (biais du répertoire seul, 0 passe) | 44 % | **96 %** |
| adaptation, 4 à 256 passes, α de 1,0 à 0,05 | 66-68 % | 93-94 % |
| + niveau contextuel (k=1) | 72-74 % | 95-97 % |
| + contexte = signature du point de décision | 74-75 % | 96-97 % |
| *plafond de la famille* | — | *96,1 / 97,3 %* |

**À politique VIERGE, le coup du corpus est déjà classé premier 96 % du temps — au plafond.**
Le biais du répertoire (+1,5 aux coups de la ligne de référence) suffit. L'adaptation ne
monte pas ce chiffre : elle le fait légèrement BAISSER (96 → 93 %) en échange de masse
(44 → 66 %). Le prior par poids, l'adaptation par gradient et le contexte dans la clé
agissaient donc tous les trois sur le CLASSEMENT — la seule chose que la politique possédait
déjà. **C'est l'explication unique des trois résultats neutres ou négatifs des sessions 6
et 7, et elle est chiffrée.**

**La quantité qui manque, et son ordre de grandeur.** Ce qui est bas, c'est la MASSE : 44 % de
probabilité moyenne sur le bon coup à politique vierge, 74 % au mieux. Or un tirage doit
enchaîner ~160 décisions, et ce qui compte est le PRODUIT : 0,74^160 ≈ 10^-21. Une politique
qui a raison 96 % du temps sur le classement ne rejoue pas pour autant une ligne de 160 coups
— elle n'y arrivera jamais par échantillonnage. Le « 1 tirage sur 800 000 fait les trois
rips » du §9.11 n'était pas un déficit de connaissance : c'était ce produit.

**Le corollaire, qui explique aussi pourquoi l'escalade est le seul mécanisme qui ait jamais
rendu.** Deux familles de leviers existent : ceux qui re-CLASSENT (prior, adaptation,
contexte — tous mesurés neutres ou négatifs, et on sait maintenant pourquoi) et ceux qui
imposent la MASSE. `--approach` appartient à la seconde : il ne suggère pas la ligne, il la
REJOUE, probabilité 1 sur son préfixe, et ne rend la main à l'échantillonnage qu'au point de
recul. C'est exactement le levier qui a produit 272 → 263 → 261 → 259. Toute mécanique de
transfert de corpus qui se contente de re-pondérer les coups est prédite NEUTRE par cette
mesure ; le seul candidat non essayé qui agisse sur la masse est la TEMPÉRATURE de GNRPA
(arXiv:2003.10024) — diviser les logits par τ < 1 concentre la masse sans rien changer au
classement. C'est le chantier 5quater — et la prédiction a été mise à l'épreuve dans la foulée.

**Chantier 5quater — la TEMPÉRATURE (`--nrpa-temp`, GNRPA arXiv:2003.10024) : la première
conversion jamais obtenue dans la bande précoce, mais elle ne se reproduit pas.** Les logits
sont divisés par τ avant le softmax, à l'échantillonnage ET à l'adaptation (sans quoi le
gradient ne serait pas celui de la distribution tirée). τ = 1 = comportement d'avant ; santé
identique vérifiée. A/B étalon 2, cinq bras :

| bras | converties | bande précoce (déc. 58-103) |
|---|---|---|
| témoin τ=1, graines 888 / 999 / 1234 | 10/15 chacune, **ensembles identiques** | aucune |
| τ=0,5 graine 888 | **11/15** (sur-ensemble strict du témoin) | **déc. 89 — première jamais convertie** |
| τ=0,25 graine 888 | 10/15, ensemble du témoin | aucune |
| τ=0,5 graine 999 | 10/15, ensemble du témoin | aucune |
| τ=0,5 graine 1234 | 10/15, ensemble du témoin | aucune |

Ce que cela vaut, sans le surinterpréter : **le témoin est invariant sur trois graines** (même
ensemble, mêmes crêtes, mêmes « manque : » — déjà vrai en session 6), ce qui rend la fenêtre
supplémentaire de τ=0,5 à la graine 888 non attribuable au bruit d'échantillonnage : c'est un
événement que le mécanisme a rendu possible. Mais **il ne se reproduit sur aucune des deux
autres graines**, donc ce n'est pas un gain sur lequel compter. Ce qui EST solide : la
température ne perd JAMAIS de fenêtre, sur trois graines et deux valeurs. Les 16 replays écrits
par le bras gagnant ont été jugés depuis zéro, depuis leur en-tête cuit et sans drapeau :
**16/16 rejoués, 0 retry, Omega@terrain 2/2, Trishula@terrain 1/1** (263-268 décisions,
20 brûlées / 57-58 actions sur le board amputé). La fenêtre déc. 89 a donc bien été refermée
pour de vrai — le mur de la bande précoce n'est pas une impossibilité, il est franchissable.
Défaut `--nrpa-temp 1.0` (éteint), opt-in, hors commande recommandée jusqu'à confirmation.

**Ce que la session 7bis a obtenu, et ce qu'elle n'a pas obtenu.** Pas de ligne sous 19/55/259,
pas de gain fiable en conversions. Mais : (1) un diagnostic chiffré qui remplace trois
hypothèses successivement réfutées — déficit de prior, verrou de séquence, plafond de
représentation — par une grandeur mesurée, et qui a PRÉDIT le signe du seul mécanisme restant
avant qu'on l'écrive ; (2) la première refermeture de la bande déc. 58-103, murée sur six runs
antérieurs, vérifiée et rejouable ; (3) deux instruments réutilisables (fraction de classement
premier, plafond par point de décision) qui rendent tout futur mécanisme de politique
jugeable en millisecondes.

**Non fait, et documenté comme tel** (session 7bis) : confirmer la température sur d'autres
graines et essayer un τ VARIABLE (décroissant avec l'évidence, ou par contexte) — le gain
existe, sa reproductibilité non ; la borne inférieure LP/IP par comptage d'opérateurs pour
trancher 18 brûlées (arXiv:2404.07934, la forme concrète de la « relaxation SMT/ILP » que
§9.10 porte depuis trois sessions) ; les landmarks généralisés appris depuis les plans résolus
(arXiv:2508.21564) ; l'escalade par fenêtre `--fire` (chantier 6bis) ; chantiers 4, 7, 8
inchangés.

### 9.15 Session 8 : le mode BUT SEUL, et le rerooter qu'on n'avait pas implémenté

**La mission** : atteindre un board donné depuis une decklist, sans ligne de référence — et,
pour y arriver, une revue d'état de l'art. C'est le premier chantier du projet dont la réponse
n'était pas dans le dépôt. La revue complète est dans `docs/etat-de-lart-but-seul.md` ; ce qui
suit n'en retient que ce qui a changé le code ou une décision.

**(a) Ce que la revue a rapporté, et qui ne se devinait pas.**

*Le domaine a un jumeau publié.* Le banc **CraftWorld** de arXiv:2605.30664 se décrit ainsi :
« l'agent collecte des matériaux bruts, les transforme en objets intermédiaires eux-mêmes
transformables en produits finaux ; **l'environnement peut se retrouver en impasse si l'on
fabrique le mauvais objet, les objets étant consommés dès qu'ils entrent dans une recette** ».
C'est le combo Yu-Gi-Oh en une phrase. Ses chiffres deviennent donc des ordres de grandeur
crédibles, ce qu'aucun résultat sur Sokoban ne pouvait être : LTS 306 224 expansions,
√LTS-L (grappes) 8 803, **√LTS-H (heuristique) 2 515**, hybride 1 348 — et la génération
EXPLICITE de sous-buts par autoencodeur, 345 096, *pire que LTS*.

*Notre `--reroot` n'est aucun des rerooters de l'article.* Le coût de √LTS est
`c(n) = min_{n_t ≺ n} (1/w_t)·c^r_{n_t}(n)` — un minimum sur TOUS les ancêtres, chacun pondéré.
Notre implémentation de la session 7ter ne considère qu'un ancêtre, le plus proche portant un
INDICE, à poids uniformes : un rerooter **dur**. Les trois conceptions mesurées par l'article
sont toutes **douces**, à poids partout non nuls. D'où un diagnostic mécanique de l'échec de
7ter, indépendant de l'argument de régime : notre indice est « le nombre de cartes cibles
posées a changé », or il ne tombe pas avant l'étape 76 sur 108 du cas Lunalight, et jamais sur
la bande précoce du cas synchron. **Sans indice, il n'existe aucun point de re-enracinement et
√LTS dégénère exactement en LTS.** Le rerooter dur est structurellement incapable de mordre
dans un paysage plat — qui est précisément notre paysage. L'A/B perdant de 7ter avait donc
deux causes, pas une.

*Un résultat négatif obtenu sans dépenser un run.* Ståhlberg & Geffner (arXiv:2512.19355)
mesurent trois variantes de HER en planification : le but relabellisé en sous-ENSEMBLE du but
original bat le but relabellisé en état complet de 31 points (82,4 % contre 51,0 %), et le
relabeling fabrique tout seul un curriculum. Mais leur domaine *Delivery* échoue : « tous les
colis doivent être livrés au MÊME endroit […] la probabilité d'en livrer plusieurs dans une
seule trajectoire est extrêmement faible, **les modèles apprennent à en livrer un seul** ».
Notre étalon A vise **3× Liger Dancer** et notre meilleur résultat mesuré est **un** Liger,
jamais deux. Même forme de but, même échec. *Le rejeu rétrospectif seul ne débloquera pas
l'étalon A*, et on le sait avant de l'écrire. Second avertissement du même papier, sur le
SIGNE : « toute trajectoire finissant en cul-de-sac est relabellisée en succès […] il n'y a
donc aucune donnée sur la façon d'ÉVITER les culs-de-sac ». Chez nous une ligne qui brûle une
pièce nécessaire vingt-cinq étapes plus tard est exactement cela : un répertoire rétrospectif
non filtré apprendrait à foncer dans le mur.

*Écarté sur structure, avec la raison.* La recherche bidirectionnelle (NBS, BAE\*, PEM-BAE\*,
DIBBS et leurs descendants 2026) exige sans exception une **fonction de prédécesseurs** ;
`ocgcore` ne peut pas en avoir — un état de duel est un tas Lua plus une pile de résolution
plus des compteurs « une fois par tour », et « l'état d'avant » n'est pas calculable, seulement
re-simulable. Les heuristiques de relaxation et le comptage d'opérateurs exigent un modèle
déclaratif des actions ; nos actions sont des scripts Lua. L'apprentissage de sketches (Drexler
et al.) exige PDDL et de petites instances du même domaine. Ce qui reste vivant de ces
directions est le **graphe de recettes appris depuis les tirages** (les matériaux consommés
sont observables à l'invocation) : ce n'est pas une recherche en arrière, c'est ce qui
fournirait un `h` qui DÉCROÎT en cours de ligne — et donc ce qui armerait le rerooter
heuristique, dont les poids sont fonction de `h`. Les deux chantiers n'en font qu'un.

**(b) Le mode but seul, implémenté dans sa forme minimale.**

- `--target "carte[@ATK|DEF]"`, répétable : le board cible est **posé**, plus édité depuis la
  capture de la référence. La cascade de `--board-remove` de la session 7ter était elle-même
  de l'information sur ce que la référence avait posé.
- `--no-plan` : le répertoire est relevé, **imprimé**, puis jeté. Le relevé reste pour que la
  mesure dise ce qu'on retire — un mécanisme neutralisé en silence n'est pas un bras témoin.
- `--no-ref` : implique `--no-plan`, exige `--target` et `--deck`. Le replay positionnel est
  dégradé au rang de **gabarit de duel** ; ses drapeaux, LP, taille de main et deck adverse
  sont imprimés en tête du rapport, pour que « sans référence » soit vérifiable et non promis.
- Le compteur `reroots` **est imprimé** dans la table du finisseur. Il existait depuis la
  session 7ter et n'était lu nulle part : l'A/B de 7ter a donc été tranché sans savoir si le
  mécanisme mordait (piège 40, sur notre propre instrument).

**Une observation du mode — et sa RÉTRACTATION, dans la même session.** Sans plan, la passe
« recherche à écarts bornés autour du plan » rend 26 états à tous les niveaux d'écart sur
l'étalon B. On en avait tiré ici même une conclusion structurelle : « le mode but seul perd
d'abord le mécanisme qui rendait le plus, c'est la définition du mode ». **C'est faux, et
l'audit de fin de session l'a établi.** La partition entre workers de `DescendTransplant`
réclame des jetons dans une table dimensionnée `plan.size() + 1` (`main.cpp:5637`), jamais
relâchés ; avec `--no-plan` le plan est vide, donc **un seul jeton pour seize workers**, et
`if(c.cost > 0 && !mine) continue;` (`search.cpp:1257`) supprime alors TOUTE déviation chez
les quinze autres — et chez le gagnant lui-même dès son deuxième nœud. Les 26 états sont
intégralement expliqués par ce défaut. Ce que le mode retire vraiment à cette passe reste
donc INCONNU : la mesure est à refaire une fois la partition corrigée (plan de correctifs,
chantier C1). Leçon, et elle est générale : *un chiffre spectaculairement bas est d'abord un
suspect de bug, pas une découverte structurelle.*

**(c) √LTS-H : le rerooter heuristique de l'article, transposé.**

    w_t = exp(−α · h(n_t) / h(racine))            (Eq. 7 de arXiv:2605.30664)

avec h = cartes cibles manquantes + résolutions manquantes, déjà calculé à chaque nœud du
finisseur. Le min sur les ancêtres est tenu en O(1) par une récurrence à deux candidats —
prolonger l'ancêtre qui minimisait déjà le coût chez le parent, ou se re-enraciner sur le
parent. Le second est toujours disponible, donc il **borne** le coût : le mécanisme est
numériquement stable par construction, là où d/π déborde. L'écart avec le min exact sur tous
les ancêtres est unilatéral (jamais de sous-estimation) et majoré par ce terme.

**α, et une erreur d'échelle trouvée par l'audit.** L'intention : à h constant — le paysage
plat — tous les 1/w valent e^α et le re-enracinement l'emporte dès que le coût accumulé du
segment dépasse ~e^α, donc **α est le logarithme du coût toléré par segment** ; et
`ForecastSearchCost` ayant chiffré ce coût à 10^5,8-10^12,5 par segment, le cadran 8/15/25
tombait dans la fenêtre α ∈ [13,4 ; 28,8]. **L'implémentation ne réalise pas ce calcul.**
`h_root` y vaut `|cible| + Σ resolve` (`search.cpp:1900`), une constante du PROBLÈME, alors
que l'Eq. 7 de l'article normalise par h à la RACINE DE LA RECHERCHE — laquelle, quand le
finisseur part d'une approche 7/8, vaut 1. L'échelle effective est donc α/8 sur l'étalon B :
le coût toléré par segment testé vaut e^1 à e^3, pas 10^5,8 à 10^12,5.

Ce qui survit à la correction, et ce qui tombe. **Tombe** : « la prévision de coût a prédit le
bon réglage » — c'est une coïncidence, pas une dérivation. **Survit** : le cadran est un
cadran, α croissant allonge les segments, et les trois résultats restent ordonnés. La lecture
qualitative (α petit ⇒ glouton ⇒ perte) est inchangée ; seule l'échelle numérique est à
refaire, avec `h_root` évalué au nœud 0 de la recherche courante (chantier C3).

Défaut `--reroot-h 0` (éteint), exclusif avec `--reroot`. Santé avant/après le chantier :
**identique, 20 lignes de diff, toutes des mesures de durée**.

**(d) Les mesures — et d'abord le montage, parce que c'est lui qui a changé le verdict.**

*L'A/B bout-en-bout ne mesure presque rien, et il a fallu s'en apercevoir.* `--reroot` et
`--reroot-h` ne touchent QU'À `RunLevin`, le finisseur. Tout ce qui précède — sonde, tirages
NRPA, archive — est le même code dans tous les bras et ne diffère que par le timing des
échanges entre workers. Deux bras bout-en-bout de 600 s sur l'étalon B (régime but seul,
graine 888) le confirment : **témoin et `--reroot` rendent le même verdict, 7/8, aucune
conversion, meilleure approche identique en substance.** C'est l'information attendue pour la
partie (b) de la mission — √LTS dur, mesuré cette fois dans le bon régime, reste sans effet —
mais elle a coûté deux fois vingt minutes pour un signal noyé dans le bruit des tirages.

*Le bon instrument : racines ET politique identiques.* On sert la MÊME approche à tous les
bras (`--approach`, cinq reculs déterministes 0/10/20/30/45), on donne au finisseur l'essentiel
du budget (`--finisher-min`), et surtout on ajoute `--no-nrpa` : la politique servie au
finisseur est alors VIDE dans tous les bras, donc identique (« politique 0 poids » au rapport).
Racines identiques + politique identique ⇒ ce qui bouge ne peut venir que de la fonction de
coût. Seule subsistance de bruit : les racines limitées par le BUDGET voient leur nombre
d'expansions varier de quelques pour cent avec la charge — d'où la lecture sur le
**recouvrement atteint**, pas sur les expansions.

*Le contrôle de correction, qui valide l'implémentation.* La racine `recul 0` ÉPUISE son
espace : **42 expansions dans tous les bras, sans exception.** Le rerooter change l'ORDRE
d'expansion, pas l'ensemble atteignable — c'est exactement ce qu'on attend d'une fonction de
coût, et cela exclut la classe de bugs qui perdrait des nœuds.

*Ce que le compteur imprimé a révélé, et c'est le résultat le plus solide de la session.*

| bras | recul 10 | recul 20 | recul 30 | recul 45 | cumul | re-enracinements |
|---|---|---|---|---|---|---|
| témoin (LTS) | 6/8 | 5/8 | 4/8 | 5/8 | 20 | — |
| `--reroot` (dur) | 6/8 | 5/8 | 4/8 | 5/8 | 20 | **1 à 157** |
| `--reroot-h 8` | 6/8 | **4/8** | **3/8** | 5/8 | **18** | 3 987 à 42 169 |
| `--reroot-h 15` | 6/8 | 5/8 | 4/8 | 5/8 | 20 | 3 908 à 40 584 |
| `--reroot-h 25` | 6/8 | 5/8 | **5/8** | 5/8 | **21** | 1 568 à 66 032 |
| `--reroot-h 40` | 6/8 | 5/8 | 4/8 | 5/8 | 20 | **2 à 922** |
| `--reroot-h 60` | 6/8 | 5/8 | 4/8 | 5/8 | 20 | **2 à 162** |

**Le rerooter DUR ne se déclenche que 1 à 157 fois pour 3 400 à 9 400 expansions — environ
2 %.** Le mécanisme tranché « perdant » en session 7ter était donc quasi INERTE, et personne
ne pouvait le savoir : le compteur existait mais n'était imprimé nulle part. Le verdict de
7ter n'était pas « √LTS perd », c'était « un mécanisme qui ne s'exécute pas ne rend rien » —
la différence est entière, et elle valide la lecture donnée en (a) : sans indice, pas de point
de re-enracinement.

**Le rerooter DOUX mord partout, et le cadran est UNIMODAL À MAXIMUM ENCADRÉ.** 18 → 20 → 21
→ 20 → 20 cartes cumulées pour α = 8 → 15 → 25 → 40 → 60, le témoin valant 20. Cinq bras
ordonnés par un seul réglage, un maximum intérieur : c'est mieux qu'une monotonie qui
s'arrêterait au dernier point testé, laquelle laisserait ouverte l'hypothèse « ça continue de
monter ». La lecture est mécanique, et les DEUX bouts se comportent comme la théorie du
mécanisme le prédit :

- α trop petit ⇒ le re-enracinement sur le parent l'emporte toujours ⇒ la recherche devient
  gloutonne sur la probabilité du dernier pas ⇒ **PERTE** (18 contre 20) ;
- α trop grand ⇒ les segments ne se cassent plus ⇒ **le mécanisme devient inerte**, et le
  compteur le montre : `rr` s'effondre de ~40 000 à **2-162** en α = 40/60, pendant que le
  verdict converge exactement vers celui du témoin. C'est la vérification que le compteur
  imprimé était censé rendre possible, et elle fonctionne.

À α = 25, une racine passe de 4/8 à 5/8.

**Ce que cela vaut, et ce que cela ne vaut pas.** Une racine gagnée sur quatre, à une graine :
ce n'est pas un gain sur lequel compter, et le mécanisme reste OPT-IN, `--reroot-h 0` par
défaut. Ce qui est solide : le cadran est un vrai cadran, avec un optimum intérieur encadré et
deux régimes limites conformes à la théorie ; et le mécanisme ne perd jamais au-dessus de
α = 15.

**L'explication, et elle désigne le chantier suivant.** Le rerooter — dur ou doux — est une
fonction de `h`. Notre `h` est le nombre de cartes cibles manquantes, et il est PLAT sur
l'essentiel de la ligne. Un rerooter à poids quasi constants ne peut alors casser les segments
qu'au coût accumulé : il fait ce que ferait un LTS à segments de taille fixe, ce qui explique
qu'on retrouve le témoin au mieux et un gain marginal au bord. *Le rerooting ne peut pas
rendre davantage tant que `h` reste plat* — et c'est la mesure qui établit que le verrou est
en amont, dans l'heuristique, pas dans l'algorithme de recherche. C'est exactement ce que le
graphe de recettes (chantier 16) fournirait.

**(e) L'étalon A en mode but seul — ce que valait la référence, et pourquoi le chiffre n'est
pas encore attribuable.**

Montage : `tools/lunalight_butseul.ps1`, 1 800 s, graine 888, `--no-ref --target` ×4,
`--max-decisions 700`, mêmes `--resolve`/`--summon-min`/`--hint` que le run de référence de la
session 7ter (ce sont des connaissances de DOMAINE écrites à la main, pas du répertoire — au
sens de Bonet & Geffner, un *sketch*). Zéro erreur de core (piège 47 vérifié).

| | avec répertoire (s7_luna_v6) | mode BUT SEUL (s8_A_base) |
|---|---|---|
| tirages | 1 246 s, 12,66 M tirages, best 1/4 | 1 246 s, best **1/4** |
| finisseur | 1/4 (une racine à 2/4) | 36 racines, **1/4** |
| passe LDS | 2 038 555 états, 294 s, **2/4** | vide (défaut C1) |
| board atteint | 1 Liger + Bagooska | **Bagooska seul** |

**Le mode but seul pose Bagooska et aucun Liger ; la référence en posait un.** La carte perdue
est exactement celle qui demande le combo profond — Bagooska est un Xyz accessible, Liger
Dancer est au bout de la chaîne Kaleido Chick → Leo Dancer au cimetière → Fusion. C'est la
forme attendue : le répertoire ne valait pas « un peu partout », il valait *la partie difficile*.

**Mais ce chiffre n'est pas encore attribuable au répertoire, et il faut le dire.** Le 2/4 du
run armé venait de la passe LDS ; or c'est précisément cette passe que le défaut C1 paralyse
quand le plan est vide (`claims_size = 1`). Les deux bras ne diffèrent donc pas d'un seul
facteur : ils diffèrent du répertoire ET d'un mécanisme cassé. **La mesure est à refaire après
C1.** Elle est conservée ici parce qu'elle établit tout de même que le mode FONCTIONNE de bout
en bout — duel synthétique, cible posée, 700 décisions, zéro erreur de core, board atteint et
replay d'approche écrit — ce qui était le livrable (c) de la session.

**Ce que la session 8 a obtenu, et ce qu'elle n'a pas obtenu.** Pas de ligne vers le board
cible en mode but seul, ni sur A ni sur B. Mais : (1) une revue qui a **changé le programme du
projet** — le problème a un jumeau publié (rétrosynthèse sous contrainte de matériaux de
départ), un résultat négatif obtenu sans dépenser un run (le HER propositionnel ne débloquera
pas 3× Liger), et une troisième famille de leviers identifiée, ceux qui touchent l'EXPOSANT ;
(2) le mode but seul, implémenté et auditable ; (3) √LTS remis dans sa forme publiée et mesuré
sur un montage DÉTERMINISTE, avec la découverte que le verdict de la session 7ter portait sur
un mécanisme s'exécutant 2 % du temps ; (4) le diagnostic qui commande la suite — *le verrou
n'est plus l'algorithme de recherche, il est dans `h`* ; (5) un audit systématique qui a
retiré deux conclusions de cette session même, et dont le plan de correctifs
(`docs/plan-correctifs.md`) ouvre la session 9.

**Non fait, et documenté comme tel** (session 8) : les correctifs C1-C3, qui bloquent
l'interprétation des mesures du mode ; le graphe de recettes (chantier 16) ; les options
(chantier 17) ; la perte de Levin comme objectif (chantier 15) ; le descripteur grossier
d'archive (chantier 14) ; le répertoire rétrospectif filtré (chantier 12) ; chantiers 4, 6bis,
7, 8 inchangés.

### 9.16 Session 9 : les instruments d'abord — trois correctifs qui retirent des conclusions, et six mécanismes chiffrés pour la première fois

**La mission** : `docs/plan-correctifs.md`, points C1 à C3, avant toute nouvelle mesure. Deux
d'entre eux invalidaient des conclusions publiées ; le troisième rendait illisible le cadran
qui portait le seul résultat positif de la session 8. Ce qui suit est ce qui a été corrigé, ce
que les instruments ont dit dès qu'ils ont existé, et ce que cela retire.

**(a) C1 — la partition entre workers s'auto-verrouillait, et le mode BUT SEUL en mourait.**

`DescendTransplant` réclamait une case par *hachage du digest d'état* dans un tableau
dimensionné sur `plan.size() + 1`, jamais relâché. Deux défauts se composaient : deux états
distincts pouvaient tomber sur la même case — le second était alors interdit à tout le monde —
et **en mode but seul le plan est vide, donc `claims_size = 1`**. Un jeton pour seize workers.
Toute déviation était supprimée, sans qu'aucun compteur ne le dise.

La correction remplace le tableau par une `ClaimTable` en adressage ouvert **sur la clé
entière** : une case refusée l'est parce que le point est déjà pris, jamais par collision. La
clé est l'indice de référence en réparation (une vraie partition de la ligne, comme
`RunRepair` le faisait déjà) et le digest d'état en transplantation, où aucun indice linéaire
n'existe. À saturation elle **échoue OUVERT** : du travail refait coûte du temps, un point
perdu serait un trou de complétude — et une preuve d'absence fausse.

Et surtout : les branches cédées sont **comptées** (`claim_denied`) et imprimées. C'est la
distinction que la session 8 n'avait pas — *partagé* et *supprimé* produisaient la même sortie.

**(b) C2 — « ÉPUISÉ » n'était pas une preuve d'absence.**

Neuf sites coupaient sur le plafond de profondeur ou d'actions sans rien incrémenter, puis
`stats.exhausted` s'écrivait « ÉPUISÉ ». Le champ `SearchStats::edges_skipped` existait depuis
des sessions et n'avait **jamais été écrit ni lu**. Il l'est désormais aux neuf sites, plus les
deux sorties par le haut des boucles de tirage ; et `SearchOutcome()` refuse d'écrire
« ÉPUISÉ » quand il est non nul — « EPUISE SOUS BORNE » à la place. Le même verdict couvre
maintenant les deux plafonds qui n'apparaissaient nulle part (`noeuds`, `memoire`) et l'arrêt
sur quota de solutions, qui s'affichait en colonne **vide**, indiscernable d'un arrêt inexpliqué.

*Le contrôle de correction de la session 8 tient* : la racine `recul 0` de l'A/B déterministe
rend **42 expansions, `b=0`, ÉPUISÉ** dans tous les bras mesurés cette session. La preuve de
correction de la session 8 survit à son propre instrument — ce qui n'allait pas de soi.

*Mais l'instrument mord ailleurs* : sur les racines profondes (reculs 20, 30, 45) il affiche
`b = 1` à `7`. Le plafond coupe bel et bien, et aucun run antérieur ne pouvait le savoir.

**(c) C3 — `h_root` ne réalisait pas l'Eq. 7, et la correction dit plus que prévu.**

`h_root` valait `|cible| + Σ resolve_min` — une constante du PROBLÈME, soit 8 sur l'étalon B —
là où l'Eq. 7 de arXiv:2605.30664 demande `h` à la racine de la **recherche courante**. Il est
désormais évalué au nœud 0 (plancher à 1) et **imprimé** en colonne `h0=`.

Le chiffre a immédiatement démenti l'hypothèse de travail du plan de correctifs, la mienne
comprise. On attendait `h_root ≈ 1` uniformément, le finisseur partant d'un état « déjà à 7/8
cartes ». Le relevé :

| racine | recul 0 | recul 10 | recul 20 | recul 30 | recul 45 |
|---|---|---|---|---|---|
| `h0` mesuré | 1 | 4 | 5 | 6 | 5 |

`h0 = 1` ne vaut que pour la racine `recul 0` — celle qui épuise en 42 expansions et ne pèse
rien dans la comparaison. Les quatre racines effectivement comparées sont à 4, 5 et 6.

**Ce que cela retire au cadran α de la session 8, et c'est plus grave qu'une échelle fausse.**
Avec `h_root` constant à 8, un même α produisait le coefficient effectif `α/8` à *toutes* les
racines, quelle que soit leur distance au but. Le cadran 8/15/25/40/60 n'a donc pas seulement
été parcouru à la mauvaise échelle : **il mélangeait des racines auxquelles Eq. 7 prescrit des
coefficients différents**. Corrigé, le coefficient est `α/h0` et s'adapte à chaque racine —
une racine à 6 cartes manquantes reçoit une rampe plus douce qu'une racine à 4. C'est ce que
la normalisation de l'article veut dire, et le mécanisme ne l'avait jamais fait.

L'équivalence entre les deux échelles est `α_s9 = α_s8 · h0/8`, soit un facteur 0,5 à 0,75 —
et non le facteur 1/8 que le plan de correctifs annonçait, lui aussi écrit sous l'hypothèse
`h_root = 1`. La correction du plan est notée ici plutôt que passée sous silence : elle a
coûté un cadran mal centré (voir (d)).

**(d) Le cadran α refait et PROLONGÉ — le rerooter doux est réfuté.**

Montage étalon 0 inchangé (racines imposées par `--approach`, politique vide par `--no-nrpa`,
régime but seul par `--no-plan`) : l'A/B est DÉTERMINISTE, seule la fonction de coût change.
Le contrôle `recul 0` rend **42 expansions, `b=0`, ÉPUISÉ dans les neuf bras** — une fonction
de coût change l'ordre, pas l'ensemble atteignable.

Un premier cadran (α = 1/2/3/5/8) a été construit sur l'arithmétique fausse ci-dessus
(`α_s9 = α_s8/8`) et couvrait donc **entièrement sous** le point où la session 8 avait vu son
gain. Il a été refait et prolongé jusqu'à α = 28 — `tools/s9_ab_alpha_complet.ps1` — sur un
**seul binaire**, parce que le gating de la nouveauté (audit 3.7) avait entre-temps changé le
débit de +20 % : prolonger sur un autre binaire aurait comparé des bras qui n'explorent pas à
la même vitesse.

**Le cadran PROLONGÉ, neuf bras sur un seul binaire, et ce qu'il établit.** Le cumul seul ne
dit rien d'interprétable ; il faut le lire avec `rr`/expansion, c'est-à-dire avec le taux
auquel le re-enracinement bat la prolongation. Témoin = 20 cartes cumulées.

| α | 1 | 2 | 3 | 5 | 8 | 12 | 16 | 20 | 28 |
|---|---|---|---|---|---|---|---|---|---|
| cumul | 18 | 17 | 17 | 18 | 18 | **20** | **20** | **21** | **20** |
| `rr`/expansion | 2,3–2,8 | 2,1–3,6 | 2,1–3,2 | 2,2–3,1 | 2,1–3,4 | 1,5–3,9 | 0,1–1,3 | 0,1–0,4 | **0,0** |

Les deux lignes racontent **une seule** histoire, et elle est monotone.

- **α ≤ 8 : le rerooter est SATURÉ.** Il se re-enracine 2 à 3 fois par expansion, c'est-à-dire
  sur la majorité des arêtes engendrées. Le coût dégénère en mesure locale — et il **perd**
  (17-18 contre 20).
- **α ≥ 16 : le rerooter est INERTE.** À α = 28 il ne se déclenche plus du tout (`rr` = 2 et 8
  sur des dizaines de milliers d'expansions).
- **Et c'est exactement là que le cumul rejoint le témoin** : 20, 20, 21, 20.

**Le « gain » de la session 8 est donc expliqué, et il n'en était pas un.** α_s8 = 25 correspond
à α_s9 ≈ 12-19 — précisément la bande où le mécanisme s'éteint. Le 21 contre 20 n'est pas un
rerooter qui travaille mieux : c'est un rerooter qui **se débranche**, la recherche qui redevient
le témoin, et une carte d'écart qui est du bruit de run. Le seul point du cadran au-dessus du
témoin se trouve dans le régime où le mécanisme ne fait plus rien.

**Il n'existe aucun α où le rerooter doux gagne en travaillant.** Quand il agit il perd ; quand
il ne perd plus, c'est qu'il n'agit plus. Le mécanisme est désactivé par défaut et le reste.

*Ce que j'ai écrit à mi-parcours et qui était faux.* Sur la seule plage α ∈ [1, 8], `rr` ne
bougeait pas, et j'en avais tiré une explication structurelle — « `hu` compose le long du chemin
alors que `inv_w` est borné, donc prolonger ne peut plus gagner, quel que soit α ». La plage
complète montre une transition nette entre 12 et 20 : l'affirmation « quel que soit α » était
une extrapolation depuis un tiers du domaine. Le mécanisme d'extinction est bien celui décrit
(la compétition entre un terme qui compose et un terme borné), mais le point de bascule est
atteignable, et c'est le prolongement du cadran qui l'a montré — piège 46, encore.

**(e) Six mécanismes d'élagage chiffrés pour la première fois.**

Une ligne `elagage :` sous chaque passe, avec six compteurs qui existaient tous et dont aucun
n'était imprimé (piège 52) :

| colonne | mécanisme | statut avant |
|---|---|---|
| `contrainte` | `--summon-min` / `--material` | six incréments, zéro lecture |
| `garde` | `--guard` | un incrément, zéro lecture |
| `tour` | ligne débordant du tour 1 | agrégé dans `ModeStats`, puis abandonné |
| `borne` | plafond décisions/actions | jamais incrémenté (C2) |
| `partition` | branches cédées à un autre worker | n'existait pas (C1) |
| `sous-ens.` | énumérations tronquées | n'existait pas (C9) |

Et le tiers manquant de `reroots` : le compteur ne s'imprimait que dans la table des racines
d'`--approach`, donc **jamais en mode but seul sans `--approach` — le mode que la session 8
venait de construire**. Il est maintenant dans la table LTS principale et en phase 2, avec
`h0=` à côté.

**(f) C9 — un trou de complétude, confirmé en lisant le code.**

Le plan le donnait `[rapporté]` ; c'est `[vérifié]`. `ForEachSubset` annonce en commentaire
« on privilégie les tailles extrêmes » et énumère par taille **croissante** en s'arrêtant au
plafond. Avec `cap = 24` et 24 candidats, les 24 émissions sont les 24 **singletons** — aucune
paire, jamais. Sur `MSG_SELECT_SUM` (somme de niveaux, tributs) une sélection d'une seule carte
ne satisfait presque jamais la contrainte : le prompt devenait stérile en silence, et toute
preuve d'absence portant sur une invocation Synchro ou par tribut s'en trouvait affaiblie.

Corrigé : les tailles alternent depuis les deux bouts (min, max, min+1, max-1…), et toute
troncature est comptée (`subsets_capped`, colonne `sous-ens.`) puis retirée à « ÉPUISÉ » sa
valeur de preuve, exactement comme un plafond de profondeur. Le paramètre lui-même, écrit en
dur à douze endroits, est exposé (`--max-subsets`) — c'est lui qui plafonne le facteur de
branchement de TOUS les prompts de sélection.

**(g) Défaillances silencieuses et constantes cachées.**

- **C8** — dix-neuf sites retournaient sur échec d'`Arena::Init` ou de `Duel::Setup` sans
  imprimer `err`. Un bras d'A/B qui n'a jamais tourné affichait `0 solutions, 0 etats` :
  indiscernable d'un bras qui a tourné et n'a rien trouvé. Tous passent par `WorkerAbort`, qui
  nomme le site et le message.
- **C11** — `AdaptCorpus` passait sept arguments pour huit paramètres : `hint_bias` forcé à 0 et
  `temp` laissé au défaut. `--adapt` combiné à `--nrpa-temp` était donc incohérent, et le
  gradient du corpus calculé sous une distribution différente de celle qui échantillonne —
  précisément sur les coups indicés, c'est-à-dire les rips. Les deux paramètres sont désormais
  **exigés sans défaut** : la même omission ne peut plus être silencieuse. Le chemin du RUN
  passe les valeurs de la config ; le chemin du RAPPORT passe des constantes nommées
  (`kReportHintBias`, `kReportTemp`), neutres et explicites, pour que les chiffres du §9.14
  restent comparables — aligner les trois instruments du rapport sur le run reste à faire, et
  re-mesure le §9.14.
- **C12** — `--no-ref` dérivait quand même `max_decisions` de la référence. Le plafond vient
  maintenant de la decklist (12 décisions par carte + 32) sous ce drapeau, et il est **imprimé
  dans tous les cas**, avec sa provenance.
- **C13** — trois encodages compacts clampaient sans avertir, dont deux gouvernent des grandeurs
  qui ont servi à décider (le score d'archive ordonne les états conservés ; `ContextKey` borne
  la segmentation lue par `ForecastSearchCost`, l'un des deux nombres qui ont décidé d'écrire
  sqrt-LTS). Vérifiés une fois au démarrage.
- **C14** — `LiftPlan` jetait sa valeur de retour sur le chemin `--fire` : un plan à 90 % de
  trous servait de répertoire comme s'il était complet, et l'échec des fenêtres était imputé à
  la recherche. Le nombre d'étapes non identifiées est imprimé, avec un avertissement au-delà
  de la moitié.
- **C15** — `nrpa_level = (budget > 180000.0) ? 3 : 2` changeait le coût d'un appel de niveau de
  ~576 à ~13 824 tirages, c'est-à-dire **l'algorithme d'échantillonnage lui-même**, sans
  qu'aucune mesure ne l'adosse — et le seuil tombait exactement sur la ligne de partage des
  commandes comparées aux sessions 5-7 (600 s sans `--finisher-min` → niveau 3 ; le *même* run
  avec `--finisher-min 420000` → niveau 2). Exposé (`--nrpa-level`), et le niveau effectif est
  imprimé dans tous les cas. L'A/B des deux valeurs à budget égal reste à faire.

**(h) Santé.** Diff avant/après du run de santé, correctifs C1-C16 compris (donc y compris le
changement d'ordre d'énumération de C9, qui pouvait légitimement bouger le résultat) :
**uniquement des durées, plus les lignes `elagage :` nouvellement ajoutées**. Identiques : 0
MSG_RETRY, 273 digests, 210/273 coups identifiés, référence retrouvée à 0 écart, 209 solutions,
19/56/272, « résiste à 12 déviations », 16 replays.

Ce que la nouvelle ligne dit, dès le premier run :

```
elagage : contrainte 0, garde 0, tour 0, borne 4, partition 4, sous-ens. 1
```

Trois faits qu'aucun run antérieur ne pouvait produire. `borne 4` : le plafond coupe, dans le
run de santé lui-même. `partition 4` : le travail est **cédé** à un autre worker, pas supprimé —
c'est exactement la distinction qui manquait à la session 8. `sous-ens. 1` : le plafond
d'énumération mord une fois par passe, sur la ligne de référence, et personne ne le savait.

**Ce que la session 9 a obtenu.** Les trois correctifs bloquants avec leur vérification ; six
mécanismes d'élagage chiffrés ; un trou de complétude fermé ; dix-neuf défaillances silencieuses
rendues bruyantes ; trois constantes cachées exposées ; **l'audit complet fermé** (section (i)) ;
**le rerooter doux réfuté** sur un cadran de neuf bras (section (d)) ; et **le chantier 16
ouvert, implémenté et mesuré** — comptage validé contre une vérité humaine, graphe de recettes
observationnel dont la limite est établie comme structurelle, amorce par le texte de carte
(sections (j) et (k)).

**Ce qu'elle n'a pas obtenu.** Un `h` qui décroît *utilement* : le graphe observationnel
n'ajoute que ~0,1 de gradient, et l'amorce par le texte, une fois débarrassée des routes mortes,
ne donne un gradient que sur les cartes dont un matériau nommé est dans le deck. Les exigences
d'archétype et de niveau ne sont pas traitées. Aucune conversion n'est attribuable au mécanisme.

**Ce qui reste, nommément** : les exigences d'ARCHÉTYPE et de NIVEAU dans l'amorce (le substrat
existe : `CardRow::setcodes`, `CardRow::level`) ; les nœuds OU à plusieurs fournisseurs (copie
de nom, substituts de Fusion) ; l'A/B `--nrpa-level` ; et toute la section 6 de l'audit — les
optimisations du chemin chaud, dont la calibration est connue (~250 µs par décision, dominée
par `duel.Process()` et `arena.Restore()`).

**(i) Le reste de l'audit — traité dans la même session.**

Le plan de correctifs (C1-C18) était l'extrait priorisé d'un audit plus large. Le reste a été
traité ici plutôt que reporté, parce que six de ses points sont de la même famille que C1 :
des défaillances qui **produisent un chiffre plausible** au lieu de produire une erreur.

*Ce qui corrompt ou biaise silencieusement une mesure.*

- **L'arène qui déborde empoisonnait le duel en silence.** Une allocation qui ne tient pas dans
  l'arène part sur le tas de l'hôte ; `Restore()` ne peut pas la restaurer, donc **tout ce que
  le worker mesure ensuite porte sur un duel divergent**. La seule alarme était un compteur
  `thread_local` lu depuis le thread principal, alors que les replis se produisent dans les
  **workers** : le rapport écrivait « aucune : tout l'état est capturé » *par construction*.
  Corrigé : compteur atomique membre d'`Arena`, drapeau `poisoned` **collant** qui fait avorter
  la recherche via `BudgetExhausted()`, `ReportPoison` sur les neuf workers, et le statut
  `!! ARENE CORROMPUE` passe **devant** « ÉPUISÉ » dans `SearchOutcome`.
- **`ResponseForbidden` échouait OUVERT.** Tous ses chemins d'échec rendaient *autorisé*, sur
  une disposition de message ocgcore codée en dur. Une dérive de format faisait cesser
  `--no-activate` et `--no-chain` de filtrer — en silence, tout en restant crus. Tri-état
  désormais : `Undecodable` est traité comme **interdit** (on retire la branche au lieu de
  l'admettre sans contrôle) et compté ; non nul, le run est déclaré à jeter, code de sortie 1.
- **Un replay tronqué se chargeait « avec succès ».** `LoadFromBuffer` rendait `true`
  inconditionnellement ; un corps tronqué donnait un deck court et une liste de réponses
  courte — et cette liste courte devient `ref_decisions`, c'est-à-dire **le plafond de
  décisions de toute la recherche**. Un problème d'octets se propageait en budget
  silencieusement réduit. Les trois boucles vérifient maintenant le compte annoncé et
  échouent avec un message précis.
- **Un code absent de `cards.cdb` devenait une vanille muette.** C'est le jumeau *base de
  données* du décalage de scripts, et il était plus silencieux : `ScriptProvider::Misses()`
  existait et s'imprimait, il n'y avait aucun équivalent côté cartes. `CardDB::UnknownCodes()`
  comble le trou.
- **Une lecture courte de script se déguisait en « fichier absent »** et faisait charger la
  même carte depuis un dépôt de rang inférieur — donc **une autre version**. Le chargeur
  fabriquait, à partir d'une erreur d'E/S, exactement le décalage de jeux de scripts que ce
  dépôt redoute, sans jamais atteindre `misses`. `Unreadable()` est distinct de `Misses()`, et
  le chargeur ne se rabat plus.
- **`--summon n:X` était satisfait par vacuité** par le contrôle « vérifié avant écriture, qui
  ne souffre pas d'exception » : une ligne qui atteint le board en moins de n invocations était
  écrite comme conforme et comptée dans « N lignes atteignant le board », alors que la
  contrainte autour de laquelle l'expérience est bâtie n'avait **jamais été exercée**. Elle
  échoue désormais, avec un compte distinct des contraintes *violées*.
- **`ProcessorState` vide défaisait le patch C1** — sans la composante d'état de processeur,
  deux instants d'une même résolution de chaîne se confondent et la branche du combo est
  élaguée dès le début. Compté et rapporté.

*Ce qui rendait un A/B illisible.*

- **`lam` saturait à 1e300 et `reroots` était gonflé par l'infini.** `seg_logpi` accumule des
  log-probabilités ; passé ~709, `exp` déborde, `log(lam - 1)` devient **constant pour toute la
  descendance** et le best-first √LTS dégénère. Symétriquement, quand `hu` part à l'infini, la
  comparaison `ext_v <= new_u` bascule pour raison **purement arithmétique** et la branche
  « re-enracinement » est prise *et comptée* — donc `stats.reroots`, dont le commentaire dit
  qu'il est l'instrument de « à zéro le mécanisme est inerte », devenait non nul exactement
  quand le calcul avait cassé. Trois compteurs séparés (`levin_overflow`, `reroot_by_overflow`,
  `lam_saturated`) et un `!!NUM` dans la table : à non nul, **le bras est à jeter, pas à
  interpréter**.
- **Le plafond de profondeur du finisseur se repliait sur un 64 magique**, écrit cinq fois et
  muet : deux bras comparés « à budget égal » pouvaient recevoir des budgets de **profondeur**
  différents. `FinisherDepth()` centralise et compte.
- **`WriteSolutions` jetait tout au-delà de la 16ᵉ solution sans dire combien il en avait.** Le
  run de santé écrit 16 replays — sur **209 candidates**. Aux sites qui ne trient pas d'abord,
  les seize retenues ne sont pas les moins chères, ce sont les seize **arrivées en premier**.
- **`--novelty 0` n'éteignait pas la nouveauté côté NRPA** : `CollectAtoms` + `Observe`
  tournaient inconditionnellement dans la boucle interne de `PolicyRollout` — quatre requêtes
  au core, quatre tris et ~60-80 sondes de table **par décision** — pour un simple terme de
  départage, et c'était aussi la principale allocation **non bornée** du run. Le bras témoin
  « sans nouveauté » du contrôle A/B ne couvrait donc que les passes LDS. Gaté. Effet mesuré au
  passage sur l'étalon 0 : **+20 % d'expansions à budget égal**, meilleurs par racine
  identiques (6/5/4/5) — une observation isolée, à comparer à la dispersion (piège 39).

*Six instruments de plus, tous déjà présents dans le code et jamais lus* : `dead_ends` et
`terminals` (le symptôme n°1 du jeu de scripts décalé, muet exactement dans le mode où il se
produirait), `novelty_novel`/`novelty_stale` (le run de santé dit **3 nouveaux sur 23** — le
terme de départage est presque saturé), `novelty_atoms` (79), `expansions_by_depth` (le facteur
de fusion réel de la table), et le compte de prompts **réduits à la réponse par défaut** —
lesquels ne sont pas un élagage mais un pan d'espace qui n'a jamais existé.

*Constantes exposées ou imprimées* : `--hint-bias` (le canal par lequel la connaissance du
joueur entre dans l'échantillonnage ; son voisin `--nrpa-bias` existait, pas lui), le partage
du budget entre phases (0,7 / 0,2 / 0,75 / 0,8 / plafond 240 s), et la portée **réelle** de
`--tt-mb` (passes LDS seulement — ni `RunLevin`, ni `RunNrpa`) et de `--nrpa-lr` (phase de
tirages seulement) documentée là où elle se lit.

*Contradictions doc/code résolues* : le commentaire de `search.h` portait encore le diagnostic
« le plafond est la REPRÉSENTATION » que le §9.13 a explicitement **rétracté** ; les reculs
d'`--approach` documentés {0,10,30,60,90,120} ne sont pas ceux du code ({0,10,20,30,45} au LTS,
{60,70,80,90,110,150} en phase A2) ; et la promesse « EXACTEMENT la même mise à jour » de
`AdaptRun` était démentie par `AdaptCorpus`.

**Ce qui reste, explicitement.** La section 6 de l'audit — les optimisations du chemin chaud
(C19-C28 : jeux de drapeaux de requête séparés, `EntryOf` sur la pile, hachage 8 octets,
`CommonCodes` calculé une fois, `LNode` en ligne, `path` en pile de tampons) — sauf le gating
de la nouveauté, qui a été fait parce que c'était **aussi** une correction de `--novelty 0`.
Et les constantes de moindre valeur (`max_solutions` variable selon le site, `k <= 12`,
`plan_window`, `repair_window`), exposées nulle part et non mesurées.

**(j) Le chantier 16 — le comptage et le graphe de recettes, implémentés et mesurés.**

C'est la mission de fond, et elle repose sur un diagnostic mesuré : *le verrou n'est plus
l'algorithme de recherche, il est dans `h`*. Notre `h` compte les cartes cibles manquantes, et
il ne bouge pas tant qu'aucune n'est posée — c'est-à-dire sur l'essentiel de la ligne. Un
mécanisme de décomposition ne peut rien décomposer sur un paysage plat.

**Ce qui rend la rétrosynthèse applicable ici, et qui n'était pas évident.** On ne peut pas
inverser un ÉTAT DE DUEL — c'est ce qui avait fait écarter la recherche en arrière. Mais on
n'en a pas besoin : il suffit d'inverser une INVOCATION. Et là, non seulement les règles du jeu
donnent le template, mais **le core nous dit ce qui a été consommé** : `MSG_MOVE` porte
`REASON_MATERIAL`, et sa charge utile contient la **zone d'origine** du matériau. Notre modèle
rétro à un pas est donc plus facile que celui de la chimie — qui exige un réseau entraîné sur
12 millions de réactions — et il est *observé*, pas prédit.

**Les trois règles, et comment chacune est tenue dans le code.**

1. *Le nœud est une EXIGENCE, pas une carte* : `Requirement{code, zone}`. La zone est
   normalisée en six seaux (`NormalizeZone`) **des deux côtés** — à l'observation du matériau
   et au test de présence ; les désaccorder rendrait toute exigence insatisfaisable et la
   distance serait fausse dans le sens dangereux, c'est-à-dire plate à nouveau. Une carte qui
   COPIE un nom devient un fournisseur de plus sous le même nœud, gratuitement : le code
   observé est déjà le code **effectif**.
2. *Le graphe ne PRUNE jamais* : `Distance` rend **1** pour un produit sans recette connue, et
   **1** quand le budget de récursion s'épuise — jamais l'infini. Conséquence directe et
   voulue : **graphe vide ⇒ distance = nombre de cartes manquantes = le `h` d'aujourd'hui**.
   Activer le mécanisme ne peut donc pas rendre un but inatteignable. C'est ce qui le rend sûr,
   et c'est la forme opérationnelle de l'évitement du piège 47.
3. *La vérité vient de l'observation* : aucun texte de carte n'est lu. Une Fusion depuis la
   zone Pendule, un substitut de Fusion et une invocation ordinaire s'enregistrent exactement
   de la même façon.

**Ce que le graphe change à `h`, concrètement.** Poser Leo Dancer au cimetière ne pose aucune
carte cible — le `h` plat ne bouge pas. Sur le graphe, la distance à Liger Dancer passe de 2 à
1, parce qu'un de ses matériaux est désormais satisfait. *C'est exactement la décroissance en
cours de ligne qui manquait.*

**Le mode `--recipes 0`, et pourquoi il existe.** Le graphe est alors **alimenté et mesuré mais
n'entre pas dans le coût** : le run est identique au témoin par construction, et la colonne
`hR` dit ce que le graphe *aurait* dit. C'est le piège 40 appliqué à soi-même — instrumenter
avant de calibrer — et c'est ce qui permet de répondre à la vraie question (*ce `h` diffère-t-il
du `h` plat ?*) avant la question dérivée (*fait-il gagner ?*). Un `rec=` à zéro signifierait
que le graphe est vide et que tout bras `--recipes w` n'a rien mesuré (piège 52).

**Coût, et où il est payé.** `RecipeDistance` relève cinq zones cachées par appel. Il n'est
appelé **que dans `RunLevin`, au développement d'un nœud** — jamais dans les tirages, où
l'audit (6.1) a montré que ce genre de bloc domine le chemin chaud. La table de présence est
triée et sondée par recherche binaire, et la récursion est mémoïsée : sans mémo elle est
exponentielle (`recettes × matériaux` par niveau, ~40⁶ dans le pire cas), ce qui ferait **pendre**
le finisseur au lieu de le ralentir.

**PRÉDICTION, écrite avant la mesure.** Le graphe n'apprend que des invocations **réussies**.
Or sur l'étalon 0 la carte manquante est précisément celle qu'aucune ligne n'a jamais réussi à
poser. Le graphe ne devrait donc connaître **aucune recette pour elle**, `Distance` devrait
rendre son plancher de 1, et `hR` devrait valoir ≈ le nombre de cartes manquantes — c'est-à-dire
**exactement le `h` plat**. Attendu : `rec=` élevé (les préfixes rejoués observent beaucoup
d'invocations), `hR` ≈ 1, et aucune séparation entre les bras.

Si c'est ce qui se produit, ce n'est pas un échec du graphe mais la mesure d'une limite
précise : *la moitié OBSERVATIONNELLE seule ne peut rien dire de la carte qu'on n'atteint
jamais*. La règle 3 dit « le texte n'est qu'une AMORCE, la vérité vient de l'observation » — je
n'ai implémenté que la seconde moitié. Amorcer le graphe avec les matériaux nommés par le texte
de carte donnerait une recette à la carte non atteinte, et c'est l'observation qui la
corrigerait ensuite. C'est cela que la mesure doit trancher.

**MESURE — la prédiction est confirmée, au chiffre près.** Bras `--recipes 0` sur l'étalon 0 :

```
  appr0 recul 0   42 exp.  best 7/8  rec=121 hR=1.0  b=0  EPUISE
```

Le graphe a bien observé (121 invocations, versées par les rejeux du préfixe d'approche) et sa
distance vaut **exactement 1,0** — le nombre de cartes cibles manquantes, c'est-à-dire le `h`
plat, à la décimale près. La règle 2 a fonctionné comme prévue : recette inconnue ⇒ plancher ⇒
`h` inchangé, aucune régression possible.

Aux racines profondes, le graphe observe massivement — `rec=` monte à 44 324 invocations — et
`hR` vaut 4,1 / 5,0 / 5,1 / 5,0 là où le `h` plat vaut 4 / 5 / 6 / 5. **Le gradient ajouté est
d'environ 0,1**, soit ~2 %.

*Le mode « mesurer sans décider » est lui-même validé* : le bras `--recipes 0` rend des `best`
**identiques** au témoin (7/6/5/4/5) et des expansions à 1,3 % près — l'écart est le coût des
cinq requêtes de zone par nœud développé. Le mode fait donc ce qu'il promet : il chiffre ce que
le graphe *dirait* sans rien changer à ce que la recherche *fait*.

**Ce que cela établit, et c'est un résultat, pas un échec.** Le graphe observationnel apprend
des invocations RÉUSSIES ; la carte qu'on cherche est celle qu'aucune ligne n'a jamais posée.
Il ne peut donc rien en dire — *par construction, pas par manque de données*. Attendre plus de
tirages n'y changerait rien : c'est la structure du signal qui est en cause.

**D'où l'amorce par le texte, implémentée dans la foulée.** La première ligne du texte de carte
est la ligne de matériaux, et son format est régulier :

```
Lunalight Liger Dancer : "Lunalight Leo Dancer" + 3 "Lunalight" monsters
Number 41: Bagooska    : 2 Level 4 monsters
```

On n'en prend que les matériaux **nommés entre guillemets et résolus à une carte existante** :
les exigences d'archétype (`3 "Lunalight" monsters`) et de niveau (`2 Level 4 monsters`) sont
ignorées, parce qu'elles sont presque toujours faciles à satisfaire — elles ajouteraient du
bruit sans gradient — et parce que les omettre **sous-estime** le coût, ce qui est la direction
sûre au regard de la règle 2. La zone est un JOKER (`kZoneAny`) : le texte nomme un matériau
sans dire d'où il vient, et c'est l'observation qui le précisera ensuite. `--no-seed-recipes`
éteint l'amorce pour isoler sa contribution.

Sur le cas qui commande tout — Liger Dancer exige NOMMÉMENT « Lunalight Leo Dancer » — l'amorce
donne enfin une recette à la carte jamais atteinte, donc une distance supérieure au plancher,
donc un `h` qui distingue les états. C'est exactement le nœud OU de la règle 1 : l'exigence est
« un monstre du nom de Leo Dancer », que le vrai Leo Dancer satisfait, et qu'une carte ayant
copié ce nom satisfait aussi.

**LA ROUTE MORTE, et pourquoi elle a fait corriger l'amorce.** Une première trace donnait la
chaîne `Liger ← Leo ← Panther Dancer` et une distance de 4 pour Liger. Deux erreurs s'y
cachaient, et la seconde était de fond.

*La première était de méthode* : ma trace hors solveur recursait dans le texte de n'importe
quelle carte, alors que le code ne pose de recette que pour les cartes CANDIDATES (extra deck
et cible). Sous le code réel, Panther Dancer n'a pas de recette et Liger vaut 3, pas 4. Une
trace qui ne reproduit pas le code ne trace rien.

*La seconde était de modèle, et c'est elle qui compte* : **Panther Dancer n'est pas dans le
deck de l'étalon A.** Or c'est le seul matériau nommé par la ligne de Leo Dancer. La recette
texte de Leo décrit donc une voie que ce deck **ne peut pas emprunter** — et Leo y est pourtant
invocable, par substitut de Fusion, par copie de nom, ou par un effet qui ignore les matériaux.
Le texte décrit UNE voie, pas LA voie.

Compter une étape pour un matériau introuvable, ce n'est pas du bruit : c'est un coût fondé sur
un chemin impossible, et il aurait faussé `h` dans le sens le plus trompeur — en récompensant
la poursuite d'une route morte. L'amorce écarte donc toute recette nommant un matériau **absent
de la decklist**, et le produit retombe au plancher : « on ne sait pas comment il arrive » est
plus vrai que « il coûte le prix d'une route impossible ». C'est la règle 2 au sens strict — on
n'invente pas de coût, et on ne déclare rien inatteignable non plus.

| carte cible | `h` plat | distance amorcée (code corrigé) |
|---|---|---|
| Lunalight Liger Dancer | 1 | **2** |
| Lunalight Leo Dancer | 1 | 1 (sa seule voie texte est morte dans ce deck) |
| Number 41: Bagooska | 1 | 1 (« 2 Level 4 monsters » : exigence de niveau, ignorée) |

Le gradient survit là où il est utile : Liger vaut 2 tant que Leo n'est pas là, et **1 dès que
Leo est posé** — or poser Leo Dancer ne pose aucune carte cible, donc ne bouge pas le `h` plat.
C'est exactement le trou que la session 8 avait identifié, et il est comblé sur ce cas.

Il est aussi plus étroit qu'espéré, et la limite est nette : les exigences d'ARCHÉTYPE
(`3 "Lunalight" monsters`) et de NIVEAU (`2 Level 4 monsters`) sont ignorées. Le substrat pour
les traiter existe (`CardRow::setcodes`, `CardRow::level`) ; c'est le prochain pas.

**Et cette omission a une conséquence qu'il fallait traiter dans le code, pas seulement noter.**
Une recette amorcée, parce qu'elle omet ces exigences, est **systématiquement moins chère**
qu'une recette réellement observée pour le même produit. Or `Distance` prend le `min` sur les
recettes : l'amorce l'aurait donc emporté à tous les coups, et l'observation n'aurait **jamais**
pu la corriger — ce qui retourne la règle 3 exactement à l'envers, en préférant une estimation
tirée d'un texte à un fait constaté dans le duel.

Les recettes portent donc un drapeau `primed`, et `Distance` **ignore les recettes amorcées dès
qu'une invocation réelle du même produit a été observée**. Une recette d'abord amorcée puis
constatée perd son drapeau : le texte disait vrai, elle devient une observation. C'est la règle
3 tenue dans le bon sens — le texte amorce, l'observation tranche.

*Validation croisée, gratuite* : l'amorce dérive « Lunalight Leo Dancer » (code 24550676) comme
matériau nommé de Liger. C'est exactement la carte que l'opérateur avait choisie **à la main**
comme `--hint 24550676` dans le montage de l'étalon A depuis la session 7ter. Comme pour le
comptage, le mécanisme dérivé retrouve la connaissance de domaine écrite par un humain.

**Le premier pas, fait avant le graphe : le comptage.** Le board cible seul impose une
arithmétique — « 3× Liger Dancer » veut dire **trois invocations Fusion**. C'est un argument
sur le multi-ensemble cible et la decklist, sans aucun modèle déclaratif. Il donne une borne de
faisabilité et un `--summon-min` **dérivé** au lieu d'écrit à la main (`--derive-summon-min`).
Deux gardes : on compte les **ÉVÉNEMENTS** d'invocation, jamais leurs déclencheurs (« trois
Polymérisations » serait faux — Lunalight Wolf fusionne depuis la zone Pendule sans
Polymérisation) ; et un manque de copies est rapporté comme un **DOUTE**, jamais comme une
impossibilité, parce qu'une carte qui copie un nom satisfait un but jugé sur le code effectif
sans être une copie physique.

La contrainte dérivée écrit dans le **guide de recherche**, pas dans le vérificateur — et ce
n'est pas un contournement : elle est *redondante* à la vérification, pour une raison exacte.
Si le board final porte trois Liger Dancer, alors trois invocations Fusion ont nécessairement
eu lieu, une invocation ne posant qu'une carte. Le vérificateur, qui compare le board final à
la cible, l'a donc déjà vérifiée en vérifiant le board.

**Le comptage est validé contre une vérité écrite à la main**, et c'est le meilleur contrôle
disponible pour un mécanisme dérivé : le montage de l'étalon A porte depuis la session 8 un
`--summon-min "54701958:3"` **tapé par l'opérateur**, choisi en regardant le combo. Le comptage,
qui ne lit que le board cible et la decklist, produit :

```
  exiges  deck  code      mecanisme  carte
  3       3     54701958  Fusion     Lunalight Liger Dancer
  1       1     90590303  Xyz        Number 41: Bagooska the Terribly Tired Tapir

  EVENEMENTS d'invocation exiges par le board seul :  3 Fusion  1 Xyz
```

Il retrouve **exactement** la contrainte humaine (3× Liger), et il ajoute celle que l'humain
n'avait pas écrite (1× Bagooska). L'alias est résolu au passage — la cible était donnée en
`90590304`, le code canonique est `90590303` — ce qui est précisément ce que la règle 1 exige :
le nœud porte l'identité **effective**, pas l'exemplaire d'illustration. Le deck fournissant
exactement 3 et 1, aucun doute de faisabilité n'est levé, ce qui est le bon verdict.

**(k) L'A/B du graphe observationnel — et une conversion qu'il ne faut PAS lui attribuer.**

Étalon 0, montage déterministe, témoin = aucun graphe :

| bras | r10 | r20 | r30 | r45 | cumul | `hR` moyen | solutions |
|---|---|---|---|---|---|---|---|
| T (témoin) | 6 | 5 | 4 | 5 | 20 | — | 0 |
| M `--recipes 0` | 6 | 5 | 4 | 5 | 20 | 4,8 | 0 |
| P1 `--recipes 1` | 6 | 5 | 5 | 5 | 21 | 4,7 | **8** |
| P2 `--recipes 2` | 6 | 5 | 5 | 5 | 21 | 4,6 | 0 |

**Le +1 n'est pas une information de recette.** Puisque `hR` ≈ le `h` plat (le graphe n'ajoute
que ~0,1), `--recipes w` revient à multiplier `h` par (1+w) : c'est un réglage de `--levin-h`
déguisé, pas un apport du graphe. Et un écart de 1 carte sur quatre racines est dans la bande
de bruit que tout le cadran α a montrée (17 à 21 pour un témoin à 20).

**Le bras P1 a écrit 8 solutions sur l'étalon B — et ce n'est PAS attribuable au graphe.**
C'est le résultat le plus spectaculaire de la session et il faut le désamorcer soigneusement,
parce que la tentation de le revendiquer est exactement le genre d'erreur que ce dépôt traque.

Trois raisons, dans l'ordre de force :

1. **Les solutions viennent d'une phase que `--recipes` ne touche pas.** Les lignes `<-- BUT`
   sont sur `appr0 recul 110`, c'est-à-dire la **phase 2 du finisseur**, fondée sur les tirages
   (`RunNrpa`). `RecipeDistance` n'est appelée que dans `RunLevin`. L'algorithme qui a trouvé
   ces lignes est *littéralement le même code* dans les quatre bras.
2. **P2, même mécanisme à poids double, n'a rien trouvé.** Un effet qui disparaît quand on
   renforce sa cause n'est pas un effet.
3. **Piège 39, et il est bien pire qu'annoncé.** Sept runs de la MÊME commande à la MÊME graine
   (888, 90 s, étalon B sans contraintes) :

   | run | best | lignes atteignant le board |
   |---|---|---|
   | 1 | 8/8 | **10** |
   | 2 | 7/8 | 0 |
   | 3 | 7/8 | 0 |
   | 4 | 7/8 | 0 |
   | 5 | 8/8 | **153** |
   | 6 | 7/8 | 0 |

   Deux runs sur six atteignent la forme 8/8, et quand ils y arrivent le compte varie d'un
   facteur 15. **Aucune comparaison à run unique sur l'étalon B ne veut rien dire** — y compris
   le « 7/8, aucune conversion » que le §9.15 donnait comme caractéristique de l'étalon, et qui
   était un run unique tombé du mauvais côté. C'est aussi la raison d'être de l'étalon 0
   (racines imposées + `--no-nrpa`) : il est déterministe, et les conclusions de la section (d)
   reposent sur lui, pas sur ce montage-ci.

**Et le « fait » lui-même doit être requalifié — c'est le point le plus important de la
section.** Une première rédaction annonçait « l'étalon B est converti, à un coût MEILLEUR que la
référence (16/47/226 contre 19/56/276) ». C'est faux dans son sens, et la vérification est
immédiate : **le montage de l'étalon B ne porte aucune contrainte.**

| montage | `--resolve` / `--summon-min` / `--guard` |
|---|---|
| `s8_ab_reroot_butseul.ps1` | aucune |
| `s8_ab_finisseur.ps1` | aucune |
| `s9_ab_alpha_complet.ps1` | aucune |
| `s9_recettes.ps1` | aucune |

Aucun montage de l'étalon B, ni ceux de la session 8 ni les miens, n'exige les **handrips** —
qui sont la raison d'être de cette ligne — ni ne tient la **garde** contre Nibiru. Le mot
« rips » n'apparaît même pas dans le journal, parce qu'aucune résolution n'était demandée.

Ce que ces dix lignes atteignent est donc une **correspondance de forme de board** : les huit
codes sont sur le terrain. Elles ne rippent pas et ne gardent pas. Et le coût inférieur à celui
de la référence ne dit pas qu'elles font mieux — **il dit qu'elles font moins** : la référence
rippe et tient la garde, elles non. Lire 16/47/226 comme une amélioration de 19/56/276, c'est
comparer un trajet complet à un raccourci qui saute les étapes.

**Conséquence sur l'étalon lui-même, et elle porte au-delà de ce run.** « 7/8 » et « 8/8 » sur
l'étalon B signifient « 7 ou 8 des codes cibles présents », pas « le combo fonctionne ». Comme
métrique d'A/B — la même dans tous les bras — elle reste valide, et la réfutation du rerooter
(section (d)) n'est pas touchée. Mais l'étalon B, tel qu'il est scripté depuis la session 8,
**ne teste pas ce que l'étalon A teste** : ce dernier porte `--resolve`, `--summon-min` et
`--hint`. Y ajouter les contraintes de ligne est un préalable avant d'en tirer quoi que ce soit
sur la qualité des lignes trouvées.
**(l) L'étalon B AVEC ses contraintes — et la question du §9.11, tranchée.**

Le montage contraint (`tools/s9_etalon_b_contraint.ps1`) reprend le jeu de la session 7 : garde
`5:Crystal Wing|Zalen@terrain+Junk Signal@main` avec extinction à `mainadv<=2`, les deux rips
(`PSY-Framelord Omega@terrain:2`, `Trishula@terrain`), et `--no-activate` sur Assault Zone. La
référence les satisfait toutes (Omega 2/2, Trishula 1/1).

| montage, 90 s, graine 888 | best | tirages NRPA | fin de tour | garde |
|---|---|---|---|---|
| sans contraintes | 7/8 | 51 729 | 95 % | — |
| **avec contraintes** | **3/8** | 265 840 | 60 % | 15 731 (6 %) |

**Le montage sans contraintes surestimait de 7/8 à 3/8.** C'est l'écart entre « poser les huit
codes » et « poser les huit codes en rippant deux fois avec Omega, une fois avec Trishula, et
en tenant de quoi répondre à Nibiru dès la cinquième invocation ». Le solveur sans contraintes
prenait le raccourci, et rien ne le lui interdisait.

**La question laissée ouverte par le §9.11 est tranchée : la garde n'élague pas l'espace.** Elle
coupe 15 731 tirages sur 265 840, soit **6 %**. Le §9.11 supposait que les fenêtres précoces
étaient « RASÉES sans les negates gratuits, ~400 k tirages morts » — la mesure dit que la garde
n'en est pas la cause.

**Et le plafond de décisions n'est pas la borne qui mord.** 95 % des tirages du montage libre
terminent à la fin du tour 1, pas au plafond de profondeur : le sampler joue des tours COMPLETS
et n'assemble simplement pas le board dedans. Augmenter `--max-decisions` ne peut donc rien
donner — c'est une borne qui ne coupe pas (piège 40), et il aura fallu brancher `turn_cuts` là
où il travaille pour le savoir.

*Note d'instrumentation* : `PrintCuts` affichait `contrainte 0, garde 0` — non parce que rien
ne coupait, mais parce qu'il n'était branché que sur les passes LDS, où ces compteurs valent
zéro par construction. Les contraintes coupent dans les TIRAGES. Un compteur imprimé au mauvais
endroit est aussi trompeur qu'un compteur absent (prolongement du piège 52).
**Pièges ajoutés.**

55. **Un correctif se vérifie avec l'instrument qu'il installe, pas avec l'hypothèse qui l'a
    motivé.** `h_root` a été corrigé sous l'hypothèse « il vaut 1 dans le finisseur » ; la
    colonne `h0=` que la correction même ajoutait a montré 4, 5 et 6. Le cadran construit sur
    l'hypothèse était mal centré. Poser l'instrument, LIRE, puis dimensionner l'expérience.
56. **Une table de partition à index haché n'est pas une partition.** Deux clés distinctes qui
    se disputent une case produisent une SUPPRESSION, pas un partage ; et un dimensionnement
    dérivé d'une autre grandeur (ici la taille du plan) devient nul quand cette grandeur
    s'annule. Clé entière, échec OUVERT, compteur de refus.
57. **Un filtre qui échoue OUVERT est pire qu'un filtre absent**, parce qu'il continue d'être
    cru. `ResponseForbidden` rendait « autorisé » sur toute dérive de format ; l'arène rendait
    de la mémoire hors instantané ; un replay tronqué se chargeait « avec succès ». Chaque fois,
    la panne se déguise en résultat plausible. Règle : sur un chemin qui décode un format
    externe ou qui garantit un invariant, l'échec doit être un TROISIÈME état, distinct du
    succès ET du refus, et il doit être fatal ou compté — jamais silencieusement permissif.
58. **Un compteur `thread_local` lu depuis un autre thread mesure zéro, toujours.**
    `host_fallbacks` imprimait « aucune : tout l'état est capturé » par construction, pendant
    seize workers. Avant de croire un compteur à zéro, vérifier QUI l'écrit et QUI le lit.
59. **« ÉPUISÉ » n'est une preuve d'absence que si tout ce qui tronque l'espace est compté.**
    Aujourd'hui : `borne` (plafonds de profondeur/actions), `sous-ens.` (énumérations
    tronquées), les prompts réduits à la réponse par défaut, et l'arène empoisonnée. Vérifier
    les quatre avant d'écrire une preuve d'absence.
60. **Lire un compteur sur la racine de CONTRÔLE, c'est le lire sur 42 nœuds.** J'ai conclu
    « le rerooter s'éteint quand α monte » d'après `rr` au recul 0 — la racine qui épuise en
    42 expansions et ne pèse rien dans la comparaison. Aux racines qui comptent, `rr` par
    expansion est **constant** sur tout le cadran, et la conclusion s'inverse : le mécanisme
    est saturé, pas éteint. Un compteur se lit là où le mécanisme travaille (piège 53 appliqué
    aux instruments, pas seulement aux A/B).
61. **Un instrument neuf doit être vérifié contre les cas où le mécanisme est CORRECT.** Mon
    détecteur de débordement a allumé `!!NUM` sur toutes les lignes de toutes les racines :
    la racine porte `hu = hv = +inf` PAR CONVENTION — elle n'a aucun ancêtre à prolonger — et
    ses enfants doivent se re-enraciner. L'instrument accusait le mécanisme de sa propre
    définition. Avant de croire une alarme qui se déclenche partout, chercher le cas nominal
    qu'elle confond avec la panne.
62. **Ne jamais tirer une loi d'un tiers du domaine.** Sur α ∈ [1, 8] le taux de
    re-enracinement ne bougeait pas, et j'en avais conclu « quel que soit α » en donnant même
    l'argument structurel qui l'expliquait. Le domaine complet montre une transition nette
    entre 12 et 20. L'argument était juste, la portée était fausse — et c'est la portée qui
    décide de ce qu'on peut écrire. Prolonger le cadran, toujours (piège 46), et se méfier
    d'autant plus d'une explication SÉDUISANTE d'un plateau : elle rend l'extrapolation
    confortable.
63. **Une recette tirée du texte peut décrire une voie que CE deck ne peut pas emprunter.**
    « Lunalight Leo Dancer » n'a qu'une ligne de matériaux, et elle nomme Panther Dancer — qui
    n'est pas dans le deck de l'étalon A. Compter cette étape aurait donné un `h` fondé sur un
    chemin impossible, c'est-à-dire récompensé la poursuite d'une route morte. Un matériau
    nommé doit être confronté à la DECKLIST avant d'entrer dans une recette ; à défaut, le
    produit retombe au plancher. (Trouvé par une question sur une trace, pas par un test.)
64. **Une trace hors outil qui ne reproduit pas le code ne trace rien.** Mon script de
    vérification recursait dans le texte de n'importe quelle carte ; le code ne pose de recette
    que pour un ensemble de candidats. Il donnait 4 là où le solveur donne 2. Vérifier une
    implémentation avec une ré-implémentation, c'est vérifier la ré-implémentation.
65. **Le résultat spectaculaire est celui qu'il faut désamorcer en premier.** Un bras
    `--recipes 1` a converti l'étalon B — 8 solutions là où le dépôt n'en avait jamais eu. Il
    aurait suffi de l'annoncer. Trois vérifications le retirent au mécanisme : les solutions
    viennent d'une phase que le drapeau ne modifie pas (`RunNrpa`, alors que la distance de
    recettes n'entre que dans `RunLevin`) ; le bras à poids DOUBLE n'a rien trouvé ; et le
    piège 39 suffit à expliquer l'écart. Avant d'attribuer un gain, vérifier que le mécanisme
    a seulement PU le produire — c'est-à-dire qu'il s'exécute sur le chemin concerné.
66. **Un coût INFÉRIEUR peut vouloir dire « en fait moins », pas « fait mieux ».** Dix lignes
    atteignant le board de l'étalon B à 16/47/226 contre 19/56/276 pour la référence : j'y ai
    lu une amélioration. Le montage de l'étalon B ne porte AUCUNE contrainte — ni `--resolve`
    (les handrips, qui sont la raison d'être de la ligne), ni `--guard`. Ces lignes sont moins
    chères parce qu'elles sautent le travail. Avant de comparer deux coûts, vérifier que les
    deux font la MÊME CHOSE ; un coût ne se lit qu'à contraintes égales.
67. **Un étalon peut mesurer autre chose que ce que son nom dit.** L'étalon B, tel qu'il est
    scripté depuis la session 8, mesure une FORME DE BOARD (les codes présents), pas le combo :
    aucun de ses montages n'exige les rips ni ne tient la garde, alors que l'étalon A porte
    `--resolve`, `--summon-min` et `--hint`. Comme métrique d'A/B c'est valide — la même dans
    tous les bras — mais « 8/8 » n'y veut pas dire « la ligne marche ».
68. **Un compteur imprimé au MAUVAIS ENDROIT est aussi trompeur qu'un compteur absent.**
    `PrintCuts` affichait fidèlement `contrainte 0, garde 0` — parce qu'il n'était branché que
    sur les passes LDS, où ces compteurs valent zéro par construction. Les contraintes coupent
    dans les TIRAGES. J'ai failli conclure « les contraintes ne coupent rien » d'un instrument
    posé là où elles ne travaillent pas. Prolongement du piège 52 : imprimer ne suffit pas,
    il faut imprimer LÀ OÙ LE MÉCANISME AGIT (piège 53 appliqué aux compteurs).
69. **Un `printf` mal aligné produit des mesures, pas une erreur.** Une colonne `%s` manquante
    a fait lire `b=209066130608` et `(null)` là où il fallait `b=0  EPUISE` — un run entier
    d'A/B illisible sans que rien n'échoue. Le compilateur le disait (`warning C4473`) ; les
    builds incrémentaux ne recompilaient pas le fichier, donc l'avertissement ne réapparaissait
    pas. Compiler en `/v:normal` après une salve d'édition, et lire les avertissements.

### 9.17 Session 10 : l'amorce mesurée là où elle agit — et le nœud d'exigence, qui n'était modélisé qu'à moitié

**La mission** : les mesures laissées prêtes par la session 9, puis le chantier 16 là où il
s'était arrêté. La première mesure a retiré la deuxième avant même d'être finie.

**(a) L'A/B de l'amorce était monté sur le mauvais étalon, et la mesure le dit.**

Le montage `tools/s9_recettes_amorce.ps1` tourne sur l'étalon B. Or l'amorce ne retient que
les matériaux nommés entre guillemets, et le board de l'étalon B est fait de Synchros et de
Liens — « 1 Tuner + 1+ non-Tuner monsters », « 2+ monsters » : aucun nom entre guillemets.
L'en-tête du run le dit au premier coup d'œil, une fois qu'on le lit :

```
  amorce par le texte : 1 recette(s) posee(s), 1 produit(s) connus
```

**Une** recette, dans un graphe qui observe des dizaines de milliers d'invocations. Quatre bras
sur un seul binaire, budget et graine identiques :

| bras | r10 | r20 | r30 | r45 | cumul | `hR` (r10/r20/r30/r45) |
|---|---|---|---|---|---|---|
| SN `--no-seed-recipes` | 6 | 5 | 4 | 5 | 20 | 4,1 / 5,0 / 5,1 / 5,0 |
| SA amorce nommée | 6 | 5 | 4 | 5 | 20 | 4,1 / 5,0 / 5,1 / 5,0 |
| SP1 `--recipes 1` | 6 | 5 | 5 | 5 | 21 | 4,1 / 4,9 / 5,1 / 4,8 |
| SP2 `--recipes 2` | 6 | 5 | 5 | 5 | 21 | 4,2 / 4,7 / 5,0 / 4,6 |

SN et SA sont identiques **au dixième près sur chaque racine**, et les `best` le sont aussi.
Le +1 de SP1/SP2 est celui que le §9.16 (k) avait déjà désamorcé : `hR` valant le `h` plat,
`--recipes w` n'est qu'un réglage de `--levin-h` déguisé, et une carte d'écart tombe dans la
bande de bruit que tout le cadran α a montrée (17 à 21 pour un témoin à 20).
Ce n'est pas « l'amorce ne marche pas » : c'est « l'amorce n'a pas tourné », et l'A/B était
incapable de le distinguer avant qu'on lise son en-tête. Piège 53 appliqué à un étalon plutôt
qu'à un montage — mesurer un mécanisme LÀ OÙ IL AGIT vaut aussi pour le choix du cas de test.

Le bras SN a par ailleurs reproduit exactement le bras M de la session 9 (`hR` 1,0 / 4,1 / 5,0 /
5,1 / 5,0, cumul 20) : le témoin d'amorce était nécessaire précisément parce que le M de la
session 9 avait été mesuré sur un binaire ANTÉRIEUR à l'amorce, et comparer SA à ce M-là aurait
fait varier deux choses à la fois.

**(b) Le nœud d'exigence n'était modélisé qu'à moitié — et l'autre moitié est celle qui porte
le gradient.**

La règle 1 dit « le nœud est une EXIGENCE, pas une carte ». L'implémentation de la session 9 ne
savait exprimer qu'un seul genre d'exigence : *cette carte-là*. Le relevé des dix cartes de
l'extra deck de l'étalon A dit ce que cela laisse dehors :

| carte | ligne de matériaux | ce que la session 9 en tirait |
|---|---|---|
| Liger Dancer | `"Lunalight Leo Dancer" + 3 "Lunalight" monsters` | Leo seul |
| Leo Dancer | `"Lunalight Panther Dancer" + 2 "Lunalight" monsters` | route morte (Panther absent) |
| Sabre Dancer | `3 "Lunalight" monsters` | **rien** |
| Perfume Dancer | `2 "Lunalight" monsters` | **rien** |
| Bagooska *(cible)* | `2 Level 4 monsters` | **rien** |
| Dugares | `2 Level 4 monsters` | **rien** |
| Tiger King | `2 Level 4 Beast-Warrior monsters` | **rien** |
| Cross-Sheep | `2 monsters with different names` | rien |
| A Bao A Qu | `2+ monsters, including a Fiend monster` | rien |
| Underworld Goddess | `4+ Effect Monsters` | rien |

**Huit cartes sur dix ne nomment aucune carte.** L'amorce posait donc UNE recette sur l'étalon A
et une sur l'étalon B — un mécanisme vivant et sans effet (piège 42), ce que la mesure (a)
montre au dixième près.

Et l'argument de la session 9 pour les écarter — « elles sont presque toujours faciles à
satisfaire, donc du bruit sans gradient » — est faux dans l'autre sens. Une exigence CARDINALE
est précisément ce qui décroît continûment : « 3 monstres Lunalight » perd une unité à chaque
Lunalight posé, c'est-à-dire **avant qu'aucune carte cible ne touche le terrain**. C'est le trou
du `h` plat, décrit à l'identique depuis le §9.14, et il se comble là.

Trois genres d'exigence, donc (`ReqKind`) : carte nommée, archétype (`setcode`), niveau. Les
deux derniers portent un COMPTE et se résolvent par dénombrement, sans récursion — aucun produit
n'est nommé, il n'y a rien à fabriquer, seulement un compte à atteindre. Chaque exemplaire
manquant coûte une unité, ce qui sous-estime (poser un monstre coûte au moins une action) :
direction sûre au regard de la règle 2.

Restent ignorés, et volontairement : type, attribut, race (`Beast-Warrior`, `Effect Monsters`,
`including a Fiend monster`) et les contraintes de distinction (`2 monsters with different
names`). Les omettre sous-estime.

**L'archétype se résout sans table de noms.** Le texte dit « Lunalight », le core ne connaît que
des setcodes numériques, et la correspondance vit dans `strings.conf`, hors de notre portée. On
la retrouve **par le deck lui-même** : les cartes dont le nom contient le fragment doivent porter
un setcode commun. Sur l'étalon A l'intersection est exactement `{0xdf}`, et le setcode retenu
est imprimé avec son nombre de fournisseurs — un chiffre dérivé se vérifie, il ne se croit pas.

**(c) LE DÉFAUT QUI RENDAIT L'AMORCE INERTE, et il n'était pas dans le parseur.**

La zone d'une exigence amorcée est un JOKER : le texte nomme un matériau sans dire d'où il
vient. Le joker de la session 9 signifiait « présent **n'importe où** », et la table de présence
relève six zones — dont le DECK et l'EXTRA DECK.

Conséquence, sur le seul cas où l'amorce avait quelque chose à dire : Liger Dancer exige
nommément « Lunalight Leo Dancer », et **Leo dort dans l'extra deck en deux exemplaires**. Il
était donc « présent » dès le premier nœud, la distance à Liger valait `1 + 0 = 1` — le
plancher, c'est-à-dire exactement le `h` plat. Le tableau du §9.16 qui annonçait `Liger = 2`
n'a jamais pu sortir du solveur : c'est une trace hors outil (piège 64, deuxième fois sur ce
même mécanisme).

Le joker signifie désormais « n'importe quelle zone où l'on peut PRENDRE un matériau » —
terrain, main, cimetière, bannie. Une carte qui dort dans le deck ou l'extra n'est pas un
matériau disponible : **il faut d'abord l'invoquer, et c'est exactement l'étape que le graphe
doit compter.**

Et pour que cela ne se reproduise pas en silence, l'en-tête imprime désormais la distance
amorcée de chaque carte cible **depuis un terrain vide**, calculée par le `DistanceAll` que le
finisseur appelle — pas par une trace.

**(d) MESURE — l'amorce donne enfin un gradient, et le chiffre sort du solveur.**

Étalon A, en-tête d'un run de cinq secondes (la table s'imprime avant toute recherche) :

```
   archetype « Lunalight » -> setcode 0xdf, 17 fournisseur(s) au deck
   (1 recette(s) ecartee(s) : elles nomment un materiau absent de ce deck)
   exigences posees : 1 nommee(s), 3 archetype(s), 3 niveau(x)
amorce par le texte : 6 recette(s) posee(s), 6 produit(s) connus

   carte cible                                    h plat   distance amorcee
   Lunalight Liger Dancer                         1        5   <-- gradient
   Number 41: Bagooska the Terribly Tired Tapir   1        3   <-- gradient
```

**Six recettes au lieu d'une**, et `h` cesse d'être plat : 5 et 3 là où il valait 1 ; sur la
cible entière, **18 contre 4**. Le détail se vérifie à la main, et c'est le but de la table :
Liger vaut `1 (l'invocation) + 1 (Leo, dont la recette texte est morte dans ce deck, donc
plancher) + 3 (les trois monstres Lunalight)`. Bagooska vaut `1 + 2 (deux monstres de niveau 4)`.

Le setcode est **déduit du deck** — les cartes dont le nom contient « Lunalight » portent toutes
`0xdf`, et l'intersection le donne sans aucune table de noms. La route morte de Leo Dancer reste
écartée (piège 63), ce qui est le comportement voulu et non un manque.

**Santé** : diff avant/après **identique hors durées**. Le run de santé n'active pas `--recipes`,
donc le nœud d'exigence ne doit s'y voir nulle part — et il ne s'y voit pas.

**Ce qui n'est PAS mesuré, et il faut le dire.** L'A/B armé (`AN` sans amorce / `AA` nommée seule
/ `AQ` complète / `AP1` pesée dans `h`) a été **interrompu en cours** : seul le témoin `AN` a
rendu sa table, `hR` = 0,0 / 3,0 / 3,5 / 3,9 / 4,0, c'est-à-dire exactement le `h` plat, ce qui
confirme au moins que le témoin est bien un témoin. *Que ce gradient fasse gagner reste une
question ouverte* — et c'est la bonne question suivante, à poser avec `--levin-h`, jamais avec
`--reroot-h` (réfuté, §9.16 (d)).

**(e) MESURE — C1 vérifié sur l'étalon A, et le §9.15 (e) tombe.**

Étalon A en mode but seul, 1 800 s, graine 888. La passe à écarts bornés :

| écarts | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| états | 1 | 21 | 54 | 113 | 358 | 983 | 3 956 | 10 156 | 30 013 | 50 852 | 60 239 |

Elle **croît**. Avant correction elle valait **26, constante à tous les niveaux**, et le §9.15 (e)
avait pris ce plateau pour une propriété structurelle du mode but seul : sans plan, disait-il, la
passe est « structurellement VIDE ». Elle ne l'était pas — un seul jeton de partition la
paralysait.

**Et le mode but seul atteint 2/4** (`best_approach_2of4.yrp`, 197 décisions), par le finisseur :
c'est le résultat de la référence ARMÉE du répertoire (s7_luna_v6, 2/4). La conclusion « le
répertoire valait la partie difficile » ne survit pas — les deux bras comparés par le §9.15 (e)
différaient du répertoire **et** d'un mécanisme cassé.

Deux réserves, notées plutôt que tues. La colonne `partition` reste à **0** dans toute cette
passe : elle ne montre ni partage ni suppression, le mécanisme ne mord pas ici, et c'est la table
des états qui porte la vérification. Et le run signale jusqu'à **247 prompts réduits à la réponse
par défaut** — « ces branches n'ont jamais existé » : c'est le piège 59, cet espace-là n'a jamais
été exploré, et aucune preuve d'absence ne peut en faire abstraction.

*Fait annexe, versé au §9.16 (l).* Sur l'étalon A aussi, c'est la **fin de tour** qui coupe :
86 % des tirages gloutons, 56 % des tirages NRPA, tandis que `contrainte` et `garde` restent à
zéro. Même diagnostic que sur l'étalon B — le sampler joue des tours complets et n'assemble pas
le board dedans.

**(f) Le chantier 17 devient chiffrable sans être écrit.**

`ForecastOptionGain` mine les sous-séquences fréquentes des coups joués du corpus et recalcule la
borne de Levin comme si elles étaient atomiques — c'est le critère de sélection d'Alikhasi & Lelis
(arXiv:2410.11262), appliqué à nos données. Deux conservatismes assumés pour que le chiffre soit
une borne BASSE : le catalogue entier est supposé proposé à chaque décision, et aucune macro
n'est créditée de raccourcir une ligne que le corpus a jouée. La mesure elle-même
(`tools/s10_options.ps1`, 30 s) **reste à lire** : la session a été arrêtée avant.

### 9.18 Session 11 : le profil est un instrument — deux régimes décomposés, et le facteur 70 expliqué

Session PERFORMANCE : réduire le coût d'UNE décision simulée, sans toucher l'algorithme. Le
constat d'entrée était que personne ne savait où partait le temps — le « ~250 µs par décision,
dominée par `duel.Process()` et `arena.Restore()` » du §9.16 était une estimation relevée dans
des logs, jamais un chiffre imprimé par un run (piège 52). La session installe l'instrument,
décompose LES DEUX régimes, tranche l'écart inexpliqué de ~70× entre eux, et livre trois
optimisations tenues par le contrôle de correction.

**(a) L'instrument : `--profile`.** Sondes `__rdtsc` à temps EXCLUSIF (une sonde imbriquée se
soustrait de celle qui l'englobe), compteurs `thread_local` versés dans des atomiques globaux au
décès du thread — les workers sont créés et joints par phase, le versement est garanti ; le
thread principal flushe explicitement (piège 58, celui de `host_fallbacks`). Aucun atomique sur
le chemin par-appel. La sonde `kSearch` enveloppe le corps des `Run*` : son temps propre EST la
ligne « reste », par construction — pas de soustraction à l'impression, pas de profil qui ment
par omission. Les allocations d'arène sont comptées mais jamais chronométrées (~10⁶ appels/s par
worker : un rdtsc par appel fabriquerait le ralentissement qu'il prétend observer). Impression
par phase (`prof::PrintPhase` aux joins des pools) plus un cumul de fin de run. Éteint, une sonde
coûte un load+branch. Le code vit dans `arena.h`/`arena.cpp` — le glob de premake est évalué à
la génération, un fichier neuf aurait exigé de régénérer la solution.

**(b) Le régime TIRAGES, décomposé pour la première fois** (étalon B contraint, 60 s, graine
888, 16 workers ; 6,05 M décisions dans la phase) :

| sonde | part | appels/déc | µs/appel |
|---|---|---|---|
| **Process (core)** | **82,0 %** | 2,79 | 27,2 |
| arène Restore | 9,0 % | 0,12 (1/tirage) | 70,7 (63 pages) |
| QueryCodes (nouveauté) | 2,9 % | 3,74 | 0,73 |
| recherche (reste) | 2,0 % | — | — |
| Query (board key) | 1,5 % | 2,15 | 0,66 |
| Push+Pop, énumération, atomes, board key, Count | ~2,6 % | — | — |

92,5 µs de temps sonde par décision (16 workers cumulés, soit ~5,8 µs de temps mur — cohérent
avec les 51 µs/état×16 déduits de la session 10). Et : **369 allocations d'arène par décision**
(2,24 milliards sur le run), toutes dans `Process` — c'est le trafic Lua/core lui-même.

Deux conclusions. D'abord, le softmax, la politique, les tables — tout ce que C27 voulait
optimiser — pèsent 2 % : morts avant d'être tentés, le §9.9 (« le coût est dans ocgcore, pas
dans l'hôte ») est re-confirmé par l'instrument. Ensuite, l'hôte entier (requêtes comprises)
pèse ~18 % : même réduit à zéro, le gain plafonnerait à ×1,2.

**(c) Le facteur ~70 entre tirages et finisseur : tranché, et ce n'est PAS le branchement.**
L'hypothèse de la session 10 (« une expansion développe tous les fils, à b≈20 les ~4 ms
retombent sur ~200 µs par fils ») est FAUSSE. Le profil du finisseur (phase 2, RunLevin, 3 536
expansions) :

| sonde | part | appels/expansion | µs/appel |
|---|---|---|---|
| **Process (core)** | **86,2 %** | **79,9** | 131,1 |
| arène Restore | 7,7 % | 2,74 | 341,6 (441 pages) |
| arène Push | 4,1 % | 1,01 | 488,5 (707 pages) |
| digest (self) | 0,03 % | 1,31 | 2,6 |

12,2 ms par expansion dont 10,5 ms de `Process` — **80 appels par expansion**. Une expansion ne
développe pas ses fils (ils sont seulement enfilés) : elle REJOUE le chemin depuis la racine
quand le nœud extrait de la file n'est pas un voisin de la pile de plongée, et chaque coup forcé
du rejeu est un `Process`. Le coût du finisseur est le REJEU, pas le développement. Les
hypothèses alternatives sont éliminées par le même tableau : digest recalculé par fils — non
(0,03 %) ; requêtes refaites par fils — non (0,4 %) ; restauration par fils — non (2,74/exp.).

S'y ajoute un fait nouveau, mesurable seulement avec la décomposition par phase : **le coût d'un
pas de core CROÎT avec la profondeur de l'état** — 27,2 µs/appel depuis la position de départ,
89,8 µs aux racines de recul profond (phase A2), 131,1 µs dans le finisseur. Un board chargé
coûte 3-5× par pas. Les 0,18 ms du jalon 0 étaient mesurés le long de la référence : ils étaient
justes, mais pas représentatifs des états que la recherche visite réellement.

**(d) Les optimisations livrées, et le contrôle qui les tient.** Santé stricte avant/après :
20 lignes de diff, toutes des durées — 273 digests deux à deux distincts, 210/273, 209
candidates, 16 replays, 19/56/272, « résiste à 12 déviations » inchangés.

- **LTO** (`/GL`+`/LTCG`, les 4 projets — `flags { "LinkTimeOptimization" }` dans premake5.lua,
  solution régénérée). Le core, Lua et le solveur étaient trois libs statiques : l'inlining
  s'arrêtait sur `OCG_DuelProcess` et sur l'allocateur d'arène branché dans Lua, exactement le
  chemin chaud. C'est le seul levier qui touche les 82-86 %.
- **C20** : `kHiddenFlags` — les 8 requêtes de digest sur main/cimetière/banni/extra ne
  sérialisent plus overlay/counters/link, qui y sont vides par règle du jeu. Valeurs de digest
  INCHANGÉES par construction (les champs étaient vides dans `EntryOf`).
- **C22** : `MixBytes` — le digest hache la charge du prompt et l'état processeur par mots de
  8 octets (longueur mélangée d'abord, queue complétée de zéros). Les valeurs de digest
  changent ; le contrôle est l'égalité structurelle de la santé, et elle passe.
- **RecipeDistance** : la surcharge vecteur de `Query` (qui alloue) remplacée par la surcharge à
  tampon — cinq allocations par nœud développé en moins sur le chemin `--recipes`.

**Le gain, mesuré à temps égal** (étalon B contraint, 60 s, graine 888, phase tirages ; le
témoin est le binaire pré-optimisation conservé — `bin\Release\combosolver_preopt.exe`) :

| bras | tirages NRPA | états |
|---|---|---|
| ancien binaire, sans profil | 113 654 | 5 651 956 |
| ancien binaire, avec `--profile` | 123 300 | 5 753 223 |
| **nouveau binaire (LTO+C20+C22), sans profil** | **196 067** | **7 450 129** |

Soit **+31,8 % d'états à temps égal** — un run unique, avec deux réserves écrites : la
dispersion à graine fixée (piège 39 — les crêtes divergent, 3/8 contre 7/8) et le fait que des
trajectoires divergentes visitent des états de profondeurs différentes, dont le coût unitaire
varie de 3-5× (cf. (c)) — une partie de l'écart peut être de la chance de trajectoire. Les trois
répétitions et les médianes restent dues. **Le coût de l'instrument**, lui, est sous le plancher
de bruit : le bras profilé de l'ancien binaire rend +1,8 % d'états par rapport au bras nu — du
MAUVAIS côté pour un surcoût — donc < 2 % et indiscernable du bruit sur un run.

Le chiffre le plus robuste à la divergence de trajectoire est le coût PAR APPEL du core, relevé
par la même sonde sur les deux binaires : `Process` passe de 27,2 à **23,5 µs/appel (−13,8 %)**
dans la phase tirages — c'est le rendement propre du LTO, par-appel et non par-run.


**(e) Ce que la session dit du problème.** Le prompt de session l'exigeait : si le profil montre
que le temps part dans `OCG_DuelProcess`, le dire clairement plutôt que gratter des pourcentages.
C'est le cas, dans les deux régimes (82 % et 86 %). Le levier n'est plus dans notre code : il est
dans le NOMBRE d'appels — les OPTIONS (chantier 17) pour les tirages, et pour le finisseur la
RÉDUCTION DES REJEUX (80 Process par expansion : garder plus de niveaux d'arène sur la pile de
plongée, ou enraciner des checkpoints aux nœuds chauds de la file — un chantier algorithmique
nouveau, désigné par l'instrument et absent de l'audit). La périphérie est déjà propre.

**Réserves.** Les mesures de gain de cette session sont des runs UNIQUES à graine fixée
(piège 39 : la dispersion domine) — l'amorce d'un A/B, pas sa conclusion ; les trois répétitions
et les médianes restent dues. Le coût de l'instrument n'a été chiffré que sur un run. Et PGO
n'a pas été tenté (LTO d'abord, mesuré seul — PGO est le candidat suivant, cas d'école sur un
profil aussi stable).

**(f) Poursuite dans la même session : le rejeu depuis l'ancêtre partagé.** Le levier désigné
en (c) a été implémenté séance tenante : la pile de plongée de `RunLevin` reconnaît désormais
l'ancêtre le plus profond du nœud extrait (elle ne reconnaissait que le parent direct), et ne
rejoue que le suffixe de chaîne. Sémantiquement neutre — le contrôle déterministe passe à
l'identique (`recul 0` : 42 exp., b=0, ÉPUISÉ) — pour Process/expansion 62,2 → 57,8 (−7 %) et
+4,6 % d'expansions à temps égal en agrégat sur les cinq racines de l'étalon 0, dont +31,8 %
sur `recul 45` (runs uniques). Le gain est réel mais plus modeste qu'espéré : sur ce montage,
les sauts de file partagent des préfixes courts — le rejeu reste le coût dominant, et le
chantier « checkpoints hors pile LIFO » reste ouvert.

**(g) Revue adversariale de clôture.** Un agent relecteur a challengé les deux commits de la
session sur six points (temps exclusif du profileur, flush thread_local, injectivité de
`MixBytes`, hypothèse de `kHiddenFlags`, rejeu par ancêtre — invariant pile d'arène/pile de
plongée reconstruit à la main —, terminaison du rejeu de suffixe) : **aucun défaut trouvé**.
Une observation PRÉEXISTANTE versée au dossier : dans `advance()` du finisseur, `Adv::Goal` est
traité comme `Adv::Dead` — un nœud-but n'a jamais d'enfants, même sous `--optimize`/anytime, là
où `DescendGuided`/`DescendRepair` documentent explicitement continuer APRÈS le but (piège 35).
À trancher en session suivante : soit c'est voulu (le finisseur s'arrête au but par
construction), soit la récupération d'après-but est structurellement absente du finisseur et
personne ne l'a mesuré.

### 9.19 Session 12 : les médianes retirent le chiffre de la session 11, et le compteur de tirages est déclassé comme instrument d'A/B

Session PERFORMANCE (suite). Programme du prompt : les trois répétitions et les médianes
d'abord, PGO ensuite, puis le chantier nouveau désigné par l'instrument — les REJEUX du
finisseur — et la lecture de la prévision des options.

**(a) Les médianes : le « +31,8 % d'états à temps égal » de la session 11 NE SURVIT PAS.**
Étalon B contraint, 60 s, graine 888, trois répétitions par bras, bras INTERCALÉS
(`tools/s12_ab_medianes.ps1`) :

| bras | tirages ×3 | médiane | états, médiane |
|---|---|---|---|
| ancien binaire (`combosolver_preopt.exe`) | 156 261 / 175 518 / 194 431 | **175 518** | 7 076 319 |
| nouveau (LTO+C20+C22+ancêtre) | 122 252 / 158 431 / 162 320 | **158 431** | 6 840 313 |
| nouveau + `--profile` | 147 363 / 205 767 / 213 390 | **205 767** | 7 744 983 |

La médiane du nouveau binaire est SOUS celle de l'ancien (−9,7 % tirages), et le bras profilé —
qui porte un SURCOÛT — est au-dessus des deux (+30 % sur le bras nu du même binaire). Aucun de
ces trois écarts n'est un fait : la dispersion à graine fixée est de ±10-18 % par bras, et elle
engloutit tout. Le run unique de la session 11 (113 654 contre 196 067) avait tiré le bas de la
bande de l'ancien et le haut de celle du nouveau — exactement le piège 39, sur le chiffre que le
§9.18 (d) avait lui-même flanqué de deux réserves écrites.

**Conséquence méthodologique, plus importante que le chiffre retiré : le compteur de
tirages/états à graine fixée est DÉCLASSÉ comme instrument d'A/B de débit** pour tout effet
< ~20 %. Ce qui le remplace : le coût PAR APPEL relevé par la même sonde `--profile` des deux
côtés — piège 39 étendu : la dispersion des COMPTEURS domine, celle du par-appel non (médiane
`Process` phase tirages sur les trois runs profilés du jour : 19,0 / 19,3 / 22,3 µs). Le
−13,8 %/appel du LTO (27,2 → 23,5 µs, §9.18 (d)) reste le seul énoncé de gain qui tienne.
Le coût de l'instrument reste indiscernable du bruit (le bras profilé rend PLUS d'états que le
bras nu).

**(b) La prévision des options (chantier 17) : enfin lue, et elle ne justifie le chantier que
dans sa forme GROS CATALOGUE.** Le script s10 n'avait jamais pu l'imprimer : `--adapt` n'est
branché que sur les chemins `--start`/`--fire` (`BuildAdaptRuns`) ; en mode réparation le flag
était accepté et IGNORÉ — la famille exacte du « mécanisme silencieusement absent du chemin »
(réflexe session 9). Corrigé : avertissement imprimé en mode réparation, script réparé
(`--start` ajouté). La table (corpus `solutions/`, 17 lignes, politique uniforme) :

| catalogue | support | décisions | avec macros | log10 d/π | absorbé |
|---|---|---|---|---|---|
| 16 | 4 | 170 | 115 | 87,6 → 151,0 | 38 % |
| 64 | 3 | 170 | 54 | 87,6 → 100,3 | 78 % |
| 256 | 2 | 170 | **32** | 87,6 → **79,1** | **93 %** |

Les petits catalogues sont CONTRE-PRODUCTIFS : proposés à chaque décision, ils gonflent le
branchement plus que la profondeur ne baisse. Le gros catalogue (256 macros, support 2,
longueur ≤ 8) absorbe 93 % du corpus, ramène la ligne de 170 à 32 décisions et gagne 8,5 ordres
de grandeur sur la borne de Levin — le seuil d'ouverture du chantier (3 ordres) est largement
franchi, mais uniquement sous cette forme. Réserve : borne calculée sous politique UNIFORME
(l'adaptée 4 passes est déjà à 28,7 en monolithe) ; le transfert du gain à la politique adaptée
n'est pas prédit par cette table.

**(c) Le chantier REJEUX : l'instrument d'abord, et il renverse le diagnostic de la
session 11.** Nouveau compteur `rj = arêtes rejouées / arêtes de chaîne, par expansion`
(imprimé sur chaque ligne de finisseur ; à chaîne = rejouées, la pile n'absorbe rien ; à 0,
tout). Sur l'étalon 0, bras témoin : **rj = 12/12 partout** — la pile de plongée n'absorbait
RIEN. Le « les sauts de file partagent des préfixes courts » du §9.18 (f) était un artefact de
l'instrument de l'époque : la pile ne retenait que les nœuds DÉVELOPPÉS de la branche courante
et perdait les intermédiaires à chaque saut — l'ancêtre partagé trouvé était quasi toujours la
racine. Les sauts ONT des préfixes profonds communs ; c'était la pile qui les perdait.

**`--dive-full`** empile un niveau d'arène à CHAQUE nœud de chaîne rejoué (pas seulement au
nœud développé) : le trafic de pages d'un segment se RÉPARTIT entre les Push — seules les pages
chaudes communes sont journalisées plusieurs fois — et la pile détient la branche entière.
Étalon 0, quatre bras sous `--profile` (`tools/s12_rejeux.ps1`), contrôle identique partout
(recul 0 : 42 exp., b=0, ÉPUISÉ ; mêmes `best` par racine — l'ordre d'extraction est inchangé,
seule la vitesse change) :

| bras | rj | Process/exp | µs/appel | exp. à temps égal (4 reculs) |
|---|---|---|---|---|
| témoin | 12/12 | 54,5 | 70,4 | 14 910 |
| `--lifo-ties` | 12/12 | 54,5 | 90,4 | 11 867 |
| **`--dive-full`** | **1,5/14** | **12,8** | 95,4 | **28 686 (+92 %)** |
| les deux | 1,4/14 | 12,3 | 103,5 | 29 817 |

**Le plus gros gain du chantier performance à ce jour, et il est STRUCTUREL** (pas un compteur
de tirages : l'ordre d'extraction étant inchangé, l'accélération est du débit pur). PAR DÉFAUT
depuis cette session ; `--no-dive-full` pour l'A/B. Confirmation sur run de contrôle défaut :
identique au bras au drapeau près.

`--lifo-ties` (départage LIFO des ex aequo de la file) est RÉFUTÉ seul : les ex aequo exacts
sont rares (les log-probabilités diffèrent par nœud), rj ne bouge pas, et l'ordre modifié
visite des états plus chers (90,4 contre 70,4 µs/appel) pour −20 % d'expansions ; neutre
combiné à `dive_full`. Éteint par défaut, gardé pour l'A/B.

**Ce que le profil dit du coût résiduel** : dans le bras `dive`, `Process` ne pèse plus que
53 % — l'arène en prend 44,5 % (Restore 22,6 %, Push 17,4 %, Pop 4,5 %, à ~119/167/43 µs
l'appel). Le prochain levier du finisseur est le COÛT FIXE par niveau d'arène (Push/Pop par
nœud rejoué), pas le core. Piste : un pas de pile (stride) ou des journaux plus légers.

**(d) PGO : GARDÉ — −21,4 % par appel, distributions disjointes.** Mécanique sans toucher
premake : le linker MSVC lit `_LINK_` — `/GENPROFILE`, entraînement (santé + étalon B 30 s +
étalon 0 30 s : les trois régimes), `/USEPROFILE` (`tools/s12_pgo.ps1`). Santé stricte du
binaire PGO : 24 lignes de diff, toutes des durées (et 0,199 → 0,177 ms/décision sur le
déroulement de référence). Mesure au par-appel (`tools/s12_pgo_mesure.ps1`, étalon B contraint
60 s ×3 par bras, intercalés, sonde identique des deux côtés) :

| bras | `Process` µs/appel (tirages) ×3 | médiane |
|---|---|---|
| LTO (témoin `combosolver_lto_s12.exe`) | 25,23 / 23,63 / 20,90 | 23,63 |
| PGO | 18,58 / 18,91 / 17,71 | **18,58 (−21,4 %)** |

Le pire run PGO (18,91) bat le meilleur run LTO (20,90) : l'effet est RÉSOLU par l'instrument,
pas inféré d'une médiane. Les compteurs déclassés pointent d'ailleurs dans le même sens
(médiane tirages +32 %, 5 des 6 paires disjointes) — cohérence, pas preuve. Cumul depuis le
témoin pré-optimisation : `Process` 27,2 → 23,6 (LTO, jour du même montage) → 18,6 µs/appel.

**ATTENTION REPRODUCTIBILITÉ** : un `MSBuild` ordinaire RELIE SANS `/USEPROFILE` et perd le
PGO en silence (le binaire redevient LTO nu). Après tout changement de moteur : recompiler,
santé, puis RE-DÉROULER `tools/s12_pgo.ps1` avant toute mesure. Le témoin LTO du jour est
conservé (`combosolver_lto_s12.exe`), comme `combosolver_preopt.exe` avant lui.

**(e) Observation ouverte — versée au dossier, non tranchée.** Le binaire INSTRUMENTÉ
(`/GENPROFILE`) a rendu sa santé d'entraînement avec **1898 réponses INDÉCODABLES** par le
filtre `--no-chain` (tri-état C10, code de sortie 1), là où les binaires normal et PGO final
en rendent ZÉRO sur la même commande. Deux hypothèses non départagées : un artefact de codegen
de l'instrumentation, ou un chemin `v >= n` du décodeur de chaîne (réponse enregistrée
confrontée à un prompt désynchronisé en réparation) que seul le rythme différent du binaire
instrumenté fait apparaître — auquel cas le classement `Undecodable` de ce cas est trop
sévère : un désaccord SÉMANTIQUE attendu en réparation y est compté comme dérive de FORMAT.
Sans effet sur les binaires livrés (santés propres) ; à trancher si un run normal remonte un
jour ce compteur. Complément de fin de session : le SECOND pipeline PGO (déroulé sur le code
final, options comprises) a rendu sa santé instrumentée PROPRE (EXIT=0) — l'anomalie ne s'est
pas reproduite ; elle reste versée au dossier comme observation isolée, non reproduite.

**(f) Le chantier 17 IMPLÉMENTÉ — et son premier A/B perd, avec le diagnostic au chiffre
près.** La session a poursuivi au-delà du programme : le catalogue d'options est passé de
prévision à mécanisme. `MineOptionCatalog` (le même minage que la prévision : support ≥ 2,
longueur ≤ 8, classement gain brut) rend un catalogue exécutable — séquences, une clé de
politique par macro (id propre dans l'espace des `plan_key`), index par première clé. Dans
`PolicyRollout`, une macro applicable (première clé légale au prompt) est UNE unité
d'échantillonnage : choisie, ses clés suivantes se jouent sans échantillonner ni produire de
`PolicyStep` (les prompts forcés passent au travers — le corpus n'en enregistre pas) ; une clé
non proposée fait AVORTER la macro, le tirage reprend son cours — rien n'est jamais retiré de
l'espace. `--options <n>` (0 = éteint, défaut), exige un corpus `--adapt`. Triptyque de vie
imprimé : prises / décisions absorbées / avortées (piège 52).

Premier A/B (étalon B contraint + `--adapt solutions`, 60 s ×3 par bras,
`tools/s12_options_ab.ps1`), forme naïve — toutes les macros applicables proposées, biais
répertoire hérité du premier coup :

| lecture | témoin ×3 | `--options 256` ×3 |
|---|---|---|
| prises / tirage | — | **7,7** (600k-969k prises) |
| absorbées / prise | — | **1,3-2,1** (cible ~6,6) |
| avortées / prises | — | 45-82 % |
| best | 7/8, 8/8, 7/8 | 5/8, 6/8, 7/8 |
| tirages ≥ 3 résolutions | 2, 0, 28 | **0, 0, 0** |

**PERDANT, et l'instrument dit pourquoi** : le catalogue INONDE le softmax — à chaque prompt,
des dizaines de macros applicables portant chacune le biais répertoire écrasent les choix
atomiques ; la masse part sur des macros qui cassent au deuxième pas (une sous-séquence minée
aux décisions 40-47 est proposée dès la décision 5, où sa première clé est légale mais pas la
suite). Deux gardes posées en réponse : UNE macro par première clé (la mieux classée), et
AUCUN biais hérité (le poids part de zéro, l'adaptation seule les élève).

**Deuxième A/B (mêmes montage et graine, `tools/s12_options_ab2.ps1`) : toujours PERDANT.**
Les gardes n'ont pas réduit la fréquence — 8-9 prises/tirage (chaque choix du prompt apporte
encore SA macro), absorption 1,0-1,6/prise, 78-84 % d'avortements. Sorties : best témoin
8/8, 8/8, 6/8 contre opt 7/8, 7/8, 7/8 ; un signal isolé sur ≥ 2 résolutions (opt 1059 et
607 contre témoin 145 et 54 sur deux runs — à la dispersion près, non concluant) ; ≥ 3
jamais atteint côté opt (le témoin l'atteint 12 fois sur un run). VERDICT : le mécanisme est
implémenté, vivant, instrumenté — et sa forme SANS CONTEXTE est réfutée sur ce montage. Le
diagnostic tient en une phrase : une sous-séquence minée aux décisions 40-47 est proposée dès
la décision 5, où sa première clé est légale mais pas sa suite. La prochaine forme est
CONDITIONNÉE — miner et proposer les macros avec leur contexte (`PolicyStep::ctx`, les cartes
cibles posées, existe déjà des deux côtés du releveur), et/ou n'accorder une macro qu'à
l'endroit de sa ligne d'origine (fenêtre de position). ÉTEINT par défaut ; `--options 256`
pour reprendre l'A/B. À noter aussi : les décisions absorbées paient toujours leur coût core
(`Process` par pas inchangé) — les options sont un levier de MASSE (probabilité d'une ligne
complète), pas de débit ; leur juge est « ≥ k résolutions » et « 8/8 AVEC rips », jamais le
compteur de tirages.

**(g) L'audit littérature exécuté — la sélection par perte de Levin retourne le verdict des
options.** Un audit de fidélité aux papiers (demandé en séance) a identifié trois rustines ;
les trois ont été traitées dans la même session.

*1. Options v3 : le critère du papier au lieu du nôtre — et le mécanisme GAGNE.* La v1/v2
classait les macros au gain brut `support × (longueur−1)` ; Alikhasi & Lelis (2410.11262)
sélectionnent en MINIMISANT LA PERTE DE LEVIN du catalogue, par ajout glouton avec
ré-évaluation — une macro paie sa présence au dénominateur PARTOUT où elle est proposable, et
la sélection s'arrête quand plus rien n'améliore. Implémenté (`MineOptionCatalog` v3 :
programmation dynamique de segmentation par ligne, vivier 1024, faisceau 64, approximations
documentées). Effet immédiat : **la taille du catalogue devient un résultat — 19 macros
retenues sur plafond 256** — et la perte modèle passe de 85,4 → 33,8 log10 (contre 87,6 →
79,1 pour la forme naïve). A/B (étalon B contraint + `--adapt`, 60 s ×3,
`tools/s12_suite_audit.ps1`) :

| lecture | témoin ×3 | sélection Levin ×3 | + fenêtre ±16 ×3 |
|---|---|---|---|
| best | 7/8, 7/8, 6/8 | **8/8, 8/8, 8/8** | 7/8, 6/8, 7/8 |
| ≥2 résolutions | 156, 19, 184 | **172, 81, 191** | 72, 14, 198 |
| ≥3 résolutions | 22, 3, 23 | **37, 31, 77** | 0, 0, 0 |
| racines de rip (départ) | 2/8 | **5-6/8**, crête 7/8 | — |

**GAGNANT sur toutes les paires** (6/6 sur ≥2 et ≥3, 3/3 sur le best) — un fait structurel
cohérent, pas une médiane. Le premier échec (f) n'était donc pas le levier qui était faux :
c'était notre écart au papier. Aucune ligne complète avec rips encore (60 s) ; le mécanisme
reste OPT-IN (`--options 256`, exige `--adapt`) — c'est la forme recommandée.

**QUALIFICATION — ce que le 8/8 prouve, et ce qu'il ne prouve pas** (question soulevée en
séance). Le run est bien en mode but seul (`--no-plan` : répertoire écarté, politique
uniforme, les réponses enregistrées ne sont pas candidates). MAIS la référence entre encore
par le corpus : sur les 17 lignes `--adapt`, 16 sont les replays du run de santé — des
variantes de RÉPARATION de la référence, à sa signature de coût près (19/56). Les macros sont
donc des fragments distillés de la ligne de référence via ses variantes, et le board cible
vient de son rejeu. L'A/B isole proprement « les macros aident-elles AU-DELÀ de l'adaptation
sur le même corpus ? » (les deux bras adaptent pareil) — oui, 6/6 — mais il mesure une
meilleure EXPLOITATION d'un corpus quasi-référence, pas une découverte autonome. Le test de
découverte est le BOOTSTRAP : étalon A en but seul (`--no-ref`, aucune ligne enregistrée),
corpus = les meilleures approches partielles des tirages eux-mêmes, macros minées dessus —
le cas d'usage exact qu'InnateCoder vise dans la revue. À monter avant de généraliser.

*La FENÊTRE DE POSITION, elle, est RÉFUTÉE* : elle réduit bien les avortements (3,8
absorbées/prise contre 1,3-2,5) mais tue les ≥3 résolutions (0 partout) — les tirages ne
s'alignent pas positionnellement avec le corpus, la garde bride les macros là où elles
servent. La précondition devra être SÉMANTIQUE (`ctx`, cartes posées), pas positionnelle.

*2. PHS* canonique (`--phs-canonical`, coût (d+h)/π du papier au lieu de notre facteur e^h) :
NEUTRE sur l'étalon 0* — contrôle et best identiques, −1,7 % d'expansions (bruit). Attendu :
h ≈ 1 aux racines de ce montage, les deux formes y coïncident presque. Le vrai juge est
l'étalon A (h_root = 4-6), où e^h et d+h divergent massivement — à trancher là-bas avant tout
défaut. Le flag existe, la garantie du papier est disponible au prix d'un drapeau.

*3. Snapshots Go-Explore aux cellules d'archive : RÉSOLU PAR L'INSTRUMENT, non rentable.* La
sonde `kPrefix` (rejeu de préfixe des racines) affiche **0,0 %** dans tous les profils de
l'étalon 0 depuis `dive_full` — l'hypothèse « restaurer les racines par memcpy comme
Go-Explore » n'a plus de charge utile à récupérer. Évalué, écarté, rien à implémenter.

**(h) Suite best-effort : le cadran des workers tranche une conjecture, le dépilage fusionné
prend les 21 % d'arène.**

*Le cadran du nombre de workers (jamais mesuré, `tools/s12_threads.ps1`, binaire PGO, µs/appel
par la même sonde) :*

| workers | `Process` µs/appel (méd. ×2) | appels totaux / 60 s |
|---|---|---|
| 8 | 11,3 | 19,4 M |
| 12 | 14,7 | 21,8 M |
| 16 (défaut) | 18,6 | **23,9 M** |
| 24 | 34,7 | 19,7 M |

Le coût PAR APPEL croît quasi linéairement avec la concurrence — saturation mémoire/L3, la
mécanique que le §6ter annonçait — mais le débit AGRÉGÉ culmine bien à 16 : la conjecture
« l'optimum est bien sous le nombre de threads » est RÉFUTÉE au sens agrégé. Deux enseignements
fermes : ne JAMAIS sursouscrire (24 : −18 %), et 8→16 ne rend que +23 % pour 2× les threads —
le mur est la bande passante, pas le CPU ; la marge future est la réduction du working set par
décision, pas plus de cœurs.

*Le dépilage fusionné (`Arena::PopToAndRestore`) :* revenir à l'ancêtre dépilait k niveaux en
k passes — chaque page chaude recopiée une fois par niveau où elle était sale, plus une fois au
Restore final. La fusion fait UNE passe : un seul `SyncDirty`, les journaux d'annulation
appliqués au miroir du haut vers le bas, puis l'arène restaurée sur l'UNION des pages sales, et
la table des spans restaurée une fois. Équivalence exacte vérifiée (étalon 0 : 42/b=0/ÉPUISÉ,
mêmes best par racine) ; Restore unitaires 4,36 → **0,58**/expansion, arène 47,2 → 37,1 % de la
phase (−21 % de temps d'arène), +4,5 % d'expansions à temps égal (run unique, mais l'économie
de trafic de pages est MÉCANIQUE — lisible dans les compteurs, pas dans une durée). PAR
DÉFAUT ; `--no-merged-pop` = témoin. Une subtilité de conception versée au code : les pages
au-delà de l'`in_use` du niveau cible ne sont PAS restaurées — leurs spans sont rendus vierges
par `RestoreMetadata` et re-carvés par `Allocate` avant tout usage, le contenu résiduel n'est
jamais lu.

**(j) LE BOOTSTRAP S'AUTO-AMORCE — le test de découverte est positif sur la masse.** Protocole
(`tools/s12_bootstrap.ps1`) : étalon A but seul (`--no-ref`, aucune ligne enregistrée
n'existe), corpus fabriqué par le solveur LUI-MÊME — les `best_approach_1of4.yrp` de trois
runs de génération à graines distinctes (3 lignes de 65-72 décisions, toutes à 1/4) — puis
trois bras d'attribution sur deux graines de mesure (300 s chacun) :

| bras | ≥1 résolution | ≥2 | ≥3 |
|---|---|---|---|
| témoin (g888 / g1234) | 608 780 / 819 007 | 1 427 / 10 433 | 0 / 1 384 |
| adaptation seule | 721 713 / 845 827 | 4 241 / 9 353 | 914 / 796 |
| **adaptation + options** | 487 908 / 517 397 | **260 273 / 242 513** | **81 702 / 68 149** |

L'attribution est nette : l'adaptation seule ≈ témoin ; **les macros multiplient les tirages à
≥2 résolutions par ×25-180 et à ≥3 par ×50-85, sur les deux graines** — à partir des seules
découvertes partielles du solveur, zéro référence. La sélection par perte de Levin retient
~15-19 macros d'un corpus de 3 lignes (les motifs répétés à l'intérieur des lignes suffisent).
C'est la réponse à la qualification du (g) : le mécanisme n'est pas qu'un exploiteur de
quasi-référence, il s'AUTO-AMORCE sur la masse. Limites : best reste 1/4 partout à ce budget
(196 s de tirages) — la conversion en 2ᵉ Liger demande l'ITÉRATION (gen2 armée des macros →
corpus plus riche) ou plus de budget ; un run par graine, mais 4 paires structurelles (≥2 et
≥3 × 2 graines) toutes dans le même sens et à ces ordres de grandeur, la dispersion ne suffit
pas comme explication.

**(k) Le rétro robustifié — trois correctifs de la revue « effets au-dessus des règles ».**
(1) **Corpus→graphe** : `LiftPolicyRun` nourrit désormais le graphe de recettes pendant le
rejeu d'adaptation — matériaux et zones RÉELS des invocations du corpus, voies d'exception
comprises (matériaux depuis le deck, invocations sans matériaux, substituts) ; le graphe est
créé et amorcé AVANT `BuildAdaptRuns`. C'est le correctif de l'œuf-et-la-poule (« un graphe
qui n'apprend que des invocations réussies ») : les recettes OBSERVÉES arrivent dès la
première seconde, sans lire un texte d'effet — règle 3 au sens strict. (2) **Consommation
INTRA-RECETTE** : au cadre du haut de `DistanceLocked` (la recette du produit cible et ses
matériaux directs), chaque exigence servie RÉCLAME ses entités — un même corps ne peut plus
être à la fois le matériau nommé « Leo Dancer » et l'un des « 3 monstres Lunalight » de la
même invocation, et « 2 "Nom" » cesse d'être satisfait par un seul exemplaire. Les
réclamations repartent à zéro entre recettes et entre produits ; en dessous, l'ancien comptage
partagé et le mémo (une consommation inter-invocations sur-estimerait — les corps recyclent
par le cimetière — et un mémo sous réclamation dépendrait de l'ordre de visite). Strictement
sous-estimant, déterministe. (3) **Ordre canonique dans `Observe`** : la même invocation vue
avec ses matériaux dans deux ordres est LA MÊME recette (elle occupait deux des huit places
par produit). Santé stricte : 0 ligne hors durées. Le 2×2 décisif (h plat/amorcé × coût
actuel/canonique, `tools/s12_retro_ab.ps1`) tourne sur ce moteur.

**(l) Le 2×2 du rétro : NEUTRE sur les seize paires — et les deux questions tombent pour la
même cause.** Étalon A déterministe (`tools/s12_retro_ab.ps1`), quatre bras : h plat/amorcé ×
coût actuel/canonique, sur le moteur robustifié (k). L'amorce pose bien son gradient en tête
(Liger 5, Bagooska 3 — inchangés par la consommation intra-recette, comme attendu à terrain
vide) ; et pourtant expansions (±2 %), `best` par racine et `hR` (3,0-4,0) sont identiques
dans les quatre bras. Deux mécanismes, une cause :

1. **L'observation écrase l'amorce là où le finisseur travaille.** Des centaines à des
   milliers d'invocations observées par racine (`rec=` 640-14 831) remplacent les recettes du
   texte dès les premières secondes (règle 3, comportement voulu) — et aux états profonds des
   racines de recul, les matériaux des recettes observées sont largement PRÉSENTS : la
   distance retombe au compte des manquants, c'est-à-dire au h plat. Le gradient amorcé
   n'existe qu'à terrain vide — la MONTÉE — que le finisseur ne visite jamais.
2. **h est un PLATEAU aux états visités, donc toute forme de coût est neutre.** `hgoal` est
   hérité du parent par TOUS ses enfants — le terme h décale un sous-arbre en bloc et ne
   réordonne qu'entre sous-arbres de h différents. h quasi constant → e^h comme (d+h) sont un
   offset : l'ordre est celui de Levin pur. `--phs-canonical` reste donc non départagé PAR
   CONSTRUCTION tant que h est plat — pas seulement non mesuré.

**Le verrou reste LE H PLAT — le diagnostic de la session 8 tient mot pour mot.** Le rétro
dans sa forme actuelle est réfuté comme h de FINISSEUR : mécanisme vivant, gradient réel au
départ, éteint là où il pèse (il n'est branché que dans `RunLevin`, pour raison de coût — la
seule phase où son gradient s'effondre). Sa valeur résiduelle est dans la MONTÉE (tirages),
où il n'est pas branché — et où les MACROS du bootstrap (j) font déjà ce travail par un autre
canal, avec un effet mesuré de deux ordres de grandeur. Conséquence pratique : la voie
« heuristique qui décroît » passe désormais par les options/macros (masse), pas par une
distance de rétrosynthèse dans le coût du finisseur ; le graphe de recettes reste précieux
comme MODÈLE (comptage dérivé, faisabilité, futur conditionnement sémantique des macros).

**(m) Où en est le chantier, au terme de la session 12.** Débit : `Process` au par-appel
27,2 µs → **18,6 (LTO+PGO, −32 %)** ; finisseur **+92 %** d'expansions à temps égal
(`dive_full`) puis **−21 % de temps d'arène** en plus (`merged_pop`) ; cadran des workers
tranché (16 validé, 24 destructeur, le mur est la bande passante). Masse : les options par
perte de Levin — **8/8 ×3 en exploitation (g)** et **×50-180 sur les résolutions profondes en
BOOTSTRAP auto-amorcé (j)** — le premier mécanisme de masse gagnant du projet, confirmé sur
les deux régimes. Réfutés proprement : lifo_ties, la fenêtre de position, le rétro comme h de
finisseur et la forme du coût PHS aux h plats (l). L'instrument a changé de statut : compteur
de tirages déclassé ; par-appel sous `--profile`, contrôles exhaustifs déterministes et
sorties structurelles (≥k résolutions, best) le remplacent.

**Ce qui reste, par rendement estimé décroissant :** (1) **l'ITÉRATION du bootstrap** — gen2
armée des macros → corpus plus riche → macros meilleures, et le conditionnement SÉMANTIQUE
(`ctx`) des macros (le graphe de recettes en est le substrat naturel) ; (2) un run LONG
d'exploitation pour convertir les 8/8 en lignes complètes avec rips ; (3) le coût fixe
d'arène résiduel du finisseur (~37 % de la phase après merged_pop) ; (4) `Adv::Goal` traité
comme `Adv::Dead` dans `advance()` (9.18 (g), toujours non tranché). Les réserves d'usage :
`dive_full` et `merged_pop` tiennent sur l'identité exhaustive, pas sur un compteur ; PGO sur
des distributions disjointes ×3 ; les options sur 6/6 paires (exploitation) et 4/4 paires à
deux ordres de grandeur (bootstrap).

### 9.20 Session 13 : l'itération du bootstrap MONTE — 1/4 → 2/4, et le corpus CUMULÉ est la forme gagnante

Programme du prompt : itérer le bootstrap (gen2 armée des macros), le run long d'exploitation,
puis le conditionnement sémantique des macros. Santé d'entrée : binaire PGO de fin de session 12
vérifié (22 lignes de diff contre `s12_sante_fin.log`, que des durées et le nom d'outdir).

**(a) La gen2 ARMÉE, appariée à la gen1 : mêmes graines, même budget, seule l'armure change —
et la graine 4242 écrit le PREMIER 2/4 en mode but seul.** Protocole
(`tools/s13_bootstrap_gen2.ps1`) : les trois graines de génération de la session 12
(888/1234/4242, 240 s, étalon A but seul) relancées avec `--adapt s12_boot_corpus
--adapt-passes 4 --options 256`. Comparaison par paire à graine égale :

| graine | gen1 (nue) ≥2 / ≥3 | gen2 (armée) ≥2 / ≥3 | approche écrite gen1 → gen2 |
|---|---|---|---|
| 888 | 4 715 / 980 | 183 358 / 33 021 | 1/4 (65 déc.) → 1/4 (76 déc.) |
| 1234 | 4 671 / 1 099 | 104 347 / 21 614 | 1/4 (72 déc.) → 1/4 (61 déc.) |
| 4242 | 6 099 / 1 240 | 186 060 / 35 722 | 1/4 (49 déc.) → **2/4 (214 déc.)** |

Le ×20-40 sur ≥2/≥3 re-confirme 9.19 (j) en appariement strict ; le fait NOUVEAU est la
conversion : la 2ᵉ Liger que la session 12 annonçait comme le fruit attendu de l'itération est
tombée dès la gen2, à budget de génération inchangé (240 s). `corpus2` = les 3 approches gen2,
dont la ligne 2/4.

**(b) L'A/B des corpus (le cœur de l'itération) : le CUMUL convertit sur les deux graines de
mesure.** Trois bras intercalés par graine (300 s, mêmes graines 888/1234, même binaire) :
`opt1` = adapt+options sur corpus1 (le témoin re-mesuré du jour), `opt2` = corpus2 seul,
`opt12` = corpus1 + corpus2 (`--adapt` répété) :

| bras | catalogue | absorbées/prise | avortées | ≥2 | ≥3 | best |
|---|---|---|---|---|---|---|
| g888 opt1 | 11 macros (moy. 4,4) | 2,9 | 604 k | 218 306 | 65 823 | 1/4 |
| g888 opt2 | 3 macros (moy. 7,3) | 5,4 | 261 k | 279 280 | 73 319 | 1/4 |
| g888 **opt12** | 6 macros (moy. 7,0) | 5,3 | 246 k | 273 953 | 73 436 | **2/4** (181 déc.) |
| g1234 opt1 | 11 | 1,4 | 2 854 k | 272 583 | 75 463 | 1/4 |
| g1234 opt2 | 3 | 5,5 | 224 k | 291 246 | 81 193 | 1/4 |
| g1234 **opt12** | 6 | 5,5 | 159 k | 277 241 | 94 665 | **2/4** (189 déc.) |

Trois faits structurels. (1) **best 2/4 dans la MESURE elle-même, sur les deux graines,
uniquement dans le bras cumul** — les deux autres bras restent 1/4 partout. (2) ≥2 et ≥3
au-dessus du témoin corpus1 sur les 4 paires (signe cohérent ; amplitudes +7 à +28 %, modestes
parce que tous les bras partent déjà armés). (3) **La qualité des macros s'améliore d'une
génération à l'autre** : minées sur des approches déjà armées, elles sont moins nombreuses et
plus longues (3-6 macros de longueur moyenne 7,0-7,3 contre 11 de 4,4), absorbent 2 à 4× plus
par prise et avortent 2,3 à 18× moins. Le compteur ≥4 (131/147/537 et 0/342/0) est trop
dispersé pour en tirer quoi que ce soit. Verdict : **la boucle InnateCoder ne fait pas que
s'amorcer (9.19 (j)), elle MONTE — et sa forme de production est le corpus CUMULÉ** (on ne
jette jamais les lignes des générations passées).

**(c) Deux mécanismes écrits cette session, opt-in et dormants par défaut** (bit-à-bit
identiques éteints), pour profiter du même relink : (1) **`--options-ctx <tol>`** — la garde
SÉMANTIQUE des macros désignée par 9.19 (g) après la réfutation de la fenêtre positionnelle :
une macro n'est proposée que si le contexte courant (cartes cibles posées EXACTES, main à
±tol) est compatible avec un contexte de départ d'une de ses occurrences du corpus. Le même
test entre dans le modèle de sélection par perte de Levin (le catalogue retenu peut changer) et
dans le rollout ; `PolicyStep::ctx` existait déjà des deux côtés (relevé inconditionnel dans
`LiftPolicyRun`, calculé au tirage quand la garde ou `ctx_shrink` l'exigent). `15` ≈ garde sur
les seules cartes posées ; l'A/B est `tools/s13_options_ctx_ab.ps1`. (2) **`--finisher-post-goal`**
— le point 9.18 (g) est TRANCHÉ par lecture : `GoalCheck` ENREGISTRE la solution avant que
`advance()` ne rende `Adv::Goal` (rien n'était perdu), mais sous `--optimize` la récupération
d'après-but (piège 35) était bien structurellement absente de `RunLevin` (nœud-but = jamais
d'enfants). Le drapeau fait continuer la ligne après le but sous anytime, comme les rollouts.
À juger sur A/B avant tout défaut.

**Piège de build reconduit** : après le relink de ces mécanismes, `tools/s13_pgo.ps1` —
variante s13 du pipeline PGO qui N'ÉCRASE PAS le témoin s12 (`combosolver_lto_s13.exe` neuf)
et ajoute un 4ᵉ run d'entraînement avec options+ctx pour couvrir le mineur v3 et la garde.

**(d) LE RUN LONG D'EXPLOITATION CONVERTIT — premières lignes complètes AVEC rips sur
l'étalon B contraint.** Chantier 2 de 9.19 (m) : le bras `sel` gagnant (adapt + options 256),
budget ×20 (1200 s), graines 888 et 1234 (`tools/s13_exploitation_longue.ps1`). Graine 888 :
**32 lignes atteignent le board via le finisseur** (lignes `crete-rip 8/8 <-- BUT` sur les
racines de recul 20/40), **16 solutions écrites**, toutes à **19 brûlées / 56 actions /
264-266 décisions — contre 276 décisions pour la référence humaine** à brûlées et actions
égales. Vérification sur pièce (`solution_00`, mode juge, rejeu depuis zéro) : **0 MSG_RETRY,
garde tenue sur 27 fenêtres adverses (0 découverte), résolutions 2/2 Omega@terrain +
1/1 Trishula@terrain**. C'est la première production de bout en bout du montage contraint —
en 9.16 il rendait « best 3/8, aucune ligne » à 90 s, et 9.19 (g) notait « aucune ligne
complète avec rips encore (60 s) ». Graine 1234 : pas de conversion à 1200 s
(`best_approach_8of8.yrp`, 205 décisions, crête-rip 7/8) — une graine sur deux convertit à ce
budget ; la dispersion reste réelle. À noter : le plafond d'écriture (16) a laissé 16
candidates non examinées — l'avertissement du rapport le dit lui-même (piège documenté, à
trier par coût si un run futur en produit plus).

**(e) Observation versée au dossier — `canonical_zones` est un mécanisme DORMANT.** En
répondant à une question de séance (« les zones et positions branchent-elles ? ») : le choix
de colonne émet bien un choix par colonne libre, mais l'identité de politique ignore la
colonne (`plan_key` sans `seq`), la nouveauté rend les colonnes jumelles muettes, et
l'équivalence de but ignore colonne ET position de combat (face seule, piège 26) ; seul le
digest d'état garde la position complète (voulu — fusionner ferait disparaître des lignes,
piège 47). MAIS `EnumOptions::canonical_zones` (une colonne représentative par type de zone)
existe, est documenté, et n'est allumé NULLE PART — un candidat d'A/B gratuit pour le
branchement du finisseur, de la même famille que le « mécanisme silencieusement absent du
chemin ».

**(f) La gen3 PLAFONNE à budget de génération constant.** Gen3 armée du CUMUL
(`tools/s13_bootstrap_gen3.ps1`, mêmes graines, 240 s) : les trois graines rendent 1/4
(74/87/53 décisions), ≥2/≥3 au niveau de la gen2 (147-220 k / 33-49 k), catalogue identique au
bras cumul (6 macros, 25,8 → 18,4). Lecture : le grand gain de l'itération était l'ARMEMENT
lui-même (gen1→gen2) ; ré-miner sur un corpus plus riche ne franchit pas de cran
supplémentaire automatiquement à ce budget — la conversion 2/4 reste un événement
stochastique (~1 run armé sur 3-5 à 240-300 s : g4242 gen2, opt12 g888 et g1234, contre six
autres runs armés restés à 1/4). Les leviers du cran suivant : le BUDGET de génération
(240 s est le plancher historique), la garde ctx si elle gagne, et le corpus cumulé qui
conserve la ligne 2/4 (c1+c2+c3 pour la prochaine itération — on ne jette jamais).

**Observation d'environnement versée au dossier** : entre la santé du matin et celle du
relink, le compte de cartes chargées est passé de 24 758 à 24 767 (+9, mêmes 33 bases) — une
`.cdb` de l'installation vivante a été mise à jour en arrière-plan (hors dépôt). Sans effet
mesuré : tout le reste de la santé est identique au champ près (273 digests, 210/273,
209 candidates, 16 replays) et les decks des étalons sont figés. À garder en tête si un jour
un A/B inter-jours diverge sur l'énumération : la base vivante N'EST PAS épinglée,
contrairement aux scripts.

**(g) La garde SÉMANTIQUE des options (`--options-ctx`) : vivante, NON-destructive — et
positive sans être tranchante.** Rebuild + santé stricte (structurel identique), pipeline PGO
s13 re-déroulé (`tools/s13_pgo.ps1`, témoin `combosolver_lto_s13.exe`, 4 régimes
d'entraînement dont un run options+ctx ; la santé INSTRUMENTÉE est restée propre — l'anomalie
C10 de 9.19 (e) ne s'est pas reproduite une seconde fois). A/B
(`tools/s13_options_ctx_ab.ps1`), trois bras : off / `ctx 1` (posées exactes, main ±1) /
`ctx 15` (posées seules).

*Étalon B contraint (60 s ×3, graine 888)* : INDÉCIS — la dispersion engloutit tout (crêtes
5/8-0/8-7/8 présentes dans les deux bras, ≥3 = 146/255/65 en sommes par bras). La garde est
bien ACTIVE : prises ÷2-8, catalogue 19 → 18, perte modèle 33,8 → 31,6 (le dénominateur du
modèle rétrécit avec la garde).

*Étalon A bootstrap (300 s ×2 graines, corpus1)* — le terrain des options :

| bras | g888 ≥2 / ≥3 / best | g1234 ≥2 / ≥3 / best | avortées (g888/g1234) |
|---|---|---|---|
| off | 244 137 / 72 407 / **2/4** | 278 905 / 77 292 / 1/4 | 722 k / 2 927 k |
| ctx 1 | **334 071** / **85 897** / **2/4** | 254 103 / 78 337 / **2/4** | 534 k / 2 092 k |
| ctx 15 | 267 359 / 73 892 / **2/4** | **290 466** / **90 479** / **2/4** | 809 k / 796 k |

Lecture : (1) le mode de défaillance de la fenêtre positionnelle (≥3 = 0 partout) est ABSENT —
la garde sémantique ne bride pas les résolutions profondes ; (2) ≥3 au-dessus du bras off sur
les 4 paires gardées (de +1,4 % à +19 %) ; (3) les bras gardés convertissent 2/4 sur LES DEUX
graines (off : une sur deux) — mais la conversion est stochastique (~1 run armé sur 2-3 à ce
budget : le off du soir convertit là où l'opt1 du matin ne convertissait pas), donc 2/2 contre
1/2 est un indice, pas un fait. VERDICT : mécanisme correct, non-destructif, signe positif —
**reste OPT-IN** ; son vrai test est DANS la boucle (gen4 armée `--options-ctx 1`) et sur des
répétitions ×3 à 300 s, pas sur l'étalon B à 60 s. Le µs/appel du PGO s13 contre son témoin
LTO n'a pas été re-relevé (hygiène en attente — la mécanique du pipeline est identique à la
s12, mesurée là-bas à −21,4 %).

### 9.21 Session 14 : le bootstrap rentre DANS le run — `--options-online`, et la boucle InnateCoder en une seule traite

Mission fixée par l'opérateur en fin de session 13 : un run complet d'une seule traite doit
monter seul vers le but — **zéro corpus externe, zéro `--approach` hérité, zéro relance**. La
session 13 avait prouvé la FAISABILITÉ de la boucle en plusieurs runs (gen1 nue → macros →
gen2 armée → premier 2/4, 9.20 (a)-(b)) ; la session 14 devait l'INTERNALISER.

**(a) `--options-online` : le minage EN LIGNE, et les trois points de conception que le code
imposait.** Précédent : Marvin (arXiv:1110.2736) mémoïse ses macros PENDANT la recherche et
les utilise dans le même solve. Ici : les workers versent leurs meilleures lignes à un corpus
VIVANT, un mineur re-déroule `MineOptionCatalog` dessus toutes les `s` secondes (la même
sélection par perte de Levin, arXiv:2410.11262), et le catalogue est échangé à une frontière
sûre. Trois points, chacun imposé par une contrainte du code existant :

1. **La frontière.** `PolicyRollout` garde un pointeur BRUT dans `seqs[m]` le temps d'une
   macro active. Le rachat se fait donc entre deux itérations de niveau supérieur — aucun
   tirage en vol dans ce worker — et chaque worker tient son catalogue par un
   `shared_ptr<const>` : l'ancien reste vivant tant qu'un worker le lit.
2. **Les poids appris survivent gratuitement** : l'id d'une macro est un HASH DE SON CONTENU,
   donc une macro re-trouvée au tour suivant retrouve exactement son poids de politique. Rien
   à transférer, et c'était déjà vrai dans le code — il fallait seulement ne pas le casser.
3. **La ligne PLATE (`NrpaRun::flat`), le point qui ne se devinait pas.** Une ligne trouvée
   AVEC macros enregistre l'ID DE LA MACRO comme coup joué (les décisions absorbées ne
   produisent aucun `PolicyStep`). Re-miner dessus ne donne PAS des « macros de macros
   gratuites » comme le prompt l'espérait : la deuxième clé d'une telle macro n'est jamais
   proposée par un prompt — `choices[i].plan_key` est toujours ATOMIQUE — donc elle avorterait
   au premier pas, systématiquement. La hiérarchie émergente s'obtient par RE-APLATISSEMENT :
   le tirage enregistre, en parallèle de `steps`, la même ligne sous forme atomique, absorbées
   comprises ; une macro minée dessus peut couvrir ce qu'une macro précédente absorbait et
   sortir plus longue qu'elle. Même hiérarchie, réalisée du côté du mineur plutôt que de
   l'exécuteur. `flat` n'est rempli que sous `--options-online` (coût nul sinon).

**La POMPE À DIVERSITÉ, côté corpus** (chantier 2, première moitié). En multi-runs la diversité
venait des graines ; en une traite elle doit venir de l'intérieur. Le corpus vivant est un
ensemble borné avec **quota par worker** : dédoublonnage par signature de ligne, au plus
`--options-per-worker` lignes par worker, plafond `--options-pool` par éviction de la pire.
Le dédoublonnage porte tout le poids — mesure sur un run de 60 s : **8 609 lignes offertes,
103 retenues, 8 506 doublons**. Sans lui, les seize workers (qui repartent tous de la meilleure
séquence partagée) rempliraient le corpus de la même ligne.

**(b) Le coût du mineur, l'instrument qui décide de sa place.** Il court DANS le budget du run :
sa durée est mesurée et imprimée. Étalon B 60 s, période 15 s : 2 tours, **39 ms** de moyenne
et de pire. Étalon A 300 s, période 60 s : 3 tours, **81-114 ms au pire**. Deux ordres de
grandeur sous la seconde — le mécanisme ne mange pas ce qu'il apporte. À surveiller si le
corpus vivant ou les lignes s'allongent : le minage est en O(lignes × longueur × faisceau).

**Santé stricte** : 24 lignes de diff contre `s13_sante_pgo.log`, QUE des durées et le nom
d'outdir (273 digests, 210/273, 209 candidates, 16 replays) — le mécanisme est bien DORMANT
éteint. **PGO s14** (`tools/s14_pgo.ps1`, témoin `combosolver_lto_s14.exe`, cinq régimes
d'entraînement dont un run `--options-online` SANS corpus — le chemin neuf) : santé du binaire
PGO à 22 lignes de diff, toutes des durées. La santé INSTRUMENTÉE est restée propre pour la
troisième fois : l'anomalie C10 de 9.19 (e) ne s'est jamais reproduite.

**Note de procédure versée au dossier** : les `.pgc` des sessions précédentes ne sont pas
effacés avant l'entraînement (12 fichiers fusionnés, dont 7 hérités de la s13). C'est la
procédure telle qu'elle tourne depuis la s12 et les gains mesurés l'ont été ainsi ; elle est
conservée pour la comparabilité, pas parce qu'elle est la bonne. À trancher si un jour un
pipeline PGO rend un résultat surprenant.

**(c) CORRECTION AU PROMPT — `--nrpa-lr` n'était pas à écrire, il existait et avait été
réfuté.** Le prompt de la session 14 donnait les répétitions limitées (GNRPA-LR,
arXiv:2401.10420) pour un mécanisme « jamais essayé ici, quelques lignes de code ». Il est
implémenté **depuis la session 4** (`--nrpa-lr`, arrêt d'un niveau après R re-trouvailles du
score MATÉRIEL de la meilleure séquence) et y a été **mesuré PERDANT** : R=2, transplantation
`test 4`, 90 s — 8/8 et 36 lignes côté témoin, 7/8 et 0 ligne côté R=2. La note de l'époque
demandait explicitement une re-mesure « R plus grand, niveau 1 seulement ». Ce qui restait à
faire était donc la MESURE, pas l'implémentation.

En revanche l'autre moitié du chantier 2 — l'**adaptation lente et longue** (recette
Montparnasse, arXiv:2505.02110 / 2606.07562) — n'avait **aucun cadran** : `nrpa_alpha` et
`nrpa_iters` étaient en dur à 1.0 et 24, jamais exposés, donc jamais mesurables. `--nrpa-alpha`
et `--nrpa-iters` les sortent, et le nombre de tirages par appel de niveau (imprimé en dur
576/13824) est désormais DÉRIVÉ de `iters^L` — sans quoi la ligne aurait menti dès que le
cadran bouge, exactement la variable cachée de C15.

**(d) L'A/B DE LA MISSION : le run part NU et convertit — 3 graines sur 3.** Étalon A but seul,
`--no-ref`, **aucun `--adapt`, aucun `--approach`, une seule traite**, 300 s, trois bras
intercalés par graine (`tools/s14_options_online_ab.ps1`). Le témoin est exactement la gen1 nue
de 9.20 (a). Le juge est l'**approche ÉCRITE** — le meilleur état que le RUN ENTIER a su
atteindre, pas la ligne « NRPA … best k/4 » qui ne résume que la phase tirages (voir le piège
d'instrument ci-dessous) :

| graine | bras | approche écrite | ≥2 | ≥3 | avortées/prise |
|---|---|---|---|---|---|
| 888 | nu | 1/4 (56 déc.) | 2 614 | **0** | — |
| 888 | `--options-online 60` | **2/4 (202 déc.)** | 220 035 | **115 933** | 0,25 |
| 888 | + `--options-ctx 1` | 1/4 (54 déc.) | 113 519 | 44 840 | 0,38 |
| 1234 | nu | 1/4 (61 déc.) | 8 885 | **0** | — |
| 1234 | `--options-online 60` | **2/4 (211 déc.)** | 201 590 | **91 887** | 0,31 |
| 1234 | + `--options-ctx 1` | **2/4 (196 déc.)** | 267 479 | 91 636 | 0,44 |
| 4242 | nu | 1/4 (49 déc.) | 2 769 | 25 | — |
| 4242 | `--options-online 60` | **2/4 (190 déc.)** | 158 243 | **76 280** | 0,31 |
| 4242 | + `--options-ctx 1` | **2/4 (193 déc.)** | 170 302 | 121 433 | 0,21 |

**Le fait, en une phrase : `--options-online` convertit 3 graines sur 3 là où le run nu en
convertit 0 sur 3, et les résolutions profondes passent de 0/0/25 à 76 k-116 k.** Trois à
quatre ordres de grandeur sur l'axe ≥3, et la conversion — que 9.20 (f) donnait pour
stochastique (~1 run armé sur 3-5, et il fallait un corpus fabriqué par des runs antérieurs) —
devient SYSTÉMATIQUE dans un run qui n'a rien reçu. Le catalogue que le run se mine (6 à 17
macros, longueur moyenne 6,8-7,9, 3 tours de minage) est d'emblée à la qualité « gen2 armée »
de 9.20 (b) — 3-6 macros de longueur 7,0-7,3 — que la session 13 n'obtenait qu'au second run
et avec un corpus externe. La boucle InnateCoder est INTERNALISÉE.

**LA GARDE SÉMANTIQUE NE SURVIT PAS À SON PROPRE TEST.** 9.20 (g) la laissait « non-destructive,
signe positif, OPT-IN » en écrivant que « son vrai test est DANS la boucle ». Le voici : elle
convertit 2/3 contre 3/3, elle divise ≥3 par 2,6 sur la graine 888, et — la lecture
diagnostique — elle **AUGMENTE le taux d'avortement** sur les deux graines où l'appariement est
propre (0,25 → 0,38 ; 0,31 → 0,44), là où la session 13 la mesurait en train de le DIVISER par
2 à 8 en régime de corpus statique. L'explication tient à la sélection : la garde rétrécit le
dénominateur du modèle, donc le catalogue retenu est plus petit (4-11 macros contre 6-17), et
la « meilleure macro par première clé PARMI LES COMPATIBLES » est souvent un moins bon ajusté
que la meilleure tout court. VERDICT : la garde reste implémentée et opt-in, mais **la forme
recommandée du minage en ligne est SANS elle**. Le signe positif de 9.20 (g) était un indice
de régime statique, pas une propriété du mécanisme — et il est retiré pour le régime en ligne.

**PIÈGE D'INSTRUMENT rencontré en séance, à verser au dossier.** La ligne de résumé
`NRPA … best k/4` ne couvre QUE la phase tirages ; sur la graine 888 le bras en ligne y affiche
1/4 alors que le run a écrit `best_approach_2of4.yrp` — la 2ᵉ Liger avait été trouvée par les
tirages ENRACINÉS du finisseur, invisibles dans cette ligne. Lire le bootstrap sur elle aurait
compté une conversion sur trois au lieu de trois sur trois. Le juge est l'approche ÉCRITE ;
c'est d'ailleurs déjà la colonne que 9.20 (a) lisait, mais rien ne le disait dans l'outil.
`tools/s14_lecture.ps1` la lit désormais, et imprime les deux côte à côte.

**(e) LES RÉPÉTITIONS LIMITÉES (R=4) : positives sur les résolutions profondes, NEUTRES sur la
conversion — et le verdict de la session 4 n'est pas renversé, il est CIRCONSCRIT.** Deux bras
neufs, appariés aux bras déjà mesurés du (d) — mêmes graines, même budget, même binaire
(`tools/s14_diversite_ab.ps1`), R=4 (strictement entre « tout de suite » et la stagnation à 8) :

| graine | bras | approche | ≥2 | ≥3 | avortées/prise |
|---|---|---|---|---|---|
| 888 | nu | 1/4 | 2 614 | **0** | — |
| 888 | nu + `--nrpa-lr 4` | 1/4 | 8 345 | **1 536** | — |
| 1234 | nu | 1/4 | 8 885 | **0** | — |
| 1234 | nu + `--nrpa-lr 4` | 1/4 | 6 257 | **701** | — |
| 888 | en ligne + ctx | 1/4 | 113 519 | 44 840 | 0,38 |
| 888 | en ligne + ctx + lr 4 | **2/4** | 198 647 | **100 887** | **0,14** |
| 1234 | en ligne + ctx | **2/4** | 267 479 | 91 636 | 0,44 |
| 1234 | en ligne + ctx + lr 4 | 1/4 | 207 217 | **118 250** | **0,20** |

Trois lectures. (1) **≥3 monte sur les QUATRE paires**, et sur les deux bras nus il passe de
ZÉRO à 701 et 1 536 : les répétitions limitées ouvrent seules l'axe des résolutions profondes
que le run nu n'atteignait jamais. (2) **La conversion est un lavage** : le bras armé convertit
1/2 avec comme sans (la graine qui convertit CHANGE — 888 gagne, 1234 perd). (3) La lecture
mécaniste, la plus intéressante : **le taux d'avortement des macros CHUTE** (0,38 → 0,14 ;
0,44 → 0,20). C'est la pompe à diversité qui fait exactement ce qu'on lui demandait — des
lignes plus variées dans le corpus vivant donnent des macros mieux ajustées, donc qui cassent
moins. Le mécanisme n'est pas seulement compatible avec le minage en ligne : il l'ALIMENTE.

**Ce que cela fait au verdict de la session 4** — qui mesurait R=2 perdant (8/8 → 7/8, 36
lignes → 0) : il n'est PAS renversé. R=2 reste réfuté ; ce qui est mesuré ici est R=4, dans un
autre régime (bootstrap but seul en une traite, pas transplantation), avec un juge différent.
Les deux tiennent ensemble : arrêter un niveau trop tôt casse la convergence, l'arrêter au bon
moment libère de la diversité. Statut : **opt-in, positif sur ≥3, à re-juger sur la conversion
avec plus de graines** — et le bras à apparier la prochaine fois est `--options-online` NU, pas
`+ ctx` (l'A/B (d) a désigné la forme gagnante APRÈS que ce script ait été écrit ; l'appariement
reste valide, il n'est simplement pas adossé à la meilleure forme connue).

**(f) LE TEST REINE (1 200 s, une traite) : le budget seul suffit pour 2/4, et le finisseur ne
travaille PAS dans ce régime.** Deux bras, graine 4242, `tools/s14_traite_longue.ps1`. Le
témoin nu est RELANCÉ à ce budget et non supposé — précisément parce que 9.20 (d) avertit que
« le budget convertit » :

| bras (1 200 s) | approche écrite | ≥2 | ≥3 | lignes écrites | tours de minage |
|---|---|---|---|---|---|
| nu | 2/4 (201 déc.) | 32 259 | 6 028 | **0** | — |
| `--options-online 90` | 2/4 (197 déc.) | **1 283 963** | **453 681** | **0** | 9 |

Lecture honnête : **sur le juge « approche écrite », les deux bras sont à égalité à ce budget.**
Le minage en ligne garde ×40 sur ≥2 et **×75 sur ≥3**, et il atteint 2/4 dès la PHASE TIRAGES
là où le nu y reste à 1/4 (le nu ne trouve son 2/4 que dans les tirages enracinés du
finisseur) — mais l'avantage de conversion mesuré à 300 s (3/3 contre 0/3) **ne se transporte
pas à 1 200 s** : à budget long, le run nu y arrive aussi. Le gain du mécanisme est donc en
BUDGET, pas en plafond : il rend en 300 s ce que le nu met 1 200 s à obtenir.

**LE FAIT NOUVEAU, plus important que la mesure elle-même : les racines d'archive du finisseur
rendent ÉPUISÉ en 0 à 13 EXPANSIONS.** Sur 37 racines, presque toutes s'épuisent en 0,0 s ;
deux seulement (`archive 00`, `archive 13`) atteignent ~800 expansions. Toute la conversion de
ce régime est faite par les tirages ENRACINÉS (`recul 0/20/40`, des millions d'états), jamais
par le LTS. Diagnostic : l'archive Go-Explore est classée `résolutions<<44 | overlap<<36 |
~décisions` ; les résolutions SATURENT ici (tout est à `r4`), donc l'archive se remplit d'états
de FIN DE LIGNE — elle range ce qui est terminal, pas ce qui est prometteur. Le « premier
retour, puis explore » n'a plus rien à explorer.

**Conséquence de priorité, à écrire avant de coder** : les ARÊTES MACRO dans `RunLevin`
(chantier 3, §9.21 (g)) n'ont **rien à compresser dans ce régime** — on ne compresse pas une
recherche qui s'épuise en 3 expansions. Elles restent justifiées sur l'étalon B contraint, où
9.20 (d) mesure 32 lignes arrivées au board PAR le finisseur ; elles ne le sont pas sur
l'étalon A. Le mécanisme est donc écrit, compilé et opt-in, et son juge est l'étalon B — pas
le bootstrap. C'est le genre d'inversion de priorité qu'un run long payé pour lui-même rend
visible et qu'aucun raisonnement a priori n'aurait donné.

**Troisième signal, dans la vie du minage : le catalogue RÉTRÉCIT quand le budget monte** — 4
macros à 1 200 s contre 6 à 17 à 300 s, pour 18 492 lignes offertes et 97 retenues. Le corpus
vivant s'homogénéise : la pompe à diversité (dédoublonnage + quota par worker) tient 300 s,
pas 1 200. C'est la limite du mécanisme tel qu'il est réglé, et elle désigne ses propres
cadrans (`--options-pool`, `--options-per-worker`, `--nrpa-lr`) comme le chantier suivant.

**(g) LE PLAFOND DE LONGUEUR DES MACROS EST SATURÉ — le levier de masse que les logs
désignent.** Relevé sur les NEUF catalogues auto-minés de la session (A/B, diversité, smoke) :
`max = 8` PARTOUT, moyennes 6,8 à 7,9. `--options-len` vaut 8 par défaut : la sélection par
perte de Levin veut des macros plus longues et n'en a pas le droit. Et le comptage du board le
met en perspective — la cible est 3 Liger Dancer (Fusion) + 1 Bagooska (Xyz), la référence
humaine met **17 invocations pour 273 décisions, soit ~16 décisions par invocation** : l'unité
naturelle du domaine, le cycle d'invocation complet, fait DEUX FOIS la taille qu'une macro a le
droit d'atteindre. Une macro actuelle ne peut couvrir qu'une demi-invocation.
Pilote (`tools/s14_masse_ab.ps1`) : lever le plafond à 16 et 24 et ne lire QUE le catalogue
(longueur max et moyenne) avant de juger le reste. Deux issues, toutes deux informatives — les
macros s'allongent, et le plafond liait ; ou elles restent à 8, et c'est le SUPPORT qui lie
(une séquence longue se répète moins), auquel cas le bras suivant est `--options-support`.
Surveiller la durée du minage : elle croît avec `max_len` (sous-séquences de longueur
2..max_len à chaque position) et doit rester sous la seconde.

**(h) PIÈGE D'ENVIRONNEMENT — le gabarit N'ÉTAIT PAS ÉPINGLÉ NON PLUS, et il a changé EN PLEIN
PILOTE.** Tous les scripts d'étalon A pointaient sur `D:\ProjectIgnis\replay\_LastReplay.yrpX`
— le « dernier replay » de l'installation EDOPro, donc un fichier VIVANT, réécrit dès que
quelqu'un joue un duel. Il l'a été le 15/08/2026 à **23:42:23**, entre le premier et le deuxième
run du pilote des leviers de masse. Les deux runs suivants sont morts sur
`!! aucun yrp1 embarqué … le solveur exige un replay exportable`.

Trois enseignements, dans l'ordre d'importance :

1. **L'outil a échoué BRUYAMMENT.** C'est le comportement voulu et il a fonctionné : pas de
   mesure silencieusement fausse, un code de sortie et un message explicite.
2. **L'en-tête de gabarit a permis de VÉRIFIER au lieu de supposer.** Le rapport imprime
   `drapeaux 0x2e812, 8000 LP, main 3, pioche 1, adversaire 43+15 cartes` et la main de départ ;
   tous les runs terminés de la session portent la MÊME ligne, donc aucune mesure rapportée
   n'est contaminée. C'est exactement ce que le README promettait de cette impression
   (« imprimé en tête du rapport pour que ce soit vérifiable et non promis ») — et c'est la
   première fois qu'elle sert.
3. **C'est la même famille que la `.cdb` vivante de 9.20** : les scripts SONT épinglés
   (`--scriptdir`), la base et le gabarit ne l'étaient pas. Corrigé : le gabarit vit désormais
   dans le dépôt (`gabarits/etalon_a_lunalight.yrp`) et les quatre scripts s14 y pointent.
   N'importe quel replay du même duel fait l'affaire — sous `--no-ref` seuls les paramètres de
   duel comptent, la ligne est écartée — donc une APPROCHE produite par le solveur lui-même
   sert de gabarit, ce qui est vérifié : en-tête identique au champ près.

Reste non épinglée : la `.cdb`. À traiter le jour où un A/B inter-jours diverge sur
l'énumération.

**(i) LES DEUX LEVIERS DE MASSE « GRATUITS » SONT RÉFUTÉS — et l'un d'eux réfute une lecture de
cette session même.** Pilote 4 bras, graine 888, 300 s, gabarit épinglé (`tools/s14_masse_ab.ps1`) :

| bras | catalogue | absorbées/prise | avortées | perte modèle | ≥2 | ≥3 |
|---|---|---|---|---|---|---|
| témoin (`len 8`) | **10 macros**, moy. 7,0 | 4,2 | 315 639 | 44,2 → **28,6** | **201 189** | **91 167** |
| `--options-len 16` | **1 macro** (16) | 10,4 | 40 619 | 44,5 → 35,4 | 142 608 | 63 914 |
| `--options-len 24` | **1 macro** (24) | 14,9 | 16 056 | 45,8 → 32,0 | 125 916 | 55 083 |
| `--options-pool 24` (3/worker) | 4 macros, moy. 7,5 | 3,0 | 110 312 | 41,1 → 31,7 | 118 962 | 45 339 |

**Le plafond de longueur : RÉFUTÉ, et le raisonnement qui l'avait désigné était faux.** Lever le
plafond allonge bien les macros (16 et 24 exactement — elles restent collées au plafond), mais le
catalogue s'effondre à UNE macro et la **perte de Levin, le critère du mineur lui-même, EMPIRE**
(28,6 au témoin contre 32,0 et 35,4). Dix macros courtes battent une longue, sur ≥2 comme sur ≥3.
L'inférence « les neuf catalogues saturent à max=8, donc le plafond bride » était une lecture de
SYMPTÔME : la sélection gloutonne prend d'abord le candidat le plus long disponible, quel que soit
le plafond — la saturation est une propriété de l'algorithme de sélection, pas un manque.
Le corpus vivant élargi (`--options-pool 24`) perd également sur toutes les colonnes : plus de
lignes, plus hétérogènes, moins de motifs à support ≥ 2, donc un catalogue plus pauvre.

**LA MESURE LA PLUS UTILE DU PILOTE EST SON TÉMOIN.** Même commande, même graine, mêmes réglages
que le bras `s14_on_on_888` de (d) — et il rend **1/4 là où l'autre rendait 2/4** (≥3 : 91 167
contre 115 933). C'est la dispersion à graine fixée, documentée depuis la session 4 (l'ordre des
échanges entre workers dépend de l'ordonnancement). **Conséquence rétroactive à appliquer à toute
cette session : une conversion k/4 lue sur UNE graine ne vaut rien.** Ce qui tient dans (d) est
l'appariement 3/3 contre 0/3 sur trois graines et les deux ordres de grandeur sur ≥3 ; ce qui ne
tient pas est n'importe quelle lecture mono-graine — y compris le 2/4 de `len16` ci-dessus, qui
ne doit PAS être compté comme un signe.

**Où cela laisse le chantier de la masse.** Les dials du catalogue sont épuisés et perdent. Le
diagnostic qui reste debout est structurel et il a été formulé en séance par l'opérateur : *si
Tenki cherche autre chose que Gold Leo, la ligne est morte en ~20 décisions, et le solveur
devrait le découvrir seul et vite.* L'information EXISTE — le tirage se termine, c'est
observable — mais le solveur n'a **aucun endroit où la mettre** : `PolicyRollout` n'a pas de
table de transposition (vérifié : la TT ne vit que dans les descentes DFS et `RunLevin`), et sa
seule mémoire est UN POIDS PAR CODE DE COUP, aveugle à l'état. Le niveau contextuel ne sauve
rien ici : son descripteur est (cartes cibles posées, taille de main), soit **(0, 3) pour toutes
les branches à la première décision** — il ne peut, par construction, distinguer aucune des
cibles de Tenki. Le sous-arbre mort est donc re-parcouru des centaines de milliers de fois, la
même conclusion atteinte et oubliée à chaque fois.
Le mécanisme manquant est un BANDIT SUR LE HAUT DE L'ARBRE : une table
`(digest d'état, coup) → (visites, meilleur score)` sur les k premières décisions, rétropropagée
en fin de tirage, lue comme biais d'échantillonnage — de l'UCT greffé sur les premiers plis d'un
NRPA, posé là où le branchement est étroit (quelques centaines d'états couvrent les dix premières
décisions) et où tout se joue. À instrumenter AVANT de coder : une sonde qui imprime, pour chaque
cible de Tenki, visites / meilleur matériel / profondeur de mort — elle dira si le solveur
concentre un jour sur la bonne, ou s'il compense l'ignorer par la masse.

**(j) ÉTUDE arXiv EN SÉANCE, ET LE CORRECTIF QU'ELLE DÉSIGNE : nous citions MCPS sans
l'appliquer.** Question posée par l'opérateur : *si Tenki cherche autre chose que Gold Leo la
ligne meurt en ~20 décisions ; le solveur ne devrait pas avoir besoin d'indice, il devrait
trouver Gold Leo seul et vite.* Diagnostic vérifié dans le code avant toute recherche
bibliographique :

- `PolicyRollout` n'a **aucune table de transposition** (elle ne vit que dans les descentes DFS
  et dans `RunLevin`) : le sous-arbre mort est re-parcouru des centaines de milliers de fois, la
  même conclusion atteinte et oubliée à chaque fois ;
- la seule mémoire des tirages est **un poids par code de coup**, aveugle à l'état ;
- le niveau contextuel censé corriger cela a pour descripteur (cartes cibles posées, taille de
  main), soit **(0, 3) pour TOUTES les branches à la première décision** — il ne peut, par
  construction, séparer deux ouvertures l'une de l'autre.

L'étude arXiv (menée en séance) donne trois choses. (1) **NEPA (arXiv:2202.13706)** nomme notre
situation mot pour mot — « NRPA cannot exploit knowledge acquired in one branch of the state
tree for another one which starts differently » — mais son remède (recherche de voisinage) vise
des solutions permutables et ne transpose pas. (2) **MCPS (arXiv:2510.06381)** — que
`search.h` CITE DÉJÀ comme inspiration du niveau contextuel — conditionne sur « toutes les
parties qui contiennent TOUS LES COUPS DU CHEMIN de la racine au nœud ». Nous en avions repris
la formule de combinaison et remplacé le conditionnement par un descripteur maison. **C'est un
écart au papier, de la même famille exacte que les options v1/v2 (9.19 (g)), dont le retour au
critère du papier avait renversé le verdict.** (3) Compléments versés : GRAVE2/GRAVER
(arXiv:2602.23318, GRAVE en mémoire contrainte), Batch MCTS (arXiv:2104.04278, arbre ET table de
transposition dans le même algorithme), et arXiv:2406.03361 qui identifie « presence of dead
ends in the environment » comme un attribut où la recherche par sous-buts paie — corroboration
indépendante que notre domaine est du bon type pour les macros.
*Limite de couverture, dite franchement* : la détection d'impasse en planification classique
vit surtout à ICAPS/AAAI, très peu sur arXiv ; les deux requêtes de cet axe n'ont ramené que de
la robotique. Cette étude couvre le versant Monte-Carlo, pas le versant symbolique.

**`--mcps <k>` : le conditionnement du papier, greffé sur l'infrastructure existante.** Le
contexte du niveau contextuel devient la **somme mélangée des coups joués** sur les k premières
décisions de la ligne, figée au-delà. Quatre points de conception :

1. **Commutatif, pas séquentiel** — MCPS parle des parties qui CONTIENNENT les coups (un
   ensemble), et notre principe directeur est la recherche sur le GRAPHE d'états (A puis B et B
   puis A convergent). Somme et non XOR : nos lignes rejouent le même coup plusieurs fois (trois
   Kaleido Chick dans la référence) et un XOR les annulerait deux à deux.
2. **La troncature à k est une contrainte de MÉMOIRE, pas de méthode.** La retenue
   s = n/(n+shrink) gère déjà la profondeur toute seule (contexte jamais revu → n = 0 → repli
   exact sur le poids global) ; ce qu'elle ne gère pas, c'est une table à une case par nœud
   visité. La troncature la borne par la largeur du HAUT de l'arbre — étroite, et c'est
   précisément là que l'information de vie ou de mort se trouve. Mesure au smoke, k = 6 :
   **207 cases** au worker le plus chargé, sur un plafond de 262 144. Mémoriser les six
   premières décisions coûte quelques kilo-octets — la prémisse de l'opérateur, chiffrée.
3. **Deux champs et non un.** `PolicyStep::ctx` reste le descripteur SÉMANTIQUE (la garde des
   options le décode en (posées, main) ; `ForecastSearchCost` lit sa composante haute pour les
   points d'indice de sqrt-LTS) ; `PolicyStep::cctx` porte le conditionnement effectif. Écraser
   `ctx` aurait cassé ces deux lectures en silence.
4. **`--mcps` implique `--ctx-shrink`** (défaut 8) et le DIT : sans le niveau contextuel, le
   conditionnement serait calculé et lu par personne — la famille du « mécanisme silencieusement
   absent du chemin ».

**Écart assumé avec le papier, à ne pas cacher** : nous prenons le CONDITIONNEMENT de MCPS, pas
sa SOURCE de statistiques. MCPS moyenne sur toutes les parties ; NRPA n'adapte que sur la
meilleure séquence, donc notre table ne voit que les chemins des bonnes lignes. Le gradient y
monte le coup joué et descend ses alternatives, branche par branche — l'effet recherché — mais
ce n'est pas la moyenne du papier. Si l'A/B est neutre, c'est la première chose à changer.

**Santé stricte** : 24 lignes de diff éteint (que des durées), et 20 après le PGO re-déroulé
(`tools/s14_pgo_b.ps1`, témoin `combosolver_lto_s14b.exe`), dont **aucune** hors durées, tailles
et nom d'outdir. Le mécanisme est dormant à l'octet près.

**Le montage d'A/B, et le bras qui empêche une conclusion trop facile.** Quatre bras
(`tools/s14_masse_ab.ps1`) sur deux graines : `temoin` (`--options-online 60`), **`ctx8`
(`--ctx-shrink 8` SEUL, ancien conditionnement)**, `mcps6`, `mcps12`. Le bras `ctx8` est celui
qui compte pour l'honnêteté du résultat : le niveau contextuel est ÉTEINT par défaut
(`ctx_shrink = -1`), donc sans lui un gain de `mcps` serait attribuable à « on a allumé le
second niveau » aussi bien qu'à « on l'a conditionné par le chemin ». Juges : ≥2 et ≥3
d'abord — la leçon de (i) est qu'une conversion mono-graine ne vaut rien.

**(k) `--mcps` : RÉFUTÉ DEUX FOIS — et le recadrage qui compte est que MCPS n'a jamais été
implémenté.** A/B quatre bras × deux graines, étalon A but seul, 300 s, binaire PGO du jour
(`tools/s14_masse_ab.ps1`) :

| graine | témoin | `ctx8` (ancien cond., ALLUMÉ) | `mcps6` | `mcps12` |
|---|---|---|---|---|
| 888 — ≥3 | 45 998 | **204 919** | 98 879 | 132 102 |
| 1234 — ≥3 | 100 429 | **150 635** | **0** | 138 119 |

*(1) Réfuté en recherche* : 0 comparaison gagnée sur 4 contre `ctx8`, et un **effondrement
total** à k = 6 sur la graine 1234 — `best 0/4`, **99 % des tirages morts au changement de
tour**, ≥2 = 0 alors que ≥1 = 757 720, et **27 lignes distinctes retenues sur 8 751 offertes**.
C'est un verrouillage de bassin : le conditionnement donne à chaque branche ses propres poids,
chaque case reçoit peu de mises à jour concurrentes, le softmax y durcit, la politique se fige.

*(2) Réfuté en représentation*, par la courbe d'accord (`tools/s14_accord_mcps.ps1`,
déterministe, quelques secondes) — palier à 256 passes, α = 1,0 :

| conditionnement | niveau éteint | ACTIF | SIGNATURE (mémorisation pure) |
|---|---|---|---|
| ancien (posées, main) | 52 | **59** | 64 |
| chemin k = 6 | 52 | 56 | 61 |
| chemin k = 12 | 52 | **59** | 62 |

Le chemin ne monte pas le palier. Et la lecture qui vaut au-delà de ce bras : le plafond de
MÉMORISATION PURE du point de décision est à 64 quand l'ancien conditionnement atteint déjà 59.
**Cinq points de marge sur tout l'axe de la représentation** — §9.14 (« ce qui manque est la
MASSE, pas la représentation ») est re-confirmé, et aucun meilleur descripteur de contexte ne
sauvera quoi que ce soit.

**LE RECADRAGE, demandé en séance et plus important que le verdict.** Le papier a été lu :
MCPS est un **MCTS** — arbre, compteurs de visites, et trois estimateurs qui sont des **MOYENNES
DE RÉCOMPENSE** (`Q`, l'AMAF `Q̃`, et la **permutation `Q̂`** : les parties contenant `a` et tous
les coups de `s` dans n'importe quel ordre), combinés à **poids proportionnels aux effectifs**
— la combinaison convexe qui minimise la variance, et qui SUPPRIME l'hyperparamètre de biais de
GRAVE. Machinerie : un bitset par code de coup sur une fenêtre glissante de W = 10 000 parties,
`Q̂` par `popcount` sur l'intersection.
Notre `EffectiveWeight` combine deux **logits** mis à jour par le **gradient NRPA de la seule
meilleure séquence**. Ni arbre, ni compteur de visites, ni moyenne de récompense, ni bitset :
une combinaison convexe de logits n'est pas une combinaison convexe de taux de victoire. **Le
commentaire de `search.h` qui invoque MCPS est trompeur** ; `--mcps` n'a fait qu'ajouter un
conditionnement au mauvais objet, et son mode d'échec est exactement celui que la moitié
manquante prédit (le gradient pousse +α sans le contrepoids d'une moyenne sur toutes les
parties).

**Ce que la session a fait de travers, et qui est la vraie leçon.** Deux fois — sur les options
en session 12, sur MCPS ici — un mécanisme a été implémenté avec NOTRE critère à la place de
celui du papier, a perdu, et le réflexe a été de se rabattre sur autre chose. En session 12 le
retour au papier avait RENVERSÉ le verdict. La règle qui en sort : **quand une implémentation
s'écarte sciemment du papier et perd, le premier correctif est l'écart, pas le mécanisme.**
La suite est écrite dans `docs/next-session-prompt.md` : implémenter `Q̂` pour de vrai —
bitsets, fenêtre glissante, poids proportionnels aux effectifs — avec l'avertissement que la
courbe d'accord ne peut PAS le juger (elle mesure la reproduction d'un corpus qui ne contient
que des bonnes lignes ; `Q̂` tire son signal des échecs).

**Le vrai résultat de cet A/B est son bras de contrôle.** `ctx8` — la politique à deux niveaux,
avec son ancien descripteur, simplement ALLUMÉE — bat le témoin sur les deux graines (×4,5 et
×1,5 sur ≥3). Ce niveau est **éteint par défaut depuis la session 7** et n'avait jamais été
mesuré dans ce régime. Sans ce bras, le gain de `mcps12` sur le témoin aurait été attribué au
conditionnement par le chemin. Réserve : ×1,5 est dans la bande de bruit (~2× à graine fixée),
donc deux graines de même signe sont un indice fort, pas un fait.

### 9.22 Session 15 : `Q̂` implémentée pour de vrai — et la sonde répond seule à la question de l'opérateur

Mission fixée en fin de session 14 : **« nos implémentations sont défaillantes ; plutôt que de
fallback sur des solutions bricolées non optimales, il faut corriger. »** La session 14 avait
listé six mécanismes à moitié câblés et s'était fait prendre deux fois à mesurer *autour* d'eux.
Chantier principal nommé d'avance : la **statistique de permutation `Q̂`** de MCPS
(arXiv:2510.06381), dont §9.21 (k) avait établi que le projet la **citait sans l'implémenter**.

**(a) LE RECADRAGE, écrit dans le code avant toute mesure.** Le papier a été relu en séance et
les commentaires qui invoquaient MCPS dans `search.h` sont corrigés :

- le bloc « politique à DEUX NIVEAUX » ne dit plus « inspiré de MCPS ». Il porte désormais un
  avertissement de paternité : MCPS est un **MCTS** (arbre, compteurs de visites, trois
  **moyennes de récompense** combinées à **poids proportionnels aux effectifs**) ; `EffectiveWeight`
  combine deux **logits** mis à jour par le gradient NRPA de la **seule meilleure séquence**. Ce
  que le bloc emprunte au papier est l'**idée** de combiner des estimateurs pondérés par leur
  évidence — et sa retenue `s = n/(n+k)` **réintroduit** l'hyperparamètre de biais que la
  dérivation par variance minimale de MCPS **supprime**. C'est l'inverse du papier, pas son
  application.
- `--mcps` est **marqué RÉFUTÉ** dans `search.h`, dans `--help` et **à l'exécution** : le run
  imprime le verdict de 9.21 (k) et le diagnostic (« conditionnement de MCPS greffé sur un
  LOGIT »). Il reste utilisable — *un mécanisme réfuté qu'on ne peut plus rejouer n'est plus
  réfutable* — mais aucune mesure ne peut plus en sortir en croyant mesurer MCPS. `--qhat` et
  `--mcps` refusent de se composer, et le run le dit au lieu de mélanger deux mécanismes
  incompatibles.

**(b) `--qhat <k>` : ce qu'on prend du papier, ce qu'on n'en prend pas, et pourquoi.** Un
**bandit de tête** sur les `k` premières décisions enregistrées : le coup y est choisi par
`argmax val`, au-delà NRPA échantillonne comme avant, **à l'octet près**.

    val(a) = (n·Q(s,a) + n̂·Q̂(s_r,a)) / (n + n̂)

- **`Q̂`, oui** — la fenêtre à bitsets est transportable telle quelle dans une architecture SANS
  arbre : elle ne dépend que des tirages. Un bitset par code de coup sur les W derniers tirages,
  le tableau parallèle des récompenses, `Q̂` par `popcount` sur l'intersection.
- **`Q`, oui, sur un arbre minuscule** — `k` plis seulement. Le haut de l'arbre est étroit
  (mesuré §9.21 (j) : 207 cases à k = 6) et c'est là qu'une ouverture décide de la viabilité de
  toute la ligne. Clé de nœud : la **somme mélangée** des coups joués — commutative, donc deux
  ordres qui mènent au même multiensemble partagent leurs statistiques. C'est à la fois le
  principe directeur du projet (recherche sur le GRAPHE d'états) et la lecture de MCPS, dont les
  permutations disent exactement cela.
- **`Q̃` (l'AMAF de GRAVE), NON — et c'est une raison de fond, pas de la paresse.** À la racine
  les deux estimateurs **coïncident identiquement** : `Q̃(root,a)` = moyenne des parties passant
  par la racine qui contiennent `a` = moyenne de **toutes** les parties contenant `a` =
  `Q̂(root,a)`, la condition du chemin étant vide. Notre arbre est profond de six plis et sa
  référence de permutation est la racine ou l'un de ses tout premiers descendants : ajouter `Q̃`
  compterait deux fois les mêmes tirages, ce que la dérivation par variance minimale — qui
  suppose les estimateurs **indépendants** — ne pardonne pas. On garde la paire `(Q, Q̂)` et la
  **même** formule de poids. **Aucun hyperparamètre de biais n'est réintroduit.**

Quatre points de conception que le code imposait :

1. **LA RÉCOMPENSE, choisie explicitement AVANT de coder.** Le papier travaille sur des taux de
   victoire dans [0,1] ; notre score de tirage est `matériel×1000 + nouveauté`, non borné et
   incomparable d'un run à l'autre. Retenu : `r = (meilleur matériel atteint le long du tirage) /
   (matériel du board cible)`, plafonné à 1, et **exactement 1** au but. Trois propriétés voulues
   — bornée ; **comparable entre runs**, le dénominateur étant une constante du PROBLÈME et non
   le meilleur score courant (normaliser par lui ferait bouger l'échelle des entrées **déjà dans
   la fenêtre**, et `Q̂` comparerait des récompenses incomparables) ; alignée sur l'objectif que
   NRPA optimise déjà. **La nouveauté en est exclue** : c'est un départage de gradient, pas une
   mesure de réussite, et elle n'est pas bornée.
2. **LES DÉCISIONS DU BANDIT SONT EXCLUES DU GRADIENT NRPA** (`PolicyStep::bandit`, sauté par
   `AdaptRun`). Le gradient de NRPA n'est défini que pour un coup **tiré du softmax** ;
   l'appliquer à un coup choisi par argmax le pousserait `+α` à chaque tirage **sans le moindre
   contrepoids** — le mode d'échec exact qui a tué `--mcps`. Le bandit a sa propre mémoire (des
   moyennes sur TOUS les tirages) ; c'est elle qui joue le rôle du gradient sur ces décisions.
3. **LE VERSEMENT PASSE PAR UN GARDE RAII.** `PolicyRollout` sort par une douzaine de `return`
   — impasse, terminal, contrainte violée, garde, borne brûlées, budget — et **ce sont justement
   les tirages MORTS dont `Q̂` tire son signal**. Un versement écrit à la main à chaque sortie en
   aurait oublié un, en silence.
4. **LA RACINE N'EST JAMAIS GELÉE.** Le papier gèle les statistiques de permutation d'un nœud à
   ρ visites et **exclut explicitement la racine** — et pour cause : c'est le seul nœud dont la
   condition est vide, donc le seul dont la statistique se maintient **incrémentalement** en
   O(coups du tirage) au lieu d'un balayage. C'est aussi celui qui porte tout le diagnostic. Les
   nœuds non racine gèlent à ρ et deviennent la référence de leur sous-arbre ; leurs `(n̂, Q̂)`
   sont calculés **à la demande et mis en cache** — même sémantique que le calcul en bloc du
   papier (une valeur par (nœud, code), figée dès le premier usage) pour une mémoire
   proportionnelle aux coups réellement proposés et non à tout le vocabulaire.

**Coût mesuré** (étalon A but seul, 16 workers, W = 4096) : **10 à 11 Mo au total**, 118 à 123
codes en fenêtre, 48 à 51 nœuds d'arbre au worker le plus chargé à k = 6. Deux ordres de
grandeur sous ce que la troncature devait borner — le cadran `--qhat-window` a de la marge, et
la mémoire est imprimée au bilan pour qu'il ne se règle jamais à l'aveugle.

**(c) LA SONDE RÉPOND SEULE À LA QUESTION DE L'OPÉRATEUR — et c'est le résultat de la session.**
Le chantier exigeait une sonde AVANT tout A/B, parce que la courbe d'accord du corpus est
**aveugle à `Q̂`** (elle mesure la reproduction d'un corpus qui ne contient QUE des bonnes lignes,
alors que `Q̂` tire son signal des échecs). Elle imprime deux tables ; c'est la seconde qui
compte — `Q̂({}, a)` pour **tout** coup `a`, la récompense moyenne des lignes qui l'ont joué,
n'importe où et dans n'importe quel ordre.

Relevé en fin d'un run nu de 120 s, graine 888 (`s15q_qhat6nu_888.log`).

**LIRE `n̂` CORRECTEMENT, c'est le premier piège de cette table.** Le run a fait 833 000 tirages,
mais la fenêtre est GLISSANTE et vaut `W = 4096` **par worker** : `Q̂` n'est calculée que sur les
4 096 derniers tirages de chacun, tout le reste a été évincé. Les `n̂` ci-dessous sont des sommes
sur seize workers, donc plafonnées à ~65 000. « `n̂ = 34 966` » signifie « présent dans 34 966 des
~65 000 tirages **encore en fenêtre** », pas dans 34 966 des 833 000. Confondre les deux ferait
lire ce tableau comme le résultat d'un budget quinze fois plus grand qu'il ne l'est.

| coup, condition `s = {}` | n̂ | Q̂ | indicé ? |
|---|---|---|---|
| **Lunalight Gold Leo** | 34 966 | **0,066** | **non** |
| Fire Formation - Tenki (activer) | 11 671 | 0,053 | non |
| Lunalight Tiger | 8 372 | 0,012 | oui |
| Lunalight Kaleido Chick | 8 186 | 0,002 | oui |
| Lunalight Yellow Marten | 4 096 | 0,002 | non |
| Lunalight Gold Leo (autre clé) | 1 123 | 0,002 | non |

**Le fait, en une phrase : `Q̂` classe Lunalight Gold Leo PREMIÈRE sur les 24 coups d'effectif
suffisant, cinq fois au-dessus de Lunalight Tiger et trente-trois fois au-dessus de Kaleido
Chick — et Gold Leo est la seule de ces cibles à ne recevoir AUCUN `--hint`.** Les deux cibles
mal classées, elles, sont dans la liste d'indices et partent donc avec un biais
d'échantillonnage de +2,0. C'est exactement le diagnostic posé par l'opérateur en fin de session
14 — *si Tenki cherche autre chose que Gold Leo la ligne meurt en ~20 décisions ; le solveur ne
devrait pas avoir besoin d'indice et devrait le trouver seul et vite* — et il y répond **sur le
« seul », sans indice, à partir des échecs**. L'information existait (le tirage se termine, c'est
observable) ; il manquait un endroit où la mettre.

**CE QUE CETTE MESURE NE DIT PAS, ET L'OBJECTION QUI L'A FAIT DIRE (séance).** Elle ne dit rien
du « **et vite** ». Le relevé est pris EN FIN de run : il établit que le classement est correct à
ce moment-là, pas à quel budget il l'est devenu. La première rédaction de ce paragraphe annonçait
« en quelques centaines de milliers de tirages » — **c'était faux par confusion** entre le nombre
de tirages que le run a effectués et le coût de la découverte, qui n'a pas été mesuré. Le second
chiffre est d'ailleurs borné par la fenêtre et non par le run : au plus 4 096 tirages par worker
comptent, quel que soit le budget.
**Le coût de la découverte est une COURBE, elle est bon marché (quelques runs de 10 à 40 s lus
sur la sonde), et elle est le premier travail de la session suivante.** Son point de comparaison
est déjà connu et il est brutal : sans `Q̂`, le solveur ne concentre **jamais**, à aucun budget —
c'est le diagnostic de 9.21 (j), un poids par code de coup aveugle à l'état et un sous-arbre mort
re-parcouru des centaines de milliers de fois. Si la courbe donne quelques milliers de tirages,
le mécanisme tient sa promesse ; si elle donne des dizaines de milliers, il la tient faiblement
et le chantier suivant est celui que 9.21 (j) nommait déjà — une vraie **mémoire d'impasse** dans
les tirages, pas une moyenne.

**Ce que la sonde a dû devenir pour dire cela, et la leçon d'instrument.** Première version : la
table du nœud racine, c'est-à-dire les coups que le bandit **choisit** à la première décision.
Elle rendait **quatre lignes** — poser Tenki, l'activer, aller en Battle Phase, finir le tour —
et ne disait rien : la cible de Tenki se décide au **deuxième** prompt. La statistique qui répond
n'est pas « ce que le bandit décide à la racine » mais **`Q̂({}, ·)` sur tout le vocabulaire**,
qui existe pour n'importe quel coup dès qu'il apparaît dans un tirage. Les deux tables sont
désormais imprimées, et c'est la seconde qui est l'instrument.

**(d) LES CINQ AUTRES DÉFAILLANCES : quatre réparées, une réfutée par son propre criblage.**

| mécanisme | ce qui était cassé | état après la s15 |
|---|---|---|
| `novelty_rollout_cut` | câblé dans `Rollout()` (tirage glouton, qui ne fait plus rien), **jamais** dans `PolicyRollout` — où le verdict de nouveauté était pourtant CALCULÉ à chaque décision (4 requêtes core, ~60-80 sondes) et jeté après un simple départage | câblé + `--novelty-rollout-cut`. **RÉFUTÉ au criblage** (ci-dessous) |
| `allow_phase_change` | lu au seul prompt *idle* ; au prompt *bataille* les deux sorties de phase étaient émises **inconditionnellement** — le drapeau ne fermait qu'une moitié de la porte. Aucun drapeau CLI | les deux prompts couverts + `--no-phase-change`, avec garde-fou : un prompt qui n'offrirait plus rien garde ses sorties |
| `canonical_zones` | déclaré, documenté, allumé nulle part (9.20 (e)) | `--canonical-zones` (non jugé cette session) |
| archive Go-Explore | clé de tri `résolutions<<44 \| overlap<<36 \| ~décisions` : les résolutions SATURENT en but seul, l'archive range des **fins de ligne**, le finisseur rend ÉPUISÉ en 0-13 expansions (9.21 (f)) | **quota par niveau de progrès** (`--archive-spread`) : l'archive redevient une COUVERTURE et non un palmarès |
| `--finisher-options` | écrit, compilant, **jamais jugé** ; sans objet sur étalon A (rien à compresser dans 3 expansions) | **non traité** — son juge est l'étalon B contraint, il reste en tête du chantier suivant |

**Le criblage a fait son travail, et il a éliminé.** 90 s, graine 888, lecture sur le critère
INTERNE de chaque mécanisme (`tools/s15_ab.ps1`) :

| bras | critère interne du mécanisme | témoin | bras |
|---|---|---|---|
| `--no-phase-change` | tirages morts au changement de tour | 82 % | **67 %** |
| `--novelty-rollout-cut` | tirages atteignant ≥1 résolution | 206 093 | **9** |
| `--archive-spread` | expansions par racine du finisseur (médiane) | 15 | **88** |

`--novelty-rollout-cut` est **RÉFUTÉ sur son propre critère** : le mécanisme ne coupe pas les
branches muettes, il **détruit les tirages** — ≥1 résolution passe de 206 093 à 9, `best` de 1/4
à 0/4, et seulement 2 % des tirages atteignent le changement de tour parce qu'ils meurent bien
avant. C'est **exactement** le mode de défaillance documenté en §9.3 pour Rollout-IW sans arbre
(la table de nouveauté est partagée entre tirages ; un tirage qui re-parcourt le même début meurt
à `patience` décisions avant d'avoir pu dévier), mesuré à l'époque sur les tirages GLOUTONS — il
vaut à l'identique sur les tirages sous politique. Le drapeau reste implémenté et opt-in ; le
commentaire de `SearchConfig` disait déjà « MESURE et désactivé par défaut », c'est maintenant
vrai des deux passes. **Coût de ce verdict : 90 secondes.** C'est ce que le criblage sur critère
interne est censé rendre, et c'est la deuxième fois qu'il rend un verdict pour le prix d'un
pilote (la première était `--options-len`, 9.21 (i)).

`--archive-spread` survit au criblage sur son critère : l'archive contient désormais des racines
de niveaux r1 et r2 à côté des r2/r3, la médiane d'expansions par racine passe de 15 à 88
(×5,9) et le total de 8 002 à 10 296. Elle range de nouveau ce qui a de la ligne devant lui.

**(e) L'A/B DE MASSE, ET CE QU'IL A VRAIMENT MESURÉ.** Quatre bras (témoin `--options-online 60`,
`--qhat 6`, `--no-phase-change`, `--archive-spread`) sur trois tirages, 300 s, binaire PGO du
jour. Onze runs terminés sur douze :

| bras | 888 | 1234 | 4242 | lignes écrites |
|---|---|---|---|---|
| témoin | 2/4 | 2/4 | 2/4 | **0** |
| `--qhat 6` | 2/4 | 2/4 | 2/4 | **0** |
| `--no-phase-change` | 2/4 | 2/4 | — | **0** |
| `--archive-spread` | 2/4 | 2/4 | — | **0** |

**Aucun bras ne dépasse 2/4 et aucun n'écrit de solution.** Deux lectures, la seconde étant la
seule qui compte.

*Lecture prématurée, écrite puis RETIRÉE dans la même séance.* Ces onze runs donnant tous 2/4,
il a été conclu que « la dispersion a disparu du juge ». **Faux** : un douzième run de la MÊME
commande sur la MÊME graine (`s15gf_temoin_888`, après re-PGO) rend **1/4**. Onze accords ne font
pas une constante, et la dispersion documentée depuis la session 4 — l'ordre des échanges entre
workers dépend de l'ordonnancement — n'a pas disparu. Elle était seulement passée sous le seuil
de résolution d'un juge qui ne compte que jusqu'à 4.

*Lecture qui compte* : **ce plafond n'est pas une propriété de la recherche, c'est un but que le
jeu interdit de satisfaire.** Voir (f).

**(f) LE BUT ÉTAIT INSATISFIABLE — sept sessions de « but seul » ont mesuré contre l'impossible.**
Question de l'opérateur : *« ce qui m'intéresse, c'est de savoir si en but seul on peut converger
vers la solution dans un temps raisonnable (quelques minutes) »*. La réponse a demandé de lire
le code du but au lieu de dépenser des runs, et elle est **non, pour une raison qui n'est pas la
recherche**. Trois faits, chacun vérifié dans le source ou dans la base de cartes :

1. **La cible POSÉE a une zone S/T VIDE.** `--target` construit le board *table rase* — « le
   board de la référence n'entre pas » — et pousse chaque carte en MZONE. La cible ne contient
   donc aucune magie ni piège.
2. **Le but est l'ÉGALITÉ EXACTE.** `GoalCheck` teste `here == target`, et `BoardKey::operator==`
   compare l'égalité des entrées, MZONE **et SZONE confondues**. Une zone S/T vide dans la cible
   est donc une **exigence d'absence** : le terrain final doit être nu.
3. **La main ne peut pas laisser la zone S/T vide.** Elle est faite de trois *Fire Formation -
   Tenki*, `type = 0x20002` — une magie **CONTINUE**, qui reste sur le terrain dès qu'on
   l'active, et qui l'occupe aussi si on la pose. Et `--resolve 2344618` exige la résolution de
   *Lunalight Masquerade*, qui siège également en zone S/T.

**Aucune ligne partant de cette main ne pouvait satisfaire ce but.** Le dossier concorde à la
ligne près : **zéro solution écrite en but seul depuis la session 8**, et un plafond que tous les
rapports décrivent par la même phrase — « tous les codes y sont : le but ne diffère que par le
DÉTAIL ». Le détail était les cartes que le jeu oblige à laisser sur le terrain.

**ET IL Y AVAIT UNE SECONDE CAUSE, INDÉPENDANTE.** `EntryOf` mélange les **matériaux Xyz** et les
compteurs dans l'entrée d'une carte, y compris sous `goal_view`. Une cible posée est un
`QueriedCard` nu — code et position, **aucun matériau**. Or *Number 41: Bagooska* est un Xyz qui
porte toujours ses matériaux sur le terrain. **L'entrée « Bagooska posée » ne pouvait donc égaler
aucune Bagooska réelle**, et Bagooska figure dans TOUTES les commandes de l'étalon A depuis la
session 7ter. Relâcher la zone S/T seule n'aurait pas suffi.

**L'INSTRUMENT QUI AURAIT DÛ LE MONTRER IMPRIMAIT LE MAUVAIS BOARD.** Le bloc de diagnostic
« tous les codes y sont » affiche `ref.target_self` — le board du **gabarit** — sous l'étiquette
« cible : ». Sous `--no-ref` ce n'est pas la cible. L'opérateur lisant ce bloc y voyait un board
*avec* ses Tenki en zone S/T et ne pouvait pas soupçonner que la cible réelle exigeait le
contraire. C'est la famille de piège que ce document catalogue depuis la session 9 — *un
mécanisme qu'on ne mesure pas là où il agit* — appliquée cette fois au but lui-même.

**LE CORRECTIF, en trois pièces.**
- **La sémantique du but dépend désormais de l'ORIGINE de la cible.** Cible **capturée** sur une
  vraie ligne (étalon B) : égalité exacte, elle porte ses propres S/T et ses propres matériaux.
  Cible **POSÉE** (`--target`) : **inclusion, par défaut**. Poser une cible veut dire « je veux
  ces cartes », pas « ces cartes et le terrain nu autour ». Arbitrage du joueur, versé en séance :
  *« les S/T sont du bonus, pour Lunalight seuls les monstres comptent in fine »*.
  `--target-exact` restaure l'ancien comportement pour l'A/B.
- **L'inclusion porte sur des entrées RELÂCHÉES** — (zone, code, face), sans matériaux ni
  compteurs (`BoardKey::loose`, `LooseEntryOf`) — sans quoi la seconde cause survivrait.
- **Le bloc de diagnostic imprime la cible réellement posée**, et dit explicitement qu'une zone
  S/T absente est une exigence d'absence.

**Santé stricte après correctif : 20 lignes de diff, durées et nom d'outdir seulement.** L'étalon
même-deck, dont la cible est CAPTURÉE, garde l'égalité exacte et ne bouge pas d'un digest.

**ET LE CORRECTIF NE SUFFIT PAS — c'est le résultat, et il faut le dire sans le tourner.** A/B à
UN SEUL FACTEUR contre les onze runs ci-dessus : commande témoin identique, graine 888, 300 s,
binaire re-PGO, seule la sémantique du but change (`s15gf_temoin_888`). Résultat : **1 des 4
cartes cibles, aucune solution**. Le board n'est tout simplement pas assemblé.

La conclusion correcte a donc deux termes, et confondre les deux serait la faute :
- **Le but était bien insatisfiable**, c'est prouvé par lecture du source et de la base de
  cartes, pas par un run. Tant qu'il l'était, *aucune ligne, même parfaite, n'aurait pu être
  enregistrée comme solution*. Ce plafond-là est réel et il est levé.
- **Il MASQUAIT le mur, il ne le CAUSAIT pas.** Avec un but désormais atteignable, la recherche
  n'assemble toujours pas 3 Liger Dancer + Bagooska en 300 s : au mieux 2 des 4 codes, parfois 1.
  Le mur de l'assemblage est intact et n'a jamais été mesuré proprement — il l'est enfin.

Autrement dit : sept sessions ont mesuré contre un but impossible, et la première mesure contre
un but possible dit que le problème d'assemblage reste entier. Les deux faits tiennent ensemble ;
seul le second est désormais un objet de recherche légitime.

**CE QUE CELA RETIRE DU DOSSIER, ET QU'IL FAUT DIRE SANS ATTÉNUER.** Toute conclusion de la forme
« le but seul plafonne à k/4 » des sessions 8 à 15 portait sur un but que le jeu interdit — y
compris le « 2/4 sur 3 graines sur 3 » de 9.21 (d) et le « 2/4 à 1 200 s » de 9.21 (f). Les
verdicts **comparatifs** restent lisibles, tous les bras ayant subi la même contrainte : la
supériorité de `--options-online` sur le run nu, la réfutation de `--options-ctx`, de
`--options-len`, de `--mcps`, le signe de `--nrpa-lr`. Ce qui tombe, ce sont les verdicts
**absolus** sur la capacité du mode but seul — ils n'ont jamais été mesurés.

**(g) RECADRAGE DEMANDÉ EN SÉANCE — LA GRAINE EST UN TIRAGE, PAS UN CADRAN, ET LA DISPERSION EST
UN DÉFAUT DU SOLVEUR.** Objection de l'opérateur, posée pendant l'A/B : *« cette histoire de
graine semble être un non-sens ; quelle que soit la seed on devrait avoir les mêmes résultats,
un utilisateur ne va pas lancer plusieurs runs en changeant la seed. »* Elle est juste, et elle
l'est davantage que formulée. Trois choses, dans l'ordre.

1. **Ce que les graines multiples mesurent.** Elles ne simulent pas un usage. L'utilisateur lance
   **un** run ; ce run tombe quelque part dans une distribution d'issues. Tirer trois graines
   estime **la probabilité que son unique run réussisse** — c'est-à-dire exactement la question
   qui l'intéresse. Il n'existe pas d'autre façon de l'estimer que de tirer plusieurs fois. Le
   juge d'un mécanisme n'est donc jamais « 2/4 sur la graine 888 » mais **la fraction de tirages
   qui convertissent** (9.21 (d) : 3 sur 3 contre 0 sur 3 — c'est cette forme-là qui vaut).
2. **Fixer la graine ne rend même pas le run reproductible.** 9.21 (i) l'a mesuré sans le
   chercher : deux runs de la commande IDENTIQUE, même graine, même binaire, rendent 2/4 contre
   1/4 et ≥3 à 115 933 contre 91 167. La cause est connue depuis la session 4 — seize workers
   échangent leur meilleure séquence de façon asynchrone et l'ordre des échanges dépend de
   l'ordonnancement du système. **« Même graine ⇒ même résultat » est déjà faux, et la graine n'y
   est pour rien.**
3. **La conséquence, qui est un objectif de produit et non de méthode.** La dispersion n'est pas
   une propriété de l'instrument à contourner : c'est **un défaut du solveur à supprimer**. La
   cible n'est pas « ça marche sur la bonne graine » mais « ça converge quel que soit le tirage ».
   Sa source est identifiée et donc attaquable (échanges asynchrones entre workers) ; le
   déterminisme strict n'est PAS le remède recherché — un run déterministe qui échoue vaut moins
   qu'un run dispersé qui réussit huit fois sur dix. Ce qu'il faut porter à 1, c'est la fraction
   de tirages qui convertissent, et c'est désormais la façon dont ce document rapporte tout A/B
   de conversion.


**(h) LE MUR, ENFIN NOMMÉ : CE N'EST PAS LA TAILLE DU BOARD, CE SONT LES SOUS-BUTS IDENTIQUES QUI
SE DISPUTENT LES RESSOURCES.** Quatre questions posées en séance par l'opérateur — *quel est le
mur ? que dit la littérature ? pourquoi bloque-t-on ? retrouve-t-on le board handrip en but seul
sans référence ?* — et c'est la quatrième qui a répondu aux trois autres.

*La mesure qui retourne le diagnostic* (`s15_B_noplan`, étalon B, `--start` + `--no-plan`, aucune
contrainte, 300 s, binaire PGO) :

| cas | cible | sans répertoire, 300 s | tirages |
|---|---|---|---|
| **étalon B** — board handrip synchron | 8 cartes | **7/8** (208 décisions) | 134 686 |
| **étalon A** — Lunalight | 4 cartes | **1-2/4** | 1 067 565 |

**Le board à HUIT cartes est presque reconstruit sans la moindre aide ; celui à QUATRE résiste.**
La difficulté n'est donc ni la taille du but, ni la profondeur de la ligne, ni le budget. La seule
différence structurelle entre les deux cibles est que l'étalon A demande **trois exemplaires de la
MÊME carte**, dont la fabrication consomme la même chaîne de ressources (Kaleido Chick → Leo
Dancer au cimetière → Fusion), et que le deck n'en a **que trois copies** : aucune marge.

*Ce que le mur n'est PAS.* Ce n'est pas un gradient plat : `CommonCodes` est une intersection de
**multiensembles** (les deux curseurs avancent à l'égalité), donc un deuxième Liger Dancer FAIT
monter le score. L'hypothèse « la politique n'a aucune raison d'en faire un second » est
**réfutée par lecture du code**, avant d'avoir coûté un run.

*Ce que le mur EST, et la littérature qui le nomme.* Les trois sous-buts **interfèrent par
consommation** : atteindre le premier détruit ce qu'il faut pour le deuxième, donc une ligne qui
prépare deux Ligers doit longtemps PARAÎTRE PIRE qu'une ligne qui en pose un tout de suite.
- **Bonet & Geffner, arXiv:2311.05490** — IW tourne en temps exponentiel en la LARGEUR, et la
  largeur n'est bornée que pour des buts **atomiques** ; d'où leur notion de **largeur
  sérialisée**, bornée dans beaucoup de domaines qui n'ont pas de largeur bornée. Notre but est
  conjonctif avec trois atomes identiques.
- **Drexler, Seipp & Geffner, arXiv:2105.04250** — « *SIW échoue quand le but n'est pas facilement
  sérialisable, ou quand l'un des sous-problèmes a une largeur élevée* ». Notre solveur sérialise
  DÉJÀ (`novelty_serialize` rouvre la table de nouveauté à chaque carte cible posée) : nous sommes
  donc dans le second cas, un sous-problème de largeur élevée.
- **Ståhlberg & Geffner, arXiv:2512.19355** (déjà au dossier §9.15) — domaine *Delivery* :
  « tous les colis doivent être livrés au même endroit […] **les modèles apprennent à en livrer un
  seul** ». Notre forme, mot pour mot, et l'avertissement était écrit **avant** cette session.
- **CraftWorld** (arXiv:2605.30664, au dossier) — matériaux consommés, impasse par consommation.

*LA MESURE QUI MANQUE, et elle ne coûte qu'un compteur* : un histogramme des invocations de Liger
Dancer par tirage (combien atteignent 1, 2, 3), et le compte des tirages qui posent le premier
Liger **tout en conservant de quoi en faire un second**. Il dira si le deuxième est **jamais
tenté** ou **toujours perdu** — deux pannes opposées qui appellent deux correctifs opposés, et que
`best_overlap` ne sépare pas. À faire AVANT tout mécanisme.

*Réserve d'appariement, dite franchement* : le run étalon B garde la cible CAPTURÉE (seule façon
de connaître le board) et tourne sans les `--resolve` / `--summon-min` / `--hint` que porte
l'étalon A. L'appariement n'est donc pas parfait — mais l'écart 7/8 contre 1/4 dépasse de loin ce
que ces différences peuvent expliquer.


**(i) L'ÉTALON B SOUS GARDE PERMANENTE : les contraintes AIDENT, et le blocage est une TROISIÈME
compétition de ressources.** Demande de l'opérateur : reprendre l'étalon B sans répertoire mais
avec toutes les contraintes ET **sans `--guard-off`** — la garde anti-Nibiru tenue jusqu'au bout.

| bras (`--start` + `--no-plan`, 300 s, sans répertoire) | board atteint |
|---|---|
| aucune contrainte (`s15_B_noplan`) | 7/8 |
| **contraintes complètes + garde permanente** (`s15_B_dur`) | **8/8** |

**Les contraintes ne font pas qu'élaguer : elles GUIDENT.** `--resolve`, `--summon-min` et la garde
portent le gradient vers les pièces réelles du combo, et le bras le plus contraint monte PLUS
HAUT que le bras libre. Le même effet est mesuré sur l'étalon A, où retirer `--resolve` fait
tomber les invocations de Liger Dancer à **zéro** (`>=1 0 <-- JAMAIS`). C'est contre-intuitif et
c'est reproductible sur deux étalons.

**Pourquoi toujours pas de solution : `meilleure crête AUX résolutions complètes : 6/8`.** Les
lignes qui accomplissent les trois résolutions exigées plafonnent à **6/8** cartes ; celles qui
atteignent **8/8** n'ont pas rippé. **Jamais les deux ensemble.** Les 24 racines du finisseur sont
toutes à `6/8 r3` et rendent `ÉPUISÉ` en 0 à 32 expansions. Le projet avait bâti l'instrument
exact pour cette question (`best_overlap_ripped`) avec le commentaire prémonitoire — « *si elles
plafonnent loin du board pendant que les lignes muettes font 8/8, les deux buts sont probablement
incompatibles en RESSOURCES* » — et le chiffre le confirme.

**TROIS FOIS LE MÊME MOTIF, sur trois cibles sans rapport entre elles :**

| cas | sous-buts en compétition | plafond mesuré |
|---|---|---|
| étalon A | 3× Liger Dancer (même chaîne de matériaux, 3 copies au deck) | 1 Liger |
| étalon B nu | board complet | 7/8 — la dernière Synchro n'est plus possible |
| étalon B contraint | board **et** handrip | 8/8 sans rips, **ou** 6/8 avec |

Ce n'est donc pas une particularité de deck : c'est **le** mode d'échec du solveur. Atteindre un
sous-but consomme ce dont le suivant a besoin, et rien dans la recherche ne le voit venir — les
états quasi-atteints sont MORTS et le finisseur le découvre en quelques expansions, run après run.

**LA LIGNE QUI SATISFAIT LA GARDE PERMANENTE EXISTE — fournie en séance par l'opérateur** :
`D:\ProjectIgnis\replay\synchron handrip optimized.yrpX`. Jugée par l'outil : **45 fenêtres
adverses sous menace, 0 découverte**, `0 MSG_RETRY`, 307 décisions, 19 brûlées (pic 23), même
board que l'étalon B (empreinte `89affac93b7a4c52`). La réserve écrite plus haut — « la garde
permanente n'est satisfaite par aucune ligne connue », vraie de `synchron handrip 2`, dont le
rapport dit « la référence VIOLE la garde : 2 fenêtres découvertes sur 47 » — est donc levée :
**la contrainte est satisfiable, et nous en avons maintenant le témoin**. C'est le nouvel étalon
de la discipline complète, et le cas où « le solveur retrouve-t-il seul une ligne que l'humain a
trouvée ? » a enfin un oui de référence.

**INSTRUMENT À CORRIGER, repéré au passage.** Le bloc « tous les codes y sont : le but ne diffère
que par le DÉTAIL » affiche, sur `s15_B_dur`, des écarts **ATK/DEF uniquement** — or le critère de
but IGNORE explicitement la position de combat (`EntryOf(..., goal_view=true)` replie la position
sur recto/verso). Le bloc désigne donc un écart qui n'en est pas un, et reste muet sur le vrai
blocage (les résolutions). Même famille que le « cible : » corrigé en (f) : un diagnostic qui
répond à côté de sa propre question.


### 9.23 Session 16 : la sonde retourne le diagnostic — le SECOND exemplaire n'est pas le problème, le PREMIER l'est

Mission fixée en fin de session 15 : **apprendre un graphe de landmarks généralisés depuis les
plans résolus, boucle de répétition comprise, et le servir comme `h`** (Hanou, Dumančić & de
Weerdt, arXiv:2508.21564). Avec une exigence préalable non négociable (piège 40) : poser **avant**
tout mécanisme le compteur qui sépare deux pannes opposées que `best_overlap` confond — le
deuxième exemplaire *jamais tenté* (le matériau était là : panne d'échantillonnage) du deuxième
*toujours perdu* (la chaîne était consommée : panne de `h`).

La sonde a été posée. Elle a répondu, et **sa réponse n'est ni l'une ni l'autre** : sur les deux
étalons, ce n'est pas le second exemplaire qui manque, c'est le premier.

**(a) `--probe-repeat` : ce qu'elle mesure, et les TROIS défauts d'instrument corrigés avant de la
croire.** Par carte surveillée (`--summon-min` / `--resolve`) elle relève : l'**histogramme des
invocations PAR TIRAGE**, en compte BRUT et par entrée — distinct de `resolve_reached`, qui
plafonne à `min_count` et SOMME les entrées, et c'est ce plafonnement qui rendait le dossier
aveugle à « un Liger contre trois » ; à la **première** invocation, la distance de recettes à un
exemplaire **de plus**, l'exemplaire frais retiré de la disponibilité (sans ce retrait la réponse
serait « 0, il est là », qui n'est pas la question) ; la distance au **reste** de la cible au même
instant ; la décision moyenne de cette première invocation et le nombre de décisions qui la
suivent.

Trois défauts ont été trouvés **sur la première lecture**, chacun capable de produire une
conclusion fausse — et le troisième a été introduit par le correctif du second :

1. **La référence et la mesure portaient sur deux graphes différents.** `d0` était pris au tout
   premier tirage, sur un graphe de recettes quasi vide ; la distance d'arrivée, des centaines de
   secondes plus tard, sur un graphe enrichi par toutes les invocations observées entre-temps. Le
   biais va vers le faux « consommé ». Corrigé : la référence est **re-prise tous les 2 048
   tirages**, la classification conservé/consommé se fait **dans le worker** contre sa référence
   contemporaine, et le nombre de relevés est imprimé.
2. **La sonde ne couvrait que la phase tirages.** C'est exactement le piège d'instrument de
   9.21 (d) — la 2ᵉ Liger de la session 14 avait été trouvée dans les tirages **enracinés** du
   finisseur, invisibles dans la ligne de résumé. Un « jamais » s'y serait lu comme un jamais du
   RUN. Corrigé : **deux tables**, imprimées séparément.
3. **Le garde-fou du plancher confondait deux lectures opposées.** Le graphe de recettes rend 1
   pour un produit dont il ne connaît AUCUNE recette (règle 2) *et* pour un produit qui est à une
   invocation près. `RecipeGraph::Knows` sépare désormais les deux.

**ET UNE RÉSERVE QUI SUBSISTE — l'axe « distance » NE REND AUCUN VERDICT, et le run le dit
lui-même.** Deux causes cumulées. Les cartes surveillées hors board cible n'avaient aucune recette
amorcée (corrigé : `SeedRecipesFromText` les amorce désormais — mais la plupart restent au
plancher, faute de ligne de matériaux). Et là où une recette existe, elle est **amorcée par le
texte**, donc porte la zone JOKER, qui accepte le cimetière — or les matériaux que l'invocation
vient de consommer y sont justement arrivés, et comptent donc encore comme disponibles. Une
distance uniformément égale à 1 est la **signature de ce biais**, pas la preuve d'un matériau
conservé. Le run imprime cette réserve à la place du verdict. C'est un axe à réparer, pas un axe
qui a parlé.

**(b) CE QUE L'HISTOGRAMME DIT — et il retourne le diagnostic du chantier.** Le contrôle est
INTERNE : dans le même run et la même table, deux cartes que l'échantillonnage répète en
permanence, et une qu'il ne répète jamais.

*Étalon A, deck et main NEUFS (`Lunalight Gold Leo` + 3 `Fake Trap`, versés par l'opérateur en
séance), 300 s, graine 888, binaire non-PGO — 930 676 tirages + 439 430 enracinés* :

| carte surveillée | phase tirages : ≥1 / ≥2 / ≥3 | finisseur enraciné : ≥1 / ≥2 |
|---|---|---|
| Lunalight Masquerade | 362 008 / 359 506 / **290 463** | 59 036 / 137 |
| Lunalight Wolf | 350 015 / 145 780 / 8 819 | 7 913 / 4 117 |
| **Lunalight Liger Dancer** | **0 / 0 / 0** | **62 / 0** |

**Le fait, en une phrase : l'échantillonnage fabrique trois Masquerade dans 290 463 tirages et ne
fabrique pas UN SEUL Liger Dancer sur 930 676 — alors que le but en demande trois.** Ce n'est donc
pas « le solveur ne sait pas répéter » : il répète tout le temps, et jamais la bonne carte. Le
finisseur, lui, pose une première Liger 62 fois sur 439 430 tirages enracinés, et **jamais une
seconde**.

*Le même motif sur l'ancien départ* (ancien = 3 `Fire Formation - Tenki`, même deck neuf, 300 s) :
Masquerade 301 552 → 299 586 (**99,3 %**), Wolf 317 657 → 156 379 (**49,2 %**), Liger **0** en
phase tirages et **3 767 → 0** au finisseur. Deux départs sans rapport, le même mur.

Et la colonne qui donne le mécanisme, sur l'ancien départ : après la première Liger il reste
**10,0 décisions** au tirage, contre 89,0 après la première Masquerade et 56,4 après le premier
Wolf, alors que la référence humaine met **~16 décisions par invocation** (9.21 (g)). Là où une
première Liger tombe, **il n'y a plus la place d'en tenter une seconde**.

*Étalon B optimisé, 300 s* : `>=1 22 095, >=2 1 305, >=3 **0**` — aucun tirage n'accomplit les
trois résolutions exigées alors que le board monte à 8/8, et **Trishula ne résout que dans 314
tirages sur 363 551** (0,086 %). Le verrou n'est pas le board, c'est la dernière résolution.

**LA CONSÉQUENCE POUR LE CHANTIER, à écrire sans la tourner.** La question posée — « le second
exemplaire est-il jamais tenté ou toujours perdu ? » — **présupposait un premier**. Il n'y en a
pas. Le chantier suivant n'est donc pas celui que la session 15 avait nommé : c'est *pourquoi
l'échantillonnage n'atteint jamais la première Liger alors qu'il accomplit ses trois activateurs
87 785 fois*.

**LA CIBLE EST FAISABLE — vérifié en séance, et le vérifier était obligatoire.** L'arithmétique
apparente ne fermait pas : Liger exige « Leo Dancer + 3 monstres Lunalight », Leo Dancer exige
« Panther Dancer + 2 Lunalight » — et Panther Dancer est ABSENT du deck, ce que le solveur signale
de lui-même (« 1 recette écartée : elle nomme un matériau absent de ce deck »). L'extra ne porte
que DEUX Leo Dancer pour TROIS Liger. Réponse de l'opérateur, confirmée par le texte des cartes :
**Kaleido Chick se substitue à Leo Dancer sur le terrain** (copie de nom), et surtout
**`Lunalight Wolf` (effet Pendule) invoque une Fusion en bannissant les matériaux depuis le
terrain OU LE CIMETIÈRE**, tandis que **`Lunalight Masquerade` autorise également les matériaux du
cimetière**. Les Leo Dancer envoyés au cimetière redeviennent donc des matériaux. **La cible est
atteignable, et tout échec de l'étalon A est imputable au solveur.**
Corollaire qui éclaire un chiffre du dossier : les trois `--resolve` de l'étalon A ne sont pas des
décorations, ce sont **exactement les activateurs de matériaux depuis le cimetière** qui rendent la
répétition possible — d'où le « retirer `--resolve` fait tomber les invocations de Liger à zéro »
de 9.22 (i), qui n'avait jusqu'ici pas d'explication.

**(c) LE GRAPHE DE LANDMARKS APPRIS (chantier 18) : écrit, imprimé, et il apprend la bonne
chose.** Landmark = **(code, zone, k)**, « k exemplaires de ce code dans cette zone ». Le COMPTE
est ce qui généralise, et la chaîne (code,zone,1) → (code,zone,2) → (code,zone,3) EST la boucle de
répétition du papier. Trois règles, héritées du graphe de recettes :

1. **UN LANDMARK EST UN ACCOMPLISSEMENT, pas un fait.** Seul un compte STRICTEMENT supérieur au
   compte initial est retenu — sans quoi « 3 Tenki en main » serait le premier landmark, satisfait
   à la décision 0, et `h` serait plat à nouveau (piège 42 mot pour mot).
2. **LE GRAPHE NE PRUNE JAMAIS, IL PONDÈRE.** Un landmark non atteint ajoute 1 à `h` ; aucun état
   n'est déclaré mort. Au pire `h` redevient le `h` plat.
3. **L'INTERSECTION, PAS L'UNION.** Un landmark doit apparaître dans TOUS les plans du corpus. Avec
   un seul plan l'intersection EST ce plan, et le run l'imprime en avertissement : ce ne sont pas
   des landmarks généralisés, c'est la trace d'une ligne.

**L'ÉCART AU PAPIER, dit d'avance** (la leçon de 9.21 (k) est qu'un écart tu est un verdict perdu).
Le papier est PDDL : ses landmarks sont des PRÉDICATS extraits d'un modèle d'actions. Nous n'en
avons pas — nos actions sont des scripts Lua. Nos landmarks s'extraient donc des **TRACES** :
chaque plan résolu est rejoué par `LiftPolicyRun` et l'on relève, à chaque décision, le
multiensemble des faits (code canonique, zone normalisée). Ce qu'on perd est la garantie de
nécessité logique ; ce qu'on garde est ce dont `h` a besoin — un ordre et des comptes appris de
plans qui ONT marché.

**LE POINT QUI A ÉVITÉ UN MÉCANISME MUET SUR LA MOITIÉ DU BUT.** Un relevé limité au joueur cible
ne peut pas exprimer un **handrip** : aucun fait de notre moitié de terrain ne devient vrai quand
l'adversaire perd une carte. Le graphe aurait appris à construire le board et serait resté muet sur
exactement la moitié qui manque, **tout en ayant l'air de fonctionner**. `CollectStateFacts` relève
donc aussi les zones de l'ADVERSAIRE, marquées par le bit 0x80. Le résultat le justifie : sur
l'étalon B, le graphe appris depuis deux plans résolus sort

    0.54   Fake Trap                      ADV banni     x3   <-- BOUCLE

— **« trois cartes bannies de la main adverse », landmark COMPTÉ, à l'ordre 0,54 de la ligne.**
C'est le handrip, appris tout seul, sous la forme exacte que le chantier demandait, et c'est le
sous-but que le solveur n'accomplit jamais. Le graphe complet fait **60 landmarks sur 55 faits
distincts, dont 4 boucles de répétition**, et son ordre est lisible (Assault Sonic Warrior main
0,02 → terrain 0,04 → cimetière 0,07 ; Stardust Synchron main 0,13 → terrain 0,15 → cimetière ×2
0,19 → banni 0,78).

Servi comme `h` en deux points, séparés pour qu'un A/B n'en bouge qu'un : `--landmark-w` dans le
**score des tirages** (99 % du travail ; en PROGRÈS et non en distance, puisque le score est un
« plus haut vaut mieux ») et `--landmark-h` dans le **`h` du finisseur** (le point d'entrée que
`recipe_h` occupait sans jamais être consommé). Le coût est rendu payable dans les tirages par un
choix de représentation : on ne relève pas l'état, on relève les **clés de landmark** — quelques
dizaines de cases — et une zone cachée n'est interrogée que si un landmark y vit (`ZoneMask`).

**(d) L'A/B : LE MÉCANISME N'EST PAS DÉMONTRÉ, et le criblage qui semblait le démontrer ne tient
pas au budget long.** Juge : étalon B optimisé — le seul cas où **une solution existe** (9.22 (i)),
donc où un échec est imputable au solveur. Binaire unique, non-PGO, identique pour tous les bras ;
la session n'a pas re-déroulé de pipeline PGO, un A/B apparié n'en ayant pas besoin (conséquence
assumée : les DÉBITS ne sont pas comparables à ceux de la session 15).

*Criblage 90 s, graine 888* — et il donnait une belle réponse en dose :

| bras | ≥1 | ≥2 | ≥3 | crête aux rés. complètes | approche écrite |
|---|---|---|---|---|---|
| témoin | 0 | 0 | **0** | 0/8 | 3/8 |
| `lm0` (appris, poids ZÉRO) | 21 | 0 | 0 | 0/8 | 7/8 |
| `lmw20` | 1 002 | 12 | 0 | 0/8 | 7/8 |
| `lmw60` | 2 539 | 205 | **28** | **5/8** | 8/8 |

*A/B 300 s, graine 888* — et il ne le confirme pas :

| bras | ≥1 | ≥2 | ≥3 | crête | approche | exp/racine (méd.) |
|---|---|---|---|---|---|---|
| témoin | 22 417 | 861 | **0** | 0/8 | 8/8 | 16 |
| `lmw60` | 18 859 | 546 | **0** | 0/8 | 8/8 | 33 |
| `lmx60` (ligne tenue à l'écart) | 20 035 | 1 090 | **14** | **3/8** | 8/8 | 2 |
| `--archive-spread` | 18 082 | 577 | **0** | 0/8 | 8/8 | **164** |

**Lecture honnête, et c'est le verdict de ce chantier : `>=3` est un ÉVÉNEMENT RARE — il vaut 0
dans quatre runs sur six, et il est non nul dans DEUX BRAS DIFFÉRENTS à deux budgets différents
(`lmw60` à 90 s, `lmx60` à 300 s).** Sur une seule graine, cela ne démontre rien : c'est exactement
la configuration contre laquelle 9.21 (i) et 9.22 (g) mettent en garde. Le mécanisme est **écrit,
instrumenté, et NON DÉMONTRÉ**. Aucune ligne n'a été écrite par aucun bras.

Ce qui reste debout de cet A/B, et qui vaut d'être gardé : le bras `lmx60` n'apprend QUE sur
`synchron handrip 2` — une **ligne tenue à l'écart** de celle qu'on juge, qui atteint le même board
et rippe mais VIOLE la garde permanente. C'est le montage à refaire, parce qu'un gain appris sur la
ligne jugée ne prouverait rien. *Réserve à ne pas effacer : c'est une LIGNE tenue à l'écart, pas
une INSTANCE — même deck, même board. La généralisation reste entièrement à mesurer.*

**UN TROU DANS NOTRE PROPRE INSTRUMENT, trouvé en relisant l'A/B.** La colonne « `lm h` » du
relevé était VIDE partout. Cause : le compteur de vie du `h` de landmarks n'était imprimé que par
`PrintCuts`, qui ne couvre pas la phase tirages — c'est-à-dire précisément pas la phase où
`--landmark-w` travaille. Piège 52 appliqué au mécanisme de la session elle-même. Corrigé (la ligne
est désormais imprimée au bilan des tirages), **mais l'A/B ci-dessus a tourné sans elle** : on ne
sait donc pas si le `h` appris a réellement décru pendant ces runs. C'est la première chose à
relever au prochain A/B, avant tout juge de recherche.

**(e) `--archive-spread` DÉPARTAGÉ sur un juge valide (chantier (c) de la mission).** Son critère
INTERNE se confirme, et largement : la médiane d'expansions par racine du finisseur passe de **16 à
164 (×10)** sur l'étalon B optimisé — la s15 mesurait ×5,9 sur l'étalon A. L'archive range de
nouveau des états qui ont de la ligne devant eux. **Sur les juges de recherche, en revanche, il est
NEUTRE à légèrement négatif** (≥1 18 082 contre 22 417, ≥3 0 contre 0, 0 ligne). Verdict : le
mécanisme fait ce qu'il annonce, et **ce qu'il annonce ne convertit pas**. Il reste opt-in. Les deux
autres réparés de la s15 (`--no-phase-change`, `--canonical-zones`) restent non départagés.

**(f) SANTÉ.** Santé stricte en cours de chantier : 22 lignes de diff contre `s15_sante.log` ;
**santé finale : 14 lignes**, toutes des durées et le nom d'outdir (273 digests, 210/273 coups
identifiés, 209 candidates, 16 replays sur 209). Les trois mécanismes de la session
(`--probe-repeat`, `--landmarks`, les zones adverses du relevé de faits) sont dormants à l'octet
près quand ils sont éteints.

**(g) LA QUESTION DE L'OPÉRATEUR, posée en fin de séance, et qui vaut d'être au dossier :
« est-ce un solveur GÉNÉRIQUE pour n'importe quel deck, ou du réglage fin sur nos deux decks de
test ? »** La réponse a deux moitiés qui ne vont pas dans le même sens, et confondre les deux
serait la faute.

*Le MOTEUR est générique, et cela se vérifie ligne à ligne.* Aucune connaissance de carte n'est
compilée : l'arène, le graphe d'états, la transposition, l'élagage par nouveauté, NRPA, le
finisseur LTS, l'archive Go-Explore, les macros et le bandit `Q̂` ne manipulent que des prompts et
des digests. Le graphe de RECETTES apprend des `MSG_MOVE`/`MSG_SUMMONING` observés, et son amorce
parse la ligne de matériaux depuis la `.cdb` — aucune liste de cartes en dur. Le graphe de
LANDMARKS relève des faits `(code, zone, compte)` sur des plans rejoués, sans nommer une seule
carte. Tout ce qui est spécifique à un deck — `--hint`, `--resolve`, `--summon-min`, `--guard`,
`--target`, `--deck`, `--hand` — est une ENTRÉE de ligne de commande. Les scripts de `tools/`
codent nos deux cas en dur, mais ce sont des bancs de mesure, pas le produit.

*Les PREUVES, elles, ne sont pas génériques — et c'est le risque réel.* Quatre points, à ne pas
enjoliver :

1. **Les constantes sont calibrées sur DEUX decks** — `resolve_weight = 250`, `burn_slack`, la
   patience de nouveauté, `--options-len 8`, et maintenant `--landmark-w`. Plusieurs sont DÉRIVÉES
   d'une mesure plutôt que posées au jugé (la patience vient de la plus longue série muette
   mesurée le long de la référence, `burn_slack` du pic de brûlées réel) — ce qui est meilleur,
   mais mesuré sur deux decks.
2. **La liste `--hint` de l'étalon A est de la connaissance métier injectée à la main.** 9.22 (c)
   l'avait déjà établi : `Q̂` trouve Gold Leo SANS indice, et les cartes indicées classent plus
   mal. C'est une béquille assumée, pas un mécanisme.
3. **Le graphe de landmarks apprend depuis des plans résolus de l'INSTANCE.** L'entraîner sur la
   ligne qu'on juge revient à lui souffler la réponse — d'où le bras `lmx60` sur une ligne tenue à
   l'écart, et la réserve qui l'accompagne : c'est une LIGNE tenue à l'écart, pas une INSTANCE.
4. **Aucune mesure n'existe sur un TROISIÈME deck.** Toute affirmation de généralité serait
   aujourd'hui une croyance.

*Ce qui joue en faveur du moteur* : la discipline du dossier a RÉFUTÉ plus de mécanismes qu'elle
n'en a réglés — `--mcps` (deux fois), `--options-len`, `--options-pool`, `--nrpa-lr 2`,
`--novelty-rollout-cut`, la garde sémantique en ligne. Ce n'est pas un historique de boutons
tournés jusqu'à ce que deux cas passent.

*Le chantier que cette question ouvre, et qui est désormais prioritaire* : un **TROISIÈME deck**,
sans rapport avec les deux autres, avec sa propre ligne de référence — et le seul juge qui vaille
pour la généralité : apprendre les landmarks sur A et B, les SERVIR sur C. Avec, au passage, une
limite du mécanisme d'aujourd'hui à corriger : il exige un plan résolu, donc il ne sert à rien à
FROID. La voie naturelle est de le miner EN LIGNE sur les meilleures approches du run lui-même,
exactement comme `--options-online` le fait déjà pour les macros.

**(h) LA VRAIE PANNE, TROUVÉE EN FIN DE SÉANCE : ce n'est pas la RÉPÉTITION, c'est l'ARITÉ.**
Question de l'opérateur : *« on a un solveur qui ne trouve que si on lui donne ? comment lui faire
trouver en no-hint, no-reference ? »* La réponse a demandé quatre runs de 90 s, et elle retire le
cadrage des sessions 15 ET 16.

*Premier run — le curriculum.* Cible réduite à **UN SEUL** Liger Dancer, tout le reste inchangé :

| bras (cible = 1 Liger, 90 s) | résolutions ≥1 / ≥3 | Liger invoqués | approche |
|---|---|---|---|
| avec `--hint` | 295 456 / 651 | **0** | **0/1** |
| sans `--hint` | 254 039 / 1 908 | **0** | **0/1** |

**Le solveur échoue à N = 1.** Le « mur des sous-buts qui se disputent les ressources » de 9.22 (h)
et le « second exemplaire » de 9.23 (b) portaient tous deux sur une répétition que le solveur
n'atteint même pas une fois. Et les indices ne servent à rien : sans eux, ≥3 est TROIS FOIS
meilleur.

*Deuxième objection de l'opérateur, et elle invalide l'instrument* : **`--resolve` est un indice
déguisé** — le code lui donne `hint_bias` d'office (`cfg.hint_cards.push_back(req.code)`), plus un
gradient de `resolve_weight` et une exigence au but. Une sonde qui, pour compter, doit aiguiller,
ne peut pas mesurer « le solveur trouve-t-il SEUL ». D'où **`--watch`** : compter, et rien d'autre.
Aucune contrainte, aucun gradient, aucun biais.

*Run VRAIMENT nu* (aucun `--hint`, aucun `--resolve`, aucun `--summon-min`, aucune référence ;
seule la cible et `--watch`) :

| Fusion | matériaux exigés | tirages qui l'invoquent |
|---|---|---|
| Lunalight Perfume Dancer | 2 "Lunalight" | **42 525** |
| Lunalight Sabre Dancer | 3 "Lunalight" | **2 073** |
| Lunalight Leo Dancer | 1 **NOMMÉ** + 2 | **0** |
| Lunalight Liger Dancer | 1 **NOMMÉ** + 3 | **0** |

**La fréquence d'une invocation s'effondre avec son ARITÉ : ÷20 par matériau supplémentaire, puis
zéro dès qu'un matériau est NOMMÉ.** Ce n'est pas un mur de conjonction entre sous-buts, c'est un
effondrement exponentiel de l'échantillonnage en la longueur de la chaîne de choix. Le solveur
prend systématiquement la Fusion la moins chère, parce que **rien ne relie l'énoncé au coup qui le
réalise**.

*Deux causes mécaniques, vérifiées dans le source :*

1. **Les codes de la CIBLE n'entrent jamais dans le biais d'échantillonnage.** `cfg.hint_cards`
   n'est alimenté que par `--hint` (écrit à la main) et par `cons.resolve_min`. Le solveur à qui
   l'on demande trois Liger Dancer n'a donc **aucune préférence** pour le coup « invoquer Liger
   Dancer » — et la liste `--hint` de l'étalon A ne contient même pas son code.
2. **`MSG_SELECT_CARD` ne renseignait pas `Choice::card`.** Ses choix sont des SOUS-ENSEMBLES, et
   aucune carte ne les nommait — or c'est exactement là que se décide *quelle* Fusion invoquer et
   *avec quels matériaux*. **Le prompt le plus déterminant du domaine était invisible au biais**,
   qui ne lit que `Choice::card`.

*Le correctif évident a été écrit et MESURÉ — et il ÉCHOUE.* `--goal-bias` fait les deux à la fois
(codes de la cible versés au biais ; identité de carte sur les prompts de sélection) : sur la
cible à un Liger, **Liger reste à ZÉRO**, et le biais fait même TOMBER les autres Fusions
(Perfume 42 525 → 12 451). Le drapeau reste implémenté et opt-in.

**POURQUOI IL ÉCHOUE, et c'est la conclusion qui vaut.** Le prompt de Fusion ne liste que les
monstres dont les matériaux sont PAYABLES à cet instant. Tant que Kaleido Chick n'a pas copié le
nom de Leo Dancer et que trois Lunalight ne sont pas disponibles, **Liger Dancer n'est pas dans la
liste** — biaiser un choix qui n'existe pas ne fait rien. Le problème n'est donc pas le choix, c'est
**l'ÉTAT** : rien dans le score ne récompense le fait de s'en approcher.

Et le score le confirme, ligne à ligne : `material = common×100 + exact×10 + monstres×3 +
cimetière`, où `common` compte les cartes cibles **PRÉSENTES**. Liger n'étant jamais posé, ce terme
vaut 0 pour toujours ; la politique optimise donc « poser des corps », et c'est très exactement ce
qu'elle fait (4 à 6 monstres posés, 0 Liger).

**LA PRESCRIPTION, désormais adossée à une mesure et non à une intuition : mettre une distance qui
DÉCROÎT dans le score des TIRAGES.** L'objet existe depuis le chantier 16 — `RecipeDistance`
compte les invocations restantes, matériaux intermédiaires compris, et vaut 5 pour Liger depuis
l'état de départ. §9.16 l'a mesurée et a conclu, mot pour mot, que **« les solutions viennent d'une
phase que `--recipes` ne touche pas »** : elle n'a jamais été évaluée ailleurs que dans le
finisseur, où elle n'est qu'un réglage de `--levin-h` déguisé. La session 16 a construit, pour les
landmarks, le chemin d'évaluation BON MARCHÉ dans les tirages (clés indexées, zones interrogées
seulement si un landmark y vit). **Y porter la distance de recettes est petit, et c'est le premier
chantier de la session 17.**

---

### 9.24 Session 17 : la sonde d'OFFRE — et le seul des quatre leviers qui monte l'arité

Mission : attaquer les quatre solutions dégagées de la littérature — assignation résolue,
hindsight, distance de recettes dans les tirages, décomposition à rebours — **avec rigueur**,
c'est-à-dire un drapeau par mécanisme, éteint par défaut, A/B seul.

Le premier geste n'est aucun des quatre. C'est l'instrument (piège 40) : §9.23 mesure la **loi
d'arité** mais pas son LIEU, et les quatre chantiers se répartissent exactement sur ce lieu.

#### (a) La sonde d'OFFRE, et le défaut d'instrument corrigé DEUX FOIS avant de conclure

`reached[0] == 0` (« jamais invoquée ») recouvre deux pannes opposées :

- **jamais proposée** — le core ne liste une invocation que si elle est payable *à cet instant* :
  la panne est dans l'**ÉTAT**, et les chantiers sont `--recipe-w` et `--backward` ;
- **proposée et jamais prise** — la panne est dans l'**ÉCHANTILLONNAGE**, et les chantiers sont
  `--assign` et `--hindsight`.

`--goal-bias` (§9.23) a échoué faute *exactement* de cette lecture.

**Implémentation, à coût et à comportement nuls.** Le marquage est greffé sur le point de passage
obligé : la lambda `canon` d'`EnumerateRaw`, par où passe *tout* code lu dans *tout* prompt. Un
marquage écrit message par message aurait manqué un prompt en silence — et « jamais proposée » est
précisément la conclusion qu'une sonde incomplète fabrique à tort. Aucun choix n'est ajouté,
retiré ni repondéré.

**Premier défaut, hérité : les noms.** Le run nu de la session 16 passait les quatre Fusions à
`--watch` **par nom**. Or « Lunalight Liger Dancer » est AMBIGU (54701958 **et** 101301030) et
l'outil sort sur un nom ambigu : ce run surveillait donc une liste **tronquée**, ce que sa lecture
n'avait pas relevé. Codes désormais, comme la règle du dossier l'impose depuis la session 15.

**Second défaut, celui de la sonde elle-même — et il a produit une conclusion FAUSSE.** Premier
relevé (étalon A nu, 90 s, graine 888, 599 234 tirages) :

| carte | offerte dans | décisions |
|---|---|---|
| Perfume Dancer | 44 201 tirages (7,4 %) | 95 764 |
| Sabre Dancer | 33 867 (5,7 %) | 34 221 |
| Leo Dancer | 33 301 (5,6 %) | 33 307 |
| Liger Dancer | 33 301 (5,6 %) | 33 310 |

Lu tel quel : « Leo et Liger sont proposés 33 000 fois et jamais pris → la panne est dans
l'échantillonnage ». **C'est faux, et la sonde l'imprimait.** Le rapport décisions/tirages vaut
1,0002 pour Leo et Liger contre 2,17 pour Perfume : *un seul prompt par tirage, toujours le même*.
« Présente dans le pool d'un prompt » n'est **pas** « invocable » — une révélation d'extra deck,
une défausse, une recherche listent la carte aussi.

Correctif en deux temps : ventilation par type de prompt (masque), puis **compteurs par type**
(`RepeatProbe::offer_by`, six types). Relevé final (90 s, 619 000 tirages) :

| carte | matériaux | SELECT_CARD | IDLECMD | **POSITION** | invoquée |
|---|---|---|---|---|---|
| Perfume Dancer | 2 | 134 758 | **13 643** | **9 349** | 7 138 |
| Sabre Dancer | 3 | 125 040 | 0 | **320** | 320 |
| Leo Dancer | 1 nommé + 2 | 123 100 | 0 | **0** | 0 |
| Liger Dancer | 1 nommé + 3 | 123 098 | 0 | **0** | 0 |

Les ~123 000 `SELECT_CARD` sont **le même pool pour les quatre** : l'extra deck lu en entier, du
bruit. Le signal est `IDLECMD` (invoquer) et `POSITION` (poser), et **`POSITION` est validée comme
juge par la mesure elle-même** — pour Sabre Dancer, POSITION (320) égale *exactement* le nombre
d'invocations (320).

**VERDICT : la panne est dans l'ÉTAT.** Leo et Liger ne sont **jamais invocables**, jamais posés.
Cela met `--recipe-w` et `--backward` devant, et donne au reste de la session un juge **binaire et
sans graine** : la colonne POSITION passe-t-elle de 0 à non nul ?

#### (b) Les quatre leviers, écrits et séparables

Quatre drapeaux, éteints par défaut, chacun A/B-able seul. `--probe-repeat` impliquant déjà
`--recipes 0`, le graphe de recettes est alimenté et mesuré **dans tous les bras, témoin compris** :
seuls les drapeaux de la session changent.

1. **`--assign`** (Delarue et al., arXiv:2010.12001 — la sélection d'action posée en OPTIMISATION).
   Les prompts de sous-ensemble émettent **en plus** les deux sous-ensembles extrêmes au sens des
   recettes (le plus utile et le moins utile), dédoublonnés. Motif : la troncature de
   `ForEachSubset` est **lexicographique**, donc au-delà de `--max-subsets` le bon sous-ensemble
   n'est pas rare, il est **absent** — aucun poids de politique ne rattrape cela. Les **deux**
   extrêmes et pas un seul : un prompt ne dit pas s'il demande des matériaux ou des cartes à
   défausser, on ne décide donc pas du signe, la politique l'apprend par `plan_key`.
2. **`--hindsight <f>`** (HER, NeurIPS 2017). Chaque monstre d'**extra deck** réellement invoqué
   devient un but de substitution ; la meilleure ligne qui l'atteint subit le gradient NRPA à
   `f × alpha`. Le score est **ré-étiqueté** — pas le matériel de la ligne (qui juge l'ancien but)
   mais le coût de *cet* accomplissement, au plus tôt et au plus court. Le filtre est un **TYPE**
   (`TYPE_FUSION|SYNCHRO|XYZ|LINK`), pas une liste : aucun nom de carte compilé, et l'ensemble
   ainsi désigné est exactement celui dont l'arité est le mur.
3. **`--recipe-w <f>`** : la distance de recettes dans le **score des tirages**, en PROGRÈS
   (`d0 − d`, sinon le score, qui est un MAX initialisé à 0, ne remonterait jamais).
4. **`--backward`** (Retro\*, AO\*) : `RecipeGraph::Expand` rend les sous-produits en ordre de
   fabrication ; le nombre de ceux déjà fabriqués entre dans la **partition de la table de
   nouveauté**, qui se rouvre donc avant qu'aucune cible ne soit posée (Serialized IW sur la
   décomposition apprise au lieu du but littéral).

**Le chemin bon marché**, sans lequel 3 et 4 seraient inévaluables dans les tirages : le graphe
partagé porte un mutex (il apprend pendant le run), donc chaque worker en prend une **copie locale**
tous les 2 048 tirages, à la frontière sûre de la profondeur 0 ; le relevé de présence n'interroge
que les zones qu'une exigence **mentionne** (`RecipeGraph::ZoneMask`) là où `RecipeDistance` en
relève cinq, DECK et EXTRA compris (~55 entités, la moitié du coût) ; le terrain sort gratuitement
du board déjà calculé. `PresentAvailT` est remontée au niveau de l'espace de noms pour que le
finisseur et les tirages mesurent **exactement la même grandeur**.

#### (c) L'A/B : un seul levier monte l'arité

Étalon A nu, 90 s, graine 888. Juge = poses (POSITION).

| bras | tirages | Perfume (2 mat.) | **Sabre (3 mat.)** | Leo | Liger |
|---|---|---|---|---|---|
| témoin | 446 281 | 74 788 | 166 | 0 | 0 |
| `--hindsight 0.25` | 536 303 | 58 738 | 589 | 0 | 0 |
| **`--hindsight 0.5`** | 528 740 | **79 009** | **3 422** | 0 | 0 |
| `--hindsight 1.0` | 582 223 | 74 309 | 3 218 | 0 | 0 |
| `--recipe-w 0.1` | 614 063 | 7 156 | 70 | 0 | 0 |
| `--recipe-w 0.3` | 489 928 | 8 870 | 24 | 0 | 0 |
| `--recipe-w 1.0` | 469 134 | 8 902 | 48 | 0 | 0 |
| `--backward` | 590 410 | 3 723 | 64 | 0 | 0 |
| `--assign` | 580 266 | 4 242 | 36 | 0 | 0 |

**`--hindsight 0.5` multiplie par 20,6 les poses de la Fusion à 3 matériaux** (166 → 3 422) **sans
rien coûter à celle à 2** (79 009 contre 74 788). C'est le seul bras qui monte l'axe d'arité, et
c'est l'effet que HER prédit : la politique apprend à payer une invocation coûteuse en s'entraînant
sur celles qu'elle réussit déjà. Vie du mécanisme : 121 buts de substitution, 271 662 adaptations.
La courbe est monotone jusqu'à 0,5 puis plate — 1,0 ne rapporte rien de plus.

#### (d) `--recipe-w` est RÉFUTÉ, et la cause est structurelle

Trois poids sur une décade (0,1 / 0,3 / 1,0) donnent **le même effondrement** (~8 000 contre
74 788). Ce n'est donc pas un cadran mal réglé. Le mécanisme est pourtant vivant et fait ce qu'on
lui demande : la distance moyenne vaut 10,70 contre 13,06 à l'état de départ, **elle décroît
réellement**. Le débit ne l'explique pas non plus — `--recipe-w 0.1` produit *plus* de tirages que
le témoin (614 063 contre 446 281).

**La cause.** La distance vers Liger Dancer compte « 3 monstres Lunalight » comme exigence
CARDINALE. Or fabriquer Perfume Dancer **consomme deux Lunalight pour en rendre un** : la distance
*monte*. Le mécanisme récompense donc l'**accumulation de corps** et **punit leur consommation** —
c'est-à-dire punit les invocations, l'inverse exact du but poursuivi. C'est le défaut jumeau du
piège 42 (« un mécanisme vivant et sans effet ») : ici il est vivant et **à contresens**.

Ce que cela dit du chantier, et qui vaut plus que le verdict : une heuristique de type h^add sur un
graphe ET/OU n'est correcte que si l'on modélise la **consommation** — la relaxation par
suppression suppose qu'atteindre un sous-but ne détruit rien, hypothèse exactement fausse ici.
`RecipeDistance` gère bien la consommation *intra-recette* (`claim_depth`, revue session 12) mais
pas *inter-invocations*, et le commentaire du source le disait déjà : « une consommation
inter-invocations modéliserait mal les corps recyclés par le cimetière ».

#### (e) `--backward` : vivant, sans matière ; `--assign` : dégrade

`--backward` : la décomposition ne contient que **2 sous-produits** (Leo, Liger) et n'en fabrique
0,02 en moyenne. L'expansion ne descend que sur les exigences NOMMÉES, et la recette amorcée de
Liger est « Leo Dancer nommé + 3 Lunalight (cardinale) » : il n'y a rien d'autre à décomposer. La
partition ×64 fragmente la table de nouveauté sans contrepartie, d'où la dégradation. Mécanisme
correct, **matière absente** — il ne pourra rien tant que le graphe n'aura pas de recettes plus
profondes, c'est-à-dire tant que Liger ne sera pas invoqué au moins une fois. Œuf et poule, et il
est nommé.

`--assign` : 4 242 poses de Perfume contre 74 788. Les sous-ensembles extrêmes injectés (15 codes
utiles, jusqu'à 4 choix ajoutés par prompt) diluent la distribution sur les prompts fréquents plus
qu'ils ne comblent la troncature sur les prompts rares. Le mécanisme ne prune rien (règle 2) mais
il n'est pas neutre en probabilité, et c'est ce qui le condamne ici.

#### (f) Budget long, deux graines : séparation complète des supports

Étalon A nu, 300 s. **Deux graines, et elles étaient nécessaires** — la variance du témoin est
énorme, et une seule graine aurait fait conclure trop fort dans un sens comme dans l'autre.

| bras | graine | tirages | Perfume (arité 2) | Sabre (arité 3) | Leo | Liger |
|---|---|---|---|---|---|---|
| témoin | 888 | 1 236 499 | 29 745 | 659 | 0 | 0 |
| témoin | 1234 | 854 444 | 504 793 | 1 770 | 0 | 0 |
| `--hindsight 0.5` | 888 | 1 009 789 | **602 016** | **10 722** | 0 | 0 |
| `--hindsight 0.5` | 1234 | 996 534 | **578 025** | **5 668** | 0 | 0 |

**`min(hindsight) > max(témoin)` sur les deux cartes** : 578 025 > 504 793 et 5 668 > 1 770. La
séparation des supports est complète, sur un juge où le témoin varie d'un facteur **17** d'une
graine à l'autre (29 745 → 504 793). Le gain sur l'arité 3 va de ×3,2 à ×16,3 selon la graine.

**Le mécanisme STABILISE aussi**, et c'est cohérent avec sa nature : le témoin dépend entièrement de
la chance d'avoir trouvé tôt une bonne ouverture, alors que hindsight entraîne la politique sur des
buts qu'elle atteint *toujours* (602 016 / 578 025, écart 4 %). Coût payé : **−18 % de débit** — une
copie de `steps` par amélioration, et 126 lignes ré-adaptées à chaque itération de niveau supérieur
(46 231 adaptations sur le run).

**Statut honnête : CONFIRMÉ sur deux graines avec séparation complète, pas encore DÉMONTRÉ** au sens
du dossier (qui demande plus de deux tirages et le passage sur l'étalon B — le seul cas où une
solution existe). `tools/s17_ab_B.ps1` est écrit et n'a pas été lancé.

#### (g) LE PREMIER PLAN RÉSOLU DE L'ÉTALON A — et ce qu'il révèle : TROIS TROUS DE COUVERTURE

L'opérateur a fourni `2026-08-16 13-19-12.yrpX`. Jugé avant d'en faire un étalon, comme la règle
l'exige : **rejeu fidèle, 283/283 réponses, 0 `MSG_RETRY`, 1 tour**. Board final : **deux Lunalight
Liger Dancer** en MZONE, plus Tiger / Tenki / Masquerade / Wolf en S/T ; 17 cartes brûlées.
Invocations : 1 normale, 17 spéciales, dont **2× Leo Dancer**, **2× Liger Dancer**, 1× Perfume
Dancer, 1× Kaleido Chick, 1× Tiger, 1× Wolf, 1× Masquerade, 1× Gold Leo.

C'est ce qui manquait à tout le dossier : l'étalon A n'avait **aucun** plan résolu, d'où trois
conséquences en cascade — pas de corpus d'adaptation, pas de landmarks apprenables, et un graphe de
recettes qui ne connaissait de Liger et de Leo que **l'amorce par le texte** (c'est la « matière
absente » de (e)).

*Réserve dite d'avance* : la ligne joue l'**ancienne** main (3 Tenki) et fait **2 Liger sur les 3
exigés, sans Bagooska**. Ce n'est donc pas une solution de la cible : c'est un **corpus**.

**LE CONTRÔLE DE COUVERTURE, ET LA CORRECTION DE SON PROPRE INSTRUMENT.** Premier relevé :

```
SELECT_IDLECMD  30/31 (97 %)   #0  : aucune des 4 propositions n'atteint l'etat
SELECT_CARD     31/33 (94 %)   #21 : aucune des 9 propositions
                               #75 : aucune des 2 propositions
```

Lu tel quel : « trois décisions sur 284 sont hors de l'espace d'actions du solveur, dont la toute
première ». **C'était faux, et c'est l'instrument qui l'était.** Le contrôle comparait
`Fingerprint(duel)` — l'état EXACT, séquences comprises — alors que la recherche travaille au
**board**. Or son propre commentaire admettait déjà que « la déduplication par code choisit un
représentant qui n'est pas forcément celui qu'a désigné le joueur » : deux exemplaires identiques
n'occupent pas la même séquence, donc l'état exact diffère quand l'intention est la même.

Le contrôle sépare désormais trois causes (réponse enregistrée rejetée / **même board, état
différent** / coup réellement absent), et rend :

| | n | couvert | taux | m.board |
|---|---|---|---|---|
| TOTAL | 283 | 280 | 99 % | 3 |
| **TOTAL (au board)** | 283 | **283** | **100 %** | — |

**L'énumérateur couvre 100 % des INTENTIONS de la ligne.** Les trois écarts sont de
**représentation**, pas d'espace. Deux attributions ont été écartées en route et n'ont pas à être
refaites : ce n'est pas `--max-subsets` (24 et 256 : trous identiques), ni la main transplantée (même
résultat sur le duel **natif**, `--start <plan>`).

#### (h) LE VRAI DÉFAUT, LUI, ÉTAIT AILLEURS : `MSG_SELECT_BATTLECMD` MAL DÉCODÉ

Le même run signalait, à un écart de la transplantation :

```
!! 90 prompt(s) reduits a LA reponse par defaut (types : 10)
   Ces branches n'ont jamais existe : un combo qui les traverse est hors d'atteinte.
   sante : impasses 90
```

Type 10 = `MSG_SELECT_BATTLECMD`. Vérification contre le source du core
(`deps/ocgcore/playerop.cpp:37`) : dans la liste **attaquable**, l'entrée est

```
code uint32 | controler uint8 | location uint8 | sequence uint8 | direct uint8   = 8 octets
```

`sequence` y est un **uint8**, alors qu'il est un `uint32` dans la liste *activable* du même message
— et notre décodeur sautait 11 octets au lieu de 8. **Trois octets de décalage par monstre
attaquable**, donc `r.Ok()` tombe dès qu'il y en a un, ce qui est le cas de tout board construit.
L'énumération sort vide, `DefaultResponse` ne couvre pas ce message, et **la branche meurt**.

**Conséquence, silencieuse depuis l'origine : toute ligne qui entre en Battle Phase était condamnée**
— donc tout combo passant par la Main 2 était hors d'atteinte, et la mort se lisait comme une impasse
ordinaire. C'est la différence entre les deux listes du *même* message qui a fait passer le défaut
inaperçu (le comptage était juste dans l'autre).

Corrigé. Effet mesuré, mêmes commandes et même graine : **90 prompts forcés → 0, 90 impasses → 0**,
la Battle Phase redevient explorable. Santé identique avant et après (273 digests, 210/273, 209
candidates, 16 replays).

**Ce que le correctif ne fait PAS, et il faut le dire aussi** : Leo et Liger restent à
`IDLECMD = POSITION = 0`, et la transplantation reste à **4 des 6 cartes** — identique à 0, 1 et
2 écarts (602, 11 248 puis 33 078 états). Le bug était réel et il n'était pas le mur.

*Ce que la transplantation apprend en creux* : **à ZÉRO écart, en ne jouant que des coups du
répertoire de la ligne résolue, le solveur atteint déjà 4/6 en 602 états** — puis n'avance plus en
en dépensant cinquante fois plus. Le répertoire est traité comme un ENSEMBLE de coups, pas comme une
séquence ; avec la nouvelle main (Gold Leo directement, au lieu de Tenki → Gold Leo) les premiers
carrefours diffèrent, et ce qui manque n'est pas dans le répertoire. **Augmenter le budget d'écarts
au-delà de 2 est la mesure suivante, et elle n'a pas été faite.**

**Leo et Liger restent à ZÉRO.** C'est le résultat le plus important de la session après le
diagnostic, et il faut le dire tel quel : **`--hindsight` monte l'arité mais ne franchit pas la
discontinuité du matériau NOMMÉ.** La loi de §9.23 se lit désormais en deux régimes distincts, et
un seul est attaqué :

- **le régime CARDINAL** (« 3 monstres Lunalight ») est une *pente* : chaque corps supplémentaire
  rapproche, et hindsight l'escalade — ÷20 par matériau devient ×20 ;
- **le régime NOMMÉ** (« "Lunalight Leo Dancer" + 3 Lunalight ») est une *discontinuité* : tant que
  la carte nommée n'existe pas, la Fusion n'est pas listée, aucune quantité de tirages ne la
  trouve, et aucun gradient de politique ne peut aider puisque **le coup n'est pas dans l'espace**.

C'est la même chose que la sonde d'offre a mesurée en (a) : `IDLECMD = 0`, `POSITION = 0`. Le
levier qui reste est donc bien la **construction dirigée de la carte nommée** — c'est-à-dire la
recherche à rebours du chantier 4, mais alimentée par un graphe qui a de quoi décomposer.
