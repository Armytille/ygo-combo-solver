# Session 19 — RÉPARER LE JUGE, PUIS LE CRÉDIT, PUIS COUPER LE GRAS

Tu reprends `combosolver` (racine `d:\ProjectIgnis\replay2video\combosolver`).

**CE N'EST PAS UNE SESSION DE MÉCANISME.** Aucun drapeau de recherche de plus.
La session 18 a audité le solveur et a trouvé que **l'instrument qui devait juger
les mécanismes rend cinq valeurs différentes pour la même commande**. Tant que
ce n'est pas réparé, tout mécanisme ajouté sera jugé à pile ou face.

Lis `README.md` (section « La règle de mesure »), puis
`docs/combo-solver-design.md` **§9.25** en entier, puis `docs/drapeaux.md`. La
bibliographie est faite (§9.22 (h), §9.23 (c), §9.24) : ne la refais pas.

## CE QUE LA SESSION 18 A ÉTABLI

| fait | valeur |
|---|---|
| étalon B, **même commande, même graine**, `≥1` sur 5 runs | **0 · 0 · 0 · 89 · 2 239** |
| `--threads 1`, même graine, tirages sur 2 runs | **41 232 · 42 179** |
| coût mesuré du mono-worker | ÷5,1 à ÷5,9 (et non ÷16) |
| drapeaux à supprimer (réfutés / départagés négatifs) | **17** sur 122 |
| drapeaux jamais jugés | **27** |
| constantes du score qui sont des **paris non mesurés** | **11** sur 22, dont les quatre du cœur (100, 10, 3, 1) |
| `Choice::card_lossy` | déclaré, remis à zéro, lu — **jamais assigné `true`** |

**Le couplage fantôme n'existe pas.** Sous `--hint-bias 0`, `--card-on-select`
est sémantiquement inerte (le seul canal de lecture de `Choice::card` est annulé)
— et les deux bras tombent quand même dans des états opposés. §9.24 (o) est
retiré : ni `--assign-bias` ni `--card-on-select` n'ont été réfutés sur B, et
`--elide-forced --hindsight` n'y a rien confirmé.

---

## CHANTIER 1 — LE BUDGET EN TIRAGES (la porte de tout le reste)

Le non-déterminisme n'est **pas** l'échange entre workers : c'est que le budget
est du **temps de mur**. Deux runs `--threads 1` à la même graine ne font pas le
même nombre de tirages, donc ne s'arrêtent pas au même point de la trajectoire.

*À faire* : un budget en **tirages** (ou en nœuds — `max_nodes` existe déjà dans
`SearchConfig`, `search.h`, et **n'est exposé par aucun drapeau**). Avec
`--threads 1`, les deux ensemble doivent donner un run **identique à l'octet
près**, diff de relevés à l'appui (hors durées).

*Le contrôle* : `Compare-Object` sur deux relevés complets. La session 18 a
mesuré 38 lignes de différence sur 392 ; l'objectif est **zéro**, hors lignes de
durée.

*Ce que cela coûte, et il faut le mesurer* : le débit par run et le temps de
mur pour un budget donné. Un mode déterministe lent reste utile — c'est le seul
mode où un A/B fin veut dire quelque chose.

*Réserve à écrire d'avance* : un run mono-worker déterministe **n'est pas le run
de production**. Il sert d'instrument d'attribution ; les mesures de performance
restent à seize workers, et se lisent en proportion.

## CHANTIER 2 — LE CRÉDIT : TRONQUER `run.steps` À L'ARGMAX

Le défaut est en deux lignes, et il explique le régime d'échec :

- `search.cpp:2912` — `if(sc > run.score) run.score = sc;` : le score du tirage
  est le **MAX** le long de la ligne ;
- `search.cpp:3422` — `for(const PolicyStep& s : run.steps)` : `AdaptRun`
  parcourt **tous** les pas, **sans troncature**.

Une ligne qui culmine à 7/8 au pas 200 puis erre 230 pas voit ses 430 pas
récompensés à `+alpha`. La politique apprend l'effondrement aussi fort que la
montée. Signature dans les relevés : le régime d'échec de l'étalon B écrit une
approche de **434 décisions** (le plafond est 435) à 3/8, contre 189-258
décisions à 7/8.

*À faire* : retenir l'indice du maximum pendant le tirage, tronquer `run.steps`
à cet indice avant `AdaptRun`. Un drapeau d'A/B (`--adapt-to-peak`, éteint par
défaut), et **une mesure au chantier 1**, c'est-à-dire en mode déterministe.

