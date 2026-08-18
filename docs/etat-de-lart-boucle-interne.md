# État de l'art : la boucle interne, et la conjonction à sous-buts consommables

*Session 23. Question de l'opérateur : la réinjection manuelle
(run → `best_joint_*.yrp` → `--approach` → run) doit devenir un mécanisme du
solveur — « une commande, un résultat final ». Existe-t-il une solution propre
dans la littérature, pour la boucle ET pour notre classe de problème ?*

**Réponse courte : oui, et la boucle que nous simulions à la main est
exactement la forme canonique de trois familles d'algorithmes publiés. Elle
appartient À L'INTÉRIEUR du programme. Notre compilation des résolutions dans
le LP est elle aussi une instance d'un cadre publié (operator-counting avec
contraintes de jalons), et la partie encore optimiste du modèle (quotas du
chemin) a un nom : relaxation partielle red-black.**

---

## 1. La boucle interne : trois familles, une même forme

### (a) Go-Explore — l'archive qui itère jusqu'au bout
Ecoffet, Huizinga, Lehman, Stanley, Clune, *Go-Explore: a New Approach for
Hard-Exploration Problems* (arXiv:1901.10995) ; *First return, then explore*
(Nature 590, 2021 ; arXiv:2004.12919).

Principes : mémoriser les états prometteurs (cellules), Y RETOURNER d'abord,
explorer ENSUITE — et itérer ce cycle en continu, dans un seul processus,
jusqu'à solution ; puis une phase finale de ROBUSTIFICATION. Notre
archive/`--reenter` est déjà du Go-Explore (cité dans le code depuis s21) —
mais notre cycle s'arrêtait à chaque fin de run : l'archive mourait, et le
transport se faisait par fichiers (`best_joint` → `--approach`). Dans
Go-Explore, l'archive PERSISTE et les découvertes de chaque phase RENOURRISSENT
l'archive. C'est le chaînon manquant nommé : chez nous, les découvertes du
finisseur (A1/A2) ne réintègrent jamais l'archive ni la politique.

### (b) Expert Iteration — la politique qui porte le progrès entre les rounds
Anthony, Tian, Barber, *Thinking Fast and Slow with Deep Learning and Tree
Search* (NeurIPS 2017 ; arXiv:1705.08439) ; pour le MONO-agent : Laterre et
al., *Ranked Reward: Enabling Self-Play RL for Combinatorial Optimization*
(arXiv:1807.01672) ; Feng, Gomes, Selman, *Solving Hard AI Planning Instances
Using Curriculum-Driven Deep RL* (IJCAI 2020 — Sokoban, le curriculum est
construit PAR le solveur depuis ses propres réussites partielles).

Forme : alterner EN ROUNDS INTERNES la recherche (l'expert, qui produit des
lignes) et l'apprentissage (l'apprenti, qui généralise et guide la recherche
suivante). Notre politique NRPA + le minage d'options (`--options-online`)
sont l'apprenti ; aujourd'hui ils repartent de zéro à chaque commande.

### (c) Largeur + politique entrelacées
Junyent, Jonsson, Gómez, *Deep Policies for Width-Based Planning* (π-IW,
ICAPS 2019 ; arXiv:1904.07091) ; O'Toole, Lipovetzky, Ramirez, Pearce,
*Width-based Lookaheads with Learnt Base Policies* (N-CPL ;
arXiv:2106.12151). La nouveauté (IW) et la politique apprise s'améliorent
mutuellement, en boucle interne — notre paire novelty/politique, itérée.

**Conclusion (a)-(c)** : la « solution propre » n'est pas un nouvel
algorithme — c'est déplacer notre boucle DANS le processus : des rounds
internes qui partagent (i) l'archive (finisseur compris), (ii) la politique et
le corpus d'options, (iii) le ré-enracinement sur la meilleure ligne jointe.
Les artefacts (`best_joint_*.yrp`) restent des CHECKPOINTS exportés, plus le
moyen de transport.

## 2. La conjonction à sous-buts consommables : les noms et les correctifs

