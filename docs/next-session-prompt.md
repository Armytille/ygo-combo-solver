Tu reprends `combosolver`, un solveur de combo EDOPro écrit en C++ qui tourne
sur sa propre copie d'`ocgcore`. Dépôt git autonome, racine
`d:\ProjectIgnis\replay2video\combosolver`. Lis d'abord `README.md`, puis
`docs/combo-solver-design.md` §9.1-9.16 (dix sessions, chaque choix adossé à une
mesure, impasses comprises), puis **`docs/etat-de-lart-but-seul.md`** — la revue
d'état de l'art de la session 8, qui contient la carte des directions possibles
et la raison de chaque écart. Ne redécouvre rien de ce qui y est chiffré.

## L'ÉTAT : les instruments sont posés, la mission de fond ne l'est pas

La session 9 a exécuté `docs/plan-correctifs.md` sans entamer la mission. C'était
le prix annoncé : aucune mesure du mode but seul n'était interprétable avant.
**Ce qui est fait est fait — ne le refais pas** (détail en §9.16) :

- **C1** la partition entre workers (`ClaimTable`, clé entière, échec OUVERT,
  compteur `partition`) ; **C2** `edges_skipped` aux onze sites + « EPUISE SOUS
  BORNE » ; **C3** `h_root` à la racine de la recherche + colonne `h0=`.
- **C4/C5** six colonnes d'élagage sous chaque passe + `reroots`/`h0` dans les
  trois tables (le tiers manquant) ; **C8** dix-neuf aborts silencieux nommés ;
  **C9** `ForEachSubset` alterne depuis les deux bouts (c'était un trou de
  complétude : 24 singletons et jamais une paire) ; **C11** `AdaptCorpus` reçoit
  `hint_bias` et `temp`, désormais EXIGÉS ; **C12** `--no-ref` dérive son plafond
  de la decklist ; **C13** saturations vérifiées au démarrage ; **C14** `LiftPlan`
  imprime ses trous sur le chemin `--fire` ; **C15** `--nrpa-level` exposé ;
  **C16** `--max-subsets` exposé.

L'audit complet dont ce plan était l'extrait a été traité dans la même session
(§9.16 (i)) : arène empoisonnée (C7), `ResponseForbidden` tri-état (C10),
replay tronqué, `cards.cdb`, lecture courte de script, `--summon n:X` vacuux,
`ProcessorState` vide, saturation `lam`/`reroots`, repli de profondeur du
finisseur, plafond d'écriture de `WriteSolutions`, `--novelty 0` côté NRPA,
six compteurs aveugles de plus, `--hint-bias` (C18), partage du budget (C17),
et trois contradictions doc/code.

**Reste, explicitement** : la **section 6 de l'audit** — les optimisations du
chemin chaud (C19-C28) — sauf le gating de la nouveauté, déjà fait parce qu'il
corrigeait aussi `--novelty 0`. Plus les constantes de moindre valeur
(`max_solutions` variable selon le site, `k <= 12`, `plan_window`,
`repair_window`), non exposées et non mesurées.

## À FAIRE D'ABORD — trois mesures que les correctifs rendent enfin possibles

1. **Re-mesurer l'étalon A en mode but seul.** Le tableau du §9.15 (e) compare
   un bras armé du répertoire à un bras but seul, mais les deux diffèrent AUSSI
   d'un mécanisme cassé : la passe LDS, qui produisait le 2/4 du bras armé, était
   paralysée par C1 quand le plan est vide. `tools/lunalight_butseul.ps1`. La
   vérification de C1 est explicite : **le nombre d'états doit CROÎTRE avec le
   niveau d'écart** (avant : 26, constant à tous les niveaux). Lire la colonne
   `partition` : non nulle, le travail est partagé ; c'est ce que la session 8
   ne pouvait pas distinguer d'une suppression.
