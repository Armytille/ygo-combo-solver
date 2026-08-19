# combosolver

A headless combo solver for [EDOPro](https://github.com/edo9300/edopro) replays.

Give it a `.yrpX`. It replays the recorded line inside a real `ocgcore` duel,
captures the board the player ends their turn on, and then searches for other
ways to reach that same board — from the same duel, from another decklist, or
from a hand you write out by hand. What it writes back are `.yrp` replays you
can open in EDOPro.

There is no rules engine of our own here. Every legality question is answered by
`ocgcore` itself, and every move the solver considers is a move the core offered
at a `MSG_SELECT_*` prompt. What the project adds is the ability to *back up*: a
memory arena that snapshots and restores a live duel in a fraction of a
millisecond, which is what turns "replay one line" into "search a space of
lines".

---

## Contents

- [What it does](#what-it-does)
- [Requirements](#requirements)
- [Build](#build)
- [Usage](#usage)
- [The constraint grammar](#the-constraint-grammar)
- [The flag you cannot forget](#the-flag-you-cannot-forget)
- [What a run prints](#what-a-run-prints)
- [How it works](#how-it-works)
- [The search stack](#the-search-stack)
- [Repository layout](#repository-layout)
- [State of the work](#state-of-the-work)
- [References](#references)

---

## What it does

Four modes, all driven from the same binary.

**1. Instrumented replay (no search).** The default. It replays the line, prints
what the core offered at every decision, captures the target board, and then
runs its own self-checks: restore fidelity and a snapshot stress test.

```bash
combosolver.exe duel.yrpX --scriptdir <scripts> --workdir <edopro-install>
```

**2. A better line to the same board, in the same duel.** Bounded-discrepancy
search seeded on the recorded line: follow the reference and allow at most *k*
deviations. At zero deviations it replays the reference, so it always finds at
least one solution — which makes "no solution" a defect signal rather than a
possible result.

```bash
combosolver.exe duel.yrpX --scriptdir <scripts> --solve --outdir solutions
```

**3. The same board from another duel** — another deck, another hand, another
seed. The recorded answers designate nothing there, so what travels is the
line's *intent*: each decision is identified by the cards it engages, not by the
index it happened to have.

```bash
# from another replay
combosolver.exe ref.yrpX --scriptdir <scripts> --start other.yrpX --outdir solutions

# from a decklist plus an opening hand, with no starting replay
combosolver.exe ref.yrpX --scriptdir <scripts> \
    --deck "<edopro-install>\deck\Lunalight.ydk" \
    --hand "Assault Zone|Ash Blossom|Ash Blossom|Ash Blossom" \
    --outdir solutions
```

The hand is forced through `DUEL_PSEUDO_SHUFFLE` and then **verified** on a
throwaway duel before any search starts: which end of the deck the core draws
from is not guessed, it is checked, and a mismatch is a hard error rather than a
search on a hand you only believe you have.

**4. Goal-only mode.** No reference line at all: the replay is demoted to a duel
template (flags, life points, opponent deck) and the target board is written out
by hand.

```bash
combosolver.exe template.yrpX --scriptdir <scripts> \
    --deck Lunalight.ydk --hand "57103969|57103969|57103969" \
    --no-ref --target 54701958 --target 54701958 --target 54701958 \
    --target "90590304@DEF" --max-decisions 700
```

**As a judge.** With the same constraint flags and no `--solve`, the tool checks
whether *any* replay — one it produced, or one played by hand — respects the
discipline you asked for.

```bash
combosolver.exe solutions/solution_00.yrp --scriptdir <scripts> \
    --guard "5:Crystal Wing|Zalen@field+Junk Signal@hand"
```

---

## Requirements

Windows x64, Visual Studio 2022 build tools, PowerShell, Python 3 for the
verification scripts in `tools/` (sympy, for the ones that check a formula).

A working EDOPro installation, pointed at by `--workdir`. It must contain
`cards.cdb`, `expansions/`, `repositories/` and the card scripts — the solver
reads them the way the client does, and never writes into that tree.

There is no default: the binary contains no absolute path. Either pass
`--workdir` on every run, or set it once in the environment:

```powershell
setx COMBOSOLVER_WORKDIR "C:\Games\ProjectIgnis"
```

`--workdir` wins over the variable. With neither, the run refuses to start
rather than guess.

External dependencies, all outside this repository:

| Path | Role |
|---|---|
| `../edopro/ocgcore` | the git repository ocgcore is extracted from, at a chosen commit |
| `../edopro/gframe/lzma` | LZMA sources (compressed replays) |
| `../deps/ocgcore` | the extracted, patched copy, produced by the script below |
| `../deps/scripts_<date>` | a frozen export of the card scripts |
| `../vcpkg` | sqlite3, `x64-windows-static` |

---

## Build

```powershell
.\tools\fetch_solver_deps.ps1        # extract and patch ocgcore, lua and the scripts
..\premake5\premake5.exe vs2022 --vcpkg-root=..\..\vcpkg
MSBuild build\combosolver.sln /p:Configuration=Release /p:Platform=x64
```

The binary lands in `bin\Release\combosolver.exe`.

`fetch_solver_deps.ps1` never writes into the machine's EDOPro installation. It
extracts one ocgcore commit plus the matching `lua/src` commit into
`../deps/ocgcore`, applies the patches below, and freezes a card script export
next to it.

### The ocgcore patches

Three files are modified in the extracted copy, and each one is load-bearing.

| Target | Why |
|---|---|
| `lua/luaconf-customize.h` | deterministic string hash seed, and the declaration of the arena allocator hook |
| `lua/src/lauxlib.c` | routes the Lua heap into the arena — without it the duel's state cannot be captured at all |
| `ocgapi.cpp` / `ocgapi.h` | adds `OCG_DuelQueryProcessorState`: phase, resolution stack, current chain, once-per-turn counters |

The C++ side of the core is **not** patched: its allocations are captured by a
global `operator new` overload on the solver's side.

The processor state matters more than it looks. The public API only exposes the
zones, so two instants in the middle of the same chain resolution — same field,
same hand, same prompt — hash to the same value without it. They then get merged
by the transposition table and the combo branch is pruned at the start, silently.

---

## Usage

`combosolver.exe --help` prints all 133 flags with their defaults. The ones you
actually need to know:

| Flag | Meaning |
|---|---|
| `--workdir <dir>` | EDOPro installation (or `COMBOSOLVER_WORKDIR`) |
| `--scriptdir <dir>` | card script set, highest priority first (see below) |
| `--player <0\|1>` | whose turn is being optimised |
| `--outdir <dir>` | where the replays are written |
| `--solve` | search instead of only replaying |
| `--solve-ms <ms>` | search time budget |
| `--threads <n>` | search workers (default: every core) |
| `--start <replay>` / `--deck <f.ydk>` | rebuild the board from another duel or decklist |
| `--target <card[@ATK\|DEF]>` | build the target board from scratch |
| `--optimize` | keep searching for cheaper lines instead of stopping at the first |
| `--profile` | hot path profile, per phase |

For reproducible measurements, `--max-rollouts` replaces the wall-clock budget
with a rollout count. At `--threads 1` two executions then do exactly the same
work; the wall-clock budget is what made runs at an identical seed diverge.

```powershell
combosolver.exe ... --threads 1 --max-rollouts 20000 --max-nodes 500000 `
    --solve-ms 900000   # time must never be the binding bound here
```

This is an attribution instrument, not the production mode: a single worker
costs roughly five to six times the throughput.

---

## The constraint grammar

Constraints are not filters applied after the fact — most of them remove the
branch from the enumeration.

**Zones**: `hand`, `field`, `grave` (or `graveyard`), `banished`, `extra`.
Default `field`. The French spellings the earlier command lines used
(`main`, `terrain`, `cimetiere`, `banni`) are still accepted.

**Attributes** (`--material`): `light`, `dark`, `earth`, `water`, `fire`,
`wind`, `divine`.

```bash
# the n-th summon must be one of these cards
--summon "5:Zalen|Crystal Wing"

# from the 5th summon on, at every OPPONENT response window at least one
# clause must hold. A clause is a conjunction of card@zone atoms.
--guard "5:Crystal Wing|Zalen@field+Junk Signal@hand"

# the guard stops being required once the opponent's hand is down to 2 cards
--guard-off "opphand<=2"

# a state predicate instead of a card: N opponent cards banished
--guard "5:Dis Pater@field+oppbanished>=1"

# the line must resolve this effect twice, activated FROM THE FIELD
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

Two distinctions that are easy to get wrong:

- `--guard` says "when the window opens, a counter is available". It does not
  say the guard *is* the n-th summon; it may already be in play.
- `--resolve` is checked **at the goal**, not along the line: a conforming board
  without the resolutions is not a solution, and the search keeps going.
- `@zone` on `--resolve` restricts the **activation** zone. Without `@field`,
  an Omega that rips from the field and an Omega effect resolving from the
  graveyard both count, which is a measured false positive.

### Opponent test

`--guard` is a static proxy: it asserts that an answer is available. `--fire`
plays the threat for real. The card is added to the opponent's hand and
activated at every window where it is legal — one attempt per window — and the
search must close the board back up from the post-injection state.

```bash
combosolver.exe duel.yrpX --scriptdir <scripts> \
    --fire "27204311" --fire-spare "Junk Signal" --fire-ms 60000 \
    --resolve "PSY-Framelord Omega@field:2"
```

`--fire-spare` names cards that may be spent answering: the target board without
them is also accepted at the goal. `--fire-open` restricts injection to windows
where the chain is empty, so the threat *starts* a chain instead of being
chained onto our own effects.

Note that a hand test start gives the opponent no hand at all. With no playable
card across the table the core never opens a response window, the guard is
satisfied vacuously and a handrip rips nothing — `--opp-hand` is what gives it
something to work with, and the replays produced only replay with the same
`--opp-hand`.

---

## The flag you cannot forget

**`--scriptdir` is mandatory in practice.**

A `.yrpX` only replays faithfully with the core *and* the Lua card scripts
contemporary with its recording. With the wrong script set the replay diverges
**silently**: the core asks a different question, the recorded answer becomes
invalid, and the tool goes on printing normal-looking measurements.

The one detector is the `MSG_RETRY` counter in the report. On the reference
replay: 218 retries against the live scripts, **0** against the pinned export
that `fetch_solver_deps.ps1` produces.

The same applies to the core itself, which is why the fetch script pins a
commit rather than following `HEAD`. A replay recorded by an older client needs
an older core, even though its scripts update themselves.

---

## What a run prints

Every run, search or not, ends with self-checks. Their point is that a wrong
number is worse than no number: if the replay does not reproduce, or if the
snapshot does not restore the state exactly, nothing measured afterwards means
anything.

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

(Reproduce it with `combosolver.exe gabarits\etalon_a_lunalight.yrp --workdir
<your EDOPro>`. The report text is still French; see
[State of the work](#state-of-the-work).)

That last block is the one that pays for everything else. Restoring costs
0.12 ms where re-simulating from the root costs 16.9 ms, and the ratio is what
makes searching viable at all.

Beyond the self-checks, a run reports what it *cut* and whether each mechanism
was actually **alive**. A counter that is not printed is not an instrument, and
several mechanisms in this repository spent whole sessions switched off while
announcing themselves as active. `!! INERT` in a report means the arm is
disposable before it is launched, not after.

---

## How it works

### The arena

The OCG API offers no state cloning, and a hand-written serialiser is out of
reach: at a `MSG_SELECT_*`, Lua coroutines sit suspended in the middle of an
effect resolution, with their stacks and their upvalues. So the project does not
save the game — it saves **the memory**.

All the duel's mutable memory is confined to an address range under our control:
the Lua heap through the allocator passed to `lua_newstate`, and the core's C++
objects through the global `operator new` overload. A restore happens **at the
same base address**, so every absolute pointer stays valid with no relocation
and the duel never knows it was restored.

Restores are the frequent operation, and the cheap one: only the pages the child
dirtied are copied back. Under Windows that uses `GetWriteWatch`; the WebAssembly
port replaces it with a software write barrier (`arena.cpp`) whose completeness
is checked by a verifier rather than assumed.

An allocation that does not fit in the arena goes to the host heap and cannot be
restored. That is not a statistic but a **stop condition**: the arena is marked
poisoned and the worker aborts, because everything it measured afterwards would
describe a duel the restore can no longer reconstitute.

### Searching the state graph, not the action tree

The action tree is not enumerable at any speed — 10^97 along the reference line
alone. What makes exploration possible is searching the **state graph**:
activating A then B and B then A converge on the same node, and the
transposition table merges them.

The transposition key covers the visible zones, the prompt payload (which is
where the once-per-turn counters hide) and the processor state. **Under-hashing
is the failure mode to watch**: it makes solutions disappear without saying so.
The report therefore counts merges along the reference line, whose states are
pairwise distinct by construction.

Equivalence is chosen, not incidental, and each choice is written down:

- Two boards that differ only by a battle position are the **same board** for
  the goal test; the state digest keeps the full position.
- Two artworks of one card are the same card (`QUERY_ALIAS`), so a line recorded
  on one deck recognises itself in another.
- Deck order **is** a game state (draws, excavations and flips read it), so it is
  hashed as is; hand and graveyard order is not, so those are sorted.

### Novelty and serialisation

The transposition table only merges *identical* states, yet two lines differing
by one card in the graveyard are distinct and not distinctly interesting.
Novelty pruning (Iterated Width) keeps a state only when it makes at least one
atom true that has never been true before. The patience is not guessed: the
`--width` mode measures the longest mute run along the reference line first,
because chain resolutions go through states that change nothing on the board.

The harder problem is that reaching a subgoal consumes what the next one needs.
The plain heuristic — how many target cards are on the field — is flat over
roughly 90 % of a line, so there is nothing to descend. Two mechanisms attack
that:

- a **recipe graph**, learned from the summons actually observed (and seeded
  from card text and from the declared operators), which counts the summons
  still to be made rather than the cards still missing;
- a **material balance** solved as a small linear program, whose firing vector
  `x*` yields intermediate subgoals that exist from the first brick placed. The
  simplex is two-phase with Bland's rule and self-tests on instances with known
  solutions before it is allowed to serve.

Neither ever prunes. A product with no known recipe is worth 1, never infinity,
so at worst the landscape falls back to the flat heuristic and nothing is lost.

---

## The search stack

Rollouts, then a finisher. Rollouts know how to climb; the last step is a needle
sampling does not find.

- **NRPA** (Nested Rollout Policy Adaptation): one weight per move *identity*,
  softmax sampling, adaptation towards the best sequence at each level. The move
  identity is semantic — the cards a choice engages — so a policy learned on one
  deck means something on another.
- **Levin Tree Search** as the finisher: a complete best-first search ordered by
  `d(n)/pi(n)`, where `pi` is the product of the policy's probabilities along
  the path. The number of expansions before finding a solution is bounded by the
  policy's quality, which is what sampling can never promise.
- **Go-Explore archive**: the K best *distinct* states are kept with the path
  that reaches them, and the finisher takes its roots there. Returning to a
  state is cheap here (replay the prefix, restore the arena), which is exactly
  the part Go-Explore normally has to pay for.
- **Options**: macros mined from a corpus of solved lines and offered as a
  single sampling unit. They attack the exponent rather than the base — a
  160-decision line becomes a ~20-decision one when eight decisions collapse
  into one action. Selection is by Levin loss, so the catalogue's size is a
  result rather than a parameter, and mining can run online, inside the run.

Every candidate is **verified before it is written**: replayed from scratch in a
fresh duel, board compared, constraints re-checked. A search that deduplicates
and canonicalises gives no a priori guarantee that its answer sequence rebuilds
the board, so only what holds is written out.

---

## Repository layout

| File | Role |
|---|---|
| `main.cpp` | CLI, the three search drivers, every report, and the instrumented replay |
| `search.h` / `search.cpp` | the search itself: transposition, novelty, NRPA, LTS, archive, recipe graph, landmarks |
| `arena.h` / `arena.cpp` | the snapshottable memory arena, its allocator, the hot path profiler |
| `duel.h` / `duel.cpp` | wrapper around one statically linked `ocgcore` duel |
| `enumerate.h` / `.cpp` | decoding a `MSG_SELECT_*` into legal answers, and the equivalence classes |
| `prompt.h` / `prompt.cpp` | branching-factor accounting per prompt type |
| `replay.h` / `replay.cpp` | reading and writing `.yrpX` / `.yrp1` |
| `assets.h` / `assets.cpp` | serving `cards.cdb` rows and Lua scripts to the core |
| `operators.h` / `.cpp` | static analysis of the deck's Lua scripts, and the LP over the operator table |
| `premake5.lua` | build definition (solver, ocgcore, Lua, LZMA) |
| `gabarits/` | a small replay used by the smoke run above |
| `tools/` | dependency fetch, replay inspection, and the sympy proofs of the formulas in the code |
| `docs/` | design notes, flag inventory, session reports |

`docs/combo-solver-design.md` is the long form: the trade-offs, the
measurements, and the dead ends, including the abandoned ones and why.
`docs/drapeaux.md` is the flag inventory — judged, refuted, never judged.

---

## State of the work

This is a research tool, and its README should say what it does not do.

**It converts, but not reliably.** On the harder of the two benchmarks the
solver writes solutions in some runs and none in others at an identical seed;
the design notes record a judge returning `0, 0, 0, 89, 2239` over five
executions of the same command. That is a rare event, not measurement noise. Any
comparison on that benchmark needs N runs per arm and must be read as a
*proportion* of runs that succeed, never as one counter's value.

**Most flags have never been judged.** There are 133 of them.
`docs/drapeaux.md` classifies each one, and the honest total of "written,
instrumented, no verdict" is large. A flag that has never been judged is a debt,
not an option.

**The default must be good.** A user cannot be expected to know which four flags
make the solver work; if that is what it takes, the defect is in the defaults.
The rule the project now follows: a defect fix is never a flag; a mechanism is a
flag only for as long as it takes to measure it, after which it becomes the
default and the flag goes negative (`--no-…`) so the A/B stays replayable.

**A mechanism must prove it is switched on before its effect is measured.**
`--assign-bias` was completely inert for two sessions while the run printed that
it was acting. Every mechanism now prints its own liveness, and a report
carrying `!! INERT` invalidates the arm before the budget is spent.

**Known gaps in this repository as published.** The diagnostic report is still
printed in French — the code comments, the `--help` text and the constraint
grammar are English; the report text is not yet. The design notes and session
reports under `docs/` are French throughout.

The per-session measurement harnesses that produced the figures quoted here
are **not in this repository**. One file per A/B, each hard-coding this
machine's paths, usually a seed and a budget, and a third of them an output
directory left by an earlier session: reproducible on the machine that ran
them, on no other. `docs/` names them as `measurements/<name>.ps1` so the
provenance of a number is still traceable, but the files themselves are
ignored. What remains in `tools/` builds or inspects the project.

There is no license file yet.

---

## References

Papers that are actually implemented here, not a reading list.

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
