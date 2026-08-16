# Session 20 — LA CONSOMMATION, et le biais d'opérateur qui n'a jamais été jugé

Tu reprends `combosolver` (racine `d:\ProjectIgnis\replay2video\combosolver`).

La session 19 a fait ce que trois sessions avaient sauté : **vérifier que le
modèle décrit le jeu avant de bâtir dessus**. Le harnais d'opérateurs passe, il
a trouvé deux défauts dans du code écrit la veille, et le nœud qui manquait au
graphe est posé — `--backward` n'est plus « un mécanisme correct sur un graphe
amputé ».

Lis `README.md` (les trois règles), puis `docs/combo-solver-design.md` **§9.27**
(le Lua, la cascade mesurée) et **§9.28** (le harnais, les deux défauts, le
re-jugement de `--backward`), puis `docs/drapeaux.md`.
La bibliographie est faite (§9.22 (h), §9.23 (c), §9.24) : ne la refais pas.

---

## CE QUE LA SESSION 19 A ÉTABLI, ET QUI NE SE REDÉMONTRE PAS

**Le harnais passe.** Sur le plan résolu (283 décisions, 0 `MSG_RETRY`, 2 Liger) :
37 activations, **0 non appariée**, 18/18 préconditions de zone tenues, 10/10 de
ressource. La table d'opérateurs extraite des scripts **décrit le jeu**.

**Le recensement des états accordés rend les deux pivots**, par un balayage de
constantes et sans nommer une carte :

```
Lunalight Kaleido Chick  ->  EFFECT_ADD_CODE
Lunalight Masquerade     ->  EFFECT_EXTRA_FUSION_MATERIAL
```

**Le nœud « code ACQUÉRABLE » est posé** (`--op-recipes`), et la décomposition à
rebours — désormais **imprimée** — contient enfin l'opérateur :

| | témoin | `--op-recipes` |
|---|---|---|
| sous-produits | 2 — Liger, Bagooska | **3 — Leo Dancer, Liger, Bagooska** |
| exigences | Leo @zone jouable · archétype `0xdf` ×3 · niveau ×2 | + **Leo @EXTRA** · **KALEIDO CHICK @TERRAIN** |

### Trois faits neufs à garder sous les yeux

1. **`aux.Stringid(id, n)` = `(n & 0xfffff) | code << 20`**, et non `id*16 + n`.
   Le décalage est **lu** dans `utility.lua`, jamais supposé. La version fausse
   laissait `Choice::card` à **zéro** sur tous les `MSG_SELECT_YESNO` — le pivot
   du combo était invisible au biais d'indices et aux sondes, en silence.
2. **Le core FABRIQUE des descriptions.** `processor.cpp:743` émet `221` et
   `:443` émet `0` pour « activer l'effet déclencheur de cette carte ? ». Le
   message porte la **carte**, pas l'**effet** : **11 activations sur 37** ne
   sont identifiables qu'au grain de la carte. C'est une **borne du protocole**,
   pas un défaut d'extraction — et elle plafonne ce que l'identité
   `(code, description)` de la s18ter peut séparer.
3. **La table est déclarative et OPTIMISTE.** Conditions et coûts sont des
   fermetures, non évaluées. Pour aller plus loin il faut le patch d'ocgcore qui
   expose `peffect->get_category()` sur `MSG_SELECT_IDLECMD` — le projet l'a déjà
   fait pour `OCG_DuelQueryProcessorState`.

---

## CHANTIER A — JUGER `--op-bias`, ET IL N'A JAMAIS ÉTÉ MESURÉ

Écrit en session 19, **instrumenté, VIVANT, jamais jugé**. C'est le chantier 2 du
dossier : depuis les faits de but non satisfaits, remonter aux opérateurs
disponibles, et **biaiser la politique sur les décisions `IDLECMD`**.

**Sa vie est déjà mesurée et non nulle** (étalon A nu, déterministe, 4 000
tirages, `--recipes 0 --op-recipes --op-bias 3`) :

