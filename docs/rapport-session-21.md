# Rapport de la session 21

*Le détail est en §9.33 de `combo-solver-design.md`. Consigne de séance :
reprendre chaque formule, la dériver via sympy, débusquer les erreurs,
dégraisser, viser des formes closes — et débloquer le run nu.*

---

## 0. Le résultat, en une phrase

**Chaque formule du solveur est désormais dérivée ou réfutée par sympy**
(`tools/s21_verify_formulas.py`, exécuté en `py -3.11`), trois défauts réels sont
corrigés, l'auto-test du simplexe passe de 5 à **65 cas exacts**, et une
**forme close** (`q* = L·ln b`, coût dominé par `b^(ℓ_max)`) transforme la
question du run nu : ce n'est plus « quel mécanisme » mais « quel écart
d'échelle dépasse 8 décisions » — et cet écart est maintenant **mesuré et
nommé**. Le run nu ne convertit toujours pas (0/3 à 90 s), mais l'échelle est
passée de 17 à 30 unités, ℓ_max de 73 à 47 réponses, et l'archive de 16-18 à
**68 cellules**.

---

## 1. Les dérivations sympy (toutes les formules du dossier)

| bloc | verdict |
|---|---|
| identités log (moyenne géométrique, `d/π`, log-sum-exp base 10) | exactes |
| gradient NRPA sous température | l'update `α(δ−p)` est la règle Rosin, pas le gradient (`(δ−p)/τ`) — cohérent, c'est un rescalage de `α` |
| shrinkage `s = n/(n+k)` | correct (limites 0/1) |
| récurrence `λ/π` (levin_reroot) | **exacte** — égalité rationnelle, 200 chaînes |
| récurrence `hu/hv` (reroot_h) | jamais de sous-estimation, écart max ×1,42 vs min exact — unilatéral comme annoncé |
| arrondi `⌈c'x*⌉` | admissible (500 tirages) |
| théorème 2 sous capacités statiques | **contre-exemple construit** : la consistance tombe, l'admissibilité et le th. 3 tiennent (relaxation) |

**Le piège du juge** : `sympy.lpmin` **ignore** l'assomption
`nonnegative=True` — sans `x ≥ 0` explicite il résout à variables libres. La
première table de fuzz a « fait échouer » le solveur 9 fois : les 9 étaient la
faute de la référence. Un assert garde le script désormais.

## 2. Correctifs de code

1. **Simplexe durci** : les artificielles ne peuvent plus entrer en base
   (`enterable`) — remplace le coût 1e12, qui laissait une réentrée possible
   sur base dégénérée. C'est vraisemblablement lui qui rendait `h = 12` au banc
   s20 (vrai optimum : 13, celui du dossier). Membre droit ∈ (0, ε] écrasé ;
   `worst_violation` mesure l'excès.
2. **Éviction d'archive** : l'entrée évincée était réécrite avec `here.hash`
   au lieu de `cell` — la carte des cellules se corrompait en silence dès que
   l'archive était pleine sous sérialisation. *Le régime exact du run nu.*
3. **Température du finisseur** : la politique est apprise sous `logits/τ`, le
   finisseur la lisait sans diviser. Corrigé ; inerte à `τ = 1` (défaut).
4. **Dégraissage** : `best_log10` (code mort, doublon de `worst_log10`) retiré ;
   le faux commentaire « les MÊMES logits » du finisseur nomme désormais les
   biais réellement absents.
5. **Auto-test 65/65** : 60 instances aléatoires résolues en rationnels exacts
   (20 infaisables) commises en `lp_fuzz_cases.inc`.

## 3. La forme close qui gouverne la sérialisation

```
cout(q blocs egaux) = q·b^(L/q)        q* = L·ln b = 195 > L = 110
=> le cout DECROIT sur tout le domaine : LE GRAIN LE PLUS FIN GAGNE TOUJOURS
   q=1 : 10^85    q=14 : 2×10^7    q=33 : 1,2×10^4    q=110 : 649
a blocs inegaux : cout = Σ b^(ℓᵢ), DOMINE par b^(ℓ_max)
   ℓ_max=8 : 10^6    ℓ_max=12 : 10^9    ℓ_max=20 : 10^15
```

