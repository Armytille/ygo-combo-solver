# Rapport — optimisation du portage web

*Suite de `docs/rapport-poc-navigateur.md`. Le PoC tournait ; ici on l'optimise,
levier par levier, chacun mesuré. **Quatre passent, trois sont réfutés**, et le
plafond n'est pas là où on le cherchait.*

---

## 0. LE RÉSULTAT EN UNE PHRASE

Le navigateur rend **88 % du débit natif** (7 runs par bras, plages désormais
disjointes) et le code wasm est à **×1,31** du natif sur le juge déterministe —
contre ×2,01 au premier jet. La charge utile servie tombe de **12,18 à 5,56 Mo**
et le chargement à **0,29 s**. Mais le plafond n'est pas le codegen : gagner
7,5 % sur le juge déterministe n'a rien déplacé de mesurable sur le débit, parce
qu'à seize fils sur huit cœurs le run est en partie **lié à la mémoire**.

---

## 1. LES DEUX JUGES, ET POURQUOI IL EN FALLAIT DEUX

Le juge à temps fixe (états produits en 35 s) disperse à ±25 % sur ce bras. Il
ne tranche rien en dessous de 20 %, et il m'a fait croire successivement à un
« écart navigateur/node de 25 % » puis à un « gain de 16 % » qui n'étaient, tous
les deux, que du bruit.

**Le binaire imprimait déjà le bon juge** : le déroulement de la ligne de
référence — 290 décisions, mono-thread, travail identique à l'unité près.

| bras | 3 mesures | ms/décision | dispersion |
|---|---|---|---|
| natif | 53,3 / 50,7 / 50,5 ms | **0,178** | ±3 % |
| wasm node | 68,8 / 69,2 / 68,3 ms | **0,237** | ±0,7 % |
| wasm navigateur | 67,8 / 67,5 / 67,2 ms | **0,234** | ±0,4 % |

Deux conclusions immédiates :

1. **Le navigateur n'a AUCUN retard sur node** (0,234 contre 0,237). L'« écart
   de 25 % » du rapport précédent était une illusion du juge bruité.
2. Le codegen wasm est à **×1,31** du natif — pas ×2,01. Le ×2,01 mesurait le
   binaire **instrumenté par la barrière** ; l'instrumentation coûte à elle
   seule ×1,45 sur le cœur (89,51 contre 61,88 µs/appel).

---

## 2. LES LEVIERS QUI PASSENT

### (a) LTO — −9,5 % sur le cœur

| | µs/appel `Process (core)` |
|---|---|
| sans LTO | 61,88 |
| **avec LTO** | **56,03** |

Il n'était pas disponible tant que la barrière était là : `--wrap` et LTO ne
composent pas (LTO inline `memcpy` avant la résolution de symbole). Établir que
la barrière était inutile a débloqué le LTO — c'est le seul enchaînement de
cette session où réfuter un mécanisme en a rendu un autre possible.

### (b) stdout tamponné — le défaut natif était un anti-patron en wasm

`main.cpp` faisait, depuis toujours :

```c
std::setvbuf(stdout, nullptr, _IONBF, 0);   // pour qu'un plantage ne perde rien
```

En wasm, **chaque écriture non tamponnée est proxiée vers le thread principal du
navigateur** — emscripten y route stdout depuis les pthreads. Le solveur imprime
des milliers de lignes pendant la recherche : le thread principal était réveillé
en permanence et volait du temps aux seize workers. Inversé sous
`__EMSCRIPTEN__` (tampon de 1 Mo, `exit` vide), le natif inchangé.

### (c) Le thread principal ne fait plus rien pendant le calcul

La page accumulait la sortie et re-rendait un `<pre>` grandissant toutes les
400 ms. Elle accumule toujours, mais **ne touche au DOM qu'à la fin**.

**(b) + (c) mesurés ensemble sur le juge déterministe, dans le navigateur :
0,253 → 0,234 ms/décision, soit −7,5 %.**

### (d) Mémoire croissante — la précaution qui coûtait

