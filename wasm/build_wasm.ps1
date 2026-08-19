# Build WebAssembly du combosolver.
#
# Ne remplace pas le build natif : il le DOUBLE. Les sources sont les memes,
# a l'octet — seul `arena.cpp` a un second dos, et le natif n'en sait rien.
#
# CONFIGURATION DE REFERENCE — celle qui a ete mesuree, et la seule a utiliser
# pour livrer. `-NoBarrier` n'est pas un repli : la barriere d'ecriture est
# REFUTEE (inutile a parite de debit, incompatible avec les threads).
#
#   .\wasm\build_wasm.ps1 -Target web -Threads 16 -MemoryMb 2048 -NoBarrier -Lto
#   .\wasm\build_wasm.ps1             -Threads 16 -MemoryMb 2048 -NoBarrier -Lto
#
# Variantes de diagnostic :
#   -Threads 1              juge deterministe (rejeu seul, +-1 %)
#   (sans -NoBarrier)       barriere d'ecriture + R2V_ARENA_VERIFY=1
#   -Parts cov,nobuiltin    bissection des leviers de la barriere
#   -Symbols                noms de fonctions dans les traces
#
# La cible `node` utilise NODERAWFS : le binaire wasm lit les VRAIS repertoires
# de scripts et les VRAIES bases de cartes, avec la MEME ligne de commande que
# le natif. C'est ce qui rend la comparaison possible sans rien maquiller.

param(
    [ValidateSet('node','web')] [string]$Target = 'node',
    [int]$Threads = 16,
    [switch]$NoBarrier,
    [switch]$Lto,
    [ValidateSet('dlmalloc','emmalloc','mimalloc')] [string]$Malloc = 'dlmalloc',
    [string]$ProfileGen = '',        # repertoire ou deposer les .profraw
    [string]$ProfileUse = '',        # .profdata a exploiter
    [switch]$Symbols,
    [switch]$NoWrap,
    [switch]$InertWrap,
    [string]$Parts = 'cov,nobuiltin,nobulk',   # bissection : sous-ensemble des leviers de la barriere
    [switch]$Clean,
    [int]$MemoryMb = 1536,
    [string]$Out = ''
)

$ErrorActionPreference = 'Stop'
$here    = Split-Path -Parent $PSScriptRoot           # ...\combosolver
$root    = Split-Path -Parent $here                   # ...\replay2video
$ocgdir  = Join-Path $root 'deps\ocgcore'
$luadir  = Join-Path $ocgdir 'lua'
$lzmadir = Join-Path $root 'edopro\gframe\lzma'
# sqlite3 : l'amalgamation, COPIEE dans wasm/sqlite. Le repertoire d'origine de
# vcpkg contient un fichier nomme `version` — un -I dessus le fait resoudre a la
# place de l'en-tete C++20 <version>, et libc++ ne compile plus.
$sqlite  = Join-Path $here 'wasm\sqlite'
# L'amalgamation est copiee a la demande : la garder dans le depot ajouterait
# 10 Mo pour un fichier que vcpkg a deja telecharge.
if (-not (Test-Path (Join-Path $sqlite 'sqlite3.c'))) {
    $src = Get-ChildItem (Join-Path $root 'vcpkg\buildtrees\sqlite3\src') -Recurse -Filter sqlite3.c -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $src) { throw "sqlite3.c introuvable sous vcpkg/buildtrees/sqlite3/src" }
    Copy-Item $src.FullName $sqlite; Copy-Item (Join-Path $src.DirectoryName 'sqlite3.h') $sqlite
    Write-Output "  sqlite3 amalgame copie depuis vcpkg"
}
$objdir  = Join-Path $here "wasm\obj\$Target$(if($NoBarrier){'-nobarrier'})$(if($Lto){'-lto'})$(if($Malloc -ne 'dlmalloc'){"-$Malloc"})$(if($ProfileGen){'-pgen'})$(if($ProfileUse){'-puse'})$(if($Threads -gt 1){"-t$Threads"})-$($Parts -replace ',','')"
if (-not $Out) { $Out = Join-Path $here "wasm\bin\combosolver$(if($NoBarrier){'_nobarrier'})$(if($Threads -gt 1){"_t$Threads"}).$(if($Target -eq 'web'){'html'}else{'js'})" }

if ($Clean -and (Test-Path $objdir)) { Remove-Item -Recurse -Force $objdir }
New-Item -ItemType Directory -Force -Path $objdir, (Split-Path $Out) | Out-Null

# --- environnement emsdk ---------------------------------------------------
$emsdk = Join-Path $root 'emsdk'
if (-not $env:EMSDK) { & (Join-Path $emsdk 'emsdk_env.ps1') | Out-Null }

