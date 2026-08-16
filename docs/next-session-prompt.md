# Session 19 — LA SONDE DE CONJONCTION, ou pourquoi Liger n'est jamais PAYABLE

Tu reprends `combosolver` (racine `d:\ProjectIgnis\replay2video\combosolver`).

La session 18 a audité le solveur, corrigé sept défauts, retiré dix mécanismes
réfutés et attaqué le mur. **Le mur n'est pas percé.** Mais le juge est réparé,
et pour la première fois un run est reproductible.

Lis `README.md`, puis `docs/combo-solver-design.md` **§9.25** (l'audit) et
**§9.26** (les correctifs et l'attaque du mur), puis `docs/drapeaux.md`. La
bibliographie est faite (§9.22 (h), §9.23 (c), §9.24) : ne la refais pas.

## CE QUI EST ACQUIS

| fait | valeur |
|---|---|
| mode **déterministe** (`--threads 1 --max-rollouts N --max-nodes N`) | **2 lignes de diff sur 397**, et ce sont les noms d'outdir |
| coût du mono-worker | ÷5,1 à ÷5,9 (pas ÷16) |
| `--adapt-to-peak` sur l'arité 3 (étalon A nu) | **×2,6 dans DEUX paires** (1 442→3 740 et 659→1 690) |
| son coût | −24 à −37 % de débit |
| drapeaux | 122 → **115** (dix retirés, trois ajoutés) |
| Leo Dancer et Liger Dancer, cinq bras, 2,1 M tirages | **ZÉRO** |

## CE QUE LA SESSION 18 A RETIRÉ DU DOSSIER

- **§9.24 (o) en entier.** Le juge de l'étalon B rend `0, 0, 0, 89, 2 239` sur
  cinq exécutions de la MÊME commande à la MÊME graine. Ni `--assign-bias` ni
  `--card-on-select` n'y ont été réfutés ; `--elide-forced --hindsight` n'y a
  rien confirmé.
- **Le « couplage fantôme »** de `--card-on-select` : il n'existe pas. Sous
  `--hint-bias 0`, où le drapeau est sémantiquement inerte, les bras divergent
  quand même (§9.25 (a)).
- **Le ×27 de `--assign-bias` sur Leo au cimetière** (§9.24 (n)) : ce bras
  portait `--assign` en plus. Le mécanisme seul était **inerte** — l'instantané
  du graphe n'était pris que sous `--assign` ou `--backward`. Corrigé ; et une
  fois réellement actif, `--assign-bias` **divise l'arité 3 par deux**.

---

## CHANTIER 1 — LA SONDE DE CONJONCTION (l'instrument, pas un mécanisme)

Trois sondes successives ont chacune déplacé le diagnostic d'un cran :

1. **offre** (§9.24 (a)) — Leo et Liger ne sont jamais listés comme invocables,
   leurs offres sont toutes sur des prompts de SÉLECTION ;
2. **activations** (§9.24 (l)) — la porte EST empruntée : `Lunalight Masquerade`
   s'active dans **21,4 %** des tirages ;
3. **présence en zone** (§9.24 (m)) — le matériau n'arrive pas : Leo au
   cimetière dans **0,004 %** des tirages (18 à 93 sur ~450 000).

**La quatrième doit mesurer la SIMULTANÉITÉ**, la seule chose qu'aucune n'a
regardée. À chaque activation de la porte (`Masquerade`, `Wolf`), relever
combien des préconditions de Liger sont vraies **au même instant** :

- `Lunalight Leo Dancer` au cimetière (≥ 1) ;
- trois monstres « Lunalight » disponibles (terrain **ou** cimetière — les deux
  effets prennent les matériaux des deux zones) ;
- la porte elle-même.

*Le livrable est un histogramme* : combien de fois 0, 1, 2 ou **3** conditions
sur 3. Si le mode « 3 sur 3 » est non nul et que Liger reste à zéro, la panne
est dans l'ÉNUMÉRATION et non dans l'état — et c'est un défaut à trouver dans le
core ou dans le décodeur. Si « 3 sur 3 » vaut zéro, le chantier suivant est de
**construire** la conjonction, et la sonde dit laquelle des trois manque.

*Coût* : le volet présence de `--watch` relève déjà les zones sur `MSG_MOVE`,
à coût nul. Le volet activations relève `MSG_CHAINING`. Il ne manque que la
**jointure** des deux, plus un compte de Lunalight disponibles.

*Piège à ne pas répéter* : compter **avant** le test d'élision. Un prompt élidé
ne redescend pas dans le corps de la boucle, et 74,6 % des prompts sont forcés —
une sonde qui perd ses événements sous un drapeau rend un « jamais » qui
n'existe pas (§9.24 (m)).

## CHANTIER 2 — `--adapt-to-peak` : LE DÉMONTRER

Il est **confirmé** (×2,6 sur deux paires, un seul facteur) et **pas démontré**
au sens du dossier : une graine, l'étalon A seul.

- **plusieurs graines** sur l'étalon A, juge = invocations de Sabre Dancer
  (arité 3), qui ne dépend pas du juge de l'étalon B ;
- **l'étalon B**, en **N runs par bras** et lecture en **proportion** de
  réussites — jamais sur la valeur d'un compteur (§9.25 (b)) ;
- et l'attribution du **coût** : −24 à −37 % de débit est-il le prix du gradient
  raccourci, ou un effet de bord ? La ligne « gradient tronque au pic : N pas
  retires » et le compte de tirages suffisent à trancher.

*Piste à mesurer au passage, elle est petite* : `run.flat` (le minage EN LIGNE
des macros) n'est **pas** tronqué au pic. Miner les macros sur le seul préfixe
productif est cohérent avec le correctif ; ce n'est pas fait, et c'est un A/B
d'une ligne.

## CHANTIER 3 — FINIR LE NETTOYAGE

Restent, avec leur verdict écrit dans `docs/drapeaux.md` :

- **`--backward`** — RÉFUTÉ (§9.24 (e), matière absente), **50 références** : la
  décomposition à rebours est imbriquée dans le graphe de recettes. C'est le
  seul retrait qui demande de la chirurgie.
- **`--prior` / `--prior-weight`** — mesure NEUTRE sur les deux étalons (§9.12).
- Les quatre cadrans `--options-len` / `--options-pool` / `--options-window` /
  `--options-ctx` : leur réfutation dit « ne pas tourner ce bouton », pas
  « supprimer le mécanisme qu'il règle ». **Les garder**, et l'écrire.

Santé avant et après chaque retrait.

---

## CE QUI RESTE OUVERT, ET QUI N'EST PAS DE CETTE SESSION

- **`ProcessorState` est-il encore nécessaire ?** ×1,00 aux points idle
  (§9.24 (j)), et `--elide-forced` retire 74,6 % des prompts intermédiaires de
  la table. La justification du patch C1 n'a pas été re-mesurée. Une mesure d'un
  bras, désormais faisable en mode déterministe.
- **L'adversaire dans la clé.** `StateDigest` boucle sur les deux joueurs : la
  moitié des requêtes porte sur un adversaire qui ne joue pas et dont la
  contribution est une **constante**. Question de **débit**, pas de clé.
- **Le finisseur seul n'a jamais été jugé.** `--no-nrpa` ne le fait pas. La
  mesure existe sans code : `--finisher-min` proche de `--solve-ms`.
- **La sensibilité des 11 paris** du score (100, 10, 3, 1, `hint_bias` 2,0,
  `resolve_weight` 250, les budgets ×0,7 et ×0,8, `qhat_*`, `hindsight_k`).
  Elle est **maintenant faisable** : le mode déterministe existe. C'est le
  premier usage sérieux à en faire.
- **L'étalon B n'a aucun run nu** — `--resolve` y est l'énoncé du but *et* un
  triple indice. Trou de dispositif.
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
  doit JAMAIS mordre). Les mesures de performance restent à seize workers.
- **Sur l'étalon B : N runs par bras, lecture en PROPORTION.**
- **Un mécanisme doit imprimer sa VIE** (un compteur non nul quand il agit)
  avant qu'on mesure son effet. `--assign-bias` était inerte et se déclarait
  allumé ; sans compteur de vie, l'A/B mesure deux fois le témoin.
- Une mesure en cours **verrouille le binaire** (`LNK1104`) : c'est voulu.
- Logs PS en **UTF-16** : `Select-String`, jamais `grep`. Runs séquentiels, un
  `--outdir` par run.
- **CODES et jamais NOMS** : `8379983` Gold Leo · `3027001` Fake Trap ·
  `54701958` Liger · `24550676` Leo · `88753594` Sabre · `81196066` Perfume ·
  `35618217` Kaleido Chick · `2344618` Masquerade · `47705572` Wolf ·
  `90590304` Bagooska.
- **Le premier plan résolu de l'étalon A existe** :
  `D:\ProjectIgnis\replay\2026-08-16 13-19-12.yrpX` (283/283, 0 MSG_RETRY, 2
  Liger). **Corpus et objet d'étude**, pas une solution de la cible — et
  l'employer dans un run NU serait un indice.
- Bancs de la session 18 : `tools/s18_axe1.ps1`, `tools/s18_axe9.ps1`,
  `tools/s18_determinisme.ps1`, `tools/s18_mur.ps1`.

## CE QU'IL NE FAUT PAS REFAIRE

- Chercher le « couplage fantôme » de `--card-on-select` : **il n'existe pas**
  (§9.25 (a)).
- Re-mesurer §9.24 (o) tel quel : un run par bras sur l'étalon B ne rend rien.
- Croire que `--threads 1` suffit au déterminisme : **c'est l'unité du budget**
  qui compte, et c'est corrigé.
- Les réfutations de `docs/drapeaux.md` §A — dix sont désormais hors du code.
- « le décodeur binaire est faux » : audité champ par champ contre
  `playerop.cpp`, **19 messages sur 19 justes** (§9.25 (c)). Les trois
  `ANNOUNCE_*` tuent la branche, mais le compteur vaut **zéro** sur les deux
  étalons.
- « `--max-subsets` explique les trous de couverture » : mesuré, faux.
- « le changement de phase gaspille les tirages » : mesuré, faux — et le drapeau
  est supprimé.
- « la ligne est hors de l'espace d'actions » : mesuré, faux (couverture **100 %
  au board**).

## LIVRABLE ATTENDU

1. **§9.27** — la sonde de conjonction, son histogramme, et le cran de
   diagnostic qu'elle déplace.
2. L'A/B de `--adapt-to-peak` sur plusieurs graines, **et** sur l'étalon B en
   proportion.
3. Le retrait de `--backward` et de `--prior`, santé à l'appui.
4. Ce prompt régénéré.

**Ne pas ajouter de mécanisme tant que la sonde n'a pas parlé.** Trois sessions
ont ajouté des mécanismes à un diagnostic incomplet ; la quatrième sonde est ce
qui manque, et elle coûte moins qu'un mécanisme.
