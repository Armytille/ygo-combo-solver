# Session 18 — AUDIT : POURQUOI CE SOLVEUR NE TROUVE RIEN

Tu reprends `combosolver` (racine `d:\ProjectIgnis\replay2video\combosolver`).

**CE N'EST PAS UNE SESSION DE MÉCANISME.** Aucun drapeau de plus. Le livrable est
un **rapport d'audit** : ce qui est faux, ce qui est mort, ce qui est du réglage
déguisé, et ce qui devrait être jeté. Écrire du code n'est autorisé que pour
**instrumenter une hypothèse d'audit** ou **supprimer**.

Lis `README.md`, puis `docs/combo-solver-design.md` **§9.24** (session 17) en
entier, puis §9.23. La bibliographie est faite (§9.22 (h), §9.23 (c), §9.24) : ne
la refais pas.

## LE CONSTAT

Dix-sept sessions. **Zéro solution but-seul écrite sur l'étalon A.** Le solveur
n'a jamais posé un seul Lunalight Liger Dancer en run nu, sur des dizaines de
millions de tirages.

Ce qui marche : le rejeu fidèle, l'arène, la réparation à écarts bornés, la
transplantation partielle (4/6). Ce qui ne marche pas : **trouver**.

La session 17 a produit quatre diagnostics utiles et **zéro conversion** :

| fait mesuré | valeur |
|---|---|
| nœuds développés n'offrant **aucun choix** | **74,6 %** |
| clé de transposition, rapport aux **boards** distincts | **×138 à ×284** |
| tirages où la porte (`Masquerade`) s'ouvre | **21,4 %** |
| tirages où le matériau (`Leo Dancer`) atteint sa zone | **0,035 %** |
| variance inter-run **à graine fixée** | **×6** |

## LA DETTE, CHIFFRÉE

| | |
|---|---|
| drapeaux CLI | **122** |
| lignes (`main.cpp` / `search.cpp` / `search.h`) | 9 386 / 5 273 / 3 285 |
| champs de `SearchConfig` | **81** |
| mécanismes **réfutés** encore présents dans le code | au moins 12 |

**Un solveur qui ne trouve rien avec 122 cadrans n'a pas un problème de
cadran.**

---

## AXE 1 — LE COUPLAGE FANTÔME (le fil le plus chaud, commencer par là)

`--card-on-select` **seul** fait passer l'étalon B de **2 239 résolutions à
ZÉRO** et l'approche écrite de 7/8 à 3/8. Reproductible : trois runs.

Or ce drapeau ne fait qu'une chose — renseigner `Choice::card` sur les prompts de
sélection. Et depuis le correctif de la s17, `hinted` **rejette** ces identités
(`Choice::card_lossy`). Avec `--assign-bias` à zéro, **plus rien ne lit ce
champ** : ce bras devrait être identique au témoin, bit pour bit.

**Il ne l'est pas. Trouve pourquoi.** Tout dépend de cette réponse : si un champ
« inerte » peut détruire un cas, alors aucune mesure du dossier n'est fiable.

*Où chercher* : tous les lecteurs de `Choice::card` (`grep -n "\.card"`), y
compris `bandit_code`, la sonde, `EmitAssignExtremes` ; l'ordre d'application des
options dans `main.cpp` (le bloc `card_on_select` est-il lu par TOUS les
`EnumOptions` du run, dont ceux du relevé de plan et du finisseur ?) ; et
`ChoiceList::Emit()`, où un champ non remis à zéro **fuite d'un prompt à
l'autre** — bug déjà trouvé une fois en s17 (641 → 0).

## AXE 2 — LE DÉCODEUR BINAIRE EST-IL JUSTE ?

`MSG_SELECT_BATTLECMD` était **faux depuis l'origine** : `sequence` est un
`uint8` dans la liste attaquable et un `uint32` dans la liste activable du *même*
message ; le décodeur sautait 11 octets au lieu de 8. Conséquence silencieuse :
toute branche entrant en Battle Phase mourait, et la mort se lisait comme une
impasse ordinaire.

**Un défaut de ce type ne se voit dans aucune mesure de recherche.** Il faut
auditer **champ par champ, message par message**, `enumerate.cpp` et
`prompt.cpp` contre `deps/ocgcore/playerop.cpp` et `processor.cpp`.

*Livrable* : un tableau « message → disposition attendue → disposition décodée →
verdict », pour les 19 types de `PromptName`. Les `ANNOUNCE_*`, `SELECT_COUNTER`
et `SORT_CARD` tombent sur `DefaultResponse` — **combien de fois en pratique, et
quelles branches cela tue-t-il ?** (`forced_default_prompts` existe et est
imprimé.)

## AXE 3 — L'ÉTAT, LA CLÉ, ET CE QU'ELLE DISTINGUE

La clé de transposition prend **138 à 284 valeurs par board distinct**, et le
rapport **croît** avec la profondeur. Attribution s17 : ce n'est ni le
`ProcessorState` (×1,00 aux points stables), ni la charge utile (×1,42), c'est
l'état de jeu lui-même — dont la **colonne** (×37 aux points idle, mais ×1,67
seulement sur l'ensemble des nœuds).

