# Rapport — PoC WebAssembly : le solveur tourne dans un navigateur

*Suite de `docs/etude-portage-navigateur.md`. L'étude chiffrait sur papier ; ici
on a construit, et on a mesuré. **Trois de ses prédictions sont réfutées par la
mesure**, dont la principale.*

---

> ## ⚠️ CHIFFRES DÉPASSÉS PAR L'OPTIMISATION — voir `docs/rapport-optimisation-wasm.md`
> Deux chiffres de ce rapport sont faux, et pour la même raison : ils viennent
> d'un juge à ±25 % de dispersion.
> - **« ×2,01 sur le cœur »** mesurait le binaire INSTRUMENTÉ par la barrière.
>   Sans elle et avec LTO : **×1,31**.
> - **« le navigateur est 25 % sous node »** n'existe pas : sur un juge à ±1 %,
>   0,234 contre 0,237 ms/décision.
> Le débit, lui, tient : **0,88 × le natif** sur 7 runs par bras (au lieu de
> 0,93 sur 3 runs, plages alors superposées).

## 0. LE RÉSULTAT EN UNE PHRASE

Le solveur tourne **de bout en bout dans Chrome**, sur seize Web Workers, avec un
rejeu **bit-identique au natif** (`MSG_RETRY 0`, empreintes
`26513409f1c5dbbc` / `7a3e73765d0df4ba`, tests de fidélité et de stress 3/3), et
il rend **93 % du débit natif** — là où l'étude prévoyait ×2,5 au mieux et ×6-9
chez le visiteur. Le mécanisme que l'étude jugeait indispensable — la barrière
d'écriture — s'avère **inutile** : le repli qu'elle avait réfuté par la bande
passante est celui qui marche.

---

## 1. CE QUI EXISTE MAINTENANT

| Fichier | Rôle |
|---|---|
| `wasm/build_wasm.ps1` | build des 57 unités, cibles `node` et `web`, leviers de barrière paramétrables (`-Parts`), threads, mémoire |
| `wasm/patch_features.py` | rapiéçage de la section `target_features` d'objets wasm (voir §5) |
| `wasm/assets/` | 22 302 scripts fusionnés **dans l'ordre de priorité du natif** + les 34 bases de cartes |
| `wasm/web/index.html` | la page : bouton, sortie, `hardwareConcurrency`, `crossOriginIsolated` |
| `wasm/web/serve.py` | serveur statique posant COOP/COEP (sans quoi : pas de threads) |
| `wasm/web/mesure.mjs` | pilote Playwright : lance, attend, extrait les compteurs |
| `arena.cpp` / `arena.h` | dos WebAssembly, barrière d'écriture, **vérificateur de barrière** |

Le natif n'est pas touché : tout est sous `#if defined(__EMSCRIPTEN__)`.

---

## 2. LA CORRECTION, D'ABORD

Aucune mesure ne vaut sans elle. Trois juges, tous du harnais existant :

```
  reponses consommees : 290 / 290
  MSG_RETRY           : 0   (rejeu fidele)
  empreinte board cible     : 26513409f1c5dbbc vs 26513409f1c5dbbc
  empreinte etat final      : 7a3e73765d0df4ba vs 7a3e73765d0df4ba
  => IDENTIQUE : l'instantane capture bien tout l'etat du duel
  freres successifs (Push/Restore/Pop)        36/36  ok
  imbrication profonde (20 niveaux)           20/20  ok
  ligne complete rejouee apres stress          1/1   ok
```

Ce sont les empreintes du **natif**, à l'octet, obtenues **dans Chrome**, sur les
3 runs. Le portage ne dérive pas.

---

## 3. LE DÉBIT — 3 RUNS PAR BRAS, PARAMÈTRES IDENTIQUES

> Étalon B contraint, graine 888, 16 workers, `--arena-mb 16`, phase de tirages
> de 35,0 s. Juge : le **compteur exact d'états**, pas l'horloge — la table
> `--profile` est inutilisable en wasm threadé (§6).

| bras | états (3 runs) | médiane | vs natif |
|---|---|---|---|
| **natif** (MSVC, LTO) | 5 384 807 / 5 351 379 / 5 641 659 | 5 384 807 | 1,00 |
| **wasm sous node** | 6 679 791 / 6 809 537 / 5 962 651 | 6 679 791 | **1,24** |
| **wasm dans Chrome 145** | 4 769 367 / 6 098 230 / 5 010 850 | 5 010 850 | **0,93** |

**Le navigateur rend 93 % du natif.** Et le même binaire wasm sous node en rend
124 % — les trois runs au-dessus du maximum natif, ce n'est pas du bruit.

### Le juge déterministe dit pourtant ×2

Sur la phase de **rejeu** (mono-thread, nombre d'appels identique à l'unité) :

