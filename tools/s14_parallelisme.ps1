# DEUX PROCESSUS A 8 THREADS CONTRE UN A 16 — la question jamais posee
#
# §9.19 (h) a mesure le cadran des workers DANS UN PROCESSUS : 8 -> 19,4 M
# appels/60 s, 16 -> 23,9 M, 24 -> 19,7 M (sursouscription destructrice). La
# conclusion tiree etait « le mur est la bande passante ». Mais personne n'a
# jamais mesure DEUX PROCESSUS A 8 THREADS, et les deux lectures de §9.19 (h)
# sont compatibles avec les deux issues :
#
#   * si le mur est vraiment la bande passante memoire, les deux processus se
#     partagent le meme plafond (~24 M) et on ne gagne RIEN ;
#   * s'il est en partie de la contention INTRA-PROCESSUS (les mutex
#     NrpaShared et OnlineOptions, la borne brulees partagee, l'allocateur),
#     deux processus approchent 2 x 19,4 = 38,8 M, soit ~1,6x de debit agrege —
#     et tous les A/B futurs se font en deux fois moins de temps.
#
# On ne peut pas trancher par le raisonnement. Dix minutes de mesure le font.
#
# JUGE : `Process (core)` de la phase TIRAGES sous --profile — appels TOTAUX
# (sommes sur les processus du bras) et us/appel. Le compteur de tirages reste
# declasse (9.19 (a)) ; ici on lit des appels au core sous la meme sonde des
# deux cotes, ce qui est precisement l'instrument que 9.19 (a) designe comme le
# remplacant valide.
#
# RESERVE A ECRIRE AVANT DE MESURER : si le bras 2x8 gagne, l'adopter est un
# RE-CALIBRAGE de la ligne de base — tous les bras d'un A/B devront partager le
# reglage, et la comparabilite avec l'historique a 16 threads sera rompue.
param([int]$Ms = 120000, [string]$Seed = '888')

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
$repo = "D:\ProjectIgnis\repositories"
$gabarit = "D:\ProjectIgnis\replay2video\combosolver\gabarits\etalon_a_lunalight.yrp"

function Start-Luna {
    param([string]$Out, [string]$S, [int]$Threads)
    $args = @(
        $gabarit,
        '--scriptdir', "D:\ProjectIgnis\replay2video\deps\compat_2026-08",
        '--scriptdir', '..\deps\scripts_2026-04-13\script',
        '--scriptdir', "$repo\delta-bagooska\script",
        '--scriptdir', "$repo\delta-puppet\script",
        '--deck', "D:\ProjectIgnis\deck\Lunalight.ydk",
        '--hand', '57103969|57103969|57103969',
        '--no-ref',
        '--target', '54701958', '--target', '54701958', '--target', '54701958',
        '--target', '90590304@DEF',
        '--max-decisions', '700',
        '--resolve', '4731783', '--resolve', '2344618', '--resolve', '47705572',
        '--summon-min', '54701958:3',
        '--threads', "$Threads", '--profile',
        '--solve-ms', "$Ms", '--seed', $S, '--finisher', 'levin',
        '--archive-k', '24', '--outdir', $Out)
    Start-Process -FilePath ".\bin\Release\combosolver.exe" -ArgumentList $args `
        -RedirectStandardOutput "$Out.log" -RedirectStandardError "$Out.err" `
        -NoNewWindow -PassThru
}

Write-Output "--- bras A : UN processus a 16 threads"
$a = Start-Luna "s14_par_A16" $Seed 16
$a | Wait-Process
Write-Output "    fait"

Write-Output "--- bras B : DEUX processus a 8 threads, simultanes"
$b1 = Start-Luna "s14_par_B8a" $Seed 8
$b2 = Start-Luna "s14_par_B8b" ([string]([int]$Seed + 1)) 8
$b1, $b2 | Wait-Process
Write-Output "    fait"

Write-Output ""
Write-Output "=== Process (core), phase TIRAGES ==="
foreach ($f in @('s14_par_A16', 's14_par_B8a', 's14_par_B8b')) {
    $t = Get-Content "$f.log" -ErrorAction SilentlyContinue
    $i = ($t | Select-String -Pattern '--- profil \[tirages\]').LineNumber
    if (-not $i) { Write-Output ("{0,-14} (pas de profil tirages)" -f $f); continue }
    $line = $t[$i..($i + 3)] | Where-Object { $_ -match 'Process \(core\)' } | Select-Object -First 1
    if ($line -match 'Process \(core\)\s+(\d+)\s+[\d.]+\s+[\d.]+ s\s+([\d.]+)') {
        Write-Output ("{0,-14} appels {1,12}   us/appel {2}" -f $f, $Matches[1], $Matches[2])
    } else {
        Write-Output ("{0,-14} {1}" -f $f, $line)
    }
}
Write-Output "LIRE : appels(B8a) + appels(B8b) contre appels(A16) — a temps de"
Write-Output "       paroi EGAL, c'est le debit agrege. Et us/appel des deux cotes :"
Write-Output "       s'il explose en B, la contention memoire a mange le gain."
