# Prompt de reprise — optimisation du combo solver

> À coller tel quel dans une nouvelle session, depuis `d:\ProjectIgnis\replay2video\combosolver`.

---

Tu reprends `combosolver`, un solveur de combo EDOPro écrit en C++ qui tourne sur
sa propre copie d'`ocgcore`. Dépôt git autonome, racine
`d:\ProjectIgnis\replay2video\combosolver`, un seul commit. Lis d'abord
`README.md` puis `docs/combo-solver-design.md` — ils portent les arbitrages, les
mesures et les impasses déjà explorées.

**Ta mission : rendre la recherche moins gloutonne, algorithmiquement d'abord,
programmatiquement ensuite.** Le constat qui déclenche ce travail : on explore
beaucoup trop d'états pour ce qu'on en tire.

## Ce que fait le solveur

Il rejoue un `.yrpX`, en extrait le board de fin de tour du joueur, puis cherche
à l'atteindre autrement — soit dans le même duel à moindre coût (`--solve`), soit
depuis un autre deck (`--start <replay>`). Sortie : des replays rejouables,
vérifiés par rejeu depuis zéro avant écriture.

Critère d'équivalence du board : mêmes cartes par **type** de zone, mêmes
positions, mêmes matériaux, mêmes compteurs. La colonne est ignorée.
Coût lexicographique : (1) cartes brûlées, (2) actions, (3) décisions.

## Construire et lancer

```powershell
Set-Location "D:\ProjectIgnis\replay2video\combosolver"
& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe" `
    build\combosolver.sln /p:Configuration=Release /p:Platform=x64 /m /nologo /v:quiet
```

```powershell
# Même deck : chercher mieux que la référence
.\bin\Release\combosolver.exe "D:\ProjectIgnis\replay\synchron handrip 2.yrpX" `
    --scriptdir ..\deps\scripts_2026-04-13\script --solve --solve-ms 60000

# Autre deck : refaire le même board depuis test 4
.\bin\Release\combosolver.exe "D:\ProjectIgnis\replay\synchron handrip 2.yrpX" `
    --scriptdir ..\deps\scripts_2026-04-13\script `
    --start "D:\ProjectIgnis\replay\test 4.yrpX" --solve-ms 600000 --outdir solutions
```

`--scriptdir` **n'est pas optionnel.** Sans lui le rejeu diverge en silence : 218
`MSG_RETRY` au lieu de 0, et l'outil continue d'afficher des mesures d'apparence
normale. Le compteur `MSG_RETRY` du rapport est le seul détecteur.

Si `..\deps\scripts_2026-04-13` manque, le régénérer avec
`.\tools\fetch_solver_deps.ps1` (il extrait aussi ocgcore et applique ses cinq
patchs). `deps/` est hors dépôt.

## L'état mesuré, à ne pas re-dériver

| | |
|---|---|
| Ligne de référence | 290 décisions, 0 `MSG_RETRY`, board atteint à la réponse #276 |
| Coût de la référence | 56 actions, 273 décisions, 19 cartes brûlées |
| Branchement | produit brut 10^97 le long de la seule ligne ; 36 % des décisions forcées |
| Vitesse | 0,2 ms/décision ; re-simulation complète 78 ms ; restauration d'instantané 0,05 ms |
| Couverture de l'énumérateur | 289/290, validée **sémantiquement** (appliquer / comparer / restaurer) |
| Digest | 290 états sur la ligne, 290 distincts, **0 fusion** |
| Même deck | ~500 solutions, toutes de coût **identique** à la référence, aucune strictement meilleure ; résiste à 7–8 écarts |
| Autre deck (`test 4`) | **aucune ligne**. Meilleure approche : 6 des 8 cartes, 6 monstres |

Sur `test 4`, les 8 cartes du board sont dans le deck, mais la ligne empruntait
Fake Trap, Red Dragon Archfiend et Scarred Dragon Archfiend, absents. Les mains
d'ouverture diffèrent : `Assault Zone + 3x Fake Trap` contre `2x Assault Zone`
(la seconde est morte, « 1 seule par tour »).

## Le chiffre qui justifie ta mission

