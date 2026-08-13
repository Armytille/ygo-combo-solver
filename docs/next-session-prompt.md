Tu reprends `combosolver`, un solveur de combo EDOPro écrit en C++ qui tourne
sur sa propre copie d'`ocgcore`. Dépôt git autonome, racine
`d:\ProjectIgnis\replay2video\combosolver`. Lis d'abord `README.md` puis
`docs/combo-solver-design.md` — les §9.1-9.12 documentent six sessions,
chaque choix adossé à une mesure, impasses comprises. Ne redécouvre rien de ce
qui y est chiffré.

**LA MISSION DE CETTE SESSION : le RENDEMENT de recherche, par les deux
leviers que la session 6 a laissés debout** — l'escalade (le seul mécanisme
qui a encore progressé : 261 → 259 en un après-midi de runs) et la mémoire de
politique (chantiers 4 et 5bis). Leçon consolidée des six sessions : le
rendement bat la vitesse (§9.9), et depuis la session 6 : **un réglage ne se
calibre qu'après instrumentation** (la borne B&B tournait à vide depuis la
session 5 — 0 coupure à toute marge), et **un A/B ne se tranche que sur les
faits STRUCTURELS** (conversions, coûts écrits, coupures, ensembles de
fenêtres) — à graine fixée, le bruit de run fait varier les compteurs de
tirages de 98 à 9 402 entre bras au mécanisme inerte. La mission de FOND
(battre 19 brûlées, coût lexicographique) reste l'étalon n°1 ; `test 3`/
`test 4` restent en PAUSE (§9.10).

## L'état acquis (session 6, §9.12) — ne pas re-dériver

- **Meilleure ligne livrée : 19/55/259**
  (`sZ6_slack255/solution_00_b19_a55.yrp`, jugée : 0 retry, garde 35/35,
  Omega@terrain 2/2, Trishula 1/1) contre la référence 19/56/273.
  Trajectoire d'escalade 272 → 263 → 261 → 259 — **elle n'a PAS convergé** :
  le run de « convergence » de la session 5 (graine 777) a été battu par la
  même graine à la session 6. Chaque relance enracinée sur la meilleure ligne
  reste le geste le plus rentable du répertoire.
