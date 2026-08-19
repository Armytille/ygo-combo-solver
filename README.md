# ygo-combo-solver

Takes a Yu-Gi-Oh! replay and finds better ways to play the same turn.

Point it at a duel you recorded. It works out the board you ended on, then goes
looking for other lines that reach it: spending fewer cards, starting from a
different opening hand, running on a different deck, or holding up against an
interruption. Everything it finds comes back as a replay you can open in EDOPro
and watch.

It answers the reverse question too. Describe a board yourself, and it will tell
you whether your deck can build it, and how.

It plays the game through the same engine EDOPro runs, so every line it hands
you is legal and playable. There is no second rules model that might disagree
with the real one.

---

## Contents

- [What you can ask it](#what-you-can-ask-it)
- [Requirements](#requirements)
- [Install](#install)
- [Quick start](#quick-start)
- [Usage by task](#usage-by-task)
- [Constraint grammar](#constraint-grammar)
- [Flag reference](#flag-reference)
- [Reading the output](#reading-the-output)
- [How it works](#how-it-works)
- [Limitations](#limitations)
- [Build from source](#build-from-source)
- [References](#references)

---

## What you can ask it

| Question | Command sketch |
|---|---|
| Does this replay reproduce, and what does the line cost? | no flags |
| Is there a cheaper line to the same board? | `--solve --optimize` |
| Can I reach this board from a different hand? | `--deck` + `--hand` |
| Can I reach this board from a different duel? | `--start` |
| Can this deck reach a board I describe myself? | `--no-ref --target` |
| Does the line hold up against an interruption? | `--guard`, `--fire` |
| Does this line someone played respect my rules? | constraint flags, no `--solve` |

Each of these is worked through in [Usage by task](#usage-by-task).

**The cost of a line.** The solver ranks solutions on three quantities, in this
order:

1. **burned** — cards that end up in the graveyard or banished. Cards on the
   final board are fixed by the target, so burned cards are what one line can
   spend less of than another.
2. **actions** — summons and activations.
3. **decisions** — prompts answered.

Filenames record the first two: `solution_00_b1_a9.yrp` burned one card and took
nine actions. Index `00` is the best line found.

---

## Requirements

- Windows x64.
- A working EDOPro installation. The solver reads its card database and card
  scripts, and never writes to it.
- A card script set matching the replay you are analysing (see
  [Quick start](#quick-start)).

Point the solver at the EDOPro installation once:

```powershell
setx COMBOSOLVER_WORKDIR "C:\Games\ProjectIgnis"
```

Or pass `--workdir <dir>` on every run. With neither, the solver stops with an
error rather than guessing.

---

## Install

Download the release zip and unpack it anywhere. It contains a single
executable with no dependencies, plus a sample replay.

The executable can live anywhere and be run from any directory. It reads cards
and scripts from the EDOPro installation, and writes its results to whatever
`--outdir` points at, relative to wherever you launched it.

To build it yourself, see [Build from source](#build-from-source).

---

## Quick start

```powershell
# 1. check that the replay reproduces
combosolver.exe duel.yrpX --scriptdir <card-scripts>

# 2. look for a cheaper line to the same board, five minutes
combosolver.exe duel.yrpX --scriptdir <card-scripts> `
    --solve --optimize --solve-ms 300000 --outdir solutions
```

Do step 1 first, every time you work on a new replay.

A replay only reproduces with the card scripts that existed when it was
recorded. With a different set, the engine asks different questions, the
recorded answers stop matching, and the run silently describes a different duel.
The report's `MSG_RETRY` counter is what tells you: **it must be 0**. If it is
not, point `--scriptdir` at a script export contemporary with the replay.

`--scriptdir` is repeatable, highest priority first. It also disables the
automatic scan of the EDOPro `repositories/` folder, so if the deck uses cards
that only exist there, pass those folders too.

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

---

### Task 1 — Check a replay and measure the line

```powershell
combosolver.exe duel.yrpX --scriptdir <card-scripts>
```

No search. The solver replays the recorded answers, shows how many options the
engine offered at each decision, captures the final board, and checks itself.

What to look at:

1. **`MSG_RETRY`** must be 0. Anything else and the rest of the report is about
   a different duel.
2. **Answers consumed** should equal answers recorded. A shortfall means the
   line stopped early.
3. **Cost of the line** and the itemised list of cards it consumes. This tells
   you whether another deck has the resources to run the same combo.

Do this before anything else, including before comparing two decks.

---

### Task 2 — Find a cheaper line to the same board

```powershell
combosolver.exe duel.yrpX --scriptdir <card-scripts> `
    --solve --optimize --solve-ms 300000 --outdir solutions
```

`--solve` stops at the first line it finds. `--optimize` keeps going and
improves: it ranks by burned cards first, keeps searching after each solution,
and lets a line continue past the board so it can recover cards it spent.

Results land in `solutions/`, sorted, best first. Compare the burned count in
the filename against the reference line's, printed by Task 1.

Useful knobs:

| Flag | Effect |
|---|---|
| `--solve-ms <ms>` | how long to search. Longer finds better lines; the default is two minutes |
| `--burn-limit <n>` | tell it a burn count you already achieve, so it only looks for better |
| `--threads <n>` | workers, defaults to every core |

Time is a budget, not a completion criterion. The solver reports the best it
found when the budget ran out.

---

### Task 3 — Reach the same board from a different hand or deck

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

The recorded answers are positions in a menu; in another duel they mean nothing.
The solver re-reads the reference line in terms of *which cards it engages*, so
the intent of the combo transfers even when the menus differ.

`--hand` takes card codes or name fragments separated by `|`. The requested hand
is verified in a throwaway duel before the search starts, so you never search on
a hand you only think you have.

To change the objective at the same time, `--board-add` and `--board-remove`
adjust the captured board instead of replacing it.

---

### Task 4 — Ask whether a deck can reach a board you describe

For combos nobody has recorded. Describe the board yourself; the replay is only
there to set up the duel.

```powershell
combosolver.exe template.yrpX --scriptdir <card-scripts> `
    --no-ref `
    --deck "C:\Games\ProjectIgnis\deck\Lunalight.ydk" `
    --hand "57103969|57103969|57103969" `
    --target 54701958 --target 54701958 --target 54701958 `
    --target "90590304@DEF" `
    --max-decisions 700 --solve-ms 900000 --outdir solutions
```

- `--target` is repeatable and repeats count: three entries with the same card
  ask for three copies. Add `@DEF` for defence position.
- The board you describe is a *minimum*: a line that also produces something
  extra still counts.
- `--max-decisions` caps how long a line may be. Without a reference line to
  size it from, set it yourself, generously.

This is the hardest mode, and the one where guidance pays off most:

| Flag | Use |
|---|---|
| `--summon-min "card:3"` | require three summons of a card, and steer the search towards them |
| `--resolve "card@field:2"` | require an effect to resolve twice, activated from the field |
| `--hint "card"` | nudge sampling towards lines that use a card you know matters |
| `--recipes 1` | let the solver reason in terms of summons still to be made rather than cards still missing |

Expect to give it minutes, not seconds.

---

### Task 5 — Require the line to survive interaction

Two levels.

**Have an answer ready.** `--guard` requires that, from the n-th summon onward,
at every point where the opponent could act, you hold at least one of the
answers you listed.

```powershell
combosolver.exe duel.yrpX --scriptdir <card-scripts> `
    --solve --optimize --solve-ms 600000 `
    --guard "5:Crystal Wing|Zalen@field+Junk Signal@hand" `
    --guard-off "opphand<=2" `
    --outdir solutions
```

**Actually get interrupted.** `--fire` hands the opponent a card and makes them
play it at every point where it is legal. The solver then has to rebuild the
board from whatever is left.

```powershell
combosolver.exe duel.yrpX --scriptdir <card-scripts> `
    --fire 27204311 --fire-spare "Junk Signal" --fire-open --fire-ms 60000 `
    --outdir solutions
```

| Flag | Effect |
|---|---|
| `--fire-spare <card>` | this card may be spent answering; the board without it still counts |
| `--fire-open` | only interrupt when the chain is empty, so the threat goes first |
| `--fire-bake` | write the card into the replay header, so the output plays in EDOPro with no extra flag |

If the duel you start from is a solo hand test, the opponent holds nothing, no
interruption is possible, and `--guard` passes without proving anything. Give
them cards with `--opp-hand` first. Replays produced that way need the same
`--opp-hand` to play back.

---

### Task 6 — Check a line against your rules

Constraint flags with no `--solve` turn the tool into a checker. It works on
lines the solver produced and on replays played by hand.

```powershell
combosolver.exe solutions\solution_00_b1_a9.yrp --scriptdir <card-scripts> `
    --guard "5:Crystal Wing|Zalen@field+Junk Signal@hand" `
    --resolve "PSY-Framelord Omega@field:2" `
    --no-activate "Duel Evolution - Assault Zone"
```

The report says, for each rule, whether it holds and where it fails.

---

### Task 7 — Compare two settings fairly

Two runs of the same command do not do the same work, because the budget is wall
time. To compare settings, replace the clock with a fixed amount of work:

```powershell
combosolver.exe ... --threads 1 --seed 888 `
    --max-rollouts 20000 --max-nodes 500000 --solve-ms 900000
```

A single worker is much slower, so use this to measure, not to produce results.
Run the two settings one after the other: two searches at once compete for the
same cores and neither timing means anything.

---

## Constraint grammar

Constraints are not filters applied at the end. Most of them remove the move
from the search entirely.

Zones: `hand`, `field`, `grave`, `banished`, `extra`. Default `field`.

Attributes for `--material`: `light`, `dark`, `earth`, `water`, `fire`, `wind`,
`divine`.

```bash
# the n-th summon must be one of these cards
--summon "5:Zalen|Crystal Wing"

# from the 5th summon onward, at every opponent window, hold one of these.
# A '+' means "all of these at once"; a '|' separates alternatives.
--guard "5:Crystal Wing|Zalen@field+Junk Signal@hand"

# stop requiring the guard once the opponent's hand is down to 2 cards
--guard-off "opphand<=2"

# a condition on the game state instead of a card
--guard "5:Dis Pater@field+oppbanished>=1"

# the line must resolve this effect twice, activated from the field
--resolve "PSY-Framelord Omega@field:2"

# the line must summon this card three times
--summon-min "Liger Dancer:3"

# summoning this card must use a LIGHT material
--material "Chaos Angel:light"

# never activate this card from the field
--no-activate "Duel Evolution - Assault Zone"

# never chain this card in response to your own effects
--no-chain "Crystal Wing"

# keep the whole combo in Main Phase 1
--mp1-only
```

Three things that are easy to get wrong:

- `--guard` asks that an answer be *available* when the window opens. It does
  not require the guard to be the n-th summon; it may already be on the field.
- `--resolve` and `--summon-min` are checked at the end. A board that matches
  the target without them is rejected and the search carries on. You may declare
  at most four of them in total.
- `@zone` on `--resolve` pins where the effect is *activated from*. Leave it out
  and a card with effects in two zones will satisfy the rule from the wrong one.

Cards named in `--resolve` and `--summon-min` also get a sampling nudge. If you
want to know whether the solver finds something *on its own*, name it with
`--watch` instead, which observes without steering.

---

## Flag reference

`combosolver.exe --help` lists all 133 flags with their defaults, grouped by
role. The ones in regular use:

**Input and output**

| Flag | Meaning |
|---|---|
| `--workdir <dir>` | EDOPro installation, or set `COMBOSOLVER_WORKDIR` |
| `--scriptdir <dir>` | card script folder, highest priority first; repeatable |
| `--player <0\|1>` | whose turn to optimise |
| `--outdir <dir>` | where to write the replays found |

**What to search for**

| Flag | Meaning |
|---|---|
| `--solve` | search for a line to the target board |
| `--start <replay>` | start from another duel |
| `--deck <file.ydk>` | start from a decklist |
| `--hand <cards>` | opening hand, `\|`-separated names or codes |
| `--target <card[@DEF]>` | describe the board yourself; repeatable, repeats count |
| `--board-add` / `--board-remove` | adjust the captured board |
| `--no-ref` | ignore the recorded line entirely |

**Budget**

| Flag | Meaning |
|---|---|
| `--solve-ms <ms>` | search time, default two minutes |
| `--threads <n>` | workers, default every core |
| `--max-decisions <n>` | longest line allowed |
| `--seed <n>` | reuse a printed seed to repeat a run |
| `--rounds <n>` | split the budget into rounds, feeding each one the best line so far |

**Cost**

| Flag | Meaning |
|---|---|
| `--optimize` | keep improving instead of stopping at the first line |
| `--burn-limit <n>` | a burn count you already achieve |
| `--burn-slack <n>` | how much worse than the best a line may get before being abandoned |

**Diagnostics**

| Flag | Meaning |
|---|---|
| `--verbose` | print every decision |
| `--probe-repeat` | why a repeated summon fails: never attempted, or always lost |
| `--operators` | print what the solver understood of the deck's cards |
| `--growth`, `--width`, `--profile` | measurement modes, no search |

---

## Reading the output

Every run ends with a self-check, whether or not it searched. If the replay does
not reproduce, or the solver's internal state handling is not exact, nothing
measured afterwards is worth reading.

| Section | What it tells you | What you want |
|---|---|---|
| Loading | cards and script folders found | non-zero, and the scripts you meant |
| Replay result | answers consumed, `MSG_RETRY` | all consumed, `MSG_RETRY` 0 |
| Decision points | how many options the engine offered, by prompt type | context for how hard the search is |
| Cost of the line | cards consumed, burned, actions, decisions | your baseline for Task 2 |
| Target board | the board captured, card by card | the board you expected |
| Restore fidelity | two state fingerprints compared | identical |
| Snapshot stress | repeated save/restore cycles | all pass |

After a search, you also get the ranking of the lines found, how many were
examined, and how many were written. When a mechanism you asked for could not
apply, the report says so with `!! INERT` rather than quietly doing nothing.

The report is currently written in French. The command line, the help text and
this document are English.

---

## How it works

**It plays the game for real.** There is no model of Yu-Gi-Oh! inside the
solver. It runs the same engine EDOPro runs, and at each prompt it enumerates
the answers the engine says are legal. A line the solver produces is playable by
construction, and it is re-played from scratch and re-checked before being
written out.

**It searches states, not sequences.** The number of ways to order a turn's
moves is astronomically large, but many orderings arrive at exactly the same
position. The solver recognises those and explores each position once, which
brings the problem back into reach.

**It can rewind.** Trying a different move means going back to an earlier point
in the duel. Restarting the turn and replaying it would be far too slow, so the
solver snapshots the engine's memory and restores it instead, which is orders of
magnitude cheaper. Every other part of the search depends on that being fast.

**It learns as it goes.** The search samples lines, keeps a policy weighted by
what worked, and adapts it towards the best line found so far. Because moves are
identified by the cards they engage rather than by menu position, what it learns
on one deck still means something on another.

**It finishes deliberately.** Sampling gets close to the board but rarely lands
the last few decisions. Once it has promising positions, the solver switches to
a complete search from the best ones it has stored, guided by the policy it
learned.

**It knows what "closer" means.** Counting how many target cards are on the
field says nothing for most of a combo, because the board only fills up at the
end. The solver builds a picture of what each summon needs and produces, and
uses the number of summons still missing as its notion of distance. It also
solves a small resource balance to work out intermediate objectives that exist
from the very first card played.

---

## Limitations

**Hard targets are not guaranteed.** On difficult boards the search converts on
some runs and not others with the same command. Run it more than once, and give
it a real time budget, before concluding a board is unreachable.

**One turn at a time.** The default domain is the player's own turn. Lines that
cross into the opponent's turn need `--turns 2` and are less well covered.

**The replay must match its scripts.** The solver cannot repair a replay
recorded against a card script set you no longer have. `MSG_RETRY` tells you
when that is the problem.

**Most flags are unproven.** There are 133 of them. A small, documented subset
carries the everyday work; the rest exist because they were worth trying.
`docs/drapeaux.md` records which is which.

**The report is in French.**

**No license file yet.**

---

## Build from source

Windows x64, Visual Studio 2022 build tools, PowerShell.

```powershell
.\tools\fetch_solver_deps.ps1
..\premake5\premake5.exe vs2022 --vcpkg-root=..\..\vcpkg
MSBuild build\combosolver.sln /p:Configuration=Release /p:Platform=x64
```

`fetch_solver_deps.ps1` fetches `ocgcore` at a pinned commit, applies the small
patches the solver needs to observe and snapshot a duel, and freezes a matching
card script export. It never touches your EDOPro installation. The commit is
pinned rather than tracked because a replay recorded by an older client needs
the engine of its time.

For a build you intend to keep or share, use the release script instead. It
applies profile-guided optimisation, which a plain MSBuild silently discards,
and it checks that the resulting executable is self-contained before packaging
it.

```powershell
.\tools\build_release.ps1 -Workdir <edopro-install>
```

---

## References

The search draws on published work; these are the papers whose methods are
implemented here.

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
