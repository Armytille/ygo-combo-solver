<#
.SYNOPSIS
  Extracts the version of ocgcore (and of its Lua) the combo solver needs.

.DESCRIPTION
  The solver has to run on a core CONTEMPORARY with the replay being analysed:
  a mismatched version does not crash, it diverges silently. The submodule pin
  in edopro/ is frozen at 2025-04-17 and does not suit recent replays.

  This script extracts a precise ocgcore commit, plus the matching commit of
  its lua/src submodule, into deps/ocgcore/. It never writes into the EDOPro
  installation nor into the edopro/ clone (apart from git fetch).

.EXAMPLE
  .\tools\fetch_solver_deps.ps1
  .\tools\fetch_solver_deps.ps1 -Commit 8e5f4e4f0ab6b8ca750e8e1c91c1a58f407e3272
#>
param(
    # 2026-08-10. Moved up from 8e5f4e4 (2026-04-07) after measuring that the
    # five arena patches apply without conflict on this tree, that the source
    # lists are identical, and that the health gate of the reference replay
    # (2026-04-13) is IDENTICAL line by line: 0 retries, 290 distinct digests
    # with 0 merges, reference found again at 0 deviations, novelty A/B
    # unchanged. The previous core is still extractable: -Commit 8e5f4e4f...
    #
    # Reminder: this pin must stay contemporary with the ANALYSED replay, not
    # with today's date. A replay recorded by an older EDOPro client needs an
    # older core, even though its scripts update themselves.
    [string]$Commit = "5a985af7c43c8470b06bef697bfb9051b40e114c",
    [string]$Dest = "deps/ocgcore",
    # Card scripts as of the reference replay's date. A mismatched script set
    # makes the replay diverge just as surely as a mismatched core, and just as
    # silently.
    [string]$ScriptsCommit = "0e90a3e8",
    [string]$ScriptsDate = "2026-04-13"
)

$ErrorActionPreference = "Stop"
# The solver repository is nested inside the replay2video tree: that is where
# the edopro/ clone (the source of ocgcore and of the LZMA sources) and the
# deps/ directory everything is extracted into come from. Those two paths are
# the project's only external dependencies; premake targets them the same way,
# through `../`.
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$src = Join-Path $root "edopro/ocgcore"
$dst = Join-Path $root $Dest

if (-not (Test-Path (Join-Path $src ".git"))) {
    throw "edopro/ocgcore not found. Run build_windows.bat first."
}

# Fetching is only useful when the commit is missing locally: a network outage
# must not stop a rebuild from objects that are already there.
$haveCommit = $(git -C $src cat-file -t $Commit 2>$null) -eq "commit"
if (-not $haveCommit) {
    Write-Host "fetching ocgcore objects..."
    git -C $src fetch --quiet origin
    if ($LASTEXITCODE -ne 0) { throw "git fetch failed in $src and commit $Commit is missing locally" }
} else {
    Write-Host "ocgcore objects already present locally"
}

# The lua/src pin is carried by the target commit's tree, not by HEAD.
$luaLine = git -C $src ls-tree $Commit lua/src
if (-not $luaLine) { throw "commit $Commit not found, or has no lua/src" }
$luaCommit = ($luaLine -split '\s+')[2]
Write-Host "  ocgcore : $Commit"
Write-Host "  lua/src : $luaCommit"

if (Test-Path $dst) { Remove-Item $dst -Recurse -Force -Confirm:$false }
New-Item -ItemType Directory -Force -Path $dst | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $dst "lua/src") | Out-Null

Write-Host "extracting ocgcore..."
$tar = Join-Path $env:TEMP "ocgcore-$($Commit.Substring(0,8)).tar"
git -C $src archive --output=$tar $Commit
tar -x -f $tar -C $dst
Remove-Item $tar -Confirm:$false

Write-Host "extracting lua..."
$luaSrc = Join-Path $src "lua/src"
if ($(git -C $luaSrc cat-file -t $luaCommit 2>$null) -ne "commit") {
    git -C $luaSrc fetch --quiet origin
    if ($LASTEXITCODE -ne 0) { throw "git fetch failed in $luaSrc and commit $luaCommit is missing locally" }
}
$tar = Join-Path $env:TEMP "lua-$($luaCommit.Substring(0,8)).tar"
git -C $luaSrc archive --output=$tar $luaCommit
tar -x -f $tar -C (Join-Path $dst "lua/src")
Remove-Item $tar -Confirm:$false

# --- patches for the memory snapshot ----------------------------------------
# Two modifications, both in Lua: the C++ core is NOT touched, since its
# allocations are captured by the solver's global operator new overload.
Write-Host "applying the arena patches..."

function Edit-File([string]$path, [string]$anchor, [string]$replacement, [string]$label) {
    $text = [IO.File]::ReadAllText($path)
    if ($text.Contains($replacement)) { Write-Host "  ${label}: already applied"; return }
    if (-not $text.Contains($anchor)) {
        throw "patch '$label': anchor not found in $path. The ocgcore version changed and the patch must be revised."
    }
    [IO.File]::WriteAllText($path, $text.Replace($anchor, $replacement))
    Write-Host "  ${label}: ok"
}

# 1. luaconf-customize.h is force-included into every Lua .c (lua/premake5.lua).
#    We fix the hash seed there (otherwise it derives from a stack address and
#    the clock, so two identical runs diverge) and declare the hook of the arena
#    allocator.
$hookDecl = @'
/* ---- combosolver: memory snapshot of the duel ---------------------------- */
/* Constant string hash seed: makes the core reproducible from one run to the
   next. Without it, luai_makeseed mixes a stack address and the clock
   (lstate.c). The seed only serves the anti-collision protection. */
