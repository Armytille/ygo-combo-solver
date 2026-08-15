# BOOTSTRAP DES OPTIONS SUR L'ETALON A (session 12, suite) — LE test de
# decouverte, par opposition au test d'EXPLOITATION de l'etalon B (9.19 (g) :
# la-bas, 16/17 lignes du corpus etaient de la quasi-reference).
#
# Ici, AUCUNE reference n'existe (--no-ref, --target pose le but) : le corpus
# est fabrique par le solveur LUI-MEME — les best_approach_*.yrp de trois runs
# but seul a graines distinctes — puis on mesure si adapter dessus et en miner
# des macros ameliore la recherche. Trois bras pour l'ATTRIBUTION :
#   temoin     : rien
#   adapt      : --adapt corpus (l'adaptation seule)
#   adapt+opt  : --adapt corpus --options 256 (la selection par perte de Levin)
#
# LIRE : best k/4 (3x Liger + Bagooska), l'histogramme >=k resolutions, le
# triptyque des options (prises/absorbees/avortees), et la taille du catalogue
# retenu. Le compteur de tirages est declasse.
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

# 1. GENERATION du corpus propre : trois graines, les approches ecrites sont
#    la decouverte du solveur, sans reference.
foreach ($s in @('888', '1234', '4242')) {
    Invoke-Luna "s12_boot_gen_$s" $s $GenMs @()
}
New-Item -ItemType Directory -Force s12_boot_corpus | Out-Null
Remove-Item s12_boot_corpus\*.yrp -Force -ErrorAction SilentlyContinue
foreach ($s in @('888', '1234', '4242')) {
    Get-ChildItem "s12_boot_gen_$s\*.yrp" -ErrorAction SilentlyContinue | ForEach-Object {
        Copy-Item $_.FullName "s12_boot_corpus\g${s}_$($_.Name)"
    }
}
$n = (Get-ChildItem s12_boot_corpus\*.yrp -ErrorAction SilentlyContinue).Count
Write-Output "corpus propre : $n ligne(s)"

# 2. A/B a trois bras, deux graines de mesure (differentes entre elles ; 888
#    recoupe une graine de generation — assume, la generation ne fige pas les
#    tirages de la mesure, seule l'adaptation initiale change).
foreach ($s in @('888', '1234')) {
    Invoke-Luna "s12_boot_temoin_$s" $s $AbMs @()
    Invoke-Luna "s12_boot_adapt_$s"  $s $AbMs @('--adapt', 's12_boot_corpus', '--adapt-passes', '4')
    Invoke-Luna "s12_boot_opt_$s"    $s $AbMs @('--adapt', 's12_boot_corpus', '--adapt-passes', '4', '--options', '256')
}
Write-Output "TERMINE"