À un écart, sur l'autre deck : **1 439 473 états développés, 222 363 fusions par
transposition — la table ne rattrape que 15 %.** Elle ne fusionne que les états
*identiques* ; or deux lignes qui diffèrent d'une carte au cimetière sont
distinctes et pourtant sans intérêt distinct. On paie 0,2 ms et un instantané
pour chacune.

Les tirages gloutons brûlent 8,5 M d'états en 300 s pour plafonner à 6/8.

## Direction algorithmique (recherche bibliographique déjà faite)

### 1. Élagage par nouveauté — le levier principal

**Iterated Width** (Lipovetzky & Geffner) renverse notre approche : au lieu de
tout développer puis dédupliquer, on **jette tout état qui ne rend vrai aucun
n-uplet d'atomes inédit**. IW(1) = un atome neuf, IW(2) = une paire neuve. Le
coût devient exponentiel dans la *largeur*, pas dans la taille de l'espace.

Trois raisons pour lesquelles c'est applicable ici sans rien remodéliser :

- **Nos atomes existent déjà** : `ComputeBoardKey` et `StateDigest`
  (`search.cpp`) calculent exactement les faits (carte, zone, position).
- **Ça marche sur simulateur opaque**, sans modèle PDDL — c'est toute la lignée
  Atari. On ne sait pas inspecter les préconditions d'ocgcore, seulement
  appliquer et restaurer : c'est précisément ce cadre.
- **`Rollout-IW` est notre `Search::Rollout()` en mieux** : la version anytime
  d'IW. Il manque la table de nouveauté et la coupure d'un tirage dès qu'il
  cesse de produire du neuf.

