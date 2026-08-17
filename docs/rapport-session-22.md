# Rapport de la session 22

*Consigne de séance : rendre le solveur UNIVERSEL — remplacer chaque choix
ajusté à la main par un CALCUL, et étendre la couverture du modèle pour que la
chaîne s'arme sur n'importe quel deck. Le juge : « la même commande rend h
fini, sous-buts, quotas et conversion sur l'étalon B, sans une ligne
spécifique ».*

---

## 0. Le résultat, en une phrase

**La sonde de généralité de l'étalon B rend désormais `h fini (2) + 32 unités
de sous-buts + quotas dérivés des duaux` par le MÊME binaire et le MÊME calcul
que l'étalon A** — l'extraction cardinale (Synchro/Xyz/Lien), la
spécialisation des produits d'invocation (pool + conversions), le couplage
d'ignition de Fusion et les duaux du simplexe (certifiés à chaque exécution)
ont remplacé les trois dérivations à la main de la s21 ; le témoin s21 reste
rejouable par `--quota-legacy`, et l'A/B en proportion (3 graines × 600 s par
bras) a tourné dans la séance.

---

## 1. CHANTIER 1 — l'extraction cardinale, et ce qu'elle a coûté de plus

### (a) Les comptes de matériaux (operators.cpp, ~30 lignes)

Lecture à position fixe dans la signature des `proc_*.lua` contemporains :

```
Synchro.AddProcedure(c, f1,min1,max1, f2,min2,max2, ...)  -> min1+min2
Xyz.AddProcedure(c, f, lv, ct, alterf, desc, maxct, ...)  -> ct
Link.AddProcedure(c, f, min, max, ...)                    -> min
```

Comptes seulement, jamais le niveau/type qu'on ne sait pas lire — « N
monstres » est plus faible que la vérité, h reste admissible (règle 9.30). Le
minimum quand la procédure déclare une fourchette. Et la recette retenue est
désormais la première NON VIDE (une carte Pendule pose d'abord une recette
`Pendulum.AddProcedure` vide qui masquait la vraie).

**Juge Bagooska (banc 4 buts)** : la sérialisation s'ARME — h(départ) = 14,
x* tire Bagooska, 15 sous-buts posés (s21 : « aucun sous-but posé »).

### (b) Le premier étage seul ne suffisait PAS : la sur-contrainte mesurée

Avec l'extraction seule, la sonde B rendait h(départ) = 4 **mais 22 états
h = INFINI au milieu d'une ligne qui aboutit**. Le diagnostic manquait : le
solveur dit désormais QUELLE contrainte tue (les artificielles positives de la
phase 1 nomment leurs lignes — `LPResult::infeasible_rows`, imprimées par le
banc). Verdict : « Hot Red Dragon Archfiend Abyss @TERRAIN » — Abyss quitte le
terrain (matériau) à la décision 243 et revient ranimé ; le ranimeur (Crimson
Dragon e2) filtre par `IsRace(RACE_DRAGON) and IsType(TYPE_SYNCHRO)` —
invisible aux codes et archétypes, la carte précise n'avait AUCUN producteur.

### (c) Le correctif général : pool + conversions (le choix du produit)

Un produit d'invocation met UNE carte PRÉCISE en jeu. Chaque effet à
`CATEGORY_SPECIAL_SUMMON` dépose désormais une unité dans un POOL (sa capacité
déclarée est préservée), et des conversions à coût NUL (c'est un choix, pas
une action — l'admissibilité l'exige) la spécialisent vers chaque candidat :
codes nommés ∪ monstres du deck portant l'archétype ∪ TYPE/RACE lus dans la
portée du filtre (nouveaux `fn_types`/`fn_races`, garde de début de jeton —
`EFFECT_TYPE_FIELD` contient « TYPE_ ») ; à défaut de tout vocabulaire, tout
monstre du deck. Une UNION, jamais une restriction : les codes de portée
peuvent être cités en NÉGATION (`not IsCode(CARD_CRIMSON_DRAGON)`).

**Le poison attrapé par le banc** : les ranimeurs d'archétype posaient Liger à
coût 1 — h(départ) de l'étalon A tombait de 14 à 3, l'échelle fondait
(60 → 33 unités). Le correctif est DÉCLARÉ, pas choisi : toute la famille
`AddMustBe...Summoned` (le script dit « n'entre en jeu QUE par sa
procédure ») est exclue des conversions. Leo (simple `EnableReviveLimit`)
reste convertible — optimisme légitime.

### (d) Le couplage d'ignition de Fusion

Le LP « fusionnait directement » : le quota de Wolf était invisible au plan
relaxé, donc les duaux ne pouvaient pas le désigner. Chaque `invoquer` de
recette `Fusion.AddProcMix*` consomme désormais UNE ignition ; les opérateurs
déclarés à `CATEGORY_FUSION_SUMMON` en produisent, à coût nul et à capacité
déclarée (`SetCountLimit` ; sans limite : copies, et kNoBound dès qu'une
transition peut reproduire la carte — un sort recyclable n'a pas de borne
prouvable). **Garde d'asymétrie** : zéro igniteur lisible = pas de couplage
(une colonne de production incomplète ferait d'une lacune une fausse preuve).
Étalon A : 3 igniteurs lus (Polymerization, Wolf, Lunalight Fusion), x* tire
« x3 igniter » à côté de « x3 invoquer Liger ».

### (e) L'état des bancs après le chantier complet

| | étalon A (liger.yrpX, 3 buts) | étalon B (sonde) |
|---|---|---|
| h(départ) | 13 (= l'optimum vrai du dossier, s21 §c) | **2 (fini ; avant : INFINI)** |
| théorème 2 | h 12→0, 0 chute > 1, 2 montées (capacités : th. 2 ne tient pas, th. 1/3 oui) | h 2→0, 0 chute, **0 état infaisable** (avant : 22) |
| échelle | 60 unités / 51 paliers / ℓ_max 33 — **intacte** | 32 unités / 29 paliers / ℓ_max 27 |
| auto-test | 65/65 (désormais AVEC certificat de dualité) | idem |
| santé | — | 290/290, 0 MSG_RETRY, 209 candidates, 16 replays — inchangée |

## 2. CHANTIER 2 — les duaux remplacent les choix à la main

### (a) Les duaux exposés, et PROUVÉS deux fois

`LPResult` porte `row_dual` (valeur marginale de chaque contrainte) et
`bound_dual` (valeur d'une unité de capacité en plus — `> 0` ⇔ la borne LIE le
plan), lus dans le tableau final (coût nul). Preuves :

1. **Certificat embarqué** (auto-test, 65 cas dont 60 à optima sympy exacts, à
   CHAQUE exécution) : y ≥ 0, w ≥ 0, faisabilité duale
   `c_j − Σ y_i a_ij + w_j ≥ 0`, dualité forte `b'y − u'w = c'x*`.