*Questions à trancher* :
- `StateDigest` hache les **deux joueurs**. En solitaire, l'adversaire ne joue
  pas — que coûte-t-il dans la clé, et pourquoi n'a-t-on jamais mesuré ?
- `ProcessorState()` : quelle est sa **taille** et sa **variabilité** ? Il a été
  ajouté par « le patch C1 » pour distinguer deux instants d'une chaîne — cette
  distinction est-elle encore nécessaire maintenant que 74,6 % des prompts sont
  élidables ?
- La transposition stocke `remaining` (budget restant) : combien d'états sont
  **ré-explorés** faute de budget suffisant, et ce mécanisme paie-t-il ?

## AXE 4 — LE SCORE, ET D'OÙ SORTENT SES NOMBRES

```
Heuristic = common×100 + exact×10 + mzone_count×3 + fodder
material  = Heuristic + resolve_weight(250) × ResolveProgress
score     = material×1000 + novel_states
but       = 1e12 − burned×1e9 − actions×1e5 − depth
```

**Aucune de ces constantes n'a de justification écrite reliée à une mesure.**
`common` compte les cartes cibles **présentes** — terme nul sur toute la montée,
et c'est la cause mécanique nommée en §9.23 (h).

*À faire* : pour chaque constante, retrouver la session qui l'a posée et le
raisonnement. Celles qui n'en ont pas sont des paris. Mesurer la **sensibilité**
du solveur à chacune (une décade autour) : celles auxquelles il est insensible
sont du bruit, celles auxquelles il est très sensible sont du **réglage sur nos
deux decks** — et c'est exactement ce que l'opérateur veut voir disparaître.

## AXE 5 — L'ALGORITHME EST-IL LE BON ?

Question de fond, jamais posée dans le dossier.

**NRPA est un algorithme d'OPTIMISATION DE SÉQUENCE** (Morpion Solitaire,
crossword) : il cherche la *meilleure* ligne dans un espace où presque toute
ligne est valide. Notre problème est un problème **d'ATTEIGNABILITÉ** : presque
aucune ligne n'est valide, et toutes les valides se valent.

*Symptômes cohérents avec un mauvais choix d'algorithme, tous déjà mesurés* :
- le score du tirage est un **MAX le long de la ligne** — donc une ligne qui
  approche puis s'écrase bat une ligne qui progresse ;
- la politique converge vers **la Fusion la moins chère** (÷20 par matériau) ;
- l'archive Go-Explore s'épuise en **0 à 13 expansions** par racine ;
- 100 % des tirages finissent par « coupure de tour », c'est-à-dire **par
  épuisement**, pas par un but.

*À trancher* : le finisseur LTS (`RunLevin`), lui, est un algorithme
d'atteignabilité **complet**. A-t-il jamais été jugé **seul**, sans phase NRPA ?
Si oui où, si non pourquoi ? Et l'exhaustif atteint **145 boards distincts** à
budget égal sous `--elide-forced` : que donne-t-il si on lui donne tout le
budget, sans NRPA du tout ?

## AXE 6 — LE FINE-TUNING DÉGUISÉ (exigence explicite de l'opérateur)

> « Il est important de ne pas fine-tuner le solveur pour répondre au cas
> Lunalight, il doit être générique et fonctionner quel que soit le deck. »

Le moteur ne compile aucun nom de carte — c'est vrai et vérifiable. **Ce n'est
pas suffisant** : la s17 l'a prouvé en mesure. `--assign-bias`, dont la liste
sort du graphe de recettes sans aucune carte compilée, gagne ×27 sur Lunalight
et **détruit Synchron** (2 239 → 0).

*À inventorier, sans complaisance* :
- toute constante réglée en regardant un de nos deux étalons ;
- toute liste `--hint` écrite à la main dans `tools/*.ps1` — **combien de
  mesures du dossier en dépendent ?** ;
- `--resolve`, qui est un **indice déguisé** (biais d'office + gradient +
  exigence au but) et qui est présent dans presque toutes les commandes de
  l'étalon B — **quelles conclusions du dossier s'effondrent si on le retire ?** ;
- tout mécanisme validé sur **un seul** étalon.

*Règle à instaurer, et à écrire dans le README* : **aucun mécanisme n'est retenu
sans avoir passé les DEUX étalons.** La s17 montre que le deuxième deck suffit à
réfuter ; le troisième n'était pas nécessaire.

## AXE 7 — LE GRAS

**122 drapeaux.** Pour chacun, trois questions : *(a)* a-t-il été jugé ?
*(b)* quel est son verdict écrit ? *(c)* le code serait-il plus simple sans lui ?

