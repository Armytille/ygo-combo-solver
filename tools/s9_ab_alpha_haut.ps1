# PROLONGEMENT DU CADRAN ALPHA VERS LE HAUT (session 9, C3 — suite).
#
# POURQUOI CE SECOND CADRAN EXISTE
#
# Le premier (tools/s9_ab_alpha.ps1) a ete centre sur une arithmetique fausse.
# J'avais suppose h_root = 1 partout apres la correction C3, parce que le
# finisseur part d'un etat de recul « deja a 7/8 cartes ». La colonne h0=,
# ajoutee par la meme correction, montre que c'est faux : h_root vaut 1 a la
# racine « recul 0 » — celle qui EPUISE en 42 expansions et ne pese rien dans la
# comparaison — mais 4, 5 et 6 aux reculs 10, 20/45 et 30, qui sont les quatre
# racines effectivement comparees.
#
# L'equivalence correcte entre les deux echelles est donc :
#
#     inv_w = exp(alpha * h(n) / h_root)
#     session 8 : h_root = 8 (constante)   ->  coefficient effectif alpha_s8 / 8
#     session 9 : h_root = h(racine)       ->  coefficient effectif alpha_s9 / h0
#     equivalence :  alpha_s9 = alpha_s8 * h0 / 8,  soit x0,5 a x0,75
#
# Le cadran 1/2/3/5/8 couvre donc des equivalents s8 de ~1,5 a ~13 : il est
# ENTIEREMENT SOUS le point ou la session 8 avait vu son gain (alpha_s8 = 25,
# equivalent s9 ~ 12,5 a 18,75). Il mesure la queue basse, pas l'optimum.
#
# Ce cadran-ci va chercher l'optimum la ou il devrait etre, et le depasse d'un
# cran pour qu'il soit ENCADRE et non seulement atteint (piege 46 : un cadran se
# prolonge, et un point isole ne prouve rien) :
#
#     alpha_s9   12    16    20    28
#     equiv. s8  ~16   ~21   ~27   ~37
#
# Montage identique au premier cadran — etalon 0, A/B DETERMINISTE. Meme
# controle gratuit : « appr0 recul 0 » doit rendre 42 exp., b=0, EPUISE dans
# chaque bras.
param([int]$Ms = 300000,
      [int]$FinMin = 260000,
      [string]$Seed = '888',
      [string]$Approche = 's8_B_T_s888\best_approach_7of8.yrp')

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
$ref = "D:\ProjectIgnis\replay\synchron handrip 2.yrpX"

$bras = @(
    @{ nom = 'A12'; arg = @('--reroot-h', '12') },
    @{ nom = 'A16'; arg = @('--reroot-h', '16') },
    @{ nom = 'A20'; arg = @('--reroot-h', '20') },
    @{ nom = 'A28'; arg = @('--reroot-h', '28') }
)

foreach ($b in $bras) {
    $out = "s9_F_$($b.nom)"
    Write-Output "--- finisseur bras $($b.nom) -> $out"
    & .\bin\Release\combosolver.exe $ref `
        --scriptdir ..\deps\scripts_2026-04-13\script `
        --start $ref --no-plan --no-nrpa `
        --approach $Approche --finisher-min $FinMin `
        --solve-ms $Ms --seed $Seed `
        --no-chain Zalen --no-chain "Crystal Wing" `
        @($b.arg) --outdir $out *> "$out.log"
    Write-Output "    EXIT=$LASTEXITCODE"
}
Write-Output "TERMINE"
