#!/usr/bin/env python3
"""Liaison ctypes vers ocgcore.dll (x64) + lecteurs de cartes et de scripts.

Sert de socle au spike du combo solver : permet de piloter un duel EDOPro
complet depuis Python, sans rendu et sans build C++.

La DLL x64 se construit depuis le clone avec :
    MSBuild build\\ygo.sln /p:Configuration=Release /p:Platform=x64 /t:ocgcoreshared
(la ocgcore.dll installee avec EDOPro est en x86, inutilisable ici.)
"""

import ctypes
import glob
import os
import sqlite3
import struct
from ctypes import (CFUNCTYPE, POINTER, Structure, byref, c_char_p, c_int,
                    c_int32, c_uint8, c_uint16, c_uint32, c_uint64, c_void_p)

# --- constantes du core (ocgcore/common.h) --------------------------------

LOCATION_DECK, LOCATION_HAND, LOCATION_MZONE, LOCATION_SZONE = 0x01, 0x02, 0x04, 0x08
LOCATION_GRAVE, LOCATION_REMOVED, LOCATION_EXTRA = 0x10, 0x20, 0x40

POS_FACEUP_ATTACK, POS_FACEDOWN_ATTACK = 0x1, 0x2
POS_FACEUP_DEFENSE, POS_FACEDOWN_DEFENSE = 0x4, 0x8

TYPE_LINK = 0x4000000

QUERY_CODE, QUERY_POSITION, QUERY_ALIAS, QUERY_TYPE = 0x1, 0x2, 0x4, 0x8
QUERY_LEVEL, QUERY_RANK, QUERY_ATTRIBUTE, QUERY_RACE = 0x10, 0x20, 0x40, 0x80
QUERY_ATTACK, QUERY_DEFENSE = 0x100, 0x200
QUERY_OVERLAY_CARD, QUERY_COUNTERS = 0x10000, 0x20000
QUERY_STATUS, QUERY_LSCALE, QUERY_RSCALE, QUERY_LINK = 0x80000, 0x200000, 0x400000, 0x800000
QUERY_END = 0x80000000

DUEL_STATUS_END, DUEL_STATUS_AWAITING, DUEL_STATUS_CONTINUE = 0, 1, 2

PHASES = {0x01: "DRAW", 0x02: "STANDBY", 0x04: "MAIN1", 0x08: "BATTLE_START",
          0x10: "BATTLE_STEP", 0x20: "DAMAGE", 0x40: "DAMAGE_CAL",
          0x80: "BATTLE", 0x100: "MAIN2", 0x200: "END"}

LOC_NAMES = {LOCATION_DECK: "DECK", LOCATION_HAND: "HAND",
             LOCATION_MZONE: "MZONE", LOCATION_SZONE: "SZONE",
             LOCATION_GRAVE: "GRAVE", LOCATION_REMOVED: "REMOVED",
             LOCATION_EXTRA: "EXTRA"}

POS_NAMES = {1: "ATK", 2: "FD-ATK", 4: "DEF", 8: "FD-DEF",
             5: "FACEUP", 10: "FACEDOWN", 3: "ATK?", 12: "DEF?"}


# --- structures de l'API --------------------------------------------------

class OCG_CardData(Structure):
    _fields_ = [("code", c_uint32), ("alias", c_uint32),
                ("setcodes", POINTER(c_uint16)), ("type", c_uint32),
                ("level", c_uint32), ("attribute", c_uint32),
                ("race", c_uint64), ("attack", c_int32), ("defense", c_int32),
                ("lscale", c_uint32), ("rscale", c_uint32),
                ("link_marker", c_uint32)]


class OCG_Player(Structure):
    _fields_ = [("startingLP", c_uint32), ("startingDrawCount", c_uint32),
                ("drawCountPerTurn", c_uint32)]


DataReader = CFUNCTYPE(None, c_void_p, c_uint32, POINTER(OCG_CardData))
DataReaderDone = CFUNCTYPE(None, c_void_p, POINTER(OCG_CardData))
ScriptReader = CFUNCTYPE(c_int, c_void_p, c_void_p, c_char_p)
LogHandler = CFUNCTYPE(None, c_void_p, c_char_p, c_int)


class OCG_DuelOptions(Structure):
    _fields_ = [("seed", c_uint64 * 4), ("flags", c_uint64),
                ("team1", OCG_Player), ("team2", OCG_Player),
                ("cardReader", DataReader), ("payload1", c_void_p),
                ("scriptReader", ScriptReader), ("payload2", c_void_p),
                ("logHandler", LogHandler), ("payload3", c_void_p),
                ("cardReaderDone", DataReaderDone), ("payload4", c_void_p),
                ("enableUnsafeLibraries", c_uint8)]