# --- drapeaux communs ------------------------------------------------------
#
# -fwasm-exceptions : Lua est compile en C++, donc LUAI_THROW est un `throw`.
#   Les exceptions JS d'emscripten couteraient sur le chemin non-lancant ;
#   l'EH natif wasm ne coute rien tant que rien ne leve.
# -msimd128 : les comparaisons et les copies de l'arene en profitent.
# -flto : REFUTE avec la barriere, et pas par un raisonnement — par le
#   verificateur (R2V_ARENA_VERIFY=1 : « pages sales NON marquees : 2 »). En LTO,
#   wasm-ld resout et INLINE memcpy depuis le bitcode de la libc AVANT la
#   resolution de symbole, donc --wrap ne voit plus rien et les copies en bloc
#   echappent au marquage. `-Lto` reste disponible pour le bras temoin
#   (-NoBarrier), ou il n'y a rien a intercepter.
#   Cout de ce renoncement : ~13 % (le gain LTO mesure cote natif, s12).
$common = @(
    '-O3', '-fwasm-exceptions', '-msimd128',
    '-DNDEBUG', '-D_7ZIP_ST', '-fno-strict-aliasing'
)
if ($Lto)          { $common += '-flto' }
# PGO. Le natif y gagne 27,2 -> 18,6 us/appel ; rien ne dit que wasm y gagne
# autant, et c'est justement ce qu'on mesure. L'instrumente s'execute sous node
# (NODERAWFS ecrit le .profraw sur le vrai disque), puis llvm-profdata merge.
# -fprofile-update=single : avec des threads, les compteurs PGO s'incrementent
# par RMW atomique, et la section de compteurs n'est pas alignee — wasm refuse
# (« operation does not support unaligned accesses »). Des compteurs non
# atomiques perdent quelques increments sous course ; un profil s'en moque.
if ($ProfileGen) { $common += @('-fprofile-instr-generate', '-fprofile-update=single') }
if ($ProfileUse) { $common += @("-fprofile-instr-use=$ProfileUse", '-Wno-profile-instr-unprofiled',
                                '-Wno-profile-instr-out-of-date') }
if ($Threads -gt 1) { $common += '-pthread' }

$incl = @("-I$here", "-I$ocgdir", "-I$luadir", "-I$ocgdir\lua\src", "-I$lzmadir", "-I$sqlite")

# La barriere n'instrumente QUE ocgcore et Lua : 82 % du temps par decision y
# vit, et le solveur lui-meme n'ecrit jamais dans l'arene.
$barrierFlags = @()
if (-not $NoBarrier) {
    $part = $Parts -split ','
    $barrierFlags = @()
    if ($part -contains 'cov') { $barrierFlags += '-fsanitize-coverage=func,trace-stores' }
    # Les intrinseques memoire echappent a -fsanitize-coverage. On les force
    # a rester des APPELS pour que --wrap puisse les intercepter.
    if ($part -contains 'nobuiltin') {
        $barrierFlags += @('-fno-builtin-memcpy', '-fno-builtin-memmove', '-fno-builtin-memset')
    }
    # -mno-bulk-memory-opt : indispensable, c'est lui qui ramene les copies de
    # STRUCTURE a des APPELS a memcpy — donc a des ecritures interceptables.
    # Sans lui le verificateur compte 2 pages manquees par run (un tableau de
    # pointeurs deplace par `memory.copy`), c'est-a-dire de la corruption.
    #
    # Sa contrepartie : la negation est hierarchique dans LLVM, elle emporte
    # `bulk-memory`, et wasm-ld exige atomics+bulk-memory de chaque objet pour
    # accorder --shared-memory. On remet donc la mention APRES coup, dans les
    # metadonnees seulement (wasm/patch_features.py) : l'objet declare une
    # capacite qu'il n'emet pas, ce qui est exactement ce que la verification
    # cherche a etablir. `--no-check-features` a ete essaye et REFUTE : il ne
    # corrige rien, il masque le garde-fou, et le programme tombe ailleurs.
    if ($part -contains 'nobulk') { $barrierFlags += '-mno-bulk-memory-opt' }
} else {
    $common += '-DR2V_NO_BARRIER'
}
if ($InertWrap) { $common += '-DR2V_WRAP_INERT'; $barrierFlags += '-DR2V_WRAP_INERT' }

# --- listes de sources -----------------------------------------------------
$luaSkip = @('lbitlib.c','lcorolib.c','ldblib.c','linit.c','loadlib.c','loslib.c',
             'ltests.c','lua.c','luac.c','lutf8lib.c','onelua.c')
