# Session 22 — L'UNIVERSALITÉ SE CALCULE : l'extraction cardinale, les duaux du LP, et l'échelle qui se raffine elle-même

Tu reprends `combosolver` (racine `d:\ProjectIgnis\replay2video\combosolver`).

La session 21 a obtenu **le premier Liger de l'histoire du run nu** (`>=1` dans
1 972 tirages à 900 s, tenu en proportion : 2 graines sur 3 après
assainissement), par une chaîne entièrement mesurée : forme close →
échelle x* → retour au barreau → dépoisonnement → (board, quotas) en clé de
cellule. Elle a aussi **audité sa propre généricité** et trouvé un cas avéré de
fine-tuning de récompense (le barreau @BANNIE, Goodharté en deux runs par la
négation de Silver) — retiré, avec la règle extraite.

**Ta mission** : rendre le solveur UNIVERSEL — c'est-à-dire remplacer chaque
choix aujourd'hui ajusté à la main par un CALCUL, et étendre la couverture du
modèle pour que la chaîne s'arme sur n'importe quel deck. Le critère n'est pas
« ça marche sur Lunalight » : c'est « la même commande rend h fini, sous-buts,
quotas et conversion sur l'étalon B, sans une ligne spécifique ».

Lis `README.md` (les trois règles), `docs/rapport-session-21.md` (la synthèse),
`docs/combo-solver-design.md` **§9.33** (le détail, dont (e) la forme close et
(k)(l) les mécanismes), et `docs/etat-de-lart-consommation.md`. La
bibliographie est faite — ne la refais pas.

---

## CE QUE LA S21 A ÉTABLI, ET QUI NE SE REDÉMONTRE PAS

- **La forme close** : coût d'un run sérialisé = `Σ b^(ℓᵢ)`, dominé par
  `b^(ℓ_max)` ; seuil de correction **ℓ_max ≤ ~8 décisions à choix**. Le grain
  le plus fin gagne toujours (`q* = L·ln b > L`).
- **Le retour au barreau** (`--reenter`, défaut 0,5) fonctionne : 0 rejeu
  échoué sur 1,5 M de ré-entrées, frontière ×12 en proportion. La moitié de
  SIW_R qui manquait aux tirages est en place.
- **L'état pertinent est (board, quotas)** : ni le digest, ni les atomes, ni la
  cellule ne voyaient un compteur d'usage — le représentant « chemin court »
  était systématiquement l'état qui n'avait pas payé ses igniteurs. Corrigé
  par le compteur de chemin (MSG_CHAINING, 1 bit × 12 hôtes, bits 52-63 de la
  clé). **C'est ce correctif qui a donné le premier Liger.**
- **Deux poisons de récompense trouvés et retirés**, avec la règle : *un
  barreau de consommation ne vaut que si la consommation est un PASSAGE OBLIGÉ
  du plan — jamais quand un coût quelconque peut le servir* (Liger@CIMETIÈRE :
  l'échelle récompensait la destruction du but ; @BANNIE : la négation de
  Silver bannissait le Leo-matériau en coût, 98,9 % de suicides récompensés).
- **La mécanique convertit quand elle est proche** : isolation `--approach
  liger.yrpX` → 3/3 retrouvé depuis recul 10 (667 exp.) et recul 20 (14 303).
- **Le juge « ≥1 Liger » est un ÉVÉNEMENT RARE** (0,13 % des tirages au
  meilleur run) : 1 972 → 260 → 0 sur trois runs voisins de la même config.
  **N graines par bras, lecture en proportion, toujours.**
- **La sonde de généralité (étalon B Synchron)** : la machinerie tourne
  (149 places, 34 transitions, 65/65), la garde dit la vérité — mais les
  Synchros n'ont AUCUN producteur (`Synchro.AddProcedure` non extraite) →
  h = ∞ → l'échelle refuse de s'armer. **Tout l'édifice s21 n'est opérationnel
  que sur les combos Fusion.**

