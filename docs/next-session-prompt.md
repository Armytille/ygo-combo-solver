# Session 18 — LA DISCONTINUITÉ DU MATÉRIAU NOMMÉ

Tu reprends `combosolver` (racine `d:\ProjectIgnis\replay2video\combosolver`).
Lis `README.md` puis `docs/combo-solver-design.md` **§9.24** (session 17) en
entier, puis §9.23. **Ne redécouvre rien de ce qui y est chiffré, et ne
re-cherche pas la littérature : elle est en §9.22 (h), §9.23 (c) et §9.24 (b).**

## CE QUE LA SESSION 17 A ÉTABLI

### La loi d'arité a DEUX RÉGIMES, et un seul est franchissable

La sonde d'OFFRE (`--probe-repeat` + `--watch`, `RepeatProbe::offer_by`) ventile
les occasions **par type de prompt**. Étalon A nu, 90 s, graine 888 :

| carte | matériaux | SELECT_CARD | IDLECMD | **POSITION** | invoquée |
|---|---|---|---|---|---|
| Perfume Dancer | 2 | 134 758 | 13 643 | **9 349** | 7 138 |
| Sabre Dancer | 3 | 125 040 | 0 | **320** | 320 |
| Leo Dancer | 1 nommé + 2 | 123 100 | 0 | **0** | 0 |
| Liger Dancer | 1 nommé + 3 | 123 098 | 0 | **0** | 0 |

Les ~123 000 `SELECT_CARD` sont **le même pool pour les quatre** — l'extra deck lu
en entier, du **bruit**. Le signal est `IDLECMD` + `POSITION`, et `POSITION` est un
juge validé par la mesure elle-même (pour Sabre, POSITION = nombre d'invocations,
exactement).

- **Régime CARDINAL** (« 3 monstres Lunalight ») : une **pente**. Chaque corps
  posé rapproche, et `--hindsight` l'escalade.
- **Régime NOMMÉ** (« "Leo Dancer" + 3 Lunalight ») : une **discontinuité**. Le
  core ne liste la Fusion que si elle est payable, donc **le coup n'est pas dans
  l'espace d'actions** : aucun gradient de politique, aucun volume de tirages ne
  peut l'atteindre.

**PIÈGE 53, payé en séance : « proposée » n'est pas « invocable ».** Le premier
relevé de la sonde donnait « Leo et Liger proposés dans 33 000 tirages et jamais
pris → panne d'échantillonnage » — **l'inverse du vrai**. Ce qui trahit est le
rapport décisions/tirages : 1,0002 pour Leo/Liger contre 2,17 pour la Fusion
réellement invocable, c'est-à-dire *un seul prompt par tirage, toujours le même*.
Toute sonde d'occasion doit ventiler par type de prompt **avec un compteur**,
jamais un total ni un masque.

### `--hindsight` marche, et c'est le seul des quatre

Étalon A nu, 300 s, **deux graines** (juge = poses) :

| bras | graine | tirages | Perfume (arité 2) | Sabre (arité 3) | Leo | Liger |
|---|---|---|---|---|---|---|
| témoin | 888 | 1 236 499 | 29 745 | 659 | 0 | 0 |
| témoin | 1234 | 854 444 | 504 793 | 1 770 | 0 | 0 |
| `--hindsight 0.5` | 888 | 1 009 789 | **602 016** | **10 722** | 0 | 0 |
| `--hindsight 0.5` | 1234 | 996 534 | **578 025** | **5 668** | 0 | 0 |

**`min(hindsight) > max(témoin)` sur les deux cartes** — séparation complète des
supports, sur un juge où le témoin varie d'un facteur **17** entre graines. Le
mécanisme **stabilise** aussi (602 016 / 578 025, écart 4 %). Courbe du cadran
(90 s) : 0,25 → Sabre 589 ; **0,5 → 3 422** ; 1,0 → 3 218 — monotone jusqu'à 0,5
puis plate. Coût assumé : −18 % de débit.

**STATUT : CONFIRMÉ, pas encore DÉMONTRÉ.** Deux graines et un seul étalon.
`tools/s17_ab_B.ps1` (étalon B optimisé, le seul cas où une solution existe) est
**écrit et jamais lancé** — c'est le chantier 1 ci-dessous.

### Trois leviers écartés, chacun avec sa cause

