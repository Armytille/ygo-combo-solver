# ygo-combo-solver

Takes a Yu-Gi-Oh! replay and finds better ways to play the same turn.

Point it at a duel you recorded. It works out the board you ended on, then looks
for other lines that reach it: spending fewer cards, starting from a different
hand or deck, or holding up against an interruption. It answers the reverse
question too — describe a board, and it tells you whether your deck can build
it, and how. Everything it finds comes back as a replay you can watch in EDOPro.

It plays through the same engine EDOPro runs, so every line it hands you is
legal and playable.

**[Download the latest release](https://github.com/Armytille/ygo-combo-solver/releases/latest)**
— Windows x64, one executable, no dependencies. Needs an EDOPro installation.

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

Unpack the release anywhere: `combosolver.exe`, a sample replay in `gabarits/`,
and this document. The executable runs from any directory and writes its results
wherever `--outdir` points, relative to where you launched it.

It reads cards and scripts from your EDOPro installation, and never writes to
it. Point it there once:

```powershell
setx COMBOSOLVER_WORKDIR "C:\Games\ProjectIgnis"
```

Or pass `--workdir <dir>` every run. With neither, it stops with an error rather
than guessing.

---

## Quick start

```powershell
# 1. check that the replay reproduces
combosolver.exe duel.yrpX --scriptdir <card-scripts>

# 2. look for a cheaper line to the same board, five minutes
combosolver.exe duel.yrpX --scriptdir <card-scripts> `
    --solve --optimize --solve-ms 300000 --outdir solutions
```

**Never skip step 1.** A replay only reproduces with the card scripts that
existed when it was recorded. With a different set the engine asks different
questions, the recorded answers stop matching, and the run silently describes a
different duel. The report's `MSG_RETRY` counter must read **0**.

`--scriptdir` is repeatable, highest priority first. It also switches off the
automatic scan of EDOPro's `repositories/`, so pass those folders too if the
deck needs them.

---

## Usage by task

The first argument is always a replay. What the solver takes from it depends on
the flags:

| Flags | The replay provides | The duel played is |
|---|---|---|
| none | the line to replay and measure | its own |
| `--solve` | the line, and the target board | its own |
| `--start other.yrpX` | the line, and the target board | the one in `other.yrpX` |
| `--deck d.ydk --hand …` | the line, and the target board | built from your decklist |
| `--no-ref --target …` | only the duel setup | built from your decklist |

`--start`, `--deck` and `--fire` turn the search on by themselves.

### 1. Check a replay and measure the line

```powershell
combosolver.exe duel.yrpX --scriptdir <card-scripts>
```

No search. Read `MSG_RETRY` (must be 0), then answers consumed against answers
recorded, then the cost of the line and the list of cards it spends — that list
tells you whether another deck could run the same combo.

### 2. Find a cheaper line to the same board

```powershell
combosolver.exe duel.yrpX --scriptdir <card-scripts> `
    --solve --optimize --solve-ms 300000 --outdir solutions
```

`--solve` stops at the first line. `--optimize` keeps going: it ranks by burned
cards, searches on after each solution, and lets a line continue past the board
to recover cards it spent.

Solutions land in `solutions/`, best first, with the cost in the filename —
`solution_00_b1_a9.yrp` burned one card and took nine actions. Compare against
the reference line's burn count from task 1.

Add `--burn-limit <n>` if you already achieve a given count, so it only looks
for better. Time is a budget, not a completion criterion.

### 3. Reach the same board from a different hand or deck

```powershell
# from a decklist and an opening hand you choose
combosolver.exe ref.yrpX --scriptdir <card-scripts> `
    --deck "C:\Games\ProjectIgnis\deck\Lunalight.ydk" `
    --hand "Assault Zone|Ash Blossom|Ash Blossom|Ash Blossom" `
    --solve-ms 600000 --outdir solutions

# or from another recorded duel, with its deck, hand and shuffle
combosolver.exe ref.yrpX --scriptdir <card-scripts> `
    --start other.yrpX --solve-ms 600000 --outdir solutions
```

Recorded answers are positions in a menu and mean nothing in another duel. The
solver re-reads the reference in terms of which cards each decision engages, so
the intent of the combo transfers.

`--hand` takes card codes or name fragments separated by `|`, and is verified in
a throwaway duel before the search starts. `--board-add` and `--board-remove`
adjust the captured board if you also want to change the objective.

### 4. Ask whether a deck can reach a board you describe

For combos nobody has recorded. The replay is only there to set up the duel.

```powershell
combosolver.exe template.yrpX --scriptdir <card-scripts> `
    --no-ref `
    --deck "C:\Games\ProjectIgnis\deck\Lunalight.ydk" `
    --hand "57103969|57103969|57103969" `
    --target 54701958 --target 54701958 --target 54701958 `
    --target "90590304@DEF" `
    --max-decisions 700 --solve-ms 900000 --outdir solutions
```

`--target` is repeatable and repeats count: three identical entries ask for
three copies. Add `@DEF` for defence position. The board you describe is a
minimum — a line producing something extra still counts. Set `--max-decisions`
generously; with no reference line there is nothing to size it from.

This is the hardest mode, and the one where guidance pays off:

| Flag | Use |
|---|---|
| `--summon-min "card:3"` | require three summons of a card, and steer towards them |
| `--resolve "card@field:2"` | require an effect to resolve twice, activated from the field |
| `--hint "card"` | nudge sampling towards lines using a card you know matters |
| `--recipes 1` | reason in summons still to be made rather than cards still missing |

### 5. Require the line to survive interaction

`--guard` requires that, from the n-th summon onward, you hold an answer at
every point where the opponent could act.

```powershell
combosolver.exe duel.yrpX --scriptdir <card-scripts> `
    --solve --optimize --solve-ms 600000 `
    --guard "5:Crystal Wing|Zalen@field+Junk Signal@hand" `
    --guard-off "opphand<=2" --outdir solutions
```

`--fire` goes further: it hands the opponent a card and makes them play it
wherever legal, and the solver must rebuild the board from what is left.

```powershell
combosolver.exe duel.yrpX --scriptdir <card-scripts> `
    --fire 27204311 --fire-spare "Junk Signal" --fire-open --fire-ms 60000 `
    --outdir solutions
```

`--fire-spare` may be spent answering; the board without it still counts.
`--fire-open` only interrupts on an empty chain, so the threat goes first.
`--fire-bake` writes the card into the replay header so the output plays in
EDOPro unaided.

If you start from a solo hand test the opponent holds nothing, no interruption
is possible, and `--guard` passes without proving anything. Give them cards with
`--opp-hand` first; replays produced that way need the same `--opp-hand` to play
back.

### 6. Check a line against your rules

Constraint flags with no `--solve` turn the tool into a checker, on lines it
produced or on replays played by hand.

```powershell
combosolver.exe solutions\solution_00_b1_a9.yrp --scriptdir <card-scripts> `
    --guard "5:Crystal Wing|Zalen@field+Junk Signal@hand" `
    --resolve "PSY-Framelord Omega@field:2"
```

### 7. Compare two settings fairly

The budget is wall time, so two runs of one command do different work. Replace
the clock with fixed work:

```powershell
combosolver.exe ... --threads 1 --seed 888 `
    --max-rollouts 20000 --max-nodes 500000 --solve-ms 900000
```

One worker is much slower, so use this to measure, not to produce results. Run
the two settings one after the other.

---

## Constraints

Constraints are not filters applied at the end; most of them remove the move
from the search entirely.

Zones: `hand`, `field`, `grave`, `banished`, `extra` (default `field`).
Attributes for `--material`: `light`, `dark`, `earth`, `water`, `fire`, `wind`,
`divine`.

```bash
--summon "5:Zalen|Crystal Wing"      # the 5th summon must be one of these

# from the 5th summon on, hold one of these at every opponent window.
# '+' means all of them at once, '|' separates alternatives.
--guard "5:Crystal Wing|Zalen@field+Junk Signal@hand"
--guard-off "opphand<=2"             # stop requiring it once they are down to 2
--guard "5:Dis Pater@field+oppbanished>=1"   # a state condition, not a card

--resolve "PSY-Framelord Omega@field:2"      # resolve this twice, from the field
--summon-min "Liger Dancer:3"                # summon this three times
--material "Chaos Angel:light"               # summon it using a LIGHT material

--no-activate "Duel Evolution - Assault Zone"  # never activate from the field
--no-chain "Crystal Wing"                      # never chain onto your own effects
--mp1-only                                     # keep the combo in Main Phase 1
```

Three things that are easy to get wrong:

- `--guard` asks that an answer be *available*, not that the guard be the n-th
  summon; it may already be on the field.
- `--resolve` and `--summon-min` are checked at the end, so a board matching the
  target without them is rejected and the search carries on. Four of them at
  most, in total.
- `@zone` on `--resolve` pins where the effect is *activated from*. Leave it out
  and a card with effects in two zones satisfies the rule from the wrong one.

Cards named in `--resolve` and `--summon-min` also get a sampling nudge. To find
out whether the solver gets there on its own, name the card with `--watch`,
which observes without steering.

---

## Flags

`combosolver.exe --help` lists all 133 with their defaults. The ones the tasks
above use:

| Flag | Meaning |
|---|---|
| `--workdir <dir>` | EDOPro installation, or set `COMBOSOLVER_WORKDIR` |
| `--scriptdir <dir>` | card script folder, highest priority first; repeatable |
| `--outdir <dir>` | where to write the replays found |
| `--player <0\|1>` | whose turn to optimise |
| `--solve` | search for a line to the target board |
| `--start <replay>` | start from another duel |
| `--deck <file.ydk>` / `--hand <cards>` | start from a decklist and a chosen hand |
| `--target <card[@DEF]>` | describe the board yourself; repeatable, repeats count |
| `--board-add` / `--board-remove` | adjust the captured board |
| `--no-ref` | ignore the recorded line entirely |
| `--optimize` | keep improving instead of stopping at the first line |
| `--burn-limit <n>` | a burn count you already achieve |
| `--solve-ms <ms>` | search time, default two minutes |
| `--threads <n>` | workers, default every core |
| `--max-decisions <n>` | longest line allowed |
| `--seed <n>` | reuse a printed seed to repeat a run |
| `--verbose` | print every decision |

---

## Reading the output

Every run ends with a self-check, whether or not it searched. If the replay does
not reproduce, nothing measured afterwards is worth reading.

| Section | What it tells you | What you want |
|---|---|---|
| Loading | cards and script folders found | non-zero, and the scripts you meant |
| Replay result | answers consumed, `MSG_RETRY` | all consumed, `MSG_RETRY` 0 |
| Decision points | options the engine offered, by prompt type | how hard the search will be |
| Cost of the line | cards consumed, burned, actions, decisions | your baseline for task 2 |
| Target board | the board captured, card by card | the board you expected |
| Self-checks | save/restore fidelity and stress cycles | all pass |

A line's cost is three numbers, compared in this order: **burned** cards (in the
graveyard or banished), **actions** (summons and activations), **decisions**
(prompts answered). Cards on the final board are fixed by the target, so burned
cards are what one line can spend less of than another.

