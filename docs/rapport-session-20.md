# Rapport de la session 20

*Le détail complet est en §9.29 à §9.32 de `combo-solver-design.md`. Ce rapport
en est la synthèse ordonnée : ce qui a été bâti, ce qui a été mesuré, ce qui a
été réfuté — y compris deux hypothèses qui étaient les miennes.*

---

## 0. Le résultat, en une phrase

**Le run nu ne fonctionne toujours pas.** Le solveur ne trouve pas seul une ligne
qui atteint le but, même en partant d'une main dont on sait qu'une solution
existe. Ce que la séance a produit n'est pas une conversion mais une **chaîne de
mesure** : là où il y avait des hypothèses, il y a désormais un nombre à chaque
étape, et cinq hypothèses sont mortes de ces nombres.

Un fait nouveau domine tout le reste : **`replay/liger.yrpX`, fourni en séance,
est la première ligne vérifiée qui atteint le but de l'étalon A** — 3 Liger
Dancer, 0 `MSG_RETRY`, 63 actions, 331 décisions. Elle a servi de banc à presque
toutes les mesures qui suivent.

---

## 1. Correctifs de code

### 1.1 Un seul point de câblage (chantier D)

Relevé automatique sur les trois `SearchConfig` du fichier, **82 champs** :

| site | champs câblés |
|---|---|
| `RunTransplantSolve` | 66 |
| `RunSolve` — *le contrôle de santé* | 11 |
| `RunGrowthMeasurement` | 6 |

Trois conséquences, dont deux inconnues du dossier :

- `--elide-forced` : troisième occurrence du piège, déjà documentée ;
- **`--max-rollouts` / `--max-nodes` n'existaient pas hors transplantation** —
  `RunSolve` codait `max_nodes` en dur à 50 000 000. *Le mode déterministe n'a
  jamais été disponible sur le contrôle de santé* ;
- `--hindsight` et `--adapt-to-peak`, promus défauts en s19, n'atteignaient pas
  la santé.

**Correctif** : `ApplyMechanisms(opt, cfg)`, seul endroit du fichier où une
option atteint un champ de `SearchConfig` ; **26 assignations dupliquées
retirées**. Restent à l'appelant : budgets propres au mode, pointeurs vers objets
locaux, dérivation NRPA par worker.

**`ReportMechanisms`** : le contrôle qui manquait. « Un mécanisme doit imprimer
sa vie » ne suffisait pas — quand le champ n'était câblé nulle part, il n'y avait
rien à imprimer. La fonction rend les mécanismes **ACTIFS** et ceux **DEMANDÉS
mais INERTES** ici faute de dépendance.

*Défaut introduit et rattrapé* : deux lignes retirées étaient des corps de `if`
sans accolades ; la suppression a laissé `if(opt.hint_bias >= 0)` avaler le
`printf` suivant.

### 1.2 Un trou de couverture réel

`dedup_by_code` repliait les indices de `SELECT_UNSELECT_CARD` sur le seul
**code**, alors que la liste traverse les zones. La décision #210 de
`liger.yrpX` joue l'index 3 ; l'énumérateur n'offrait que 0, 1, 2.

Le dédoublonnage porte désormais sur **(code canonique, emplacement)** — le
`loc_info` était lu puis jeté.

| | avant | après |
|---|---|---|
| couverture sur `liger.yrpX` | 251/252 (**1 absent**) | **252/252** |
| arité moyenne géométrique | 4,11 | 4,17 |
| `log10 P` de la ligne | −104,3 | −105,4 |

### 1.3 Quatre défauts trouvés en câblant la sérialisation

1. **`owned` est dédoublonné** dans le chemin de recherche → une copie par code
   au lieu de trois → « trois Liger » infaisable pour une raison sans rapport
   avec le deck.
2. **Les recettes purement cardinales étaient sautées** —
   `Xyz.AddProcedure(c, nil, 4, 2)` ne remplit ni `named` ni `setcode`.
3. **L'alias n'était pas réduit** — `90590304` et `90590303` sont la même carte
   pour le core, pas pour `Canonical`.
4. **Une place sans producteur passait pour une preuve d'impossibilité.**

---

## 2. Instruments neufs

| instrument | ce qu'il rend | coût |
|---|---|---|
| `ReportMechanisms` | mécanismes actifs / demandés mais inertes | nul |
| vraisemblance de référence | `log10 P` d'un tirage uniforme, ventilé par type de prompt | 119 ms |
| couverture séparée | « coup RETROUVÉ » ≠ « coup IDENTIFIÉ » | nul |
| `PrintConsumption` | colonne négative : ce qu'un opérateur détruit, avec place et capacité | 0,2 s |
| `PrintFiringCounts` | comptes de tir par multiplicité | 0,2 s |
| `BalanceModel` + simplexe | `h`, `x*`, impasses prouvées | 0,2 s |
| banc du théorème 2 | `h` le long d'une ligne réelle, 332 décisions | 0,6 s |