class OCG_NewCardInfo(Structure):
    _fields_ = [("team", c_uint8), ("duelist", c_uint8), ("code", c_uint32),
                ("con", c_uint8), ("loc", c_uint32), ("seq", c_uint32),
                ("pos", c_uint32)]


class OCG_QueryInfo(Structure):
    _fields_ = [("flags", c_uint32), ("con", c_uint8), ("loc", c_uint32),
                ("seq", c_uint32), ("overlay_seq", c_uint32)]


# --- base de cartes -------------------------------------------------------

class CardDB:
    """Agrege toutes les .cdb comme le fait DataManager (data_manager.cpp:100)."""

    def __init__(self, workdir):
        self.cards = {}
        paths = sorted(glob.glob(os.path.join(workdir, "expansions", "*.cdb")))
        paths += sorted(glob.glob(os.path.join(workdir, "repositories",
                                               "**", "*.cdb"), recursive=True))
        root = os.path.join(workdir, "cards.cdb")
        if os.path.exists(root) and os.path.getsize(root) > 0:
            paths.insert(0, root)
        self.sources = []
        for p in paths:
            try:
                n = self._load(p)
                self.sources.append((p, n))
            except sqlite3.Error:
                continue

    def _load(self, path):
        con = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
        try:
            rows = con.execute(
                "select id,ot,alias,setcode,type,atk,def,level,race,"
                "attribute from datas").fetchall()
        except sqlite3.Error:
            con.close()
            return 0
        con.close()
        for (cid, _ot, alias, setcode, ctype, atk, dfn, level, race,
             attribute) in rows:
            setcodes = [(setcode >> (i * 16)) & 0xffff for i in range(4)]
            setcodes = [s for s in setcodes if s]
            link_marker = 0
            if ctype & TYPE_LINK:
                link_marker = dfn & 0xffffffff
                dfn = 0
            lvl = -(level & 0xff) if level < 0 else (level & 0xff)
            self.cards[cid] = {
                "code": cid, "alias": alias, "setcodes": setcodes,
                "type": ctype & 0xffffffff, "level": lvl & 0xffffffff,
                "attribute": attribute & 0xffffffff, "race": race & 0xffffffffffffffff,
                "attack": atk, "defense": dfn,
                "lscale": (level >> 24) & 0xff, "rscale": (level >> 16) & 0xff,
                "link_marker": link_marker,
            }
        return len(rows)

    def __contains__(self, code):
        return code in self.cards

    def __len__(self):
        return len(self.cards)


# --- lecteur de scripts ---------------------------------------------------

class ScriptProvider:
    """Reproduit l'ordre de recherche de Game::FindScript (game.cpp:4120)."""

    def __init__(self, workdir, override_roots=None):
        """override_roots : jeux de scripts a placer en tete.

        Un replay n'est fidelement rejouable qu'avec les scripts de son epoque.
        Passer ici un export daté du depot (cf. --scriptdir) remplace les
        depots vivants de l'installation, qui ont pu diverger depuis.
        """
        def expand(root):
            """Un dossier de scripts et ses sous-dossiers directs."""
            if not os.path.isdir(root):
                return []
            out = [root]
            for entry in sorted(os.listdir(root)):
                sub = os.path.join(root, entry)
                if os.path.isdir(sub) and not entry.startswith("."):
                    out.append(sub)
            return out

        # Ordre de Game::FindScript : les depots passent devant (game.cpp:2959),
        # puis expansions/script, puis ./script. Le depot fait autorite : c'est
        # lui qui porte les scripts a jour, ./script peut etre une copie datee.
        self.dirs = []
        if override_roots:
            for root in override_roots:
                self.dirs += expand(root)
        else:
            for repo in sorted(glob.glob(os.path.join(workdir, "repositories", "*"))):
                self.dirs += expand(os.path.join(repo, "script"))
        self.dirs += expand(os.path.join(workdir, "expansions", "script"))
        self.dirs += expand(os.path.join(workdir, "script"))
        self.workdir = workdir
        self.misses = set()
        self.hits = 0

    def read(self, name):
        name = name.replace("\\", "/").lstrip("./")
        for d in self.dirs:
            p = os.path.join(d, name)
            if os.path.isfile(p):
                with open(p, "rb") as f:
                    data = f.read()
                if data[:3] == b"\xef\xbb\xbf":
                    data = data[3:]
                self.hits += 1
                return data
        direct = os.path.join(self.workdir, name)
        if os.path.isfile(direct):
            with open(direct, "rb") as f:
                return f.read()
        self.misses.add(name)
        return None


# --- pilote de duel -------------------------------------------------------

