Tu reprends `combosolver`, un solveur de combo EDOPro écrit en C++ qui tourne
sur sa propre copie d'`ocgcore`. Dépôt git autonome, racine
`d:\ProjectIgnis\replay2video\combosolver`. Lis d'abord `README.md` puis
`docs/combo-solver-design.md` — les §9.1-9.14 documentent huit sessions, chaque
choix adossé à une mesure, impasses comprises. Ne redécouvre rien de ce qui y
est chiffré.

**LA MISSION DE CETTE SESSION : le MODE BUT SEUL — atteindre un board donné
depuis une decklist, SANS ligne de référence — et, pour y arriver, une revue
d'ÉTAT DE L'ART approfondie.** C'est le premier chantier du projet où la
réponse n'est pas dans le dépôt : il faut aller la chercher dans la
littérature, puis la transposer et la mesurer. La revue n'est pas un préalable
décoratif, c'est la moitié du travail — et elle se fait sur arXiv d'abord
(serveur MCP `arxiv` disponible : `search_papers`, `get_abstract`,
`download_paper`, `read_paper`), en lisant les papiers retenus et pas seulement
leurs résumés.

## Pourquoi ce chantier, et ce que « but seul » veut dire exactement

Aujourd'hui l'outil exige un replay de référence. Il en tire QUATRE choses
distinctes, et il faut les traiter séparément parce que trois relèvent du génie
logiciel et une seule de la recherche :

1. **Le board cible** — la définition de « réussi ». *Génie logiciel* : la
   capture existe déjà, et l'édition depuis une capture VIDE (`--board-add` sur
   un replay qui ne finit pas son tour) fait déjà le travail. Reste à en faire
   un vrai `--target "carte[@DEF]"`.
2. **Le gabarit de duel** — graine, format, LP, taille de main, pioche, deck
   adverse. *Génie logiciel* : `BuildSyntheticStart` copie l'en-tête d'un
   replay ; lui fournir un en-tête par défaut et un adversaire neutre est
   borné.
3. **Le contrôle de correction** — l'invariant « à zéro écart la recherche
   retrouve la référence », sur lequel repose toute la porte de santé. *Semi-
   ouvert* : sans référence il faut un substitut (voir plus bas, c'est un vrai
   sujet).
4. **Le RÉPERTOIRE** — les identités sémantiques des coups joués par la
   référence, relevées par `LiftPlan`, servies en prior à la politique NRPA.
   **C'est LE problème de recherche.** Sans lui la politique démarre uniforme,
   et on retombe sur le mur mesuré au §9.14.

Le mur, chiffré : la politique classe déjà le bon coup **premier dans 96 % des
cas** (au plafond calculé de 96,1 / 97,3 %) mais ne lui donne que **44 à 74 %**
de masse de probabilité. Sur une ligne de 160 décisions, 0,74^160 ≈ 10⁻²¹. Le
prior de répertoire est le seul mécanisme du projet qui donne de la masse à une
longue séquence — avec `--approach`, qui la rejoue en dur. **Enlever la
référence, c'est enlever la masse.** La question de recherche est donc :

> Comment donner de la masse à une séquence de 300-500 décisions vers un but
> CONJONCTIF, sans aucune démonstration à imiter, dans un simulateur dont on
> n'a pas de modèle déclaratif ?

Et une difficulté propre au domaine, mesurée sur le cas Lunalight : **le
paysage est PLAT**. L'heuristique qui guide (cartes du board cible déjà posées)
vaut zéro sur ~90 % de la ligne — le premier monstre du board tombe à l'étape
76 sur 108. Envoyer une pièce au cimetière ne « vaut » rien tant que, 25 étapes
plus tard, une autre carte ne la bannit pas comme matériau. Pas de gradient,
récompense tout au bout.

## L'état acquis — ne pas re-dériver

- **Sur la politique (§9.14, session 7bis).** Classement au plafond, masse
  insuffisante ; le prior par POIDS (`--prior`), l'adaptation par GRADIENT
  (`--adapt`) et le CONTEXTE dans la clé (`--ctx-shrink`) agissent tous les
  trois sur le classement — donc neutres ou négatifs, et on sait pourquoi. La
  température (`--nrpa-temp`, seul levier de masse essayé) a ouvert une fenêtre
  jamais convertie mais ne se reproduit pas. Trois drapeaux opt-in, éteints par
  défaut.
