# Session 19 — LES OPÉRATEURS DÉCLARÉS : lire les cartes au lieu de les observer

Tu reprends `combosolver` (racine `d:\ProjectIgnis\replay2video\combosolver`).

La session 18 a audité le solveur, corrigé sept défauts, retiré dix mécanismes
réfutés, rendu le run **reproductible** — puis a ouvert le **code Lua des
cartes** pour la première fois. Cette lecture a retiré le cadrage que le dossier
portait depuis trois sessions et désigné, à la constante près, les deux coups
qui bloquent le combo.

Lis `README.md` (les trois règles), puis `docs/combo-solver-design.md` **§9.25**
(l'audit), **§9.26** (les correctifs) et **§9.27** (le Lua, la cascade mesurée,
la règle « un drapeau n'est jamais un correctif »), puis `docs/drapeaux.md`.
La bibliographie est faite (§9.22 (h), §9.23 (c), §9.24) : ne la refais pas.

---

## LE FAIT QUI COMMANDE CETTE SESSION

Sur les **24 cartes** du deck de l'étalon A, exactement **deux** accordent un
état qui débloque quoi que ce soit :

```
35618217  Lunalight Kaleido Chick  ->  EFFECT_ADD_CODE
2344618   Lunalight Masquerade     ->  EFFECT_EXTRA_FUSION_MATERIAL
```

**Ce sont exactement les deux goulots mesurés** (0,14 % et 0 %). Trouvées par un
`grep` sur deux constantes du jeu, sans nommer une seule carte.

Et le recensement des `CATEGORY_*` **ne les aurait pas trouvées** : il rend
`CATEGORY_FUSION_SUMMON` ×1 (Wolf) et pas un mot des deux pivots. **Il faut DEUX
vocabulaires** :

| vocabulaire | décrit | exemple |
|---|---|---|
| `CATEGORY_*` | ce que l'effet fait aux **cartes** | envoyer au cimetière, chercher |
| `EFFECT_*` | quel **état** il accorde | renommer, autoriser un matériau du cimetière |

Le combo repose entièrement sur le second. **C'est l'erreur du graphe de
recettes** : il modélise des *produits*, jamais des *états accordés*.

## LA CASCADE, MESURÉE (§9.27 (c), étalon A nu, 353 642 tirages)

| étape | mesure | part |
|---|---|---|
| Masquerade actif (porte 2) | 335 365 activations | **34,7 %** |
| Wolf activé (porte 1, Zone Pendule) | 18 856 | **4,5 %** |
| **Kaleido Chick — renommage activé** | **519 décisions** | **0,14 %** |
| Leo choisi quand offert | 6 / 519 | **1,16 %** |
| Leo atteint le cimetière | 6 tirages | 0,0017 % |
| Masquerade e2 (la défausse) déclenché | **0** | — |
| **Liger invoqué** | **0** | — |

**Le goulot n'est ni la porte ni le choix : c'est l'OFFRE.** Et Kaleido Chick est
la voie **UNIQUE** — le scan des 24 scripts ne rend qu'un autre candidat,
`Lunalight Fusion` (87931906), dont l'accès à l'Extra est gardé par *« si
l'adversaire contrôle un monstre invoqué depuis l'Extra Deck »*, faux en
solitaire.

## LA LIGNE CIBLE, EN ENTIER (lue dans les scripts, §9.27 (a))

```
Liger  : Fusion.AddProcMixN(c,false,false,24550676,1,IsSetCard(SET_LUNALIGHT),3)
         + AddMustBeFusionSummoned()
Leo    : materiau NOMME 97165977 (Panther Dancer), ABSENT du deck
Chick  : IGNITION/MZONE, 1x/tour/exemplaire
         COUT = envoyer un Lunalight du DECK ou de l'EXTRA au cimetiere
         OP   = EFFECT_ADD_CODE du code envoye, UNIQUEMENT comme MATERIAU DE
                FUSION, jusqu'a la End Phase
Wolf   : PORTE 1 — Fusion depuis LOCATION_PZONE, 1x/copie, materiaux terrain
         + CIMETIERE bannis
Masq.  : PORTE 2 — TRIGGER sur EVENT_SPSUMMON_SUCCESS d'une Fusion Lunalight,
         exige une Polymerization DEJA au cimetiere, recupere Poly, puis
         SelectYesNo -> DiscardHand(1) -> EFFECT_EXTRA_FUSION_MATERIAL
```

Le coût de Chick **est** l'envoi de Leo au cimetière : une activation produit
**deux** entités de code 24550676 (le Chick renommé et le vrai Leo au cimetière).
Les **3 Fake Trap** de la main sont le carburant des défausses de Masquerade —
c'est ce que l'ancienne main (3 Tenki) ne pouvait pas payer, d'où les 2 Liger du
plan résolu au lieu de 3.

## POURQUOI LES TROIS TENTATIVES PRÉCÉDENTES ONT ÉCHOUÉ

Recettes (s16), landmarks (s16), `--backward` (s17) : tous **nourris par
l'OBSERVATION** — ce que le solveur a déjà réussi — au lieu de la
**DÉCLARATION** — ce que les cartes disent.

- landmarks : « exige un plan résolu, donc ne sert à rien À FROID » (§9.23 (g)) ;
- `--backward` : « mécanisme correct, **matière absente** », 2 sous-produits,
  0,02 fabriqué (§9.24 (e)).

**`--backward` était le bon algorithme sur un graphe amputé de ses arêtes.** Les
arêtes manquantes sont les effets, et elles sont déclarées.

## LA JUSTIFICATION QUANTITATIVE, à garder sous les yeux

Le plan résolu fait **284 décisions, 141 forcées, 143 LIBRES**, produit des
branchements dédupliqués **10^86,3**. La politique plafonne à **66 %** d'accord
par décision ; il en faudrait **91 %** pour espérer un succès. Un poids par code
de coup ne peut pas monter : il est **aveugle à l'état**.

Mais 32 seulement des 143 décisions libres sont des `IDLECMD` — « que jouer ».
`0,66^32 ≈ 1,7 × 10⁻⁶`, soit de l'ordre d'un succès par run de 90 s.

**Un plan fournit exactement la dépendance à l'état qui manque.** Il ne remplace
pas l'échantillonnage : il conditionne la distribution sur les 32 décisions qui
comptent. C'est le seul levier que l'arithmétique autorise.

---

## CHANTIER 0 — LE HARNAIS DE VALIDATION (et ce n'est pas un mécanisme)

**À faire avant tout le reste, et à ne pas sauter.**

Extraire du deck la table d'opérateurs, l'imprimer, puis **vérifier qu'elle
explique le plan résolu** `D:\ProjectIgnis\replay\2026-08-16 13-19-12.yrpX`
(283 décisions, 0 `MSG_RETRY`, 2 Liger) : chaque activation correspond-elle à un
opérateur extrait, et la séquence est-elle valide sous les préconditions
extraites ?

*Un opérateur, par `(code, description)`* — la paire que la session 18 vient de
rendre visible en donnant une identité aux prompts :

```
préconditions : SetRange (où la carte doit être) · SetCountLimit (ressource)
                le gate `chk == 0` du coût / de la condition
produit       : SetOperationInfo(CATEGORY, LOCATION)   <- categorie ET zone
accorde       : EFFECT_ADD_CODE, EFFECT_EXTRA_FUSION_MATERIAL, ...
consomme      : materiaux, main, compte par tour
```

*Matière disponible, déjà comptée* : 24 cartes avec script, **40 déclarations
`SetOperationInfo`**, 14 catégories distinctes sur tout le deck.

**C'est falsifiable, ça coûte un run, et ça dit si l'extraction est fidèle AVANT
qu'on bâtisse dessus.** Si le harnais échoue, tout le reste de la session est
sans objet — et c'est exactement ce qu'on veut savoir en premier.

*Où prendre la donnée, et le choix n'est pas neutre* :

- **le core la possède** (`peffect->get_category()`), mais `MSG_SELECT_IDLECMD`
  ne la transporte pas. Un petit patch d'ocgcore l'exposerait — le projet l'a
  déjà fait pour `OCG_DuelQueryProcessorState`. **Exact.**
- **l'analyse statique du Lua** est immédiate et **optimiste** : conditions et
  coûts sont des fermetures, on lit la *déclaration*, pas la sémantique.

Commencer par le statique pour le harnais ; le patch si le harnais montre que la
déclaration ne suffit pas.

## CHANTIER 1 — LE TYPE DE NŒUD MANQUANT

Le graphe range Leo comme un **produit à fabriquer** — il essayait de construire
une carte non constructible. Leo est ici une **propriété acquérable**.

Ajouter le nœud « **code qu'une carte peut ACQUÉRIR** » (`EFFECT_ADD_CODE` /
`EFFECT_CHANGE_CODE`, constantes du jeu), plus les arêtes *produit (catégorie,
zone)*. Puis **re-juger `--backward`** : sa décomposition de Liger contient-elle
enfin l'opérateur de Kaleido Chick ?