---

## CHANTIER 1 — L'EXTRACTION CARDINALE (la fondation, TROIS gains)

`Fusion.AddProcMixN(c, ..., 24550676, 1, IsSetCard(...), 3)` porte des codes
dans l'appel — c'est pour cela que les Fusions sont couvertes. Les procédures
`Synchro.AddProcedure`, `Xyz.AddProcedure`, `Link.AddProcedure`,
`Ritual.AddProcedure` portent des **comptes et des filtres** (niveau, tuner,
matériaux min/max) qui ne remplissent pas `unresolved_counts` aujourd'hui.

À extraire (même niveau de difficulté que `SetCountLimit` et `aux.Stringid`,
qui se sont lus) : le **compte de matériaux** de chaque procédure, versé dans
`DeclaredRecipe::unresolved_counts`. Ne PAS extraire le niveau/type exigé si
tu ne peux pas le lire — « N monstres » est plus faible que la vérité, donc
`h` reste admissible (règle de sous-contrainte de 9.30).

**Trois gains, chacun avec son juge :**

| gain | juge (gratuit, 0,2 s) |
|---|---|
| Bagooska a un producteur | `--target "90590304@DEF"` : la sérialisation s'ARME à 4 buts (aujourd'hui « aucun sous-but posé ») |
| les véhicules d'extra entrent dans x* → barreaux dans les déserts | banc du profil des écarts sur `liger.yrpX` : ℓ_max descend sous 33 |
| tout deck non-Fusion | sonde de généralité étalon B : `h` FINI + sous-buts posés + quotas dérivés |

**Piège nommé d'avance** : le LP « fusionne directement » — ajouter les
recettes des véhicules ne suffit PAS à les faire tirer par x* (mesuré en s21 :
ils ne sont pas requis par le plan relaxé). Le gain sur les déserts passe par
le chantier 3, pas par l'espoir.

---

## CHANTIER 2 — LES DUAUX DU LP REMPLACENT LES CHOIX À LA MAIN

Le fine-tuning de la s21 n'était pas des noms de cartes : c'était des **règles
de sélection itérées jusqu'à couvrir les pivots de l'étalon** (trois versions
de la dérivation des hôtes à quota « jusqu'à ce que Wolf apparaisse » ; le
choix des classes ADD_CODE/EXTRA_FUSION_MATERIAL). La dérivation principielle
est DANS le solveur :