$luaSrc  = Get-ChildItem "$luadir\src\*.c" | Where-Object { $luaSkip -notcontains $_.Name }
$ocgSrc  = Get-ChildItem "$ocgdir\*.cpp"
$lzmaSrc = @('Alloc.c','LzFind.c','LzmaDec.c','LzmaEnc.c','LzmaLib.c') |
             ForEach-Object { Join-Path $lzmadir $_ }
$solverSrc = Get-ChildItem "$here\*.cpp"

$jobs = New-Object System.Collections.ArrayList
function Add-Job([string]$src, [string[]]$flags, [string]$tag) {
    $obj = Join-Path $objdir ("{0}_{1}.o" -f $tag, [IO.Path]::GetFileNameWithoutExtension($src))
    [void]$jobs.Add([pscustomobject]@{ Src = $src; Obj = $obj; Flags = $flags })
}

# Lua : compile en C++ (ocgcore l'appelle depuis du C++ et il utilise longjmp),
# force-include de luaconf-customize.h (accroche de l'arene + graine constante).
foreach ($f in $luaSrc) {
    Add-Job $f.FullName ($common + $incl + $barrierFlags +
        @('-x','c++','-std=c++17','-include','luaconf-customize.h','-w')) 'lua'
}
foreach ($f in $ocgSrc) {
    Add-Job $f.FullName ($common + $incl + $barrierFlags +
        @('-std=c++17','-fno-rtti','-w')) 'ocg'
}
foreach ($f in $lzmaSrc) { Add-Job $f ($common + @("-I$lzmadir", '-w')) 'lzma' }
# Le solveur est instrumente LUI AUSSI, et pour une raison qui n'etait pas
# evidente : l'ALLOCATEUR DE L'ARENE ecrit dans l'arene (chainage des listes
# libres, en-tetes de spans). Ces ecritures-la sont invisibles a une
# instrumentation limitee a ocgcore et Lua — le verificateur les a trouvees a la
# premiere restauration (« barriere INCOMPLETE : page 144 »). Les fonctions de
# la barriere elle-meme portent no_sanitize("coverage").
foreach ($f in $solverSrc) {
    Add-Job $f.FullName ($common + $incl + $barrierFlags + @('-std=c++17')) 'solver'
}
Add-Job (Join-Path $sqlite 'sqlite3.c') ($common + @(
    '-DSQLITE_OMIT_LOAD_EXTENSION', '-DSQLITE_THREADSAFE=1',
    '-DSQLITE_OMIT_DEPRECATED', '-DSQLITE_DQS=0', '-w')) 'sqlite'

# --- compilation parallele -------------------------------------------------
Write-Output "compilation : $($jobs.Count) unites -> $objdir"
$sw = [Diagnostics.Stopwatch]::StartNew()
# EMPREINTE DES DRAPEAUX. Le cache incremental ne comparait que les dates : un
# drapeau change ne recompilait rien, et la mesure suivante portait sur l'ANCIEN
# binaire. Piege paye une fois (-fprofile-update=single), plus jamais.
$stamp = Join-Path $objdir '.flags'
$sig = ($jobs | ForEach-Object { $_.Flags -join ' ' }) -join "`n"
if ((Test-Path $stamp) -and ((Get-Content $stamp -Raw) -ne $sig)) {
    Write-Output "  drapeaux modifies -> recompilation complete"
    Get-ChildItem $objdir -Filter *.o | Remove-Item -Force
}
Set-Content $stamp $sig -NoNewline

$results = $jobs | ForEach-Object -ThrottleLimit ([Environment]::ProcessorCount) -Parallel {
    $j = $_
    if ((Test-Path $j.Obj) -and
        ((Get-Item $j.Obj).LastWriteTime -gt (Get-Item $j.Src).LastWriteTime)) {
        return [pscustomobject]@{ Src = $j.Src; Ok = $true; Log = '' }
    }
    $out = & emcc @($j.Flags) '-c' $j.Src '-o' $j.Obj 2>&1
    [pscustomobject]@{ Src = $j.Src; Ok = ($LASTEXITCODE -eq 0); Log = ($out -join "`n") }
}
$bad = @($results | Where-Object { -not $_.Ok })
foreach ($b in $bad) {
    Write-Output "ECHEC $($b.Src)"
    ($b.Log -split "`n") | Select-Object -First 20 | ForEach-Object { Write-Output "    $_" }
}
if ($bad.Count) { throw "$($bad.Count) unite(s) en echec" }
Write-Output "  compile en $([int]$sw.Elapsed.TotalSeconds) s"