*Le juge* : sur l'étalon A, les poses de Sabre Dancer (arité 3) et les
invocations, qui ne dépendent pas du juge de B. Sur l'étalon B, N runs et la
proportion de réussites.

*Piège à éviter* : le bandit `Q̂` a déjà sa propre récompense (`qh_reward`,
`search.cpp:2918`) et `AdaptRun` saute déjà les pas `bandit`. Ne pas tronquer
avant ce filtre.

## CHANTIER 3 — SUPPRIMER LES 17

`docs/drapeaux.md` liste les 17, avec le verdict écrit et sa référence. **Un
drapeau à la fois, santé avant et après.** Supprimer un mécanisme réfuté n'est
pas une perte : c'est retirer une branche morte que chaque lecture future devra
sinon ré-évaluer.

Commencer par ceux dont le retrait simplifie le plus : `--recipe-w` (un terme du
score des tirages), `--backward` (une partition ×64 de la table de nouveauté),
`--canonical-digest` (un second chemin dans `StateDigest`), `--archive-spread`
(un quota par niveau dans l'archive), `--mcps` (le conditionnement par le
chemin).

Et **`Choice::card_lossy`** : code mort, à supprimer avec son commentaire, qui
décrit un comportement qui n'existe pas (`search.cpp:3028`).

## CHANTIER 4 — LES DEUX INSTRUMENTS QUI MENTENT ENCORE

1. **`hint_seen` (« visibilité des indices »)** a trois causes à la fois : le
   drapeau `--card-on-select`, la qualité du run, et le volume de travail. Il
   rend 4 avec le drapeau à un worker et 64 617 sans le drapeau à seize. **Le
   ventiler par TYPE de prompt** — le même correctif que la sonde d'offre a reçu
   en §9.24 (a), sur le même défaut, deux sessions plus tard.
2. **`forced_default`** est incrémenté (`search.cpp:507`) **avant** d'essayer
   `DefaultResponse` ; quand celui-ci échoue la branche meurt et compte **aussi**
   en `dead_ends`. Le texte imprimé — « réduits à LA réponse par défaut » — est
   faux pour les trois `ANNOUNCE_*`, où la branche est **supprimée**. Séparer les
   deux compteurs.

Et poser le compteur **manquant** nommé en §9.25 (d) : combien d'états la
transposition **ré-explore** faute d'un budget suffisant à la première visite
(`search.cpp:2170-2177` stocke `disc + 1`). `stats.transpositions` ne compte que
la coupure ; on ne sait pas si le mécanisme paie.

---

## CE QUI RESTE OUVERT, ET QUI N'EST PAS DE CETTE SESSION

- **`ProcessorState` est-il encore nécessaire ?** §9.24 (j) le mesure à ×1,00 aux
  points idle et dominant aux prompts intermédiaires — or `--elide-forced` retire
  74,6 % de ces prompts de la table. La justification du patch C1 n'a pas été
  re-mesurée depuis. Une mesure d'un bras, quand le juge sera réparé.
- **L'adversaire dans la clé.** `StateDigest` (`search.cpp:619`) boucle sur les
  deux joueurs : douze `Query` et deux `Count`, dont la moitié sur un adversaire
  qui ne joue pas. En solitaire, sa contribution est une **constante** — zéro
  entropie, la moitié du coût. C'est une question de **débit**, pas de clé.
