# Session 17 — LE PREMIER EXEMPLAIRE, ET LE TROISIÈME DECK

Tu reprends `combosolver` (racine `d:\ProjectIgnis\replay2video\combosolver`).
Lis `README.md` puis `docs/combo-solver-design.md` **§9.23** (session 16) en
entier, puis §9.22. **Ne redécouvre rien de ce qui y est chiffré, et ne
re-cherche pas la littérature : elle est en §9.22 (h) et §9.23 (c).**

## CE QUE LA SESSION 16 A RETOURNÉ

La session 15 avait nommé le mur — « atteindre un sous-but consomme ce dont le
suivant a besoin » — et posé la question : *le deuxième exemplaire est-il **jamais
tenté** ou **toujours perdu** ?* La sonde a été construite (`--probe-repeat`) et
elle répond : **ni l'un ni l'autre. La question présupposait un premier
exemplaire, et il n'y en a pas.**

Étalon A, deck et main NEUFS (`Gold Leo` + 3 `Fake Trap`), 300 s, graine 888 :

| carte surveillée | tirages : ≥1 / ≥2 / ≥3 | finisseur enraciné : ≥1 / ≥2 |
|---|---|---|
| Lunalight Masquerade | 362 008 / 359 506 / **290 463** | 59 036 / 137 |
| Lunalight Wolf | 350 015 / 145 780 / 8 819 | 7 913 / 4 117 |
| **Lunalight Liger Dancer** | **0 / 0 / 0** | **62 / 0** |

**L'échantillonnage fabrique TROIS Masquerade dans 290 463 tirages et pas UN SEUL
Liger sur 930 676.** Le contrôle est dans la même table et le même run : ce n'est
pas « le solveur ne sait pas répéter ». Même motif sur l'ancien départ
(Masquerade 99,3 % de répétition, Wolf 49,2 %, Liger 0 puis 3 767 → 0).

**LA CIBLE EST FAISABLE — vérifié en séance, ne pas y revenir.** L'arithmétique
apparente ne ferme pas (Liger exige Leo Dancer, Leo Dancer exige Panther Dancer
qui est absent du deck, et l'extra n'a que 2 Leo Dancer pour 3 Liger). Réponse de
l'opérateur, confirmée par le texte des cartes : Kaleido Chick se substitue à Leo
Dancer par copie de nom, et **`Lunalight Wolf` (effet Pendule) invoque une Fusion
en bannissant les matériaux depuis le terrain OU LE CIMETIÈRE**, tandis que
`Lunalight Masquerade` autorise également les matériaux du cimetière. Les Leo
Dancer envoyés au cimetière redeviennent donc des matériaux. **Tout échec de
l'étalon A est imputable au solveur.** Corollaire : les trois `--resolve` de
l'étalon A sont exactement les activateurs de matériaux depuis le cimetière —
d'où le « retirer `--resolve` fait tomber les Liger à zéro » de 9.22 (i), qui
n'avait jusqu'ici pas d'explication.

## LA MISSION — DEUX CHANTIERS, DANS CET ORDRE

### 1. POURQUOI LE PREMIER LIGER N'ARRIVE JAMAIS

> **L'échantillonnage accomplit ses trois activateurs 87 785 fois et ne fabrique
> pas une seule Fusion cible. Trouver où la ligne meurt, et le mesurer.**

C'est la question que la sonde a ouverte, et elle est plus précise que tout ce
que les sessions 8-15 ont pu poser. Trois pistes, la première étant la moins
chère :

- **LA LÉGALITÉ, PAS L'ÉCHANTILLONNAGE ?** Le compteur `hint_seen`/`hint_taken`
  existe déjà et il agrège toutes les cartes indicées ensemble. Le décomposer
  PAR CARTE dirait si l'invocation de Liger est jamais seulement PROPOSÉE par un
  prompt. Si elle ne l'est jamais, le problème n'est pas la recherche — c'est
  que l'état requis n'est jamais atteint, et il faut remonter d'un cran.
  **Attention au piège mesuré en s16** : un choix qui « engage » une carte n'est
  pas forcément une invocation de cette carte, et une Fusion passe par
  l'activation du sort, pas par un choix portant le nom du monstre. Décomposer
  par GROUPE de `MSG_SELECT_IDLECMD` (0 invocation, 1 inv. spéciale, 5
  activation), pas par `Choice::card` seul.
