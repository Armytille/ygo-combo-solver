// Card data and Lua scripts, served to the core.
//
// Mirrors what EDOPro does (data_manager.cpp:100 for cards, game.cpp:4120 for
// the script search order) without pulling in the rest of the client.
#pragma once

#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ocgapi_types.h"

namespace solver {

// One cards.cdb row, laid out for OCG_CardData. Setcodes are stored here
// because the core keeps the pointer for the duration of the read.
struct CardRow {
	uint32_t code{}, alias{}, type{}, level{}, attribute{}, lscale{}, rscale{};
	uint64_t race{};
	int32_t attack{}, defense{};
	uint32_t link_marker{};
	std::vector<uint16_t> setcodes; // zero-terminated when non-empty
};

class CardDB {
public:
	// Loads expansions/*.cdb and repositories/**/*.cdb. An empty cards.cdb at the
	// root is ignored rather than failing the load.
	bool Load(const std::string& workdir, std::string& error);

	const CardRow* Find(uint32_t code) const;
	// FIRST LINE of the card text. For extra deck monsters that is the MATERIALS
	// line, and its format is regular:
	//     "Lunalight Leo Dancer" + 3 "Lunalight" monsters
	//     2 Level 4 monsters
	// It is the SEED of the recipe graph: the text only seeds it, the truth comes
	// from observation. Empty when absent.
	const std::string& MaterialLine(uint32_t code) const;
	// Canonical code of a card from its EXACT name (0 when unknown). Materials
	// named in card text are quoted, exactly.
	uint32_t CodeByExactName(const std::string& name) const;
	// Readable name, for reports only. "?" when unknown.
	const std::string& Name(uint32_t code) const;
	// Cards whose name contains `needle` (case-insensitive). CANONICAL codes,
	// deduplicated: two artworks of the same card share a name and must count
	// once. Used by the constraint CLI; an ambiguous fragment must be reported,
	// not guessed.
	std::vector<std::pair<uint32_t, std::string>>
	FindByName(const std::string& needle) const;
	// Canonical code of an artwork variant. QUERY_OVERLAY_CARD returns the
	// physical code of the materials (card.cpp:180) without going through
	// get_code(): without this resolution, two Xyz carrying different artworks
	// look like they carry different cards.
	uint32_t Canonical(uint32_t code) const;
	size_t Size() const { return cards.size(); }
	const std::vector<std::string>& Sources() const { return sources; }

	// Codes the core asked for that the database does not know. This is the
	// DATABASE TWIN of a script-set mismatch, and it is quieter still: the core
	// gives the unknown card a vanilla body with no effect (duel.cpp), the deck
	// loads, the duel starts, the line diverges, and there is no equivalent of
	// ScriptProvider::Misses() to report it. The mutex guards concurrent calls
	// from the card reader in the workers.
	void NoteUnknown(uint32_t code) const;
	std::vector<uint32_t> UnknownCodes() const;

private:
	bool LoadFile(const std::string& path);
	std::unordered_map<uint32_t, CardRow> cards;
	std::unordered_map<uint32_t, std::string> names;
	// First line of the card text, by code. Loaded along with the names.
	std::unordered_map<uint32_t, std::string> material_lines;
	// Exact name -> canonical code, built after loading.
	std::unordered_map<std::string, uint32_t> by_name;
	std::vector<std::string> sources;
	mutable std::mutex unknown_mx;
	mutable std::set<uint32_t> unknown;
};

class ScriptProvider {
public:
	// `override_roots` replaces the installation's live repositories. Use an
	// export of the repository contemporary with the replay: a mismatched script
	// set makes the replay diverge silently.
	void Init(const std::string& workdir,
			  const std::vector<std::string>& override_roots);

	// Returns the script contents with the BOM stripped. Empty when not found.
	// Callable from several threads: the provider is shared between workers, and
	// only the miss log is mutable.
	//
	// IN-MEMORY CACHE. A script loaded AFTER an arena's root Push is undone by
	// every Restore: the core asks for it again on the next rollout, and each
	// request used to cost a directory sweep plus fopen/fread. Script contents
	// are IMMUTABLE for the duration of a run (repositories do not move under our
	// feet), so the first read comes from disk and every later one from memory.
	// Lua recompilation still happens; only the I/O disappears. `CacheHits()` is
	// the liveness counter of the mechanism.
	std::vector<char> Read(const std::string& name);
	uint64_t CacheHits() const {
		std::lock_guard<std::mutex> lock(misses_mutex);
		return cache_hits;
	}
	size_t CacheEntries() const {
		std::lock_guard<std::mutex> lock(misses_mutex);
		return cache.size();
	}

	const std::vector<std::string>& Dirs() const { return dirs; }
	std::unordered_set<std::string> Misses() const {
		std::lock_guard<std::mutex> lock(misses_mutex);
		return misses;
	}
	// Files that are PRESENT but that the read did not return (short I/O, zero
	// size). Distinct from misses: a short read used to pass for "absent" and
	// made the loader take the SAME script from a lower-ranked repository, hence
	// another version. That is a script-set mismatch manufactured out of an I/O
	// error, and invisible because it never reached `misses`.
	std::unordered_set<std::string> Unreadable() const {
		std::lock_guard<std::mutex> lock(misses_mutex);
		return unreadable;
	}

private:
	std::string workdir;
	std::vector<std::string> dirs;
	mutable std::mutex misses_mutex;
	std::unordered_set<std::string> misses;
	std::unordered_set<std::string> unreadable;
	// In-memory cache of scripts already read; key: normalised name. Failures are
	// NOT cached: they are rare, already logged (misses), and a negative cache
	// would hide a transient I/O error.
	std::unordered_map<std::string, std::vector<char>> cache;
	uint64_t cache_hits = 0;
};

} // namespace solver
