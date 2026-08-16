# LA COURBE D'ACCORD SOUS LES DEUX CONDITIONNEMENTS — sans depenser un run
#
# CE QUE CA MESURE, et pourquoi c'est la bonne question. Le rapport `--adapt`
# imprime une grille passes x alpha de l'accord du corpus : moyenne geometrique
# de p(coup joue) et fraction ou le coup du corpus est classe premier. La
# « convergence » est la ligne a partir de laquelle la colonne cesse de monter.
# Ligne de base connue (9.14 / 9.19, ancien conditionnement) : 44 % a politique
# vierge, 65 % des la PREMIERE passe, palier a 66 % ; au classement 96 % contre
# un plafond calcule de 96-97 %.
#
# Donc le nombre de passes n'est pas le levier — l'ancien conditionnement
# converge deja en une passe. Le levier est LE PALIER, parce que c'est lui qui
# fixe la MASSE : 0,66^160 contre 0,80^160, ce sont quinze ordres de grandeur
# sur la probabilite d'une ligne complete (9.14 : « ce qui manque est la MASSE,
# pas la representation »).
#
# TROIS TABLES SONT IMPRIMEES, et la troisieme est le juge :
#   1. niveau contextuel ETEINT      -> le global seul
#   2. niveau contextuel ACTIF (k=1) -> le conditionnement COURANT, c'est-a-dire
#      (posees, main) sans --mcps, et LE CHEMIN avec
#   3. contexte = SIGNATURE du point de decision -> la MEMORISATION pure, donc
#      le PLAFOND que tout conditionnement peut esperer atteindre
# La lecture qui tranche : de combien MCPS ferme-t-il l'ecart entre (2) et (3) ?
#
# DETERMINISTE : rejeu de corpus, ni graine ni workers ni budget. Deux runs de
# quelques dizaines de secondes remplacent un A/B de 50 minutes pour CETTE
# question. C'est la sonde qui aurait du preceder l'A/B de la session.
param([string]$Corpus = 's13_boot_corpus2', [int]$Ms = 1000)

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
$repo = "D:\ProjectIgnis\repositories"
$gabarit = "D:\ProjectIgnis\replay2video\combosolver\gabarits\etalon_a_lunalight.yrp"

function Invoke-Sonde {
    param([string]$Out, [string[]]$Extra)
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
        --adapt $Corpus --adapt-passes 4 `
        @Extra `
        --solve-ms $Ms --seed 888 --outdir $Out *> "$Out.log"
    Write-Output "    EXIT=$LASTEXITCODE"
}

Invoke-Sonde "s14_ac_ancien" @()
Invoke-Sonde "s14_ac_mcps6"  @('--mcps', '6')
Invoke-Sonde "s14_ac_mcps12" @('--mcps', '12')

foreach ($f in @('s14_ac_ancien', 's14_ac_mcps6', 's14_ac_mcps12')) {
    Write-Output ""
    Write-Output "=============== $f"
    $t = Get-Content "$f.log"
    $i = ($t | Select-String -Pattern "accord du corpus").LineNumber
    foreach ($n in $i) { $t[($n - 1)..($n + 7)] }
}
Write-Output ""
Write-Output "LIRE : le PALIER de chaque colonne (pas le nombre de passes — il"
Write-Output "       converge deja en une), et l'ecart entre la table 2 et la 3."