2. **Prolonger le cadran α vers le haut.** `tools/s9_ab_alpha_haut.ps1` est
   écrit et prêt (α = 12/16/20/28). Le cadran bas est fait et documenté ; il est
   MAL CENTRÉ, et le §9.16 (d) dit pourquoi — l'équivalence entre échelles est
   `α_s9 = α_s8·h0/8`, facteur 0,5 à 0,75, et non 1/8. L'optimum de la session 8
   (α_s8 = 25) tombe vers α_s9 ≈ 12-19, au-dessus du cadran bas (T=20 ; α=1→18,
   2→17, 3→17, 5→18, 8→18 cartes cumulées).
   **Lire `rr` en même temps que le cumul.** Au contrôle il s'effondre quand α
   monte : 123, 118, 118, 83, **3**. Le mécanisme s'éteint par le haut. Le
   prolongement peut donc mesurer un mécanisme INERTE et retrouver le témoin —
   ce qui expliquerait le « gain » marginal de la session 8 au lieu de le
   confirmer. C'est l'hypothèse à trancher, pas à supposer (pièges 42 et 52).
3. **A/B `--nrpa-level` à budget égal.** La constante était une variable cachée
   dans plusieurs A/B des sessions 5-7 ; elle est maintenant explicite et
   imprimée. C'est peut-être un gain gratuit.

## LA MISSION : donner au solveur une heuristique qui DÉCROÎT

Le diagnostic de la session 8 commande toujours la suite, et il est mesuré :

> Le verrou n'est plus dans l'algorithme de recherche. Il est dans `h`.

Notre `h` — le nombre de cartes cibles manquantes — vaut zéro sur ~90 % de la
ligne. **Un mécanisme de décomposition ne peut rien décomposer sur un paysage
plat.** La session 9 ajoute un fait à ce diagnostic : `h0` mesuré vaut 1 à la
racine `recul 0` et 4-6 aux racines profondes. `h` n'est pas plat *partout* — il
est plat *le long de la ligne*, ce qui est le cas qui nuit.

**(A) Le GRAPHE DE RECETTES (chantier 16).** Le combo est une rétrosynthèse sous
contrainte de matériaux de départ : DESP (arXiv:2407.06334, lu) formalise
exactement notre problème — molécule cible ↔ board cible, briques achetables ↔
deck, matériau imposé ↔ main d'ouverture, réaction ↔ invocation, impasse par
consommation ↔ pièce brûlée. On ne peut pas inverser un ÉTAT DE DUEL, mais on
peut inverser une INVOCATION, et les règles du jeu donnent le template
gratuitement. La distance sur ce graphe est le `h` qui manque.

Trois règles NON NÉGOCIABLES, dérivées de la revue et d'une objection soulevée en
séance (Kaleido Chick copiant le nom de Leo Dancer) :

1. **Le nœud OU est une EXIGENCE, pas une carte** (« un monstre de nom Leo
   Dancer », « un Lunalight au cimetière »). Une copie de nom ou un substitut de
   Fusion est un FOURNISSEUR de plus sous le même nœud, pas une exception. La
   zone entre dans le nœud — nos atomes de nouveauté sont déjà des triplets
   `(zone, carte, occurrence)` et `QueriedCard::Code()` distingue déjà le code
   physique de l'alias effectif.
2. **Le graphe ne PRUNE jamais, il ne fait que pondérer.** S'il servait d'oracle,
   une voie non modélisée supprimerait des solutions en silence — la forme exacte
   du piège 47. Au pire, `h` redevient le `h` plat d'aujourd'hui.
3. **Le texte de carte n'est qu'une amorce ; la vérité vient de l'observation**
   des matériaux consommés à chaque invocation, et toute recette proposée est
   VÉRIFIABLE par l'énumérateur.

Premier pas, petit et à faire d'abord : le **comptage**. « 3× Liger Dancer =
trois invocations Fusion » est un argument sur le multi-ensemble cible et la
decklist, sans modèle déclaratif. Il donne une borne de faisabilité plus fine et
un `--resolve` DÉRIVÉ au lieu d'écrit à la main. Attention : on compte les
ÉVÉNEMENTS d'invocation, jamais leurs déclencheurs — « trois Polymérisations »
serait faux, Lunalight Wolf fusionne depuis la zone Pendule sans Polymérisation.

