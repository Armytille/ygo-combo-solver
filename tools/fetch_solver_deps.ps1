<#
.SYNOPSIS
  Extrait la version d'ocgcore (et de son Lua) requise par le combo solver.

.DESCRIPTION
  Le solveur doit tourner sur un core CONTEMPORAIN du replay analyse : une
  version decalee ne plante pas, elle diverge en silence (cf.
  docs/combo-solver-design.md section 6bis). Le pin du sous-module dans
  edopro/ est fige au 2025-04-17 et ne convient pas aux replays recents.

  Ce script extrait un commit precis d'ocgcore, plus le commit correspondant
  de son sous-module lua/src, vers deps/ocgcore/. Il n'ecrit jamais dans
  l'installation EDOPro ni dans le clone edopro/ (hors git fetch).

.EXAMPLE
  .\tools\fetch_solver_deps.ps1
  .\tools\fetch_solver_deps.ps1 -Commit 8e5f4e4f0ab6b8ca750e8e1c91c1a58f407e3272
#>
param(
    # 2026-04-07 : dernier commit avant le replay de reference du 2026-04-13.
    [string]$Commit = "8e5f4e4f0ab6b8ca750e8e1c91c1a58f407e3272",
    [string]$Dest = "deps/ocgcore",
    # Scripts de cartes a la date du replay de reference. Un jeu decale fait
    # diverger le rejeu aussi surement qu'un core decale, et sans le dire.
    [string]$ScriptsCommit = "0e90a3e8",
    [string]$ScriptsDate = "2026-04-13"
)

$ErrorActionPreference = "Stop"
# Le depot du solveur est imbrique dans l'arborescence replay2video : c'est de
# LA que viennent le clone edopro/ (source d'ocgcore et des sources LZMA) et le
# dossier deps/ ou tout est extrait. Ces deux chemins sont les seules
# dependances externes du projet ; premake les vise pareillement en `../`.
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$src = Join-Path $root "edopro/ocgcore"
$dst = Join-Path $root $Dest

if (-not (Test-Path (Join-Path $src ".git"))) {
    throw "edopro/ocgcore introuvable. Lancer build_windows.bat d'abord."
}

# Le fetch n'est utile que si le commit manque localement : une coupure reseau
# ne doit pas empecher de reconstruire a partir d'objets deja presents.
$haveCommit = $(git -C $src cat-file -t $Commit 2>$null) -eq "commit"
if (-not $haveCommit) {
    Write-Host "recuperation des objets ocgcore..."
    git -C $src fetch --quiet origin
    if ($LASTEXITCODE -ne 0) { throw "git fetch a echoue dans $src et le commit $Commit est absent localement" }
} else {
    Write-Host "objets ocgcore deja presents localement"
}

# Le pin de lua/src est porte par l'arbre du commit cible, pas par le HEAD.
$luaLine = git -C $src ls-tree $Commit lua/src
if (-not $luaLine) { throw "commit $Commit introuvable ou sans lua/src" }
$luaCommit = ($luaLine -split '\s+')[2]
Write-Host "  ocgcore : $Commit"
Write-Host "  lua/src : $luaCommit"

if (Test-Path $dst) { Remove-Item $dst -Recurse -Force -Confirm:$false }
New-Item -ItemType Directory -Force -Path $dst | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $dst "lua/src") | Out-Null

Write-Host "extraction d'ocgcore..."
$tar = Join-Path $env:TEMP "ocgcore-$($Commit.Substring(0,8)).tar"
git -C $src archive --output=$tar $Commit
tar -x -f $tar -C $dst
Remove-Item $tar -Confirm:$false

Write-Host "extraction de lua..."
$luaSrc = Join-Path $src "lua/src"
if ($(git -C $luaSrc cat-file -t $luaCommit 2>$null) -ne "commit") {
    git -C $luaSrc fetch --quiet origin
    if ($LASTEXITCODE -ne 0) { throw "git fetch a echoue dans $luaSrc et le commit $luaCommit est absent localement" }
}
$tar = Join-Path $env:TEMP "lua-$($luaCommit.Substring(0,8)).tar"
git -C $luaSrc archive --output=$tar $luaCommit
tar -x -f $tar -C (Join-Path $dst "lua/src")
Remove-Item $tar -Confirm:$false