class Core:
    def __init__(self, dll_path):
        self.lib = ctypes.CDLL(dll_path)
        L = self.lib
        L.OCG_GetVersion.restype = None
        L.OCG_GetVersion.argtypes = [POINTER(c_int), POINTER(c_int)]
        L.OCG_CreateDuel.restype = c_int
        L.OCG_CreateDuel.argtypes = [POINTER(c_void_p), POINTER(OCG_DuelOptions)]
        L.OCG_DestroyDuel.argtypes = [c_void_p]
        L.OCG_DuelNewCard.argtypes = [c_void_p, POINTER(OCG_NewCardInfo)]
        L.OCG_StartDuel.argtypes = [c_void_p]
        L.OCG_DuelProcess.restype = c_int
        L.OCG_DuelProcess.argtypes = [c_void_p]
        L.OCG_DuelGetMessage.restype = c_void_p
        L.OCG_DuelGetMessage.argtypes = [c_void_p, POINTER(c_uint32)]
        L.OCG_DuelSetResponse.argtypes = [c_void_p, c_void_p, c_uint32]
        L.OCG_LoadScript.restype = c_int
        L.OCG_LoadScript.argtypes = [c_void_p, c_char_p, c_uint32, c_char_p]
        L.OCG_DuelQueryCount.restype = c_uint32
        L.OCG_DuelQueryCount.argtypes = [c_void_p, c_uint8, c_uint32]
        L.OCG_DuelQueryLocation.restype = c_void_p
        L.OCG_DuelQueryLocation.argtypes = [c_void_p, POINTER(c_uint32),
                                            POINTER(OCG_QueryInfo)]

    def version(self):
        maj, mnr = c_int(), c_int()
        self.lib.OCG_GetVersion(byref(maj), byref(mnr))
        return maj.value, mnr.value


class Duel:
    """Un duel vivant. Garde les callbacks en vie cote Python."""

    def __init__(self, core, db, scripts, seed, flags, lp, hand, draw,
                 log=None):
        self.core = core
        self.db = db
        self.scripts = scripts
        self.log_lines = []
        self._log_cb = log
        self._setcode_keepalive = {}
        self.handle = c_void_p()

        self._card_reader = DataReader(self._on_card)
        self._script_reader = ScriptReader(self._on_script)
        self._log_handler = LogHandler(self._on_log)
        self._card_done = DataReaderDone(lambda p, d: None)

        opts = OCG_DuelOptions()
        for i in range(4):
            opts.seed[i] = seed[i]
        opts.flags = flags
        opts.team1 = OCG_Player(lp, hand, draw)
        opts.team2 = OCG_Player(lp, hand, draw)
        opts.cardReader = self._card_reader
        opts.scriptReader = self._script_reader
        opts.logHandler = self._log_handler
        opts.cardReaderDone = self._card_done
        opts.enableUnsafeLibraries = 1
        self._opts = opts

        rc = core.lib.OCG_CreateDuel(byref(self.handle), byref(opts))
        if rc != 0:
            raise RuntimeError(f"OCG_CreateDuel a echoue (code {rc})")
        for boot in ("constant.lua", "utility.lua"):
            if not self.load_script(boot):
                raise RuntimeError(f"impossible de charger {boot}")

    # -- callbacks ---------------------------------------------------------

    def _on_card(self, payload, code, data_ptr):
        d = data_ptr.contents
        row = self.db.cards.get(code)
        if row is None:
            ctypes.memset(byref(d), 0, ctypes.sizeof(OCG_CardData))
            d.code = code
            return
        d.code = row["code"]
        d.alias = row["alias"]
        if row["setcodes"]:
            arr = self._setcode_keepalive.get(code)
            if arr is None:
                vals = row["setcodes"] + [0]
                arr = (c_uint16 * len(vals))(*vals)
                self._setcode_keepalive[code] = arr
            d.setcodes = ctypes.cast(arr, POINTER(c_uint16))
        else:
            d.setcodes = None
        d.type = row["type"]
        d.level = row["level"]
        d.attribute = row["attribute"]
        d.race = row["race"]
        d.attack = row["attack"]
        d.defense = row["defense"]
        d.lscale = row["lscale"]
        d.rscale = row["rscale"]
        d.link_marker = row["link_marker"]

    def _on_script(self, payload, duel, name):
        return 1 if self.load_script(name.decode("utf-8")) else 0

    def _on_log(self, payload, msg, typ):
        line = msg.decode("utf-8", "replace")
        self.log_lines.append((typ, line))
        if self._log_cb:
            self._log_cb(typ, line)

    # -- API ---------------------------------------------------------------

    def load_script(self, name):
        data = self.scripts.read(name)
        if not data:
            return False
        return self.core.lib.OCG_LoadScript(
            self.handle, data, len(data), name.encode("utf-8")) == 1

    def exec_lua(self, code, name=" "):
        b = code.encode("utf-8")
        return self.core.lib.OCG_LoadScript(self.handle, b, len(b),
                                            name.encode("utf-8")) == 1

    def new_card(self, code, team, con, loc, seq=0, pos=POS_FACEDOWN_DEFENSE,
                 duelist=0):
        info = OCG_NewCardInfo(team, duelist, code, con, loc, seq, pos)
        self.core.lib.OCG_DuelNewCard(self.handle, byref(info))

    def start(self):
        self.core.lib.OCG_StartDuel(self.handle)

    def process(self):
        return self.core.lib.OCG_DuelProcess(self.handle)

    def messages(self):
        """Renvoie la liste des (type, payload) produits depuis le dernier appel."""
        ln = c_uint32()
        ptr = self.core.lib.OCG_DuelGetMessage(self.handle, byref(ln))
        if not ptr or ln.value == 0:
            return []
        raw = ctypes.string_at(ptr, ln.value)
        out, off = [], 0
        while off + 4 <= len(raw):
            size = struct.unpack_from("<I", raw, off)[0]
            off += 4
            if size == 0 or off + size > len(raw):
                break
            out.append((raw[off], raw[off + 1:off + size]))
            off += size
        return out

    def set_response(self, data):
        buf = (c_uint8 * len(data)).from_buffer_copy(data)
        self.core.lib.OCG_DuelSetResponse(self.handle, buf, len(data))

    def query_location(self, con, loc, flags):
        info = OCG_QueryInfo(flags, con, loc, 0, 0)
        ln = c_uint32()
        ptr = self.core.lib.OCG_DuelQueryLocation(self.handle, byref(ln),
                                                  byref(info))
        if not ptr or ln.value <= 4:
            return []
        # OCG_DuelQueryLocation prefixe le buffer par la taille utile
        # (ocgapi.cpp:234) : on la saute.
        return parse_query_stream(ctypes.string_at(ptr, ln.value)[4:])

    def count(self, team, loc):
        return self.core.lib.OCG_DuelQueryCount(self.handle, team, loc)

    def destroy(self):
        if self.handle:
            self.core.lib.OCG_DestroyDuel(self.handle)
            self.handle = c_void_p()


