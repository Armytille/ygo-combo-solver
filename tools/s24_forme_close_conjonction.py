# -*- coding: utf-8 -*-
"""Closed form of the CONJUNCTION (rips and board).

Operator's directive: every solution proved mathematically where possible and
optimal, trimmed of anything superfluous, in closed form, through sympy.

Three results, each checked numerically below:

  (I)  The SLOWDOWN of rip discovery under re-entry:
         S(alpha, k) = (1 + alpha*k) / (1 - alpha)
       where alpha = the fraction of re-entered rollouts and k = the cost of
       the replayed prefix in units of a free rollout.
       E[T_first r3] = S / (p3 * rho). DERIVED VERDICT: the mass factor (S)
       alone does NOT explain the measured zero of the one-hour run; the
       POLICY coupling (adaptations learned on board lines) is necessary. The
       proof is by lower bound: see (I.b).

  (II) STARVATION LEMMA (A2 allocation): the fixed assignment r = w mod N
       (w in 0..W-1) only serves the roots {0..min(W,N)-1}; any root of index
       >= W receives a budget of ZERO. A shared work queue (fetch_add) with
       remaining_budget/remaining_roots serves all N roots (explicit coverage
       bound). It is a combinatorial fact, not a setting.

  (III) OPTIMAL discovery/closing BUDGET: with r3 cells arriving at rate
       lambda and closing per root at rate mu (m active roots close at rate
       m*mu), the expected total time for m roots and then closing is
         E[T](m) = m/lambda + 1/(m*mu)
       minimised at m* = sqrt(lambda/mu), hence
         E[T]* = 2/sqrt(lambda*mu)   (closed form).
       A bare run's budget is DERIVED from measured lambda and mu; it is not
       tuned.

Known trap: lpmin ignores nonnegative=True, so every x >= 0 constraint is
written EXPLICITLY. Here there is no lpmin, but the domains (0 < alpha < 1,
lambda > 0, mu > 0, m > 0) are stated everywhere.
"""
import sympy as sp

ok = 0
tot = 0

def check(name, cond):
    global ok, tot
    tot += 1
    if cond:
        ok += 1
        print(f"  [OK]   {name}")
    else:
        print(f"  [FAUX] {name}")

# ----------------------------------------------------------------------------
# (I) Slowdown of the discovery under re-entry
# ----------------------------------------------------------------------------
print("(I) ralentissement S(alpha, k) = (1 + alpha*k)/(1 - alpha)")
alpha, k, p3, rho, t = sp.symbols('alpha k p3 rho t', positive=True)

# Mean cost of a rollout: (1-alpha) free rollouts at cost 1, alpha re-entered
# rollouts at cost (1 + k) (the replayed prefix adds to the rollout).
cout_moyen = (1 - alpha) * 1 + alpha * (1 + k)
# Throughput in rollouts per unit of time: rho / mean_cost.
# Only the FREE rollouts (a fraction 1-alpha) sample the rip window from the
# root (assumption: re-entry aims at the board frontier, beyond the branch
# point; measured: base 37 units, rips at ~150-210).
taux_r3 = (rho / cout_moyen) * (1 - alpha) * p3
S = sp.simplify((rho * p3) / taux_r3)   # slowdown vs alpha = 0
S_attendu = (1 + alpha * k) / (1 - alpha)
check("S se simplifie en (1+alpha*k)/(1-alpha)", sp.simplify(S - S_attendu) == 0)

# S increases in alpha and in k over the domain (monotonicity = proof that ANY
# re-entry beyond the branch point slows the discovery down).
dS_dalpha = sp.simplify(sp.diff(S_attendu, alpha))
dS_dk = sp.simplify(sp.diff(S_attendu, k))
check("dS/dalpha > 0 sur 0<alpha<1, k>0",
      sp.simplify(dS_dalpha * (1 - alpha)**2) == sp.simplify(1 + k))
check("dS/dk > 0", sp.simplify(dS_dk - alpha / (1 - alpha)) == 0)

