# -*- coding: utf-8 -*-
"""LA FORME CLOSE RAFFINEE de l'entrelacement (s24quater).

La version precedente (s24_entrelacement_optimal.py) portait trois
idealisations. Chacune est LEVEE ici, et chaque levee rend la formule
strictement plus forte :

 (R1) GRAIN ATTEIGNABLE. k* = l*ln b donne des blocs de 1/ln b < 1 decision
      — INATTEIGNABLE des que b > e. Sous la contrainte k <= l (au plus un
      point d'archive par decision a choix), k |-> k*b**(l/k) est strictement
      DECROISSANTE sur [1, l] quand ln b > 1 : l'optimum ATTEIGNABLE est
      k = l, cout = b*l. (e*l*ln b etait un infimum non atteint ;
      b*l est atteint par « archiver chaque frontiere de choix ».)

 (R2) LE RETOUR N'EST PAS GRATUIT. Chaque essai depuis la profondeur d
      REJOUE d decisions (retour par rejeu, notre mecanisme reel). Cout
      exact a blocs egaux : C(k) = b**(L/k) * L*(k+1)/2. Sous k <= L et
      ln b > L/(L+1), l'optimum est k = L :
          C_rejeu = b * L*(L+1)/2      (QUADRATIQUE en L, pas lineaire —
      la version precedente sous-estimait d'un facteur ~L/2.)

 (R3) L'ORDRE REVIENT (rearrangement). Avec des arites PAR DECISION b_d,
      C = somme_d d * b_d : l'inegalite de rearrangement dit que le cout est
      minimise en triant les arites DECROISSANTES — les decisions a forte
      arite AUSSI TOT que la precedence (A1 < R < A2) le permet. Le modele
      a retour gratuit avait efface l'ordre ; le retour paye le restaure,
      en forme close, et il est CONSTRUCTIF (tri, pas recherche).

 (R4) LA QUEUE SCALAIRE, EXACTE SOUS REJEU :
          C_scal = b*(L-l_t)*(L-l_t+1)/2 + b**l_t * L
      (chaque essai de queue rejoue tout le prefixe). Rapport en forme
      close : R = C_scal/C_grille ~ 2*b**(l_t-1)/L pour la queue dominante.

 (R5) LE BUDGET EST UN QUANTILE, PAS UNE ESPERANCE. La fouille est un
      processus sans memoire : P(T > t) = exp(-t/E[T]) =>
          T_q = E[T] * ln(1/(1-q))     (P95 ~ 3*E[T] ; P99 ~ 4,6*E[T]).

 (R6) L'ALLOCATION UNIFORME SUR LA GRILLE EST C-COMPETITIVE. Course
      exponentielle sur C cellules de taux mu_c inconnus : l'uniforme donne
      T_unif = C/somme(mu_c) <= C * T_oracle — avec C <= 28 (grille 4x7),
      la borne est petite et AUCUN bandit n'est necessaire a cette taille.

 FORMULE MAITRESSE (assemblee, unites = pas de decision ; tau = cout d'un
 pas, w = workers) :
     E[T_conj] = (tau/w) * [ somme_d d * b_(sigma*(d)) ]  +  T_ferm
     avec sigma* = tri decroissant des arites sous precedence (R3),
     borne par (tau/w) * b_max * L(L+1)/2 (R2),
     T_ferm quantile via (R5) sur la course (R6).
 Le contenu PREDICTIF est dans les RAPPORTS (grille vs scalaire, ordre vs
 desordre), pas dans les secondes absolues : b**l modelise une fouille
 aveugle, la politique apprise reduit b effectif — jamais la structure.

Piege memoire s21 : positivite EXPLICITE partout.
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
g = sp.log(k * b**(l / k))          # ln du cout
dg = sp.simplify(sp.diff(g, k))     # 1/k - l*ln b/k**2
check("dg/dk = (k - l*ln b)/k**2",
      sp.simplify(dg - (k - l * lnb) / k**2) == 0)
# Sur k in [1, l] : k - l*ln b <= l*(1 - ln b) < 0 quand ln b > 1
# => strictement decroissante => minimum au bord k = l.
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
# essai du bloc i : rejoue (i-1)*L/k puis explore L/k ; b**(L/k) essais.
Ck = sp.summation(b**blk * ((i - 1) * blk + blk), (i, 1, k))
Ck = sp.simplify(Ck)
check("somme exacte : C(k) = b**(L/k) * L*(k+1)/2",
      sp.simplify(Ck - b**(L / k) * L * (k + 1) / 2) == 0)
# ln C decroissant en k au bord k=L quand ln b > L/(L+1) :
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
# => si x > y (forte arite d'abord), la difference est NEGATIVE : placer la
# forte arite TOT est optimal ; par echanges adjacents, le tri decroissant
# est l'optimum global sous precedence (argument d'echange standard).
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
# T_unif/T_oracle = C*mu_max/mu_sum <= C puisque mu_sum >= mu_max.
comp = C_sym * mu_max / mu_sum
check("competitivite <= C des que mu_sum >= mu_max",
      sp.simplify(comp.subs(mu_sum, mu_max) - C_sym) == 0 and
      bool(sp.simplify(comp.subs(mu_sum, 2 * mu_max) - C_sym / 2) == 0))
print("  grille 4x7 : C <= 28 — pas de bandit necessaire a cette taille.")

print(f"\nverification : {ok}/{tot}")
raise SystemExit(0 if ok == tot else 1)