# --- remise des features dans les metadonnees ------------------------------
if (-not $NoBarrier -and $Threads -gt 1 -and ($Parts -split ',') -contains 'nobulk') {
    $touched = $jobs | Where-Object { $_.Flags -contains '-mno-bulk-memory-opt' } |
                 ForEach-Object { $_.Obj }
    $rep = & python (Join-Path $PSScriptRoot 'patch_features.py') 'atomics,bulk-memory' 'shared-mem' @touched
    $n = @($rep | Where-Object { $_ -match 'ajoute|retire' }).Count
    Write-Output "  features rapiecees sur $n objet(s) (+bulk-memory, -shared-mem retire)"
}

# --- edition de liens ------------------------------------------------------
$link = @('-O3', '-fwasm-exceptions', '-msimd128')
# mimalloc apporte SES PROPRES operator new/delete, qui entrent en collision
# avec ceux d'arena.cpp — or ce sont eux qui routent vers l'arene, ils doivent
# gagner. wasm-ld retient la PREMIERE definition, et nos objets passent avant la
# bibliotheque : autoriser la double definition donne donc le bon vainqueur.
if ($Malloc -ne 'dlmalloc') { $link += @("-sMALLOC=$Malloc", '-Wl,--allow-multiple-definition') }
if ($ProfileGen) { $link += '-fprofile-instr-generate' }
if ($ProfileUse) { $link += "-fprofile-instr-use=$ProfileUse" }
if ($Lto) { $link += '-flto' }
if ($Symbols) { $link += @('-g2','-sASSERTIONS=1') }
if (-not $NoBarrier -and ($Parts -split ',') -contains 'nobuiltin') { $link += @('-fno-builtin-memcpy','-fno-builtin-memmove','-fno-builtin-memset') }
if (-not $NoBarrier -and ($Parts -split ',') -contains 'nobulk') { $link += '-mno-bulk-memory-opt' }
if ($Threads -gt 1) {
    # PROXY_TO_PTHREAD : main() part sur un pthread, donc ses join() bloquants
    # redeviennent legaux (sur le thread du navigateur ils sont interdits).
    # DEFAULT_PTHREAD_STACK_SIZE : sous PROXY_TO_PTHREAD, main() TOURNE sur un
    # pthread — il herite donc de cette taille et NON de -sSTACK_SIZE. Le defaut
    # (64 Ko) deborde des l'initialisation ; le debordement se presente comme un
    # « memory access out of bounds », pas comme une pile pleine.
    $link += @('-pthread', "-sPTHREAD_POOL_SIZE=$($Threads + 2)", '-sPROXY_TO_PTHREAD',
               '-sDEFAULT_PTHREAD_STACK_SIZE=8MB')
}
# MEMOIRE CROISSANTE. Elle etait interdite « pour que la base de l'arene ne
# bouge pas » — c'etait une precaution de trop : `memory.grow` ETEND la memoire
# lineaire par la fin, les adresses deja servies ne bougent jamais, et
# l'invariant de l'arene est intact. Le figeage, lui, coutait : a 2 Go un run
# sur deux tombait en `Aborted(OOM)` quand l'archive grossissait, et a 3 Go le
# navigateur reservait tout d'entree. On demarre donc petit et on grandit.
$link += @(
    "-sINITIAL_MEMORY=$($MemoryMb * 1MB)",
    '-sALLOW_MEMORY_GROWTH=1',
    '-sMAXIMUM_MEMORY=4294967296',
    '-sSTACK_SIZE=8MB',           # les coroutines Lua descendent profond
    '-sEXIT_RUNTIME=1',
    "-sENVIRONMENT=$(if ($Target -eq 'web') { 'web,worker' } else { 'node' })"
)
if (-not $NoBarrier) {
    if (-not $NoWrap -and ($Parts -split ',') -contains 'nobuiltin') { $link += @('-Wl,--wrap=memcpy', '-Wl,--wrap=memmove', '-Wl,--wrap=memset') }
}
if ($Target -eq 'node') {
    # Le systeme de fichiers REEL : meme ligne de commande que le natif, aucun
    # empaquetage, aucune divergence d'assets a expliquer dans la mesure.
    $link += '-sNODERAWFS=1'
} else {
    # Les assets sont EMPAQUETES dans le module : 3757 scripts Lua et les bases
    # de cartes. C'est le seul point ou la cible web differe vraiment de la
    # cible node — le solveur, lui, ne sait pas qu'il lit un MEMFS.
    # Les assets, rassembles par wasm/assets.ps1 hors de l'installation EDOPro.
    # DEUX pieges y sont deja desamorces, et aucun des deux ne ressemble a un
    # probleme de portage :
    #   - n'embarquer que expansions/*.cdb (7 bases) au lieu des 34 fait
    #     manquer des cartes, et le run s'arrete sur « aucune carte ne contient
    #     Zalen » ;
    #   - n'embarquer que le jeu de scripts EPINGLE laisse 29 scripts
    #     introuvables, et le rejeu DIVERGE en silence (253 MSG_RETRY). Les
    #     scripts sont donc aplatis en UN dossier, dans l'ordre de priorite du
    #     natif : installation, puis expansions, puis le jeu epingle par-dessus.
    $assets = Join-Path $here 'wasm\assets'
    $link += @('-sFORCE_FILESYSTEM=1', '-sMODULARIZE=1',
               '-sEXPORT_NAME=createSolver', '-sINVOKE_RUN=0', '-sEXIT_RUNTIME=1',
               "-sEXPORTED_RUNTIME_METHODS=['callMain','FS']",
               "--preload-file", "$assets\scripts@/scripts",
               "--preload-file", "$assets\edopro@/edopro")
}