**Le seuil de correction du grain est ℓ_max ≤ ~8 décisions à choix.** Tout le
reste (politique, budget, biais) est du second ordre tant qu'un écart le
dépasse.

## 4. Le profil des écarts — l'instrument qui manquait, et ce qu'il a montré

Nouveau banc (gratuit, déterministe, dans la marche du théorème 2) : position
de chaque unité de sous-but de `x*` le long de `liger.yrpX`.

**Avant** : 17 unités, 14 paliers, écarts en tête **73 / 69 / 46** réponses —
trois déserts dont chacun interdit à lui seul le run nu (`b^37 ≈ 10^28,7`).
Nommés : +69 = du dernier corps au premier Leo (tout l'assemblage) ; +73 = de
Leo 2 à Liger 2 ; +46 = au milieu de la montée des corps. Cause structurelle :
les renommages produisent dans une place agrégée **déjà saturée**.

**Le correctif — les barreaux de CONSOMMATION** (`ConsumedFrom`) : la colonne
négative de `x*` rendue en sous-buts `@CIMETIERE` (zone 3 de `SerialReq`).
Chaque tir consommateur envoie un corps au cimetière : cette arrivée-là monte
pendant les déserts. C'est le « critère de progrès : consommation de x* » que
§9.31 nommait sans l'avoir construit.

**Après** : **30 unités, 33 paliers, ℓ_max 47** (~24 à choix), terme dominant
**10^18,5** (−10 ordres). Étalon A : 8 sous-buts (56 unités) au lieu de 5.

## 5. Le profil de progression par tirage (l'instrument de 9.32, construit)

`sp_final[k]` par garde RAII, agrégé, imprimé avec verdict automatique. Run nu
90 s : 277 584 mesures, dispersion sur les unités 1-11, ~0,07 % au-delà de 12,
dernier progrès à la décision 13,7 en moyenne.

**Verdict rendu par l'outil : DISPERSION — l'arité tue, couper plus fin.**
Pas de verrou unique. La question binaire de §9.32 est tranchée.

## 6. Le run nu

| run | résultat |
|---|---|
| s20, 300 s, 3 buts | 0/3, 6 monstres, archive 16-18 cellules |
| s21, 90 s, 3 buts | 0/3, 5 monstres, archive **68 cellules**, 30 racines |
| s21, 300 s, 3 buts | 0/3, 6 monstres, archive 54 cellules — l'histogramme monte à **19 unités sur 30** (9,9 % des tirages à 11, dernier progrès à la décision 26,1), queue exponentielle : la signature de l'ARITÉ, pas d'un verrou |
| s21, 90 s, 4 buts | 1/4, 6 monstres — la carte atteinte est **Bagooska** (régime cardinal, +2 matériaux), pas un Liger ; et **sérialisation NON armée** (voir ci-dessous) |

**La lacune qui a montré sa dent** : avec `--target 90590304@DEF`, la
sérialisation refuse de s'armer — Bagooska n'a **aucun producteur**
(`unresolved_counts` vide pour `Xyz.AddProcedure`, lacune nommée en s20 §7),
donc programme infaisable, donc « aucun sous-but posé ». La garde fait son
travail ; mais le run à 4 buts perd toute la sérialisation pour une lacune
d'extraction. **L'extraction `Xyz.AddProcedure` conditionne désormais
l'armement du mécanisme central.**

## 7. Invariants tenus

- Santé passée après chaque changement : `273 digests, 211/211, 209
  candidates, 16 replays, 0 MSG_RETRY, 290/290/0` — inchangée.
- Harnais : 44 activations, 0 non appariée, 20/20 zone, 11/11 ressource.
- Théorème 2 au banc : `h : 13 → 0`, **0 chute > 1** (s20 : 1 — c'était le
  solveur, pas le modèle), densité 5 % inchangée.
- Auto-test simplexe : 65/65 à chaque exécution.

## 7bis. LE RETOUR AU BARREAU (`--reenter`, seconde moitié de séance)

La relecture du code a montré que **seul le finisseur** prenait ses racines
dans l'archive-échelle : chaque tirage NRPA repartait de la racine — la forme
close dit que `Σ b^(ℓᵢ)` n'existe alors pas. Mécanisme câblé : avec
probabilité 0,5 (drapeau `--reenter`, 0 = témoin), un tirage rejoue le chemin
d'une cellule tirée uniformément (Go-Explore) et continue de là — compteurs de
préfixe armés par le même `StepToPrompt`, `path` semé (solutions rejouables),
graphe de recettes non pollué (`replaying`), rejeu échoué compté comme défaut.
Gardé par la sérialisation : santé inchangée, inertie annoncée.

**Défaut latent corrigé en câblant** : la branche d'élision de `PolicyRollout`
omettait la réponse forcée dans `path` — sous `--elide-forced`, solutions et
chemins d'archive étaient silencieusement non rejouables.

**A/B (90 s, graine 888)** : vie = 95 267 re-entrées (49,8 %), 0 rejeu échoué.
Tirages à ≥ 11 unités : 1,6 % → **21,2 %** ; à ≥ 14 : 20 tirages → **5 724**
(×400 en proportion), pour −30 % de débit. À 300 s : ≥ 14 unités 2,2 % →
**8,8 %**, ≥ 16 : ×12. **La frontière de progression se déplace** — première
fois du dossier — et se **comprime contre un mur à 17-19 unités** : le désert,
au barreau près. Le but reste 0/3.

**Les déserts nommés (rejeu `--verbose`)** : la fenêtre 239-286 est une suite
d'invocations spéciales intermédiaires — les **véhicules d'extra deck** du
« 15 → 4 », que x* ne modélise pas (le LP fusionne « directement »). Les
barreaux de bannissement (zone 4, Wolf/Masquerade bannissent les matériaux)
densifient la fin de ligne (30 → 39 unités au banc) mais tombent en rafale à
l'invocation : ℓ_max reste 47. **C'est la même lacune que Bagooska, vue de
l'autre côté** : les procédures Xyz/Lien sans compte de matériaux n'entrent
dans `A` ni comme producteurs (armement 4 buts) ni comme transitions (les
véhicules invisibles). Une extraction, deux gains.

## 7ter. LES DÉPARTS DE RÉSERVE, et le tournoi de réentrée (troisième moitié de séance)

L'analyse a réfuté mon propre plan : même extraite, la recette des véhicules
n'entrerait pas dans x* — le LP fusionne « directement », il ne tirerait
jamais ces transitions. Ce qui monte RÉGULIÈREMENT pendant les déserts, c'est
la ressource irréversible : **les départs de réserve** (deck+extra, le
« 15 → 4 »). Barreau zone 5 : une unité par carte sortie depuis la racine,
deux tranches de 15 (le champ `arch` porte le décalage — la première tranche
seule saturait à la réponse 118, juste avant les déserts). Partitions élargies
à 7 bits, histogramme à 72 cases.

**Mesure au banc** : ℓ_max **47 → 33 réponses (~17 à choix)**, terme dominant
**10^18,5 → 10^13**. Tous les déserts géants sont cassés ; le +33 restant est
la fenêtre de la première Fusion (163→196) — le régime nommé, une rafale de
sélections sans mouvement de zone.

**Run 90 s** : archive **359 cellules** (×5), 70 racines, base de réentrée
20,3 unités, frontière à **38 unités** — au seuil du premier Leo. La cascade
coule comme jamais : Chick activée 0,78 % (s19 : 0,14 %), Wolf 1,48 %,
**Leo au cimetière dans 333 tirages** (s19 : 6). Leo/Liger invoqués : 0 — la
porte Wolf+matériaux reste le dernier bloc.

**Le tournoi de 2** : l'uniforme sur 359 cellules diluait le budget (1/N par
cellule-frontière). Deux tirages, on garde le meilleur score — la masse double
sur la moitié haute sans abandonner les bas paliers.