2. **`tools/s22_verify_duals.py`** (py -3.11, gabarit s21, piège lpmin gardé) :
   dualité forte primal = dual sur 141 instances faisables — le certificat
   suffit à l'optimalité, donc `w_j > 0` désigne bien une borne liante.

### (b) La dérivation calculée (`BalanceModel::QuotaHostsFrom`)

Un hôte à quota entre par CALCUL : transition à borne FINIE déclarée, et
(tirée par x* | saturée | dual positif | **productrice d'une place que x*
consomme**). La dernière clause est la robustesse à la dégénérescence : trois
igniteurs à coût nul sont interchangeables pour le simplexe, pas pour le
chemin. Habilitants = hôtes des transitions TIRÉES par x* qui PERSISTENT en
jeu — **Chick RESSORT par son renommage, Wolf par son ignition** ; personne ne
les a poussés. Les hôtes d'un `EFFECT_EXTRA_FUSION_MATERIAL` s'y ajoutent tant
que x* tire une ignition (l'agrégat DISPO ⊇ cimetière n'est optimiste que PAR
leur concession — hypothèse de STRUCTURE du modèle, écrite dans le code, pas
un nom de carte ; c'est le pont qui fait ressortir Masquerade).

Sur l'étalon A : `QUOTAS (duaux)` = Masquerade, Gold Leo, Serenade Dance,
Emerald Bird, Chick, Silver Hound, **Wolf**, Yellow Marten, Perfume, Tiger,
Scarlet Tiger — et le témoin s21 s'imprime à côté (diff : ±Leo Dancer,
±Purple Butterfly, ±Scarlet Tiger). Sur l'étalon B : **Starjunk Synchron,
Crimson Dragon Quetzacoatl** — dérivés, zéro ligne spécifique. Les deux
lignes de VIE s'impriment dans tous les cas ; le câblage par défaut est
DUAUX, le témoin se rejoue par `--quota-legacy`.

### (c) Le représentant à ressources fraîches (2.3)

