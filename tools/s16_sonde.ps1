# SONDE DE REPETITION (session 16) — l'INSTRUMENT AVANT LE MECANISME (piege 40).
#
# CE QU'ELLE TRANCHE. Le mur nomme en 9.22 (h) est « atteindre un sous-but
# consomme ce dont le suivant a besoin ». Deux pannes OPPOSEES rendent le meme
# `best_overlap`, et rien dans le dossier ne les separait :
#   - le 2e exemplaire n'est JAMAIS TENTE : le materiau etait encore la quand le
#     1er est tombe. Correctif = ECHANTILLONNAGE.
#   - il est TOUJOURS PERDU : la chaine etait deja consommee. Correctif = `h`.
# Elles appellent des chantiers opposes. Tant qu'on ne sait pas laquelle on a,
# tout mecanisme ecrit est un pari.
#
# LE JUGE EST LA LIGNE « VERDICT » du bloc `--probe-repeat`, pas la conversion.
# Ce n'est PAS un A/B : aucun bras temoin, aucune graine multiple a lire. Une
# sonde se lit sur ses propres compteurs.
#
# BUDGET. 90 s suffisent : la sonde compte des TIRAGES (des centaines de
# milliers a ce budget), pas des conversions. Le budget long ne sert qu'a
# resserrer une queue de distribution qu'on ne lit pas.
#
# BINAIRE : non-PGO assume. La sonde rend des FRACTIONS (conserve / consomme),
# pas des debits — le PGO changerait le nombre de tirages, pas leur proportion.
# Le PGO de la session reste requis AVANT l'A/B du mecanisme.
param([int]$Ms = 90000,
      [string]$Seed = '888',
      [string]$Prefixe = 's16',
      [string[]]$Cas = @('A', 'B'))

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
$repo = "D:\ProjectIgnis\repositories"
$gabarit = "D:\ProjectIgnis\replay2video\combosolver\gabarits\etalon_a_lunalight.yrp"
$bopt = "D:\ProjectIgnis\replay\synchron handrip optimized.yrpX"

# --- ETALON A : 3x Liger Dancer + Bagooska, but seul, gabarit EPINGLE ---------
#
# DECK ET MAIN CHANGES PAR L'OPERATEUR (session 16). Le board cible ne bouge
# pas ; ce qui change est le DEPART :
#   * le deck Lunalight.ydk a ete refait ;
#   * la main n'est plus « 3 Fire Formation - Tenki » mais
#     « 1 Lunalight Gold Leo + 3 Fake Trap » — QUATRE cartes, pas trois.
# `BuildSyntheticStart` derive `start_hand` de la taille de --hand, donc le
# gabarit (main 3) est correctement surcharge. NB : `start_hand` est PARTAGE
# entre les deux joueurs — l'adversaire pioche donc 4 lui aussi.
#
# CODES ET NON NOMS, et ce n'est pas un detail : « Lunalight Gold Leo » est
# AMBIGU dans la base (8379983 et 101301005 portent ce nom), comme « Junk
# Meister » et « Crimson Dragon » l'etaient. Un nom ambigu fait sortir l'outil.
#   8379983 = Lunalight Gold Leo      3027001 = Fake Trap
#
# TOLERANCE DEMANDEE PAR L'OPERATEUR — « seuls les monstres comptent ; des
# magies/pieges en zone S/T ou des monstres EN PLUS sur le board ne sont pas
# genants, du moment qu'on a tout ce qui est demande ». C'est EXACTEMENT le
# comportement par defaut depuis la session 15 : une cible POSEE (`--target`)
# se controle par INCLUSION sur des entrees RELACHEES (zone, code, face), et la
# position ATK/DEF est repliee par le critere de but. Aucun drapeau a ajouter —
# `--target-exact` ferait l'inverse.
function Invoke-A {
    param([string]$Out, [int]$Budget)
    Write-Output "--- $Out (etalon A, graine $Seed)"
    & .\bin\Release\combosolver.exe $gabarit `
        --scriptdir "D:\ProjectIgnis\replay2video\deps\compat_2026-08" `
        --scriptdir ..\deps\scripts_2026-04-13\script `
        --scriptdir "$repo\delta-bagooska\script" `
        --scriptdir "$repo\delta-puppet\script" `
        --deck "D:\ProjectIgnis\deck\Lunalight.ydk" `
        --hand "8379983|3027001|3027001|3027001" `
        --no-ref `
        --target 54701958 --target 54701958 --target 54701958 `
        --target "90590304@DEF" `
        --max-decisions 700 `
        --resolve 4731783 --resolve 2344618 --resolve 47705572 `
        --summon-min "54701958:3" `
        --hint 35618217 --hint 24550676 --hint 100460013 --hint 24094653 `
        --hint 48444114 --hint 83190280 --hint 50277355 --hint 14152693 `
        --options-online 60 --probe-repeat `
        --solve-ms $Budget --seed $Seed --finisher levin --archive-k 24 `
        --outdir $Out *> "$Out.log"
    Write-Output "    EXIT=$LASTEXITCODE"
}

# --- ETALON B OPTIMISE : discipline complete, garde PERMANENTE ----------------
# La ligne de l'operateur satisfait tout (9.22 (i)) : UNE SOLUTION EXISTE, donc
# tout echec est imputable au solveur. Les deux entrees surveillees sont les
# `--resolve` deja presents (Omega x2, Trishula) — aucun drapeau ajoute, donc
# aucun facteur change hors la sonde elle-meme.
function Invoke-B {
    param([string]$Out, [int]$Budget)
    Write-Output "--- $Out (etalon B optimise, graine $Seed)"
    & .\bin\Release\combosolver.exe $bopt `
        --scriptdir ..\deps\scripts_2026-04-13\script `
        --start $bopt --no-plan --solve-ms $Budget --seed $Seed `
        --guard "5:Crystal Wing|Zalen@terrain+Junk Signal@main" `
        --no-activate "Duel Evolution - Assault Zone" `
        --no-chain Zalen --no-chain "Crystal Wing" `
        --resolve "PSY-Framelord Omega@terrain:2" `
        --resolve "Trishula, Dragon of the Ice Barrier@terrain" `
        --probe-repeat `
        --finisher levin --archive-k 24 --outdir $Out *> "$Out.log"
    Write-Output "    EXIT=$LASTEXITCODE"
}