#define luai_makeseed(L) ((void)(L), 0u)

/* Allocator supplied by the host. Left null, Lua uses realloc/free. When it is
   filled in, the WHOLE Lua heap (tables, closures, upvalues, suspended
   coroutines) lives in the arena and becomes snapshottable. */
#include <stddef.h>
#if defined(__cplusplus)
extern "C" {
#endif
/* Thread-local: each worker has its own arena, and the Lua state it creates
   must draw from that one. A global pointer would mix them up. */
typedef void* (*combosolver_alloc_fn)(void* ud, void* ptr, size_t osize, size_t nsize);
extern thread_local combosolver_alloc_fn combosolver_lua_alloc;
extern thread_local void* combosolver_lua_alloc_ud;
/* Lua state of the last duel created on this thread. The core does not expose
   it, and it has to be reachable in order to stop the garbage collector: its
   marking writes into the header of EVERY live object, which dirties nearly
   every page and ruins the point of an incremental restore. With no GC, no
   memory is lost: a restore reclaims everything an abandoned branch
   allocated. */
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
    "luaconf-customize.h (seed + allocator hook)"

# 2. luaL_newstate is the only place where the core creates its lua_State
#    (interpreter.cpp calls luaL_newstate()). We wire the allocator in there
#    without touching ocgcore itself, which keeps atpanic and setwarnf intact.
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
    "lauxlib.c (luaL_newstate -> arena allocator + state export)"

# 3. Processor state. The public API only exposes the zones; two states can
#    have the same field, the same hand and the same prompt while differing in
#    the resolution stack in progress or in the once-per-turn counters.
#    Conflating them makes solutions DISAPPEAR with nothing to show for it
#    (measured: 10 merges over the 290 states of the reference line).
$procState = @'
/* --- combosolver: processor state ------------------------------------------
   Serialises what the zones do not say: the stack of units being resolved, the
   current chain, the per-turn activation counters, the phase and the life
   points. Complements the solver's transposition key. */
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
	/* Sorted: an unordered_map has no stable order, and an unstable key would
	   make the digest non-deterministic. */
	std::vector<std::pair<uint64_t, uint32_t>> sorted(m.begin(), m.end());
	std::sort(sorted.begin(), sorted.end());
	insert_value<uint32_t>(buf, sorted.size());
	for(const auto& kv : sorted) {
		insert_value<uint64_t>(buf, kv.first);
		insert_value<uint32_t>(buf, kv.second);
	}
}
/* No OCGAPI prefix here: in this version of the core the linkage is carried by
   the header declaration, and the definitions do not repeat it. */
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
	/* Resolution stack: it is what tells apart two instants with an identical
	   board in the middle of the same chain. */
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

/* combosolver: processor state, invisible from the zones alone. */
OCGAPI void* OCG_DuelQueryProcessorState(OCG_Duel ocg_duel, uint32_t* length);
'@ `
    "ocgapi.h (declaration)"

# --- Card scripts contemporary with the replay.
#
# The core is not enough: a mismatched script set makes the replay diverge just
# as silently. The scripts installed in EDOPro follow the live repository and
# move constantly, so an export has to be frozen at the replay's date and passed
# to the solver through --scriptdir.
$scriptsDst = Join-Path $root "deps/scripts_$ScriptsDate"
if (Test-Path (Join-Path $scriptsDst "script")) {
    Write-Host "scripts already extracted -> deps/scripts_$ScriptsDate"
} else {
    $scriptsSrc = Join-Path $root "deps/CardScripts.git"
    if (-not (Test-Path (Join-Path $scriptsSrc ".git"))) {
        Write-Host "cloning the script repository (once)..."
        git clone --quiet --bare https://github.com/ProjectIgnis/CardScripts.git $scriptsSrc
        if ($LASTEXITCODE -ne 0) { throw "cloning the script repository failed" }
    }
    if ($(git -C $scriptsSrc cat-file -t $ScriptsCommit 2>$null) -ne "commit") {
        git -C $scriptsSrc fetch --quiet origin
        if ($LASTEXITCODE -ne 0) { throw "git fetch failed and $ScriptsCommit is missing locally" }
    }
    New-Item -ItemType Directory -Force -Path (Join-Path $scriptsDst "script") | Out-Null
    $tar = Join-Path $env:TEMP "scripts-$($ScriptsCommit.Substring(0,8)).tar"
    git -C $scriptsSrc archive --output=$tar $ScriptsCommit
    tar -x -f $tar -C (Join-Path $scriptsDst "script")
    Remove-Item $tar -Confirm:$false
    Write-Host "scripts -> deps/scripts_$ScriptsDate  ($ScriptsCommit)"
}

# Version stamp: the solver reads it back to record it in its reports.
@{ ocgcore = $Commit; lua = $luaCommit; patched = $true;
   fetched = (Get-Date -Format "o") } | ConvertTo-Json |
    Set-Content (Join-Path $dst "SOLVER_DEPS.json")

Write-Host "ok -> $Dest"
Write-Host ("  {0} .cpp files, lua {1} .c files" -f
    (Get-ChildItem $dst -Filter *.cpp).Count,
    (Get-ChildItem (Join-Path $dst "lua/src") -Filter *.c).Count)
