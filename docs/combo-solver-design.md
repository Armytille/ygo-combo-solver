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
| Équivalence du board final | Mêmes cartes sur le terrain par **type de zone** (MZONE / EMZ / SZONE / Pendule), mêmes **positions**, mêmes **matériaux** (Xyz/Fusion/Link overlay), mêmes **compteurs**. La colonne exacte est ignorée. |
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