# --- ETALON B, axe SYNCHRO MANQUANTE -----------------------------------------
# 9.22 (h) : le bras nu s'arrete a 7/8, et la carte qui manque est TOUJOURS
# `Hot Red Dragon Archfiend Abyss` (9753964), avec un `Accel Synchro Stardust
# Dragon` en trop a sa place. La surveiller repond a « a quelle decision son
# materiau a-t-il ete consomme ? ».
# RESERVE, dite d'avance : `--summon-min` n'est PAS neutre — il ajoute un
# gradient de resolution (+250) et le biais d'indice d'office. Ce bras n'est
# donc pas apparie au precedent ; il se lit seul, sur sa propre sonde.
function Invoke-Bs {
    param([string]$Out, [int]$Budget)
    Write-Output "--- $Out (etalon B optimise + surveillance Hot Red, graine $Seed)"
    & .\bin\Release\combosolver.exe $bopt `
        --scriptdir ..\deps\scripts_2026-04-13\script `
        --start $bopt --no-plan --solve-ms $Budget --seed $Seed `
        --guard "5:Crystal Wing|Zalen@terrain+Junk Signal@main" `
        --no-activate "Duel Evolution - Assault Zone" `
        --no-chain Zalen --no-chain "Crystal Wing" `
        --resolve "PSY-Framelord Omega@terrain:2" `
        --resolve "Trishula, Dragon of the Ice Barrier@terrain" `
        --summon-min "9753964:1" --probe-repeat `
        --finisher levin --archive-k 24 --outdir $Out *> "$Out.log"
    Write-Output "    EXIT=$LASTEXITCODE"
}

foreach ($c in $Cas) {
    switch ($c) {
        'A'  { Invoke-A  "${Prefixe}_sondeA_$Seed"  $Ms }
        'B'  { Invoke-B  "${Prefixe}_sondeB_$Seed"  $Ms }
        'Bs' { Invoke-Bs "${Prefixe}_sondeBs_$Seed" $Ms }
        default { Write-Output "!! cas inconnu : $c" }
    }
}
Write-Output "TERMINE"
