Tu reprends `combosolver`, un solveur de combo EDOPro écrit en C++ qui tourne
sur sa propre copie d'`ocgcore`. Dépôt git autonome, racine
`d:\ProjectIgnis\replay2video\combosolver`. Lis d'abord `README.md` puis
`docs/combo-solver-design.md` — les §9.1-9.11 documentent cinq sessions,
chaque choix adossé à une mesure, impasses comprises. Ne redécouvre rien de ce
qui y est chiffré.

**LA MISSION (inchangée) : battre 19 brûlées sur le replay de référence
`synchron handrip 2` — même deck, même main, même board, discipline complète,
coût lexicographique (brûlées, puis actions, puis décisions).** L'état
d'avancement de la session 5 (§9.11) :

- **LIVRÉ : 19/55/261** (`sF_final/solution_00_b19_a55.yrp`), strictement
  meilleure que la référence (19/56/273) — écrite, vérifiée, jugée depuis
  zéro sous tous les drapeaux (garde 33/33, Omega@terrain 2/2, Trishula 1/1,
  0 activation interdite). Le corpus historique (19/56/272) est dépassé.
- **19 brûlées TIENT** : espace épuisé à k≤4 déviations de la référence
  (959 k états, bornes relâchées 64 act/321 déc) ; k=5 incomplet à 2,33 M ;
  ~3,5 M tirages pleine ligne + ~6 M enracinés sur trois graines
  (888/999/777) et quatre budgets (600-1800 s), borne B&B armée — pas UNE
  ligne à 18, toutes tombent sur 19 exactement.
- La machinerie d'optimisation EXISTE et marche : `--optimize` (§9.11 —
  score de but NRPA lexicographique, anytime avec remplacement du pire +
  dedup, poursuite APRÈS le but, borne B&B brûlées `--burn-slack`/
  `--burn-limit`, archive par coût, racines = solutions les moins chères,
  LDS à bornes relâchées qui ÉPUISE ses passes). L'optimiseur effectif est
  l'**escalade** : chaque run s'enracine (`--approach`) sur la meilleure
  ligne du précédent — 272 → 263 → 261/55a en trois runs — puis a convergé
  (graine 777, 4 racines convertissent, coût inchangé).
- **Le TEST ADVERSE existe (`--fire`, demande du joueur) : Nibiru joué POUR
  DE VRAI.** La carte est ajoutée à la main adverse et ACTIVÉE à chaque
  fenêtre légale (37 sur la référence, une par essai) ; la recherche referme
  depuis l'état post-injection, but double (board complet, ou board sans
  `--fire-spare "Junk Signal"`). Verdict à 300 s/fenêtre (graine 999) :
  **16/37 converties — 6 board COMPLET (19 brûlées/58-59 actions, Crystal
  Wing contre gratuitement, déc. 152-212) et 10 sans Junk Signal (20/58-62,
  voie Zalen+JS)**. Frontière : déc. ≥ ~131 convertit presque partout ; les
  tirs précoces (déc. 55-128) touchent 8/8 sans refermer — indéterminés,
  pas réfutés. Replays écrits/vérifiés/jugés (`sM_fire5/`, `sN_fire_deep/`,
  rejugeables avec `--opp-hand "27204311"`). Pour les fenêtres précoces :
  ensemencer avec les refermetures en `--approach`.
- Les questions `test 3`/`test 4` restent en PAUSE (§9.10, ne pas reprendre
  sans demande du joueur).

## Les chantiers, par rendement attendu

