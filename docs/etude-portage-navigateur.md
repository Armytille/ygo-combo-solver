# Étude — héberger le solveur sur une page web, calcul intégral chez le visiteur

*Question posée : peut-on servir le solveur depuis une page web, le faire tourner
entièrement sur la machine du visiteur, sans sacrifier les performances et sans
réécrire dans un autre langage ?*

*Cette étude ne propose aucun code. Elle inventorie la surface de portage, chiffre
chaque axe sur des mesures prises aujourd'hui (sonde `wasm_mem_probe`, binaire
`s24f`, Ryzen 9800X3D, 16 workers), réfute trois raccourcis, et isole UNE
expérience qui tranche la question en une session — sans écrire une ligne de
JavaScript.*

---

> ## ⚠️ CETTE ÉTUDE A ÉTÉ MESURÉE — ET TROIS DE SES CONCLUSIONS SONT RÉFUTÉES
> Le PoC existe et tourne dans Chrome : voir **`docs/rapport-poc-navigateur.md`**.
> Ce qui tient : le facteur wasm sur le cœur (**×2,01**, prédit ×1,5-2,5), la
> nécessité de COOP/COEP, l'inutilité de `memory64`, la perte du profileur.
> Ce qui tombe :
> 1. **« ×2,5 au plancher, ×6-9 chez le visiteur »** → mesuré **0,93 × le natif**
>    dans Chrome. Le cœur est bien 2× plus lent ; la recherche à 16 workers ne
>    l'est pas, parce qu'elle n'est pas limitée par le cœur.
> 2. **« la copie pleine est réfutée par la bande passante »** (§4A) → c'est le
>    bras qui MARCHE, et il est à parité. La borne de l'instantané valait 4-5 Mo
>    et non 7,38, et 128 Mo de jeu de travail tiennent en L3.
> 3. **« la barrière d'écriture EST le portage »** (§4C) → elle est écrite,
>    vérifiée (0 page manquée en mono-thread), et **inutile**.
> Un quatrième chiffre est faux sans être une erreur de raisonnement :
> la charge utile est de **12,1 Mo** compressés, pas 7 — le corpus de scripts
> complet est nécessaire, l'épinglé seul fait diverger le rejeu en silence.

## 0. LE RÉSULTAT EN UNE PHRASE

Le portage se fait **sans réécriture** — un seul fichier du dépôt est non
portable (`arena.cpp`), `ocgcore` est déjà compilé par clang sous Android, et les
30 942 lignes du solveur ne contiennent **aucun** appel système hors de ce
fichier — mais il bute sur **un seul verrou dur, `GetWriteWatch`**, qui n'a
aucun équivalent en WebAssembly, et dont le repli naïf (copie pleine de la zone
servie) est **réfuté non par le CPU mais par la bande passante mémoire** :
×21,4 sur les octets copiés, soit 4,63 To en 26 s. Avec une barrière d'écriture
logicielle à la place des bits sales matériels, le plancher réaliste est
**≈ ×2,5 sur la même machine** — dont l'essentiel est le facteur wasm lui-même,
et c'est le seul nombre que cette étude ne mesure pas.

---

## 1. CE QU'IL Y A À PORTER — INVENTAIRE

| Composant | TU | Portabilité |
|---|---|---|
| solveur (`*.cpp/*.h`) | 17 | 30 942 lignes ; **zéro** `system()`, `_popen`, `GetTickCount`, `Sleep`, socket |
| `ocgcore` | 18 | **déjà porté clang** — `Android.mk`, `jni/`, gardes `_MSC_VER`/`__builtin_*` dans `common.h` |
| Lua 5.4 (compilé en **C++**) | 28 | `try/catch` interne (`luaconf-customize.h` l.34) → `-fwasm-exceptions` |
| LZMA | 7 | pur C |
| sqlite3 | 1 | lecture seule ; l'amalgamation compile sous emcc, `ws2_32`/`advapi32` tombent |

**63 unités de compilation au total.** Un script de build direct suffit : le
`premake5.lua` actuel ne cible que VS2022, mais il n'y a rien à en tirer pour
emcc — la liste des sources tient en trente lignes.