**Run 600 s (tournoi armé)** : 592 526 ré-entrées (49,9 %), base moyenne 26,2
unités, **Leo au cimetière dans 9 859 tirages** (s19 : 6) — et Liger toujours
à 0. Le diagnostic est structurel : le but exige une CONJONCTION
(Leo-matériau ∧ concession de Masquerade active ∧ renommeur en zone) et
**l'échelle était aveugle aux habilitants** — Masquerade n'est pas dans x*,
donc une cellule « 40 unités avec Masquerade posée » et une « sans » avaient
la même clé, et le score (chemin court d'abord) gardait systématiquement la
mauvaise représentante.

**Les barreaux de PRÉSENCE des hôtes d'effets accordés** (dérivation générale,
règle 3 — aucun nom compilé) : tout hôte d'un `EFFECT_ADD_CODE` ou d'un
`EFFECT_EXTRA_FUSION_MATERIAL` présent au deck devient un barreau @EN JEU
(zone 6 = MZONE ∪ SZONE — **Masquerade est une magie continue, sa concession
vit en SZONE**, une zone 2 ne l'aurait jamais vue). Au banc : Chick @EN JEU à
la réponse 17, Masquerade @EN JEU à la 110, **51 paliers distincts** (40
avant). Les cellules porteuses d'habilitants sont désormais distinctes et le
tournoi les cible.

## 7quater. La conjonction TROUVÉE, le verrou NOMMÉ (script lu), et le finisseur déverrouillé

**Le script de Wolf, lu** (`c47705572.lua`) : sa fusion est une ignition à
`SetRange(LOCATION_PZONE)` (Wolf doit être POSÉ EN ZONE PENDULE), avec
`SetCountLimit(1)` — un usage par tour par copie — et `fextra` prend les
matériaux du **cimetière nativement**. Conséquence : le piège n'est pas
l'accessibilité mais le **CHOIX** — une fusion bon marché (Tiger) tirée tôt
dépense le quota et tue la fenêtre Liger du tour.

**La sonde de racine** (vecteur `sp=` par racine du finisseur, 4 bits par
exigence) prouve que **la conjonction existe dans les cellules-frontière** :
`Leo@CIMETIÈRE = 1 ∧ Chick@EN JEU = 1 ∧ Masquerade@EN JEU = 1` (slots 9/13/14).
Mais l'LTS s'y épuise en 2-5 expansions : ces états sont capturés en **fin de
tour**, tout est dépensé — et le quota de Wolf est INVISIBLE au vecteur.

**Les reculs déverrouillent** : depuis `recul 30`, l'LTS passe de 5 à
**15 717 expansions** (138 s, budget). Toujours 0/3 : le recul de 30 réponses
ne remonte probablement pas avant la dépense du quota. Reculs portés à
{15, 30, 45, 60} sur les 12 premières cellules, et budget finisseur réservé
(`--finisher-min`).

## 7quinquies. L'étude des briques (à la demande de l'opérateur), et ce qu'elle a corrigé

« Manque-t-il vraiment ces briques ? » — vérifié dans le code et contre nos
propres mesures, pas sur la foi de la revue :

- **Brique 1 (élagage d'impasse en tirage)** : absente du code (aucun LP dans
  search.cpp) mais **ÉDENTÉE seule** — le deck porte un RECYCLEUR (« capacité
  de RECYCLAGE sur l'extra : 1/tour + un recycleur sans borne déclarée », notre
  propre marche 1), donc un Liger brûlé laisse le LP faisable et l'élagage ne
  mordrait pas. Elle dépend de la brique 2 (capacités dynamiques).
- **Brique 2 (l'état-ressource dans les clés)** : CONFIRMÉE ligne à ligne — ni
  le digest (zones+payload+pile, search.cpp:554-613), ni les atomes, ni la
  cellule ne voient un compteur d'usage. C'est LA brique porteuse, et elle
  explique le représentant mort mesuré (conjonction au vecteur, LTS 5 exp).
- **Brique 3 (h^SEQ comme heuristique de nœud)** : RETIRÉE — notre banc la
  réfute probablement (densité 5 % contre 15 % pour la nouveauté, s20 et s21).

**Implémentation de la brique 2** : usages des hôtes à quota comptés le long du
chemin (`MSG_CHAINING`, aucun patch du core), un bit par hôte (dépensé/frais),
12 hôtes, injectés dans la clé de cellule (bits 52-63). Dérivation générale en
trois temps — produits de FUSION déclarés ∪ hôtes à effet borné dont les séries
croisent l'archétype du but, MOINS les cartes du but et les sorts à usage
unique (leur activation laisse une trace visible en zone ; l'ignition d'une
carte qui RESTE en jeu n'en laisse aucune — c'est elle que la clé doit porter).
Trois dérivations naïves ont coupé Wolf avant celle-ci (plafond, catégorie dans
Fusion.lua partagé, tri par code) — chaque fois vu par la ligne de VIE
« QUOTAS suivis », jamais par un silence.

## 7sexies. LE PREMIER LIGER DU RUN NU

Run 900 s, graine 888, échelle complète (13 sous-buts après dépoisonnement) +
clé de cellule (board, quotas) :

```
Lunalight Liger Dancer : >=1 1972    (toutes les sessions precedentes : 0, « JAMAIS »)
au mieux 1 des 3 cartes cibles ; frontiere a 47 unites ; archive 630 cellules
retour au barreau : 715 466 re-entrees (50,0 %), base moyenne 29,0 unites
best_approach_1of3.yrp ecrite (223 decisions)
```

**1 972 tirages invoquent un Liger.** La chaîne complète — forme close →
barreaux (consommation, départs, présence) → retour au barreau → dépoisonnement
→ (board, quotas) en clé — vient de faire tomber la discontinuité du régime
nommé. Le verrou était bien le REPRÉSENTANT de cellule quota-aveugle. Reste
`>=2 : 0` : la même conjonction à reconstruire une deuxième et troisième fois
dans le même tour — c'est le bloc suivant de la même échelle, plus une affaire
de budget et de grain que de mécanisme inconnu.

**Reproduit et borné à 1 800 s** : 1/3 stable (260 tirages à ≥1, meilleure
approche 113 décisions), `>=2 : 0`. Le bloc Liger 1 → Liger 2 est le plus long
segment de la référence (réponses 217 → 312, ~95 réponses) : la conjonction
entière à rebâtir — deuxième Leo-matériau, igniteur frais, corps — et c'est le
chantier de la s22, avec l'échelle et la clé désormais en place pour le porter.

## 8. Ce qui reste — UNE extraction, et elle est doublement payante

1. **Extraire le compte de matériaux des procédures Xyz/Lien** (l'argument de
   `Xyz.AddProcedure`, `Link.AddProcedure`). Elle rend à la fois : le
   producteur de Bagooska (l'armement de la sérialisation à 4 buts) ET les
   transitions « véhicule » que x* pourrait tirer — donc des barreaux DANS les
   déserts +46/+47, là où la ligne réelle invoque ses intermédiaires.
2. Le critère de réussite est fixé par la forme close : **ℓ_max ≤ ~8 décisions
   à choix** au banc du profil des écarts. Tant qu'un écart le dépasse, aucun
   budget ne le franchit — et avec le retour au barreau désormais en place,
   c'est le SEUL levier restant.
3. Rejouer alors l'A/B du run nu : la frontière (aujourd'hui comprimée à
   17-19 unités) doit atteindre les barreaux Leo/Liger, et la conversion 0/3 →
   ≥ 1/3 est le juge.

## 9. Inventaire

**Code** — `operators.h/cpp` (`ConsumedFrom`, simplexe durci, auto-test 65
cas, `lp_fuzz_cases.inc`) ; `search.h/cpp` (éviction d'archive corrigée,
zone 3 CIMETIERE de `SerialProgress`, température du finisseur, `sp_final[]` +
garde RAII, `best_log10` retiré) ; `main.cpp` (barreaux de consommation dans le
câblage, profil des écarts + échelle nommée dans le banc, histogramme agrégé et
imprimé avec verdict).

**Instruments neufs** — profil des écarts entre barreaux (banc, gratuit) ;
profil de progression par tirage (un compteur) ; fuzz du simplexe contre
rationnels exacts.

**Aucun drapeau nouveau** : les barreaux de consommation suivent `--no-serial`
existant ; le profil ne coûte qu'un compteur et s'imprime quand il vit.
