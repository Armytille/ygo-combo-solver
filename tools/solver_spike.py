#!/usr/bin/env python3
"""Milestone 0 of the combo solver: instrumented replay of the reference line.

Boots a headless ocgcore duel from a .yrpX, feeds it back the answers recorded
in the embedded yrp1, and measures what everything else depends on:

  * the real number of decision points and their type;
  * the branching factor the core offers at each decision;
  * the cost of the reference line (C_ref, A_ref, D_ref);
  * the target board at the end of the target player's turn.

usage: solver_spike.py <replay.yrpX> [--workdir <edopro-install>] [--verbose]
"""

import argparse
import math
import os
import struct
import sys
from collections import Counter, defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import ocgcore as oc
from inspect_replay import Replay

DEFAULT_DLL = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "..", "edopro", "bin", "x64", "release", "ocgcore.dll")

MSG_RETRY, MSG_HINT, MSG_WIN = 1, 2, 5
MSG_NEW_TURN, MSG_NEW_PHASE = 40, 41
MSG_SUMMONING, MSG_SPSUMMONING, MSG_FLIPSUMMONING, MSG_CHAINING = 60, 62, 64, 70

PROMPT_NAMES = {
    10: "SELECT_BATTLECMD", 11: "SELECT_IDLECMD", 12: "SELECT_EFFECTYN",
    13: "SELECT_YESNO", 14: "SELECT_OPTION", 15: "SELECT_CARD",
    16: "SELECT_CHAIN", 18: "SELECT_PLACE", 19: "SELECT_POSITION",
    20: "SELECT_TRIBUTE", 22: "SELECT_COUNTER", 23: "SELECT_SUM",
    24: "SELECT_DISFIELD", 25: "SORT_CARD", 26: "SELECT_UNSELECT_CARD",
    140: "ANNOUNCE_RACE", 141: "ANNOUNCE_ATTRIB", 142: "ANNOUNCE_CARD",
    143: "ANNOUNCE_NUMBER",
}


class Buf:
    def __init__(self, b):
        self.b, self.p = b, 0

    def u8(self):
        v = self.b[self.p]; self.p += 1; return v

    def u16(self):
        v = struct.unpack_from("<H", self.b, self.p)[0]; self.p += 2; return v

    def u32(self):
        v = struct.unpack_from("<I", self.b, self.p)[0]; self.p += 4; return v

    def u64(self):
        v = struct.unpack_from("<Q", self.b, self.p)[0]; self.p += 8; return v

    def loc(self):
        return (self.u8(), self.u8(), self.u32(), self.u32())


def nsubsets(n, lo, hi):
    """Number of subsets of size lo..hi out of n."""
    hi = min(hi, n)
    if lo > hi:
        return 0
    return sum(math.comb(n, k) for k in range(max(lo, 0), hi + 1))