`-sALLOW_MEMORY_GROWTH=0` était justifié par « la base de l'arène ne doit jamais
bouger ». C'était une précaution de trop : `memory.grow` **étend** la mémoire
linéaire par la fin, les adresses déjà servies ne bougent pas, l'invariant est
intact. Le figeage, lui, coûtait : à 2 Go un run sur deux tombait en
`Aborted(OOM)` quand l'archive grossissait, et à 3 Go le navigateur réservait
tout d'entrée.

Réglage retenu : **2 Go initiaux, croissance autorisée jusqu'à 4 Go.** Assez
grand pour que la croissance ne se produise pas en pratique, et le filet est là
si elle se produit.

### (e) Brotli précompressé — la charge utile divisée par deux

| | brut | gzip -6 | **brotli -q 11** |
|---|---|---|---|
| `combosolver.wasm` | 3,97 Mo | 1,37 Mo | **0,98 Mo** |
| `combosolver.data` | 60,44 Mo | 10,56 Mo | **4,45 Mo** |
| `combosolver.js` | 1,46 Mo | 0,25 Mo | **0,14 Mo** |
| **total servi** | | 12,18 Mo | **5,56 Mo** |

Les 22 302 scripts Lua se ressemblent énormément ; la fenêtre de brotli
l'exploite là où gzip ne le peut pas. Précompressé au build, servi tel quel avec
`Content-Encoding: br` — jamais à la volée.

---

## 3. LES LEVIERS RÉFUTÉS

### PGO — bloqué par une incohérence **interne à l'emsdk**

Le natif y gagne 27,2 → 18,6 µs/appel. En wasm, le pipeline fonctionne jusqu'au
bout — l'instrumenté tourne, écrit 1,7 Mo de `.profraw` — puis :

```
raw profile version mismatch: Profile uses raw profile format version = 10;
expected version = 11
```

Le compiler-rt livré par l'emsdk déclare `INSTR_PROF_RAW_VERSION 10`
(`system/lib/compiler-rt/include/profile/InstrProfData.inc:723`) et son propre
`llvm-profdata` (LLVM 24) en attend 11. **Le runtime et l'outil du même SDK ne
se parlent pas.** Deux pièges secondaires ont été levés au passage et sont dans
le script :

- `-fprofile-generate` échoue à l'initialisation ; `-fprofile-instr-generate`
  passe ;
- avec des threads, les compteurs s'incrémentent par RMW atomique sur une
  section non alignée — wasm refuse. `-fprofile-update=single` corrige.

Le pipeline reste câblé (`-ProfileGen` / `-ProfileUse`) : il suffira d'un emsdk
dont les deux moitiés s'accordent.

### mimalloc — conflit frontal avec le routage vers l'arène

mimalloc apporte ses propres `operator new` / `operator delete`. Or ce sont
exactement ceux d'`arena.cpp` qui aiguillent vers l'arène — ils doivent gagner.
`--allow-multiple-definition` lie, mais l'appariement allocation/libération se
casse et le run plante. Réfuté par construction, pas par mesure de vitesse.

### Élision des spans libres — INERTE, retirée