### Ce qui passe sans une ligne de code

- `search.h:597` — `__popcnt64` a **déjà** la branche `__builtin_popcountll`,
  qui devient l'instruction native `i64.popcnt`. Zéro travail.
- `std::filesystem` — 7 sites, tous compatibles MEMFS.
- `ScriptProvider` a **déjà** un cache mémoire (47 572 relectures disque évitées
  sur le livrable s24, 64 scripts résidents). Le préchargement navigateur est
  trivial : on remplit le cache une fois, l'E/S disparaît par construction.
- Le pool de 16 `std::thread` joignables → `-sPROXY_TO_PTHREAD` (main sur un
  pthread, donc les `join()` bloquants redeviennent légaux).

### Ce qui demande du travail — un seul fichier

`arena.cpp`, et rien d'autre :

| Site | Appel | Substitut wasm |
|---|---|---|
| l.11 | `#include <windows.h>` | — |
| l.81 | `GetSystemInfo` (taille de page) | constante 65536 (page wasm) ou 4096 logique |
| l.90 / 146 / 159 | `VirtualAlloc`/`VirtualFree`/`MEM_COMMIT` | un `malloc` unique ; `EnsureCommitted` devient un simple filigrane |
| **l.323 / 446 / 546** | **`GetWriteWatch` / `ResetWriteWatch`** | **aucun — §3** |
| l.367 | `_BitScanForward64` | `__builtin_ctzll` |
| l.696/755/773/777 | `__rdtsc` | `emscripten_get_now()` — **avec perte, §6** |

La contrainte de conception centrale de l'arène — *restaurer à la même adresse
de base pour que les pointeurs absolus restent valides* — est **gratuitement
satisfaite** en wasm : la mémoire linéaire est un bloc contigu unique, et un
`malloc` fait au démarrage ne bouge jamais (`memory.grow` n'ajoute qu'à la fin).

---

## 2. LE BUDGET MÉMOIRE — MESURÉ, ET IL PASSE

> Sonde `wasm_mem_probe` : étalon B contraint, `--solve-ms 45000`, graine 888,
> 16 workers, mesure du pic par échantillonnage à 500 ms.

```
PEAK WorkingSet : 396 Mo
PEAK Private    : 407 Mo
```

Rapport d'arène du même run :

```
engage       : 8.00 Mo
zone servie  : 7.38 Mo   (borne de l'instantané)
blocs vivants: 4.35 Mo
```

Les « 256 Mo réservés » par worker sont de l'**espace d'adressage**, pas de la
mémoire : le commit réel est de 8 Mo. **407 Mo de commit total pour 16 workers**,
soit ~25 Mo par worker (arène + miroir + journaux + structures de recherche).

