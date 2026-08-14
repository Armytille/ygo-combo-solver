# État de l'art : atteindre un board donné sans ligne de référence

Revue menée en session 8, pour le **mode BUT SEUL** — atteindre un board depuis
une decklist, sans replay à imiter. C'est le premier chantier du projet dont la
réponse n'était pas dans le dépôt.

Les papiers marqués **[lu]** ont été lus dans le texte (dérivations, protocole
expérimental, tables de résultats, sections d'échec) ; les autres n'ont été vus
qu'au résumé et servent de bornes de champ. La distinction est faite partout :
une transposition ne se décide pas sur un résumé.

---

## 1. La question, posée avec les grandeurs du dépôt

> Donner de la masse à une séquence de 300-500 décisions vers un but
> CONJONCTIF, sans démonstration à imiter, dans un simulateur sans modèle
> déclaratif.

Deux mesures antérieures cadrent toute lecture de la littérature, et il faut
les avoir en tête pour ne pas retenir un mécanisme dont on sait déjà le signe :

- **§9.14 — le mur est la MASSE, pas le classement.** La politique classe le bon
  coup premier 96 % du temps (plafond calculé 96,1 / 97,3 %) mais ne lui donne
  que 44 à 74 % de probabilité. Sur 160 décisions, 0,74¹⁶⁰ ≈ 10⁻²¹. *Tout
  mécanisme qui re-pondère les coups est prédit neutre* (piège 45).
- **Le paysage est PLAT.** L'unique signal de progression disponible — le nombre
  de cartes du board cible posées — vaut zéro sur ~90 % de la ligne Lunalight
  (le premier monstre du board tombe à l'étape 76 sur 108).

Ces deux faits ensemble éliminent d'emblée une grande part de la littérature :
tout ce qui suppose une heuristique informative en tout point, et tout ce qui
améliore un classement.

---

## 2. Le jumeau publié du domaine — et il existe

La découverte la plus utile de la revue n'est pas un algorithme, c'est une
**correspondance de domaine**. Tuero, Buro, Orseau & Lelis décrivent leur
banc *CraftWorld* ainsi :

> « L'agent collecte des matériaux bruts et interagit avec des établis et des
> fourneaux pour fabriquer des objets intermédiaires, eux-mêmes transformables
> en produits finaux. **L'environnement peut se retrouver en impasse si l'on
> fabrique le mauvais objet, les objets étant consommés dès qu'ils entrent dans
> une recette.** » (arXiv:2605.30664 §4.2) **[lu]**

C'est le combo Yu-Gi-Oh en une phrase : matériaux consommés, recettes
emboîtées, impasse silencieuse par consommation d'une pièce. Les chiffres de ce
domaine deviennent donc des ordres de grandeur crédibles pour nous, ce qu'aucun
résultat sur Sokoban ou le taquin ne pouvait être.

| CraftWorld (hard), expansions à la solution | |
|---|---|
| LTS(π^SG) — sous-buts VQ-VAE explicites | 345 096 |
| LTS | 306 224 |
| √LTS-L (rerooter par grappes, Leiden) | 8 803 |
| **√LTS-H (rerooter heuristique)** | **2 515** |
| PHS*(π^SG) | 1 413 |
| √LTS-LH (hybride L + H) | 1 348 |

Trois lectures. (a) La famille du **rerooting** est la bonne : ×122 sur LTS pour
-H, ×227 pour l'hybride, avec un mécanisme qui ne coûte presque rien. (b) Entre
les deux signaux, **l'heuristique porte l'essentiel** (2 515 contre 8 803 pour
les grappes) — et c'est le moins cher des deux à écrire, puisqu'il ne demande
aucun clustering du graphe développé. (c) La génération EXPLICITE de sous-buts
(VQ-VAE) est *pire que LTS* sur ce domaine — avertissement direct contre la
variante de la piste 5 qui consisterait à fabriquer des états sous-buts et à
raisonner dessus.

---

## 3. Les six directions

### Direction 6 — √LTS, et pourquoi notre implémentation n'en est pas une

**Papiers.** Orseau, Hutter & Lelis, *Exponential Speedups by Rerooting Levin
Tree Search* (arXiv:2412.05196) ; Tuero, Buro, Orseau & Lelis,
*Structure-Induced Information for Rerooting Levin Tree Search*
(arXiv:2605.30664) **[lu]**.

**Le mécanisme, tel que l'article le définit.** Le coût est

    c^r(n) = min_{n_t ≺ n} (1/w_t) · c^r_{n_t}(n),
    c^r_{n_t}(n) = Σ_{n_t ≺ n' ⪯ n} 1/π(n' | n_t)

— un **minimum sur TOUS les ancêtres**, chacun pondéré par son poids de
re-enracinement w_t. Le rerooter est ce qui fabrique les w_t.

**Ce que fait notre `--reroot` (session 7ter), et où il s'écarte.** Il ne
considère qu'un seul ancêtre — le plus proche portant un INDICE — avec des poids
uniformes. C'est un rerooter **dur** : un nœud est un point de re-enracinement,
ou il ne l'est pas. L'article, lui, mesure trois rerooters **doux**, tous à poids
partout non nuls : par grappes (Leiden), par heuristique, et leur somme.

**Le diagnostic mécanique de l'échec de la session 7ter, indépendant du régime.**
Notre indice est « le nombre de cartes du board cible posées a changé ». Sur le
cas Lunalight cet événement ne tombe pas avant l'étape 76 sur 108 ; sur la bande
précoce du cas synchron, jamais. *Sans indice, il n'y a aucun point de
re-enracinement, et √LTS dégénère exactement en LTS.* Le rerooter dur est donc
structurellement incapable de mordre dans un paysage plat — ce qui est
précisément notre paysage. L'A/B perdant de la session 7ter avait deux causes,
pas une : le mauvais régime *et* le mauvais rerooter.

**La transposition retenue : √LTS-H** (Eq. 7 de l'article),

    w_t = exp(−α · h(n_t) / h(racine))

avec h = cartes cibles manquantes + résolutions manquantes, déjà calculé à chaque
nœud du finisseur (`hgoal`). Aucun réseau, aucune grappe, aucun sous-but
reconstruit.

**Ce que α veut dire chez nous, et il se calibre sans dépenser un run.** À h
constant (paysage plat), tous les 1/w valent e^α ; le re-enracinement sur le
parent l'emporte dès que le coût accumulé du segment dépasse ~e^α. Donc **α est
le logarithme du coût toléré par segment**. Or `ForecastSearchCost` avait déjà
chiffré ce coût : 10^5,8 à 10^12,5 par segment, soit **α ∈ [13,4 ; 28,8]**. Le
cadran d'A/B en découle (8 / 15 / 25) au lieu d'être deviné — piège 40 respecté.
Quand h varie, les segments se cassent préférentiellement aux points de
progression : le comportement voulu, avec dégradation gracieuse là où le
paysage est plat. C'est exactement ce que le rerooter dur ne sait pas faire.

**Une propriété de bord, gratuite.** Le min borne le coût par e^α/π_min : le
mécanisme est **numériquement stable par construction**, là où d/π déborde. Le
commentaire de `search.h` notait ce débordement comme la raison de la forme
retenue en 7ter ; le rerooter doux le règle sans artifice.

**Coût.** ~40 lignes, récurrence en O(1) par nœud (deux candidats : prolonger le
meilleur ancêtre courant, ou se re-enraciner sur le parent). Écart assumé avec
le min exact sur tous les ancêtres : unilatéral (on ne sous-estime jamais) et
majoré par le terme de re-enracinement, qui est toujours disponible.
**IMPLÉMENTÉ cette session (`--reroot-h`), mesuré §9.15.**

### Direction 5 — Rejeu rétrospectif (hindsight) : le papier qui prédit notre échec

**Papiers.** Ståhlberg & Geffner, *First-Order Representation Languages for
Goal-Conditioned RL* (arXiv:2512.19355) **[lu]** ; Tuero, Buro & Lelis,
*Subgoal-Guided Policy Heuristic Search with Learned Subgoals*
(arXiv:2506.07255) **[lu]** ; Andrychowicz et al. (HER, cité) ; Rauber et al.
(arXiv:1711.06006, résumé).

**Le mécanisme.** Ståhlberg & Geffner transposent HER à la planification et
comparent trois relabellisations : *state HER* (le but devient l'état final
atteint), *propositional HER* (**le but devient le plus grand SOUS-ENSEMBLE du
but original satisfait à la fin**), *lifted HER* (la version liftée du
sous-but). Résultats : 51,0 % / **82,4 %** / 78,6 % de couverture. Le sous-ensemble
du but bat l'état complet de 31 points. Et le relabeling **fabrique tout seul un
curriculum** : taille de but et longueur de trajectoire croissent au fil de
l'entraînement.

La transposition est immédiate et sans réseau : notre but est un MULTI-ENSEMBLE
de cartes ; un tirage qui pose k des n cartes cibles est une **démonstration
réussie pour le sous-but à k cartes**. C'est très exactement ce que l'archive
Go-Explore stocke déjà (`best_path` par cellule) et n'a jamais servi comme
démonstration.

**Mais — et c'est le résultat le plus utile de toute la revue — le papier
prédit notre échec sur l'étalon A, avant qu'on le mesure.** Leur domaine
*Delivery* :

> « Tous les colis doivent être livrés au MÊME endroit, mais le camion n'en
> porte qu'un à la fois. L'exploration aléatoire livre parfois un colis, mais la
> probabilité d'en livrer plusieurs au même endroit dans une seule trajectoire
> est extrêmement faible. **Résultat : les modèles apprennent à livrer un seul
> colis et ne généralisent pas à la tâche complète.** » (couverture 11-12 %.)

Notre étalon A vise **3× Lunalight Liger Dancer**. Notre meilleur résultat
mesuré : **un** Liger, jamais deux. Même forme de but (n exemplaires de la même
chose, ressources partagées), même échec, même chiffre. *Le HER propositionnel
seul ne débloquera pas l'étalon A* — et on le sait sans dépenser un run.

**Le second avertissement, sur le SIGNE.** Leur mode d'échec *Childsnack* :

> « Toute trajectoire finissant en cul-de-sac est relabellisée en succès pour le
> but relabellisé. […] Il n'y a donc aucune donnée d'entraînement sur la façon
> d'ÉVITER les culs-de-sac. »

Chez nous, une ligne qui brûle une pièce nécessaire vingt-cinq étapes plus tard
est exactement cela. Un répertoire rétrospectif non filtré **apprendrait à foncer
dans le mur** : mécanisme prédit NÉGATIF sans filtre de viabilité. Le filtre
existe déjà en pièces détachées (coupure de tour, comptabilité des brûlées,
`--resolve`) ; c'est le premier travail à faire si l'on implémente cette piste.

**Ce que 2506.07255 ajoute côté RECHERCHE (et pas RL).** Il apprend ses sous-buts
depuis les arbres d'ÉCHEC : clustering Louvain du sous-graphe développé, tirage
de paires (s_cur, s_tar) dans des grappes voisines, la trajectoire entre elles
devenant la démonstration de la politique bas-niveau. Chez nous, le clustering
est inutile : **le partitionnement est déjà donné** par l'indice de progression
et par les cellules d'archive. Mais son résultat mesuré est un avertissement
(cf. §2) : la génération explicite de sous-buts perd contre le rerooting.

**Coût.** Relabeling depuis l'archive : ~80 lignes (`LiftPolicyRun` existe déjà
et fait le relevé ; il faut la sélection du plus grand sous-but atteint et le
filtre de viabilité). **NON implémenté cette session** — chantier 12, avec son
signe prédit et son filtre obligatoire.

### Direction 1 — Recherche en arrière et bidirectionnelle : écartée sur STRUCTURE

**Papiers (résumés).** Chen, Holte, Zilles & Sturtevant, NBS (arXiv:1703.03868) ;
Siag, Shperberg, Felner & Sturtevant, PEM-BAE\* (arXiv:2412.21104) ; Shperberg et
al., BAE\* borné-suboptimal (arXiv:2511.10272) ; Shubi et al.
(arXiv:2606.05956) ; Nayak & Otte, GBRRT (arXiv:2010.14692).

C'était la direction placée en tête par le cahier des charges, et elle est
**écartée, pour une raison qui n'est pas un goût**. Tous ces algorithmes — sans
exception dans ce qui a été trouvé — exigent une **fonction de prédécesseurs**,
c'est-à-dire la capacité de développer un nœud vers l'arrière. `ocgcore` n'en a
pas et ne peut pas en avoir : un état de duel est un tas Lua plus une pile de
résolution plus des compteurs « une fois par tour » ; « l'état d'avant » n'est
pas calculable, seulement re-simulable depuis la racine. Le seul travail trouvé
qui s'en passe (GBRRT : l'arbre inverse ne sert que d'heuristique, jamais de
jonction) construit tout de même un arbre inverse, donc échantillonne toujours
en arrière depuis le but.

Le coût réel de cette direction est donc « écrire un modèle inverse du duel »,
c'est-à-dire un projet à part entière, à comparer à ~40 lignes pour √LTS-H. Le
substitut bon marché de l'intuition qui la motivait (« Liger exige Leo Dancer,
donc Leo Dancer doit être au cimetière ») n'est pas la recherche
bidirectionnelle : c'est le **graphe de recettes** de la direction 2.