- **Sur la décomposition (session 7ter).** `--reroot` implémente √LTS
  (arXiv:2412.05196) dans le finisseur : coût enraciné λ/π(n ; ancêtre-indice
  le plus proche), les indices étant les changements du nombre de cartes cibles
  posées. Prévision calculée AVANT écriture (`ForecastSearchCost`, imprimée par
  `--adapt`) : **borne LTS monolithique 10²⁶-10⁶⁰ expansions contre 10^5,8-10^12,5
  décomposée sur 18 segments**. A/B étalon 1 : **PERDANT** (4 conversions → 2).
  Lecture, et elle est capitale pour toi : l'étalon 1 fait partir le finisseur
  de reculs d'une solution CONNUE — suffixes courts, forte probabilité — alors
  que la prévision porte sur des lignes reconstruites **de zéro**. *Le régime de
  la prévision, c'est exactement le mode BUT SEUL.* √LTS n'a donc jamais été
  mesuré là où il est censé rendre. **C'est ta première expérience.**
- **Sur les instruments déjà construits, à réutiliser** : courbe d'accord
  (masse ET fraction de classement premier — ne jamais lire la première seule),
  plafond par point de décision (`CorpusCoherence`), prévision de coût
  (`ForecastSearchCost`), compteurs `burn_cuts`/`goal_hits`/`reroots`.
- **Sur l'environnement (session 7ter).** Core remonté à `5a985af`
  (2026-08-10), les 5 patchs d'arène s'appliquent, santé identique.
  `deps/compat_2026-08/utility.lua` définit `Effect.IsCardSetcode`, absente de
  l'export épinglé, sans quoi certaines cartes récentes sont INERTES.
  `--max-decisions` existe désormais (le plafond dérivé de la référence
  tronquait en silence).
- **Sessions 1-6** : `--optimize`, `--fire`, archive Go-Explore, LDS,
  transposition, élagage par nouveauté, borne B&B brûlées (inactive), LuaJIT
  fermé. Meilleure ligne du cas synchron : **19/55/259**, escalade close sur
  quatre graines.

## Les étalons de mesure — et l'idée méthodologique de cette session

**L'étalon A, et c'est le bon : un cas dont on CONNAÎT la réponse, mais qu'on
cache au solveur.** Le deck `D:\ProjectIgnis\deck\Lunalight.ydk` avec une main
de 3× Fire Formation - Tenki atteint 3× Lunalight Liger Dancer — la ligne
complète en 108 étapes est dans l'historique de la session 7ter (combo
ygocombo #105, Nyarla remplacé par Dugares), et `_LastReplay.yrpX` en contient
une variante à un Liger, rejouée à 0 retry. **Le mode but seul doit retrouver
ça sans qu'on le lui montre.** C'est une vérité terrain, ce qui est rare et
précieux : la plupart des chantiers passés se mesuraient sur « mieux ou pas
mieux », celui-ci se mesure sur « trouve ou ne trouve pas ce qu'on sait
atteignable ».