**Conséquence : wasm32 suffit largement** (4 Gio d'espace linéaire). Il faut
seulement remplacer la réserve paresseuse par une allocation fixe dimensionnée
au filigrane observé (32 Mo/worker avec marge → 512 Mo), et le mécanisme
d'**empoisonnement** déjà en place (`Arena::NoteFallback`, drapeau collant) donne
exactement le garde-fou voulu : un dépassement s'annonce bruyamment au lieu de
corrompre l'état en silence.

**`memory64` est inutile ici** — et c'est une bonne nouvelle : Chrome le supporte
sans drapeau depuis la 133, Firefox depuis la 143 (parfois encore derrière un
drapeau), mais Safari est très en retard et n'a pris aucun engagement public.

---

## 3. LE VERROU : `GetWriteWatch`

L'arène ne sauvegarde pas le jeu, elle **sauvegarde la mémoire** — parce qu'au
moment d'un `MSG_SELECT_*` des coroutines Lua sont suspendues au milieu d'une
résolution d'effet, avec leurs piles et leurs upvalues. Et pour ne recopier que
ce qui a bougé, `SyncDirty()` interroge le **suivi matériel des pages sales** de
Windows.

En WebAssembly, **il n'y a rien**. Pas de `mprotect`, pas de gestionnaire de
faute de page, pas de bits sales. La proposition *memory-control* — qui
apporterait `memory.protect` et `memory.discard` — n'est **pas expédiée**, et
`memory.protect` y est de surcroît discutée comme *optionnelle*, sujette à test
de fonctionnalité.

### Ce que l'arène coûte AUJOURD'HUI — et ce n'est pas ce qu'on croit

> Profil `[tirages]` de la sonde : 419,572 s CPU cumulées sur 26,3 s de mur.

| sonde | appels | total | µs/appel | part |
|---|---|---|---|---|
| **Process (core)** | 12 022 974 | 345,309 s | 28,72 | **82,3 %** |
| arène Restore | 524 636 | 33,535 s | 63,92 | 8,0 % |
| QueryCodes | 15 985 867 | 12,776 s | 0,80 | 3,0 % |
| recherche (reste) | 16 | 8,568 s | — | 2,0 % |
| Query (zones) | 9 318 280 | 8,301 s | 0,89 | 2,0 % |
| arène Push | 102 910 | 5,809 s | 56,45 | 1,4 % |
| arène Pop | 102 910 | 2,332 s | 22,66 | 0,6 % |
| **arène (total)** | 730 456 | **41,68 s** | | **9,9 %** |

```
pages : 56.4/Restore (524636 appels)   225.1/Push (102910 appels)
```

**L'arène ne pèse que 9,9 % du budget de recherche.** C'est structurel, pas
accidentel : la recherche est à **tirages** (NRPA), pas en profondeur d'abord —
il n'y a que **0,12 Restore et 0,02 Push par décision**. Le « 75 % de surcoût »
imprimé en tête de rapport est une projection du régime *rejeu instrumenté*, où
un instantané tombe à chaque décision. Ce n'est pas le régime qui compte.

On serait donc tenté de conclure : *9,9 %, on peut se permettre de dégrader.*
C'est faux, et c'est le point de l'étude suivante.

---

## 4. LES TROIS REPLIS, ET POURQUOI DEUX SONT RÉFUTÉS

Le juge n'est pas le CPU. **C'est la bande passante mémoire.**

### Trafic actuel

| | appels | octets/appel | total sur 26,3 s |
|---|---|---|---|
| Restore | 524 636 | 56,4 pages = 231 Ko | 121,2 Go |
| Push | 102 910 | 225,1 pages = 922 Ko | 94,9 Go |
| | | **copié** | **216 Go → 8,2 Go/s** |

soit ~16,4 Go/s de trafic lecture+écriture. Confortable, et en grande partie
résident dans les 96 Mo de V-Cache du 9800X3D.

### Repli A — copie pleine de la zone servie · **RÉFUTÉ**

Sans bits sales, `Push` et `Restore` doivent recopier toute la zone servie,
7,38 Mo, à chaque appel :

```
(524 636 + 102 910) × 7,38 Mo = 4 631 Go = 4,63 To  en 26,3 s
                              = 176 Go/s copiés, 352 Go/s de trafic
```

**×21,4 sur les octets.** Un DDR5 grand public délivre 40 à 60 Go/s utiles. Le
run cesse d'être limité par le CPU : il devient limité par le bus, et les 9,26 To
de trafic prennent **≥ 200 s** contre 26,3 s aujourd'hui. Le poste à 9,9 % devient
le poste dominant. **Un argument CPU sur un problème de bande passante donne la
mauvaise réponse** — c'est exactement le piège.

Note secondaire, mais rédhibitoire elle aussi : le journal d'annulation par
niveau passerait de « pages sales » à 7,38 Mo par niveau. À 20 niveaux de pile
et 16 workers, 2,4 Go — wasm32 casse.

### Repli B — détection par comparaison au miroir · **RÉFUTÉ, même cause**

Trouver les pages sales en comparant l'arène au miroir lit 2 × 7,38 Mo par
synchronisation, et il y a au moins une synchronisation par Push/Restore/Pop :

```
730 456 × 14,76 Mo = 10,8 To lus
```

Même ordre de grandeur que le repli A. Le SIMD wasm (`v128`) accélère la
comparaison mais ne réduit pas le **trafic**, qui est le facteur limitant.
Réfuté.

### Repli C — barrière d'écriture logicielle · **LE SEUL VIABLE**

Marquer le bit sale au moment du store, au lieu de le demander au matériel après
coup. Une passe Binaryen/LLVM sur les seules fonctions de `ocgcore` + Lua,
émettant devant chaque store :

```
bitmap[addr >> 12] |= 1 << (…)
```

Deux propriétés rendent cela bon marché :

1. **Le bitmap couvre TOUTE la mémoire linéaire** (512 Mo → 16 Ko de bitmap,
   résident L1). Donc **aucun test d'intervalle, aucun branchement** : un store
   vers la pile C marque un bit sans conséquence.
2. Le trafic reste celui d'aujourd'hui — 8,2 Go/s. On ne paie qu'en
   **instructions**, sur les 82,3 % que représente `Process (core)`.

Coût attendu : +30 à 60 % sur le code instrumenté, soit ≈ ×1,35 sur le total,
contre ×8 pour le repli A. En échange, `GetWriteWatch` disparaît (≈ 7 µs par
appel × 730 456 appels = 5,1 s récupérées) et Push/Restore retombent à leur coût
natif.

**Cette barrière n'est pas une optimisation du portage : c'est le portage.**

---

## 5. LE CHIFFRAGE GLOBAL, HONNÊTEMENT

Trois facteurs se composent, et seul le premier est propre à wasm.

**(a) Le facteur wasm.** Les mesures publiées donnent 1,3 à 2,5× pour du C++
intensif. La boucle de dispatch d'une VM Lua — appels indirects, pointeurs
poursuivis — se situe plutôt en haut de fourchette. **C'est le seul nombre que
cette étude ne mesure pas ; §8 l'obtient en une session.**

**(b) Le nombre de fils.** Référence : 16. `navigator.hardwareConcurrency` chez
le visiteur médian : 4 à 12, et Safari plafonne bas. Facteur ×1,5 à ×4.

**(c) Le cache de la machine de référence, qu'on oublie toujours.** Le jeu de
travail chaud est de 4,35 Mo de blocs vivants par worker, ×16 = 70 Mo. Cela
**tient** dans les 96 Mo de V-Cache du 9800X3D. Cela ne tient dans **aucun**
L3 grand public (8 à 32 Mo). Facteur ×1,3 à ×2 supplémentaire, indépendant de
wasm — la référence est mesurée sur un CPU atypiquement riche en cache.

| | mur (phase tirages) | facteur |
|---|---|---|
| natif, 16 fils, X3D | 26,3 s | 1 |
| wasm + barrière, 16 fils, même machine | ~60-75 s | **×2,3-2,9** |
| wasm + copie pleine, 16 fils, même machine | ~210-260 s | ×8-10 |
| wasm + barrière, visiteur médian (8 fils, L3 normal) | ~150-250 s | **×6-9** |

Traduit sur le livrable s24 (3 rounds, ~1 h de mur, 49,9 M états) : **6 à 9 h
chez un visiteur médian**, tout bien fait. Avec le repli en copie pleine, on est
hors sujet.

**Réponse directe à « sans sacrifier les performances » : impossible au sens
strict.** Le plancher est ×2,5 à machine égale, et le visiteur n'a ni les 16 fils
ni les 96 Mo de L3.

---

## 6. CE QUE LE NAVIGATEUR RETIRE, ET QUI COMPTE POUR **CE** PROJET

### Le juge canonique devient inmesurable

`__rdtsc` n'existe pas. Le substitut est `performance.now()`, dont la résolution
est **bridée à 5 µs en contexte cross-origin isolé** — et il faut l'être pour
avoir les threads (§7). Or le profileur mesure des sondes à **0,18 µs**
(board key), **0,80 µs** (QueryCodes), **0,89 µs** (Query). Elles disparaissent
sous le plancher d'horloge.

**Conséquence méthodologique, et elle est structurante :** le build navigateur
est une **cible de livraison**, jamais un banc de mesure. Toute la discipline
d'A/B — µs/appel, médiane de trois runs intercalés, `tools/s24_perf_mesure.ps1` —
reste native. Cela doit être écrit dans le dépôt le jour du portage, sinon
quelqu'un mesurera un jour dans le navigateur et conclura n'importe quoi.

### Le PGO

Le natif tourne en PGO : 27,2 → 23,6 (LTO) → **18,6 µs/appel** sur l'étalon B.
clang/wasm-ld supporte `-fprofile-generate` / `-fprofile-use`, mais il faut
exécuter l'instrumenté sous node et récupérer le `.profraw` depuis NODEFS.
Faisable, non gratuit, et à faire **avant** de publier un chiffre de comparaison —
sinon on impute à wasm les ~30 % qui reviennent à l'absence de PGO.

### Le déterminisme

Rien ne change : il n'y en avait déjà pas (deux exécutions identiques ne font pas
le même nombre de tirages, même à `--threads 1`).

