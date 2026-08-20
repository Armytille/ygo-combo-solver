# ygo-combo-solver

Searches for Yu-Gi-Oh! combo lines inside a recorded duel.

Given a replay, the solver determines the board the player ended their turn on
and searches for other move sequences that reach it: lines that spend fewer
cards, lines that start from a different opening hand or a different deck, and
lines that still work when the opponent interrupts. It also accepts a board
description and a decklist with no replay, and searches for a way to build that
board. Results are written as replay files that open in EDOPro.

The solver runs the same engine EDOPro runs, so every line it produces is legal
and playable.

**[Download the latest release](https://github.com/Armytille/ygo-combo-solver/releases/latest)**

Windows x64. One executable, no dependencies. Requires an EDOPro installation.

---

## Contents

- [Setup](#setup)
- [Quick start](#quick-start)
- [Usage by task](#usage-by-task)
- [Constraints](#constraints)
- [Flags](#flags)
- [Reading the output](#reading-the-output)
- [How it works](#how-it-works)
- [Limitations](#limitations)
- [Build from source](#build-from-source)
- [License](#license)
- [References](#references)

---

## Setup

The release archive contains `combosolver.exe`, a sample replay under
`gabarits/`, this document, and the licence files. The executable runs from any
directory. Output goes to `--outdir`, resolved against the current directory.

Cards and card scripts are read from an EDOPro installation. The solver only
reads that directory. Set its location once:

```powershell
setx COMBOSOLVER_WORKDIR "C:\Games\ProjectIgnis"
```

`--workdir <dir>` overrides the variable. Without either, the solver exits with
an error.

---

## Quick start

```powershell
# 1. verify that the replay reproduces
combosolver.exe duel.yrpX --scriptdir <card-scripts>

# 2. search for a cheaper line to the same board, five minutes
combosolver.exe duel.yrpX --scriptdir <card-scripts> `
    --solve --optimize --solve-ms 300000 --outdir solutions
```

Step 1 comes first on every new replay. A replay reproduces only with the card
scripts contemporary with its recording. Under a different script set the engine
issues different prompts, the recorded answers stop applying, and the run
continues on a different duel with no error message. The `MSG_RETRY` counter in
the report must read 0.

`--scriptdir` is repeatable, highest priority first. It also disables the
automatic scan of EDOPro's `repositories/` directory, so pass those directories
explicitly when the deck requires them.

---

## Usage by task

The first argument is always a replay. Its role depends on the flags:

| Flags | The replay provides | The duel played is |
|---|---|---|
| none | the line to replay and measure | its own |
| `--solve` | the line, and the target board | its own |
| `--start other.yrpX` | the line, and the target board | the one in `other.yrpX` |
| `--deck d.ydk --hand …` | the line, and the target board | built from the decklist |
| `--no-ref --target …` | the duel setup only | built from the decklist |

`--start`, `--deck` and `--fire` enable the search on their own.

### 1. Verify a replay and measure the line

```powershell
combosolver.exe duel.yrpX --scriptdir <card-scripts>
```

No search is performed. The report gives, in this order:

1. `MSG_RETRY`, which must be 0.
2. answers consumed against answers recorded.
3. the cost of the line, with an itemised list of the cards it consumes. That
   list determines whether another deck holds the resources for the same combo.

### 2. Search for a cheaper line to the same board

```powershell
combosolver.exe duel.yrpX --scriptdir <card-scripts> `
    --solve --optimize --solve-ms 300000 --outdir solutions
```

`--solve` stops at the first line found. `--optimize` continues: it ranks lines
by burned cards, keeps searching after each solution, and allows a line to run
past the board so that spent cards can be recovered.

Solutions are written to `--outdir`, sorted, best first. The cost appears in the
filename: `solution_00_b1_a9.yrp` burned one card and used nine actions. The
reference line's own burn count comes from task 1.

`--burn-limit <n>` declares a burn count already achieved, so the search only
looks below it. `--solve-ms` is a budget; the run reports the best line found
when it expires.

### 3. Reach the same board from a different hand or deck

```powershell
# from a decklist and a chosen opening hand
combosolver.exe ref.yrpX --scriptdir <card-scripts> `
    --deck "C:\Games\ProjectIgnis\deck\Lunalight.ydk" `
    --hand "Assault Zone|Ash Blossom|Ash Blossom|Ash Blossom" `
    --solve-ms 600000 --outdir solutions

# from another recorded duel, with its deck, hand and shuffle
combosolver.exe ref.yrpX --scriptdir <card-scripts> `
    --start other.yrpX --solve-ms 600000 --outdir solutions
```

Recorded answers are prompt indices, and in another duel they designate
different cards. The solver re-reads the reference line by the cards each
decision engages, which carries the combo's intent across.

`--hand` accepts card codes or name fragments separated by `|`. The hand is
verified in a throwaway duel before the search starts. `--board-add` and
`--board-remove` modify the captured board.

### 4. Build a board described by hand

For combos with no recording. The replay supplies the duel setup only.

```powershell
combosolver.exe template.yrpX --scriptdir <card-scripts> `
    --no-ref `
    --deck "C:\Games\ProjectIgnis\deck\Lunalight.ydk" `
    --hand "57103969|57103969|57103969" `
    --target 54701958 --target 54701958 --target 54701958 `
    --target "90590304@DEF" `
    --max-decisions 700 --solve-ms 900000 --outdir solutions
```

`--target` is repeatable and repeats count: three identical entries require
three copies. `@DEF` sets defence position. The described board is a minimum, so
a line that also produces something else satisfies it. `--max-decisions` has no
reference line to derive its default from in this mode and should be set
generously.

Guidance flags for this mode:

| Flag | Effect |
|---|---|
| `--summon-min "card:3"` | require three summons of a card, and steer the search towards them |
| `--resolve "card@field:2"` | require an effect to resolve twice, activated from the field |
| `--hint "card"` | bias sampling towards lines that engage a card |
| `--recipes 1` | measure distance in summons still required |

### 5. Require the line to survive interaction

`--guard` requires that from the n-th summon onward, at every point where the
opponent can act, at least one listed answer is held.

```powershell
combosolver.exe duel.yrpX --scriptdir <card-scripts> `
    --solve --optimize --solve-ms 600000 `
    --guard "5:Crystal Wing|Zalen@field+Junk Signal@hand" `
    --guard-off "opphand<=2" --outdir solutions
```

`--fire` adds a card to the opponent's hand and plays it at every point where
that is legal. The search then rebuilds the board from the resulting state.

```powershell
combosolver.exe duel.yrpX --scriptdir <card-scripts> `
    --fire 27204311 --fire-spare "Junk Signal" --fire-open --fire-ms 60000 `
    --outdir solutions
```

`--fire-spare` names a card that may be spent answering; a board missing it
satisfies the goal. `--fire-open` restricts injection to points where the chain
is empty. `--fire-bake` writes the card into the replay header so the output
plays back in EDOPro without extra flags.

A solo hand test gives the opponent no cards. No response window opens, and
`--guard` is satisfied vacuously. `--opp-hand` supplies the opponent with cards.
Replays produced under `--opp-hand` require the same value to play back.

### 6. Check a line against a set of rules

Constraint flags without `--solve` run the tool as a checker, on solver output
and on hand-played replays alike.

```powershell
combosolver.exe solutions\solution_00_b1_a9.yrp --scriptdir <card-scripts> `
    --guard "5:Crystal Wing|Zalen@field+Junk Signal@hand" `
    --resolve "PSY-Framelord Omega@field:2"
```

### 7. Compare two configurations

The default budget is wall time, so two runs of one command perform different
amounts of work. `--max-rollouts` replaces it with a fixed amount of work.

```powershell
combosolver.exe ... --threads 1 --seed 888 `
    --max-rollouts 20000 --max-nodes 500000 --solve-ms 900000
```

`--threads 1` divides throughput by five to six. Run the two configurations
sequentially.

---

## Constraints

Constraints act during the search: a move that violates one is removed from the
enumeration.

Zones: `hand`, `field`, `grave`, `banished`, `extra`, default `field`.
Attributes for `--material`: `light`, `dark`, `earth`, `water`, `fire`, `wind`,
`divine`.

```bash
--summon "5:Zalen|Crystal Wing"      # the 5th summon must be one of these

# from the 5th summon on, hold one of these at every opponent window.
# '+' requires all of them at once, '|' separates alternatives.
--guard "5:Crystal Wing|Zalen@field+Junk Signal@hand"
--guard-off "opphand<=2"             # drop the requirement below 2 cards in hand
--guard "5:Dis Pater@field+oppbanished>=1"   # a condition on the game state

--resolve "PSY-Framelord Omega@field:2"      # resolve this twice, from the field
--summon-min "Liger Dancer:3"                # summon this three times
--material "Chaos Angel:light"               # summon it using a LIGHT material

--no-activate "Duel Evolution - Assault Zone"  # never activate from the field
--no-chain "Crystal Wing"                      # never chain onto your own effects
--mp1-only                                     # keep the combo in Main Phase 1
```

Points of detail:

- `--guard` requires an answer to be available when the window opens. The guard
  card may already be on the field before the n-th summon.
- `--resolve` and `--summon-min` are verified at the goal. A board matching the
  target without them is rejected and the search continues. At most four entries
  in total.
- `@zone` on `--resolve` fixes the activation zone. Omitted, a card with effects
  in two zones satisfies the requirement from either one.

Cards named in `--resolve` and `--summon-min` receive a sampling bias. `--watch`
names a card for observation, with no constraint and no bias.

---

## Flags

`combosolver.exe --help` lists all 133 with their defaults. The tasks above use
these:

| Flag | Meaning |
|---|---|
| `--workdir <dir>` | EDOPro installation, or set `COMBOSOLVER_WORKDIR` |
| `--scriptdir <dir>` | card script directory, highest priority first; repeatable |
| `--outdir <dir>` | destination of the replays found |
| `--player <0\|1>` | whose turn is optimised |
| `--solve` | search for a line to the target board |
| `--start <replay>` | start from another duel |
| `--deck <file.ydk>` / `--hand <cards>` | start from a decklist and a chosen hand |
| `--target <card[@DEF]>` | describe the board; repeatable, repeats count |
| `--board-add` / `--board-remove` | modify the captured board |
| `--no-ref` | ignore the recorded line |
| `--optimize` | keep improving after the first line |
| `--burn-limit <n>` | a burn count already achieved |
| `--solve-ms <ms>` | search time, default two minutes |
| `--threads <n>` | workers, default every core |
| `--max-decisions <n>` | longest line allowed |
| `--seed <n>` | reuse a printed seed to repeat a run |
| `--verbose` | print every decision |

---

## Reading the output

Every run ends with self-checks, search or no search. The measurements are valid
only when the replay reproduces and the self-checks pass.

| Section | Contents | Expected |
|---|---|---|
| Loading | cards and script directories found | non-zero, and the scripts intended |
| Replay result | answers consumed, `MSG_RETRY` | all consumed, `MSG_RETRY` 0 |
| Decision points | options the engine offered, by prompt type | sizes the search space |
| Cost of the line | cards consumed, burned, actions, decisions | the baseline for task 2 |
| Target board | the board captured, card by card | the board expected |
| Self-checks | save and restore fidelity, stress cycles | all pass |

A line's cost is three numbers, compared in this order: burned cards, in the
graveyard or banished; actions, meaning summons and activations; decisions,
meaning prompts answered. The cards on the final board are fixed by the target,
so burned cards are the variable part.

After a search the report adds the ranking of the lines found, the number
examined and the number written. A flag with no effect on the run is marked
`!! INERT`.

The report is in French. The command line, the help text and this document are
in English.

---

## How it works

**Rules.** The solver contains no implementation of the game. It runs the engine
and, at each prompt, enumerates the answers the engine reports as legal. Each
candidate line is replayed in a fresh duel and re-checked before being written.

**Search space.** A turn's moves can be ordered in a very large number of ways,
and many orderings reach the same position. The solver identifies equal
positions and expands each one once.

**Snapshots.** Trying an alternative move requires returning to an earlier point
in the duel. The solver snapshots the engine's memory and restores it, which
costs orders of magnitude less than replaying the turn from the start.

**Policy.** The search samples lines, maintains a policy weighted by the
results, and adapts it towards the best line found. A move is identified by the
cards it engages, so a policy learned on one deck applies to another.

**Finisher.** Sampling reaches the neighbourhood of the target board and seldom
completes the last decisions. The solver stores its best positions and runs a
complete search from them, ordered by the learned policy.

**Heuristic.** The number of target cards on the field stays flat for most of a
combo, since the board fills at the end. The solver derives what each summon
consumes and produces, and measures distance as the number of summons still
required.

---

## Limitations

- Difficult boards do not convert on every run. Allow a full budget and repeat
  the run before concluding that a board is unreachable.
- The default domain is a single turn. `--turns 2` covers lines that cross into
  the opponent's turn, with less coverage.
- A replay whose card script set is unavailable cannot be reproduced.
- 133 flags exist. The tasks above use about fifteen.
- The report is in French.

---

## Build from source

Windows x64, Visual Studio 2022 build tools, PowerShell.

```powershell
.\tools\fetch_solver_deps.ps1
..\premake5\premake5.exe vs2022 --vcpkg-root=..\..\vcpkg
MSBuild build\combosolver.sln /p:Configuration=Release /p:Platform=x64
```

`fetch_solver_deps.ps1` retrieves the engine at a pinned commit, applies the
patches the solver requires to observe and snapshot a duel, and freezes a
matching card script export. It does not modify the EDOPro installation.

`.\tools\build_release.ps1 -Workdir <dir>` produces the optimised,
self-contained executable and the release archive.

---

## License

GNU Affero General Public License version 3 or later. Full text in
[LICENSE](LICENSE), per-component notices in [NOTICE](NOTICE).

`combosolver.exe` statically links
[ocgcore](https://github.com/edo9300/ygopro-core), which is AGPL-3.0-or-later,
so the combined work carries the same terms. Recipients of a copy, including
over a network, are entitled to the corresponding source.

Card data and card scripts are read from the EDOPro installation at run time and
are not redistributed. Yu-Gi-Oh! is a trademark of Konami Digital Entertainment.
This project is unaffiliated with Konami.

---

## References

The search implements published methods: NRPA and GNRPA for the sampled policy
([arXiv:2003.10024](https://arxiv.org/abs/2003.10024)), Levin Tree Search and
PHS* for the complete finish
([arXiv:2103.11505](https://arxiv.org/abs/2103.11505),
[arXiv:2412.05196](https://arxiv.org/abs/2412.05196)), Iterated Width for
novelty pruning ([arXiv:1801.03354](https://arxiv.org/abs/1801.03354)),
Go-Explore for the archive of positions
([arXiv:2004.12919](https://arxiv.org/abs/2004.12919)), hindsight relabelling
(Andrychowicz et al., NeurIPS 2017), macro-operators selected by Levin loss
([arXiv:2410.11262](https://arxiv.org/abs/2410.11262)), learned landmarks
([arXiv:2508.21564](https://arxiv.org/abs/2508.21564)), and retrosynthesis
search for goal decomposition
([arXiv:2006.15820](https://arxiv.org/abs/2006.15820),
[arXiv:2407.06334](https://arxiv.org/abs/2407.06334)).
