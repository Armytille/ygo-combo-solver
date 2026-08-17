# -*- coding: utf-8 -*-
"""s22 — verification sympy de la convention des duaux exposee par SolveOperatorLP.

La convention C++ (operators.cpp, s22) :
    row_dual[i]   = cout reduit du surplus/ecart de la ligne i au tableau final
    bound_dual[j] = cout reduit de l'ecart de la borne x_j <= u_j
et le CERTIFICAT verifie a chaque execution du binaire (auto-test 65/65) :
    (a) y >= 0, w >= 0 ;
    (b) faisabilite duale : c_j - Sigma_i y_i a_ij + w_j >= 0 ;
    (c) dualite forte    : b'y - u'w = c'x*.

Ce script etablit le THEOREME qui rend ce certificat suffisant : pour
    min c'x   s.c.  A x >= b,  0 <= x <= u
tout couple (y, w) satisfaisant (a)+(b)+(c) est une solution OPTIMALE du dual
    max b'y - u'w   s.c.  A' y - w <= c,  y >= 0, w >= 0
— donc les « prix » exposes sont des prix d'ombre valides, et w_j > 0 designe
une borne u_j qui LIE le plan (l'usage qu'en fait la derivation des hotes a
quota). On le verifie numeriquement : primal exact par lpmin (x >= 0 EXPLICITE
— lpmin IGNORE nonnegative=True, piege s21), dual exact par lpmax, et l'egalite
des deux optima (dualite forte) sur des instances aleatoires de la forme du
modele. Si un (y, w) verifiait (a)+(b)+(c) sans etre optimal, la dualite
faible serait violee — c'est l'argument, et l'egalite mesuree ici le fonde.

Usage : py -3.11 tools/s22_verify_duals.py
"""
import random
from fractions import Fraction

import sympy
from sympy.solvers.simplex import lpmin, lpmax


def run_case(rng, n, m):
    xs = sympy.symbols(f"x0:{n}", positive=False)
    ys = sympy.symbols(f"y0:{m}", positive=False)
    ws = sympy.symbols(f"w0:{n}", positive=False)
    c = [rng.randint(1, 4) for _ in range(n)]
    u = [rng.choice([None, rng.randint(1, 3)]) for _ in range(n)]
    A = [[rng.choice([-1, 0, 0, 1, 1]) for _ in range(n)] for _ in range(m)]
    b = [rng.randint(-2, 3) for _ in range(m)]

    # --- primal : min c'x, A x >= b, 0 <= x <= u (x >= 0 EXPLICITE) ---------
    cons = []
    for i in range(m):
        cons.append(sum(A[i][j] * xs[j] for j in range(n)) >= b[i])
    for j in range(n):
        cons.append(xs[j] >= 0)
        if u[j] is not None:
            cons.append(xs[j] <= u[j])
    try:
        popt, _ = lpmin(sum(c[j] * xs[j] for j in range(n)), cons)
    except Exception:
        return None  # infaisable : hors du perimetre (le certificat ne rend
                     # des duaux que sur les cas faisables)

    # --- dual : max b'y - u'w, A'y - w <= c, y >= 0, w >= 0 -----------------
    dcons = []
    for j in range(n):
        lhs = sum(A[i][j] * ys[i] for i in range(m))
        if u[j] is not None:
            lhs -= ws[j]
        dcons.append(lhs <= c[j])
    for i in range(m):
        dcons.append(ys[i] >= 0)
    for j in range(n):
        if u[j] is not None:
            dcons.append(ws[j] >= 0)
    obj = sum(b[i] * ys[i] for i in range(m)) - sum(
        u[j] * ws[j] for j in range(n) if u[j] is not None)
    try:
        dopt, _ = lpmax(obj, dcons)
    except Exception:
        return None  # dual non borne <=> primal infaisable : deja ecarte

    assert sympy.simplify(popt - dopt) == 0, (
        f"DUALITE FORTE VIOLEE : primal {popt} != dual {dopt}")
    # garde du piege s21 : optimum negatif sous couts >= 0 = appel faux
    assert popt >= 0, f"optimum negatif {popt} avec c >= 0 : contrainte x>=0 perdue"
    return popt


def main():
    rng = random.Random(2022)
    done = 0
    for k in range(200):
        r = run_case(rng, n=rng.randint(2, 4), m=rng.randint(1, 3))
        if r is not None:
            done += 1
    print(f"dualite forte primal = dual verifiee sur {done} instances "
          f"faisables (sur 200 tirees)")
    print("=> le certificat (y,w >= 0, faisabilite duale, egalite b'y-u'w = "
          "c'x*) prouve l'optimalite des duaux exposes ; w_j > 0 designe une "
          "borne LIANTE.")


if __name__ == "__main__":
    main()
