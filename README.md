# ygo-combo-solver

Finds Yu-Gi-Oh! combo lines by searching inside a real duel.

The solver links [`ocgcore`](https://github.com/edo9300/ygopro-core) statically.
It replays a `.yrpX` recording in a live duel, captures the board the player
finishes their turn on, then searches for other move sequences that reach the
same board. Results are written as `.yrp` replays that open in EDOPro.

The solver implements no game rules. Legality is decided by `ocgcore`, and every
move it considers comes from a `MSG_SELECT_*` prompt the core issued. The
project supplies a memory arena that snapshots and restores a live duel in
0.12 ms, making tree search over duel states practical.

---

## Contents

- [Requirements](#requirements)
- [Build](#build)
- [Release](#release)
- [Modes](#modes)
- [Flags](#flags)
- [Script sets and replay fidelity](#script-sets-and-replay-fidelity)
- [Constraint grammar](#constraint-grammar)
- [Opponent test](#opponent-test)
- [Output](#output)
- [Design](#design)
- [Search algorithms](#search-algorithms)
- [Repository layout](#repository-layout)
- [Conventions](#conventions)
- [Limitations](#limitations)
- [References](#references)

---

## Requirements

Windows x64, Visual Studio 2022 build tools, PowerShell. Python 3 with sympy for
the formula-checking scripts in `tools/`.

A working EDOPro installation containing `cards.cdb`, `expansions/`,
`repositories/` and the card scripts. The solver reads that tree the way the
client does and never writes to it.

The binary contains no absolute path. Point it at the installation with
`--workdir` on each run, or set the location once:

```powershell
setx COMBOSOLVER_WORKDIR "C:\Games\ProjectIgnis"
```

`--workdir` takes precedence over the variable. With neither set, the run stops
with an error.

Dependencies fetched or expected outside this repository:

| Path | Contents |
|---|---|
| `../edopro/ocgcore` | source repository for `ocgcore`, checked out at a pinned commit |
| `../edopro/gframe/lzma` | LZMA sources, used for compressed replays |
| `../deps/ocgcore` | patched copy produced by `tools/fetch_solver_deps.ps1` |
| `../deps/scripts_<date>` | frozen export of the card scripts |
| `../vcpkg` | sqlite3, triplet `x64-windows-static` |

---

## Build

```powershell
.\tools\fetch_solver_deps.ps1
..\premake5\premake5.exe vs2022 --vcpkg-root=..\..\vcpkg
MSBuild build\combosolver.sln /p:Configuration=Release /p:Platform=x64
```

The binary is written to `bin\Release\combosolver.exe`. Use this build for
development. For a build you intend to keep or distribute, see
[Release](#release): an ordinary MSBuild discards the profile-guided
optimisation without reporting it.

`fetch_solver_deps.ps1` extracts one `ocgcore` commit and the matching `lua/src`
commit into `../deps/ocgcore`, applies the patches listed below, and freezes a
card script export next to it. It does not modify the EDOPro installation.

### Patches applied to ocgcore

| File | Change |
|---|---|
| `lua/luaconf-customize.h` | fixed string hash seed; declaration of the arena allocator hook |
| `lua/src/lauxlib.c` | routes the Lua heap through the arena allocator |
| `ocgapi.cpp` / `ocgapi.h` | adds `OCG_DuelQueryProcessorState`: phase, resolution stack, current chain, once-per-turn counters |

The C++ side of the core is unpatched. Its allocations are captured by a global
`operator new` overload on the solver side.

`OCG_DuelQueryProcessorState` is required for correct state hashing. The public
API exposes only the zones, so two instants inside the same chain resolution —
identical field, hand and prompt — produce the same hash without it. The
transposition table then merges them and prunes the branch.

---

## Release

```powershell
.\tools\build_release.ps1 -Workdir <edopro-install>
```

Takes about ten minutes: instrumented link, three training runs, optimised
relink, self-containment check, then `dist\` and a zip beside it. `-NoPgo` skips
the training and produces a plain LTO build in about two minutes.

MSVC applies a profile at link time through the `_LINK_` environment variable.
An ordinary MSBuild relinks without `/USEPROFILE` and silently drops the
profile, so releases must be built with this script and rebuilt with it after
every engine change.

Measured gain from the profile this script produces: 11.06 → 10.55 µs per
`Process` call, a 4.6 % median improvement over three interleaved repetitions.
All six runs made exactly 49 280 calls, making this a fixed-work comparison, and
the two distributions do not overlap. The design notes record 21.4 % for a
profile trained on seven regimes including the benchmark it was measured on;
this script trains on the replay template in `gabarits/` only. The metric is µs
per call. The rollout counter is unusable here because its dispersion at a fixed
seed exceeds the effect size.

The binary is statically linked and imports `KERNEL32.dll` only; the build
script fails if that changes. No path is resolved relative to the executable, so
it runs from any location with any working directory:

```powershell
$env:COMBOSOLVER_WORKDIR = "C:\Games\ProjectIgnis"
C:\anywhere\combosolver.exe .\gabarits\etalon_a_lunalight.yrp --solve --outdir out
```

Card databases and scripts are read from the EDOPro installation. `--outdir` is
resolved against the current directory. The zip contains the executable, this
README, and the replay template used by the examples.

---

## Modes

### 1. Instrumented replay

The default. Replays the recorded line, reports what the core offered at each
decision, captures the target board, and runs its self-checks.

```bash
combosolver.exe duel.yrpX --scriptdir <scripts> --workdir <edopro-install>
```

### 2. Search in the same duel

Bounded-discrepancy search seeded on the recorded line: follow the reference and
allow at most *k* deviations. At zero deviations it reproduces the reference, so
this mode always returns at least one solution. An empty result signals a
defect.

```bash
combosolver.exe duel.yrpX --scriptdir <scripts> --solve --outdir solutions
```

### 3. Same board, different duel

Another deck, another hand, another seed. Recorded answers are indices and mean
nothing in a different duel, so each decision is re-identified by the cards it
engages.

```bash
# starting from another replay
combosolver.exe ref.yrpX --scriptdir <scripts> --start other.yrpX --outdir solutions

# starting from a decklist and an opening hand
combosolver.exe ref.yrpX --scriptdir <scripts> \
    --deck "<edopro-install>\deck\Lunalight.ydk" \
    --hand "Assault Zone|Ash Blossom|Ash Blossom|Ash Blossom" \
    --outdir solutions
```

The requested hand is forced through `DUEL_PSEUDO_SHUFFLE` and then verified on
a throwaway duel before the search starts. Which end of the deck the core draws
from is checked rather than assumed, and a mismatch stops the run.

### 4. Goal-only

No reference line. The replay supplies the duel template — flags, life points,
opponent deck — and the target board is written out explicitly.

```bash
combosolver.exe template.yrpX --scriptdir <scripts> \
    --deck Lunalight.ydk --hand "57103969|57103969|57103969" \
    --no-ref --target 54701958 --target 54701958 --target 54701958 \
    --target "90590304@DEF" --max-decisions 700
```

### 5. Judge

With constraint flags and without `--solve`, the tool checks whether a replay —
one it produced, or one played by hand — satisfies the constraints.

```bash
combosolver.exe solutions/solution_00.yrp --scriptdir <scripts> \
    --guard "5:Crystal Wing|Zalen@field+Junk Signal@hand"
```

---

## Flags

`combosolver.exe --help` lists all 133 flags with their defaults. The ones in
regular use:

| Flag | Meaning |
|---|---|
| `--workdir <dir>` | EDOPro installation, or set `COMBOSOLVER_WORKDIR` |
| `--scriptdir <dir>` | card script set, highest priority first; repeatable |
| `--player <0\|1>` | whose turn is optimised |
| `--outdir <dir>` | destination for the replays produced |
| `--solve` | search instead of replaying only |
| `--solve-ms <ms>` | search time budget |
| `--threads <n>` | search workers; defaults to every core |
| `--start <replay>` | rebuild the board starting from another duel |
| `--deck <file.ydk>` | rebuild the board starting from a decklist |
| `--target <card[@ATK\|DEF]>` | define the target board explicitly; repeatable |
| `--optimize` | keep searching for cheaper lines rather than stopping at the first |
| `--profile` | per-phase profile of the hot paths |

### Reproducible runs

`--max-rollouts` replaces the wall-clock budget with a rollout count. At
`--threads 1`, two executions then perform identical work. Under a wall-clock
budget, two runs at an identical seed diverge.

```powershell
combosolver.exe ... --threads 1 --max-rollouts 20000 --max-nodes 500000 `
    --solve-ms 900000   # large enough that time never binds
```

A single worker costs roughly five to six times the throughput, so this mode is
for attribution, not for production runs.

---

## Script sets and replay fidelity

`--scriptdir` is required in practice.

A `.yrpX` replays faithfully only with the core and the Lua card scripts
contemporary with its recording. With a different script set the replay diverges
without any error: the core asks a different question, the recorded answer no
longer applies, and the tool continues printing plausible measurements.

The `MSG_RETRY` counter in the report is the detector. On the reference replay:
218 retries against the live scripts, 0 against the pinned export produced by
`fetch_solver_deps.ps1`.

The same constraint applies to the core, which is why the fetch script pins a
commit instead of tracking `HEAD`. A replay recorded by an older client requires
an older core even though its scripts update independently.

---

## Constraint grammar

Most constraints remove branches from the enumeration rather than filtering
results afterwards.

Zones: `hand`, `field`, `grave` (also `graveyard`), `banished`, `extra`.
Default `field`. The French spellings used by earlier command lines — `main`,
`terrain`, `cimetiere`, `banni` — remain accepted.

Attributes, for `--material`: `light`, `dark`, `earth`, `water`, `fire`, `wind`,
`divine`.

```bash
# the n-th summon must be one of these cards
--summon "5:Zalen|Crystal Wing"

# from the 5th summon onward, at every opponent response window at least one
# clause must hold; a clause is a conjunction of card@zone atoms
--guard "5:Crystal Wing|Zalen@field+Junk Signal@hand"

# the guard stops being required once the opponent's hand is down to 2 cards
--guard-off "opphand<=2"

# state predicate instead of a card: N opponent cards banished
--guard "5:Dis Pater@field+oppbanished>=1"

# the line must resolve this effect twice, activated from the field
--resolve "PSY-Framelord Omega@field:2"

# the line must summon this card at least once
--summon-min "Junk Meister"

# summoning this card must consume a LIGHT material
--material "Chaos Angel:light"

# never activate this card from the field
--no-activate "Duel Evolution - Assault Zone"

# never chain this card at a response window
--no-chain "Crystal Wing"
```

Three details that are easy to misread:

- `--guard` asserts that an answer is available when the window opens. It does
  not require the guard to be the n-th summon; the card may already be in play.
- `--resolve` is checked at the goal, not along the line. A board that matches
  the target without the required resolutions is rejected and the search
  continues.
- `@zone` on `--resolve` restricts the activation zone. Without `@field`, an
  Omega banishing from the field and an Omega effect resolving from the
  graveyard both count. This produced a measured false positive.

---

## Opponent test

`--guard` is a static check: it asserts that an answer is available. `--fire`
plays the threat. The named card is added to the opponent's hand and activated
at every window where doing so is legal, one attempt per window, and the search
must rebuild the board from the resulting state.

```bash
combosolver.exe duel.yrpX --scriptdir <scripts> \
    --fire "27204311" --fire-spare "Junk Signal" --fire-ms 60000 \
    --resolve "PSY-Framelord Omega@field:2"
```

`--fire-spare` lists cards that may be spent answering; the target board without
them is also accepted at the goal. `--fire-open` restricts injection to windows
with an empty chain, so the threat starts a chain instead of being chained onto
the player's own effects.

A hand test start gives the opponent no hand. With no playable card across the
table the core never opens a response window, the guard passes vacuously and a
handrip removes nothing. `--opp-hand` supplies the opponent with cards. Replays
produced under `--opp-hand` only replay with the same `--opp-hand`.

---

## Output

Every run ends with self-checks. If the replay does not reproduce, or if a
snapshot does not restore the state exactly, no later measurement is meaningful.

```
=== resultats ===
  reponses consommees : 56 / 56
  MSG_RETRY           : 0   (rejeu fidele)

--- potentiel d'elision (une seule reponse legale) ---
  SELECT_CHAIN               29 / 31   forcees  (94%)
  TOTAL                      29 / 57   forcees  (51%)

--- couts d'execution ---
  deroulement de la ligne    :      2.3 ms pour 56 decisions  (0.041 ms/decision)
  => re-simulation complete depuis la racine : 16.9 ms

=== test de fidelite de la restauration ===
  restauration              : 0.12 ms  (0.84 Mo recopies)
  empreinte etat final      : 067573434fddcd1d vs 067573434fddcd1d
  => IDENTIQUE : l'instantane capture bien tout l'etat du duel

=== test de stress des instantanes ===
  freres successifs (Push/Restore/Pop)        18/18  ok
  imbrication profonde (20 niveaux)            9/9   ok
  ligne complete rejouee apres stress          1/1   ok
```

Reproduce with `combosolver.exe gabarits\etalon_a_lunalight.yrp --workdir <your
EDOPro>`. The report text is French; see [Limitations](#limitations).

The search depends on the ratio between the two figures above: 0.12 ms to
restore a state against 16.9 ms to re-simulate it from the root.

Runs also report what was pruned and whether each mechanism was active.
Mechanisms in this project have stayed switched off for entire sessions while
reporting as active, so each one now prints a liveness counter. A report
carrying `!! INERT` invalidates the run before the budget is spent.

---

## Design

### Memory arena

The OCG API provides no state cloning. A hand-written serialiser is impractical:
at a `MSG_SELECT_*`, Lua coroutines are suspended mid-resolution with their
stacks and upvalues live. The solver saves the memory instead of the game state.

All mutable duel memory is confined to a controlled address range: the Lua heap
through the allocator passed to `lua_newstate`, and the core's C++ objects
through the global `operator new` overload. Restores happen at the same base
address, so absolute pointers stay valid without relocation and the duel is
unaware of the restore.

Restore is the frequent operation and copies back only the pages the child
dirtied. On Windows this uses `GetWriteWatch`. The WebAssembly port replaces it
with a software write barrier (`arena.cpp`) whose completeness is checked by a
verifier.

An allocation that does not fit in the arena goes to the host heap and cannot be
restored. The arena is then marked poisoned and the worker aborts, because any
subsequent measurement would describe a duel the restore can no longer
reconstruct.

### State graph rather than action tree

The action tree along the reference line alone contains about 10^97 sequences.
The search instead explores the state graph: activating A then B and B then A
converge on the same node, and the transposition table merges them.

The transposition key covers the visible zones, the prompt payload — which is
where once-per-turn counters appear — and the processor state. Under-hashing
removes solutions without any error, so the report counts merges along the
reference line, whose states are pairwise distinct by construction.

Equivalence decisions:

| Rule | Reason |
|---|---|
| Battle position is ignored for the goal test, kept in the state digest | two boards differing only by position are the same board |
| Card artworks are collapsed via `QUERY_ALIAS` | a line recorded on one deck must be recognised on another |
| Deck order is hashed as is | draws, excavations and flips read it |
| Hand and graveyard order is sorted before hashing | neither is a game state |

### Novelty pruning and subgoals

The transposition table merges identical states only. Two lines differing by one
card in the graveyard are distinct without being usefully different. Novelty
pruning (Iterated Width) keeps a state only when it makes at least one atom true
that has never been true before. `--width` derives the patience parameter from
a measurement: it records the longest run of states that change nothing along
the reference line, since chain resolutions produce such runs.

Reaching one subgoal consumes the resources the next one needs. The plain
heuristic — target cards currently on the field — is flat over roughly 90 % of a
line, leaving nothing to descend. Two mechanisms address this:

- a **recipe graph**, learned from observed summons and seeded from card text
  and declared operators, counting the summons still required rather than the
  cards still missing;
- a **material balance** solved as a small linear program whose firing vector
  `x*` yields intermediate subgoals that exist from the first card placed. The
  simplex is two-phase with Bland's rule, and self-tests on instances with known
  solutions run before it is used.

Neither mechanism prunes. A product with no known recipe is worth 1 rather than
infinity, so the worst case falls back to the flat heuristic.

---

## Search algorithms

Rollouts first, then a finisher. Rollouts make progress towards the board;
sampling rarely reaches the last few decisions, so a complete search closes the
line.

**NRPA** (Nested Rollout Policy Adaptation). One weight per move identity,
softmax sampling, adaptation towards the best sequence at each level. Move
identity is semantic — the cards a choice engages — so a policy learned on one
deck transfers to another.

**Levin Tree Search**, used as the finisher. Complete best-first search ordered
by `d(n)/pi(n)`, where `pi` is the product of the policy's probabilities along
the path. The number of expansions before a solution is found is bounded by the
policy's quality.

**Go-Explore archive.** The K best distinct states are kept together with the
path that reaches them, and the finisher roots itself there. Returning to a
state costs a prefix replay plus an arena restore. In other Go-Explore
implementations that return step is the expensive one.

**Options.** Macros mined from a corpus of solved lines and offered as a single
sampling unit. They reduce the number of decisions rather than the branching
factor: a 160-decision line becomes roughly 20 decisions when eight collapse
into one action. Selection is by Levin loss, which derives the catalogue size
from the corpus. Mining can run inside the search.

Every candidate is verified before being written: replayed from scratch in a
fresh duel, board compared, constraints re-checked. Deduplication and
canonicalisation give no guarantee that an answer sequence rebuilds the board,
so only verified candidates are written out.

---

## Repository layout

| File | Contents |
|---|---|
| `main.cpp` | CLI, the three search drivers, all reports, instrumented replay |
| `search.h` / `search.cpp` | transposition, novelty, NRPA, LTS, archive, recipe graph, landmarks |
| `arena.h` / `arena.cpp` | snapshottable memory arena, allocator, hot path profiler |
| `duel.h` / `duel.cpp` | wrapper around one statically linked `ocgcore` duel |
| `enumerate.h` / `.cpp` | decoding `MSG_SELECT_*` into legal answers; equivalence classes |
| `prompt.h` / `prompt.cpp` | branching-factor accounting per prompt type |
| `replay.h` / `replay.cpp` | reading and writing `.yrpX` / `.yrp1` |
| `assets.h` / `assets.cpp` | serving `cards.cdb` rows and Lua scripts to the core |
| `operators.h` / `.cpp` | static analysis of the deck's Lua scripts; LP over the operator table |
| `lp_fuzz_cases.inc` | LP instances solved in exact rationals by sympy, compiled in as self-tests |
| `premake5.lua` | build definition: solver, ocgcore, Lua, LZMA |
| `gabarits/` | replay template used by the examples |
| `tools/` | dependency fetch, release build, replay inspection, sympy proofs |
| `docs/` | design notes, flag inventory, session reports |

`docs/combo-solver-design.md` is the long-form record of trade-offs,
measurements and abandoned approaches. `docs/drapeaux.md` is the flag inventory,
classified as judged, refuted, or never judged.

---

## Conventions

**Defaults carry the behaviour.** The project rule: a defect is fixed in the
code, never behind a flag. A mechanism gets a flag only while it is being
measured; once judged it becomes the default and the flag inverts to `--no-…`
so the A/B stays reproducible.

**Mechanisms report their own activity.** `--assign-bias` was inert for two
sessions while the run reported it as active. Every mechanism now prints a
liveness counter, and `!! INERT` invalidates a run.

**Measurements are judged on µs per call and on structural outputs.**
Rollout and state counters vary by 10 to 18 % at a fixed seed, which
exceeds most of the effects being measured.

---

## Limitations

**Conversion is unreliable on the harder benchmark.** At an identical seed and
command, a judge returned solution counts of 0, 0, 0, 89 and 2239 over five
executions. The dispersion comes from a rare event, not from measurement noise.
Comparisons on that benchmark require N runs per arm and must be read as the
proportion of runs that succeed.

**Most flags have no verdict.** Of the 133 flags, a large share is written and
instrumented but never judged. `docs/drapeaux.md` classifies each one.

**Report language.** The diagnostic report is printed in French. Code comments,
`--help` and the constraint grammar are English. `docs/` is French throughout.

**Measurement harnesses are not in this repository.** The scripts that produced
the figures quoted here are one file per A/B, each hard-coding a machine path,
usually a seed and a budget, and about a third of them an output directory left
by an earlier session. `docs/` cites them as `measurements/<name>.ps1` so the
provenance of a number remains traceable. `tools/` contains only what builds or
inspects the project.

**No license file.**

---

## References

Papers implemented in this codebase.

| Idea | Paper |
|---|---|
| NRPA / GNRPA | Cazenave, *Nested Rollout Policy Adaptation*; [arXiv:2003.10024](https://arxiv.org/abs/2003.10024) |
| Limited repetitions | [arXiv:2401.10420](https://arxiv.org/abs/2401.10420) |
| Levin Tree Search, PHS* | [arXiv:2103.11505](https://arxiv.org/abs/2103.11505) |
| sqrt-LTS re-rooting | [arXiv:2412.05196](https://arxiv.org/abs/2412.05196) |
| Iterated Width, Rollout-IW | Lipovetzky & Geffner; [arXiv:1801.03354](https://arxiv.org/abs/1801.03354) |
| Go-Explore | [arXiv:2004.12919](https://arxiv.org/abs/2004.12919) |
| Hindsight relabelling (HER) | Andrychowicz et al., NeurIPS 2017 |
| Policy learning from solved games | [arXiv:2401.10431](https://arxiv.org/abs/2401.10431) |
| Macro-operators, option selection by Levin loss | [arXiv:1109.2154](https://arxiv.org/abs/1109.2154), [arXiv:1810.09145](https://arxiv.org/abs/1810.09145), [arXiv:2410.11262](https://arxiv.org/abs/2410.11262) |
| Learning macros during search (Marvin) | [arXiv:1110.2736](https://arxiv.org/abs/1110.2736) |
| Permutation statistic / MCPS | [arXiv:2510.06381](https://arxiv.org/abs/2510.06381) |
| Generalised landmarks | [arXiv:2508.21564](https://arxiv.org/abs/2508.21564) |
| Retrosynthesis as search (Retro*, DESP) | [arXiv:2006.15820](https://arxiv.org/abs/2006.15820), [arXiv:2407.06334](https://arxiv.org/abs/2407.06334) |
| Action selection as optimisation | [arXiv:2010.12001](https://arxiv.org/abs/2010.12001) |
| Red-black relaxation | Katz, Hoffmann & Domshlak |