**Correction d'instrument notable** : `ligne relevée : 251/331 coups identifiés`
confondait *retrouvé* (le coup est dans l'espace d'actions) et *identifié* (il
porte un `plan_key`). Séparés : **252/252 retrouvés**, l'écart 331 → 252 étant
les décisions **adverses**, jamais énumérées.

---

## 3. Le modèle formel : `h^SEQ`

### 3.1 Les quatre théorèmes (§9.30)

```
h(s) = min c'x    s.c.  A'x ≥ M_G − M_s ,  0 ≤ x ≤ u
```

| théorème | énoncé | preuve |
|---|---|---|
| 1 — admissibilité | tout plan coûte ≥ `h(s)` | `u_π` est admissible pour le programme |
| 2 — consistance | `h(s) ≤ c_o + h(s')` | `x' + e_o` est admissible en `s` |
| 3 — impasses | programme infaisable ⇒ aucun plan | contraposée de 1 |
| 4 — resserrement | toute contrainte que tout plan satisfait ⇒ `h` monte, admissibilité tenue | l'ensemble admissible se réduit sans exclure `u_π` |

La consistance **fabrique le crédit partiel** sans qu'on l'invente : accumuler
un corps réduit déjà les tirs exigés en amont.

### 3.2 Implémentation

- **Simplexe deux phases, règle de Bland** (terminaison garantie, pas de
  cyclage), **gardes primal/optimal vérifiées à chaque appel**,
  **auto-test 5/5** sur des instances à solution connue couvrant les quatre
  théorèmes.
- **Modèle purement déclaré** : une transition par `Duel.SetOperationInfo`
  (catégorie → destination, zone déclarée → source), capacité par
  `SetCountLimit`, place par le filtre (`IsSetCard`/`IsCode` via `fn_refs`).
  La transition inventée `mobiliser` a été **supprimée**.
- **109 places, 47 transitions** sur l'étalon A.
- L'arrondi `⌈c'x*⌉` manquait : le solveur rend des `x` fractionnaires (`x1.5`).

### 3.3 Ce que le modèle rend

| but | `h(départ)` |
|---|---|
| 1 Liger | 4 |
| 3 Liger | **14** |
| 4 Liger | **∞ — IMPASSE PROUVÉE** (le deck n'a que trois copies) |

Aucune session n'avait jamais pu prononcer « ce but est impossible depuis ce
deck ». C'est un calcul de 0,2 s, et c'est une preuve.

Le vecteur `x*` retrouve **seul** le mécanisme du combo : un renommage — qui rend
deux matériaux-Leo — plus un Leo mobilisé = trois matériaux-Leo pour trois
Ligers.

### 3.4 Le fait de jeu qui a corrigé quatre sessions

Le `SetValue` d'un `EFFECT_ADD_CODE` **n'est pas une constante** :

```lua
local cg = Duel.SelectMatchingCard(tp, s.costfilter, tp, LOCATION_DECK|LOCATION_EXTRA, ...)
Duel.SendtoGrave(cg, REASON_COST)
e:SetLabel(cg:GetFirst():GetCode())   -- le code de la carte ENVOYEE
e1:SetValue(e:GetLabel())             -- RUNTIME
```

Aucune lecture statique ne le rendra jamais. Ce n'est pas « Chick accorde le nom
de Leo » mais une **famille de transitions paramétrées** : renommer coûte
d'envoyer une Lunalight du deck ou de l'extra au cimetière, où
`EFFECT_EXTRA_FUSION_MATERIAL` la rend elle-même matériau.

---

## 4. Les mesures sur `liger.yrpX`

| grandeur | valeur |
|---|---|
| rejeu | 0 `MSG_RETRY` |
| board | 3 × Lunalight Liger Dancer + Tiger, Masquerade, Tenki, Wolf |
| coût | 63 actions, 331 décisions, 20 cartes brûlées |
| harnais | 44 activations, **0 non appariée**, 20/20 zone, 11/11 ressource |
| test de but | **se déclenche** en rejouant la référence |
| extra deck | **15 → 4** : onze cartes le quittent, dont trois sont les Ligers |
| décisions joueur | 252 (82 forcées, **170 à choix**) |
| `log10 P` sous politique neuve | **−105,4** |
| `h` le long de la ligne | **12 → 0**, 0 violation par transition |
| densité de descente de `h` | **5 %** (nouveauté : 15 %) |

### D'où viennent les 105 ordres de grandeur

| type de prompt | ordres | décisions | arité moy. géo. |
|---|---|---|---|
| `SELECT_IDLECMD` | 31,0 | 35 | 7,69 |
| `SELECT_CARD` | 24,7 | 33 | 5,62 |
| `SELECT_UNSELECT_CARD` | 19,6 | 29 | 4,73 |
| `SELECT_PLACE` | 15,9 | 29 | 3,54 |
| `SELECT_CHAIN` | 5,5 | 16 | 2,21 |
| `SELECT_POSITION` | 4,5 | 15 | 2,00 |
| reste | 4,1 | 13 | ~2 |

**Un quotient parfait sur `PLACE` + `POSITION` ne prend que 20 ordres sur 105.**
Il en reste 85 sur de vraies décisions.

### L'arithmétique du run nu

```
85 ordres sur ~110 decisions reelles  =>  arite geometrique ~5,9
   en UN bloc de 110 decisions  :  5,9^110  ≈ 10^85     impossible
   en 14 blocs de 8 decisions   :  14 × 5,9^8 ≈ 2×10^7  atteignable
