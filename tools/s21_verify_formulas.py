# -*- coding: utf-8 -*-
"""Session 21 — verification par sympy de chaque formule du combosolver.

Chaque bloc rend VRAI/FAUX + la derivation. Sortie ASCII pour eviter les
soucis d'encodage console.
"""
import math
import random
import sys
from fractions import Fraction

import sympy as sp
from sympy.solvers.simplex import lpmin, InfeasibleLPError, UnboundedLPError

OK = "  [OK] "
KO = "  [ECHEC] "


def bloc(titre):
    print("\n=== " + titre + " ===")


# ---------------------------------------------------------------------------
bloc("B1. Les 5 cas de SelfTestOperatorLP, resolus en rationnels exacts")
# forme : min c'x  s.c.  A'x >= b, 0 <= x <= u


def solve_exact(cost, upper, rows):
    # PIEGE TROUVE EN SEANCE : lpmin IGNORE l'assomption nonnegative=True des
    # symboles — sans `x >= 0` EXPLICITE il resout le programme a variables
    # LIBRES (optima negatifs rendus avec des couts >= 0, impossible). La
    # premiere table de fuzz etait donc fausse par MON appel, pas par sympy.
    n = len(cost)
    xs = sp.symbols("x0:%d" % n)
    cons = [x >= 0 for x in xs]
    for coefs, rhs in rows:
        cons.append(sp.Add(*[sp.Rational(c) * xs[j] for j, c in coefs]) >= rhs)
    for j, u in enumerate(upper):
        if u is not None:
            cons.append(xs[j] <= sp.Rational(u))
    f = sp.Add(*[sp.Rational(c) * xs[j] for j, c in enumerate(cost)])
    try:
        val, sol = lpmin(f, cons)
        v = sp.Rational(val)
        assert all(c >= 0 for c in cost) is False or v >= 0, \
            "optimum negatif avec c>=0, x>=0 : appel faux"
        return True, v
    except InfeasibleLPError:
        return False, None


CASES = [
    # (cost, upper, rows[(coefs,rhs)], feas attendu, val attendu)
    ([1], [None], [([(0, 1)], 3)], True, 3),
    ([1, 1], [None, None], [([(0, 1)], 2), ([(0, -1), (1, 1)], 0)], True, 4),
    ([1, 5], [1, None], [([(0, 1), (1, 1)], 3)], True, 11),
    ([1], [2], [([(0, 1)], 3)], False, None),
    ([1], [None], [([(0, 1)], 1), ([(0, -1)], -4)], True, 1),
]
for i, (c, u, rows, ef, ev) in enumerate(CASES, 1):
    feas, val = solve_exact(c, u, rows)
    good = feas == ef and (not feas or val == ev)
    print((OK if good else KO) + "cas %d : attendu %s %s, sympy %s %s"
          % (i, ef, ev, feas, val))

# ---------------------------------------------------------------------------
bloc("B2. Identites logarithmiques (Levin, vraisemblance)")

# (a) moyenne geometrique : 10^(sum log10 a_i / n) == (prod a_i)^(1/n)
a1, a2, a3 = sp.symbols("a1 a2 a3", positive=True)
lhs = 10 ** ((sp.log(a1, 10) + sp.log(a2, 10) + sp.log(a3, 10)) / 3)
rhs = (a1 * a2 * a3) ** sp.Rational(1, 3)
d = sp.simplify(sp.log(lhs) - sp.log(rhs))
print((OK if d == 0 else KO) + "moyenne geometrique = 10^(log10p/n) : diff = %s" % d)

# (b) borne monolithique : log10(d) - ln(pi)/ln(10) == log10(d/pi)
dd, pi = sp.symbols("d pi", positive=True)
d2 = sp.simplify((sp.log(dd, 10) - sp.log(pi) / sp.log(10)) - sp.log(dd / pi, 10))
print((OK if d2 == 0 else KO) + "log10(d) - ln(pi)/ln10 = log10(d/pi) : diff = %s" % d2)