| | appels | µs/appel |
|---|---|---|
| natif | 3 188 | **44,47** |
| wasm | 3 188 | **89,51** |

**×2,01 sur le cœur.** Les deux mesures sont vraies et ne se contredisent pas :
le cœur wasm est bien deux fois plus lent, mais **la recherche à seize workers
n'est pas limitée par le cœur**. Ce qu'elle attend, wasm l'attend aussi bien que
le natif — et la bibliothèque standard de clang le lui rend.

*Piste NATIVE, pas wasm :* les 24 % que node prend au natif à code source
identique désignent la pile MSVC (STL + tas CRT) sous contention à seize
threads. C'est un gain natif potentiel, mesurable avec `clang-cl`.

---

## 4. CE QUE LA MESURE RÉFUTE DE L'ÉTUDE

### (1) « ×2,5 au plancher, ×6-9 chez le visiteur » — **RÉFUTÉ**

Mesuré : **0,93** dans Chrome, sur la même machine. Le raisonnement était juste
sur le cœur (×2,01, confirmé) et faux sur ce qui en découle.

### (2) « La copie pleine est réfutée par la bande passante (×8-10) » — **RÉFUTÉ**

C'est le bras qui marche, et il est à parité. L'erreur était double :

- la borne de l'instantané a été prise à **7,38 Mo** (un run lourd) ; sur
  l'étalon B elle vaut **4 à 5 Mo** (mesuré : 1024 pages par Push) ;
- le trafic a été compté contre la DRAM alors que 16 workers × (4 Mo d'arène +
  4 Mo de miroir) = **128 Mo**, largement servis par le L3.

Et surtout : le calcul supposait le poste dominant. Il ne l'est pas — les
instantanés restent à 0,07 Restore par décision.

### (3) « ≈ 7 Mo de charge utile » — **RÉFUTÉ, c'est 12,1 Mo**

| | brut | gzip -6 |
|---|---|---|
| `combosolver.wasm` | 3,39 Mo | **1,24 Mo** |
| `combosolver.data` (22 302 scripts + 34 bases) | 60,44 Mo | **10,62 Mo** |
| `combosolver.js` | 1,46 Mo | 0,25 Mo |

Le jeu de scripts **épinglé** ne suffit pas : 29 scripts manquaient et le rejeu
divergeait en silence (253 `MSG_RETRY`). Il faut le corpus complet, fusionné
**dans l'ordre de priorité du natif** — sinon un script de `pre-release` écrase
son homologue `official` et la divergence revient, à 210 `MSG_RETRY`, sans le
moindre message.

### (4) « `memory64` inutile » — **CONFIRMÉ, mais la mémoire est plus serrée**

wasm32 suffit, mais pas à 2 Go : le MEMFS porte 60 Mo d'assets, et la dernière
phase tombe en `Aborted(OOM)`. Réglage qui passe : **3 072 Mo de mémoire
linéaire et `--arena-mb 16`** (16 workers × 16 Mo au lieu de 32).

### (5) « COOP/COEP obligatoires » — **CONFIRMÉ**

```
crossOriginIsolated : true      SharedArrayBuffer : true
hardwareConcurrency : 16        Chrome/145.0.0.0
```

Sans les deux en-têtes, pas de `SharedArrayBuffer`, donc un seul worker.

---

## 5. LA BARRIÈRE D'ÉCRITURE — CONSTRUITE, VÉRIFIÉE, ET FINALEMENT INUTILE

