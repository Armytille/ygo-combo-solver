# LA POMPE A DIVERSITE — repetitions limitees (session 14, chantier 2)
#
# GNRPA-LR (arXiv:2401.10420) : un niveau s'arrete apres R re-trouvailles de sa
# meilleure sequence, au lieu d'attendre les 8 iterations de stagnation. But :
# casser l'effondrement de l'adaptation dans un seul bassin — notre cause
# probable de la conversion STOCHASTIQUE (9.20 (f) : ~1 run arme sur 3-5).
#
# CE QUI EXISTE DEJA : le mecanisme est implemente depuis la session 4 sous
# `--nrpa-lr`, et il y a ete MESURE PERDANT — mais a R=2, sur la transplantation
# `test 4`, en 90 s, et tous niveaux confondus (8/8 -> 7/8). La note de l'epoque
# demandait explicitement une re-mesure « R plus grand ». C'est celle-ci, dans
# le regime de la mission : etalon A but seul, une seule traite, R=4 (strictement
# entre « tout de suite » et la stagnation a 8).
#
# ATTRIBUTION SEPAREE (l'exigence du prompt) : les deux bras temoins ne sont pas
# relances — ce sont ceux de tools/s14_options_online_ab.ps1, meme binaire,
# memes graines, meme budget. On ne mesure ici que les DEUX bras neufs :
#   nu    + lr4  contre s14_on_nu_<graine>
#   onctx + lr4  contre s14_on_onctx_<graine>
# La difference des deux differences dit si la diversite et le minage en ligne
# se composent ou se marchent dessus.
param([int]$Ms = 300000, [int]$Period = 60, [int]$R = 4,
      [string[]]$Seeds = @('888','1234','4242'))

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
$repo = "D:\ProjectIgnis\repositories"
$gabarit = "D:\ProjectIgnis\replay\_LastReplay.yrpX"

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
        @Extra `
        --solve-ms $Budget --seed $Seed --finisher levin --archive-k 24 `
        --outdir $Out *> "$Out.log"
    Write-Output "    EXIT=$LASTEXITCODE"
}

foreach ($s in $Seeds) {
    Invoke-Luna "s14_lr_nu_$s"    $s $Ms @('--nrpa-lr', "$R")
    Invoke-Luna "s14_lr_onctx_$s" $s $Ms @('--options-online', "$Period",
                                           '--options-ctx', '1',
                                           '--nrpa-lr', "$R")
}
Write-Output "TERMINE"
