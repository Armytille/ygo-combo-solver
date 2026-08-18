# Diagnostic : pourquoi la jonction ne converge pas (étalon B discipliné, s22quater)

*Run analysé : `s22_USER_B` — garde permanente, 20 min, graine 8305444233615674939,
binaire `f9854ee`. Verdict du run : board 6/6 atteint, chaque pièce du handrip
vivante, ≥3 résolutions = 0, crête jointe 0/6.*

---

## 0. La réponse, en une phrase

**Le run ne converge pas parce que tout le crédit des rips est POST-résolution
(score +250, barreau sp_eff, clé rvec, partition de nouveauté — tous ne paient
qu'APRÈS l'événement) alors que la jonction exige des rips PRÉPARÉS AU MILIEU de
la ligne ; l'échelle qui guide la montée (`serial_from_balance`) est
board-seulement — aucun barreau Omega/Trishula/Dis Pater n'existe — donc les
rips ne vivent que dans le spasme terminal des lignes (Omega : 1ʳᵉ invocation à
la décision 240,5 en moyenne, 12,9 décisions de vie restante), là où ni le
détour ni la refermeture ne tiennent plus.**

C'est exactement la limite nommée par le rapport du run (« le LP ne modélise
pas les rips ») — ce document établit que cette limite est LE verrou, mesure
la boucle auto-entretenue qui en découle, et nomme la sortie.

---

## 1. La forme de la jonction (lue sur la référence, pas supposée)

La séquence de la référence (31 invocations) entrelace :

- Trishula à l'invocation **#17**, Omega à **#23–24** — les rips vivent au
  MILIEU de la ligne ;
- les 6 pièces du board final sont les invocations **#25–31** — le board entier
  est (re)fabriqué APRÈS les rips (Crystal Wing #12 puis #28, Abyss #14 puis
  #29, Crimson Dragon #16 puis #25 : consommées comme matériaux puis ranimées,
  Crimson Dragon e2 en fin de ligne).

La jonction est donc un ORDRE : préparer les rips avant d'avoir dépensé la
réserve, rip₁ → Dis Pater → rip₂ → Trishula, PUIS refermer le board. Ce n'est
pas « board + rips », c'est « rips PENDANT, board APRÈS ».

## 2. Ce que le run fait réellement (mesures du log, mono-run)

| mesure | valeur | lecture |
|---|---|---|
| Omega, 1ʳᵉ invocation (phase tirages) | décision **240,5** en moyenne, **12,9** décisions restantes après | le rip vit dans le spasme terminal |
| Trishula, 1ʳᵉ invocation | 221,5 / 28,7 restantes | idem |
| fin des tirages NRPA | garde 282 087 (46 %), tour 329 532 (54 %) — 100 % coupés | les lignes meurent vers ~253 décisions |
| board seul (best approach) | 234 décisions, **r0** | le board consomme le même budget que la fenêtre où les rips devraient vivre |
| archive (946 cellules) : crête jointe | 6/6 seulement à **r0** ; r1 au mieux à **5/6** ; **r2 seulement à 2/6** | l'anti-corrélation board×rips, vue dans la structure même de l'archive |
| finisseur A2, racines rippées (top-3, toutes « 5/6 r1 ») recul 0 | 2 × ~100 000 tirages, rips **0/0/0** | depuis la meilleure frontière rippée, la continuation NE PEUT PLUS ripper |
| idem recul 20–40 | rips ≥1 : 580–3 425 par worker, **≥2 : 0 partout** | on re-fait rip₁, jamais le détour — la préparation de rip₂ est AVANT l'état archivé |
| détour Omega×2 (phase tirages) | 46 / 1 008 035 tirages | vivant, mais accident de fin de ligne |
| Trishula ≥1 | 446 / 1 008 035 | idem |
| ≥3 résolutions | **0** | voir §3 |

## 3. Le nombre qui dit « structurel, pas du bruit »