# --- patchs pour l'instantane memoire ---------------------------------------
# Deux modifications, toutes deux dans Lua : le core C++ n'est PAS touche, ses
# allocations sont captees par la surcharge globale d'operator new cote solveur.
# Voir docs/combo-solver-design.md section 5.
Write-Host "application des patchs d'arene..."

function Edit-File([string]$path, [string]$anchor, [string]$replacement, [string]$label) {
    $text = [IO.File]::ReadAllText($path)
    if ($text.Contains($replacement)) { Write-Host "  $label : deja applique"; return }
    if (-not $text.Contains($anchor)) {
        throw "patch '$label' : ancre introuvable dans $path. La version d'ocgcore a change, le patch doit etre revu."
    }
    [IO.File]::WriteAllText($path, $text.Replace($anchor, $replacement))
    Write-Host "  $label : ok"
}

# 1. luaconf-customize.h est force-include dans chaque .c de Lua (lua/premake5.lua).
#    On y fixe la graine de hachage (sinon elle derive d'une adresse de pile et de
#    l'horloge : deux runs identiques divergent) et on y declare le point
#    d'accroche de l'allocateur d'arene.
$hookDecl = @'
/* ---- combosolver : instantane memoire du duel ---------------------------- */
/* Graine de hachage des chaines constante : rend le core reproductible d'un
   run a l'autre. Sans cela, luai_makeseed melange une adresse de pile et
   l'horloge (lstate.c). Le seed ne sert qu'a la protection anti-collision. */
#define luai_makeseed(L) ((void)(L), 0u)

/* Allocateur fourni par l'hote. Laisse nul, Lua utilise realloc/free. Quand il
   est renseigne, TOUT le heap Lua (tables, closures, upvalues, coroutines
   suspendues) vit dans l'arene et devient instantanable. */
#include <stddef.h>
#if defined(__cplusplus)
extern "C" {
#endif
/* Propres au thread : chaque worker a sa propre arene, et l'etat Lua qu'il cree
   doit y puiser. Un pointeur global les ferait se melanger. */
typedef void* (*combosolver_alloc_fn)(void* ud, void* ptr, size_t osize, size_t nsize);
extern thread_local combosolver_alloc_fn combosolver_lua_alloc;
extern thread_local void* combosolver_lua_alloc_ud;
/* Etat Lua du dernier duel cree sur ce thread. Le core ne l'expose pas, et il
   faut y acceder pour arreter le ramasse-miettes : son marquage ecrit dans
   l'en-tete de TOUS les objets vivants, ce qui salit la quasi-totalite des
   pages et ruine l'interet d'une restauration incrementale. Sans GC, la
   memoire n'est pas perdue : une restauration recupere tout ce qu'une branche
   abandonnee a alloue. */
extern thread_local void* combosolver_lua_state;
#if defined(__cplusplus)
}
#endif
/* -------------------------------------------------------------------------- */

'@
$customize = Join-Path $dst "lua/luaconf-customize.h"
Edit-File $customize `
    '#if defined(LUA_EPRO_APICHECK)' `
    ($hookDecl + '#if defined(LUA_EPRO_APICHECK)') `
    "luaconf-customize.h (graine + accroche allocateur)"

# 2. luaL_newstate est le seul endroit ou le core cree son lua_State
#    (interpreter.cpp appelle luaL_newstate()). On y branche l'allocateur sans
#    toucher a ocgcore lui-meme, ce qui garde atpanic et setwarnf intacts.
$lauxlib = Join-Path $dst "lua/src/lauxlib.c"
Edit-File $lauxlib `
    '  lua_State *L = lua_newstate(l_alloc, NULL);' `
    @'
  lua_State *L = combosolver_lua_alloc
                   ? lua_newstate((lua_Alloc)combosolver_lua_alloc,
                                  combosolver_lua_alloc_ud)
                   : lua_newstate(l_alloc, NULL);
  combosolver_lua_state = L;
'@ `
    "lauxlib.c (luaL_newstate -> allocateur d'arene + export de l'etat)"