1. **La preuve pour 18 : relaxation arithmétique SMT/ILP des ressources**
   (déjà cadrée §9.10 avec le joueur : conservation des corps, tuners/
   niveaux, copies, arithmétique du handrip — le jeu complet n'est pas
   encodable, le moteur reste la seule spécification, mais l'UNSAT d'une
   RELAXATION est une preuve d'absence valide). C'est la seule voie qui
   TRANCHE : 18 existe ou 19 est optimal. Commencer par l'inventaire des
   contraintes comptables du board cible (6 monstres dont 5 synchros, les
   matériaux finissent au cimetière, Omega banni par son propre rip…).
2. **Pousser l'escalade au-delà de son rayon** : reculs d'approche > 150
   (le point de conversion le plus profond mesuré est recul 110), budgets
   d'un autre ordre sur la phase A2, plusieurs approches STRUCTURELLEMENT
   différentes en même temps (les 49 lignes de sG sont des variantes de la
   même fin — chercher des lignes 8/8 muettes DISTINCTES comme racines).
3. **Épuiser k=5** (LDS-optimize) : 2,33 M états en 550 s, incomplet —
   un run dédié (~30-40 min, `--tt-mb` plus grand) fermerait le rayon 5.
4. **Petits chantiers code** : afficher `burn_cuts`/`goal_hits` (les stats
   existent, aucun printf ne les sert) ; partager la borne brûlées entre
   workers (atomique global — chaque Search resserre la sienne aujourd'hui) ;
   A/B isolé de `--burn-slack` (défaut 6, marge mesurée 4, jamais stressé).
5. **Génériques toujours ouverts** (§9.10) : LTS à frontière partagée,
   LuaJIT côté core (2-10× plausible, risqué), adaptation lente niveau 1.

## L'état mesuré, à ne pas re-dériver

| | |
|---|---|
| Référence | 19/56/273 ; garde 33/33 ; pic de brûlées EN COURS de ligne 23 (marge de récupération 4 — calibre `--burn-slack`) |
| **Meilleure ligne connue (session 5)** | **19/55/261** — `sF_final/solution_00_b19_a55.yrp` (jugée : 261/261, 0 retry, garde 33/33, rips 2/2+1/1) ; variantes 49× dans `sG_iter/`, 263 dans `sD_corpus/` |
| Résistance de 19 brûlées | LDS épuisée k≤4 (959 k états à k=4, 253 s) ; k=5 incomplet (2,33 M états, 550 s) ; 3 graines, 4 budgets, 0 ligne à 18 |
| Verrou d'échantillonnage | 1 tirage pleine ligne sur ~800 k fait les 3 résolutions ; la conversion se joue aux reculs 60-110 des approches (crête-rip 8/8, 4 racines sur 4 en sG) |
| Racines rippées-tôt | STÉRILES (3/8 avec 3 rips, ~700 k tirages) — rips en fin de ligne = structurel sur ce deck |
| Ancienne « résistance à 8-12 déviations » | INVALIDÉE comme preuve : les passes s'arrêtaient à 16 variantes de coût égal (330 états/0,2 s) — seuls les épuisements sous `--optimize` comptent |
| Le même board, autre deck | 13/45/208 (test 4, sans discipline) — la marge venait de là ; sous discipline même-deck elle ne s'est PAS matérialisée en tier 1 |
| Rejetés sur mesure (sessions 1-4) | GNRPA-LR R=2 ; gloutons >1/8 ; événements courants dans resolve/summon-min ; Rollout-IW sans arbre ; allocations hôte |
| Reproductibilité | jamais bit-à-bit à graine fixée — écarts francs ou répétitions ; A/B intra-run de préférence |
| `test 3`/`test 4` (EN PAUSE) | état complet §9.10 |

## Commandes

```powershell
# construire
& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe" `
    build\combosolver.sln /p:Configuration=Release /p:Platform=x64 /m /nologo /v:quiet

# santé (0 écart retrouve la référence, couverture 290/290, 0 fusion, A/B nouveauté)
.\bin\Release\combosolver.exe "D:\ProjectIgnis\replay\synchron handrip 2.yrpX" `
    --scriptdir ..\deps\scripts_2026-04-13\script --solve --solve-ms 60000 `
    --outdir sX_sante --no-chain Zalen --no-chain "Crystal Wing"
# NB : --outdir dédié TOUJOURS — le défaut « solutions/ » écraserait le corpus.

# L'ESCALADE (l'optimiseur effectif de la session 5) — enraciner sur la
# meilleure ligne connue, budget au finisseur, borne armée :
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

# LDS-optimize (résistance locale, épuisements réels) : la commande santé
# + --optimize + les drapeaux de discipline, --solve-ms selon le k visé
# (k=4 a coûté 253 s ; k=5 dépasse 550 s).

# mode JUGE : mêmes drapeaux sans --solve, sur n'importe quel replay produit.

# TEST ADVERSE : Nibiru joué pour de vrai à chaque fenêtre légale.
.\bin\Release\combosolver.exe "D:\ProjectIgnis\replay\synchron handrip 2.yrpX" `
    --scriptdir ..\deps\scripts_2026-04-13\script `
    --fire "27204311" --fire-spare "Junk Signal" --fire-ms 300000 --seed 999 `
    --outdir sX_fire `
    --guard "5:Crystal Wing|Zalen@terrain+Junk Signal@main" --guard-off "mainadv<=2" `
    --no-activate "Duel Evolution - Assault Zone" `
    --no-chain Zalen --no-chain "Crystal Wing" `
    --resolve "PSY-Framelord Omega@terrain:2" `
    --resolve "Trishula, Dragon of the Ice Barrier@terrain"
# (la garde sert au rapport ; la continuation post-injection tourne SANS garde
# — menace dépensée — et SANS no_chain — chaîner sur la menace est le rôle des
# gardes, piège 37. Les replays produits se rejugent avec --opp-hand.)
```

Drapeaux d'optimisation : `--optimize` (anytime + score lexicographique +
poursuite après but + archive par coût + racines-solutions), `--burn-slack`
(marge B&B, défaut 6, marge mesurée sur la référence : 4), `--burn-limit`
(ensemencer la borne, typiquement 19). Le reste inchangé : `--finisher levin`,
`--archive-k`, `--approach` (répétable), `--finisher-min`, `--levin-h`,
`--resolve-weight` 250, `--no-chain`, `--scriptdir` obligatoire (218 retries
silencieux sans lui).

## Les pièges qui ont coûté cher — ne pas les redécouvrir

1-31 : sessions 1-4 (voir la liste complète dans l'ancien prompt au besoin —
les plus mordants : arène `Pop()` jamais `Discard()` (22) ; `--resolve`/
`--summon-min` = événements RARES uniquement, deux effondrements mesurés
(28, 31) ; une approche se rejoue sur le duel de SON en-tête (21) ; jamais
bit-à-bit à graine fixée (24) ; équivalence de but sans position mais avec
la face (26) ; ne pas élaguer sur « carte brûlée » (27) ; `Choice::card`
couvre les fenêtres de chaîne (29)).

32. **Le défaut `--outdir` est `solutions/` — le corpus.** Toujours un
    `--outdir` dédié par run, sinon les 16 fichiers écrasent le répertoire
    de racines.
33. **Un « résiste à N déviations » sans `--optimize` est un arrêt à 16
    variantes, pas une preuve.** Seules les passes marquées ÉPUISÉ sous
    `--optimize` comptent (et un run court tronque sans le dire — le run
    900 s fait foi contre le run 60 s, mêmes k).
34. **Les brûlées ne sont pas monotones** (pic 23 → final 19 sur la
    référence) : toute borne sur les brûlées porte une MARGE (`--burn-slack`,
    défaut 6 > marge mesurée 4). Une borne sans marge couperait la référence.
35. **La poursuite d'après-but est active sous `--optimize`** : le board
    atteint n'est plus terminal (tirages ET DFS). Les 5 « solutions » à k=0
    sont les ré-atteintes le long de la référence — normales, pas un bug.
36. **Une réponse posée non traitée est ÉCRASÉE par le SetResponse suivant.**
    La boucle de rejeu d'un préfixe sort avec la dernière réponse pendante
    (la convention d'entrée du finisseur) ; injecter/poser une autre réponse
    sans avoir traité la première la remplace en silence — l'état part
    décalé d'une réponse et le chemin assemblé ne rejoue pas (payé 3 runs
    sur `--fire` : 16/16 MSG_RETRY). Avancer au prompt AVANT d'injecter ;
    l'auto-contrôle du worker (rejeu du chemin assemblé + indice de
    divergence) est le détecteur.
37. **`no_chain` ne s'applique pas à la continuation post-injection de
    `--fire`** : la règle « Zalen/CW ne chaînent jamais » supposait le
    solitaire ; chaîner sur la menace RÉELLE est leur rôle (mesuré : avec le
    filtre hérité, zéro conversion board complet).

## Discipline de vérification — non négociable

0 retry / couverture 290/290 / 0 fusion ; **à 0 écart la référence est
retrouvée** ; A/B nouveauté automatique ; tout replay écrit rejoué depuis
zéro et jugé avec les MÊMES drapeaux ; **toute solution « moins chère » se
vérifie sur les trois coûts ET la discipline avant d'être annoncée** ; A/B
intra-run de préférence ; un A/B perdant se documente et se désactive par
défaut.

## Cas de test

- LE cas : `D:\ProjectIgnis\replay\synchron handrip 2.yrpX` (290/290, 0
  fusion ; référence 19/56/273 ; MEILLEURE LIGNE CONNUE 19/55/261)
- Répertoire : `solutions/` (corpus historique 19/56/272 — NE PAS écraser),
  `sD_corpus/` (263), `sF_final/` (261/55a — LA racine d'escalade),
  `sG_iter/` (49 variantes de 261)
- Refermetures anti-Nibiru : `sM_fire5/`, `sN_fire_deep/` (b19_a58/59 board
  complet, b20_a58-62 sans JS — rejugeables avec `--opp-hand "27204311"`,
  racines `--approach` pour les fenêtres précoces) ; `sP_bake300/`
  (`--fire-bake` : en-tête CUIT, VISIONNABLES dans EDOPro sans drapeau —
  Nibiru dans le deck adverse servi en main par le pseudo-mélange ; 29
  fenêtres seulement, le handrip peut ripper Nibiru lui-même) ;
  `sT_zalen3/` (séquence Nibiru→JS→Zalen→Omega mise en scène — mais Nibiru
  y est chaîné sur Junk Speeder : « frauduleux » selon le joueur, la vraie
  menace DÉMARRE une chaîne) ; `sY_zalen5/` (LA conversion authentique :
  fenêtre OUVERTE déc. 173, Nibiru ouvreur contré par Junk Signal, jugée
  286/286 — via `--fire-open`, muselières `--fire-no-chain` CW+Dis Pater,
  3 `--fire-spare`). Acquis §9.11 : 4 familles de contres par coût
  croissant (CW, Dis Pater, JS, Omega/AZ), Zalen ne chaîne QUE par-dessus
  JS, `--resolve Zalen` seul se fait contourner, le contre JS sauve les
  fenêtres précoces (7/8) mais les rips depuis déc. 58 restent le mur ;
  levier suivant : écrire la MEILLEURE APPROCHE par fenêtre pour visionner
  les contres non refermés.
- Logs session 5 : `sA_selfstart` → `sN_fire_deep` (.log + dossiers),
  commandes au §9.11 ; logs session 4 : `mA_*` → `mO_*`
- En pause : `test 4.yrpX`, `test 3.ydk` + leurs approches (§9.10)
- `--workdir` par défaut `D:\ProjectIgnis` (ne jamais y écrire)

## Bibliographie (vérifiée sur arXiv)

| Levier | Papier | arXiv |
|---|---|---|
| Coût lexicographique NRPA (IMPLÉMENTÉ session 5) | Montparnasse / MOGNRPALR | 2505.02110, 2606.07562 |
| Multi-objectif (si Pareto redevient utile) | Pareto-NRPA | 2507.19109 |
| LTS/PHS* (implémentés) | Policy-Guided Heuristic Search | 2103.11505 |
| Recul principled | √LTS | 2412.05196 |
| Archive d'états (fait, re-scorée par coût) | Go-Explore | 2004.12919 |
| Politique CPU sans réseau | LTS with Context Models | 2305.16945 |
| Stabilité NRPA | Stabilized NRPA | 2101.03563 |
| Rejeté sur mesure | GNRPA-LR | 2401.10420 |
| Théorie YGO | Deciding winning strategies is hard | 2603.02863 |

Définition de « terminé » pour la session 6 : (a) la question « 18 brûlées
existe-t-il ? » a avancé d'un cran PROUVABLE — relaxation SMT posée (même
partielle : quelles contraintes comptables, quel solveur, premier UNSAT/SAT
sur un sous-problème), OU un rayon d'épuisement étendu (k=5 fermé, reculs
> 150 fouillés) ; (b) si une ligne < 19 tombe : vérifiée sur les trois coûts
ET la discipline, jugée, livrée ; (c) §9.12 documenté, ce prompt régénéré.