**(B) Les OPTIONS (chantier 17).** Le seul levier identifié qui attaque
l'EXPOSANT et non la base : `0,74^160 ≈ 10⁻²¹`, mais `0,74^20 ≈ 2·10⁻³`. Si
« invoquer Kaleido Chick et envoyer Leo Dancer au cimetière » est UNE action, la
ligne passe de 160 décisions à ~20. Deux sources : le **minage des sous-séquences
fréquentes** du corpus (Macro-FF arXiv:1109.2154 ; Castellanos-Paez
arXiv:1810.09145), gratuit et applicable à l'étalon B ; et un **modèle de
fondation** (InnateCoder arXiv:2505.12508) pour le mode but seul, où il n'y a
précisément aucun corpus. Le critère de sélection est la **perte de Levin**
(arXiv:2410.11262) — et `ForecastSearchCost` la calcule déjà sur notre corpus :
*le gain est chiffrable avant d'écrire le mécanisme*. Attention : sa segmentation
passe par `ContextKey`, qui clampe à 15 (C13 le dit maintenant au démarrage).

**(C) La perte de Levin comme objectif de la politique (chantier 15).** Notre
`AdaptRun` optimise « ressembler à la meilleure séquence » ; la perte de Levin
`Σ d(n)/π(n)` pèse chaque étape par l'inverse de la probabilité déjà accordée,
donc cible en priorité les étapes à p ≈ 0 — exactement celles que §9.14 a
désignées comme responsables de l'effondrement de la masse. Convexe sous modèle
de contexte (arXiv:2305.16945), et notre politique par poids EST un modèle de
contexte dégénéré. Levier de MASSE par construction.

## L'état acquis — ne pas re-dériver

- **Le mode BUT SEUL existe et il est auditable** : `--target "carte[@ATK|DEF]"`
  (répétable) pose le board cible sans référence ; `--no-plan` écarte le
  répertoire APRÈS l'avoir relevé et imprimé ; `--no-ref` dégrade le replay
  positionnel au rang de gabarit de duel et l'imprime en tête du rapport.
- **Sur la politique (§9.14)** : classement au plafond (96 % contre 96,1/97,3 %
  calculés), masse insuffisante (44-74 %). Prior par poids, adaptation par
  gradient et contexte dans la clé agissent tous trois sur le CLASSEMENT : morts.
- **Sur le rerooting (§9.15/§9.16)** : `--reroot` (dur) et `--reroot-h <α>` (doux)
  existent, sont exclusifs, éteints par défaut. `rr=` et `h0=` sont imprimés dans
  les trois tables. À faible α le doux perd (cadran bas de la session 9).
- **Écarté sur STRUCTURE, avec la raison** (détail dans la revue) : recherche
  bidirectionnelle et régression ; heuristiques de relaxation et comptage
  d'opérateurs ; apprentissage de sketches ; génération EXPLICITE de sous-buts
  par autoencodeur (mesurée PIRE que LTS sur CraftWorld) ; famille NRPA.
- **Un résultat négatif obtenu gratuitement, à ne pas re-tester** : le HER
  propositionnel ne débloquera pas 3× Liger Dancer. Le domaine *Delivery* de
  arXiv:2512.19355 est la même instance et échoue de la même façon, avec le même
  symptôme : « les modèles apprennent à en livrer un seul ».

## Les étalons

