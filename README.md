# combosolver — optimiseur de combo EDOPro

Prend un replay `.yrpX`, rejoue fidèlement sa ligne, en extrait le **board de fin
de tour**, puis cherche d'autres façons de l'atteindre : soit dans le même duel
et à moindre coût, soit **depuis un autre deck**. La sortie est un répertoire de
replays rejouables dans EDOPro.

Le solveur lie sa propre copie d'`ocgcore` : aucune dépendance au rendu, aucune
à `gframe`.

## Ce que fait le binaire

```bash
# Rejeu instrumenté + tests d'arène + mesures (aucune recherche)
combosolver.exe duel.yrpX --scriptdir <scripts>

# Chercher une meilleure ligne vers le MÊME board, dans le même duel
combosolver.exe duel.yrpX --scriptdir <scripts> --solve --outdir solutions

# Refaire ce board depuis un AUTRE duel (autre deck, autre main, autre graine)
combosolver.exe ref.yrpX --scriptdir <scripts> --start autre.yrpX --outdir solutions

# Refaire ce board depuis une DECKLIST + une main de depart, sans replay de
# depart : le duel est construit (parametres et adversaire de la reference,
# main forcee par pseudo-shuffle et VERIFIEE sur un duel jetable).
combosolver.exe ref.yrpX --scriptdir <scripts> `
    --deck "D:\ProjectIgnis\deck\test 3.ydk" `
    --hand "Assault Zone|Ash Blossom|Ash Blossom|Ash Blossom"
# --hand est optionnel : par defaut, la main de la reference (si la decklist
# peut la fournir — sinon erreur explicite).

# Donner une vraie main a l'adversaire d'un hand test : sans cartes JOUABLES
# en face, le core n'ouvre aucune fenetre de reponse adverse — la garde serait
# satisfaite par vacuite et le handrip ne ripperait rien. Les replays produits
# ne se rejouent qu'avec le meme --opp-hand.
... --opp-hand "27204311|27204311|27204311"   # 3 Nibiru en main adverse

# Contraintes de ligne : jouer sous menace Nibiru. Des que la 5e invocation
# resout (Nibiru devient actif), a chaque fenetre de reponse adverse : soit
# Crystal Wing est en jeu, soit Zalen est en jeu AVEC Junk Signal encore en
# main pour chainer par-dessus. La garde s'eteint une fois la main adverse
# videe (handrip). Et on ne paie jamais les 2000 LP du terrain.
combosolver.exe duel.yrpX --scriptdir <scripts> --solve `
    --guard  "5:Crystal Wing|Zalen@terrain+Junk Signal@main" `
    --guard-off "mainadv<=2" `
    --no-activate "Duel Evolution - Assault Zone" `
    --resolve "PSY-Framelord Omega@terrain:2" `
    --resolve "Trishula, Dragon of the Ice Barrier@terrain"

# --resolve : la ligne doit resoudre ces effets (ici : le handrip de 3 cartes
# qui eteint la garde). Controle au but, pas en cours de ligne. @zone restreint
# la zone d'ACTIVATION : l'Omega qui rippe s'active du TERRAIN — sans @terrain,
# son effet de cimetiere compterait aussi (faux positif mesure).

# --summon "5:carte|carte" existe aussi (le n-ieme summon DOIT etre une de ces
# cartes) — a ne pas confondre avec la garde : "protege quand la fenetre
# s'ouvre" n'exige pas que le garde SOIT la 5e invocation, il peut deja etre
# en jeu (la reference joue Zalen en 4e).

