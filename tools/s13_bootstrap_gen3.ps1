# GEN3 DU BOOTSTRAP (session 13) — l'iteration continue-t-elle de monter ?
#
# La gen2 armee de corpus1 a converti 1/4 -> 2/4 (graine 4242, et les bras de
# mesure cumules convertissent sur 888 et 1234 — 9.20 (a)-(b)). La gen3 est
# armee de la forme GAGNANTE : le corpus CUMULE (corpus1 + corpus2). Memes
# graines et meme budget que gen1/gen2 pour l'appariement strict.
#
# LIRE : best k/4 (une graine passe-t-elle a 3/4 ? le 2/4 devient-il
# routinier ?), >=k resolutions, triptyque des options. corpus3 = les
# approches ecrites (matiere de l'iteration suivante).
param([int]$GenMs = 240000)

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

foreach ($s in @('888', '1234', '4242')) {
    Invoke-Luna "s13_boot_gen3_$s" $s $GenMs @(
        '--adapt', 's12_boot_corpus', '--adapt', 's13_boot_corpus2',
        '--adapt-passes', '4', '--options', '256')
}
New-Item -ItemType Directory -Force s13_boot_corpus3 | Out-Null
Remove-Item s13_boot_corpus3\*.yrp -Force -ErrorAction SilentlyContinue
foreach ($s in @('888', '1234', '4242')) {
    Get-ChildItem "s13_boot_gen3_$s\*.yrp" -ErrorAction SilentlyContinue | ForEach-Object {
        Copy-Item $_.FullName "s13_boot_corpus3\g${s}_$($_.Name)"
    }
}
$n = (Get-ChildItem s13_boot_corpus3\*.yrp -ErrorAction SilentlyContinue).Count
Write-Output "corpus3 : $n ligne(s)"
Write-Output "TERMINE"
