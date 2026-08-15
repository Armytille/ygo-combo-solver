# ITERATION DU BOOTSTRAP (session 13) — la gen2 est ARMEE des macros de la
# gen1. Question posee par 9.19 (j) : les approches ecrites par des runs armes
# font-elles un MEILLEUR corpus (macros minees dessus plus utiles) que les
# approches nues de la gen1 ?
#
# Protocole :
#   1. gen2 : MEMES graines et MEME budget que la gen1 (888/1234/4242, 240 s)
#      mais armees de --adapt s12_boot_corpus --options 256. Comparaison
#      appariee gen1/gen2 par graine (best k/4, >=k, approche ecrite).
#   2. corpus2 = les *.yrp ecrits par la gen2 (approches, et solutions si
#      conversion).
#   3. A/B trois bras, deux graines de mesure, bras intercales par graine :
#        opt1  : --adapt corpus1 --options 256   (temoin re-mesure, meme binaire)
#        opt2  : --adapt corpus2 --options 256   (l'iteration pure)
#        opt12 : --adapt corpus1 --adapt corpus2 --options 256  (cumul)
#
# LIRE : best k/4, la ligne "resolutions atteintes par tirage" (>=2, >=3),
# le triptyque des options, la taille du catalogue retenu et la perte modele.
# Le compteur de tirages est declasse.
param([int]$GenMs = 240000, [int]$AbMs = 300000)

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
$repo = "D:\ProjectIgnis\repositories"
$gabarit = "D:\ProjectIgnis\replay\_LastReplay.yrpX"

function Invoke-Luna {
    param([string]$Out, [string]$Seed, [int]$Ms, [string[]]$Extra)
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
        --solve-ms $Ms --seed $Seed --finisher levin --archive-k 24 `
        --outdir $Out *> "$Out.log"
    Write-Output "    EXIT=$LASTEXITCODE"
}

# 1. GEN2 armee : memes graines et budget que la gen1 (comparaison appariee).
foreach ($s in @('888', '1234', '4242')) {
    Invoke-Luna "s13_boot_gen2_$s" $s $GenMs @(
        '--adapt', 's12_boot_corpus', '--adapt-passes', '4', '--options', '256')
}
New-Item -ItemType Directory -Force s13_boot_corpus2 | Out-Null
Remove-Item s13_boot_corpus2\*.yrp -Force -ErrorAction SilentlyContinue
foreach ($s in @('888', '1234', '4242')) {
    Get-ChildItem "s13_boot_gen2_$s\*.yrp" -ErrorAction SilentlyContinue | ForEach-Object {
        Copy-Item $_.FullName "s13_boot_corpus2\g${s}_$($_.Name)"
    }
}
$n = (Get-ChildItem s13_boot_corpus2\*.yrp -ErrorAction SilentlyContinue).Count
Write-Output "corpus2 : $n ligne(s)"

# 2. A/B trois bras, bras intercales par graine.
foreach ($s in @('888', '1234')) {
    Invoke-Luna "s13_boot_opt1_$s" $s $AbMs @(
        '--adapt', 's12_boot_corpus', '--adapt-passes', '4', '--options', '256')
    Invoke-Luna "s13_boot_opt2_$s" $s $AbMs @(
        '--adapt', 's13_boot_corpus2', '--adapt-passes', '4', '--options', '256')
    Invoke-Luna "s13_boot_opt12_$s" $s $AbMs @(
        '--adapt', 's12_boot_corpus', '--adapt', 's13_boot_corpus2',
        '--adapt-passes', '4', '--options', '256')
}
Write-Output "TERMINE"
