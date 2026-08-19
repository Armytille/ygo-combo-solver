-- combosolver: a headless combo solver for EDOPro replays.
--
-- Independent of the replay2video build: it compiles ITS OWN copy of ocgcore
-- (deps/ocgcore, fetched by tools/fetch_solver_deps.ps1) because it needs a
-- version contemporary with the replay being analysed, whereas the submodule of
-- an edopro/ clone is pinned a year behind.
--
-- Generate:
--   premake5 vs2022 --vcpkg-root=<...\vcpkg>
-- Build:
--   MSBuild build\combosolver.sln /p:Configuration=Release /p:Platform=x64

newoption {
	trigger = "vcpkg-root",
	value = "PATH",
	description = "vcpkg root providing sqlite3 (x64-windows-static)"
}
newoption {
	trigger = "ocgcore-dir",
	value = "PATH",
	description = "ocgcore tree to compile (default ../deps/ocgcore)"
}
newoption {
	trigger = "lzma-dir",
	value = "PATH",
	description = "LZMA sources (default ../edopro/gframe/lzma)"
}

local here = path.getabsolute(".")
local ocgdir = _OPTIONS["ocgcore-dir"] or path.join(here, "../deps/ocgcore")
local lzmadir = _OPTIONS["lzma-dir"] or path.join(here, "../edopro/gframe/lzma")
local vcpkg = _OPTIONS["vcpkg-root"] or path.join(here, "../vcpkg")
local vcpkgtriplet = path.join(vcpkg, "installed/x64-windows-static")

workspace "combosolver"
	location "build"
	language "C++"
	cppdialect "C++17"
	configurations { "Debug", "Release" }
	platforms { "x64" }
	architecture "x64"
	objdir "obj"
	targetdir "bin/%{cfg.buildcfg}"
	staticruntime "on"
	symbols "On"
	startproject "combosolver"

	filter "system:windows"
		defines { "WIN32", "_WIN32", "NOMINMAX", "WIN32_LEAN_AND_MEAN",
				  "_CRT_SECURE_NO_WARNINGS" }
	filter "configurations:Debug"
		defines "_DEBUG"
		optimize "Off"
		runtime "Debug"
	filter "configurations:Release"
		defines "NDEBUG"
		optimize "Speed"
		runtime "Release"
		-- LTO (/GL + /LTCG): the core, Lua and the solver are three separate
		-- static libraries. Without it, inlining stops at their boundaries,
		-- i.e. exactly at OCG_DuelProcess and at the arena allocator wired
		-- into Lua.
		flags { "LinkTimeOptimization" }
	filter {}

-- Lua, built the way ocgcore builds it: same exclusion list, same
-- force-include of luaconf-customize.h (which is where the deterministic
-- hashing and the arena allocator hook come in).
project "solver_lua"
	kind "StaticLib"
	-- ocgcore requires Lua compiled as C++: it calls the Lua functions from
	-- C++ without extern "C", and Lua uses longjmp, which would be undefined
	-- behaviour across C++ frames (interpreter.h:10).
	compileas "C++"
	files { path.join(ocgdir, "lua/src/*.c") }
	removefiles {
		path.join(ocgdir, "lua/src/lbitlib.c"),
		path.join(ocgdir, "lua/src/lcorolib.c"),
		path.join(ocgdir, "lua/src/ldblib.c"),
		path.join(ocgdir, "lua/src/linit.c"),
		path.join(ocgdir, "lua/src/loadlib.c"),
		path.join(ocgdir, "lua/src/loslib.c"),
		path.join(ocgdir, "lua/src/ltests.c"),
		path.join(ocgdir, "lua/src/lua.c"),
		path.join(ocgdir, "lua/src/luac.c"),
		path.join(ocgdir, "lua/src/lutf8lib.c"),
		path.join(ocgdir, "lua/src/onelua.c"),
	}
	includedirs { path.join(ocgdir, "lua"), here }
	forceincludes { "luaconf-customize.h" }
	warnings "Off"
	-- Codegen levers, CORE AND LUA ONLY (82-86 % of the time per decision
	-- lives in OCG_DuelProcess; the solver itself carries the simplex in
	-- double, and its codegen is left alone so the benches keep returning the
	-- same h byte for byte). AVX2: the reference machine is a Zen 5, and
	-- /fp:precise does not contract into FMA under VS2022, so the Lua
	-- arithmetic stays bit-identical. /GS-: no stack cookie on tiny hot
	-- functions. /Ob3: aggressive inlining (VS2019+). Each lever is judged by
	-- the us/call measurement of Process (tools/s24_perf_mesure.ps1) AND by
	-- the benches.
	filter "configurations:Release"
		vectorextensions "AVX2"
		buildoptions { "/GS-", "/Ob3" }
	filter {}

project "solver_ocgcore"
	kind "StaticLib"
	files { path.join(ocgdir, "*.cpp"), path.join(ocgdir, "RNG/*.cpp") }
	includedirs { ocgdir, path.join(ocgdir, "lua/src") }
	rtti "Off"
	warnings "Off"
	-- Same levers as solver_lua (see the comment above).
	filter "configurations:Release"
		vectorextensions "AVX2"
		buildoptions { "/GS-", "/Ob3" }
	filter {}

project "solver_lzma"
	kind "StaticLib"
	language "C"
	files {
		path.join(lzmadir, "Alloc.c"),
		path.join(lzmadir, "LzFind.c"),
		path.join(lzmadir, "LzmaDec.c"),
		path.join(lzmadir, "LzmaEnc.c"),
		path.join(lzmadir, "LzmaLib.c"),
	}
	includedirs { lzmadir }
	defines { "_7ZIP_ST" }
	warnings "Off"

project "combosolver"
	kind "ConsoleApp"
	files { "*.cpp", "*.h" }
	includedirs {
		here,
		ocgdir,
		path.join(ocgdir, "lua"),      -- luaconf-customize.h: the arena hook
		path.join(ocgdir, "lua/src"),
		lzmadir,
		path.join(vcpkgtriplet, "include"),
	}
	libdirs { path.join(vcpkgtriplet, "lib") }
	links { "solver_ocgcore", "solver_lua", "solver_lzma", "sqlite3" }
	warnings "Extra"

	filter "system:windows"
		links { "ws2_32", "advapi32" }
	filter {}