Write-Output "edition de liens -> $Out"
$objs = $jobs | ForEach-Object { $_.Obj }
# em++ et non emcc : l'edition de liens doit tirer libc++ et l'ABI C++.
& em++ @link @objs -o $Out
if ($LASTEXITCODE -ne 0) { throw "edition de liens en echec" }

$wasm = [IO.Path]::ChangeExtension($Out, '.wasm')

# --- precompression brotli (cible web) -------------------------------------
# 12,18 Mo en gzip contre 5,56 Mo en brotli : les 22 302 scripts Lua se
# ressemblent, et la fenetre de brotli l'exploite. On precompresse au build,
# jamais a la volee.
if ($Target -eq 'web') {
    # brotli vit souvent hors du PATH de PowerShell (msys2, git-for-windows).
    $br = Get-Command brotli -ErrorAction SilentlyContinue
    if (-not $br) {
        # Git for Windows en embarque un : on le deduit de l'emplacement de git
        # plutot que de coder un chemin en dur (il n'est pas toujours sur C:).
        $g = Get-Command git -ErrorAction SilentlyContinue
        $cands = @('C:\msys64\mingw64\bin\brotli.exe')
        if ($g) { $cands += (Join-Path (Split-Path (Split-Path $g.Source)) 'mingw64\bin\brotli.exe') }
        foreach ($c in $cands) { if (Test-Path $c) { $br = $c; break } }
    }
    if ($br) {
        foreach ($f in @($Out, $wasm, [IO.Path]::ChangeExtension($Out, '.data'))) {
            if (Test-Path $f) { & $br -q 11 -f -o "$f.br" $f 2>$null }
        }
        $tot = (Get-ChildItem (Split-Path $Out) -Filter *.br |
                Measure-Object -Property Length -Sum).Sum
        Write-Output ("  brotli : {0:N2} Mo servis" -f ($tot / 1MB))
    } else {
        Write-Output "  (brotli absent : la page sera servie non precompressee)"
    }
}

# --- abaissement des operations en bloc ------------------------------------
# Referme le dernier trou de la barriere : les copies de structure qu'emet
# clang en `memory.copy` deviennent des appels a __memory_copy/__memory_fill,
# que arena.cpp definit et marque.
if (-not $NoBarrier -and -not $NoWrap -and (Test-Path $wasm)) {
    $wopt = Join-Path $env:EMSDK 'upstream/bin/wasm-opt.exe'
    $feat = @('--enable-threads','--enable-bulk-memory','--enable-simd',
              '--enable-exception-handling','--enable-sign-ext',
              '--enable-mutable-globals','--enable-nontrapping-float-to-int',
              '--enable-reference-types','--enable-multivalue')
    & $wopt $wasm -o "$wasm.tmp" --llvm-memory-copy-fill-lowering @feat
    if ($LASTEXITCODE -eq 0) {
        Move-Item -Force "$wasm.tmp" $wasm
        Write-Output "  memory.copy/fill abaisses en appels marques"
    } else {
        # Cause connue et structurelle : « memory.copy lowering should only be
        # run on modules with no passive segments » — or --shared-memory les
        # impose. Le trou des copies en bloc reste donc ouvert en threade.
        Write-Output "  wasm-opt refuse (segments passifs) : trou des copies en bloc ouvert"
        Remove-Item -Force "$wasm.tmp" -ErrorAction SilentlyContinue
    }
}
Write-Output "OK   $Out"
if (Test-Path $wasm) {
    Write-Output ("     {0} : {1:N2} Mo" -f (Split-Path $wasm -Leaf), ((Get-Item $wasm).Length / 1MB))
}