1. **Quotas = contraintes de capacité ACTIVES à l'optimum.** Le simplexe rend
   déjà les coûts réduits ; expose les **duaux des lignes de borne** (`x_o ≤
   u_o`). Une capacité saturée par x* (dual > 0, ou x*_o = u_o) désigne un
   opérateur dont le quota LIE le plan — son hôte est un hôte à quota, calculé
   et non choisi. Remplace la dérivation actuelle de `quota_hosts_wiring`
   (garde-la en A/B témoin le temps d'une mesure).
2. **Habilitants = hôtes des transitions tirées par x*** (x*_t > 0), pas des
   classes d'effets choisies. Les barreaux de présence @EN JEU se dérivent de
   là — Chick et Masquerade doivent en RESSORTIR, pas y être poussés.
3. **Représentant de cellule** : à score de progrès égal, préférer l'état aux
   **ressources liantes non dépensées** (les mêmes duaux croisés avec les
   compteurs de quota du chemin) au lieu du chemin court. C'est le défaut qui
   a coûté trois runs de 600-900 s.

**Vie obligatoire** : imprime les contraintes actives et les hôtes dérivés
(« QUOTAS suivis (duaux) : ... ») — la s21 a vu trois dérivations fausses
UNIQUEMENT parce que la ligne de vie existait.

---

## CHANTIER 3 — LA RE-SÉRIALISATION DEPUIS LA FRONTIÈRE (l'échelle auto-raffinante)

Le run nu n'a pas de ligne de référence — le banc du profil des écarts ne peut
pas le guider. Mais l'histogramme `sp_final` sait dire « la frontière se
COMPRIME contre un mur » (mesuré : 17-19 unités avant le correctif quotas).
La réponse générique au « couper plus fin » de la forme close :

- au déclencheur (frontière comprimée : part de la masse dans les k derniers
  barreaux au-dessus d'un seuil, à définir et IMPRIMER),
- prendre les meilleures cellules-frontière, y résoudre le LP (le marquage se
  lit du duel — `BalanceModel::Solve` le fait déjà à ~2 ms),
- sérialiser sur le **x\* restant** : ses places non encore servies deviennent
  les barreaux d'une sous-échelle, la clé de cellule s'étend d'un niveau,
- le retour au barreau et le tournoi travaillent inchangés sur l'échelle
  raffinée.

C'est récursif, dérivé, et sans référence. Commence par UN niveau de
raffinement, mesuré : la frontière doit franchir le mur mesuré de la s21
(`>=2 Liger` est le juge sur l'étalon A, en proportion sur N graines).

---

## CHANTIER 4 — L'ÉLAGAGE D'IMPASSE À CAPACITÉS DYNAMIQUES

L'élagage d'impasse (chantier E historique) est **édenté** tel quel : le deck
recycle (« il MANQUE 1, à servir par recyclage » — notre propre marche 1),
donc brûler une pièce du but laisse le LP faisable. Il ne mord qu'avec des
capacités DYNAMIQUES : les compteurs de quota du chemin (chantier 2) injectés
dans le vecteur `u` avant `Solve`. Alors :

- un état qui a dépensé son recycleur ET brûlé une pièce → h = ∞ **prouvé** ;
- règle 2 tenue : on n'élague jamais un choix, on arrête un tirage démontré
  mort ;
- la valeur première n'est pas le débit (~0,7 % des tirages) mais l'HYGIÈNE :
  les lignes mortes hors de l'archive, du partage `shared_best` et de
  l'adaptation hindsight.

Coût : n'appelle PAS le LP par décision (~2 ms). Appelle-le sur ÉVÉNEMENT
(une pièce du but quitte DISPO, un quota lié se dépense) ou au moment
d'archiver. Mesure le coût avant de juger l'effet.

---

## CHANTIER 5 — L'ASSAINISSEMENT DE L'EXISTANT (dé-fine-tuning)

Chaque point ci-dessous est un écart nommé entre « générique de forme » et
« sélectionné pour l'étalon ». Pour chacun : dériver, ou baliser comme
hypothèse de JEU (pas de deck) avec sa mesure.

1. **L'observable « cimetière » de `ConsumedFrom`** : « ce que x* consomme
   atterrit au cimetière » est une hypothèse YGO-générale mais pas
   universelle (coûts bannis, mélangés au deck). Dérivation possible : la
   destination se lit de la catégorie du coût quand elle est déclarée ; sinon
   l'hypothèse reste, ÉCRITE, avec le troisième deck pour juge.
2. **Les constantes** : tranches de départs (2 × 15), reculs {15, 30, 45, 60},
   plafond de 12 hôtes, seuil du tournoi (2). Aucune n'est fausse ; aucune
   n'est dérivée. Balise-les (un bloc de constantes commentées « calées sur
   l'étalon A, jamais balayées ») et ne les balaye QUE si un juge gratuit le
   demande.
3. **Le budget du finisseur** : les premières racines de recul mangent tout
   (mesuré : 138-399 s chacune, la phase A2 — NRPA depuis les racines, avec
   re-entrée — n'a jamais tourné). Un plafond PAR RACINE (budget/racines,
   avec restitution) est la correction évidente ; mesure-la.
4. **Hindsight renforce les fusions précoces** (qui dépensent le quota de Wolf
   sur Tiger — le piège de choix nommé par le script). Mesure AVANT de
   corriger : ventile les buts de substitution par « quota lié dépensé
   oui/non ». Si le biais est réel, le correctif est un conditionnement du
   crédit, jamais un élagage.
5. **La négation de Silver reste jouable** (règle 2) : ne la bannis pas de
   l'énumération. Si tu veux la décourager, c'est un biais mesuré
   (`--self-negate-w`, famille de `--phase-w`), et l'A/B décide.

---

## MÉTHODE — les règles qui ont produit le Liger, à ne pas relâcher

- **La santé est la porte, avant ET après chaque changement** :
  ```powershell
  .\bin\Release\combosolver.exe "D:\ProjectIgnis\replay\synchron handrip 2.yrpX" `
      --scriptdir ..\deps\scripts_2026-04-13\script --solve --solve-ms 60000 `
      --outdir s22_sante --no-chain Zalen --no-chain "Crystal Wing"
  # 273 digests, 211/211, 209 candidates, 16 replays, 0 MSG_RETRY, 290/290/0.
  ```
- **Le banc structurel de l'échelle (gratuit, 0,2 s + 0,6 s)** :
  ```powershell
  .\bin\Release\combosolver.exe "D:\ProjectIgnis\replay\liger.yrpX" `
      --scriptdir "D:\ProjectIgnis\replay2video\deps\compat_2026-08" `
      --scriptdir ..\deps\scripts_2026-04-13\script `
      --scriptdir "D:\ProjectIgnis\repositories\delta-bagooska\script" `
      --scriptdir "D:\ProjectIgnis\repositories\delta-puppet\script" `
      --operators --target 54701958 --target 54701958 --target 54701958
  # auto-test 65/65 ; h(depart)=14 ; theoreme 2 : 13->0, 0 chute>1 ;
  # PROFIL DES ECARTS : l_max=33 (~17 a choix) — le nombre a faire descendre.
  ```
- **La sonde de généralité (le juge du chantier 1)** :
  ```powershell
  .\bin\Release\combosolver.exe "D:\ProjectIgnis\replay\synchron handrip 2.yrpX" `
      --scriptdir ..\deps\scripts_2026-04-13\script --operators `
      --target 50954680 --target 9753964
  # Aujourd'hui : « place SANS PRODUCTEUR », h = INFINI, echelle desarmee.
  # SUCCES = h fini + sous-buts poses + quotas derives, SANS ligne specifique.
  ```
- **Le run nu de référence (étalon A)** :
  ```powershell
  .\bin\Release\combosolver.exe gabarits\etalon_a_lunalight.yrp `
      --scriptdir "D:\ProjectIgnis\replay2video\deps\compat_2026-08" `
      --scriptdir ..\deps\scripts_2026-04-13\script `
      --scriptdir "D:\ProjectIgnis\repositories\delta-bagooska\script" `
      --scriptdir "D:\ProjectIgnis\repositories\delta-puppet\script" `
      --deck "D:\ProjectIgnis\deck\Lunalight.ydk" `
      --hand "8379983|3027001|3027001|3027001" --no-ref `
      --target 54701958 --target 54701958 --target 54701958 `
      --max-decisions 700 --watch 35618217 --watch 24550676 `
      --watch 54701958 --watch 47705572 --probe-repeat --options-online 60 `
      --op-recipes --op-bias 3 --solve-ms 600000 --finisher-min 240000 `
      --seed <graine> --finisher levin --archive-k 96 --outdir s22_<nom>
  # JUGE : proportion de graines a >=1 (acquis : 2/3) et >=2 (le chantier).
  # JAMAIS un run par bras : le juge est a 0,13 % des tirages.
  ```
- **Les juges gratuits d'abord** : harnais, banc, sonde de généralité, marche 1
  — 0,2 s chacun. Vingt minutes de mur ne se paient qu'après.
- **Un mécanisme doit imprimer sa VIE** avant tout A/B (les trois dérivations
  fausses de `quota_hosts` n'ont été vues QUE par la ligne « QUOTAS suivis »).
- **Un seul point de câblage** (`ApplyMechanisms`) ; tout champ ajouté à
  `Choice` se remet à zéro dans `Emit()`.
- **Règle 2** : jamais un élagage d'un choix jugé mauvais — un biais mesuré,
  ou l'arrêt d'un état PROUVÉ mort.
- Une mesure en cours **verrouille le binaire** (`LNK1104`) : voulu. Logs PS
  en **UTF-16** : `Select-String`, jamais `grep`. Runs séquentiels, un
  `--outdir` par run.
- **CODES et jamais NOMS** : `54701958` Liger · `24550676` Leo · `35618217`
  Kaleido Chick · `2344618` Masquerade · `47705572` Wolf · `35763582` Silver
  Hound · `90590304/90590303` Bagooska · `8379983` Gold Leo · `3027001` Fake
  Trap · étalon B : `50954680` Crystal Wing · `9753964` Hot Red Abyss.
- `py -3.11` + sympy pour toute formule neuve (`tools/s21_verify_formulas.py`
  est le gabarit ; **piège : `lpmin` IGNORE `nonnegative=True` — les
  contraintes `x ≥ 0` s'écrivent EXPLICITEMENT**).

## LES PIÈGES PAYÉS EN S21 — ne les repaie pas

1. **La récompense calquée sur la surface** (@BANNIE) : Goodhartée en deux
   runs. Un barreau se dérive de x* ou d'une ressource irréversible — jamais
   d'un type d'action vu dans la solution connue.
2. **La dérivation itérée jusqu'à voir la carte attendue** (trois versions
   pour Wolf) : c'est du fitting même sans nom de carte. Si tu itères un
   critère en regardant si LA bonne carte sort, arrête : dérive des duaux.
3. **Le représentant « chemin court »** : à vecteur égal il choisit l'état qui
   n'a pas payé. Toute clé incomplète re-crée ce défaut ailleurs.
4. **Un run par bras sur un juge rare** : 1 972 → 260 → 0 sur la même config.
   Proportion sur N graines, sinon tu lis du bruit (payé trois fois en une
   nuit).
5. **L'éviction d'archive écrivait `here.hash` au lieu de `cell`** ; **la
   branche d'élision n'ajoutait pas la réponse forcée à `path`** (solutions
   non rejouables sous `--elide-forced`). Les deux sont corrigés — mais la
   famille (une clé/un chemin incomplets qui ne cassent RIEN visiblement) est
   la plus silencieuse du dossier.
6. **La fusion de Wolf vit dans `Fusion.lua` partagé** : l'extraction par
   carte ne la voit pas. Toute dérivation « par produits déclarés » a cet
   angle mort — les procédures partagées se traitent au chantier 1.

## ÉTAT DU DÉPÔT

Commits : `a76bd55` (sympy, simplexe 65/65, barreaux de consommation),
`a75ad3f` (premier Liger : reenter, échelle, quotas), `7714d4e` (poison
Silver, audit du fine-tuning). Drapeau neuf : `--reenter <p>` (défaut 0,5,
0 = témoin). Sources clés : `operators.h/cpp` (LP, `ConsumedFrom`,
`lp_fuzz_cases.inc`), `search.h/cpp` (échelle zones 3/5/6, `ReenterMaybe`,
`QuotaKey`, `sp_final`), `main.cpp` (câblage, banc du profil, sonde `sp=` par
racine). Rapports : `docs/rapport-session-21.md`, design §9.33.

**La définition du succès de ta session, en une ligne** : la sonde de
généralité de l'étalon B rend `h fini + sous-buts + quotas dérivés des duaux`,
et l'étalon A passe `>=2 Liger` en proportion — les deux par le MÊME binaire,
sans un choix écrit à la main.
