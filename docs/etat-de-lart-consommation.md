# État de l'art — LA CONSOMMATION, et le nom que la littérature donne à notre mur

*Revue faite en ouverture de la session 20, à la demande de l'opérateur, AVANT
d'écrire une ligne. Elle ne refait pas la bibliographie de §9.22 (h), §9.23 (c)
et §9.24 : elle part de là où celles-ci s'arrêtent. Ces trois sections ont
**nommé** le mur (« les sous-buts identiques se disputent les ressources ») et
cité les papiers qui le **diagnostiquent** (largeur sérialisée, SIW qui échoue,
Delivery, CraftWorld). Aucune n'a cité un papier qui le **résout**. C'est
l'objet de celle-ci.*

---

## 0. LE RÉSULTAT EN UNE PHRASE

Notre mur n'est pas un problème ouvert : c'est un problème **résolu en 2013**,
et la solution est une **équation de bilan matière** (state equation / net
change) résolue par un programme linéaire — pas une heuristique de plus, pas un
drapeau, et surtout pas une variante de `h^add` réparée.

`--recipe-w` n'était pas un mauvais réglage : c'était **la mauvaise famille de
relaxation**, et la littérature dit exactement laquelle prendre à la place.

---

## 1. LE RÉSULTAT CENTRAL : `h^SEQ`, ET IL EST FAIT POUR NOUS