def parse_prompt(msg, payload):
    """Extracts the branching factor of a MSG_SELECT_*.

    Returns (n_raw, n_dedup, detail). n_dedup applies the reduction by
    multiset of codes described in the design notes.
    """
    b = Buf(payload)
    try:
        if msg == 11:  # SELECT_IDLECMD
            b.u8()
            groups, detail = [], {}
            for key, stride in (("summon", 10), ("spsummon", 10),
                                ("repos", 7), ("mset", 10), ("sset", 10)):
                n = b.u32()
                codes = []
                for _ in range(n):
                    codes.append(b.u32())
                    b.p += stride - 4
                detail[key] = n
                groups.append(codes)
            n_act = b.u32()
            acts = []
            for _ in range(n_act):
                code = b.u32(); b.p += 1 + 1 + 4
                desc = b.u64(); b.p += 1
                acts.append((code, desc))
            detail["activate"] = n_act
            to_bp, to_ep, shuffle = b.u8(), b.u8(), b.u8()
            detail["to_bp"], detail["to_ep"], detail["shuffle"] = to_bp, to_ep, shuffle
            raw = sum(len(g) for g in groups) + n_act + to_bp + to_ep + shuffle
            ded = sum(len(set(g)) for g in groups) + len(set(acts)) \
                + to_bp + to_ep + shuffle
            return raw, ded, detail

        if msg == 10:  # SELECT_BATTLECMD
            b.u8()
            n_act = b.u32()
            for _ in range(n_act):
                b.p += 4 + 1 + 1 + 4 + 8 + 1
            n_atk = b.u32()
            for _ in range(n_atk):
                b.p += 4 + 1 + 1 + 4 + 1
            to_m2, to_ep = b.u8(), b.u8()
            raw = n_act + n_atk + to_m2 + to_ep
            return raw, raw, {"activate": n_act, "attack": n_atk}

        if msg in (12, 13):  # EFFECTYN / YESNO
            return 2, 2, {}

        if msg == 14:  # SELECT_OPTION
            b.u8()
            n = b.u8()
            return n, n, {"options": n}

        if msg in (15, 20):  # SELECT_CARD / SELECT_TRIBUTE
            b.u8()
            cancelable = b.u8()
            lo, hi, n = b.u32(), b.u32(), b.u32()
            codes = []
            for _ in range(n):
                codes.append(b.u32())
                b.p += 10 if msg == 15 else 7
            raw = nsubsets(n, lo, hi) + (1 if cancelable else 0)
            ded = nsubsets(len(set(codes)), lo, hi) + (1 if cancelable else 0)
            return raw, ded, {"n": n, "min": lo, "max": hi,
                              "distincts": len(set(codes))}

        if msg == 26:  # SELECT_UNSELECT_CARD
            b.u8(); fin, canc = b.u8(), b.u8()
            lo, hi = b.u32(), b.u32()
            n = b.u32()
            codes = []
            for _ in range(n):
                codes.append(b.u32()); b.p += 10
            nun = b.u32()
            raw = n + nun + (1 if (fin or canc) else 0)
            ded = len(set(codes)) + nun + (1 if (fin or canc) else 0)
            return raw, ded, {"selectables": n, "unselectables": nun,
                              "min": lo, "max": hi}

        if msg == 16:  # SELECT_CHAIN
            player = b.u8()
            spe, forced = b.u8(), b.u8()
            ht_self, ht_oppo = b.u32(), b.u32()
            n = b.u32()
            items = []
            for _ in range(n):
                code = b.u32(); b.p += 10
                desc = b.u64(); b.p += 1
                items.append((code, desc))
            raw = n + (0 if forced else 1)
            ded = len(set(items)) + (0 if forced else 1)
            return raw, ded, {"player": player, "chains": n, "forced": forced,
                              "spe": spe, "timing": f"0x{ht_self:x}"}

        if msg in (18, 24):  # SELECT_PLACE / SELECT_DISFIELD
            b.u8()
            count = b.u8()
            flag = b.u32()
            free = 0
            for owner in range(2):
                for seq in range(7):                       # MZONE
                    if not (flag & (1 << (seq + owner * 16))):
                        free += 1
                for seq in range(8):                       # SZONE
                    if not (flag & (1 << (seq + 8 + owner * 16))):
                        free += 1
            raw = nsubsets(free, count, count)
            return raw, raw, {"count": count, "zones_libres": free}

        if msg == 19:  # SELECT_POSITION
            b.u8(); b.u32()
            pos = b.u8() & 0xf
            n = bin(pos).count("1")
            return n, n, {"positions": pos}

        if msg == 22:  # SELECT_COUNTER
            b.u8(); b.u16()
            count = b.u16()
            n = b.u32()
            return n, n, {"cards": n, "count": count}

        if msg == 23:  # SELECT_SUM
            b.u8(); mode = b.u8()
            acc, lo, hi = b.u32(), b.u32(), b.u32()
            nmust = b.u32()
            for _ in range(nmust):
                b.p += 4 + 10 + 4
            n = b.u32()
            raw = nsubsets(n, 1, n)
            return raw, raw, {"n": n, "must": nmust, "acc": acc, "mode": mode}

        if msg == 25:  # SORT_CARD
            b.u8()
            n = b.u32()
            return math.factorial(min(n, 10)), 1, {"n": n}

    except (struct.error, IndexError):
        pass
    return -1, -1, {}