# Mode JUGE : memes drapeaux sans --solve, sur n'importe quel replay, pour
# savoir s'il respecte la discipline demandee.
combosolver.exe solutions/solution_00.yrp --scriptdir <scripts> `
    --guard "5:Crystal Wing|Zalen@terrain+Junk Signal@main"

# TEST ADVERSE (--fire) : la garde ci-dessus est un proxy statique ("un contre
# est disponible") ; ce mode joue la menace POUR DE VRAI. Nibiru est ajoute a
# la main adverse et ACTIVE a chaque fenetre ou il est legal (un essai par
# fenetre) ; la recherche doit refermer le board depuis l'etat post-injection —
# board complet (Crystal Wing contre gratuitement) ou board sans la carte
# sacrifiee (--fire-spare : contrer par Zalen consomme Junk Signal). Les
# replays produits se rejugent avec --opp-hand "27204311".
combosolver.exe duel.yrpX --scriptdir <scripts> `
    --fire "27204311" --fire-spare "Junk Signal" --fire-ms 60000 `
    --no-activate "Duel Evolution - Assault Zone" `
    --resolve "PSY-Framelord Omega@terrain:2" `
    --resolve "Trishula, Dragon of the Ice Barrier@terrain"
```

`--help` liste le reste (`--player`, `--threads`, `--solve-ms`, `--arena-mb`,
`--growth`, `--width`, `--novelty`, `--no-novelty`, `--no-nrpa`, `--seed`,
`--nrpa-keep`, `--nrpa-lr`, `--tt-mb`, `--finisher`, `--archive-k`,
`--approach`, `--verbose`). La graine des tirages est dérivée du temps et
imprimée — la redonner via `--seed` rejoue les mêmes tirages.

