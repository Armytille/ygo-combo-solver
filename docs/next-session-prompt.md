# État après la session 13 — lire §9.20 avant tout

Tu reprends `combosolver` (racine `d:\ProjectIgnis\replay2video\combosolver`).
Lis `README.md` puis `docs/combo-solver-design.md` **§9.20** (session 13 : le
bootstrap itéré, le run long converti, la garde sémantique), et §9.19 (les
médianes, le compteur déclassé, dive_full, PGO, options par perte de Levin).
**Ne redécouvre rien de ce qui y est chiffré.**

## CE QUE LA SESSION 13 A ÉTABLI — ne pas re-mesurer

- **Le bootstrap ITÈRE et MONTE (9.20 (a)-(b))** : la gen2 armée des macros de
  la gen1 a écrit le PREMIER 2/4 en but seul (étalon A, graine 4242, 214
  décisions, budget de génération inchangé 240 s) ; ≥2/≥3 ×20-40 en
  appariement par graine. L'A/B des corpus tranche la forme de production :
  **le corpus CUMULÉ** (c1+c2, `--adapt` répété) convertit best 2/4 sur les
  DEUX graines de mesure à 300 s ; les macros minées sur des approches armées
  sont moins nombreuses et plus longues (3-6 × 7,0-7,3 contre 11 × 4,4),
  absorption ×2-4, avortements ÷2,3-18. **On ne jette jamais les lignes des
  générations passées.**
- **Le run LONG convertit (9.20 (d))** : étalon B contraint, bras sel, 1200 s,
  graine 888 → **16 solutions complètes** (board 8/8 + rips + garde), toutes à
  19 brûlées / 56 actions / **264-266 décisions contre 276 pour la référence
  humaine**. Vérifiée sur pièce : 0 MSG_RETRY, garde tenue 27 fenêtres,
  résolutions 2/2+1/1. Graine 1234 : pas de conversion (crête-rip 7/8) — une
  graine sur deux à ce budget.
- **La gen3 PLAFONNE à budget constant (9.20 (f))** : 3× 1/4, ≥k au niveau
  gen2. Le grand gain était l'ARMEMENT (gen1→gen2) ; le cran suivant demande du
  BUDGET, pas juste une itération de plus. La conversion reste stochastique
  (~1 run armé sur 2-3 à 240-300 s).
- **`--options-ctx` (garde sémantique : posées exactes, main ±tol) : vivante,
  NON-destructive, signe positif, OPT-IN (9.20 (g))** : ≥3 > off sur 4/4
  paires gardées (étalon A), conversions 2/2 contre 1/2, le mode de
  défaillance de la fenêtre positionnelle (≥3=0) est ABSENT. Étalon B 60 s :
  indécis (dispersion). Même test dans le mineur et le rollout ; le catalogue
  peut changer sous la garde.
- **`--finisher-post-goal` écrit, JAMAIS jugé** : 9.18 (g) tranché — GoalCheck
  enregistre avant Adv::Goal (rien n'était perdu), mais sous --optimize la
  récupération d'après-but était absente du finisseur. Opt-in.
- **PGO s13 re-déroulé** (`tools/s13_pgo.ps1`, témoin `combosolver_lto_s13.exe`
  conservé, 4 régimes d'entraînement). Santé instrumentée PROPRE ×2 — l'anomalie
  C10 de 9.19 (e) reste non reproduite. Le µs/appel du PGO s13 contre son
  témoin n'a PAS été re-relevé (hygiène en attente, `s12_pgo_mesure.ps1`
  adaptable).

## PIÈGES DE BUILD ET D'ENVIRONNEMENT

- Un `MSBuild` ordinaire RELIE SANS `/USEPROFILE` : après tout changement de
  moteur → rebuild, santé, puis **`tools/s13_pgo.ps1`** AVANT toute mesure.
  Témoins : `combosolver_preopt.exe`, `combosolver_lto_s12.exe`,
  `combosolver_lto_s13.exe`.
- **La base de cartes vivante N'EST PAS épinglée** (9.20 (f), observation) :
  +9 cartes en cours de session (24 758 → 24 767, mise à jour cdb externe).
  Sans effet sur les montages (decks figés) — mais si un A/B inter-jours
  diverge sur l'énumération, vérifier ça d'abord. Les scripts, eux, SONT
  épinglés par --scriptdir.

## À FAIRE, par rendement estimé décroissant

1. **LE RUN LONG ARMÉ sur l'étalon A** — l'expérience reine, jamais faite :
   `--adapt` cumul (s12_boot_corpus + s13_boot_corpus2 + s13_boot_corpus3)
   `--options 256`, 1200-1800 s, 2-3 graines. Viser le 3/4 (le budget
   convertit : preuve sur l'étalon B). Variante : `--options-ctx 1` en bras
   intercalé — son vrai test est ICI, pas à 60 s.
2. **Boucle gen4+** avec corpus cumulé et budget de génération relevé
   (600 s+). Juge : conversions best k/4 — jamais le compteur de tirages.
3. **`--finisher-post-goal`** : A/B sous `--optimize` (étalon B même-deck,
   s10-style), juge = coûts des solutions (brûlées/actions/décisions).
4. **`--phs-canonical` sur l'étalon A** (h_root 4-6 à la racine) — en attente
   depuis la s12 ; c'est le seul montage où h n'est pas plat à la racine.
5. **Coût fixe d'arène du finisseur** (~37 % de la phase) : Push allégé,
   stride. Et **`canonical_zones`** (9.20 (e)) : mécanisme DORMANT jamais
   branché — A/B gratuit sur le branchement de colonnes du finisseur.
6. **Décomposition ETW de `Process`** (part Lua VM / GC / C++) : xperf sur le
   binaire PGO, sans code ni relink. Si le GC Lua pèse → mode générationnel
   5.4 (`lua_gc`), compatible arène, jamais essayé.

## Montage de mesure (inchangé)

- Santé : commande dans ce fichier (§ ci-dessous) — diff attendu : QUE des
  durées (et la dérive cdb éventuelle, structurel identique).
- Étalon B contraint : `tools/s9_etalon_b_contraint.ps1` ; ±`--profile` pour le
  µs/appel ; bras intercalés, médianes ×3 minimum.
- Étalon 0 : `tools/s12_rejeux.ps1` (contrôle 42/b=0/ÉPUISÉ, colonne rj=).
- Bootstrap étalon A : `tools/s13_bootstrap_gen3.ps1` (gabarit des runs armés).
- Runs séquentiels, un `--outdir` par run, jamais de relink pendant une mesure.
- Logs PowerShell 5.1 en UTF-16 : `Select-String`/`pwsh`, pas `grep`.

```powershell
# construire
& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe" `
    build\combosolver.sln /p:Configuration=Release /p:Platform=x64 /m /nologo /v:quiet

# sante — LA porte de tout changement de moteur, avant ET apres
.\bin\Release\combosolver.exe "D:\ProjectIgnis\replay\synchron handrip 2.yrpX" `
    --scriptdir ..\deps\scripts_2026-04-13\script --solve --solve-ms 60000 `
    --outdir s14_sante --no-chain Zalen --no-chain "Crystal Wing"
# 273 digests, 210/273, 209 candidates, 16 replays sur 209, 19/56/272.
# Reference du jour : s13_sante_pgo.log.
```