- **LA PROFONDEUR.** Sur l'ancien départ, après la première Liger il ne reste que
  **10 décisions** au tirage, contre 89 après une Masquerade — alors que la
  référence met ~16 décisions par invocation. Mesurer où meurent les tirages qui
  ont accompli les trois activateurs : coupure de tour, impasse, plafond ?
- **LE `h` PLAT, ENCORE.** Rien dans le score ne récompense « Leo Dancer au
  cimetière ». C'est ce que le graphe de landmarks devait apporter, et il n'a pas
  pu être jugé sur l'étalon A faute de plan résolu (voir ci-dessous).

### 2. LE TROISIÈME DECK — la seule mesure de généralité qui existe

> **Question posée par l'opérateur en fin de session 16 : « est-ce un solveur
> générique pour n'importe quel deck, ou du réglage fin sur nos deux decks de
> test ? »** §9.23 (g) y répond en deux moitiés : le MOTEUR est générique (aucune
> connaissance de carte compilée, tout le spécifique est une entrée de ligne de
> commande) ; les PREUVES ne le sont pas (constantes calibrées sur deux decks,
> liste `--hint` injectée à la main, et **aucune mesure sur un troisième deck**).

Le chantier : **un troisième deck, sans rapport avec les deux autres, avec sa
propre ligne de référence.** Il rend possible le seul juge de généralité qui
vaille — apprendre les landmarks sur A et B, les **servir sur C**. Demander le
replay à l'opérateur ; le solveur le juge d'abord (`--guard`, `--resolve`,
`MSG_RETRY`) avant d'en faire un étalon.

## L'ÉTAT DES MÉCANISMES DE LA SESSION 16

- **`--probe-repeat` : l'instrument, il MARCHE et il a rendu son verdict.** Deux
  tables (phase tirages, tirages enracinés), histogramme brut par entrée
  surveillée. Trois défauts d'instrument corrigés en séance, dont un introduit
  par le correctif d'un autre — lire §9.23 (a) avant de le modifier.
  **RÉSERVE : son axe « distance de recettes » ne rend AUCUN verdict** et le dit
  lui-même. Cause : les recettes amorcées portent la zone JOKER, qui accepte le
  cimetière — le matériau qu'on vient de consommer y est justement arrivé, donc
  « un exemplaire de plus » paraît toujours à une invocation près. Une distance
  uniformément égale à 1 est la signature de ce biais. **Le réparer demande de
  n'utiliser que les recettes OBSERVÉES** (qui portent des zones concrètes) pour
  cette question précise.
- **`--landmarks` / `--landmark-w` / `--landmark-h` : ÉCRITS, INSTRUMENTÉS, NON
  DÉMONTRÉS.** Le graphe apprend la bonne chose — sur l'étalon B, depuis deux
  lignes résolues, il sort `Fake Trap @ADV banni x3` à l'ordre 0,54, c'est-à-dire
  **le handrip lui-même en landmark compté**, sans qu'aucune carte soit nommée
  dans le code. Mais l'A/B ne conclut pas : `>=3` est un événement rare, nul dans
  quatre runs sur six, non nul dans **deux bras différents à deux budgets
  différents** (`lmw60` à 90 s, `lmx60` à 300 s), sur une seule graine.
  **À refaire : plusieurs graines, et le bras qui compte est `lmx60`** (landmarks
  appris sur une LIGNE TENUE À L'ÉCART de celle qu'on juge).
