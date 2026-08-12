#include "search.h"

#include <cstdio>

#include <algorithm>
#include <chrono>
#include <cstring>

namespace solver {
namespace {

uint64_t Mix(uint64_t h, uint64_t v) {
	h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
	return h;
}

constexpr uint32_t kBoardFlags = QUERY_CODE | QUERY_ALIAS | QUERY_POSITION |
								 QUERY_OVERLAY_CARD | QUERY_COUNTERS | QUERY_LINK;

// Une entree de board, independante de la colonne occupee.
uint64_t EntryOf(uint32_t loc_kind, const QueriedCard& c, const CardDB& db) {
	uint64_t h = Mix(loc_kind * 0x1000193ull, c.Code());
	h = Mix(h, c.position);
	// Les materiaux comptent, leur ordre non.
	std::vector<uint32_t> ov;
	ov.reserve(c.overlay.size());
	for(uint32_t o : c.overlay)
		ov.push_back(db.Canonical(o));
	std::sort(ov.begin(), ov.end());
	for(uint32_t o : ov)
		h = Mix(h, o * 3ull);
	std::vector<uint32_t> ct = c.counters;
	std::sort(ct.begin(), ct.end());
	for(uint32_t k : ct)
		h = Mix(h, k * 5ull);
	return h;
}

} // namespace

BoardKey ComputeBoardKey(Duel& duel, uint8_t con) {
	BoardKey key;
	for(uint32_t loc : { LOCATION_MZONE, LOCATION_SZONE }) {
		for(const auto& c : duel.Query(con, loc, kBoardFlags)) {
			if(!c.present)
				continue;
			key.entries.push_back(EntryOf(loc, c, duel.Db()));
			key.codes.push_back(c.Code());
		}
	}
	// Tri : deux boards identiques a permutation de colonnes pres doivent
	// donner la meme cle (criterium d'equivalence retenu, cf. section 1).
	std::sort(key.entries.begin(), key.entries.end());
	std::sort(key.codes.begin(), key.codes.end());
	for(uint64_t e : key.entries)
		key.hash = Mix(key.hash, e);
	return key;
}

Search::Search(Duel& d, Arena& a, const Replay& y, const SearchConfig& c)
	: duel(d), arena(a), yrp(y), cfg(c) {
	stats.distinct_by_depth.assign(cfg.max_decisions + 2, 0);
	stats.expansions_by_depth.assign(cfg.max_decisions + 2, 0);
}

bool Search::BudgetExhausted() const {
	if(stats.nodes >= cfg.max_nodes)
		return true;
	double ms = std::chrono::duration<double, std::milli>(
					std::chrono::steady_clock::now() - start).count();
	return ms >= cfg.time_limit_ms;
}

// Avance le duel jusqu'au prochain point de decision.
Search::Step Search::StepToPrompt() {
	actions_this_step = 0;
	saw_retry = false;
	for(;;) {
		int status = duel.Process();
		for(const Message& m : duel.Messages()) {
			switch(m.type) {
			case MSG_SUMMONING:
			case MSG_SPSUMMONING:
			case MSG_FLIPSUMMONING:
			case MSG_CHAINING:
				++actions_this_step;
				break;
			case MSG_RETRY:
				saw_retry = true;
				break;
			default:
				break;
			}
			if(IsPrompt(m.type)) {
				prompt_type = m.type;
				prompt_payload.assign(m.data, m.data + m.size);
				prompt_player = m.size ? m.data[0] : -1;
			}
		}
		if(saw_retry)
			return Step::Rejected;   // reponse illegale : branche morte
		if(status == OCG_DUEL_STATUS_AWAITING)
			return Step::Prompt;
		if(status == OCG_DUEL_STATUS_END)
			return Step::Ended;
		if(status != OCG_DUEL_STATUS_CONTINUE)
			return Step::Rejected;
	}
}

uint64_t Search::Digest() const {
	return StateDigest(const_cast<Duel&>(duel), prompt_type, prompt_payload);
}

uint64_t StateDigest(Duel& d, uint8_t prompt_type,
					 const std::vector<uint8_t>& prompt_payload) {
	uint64_t h = 0xcbf29ce484222325ull;
	for(uint8_t con = 0; con < 2; ++con) {
		// Terrain : l'ordre des zones est conserve, la colonne pouvant compter
		// (fleches de lien, effets colonne-dependants).
		for(uint32_t loc : { LOCATION_MZONE, LOCATION_SZONE }) {
			h = Mix(h, loc * 131ull + con);
			for(const auto& c : d.Query(con, loc, kBoardFlags)) {
				if(!c.present) { h = Mix(h, 1); continue; }
				h = Mix(h, EntryOf(loc, c, d.Db()));
			}
		}
		// Zones ou l'ordre n'a pas de sens de jeu : on canonicalise, sinon un
		// simple melange de main creerait un etat "different" et ferait
		// exploser la table de transposition pour rien.
		for(uint32_t loc : { LOCATION_HAND, LOCATION_GRAVE, LOCATION_REMOVED,
							 LOCATION_EXTRA }) {
			std::vector<uint64_t> entries;
			for(const auto& c : d.Query(con, loc, kBoardFlags))
				if(c.present)
					entries.push_back(EntryOf(loc, c, d.Db()));
			std::sort(entries.begin(), entries.end());
			h = Mix(h, loc * 131ull + con);
			for(uint64_t e : entries)
				h = Mix(h, e);
		}
		h = Mix(h, d.Count(con, LOCATION_DECK));
	}
	h = Mix(h, prompt_type);
	for(uint8_t b : prompt_payload)
		h = Mix(h, b);
	// Sans ceci, deux instants distincts d'une meme resolution de chaine —
	// meme terrain, meme main, meme prompt — sont confondus, et la branche du
	// combo est elaguee des le debut (patch C1).
	for(uint8_t b : d.ProcessorState())
		h = Mix(h, b);
	return h;
}

void Search::Descend(uint32_t depth, uint32_t actions) {
	if(BudgetExhausted()) {
		stats.hit_time_limit = true;
		return;
	}
	Step st = StepToPrompt();
	uint32_t total_actions = actions + actions_this_step;

	if(st == Step::Rejected) {
		++stats.dead_ends;
		return;
	}
	if(st == Step::Ended) {
		++stats.terminals;
		return;
	}

	++stats.nodes;
	if(depth < stats.expansions_by_depth.size())
		++stats.expansions_by_depth[depth];

	// Test de but : le board du joueur cible correspond-il ?
	if(cfg.collect_solutions && solutions.size() < cfg.max_solutions) {
		BoardKey here = ComputeBoardKey(duel, static_cast<uint8_t>(cfg.target_player));
		if(here == target) {
			Solution s;
			s.responses = path;
			s.actions = total_actions;
			s.decisions = depth;
			auto con = static_cast<uint8_t>(cfg.target_player);
			s.hand_left = duel.Count(con, LOCATION_HAND);
			s.deck_left = duel.Count(con, LOCATION_DECK);
			s.extra_left = duel.Count(con, LOCATION_EXTRA);
			s.burned = duel.Count(con, LOCATION_GRAVE) + duel.Count(con, LOCATION_REMOVED);
			solutions.push_back(std::move(s));
		}
	}

	if(depth >= cfg.max_decisions)
		return;
	if(cfg.max_actions && total_actions >= cfg.max_actions)
		return;

	// Transposition. Le budget restant est stocke avec l'etat : un etat resolu
	// avec peu de marge ne dispense pas de le reexplorer avec davantage.
	uint32_t remaining = cfg.max_decisions - depth;
	uint64_t key = Digest();
	auto it = tt.find(key);
	if(it != tt.end() && it->second >= remaining) {
		++stats.transpositions;
		return;
	}
	bool fresh = (it == tt.end());
	tt[key] = remaining;
	if(fresh && depth < stats.distinct_by_depth.size())
		++stats.distinct_by_depth[depth];

	std::vector<Choice> choices;
	if(prompt_player == cfg.target_player) {
		choices = Enumerate(prompt_type, prompt_payload.data(),
							static_cast<uint32_t>(prompt_payload.size()),
							cfg.enumeration);
	} else {
		// L'adversaire ne joue pas : il passe systematiquement (perimetre
		// solitaire retenu). On prend la derniere option, qui est le "ne rien
		// faire" pour les fenetres de chaine et les questions oui/non.
		auto all = Enumerate(prompt_type, prompt_payload.data(),
							 static_cast<uint32_t>(prompt_payload.size()),
							 cfg.enumeration);
		if(!all.empty())
			choices.push_back(all.back());
	}
	if(choices.empty()) {
		// Prompt non enumerable : on tente la reponse par defaut plutot que de
		// laisser la branche mourir.
		std::vector<uint8_t> def;
		if(DefaultResponse(prompt_type, prompt_payload.data(),
						   static_cast<uint32_t>(prompt_payload.size()), def))
			choices.push_back({ std::move(def), 0, "defaut" });
		else {
			++stats.dead_ends;
			return;
		}
	}

	arena.Push();
	for(const Choice& c : choices) {
		duel.SetResponse(c.response);
		path.push_back(c.response);
		Descend(depth + 1, total_actions);
		path.pop_back();
		arena.Restore();
		if(BudgetExhausted())
			break;
	}
	arena.Pop();
}

uint32_t Search::Heuristic() const {
	BoardKey here = ComputeBoardKey(const_cast<Duel&>(duel),
									static_cast<uint8_t>(cfg.target_player));
	// Intersection sur les CODES, pas sur les entrees completes : une entree
	// complete inclut position et materiaux, qui n'arrivent qu'a la toute fin.
	// Elle vaudrait zero partout et ne guiderait rien. Le code, lui, donne un
	// gradient : chaque carte du board cible posee fait monter le score.
	uint32_t common = 0;
	size_t i = 0, j = 0;
	while(i < here.codes.size() && j < target.codes.size()) {
		if(here.codes[i] == target.codes[j]) { ++common; ++i; ++j; }
		else if(here.codes[i] < target.codes[j]) ++i;
		else ++j;
	}
	// Bonus pour une correspondance exacte d'entree : departage deux etats qui
	// ont les memes cartes mais pas encore les bons materiaux.
	size_t a = 0, b = 0;
	uint32_t exact = 0;
	while(a < here.entries.size() && b < target.entries.size()) {
		if(here.entries[a] == target.entries[b]) { ++exact; ++a; ++b; }
		else if(here.entries[a] < target.entries[b]) ++a;
		else ++b;
	}

	// Les cartes du board cible sont des Synchro de fin de chaine : elles
	// n'arrivent qu'apres des centaines de decisions. Compter ces seules cartes
	// laisse l'heuristique a zero sur toute la montee et ne guide rien — c'est
	// ce qui faisait echouer la descente guidee.
	//
	// Ce qui progresse tout du long, c'est le MATERIEL : des corps sur le
	// terrain, et des monstres au cimetiere prets a etre recycles. On le compte
	// avec un poids faible devant les cartes cibles, pour orienter la montee
	// sans jamais primer sur l'objectif reel.
	Duel& d = const_cast<Duel&>(duel);
	auto con = static_cast<uint8_t>(cfg.target_player);
	uint32_t bodies = 0;
	for(const auto& c : d.Query(con, LOCATION_MZONE, QUERY_CODE))
		if(c.present)
			++bodies;
	uint32_t fodder = d.Count(con, LOCATION_GRAVE);
	return common * 100 + exact * 10 + bodies * 3 + fodder;
}

// Renvoie true des qu'une solution est trouvee et qu'on a de quoi s'arreter.
bool Search::DescendGuided(uint32_t depth, uint32_t actions) {
	if(BudgetExhausted()) {
		stats.hit_time_limit = true;
		return false;
	}
	Step st = StepToPrompt();
	uint32_t total_actions = actions + actions_this_step;
	if(st == Step::Rejected) { ++stats.dead_ends; return false; }
	if(st == Step::Ended)    { ++stats.terminals; return false; }

	++stats.nodes;
	if(depth < stats.expansions_by_depth.size())
		++stats.expansions_by_depth[depth];

	auto con = static_cast<uint8_t>(cfg.target_player);
	BoardKey here = ComputeBoardKey(duel, con);
	{
		uint32_t common = 0;
		for(size_t i = 0, j = 0; i < here.codes.size() && j < target.codes.size();) {
			if(here.codes[i] == target.codes[j]) { ++common; ++i; ++j; }
			else if(here.codes[i] < target.codes[j]) ++i;
			else ++j;
		}
		if(common > stats.best_overlap) {
			stats.best_overlap = common;
			stats.best_board = here.codes;
		}
		uint32_t monsters = 0;
		for(const auto& c : duel.Query(con, LOCATION_MZONE, QUERY_CODE))
			if(c.present)
				++monsters;
		if(monsters > stats.best_monsters)
			stats.best_monsters = monsters;
	}
	if(here == target) {
		Solution s;
		s.responses = path;
		s.actions = total_actions;
		s.decisions = depth;
		s.hand_left = duel.Count(con, LOCATION_HAND);
		s.deck_left = duel.Count(con, LOCATION_DECK);
		s.extra_left = duel.Count(con, LOCATION_EXTRA);
		s.burned = duel.Count(con, LOCATION_GRAVE) + duel.Count(con, LOCATION_REMOVED);
		solutions.push_back(std::move(s));
		return solutions.size() >= cfg.max_solutions;
	}

	if(depth >= cfg.max_decisions)
		return false;
	if(cfg.max_actions && total_actions > cfg.max_actions)
		return false;

	uint32_t remaining = cfg.max_decisions - depth;
	uint64_t key = Digest();
	auto it = tt.find(key);
	if(it != tt.end() && it->second >= remaining) {
		++stats.transpositions;
		return false;
	}
	if(it == tt.end() && depth < stats.distinct_by_depth.size())
		++stats.distinct_by_depth[depth];
	tt[key] = remaining;

	std::vector<Choice> choices;
	{
		auto all = Enumerate(prompt_type, prompt_payload.data(),
							 static_cast<uint32_t>(prompt_payload.size()),
							 cfg.enumeration);
		if(prompt_player == cfg.target_player)
			choices = std::move(all);
		else if(!all.empty())
			choices.push_back(all.back());   // l'adversaire passe
	}
	if(choices.empty()) {
		std::vector<uint8_t> def;
		if(!DefaultResponse(prompt_type, prompt_payload.data(),
							static_cast<uint32_t>(prompt_payload.size()), def)) {
			++stats.dead_ends;
			return false;
		}
		choices.push_back({ std::move(def), 0, "defaut" });
	}

	// Evaluation des fils : on avance, on mesure, on restaure. C'est
	// exactement ce que l'instantane rend abordable (0,05 ms par retour).
	struct Scored { size_t index; uint32_t score; uint32_t acts; bool dead; };
	std::vector<Scored> scored;
	scored.reserve(choices.size());
	arena.Push();
	for(size_t i = 0; i < choices.size(); ++i) {
		duel.SetResponse(choices[i].response);
		Step cs = StepToPrompt();
		bool dead = (cs == Step::Rejected);
		uint32_t sc = dead ? 0 : Heuristic();
		scored.push_back({ i, sc, actions_this_step, dead });
		arena.Restore();
	}
	// Le plus prometteur d'abord ; a score egal, celui qui coute le moins
	// d'actions (le critere de cout du solveur).
	std::stable_sort(scored.begin(), scored.end(),
					 [](const Scored& a, const Scored& b) {
						 if(a.score != b.score) return a.score > b.score;
						 return a.acts < b.acts;
					 });

	bool stop = false;
	for(const Scored& s : scored) {
		if(s.dead) { ++stats.dead_ends; continue; }
		duel.SetResponse(choices[s.index].response);
		path.push_back(choices[s.index].response);
		stop = DescendGuided(depth + 1, total_actions);
		path.pop_back();
		arena.Restore();
		if(stop || BudgetExhausted())
			break;
	}
	arena.Pop();
	return stop;
}

bool Search::DescendRepair(uint32_t depth, uint32_t actions, size_t ref_index,
						   uint32_t disc) {
	if(BudgetExhausted()) {
		stats.hit_time_limit = true;
		return false;
	}
	Step st = StepToPrompt();
	uint32_t total_actions = actions + actions_this_step;
	if(st == Step::Rejected) { ++stats.dead_ends; return false; }
	if(st == Step::Ended)    { ++stats.terminals; return false; }

	++stats.nodes;
	if(depth < stats.expansions_by_depth.size())
		++stats.expansions_by_depth[depth];

	if(ComputeBoardKey(duel, static_cast<uint8_t>(cfg.target_player)) == target) {
		Solution s;
		s.responses = path;
		s.actions = total_actions;
		s.decisions = depth;
		auto con = static_cast<uint8_t>(cfg.target_player);
		s.hand_left = duel.Count(con, LOCATION_HAND);
		s.deck_left = duel.Count(con, LOCATION_DECK);
		s.extra_left = duel.Count(con, LOCATION_EXTRA);
		s.burned = duel.Count(con, LOCATION_GRAVE) + duel.Count(con, LOCATION_REMOVED);
		solutions.push_back(std::move(s));
		return solutions.size() >= cfg.max_solutions;
	}

	if(depth >= cfg.max_decisions)
		return false;
	if(cfg.max_actions && total_actions > cfg.max_actions)
		return false;

	// Le budget d'ecarts est la contrainte qui lie : c'est lui qu'on memorise.
	uint64_t key = Digest();
	auto it = tt.find(key);
	if(it != tt.end() && it->second >= disc + 1) {
		++stats.transpositions;
		return false;
	}
	if(it == tt.end() && depth < stats.distinct_by_depth.size())
		++stats.distinct_by_depth[depth];
	tt[key] = disc + 1;

	// Candidats : la reponse enregistree d'abord (cout zero), puis les
	// alternatives (cout un ecart chacune).
	std::vector<std::pair<const std::vector<uint8_t>*, uint32_t>> cands;
	const std::vector<uint8_t>* recorded = nullptr;
	if(ref_index < yrp.responses.size()) {
		recorded = &yrp.responses[ref_index];
		cands.emplace_back(recorded, 0u);
	}
	// Partition entre workers : tant qu'aucun ecart n'a ete pris, ce worker doit
	// reclamer le point avant d'y devier. Une fois le premier ecart consomme le
	// sous-arbre lui appartient, les ecarts suivants ne sont plus contraints.
	const uint32_t level = cfg_discrepancies - disc;   // 0 = aucun ecart pris
	bool mine = true;
	if(cfg.claims && level == cfg.claim_level && ref_index < cfg.claims_size) {
		uint32_t expected = 0;
		mine = cfg.claims[ref_index].compare_exchange_strong(
			expected, 1, std::memory_order_relaxed);
	}

	std::vector<Choice> alts;
	if(disc > 0 && mine) {
		alts = Enumerate(prompt_type, prompt_payload.data(),
						 static_cast<uint32_t>(prompt_payload.size()),
						 cfg.enumeration);
		if(prompt_player != cfg.target_player && alts.size() > 1) {
			// L'adversaire ne joue pas : aucune deviation de son cote.
			alts.clear();
		}
		for(const Choice& c : alts)
			if(!recorded || c.response != *recorded)
				cands.emplace_back(&c.response, 1u);
	}
	if(cands.empty())
		return false;

	arena.Push();
	bool stop = false;
	for(const auto& [resp, cost] : cands) {
		if(cost > disc)
			continue;
		duel.SetResponse(*resp);
		path.push_back(*resp);
		stop = DescendRepair(depth + 1, total_actions, ref_index + 1, disc - cost);
		path.pop_back();
		arena.Restore();
		if(stop || BudgetExhausted())
			break;
	}
	arena.Pop();
	return stop;
}

bool Search::DescendTransplant(uint32_t depth, uint32_t actions, uint32_t disc) {
	if(BudgetExhausted()) {
		stats.hit_time_limit = true;
		return false;
	}
	Step st = StepToPrompt();
	uint32_t total_actions = actions + actions_this_step;
	if(st == Step::Rejected) { ++stats.dead_ends; return false; }
	if(st == Step::Ended)    { ++stats.terminals; return false; }

	++stats.nodes;
	if(depth < stats.expansions_by_depth.size())
		++stats.expansions_by_depth[depth];

	{
		auto con = static_cast<uint8_t>(cfg.target_player);
		BoardKey here = ComputeBoardKey(duel, con);
		// Combien de cartes du board cible sont reunies ici. Sans cette trace,
		// un echec ne dit pas si la recherche a manque de souffle ou si le deck
		// n'a jamais approche la cible.
		uint32_t common = 0;
		for(size_t i = 0, j = 0; i < here.codes.size() && j < target.codes.size();) {
			if(here.codes[i] == target.codes[j]) { ++common; ++i; ++j; }
			else if(here.codes[i] < target.codes[j]) ++i;
			else ++j;
		}
		if(common > stats.best_overlap) {
			stats.best_overlap = common;
			stats.best_board = here.codes;
		}
		uint32_t monsters = 0;
		for(const auto& c : duel.Query(con, LOCATION_MZONE, QUERY_CODE))
			if(c.present)
				++monsters;
		if(monsters > stats.best_monsters)
			stats.best_monsters = monsters;

		if(here == target) {
			Solution s;
			s.responses = path;
			s.actions = total_actions;
			s.decisions = depth;
			s.hand_left = duel.Count(con, LOCATION_HAND);
			s.deck_left = duel.Count(con, LOCATION_DECK);
			s.extra_left = duel.Count(con, LOCATION_EXTRA);
			s.burned = duel.Count(con, LOCATION_GRAVE) +
					   duel.Count(con, LOCATION_REMOVED);
			solutions.push_back(std::move(s));
			return solutions.size() >= cfg.max_solutions;
		}
	}

	if(depth >= cfg.max_decisions)
		return false;
	if(cfg.max_actions && total_actions > cfg.max_actions)
		return false;

	uint64_t key = Digest();
	auto it = tt.find(key);
	if(it != tt.end() && it->second >= disc + 1) {
		++stats.transpositions;
		return false;
	}
	if(it == tt.end() && depth < stats.distinct_by_depth.size())
		++stats.distinct_by_depth[depth];
	tt[key] = disc + 1;

	std::vector<Choice> choices;
	{
		auto all = Enumerate(prompt_type, prompt_payload.data(),
							 static_cast<uint32_t>(prompt_payload.size()),
							 cfg.enumeration);
		if(prompt_player == cfg.target_player)
			choices = std::move(all);
		else if(!all.empty())
			choices.push_back(all.back());   // l'adversaire passe
	}
	if(choices.empty()) {
		std::vector<uint8_t> def;
		if(!DefaultResponse(prompt_type, prompt_payload.data(),
							static_cast<uint32_t>(prompt_payload.size()), def)) {
			++stats.dead_ends;
			return false;
		}
		choices.push_back({ std::move(def), 0, "defaut" });
	}

	// Repertoire : tout coup que la reference a joue est gratuit, ou qu'il soit
	// apparu dans sa ligne. Une reponse unique l'est aussi — il n'y a rien a
	// decider, et la facturer viderait le budget sur des non-choix.
	struct Cand { size_t index; uint32_t cost; size_t rank; };
	std::vector<Cand> cands;
	cands.reserve(choices.size());
	const bool forced = choices.size() == 1;
	size_t in_plan = 0;
	for(size_t i = 0; i < choices.size(); ++i) {
		auto it_plan = plan_index.find(choices[i].plan_key);
		if(it_plan != plan_index.end()) {
			cands.push_back({ i, 0u, it_plan->second });
			++in_plan;
		} else if(forced) {
			cands.push_back({ i, 0u, plan_index.size() });
		} else if(disc > 0) {
			cands.push_back({ i, 1u, plan_index.size() + i });
		}
	}

	if(cfg.trace) {
		std::printf("    d%-4u %-22s %2zu choix, %zu au repertoire%s\n", depth,
					PromptName(prompt_type), choices.size(), in_plan,
					(in_plan == 0 && !forced) ? "   <-- hors repertoire" : "");
		if(in_plan == 0 && !forced)
			for(const Choice& c : choices)
				std::printf("            propose : %s\n", c.label.c_str());
	}

	if(cands.empty())
		return false;
	// Gratuit d'abord, et parmi eux ceux que la reference jouait tot : c'est la
	// seule trace d'ordre que le repertoire conserve.
	std::stable_sort(cands.begin(), cands.end(),
					 [](const Cand& a, const Cand& b) {
						 if(a.cost != b.cost) return a.cost < b.cost;
						 return a.rank < b.rank;
					 });

	// Partition entre workers, au meme principe que RunRepair : on reclame au
	// N-ieme ecart, pas au premier — devier tot ouvre un sous-arbre enorme que
	// les autres workers doivent pouvoir partager.
	const uint32_t level = cfg_discrepancies - disc;
	bool mine = true;
	if(cfg.claims && level == cfg.claim_level) {
		size_t slot = (key ^ (key >> 32)) % (cfg.claims_size ? cfg.claims_size : 1);
		uint32_t expected = 0;
		mine = cfg.claims[slot].compare_exchange_strong(
			expected, 1, std::memory_order_relaxed);
	}

	arena.Push();
	bool stop = false;
	for(const Cand& c : cands) {
		if(c.cost > disc)
			continue;
		if(c.cost > 0 && !mine)
			continue;   // sous-arbre pris par un autre worker
		duel.SetResponse(choices[c.index].response);
		path.push_back(choices[c.index].response);
		stop = DescendTransplant(depth + 1, total_actions, disc - c.cost);
		path.pop_back();
		arena.Restore();
		if(stop || BudgetExhausted())
			break;
	}
	arena.Pop();
	return stop;
}

bool Search::Rollout(uint64_t& rng) {
	auto next = [&rng] {
		rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
		return rng;
	};
	const auto con = static_cast<uint8_t>(cfg.target_player);
	uint32_t actions = 0;

	for(uint32_t depth = 0; depth < cfg.max_decisions; ++depth) {
		if(BudgetExhausted()) {
			stats.hit_time_limit = true;
			return false;
		}
		Step st = StepToPrompt();
		actions += actions_this_step;
		if(st == Step::Rejected) { ++stats.dead_ends; return false; }
		if(st == Step::Ended)    { ++stats.terminals; return false; }
		++stats.nodes;

		BoardKey here = ComputeBoardKey(duel, con);
		{
			uint32_t common = 0;
			for(size_t i = 0, j = 0;
				i < here.codes.size() && j < target.codes.size();) {
				if(here.codes[i] == target.codes[j]) { ++common; ++i; ++j; }
				else if(here.codes[i] < target.codes[j]) ++i;
				else ++j;
			}
			if(common > stats.best_overlap)
				stats.best_overlap = common;
			uint32_t monsters = 0;
			for(const auto& c : duel.Query(con, LOCATION_MZONE, QUERY_CODE))
				if(c.present)
					++monsters;
			if(monsters > stats.best_monsters)
				stats.best_monsters = monsters;
		}
		if(here == target) {
			Solution s;
			s.responses = path;
			s.actions = actions;
			s.decisions = depth;
			s.hand_left = duel.Count(con, LOCATION_HAND);
			s.deck_left = duel.Count(con, LOCATION_DECK);
			s.extra_left = duel.Count(con, LOCATION_EXTRA);
			s.burned = duel.Count(con, LOCATION_GRAVE) +
					   duel.Count(con, LOCATION_REMOVED);
			solutions.push_back(std::move(s));
			return true;
		}

		std::vector<Choice> choices;
		{
			auto all = Enumerate(prompt_type, prompt_payload.data(),
								 static_cast<uint32_t>(prompt_payload.size()),
								 cfg.enumeration);
			if(prompt_player == cfg.target_player)
				choices = std::move(all);
			else if(!all.empty())
				choices.push_back(all.back());
		}
		if(choices.empty()) {
			std::vector<uint8_t> def;
			if(!DefaultResponse(prompt_type, prompt_payload.data(),
								static_cast<uint32_t>(prompt_payload.size()), def)) {
				++stats.dead_ends;
				return false;
			}
			choices.push_back({ std::move(def), 0, "defaut" });
		}

		size_t pick = 0;
		if(choices.size() > 1) {
			// On avance, on mesure, on restaure : c'est ce que l'instantane rend
			// abordable, et c'est ici que se joue toute la qualite du tirage.
			struct Scored { size_t index; int64_t score; bool known; bool dead; };
			std::vector<Scored> scored;
			scored.reserve(choices.size());
			arena.Push();
			for(size_t i = 0; i < choices.size(); ++i) {
				duel.SetResponse(choices[i].response);
				Step cs = StepToPrompt();
				bool dead = (cs == Step::Rejected);
				int64_t sc = dead ? -1 : static_cast<int64_t>(Heuristic());
				scored.push_back({ i, sc,
								   plan_index.count(choices[i].plan_key) != 0,
								   dead });
				arena.Restore();
			}
			arena.Pop();

			// Le repertoire DEPARTAGE, il ne domine pas. En prime additive il
			// ferait passer "fin du tour" — que la reference joue, donc connue —
			// devant une invocation qui, elle, avance vraiment. Or terminer le
			// tour est irreversible : le board est fige.
			std::stable_sort(scored.begin(), scored.end(),
							 [](const Scored& a, const Scored& b) {
								 if(a.score != b.score) return a.score > b.score;
								 return a.known > b.known;
							 });
			// Le premier tirage est glouton pur ; les suivants s'en ecartent de
			// plus en plus rarement a mesure qu'on descend le classement.
			size_t r = 0;
			while(r + 1 < scored.size() && scored[r + 1].score >= 0 &&
				  (next() & 3u) == 0)
				++r;
			if(scored[r].dead) {
				++stats.dead_ends;
				return false;
			}
			pick = scored[r].index;
		}

		duel.SetResponse(choices[pick].response);
		path.push_back(choices[pick].response);
	}
	return false;
}

void Search::RunRollouts(const BoardKey& t, const std::vector<PlanStep>& p,
						 uint32_t count, uint64_t seed) {
	target = t;
	plan = &p;
	plan_index.clear();
	for(size_t i = 0; i < p.size(); ++i)
		if(p[i].edge)
			plan_index.emplace(p[i].edge, i);
	start = std::chrono::steady_clock::now();
	solutions.clear();
	path.clear();

	arena.Push();     // point de reprise : la racine
	for(uint32_t r = 0; r < count && !BudgetExhausted(); ++r) {
		uint64_t rng = seed + r * 0x9e3779b97f4a7c15ull;
		if(!rng)
			rng = 1;
		path.clear();
		bool hit = Rollout(rng);
		arena.Restore();
		if(hit && solutions.size() >= cfg.max_solutions)
			break;
	}
	arena.Pop();

	stats.ms = std::chrono::duration<double, std::milli>(
				   std::chrono::steady_clock::now() - start).count();
	stats.hit_node_limit = stats.nodes >= cfg.max_nodes;
	stats.exhausted = false;   // un tirage n'epuise jamais rien
}

void Search::RunTransplant(const BoardKey& t, const std::vector<PlanStep>& p,
						   uint32_t discrepancies) {
	target = t;
	plan = &p;
	start = std::chrono::steady_clock::now();
	solutions.clear();
	path.clear();
	tt.clear();
	cfg_discrepancies = discrepancies;
	plan_index.clear();
	for(size_t i = 0; i < p.size(); ++i)
		if(p[i].edge)
			plan_index.emplace(p[i].edge, i);
	DescendTransplant(0, 0, discrepancies);
	stats.ms = std::chrono::duration<double, std::milli>(
				   std::chrono::steady_clock::now() - start).count();
	stats.hit_node_limit = stats.nodes >= cfg.max_nodes;
	stats.exhausted = !stats.hit_time_limit && !stats.hit_node_limit;
}

size_t LiftPlan(Duel& duel, Arena& arena, const Replay& yrp, int target_player,
				size_t stop_after, const EnumOptions& eo,
				std::vector<PlanStep>& out) {
	size_t unknown = 0, ri = 0;
	uint8_t ptype = 0;
	std::vector<uint8_t> payload;
	int player = -1;

	// Applique une reponse et rend l'empreinte de l'etat atteint, prompt
	// compris. 0 signale un rejet par le core (donc aucune correspondance
	// possible) ; la collision avec une empreinte legitime est negligeable.
	auto advance = [&](const std::vector<uint8_t>& resp) -> uint64_t {
		duel.SetResponse(resp);
		uint8_t nt = 0;
		std::vector<uint8_t> np;
		for(;;) {
			int status = duel.Process();
			for(const Message& m : duel.Messages()) {
				if(m.type == MSG_RETRY)
					return 0;
				if(IsPrompt(m.type)) {
					nt = m.type;
					np.assign(m.data, m.data + m.size);
				}
			}
			if(status != OCG_DUEL_STATUS_CONTINUE)
				break;
		}
		return StateDigest(duel, nt, np);
	};

	for(;;) {
		int status = duel.Process();
		for(const Message& m : duel.Messages()) {
			if(IsPrompt(m.type)) {
				ptype = m.type;
				payload.assign(m.data, m.data + m.size);
				player = m.size ? m.data[0] : -1;
			}
		}
		if(status == OCG_DUEL_STATUS_END)
			break;
		if(status != OCG_DUEL_STATUS_AWAITING)
			continue;
		if(ri >= yrp.responses.size() || ri >= stop_after)
			break;

		const std::vector<uint8_t>& recorded = yrp.responses[ri];
		if(player == target_player) {
			auto choices = Enumerate(ptype, payload.data(),
									 static_cast<uint32_t>(payload.size()), eo);
			arena.Push();
			uint64_t want = advance(recorded);
			arena.Restore();
			bool found = false;
			for(const Choice& c : choices) {
				uint64_t got = advance(c.response);
				arena.Restore();
				if(want && got == want) {
					// plan_key et non edge : c'est l'identite debarrassee de ce
					// que l'equivalence de board ignore deja (la colonne). Y
					// mettre l'arete rendrait tout choix de zone inappariable.
					out.push_back({ ptype, c.plan_key, c.label });
					found = true;
					break;
				}
			}
			arena.Pop();
			if(!found) {
				++unknown;
				out.push_back({ ptype, 0, "non identifiee" });
			}
		}
		duel.SetResponse(recorded);
		++ri;
	}
	return unknown;
}

void Search::RunRepair(const BoardKey& t, uint32_t discrepancies) {
	target = t;
	start = std::chrono::steady_clock::now();
	solutions.clear();
	path.clear();
	tt.clear();
	cfg_discrepancies = discrepancies;
	DescendRepair(0, 0, 0, discrepancies);
	stats.ms = std::chrono::duration<double, std::milli>(
				   std::chrono::steady_clock::now() - start).count();
	stats.hit_node_limit = stats.nodes >= cfg.max_nodes;
	stats.exhausted = !stats.hit_time_limit && !stats.hit_node_limit;
}

void Search::RunGuided(const BoardKey& t) {
	target = t;
	start = std::chrono::steady_clock::now();
	solutions.clear();
	path.clear();
	tt.clear();
	DescendGuided(0, 0);
	stats.ms = std::chrono::duration<double, std::milli>(
				   std::chrono::steady_clock::now() - start).count();
	stats.hit_node_limit = stats.nodes >= cfg.max_nodes;
	stats.exhausted = !stats.hit_time_limit && !stats.hit_node_limit;
}

void Search::Run(const BoardKey& t) {
	target = t;
	start = std::chrono::steady_clock::now();
	solutions.clear();
	path.clear();
	tt.clear();
	Descend(0, 0);
	stats.ms = std::chrono::duration<double, std::milli>(
				   std::chrono::steady_clock::now() - start).count();
	stats.exhausted = !stats.hit_time_limit && stats.nodes < cfg.max_nodes;
	stats.hit_node_limit = stats.nodes >= cfg.max_nodes;
}

} // namespace solver
