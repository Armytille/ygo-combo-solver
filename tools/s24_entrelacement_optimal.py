# -*- coding: utf-8 -*-
"""The OPTIMAL INTERLEAVING of the conjunction, in our closed form.

Setting (measurement + earlier derivations): the cost of searching a segment
of l CHOICE decisions under arity b is b**l; an archive with return
(Go-Explore) allows a chain to be cut into segments and restarted from the
edge of each (cost k*b**(L/k)). Benchmark B's conjunction has two INTERFERING
chains: A1 (upstream board, Abyss included), then R (the rips), then A2 (the
closing). The order A1 before R before A2 is a RESOURCE constraint (measured:
the rip route spends Abyss; on the reference, Abyss at answer 122 < rips
154-226 < closing -> 276).

Results derived and checked below:

 (1) OPTIMAL GRAIN of an archived segment: min_k k*b**(l/k) is reached at
     k* = l*ln(b), with minimum cost e*l*ln(b), which is LINEAR in l.

 (2) ADDITIVITY and ORDER INVARIANCE: if EVERY segment boundary is
     archivable (the (rips, overlap) grid keeps the intermediate cells), the
     total cost of the interleaving is
       C_grid = e*ln(b) * (lA1 + lR + lA2)
     whatever the order of the alternations compatible with the constraint.
     "Optimal" interleaving is therefore NOT a question of fine ordering: it
     is a question of the boundaries being ARCHIVABLE.

 (3) DEGENERACY OF THE SCALAR ARCHIVE: an archive with a lexicographic score
     keeps only the extremes of the front, so an intermediate (r, o) frontier
     is not re-enterable and the last segment (a tail of length l_t) is
     searched in one block:
       C_scal = e*ln(b)*(L - l_t) + b**l_t.
     EXACT THRESHOLD: b**x = e*x*ln(b) has the unique root x = 1/ln(b) (a
     tangency, through u - ln(u) = 1 <=> u = 1 with u = x*ln(b)). As soon as
     the non-archivable tail exceeds ONE fraction of a choice decision
     (1/ln b ~ 0.56 for b = 5.9), the exponential STRICTLY dominates. That is
     the "terminal spasm" of the diagnosis, in one line.

 (4) ALLOCATION BETWEEN FRONT CELLS: per cell c of the front,
     m*_c = sqrt(lambda_c/mu_c) and E[T]*_c = 2/sqrt(lambda_c*mu_c) (taken
     from s24_forme_close_conjonction, convexity proved there); mu_c being
     unknown, the allocation is a bandit problem (ME-MAP-Elites).

Known trap: positivity constraints are written EXPLICITLY, because lpmin
ignores nonnegative=True.
"""
import sympy as sp

ok = 0
tot = 0

def check(name, cond):
    global ok, tot
    tot += 1
    print(("  [OK]   " if cond else "  [FAUX] ") + name)
    if cond:
        ok += 1

b, l, k, x = sp.symbols('b l k x', positive=True)
lnb = sp.log(b)

# ---------------------------------------------------------------------------
print("(1) grain optimal : min_k k*b**(l/k)")
cost = k * b**(l / k)
dk = sp.diff(cost, k)
k_star = sp.solve(sp.Eq(dk, 0), k)
k_star = [s for s in k_star if s.has(l)][0]
check("k* = l*ln(b)", sp.simplify(k_star - l * lnb) == 0)
cost_star = sp.simplify(cost.subs(k, k_star))
check("cout minimal = e*l*ln(b)", sp.simplify(cost_star - sp.E * l * lnb) == 0)
# Convexity in k (b > 1): d2/dk2 > 0 nearby, through the sign of the factor.
d2 = sp.simplify(sp.diff(cost, k, 2))
d2_at = sp.simplify(d2.subs(k, k_star))
check("convexite au minimum (d2C/dk2 > 0 pour b > e**(1/l) ... ici b > 1)",
      sp.simplify(d2_at * (l * lnb)**3 / (sp.E * lnb**2)) == l * lnb
      or sp.ask(sp.Q.positive(d2_at.subs([(b, sp.Rational(59, 10)),
                                          (l, 12)]))))