```

La différence entre impossible et faisable n'est ni la politique, ni le budget,
ni le quotient : **c'est la sérialisation**.

---

## 5. Ce qui a été réfuté

| hypothèse | verdict et mesure |
|---|---|
| la ligne est hors de l'espace d'actions | **non** — 252/252 après le correctif |
| la main de l'étalon A est insuffisante | **non** — run nu depuis la main de `liger.yrpX` : 0/3, 5 monstres |
| `--op-bias` manquait | **non** — vivant à 97,6 % de prise, 0/3 ; à `w=6` la prise atteint 100 % et le résultat **empire** (5 contre 6 monstres) |
| la sérialisation n'était pas câblée | **non** — 5 sous-buts, échelle de 16 à 18 barreaux, 0/3 |
| la platitude vient de la transition inventée `mobiliser` | **non** — modèle purement déclaré, densité **inchangée à 5 %** |
| les tirages gaspillent la fin de tour | **non** — `--phase-w 0` : 27 281 coupures (100 %) ; `--phase-w 3` : 26 711 (100 %). Aucun effet |
| `--canonical-zones` casse le combo par les Zones Pendule | **le danger nommé n'est pas celui qui mord** : les 10 échecs sont en MZONE. Le vrai obstacle est que ce deck porte trois monstres Lien |

**Deux de ces réfutations portent sur mes propres hypothèses de la séance** — la
cause de la platitude, et la fin de tour. Le drapeau `--phase-w` reste, éteint
par défaut, avec sa mesure : c'est une réfutation, pas un mécanisme.

---

## 6. Invariants tenus

La santé a été repassée **sept fois**, après chaque changement structurel, et
n'a jamais bougé :

```
273 digests, 209 candidates, 16 replays, 0 MSG_RETRY
290 etats sur la ligne, 290 distincts, 0 fusions
COUVERTURE : 211/211
```

Le harnais d'opérateurs est resté à **0 non appariée** (37/37 puis 44/44 sur
`liger.yrpX`), 18/18 puis 20/20 en zone, 10/10 puis 11/11 en ressource.

---

## 7. Ce qui reste ouvert

**Lacunes d'extraction nommées :**

- `unresolved_counts` reste vide pour `Xyz.AddProcedure` : Bagooska n'a aucun
  producteur, et le rapport le dit comme **lacune**, pas comme preuve.
- le deck de `liger.yrpX` n'a pas été diffé contre `Lunalight.ydk` : la ligne
  emploie **A Bao A Qu**, absent des étalons antérieurs.

**Le prochain instrument, et il n'est pas un mécanisme de plus.** Entre « 5
monstres posés » et « 3 Liger » il y a 63 actions dans la ligne réelle, et le
solveur en enchaîne cinq. Il faut le **profil de progression d'un tirage** : à
quelle décision cesse-t-il d'ajouter au board, et quelle option lui manquait.
`SerialProgress` est déjà calculé à chaque décision ; l'histogramme « progrès
atteint par tirage » ne coûte qu'un compteur, et il tranchera entre

- **un verrou nommable** — tous les tirages meurent au même endroit ;
- **l'arité** — ils se dispersent, et seul un grain de sérialisation encore plus
  fin peut aider.

**Non fait du prompt s20** : `--elide-forced` jugé sur le chemin de recherche, la
sonde qui rend la pièce (chantier E), `--prior` retiré, `docs/drapeaux.md` §A
corrigé pour `--backward`, les 27 jamais jugés.

---

## 8. Inventaire

**Code** — `main.cpp` (`ApplyMechanisms`, `ReportMechanisms`, vraisemblance et
ventilation, banc du théorème 2, câblage de la sérialisation) ; `operators.h/cpp`
(`PrintConsumption`, `PrintFiringCounts`, `OperatorLP`, `SolveOperatorLP`,
`SelfTestOperatorLP`, `BalanceModel`, `fn_setcodes`/`fn_codes`/`fn_refs`) ;
`search.h/cpp` (`RefLineStats`, `SerialReq`, `SerialProgress`, archive indexée
par vecteur de sous-buts, `phase_w`) ; `enumerate.cpp` (dédoublonnage par
(code, emplacement)).

**Drapeaux neufs** — `--no-serial` (sérialisation allumée par défaut),
`--phase-w <f>` (éteint, réfuté).

**Documentation** — `combo-solver-design.md` §9.29 à §9.32 (7 934 lignes) ;
`etat-de-lart-consommation.md` (revue de littérature : équation d'état de Bonet,
cadre operator-counting, red-black planning écarté, l'unique concurrent connu).