À progrès et overlap égaux, le score d'archive préfère la cellule aux quotas
FRAIS (4 bits entre le progrès et le reste du score — dans une même cellule
les quotas sont égaux par construction de la clé : le critère n'agit que sur
l'éviction et le tournoi de ré-entrée). Fait partie du paquet « duaux » ;
`--quota-legacy` rejoue s21 à l'identique.

## 3. CHANTIER 3 — l'échelle auto-raffinante (écrite, fumée, PAS encore jugée)

`--refine-after <n>` (0 = éteint — un mécanisme est un drapeau le temps de le
mesurer) : quand le max de progrès STAGNE depuis n tirages mesurés (détecteur
dans le garde RAII de sp_final), le prochain retour au barreau vise la
MEILLEURE cellule ; le duel y est déjà à l'état voulu, le LP s'y résout (le
membre droit déjà diminué du marquage rend exactement « ce qu'il reste à
faire ») et ses places non servies deviennent les sous-barreaux d'une
sous-échelle : au-delà de la porte (le palier de la cellule raffinée), la clé
de cellule s'étend d'un niveau et les sous-barreaux entrent dans le score. UN
niveau. Vie complète : ligne « RAFFINEMENT » au déclenchement (tirages sans
gain, seuil, h à la cellule, sous-barreaux, porte) + relevé agrégé (workers
re-sérialisés, sous-barreaux, états archivés AU-DELÀ de la porte — à zéro, la
sous-échelle est posée mais jamais foulée). Un LP infaisable à la cellule ne
raffine pas (théorème 3 : cellule morte) et relance le détecteur.

Le modèle de bilan est prêté à la recherche (`cfg.balance`, statique de
process, `SetcodesOf` verrouillé — les workers appellent Solve en parallèle).
C'est aussi l'infrastructure du chantier 4.

**La fumée (étalon A, 300 s, graine 1234, `--refine-after 30000`)** — la vie
entière du mécanisme, en un run :

```
RAFFINEMENT (s22) : frontiere stagnante (30000 tirages sans gain, seuil 30000)
   — LP a la cellule h=5, 8 sous-barreau(x) poses, porte sp=30   (x6 workers,
     h=5-6, portes sp=26-34)
raffinement : 6 worker(s) re-serialise(s), 42 sous-barreau(x) au total,
   133163 etat(s) archive(s) au-dela de la porte
>=1 Liger : 321 tirages (best 1/3) — a la MOITIE du budget de la batterie
```

La sous-échelle est FOULÉE (133 k états au-delà de la porte), pas seulement
posée. Le juge du mécanisme reste ≥2 en proportion — première mesure de la
s23, le mécanisme est un drapeau jusque-là.

## 4. CHANTIER 4 — resté à l'état d'infrastructure, et pourquoi

L'élagage d'impasse à capacités dynamiques exige de réduire `u` par les
compteurs de chemin — or un bit MSG_CHAINING ne dit pas QUEL effet de l'hôte a
tiré : réduire toutes ses transitions bornées sur-contraint, et une fausse
preuve de mort tue des lignes réelles en silence (la pire famille du dossier).
La validation exigée (la marche du théorème 2 avec compteurs de quota le long
de la ligne réelle : 0 infaisable exigé) n'était pas payable dans la séance.
Ce qui est en place : le diagnostic nominal d'infaisabilité (phase 1 nomme ses
lignes), le modèle dans la recherche, et la règle écrite. Suite nommée s23.

## 5. CHANTIER 5 — l'assainissement

1. **Plafond PAR RACINE du finisseur, avec restitution** : la première racine
   de recul mangeait tout (`time_limit = left`, 138-399 s mesurés s21) ;
   chaque racine reçoit `max(2 s, restant / racines restantes)` et restitue
   son solde par la re-lecture de `left`.
2. **Balises** : reculs {15,30,45,60}, TOP-12, tranches 2×15, tournoi de 2,
   plafond 12 — « calés sur l'étalon A, jamais balayés » écrit dans le code ;
   l'observable « cimetière » de `ConsumedFrom` balisé hypothèse de JEU (coûts
   bannis/mélangés y échappent ; dérivable de la catégorie du coût ; juge = le
   troisième deck).
3. **Ventilation hindsight** (mesure AVANT correctif) : buts de substitution
   commis avec un quota suivi DÉPENSÉ vs frais — imprimée sous la ligne de vie
   hindsight. Première lecture (fumée, 300 s) : **240 913 commits, 100 %
   quota dépensé** — mais l'instrument mesure au COMMIT (fin de tirage), où
   presque tout tirage qui invoque a dépensé quelque chose : pour trancher le
   biais soupçonné (« renforcer les fusions précoces qui dépensent Wolf sur
   Tiger »), il faudra comparer le quota À LA PROFONDEUR de l'invocation du
   but de substitution, pas en fin de ligne. Instrument à affiner, dit ici
   plutôt que surinterprété. Le correctif éventuel sera un conditionnement du
   crédit, jamais un élagage (règle 2).