def board_snapshot(duel, con):
    """State of a player's public zones, in the sense of the first equivalence criterion."""
    flags = (oc.QUERY_CODE | oc.QUERY_POSITION | oc.QUERY_TYPE
             | oc.QUERY_LEVEL | oc.QUERY_ATTACK | oc.QUERY_DEFENSE
             | oc.QUERY_OVERLAY_CARD | oc.QUERY_COUNTERS | oc.QUERY_LINK)
    out = {}
    for loc in (oc.LOCATION_MZONE, oc.LOCATION_SZONE):
        cards = duel.query_location(con, loc, flags)
        out[oc.LOC_NAMES[loc]] = cards
    for loc in (oc.LOCATION_HAND, oc.LOCATION_GRAVE, oc.LOCATION_REMOVED,
                oc.LOCATION_EXTRA, oc.LOCATION_DECK):
        out[oc.LOC_NAMES[loc]] = duel.count(con, loc)
    return out


def fmt_board(snap, names):
    lines = []
    for loc in ("MZONE", "SZONE"):
        for i, c in enumerate(snap[loc]):
            if not c:
                continue
            code = c.get("code", 0)
            pos = oc.POS_NAMES.get(c.get("position", 0), str(c.get("position")))
            ov = c.get("overlay") or []
            extra = f" +{len(ov)} mat" if ov else ""
            ctr = c.get("counters") or []
            extra += f" ctr={ctr}" if ctr else ""
            lines.append(f"      {loc}[{i}] {code:>9} {names.get(code, '?')[:34]:<34}"
                         f" {pos}{extra}")
    counts = "  ".join(f"{k}={snap[k]}" for k in
                       ("HAND", "DECK", "EXTRA", "GRAVE", "REMOVED"))
    lines.append(f"      {counts}")
    return "\n".join(lines)


def load_names(workdir):
    import glob
    import sqlite3
    names = {}
    paths = glob.glob(os.path.join(workdir, "expansions", "*.cdb"))
    paths += glob.glob(os.path.join(workdir, "repositories", "**", "*.cdb"),
                       recursive=True)
    for p in paths:
        try:
            con = sqlite3.connect(f"file:{p}?mode=ro", uri=True)
            for cid, nm in con.execute("select id,name from texts"):
                names[cid] = nm or ""
            con.close()
        except sqlite3.Error:
            pass
    return names


