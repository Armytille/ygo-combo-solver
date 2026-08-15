# Session 14 — LA MISSION CHANGE DE FORME : le bootstrap en UNE SEULE TRAITE

Tu reprends `combosolver` (racine `d:\ProjectIgnis\replay2video\combosolver`).
Lis `README.md` puis `docs/combo-solver-design.md` **§9.20** (session 13) et
§9.19. **Ne redécouvre rien de ce qui y est chiffré.**

## LA MISSION, fixée par l'opérateur en fin de session 13

**Un run complet d'une seule traite** : `combosolver gabarit.yrpX --no-ref
--target ... ` doit monter SEUL de 0/4 vers le but — **zéro corpus externe,
zéro `--approach` hérité, zéro relance**. La session 13 a prouvé la
FAISABILITÉ de la boucle en plusieurs runs (gen1 nue → macros → gen2 armée →
premier 2/4 ; §9.20 (a)-(b)) ; la session 14 doit l'INTERNALISER. La matière
existe déjà en mémoire pendant un run : `NrpaShared.best` (avec ses
`PolicySteps`), les meilleurs runs par worker, les lignes des racines de rip.
Ce qui manque : le RE-MINAGE périodique et la DIVERSITÉ des lignes minées.

## CE QUE LA SESSION 13 A ÉTABLI — ne pas re-mesurer (détail §9.20)

- Bootstrap multi-runs : gen2 armée → premier 2/4 but seul ; forme gagnante =
  corpus CUMULÉ ; macros de gen2 plus longues, avortements ÷2,3-18 ; gen3
  PLATEAU à budget constant — la conversion armée est stochastique (~1 run
  sur 2-3 à 240-300 s).
- Run long étalon B contraint (1200 s) : **16 solutions complètes avec rips
  et garde, 264 déc. contre 276 (référence), 0 MSG_RETRY** — le budget
  convertit.
- `--options-ctx <tol>` (garde sémantique) : NON-destructif, signe positif
  étalon A (≥3 sur 4/4 paires, conversions 2/2 contre 1/2), indécis étalon B
  60 s. OPT-IN. `--finisher-post-goal` écrit, jamais jugé.
- PGO s13 propre (`tools/s13_pgo.ps1`, témoin `combosolver_lto_s13.exe`).
  `canonical_zones` découvert DORMANT (jamais branché nulle part).

## LES CHANTIERS, par ordre — chacun adossé à un papier lu en séance

**1. `--options-online` : le minage EN LIGNE (le cœur de la mission).**
Précédent : Marvin (arXiv:1110.2736), macros mémoïsées PENDANT la recherche et
utilisées dans le même solve. Chez nous : re-dérouler `MineOptionCatalog`
(sélection par perte de Levin, déjà en place — arXiv:2410.11262) toutes les
60-120 s sur les meilleures lignes DU RUN (shared best + top-N par worker +
lignes diverses), et échanger le catalogue à une frontière sûre (les workers
le lisent en const : swap entre itérations de niveau ou au join d'une phase,
PAS en plein tirage). Deux faits d'architecture qui aident : (a) les ids de
macro sont des hash de CONTENU — stables entre re-minages, les poids appris
survivent ; (b) les lignes trouvées AVEC macros sont enregistrées compressées
(les décisions absorbées ne produisent pas de PolicyStep) — re-miner dessus
donne des MACROS DE MACROS gratuitement (hiérarchie émergente). Garde ctx
(`--options-ctx 1`) dans la boucle : c'est la leçon de MAGIC
(arXiv:2011.03813, macros conditionnées à la situation).
JUGE : étalon A but seul, UN run, SANS `--adapt` — témoin = run nu ;
best k/4 et ≥k résolutions, 2-3 graines, 300-600 s d'abord, puis LE test
final : 1200-1800 s une traite. La barre : ≥2/4 reproduit là où le run nu
fait 1/4 — puis viser 3/4.

**2. La POMPE À DIVERSITÉ : répétitions limitées + adaptation lente.**
Le mineur en ligne a besoin de lignes bonnes ET DIVERSES (en multi-runs, la
diversité venait des graines ; en une traite elle doit venir de l'intérieur).
Deux mécanismes de la littérature NRPA, jamais essayés ici :
- **GNRPA à répétitions limitées** (arXiv:2401.10420) : plafonner les
  répétitions de la meilleure séquence par niveau — contre l'effondrement de
  l'adaptation dans un seul bassin (notre cause probable de conversion
  stochastique). Quelques lignes de code.
- **Adaptation lente et longue au niveau 1** (recette Montparnasse,
  arXiv:2505.02110 et 2606.07562, Eterna100 résolu ainsi) : cadran
  `--nrpa-level`/`--nrpa-lr`/passes sous cette forme.
Option si le front vaut le coût : best MULTI-OBJECTIF façon Pareto-NRPA
(arXiv:2507.19109) — un petit front non dominé (≥k résolutions, recouvrement,
coût) à la place du best unique, adaptation pondérée par l'isolement.

**3. Les OPTIONS DANS LE FINISSEUR (adoption complète de 2410.11262).**
Les macros ne vivent que dans PolicyRollout ; or c'est RunLevin qui convertit
(§9.20 (d)). Proposer les macros applicables comme ARÊTES de l'arbre de Levin
(coût log 1/π_macro, avance de k décisions, avortement = arête morte comme au
rollout). ATTENTION au contrôle : l'étalon 0 changera LÉGITIMEMENT ses
comptes d'expansions (les arêtes macro compressent les chemins) — le contrôle
devient « mêmes best par racine, aucune solution perdue, ÉPUISÉ toujours
ÉPUISÉ », pas l'identité des 42 expansions. Le documenter AVANT de mesurer.

**4. Toujours en attente (secondaire)** : `--finisher-post-goal` sous
`--optimize` ; `--phs-canonical` sur étalon A (h_root 4-6) ; coût d'arène du
finisseur (~37 %) ; `canonical_zones` à brancher (A/B gratuit) ; ETW pour
décomposer `Process` (part Lua VM/GC — si le GC pèse : mode générationnel
5.4, compatible arène).

## PIÈGES DE BUILD ET D'ENVIRONNEMENT

- Un `MSBuild` ordinaire RELIE SANS `/USEPROFILE` : après tout changement de
  moteur → rebuild, santé stricte, puis **`tools/s13_pgo.ps1`** (PAS s12 — il
  écraserait son témoin) AVANT toute mesure. Témoins : `combosolver_preopt`,
  `combosolver_lto_s12`, `combosolver_lto_s13`.
- **La cdb vivante n'est PAS épinglée** (+9 cartes en cours de s13, sans
  effet — decks figés) ; si un A/B inter-jours diverge sur l'énumération,
  vérifier ça d'abord. Les scripts SONT épinglés par `--scriptdir`.
- Le re-minage en ligne touche le chemin chaud : re-dérouler le PGO après,
  et vérifier que le minage lui-même reste sous la seconde (il tourne
  désormais DANS le budget du run).

## Montage de mesure

- Santé : commande ci-dessous — diff attendu : QUE des durées (référence du
  jour : `s13_sante_pgo.log`).
- Bootstrap étalon A une traite : partir de `tools/s13_bootstrap_gen3.ps1`
  en RETIRANT les `--adapt` (le témoin nu) ; bras `--options-online` en face.
- Étalon 0 : `tools/s12_rejeux.ps1` — contrôle à REDÉFINIR pour le chantier 3
  (voir ci-dessus).
- Runs séquentiels, un `--outdir` par run, jamais de relink pendant une
  mesure. Logs PS 5.1 en UTF-16 : `Select-String`/`pwsh`, pas `grep`.
- Juges : best k/4, ≥k résolutions, conversions — JAMAIS le compteur de
  tirages (déclassé, §9.19 (a)).

```powershell
# construire
& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe" `
    build\combosolver.sln /p:Configuration=Release /p:Platform=x64 /m /nologo /v:quiet

# sante — LA porte de tout changement de moteur, avant ET apres
.\bin\Release\combosolver.exe "D:\ProjectIgnis\replay\synchron handrip 2.yrpX" `
    --scriptdir ..\deps\scripts_2026-04-13\script --solve --solve-ms 60000 `
    --outdir s14_sante --no-chain Zalen --no-chain "Crystal Wing"
# 273 digests, 210/273, 209 candidates, 16 replays sur 209, 19/56/272.
```

## Définition de « terminé »

(a) `--options-online` implémenté, santé stricte, A/B une-traite contre le
run nu sur 2-3 graines. (b) Répétitions limitées implémentées et jugées (avec
et sans le minage en ligne — l'attribution séparée). (c) Le test reine : UN
run de 1200-1800 s, une traite, `--no-ref --target ... --options-online`,
best k/4 lu. (d) Si le temps le permet : arêtes macro dans RunLevin, contrôle
redéfini. (e) §9.21 documenté, ce prompt régénéré.