**Étalon A — Lunalight, VÉRITÉ TERRAIN.** `D:\ProjectIgnis\deck\Lunalight.ydk`,
main 3× Fire Formation - Tenki, cible 3× Liger Dancer + Bagooska@DEF. La cible
est ATTEIGNABLE (ygocombo #105, 108 étapes) — c'est ce qui fait son prix. Montage
dans `tools/lunalight_butseul.ps1` (but seul) et `tools/lunalight3.ps1` (témoin).
Référence AVEC répertoire, s7_luna_v6 : LDS à 0 écart 2 038 555 états / 294 s /
**2 des 4 cartes** ; tirages 1/4.

**Étalon B — synchron, référence NEUTRALISÉE** (`--start` sur lui-même +
`--no-plan`). Board 8 cartes. Bout-en-bout 600 s : 7/8, aucune conversion.

**Étalon 0 — l'A/B DÉTERMINISTE du finisseur**, le montage à réutiliser pour tout
mécanisme qui ne touche que `RunLevin` : `tools/s9_ab_alpha.ps1` (ou
`s9_ab_alpha_haut.ps1`). Mêmes racines (`--approach` + reculs 0/10/20/30/45), même
politique (`--no-nrpa`), `--finisher-min`. **Contrôle de correction gratuit** : la
racine `recul 0` rend **42 expansions, `b=0`, ÉPUISÉ** dans tous les bras — une
fonction de coût change l'ordre, pas l'ensemble atteignable. Le `b=0` fait partie
du contrôle depuis la session 9 : sans lui, « ÉPUISÉ » ne prouve rien.

```powershell
# construire
& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe" `
    build\combosolver.sln /p:Configuration=Release /p:Platform=x64 /m /nologo /v:quiet

# sante — LA porte de tout changement de moteur, avant ET apres
.\bin\Release\combosolver.exe "D:\ProjectIgnis\replay\synchron handrip 2.yrpX" `
    --scriptdir ..\deps\scripts_2026-04-13\script --solve --solve-ms 60000 `
    --outdir s10_sante --no-chain Zalen --no-chain "Crystal Wing"
# 273 digests, 210/273 coups identifies, reference retrouvee a 0 ecart,
# 209 solutions, 19/56/272, 16 replays ecrits SUR 209 candidates.
# Sous chaque passe (k>=2) :
#   elagage : contrainte 0, garde 0, tour 0, borne 4, partition 4, sous-ens. 1
#   sante   : impasses 4, terminaux 0, nouveaute 3/23, atomes 79
# Le diff complet avant/apres ne doit contenir QUE des durees.
```

Note : `powershell -File` (5.1) écrit ses logs en UTF-16 ; `Select-String` les lit
correctement, `grep` non. Utiliser `pwsh` ou `Select-String`.

## Les pièges — la liste complète

1-31 : sessions 1-4 (les plus mordants : `Pop()` jamais `Discard()` (22) ;
`--resolve`/`--summon-min` = événements RARES uniquement (28, 31) ; une approche
se rejoue sur le duel de SON en-tête (21) ; jamais bit-à-bit à graine fixée (24) ;
équivalence de but sans position mais avec la face (26) ; pas d'élagage sur
« carte brûlée » (27) ; `Choice::card` couvre les fenêtres de chaîne (29)).

32. `--outdir` par défaut = `solutions/` (le corpus) : un `--outdir` dédié par run.
33. Un « résiste à N déviations » sans `--optimize` est un arrêt à 16 variantes.
34. Les brûlées ne sont pas monotones (pic 23 → 19).
35. La poursuite d'après-but est active sous `--optimize`.
36. Une réponse posée non traitée est ÉCRASÉE par le `SetResponse` suivant.
37. `no_chain` global ≠ continuation `--fire`.
38. Une contrainte `--resolve` ne dit pas OÙ résoudre.
39. À graine fixée, le bruit de run domine les compteurs de tirages — et **même
    un fait STRUCTUREL se compare à une DISPERSION, pas à un run**. Confirmé de
    façon indépendante par une étude Go-Explore (arXiv:2601.00042) : « la variance
    de graine domine les paramètres algorithmiques, facteur 8 ».
40. Une borne qui ne coupe jamais est un réglage mort — instrumenter AVANT de
    calibrer, et l'instrument doit appliquer la MÊME mise à jour que le run.
41. Une « convergence » d'escalade n'est qu'un plateau local (barre ATTEINTE sur
    la racine 259 : ne pas relancer l'escalade telle quelle).
42. Un mécanisme peut être VIVANT et sans effet.
43. (RETIRÉ par la 7bis.)
44. Une moyenne GÉOMÉTRIQUE de probabilités, lue seule, ment (44 % affiché là où
    le classement était à 96 %). Deux colonnes, et un plafond calculé.
45. Distinguer les leviers de CLASSEMENT et de MASSE **avant** d'écrire : ça
    prédit le signe. Session 8 : et un troisième type existe, ceux qui touchent
    l'EXPOSANT (options), qui dominent les deux autres.
46. Une perte — ou un gain — MONOTONE dans un cadran est un vrai résultat ; un
    point isolé ne prouve rien. Session 8 : **et un cadran se prolonge**, le gain
    de √LTS-H n'est apparu qu'à son bord supérieur.
47. Une carte qui lève une erreur Lua est une carte ABSENTE de l'espace de
    recherche. Lire le compteur « erreurs du core » avant toute conclusion.
48. `--scriptdir` DÉSACTIVE le scan automatique de `repositories/`.
49. Le plafond de décisions est dérivé de la référence (1,5× + 32) — SAUF sous
    `--no-ref`, où il vient de la decklist depuis la session 9. Il est imprimé.
50. Quand l'interdit a priori est trop grossier (`--no-chain` sur une carte à deux
    effets), juger A POSTERIORI.
51. Une date de fichier n'est pas une version.
52. **(Session 8)** Un compteur qui n'est pas IMPRIMÉ n'est pas un instrument.
    `SearchStats::reroots` existait depuis une session entière sans apparaître
    dans aucune sortie ; l'A/B de la 7ter a donc été tranché sans savoir que le
    mécanisme ne se déclenchait que 2 % du temps.
53. **(Session 8)** Mesurer un mécanisme LÀ OÙ IL AGIT. Imposer les racines
    (`--approach`) ET la politique (`--no-nrpa`) rend l'A/B déterministe.
54. **(Session 8)** Un mode peut retirer un MÉCANISME, pas seulement une
    information. Vérifier ce qu'un drapeau désactive indirectement.
55. **(Session 9)** Un correctif se vérifie avec l'instrument qu'il installe, pas
    avec l'hypothèse qui l'a motivé. `h_root` a été corrigé sous l'hypothèse
    « il vaut 1 dans le finisseur » ; la colonne `h0=` qu'ajoutait la correction
    même a montré 4, 5 et 6, et le cadran construit sur l'hypothèse était mal
    centré. Poser l'instrument, LIRE, puis dimensionner l'expérience.
56. **(Session 9)** Une table de partition à index haché n'est pas une partition.
    Deux clés distinctes qui se disputent une case produisent une SUPPRESSION,
    pas un partage ; et un dimensionnement dérivé d'une autre grandeur (ici la
    taille du plan) devient nul quand cette grandeur s'annule. Clé entière, échec
    OUVERT, compteur de refus.
57. **(Session 9)** Un filtre qui échoue OUVERT est pire qu'un filtre absent,
    parce qu'il continue d'être cru. Sur un chemin qui décode un format externe
    ou garantit un invariant, l'échec doit être un TROISIÈME état — distinct du
    succès ET du refus — et fatal ou compté, jamais silencieusement permissif.
58. **(Session 9)** Un compteur `thread_local` lu depuis un autre thread mesure
    zéro, toujours. Avant de croire un compteur à zéro, vérifier QUI l'écrit et
    QUI le lit.
59. **(Session 9)** « ÉPUISÉ » n'est une preuve d'absence que si TOUT ce qui
    tronque l'espace est compté : `borne`, `sous-ens.`, les prompts réduits à la
    réponse par défaut, et l'arène empoisonnée. Vérifier les quatre.

## Discipline de vérification — non négociable

Santé AVANT et APRÈS chaque changement de moteur, diff complet (seules les durées
peuvent bouger) ; tout replay écrit rejoué depuis zéro et jugé ; A/B sur faits
STRUCTURELS uniquement, comparés à la dispersion du témoin et non à un run ; un
A/B perdant se documente et se désactive par défaut ; **runs séquentiels — jamais
deux mesures en parallèle** (et ne pas relier le binaire pendant qu'une mesure
tourne : le lien échoue, ce qui est la bonne protection) ; `--outdir` dédié par run.

## Définition de « terminé »

(a) Les trois mesures de la section « À FAIRE D'ABORD ». (b) Le comptage dérivé
du board cible, implémenté et branché sur la faisabilité. (c) Le graphe de
recettes dans sa forme minimale — nœuds d'exigence, fournisseurs observés,
distance — et un `h` qui en dérive, mesuré sur les étalons A et B contre le `h`
actuel. (d) `--reroot-h` re-mesuré sur ce nouveau `h` : c'est le test décisif du
diagnostic de la session 8. (e) §9.17 documenté, ce prompt régénéré.
