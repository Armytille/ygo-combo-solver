# État PERFORMANCE après la session 12 — lire §9.19 avant tout

Tu reprends `combosolver` (racine `d:\ProjectIgnis\replay2video\combosolver`).
Lis `README.md` puis `docs/combo-solver-design.md` **§9.19** (session 12 : les
médianes, le compteur déclassé, dive_full, PGO, les options réfutées sans
contexte), et §9.18. **Ne redécouvre rien de ce qui y est chiffré.**

## CE QUE LA SESSION 12 A TRANCHÉ — ne pas re-mesurer

- **Le compteur tirages/états à graine fixée est DÉCLASSÉ comme instrument
  d'A/B de débit** (dispersion ±10-18 % par bras, médianes ×3). Le « +31,8 % »
  de la s11 est RETIRÉ. Les juges : le µs/appel sous `--profile` (même sonde
  des deux côtés), et les contrôles exhaustifs déterministes.
- **`Process` par appel (tirages)** : 27,2 (pré-opt) → 23,6 (LTO) → **18,6
  (PGO, −21,4 %, distributions disjointes ×3)**. PGO gardé.
- **`dive_full` PAR DÉFAUT** (pile de plongée complète) : rj 12/12 → 1,5/14,
  Process/expansion 54,5 → 12,8, **+92 % d'expansions à temps égal**, ordre
  d'extraction inchangé (contrôle : 42/b=0/ÉPUISÉ, mêmes best). `--no-dive-full`
  = témoin. `--lifo-ties` réfuté seul, neutre combiné.
- **Options (chantier 17) : implémentées, VIVANTES, et leur forme SANS CONTEXTE
  est réfutée** (deux A/B perdants — 8-9 prises/tirage, absorption 1,0-2,1
  contre ~6,6 visées, 78-84 % d'avortements). Diagnostic : une macro minée aux
  décisions 40-47 est proposée dès la décision 5. Éteintes par défaut
  (`--options 256` pour reprendre, exige `--adapt`).
- La prévision (lue enfin — le script s10 tournait sur un chemin où `--adapt`
  était IGNORÉ, corrigé) : seul le gros catalogue vaut (256/support 2 :
  8,5 ordres, 93 % absorbé) ; 16 et 64 contre-productifs.

## PIÈGE DE BUILD NOUVEAU — reproductibilité PGO

Un `MSBuild` ordinaire RELIE SANS `/USEPROFILE` : le binaire redevient LTO nu,
en silence. Après tout changement de moteur : rebuild, santé, puis re-dérouler
`tools/s12_pgo.ps1` AVANT toute mesure. Témoins conservés :
`combosolver_preopt.exe` (pré-s11), `combosolver_lto_s12.exe` (LTO, code s12).

## À FAIRE, par rendement estimé décroissant

1. **Options CONDITIONNÉES** : miner et proposer les macros avec leur contexte
   (`PolicyStep::ctx` existe des deux côtés du releveur) et/ou fenêtre de
   position sur la ligne d'origine. Juge : « ≥ k résolutions » et « 8/8 AVEC
   rips » — jamais le compteur de tirages. Montage : `tools/s12_options_ab2.ps1`.
2. **Coût fixe d'arène du finisseur** (44,5 % de la phase depuis dive_full :
   Restore 22,6 %, Push 17,4 %, Pop 4,5 %) : stride de pile, journaux plus
   légers, Restore paresseux. Contrôle : étalon 0 (`tools/s12_rejeux.ps1`).
3. **`Adv::Goal` traité comme `Adv::Dead`** dans `advance()` (9.18 (g)) :
   toujours non tranché — pas de récupération d'après-but dans le finisseur.
4. Observation ouverte : binaire `/GENPROFILE` → 1898 indécodables C10 en
   santé (binaires normal et PGO : zéro). À trancher si un run normal en
   remonte un jour.

## Montage de mesure (inchangé par ailleurs)

- Santé : commande dans `docs/next-session-prompt.md` (60 s) — diff attendu :
  QUE des durées.
- Étalon B contraint : `tools/s9_etalon_b_contraint.ps1` ; ±`--profile` pour le
  µs/appel ; bras intercalés, médianes ×3 minimum.
- Étalon 0 : `tools/s12_rejeux.ps1` (contrôle 42/b=0/ÉPUISÉ, colonne rj=).
- Runs séquentiels, un `--outdir` par run, jamais de relink pendant une mesure.
- Logs PowerShell 5.1 en UTF-16 : `Select-String`/`pwsh`, pas `grep`.
