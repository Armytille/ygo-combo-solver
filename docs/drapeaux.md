# Les drapeaux — inventaire d'audit (session 18)

> **ÉTAT AU TERME DE LA SESSION 18 : 122 → 115.** Dix mécanismes réfutés ont été **retirés du
> code** (§9.26 (d)) et trois drapeaux ajoutés (`--max-rollouts`, `--max-nodes`,
> `--adapt-to-peak`). Les sections A et B ci-dessous décrivent l'état AVANT ; les lignes barrées
> d'un ✔ sont faites.
>
> Retirés : `--mcps` ✔ · `--nrpa-lr` ✔ · `--recipe-w` ✔ · `--goal-bias` ✔ ·
> `--canonical-digest` ✔ · `--no-phase-change` ✔ · `--novelty-rollout-cut` ✔ ·
> `--archive-spread` ✔ · `--phs-canonical` ✔ · `--subsets-ascending` ✔
>
> Restent à trancher : `--backward` (réfuté, mais 50 références — la décomposition à rebours est
> imbriquée dans le graphe de recettes), `--prior` / `--prior-weight` (neutres sur les deux
> étalons), et les quatre cadrans `--options-*` réfutés, qui règlent un mécanisme RETENU et ne
> sont donc pas des mécanismes à supprimer.

Inventaire exhaustif : les drapeaux parsés (chaîne `else if` + tables `kBoolFlags` / `kU64Flags`).

Trois colonnes, comme le demande l'audit : *(a)* a-t-il été **jugé** ? *(b)* quel est son **verdict
écrit**, et où ? *(c)* le code serait-il plus simple **sans lui** ?

**Deux avertissements qui conditionnent la lecture de tout ce tableau :**

1. **§9.25 (b) retire les verdicts de l'étalon B à un run par bras.** Le juge `≥1` y rend
   `0, 0, 0, 89, 2 239` sur cinq exécutions de la MÊME commande à la MÊME graine. Tout ce que
   §9.24 (o) déclarait réfuté ou confirmé sur B redevient **NON JUGÉ**, dans les deux sens.
2. **8 drapeaux sont parsés et absents de `--help`** : `--assign-bias`, `--canonical-digest`,
   `--card-on-select`, `--dive-full`, `--elide-forced`, `--max-decisions`, `--merged-pop`,
   `--subsets-ascending`.
3. **17 drapeaux n'ont aucune mention dans les 5 469 lignes du dossier** : `--arena-mb`,
   `--ctx-max`, `--fire-ms`, `--growth-max`, `--growth-ms`, `--hindsight-k`, `--keep-gc`,
   `--merged-pop`, `--no-arena`, `--no-qhat-probe`, `--no-seed-quant`, `--options-window`,
   `--qhat-nodes`, `--qhat-rho`, `--subsets-ascending`, `--target-subset`, `--verbose`.

---

## A. À SUPPRIMER — réfutés ou départagés négativement (13)

Le dossier garde la trace ; le code n'a pas à la porter. Chaque suppression est un changement comme
un autre : **santé avant et après**, une à la fois.

| drapeau | verdict écrit | où |
|---|---|---|
| `--mcps` | RÉFUTÉ deux fois ; effondrement total à k=6 | §9.19 (k), §9.21 |
| `--nrpa-lr` | RÉFUTÉ | §9.19 |
| `--recipe-w` | RÉFUTÉ ; cause STRUCTURELLE : la distance monte quand on consomme, donc le mécanisme punit les invocations | §9.24 (d) |
| `--goal-bias` | RÉFUTÉ ; Liger reste à zéro ET les autres Fusions tombent (42 525 → 12 451) | §9.23 (h) |
| `--backward` | RÉFUTÉ ; mécanisme correct, MATIÈRE absente (2 sous-produits, 0,02 fabriqué) | §9.24 (e) |
| `--canonical-digest` | RÉFUTÉ sur A (poses ÷2 et ÷12) ; cause nommée : 3 monstres Lien au deck | §9.24 (j) |
| `--no-phase-change` | DÉPARTAGÉ NÉGATIF : allonge les tirages, ne change ni le juge ni les coupures | §9.24 (o) |
| `--options-ctx` | RÉFUTÉ | §9.20 |
| `--options-len` | RÉFUTÉ | §9.23 (g) |
| `--options-pool` | RÉFUTÉ | §9.23 (g) |
| `--options-window` | RÉFUTÉ (fenêtre positionnelle) — et **zéro mention** sous ce nom | §9.19 (g) |
| `--novelty-rollout-cut` | RÉFUTÉ | §9.22 |
| `--archive-spread` | DÉPARTAGÉ : critère INTERNE ×10, juges de recherche **neutres à négatifs**, 0 ligne | §9.23 (e) |

**Cas particuliers, à supprimer aussi mais pour une autre raison :**