# (c) log-sum-exp base 10 : w + log10(sum 10^(l_i - w)) == log10(sum 10^l_i)
l1, l2, l3, w = sp.symbols("l1 l2 l3 w", real=True)
lse = w + sp.log(10 ** (l1 - w) + 10 ** (l2 - w) + 10 ** (l3 - w), 10)
ref = sp.log(10 ** l1 + 10 ** l2 + 10 ** l3, 10)
d3 = sp.simplify(sp.expand_log(lse - ref, force=True))
print((OK if d3 == 0 else KO) + "log-sum-exp base 10 : diff = %s" % d3)

# ---------------------------------------------------------------------------
bloc("B3. Gradient NRPA sous softmax a temperature")
# p_i = exp(w_i/t)/sum ; d ln p_c / d w_j = (delta_cj - p_j)/t
wsym = sp.symbols("w0:3", real=True)
t = sp.symbols("tau", positive=True)
exps = [sp.exp(ws / t) for ws in wsym]
Z = sp.Add(*exps)
lnpc = sp.log(exps[1] / Z)   # choisi = indice 1
grads = [sp.simplify(sp.diff(lnpc, ws)) for ws in wsym]
pj = [sp.simplify(e / Z) for e in exps]
expect = [sp.simplify(((1 if j == 1 else 0) - pj[j]) / t) for j in range(3)]
ok = all(sp.simplify(g - e) == 0 for g, e in zip(grads, expect))
print((OK if ok else KO) + "d ln pi(choisi)/d w_j = (delta - p_j)/tau")
print("        => l'update NRPA du code est alpha*(delta - p) SANS le facteur 1/tau :")
print("           c'est la regle NRPA classique (Rosin 2011), pas le gradient exact ;")
print("           a tau constant c'est un simple rescalage de alpha. COHERENT.")

# ---------------------------------------------------------------------------
bloc("B4. Shrinkage contextuel s = n/(n+k)")
n_, k_ = sp.symbols("n k", positive=True)
s = n_ / (n_ + k_)
lim0 = sp.limit(s, n_, 0)
liminf = sp.limit(s, n_, sp.oo)
print((OK if (lim0 == 0 and liminf == 1) else KO)
      + "s(0)=%s (global seul), s(inf)=%s (contexte seul) — interpolation monotone" % (lim0, liminf))

# ---------------------------------------------------------------------------
bloc("B5. Recurrence lambda/pi (mode levin_reroot) — exacte ?")
# lam(n) = lam(parent) + 1/pi(prefixe du segment) ; verifie contre la forme
# close  lam(n) - 1 = somme_{k=1..d} 1/prod_{j<=k} p_j  sur chaines aleatoires.
random.seed(41)
worst = 0
for trial in range(200):
    depth = random.randint(1, 12)
    probs = [Fraction(random.randint(1, 9), 10) for _ in range(depth)]
    lam = Fraction(1)
    prefix = Fraction(1)
    for p in probs:
        prefix *= p
        lam += 1 / prefix
    # forme close
    close = Fraction(1)
    run = Fraction(1)
    acc = Fraction(0)
    for p in probs:
        run *= p
        acc += 1 / run
    close += acc
    if lam != close:
        worst += 1
print((OK if worst == 0 else KO) + "200 chaines : recurrence == forme close (ecarts : %d)" % worst)