# OPTIMISATION DE COUT : chercher une ligne MOINS CHERE que la reference
# (cout lexicographique : brulees, puis actions, puis decisions). La recherche
# ne s'arrete plus a la premiere solution (chaque solution resserre la borne),
# le score de but NRPA devient lexicographique, les lignes continuent APRES le
# but (une recuperation d'apres-but reduit les brulees sans toucher au board),
# et le finisseur s'enracine sur les prefixes des solutions les moins cheres.
# --burn-slack regle la marge de la borne brulees (defaut 6 ; la reference
# pique a 23 pour finir a 19 — marge de recuperation mesuree 4) ;
# --burn-limit ensemence la borne avec un cout deja connu.
combosolver.exe duel.yrpX --scriptdir <scripts> --solve --optimize
combosolver.exe ref.yrpX --scriptdir <scripts> --start ref.yrpX --optimize `
    --approach "solutions/solution_00_b19_a56.yrp" --finisher-min 420000

# Le finisseur : quand les tirages montent a 7-8/8 sans convertir, la
# transplantation fouille les K meilleurs etats DISTINCTS (archive Go-Explore)
# et leurs prefixes de recul en Levin Tree Search sur la politique NRPA du run.
# --approach ressert les best_approach_*.yrp des sessions passees comme racines
# supplementaires ; --finisher mono|ab rejoue l'ancien finisseur (A/B).

## Comment la recherche évite de tout explorer

Trois mécanismes, tous mesurés (docs/combo-solver-design.md §9) :

- **Élagage par nouveauté (Iterated Width).** Un état n'est retenu que s'il rend
  vrai un fait `(zone, carte, occurrence)` inédit ; une branche muette depuis
  `patience` décisions est coupée. La patience est calibrée par `--width` : le
  long de la ligne de référence, 82 % des états sont muets et la plus longue
  série muette fait 17 décisions. Le préfixe répertoire est exempt, et un
  contrôle A/B automatique sur le cas même-deck imprime états gagnés et
  solutions perdues — un élagage qui perd des solutions le dit lui-même.
- **Tirages NRPA (politique apprise).** Un poids par code de coup (`plan_key`),
  échantillonnage softmax, adaptation vers la meilleure séquence, le répertoire
  de la référence en biais — sans évaluation des fils, chaque décision coûte
  plusieurs fois moins cher qu'un tirage glouton. La politique est persistante
  entre redémarrages (`--nrpa-keep`) et la meilleure séquence est partagée
  entre les workers : c'est cette mémoire, pas la vitesse brute, qui a rendu la
  transplantation reproductible (§9.9). C'est la passe qui porte la
  transplantation.
- **Coupure de tour.** Le board cible est celui de la fin du tour 1 : tout état
  au-delà du changement de tour est du temps perdu.

Preuve sur pièce : le board de `synchron handrip 2` a été refait **depuis le
deck de `test 4`** par la passe NRPA — 208 décisions, 45 actions, 13 cartes
brûlées, moins cher que la référence sur son propre deck — et le `.yrp` produit
se rejoue depuis zéro sans un seul `MSG_RETRY`. Depuis que la politique NRPA
est persistante entre redémarrages et que la meilleure séquence est partagée
entre les workers, ce résultat tombe en 90 s de budget sur les deux graines
testées (il demandait 530 s et une graine chanceuse sur quatre).

## Le drapeau qu'on ne peut pas oublier

`--scriptdir` **est obligatoire en pratique.** Un `.yrpX` ne se rejoue
fidèlement qu'avec le core *et* les scripts Lua contemporains de son
enregistrement. Sans le bon jeu de scripts, le rejeu diverge **en silence** : le
core pose une question différente, la réponse enregistrée devient invalide, et
l'outil continue d'afficher des mesures d'apparence normale.

Le seul détecteur est le compteur `MSG_RETRY` du rapport. Sur le replay de
référence : 218 retries avec les scripts vivants, **0** avec l'export épinglé.

## Construire

Dépendances externes, toutes hors du dépôt :

| Chemin | Rôle |
|---|---|
| `../edopro/ocgcore` | dépôt d'où `ocgcore` est extrait au commit voulu |
| `../edopro/gframe/lzma` | sources LZMA (lecture des replays compressés) |
| `../deps/ocgcore` | copie extraite et patchée, produite par le script ci-dessous |
| `../deps/scripts_<date>` | export figé des scripts de cartes |
| `../vcpkg` | sqlite3 en `x64-windows-static` |

```powershell
.\tools\fetch_solver_deps.ps1          # extrait + patche ocgcore, lua et les scripts
..\premake5\premake5.exe vs2022 --vcpkg-root=..\..\vcpkg
MSBuild build\combosolver.sln /p:Configuration=Release /p:Platform=x64
```

Le script d'extraction n'écrit jamais dans l'installation EDOPro de la machine.

## Les patchs d'ocgcore

`fetch_solver_deps.ps1` applique cinq modifications, toutes nécessaires :

| Cible | Pourquoi |
|---|---|
| `lua/luaconf-customize.h` | graine de hachage déterministe, et point d'accroche de l'allocateur d'arène |
| `lua/src/lauxlib.c` | branche le heap Lua sur l'arène — sans quoi l'état du duel n'est pas capturable |
| `ocgapi.cpp/.h` | ajoute `OCG_DuelQueryProcessorState` : phase, pile de résolution, chaîne courante, compteurs « une fois par tour » |

L'état du processeur est indispensable au digest de transposition : sans lui,
deux instants distincts d'une même résolution de chaîne se confondent et la
branche du combo est élaguée dès le début.

## Comment ça tient debout

- **Arène à base fixe.** Tout le heap du duel (C++ *et* Lua) vit dans une plage
  réservée à adresse fixe. Un instantané se prend par pages sales et se restaure
  en 0,05 ms, contre 78 ms pour re-simuler depuis la racine. C'est ce rapport qui
  rend la recherche possible.
- **Recherche sur le graphe d'états, pas sur l'arbre d'actions.** Activer A puis
  B et B puis A convergent ; la table de transposition les fusionne. L'arbre brut
  vaut 10^97 le long de la seule ligne de référence.
- **Le sous-hachage du digest est le mode de défaillance à surveiller** : il fait
  disparaître des solutions sans rien signaler. Le rapport compte les fusions le
  long de la ligne de référence, qui sont deux à deux distinctes par
  construction.

`docs/combo-solver-design.md` détaille les arbitrages, les mesures et les
impasses — y compris celles qui ont été abandonnées, et pourquoi.
