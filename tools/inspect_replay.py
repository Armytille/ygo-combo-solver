#!/usr/bin/env python3
"""Inspecteur de replay EDOPro (.yrpX / .yrp).

Etape 0.a du combo solver : valider qu'un replay est exploitable (yrp1 embarque
present, decks lisibles, reponses recuperables) et sortir les metriques de
reference avant d'ecrire quoi que ce soit en C++.

Format decode d'apres edopro/gframe/replay.cpp et edopro/gframe/replay.h.
"""

import lzma
import struct
import sys
from collections import Counter

REPLAY_YRP1 = 0x31707279
REPLAY_YRPX = 0x58707279

FLAGS = [
    (0x001, "COMPRESSED"),
    (0x002, "TAG"),
    (0x004, "DECODED"),
    (0x008, "SINGLE_MODE"),
    (0x010, "LUA64"),
    (0x020, "NEWREPLAY"),
    (0x040, "HAND_TEST"),
    (0x080, "DIRECT_SEED"),
    (0x100, "64BIT_DUELFLAG"),
    (0x200, "EXTENDED_HEADER"),
]

OLD_REPLAY_MODE = 231
MSG_AI_NAME = 163
MSG_NEW_TURN = 40

MSG_NAMES = {
    1: "RETRY", 2: "HINT", 3: "WAITING", 4: "START", 5: "WIN", 6: "UPDATE_DATA",
    7: "UPDATE_CARD", 8: "REQUEST_DECK", 10: "SELECT_BATTLECMD", 11: "SELECT_IDLECMD",
    12: "SELECT_EFFECTYN", 13: "SELECT_YESNO", 14: "SELECT_OPTION", 15: "SELECT_CARD",
    16: "SELECT_CHAIN", 18: "SELECT_PLACE", 19: "SELECT_POSITION", 20: "SELECT_TRIBUTE",
    21: "SORT_CHAIN", 22: "SELECT_COUNTER", 23: "SELECT_SUM", 24: "SELECT_DISFIELD",
    25: "SORT_CARD", 26: "SELECT_UNSELECT_CARD", 30: "CONFIRM_DECKTOP",
    31: "CONFIRM_CARDS", 32: "SHUFFLE_DECK", 33: "SHUFFLE_HAND", 34: "REFRESH_DECK",
    35: "SWAP_GRAVE_DECK", 36: "SHUFFLE_SET_CARD", 37: "REVERSE_DECK", 38: "DECK_TOP",
    39: "SHUFFLE_EXTRA", 40: "NEW_TURN", 41: "NEW_PHASE", 42: "CONFIRM_EXTRATOP",
    50: "MOVE", 53: "POS_CHANGE", 54: "SET", 55: "SWAP", 56: "FIELD_DISABLED",
    60: "SUMMONING", 61: "SUMMONED", 62: "SPSUMMONING", 63: "SPSUMMONED",
    64: "FLIPSUMMONING", 65: "FLIPSUMMONED", 70: "CHAINING", 71: "CHAINED",
    72: "CHAIN_SOLVING", 73: "CHAIN_SOLVED", 74: "CHAIN_END", 75: "CHAIN_NEGATED",
    76: "CHAIN_DISABLED", 80: "CARD_SELECTED", 81: "RANDOM_SELECTED",
    83: "BECOME_TARGET", 90: "DRAW", 91: "DAMAGE", 92: "RECOVER", 93: "EQUIP",
    94: "LPUPDATE", 95: "UNEQUIP", 96: "CARD_TARGET", 97: "CANCEL_TARGET",
    100: "PAY_LPCOST", 101: "ADD_COUNTER", 102: "REMOVE_COUNTER", 110: "ATTACK",
    111: "BATTLE", 112: "ATTACK_DISABLED", 113: "DAMAGE_STEP_START",
    114: "DAMAGE_STEP_END", 120: "MISSED_EFFECT", 121: "BE_CHAIN_TARGET",
    122: "CREATE_RELATION", 123: "RELEASE_RELATION", 130: "TOSS_COIN",
    131: "TOSS_DICE", 132: "ROCK_PAPER_SCISSORS", 133: "HAND_RES",
    140: "ANNOUNCE_RACE", 141: "ANNOUNCE_ATTRIB", 142: "ANNOUNCE_CARD",
    143: "ANNOUNCE_NUMBER", 160: "CARD_HINT", 161: "TAG_SWAP", 162: "RELOAD_FIELD",
    163: "AI_NAME", 164: "SHOW_HINT", 165: "PLAYER_HINT", 170: "MATCH_KILL",
    180: "CUSTOM_MSG", 190: "REMOVE_CARDS", 231: "OLD_REPLAY_MODE",
}