C'est la mesure qui dit si la matière est arrivée. À décomposition inchangée, ne
pas aller plus loin.

## CHANTIER 2 — LE CHAÎNAGE ARRIÈRE COMME BIAIS

Depuis les faits de but non satisfaits, remonter aux opérateurs disponibles →
un ensemble d'« opérateurs utiles maintenant » → biais de la politique sur les
décisions `IDLECMD`.

**Règle 2 du dossier, non négociable : un plan est un BIAIS, jamais un
élagage.** Si le planificateur se trompe, l'échantillonneur couvre encore
l'espace. Et **compteur de vie imprimé** — combien d'opérateurs proposés,
combien retenus. À zéro, le mécanisme est inerte, et le dossier a déjà payé deux
sessions pour l'avoir ignoré (`--assign-bias`).

## CHANTIER 3 — LA CONSOMMATION (si les trois premiers passent)

3 Liger = **3 codes-Leo + 9 corps Lunalight + 3 portes**, dont deux Wolf
(un par copie) et une Masquerade payée par une défausse.

C'est la leçon de §9.24 (d), et elle a déjà tué `--recipe-w` : *une heuristique
h^add sur un graphe ET/OU n'est correcte que si la CONSOMMATION est modélisée* —
la relaxation par suppression suppose qu'atteindre un sous-but ne détruit rien,
hypothèse exactement fausse ici.