---

## 7. LE DÉPLOIEMENT — CE QUI CONTRAINT L'HÉBERGEUR

- **COOP/COEP obligatoires.** `SharedArrayBuffer` — donc les pthreads — exige
  `Cross-Origin-Opener-Policy: same-origin` **et**
  `Cross-Origin-Embedder-Policy: require-corp` sur le document.
  → **GitHub Pages est écarté** (pas d'en-têtes personnalisés ; le shim
  `coi-serviceworker` fonctionne mais impose un rechargement et a des angles
  morts). Cloudflare Pages / Netlify / Vercel : fichier `_headers`, réglé.
- **Onglet en arrière-plan.** Un Worker en calcul serré n'est pas throttlé par
  minuterie, mais un onglet caché peut être gelé ou déchargé — sur mobile,
  systématiquement. **Un run d'une heure dans un onglet est un pari**, et un run
  de six heures n'en est pas un : c'est un échec annoncé. Le mode navigateur
  impose de facto des budgets courts, ou une reprise sur archive
  (`--carry`/`--archive-fin` existent déjà, et c'est précisément ce qu'il faut).
- **Charge utile mesurée** : scripts 18 Mo → **1,7 Mo** en `gzip -9` (mesuré, 3 757
  fichiers) ; bases de cartes ~9,9 Mo → ~3 Mo compressées ; wasm ~2-3 Mo en
  brotli (le `.exe` natif fait 4,3 Mo). **Total ≈ 7 Mo**, mis en cache une fois
  (Cache API / IndexedDB). Non bloquant.