# Prompts qui exigent une reponse du joueur : ce sont les points de branchement
# du solveur (cf. Processors::NeedsAnswer, processor_visit.cpp:19).
PROMPTS = {10, 11, 12, 13, 14, 15, 16, 18, 19, 20, 22, 23, 24, 25, 26,
           140, 141, 142, 143}


def decode_flags(flag):
    return " | ".join(name for bit, name in FLAGS if flag & bit) or "(aucun)"


class Reader:
    def __init__(self, data):
        self.d = data
        self.p = 0

    def raw(self, n):
        if self.p + n > len(self.d):
            raise EOFError(f"lecture de {n} octets a l'offset {self.p}, "
                           f"taille {len(self.d)}")
        out = self.d[self.p:self.p + n]
        self.p += n
        return out

    def u8(self):
        return self.raw(1)[0]

    def u16(self):
        return struct.unpack("<H", self.raw(2))[0]

    def u32(self):
        return struct.unpack("<I", self.raw(4))[0]

    def u64(self):
        return struct.unpack("<Q", self.raw(8))[0]

    def name(self):
        return self.raw(40).decode("utf-16-le").split("\x00")[0]

    def eof(self):
        return self.p >= len(self.d)


class Replay:
    def __init__(self, blob, label="replay"):
        self.label = label
        self.players = []
        self.decks = []
        self.responses = []
        self.packets = []
        self.rule_cards = []
        self.scriptname = ""
        self.turn_count = 0
        self.yrp = None
        self._parse(blob)

    def _parse(self, blob):
        if len(blob) < 32:
            raise ValueError("trop court pour un en-tete")
        (self.id, self.version, self.flag, self.timestamp,
         self.datasize, self.hash) = struct.unpack("<IIIIII", blob[:24])
        self.props = blob[24:32]
        header_len = 32
        self.header_version = None
        self.seed = None
        if self.flag & 0x200:  # EXTENDED_HEADER
            self.header_version = struct.unpack("<Q", blob[32:40])[0]
            self.seed = struct.unpack("<QQQQ", blob[40:72])
            header_len = 72

        if self.id not in (REPLAY_YRP1, REPLAY_YRPX):
            raise ValueError(f"id inconnu 0x{self.id:08x}")

        body = blob[header_len:]
        if self.flag & 0x1:  # COMPRESSED
            body = self._lzma(body, self.props, self.datasize)

        r = Reader(body)
        self._parse_names(r)
        self._parse_params(r)
        if self.id == REPLAY_YRP1:
            self._parse_decks(r)
            self._parse_responses(r)
        else:
            self._parse_stream(r)

    @staticmethod
    def _lzma(body, props, outsize):
        d0 = props[0]
        lc = d0 % 9
        rest = d0 // 9
        lp = rest % 5
        pb = rest // 5
        dict_size = struct.unpack("<I", props[1:5])[0]
        filt = [{"id": lzma.FILTER_LZMA1, "dict_size": dict_size,
                 "lc": lc, "lp": lp, "pb": pb}]
        dec = lzma.LZMADecompressor(format=lzma.FORMAT_RAW, filters=filt)
        return dec.decompress(body, max_length=outsize)

    def _parse_names(self, r):
        if self.flag & 0x8:  # SINGLE_MODE
            self.players = [r.name(), r.name()]
            self.home_count = self.opposing_count = 1
            return
        counts = []
        for _ in range(2):
            if self.flag & 0x20:      # NEWREPLAY
                n = r.u32()
            elif self.flag & 0x2:     # TAG
                n = 2
            else:
                n = 1
            counts.append(n)
            for _ in range(n):
                self.players.append(r.name())
        self.home_count, self.opposing_count = counts

    def _parse_params(self, r):
        self.start_lp = self.start_hand = self.draw_count = None
        if self.id == REPLAY_YRP1:
            self.start_lp = r.u32()
            self.start_hand = r.u32()
            self.draw_count = r.u32()
        self.duel_flags = r.u64() if (self.flag & 0x100) else r.u32()
        if (self.flag & 0x8) and self.id == REPLAY_YRP1:
            slen = r.u16()
            self.scriptname = r.raw(slen).decode("utf-8", "replace")

    def _parse_decks(self, r):
        if self.id != REPLAY_YRP1:
            return
        if (self.flag & 0x8) and not (self.flag & 0x40):  # SINGLE && !HAND_TEST
            return
        for _ in range(self.home_count + self.opposing_count):
            main = [r.u32() for _ in range(r.u32())]
            extra = [r.u32() for _ in range(r.u32())]
            self.decks.append((main, extra))
        if (self.flag & 0x20) and not (self.flag & 0x40):
            self.rule_cards = [r.u32() for _ in range(r.u32())]

    def _parse_responses(self, r):
        while not r.eof():
            n = r.u8()
            if n == 0:
                break
            self.responses.append(r.raw(n))

    def _parse_stream(self, r):
        while not r.eof():
            try:
                msg = r.u8()
                ln = r.u32()
                buf = r.raw(ln)
            except EOFError:
                break
            if msg == OLD_REPLAY_MODE:
                if self.yrp is None:
                    self.yrp = Replay(buf, label="yrp1 embarque")
                continue
            if msg == MSG_NEW_TURN:
                self.turn_count += 1
            if msg == MSG_AI_NAME:
                continue
            self.packets.append((msg, buf))


