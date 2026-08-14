# Session 7 — bras ADAPT de l'etalon 1 (escalade meme-deck enracinee sur la
# ligne 259). Le bras temoin est le run d'escalade de MEME graine (s7_esc<seed>) :
# meme binaire, meme commande, le mecanisme etant inerte sans --adapt.
param([string]$Seed = '888',
      [string]$Corpus = 'sF_final',
      [int]$Passes = 4,
      [int]$Ms = 600000)

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
$out = "s7_e1_adapt$Seed"
& .\bin\Release\combosolver.exe "D:\ProjectIgnis\replay\synchron handrip 2.yrpX" `
    --scriptdir ..\deps\scripts_2026-04-13\script `
    --start "D:\ProjectIgnis\replay\synchron handrip 2.yrpX" `
    --solve-ms $Ms --seed $Seed --finisher levin --optimize `
    --finisher-min 420000 --burn-limit 19 --archive-k 24 `
    --approach "sZ6_slack255/solution_00_b19_a55.yrp" `
    --adapt $Corpus --adapt-passes $Passes `
    --outdir $out `
    --guard "5:Crystal Wing|Zalen@terrain+Junk Signal@main" --guard-off "mainadv<=2" `
    --no-activate "Duel Evolution - Assault Zone" `
    --no-chain Zalen --no-chain "Crystal Wing" `
    --resolve "PSY-Framelord Omega@terrain:2" `
    --resolve "Trishula, Dragon of the Ice Barrier@terrain" 2>&1 |
    Tee-Object -FilePath "$out.log" |
    Select-String -Pattern 'accord du corpus|adaptation :|BUT|brulees' |
    Select-Object -Last 12