Idée : ne pas restaurer les pages d'un span libre au point de reprise. Sûre
(l'allocateur recarve un span avant de le servir), et **sans effet** :

```
pages : 899.5/Restore  (avant)
pages : 899.5/Restore  (apres)
```

L'écart entre `zone servie` (5,31 Mo) et `blocs vivants` (2,92 Mo) est de la
fragmentation **intra**-span, pas des spans libres. Un mécanisme inerte qui
coûte une branche est pire que rien : supprimé.

### Moins de workers — non

| workers | états |
|---|---|
| 16 | 5 371 019 |
| 15 | 5 058 204 |
| 8 | 3 578 090 |

Laisser un cœur au thread principal ne paie pas — parce qu'il n'a plus rien à
faire (levier c).

---

## 4. LE RÉSULTAT, 7 RUNS PAR BRAS

> Étalon B contraint, graine 888, 16 workers, `--arena-mb 16`, phase de 35,0 s.

| bras | états (7 runs, triés) | médiane | vs natif |
|---|---|---|---|
| **natif** | 5 351 379 · 5 376 251 · 5 384 807 · **5 576 413** · 5 641 659 · 5 911 880 · 6 292 276 | 5 576 413 | 1,00 |
| **navigateur** | 4 541 741 · 4 571 710 · 4 926 522 · **4 927 795** · 4 964 716 · 5 173 406 · 5 307 815 | 4 927 795 | **0,88** |

Avec sept runs les plages se séparent (natif min 5 351 379 > navigateur max
5 307 815) : **l'écart de 12 % est réel**, là où trois runs le noyaient dans le
bruit.

Fidélité conservée sur tous les runs : `MSG_RETRY 0`, empreintes
`26513409f1c5dbbc` / `7a3e73765d0df4ba`, `=> IDENTIQUE`.

---

## 5. LE PLAFOND N'EST PAS LE CODEGEN

Le fait qui commande tout le reste :

- déficit de **codegen** : ×1,31 (soit −24 % de vitesse) ;
- déficit de **débit parallèle** : ×1,13 (soit −12 %).

Un déficit de calcul de 24 % ne coûte que 12 % de débit. Et symétriquement,
gagner 7,5 % de codegen (leviers b+c) n'a rien déplacé de mesurable sur le
débit. **À seize fils sur huit cœurs, avec ~160 Mo de jeu de travail (16 arènes
+ 16 miroirs) contre 96 Mo de L3, le run est en partie lié à la mémoire, pas au
calcul.**

Conséquence pratique : **continuer à optimiser le code wasm ne rendra pas les
12 % restants.** Ce qui les rendrait, c'est réduire le jeu de travail par
worker — c'est-à-dire ce que faisait la barrière d'écriture (54 pages par
restauration contre 899). Elle reste bloquée par le couple
`-mno-bulk-memory-opt` / `--shared-memory`, et c'est aujourd'hui la seule voie
identifiée vers la parité.

---

## 6. CONFIGURATION DE RÉFÉRENCE

```powershell
# navigateur
.\wasm\build_wasm.ps1 -Target web -Threads 16 -MemoryMb 2048 -NoBarrier -Lto `
    -Out .\wasm\web\combosolver.js
python .\wasm\web\serve.py 8765          # COOP/COEP + brotli precompresse
node   .\wasm\web\mesure.mjs http://127.0.0.1:8765/index.html 240 16

# node (systeme de fichiers reel, meme ligne de commande que le natif)
.\wasm\build_wasm.ps1 -Threads 16 -MemoryMb 2048 -NoBarrier -Lto
```

Drapeaux du build : `-O3 -flto -fwasm-exceptions -msimd128 -pthread`,
`-sPROXY_TO_PTHREAD -sDEFAULT_PTHREAD_STACK_SIZE=8MB -sSTACK_SIZE=8MB`,
`-sINITIAL_MEMORY=2Go -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=4Go`.

Un garde-fou a été ajouté au script : **l'empreinte des drapeaux**. Le cache
incrémental ne comparait que les dates ; changer un drapeau ne recompilait rien
et la mesure suivante portait sur l'ancien binaire. Payé une fois, plus jamais.

---

## 7. CE QUI RESTE

- **Scripts à la demande** plutôt qu'empaquetés : 4,45 Mo de `.data` pour ~64
  scripts réellement lus par run. Un `fetch` par script manquant, mis en cache,
  ramènerait la charge utile totale sous 1,5 Mo.
- **PGO**, dès qu'un emsdk cohérent existe : le pipeline est déjà câblé.
- **La barrière**, seule voie identifiée vers la parité de débit — elle attend
  soit un moyen d'intercepter `memory.copy` en mémoire partagée, soit que le
  couple `-mno-bulk-memory-opt`/`--shared-memory` cesse de s'exclure.
- **PGO natif** : le bras natif mesuré ici n'en a pas. Le remettre déplacerait
  la référence d'environ 30 % et rouvrirait l'écart.
