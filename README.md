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

# MODE BUT SEUL (session 8) : atteindre un board depuis une decklist, SANS
# ligne de reference. Le replay positionnel n'est plus qu'un GABARIT de duel
# (drapeaux, LP, taille de main, deck adverse) — il est imprime en tete du
# rapport pour que ce soit verifiable et non promis. `--target` POSE le board
# cible au lieu de l'editer depuis une capture ; `--no-ref` implique
# `--no-plan`, qui ecarte le REPERTOIRE (les identites semantiques des coups de
# la reference, servies en biais a la politique NRPA).
combosolver.exe gabarit.yrpX --scriptdir <scripts> `
    --deck "D:\ProjectIgnis\deck\Lunalight.ydk" `
    --hand "57103969|57103969|57103969" `
    --no-ref --target 54701958 --target 54701958 --target 54701958 `
    --target "90590304@DEF" --max-decisions 700
# `--no-plan` SEUL (reference intacte par ailleurs) est l'etalon de mesure : un
# seul facteur change, et il chiffre ce que le repertoire valait.
# Attention : sans plan, la passe « a ecarts bornes autour du plan » etait
# structurellement VIDE (26 etats a tous les niveaux, mesure session 8) — mais
# c'etait le defaut C1 (un seul jeton de partition pour seize workers), corrige
# session 9. La mesure est A REFAIRE ; lire la colonne "partition".

# MINAGE EN LIGNE DES OPTIONS (session 14) : le bootstrap EN UNE SEULE TRAITE.
# Jusqu'ici le catalogue de macros etait mine UNE fois, au demarrage, sur un
# corpus EXTERNE (`--adapt`) — un run parti de rien restait nu, et l'auto-amorce
# demandait plusieurs runs enchaines a la main (gen1 nue -> macros -> gen2
# armee). `--options-online <s>` fait rentrer la boucle DANS le run : les
# workers versent leurs meilleures lignes a un corpus vivant, le catalogue est
# re-mine toutes les s secondes (meme selection par perte de Levin) et echange a
# une frontiere sure. Aucun corpus, aucun `--approach`, aucune relance.
combosolver.exe gabarit.yrpX --scriptdir <scripts> --deck ... --hand ... `
    --no-ref --target ... --options-online 60 --options-ctx 1
# `--options-pool` / `--options-per-worker` reglent le corpus vivant : c'est la
# POMPE A DIVERSITE (les seize workers repartent tous de la meilleure sequence
# partagee — sans quota par worker, le corpus serait seize fois la meme ligne).

# BANDIT DE TETE A STATISTIQUE DE PERMUTATION (session 15) : le mecanisme de
# MCPS (arXiv:2510.06381), pour de vrai. Sur les `k` premieres decisions du
# tirage, le coup n'est plus echantillonne sous la politique : il est choisi par
# argmax de `val = (n·Q + n̂·Q̂) / (n + n̂)` — `Q`, moyenne des recompenses des
# tirages passes par ce noeud puis par ce coup ; `Q̂`, moyenne sur TOUS les
# tirages contenant ce coup ET tous ceux du chemin, dans n'importe quel ordre et
# n'importe ou. Poids proportionnels aux effectifs : aucun hyperparametre de
# biais (c'est le point du papier). Au-dela de k, NRPA echantillonne comme avant.
# Machinerie : un bitset par code de coup sur une fenetre glissante des W
# derniers tirages, `Q̂` par popcount sur l'intersection.
combosolver.exe gabarit.yrpX --scriptdir <scripts> --deck ... --hand ... `
    --no-ref --target ... --options-online 60 --qhat 6
