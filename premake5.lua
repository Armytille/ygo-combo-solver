-- combosolver — solveur de combo headless.
--
-- Independant du build replay2video : il compile SA PROPRE copie d'ocgcore
-- (deps/ocgcore, extraite par tools/fetch_solver_deps.ps1) parce qu'il lui faut
-- une version contemporaine du replay analyse, alors que le sous-module du
-- clone edopro/ est fige un an en arriere.
--
-- Generation :
--   premake5 vs2022 --vcpkg-root=<...\vcpkg>
-- Compilation :
--   MSBuild build\combosolver.sln /p:Configuration=Release /p:Platform=x64

newoption {
	trigger = "vcpkg-root",
	value = "PATH",
	description = "racine vcpkg fournissant sqlite3 (x64-windows-static)"
}
newoption {
	trigger = "ocgcore-dir",
	value = "PATH",
	description = "arborescence ocgcore a compiler (defaut ../deps/ocgcore)"
}
newoption {
	trigger = "lzma-dir",
	value = "PATH",
	description = "sources LZMA (defaut ../edopro/gframe/lzma)"
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
		-- LTO (/GL + /LTCG) : le core, Lua et le solveur sont trois libs
		-- statiques distinctes — sans lui, l'inlining s'arrete a leurs
		-- frontieres, c'est-a-dire exactement sur OCG_DuelProcess et sur
		-- l'allocateur d'arene branche dans Lua (chantier perf, session 11).
		flags { "LinkTimeOptimization" }
	filter {}

-- Lua, tel que le construit ocgcore : meme liste d'exclusions, meme
-- force-include de luaconf-customize.h (c'est par la que passent le
-- deterministe du hachage et le branchement de l'allocateur d'arene).
project "solver_lua"
	kind "StaticLib"
	-- ocgcore exige un Lua compile en C++ : il appelle les fonctions Lua
	-- depuis du C++ sans extern "C", et Lua utilise longjmp, ce qui serait un
	-- comportement indefini a travers des cadres C++ (interpreter.h:10).
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

project "solver_ocgcore"
	kind "StaticLib"
	files { path.join(ocgdir, "*.cpp"), path.join(ocgdir, "RNG/*.cpp") }
	includedirs { ocgdir, path.join(ocgdir, "lua/src") }
	rtti "Off"
	warnings "Off"

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
		path.join(ocgdir, "lua"),      -- luaconf-customize.h : accroche d'arene
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