# 3. Etat du processeur. L'API publique n'expose que les zones ; deux etats
#    peuvent avoir le meme terrain, la meme main et le meme prompt tout en
#    differant par la pile de resolution en cours ou les compteurs "une fois par
#    tour". Les confondre fait DISPARAITRE des solutions sans rien signaler
#    (mesure : 10 fusions sur les 290 etats de la ligne de reference).
$procState = @'
/* --- combosolver : etat du processeur -------------------------------------
   Serialise ce que les zones ne disent pas : pile d'unites en cours de
   resolution, chaine courante, compteurs d'activation par tour, phase et LP.
   Sert de complement a la cle de transposition du solveur. */
static uint16_t combosolver_unit_step(const processor_unit& u) {
	return std::visit([](const auto& arg) -> uint16_t {
		using T = std::decay_t<decltype(arg)>;
		if constexpr(Processors::IsProcess<T>)
			return static_cast<uint16_t>(arg.step);
		else
			return 0;
	}, u);
}
static void combosolver_dump_counts(std::vector<uint8_t>& buf,
									const std::unordered_map<uint64_t, uint32_t>& m) {
	/* Trie : un unordered_map n'a pas d'ordre stable, et une cle instable
	   rendrait le digest non deterministe. */
	std::vector<std::pair<uint64_t, uint32_t>> sorted(m.begin(), m.end());
	std::sort(sorted.begin(), sorted.end());
	insert_value<uint32_t>(buf, sorted.size());
	for(const auto& kv : sorted) {
		insert_value<uint64_t>(buf, kv.first);
		insert_value<uint32_t>(buf, kv.second);
	}
}
/* Pas de prefixe OCGAPI ici : dans cette version du core la liaison est portee
   par la declaration de l'en-tete, les definitions n'en portent pas. */
void* OCG_DuelQueryProcessorState(OCG_Duel ocg_duel, uint32_t* length) {
	auto* pduel = static_cast<duel*>(ocg_duel);
	auto& field = *pduel->game_field;
	auto& buf = pduel->query_buffer;
	buf.clear();

	insert_value<uint16_t>(buf, field.infos.phase);
	insert_value<int16_t>(buf, field.infos.turn_id);
	insert_value<uint8_t>(buf, field.infos.turn_player);
	for(int p = 0; p < 2; ++p) {
		insert_value<int32_t>(buf, field.player[p].lp);
		insert_value<int32_t>(buf, field.core.summon_count[p]);
		insert_value<uint32_t>(buf, field.player[p].used_location);
		insert_value<uint32_t>(buf, field.player[p].extra_p_count);
	}
	/* Pile de resolution : c'est elle qui distingue deux instants au board
	   identique au milieu d'une meme chaine. */
	insert_value<uint32_t>(buf, field.core.units.size());
	for(const auto& u : field.core.units) {
		insert_value<uint8_t>(buf, u.index());
		insert_value<uint16_t>(buf, combosolver_unit_step(u));
	}
	insert_value<uint32_t>(buf, field.core.subunits.size());
	for(const auto& u : field.core.subunits) {
		insert_value<uint8_t>(buf, u.index());
		insert_value<uint16_t>(buf, combosolver_unit_step(u));
	}
	insert_value<uint32_t>(buf, field.core.current_chain.size());
	for(const auto& ch : field.core.current_chain) {
		insert_value<uint16_t>(buf, ch.chain_id);
		insert_value<uint8_t>(buf, ch.triggering_player);
		insert_value<uint32_t>(buf, ch.event_id);
		insert_value<uint32_t>(buf, ch.flag);
	}
	combosolver_dump_counts(buf, field.core.effect_count_code);
	combosolver_dump_counts(buf, field.core.effect_count_code_duel);
	combosolver_dump_counts(buf, field.core.effect_count_code_chain);

	if(length)
		*length = static_cast<uint32_t>(buf.size());
	return buf.data();
}