- **`--recipe-w` RÉFUTÉ, cause STRUCTURELLE et non de réglage.** Trois poids sur
  une décade (0,1 / 0,3 / 1,0) donnent le même effondrement (poses ÷10) alors que
  la distance décroît réellement (10,70 contre 13,06 au départ) et que le débit
  **monte** (614 063 contre 446 281). Cause : fabriquer une Fusion **consomme deux
  Lunalight pour en rendre un**, donc la distance MONTE — le terme **punit les
  invocations**, l'inverse exact du but. Une heuristique h^add sur graphe ET/OU
  n'est correcte que si l'on modélise la **consommation inter-invocations**, ce
  que `RecipeDistance` ne fait pas (elle ne gère que l'intra-recette, via
  `claim_depth`). **Ne pas re-tenter sans ce préalable.**
- **`--backward` : vivant, sans matière.** La décomposition ne trouve que
  2 sous-produits (Leo, Liger) et n'en fabrique 0,02 en moyenne. `Expand` ne
  descend que sur les exigences NOMMÉES, et la recette amorcée de Liger est « Leo
  nommé + 3 Lunalight (cardinale) » : il n'y a rien d'autre à décomposer. Œuf et
  poule, et il est nommé — le graphe n'apprendra une recette profonde qu'après une
  invocation réussie.
- **`--assign` dégrade** (4 242 contre 74 788). Les sous-ensembles extrêmes
  injectés diluent les prompts fréquents plus qu'ils ne comblent la troncature des
  rares. Le mécanisme ne prune rien (règle 2) mais il n'est pas neutre en
  probabilité, et c'est ce qui le condamne ici.

### LE FAIT QUI DOMINE TOUT LE RESTE : TROIS TROUS DE COUVERTURE

L'opérateur a fourni **le premier plan résolu de l'étalon A** :
`D:\ProjectIgnis\replay\2026-08-16 13-19-12.yrpX`. Jugé avant usage : **rejeu
fidèle, 283/283, 0 `MSG_RETRY`**, board = **2 Liger Dancer**, et **2× Leo Dancer
invoqués** — les deux cartes que le solveur n'a jamais rendues invocables.
*Réserve* : ancienne main (3 Tenki), 2 Liger sur 3, sans Bagooska. **C'est un
corpus, pas une solution de la cible.**

Le contrôle de couverture rend **100 % au board** (283/283) : l'énumérateur
couvre toutes les INTENTIONS de la ligne. Les trois écarts bruts que le premier
relevé signalait (`SELECT_IDLECMD #0`, `SELECT_CARD #21`, `#75`) sont de
**représentation** — le contrôle comparait l'état EXACT (`Fingerprint`,
séquences comprises) alors que la recherche travaille au board, et la
déduplication par code choisit un autre exemplaire de la même carte. Le contrôle
sépare désormais les trois causes et affiche une ligne « TOTAL (au board) ».
Attributions écartées, ne pas les refaire : ni `--max-subsets` (24 et 256 :
identiques), ni la main transplantée (même résultat sur le duel natif).

### UN VRAI BUG TROUVÉ ET CORRIGÉ : `MSG_SELECT_BATTLECMD`

Dans la liste **attaquable**, `sequence` est un **uint8** (`playerop.cpp:37`),
pas un uint32 comme dans la liste *activable* du même message. Le décodeur
sautait 11 octets au lieu de 8 : **trois octets de décalage par monstre
attaquable**, donc `r.Ok()` tombait dès qu'il y en avait un — c'est-à-dire sur
tout board construit. L'énumération sortait vide, `DefaultResponse` ne couvre pas
ce message, **la branche mourait**. Silencieusement depuis l'origine : **toute
ligne entrant en Battle Phase était condamnée**, donc tout combo passant par la
Main 2 était hors d'atteinte, et la mort se lisait comme une impasse ordinaire.

Corrigé. Mesuré : **90 prompts forcés → 0, 90 impasses → 0**. Santé identique.
**Mais ce n'était pas le mur** : Leo/Liger restent à `IDLECMD = POSITION = 0` et
la transplantation reste à 4/6.

## LA MISSION — QUATRE CHANTIERS, DANS CET ORDRE

### 0. LE RÉPERTOIRE S'ÉPUISE À 4/6 — MONTER LE BUDGET D'ÉCARTS

Fait mesuré et non exploité, le plus prometteur du dossier : **à ZÉRO écart, en
ne jouant que des coups du répertoire de la ligne résolue, le solveur atteint
déjà 4 des 6 cartes en 602 états.** Puis il n'avance plus — 4/6 encore à 1 écart
(11 248 états) et à 2 écarts (33 078). La recherche s'arrête à 2 écarts.