Progression connue, à battre : 0 Liger → 2 912 tirages sur 5,4 M en posant un
(après correction de Scarlet Tiger et avec le répertoire d'une référence) ; le
deuxième Liger n'est jamais tombé. Sans référence, on repart d'en dessous.

**L'étalon B — le cas synchron, référence NEUTRALISÉE.** Même duel, même board
cible 19/56/273, mais plan vidé (ou drapeau `--no-plan` à ajouter). On sait le
board atteignable ; on mesure ce que coûte l'absence de répertoire. C'est
l'étalon *contrôlé* : un seul facteur change.

**L'étalon 0 — les sondes de politique**, qui répondent en millisecondes et
doivent servir de porte avant tout run (piège 40).

```powershell
# construire
& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe" `
    build\combosolver.sln /p:Configuration=Release /p:Platform=x64 /m /nologo /v:quiet

# sante — LA porte de tout changement de moteur, avant ET apres
.\bin\Release\combosolver.exe "D:\ProjectIgnis\replay\synchron handrip 2.yrpX" `
    --scriptdir ..\deps\scripts_2026-04-13\script --solve --solve-ms 60000 `
    --outdir sX_sante --no-chain Zalen --no-chain "Crystal Wing"
# 0 retry, 290 digests distincts / 0 fusion, reference retrouvee a 0 ecart,
# A/B nouveaute sans perte, 19/56/272. L'ecart IDLECMD #240 est PREEXISTANT.
# Un mecanisme inerte sans son drapeau doit rendre une sante identique AUX
# DUREES PRES — le diff complet le verifie.

# etalon A (Lunalight) : le montage complet est dans tools/lunalight3.ps1
# (temoin, scripts compat, cible editee, --max-decisions, --resolve des
# evenements rares du combo). En mode but seul, il faudra le meme SANS temoin.
```

## Le travail de recherche : où chercher, et ce qui est déjà écarté

Le problème posé — horizon long, but conjonctif, récompense terminale, pas de
démonstration, simulateur sans modèle déclaratif — a plusieurs littératures
qui le traitent de front. **Ne te limite pas à arXiv** : les actes d'ICAPS, de
NeurIPS/ICML, de SoCS (Symposium on Combinatorial Search) et d'AAAI portent
l'essentiel du domaine, et beaucoup de ces papiers sont aussi sur arXiv. Six
directions, par pertinence estimée :

1. **Recherche EN ARRIÈRE depuis le but (régression), et bidirectionnelle.**
   C'est ainsi qu'un humain résout ce combo : Liger exige Leo Dancer + 3
   Lunalight, donc Leo Dancer doit être au cimetière, donc Kaleido Chick doit
   l'y envoyer… Le paysage plat à l'aller devient un gradient au retour. La
   difficulté : notre simulateur n'a pas de modèle inverse. Chercher : *near-
   optimal bidirectional search* (NBS, DIBBS, Sturtevant & Felner), régression
   en planification classique, apprentissage d'un modèle inverse ou de
   « predecessors » depuis des rollouts. **C'est la direction que je placerais
   en tête** : elle attaque la cause (paysage plat) et non le symptôme.
2. **Heuristiques dérivées du BUT, sans démonstration.** Toute la planification
   classique construit son guidage à partir de l'objectif : relaxation sans
   effets négatifs (h_add, h_max, h_FF), *landmarks* (LM-cut), *operator
   counting* IP/LP. Nous n'avons pas de PDDL, mais les **matériaux de Fusion /
   Xyz / Link sont un modèle de recettes** extractible des textes de cartes ou
   de l'énumérateur : un graphe d'exigences donne des landmarks gratuits.
   Chercher aussi : apprentissage d'heuristiques admissibles
   (arXiv:2509.22626, arXiv:2606.04597), *operator counting* (arXiv:2404.07934,
   arXiv:1605.07989).
3. **Largeur et sérialisation — le cadre théorique de ce qu'on fait déjà à
   moitié.** Bonet & Geffner, *General Policies, Subgoal Structure, and
   Planning Width* (arXiv:2311.05490) : la largeur SÉRIALISÉE et les
   *sketches*, un langage compact pour décomposer un but conjonctif, où
   « sketch de largeur bornée ⟹ résoluble en temps polynomial ». Notre
   `novelty_serialize` en est un cas particulier non théorisé, et nos
   `--resolve` sont un sketch écrit à la main. Chercher : IW, BFWS, Serialized
   IW, apprentissage de sketches.
4. **Go-Explore et la famille « récompense rare, horizon long, pas de
   démonstration ».** L'archive existe déjà (`--archive-k`) mais elle est
   indexée par board complet, donc quasi-injective : elle stocke des
   quasi-doublons. Un descripteur GROSSIER (progression × ressources) en ferait
   une vraie archive de cellules. Chercher aussi les descendants de Go-Explore
   et la *quality-diversity* appliquée à la recherche combinatoire.
5. **Apprentissage par rejeu rétrospectif (hindsight).** HER et sa descendance :
   *tout tirage raté est une réussite pour le but qu'il a effectivement
   atteint*. C'est le seul mécanisme connu qui fabrique de la démonstration à
   partir de rien — donc exactement ce que le mode but seul réclame. À croiser
   avec les sous-buts appris depuis les arbres d'ÉCHEC (arXiv:2506.07255).
6. **√LTS dans son bon régime.** arXiv:2412.05196 et son successeur
   arXiv:2605.30664 (trois conceptions de *rerooter*). Déjà implémenté
   (`--reroot`), mesuré uniquement dans le mauvais régime. Le mode but seul EST
   le régime de la prévision : 10²⁶ → 10^5,8. À re-mesurer en priorité, avant
   d'écrire quoi que ce soit de neuf.

**Déjà écarté sur mesure, ne pas y revenir** : prior par poids et adaptation
par gradient (agissent sur le classement, déjà au plafond) ; contexte dans la
clé de politique (perte monotone) ; LuaJIT (fermé sur pièces, §9.12) ; racines
croisées `--fire` (clos sur structure, §9.13).

**Un point de méthode sur la revue.** Le moteur de recherche arXiv du serveur
MCP répond mal aux requêtes larges et bien aux requêtes précises : `au:"Nom"`,
`ti:"phrase exacte"`, `abs:"terme"`, avec `categories: ["cs.AI"]`. Passer par
les auteurs est souvent le plus rentable (Lelis, Orseau, Cazenave, Geffner,
Bonet, Sturtevant, Helmert, Katz). Et lire les papiers retenus : la session
7ter a trouvé son meilleur levier en lisant la dérivation de MCPS, pas son
résumé.

## Ce qu'il faudra construire côté outil (borné, à faire une fois la revue faite)

- `--target "carte[@DEF]"` répétable : board cible construit directement, sans
  passer par un replay ni par des `--board-remove` en cascade.
- Duel synthétique sans référence : en-tête par défaut + adversaire neutre
  (`BuildSyntheticStart` prend déjà tout sauf ça).
- `--no-plan` : neutraliser le répertoire même quand une référence existe.
  C'est l'étalon B, et c'est trois lignes.
- **Un substitut au contrôle de correction.** Sans référence, l'invariant « à
  0 écart on retrouve la ligne connue » disparaît. Proposition à instruire :
  garder un cas à vérité terrain (Lunalight) comme test de non-régression du
  mode, et vérifier toute ligne produite par le rejeu depuis zéro — ce dernier
  contrôle, lui, ne dépend d'aucune référence.

## Les pièges — ceux qui ont coûté cher

Reprends la liste complète du prompt précédent (`docs/next-session-prompt.md`,
pièges 1-46). Les plus mordants pour CE chantier :

- **(40)** Instrumenter AVANT de calibrer, et l'instrument doit appliquer la
  même mise à jour que le run.
- **(44)** Une moyenne géométrique de probabilités, lue seule, ment : elle
  affichait 44 % là où le classement était déjà à 96 %. Toute métrique de
  politique se lit à deux colonnes et se compare à un plafond calculé.
- **(45)** Classer un mécanisme en levier de CLASSEMENT ou de MASSE avant de
  l'écrire : ça prédit son signe.
- **(46)** Une perte monotone dans un cadran est un vrai résultat ; un point
  isolé ne prouve rien.
- **(47, session 7ter)** *Une carte qui lève une erreur Lua est une carte
  ABSENTE de l'espace de recherche.* Scarlet Tiger errait 462 fois par rejeu et
  rendait tout un combo introuvable. **Lire le compteur « erreurs du core »
  avant de conclure quoi que ce soit sur une recherche infructueuse.**
- **(48, session 7ter)** `--scriptdir` DÉSACTIVE le scan automatique de
  `repositories/` (assets.cpp, branche `else`). Les cartes qui n'existent que
  dans un dépôt delta disparaissent en silence.
- **(49, session 7ter)** Le plafond de décisions est dérivé de la référence
  (1,5× + 32) : viser une ligne plus longue que la référence tronque la
  recherche sans le dire. `--max-decisions`.
- **(50, session 7ter)** La contrainte voulue n'est pas toujours exprimable à
  la bonne finesse : `--no-chain` sur Silver Hound interdisait aussi son effet
  de RÉSURRECTION, indispensable au combo, parce que ses deux effets partent du
  cimetière. Quand l'interdit a priori est trop grossier, juger A POSTERIORI.
- **(51, session 7ter)** Une date de fichier n'est pas une version : le client
  EDOPro 41.0.2 est daté de 2025-05-05 tout en étant à jour. Vérifier la
  version, pas l'horodatage.

## Discipline de vérification — non négociable

Santé AVANT et APRÈS chaque changement de moteur, diff complet (seules les
durées peuvent bouger) ; tout replay écrit rejoué depuis zéro et jugé ; A/B sur
faits STRUCTURELS uniquement, comparés à la dispersion du témoin et non à un
run ; un A/B perdant se documente et se désactive par défaut ; runs séquentiels
(jamais deux mesures en parallèle) ; `--outdir` dédié par run (le défaut
`solutions/` écraserait le corpus).

## Définition de « terminé »

(a) Une revue d'état de l'art écrite dans `docs/`, avec pour chaque piste
retenue le papier, le mécanisme, la transposition envisagée et le coût estimé —
et pour chaque piste écartée, la raison. Les papiers décisifs LUS, pas
survolés. (b) √LTS re-mesuré dans le régime but seul, sur l'étalon B. (c) Le
mode but seul implémenté au moins dans sa forme minimale (`--target`,
`--no-plan`, duel synthétique sans référence) et mesuré sur les étalons A et B.
(d) Un mécanisme de la revue implémenté et tranché en A/B structurel. (e) §9.15
documenté, ce prompt régénéré.