Lectures : arXiv:2106.04866 (survol, point d'entrée), arXiv:1801.03354
(Rollout-IW), arXiv:2404.17648 (BFWS, nouveauté + heuristique).

### 2. Sérialiser le but — la moitié structurelle

On teste le board comme **un but conjonctif atomique** : les 8 cartes d'un coup.
C'est le pire cas — le test ne se déclenche jamais avant la toute fin, donc il ne
guide rien pendant 300 décisions. IW brille sur les buts *atomiques*.

`Serialized IW` atteint les sous-buts un par un. Les **policy sketches**
(arXiv:2311.05490, arXiv:2105.04250) donnent un langage pour déclarer la
décomposition, explicitement conçu pour « encoder de la connaissance de domaine
à la main ou l'apprendre à partir de petits exemples ». C'est mot pour mot ce
qu'est notre répertoire — mais un sketch de largeur bornée vient avec une
garantie polynomiale, là où le répertoire n'est qu'un ordre de visite.

### 3. NRPA pour les tirages

La prime de répertoire dans `Search::Rollout()` est réglée à la main, et elle
était fausse au premier essai (elle valait +40 quand un monstre posé vaut +3, si
bien que « terminer le tour » — coup connu de la référence, donc primé — passait
devant une invocation productive ; terminer le tour est irréversible). Corrigée
en départage plutôt qu'en prime additive : 5/8 → 6/8.

**NRPA** (Cazenave) fait ça proprement : il apprend un poids par code de coup au
fil de tirages imbriqués. Nos `Choice::plan_key` **sont déjà** ces codes de
coups. GNRPA (arXiv:2003.10024) ajoute température et **biais** — l'emplacement
prévu pour un prior comme le nôtre. arXiv:2401.10420 traite le mode de
défaillance qu'on rencontrerait : la politique qui converge et rejoue sans cesse
la même séquence. Voir aussi arXiv:2101.03563.

Domaines d'application de NRPA : SameGame, Morpion Solitaire, TSP avec fenêtres,
repliement d'ARN inverse — tous mono-joueur, déterministes, à longue séquence.
Notre forme exacte.

**Réserve honnête :** la garantie polynomiale d'IW suppose une largeur bornée, et
rien ne dit qu'un combo Yu-Gi-Oh! en a une — la disponibilité d'une carte dépend
de beaucoup d'autres. Vise IW(2) sérialisé, sans garantie, mais avec un élagage
sans commune mesure avec l'existant. **Mesure la largeur effective avant de
promettre quoi que ce soit.**

## Direction programmatique

- **Parallélisme à 5 cœurs sur 16.** Chaque worker a sa propre arène, son propre
  duel et sa **propre** table de transposition : ils refont le même travail. Une
  table partagée (lock-free, à la lazy SMP) est le levier identifié. La
  réclamation dynamique est déjà en place (`SearchConfig::claims`), réclamer au
  *deuxième* écart et non au premier a fait passer de 2 à 5 cœurs.
- **`Heuristic()` fait deux requêtes de zone par fils évalué.** Chaud.
- **`StateDigest()` interroge 7 zones × 2 joueurs + l'état du processeur** à
  chaque nœud. Si la nouveauté remplace la transposition, une bonne part de ce
  coût disparaît.
- Profiler avant d'optimiser : le rapport donne déjà ms/décision et pages sales.

## Les pièges qui ont coûté cher — ne pas les redécouvrir

1. **`Arena::Init` s'approprie le routage des libérations du thread** (`t_owner`).
   Jamais deux arènes sur le même thread : toute arène supplémentaire tourne sur
   son propre thread. Et le `Duel` doit mourir **avant** `Arena::Shutdown()`.
2. **`ScriptProvider::Read` commence par `ArenaPause`** — son journal `misses`
   est partagé entre threads et ne doit pas vivre dans une arène.
3. **`Arena::Init` doit `reserve()` ses propres structures AVANT `t_owner = this`**,
   sinon l'allocateur alloue ses tables dans l'arène qu'il gère. `SelfCheck()`
   vérifie l'invariant.
4. **Toute lecture du suivi de pages sales passe par `SyncDirty()`** :
   `GetWriteWatch` avec RESET détruit les bits.
5. **`QUERY_CODE` ≠ `QUERY_ALIAS`.** `c.Code()` (alias résolu) pour l'identité de
   board — deux illustrations sont la même carte. `c.code` + `db.Canonical()`
   pour compter ce qu'un deck doit contenir : `get_code()` reflète aussi
   `EFFECT_CHANGE_CODE`, et Scarred Dragon Archfiend s'y présente comme « Red
   Dragon Archfiend ».
6. **L'appariement au plan utilise `plan_key`, pas `edge`** — la clé sans la
   colonne. Y mettre l'arête rend tout choix de zone inappariable.
7. **Une décision forcée (un seul choix légal) coûte zéro écart.** La facturer
   vide le budget sur des non-choix et tue la descente au premier prompt de
   l'adversaire.
8. **Le plan est un répertoire, pas un calendrier.** Le suivre pas à pas fait
   décrocher à la 4ᵉ décision sur une question inédite (4 états contre 60 000).

## Discipline de vérification — non négociable

Chaque changement doit préserver, et le rapport les affiche tous :

- `MSG_RETRY` = 0 au rejeu de référence ;
- couverture de l'énumérateur ≥ 289/290 ;
- **0 fusion de digest** sur les 290 états de la ligne (ils sont distincts par
  construction ; une fusion signifie que le digest sous-hache et fait disparaître
  des solutions **sans rien signaler** — c'est le mode de défaillance à
  surveiller) ;
- à zéro écart sur le même deck, la référence **doit** être retrouvée : c'est ce
  qui fait qu'« aucune solution » reste un signal de défaut ;
- les replays écrits sont rejoués depuis zéro dans un duel neuf avant écriture.

Si tu introduis un élagage par nouveauté, **ajoute une mesure du taux de coupure
et du taux de solutions perdues** sur le cas même-deck, dont on connaît les ~500
solutions. Un élagage qui gagne 10× en états mais perd des solutions doit le dire
lui-même, pas être découvert plus tard.

## Cas de test

- Référence : `D:\ProjectIgnis\replay\synchron handrip 2.yrpX`
- Autre deck : `D:\ProjectIgnis\replay\test 4.yrpX`
- `--workdir` par défaut `D:\ProjectIgnis` (installation EDOPro réelle, à ne
  jamais modifier)

Question ouverte à laquelle personne n'a répondu : **le board de la référence
est-il atteignable depuis le deck de `test 4` ?** Rien ne l'exclut — les 8 cartes
sont là — mais aucune ligne n'a été trouvée. L'outil peut prouver
« atteignable » en produisant le replay ; il ne peut prouver « inatteignable »
que par le test de disponibilité des cartes, qui passe.