**Bonet, *An Admissible Heuristic for SAS+ Planning Obtained from the State
Equation*, IJCAI 2013**
([PDF](https://bonetblai.github.io/reports/IJCAI13-seq-heuristic.pdf)).

Le problème de planification est vu comme un **réseau de Petri** : chaque atome
est une *place*, chaque action une *transition*, et la matrice d'incidence `A`
porte, en entrée `a_ij`, le **changement net de jetons** que la transition `i`
cause à la place `j`. Pour une suite d'actions `π`, le vecteur de comptes de tir
`u_π` (combien de fois chaque action est jouée) satisfait nécessairement

```
    A^T u_π  ≥  M_but − M_s
```

D'où l'heuristique, **calculable en temps polynomial** :

```
    h_SEQ(s) = ⌈ min c^T x ⌉     s.c.   A^T x ≥ M_but − M_s ,   x ≥ 0
```

Trois phrases du papier, qui sont la description littérale de notre situation :

> « it is **not a delete-relaxation heuristic** as it considers the positive and
> negative effects of the actions »

> « it **may even infer that an action must be applied multiple times** in order
> to reach the goal from a given state »

> « if `A^T x ≥ M_sG − M_0` has **no solution**, then there is no sequence `π`
> that achieves the goal »

Traduction dans notre vocabulaire, ligne par ligne :

| le papier | l'étalon A |
|---|---|
| effets négatifs pris en compte | fabriquer Perfume **consomme** deux corps Lunalight pour en rendre un — la cause exacte de la réfutation de `--recipe-w` (§9.24 (d)) |
| l'action doit tirer plusieurs fois | **3 Liger Dancer** : le LP infère « la Fusion tire trois fois », et **facture les neuf corps** |
| système infaisable ⇒ pas de plan | les **douze arêtes sur treize** de §9.28 (h) qui « détruisaient le but » sortent d'un test de faisabilité, pas d'un pis-aller (« le code est-il nommé comme matériau ? ») |

**Et le point de mesure est déjà au dossier.** §9.22 (h) a mesuré qu'un board de
**huit** cartes distinctes se reconstruit presque nu (7/8) tandis qu'un board de
**quatre** avec trois exemplaires identiques résiste (1-2/4). C'est exactement
le régime où la relaxation par suppression est la plus mauvaise (elle rend le
comptage **gratuit**) et où `h^SEQ` est la plus forte (la place du but porte
`M_but(p) = 3`, la contrainte est serrée). Les deux étalons ne se contredisent
pas : ils séparent les deux familles de relaxation.

### Ce que `h^SEQ` rend, et seulement le troisième est une heuristique

1. **`∞` = impasse PROUVÉE.** Un tirage où le but est devenu inatteignable est
   détectable en un test, sans dérouler la fin du tour. C'est le chantier E vu
   par l'autre bout, et c'est la seule forme d'élagage compatible avec la
   règle 2 du dossier — on n'élague pas un choix jugé mauvais, on **arrête un
   tirage démontré mort**.
2. **`x*` = le vecteur de comptes de tir.** *Quels* opérateurs, et **combien de
   fois**. C'est précisément ce que `--op-bias` consomme aujourd'hui sous une
   forme dégradée (« 1 carte désignée » — Kaleido Chick). Le LP le rend avec sa
   **multiplicité**, ce qui transforme « la politique n'a aucune raison d'en
   faire un second » en « le plan dit : tirer cet opérateur trois fois ».
3. **La valeur `⌈c^T x*⌉` = un gradient**, admissible et — contrairement à
   `RecipeDistance` — **décroissant quand on consomme à bon escient**.

### L'escalier d'implémentation — et la première marche ne demande AUCUN solveur

*Marche 0 — la relaxation par ligne (gratuite, saine, ~40 lignes).* Une seule
inégalité `A_p^T x ≥ b_p`, `x ≥ 0`, est infaisable **ssi** `b_p > 0` et aucune
entrée de la ligne n'est positive. En clair : *le but demande 3 Ligers, il en
reste 1 accessible, et plus aucun opérateur ne peut en produire un net* ⇒
impasse. Aucun simplexe, une boucle sur les places. C'est la version minimale et
**sonore** du test, et c'est le juge que §9.28 (h) a fabriqué à la main.

**OBJECTION POSÉE EN SÉANCE, ET ELLE CORRIGE L'ÉNONCÉ : « le deck RECYCLE ».**
Exact, et « il en reste 1 » était un raisonnement de **stock** — c'est-à-dire
exactement l'erreur que l'équation de bilan existe pour ne pas commettre. Un
recycleur est une **transition** portant une entrée **positive** sur la place
visée ; tant qu'il figure dans la matrice, la ligne reste faisable et **rien
n'est élagué**. Le modèle est un modèle de **flux**, pas de compte.

Trois conséquences, dont deux sont des règles d'ingénierie :

1. **L'asymétrie du risque dicte la construction.** Oublier une arête de
   **consommation** rend le LP trop optimiste : heuristique plus molle, jamais
   d'élagage faux. Oublier une arête de **production** (un recycleur) rend le
   test **NON SONORE** : il tue une vraie ligne. Donc la colonne positive se
   construit **sur-complète** — au moindre doute, on suppose la production
   possible et la ligne reste vivante. C'est la direction sûre de la règle 2.
2. **Ce qui fait quand même mordre le test, ce n'est pas la rareté, c'est la
   CAPACITÉ.** Un recycleur n'est pas gratuit et surtout il est **borné** :
   `e:SetCountLimit(1, {id,n})` est un jeton **déclaratif** du Lua, greppable
   comme `aux.Stringid`. Il se traduit directement en `Y_o ≤ 1` dans le cadre
   *operator counting*. **Sans cette borne, le LP fera tirer le recycleur cinq
   fois et ne dira jamais rien** ; avec elle, il est serré. C'est la pièce
   manquante à extraire, et elle est du même niveau de difficulté que la table
   de la s19.
3. **Le test ne prouve JAMAIS l'atteignabilité, seulement la NON-atteignabilité**
   (condition nécessaire : ordre et légalité sont ignorés, les conditions sont
   des fermetures non évaluées). Le recyclage l'affaiblit ; il ne le rend pas
   faux.

**Le cas de l'étalon A, LU dans les scripts et non supposé** (`c54701958.lua`,
`c81196066.lua` de `deps/scripts_2026-04-13`) :

- **Liger porte `c:AddMustBeFusionSummoned()` et `c:EnableReviveLimit()`.** Un
  Liger au **cimetière ne peut plus jamais revenir sur le terrain**. La place
  « Liger @cimetière » est un **puits**, script en main.
- **Le seul recycleur du deck sur ce corps est Perfume `e2`** : il renvoie une
  Lunalight **face recto du TERRAIN** à la main — et pour un monstre d'extra,
  `SendtoHand` le remet à l'**EXTRA** (le script teste
  `tc:IsLocation(LOCATION_HAND|LOCATION_EXTRA)`). C'est donc une arête
  **terrain → extra**, pas cimetière → terrain, et elle porte
  **`SetCountLimit(1,{id,1})` : une fois par tour**, toutes copies confondues.
