# Les drapeaux — inventaire d'audit (session 18, tenu à jour en 19, 21, 22, 23, 24)

> **s24quater** : `--grid` — **RÉFUTÉ en deux variantes, gardé comme témoin
> de sa propre réfutation** (défaut off, périmètre à l'octet). Clé d'archive
> = cellule (rips, overlap), un élite par cellule, ré-entrée uniforme,
> racines A2 = toutes cellules rippées — dérivé de la forme close raffinée
> (`tools/s24_forme_close_raffinee.py`, 13/13). Mesures (4 graines × 180 s,
> témoin MIN au même binaire) : V1 (toutes cellules) → 3/4 graines à ZÉRO
> rip ; V2 (rippées seulement) → ≥3 libre = 0 en 4/4 et jointes dégradées
> (3r∧1/6 contre 3r∧5/6 au témoin). Cause mesurée trois fois (échelle, V1,
> V2) : la ré-entrée par rejeu COUPLE la politique NRPA partagée à la
> famille ré-entrée — l'hypothèse d'indépendance (R6) est violée par le
> canal d'apprentissage. Tout successeur doit DÉCOUPLER l'adaptation des
> lignes ré-entrées. La commande nue de référence reste le bras MIN
> (`--no-serial`, 180 s : 3r∧5/6 sur 3/4 graines, crête-rip 5/6).

> **s24** : trois ajouts, tous FAUX par défaut (témoin = l'historique à l'octet,
> vérifié sur bancs A et B).
>
> | drapeau | famille | statut |
> |---|---|---|
> | `--quota-h` | mécanisme | **chantier 4 — red-black** (Katz-Hoffmann-Domshlak) : les usages observés des hôtes à quota (MSG_CHAINING, mêmes compteurs que la clé de cellule) entrent dans les capacités du LP au RAFFINEMENT — une ligne agrégée par hôte sur ses transitions chaînantes (`invoquer`/`choisir` exclues par construction), gardes nommées « sans borne » (→ extraction) et « hors budget » (→ couverture). Ne mord qu'avec `--refine-after` + modèle armé (ReportMechanisms le dit). **Juge passé** : marche théorème-2 avec quotas — étalon B 2 lignes posées, 0 état infaisable NOUVEAU (22 hérités inchangés) ; étalon A 7 lignes, 0 nouveau, h_quota 12→0. Arme aussi la colonne h_quota du banc. |
> | `--archive-fin` | mécanisme | **Go-Explore complet (1/2)** : les archives des recherches du finisseur (A1 LTS d'approche — gabarit vérifié —, A2 tirages enracinés, phase 2) fusionnent dans l'archive globale, chemins RÉ-ENRACINÉS (préfixe + chemin, décisions cumulées, queue de profondeur du score ré-étalonnée exactement ; clé/nibble-frais datés de la racine du finisseur = sur-partitionnement, direction sûre). Vie : « ARCHIVE DU FINISSEUR : +N cellules ». Fumée : +235/+273/+89 sur 3 rounds. |
> | `--carry` | mécanisme | **Go-Explore complet (2/2)** : sous `--rounds`, l'archive globale et la politique fusionnée PERSISTENT entre rounds (`RoundCarry`), et les workers de tirages du round suivant sont SEMÉS (SeedArchive, plafond archive-k, jamais un finisseur enraciné). La boucle continue sans ligne jointe si l'archive porte. Vie : « ARCHIVE PORTÉE », « semis d'archive ». Fumée : base moyenne de ré-entrée 19,7 → 38,1/38,3 unités aux rounds 2-3, 0 échec de rejeu. |
>
> **s24, correctif `--rounds`** : `BalanceModel::Build` ne remettait pas
> l'instance à zéro — au round ≥ 2 le second Build EMPILAIT places/transitions
> /demandes sur le premier, le LP devenait infaisable, la sérialisation se
> désarmait et `reenter`/`refine-after`/`quota-h` passaient INERTES (présent
> depuis la s23 ; vu par la ligne « !! INERTE » de la fumée). Corrigé : reset
> complet en tête de Build ; la fumée s24 arme l'échelle aux trois rounds.

> **s23** : `--rounds <n>` — **mécanisme** (défaut 1 = historique à l'octet,
> zéro bannière). La boucle interne (directive opérateur ; forme
> Go-Explore/ExIt, cf. `etat-de-lart-boucle-interne.md`) : le budget
> `--solve-ms` se découpe en N rounds, la meilleure ligne JOINTE de chaque
> round est réinjectée automatiquement au suivant (remplace la précédente ;
> les `--approach` de la commande restent). Arrêt sur solution ou plus de
> ligne jointe. Fumée : R1 → 2r_4of6, R2 (réinjectée) → 2r_5of6 en une seule
> commande de 240 s. Vie : bannières « ===== ROUND k/N ===== ».

> **s23** : `--resolve-legacy` — **TÉMOIN d'A/B**. Rejoue le câblage s22quater
> des résolutions (crédit post-résolution seulement) à la place de la
> compilation dans le bilan (défaut s23 : chaque `--resolve`/`--summon-min`
> pose UNE demande de présence @DISPO — le but se COMPILE, il ne se récompense
> pas ; garde d'asymétrie si aucun producteur lisible). Vie : ligne
> « RESOLUTIONS -> BILAN » avec le câblage actif. À retirer dès que l'A/B en
> proportion a tranché.

> **ÉTAT AU TERME DE LA SESSION 22 : +2.**
>
> | drapeau | famille | statut |
> |---|---|---|
> | `--quota-legacy` | **TÉMOIN d'A/B** | rejoue la dérivation s21 des quotas/habilitants (classes d'effets) à la place des DUAUX du LP (défaut s22). À retirer dès que l'A/B en proportion a tranché. |
> | `--refine-after <n>` | mécanisme | l'échelle auto-raffinante (chantier 3 s22) : re-sérialisation depuis la meilleure cellule-frontière quand sp_max stagne depuis n tirages. **Écrit, vie complète, NON JUGÉ** — 0 = éteint (défaut). |
>
> `--reenter` (s21) reste le témoin de son propre A/B (défaut 0,5).
>
> **s22ter** : `--no-self-negate` — famille **DISCIPLINE** (comme
> `--no-activate`/`--no-chain`) : jamais proposer une négation du joueur sur
> son propre maillon de chaîne. Effets dérivés de la table déclarée
> (NEGATE/DISABLE), vie imprimée (813 coupes/60 s mesurées étalon A). Une
> contrainte choisie n'a pas d'A/B à passer — elle a un contrat à tenir.

> **ÉTAT AU TERME DE LA SESSION 19 : 114 → 117.** Trois ajouts, et leur statut
> est écrit d'avance :
>
> | drapeau | famille | statut |
> |---|---|---|
> | `--operators` | **INSTRUMENT** (§F) | n'entre dans aucun coût, ne change aucune recherche. Rien à départager : il MESURE. |
> | `--op-recipes` | mécanisme | **drapeau le temps de le mesurer** (règle 2). Jugé POSITIF sur l'étalon A par un juge STRUCTUREL (§9.28 (d)) ; l'étalon B reste dû. |
> | `--op-bias` | mécanisme | écrit, instrumenté (compteur de vie), **non jugé**. |
>
> Et `--card-on-select` / `--yn-identity` ont disparu en s18ter : l'identité est
> inconditionnelle. Le tableau §H ci-dessous ne les mentionne plus.
>
> **PROMUS EN DÉFAUT** (étalon B, dix runs par bras, `>=2` de 3/10 à 9/10) :
> `--adapt-to-peak` → `--no-adapt-to-peak` · `--hindsight 0.5` →
> `--no-hindsight`.
>
> **`--elide-forced` REDEVIENT NON JUGÉ**, et c'est un défaut de câblage :
> `cfg.elide_forced` n'était assigné que dans `RunGrowthMeasurement`. Il était
> **inerte dans toute recherche** (preuve déterministe : relevé identique à
> l'octet avec et sans). Les « +61 % de débit, ×2,1 de boards » de §9.24 (k)
> valent pour le chemin `--growth`, et pour lui seul. §C ci-dessous est corrigé
> en conséquence.
>
> **`--backward` SORT DE §A** : le chantier 1 de la s19 lui a donné la matière
> qui lui manquait (§9.28 (d)). La suppression proposée est **annulée**.

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
| ~~`--backward`~~ | ⚠️ **SUPPRESSION ANNULÉE (s19).** Le diagnostic « mécanisme correct, MATIÈRE absente » était juste, et le chantier 1 a fourni la matière : la décomposition contient désormais `KALEIDO CHICK @TERRAIN` | §9.24 (e), §9.28 (d) |
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
| `--elide-forced` | ⚠️ **RETIRÉ DE CETTE LISTE PAR LA s19.** Les +61 % de débit et le ×2,1 de boards ont été mesurés avec `--growth`, **le seul chemin qui câblait le drapeau** ; sur la recherche il était inerte. Câblage corrigé, mécanisme **à juger** | §9.24 (k), §9.28 (f) |
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
| `--op-bias` | **écrit, instrumenté, jamais jugé** (session 19, chantier 2). Il lit `snap_operators`, que seul l'instantané du graphe remplit et que seul `--op-recipes` alimente : les deux façons dont il serait INERTE sont testées et **dites avant le run**. Sa vie est imprimée (désignées / proposées / prises). |
| `--finisher-options` | **écrit, jamais jugé** — §9.24 le dit lui-même |
| `--canonical-zones` | **jamais départagé**, et il ne doit PAS l'être en l'état : les **Zones Pendule** sont des séquences particulières de `LOCATION_SZONE`, que la canonicalisation confond avec une pose de magie ordinaire — elle **supprime donc la possibilité de poser une échelle**. Sur l'étalon A, `Lunalight Wolf` n'invoque par Fusion que depuis la Zone Pendule (`e2:SetRange(LOCATION_PZONE)`) : le drapeau referme silencieusement deux des trois portes du combo. La règle correcte est « une zone par **classe d'équivalence que les règles respectent** » — Zone Pendule et zone pointée par un Lien sont leurs propres classes. |
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
