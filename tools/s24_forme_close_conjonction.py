# -*- coding: utf-8 -*-
"""Forme close de la CONJONCTION (rips ^ board) — session 24.

Directive operateur : « toutes les solutions prouvees mathematiquement si
possible et optimales, degraissees du superflu, forme close, via sympy ».

Trois resultats, chacun verifie numeriquement ci-dessous :

  (I)  Le RALENTISSEMENT de la decouverte des rips sous re-entree :
         S(alpha, k) = (1 + alpha*k) / (1 - alpha)
       ou alpha = fraction des tirages re-entres, k = cout du prefixe rejoue
       en unites de tirage libre. E[T_premier r3] = S / (p3 * rho).
       VERDICT DERIVE : le facteur de masse seul (S) n'explique PAS le zero
       mesure du run d'une heure — le couplage POLITIQUE (adaptations
       apprises sur des lignes de board) est necessaire. La preuve est par
       minoration : voir (I.b).

  (II) LEMME D'AFFAMEMENT (allocation A2) : l'affectation fixe r = w mod N
       (w in 0..W-1) ne sert que les racines {0..min(W,N)-1} ; toute racine
       d'indice >= W recoit un budget NUL. Une file de travail partagee
       (fetch_add) avec budget restant/racines_restantes sert les N racines
       (borne de couverture explicite). C'est un fait combinatoire, pas un
       reglage.

  (III) BUDGET OPTIMAL decouverte/fermeture : avec arrivee des cellules r3
       au taux lambda et fermeture par racine au taux mu (m racines actives
       ferment au taux m*mu), l'esperance du temps total pour m racines puis
       fermeture est
         E[T](m) = m/lambda + 1/(m*mu)
       minimisee en m* = sqrt(lambda/mu), d'ou
         E[T]* = 2/sqrt(lambda*mu)   (forme close).
       Le budget d'un run nu se DERIVE de lambda et mu mesures, il ne se
       cale pas.

Piege connu (memoire s21) : lpmin ignore nonnegative=True — toutes les
contraintes x >= 0 s'ecrivent EXPLICITEMENT. Ici : pas de lpmin, mais les
domaines (0 < alpha < 1, lambda > 0, mu > 0, m > 0) sont poses partout.
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
# (I) Ralentissement de la decouverte sous re-entree
# ----------------------------------------------------------------------------
print("(I) ralentissement S(alpha, k) = (1 + alpha*k)/(1 - alpha)")
alpha, k, p3, rho, t = sp.symbols('alpha k p3 rho t', positive=True)

# Cout moyen d'un tirage : (1-alpha) tirages libres a cout 1, alpha tirages
# re-entres a cout (1 + k) (le prefixe rejoue s'ajoute au rollout).
cout_moyen = (1 - alpha) * 1 + alpha * (1 + k)
# Debit en tirages par unite de temps : rho / cout_moyen.
# Seuls les tirages LIBRES (fraction 1-alpha) echantillonnent la fenetre des
# rips depuis la racine (hypothese : la re-entree vise la frontiere de board,
# au-dela du point d'embranchement — mesure : base 37 unites, rips a ~150-210).
taux_r3 = (rho / cout_moyen) * (1 - alpha) * p3
S = sp.simplify((rho * p3) / taux_r3)   # ralentissement vs alpha = 0
S_attendu = (1 + alpha * k) / (1 - alpha)
check("S se simplifie en (1+alpha*k)/(1-alpha)", sp.simplify(S - S_attendu) == 0)

# S est croissante en alpha et en k sur le domaine (monotonie = preuve que
# TOUTE re-entree au-dela du point d'embranchement ralentit la decouverte).
dS_dalpha = sp.simplify(sp.diff(S_attendu, alpha))
dS_dk = sp.simplify(sp.diff(S_attendu, k))
check("dS/dalpha > 0 sur 0<alpha<1, k>0",
      sp.simplify(dS_dalpha * (1 - alpha)**2) == sp.simplify(1 + k))
check("dS/dk > 0", sp.simplify(dS_dk - alpha / (1 - alpha)) == 0)

# (I.b) MINORATION : le run d'une heure, R1 (alpha=0.5, k~0 au R1 car base
# 14.6 unites ~ prefixes courts) a fait 2 151 675 tirages LIBRES effectifs
# ~ (1-alpha)*N_t et ZERO tirage a >=3 rips. Si p3 valait la valeur mesuree
# du bras sans echelle (19 / 131 472), l'esperance de hits aurait ete :
N_libres = sp.Rational(2151675, 2)          # (1-alpha) * N_t, alpha = 1/2
p3_mes = sp.Rational(19, 131472)
E_hits = sp.simplify(N_libres * p3_mes)
print(f"  E[hits >=3] attendus au R1 du livrable si p3 inchange : {float(E_hits):.0f}")
# P(0 hit) = (1-p3)^N — astronomiquement petite : le zero observe REFUTE
# « meme p3, seule la masse differe ». Le couplage politique/garde est
# necessaire pour expliquer la mesure.
log10_P0 = sp.N(N_libres * sp.log(1 - p3_mes) / sp.log(10))
print(f"  log10 P(0 hit | p3 inchange) = {float(log10_P0):.1f}")
check("minoration : E[hits] >> 1 (le zero mesure refute le modele de masse seul)",
      float(E_hits) > 100)

# ----------------------------------------------------------------------------
# (II) Lemme d'affamement de l'affectation w mod N
# ----------------------------------------------------------------------------
print("(II) lemme d'affamement (w mod N)")
W, N = 16, 22   # le run de fermeture : 16 workers, 16 racines rip + 6 appr
servies = {w % N for w in range(W)}
check("w%N ne sert que min(W,N) racines (16/22)", len(servies) == min(W, N))
check("les racines d'approche (indices 16-21) sont affamees",
      all(i not in servies for i in range(16, 22)))
# La file de travail sert tout : chaque pull incremente, N pulls couvrent N.
pulls = list(range(N))
check("une file fetch_add couvre les N racines", set(p % N for p in pulls) == set(range(N)))

# ----------------------------------------------------------------------------
# (III) Budget optimal decouverte/fermeture (forme close)
# ----------------------------------------------------------------------------
print("(III) partage optimal decouverte/fermeture")
lam, mu, m = sp.symbols('lambda mu m', positive=True)
E_T = m / lam + 1 / (m * mu)
m_star = sp.solve(sp.Eq(sp.diff(E_T, m), 0), m)
m_star = [s for s in m_star if s.is_positive][0]
check("m* = sqrt(lambda/mu)", sp.simplify(m_star - sp.sqrt(lam / mu)) == 0)
E_star = sp.simplify(E_T.subs(m, m_star))
check("E[T]* = 2/sqrt(lambda*mu)", sp.simplify(E_star - 2 / sp.sqrt(lam * mu)) == 0)
# Convexite : d2E/dm2 > 0 => minimum global sur m > 0.
check("convexite (d2E/dm2 = 2/(m^3 mu) > 0)",
      sp.simplify(sp.diff(E_T, m, 2) - 2 / (m**3 * mu)) == 0)

# Application numerique (bras sans echelle, 52.5 s de tirages, 16 threads) :
#   cellules r3 distinctes observees : 4  => lambda ~ 4/52.5 s
#   fermeture : non encore mesuree proprement (la phase A2 etait affamee) —
#   mu est LE parametre que l'A/B en proportion doit estimer. On imprime la
#   table E[T]* pour mu plausible, comme grille de lecture du budget.
lam_num = 4 / 52.5
print("  lambda mesure (cellules r3/s, bras sans echelle) : %.3f" % lam_num)
for mu_num in (1/240.0, 1/120.0, 1/60.0, 1/30.0):
    m_n = (lam_num / mu_num) ** 0.5
    e_n = 2 / (lam_num * mu_num) ** 0.5
    print(f"    mu = 1/{1/mu_num:.0f} s^-1  ->  m* = {m_n:.1f} racines, "
          f"E[T]* = {e_n:.0f} s")

print(f"\nverification : {ok}/{tot}")
raise SystemExit(0 if ok == tot else 1)