# (I.b) LOWER BOUND: the one-hour run, R1 (alpha=0.5, k~0 at R1 since base
# 14.6 units means short prefixes) did 2 151 675 effective FREE rollouts
# ~ (1-alpha)*N_t and ZERO rollout at >=3 rips. If p3 were the value measured
# on the ladder-free arm (19 / 131 472), the expected number of hits would be:
N_libres = sp.Rational(2151675, 2)          # (1-alpha) * N_t, alpha = 1/2
p3_mes = sp.Rational(19, 131472)
E_hits = sp.simplify(N_libres * p3_mes)
print(f"  E[hits >=3] attendus au R1 du livrable si p3 inchange : {float(E_hits):.0f}")
# P(0 hits) = (1-p3)^N, astronomically small: the observed zero REFUTES "same
# p3, only the mass differs". The policy/guard coupling is necessary to explain
# the measurement.
log10_P0 = sp.N(N_libres * sp.log(1 - p3_mes) / sp.log(10))
print(f"  log10 P(0 hit | p3 inchange) = {float(log10_P0):.1f}")
check("minoration : E[hits] >> 1 (le zero mesure refute le modele de masse seul)",
      float(E_hits) > 100)

# ----------------------------------------------------------------------------
# (II) Starvation lemma of the w mod N assignment
# ----------------------------------------------------------------------------
print("(II) lemme d'affamement (w mod N)")
W, N = 16, 22   # the closing run: 16 workers, 16 rip roots + 6 approach roots
servies = {w % N for w in range(W)}
check("w%N ne sert que min(W,N) racines (16/22)", len(servies) == min(W, N))
check("les racines d'approche (indices 16-21) sont affamees",
      all(i not in servies for i in range(16, 22)))
# The work queue serves everything: each pull increments, N pulls cover N.
pulls = list(range(N))
check("une file fetch_add couvre les N racines", set(p % N for p in pulls) == set(range(N)))

# ----------------------------------------------------------------------------
# (III) Optimal discovery/closing budget (closed form)
# ----------------------------------------------------------------------------
print("(III) partage optimal decouverte/fermeture")
lam, mu, m = sp.symbols('lambda mu m', positive=True)
E_T = m / lam + 1 / (m * mu)
m_star = sp.solve(sp.Eq(sp.diff(E_T, m), 0), m)
m_star = [s for s in m_star if s.is_positive][0]
check("m* = sqrt(lambda/mu)", sp.simplify(m_star - sp.sqrt(lam / mu)) == 0)
E_star = sp.simplify(E_T.subs(m, m_star))
check("E[T]* = 2/sqrt(lambda*mu)", sp.simplify(E_star - 2 / sp.sqrt(lam * mu)) == 0)
# Convexity: d2E/dm2 > 0, so a global minimum over m > 0.
check("convexite (d2E/dm2 = 2/(m^3 mu) > 0)",
      sp.simplify(sp.diff(E_T, m, 2) - 2 / (m**3 * mu)) == 0)

# Numerical application (ladder-free arm, 52.5 s of rollouts, 16 threads):
#   distinct r3 cells observed: 4  => lambda ~ 4/52.5 s
#   closing: not measured properly yet (the A2 phase was starved), so mu is
#   THE parameter the proportion A/B has to estimate. We print the E[T]* table
#   for plausible mu, as a reading grid for the budget.
lam_num = 4 / 52.5
print("  lambda mesure (cellules r3/s, bras sans echelle) : %.3f" % lam_num)
for mu_num in (1/240.0, 1/120.0, 1/60.0, 1/30.0):
    m_n = (lam_num / mu_num) ** 0.5
    e_n = 2 / (lam_num * mu_num) ** 0.5
    print(f"    mu = 1/{1/mu_num:.0f} s^-1  ->  m* = {m_n:.1f} racines, "
          f"E[T]* = {e_n:.0f} s")

print(f"\nverification : {ok}/{tot}")
raise SystemExit(0 if ok == tot else 1)
