#include "duel.h"

#include <cstring>

#include "luaconf-customize.h"   // combosolver_lua_alloc (patch d'arene)

// Sans extern "C" : Lua est compile en C++ ici (cf. premake5.lua), ses symboles
// sont donc mangles comme ceux du core.
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
		// Carte inconnue : le core sait faire, il ne lui donnera aucun effet.
		// C'est precisement le probleme — une VANILLE MUETTE la ou le deck
		// attend un effet. Releve pour le bilan, comme les scripts manquants
		// (4.6) : sans cela le duel demarre, la ligne diverge, et rien ne le dit.
		self->db.NoteUnknown(code);
		data->code = code;
		return;
	}
	data->code = row->code;
	data->alias = row->alias;
	// Le core ne conserve pas le pointeur au-dela de l'appel (duel::read_card
	// copie dans card_data), donc pointer dans la base est sur.
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
		// Appele DEPUIS le core, donc arene active : le tampon de lecture
		// appartient a l'hote, il ne doit pas etre alloue dans l'arene.
		ArenaPause off;
		buf = self->scripts.Read(name);
	}
	if(buf.empty())
		return 0;
	// En revanche le chargement lui-meme alloue dans Lua : arene active.
	return OCG_LoadScript(duel, buf.data(), static_cast<uint32_t>(buf.size()), name);
}

void Duel::LogThunk(void* payload, const char* msg, int type) {
	if(type != OCG_LOG_TYPE_ERROR)
		return;
	// Le journal survit aux restaurations : il doit vivre hors arene.
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
	// Branche le heap Lua sur l'arene avant que le core ne cree son lua_State
	// (patch lauxlib.c). Sans cela, les coroutines suspendues resteraient hors
	// de l'instantane et la restauration serait incoherente.
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
			error = std::string("impossible de charger ") + boot +
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
		error = "le yrp1 ne contient pas deux decks";
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
	// Le mode hand test laisse le champ au script de debug plutot qu'a une
	// mise en place classique (old_replay_mode.cpp:167).
	if(yrp.IsHandTest() && !ExecLua("Debug.ReloadFieldEnd()")) {
		error = "Debug.ReloadFieldEnd() a echoue";
		return false;
	}
	// Apres le ReloadFieldEnd du hand test (qui reconstruit le terrain), avant
	// le demarrage : les cartes ajoutees arrivent en main telles quelles.
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
	// Le vecteur appartient a l'hote ; les Message pointent en revanche
	// dans le tampon du core, donc dans l'arene : a copier avant toute
	// restauration.
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
	// Layout : voir le patch OCG_DuelQueryProcessorState (ocgapi.cpp), la
	// source de verite. phase u16, turn i16, turn_player u8, puis par joueur
	// (lp i32, summon_count i32, used_location u32, extra_p_count u32), puis
	// unites (u32 n + 3n octets), sous-unites (idem), puis la chaine
	// (u32 n + 11n octets : chain_id u16, joueur u8, event u32, flag u32).
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
	off += 3ull * n;   // unites
	if(!take_u32(n) || off + 3ull * n > b.size())
		return false;
	off += 3ull * n;   // sous-unites
	if(!take_u32(n) || n == 0 || off + 11ull * n > b.size())
		return false;
	off += 11ull * (n - 1) + 2;   // dernier maillon, apres son chain_id
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
	// Un tampon nul ou vide DEFAIT le patch C1 en silence : sans la composante
	// d'etat de processeur, deux instants distincts d'une meme resolution de
	// chaine portent le meme digest, sont fusionnes par la table de
	// transposition, et la branche du combo est elaguee des le debut. Le comptage
	// rend la panne lisible au lieu de la laisser se deguiser en « il n'y a rien
	// a trouver » (audit 18).
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
	// Meme decoupage que ParseQueryStream, reduit aux deux champs utiles :
	// Code() = alias sinon code (identite de board, cf. QueriedCard::Code).
	const uint8_t* p = data + 4;
	uint32_t n = len - 4, off = 0;
	uint32_t code = 0, alias = 0;
	bool building = false;
	while(off + 2 <= n) {
		uint16_t size = 0;
		std::memcpy(&size, p + off, 2);
		off += 2;
		if(size == 0)   // emplacement vide
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
	auto buf = scripts.Read(name);   // hors arene : appel depuis l'hote
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
		if(size == 0) {           // emplacement vide (ocgapi.cpp:207)
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
		default: break;       // champ non demande ou non exploite
		}
	}
	if(building) {
		cur.present = true;
		out.push_back(std::move(cur));
	}
}

} // namespace solver
