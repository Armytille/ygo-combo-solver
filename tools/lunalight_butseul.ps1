# ETALON A en MODE BUT SEUL — Lunalight, sans replay de reference.
#
# CE QUI CHANGE PAR RAPPORT A tools/lunalight3.ps1, ET RIEN D'AUTRE
#
#  * --no-ref : _LastReplay.yrpX n'est plus qu'un GABARIT de duel (drapeaux,
#    LP, taille de main, deck adverse). Sa ligne, son board de fin de tour et
#    son REPERTOIRE sont tous ecartes. Le rapport imprime le gabarit pour que
#    ce soit verifiable et non promis.
#  * --target : le board cible est POSE, plus edite. lunalight3 partait de la
#    capture de la reference et la vidait par six --board-remove ; ces six
#    codes etaient de l'information sur ce que la reference avait pose.
#
# CE QUI RESTE, ET POURQUOI CE N'EST PAS DE LA REFERENCE DEGUISEE
#
#  --resolve / --summon-min / --hint / --max-decisions sont des CONNAISSANCES
#  DE DOMAINE ecrites a la main : « la ligne doit resoudre Masquerade deux
#  fois », « il faut trois Liger ». Au sens de Bonet & Geffner c'est un SKETCH,
#  pas une demonstration (cf. docs/etat-de-lart-but-seul.md, direction 3). Les
#  --hint restent la partie la plus discutable : ils ont ete choisis en
#  regardant le combo de reference. Le bras HINTLESS les retire pour chiffrer
#  ce qu'ils valent.
#
# LA VERITE TERRAIN, ET CE QU'IL FAUT BATTRE
#
#  Le deck atteint 3x Liger Dancer en 108 etapes (ygocombo #105) : la cible est
#  ATTEIGNABLE, c'est ce qui fait le prix de cet etalon.
#
#  Meilleur resultat connu, AVEC repertoire (s7_luna_v6, 1800 s, graine 888) :
#    tirages   1 246 s, 12,66 M tirages, best 1/4
#    finisseur best 1/4 (une racine monte a 2/4 en recul 40)
#    LDS a 0 ecart  2 038 555 etats, 294 s, best 2/4  <-- LE MEILLEUR
#    board atteint : 1 Liger + Bagooska ; MANQUE 2x Liger. 0 erreur de core.
#
#  Le meilleur venait donc de la passe qui suit LE PLAN. En --no-plan cette
#  passe devient structurellement VIDE (mesure sur l'etalon B : 26 etats a tous
#  les niveaux d'ecart) : le mode but seul perd exactement le mecanisme qui
#  avait produit le 2/4. C'est a chiffrer, pas a contourner.
#
#  Et le HER propositionnel (arXiv:2512.19355, domaine Delivery) predit que
#  « n exemplaires de la meme chose » est precisement la forme de but ou ces
#  methodes apprennent a en poser UN et s'y arretent.
param([int]$Ms = 1800000, [string]$Seed = '888', [string]$Bras = 'tous')

Set-Location "D:\ProjectIgnis\replay2video\combosolver"
$repo = "D:\ProjectIgnis\repositories"
$gabarit = "D:\ProjectIgnis\replay\_LastReplay.yrpX"

function Invoke-Bras([string]$nom, [string[]]$extra, [bool]$hints) {
    $out = "s8_A_$nom"
    Write-Output "--- etalon A bras $nom -> $out"
    $hintArgs = @()
    if ($hints) {
        $hintArgs = @('--hint','35618217','--hint','24550676','--hint','100460013',
                      '--hint','24094653','--hint','48444114','--hint','83190280',
                      '--hint','50277355','--hint','14152693')
    }
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
        @hintArgs @extra `
        --solve-ms $Ms --seed $Seed --finisher levin --archive-k 24 `
        --outdir $out *> "$out.log"
    Write-Output "    EXIT=$LASTEXITCODE"
}

if ($Bras -eq 'tous' -or $Bras -eq 'base')  { Invoke-Bras 'base'  @() $true }
if ($Bras -eq 'tous' -or $Bras -eq 'rh')    { Invoke-Bras 'rh15'  @('--reroot-h','15') $true }
Write-Output "TERMINE"
