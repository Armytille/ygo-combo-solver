# LEVIERS DE MASSE, sur le catalogue AUTO-MINE (session 14, suite)
#
# TOUS CES BRAS SONT A DRAPEAU SEUL : aucun rebuild, donc ils tournent sur le
# binaire EXACT qui a mesure les temoins de 9.21 (d). L'appariement est gratuit
# et parfait — le temoin de chaque bras est `s14_on_on_<graine>`
# (--options-online 60, la forme gagnante), deja mesure, on ne le relance pas.
#
# CE QUI DESIGNE LE PREMIER BRAS. Les NEUF catalogues auto-mines de la session
# (A/B + diversite + smoke) rendent tous `max = 8` avec des moyennes de 6,8 a
# 7,9 : la distribution est COLLEE au plafond `--options-len` (defaut 8). La
# selection par perte de Levin veut des macros plus longues et n'en a pas le
# droit — or la longueur est precisement ce qui attaque l'EXPOSANT (9.19 (b) :
# une ligne de 170 decisions tombe a 32 quand les macros absorbent).
#
# PILOTE D'ABORD (piege 40 : instrumenter avant de calibrer). Deux runs sur une
# graine suffisent a repondre « le plafond leve, les macros s'allongent-elles
# vraiment ? ». Si `max` reste a 8, ce n'est pas le plafond qui liait mais le
# SUPPORT (une sequence plus longue se repete moins souvent, --options-support
# 2 devient la contrainte active) — et le bras suivant est --options-support,
# pas --options-len. Surveiller aussi la duree du minage : elle croit avec
# max_len (enumeration des sous-sequences de longueur 2..max_len a chaque
# position) et doit rester sous la seconde.
#
# LIRE : longueur max et moyenne du catalogue AVANT tout le reste (le pilote ne
# juge que ca), puis approche ECRITE k/4, >=2 / >=3, absorbees/prise.
param([int]$Ms = 300000,
      [int]$Period = 60,
      [string[]]$Seeds = @('888'),
      [string[]]$Bras = @('len16', 'len24'))

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
$repo = "D:\ProjectIgnis\repositories"
# GABARIT EPINGLE (session 14). Il pointait sur le _LastReplay.yrpX de
# l'installation EDOPro — un fichier VIVANT, reecrit des que quelqu'un joue un
# duel. C'est arrive EN PLEIN PILOTE le 15/08/2026 a 23:42 : deux runs sont
# morts avec « aucun yrp1 embarque ». Bruyamment, pas en silence — et l'en-tete
# de gabarit imprime par le rapport a permis de VERIFIER que les runs anterieurs
# portaient tous le meme duel, au lieu de le supposer. Meme famille que la .cdb
# vivante non epinglee (9.20). Le gabarit vit desormais DANS le depot ; sous
# --no-ref seuls ses parametres de duel comptent (drapeaux, LP, taille de main,
# deck adverse), sa ligne est ecartee — c'est pourquoi n'importe quel replay du
# meme duel fait l'affaire, y compris une approche produite par le solveur.
$gabarit = "D:\ProjectIgnis\replay2video\combosolver\gabarits\etalon_a_lunalight.yrp"

$defs = @{
    # le temoin, RELANCE ici : les bras de 9.21 (d) tournaient sur l'ancien
    # gabarit vivant. Le duel est prouve identique (meme en-tete imprime), mais
    # un pilote auto-porte vaut mieux qu'un appariement a argumenter.
    'temoin' = @()
    # 1. le plafond de longueur, celui que les catalogues saturent
    'len16' = @('--options-len', '16')
    'len24' = @('--options-len', '24')
    # support relache : le bras a jouer SI len16/len24 ne rallongent rien
    'sup1'  = @('--options-len', '16', '--options-support', '1')
    # 3. la pompe a diversite composee a la forme GAGNANTE (jamais fait : le
    #    script s14_diversite_ab.ps1 l'a appariee a + ctx, ecrit avant que ctx
    #    ne perde son A/B)
    'lr4'   = @('--nrpa-lr', '4')
    # 4. le corpus vivant : plus de lignes, plus de lignes par worker
    'pool'  = @('--options-pool', '24', '--options-per-worker', '3')
    # 5. MCPS : le conditionnement PAR LE CHEMIN du niveau contextuel, au lieu
    #    du descripteur (posees, main) qui vaut (0,3) pour toutes les branches a
    #    la premiere decision. C'est le retour au critere du papier que le projet
    #    citait sans l'appliquer. k = profondeur au-dela de laquelle le chemin
    #    est fige (ce qui BORNE la table ; smoke : 207 cases a k=6).
    'mcps6'  = @('--mcps', '6')
    'mcps12' = @('--mcps', '12')
    # temoin du niveau contextuel SEUL (ancien conditionnement), pour separer
    # « allumer le 2e niveau » de « le conditionner par le chemin ».
    'ctx8'   = @('--ctx-shrink', '8')
}

function Invoke-Luna {
    param([string]$Out, [string]$Seed, [int]$Budget, [string[]]$Extra)
    Write-Output "--- $Out (graine $Seed)"
    & .\bin\Release\combosolver.exe $gabarit `
        --scriptdir "D:\ProjectIgnis\replay2video\deps\compat_2026-08" `
        --scriptdir ..\deps\scripts_2026-04-13\script `
        --scriptdir "$repo\delta-bagooska\script" `
        --scriptdir "$repo\delta-puppet\script" `
        --deck "D:\ProjectIgnis\deck\Lunalight.ydk" `
        --hand "57103969|57103969|57103969" `
        --no-ref `
        --target 54701958 --target 54701958 --target 54701958 `
        --target "90590304@DEF" `
        --max-decisions 700 `
        --resolve 4731783 --resolve 2344618 --resolve 47705572 `
        --summon-min "54701958:3" `
        --hint 35618217 --hint 24550676 --hint 100460013 --hint 24094653 `
        --hint 48444114 --hint 83190280 --hint 50277355 --hint 14152693 `
        --options-online $Period @Extra `
        --solve-ms $Budget --seed $Seed --finisher levin --archive-k 24 `
        --outdir $Out *> "$Out.log"
    Write-Output "    EXIT=$LASTEXITCODE"
}

foreach ($s in $Seeds) {
    foreach ($b in $Bras) {
        if (-not $defs.ContainsKey($b)) { Write-Output "!! bras inconnu : $b"; continue }
        Invoke-Luna "s14_ms_${b}_$s" $s $Ms $defs[$b]
    }
}
Write-Output "TERMINE"
