Tu reprends `combosolver`, un solveur de combo EDOPro écrit en C++ qui tourne
sur sa propre copie d'`ocgcore`. Dépôt git autonome, racine
`d:\ProjectIgnis\replay2video\combosolver`. Lis d'abord `README.md` puis
`docs/combo-solver-design.md` — les §9.1-9.11 documentent cinq sessions,
chaque choix adossé à une mesure, impasses comprises. Ne redécouvre rien de ce
qui y est chiffré.

**LA MISSION DE CETTE SESSION : améliorer les PERFORMANCES du solveur** —
d'abord le RENDEMENT de recherche (ce qu'on trouve à débit égal), ensuite le
DÉBIT brut (états/s). Leçon centrale des cinq sessions, à ne pas oublier : le
rendement a toujours payé plus que la vitesse (§9.9 — tout le dégraissage
hôte n'a RIEN changé, ~90 k états/s avant comme après ; ce sont la mémoire de
politique, l'élagage demandé par le joueur et les racines qui ont fait les
percées). Les améliorations restent GÉNÉRIQUES ; chaque gain se mesure en A/B
(même graine, même budget) sur les étalons ci-dessous. La mission de FOND
(battre 19 brûlées sur `synchron handrip 2`, coût lexicographique) reste
l'étalon n°1 ; les questions `test 3`/`test 4` restent en PAUSE (§9.10).

## L'état acquis (session 5, §9.11) — ne pas re-dériver

- **Meilleure ligne livrée : 19/55/261** (`sF_final/solution_00_b19_a55.yrp`,
  vérifiée, jugée) contre la référence 19/56/273. Trouvée par l'ESCALADE :
  chaque run `--optimize` s'enracine (`--approach`) sur la meilleure ligne du
  précédent (272 → 263 → 261/55a), puis convergence (graine 777).
- **19 brûlées résiste** : LDS-optimize épuisée à k≤4 (959 k états, bornes
  relâchées 64 act/321 déc) ; k=5 INCOMPLET à 2,33 M états / 550 s ; ~3,5 M
  tirages pleine ligne + ~6 M enracinés, 3 graines, borne B&B armée — pas une
  ligne à 18.
- **`--optimize`** : score de but NRPA lexicographique, anytime (poursuite
  après solution ET après but), borne B&B brûlées (`--burn-slack` défaut 6,
  marge de récupération mesurée 4 sur la référence — pic 23 pour 19 au
  board), archive par coût, racines = solutions les moins chères.
- **`--fire`** (test adverse) : menace jouée pour de vrai, `--fire-open`
  (ouverture de chaîne = la vraie menace, verdict du joueur), `--fire-bake`
  (replays visionnables EDOPro), `--fire-no-chain`/`--fire-spare` (mise en
  scène, buts par sous-ensembles), diagnostics (réponses à la menace, cartes
  manquantes à la crête, trace des chaînes sous `--verbose`, auto-contrôle).
  Cartographie : 4 familles de contres par coût croissant (CW, Dis Pater,
  Junk Signal, Omega/AZ) ; Zalen ne chaîne QUE par-dessus JS. **Le mur des
  fenêtres précoces : re-dériver les 3 rips depuis la décision 58 — un
  problème de PRIOR, pas de vitesse** (1 tirage pleine ligne sur ~800 k fait
  les 3 résolutions).

## Les chantiers, par rendement attendu

1. **Instrumenter la borne B&B (trivial, à faire en premier).** `burn_cuts`
   et `goal_hits` existent dans `SearchStats`, AUCUN printf ne les sert — les
   ajouter aux rapports de phase (tirages, finisseur, LDS). Sans eux la borne
   tourne en aveugle et le chantier 3 n'est pas mesurable.