- **La borne B&B brûlées est structurellement inactive sur ce problème**
  (session 6, instrumentée puis mesurée) : 0 coupure en phase tirages à toute
  marge (4-255) sur 7 runs de 600 s — aucun tirage n'atteint le but sur ce
  flux ET aucun état de tirage ne dépasse 23 brûlées au tour 1. Pics mesurés :
  référence 23, ligne a55 23, ligne a56 22 — l'espace gagnant vit sous le pic
  de la référence. Elle ne mord qu'au finisseur à marge 4 (201 coupures, sans
  avantage). Défauts inchangés : `--burn-slack 6`, partage ON
  (`--no-burn-share` = instrument d'A/B, neutre mesuré). CLOS.
- **Le prior par POIDS (`--prior`, arXiv:2401.10431) est NEUTRE — hypothèse
  « déficit de poids par coup » réfutée sur les deux étalons.** Relevé propre
  (28 lignes/5 s/101 coups, 1 étape non identifiée par ligne, piège 21
  respecté : LiftPlan sur le duel de l'en-tête), mais mêmes coûts sur
  l'étalon 1 et ensembles de conversion `--fire` IDENTIQUES (10/15, déc.
  113-173 ; les 5 fenêtres précoces déc. 58-103 murées des deux côtés, mêmes
  manques Crimson Dragon/Crystal Wing). Le verrou des rips précoces est un
  verrou de SÉQUENCE/ressources, pas d'échantillonnage par coup. Le drapeau
  reste opt-in, hors commande recommandée.
- **LuaJIT : FERMÉ sur pièces** (§9.12) — syntaxe 5.3+ dans 1 862/2 503
  scripts + les deux socles, `lua_newstate` custom non supporté en 64-bit
  (l'accroche arène), traces JIT vs Restore(), API C 5.4. Le « 2-10× » de
  fond n'existe pas sous cette forme. Ne pas y revenir.
- **Instrumentation en place** (session 6) : `burn_cuts`/`goal_hits` dans les
  trois rapports (tirages `anytime :`, finisseur, LDS), pic de brûlées au
  rejeu même sans board cible (calibre la marge sur les lignes de solution).
- Sessions 1-5 : `--optimize` (anytime lexicographique), `--fire` complet
  (`--fire-open/-bake/-spare/-no-chain`), 4 familles de contres, Zalen ne
  chaîne QUE par-dessus JS, 19 brûlées résiste (k≤4 ÉPUISÉ, k=5 incomplet à
  2,33 M états).

## Les chantiers, par rendement attendu

1. **L'ESCALADE CONTINUE (prouvée, triviale à lancer).** Enraciner sur la
   ligne 259 (`--approach sZ6_slack255/solution_00_b19_a55.yrp`), plusieurs
   graines (777 a déjà donné ; essayer 888/999/1234), budgets 600-1800 s.
   Chaque run est une chance de descendre encore ; s'arrêter quand 3 graines
   consécutives ne battent plus le meilleur. C'est le SEUL mécanisme qui a
   produit du nouveau en session 6.
2. **Chantier 5bis : le rejeu d'ADAPTATION (la voie restante du prior).**
   Les poids initiaux ne suffisent pas ; l'alternative de 2401.10431 est
   d'ADAPTER la politique vers les séquences du corpus. Sur l'étalon 1 le
   corpus same-deck se REJOUE sur le duel de départ (même en-tête — piège 21
   sans objet ici, le vérifier par l'alignement 0 retry) : rejouer chaque
   ligne en collectant les PolicyStep (choix légaux + choisi à chaque prompt
   multi-choix), puis Adapt() ×N à l'init de chaque worker. C'est l'adaptation
   NRPA standard, pas un mécanisme neuf. A/B étalon 1 (structurel : coûts,
   conversions) et étalon 2 si le rejeu s'aligne sur le duel cuit.
3. **Chantier 6 : racines croisées `--fire`** (jamais essayé) : les lignes
   converties d'une fenêtre servent d'`--approach` aux fenêtres voisines
   (duels cuits identiques — même en-tête). L'équivalent de l'escalade, par
   fenêtre — le levier documenté pour les fenêtres précoces (§9.11).
4. **Chantier 4 : partage périodique de la POLITIQUE entre workers** (le plus
   ancien non-fait, session 3) : fusion périodique des poids (moyenne des
   logits) toutes les N itérations de niveau supérieur, ou snapshot sous le
   mutex de `NrpaShared`. Attention : l'A/B se tranche sur les faits
   structurels UNIQUEMENT (le bruit de graine mesuré en session 6).
5. **Chantier 7 : LTS à frontière partagée** — 16 cœurs sur UNE racine
   prometteuse (~8 ms/expansion aujourd'hui, une racine par worker).
6. **Chantier 8 : miroir des zones depuis `MSG_MOVE` (profil D'ABORD).**
   Chaque décision paie des requêtes de zones (`ComputeBoardKeyInto` ×1-2,
   `Heuristic`, borne +2 Counts — quasi gratuits maintenant que la borne est
   close, mais le miroir vise les requêtes de BoardKey). Gain plausible
   10-30 % — à confirmer au profil avant d'écrire une ligne. C'est le seul
   chantier de débit encore ouvert avec le 7.

Deux murs, à garder en tête (pas prioritaires sauf demande du joueur) :
**18 brûlées** (la voie qui tranche : relaxation SMT/ILP des ressources,
§9.10 ; côté recherche : fermer k=5 — 30-40 min avec `--tt-mb` plus grand —
et les reculs > 150) ; **les rips précoces** (chantiers 5bis/6 ci-dessus —
le prior par poids est déjà réfuté, ne pas le re-tester).

## Les étalons de mesure (A/B, même graine, même budget)

```powershell
# construire
& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe" `
    build\combosolver.sln /p:Configuration=Release /p:Platform=x64 /m /nologo /v:quiet

# santé — LA porte de tout changement (0 écart retrouve la référence,
# 290 digests distincts / 0 fusion, A/B nouveauté ; l'écart de couverture
# IDLECMD #240 est PRÉEXISTANT — pas une régression)
.\bin\Release\combosolver.exe "D:\ProjectIgnis\replay\synchron handrip 2.yrpX" `
    --scriptdir ..\deps\scripts_2026-04-13\script --solve --solve-ms 60000 `
    --outdir sX_sante --no-chain Zalen --no-chain "Crystal Wing"
# NB : --outdir dédié TOUJOURS — le défaut « solutions/ » écraserait le corpus.

# Étalon 1 — l'escalade même-deck (racine = LA LIGNE 259 désormais) :
.\bin\Release\combosolver.exe "D:\ProjectIgnis\replay\synchron handrip 2.yrpX" `
    --scriptdir ..\deps\scripts_2026-04-13\script `
    --start "D:\ProjectIgnis\replay\synchron handrip 2.yrpX" `
    --solve-ms 600000 --seed 777 --finisher levin --optimize `
    --finisher-min 420000 --burn-limit 19 --archive-k 24 `
    --approach "sZ6_slack255/solution_00_b19_a55.yrp" `
    --outdir sX_iter `
    --guard "5:Crystal Wing|Zalen@terrain+Junk Signal@main" --guard-off "mainadv<=2" `
    --no-activate "Duel Evolution - Assault Zone" `
    --no-chain Zalen --no-chain "Crystal Wing" `
    --resolve "PSY-Framelord Omega@terrain:2" `
    --resolve "Trishula, Dragon of the Ice Barrier@terrain"
# Métriques STRUCTURELLES : coûts écrits, conversions (<-- BUT), coupures,
# lignes anytime/finisseur. Les compteurs de tirages sont du bruit (§9.12).

# Étalon 2 — les fenêtres --fire précoces (le mur des rips ; chantier 6) :
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
# Ligne de base session 6 (même build, même graine) : 10/15 converties
# (déc. 113-173), fenêtres 0-4 (déc. 58-103) non converties, crêtes 7-8/8,
# manques Crimson Dragon / Crystal Wing. Toute conversion 58-103 = percée.

# Étalon 3 — débit brut (chantiers 7-8) : états/s de la phase tirages de
# l'étalon 1 (~90 k états/s), ms/expansion du finisseur (~8 ms).

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
    variantes, pas une preuve** — seules les passes ÉPUISÉ comptent.
34. **Les brûlées ne sont pas monotones** (pic 23 → 19) : toute borne
    brûlées porte une marge — mais voir 40 : la borne est de toute façon
    inactive sur ce problème.
35. **La poursuite d'après-but est active sous `--optimize`** : les
    « solutions » à k=0 sont des ré-atteintes, pas un bug.
36. **Une réponse posée non traitée est ÉCRASÉE par le SetResponse suivant**
    — avancer au prompt avant d'injecter.
37. **`no_chain` global ≠ continuation `--fire`** : `--fire-no-chain` est le
    drapeau de mise en scène.
38. **Une contrainte `--resolve` ne dit pas OÙ résoudre** : pour imposer un
    rôle, museler les alternatives.
39. **(Session 6) À graine fixée, le bruit de run domine les compteurs de
    tirages** — rips ≥1 de 98 à 9 402 entre bras au mécanisme inerte. Un A/B
    se tranche sur les faits STRUCTURELS : conversions, coûts écrits,
    coupures, ensembles de fenêtres converties.
40. **(Session 6) Une borne qui ne coupe jamais est un réglage mort** — 
    instrumenter (compteurs dans les rapports) AVANT de calibrer. La borne
    B&B brûlées ne coupe rien sur ce problème : l'espace gagnant vit sous le
    pic de la référence (23).
41. **(Session 6) Une « convergence » d'escalade n'est qu'un plateau local**
    — la graine 777 déclarée convergée en session 5 a redonné −2 décisions
    en session 6. Trois graines muettes consécutives au moins avant de
    conclure.

## Discipline de vérification — non négociable

La santé passe AVANT et APRÈS chaque changement de moteur ; 0 retry /
290 distincts / 0 fusion ; **à 0 écart la référence est retrouvée** ; A/B
nouveauté automatique ; tout replay écrit rejoué depuis zéro et jugé avec les
MÊMES drapeaux ; toute amélioration de coût vérifiée sur les trois coûts ET
la discipline avant annonce ; A/B sur faits structurels uniquement (piège
39) ; un A/B perdant se documente et se désactive par défaut ; les runs
séquentiels (jamais deux mesures en parallèle sur la machine).

## Cas de test et fichiers

- LE cas : `D:\ProjectIgnis\replay\synchron handrip 2.yrpX` (référence
  19/56/273 ; **meilleure ligne connue 19/55/259**,
  `sZ6_slack255/solution_00_b19_a55.yrp`)
- Corpus historique `solutions/` (NE PAS écraser) ; variantes `sF_final/`
  (261), `sD_corpus/`, `sG_iter/` ; session 6 : `sZ6_*` (9 runs A/B + santé,
  commandes au §9.12)
- Refermetures `--fire` : `sM_fire5/`, `sN_fire_deep/` (`--opp-hand
  "27204311"` pour juger), `sP_bake300/`, `sT_zalen3/`, `sY_zalen5/`,
  `sZ6_fire_base/`, `sZ6_fire_prior/` (cuits : jugeables sans drapeau)
- Logs sessions 4-6 : `mA_*`→`mO_*`, `sA_*`→`sY_*`, `sZ6_*` (ignorés par
  git)
- `--workdir` par défaut `D:\ProjectIgnis` (ne jamais y écrire)

## Bibliographie (vérifiée sur arXiv)

| Levier | Papier | arXiv |
|---|---|---|
| Prior par rejeu — POIDS réfuté, ADAPTATION restante (chantier 5bis) | Policy Learning from Solved Games | 2401.10431 |
| Coût lexicographique NRPA (implémenté) | Montparnasse / MOGNRPALR | 2505.02110, 2606.07562 |
| LTS/PHS* (implémentés ; frontière partagée = chantier 7) | Policy-Guided Heuristic Search | 2103.11505 |
| Recul principled | √LTS | 2412.05196 |
| Archive d'états (fait) | Go-Explore | 2004.12919 |
| Stabilité NRPA | Stabilized NRPA | 2101.03563 |
| Adaptation lente niveau 1 (option chantier 4) | Montparnasse | 2505.02110 |
| Rejetés sur mesure | GNRPA-LR ; prior par poids | 2401.10420 ; 2401.10431 |

Définition de « terminé » pour la session 7 : (a) l'escalade relancée sur la
ligne 259 (≥ 3 graines) et son verdict chiffré ; (b) le chantier 5bis
(rejeu d'adaptation) OU le chantier 6 (racines croisées `--fire`) implémenté
et mesuré en A/B structurel sur son étalon ; (c) tout gain/perte chiffré,
les perdants désactivés par défaut et documentés ; (d) §9.13 documenté, ce
prompt régénéré.
