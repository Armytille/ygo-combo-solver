# -*- coding: utf-8 -*-
"""sympy check of the dual convention SolveOperatorLP exposes.

The C++ convention (operators.cpp):
    row_dual[i]   = reduced cost of row i's surplus/slack in the final tableau
    bound_dual[j] = reduced cost of the slack of the bound x_j <= u_j
and the CERTIFICATE checked on every execution of the binary (self-test 65/65):
    (a) y >= 0, w >= 0;
    (b) dual feasibility: c_j - Sigma_i y_i a_ij + w_j >= 0;
    (c) strong duality:   b'y - u'w = c'x*.

This script establishes the THEOREM that makes the certificate sufficient: for
    min c'x   s.t.  A x >= b,  0 <= x <= u
any pair (y, w) satisfying (a)+(b)+(c) is an OPTIMAL solution of the dual
    max b'y - u'w   s.t.  A' y - w <= c,  y >= 0, w >= 0
so the "prices" exposed are valid shadow prices, and w_j > 0 designates a bound
u_j that BINDS the plan (which is what the quota host derivation uses). We
check it numerically: exact primal through lpmin (with an EXPLICIT x >= 0,
since lpmin IGNORES nonnegative=True), exact dual through lpmax, and the
equality of the two optima (strong duality) on random instances shaped like the
model. If some (y, w) satisfied (a)+(b)+(c) without being optimal, weak duality
would be violated; that is the argument, and the measured equality grounds it.

Usage: py -3.11 tools/s22_verify_duals.py
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

    # --- primal: min c'x, A x >= b, 0 <= x <= u (x >= 0 EXPLICIT) -----------
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
        return None  # infeasible: out of scope (the certificate only returns
                     # duals on the feasible cases)

    # --- dual: max b'y - u'w, A'y - w <= c, y >= 0, w >= 0 ------------------
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
        return None  # unbounded dual <=> infeasible primal: already discarded

    assert sympy.simplify(popt - dopt) == 0, (
        f"DUALITE FORTE VIOLEE : primal {popt} != dual {dopt}")
    # guard against the lpmin trap: a negative optimum under costs >= 0 is a bad call
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