# ---------------------------------------------------------------------------
bloc("B6. Recurrence hu/hv (mode reroot_h) : greedy vs min EXACT sur les ancetres")
# Definition : pour un re-enracinement en l'ancetre a,
#   cout(a, n) = somme_{k=a+1..n} (1/w_a) / pi(a->k)
# Le code garde 2 candidats (prolonger le meilleur courant, ou re-enraciner au
# parent). On verifie : greedy >= exact (jamais de sous-estimation) et on mesure
# l'ecart max sur 300 chaines aleatoires.
random.seed(42)
sous_estime = 0
ecart_max = 0.0
for trial in range(300):
    depth = random.randint(2, 14)
    probs = [Fraction(random.randint(1, 9), 10) for _ in range(depth)]
    invw = [Fraction(random.randint(1, 40), 10) for _ in range(depth + 1)]  # 1/w_a >= ~0
    # greedy du code : hu/hv ; racine hu=hv=+inf => l'enfant 1 se re-enracine.
    INF = None
    hu, hv = INF, INF
    for i, p in enumerate(probs):
        if hu is None:
            new_u = invw[i] / p
            hu, hv = new_u, new_u
        else:
            ext_u = hu / p
            ext_v = hv + ext_u
            new_u = invw[i] / p
            if ext_v <= new_u:
                hu, hv = ext_u, ext_v
            else:
                hu, hv = new_u, new_u
    # exact : min sur TOUS les ancetres a (0..depth-1) du cout re-enracine en a
    best = None
    for a in range(depth):
        run = Fraction(1)
        tot = Fraction(0)
        for k in range(a, depth):
            run *= probs[k]
            tot += invw[a] / run
        if best is None or tot < best:
            best = tot
    if hv < best:
        sous_estime += 1
    ecart_max = max(ecart_max, float(hv / best))
print((OK if sous_estime == 0 else KO)
      + "300 chaines : greedy jamais < exact (sous-estimations : %d)" % sous_estime)
print("        ecart max greedy/exact = x%.2f  (unilateral, borne par le candidat parent)"
      % ecart_max)

# ---------------------------------------------------------------------------
bloc("B7. Arithmetique de la serialisation (9.31) — et sa FORME CLOSE")
q, L, b = sp.symbols("q L b", positive=True)
f = q * b ** (L / q)
df = sp.diff(sp.log(f), q)
qstar = sp.solve(sp.Eq(df, 0), q)
print("  cout(q blocs egaux) = q * b^(L/q) ; d ln f/dq = %s" % sp.simplify(df))
print("  q* = %s  (= L ln b)" % qstar)
Lv, bv = 110.0, 5.9
qs = Lv * math.log(bv)
print("  L=110, b=5.9  =>  q* = %.0f > L : le cout DECROIT sur tout q in [1, L]." % qs)
print("  => LE GRAIN LE PLUS FIN GAGNE TOUJOURS ; le plancher est q=L, cout = b*L = %.0f" % (bv * Lv))
for qq in (1, 14, 17, 33, 110):
    c = qq * bv ** (Lv / qq)
    print("    q=%3d blocs egaux : cout = %.3g" % (qq, c))
print("  NB : a blocs INEGAUX le cout est Sigma b^(l_i), domine par b^(l_max) :")
for lm in (8, 12, 16, 20, 30):
    print("    l_max=%2d : terme dominant %.2g" % (lm, bv ** lm))
print("  => 16-18 cellules qui echouent a ~1e6 etats ne peuvent s'expliquer que par")
print("     l_max >~ 12 : le profil des ECARTS entre barreaux est la mesure qui manque.")

# ---------------------------------------------------------------------------
bloc("B8. Admissibilite de l'arrondi h = ceil(c'x*)")
# c entiers, tout plan a un cout entier >= c'x*, donc >= ceil(c'x*).
rnd = random.Random(7)
viol = 0
for _ in range(500):
    lp_opt = Fraction(rnd.randint(0, 400), rnd.randint(1, 12))
    hstar_candidates = [i for i in range(0, 80) if i >= lp_opt]
    if not hstar_candidates:
        continue
    hstar = rnd.choice(hstar_candidates)   # un cout de plan entier >= optimum LP
    if math.ceil(lp_opt) > hstar:
        viol += 1
print((OK if viol == 0 else KO)
      + "500 tirages : ceil(opt LP) <= tout cout entier >= opt (violations : %d)" % viol)