```
biais d'OPERATEUR : 1 carte(s) designee(s) par la decomposition ;
                    704 decision(s) en offraient une, 666 l'ont prise (94,60 %)
```

La carte désignée est **Kaleido Chick**, et c'est la bonne. **Il ne reste donc
que l'effet à juger** — et 94,6 % de prise à `w = 3` est un candidat sérieux à
l'effondrement de diversité, donc `w` doit être balayé avant tout verdict.

*Ce qu'il fait, et en quoi il diffère de `--assign-bias`* : ce dernier désigne
des **matériaux** et mord sur les prompts de **sélection** ; `--op-bias` désigne
des **opérateurs** — les cartes dont la décomposition exige la présence **sur le
terrain** — et mord sur « que jouer ».

*L'arithmétique qui le justifie, et elle est la seule à autoriser un levier* :
le plan résolu fait 143 décisions **libres**, dont **32 seulement** sont des
`IDLECMD`. La politique plafonne à **66 %** d'accord par décision ; il en
faudrait 91 % sur 143. Mais `0,66^32 ≈ 1,7 × 10⁻⁶`, soit de l'ordre d'un succès
par run de 90 s. Un plan ne remplace pas l'échantillonnage : il **conditionne la
distribution sur les 32 décisions qui comptent**.

**Le protocole, et il n'est pas négociable :**

1. **Relire la VIE à chaque changement.** À `M = 0` le mécanisme est INERTE et
   aucun juge de recherche ne le concerne. Le dossier a payé **trois** sessions
   pour avoir sauté cette lecture (§9.26 (e), §9.28 (c), §9.28 (f)).
2. Puis l'étalon A **nu**, `--recipes 0 --op-recipes --op-bias <w>`, juge :
   la colonne « Kaleido Chick — renommage activé » passe-t-elle au-dessus de
   **0,14 %** ? C'est **le goulot mesuré** (§9.27 (c)), et c'est exactement ce
   que ce biais vise.
3. Balayer `w` (1, 3, 6) : un poids est un pari tant qu'il n'est pas balayé.
4. Puis l'étalon B, **N runs, lecture en proportion**.

**Règle 2 du dossier, non négociable : un plan est un BIAIS, jamais un
élagage.** Si le planificateur se trompe, l'échantillonneur couvre encore
l'espace.

## CHANTIER B — LA CONSOMMATION

3 Liger = **3 codes-Leo + 9 corps Lunalight + 3 portes**, dont deux Wolf (un par
copie) et une Masquerade payée par une défausse.

C'est la leçon de §9.24 (d), et elle a déjà tué `--recipe-w` : *une heuristique
h^add sur un graphe ET/OU n'est correcte que si la CONSOMMATION est modélisée* —
la relaxation par suppression suppose qu'atteindre un sous-but ne détruit rien,
hypothèse **exactement fausse** ici.

Le graphe sait désormais dire ce qu'un opérateur **exige** ; il ne sait toujours
pas dire ce qu'il **détruit**. La matière est là : `Duel.SetOperationInfo` porte
la catégorie *et* la zone, et les verbes (`SendtoGrave`, `Remove`, `DiscardHand`)
sont relevés par fonction dans `CardOperators::fn_verbs`. **Rien n'en est
encore fait.**

Deux gardes, écrites parce que la première version du chantier 1 les rendait
fausses, et qui valent pour celui-ci :

- `Requirement::zone` est **un seul seau** : il ne sait pas dire « DECK ou
  EXTRA ». Toute nouvelle arête doit trancher la zone sur un **fait** (le deck),
  jamais sur une normalisation en aveugle ;
- une arête qui boucle sur elle-même (acquérir son propre nom) coûte à chaque
  niveau de récursion dans un graphe déjà cyclique.

## CHANTIER C — LA DETTE MÉCANIQUE, ET ELLE A CHANGÉ

1. **`--backward` n'est plus à retirer.** Le chantier 1 l'a ressuscité :
   `docs/drapeaux.md` §A doit être corrigé, et la suppression annulée.
2. **`--prior` / `--prior-weight`** restent neutres sur les deux étalons : à
   retirer, santé avant et après.