En indépendance optimiste : P(Omega×2) ≈ 4,6·10⁻⁵, P(Trishula≥1) ≈ 4,4·10⁻⁴ →
P(≥3) ≈ **2·10⁻⁸ par tirage**, soit ~0,02 co-occurrence attendue sur le 1,5 M
de tirages du run. Et l'hypothèse d'indépendance est GÉNÉREUSE : les deux
événements se disputent la même fenêtre terminale et les mêmes matériaux Synchro
(anti-corrélés), et il faut ENCORE le board 6/6 et la garde par-dessus. Zéro
observé est la prédiction du modèle, pas une malchance — aucun rallongement de
budget ne convergera tant que la conjonction n'est pas mise en barreaux.

## 4. La boucle auto-entretenue (pourquoi l'archive n'aide pas)

1. Les rips n'arrivent qu'en fin de ligne (aucun gradient avant l'événement) ;
2. donc les cellules rippées de l'archive sont des états de fin de ligne
   (les 3 meilleures : toutes 5/6 r1, continuations mortes) ;
3. donc re-entrer/reculer {0, 20, 40} depuis elles ne remonte jamais AVANT la
   préparation du rip (le détour exige Dis Pater, ~60–90 décisions en amont) ;
4. donc aucune cellule rippée MI-LIGNE n'est jamais découverte, et les rips
   restent en fin de ligne. Retour au 1.

Le NRPA n'en sort pas seul : sa politique est contextuelle — renforcer
« Omega à la décision 240 » ne fabrique pas « Omega à la décision 150 », les
contextes n'existent pas encore.

## 5. Les mécanismes qui auraient pu aider — état mesuré dans CE run

- **`--op-bias 3` : INERTE** (« 0 carte(s) désignée(s)... 0 décision(s) en
  offraient une ») — la décomposition à rebours a posé 0 sous-produit sur ce
  deck (« amorce par les OPÉRATEURS : 0 recette(s) déclarée(s) »). Le seul
  mécanisme qui biaise les CHOIX mi-ligne vers des intermédiaires nommés est
  mort sur l'étalon B. (Le canal LP/bilan, lui, marche : h(départ)=6 — deux
  pipelines distincts.)
- **Graphe de recettes : poids 0,00** — mesuré, n'entre dans aucun coût.
- **`--refine-after` : éteint** dans ce run ; et tel qu'implémenté il re-résout
  le MÊME LP (board restant) à la cellule — il poserait des sous-barreaux de
  board, jamais des barreaux de rip. Il ne peut pas combler cette jonction-ci.