# ---------------------------------------------------------------------------
bloc("B9. Theoreme 2 et capacites STATIQUES : contre-exemple construit")
# u statique (lu a Build) : h(s') peut re-utiliser une capacite deja consommee.
# but p >= 2, un seul operateur o (u=1) produit p.
feas_s, val_s = solve_exact([1], [1], [([(0, 1)], 2)])
# apres avoir tire o une fois : marquage p=1, le LP STATIQUE autorise encore x<=1
feas_sp, val_sp = solve_exact([1], [1], [([(0, 1)], 1)])
print("  h(s)  [statique] : %s" % ("INFAISABLE (infini)" if not feas_s else val_s))
print("  h(s') [statique] : %s" % (val_sp if feas_sp else "INFAISABLE"))
print("  => h(s)=inf > c_o + h(s') = 2 : la CONSISTANCE (th. 2) tombe avec u statique,")
print("     exactement la reserve de 9.30. L'ADMISSIBILITE et le th. 3 tiennent :")
print("     u statique >= u restant => relaxation => h plus petit et inf reste une preuve.")

# ---------------------------------------------------------------------------
bloc("B10. Fuzz du simplexe : instances aleatoires resolues en exact -> table C++")
rnd = random.Random(20260817)
cases = []
n_infeas = 0
tries = 0
while len(cases) < 60 and tries < 4000:
    tries += 1
    n = rnd.randint(1, 4)
    nr = rnd.randint(1, 5)
    cost = [rnd.randint(0, 4) for _ in range(n)]
    upper = [rnd.choice([None, None, rnd.randint(1, 4)]) for _ in range(n)]
    rows = []
    for _ in range(nr):
        coefs = []
        for j in range(n):
            c = rnd.randint(-2, 3)
            if c:
                coefs.append((j, c))
        if not coefs:
            continue
        rows.append((coefs, rnd.randint(-3, 6)))
    if not rows:
        continue
    try:
        feas, val = solve_exact(cost, upper, rows)
    except UnboundedLPError:
        continue   # c>=0 : ne devrait jamais arriver ; on ecarte par prudence
    if not feas:
        n_infeas += 1
        if n_infeas > 20:
            continue   # garder un melange faisable/infaisable
    cases.append((cost, upper, rows, feas, val))
print("  %d instances retenues (%d infaisables) sur %d essais" %
      (len(cases), sum(1 for c in cases if not c[3]), tries))

out = []
out.append("// GENERE PAR verify_formulas.py (s21) — instances aleatoires resolues en")
out.append("// rationnels EXACTS par sympy.lpmin. Ne pas editer a la main.")
out.append("struct FuzzCase {")
out.append("\tsize_t n;")
out.append("\tdouble cost[4];")
out.append("\tdouble upper[4];   // -1 = sans borne")
out.append("\tsize_t nrows;")
out.append("\tdouble coef[5][4]; // coefficients denses par ligne")
out.append("\tdouble rhs[5];")
out.append("\tbool feas;")
out.append("\tdouble val;        // ceil(optimum exact), la valeur que rend le solveur")
out.append("};")
out.append("static const FuzzCase kFuzzCases[] = {")
for cost, upper, rows, feas, val in cases:
    n = len(cost)
    cc = [float(c) for c in cost] + [0.0] * (4 - n)
    uu = [(-1.0 if u is None else float(u)) for u in upper] + [0.0] * (4 - n)
    dense = []
    rr = []
    for coefs, rhs in rows:
        row = [0.0] * 4
        for j, c in coefs:
            row[j] = float(c)
        dense.append(row)
        rr.append(float(rhs))
    while len(dense) < 5:
        dense.append([0.0] * 4)
        rr.append(0.0)
    v = float(math.ceil(val)) if feas else 0.0
    out.append("\t{ %d, {%s}, {%s}, %d, {%s}, {%s}, %s, %.1f }," % (
        n,
        ", ".join("%.1f" % x for x in cc),
        ", ".join("%.1f" % x for x in uu),
        len(rows),
        ", ".join("{%s}" % ", ".join("%.1f" % x for x in row) for row in dense),
        ", ".join("%.1f" % x for x in rr),
        "true" if feas else "false",
        v))
out.append("};")
with open(sys.argv[1] if len(sys.argv) > 1 else "lp_fuzz_cases.inc", "w") as fh:
    fh.write("\n".join(out) + "\n")
print("  table C++ ecrite : %s" % (sys.argv[1] if len(sys.argv) > 1 else "lp_fuzz_cases.inc"))

print("\n=== FIN : toute ligne [ECHEC] ci-dessus est une erreur a corriger ===")