3. Les **27 jamais jugés** de `docs/drapeaux.md`, plus `--op-bias`. Attention :
   `--canonical-zones` **casserait le combo** (les Zones Pendule sont des
   séquences particulières de `LOCATION_SZONE`, et Wolf n'invoque que de là) —
   le départager tel quel rendrait un verdict faussement négatif.
4. **La promotion est FAITE pour deux des trois.** `--adapt-to-peak` et
   `--hindsight 0.5` sont le **défaut** (étalon B, dix runs par bras : `>=2`
   passe de **3/10 à 9/10**) ; `--no-adapt-to-peak` et `--no-hindsight` rejouent
   l'A/B. **`--elide-forced` NON** — voir le chantier D, qui est plus grave.

## CHANTIER D — UN SEUL POINT DE CÂBLAGE, ET C'EST LA TROISIÈME FOIS

`cfg.elide_forced` n'était assigné **qu'à un seul endroit** :
`RunGrowthMeasurement`. Ni `RunSolve` ni `RunTransplantSolve` ne le câblaient.
**Trois sessions de bancs lui ont passé le drapeau sur le chemin de RECHERCHE, où
il ne faisait rien** — preuve déterministe : `3000 tirages, 149334 états,
3002 adaptations` **à l'octet près** avec et sans (§9.28 (f)). Câblé, il rend
186 915 états contre 149 334 à tirages égaux.

**C'est la troisième occurrence du même piège**, après `--assign-bias`
(§9.26 (e)) et le `Choice::card` des prompts oui/non (§9.28 (c)). « Un mécanisme
doit imprimer sa vie » ne suffit pas : ici, il n'y avait **rien à imprimer**.

*La règle qui manque, et elle est mécanique* : **tout champ de `SearchConfig`
doit être assigné depuis `Options` en UN SEUL point de câblage.** Trois
`SearchConfig` sont construits dans `main.cpp` — `RunSolve` (3938),
`RunTransplantSolve` (6238), `RunGrowthMeasurement` (9191) — et **aucun contrôle
ne dit lequel oublie quoi**. Le premier travail est de le lire, champ par champ,
et de faire converger les trois vers une fonction unique. C'est mécanique, c'est
falsifiable, et cela vaut plus qu'un mécanisme de plus.

**Conséquence immédiate à ne pas oublier** : `--hindsight` et `--adapt-to-peak`
ne sont câblés que dans `RunTransplantSolve`. Le mode `--solve` **même-deck** (le
contrôle de santé !) n'en a jamais vu un seul — c'est structurel, pas un hasard,
et cela explique que la santé soit restée identique à travers la promotion.

**Et `--elide-forced` redevient NON JUGÉ sur la recherche.** Les « +61 % de débit,
×2,1 de boards » de §9.24 (k) ont été mesurés avec `--growth`, le seul chemin qui
le câblait. Son défaut reste **éteint** : un correctif de câblage ne vaut pas une
mesure. Le juger là où il agit désormais est un A/B propre et pas cher.

## CE QUI RESTE OUVERT, ET QUI N'EST PAS DE CETTE SESSION

- **`ProcessorState` est-il encore nécessaire ?** ×1,00 aux points idle, et
  `--elide-forced` retire 74,6 % des prompts intermédiaires de la table. La
  justification du patch C1 n'a pas été re-mesurée. **Faisable maintenant** en
  mode déterministe.
- **L'adversaire dans la clé** : `StateDigest` boucle sur les deux joueurs, la
  moitié des requêtes porte sur un adversaire qui ne joue pas. Question de
  **débit**, pas de clé.
- **Le finisseur seul n'a jamais été jugé.** `--finisher-min` proche de
  `--solve-ms` le fait sans code.
- **La sensibilité des 11 paris** du score (100, 10, 3, 1, `hint_bias` 2,0,
  `resolve_weight` 250, budgets ×0,7 et ×0,8, `qhat_*`, `hindsight_k`).
  **Faisable maintenant** : le mode déterministe existe.
- **L'étalon B n'a aucun run nu** — `--resolve` y est l'énoncé du but *et* un
  triple indice.
- **Les scripts Lunalight ne sont PAS dans l'export épinglé** : ils viennent de
  l'installation vivante via le repli `assets.cpp:253` (`<workdir>/script`).
  `MSG_RETRY = 0` aujourd'hui ; rien ne le garantit demain. **Et la table
  d'opérateurs hérite du même risque** : elle est lue par le MÊME
  `ScriptProvider`, donc un décalage de scripts décale aussi la table.
- **Un troisième deck** reste le seul juge de généralité, et il n'existe pas.

---

## MÉTHODE

- **La santé est la porte, avant ET après chaque changement** :
  ```powershell
  .\bin\Release\combosolver.exe "D:\ProjectIgnis\replay\synchron handrip 2.yrpX" `
      --scriptdir ..\deps\scripts_2026-04-13\script --solve --solve-ms 60000 `
      --outdir s20_sante --no-chain Zalen --no-chain "Crystal Wing"
  # 273 digests, 210/273, 209 candidates, 16 replays, 0 MSG_RETRY.
  ```
- **Le harnais est la porte du MODÈLE, et il coûte un run** :
  ```powershell
  .\bin\Release\combosolver.exe "D:\ProjectIgnis\replay\2026-08-16 13-19-12.yrpX" `
      --scriptdir "D:\ProjectIgnis\replay2video\deps\compat_2026-08" `
      --scriptdir ..\deps\scripts_2026-04-13\script `
      --scriptdir "D:\ProjectIgnis\repositories\delta-bagooska\script" `
      --scriptdir "D:\ProjectIgnis\repositories\delta-puppet\script" --operators
  # 37 activations, 0 NON APPARIEE, 18/18 zone, 10/10 ressource.
  ```
  **Toute modification de l'extraction se re-juge là.** Un harnais qu'on cesse
  de passer est un harnais qui ment.
- **Le mode déterministe est l'instrument d'attribution** :
  `--threads 1 --max-rollouts N --max-nodes N --solve-ms 900000` (le temps ne
  doit JAMAIS mordre). Contrôle : 2 lignes de diff sur 397. Les mesures de
  performance restent à seize workers.
- **Sur l'étalon B : N runs par bras, lecture en PROPORTION.** Le juge y est
  **bimodal** et la session 19 l'a re-mesuré : bras nu, `>=1` dans **5 runs sur
  10**, `>=2` dans **3 sur 10**, et **aucun** des vingt runs n'écrit de solution
  à 60 s. Le compteur de résolutions est le seul juge qui parle.
- **Les juges les plus forts du dossier coûtent ZÉRO budget**, et il faut y
  penser avant de lancer vingt minutes de mur : le verdict du harnais et la
  décomposition imprimée sortent d'un rejeu de 0,2 s, sans un seul tirage. Le
  chantier 1 a été tranché comme cela.
- **`--solve-ms 60000` sur l'étalon B est un pari jamais balayé.** La bimodalité
  est dans la GRAINE, pas dans la durée : si le mode se décide tôt,
  `--max-rollouts` rendrait le même signal en trois fois moins de temps, donc
  **trente runs pour le prix de dix** — plus de puissance statistique pour moins
  de mur. Les vingt logs `s19p_*` sont là pour le vérifier sans rien relancer.
- **UN DRAPEAU N'EST JAMAIS UN CORRECTIF.** Si aucun utilisateur ne voudrait le
  comportement d'avant, c'est le défaut. Un mécanisme est un drapeau *le temps de
  le mesurer*, puis devient le défaut.
- **Un mécanisme doit imprimer sa VIE** (un compteur non nul quand il agit)
  avant qu'on mesure son effet.
- Une mesure en cours **verrouille le binaire** (`LNK1104`) : c'est voulu.
- Logs PS en **UTF-16** : `Select-String`, jamais `grep`. Runs séquentiels, un
  `--outdir` par run.
- **CODES et jamais NOMS** : `8379983` Gold Leo · `3027001` Fake Trap ·
  `54701958` Liger · `24550676` Leo · `97165977` Panther (absent) ·
  `88753594` Sabre · `81196066` Perfume · `35618217` Kaleido Chick ·
  `2344618` Masquerade · `47705572` Wolf · `90590304` Bagooska ·
  `24094653` Polymerization · `87931906` Lunalight Fusion.
- Les scripts se lisent dans `D:\ProjectIgnis\script\official\c<code>.lua` — et
  `--operators` les lit **pour toi**, avec leurs préconditions.
- Bancs : `tools/s19_chantier1.ps1` (juge STRUCTUREL du graphe),
  `tools/s19_promotion.ps1` (étalon B en proportion), `tools/s18_mur.ps1`,
  `s18_determinisme.ps1`, `s18_axe1.ps1`, `s18_axe9.ps1`, `s18_yesno.ps1`.

## CE QU'IL NE FAUT PAS REFAIRE

- **Supposer la convention `aux.Stringid`** : elle est `code << 20 | n` ici, et
  elle se **lit** dans `utility.lua`. La supposer a rendu « 0 appariée sur 37 »,
  ce qui se lit comme un échec d'extraction alors que c'est un échec
  d'hypothèse.
- **Juger une précondition sur un opérateur non déterminé** : le core fabrique
  les descriptions 221 et 0, et Gold Leo a trois déclencheurs à compteurs
  distincts. On COMPTE l'ambiguïté, on ne tranche pas.
- **Croire que le script d'une carte déclare tous ses opérateurs** : la pose
  d'une échelle Pendule et l'activation d'une Polymérisation vivent dans
  `proc_*.lua`, à un et deux niveaux d'indirection.
- **Chercher le « couplage fantôme » de `--card-on-select`** : il n'existe pas
  (§9.25 (a)), et le drapeau a été supprimé — l'identité est inconditionnelle.
- **Croire que `--threads 1` suffit au déterminisme** : c'est l'UNITÉ du budget,
  et c'est corrigé.
- **Re-mesurer §9.24 (o)** : un run par bras sur l'étalon B ne rend rien.
- **Croire au ×27 de `--assign-bias`** (§9.24 (n)) : ce bras portait `--assign`
  en plus ; une fois réellement actif, le mécanisme **divise l'arité 3 par
  deux**.
- **Croire à la « discontinuité du matériau NOMMÉ »** (§9.24) : elle n'existe
  pas. L'exigence est `IsCode(24550676)` sur un **matériau**, et
  `EFFECT_ADD_CODE` la satisfait.
- **Le décodeur binaire** : audité champ par champ contre `playerop.cpp`,
  **19 messages sur 19 justes** (§9.25 (c)) — et le décodeur d'activations de la
  s19 suit exactement les mêmes dispositions.
- Les réfutations de `docs/drapeaux.md` §A — dix sont hors du code, et
  `--backward` en sort par le chantier 1.
- « `--max-subsets` explique les trous de couverture » : mesuré, faux.
- « le changement de phase gaspille les tirages » : mesuré, faux, drapeau retiré.
- « la ligne est hors de l'espace d'actions » : mesuré, faux (couverture **100 %
  au board**).

## LIVRABLE ATTENDU

1. **UN SEUL POINT DE CÂBLAGE** (chantier D). C'est mécanique, c'est falsifiable,
   et cela vaut plus qu'un mécanisme de plus : trois `SearchConfig` construits à
   trois endroits, dont deux oublient des champs, ont fait mesurer du vide
   pendant trois sessions.
2. **Le verdict de `--op-bias`** : `w` balayé, puis le goulot de Kaleido Chick
   (0,14 % → ?), puis l'étalon B en proportion. Sa vie est déjà lue.
3. **`--elide-forced` jugé là où il agit enfin.**
4. **La CONSOMMATION dans le graphe**, si et seulement si (2) rend un signal.
5. La dette : `--prior` retiré, `docs/drapeaux.md` §A corrigé pour `--backward`,
   les jamais-jugés entamés.
6. Ce prompt régénéré.

**Le harnais est la porte du modèle. Le passer coûte un run ; ne pas le passer a
coûté trois sessions.**
