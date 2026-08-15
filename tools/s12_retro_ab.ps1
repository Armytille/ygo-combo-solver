# LE VERDICT DU RETRO (session 12, suite) — la question laissee ouverte par le
# §9.17 (d) : le gradient de l'amorce (18 contre 4 sur la cible entiere) fait-il
# GAGNER quand il PESE dans le cout du finisseur (--levin-h, jamais --reroot-h) ?
#
# 2x2, parce que la forme du cout et le gradient se jugent ENSEMBLE : notre
# cout par defaut met h en facteur e^h (agressif), le canonique du papier en
# (d+h). Sur h plat les deux coincident (mesure, 9.19 (g)) ; sur le h amorce
# (5, 3, 18...) ils divergent massivement — c'est ICI que --phs-canonical se
# joue, pas sur l'etalon 0.
#
#   AN    h plat,   cout actuel   (temoin — la table de s10 l'a deja rendu)
#   ANC   h plat,   cout canonique
#   AP1   h amorce, cout actuel   (le bras jamais mesure de s10)
#   AP1C  h amorce, cout canonique
#
# Montage deterministe repris de tools/s10_amorce_etalon_a.ps1 (racines
# imposees --approach, --no-nrpa). LIRE : la table « distance amorcee » de
# l'en-tete (Liger > 1 sinon l'amorce n'a pas tourne), puis hR et best/cartes
# cumulees par racine, bras contre bras.
param([int]$Ms = 300000, [int]$FinMin = 260000, [string]$Seed = '888',
      [string]$Approche = 's7_luna_v6\best_approach_2of4.yrp')

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
$repo = "D:\ProjectIgnis\repositories"
$gabarit = "D:\ProjectIgnis\replay\_LastReplay.yrpX"

$bras = @(
    @{ nom = 'AN';   arg = @('--recipes', '0', '--no-seed-recipes') },
    @{ nom = 'ANC';  arg = @('--recipes', '0', '--no-seed-recipes', '--phs-canonical') },
    @{ nom = 'AP1';  arg = @('--recipes', '1') },
    @{ nom = 'AP1C'; arg = @('--recipes', '1', '--phs-canonical') }
)

foreach ($b in $bras) {
    $out = "s12_R_$($b.nom)"
    Write-Output "--- retro 2x2, bras $($b.nom) -> $out"
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
        --no-nrpa --approach $Approche --finisher-min $FinMin `
        --solve-ms $Ms --seed $Seed --finisher levin --archive-k 24 `
        @($b.arg) --outdir $out *> "$out.log"
    Write-Output "    EXIT=$LASTEXITCODE"
}
Write-Output "TERMINE"