# La SONDE (imprimee d'office) donne `n̂` et `Q̂` par coup a la premiere
# decision. C'est le seul instrument qui reponde a « le solveur trouve-t-il la
# bonne ouverture tout seul ? » — la courbe d'accord du corpus en est AVEUGLE,
# puisqu'elle mesure la reproduction d'un corpus qui ne contient que des bonnes
# lignes alors que `Q̂` tire son signal des ECHECS. Mesure session 15 : au bout
# de ~800 000 tirages, Lunalight Gold Leo (la bonne cible de Tenki, et une carte
# qui ne recoit AUCUN `--hint`) sort premiere a Q̂ = 0,066 contre 0,012 pour
# Lunalight Tiger et 0,002 pour Kaleido Chick — tous deux indices.
# `--qhat-window` (defaut 4096), `--qhat-rho` (32), `--qhat-nodes` (65536).

# SONDE DE REPETITION (session 16) : l'instrument qui separe deux pannes que le
# score de board CONFOND. Le mur du solveur est « atteindre un sous-but consomme
# ce dont le suivant a besoin » — mais encore faut-il savoir si le deuxieme
# exemplaire n'est JAMAIS TENTE (le materiau etait la : panne d'echantillonnage)
# ou TOUJOURS PERDU (la chaine etait consommee : panne de h). Deux correctifs
# opposes. `--probe-repeat` imprime, par carte surveillee (--summon-min /
# --resolve) et POUR CHAQUE PHASE (tirages, puis tirages enracines du
# finisseur), l'histogramme des invocations PAR TIRAGE en compte brut.
combosolver.exe ... --summon-min "54701958:3" --probe-repeat
# Mesure session 16, etalon Lunalight : l'echantillonnage fabrique TROIS
# Lunalight Masquerade dans 290 463 tirages et pas UN SEUL Liger Dancer sur
# 930 676 — alors que le but en demande trois. Le controle est dans la meme
# table : ce n'est pas « il ne sait pas repeter ».
# NB : l'axe « distance de recettes » de la sonde ne rend AUCUN verdict tant que
# les recettes lues sont amorcees par le texte (zone joker : le materiau qu'on
# vient de consommer compte encore depuis le cimetiere). Le run le dit.

# GRAPHE DE LANDMARKS APPRIS (session 16, arXiv:2508.21564) : apprend, depuis
# des plans RESOLUS, les faits (carte, zone, COMPTE) que tout plan atteint, dans
# quel ordre, et combien de fois — les BOUCLES DE REPETITION du papier. Sert
# ensuite de `h` : un h qui DECROIT pendant qu'on construit, la ou le h plat ne
# bouge pas tant qu'aucune carte cible n'est posee.
combosolver.exe ... --landmarks corpus/ --landmark-w 60
# Le graphe est IMPRIME avant de peser. Sur l'etalon handrip, appris depuis deux
# lignes resolues, il sort « Fake Trap @ADV banni x3 » a l'ordre 0,54 — le
# handrip lui-meme, en landmark COMPTE, sans qu'aucune carte soit nommee dans le
# code (les zones de l'ADVERSAIRE sont relevees : sans elles le graphe serait
# reste muet sur la moitie du but tout en ayant l'air de fonctionner).
# `--landmark-w` pese dans le score des TIRAGES, `--landmark-h` dans le h du
# FINISSEUR ; a 0, le graphe est appris et MESURE sans entrer dans aucun cout.
# STATUT : ECRIT, INSTRUMENTE, NON DEMONTRE. L'A/B de la session 16 rend un
# evenement rare (>=3 resolutions) non nul dans deux bras differents a deux
# budgets differents, sur une seule graine — cela ne demontre rien (§9.23 (d)).
# RESERVE : le mecanisme exige un plan resolu, donc il ne sert a rien A FROID.

# CORRECTIF DE CREDIT (session 18) : le score d'un tirage est un MAX sur ses
# prefixes, mais le gradient renforcait TOUS ses pas — y compris ceux d'apres le
# pic, c'est-a-dire ceux qui ont DEFAIT le board. `--adapt-to-peak` tronque le
# gradient au pic. Mesure etalon A nu, deux paires, un seul facteur :
# l'arite 3 (Sabre Dancer) fait x2,6 dans les deux (1 442 -> 3 740 et
# 659 -> 1 690), a -24 a -37 % de debit. La vie du mecanisme est imprimee
# (« gradient tronque au pic : N pas retires ») : a zero il est INERTE.
combosolver.exe ... --adapt-to-peak