# --- decodage des queries (card::get_infos, card.cpp:118) -----------------

def parse_query_stream(raw):
    """Decoupe un buffer de query en liste de cartes (None = zone vide)."""
    out, off, cur = [], 0, None
    while off + 2 <= len(raw):
        size = struct.unpack_from("<H", raw, off)[0]
        off += 2
        if size == 0:                      # emplacement vide
            out.append(None)
            continue
        if off + size > len(raw):
            break
        flag = struct.unpack_from("<I", raw, off)[0]
        body = raw[off + 4:off + size]
        off += size
        if flag == QUERY_END:
            out.append(cur or {})
            cur = None
            continue
        if cur is None:
            cur = {}
        _decode_field(cur, flag, body)
    if cur:
        out.append(cur)
    return out


def _decode_field(dst, flag, body):
    if flag == QUERY_CODE:
        dst["code"] = struct.unpack_from("<I", body)[0]
    elif flag == QUERY_POSITION:
        dst["position"] = struct.unpack_from("<I", body)[0]
    elif flag == QUERY_ALIAS:
        dst["alias"] = struct.unpack_from("<I", body)[0]
    elif flag == QUERY_TYPE:
        dst["type"] = struct.unpack_from("<I", body)[0]
    elif flag == QUERY_LEVEL:
        dst["level"] = struct.unpack_from("<I", body)[0]
    elif flag == QUERY_RANK:
        dst["rank"] = struct.unpack_from("<I", body)[0]
    elif flag == QUERY_ATTACK:
        dst["attack"] = struct.unpack_from("<I", body)[0]
    elif flag == QUERY_DEFENSE:
        dst["defense"] = struct.unpack_from("<I", body)[0]
    elif flag == QUERY_STATUS:
        dst["status"] = struct.unpack_from("<I", body)[0]
    elif flag == QUERY_LSCALE:
        dst["lscale"] = struct.unpack_from("<I", body)[0]
    elif flag == QUERY_RSCALE:
        dst["rscale"] = struct.unpack_from("<I", body)[0]
    elif flag == QUERY_LINK:
        dst["link"] = struct.unpack_from("<I", body)[0]
        dst["link_marker"] = struct.unpack_from("<I", body, 4)[0]
    elif flag == QUERY_OVERLAY_CARD:
        n = struct.unpack_from("<I", body)[0]
        dst["overlay"] = list(struct.unpack_from(f"<{n}I", body, 4)) if n else []
    elif flag == QUERY_COUNTERS:
        n = struct.unpack_from("<I", body)[0]
        dst["counters"] = list(struct.unpack_from(f"<{n}I", body, 4)) if n else []