def run(path, workdir, dll, verbose, scriptdirs=None):
    rp = Replay(open(path, "rb").read())
    if rp.yrp is None:
        print("!! pas de yrp1 embarque : reponses irrecuperables, abandon.")
        return 1
    y = rp.yrp

    print(f"=== chargement ===")
    db = oc.CardDB(workdir)
    print(f"  cartes            : {len(db)} depuis {len(db.sources)} base(s)")
    scripts = oc.ScriptProvider(workdir, override_roots=scriptdirs)
    print(f"  dossiers scripts  : {len(scripts.dirs)}"
          + (f"  (override: {scriptdirs})" if scriptdirs else ""))
    core = oc.Core(os.path.abspath(dll))
    print(f"  ocgcore           : v{core.version()[0]}.{core.version()[1]}")

    errors = []

    def on_log(typ, line):
        if typ == 0:
            errors.append(line)

    duel = oc.Duel(core, db, scripts, y.seed, y.duel_flags,
                   y.start_lp, y.start_hand, y.draw_count, log=on_log)

    hand_test = bool(y.flag & 0x40)
    for team in (0, 1):
        if team >= len(y.decks):
            break
        main, extra = y.decks[team]
        for code in main:
            duel.new_card(code, team, team, oc.LOCATION_DECK)
        for code in extra:
            duel.new_card(code, team, team, oc.LOCATION_EXTRA)
    if hand_test:
        duel.exec_lua("Debug.ReloadFieldEnd()")
    duel.start()

    print(f"\n=== rejeu de la ligne de reference ===")
    print(f"  mode              : {'HAND TEST' if hand_test else 'duel normal'}")
    print(f"  reponses a rejouer: {len(y.responses)}")

    prompts = Counter()
    branching = defaultdict(list)
    idle_detail = []
    acts = Counter()
    turn, phase, turn_player = 0, None, None
    target_board = None
    target_turn_end_at = None
    ri = 0
    retries = 0
    trace = []

    while True:
        status = duel.process()
        for msg, payload in duel.messages():
            if msg == MSG_NEW_TURN:
                turn += 1
                turn_player = payload[0] if payload else -1
                if turn == 2 and target_board is None:
                    # the target player's turn has just ended
                    target_board = (board_snapshot(duel, 0),
                                    board_snapshot(duel, 1))
                    target_turn_end_at = ri
            elif msg == MSG_NEW_PHASE and len(payload) >= 2:
                phase = struct.unpack_from("<H", payload)[0]
            elif msg in (MSG_SUMMONING, MSG_SPSUMMONING, MSG_FLIPSUMMONING,
                         MSG_CHAINING):
                acts[msg] += 1
            elif msg == MSG_RETRY:
                retries += 1
            elif msg == MSG_WIN:
                pass
            if msg in PROMPT_NAMES:
                raw, ded, detail = parse_prompt(msg, payload)
                prompts[msg] += 1
                branching[msg].append((raw, ded))
                if msg == 11:
                    idle_detail.append(detail)
                if verbose:
                    trace.append(
                        f"  #{ri:<4} T{turn} {oc.PHASES.get(phase, phase)}"
                        f"  {PROMPT_NAMES[msg]:<20} brut={raw:<8} dedup={ded:<6}"
                        f" {detail}")

        if status == oc.DUEL_STATUS_AWAITING:
            if ri >= len(y.responses):
                # Normal: the recording stops when the player leaves, not at the
                # end of the duel. Only an EARLY shortfall would be suspicious.
                print(f"\n  (fin de l'enregistrement : {ri} reponses epuisees, "
                      f"le duel continuait)")
                break
            duel.set_response(y.responses[ri])
            ri += 1
        elif status == oc.DUEL_STATUS_END:
            break
        elif status != oc.DUEL_STATUS_CONTINUE:
            print(f"\n!! statut inattendu {status}")
            break

    if verbose:
        print("\n--- trace des decisions ---")
        for line in trace:
            print(line)

    print(f"\n=== resultats ===")
    print(f"  reponses consommees : {ri} / {len(y.responses)}")
    print(f"  MSG_RETRY           : {retries}"
          + ("   <-- rejeu DIVERGENT" if retries else "   (rejeu fidele)"))
    print(f"  tours joues         : {turn}")
    if errors:
        print(f"  erreurs du core     : {len(errors)}")
        for e in errors[:8]:
            print(f"      {e}")

    total_prompts = sum(prompts.values())
    print(f"\n--- points de decision par type ---")
    print(f"  {'type':<22}{'n':>6}{'brut moy':>11}{'brut max':>10}"
          f"{'dedup moy':>11}{'dedup max':>11}")
    for msg, n in sorted(prompts.items(), key=lambda x: -x[1]):
        vals = [v for v in branching[msg] if v[0] >= 0]
        if vals:
            rawv = [v[0] for v in vals]
            dedv = [v[1] for v in vals]
            print(f"  {PROMPT_NAMES[msg]:<22}{n:>6}{sum(rawv)/len(rawv):>11.1f}"
                  f"{max(rawv):>10}{sum(dedv)/len(dedv):>11.1f}{max(dedv):>11}")
        else:
            print(f"  {PROMPT_NAMES[msg]:<22}{n:>6}{'?':>11}")
    print(f"  {'TOTAL':<22}{total_prompts:>6}")

    prod_raw = prod_ded = 0.0
    for msg, vals in branching.items():
        for raw, ded in vals:
            if raw > 0:
                prod_raw += math.log10(raw)
            if ded > 0:
                prod_ded += math.log10(ded)
    print(f"\n--- ordre de grandeur du branchement le long de CETTE ligne ---")
    print(f"  produit des branchements bruts  : 10^{prod_raw:.1f}")
    print(f"  produit apres dedup par code    : 10^{prod_ded:.1f}")
    print("  (indicateur d'echelle, PAS un decompte de feuilles : changer un "
          "choix precoce modifie")
    print("   les prompts suivants, les facteurs ne se combinent donc pas "
          "librement.)")

    a_ref = sum(acts.values())
    print(f"\n--- cout de la ligne de reference ---")
    print(f"  invocations normales   : {acts[MSG_SUMMONING]}")
    print(f"  invocations speciales  : {acts[MSG_SPSUMMONING]}")
    print(f"  invocations flip       : {acts[MSG_FLIPSUMMONING]}")
    print(f"  activations            : {acts[MSG_CHAINING]}")
    print(f"  A_ref (tier 2)         : {a_ref}")
    print(f"  D_ref (tier 3)         : {ri}")

    if target_board:
        names = load_names(workdir)
        p0, p1 = target_board
        main0 = len(y.decks[0][0]) if y.decks else 0
        extra0 = len(y.decks[0][1]) if y.decks else 0
        left = p0["HAND"] + p0["DECK"] + p0["EXTRA"]
        onboard = sum(1 for c in p0["MZONE"] + p0["SZONE"] if c)
        burned = p0["GRAVE"] + p0["REMOVED"]
        print(f"\n--- board cible (fin du tour 1, joueur 0) ---")
        print(f"    capture apres la reponse #{target_turn_end_at}")
        print(fmt_board(p0, names))
        print(f"\n  C_ref (tier 1) = cartes hors main/deck/extra")
        print(f"    total joueur 0        : {main0 + extra0}")
        print(f"    restant main+deck+xtra: {left}")
        print(f"    => consommees         : {main0 + extra0 - left}")
        print(f"       dont sur le board  : {onboard}  (constant : impose par "
              f"le critere d'equivalence, donc sans effet sur le classement)")
        print(f"       dont brulees GY/ban: {burned}  <-- c'est CELA que le "
              f"solveur doit minimiser")
        onfield = sum(1 for c in p1["MZONE"] + p1["SZONE"] if c)
        if onfield:
            print(f"\n  (joueur 1 a {onfield} carte(s) sur le terrain)")
    else:
        print("\n!! le tour du joueur cible ne s'est jamais termine, "
              "board cible non capture")

    if scripts.misses:
        print(f"\n  scripts introuvables ({len(scripts.misses)}) : "
              f"{sorted(scripts.misses)[:6]}")

    duel.destroy()
    return 0


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("replay")
    ap.add_argument("--workdir", default=os.environ.get("COMBOSOLVER_WORKDIR"),
                    required="COMBOSOLVER_WORKDIR" not in os.environ)
    ap.add_argument("--dll", default=DEFAULT_DLL)
    ap.add_argument("--verbose", "-v", action="store_true")
    ap.add_argument("--scriptdir", action="append", default=None,
                    help="jeu de scripts prioritaire (repetable). A utiliser "
                         "avec un export du depot date de l'epoque du replay.")
    a = ap.parse_args()
    sys.exit(run(a.replay, a.workdir, a.dll, a.verbose, a.scriptdir))