PHASES = {
    0x01: "DRAW", 0x02: "STANDBY", 0x04: "MAIN1", 0x08: "BATTLE_START",
    0x10: "BATTLE_STEP", 0x20: "DAMAGE", 0x40: "DAMAGE_CAL", 0x80: "BATTLE",
    0x100: "MAIN2", 0x200: "END",
}


def segment_turns(packets):
    """Decoupe le flux aux MSG_NEW_TURN et compte les actions de chaque tour."""
    segs = []
    cur = None
    for msg, buf in packets:
        if msg == MSG_NEW_TURN:
            player = buf[0] if buf else -1
            cur = {"turn": len(segs) + 1, "player": player, "n": 0,
                   "counts": Counter(), "phases": []}
            segs.append(cur)
            continue
        if cur is None:
            cur = {"turn": 0, "player": -1, "n": 0,
                   "counts": Counter(), "phases": ["(pre-tour)"]}
            segs.append(cur)
        cur["n"] += 1
        cur["counts"][msg] += 1
        if msg == 41 and len(buf) >= 2:  # MSG_NEW_PHASE
            ph = struct.unpack("<H", buf[:2])[0]
            cur["phases"].append(PHASES.get(ph, f"0x{ph:x}"))
    return segs


def fmt_deck(main, extra):
    c = Counter(main)
    return (f"main {len(main)} cartes / {len(c)} codes distincts, "
            f"extra {len(extra)} cartes / {len(Counter(extra))} codes distincts")