**Les deux hybrides qui, eux, sont réalisables** (question posée en séance) :

- **Inverse au niveau des CARTES, pas des états.** Le core dit quels matériaux
  une invocation a consommés ; la relation « Liger ← Leo Dancer(cimetière) +
  Lunalight » s'apprend donc depuis les tirages, sans modèle déclaratif. On ne
  s'en sert pas pour chercher en arrière — on s'en sert pour fabriquer un `h`
  qui DÉCROÎT en cours de route. Et c'est là que la boucle se referme : le
  rerooter √LTS-H est piloté par `h`. Avec le `h` actuel (cartes cibles posées),
  plat sur 90 % de la ligne, il ne peut casser les segments qu'au coût accumulé ;
  avec un `h` dérivé des recettes, il les casse aux vraies étapes du combo.
  **Le graphe de recettes est ce qui ARME le rerooter** — les deux chantiers
  n'en font qu'un.
- **Rencontre au milieu sans inversion.** On ne sait pas inverser, mais on sait
  re-simuler, et la table de transposition indexe déjà les états par digest. Une
  bibliothèque de SUFFIXES (« depuis cet état, ces 12 coups posent un Liger »),
  consultée pendant les tirages, raccorderait une ligne avant sur un morceau de
  fin déjà connu sans jamais développer un prédécesseur. C'est le symétrique
  exact de `--approach`, qui ne sait aujourd'hui rejouer que des PRÉFIXES ; et
  c'est la version « segment » du rejeu rétrospectif de la direction 5.