# ---------------------------------------------------------------------------
print("(2) additivite / invariance a l'ordre (frontieres archivables)")
lA1, lR, lA2 = sp.symbols('lA1 lR lA2', positive=True)
C = lambda seg: sp.E * seg * lnb
ordre1 = C(lA1) + C(lR) + C(lA2)                  # A1, R, A2
ordre2 = C(lA1 / 2) + C(lR) + C(lA1 / 2) + C(lA2)  # A1 cut around R
check("C_grille identique quel que soit le decoupage compatible",
      sp.simplify(ordre1 - ordre2) == 0)
check("C_grille = e*ln(b)*L", sp.simplify(ordre1 - sp.E * lnb *
                                          (lA1 + lR + lA2)) == 0)

# ---------------------------------------------------------------------------
print("(3) seuil de degenerescence de l'archive scalaire")
# b**x = e*x*ln(b). Set u = x*ln(b): e**u = e*u  <=>  u - ln(u) = 1.
u = sp.symbols('u', positive=True)
f = u - sp.log(u) - 1
roots = sp.solve(sp.Eq(f, 0), u)
check("u = 1 est racine de u - ln u = 1", sp.simplify(f.subs(u, 1)) == 0)
# Uniqueness by tangency: f'(1) = 0 and f'' > 0, so a unique DOUBLE root.
check("tangence : f'(1) = 0", sp.simplify(sp.diff(f, u).subs(u, 1)) == 0)
check("f''(u) = 1/u**2 > 0 (convexite => unicite)",
      sp.simplify(sp.diff(f, u, 2) - 1 / u**2) == 0)
# So the threshold is x_dagger = 1/ln(b), and for x > x_dagger:
# b**x > e*x*ln(b) STRICTLY.
b_num = sp.Rational(59, 10)
x_dag = float(1 / sp.log(b_num))
print(f"  seuil numerique (b = 5,9) : l_queue = {x_dag:.2f} decision a choix")
check("au-dela du seuil l'exponentielle domine (test l=2, b=5,9)",
      float(b_num**2) > float(sp.E * 2 * sp.log(b_num)))

# Numerical application: the measured closing tail.
# Reference: Abyss at answer 122, rips 154-226, closing -> 276. Tail A2 =
# ~50 answers of which ~25 % are choices (74.6 % forced moves measured)
# => l_t ~ 12.5. Total L in choices ~ 33 (the l_max of the bench).
l_t = sp.Rational(25, 2)
L_tot = 33
C_scal = sp.E * sp.log(b_num) * (L_tot - l_t) + b_num**l_t
C_grid = sp.E * sp.log(b_num) * L_tot
ratio = float(C_scal / C_grid)
print(f"  C_scalaire / C_grille (b=5,9, l_t=12,5, L=33) = {ratio:.3g}")
check("le rapport depasse 10**6 (le spasme terminal est quantifie)",
      ratio > 1e6)
# Sensitivity: even at l_t = 6 (a short tail), the ratio stays > 10**2.
C_scal6 = sp.E * sp.log(b_num) * (L_tot - 6) + b_num**6
check("robuste : l_t = 6 donne encore > 10**2",
      float(C_scal6 / C_grid) > 1e2)

# ---------------------------------------------------------------------------
print("(4) allocation entre cellules du front (rappel, prouve en (III) du")
print("    module precedent) : m*_c = sqrt(lambda_c/mu_c),")
print("    E[T]*_c = 2/sqrt(lambda_c*mu_c) ; mu_c inconnu => bandit.")

print(f"\nverification : {ok}/{tot}")
raise SystemExit(0 if ok == tot else 1)