- **s22quater (résolutions dans score + clé)** : fonctionne comme conçu (les
  cellules rippées existent, le finisseur s'y enracine) — mais c'est un crédit
  d'ARRIVÉE. Il ne crée pas le chemin.

## 6. La faisabilité de la conjonction : argumentée, pas témoignée

La référence n'est PAS un témoin du domaine (garde violée 2/47 ; son propre
rip₂ historique vivait au tour adverse avant la découverte du détour). La spec
opératrice argumente la couverture (garde par « Zalen@terrain + Junk
Signal@main » pendant la manœuvre ; S/T vide satisfiable en gardant les cartes
mortes en main ; détour Omega×2 vérifié T1-légal sur pièce, chaînes 21→23 du
replay). Aucun certificat machine n'existe. Le chantier de sortie en fournit un
gratuitement : un LP qui MODÉLISE les rips rend « h fini à la racine avec
demandes de rip » = faisabilité relaxée certifiée (et h infini dirait
« impossible même relaxé » — l'instrument qui a manqué sept sessions en s15).

## 7. La sortie, dans l'ordre

**Principe générique (directive opérateur : rien de calé sur un étalon)** : le
défaut est une asymétrie entre deux moitiés de la même spécification —
`--target` est COMPILÉ en demandes du bilan (→ x*, barreaux, gradient
mi-ligne), `--resolve` est RÉCOMPENSÉ post-hoc (→ aucun gradient avant
l'événement). La règle : *tout prédicat du but entre dans le modèle de bilan
comme demande ; aucun n'entre comme récompense de surface.* Compilation
dérivée, sans nom de carte dans le code : (1) chaque `--resolve X@Z:N` pose une
demande de présence `X@Z ×1` dans le LP (même chemin que `--target`, marquée
transitoire — hors du contrôle d'état final), servie par les producteurs déjà
extraits (pool + conversions) ; (2) N barreaux de résolution entrent dans
`serial_reqs`, adossés au compteur `resolved` existant, après le barreau de
présence. Admissible SANS modéliser les mécaniques custom (9.30) : demander une
production quand N résolutions sont exigées est plus faible que la vérité — ni
l'auto-bannissement, ni le 1/tour, ni le détour ne sont modélisés ; le détour,
la recherche le trouve déjà (46 tirages), il ne manquait que le gradient qui
place X au milieu de la ligne. Gardes tenues : barreaux dérivés de x* (jamais
d'un type d'action vu dans la solution) ; garde d'asymétrie (aucun effet
lisible → pas de couplage, dit) ; sans `--resolve`, périmètre à l'octet.
Nettoyage de la même famille : les reculs en constantes de décisions sont des
balises d'étalon — la forme dérivée recule AU BARREAU (k barreaux d'échelle en
arrière, profondeurs lues sur le chemin archivé), auto-dimensionnée à tout
deck. Juges : barreaux de résolution visibles au banc sur DEUX decks (étalon B
et un cas `--resolve` de l'étalon A), théorème 2 sur la référence, santé/banc A
inchangés à l'octet, puis A/B en proportion (juge ≥3 et crête jointe).

1. **Les résolutions deviennent des DEMANDES du modèle de bilan** (le chantier
   nommé). Forme : bornes inférieures sur les transitions nommées —
   x(rip_Omega) ≥ 2 (consomme Omega@TERRAIN, produit Omega@BANNI — déjà dans
   DISPO), x(retour Dis Pater) réalise @BANNI→@TERRAIN (producteur
   CATEGORY_SPECIAL_SUMMON déjà extrait par pool+conversions), x(rip_Trishula)
   ≥ 1. Alors x* TIRE le détour, la sérialisation pose les barreaux AVANT la
   refermeture (Omega@DISPO → Omega@TERRAIN → rip₁ → Dis Pater@TERRAIN → rip₂ →
   Trishula → re-board), et reenter/tournoi/finisseur travaillent inchangés sur
   une échelle qui CONTIENT la jonction — la forme close s'applique : couper la
   conjonction à 2·10⁻⁸ en blocs ℓ ≤ 8. Extraction à vérifier : la transition
   « activation d'un effet depuis le terrain » (le harnais s19 lit les
   enregistrements d'effets ; le rip d'Omega y est une activation à catégorie
   lisible). **Juges gratuits avant tout run** : le banc doit montrer les
   barreaux de rip dans « SÉRIALISATION par le bilan matière » ; théorème 2 sur
   la ligne de référence (h décroît, 0 état infaisable) ; h(racine) fini = le
   certificat de faisabilité du §6.
2. **Levier immédiat, mesurable au même budget** (en attendant le chantier) :
   la grille de reculs des racines rippées est {0, 20, 40} — sous le point de
   non-retour (le détour exige ~60–90 décisions d'amont ; la fenêtre mesurée au
   test 4 était 70–80). Recul 0 brûle ~200 k tirages pour un zéro structurel.
   Passer les racines rippées à {20, 60, 90, 120} et enraciner AUSSI la cellule
   r2. Juge : rips ≥2 dans les workers A2 > 0, en proportion sur N graines
   (instrument interne, pas un livrable).
3. **Nommer l'inertie `--op-bias` sur l'étalon B** (0 recette opérateur
   déclarée) — soit la couvrir, soit la baliser ; aujourd'hui la ligne de vie
   le dit et personne ne l'a lue.

---

*Câblage vérifié dans le code : `main.cpp:7468` (`cfg.serial_reqs =
serial_from_balance` — l'échelle vient du seul LP board) ; `search.cpp:1804`
(rp_rungs post-hoc dans sp_eff) ; `search.cpp:3175` (score de tirage :
`resolve_weight` = 250, crédit à la résolution) ; `main.cpp:9290-9314` (racines
rippées : top-3 par score, reculs {0,20,40}) ; `search.cpp:3898`
(ReenterMaybe : tournoi de 2 sur le score, sp_eff domine — 3 barreaux de rip
contre ~40 de board).*

---

## 8. LE CHANTIER EST FAIT (s23, commit `8ca29c0`) — et le contrôle mesure

Implémenté tel que §7 : chaque `--resolve`/`--summon-min` pose UNE demande de
présence `@DISPO` dans le bilan (jamais N — admissibilité), garde d'asymétrie
(sans producteur lisible : non posée, nommée), témoin `--resolve-legacy` ;
reculs des racines rippées dérivés de la longueur du chemin (0, L/12, L/6,
L/3) + la cellule la plus rippée toujours racine.

**Juges gratuits (tous passés)** : banc B + resolve → h(départ)=8 (fini),
x* tire « choisir Omega/Trishula @DISPO », barreaux MI-LIGNE aux réponses 154
(Trishula) et 210 (Omega) — où la référence les joue ; théorème 2 : 0 chute
> 1 ; h final=1 et 22 états h=∞ (Dis Pater@TERRAIN, décisions 243-248)
PRÉEXISTENT — témoin sans `--resolve` identique : lacune de modèle héritée,
pas un effet du chantier. `--resolve-legacy` → h=6, s22quater à l'identique.
Santé et banc A : inchangés.

**Contrôle (600 s + 150 s, graine 8305444233615674939, `s23_ctrl_600.log`)**,
contre le run diagnostic (20 min, même graine) :

| jauge | diagnostic (s22quater, 20 min) | contrôle (s23, 600 s) |
|---|---|---|
| échelle au départ | h=6, 18 sous-buts (0 rip) | **h=8, 23 sous-buts (Omega/Trishula @DISPO+@TERRAIN)** |
| Omega, 1ʳᵉ invocation (phase libre) | décision 240,5 (spasme terminal) | **185,7 — mi-ligne** |
| rips ≥2 au finisseur (A2, continuations) | **0 partout** (16 workers) | **~700 tirages** (7 workers : 96, 23, 52, 152, 41, 102, 230) |
| Omega×2 (détour) aux racines du finisseur | 0 | **80 tirages** |
| reculs des racines rippées | {0,20,40} — 200 k tirages à recul 0 pour 0 rip | {0,19,38,77} dérivés — le détour vit aux reculs 19-77 |
| ≥3 / crête jointe | 0 / 0-6 | 0 / 0-6 — **le dernier bloc (Omega×2 ∧ Trishula) reste dû** |

Lecture : le mécanisme fait exactement ce que le diagnostic demandait — les
rips ont quitté le spasme terminal, le détour est un phénomène de mécanisme
(finisseur) et non plus d'accident (46/10⁶). La conjonction complète ≥3 n'est
pas encore franchie à 600 s ; le run livrable (20 min, même graine) est la
mesure suivante.

## 9. La boucle de conversion (s23, suite) — trajectoire de la crête jointe

Le run livrable 20 min a CASSÉ le mur ≥3 : **47 tirages à résolutions
complètes** (Omega×2 ∧ Trishula, worker rippé recul 80), crête jointe 2/6.
Ces lignes mouraient avec le run → chantier `best_joint_*.yrp` (commit
`61b5d12`) : GoalCheck conserve le max lexicographique (rips, board, chemin
court — 3ᵉ axe ajouté en `fd2fc48` : la ligne 3r de 239 décisions finissait
son tour sans ressources, LTS épuisé à 34 expansions), fusion aux six sites
(approches comprises, gabarit VÉRIFIÉ — l'it3 a perdu sa meilleure ligne à
cause de l'exclusion aveugle), écrit à côté de `best_approach`, réinjecté par
`--approach` (l'instrument d'isolation s21).

| passe | budget | approche d'entrée | crête jointe | ≥3 (phase libre / finisseur) |
|---|---|---|---|---|
| s22quater (diagnostic) | 20 min | — | 0 | 0 / 0 |
| s23 run 1 | 20 min | — | 3r ∧ 2/6 | 0 / 47 |
| fumée | 60 s | — | 3r ∧ 1/6 (écrite !) | — |
| it2 | 5 min | 3r_1of6 | **3r ∧ 3/6** | 121 / 37 558 (contin. r3) |
| it3 (fusion aveugle) | 5 min | 3r_3of6 | perdue (2r_2of6 écrite) | 0 / 267 |
| it4 (corrigé) | 5 min | 3r_3of6 | 3r ∧ 3/6 (plateau) | 0 / ~700 ≥2 |
| final | 20 min | 3r_3of6 | 3r ∧ 3/6 (235 déc.) | 0 / ~1 200 |
| final2 (+`--refine-after 30000`) | 20 min | 3r_3of6 | **3r ∧ 4/6** (275 déc.) | 0 / ~2 100 (≥2 : 5 860) |

**Le raffinement a jugé POSITIF sur son premier vrai cas d'usage** : 10
workers re-sérialisés, 160 sous-barreaux, 576 745 états archivés AU-DELÀ de la
porte — et la crête jointe, plate à 3/6 sur trois passes, monte à 4/6 dans la
même passe (w15, appr recul 90 : crête-rip 4/6, 141 tirages ≥3).

**Le juge décisif du plateau (banc, gratuit)** : le théorème 2 marché LE LONG
de la ligne jointe elle-même (`best_joint_3r_3of6.yrp` comme replay du banc) :
h 8→3, **0 état infaisable, h final = 3** — les 3 pièces manquantes restent
fabricables en relaxation depuis la fin de la ligne. Le plafond 3/6 est un
trou de RECHERCHE (fermeture non guidée), pas un mur de ressources prouvé —
avec la réserve nommée : h ignore les quotas du CHEMIN (chantier 4), donc
« fabricable en relaxation » sur-estime peut-être l'état réel.

La mesure en cours : le même run + `--refine-after 30000` — la
re-sérialisation à la meilleure cellule (désormais une cellule RIPPÉE, avec
les demandes compilées) pose la sous-échelle de fermeture. C'est le cas
d'usage exact que la limite nommée du rapport s22quater désignait.

## 10. La boucle devient INTERNE (`--rounds`, commit `7ef1bee`)

Directive opérateur : « un utilisateur s'attend à ce que le programme retourne
le résultat final sans qu'il ne doive itérer lui-même ». La chaîne manuelle
des §8-9 est la forme canonique de Go-Explore / Expert Iteration
(`etat-de-lart-boucle-interne.md`) et vit désormais dans le binaire :
`--rounds <n>` découpe `--solve-ms` en rounds, réinjecte automatiquement la
meilleure ligne jointe (remplacement, pas empilement), s'arrête sur solution
— les solutions des racines d'approche comptent — ou à l'épuisement. Câblage
au site d'appel (`TransplantOutcome`), `rounds=1` = historique à l'octet.

Fumée (UNE commande, 240 s, 3 rounds) : R1 écrit 2r∧4/6, R2 la réinjecte et
écrit **2r∧5/6**, R3 stable — la chaîne manuelle reproduite sans opérateur,
crête montée dans la même commande. Suite de la mesure : le run livrable
mono-commande (3 × 20 min, raffinement armé, graine 8305444233615674939).

Reste nommé, dans l'ordre : (1) chantier 4 — capacités dynamiques
(red-black) : h honnête vis-à-vis des quotas du chemin, au raffinement et à
l'élagage ; (2) faire entrer les lignes du finisseur dans l'ARCHIVE du round
suivant (aujourd'hui seule la ligne jointe transite — Go-Explore garde tout) ;
(3) l'A/B en proportion (N graines) des mécanismes s23 : demandes @DISPO
(`--resolve-legacy` témoin), `--refine-after`, `--rounds`.
