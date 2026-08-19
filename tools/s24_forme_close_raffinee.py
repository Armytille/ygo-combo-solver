# -*- coding: utf-8 -*-
"""THE REFINED CLOSED FORM of the interleaving.

The previous version (s24_entrelacement_optimal.py) carried three
idealisations. Each is LIFTED here, and each lifting makes the formula
strictly stronger:

 (R1) REACHABLE GRAIN. k* = l*ln b gives blocks of 1/ln b < 1 decision,
      which is UNREACHABLE as soon as b > e. Under the constraint k <= l (at
      most one archive point per choice decision), k |-> k*b**(l/k) is
      strictly DECREASING over [1, l] when ln b > 1: the REACHABLE optimum is
      k = l, with cost b*l. (e*l*ln b was an infimum that is never attained;
      b*l is attained by "archive every choice frontier".)

 (R2) THE RETURN IS NOT FREE. Every attempt from depth d REPLAYS d
      decisions (return by replay, our actual mechanism). Exact cost with
      equal blocks: C(k) = b**(L/k) * L*(k+1)/2. Under k <= L and
      ln b > L/(L+1), the optimum is k = L:
          C_replay = b * L*(L+1)/2      (QUADRATIC in L, not linear, so the
      previous version understated it by a factor of ~L/2.)

 (R3) ORDER COMES BACK (rearrangement). With PER DECISION arities b_d,
      C = sum_d d * b_d: the rearrangement inequality says the cost is
      minimised by sorting the arities DECREASING, i.e. the high-arity
      decisions AS EARLY as the precedence (A1 < R < A2) allows. The
      free-return model had erased the order; a paid return restores it, in
      closed form, and it is CONSTRUCTIVE (a sort, not a search).

 (R4) THE SCALAR TAIL, EXACT UNDER REPLAY:
          C_scal = b*(L-l_t)*(L-l_t+1)/2 + b**l_t * L
      (every tail attempt replays the whole prefix). Ratio in closed form:
      R = C_scal/C_grid ~ 2*b**(l_t-1)/L for the dominant tail.

 (R5) THE BUDGET IS A QUANTILE, NOT AN EXPECTATION. The search is a
      memoryless process: P(T > t) = exp(-t/E[T]), hence
          T_q = E[T] * ln(1/(1-q))     (P95 ~ 3*E[T]; P99 ~ 4.6*E[T]).

 (R6) UNIFORM ALLOCATION OVER THE GRID IS C-COMPETITIVE. An exponential
      race over C cells with unknown rates mu_c: uniform gives
      T_unif = C/sum(mu_c) <= C * T_oracle. With C <= 28 (a 4x7 grid) the
      bound is small and NO bandit is needed at that size.

 MASTER FORMULA (assembled, units = decision steps; tau = the cost of one
 step, w = workers):
     E[T_conj] = (tau/w) * [ sum_d d * b_(sigma*(d)) ]  +  T_close
     with sigma* = the decreasing sort of the arities under precedence (R3),
     bounded by (tau/w) * b_max * L(L+1)/2 (R2),
     and T_close a quantile through (R5) over the race (R6).
 The PREDICTIVE content is in the RATIOS (grid vs scalar, ordered vs
 unordered), not in absolute seconds: b**l models a blind search, and the
 learned policy reduces the effective b, never the structure.

Known trap: positivity written EXPLICITLY everywhere.
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

b, l, k, L = sp.symbols('b l k L', positive=True)
lnb = sp.log(b)

# ---------------------------------------------------------------------------
print("(R1) l'optimum ATTEIGNABLE du grain : k = l, cout b*l (quand ln b > 1)")
g = sp.log(k * b**(l / k))          # ln of the cost
dg = sp.simplify(sp.diff(g, k))     # 1/k - l*ln b/k**2
check("dg/dk = (k - l*ln b)/k**2",
      sp.simplify(dg - (k - l * lnb) / k**2) == 0)
# Over k in [1, l]: k - l*ln b <= l*(1 - ln b) < 0 when ln b > 1
# => strictly decreasing => minimum at the edge k = l.
borne = sp.simplify((k - l * lnb).subs(k, l))
check("au bord k=l le signe est l*(1-ln b) (< 0 ssi b > e)",
      sp.simplify(borne - l * (1 - lnb)) == 0)
cout_atteint = sp.simplify((k * b**(l / k)).subs(k, l))
check("cout atteignable = b*l", sp.simplify(cout_atteint - b * l) == 0)
check("b*l > e*l*ln b est FAUX en general — l'infimum e*l*ln b n'est PAS "
      "atteint (b=5,9 : 5,9 > 4,83)",
      float((b * l / (sp.E * l * lnb)).subs([(b, sp.Rational(59, 10)),
                                             (l, 1)])) > 1)

# ---------------------------------------------------------------------------
print("(R2) retour par rejeu : C(k) = b**(L/k)*L*(k+1)/2, optimum k = L")
i = sp.symbols('i', positive=True, integer=True)
blk = L / k
# attempt at block i: replays (i-1)*L/k then explores L/k; b**(L/k) attempts.
Ck = sp.summation(b**blk * ((i - 1) * blk + blk), (i, 1, k))
Ck = sp.simplify(Ck)
check("somme exacte : C(k) = b**(L/k) * L*(k+1)/2",
      sp.simplify(Ck - b**(L / k) * L * (k + 1) / 2) == 0)
# ln C decreasing in k at the edge k=L when ln b > L/(L+1):
dlnC = sp.simplify(sp.diff(sp.log(b**(L / k) * L * (k + 1) / 2), k))
sgn = sp.simplify(dlnC.subs(k, L) * (L * (L + 1)))
check("d lnC/dk en k=L a le signe de L - (L+1)*ln b (< 0 ssi ln b > L/(L+1))",
      sp.simplify(sgn - (L - (L + 1) * lnb)) == 0)
C_rejeu = sp.simplify(Ck.subs(k, L))
check("C_rejeu = b*L*(L+1)/2", sp.simplify(C_rejeu - b * L * (L + 1) / 2) == 0)

# ---------------------------------------------------------------------------
print("(R3) l'ordre optimal par rearrangement (echange adjacent)")
d0, x, y = sp.symbols('d0 x y', positive=True)
cost_x_first = d0 * x + (d0 + 1) * y
cost_y_first = d0 * y + (d0 + 1) * x
diff = sp.simplify(cost_x_first - cost_y_first)
check("echange adjacent : cout(x d'abord) - cout(y d'abord) = y - x",
      sp.simplify(diff - (y - x)) == 0)
# => when x > y (high arity first), the difference is NEGATIVE: placing the
# high arity EARLY is optimal; by adjacent swaps, the decreasing sort is the
# global optimum under precedence (a standard exchange argument).
check("x > y => x d'abord est strictement moins cher",
      sp.simplify(diff.subs([(x, 3), (y, 2)])) == -1)

# ---------------------------------------------------------------------------
print("(R4) queue scalaire exacte sous rejeu, et rapport en forme close")
lt = sp.symbols('l_t', positive=True)
C_scal = b * (L - lt) * (L - lt + 1) / 2 + b**lt * L
b_num, L_num, lt_num = sp.Rational(59, 10), 33, sp.Rational(25, 2)
ratio = float((C_scal / C_rejeu).subs([(b, b_num), (L, L_num), (lt, lt_num)]))
print(f"  rapport numerique (b=5,9, L=33, l_t=12,5) : {ratio:.3g}")
check("le rapport reste > 10**6 sous le modele au rejeu paye", ratio > 1e6)
approx = float((2 * b**(lt - 1) / L).subs([(b, b_num), (L, L_num),
                                           (lt, lt_num)]))
check("l'approximation 2*b**(l_t-1)/L est au bon ordre (facteur < 3)",
      0.33 < ratio / approx < 3)

# ---------------------------------------------------------------------------
print("(R5) budget = quantile du processus sans memoire")
t, E_T, q = sp.symbols('t E_T q', positive=True)
Tq = sp.solve(sp.Eq(sp.exp(-t / E_T), 1 - q), t)[0]
check("T_q = E[T]*ln(1/(1-q))",
      sp.simplify(Tq - E_T * sp.log(1 / (1 - q))) == 0)
print(f"  P95 = {float(sp.log(20)):.2f} x E[T] ; P99 = "
      f"{float(sp.log(100)):.2f} x E[T]")

# ---------------------------------------------------------------------------
print("(R6) allocation uniforme C-competitive sur la grille")
C_sym = sp.symbols('C', positive=True)
mu_max, mu_sum = sp.symbols('mu_max mu_sum', positive=True)
# T_unif/T_oracle = C*mu_max/mu_sum <= C since mu_sum >= mu_max.
comp = C_sym * mu_max / mu_sum
check("competitivite <= C des que mu_sum >= mu_max",
      sp.simplify(comp.subs(mu_sum, mu_max) - C_sym) == 0 and
      bool(sp.simplify(comp.subs(mu_sum, 2 * mu_max) - C_sym / 2) == 0))
print("  grille 4x7 : C <= 28 — pas de bandit necessaire a cette taille.")

print(f"\nverification : {ok}/{tot}")
raise SystemExit(0 if ok == tot else 1)