- **UN TROU D'INSTRUMENT CORRIGÉ APRÈS L'A/B** : la ligne « landmarks : h moyen »
  n'était imprimée que par `PrintCuts`, qui ne couvre pas la phase tirages —
  donc pas la phase où `--landmark-w` travaille. Elle l'est maintenant, mais
  **l'A/B de la s16 a tourné sans elle** : on ne sait pas si le `h` appris a
  réellement décru. **Première chose à relever au prochain A/B.**
- **`--archive-spread` : DÉPARTAGÉ.** Son critère interne se confirme largement
  sur un juge valide (médiane d'expansions par racine **16 → 164**, ×10) ; sur
  les juges de recherche il est **neutre à légèrement négatif** (0 ligne, ≥3
  inchangé à 0). Il fait ce qu'il annonce, et ce qu'il annonce ne convertit pas.
  Reste opt-in. **`--no-phase-change` et `--canonical-zones` restent non
  départagés.**
- **LIMITE DE FOND DU GRAPHE DE LANDMARKS, à traiter** : il exige un plan
  RÉSOLU, donc il ne sert à rien À FROID — et l'étalon A n'a aucun plan résolu,
  donc le mécanisme n'y est pas jugeable. La voie naturelle est de le miner **en
  ligne** sur les meilleures approches du run lui-même, exactement comme
  `--options-online` le fait pour les macros.

## CE QUI EST ACQUIS — ne pas re-mesurer, ne pas re-chercher

- **Bibliographie faite** (§9.22 (h), §9.23 (c)) : Bonet & Geffner
  (arXiv:2311.05490, largeur sérialisée) ; Drexler, Seipp & Geffner
  (arXiv:2105.04250, « SIW échoue quand un sous-problème a une largeur élevée ») ;
  Ståhlberg & Geffner (arXiv:2512.19355, « les modèles apprennent à en livrer un
  seul ») ; Hanou, Dumančić & de Weerdt (arXiv:2508.21564, landmarks généralisés).
  **Trois résultats NÉGATIFS à ne pas re-chercher** : la littérature des jeux de
  cartes porte sur l'information cachée ou le deckbuilding ; le nogood/CDCL vit
  dans des solveurs déclaratifs, or nos actions sont des scripts Lua ;
  « ressources consommables + heuristique » ne ramène que du réseau et du
  matériel. Piste secondaire non explorée : **arXiv:2601.21684, *Do Not Waste
  Your Rollouts*** — recyclage NÉGATIF des motifs d'échec, sans entraînement.
- **RÉFUTÉS** : `--options-ctx` en ligne ; `--options-len 16/24` ;
  `--options-pool 24` ; `--mcps` (deux fois) ; `--nrpa-lr 2` ;
  `--novelty-rollout-cut` (criblage 90 s : ≥1 résolution 206 093 → **9**).
- **LES CONTRAINTES GUIDENT** — `--resolve` / `--summon-min` / la garde ne font
  pas qu'élaguer : le bras le plus contraint monte PLUS HAUT. **Ne jamais
  « simplifier » en les retirant.** La session 16 a trouvé POURQUOI sur
  l'étalon A : ces trois cartes sont les activateurs de matériaux depuis le
  cimetière.
- **`Q̂` (`--qhat`) est implémentée fidèlement** et trouve la bonne ouverture
  seule (§9.22 (b)-(c)). **`--finisher-options`** : écrit, **toujours jamais
  jugé**.
- **Plafond de représentation** : accord du corpus 52 → 59 → 64 (mémorisation
  pure). Aucun meilleur descripteur de contexte ne sauvera la masse.

## MÉTHODE — ce qui a payé en session 16

- **L'INSTRUMENT AVANT LE MÉCANISME, et l'instrument lui-même se met en doute.**
  Trois défauts trouvés sur la PREMIÈRE lecture de la sonde, dont un introduit
  par le correctif d'un autre. Sans cette relecture, la session aurait publié
  « matériau conservé à 100 % » — un verdict fabriqué à partir d'une absence de
  mesure.
- **LIRE LE TEXTE DES CARTES AVANT DE DÉPENSER DES RUNS.** La faisabilité de la
  cible A a été tranchée par une requête `.cdb` et une question à l'opérateur,
  pas par un run.
- **UN CRIBLAGE 90 s NE PROMEUT PAS.** Il avait donné une belle réponse en dose
  (0 → 28 sur `>=3`) que le budget long n'a pas confirmée. **Le criblage ÉLIMINE,
  il ne promeut jamais** — la règle était écrite, elle vient d'être payée.
- **LA GRAINE EST UN TIRAGE, PAS UN CADRAN.** Le juge est la fraction de tirages
  qui convertissent, jamais « k/4 sur la graine 888 ». Et fixer la graine ne rend
  pas le run reproductible (échanges asynchrones entre workers) : c'est un
  **défaut du solveur**, pas une propriété de l'instrument.
- **NE JAMAIS CHANGER DEUX FACTEURS.** La session 16 n'a pas re-déroulé de
  pipeline PGO, précisément pour que l'A/B reste apparié sur un binaire unique.
  Conséquence assumée et écrite : les DÉBITS ne sont pas comparables à la s15.

## PIÈGES DE BUILD ET D'ENVIRONNEMENT

- Un `MSBuild` ordinaire RELIE SANS `/USEPROFILE` : le binaire courant est
  **non-PGO**. Témoins PGO : `combosolver_preopt`, `_lto_s12`, `_lto_s13`,
  `_lto_s14`, `_lto_s14b`, `_lto_s15` (`tools/s15_pgo.ps1` comme modèle — ne
  jamais écraser le témoin précédent).
- **Une mesure en cours VERROUILLE le binaire** (`LNK1104`). C'est voulu.
  Attendre : `while (Get-Process combosolver) { Start-Sleep 15 }`.
- **Le gabarit est ÉPINGLÉ** (`gabarits/etalon_a_lunalight.yrp`). **Le DECK ne
  l'est pas** : `D:\ProjectIgnis\deck\Lunalight.ydk` a changé le 16/08/2026 à
  13:19, en pleine session. La `.cdb` non plus.
- **Les noms de cartes sont AMBIGUS** : « Lunalight Gold Leo » existe en 8379983
  ET 101301005 ; « Junk Meister » et « Crimson Dragon » l'étaient déjà. Toujours
  passer les CODES pour l'étalon A.
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
    --outdir s17_sante --no-chain Zalen --no-chain "Crystal Wing"
# 273 digests, 210/273, 209 candidates, 16 replays sur 209.

# ETALON A, depart NEUF (deck refait + main Gold Leo/Fake Trap) :
.\tools\s16_sonde.ps1 -Ms 300000 -Cas A -Prefixe s17

# ETALON B OPTIMISE — le seul cas ou UNE SOLUTION EXISTE :
.\tools\s16_ab.ps1 -Ms 300000 -Bras temoin,lmx60 -Seeds 888,1234,4242
```

Outils : `s16_sonde.ps1` (sonde de répétition, étalons A et B),
`s16_ab.ps1` (A/B étalon B, bras nommés dont `lmx60` à ligne tenue à l'écart),
`s16_lecture.ps1` (juges + vie du mécanisme + expansions du finisseur),
`s15_ab.ps1`, `s15_lecture.ps1`, `s15_curriculum.ps1`, `s15_courbe_qhat.ps1`,
`s15_pgo.ps1`, `s14_accord_mcps.ps1`, `s14_aretes_macro.ps1`,
`s13_exploitation_longue.ps1`, `s14_parallelisme.ps1` (jamais mesuré).

## Définition de « terminé »

(a) **Où meurt la ligne avant la première Fusion cible**, mesuré et non supposé —
avec le compteur qui sépare « jamais proposée par un prompt » de « proposée et
jamais prise ».
(b) Un **troisième deck** au dossier, jugé par l'outil, et le premier transfert
de landmarks A+B → C.
(c) L'A/B des landmarks **refait sur plusieurs graines**, avec la ligne
« landmarks : h moyen » lue en premier.
(d) §9.24 documenté, ce prompt régénéré.