2. **Partager la borne brûlées ENTRE workers.** Aujourd'hui chaque `Search`
   resserre sa borne locale (`best_burned_seen` + `burn_cut`) : un worker qui
   trouve 19 ne coupe rien chez les quinze autres. Un
   `std::atomic<uint32_t>*` dans `SearchConfig` (nul = comportement actuel),
   lu à chaque resserrement, publié à chaque amélioration. A/B : run
   d'escalade 600 s ± partage, compter `burn_cuts` et la crête.
3. **A/B isolé de `--burn-slack`** (jamais stressé) : 4 / 6 / 8 / 255 sur le
   même run d'escalade — la marge mesurée de la référence est 4, le défaut 6.
4. **Partage périodique de la POLITIQUE entre workers** (le non-fait le plus
   ancien, session 3 — seule la meilleure SÉQUENCE est partagée). Fusion
   périodique des poids (moyenne des logits, le mécanisme de fusion existe en
   fin de phase) toutes les N itérations de niveau supérieur, ou via un
   snapshot protégé par le mutex de `NrpaShared`. Cohérent avec tout ce qui a
   marché (la mémoire de politique bat la vitesse brute).
5. **LE gros morceau : prior par rejeu de solutions (arXiv:2401.10431), visé
   sur les RIPS.** La politique ne sait pas ripper (1/800 k) ; or le corpus
   (`solutions/`, `sF_final/`, la référence elle-même) CONTIENT les séquences
   de rip complètes. Rejouer les solutions du corpus dans `RunNrpa` au
   démarrage (adaptation vers leurs séquences, ou poids initiaux par
   `plan_key` relevés sur elles) donnerait à chaque worker une politique qui
   sait DÉJÀ ripper. Débloque : les fenêtres `--fire` précoces, et peut-être
   18 brûlées par restructuration profonde. Attention piège 21 : un rejeu
   d'approche se fait sur le duel de SON en-tête — pour le PRIOR on ne
   rejoue pas, on RELÈVE les plan_keys (LiftPlan/LiftRefLine sur leur duel).
6. **Refermetures `--fire` en racines croisées** : les lignes converties
   d'une fenêtre servent d'`--approach` aux fenêtres voisines (duels cuits
   identiques — même en-tête). L'équivalent de l'escalade, par fenêtre.
7. **LTS à frontière partagée** : le finisseur travaille une racine par
   worker (~8 ms/expansion) ; les nœuds voyagent par leur chemin de réponses
   — 16 cœurs sur UNE racine prometteuse. C'est du débit là où les
   conversions se jouent.