After a search you also get the ranking of the lines found and how many were
examined and written. A flag that could not apply is reported as `!! INERT`.

The report is written in French; the command line, help text and documentation
are English.

---

## How it works

**It plays the game for real.** There is no model of Yu-Gi-Oh! in the solver: it
runs the engine, and at each prompt enumerates the answers the engine says are
legal. Every line is replayed from scratch and re-checked before being written.

**It searches positions, not sequences.** The number of ways to order a turn's
moves is astronomically large, but many orderings arrive at the same position.
The solver recognises those and explores each position once.

**It can rewind.** Trying a different move means going back to an earlier point
in the duel. Replaying the turn from the start would be far too slow, so the
solver snapshots the engine's memory and restores it, which is orders of
magnitude cheaper.

**It learns as it goes.** It samples lines, keeps a policy weighted by what
worked, and adapts it towards the best line so far. Moves are identified by the
cards they engage rather than by menu position, so what it learns on one deck
still means something on another.

**It finishes deliberately.** Sampling gets close to the board but rarely lands
the last few decisions, so the solver switches to a complete search from the
best positions it has stored, guided by the policy it learned.

**It knows what "closer" means.** Counting target cards on the field says
nothing for most of a combo, because the board only fills at the end. The solver
works out what each summon needs and produces, and measures distance in summons
still missing.