| drapeau | raison |
|---|---|
| `--subsets-ascending` | son unique rôle était de rejouer l'A/B d'attribution du correctif C9. **Cet A/B est terminé.** Zéro mention au dossier. |
| `--phs-canonical` | départagé et jamais retenu ; §9.19. |
| `--prior` + `--prior-weight` | mesure **NEUTRE sur les deux étalons** (§9.12). Un mécanisme neutre sur tous les cas connus est une branche morte. |

---

## B. À NE PLUS DÉCLARER RÉFUTÉS — le verdict tombe avec §9.25 (b) (3)

| drapeau | ce que le dossier disait | ce qu'il faut écrire |
|---|---|---|
| `--assign-bias` | « RÉFUTÉ sur le second deck : 2 239 → 0 » | **NON JUGÉ.** Le témoin de ce bras rend 0 trois fois sur cinq. Gain ×27 sur A (chaîne causale vérifiée) toujours debout. |
| `--card-on-select` | « fait tomber l'étalon B à ZÉRO » | **NON JUGÉ**, et §9.25 (a) montre que le drapeau n'est pas la cause : sous `--hint-bias 0`, où il est sémantiquement inerte, les bras divergent quand même. |
| `--assign` | « dégrade » (sur A : 4 242 poses contre 74 788) | **RÉFUTÉ SUR A** — ce verdict-là tient, il est mesuré sur l'étalon A et non sur B. |

---

## C. JUGÉS ET RETENUS (14)

| drapeau | verdict | où |
|---|---|---|
| `--novelty` / `--no-novelty` | patience DÉRIVÉE d'une mesure (plus longue série muette 17-18) ; contrôle A/B automatique | §9.1, §9.2 |
| `--nrpa-keep` | la mémoire de politique bat la vitesse brute — 530 s → 90 s | §9.9 |
| `--nrpa-temp` | jugé | §9.13 |
| `--ctx-shrink` | politique à DEUX NIVEAUX, jugée | §9.13 |
| `--optimize` | coût lexicographique anytime, jugé | §9.11 |
| `--finisher` (levin) | archive Go-Explore + LTS, jugé | §9.10 |
| `--reroot`, `--reroot-h` | sqrt-LTS, jugés | §9.13, §9.15 |
| `--options`, `--options-online`, `--options-per-worker` | catalogue de macros + minage EN LIGNE ; bootstrap en une traite | §9.20, §9.21 |
| `--qhat` | bandit à statistique de permutation ; la SONDE répond seule à la question de l'opérateur | §9.22 |
| `--recipes` | graphe de recettes, jugé et instrumenté | §9.16, §9.23 |
| `--hindsight` | **le seul mécanisme de la s17 encore debout** : ×20,6 sur l'arité 3, séparation COMPLÈTE des supports sur deux graines (étalon A) | §9.24 (c), (f) |
| `--elide-forced` | +61 % de débit, ×2,1 de boards en exhaustif, table qui fusionne enfin (×21,6-28,9) — **acquis SANS graine** | §9.24 (k) |
| `--adapt` | courbe d'accord 44 % → 66 % : le mécanisme MORD, mais conversions identiques. Opt-in. | §9.13 |
| `--width`, `--growth` | instruments qui ont produit des chiffres du dossier | §9.1, §9.24 (j) |

*Réserve sur les deux derniers du tableau des mécanismes* : `--hindsight` et `--elide-forced` ne
sont retenus que sur ce qui ne dépend pas du juge de l'étalon B. Leur mesure sur B (§9.24 (o))
tombe avec §9.25 (b) — dans le sens favorable comme dans l'autre.

---

## D. ÉNONCÉ DU CAS — entrées de ligne de commande, pas des mécanismes (27)

`--deck` `--hand` `--opp-hand` `--start` `--target` `--target-exact` `--target-subset`
`--board-add` `--board-remove` `--hint` `--material` `--summon` `--summon-min` `--guard`
`--guard-off` `--no-activate` `--no-chain` `--resolve` `--no-ref` `--no-plan` `--approach`
`--fire` `--fire-spare` `--fire-no-chain` `--fire-open` `--fire-bake` `--fire-ms`

**Trois d'entre eux ne sont pas neutres et doivent être lus comme tels :**

- **`--resolve` est un indice déguisé** — il verse au biais d'indices (`main.cpp:6763`), pose un
  gradient de `resolve_weight = 250` et une exigence au but. Trois mécanismes en un drapeau.
  47 des 73 scripts de `tools/` le portent, et **l'étalon B ne peut pas s'en passer** : c'est
  l'énoncé de son but. Conséquence : **l'étalon B n'a aucun run nu** (§9.25 (g)).
- **`--hint`** est de la connaissance métier injectée à la main ; §9.22 (c) a mesuré que les cartes
  indicées classent **plus mal** que Gold Leo non indicée. Béquille assumée. 20 scripts sur 73.