# LES OPERATEURS DECLARES (session 19) : lire les cartes au lieu de les
# observer. `--operators` extrait des SCRIPTS LUA du deck la table des
# operateurs — preconditions (SetRange, SetCountLimit), produit
# (SetOperationInfo : categorie ET zone), ETAT ACCORDE (EFFECT_ADD_CODE,
# EFFECT_EXTRA_FUSION_MATERIAL...), recettes en CODES (Fusion.AddProcMix*),
# et jusqu'aux operateurs declares par une PROCEDURE (proc_*.lua) —, l'imprime,
# puis la CONFRONTE au plan rejoue. C'est un HARNAIS, pas un mecanisme : il ne
# change aucune recherche.
combosolver.exe plan_resolu.yrpX --scriptdir <scripts> --operators
# Il faut DEUX vocabulaires, et le dossier n'en lisait aucun :
#   CATEGORY_* : ce que l'effet fait aux CARTES  (envoyer, chercher)
#   EFFECT_*   : quel ETAT il accorde            (renommer, autoriser un
#                                                 materiau du cimetiere)
# Le combo repose entierement sur le second. Un balayage de constantes rend
#   Lunalight Kaleido Chick -> EFFECT_ADD_CODE
#   Lunalight Masquerade    -> EFFECT_EXTRA_FUSION_MATERIAL
# c'est-a-dire EXACTEMENT les deux goulots mesures (0,14 % et 0 %), sans qu'une
# seule carte soit nommee dans le code. Le recensement des CATEGORY_* ne les
# aurait pas trouves.
# VERDICT sur le plan resolu (283 decisions, 0 MSG_RETRY) : 37 activations,
# 0 non appariee, 18/18 preconditions de zone, 10/10 de ressource.
# La table est DECLARATIVE et OPTIMISTE : conditions et couts sont des
# fermetures, non evaluees. Elle dit ce qu'une carte declare pouvoir faire,
# jamais ce qu'elle peut faire A CET INSTANT.

# `--op-recipes` amorce le graphe de recettes depuis cette table, et y pose le
# TYPE DE NŒUD QUI MANQUAIT : « ce CODE peut etre ACQUIS ». Le graphe rangeait
# `Lunalight Leo Dancer` comme un PRODUIT A FABRIQUER, alors que son materiau
# nomme est absent du deck — d'ou l'echec de `--backward` (« mecanisme correct,
# MATIERE absente »). La decomposition a rebours est desormais IMPRIMEE, et
# elle contient enfin l'operateur de Kaleido Chick.
combosolver.exe ... --recipes 0 --backward --op-recipes