- **Entrées/sorties** : `.yrpX` et `.ydk` par `<input type=file>` ; sorties
  (`replay.cpp:369`) vers MEMFS puis zip et téléchargement.

---

## 8. L'EXPÉRIENCE QUI TRANCHE — E1, une session, zéro JavaScript

Tout le chiffrage du §5 repose sur **un** inconnu : le facteur wasm. Il se mesure
seul, et le harnais existe déjà.

> **Montage.** Build wasm **mono-thread**, arène en copie pleine (ou `--no-arena`,
> le drapeau existe), et on n'exécute **que la phase de rejeu instrumenté** — pas
> de recherche, donc pas de dispersion NRPA, ~20 s en natif.
>
> **Juge.** La ligne `Process (core)` de la table `--profile`, colonne µs/appel,
> contre la valeur native de la même phase : **40,07 µs/appel** (148 594 appels,
> `s24_P1_A_r1.log`). Un seul nombre, comparable à l'octet, produit par le
> binaire lui-même.
>
> **Seuils.** ≤ 2,5× → continuer sur E2. 2,5-4× → portage possible mais à budget
> court assumé, sans promesse de parité. ≥ 4× → la question doit être reposée
> autrement (§10).

Coût : installation d'emsdk (absent de la machine), un script de build sur
63 TU, `-fwasm-exceptions`, `-msimd128`. Aucun travail sur l'arène, aucun
threading, aucune page web.

### E2 — la barrière d'écriture, et elle s'arbitre **nativement**