- **`--summon-min`** alimente `hint_cards` comme `--resolve`.

`--target-exact` et `--target-subset` : jamais jugés, `--target-subset` sans aucune mention.

---

## E. INFRASTRUCTURE — aucun verdict à rendre (17)

`--workdir` `--scriptdir` `--outdir` `--player` `--seed` `--threads` `--verbose` `--help`
`--solve` `--solve-ms` `--arena-mb` `--tt-mb` `--keep-gc` `--no-arena` `--max-decisions`
`--profile` `--no-qhat-probe`

**`--tt-mb` n'est PAS neutre pour les instruments** : à sa valeur par défaut (**64**,
`main.cpp:985`) et dès que `n > 1`, la table partagée remplace la table privée et
`stats.distinct_by_depth` **n'est plus jamais incrémenté** (`search.cpp:1853`, `1983`, `2175`).
Tout « états distincts par profondeur » lu sur un run multi-worker est un zéro structurel.

**`--solve-ms` est la cause du non-déterminisme** (§9.25 (b)) : le budget est du temps de mur, donc
deux exécutions identiques ne font pas le même nombre de tirages, même à `--threads 1`
(41 232 contre 42 179 mesurés). Il manque un budget en **tirages** ou en **nœuds** — `max_nodes`
existe dans `SearchConfig` et **n'est exposé par aucun drapeau**.

---

## F. INSTRUMENTS — mesurent, n'agissent pas (8)

`--watch` `--probe-repeat` `--growth` `--growth-max` `--growth-ms` `--width` `--no-nrpa`
`--no-seed-recipes`

`--no-nrpa` et `--no-seed-recipes` sont des A/B d'attribution, pas des mécanismes. **`--no-nrpa` ne
donne PAS le budget au finisseur** : il remplace `RunNrpa` par `RunRollouts` glouton sur tous les
workers (`main.cpp:7074`, `7159-7162`).

---

## G. RÉGLAGES d'un mécanisme retenu (13)

`--nrpa-alpha` `--nrpa-bias` `--nrpa-iters` `--nrpa-level` `--archive-k` `--finisher-min`
`--levin-h` `--adapt-passes` `--prior-weight` `--hint-bias` `--resolve-weight` `--burn-slack`
`--burn-limit`

- `--nrpa-level` et `--finisher-min` ont été rendus explicites après le défaut C15 (variable cachée
  dans plusieurs A/B publiés) — c'est un bon exemple du dossier.
- `--burn-slack` : la borne **ne coupe jamais** en phase tirages (§9.12) ; la constante est sans
  effet mesuré.
- `--hint-bias` : §9.25 (a) montre qu'il porte un effet de **premier ordre** sur l'étalon B et
  qu'il n'a jamais été balayé.

---

## H. JAMAIS JUGÉS — à trancher (27)

| drapeau | remarque |
|---|---|
| `--finisher-options` | **écrit, jamais jugé** — §9.24 le dit lui-même |
| `--canonical-zones` | **jamais départagé**, signalé depuis la s15 |
| `--hindsight-k` | réglage de `--hindsight`, **zéro mention** |
| `--qhat-window`, `--qhat-rho`, `--qhat-nodes` | jamais balayés |
| `--ctx-max` | plafond ; zéro mention |
| `--merged-pop`, `--no-merged-pop` | réglage du finisseur ; `--merged-pop` sans mention |
| `--dive-full`, `--no-dive-full` | réglage du finisseur |
| `--lifo-ties` | départage à coût de Levin égal |
| `--finisher-post-goal` | sous `--optimize` |
| `--no-burn-share` | jamais jugé |
| `--options-support` | jamais jugé |
| `--derive-summon-min` | jamais jugé |
| `--no-seed-quant` | zéro mention |
| `--max-subsets` | **DÉPARTAGÉ en creux** : 24 et 256 donnent les mêmes trous (§9.24 (g)) ; la valeur 64 reste un pari |

---

## Bilan

| catégorie | n |
|---|---|
| à SUPPRIMER (réfutés / départagés négatifs / A/B terminé) | **17** |
| à re-déclarer NON JUGÉS (verdict B retiré) | 2 |
| jugés et retenus | 14 |
| énoncé du cas | 27 |
| infrastructure | 17 |
| instruments | 8 |
| réglages d'un mécanisme retenu | 13 |
| jamais jugés | 27 |
| **total** | **122** |

**Aucune suppression n'a été faite dans la session 18.** La raison est écrite en §9.25 (i) :
supprimer se juge sur la santé, et la santé de l'étalon A est fiable, mais l'ordre de priorité
place la réparation du juge avant le nettoyage. C'est le premier travail mécanique de la s19 —
un drapeau à la fois, santé avant et après.