8. **Miroir des zones depuis `MSG_MOVE`** (profil D'ABORD) : chaque décision
   paie des requêtes de zones au core (`ComputeBoardKeyInto` ×1-2,
   `Heuristic` +1 Count, borne B&B +2 Counts). Un miroir hôte tenu par
   messages les supprimerait. Gain plausible 10-30 % — à confirmer au profil
   avant d'écrire une ligne.
9. **LuaJIT côté core — le seul 2-10× plausible, chantier de fond ISOLÉ.**
   La charge est dominée par les scripts Lua. Risque réel :
   `luaconf-customize.h` porte l'allocateur d'arène et le hachage
   déterministe (`fetch_solver_deps.ps1` applique les patchs). Procédure :
   branche dédiée, porter luaconf, puis TOUTE la batterie d'invariants
   (0 retry, 290/290, 0 fusion, instantanés IDENTIQUE, stress-test,
   référence retrouvée à 0 écart) avant le moindre run de mesure. Un échec
   d'invariant = on documente et on recule.

Deux murs qui ne céderont PAS au débit seul, à garder en tête (pas
prioritaires cette session, sauf demande du joueur) : **18 brûlées** (la voie
qui tranche est la relaxation SMT/ILP des ressources, §9.10 ; côté recherche
fermer k=5 — 30-40 min avec `--tt-mb` plus grand — et les reculs > 150) ;
**les rips précoces** (chantier 5 ci-dessus).

## Les étalons de mesure (A/B, même graine, même budget)

```powershell
# construire
& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe" `
    build\combosolver.sln /p:Configuration=Release /p:Platform=x64 /m /nologo /v:quiet

# santé — LA porte de tout changement (0 écart retrouve la référence,
# couverture 290/290, 0 fusion, instantanés, A/B nouveauté)
.\bin\Release\combosolver.exe "D:\ProjectIgnis\replay\synchron handrip 2.yrpX" `
    --scriptdir ..\deps\scripts_2026-04-13\script --solve --solve-ms 60000 `
    --outdir sX_sante --no-chain Zalen --no-chain "Crystal Wing"
# NB : --outdir dédié TOUJOURS — le défaut « solutions/ » écraserait le corpus.

# Étalon 1 — l'escalade même-deck (rendement de recherche) :
.\bin\Release\combosolver.exe "D:\ProjectIgnis\replay\synchron handrip 2.yrpX" `
    --scriptdir ..\deps\scripts_2026-04-13\script `
    --start "D:\ProjectIgnis\replay\synchron handrip 2.yrpX" `
    --solve-ms 600000 --seed 777 --finisher levin --optimize `
    --finisher-min 420000 --burn-limit 19 --archive-k 24 `
    --approach "sF_final/solution_00_b19_a55.yrp" `
    --outdir sX_iter `
    --guard "5:Crystal Wing|Zalen@terrain+Junk Signal@main" --guard-off "mainadv<=2" `
    --no-activate "Duel Evolution - Assault Zone" `
    --no-chain Zalen --no-chain "Crystal Wing" `
    --resolve "PSY-Framelord Omega@terrain:2" `
    --resolve "Trishula, Dragon of the Ice Barrier@terrain"
# Métriques : crête, lignes/coûts, tirages rippants (>=1/>=2/>=3), burn_cuts.

# Étalon 2 — les fenêtres --fire précoces (le mur des rips ; chantiers 5-6) :
.\bin\Release\combosolver.exe "D:\ProjectIgnis\replay\synchron handrip 2.yrpX" `
    --scriptdir ..\deps\scripts_2026-04-13\script `
    --fire "27204311" --fire-open --fire-bake --fire-ms 300000 `
    --fire-spare "Junk Signal" --fire-spare "Crystal Wing" --fire-spare "63436931" `
    --seed 888 --outdir sX_fire `
    --guard "5:Crystal Wing|Zalen@terrain+Junk Signal@main" --guard-off "mainadv<=2" `
    --no-activate "Duel Evolution - Assault Zone" `
    --no-chain Zalen --no-chain "Crystal Wing" `
    --resolve "PSY-Framelord Omega@terrain:2" `
    --resolve "Trishula, Dragon of the Ice Barrier@terrain"
# Aujourd'hui : fenêtres déc. 55-146 non converties (crêtes 7-8/8, rips
# incomplets). Toute conversion précoce nouvelle = gain de rendement prouvé.

# Étalon 3 — débit brut (chantiers 8-9) : états/s de la phase tirages de
# l'étalon 1 (~90 k états/s aujourd'hui), ms/expansion du finisseur (~8 ms).

# mode JUGE : mêmes drapeaux sans --solve, sur n'importe quel replay produit.
```

## Les pièges qui ont coûté cher — ne pas les redécouvrir

1-31 : sessions 1-4 (liste dans les prompts précédents / §9 ; les plus
mordants : `Pop()` jamais `Discard()` (22) ; `--resolve`/`--summon-min` =
événements RARES uniquement (28, 31) ; une approche se rejoue sur le duel de
SON en-tête (21) ; jamais bit-à-bit à graine fixée (24) ; équivalence de but
sans position mais avec la face (26) ; pas d'élagage sur « carte brûlée »
(27) ; `Choice::card` couvre les fenêtres de chaîne (29)).

32. **`--outdir` par défaut = `solutions/` (le corpus)** — un `--outdir`
    dédié par run, toujours.
33. **Un « résiste à N déviations » sans `--optimize` est un arrêt à 16
    variantes, pas une preuve** — seules les passes ÉPUISÉ comptent, et un
    budget court tronque sans le dire.
34. **Les brûlées ne sont pas monotones** (pic 23 → 19 sur la référence) :
    toute borne brûlées porte une marge.
35. **La poursuite d'après-but est active sous `--optimize`** : les
    « solutions » à k=0 sont des ré-atteintes, pas un bug.
36. **Une réponse posée non traitée est ÉCRASÉE par le SetResponse suivant**
    — avancer au prompt avant d'injecter ; l'auto-contrôle du worker est le
    détecteur.
37. **`no_chain` global ≠ continuation `--fire`** : chaîner sur la menace
    réelle est le rôle des gardes ; `--fire-no-chain` est le drapeau de mise
    en scène.
38. **Une contrainte `--resolve` ne dit pas OÙ résoudre** : le solveur la
    contourne en résolvant AILLEURS (mesuré : Zalen activé chaîne 30 pendant
    que CW nège chaîne 15). Pour imposer un rôle, museler les alternatives.

## Discipline de vérification — non négociable

La santé passe AVANT et APRÈS chaque changement de moteur ; 0 retry /
290/290 / 0 fusion ; **à 0 écart la référence est retrouvée** ; A/B nouveauté
automatique ; tout replay écrit rejoué depuis zéro et jugé avec les MÊMES
drapeaux ; toute amélioration de coût vérifiée sur les trois coûts ET la
discipline avant annonce ; A/B intra-run de préférence ; un A/B perdant se
documente et se désactive par défaut. Pour LuaJIT (chantier 9) : branche
dédiée, batterie d'invariants complète avant toute mesure.

## Cas de test et fichiers

- LE cas : `D:\ProjectIgnis\replay\synchron handrip 2.yrpX` (référence
  19/56/273 ; meilleure ligne connue 19/55/261)
- Racine d'escalade : `sF_final/solution_00_b19_a55.yrp` ; corpus historique
  `solutions/` (NE PAS écraser) ; variantes `sD_corpus/`, `sG_iter/`
- Refermetures `--fire` : `sM_fire5/`, `sN_fire_deep/` (`--opp-hand
  "27204311"` pour juger), `sP_bake300/`, `sT_zalen3/`, `sY_zalen5/`
  (cuits : jugeables sans drapeau, visionnables EDOPro)
- Logs sessions 4-5 : `mA_*`→`mO_*`, `sA_*`→`sY_*` (ignorés par git,
  commandes au §9.10-9.11)
- `--workdir` par défaut `D:\ProjectIgnis` (ne jamais y écrire)

## Bibliographie (vérifiée sur arXiv)

| Levier | Papier | arXiv |
|---|---|---|
| **Prior par rejeu — chantier 5** | Policy Learning from Solved Games | 2401.10431 |
| Coût lexicographique NRPA (implémenté) | Montparnasse / MOGNRPALR | 2505.02110, 2606.07562 |
| LTS/PHS* (implémentés ; frontière partagée = chantier 7) | Policy-Guided Heuristic Search | 2103.11505 |
| Recul principled | √LTS | 2412.05196 |
| Archive d'états (fait) | Go-Explore | 2004.12919 |
| Stabilité NRPA | Stabilized NRPA | 2101.03563 |
| Adaptation lente niveau 1 (option chantier 4) | Montparnasse | 2505.02110 |
| Rejeté sur mesure | GNRPA-LR | 2401.10420 |

Définition de « terminé » pour la session 6 : (a) chantiers 1-3 faits et
mesurés (la borne B&B instrumentée, partagée, calibrée) ; (b) le chantier 5
(prior par rejeu) implémenté et mesuré sur les DEUX étalons — tirages
rippants et fenêtres `--fire` précoces ; (c) tout gain/perte chiffré en A/B,
les perdants désactivés par défaut et documentés ; (d) si LuaJIT est tenté :
verdict d'invariants AVANT toute mesure, sur branche dédiée ; (e) §9.12
documenté, ce prompt régénéré.