4. **La négation de Silver reste jouable** — rien n'a été retiré de
   l'énumération (règle 2 tenue, aucune action de plus qu'en s21).

## 6. L'A/B en proportion (étalon A, 3 graines × 600 s par bras)

Bras DUAUX (défaut s22) contre bras TÉMOIN (`--quota-legacy` = dérivation s21
par classes). Juges : proportion de graines à ≥1 Liger (acquis s21 : 2/3) et
à ≥2 (le chantier).

| graine | DUAUX (s22, calculé) | TÉMOIN (`--quota-legacy`, s21) |
|---|---|---|
| 888 | ≥1 : 0 | ≥1 : 0 |
| 1234 | **≥1 : 500 tirages** (best 1/3, approche 201 déc.) | **≥1 : 77** (best 1/3, approche 192 déc.) |
| 4242 | ≥1 : 0 | ≥1 : 0 |

**Lecture** (règle de l'événement rare : proportion, jamais les compteurs) :

- **≥1 : 1/3 = 1/3, sur la même graine** — la dérivation entièrement CALCULÉE
  tient l'acquis de la dérivation à la main. C'est le résultat du chantier :
  rien n'était dû au fine-tuning, et l'universalité ne coûte rien sur
  l'étalon A. (Le ×6,5 sur les tirages à ≥1 de la graine 1234 est un
  compteur, pas un juge — noté, pas conclu.)
- **1/3 contre 2/3 en s21** : à N = 3 par bras sur un juge à 0,1 % des
  tirages, c'est du bruit binomial (p ≈ 0,5 rend 1/3 dans 3 cas sur 8) — la
  s21 elle-même a mesuré 1 972 → 260 → 0 sur trois runs voisins de la même
  config.
- **≥2 : 0 partout** — le bloc Liger 1 → Liger 2 reste entier. C'est
  exactement le chantier du raffinement (`--refine-after`), implémenté dans la
  séance mais volontairement HORS de ces bras (binaire figé avant) : son A/B
  est la première mesure de la s23.

## 7. Invariants tenus

- Santé après chaque vague : 290/290, 0 MSG_RETRY, 273 digests, 211/273,
  209 candidates, 16 replays — inchangée à chaque contrôle.
- Banc A : h(départ) 13, échelle 60 unités / 51 paliers / ℓ_max 33, 0 chute.
- Harnais : 44 activations, 0 non appariée (étalon A) ; 28, 0 non appariée
  (étalon B).
- Auto-test simplexe : 65/65, désormais avec certificat de dualité.
- Un seul point de câblage tenu (`quota_fresh_pref`, `refine_after`,
  `balance` assignés à côté de `quota_hosts`) ; tout mécanisme neuf imprime sa
  vie ET son inertie (`ReportMechanisms` : `refine-after` dépend de l'échelle
  et du modèle prêté).

## 8. Ce qui reste, dans l'ordre

1. **Juger le raffinement** (`--refine-after`) sur l'étalon A en proportion —
   le bloc Liger 1 → Liger 2 (~95 réponses) est exactement ce que la
   sous-échelle doit couper ; le juge est ≥2 en proportion.
2. **Chantier 4** : la marche de validation à compteurs de quota, puis
   l'élagage sur événement.
3. **Le troisième deck** : la sonde B passe — un étalon C (deck Xyz/Lien pur,
   but à 2-3 exemplaires) départagerait l'hypothèse « cimetière » et les
   candidats TYPE/RACE.
4. **Rituels** : hors modèle (la procédure vit sur le SORT rituel, la forme
   « invoquer consomme l'hôte de la RÉSERVE » ne s'y applique pas) — lacune
   nommée, la garde « place SANS PRODUCTEUR » la dira le jour venu.

## 9. Inventaire

**Code** — `operators.h/cpp` : extraction cardinale, `fn_types`/`fn_races`,
pool + conversions « choisir », exclusion `AddMustBe...Summoned`, couplage
d'ignition (« igniter », garde d'asymétrie), duaux + certificat,
`infeasible_rows`, `QuotaHostsFrom`, verrou `sc_mx`, balise ConsumedFrom.
`search.h/cpp` : `quota_fresh_pref` (score à quotas frais), détecteur de
stagnation, `RefineLadderHere` (LP à la cellule, sous-échelle, porte),
extension de clé/score, ventilation hindsight. `main.cpp` : dérivation duaux
vs témoin (`--quota-legacy`), `--refine-after`, modèle prêté, plafond par
racine, balises, vies agrégées. `tools/s22_verify_duals.py`.

**Drapeaux neufs** — `--quota-legacy` (témoin d'A/B, à retirer après
promotion) ; `--refine-after <n>` (mécanisme à mesurer, 0 = éteint).