Elle existe et elle **fonctionne** : `-fsanitize-coverage=func,trace-stores` sur
ocgcore, Lua **et le solveur**, bitmap indexé par page absolue (donc ni test
d'intervalle ni branchement), copies en bloc interceptées par `-Wl,--wrap=`.

Le juge n'est pas un raisonnement : `R2V_ARENA_VERIFY=1` compare l'arène au
miroir à chaque restauration et compte les pages sales **non marquées**.

**En mono-thread : `pages sales NON marquees : 0`**, fidélité `IDENTIQUE`.

**En threadé : 2 pages par run, et le rejeu diverge.** Ce sont des tableaux de
pointeurs déplacés d'un cran — le vérificateur les affiche en hexadécimal, pas
sur parole. La cause est identifiée et le verrou est structurel :

- clang abaisse les copies de structure en instruction `memory.copy`, que ni
  `-fsanitize-coverage` ni `--wrap` ne voient ;
- `-mno-bulk-memory-opt` les ramène à des appels — mais la négation est
  hiérarchique dans LLVM et emporte `bulk-memory`, que `--shared-memory` exige ;
- **`wasm-opt --llvm-memory-copy-fill-lowering`** : *« memory.copy lowering
  should only be run on modules with no passive segments »* — or la mémoire
  partagée impose les segments passifs.

Trois contournements essayés et réfutés, chacun par son symptôme :

| tentative | verdict |
|---|---|
| `-Wl,--no-check-features` | ne corrige rien, masque le garde-fou ; le programme tombe ailleurs |
| rapiéçage de `target_features` (`wasm/patch_features.py`) | lie, puis « null function or function signature mismatch » |
| `-flto` | vérificateur : 2 pages manquées — LTO inline `memcpy` **avant** la résolution de symbole, `--wrap` ne voit plus rien |

**Conclusion : la barrière n'est pas nécessaire.** Le bras à dirty-set complet
est correct par construction et à parité de débit. La barrière reste dans
l'arbre, vérificateur compris, comme instrument — pas comme dépendance.

---

## 6. CE QUE LE NAVIGATEUR RETIRE — CONFIRMÉ, ET PIRE QUE PRÉVU

L'étude annonçait une résolution d'horloge de 5 µs. Le fait mesuré est plus
franc : **la table `--profile` est incohérente en wasm threadé**. Elle impute
701 s de CPU là où seize threads n'ont pu en fournir que 560 — chaque sonde
traverse un appel JS (`emscripten_get_now`) et l'attribution s'effondre.

**Le navigateur livre, il ne juge pas.** Les seuls juges valides y sont les
**compteurs exacts** et le **temps de mur**. C'est ce qui est utilisé au §3.

---

## 7. PIÈGES RENCONTRÉS, ET CE QU'ILS COÛTENT À QUI LES REFERA

1. **`size_t(4) << 30` vaut ZÉRO** en wasm32. Le bitmap devenait un tableau vide
   et chaque marquage écrivait hors bornes. Compter en **pages**, jamais en octets.
2. **`PROXY_TO_PTHREAD` fait tourner `main()` sur un pthread** : il hérite de
   `-sDEFAULT_PTHREAD_STACK_SIZE` (64 Ko par défaut) et **non** de `-sSTACK_SIZE`.
   Le débordement se présente comme un « memory access out of bounds ».
3. **Le répertoire de vcpkg contient un fichier nommé `version`** : un `-I`
   dessus le fait résoudre à la place de l'en-tête C++20 `<version>`.
4. **`sqlite3` ne peut pas ouvrir un chemin `D:/…`** sous NODERAWFS : la VFS unix
   préfixe le cwd. Chemins POSIX absolus (`/ProjectIgnis/…`) obligatoires.
5. **Le corpus de scripts et de bases doit être complet ET ordonné** (§4-3), sinon
   le rejeu diverge **en silence**.
6. **`em++`, pas `emcc`, à l'édition de liens** — sinon libc++ et l'ABI C++ manquent.

---

## 8. COMMENT LE RELANCER

```powershell
# build navigateur (16 workers, 3 Go de memoire lineaire)
.\wasm\build_wasm.ps1 -Target web -Threads 16 -MemoryMb 3072 -NoBarrier `
    -Out .\wasm\web\combosolver.js

# servir avec COOP/COEP, puis mesurer
python .\wasm\web\serve.py 8765
node .\wasm\web\mesure.mjs http://127.0.0.1:8765/index.html 240
```

```powershell
# build node (systeme de fichiers REEL : meme ligne de commande que le natif)
.\wasm\build_wasm.ps1 -Threads 16 -MemoryMb 2032 -NoBarrier
# chemins POSIX absolus, cwd sur le bon lecteur
node .\wasm\bin\combosolver_nobarrier_t16.js "/ProjectIgnis/replay/....yrpX" `
     --workdir /ProjectIgnis --scriptdir /ProjectIgnis/replay2video/deps/.../script

# barriere : mono-thread, et on EXIGE sa completude
.\wasm\build_wasm.ps1 -Threads 1
$env:R2V_ARENA_VERIFY = "1"      # « pages sales NON marquees » doit rester a 0
```

---

## 9. CE QUI RESTE À FAIRE POUR UN VRAI DÉPLOIEMENT

- **Entrées utilisateur** : `<input type=file>` pour le `.yrpX` et le `.ydk` (la
  page les dépose déjà dans MEMFS ; rien ne part sur le réseau).
- **Sorties** : les replays de solution sont écrits dans MEMFS — les zipper et
  les offrir au téléchargement.
- **Scripts à la demande** plutôt qu'empaquetés : 10,6 Mo compressés au premier
  chargement contre ~64 scripts réellement lus par run. Un `fetch` par script
  manquant, mis en cache, ramènerait la charge utile à ~1,5 Mo.
- **Hébergeur à en-têtes** : Cloudflare Pages / Netlify / Vercel (`_headers`).
  GitHub Pages reste exclu.
- **Onglet en arrière-plan** : un run d'une heure dans un onglet reste un pari.
  `--rounds` + `--carry` avec archive en IndexedDB est la forme qui tient.
- **PGO** : le natif mesuré ici n'en a pas. Les deux bras sont donc comparés
  sans PGO ; le remettre déplacerait le natif d'environ 30 %.