def report(path):
    with open(path, "rb") as f:
        blob = f.read()
    rp = Replay(blob, label=path)

    print(f"=== {path} ({len(blob)} octets) ===\n")
    kind = "yrpX (streame)" if rp.id == REPLAY_YRPX else "yrp1 (ancien)"
    print(f"  type            : {kind}")
    print(f"  version client  : {rp.version >> 12}.{(rp.version >> 4) & 0xff}."
          f"{rp.version & 0xf}  (0x{rp.version:x})")
    print(f"  flags           : 0x{rp.flag:03x}  {decode_flags(rp.flag)}")
    print(f"  taille decomp.  : {rp.datasize} octets")
    if rp.header_version is not None:
        print(f"  header version  : {rp.header_version}")
        print(f"  seed Xoshiro256 : " +
              ", ".join(f"0x{s:016x}" for s in rp.seed))
    print(f"  duel_flags      : 0x{rp.duel_flags:x}")
    print(f"  joueurs         : {rp.players}")
    print(f"  tours           : {rp.turn_count}")
    print(f"  paquets         : {len(rp.packets)}")

    print("\n--- distribution des messages du flux ---")
    counts = Counter(m for m, _ in rp.packets)
    for msg, n in counts.most_common():
        name = MSG_NAMES.get(msg, f"?{msg}")
        mark = "  <-- prompt" if msg in PROMPTS else ""
        print(f"  {n:5d}  MSG_{name}{mark}")

    prompt_total = sum(n for m, n in counts.items() if m in PROMPTS)
    if prompt_total == 0 and rp.id == REPLAY_YRPX:
        print("\n  (aucun prompt dans le flux : normal pour un yrpX, les "
              "MSG_SELECT_* ne sont pas")
        print("   diffuses aux spectateurs. Les decisions sont dans le yrp1 "
              "embarque.)")

    print("\n--- decoupage par tour ---")
    for seg in segment_turns(rp.packets):
        c = seg["counts"]
        acts = (c.get(60, 0) + c.get(62, 0) + c.get(64, 0) + c.get(70, 0))
        print(f"  tour {seg['turn']} (joueur {seg['player']}) : "
              f"{seg['n']} paquets, "
              f"NS={c.get(60, 0)} SS={c.get(62, 0)} "
              f"activations={c.get(70, 0)} pioches={c.get(90, 0)} "
              f"=> {acts} actions")
        phases = seg["phases"]
        if phases:
            print(f"      phases : {' -> '.join(phases)}")

    print("\n--- proxys de cout de la ligne de reference ---")
    acts = (counts.get(60, 0) + counts.get(62, 0) + counts.get(64, 0)
            + counts.get(70, 0))
    print(f"  invocations normales (SUMMONING)     : {counts.get(60, 0)}")
    print(f"  invocations speciales (SPSUMMONING)  : {counts.get(62, 0)}")
    print(f"  invocations flip (FLIPSUMMONING)     : {counts.get(64, 0)}")
    print(f"  activations (CHAINING)               : {counts.get(70, 0)}")
    print(f"  => A_ref (tier 2)                    : {acts}")

    if rp.yrp is None:
        print("\n!! AUCUN yrp1 EMBARQUE — les reponses du joueur sont "
              "irrecuperables.")
        print("   Le solveur ne peut pas fonctionner sur ce fichier.")
        return rp

    y = rp.yrp
    print("\n=== yrp1 embarque (reponses du joueur) ===\n")
    print(f"  flags           : 0x{y.flag:03x}  {decode_flags(y.flag)}")
    print(f"  start_lp        : {y.start_lp}")
    print(f"  start_hand      : {y.start_hand}")
    print(f"  draw_count      : {y.draw_count}")
    print(f"  duel_flags      : 0x{y.duel_flags:x}")
    if y.seed:
        print(f"  seed Xoshiro256 : " +
              ", ".join(f"0x{s:016x}" for s in y.seed))
    if y.scriptname:
        print(f"  scriptname      : {y.scriptname!r}")
    print(f"  decks           : {len(y.decks)}")
    for i, (main, extra) in enumerate(y.decks):
        print(f"     [{i}] {fmt_deck(main, extra)}")
    print(f"  rule cards      : {len(y.rule_cards)}")
    print(f"  => D_ref (tier 3, reponses enregistrees) : {len(y.responses)}")

    lens = Counter(len(r) for r in y.responses)
    print("\n--- tailles des reponses ---")
    for ln, n in sorted(lens.items()):
        print(f"  {n:5d} reponses de {ln} octet(s)")

    return rp


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        print("usage: inspect_replay.py <fichier.yrpX> [...]")
        sys.exit(1)
    for arg in sys.argv[1:]:
        report(arg)
        print()
