#!/usr/bin/env python3
"""Ajoute des features a la section `target_features` d'objets wasm relogeables.

POURQUOI CE SCRIPT EXISTE
-------------------------
La barriere d'ecriture ne voit pas les copies en bloc : clang abaisse les copies
de STRUCTURE en instruction `memory.copy`, que ni -fsanitize-coverage ni
-Wl,--wrap n'interceptent. Le verificateur (R2V_ARENA_VERIFY=1) le mesure : deux
pages echappent a chaque run, et ce sont des tableaux de pointeurs deplaces d'un
cran — donc de la vraie corruption d'etat, pas du bruit.

`-mno-bulk-memory-opt` ferme ce trou (les copies redeviennent des APPELS a
memcpy, donc interceptables). Mais la negation est hierarchique dans LLVM : elle
emporte aussi `bulk-memory`, et wasm-ld exige atomics+bulk-memory de chaque
objet pour accorder `--shared-memory`. Sans threads, pas de solveur.

`--no-check-features` a ete essaye : il ne corrige rien, il masque le garde-fou,
et le programme tombe ailleurs.

La sortie est donc : compiler SANS bulk-memory (aucune instruction bulk n'est
emise dans notre code) puis REMETTRE la mention dans les metadonnees. L'objet
declare une capacite qu'il n'utilise pas — ce qui est exactement vrai, et c'est
ce que la verification de wasm-ld cherche a etablir.

FORMAT
------
Section custom (id 0) nommee "target_features" :
    uleb  nombre de features
    pour chacune : 1 octet de prefixe ('+' 0x2b, '-' 0x2d, '=' 0x3d)
                   uleb longueur du nom
                   nom
"""
import sys


def read_uleb(b, i):
    v, s = 0, 0
    while True:
        x = b[i]
        i += 1
        v |= (x & 0x7F) << s
        if not (x & 0x80):
            return v, i
        s += 7


def write_uleb(v):
    out = bytearray()
    while True:
        x = v & 0x7F
        v >>= 7
        if v:
            out.append(x | 0x80)
        else:
            out.append(x)
            return bytes(out)


def patch(path, wanted, drop):
    data = bytearray(open(path, 'rb').read())
    if data[:4] != b'\0asm':
        return 'pas un module wasm'
    i = 8
    while i < len(data):
        sec_id = data[i]
        size, j = read_uleb(data, i + 1)
        body_start, body_end = j, j + size
        if sec_id == 0:
            nlen, k = read_uleb(data, body_start)
            name = bytes(data[k:k + nlen])
            if name == b'target_features':
                p = k + nlen
                count, q = read_uleb(data, p)
                kept, have, removed = bytearray(), set(), []
                for _ in range(count):
                    start = q
                    q += 1                       # prefixe
                    ln, q = read_uleb(data, q)
                    name = bytes(data[q:q + ln]).decode()
                    q += ln
                    # `-mno-bulk-memory-opt` fait emettre `-shared-mem`, une
                    # INTERDICTION explicite : c'est elle que wasm-ld refuse, et
                    # son message parle d'atomics/bulk-memory, ce qui envoie
                    # chercher au mauvais endroit. On la retire.
                    if name in drop:
                        removed.append(name)
                        continue
                    have.add(name)
                    kept += data[start:q]
                add = [f for f in wanted if f not in have]
                if not add and not removed:
                    return 'rien a faire'
                for f in add:
                    kept += b'+' + write_uleb(len(f)) + f.encode()
                new_body = (data[body_start:p]
                            + write_uleb(count - len(removed) + len(add))
                            + kept)
                new_sec = bytes([0]) + write_uleb(len(new_body)) + new_body
                data[i:body_end] = new_sec
                open(path, 'wb').write(data)
                return ('ajoute ' + ','.join(add) if add else '') +                        (' retire ' + ','.join(removed) if removed else '')
        i = body_end
    return 'pas de section target_features'


if __name__ == '__main__':
    feats = [x for x in sys.argv[1].split(',') if x]
    drop = set(x for x in sys.argv[2].split(',') if x)
    for f in sys.argv[3:]:
        print('%-40s %s' % (f.split('\\')[-1], patch(f, feats, drop)))
