// Wrapper around a statically linked ocgcore duel.
//
// The solver only uses the public API (ocgapi.h): no dependency on rendering,
// none on gframe.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "arena.h"
#include "assets.h"
#include "ocgapi.h"
#include "ocgapi_constants.h"
#include "ocgapi_types.h"
#include "replay.h"

namespace solver {

struct Message {
	uint8_t type{};
	const uint8_t* data{};   // points into the core's buffer, valid until the
	uint32_t size{};         // next Process()
};

// One card as OCG_DuelQueryLocation returns it.
struct QueriedCard {
	bool present{ false };   // false = empty slot
	uint32_t code{}, alias{}, position{}, type{}, level{}, status{};
	int32_t attack{}, defense{};
	uint32_t link{}, link_marker{};
	std::vector<uint32_t> overlay;
	std::vector<uint32_t> counters;

	// QUERY_CODE returns the printed code; QUERY_ALIAS returns card::get_code()
	// (card.cpp:236), which resolves artwork variants (29053657 and 29053656 are
	// the same Quetzalcoatl) and name-changing effects.
	//
	// Comparing two boards must go through HERE: two decks playing different
	// artworks of the same card produce the same field, and telling them apart
	// would make the target artificially unreachable.
	// `code` stays the physical code, the only one valid for counting how many
	// copies a deck has spent.
	uint32_t Code() const { return alias ? alias : code; }
};

class Duel {
public:
	// `arena` may be null: the duel then runs on the system allocator, with no
	// snapshot possible. Otherwise ALL the duel's memory is confined to it.
	Duel(CardDB& db, ScriptProvider& scripts, Arena* arena = nullptr);
	~Duel();
	Duel(const Duel&) = delete;
	Duel& operator=(const Duel&) = delete;

	// Creates the duel and loads constant.lua / utility.lua the way
	// Game::SetupDuel does (game.cpp:4173).
	bool Create(const uint64_t seed[4], uint64_t flags, uint32_t lp,
				uint32_t hand, uint32_t draw, std::string& error);

	// Installs the decks and the extra deck, then starts. Mirrors
	// ReplayMode::StartDuel (old_replay_mode.cpp:108), hand test mode included.
	// `extra_hand`: cards ADDED to `extra_hand_team`'s hand before the start
	// (--opp-hand). That is what gives the opponent of a hand test a real hand:
	// with no playable card across the table the core never opens an opponent
	// response window, the guard would be satisfied vacuously and the handrip
	// would rip nothing.
	bool Setup(const Replay& yrp, std::string& error,
			   const std::vector<uint32_t>* extra_hand = nullptr,
			   uint8_t extra_hand_team = 1);

	int Process();                       // OCG_DUEL_STATUS_*
	std::vector<Message> Messages();     // messages produced by the last Process
	// Allocation-free variant once warm: `out` is reused.
	void Messages(std::vector<Message>& out);
	void SetResponse(const std::vector<uint8_t>& data);

	std::vector<QueriedCard> Query(uint8_t con, uint32_t loc, uint32_t flags);
	// Variant reusing `out` (hot paths query the zones at every decision, and
	// allocating per query was a measured hot spot).
	void Query(uint8_t con, uint32_t loc, uint32_t flags,
			   std::vector<QueriedCard>& out);
	// Codes only (Code() = alias when there is one, else code), hidden zones for
	// the atoms and the guard: no QueriedCard at all.
	void QueryCodes(uint8_t con, uint32_t loc, std::vector<uint32_t>& out);
	uint32_t Count(uint8_t team, uint32_t loc);

	// State the zones do not show: the resolution stack in progress, the current
	// chain, once-per-turn counters, phase, LP. Without it, two distinct instants
	// of the same resolution share a digest and the search prunes the combo
	// branch.
	const std::vector<uint8_t>& ProcessorState();
	// THE LAST LINK OF THE CURRENT CHAIN (--no-self-negate). Returns false when
	// no chain is in progress; otherwise writes the player who triggered the most
	// recent link. Decodes the ProcessorState blob (layout: the
	// OCG_DuelQueryProcessorState patch in ocgapi.cpp is the source of truth)
	// rather than tracking MSG_CHAINING: a member counter would desynchronise on
	// arena restores, and pruning wrongly is the bad direction, since it cuts a
	// real move.
	bool LastChainLink(uint8_t* trigger_player);

	bool LoadScript(const std::string& name);
	bool ExecLua(const std::string& code, const std::string& chunk_name = " ");

	// Stops or restarts the Lua garbage collector. Stopped, its marking no longer
	// writes into the header of every live object, which is what dirties pages en
	// masse and ruins the incremental restore. No memory is lost for it: a
	// restore reclaims everything an abandoned branch allocated. No effect on the
	// rules either, since ocgcore relies on no __gc metamethod and counts its own
	// references.
	bool SetLuaGc(bool enabled);

	const std::vector<std::string>& Errors() const { return errors; }
	OCG_Duel Handle() const { return handle; }
	Arena* GetArena() const { return arena; }
	const CardDB& Db() const { return db; }

private:
	static void CardReaderThunk(void* payload, uint32_t code, OCG_CardData* data);
	static int ScriptReaderThunk(void* payload, OCG_Duel duel, const char* name);
	static void LogThunk(void* payload, const char* msg, int type);

	CardDB& db;
	ScriptProvider& scripts;
	Arena* arena{ nullptr };
	OCG_Duel handle{ nullptr };
	std::vector<std::string> errors;
	std::vector<uint8_t> processor_state;
	// Processor-state queries the core returned EMPTY. When non-zero, the digest
	// loses its chain component: two distinct instants of the same resolution are
	// conflated and the combo branch is pruned right away, silently undoing the
	// processor-state patch.
	size_t empty_processor_states = 0;

public:
	size_t EmptyProcessorStates() const { return empty_processor_states; }
};

// Splits the buffer returned by OCG_DuelQueryLocation (prefixed with its
// useful size, ocgapi.cpp:234) into cards.
std::vector<QueriedCard> ParseQueryStream(const uint8_t* data, uint32_t len);
void ParseQueryStreamInto(const uint8_t* data, uint32_t len,
						  std::vector<QueriedCard>& out);

} // namespace solver
