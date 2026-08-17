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

## 8. Ce qui reste, dans l'ordre que les nombres donnent

1. **Extraction `Xyz.AddProcedure`** (producteur pour Bagooska) : débloque la
   sérialisation à 4 buts.
2. **Les deux déserts restants** (+46 aux réponses 72-118, +47 aux 239-286) :
   lister les activations du harnais dans ces fenêtres et trouver la place qui
   les rend visibles — poursuite du geste des barreaux de consommation.
3. Le critère est fixé : **ℓ_max ≤ ~8 décisions à choix**, sinon aucun budget
   ne franchit l'écart.

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
