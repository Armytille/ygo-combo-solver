#include "search.h"

#include "operators.h"

#include <cstdio>

#include <algorithm>
#include <bitset>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <queue>

namespace solver {

// (SerialProgress is declared in search.h: it also serves the finisher root
// probe. Definition further down in this file.)

namespace {

uint64_t Mix(uint64_t h, uint64_t v) {
	h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
	return h;
}

// Mixes a buffer by 8-byte words rather than byte by byte: the length is mixed
// first and the tail is zero-padded, so two distinct buffers cannot produce the
// same word sequence. The digest VALUES change, their discriminating power does
// not; the control is the equality of the structural health facts (273
// pairwise-distinct digests).
uint64_t MixBytes(uint64_t h, const uint8_t* p, size_t n) {
	h = Mix(h, n);
	size_t i = 0;
	for(; i + 8 <= n; i += 8) {
		uint64_t v;
		std::memcpy(&v, p + i, 8);
		h = Mix(h, v);
	}
	if(i < n) {
		uint64_t tail = 0;
		std::memcpy(&tail, p + i, n - i);
		h = Mix(h, tail);
	}
	return h;
}

// MATERIAL part of an NRPA rollout's score. The score is material*1000 +
// novelty (see PolicyRollout); novelty is consumed by the table on the first
// pass, so replaying the same line yields a slightly LOWER score, never an
// equal one. Comparing the material score is the "re-found" criterion of
// limited repetitions (arXiv:2401.10420).
uint64_t MatScore(double score) {
	return score < 0 ? 0 : static_cast<uint64_t>(score / 1000.0);
}

constexpr uint32_t kBoardFlags = QUERY_CODE | QUERY_ALIAS | QUERY_POSITION |
								 QUERY_OVERLAY_CARD | QUERY_COUNTERS | QUERY_LINK |
								 QUERY_LEVEL | QUERY_STATUS;

// THE STATUS MASK THAT MATTERS TO THE RULES. The core's status word mostly
// carries ENGINE flags (SUMMONING, BATTLE_RESULT, INITIALIZING...) whose
// hashing would blow the table up for nothing; OVER-hashing loses merges, the
// other failure. We only keep what changes the LEGALITY of a FUTURE move:
//   DISABLED       the card is negated (Imperm...): its effects no longer
//                  exist, and merging "negated" with "active" made lines
//                  disappear without saying so;
//   PROC_COMPLETE  summoned PROPERLY: decides whether a ReviveLimit lets it be
//                  revived, the exact legality the balance model approximates;
//   FORBIDDEN      cannot be activated;
//   SET_TURN       set THIS turn (a trap set this turn does not activate);
//   *_SUMMON_TURN  summoned/flipped this turn (tributes, turn restrictions).
constexpr uint32_t kDigestStatusMask =
	STATUS_DISABLED | STATUS_PROC_COMPLETE | STATUS_FORBIDDEN |
	STATUS_SET_TURN | STATUS_SUMMON_TURN | STATUS_SPSUMMON_TURN |
	STATUS_FLIP_SUMMON_TURN;

// HIDDEN zones (hand, graveyard, banished, extra): materials, counters and link
// arrows do not exist there, since cards lost their overlays on leaving the
// field. Asking for those fields made the core serialise (and us parse) empty
// bytes at every digest query. The values of EntryOf are UNCHANGED by
// construction: overlay and counters were already empty there.
constexpr uint32_t kHiddenFlags = QUERY_CODE | QUERY_ALIAS | QUERY_POSITION;

// One board entry, independent of the column occupied. `goal_view`: GOAL
// equivalence, where the ATK/DEF position is IGNORED (player's ruling: two
// boards differing only by a battle position are the SAME board) and only the
// face (up/down) counts, since a Junk Signal set face down is not a face-up
// Junk Signal. The STATE digest, on the other hand, keeps the full position: a
// different position IS a different game state, and merging it would make lines
// disappear without saying so.
// LOOSE entry: zone, code, face, and NOTHING else. It is the comparison a
// target POSTED by hand (`--target`) calls for, which is a code and a position
// and CANNOT carry materials: a posted Xyz with no materials would otherwise
// equal no real Xyz, since every Xyz on the field carries some.
// This was the SECOND reason, independent of the empty S/T zone, why benchmark
// A's goal was unsatisfiable, Bagooska being in all of its commands.
uint64_t LooseEntryOf(uint32_t loc_kind, uint32_t code, uint32_t position) {
	uint64_t h = Mix(loc_kind * 0x1000193ull, code);
	return Mix(h, (position & POS_FACEUP) ? POS_FACEUP : POS_FACEDOWN);
}

uint64_t EntryOf(uint32_t loc_kind, const QueriedCard& c, const CardDB& db,
				 bool goal_view = false) {
	uint64_t h = Mix(loc_kind * 0x1000193ull, c.Code());
	h = Mix(h, goal_view ? ((c.position & POS_FACEUP) ? POS_FACEUP
													  : POS_FACEDOWN)
						 : c.position);
	// TWO UNDER-HASHINGS FIXED, never in the GOAL view (a target posted by
	// hand is a code and a face, not a level of the moment): the current
	// LEVEL (level modifiers change the legality of a Synchro/Xyz, so two
	// states differing only by it do not have the same moves) and the STATUS
	// filtered by the mask above.
	// The hidden zones do not request those fields (kHiddenFlags): they are
	// zero there, mixed as a constant, and their value is unchanged.
	if(!goal_view) {
		h = Mix(h, c.level * 7ull);
		h = Mix(h, (c.status & kDigestStatusMask) * 11ull);
	}
	// Materials count, their order does not.
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

void ComputeBoardKeyInto(Duel& duel, uint8_t con, BoardKey& key) {
	// Self time: the two internal Query calls charge their own probe.
	prof::Scope ps(prof::kBoardKey);
	static thread_local std::vector<QueriedCard> cards;
	key.hash = 0;
	key.entries.clear();
	key.loose.clear();
	key.codes.clear();
	key.mzone_count = 0;
	for(uint32_t loc : { LOCATION_MZONE, LOCATION_SZONE }) {
		duel.Query(con, loc, kBoardFlags, cards);
		for(const auto& c : cards) {
			if(!c.present)
				continue;
			key.entries.push_back(EntryOf(loc, c, duel.Db(), /*goal_view=*/true));
			key.loose.push_back(LooseEntryOf(loc, c.Code(), c.position));
			key.codes.push_back(c.Code());
			if(loc == LOCATION_MZONE)
				++key.mzone_count;
		}
	}
	// Sort: two boards identical up to a permutation of columns must give the same
	// key (the equivalence criterion we use).
	std::sort(key.entries.begin(), key.entries.end());
	std::sort(key.loose.begin(), key.loose.end());
	std::sort(key.codes.begin(), key.codes.end());
	for(uint64_t e : key.entries)
		key.hash = Mix(key.hash, e);
}

BoardKey ComputeBoardKey(Duel& duel, uint8_t con) {
	BoardKey key;
	ComputeBoardKeyInto(duel, con, key);
	return key;
}

BoardKey MakeBoardKey(const std::vector<QueriedCard>& mzone,
					  const std::vector<QueriedCard>& szone, const CardDB& db) {
	BoardKey key;
	for(const auto& c : mzone) {
		if(!c.present)
			continue;
		key.entries.push_back(EntryOf(LOCATION_MZONE, c, db, /*goal_view=*/true));
		key.loose.push_back(
			LooseEntryOf(LOCATION_MZONE, db.Canonical(c.Code()), c.position));
		key.codes.push_back(c.Code());
		++key.mzone_count;
	}
	for(const auto& c : szone) {
		if(!c.present)
			continue;
		key.entries.push_back(EntryOf(LOCATION_SZONE, c, db, /*goal_view=*/true));
		key.loose.push_back(
			LooseEntryOf(LOCATION_SZONE, db.Canonical(c.Code()), c.position));
		key.codes.push_back(c.Code());
	}
	std::sort(key.entries.begin(), key.entries.end());
	std::sort(key.loose.begin(), key.loose.end());
	std::sort(key.codes.begin(), key.codes.end());
	for(uint64_t e : key.entries)
		key.hash = Mix(key.hash, e);
	return key;
}

uint32_t CommonCodes(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
	uint32_t common = 0;
	size_t i = 0, j = 0;
	while(i < a.size() && j < b.size()) {
		if(a[i] == b[j]) { ++common; ++i; ++j; }
		else if(a[i] < b[j]) ++i;
		else ++j;
	}
	return common;
}

void CollectAtoms(Duel& duel, uint8_t con, const BoardKey& here,
				  uint32_t partition, std::vector<uint64_t>& out) {
	prof::Scope ps(prof::kAtoms);
	out.clear();
	const uint64_t p = partition * 0x9e3779b97f4a7c15ull;
	// Field: the complete entries (code, position, materials, counters) are
	// already computed, i.e. fine-grained atoms. The code alone is a separate
	// atom: "the card has reached the field" is a fact in itself, even though
	// position and materials will still change.
	for(uint64_t e : here.entries)
		out.push_back(Mix(p ^ 0x51, e));
	for(uint32_t c : here.codes)
		out.push_back(Mix(p ^ 0x52, c));
	// Hidden zones: (zone, card, occurrence). It is through the graveyard and
	// the hand that a combo climbs; the target board cards are Synchros at the
	// end of the chain, invisible for 250 decisions.
	// The occurrence distinguishes the second copy: sending a second copy to
	// the graveyard is a new fact. QueryCodes: the codes are enough, no
	// QueriedCard to build (a measured hot spot, 4 queries per decision).
	static thread_local std::vector<uint32_t> zone_codes;
	for(uint32_t loc : { LOCATION_HAND, LOCATION_GRAVE, LOCATION_REMOVED,
						 LOCATION_EXTRA }) {
		duel.QueryCodes(con, loc, zone_codes);
		std::sort(zone_codes.begin(), zone_codes.end());
		uint32_t prev = 0, occ = 0;
		for(uint32_t c : zone_codes) {
			occ = (c == prev) ? occ + 1 : 1;
			prev = c;
			out.push_back(Mix(Mix(p ^ (loc * 131ull), c), occ));
		}
	}
	out.push_back(Mix(p ^ 0x53, duel.Count(con, LOCATION_DECK)));
}

Search::Search(Duel& d, Arena& a, const Replay& y, const SearchConfig& c)
	: duel(d), arena(a), yrp(y), cfg(c) {
	stats.distinct_by_depth.assign(cfg.max_decisions + 2, 0);
	stats.expansions_by_depth.assign(cfg.max_decisions + 2, 0);
	// The enumerator counts its own truncations in OUR stats: a subset never
	// emitted is a branch missing from the space, exactly like a ceiling cut.
	cfg.enumeration.subsets_capped = &stats.subsets_capped;
	// OFFER PROBE: the enumerator sets one bit per watched card present in the
	// prompt's pool. `cfg` is a COPY per Search, so pointing at a member is safe,
	// each worker having its own.
	// Armed only under --probe-repeat: outside the probe there is no cost, not even
	// the pointer test in the canonicalisation loop.
	if(cfg.probe_repeat && !cfg.probe_watch.empty()) {
		cfg.enumeration.watch = &cfg.probe_watch;
		cfg.enumeration.watch_offered = &offer_this_step;
	}
	for(const ResolveReq& req : cfg.resolve_min)
		resolve_total += req.min_count;
	// Seed of the burned bound: the best burned counts of the previous phases bound
	// it from the start (anytime only, slack included).
	if(cfg.anytime && cfg.burn_limit && cfg.burn_slack < 255)
		burn_cut = cfg.burn_limit + cfg.burn_slack;
}

uint32_t Search::CurrentBurned() {
	const auto con = static_cast<uint8_t>(cfg.target_player);
	return duel.Count(con, LOCATION_GRAVE) + duel.Count(con, LOCATION_REMOVED);
}

uint32_t Search::EffectiveBurnCut() const {
	uint32_t cut = burn_cut;
	if(cfg.shared_burn && cfg.anytime && cfg.burn_slack < 255) {
		const uint32_t g = cfg.shared_burn->load(std::memory_order_relaxed);
		if(g != UINT32_MAX && g + cfg.burn_slack < cut)
			cut = g + cfg.burn_slack;
	}
	return cut;
}

// Packed lexicographic cost: burned, then actions, then decisions, comparable
// through a single integer.
static inline uint64_t CostKey(uint32_t burned, uint32_t actions,
							   uint32_t decisions) {
	return (static_cast<uint64_t>(burned) << 44) |
		   (static_cast<uint64_t>((std::min)(actions, 0xFFFFFFu)) << 20) |
		   (std::min)(decisions, 0xFFFFFu);
}

bool Search::BudgetExhausted() const {
	// POISONED ARENA: an allocation escaped the arena, so Restore() no longer
	// reconstitutes the duel. Anything that followed would describe a divergent
	// state, so we stop here and the worker says so. One relaxed atomic load per
	// node, on a path that already reads a clock.
	if(arena.Poisoned()) {
		stats.arena_poisoned = true;
		return true;
	}
	if(stats.nodes >= cfg.max_nodes)
		return true;
	// BUDGET IN ROLLOUTS: tested BEFORE the clock, otherwise time would remain the
	// binding bound and the deterministic mode would not exist.
	if(cfg.max_rollouts && stats.rollout_count >= cfg.max_rollouts)
		return true;
	double ms = std::chrono::duration<double, std::milli>(
					std::chrono::steady_clock::now() - start).count();
	return ms >= cfg.time_limit_ms;
}

// Advances the duel to the next decision point.
Search::Step Search::StepToPrompt() {
	actions_this_step = 0;
	turns_this_step = 0;
	summons_this_step.clear();
	resolved_this_step = 0;
	watch_this_step = 0;
	watch_act_this_step = 0;
	watch_zone_this_step = 0;
	material_violation = false;
	recent_materials.clear();
	recipe_materials.clear();
	saw_retry = false;
	for(;;) {
		int status = duel.Process();
		duel.Messages(msgs_scratch);
		for(const Message& m : msgs_scratch) {
			switch(m.type) {
			case MSG_MOVE:
				// The materials of a summon carry REASON_MATERIAL: we accumulate
				// them to attribute them to the summon that follows in the same
				// resolution.
				//
				// The payload is code(4) | PREVIOUS loc(10) | current loc(10) |
				// reason(4), so byte 5 is THE ZONE THE MATERIAL WAS TAKEN FROM. It
				// is what makes a recipe graph node a REQUIREMENT ("Leo Dancer in
				// the graveyard") rather than a card, which is rule 1.
				// ZONE PRESENCE PROBE (`--watch`). The DESTINATION zone is at
				// offset 15 (current loc_info: controller 14, location 15).
				// Recorded here, hence at zero cost: MSG_MOVE is already decoded
				// and no zone query is added.
				//
				// It is the only part of the probe that talks about STATES rather
				// than events, and the one benchmark A needs: `Lunalight Leo
				// Dancer` is neither summoned nor activated, its role is to REACH
				// THE GRAVEYARD so it can be banished there as a material.
				if(!cfg.probe_watch.empty() && m.size >= 16) {
					uint32_t mc = 0;
					std::memcpy(&mc, m.data, 4);
					if(mc) {
						mc = duel.Db().Canonical(mc);
						const int slot = ZoneSlot(NormalizeZone(m.data[15]));
						if(slot >= 0)
							for(size_t i = 0;
								i < cfg.probe_watch.size() && i < 4; ++i)
								if(cfg.probe_watch[i] == mc)
									watch_zone_this_step |=
										1u << (8 * i + slot);
					}
				}
				if((!cfg.material_req.empty() || cfg.recipes) && m.size >= 28) {
					uint32_t code = 0, reason = 0;
					std::memcpy(&code, m.data, 4);
					std::memcpy(&reason, m.data + 24, 4);
					const uint8_t from = m.data[5];   // previous location
					if(reason & REASON_MATERIAL) {
						// --material only knows the Synchro case; the recipe
						// graph, on the other hand, takes EVERY mechanism, which
						// is the whole point.
						if((reason & REASON_SYNCHRO) &&
						   !cfg.material_req.empty())
							recent_materials.push_back(code);
						if(cfg.recipes)
							recipe_materials.push_back(
								Requirement{ duel.Db().Canonical(code),
											 NormalizeZone(from) });
					}
				}
				break;
			case MSG_SUMMONING:
			case MSG_SPSUMMONING:
				// The code of the summoned card is the message's first u32
				// (operations.cpp), or 0 when it arrives face down. It is the
				// count the summon constraints observe.
				if(m.size >= 4) {
					uint32_t code = 0;
					std::memcpy(&code, m.data, 4);
					summons_this_step.push_back(code);
					// PURE PROBE (`--watch`): count, and nothing else. No
					// constraint, no gradient, no hint bias; that is the whole
					// point, see SearchConfig::probe_watch.
					if(!cfg.probe_watch.empty() && code) {
						const uint32_t wc = duel.Db().Canonical(code);
						for(size_t i = 0; i < cfg.probe_watch.size() && i < 4; ++i)
							if(cfg.probe_watch[i] == wc)
								watch_this_step += 1ull << (16 * i);
					}
					// Watched summons (--summon-min): the same packed counter as
					// the resolutions.
					if(!cfg.resolve_min.empty() && code) {
						const uint32_t sc = duel.Db().Canonical(code);
						for(size_t i = 0; i < cfg.resolve_min.size(); ++i)
							if(cfg.resolve_min[i].on_summon &&
							   cfg.resolve_min[i].code == sc)
								resolved_this_step += 1ull << (16 * i);
					}
					// Material constraint: the watched summon must have consumed
					// at least one card of the right attribute.
					if(!cfg.material_req.empty() && code) {
						uint32_t canon = duel.Db().Canonical(code);
						for(const auto& [card, attrs] : cfg.material_req) {
							if(card != canon)
								continue;
							bool ok = false;
							for(uint32_t mat : recent_materials) {
								const CardRow* row = duel.Db().Find(mat);
								if(row && (row->attribute & attrs)) {
									ok = true;
									break;
								}
							}
							if(!ok)
								material_violation = true;
						}
						// Every summon consumes its materials.
						recent_materials.clear();
					}
					// RECIPE GRAPH: the summon we just saw is an OBSERVED recipe,
					// with its materials and their source zones. No card text is
					// read (rule 3): a Fusion from the Pendulum Zone, a Fusion
					// substitute or a card that copies a name all register in
					// exactly the same way, as one more supplier.
					// `!replaying`: the prefix replay of the return to the rung
					// goes over the SAME summons at every re-entry, and
					// re-observing them would inflate those recipes' support in
					// proportion to the re-entry rate, a manufactured bias.
					if(cfg.recipes && code && !replaying) {
						cfg.recipes->Observe(duel.Db().Canonical(code),
											 recipe_materials);
						++stats.recipes_seen;
					}
					recipe_materials.clear();
				}
				++actions_this_step;
				break;
			case MSG_FLIPSUMMONING:
				++actions_this_step;
				break;
			case MSG_CHAINING:
				// PURE PROBE, ACTIVATIONS PART (`--watch`).
				//
				// WHY IT WAS MISSING, and what its absence hid. The probe only
				// counted SUMMONS, so it could say nothing about cards whose role
				// is to OPEN a route. That is exactly benchmark A's case:
				// `Lunalight Leo Dancer` requires a named material ABSENT FROM THE
				// DECK (Panther Dancer), so it is NEVER summonable by the normal
				// route; it can only come from the effect of `Lunalight Wolf` or of
				// `Lunalight Masquerade`, which summon a Fusion by banishing the
				// materials from the field OR THE GRAVEYARD. So "Leo is never
				// summoned" did not say whether the solver had even tried the door.
				//
				// Strictly observational, like the summons part: no constraint, no
				// gradient, no bias.
				if(!cfg.probe_watch.empty() && m.size >= 4) {
					uint32_t wcode = 0;
					std::memcpy(&wcode, m.data, 4);
					wcode = duel.Db().Canonical(wcode);
					for(size_t i = 0; i < cfg.probe_watch.size() && i < 4; ++i)
						if(cfg.probe_watch[i] == wcode)
							watch_act_this_step += 1ull << (16 * i);
				}
				// USES OF THE QUOTA HOSTS: one activation of the host = one
				// more use on this path. Enters the cell key (QuotaKey), since
				// the relevant state is (board, quotas).
				if(!cfg.quota_hosts.empty() && m.size >= 4) {
					uint32_t qc = 0;
					std::memcpy(&qc, m.data, 4);
					qc = duel.Db().Canonical(qc);
					for(size_t i = 0;
						i < cfg.quota_hosts.size() && i < 12; ++i)
						if(cfg.quota_hosts[i] == qc)
							++quota_uses[i];
				}
				// Watched resolutions (--resolve). Counted at activation:
				// playing solo, nothing negates a chain. Filtered by
				// ACTIVATION zone: the Omega that rips activates from the
				// field, and its graveyard effect does not count.
				if(!cfg.resolve_min.empty() && m.size >= 4) {
					uint32_t code = 0;
					std::memcpy(&code, m.data, 4);
					code = duel.Db().Canonical(code);
					const uint32_t loc = ChainingLocation(m.data, m.size);
					for(size_t i = 0; i < cfg.resolve_min.size(); ++i)
						if(!cfg.resolve_min[i].on_summon &&
						   cfg.resolve_min[i].code == code &&
						   (!cfg.resolve_min[i].zones ||
							(loc & cfg.resolve_min[i].zones)))
							resolved_this_step += 1ull << (16 * i);
				}
				++actions_this_step;
				break;
			case MSG_NEW_TURN:
				++turns_this_step;
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
			return Step::Rejected;   // illegal answer: dead branch
		if(status == OCG_DUEL_STATUS_AWAITING)
			return Step::Prompt;
		if(status == OCG_DUEL_STATUS_END)
			return Step::Ended;
		if(status != OCG_DUEL_STATUS_CONTINUE)
			return Step::Rejected;
	}
}

uint64_t Search::Digest() const {
	return StateDigest(const_cast<Duel&>(duel), prompt_type, prompt_payload,
					   false);
}

bool Search::FillChoices(ChoiceList& out) {
	// OFFER PROBE: the bits are valid for THE prompt being enumerated now. Reset
	// here rather than in the caller, because FillChoices is the single choke point
	// of ALL the strategies; an omission in one of them would leak an offer from
	// one prompt into the next, silently.
	offer_this_step = 0;
	EnumerateInto(prompt_type, prompt_payload.data(),
				  static_cast<uint32_t>(prompt_payload.size()),
				  cfg.enumeration, out);
	// The opponent does not play: it passes systematically (the solo scope we
	// chose). The last option is the "do nothing" of chain windows and yes/no
	// questions.
	if(prompt_player != cfg.target_player)
		out.KeepOnlyLast();
	// THE --no-self-negate DISCIPLINE. A chain window of the player, NOT forced
	// (the last option is the decline, answer -1; a forced window emits none, and a
	// rule obligation is not filtered), and the last link of the chain is OURS:
	// the declared NEGATION effects (pairs (code, desc) from the table, desc 0 =
	// the whole card) are not offered. The decline always remains.
	if(cfg.self_negate && !cfg.self_negate->empty() &&
	   prompt_type == MSG_SELECT_CHAIN && prompt_player == cfg.target_player &&
	   out.size() > 1) {
		const Choice& last = out[out.size() - 1];
		const bool declinable =
			last.response.size() == 4 && last.response[0] == 0xff &&
			last.response[1] == 0xff && last.response[2] == 0xff &&
			last.response[3] == 0xff;
		uint8_t link_player = 255;
		if(declinable && duel.LastChainLink(&link_player) &&
		   link_player == cfg.target_player) {
			for(size_t i = out.size() - 1; i-- > 0;) {
				bool banned = false;
				for(const auto& [bc, bd] : *cfg.self_negate)
					banned = banned || (out[i].card == bc &&
										(bd == 0 || out[i].desc == bd));
				if(banned) {
					out.RemoveAt(i);
					++stats.self_negate_cuts;
				}
			}
		}
	}
	if(out.empty()) {
		// Non-enumerable prompt: we try the default answer rather than letting the
		// branch die. This is a REDUCTION TO ONE BRANCH, and it used to be mute: a
		// combo requiring a card name to be declared (ANNOUNCE_*), a counter to be
		// chosen or a sort to be done is structurally out of reach, and nothing said
		// so. Counted by prompt type so the report can say WHICH.
		Choice& c = out.Emit();
		if(!DefaultResponse(prompt_type, prompt_payload.data(),
							static_cast<uint32_t>(prompt_payload.size()),
							c.response)) {
			// NO default answer: the branch DIES here. That is not a reduction, and
			// counting it as one overcounted the same event twice (it also counts in
			// `dead_ends` in the caller). That is the case of the three ANNOUNCE_*,
			// and of SELECT_COUNTER when the requested count is unreachable.
			++stats.forced_killed;
			stats.forced_killed_prompts |= 1ull << (prompt_type & 63);
			out.Clear();
			return false;
		}
		++stats.forced_default;
		stats.forced_default_prompts |= 1ull << (prompt_type & 63);
		if(cfg.enumeration.labels)
			c.label = "defaut";
	}
	return true;
}

// DIGEST ATTRIBUTION: splits the transposition key into its three components
// WITHOUT changing its value.
//
// THE QUESTION: at the stable points (idle prompt) the solver maintains 284
// distinct states per BOARD, and the ratio GROWS with depth. The key is zones +
// prompt type + prompt payload + processor state; the hidden zones are already
// canonicalised by sorting, so the inflation comes from the last two. WHICH,
// and in what proportion? Without that answer, fixing it would be a bet, and
// the rule here is attribution before the fix.
// `StateDigest` calls this function and combines EXACTLY in the same order: the
// value produced is identical byte for byte, so no earlier measurement is
// invalidated.
DigestParts StateDigestParts(Duel& d, uint8_t prompt_type,
							 const std::vector<uint8_t>& prompt_payload) {
	prof::Scope ps(prof::kDigest);
	static thread_local std::vector<QueriedCard> cards;
	static thread_local std::vector<uint64_t> entries;
	DigestParts out;
	uint64_t h = 0xcbf29ce484222325ull;
	// SORTED VARIANT of the field, computed in parallel and changing nothing in
	// `h`: it is what ISOLATES THE PRICE OF THE COLUMN. `StateDigest` preserves the
	// order of the field zones (the column can matter: link arrows,
	// column-dependent effects), which is a CONSERVATIVE choice, not a necessity,
	// and `BoardKey` explicitly ignores it on its side. The gap between `zones` and
	// `zones_sorted` says exactly what that choice costs.
	uint64_t hs = 0xcbf29ce484222325ull;
	static thread_local std::vector<uint64_t> field;
	for(uint8_t con = 0; con < 2; ++con) {
		field.clear();
		for(uint32_t loc : { LOCATION_MZONE, LOCATION_SZONE }) {
			h = Mix(h, loc * 131ull + con);
			d.Query(con, loc, kBoardFlags, cards);
			for(const auto& c : cards) {
				if(!c.present) { h = Mix(h, 1); continue; }
				const uint64_t e = EntryOf(loc, c, d.Db());
				h = Mix(h, e);
				// The zone enters the sorted entry, the COLUMN does not: two
				// monsters swapping columns give the same value.
				field.push_back(e ^ (loc * 0x9e3779b97f4a7c15ull));
			}
		}
		std::sort(field.begin(), field.end());
		hs = Mix(hs, con);
		for(uint64_t e : field)
			hs = Mix(hs, e);
		for(uint32_t loc : { LOCATION_HAND, LOCATION_GRAVE, LOCATION_REMOVED,
							 LOCATION_EXTRA }) {
			entries.clear();
			d.Query(con, loc, kHiddenFlags, cards);
			for(const auto& c : cards)
				if(c.present)
					entries.push_back(EntryOf(loc, c, d.Db()));
			std::sort(entries.begin(), entries.end());
			h = Mix(h, loc * 131ull + con);
			hs = Mix(hs, loc * 131ull + con);
			for(uint64_t e : entries) {
				h = Mix(h, e);
				hs = Mix(hs, e);
			}
		}
		h = Mix(h, d.Count(con, LOCATION_DECK));
		hs = Mix(hs, d.Count(con, LOCATION_DECK));
		// THE DECK ORDER IS A GAME STATE: draws, excavations and flips read
		// it, and two paths leaving the same VISIBLE state can leave
		// different orders (a search shuffle consumes the RNG). Merging it
		// made lines disappear without saying so, the under-hashing family.
		// Hashed AS IS, never sorted: the order is precisely the state.
		// (Two states with the same order but a different RNG position stay
		// merged: a shuffle is RANDOM as far as the rules go, every legal
		// permutation is equivalent, and that equivalence is chosen and
		// written down.)
		static thread_local std::vector<uint32_t> deck_codes;
		d.QueryCodes(con, LOCATION_DECK, deck_codes);
		for(uint32_t dc : deck_codes) {
			h = Mix(h, dc * 13ull);
			hs = Mix(hs, dc * 13ull);
		}
	}
	out.zones = h;
	out.zones_sorted = hs;
	h = Mix(h, prompt_type);
	h = MixBytes(h, prompt_payload.data(), prompt_payload.size());
	out.with_payload = h;
	const std::vector<uint8_t>& pstate = d.ProcessorState();
	out.procstate = MixBytes(0xcbf29ce484222325ull, pstate.data(), pstate.size());
	out.full = MixBytes(h, pstate.data(), pstate.size());
	return out;
}

uint64_t StateDigest(Duel& d, uint8_t prompt_type,
					 const std::vector<uint8_t>& prompt_payload,
					 bool sort_field) {
	// COLUMN CANONICALISATION. The sorted path goes through StateDigestParts,
	// which computes both variants: that is twice as expensive, but this path is
	// only taken under the flag. The default path below is unchanged byte for
	// byte.
	if(sort_field) {
		const DigestParts p = StateDigestParts(d, prompt_type, prompt_payload);
		// Same composition as the ordered version, starting from the sorted field.
		uint64_t h = Mix(p.zones_sorted, prompt_type);
		h = MixBytes(h, prompt_payload.data(), prompt_payload.size());
		const std::vector<uint8_t>& ps2 = d.ProcessorState();
		return MixBytes(h, ps2.data(), ps2.size());
	}
	// Self time: the 12 internal Query calls, the 2 Count calls and the
	// ProcessorState go to their own probes; only EntryOf + sort + mix stays here.
	prof::Scope ps(prof::kDigest);
	static thread_local std::vector<QueriedCard> cards;
	static thread_local std::vector<uint64_t> entries;
	uint64_t h = 0xcbf29ce484222325ull;
	for(uint8_t con = 0; con < 2; ++con) {
		// Field: the order of the zones is preserved, since the column can matter
		// (link arrows, column-dependent effects).
		for(uint32_t loc : { LOCATION_MZONE, LOCATION_SZONE }) {
			h = Mix(h, loc * 131ull + con);
			d.Query(con, loc, kBoardFlags, cards);
			for(const auto& c : cards) {
				if(!c.present) { h = Mix(h, 1); continue; }
				h = Mix(h, EntryOf(loc, c, d.Db()));
			}
		}
		// Zones where the order has no meaning in the game: we canonicalise,
		// otherwise a simple hand shuffle would create a "different" state and blow
		// the transposition table up for nothing.
		for(uint32_t loc : { LOCATION_HAND, LOCATION_GRAVE, LOCATION_REMOVED,
							 LOCATION_EXTRA }) {
			entries.clear();
			d.Query(con, loc, kHiddenFlags, cards);
			for(const auto& c : cards)
				if(c.present)
					entries.push_back(EntryOf(loc, c, d.Db()));
			std::sort(entries.begin(), entries.end());
			h = Mix(h, loc * 131ull + con);
			for(uint64_t e : entries)
				h = Mix(h, e);
		}
		h = Mix(h, d.Count(con, LOCATION_DECK));
		// The deck order, same reason and same shape as in StateDigestParts; the
		// two copies MUST stay in sync.
		static thread_local std::vector<uint32_t> deck_codes;
		d.QueryCodes(con, LOCATION_DECK, deck_codes);
		for(uint32_t dc : deck_codes)
			h = Mix(h, dc * 13ull);
	}
	h = Mix(h, prompt_type);
	h = MixBytes(h, prompt_payload.data(), prompt_payload.size());
	// Without this, two distinct instants of the same chain resolution (same
	// field, same hand, same prompt) are conflated, and the combo branch is
	// pruned right away.
	const std::vector<uint8_t>& pstate = d.ProcessorState();
	h = MixBytes(h, pstate.data(), pstate.size());
	return h;
}

void Search::Descend(uint32_t depth, uint32_t actions, uint32_t prompt_depth) {
	if(BudgetExhausted()) {
		stats.hit_time_limit = true;
		return;
	}
	// ELISION SAFEGUARD: a forced move does not advance `depth`, so nothing would
	// stop a looping chain of forced moves. The ceiling bears on PROMPTS and it is
	// wide: it must only bite on a pathology, never on a normal line (the
	// reference has 284).
	if(prompt_depth > cfg.max_decisions * 8u + 64u) {
		++stats.edges_skipped;
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
	prof::Count(prof::kDecisions);
	if(depth < stats.expansions_by_depth.size())
		++stats.expansions_by_depth[depth];

	// Goal test: does the target player's board match?
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

	// CEILING cuts: it is not the space that stops here, it is the bound. Counted,
	// otherwise "EXHAUSTED" would lie. Tested BEFORE the enumeration, as
	// originally: enumerating a node we are about to cut would be a cost added TO
	// THE CONTROL ARM, and the A/B would no longer measure the mechanism but the
	// move of this call.
	if(depth >= cfg.max_decisions) {
		++stats.edges_skipped;
		return;
	}
	if(cfg.max_actions && total_actions >= cfg.max_actions) {
		++stats.edges_skipped;
		return;
	}

	// --- ENUMERATION BEFORE THE TABLE ---------------------------------------
	//
	// The order is reversed compared with the original, and that is the whole
	// mechanism: one cannot know a prompt is FORCED before enumerating it, and a
	// forced prompt must cost neither a table entry, nor a snapshot, nor depth.
	// Without `elide_forced` the behaviour is the previous one; the only
	// difference is that `FillChoices` is called before the table lookup rather
	// than after, on the same state and with the same result.
	ChoiceList& choices = ChoicesAt(prompt_depth);
	if(!FillChoices(choices)) {
		++stats.dead_ends;
		return;
	}
	// GLOBAL ATTRIBUTION: the breakdown of EVERY expanded node. This is the
	// measurement that was missing when the column was fixed on the strength of a
	// count restricted to idle points; an attribution over a subset does not carry
	// over to the whole.
	if(choices.size() <= 1)
		++stats.nodes_forced;
	else if(prompt_type == MSG_SELECT_IDLECMD)
		++stats.nodes_idle;
	else
		++stats.nodes_multi;

	// FORCED MOVE: played INLINE. No arena Push/Pop, since there is no sibling to
	// restore and the snapshot is a measured cost (0.084 ms against 0.056 ms of
	// useful work, i.e. 149 % overhead). No table entry either: a state with no
	// alternative has nothing to transpose. And no depth: `depth` now counts
	// DECISIONS, not prompts.
	if(cfg.elide_forced && choices.size() == 1) {
		++stats.elided;
		duel.SetResponse(choices[0].response);
		path.push_back(choices[0].response);
		Descend(depth, total_actions, prompt_depth + 1);
		path.pop_back();
		return;
	}

	// Transposition. The remaining budget is stored with the state: a state solved
	// with little margin does not excuse re-exploring it with more.
	uint32_t remaining = cfg.max_decisions - depth;
	uint64_t key = Digest();
	auto it = tt.find(key);
	if(it != tt.end() && it->second >= remaining) {
		++stats.transpositions;
		return;
	}
	bool fresh = (it == tt.end());
	tt[key] = remaining;
	// DISTINCT BOARDS. Counted AFTER the transposition: we do not want to measure
	// how many times we come back, but how many DIFFERENT boards the retained
	// states realise. The states/boards ratio is the price paid for the
	// transposition key carrying the history of the effects (the once-per-turn
	// counters the public API does not expose).
	if(cfg.count_boards) {
		ComputeBoardKeyInto(duel, static_cast<uint8_t>(cfg.target_player),
							board_scratch);
		seen_boards.insert(board_scratch.hash);
		uint64_t hl = 0, hc = 0;
		for(uint64_t e : board_scratch.loose)
			hl = hl * 0x100000001b3ull + e;
		for(uint32_t c : board_scratch.codes)
			hc = hc * 0x100000001b3ull + c;
		seen_loose.insert(hl);
		seen_codes.insert(hc);
		// STABLE POINTS: the idle prompt is the only instant where the board is
		// formed and where two states with the same board ought to be the same node.
		if(prompt_type == MSG_SELECT_IDLECMD) {
			++stats.states_idle;
			seen_idle.insert(board_scratch.hash);
			stats.boards_idle = seen_idle.size();
			// ATTRIBUTION: the same nodes, counted per key component. It is the gap
			// between these four numbers that names the culprit; fixing without that
			// reading would be a bet.
			const DigestParts dp =
				StateDigestParts(duel, prompt_type, prompt_payload);
			dz_set.insert(dp.zones);
			dzs_set.insert(dp.zones_sorted);
			dp_set.insert(dp.with_payload);
			dpr_set.insert(dp.procstate);
			df_set.insert(dp.full);
			stats.d_zones = dz_set.size();
			stats.d_zsort = dzs_set.size();
			stats.d_payload = dp_set.size();
			stats.d_proc = dpr_set.size();
			stats.d_full = df_set.size();
		}
		stats.boards_entries = seen_boards.size();
		stats.boards_loose = seen_loose.size();
		stats.boards_codes = seen_codes.size();
	}
	if(fresh && depth < stats.distinct_by_depth.size())
		++stats.distinct_by_depth[depth];

	arena.Push();
	for(const Choice& c : choices) {
		duel.SetResponse(c.response);
		path.push_back(c.response);
		Descend(depth + 1, total_actions, prompt_depth + 1);
		path.pop_back();
		arena.Restore();
		if(BudgetExhausted())
			break;
	}
	arena.Pop();
}

// DISTANCE OVER THE RECIPE GRAPH.
//
// Sum, over the target board cards NOT YET PLACED, of the minimum number of
// summons still to be made according to the observed recipes. The intermediate
// materials count: that is what makes the distance DECREASE mid-line, where
// the flat `h` does not move until a target card is placed.
//
// THE PRESENCE TEST IS ZONE-AWARE, and that is rule 1: "Leo Dancer in the
// GRAVEYARD" is not satisfied by a Leo Dancer in the extra deck. The scan
// costs ONE zone query per call (graveyard + banished), and is only done in
// RunLevin, when expanding a node, never in the rollouts.
//
// FLOOR COST, NEVER INFINITE (rule 2): a product with no known recipe is worth
// 1. So an empty graph returns exactly today's `h`, and enabling it cannot
// make a goal unreachable.
// How many present entities satisfy a requirement, and, in DistanceLocked's
// CLAIMING frame (the top recipe and its direct materials), which entities are
// already SERVED: one body can no longer be both the named material and one of
// the "3 Lunalight monsters" of the same summon. Binary search over the sorted
// table for a named card, a linear scan for a cardinal requirement (the table
// is sorted neither on the archetype nor on the level).
//
// AT NAMESPACE LEVEL AND TEMPLATED. Two callers share it: `RecipeDistance`
// (finisher, full table) and `RecipeEval` (rollouts, table reduced to the
// required zones). They MUST have exactly the same claiming semantics,
// otherwise the distance measured in the rollouts and the finisher's would not
// be the same quantity and the A/B would compare two definitions. Templated
// because `Search::PresentInfo` is a private type: the instantiation happens
// from the members.
template<typename Info>
struct PresentAvailT {
	const std::vector<Requirement>& present;
	const std::vector<Info>& info;     // parallel to `present`
	std::vector<uint8_t>& claimed;     // parallel to `present`
	static bool ZoneOk(const Requirement& r, uint8_t z) {
		// JOKER zone (a requirement seeded from text): any zone a material can be
		// TAKEN from. The deck and the extra are not among them: a card sleeping
		// there still has to be summoned, and that is the step being counted.
		return r.zone == kZoneAny ? ZoneIsPlayable(z) : z == r.zone;
	}
	bool CardinalMatch(const Requirement& r, size_t i) const {
		const Info& pi = info[i];
		if(!pi.row || !(pi.row->type & kRecipeTypeMonster))
			return false;
		if(!ZoneOk(r, pi.zone))
			return false;
		if(r.kind == kReqLevel)
			return (pi.row->level & 0xff) == r.code;
		for(uint16_t sc : pi.row->setcodes)
			if(sc && SetcodeMatches(sc, static_cast<uint16_t>(r.code)))
				return true;
		return false;
	}
	size_t NamedFirst(const Requirement& r) const {
		auto by_code = [](const Requirement& a, const Requirement& b) {
			if(a.code != b.code) return a.code < b.code;
			return a.zone < b.zone;
		};
		auto it = std::lower_bound(present.begin(), present.end(),
								   Requirement{ r.code, 0 }, by_code);
		return static_cast<size_t>(it - present.begin());
	}
	// SHARED counting (non-claiming frames): the old behaviour.
	uint32_t Count(const Requirement& r) const {
		if(r.kind != kReqCard) {
			uint32_t n = 0;
			for(size_t i = 0; i < info.size(); ++i)
				n += CardinalMatch(r, i) ? 1u : 0u;
			return n;
		}
		for(size_t i = NamedFirst(r);
			i < present.size() && present[i].code == r.code; ++i)
			if(ZoneOk(r, present[i].zone))
				return 1u;
		return 0u;
	}
	// Claiming a named copy: the first one that fits and is not already
	// served.
	bool Claim(const Requirement& r) {
		for(size_t i = NamedFirst(r);
			i < present.size() && present[i].code == r.code; ++i)
			if(!claimed[i] && ZoneOk(r, present[i].zone)) {
				claimed[i] = 1;
				return true;
			}
		return false;
	}
	// Claiming cardinal: counts the unserved entities and serves up to
	// `count` of them.
	uint32_t CountAndClaim(const Requirement& r) {
		uint32_t n = 0, taken = 0;
		for(size_t i = 0; i < info.size(); ++i) {
			if(claimed[i] || !CardinalMatch(r, i))
				continue;
			++n;
			if(taken < r.count) {
				claimed[i] = 1;
				++taken;
			}
		}
		return n;
	}
	void ResetClaims() { std::fill(claimed.begin(), claimed.end(), 0); }
};

float Search::RecipeDistance(const BoardKey& here, uint64_t resolved,
							 uint32_t probe_code, uint32_t* d_more) {
	if(!cfg.recipes) {
		if(d_more)
			*d_more = 0;
		return 0.0f;
	}
	prof::Scope ps(prof::kRecipe);
	const auto con = static_cast<uint8_t>(cfg.target_player);
	// Hidden zones, scanned once per call. The field comes from `here.codes`,
	// already computed by the caller: nothing to pay again.
	recipe_present.clear();
	recipe_present_info.clear();
	// Field: already computed by the caller (`here.codes`), nothing to pay again.
	for(uint32_t code : here.codes)
		recipe_present.push_back({ code, NormalizeZone(LOCATION_MZONE) });
	// Hidden zones: one query each. That is the mechanism's cost, and it is
	// ONLY paid in RunLevin, when expanding a node, never in the rollouts,
	// where it would be prohibitive.
	// BUFFER overload: the vector overload allocated a std::vector per query,
	// five times per expanded node.
	static thread_local std::vector<QueriedCard> rq;
	for(uint32_t loc : { LOCATION_GRAVE, LOCATION_REMOVED, LOCATION_HAND,
						 LOCATION_EXTRA, LOCATION_DECK }) {
		duel.Query(con, loc, QUERY_CODE | QUERY_ALIAS, rq);
		for(const QueriedCard& c : rq)
			if(c.present)
				recipe_present.push_back(
					{ duel.Db().Canonical(c.Code()),
					  NormalizeZone(static_cast<uint8_t>(loc)) });
	}
	std::sort(recipe_present.begin(), recipe_present.end(),
			  [](const Requirement& a, const Requirement& b) {
				  if(a.code != b.code) return a.code < b.code;
				  return a.zone < b.zone;
			  });
	// What each entity IS: level, archetypes, monster or not. One database lookup
	// per present entity (~60-80 per expanded node) instead of one per cardinal
	// requirement evaluated.
	if(cfg.recipes->HasCardinal()) {
		recipe_present_info.resize(recipe_present.size());
		for(size_t i = 0; i < recipe_present.size(); ++i)
			recipe_present_info[i] = { duel.Db().Find(recipe_present[i].code),
									   recipe_present[i].zone };
	}
	static thread_local std::vector<uint8_t> claim_scratch;
	claim_scratch.assign(recipe_present.size(), 0);
	// `info` must be parallel to `present` even without cardinals: the claims
	// index both tables with the same index.
	if(!cfg.recipes->HasCardinal())
		recipe_present_info.assign(recipe_present.size(), PresentInfo{});
	PresentAvailT<PresentInfo> avail{ recipe_present, recipe_present_info,
									  claim_scratch };

	// Missing target cards: they are what the distance bears on.
	std::vector<uint32_t> missing;
	{
		std::vector<uint32_t> have = here.codes;
		for(uint32_t t : target.codes) {
			auto it = std::find(have.begin(), have.end(), t);
			if(it != have.end())
				have.erase(it);
			else
				missing.push_back(t);
		}
	}
	// ONE call: the graph's lock and the memo are taken once for all the missing
	// cards.
	const uint32_t total = cfg.recipes->DistanceAll(
		missing, NormalizeZone(LOCATION_MZONE), avail);
	// REPETITION PROBE: "how many summons for ONE MORE copy". The FRESH copy is
	// removed from the field before asking, otherwise `Claim` would find it
	// present and return 0, i.e. "you have one", which is not what is being asked.
	// The removal happens after the computation above so that the distance to the
	// REST of the target bears on the real state. The erasure preserves the sort,
	// and the three parallel tables (present / info / claims) lose the same index.
	if(d_more) {
		*d_more = 0;
		if(probe_code) {
			const uint8_t onfield = NormalizeZone(LOCATION_MZONE);
			for(size_t i = 0; i < recipe_present.size(); ++i)
				if(recipe_present[i].code == probe_code &&
				   recipe_present[i].zone == onfield) {
					recipe_present.erase(recipe_present.begin() + i);
					if(i < recipe_present_info.size())
						recipe_present_info.erase(
							recipe_present_info.begin() + i);
					if(i < claim_scratch.size())
						claim_scratch.erase(claim_scratch.begin() + i);
					break;
				}
			claim_scratch.assign(recipe_present.size(), 0);
			static thread_local std::vector<uint32_t> one;
			one.assign(1, probe_code);
			*d_more = cfg.recipes->DistanceAll(one, onfield, avail);
		}
	}
	// The required resolutions stay counted as before: the graph only models
	// SUMMONS.
	(void)resolved;
	return static_cast<float>(total);
}

// RECIPE GRAPH SNAPSHOT: the safe boundary.
//
// The shared graph learns DURING the run, so it carries a mutex, so it is
// forbidden to the rollouts: sixteen workers taking it at every decision
// serialise the search. So it is copied locally every
// `cfg.recipe_snap_period` rollouts, and the three objects the mechanisms
// consume are derived from it in one go. Called AT DEPTH 0 of a rollout, never
// mid-line: `cfg.enumeration.assign_useful` points into `snap_useful`, which a
// prompt in flight must not see change.
void Search::RecipeSnapshot() {
	if(!cfg.recipes)
		return;
	cfg.recipes->CopyInto(recipe_snap);
	snap_zone_mask = recipe_snap.ZoneMask();
	// The roots are the TARGET's cards: the decomposition starts from the goal,
	// which is the whole principle of backward search (Retro*, AO*).
	static thread_local std::vector<uint32_t> roots;
	roots = target.codes;
	std::sort(roots.begin(), roots.end());
	roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
	recipe_snap.Expand(roots, 3, snap_reqs, snap_backward);
	// USEFUL CODES. NAMED requirements enter as they are; CARDINAL requirements
	// ("3 Lunalight monsters") have no code, so they are resolved by sweeping the
	// cards the player OWNS: deck, extra, hand, graveyard, field. A card absent
	// from those five zones cannot serve as a material anyway. That sweep is the
	// only place in the mechanism that touches the card list, and it is paid once
	// per snapshot, never per decision.
	snap_useful.clear();
	// OPERATORS TO PLAY NOW. The criterion is DECLARATIVE and fits in one line: a
	// NAMED requirement whose zone is the FIELD. Only an ACQUISITION edge posts
	// one, since a material is taken from the graveyard, the hand or the reserve,
	// never from "in play". So the list designates exactly the cards whose
	// presence in play UNLOCKS a subgoal, and nothing else.
	snap_operators.clear();
	for(const Requirement& q : snap_reqs)
		if(q.kind == kReqCard) {
			snap_useful.push_back(q.code);
			if(q.zone == 0x0c)
				snap_operators.push_back(q.code);
		}
	for(uint32_t c : roots)
		snap_useful.push_back(c);
	if(recipe_snap.HasCardinal()) {
		static thread_local std::vector<QueriedCard> rq;
		const auto con = static_cast<uint8_t>(cfg.target_player);
		for(uint32_t loc : { LOCATION_DECK, LOCATION_EXTRA, LOCATION_HAND,
							 LOCATION_GRAVE, LOCATION_MZONE }) {
			duel.Query(con, loc, QUERY_CODE | QUERY_ALIAS, rq);
			for(const QueriedCard& qc : rq) {
				if(!qc.present)
					continue;
				const uint32_t code = duel.Db().Canonical(qc.Code());
				const CardRow* row = duel.Db().Find(code);
				if(!row || !(row->type & kRecipeTypeMonster))
					continue;
				for(const Requirement& q : snap_reqs) {
					if(q.kind == kReqCard)
						continue;
					bool hit = false;
					if(q.kind == kReqLevel) {
						hit = (row->level & 0xff) == q.code;
					} else {
						for(uint16_t sc : row->setcodes)
							if(sc && SetcodeMatches(sc,
													static_cast<uint16_t>(q.code))) {
								hit = true;
								break;
							}
					}
					if(hit) {
						snap_useful.push_back(code);
						break;
					}
				}
			}
		}
	}
	std::sort(snap_useful.begin(), snap_useful.end());
	snap_useful.erase(std::unique(snap_useful.begin(), snap_useful.end()),
					  snap_useful.end());
	// The branch to the enumerator only happens under --assign: the other
	// mechanisms read the same snapshot without touching the action space, and
	// that is what makes them separable.
	cfg.enumeration.assign_useful =
		(cfg.assign && !snap_useful.empty()) ? &snap_useful : nullptr;
	std::sort(snap_operators.begin(), snap_operators.end());
	snap_operators.erase(std::unique(snap_operators.begin(),
									 snap_operators.end()),
						 snap_operators.end());
	stats.op_bias_listed = snap_operators.size();
	++stats.recipe_snaps;
	stats.snap_products = recipe_snap.Products();
	stats.snap_useful = snap_useful.size();
	stats.snap_backward = snap_backward.size();
}

// RECIPE DISTANCE AND BACKWARD PROGRESS, on the ROLLOUT path.
//
// Same quantity as `RecipeDistance` (same recursion, same claim table) but
// affordable at every decision, through three savings:
//   - the LOCAL snapshot: the mutex is no longer contended;
//   - the zones: only those a requirement MENTIONS are queried. The finisher
//     scans five, DECK and EXTRA included (~55 entities, half the cost),
//     although no seeded requirement names them (kZoneAny excludes the
//     reserve);
//   - the field comes FREE from `here`, already computed by the caller.
//
// ONE FUNCTION FOR BOTH MEASUREMENTS, because the cost IS the presence scan:
// redoing it for the backward progress would double the only item that
// matters.
uint32_t Search::RecipeEval(const BoardKey& here, uint32_t* backward_out) {
	if(backward_out)
		*backward_out = 0;
	if(!recipe_snap.Products())
		return 0;
	prof::Scope ps(prof::kRecipe);
	const auto con = static_cast<uint8_t>(cfg.target_player);
	recipe_present.clear();
	recipe_present_info.clear();
	// Field: always, and with no mask; it costs nothing (`here.codes` is already
	// computed) and it is the zone the backward progress needs.
	for(uint32_t code : here.codes)
		recipe_present.push_back({ code, NormalizeZone(LOCATION_MZONE) });
	static thread_local std::vector<QueriedCard> rq;
	for(uint32_t loc : { LOCATION_GRAVE, LOCATION_REMOVED, LOCATION_HAND,
						 LOCATION_EXTRA, LOCATION_DECK }) {
		const uint8_t z = NormalizeZone(static_cast<uint8_t>(loc));
		// The values of NormalizeZone are bitwise disjoint: the mask is tested with a
		// plain AND.
		if(!(snap_zone_mask & z))
			continue;
		duel.Query(con, loc, QUERY_CODE | QUERY_ALIAS, rq);
		for(const QueriedCard& c : rq)
			if(c.present)
				recipe_present.push_back({ duel.Db().Canonical(c.Code()), z });
	}
	std::sort(recipe_present.begin(), recipe_present.end(),
			  [](const Requirement& a, const Requirement& b) {
				  if(a.code != b.code) return a.code < b.code;
				  return a.zone < b.zone;
			  });
	recipe_present_info.assign(recipe_present.size(), PresentInfo{});
	if(recipe_snap.HasCardinal())
		for(size_t i = 0; i < recipe_present.size(); ++i)
			recipe_present_info[i] = { duel.Db().Find(recipe_present[i].code),
									   recipe_present[i].zone };
	static thread_local std::vector<uint8_t> claim_scratch;
	claim_scratch.assign(recipe_present.size(), 0);
	PresentAvailT<PresentInfo> avail{ recipe_present, recipe_present_info,
									  claim_scratch };
	// BACKWARD PROGRESS: how many subproducts of the decomposition are already
	// AVAILABLE. Computed BEFORE the distance, which consumes the claims.
	if(backward_out && !snap_backward.empty()) {
		uint32_t done = 0;
		for(uint32_t code : snap_backward) {
			auto it = std::lower_bound(
				recipe_present.begin(), recipe_present.end(),
				Requirement{ code, 0 },
				[](const Requirement& a, const Requirement& b) {
					if(a.code != b.code) return a.code < b.code;
					return a.zone < b.zone;
				});
			for(; it != recipe_present.end() && it->code == code; ++it)
				if(ZoneIsPlayable(it->zone)) {
					++done;
					break;
				}
		}
		*backward_out = done;
	}
	static thread_local std::vector<uint32_t> missing;
	missing.clear();
	{
		static thread_local std::vector<uint32_t> have;
		have = here.codes;
		for(uint32_t t : target.codes) {
			auto it = std::find(have.begin(), have.end(), t);
			if(it != have.end())
				have.erase(it);
			else
				missing.push_back(t);
		}
	}
	return recipe_snap.DistanceAll(missing, NormalizeZone(LOCATION_MZONE),
								   avail);
}

// HINDSIGHT: the substitute goals of ONE rollout.
//
// Andrychowicz et al., NeurIPS 2017: a failure relabelled by the goal it
// ACTUALLY reached. So the stored score is NOT the rollout's (which judges the
// old goal) but the cost of THIS achievement, as early and as short as
// possible. Without that relabelling we would keep, for "how to pay for
// Perfume Dancer", the line with the most material overall: exactly the wrong
// example.
void Search::HindsightCommit(const NrpaRun& run,
							 const std::vector<HindsightHit>& hits) {
	if(hits.empty() || run.steps.empty())
		return;
	// BREAKDOWN: hindsight is suspected of reinforcing EARLY fusions, the ones
	// that spend Wolf's quota on Tiger, the choice trap the script names. We
	// MEASURE before fixing: substitute goals kept with a tracked quota already
	// spent, against fresh quotas. If the bias is real, the fix will be a
	// conditioning of the credit, never a pruning (rule 2).
	if(QuotaKey())
		stats.hindsight_quota_spent += hits.size();
	else
		stats.hindsight_quota_fresh += hits.size();
	for(const auto& h : hits) {
		const double s = 1e12 - static_cast<double>(h.depth) * 1e5 -
						 static_cast<double>(run.steps.size());
		auto it = hindsight.find(h.code);
		if(it == hindsight.end()) {
			if(hindsight.size() >= cfg.hindsight_k)
				continue;
			HindsightGoal g;
			g.score = s;
			// `steps` ONLY: it is all AdaptRun consumes, and copying `flat`
			// (online mining) would double the mechanism's memory for
			// nothing.
			g.run.steps = run.steps;
			// THE PEAK OF THAT GOAL: the steps following the summon are not
			// part of "how to pay for this Fusion". The ROLLOUT's peak would
			// be the old goal's, and using it here would teach something
			// other than what the relabelling promises.
			g.run.peak_steps = h.steps;
			hindsight.emplace(h.code, std::move(g));
			++stats.hindsight_goals;
		} else if(s > it->second.score) {
			it->second.score = s;
			it->second.run.steps = run.steps;
			it->second.run.peak_steps = h.steps;
		}
	}
}

// OFFER PROBE: did this prompt OFFER the watched card?
//
// This is the decomposition that was missing: "never summoned" covers "never
// offered" (a STATE failure) and "offered, never taken" (a SAMPLING failure),
// and the two call for opposite work.
//
// EXTRACTED AS A FUNCTION because it has to be called AT TWO PLACES now that
// elision exists: a forced prompt does not come back down into the loop body,
// and counting lower down lost its offers. Measurement of that defect before
// the fix: Leo Dancer went from 14 433 offers to 1 381, a denominator divided
// by ten, hence a conversion rate incomparable between arms. An instrument
// whose denominator moves with the flag it judges judges nothing.
void Search::CountOffers(uint64_t& rep_offered) {
	if(!cfg.probe_repeat || !offer_this_step)
		return;
	for(size_t i = 0; i < ProbeCount(); ++i) {
		if(!(offer_this_step & (1ull << i)))
			continue;
		++stats.rep[i].offer_steps;
		// The prompt TYPE, without which "offered" is indistinguishable from
		// "listed by an extra deck reveal" (see offer_msgs).
		stats.rep[i].offer_msgs |= 1ull << (prompt_type & 63);
		if(const int slot = OfferSlot(prompt_type); slot >= 0)
			++stats.rep[i].offer_by[slot];
		if(!(rep_offered & (1ull << i))) {
			rep_offered |= 1ull << i;
			++stats.rep[i].offer_rollouts;
		}
	}
}

// LANDMARKS: how many achievements are left to make.
//
// WHAT MAKES THE MECHANISM AFFORDABLE IN THE ROLLOUTS. We do not record the
// state, we record the LANDMARK KEYS, a few dozen cells. The field comes for
// free from `here`, already computed by the caller; a hidden zone is only
// queried when a landmark lives there (`ZoneMask`). On benchmark A that is ONE
// query (the graveyard) where RecipeDistance makes five, plus a sort and ~80
// database lookups, which is why the latter only runs in the finisher and this
// one runs everywhere.
//
// RULE 2 (inherited from the recipe graph): this value cuts NOTHING. It enters
// a score, never a life-or-death test.
uint32_t Search::LandmarkRemaining(const BoardKey& here) {
	if(!cfg.landmarks || cfg.landmarks->Empty())
		return 0;
	const LandmarkGraph& g = *cfg.landmarks;
	lm_counts.assign(g.KeyCount(), 0);
	const auto con = static_cast<uint8_t>(cfg.target_player);
	const uint32_t mask = g.ZoneMask();
	constexpr uint8_t kOnField = 0x0c;
	if(mask & LandmarkGraph::ZoneBit(kOnField))
		for(uint32_t code : here.codes) {
			const size_t ix = g.IndexOf(LandmarkGraph::KeyOf(code, kOnField));
			if(ix != SIZE_MAX)
				++lm_counts[ix];
		}
	static thread_local std::vector<QueriedCard> rq;
	// Both sides, and only the zones where a landmark lives: the mask is what
	// makes the bill proportional to what was LEARNED rather than to the number of
	// zones that exist.
	for(int side = 0; side < 2; ++side) {
		const uint8_t who = side ? (con ^ 1) : con;
		const uint8_t tag = side ? 0x80 : 0x00;
		for(uint32_t loc : { LOCATION_GRAVE, LOCATION_REMOVED, LOCATION_HAND }) {
			const uint8_t z = static_cast<uint8_t>(
				NormalizeZone(static_cast<uint8_t>(loc)) | tag);
			if(!(mask & LandmarkGraph::ZoneBit(z)))
				continue;
			duel.Query(who, loc, QUERY_CODE | QUERY_ALIAS, rq);
			for(const QueriedCard& c : rq)
				if(c.present) {
					const size_t ix = g.IndexOf(
						LandmarkGraph::KeyOf(duel.Db().Canonical(c.Code()), z));
					if(ix != SIZE_MAX)
						++lm_counts[ix];
				}
		}
	}
	const uint32_t rem = g.Remaining(lm_counts);
	stats.landmark_h_sum += rem;
	++stats.landmark_h_count;
	return rem;
}

// REPETITION PROBE: the reading at the FIRST summon of the watched product.
// One zone sweep, once per rollout that gets there.
void Search::RepeatProbeFirst(size_t i, const BoardKey& here,
							  uint64_t resolved, uint32_t depth) {
	if(!cfg.recipes || i >= ProbeCount())
		return;
	RepeatProbe& rp = stats.rep[i];
	const uint32_t code = ProbeCode(i);
	rp.code = code;
	rp.known = cfg.recipes->Knows(code);
	uint32_t more = 0;
	const float rest = RecipeDistance(here, resolved, code, &more);
	++rp.more_n;
	rp.more_sum += more;
	rp.rest_sum += rest;
	rp.first_depth_sum += depth;
	rp.more_min = (std::min)(rp.more_min, more);
	rp.more_max = (std::max)(rp.more_max, more);
	// The verdict is in this comparison: a second copy as close as the first was
	// at the start = the material was PRESERVED (a sampling failure); further away
	// = the chain was CONSUMED (an `h` failure). Without a measured reference,
	// `more` alone says nothing.
	if(rp.d0 != 0xffffffffu) {
		if(more <= rp.d0)
			++rp.more_kept;
		else
			++rp.more_lost;
	}
}

uint32_t Search::Heuristic(const BoardKey& here) const {
	// Intersection on the CODES, not on the complete entries: a complete entry
	// includes position and materials, which only arrive at the very end. It would
	// be zero everywhere and guide nothing. The code, on the other hand, gives a
	// gradient: every target board card placed raises the score.
	uint32_t common = CommonCodes(here.codes, target.codes);
	// Bonus for an exact entry match: separates two states that have the same
	// cards but not yet the right materials.
	size_t a = 0, b = 0;
	uint32_t exact = 0;
	while(a < here.entries.size() && b < target.entries.size()) {
		if(here.entries[a] == target.entries[b]) { ++exact; ++a; ++b; }
		else if(here.entries[a] < target.entries[b]) ++a;
		else ++b;
	}

	// The target board's cards are Synchros at the end of the chain: they only
	// arrive after hundreds of decisions. Counting those cards alone leaves the
	// heuristic at zero over the whole climb and guides nothing, which is what
	// made the guided descent fail.
	//
	// What progresses throughout is the MATERIAL: bodies on the field, and
	// monsters in the graveyard ready to be recycled. We count it with a low
	// weight next to the target cards, to steer the climb without ever outweighing
	// the real objective.
	//
	// The bodies are already counted by ComputeBoardKey: that was the hot spot
	// (one redundant zone query per evaluated child).
	Duel& d = const_cast<Duel&>(duel);
	auto con = static_cast<uint8_t>(cfg.target_player);
	uint32_t fodder = d.Count(con, LOCATION_GRAVE);
	return common * 100 + exact * 10 + here.mzone_count * 3 + fodder;
}

uint32_t Search::ResolveProgress(uint64_t resolved) const {
	uint32_t progress = 0;
	for(size_t i = 0; i < cfg.resolve_min.size(); ++i) {
		uint32_t count = static_cast<uint32_t>((resolved >> (16 * i)) & 0xffff);
		progress += (std::min)(count, cfg.resolve_min[i].min_count);
	}
	return progress;
}

bool Search::GoalCheck(const BoardKey& here, uint32_t depth, uint32_t actions,
					   uint64_t resolved) {
	uint32_t common = CommonCodes(here.codes, target.codes);
	// Peak conditioned on complete resolutions: how far do the lines that did ALL
	// the rips climb?
	const uint32_t rp_here = resolve_total ? ResolveProgress(resolved) : 0;
	if(resolve_total && rp_here >= resolve_total &&
	   common > stats.best_overlap_ripped)
		stats.best_overlap_ripped = common;
	// The best JOINT line: lexicographic max (rips, board, SHORT path). The third
	// axis is a lesson paid for: a 3r+3/6 line reached 239 decisions at the end of
	// the turn with NO resources, and the LTS conversion from its backtrack 0 ran
	// out at 34 expansions. At equal rips and board, the short line kept its
	// reserve and its turn, and it is the one that closes. A rare copy (at most a
	// few improvements per search); the path is re-injected through --approach.
	if(rp_here &&
	   (rp_here > stats.best_joint_rp ||
		(rp_here == stats.best_joint_rp &&
		 (common > stats.best_joint_overlap ||
		  (common == stats.best_joint_overlap &&
		   !stats.best_joint_path.empty() &&
		   path.size() < stats.best_joint_path.size()))))) {
		stats.best_joint_rp = rp_here;
		stats.best_joint_overlap = common;
		stats.best_joint_path = path;
	}
	if(common > stats.best_overlap) {
		stats.best_overlap = common;
		stats.best_board = here.codes;
		// The path leading here: a rare copy (at most 8 improvements per search), it
		// is the finisher's raw material. The field detail says what differs when all
		// the codes are there.
		stats.best_path = path;
		auto con = static_cast<uint8_t>(cfg.target_player);
		stats.best_mzone = duel.Query(con, LOCATION_MZONE, kBoardFlags);
		stats.best_szone = duel.Query(con, LOCATION_SZONE, kBoardFlags);
	}
	if(here.mzone_count > stats.best_monsters)
		stats.best_monsters = here.mzone_count;
	// Main goal, or one of the ALTERNATIVE goals (opponent test --fire: the board
	// without the cards sacrificed to answer the threat).
	// Goal by INCLUSION (--target-subset) or by EXACT EQUALITY (default). The
	// entries are sorted on both sides, so inclusion is a linear merge.
	auto reaches = [&](const BoardKey& want) {
		if(!cfg.goal_subset)
			return here == want;
		// Inclusion on the LOOSE entries (zone, code, face): a posted target
		// carries neither materials nor counters, so comparing it with the
		// complete entries could never succeed on an Xyz.
		return std::includes(here.loose.begin(), here.loose.end(),
							 want.loose.begin(), want.loose.end());
	};
	bool alt_hit = false;
	if(!reaches(target)) {
		if(!cfg.target_alts)
			return false;
		for(const BoardKey& a : *cfg.target_alts)
			if(reaches(a)) {
				alt_hit = true;
				break;
			}
		if(!alt_hit)
			return false;
	}
	// Resolution minimums (--resolve): a conforming board that has not resolved
	// what it must is NOT a solution, and the line can still fulfil it further on,
	// so we do not prune: we continue.
	for(size_t i = 0; i < cfg.resolve_min.size(); ++i) {
		uint32_t count = static_cast<uint32_t>((resolved >> (16 * i)) & 0xffff);
		if(count < cfg.resolve_min[i].min_count)
			return false;
	}
	Solution s;
	s.responses = path;
	s.actions = actions;
	s.decisions = depth;
	s.alt = alt_hit;
	auto con = static_cast<uint8_t>(cfg.target_player);
	s.hand_left = duel.Count(con, LOCATION_HAND);
	s.deck_left = duel.Count(con, LOCATION_DECK);
	s.extra_left = duel.Count(con, LOCATION_EXTRA);
	s.burned = duel.Count(con, LOCATION_GRAVE) + duel.Count(con, LOCATION_REMOVED);
	++stats.goal_hits;
	// Cost of the goal reached, readable by the caller (lexicographic NRPA score)
	// even when the recording is refused (duplicate, worse than the set).
	goal_burned = s.burned;
	goal_actions = actions;
	goal_depth = depth;
	const uint64_t ck = CostKey(s.burned, s.actions, s.decisions);
	if(ck < best_cost_key)
		best_cost_key = ck;
	if(s.burned < best_burned_seen) {
		best_burned_seen = s.burned;
		// The B&B bound tightens with every improvement (anytime only).
		if(cfg.anytime && cfg.burn_slack < 255)
			burn_cut = best_burned_seen + cfg.burn_slack;
		// Publication to the other workers (CAS min): an improvement HERE cuts for
		// EVERYONE from the next decision on.
		if(cfg.shared_burn) {
			uint32_t cur = cfg.shared_burn->load(std::memory_order_relaxed);
			while(s.burned < cur &&
				  !cfg.shared_burn->compare_exchange_weak(
					  cur, s.burned, std::memory_order_relaxed)) {}
		}
	}
	if(!cfg.anytime) {
		solutions.push_back(std::move(s));
		return true;
	}
	// Anytime: dedup by path, then replacement of the worst; the set is bounded
	// and the search does not stop.
	uint64_t h = 1469598103934665603ull;
	for(const auto& r : s.responses) {
		for(uint8_t b : r) {
			h ^= b;
			h *= 1099511628211ull;
		}
		h ^= 0xff;
		h *= 1099511628211ull;
	}
	if(!solution_hashes.insert(h).second)
		return true;
	if(solutions.size() < cfg.max_solutions) {
		solutions.push_back(std::move(s));
		return true;
	}
	size_t worst = 0;
	uint64_t worst_key = 0;
	for(size_t i = 0; i < solutions.size(); ++i) {
		const uint64_t k = CostKey(solutions[i].burned, solutions[i].actions,
								   solutions[i].decisions);
		if(k > worst_key) {
			worst_key = k;
			worst = i;
		}
	}
	if(ck < worst_key)
		solutions[worst] = std::move(s);
	return true;
}

bool GuardHolds(Duel& duel, uint8_t con, const std::vector<GuardClause>& clauses,
				const std::vector<uint32_t>& field_codes) {
	// At most one query per zone, and only when a clause mentions it. Buffers
	// reused between calls: the guard is evaluated at every opponent window, and
	// allocating per call was pure waste.
	struct ZoneCache {
		std::vector<uint32_t> codes;
		bool loaded = false;
	};
	static thread_local ZoneCache hand, grave, removed, extra;
	hand.loaded = grave.loaded = removed.loaded = extra.loaded = false;
	// Count of the OPPONENT's banished cards (predicate kind 1), loaded at most
	// once per evaluation, the same cache discipline as the zones.
	int opp_removed = -1;
	auto in_zone = [&](uint32_t loc, ZoneCache& z, uint32_t code) {
		if(!z.loaded) {
			duel.QueryCodes(con, loc, z.codes);
			for(uint32_t& c : z.codes)
				c = duel.Db().Canonical(c);
			std::sort(z.codes.begin(), z.codes.end());
			z.loaded = true;
		}
		return std::binary_search(z.codes.begin(), z.codes.end(), code);
	};
	auto atom_holds = [&](const GuardAtom& a) {
		if(a.kind == 1) {
			if(opp_removed < 0)
				opp_removed = static_cast<int>(duel.Count(
					static_cast<uint8_t>(1 - con), LOCATION_REMOVED));
			return opp_removed >= static_cast<int>(a.count);
		}
		if(a.zones & (LOCATION_MZONE | LOCATION_SZONE)) {
			if(std::binary_search(field_codes.begin(), field_codes.end(), a.code))
				return true;
		}
		if((a.zones & LOCATION_HAND) && in_zone(LOCATION_HAND, hand, a.code))
			return true;
		if((a.zones & LOCATION_GRAVE) && in_zone(LOCATION_GRAVE, grave, a.code))
			return true;
		if((a.zones & LOCATION_REMOVED) && in_zone(LOCATION_REMOVED, removed, a.code))
			return true;
		if((a.zones & LOCATION_EXTRA) && in_zone(LOCATION_EXTRA, extra, a.code))
			return true;
		return false;
	};
	for(const GuardClause& clause : clauses) {
		bool all = true;
		for(const GuardAtom& a : clause)
			if(!atom_holds(a)) { all = false; break; }
		if(all)
			return true;
	}
	return clauses.empty();
}

bool Search::GuardCut(const BoardKey& here, uint32_t summons) {
	if(cfg.guard_clauses.empty() || summons < cfg.guard_after)
		return false;
	// Only the windows where the OPPONENT can act count: that is where Nibiru
	// would land. The player's intermediate states, however stripped, are out of
	// the opponent's reach.
	if(prompt_player == cfg.target_player || prompt_player < 0)
		return false;
	// Threat gone: the opponent's hand has been emptied enough (handrip).
	if(cfg.guard_opp_hand_release >= 0 &&
	   static_cast<int>(duel.Count(static_cast<uint8_t>(1 - cfg.target_player),
								   LOCATION_HAND)) <= cfg.guard_opp_hand_release)
		return false;
	if(GuardHolds(duel, static_cast<uint8_t>(cfg.target_player),
				  cfg.guard_clauses, here.codes))
		return false;
	++stats.guard_cuts;
	return true;
}

bool Search::SummonsOk(uint32_t before) const {
	if(cfg.summon_constraints.empty() || summons_this_step.empty())
		return true;
	for(size_t i = 0; i < summons_this_step.size(); ++i) {
		auto it = cfg.summon_constraints.find(
			before + static_cast<uint32_t>(i) + 1);
		if(it == cfg.summon_constraints.end())
			continue;
		// Canonical code: the constraint designates a card, not an artwork. A
		// code of 0 (summoned face down) cannot satisfy any constraint, which is
		// conservative and moot in practice.
		uint32_t canon = duel.Db().Canonical(summons_this_step[i]);
		bool ok = false;
		for(uint32_t allowed : it->second)
			if(allowed == canon) { ok = true; break; }
		if(!ok)
			return false;
	}
	return true;
}

void Search::ArchiveObserve(const BoardKey& here, uint32_t depth,
							uint64_t resolved) {
	if(!cfg.archive_k)
		return;
	const uint32_t overlap = CommonCodes(here.codes, target.codes);
	const uint32_t rp = ResolveProgress(resolved);
	// THE CELL IS SUBGOAL PROGRESS, NOT A BOARD HASH.
	//
	// This is the missing half of SIW_R. Reopening the novelty table changes what
	// counts as new; only the RETURN makes each block restart from the previous
	// one, and that is what turns `5.9^110` into `14 x 5.9^8`.
	//
	// Go-Explore calls "cell" a class of states of which only the best is kept.
	// Indexing by the board hash gives one cell PER STATE: the archive becomes a
	// cache, never a ladder. Indexed by progress, it keeps the BEST state of each
	// rung, and the finisher, which already takes its roots from the sorted
	// archive, therefore restarts from every rung.
	uint64_t sp_vec = 0;
	const uint32_t sp = SerialProgress(duel,
									   static_cast<uint8_t>(cfg.target_player),
									   cfg.serial_reqs, duel.Db(), serial_res0,
									   &sp_vec);
	// THE REFINED LADDER: beyond the gate, the sub-rungs count in the score
	// and the key extends by one level, so two states at the same base rung
	// but with different sub-progress stop sharing a representative. Cost: a
	// second SerialProgress, only near the frontier.
	uint32_t sp2 = 0;
	uint64_t sp2_vec = 0;
	if(refined && sp >= refine_gate_sp && !refine_reqs.empty()) {
		sp2 = SerialProgress(duel, static_cast<uint8_t>(cfg.target_player),
							 refine_reqs, duel.Db(), serial_res0, &sp2_vec);
		if(sp2)
			++stats.refine_top_hits;
	}
	// RESOLUTION RUNGS. The disciplined bare run on benchmark B climbed to 6/6
	// of the board WITHOUT a single rip, and to >=2 rips with no board: the
	// score ranked ladder progress (bits 56+) above the resolutions (bits
	// 44-48), so "board+1 with no rip" dominated "board-1 with 2 rips" and the
	// frontier climbed while ignoring the handrip. Required resolutions
	// (--resolve) are SUBGOALS just as much as the balance sheet's rungs: they
	// enter ladder progress AND the cell key, so two states with the same board
	// but different rips stop sharing a representative, and the return to the
	// rung also re-enters ripped cells (the prefix replay re-accumulates
	// `resolved`). Scope guard: nothing without armed serialisation and
	// --resolve, so benchmark A and every rip-free mode are unchanged byte for
	// byte.
	uint32_t rp_rungs = 0;
	uint64_t rvec = 0;
	if(!cfg.serial_reqs.empty() && !cfg.resolve_min.empty()) {
		rp_rungs = rp;
		for(size_t i = 0; i < cfg.resolve_min.size() && i < 4; ++i) {
			const uint32_t got = (std::min)(
				static_cast<uint32_t>((resolved >> (16 * i)) & 0xffff),
				cfg.resolve_min[i].min_count);
			rvec |= static_cast<uint64_t>((std::min)(got, 15u)) << (4 * i);
		}
	}
	const uint32_t sp_eff = (std::min)(sp + sp2 + rp_rungs, 127u);
	// Score: under --resolve, the RESOLUTIONS first. The measured lock is the
	// rips+board junction, and the roots that cross it are the states already
	// ripped, not the mute 8/8 ones. At equal rank, the SHORTEST path (more
	// depth budget for the finisher). In ANYTIME mode, a LOW partial cost comes
	// before the short path: the cells useful to the optimisation are "board
	// close + burned low", not only "overlap high".
	uint32_t burned = 0;
	auto pack = [&](uint32_t b) -> uint64_t {
		const uint64_t tail = 0xFFFFFFFFull - (std::min)(depth, 0xFFFFFF00u);
		if(!cfg.anytime)
			return cfg.resolve_min.empty()
				? (static_cast<uint64_t>(overlap) << 40) | tail
				: (static_cast<uint64_t>((std::min)(rp, 15u)) << 44) |
					  (static_cast<uint64_t>((std::min)(overlap, 255u)) << 36) |
					  tail;
		const uint64_t bb = 0xFFull - (std::min)(b, 0xFFu);
		return cfg.resolve_min.empty()
			? (static_cast<uint64_t>(overlap) << 40) | (bb << 32) | tail
			: (static_cast<uint64_t>((std::min)(rp, 15u)) << 48) |
				  (static_cast<uint64_t>((std::min)(overlap, 255u)) << 40) |
				  (bb << 32) | tail;
	};
	// Common case free: the OPTIMISTIC score (0 burned) below the floor avoids
	// the two zone queries of the real count.
	auto floor_of = [&](void) -> uint64_t {
		return archive.size() >= cfg.archive_k ? archive_min_score : 0ull;
	};
	// Progress comes BEFORE overlap: a cell that has crossed an intermediate
	// subgoal is worth more than one that placed a target card by chance.
	// Without serialisation, `sp` is 0 and the ranking is the previous one, so
	// the mechanism cannot degrade the control arm.
	auto pack_sp = [&](uint32_t b) -> uint64_t {
		const uint64_t base = pack(b);
		if(cfg.serial_reqs.empty())
			return base;
		// THE REPRESENTATIVE WITH FRESH RESOURCES. At EQUAL progress and
		// overlap, a cell that has not spent its quotas dominates: the same
		// rungs reached with more options left. It is the same defect as the
		// "short path" one, seen from the SCORE side: the (board, quotas) key
		// separates the cells, but eviction and the re-entry tournament still
		// compared without looking at what is left to spend. 4 bits between the
		// progress (56-62) and the rest of the score (base >> 8 peaks at bit
		// 43): no collision. Compares two states of the SAME cell, where quotas
		// are equal by construction of the key, so the criterion is neutral
		// there.
		uint64_t fresh = 0;
		if(cfg.quota_fresh_pref && !cfg.quota_hosts.empty()) {
			const uint64_t used_mask =
				(1ull << (std::min)(cfg.quota_hosts.size(), size_t(12))) - 1;
			fresh = (std::min<uint64_t>)(
				std::bitset<12>((~QuotaKey()) & used_mask).count(), 15u);
		}
		// 7 bits: the total served exceeds 63 with the reserve-departure
		// slices, and 127 << 56 fits in the low 63 bits. `sp_eff` includes the
		// refined sub-rungs: the refined frontier MUST outrank the base
		// frontier.
		return (static_cast<uint64_t>((std::min)(sp_eff, 127u)) << 56) |
			   (fresh << 52) | (base >> 8);
	};
	uint64_t score = pack_sp(0);
	if(uint64_t fl = floor_of(); fl && score <= fl)
		return;
	if(cfg.anytime) {
		burned = CurrentBurned();
		score = pack_sp(burned);
		if(uint64_t fl = floor_of(); fl && score <= fl)
			return;
	}
	// The key: the progress rung when serialisation is on, otherwise the board
	// hash (the previous behaviour, byte for byte). The QUOTAS enter the key: two
	// states with the same vector but different quotas stop sharing a
	// representative. That was the invisible member of the conjunction, and the
	// "short path" representative was systematically the state that had not paid.
	//
	// THE GRID (cfg.grid, the regime without serialisation): the cell IS
	// (resolutions, overlap), at most 28 cells with one elite each. The scalar
	// score no longer decides WHICH families survive (the scalarisation theorem:
	// it only kept the extremes of the front); it only breaks ties WITHIN a cell,
	// where rips and overlap are fixed, and there it is exactly the "shallowest at
	// equal progress" the derivation prescribes (the depth tail of the score).
	const uint64_t cell =
		cfg.grid && cfg.serial_reqs.empty() && !cfg.resolve_min.empty()
			? (0x6A1DBA5E00000000ull |
			   (static_cast<uint64_t>((std::min)(rp, 15u)) << 8) |
			   (std::min)(overlap, 255u))
			: cfg.serial_reqs.empty()
				  ? here.hash
				  : (0x5E21A1000000000ull ^ sp_vec ^ (QuotaKey() << 52) ^
					 (sp2_vec * 0x9E3779B97F4A7C15ull) ^
					 (rvec * 0xA24BAED4963EE407ull));
	auto it = archive_cells.find(cell);
	if(it != archive_cells.end()) {
		ArchiveEntry& e = archive[it->second];
		if(score <= e.score)
			return;
		e.score = score;
		e.overlap = overlap;
		e.resolves = rp;
		e.decisions = depth;
		e.burned = burned;
		e.path = path;
	} else if(archive.size() < cfg.archive_k) {
		archive_cells.emplace(cell, archive.size());
		archive.push_back({ cell, score, overlap, rp, depth, burned, path });
	} else {
		size_t worst = 0;
		for(size_t i = 1; i < archive.size(); ++i)
			if(archive[i].score < archive[worst].score)
				worst = i;
		if(score <= archive[worst].score)
			return;
		archive_cells.erase(archive[worst].cell);
		archive_cells.emplace(cell, worst);
		// THE STORED KEY IS `cell`, NOT `here.hash`. The entry used to carry the
		// board hash although the `archive_cells` map is indexed by the progress
		// rung: from the first eviction under serialisation,
		// `erase(archive[worst].cell)` targeted an ABSENT key, the real key stayed
		// and pointed at an entry of another cell, and the ladder corrupted itself
		// silently, in exactly the regime (a full archive) where a bare run lives.
		archive[worst] = { cell, score, overlap, rp, depth, burned, path };
	}
	archive_min_score = ~0ull;
	for(const ArchiveEntry& e : archive)
		archive_min_score = (std::min)(archive_min_score, e.score);
	// While the archive is not full, every state deserves to enter it, so the
	// floor is zero, which CANCELS the cheap early-out at the top of the function
	// for the whole filling phase. That is intended (otherwise we would refuse
	// states while cells are still free) but it has to be read that way: the
	// early-out only works on a full archive.
	if(archive.size() < cfg.archive_k)
		archive_min_score = 0;
}

// SEEDING. The entries may or may not arrive sorted from the caller, so we
// sort here, best first, and keep only cfg.archive_k: seeding beyond that
// would be evicted at the first observation. The seeded scores date from the
// previous round (same ladder: same serial_reqs, same target); a cell
// re-observed this round takes the live score through ArchiveObserve's normal
// path.
size_t Search::SeedArchive(const std::vector<ArchiveEntry>& seed) {
	if(!cfg.archive_k || seed.empty())
		return 0;
	std::vector<const ArchiveEntry*> order;
	order.reserve(seed.size());
	for(const ArchiveEntry& e : seed)
		if(!e.path.empty())
			order.push_back(&e);
	std::sort(order.begin(), order.end(),
			  [](const ArchiveEntry* a, const ArchiveEntry* b) {
				  return a->score > b->score;
			  });
	if(order.size() > cfg.archive_k)
		order.resize(cfg.archive_k);
	size_t added = 0;
	for(const ArchiveEntry* e : order) {
		auto it = archive_cells.find(e->cell);
		if(it != archive_cells.end()) {
			if(e->score > archive[it->second].score)
				archive[it->second] = *e;
			continue;
		}
		if(archive.size() >= cfg.archive_k)
			break;
		archive_cells.emplace(e->cell, archive.size());
		archive.push_back(*e);
		++added;
	}
	archive_min_score = ~0ull;
	for(const ArchiveEntry& e : archive)
		archive_min_score = (std::min)(archive_min_score, e.score);
	if(archive.size() < cfg.archive_k)
		archive_min_score = 0;
	return added;
}

bool Search::NoveltyCut(const BoardKey& here, uint32_t depth, uint64_t resolved,
						uint32_t& stale) {
	if(!cfg.novelty_patience)
		return false;
	// Partition on BOTH dimensions of the goal: target cards placed AND required
	// resolutions done. Each rip reopens the table, like each card placed.
	uint32_t partition = cfg.novelty_serialize
		? CommonCodes(here.codes, target.codes) * 16u + ResolveProgress(resolved)
		: 0u;
	// SERIALISATION BY x*: progress on the material balance's subgoals replaces
	// the count of target cards alone. It moves from the first brick placed,
	// where the other stays flat until the end.
	// 7 bits: with the reserve-departure slices the total served can exceed 63,
	// and a 6-bit mask would ALIAS the high rungs onto the low ones.
	if(!cfg.serial_reqs.empty())
		partition = partition * 128u +
					(SerialProgress(duel, static_cast<uint8_t>(cfg.target_player),
									cfg.serial_reqs, duel.Db(), serial_res0) &
					 127u);
	CollectAtoms(duel, static_cast<uint8_t>(cfg.target_player), here, partition,
				 atoms_scratch);
	if(novelty.Observe(atoms_scratch, depth, cfg.novelty_strict)) {
		++stats.novelty_novel;
		stale = 0;
		return false;
	}
	++stats.novelty_stale;
	if(++stale > cfg.novelty_patience) {
		++stats.novelty_cuts;
		return true;
	}
	return false;
}

// Returns true as soon as a solution is found and there is reason to stop.
bool Search::DescendGuided(uint32_t depth, uint32_t actions, uint32_t turns,
						   uint32_t summons, uint64_t resolved) {
	if(BudgetExhausted()) {
		stats.hit_time_limit = true;
		return false;
	}
	Step st = StepToPrompt();
	uint32_t total_actions = actions + actions_this_step;
	uint32_t total_turns = turns + turns_this_step;
	if(st == Step::Rejected) { ++stats.dead_ends; return false; }
	if(st == Step::Ended)    { ++stats.terminals; return false; }
	if(!SummonsOk(summons) || material_violation) {
		++stats.constraint_cuts;
		return false;
	}
	uint32_t total_summons =
		summons + static_cast<uint32_t>(summons_this_step.size());
	uint64_t total_resolved = resolved + resolved_this_step;

	++stats.nodes;
	prof::Count(prof::kDecisions);
	if(depth < stats.expansions_by_depth.size())
		++stats.expansions_by_depth[depth];

	ComputeBoardKeyInto(duel, static_cast<uint8_t>(cfg.target_player),
						board_scratch);
	const BoardKey& here = board_scratch;
	if(GoalCheck(here, depth, total_actions, total_resolved)) {
		if(!cfg.anytime)
			return solutions.size() >= cfg.max_solutions;
		// Anytime: DO NOT stop at the goal; the descent continues under the
		// relaxed bounds. POST-goal recoveries (a graveyard effect shuffling
		// cards back into the deck) reduce the burned count without touching
		// the board, and every re-reaching is re-recorded when it is cheaper.
		// That is the class of lines that stopping at the goal made
		// structurally unfindable.
	}
	// The target board is the one at the end of turn 1: past that point the
	// player's board is frozen and the whole descent below is wasted time.
	// `max_turns`: the reference's second Omega rip LIVES in the OPPONENT's turn
	// (Omega banishes itself on the first rip and comes back at the opposing
	// standby), so `--resolve Omega:2` was structurally outside the domain and no
	// budget could cross it. `--turns 2` lets the line cross the opponent's turn,
	// where our only decisions are the quick windows (rip, guard); the default of
	// 1 is the historical behaviour byte for byte.
	if(total_turns >= cfg.max_turns + 1) {
		++stats.turn_cuts;
		return false;
	}
	if(GuardCut(here, total_summons))
		return false;
	ArchiveObserve(here, depth, total_resolved);

	if(depth >= cfg.max_decisions) {
		++stats.edges_skipped;
		return false;
	}
	if(cfg.max_actions && total_actions > cfg.max_actions) {
		++stats.edges_skipped;
		return false;
	}

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

	ChoiceList& choices = ChoicesAt(depth);
	if(!FillChoices(choices)) {
		++stats.dead_ends;
		return false;
	}

	// Child evaluation: advance, measure, restore. That is exactly what the
	// snapshot makes affordable (0.05 ms per return).
	struct Scored { size_t index; uint32_t score; uint32_t acts; bool dead; };
	std::vector<Scored> scored;
	scored.reserve(choices.size());
	arena.Push();
	for(size_t i = 0; i < choices.size(); ++i) {
		duel.SetResponse(choices[i].response);
		Step cs = StepToPrompt();
		bool dead = (cs == Step::Rejected) || !SummonsOk(total_summons) ||
					material_violation;
		uint32_t sc = 0;
		if(!dead) {
			ComputeBoardKeyInto(duel, static_cast<uint8_t>(cfg.target_player),
								child_board_scratch);
			sc = Heuristic(child_board_scratch);
		}
		scored.push_back({ i, sc, actions_this_step, dead });
		arena.Restore();
	}
	// The most promising first; at equal score, the one costing the fewest
	// actions (the solver's cost criterion).
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
		stop = DescendGuided(depth + 1, total_actions, total_turns,
							 total_summons, total_resolved);
		path.pop_back();
		arena.Restore();
		if(stop || BudgetExhausted())
			break;
	}
	arena.Pop();
	return stop;
}

bool Search::DescendRepair(uint32_t depth, uint32_t actions, size_t ref_index,
						   uint32_t disc, uint32_t stale, uint32_t turns,
						   uint32_t summons, uint64_t resolved) {
	if(BudgetExhausted()) {
		stats.hit_time_limit = true;
		return false;
	}
	Step st = StepToPrompt();
	uint32_t total_actions = actions + actions_this_step;
	uint32_t total_turns = turns + turns_this_step;
	if(st == Step::Rejected) { ++stats.dead_ends; return false; }
	if(st == Step::Ended)    { ++stats.terminals; return false; }
	if(!SummonsOk(summons) || material_violation) {
		++stats.constraint_cuts;
		return false;
	}
	uint32_t total_summons =
		summons + static_cast<uint32_t>(summons_this_step.size());
	uint64_t total_resolved = resolved + resolved_this_step;

	++stats.nodes;
	prof::Count(prof::kDecisions);
	if(depth < stats.expansions_by_depth.size())
		++stats.expansions_by_depth[depth];

	ComputeBoardKeyInto(duel, static_cast<uint8_t>(cfg.target_player),
						board_scratch);
	const BoardKey& here = board_scratch;
	if(GoalCheck(here, depth, total_actions, total_resolved)) {
		if(!cfg.anytime)
			return solutions.size() >= cfg.max_solutions;
		// Anytime: DO NOT stop at the goal; the descent continues under the
		// relaxed bounds. POST-goal recoveries (a graveyard effect shuffling
		// cards back into the deck) reduce the burned count without touching
		// the board, and every re-reaching is re-recorded when it is cheaper.
		// That is the class of lines that stopping at the goal made
		// structurally unfindable.
	}
	if(total_turns >= cfg.max_turns + 1) {
		++stats.turn_cuts;
		return false;
	}
	if(GuardCut(here, total_summons))
		return false;
	ArchiveObserve(here, depth, total_resolved);

	if(depth >= cfg.max_decisions) {
		++stats.edges_skipped;
		return false;
	}
	if(cfg.max_actions && total_actions > cfg.max_actions) {
		++stats.edges_skipped;
		return false;
	}

	// Novelty pruning, never on the pure prefix (no deviation taken): that is what
	// guarantees that at zero deviations the reference is found again, and hence
	// that "no solution" stays a defect signal.
	if(disc < cfg_discrepancies && NoveltyCut(here, depth, total_resolved, stale))
		return false;

	// The deviation budget is the binding constraint: that is what we memoise.
	uint64_t key = Digest();
	if(cfg.shared_tt) {
		// Table shared between workers (lazy SMP): a state solved by one prunes for
		// all. `fresh`: see SharedTT::CheckAndClaim; without it `distinct_by_depth`
		// stayed at ZERO in every multi-worker run.
		bool fresh = false;
		if(cfg.shared_tt->CheckAndClaim(key, disc + 1, &fresh)) {
			++stats.transpositions;
			return false;
		}
		if(fresh) {
			if(depth < stats.distinct_by_depth.size())
				++stats.distinct_by_depth[depth];
		} else {
			++stats.tt_reexplored;
		}
	} else {
		auto it = tt.find(key);
		if(it != tt.end() && it->second >= disc + 1) {
			++stats.transpositions;
			return false;
		}
		// ALREADY SEEN BUT WITH LESS BUDGET: the state is RE-EXPANDED. Only the cut
		// used to be counted, never this work.
		if(it != tt.end())
			++stats.tt_reexplored;
		if(it == tt.end() && depth < stats.distinct_by_depth.size())
			++stats.distinct_by_depth[depth];
		tt[key] = disc + 1;
	}

	// SEMANTIC resynchronisation: when this state is a state of the reference
	// FURTHER ALONG its line (same digest, LiftPlan's state matching), we pick the
	// line up there and the recorded suffix becomes readable again. Without it,
	// any deviation that converges (swapping summons #4/#5) left the rest of the
	// answers unreadable: measured unfindable up to k=12.
	if(cfg.ref_digests && disc < cfg_discrepancies) {
		auto rit = cfg.ref_digests->find(key);
		if(rit != cfg.ref_digests->end() && rit->second > ref_index) {
			ref_index = rit->second;
			++stats.resyncs;
		}
	}

	// Candidates: the recorded answer first (zero cost), then the alternatives
	// (one deviation each).
	std::vector<std::pair<const std::vector<uint8_t>*, uint32_t>> cands;
	const std::vector<uint8_t>* recorded = nullptr;
	if(ref_index < yrp.responses.size()) {
		recorded = &yrp.responses[ref_index];
		// The recorded answer does not go through the enumerator: the --no-activate
		// filter has to catch it here, otherwise the reference line would carry the
		// forbidden activation with impunity.
		if(!ResponseForbidden(prompt_type, prompt_payload.data(),
							  static_cast<uint32_t>(prompt_payload.size()),
							  *recorded, cfg.enumeration))
			cands.emplace_back(recorded, 0u);
	}
	// Partitioning between workers: while no deviation has been taken, this worker
	// must claim the point before deviating there. Once the first deviation is
	// spent the subtree belongs to it and the later deviations are unconstrained.
	const uint32_t level = cfg_discrepancies - disc;   // 0 = no deviation taken
	bool mine = true;
	if(cfg.claims && level == cfg.claim_level)
		mine = cfg.claims->Claim(ref_index);

	// WINDOWED repertoire: after a first deviation, a move the reference plays
	// within `repair_window` decisions of the current point is FREE. That is what
	// makes local permutations affordable: swapping summons #4/#5 (~17 decisions
	// apart, one activation between them) used to cost one deviation PER reordered
	// decision (measured unfindable up to k=12) and now costs only one, with the
	// digest resynchronisation hooking the exact suffix back on afterwards. Never
	// on the pure prefix.
	const bool windowed = disc < cfg_discrepancies && cfg.ref_keys &&
						  !cfg.ref_keys->empty();
	ChoiceList& alts = ChoicesAt(depth);
	alts.Clear();
	if((disc > 0 && mine) || windowed) {
		EnumerateInto(prompt_type, prompt_payload.data(),
					  static_cast<uint32_t>(prompt_payload.size()),
					  cfg.enumeration, alts);
		if(prompt_player != cfg.target_player && alts.size() > 1) {
			// The opponent does not play: no deviation on its side.
			alts.Clear();
		}
		size_t wlo = 0, whi = 0;
		if(windowed) {
			wlo = ref_index > cfg.repair_window ? ref_index - cfg.repair_window
												: 0;
			whi = (std::min)(ref_index + cfg.repair_window,
							 cfg.ref_keys->size());
		}
		auto free_move = [&](uint64_t pk) {
			if(!windowed || !pk)
				return false;
			for(size_t j = wlo; j < whi; ++j)
				if((*cfg.ref_keys)[j] == pk)
					return true;
			return false;
		};
		for(const Choice& c : alts) {
			if(recorded && c.response == *recorded)
				continue;
			uint32_t cost = free_move(c.plan_key) ? 0u : 1u;
			// Paying deviations remain subject to the budget and to the partitioning
			// between workers; windowed moves do not.
			if(cost == 1 && (disc == 0 || !mine)) {
				if(disc > 0)
					++stats.claim_denied;
				continue;
			}
			cands.emplace_back(&c.response, cost);
		}
	}
	if(cands.empty())
		return false;
	// Free ones first (the recorded answer stays at the head among them).
	std::stable_sort(cands.begin(), cands.end(),
					 [](const std::pair<const std::vector<uint8_t>*, uint32_t>& a,
						const std::pair<const std::vector<uint8_t>*, uint32_t>& b) {
						 return a.second < b.second;
					 });

	arena.Push();
	bool stop = false;
	for(const auto& [resp, cost] : cands) {
		if(cost > disc)
			continue;
		duel.SetResponse(*resp);
		path.push_back(*resp);
		stop = DescendRepair(depth + 1, total_actions, ref_index + 1, disc - cost,
							 stale, total_turns, total_summons, total_resolved);
		path.pop_back();
		arena.Restore();
		if(stop || BudgetExhausted())
			break;
	}
	arena.Pop();
	return stop;
}

bool Search::DescendTransplant(uint32_t depth, uint32_t actions, uint32_t disc,
							   uint32_t stale, uint32_t turns,
							   uint32_t summons, uint64_t resolved) {
	if(BudgetExhausted()) {
		stats.hit_time_limit = true;
		return false;
	}
	Step st = StepToPrompt();
	uint32_t total_actions = actions + actions_this_step;
	uint32_t total_turns = turns + turns_this_step;
	if(st == Step::Rejected) { ++stats.dead_ends; return false; }
	if(st == Step::Ended)    { ++stats.terminals; return false; }
	if(!SummonsOk(summons) || material_violation) {
		++stats.constraint_cuts;
		return false;
	}
	uint32_t total_summons =
		summons + static_cast<uint32_t>(summons_this_step.size());
	uint64_t total_resolved = resolved + resolved_this_step;

	++stats.nodes;
	prof::Count(prof::kDecisions);
	if(depth < stats.expansions_by_depth.size())
		++stats.expansions_by_depth[depth];

	ComputeBoardKeyInto(duel, static_cast<uint8_t>(cfg.target_player),
						board_scratch);
	const BoardKey& here = board_scratch;
	if(GoalCheck(here, depth, total_actions, total_resolved)) {
		if(!cfg.anytime)
			return solutions.size() >= cfg.max_solutions;
		// Anytime: DO NOT stop at the goal; the descent continues under the
		// relaxed bounds. POST-goal recoveries (a graveyard effect shuffling
		// cards back into the deck) reduce the burned count without touching
		// the board, and every re-reaching is re-recorded when it is cheaper.
		// That is the class of lines that stopping at the goal made
		// structurally unfindable.
	}
	if(total_turns >= cfg.max_turns + 1) {
		++stats.turn_cuts;
		return false;
	}
	if(GuardCut(here, total_summons))
		return false;
	ArchiveObserve(here, depth, total_resolved);

	if(depth >= cfg.max_decisions) {
		++stats.edges_skipped;
		return false;
	}
	if(cfg.max_actions && total_actions > cfg.max_actions) {
		++stats.edges_skipped;
		return false;
	}

	// Novelty pruning. This is where it works hardest: the transposition table
	// only caught 15 % of the states at one deviation, because it merges only
	// IDENTICAL states. The pure repertoire prefix (no deviation taken) stays
	// exempt: the repertoire is the guide, not the explored part.
	if(disc < cfg_discrepancies && NoveltyCut(here, depth, total_resolved, stale))
		return false;

	uint64_t key = Digest();
	if(cfg.shared_tt) {
		// Table shared between workers (lazy SMP): a state solved by one prunes for
		// all. `fresh`: see SharedTT::CheckAndClaim; without it `distinct_by_depth`
		// stayed at ZERO in every multi-worker run.
		bool fresh = false;
		if(cfg.shared_tt->CheckAndClaim(key, disc + 1, &fresh)) {
			++stats.transpositions;
			return false;
		}
		if(fresh) {
			if(depth < stats.distinct_by_depth.size())
				++stats.distinct_by_depth[depth];
		} else {
			++stats.tt_reexplored;
		}
	} else {
		auto it = tt.find(key);
		if(it != tt.end() && it->second >= disc + 1) {
			++stats.transpositions;
			return false;
		}
		// ALREADY SEEN BUT WITH LESS BUDGET: the state is RE-EXPANDED. Only the cut
		// used to be counted, never this work.
		if(it != tt.end())
			++stats.tt_reexplored;
		if(it == tt.end() && depth < stats.distinct_by_depth.size())
			++stats.distinct_by_depth[depth];
		tt[key] = disc + 1;
	}

	ChoiceList& choices = ChoicesAt(depth);
	if(!FillChoices(choices)) {
		++stats.dead_ends;
		return false;
	}

	// Hint visibility, on the systematic search side.
	if(!cfg.hint_cards.empty()) {
		for(const Choice& c : choices) {
			if(c.card && std::find(cfg.hint_cards.begin(), cfg.hint_cards.end(),
								   c.card) != cfg.hint_cards.end()) {
				++stats.hint_seen;
				break;
			}
		}
	}

	// Repertoire: every move the reference played is free, wherever it appeared in
	// its line. A single answer is free too: there is nothing to decide, and
	// charging for it would drain the budget on non-choices.
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
	// Free first, and among those the ones the reference played early: it is the
	// only trace of order the repertoire keeps.
	std::stable_sort(cands.begin(), cands.end(),
					 [](const Cand& a, const Cand& b) {
						 if(a.cost != b.cost) return a.cost < b.cost;
						 return a.rank < b.rank;
					 });

	// Partitioning between workers, on the same principle as RunRepair: we claim
	// at the N-th deviation, not at the first, since deviating early opens a huge
	// subtree the other workers must be able to share.
	//
	// The key is the state DIGEST: there is no linear index here, and in goal-only
	// mode there is not even a line. Two distinct states at the claim level open
	// two disjoint subtrees, and that is the partition.
	const uint32_t level = cfg_discrepancies - disc;
	bool mine = true;
	if(cfg.claims && level == cfg.claim_level)
		mine = cfg.claims->Claim(key);

	arena.Push();
	bool stop = false;
	for(const Cand& c : cands) {
		if(c.cost > disc)
			continue;
		if(c.cost > 0 && !mine) {
			++stats.claim_denied;
			continue;   // subtree taken by another worker
		}
		duel.SetResponse(choices[c.index].response);
		path.push_back(choices[c.index].response);
		stop = DescendTransplant(depth + 1, total_actions, disc - c.cost,
								 stale, total_turns, total_summons,
								 total_resolved);
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
	// Initial counters: a rollout can start in the MIDDLE of a line (finisher: an
	// approach prefix replayed, then sampling).
	uint32_t actions = 0, turns = cfg.initial_turns,
			 summons = cfg.initial_summons;
	uint64_t resolved = cfg.initial_resolved;
	uint32_t rp_prev = ResolveProgress(resolved);
	bool hit = false;

	for(uint32_t depth = 0; depth < cfg.max_decisions; ++depth) {
		if(BudgetExhausted()) {
			stats.hit_time_limit = true;
			return hit;
		}
		Step st = StepToPrompt();
		actions += actions_this_step;
		turns += turns_this_step;
		if(st == Step::Rejected) { ++stats.dead_ends; return hit; }
		if(st == Step::Ended)    { ++stats.terminals; return hit; }
		if(!SummonsOk(summons) || material_violation) {
		++stats.constraint_cuts;
		return hit;
	}
		summons += static_cast<uint32_t>(summons_this_step.size());
		resolved += resolved_this_step;
		++stats.nodes;
		prof::Count(prof::kDecisions);
		if(resolved_this_step) {
			const uint32_t rp = ResolveProgress(resolved);
			for(uint32_t k = rp_prev; k < rp; ++k) {
				if(k < 4)
					++stats.resolve_reached[k];
				else
					++stats.resolve_overflow;   // histogram truncated
			}
			rp_prev = rp;
		}

		ComputeBoardKeyInto(duel, static_cast<uint8_t>(cfg.target_player),
							board_scratch);
		const BoardKey& here = board_scratch;
		if(GoalCheck(here, depth, actions, resolved)) {
			// Anytime: keep going; post-goal recoveries can reduce the burned
			// count, and every re-reaching re-records.
			if(!cfg.anytime)
				return true;
			hit = true;
		}
		// Past the end of turn 1 the board is frozen: continuing the rollout into
		// the opponent's turn can no longer reach anything.
		if(turns >= 2) {
			++stats.turn_cuts;
			return hit;
		}
		if(GuardCut(here, summons))
			return hit;
		if(const uint32_t bcut = EffectiveBurnCut();
		   bcut != UINT32_MAX && CurrentBurned() > bcut) {
			++stats.burn_cuts;
			return hit;
		}
		ArchiveObserve(here, depth, resolved);
		// Rollout-IW without a tree (a rollout that stops producing anything new
		// is cut) used to cut here. Removed: REFUTED, and the cause was
		// measured. Since the table is shared between rollouts, re-walking the
		// same beginning kills the rollout before it could deviate (2/8 instead
		// of 6/8 on the transplantation case).

		ChoiceList& choices = ro_choices;
		if(!FillChoices(choices)) {
			++stats.dead_ends;
			return hit;
		}

		size_t pick = 0;
		if(choices.size() > 1) {
			// Advance, measure, restore: that is what the snapshot makes affordable,
			// and this is where the whole quality of the rollout is decided.
			struct Scored { size_t index; int64_t score; bool known; bool dead; };
			std::vector<Scored> scored;
			scored.reserve(choices.size());
			arena.Push();
			for(size_t i = 0; i < choices.size(); ++i) {
				duel.SetResponse(choices[i].response);
				Step cs = StepToPrompt();
				bool dead = (cs == Step::Rejected) || !SummonsOk(summons) ||
							material_violation;
				int64_t sc = -1;
				if(!dead) {
					ComputeBoardKeyInto(duel,
										static_cast<uint8_t>(cfg.target_player),
										child_board_scratch);
					sc = static_cast<int64_t>(Heuristic(child_board_scratch));
				}
				scored.push_back({ i, sc,
								   plan_index.count(choices[i].plan_key) != 0,
								   dead });
				arena.Restore();
			}
			arena.Pop();

			// The repertoire BREAKS TIES, it does not dominate. As an additive
			// bonus it would put "end the turn", which the reference plays and is
			// therefore known, ahead of a summon that actually advances. And
			// ending the turn is irreversible: the board is frozen.
			std::stable_sort(scored.begin(), scored.end(),
							 [](const Scored& a, const Scored& b) {
								 if(a.score != b.score) return a.score > b.score;
								 return a.known > b.known;
							 });
			// The first rollout is purely greedy; the following ones deviate from
			// it more and more rarely as one goes down the ranking.
			size_t r = 0;
			while(r + 1 < scored.size() && scored[r + 1].score >= 0 &&
				  (next() & 3u) == 0)
				++r;
			if(scored[r].dead) {
				++stats.dead_ends;
				return hit;
			}
			pick = scored[r].index;
		}

		duel.SetResponse(choices[pick].response);
		path.push_back(choices[pick].response);
	}
	// Exit through the TOP of the loop: the decision ceiling bit, not the space.
	// l'espace (C2).
	++stats.edges_skipped;
	return hit;
}

void Search::RunRollouts(const BoardKey& t, const std::vector<PlanStep>& p,
						 uint32_t count, uint64_t seed) {
	InitSerialBase();
	// The self time of this probe is the profile's "everything else" line:
	// whatever the internal probes do not cover (softmax, policy, tables, PQ...).
	prof::Scope ps(prof::kSearch);
	target = t;
	plan = &p;
	plan_index.clear();
	for(size_t i = 0; i < p.size(); ++i)
		if(p[i].edge)
			plan_index.emplace(p[i].edge, i);
	start = std::chrono::steady_clock::now();
	solutions.clear();
	path.clear();
	novelty.Clear();

	arena.Push();     // resume point: the root
	for(uint32_t r = 0; r < count && !BudgetExhausted(); ++r) {
		uint64_t rng = seed + r * 0x9e3779b97f4a7c15ull;
		if(!rng)
			rng = 1;
		path.clear();
		++stats.rollout_count;
		bool hit = Rollout(rng);
		arena.Restore();
		if(hit && !cfg.anytime && solutions.size() >= cfg.max_solutions)
			break;
	}
	arena.Pop();

	stats.ms = std::chrono::duration<double, std::milli>(
				   std::chrono::steady_clock::now() - start).count();
	stats.hit_node_limit = stats.nodes >= cfg.max_nodes;
	stats.exhausted = false;   // a rollout never exhausts anything
	stats.novelty_atoms = novelty.Size();
}

void Search::PolicyRollout(uint64_t& rng, const Policy& pol, NrpaRun& run) {
	auto next = [&rng] {
		rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
		return rng;
	};
	++stats.rollout_count;
	// RETURN TO THE RUNG: a re-entered rollout starts PREFIX INCLUDED, since the
	// solutions and `best_path` must stay replayable from the root. The quota uses
	// of a re-entered rollout were already counted during the prefix replay
	// (ReenterMaybe zeroes them beforehand): do not overwrite them here.
	// ecraser ici.
	if(reenter_active) {
		path = reenter_path;
	} else {
		path.clear();
		std::memset(quota_uses, 0, sizeof quota_uses);
	}
	run.score = 0;
	run.steps.clear();
	run.flat.clear();
	// Every field of NrpaRun must be zeroed here: the same object is reused from
	// one rollout to the next, and a peak leaking through would truncate the NEXT
	// rollout's gradient at an arbitrary point (the same family of defect as
	// `ChoiceList::Emit`).
	run.peak_steps = 0;
	// FLAT line for ONLINE mining: the decisions ABSORBED by a macro do not appear
	// in `steps`, and re-mining on that would manufacture macros of macros, which
	// abort on the first step (see NrpaRun::flat).
	const bool mine_flat = cfg.options_online != nullptr;
	// Initial counters: the finisher samples from an already deep backtrack state,
	// so summon constraints, the turn cut-off and the resolution gradient must
	// count from the prefix rather than from zero. The RETURN TO THE RUNG does the
	// same per rollout: its counters come from replaying the cell (same
	// StepToPrompt, same bookkeeping), `actions` included. Without it the
	// lexicographic cost (burned, actions, decisions) would compare re-entered
	// lines shorn of their prefix against complete lines.
	uint32_t actions = reenter_active ? reenter_actions : 0,
			 turns = reenter_active ? reenter_turns : cfg.initial_turns,
			 summons = reenter_active ? reenter_summons : cfg.initial_summons;
	uint64_t resolved =
		reenter_active ? reenter_resolved : cfg.initial_resolved;
	uint32_t rp_prev = ResolveProgress(resolved);
	// REPETITION PROBE: RAW count of the summons of each watched card in THIS
	// rollout. Distinct from `ResolveProgress`, which caps at min_count and sums
	// every entry; that capping and that sum are what made the histogram blind to
	// "one Liger against three". `rep_active`: at least one first summon has
	// happened, so there are later decisions to count.
	uint32_t rep_seen[4] = { 0, 0, 0, 0 };
	bool rep_active = false;
	// Watched cards already counted as OFFERED in this rollout: one bit per entry,
	// to separate "how many decisions offered the card" from "how many rollouts
	// saw at least one".
	uint64_t rep_offered = 0;
	// Watched cards already ACTIVATED in this rollout.
	uint64_t rep_activated = 0;
	// Zones already counted in this rollout: 8 bits per watched entry.
	uint32_t rep_zone_seen = 0;
	const bool probe_on = cfg.probe_repeat && ProbeCount() > 0;
	// The source of the delta: `--watch` (pure summons) when it exists,
	// otherwise the old resolution counter. Both are packed the same way, but
	// they do NOT count the same thing: the first counts only summons, the
	// second also counts activations.
	const bool probe_watch = !cfg.probe_watch.empty();
	double novel_states = 0;
	// Consecutive decisions with no unseen atom. The rollout-level novelty cut
	// used to consume them; it is removed (REFUTED) and `stale` now only serves as
	// a gradient tie-break.
	uint32_t stale = 0;
	bool novelty_cut = false;
	std::vector<double> logit;
	// Macro being executed: the remaining keys are played without sampling and
	// without producing a PolicyStep. Intermediate FORCED prompts (a single
	// choice) pass through: the corpus records none of them, so the mined
	// sequences never contain any.
	const std::vector<uint64_t>* active_macro = nullptr;
	size_t macro_pos = 0;
	std::vector<std::pair<uint32_t, uint32_t>> applicable;   // (macro, choice)
	// RECORDED decisions (multiple choices) since the start of the rollout: the
	// unit the corpus positions are expressed in. A macro's offer window compares
	// against this counter, not against the depth in prompts (which also counts
	// forced moves).
	uint32_t nsteps = 0;
	// The context descriptor costs one zone query per decision: it is only computed
	// when someone consumes it, i.e. the policy's contextual level, the semantic
	// guard of the CURRENT catalogue, or (online mining) that of the catalogue TO
	// COME, whose contexts are being recorded now. That third term is the only new
	// one: without it, a run starting bare would record all-zero contexts and the
	// semantic guard would be born blind.
	const bool want_ctx = cfg.ctx_shrink >= 0.0f ||
						  (cfg.options && cfg.options->ctx_tol >= 0) ||
						  (mine_flat && cfg.options_online->ctx_tol >= 0);
	// A PATH CONDITIONING (mixed sum of the moves of the first k decisions) used to
	// be maintained here. Removed: REFUTED twice, 0 comparisons won out of 4
	// against `--ctx-shrink` alone and a collapse at k = 6. The contextual level's
	// context is now always the SEMANTIC descriptor (`step.ctx`).
	// HEAD BANDIT (--qhat): key of the current node, mixed sum of the moves ALREADY
	// played. It carries the move ACTUALLY chosen (the macro's id when a macro is
	// taken, not its first key) and it simply stops being consulted past k.
	uint64_t qh_ctx = 0;
	const bool qhat_on = cfg.qhat_depth != 0;
	// RAII GUARD: PolicyRollout exits through a dozen `return` statements (dead
	// end, terminal, constraint violated, guard, burned bound, budget) and the DEAD
	// rollouts are precisely the ones Q^ draws its signal from. A hand-written pour
	// at each exit would have missed one, silently.
	struct QhatGuard {
		Search* self;
		bool armed;
		~QhatGuard() { if(armed) self->QhatCommit(); }
	} qhat_guard{ this, qhat_on };
	// HINDSIGHT: substitute goals reached by THIS rollout, (code, depth of the
	// first summon). An RAII guard for the same reason as Q^: the exits are too
	// numerous for a hand-written pour, and it is precisely the DEAD rollouts that
	// carry the signal.
	static thread_local std::vector<HindsightHit> hs_hits;
	const bool hs_on = cfg.hindsight > 0.0f;
	hs_hits.clear();
	struct HindsightGuard {
		Search* self;
		const NrpaRun* run;
		const std::vector<HindsightHit>* hits;
		bool armed;
		~HindsightGuard() { if(armed) self->HindsightCommit(*run, *hits); }
	} hs_guard{ this, &run, &hs_hits, hs_on };
	// PROGRESS PROFILE: the max SerialProgress reached by THIS rollout, and the
	// decision index of its last progress. Poured by an RAII guard for the same
	// reason as Q^ and hindsight: the exits are too numerous, and it is precisely
	// the DEAD rollouts that carry the signal.
	uint32_t sp_max = 0, sp_max_at = 0;
	struct SpGuard {
		Search* self;
		const uint32_t *mx, *at;
		bool armed;
		~SpGuard() {
			if(!armed)
				return;
			++self->stats.sp_final[(std::min)(*mx, 71u)];
			self->stats.sp_at_sum += *at;
			++self->stats.sp_lines;
			// The frontier STAGNATION detector: the trigger of the
			// refinement. A rung gain resets the counter; otherwise it climbs,
			// and ReenterMaybe compares it against the cfg.refine_after
			// threshold (which is printed).
			if(*mx > self->max_sp_seen) {
				self->max_sp_seen = *mx;
				self->rollouts_since_gain = 0;
			} else {
				++self->rollouts_since_gain;
			}
		}
	} sp_guard{ this, &sp_max, &sp_max_at,
				!cfg.serial_reqs.empty() && cfg.novelty_patience != 0 };
	// THE MECHANISMS THAT READ THE GRAPH SNAPSHOT.
	//
	// `assign_bias` WAS MISSING HERE, and it was a measured defect. The snapshot
	// was only taken under `--assign` or `--backward`, yet it is what fills
	// `snap_useful`, the list of MATERIALS `--assign-bias` consults at every
	// decision. So `--assign-bias 3` ALONE was completely INERT, and the run still
	// announced it as active ("choices engaging a MATERIAL from the recipe graph
	// are favoured"). The costliest form of the trap: a mechanism that is off and
	// declares itself on.
	//
	// Trace of the defect in the report: "recipes: N summon(s) observed, mean h
	// 0.00 over ZERO evaluation(s)". The graph was learning and nobody was reading
	// it.
	// `op_bias` is here from the start for the same reason: it reads
	// `snap_operators`, which only the snapshot fills. Forgetting it would
	// reproduce the defect above identically.
	const bool rec_on =
		cfg.recipes && (cfg.assign || cfg.backward || cfg.assign_bias > 0.0f ||
						cfg.op_bias > 0.0f);
	if(qhat_on) {
		qh_nodes.clear();
		qh_moves.clear();
		qh_back.clear();
		qh_reward = 0.0;
	}

	// ELISION OF FORCED MOVES IN THE ROLLOUTS: where the cost is REALLY paid.
	//
	// THE MEASUREMENT THAT JUSTIFIES IT: 74.6 % of expanded nodes offer only ONE
	// legal answer (global attribution, --growth). For each of them a rollout pays
	// full price today: `ComputeBoardKeyInto` (two zone queries), `CollectAtoms`
	// (four queries and ~60-80 table probes), the heuristic, the archive, the
	// score, for a point where there is NOTHING TO DECIDE and where the policy
	// learns nothing (no PolicyStep is produced on a single-choice prompt).
	//
	// So `depth` stops being the loop index: it counts REAL DECISIONS, and `pi`
	// counts the prompts. A consequence worth stating: `--max-decisions` changes
	// meaning under the flag, so depths only compare with the control arm at equal
	// TIME budget.
	//
	// WHAT IS PRESERVED ON AN ELIDED PROMPT, and it is not negotiable: the
	// bookkeeping of actions, turns, summons and resolutions. A forced chain
	// RESOLVES effects and can place a card; skipping that would falsify the
	// constraints and the goal. Only the EVALUATION work is skipped.
	// saute.
	// A re-entered rollout starts at its cell's depth: the `--max-decisions`
	// budget keeps the meaning "total length of the line", and the solutions carry
	// the full count (prefix + suffix).
	uint32_t depth = reenter_active ? reenter_depth : 0;
	const uint32_t prompt_cap =
		cfg.elide_forced ? cfg.max_decisions * 8u + 64u : cfg.max_decisions;
	for(uint32_t pi = 0; pi < prompt_cap && depth < cfg.max_decisions; ++pi) {
		if(BudgetExhausted()) {
			stats.hit_time_limit = true;
			return;
		}
		Step st = StepToPrompt();
		actions += actions_this_step;
		turns += turns_this_step;
		if(st == Step::Rejected) { ++stats.dead_ends; return; }
		if(st == Step::Ended)    { ++stats.terminals; return; }
		// A violated constraint kills the rollout with its current score: the
		// policy learns to respect it by itself.
		if(!SummonsOk(summons) || material_violation) {
			++stats.constraint_cuts;
			return;
		}
		// HINDSIGHT: the EXTRA DECK monsters actually summoned by this rollout.
		// The filter is a TYPE, not a list: no card name is compiled in, and the
		// set it designates is exactly the one whose arity is the wall (a Fusion,
		// a Synchro, an Xyz, a Link consume materials; a normal monster does
		// not).
		if(hs_on)
			for(uint32_t raw : summons_this_step) {
				if(!raw)
					continue;   // summoned face down: unknown code
				const uint32_t code = duel.Db().Canonical(raw);
				const CardRow* row = duel.Db().Find(code);
				if(!row || !(row->type & (TYPE_FUSION | TYPE_SYNCHRO | TYPE_XYZ |
										  TYPE_LINK)))
					continue;
				bool known = false;
				for(const auto& p : hs_hits)
					if(p.code == code) { known = true; break; }
				if(!known)
					hs_hits.push_back(
						{ code, depth,
						  static_cast<uint32_t>(run.steps.size()) });
			}
		summons += static_cast<uint32_t>(summons_this_step.size());
		resolved += resolved_this_step;
		++stats.nodes;
		prof::Count(prof::kDecisions);
		// --- PROBE: THE PARTS THAT DO NOT DEPEND ON THE BOARD ----------------
		// PLACED HERE, BEFORE THE ELISION, and that is a fix rather than a
		// detail: an elided prompt does not come back down into the loop body,
		// and `watch_*_this_step` is reset at the next StepToPrompt. Counting
		// lower down would therefore make everything a forced chain moved or
		// activated DISAPPEAR, i.e. most of it, since 74.6 % of prompts are
		// forced. A probe that loses its events under a flag would return a
		// "never" that does not exist.
		if(probe_on) {
			// ZONE PRESENCE: a rollout counts ONCE per zone reached.
			if(watch_zone_this_step)
				for(size_t i = 0; i < ProbeCount(); ++i)
					for(int z = 0; z < 6; ++z) {
						const uint32_t bit = 1u << (8 * i + z);
						if(!(watch_zone_this_step & bit) ||
						   (rep_zone_seen & bit))
							continue;
						rep_zone_seen |= bit;
						++stats.rep[i].zone_rollouts[z];
					}
			// ACTIVATIONS, strictly observational.
			if(watch_act_this_step)
				for(size_t i = 0; i < ProbeCount(); ++i) {
					const uint32_t a = static_cast<uint32_t>(
						(watch_act_this_step >> (16 * i)) & 0xffff);
					if(!a)
						continue;
					stats.rep[i].act_total += a;
					if(!(rep_activated & (1ull << i))) {
						rep_activated |= 1ull << i;
						++stats.rep[i].act_rollouts;
					}
				}
		}
		// --- ELISION: is the prompt FORCED? -------------------------------
		// Enumerated HERE, before any evaluation work. The bookkeeping above
		// (actions, turns, summons, resolutions) is already done: an elided
		// prompt is still counted, only its EVALUATION is skipped.
		bool ro_filled = false;
		if(cfg.elide_forced) {
			if(!FillChoices(ro_choices)) {
				++stats.dead_ends;
				return;
			}
			ro_filled = true;
			CountOffers(rep_offered);
			if(ro_choices.size() == 1) {
				++stats.elided;
				duel.SetResponse(ro_choices[0].response);
				// `path` MUST carry the elided answer (a latent defect): `path`
				// feeds the solutions, `best_path` and the archive, and a path
				// without the forced answers does not replay from the root. The DFS
				// branch (Descend) already did this.
				path.push_back(ro_choices[0].response);
				// NEITHER `depth`, NOR a PolicyStep, NOR a score: nothing was
				// decided. A single-choice prompt already produced no PolicyStep,
				// so the policy loses strictly nothing.
				continue;
			}
		}
		// Histogram of the resolutions the rollouts reach: monotonic along a
		// rollout, so each threshold is crossed only once.
		if(resolved_this_step) {
			const uint32_t rp = ResolveProgress(resolved);
			for(uint32_t k = rp_prev; k < rp; ++k) {
				if(k < 4)
					++stats.resolve_reached[k];
				else
					++stats.resolve_overflow;   // histogram truncated
			}
			rp_prev = rp;
		}

		ComputeBoardKeyInto(duel, static_cast<uint8_t>(cfg.target_player),
							board_scratch);
		const BoardKey& here = board_scratch;
		// RECIPE GRAPH SNAPSHOT: the only safe boundary is depth 0, where no prompt
		// is enumerated, so nothing holds the address of `snap_useful`, which
		// --assign serves to the enumerator.
		if(rec_on && depth == 0 && stats.rollout_count >= recipe_snap_next) {
			recipe_snap_next = stats.rollout_count + cfg.recipe_snap_period;
			RecipeSnapshot();
		}
		// --- REPETITION PROBE ---
		// Placed HERE and not in the resolution block above: it needs the board, which
		// has just been computed. Three readings, in order.
		if(probe_on) {
			// 1. The REFERENCE, once per worker: how many summons for a FIRST
			//    copy, from the rollouts' starting state. Without it, the distance
			//    measured further down is a bare number.
			if(depth == 0 && stats.rollout_count >= rep_d0_next) {
				rep_d0_next = stats.rollout_count + kRepD0Period;
				for(size_t i = 0; i < ProbeCount(); ++i) {
					uint32_t d = 0;
					const float r0 =
						RecipeDistance(here, resolved, ProbeCode(i), &d);
					stats.rep[i].code = ProbeCode(i);
					stats.rep[i].known = cfg.recipes->Knows(ProbeCode(i));
					stats.rep[i].d0 = d;
					stats.rep[i].rest0 = static_cast<uint32_t>(r0);
					++stats.rep[i].d0_samples;
				}
			}
			// 2. The per-rollout HISTOGRAM, in RAW counts and per entry.
			const uint64_t probe_delta =
				probe_watch ? watch_this_step : resolved_this_step;
			if(probe_delta) {
				for(size_t i = 0; i < ProbeCount(); ++i) {
					const uint32_t d = static_cast<uint32_t>(
						(probe_delta >> (16 * i)) & 0xffff);
					if(!d)
						continue;
					for(uint32_t k = rep_seen[i]; k < rep_seen[i] + d && k < 5;
						++k)
						++stats.rep[i].reached[k];
					const bool was_first = rep_seen[i] == 0;
					rep_seen[i] += d;
					// 3. At the FIRST summon: the distances. One zone sweep, once per
					//    rollout and per product.
					if(was_first) {
						RepeatProbeFirst(i, here, resolved, depth);
						rep_active = true;
					}
				}
			}
			// Later decisions: "the second never arrives" only means something when the
			// rollout still had decisions ahead of it.
			if(rep_active)
				for(size_t i = 0; i < ProbeCount(); ++i)
					if(rep_seen[i])
						++stats.rep[i].after_sum;
		}
		if(GoalCheck(here, depth, actions, resolved)) {
			// Reaching the board dominates everything; the cost breaks ties
			// LEXICOGRAPHICALLY: burned first, then actions, then decisions (the
			// 1e9/1e5/1 units stay disjoint: burned <= ~55, actions <= ~1000,
			// decisions <= ~500, and everything stays an exact integer in a
			// double). So the NRPA adaptation pulls towards the CHEAPEST line, not
			// towards the first one found.
			const double gs = 1e12 - static_cast<double>(goal_burned) * 1e9 -
							  static_cast<double>(goal_actions) * 1e5 -
							  static_cast<double>(depth);
			if(gs > run.score) {
				run.score = gs;
				run.peak_steps = run.steps.size();
			}
			// Bandit reward: the goal is worth exactly 1, the top of the scale. It
			// is the only point where it is not derived from the material, since a
			// board reached is not "a lot of material".
			if(qhat_on)
				qh_reward = 1.0;
			if(!cfg.anytime)
				return;
			// Anytime: the line CONTINUES; further decisions can reduce the burned
			// count (real recoveries), and every re-reaching of the board is
			// re-recorded when it is cheaper.
		}
		if(turns >= 2) {
			++stats.turn_cuts;
			break;
		}
		if(GuardCut(here, summons))
			return;
		// Burned bound (anytime B&B): a state already burning more than the best
		// known line plus the recovery slack will not beat it.
		if(const uint32_t bcut = EffectiveBurnCut();
		   bcut != UINT32_MAX && CurrentBurned() > bcut) {
			++stats.burn_cuts;
			return;
		}
		ArchiveObserve(here, depth, resolved);

		// --- THE RECIPE GRAPH ON THE HOT PATH -------------------------------
		// One evaluation for both the distance and the backward progress: the cost IS
		// the presence scan, and redoing it would double it.
		uint32_t rec_rem = 0, rec_back = 0;
		if(rec_on && cfg.backward) {
			rec_rem = RecipeEval(here, cfg.backward ? &rec_back : nullptr);
			// The reference that turns the distance into PROGRESS. Taken at the
			// rollout's first decision: every rollout of one phase starts from the
			// same state, so it is a constant of the phase rather than a
			// denominator moving under the comparisons.
			if(depth == 0)
				recipe_base = rec_rem;
			stats.rec_roll_sum += rec_rem;
			stats.rec_roll_d0_sum += recipe_base;
			++stats.rec_roll_count;
			if(cfg.backward) {
				stats.backward_sum += rec_back;
				++stats.backward_count;
			}
		}

		// A rollout's score is the MAX along the line, not the final state: a
		// rollout that approached the board then crashed is still a better guide
		// than one that never placed anything. Novelty breaks ties: between two
		// lines with the same material, the one that visited unseen facts
		// deserves the adaptation.
		{
			// GATE. `--novelty 0` turned NoveltyCut off (the DFS paths) but NOT this
			// block: the `novel_states` term stayed in the score and the cost was
			// paid in full, four core queries, four sorts and ~60-80 table probes PER
			// DECISION, for a mere tie-break. So the "no novelty" control arm of the
			// A/B only covered the LDS passes, never NRPA. It is also the run's main
			// UNBOUNDED allocation: `seen` grows without limit, per worker.
			if(cfg.novelty_patience) {
				uint32_t partition = cfg.novelty_serialize
					? CommonCodes(here.codes, target.codes) * 16u +
						  ResolveProgress(resolved)
					: 0u;
				// BACKWARD SERIALISATION. The current partition counts the TARGET
				// cards placed: it is flat over the whole climb, precisely where
				// arity kills the solver. The number of subproducts of the AND/OR
				// decomposition already built, on the other hand, moves from the
				// first brick, so the novelty table reopens BEFORE any target card
				// exists. Serialized IW / BFWS, applied to the learned
				// decomposition rather than to the literal goal.
				if(cfg.backward)
					partition = partition * 64u + (rec_back & 63u);
				// SERIALISATION BY x*, the same gesture as above. The value also
				// feeds the progress profile: it used to be computed and then
				// thrown away after the partition.
				if(!cfg.serial_reqs.empty()) {
					const uint32_t spv = SerialProgress(
						duel, static_cast<uint8_t>(cfg.target_player),
						cfg.serial_reqs, duel.Db(), serial_res0);
					// 7 bits, like NoveltyCut: the total served exceeds 63 with
					// the departure slices.
					partition = partition * 128u + (spv & 127u);
					if(spv > sp_max) {
						sp_max = spv;
						sp_max_at = nsteps;
					}
				}
				CollectAtoms(duel, static_cast<uint8_t>(cfg.target_player), here,
							 partition, atoms_scratch);
				if(novelty.Observe(atoms_scratch, depth)) {
					++stats.novelty_novel;
					novel_states += 1;
					stale = 0;
				} else {
					++stats.novelty_stale;
					// NOVELTY PRUNING IN THE POLICY ROLLOUTS. The rollout-level cut
					// used to be wired into Rollout(), the GREEDY rollout, which no
					// longer does anything, and NEVER here, although the novelty
					// verdict is already COMPUTED here at every decision (four core
					// queries, ~60-80 table probes) and thrown away after a mere
					// tie-break. The rollout's most expensive mechanism only served
					// to break score ties.
					// The rollout-level cut fired here after `patience` mute
					// decisions. Removed: REFUTED. `stale` is still counted, since
					// it serves the gradient tie-break.
					++stale;
				}
			}
			// Required resolutions (--resolve) weigh MORE than target cards
			// (cfg.resolve_weight): at equal weight, the 8/8 lines with no rip won
			// the adaptation race against the partially ripping ones.
			double material = static_cast<double>(Heuristic(here)) +
							  static_cast<double>(cfg.resolve_weight) *
								  ResolveProgress(resolved);
			double sc = material * 1000.0 + novel_states;
			// LEARNED LANDMARKS: the `h` that DECREASES, served where it can
			// change something. Every landmark achievement is worth
			// `landmark_weight` points of material, the same unit as
			// `resolve_weight`, so it is tunable on the same scale.
			//
			// AS PROGRESS AND NOT AS DISTANCE, and that is what matters: the
			// rollout score is a "higher is better", so we count the landmarks
			// ACHIEVED, not those left. Placing "Leo Dancer in the graveyard" a
			// second time raises the score BEFORE any
			// Liger exists, which is exactly what the flat `h` cannot do, and the
			// reason a line preparing two Ligers looked worse than one placing a
			// single Liger.
			//
			// OUTSIDE `material`: `material` also serves as the Q^ bandit's reward,
			// normalised by the target board's material. Pouring landmark points into
			// it would change that scale and make earlier measurements
			// incomparable.
			if(cfg.landmark_weight > 0.0f && cfg.landmarks) {
				const uint32_t total =
					static_cast<uint32_t>(cfg.landmarks->Items().size());
				const uint32_t rem = LandmarkRemaining(here);
				sc += static_cast<double>(cfg.landmark_weight) * 1000.0 *
					  static_cast<double>(total - rem);
			}
			// `--recipe-w` used to pour the RECIPE DISTANCE in here as progress.
			// Removed: REFUTED with a STRUCTURAL cause, and that is the lesson to
			// keep. The distance to Liger counts "3 Lunalight monsters" as a
			// CARDINAL requirement; but building Perfume CONSUMES two Lunalight to
			// return one, so the distance RISES. The mechanism rewarded accumulating
			// bodies and PUNISHED consuming them, i.e. punished summoning, the exact
			// opposite of the goal. An h^add heuristic over an AND/OR graph is only
			// correct when CONSUMPTION is modelled; the delete relaxation assumes
			// reaching a subgoal destroys nothing, which is precisely false here.
			// `recipe_base` and `rec_rem` are still RECORDED (stats.rec_roll_*): the
			// distance is measured, it no longer enters any cost.
			if(sc > run.score) {
				run.score = sc;
				// The peak is HERE, and it is all the gradient should learn under
				// --adapt-to-peak.
				run.peak_steps = run.steps.size();
			}
			// Bandit reward (--qhat): the SAME material, relative to the target
			// board's. Bounded, comparable across runs, and WITHOUT novelty, which
			// is a gradient tie-break rather than a measure of success, and which
			// is unbounded.
			if(qhat_on && qhat_scale > 0.0) {
				const double r = material / qhat_scale;
				if(r > qh_reward)
					qh_reward = r;
			}
			if(novelty_cut) {
				++stats.novelty_cuts;
				return;
			}
		}

		ChoiceList& choices = ro_choices;
		// Already filled by the elision test: redoing it would cost a second
		// enumeration per decision, i.e. exactly what is being saved.
		if(!ro_filled && !FillChoices(choices)) {
			++stats.dead_ends;
			return;
		}
		// OFFER PROBE: counts ONLY when the elision has not already done it,
		// otherwise the non-forced prompts would be counted twice.
		if(!ro_filled)
			CountOffers(rep_offered);

		size_t pick = 0;
		bool scripted = false;
		// Active macro: play its next key when THIS (multiple-choice) prompt offers
		// it; otherwise it ABORTS and the rollout resumes its course. The mechanism
		// never removes anything from the space; at worst it does nothing.
		if(active_macro && choices.size() > 1) {
			const uint64_t want = (*active_macro)[macro_pos];
			for(size_t i = 0; i < choices.size(); ++i)
				if(choices[i].plan_key == want) {
					pick = i;
					scripted = true;
					break;
				}
			if(scripted) {
				++stats.macro_absorbed;
				if(++macro_pos >= active_macro->size())
					active_macro = nullptr;
				// The ABSORBED decision still enters the flat line: it is precisely
				// the one `steps` will never see, and the one a re-mining must be
				// able to cross in order to lengthen the macro instead of
				// manufacturing a macro of macros.
				if(mine_flat) {
					PolicyStep f;
					f.keys.reserve(choices.size());
					for(const Choice& c : choices)
						f.keys.push_back(c.plan_key);
					// An absorbed decision was never sampled: its biases are undefined.
					// The vectors are sized (any consumer indexing them stays on its
					// feet) and zero; the miner only reads `keys`, `chosen` and `ctx`.
					f.known.assign(choices.size(), 0);
					f.hinted.assign(choices.size(), 0);
					f.chosen = pick;
					if(want_ctx)
						f.ctx = ContextKey(
							CommonCodes(here.codes, target.codes),
							duel.Count(static_cast<uint8_t>(cfg.target_player),
									   LOCATION_HAND));
					run.flat.push_back(std::move(f));
				}
			} else {
				++stats.macro_aborted;
				active_macro = nullptr;
			}
		}
		if(!scripted && choices.size() > 1) {
			// Softmax sampling under the policy, with NO child evaluation: that is what
			// makes an NRPA rollout several times cheaper than a greedy one (which
			// advances/measures/restores every child).
			PolicyStep step;
			// CONTEXT of the decision: target board cards already placed. The same
			// descriptor as the one recorded on corpus lines; it is SEMANTIC, hence
			// comparable from one line to another, unlike depth.
			// l'autre, contrairement a la profondeur.
			// The descriptor costs one zone query per decision: it is only paid for
			// when the contextual level is on, or when the options' semantic guard
			// needs it.
			if(want_ctx)
				step.ctx = ContextKey(
					CommonCodes(here.codes, target.codes),
					duel.Count(static_cast<uint8_t>(cfg.target_player),
							   LOCATION_HAND));
			// EFFECTIVE CONDITIONING of the contextual level: the PATH under MCPS
			// (the moves already played on this line), the semantic descriptor
			// otherwise. `path_ctx` is the path BEFORE this decision, i.e. "the root
			// up to this node"; the current move is not part of it.
			step.cctx = static_cast<uint64_t>(step.ctx);
			step.keys.reserve(choices.size());
			step.known.reserve(choices.size());
			step.hinted.reserve(choices.size());
			logit.resize(choices.size());
			double mx = -1e300;
			// LIVENESS OF THE OPERATOR BIAS: "at least one designated operator could
			// be offered at this decision". Reset AT EVERY decision; a flag leaking
			// from one decision to the next is the `ChoiceList::Emit` defect, and it
			// has already cost a session.
			bool any_op_useful = false;
			for(size_t i = 0; i < choices.size(); ++i) {
				uint64_t key = choices[i].plan_key;
				// Phase changes are in the repertoire (the reference ends its turn)
				// but deserve no bias there: at +1.5 an idle prompt with ~10 choices
				// would end the turn one time in three, and ending the turn is
				// irreversible. The policy is still free to learn it; only the PRIOR
				// is withdrawn. (The `phase` flag, not the label: hot paths no longer
				// have labels.)
				bool known = plan_index.count(key) != 0 && !choices[i].phase;
				// Domain hint: this move engages a card designated by --hint, so it
				// starts with a sampling bonus.
				//
				// THE GUARD IS HERE, AND IT IS THE ONLY THING THAT WAS EVER DEBATABLE.
				// On a SUBSET prompt, `card` is only the first code of a selection:
				// applying an operator's instruction ("this card, try it more often")
				// to an approximate identity is wrong. The IDENTITY itself is now
				// unconditional: it serves the probes and the ASSIGNMENT bias, which
				// designates a ROLE (material) rather than a card to play.
				bool hinted = choices[i].card && !IsSubsetPrompt(prompt_type) &&
							  !cfg.hint_cards.empty() &&
							  std::find(cfg.hint_cards.begin(),
										cfg.hint_cards.end(),
										choices[i].card) != cfg.hint_cards.end();
				// ASSIGNMENT BIAS (--assign-bias): does this choice engage a MATERIAL
				// designated by the recipe graph? The list comes from `snap_useful`,
				// derived from the recipes rather than from the literal target, and
				// that is what distinguishes it from a goal-directed bias, which
				// steered towards a move that does not exist yet. The measured
				// bottleneck is here: Leo Dancer is offered 14 433 times in the "which
				// Lunalight to send to the graveyard" prompt and chosen 202 times.
				const bool useful =
					cfg.assign_bias > 0.0f && choices[i].card &&
					!snap_useful.empty() &&
					std::binary_search(snap_useful.begin(), snap_useful.end(),
									   choices[i].card);
				// OPERATOR BIAS (--op-bias). Does this choice play a card whose
				// decomposition requires PRESENCE IN PLAY? That is "what to play", not
				// "what to choose", the other half of the problem, and the one
				// `--assign-bias` does not reach.
				const bool op_useful =
					cfg.op_bias > 0.0f && choices[i].card &&
					!IsSubsetPrompt(prompt_type) && !snap_operators.empty() &&
					std::binary_search(snap_operators.begin(),
									   snap_operators.end(), choices[i].card);
				if(op_useful)
					any_op_useful = true;
				double w = EffectiveWeight(pol, &ctx_weights, key, step.cctx,
										   cfg.ctx_shrink);
				logit[i] = (w + (known ? cfg.nrpa_bias_known : 0.0f) +
							(hinted ? cfg.hint_bias : 0.0f) +
							(useful ? cfg.assign_bias : 0.0f) +
							(op_useful ? cfg.op_bias : 0.0f) -
							(choices[i].phase ? cfg.phase_w : 0.0f)) /
						   (cfg.nrpa_temp > 1e-3f ? cfg.nrpa_temp : 1e-3f);
				mx = (std::max)(mx, logit[i]);
				step.keys.push_back(key);
				step.known.push_back(known ? 1 : 0);
				// Bit 0 = --hint, bit 1 = useful material (--assign-bias). BOTH must
				// be replayed by AdaptRun: a gradient computed under a distribution
				// that is not the sampling's is wrong, and that is exactly the fault
				// once found on hint_bias.
				// trouvee sur hint_bias.
				step.hinted.push_back(static_cast<uint8_t>((hinted ? 1 : 0) |
														   (useful ? 2 : 0)));
			}
			// OPTIONS: an applicable macro becomes one more choice, weighted by ITS
			// OWN weight, the sampling unit that concentrates the mass. Two guards,
			// set by the first A/B: ONE macro per first key (the best ranked, since
			// the index follows the mining order), otherwise the catalogue floods
			// the softmax (7.7 picks per rollout measured); and NO inherited bias
			// (the weight starts at zero and only the adaptation raises it; with
			// the repertoire bias, 82 % of picks aborted after ~1.3 steps).
			applicable.clear();
			if(cfg.options) {
				const uint32_t W = cfg.options->window;
				for(size_t i = 0; i < choices.size(); ++i) {
					auto it = cfg.options->by_first.find(choices[i].plan_key);
					if(it == cfg.options->by_first.end())
						continue;
					// Preconditions: the position window (refuted, kept for the
					// A/B) and the SEMANTIC guard (ctx compatible with a corpus
					// occurrence), the same tests as the selection model. The "best
					// macro per first key" becomes the best ranked AMONG THE
					// COMPATIBLE ONES.
					for(uint32_t mi : it->second) {
						const uint32_t p = cfg.options->pos[mi];
						if(W && (nsteps + W < p || nsteps > p + W))
							continue;
						if(!cfg.options->CtxOk(mi, step.ctx))
							continue;
						applicable.emplace_back(mi, static_cast<uint32_t>(i));
						break;   // one macro per first key
					}
				}
				for(const auto& [mi, ci] : applicable) {
					const double w =
						EffectiveWeight(pol, &ctx_weights, cfg.options->ids[mi],
										step.cctx, cfg.ctx_shrink);
					logit.push_back(
						w / (cfg.nrpa_temp > 1e-3f ? cfg.nrpa_temp : 1e-3f));
					mx = (std::max)(mx, logit.back());
					step.keys.push_back(cfg.options->ids[mi]);
					step.known.push_back(0);
					step.hinted.push_back(0);
				}
			}
			size_t pick_index = 0;
			// HEAD BANDIT (--qhat): over the first k RECORDED decisions, MCPS's
			// selection rule replaces the softmax. It is GREEDY (argmax), as in the
			// paper: diversity comes from the NRPA rollout below, not from noise
			// added here, and the averages move at every rollout, including when it
			// dies.
			bool by_bandit = false;
			if(qhat_on && nsteps < cfg.qhat_depth) {
				BanditNode* node = nullptr;
				auto in = bandit.find(qh_ctx);
				if(in != bandit.end()) {
					node = &in->second;
				} else if(bandit.size() < cfg.qhat_max_nodes) {
					node = &bandit[qh_ctx];
					// Q^'s CONDITION at this node: the moves of the PATH, i.e. the
					// ones the bandit itself chose (`qh_back`), not everything the
					// rollout has played since. Decisions absorbed by a macro are
					// not part of it: the tree's edge is the MACRO and its steps
					// are its consequence, so including them would shrink the
					// conditioning set without conditioning on anything more.
					node->cond.reserve(qh_back.size());
					for(const auto& [nk, mk] : qh_back)
						node->cond.push_back(mk);
					std::sort(node->cond.begin(), node->cond.end());
					node->cond.erase(
						std::unique(node->cond.begin(), node->cond.end()),
						node->cond.end());
				} else {
					++stats.qhat_fallback;
				}
				if(node) {
					qh_nodes.push_back(qh_ctx);
					// PERMUTATION REFERENCE: the deepest FROZEN node of the path
					// (the paper's s_r, propagated to the subtree). None -> the
					// root, whose statistic is kept incrementally and is always
					// fresh.
					BanditNode* sr = nullptr;
					for(size_t i = qh_nodes.size(); i-- > 0;) {
						auto it = bandit.find(qh_nodes[i]);
						if(it != bandit.end() && it->second.frozen) {
							sr = &it->second;
							break;
						}
					}
					double best_val = -1e300;
					uint32_t ties = 0;
					// LABELLING FOR THE PROBE, on the very first passes over this
					// node only (`q` fills from the first backpropagation): the
					// probe must be able to NAME the moves, including those the
					// bandit never chose, which are precisely the ones Q^ ranks.
					// Labelling on every visit would cost one table probe per
					// candidate.
					const bool label_here =
						cfg.enumeration.db && node->q.empty();
					for(size_t i = 0; i < step.keys.size(); ++i) {
						const uint64_t key = step.keys[i];
						if(label_here && i < choices.size())
							bandit_code.emplace(key, choices[i].card);
						PermStat ph;
						if(sr) {
							auto ic = sr->perm.find(key);
							if(ic != sr->perm.end()) {
								ph = ic->second;
							} else {
								ph = perm_win.Stat(key, sr->mask);
								sr->perm.emplace(key, ph);
							}
						} else {
							ph = perm_win.Root(key);
						}
						uint32_t n = 0;
						double w = 0;
						if(auto iq = node->q.find(key); iq != node->q.end()) {
							n = iq->second.first;
							w = iq->second.second;
						}
						// val = (n Q + n^ Q^) / (n + n^), with n Q = w. Weights
						// PROPORTIONAL TO THE SAMPLE SIZES: that is the
						// minimum-variance combination, and it is what removes
						// GRAVE's bias hyperparameter. A move never seen
						// (n + n^ = 0) goes ahead of all the others, the paper's
						// first-visit rule, which guarantees every opening is tried
						// at least once.
						const double val =
							(n + ph.n) ? (w + double(ph.n) * double(ph.q)) /
											 double(n + ph.n)
									   : 2.0;
						// Ties broken at random (reservoir): without it the core's
						// ENUMERATION order would decide the opening for the whole
						// first pass, where everything is tied at 2.0.
						if(val > best_val) {
							best_val = val;
							pick_index = i;
							ties = 1;
						} else if(val == best_val &&
								  (next() % ++ties) == 0) {
							pick_index = i;
						}
					}
					by_bandit = true;
					step.bandit = 1;
					++stats.qhat_decisions;
					if(!nsteps)
						++stats.qhat_first;
					qh_back.emplace_back(qh_ctx, step.keys[pick_index]);
					if(cfg.enumeration.db && pick_index < choices.size())
						bandit_code.emplace(step.keys[pick_index],
											choices[pick_index].card);
				}
			}
			if(!by_bandit) {
				double sum = 0;
				for(double& x : logit) { x = std::exp(x - mx); sum += x; }
				double u = double(next() >> 11) *
						   (1.0 / 9007199254740992.0) * sum;
				double acc = 0;
				for(size_t i = 0; i < logit.size(); ++i) {
					acc += logit[i];
					if(u < acc || i + 1 == logit.size()) {
						pick_index = i;
						break;
					}
				}
			}
			if(pick_index >= choices.size()) {
				// Macro chosen: the answer applied is its first move, and the
				// cursor takes over from there.
				const auto& [mi, ci] = applicable[pick_index - choices.size()];
				pick = ci;
				++stats.macro_taken;
				const std::vector<uint64_t>& seq = cfg.options->seqs[mi];
				if(seq.size() > 1) {
					active_macro = &seq;
					macro_pos = 1;
				}
				// The macro is a MOVE in its own right for the window: its id is a
				// code like any other, and that is how Q^ can rank it against its
				// atomic competitors at the same decision.
				if(qhat_on)
					qh_moves.push_back(cfg.options->ids[mi]);
			} else {
				pick = pick_index;
			}
			// LIVENESS OF THE OPERATOR BIAS, recorded AFTER the rollout: how many
			// decisions offered a designated operator, and in how many the move
			// played engaged one. At `offered = 0` the mechanism is INERT and no
			// search judge concerns it.
			if(cfg.op_bias > 0.0f && any_op_useful) {
				++stats.op_bias_offered;
				if(pick < choices.size() && choices[pick].card &&
				   !snap_operators.empty() &&
				   std::binary_search(snap_operators.begin(),
									  snap_operators.end(), choices[pick].card))
					++stats.op_bias_taken;
			}
			// The bandit's PATH grows by the move actually chosen (macro id
			// included). Past k it is no longer consulted, so there is no point
			// maintaining it.
			if(qhat_on && nsteps < cfg.qhat_depth)
				qh_ctx += MixMove(step.keys[pick_index]);
			// Hint visibility: was a hinted move even LEGAL here? That is the
			// measurement separating "badly sampled" from "never offered by the
			// core".
			if(!cfg.hint_cards.empty()) {
				bool any = false;
				for(uint8_t h : step.hinted)
					any |= (h & 1) != 0;
				if(any) {
					++stats.hint_seen;
					// BREAKDOWN BY PROMPT TYPE: without it, the counter mixes the
					// effect of subset-prompt identity (which touches ONLY selection
					// prompts) with the run's quality.
					if(IsSubsetPrompt(prompt_type))
						++stats.hint_seen_sel;
					else
						++stats.hint_seen_exact;
					if(step.hinted[pick_index] & 1)
						++stats.hint_taken;
				}
			}
			// FLAT line: the same decision in ATOMIC form. The macro ids (the tail of
			// step.keys) are dropped and the move kept is the choice of the PROMPT
			// actually played, chosen macro included (`pick` is then its first move).
			if(mine_flat) {
				const size_t n = choices.size();
				PolicyStep f;
				f.ctx = step.ctx;
				f.chosen = pick;
				f.keys.assign(step.keys.begin(), step.keys.begin() + n);
				f.known.assign(step.known.begin(), step.known.begin() + n);
				f.hinted.assign(step.hinted.begin(), step.hinted.begin() + n);
				run.flat.push_back(std::move(f));
			}
			step.chosen = pick_index;
			run.steps.push_back(std::move(step));
		}

		if(choices.size() > 1) {
			// The path grows by the ATOMIC move actually played (chosen macro
			// included: it is its first move), and only over the first `mcps_depth`
			// decisions. Past that it is frozen: every deep decision then shares its
			// prefix's context, which BOUNDS the table instead of creating one cell
			// per node.
			// The ATOMIC move played enters the rollout's multiset, whether the
			// decision was sampled, dictated by the bandit or ABSORBED by a macro:
			// "the game contains a" cares neither about the order nor about who
			// decided.
			if(qhat_on)
				qh_moves.push_back(choices[pick].plan_key);
			++nsteps;
		}
		// OFFER -> CHOICE CONVERSION. Does the move kept engage a watched card?
		// Paired with `offer_steps`, that gives the judge the rarity of "Leo in the
		// graveyard" made unusable.
		if(probe_on && choices[pick].card)
			for(size_t i = 0; i < ProbeCount(); ++i)
				if(ProbeCode(i) == choices[pick].card) {
					++stats.rep[i].taken_steps;
					// THE YES/NO PART. `response` is 1 for yes, 0 for no
					// (playerop.cpp): we read the answer and not the label, which does
					// not exist on the hot paths.
					if(prompt_type == MSG_SELECT_EFFECTYN ||
					   prompt_type == MSG_SELECT_YESNO) {
						++stats.rep[i].yn_steps;
						int32_t v = 0;
						if(choices[pick].response.size() == 4)
							std::memcpy(&v, choices[pick].response.data(), 4);
						if(v == 1)
							++stats.rep[i].yn_yes;
					}
				}
		duel.SetResponse(choices[pick].response);
		path.push_back(choices[pick].response);
		// One more REAL DECISION. `depth` is no longer the loop index: it only
		// advances on prompts that offered a choice, and that is what makes
		// `--max-decisions` comparable with a line's "after elision" depth (143
		// instead of 284 on the reference).
		++depth;
	}
	// Exit through the TOP: the decision ceiling.
	++stats.edges_skipped;
}

void Search::QhatCommit() {
	// Rollout reward, bounded. It is 0 for a rollout that placed nothing, which
	// is the case of the DEAD rollouts, and that is the whole point: Q^ averages
	// over ALL rollouts, not over the best ones.
	double r = qh_reward;
	if(r < 0.0) r = 0.0;
	if(r > 1.0) r = 1.0;
	++stats.qhat_playouts;
	stats.qhat_reward_sum += r;
	// 1. THE SLIDING WINDOW. The multiset of the rollout's moves, sorted and
	//    deduplicated: MCPS counts one presence per game.
	std::sort(qh_moves.begin(), qh_moves.end());
	qh_moves.erase(std::unique(qh_moves.begin(), qh_moves.end()),
				   qh_moves.end());
	perm_win.Push(qh_moves, static_cast<float>(r));
	// 2. Q(s,a): backpropagation over the (node, move) pairs crossed.
	for(const auto& [nk, mk] : qh_back) {
		auto it = bandit.find(nk);
		if(it == bandit.end())
			continue;
		auto& e = it->second.q[mk];
		++e.first;
		e.second += r;
	}
	// 3. VISITS AND FREEZING. A non-root node reaching rho visits freezes its
	//    permutation statistic and becomes the reference of its subtree. The
	//    root (empty condition) never freezes: it is kept incrementally by the
	//    window and so is always fresh, and it is the one that carries the
	//    diagnosis of the first decision.
	for(uint64_t nk : qh_nodes) {
		auto it = bandit.find(nk);
		if(it == bandit.end())
			continue;
		BanditNode& nd = it->second;
		++nd.visits;
		if(!nd.frozen && !nd.cond.empty() && nd.visits >= cfg.qhat_rho) {
			nd.frozen = true;
			perm_win.Mask(nd.cond, nd.mask);
		}
	}
}

std::vector<BanditProbe> Search::RootBandit() const {
	std::vector<BanditProbe> out;
	if(!cfg.qhat_depth)
		return out;
	// The UNION of two sets, and that is the point of the probe. The moves the
	// bandit CHOSE at the root (`q`) are only the four openings of the first
	// prompt, a trivial decision. What the diagnosis aims at is Q^({}, a) for
	// EVERY move a: the mean reward of the lines that played it, anywhere and in
	// any order. That is exactly the window's root statistic, and it exists for
	// every code the first k decisions offered, Tenki's target included, which is
	// decided at the SECOND prompt and which the root's `q` table would never
	// see.
	std::unordered_map<uint64_t, BanditProbe> acc;
	if(auto it = bandit.find(0ull); it != bandit.end())
		for(const auto& [key, nw] : it->second.q) {
			BanditProbe& p = acc[key];
			p.key = key;
			p.n = nw.first;
			p.w = nw.second;
		}
	for(const auto& [key, code] : bandit_code) {
		BanditProbe& p = acc[key];
		p.key = key;
		p.code = code;
	}
	out.reserve(acc.size());
	for(auto& [key, p] : acc) {
		const PermStat ph = perm_win.Root(key);
		p.nhat = ph.n;
		p.qhat_sum = double(ph.n) * double(ph.q);
		if(p.n || p.nhat)
			out.push_back(p);
	}
	return out;
}

void AdaptRun(NrpaPolicy& pol, NrpaResidual* res, const NrpaRun& run,
			  float alpha, float bias_known, float hint_bias, float shrink,
			  float temp, size_t ctx_max, float assign_bias, bool to_peak) {
	const double inv_t = 1.0 / (temp > 1e-3f ? temp : 1e-3f);
	const bool two_level = res && shrink >= 0.0f;
	// TRUNCATION AT THE PEAK. The score is a MAX over prefixes: the steps after
	// the maximum contributed to NO part of the score, and reinforcing them
	// teaches the post-peak collapse. `peak_steps` is 0 on CORPUS lines (they do
	// not go through PolicyRollout), so a line with no measured peak is never
	// truncated; otherwise the adaptation replay would go mute silently.
	// d'adaptation deviendrait muet en silence.
	const size_t n_steps =
		(to_peak && run.peak_steps > 0 && run.peak_steps <= run.steps.size())
			? run.peak_steps
			: run.steps.size();
	// The contextual gradient CREATES one cell per (move, context). Under path
	// conditioning the number of contexts is no longer bounded by 256 but by the
	// number of prefixes visited: with no cap the table would swell without end,
	// per worker (the same family as the novelty table). At the cap, existing
	// cells live their life and no new one appears.
	auto touch = [&](uint64_t k) -> CtxWeight* {
		auto it = res->find(k);
		if(it != res->end())
			return &it->second;
		if(ctx_max && res->size() >= ctx_max)
			return nullptr;
		return &(*res)[k];
	};
	std::vector<double> p;
	for(size_t si = 0; si < n_steps; ++si) {
		const PolicyStep& s = run.steps[si];
		// Decision taken by the HEAD BANDIT (--qhat): it was not drawn from
		// the softmax, so the NRPA gradient is undefined there. Applying it
		// would push the chosen move by +alpha at every rollout with no
		// counterweight, the exact failure mode of the path-conditioning flag.
		if(s.bandit)
			continue;
		p.resize(s.keys.size());
		double mx = -1e300;
		for(size_t i = 0; i < s.keys.size(); ++i) {
			double w = EffectiveWeight(pol, res, s.keys[i], s.cctx, shrink);
			p[i] = (w + (s.known[i] ? bias_known : 0.0f) +
					(i < s.hinted.size() ? ((s.hinted[i] & 1) ? hint_bias : 0.0f) +
											   ((s.hinted[i] & 2) ? assign_bias : 0.0f)
										 : 0.0f)) *
				   inv_t;
			mx = (std::max)(mx, p[i]);
		}
		double sum = 0;
		for(double& x : p) { x = std::exp(x - mx); sum += x; }
		pol[s.keys[s.chosen]] += alpha;
		for(size_t i = 0; i < s.keys.size(); ++i)
			pol[s.keys[i]] -= static_cast<float>(alpha * p[i] / sum);
		if(!two_level)
			continue;
		// The contextual level receives the SAME gradient, on the (move,
		// context) cell. Its counter measures the evidence accumulated in THAT
		// context: it is what decides, through s(n), when it takes over.
		if(CtxWeight* c = touch(CtxKey(s.keys[s.chosen], s.cctx)))
			c->w += alpha;
		for(size_t i = 0; i < s.keys.size(); ++i) {
			CtxWeight* c = touch(CtxKey(s.keys[i], s.cctx));
			if(!c)
				continue;
			c->w -= static_cast<float>(alpha * p[i] / sum);
			++c->n;
		}
	}
}

void AdaptCorpus(NrpaPolicy& pol, NrpaResidual* res,
				 const std::vector<NrpaRun>& runs, uint32_t passes, float alpha,
				 float bias_known, float hint_bias, float shrink, float temp,
				 size_t ctx_max) {
	for(uint32_t pass = 0; pass < passes; ++pass)
		for(const NrpaRun& r : runs)
			AdaptRun(pol, res, r, alpha, bias_known, hint_bias, shrink, temp,
					 ctx_max);
}

void Search::Adapt(Policy& pol, const NrpaRun& best) {
	AdaptRun(pol, &ctx_weights, best, cfg.nrpa_alpha, cfg.nrpa_bias_known,
			 cfg.hint_bias, cfg.ctx_shrink, cfg.nrpa_temp, cfg.ctx_max,
			 cfg.assign_bias, cfg.adapt_to_peak);
	if(cfg.adapt_to_peak && best.peak_steps > 0 &&
	   best.peak_steps < best.steps.size())
		stats.peak_truncations += best.steps.size() - best.peak_steps;
}

// THE RETURN TO THE RUNG: the half of SIW_R the ROLLOUTS were missing.
//
// The ladder existed (an archive cell = a SerialProgress rung) and only the
// FINISHER took its roots there: every NRPA rollout restarted from the root
// and had to re-climb the whole line, as the sp_final histogram shows (mass on
// the first units, exponential tail). The closed form is categorical: the
// serialised cost `Sigma b^(l_i)` only exists when each block is searched FROM
// the previous rung; with no return one stays at `b^L`.
//
// What this mechanism does, and nothing else: with probability cfg.reenter,
// replay from the root the path of a UNIFORMLY drawn cell (Go-Explore: high
// rungs can be dead ends, so the budget is not concentrated there, the
// frontier is spread), arm the prefix counters (turns, summons, resolutions,
// actions, decisions, the SAME bookkeeping as the rollout, through the SAME
// StepToPrompt), and let PolicyRollout continue from there. `path` is seeded
// with the prefix: solutions stay replayable from the root and `best_path`
// stays a valid finisher root.
//
// SCOPE GUARD: nothing happens without armed serialisation (with no ladder a
// cell is a cache, not a rung), so health checks and every mode without
// --target are unchanged byte for byte.
// The base of the departure rungs (zone 5): the size of the RESERVE
// (deck+extra) at the ROOT of this search. Set on entry to each Run*, since
// the first SerialProgress arrives mid-line and a lazy initialisation would
// measure departures from an arbitrary state.
void Search::InitSerialBase() {
	// Quota uses restart from zero at each search entry; in the DFS descents
	// they do not go back down on backtracking, so the DFS cells
	// over-partition (the safe direction).
	std::memset(quota_uses, 0, sizeof quota_uses);
	if(cfg.serial_reqs.empty())
		return;
	const auto con = static_cast<uint8_t>(cfg.target_player);
	serial_res0 = duel.Count(con, 0x01u) + duel.Count(con, 0x40u);
}

void Search::ReenterMaybe(uint64_t& rng) {
	reenter_active = false;
	// Re-entry requires a STRUCTURE of cells: the ladder (armed serialisation)
	// or the GRID (rips x overlap). Without one of the two, the archive is a
	// cache, not a frontier.
	if(cfg.reenter <= 0.0f || archive.empty() ||
	   (cfg.serial_reqs.empty() && !cfg.grid))
		return;
	auto next = [&rng] {
		rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
		return rng;
	};
	if(static_cast<double>(next() & 0xffff) >=
	   static_cast<double>(cfg.reenter) * 65536.0)
		return;
	// ARMED REFINEMENT: when the frontier stagnates, this return aims at the
	// BEST cell, since that is what the sub-ladder is derived from, and the
	// duel will be exactly in the state wanted.
	const bool want_refine = cfg.refine_after && cfg.balance && !refined &&
							 rollouts_since_gain >= cfg.refine_after;
	// TOURNAMENT OF 2 (after the 359-cell measurement): pure uniform diluted
	// the budget, and the frontier cells, whose next block is the work, only
	// received 1/N of the returns. Two draws, keep the better score (the score
	// already ranks progress first): the mass doubles on the top half of the
	// ladder, and no low rung is abandoned (Go-Explore: high ones can be dead
	// ends).
	//
	// UNDER THE GRID: UNIFORM over the RIPPED cells ONLY (r > 0), no
	// tournament. WARNING: BOTH VARIANTS ARE REFUTED IN PROPORTION (4 seeds,
	// 180 s, MIN control on the same binary):
	//   V1 (uniform over ALL cells): 3/4 seeds at ZERO rips. The early-run
	//      archive only has (0 rips, o) cells, re-entry pours the mass there
	//      and the SHARED NRPA policy adapts on those continuations, so the
	//      rip-seeker dies.
	//   V2 (ripped only, this code): >=3 free = 0 on 4/4 and DEGRADED joint
	//      lines (3r/1-of-6 against the control's 3r/5-of-6). The r>0 cells
	//      are massively low-overlap, the policy adapts on continuations poor
	//      in board, and the INTERLEAVED lines the control finds from the root
	//      disappear.
	// The lesson (the third measurement of the same channel after the ladder):
	// re-entry by replay COUPLES the shared adaptive policy to the re-entered
	// family, whatever it is. The independence assumption of the race (R6 of the
	// refined closed form) is violated by this channel, and
	// the additive grid cost is UNREACHABLE by "replay + shared adaptation"
	// together. The mechanism stays wired as the CONTROL of its own refutation;
	// any successor must DECOUPLE the adaptation from the re-entered lines, and
	// go back through the A/B in proportion.
	const ArchiveEntry* pick = nullptr;
	if(cfg.grid && cfg.serial_reqs.empty()) {
		static thread_local std::vector<const ArchiveEntry*> ripped;
		ripped.clear();
		for(const ArchiveEntry& e : archive)
			if(e.resolves > 0)
				ripped.push_back(&e);
		if(ripped.empty())
			return;
		pick = ripped[next() % ripped.size()];
	} else {
		pick = &archive[next() % archive.size()];
		const ArchiveEntry* other = &archive[next() % archive.size()];
		if(other->score > pick->score)
			pick = other;
	}
	if(want_refine) {
		size_t best_i = 0;
		for(size_t i = 1; i < archive.size(); ++i)
			if(archive[i].score > archive[best_i].score)
				best_i = i;
		pick = &archive[best_i];
	}
	const ArchiveEntry& e = *pick;
	if(e.path.empty())
		return;
	// COPY: the archive can evict this entry during the rollout that follows.
	reenter_path = e.path;
	uint32_t turns = 0, summons = 0, actions = 0;
	uint64_t resolved = 0;
	size_t used = 0;
	bool dead = false;
	// The PREFIX's quota uses count: zeroed here, accumulated by StepToPrompt
	// during the replay, kept by PolicyRollout.
	std::memset(quota_uses, 0, sizeof quota_uses);
	replaying = true;
	while(used < reenter_path.size()) {
		const Step st = StepToPrompt();
		actions += actions_this_step;
		turns += turns_this_step;
		summons += static_cast<uint32_t>(summons_this_step.size());
		resolved += resolved_this_step;
		if(st != Step::Prompt) {
			dead = true;
			break;
		}
		duel.SetResponse(reenter_path[used++]);
	}
	replaying = false;
	// An archive path MUST replay from the root (same duel, same seed, complete
	// answers). A failure here is a defect to look at, not noise; it is counted,
	// and the rollout restarts from the root.
	if(dead) {
		++stats.reenter_fail;
		arena.Restore();
		return;
	}
	reenter_turns = turns;
	reenter_summons = summons;
	reenter_actions = actions;
	reenter_resolved = resolved;
	reenter_depth = e.decisions;
	reenter_active = true;
	++stats.reenter_rollouts;
	stats.reenter_base_sum += SerialProgress(
		duel, static_cast<uint8_t>(cfg.target_player), cfg.serial_reqs,
		duel.Db(), serial_res0);
	// The duel is at the cell's state: this is where the sub-ladder is posted.
	// pose (s22, chantier 3).
	if(want_refine)
		RefineLadderHere();
}

// RE-SERIALISATION FROM THE FRONTIER. A bare run has no reference line, so the
// gap-profile bench cannot guide it. But the LP solves FROM THE DUEL, wherever
// one is: its places not yet served (the right-hand side already reduced by the
// marking) are exactly "what is left to do from this rung", and the closed form
// says that cutting finer always wins. One refinement level; the cell key
// extends beyond the gate (the refined cell's rung), and the return to the rung
// and the tournament work unchanged.
void Search::RefineLadderHere() {
	const auto con = static_cast<uint8_t>(cfg.target_player);
	const CardDB& db = duel.Db();
	static thread_local std::vector<uint32_t> scratch;
	std::vector<uint32_t> zres, zava, zfld;
	for(uint32_t loc : { 0x01u, 0x40u }) {
		duel.QueryCodes(con, loc, scratch);
		for(uint32_t c : scratch)
			zres.push_back(db.Canonical(c));
	}
	for(uint32_t loc : { 0x02u, 0x04u, 0x08u, 0x10u, 0x20u }) {
		duel.QueryCodes(con, loc, scratch);
		for(uint32_t c : scratch)
			zava.push_back(db.Canonical(c));
		if(loc == 0x04u)
			for(uint32_t c : scratch)
				zfld.push_back(db.Canonical(c));
	}
	// THE PATH QUOTAS. The uses accumulated by StepToPrompt up to this cell (prefix
	// replay included, the same counter as the cell key) enter the LP's capacities:
	// the sub-ladder derived from x* can no longer route through an effect this
	// path has already spent. Guarded by cfg.quota_h (control = off).
	std::vector<std::pair<uint32_t, uint32_t>> spent;
	if(cfg.quota_h)
		for(size_t i = 0; i < cfg.quota_hosts.size() && i < 12; ++i)
			if(quota_uses[i])
				spent.emplace_back(cfg.quota_hosts[i], quota_uses[i]);
	LPResult r;
	const double h2 = cfg.balance->Solve(zres, zava, zfld, &r, spent);
	// An infeasible LP HERE would be a cell dead in the sense of theorem 3: we do
	// not refine on it, and let the stagnation counter arm the next return at the
	// top. Under quota_h this case includes "dead UP TO THE QUOTAS": giving up on
	// the refinement is the intended soft failure mode.
	if(h2 < 0 || !r.feasible) {
		rollouts_since_gain = 0;
		return;
	}
	refine_reqs.clear();
	auto push = [&](const BalanceModel::Need& n) {
		if(n.zone == 0 || refine_reqs.size() >= 16)
			return;
		SearchConfig::SerialReq rq;
		rq.code = n.code;
		rq.arch = n.arch;
		rq.zone = n.zone;
		rq.count = n.count;
		refine_reqs.push_back(rq);
	};
	for(const BalanceModel::Need& n : cfg.balance->NeedsFrom(r))
		push(n);
	for(const BalanceModel::Need& n : cfg.balance->ConsumedFrom(r))
		push(n);
	if(refine_reqs.empty()) {
		rollouts_since_gain = 0;
		return;
	}
	refine_gate_sp = SerialProgress(duel, con, cfg.serial_reqs, db,
									serial_res0);
	refined = true;
	stats.refine_done = 1;
	stats.refine_subrungs = refine_reqs.size();
	stats.refine_gate = refine_gate_sp;
	// Liveness of the path quotas: silent when the mechanism is off, ALWAYS stated
	// when it is armed, "0 posted" included (a host never spent at this rung is
	// information, not silence).
	char qh[96] = "";
	if(cfg.quota_h) {
		std::snprintf(qh, sizeof(qh),
					  ", quotas du chemin : %u pose(s), %zu sans-borne, "
					  "%zu hors-budget",
					  r.quota_applied, r.quota_unbounded_hosts.size(),
					  r.quota_overrun_hosts.size());
	}
	std::printf("  RAFFINEMENT (s22) : frontiere stagnante (%u tirages sans "
				"gain, seuil %u) — LP a la cellule h=%.0f, %zu sous-barreau(x) "
				"poses, porte sp=%u%s\n",
				rollouts_since_gain, cfg.refine_after, h2, refine_reqs.size(),
				refine_gate_sp, qh);
}

double Search::Nrpa(int level, const Policy& pol, NrpaRun& best, uint64_t& rng) {
	if(level <= 0) {
		best.score = -1;
		best.steps.clear();
		// The return to the rung precedes the rollout; the common arena.Restore()
		// brings us back to the root in every case (the replay pushes nothing).
		ReenterMaybe(rng);
		PolicyRollout(rng, pol, best);
		arena.Restore();
		reenter_active = false;
		return best.score;
	}
	// ONE copy of the policy per level call. It used to be copied at every rollout
	// (passed by value down to level 0): a table of thousands of entries
	// duplicated tens of thousands of times per run.
	Policy local = pol;
	best.score = -1;
	uint32_t stagnant = 0, repeats = 0;
	for(uint32_t i = 0; i < cfg.nrpa_iters; ++i) {
		if(BudgetExhausted() ||
		   (!cfg.anytime && solutions.size() >= cfg.max_solutions))
			break;
		NrpaRun child;
		Nrpa(level - 1, local, child, rng);
		if(child.score > best.score) {
			best = std::move(child);
			stagnant = 0;
			repeats = 0;
		} else if(++stagnant >= 8) {
			// The policy replays the same line without progressing any further: that is
			// NRPA's documented failure mode (premature convergence). Adapting more would
			// only freeze it, so we hand back control and the level above, or a restart,
			// will bring diversity back.
			break;
		}
		if(best.score >= 0) {
			Adapt(local, best);
			++stats.nrpa_adapts;
		}
	}
	return best.score;
}

void Search::OnlineOptionsTick(const NrpaRun& best) {
	OnlineOptions* oo = cfg.options_online;
	if(!oo)
		return;
	const auto now = std::chrono::steady_clock::now();
	bool mine = false;
	std::vector<NrpaRun> corpus;
	// Mining parameters copied UNDER LOCK: they never change after startup, but
	// reading them outside the lock would be a race to the letter of the standard,
	// and this file leaves no "benign" race behind it.
	size_t m_max = 0, m_len = 0;
	uint32_t m_sup = 0, m_win = 0;
	int m_ctx = -1;
	{
		std::lock_guard<std::mutex> lock(oo->mu);
		// 1. CONTRIBUTION. The flat line of this worker's best sequence enters the
		// living corpus. Three filters, in this order: it must exist, be UNSEEN (the
		// sixteen workers all restart from the best shared sequence, so without dedup
		// the corpus would be the same line sixteen times), and respect its worker's
		// quota.
		if(best.score > 0 && best.flat.size() >= 2) {
			++oo->offered;
			uint64_t sig = 0x9E3779B97F4A7C15ull;
			for(const PolicyStep& s : best.flat)
				sig = (sig ^ s.keys[s.chosen]) * 0x100000001B3ull;
			bool dup = false;
			for(const auto& e : oo->pool)
				if(e.sig == sig) { dup = true; break; }
			if(dup) {
				++oo->dups;
			} else {
				// Per-worker quota: the slot is taken from this worker's WORST line,
				// not from another's. That is what stops a prolific worker from
				// emptying the corpus of the others' diversity.
				size_t mine_n = 0, worst = SIZE_MAX;
				for(size_t i = 0; i < oo->pool.size(); ++i)
					if(oo->pool[i].worker == cfg.worker_id) {
						++mine_n;
						if(worst == SIZE_MAX ||
						   oo->pool[i].score < oo->pool[worst].score)
							worst = i;
					}
				bool take = true;
				if(mine_n >= oo->per_worker) {
					if(oo->pool[worst].score >= best.score)
						take = false;
					else
						oo->pool.erase(oo->pool.begin() +
									   static_cast<ptrdiff_t>(worst));
				}
				if(take) {
					OnlineOptions::Entry e;
					e.sig = sig;
					e.score = best.score;
					e.worker = cfg.worker_id;
					e.run.score = best.score;
					e.run.steps = best.flat;   // the miner consumes `steps`
					oo->pool.push_back(std::move(e));
					++oo->kept;
					// Global ceiling: the corpus's worst line, across all workers, goes
					// out.
					while(oo->pool.size() > oo->max_pool) {
						size_t w = 0;
						for(size_t i = 1; i < oo->pool.size(); ++i)
							if(oo->pool[i].score < oo->pool[w].score)
								w = i;
						oo->pool.erase(oo->pool.begin() +
									   static_cast<ptrdiff_t>(w));
					}
				}
			}
		}
		// 2. BUYING BACK the catalogue. The shared_ptr keeps the one we are leaving
		// alive as long as another worker still holds it.
		if(oo->gen != options_gen) {
			options_hold = oo->cat;
			options_gen = oo->gen;
			cfg.options = options_hold.get();
		}
		// 3. CLAIMING the mining turn. One miner at a time; the first worker to
		// cross the deadline takes it.
		const size_t lines = oo->pool.size() + (oo->seed ? oo->seed->size() : 0);
		if(!oo->mining && now >= oo->next && lines >= oo->min_lines) {
			oo->mining = true;
			mine = true;
			m_max = oo->max_options;
			m_sup = oo->support;
			m_len = oo->max_len;
			m_win = oo->window;
			m_ctx = oo->ctx_tol;
			corpus.reserve(lines);
			if(oo->seed)
				for(const NrpaRun& r : *oo->seed)
					corpus.push_back(r);
			for(const auto& e : oo->pool)
				corpus.push_back(e.run);
		}
	}
	if(!mine)
		return;
	// The mining runs WITH THE LOCK RELEASED: the other fifteen workers keep
	// rolling meanwhile. It also runs inside the run's budget, so its duration is
	// measured, and that is what decides whether it belongs online.
	const auto t0 = std::chrono::steady_clock::now();
	OptionCatalog cat = MineOptionCatalog(corpus, m_max, m_sup, m_len, m_win,
										  m_ctx);
	const double ms = std::chrono::duration<double, std::milli>(
						  std::chrono::steady_clock::now() - t0).count();
	{
		std::lock_guard<std::mutex> lock(oo->mu);
		oo->mining = false;
		oo->next = std::chrono::steady_clock::now() +
				   std::chrono::milliseconds(
					   static_cast<long long>(oo->period_ms));
		++oo->rounds;
		oo->mine_ms_total += ms;
		oo->mine_ms_max = (std::max)(oo->mine_ms_max, ms);
		oo->last_lines = corpus.size();
		if(cat.Size()) {
			size_t sum = 0, mx = 0;
			for(const auto& s : cat.seqs) {
				sum += s.size();
				mx = (std::max)(mx, s.size());
			}
			oo->last_size = cat.Size();
			oo->last_maxlen = mx;
			oo->last_avglen = double(sum) / double(cat.Size());
			oo->last_flat = cat.model_flat;
			oo->last_opt = cat.model_opt;
			oo->cat = std::make_shared<const OptionCatalog>(std::move(cat));
			++oo->gen;
		}
		// A round that keeps NOTHING leaves the previous catalogue in place: we never
		// disarm a worker over a momentarily poor corpus.
		if(oo->gen != options_gen) {
			options_hold = oo->cat;
			options_gen = oo->gen;
			cfg.options = options_hold.get();
		}
	}
}

double Search::NrpaTop(Policy& pol, NrpaRun& best, uint64_t& rng) {
	best.score = -1;
	best.steps.clear();
	// Restart seeded by the GLOBAL best sequence: the adaptation pulls at once
	// towards the best line known to ALL the workers, instead of relearning the
	// same sublines at every restart (measured: 6/8 with the same cards missing,
	// run after run).
	if(cfg.nrpa_shared) {
		std::lock_guard<std::mutex> lock(cfg.nrpa_shared->mu);
		if(cfg.nrpa_shared->best.score > 0)
			best = cfg.nrpa_shared->best;
	}
	uint32_t stagnant = 0, repeats = 0;
	for(uint32_t i = 0; i < cfg.nrpa_iters; ++i) {
		if(BudgetExhausted() ||
		   (!cfg.anytime && solutions.size() >= cfg.max_solutions))
			break;
		// SAFE BOUNDARY of the online mining: no rollout is in flight in this
		// worker, so no `active_macro` points into the catalogue, and it can be
		// swapped here and nowhere else.
		OnlineOptionsTick(best);
		NrpaRun child;
		Nrpa(cfg.nrpa_level - 1, pol, child, rng);
		if(child.score > best.score) {
			best = std::move(child);
			stagnant = 0;
			repeats = 0;
		} else if(++stagnant >= 8) {
			break;
		}
		// Exchange with the other workers, on a low-frequency mutex: one
		// acquisition per upper-level iteration (iters^(level-1) rollouts), not
		// per rollout. Publish an improvement; adopt the global best only on
		// stagnation, so as not to crush the diversity.
		if(cfg.nrpa_shared) {
			std::lock_guard<std::mutex> lock(cfg.nrpa_shared->mu);
			if(best.score > cfg.nrpa_shared->best.score) {
				cfg.nrpa_shared->best = best;
			} else if(cfg.nrpa_shared->best.score > best.score && stagnant >= 4) {
				best = cfg.nrpa_shared->best;
				stagnant = 0;
			}
		}
		if(best.score >= 0) {
			Adapt(pol, best);
			++stats.nrpa_adapts;
		}
		// HINDSIGHT: the SAME NRPA gradient, applied to the lines that reached
		// ANOTHER goal than the one we are after. That is the whole HER
		// mechanism: the information is already there, `Adapt` reinforces only a
		// millionth of it (the single best sequence), and the rest is thrown away
		// at every rollout.
		//
		// At `cfg.hindsight` fraction of alpha: those lines are NOT solutions of
		// the problem posed, only demonstrations of "how to pay for a k-material
		// summon". Adapting them at full strength would converge the policy
		// towards the cheapest Fusion, i.e. towards the failure being fixed.
		if(cfg.hindsight > 0.0f && !hindsight.empty())
			for(const auto& [code, g] : hindsight) {
				(void)code;
				AdaptRun(pol, &ctx_weights, g.run,
						 cfg.nrpa_alpha * cfg.hindsight, cfg.nrpa_bias_known,
						 cfg.hint_bias, cfg.ctx_shrink, cfg.nrpa_temp,
						 cfg.ctx_max, cfg.assign_bias, cfg.adapt_to_peak);
				++stats.hindsight_adapts;
			}
	}
	return best.score;
}

void Search::RunNrpa(const BoardKey& t, const std::vector<PlanStep>& p,
					 uint64_t seed) {
	prof::Scope ps(prof::kSearch);
	InitSerialBase();
	target = t;
	plan = &p;
	plan_index.clear();
	for(size_t i = 0; i < p.size(); ++i)
		if(p[i].edge)
			plan_index.emplace(p[i].edge, i);
	start = std::chrono::steady_clock::now();
	solutions.clear();
	path.clear();
	novelty.Clear();
	uint64_t rng = seed ? seed : 1;
	// HEAD BANDIT (--qhat): the sliding window is allocated here, once per search,
	// and the reward's DENOMINATOR is fixed here too. It is a constant of the
	// PROBLEM (the target board's material) and not the current best score;
	// normalising by that would move the scale of entries already in the window,
	// and Q^ would compare incomparable rewards.
	// The floor of 40 is the graveyard (`fodder` in Heuristic), which has no
	// structural maximum; the reward is capped at 1 in any case.
	if(cfg.qhat_depth) {
		if(!perm_win.Ready())
			perm_win.Init(cfg.qhat_window);
		qhat_scale = double(target.codes.size()) * 100.0 +
					 double(target.entries.size()) * 10.0 +
					 double(target.mzone_count) * 3.0 + 40.0 +
					 double(cfg.resolve_weight) * double(resolve_total);
	}

	arena.Push();     // resume point: the root
	// Successive restarts against premature convergence, but the policy is NO
	// LONGER reset: it is attenuated (partial persistence). The sublines learned
	// survive the restart and diversity comes back through the sampling. At 0,
	// the previous behaviour.
	// The initial policy (cfg.nrpa_init) seeds the first restart: the finisher
	// starts again from what the rollout phase learned.
	Policy pol;
	if(cfg.nrpa_init)
		pol = *cfg.nrpa_init;
	// ADAPTATION replay of the corpus: before the first rollout, the policy
	// undergoes the NRPA gradient of the already solved lines. Same injection
	// point as the weight prior; what changes is the FORM of the signal
	// (discriminative, see LiftPolicyRun). The adaptation is self-limiting: once
	// p(chosen) ~ 1 at a step, the update there is alpha(1-p) ~ 0, so the passes
	// saturate instead of exploding.
	if(cfg.nrpa_adapt_runs && cfg.nrpa_adapt_passes)
		AdaptCorpus(pol, &ctx_weights, *cfg.nrpa_adapt_runs,
					cfg.nrpa_adapt_passes, cfg.nrpa_alpha, cfg.nrpa_bias_known,
					cfg.hint_bias, cfg.ctx_shrink, cfg.nrpa_temp);
	while(!BudgetExhausted() &&
		  (cfg.anytime || solutions.size() < cfg.max_solutions)) {
		NrpaRun best;
		NrpaTop(pol, best, rng);
		// The policy at the end of a restart is the richest one: that is what we
		// export (before attenuation) for the LTS finisher.
		final_policy = pol;
		if(cfg.nrpa_restart_keep <= 0.0f) {
			pol.clear();
			ctx_weights.clear();
		} else {
			for(auto it = pol.begin(); it != pol.end();) {
				it->second *= cfg.nrpa_restart_keep;
				// Purge of negligible weights: the table stays bounded.
				if(it->second > -0.01f && it->second < 0.01f)
					it = pol.erase(it);
				else
					++it;
			}
			// The contextual level follows the SAME attenuation. Its counter, on the
			// other hand, survives: it is the evidence accumulated in that context,
			// not a learned magnitude, and a restart does not invalidate the fact
			// that the cell was visited.
			for(auto it = ctx_weights.begin(); it != ctx_weights.end();) {
				it->second.w *= cfg.nrpa_restart_keep;
				if(it->second.w > -0.01f && it->second.w < 0.01f)
					it = ctx_weights.erase(it);
				else
					++it;
			}
		}
	}
	arena.Pop();

	stats.ms = std::chrono::duration<double, std::milli>(
				   std::chrono::steady_clock::now() - start).count();
	stats.hit_node_limit = stats.nodes >= cfg.max_nodes;
	stats.exhausted = false;
	stats.novelty_atoms = novelty.Size();
	// The contextual level's liveness, read WHERE IT WORKS: a table at the cap
	// learns nothing new any more, and a tiny table says the conditioning
	// distinguishes nothing.
	stats.ctx_entries = ctx_weights.size();
	stats.ctx_capped = cfg.ctx_max && ctx_weights.size() >= cfg.ctx_max;
	// The BANDIT's liveness: size of the head tree, vocabulary of the window, and
	// measured MEMORY. This mechanism's cost is in memory and it is paid per
	// worker; without that figure, W would be tuned blind.
	stats.qhat_nodes = bandit.size();
	stats.qhat_codes = perm_win.Codes();
	stats.qhat_bytes = perm_win.Bytes();
}

void Search::RunLevin(const BoardKey& t, const std::vector<PlanStep>& p,
					  const NrpaPolicy& pol) {
	prof::Scope ps(prof::kSearch);
	InitSerialBase();
	target = t;
	plan = &p;
	plan_index.clear();
	for(size_t i = 0; i < p.size(); ++i)
		if(p[i].edge)
			plan_index.emplace(p[i].edge, i);
	start = std::chrono::steady_clock::now();
	solutions.clear();
	path.clear();
	tt.clear();

	// One node = one MULTIPLE-CHOICE prompt; the forced moves between two nodes
	// are replayed, not stored. A node's path is rebuilt by walking up the
	// parents, which is what allows best-first on an engine that can only replay.
	struct LNode {
		int32_t parent;
		uint32_t depth;                  // real decisions (multiple choices)
		float logpi;                     // log of the product of the probabilities
		std::vector<uint8_t> response;   // answer applied from the parent
		// --- sqrt-LTS (cfg.levin_reroot) ---
		// log of pi(n | n_k): the probability RELATIVE to the nearest hint
		// ancestor. It is what restarts from zero at every hint, and that is the
		// whole mechanism.
		float seg_logpi = 0.0f;
		// lambda/pi(n ; n_k) = sum of 1/pi(n_bar | n_k) over the segment.
		double lam = 1.0;
		// Target board cards placed at the PARENT: a hint lands when the node does
		// not have the same count (the board changed subgoal).
		uint16_t placed_parent = 0;
		// --- sqrt-LTS-H (cfg.reroot_h) ---
		// `hv` = c(n) = min over the ancestors of (1/w_t) * c^r_{n_t}(n), the
		// value that orders the queue. `hu` = the (1/w_t) * (1/pi(n|n_t)) term of
		// the ancestor achieving that min: it is what allows the sum to be
		// extended by one step without walking the path again.
		double hv = 0.0, hu = 0.0;
		// --- MACRO edge (cfg.finisher_options) ---
		// Index of the macro in cfg.options. UINT32_MAX = an ordinary edge.
		// `response` stays the macro's FIRST answer; the following ones are found
		// again on replay, key by key, at the intermediate prompts. They are not
		// stored (they are not choices of the tree).
		uint32_t macro = UINT32_MAX;
		// REAL decisions on the path. `depth` counts the EDGES, i.e. the d(n) of the
		// Levin bound, and it is indeed what must drop when a macro compresses k
		// decisions into one. But the decision CEILING is a bound on the game, not on
		// the tree: an edge of length 8 advances by 8 decisions, and letting it count
		// as 1 would make the ceiling eight times looser with nothing saying so. So
		// the two are kept separate.
		uint32_t rdepth = 0;
	};
	std::vector<LNode> nodes;
	nodes.push_back({ -1, 0, 0.0f, {}, 0.0f, 1.0, 0xffffu,
					  std::numeric_limits<double>::infinity(),
					  std::numeric_limits<double>::infinity(), UINT32_MAX, 0u });
	// h(root) of Eq. 7: the scale that makes the soft rerooter invariant under a
	// change of unit of the heuristic. It is h AT THE ROOT OF THE CURRENT SEARCH,
	// not a constant of the problem. An earlier version took
	// |target| + Sigma resolve_min, which is h at the start of the DUEL. In the
	// finisher the root is a backtrack state already at 7/8 cards, where h is ~1,
	// so the alpha dial was being swept at a scale eight times too large.
	// Evaluated when node 0 is expanded, which always precedes any use (it is
	// queued at zero cost and comes out first). Floored at 1.
	double h_root = 1.0;
	// Levin cost: d(n)/pi(n), in log for stability. The node of least cost is
	// expanded first, which is the paper's guarantee: the number of expansions is
	// bounded by the probability of the solution under the policy.
	// politique.
	using QE = std::pair<double, uint32_t>;
	// Tie-break (cfg.lifo_ties): std::greater on the pair extracted the SMALLEST
	// index, i.e. the oldest node, the furthest from the dive stack. In LIFO, the
	// tie extracted is the last queued: almost always a child of the node just
	// expanded, whose replay is ONE answer from the top of the stack.
	struct QCmp {
		bool lifo;
		bool operator()(const QE& a, const QE& b) const {
			if(a.first != b.first)
				return a.first > b.first;
			return lifo ? a.second < b.second : a.second > b.second;
		}
	};
	std::priority_queue<QE, std::vector<QE>, QCmp> pq(QCmp{ cfg.lifo_ties });
	pq.push({ 0.0, 0 });

	std::vector<uint32_t> chain;
	std::vector<double> logit;
	// Macros applicable at the expanded node: (macro index, index of the choice
	// carrying its first key). Buffer reused between expansions.
	std::vector<std::pair<uint32_t, uint32_t>> lapp;
	uint32_t actions = 0, turns = 0, summons = 0;
	uint64_t resolved = 0;
	enum class Adv { Branch, Dead, Goal };
	// Total of the required resolutions (--resolve): the "missing resolutions"
	// part of the PHS* heuristic.
	uint32_t resolve_target = 0;
	for(const ResolveReq& req : cfg.resolve_min)
		resolve_target += req.min_count;

	// Advances to the next multiple-choice prompt by playing the forced moves (the
	// opponent passes, single-candidate selections; a forced decision costs zero).
	// `check`: goal/turn/guard checks, active only on the NEW segment (after the
	// path's last answer). The earlier segments were checked when their nodes were
	// expanded, and redoing the board there would pay two zone queries per replay
	// prompt for nothing.
	auto advance = [&](bool check) -> Adv {
		for(;;) {
			Step st = StepToPrompt();
			actions += actions_this_step;
			turns += turns_this_step;
			if(st == Step::Rejected) { ++stats.dead_ends; return Adv::Dead; }
			if(st == Step::Ended)    { ++stats.terminals; return Adv::Dead; }
			if(!SummonsOk(summons) || material_violation) {
				++stats.constraint_cuts;
				return Adv::Dead;
			}
			summons += static_cast<uint32_t>(summons_this_step.size());
			resolved += resolved_this_step;
			if(check) {
				ComputeBoardKeyInto(duel, static_cast<uint8_t>(cfg.target_player),
									board_scratch);
				if(GoalCheck(board_scratch, static_cast<uint32_t>(path.size()),
							 actions, resolved)) {
					// Post-goal (opt-in): under anytime the line CONTINUES. GoalCheck
					// has already recorded the solution, and what follows can make it
					// cheaper (recoveries).
					if(!(cfg.anytime && cfg.finisher_post_goal))
						return Adv::Goal;
				}
				if(turns >= 2) { ++stats.turn_cuts; return Adv::Dead; }
				if(GuardCut(board_scratch, summons))
					return Adv::Dead;
			} else if(turns >= 2) {
				return Adv::Dead;
			}
			if(!FillChoices(ro_choices)) {
				++stats.dead_ends;
				return Adv::Dead;
			}
			if(ro_choices.size() > 1)
				return Adv::Branch;
			duel.SetResponse(ro_choices[0].response);
			path.push_back(ro_choices[0].response);
		}
	};

	// CONTINUATION of a MACRO EDGE. Its FIRST answer has just been applied by the
	// caller, as for an ordinary edge; the following keys are played by advancing
	// to each multiple-choice prompt and finding the key there. A missing key
	// ABORTS the edge, which becomes dead, exactly like a macro breaking in a
	// rollout. Returns false when the edge is dead (the caller stops), true when
	// the macro was played to the end (the caller resumes its flow: it is the one
	// that will do the next advance, as for an ordinary edge).
	//
	// `check` has the same meaning as for advance: true only on the NEW segment of
	// the chain. The counters only increment there; on the prefixes the same edge
	// is replayed thousands of times and a counter counting it there would measure
	// nothing but the replay rate.
	auto play_rest = [&](uint32_t n, bool check) -> bool {
		if(nodes[n].macro == UINT32_MAX)
			return true;
		const std::vector<uint64_t>& seq = cfg.options->seqs[nodes[n].macro];
		for(size_t k = 1; k < seq.size(); ++k) {
			if(advance(check) != Adv::Branch)
				return false;
			size_t hit = SIZE_MAX;
			for(size_t i = 0; i < ro_choices.size(); ++i)
				if(ro_choices[i].plan_key == seq[k]) {
					hit = i;
					break;
				}
			if(hit == SIZE_MAX) {
				if(check)
					++stats.macro_aborted;
				return false;
			}
			if(check)
				++stats.macro_absorbed;
			duel.SetResponse(ro_choices[hit].response);
			path.push_back(ro_choices[hit].response);
		}
		return true;
	};

	// Dive stack: arena level root+1+i is the state of node dive[i].node AT ITS
	// PROMPT. Under a sharp policy, best-first behaves almost depth-first: the next
	// node extracted is almost always a child or a sibling of a node on the stack,
	// so its replay reduces to popping back to the parent (Pop restores, never
	// Discard, which would not pour the child's dirty pages into the parent) and
	// playing ONE answer, instead of replaying the whole chain from the root.
	struct DiveLevel {
		uint32_t node;
		size_t path_len;
		uint32_t actions, turns, summons;
		uint64_t resolved;
	};
	std::vector<DiveLevel> dive;

	arena.Push();
	while(!pq.empty()) {
		if(BudgetExhausted()) {
			stats.hit_time_limit = true;
			break;
		}
		if(!cfg.anytime && solutions.size() >= cfg.max_solutions)
			break;
		// Memory safeguard: every queued child carries its answer; past a few
		// million nodes the time budget is consumed by the replays anyway.
		if(stats.nodes >= cfg.max_nodes || nodes.size() > 4000000) {
			stats.hit_node_limit = true;
			if(nodes.size() > 4000000)
				stats.hit_memory_limit = true;
			break;
		}
		const uint32_t idx = pq.top().second;
		pq.pop();
		const int32_t parent = nodes[idx].parent;

		// Ancestor chain of idx (root excluded, idx included), from the root towards
		// idx.
		chain.clear();
		for(int32_t i = static_cast<int32_t>(idx); i > 0; i = nodes[i].parent)
			chain.push_back(static_cast<uint32_t>(i));
		std::reverse(chain.begin(), chain.end());
		// Edges of the chain ALREADY EXPANDED (everything but idx's): the denominator
		// of the replay rate. A stack that absorbs everything replays 0.
		if(!chain.empty())
			stats.replay_chain += chain.size() - 1;

		// The DEEPEST ancestor present on the dive stack, not merely the direct
		// parent. The profile measured ~80 Process per expansion: every jump of the
		// queue towards another subtree replayed the WHOLE chain from the root,
		// although the stack held the common prefix. Popping back to the shared
		// ancestor and replaying only the SUFFIX is semantically neutral (same
		// answers, same checks, only the replay's starting point changes). Control:
		// benchmark 0 (backtrack 0 must return 42 expansions, b=0, EXHAUSTED).
		int64_t at = -1;
		size_t ci = 0;
		for(size_t j = dive.size(); j-- > 0;) {
			const uint32_t dn = dive[j].node;
			if(dn == 0) {   // the root is the ancestor of every node
				at = static_cast<int64_t>(j);
				ci = 0;
				break;
			}
			bool anc = false;
			for(size_t k = chain.size(); k-- > 0;)
				if(chain[k] == dn) {
					anc = true;
					ci = k + 1;   // first move to replay: dn's child
					break;
				}
			if(anc) {
				at = static_cast<int64_t>(j);
				break;
			}
		}

		bool dead = false;
		if(parent >= 0 && at < 0) {
			// Empty stack (or no common ancestor): replay from the root, and the
			// forced moves are re-derived.
			++stats.dive_misses;
			if(cfg.merged_pop) {
				arena.PopToAndRestore(dive.size());
				dive.clear();
			} else {
				while(!dive.empty()) {
					arena.Pop();
					dive.pop_back();
				}
				arena.Restore();
			}
			path.clear();
			actions = 0;
			turns = cfg.initial_turns;
			summons = cfg.initial_summons;
			resolved = cfg.initial_resolved;
			ci = 0;
			for(;;) {
				Adv a = advance(ci == chain.size());
				if(a != Adv::Branch) {
					dead = true;   // dead or goal: no children
					break;
				}
				if(ci == chain.size())
					break;
				// Full stack: the current state is chain[ci-1]'s prompt (the root
				// for ci = 0), an already expanded node, hence a possible branch
				// point for future extractions.
				if(cfg.dive_full) {
					arena.Push();
					dive.push_back({ ci == 0 ? 0u : chain[ci - 1], path.size(),
									 actions, turns, summons, resolved });
				}
				const std::vector<uint8_t>& r = nodes[chain[ci]].response;
				duel.SetResponse(r);
				path.push_back(r);
				++ci;
				if(ci < chain.size())
					++stats.replay_decisions;
				// Macro edge: its following keys are played now, before the loop
				// resumes its advance.
				if(!play_rest(chain[ci - 1], ci == chain.size())) {
					dead = true;
					break;
				}
			}
		} else {
			// Pop back to the shared ancestor (each Pop restores and merges its
			// dirty pages), restore ITS state (it is AT ITS PROMPT), then replay
			// the chain suffix: at shortest, idx's single answer (a child or a
			// sibling, the old fast path).
			if(cfg.merged_pop) {
				arena.PopToAndRestore(dive.size() -
									  static_cast<size_t>(at + 1));
				dive.resize(static_cast<size_t>(at + 1));
			} else {
				while(static_cast<int64_t>(dive.size()) > at + 1) {
					arena.Pop();
					dive.pop_back();
				}
				arena.Restore();
			}
			if(at >= 0) {
				const DiveLevel& L = dive[static_cast<size_t>(at)];
				path.resize(L.path_len);
				actions = L.actions;
				turns = L.turns;
				summons = L.summons;
				resolved = L.resolved;
			} else {
				path.clear();
				actions = 0;
				turns = cfg.initial_turns;
				summons = cfg.initial_summons;
				resolved = cfg.initial_resolved;
			}
			if(idx == 0) {
				dead = advance(true) != Adv::Branch;
			} else {
				for(;;) {
					duel.SetResponse(nodes[chain[ci]].response);
					path.push_back(nodes[chain[ci]].response);
					++ci;
					if(ci < chain.size())
						++stats.replay_decisions;
					if(!play_rest(chain[ci - 1], ci == chain.size())) {
						dead = true;
						break;
					}
					Adv a = advance(ci == chain.size());
					if(a != Adv::Branch) {
						dead = true;
						break;
					}
					if(ci == chain.size())
						break;
					// Full stack: we have just arrived at chain[ci-1]'s prompt,
					// already expanded; push it so that future jumps land here
					// instead of at the root.
					if(cfg.dive_full) {
						arena.Push();
						dive.push_back({ chain[ci - 1], path.size(),
										 actions, turns, summons, resolved });
					}
				}
			}
		}
		if(dead)
			continue;

		// Transposition: in best-first, the first visit is the one of least cost,
		// and the following ones are duplicates.
		const uint64_t key = Digest();
		if(tt.count(key)) {
			++stats.transpositions;
			continue;
		}
		tt.emplace(key, 1u);
		++stats.nodes;
		prof::Count(prof::kDecisions);
		const uint32_t ndepth = nodes[idx].depth;
		const uint32_t nrdepth = nodes[idx].rdepth;
		const float nlogpi = nodes[idx].logpi;
		if(nrdepth >= cfg.max_decisions) {
			++stats.edges_skipped;
			continue;
		}
		// The expanded node joins the dive stack: its children and its siblings will
		// be replayed with one restore instead of a full replay.
		arena.Push();
		dive.push_back({ idx, path.size(), actions, turns, summons, resolved });

		// PHS*: distance to the goal of the expanded node (missing target cards +
		// missing resolutions), inherited by its children in the cost.
		// `board_scratch` is the node's board (last advance in checked mode). At 0,
		// pure Levin.
		float hgoal = 0;
		uint32_t got = 0;
		if(cfg.levin_h > 0 || cfg.levin_reroot || cfg.reroot_h > 0)
			got = CommonCodes(board_scratch.codes, target.codes);
		if(cfg.levin_h > 0 || cfg.reroot_h > 0) {
			hgoal = static_cast<float>(target.codes.size() - got) +
					static_cast<float>(resolve_target - ResolveProgress(resolved));
		}
		// RECIPE GRAPH: the distance that DECREASES.
		//
		// The flat `h` counts the missing target cards, so it does not move until one
		// is placed, i.e. over ~90 % of the line. The recipe distance counts the
		// SUMMONS left to make, intermediate materials included: placing Leo Dancer in
		// the graveyard places no target card, but drops the distance to Liger Dancer
		// from 2 to 1.
		//
		// Always EVALUATED when the graph exists, even when `recipe_h` is 0: that is
		// the mode that quantifies what the graph would be able to say BEFORE letting
		// it decide (instrument before calibrating).
		if(cfg.recipes) {
			const float rh = RecipeDistance(board_scratch, resolved);
			stats.recipe_h_sum += rh;
			++stats.recipe_h_count;
			hgoal += cfg.recipe_h * rh;
		}
		// LEARNED LANDMARKS: the SAME entry point as the recipe graph, and the same
		// discipline. Always EVALUATED as soon as the graph exists (the counter says
		// what it would be able to say), and it only WEIGHS when its weight is
		// non-zero.
		if(cfg.landmarks && !cfg.landmarks->Empty()) {
			const uint32_t lrem = LandmarkRemaining(board_scratch);
			hgoal += cfg.landmark_h * static_cast<float>(lrem);
		}
		// Eq. 7: the scale is h at the root of THIS search.
		if(idx == 0 && cfg.reroot_h > 0) {
			h_root = (std::max)(1.0, static_cast<double>(hgoal));
			stats.h_root = h_root;
		}
		// sqrt-LTS-H: the re-rooting weight of THIS node. The root is 1 by the
		// paper's convention; elsewhere, the further the node is from the goal, the
		// more expensive re-rooting on it is.
		double inv_w = 1.0;
		if(cfg.reroot_h > 0 && idx != 0)
			inv_w = std::exp(static_cast<double>(cfg.reroot_h) * hgoal / h_root);
		// sqrt-LTS: is this node a HINT? The root always is (otherwise the nodes
		// before the first hint would have no re-rooting ancestor). On re-rooting the
		// segment restarts: the relative probability returns to 1, so lambda/pi
		// returns to 1.
		double base_lam = nodes[idx].lam;
		float base_seg = nodes[idx].seg_logpi;
		if(cfg.levin_reroot &&
		   (idx == 0 || got != nodes[idx].placed_parent)) {
			base_lam = 1.0;
			base_seg = 0.0f;
			++stats.reroots;
		}

		// Softmax of the policy over the choices, the same logits as the NRPA
		// rollouts with ONE exception, which must be named: the snapshot biases
		// (--assign-bias, --op-bias, --phase-w) do not exist here, for lack of the
		// `snap_*` lists only the rollout maintains.
		// The TEMPERATURE, on the other hand, is now applied: the policy was LEARNED
		// under logits/tau (PolicyRollout and AdaptRun both divide), so reading it
		// without dividing changed its meaning as soon as --nrpa-temp != 1. At
		// temp = 1 (the default), unchanged byte for byte.
		const double finv_t =
			1.0 / (cfg.nrpa_temp > 1e-3f ? cfg.nrpa_temp : 1e-3f);
		const size_t nc = ro_choices.size();
		logit.resize(nc);
		double mx = -1e300;
		for(size_t i = 0; i < nc; ++i) {
			const Choice& c = ro_choices[i];
			const bool known = plan_index.count(c.plan_key) != 0 && !c.phase;
			// Same guard as in the rollout: the HINT bias does not act on an
			// approximate identity. This site did not even test the old flag, which
			// made it a second hole.
			const bool hinted = c.card && !IsSubsetPrompt(prompt_type) &&
								!cfg.hint_cards.empty() &&
								std::find(cfg.hint_cards.begin(),
										  cfg.hint_cards.end(),
										  c.card) != cfg.hint_cards.end();
			auto pit = pol.find(c.plan_key);
			const double w = (pit == pol.end()) ? 0.0 : pit->second;
			logit[i] = (w + (known ? cfg.nrpa_bias_known : 0.0f) +
						(hinted ? cfg.hint_bias : 0.0f)) *
					   finv_t;
			mx = (std::max)(mx, logit[i]);
		}
		// MACRO EDGES. Same guards as in the rollout: ONE macro per first key (the
		// best ranked among the compatible ones), NO inherited bias (the weight is
		// the one the policy learned on its id). The positional window makes no
		// sense here (the finisher counts no decisions since a rollout start) and
		// it is refuted: only the SEMANTIC guard applies.
		//
		// So the softmax denominator grows by `lapp.size()`: the atomic edges become
		// LESS probable. That is exactly why benchmark 0's expansion count is no
		// longer the control (see SearchConfig::finisher_options).
		// SearchConfig::finisher_options).
		lapp.clear();
		if(cfg.finisher_options && cfg.options) {
			uint16_t mctx = 0;
			if(cfg.options->ctx_tol >= 0)
				mctx = ContextKey(
					CommonCodes(board_scratch.codes, target.codes),
					duel.Count(static_cast<uint8_t>(cfg.target_player),
							   LOCATION_HAND));
			for(size_t i = 0; i < nc; ++i) {
				auto it = cfg.options->by_first.find(ro_choices[i].plan_key);
				if(it == cfg.options->by_first.end())
					continue;
				for(uint32_t mi : it->second) {
					if(!cfg.options->CtxOk(mi, mctx))
						continue;
					lapp.emplace_back(mi, static_cast<uint32_t>(i));
					break;   // one macro per first key
				}
			}
			for(const auto& mp : lapp) {
				auto pit = pol.find(cfg.options->ids[mp.first]);
				// Same temperature as the atomic edges: a half-tempered softmax is the
				// softmax of no policy at all.
				logit.push_back(((pit == pol.end()) ? 0.0 : pit->second) *
								finv_t);
				mx = (std::max)(mx, logit.back());
				++stats.macro_taken;
			}
		}
		double sum = 0;
		for(double& x : logit) {
			x = std::exp(x - mx);
			sum += x;
		}
		for(size_t i = 0; i < logit.size(); ++i) {
			LNode child;
			child.parent = static_cast<int32_t>(idx);
			child.depth = ndepth + 1;
			child.logpi = nlogpi + static_cast<float>(std::log(
				(std::max)(logit[i] / sum, 1e-30)));
			if(i < nc) {
				child.response = ro_choices[i].response;
				child.rdepth = nrdepth + 1;
			} else {
				// Macro edge: its answer is the macro's first move; its TREE DEPTH
				// stays 1, which is the whole compression, but it advances by
				// |macro| real decisions.
				const auto& mp = lapp[i - nc];
				child.response = ro_choices[mp.second].response;
				child.macro = mp.first;
				child.rdepth = nrdepth +
					static_cast<uint32_t>(cfg.options->seqs[mp.first].size());
			}
			double lcost;
			if(cfg.reroot_h > 0) {
				// Eq. 3 of arXiv:2605.30664, kept in O(1). Only two candidates:
				// EXTEND the ancestor that already minimised the cost at the parent,
				// or RE-ROOT on the parent itself. The second is always available and
				// is worth (1/w_parent)/pi: it BOUNDS the cost, which makes the
				// mechanism numerically stable where d/pi overflows. The gap with the
				// exact min over all the ancestors is therefore bounded by that term,
				// and one-sided (we never underestimate).
				const double pi_c = (std::max)(logit[i] / sum, 1e-30);
				const double ext_u = nodes[idx].hu / pi_c;
				const double ext_v = nodes[idx].hv + ext_u;
				const double new_u = inv_w / pi_c;
				// OVERFLOW. A few levels at pi_c ~ 1e-30 are enough to
				// send hu to infinity; `ext_v <= new_u` then becomes false for a
				// purely ARITHMETIC reason, the "re-rooting" branch is taken, and
				// stats.reroots, whose comment says it is the instrument of "at zero
				// the mechanism is inert", becomes non-zero exactly when the
				// computation broke. The two are counted separately.
				//
				// BUT the ROOT carries hu = hv = +inf BY CONVENTION: it has no
				// ancestor to extend, so its children MUST re-root, and that is
				// correct. Counting them as an overflow was a FALSE POSITIVE, and it
				// lit the flag on every line of every root: the instrument accused
				// the mechanism of what is its definition. A real overflow is one
				// starting from a FINITE parent.
				const bool parent_finite = std::isfinite(nodes[idx].hu) &&
										   std::isfinite(nodes[idx].hv);
				const bool overflowed =
					parent_finite &&
					(!std::isfinite(ext_v) || !std::isfinite(ext_u));
				if(overflowed)
					++stats.levin_overflow;
				if(ext_v <= new_u) {
					child.hv = ext_v;
					child.hu = ext_u;
				} else {
					child.hv = new_u;
					child.hu = new_u;
					if(overflowed)
						++stats.reroot_by_overflow;
					// The counter no longer says "a hint landed" (with this rerooter
					// every node is one) but "re-rooting BEAT extension": at zero the
					// mechanism is inert and the A/B measures nothing.
					++stats.reroots;
				}
				child.placed_parent = static_cast<uint16_t>(got);
				lcost = std::log((std::max)(child.hv, 1e-300)) +
						static_cast<double>(cfg.levin_h) * hgoal;
			} else if(cfg.levin_reroot) {
				child.seg_logpi = base_seg + static_cast<float>(std::log(
					(std::max)(logit[i] / sum, 1e-30)));
				// lambda/pi(child ; n_k) = lambda/pi(parent ; n_k) +
				// 1/pi(child | n_k). The added term is bounded by the SEGMENT's cost,
				// not by the whole line's: that is what avoids the underflow which
				// makes d/pi unusable here.
				const double inv = std::exp(-static_cast<double>(child.seg_logpi));
				// SATURATION. `seg_logpi` is a float accumulating log-probabilities;
				// past ~709 in absolute value, exp overflows and the guard
				// substitutes 1e300. From then on log(lam - 1) is CONSTANT for the
				// whole descent and the sqrt-LTS best-first degenerates into its
				// tie-break, so a --reroot A/B would be measuring a mechanism that
				// switched itself off. Counted; non-zero, the arm is to be thrown
				// away, not interpreted.
				if(!std::isfinite(inv))
					++stats.levin_overflow;
				child.lam = base_lam + (std::isfinite(inv) ? inv : 1e300);
				if(child.lam >= 1e300)
					++stats.lam_saturated;
				child.placed_parent = static_cast<uint16_t>(got);
				// sqrt-LTS cost (Eq. 14): lambda/pi - 1, in log to stay commensurable
				// with the PHS* heuristic term. The re-rooting weights being uniform,
				// their divisor is a constant and the best-first is invariant under
				// it.
				lcost = std::log((std::max)(child.lam - 1.0, 1e-300)) +
						static_cast<double>(cfg.levin_h) * hgoal;
			} else {
				lcost = std::log(static_cast<double>(child.depth) + 1.0) +
						static_cast<double>(cfg.levin_h) * hgoal - child.logpi;
			}
			++stats.levin_children;
			nodes.push_back(std::move(child));
			pq.push({ lcost, static_cast<uint32_t>(nodes.size() - 1) });
		}
	}
	const bool drained = pq.empty();
	while(!dive.empty()) {
		arena.Pop();
		dive.pop_back();
	}
	arena.Pop();

	stats.ms = std::chrono::duration<double, std::milli>(
				   std::chrono::steady_clock::now() - start).count();
	// Exhausted = the queue emptied: the space reachable under these bounds was
	// ENTIRELY enumerated, the single-state finisher's proof of absence, but per
	// root this time.
	stats.exhausted = drained && !stats.hit_time_limit && !stats.hit_node_limit;
}

void Search::RunTransplant(const BoardKey& t, const std::vector<PlanStep>& p,
						   uint32_t discrepancies) {
	prof::Scope ps(prof::kSearch);
	InitSerialBase();
	target = t;
	plan = &p;
	start = std::chrono::steady_clock::now();
	solutions.clear();
	path.clear();
	tt.clear();
	novelty.Clear();
	cfg_discrepancies = discrepancies;
	plan_index.clear();
	for(size_t i = 0; i < p.size(); ++i)
		if(p[i].edge)
			plan_index.emplace(p[i].edge, i);
	DescendTransplant(0, 0, discrepancies, 0, 0, 0, 0);
	stats.ms = std::chrono::duration<double, std::milli>(
				   std::chrono::steady_clock::now() - start).count();
	stats.hit_node_limit = stats.nodes >= cfg.max_nodes;
	stats.exhausted = !stats.hit_time_limit && !stats.hit_node_limit;
	stats.novelty_atoms = novelty.Size();
}

size_t LiftPlan(Duel& duel, Arena& arena, const Replay& yrp, int target_player,
				size_t stop_after, const EnumOptions& eo,
				std::vector<PlanStep>& out) {
	size_t unknown = 0, ri = 0;
	uint8_t ptype = 0;
	std::vector<uint8_t> payload;
	int player = -1;

	// Applies an answer and returns the fingerprint of the state reached, prompt
	// included. 0 signals a rejection by the core (so no match is possible); a
	// collision with a legitimate fingerprint is negligible.
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
					// plan_key and not edge: it is the identity stripped of what board
					// equivalence already ignores (the column). Putting the edge there
					// would make every zone choice unmatchable.
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

// THE FACTS OF A STATE. Five zone queries: that is expensive, and it does not
// matter, since this function only runs OFFLINE, when replaying the corpus of
// resolved plans (a handful of lines of a few hundred decisions). The hot path
// records ONLY the landmark keys (Search::LandmarkRemaining), which is a
// completely different bill.
void CollectStateFacts(Duel& duel, uint8_t con,
					   std::unordered_map<uint64_t, uint32_t>& out) {
	out.clear();
	static thread_local std::vector<QueriedCard> rq;
	for(uint32_t loc : { LOCATION_MZONE, LOCATION_SZONE, LOCATION_GRAVE,
						 LOCATION_REMOVED, LOCATION_HAND }) {
		duel.Query(con, loc, QUERY_CODE | QUERY_ALIAS, rq);
		for(const QueriedCard& c : rq)
			if(c.present)
				++out[LandmarkGraph::KeyOf(
					duel.Db().Canonical(c.Code()),
					NormalizeZone(static_cast<uint8_t>(loc)))];
	}
	// THE OPPONENT'S ZONES, and this is not a refinement.
	//
	// Half of benchmark B's goal is a HANDRIP: three cards torn from the
	// opponent's hand. A record limited to the target player cannot express it,
	// since no fact on its own half of the field becomes true when the opponent
	// loses a card. So the graph would have learned to build the board and stayed
	// MUTE on exactly the missing half, while looking as if it worked. That is the
	// most expensive failure of all (a diagnosis answering beside its own
	// question), avoided here before it cost a run.
	//
	// Marked by the 0x80 bit on the zone: "Ash Blossom in the OPPONENT's
	// graveyard" is a fact distinct from "Ash Blossom in mine".
	const uint8_t opp = con ^ 1;
	for(uint32_t loc : { LOCATION_GRAVE, LOCATION_REMOVED, LOCATION_HAND }) {
		duel.Query(opp, loc, QUERY_CODE | QUERY_ALIAS, rq);
		for(const QueriedCard& c : rq)
			if(c.present)
				++out[LandmarkGraph::KeyOf(
					duel.Db().Canonical(c.Code()),
					static_cast<uint8_t>(
						NormalizeZone(static_cast<uint8_t>(loc)) | 0x80))];
	}
}

size_t LiftPolicyRun(Duel& duel, Arena& arena, const Replay& yrp,
					 int target_player, size_t stop_after, const EnumOptions& eo,
					 const std::unordered_map<uint64_t, size_t>& repertoire,
					 const BoardKey& target, NrpaRun& out,
					 RecipeGraph* recipes, uint32_t mcps_depth,
					 LandmarkTrace* landmarks) {
	size_t unknown = 0, ri = 0;
	uint8_t ptype = 0;
	std::vector<uint8_t> payload;
	int player = -1;
	BoardKey here;
	out.score = 0;
	out.steps.clear();
	// The SAME path as in the rollout: mixed sum of the moves played over the
	// first `mcps_depth` recorded decisions of THIS line.
	uint64_t path_ctx = 0;
	uint32_t nsteps = 0;

	// CORPUS -> GRAPH: the summons of the replayed line are OBSERVED recipes, the
	// same logic as StepToPrompt (MSG_MOVE with REASON_MATERIAL accumulated,
	// MSG_SUMMONING/SPSUMMONING poured). This is where the exception routes the
	// text does not state enter.
	std::vector<Requirement> lift_mats;
	auto watch = [&](const Message& m) {
		if(!recipes)
			return;
		if(m.type == MSG_MOVE && m.size >= 28) {
			uint32_t code = 0, reason = 0;
			std::memcpy(&code, m.data, 4);
			std::memcpy(&reason, m.data + 24, 4);
			if(reason & REASON_MATERIAL)
				lift_mats.push_back(
					Requirement{ duel.Db().Canonical(code),
								 NormalizeZone(m.data[5]) });
		} else if((m.type == MSG_SUMMONING || m.type == MSG_SPSUMMONING) &&
				  m.size >= 4) {
			uint32_t code = 0;
			std::memcpy(&code, m.data, 4);
			if(code)
				recipes->Observe(duel.Db().Canonical(code), lift_mats);
			lift_mats.clear();
		}
	};

	// Same matching as LiftPlan: the recorded answer is identified by the STATE it
	// reaches, never byte by byte (EDOPro's encoding and the enumerator's differ).
	// 0 = rejected by the core.
	auto advance = [&](const std::vector<uint8_t>& resp) -> uint64_t {
		duel.SetResponse(resp);
		uint8_t nt = 0;
		std::vector<uint8_t> np;
		for(;;) {
			int status = duel.Process();
			for(const Message& m : duel.Messages()) {
				if(m.type == MSG_RETRY)
					return 0;
				watch(m);
				if(IsPrompt(m.type)) {
					nt = m.type;
					np.assign(m.data, m.data + m.size);
					// Step boundary: materials left with no summon must not stick to
					// the next one (the same semantics as StepToPrompt, which restarts
					// empty at every step).
					lift_mats.clear();
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
			watch(m);
			if(IsPrompt(m.type)) {
				ptype = m.type;
				payload.assign(m.data, m.data + m.size);
				player = m.size ? m.data[0] : -1;
				lift_mats.clear();
			}
		}
		if(status == OCG_DUEL_STATUS_END)
			break;
		if(status != OCG_DUEL_STATUS_AWAITING)
			continue;
		if(ri >= yrp.responses.size() || ri >= stop_after)
			break;

		const std::vector<uint8_t>& recorded = yrp.responses[ri];
		// LANDMARKS: the multiset of the facts BEFORE playing this decision. For
		// each fact we record the index at which its k-th copy appeared FOR THE
		// FIRST TIME, and that is where both the count (the paper's repetition
		// loop) and the order (the progress edges) come from.
		//
		// The recording covers ALL of the target player's decisions, single-choice
		// prompts included: a fact becomes true during a resolution, not only at a
		// junction. Filtering on multiple-choice prompts would have missed exactly
		// the facts that arrive "on their own", the ones a macro absorbs.
		if(landmarks && player == target_player) {
			static thread_local std::unordered_map<uint64_t, uint32_t> facts;
			CollectStateFacts(duel, static_cast<uint8_t>(target_player), facts);
			if(landmarks->decisions == 0)
				landmarks->initial = facts;
			for(const auto& [k, n] : facts) {
				std::vector<float>& v = landmarks->first[k];
				while(v.size() < n)
					v.push_back(static_cast<float>(landmarks->decisions));
			}
			++landmarks->decisions;
		}
		if(player == target_player) {
			auto choices = Enumerate(ptype, payload.data(),
									 static_cast<uint32_t>(payload.size()), eo);
			// A single-choice decision teaches nothing: the gradient there is
			// alpha - alpha*1 = 0. That is also PolicyRollout's convention, which only
			// records a PolicyStep from two choices on.
			if(choices.size() > 1) {
				arena.Push();
				uint64_t want = advance(recorded);
				arena.Restore();
				size_t pick = choices.size();
				for(size_t i = 0; i < choices.size(); ++i) {
					uint64_t got = advance(choices[i].response);
					arena.Restore();
					if(want && got == want) {
						pick = i;
						break;
					}
				}
				arena.Pop();
				if(pick < choices.size()) {
					PolicyStep step;
					// The CONTEXT, computed as in the rollout: target board cards
					// already placed at that instant of the line.
					ComputeBoardKeyInto(duel,
										static_cast<uint8_t>(target_player),
										here);
					step.ctx = ContextKey(
						CommonCodes(here.codes, target.codes),
						duel.Count(static_cast<uint8_t>(target_player),
								   LOCATION_HAND));
					step.keys.reserve(choices.size());
					step.known.reserve(choices.size());
					for(const Choice& c : choices) {
						step.keys.push_back(c.plan_key);
						// The repertoire bias is reproduced here because Adapt() must
						// recompute the SAME probabilities as the sampling (see
						// PolicyRollout: a phase change is in the repertoire without
						// deserving a bias there).
						step.known.push_back(
							(repertoire.count(c.plan_key) != 0 && !c.phase) ? 1
																			: 0);
					}
					step.chosen = pick;
					step.cctx = mcps_depth ? path_ctx
										   : static_cast<uint64_t>(step.ctx);
					if(mcps_depth && nsteps < mcps_depth)
						path_ctx += MixMove(choices[pick].plan_key);
					++nsteps;
					out.steps.push_back(std::move(step));
				} else {
					++unknown;
					// An unidentified step BREAKS the path: we do not know which move
					// the line played, so it cannot be added. The context of the
					// following steps will no longer match that of a rollout having
					// played the same line, which is an acknowledged underestimate,
					// like the skip itself.
					++nsteps;
				}
			}
		}
		duel.SetResponse(recorded);
		++ri;
	}
	return unknown;
}

double CorpusAgreement(const NrpaPolicy& pol, const NrpaResidual* res,
					   const std::vector<NrpaRun>& runs, float bias_known,
					   float shrink, double* argmax_frac) {
	double total = 0;
	size_t n = 0, top = 0;
	std::vector<double> p;
	for(const NrpaRun& r : runs) {
		for(const PolicyStep& s : r.steps) {
			p.resize(s.keys.size());
			double mx = -1e300;
			for(size_t i = 0; i < s.keys.size(); ++i) {
				double w = EffectiveWeight(pol, res, s.keys[i], s.cctx, shrink);
				p[i] = w + (s.known[i] ? bias_known : 0.0f);
				mx = (std::max)(mx, p[i]);
			}
			double sum = 0;
			for(double& x : p) { x = std::exp(x - mx); sum += x; }
			total += std::log((std::max)(p[s.chosen] / sum, 1e-30));
			bool first = true;
			for(size_t i = 0; i < p.size(); ++i)
				if(i != s.chosen && p[i] > p[s.chosen]) { first = false; break; }
			top += first ? 1 : 0;
			++n;
		}
	}
	if(argmax_frac)
		*argmax_frac = n ? static_cast<double>(top) / static_cast<double>(n) : 0.0;
	return n ? total / static_cast<double>(n) : 0.0;
}

CostForecast ForecastSearchCost(const NrpaPolicy& pol, const NrpaResidual* res,
								const std::vector<NrpaRun>& runs,
								float bias_known, float shrink) {
	CostForecast out;
	std::vector<double> p;
	double sum_mono = 0, sum_decomp = 0, sum_q = 0, sum_worst = 0;
	for(const NrpaRun& r : runs) {
		if(r.steps.empty())
			continue;
		// Current segment: depth and log-probability accumulated since the last
		// hint. A hint = the "cards placed" component of the context increases (a
		// subtask of the conjunctive goal has just fallen).
		double seg_logpi = 0, total_logpi = 0;
		uint32_t seg_depth = 0, total_depth = 0;
		uint16_t last_placed = static_cast<uint16_t>(r.steps.front().ctx / 16);
		// Accumulated bounds: sum of the d_i/pi_i, in log10 per segment then summed
		// linearly through the max (the terms span enormous orders of magnitude, and
		// summing naively would lose everything).
		double worst_log10 = -1e300;
		std::vector<double> seg_log10;
		auto close_segment = [&] {
			if(!seg_depth)
				return;
			const double l = std::log10(static_cast<double>(seg_depth)) -
							 seg_logpi / std::log(10.0);
			seg_log10.push_back(l);
			worst_log10 = (std::max)(worst_log10, l);
			seg_logpi = 0;
			seg_depth = 0;
		};
		for(const PolicyStep& s : r.steps) {
			const uint16_t placed = static_cast<uint16_t>(s.ctx / 16);
			if(placed != last_placed) {
				close_segment();
				last_placed = placed;
			}
			p.resize(s.keys.size());
			double mx = -1e300;
			for(size_t i = 0; i < s.keys.size(); ++i) {
				double w = EffectiveWeight(pol, res, s.keys[i], s.cctx, shrink);
				p[i] = w + (s.known[i] ? bias_known : 0.0f);
				mx = (std::max)(mx, p[i]);
			}
			double sm = 0;
			for(double& x : p) { x = std::exp(x - mx); sm += x; }
			const double lp = std::log((std::max)(p[s.chosen] / sm, 1e-300));
			seg_logpi += lp;
			total_logpi += lp;
			++seg_depth;
			++total_depth;
		}
		close_segment();
		if(seg_log10.empty())
			continue;
		// Sum of the per-segment bounds, in stable log10 (log-sum-exp base 10).
		double acc = 0;
		for(double l : seg_log10)
			acc += std::pow(10.0, l - worst_log10);
		const double decomp = worst_log10 + std::log10(acc);
		const double mono = std::log10(static_cast<double>(total_depth)) -
							total_logpi / std::log(10.0);
		sum_mono += mono;
		sum_decomp += decomp;
		sum_q += static_cast<double>(seg_log10.size());
		sum_worst += worst_log10;
		++out.lines;
	}
	if(out.lines) {
		const double n = static_cast<double>(out.lines);
		out.log10_mono = sum_mono / n;
		out.log10_decomp = sum_decomp / n;
		out.segments = sum_q / n;
		out.worst_seg_log10 = sum_worst / n;
	}
	return out;
}

OptionForecast ForecastOptionGain(const std::vector<NrpaRun>& runs,
								  size_t max_options, uint32_t min_support,
								  size_t max_len) {
	OptionForecast out;
	// The corpus lines, reduced to the SEQUENCE OF MOVES PLAYED. That is what the
	// macros are mined on, and what the substitution is measured on.
	std::vector<std::vector<uint64_t>> played;
	std::vector<std::vector<uint32_t>> legal;
	for(const NrpaRun& r : runs) {
		if(r.steps.empty())
			continue;
		played.emplace_back();
		legal.emplace_back();
		for(const PolicyStep& s : r.steps) {
			played.back().push_back(s.keys[s.chosen]);
			legal.back().push_back(
				static_cast<uint32_t>((std::max)(s.keys.size(), size_t{ 1 })));
		}
	}
	if(played.empty())
		return out;

	// MINING. The support of a subsequence = the number of occurrences over the
	// whole corpus. A macro is only worth keeping when it comes back: a
	// subsequence seen once is a line, not an option.
	std::map<std::vector<uint64_t>, uint32_t> support;
	for(const std::vector<uint64_t>& seq : played)
		for(size_t i = 0; i < seq.size(); ++i)
			for(size_t len = 2; len <= max_len && i + len <= seq.size(); ++len)
				++support[std::vector<uint64_t>(seq.begin() + i,
												seq.begin() + i + len)];

	// SELECTION. Raw gain of a macro = (len - 1) decisions saved per occurrence.
	// We sort on that and take the first `max_options`: an exact greedy selection
	// (re-evaluating the loss after each addition) would cost an order of
	// magnitude more for an instrument whose role is to say YES or NO, not to
	// deliver the definitive catalogue.
	std::vector<std::pair<double, std::vector<uint64_t>>> ranked;
	for(const auto& [seq, n] : support) {
		if(n < min_support)
			continue;
		ranked.emplace_back(static_cast<double>(n) *
								static_cast<double>(seq.size() - 1),
							seq);
	}
	std::sort(ranked.begin(), ranked.end(),
			  [](const auto& a, const auto& b) {
				  if(a.first != b.first) return a.first > b.first;
				  return a.second.size() > b.second.size();
			  });
	std::vector<std::vector<uint64_t>> catalog;
	for(const auto& [gain, seq] : ranked) {
		if(catalog.size() >= max_options)
			break;
		catalog.push_back(seq);
		out.max_len = (std::max)(out.max_len, seq.size());
	}
	out.options = catalog.size();
	if(catalog.empty())
		return out;

	// SUBSTITUTION AND COST. The matching is GREEDY, longest first: that is what
	// an enumerator offering its macros would do.
	const double m = static_cast<double>(catalog.size());
	double sum_flat = 0, sum_opt = 0, sum_dflat = 0, sum_dopt = 0;
	size_t absorbed = 0, total_steps = 0;
	for(size_t li = 0; li < played.size(); ++li) {
		const std::vector<uint64_t>& seq = played[li];
		double logpi_flat = 0, logpi_opt = 0;
		size_t d_opt = 0;
		for(size_t i = 0; i < seq.size();) {
			// Cost without options: one decision, among its legal moves.
			logpi_flat += std::log10(static_cast<double>(legal[li][i]));
			size_t best = 0;
			for(const std::vector<uint64_t>& opt : catalog) {
				if(opt.size() <= best || i + opt.size() > seq.size())
					continue;
				if(std::equal(opt.begin(), opt.end(), seq.begin() + i))
					best = opt.size();
			}
			// The denominator carries the WHOLE catalogue: we assume every option is
			// offered everywhere, which UNDERSTATES the gain.
			logpi_opt += std::log10(static_cast<double>(legal[li][i]) + m);
			++d_opt;
			if(best) {
				absorbed += best;
				// The absorbed decisions keep their cost IN the flat arm, which must
				// stay the complete line.
				for(size_t k = 1; k < best; ++k)
					logpi_flat +=
						std::log10(static_cast<double>(legal[li][i + k]));
				i += best;
			} else {
				++i;
			}
		}
		total_steps += seq.size();
		sum_flat += std::log10(static_cast<double>(seq.size())) + logpi_flat;
		sum_opt += std::log10(static_cast<double>(d_opt)) + logpi_opt;
		sum_dflat += static_cast<double>(seq.size());
		sum_dopt += static_cast<double>(d_opt);
		++out.lines;
	}
	if(out.lines) {
		const double n = static_cast<double>(out.lines);
		out.log10_flat = sum_flat / n;
		out.log10_opt = sum_opt / n;
		out.depth_flat = sum_dflat / n;
		out.depth_opt = sum_dopt / n;
		out.covered = total_steps ? static_cast<double>(absorbed) /
										static_cast<double>(total_steps)
								  : 0.0;
	}
	return out;
}

// EXECUTABLE option catalogue: mining of the corpus's subsequences, then
// GREEDY SELECTION BY LEVIN LOSS instead of raw gain, which is the paper's
// criterion (2410.11262). Under a uniform policy the model evaluates
// Sigma log10(b_i + offerable_i) along each line with the BEST segmentation
// (dynamic programming): a macro pays for its presence in the denominator
// EVERYWHERE it can be offered, and only gets credit where the line played
// it. The selection stops when no candidate improves the loss any more, so the
// catalogue's size is a RESULT, not a parameter; max_options is only a
// ceiling.
OptionCatalog MineOptionCatalog(const std::vector<NrpaRun>& runs,
								size_t max_options, uint32_t min_support,
								size_t max_len, uint32_t window, int ctx_tol) {
	OptionCatalog cat;
	cat.window = window;
	cat.ctx_tol = ctx_tol;
	// 1. The lines: move played, legal width, legal keys and semantic context
	// per decision.
	struct Line {
		std::vector<uint64_t> played;
		std::vector<uint32_t> legal;
		std::vector<const std::vector<uint64_t>*> keys;
		std::vector<uint16_t> ctx;
	};
	std::vector<Line> lines;
	for(const NrpaRun& r : runs) {
		if(r.steps.empty())
			continue;
		lines.emplace_back();
		for(const PolicyStep& s : r.steps) {
			lines.back().played.push_back(s.keys[s.chosen]);
			lines.back().legal.push_back(static_cast<uint32_t>(
				(std::max)(s.keys.size(), size_t{ 1 })));
			lines.back().keys.push_back(&s.keys);
			lines.back().ctx.push_back(s.ctx);
		}
	}
	if(lines.empty())
		return cat;

	// 2. The pool: subsequences with support >= min_support, mean occurrence
	// position, occurrence start contexts, capped at the 1024 best by raw gain
	// (an exact greedy over the whole lattice would cost an order more).
	struct Cand {
		std::vector<uint64_t> seq;
		uint32_t n = 0;
		uint32_t pos = 0;
		double brut = 0;
		std::vector<uint16_t> ctxs;
	};
	struct SupEntry {
		uint32_t n = 0;
		uint64_t pos = 0;
		std::vector<uint16_t> ctxs;
	};
	std::map<std::vector<uint64_t>, SupEntry> sup;
	for(const Line& L : lines)
		for(size_t i = 0; i < L.played.size(); ++i)
			for(size_t len = 2; len <= max_len && i + len <= L.played.size();
				++len) {
				auto& e = sup[std::vector<uint64_t>(
					L.played.begin() + i, L.played.begin() + i + len)];
				++e.n;
				e.pos += i;
				e.ctxs.push_back(L.ctx[i]);
			}
	std::vector<Cand> pool;
	for(auto& [seq, e] : sup) {
		if(e.n < min_support)
			continue;
		std::sort(e.ctxs.begin(), e.ctxs.end());
		e.ctxs.erase(std::unique(e.ctxs.begin(), e.ctxs.end()), e.ctxs.end());
		pool.push_back({ seq, e.n, static_cast<uint32_t>(e.pos / e.n),
						 static_cast<double>(e.n) *
							 static_cast<double>(seq.size() - 1),
						 std::move(e.ctxs) });
	}
	std::sort(pool.begin(), pool.end(), [](const Cand& a, const Cand& b) {
		if(a.brut != b.brut) return a.brut > b.brut;
		return a.seq.size() > b.seq.size();
	});
	constexpr size_t kPool = 1024;
	if(pool.size() > kPool)
		pool.resize(kPool);
	if(pool.empty())
		return cat;

	// 3. Precomputations per global position: where each candidate is OFFERABLE
	// (first key legal + window, the same test as the rollout) and where it is
	// PLAYED (the line played exactly its sequence here).
	std::vector<size_t> line_base(lines.size());
	size_t total = 0;
	for(size_t li = 0; li < lines.size(); ++li) {
		line_base[li] = total;
		total += lines[li].played.size();
	}
	std::unordered_map<uint64_t, std::vector<uint32_t>> cand_by_first;
	for(size_t ci = 0; ci < pool.size(); ++ci)
		cand_by_first[pool[ci].seq.front()].push_back(
			static_cast<uint32_t>(ci));
	auto in_window = [&](const Cand& c, size_t i) {
		if(!window)
			return true;
		const uint32_t p = c.pos;
		return i + window >= p && i <= size_t{ p } + window;
	};
	// The semantic guard of the selection model: the SAME test as the
	// rollout (OptionCatalog::CtxOk), otherwise the model loss would be
	// computed on a denominator the run does not pay.
	auto ctx_ok = [&](const Cand& c, uint16_t ctx) {
		if(ctx_tol < 0)
			return true;
		for(uint16_t w : c.ctxs)
			if(OptionCtxCompatible(ctx, w, static_cast<uint32_t>(ctx_tol)))
				return true;
		return false;
	};
	std::vector<std::vector<uint32_t>> proposable(total), playable(total);
	for(size_t li = 0; li < lines.size(); ++li) {
		const Line& L = lines[li];
		for(size_t i = 0; i < L.played.size(); ++i) {
			const size_t g = line_base[li] + i;
			for(uint64_t k : *L.keys[i]) {
				auto it = cand_by_first.find(k);
				if(it == cand_by_first.end())
					continue;
				for(uint32_t ci : it->second)
					if(in_window(pool[ci], i) && ctx_ok(pool[ci], L.ctx[i]) &&
					   (proposable[g].empty() || proposable[g].back() != ci))
						proposable[g].push_back(ci);
			}
			auto it = cand_by_first.find(L.played[i]);
			if(it == cand_by_first.end())
				continue;
			for(uint32_t ci : it->second) {
				const auto& s = pool[ci].seq;
				if(in_window(pool[ci], i) && ctx_ok(pool[ci], L.ctx[i]) &&
				   i + s.size() <= L.played.size() &&
				   std::equal(s.begin(), s.end(), L.played.begin() + i))
					playable[g].push_back(ci);
			}
		}
	}

	// 4. Greedy selection. prop_count[g] = offerable macros of the current
	// catalogue at position g; evaluating a candidate adds its own bit on the
	// fly. Beam: the 64 best remaining raw candidates.
	std::vector<uint32_t> prop_count(total, 0);
	std::vector<char> chosen_mask(pool.size(), 0);
	std::vector<double> cost;
	auto loss = [&](int extra) -> double {
		double sum = 0;
		for(size_t li = 0; li < lines.size(); ++li) {
			const Line& L = lines[li];
			const size_t n = L.played.size();
			cost.assign(n + 1, 0.0);
			for(size_t i = n; i-- > 0;) {
				const size_t g = line_base[li] + i;
				uint32_t extra_here = 0;
				if(extra >= 0)
					for(uint32_t ci : proposable[g])
						if(ci == static_cast<uint32_t>(extra)) {
							extra_here = 1;
							break;
						}
				const double denom = std::log10(static_cast<double>(
					L.legal[i] + prop_count[g] + extra_here));
				double best = denom + cost[i + 1];
				for(uint32_t ci : playable[g])
					if(chosen_mask[ci] ||
					   (extra >= 0 && ci == static_cast<uint32_t>(extra))) {
						const double c =
							denom + cost[i + pool[ci].seq.size()];
						best = (std::min)(best, c);
					}
				cost[i] = best;
			}
			sum += cost[0];
		}
		return sum / static_cast<double>(lines.size());
	};
	cat.model_flat = loss(-1);
	double current = cat.model_flat;
	std::vector<uint32_t> chosen;
	constexpr size_t kBeam = 64;
	while(chosen.size() < max_options) {
		double best_loss = current;
		int best_ci = -1;
		size_t seen = 0;
		for(size_t ci = 0; ci < pool.size() && seen < kBeam; ++ci) {
			if(chosen_mask[ci])
				continue;
			++seen;
			const double l = loss(static_cast<int>(ci));
			if(l < best_loss) {
				best_loss = l;
				best_ci = static_cast<int>(ci);
			}
		}
		// No candidate of the beam improves the loss any more: the catalogue has
		// reached its NATURAL size.
		if(best_ci < 0)
			break;
		chosen_mask[best_ci] = 1;
		chosen.push_back(static_cast<uint32_t>(best_ci));
		current = best_loss;
		for(size_t g = 0; g < total; ++g)
			for(uint32_t ci : proposable[g])
				if(ci == static_cast<uint32_t>(best_ci)) {
					++prop_count[g];
					break;
				}
	}
	cat.model_opt = current;

	// 5. The catalogue, in selection order (front() = the best).
	for(uint32_t ci : chosen) {
		cat.seqs.push_back(pool[ci].seq);
		cat.pos.push_back(pool[ci].pos);
		cat.ctxs.push_back(pool[ci].ctxs);
	}
	for(size_t m = 0; m < cat.seqs.size(); ++m) {
		// Identity of the macro in the weight space. An id/plan_key collision is of
		// the same order as one between two plan_keys: neglected on both sides.
		uint64_t id = 0x9E3779B97F4A7C15ull;
		for(uint64_t k : cat.seqs[m])
			id = (id ^ k) * 0x100000001B3ull;
		cat.ids.push_back(id);
		cat.by_first[cat.seqs[m].front()].push_back(static_cast<uint32_t>(m));
	}
	return cat;
}

double CorpusCoherence(const std::vector<NrpaRun>& runs, bool use_ctx,
					   size_t* groups) {
	// signature of the decision point -> (move played -> number of times)
	std::unordered_map<uint64_t, std::unordered_map<uint64_t, uint32_t>> seen;
	std::vector<uint64_t> sorted;
	size_t total = 0;
	for(const NrpaRun& r : runs) {
		for(const PolicyStep& s : r.steps) {
			sorted = s.keys;
			std::sort(sorted.begin(), sorted.end());
			uint64_t sig = use_ctx ? (s.cctx + 1) * 0x9e3779b97f4a7c15ull : 0;
			for(uint64_t k : sorted)
				sig = (sig ^ k) * 0x100000001b3ull;
			++seen[sig][s.keys[s.chosen]];
			++total;
		}
	}
	if(groups)
		*groups = seen.size();
	if(!total)
		return 0.0;
	size_t majority = 0;
	for(const auto& [sig, choices] : seen) {
		uint32_t best = 0;
		for(const auto& [k, n] : choices)
			best = (std::max)(best, n);
		majority += best;
	}
	return static_cast<double>(majority) / static_cast<double>(total);
}

// PROGRESS ON THE MATERIAL BALANCE'S SUBGOALS.
//
// Returns the sum of the requirements SERVED: for each place `x*` demands, how
// many tokens are already there, capped by what the plan requires. So the
// number climbs from the first brick, which is the whole difference with
// `CommonCodes`, which only counts the target cards PLACED and stays flat over
// the whole climb.
//
// COST: the zone queries are only made when requirements exist. Mechanism off
// = zero queries, hence zero throughput regression on the control arms. That
// is the guard three earlier mechanisms lacked.
uint32_t SerialProgress(Duel& duel, uint8_t con,
						const std::vector<SearchConfig::SerialReq>& reqs,
						const CardDB& db, uint32_t res0, uint64_t* packed) {
	if(reqs.empty())
		return 0;
	static thread_local std::vector<uint32_t> zone_scratch;
	// AVAILABLE = everything that can serve as a material; FIELD = the monster
	// zone; GRAVEYARD (zone 3) = x*'s CONSUMPTION rungs, since every consuming
	// firing drops a body there, and that is what climbs during the deserts the
	// gap profile named. The lists are built once per call, then counted.
	// appel, puis comptees.
	std::vector<uint32_t> ava, fld, grv, rmv, fzn;
	for(uint32_t loc : { 0x02u, 0x04u, 0x08u, 0x10u, 0x20u }) {
		duel.QueryCodes(con, loc, zone_scratch);
		for(uint32_t c : zone_scratch)
			ava.push_back(db.Canonical(c));
		// IN PLAY (zone 6) = MZONE + SZONE: the enablers to count by presence are not
		// all monsters, since Masquerade is a CONTINUOUS spell and its concession
		// lives in the SZONE.
		if(loc == 0x04u || loc == 0x08u)
			for(uint32_t c : zone_scratch)
				fzn.push_back(db.Canonical(c));
		if(loc == 0x04u)
			for(uint32_t c : zone_scratch)
				fld.push_back(db.Canonical(c));
		if(loc == 0x10u)
			for(uint32_t c : zone_scratch)
				grv.push_back(db.Canonical(c));
		if(loc == 0x20u)
			for(uint32_t c : zone_scratch)
				rmv.push_back(db.Canonical(c));
	}
	uint32_t served = 0;
	uint32_t slot = 0;
	if(packed)
		*packed = 0;
	for(const SearchConfig::SerialReq& rq : reqs) {
		uint32_t have = 0;
		if(rq.zone == 5) {
			// RESERVE DEPARTURES: one unit per card that left deck+extra since the
			// root (`res0`). That is what climbs during the deserts the intermediate
			// summons dig.
			// `arch` acts as an OFFSET (slice): the packing caps each requirement at
			// 15, and the real line makes ~25 departures, so the first slice
			// saturated at answer 118, JUST before the deserts that had to be
			// covered.
			const uint32_t cur =
				duel.Count(con, 0x01u) + duel.Count(con, 0x40u);
			const uint32_t dep = res0 > cur ? res0 - cur : 0;
			const uint32_t off = static_cast<uint32_t>(rq.arch);
			have = dep > off ? dep - off : 0;
		} else {
			const std::vector<uint32_t>& pool =
				rq.zone == 2   ? fld
				: rq.zone == 3 ? grv
				: rq.zone == 4 ? rmv
				: rq.zone == 6 ? fzn
							   : ava;
			for(uint32_t c : pool) {
				if(rq.code) {
					if(c == rq.code)
						++have;
				} else if(const CardRow* row = db.Find(c)) {
					for(uint16_t sc : row->setcodes)
						if(sc && (sc & 0x0fffu) == (rq.arch & 0x0fffu)) {
							++have;
							break;
						}
				}
				if(have >= rq.count)
					break;
			}
		}
		const uint32_t got = (std::min)(have, rq.count);
		served += got;
		// THE VECTOR, AND NO LONGER ITS SUM. Thirty-three subgoal units aggregated
		// into one integer yielded only TEN archive rungs: "two bodies and a Leo"
		// and "three bodies" had the same key there, hence one cell, hence one
		// ladder rung. Packing each requirement on four bits distinguishes the
		// combinations, and it is the number of RUNGS that decides whether
		// `5.9^110` is cut into crossable blocks.
		// franchissables.
		if(packed && slot < 16)
			*packed |= static_cast<uint64_t>((std::min)(got, 15u))
					   << (slot * 4);
		++slot;
	}
	return served;
}

void LiftRefLine(Duel& duel, Arena& arena, const Replay& yrp, int target_player,
				 size_t stop_after, const EnumOptions& eo,
				 std::unordered_map<uint64_t, size_t>& digests,
				 std::vector<uint64_t>& keys, RefLineStats* stats) {
	uint8_t ptype = 0;
	std::vector<uint8_t> payload;
	int player = -1;
	size_t ri = 0;
	keys.assign((std::min)(yrp.responses.size(), stop_after), 0);
	if(stats) {
		stats->arity.assign(keys.size(), 0);
		stats->matched.assign(keys.size(), 0);
		stats->prompt.assign(keys.size(), 0);
	}

	// Same matching as LiftPlan: applies an answer, returns the fingerprint of the
	// state reached (0 = rejected, no match possible).
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
		// First occurrence kept, moot in practice: the reference's states are
		// pairwise distinct (the report verifies 0 merges).
		digests.emplace(StateDigest(duel, ptype, payload), ri);

		const std::vector<uint8_t>& recorded = yrp.responses[ri];
		if(player == target_player) {
			auto choices = Enumerate(ptype, payload.data(),
									 static_cast<uint32_t>(payload.size()), eo);
			if(stats && ri < stats->arity.size()) {
				stats->arity[ri] = static_cast<uint32_t>(choices.size());
				stats->prompt[ri] = ptype;
			}
			arena.Push();
			uint64_t want = advance(recorded);
			arena.Restore();
			bool hit = false;
			for(const Choice& c : choices) {
				uint64_t got = advance(c.response);
				arena.Restore();
				if(want && got == want) {
					hit = true;
					keys[ri] = c.plan_key;
					// FOUND: the move played IS in the action space. To be distinguished
					// from `plan_key != 0`, which only says it is in the repertoire; the
					// confusion between the two made a holed coverage read where there was
					// only a move without a key.
					if(stats && ri < stats->matched.size())
						stats->matched[ri] = 1;
					break;
				}
			}
			if(!hit && stats && stats->misses.size() < RefLineStats::kMaxMisses) {
				RefLineStats::Miss m;
				m.index = ri;
				m.prompt = ptype;
				m.recorded = recorded;
				for(const Choice& c : choices)
					m.offered.push_back(c.response);
				stats->misses.push_back(std::move(m));
			}
			arena.Pop();
		}
		duel.SetResponse(recorded);
		++ri;
	}
}

void Search::RunRepair(const BoardKey& t, uint32_t discrepancies) {
	prof::Scope ps(prof::kSearch);
	InitSerialBase();
	target = t;
	start = std::chrono::steady_clock::now();
	solutions.clear();
	path.clear();
	tt.clear();
	novelty.Clear();
	cfg_discrepancies = discrepancies;
	DescendRepair(0, 0, 0, discrepancies, 0, 0, 0, 0);
	stats.ms = std::chrono::duration<double, std::milli>(
				   std::chrono::steady_clock::now() - start).count();
	stats.hit_node_limit = stats.nodes >= cfg.max_nodes;
	stats.exhausted = !stats.hit_time_limit && !stats.hit_node_limit;
	stats.novelty_atoms = novelty.Size();
}

void Search::RunGuided(const BoardKey& t) {
	prof::Scope ps(prof::kSearch);
	InitSerialBase();
	target = t;
	start = std::chrono::steady_clock::now();
	solutions.clear();
	path.clear();
	tt.clear();
	DescendGuided(0, 0, cfg.initial_turns, cfg.initial_summons,
				  cfg.initial_resolved);
	stats.ms = std::chrono::duration<double, std::milli>(
				   std::chrono::steady_clock::now() - start).count();
	stats.hit_node_limit = stats.nodes >= cfg.max_nodes;
	stats.exhausted = !stats.hit_time_limit && !stats.hit_node_limit;
}

void Search::Run(const BoardKey& t) {
	prof::Scope ps(prof::kSearch);
	InitSerialBase();
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
