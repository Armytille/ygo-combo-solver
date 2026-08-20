# ygo-combo-solver

Searches for Yu-Gi-Oh! combo lines inside a recorded duel.

Given a replay, the solver determines the board the player ended their turn on
and searches for other move sequences that reach it: lines that spend fewer
cards, lines that start from a different opening hand or a different deck, and
lines that hold up when the opponent interrupts. It also accepts a board
description and a decklist with no replay, and searches for a way to build that
board. Results are written as replay files that open in EDOPro.

The solver runs the same engine EDOPro runs, so every line it produces is legal
and playable.

**[Download the latest release](https://github.com/Armytille/ygo-combo-solver/releases/latest)**

Windows x64. One executable, no dependencies. Requires an EDOPro installation.

---

## Contents

- [Setup](#setup)
- [Command line](#command-line)
- [Tasks](#tasks)
- [Naming a card](#naming-a-card)
- [Constraint grammar](#constraint-grammar)
- [Flag reference](#flag-reference)
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

Cards and card scripts are read from an EDOPro installation, which the solver
only reads. Set its location once:

```powershell
setx COMBOSOLVER_WORKDIR "C:\Games\ProjectIgnis"
```

`--workdir <dir>` overrides the variable. Without either, the solver exits with
an error.

---

## Command line

```
combosolver.exe <replay.yrpX> [options]
```

The first argument is always a replay. What it supplies depends on the flags:

| Flags given | The replay supplies | The duel played is |
|---|---|---|
| none | the line to replay and measure | its own |
| `--solve` | the line, and the target board | its own |
| `--start other.yrpX` | the line, and the target board | the one in `other.yrpX` |
| `--deck d.ydk --hand …` | the line, and the target board | built from the decklist |
| `--no-ref --target …` | the duel setup only: flags, life points, opponent | built from the decklist |

`--start`, `--deck` and `--fire` each imply `--solve`. Without `--solve` or a
flag that implies it, the run is a replay plus the self-checks, and no search.

### Conventions used in the examples

| Placeholder | Meaning |
|---|---|
| `duel.yrpX` | the replay under study |
| `<card-scripts>` | a card script directory; repeat the flag, highest priority first |
| `solutions` | the output directory, `--outdir`, default `solutions/` |

Examples are written for PowerShell, where a trailing backtick continues the
line. In `cmd.exe` the continuation character is `^`; written on a single line,
neither is needed.

### Card script sets

A replay reproduces only with the card scripts contemporary with its recording.
Under a different script set the engine issues different prompts, the recorded
answers stop applying, and the run continues on a different duel with no error
message. Verify every new replay with
task 1 before anything else:
`MSG_RETRY` must read 0.

`--scriptdir` is repeatable, highest priority first. It also disables the
automatic scan of EDOPro's `repositories/` directory, so pass those directories
explicitly when the deck requires them.

---

## Tasks

<details>
<summary><b>1. Verify a replay and measure its line</b> — no flags</summary>

No search. Establishes that the replay reproduces, and prints the cost of the
recorded line, which is the baseline every later run is compared against.

```powershell
combosolver.exe duel.yrpX --scriptdir <card-scripts>
```

The report gives, in this order:

1. `MSG_RETRY`, which must be 0.
2. Answers consumed against answers recorded.
3. The cost of the line, with an itemised list of the cards it consumes. That
   list determines whether another deck holds the resources for the same combo.

</details>

<details>
<summary><b>2. Find a cheaper line to the same board</b> — <code>--solve --optimize</code></summary>

```powershell
combosolver.exe duel.yrpX --scriptdir <card-scripts> `
    --solve --optimize `
    --solve-ms 300000 `
    --outdir solutions
```

| Flag | Effect |
|---|---|
| `--solve` | search for a line towards the captured board, stop at the first one found |
| `--optimize` | do not stop: each solution tightens the bound, the goal score becomes lexicographic (burned, actions, decisions), rollouts continue past the goal so spent cards can be recovered, and the finisher runs even when lines already exist |
| `--solve-ms 300000` | five minutes; the run reports the best line found when the budget expires |
| `--outdir solutions` | destination of the replays produced |

Solutions are written sorted, best first. The cost appears in the filename:
`solution_00_b1_a9.yrp` burned one card and used nine actions. The reference
line's own burn count comes from task 1.

Add `--burn-limit <n>` to declare a burn count already achieved, so the search
only looks below it.

</details>

<details>
<summary><b>3. Reach the same board from another decklist</b> — <code>--deck --hand</code></summary>

The board comes from the reference replay; the duel is rebuilt from a decklist
and a chosen opening hand.

```powershell
combosolver.exe ref.yrpX --scriptdir <card-scripts> `
    --deck "C:\Games\ProjectIgnis\deck\Lunalight.ydk" `
    --hand "Assault Zone|Ash Blossom|Ash Blossom|Ash Blossom" `
    --solve-ms 600000 `
    --outdir solutions
```

| Flag | Effect |
|---|---|
| `--deck <f.ydk>` | rebuild the reference board from this decklist. Implies `--solve` |
| `--hand <cards>` | opening hand, cards separated by `\|`, passcodes or name fragments. Default: the reference's hand |

The hand is verified in a throwaway duel before the search starts.

Recorded answers are prompt indices, and in another duel they designate
different cards. The solver re-reads the reference line by the cards each
decision engages, which carries the combo's intent across. `--no-plan` discards
that repertoire and starts the policy uniform, which measures what the reference
was worth.

</details>

<details>
<summary><b>4. Reach the same board from another recorded duel</b> — <code>--start</code></summary>

Same target board, with the deck, hand and shuffle of a second replay.

```powershell
combosolver.exe ref.yrpX --scriptdir <card-scripts> `
    --start other.yrpX `
    --solve-ms 600000 `
    --outdir solutions
```

`--start` implies `--solve`.

</details>

<details>
<summary><b>5. Edit the captured board</b> — <code>--board-add --board-remove</code></summary>

`--board-add` and `--board-remove` modify the board captured from the reference
instead of describing one from scratch. Both require `--deck` or `--start`, and
take the same grammar as `--target`.

```powershell
combosolver.exe ref.yrpX --scriptdir <card-scripts> `
    --start other.yrpX `
    --board-add "Crystal Wing" `
    --board-remove 27204311 `
    --solve-ms 600000 `
    --outdir solutions
```

</details>

<details>
<summary><b>6. Build a board described by hand</b> — <code>--no-ref --target</code></summary>

For combos with no recording. The replay is demoted to a duel template.

```powershell
combosolver.exe template.yrpX --scriptdir <card-scripts> `
    --no-ref `
    --deck "C:\Games\ProjectIgnis\deck\Lunalight.ydk" `
    --hand "57103969|57103969|57103969" `
    --target 54701958 --target 54701958 --target 54701958 `
    --target "90590304@DEF" `
    --max-decisions 700 `
    --solve-ms 900000 `
    --outdir solutions
```

| Flag | Effect |
|---|---|
| `--no-ref` | goal-only mode: the replay supplies flags, life points and opponent only. Implies `--no-plan`, requires `--target` and `--deck` |
| `--target <card[@ATK\|DEF]>` | describe the board. Repeatable, and repeats count: three identical entries require three copies. Default position ATK |
| `--max-decisions 700` | depth ceiling. In this mode there is no reference line to derive the default from, so set it generously |

The described board is a minimum: a line that also produces something else
satisfies it. This is `--target-subset`, already the default as soon as a target
is posted with `--target`. `--target-exact` restores equality and is kept for
A/B comparisons: it requires an empty spell/trap zone, which is unsatisfiable as
soon as the hand holds a continuous spell.

</details>

<details>
<summary><b>7. Steer a described board</b> — <code>--summon-min --resolve --hint --recipes</code></summary>

A described board gives the search no line to imitate. These flags supply the
missing structure.

```powershell
combosolver.exe template.yrpX --scriptdir <card-scripts> `
    --no-ref `
    --deck "C:\Games\ProjectIgnis\deck\Lunalight.ydk" `
    --hand "57103969|57103969|57103969" `
    --target 54701958 --target 54701958 --target 54701958 `
    --summon-min "Liger Dancer:3" `
    --resolve "PSY-Framelord Omega@field:2" `
    --hint "Crimson Dragon" `
    --recipes 1 `
    --max-decisions 700 `
    --solve-ms 900000 `
    --outdir solutions
```

| Flag | Effect |
|---|---|
| `--summon-min "card:n"` | the line must summon this card at least n times. Checked at the goal |
| `--resolve "card@zone:n"` | the line must resolve this card's effect at least n times before the board |
| `--hint "card"` | the moves that engage this card receive a sampling bonus. No constraint. Repeatable |
| `--recipes 1` | measure distance in summons still to be made, over the observed recipes, instead of in target cards placed |
| `--derive-summon-min` | derive the `--summon-min` constraints from the target board instead of writing them out |

Cards named in `--resolve` and `--summon-min` receive the hint bias
automatically. `--watch` names a card for observation with no constraint, no
gradient and no bias, which is the flag to use when measuring whether the solver
finds something on its own.

</details>

<details>
<summary><b>8. Require the line to hold an answer</b> — <code>--guard --guard-off --opp-hand</code></summary>

`--guard` requires that from the n-th summon onward, at every point where the
opponent could respond, at least one listed clause holds.

```powershell
combosolver.exe duel.yrpX --scriptdir <card-scripts> `
    --solve --optimize `
    --guard "5:Crystal Wing|Zalen@field+Junk Signal@hand" `
    --guard-off "opphand<=2" `
    --solve-ms 600000 `
    --outdir solutions
```

| Flag | Effect |
|---|---|
| `--guard "n:clause\|clause"` | from the n-th summon on, one clause must hold at every opponent window. `+` requires all atoms at once, `\|` separates alternatives |
| `--guard-off "opphand<=N"` | drop the requirement at windows where the opponent holds at most N cards |

A solo hand test gives the opponent no cards. No response window opens, and
`--guard` is satisfied vacuously. `--opp-hand` adds cards to the opponent's
hand; playable cards such as Nibiru are what make the engine open response
windows. Replays produced under `--opp-hand` require the same value to play
back.

```powershell
combosolver.exe duel.yrpX --scriptdir <card-scripts> `
    --solve --optimize `
    --opp-hand "Nibiru, the Primal Being" `
    --guard "5:Crystal Wing|Zalen@field+Junk Signal@hand" `
    --solve-ms 600000 `
    --outdir solutions
```

</details>

<details>
<summary><b>9. Prove the line against a card actually played</b> — <code>--fire</code></summary>

`--fire` adds a card to the opponent's hand and makes the opponent play it at
every window where that is legal, one attempt per window. The search must then
close the board back up from the post-injection state, which turns the static
guard into a dynamic proof.

```powershell
combosolver.exe duel.yrpX --scriptdir <card-scripts> `
    --fire 27204311 `
    --fire-spare "Junk Signal" `
    --fire-no-chain "Crystal Wing" `
    --fire-open `
    --fire-ms 60000 `
    --fire-bake `
    --outdir solutions
```

| Flag | Effect |
|---|---|
| `--fire <card>` | the injected card. Implies `--solve` |
| `--fire-spare <c>` | a card that may be sacrificed answering; a board missing it is also accepted at the goal |
| `--fire-no-chain <c>` | no-chain list specific to the post-injection continuation, which stages a precise answer. Repeatable |
| `--fire-open` | inject only at open windows, empty chain, where the card starts a chain instead of being chained onto our own effects |
| `--fire-ms <ms>` | search budget per window, default 45000 |
| `--fire-bake` | write the card into the header of the replays produced, so they play back in EDOPro with no extra flag |

Without `--fire-bake`, the replays produced only play back with
`--opp-hand <card>` in judge mode.

</details>

<details>
<summary><b>10. Check an existing line against rules</b> — constraints, no <code>--solve</code></summary>

Constraint flags without `--solve` run the tool as a checker, on solver output
and on hand-played replays alike.

```powershell
combosolver.exe solutions\solution_00_b1_a9.yrp --scriptdir <card-scripts> `
    --guard "5:Crystal Wing|Zalen@field+Junk Signal@hand" `
    --resolve "PSY-Framelord Omega@field:2" `
    --material "Chaos Angel:light" `
    --mp1-only
```

</details>

<details>
<summary><b>11. Compare two configurations</b> — <code>--max-rollouts --threads 1 --seed</code></summary>

The default budget is wall time, so two runs of one command perform different
amounts of work. `--max-rollouts` and `--max-nodes` replace it with a fixed
amount of work per worker.

```powershell
combosolver.exe duel.yrpX --scriptdir <card-scripts> --solve `
    --threads 1 --seed 888 `
    --max-rollouts 20000 --max-nodes 500000 `
    --solve-ms 900000
```

With `--threads 1`, two executions of the same command do exactly the same work.
`--threads 1` divides throughput by five to six, and the two configurations must
be run sequentially.

`--seed` is otherwise derived from the clock and printed; passing it back
replays the same rollouts.

</details>

<details>
<summary><b>12. Run a long search</b> — <code>--rounds --carry --archive-fin</code></summary>

`--rounds` splits `--solve-ms` into n rounds and re-injects the best joint line
of each round into the next. `--carry` keeps the archive and the merged policy
across rounds; `--archive-fin` feeds the finisher's own archives back into the
global one.

```powershell
combosolver.exe duel.yrpX --scriptdir <card-scripts> `
    --solve --optimize `
    --rounds 4 --carry --archive-fin `
    --solve-ms 3600000 `
    --outdir solutions
```

</details>

<details>
<summary><b>13. Reuse work from earlier runs</b> — <code>--prior --adapt --options-online --approach</code></summary>

```powershell
combosolver.exe duel.yrpX --scriptdir <card-scripts> `
    --solve --optimize `
    --adapt solutions --adapt-passes 4 `
    --prior solutions --prior-weight 2.0 `
    --options-online 60 `
    --approach best_approach_00.yrp `
    --solve-ms 900000 `
    --outdir solutions2
```

| Flag | Effect |
|---|---|
| `--prior <f\|dir>` | the plan keys of the given lines become initial policy weights |
| `--adapt <f\|dir>` | the same lines, recorded as sequences of decisions, are adapted into the policy before the first rollout: a discriminative signal where `--prior` only gives a per-move bonus |
| `--options-online <s>` | re-mine a macro catalogue every s seconds from the run's own best lines; no external corpus needed |
| `--approach <f.yrp>` | an approach written by an earlier run, `best_approach_*.yrp`, served to the finisher as an extra root. Must come from the same starting duel and the same `--opp-hand` |

</details>

<details>
<summary><b>14. Measure and diagnose</b> — <code>--operators --width --growth --probe-repeat --profile</code></summary>

Each of these instruments or replaces the run rather than searching.

```powershell
# operator table extracted from the deck's Lua scripts, confronted with the replayed plan
combosolver.exe duel.yrpX --scriptdir <card-scripts> --operators

# effective width along the reference line, no search
combosolver.exe duel.yrpX --scriptdir <card-scripts> --width

# growth of the state graph, per depth
combosolver.exe duel.yrpX --scriptdir <card-scripts> `
    --growth --growth-max 14 --growth-ms 20000

# per-rollout histogram of repeated summons, and the recipe distance to one more copy
combosolver.exe duel.yrpX --scriptdir <card-scripts> --solve `
    --summon-min "Liger Dancer:3" --probe-repeat --solve-ms 300000

# hot path profile, rdtsc probes per phase
combosolver.exe duel.yrpX --scriptdir <card-scripts> --solve `
    --profile --solve-ms 300000
```

</details>

---

## Naming a card

`--hand`, `--target`, `--board-add`, `--board-remove`, `--summon`,
`--summon-min`, `--guard`, `--resolve`, `--material`, `--no-activate`,
`--no-chain`, `--hint`, `--watch`, `--opp-hand`, `--fire`, `--fire-spare` and
`--fire-no-chain` accept either the numeric passcode or a fragment of the card
name.

```powershell
--target 54701958
--target "Liger Dancer"
```

A fragment must match exactly one card. When it matches several, the run stops
and lists the candidates with their codes:

```
!! --target : "Liger Dancer" est ambigu (2 cartes) :
      54701958  Lunalight Liger Dancer
     101301030  Lunalight Liger Dancer
```

Copy the code of the printing wanted. Name collisions are common: alternate
artworks, anime versions and Rush Duel cards carry the name of the card they are
based on. A passcode designates one printing.

Three other sources give the same numbers:

- **EDOPro's deck editor.** The card information panel shows the passcode.
- **A `.ydk` file.** After the `#main` line it is one passcode per line, one
  line per copy. Opening the decklist in a text editor is the quickest way to
  read the codes of a deck already built.
- **The card itself.** The passcode is printed at the bottom left of the
  physical card.

---

## Constraint grammar

Constraints act during the search: a move that violates one is removed from the
enumeration.

| Term | Values |
|---|---|
| `card` | a passcode, or a name fragment that resolves uniquely |
| `zone` | `hand`, `field`, `grave`, `banished`, `extra`. Default `field` |
| `attr` | `light`, `dark`, `earth`, `water`, `fire`, `wind`, `divine` |
| position | `ATK`, `DEF`. Default `ATK` |

| Flag | Grammar | Meaning |
|---|---|---|
| `--target` | `card[@ATK\|DEF]` | require this card on the final board |
| `--summon` | `n:card[\|card…]` | the n-th summon, normal or special, must be one of these |
| `--guard` | `n:clause[\|clause…]`, clause = `card[@zone][+…]` | from the n-th summon on, one clause holds at every opponent window. Predicate atom: `oppbanished>=N` |
| `--guard-off` | `opphand<=N` | the guard is not required where the opponent holds at most N cards |
| `--resolve` | `card[@zone][:n]`, default n = 1 | resolve this card's effect at least n times. `@zone` restricts the activation zone |
| `--summon-min` | `card[:n]`, default n = 1 | summon this card at least n times |
| `--material` | `card:attr[,attr…]` | summoning this card must consume at least one material of these attributes |
| `--no-activate` | `card[@zone]`, default field | forbid activating from that zone. Activating from the hand, which places the card, stays allowed |
| `--no-chain` | `card` | never chain this card at a response window. Forced triggers and idle commands stay allowed |

```
--summon      "5:Zalen|Crystal Wing"
--guard       "5:Crystal Wing|Zalen@field+Junk Signal@hand"
--guard       "5:Dis Pater@field+oppbanished>=1"
--guard-off   "opphand<=2"
--resolve     "PSY-Framelord Omega@field:2"
--summon-min  "Liger Dancer:3"
--material    "Chaos Angel:light"
--no-activate "Duel Evolution - Assault Zone"
--no-chain    "Crystal Wing"
```

Points of detail:

- `--guard` requires an answer to be available when the window opens. The guard
  card may already be on the field before the n-th summon.
- `--resolve` and `--summon-min` are checked at the goal. A board matching the
  target without them is not a solution, and the search continues. At most four
  entries in total.
- `@zone` on `--resolve` fixes the activation zone. Omitted, a card with effects
  in two zones satisfies the requirement from either one.
- `--no-self-negate` takes no card: it removes the deck's own negation effects
  from the chain links of our own actions, derived from the declared operator
  table.

---

## Flag reference

`combosolver.exe --help` prints the same list. All 133 flags follow, grouped as
they are there.

<details>
<summary><b>Input and output</b></summary>

| Flag | Default | Meaning |
|---|---|---|
| `--workdir <dir>` | `COMBOSOLVER_WORKDIR` | EDOPro installation. Required unless the environment variable supplies it |
| `--scriptdir <dir>` | scan of `repositories/` | card script set, highest priority first. Repeatable. Use an export contemporary with the replay: a mismatched set makes the replay diverge in silence |
| `--player <0\|1>` | 0 | player whose turn is optimised |
| `--outdir <dir>` | `solutions/` | where the replays produced are written |
| `--verbose` | off | trace every decision |
| `--help`, `-h` | — | the flag list |

</details>

<details>
<summary><b>What to search for</b></summary>

| Flag | Default | Meaning |
|---|---|---|
| `--solve` | off | search for a line towards the target board |
| `--start <replay>` | — | rebuild the reference board from that duel, with its deck, hand and seed. Implies `--solve` |
| `--deck <f.ydk>` | — | rebuild the reference board from that decklist, with no starting replay. Implies `--solve` |
| `--hand <cards>` | the reference's | opening hand, cards separated by `\|`. Used with `--deck` |
| `--opp-hand <c>` | — | add these cards to the opponent's hand, separated by `\|`. Repeatable. Playable cards are needed for the core to open opponent response windows. The replays produced only replay with the same `--opp-hand` |
| `--target <c>` | — | build the target board from scratch; the reference's capture does not enter. Repeatable, repeats count |
| `--board-add <c>` | — | edit the captured board: also require this card. Requires `--deck` or `--start` |
| `--board-remove <c>` | — | edit the captured board: stop requiring this card |
| `--target-subset` | on with `--target` | the final board must contain the target rather than equal it |
| `--target-exact` | off | restore exact equality, for A/B comparisons. Requires an empty spell/trap zone, unsatisfiable as soon as the hand holds a continuous spell |
| `--no-plan` | off | discard the reference's repertoire; the policy starts uniform |
| `--no-ref` | off | goal-only mode: the replay is demoted to a duel template. Implies `--no-plan`, requires `--target` and `--deck` |
| `--approach <f.yrp>` | — | an approach written by an earlier run, served to the finisher as an extra root, full path plus backtracks. Repeatable. Must come from the same starting duel and `--opp-hand` |

</details>

<details>
<summary><b>Line constraints</b></summary>

| Flag | Default | Meaning |
|---|---|---|
| `--summon <spec>` | — | the n-th summon must be one of the cards given. Repeatable |
| `--guard <spec>` | — | from the n-th summon on, a clause holds at every opponent response window, where Nibiru would land |
| `--guard-off <cond>` | — | turn the guard off once the threat is gone |
| `--resolve <spec>` | — | resolve this card's effect at least n times before the board. Repeatable, at most 4 shared with `--summon-min` |
| `--summon-min <spec>` | — | summon this card at least n times. Same machinery as `--resolve`, counted on summons |
| `--material <spec>` | — | summoning this card must consume a material of these attributes. Repeatable |
| `--no-activate <c>` | — | forbid activating this card from a zone. Repeatable |
| `--no-chain <c>` | — | never chain this card at response windows. Playing solo, every chain answers our own actions, so a guard that negates our cards is a useless branch. Repeatable |
| `--no-self-negate` | off | never offer one of the deck's own negation effects on a chain link of ours. Derived from the declared operator table; no card name is compiled in |
| `--mp1-only` | off | the whole combo lives in Main Phase 1: entering the Battle Phase is removed from the enumeration; `-> End Phase` always remains |
| `--turns <n>` | 1 | domain in turns. 2 lets the line cross the opponent's turn, where our only decisions are the quick windows |

</details>

<details>
<summary><b>Search budget</b></summary>

| Flag | Default | Meaning |
|---|---|---|
| `--solve-ms <ms>` | 120000 | search time budget |
| `--threads <n>` | every core | search workers |
| `--rounds <n>` | 1 | internal loop: n rounds share `--solve-ms`, and the best joint line of each is re-injected into the next |
| `--max-decisions <n>` | 1.5x the reference + 32 | depth ceiling of a searched line, in decisions |
| `--max-rollouts <n>` | — | deterministic mode: budget in rollouts per worker instead of wall time. Wall time is the cause of the run-to-run variation |
| `--max-nodes <n>` | — | the same, in nodes expanded per worker |
| `--arena-mb <n>` | 256 | address space reserved for the arena |
| `--tt-mb <n>` | 64 | transposition table shared between workers (lazy SMP), MB per pass. 0 = private tables |
| `--seed <n>` | clock, printed | seed of the rollouts; passing it back replays the same rollouts |

</details>

<details>
<summary><b>Search mechanisms</b></summary>

| Flag | Default | Meaning |
|---|---|---|
| `--no-nrpa` | off | greedy rollouts only, with no learned policy |
| `--novelty <n>` | auto-calibrated | patience of the novelty pruning, calibrated from the width measurement |
| `--no-novelty` | off | disable novelty pruning |
| `--nrpa-bias <x>` | 1.5 | GNRPA bias of the repertoire's moves |
| `--nrpa-keep <x>` | 0.5 | persistence of the policy across restarts: weights attenuated by x instead of restarting from zero. 0 = a virgin policy |
| `--nrpa-alpha <x>` | 1.0 | adaptation step. Small = the policy moves slowly and explores one basin for longer |
| `--nrpa-iters <n>` | 24 | iterations per NRPA level. A level-L call costs n^L rollouts |
| `--nrpa-level <n>` | 3 above 180 s, else 2 | nesting level, 1..4. The effective level is printed |
| `--nrpa-temp <t>` | 1.0 | softmax temperature of the rollouts. t < 1 concentrates the mass on the best ranked moves without changing the ranking |
| `--hint <card>` | — | domain hint: the moves that engage this card, summon, activate or position, receive a sampling bonus. Repeatable |
| `--hint-bias <b>` | 2.0 | weight of that bias. `--resolve` and `--summon-min` cards receive it automatically |
| `--phase-w <x>` | 0 = off | weight subtracted from the logit of a phase change. Ending the turn is irreversible and is otherwise drawn uniformly among the idle choices |
| `--resolve-weight <x>` | 250 | weight of a required resolution in the rollouts' gradient. 100 = one target card |
| `--max-subsets <n>` | 24 | subsets emitted per selection prompt; caps the branching factor of every `SELECT_CARD` / `SELECT_SUM`. Sizes are visited alternating from both ends |
| `--elide-forced` | off | a prompt with a single legal answer is played inline: no table entry, no arena snapshot, no depth, no evaluation. 74.6 % of nodes offer no choice at all |
| `--canonical-zones` | off | explore one representative free zone per zone type. Link arrows and columns can change everything: judge it before trusting it |
| `--adapt-to-peak` / `--no-adapt-to-peak` | on | adapt only the prefix that produced the score. A rollout's score is a max over prefixes; adapting every step made a line peaking at step 200 then wandering for 230 more learn the collapse as strongly as the climb |
| `--no-serial` | off | turn off serialisation by the material balance. On by default, the novelty table reopens at every subgoal of the LP's plan instead of only at every target card placed |
| `--reenter <p>` | 0.5 | probability that a rollout restarts from an archive cell, a rung of the x* ladder, instead of from the root. Only bites under armed serialisation. 0 = the A/B control |
| `--refine-after <n>` | 0 = off | self-refining ladder: re-serialise from the best frontier cell once `sp_max` has stagnated for n measured rollouts |
| `--quota-h` | off | put the path's observed quota uses into the LP's capacities at refinement time (red-black relaxation) |
| `--grid` | off | archive key = the (resolutions, overlap) cell, one elite per cell, uniform re-entry. Requires `--resolve` and a regime without armed serialisation |
| `--carry` | off | under `--rounds`, the global archive and the merged policy persist from one round to the next |
| `--archive-fin` | off | the finisher's search archives enter the global archive, with their paths re-rooted |

</details>

<details>
<summary><b>Goal decomposition</b></summary>

| Flag | Default | Meaning |
|---|---|---|
| `--recipes <w>` | off | h becomes the distance in summons still to be made over the observed recipes, intermediate materials included, so it decreases where the flat h does not move. w = 0 feeds and measures the graph without entering the cost; w > 0 weighs it in. It never prunes: an unknown recipe is worth 1, so at worst h falls back to the flat h |
| `--no-seed-recipes` | off | do not seed the graph from card text; it then learns only from successful summons |
| `--no-seed-quant` | off | seed the named materials only, without the cardinal requirements ("3 Lunalight monsters", "2 Level 4 monsters") |
| `--op-recipes` | off | seed the graph from the declared operators instead of English card text: recipes as codes (`Fusion.AddProcMix*`), plus "this code can be acquired" for every `EFFECT_ADD_CODE` / `EFFECT_CHANGE_CODE` of the deck. The edges posted are printed one by one. Requires `--recipes` |
| `--backward` | off | backward serialisation (Retro*, AO*): a summon is an AND node, arity becomes a decomposition, and the number of subproducts already built enters the novelty partition, which therefore reopens before any target card is placed |
| `--assign` | off | resolved assignment: subset prompts also emit the two extreme subsets in the sense of the recipes. The enumeration's truncation is lexicographic, so past `--max-subsets` the right subset is absent, not rare, and no weight makes up for that |
| `--assign-bias <f>` | — | sampling weight of the moves engaging a code the recipe graph designates as a material |
| `--op-bias <f>` | 0 = inert | weight added to the moves that play a card the backward decomposition requires on the field, i.e. the hosts of the acquisition edges. Requires `--recipes` and `--op-recipes`; its liveness is printed |
| `--hindsight <f>` | 0.5 | every extra deck monster actually summoned becomes a substitute goal, and the best line reaching it undergoes the gradient at f x alpha |
| `--no-hindsight` | — | turn hindsight off |
| `--hindsight-k <n>` | 16 | substitute goals kept at most |
| `--landmarks <f\|d>` | — | learn, from resolved plans, the facts (card, zone, count) every plan reaches, in which order and how many times. Repeatable. The graph is printed before it is allowed to weigh |
| `--landmark-w <f>` | — | weight of a landmark achievement in the rollout score, in units of material (one target card placed = 100). 0 = learned and measured without weighing |
| `--landmark-h <f>` | — | weight of the landmark h in the finisher, same entry point as `--recipes` |
| `--derive-summon-min` | off | derive the `--summon-min` constraints from the target board. 3x Liger Dancer = three Fusion summon events, never "three Polymerizations": the trigger varies. The counting is always printed; this flag wires it into the constraints |

</details>

<details>
<summary><b>Policy learning from solved lines</b></summary>

| Flag | Default | Meaning |
|---|---|---|
| `--prior <f\|dir>` | — | the plan keys of the given lines, each recorded on its own duel, become initial policy weights. A `.yrp` file or a directory. Repeatable |
| `--prior-weight <x>` | 2.0 | weight of a move present in the whole corpus; proportional to its frequency otherwise |
| `--adapt <f\|dir>` | — | the same lines recorded as sequences of decisions, legal choices plus the chosen one, and adapted into the policy before the first rollout. Repeatable |
| `--adapt-passes <n>` | 4 | adaptation passes per line. 0 disables the mechanism without touching the recording, which is the A/B |
| `--options <n>` | 0 = off | catalogue of n macros mined from the `--adapt` corpus and offered as one sampling unit to the rollouts |
| `--options-support <n>` | 2 | minimum occurrences of a macro |
| `--options-len <n>` | 8 | maximum length of a macro |
| `--options-window <n>` | 0 = no guard | only offer a macro within ±n decisions of its original position in the corpus |
| `--options-ctx <n>` | -1 = off | semantic guard: only offer a macro when the current context is compatible with a corpus occurrence, target cards placed exact and hand within ±n |
| `--options-online <s>` | 0 = off | re-mine the catalogue every s seconds from the run's own best lines. Implies `--options 256` when `--options` is not given |
| `--options-pool <n>` | 12 | living corpus: lines kept in total |
| `--options-per-worker <n>` | 2 | and at most n per worker; without a quota the workers pour the same shared best line in sixteen times |

</details>

<details>
<summary><b>Head bandit (MCPS)</b></summary>

| Flag | Default | Meaning |
|---|---|---|
| `--qhat <k>` | 0 = off | over the first k decisions of a rollout, the move is chosen by argmax of (n Q + n^ Q^)/(n + n^), where Q is the mean reward through this node then this move and Q^ the mean over all rollouts containing this move and those of the path, in any order. Weights are proportional to the sample sizes, so there is no bias hyperparameter. Those k decisions are excluded from the gradient |
| `--qhat-window <W>` | 4096 | sliding window of rollouts, per worker. The measured memory is printed |
| `--qhat-rho <r>` | 32 | visits after which a non-root node freezes its permutation statistic |
| `--qhat-nodes <n>` | 65536 | cap on the bandit's nodes per worker |
| `--no-qhat-probe` | off | do not print the first-decision probe |
| `--ctx-shrink <k>` | negative = off | two-level policy: one weight per move and one per (move, context), mixed convexly by s = n/(n+k), n being the evidence of the contextual cell. The agreement curve printed by `--adapt` calibrates k without spending a run |
| `--ctx-max <n>` | 262144 | cap on the contextual level's entries per worker. 0 = unlimited |

</details>

<details>
<summary><b>Finisher</b></summary>

| Flag | Default | Meaning |
|---|---|---|
| `--finisher <mode>` | `levin` | `levin` = Go-Explore archive + backtracks + Levin Tree Search over the policy; `mono` = the single best state; `ab` = both at equal budget |
| `--archive-k <n>` | 16 | size of the finisher's state archive |
| `--finisher-min <ms>` | 0 | minimum budget reserved for the finisher. 0 = the original split |
| `--levin-h <x>` | 1.0 | PHS* weight of the distance to the goal, missing cards plus missing resolutions. 0 = pure Levin, blind to the goal |
| `--reroot` | off | sqrt-LTS with a hard rerooter: re-root at every hint, i.e. whenever the number of target cards placed changes. With no hint, inert |
| `--reroot-h <a>` | 0 = off | sqrt-LTS-H with a heuristic rerooter: weight exp(-a*h/h0) on every node, hence active even when no hint lands. a = inverse temperature. Exclusive with `--reroot` |
| `--dive-full` / `--no-dive-full` | on | push an arena level at every replayed chain node |
| `--merged-pop` / `--no-merged-pop` | on | merged arena pop when returning to the shared ancestor |
| `--lifo-ties` | off | at equal Levin cost, extract the node queued last. Refuted on its own |
| `--finisher-post-goal` | off | under `--optimize`, a goal node continues instead of stopping (post-goal recovery) |
| `--finisher-options` | off | the catalogue's macros become edges of the Levin tree, at cost log 1/pi, advancing k decisions, an abort being a dead edge |

</details>

<details>
<summary><b>Cost optimisation</b></summary>

| Flag | Default | Meaning |
|---|---|---|
| `--optimize` | off | anytime cost optimisation: no stop at the first solution, lexicographic goal score (burned, actions, decisions), rollouts continue past the goal so recoveries reduce the burned count, and the finisher runs even when lines already exist |
| `--burn-slack <n>` | 6 | slack of the burned B&B bound: cut the states above `best_burned + n`. Burned cards are not monotonic, because of recoveries, and the slack is measured on the reference. 255 = off |
| `--burn-limit <n>` | 0 = none | seed of the bound: best burned count known in advance |
| `--no-burn-share` | off | do not share the burned bound between workers. By default a worker that improves it cuts for all |

</details>

<details>
<summary><b>Opponent test</b></summary>

| Flag | Default | Meaning |
|---|---|---|
| `--fire <card>` | — | add the card to the opponent's hand and make the opponent play it at every legal window, one attempt per window; the search must close the board back up from the post-injection state. Implies `--solve`. The replays produced only replay with `--opp-hand <card>` in judge mode |
| `--fire-spare <c>` | — | card that may be sacrificed to answer: the target board without it is also accepted at the goal |
| `--fire-ms <ms>` | 45000 | search budget per window |
| `--fire-bake` | off | bake the drawn card into the header of the replays produced, opponent deck, served into the hand by the pseudo-shuffle, so they replay from their own file with no flag. The card replaces the last card of the opponent's original hand |
| `--fire-no-chain <c>` | — | no-chain list specific to the post-injection continuation; the global `--no-chain` lists are lifted there. Repeatable |
| `--fire-open` | off | inject only at open windows, empty chain, where the drawn card starts a chain instead of being chained onto our effects |

</details>

<details>
<summary><b>Measurement and diagnostics</b></summary>

| Flag | Default | Meaning |
|---|---|---|
| `--growth` | off | measure the growth of the state graph |
| `--growth-max <n>` | 14 | maximum depth explored |
| `--growth-ms <ms>` | 20000 | time budget per depth |
| `--width` | off | measure the effective width, IW atoms, along the reference line, with no search |
| `--watch <card>` | — | card observed by `--probe-repeat`, with no constraint, no gradient and no hint bias. Repeatable, at most 4 |
| `--probe-repeat` | off | per `--summon-min` / `--resolve` card, the histogram of summons per rollout and, at the first one, the recipe distance to one more copy against the same distance from the starting state. Separates the second copy never attempted from the second always lost. Implies `--recipes 0` |
| `--operators` | off | extract the operator table from the deck's Lua scripts — preconditions, product, granted state, recipes — print it, then confront it with the replayed plan. The constants come from the game's `constant.lua`: no card is named in the code |
| `--quota-legacy` | off | replay the earlier quota derivation by effect classes instead of the derivation from the LP's duals |
| `--resolve-legacy` | off | do not compile the `--resolve` / `--summon-min` requirements into the material balance |
| `--profile` | off | hot path profile, rdtsc probes per phase, "everything else" line included |
| `--no-arena` | off | system allocator, no snapshot. For comparison |
| `--keep-gc` | off | leave the Lua garbage collector running. For comparison |

</details>

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

A line's cost is three numbers, compared in this order:

1. **Burned cards** — in the graveyard or banished.
2. **Actions** — summons and activations.
3. **Decisions** — prompts answered.

The cards on the final board are fixed by the target, so burned cards are the
variable part.

After a search the report adds the ranking of the lines found, the number
examined and the number written. A flag with no effect on the run is marked
`!! INERT`.

The report is in French. The command line, the help text and this document are
in English.

---

## How it works

- **Rules.** The solver contains no implementation of the game. It runs the
  engine and, at each prompt, enumerates the answers the engine reports as
  legal. Each candidate line is replayed in a fresh duel and re-checked before
  being written.
- **Search space.** A turn's moves can be ordered in a very large number of
  ways, and many orderings reach the same position. The solver identifies equal
  positions and expands each one once.
- **Snapshots.** Trying an alternative move requires returning to an earlier
  point in the duel. The solver snapshots the engine's memory and restores it,
  which costs orders of magnitude less than replaying the turn from the start.
- **Policy.** The search samples lines, maintains a policy weighted by the
  results, and adapts it towards the best line found. A move is identified by
  the cards it engages, so a policy learned on one deck applies to another.
- **Finisher.** Sampling reaches the neighbourhood of the target board and
  seldom completes the last decisions. The solver stores its best positions and
  runs a complete search from them, ordered by the learned policy.
- **Heuristic.** The number of target cards on the field stays flat for most of
  a combo, since the board fills at the end. The solver derives what each summon
  consumes and produces, and measures distance as the number of summons still
  required.

---

## Limitations

- Difficult boards do not convert on every run. Allow a full budget and repeat
  the run before concluding that a board is unreachable.
- The default domain is a single turn. `--turns 2` covers lines that cross into
  the opponent's turn, with less coverage.
- A replay whose card script set is unavailable cannot be reproduced.

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

The search implements published methods:

| Component | Method | Source |
|---|---|---|
| Sampled policy | NRPA, GNRPA | [arXiv:2003.10024](https://arxiv.org/abs/2003.10024) |
| Complete finish | Levin Tree Search, PHS* | [arXiv:2103.11505](https://arxiv.org/abs/2103.11505), [arXiv:2412.05196](https://arxiv.org/abs/2412.05196) |
| Novelty pruning | Iterated Width | [arXiv:1801.03354](https://arxiv.org/abs/1801.03354) |
| Archive of positions | Go-Explore | [arXiv:2004.12919](https://arxiv.org/abs/2004.12919) |
| Relabelling | Hindsight experience replay | Andrychowicz et al., NeurIPS 2017 |
| Macro-operators | Selection by Levin loss | [arXiv:2410.11262](https://arxiv.org/abs/2410.11262) |
| Landmarks | Learned landmarks | [arXiv:2508.21564](https://arxiv.org/abs/2508.21564) |
| Goal decomposition | Retrosynthesis search | [arXiv:2006.15820](https://arxiv.org/abs/2006.15820), [arXiv:2407.06334](https://arxiv.org/abs/2407.06334) |
| Head bandit | MCPS | [arXiv:2510.06381](https://arxiv.org/abs/2510.06381) |
| Resolved assignment | Subset selection | [arXiv:2010.12001](https://arxiv.org/abs/2010.12001) |
