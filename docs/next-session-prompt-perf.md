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
- **Options (chantier 17) : GAGNANTES dans leur forme v3** — sélection
  gloutonne par PERTE DE LEVIN (le critère d'Alikhasi & Lelis, §9.19 (g)) au
  lieu du gain brut : le catalogue s'auto-limite (19 macros sur plafond 256),
  et l'A/B rend **best 8/8 trois runs sur trois** (témoin 7/7/6), ≥2 et ≥3
  résolutions au-dessus du témoin sur TOUTES les paires. OPT-IN :
  `--options 256` (exige `--adapt`) — forme recommandée. Les formes v1/v2
  (gain brut) et la FENÊTRE DE POSITION (`--options-window`) sont réfutées ;
  la précondition suivante doit être SÉMANTIQUE (`ctx`).
- `--phs-canonical` (coût (d+h)/π du papier au lieu de notre facteur e^h) :
  neutre sur l'étalon 0 (h≈1) — à juger sur l'étalon A (h_root 4-6).
- Snapshots Go-Explore aux cellules : écartés par l'instrument (kPrefix 0,0 %
  depuis dive_full).

## PIÈGE DE BUILD NOUVEAU — reproductibilité PGO

Un `MSBuild` ordinaire RELIE SANS `/USEPROFILE` : le binaire redevient LTO nu,
en silence. Après tout changement de moteur : rebuild, santé, puis re-dérouler
`measurements/s12_pgo.ps1` AVANT toute mesure. Témoins conservés :
`combosolver_preopt.exe` (pré-s11), `combosolver_lto_s12.exe` (LTO, code s12).

## ACQUIS DE LA SUITE DE SESSION (9.19 (h)-(m)) — ne pas re-mesurer

- **BOOTSTRAP AUTO-AMORCÉ POSITIF** (j) : étalon A but seul, corpus = 3
  approches 1/4 auto-générées → les macros multiplient les ≥2 résolutions par
  ×25-180 et les ≥3 par ×50-85 (2 graines, attribution nette : adaptation
  seule ≈ témoin). best reste 1/4 à 196 s.
- **`merged_pop` PAR DÉFAUT** (h) : Restore unitaires 4,36 → 0,58/exp, arène
  47 → 37 % de la phase, équivalence exacte. Cadran workers : 16 validé,
  24 destructeur (−18 %), le mur est la bande passante mémoire.
- **Rétro robustifié** (k) : corpus→graphe (LiftPolicyRun nourrit Observe),
  consommation intra-recette, tri canonique. Et **réfuté comme h de
  FINISSEUR** (l) : 2×2 neutre sur 16 paires — l'observation écrase l'amorce
  aux états profonds, et h est un plateau là-bas (donc `--phs-canonical`
  neutre PAR CONSTRUCTION aux h plats, pas seulement non mesuré). Le verrou
  reste le h plat ; la montée est couverte par les MACROS, pas par une
  distance dans le coût du finisseur.

## À FAIRE, par rendement estimé décroissant

1. **ITÉRER le bootstrap** (`measurements/s12_bootstrap.ps1`) : gen2 ARMÉE des
   macros (--adapt corpus1 --options 256) → approches meilleures → corpus2 →
   macros meilleures. Et le conditionnement SÉMANTIQUE des macros (`ctx` ;
   le graphe de recettes est le substrat). Juge : « ≥ k résolutions » et
   best/4 — jamais le compteur de tirages.
2. **Run LONG d'exploitation** (étalon B, bras `sel` de
   `measurements/s12_suite_audit.ps1`, 600-1800 s) : convertir les 8/8 en lignes
   complètes AVEC rips.
3. **Coût fixe d'arène résiduel** (~37 % de la phase du finisseur) : Push
   allégé (miroir paresseux), stride. Contrôle : étalon 0.
4. **`Adv::Goal` traité comme `Adv::Dead`** dans `advance()` (9.18 (g)) :
   toujours non tranché.
5. Observation dormante : `/GENPROFILE` → 1898 indécodables C10 une fois,
   non reproduite ensuite.

## Montage de mesure (inchangé par ailleurs)

- Santé : commande dans `docs/next-session-prompt.md` (60 s) — diff attendu :
  QUE des durées.
- Étalon B contraint : `measurements/s9_etalon_b_contraint.ps1` ; ±`--profile` pour le
  µs/appel ; bras intercalés, médianes ×3 minimum.
- Étalon 0 : `measurements/s12_rejeux.ps1` (contrôle 42/b=0/ÉPUISÉ, colonne rj=).
- Runs séquentiels, un `--outdir` par run, jamais de relink pendant une mesure.
- Logs PowerShell 5.1 en UTF-16 : `Select-String`/`pwsh`, pas `grep`.