---

## Limitations

- **Hard targets are not guaranteed.** On difficult boards the search converts
  on some runs and not others. Give it a real budget and try more than once
  before concluding a board is unreachable.
- **One turn at a time.** Lines crossing into the opponent's turn need
  `--turns 2` and are less well covered.
- **The replay must match its scripts.** Nothing can repair a replay recorded
  against a card script set you no longer have.
- **The flag list is long.** 133 of them; the tasks above use about fifteen.
- **The report is in French.**

---

## Build from source

Windows x64, Visual Studio 2022 build tools, PowerShell.

```powershell
.\tools\fetch_solver_deps.ps1
..\premake5\premake5.exe vs2022 --vcpkg-root=..\..\vcpkg
MSBuild build\combosolver.sln /p:Configuration=Release /p:Platform=x64
```

`fetch_solver_deps.ps1` fetches the engine at a pinned commit, applies the
patches the solver needs to observe and snapshot a duel, and freezes a matching
card script export. It never touches your EDOPro installation.

For a build to keep or share, use `.\tools\build_release.ps1 -Workdir <dir>`
instead: it produces an optimised, self-contained executable and packages it.

---

## License

GNU Affero General Public License, version 3 or later. The full text is in
[LICENSE](LICENSE); the component-by-component notices are in [NOTICE](NOTICE).