### Direction 2 — Heuristiques dérivées du but : partiellement écartée, un reste vivant

**Papiers (résumés).** Hanou, Dumančić & de Weerdt, *Revisiting Landmarks*
(arXiv:2508.21564) ; Meneguzzi et al. (arXiv:2404.07934).

Toute la planification classique dérive son guidage du but par relaxation
(h_add, h_max, h_FF, LM-cut) ou par comptage d'opérateurs en PL/PLNE. **Cela
exige un modèle déclaratif des actions.** Nous n'en avons pas : nos « actions »
sont des scripts Lua arbitraires. Le portage de ces heuristiques suppose donc
d'écrire un modèle PDDL du jeu — écarté au même titre que la direction 1.

Vérification faite, arXiv:2404.07934 n'est pas le papier de bornes inférieures
par comptage d'opérateurs que le cahier des charges supposait : c'est de la
*reconnaissance de but* utilisant ce cadre. La piste « borne inférieure LP/IP
pour trancher 18 brûlées » reste donc sans papier de référence identifié.

**Ce qui reste vivant, et c'est réel.** Les matériaux de Fusion / Xyz / Synchro /
Link **sont observables à l'exécution** : quand une carte est invoquée, le core
dit quels matériaux ont été consommés. Un graphe d'exigences « carte → matériaux
effectivement consommés » s'apprend donc **depuis les tirages, sans modèle
déclaratif et sans texte de carte**, et donne des landmarks au sens de
2508.21564 (dont l'apport propre est justement d'apprendre les landmarks depuis
des plans résolus plutôt que de les extraire d'un modèle). Ce serait notre seul
accès à un raisonnement « en arrière ». Coût estimé : ~150 lignes plus un
passage sur l'énumérateur. **Chantier 13, non fait.**

### Direction 3 — Largeur sérialisée et sketches : retenue comme CADRE, pas comme code

**Papiers.** Bonet & Geffner, *General Policies, Subgoal Structure, and Planning
Width* (arXiv:2311.05490) ; Drexler, Seipp & Geffner (arXiv:2105.04250 et
arXiv:2203.14852) ; Bonet, Drexler & Geffner (arXiv:2403.16824) — tous au résumé.

Le cadre dit la chose juste : un but conjonctif est résoluble en temps
polynomial *dès lors qu'on dispose d'un sketch de largeur bornée*, et un sketch
est une décomposition en sous-buts. Nos `--resolve` et `--hint` **sont** un
sketch écrit à la main ; `novelty_serialize` en est un cas particulier non
théorisé. C'est la bonne grille de lecture, et elle donne la phrase de synthèse
de cette revue (§4).

Mais l'apprentissage automatique de sketches (Drexler et al.) passe par un
encodage ASP sur un domaine PDDL et un jeu de petites instances du même domaine.
Nous n'avons ni PDDL, ni petites instances, ni domaine au sens où ils l'entendent
(chaque deck est un domaine). **Non transposable en l'état** ; retenu comme
vocabulaire.

### Direction 4 — Go-Explore : une correction de trois lignes, jamais faite

**Papiers (résumés).** Ecoffet et al. (arXiv:1901.10995) ; Gallouédec &
Dellandréa, *Cell-Free Latent Go-Explore* (arXiv:2208.14928) ; Höftmann et al.
(arXiv:2301.05635) ; Bhatt et al. (arXiv:2601.00042).

Le diagnostic du cahier des charges est confirmé par la littérature : Go-Explore
« échoue complètement à explorer si le partitionnement en cellules n'est pas
assez informatif » (2208.14928). Notre archive est indexée par **board complet** :
partition quasi injective, donc quasi-doublons — le cas d'échec nommé par
l'article. LGE y répond par une représentation latente apprise ; nous n'en avons
pas besoin, car **le descripteur grossier existe déjà dans le code** :
`ContextKey(cartes cibles posées, cartes en main)`, écrit en session 7bis pour
la politique à deux niveaux, 20 valeurs distinctes mesurées sur le corpus.
Ré-indexer l'archive dessus est une modification de quelques lignes.

Prédiction de signe (piège 45) : c'est un levier de **diversité des racines du
finisseur**, pas de masse. Attendu : plus de racines distinctes, moins de
racines redondantes ; l'effet sur les conversions reste à mesurer.
**Chantier 14, non fait.**

**Confirmation méthodologique tierce, à verser au piège 39.** L'étude
Go-Explore de Bhatt et al. (arXiv:2601.00042) conclut : « la variance de graine
domine les paramètres algorithmiques, avec un facteur 8 d'écart ; les
comparaisons à graine unique ne sont pas fiables ». C'est notre discipline de
dispersion, établie indépendamment sur un autre domaine.

### Hors des six directions — la fonction de perte, une piste non anticipée

**Papier.** Orseau, Hutter & Lelis, *Levin Tree Search with Context Models*
(arXiv:2305.16945) **[lu : §1-3, la dérivation de la perte]**.

La perte de Levin est exactement la borne du théorème 1, prise comme objectif :

    L(N') = Σ_{n ∈ N'} d(n) / π(n)

sur un ensemble de nœuds-solutions. Le papier montre qu'elle est **convexe**
sous un modèle de contexte paramétré (produit d'experts), d'où des garanties de
convergence vers l'optimum qu'un réseau ne donne pas — et LTS+CM résout le
taquin 24 et le Rubik's cube en quelques centaines d'expansions, là où LTS+NN
échoue.

**Pourquoi c'est le meilleur candidat suivant, et l'argument est chiffré.** Notre
`AdaptRun` applique le gradient NRPA : +α au coup joué, −α·p à chacun des
légaux. Il optimise « ressembler à la meilleure séquence ». La perte de Levin,
elle, pèse chaque étape par d/π — donc **par l'inverse de la probabilité déjà
accordée**. Les étapes à p ≈ 0 y pèsent des ordres de grandeur de plus que les
autres. Or ce sont *exactement* celles que §9.14 a désignées : la moyenne
géométrique s'effondrait à 44 % alors que le classement était à 96 %, parce
qu'« une poignée d'étapes à p ≈ 0 » écrasait le produit. Le gradient NRPA ne les
distingue pas ; la perte de Levin les cible en priorité. C'est un levier de
**MASSE** par construction (piège 45), et le désaccord entre ce qu'on optimise et
ce qui coûte est mesuré, pas supposé. Une politique par poids sur `plan_key` est
par ailleurs un modèle de contexte dégénéré (un seul contexte), donc déjà dans
le cadre du papier. **Chantier 15, non fait, candidat de tête pour la suite.**

### Direction 7 (hors cahier des charges) — LE COMBO EST UNE RÉTROSYNTHÈSE

**Papier.** Yu, Roh, Li, Gao, Wang & Coley, *Double-Ended Synthesis Planning with
Goal-Constrained Bidirectional Search* (arXiv:2407.06334) **[lu : §1-3]**. Voisinage : Retro\*
(Chen et al. 2020), GRASP (RL + HER pour l'estimation de valeur conditionnée au but),
Tango\* (arXiv:2412.03424), PDVN (arXiv:2301.13755).

La littérature qui traite notre problème de front n'est pas la planification classique : c'est
la **planification de synthèse chimique assistée par ordinateur**, et précisément sa variante
*sous contrainte de matériaux de départ*. La correspondance est terme à terme, pas analogique :

| CASP | combosolver |
|---|---|
| molécule cible `p*` | board cible |
| briques achetables `B` | cartes du deck |
| **matériau de départ imposé `r*`** | **main d'ouverture** (elle DOIT être jouée) |
| réaction (réactifs → produit) | invocation (matériaux → monstre) |
| route synthétique = DAG | ligne de combo |
| impasse par consommation | pièce brûlée au mauvais moment |

Et la contrainte 3 de l'article — « `r*` est utilisé et n'est produit par aucune réaction » —
est mot pour mot notre situation : la main de départ est consommée, jamais fabriquée.

**Ce que l'article apporte que rien d'autre n'apportait.**

*Le graphe ET/OU.* Les molécules sont des nœuds **OU** (une seule voie suffit), les réactions
des nœuds **ET** (tous les réactifs doivent être obtenus). C'est exactement la structure du
combo Lunalight : « Liger Dancer » est un nœud OU à trois voies (Polymérisation ordinaire,
Polymérisation qui bannit du cimetière, Wolf en zone Pendule), chacune un nœud ET sur ses
matériaux. **Nous ne représentons rien de tout cela** : nous cherchons dans un espace d'actions
séquentiel plat, où cette structure existe mais n'est jamais nommée.

*Pourquoi la rétrosynthèse, elle, peut chercher en arrière — et pourquoi nous le pouvons
aussi, à ce niveau-là.* Elle dispose de **templates rétro** : une fonction produit → précurseurs.
La direction 1 était écartée parce qu'on ne sait pas inverser un ÉTAT DE DUEL. Mais on n'a pas
besoin de l'inverser : il suffit d'inverser une INVOCATION, et là les règles du jeu donnent le
template gratuitement — Fusion : les matériaux nommés par le texte ; Synchro : un Syntoniseur
plus des non-Syntoniseurs de somme de niveaux imposée ; Xyz : n monstres de même niveau ;
Lien : n monstres. Notre « modèle rétro à un pas » est donc **plus facile** que celui de la
chimie, qui exige un réseau entraîné sur 12 millions de réactions. Et il est de surcroît
*observable* : chaque invocation vue dans un tirage en fournit un exemple vérifié.

*La distance synthétique `D(m₁, m₂)`* — coût de fabriquer m₂ **à partir de m₁ précisément** —
est apprise HORS LIGNE depuis des routes déjà connues, « sans passer par de l'auto-jeu en
apprentissage par renforcement », en remarquant que dans toute route, *n'importe quelle
molécule non-racine est un matériau de départ pour la cible*. C'est le rejeu rétrospectif de
la direction 5, dans un autre champ et sur des données statiques. Ils échantillonnent aussi
des paires **négatives** (aucun chemin n'existe) — le signal d'impasse qui manquait exactement
à Ståhlberg & Geffner.

**La transposition, et c'est le programme le plus ambitieux que la revue ait produit.**
La recherche reste **unidirectionnelle dans le simulateur** — on ne peut pas reculer dans
`ocgcore` — mais devient **bidirectionnelle dans le graphe de recettes** :

- côté descendant (rétro), on développe le graphe ET/OU depuis le board cible : Liger exige
  Leo Dancer au cimetière, donc Kaleido Chick doit l'y envoyer, donc… ;
- côté montant, c'est le simulateur lui-même qui avance ;
- la jonction n'est pas une jonction d'états mais une **évaluation** : `D(état courant, cible)`
  lue sur le graphe de recettes donne un `h` qui **décroît en cours de ligne**.

C'est la variante F2E de DESP, où le côté descendant est symbolique et le côté montant est le
simulateur. Et cela referme la boucle de toute la revue : ce `h` est précisément ce qui manque
au rerooter √LTS-H (direction 6), dont les poids sont une fonction de `h`, et ce qui rendrait
non plat le paysage qui bloque tout depuis trois sessions.

**L'objection qui casserait le modèle si on s'y prenait mal, et les trois règles qui
l'absorbent.** Une Fusion ne consomme pas des CARTES : elle consomme des entités qui
*satisfont une exigence de nom* à cet instant. Kaleido Chick ayant copié le nom de Leo Dancer
**est** Leo Dancer pour la Fusion ; les matériaux peuvent venir du cimetière, de la zone
bannie ou de la zone Pendule ; des substituts de Fusion existent. Un dictionnaire
carte → matériaux est donc faux par construction. Trois règles, et le modèle tient :

1. **Le nœud OU est une EXIGENCE, pas une carte** : « un monstre de nom Leo Dancer », « un
   Lunalight au cimetière ». Le changement de nom n'est pas une exception, c'est une arête de
   plus vers le même nœud OU : « Kaleido Chick (effet, cimetière) → fournit le nom Leo
   Dancer ». Les substituts s'y branchent pareillement. Et la ZONE entre dans le nœud — ce
   n'est pas à inventer : nos atomes de nouveauté sont déjà des triplets
   `(zone, carte, occurrence)`, et `QueriedCard::Code()` distingue déjà le code physique de
   l'alias effectif, avec en commentaire « et les effets de changement de nom ».
2. **Le graphe ne PRUNE jamais, il ne fait que pondérer.** S'il servait d'oracle
   (« cette Fusion est impossible »), une voie non modélisée supprimerait des solutions en
   silence — la forme exacte du piège 47. Usage strictement heuristique : il alimente `h`,
   jamais une coupure. Une recette manquante coûte alors de l'efficacité, jamais la
   complétude ; au pire `h` redevient le `h` plat d'aujourd'hui et on n'a rien perdu.
3. **Le texte n'est qu'une amorce ; la vérité vient de l'observation.** Chaque invocation
   exécutée dit quelles entités ont été consommées, depuis quelles zones, sous quels noms
   effectifs. Le fournisseur « Kaleido Chick sous le nom Leo Dancer » s'apprend au premier
   tirage qui l'emploie. C'est exactement la position de DESP, dont l'hypergraphe est
   explicitement *partiellement observé* et dont le modèle à un pas est un prédicteur top-n,
   pas un oracle : la chimie a le même problème (substituts, groupes protecteurs, réactions
   non répertoriées) et y répond en apprenant un COÛT, pas une POSSIBILITÉ.

**Un corollaire bon marché, à faire AVANT le graphe complet — et son piège.** Le board cible
seul impose une arithmétique : 3× Liger Dancer, c'est **trois invocations Fusion**. C'est un
argument de COMPTAGE, dans l'esprit du comptage d'opérateurs de la direction 2 mais sans
modèle déclaratif. Il rend une borne de faisabilité plus fine que l'actuelle et un `--resolve`
DÉRIVÉ au lieu d'écrit à la main — un sketch automatique. **Mais on compte les ÉVÉNEMENTS
d'invocation, jamais leurs déclencheurs** : « trois Liger donc trois Polymérisations » serait
faux, puisque Lunalight Wolf en zone Pendule fusionne sans Polymérisation. La même erreur au
niveau du comptage qu'au niveau des recettes, et le même remède : ne jamais nommer le moyen,
seulement l'effet observable.

**Coût.** Le plus élevé de la revue — extraction des recettes, graphe ET/OU, distance sur le
graphe, branchement dans `h`. Plusieurs sessions. **Chantier 16, non fait, et c'est la
direction de fond.**

### Direction 8 (hors cahier des charges) — LES OPTIONS CHANGENT L'EXPOSANT

**Papiers.** Moraes, Sadmine, Baier & Lelis, *InnateCoder: Learning Programmatic Options with
Foundation Models* (arXiv:2505.12508) ; Alikhasi & Lelis, *Unveiling Options with Neural
Decomposition* (arXiv:2410.11262) ; Carvalho, Tjhia & Lelis (arXiv:2410.12166) — résumés.

**L'argument, et il est arithmétique.** Le mur du §9.14 est `0,74^160 ≈ 10^-21`. Tous les
mécanismes essayés depuis trois sessions travaillent sur la **base** — mieux classer, mieux
pondérer, concentrer la masse. Aucun n'a jamais touché à l'**exposant**. Or une *option* —
une action temporellement étendue — le divise directement : si « invoquer Kaleido Chick et
envoyer Leo Dancer au cimetière » est UNE action au lieu de huit décisions, une ligne de 160
décisions devient une ligne de ~20 options, et `0,74^20 ≈ 2·10^-3`. On passe de « jamais » à
« deux tirages sur mille ». Aucun autre mécanisme de cette revue ne déplace la grandeur de
dix-huit ordres de grandeur.

**Pourquoi ce domaine s'y prête particulièrement.** Un joueur ne pense pas en 160 décisions :
il pense en une dizaine de gestes qu'il sait nommer. Les options existent donc déjà dans la
tête des gens, et elles sont écrites en clair dans les guides de combo — la ligne de référence
de l'étalon A vient d'un site qui la décrit en 108 étapes REGROUPÉES. InnateCoder propose
exactement de récupérer cette connaissance depuis un modèle de fondation, en **zéro-coup**,
sans interaction avec l'environnement, puis de composer les options en programmes plus larges.
La validation est la même règle que pour le graphe de recettes : le modèle propose une macro,
le simulateur la joue ou la rejette ; une option qui ne se joue pas n'entre jamais dans la
recherche.

**Et c'est la même décomposition, pour la troisième fois.** Une option est un segment entre
deux points de re-enracinement (direction 6) ; c'est aussi une sous-trajectoire de HER
propositionnel qui atteint son sous-but à son état final (direction 5). Trois littératures
indépendantes convergent sur le même objet, chacune l'utilisant pour autre chose : chercher,
apprendre, agir. C'est le signe le plus fort que la revue ait produit sur ce qu'il faut
construire.

**Le chaînon manquant — « quelles options garder ? » — est déjà à moitié écrit chez nous.**
Alikhasi & Lelis (arXiv:2410.11262) produisent des centaines d'options candidates et les
sélectionnent en **minimisant la perte de Levin** sous politique uniforme. Or `d/π` sur le
corpus, c'est exactement ce que calcule `ForecastSearchCost` depuis la session 7ter. Le gain
d'un catalogue d'options est donc **calculable avant d'écrire la première ligne du mécanisme**,
comme α l'a été aujourd'hui (piège 40). Le critère de sélection n'est pas à inventer, il est
à brancher.

**Deux sources d'options, et elles ne servent pas le même étalon.**

- *Le minage du corpus* — Macro-FF (arXiv:1109.2154) et Castellanos-Paez et al.
  (arXiv:1610.02293, arXiv:1810.09145) extraient les macro-opérateurs des plans déjà résolus
  par fouille de sous-séquences fréquentes. Aucun texte de carte, aucun modèle de fondation :
  notre corpus de solutions suffit. Utilisable immédiatement sur l'étalon B.
- *Le modèle de fondation* (InnateCoder) — indispensable en mode BUT SEUL, où il n'y a
  précisément aucun corpus à miner. C'est la seule source de macros quand on part de rien.

Les deux se valident de la même façon : le simulateur joue la macro ou la rejette.

**Coût.** Moyen : un type d'action composite dans l'énumérateur, un catalogue d'options par
deck, la validation par le simulateur. Bien moins que le graphe de recettes complet, et les
deux se renforcent (une option est le chemin qui réalise une arête du graphe de recettes).
**Chantier 17, non fait — et à mettre en tête avec le chantier 15.**

### Deux vérifications négatives, utiles à savoir

**La famille NRPA est déjà exploitée.** Balayage du corpus Cazenave : GNRPA à température et
biais (arXiv:2003.10024) est implémenté (`--nrpa-temp`, §9.14) ; GNRPA à répétitions limitées
(arXiv:2401.10420) l'est aussi (§9.10) ; le rejeu d'adaptation (arXiv:2401.10431) également
(§9.13). La seule variante non essayée est *Stabilized NRPA* (arXiv:2101.03563), dont l'apport
annoncé est la stabilité et non la masse — donc prédit neutre par le piège 45. Il n'y a plus
de rendement facile de ce côté.

**Il n'existe pratiquement pas de littérature sur la recherche de combos en jeu de cartes.**
Recherche faite, le seul travail voisin est le *draft* de Magic (arXiv:2009.00655), qui est un
problème de sélection, pas de séquencement. Personne n'a publié sur « atteindre un board donné
en enchaînant des effets ». C'est pourquoi tout ce dépôt a dû être inventé — et c'est aussi
pourquoi le bon réflexe est d'aller chercher les domaines ISOMORPHES (rétrosynthèse,
CraftWorld) plutôt qu'un voisinage thématique qui n'existe pas.

---

## 4. Ce que la revue conclut

**Trois littératures, une seule décomposition.** Le HER propositionnel découpe
une trajectoire en sous-trajectoires « qui atteignent le sous-but seulement à
leur état final » ; √LTS découpe le chemin en segments entre points de
re-enracinement ; une *option* est une action temporellement étendue. *C'est le
même objet, vu de trois côtés* — apprendre, chercher, agir — et les trois
prennent le même signal, la progression sur le but conjonctif. Trois
communautés qui ne se citent pas convergent sur la même construction : c'est le
signe le plus fort que la revue ait produit sur ce qu'il faut bâtir.

**Trois familles de leviers, pas deux.** Le §9.14 en distinguait deux — le
CLASSEMENT (mort, il est au plafond) et la MASSE. La revue en révèle une
troisième, et elle domine : ceux qui touchent l'**EXPOSANT**. `0,74^160 ≈ 10⁻²¹`
mais `0,74^20 ≈ 2·10⁻³` : réduire le nombre de décisions d'un facteur k vaut
plus que toute amélioration réaliste de la base. Seules les options font cela.

Ordre de bataille, revu après mesure — et ce n'est pas celui du cahier des
charges :

1. **Rendre `h` informatif** — le graphe de recettes (direction 7). La mesure de
   la session 8 a établi que le verrou n'est plus l'algorithme de recherche mais
   l'heuristique : le rerooting, dur ou doux, est une fonction de `h`, et un `h`
   plat ne lui laisse rien à décomposer. Tout le reste en dépend.
2. **Les options** (direction 8) : le seul levier sur l'exposant. Sélection par
   la perte de Levin, déjà calculable avec `ForecastSearchCost`.
3. **La perte de Levin comme objectif de la politique** (2305.16945) : aligner
   ce qu'on optimise sur ce qui coûte. Levier de masse.
4. **Le répertoire rétrospectif** (HER propositionnel) **avec filtre de
   viabilité obligatoire**, en sachant qu'il ne suffira pas sur un but « n
   exemplaires de la même carte » — le papier le dit.
5. Le descripteur grossier d'archive : bon marché, effet attendu limité.
6. Le rerooting doux (√LTS-H) — *fait cette session* : implémenté, cadran
   monotone, gain marginal au bord supérieur de la fenêtre calculée. À
   re-mesurer une fois `h` informatif ; c'est le test décisif du point 1.

**Écarté sur structure, avec la raison** : recherche bidirectionnelle et
régression (pas de fonction de prédécesseurs, et aucune ne s'en passe) ;
heuristiques de relaxation et comptage d'opérateurs (exigent un modèle
déclaratif des actions) ; apprentissage de sketches (exige PDDL et petites
instances du même domaine) ; génération explicite de sous-buts par autoencodeur
(mesurée PIRE que LTS sur le domaine jumeau, §2).

**Un résultat négatif obtenu gratuitement** : le HER propositionnel ne
débloquera pas 3× Liger Dancer, parce que *Delivery* est la même instance et
échoue de la même façon, avec le même symptôme (« apprend à en livrer un seul »).
Économie : le run qu'on n'a pas eu à dépenser pour l'apprendre.