- **Le finisseur seul n'a jamais été jugé.** `--no-nrpa` ne le fait pas (il
  remplace NRPA par du glouton, `main.cpp:7074`). La mesure existe sans code :
  `--finisher-min` proche de `--solve-ms` affame les tirages (`main.cpp:6917`).
- **La sensibilité des 11 paris** (100, 10, 3, 1, `hint_bias` 2,0,
  `resolve_weight` 250, ×0,7 et ×0,8 des budgets, `qhat_*`, `hindsight_k`).
  **Suspendue jusqu'à la réparation du juge** : un balayage sur un instrument à
  événement rare produirait une carte de bruit qu'on prendrait pour une carte de
  réglage.
- **L'étalon B n'a aucun run nu.** `--resolve` est un indice déguisé — biais
  d'office, gradient de 250, exigence au but — et il **est** l'énoncé du but sur
  B. La question « le solveur trouve-t-il seul ? » n'y a jamais été posée. C'est
  un trou de dispositif, pas un réglage.
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
- **Supprimer un drapeau est un changement comme un autre** : santé avant/après.
- Une mesure en cours **verrouille le binaire** (`LNK1104`) : c'est voulu.
- Logs PS en **UTF-16** : `Select-String`, jamais `grep`. Runs séquentiels, un
  `--outdir` par run.
- **Sur l'étalon B : N runs par bras, lecture en PROPORTION.** Un run par bras ne
  mesure que le tirage (§9.25 (b)).
- **CODES et jamais NOMS** : `8379983` Gold Leo · `3027001` Fake Trap ·
  `54701958` Liger · `24550676` Leo · `88753594` Sabre · `81196066` Perfume ·
  `35618217` Kaleido Chick · `2344618` Masquerade · `47705572` Wolf ·
  `90590304` Bagooska.
- **Le premier plan résolu de l'étalon A existe** :
  `D:\ProjectIgnis\replay\2026-08-16 13-19-12.yrpX` (283/283, 0 MSG_RETRY, 2
  Liger). C'est un **corpus et un objet d'étude**, pas une solution de la cible.
- Bancs de la session 18 : `tools/s18_axe1.ps1` (attribution du couplage),
  `tools/s18_axe9.ps1` (recensement + déterminisme). Les rejouer coûte ~16 min.

## CE QU'IL NE FAUT PAS REFAIRE

- Chercher le « couplage fantôme » de `--card-on-select` : **il n'existe pas**,
  et la démonstration est en §9.25 (a).
- Re-mesurer §9.24 (o) tel quel : un run par bras sur l'étalon B ne rend rien.
- La bibliographie (§9.22 (h), §9.23 (c), §9.24).
- Les réfutations de `docs/drapeaux.md` §A.
- « `--max-subsets` explique les trous de couverture » : mesuré, faux.
- « le changement de phase gaspille les tirages » : mesuré, faux — la coupure de
  tour à 100 % est la fin **normale** d'un tirage.
- « la ligne est hors de l'espace d'actions » : mesuré, faux (couverture **100 %
  au board**).
- « le décodeur binaire est faux » : audité champ par champ contre
  `playerop.cpp`, **19 messages sur 19 justes** (§9.25 (c)). Les trois
  `ANNOUNCE_*` tuent la branche, mais `forced_default` vaut **zéro** sur les deux
  étalons : c'est un risque latent, pas le mur.

## LIVRABLE ATTENDU

1. **§9.26** — un mode déterministe, son coût mesuré, et le diff à zéro.
2. **L'A/B de la troncature à l'argmax**, mené en mode déterministe.
3. **Les 17 suppressions**, avec la santé de chacune.
4. Les deux instruments corrigés, et le compteur de ré-exploration posé.
5. Ce prompt régénéré.

**Ne pas ajouter de mécanisme de recherche.** Les chantiers 1 et 4 sont des
instruments, le 2 est un correctif de crédit, le 3 est du retrait. Si l'audit
d'une session ultérieure désigne un mécanisme, il aura un juge pour le mesurer —
et c'est tout l'objet de celle-ci.