This is inherited rather than chosen. `combosolver.exe` statically links
[ocgcore](https://github.com/edo9300/ygopro-core), which is AGPL-3.0-or-later,
so the combined work carries the same terms. In practice: you may use, study,
modify and redistribute it, and anyone you give a copy to — including over a
network — is entitled to the corresponding source.

Card data and card scripts are read from your EDOPro installation at run time.
They are not part of this program and are not redistributed with it. Yu-Gi-Oh!
is a trademark of Konami Digital Entertainment; this project is unaffiliated
with and unendorsed by Konami.

---

## References

The search is built on published methods: NRPA and GNRPA for the sampled policy
([arXiv:2003.10024](https://arxiv.org/abs/2003.10024)), Levin Tree Search and
PHS* for the complete finish ([arXiv:2103.11505](https://arxiv.org/abs/2103.11505),
[arXiv:2412.05196](https://arxiv.org/abs/2412.05196)), Iterated Width for novelty
pruning ([arXiv:1801.03354](https://arxiv.org/abs/1801.03354)), Go-Explore for the
archive of promising positions ([arXiv:2004.12919](https://arxiv.org/abs/2004.12919)),
hindsight relabelling (Andrychowicz et al., NeurIPS 2017), macro-operators
selected by Levin loss ([arXiv:2410.11262](https://arxiv.org/abs/2410.11262)),
learned landmarks ([arXiv:2508.21564](https://arxiv.org/abs/2508.21564)), and
retrosynthesis search for goal decomposition
([arXiv:2006.15820](https://arxiv.org/abs/2006.15820),
[arXiv:2407.06334](https://arxiv.org/abs/2407.06334)).