Le répertoire est traité comme un ENSEMBLE de coups, pas comme une séquence.
Avec la nouvelle main (Gold Leo directement au lieu de Tenki → Gold Leo), les
premiers carrefours diffèrent et ce qui manque n'est pas dans le répertoire.
**Remarque de l'opérateur, et elle est juste : la nouvelle main EST le résultat
de la première étape de l'ancienne ligne, en plus fort — la suite du combo est
la même, elle devrait donc être atteignable.**

*À faire* : monter `--discrepancies` (3, 4, 6…) sur `tools/s17_transplant.ps1`,
avec un budget de temps qui suive, et lire à quel nombre d'écarts la 5e puis la
6e carte tombent. Si elles ne tombent jamais, identifier **quel coup** manque au
répertoire — c'est-à-dire ce que la nouvelle main oblige à inventer.

### 1. FINIR LA MESURE DE `--hindsight` (obligatoire, court)

**L'étalon B optimisé d'abord** — le seul cas où une solution existe, donc le seul
qui dise si `--hindsight` PAIE ou seulement AGITE (`tools/s17_ab_B.ps1`, écrit et
**jamais lancé** faute de temps). Juges inchangés depuis la s16 : ≥1/≥2/≥3, crête
aux résolutions complètes, **approche ÉCRITE** (`best_approach_kof8.yrp`), jamais
la ligne « NRPA best ». Puis une ou deux graines de plus sur l'étalon A
(`tools/s17_ab.ps1 -Ms 300000 -Seed 4242`, puis 9999).

Verdict attendu, à écrire quel qu'il soit : `--hindsight` passe-t-il de
**CONFIRMÉ** à **DÉMONTRÉ**, ou l'étalon B le renvoie-t-il au rayon « écrit,
instrumenté, non démontré » ?

### 2. FRANCHIR LA DISCONTINUITÉ : CONSTRUIRE LA CARTE NOMMÉE

Le seul chantier qui porte encore sur le mur. Fait mesuré : Leo Dancer n'est
**jamais invocable** (IDLECMD = 0, POSITION = 0), donc plus de tirages n'y
changeront rien — il faut **produire l'état** où elle le devient.

Deux voies, à départager par la mesure et non par goût :

**(a) Le sous-but explicite.** La décomposition (`RecipeGraph::Expand`, déjà
écrite, déjà imprimée) dit que Leo Dancer précède Liger. En faire une **cible
intermédiaire** — pas un `--hint`, une vraie entrée dans `Heuristic` avec son
propre terme `common` — donne au score le terme non nul qui manque *avant*
qu'aucun Liger n'existe. C'est ce que `--recipe-w` tentait, mais par une distance
globale qui punissait la consommation ; **un sous-but discret ne punit rien**, et
c'est toute la différence.
*Juge* : la colonne POSITION de Leo Dancer passe-t-elle de 0 à non nul ?

**(b) Le curriculum par hindsight.** `--hindsight` retient déjà 121-126 buts de
substitution par run. Les **ordonner** par la décomposition (fabriquer d'abord ce
qui sert) donne un curriculum appris sans corpus. Vérifier d'abord, par un simple
relevé, **quels** buts sont retenus — le mécanisme les compte mais ne les nomme
pas encore, et c'est un trou d'instrument à combler avant d'écrire quoi que ce
soit.

### 3. LE TROISIÈME DECK — dette de la s16, jamais payée

`combosolver-generalite-non-mesuree` : le moteur est générique (aucune carte
compilée, tout le spécifique est une entrée de ligne de commande) mais toutes les
preuves sont calibrées sur deux decks. **Un troisième deck est le seul juge de
généralité qui existe.** Reporté deux fois — le dire en tête de session plutôt
qu'en fin. Demander le replay à l'opérateur ; le solveur le juge d'abord
(`--guard`, `--resolve`, `MSG_RETRY`) avant d'en faire un étalon.

## L'ÉTAT DES MÉCANISMES ANTÉRIEURS

- **`--probe-repeat` : l'instrument, il MARCHE**, désormais avec la sonde d'offre
  (`offer_by`, six types via `OfferSlot`). Lire §9.24 (a) avant de le modifier.
  **RÉSERVE conservée de la s16 : son axe « distance de recettes » ne rend AUCUN
  verdict** et le dit lui-même — les recettes amorcées portent la zone JOKER, qui
  accepte le cimetière, donc le matériau qu'on vient de consommer y compte encore.
  Le réparer demande de n'utiliser que les recettes **OBSERVÉES**.