Si E1 passe : la barrière du §4C se mesure **sur le X3D, en µs/appel**, en
passant le build à `clang-cl` — sans emscripten, sans navigateur, avec le juge
canonique intact. On compare l'arène à bits sales matériels contre l'arène à
barrière logicielle sur l'étalon B 60 s, médiane de trois runs intercalés
(`tools/s24_perf_mesure.ps1`, tel quel).

C'est le juge gratuit de cette étude : **le mécanisme le plus risqué du portage
est mesurable avant d'avoir porté quoi que ce soit.**

---

## 9. CE QUI EST RÉFUTÉ

1. **« Il suffira de recompiler. »** Non. `GetWriteWatch` n'a aucun équivalent, et
   la proposition qui l'apporterait n'est pas expédiée.
2. **« L'arène ne pèse que 9,9 %, la copie pleine passera. »** Non. ×21,4 sur les
   octets, 4,63 To en 26 s, 176 Go/s demandés contre ~50 disponibles. Argument
   CPU sur un problème de bande passante.
3. **« La détection par comparaison remplace les bits sales. »** Non. 10,8 To lus,
   même mur.
4. **« Il faut `memory64` pour la place. »** Non : 407 Mo mesurés. Et Safari ne
   suit pas.
5. **« On mesurera dans le navigateur. »** Non. `performance.now()` à 5 µs contre
   des sondes à 0,18 µs. Le navigateur livre, il ne juge pas.
6. **« Sans sacrifier les performances. »** Impossible au sens strict. ×2,5 à
   machine égale est le plancher ; ×6-9 chez le visiteur médian est l'attente
   réaliste.

---

## 10. SI E1 ÉCHOUE — LA QUESTION REPOSÉE

Un facteur ≥ 4× ne condamne pas l'idée d'une page web ; il condamne l'idée d'y
faire tourner **le même run**. Trois formes restent ouvertes, par ordre de coût
croissant, et aucune ne demande de réécriture :

- **Solveur de démonstration.** Budget 30-60 s, un round, `--rounds 1`, sur un
  gabarit fourni. Le navigateur montre le mécanisme et rend des lignes courtes ;
  les runs longs restent natifs. C'est la seule forme qui tient dans un onglet.
- **Reprise sur archive.** `--carry` / `--archive-fin` existent : le visiteur fait
  N rounds courts, l'archive persiste en IndexedDB entre les sessions. Un run
  d'une heure devient dix runs de six minutes, ce qui est compatible avec le
  cycle de vie d'un onglet.
- **Réduire le nombre d'instantanés**, pas leur coût. 19 948 Restore/s et
  50,1 % de tirages ré-entrants sont des paramètres de recherche, pas des
  constantes. Mais c'est un changement d'algorithme, à mesurer nativement sur les
  juges du projet — et donc hors du portage.

---

## 11. VERDICT

| Axe | Verdict |
|---|---|
| Réécriture | **Aucune.** Un fichier (`arena.cpp`) + un script de build. |
| Mémoire | **Passe.** 407 Mo mesurés, wasm32 suffit, `memory64` inutile. |
| Charge utile | **Passe.** ~7 Mo compressés, mis en cache. |
| Threads | **Passe**, au prix de COOP/COEP → hébergeur à en-têtes (pas GitHub Pages). |
| Exceptions / SIMD / popcount | **Passent** sans travail. |
| `ocgcore` / Lua / LZMA / sqlite | **Passent** — clang les compile déjà. |
| **Instantané d'arène** | **Verrou dur.** Une barrière d'écriture est obligatoire ; les deux replis simples sont réfutés par la bande passante. |
| Mesure | **Perdue** dans le navigateur. Le banc reste natif, définitivement. |
| Performances | **×2,5 au plancher**, ×6-9 chez le visiteur médian. |

**Faisable, sans réécriture, à condition d'écrire une barrière d'écriture — et
en acceptant un facteur qu'aucun travail d'ingénierie ne ramènera à 1.**

Prochain pas : **E1**, une session, aucune ligne de JavaScript.