Candidats à la suppression, déjà réfutés ou jamais jugés :
`--mcps` (réfuté deux fois), `--nrpa-lr` (réfuté), `--recipe-w` (réfuté, cause
structurelle), `--assign` / `--assign-bias` (réfuté sur B), `--goal-bias`
(réfuté), `--canonical-digest` (réfuté sur A), `--no-phase-change` (départagé,
négatif), `--options-ctx` (réfuté), `--subsets-ascending` (A/B d'attribution
terminé), `--finisher-options` (**écrit, jamais jugé**), `--canonical-zones`
(**jamais départagé**), `--archive-spread` (départagé, neutre).

**Supprimer un mécanisme réfuté n'est pas une perte : c'est retirer une branche
morte que chaque lecture future devra sinon ré-évaluer.** Le dossier garde la
trace ; le code n'a pas à la porter.

## AXE 8 — LES INSTRUMENTS MENTENT

Trois faux verdicts en une seule session :
1. le contrôle de couverture comparait `Fingerprint` (état exact) au lieu du
   **board** → « trois trous dans l'espace » alors qu'il couvre **100 %** ;
2. la sonde d'offre comptait « présente dans un pool » comme « invocable » →
   verdict **inverse** du vrai ;
3. `offer_steps` était compté après le `continue` de l'élision → dénominateur
   divisé par deux, taux **incomparables entre bras**.

*À faire* : passer en revue **chaque compteur imprimé** et répondre à « que
mesure-t-il exactement, et sous quels drapeaux cesse-t-il d'être valide ? ». Un
compteur dont la définition change avec un drapeau ne peut pas juger ce drapeau.

## AXE 9 — LE NON-DÉTERMINISME

Deux runs de **configuration identique et de même graine** donnent 202 et 33 sur
le même compteur : **facteur 6**. Cause connue et écrite : échanges asynchrones
entre workers (`NrpaShared`, `OnlineOptions`, `SharedTT`).

**C'est un défaut du solveur, pas une propriété de l'instrument** — et il rend
non concluante toute mesure d'un effet inférieur à ×6.

*À trancher* : un mode **strictement déterministe** (un worker, aucun partage)
est-il réalisable ? À quel coût en débit ? Sans lui, aucun A/B fin n'est possible
et toute la discipline de mesure du dossier repose sur du sable.

---

## MÉTHODE

- **La santé est la porte, avant ET après chaque changement** :
  ```powershell
  .\bin\Release\combosolver.exe "D:\ProjectIgnis\replay\synchron handrip 2.yrpX" `
      --scriptdir ..\deps\scripts_2026-04-13\script --solve --solve-ms 60000 `
      --outdir s18_sante --no-chain Zalen --no-chain "Crystal Wing"
  # 273 digests, 210/273, 209 candidates, 16 replays.
  ```
- **Supprimer un drapeau est un changement comme un autre** : santé avant/après.
- Une mesure en cours **verrouille le binaire** (`LNK1104`) : c'est voulu.
- Logs PS en **UTF-16** : `Select-String`, jamais `grep`. Runs séquentiels, un
  `--outdir` par run.
- **CODES et jamais NOMS** : `8379983` Gold Leo · `3027001` Fake Trap ·
  `54701958` Liger · `24550676` Leo · `88753594` Sabre · `81196066` Perfume ·
  `35618217` Kaleido Chick · `2344618` Masquerade · `47705572` Wolf ·
  `90590304` Bagooska.
- **Le premier plan résolu de l'étalon A existe** :
  `D:\ProjectIgnis\replay\2026-08-16 13-19-12.yrpX` (283/283, 0 MSG_RETRY, 2
  Liger). C'est un **corpus et un objet d'étude**, pas une solution de la cible
  (2 Liger sur 3, pas de Bagooska, ancienne main).

## CE QU'IL NE FAUT PAS REFAIRE

- La bibliographie (§9.22 (h), §9.23 (c), §9.24).
- Les réfutations listées à l'axe 7.
- « `--max-subsets` explique les trous de couverture » : mesuré, faux (24 et 256
  identiques).
- « le changement de phase gaspille les tirages » : mesuré, faux
  (`--no-phase-change` laisse les coupures de tour à 100 % ; c'est la fin
  **normale** d'un tirage).
- « la ligne est hors de l'espace d'actions » : mesuré, faux (couverture **100 %
  au board**).

## LIVRABLE ATTENDU

1. **§9.25 — rapport d'audit**, un verdict par axe, chiffré.
2. **La réponse de l'axe 1** — le couplage fantôme identifié, ou la démonstration
   écrite qu'il n'existe pas.
3. **Un tableau des 122 drapeaux** : jugé / réfuté / jamais jugé, avec la
   proposition de suppression.
4. **Une liste des constantes** avec, pour chacune, sa justification ou la
   mention « pari non mesuré ».
5. **Le verdict de l'axe 5** : NRPA est-il le bon algorithme pour un problème
   d'atteignabilité, ou faut-il basculer le budget vers le finisseur complet ?
6. Ce prompt régénéré.

**Ne pas ajouter de mécanisme. Si l'audit désigne un chantier, il sera la mission
de la session 19 — pas de celle-ci.**