---

## LA DETTE QUI RESTE, ET ELLE EST MÉCANIQUE

1. **PROMOUVOIR les mécanismes mesurés en DÉFAUT.** `--elide-forced`,
   `--hindsight 0.5`, `--adapt-to-peak` sont mesurés bons sur l'étalon A et
   restent **éteints**. Le solveur nu n'en bénéficie pas. Il leur manque
   l'étalon B **en N runs, lu en PROPORTION** — la bimodalité l'impose
   (§9.25 (b)). Une dizaine de runs, et ils deviennent le défaut avec un
   drapeau négatif pour l'A/B.
2. **Retirer `--backward`** (réfuté, 50 références) *si* le chantier 1 ne le
   ressuscite pas — c'est la seule suppression qui demande de la chirurgie — et
   **`--prior` / `--prior-weight`** (neutres sur les deux étalons).
3. Les **27 jamais jugés** de `docs/drapeaux.md`. Attention : `--canonical-zones`
   **casserait le combo** (les Zones Pendule sont des séquences particulières de
   `LOCATION_SZONE`, et Wolf n'invoque que de là) — le départager tel quel
   rendrait un verdict faussement négatif.

## CE QUI RESTE OUVERT, ET QUI N'EST PAS DE CETTE SESSION

- **`ProcessorState` est-il encore nécessaire ?** ×1,00 aux points idle, et
  `--elide-forced` retire 74,6 % des prompts intermédiaires de la table. La
  justification du patch C1 n'a pas été re-mesurée. **Faisable maintenant** en
  mode déterministe.
- **L'adversaire dans la clé** : `StateDigest` boucle sur les deux joueurs, la
  moitié des requêtes porte sur un adversaire qui ne joue pas. Question de
  **débit**, pas de clé.
- **Le finisseur seul n'a jamais été jugé.** `--finisher-min` proche de
  `--solve-ms` le fait sans code.
- **La sensibilité des 11 paris** du score (100, 10, 3, 1, `hint_bias` 2,0,
  `resolve_weight` 250, budgets ×0,7 et ×0,8, `qhat_*`, `hindsight_k`).
  **Faisable maintenant** : le mode déterministe existe.
- **L'étalon B n'a aucun run nu** — `--resolve` y est l'énoncé du but *et* un
  triple indice.
- **Les scripts Lunalight ne sont PAS dans l'export épinglé** : ils viennent de
  l'installation vivante via le repli `assets.cpp:253` (`<workdir>/script`). Le
  dossier avertit lui-même qu'un mauvais jeu de scripts fait diverger le rejeu
  **en silence**. `MSG_RETRY = 0` aujourd'hui ; rien ne le garantit demain.
- **Un troisième deck** reste le seul juge de généralité, et il n'existe pas.

---

## MÉTHODE

- **La santé est la porte, avant ET après chaque changement** :
  ```powershell
  .\bin\Release\combosolver.exe "D:\ProjectIgnis\replay\synchron handrip 2.yrpX" `
      --scriptdir ..\deps\scripts_2026-04-13\script --solve --solve-ms 60000 `
      --outdir s19_sante --no-chain Zalen --no-chain "Crystal Wing"
  # 273 digests, 210/273, 209 candidates, 16 replays.
  ```
- **Le mode déterministe est l'instrument d'attribution** :
  `--threads 1 --max-rollouts N --max-nodes N --solve-ms 900000` (le temps ne
  doit JAMAIS mordre). Contrôle : 2 lignes de diff sur 397, et ce sont les noms
  d'outdir. Les mesures de performance restent à seize workers.
- **Sur l'étalon B : N runs par bras, lecture en PROPORTION.** Le juge y rend
  `0, 0, 0, 89, 2 239` à commande et graine identiques.
- **UN DRAPEAU N'EST JAMAIS UN CORRECTIF.** Si aucun utilisateur ne voudrait le
  comportement d'avant, il n'y a rien à choisir : c'est le défaut. Un mécanisme
  est un drapeau *le temps de le mesurer*, puis devient le défaut.
- **Un mécanisme doit imprimer sa VIE** (un compteur non nul quand il agit)
  avant qu'on mesure son effet.
- Une mesure en cours **verrouille le binaire** (`LNK1104`) : c'est voulu.
- Logs PS en **UTF-16** : `Select-String`, jamais `grep`. Runs séquentiels, un
  `--outdir` par run.
- **CODES et jamais NOMS** : `8379983` Gold Leo · `3027001` Fake Trap ·
  `54701958` Liger · `24550676` Leo · `97165977` Panther (absent) ·
  `88753594` Sabre · `81196066` Perfume · `35618217` Kaleido Chick ·
  `2344618` Masquerade · `47705572` Wolf · `90590304` Bagooska ·
  `24094653` Polymerization · `87931906` Lunalight Fusion.
- Les scripts se lisent dans `D:\ProjectIgnis\script\official\c<code>.lua`.
- Bancs de la s18 : `tools/s18_axe1.ps1`, `s18_axe9.ps1`, `s18_determinisme.ps1`,
  `s18_mur.ps1`, `s18_yesno.ps1`.

## CE QU'IL NE FAUT PAS REFAIRE

- **Chercher le « couplage fantôme » de `--card-on-select`** : il n'existe pas
  (§9.25 (a)), et le drapeau lui-même a été supprimé — l'identité est
  inconditionnelle.
- **Croire que `--threads 1` suffit au déterminisme** : c'est l'UNITÉ du budget,
  et c'est corrigé.
- **Re-mesurer §9.24 (o)** : un run par bras sur l'étalon B ne rend rien.
- **Croire au ×27 de `--assign-bias`** (§9.24 (n)) : ce bras portait `--assign`
  en plus, et le mécanisme seul était **inerte**. Une fois réellement actif, il
  **divise l'arité 3 par deux**.
- **Croire à la « discontinuité du matériau NOMMÉ »** (§9.24) : elle n'existe
  pas. L'exigence est `IsCode(24550676)` sur un **matériau**, et
  `EFFECT_ADD_CODE` la satisfait.
- **Le décodeur binaire** : audité champ par champ contre `playerop.cpp`,
  **19 messages sur 19 justes** (§9.25 (c)).
- Les réfutations de `docs/drapeaux.md` §A — dix sont hors du code.
- « `--max-subsets` explique les trous de couverture » : mesuré, faux.
- « le changement de phase gaspille les tirages » : mesuré, faux, drapeau retiré.
- « la ligne est hors de l'espace d'actions » : mesuré, faux (couverture **100 %
  au board**).

## LIVRABLE ATTENDU

1. **§9.28 — le harnais du chantier 0** : la table d'opérateurs du deck, et le
   verdict de sa confrontation au plan résolu. **C'est le livrable qui compte**,
   même si tout le reste échoue.
2. Le chantier 1, et le re-jugement de `--backward` sur une décomposition qui a
   enfin de la matière.
3. Le chantier 2 **seulement si** 0 et 1 passent, avec son compteur de vie.
4. La **promotion en défaut** de `--elide-forced`, `--hindsight`,
   `--adapt-to-peak` après leur mesure en proportion sur l'étalon B.
5. Ce prompt régénéré.

**Ne pas écrire le planificateur avant que le harnais ait parlé.** Trois
sessions ont bâti sur un graphe dont personne n'avait vérifié qu'il décrivait le
jeu ; la quatrième doit commencer par cette vérification, et elle coûte un run.
