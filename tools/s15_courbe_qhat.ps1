# COURBE DE DECOUVERTE DE Q^ — « et vite ? », la moitie de la question de
# l'operateur que la sonde de fin de run ne repondait PAS.
#
# La sonde etablit qu'en fin de run le classement est bon. Elle ne dit pas a
# quel budget il l'est devenu, ni combien de tirages le soutiennent. Deux axes,
# tous deux bon marche :
#
#   1. LE BUDGET : 10 / 20 / 40 s. Le classement lu en fin de run d'un budget
#      donne une BORNE SUPERIEURE du cout de la decouverte a ce budget.
#   2. LA FENETRE : W = 128 / 512 / 4096 a budget FIXE. C'est l'axe qui repond
#      vraiment — Q^ ne voit QUE les W derniers tirages par worker, donc si le
#      classement tient a W = 128 il est soutenu par 128 x 16 = 2048 tirages
#      recents, quel que soit le temps passe avant.
#
# LIRE : la table « PERMUTATION Q^({}, a) » de chaque log, et la position de
# Lunalight Gold Leo (code 26608347 dans la .cdb de la machine ; il est nomme
# dans la table). Le point de comparaison est connu et brutal : sans --qhat le
# solveur ne concentre JAMAIS, a aucun budget (9.21 (j)).
param([int[]]$Budgets = @(10, 20, 40),
      [int[]]$Fenetres = @(128, 512, 4096),
      [int]$BudgetFenetre = 40,
      [string]$Seed = '888')

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
$repo = "D:\ProjectIgnis\repositories"
$gabarit = "D:\ProjectIgnis\replay2video\combosolver\gabarits\etalon_a_lunalight.yrp"

function Invoke-Luna {
    param([string]$Out, [int]$Budget, [string[]]$Extra)
    Write-Output "--- $Out"
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
        --qhat 6 @Extra `
        --solve-ms ($Budget * 1000) --seed $Seed --archive-k 24 `
        --outdir $Out *> "$Out.log"
    Write-Output "    EXIT=$LASTEXITCODE"
}

foreach ($b in $Budgets) { Invoke-Luna "s15cb_t$b" $b @() }
foreach ($w in $Fenetres) {
    Invoke-Luna "s15cb_w$w" $BudgetFenetre @('--qhat-window', "$w")
}
Write-Output "TERMINE"