- **`--landmarks` / `--landmark-w` / `--landmark-h` : ÉCRITS, INSTRUMENTÉS, NON
  DÉMONTRÉS.** Le graphe apprend la bonne chose (sur l'étalon B, `Fake Trap @ADV
  banni x3` à l'ordre 0,54 — le handrip lui-même en landmark compté, sans qu'aucune
  carte soit nommée dans le code) mais l'A/B ne conclut pas. **À refaire sur
  plusieurs graines, bras `lmx60`**, en lisant `landmarks : h moyen` EN PREMIER.
  Limite de fond : le mécanisme exige un plan RÉSOLU, donc il ne sert à rien à
  froid — l'étalon A n'en a aucun. La voie naturelle est le minage **en ligne** sur
  les meilleures approches du run, comme `--options-online` pour les macros.
- **`--archive-spread` : DÉPARTAGÉ** — critère interne confirmé (expansions par
  racine 16 → 164) mais neutre à négatif sur les juges de recherche. Reste opt-in.
  **`--no-phase-change` et `--canonical-zones` restent non départagés.**

## CE QUI EST ACQUIS — ne pas re-mesurer, ne pas re-chercher

- **Bibliographie faite** (§9.22 (h), §9.23 (c), §9.24 (b)) : Bonet & Geffner
  (arXiv:2311.05490) ; Drexler, Seipp & Geffner (arXiv:2105.04250) ; Ståhlberg &
  Geffner (arXiv:2512.19355) ; Hanou, Dumančić & de Weerdt (arXiv:2508.21564) ;
  HER (NeurIPS 2017) ; Retro\* (arXiv:2006.15820) et AOT\* (arXiv:2509.20988) ;
  Delarue et al. (arXiv:2010.12001) ; DeepCubeA. **Résultats NÉGATIFS à ne pas
  re-chercher** : aucune littérature académique sur l'assemblage de combo en
  solitaire ; le nogood/CDCL vit dans des solveurs déclaratifs (mais le
  sous-problème d'AFFECTATION des matériaux, lui, est déclaratif — c'est
  `--assign`, mesuré et négatif) ; « ressources consommables + heuristique » ne
  ramène que du réseau et du matériel. Piste secondaire non explorée :
  **arXiv:2601.21684, *Do Not Waste Your Rollouts*** — recyclage NÉGATIF des
  motifs d'échec, sans entraînement.
- **RÉFUTÉS** : `--recipe-w` (s17, cause structurelle) ; `--assign` (s17) ;
  `--goal-bias` (s16) ; `--options-ctx` en ligne ; `--options-len 16/24` ;
  `--options-pool 24` ; `--mcps` (deux fois) ; `--nrpa-lr 2` ;
  `--novelty-rollout-cut`.
- **LES CONTRAINTES GUIDENT** — `--resolve` / `--summon-min` / la garde ne font pas
  qu'élaguer : le bras le plus contraint monte PLUS HAUT. **Ne jamais
  « simplifier » en les retirant.** Sur l'étalon A, ces trois cartes sont les
  activateurs de matériaux depuis le cimetière.
- **`Q̂` (`--qhat`) est implémentée fidèlement** et trouve la bonne ouverture seule.
  **`--finisher-options`** : écrit, **toujours jamais jugé**.
- **Plafond de représentation** : accord du corpus 52 → 59 → 64 (mémorisation
  pure). Aucun meilleur descripteur de contexte ne sauvera la masse.

## MÉTHODE — NON NÉGOCIABLE

- **L'INSTRUMENT AVANT LE MÉCANISME, et l'instrument lui-même se met en doute.**
  La s17 l'a payé une fois de plus : la sonde d'offre a produit, à sa première
  lecture, la conclusion **inverse** de la vraie.
- Un criblage à 90 s **élimine, il ne promeut jamais**. La graine est un tirage,
  pas un cadran. **Ne jamais changer deux facteurs.**
- Le juge de l'étalon A est **POSITION**, jamais le total d'offres (piège 53).
- Le juge de l'étalon B est l'**approche ÉCRITE**, jamais la ligne `NRPA … best k/N`.
- **Lire la vie du mécanisme AVANT le juge de recherche** (piège 52) :
  `s17_lecture.ps1` imprime `hsButs`/`hsAdapt`, `recDist`/`recD0`,
  `rebours`/`rebTot`. À zéro but, `--hindsight` est inerte et rien d'autre n'a de
  sens.
- **CODES et jamais NOMS**, y compris à `--watch` : « Lunalight Liger Dancer » est
  ambigu (54701958 / 101301030) et l'outil SORT. Le run nu de la s16 surveillait
  une liste tronquée sans le dire.
  `8379983` Gold Leo · `3027001` Fake Trap · `54701958` Liger · `24550676` Leo ·
  `88753594` Sabre · `81196066` Perfume · `90590304` Bagooska.

## PIÈGES DE BUILD ET D'ENVIRONNEMENT

- Un `MSBuild` ordinaire RELIE SANS `/USEPROFILE` : le binaire courant est
  **non-PGO**. Témoins PGO : `combosolver_preopt`, `_lto_s12`, `_lto_s13`,
  `_lto_s14`, `_lto_s14b`, `_lto_s15` (`tools/s15_pgo.ps1` comme modèle — ne
  jamais écraser le témoin précédent). La s17 n'a pas re-déroulé de pipeline PGO :
  un A/B apparié n'en a pas besoin, mais les DÉBITS ne se comparent pas à la s15.
- **Une mesure en cours VERROUILLE le binaire** (`LNK1104`). C'est voulu.
  Attendre : `while (Get-Process combosolver) { Start-Sleep 15 }`.
- **Le gabarit est ÉPINGLÉ** (`gabarits/etalon_a_lunalight.yrp`). **Le DECK ne
  l'est pas** : `D:\ProjectIgnis\deck\Lunalight.ydk` a changé le 16/08/2026 en
  pleine session. La `.cdb` non plus.
- Runs séquentiels, un `--outdir` par run. Logs PS en UTF-16 : `Select-String`,
  jamais `grep`.

## Montage de mesure

```powershell
# construire
& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe" `
    build\combosolver.sln /p:Configuration=Release /p:Platform=x64 /m /nologo /v:quiet

# sante — LA porte de tout changement de moteur, avant ET apres
.\bin\Release\combosolver.exe "D:\ProjectIgnis\replay\synchron handrip 2.yrpX" `
    --scriptdir ..\deps\scripts_2026-04-13\script --solve --solve-ms 60000 `
    --outdir s18_sante --no-chain Zalen --no-chain "Crystal Wing"
# 273 digests, 210/273, 209 candidates, 16 replays sur 209.

# ETALON A : A/B des leviers, juge = POSITION
.\tools\s17_ab.ps1 -Ms 300000 -Seed 4242 -Bras temoin,hind
.\tools\s17_lecture.ps1 -Motif 's17ab_*'

# ETALON B OPTIMISE — le seul cas ou UNE SOLUTION EXISTE (jamais lance)
.\tools\s17_ab_B.ps1 -Ms 300000 -Seeds 888 -Bras temoin,hind

# SONDE D'OFFRE seule (etalon A nu, decomposition par type de prompt)
.\tools\s17_sonde_offre.ps1 -Ms 90000 -Seed 888
```

Outils de la session 17 : `s17_sonde_offre.ps1` (sonde d'offre, étalon A nu),
`s17_ab.ps1` (A/B étalon A ; bras `temoin`, `recw*`, `back`, `assign`, `hind*`,
`hind_recw`), `s17_ab_B.ps1` (A/B étalon B, **écrit et jamais lancé**),
`s17_lecture.ps1` (juge POSITION + vie des mécanismes).
Antérieurs : `s16_sonde.ps1`, `s16_ab.ps1`, `s16_lecture.ps1`, `s15_ab.ps1`,
`s15_lecture.ps1`, `s15_curriculum.ps1`, `s15_courbe_qhat.ps1`, `s15_pgo.ps1`,
`s14_accord_mcps.ps1`, `s14_aretes_macro.ps1`, `s13_exploitation_longue.ps1`,
`s14_parallelisme.ps1` (jamais mesuré).

## Définition de « terminé »

(a) **Les trois trous de couverture attribués** (avec l'instrument qui sépare
« non énuméré » de « état différent ») et refermés — ou la démonstration écrite
de ce qui les cause.
(b) `--hindsight` **démontré ou réfuté** sur plusieurs graines ET sur l'étalon B.
(c) Un mécanisme qui fait passer la colonne POSITION de **Leo Dancer** de 0 à non
nul — ou la démonstration écrite qu'aucune des deux voies du chantier 2 ne le fait.
(d) Un **troisième deck** au dossier, jugé par l'outil.
(e) §9.25 documenté, ce prompt régénéré.