`--help` liste le reste (`--player`, `--threads`, `--solve-ms`, `--arena-mb`,
`--growth`, `--width`, `--novelty`, `--no-novelty`, `--no-nrpa`, `--seed`,
`--nrpa-keep`, `--tt-mb`, `--finisher`, `--archive-k`, `--max-rollouts`,
`--max-nodes`, `--adapt-to-peak`, `--elide-forced`, `--hindsight`,
`--approach`, `--prior`, `--prior-weight`, `--adapt`, `--adapt-passes`,
`--no-burn-share`, `--max-decisions`, `--reroot`, `--reroot-h`,
`--nrpa-level`, `--nrpa-alpha`, `--nrpa-iters`, `--max-subsets`,
`--recipes`, `--no-seed-recipes`, `--no-seed-quant`, `--derive-summon-min`,
`--options`, `--options-ctx`, `--options-online`, `--finisher-options`,
`--qhat`, `--canonical-zones`, `--assign`,
`--assign-bias`, `--backward`, `--watch`, `--operators`, `--op-recipes`,
`--probe-repeat`, `--landmarks`, `--landmark-w`,
`--landmark-h`, `--profile`, `--verbose`).
Dix mécanismes réfutés ont été **retirés du code** en session 18 — `--mcps`,
`--nrpa-lr`, `--recipe-w`, `--goal-bias`, `--canonical-digest`,
`--no-phase-change`, `--novelty-rollout-cut`, `--archive-spread`,
`--phs-canonical`, `--subsets-ascending`. `docs/drapeaux.md` tient l'inventaire
et le verdict de chacun.
`--profile` imprime le profil du chemin chaud par phase (sondes rdtsc, temps
exclusif, ligne « reste ») — c'est l'instrument qui a tranché que 82-86 % du
temps part dans le core (§9.18) ; son coût mesuré est sous le bruit (< 2 %).
La graine des tirages est dérivée du temps et imprimée — la redonner via
`--seed` rejoue les mêmes tirages. NB session 6 : à graine fixée, deux runs
divergent quand même (l'ordre des échanges entre workers dépend du timing) —
les compteurs de tirages ne sont pas des métriques d'A/B, seuls les faits
structurels le sont (coûts écrits, conversions, coupures).

# OPTIMISATION DE COUT : chercher une ligne MOINS CHERE que la reference
# (cout lexicographique : brulees, puis actions, puis decisions). La recherche
# ne s'arrete plus a la premiere solution (chaque solution resserre la borne),
# le score de but NRPA devient lexicographique, les lignes continuent APRES le
# but (une recuperation d'apres-but reduit les brulees sans toucher au board),
# et le finisseur s'enracine sur les prefixes des solutions les moins cheres.
# --burn-slack regle la marge de la borne brulees (defaut 6 ; la reference
# pique a 23 pour finir a 19 — marge de recuperation mesuree 4) ;
# --burn-limit ensemence la borne avec un cout deja connu. Mesure session 6 :
# sur le cas etalon la borne ne coupe JAMAIS en phase tirages (l'espace des
# lignes gagnantes vit sous le pic de la reference) — la regler n'apporte
# rien ; les compteurs burn_cuts/goal_hits des rapports en font foi.
# --prior <f|dossier> (repetable) : prior par rejeu de solutions — les
# plan_key du corpus deviennent des poids initiaux de politique NRPA
# (releves sur le duel de LEUR en-tete). Mesure NEUTRE sur les deux etalons
# (session 6) : disponible, hors commande recommandee.
# --adapt <f|dossier> (repetable) + --adapt-passes <n> : rejeu d'ADAPTATION
# du meme corpus — au lieu d'une prime par coup, le gradient NRPA sur les
# carrefours des solutions (choix legaux + choisi). Le releve imprime la
# COURBE D'ACCORD (probabilite moyenne du coup joue) : 44 % a politique
# vierge, 65 % des la premiere passe, palier a 66 % — le mecanisme mord,
# mais un poids par plan_key est aveugle a l'etat et ne peut pas monter plus
# haut. Mesure session 7 : ensembles de conversion --fire IDENTIQUES,
# meilleur cout inchange sur l'etalon meme-deck. Opt-in, hors commande
# recommandee.
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

## La règle de mesure (session 18)

**Aucun mécanisme n'est retenu sans avoir passé les DEUX étalons.** Un mécanisme
validé sur un seul étalon est une hypothèse, pas un résultat.

**Et un mécanisme doit prouver qu'il est ALLUMÉ avant qu'on mesure son effet.**
La session 18 a trouvé `--assign-bias` totalement inerte — l'instantané du
graphe de recettes n'était pris que sous `--assign` — alors que le run imprimait
« les choix engageant un MATERIAU du graphe de recettes sont favorises ». Tout
mécanisme doit imprimer sa **vie** (un compteur non nul quand il agit) ; sans
elle, un A/B mesure deux fois le témoin.

## Un drapeau n'est jamais un correctif

**Le solveur doit bien marcher SANS drapeau.** Un utilisateur ne peut pas savoir
qu'il faut passer `--elide-forced --hindsight 0.5 --adapt-to-peak` pour que le
solveur fonctionne — et si c'est le cas, le défaut est dans les défauts.

C'est la maladie historique de ce dépôt : chaque amélioration mesurée a été
garée derrière un interrupteur que personne n'allume. Le solveur nu ne bénéficie
d'aucune d'elles. **122 drapeaux et un solveur qui ne trouve rien, ce n'est pas
une coïncidence.**

La règle, en trois lignes :

1. **Un correctif de défaut n'est JAMAIS un drapeau.** Si aucun utilisateur ne
   voudrait le comportement d'avant, il n'y a rien à choisir. Exemples appliqués
   en session 18ter : l'identité de carte sur les prompts de sélection et
   l'identité des prompts oui/non sont devenues **inconditionnelles**, et leurs
   drapeaux ont disparu. Ce qui était discutable n'était pas l'identité — c'était
   le *biais d'indices* qui s'y appliquait, et c'est **lui** qui est gardé.
2. **Un mécanisme est un drapeau seulement le temps de le mesurer.** Une fois
   passé sur les deux étalons, il devient le **défaut**, et le drapeau devient
   négatif (`--no-…`) s'il faut encore pouvoir l'éteindre pour un A/B.
3. **Un drapeau qui n'a jamais été jugé est une dette, pas une option.** Il y en
   a 27 (`docs/drapeaux.md`) ; chacun doit être jugé ou retiré.

*Ce qui reste à promouvoir, et ce qui manque pour le faire* : `--elide-forced`,
`--hindsight 0.5` et `--adapt-to-peak` sont mesurés bons sur l'étalon A et
restent opt-in — il leur manque la mesure de l'étalon B **en proportion sur N
runs**, que la bimodalité du juge impose désormais. C'est le premier travail de
la session 19, et il est mécanique.

**Et aucune mesure de l'étalon B ne vaut à UN RUN PAR BRAS.** L'audit de la
session 18 (§9.25 (b)) a recensé cinq exécutions de la *même* commande à la
*même* graine : le juge « résolutions atteintes par tirage » y rend
**0, 0, 0, 89, 2 239**. Médiane zéro, maximum deux mille. Ce n'est pas une mesure
bruyante, c'est un **événement rare** — un A/B à un tirage par bras ne mesure que
le tirage.

La cause n'était pas le nombre de workers : deux runs `--threads 1` à la même
graine faisaient 41 232 et 42 179 tirages, parce que **le budget était du temps
de mur**. C'est **corrigé** (§9.26 (a)) :

```powershell
# Mode DETERMINISTE : deux executions rendent des relevés identiques.
combosolver.exe ... --threads 1 --max-rollouts 20000 --max-nodes 500000 `
    --solve-ms 900000    # le temps ne doit JAMAIS mordre
```

Contrôle mesuré : **2 lignes de diff sur 397**, et ce sont les deux noms
d'outdir. Avant : 38 sur 392. Ce mode n'est **pas** le mode de production — le
mono-worker coûte ÷5,1 à ÷5,9 — c'est un **instrument d'attribution**.

Conséquence pratique, à appliquer sans exception :

- sur l'étalon B, **N runs par bras**, et la lecture porte sur la **proportion**
  d'exécutions qui aboutissent, jamais sur la valeur d'un compteur ;
- les faits **sans graine** (couverture, boards distincts en exhaustif, facteurs
  de fusion de table, santé) restent les juges les plus sûrs du dossier ;
- la sortie **structurelle** (l'approche écrite `k/8` et son nombre de décisions)
  sépare les régimes sans passer par un compteur — la préférer.

`docs/drapeaux.md` tient l'inventaire des 122 drapeaux : jugé, réfuté, jamais
jugé, et la proposition de suppression.

`docs/combo-solver-design.md` détaille les arbitrages, les mesures et les
impasses — y compris celles qui ont été abandonnées, et pourquoi.
