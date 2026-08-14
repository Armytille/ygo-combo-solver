# Session PERFORMANCE (suite) : le profil existe, le levier est le NOMBRE d'appels

Tu reprends `combosolver` (racine `d:\ProjectIgnis\replay2video\combosolver`) pour
la suite du chantier performance. Lis d'abord `README.md`, puis
`docs/combo-solver-design.md` **§9.18** (la session 11 : l'instrument, les deux
régimes décomposés, le facteur 70 expliqué), §5, §7, et §9.16 (i).
**Ne redécouvre rien de ce qui y est chiffré.**

## L'ÉTAT : l'instrument est posé, il a déjà tranché

La session 11 a livré `--profile` (sondes rdtsc à temps exclusif, thread_local
versées par phase, ligne « reste » par construction — §9.18 (a)) et les trois
premières optimisations tenues par la santé stricte (LTO, C20, C22, plus la
surcharge à tampon de `RecipeDistance`). Le diff de santé avant/après ne
contient QUE des durées.

**Ce que le profil a tranché — ne pas le re-mesurer :**

- **Tirages** (étalon B contraint) : `Process` 82 %, 2,79 appels/décision à
  27,2 µs ; Restore 9 % (1/tirage) ; TOUT l'hôte ~18 %. Le softmax/politique
  (« reste ») : 2 % — **C27 est mort avant d'être tenté**, C21/C23-C28 ne
  peuvent pas rendre plus que des miettes.
- **Finisseur** (RunLevin) : `Process` 86 %, **79,9 appels par expansion** à
  131 µs — le coût est le REJEU du chemin à chaque saut de la file best-first,
  pas le développement des fils. Digest 0,03 %, requêtes 0,4 % : les hypothèses
  « digest/requêtes refaits par fils » sont éliminées.
- **Le coût d'un pas de core CROÎT avec la profondeur de l'état** : 27 µs
  (départ) → 90 µs (reculs profonds) → 131 µs (finisseur). Les 0,18 ms du
  jalon 0 étaient justes le long de la référence, pas représentatifs.
- **369 allocations d'arène par décision**, toutes dans `Process` (trafic
  Lua/core).

## À FAIRE, dans l'ordre

1. **Les trois répétitions et les médianes.** Les gains de la session 11 sont
   des runs uniques à graine fixée (piège 39). Refaire l'A/B binaire s11
   (`bin\Release\combosolver_preopt.exe` est le témoin pré-optimisation
   conservé) : étalon B contraint 60 s ×3 par bras, lire `tirages` et `etats`
   de la phase tirages, comparer des médianes. Chiffrer aussi le coût de
   l'instrument (même commande ±`--profile`, ×3).
2. **PGO.** Le profil d'exécution est extrêmement stable : cas d'école.
   `/GL` est déjà posé (LTO) ; ajouter l'instrumentation PGO, un run
   d'entraînement (santé + étalon B court), recompiler optimisé, mesurer comme
   en 1. Garder seulement si la santé stricte passe ET si la médiane gagne.
3. **Le chantier NOUVEAU désigné par l'instrument : réduire les REJEUX du
   finisseur.** 80 `Process` par expansion. Pistes, par coût croissant :
   allonger la pile de plongée (aujourd'hui elle ne retient que la branche
   courante) ; des checkpoints d'arène aux nœuds chauds de la file (attention
   mémoire : 441 pages/Restore aux états profonds) ; trier la file pour
   favoriser les voisins de la pile. CONTRÔLE : étalon 0 (`s9_ab_alpha.ps1`),
   la racine `recul 0` doit rendre 42 exp., b=0, ÉPUISÉ dans tous les bras —
   et le contrôle de GAIN est « plus d'expansions à temps égal ».
4. **Les OPTIONS (chantier 17)** restent le levier de l'EXPOSANT pour les
   tirages — `tools/s10_options.ps1` (30 s) n'a toujours pas été lu.

## Ce qu'il ne faut PAS faire

- Pas de C21/C23-C28 : le « reste » pèse 2 %, il n'y a rien à y gagner.
- Ne pas chronométrer les allocations d'arène (l'instrument fabriquerait le
  ralentissement — §9.18 (a)) ; comptées, jamais chronométrées.
- Ne pas relier le binaire pendant qu'une mesure tourne ; runs séquentiels ;
  un `--outdir` par run ; santé avant/après tout changement de moteur — ici le
  diff attendu est « exactement les mêmes nombres, durées plus basses ».
- Le digest : C22 a changé les VALEURS (contrôle structurel passé). Toute
  nouvelle modification exige le même contrôle : 273 digests deux à deux
  distincts, 210/273, 209 candidates, 16 replays, 19/56/272.

## Le montage de mesure (inchangé)

- **Santé** : commande dans `docs/next-session-prompt.md` (60 s).
- **Étalon B contraint** : `tools/s9_etalon_b_contraint.ps1` (90 s ; la session
  11 a utilisé 60 s).
- **Étalon 0 déterministe** : `tools/s9_ab_alpha.ps1` (finisseur seul).
- **Fumée** : `--solve-ms 5000` sur l'étalon A — l'en-tête s'imprime avant
  toute recherche.
- Logs PowerShell 5.1 en UTF-16 : lire avec `Select-String`/`pwsh`, pas `grep`.