### (a) Notre mur a un nom : buts non sérialisables de SIW
Lipovetzky & Geffner (ECAI 2012) : SIW échoue « quand le but n'est pas
facilement sérialisable ». La réponse publiée : les **policy sketches** —
Bonet & Geffner (AAAI 2021) ; Drexler, Seipp, Geffner, *Expressing and
Exploiting the Common Subgoal Structure of Classical Planning Domains Using
Sketches* (KR 2021 ; arXiv:2105.04250) et *Learning Sketches for Decomposing
Planning Problems into Subproblems* (KR 2022) : des règles ⟨condition →
changement de traits⟩ qui découpent le problème en sous-problèmes de largeur
bornée, SANS exiger qu'un sous-but s'atteigne en un pas, et qui s'APPRENNENT.
Notre échelle dérivée du LP + les barreaux de résolution = un sketch dérivé
par la machine ; le raffinement (`--refine-after`, jugé positif en s23) = le
raffinement de sketch. La filiation est exacte et VALIDE l'architecture.

### (b) Notre compilation a un nom : operator-counting avec jalons
Pommerening, Röger, Helmert, Bonet, *LP-based Heuristics for Cost-optimal
Planning* (ICAPS 2014) — un SEUL LP qui accueille des familles hétérogènes de
contraintes : équation d'état (notre bilan matière — van den Briel et al.
2007 ; Bonet 2013), **contraintes de landmarks** (nos demandes de
résolution !), et toute contrainte prouvée. Le cadre est encore actif
(reconnaissance de but par operator-counting : arXiv:2404.07934). Compiler
`--resolve` en demandes du LP n'était donc pas un bricolage local : c'est LE
geste standard du cadre.

### (c) L'optimisme restant a un nom : red-black
Katz, Hoffmann, Domshlak (AAAI 2013) ; Domshlak, Hoffmann, Katz, *Red-black
planning: a new systematic approach to partial delete relaxation* (AIJ 2015).
Peindre en NOIR (sémantique réelle) les variables consommables/à quota, en
ROUGE (relaxées) le reste. Notre chantier 4 (injecter les compteurs de quota
du chemin dans les bornes `u` avant `Solve`) est exactement une relaxation
partielle red-black du bilan — la direction est publiée et sûre.

### (d) Les sous-buts APPRIS à distance k
Czechowski et al., *Subgoal Search For Complex Reasoning Tasks* (kSubS,
NeurIPS 2021 ; arXiv:2108.11204) ; Zawalski et al., *Adaptive Subgoal Search*
(AdaSubS ; arXiv:2206.00702) — Sokoban, Rubik, preuves : générer des ÉTATS
intermédiaires à k pas, VÉRIFIER leur atteignabilité, planifier entre eux.
Le cousin neuronal de notre sous-échelle ; confirme le motif
« générer/vérifier des jalons intermédiaires » sur des domaines à
consommation (Sokoban). Analogue industriel : la rétrosynthèse chimique
(Segler et al., Nature 2018) — cibles fabriquées par réactions qui CONSOMMENT
des précurseurs nommés, résolue par recherche AND/OR guidée par politiques
apprises, itérée en interne.

## 3. Ce que cela prescrit pour combosolver, dans l'ordre

1. **Internaliser la boucle (directive opérateur, forme Go-Explore/ExIt)** :
   des ROUNDS internes sous un seul budget/une seule commande —
   round = { tirages + finisseur } ; entre les rounds PERSISTENT : l'archive
   (les lignes du finisseur y ENTRENT — aujourd'hui elles meurent), la
   politique NRPA fusionnée, le corpus d'options, et le ré-enracinement
   interne sur la meilleure ligne jointe (l'équivalent de `--approach`, sans
   fichier). Arrêt : solution trouvée ou budget épuisé. `best_joint_*.yrp`
   devient un checkpoint de sortie.
2. **Garder la compilation LP** (validée §2b) et la pousser vers red-black
   (§2c — chantier 4 : quotas du chemin dans `u`), ce qui rend aussi h honnête
   au moment du raffinement.
3. **Le raffinement** est la version dérivée des sketches (§2a) — son A/B en
   proportion reste dû, mais la filiation justifie d'en faire un défaut
   candidat après mesure.

*Méthode de la recherche : arXiv (recherches ciblées Go-Explore / ExIt /
width+policy / operator-counting / subgoal search, 2026-08-18) + canon
ICAPS/JAIR/AAAI hors arXiv cité de connaissance. Les affirmations sur nos
mécanismes renvoient aux mesures des runs s23 (docs/diagnostic-jonction-etalon-b.md).*