- **Et Liger se détruit lui-même** : son coût `descost` fait
  `Duel.SendtoGrave(g, REASON_COST)` sur une Lunalight prise **dans l'EXTRA** —
  donc potentiellement un autre Liger. C'est la famille des « douze arêtes sur
  treize » de §9.28 (h), **confirmée dans le script**, pas déduite.

**Ce que cela dit du chantier E, et c'est neuf.** La sonde du run de 300 s rend
`Liger : terrain 3, cimetiere 380`. Ces 380 ne sont pas des quasi-réussites :
un Liger au cimetière est un jeton **définitivement mort** pour un but « au
terrain ». Le rapport 380/3 mesure donc que la recherche passe l'essentiel de
son budget sur des routes qui **brûlent** l'extra deck — et c'est précisément
ce qu'un test de faisabilité par ligne détecterait au moment où la brûlure a
lieu, au lieu de laisser le tirage courir jusqu'à la fin du tour.

*Marche 1 — le LP complet.* ~50-200 opérateurs × ~50-100 places : un simplexe
maison ou une descente duale suffit, le coût est en microsecondes. Rend `x*`.

*Marche 2 — les resserrements du papier, dans l'ordre où il les donne :*
- **Safeness** : si une place ne peut jamais porter plus d'un jeton, l'inégalité
  devient une **égalité**. Test donné sur le problème, pas sur le réseau : `S_X`
  est sûre si toute action qui affecte `X` a une précondition sur `X`.
- **Landmarks** : `x(t_1) + … + x(t_k) ≥ 1` pour tout ensemble d'actions dont
  une au moins doit être jouée. Le dossier a **déjà** un graphe de landmarks
  appris (§9.23 (c)) — il n'a jamais été branché sur un LP.
- **Reformulation du but** : ajouter au but les atomes qui l'accompagnent
  nécessairement. Bonet mesure **+72,7 %** de couverture sur un domaine par ce
  seul geste. Notre équivalent : un but à peu d'atomes rend des contraintes
  molles.

### Le cadre qui généralise, et qui dit comment TOUT combiner

