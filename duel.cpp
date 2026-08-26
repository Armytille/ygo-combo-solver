#include "duel.h"

#include <cstring>

#include "luaconf-customize.h"   // combosolver_lua_alloc (arena patch)

// Without extern "C": Lua is compiled as C++ here (see premake5.lua), so its
// symbols are mangled like the core's.
#include "lua.h"

namespace solver {
namespace {

template<typename T>
T Take(const uint8_t*& p) {
	T v{};
	std::memcpy(&v, p, sizeof(T));
	p += sizeof(T);
	return v;
}

} // namespace

Duel::Duel(CardDB& db_, ScriptProvider& scripts_, Arena* arena_)
	: db(db_), scripts(scripts_), arena(arena_) {}

Duel::~Duel() {
	if(handle) {
		ArenaScope scope(arena);
		OCG_DestroyDuel(handle);
		handle = nullptr;
	}
}

void Duel::CardReaderThunk(void* payload, uint32_t code, OCG_CardData* data) {
	auto* self = static_cast<Duel*>(payload);
	std::memset(data, 0, sizeof(*data));
	const CardRow* row = self->db.Find(code);
	if(!row) {
		// Unknown card: the core copes, it simply gives the card no effect. That is
		// exactly the problem, a SILENT VANILLA where the deck expects an effect.
		// Recorded in the summary, like missing scripts: without it the duel starts,
		// the line diverges, and nothing says so.
		self->db.NoteUnknown(code);
		data->code = code;
		return;
	}
	data->code = row->code;
	data->alias = row->alias;
	// The core does not keep the pointer past the call (duel::read_card copies
	// into card_data), so pointing into the database is safe.
	data->setcodes = row->setcodes.empty()
						 ? nullptr
						 : const_cast<uint16_t*>(row->setcodes.data());
	data->type = row->type;
	data->level = row->level;
	data->attribute = row->attribute;
	data->race = row->race;
	data->attack = row->attack;
	data->defense = row->defense;
	data->lscale = row->lscale;
	data->rscale = row->rscale;
	data->link_marker = row->link_marker;
}

int Duel::ScriptReaderThunk(void* payload, OCG_Duel duel, const char* name) {
	auto* self = static_cast<Duel*>(payload);
	std::vector<char> buf;
	{
		// Called FROM the core, so the arena is live: the read buffer belongs to the
		// host and must not be allocated in the arena.
		ArenaPause off;
		buf = self->scripts.Read(name);
	}
	if(buf.empty())
		return 0;
	// The load itself, on the other hand, allocates inside Lua: arena live.
	return OCG_LoadScript(duel, buf.data(), static_cast<uint32_t>(buf.size()), name);
}

void Duel::LogThunk(void* payload, const char* msg, int type) {
	if(type != OCG_LOG_TYPE_ERROR)
		return;
	// The log outlives restores, so it has to live outside the arena.
	ArenaPause off;
	static_cast<Duel*>(payload)->errors.emplace_back(msg ? msg : "");
}

bool Duel::Create(const uint64_t seed[4], uint64_t flags, uint32_t lp,
				  uint32_t hand, uint32_t draw, std::string& error) {
	OCG_DuelOptions opts{};
	for(int i = 0; i < 4; ++i)
		opts.seed[i] = seed[i];
	opts.flags = flags;
	opts.team1 = OCG_Player{ lp, hand, draw };
	opts.team2 = OCG_Player{ lp, hand, draw };
	opts.cardReader = &Duel::CardReaderThunk;
	opts.payload1 = this;
	opts.scriptReader = &Duel::ScriptReaderThunk;
	opts.payload2 = this;
	opts.logHandler = &Duel::LogThunk;
	opts.payload3 = this;
	opts.cardReaderDone = [](void*, OCG_CardData*) {};
	opts.payload4 = nullptr;
	opts.enableUnsafeLibraries = 1;

	ArenaScope scope(arena);
	// Points the Lua heap at the arena before the core creates its lua_State
	// (lauxlib.c patch). Without this, suspended coroutines would sit outside the
	// snapshot and the restore would be inconsistent.
	if(arena) {
		combosolver_lua_alloc = &Arena::LuaAlloc;
		combosolver_lua_alloc_ud = arena;
	}
	int rc = OCG_CreateDuel(&handle, &opts);
	if(rc != OCG_DUEL_CREATION_SUCCESS) {
		error = "OCG_CreateDuel a echoue (code " + std::to_string(rc) + ")";
		return false;
	}
	for(const char* boot : { "constant.lua", "utility.lua" }) {
		if(!LoadScript(boot)) {
			error = std::string("cannot load ") + boot +
					" : verifier --workdir / --scriptdir";
			return false;
		}
	}
	return true;
}

bool Duel::Setup(const Replay& yrp, std::string& error,
				 const std::vector<uint32_t>* extra_hand,
				 uint8_t extra_hand_team) {
	if(yrp.decks.size() < 2) {
		error = "the yrp1 does not contain two decks";
		return false;
	}
	ArenaScope scope(arena);
	for(uint8_t team = 0; team < 2; ++team) {
		const Deck& deck = yrp.decks[team];
		for(uint32_t code : deck.main) {
			OCG_NewCardInfo info{ team, 0, code, team, LOCATION_DECK, 0,
								  POS_FACEDOWN_DEFENSE };
			OCG_DuelNewCard(handle, &info);
		}
		for(uint32_t code : deck.extra) {
			OCG_NewCardInfo info{ team, 0, code, team, LOCATION_EXTRA, 0,
								  POS_FACEDOWN_DEFENSE };
			OCG_DuelNewCard(handle, &info);
		}
	}
	// Hand test mode hands the field to the debug script rather than to a regular
	// setup (old_replay_mode.cpp:167).
	if(yrp.IsHandTest() && !ExecLua("Debug.ReloadFieldEnd()")) {
		error = "Debug.ReloadFieldEnd() a echoue";
		return false;
	}
	// After the hand test's ReloadFieldEnd (which rebuilds the field) and before
	// the start: the added cards land in hand as they are.
	if(extra_hand) {
		for(uint32_t code : *extra_hand) {
			OCG_NewCardInfo info{ extra_hand_team, 0, code, extra_hand_team,
								  LOCATION_HAND, 0, POS_FACEDOWN_DEFENSE };
			OCG_DuelNewCard(handle, &info);
		}
	}
	OCG_StartDuel(handle);
	return true;
}

int Duel::Process() {
	prof::Scope ps(prof::kProcess);
	ArenaScope scope(arena);
	return OCG_DuelProcess(handle);
}

std::vector<Message> Duel::Messages() {
	std::vector<Message> out;
	Messages(out);
	return out;
}

void Duel::Messages(std::vector<Message>& out) {
	out.clear();
	uint32_t len = 0;
	uint8_t* base = nullptr;
	{
		ArenaScope scope(arena);
		base = static_cast<uint8_t*>(OCG_DuelGetMessage(handle, &len));
	}
	// The vector belongs to the host; the Messages, however, point into the
	// core's buffer, hence into the arena, so they must be copied before any
	// restore.
	if(!base || !len)
		return;
	uint32_t off = 0;
	while(off + 4 <= len) {
		uint32_t size = 0;
		std::memcpy(&size, base + off, 4);
		off += 4;
		if(size == 0 || off + size > len)
			break;
		out.push_back(Message{ base[off], base + off + 1, size - 1 });
		off += size;
	}
}

void Duel::SetResponse(const std::vector<uint8_t>& data) {
	ArenaScope scope(arena);
	OCG_DuelSetResponse(handle, data.data(), static_cast<uint32_t>(data.size()));
}

uint32_t Duel::Count(uint8_t team, uint32_t loc) {
	prof::Scope ps(prof::kCount);
	ArenaScope scope(arena);
	return OCG_DuelQueryCount(handle, team, loc);
}

bool Duel::LastChainLink(uint8_t* trigger_player) {
	const std::vector<uint8_t>& b = ProcessorState();
	// Layout: see the OCG_DuelQueryProcessorState patch (ocgapi.cpp), the source
	// of truth. phase u16, turn i16, turn_player u8, then per player (lp i32,
	// summon_count i32, used_location u32, extra_p_count u32), then units (u32 n
	// + 3n bytes), subunits (same), then the chain (u32 n + 11n bytes: chain_id
	// u16, player u8, event u32, flag u32).
	size_t off = 2 + 2 + 1 + 2 * 16;
	auto take_u32 = [&](uint32_t& v) {
		if(off + 4 > b.size())
			return false;
		std::memcpy(&v, b.data() + off, 4);
		off += 4;
		return true;
	};
	uint32_t n = 0;
	if(!take_u32(n) || off + 3ull * n > b.size())
		return false;
	off += 3ull * n;   // units
	if(!take_u32(n) || off + 3ull * n > b.size())
		return false;
	off += 3ull * n;   // subunits
	if(!take_u32(n) || n == 0 || off + 11ull * n > b.size())
		return false;
	off += 11ull * (n - 1) + 2;   // last link, after its chain_id
	if(off >= b.size())
		return false;
	if(trigger_player)
		*trigger_player = b[off];
	return true;
}

const std::vector<uint8_t>& Duel::ProcessorState() {
	prof::Scope ps(prof::kProcState);
	uint32_t len = 0;
	const uint8_t* data = nullptr;
	{
		ArenaScope scope(arena);
		data = static_cast<const uint8_t*>(
			OCG_DuelQueryProcessorState(handle, &len));
	}
	// A null or empty buffer silently undoes the processor-state patch: without
	// that component, two distinct instants of the same chain resolution carry
	// the same digest, get merged by the transposition table, and the combo
	// branch is pruned right away. Counting makes the failure readable instead of
	// letting it pass for "there is nothing to find".
	if(!data || !len)
		++empty_processor_states;
	processor_state.assign(data, data + (data ? len : 0));
	return processor_state;
}

std::vector<QueriedCard> Duel::Query(uint8_t con, uint32_t loc, uint32_t flags) {
	std::vector<QueriedCard> out;
	Query(con, loc, flags, out);
	return out;
}

void Duel::Query(uint8_t con, uint32_t loc, uint32_t flags,
				 std::vector<QueriedCard>& out) {
	prof::Scope ps(prof::kQuery);
	out.clear();
	OCG_QueryInfo info{ flags, con, loc, 0, 0 };
	uint32_t len = 0;
	uint8_t* data = nullptr;
	{
		ArenaScope scope(arena);
		data = static_cast<uint8_t*>(OCG_DuelQueryLocation(handle, &len, &info));
	}
	if(!data || len <= 4)
		return;
	ParseQueryStreamInto(data + 4, len - 4, out);
}

void Duel::QueryCodes(uint8_t con, uint32_t loc, std::vector<uint32_t>& out) {
	prof::Scope ps(prof::kQueryCodes);
	out.clear();
	OCG_QueryInfo info{ QUERY_CODE | QUERY_ALIAS, con, loc, 0, 0 };
	uint32_t len = 0;
	uint8_t* data = nullptr;
	{
		ArenaScope scope(arena);
		data = static_cast<uint8_t*>(OCG_DuelQueryLocation(handle, &len, &info));
	}
	if(!data || len <= 4)
		return;
	// Same split as ParseQueryStream, reduced to the two useful fields:
	// Code() = alias when there is one, else code (board identity, see QueriedCard::Code).
	const uint8_t* p = data + 4;
	uint32_t n = len - 4, off = 0;
	uint32_t code = 0, alias = 0;
	bool building = false;
	while(off + 2 <= n) {
		uint16_t size = 0;
		std::memcpy(&size, p + off, 2);
		off += 2;
		if(size == 0)   // empty slot
			continue;
		if(off + size > n)
			break;
		const uint8_t* body = p + off;
		uint32_t flag = Take<uint32_t>(body);
		off += size;
		if(flag == QUERY_END) {
			out.push_back(alias ? alias : code);
			code = alias = 0;
			building = false;
			continue;
		}
		building = true;
		if(flag == QUERY_CODE)
			code = Take<uint32_t>(body);
		else if(flag == QUERY_ALIAS)
			alias = Take<uint32_t>(body);
	}
	if(building)
		out.push_back(alias ? alias : code);
}

bool Duel::LoadScript(const std::string& name) {
	auto buf = scripts.Read(name);   // outside the arena: called from the host
	if(buf.empty())
		return false;
	ArenaScope scope(arena);
	return OCG_LoadScript(handle, buf.data(), static_cast<uint32_t>(buf.size()),
						  name.c_str()) == 1;
}

bool Duel::ExecLua(const std::string& code, const std::string& chunk_name) {
	ArenaScope scope(arena);
	return OCG_LoadScript(handle, code.c_str(), static_cast<uint32_t>(code.size()),
						  chunk_name.c_str()) == 1;
}

bool Duel::SetLuaGc(bool enabled) {
	auto* L = static_cast<lua_State*>(combosolver_lua_state);
	if(!L)
		return false;
	ArenaScope scope(arena);
	lua_gc(L, enabled ? LUA_GCRESTART : LUA_GCSTOP);
	return true;
}

std::vector<QueriedCard> ParseQueryStream(const uint8_t* data, uint32_t len) {
	std::vector<QueriedCard> out;
	ParseQueryStreamInto(data, len, out);
	return out;
}

void ParseQueryStreamInto(const uint8_t* data, uint32_t len,
						  std::vector<QueriedCard>& out) {
	out.clear();
	QueriedCard cur;
	bool building = false;
	uint32_t off = 0;
	while(off + 2 <= len) {
		uint16_t size = 0;
		std::memcpy(&size, data + off, 2);
		off += 2;
		if(size == 0) {           // empty slot (ocgapi.cpp:207)
			out.push_back(QueriedCard{});
			continue;
		}
		if(off + size > len)
			break;
		const uint8_t* body = data + off;
		uint32_t flag = Take<uint32_t>(body);
		off += size;

		if(flag == QUERY_END) {
			cur.present = true;
			out.push_back(std::move(cur));
			cur = QueriedCard{};
			building = false;
			continue;
		}
		building = true;
		switch(flag) {
		case QUERY_CODE:      cur.code = Take<uint32_t>(body); break;
		case QUERY_ALIAS:     cur.alias = Take<uint32_t>(body); break;
		case QUERY_POSITION:  cur.position = Take<uint32_t>(body); break;
		case QUERY_TYPE:      cur.type = Take<uint32_t>(body); break;
		case QUERY_LEVEL:     cur.level = Take<uint32_t>(body); break;
		case QUERY_ATTACK:    cur.attack = Take<int32_t>(body); break;
		case QUERY_DEFENSE:   cur.defense = Take<int32_t>(body); break;
		case QUERY_STATUS:    cur.status = Take<uint32_t>(body); break;
		case QUERY_LINK:
			cur.link = Take<uint32_t>(body);
			cur.link_marker = Take<uint32_t>(body);
			break;
		case QUERY_OVERLAY_CARD: {
			uint32_t n = Take<uint32_t>(body);
			for(uint32_t i = 0; i < n; ++i)
				cur.overlay.push_back(Take<uint32_t>(body));
			break;
		}
		case QUERY_COUNTERS: {
			uint32_t n = Take<uint32_t>(body);
			for(uint32_t i = 0; i < n; ++i)
				cur.counters.push_back(Take<uint32_t>(body));
			break;
		}
		default: break;       // field not requested, or requested and unused
		}
	}
	if(building) {
		cur.present = true;
		out.push_back(std::move(cur));
	}
}

} // namespace solver
