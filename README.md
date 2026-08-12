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
```

`--help` liste le reste (`--player`, `--threads`, `--solve-ms`, `--arena-mb`,
`--growth`, `--verbose`).

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