**Pommerening, Röger, Helmert & Bonet, *LP-Based Heuristics for Cost-Optimal
Planning*, ICAPS 2014** (prix du papier influent ICAPS-24,
[PDF](https://ai.dmi.unibas.ch/papers/pommerening-et-al-icaps2014.pdf)).

Une variable `Y_o` par opérateur = son nombre d'emplois. Toute heuristique
devient un **jeu de contraintes** sur ces variables, et — c'est le théorème qui
compte — **la réunion des contraintes domine le max des composantes**. On peut
donc empiler landmarks + bilan matière + abstractions dans **un seul LP**.

Les quatre classes d'opérateurs par atome, à recopier telles quelles (elles
tranchent le cas « on ne sait pas ce qu'il y avait avant ») :

```
AP  toujours produit    : eff = v   et   pre = v' ≠ v
SP  parfois produit     : eff = v   et   pre indéfinie
AC  toujours consomme   : eff = v' ≠ v   et   pre = v
SC  parfois consomme    : eff = v' ≠ v   et   pre indéfinie

contrainte basse :  Σ_AP Y_o + Σ_SP Y_o − Σ_AC Y_o  ≥  L
contrainte haute :  U  ≥  Σ_AP Y_o − Σ_AC Y_o − Σ_SC Y_o
```

**Le « parfois » est notre cas nominal**, et c'est la garde à retenir : la table
d'opérateurs de la s19 est **déclarative et optimiste** (les conditions sont des
fermetures non évaluées). Elle produit donc massivement du `SP`/`SC`, et la
littérature dit exactement quelle borne cela affaiblit — **la basse reste
valide**, ce qui suffit à la faisabilité et aux comptes de tir.

### La matière est déjà extraite, et lue par personne

Le prompt s20 le dit : **54 déclarations `Duel.SetOperationInfo`** (catégorie
*et* zone) et les verbes (`SendtoGrave`, `Remove`, `DiscardHand`) relevés par
fonction dans `CardOperators::fn_verbs` sont extraits, imprimés
([operators.cpp:1102](../operators.cpp#L1102) les compte,
[:1208](../operators.cpp#L1208) les imprime), et branchés sur rien. **C'est la
colonne négative de la matrice d'incidence.** La colonne positive existe déjà :
les recettes de [search.h:1569](../search.h#L1569). Il ne manque que l'algèbre.

---

## 2. LES CONCURRENTS, ET POURQUOI ILS PASSENT APRÈS

### Red-black planning — la réponse célèbre, et elle ne nous va PAS

**Domshlak, Hoffmann & Katz, *Red-Black Planning: A New Systematic Approach to
Partial Delete Relaxation*, AIJ 221 (2015)**
([PDF](https://fai.cs.uni-saarland.de/hoffmann/papers/ai15.pdf)).

Peindre en **noir** les variables dont on garde la sémantique réelle (les
suppressions comptent), en **rouge** les autres (sémantique relaxée,
accumulative). Interpolation exacte entre `h^+` et le vrai plan.

**Trois raisons de ne pas commencer par là**, et elles sont dans le papier :

1. La tractabilité exige que le **graphe causal des variables noires soit
   acyclique** et que ces variables soient **inversibles**. Nos zones sont
   cycliques par construction (terrain ↔ cimetière ↔ bannie) et le flux
   deck/extra → terrain n'est pas inversible.
2. Le comportement est **peu fiable** : le papier mesure un espace de recherche
   divisé par 100 sur certaines instances et **multiplié par 100** sur d'autres.
   Le dossier a déjà payé ce genre de variance (§9.24 (o), un run par bras).
3. Il faut une **stratégie de peinture**, c'est-à-dire un cadran de plus — et
   un cadran non balayé est un pari (règle du dossier).

`h^SEQ` n'a ni peinture, ni condition structurelle, ni cadran.

### LP-RPG — le plus proche voisin de notre domaine

**Coles, Coles, Fox & Long, *A Hybrid LP-RPG Heuristic for Modelling Numeric
Resource Flows in Planning*, JAIR (arXiv:1402.0564).**

Le résumé décrit notre panne mot pour mot :

> « A particular challenge […] is in handling interactions between metric
> fluents that represent **exchange**, such as the transformation of quantities
> of raw materials into quantities of processed goods […] The usual relaxation
> […] is often very poor in these situations, since it **does not recognise that
> resources, once spent, are no longer available to be spent again**. »

Ils définissent une classe de problèmes **producteur-consommateur** et couplent
un MIP (les ressources, exactement) à un RPG (le causal, relaxé). C'est
strictement plus fort que `h^SEQ` sur les flux, et strictement plus cher.
**À garder pour la marche 3**, si le LP seul est trop mou.

### Ce qui ne sert pas ici

- `h^m` / `h^2` (interactions par paires) : coûteux, et ne capture pas les
  multiplicités ≥ 3.
- Numeric novelty (Chen & Thiébaux, arXiv:2404.05235) : **utile plus tard**, la
  table de nouveauté du solveur pourrait indexer les compteurs de ressources et
  non les seuls atomes. Ce n'est pas la panne du jour.

---

## 3. LES SOUS-BUTS IDENTIQUES : SÉRIALISER, ET COMMENT

§9.22 (h) a établi le diagnostic (Drexler/Seipp/Geffner : *SIW échoue quand un
sous-problème a une largeur élevée*) sans citer le **remède des mêmes auteurs**.

**Drexler, Seipp & Geffner, *Learning Sketches for Decomposing Planning Problems
into Subproblems of Bounded Width*, ICAPS 2022 (arXiv:2203.14852).**

Un *sketch* est un jeu de règles `C ↦ E` sur des **traits** : `C` est une
condition booléenne, `E` un **changement qualitatif** (« ce compteur
décroît »). Chaque règle définit un sous-problème. Les sketches encodent
indifféremment une sérialisation de buts, une politique générale, ou une
décomposition de **largeur bornée** résoluble gloutonnement par `SIW_R`.

Ce que cela dit de nous, sans un run : notre `novelty_serialize` (rouvrir la
table de nouveauté à chaque carte cible posée) **est** un sketch — le plus
pauvre possible, à une règle et sans condition. `--op-bias` est un second
embryon de sketch. Le vocabulaire manquant est celui des **traits numériques**
(« nombre de corps Lunalight disponibles », « nombre de Ligers posés »), et le
critère de rouverture devrait porter sur leur **décroissance**, pas sur la pose
d'une carte. Les traits, eux, sortent du LP de la section 1 : ce sont ses
places à `M_but > 0`.

*Note d'honnêteté* : le papier **apprend** les sketches par ASP sur plusieurs
instances d'un domaine. Nous n'avons pas plusieurs instances (« un troisième
deck reste le seul juge de généralité, et il n'existe pas »). Ce qu'on prend
est donc la **forme** (règle = condition + décroissance d'un trait), pas
l'apprentissage.

---

## 4. LES IMPASSES : PROUVER QU'UN TIRAGE EST MORT

**Lipovetzky, Muise & Geffner, *Traps, Invariants, and Dead-Ends*, ICAPS 2016**
([PDF](https://www.dtic.upf.edu/~hgeffner/Nir-ICAPS-2016.pdf), implémentation
[trapper-lapkt](https://github.com/nirlipo/trapper-lapkt)).

- *invariant* : vrai à l'état initial et dans tout état atteignable ;
- *trap* : invariant **conditionnel** — une fois entré, on n'en sort plus ;
- *dead-end* : formule satisfaite par les états d'où le but est inatteignable.

Calcul en prétraitement polynomial, en k-DNF (exponentiel en `k` seulement).

C'est le pendant **symbolique** du `∞` de `h^SEQ`, et il est *statique* : il
peut se calculer une fois par deck. « Les trois copies de Liger sont dans
l'extra ; une route qui en consomme une pour renommer un Chick rend le but
inatteignable » **est un trap**, et §9.28 (h) l'a découvert à la main sur douze
arêtes.

**Junghanns & Schaeffer, *Sokoban: Enhancing general single-agent search methods
using domain knowledge*, AIJ 129 (2001)**
([PDF](http://sokoban.dk/wp-content/uploads/2016/02/Single-Agent.pdf)) reste la
référence pratique du même geste dans un domaine à consommation irréversible :
tables de blocage, coupes de pertinence, macro-coups. Le dossier a déjà les
macro-coups (`--elide-forced`, les options) ; il n'a **aucune** détection de
blocage.

---

## 5. LES JEUX DE CARTES : CE QUI EXISTE VRAIMENT

Le constat de la session 3 (« aucun solveur de combo Yu-Gi-Oh publié n'existe »)
**tient toujours**, et la revue le précise :

| travail | ce que c'est | ce qu'on en tire |
|---|---|---|
| **Nicolosi, Pisciotta & Bresolin, *Deciding winning strategies in Yu-Gi-Oh! TCG is hard*** (arXiv:2603.02863) | Le problème est **`Π¹₁`-complet**, pas seulement indécidable ; deux decks légaux explicités pour la réduction | Aucune complétude n'est atteignable **en principe**. Notre échantillonneur est le bon paradigme ; ce papier interdit d'espérer un solveur exact |
| **Tillo, Rehal & Muise, *Strategic Sorcery: Automated Planning for 'Magic: The Gathering'*, démo ICAPS 2024** ([OpenReview](https://openreview.net/forum?id=bZeQ7DB0T9), [Mu Lab](https://mulab.ai/publication/2024-icapsdemo-tillo/)) | Sous-ensemble des mécaniques de MTG en **PDDL numérique**, résolu par le planificateur **ENHSP** | **Le seul travail publié voisin, et il a choisi le NUMÉRIQUE** — pas la relaxation par suppression. Confirmation externe de la section 1 |
| Churchill, Biderman & Herrick, *Magic: The Gathering is Turing Complete* (arXiv:1904.09828) ; *Magic: the Gathering is as Hard as Arithmetic* (arXiv:2003.05119) | Complexité de MTG | Même leçon que ci-dessus |
| **[ygo-agent](https://github.com/sbl1996/ygo-agent)** (`ygoenv` sur envpool + ygopro-core, JAX, LSTM) | Environnement RL pour **duels complets** contre adversaire | Substrat identique au nôtre (ocgcore), **objectif orthogonal** : gagner une partie, pas atteindre un board nommé en solitaire. Rien à reprendre côté recherche ; à connaître si un jour on veut un adversaire |
| **[rafiimanggala/ygo-combo-solver](https://github.com/rafiimanggala/ygo-combo-solver)** | **Le seul concurrent direct connu.** Python, ocgcore par ctypes, DFS par rejeu + éval de board. Voir §5bis | Valide notre cadrage ; s'arrête là où nos difficultés commencent |
| [Open Combo Codex](https://www.opencombocodex.com/), YGO Combo Builder, calculateurs de probabilité | Outils communautaires : **documentation** et **taux de réalisation** de combos écrits par des humains | Aucun ne **cherche** une ligne. Le territoire d'application reste vierge |
| Pokémon : PTCG-Bench (arXiv:2605.29653), analyse Lean 4 du métagame (arXiv:2607.08692) | Agents LLM, théorie des jeux sur le métagame | Hors sujet |

### 5bis. LE SEUL CONCURRENT DIRECT — [rafiimanggala/ygo-combo-solver](https://github.com/rafiimanggala/ygo-combo-solver)

1 940 lignes de Python, **2 commits**, 11 juin 2026, 0 étoile, macOS. Même
substrat (ocgcore par ctypes), même cadrage solitaire (adversaire = 10 vanilles),
même déterminisme (`DUEL_PSEUDO_SHUFFLE` + graine). Son README énonce notre
propre conclusion : *« points can't find a combo — only rank a board »*.

**Les plafonds, et ce sont des murs d'architecture :**

| | eux | nous |
|---|---|---|
| coût d'un nœud | **rejeu complet du préfixe** depuis un duel neuf | arène, restauration 0,05 ms |
| budget par défaut | **500 nœuds**, `max_depth = 30` | 10⁵–10⁶ états ; plan résolu à **283 décisions** |
| recherche | DFS récursif, ordre d'enfants fixe, mono-thread | NRPA + archive + LTS, 16 workers |
| but | maximiser un score à table de passcodes curée | **board cible**, ou preuve d'absence |

**Trois défauts qui sont des bugs, pas des choix :**

1. **La sélection multi-cartes n'est pas un point de branchement.**
   `_parse_select_card` ne branche que si `max == 1` ; le reste tombe sur
   `pick first min`. **Choisir les matériaux d'une Fusion leur est
   structurellement impossible** — exactement l'arité de nos sessions 15-17.
2. **`MSG_SELECT_YESNO` / `EFFECTYN` → toujours OUI**, jamais branché. §9.28 (c) :
   le pivot du combo Lunalight vit sur un prompt oui/non.
3. **Clé de transposition NON SONORE** : `signature()` trie des `(code, position)`
   et omet l'extra deck (lu, jamais mis dans la clé), le deck, les **matériaux
   Xyz** (jetés explicitement), les compteurs, la **séquence de zone**, et tout
   état de chaîne. Avec un ensemble fermé global et un `return` sec, cela
   **élague des lignes réelles en silence** — la classe de défaut que §9.24 (j)
   a mesurée chez nous.

**Ce qu'il ne faut PAS croire (vérifié, session 20).** Leur note de build « Lua
compilé en C++ pour que les erreurs remontent en exceptions au lieu de `longjmp`
par-dessus les destructeurs » n'est pas un acquis à reprendre : **notre build le
fait déjà** ([premake5.lua:74](../premake5.lua#L74), `compileas "C++"`, même
justification en commentaire). Et **ce n'est pas une optimisation** : le chemin
sans erreur d'une exception C++ est piloté par tables (coût nul), le `setjmp`
qu'elle remplace coûtait une sauvegarde de registres à *chaque* appel protégé, et
le chemin de lancement est au contraire plus cher. Neutre sur le débit, requis
pour la correction.

Reste **une** chose à leur prendre : leur décodeur de `OCG_DuelQueryLocation`
**parse par taille d'enregistrement**, donc un champ de requête inconnu est sauté
sans casser. Le nôtre est audité champ par champ (19/19, §9.25 (c)) — plus
correct, plus fragile à une mise à jour du core.

*Verdict* : prototype Phase 1 honnête qui valide notre cadrage et **s'arrête là
où nos difficultés commencent**. Le risque compétitif est la **visibilité**, pas
la capacité.

*Réserve dite franchement* : le texte intégral de *Strategic Sorcery* n'a pas pu
être lu (OpenReview derrière une vérification de navigateur). Le résumé, le
choix d'ENHSP et la mention « numeric planning » sont attestés par deux sources
indépendantes ; le détail de l'encodage ne l'est pas.

---

## 6. OÙ BRANCHER UNE HEURISTIQUE DANS UN ÉCHANTILLONNEUR

**Keller & Helmert, *Trial-based Heuristic Tree Search for Finite Horizon MDPs*,
ICAPS 2013** ([PDF](https://gki.informatik.uni-freiburg.de/papers/keller-helmert-icaps2013.pdf)),
puis Schulte & Keller (2014) pour la planification classique.

THTS **subsume** MCTS, la programmation dynamique et la recherche heuristique
par cinq ingrédients séparables : *fonction heuristique, fonction de remontée,
sélection d'action, sélection d'issue, longueur d'essai*. Deux observations du
travail de 2014 valent pour nous :

1. en planification classique, **le tirage ne termine presque jamais** (buts
   rares) — d'où la longueur d'essai comme paramètre à part entière ;
2. une file ouverte arborescente absorbe les réordonnancements et implémente A*
   comme GBFS.

Intérêt immédiat : cela donne le **vocabulaire** pour dire où `h^SEQ` entre dans
notre architecture (initialisation de valeur au nœud, pas remplacement de la
politique) et pour ne pas confondre trois modifications en une.

---

## 7. CE QUE LA LITTÉRATURE RÉFUTE D'AVANCE

- **Réparer `--recipe-w`.** Toute variante de `h^add` sur le graphe ET/OU
  reproduira le contresens de §9.24 (d), parce que la relaxation par suppression
  suppose qu'atteindre un sous-but ne détruit rien. Ce n'est pas un réglage.
- **Commencer par red-black planning** : conditions structurelles non tenues,
  variance ×100 mesurée par les auteurs, cadran de peinture à balayer.
- **Espérer une garantie de complétude** : `Π¹₁`-complet (arXiv:2603.02863).
- **Attendre qu'un travail YGO publié nous serve de repère** : il n'y en a
  aucun ; le voisin le plus proche est une démo MTG en planification numérique.
- **Croire qu'un but à peu d'atomes rendra un LP fort** : Bonet le dit
  explicitement, et donne le correctif (reformulation, safeness, landmarks).

---

## 8. CONSÉQUENCES POUR LA SESSION 20

L'ordre des chantiers du prompt **ne change pas** — le chantier D (un seul point
de câblage) reste premier, parce qu'aucune mesure ne vaut tant que trois
`SearchConfig` divergent. Mais le chantier B change de **nature** :

> Il n'était formulé que comme « brancher la matière déjà extraite ». Il a
> maintenant une **forme fermée** (l'équation de bilan), un **critère de
> correction** (faisabilité), un **produit dérivé** qui alimente le chantier A
> (les comptes de tir `x*`), et une **première marche à coût nul et sans
> solveur** (la relaxation par ligne).

Trois prédictions falsifiables à poser **avant** de coder, dans la discipline du
dossier :

1. *Vie* — sur l'étalon A nu, le test de faisabilité par ligne doit déclarer
   morts **un nombre non nul et non total** de tirages. À 0 %, le mécanisme est
   inerte ; à 100 %, le modèle de consommation est faux.
2. *Structure* — la décomposition imprimée avec bilan matière doit exiger la
   Fusion **trois fois** et neuf corps Lunalight, contre « 3 monstres Lunalight »
   cardinal aujourd'hui. Juge à **0,2 s**, sans un seul tirage (§9.28, les juges
   gratuits).
3. *Recherche* — et seulement ensuite : étalon A nu, la colonne « Kaleido Chick
   — renommage activé » contre son goulot mesuré ; puis étalon B, N runs, lecture
   en proportion.

---

## 9. BIBLIOGRAPHIE (ce que cette revue AJOUTE au dossier)

| réf. | ce qu'on en prend |
|---|---|
| Bonet, IJCAI 2013 — *An Admissible Heuristic for SAS+ Planning Obtained from the State Equation* | **La formulation centrale.** `h_SEQ = ⌈min c^T x⌉` s.c. `A^T x ≥ M_but − M_s`, `x ≥ 0` ; infaisable ⇒ impasse prouvée ; safeness, landmarks, reformulation du but |
| Pommerening, Röger, Helmert & Bonet, ICAPS 2014 — *LP-Based Heuristics for Cost-Optimal Planning* | Le cadre **operator-counting** ; les quatre classes AP/SP/AC/SC ; réunion de contraintes ⇒ domine le max des composantes |
| Coles, Coles, Fox & Long — *A Hybrid LP-RPG Heuristic…* (arXiv:1402.0564) | La classe **producteur-consommateur** ; MIP pour les ressources + RPG pour le causal. Marche 3 |
| Domshlak, Hoffmann & Katz, AIJ 2015 — *Red-Black Planning* | **Pourquoi ne PAS commencer par là** : DAG causal noir + inversibilité, variance ×100 |
| Drexler, Seipp & Geffner, ICAPS 2022 (arXiv:2203.14852) — *Learning Sketches…* | La **forme** d'une sérialisation : règle `C ↦ E`, `E` = décroissance d'un trait ; `SIW_R` |
| Lipovetzky, Muise & Geffner, ICAPS 2016 — *Traps, Invariants, and Dead-Ends* | Impasses **statiques**, calculables une fois par deck, en k-DNF |
| Junghanns & Schaeffer, AIJ 2001 — *Sokoban…* | Tables de blocage, coupes de pertinence : le précédent pratique en domaine à consommation irréversible |
| Keller & Helmert, ICAPS 2013 — *THTS* | Les cinq ingrédients séparables ; où brancher une heuristique sans toucher à la politique |
| Chen & Thiébaux (arXiv:2404.05235) — *Novelty Heuristics… for Numeric Planning* | Plus tard : indexer la table de nouveauté sur des **compteurs**, pas des atomes |
| Aso-Mollar, Aineto, Scala & Onaindia (arXiv:2512.22367) | Sous-buts en planification numérique, école Scala (ENHSP) |
| Nicolosi, Pisciotta & Bresolin (arXiv:2603.02863) | Yu-Gi-Oh! est `Π¹₁`-complet : aucune complétude à espérer |
| Tillo, Rehal & Muise, démo ICAPS 2024 — *Strategic Sorcery* | Le seul travail publié voisin : MTG en **PDDL numérique**, ENHSP |
| Churchill, Biderman & Herrick (arXiv:1904.09828, 2003.05119) | Complexité de MTG |
| Hoffmann — *Where Ignoring Delete Lists Works* (arXiv:1109.5713) I & II | La taxonomie qui dit **quand** `h^+` suffit — et notre domaine n'y est pas |
| [ygo-agent](https://github.com/sbl1996/ygo-agent) | Substrat identique (ocgcore), objectif orthogonal (duel complet en RL) |
