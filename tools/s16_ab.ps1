# A/B DE LA SESSION 16 — ETALON B OPTIMISE, le seul juge ou UNE SOLUTION EXISTE.
#
# POURQUOI CE CAS ET PAS L'ETALON A. La ligne de l'operateur
# (`synchron handrip optimized.yrpX`) satisfait la discipline complete jusqu'au
# bout — 45 fenetres adverses sous menace, 0 decouverte, 0 MSG_RETRY, meme board
# (9.22 (i)). Tout echec du solveur y est donc imputable au solveur, et non a un
# but que le jeu interdirait — la faute qui a coute sept sessions (9.22 (f)).
# L'etalon A, lui, n'a AUCUN plan resolu connu : on ne peut pas y apprendre de
# landmarks, faute de corpus. Le mecanisme n'y est pas jugeable, et le dire vaut
# mieux qu'un bras qui aurait l'air de tourner.
#
# LES JUGES, DANS L'ORDRE :
#   1. « resolutions atteintes par tirage » >=1 / >=2 / >=3. Le verrou mesure de
#      l'etalon B est la JONCTION board+rips : >=3 vaut ZERO chez le temoin.
#   2. « meilleure crete AUX resolutions completes » — jusqu'ou montent les
#      lignes qui ont fait tous les rips.
#   3. l'approche ECRITE (best_approach_kof8.yrp), jamais la ligne « NRPA best ».
#   4. « landmarks : h moyen » — le critere INTERNE du mecanisme. S'il ne
#      descend pas, le mecanisme est inerte et aucun juge de recherche n'a de
#      sens (piege 52).
#
# LE BRAS `lm0` EST CELUI QUI COMPTE POUR L'HONNETETE : graphe appris et
# MESURE, poids ZERO. Sans lui, un gain de `lmw` serait attribuable au cout du
# mecanisme (requetes de zone en plus par decision) aussi bien qu'a son signal.
# C'est le meme role que le bras `ctx8` de 9.21 (j).
#
# BINAIRE : un seul, non-PGO, IDENTIQUE pour tous les bras. La session n'a pas
# re-deroule de pipeline PGO : un A/B apparie n'en a pas besoin (les deux bras
# paient le meme prix), et le PGO aurait ete un second facteur. Consequence a
# assumer : les DEBITS ne sont pas comparables aux chiffres de la session 15.
param([int]$Ms = 300000,
      [string[]]$Seeds = @('888'),
      [string[]]$Bras = @('temoin', 'lm0', 'lmw20'),
      [string]$Prefixe = 's16ab',
      [string]$Corpus = 'corpus_b16')

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
$bopt = "D:\ProjectIgnis\replay\synchron handrip optimized.yrpX"

$defs = @{
    # temoin : la commande de 9.22 (i), a l'octet pres.
    'temoin'  = @()
    # graphe APPRIS et MESURE, poids nul : isole le COUT du signal.
    'lm0'     = @('--landmarks', $Corpus)
    # le `h` appris entre dans le score des TIRAGES — 99 % du travail.
    'lmw20'   = @('--landmarks', $Corpus, '--landmark-w', '20')
    'lmw60'   = @('--landmarks', $Corpus, '--landmark-w', '60')
    # le meme signal, mais dans le `h` du FINISSEUR seulement (point d'entree
    # historique de `recipe_h`). Un seul facteur d'ecart avec lmw20.
    'lmh1'    = @('--landmarks', $Corpus, '--landmark-h', '1')
    # LIGNE TENUE A L'ECART. Le corpus par defaut contient la ligne meme qu'on
    # juge : un gain s'y lirait comme « le solveur suit la reponse qu'on lui a
    # donnee », ce qui ne prouve rien sur un cas neuf. Ce bras n'apprend QUE sur
    # `synchron handrip 2` — une autre ligne, qui atteint le meme board et
    # rippe, mais VIOLE la garde permanente (9.22 (i)) et n'est donc pas la
    # solution du cas juge. Si le gain survit, les landmarks portent au-dela de
    # la ligne dont ils sortent.
    # RESERVE : c'est une ligne TENUE A L'ECART, pas une INSTANCE tenue a
    # l'ecart — meme deck, meme board. La generalisation reste a mesurer sur un
    # deck que le corpus n'a jamais vu.
    'lmx60'   = @('--landmarks', "D:\ProjectIgnis\replay\synchron handrip 2.yrpX",
                  '--landmark-w', '60')
    # --- chantier (c) : departager un repare de la s15 sur un juge VALIDE ---
    # 9.22 (d) : --archive-spread survit a son criblage interne (expansions par
    # racine 15 -> 88) et n'a JAMAIS ete juge. Son juge est ce cas-ci, ou 9.20
    # (d) mesure 32 lignes arrivees au board PAR le finisseur.
    'spread'  = @('--archive-spread')
    'nophase' = @('--no-phase-change')
    'canon'   = @('--canonical-zones')
}

function Invoke-B {
    param([string]$Out, [string]$Seed, [int]$Budget, [string[]]$Extra)
    Write-Output "--- $Out (graine $Seed)"
    & .\bin\Release\combosolver.exe $bopt `
        --scriptdir ..\deps\scripts_2026-04-13\script `
        --start $bopt --no-plan --solve-ms $Budget --seed $Seed `
        --guard "5:Crystal Wing|Zalen@terrain+Junk Signal@main" `
        --no-activate "Duel Evolution - Assault Zone" `
        --no-chain Zalen --no-chain "Crystal Wing" `
        --resolve "PSY-Framelord Omega@terrain:2" `
        --resolve "Trishula, Dragon of the Ice Barrier@terrain" `
        @Extra `
        --finisher levin --archive-k 24 --outdir $Out *> "$Out.log"
    Write-Output "    EXIT=$LASTEXITCODE"
}

foreach ($s in $Seeds) {
    foreach ($b in $Bras) {
        if (-not $defs.ContainsKey($b)) { Write-Output "!! bras inconnu : $b"; continue }
        Invoke-B "${Prefixe}_${b}_$s" $s $Ms $defs[$b]
    }
}
Write-Output "TERMINE"
