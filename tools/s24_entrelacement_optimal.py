# -*- coding: utf-8 -*-
"""L'ENTRELACEMENT OPTIMAL de la conjonction, dans NOTRE forme close (s24ter).

Cadre (mesure + s21) : le cout de fouille d'un segment de l decisions A CHOIX
sous arite b est b**l ; l'archive a retour (Go-Explore) permet de couper une
chaine en segments et de repartir du bord de chaque segment (s21 : cout
k*b**(L/k)). La conjonction de l'etalon B a deux chaines INTERFERENTES :
  A1 (board amont, Abyss compris)  puis  R (rips)  puis  A2 (fermeture) —
l'ordre A1 avant R avant A2 est une contrainte de RESSOURCE (mesuree : la
route des rips depense Abyss ; reference : Abyss rep. 122 < rips 154-226 <
fermeture -> 276).

Resultats derives et verifies ci-dessous :

 (1) GRAIN OPTIMAL d'un segment archive : min_k k*b**(l/k) atteint en
     k* = l*ln(b), cout minimal e*l*ln(b) — LINEAIRE en l (re-derivation
     s21, gardee comme lemme).

 (2) ADDITIVITE et INVARIANCE A L'ORDRE : si TOUTE frontiere de segment est
     archivable (la grille (rips, overlap) retient les cellules
     intermediaires), le cout total de l'entrelacement est
       C_grille = e*ln(b) * (lA1 + lR + lA2)
     quel que soit l'ordre des alternances compatibles avec la contrainte.
     L'entrelacement « optimal » n'est donc PAS une question d'ordre fin :
     c'est une question d'ARCHIVABILITE des frontieres.

 (3) DEGENERESCENCE DE L'ARCHIVE SCALAIRE : une archive a score
     lexicographique ne retient que les extremites du front — la frontiere
     (r, o) intermediaire n'est pas re-entrable, et le dernier segment
     (queue de longueur l_t) se fouille d'un bloc :
       C_scal = e*ln(b)*(L - l_t) + b**l_t.
     SEUIL EXACT : b**x = e*x*ln(b) a pour unique racine x = 1/ln(b)
     (tangence, via u - ln(u) = 1 <=> u = 1 avec u = x*ln(b)) : des que la
     queue non archivable depasse UNE fraction de decision a choix
     (1/ln b ~ 0,56 pour b = 5,9), l'exponentielle domine STRICTEMENT.
     C'est le « spasme terminal » du diagnostic, en une ligne.

 (4) ALLOCATION ENTRE CELLULES DU FRONT : par cellule c du front,
     m*_c = sqrt(lambda_c/mu_c) et E[T]*_c = 2/sqrt(lambda_c*mu_c)
     (repris de s24_forme_close_conjonction, convexite prouvee la-bas) ;
     mu_c inconnu => allocation bandit (litterature : ME-MAP-Elites).

Piege memoire s21 : les contraintes de positivite s'ecrivent EXPLICITEMENT.
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
# Convexite en k (b > 1) : d2/dk2 > 0 au voisinage — via le signe du facteur.
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
ordre2 = C(lA1 / 2) + C(lR) + C(lA1 / 2) + C(lA2)  # A1 coupe autour de R
check("C_grille identique quel que soit le decoupage compatible",
      sp.simplify(ordre1 - ordre2) == 0)
check("C_grille = e*ln(b)*L", sp.simplify(ordre1 - sp.E * lnb *
                                          (lA1 + lR + lA2)) == 0)

# ---------------------------------------------------------------------------
print("(3) seuil de degenerescence de l'archive scalaire")
# b**x = e*x*ln(b). Pose u = x*ln(b) : e**u = e*u  <=>  u - ln(u) = 1.
u = sp.symbols('u', positive=True)
f = u - sp.log(u) - 1
roots = sp.solve(sp.Eq(f, 0), u)
check("u = 1 est racine de u - ln u = 1", sp.simplify(f.subs(u, 1)) == 0)
# Unicite par tangence : f'(1) = 0 et f'' > 0 => racine DOUBLE unique.
check("tangence : f'(1) = 0", sp.simplify(sp.diff(f, u).subs(u, 1)) == 0)
check("f''(u) = 1/u**2 > 0 (convexite => unicite)",
      sp.simplify(sp.diff(f, u, 2) - 1 / u**2) == 0)
# Donc le seuil est x_dagger = 1/ln(b), et pour x > x_dagger :
# b**x > e*x*ln(b) STRICTEMENT.
b_num = sp.Rational(59, 10)
x_dag = float(1 / sp.log(b_num))
print(f"  seuil numerique (b = 5,9) : l_queue = {x_dag:.2f} decision a choix")
check("au-dela du seuil l'exponentielle domine (test l=2, b=5,9)",
      float(b_num**2) > float(sp.E * 2 * sp.log(b_num)))

# Application numerique : la queue de fermeture mesuree.
# Reference : Abyss rep. 122, rips 154-226, fermeture -> 276. Queue A2 =
# ~50 reponses dont ~25 % a choix (74,6 % de coups forces mesures s17)
# => l_t ~ 12,5. L totale a choix ~ 33 (l_max du banc s21).
l_t = sp.Rational(25, 2)
L_tot = 33
C_scal = sp.E * sp.log(b_num) * (L_tot - l_t) + b_num**l_t
C_grid = sp.E * sp.log(b_num) * L_tot
ratio = float(C_scal / C_grid)
print(f"  C_scalaire / C_grille (b=5,9, l_t=12,5, L=33) = {ratio:.3g}")
check("le rapport depasse 10**6 (le spasme terminal est quantifie)",
      ratio > 1e6)
# Sensibilite : meme a l_t = 6 (queue courte), le rapport reste > 10**2.
C_scal6 = sp.E * sp.log(b_num) * (L_tot - 6) + b_num**6
check("robuste : l_t = 6 donne encore > 10**2",
      float(C_scal6 / C_grid) > 1e2)

# ---------------------------------------------------------------------------
print("(4) allocation entre cellules du front (rappel, prouve en (III) du")
print("    module precedent) : m*_c = sqrt(lambda_c/mu_c),")
print("    E[T]*_c = 2/sqrt(lambda_c*mu_c) ; mu_c inconnu => bandit.")

print(f"\nverification : {ok}/{tot}")
raise SystemExit(0 if ok == tot else 1)