'@
Edit-File (Join-Path $dst "ocgapi.cpp") `
    'void* OCG_DuelQueryField(OCG_Duel ocg_duel, uint32_t* length) {' `
    ($procState + 'void* OCG_DuelQueryField(OCG_Duel ocg_duel, uint32_t* length) {') `
    "ocgapi.cpp (OCG_DuelQueryProcessorState)"

Edit-File (Join-Path $dst "ocgapi.cpp") `
    '#include <cstring> //std::memcpy' `
    "#include <algorithm> //std::sort`n#include <cstring> //std::memcpy" `
    "ocgapi.cpp (include algorithm)"

Edit-File (Join-Path $dst "ocgapi.h") `
    'OCGAPI void* OCG_DuelQueryField(OCG_Duel ocg_duel, uint32_t* length);' `
    @'
OCGAPI void* OCG_DuelQueryField(OCG_Duel ocg_duel, uint32_t* length);

/* combosolver : etat du processeur, invisible depuis les zones seules. */
OCGAPI void* OCG_DuelQueryProcessorState(OCG_Duel ocg_duel, uint32_t* length);
'@ `
    "ocgapi.h (declaration)"

# --- Scripts de cartes contemporains du replay.
#
# Le core ne suffit pas : un jeu de scripts decale fait diverger le rejeu tout
# aussi silencieusement. Les scripts installes dans EDOPro suivent le depot
# vivant et avancent en permanence ; il faut donc en figer un export a la date
# du replay et le passer au solveur via --scriptdir.
$scriptsDst = Join-Path $root "deps/scripts_$ScriptsDate"
if (Test-Path (Join-Path $scriptsDst "script")) {
    Write-Host "scripts deja extraits -> deps/scripts_$ScriptsDate"
} else {
    $scriptsSrc = Join-Path $root "deps/CardScripts.git"
    if (-not (Test-Path (Join-Path $scriptsSrc ".git"))) {
        Write-Host "clonage du depot de scripts (une fois)..."
        git clone --quiet --bare https://github.com/ProjectIgnis/CardScripts.git $scriptsSrc
        if ($LASTEXITCODE -ne 0) { throw "clonage du depot de scripts en echec" }
    }
    if ($(git -C $scriptsSrc cat-file -t $ScriptsCommit 2>$null) -ne "commit") {
        git -C $scriptsSrc fetch --quiet origin
        if ($LASTEXITCODE -ne 0) { throw "git fetch a echoue et $ScriptsCommit est absent localement" }
    }
    New-Item -ItemType Directory -Force -Path (Join-Path $scriptsDst "script") | Out-Null
    $tar = Join-Path $env:TEMP "scripts-$($ScriptsCommit.Substring(0,8)).tar"
    git -C $scriptsSrc archive --output=$tar $ScriptsCommit
    tar -x -f $tar -C (Join-Path $scriptsDst "script")
    Remove-Item $tar -Confirm:$false
    Write-Host "scripts -> deps/scripts_$ScriptsDate  ($ScriptsCommit)"
}

# Trace de version : le solveur la relit pour l'inscrire dans ses rapports.
@{ ocgcore = $Commit; lua = $luaCommit; patched = $true;
   fetched = (Get-Date -Format "o") } | ConvertTo-Json |
    Set-Content (Join-Path $dst "SOLVER_DEPS.json")

Write-Host "ok -> $Dest"
Write-Host ("  {0} fichiers .cpp, lua {1} fichiers .c" -f
    (Get-ChildItem $dst -Filter *.cpp).Count,
    (Get-ChildItem (Join-Path $dst "lua/src") -Filter *.c).Count)
