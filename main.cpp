// combosolver: instrumented replay, arena validation, and the search drivers.
//
// Faithfully reproduces the line played in a .yrpX, measures its cost and the
// branching the core offers at every decision, captures the target board, then
// checks that the memory snapshot restores a rigorously identical state.
// Without a faithful replay the target board is wrong; without a faithful
// restore, so is the whole search.

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "arena.h"
#include "assets.h"
#include "duel.h"
#include "enumerate.h"
#include "operators.h"
#include "prompt.h"
#include "replay.h"
#include "search.h"

using namespace solver;

namespace {

// OUTPUT LEVEL. The default report answers the question that was asked: the
// cost of the line, the target board, the verdict of the self-checks, the lines
// found. Everything that measures the SOLVER rather than the duel -- counters,
// probes, per-mechanism liveness, cost tables -- is instrumentation, and waits
// behind --verbose. Set once in main, before any thread exists.
bool g_verbose = false;

// The self-check summary, gathered as the checks run and printed as ONE line at
// the default level. The numbers are the ones the health gate is read on; only
// their layout changes when --verbose opens the full blocks.
std::string g_stress_line;      // passed/total/failed of the stress test
std::string g_enum_line;        // digests, coverage and candidates
std::string g_width_line;       // effective width (IW atoms)

using Clock = std::chrono::steady_clock;

double MsSince(Clock::time_point t0) {
	return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// The six mechanisms that REMOVE branches, per line and per pass. All of them
// were active in every disciplined run and none was quantified (a counter that
// is not printed is not an instrument).
//   constraint = --summon-min / --material   guard  = --guard
//   turn       = line overflowing turn 1     bound  = decision/action ceiling
//   partition  = branches ceded to another worker (ClaimTable)
//   subsets    = enumerations truncated by max_subsets
// A non-zero `bound` or `subsets` strips "EXHAUSTED" of its value as a proof of
// absence; a non-zero `partition` says the work was SHARED, not REMOVED, and
// that is a distinction the report must keep.
struct CutCounts {
	uint64_t constraint = 0, guard = 0, turn = 0, bound = 0, claim = 0,
			 subsets = 0, selfneg = 0, guardkeep = 0;
	// --- what is NOT pruning, but an amputation of the space ---
	uint64_t forced = 0;          // prompts reduced to the default answer
	uint64_t forced_mask = 0;     // which prompt types
	// DISTINCT FROM THE ABOVE: here the branch does NOT survive. The single
	// counter mixed the two and counted the second family twice (it also falls
	// into `dead_ends`), hence the "90 forced / 90 dead ends" that was ONE fact
	// and not two.
	uint64_t killed = 0;
	uint64_t killed_mask = 0;
	// --- search health, never printed before ---
	uint64_t dead_ends = 0, terminals = 0;
	uint64_t novel = 0, stale = 0;   // novelty rate of the rollouts
	size_t atoms = 0;                // measured width of the atom table
	uint64_t num_broken = 0;         // broken sqrt-LTS arithmetic
	// --- recipe graph ---
	uint64_t recipes_seen = 0;       // summons observed and poured in
	double recipe_h_sum = 0.0;       // sum of the distances evaluated
	uint64_t recipe_h_count = 0;
	// --- landmark graph ---
	double landmark_h_sum = 0.0;
	uint64_t landmark_h_count = 0;
	void Add(const SearchStats& s) {
		constraint += s.constraint_cuts;
		guard      += s.guard_cuts;
		selfneg    += s.self_negate_cuts;
		guardkeep  += s.guard_keep_cuts;
		turn       += s.turn_cuts;
		bound      += s.edges_skipped;
		claim      += s.claim_denied;
		subsets    += s.subsets_capped;
		forced     += s.forced_default;
		forced_mask |= s.forced_default_prompts;
		killed     += s.forced_killed;
		killed_mask |= s.forced_killed_prompts;
		dead_ends  += s.dead_ends;
		terminals  += s.terminals;
		novel      += s.novelty_novel;
		stale      += s.novelty_stale;
		atoms       = (std::max)(atoms, s.novelty_atoms);
		num_broken += s.levin_overflow + s.lam_saturated;
		recipes_seen   += s.recipes_seen;
		recipe_h_sum   += s.recipe_h_sum;
		recipe_h_count += s.recipe_h_count;
		landmark_h_sum   += s.landmark_h_sum;
		landmark_h_count += s.landmark_h_count;
	}
};

// A worker that fails to initialise must NOT return silently. Its pass would
// display "0 solutions, 0 states", exactly what a worker that ran fine and
// found nothing displays; under address space pressure (16 workers x
// --arena-mb) a whole run could then never have run with nothing saying so.
void WorkerAbort(const char* ou, const std::string& err) {
	static std::mutex abort_mx;
	std::lock_guard<std::mutex> lk(abort_mx);
	std::printf("  !! worker %s : %s\n", ou,
				err.empty() ? "failure with no message" : err.c_str());
	std::fflush(stdout);
}

// Arena overflowed in a worker: its measurements are INVALID from the escape
// on, not merely incomplete. `Restore()` does not restore the objects that went
// to the host heap, so the duel diverges from what the search believes it
// restored. The counter that should have said so was a `thread_local` read from
// the MAIN thread, hence structurally zero whatever happened: the report
// printed "none: all state is captured" by construction.
void ReportPoison(const char* ou, const Arena& a) {
	if(!a.Poisoned())
		return;
	static std::mutex poison_mx;
	std::lock_guard<std::mutex> lk(poison_mx);
	std::printf("  !! ARENA POISONED - worker %s: %zu allocation(s) "
					"outside the arena.\n     Restore() does not restore "
					"them: everything this worker measured after that is "
					"WRONG.\n     Raise --arena-mb, or lower --threads.\n", ou, a.Fallbacks());
	std::fflush(stdout);
}

// Distribution under which the POLICY REPORT probes the corpus. It is NEUTRAL,
// and deliberately different from the run's (cfg.hint_bias = 2, cfg.nrpa_temp
// tunable through --nrpa-temp): the report measures the corpus's own
// discriminating power, and its three instruments (AdaptCorpus,
// CorpusAgreement, ForecastSearchCost) must at least agree WITH EACH OTHER,
// which only the last two enforced. The values are written here, once, instead
// of being omitted at the call site: it was that omission that made the
// inconsistency invisible.
//
// Worth keeping in mind: the corpus figures are read under THIS distribution,
// not under the rollouts'. Aligning the three instruments on the run is a
// separate piece of work.
constexpr float kReportHintBias = 0.0f;
constexpr float kReportTemp = 1.0f;

// SILENT SATURATIONS. Three compact encodings clamp their fields without
// warning, and two of them govern quantities that were used to decide: the
// archive score (rp on 4 bits, overlap on 8) orders the states kept, and
// ContextKey (15 max) bounds the segmentation ForecastSearchCost read to write
// sqrt-LTS. Checked once, at startup, as the cap of 4 --resolve entries already
// is.
void CheckSaturations(size_t target_size,
					  const std::vector<ResolveReq>& resolve_min) {
	uint32_t total = 0;
	for(const ResolveReq& r : resolve_min)
		total += r.min_count;
	if(total > 15)
		std::printf("!! %u resolutions required: the archive score "
							"encodes only 15\n   (states beyond that rank as "
							"ties - lower --resolve)\n", total);
	if(target_size > 255)
		std::printf("!! target board of %zu cards: the archive score "
							"encodes only 255\n", target_size);
	if(target_size > 15)
		std::printf("!! target board of %zu cards: ContextKey "
							"distinguishes only 15\n"
							"   (the policy context saturates)\n", target_size);
}

// Depth left for the finisher after replaying a prefix.
//
// When the prefix already reaches the ceiling (itself derived from the
// reference), the rest is zero. A hard-coded fallback of 64 decisions, written
// in five places and silent, would let a `--finisher ab` comparison "at equal
// budget" give the two engines different DEPTH budgets with not a word in the
// log. The fallback is now
// counted, and the summary says so, as the --max-decisions one already does.
constexpr uint32_t kFinisherFallbackDepth = 64;
std::atomic<uint64_t> g_depth_fallbacks{ 0 };

uint32_t FinisherDepth(uint32_t ceiling, size_t prefix) {
	if(ceiling > prefix)
		return static_cast<uint32_t>(ceiling - prefix);
	g_depth_fallbacks.fetch_add(1, std::memory_order_relaxed);
	return kFinisherFallbackDepth;
}

// --- COUNTING DERIVED FROM THE TARGET BOARD ---------------------------------
//
// The target board alone imposes an ARITHMETIC, with no declarative model:
// "3x Liger Dancer" means THREE Fusion summons. It is an argument about the
// target multiset and the decklist, in the spirit of operator counting, and it
// gives two things otherwise written by hand: a finer feasibility bound,
// and a DERIVED --summon-min.
//
// THE TRAP, and it is explicit: we count summon EVENTS, NEVER their triggers.
// "Three Ligers therefore three Polymerizations" would be wrong, since
// Lunalight Wolf fuses from the Pendulum Zone with no Polymerization. The
// target card's type says which EVENT must happen; it says nothing about what
// triggers it.
//
// AND THE RULE: this never PRUNES. A shortage of copies is reported as a DOUBT,
// not as an impossibility, because a card copying a name (Kaleido Chick taking
// Leo Dancer's name) satisfies a goal based on the effective code without being
// a physical copy. Acting as an oracle here would delete solutions silently.

constexpr uint32_t kTypeMonster  = 0x1;
constexpr uint32_t kTypeFusion   = 0x40;
constexpr uint32_t kTypeRitual   = 0x80;
constexpr uint32_t kTypeSynchro  = 0x2000;
constexpr uint32_t kTypeToken    = 0x4000;
constexpr uint32_t kTypeXyz      = 0x800000;
constexpr uint32_t kTypePendulum = 0x1000000;
constexpr uint32_t kTypeLink     = 0x4000000;

// Summoning mechanism imposed by the target card's TYPE.
enum class Mech { Fusion, Synchro, Xyz, Link, Ritual, MainMonster, SpellTrap };

const char* MechName(Mech m) {
	switch(m) {
	case Mech::Fusion:      return "Fusion";
	case Mech::Synchro:     return "Synchro";
	case Mech::Xyz:         return "Xyz";
	case Mech::Link:        return "Lien";
	case Mech::Ritual:      return "Rituelle";
	case Mech::MainMonster: return "play from hand (main deck)";
	default:                return "place/activate";
	}
}

Mech MechOf(uint32_t type) {
	// Order matters: a Pendulum can also be Synchro/Xyz/Link, and it is the EXTRA
	// DECK mechanism that imposes the event.
	if(type & kTypeFusion)  return Mech::Fusion;
	if(type & kTypeSynchro) return Mech::Synchro;
	if(type & kTypeXyz)     return Mech::Xyz;
	if(type & kTypeLink)    return Mech::Link;
	if(type & kTypeRitual)  return Mech::Ritual;
	if(type & kTypeMonster) return Mech::MainMonster;
	return Mech::SpellTrap;
}

bool FromExtraDeck(Mech m) {
	return m == Mech::Fusion || m == Mech::Synchro || m == Mech::Xyz ||
		   m == Mech::Link;
}

struct TargetCount {
	uint32_t code = 0;
	uint32_t need = 0;      // copies required by the target board
	uint32_t have = 0;      // copies in the decklist (hand + extra)
	Mech mech = Mech::SpellTrap;
	bool token = false;
};

// Returns the per-target-card counts, and fills `events`: mechanism -> number
// of summon EVENTS required.
std::vector<TargetCount> CountTarget(const BoardKey& target, const Deck& deck,
									 const CardDB& db,
									 std::map<Mech, uint32_t>& events) {
	std::map<uint32_t, uint32_t> need;
	for(uint32_t c : target.codes)
		++need[db.Canonical(c)];

	std::map<uint32_t, uint32_t> have;
	for(const auto* list : { &deck.main, &deck.extra })
		for(uint32_t c : *list)
			++have[db.Canonical(c)];

	std::vector<TargetCount> out;
	for(const auto& [code, n] : need) {
		TargetCount t;
		t.code = code;
		t.need = n;
		auto it = have.find(code);
		t.have = it == have.end() ? 0u : it->second;
		const CardRow* row = db.Find(code);
		t.mech = row ? MechOf(row->type) : Mech::SpellTrap;
		t.token = row && (row->type & kTypeToken);
		// A Token is not in the decklist and is not summoned: it is PRODUCED by
		// an effect. Counting it as a missing summon would be a guaranteed false
		// positive.
		if(!t.token && FromExtraDeck(t.mech))
			events[t.mech] += n;
		out.push_back(t);
	}
	std::sort(out.begin(), out.end(),
			  [](const TargetCount& a, const TargetCount& b) {
				  if(a.need != b.need) return a.need > b.need;
				  return a.code < b.code;
			  });
	return out;
}

// Report, and feasibility verdict: a DOUBT, never a stop.
// Returns the number of cards whose copies the decklist cannot supply.
size_t ReportTargetCounting(const std::vector<TargetCount>& counts,
							const std::map<Mech, uint32_t>& events,
							const CardDB& db) {
	std::printf("\n--- counting derived from the target board ---\n");
	std::printf("  %-9s %-6s %-6s %-24s %s\n", "required", "deck", "code",
				"mecanisme", "card");
	size_t short_of = 0;
	for(const TargetCount& t : counts) {
		const bool manque = !t.token && t.have < t.need;
		if(manque)
			++short_of;
		std::printf("  %-9u %-6u %-6u %-24s %s%s\n", t.need,
					t.token ? 0u : t.have, t.code, MechName(t.mech),
					db.Name(t.code).c_str(),
					t.token ? "   (Token: produced by an "
												"effect)"
							: (manque ? "   <-- the decklist "
																	"does not hold "
																	"enough"
									  : ""));
	}
	if(!events.empty()) {
		std::printf("\n  Summon EVENTS required by the board alone:");
		for(const auto& [m, n] : events)
			std::printf("  %u %s", n, MechName(m));
		std::printf("\n  (EVENTS are counted, never their triggers: "
							"\"three Fusions\" does not\n"
							"   mean \"three Polymerizations\" - a Fusion can "
							"start from elsewhere.)\n");
	}
	if(short_of) {
		std::printf("\n  !! FEASIBILITY DOUBTFUL: %zu target card(s) "
							"require more copies than the\n"
							"     decklist holds. This is NOT a verdict of "
							"impossibility - a card that COPIES\n"
							"     a name satisfies the goal without being a "
							"physical copy, and the goal is\n"
							"     judged on the EFFECTIVE code. The search "
							"continues (rule: weigh, never prune).\n", short_of);
	}
	return short_of;
}

// SEEDING THE RECIPE GRAPH FROM CARD TEXT (rule 3).
//
// "The text only seeds it; the truth comes from observation." The
// observational half alone has an exact limit: it only learns from SUCCESSFUL
// summons, and the card we are after is precisely the one no line has ever
// placed. Without seeding, `Distance` returns its floor for it and `h` stays
// FLAT where it should be informative.
//
// The text fills that hole. Its first line, for an extra deck monster, is the
// materials line, and its format is regular:
//     "Lunalight Leo Dancer" + 3 "Lunalight" monsters
//     2 Level 4 monsters
//
// WHAT WE TAKE FROM IT, after a measured widening.
//
// An earlier version kept only the materials NAMED IN QUOTES, judging archetype
// and level requirements "almost always easy to satisfy, hence noise with no
// gradient". The census of the ten extra deck cards of benchmark A says the
// opposite:
//
//   Liger Dancer    "Lunalight Leo Dancer" + 3 "Lunalight" monsters
//   Leo Dancer      "Lunalight Panther Dancer" + 2 "Lunalight" monsters
//   Sabre Dancer    3 "Lunalight" monsters
//   Perfume Dancer  2 "Lunalight" monsters
//   Bagooska        2 Level 4 monsters          <- a TARGET card
//   Dugares         2 Level 4 monsters
//   ... (Cross-Sheep, A Bao A Qu, Tiger King, Underworld Goddess)
//
// EIGHT cards out of ten name NO card at all: without the cardinal
// requirements, the seeding posts a single recipe and the mechanism is live
// with no effect. And the "no gradient" argument is wrong the other way round:
// a CARDINAL requirement is precisely what decreases continuously, since "3
// Lunalight monsters" loses one unit for every Lunalight placed, i.e. BEFORE
// any target card touches the field. That is the hole in the flat `h`.
//
// Still ignored, deliberately: type/attribute/race requirements
// ("Beast-Warrior", "Effect Monsters", "including a Fiend monster") and
// distinctness constraints ("2 monsters with different names"). Omitting them
// UNDERSTATES the cost, the safe direction under rule 2.
//
// The zone is the JOKER: the text names a material without saying where it
// comes from. That joker EXCLUDES the deck and the extra deck (see kZoneAny): a
// card sleeping there is not an available material.

// An archetype is named in the text ("Lunalight"), but the core only knows
// numeric SETCODES, and no name -> setcode table is available outside
// `strings.conf`. So we resolve it through the DECK itself: the cards whose
// name contains the fragment must all carry a common setcode. That is true by
// construction of an archetype, and it is checkable; the setcode kept and its
// number of suppliers are printed.
//
// Returns 0 when the intersection is empty or when the fragment designates
// fewer than two cards: when in doubt, we do not seed (rule 2).
uint16_t SetcodeOfFragment(const CardDB& db, const std::vector<uint32_t>& pool,
						   const std::string& fragment, size_t* providers) {
	std::vector<uint16_t> common;
	size_t matched = 0;
	for(uint32_t code : pool) {
		const std::string name = db.Name(code);
		if(name.find(fragment) == std::string::npos)
			continue;
		const CardRow* row = db.Find(code);
		if(!row)
			continue;
		std::vector<uint16_t> mine;
		for(uint16_t sc : row->setcodes)
			if(sc)
				mine.push_back(sc);
		if(mine.empty())
			return 0;   // a card with that name and no setcode: unreliable fragment
		if(!matched++) {
			common = mine;
		} else {
			std::vector<uint16_t> keep;
			for(uint16_t sc : common)
				if(std::find(mine.begin(), mine.end(), sc) != mine.end())
					keep.push_back(sc);
			common.swap(keep);
		}
		if(common.empty())
			return 0;
	}
	if(matched < 2 || common.empty())
		return 0;
	if(providers)
		*providers = matched;
	return common.front();
}

// WHAT THE SEEDING IS WORTH, PRINTED AND CHECKABLE BY HAND.
//
// An earlier note published a table of seeded distances (Liger 2, Leo 1,
// Bagooska 1) that NO output of the solver produced: it came from a trace taken
// outside the tool, and a trace that does not replay the code is worth little.
// This table comes from the graph itself, through the same `DistanceAll` the
// finisher calls.
//
// The reference state is the EMPTY FIELD: nothing placed, nothing in the
// graveyard. It is the line's starting point, and the only configuration
// defined without replaying a duel. The flat `h` is 1 per missing target card
// there, by construction, so the right-hand column immediately says whether the
// seeding adds anything, and how much.
void ReportSeededDistances(const BoardKey& target, const RecipeGraph& graph,
						   const CardDB& db,
						   const std::vector<uint32_t>& watched = {}) {
	if(target.codes.empty())
		return;
	// Empty presence: no entity anywhere. So every requirement is to be
	// satisfied, and the distance displayed is the one at the start.
	struct NoAvail {
		uint32_t Count(const Requirement&) const { return 0u; }
		bool Claim(const Requirement&) { return false; }
		uint32_t CountAndClaim(const Requirement&) { return 0u; }
		void ResetClaims() {}
	} none;
	std::vector<uint32_t> seen;
	std::printf("     %-44s %-8s %s\n", "card", "h plat",
				"distance amorcee");
	// The WATCHED cards are printed with the targets, and for the same reason:
	// the repetition probe measures distances to THOSE cards, and a line at the
	// floor warns that the probe will have nothing to say, before the run rather
	// than after.
	auto row = [&](uint32_t code, const char* tag) {
		const uint32_t c = db.Canonical(code);
		if(std::find(seen.begin(), seen.end(), c) != seen.end())
			return;
		seen.push_back(c);
		const std::vector<uint32_t> one{ c };
		const uint32_t d = graph.DistanceAll(one, 0x0c /* terrain */, none);
		std::printf("     %-44s %-8u %u%s%s\n", db.Name(c).c_str(), 1u, d,
					d > 1 ? "   <-- gradient" : "   (floor: nothing to say)",
					tag);
	};
	for(uint32_t code : target.codes)
		row(code, "");
	for(uint32_t code : watched)
		row(code, "   [surveillee]");
}

// REPETITION PROBE: printed once per PHASE.
//
// It is printed separately for the rollouts and for the finisher, and that is
// not cosmetic: a judge covering only the rollout phase misreads a conversion
// that happens in the ROOTED rollouts. A "never" in the first table therefore
// only holds for that table.
// `card_id` / `yn_id`: is `Choice::card` filled on SELECTION prompts and on
// YES/NO prompts? They are now ALWAYS true, since the identity is
// unconditional, but the guards remain, and that is not superstition: WITHOUT
// them the choice counters are structurally ZERO and print "NEVER TAKEN", which
// reads as a fact. The defect nearly produced a false conclusion; the guard
// stays so that the day someone makes the identity conditional again, the probe
// SAYS so instead of lying.
void PrintRepeatProbe(const RepeatProbe rep[4], uint64_t rollouts,
					  const CardDB& db, bool card_id, bool yn_id,
					  const char* phase) {
	std::printf("\n  --- repetition probe (--probe-repeat), %s: %llu "
					"rollout(s) ---\n", phase, (unsigned long long)rollouts);
	bool any = false;
	for(int i = 0; i < 4; ++i) {
		const RepeatProbe& r = rep[i];
		if(!r.code)
			continue;
		any = true;
		std::printf("  %s : >=1 %llu  >=2 %llu  >=3 %llu  >=4 %llu  "
							">=5 %llu%s\n",
					db.Name(r.code).c_str(),
					(unsigned long long)r.reached[0],
					(unsigned long long)r.reached[1],
					(unsigned long long)r.reached[2],
					(unsigned long long)r.reached[3],
					(unsigned long long)r.reached[4],
					r.reached[0] == 0
						? "  <-- NEVER: no summon in "
												"this phase"
						: "");
		// OFFER PROBE: THE DECOMPOSITION OF THE ARITY LAW.
		// Printed BEFORE everything else because it decides which kind of work the
		// failure belongs to: "the solver never goes there" means nothing without
		// knowing whether the game offered it.
		{
			const double per = rollouts ? double(r.offer_rollouts) * 100.0 /
											  double(rollouts)
										: 0.0;
			std::printf("      OFFER: proposed in %llu rollout(s) "
									"(%.2f %%), %llu decision(s)\n",
						(unsigned long long)r.offer_rollouts, per,
						(unsigned long long)r.offer_steps);
			static const char* kOfferNames[7] = { "IDLECMD", "SELECT_CARD",
												  "UNSELECT", "SUM", "CHAIN",
												  "POSITION", "OUI/NON" };
			// ACTIVATIONS: the only figure that says whether the solver tried the
			// DOOR, for a card whose role is to open a route rather than to be
			// placed (Wolf, Masquerade, and Leo Dancer, which is never summonable
			// by the normal route since its named material is absent from the
			// deck).
			std::printf("      ACTIVATED in %llu rollout(s) (%.2f "
									"%%), %llu time(s) in total%s\n",
						(unsigned long long)r.act_rollouts,
						rollouts ? double(r.act_rollouts) * 100.0 / double(rollouts)
								 : 0.0,
						(unsigned long long)r.act_total,
						r.act_rollouts ? "" : "   <-- NEVER ACTIVATED");
			// ZONE PRESENCE: the only part that talks about STATES. For a card whose
			// role is to ARRIVE somewhere (Leo Dancer in the graveyard, whence it
			// will be banished as a material), this is THE measurement: "never
			// summoned" did not say whether it had reached its zone.
			{
				bool any_zone = false;
				for(int z = 0; z < 6; ++z)
					if(r.zone_rollouts[z]) { any_zone = true; break; }
				std::printf("      ATTEINT :");
				if(!any_zone) {
					std::printf("  no zone       <-- the "
													"card NEVER MOVED\n");
				} else {
					for(int z = 0; z < 6; ++z)
						if(r.zone_rollouts[z])
							std::printf("  %s %llu (%.1f %%)", ZoneSlotName(z),
										(unsigned long long)r.zone_rollouts[z],
										rollouts ? double(r.zone_rollouts[z]) *
													   100.0 / double(rollouts)
												 : 0.0);
					std::printf("\n");
				}
			}
			// OFFER -> CHOICE CONVERSION: the usable judge. It counts
			// OPPORTUNITIES (thousands) where "the card reached its zone" counts
			// EVENTS (hundreds), and that is what makes it readable despite the
			// inter-run noise.
			if(r.offer_steps && !card_id)
				std::printf("      CHOSEN when offered: "
											"UNAVAILABLE - on SELECTION "
											"prompts the choice carries no\n"
											"                           card "
											"identity, so the count is "
											"structurally ZERO and says "
											"NOTHING.\n");
			else if(r.offer_steps)
				std::printf("      CHOISIE quand offerte : "
											"%llu / %llu   conversion %.2f "
											"%%%s\n",
							(unsigned long long)r.taken_steps,
							(unsigned long long)r.offer_steps,
							100.0 * double(r.taken_steps) / double(r.offer_steps),
							r.taken_steps ? "" : "   <-- NEVER TAKEN");
			std::printf("        by prompt:");
			for(int k = 0; k < 7; ++k)
				if(r.offer_by[k])
					std::printf("  %s %llu", kOfferNames[k],
								(unsigned long long)r.offer_by[k]);
			std::printf("\n");
			// THE YES/NO PART. An OPTIONAL discard that UNLOCKS a route appears in
			// NO other counter: the prompt is offered, the solver answers, and
			// refusing costs nothing visible, neither on the board nor in the
			// score, where a discard is worth +1 of `fodder` against +100 for a
			// target card placed. On benchmark A it is nevertheless the decision
			// that opens access to the GRAVEYARD for every following Fusion.
			// Requires the yes/no identity, without which the prompt is anonymous
			// and the probe can attribute it to nobody.
			if(!yn_id && r.offer_by[6])
				std::printf("        YES/NO: UNAVAILABLE - "
											"the prompt carries no card "
											"identity\n");
			if(r.yn_steps)
				std::printf("        YES/NO: %llu offer(s), "
											"YES taken %llu time(s) (%.1f "
											"%%)%s\n",
							(unsigned long long)r.yn_steps,
							(unsigned long long)r.yn_yes,
							100.0 * double(r.yn_yes) / double(r.yn_steps),
							r.yn_yes ? "" : "   <-- NEVER YES");
			// THE VERDICT IS NOT READ OFF THE TOTAL, and that is the probe's most
			// important correction. `SELECT_CARD` is AMBIGUOUS: it carries "choose
			// your Fusion among the payable ones" as well as "look at your extra
			// deck". The unambiguous prompts are `IDLECMD` (summon from the hand or
			// the extra) and `POSITION` (the card is PLACED, direct proof). When
			// those are at zero, the card was never summonable, whatever the total.
			const uint64_t real = r.offer_by[0] + r.offer_by[5];
			if(!r.offer_rollouts) {
				std::printf("        <-- NEVER OFFERED, on "
											"any prompt.\n");
			} else if(!real && !r.reached[0]) {
				std::printf("        <-- NEVER SUMMONABLE. "
											"All %llu offer(s) are on "
											"SELECTION prompts, none on\n"
											"            IDLECMD or POSITION: "
											"the pool CONTAINS it without it "
											"being payable\n"
											"            (a pool that lists "
											"the whole extra deck). The "
											"failure is in the STATE,\n"
											"            not in the sampling.\n",
							(unsigned long long)r.offer_steps);
			} else if(!r.reached[0]) {
				std::printf("        <-- SUMMONABLE AND NEVER "
											"TAKEN (%llu offer(s) on "
											"IDLECMD/POSITION). The failure "
											"is\n            in the SAMPLING: "
											"subset truncation, or policy "
											"weights.\n",
							(unsigned long long)real);
			} else {
				const double take = 100.0 * double(r.reached[0]) /
									double(r.offer_rollouts);
				std::printf("        offer -> summon "
											"conversion: %.1f %% of the "
											"rollouts that saw it\n", take);
			}
		}
		if(!r.more_n) {
			std::printf("      (no first summon: nothing to "
									"probe)\n");
			continue;
		}
		char refd0[32], refrest[32];
		if(r.d0 == 0xffffffffu)
			std::snprintf(refd0, sizeof refd0, "non mesuree");
		else
			std::snprintf(refd0, sizeof refd0, "%u", r.d0);
		if(r.rest0 == 0xffffffffu)
			std::snprintf(refrest, sizeof refrest, "non mesuree");
		else
			std::snprintf(refrest, sizeof refrest, "%u", r.rest0);
		std::printf("      recipe distance to ONE MORE copy, taken AT "
							"the 1st summon:\n        mean %.2f, min %u, max "
							"%u   (reference from the starting state: %s, "
							"%llu sample(s))\n",
					r.more_sum / double(r.more_n), r.more_min, r.more_max, refd0,
					(unsigned long long)r.d0_samples);
		// FLOOR SAFEGUARD (the most expensive lesson here: a diagnosis that
		// answers beside its own question). The recipe graph returns 1 for any
		// product whose recipe it does not know, which is rule 2 and is intended.
		// But then "distance 1 <= reference 1" is not a preserved material: it is
		// a MUTE graph. Returning a verdict on that would manufacture a
		// conclusion out of an absence of measurement.
		// absence de mesure.
		if(!r.known) {
			std::printf("        <-- FLOOR: the graph knows NO "
									"recipe for this card (rule 2).\n"
									"            The distance can say NOTHING "
									"here - no verdict is returned.\n");
		} else if(r.more_max <= 1) {
			// MEASURED RESERVATION, and it forbids the verdict just as much as a
			// floor does. Recipes SEEDED FROM TEXT carry the JOKER zone
			// (kZoneAny), which accepts the graveyard. But the materials the summon
			// has just consumed have arrived precisely there, so they still count
			// as available and "one more copy" always looks one summon away. A
			// distance uniformly equal to 1 is the signature of that bias, not
			// proof of a preserved material.
			std::printf("        <-- distance uniformly 1: the "
									"recipe read is SEEDED (wildcard zone),\n"
									"            and the material just "
									"consumed still counts from the "
									"graveyard.\n            Known bias - no "
									"verdict is returned on this axis.\n");
		} else if(r.more_kept + r.more_lost) {
			const double kept = 100.0 * double(r.more_kept) /
								double(r.more_kept + r.more_lost);
			std::printf("        materiau CONSERVE (<= reference) "
									": %llu (%.1f %%)   CONSOMME (> "
									"reference) : %llu\n",
						(unsigned long long)r.more_kept, kept,
						(unsigned long long)r.more_lost);
			std::printf("        VERDICT : %s\n",
						kept >= 50.0
							? "the 2nd copy is "
														"NEVER ATTEMPTED - "
														"the material is "
														"there, the fix is "
														"in the SAMPLING"
							: "the 2nd copy is "
														"ALWAYS LOST - the "
														"1st summon consumes "
														"the chain, the fix "
														"is in `h`");
		}
		// THE CONSUMPTION AXIS, which also applies to a goal WITHOUT repetition
		// (benchmark B): the distance to the rest of the board when this piece
		// lands, against the same distance at the start. It MUST have dropped,
		// since a piece placed brings the board closer. If it does not, placing
		// that piece cost elsewhere what it gained here: exactly the wall.
		std::printf("      distance to the REST of the target at the "
							"same instant: %.2f  (at the start: %s)\n"
							"      mean decision of the 1st summon: %.1f  |  "
							"decisions left after: %.1f\n",
					r.rest_sum / double(r.more_n), refrest,
					double(r.first_depth_sum) / double(r.more_n),
					double(r.after_sum) / double(r.more_n));
	}
	if(!any)
		std::printf("  (no card watched: --probe-repeat needs "
							"--summon-min or --resolve)\n");
}

size_t SeedRecipesFromText(const CardDB& db, const Deck& deck,
						   const BoardKey& target, RecipeGraph& graph,
						   bool cardinal,
						   const std::vector<uint32_t>& watched = {}) {
	// Candidates: the extra deck (the only cards with a materials line), the
	// target board's cards, which may not be in the decklist, and the WATCHED
	// cards (--resolve / --summon-min).
	//
	// The watched cards were added for a measured reason: the repetition probe
	// asks for the distance to ONE MORE copy of the watched card, and a card
	// outside the target board had NO seeded recipe, so the distance fell back
	// to the floor of 1 and the probe returned a verdict on a mute graph. The
	// mechanism was live and had no effect.
	std::vector<uint32_t> candidates;
	for(uint32_t c : deck.extra)
		candidates.push_back(db.Canonical(c));
	for(uint32_t c : target.codes)
		candidates.push_back(db.Canonical(c));
	for(uint32_t c : watched)
		candidates.push_back(db.Canonical(c));
	std::sort(candidates.begin(), candidates.end());
	candidates.erase(std::unique(candidates.begin(), candidates.end()),
					 candidates.end());

	// AVAILABLE IN THIS DECK: hand + extra. A named material that is not there
	// cannot be placed by this deck, and the recipe requiring it is a DEAD
	// ROUTE; counting it would give a cost based on an impossible path.
	//
	// The case is real and was found by reading a trace: "Lunalight Leo Dancer"
	// has only one materials line, `"Lunalight Panther Dancer" + 2 "Lunalight"
	// monsters`, and Panther Dancer is NOT in benchmark A's deck. Yet Leo is
	// perfectly summonable there, through a Fusion substitute, a name copy, or
	// an effect that ignores the materials. The text describes ONE route, not
	// THE route.
	//
	// So we fall back to the floor for that product: "we do not know how it
	// arrives" is truer than "it costs the price of an impossible route". That
	// is rule 2 applied strictly: we invent no cost, and we declare nothing
	// unreachable either.
	std::vector<uint32_t> available;
	for(const auto* list : { &deck.main, &deck.extra })
		for(uint32_t c : *list)
			available.push_back(db.Canonical(c));
	std::sort(available.begin(), available.end());
	available.erase(std::unique(available.begin(), available.end()),
					available.end());
	auto in_deck = [&available](uint32_t c) {
		return std::binary_search(available.begin(), available.end(), c);
	};

	size_t seeded = 0, dead_routes = 0, named = 0, arch = 0, lvl = 0;
	std::vector<std::string> announced;   // archetype fragments already printed
	for(uint32_t code : candidates) {
		// ONLY EXTRA DECK cards have a materials line. For any other, the first
		// line of the text is PROSE, and prose readily contains "Special Summon 1
		// Level 4 monster", which the analysis below would take for a
		// requirement. We would then seed a recipe from a sentence, which is no
		// longer seeding but invention (rule 2).
		const CardRow* prow = db.Find(code);
		if(!prow || !(prow->type & (kTypeFusion | kTypeSynchro | kTypeXyz |
									kTypeLink)))
			continue;
		const std::string& line = db.MaterialLine(code);
		if(line.empty())
			continue;
		std::vector<Requirement> mats;
		bool dead = false;
		// Token-by-token analysis. A leading number qualifies what FOLLOWS:
		//     3 "Lunalight" monsters   ->  archetype Lunalight, count 3
		//     2 Level 4 monsters       ->  level 4, count 2
		//     "Lunalight Leo Dancer"   ->  named card, count 1
		// A number followed by anything else ("2+ monsters", "4+ Effect Monsters")
		// is DROPPED: no requirement is derived from it, which understates.
		uint32_t pending = 0;   // 0 = no pending number
		for(size_t i = 0; i < line.size() && !dead;) {
			if(std::isdigit(static_cast<unsigned char>(line[i]))) {
				uint32_t n = 0;
				while(i < line.size() &&
					  std::isdigit(static_cast<unsigned char>(line[i])))
					n = n * 10 + static_cast<uint32_t>(line[i++] - '0');
				pending = n;
				continue;
			}
			if(line[i] == '"') {
				const size_t b = line.find('"', i + 1);
				if(b == std::string::npos)
					break;
				const std::string name = line.substr(i + 1, b - i - 1);
				const uint32_t want = pending ? pending : 1u;
				i = b + 1;
				pending = 0;
				const uint32_t mc = db.CodeByExactName(name);
				if(mc && mc != code) {
					if(!in_deck(mc)) {
						dead = true;   // dead route
						break;
					}
					// `2 "Name"` becomes TWO requirements of one copy, not one
					// requirement of two: a named card resolves through a presence
					// search (0 or 1) and through recursion over ITS recipe, two
					// things a counter cannot do. The `count` field therefore stays
					// 1 for kReqCard, which is what lets Distance's memo ignore it.
					for(uint32_t k = 0; k < want; ++k)
						mats.push_back(Requirement{ mc, kZoneAny, kReqCard, 1 });
					++named;
					continue;
				}
				if(mc)          // the product names itself: nothing to require
					continue;
				// No card of that name: it is an ARCHETYPE fragment.
				if(!cardinal)
					continue;
				size_t providers = 0;
				const uint16_t sc =
					SetcodeOfFragment(db, available, name, &providers);
				if(!sc)
					continue;   // unresolved fragment: we invent nothing
				// The setcode is DEDUCED from the deck, not read from a table: it
				// is printed with its number of suppliers so it can be checked.
				if(std::find(announced.begin(), announced.end(), name) ==
				   announced.end()) {
					announced.push_back(name);
					std::printf("     archetype \"%s\" -> "
													"setcode 0x%x, %zu "
													"provider(s) in the deck\n", name.c_str(), sc,
								providers);
				}
				mats.push_back(Requirement{ sc, kZoneAny, kReqSetcode,
											static_cast<uint8_t>(want) });
				++arch;
				continue;
			}
			// "Level N": the required level follows the word.
			if(cardinal && pending &&
			   line.compare(i, 6, "Level ") == 0) {
				size_t j = i + 6;
				uint32_t m = 0;
				while(j < line.size() &&
					  std::isdigit(static_cast<unsigned char>(line[j])))
					m = m * 10 + static_cast<uint32_t>(line[j++] - '0');
				if(m) {
					mats.push_back(Requirement{ m, kZoneAny, kReqLevel,
												static_cast<uint8_t>(pending) });
					++lvl;
					pending = 0;
					i = j;
					continue;
				}
			}
			// Any ordinary word consumes the pending number: "2+ monsters,
			// including a Fiend monster" must not have its "2" re-attached to
			// a fragment further along the line.
			if(std::isalpha(static_cast<unsigned char>(line[i]))) {
				while(i < line.size() &&
					  (std::isalpha(static_cast<unsigned char>(line[i])) ||
					   line[i] == '-'))
					++i;
				pending = 0;
				continue;
			}
			++i;
		}
		if(dead) {
			++dead_routes;
			continue;
		}
		if(mats.empty())
			continue;
		// `primed`: this recipe comes from the TEXT. It will be discarded as soon as
		// a real summon of the same product has been observed (rule 3).
		graph.Observe(code, mats, /*primed=*/true);
		++seeded;
	}
	if(dead_routes)
		std::printf("     (%zu recipe(s) set aside: they name a "
							"material absent from this deck)\n", dead_routes);
	if(seeded)
		std::printf("     requirements posted: %zu named, %zu "
							"archetype(s), %zu level(s)%s\n", named, arch, lvl,
					cardinal ? "" : "   (--no-seed-quant : cardinales ETEINTES)");
	return seeded;
}

void PrintCuts(const CutCounts& c) {
	std::printf("           pruning: constraint %llu, guard %llu, turn "
					"%llu, bound %llu, partition %llu, subsets %llu\n",
				(unsigned long long)c.constraint, (unsigned long long)c.guard,
				(unsigned long long)c.turn, (unsigned long long)c.bound,
				(unsigned long long)c.claim, (unsigned long long)c.subsets);
	// Liveness of --no-self-negate: chain options removed.
	if(c.selfneg)
		std::printf("           discipline: %llu self-negation(s) "
							"removed (--no-self-negate)\n",
					(unsigned long long)c.selfneg);
	// Liveness of --guard-keep: main-phase activations of a guard resource removed.
	if(c.guardkeep)
		std::printf("           discipline: %llu guard-resource "
							"activation(s) removed (--guard-keep)\n",
					(unsigned long long)c.guardkeep);
	// `dead ends` is the number one symptom of a mismatched script set, the
	// failure this project fears most, and it was only printed in --width mode,
	// i.e. mute exactly where it would occur. `novelty` says whether the
	// rollouts' tie-break term still counts for anything or is saturated;
	// `atoms` is the measured width.
	std::printf("           health : dead ends %llu, terminals %llu, "
					"novelty %llu/%llu, atoms %zu\n",
				(unsigned long long)c.dead_ends,
				(unsigned long long)c.terminals, (unsigned long long)c.novel,
				(unsigned long long)(c.novel + c.stale), c.atoms);
	if(c.forced) {
		std::printf("           !! %llu prompt(s) reduced to THE "
							"default answer (types:", (unsigned long long)c.forced);
		for(int b = 0; b < 64; ++b)
			if(c.forced_mask & (1ull << b))
				std::printf(" %d", b);
		std::printf(")\n              The branch SURVIVES, reduced to "
							"one choice: all the rest of that prompt is out "
							"of reach.\n");
	}
	// SEPARATE FROM THE ABOVE: here no default answer exists, so the branch
	// DIES, and it also counts in `dead ends` above. Conflating them made a
	// single fact read as two.
	if(c.killed) {
		std::printf("           !! %llu prompt(s) with NO default "
							"answer at all: BRANCH KILLED (types:",
					(unsigned long long)c.killed);
		for(int b = 0; b < 64; ++b)
			if(c.killed_mask & (1ull << b))
				std::printf(" %d", b);
		std::printf(")\n              These branches never existed, "
							"and they are ALREADY counted in \"dead ends\".\n");
	}
	if(c.num_broken)
		std::printf("           !! %llu sqrt-LTS arithmetic "
							"overflow(s): this arm is to be DISCARDED\n", (unsigned long long)c.num_broken);
	// RECIPE GRAPH: without these two figures the mechanism would be invisible.
	// `summons observed` at zero = the graph is EMPTY, so the distance is exactly
	// the flat `h` and the mechanism is INERT, which has to be known before any
	// conclusion. `mean h` compared with |missing target| says whether the
	// landscape really got deeper.
	//
	// `observed` counts OCCURRENCES, prefix replays included: the same summon seen
	// again at every re-descent counts every time. It is the right figure to say
	// "has the graph seen anything", not to say "how many DISTINCT recipes it
	// knows".
	if(c.recipes_seen || c.recipe_h_count)
		std::printf("           recipes: %llu summon(s) observed, "
							"mean h %.2f over %llu evaluation(s)\n",
					(unsigned long long)c.recipes_seen,
					c.recipe_h_count ? c.recipe_h_sum / double(c.recipe_h_count)
									 : 0.0,
					(unsigned long long)c.recipe_h_count);
	// THE MECHANISM'S LIVENESS. A landmark `h` that is on but never evaluated is
	// indistinguishable from an evaluated `h` that says nothing: the count
	// separates the two, and the mean says whether the landscape deepens. Stuck to
	// the total number of landmarks, the search achieves NOTHING; stuck to zero,
	// the landmarks are too easy and do not guide.
	if(c.landmark_h_count)
		std::printf("           landmarks: mean h %.2f over %llu "
							"evaluation(s)\n",
					c.landmark_h_sum / double(c.landmark_h_count),
					(unsigned long long)c.landmark_h_count);
}

struct Options {
	std::string replay;
	// Replay supplying the STARTING position (deck, hand, seed). Empty: we search
	// inside the reference's own duel.
	std::string start_replay;
	// EDOPro installation. No default: an absolute path baked into the binary
	// only ever works on one machine. Taken from --workdir, else from the
	// COMBOSOLVER_WORKDIR environment variable; absent both, the run refuses
	// to start rather than guess.
	std::string workdir;
	std::vector<std::string> scriptdirs;
	// Directory of the replays produced: the requested deliverable.
	std::string outdir = "solutions";
	bool verbose = false;
	// `--help` asked for the text and got it: that is a SUCCESS. Parse failures
	// print the same text but exit non-zero, so the two paths must be told apart
	// here rather than at the single `return false` they used to share.
	bool help = false;
	// Hot path profile: thread_local rdtsc probes, printed per phase with the
	// "everything else" line. The instrument's cost is quantified by comparing two
	// runs at equal seed, with and without.
	bool profile = false;
	int target_player = 0;
	bool no_arena = false;
	bool stop_gc = true;
	size_t arena_mb = 256;
	bool growth = false;          // measure the graph's growth curve
	uint32_t growth_max = 14;
	double growth_ms = 20000;
	bool solve = false;           // search guided towards the target board
	double solve_ms = 120000;
	unsigned threads = 0;         // 0 = all cores
	// Novelty pruning: -1 = patience auto-calibrated on the width measurement,
	// 0 = disabled, >0 = imposed patience.
	int novelty = -1;
	bool nrpa = true;             // rollouts under a learned policy (NRPA)
	bool width = false;           // width measurement only
	// Seed of the rollouts (0 = derived from the clock and printed: two runs with
	// the same seed explore largely the same trajectories, and the old constant
	// made every relaunch the same run).
	uint64_t seed = 0;
	// GNRPA bias of the repertoire's moves (-1 = the engine default, 1.5).
	double nrpa_bias = -1.0;
	// Partial persistence of the NRPA policy across restarts (weight attenuation;
	// 0 = a virgin policy, the previous behaviour).
	double nrpa_keep = 0.5;
	// GNRPA with limited repetitions (arXiv:2401.10420): number of times the best
	// sequence may be re-found before the level is stopped. 0 = stagnation alone,
	// which is the DEFAULT, on measurement: at R=2 one transplantation test (90 s)
	// falls from 8/8 + 36 lines to 7/8 + 0 lines, since stopping the levels early
	// breaks the convergence that stagnation at 8 let complete. The flag stays so
	// it can be re-measured.
	// REAL SCOPE: the rollout phase only. Neither the rooted finisher nor the
	// --fire windows. A re-measurement would therefore only cover a third of the
	// flow.
	// SLOW AND LONG ADAPTATION (the Montparnasse recipe, arXiv:2505.02110 /
	// 2606.07562, which solved Eterna100): the NRPA adaptation step and the number
	// of iterations per level. Hard-coded (1.0 and 24) they would be on no dial and
	// never measured. The paper's recipe is a SMALL ALPHA
	// compensated by MANY iterations at the low level: the policy moves slowly and
	// explores the same basin for a long time instead of locking into it in a few
	// adaptations. 0 = the engine default (the previous behaviour byte for byte).
	double nrpa_alpha = 0;
	uint32_t nrpa_iters = 0;
	// Transposition table SHARED between workers (lazy SMP), in MB per pass. 0 =
	// private tables (the previous behaviour).
	// Transposition table SHARED between workers. REAL SCOPE: the LDS passes only
	// (repair, transplantation). Neither RunLevin, which keeps its private table,
	// nor RunNrpa, which has none. So the flag has no effect on the two phases
	// that consume the budget.
	size_t tt_mb = 64;
	// Finisher of the transplantation: "levin" (Go-Explore archive + backtrack +
	// Levin Tree Search over the NRPA policy), "mono" (the old one: guided search
	// of the single best state, measured exhausted three times in ~6 states),
	// "ab" (both at equal budget: the measurement).
	std::string finisher = "levin";
	// Size of the Go-Explore archive (distinct states kept with their path, per
	// worker and as the number of finisher roots). 0 = no archive.
	size_t archive_k = 16;
	// Minimum budget RESERVED for the finisher (ms). 0 = the original split (70 %
	// rollouts, finisher 0.8 x the rest capped at 240 s). To be set when
	// conversion is the question and --approach already supplies the roots: the
	// rollouts no longer have to carry the whole budget.
	double finisher_min = 0;
	// PHS* weight of the distance to the goal in the finisher's cost (0 = pure
	// Levin, blind to the goal; measured: it climbs back up deep backtracks
	// without preferring the branches that rip).
	double levin_h = 1.0;
	// Finisher replays: full dive stack (WINNER, +92 % expansions at equal time on
	// benchmark 0, on by default) and LIFO tie-break (wins nothing alone, off).
	// See SearchConfig.
	bool dive_full = true;
	// Weight of a required resolution in the rollouts' gradient (default 250;
	// 100 = the old weight, one target card, and a measured loser: the 8/8 lines
	// with no rip won the adaptation race against the partially ripping ones).
	double resolve_weight = 250.0;
	// Anytime COST OPTIMISATION (--optimize): the search no longer stops at the
	// first solution. Each solution tightens the bound, the NRPA goal score becomes
	// lexicographic (burned, then actions, then decisions), rollouts continue PAST
	// the goal (recoveries reduce the burned count), the archive prefers states
	// with a low partial cost, and the finisher runs even when the rollouts already
	// have lines.
	bool optimize = false;
	// OPPONENT TEST (--fire "card"): the card is ADDED to the opponent's hand and
	// the opponent PLAYS it, one distinct attempt at every window where it is
	// legal, and the rooted search must then close the board back up from the
	// post-injection state. The guard was a static proxy ("a counter is
	// available"); this mode is the dynamic proof ("the counter works AND the combo
	// closes").
	std::string fire_spec;
	// Cards that may be SACRIFICED to answer (--fire-spare, repeatable): the target
	// board WITHOUT those cards is also accepted at the goal (player's ruling:
	// answering Nibiru with Zalen consumes Junk Signal, and the answerer can
	// consume itself).
	std::vector<std::string> fire_spare_specs;
	double fire_ms = 45000;   // search budget per injection window
	// --fire-bake: the drawn card is BAKED into the header of the replays produced
	// (inserted into the opponent's deck where the pseudo-shuffle serves the hand),
	// so they replay FROM THEIR FILE and EDOPro can watch them. In exchange, since
	// start_hand is shared, the card takes the place of the last card of the
	// opponent's original hand (moved to the deck): the duel differs from the
	// default mode by one opponent hand card, and the alignment proof decides
	// whether it stays replayable.
	bool fire_bake = false;
	// --fire-no-chain: a no-chain list specific to the post-injection CONTINUATION
	// (the global --no-chain lists are lifted there, since chaining onto the threat
	// is the guards' job). Used to STAGE a precise answer: forbidding Crystal Wing
	// forces the Zalen+Junk Signal route (measured: without it, the solver
	// satisfies "Zalen resolves" by activating it ELSEWHERE while CW negates
	// Nibiru).
	std::vector<std::string> fire_no_chain_specs;
	// --fire-open: inject only at OPEN windows (empty chain), so the drawn card
	// STARTS a chain (link 1) instead of being chained onto our effects. That is
	// the real threat (player's verdict: a Nibiru chained onto Junk Speeder is
	// easily negated and does not model a real opponent).
	bool fire_open = false;
	// Slack of the burned bound (B&B): burned cards are not monotonic (real
	// recoveries), so the slack is measured on the reference ("max burned
	// mid-line"). >= 255 = bound inactive.
	uint32_t burn_slack = 6;
	// Seed of the bound: best burned count known in advance (0 = none).
	uint32_t burn_limit = 0;
	// Sharing of the burned bound BETWEEN workers: a worker that improves the
	// burned count tightens the B&B cut for everyone, through an atomic (CAS min
	// on publication, relaxed load at the cut).
	// --no-burn-share disables it.
	bool burn_share = true;
	std::vector<std::string> adapt_files;
	uint32_t adapt_passes = 4;
	// OPTIONS: size of the catalogue of macros mined from the --adapt corpus and
	// offered to the NRPA sampling. 0 = off (the previous behaviour byte for byte).
	// The forecast only justifies the BIG catalogue: 256 with support 2 gains 8.5
	// orders, while 16 and 64 are counterproductive.
	uint32_t options_n = 0;
	uint32_t options_support = 2;
	// Dials of the mining, judged and frozen. Macro length, offer window and
	// semantic guard were each refuted on their own; the living corpus size was
	// too. They stay as the values that were measured, not as flags.
	static constexpr uint32_t kOptionsMaxLen = 8;
	static constexpr uint32_t kOptionsWindow = 0;   // no positional guard
	static constexpr int      kOptionsCtxTol = -1;  // semantic guard off
	static constexpr uint32_t kOptionsPool   = 12;
	// ONLINE MINING (Marvin, arXiv:1110.2736): period in SECONDS of the re-mining
	// over the run's own best lines. 0 = off (the catalogue is mined once at
	// startup from --adapt and never moves: the previous behaviour byte for byte).
	// It is the mechanism that makes the bootstrap possible IN A SINGLE RUN,
	// without an external corpus, without an inherited --approach, without a
	// relaunch.
	uint32_t options_online = 0;
	uint32_t options_per_worker = 2;
	// Canonical PHS*: the paper's (d + h)/pi cost instead of our
	// log(d+1) + h - log pi (a factor e^h, with no guarantee).
	// Merged arena pop when returning to the ancestor (see
	// SearchConfig::merged_pop). WINNER on benchmark 0, on by default.
	bool merged_pop = true;
	// Post-goal recovery in the finisher (opt-in): under --optimize, a goal node of
	// RunLevin keeps going instead of stopping.
	bool finisher_post_goal = false;
	// MACRO EDGES in the finisher: the catalogue's macros become edges of the Levin
	// tree. Opt-in: benchmark 0's control LEGITIMATELY changes shape when it is on
	// (see SearchConfig::finisher_options).
	// SearchConfig::finisher_options).
	bool finisher_options = false;
	// TWO-LEVEL POLICY (MCPS 2510.06381): retention of the contextual level,
	// s = n/(n+k). Negative = off.
	double ctx_shrink = -1.0;
	// PATH CONDITIONING: the contextual level's context becomes the sum of the
	// moves played over the first k decisions, instead of the (cards placed, hand)
	// descriptor. 0 = off.
	//
	// It is a return to MCPS's criterion, which this project CITED without applying
	// it: its conditioning is "the games containing every move of the path", ours
	// was a two-axis descriptor that is (0, 3) for EVERY branch at the first
	// decision, hence incapable by construction of telling two openings apart.
	// HEAD BANDIT WITH PERMUTATION STATISTIC (--qhat): MCPS's selection rule,
	// argmax of (n Q + n^ Q^)/(n + n^) with weights proportional to the sample
	// sizes, over the first k decisions of the rollout. It is the paper's mechanism
	// FOR REAL (reward averages over sets of rollouts, the DEAD ones included),
	// where the path conditioning had taken only its conditioning and put it on
	// NRPA logits. 0 = off.
	uint32_t qhat_depth = 0;
	uint32_t qhat_window = 4096;
	uint32_t qhat_rho = 32;
	size_t qhat_nodes = 65536;
	// Bandit PROBE: print the ROOT's table (first decision) at the end of the
	// rollout phase. On automatically under --qhat, since the corpus agreement
	// curve CANNOT judge Q^ (it measures the reproduction of a corpus containing
	// only good lines), so it is the only free instrument that says whether the
	// mechanism separates anything.
	bool qhat_probe = true;
	// NOVELTY PRUNING IN THE POLICY ROLLOUTS: the novelty verdict is already
	// computed at every PolicyRollout decision and thrown away after a mere score
	// tie-break. Opt-in: the mechanism has a documented failure mode and must be
	// judged rather than assumed.
	// Phase exits (Battle/End) removed from the enumeration: the target board is
	// the one at the END OF TURN 1, so changing phase can only shorten the line.
	// The flag existed in EnumOptions with no dial at all, and it was only read at
	// the idle prompt; at the battle prompt the two exits were emitted
	// unconditionally.
	// One representative free zone per zone type. Wired into all three search
	// configurations; link arrows and columns can change everything, so it stays
	// off until it is judged.
	bool canonical_zones = false;
	// QUOTA PER PROGRESS LEVEL in the Go-Explore archive: gives the archive back
	// its nature as a COVERING when its sort key saturates.
	// GOAL BY INCLUSION: the final board must CONTAIN the target instead of being
	// EQUAL to it. See SearchConfig::goal_subset.
	//
	// ON BY DEFAULT as soon as the target is POSTED (--target): a posted target
	// means "I want these cards", not "these cards and an empty field around them".
	// The old default required an EMPTY S/T zone and made benchmark A
	// unsatisfiable (a hand of three CONTINUOUS spells).
	// A CAPTURED target keeps exact equality: it carries its own S/T.
	bool target_subset = false;   // --target-subset: force inclusion
	bool target_exact = false;    // --target-exact : force equality
	// Cap on the entries of the contextual level, per worker (0 = unlimited).
	size_t ctx_max = 262144;
	// Temperature of the NRPA sampling (1.0 = the previous behaviour).
	double nrpa_temp = 1.0;
	// NRPA nesting level. 0 = the historical default, chosen by a 180 s threshold
	// on the rollout budget, a threshold that changes the ALGORITHM (iters^2
	// against iters^3) with no measurement behind it.
	int nrpa_level = 0;
	// Weight of the channel through which the PLAYER'S KNOWLEDGE enters the
	// sampling: the --resolve/--summon-min cards receive it automatically. Its
	// neighbour nrpa_bias_known has had --nrpa-bias for a long time; this one had
	// nothing, and the published figures quantify the `known` bias's contribution
	// without ever isolating this one. Negative = the engine default.
	double hint_bias = -1.0;
	// DETERMINISTIC MODE: budget in ROLLOUTS per worker, instead of wall time.
	// Combined with `--threads 1`, two executions do exactly the same work, and it
	// is the only mode in which a fine comparison means anything. 0 = unlimited.
	uint64_t max_rollouts = 0;
	uint64_t max_nodes = 0;
	// GRADIENT TRUNCATION AT THE SCORE PEAK. See NrpaRun::peak_steps.
	//
	// ON BY DEFAULT. Measured on BOTH benchmarks: x2.6 on arity 3 in two
	// independent pairs of benchmark A, and on benchmark B in PROPORTION over ten
	// runs per arm, where the stack carries `>=2` from 3/10 to 9/10. The flag is
	// NEGATIVE (`--no-adapt-to-peak`) so that it can be turned off.
	bool adapt_to_peak = true;
	// Maximum number of subsets emitted per selection prompt. It is what caps the
	// branching factor of EVERY selection prompt. Its effect is read in the
	// "subsets" column of the pruning line.
	uint32_t max_subsets = 24;
	// Derive --summon-min from the TARGET BOARD instead of writing it by hand.
	// Opt-in: a derived constraint changes the search's behaviour and must not
	// impose itself silently.
	bool derive_summon_min = false;
	// RECIPE GRAPH. Three states:
	//   negative: off, the previous behaviour byte for byte;
	//   0.0     : the graph is FED and MEASURED but does not enter the cost, which
	//             is the mode that quantifies what it would be able to say BEFORE
	//             letting it decide;
	//   > 0     : the recipe distance enters `h` with this weight.
	double recipes = -1.0;
	// Seed the graph with the materials NAMED by the card text (rule 3: the text is
	// a SEED, observation is the truth). Without seeding the graph only learns from
	// SUCCESSFUL summons, and the card being sought is precisely the one that never
	// succeeds.
	bool seed_recipes = true;
	// ALSO seed the CARDINAL requirements ("3 \"Lunalight\" monsters", "2 Level 4
	// monsters"). Separate from seed_recipes so that what they bring can be
	// isolated: without them the seeding posts a single recipe on benchmark A (eight
	// of the ten extra deck cards name no card at all).
	bool seed_cardinal = true;
	// LEARNED LANDMARK GRAPH. The corpus of RESOLVED plans the landmarks are
	// extracted from: files or directories, like --adapt.
	//
	// EXPLICIT AND NEVER IMPLICIT, and that is a measurement requirement: the
	// reference line is a legitimate resolved plan, but if the solver absorbed it
	// on its own, a "with landmarks" arm would mix two factors, the mechanism and
	// the return of the repertoire --no-plan had just set aside. The operator
	// designates the corpus, or there is none.
	std::vector<std::string> landmark_files;
	// Weight of a landmark achievement in the ROLLOUT SCORE, in units of material
	// (one target card placed is worth 100). 0 = the landmarks are learned, printed
	// and MEASURED without weighing.
	double landmark_weight = 0.0;
	// Weight of the landmark `h` in the finisher (same entry point as --recipes).
	double landmark_h = 0.0;
	// REPETITION PROBE. Implies `recipes >= 0`: the probe measures DISTANCES over
	// the recipe graph, so without a graph it would have only a histogram and
	// silence on the one question asked. The implication is applied AND printed; a
	// flag that turns another on without saying so is the family of trap this
	// project catalogues.
	bool probe_repeat = false;
	// DECLARED OPERATOR HARNESS. An INSTRUMENT, not a mechanism: it extracts the
	// operator table (preconditions, product, granted state, consumption) from the
	// deck's Lua scripts, prints it, then CONFRONTS that table with the replayed
	// plan. Does every activation correspond to a declared operator, and is the
	// sequence valid under the extracted preconditions?
	//
	// WHY IT COMES BEFORE EVERYTHING ELSE. Recipes, landmarks and `--backward` are
	// all fed by OBSERVATION (what the solver has already managed) instead of by
	// DECLARATION, so they rest on a graph nobody has checked describes the game.
	// The harness is falsifiable and costs one run; if it fails, the
	// planner is pointless, and that is what one wants to know first.
	bool operators = false;
	// SEEDING THE RECIPE GRAPH FROM THE DECLARED OPERATORS, instead of from card
	// TEXT alone.
	//
	// Two contributions, and the second is the node type that was missing:
	//   - the recipes come from `Fusion.AddProcMix*`, i.e. CODES rather than an
	//     English sentence to re-resolve;
	//   - an edge "this CODE can be ACQUIRED" for every `EFFECT_ADD_CODE` /
	//     `EFFECT_CHANGE_CODE` of the deck. The graph filed Leo as a product to
	//     BUILD, hence `--backward`'s "2 subproducts, 0.02 built": it was trying to
	//     build a non-buildable card.
	//
	// A FLAG ONLY FOR AS LONG AS IT TAKES TO MEASURE IT: once it passes on both
	// benchmarks it becomes the default and the flag becomes negative.
	bool op_recipes = false;
	// SERIALISATION BY THE MATERIAL BALANCE. A mechanism is a flag ONLY WHILE IT
	// IS BEING MEASURED, then becomes the default: this one is therefore born on
	// and is turned off by `--no-serial`, like `--adapt-to-peak` after its
	// promotion. It is the only route the arithmetic of the 105 orders leaves open.
	bool serial = true;
	// RETURN TO THE RUNG: probability that an NRPA rollout restarts from an archive
	// cell instead of the root. The half of SIW_R the rollouts were missing; the
	// closed form says `Sigma b^(l_i)` only exists when each block is searched from
	// the previous rung. Only bites under armed serialisation. 0 = off.
	double reenter = 0.5;
	bool quota_h = false;
	// THE GRID (rips x overlap), derived from the refined closed form (sympy
	// 13/13). Archive key = the (resolutions, overlap) cell, one elite per cell (28
	// max); UNIFORM re-entry over the cells; A2 roots = ALL the ripped cells. The
	// canonical MAP-Elites/Pareto-MCTS gesture, and the only key under which the
	// cost of interleaving the conjunction is additive (the scalar loses
	// ~2*b^(l_t-1)/L = 4e7 at the measured values). The regime without
	// serialisation, and it requires --resolve. False by default.
	// FULL GO-EXPLORE, first half: the archives of the FINISHER's searches
	// (A1/A2/phase 2) enter the global archive, with their paths re-rooted at the
	// start. Until now they DIED with their phase; the literature (Go-Explore:
	// "each phase's discoveries feed the archive back") and the measurement (the
	// joint lines are born in the finisher) say the same thing. False by default
	// while it is being measured.
	bool archive_fin = false;
	// FULL GO-EXPLORE, second half: under --rounds, the global archive and the
	// merged policy PERSIST from one round to the next, and the next round's
	// rollout workers are SEEDED with the carried cells. Without it, each round
	// restarts from an empty archive and only the joint line carries over. False by
	// default while it is being measured.
	bool carry = false;
	// DISCIPLINE: never offer a player NEGATION on the player's own chain link
	// (Crystal Wing, Zalen, Silver Hound...). The --no-activate/--no-chain family:
	// a chosen constraint, not a quality pruning. The effects targeted are derived
	// from the declared table (NEGATE/DISABLE categories), with zero names.
	bool no_self_negate = false;
	// DISCIPLINE: the whole combo lives in MAIN PHASE 1, so entering the Battle
	// Phase (hence Main 2) is removed from the enumeration and "-> End Phase"
	// remains.
	bool mp1_only = false;
	// The domain in TURNS (--turns): 0/1 = one turn (historical), 2 = the line
	// crosses the opponent's turn (quick windows only).
	uint64_t turns = 0;
	// THE SELF-REFINING LADDER: re-serialisation from the best frontier cell when
	// sp_max has stagnated for N measured rollouts. 0 = off; a mechanism is a flag
	// only while it is being measured.
	// mesurer.
	uint64_t refine_after = 0;
	// THE INTERNAL LOOP (Go-Explore/ExIt shape): the --solve-ms budget is cut into
	// N internal rounds, and between two rounds the best JOINT line written is
	// re-injected as the next round's approach. "One command, one final result":
	// re-injection is no longer the operator's job.
	// 1 = the historical behaviour (no banner, no round).
	uint64_t rounds = 1;
	// Weight subtracted from the logit of a phase change. 0 = off (the control).
	double phase_w = 0.0;
	// BACKWARD CHAINING AS A BIAS.
	//
	// Weight added to the logit of the choices that PLAY a card whose backward
	// decomposition requires presence ON THE FIELD, i.e. the host of an acquisition
	// edge. `--assign-bias` designates MATERIALS and bites on SELECTION prompts;
	// this one designates OPERATORS and bites on "what to play". Requires
	// `--op-recipes` to have anything to work with.
	//
	// THE RULE, NON-NEGOTIABLE: a plan is a BIAS, never a pruning.
	double op_bias = 0.0;
	// Cards OBSERVED by the probe, with no constraint at all (`--watch`). The
	// objection that led to them: `--resolve` is a DISGUISED HINT (automatic hint
	// bias + gradient + goal requirement), so a probe that can only count
	// `--resolve` cards cannot measure "does the solver find it ON ITS OWN".
	std::vector<std::string> watch_specs;
	// --guard-keep: cards a guard clause rests on, never spent from the main phase
	// while they are the only cover. Resolved into codes like the rest.
	std::vector<std::string> guard_keep_specs;
	// BIAS DERIVED FROM THE TARGET. Two changes that are useless without each
	// other, hence one flag:
	//   1. the TARGET BOARD's codes enter `hint_cards`; until now only `--hint`
	//      (written by hand) and `--resolve` did, so a solver asked for a Liger
	//      Dancer had NO preference at all for the move "summon Liger Dancer";
	//   2. `MSG_SELECT_CARD` fills `Choice::card`, without which the prompt
	//      deciding WHICH Fusion to summon stays invisible to the bias.
	// This is NOT domain knowledge: it is reading the statement of the problem.
	// That is the difference with `--hint`, which is a crutch.
	// --- FOUR LEVERS AGAINST THE ARITY LAW ----------------------------------
	// All off by default, all separable, all judgeable on their own. See
	// SearchConfig for the full reasoning behind each.
	//
	// (1) --assign: subset prompts ALSO emit the two extreme subsets in the sense
	// of the recipes. Attacks the LEXICOGRAPHIC truncation of the enumeration,
	// which makes the right subset not rare but ABSENT.
	bool assign = false;
	// (1b) --assign-bias <f>: the mechanism the DIAGNOSIS points at. Leo Dancer is
	// offered 14 433 times in the "which Lunalight to send to the graveyard" prompt
	// and chosen 202 times, i.e. 1.4 %. This weight steers that choice towards the
	// codes the RECIPE GRAPH designates as materials.
	double assign_bias = 0.0;
	// (2) --hindsight <f>: every extra deck monster actually summoned becomes a
	// substitute goal, and the best line reaching it undergoes the NRPA gradient at
	// f x alpha (HER, NeurIPS 2017).
	//
	// DEFAULT 0.5. The value is not new: it is the one both benchmarks measured
	// (x20.6 on arity 3, complete separation of the supports at two seeds on A;
	// `>=2` from 3/10 to 9/10 on B). It is turned off by `--no-hindsight`.
	double hindsight = 0.5;
	size_t hindsight_k = 16;
	// (3) --recipe-w <f>: the recipe distance in the ROLLOUT SCORE, as progress. It
	// is the piece of work that was written without being wired in, since
	// `RecipeDistance` only existed in the finisher.
	// (4) --backward: the number of subproducts of the AND/OR decomposition already
	// built enters the PARTITION of the novelty table (Serialized IW over the
	// learned decomposition, instead of over the literal goal).
	bool backward = false;
	// (5) --canonical-zones: conflate the COLUMNS in the transposition key.
	// Attribution measurement (at the stable points): the column alone is worth
	// x33.7 in distinct values, by far the largest contributor, ahead of the prompt
	// payload (x1.38) and the processor state (x1.00). Opt-in: LINK arrows are
	// column-dependent.
	// (6) --elide-forced: a prompt offering only ONE legal answer is played inline,
	// with no depth, no table entry and no arena snapshot. It is what the key's
	// attribution points at: most nodes are not decision points. The finisher
	// already does it; the exhaustive search did not.
	//
	// NOT PROMOTED, and the reason is a measurement: the flag was WIRED ONLY into
	// `--growth`. The +61 % throughput and the x2.1 on boards therefore hold for
	// the EXHAUSTIVE path, never for the search. The wiring is fixed (that is a
	// correction: the flag claimed to act); the DEFAULT stays off until the
	// mechanism has been judged where it now acts.
	bool elide_forced = false;
	// Restores the HISTORICAL order of the subsets (ascending sizes), to attribute
	// the enumeration fix. A fix whose effect cannot be turned off is not
	// attributable, only believed.
	// sqrt-LTS: re-root the finisher at every hint.
	bool levin_reroot = false;
	// sqrt-LTS-H (arXiv:2605.30664 section 3.2): a HEURISTIC rerooter, soft and
	// non-zero everywhere. w_t = exp(-alpha * h(n_t)/h(root)). Unlike --reroot (a
	// HARD rerooter on the hints), it needs no discrete event: in a flat landscape
	// where the hint never lands, it is the only one of the two that can
	// decompose. 0 = off.
	double reroot_h = 0.0;
	// GOAL-ONLY MODE.
	//  --no-plan: the reference's repertoire is EMPTY. That is benchmark B; one
	//    factor changes, and it quantifies what the reference was worth.
	//  --target : the target board is built from scratch (instead of being captured
	//    from the reference then edited by a cascade of --board-remove).
	//  --no-ref : the positional replay is demoted to a duel TEMPLATE (header,
	//    flags, opponent); its line, its board and its repertoire are all set
	//    aside. Implies --no-plan and requires --target.
	bool no_plan = false;
	bool no_ref = false;
	// Ceiling on the decisions of a line being searched. 0 = derived from the
	// reference (ref_decisions * 3/2 + 32), the previous behaviour. To be raised
	// when the line AIMED AT is longer than the reference, e.g. aiming for three
	// Fusions when the reference places one. Too short a ceiling truncates SILENTLY.
	uint32_t max_decisions = 0;
	// Ceiling of the discrepancy ladder in the guided search. 12 was hard-coded;
	// it is the DEFAULT here, so every earlier measurement is reproduced byte for
	// byte. Raising it is the only way for a budget beyond ~100 s to be spent at
	// all on this mode: the ladder used to stop on the ceiling, never on time.
	uint32_t max_ecarts = 12;
	// NOVELTY CONTROL: two extra passes at k = 1, with then without the
	// pruning, printed side by side. Off by default -- it spends up to 40 s
	// of the search budget and starts the discrepancy ladder at k = 2.
	bool novelty_ab = false;
	// Line constraints, raw, resolved into codes once the card database is loaded.
	// cartes chargee.
	std::vector<std::string> summon_specs;      // "5:Zalen|Crystal Wing"
	std::vector<std::string> guard_specs;       // "5:CW@field|Zalen@field+Junk Signal@hand"
	std::vector<std::string> no_activate_specs; // "Assault Zone@field"
	// Cards the target player never CHAINS (--no-chain): the guards (Zalen, Crystal
	// Wing) answer a hypothetical threat, and chaining them onto our own
	// activations is a useless branch by construction.
	std::vector<std::string> no_chain_specs;
	std::string guard_off_spec;                 // "opphand<=2"
	std::vector<std::string> resolve_specs;     // "PSY-Framelord Omega:2"
	// The line must SUMMON these cards (same machinery as --resolve, over the
	// MSG_SUMMONING/SPSUMMONING messages): "card[:n]".
	std::vector<std::string> summon_min_specs;
	// SYNTHETIC starting position: a .ydk decklist + a starting hand, with no
	// starting replay. The target and the duel parameters stay the reference's.
	// reference.
	std::string deck_file;                      // "D:\...\test 3.ydk"
	std::string hand_spec;                      // "card|card|..." (default:
												// the reference's hand)
	// Domain hints: cards whose moves receive an NRPA sampling bonus.
	// d'echantillonnage NRPA.
	std::vector<std::string> hint_specs;
	// Approaches from earlier runs (best_approach_*.yrp) served to the finisher as
	// extra roots (full path + backtracks): the Go-Explore archive that persists
	// BETWEEN runs.
	std::vector<std::string> approach_files;
	// Cards ADDED to the opponent's hand in the starting duel (--opp-hand). Gives
	// the guard and the handrip something to work on when the start is a hand test
	// (an opponent with no hand); PLAYABLE cards (Nibiru...) are needed for the core
	// to open opponent response windows.
	std::vector<std::string> opp_hand_specs;
	// Editing of the target board, and material constraints.
	std::vector<std::string> board_add_specs;    // "Naturia Beast[@ATK|DEF]"
	std::vector<std::string> board_remove_specs; // "Hot Red Dragon..."
	// Target board built FROM SCRATCH (--target, same grammar as --board-add).
	std::vector<std::string> target_specs;
	std::vector<std::string> material_specs;     // "Chaos Angel:light"
};

// n-th summon (1-based) -> admissible canonical codes.
using SummonConstraints = std::map<uint32_t, std::vector<uint32_t>>;

// The full set of line constraints, resolved into codes.
struct LineConstraints {
	SummonConstraints summons;
	uint32_t guard_after = 0;
	std::vector<GuardClause> guard;
	// Cards the guard RESTS on (--guard-keep): canonical codes.
	std::vector<uint32_t> guard_keep;
	// Turning the guard off: no longer required when the opponent's hand holds at
	// most this many cards (-1 = never). A handrip deck extinguishes the threat.
	int guard_opp_hand_release = -1;
	std::map<uint32_t, uint32_t> no_activate;   // code -> LOCATION_ mask
	// Cards the target player never chains (--no-chain, canonical codes): pruned at
	// the ENUMERATION of the chain windows.
	std::vector<uint32_t> no_chain;
	// The deck's NEGATION effects (--no-self-negate): pairs (canonical code, desc;
	// desc 0 = the whole card), derived from the declared table after the scripts
	// are loaded. Never offered on a link of OURS.
	std::vector<std::pair<uint32_t, uint64_t>> self_negate;
	// Minimum number of effect resolutions, filtered by ACTIVATION zone (--resolve
	// "card[@zone][:n]"; see ResolveReq. Omega's graveyard effect does not count
	// towards the handrip, a measured false positive).
	std::vector<ResolveReq> resolve_min;
	// Domain hints (--hint): not constraints but a prior; they do not enter Any()
	// and spoil nothing.
	std::vector<uint32_t> hints;
	// Cards added to the OPPONENT's hand in the starting duel (--opp-hand). Not a
	// line constraint (outside Any()) but a modifier of the starting position, which
	// travels with the rest of the configuration.
	std::vector<uint32_t> opp_hand;
	// Material constraint: (canonical card, attribute mask). At least one material
	// of the summon must carry one of those attributes.
	std::vector<std::pair<uint32_t, uint32_t>> material_req;
	// Editing of the TARGET board (this is not a line constraint): cards added
	// (canonical code, position) and removed. Requires transplantation (--deck or
	// --start): in repair mode the reference can no longer act as a control over a
	// target it does not reach.
	std::vector<std::pair<uint32_t, uint32_t>> board_add;
	std::vector<uint32_t> board_remove;
	// --target: the target board does NOT start from the reference's capture but
	// from an empty table. `board_add` then carries the whole target.
	bool target_scratch = false;
	bool AnyBoardEdit() const {
		return !board_add.empty() || !board_remove.empty() || target_scratch;
	}
	// Every CHOSEN discipline counts, `self_negate` and `guard_keep` included:
	// leaving one out makes the whole constraint report vanish when it is the
	// only flag passed, which reads as "the flag does nothing" and is exactly
	// what the report exists to disprove.
	bool Any() const {
		return !summons.empty() || !guard.empty() || !no_activate.empty() ||
			   !no_chain.empty() || !resolve_min.empty() ||
			   !material_req.empty() || !self_negate.empty() ||
			   !guard_keep.empty();
	}
};

// --summon-min DERIVED from the counting, instead of written by hand.
//
// A `--summon-min "Liger Dancer:3"` typed by the operator is domain knowledge;
// derived from the target board, it is a CONSEQUENCE of the goal. Three
// non-negotiable guards:
//   - the cap of four --resolve/--summon-min entries is respected;
//   - only EXTRA DECK cards are kept. A main deck card can arrive through routes
//     that are not summons, and --resolve is only worth anything for RARE
//     EVENTS: a constraint posted on a frequent event collapses the policy;
//   - hand-written entries win: we complete, we do not replace.
//
// `cons` is READ only (so as not to duplicate a manual entry); the write goes
// into `cfg`, i.e. into the SEARCH GUIDE. This is not a way around the "verify
// before writing" check: a constraint DERIVED from the target board is
// REDUNDANT at verification time, and for an exact reason. If the final board
// carries three Liger Dancer, then three Fusion summons necessarily happened,
// since a summon places one card. The verifier, which compares the final board
// with the target, has therefore already checked it by checking the board. It
// serves to GUIDE earlier, not to JUDGE later.
void DeriveSummonMin(const std::vector<TargetCount>& counts,
					 const LineConstraints& cons, const CardDB& db,
					 SearchConfig& cfg) {
	constexpr size_t kMaxEntries = 4;
	std::vector<ResolveReq> eff = cons.resolve_min;   // the manual ones first
	size_t added = 0;
	for(const TargetCount& t : counts) {
		if(eff.size() >= kMaxEntries)
			break;
		if(t.token || !FromExtraDeck(t.mech))
			continue;
		bool already = false;
		for(const ResolveReq& r : eff)
			if(r.code == t.code) { already = true; break; }
		if(already)
			continue;
		ResolveReq r;
		r.code = t.code;
		r.min_count = t.need;
		r.on_summon = true;
		eff.push_back(r);
		++added;
	}
	if(!added) {
		std::printf("  --derive-summon-min: nothing to derive (no "
							"free extra-deck card under the ceiling of %zu).\n", kMaxEntries);
		return;
	}
	cfg.resolve_min = eff;
	std::printf("  --derive-summon-min: %zu constraint(s) DERIVED from "
					"the target board.\n     A search guide; redundant at "
					"verification, which judges the board itself:\n", added);
	for(const ResolveReq& r : eff)
		std::printf("      %ux %s\n", r.min_count, db.Name(r.code).c_str());
	if(counts.size() > kMaxEntries)
		std::printf("      (ceiling of %zu entries: target cards "
							"beyond it are not constrained)\n", kMaxEntries);
}


void Usage() {
	std::printf(
		"usage: combosolver <replay.yrpX> [options]\n\n"
				"Replays the line recorded in a .yrpX, captures the board it "
				"ends the turn on,\nthen searches for other ways to reach "
				"that board. Without --solve (or a flag\n"
				"that implies it) the run is a replay plus the self-checks, "
				"and no search.\n\nINPUT AND OUTPUT\n"
				"  --workdir <dir>    EDOPro installation. REQUIRED, unless "
				"the\n                     COMBOSOLVER_WORKDIR environment "
				"variable\n                     supplies it.\n"
				"  --scriptdir <dir>  card script set, highest priority first "
				"(repeatable).\n                     Use an export of the "
				"repository contemporary with the\n"
				"                     replay: a mismatched set makes the "
				"replay diverge in\n                     silence.\n"
				"  --player <0|1>     player whose turn is optimised (default "
				"0)\n  --outdir <dir>     where the replays produced are "
				"written (default\n                     solutions/)\n"
				"  --verbose          trace every decision\n"
				"  --help             this text\n\nWHAT TO SEARCH FOR\n"
				"  --solve            search for a line towards the target "
				"board\n  --start <replay>   rebuild the reference board from "
				"THAT duel (another deck,\n"
				"                     hand and seed). Implies --solve.\n"
				"  --deck <f.ydk>     rebuild the reference board from THAT "
				"decklist, with no\n                     starting replay. The "
				"opening hand comes from --hand\n"
				"                     (default: the reference's). Implies "
				"--solve.\n  --hand <cards>     opening hand, cards separated "
				"by '|' (codes or name\n                     fragments). Used "
				"with --deck.\n  --opp-hand <c>     ADD these cards to the "
				"opponent's hand in the starting\n"
				"                     duel (separated by '|', repeatable). "
				"Gives the guard and\n                     the handrip "
				"something to work on when the start is a hand\n"
				"                     test; PLAYABLE cards (Nibiru...) are "
				"needed for the core\n                     to open opponent "
				"response windows. The replays produced\n"
				"                     only replay with the same --opp-hand.\n"
				"  --target <c>       BUILD the target board from scratch, "
				"<c> = card[@ATK|DEF]\n                     (default ATK). "
				"The reference's capture does not enter.\n"
				"                     Repeatable.\n"
				"  --board-add <c>    EDIT the captured target board: also "
				"require this card.\n                     Same grammar as "
				"--target. Requires --deck or --start.\n"
				"  --board-remove <c> EDIT the captured target board: stop "
				"requiring this card.\n  --target-subset    the final board "
				"must CONTAIN the target instead of being\n"
				"                     EQUAL to it. This is already the "
				"default as soon as the\n                     target is "
				"posted with --target.\n  --target-exact     restore exact "
				"equality on a posted target (kept for A/B\n"
				"                     comparisons). Careful: it requires an "
				"empty spell/trap\n                     zone, which is "
				"unsatisfiable as soon as the hand holds a\n"
				"                     continuous spell.\n"
				"  --no-plan          discard the reference's REPERTOIRE: the "
				"NRPA policy\n                     starts uniform. Measures "
				"what the reference was worth.\n"
				"  --no-ref           GOAL-ONLY MODE: the replay is demoted "
				"to a duel template\n                     (flags, life "
				"points, opponent). Implies --no-plan and\n"
				"                     requires --target and --deck.\n"
				"  --approach <f.yrp> an approach written by an earlier run\n"
				"                     (best_approach_*.yrp), served to the "
				"finisher as an extra\n                     root (full path + "
				"backtracks). Repeatable. Must have been\n"
				"                     produced on the SAME starting duel (and "
				"--opp-hand).\n\nLINE CONSTRAINTS\n"
				"  --summon <spec>    the n-th summon (normal or special, "
				"Nibiru's count) must\n                     be one of the "
				"cards given. <spec> = n:card[|card...],\n"
				"                     card = a code or a name fragment that "
				"resolves uniquely.\n                     Repeatable. "
				"Example: --summon \"5:Zalen|Crystal Wing\"\n"
				"  --guard <spec>     from the n-th summon on, at every "
				"OPPONENT response\n                     window (where Nibiru "
				"would land) at least one clause must\n"
				"                     hold. <spec> = n:clause[|clause...],\n"
				"                     clause = card[@zone][+...], zones: hand "
				"field grave\n                     banished extra (default "
				"field). Predicate atom:\n"
				"                     oppbanished>=N (opponent cards "
				"banished).\n                     Example: --guard "
				"\"5:Crystal Wing|Zalen@field+Junk\n"
				"                     Signal@hand|27572350@field+oppbanished>="
				"1\"\n  --guard-keep <c>   this card is a GUARD RESOURCE: its "
				"main-phase\n                     activation is forbidden "
				"while the guard rests on it,\n"
				"                     i.e. while no clause WITHOUT it holds. "
				"Presence in\n                     hand is not availability: "
				"a once-per-turn already\n"
				"                     spent leaves the clause covering "
				"nothing. Chain\n                     windows are untouched "
				"(there the activation IS the\n"
				"                     protection). Repeatable.\n"
				"  --guard-off <cond> turn the guard off once the threat is "
				"gone. Form:\n                     opphand<=N, i.e. the guard "
				"is no longer required at\n"
				"                     windows where the opponent's hand holds "
				"at most N cards\n                     (a handrip deck "
				"empties Nibiru out of it).\n"
				"  --resolve <spec>   the line must resolve this card's "
				"effect at least n times\n"
				"                     before the board (<spec> = "
				"card[@zone][:n], default 1).\n"
				"                     @zone restricts the ACTIVATION zone "
				"(the Omega that rips\n                     activates from "
				"the field; without @field its graveyard\n"
				"                     effect would count too). Checked AT THE "
				"GOAL: a\n                     conforming board without the "
				"resolutions is not a\n                     solution. "
				"Repeatable, at most 4.\n                     Example: "
				"--resolve \"Omega@field:2\"\n"
				"  --summon-min <s>   the line must SUMMON this card at least "
				"n times\n                     (<s> = card[:n], default 1). "
				"Same machinery as --resolve,\n"
				"                     counted on summons. Shares the limit of "
				"4.\n  --material <spec>  summoning this card must consume at "
				"least one material of\n                     these "
				"attributes. <spec> = card:attr[,attr...],\n"
				"                     attributes: light dark earth water fire "
				"wind divine.\n                     Repeatable. Example: "
				"--material \"Chaos Angel:light\"\n"
				"  --no-activate <c>  forbid activating this card from a zone "
				"(<c> =\n                     card[@zone], default field; "
				"activating from the hand,\n"
				"                     which places the card, stays allowed). "
				"Repeatable.\n  --no-chain <c>     this card is never CHAINED "
				"at response windows (playing\n"
				"                     solo, every chain answers our own "
				"actions, so a guard\n                     that negates our "
				"cards is a useless branch). Forced\n"
				"                     triggers and idle commands stay "
				"allowed. Repeatable.\n  --no-self-negate   never offer one "
				"of the deck's own NEGATION effects on a\n"
				"                     chain link of ours. The effects are "
				"derived from the\n                     declared operator "
				"table; no card name is compiled in.\n"
				"  --mp1-only         the whole combo lives in Main Phase 1: "
				"entering the\n                     Battle Phase is removed "
				"from the enumeration\n                     (\"-> End Phase\" "
				"always remains).\n  --turns <n>        domain in turns: 1 "
				"(default) or 2, where the line crosses\n"
				"                     the opponent's turn and our only "
				"decisions there are the\n                     quick windows.\n"
				"\nSEARCH BUDGET\n  --solve-ms <ms>    search time budget "
				"(default 120000)\n  --threads <n>      search workers "
				"(default: every core)\n  --rounds <n>       INTERNAL LOOP: n "
				"rounds share --solve-ms, and the best\n"
				"                     joint line of each round is re-injected "
				"into the next\n                     (default 1, i.e. a "
				"single round)\n  --max-decisions <n>  depth ceiling of a "
				"searched line, in decisions.\n"
				"                     Default: derived from the reference "
				"(1.5x + 32).\n  --max-ecarts <n>   ceiling of the "
				"discrepancy ladder (default 12). The ladder\n"
				"                     stops on this ceiling OR on --solve-ms, "
				"whichever comes\n                     first; at 12 a budget "
				"over ~100 s stays unspent.\n"
				"  --max-rollouts <n> DETERMINISTIC MODE: budget in ROLLOUTS "
				"per worker instead\n                     of wall time. Wall "
				"time is the cause of the run-to-run\n"
				"                     variation; with --threads 1, two "
				"executions do exactly\n                     the same work.\n"
				"  --max-nodes <n>    the same, in nodes expanded per worker\n"
				"  --arena-mb <n>     address space reserved for the arena "
				"(default 256)\n  --tt-mb <n>        transposition table "
				"SHARED between workers (lazy SMP), in\n"
				"                     MB per pass (default 64; 0 = private "
				"tables)\n  --seed <n>         seed of the rollouts (default: "
				"derived from the clock and\n"
				"                     printed; passing it back replays the "
				"same rollouts)\n\nSEARCH MECHANISMS\n"
				"  --no-nrpa          greedy rollouts only, with no learned "
				"policy\n  --novelty <n>      patience of the novelty pruning "
				"(default: auto-calibrated\n"
				"                     from the width measurement)\n"
				"  --no-novelty       disable novelty pruning\n"
				"  --novelty-ab      A/B CONTROL of the novelty pruning: two "
				"extra passes\n                     at k = 1, with then "
				"without it, printed side by\n"
				"                     side. Costs up to 2 x 20 s of "
				"--solve-ms and\n                     starts the discrepancy "
				"ladder at k = 2.\n  --nrpa-bias <x>    GNRPA bias of the "
				"repertoire's moves (default 1.5)\n"
				"  --nrpa-keep <x>    persistence of the NRPA policy across "
				"restarts: weights\n                     attenuated by x "
				"instead of restarting from zero\n"
				"                     (default 0.5; 0 = a virgin policy)\n"
				"  --nrpa-alpha <x>   NRPA adaptation step (default 1.0). "
				"Small = the policy\n                     moves slowly and "
				"explores one basin for longer.\n"
				"  --nrpa-iters <n>   iterations per NRPA level (default 24). "
				"A level-L call\n                     costs n^L rollouts.\n"
				"  --nrpa-level <n>   NRPA nesting level, 1..4. Default: 3 "
				"when the rollout\n                     budget exceeds 180 s, "
				"2 otherwise. The effective level is\n"
				"                     always printed.\n"
				"  --nrpa-temp <t>    softmax temperature of the rollouts "
				"(default 1.0). t < 1\n                     concentrates the "
				"mass on the best ranked moves WITHOUT\n"
				"                     changing the ranking (GNRPA, "
				"arXiv:2003.10024).\n  --hint <card>      domain hint: the "
				"moves that engage this card (summon,\n"
				"                     activate, position) receive an NRPA "
				"sampling bonus.\n                     Repeatable.\n"
				"  --hint-bias <b>    weight of the HINT bias in the sampling "
				"(default 2.0).\n                     The "
				"--resolve/--summon-min cards receive it\n"
				"                     automatically.\n"
				"  --phase-w <x>      weight subtracted from the logit of a "
				"phase change\n                     (0 = off). Ending the "
				"turn is irreversible and is drawn\n"
				"                     uniformly among the idle choices.\n"
				"  --resolve-weight <x>  weight of a required resolution in "
				"the rollouts'\n                     gradient (default 250; "
				"100 = one target card)\n  --max-subsets <n>  subsets emitted "
				"per selection prompt (default 24). This\n"
				"                     caps the branching factor of every "
				"SELECT_CARD /\n                     SELECT_SUM; sizes are "
				"visited alternating from both ends.\n"
				"  --elide-forced     a prompt with a SINGLE legal answer is "
				"played inline: no\n                     table entry, no "
				"arena snapshot, no depth, no evaluation.\n"
				"                     74.6 %% of nodes offer no choice at "
				"all.\n  --canonical-zones  explore only one representative "
				"free zone per zone type.\n"
				"                     Link arrows and columns can change "
				"everything: judge it\n                     before trusting "
				"it.\n   --no-adapt-to-peak\n"
				"                     ON BY DEFAULT: adapt only the PREFIX "
				"that produced the\n                     score. A rollout's "
				"score is a MAX over prefixes, but the\n"
				"                     adaptation used to reinforce EVERY "
				"step, so a line\n                     peaking at step 200 "
				"then wandering for 230 more learned\n"
				"                     the collapse as strongly as the climb. "
				"This flag turns it\n                     OFF, to replay the "
				"A/B.\n  --no-serial        turn off serialisation by the "
				"material balance. On by\n"
				"                     default, the novelty table reopens at "
				"every subgoal of\n                     the LP's plan instead "
				"of only at every target card\n                     placed.\n"
				"  --reenter <p>      RETURN TO THE RUNG: probability that an "
				"NRPA rollout\n                     restarts from an archive "
				"cell (a rung of the x* ladder)\n"
				"                     instead of from the root. Only bites "
				"under armed\n                     serialisation. Default "
				"0.5; 0 = the A/B control.\n"
				"  --refine-after <n>  self-refining ladder: re-serialise "
				"from the best\n                     frontier cell once "
				"sp_max has stagnated for n measured\n"
				"                     rollouts (0 = off)\n"
				"  --quota-h          put the path's observed quota uses into "
				"the LP's\n                     capacities at refinement time "
				"(red-black relaxation)\n  --carry            under --rounds, "
				"the global archive and the merged policy\n"
				"                     persist from one round to the next\n"
				"  --archive-fin      the finisher's search archives enter "
				"the global archive,\n                     with their paths "
				"re-rooted\n\nGOAL DECOMPOSITION\n"
				"  --recipes <w>      RECIPE GRAPH: h becomes the distance in "
				"SUMMONS still to\n                     be made over the "
				"OBSERVED recipes, intermediate materials\n"
				"                     included, i.e. a distance that "
				"DECREASES where the flat h\n"
				"                     does not move. w = 0: the graph is fed "
				"and MEASURED\n                     without entering the "
				"cost. w > 0: it weighs in h. It\n"
				"                     never PRUNES; an unknown recipe is "
				"worth 1, so at worst h\n                     falls back to "
				"today's flat h.\n  --no-seed-recipes  do NOT seed the graph "
				"from card text: the graph then only\n"
				"                     learns from successful summons\n"
				"  --no-seed-quant    seed the NAMED materials only, without "
				"the CARDINAL\n                     requirements (\"3 "
				"\\\"Lunalight\\\" monsters\", \"2 Level 4\n"
				"                     monsters\")\n"
				"  --op-recipes       seed the recipe graph from the DECLARED "
				"OPERATORS instead\n                     of from English card "
				"text: recipes as CODES\n                     "
				"(Fusion.AddProcMix*), plus the node type that was\n"
				"                     missing, \"this CODE can be ACQUIRED\", "
				"for every\n                     EFFECT_ADD_CODE / "
				"EFFECT_CHANGE_CODE of the deck. The\n"
				"                     edges posted are printed one by one. "
				"Requires --recipes.\n  --backward         BACKWARD "
				"SERIALISATION (Retro*, AO*). A summon is an AND\n"
				"                     node: arity, fatal forwards, becomes a "
				"DECOMPOSITION\n                     backwards. The number of "
				"subproducts already built enters\n"
				"                     the partition of the novelty table, "
				"which therefore\n                     reopens BEFORE any "
				"target card is placed.\n  --assign           RESOLVED "
				"ASSIGNMENT (arXiv:2010.12001). Subset prompts\n"
				"                     ALSO emit the two extreme subsets in "
				"the sense of the\n                     recipes. The "
				"enumeration's truncation is LEXICOGRAPHIC:\n"
				"                     past --max-subsets the right subset is "
				"not rare, it is\n                     ABSENT, and no weight "
				"makes up for that.\n  --assign-bias <f>  sampling weight of "
				"the moves that engage a code the\n"
				"                     RECIPE graph designates as a MATERIAL\n"
				"  --op-bias <f>      weight added to the moves that PLAY a "
				"card the backward\n                     decomposition "
				"requires ON THE FIELD, i.e. the hosts of\n"
				"                     the acquisition edges. Requires "
				"--recipes and\n                     --op-recipes; its "
				"liveness is printed, and at zero it is\n"
				"                     inert.\n"
				"  --hindsight <f>    HINDSIGHT (HER, NeurIPS 2017). Every "
				"extra deck monster\n                     actually summoned "
				"becomes a substitute goal, and the best\n"
				"                     line reaching it undergoes the NRPA "
				"gradient at f x\n                     alpha. DEFAULT 0.5; "
				"--no-hindsight turns it off.\n"
				"  --hindsight-k <n>  substitute goals kept at most (default "
				"16)\n  --landmarks <f|d>  LEARNED LANDMARK GRAPH "
				"(arXiv:2508.21564): learns, from\n"
				"                     RESOLVED plans, the facts (card, zone, "
				"COUNT) every plan\n                     reaches, in which "
				"order, and how many times. Repeatable.\n"
				"                     The graph is printed before it is "
				"allowed to weigh.\n  --landmark-w <f>   weight of a landmark "
				"achievement in the ROLLOUT SCORE, in\n"
				"                     units of material (one target card "
				"placed is worth 100).\n                     0 = learned and "
				"measured without weighing.\n"
				"  --landmark-h <f>   weight of the landmark h in the "
				"FINISHER (same entry\n                     point as "
				"--recipes)\n  --derive-summon-min  derive the --summon-min "
				"constraints from the TARGET\n"
				"                     BOARD instead of writing them by hand: "
				"3x Liger Dancer =\n                     three Fusion summon "
				"EVENTS (never \"three\n                     "
				"Polymerizations\": the trigger varies). The counting is\n"
				"                     always printed; this flag wires it into "
				"the constraints.\n\nPOLICY LEARNING FROM SOLVED LINES\n"
				"  --adapt <f|dir>    ADAPTATION replay of the corpus "
				"(repeatable): the same\n                     lines, recorded "
				"as SEQUENCES OF DECISIONS (legal choices\n"
				"                     + the chosen one) and adapted into the "
				"NRPA policy before\n                     the first rollout.\n"
				"  --adapt-passes <n>  adaptation passes per line (default 4; "
				"0 disables the\n                     mechanism without "
				"touching the recording, which is the\n"
				"                     A/B)\n"
				"  --options <n>      OPTIONS: catalogue of n macros mined "
				"from the --adapt\n                     corpus and offered as "
				"ONE sampling unit to the NRPA\n"
				"                     rollouts (0 = off, the default)\n"
				"  --options-support <n>  minimum occurrences of a macro "
				"(default 2)\n  --options-online <s>  ONLINE MINING: re-mine "
				"the catalogue every s seconds\n"
				"                     from the run's own best lines (0 = "
				"off). No external\n                     corpus needed: the "
				"run starts bare and arms itself.\n"
				"                     Implies --options 256 when --options is "
				"not given.\n  --options-per-worker <n>  and at most n per "
				"worker (default 2): without a\n"
				"                     quota the workers pour the same shared "
				"best line in\n                     sixteen times.\n\n"
				"HEAD BANDIT (MCPS)\n  --qhat <k>         over the first k "
				"decisions of a rollout, the move is\n"
				"                     chosen by argmax of (n Q + n^ Q^)/(n + "
				"n^): Q is the mean\n                     reward of the "
				"rollouts through this node then this move,\n"
				"                     Q^ the mean over ALL rollouts "
				"containing this move AND\n"
				"                     those of the path, in any order (MCPS,\n"
				"                     arXiv:2510.06381). Weights proportional "
				"to the sample\n                     sizes, so no bias "
				"hyperparameter. Past k, NRPA samples as\n"
				"                     before. Those k decisions are EXCLUDED "
				"from the NRPA\n                     gradient. 0 = off.\n"
				"  --qhat-window <W>  size of the sliding window of rollouts, "
				"PER worker\n                     (default 4096). The "
				"measured memory is printed.\n"
				"  --qhat-rho <r>     visits after which a non-root node "
				"FREEZES its\n                     permutation statistic "
				"(default 32)\n  --qhat-nodes <n>   cap on the bandit's nodes "
				"per worker (default 65536)\n"
				"  --no-qhat-probe    do not print the first-decision probe\n"
				"  --ctx-shrink <k>   TWO-LEVEL policy: one weight per move "
				"AND one per (move,\n                     context), mixed "
				"convexly by s = n/(n+k) where n is the\n"
				"                     evidence of the contextual cell. "
				"Negative k (default) =\n                     off. The "
				"agreement curve printed by --adapt calibrates k\n"
				"                     without spending a run.\n"
				"  --ctx-max <n>      cap on the contextual level's entries "
				"per worker (default\n                     262144, 0 = "
				"unlimited)\n\nFINISHER\n  --finisher <mode>  levin "
				"(Go-Explore archive + backtracks + Levin Tree\n"
				"                     Search over the NRPA policy, the "
				"default), mono (the old\n"
				"                     one: the single best state), ab (both "
				"at equal budget)\n  --archive-k <n>    size of the "
				"finisher's state archive (default 16)\n"
				"  --finisher-min <ms>  minimum budget RESERVED for the "
				"finisher (0 = the\n                     original split)\n"
				"  --levin-h <x>      PHS* weight of the distance to the goal "
				"(missing cards +\n                     missing resolutions) "
				"in the finisher's cost (default 1.0;\n"
				"                     0 = pure Levin, blind to the goal)\n"
				"  --reroot           sqrt-LTS with a HARD rerooter "
				"(arXiv:2412.05196): the\n"
				"                     finisher re-roots at every HINT (the "
				"number of target\n                     cards placed "
				"changes). With no hint, inert.\n"
				"  --reroot-h <a>     sqrt-LTS-H with a HEURISTIC rerooter\n"
				"                     (arXiv:2605.30664 section 3.2): weight "
				"exp(-a*h/h0) on\n                     EVERY node, hence "
				"active even when no hint lands.\n"
				"                     a = inverse temperature (0 = off). "
				"Exclusive with\n                     --reroot.\n"
				"  --no-dive-full     do NOT push an arena level at every "
				"replayed chain\n                     node (on by default)\n"
				"  --no-merged-pop    do NOT merge the arena pop when "
				"returning to the\n                     shared ancestor (on "
				"by default)\n  --finisher-post-goal  under --optimize, a "
				"goal node CONTINUES instead of\n"
				"                     stopping (post-goal recovery)\n"
				"  --finisher-options  the catalogue's macros become EDGES of "
				"the Levin tree\n                     (cost log 1/pi, "
				"advancing k decisions, an abort being a\n"
				"                     dead edge)\n\nCOST OPTIMISATION\n"
				"  --optimize         anytime COST OPTIMISATION: the search "
				"no longer stops at\n                     the first solution "
				"(each one tightens the bound), the\n"
				"                     NRPA goal score becomes lexicographic "
				"(burned, then\n                     actions, then "
				"decisions), rollouts continue PAST the goal\n"
				"                     (recoveries reduce the burned count), "
				"and the finisher\n                     runs even when lines "
				"already exist\n  --burn-slack <n>   slack of the burned B&B "
				"bound (default 6): cut the states\n"
				"                     above best_burned + n. Burned cards are "
				"NOT monotonic\n                     (recoveries), and the "
				"slack is measured on the reference.\n"
				"                     255 = off.\n"
				"  --burn-limit <n>   seed of the bound: best burned count "
				"known in advance\n                     (0 = none)\n"
				"  --no-burn-share    do NOT share the burned bound between "
				"workers (default:\n                     shared, so a worker "
				"that improves it cuts for all)\n\nOPPONENT TEST\n"
				"  --fire <card>      adds the card to the opponent's hand "
				"and makes the\n                     opponent PLAY it at "
				"every window where it is legal (one\n"
				"                     attempt per window); the search must "
				"then close the board\n                     back up from the "
				"post-injection state. The static guard\n"
				"                     becomes a dynamic proof. The replays "
				"produced only replay\n                     with --opp-hand "
				"<card> in judge mode.\n  --fire-spare <c>   card that may be "
				"SACRIFICED to answer: the target board\n"
				"                     without it is also accepted at the goal "
				"(answering with\n                     Zalen consumes Junk "
				"Signal)\n  --fire-ms <ms>     search budget per window "
				"(default 45000)\n  --fire-bake        bake the drawn card "
				"into the header of the replays\n"
				"                     produced (opponent deck, served into "
				"the hand by the\n                     pseudo-shuffle), so "
				"they replay from their own file and\n"
				"                     EDOPro can watch them with no flag. The "
				"card replaces the\n                     last card of the "
				"opponent's original hand.\n"
				"  --fire-no-chain <c>  no-chain list specific to the "
				"post-injection\n                     continuation (the "
				"global --no-chain lists are lifted\n"
				"                     there). Stages a precise answer: "
				"forbidding Crystal Wing\n"
				"                     forces the Zalen+Junk Signal route. "
				"Repeatable.\n  --fire-open        only inject at OPEN "
				"windows (empty chain): the drawn card\n"
				"                     STARTS a chain instead of being chained "
				"onto our effects,\n                     which is the real "
				"threat\n\nMEASUREMENT AND DIAGNOSTICS\n"
				"  --growth           measure the growth of the state graph\n"
				"  --growth-max <n>   maximum depth explored (default 14)\n"
				"  --growth-ms <ms>   time budget per depth (default 20000)\n"
				"  --width            measure the effective width (IW atoms) "
				"along the\n                     reference line, with no "
				"search\n  --watch <card>     card OBSERVED by "
				"--probe-repeat, with NO constraint, no\n"
				"                     gradient and no hint bias. Use it "
				"whenever measuring\n                     whether the solver "
				"finds something ON ITS OWN: --resolve\n"
				"                     is a disguised hint (it receives "
				"hint_bias\n                     automatically). Repeatable, "
				"at most 4.\n  --probe-repeat     REPETITION PROBE: per "
				"--summon-min/--resolve card, the\n"
				"                     histogram of summons PER ROLLOUT and, "
				"at the FIRST one,\n                     the recipe distance "
				"to ONE MORE copy compared with the\n"
				"                     same distance from the starting state. "
				"Separates the two\n                     failures "
				"best_overlap conflates: the 2nd copy NEVER\n"
				"                     ATTEMPTED (the material was there, a "
				"sampling failure)\n                     from the 2nd ALWAYS "
				"LOST (the chain was consumed, an h\n"
				"                     failure). Implies --recipes 0.\n"
				"  --operators        DECLARED OPERATOR HARNESS: extracts the "
				"operator table\n                     from the deck's LUA "
				"SCRIPTS (preconditions, product,\n"
				"                     granted state, recipes), prints it, "
				"then CONFRONTS it\n                     with the replayed "
				"plan. The constants come from the\n"
				"                     game's constant.lua: no card is named "
				"in the code. An\n                     instrument, not a "
				"mechanism.\n  --profile          hot path profile (rdtsc "
				"probes per phase, \"everything\n"
				"                     else\" line included); the instrument "
				"costs something, and\n                     quantifying it is "
				"part of the measurement\n"
				"  --no-arena         system allocator, no snapshot "
				"(comparison)\n  --keep-gc          leave the Lua garbage "
				"collector running (comparison)\n");
}

bool ParseArgs(int argc, char** argv, Options& o) {
	for(int i = 1; i < argc; ++i) {
		std::string a = argv[i];
		auto next = [&](const char* what) -> const char* {
			if(i + 1 >= argc) {
				std::printf("!! %s expects a value\n", what);
				return nullptr;
			}
			return argv[++i];
		};
		// SIMPLE BOOLEAN FLAGS, taken out of the `else if` chain below. MSVC caps
		// nesting at 128 blocks (C1061) and the chain was at the limit: every new
		// value-less flag now goes through this table, which costs nothing and can no
		// longer break the build.
		{
			static const struct { const char* name; bool Options::* member; }
			kBoolFlags[] = {
				{ "--assign",           &Options::assign },
				{ "--backward",         &Options::backward },
				{ "--elide-forced",     &Options::elide_forced },
				{ "--novelty-ab",       &Options::novelty_ab },
				{ "--operators",        &Options::operators },
				{ "--op-recipes",       &Options::op_recipes },
				{ "--quota-h",          &Options::quota_h },
				{ "--archive-fin",      &Options::archive_fin },
				{ "--carry",            &Options::carry },
				{ "--no-self-negate",   &Options::no_self_negate },
				{ "--mp1-only",         &Options::mp1_only },
			};
			bool matched = false;
			for(const auto& f : kBoolFlags)
				if(a == f.name) { o.*(f.member) = true; matched = true; break; }
			if(matched)
				continue;
			// NEGATIVE FLAGS OF THE MECHANISMS PROMOTED TO DEFAULTS. Once it passes
			// on BOTH benchmarks a mechanism becomes the default, and the flag
			// becomes negative: it then only serves to turn the mechanism off.
			{
				static const struct { const char* name; bool Options::* member; }
				kNoFlags[] = {
					{ "--no-adapt-to-peak", &Options::adapt_to_peak },
					{ "--no-serial",        &Options::serial },
				};
				for(const auto& f : kNoFlags)
					if(a == f.name) { o.*(f.member) = false; matched = true; break; }
				if(matched)
					continue;
			}
			if(a == "--no-hindsight") {
				o.hindsight = 0.0;
				continue;
			}
		}
		// UNSIGNED INTEGER FLAGS, same reason: the `else if` chain is at the
		// compiler's limit, and one more flag broke it (C1061, measured). Any option
		// with a value added later goes through here.
		{
			static const struct { const char* name; uint64_t Options::* member; }
			kU64Flags[] = {
				{ "--max-rollouts", &Options::max_rollouts },
				{ "--max-nodes",    &Options::max_nodes },
				{ "--refine-after", &Options::refine_after },
				{ "--turns",        &Options::turns },
				{ "--rounds",       &Options::rounds },
			};
			bool matched = false;
			for(const auto& f : kU64Flags)
				if(a == f.name) {
					const char* v = next(f.name);
					if(!v)
						return false;
					o.*(f.member) = std::strtoull(v, nullptr, 10);
					matched = true;
					break;
				}
			if(matched)
				continue;
		}
		if(a == "--workdir") {
			const char* v = next("--workdir"); if(!v) return false;
			o.workdir = v;
		} else if(a == "--scriptdir") {
			const char* v = next("--scriptdir"); if(!v) return false;
			o.scriptdirs.emplace_back(v);
		} else if(a == "--player") {
			const char* v = next("--player"); if(!v) return false;
			o.target_player = std::atoi(v);
		} else if(a == "--arena-mb") {
			const char* v = next("--arena-mb"); if(!v) return false;
			o.arena_mb = static_cast<size_t>(std::atoi(v));
		} else if(a == "--no-arena") {
			o.no_arena = true;
		} else if(a == "--keep-gc") {
			o.stop_gc = false;
		} else if(a == "--growth") {
			o.growth = true;
		} else if(a == "--growth-max") {
			const char* v = next("--growth-max"); if(!v) return false;
			o.growth_max = static_cast<uint32_t>(std::atoi(v));
		} else if(a == "--growth-ms") {
			const char* v = next("--growth-ms"); if(!v) return false;
			o.growth_ms = std::atof(v);
		} else if(a == "--start") {
			const char* v = next("--start"); if(!v) return false;
			o.start_replay = v;
			o.solve = true;
		} else if(a == "--deck") {
			const char* v = next("--deck"); if(!v) return false;
			o.deck_file = v;
			o.solve = true;
		} else if(a == "--hand") {
			const char* v = next("--hand"); if(!v) return false;
			o.hand_spec = v;
		} else if(a == "--hint") {
			const char* v = next("--hint"); if(!v) return false;
			o.hint_specs.emplace_back(v);
		} else if(a == "--guard-keep") {
			const char* v = next("--guard-keep"); if(!v) return false;
			o.guard_keep_specs.emplace_back(v);
		} else if(a == "--approach") {
			const char* v = next("--approach"); if(!v) return false;
			o.approach_files.emplace_back(v);
		} else if(a == "--opp-hand") {
			const char* v = next("--opp-hand"); if(!v) return false;
			o.opp_hand_specs.emplace_back(v);
		} else if(a == "--board-add") {
			const char* v = next("--board-add"); if(!v) return false;
			o.board_add_specs.emplace_back(v);
		} else if(a == "--board-remove") {
			const char* v = next("--board-remove"); if(!v) return false;
			o.board_remove_specs.emplace_back(v);
		} else if(a == "--material") {
			const char* v = next("--material"); if(!v) return false;
			o.material_specs.emplace_back(v);
		} else if(a == "--outdir") {
			const char* v = next("--outdir"); if(!v) return false;
			o.outdir = v;
		} else if(a == "--solve") {
			o.solve = true;
		} else if(a == "--solve-ms") {
			const char* v = next("--solve-ms"); if(!v) return false;
			o.solve_ms = std::atof(v);
		} else if(a == "--threads") {
			const char* v = next("--threads"); if(!v) return false;
			o.threads = static_cast<unsigned>(std::atoi(v));
		} else if(a == "--width") {
			o.width = true;
		} else if(a == "--novelty") {
			const char* v = next("--novelty"); if(!v) return false;
			o.novelty = std::atoi(v);
		} else if(a == "--no-novelty") {
			o.novelty = 0;
		} else if(a == "--no-nrpa") {
			o.nrpa = false;
		} else if(a == "--seed") {
			const char* v = next("--seed"); if(!v) return false;
			o.seed = std::strtoull(v, nullptr, 10);
		} else if(a == "--nrpa-bias") {
			const char* v = next("--nrpa-bias"); if(!v) return false;
			o.nrpa_bias = std::atof(v);
		} else if(a == "--nrpa-keep") {
			const char* v = next("--nrpa-keep"); if(!v) return false;
			o.nrpa_keep = std::atof(v);
		} else if(a == "--nrpa-alpha") {
			const char* v = next("--nrpa-alpha"); if(!v) return false;
			o.nrpa_alpha = std::atof(v);
		} else if(a == "--nrpa-iters") {
			const char* v = next("--nrpa-iters"); if(!v) return false;
			o.nrpa_iters = static_cast<uint32_t>(std::atoi(v));
		} else if(a == "--finisher") {
			const char* v = next("--finisher"); if(!v) return false;
			o.finisher = v;
			if(o.finisher != "levin" && o.finisher != "mono" &&
			   o.finisher != "ab") {
				std::printf("!! --finisher attend levin, mono "
											"ou ab\n");
				return false;
			}
		} else if(a == "--archive-k") {
			const char* v = next("--archive-k"); if(!v) return false;
			o.archive_k = static_cast<size_t>(std::atoi(v));
		} else if(a == "--finisher-min") {
			const char* v = next("--finisher-min"); if(!v) return false;
			o.finisher_min = std::atof(v);
		} else if(a == "--levin-h") {
			const char* v = next("--levin-h"); if(!v) return false;
			o.levin_h = std::atof(v);
		} else if(a == "--resolve-weight") {
			const char* v = next("--resolve-weight"); if(!v) return false;
			o.resolve_weight = std::atof(v);
		} else if(a == "--optimize") {
			o.optimize = true;
		} else if(a == "--fire") {
			const char* v = next("--fire"); if(!v) return false;
			o.fire_spec = v;
		} else if(a == "--fire-spare") {
			const char* v = next("--fire-spare"); if(!v) return false;
			o.fire_spare_specs.emplace_back(v);
		} else if(a == "--fire-ms") {
			const char* v = next("--fire-ms"); if(!v) return false;
			o.fire_ms = std::atof(v);
		} else if(a == "--fire-bake") {
			o.fire_bake = true;
		} else if(a == "--fire-no-chain") {
			const char* v = next("--fire-no-chain"); if(!v) return false;
			o.fire_no_chain_specs.emplace_back(v);
		} else if(a == "--fire-open") {
			o.fire_open = true;
		} else if(a == "--burn-slack") {
			const char* v = next("--burn-slack"); if(!v) return false;
			o.burn_slack = static_cast<uint32_t>(std::atoi(v));
		} else if(a == "--burn-limit") {
			const char* v = next("--burn-limit"); if(!v) return false;
			o.burn_limit = static_cast<uint32_t>(std::atoi(v));
		} else if(a == "--no-burn-share") {
			o.burn_share = false;
		} else if(a == "--adapt") {
			const char* v = next("--adapt"); if(!v) return false;
			o.adapt_files.emplace_back(v);
		} else if(a == "--adapt-passes") {
			const char* v = next("--adapt-passes"); if(!v) return false;
			o.adapt_passes = static_cast<uint32_t>(std::atoi(v));
		} else if(a == "--options") {
			const char* v = next("--options"); if(!v) return false;
			o.options_n = static_cast<uint32_t>(std::atoi(v));
		} else if(a == "--options-support") {
			const char* v = next("--options-support"); if(!v) return false;
			o.options_support = static_cast<uint32_t>(std::atoi(v));
		} else if(a == "--options-online") {
			const char* v = next("--options-online"); if(!v) return false;
			o.options_online = static_cast<uint32_t>(std::atoi(v));
		} else if(a == "--options-per-worker") {
			const char* v = next("--options-per-worker"); if(!v) return false;
			o.options_per_worker = static_cast<uint32_t>(std::atoi(v));
		} else if(a == "--no-merged-pop") {
			o.merged_pop = false;
		} else if(a == "--finisher-post-goal") {
			o.finisher_post_goal = true;
		} else if(a == "--finisher-options") {
			o.finisher_options = true;
		} else if(a == "--max-decisions") {
			const char* v = next("--max-decisions"); if(!v) return false;
			o.max_decisions = static_cast<uint32_t>(std::atoi(v));
		} else if(a == "--max-ecarts") {
			const char* v = next("--max-ecarts"); if(!v) return false;
			o.max_ecarts = static_cast<uint32_t>(std::atoi(v));
		} else if(a == "--reroot") {
			o.levin_reroot = true;
		} else if(a == "--reroot-h") {
			const char* v = next("--reroot-h"); if(!v) return false;
			o.reroot_h = std::atof(v);
		} else if(a == "--no-dive-full") {
			o.dive_full = false;
		} else if(a == "--no-plan") {
			o.no_plan = true;
		} else if(a == "--no-ref") {
			o.no_ref = true;
			o.no_plan = true;
		} else if(a == "--target") {
			const char* v = next("--target"); if(!v) return false;
			o.target_specs.emplace_back(v);
		} else if(a == "--nrpa-temp") {
			const char* v = next("--nrpa-temp"); if(!v) return false;
			o.nrpa_temp = std::atof(v);
		} else if(a == "--max-subsets") {
			const char* v = next("--max-subsets"); if(!v) return false;
			const int n = std::atoi(v);
			if(n < 1 || n > 4096) {
				std::printf("!! --max-subsets attend 1..4096 "
											"(recu %s)\n", v);
				return false;
			}
			o.max_subsets = static_cast<uint32_t>(n);
		} else if(a == "--recipes") {
			const char* v = next("--recipes"); if(!v) return false;
			o.recipes = std::atof(v);
			if(o.recipes < 0) {
				std::printf("!! --recipes expects a weight >= "
											"0 (0 = fed and measured without "
											"entering the cost)\n");
				return false;
			}
		} else if(a == "--landmarks") {
			const char* v = next("--landmarks"); if(!v) return false;
			o.landmark_files.emplace_back(v);
		} else if(a == "--landmark-w") {
			const char* v = next("--landmark-w"); if(!v) return false;
			o.landmark_weight = std::atof(v);
			if(o.landmark_weight < 0) {
				std::printf("!! --landmark-w expects a weight "
											">= 0\n");
				return false;
			}
		} else if(a == "--landmark-h") {
			const char* v = next("--landmark-h"); if(!v) return false;
			o.landmark_h = std::atof(v);
			if(o.landmark_h < 0) {
				std::printf("!! --landmark-h expects a weight "
											">= 0\n");
				return false;
			}
		} else if(a == "--hindsight") {
			const char* v = next("--hindsight"); if(!v) return false;
			o.hindsight = std::atof(v);
			if(o.hindsight < 0) {
				std::printf("!! --hindsight expects a "
											"fraction of alpha >= 0\n");
				return false;
			}
		} else if(a == "--hindsight-k") {
			const char* v = next("--hindsight-k"); if(!v) return false;
			const long k = std::atol(v);
			if(k <= 0) {
				std::printf("!! --hindsight-k attend un "
											"entier > 0\n");
				return false;
			}
			o.hindsight_k = static_cast<size_t>(k);
		} else if(a == "--assign-bias") {
			const char* v = next("--assign-bias"); if(!v) return false;
			o.assign_bias = std::atof(v);
			if(o.assign_bias < 0) {
				std::printf("!! --assign-bias expects a "
											"weight >= 0\n");
				return false;
			}
		} else if(a == "--reenter") {
			// Return to the rung: probability of re-entry through a cell.
			const char* v = next("--reenter"); if(!v) return false;
			o.reenter = std::atof(v);
			if(o.reenter < 0.0 || o.reenter > 1.0) {
				std::printf("!! --reenter expects a "
											"probability in [0, 1]\n");
				return false;
			}
		} else if(a == "--phase-w") {
			// Weight SUBTRACTED from the logit of a phase change. This is not a
			// pruning: the choice stays drawable (rule 2), its mass drops.
			const char* v = next("--phase-w"); if(!v) return false;
			o.phase_w = std::atof(v);
		} else if(a == "--op-bias") {
			const char* v = next("--op-bias"); if(!v) return false;
			o.op_bias = std::atof(v);
			if(o.op_bias < 0) {
				std::printf("!! --op-bias expects a weight >= "
											"0\n");
				return false;
			}
		} else if(a == "--watch") {
			const char* v = next("--watch"); if(!v) return false;
			o.watch_specs.emplace_back(v);
		} else if(a == "--probe-repeat") {
			o.probe_repeat = true;
		} else if(a == "--no-seed-recipes") {
			o.seed_recipes = false;
		} else if(a == "--no-seed-quant") {
			o.seed_cardinal = false;
		} else if(a == "--derive-summon-min") {
			o.derive_summon_min = true;
		} else if(a == "--hint-bias") {
			const char* v = next("--hint-bias"); if(!v) return false;
			o.hint_bias = std::atof(v);
		} else if(a == "--nrpa-level") {
			const char* v = next("--nrpa-level"); if(!v) return false;
			o.nrpa_level = std::atoi(v);
			if(o.nrpa_level < 1 || o.nrpa_level > 4) {
				std::printf("!! --nrpa-level expects 1..4 "
											"(got %s); 2 costs ~576 rollouts "
											"per call, 3 costs ~13824\n", v);
				return false;
			}
		} else if(a == "--ctx-shrink") {
			const char* v = next("--ctx-shrink"); if(!v) return false;
			o.ctx_shrink = std::atof(v);
		} else if(a == "--qhat") {
			const char* v = next("--qhat"); if(!v) return false;
			o.qhat_depth = static_cast<uint32_t>(std::atoi(v));
		} else if(a == "--qhat-window") {
			const char* v = next("--qhat-window"); if(!v) return false;
			o.qhat_window = static_cast<uint32_t>(std::atoi(v));
		} else if(a == "--qhat-rho") {
			const char* v = next("--qhat-rho"); if(!v) return false;
			o.qhat_rho = static_cast<uint32_t>(std::atoi(v));
		} else if(a == "--qhat-nodes") {
			const char* v = next("--qhat-nodes"); if(!v) return false;
			o.qhat_nodes = static_cast<size_t>(std::atoll(v));
		} else if(a == "--no-qhat-probe") {
			o.qhat_probe = false;
		} else if(a == "--canonical-zones") {
			o.canonical_zones = true;
		} else if(a == "--target-subset") {
			o.target_subset = true;
		} else if(a == "--target-exact") {
			o.target_exact = true;
		} else if(a == "--ctx-max") {
			const char* v = next("--ctx-max"); if(!v) return false;
			o.ctx_max = static_cast<size_t>(std::atoll(v));
		} else if(a == "--tt-mb") {
			const char* v = next("--tt-mb"); if(!v) return false;
			o.tt_mb = static_cast<size_t>(std::atoi(v));
		} else if(a == "--summon") {
			const char* v = next("--summon"); if(!v) return false;
			o.summon_specs.emplace_back(v);
		} else if(a == "--guard") {
			const char* v = next("--guard"); if(!v) return false;
			o.guard_specs.emplace_back(v);
		} else if(a == "--no-activate") {
			const char* v = next("--no-activate"); if(!v) return false;
			o.no_activate_specs.emplace_back(v);
		} else if(a == "--no-chain") {
			const char* v = next("--no-chain"); if(!v) return false;
			o.no_chain_specs.emplace_back(v);
		} else if(a == "--guard-off") {
			const char* v = next("--guard-off"); if(!v) return false;
			o.guard_off_spec = v;
		} else if(a == "--resolve") {
			const char* v = next("--resolve"); if(!v) return false;
			o.resolve_specs.emplace_back(v);
		} else if(a == "--summon-min") {
			const char* v = next("--summon-min"); if(!v) return false;
			o.summon_min_specs.emplace_back(v);
		} else if(a == "--profile") {
			o.profile = true;
		} else if(a == "--verbose" || a == "-v") {
			o.verbose = true;
		} else if(a == "--help" || a == "-h") {
			o.help = true;
			return false;
		} else if(!a.empty() && a[0] == '-') {
			std::printf("!! unknown option: %s\n", a.c_str());
			return false;
		} else if(o.replay.empty()) {
			o.replay = a;
		} else {
			std::printf("!! extra argument: %s\n", a.c_str());
			return false;
		}
	}
	// NO path conditioning of the LOGIT belongs here: it wins 0 comparisons out of
	// 4 against `--ctx-shrink` alone, collapses at k = 6, and reaches the same
	// agreement plateau. The substantive reason: it grafts MCPS's conditioning onto
	// an NRPA LOGIT, where the paper conditions a REWARD AVERAGE. `--qhat` is what
	// implements the mechanism.
	if(o.workdir.empty()) {
		if(const char* env = std::getenv("COMBOSOLVER_WORKDIR"))
			o.workdir = env;
	}
	if(o.workdir.empty()) {
		std::printf("!! no EDOPro installation: pass --workdir <dir>, "
							"or set the COMBOSOLVER_WORKDIR environment "
							"variable\n");
		return false;
	}
	return !o.replay.empty();
}

// A card is given by code or by name fragment; a fragment that does not
// designate exactly one card is an error that lists the candidates. Guessing on
// the user's behalf would be worse than refusing.
bool ResolveCard(const std::string& item, const CardDB& db, const char* flag,
				 uint32_t& out) {
	char* end = nullptr;
	unsigned long code = std::strtoul(item.c_str(), &end, 10);
	if(end && *end == '\0' && code > 1000) {
		out = db.Canonical(static_cast<uint32_t>(code));
		return true;
	}
	auto matches = db.FindByName(item);
	if(matches.size() == 1) {
		out = matches[0].first;
		return true;
	}
	if(matches.empty()) {
		std::printf("!! %s: no card contains \"%s\"\n", flag, item.c_str());
	} else {
		std::printf("!! %s: \"%s\" is ambiguous (%zu cards):\n", flag,
					item.c_str(), matches.size());
		for(size_t i = 0; i < matches.size() && i < 8; ++i)
			std::printf("     %9u  %s\n", matches[i].first,
						matches[i].second.c_str());
		if(matches.size() > 8)
			std::printf("     ...\n");
	}
	return false;
}

std::string Trimmed(std::string s) {
	while(!s.empty() && s.front() == ' ') s.erase(s.begin());
	while(!s.empty() && s.back() == ' ') s.pop_back();
	return s;
}

std::vector<std::string> SplitOn(const std::string& s, char sep) {
	std::vector<std::string> out;
	size_t pos = 0;
	while(pos <= s.size()) {
		size_t at = s.find(sep, pos);
		out.push_back(Trimmed(at == std::string::npos
			? s.substr(pos) : s.substr(pos, at - pos)));
		pos = (at == std::string::npos) ? s.size() + 1 : at + 1;
	}
	return out;
}

// Zone names of the constraint grammar. The French spellings used by the
// earlier command lines are still accepted, so recorded scripts keep working.
bool ZoneMaskOf(const std::string& z, uint32_t& mask) {
	if(z == "hand" || z == "main")
		mask = LOCATION_HAND;
	else if(z == "field" || z == "terrain")
		mask = LOCATION_MZONE | LOCATION_SZONE;
	else if(z == "grave" || z == "graveyard" || z == "cimetiere")
		mask = LOCATION_GRAVE;
	else if(z == "banished" || z == "banni")
		mask = LOCATION_REMOVED;
	else if(z == "extra")
		mask = LOCATION_EXTRA;
	else return false;
	return true;
}

// Readable name of a zone mask (the inverse of ZoneMaskOf, for display).
std::string ZoneMaskName(uint32_t mask) {
	std::string s;
	auto add = [&](uint32_t m, const char* n) {
		if(mask & m) {
			if(!s.empty())
				s += "+";
			s += n;
		}
	};
	add(LOCATION_HAND, "hand");
	add(LOCATION_MZONE | LOCATION_SZONE, "field");
	add(LOCATION_GRAVE, "grave");
	add(LOCATION_REMOVED, "banished");
	add(LOCATION_EXTRA, "extra");
	return s;
}

// "card[@zone]" -> (canonical code, mask). Default zone: the field.
bool ResolveCardZone(const std::string& item, const CardDB& db, const char* flag,
					 uint32_t& code, uint32_t& zones) {
	size_t at = item.rfind('@');
	std::string card = (at == std::string::npos) ? item : item.substr(0, at);
	std::string zone = (at == std::string::npos) ? "field" : item.substr(at + 1);
	if(!ZoneMaskOf(Trimmed(zone), zones)) {
		std::printf("!! %s : unknown zone \"%s\" (hand field grave "
							"banished extra)\n", flag, zone.c_str());
		return false;
	}
	return ResolveCard(Trimmed(card), db, flag, code);
}

// Resolves every CLI constraint. Any error stops BEFORE the search.
bool ResolveConstraints(const Options& opt, const CardDB& db,
						LineConstraints& out) {
	for(const std::string& spec : opt.summon_specs) {
		size_t colon = spec.find(':');
		int n = (colon == std::string::npos)
			? 0 : std::atoi(spec.substr(0, colon).c_str());
		if(n <= 0) {
			std::printf("!! --summon \"%s\": expected form "
									"n:card[|card...], n >= 1\n", spec.c_str());
			return false;
		}
		std::vector<uint32_t> allowed;
		for(const std::string& item : SplitOn(spec.substr(colon + 1), '|')) {
			if(item.empty())
				continue;
			uint32_t code = 0;
			if(!ResolveCard(item, db, "--summon", code))
				return false;
			allowed.push_back(code);
		}
		if(allowed.empty()) {
			std::printf("!! --summon \"%s\": no card given\n", spec.c_str());
			return false;
		}
		auto& slot = out.summons[static_cast<uint32_t>(n)];
		slot.insert(slot.end(), allowed.begin(), allowed.end());
	}

	if(opt.guard_specs.size() > 1) {
		std::printf("!! --guard: one guard at a time\n");
		return false;
	}
	for(const std::string& spec : opt.guard_specs) {
		size_t colon = spec.find(':');
		int n = (colon == std::string::npos)
			? 0 : std::atoi(spec.substr(0, colon).c_str());
		if(n <= 0) {
			std::printf("!! --guard \"%s\" : forme attendue "
									"n:clause[|clause...]\n",
						spec.c_str());
			return false;
		}
		out.guard_after = static_cast<uint32_t>(n);
		for(const std::string& clause_s : SplitOn(spec.substr(colon + 1), '|')) {
			if(clause_s.empty())
				continue;
			GuardClause clause;
			for(const std::string& atom_s : SplitOn(clause_s, '+')) {
				if(atom_s.empty())
					continue;
				GuardAtom a;
				// PREDICATE atom: "oppbanished>=N", i.e. OPPONENT cards banished. A
				// game rule stated by the player: Dis Pater on the field + one
				// opponent card banished = an available negation, the same family as
				// Zalen/Crystal Wing.
				const std::string trimmed = Trimmed(atom_s);
				std::string pfx = "oppbanished>=";
				if(trimmed.rfind(pfx, 0) != 0)
					pfx = "bannieadv>=";        // accepted alias
				if(trimmed.rfind(pfx, 0) == 0) {
					char* end = nullptr;
					unsigned long v = std::strtoul(
						trimmed.c_str() + pfx.size(), &end, 10);
					if(!end || *end != '\0' || v == 0 || v > 0xffff) {
						std::printf("!! --guard: \"%s\" "
															"- expected form "
															"oppbanished>=N (N "
															">= 1)\n",
									trimmed.c_str());
						return false;
					}
					a.kind = 1;
					a.count = static_cast<uint32_t>(v);
					clause.push_back(a);
					continue;
				}
				if(!ResolveCardZone(atom_s, db, "--guard", a.code, a.zones))
					return false;
				clause.push_back(a);
			}
			if(!clause.empty())
				out.guard.push_back(std::move(clause));
		}
		if(out.guard.empty()) {
			std::printf("!! --guard \"%s\": no clause\n", spec.c_str());
			return false;
		}
	}

	for(const std::string& spec : opt.no_activate_specs) {
		uint32_t code = 0, zones = 0;
		if(!ResolveCardZone(spec, db, "--no-activate", code, zones))
			return false;
		out.no_activate[code] |= zones;
	}

	for(const std::string& spec : opt.no_chain_specs) {
		uint32_t code = 0;
		if(!ResolveCard(spec, db, "--no-chain", code))
			return false;
		if(std::find(out.no_chain.begin(), out.no_chain.end(), code) ==
		   out.no_chain.end())
			out.no_chain.push_back(code);
	}

	for(const std::string& spec : opt.resolve_specs) {
		if(out.resolve_min.size() >= 4) {
			std::printf("!! --resolve: at most 4 cards watched\n");
			return false;
		}
		// "card[:n]": the n is the suffix after the LAST ':' when it is numeric;
		// some names contain a ':' (Number 39: Utopia).
		std::string card = spec;
		uint32_t n = 1;
		size_t colon = spec.rfind(':');
		if(colon != std::string::npos) {
			const std::string tail = Trimmed(spec.substr(colon + 1));
			char* end = nullptr;
			unsigned long v = std::strtoul(tail.c_str(), &end, 10);
			if(end && *end == '\0' && v > 0 && v < 0xffff) {
				n = static_cast<uint32_t>(v);
				card = spec.substr(0, colon);
			}
		}
		// Optional activation zone: "card[@zone]". Default: every zone (the previous
		// behaviour); the filter is asked for explicitly.
		ResolveReq req;
		req.min_count = n;
		size_t at = card.rfind('@');
		if(at != std::string::npos) {
			if(!ZoneMaskOf(Trimmed(card.substr(at + 1)), req.zones)) {
				std::printf("!! --resolve: unknown zone "
											"\"%s\" (hand field grave "
											"banished extra)\n",
							card.substr(at + 1).c_str());
				return false;
			}
			card = card.substr(0, at);
		}
		if(!ResolveCard(Trimmed(card), db, "--resolve", req.code))
			return false;
		out.resolve_min.push_back(req);
	}

	for(const std::string& spec : opt.summon_min_specs) {
		if(out.resolve_min.size() >= 4) {
			std::printf("!! --resolve/--summon-min: at most 4 "
									"cards watched\n");
			return false;
		}
		std::string card = spec;
		uint32_t n = 1;
		size_t colon = spec.rfind(':');
		if(colon != std::string::npos) {
			const std::string tail = Trimmed(spec.substr(colon + 1));
			char* end = nullptr;
			unsigned long v = std::strtoul(tail.c_str(), &end, 10);
			if(end && *end == '\0' && v > 0 && v < 0xffff) {
				n = static_cast<uint32_t>(v);
				card = spec.substr(0, colon);
			}
		}
		ResolveReq req;
		req.min_count = n;
		req.on_summon = true;
		if(!ResolveCard(Trimmed(card), db, "--summon-min", req.code))
			return false;
		out.resolve_min.push_back(req);
	}

	for(const std::string& spec : opt.hint_specs) {
		uint32_t code = 0;
		if(!ResolveCard(Trimmed(spec), db, "--hint", code))
			return false;
		out.hints.push_back(code);
	}

	for(const std::string& spec : opt.guard_keep_specs) {
		uint32_t code = 0;
		if(!ResolveCard(Trimmed(spec), db, "--guard-keep", code))
			return false;
		out.guard_keep.push_back(db.Canonical(code));
	}

	for(const std::string& spec : opt.opp_hand_specs) {
		for(const std::string& item : SplitOn(spec, '|')) {
			if(item.empty())
				continue;
			uint32_t code = 0;
			if(!ResolveCard(Trimmed(item), db, "--opp-hand", code))
				return false;
			out.opp_hand.push_back(code);
		}
	}

	// --board-add and --target share the "card[@ATK|DEF]" grammar: the first ADDS
	// to the reference's capture, the second BUILDS the target from scratch. One
	// parser, so they never diverge.
	auto parse_board_card = [&](const std::string& spec, const char* flag) -> bool {
		size_t at = spec.rfind('@');
		std::string card = (at == std::string::npos) ? spec : spec.substr(0, at);
		std::string pos = (at == std::string::npos)
			? "ATK" : Trimmed(spec.substr(at + 1));
		uint32_t position = 0;
		if(pos == "ATK")      position = POS_FACEUP_ATTACK;
		else if(pos == "DEF") position = POS_FACEUP_DEFENSE;
		else {
			std::printf("!! %s: unknown position \"%s\" (ATK or "
									"DEF)\n",
						flag, pos.c_str());
			return false;
		}
		uint32_t code = 0;
		if(!ResolveCard(Trimmed(card), db, flag, code))
			return false;
		out.board_add.emplace_back(code, position);
		return true;
	};
	for(const std::string& spec : opt.board_add_specs)
		if(!parse_board_card(spec, "--board-add"))
			return false;
	if(!opt.target_specs.empty()) {
		out.target_scratch = true;
		for(const std::string& spec : opt.target_specs)
			if(!parse_board_card(spec, "--target"))
				return false;
	}
	for(const std::string& spec : opt.board_remove_specs) {
		uint32_t code = 0;
		if(!ResolveCard(Trimmed(spec), db, "--board-remove", code))
			return false;
		out.board_remove.push_back(code);
	}

	for(const std::string& spec : opt.material_specs) {
		size_t colon = spec.rfind(':');
		if(colon == std::string::npos) {
			std::printf("!! --material \"%s\": expected form "
									"card:attr[,attr]\n",
						spec.c_str());
			return false;
		}
		uint32_t mask = 0;
		bool attrs_ok = true;
		for(const std::string& a : SplitOn(spec.substr(colon + 1), ',')) {
			if(a.empty())
				continue;
			if(a == "light" || a == "lumiere")        mask |= ATTRIBUTE_LIGHT;
			else if(a == "dark" || a == "tenebres")   mask |= ATTRIBUTE_DARK;
			else if(a == "earth" || a == "terre")     mask |= ATTRIBUTE_EARTH;
			else if(a == "water" || a == "eau")       mask |= ATTRIBUTE_WATER;
			else if(a == "fire" || a == "feu")        mask |= ATTRIBUTE_FIRE;
			else if(a == "wind" || a == "vent")       mask |= ATTRIBUTE_WIND;
			else if(a == "divine" || a == "divin")    mask |= ATTRIBUTE_DIVINE;
			else { attrs_ok = false; break; }
		}
		if(!attrs_ok || !mask) {
			std::printf("!! --material \"%s\": attributes "
									"expected after ':' (light dark earth "
									"water fire wind divine)\n",
						spec.c_str());
			return false;
		}
		uint32_t code = 0;
		if(!ResolveCard(Trimmed(spec.substr(0, colon)), db, "--material", code))
			return false;
		out.material_req.emplace_back(code, mask);
	}

	if(!opt.guard_off_spec.empty()) {
		const std::string s = Trimmed(opt.guard_off_spec);
		const char* prefix = "opphand<=";
		size_t plen = 9;
		if(s.compare(0, plen, prefix) != 0) {
			prefix = "mainadv<=";        // accepted alias
			if(s.compare(0, plen, prefix) != 0) {
				std::printf("!! --guard-off \"%s\": expected "
											"form opphand<=N\n",
							s.c_str());
				return false;
			}
		}
		out.guard_opp_hand_release = std::atoi(s.c_str() + plen);
		if(out.guard.empty()) {
			std::printf("!! --guard-off without --guard: nothing "
									"to turn off\n");
			return false;
		}
	}
	return true;
}

const char* PhaseName(uint32_t p) {
	switch(p) {
	case PHASE_DRAW:    return "DRAW";
	case PHASE_STANDBY: return "STANDBY";
	case PHASE_MAIN1:   return "MAIN1";
	case PHASE_BATTLE:  return "BATTLE";
	case PHASE_MAIN2:   return "MAIN2";
	case PHASE_END:     return "END";
	default:            return "?";
	}
}

const char* PosName(uint32_t p) {
	switch(p) {
	case POS_FACEUP_ATTACK:    return "ATK";
	case POS_FACEDOWN_ATTACK:  return "FD-ATK";
	case POS_FACEUP_DEFENSE:   return "DEF";
	case POS_FACEDOWN_DEFENSE: return "FD-DEF";
	case POS_FACEUP:           return "FACEUP";
	case POS_FACEDOWN:         return "FACEDOWN";
	default:                   return "?";
	}
}

constexpr uint32_t kBoardFlags = QUERY_CODE | QUERY_ALIAS | QUERY_POSITION |
								 QUERY_TYPE | QUERY_LEVEL | QUERY_ATTACK |
								 QUERY_DEFENSE | QUERY_OVERLAY_CARD |
								 QUERY_COUNTERS | QUERY_LINK;

// State of a player's zones in the sense of the equivalence criterion we use:
// the field with positions and materials, counts for the hidden zones.
//
// The contents of the hand, the graveyard and the banished zone do NOT enter
// the equivalence, but they say which cards the line consumed, which is the
// only way to know whether another deck can hope to rebuild the same board.
struct Board {
	std::vector<QueriedCard> mzone, szone;
	std::vector<QueriedCard> hand_cards, grave_cards, removed_cards;
	uint32_t hand{}, deck{}, extra{}, grave{}, removed{};
};

Board Snapshot(Duel& duel, uint8_t con) {
	Board b;
	b.mzone = duel.Query(con, LOCATION_MZONE, kBoardFlags);
	b.szone = duel.Query(con, LOCATION_SZONE, kBoardFlags);
	b.hand_cards = duel.Query(con, LOCATION_HAND, kBoardFlags);
	b.grave_cards = duel.Query(con, LOCATION_GRAVE, kBoardFlags);
	b.removed_cards = duel.Query(con, LOCATION_REMOVED, kBoardFlags);
	b.hand = duel.Count(con, LOCATION_HAND);
	b.deck = duel.Count(con, LOCATION_DECK);
	b.extra = duel.Count(con, LOCATION_EXTRA);
	b.grave = duel.Count(con, LOCATION_GRAVE);
	b.removed = duel.Count(con, LOCATION_REMOVED);
	return b;
}

// Multiset of the PHYSICAL cards of a zone.
//
// Deliberately `c.code` and not `c.Code()`: the latter goes through get_code(),
// which returns the EFFECTIVE name, so a monster whose name an effect changed
// would appear there as the card it imitates. That is right for comparing two
// fields, not for counting what a deck must contain. Only the artwork alias is
// resolved, because two artworks really are the same copy.
std::map<uint32_t, uint32_t> CodeCounts(const std::vector<QueriedCard>& zone,
										const CardDB& db) {
	std::map<uint32_t, uint32_t> out;
	for(const auto& c : zone)
		if(c.present)
			++out[db.Canonical(c.code)];
	return out;
}

void PrintBoard(const Board& b, const CardDB& db) {
	auto dump = [&](const char* label, const std::vector<QueriedCard>& zone) {
		for(size_t i = 0; i < zone.size(); ++i) {
			const auto& c = zone[i];
			if(!c.present)
				continue;
			std::string extra;
			if(!c.overlay.empty())
				extra += " +" + std::to_string(c.overlay.size()) + " mat";
			if(!c.counters.empty())
				extra += " +compteurs";
			std::printf("      %s[%zu] %9u  %-36.36s %-8s%s\n", label, i, c.Code(),
						db.Name(c.Code()).c_str(), PosName(c.position), extra.c_str());
		}
	};
	dump("MZONE", b.mzone);
	dump("SZONE", b.szone);
	std::printf("      HAND=%u  DECK=%u  EXTRA=%u  GRAVE=%u  REMOVED=%u\n",
				b.hand, b.deck, b.extra, b.grave, b.removed);
}

size_t CountPresent(const std::vector<QueriedCard>& v) {
	return static_cast<size_t>(
		std::count_if(v.begin(), v.end(), [](const QueriedCard& c) { return c.present; }));
}

// Complete fingerprint of a visible state, fine enough to detect an unfaithful
// restore. It is not yet the solver's transposition digest: that one must also
// cover the processor state.
uint64_t Fingerprint(Duel& duel) {
	uint64_t h = 1469598103934665603ull;
	auto mix = [&h](uint64_t v) { h ^= v; h *= 1099511628211ull; };
	constexpr uint32_t flags = kBoardFlags | QUERY_STATUS;
	for(uint8_t con = 0; con < 2; ++con) {
		for(uint32_t loc : { LOCATION_MZONE, LOCATION_SZONE, LOCATION_HAND,
							 LOCATION_GRAVE, LOCATION_REMOVED, LOCATION_EXTRA,
							 LOCATION_DECK }) {
			mix(loc * 0x9e3779b97f4a7c15ull + con);
			for(const auto& c : duel.Query(con, loc, flags)) {
				if(!c.present) { mix(0); continue; }
				mix(c.code);
				mix(c.position);
				mix((uint64_t(uint32_t(c.attack)) << 32) ^ uint32_t(c.defense));
				mix(c.status);
				mix((uint64_t(c.link) << 32) ^ c.link_marker);
				for(uint32_t o : c.overlay) mix(o * 31ull);
				for(uint32_t k : c.counters) mix(k * 37ull);
			}
		}
	}
	return h;
}

struct Stat {
	int n = 0;
	long double raw_sum = 0, dedup_sum = 0;
	long double raw_max = 0, dedup_max = 0;
	int undecoded = 0;
	int forced = 0;   // a single legal answer: a candidate for elision
	int binary = 0;
};

struct LineResult {
	size_t responses_used = 0;
	size_t retries = 0;
	int turns = 0;
	long long summon = 0, spsummon = 0, flipsummon = 0, chaining = 0;
	std::map<uint8_t, Stat> stats;
	long double log_raw = 0, log_dedup = 0;
	bool have_target = false;
	// The target was taken at the END of the recording rather than at a turn
	// change: the line does not cross the end of the turn (a hand test stopped once
	// the combo is placed). Worth signalling, because the checks that assume a
	// complete turn (turn cut-off, burned peak) read differently.
	bool target_at_is_end = false;
	Board target_self, target_oppo;
	size_t target_at = 0;
	// Starting position, captured at the very first decision point: it is what says
	// whether a deck even has enough to start.
	bool have_start = false;
	Board start_self;
	// Sequence of the summons (normal + special, Nibiru's order), and how many had
	// happened when the target board was captured. It is what the --summon
	// constraints judge the reference against.
	std::vector<uint32_t> summon_codes;
	size_t summons_at_target = 0;
	// Verdict of the reference against --guard and --no-activate, evaluated as the
	// instrumented replay goes (the opponent windows and the recorded answers
	// cannot be reconstructed after the fact).
	size_t guard_checks = 0, guard_violations = 0;
	size_t first_guard_violation_summon = 0;
	uint32_t first_guard_violation_opp_hand = 0;
	size_t forbidden_activations = 0;
	// Resolutions of the watched cards (--resolve), aligned with
	// cons->resolve_min, counted up to the board.
	std::vector<size_t> resolve_counts;
	// Burned cards (graveyard + banished): peak mid-line and count at the board.
	// The gap between the two MEASURES the recovery slack, and it is what
	// calibrates --burn-slack (burned cards are not monotonic).
	uint32_t burned_max = 0;
	uint32_t burned_at_target = 0;
	uint64_t fingerprint_at_target = 0;
	uint64_t fingerprint_final = 0;
	double ms = 0;
	// Pages dirtied between two consecutive decisions. That is THE granularity that
	// matters: the solver branches at every decision, not at every action, so that
	// is the rate at which it will pay for a snapshot.
	std::vector<size_t> dirty_per_decision;
	double ms_write_watch = 0;   // cumulated cost of the GetWriteWatch calls
	size_t write_watch_calls = 0;
	// Activations recorded in the line (--operators). Empty otherwise: the harness
	// must cost the ordinary replay nothing.
	std::vector<ObservedActivation> activations;
};

// Plays back the recorded answers. `instrument` enables the full collection; a
// second verification pass does not need it. `cons` (optional) makes the
// reference be judged against --guard and --no-activate during the replay.
LineResult RunLine(Duel& duel, const Replay& yrp, const Options& opt,
				   bool instrument, const LineConstraints* cons = nullptr) {
	LineResult r;
	auto t0 = Clock::now();
	Arena* arena = duel.GetArena();
	uint32_t phase = 0;
	bool first_idle_seen = false;
	// Tracking of the current prompt, only when constraints are to be judged, or
	// when the operator harness is recording the activations.
	const bool track = cons && cons->Any();
	const bool need_prompt = track || opt.operators;
	uint8_t ptype = 0;
	int pplayer = -1;
	std::vector<uint8_t> ppayload;
	EnumOptions peo;
	if(need_prompt) {
		if(track)
			peo.no_activate =
				cons->no_activate.empty() ? nullptr : &cons->no_activate;
		peo.db = &duel.Db();
	}

	// Capture of the TARGET BOARD. Two possible instants, and it is the second that
	// was missing: (1) the turn change, when the recorded line crosses it; (2) THE
	// END OF THE RECORDING. A hand test that stops once the combo is placed is a
	// perfectly valid replay (it is even the normal way to record a line), and
	// requiring it to pass its turn was an assumption of this tool, not a property
	// of replays. Without that fallback, such a replay returned no target, hence no
	// plan, and the transplantation flow refused to start.
	auto capture_target = [&] {
		r.target_self = Snapshot(duel, uint8_t(opt.target_player));
		r.target_oppo = Snapshot(duel, uint8_t(1 - opt.target_player));
		r.target_at = r.responses_used;
		r.fingerprint_at_target = Fingerprint(duel);
		r.summons_at_target = r.summon_codes.size();
		r.burned_at_target =
			duel.Count(uint8_t(opt.target_player), LOCATION_GRAVE) +
			duel.Count(uint8_t(opt.target_player), LOCATION_REMOVED);
		r.have_target = true;
	};

	if(instrument && arena)
		arena->ResetDirtyTracking();

	for(;;) {
		int status = duel.Process();
		for(const Message& m : duel.Messages()) {
			switch(m.type) {
			case MSG_NEW_TURN:
				++r.turns;
				// End of the target player's turn: the instant at which the target board is
				// defined.
				if(r.turns == 2 && !r.have_target)
					capture_target();
				break;
			case MSG_NEW_PHASE:
				if(m.size >= 2) { uint16_t p = 0; std::memcpy(&p, m.data, 2); phase = p; }
				break;
			case MSG_SUMMONING:
			case MSG_SPSUMMONING:
				if(m.type == MSG_SUMMONING)
					++r.summon;
				else
					++r.spsummon;
				if(m.size >= 4) {
					uint32_t c = 0;
					std::memcpy(&c, m.data, 4);
					r.summon_codes.push_back(c);
					// Watched summons (--summon-min), counted up to the board like the
					// resolutions.
					if(track && !cons->resolve_min.empty() && !r.have_target &&
					   c) {
						const uint32_t sc = duel.Db().Canonical(c);
						r.resolve_counts.resize(cons->resolve_min.size(), 0);
						for(size_t i = 0; i < cons->resolve_min.size(); ++i)
							if(cons->resolve_min[i].on_summon &&
							   cons->resolve_min[i].code == sc)
								++r.resolve_counts[i];
					}
				}
				break;
			case MSG_FLIPSUMMONING: ++r.flipsummon; break;
			case MSG_CHAINING:
				++r.chaining;
				// The sequence of chains, readable: it is what says WHO answers WHAT
				// (the --fire opponent test produces replays where "who negated
				// Nibiru?" is read here).
				if(opt.verbose && m.size >= 4) {
					uint32_t vc = 0;
					std::memcpy(&vc, m.data, 4);
					std::printf("      [chain %lld] %s  "
													"(activation %s)\n",
								r.chaining,
								duel.Db().Name(duel.Db().Canonical(vc)).c_str(),
								ZoneMaskName(ChainingLocation(m.data, m.size))
									.c_str());
				}
				if(track && !cons->resolve_min.empty() && !r.have_target &&
				   m.size >= 4) {
					uint32_t c = 0;
					std::memcpy(&c, m.data, 4);
					c = duel.Db().Canonical(c);
					const uint32_t loc = ChainingLocation(m.data, m.size);
					r.resolve_counts.resize(cons->resolve_min.size(), 0);
					for(size_t i = 0; i < cons->resolve_min.size(); ++i)
						if(!cons->resolve_min[i].on_summon &&
						   cons->resolve_min[i].code == c &&
						   (!cons->resolve_min[i].zones ||
							(loc & cons->resolve_min[i].zones)))
							++r.resolve_counts[i];
				}
				break;
			case MSG_RETRY:         ++r.retries; break;
			default: break;
			}

			if(need_prompt && IsPrompt(m.type)) {
				ptype = m.type;
				pplayer = m.size ? m.data[0] : -1;
				ppayload.assign(m.data, m.data + m.size);
			}
			if(!instrument || !IsPrompt(m.type))
				continue;
			PromptInfo info = DecodePrompt(m.type, m.data, m.size);
			Stat& s = r.stats[m.type];
			++s.n;
			if(info.raw < 0) {
				++s.undecoded;
			} else {
				if(info.dedup <= 1) ++s.forced;
				else if(info.dedup <= 2) ++s.binary;
				s.raw_sum += info.raw;
				s.dedup_sum += info.dedup;
				s.raw_max = (std::max)(s.raw_max, info.raw);
				s.dedup_max = (std::max)(s.dedup_max, info.dedup);
				if(info.raw > 0) r.log_raw += std::log10(double(info.raw));
				if(info.dedup > 0) r.log_dedup += std::log10(double(info.dedup));
			}
			if(opt.verbose)
				std::printf("  #%-4zu T%d %-8s %-22s "
											"raw=%-10.0Lf dedup=%-8.0Lf %s\n",
							r.responses_used, r.turns, PhaseName(phase),
							PromptName(m.type), info.raw, info.dedup,
							info.detail.c_str());
		}

		if(status == OCG_DUEL_STATUS_AWAITING) {
			if(!r.have_start) {
				r.start_self = Snapshot(duel, uint8_t(opt.target_player));
				r.have_start = true;
			}
			// Peak of burned cards mid-line, up to the board: the measurement that
			// calibrates the slack of the B&B bound (--burn-slack).
			if(!r.have_target) {
				const uint32_t b =
					duel.Count(uint8_t(opt.target_player), LOCATION_GRAVE) +
					duel.Count(uint8_t(opt.target_player), LOCATION_REMOVED);
				if(b > r.burned_max)
					r.burned_max = b;
			}
			// Judgement of the reference against --guard (at the opponent windows,
			// where Nibiru would land) and --no-activate (the recorded answer).
			if(track && !r.have_target) {
				if(!cons->guard.empty() &&
				   pplayer == 1 - opt.target_player &&
				   r.summon_codes.size() >= cons->guard_after) {
					uint32_t opp_hand = duel.Count(
						static_cast<uint8_t>(1 - opt.target_player),
						LOCATION_HAND);
					// Threat gone (handrip done): the window is moot.
					bool threat = cons->guard_opp_hand_release < 0 ||
								  static_cast<int>(opp_hand) >
									  cons->guard_opp_hand_release;
					if(threat) {
						++r.guard_checks;
						BoardKey fk = ComputeBoardKey(
							duel, static_cast<uint8_t>(opt.target_player));
						if(!GuardHolds(duel,
									   static_cast<uint8_t>(opt.target_player),
									   cons->guard, fk.codes)) {
							if(!r.guard_violations) {
								r.first_guard_violation_summon =
									r.summon_codes.size();
								r.first_guard_violation_opp_hand = opp_hand;
							}
							++r.guard_violations;
						}
					}
				}
				if(peo.no_activate && r.responses_used < yrp.responses.size() &&
				   ResponseForbidden(ptype, ppayload.data(),
									 static_cast<uint32_t>(ppayload.size()),
									 yrp.responses[r.responses_used], peo))
					++r.forbidden_activations;
			}
			if(instrument && arena) {
				// Pages dirtied to advance by ONE decision: that is what an
				// incremental snapshot per explored node would cost.
				auto t = Clock::now();
				size_t pages = arena->CountDirtyPages();
				r.ms_write_watch += MsSince(t);
				++r.write_watch_calls;
				if(first_idle_seen)   // the initial setup is ignored
					r.dirty_per_decision.push_back(pages);
				first_idle_seen = true;
			}
			if(r.responses_used >= yrp.responses.size())
				break;   // end of the recording: the player left
			// OPERATOR HARNESS: what the RECORDED answer activates. Only the
			// target player's decisions count: an opponent's operator would not be
			// in the table (which is built on OUR deck) and would wrongly count as
			// "unmatched".
			if(opt.operators && pplayer == opt.target_player) {
				ActivationRead ar;
				if(DecodeActivation(ptype, ppayload.data(),
									static_cast<uint32_t>(ppayload.size()),
									yrp.responses[r.responses_used], peo, ar)) {
					ObservedActivation oa;
					oa.at = r.responses_used;
					oa.message = ptype;
					oa.code = ar.code;
					oa.desc = ar.desc;
					oa.location = ar.has_zone ? ar.location : uint8_t(0);
					oa.sequence = ar.sequence;
					oa.turn = r.turns;
					r.activations.push_back(oa);
				}
			}
			duel.SetResponse(yrp.responses[r.responses_used]);
			++r.responses_used;
		} else if(status == OCG_DUEL_STATUS_END) {
			break;
		} else if(status != OCG_DUEL_STATUS_CONTINUE) {
			std::printf("\n!! unexpected duel status: %d\n", status);
			break;
		}
	}
	// Fallback: the line stops with no turn change (the player left once the board
	// was placed). The final state IS the target board.
	if(!r.have_target && r.responses_used) {
		capture_target();
		r.target_at_is_end = true;
	}
	r.fingerprint_final = Fingerprint(duel);
	r.ms = MsSince(t0);
	return r;
}

void ReportLine(const LineResult& r, const Replay& yrp, const CardDB& db,
				const Options& opt) {
	std::printf("\nresults\n");
	std::printf("  answers consumed    : %zu / %zu\n", r.responses_used,
				yrp.responses.size());
	std::printf("  MSG_RETRY           : %zu   %s\n", r.retries,
				r.retries ? "<-- DIVERGENT REPLAY, "
											"measurements invalid"
						  : "(faithful replay)");
	std::printf("  turns played        : %d\n", r.turns);
	if(r.have_target)
		std::printf("  burned              : %u at the board, peak %u "
							"along the line (recovery margin %d - calibrates "
							"--burn-slack)\n",
					r.burned_at_target, r.burned_max,
					static_cast<int>(r.burned_max) -
						static_cast<int>(r.burned_at_target));
	else if(r.burned_max)
		// A solution line stops in turn 1 without capturing a target: its peak
		// is still the datum that calibrates --burn-slack (the slack must cover
		// the recovery of the BEST line, not only the reference's).
		std::printf("  burned              : peak %u along the line "
							"(no target board captured)\n", r.burned_max);

	int total = 0, forced = 0, binary = 0, decoded = 0;
	for(const auto& [type, s] : r.stats) {
		total += s.n;
		forced += s.forced;
		binary += s.binary;
		decoded += s.n - s.undecoded;
	}
	std::vector<std::pair<uint8_t, Stat>> ordered(r.stats.begin(), r.stats.end());
	std::sort(ordered.begin(), ordered.end(),
			  [](const auto& a, const auto& b) { return a.second.n > b.second.n; });
	// Per-type breakdown and elision potential: they measure the SOLVER's input,
	// not the duel. The scale of the problem stays below, at the default level.
	if(g_verbose) {
		std::printf("\n--- decision points by type ---\n");
		std::printf("  %-24s%6s%12s%11s%12s%11s\n", "type", "n", "raw avg",
					"raw max", "dedup avg", "dedup max");
		for(const auto& [type, s] : ordered) {
			const int d = s.n - s.undecoded;
			if(d > 0)
				std::printf("  %-24s%6d%12.1Lf%11.0Lf%11.1Lf%1"
											"1.0Lf\n",
							PromptName(type), s.n, s.raw_sum / d, s.raw_max,
							s.dedup_sum / d, s.dedup_max);
			else
				std::printf("  %-24s%6d%12s\n", PromptName(type), s.n,
							"not decoded");
		}
		std::printf("  %-24s%6d\n", "TOTAL", total);

		std::printf("\n--- elision potential (a single legal answer) "
							"---\n");
		for(const auto& [type, s] : ordered) {
			const int d = s.n - s.undecoded;
			if(d > 0 && s.forced)
				std::printf("  %-24s %4d / %-4d forced  "
											"(%.0f%%)\n",
							PromptName(type), s.forced, d, 100.0 * s.forced / d);
		}
		std::printf("  %-24s %4d / %-4d forced  (%.0f%%)\n", "TOTAL", forced,
					decoded, decoded ? 100.0 * forced / decoded : 0.0);
		std::printf("  => depth after elision : %d instead of %d\n",
					decoded - forced, decoded);
	}

	std::printf("\n--- possible lines along THIS line ---\n");
	std::printf("  raw branching product          : 10^%.1Lf\n", r.log_raw);
	std::printf("  after dedup by code            : 10^%.1Lf\n", r.log_dedup);
	std::printf("  (scale indicator, NOT a leaf count: changing an early\n"
					"   choice changes the prompts that follow.)\n");

	if(r.have_start) {
		std::printf("\n--- starting position (player %d) ---\n", opt.target_player);
		for(const auto& c : r.start_self.hand_cards)
			if(c.present)
				std::printf("      MAIN      %9u  %s\n", c.Code(),
							db.Name(c.Code()).c_str());
		std::printf("      DECK=%u  EXTRA=%u\n", r.start_self.deck,
					r.start_self.extra);
	}

	std::printf("\n--- cost of the reference line ---\n");
	std::printf("  normal summons         : %lld\n", r.summon);
	std::printf("  special summons        : %lld\n", r.spsummon);
	std::printf("  flip summons           : %lld\n", r.flipsummon);
	std::printf("  activations            : %lld\n", r.chaining);
	std::printf("  A_ref (tier 2)         : %lld\n",
				r.summon + r.spsummon + r.flipsummon + r.chaining);
	std::printf("  D_ref (tier 3)         : %zu\n", r.responses_used);

	if(r.have_target) {
		const Deck& deck = yrp.decks[opt.target_player];
		size_t owned = deck.main.size() + deck.extra.size();
		size_t left = r.target_self.hand + r.target_self.deck + r.target_self.extra;
		size_t on_board = CountPresent(r.target_self.mzone) +
						  CountPresent(r.target_self.szone);
		size_t burned = r.target_self.grave + r.target_self.removed;
		std::printf("\n--- target board (%s, player %d) ---\n",
					r.target_at_is_end
						? "END OF THE RECORDING: the "
												"line does not get through "
												"the turn"
						: "end of turn",
					opt.target_player);
		std::printf("    captured after answer #%zu\n", r.target_at);
		PrintBoard(r.target_self, db);
		std::printf("\n  C_ref (tier 1) = cards outside "
							"hand/deck/extra\n");
		std::printf("    player %d total        : %zu\n", opt.target_player, owned);
		std::printf("    left in hand+deck+extra: %zu\n", left);
		std::printf("    => consommees         : %zu\n", owned - left);
		std::printf("       of which on the board : %zu  (constant: "
							"fixed by the equivalence criterion)\n", on_board);
		std::printf("       of which burned to GY/banished: %zu  <-- "
							"THIS is what the solver minimises\n", burned);

		// Itemised list of the cards consumed. Without it one cannot say whether
		// ANOTHER deck has what it takes to rebuild this board: only the detail allows
		// the line's spending to be confronted with a different deck's contents.
		std::map<uint32_t, uint32_t> spent;
		for(const auto* zone : { &r.target_self.grave_cards,
								 &r.target_self.removed_cards,
								 &r.target_self.mzone, &r.target_self.szone })
			for(const auto& [code, n] : CodeCounts(*zone, db))
				spent[code] += n;
		std::printf("\n--- cards the line engages (board + GY + "
							"banished) ---\n");
		for(const auto& [code, n] : spent)
			std::printf("      %dx %9u  %s\n", n, code, db.Name(code).c_str());
	} else {
		std::printf("\n!! the target player's turn did not end\n");
	}
}

void ReportDirty(const LineResult& r, const Arena& arena, double ms_per_decision) {
	if(r.dirty_per_decision.empty())
		return;
	std::vector<size_t> v = r.dirty_per_decision;
	std::sort(v.begin(), v.end());
	size_t sum = 0;
	for(size_t x : v) sum += x;
	size_t page = arena.PageSize();
	auto pct = [&](double q) { return v[(std::min)(v.size() - 1,
												   size_t(q * v.size()))]; };
	double avg = double(sum) / v.size();
	auto kb = [&](double pages) { return pages * page / 1024.0; };
	std::printf("\n--- pages dirtied to advance ONE decision (%zu "
					"measurements) ---\n",
				v.size());
	std::printf("  mediane : %6zu pages  (%7.0f Ko)\n", pct(0.5), kb(double(pct(0.5))));
	std::printf("  moyenne : %6.0f pages  (%7.0f Ko)\n", avg, kb(avg));
	std::printf("  p90     : %6zu pages  (%7.0f Ko)\n", pct(0.9), kb(double(pct(0.9))));
	std::printf("  max     : %6zu pages  (%7.0f Ko)\n", v.back(), kb(double(v.back())));

	// An incremental snapshot copies the dirty page twice (log + mirror) and a
	// restore once, at ~10 GB/s of memory bandwidth.
	double bytes_push = 2.0 * avg * page, bytes_pop = avg * page;
	double ms_push = bytes_push / 10e9 * 1000.0, ms_pop = bytes_pop / 10e9 * 1000.0;
	std::printf("\n  projection of an incremental snapshot, per node "
					"explored:\n");
	std::printf("    copy on push           : %7.0f KB -> %.3f ms\n",
				bytes_push / 1024.0, ms_push);
	std::printf("    copy on restore        : %6.0f KB -> %.3f ms\n",
				bytes_pop / 1024.0, ms_pop);
	if(r.write_watch_calls) {
		double ww = r.ms_write_watch / r.write_watch_calls;
		std::printf("    GetWriteWatch measured : %.3f ms per call, 2 "
							"calls/node\n", ww);
		double total = ms_push + ms_pop + 2 * ww;
		std::printf("    snapshot total         : %.3f ms  against "
							"%.3f ms of work  => %.0f%% overhead\n", total, ms_per_decision,
					ms_per_decision > 0 ? 100.0 * total / ms_per_decision : 0.0);
	}
}

// Advances the duel by exactly `n` decisions from answer `from`. Returns the
// number of decisions actually consumed.
// `chain_codes`: receives the RAW code of every activation (MSG_CHAINING) met,
// the same observation as quota_uses in the search, so that the theorem 2 walk
// counts the path quotas with the run's own bookkeeping.
size_t Advance(Duel& duel, const Replay& yrp, size_t from, size_t n,
			   uint32_t* actions = nullptr,
			   std::vector<uint32_t>* chain_codes = nullptr) {
	size_t used = 0;
	while(used < n) {
		int status = duel.Process();
		for(const Message& m : duel.Messages()) {
			if(actions && (m.type == MSG_SUMMONING || m.type == MSG_SPSUMMONING ||
						   m.type == MSG_FLIPSUMMONING || m.type == MSG_CHAINING))
				++*actions;
			if(chain_codes && m.type == MSG_CHAINING && m.size >= 4) {
				uint32_t cc = 0;
				std::memcpy(&cc, m.data, 4);
				chain_codes->push_back(cc);
			}
		}
		if(status == OCG_DUEL_STATUS_AWAITING) {
			if(from + used >= yrp.responses.size())
				break;
			duel.SetResponse(yrp.responses[from + used]);
			++used;
		} else if(status == OCG_DUEL_STATUS_END) {
			break;
		}
	}
	return used;
}

// EFFECTIVE WIDTH MEASUREMENT: the prerequisite to any novelty pruning.
//
// Iterated Width only keeps a state when it makes an unseen fact true. Before
// pruning anything, one has to know whether the REFERENCE LINE itself would
// survive: chain resolutions go through "mute" states that change nothing on
// the board, and cutting them at the first silence would kill the only known
// solution. So we measure, decision by decision, whether the state produces a
// new atom, and the longest mute run; that run is what sets the pruning's
// patience. A width that is not measured is not promised.
uint32_t MeasureWidth(Duel& duel, const Replay& yrp, const Options& opt,
					  Arena& arena, const LineResult& ref) {
	if(g_verbose)
		std::printf("\n--- effective width (IW atoms) along the "
							"reference ---\n");
	while(arena.Depth() > 1)
		arena.Pop();
	if(arena.Depth() == 0)
		arena.Push();
	arena.Restore();

	const auto con = static_cast<uint8_t>(opt.target_player);
	BoardKey target;
	{
		size_t at = 0;
		at += Advance(duel, yrp, at, ref.target_at);
		target = ComputeBoardKey(duel, con);
		arena.Restore();
	}

	NoveltyTable flat, serial;
	std::vector<uint64_t> atoms;
	uint32_t states = 0;
	uint32_t mute_flat = 0, run_flat = 0, max_flat = 0;
	uint32_t mute_serial = 0, run_serial = 0, max_serial = 0;
	size_t at = 0;
	while(at < ref.target_at) {
		size_t used = Advance(duel, yrp, at, 1);
		if(!used)
			break;
		at += used;
		++states;
		BoardKey here = ComputeBoardKey(duel, con);
		CollectAtoms(duel, con, here, 0, atoms);
		if(flat.Observe(atoms, static_cast<uint32_t>(at)))
			run_flat = 0;
		else { ++mute_flat; max_flat = (std::max)(max_flat, ++run_flat); }
		// Partition by subgoal reached: the table reopens at every target card
		// placed (serialisation of the conjunctive goal).
		uint32_t part = CommonCodes(here.codes, target.codes);
		CollectAtoms(duel, con, here, part, atoms);
		if(serial.Observe(atoms, static_cast<uint32_t>(at)))
			run_serial = 0;
		else { ++mute_serial; max_serial = (std::max)(max_serial, ++run_serial); }
	}
	arena.Restore();

	if(g_verbose) {
		std::printf("  states visited           : %u\n", states);
		std::printf("  distinct atoms           : %zu  (%zu "
							"serialised)\n",
					flat.Size(), serial.Size());
		std::printf("  mute states (nothing new): %u (%.0f%%)  |  "
							"serialised: %u (%.0f%%)\n",
					mute_flat, states ? 100.0 * mute_flat / states : 0.0,
					mute_serial, states ? 100.0 * mute_serial / states : 0.0);
		std::printf("  longest mute run         : %u  |  serialised: "
							"%u\n",
					max_flat, max_serial);
	}

	// The patience must cover the longest mute run of the known line, with a
	// margin: another deck can be a little more talkative in silences.
	uint32_t patience = (std::max)(12u, max_serial + 4u);
	if(opt.novelty >= 0)
		patience = static_cast<uint32_t>(opt.novelty);
	char wl[80];
	std::snprintf(wl, sizeof(wl), "novelty patience %u%s", patience,
				  patience == 0 ? " (pruning OFF)"
								: (opt.novelty >= 0 ? " (forced)" : ""));
	g_width_line = wl;
	if(g_verbose) {
		std::printf("  => patience %s : %u decisions%s\n",
					opt.novelty >= 0 ? "forced" : "chosen", patience,
					patience == 0 ? "  (pruning DISABLED)" : "");
		if(max_flat + 4 > patience && patience)
			std::printf("     (an unserialised line would have "
									"asked for %u: serialising the goal "
									"shortens the silence)\n", max_flat + 4);
	}
	return patience;
}

// --- ONE SINGLE WIRING POINT -----------------------------------------------
//
// THE FACT THAT JUSTIFIES THIS FUNCTION, and it is measured. Three
// `SearchConfig` objects were built in three places of this file; a field by
// field census gives, out of 82 fields: `RunTransplantSolve` wires 66,
// `RunSolve` 11, `RunGrowth` 6. That is NOT a divergence of purpose (budgets
// legitimately differ from one mode to the next), it is that the MECHANISMS
// chosen on the command line were only applied on ONE path:
//
//   * `--elide-forced` did nothing on the search path, provably byte for byte;
//   * the `--solve` mode, the HEALTH CHECK, never saw `--hindsight` or
//     `--adapt-to-peak`, which explains why the health figures stayed identical
//     across their promotion;
//   * `--max-rollouts` / `--max-nodes`, the instrument of DETERMINISTIC mode,
//     did not exist outside transplantation.
//
// The rule is now mechanical: EVERY field of `SearchConfig` that comes from an
// option is assigned HERE, and nowhere else. What stays with the caller, and
// only that:
//   - the BUDGETS specific to a mode (bounds from the reference line,
//     `--growth` depth, `anytime` from `--optimize`);
//   - the POINTERS to local objects (recipe graph, option catalogue, shared
//     table, NRPA policy): they do not exist in every mode, and that is
//     exactly what `ReportMechanisms` makes visible;
//   - the PER-WORKER NRPA derivation (`nrpa_level` depends on the thread
//     count), the one acknowledged exception, local to the rollout phase.
void ApplyMechanisms(const Options& opt, SearchConfig& cfg) {
	cfg.target_player = opt.target_player;

	// Budget in COUNT (deterministic mode). `max_rollouts` at 0 = no bound, so the
	// unconditional assignment is neutral; `max_nodes` at 0 means "let the mode
	// choose", hence the guard.
	cfg.max_rollouts = opt.max_rollouts;
	if(opt.max_nodes)
		cfg.max_nodes = opt.max_nodes;
	// The domain in TURNS: --turns 2 lets the line cross the opponent's turn
	// (Omega's second rip lives there). 0 = default = 1 turn.
	cfg.max_turns = opt.turns ? static_cast<uint32_t>(opt.turns) : 1u;

	// The four levers and their successors. All of them are READ under the guard
	// of `cfg.recipes`: wiring them with no graph is safe and inert, and
	// `ReportMechanisms` SAYS so instead of keeping quiet.
	cfg.assign = opt.assign;
	cfg.assign_bias = static_cast<float>(opt.assign_bias);
	cfg.op_bias = static_cast<float>(opt.op_bias);
	cfg.backward = opt.backward;
	cfg.probe_repeat = opt.probe_repeat;
	if(opt.recipes >= 0)
		cfg.recipe_h = static_cast<float>(opt.recipes);
	cfg.landmark_weight = static_cast<float>(opt.landmark_weight);
	cfg.landmark_h = static_cast<float>(opt.landmark_h);

	cfg.hindsight = static_cast<float>(opt.hindsight);
	cfg.hindsight_k = opt.hindsight_k;
	cfg.adapt_to_peak = opt.adapt_to_peak;
	cfg.elide_forced = opt.elide_forced;
	cfg.resolve_weight = static_cast<float>(opt.resolve_weight);

	// Finisher and traversal.
	cfg.levin_h = static_cast<float>(opt.levin_h);
	cfg.levin_reroot = opt.levin_reroot;
	cfg.reroot_h = static_cast<float>(opt.reroot_h);
	cfg.dive_full = opt.dive_full;
	cfg.merged_pop = opt.merged_pop;
	cfg.finisher_post_goal = opt.finisher_post_goal;
	cfg.finisher_options = opt.finisher_options;
	cfg.archive_k = opt.archive_k;

	// Policy. `-1` is the "not given" sentinel: the engine default wins, and that
	// is what the report prints.
	if(opt.hint_bias >= 0)
		cfg.hint_bias = static_cast<float>(opt.hint_bias);
	if(opt.nrpa_bias >= 0)
		cfg.nrpa_bias_known = static_cast<float>(opt.nrpa_bias);
	if(opt.nrpa_alpha > 0)
		cfg.nrpa_alpha = static_cast<float>(opt.nrpa_alpha);
	if(opt.nrpa_iters)
		cfg.nrpa_iters = opt.nrpa_iters;
	cfg.nrpa_restart_keep = static_cast<float>(opt.nrpa_keep);
	cfg.nrpa_temp = static_cast<float>(opt.nrpa_temp);
	cfg.nrpa_adapt_passes = opt.adapt_passes;
	cfg.ctx_shrink = static_cast<float>(opt.ctx_shrink);
	cfg.ctx_max = opt.ctx_max;
	cfg.qhat_depth = opt.qhat_depth;
	cfg.qhat_window = opt.qhat_window;
	cfg.qhat_rho = opt.qhat_rho;
	cfg.qhat_max_nodes = opt.qhat_nodes;

	// Lexicographic cost of the burned cards. The default values of `Options` and
	// of `SearchConfig` coincide: the assignment only bites when the flag was
	// passed, and it now bites in EVERY mode, not only under `--optimize`.
	// `--optimize`.
	cfg.burn_slack = opt.burn_slack;
	cfg.burn_limit = opt.burn_limit;
	cfg.phase_w = static_cast<float>(opt.phase_w);
	// Return to the rung. Read under the guard of serial_reqs (search.cpp): wiring
	// it with no ladder is safe and inert, and ReportMechanisms SAYS so.
	cfg.reenter = static_cast<float>(opt.reenter);
	// Path quotas in the LP. Only bites at refinement time: with no refine_after
	// and no model it is inert, and ReportMechanisms SAYS so.
	cfg.quota_h = opt.quota_h;
}

// THE CHECK THAT WAS MISSING, and it is the real lesson of the wiring census.
//
// "A mechanism must print its liveness" was not enough: when the field was
// wired NOWHERE there was simply NOTHING to print, and the bench read a silence
// as an absence of effect. This function runs AFTER the caller has wired its
// pointers, and returns two things no log gave: the mechanisms ACTIVE in this
// mode, and those REQUESTED but INERT here for lack of a dependency. A
// measurement arm whose report carries an `!! INERT` line is an arm to throw
// away before launching it, not after.
void ReportMechanisms(const SearchConfig& cfg, const char* mode) {
	const SearchConfig d;   // the defaults, so only the deviations are printed
	std::string on, inert;
	char buf[160];
	auto add = [&](std::string& dst, const char* fmt, auto... args) {
		std::snprintf(buf, sizeof(buf), fmt, args...);
		if(!dst.empty())
			dst += ", ";
		dst += buf;
	};

	if(cfg.elide_forced != d.elide_forced)     add(on, "elide-forced");
	if(cfg.adapt_to_peak != d.adapt_to_peak)   add(on, "adapt-to-peak");
	if(cfg.hindsight != d.hindsight)           add(on, "hindsight %.2f", cfg.hindsight);
	if(cfg.assign != d.assign)                 add(on, "assign");
	if(cfg.resolve_weight != d.resolve_weight) add(on, "resolve-w %.0f", cfg.resolve_weight);
	if(cfg.max_rollouts)                       add(on, "max-rollouts %llu",
												   (unsigned long long)cfg.max_rollouts);
	if(cfg.hint_bias != d.hint_bias)           add(on, "hint-bias %.2f", cfg.hint_bias);
	if(cfg.qhat_depth != d.qhat_depth)         add(on, "qhat %u", cfg.qhat_depth);

	// The five that REQUIRE the recipe graph, and the two that require the
	// landmarks. A non-zero weight without its pointer is the "live but inert" trap.
	auto dep = [&](bool served, const char* fmt, auto... args) {
		add(served ? on : inert, fmt, args...);
	};
	if(cfg.assign_bias > 0.0f)
		dep(cfg.recipes != nullptr, "assign-bias %.2f", cfg.assign_bias);
	if(cfg.op_bias > 0.0f)
		dep(cfg.recipes != nullptr, "op-bias %.2f", cfg.op_bias);
	if(cfg.backward)
		dep(cfg.recipes != nullptr, "backward");
	if(cfg.probe_repeat)
		dep(cfg.recipes != nullptr, "probe-repeat");
	if(cfg.recipe_h > 0.0f)
		dep(cfg.recipes != nullptr, "recipe-h %.2f", cfg.recipe_h);
	if(cfg.landmark_weight > 0.0f)
		dep(cfg.landmarks != nullptr, "landmark-w %.2f", cfg.landmark_weight);
	if(cfg.landmark_h > 0.0f)
		dep(cfg.landmarks != nullptr, "landmark-h %.2f", cfg.landmark_h);
	// The return to the rung requires a STRUCTURE of cells: the ladder
	// (serial_reqs). Without it an archive cell is a cache, not a frontier, and
	// the mechanism is deliberately inert. Saying so avoids a stillborn
	// measurement.
	if(cfg.reenter > 0.0f)
		dep(!cfg.serial_reqs.empty(), "reenter %.2f", cfg.reenter);
	// The refinement requires the ladder AND the balance model to be lent.
	if(cfg.refine_after)
		dep(!cfg.serial_reqs.empty() && cfg.balance != nullptr,
			"refine-after %u", cfg.refine_after);
	// The path quotas only bite AT refinement: without it (or without derived
	// quota hosts), the flag is requested but inert.
	if(cfg.quota_h)
		dep(cfg.refine_after != 0 && !cfg.serial_reqs.empty() &&
				cfg.balance != nullptr && !cfg.quota_hosts.empty(),
			"quota-h");

	std::printf("  MECANISMES [%s] : %s\n", mode,
				on.empty() ? "none (engine defaults)" : on.c_str());
	if(!inert.empty())
		std::printf("!! REQUESTED but INERT here (dependency absent "
							"in this mode): %s\n", inert.c_str());
}

// A single test only proves one thing: Push/Pop works at depth 1. A log
// mechanism tends to break on nested sequences, successive siblings and
// repeated restores instead. Those are exercised here all along the line,
// checking at every step that the state comes back identical.
int RunStressTest(Duel& duel, const Replay& yrp, const Options& opt, Arena& arena,
				  uint64_t expected_final) {
	// Entry AND exit invariant of every case: depth 1, duel at the start of the
	// line. We return there through Restore(), which preserves the level.
	if(g_verbose)
		std::printf("\n--- snapshot stress test ---\n");
	while(arena.Depth() > 1)
		arena.Pop();
	if(arena.Depth() == 0)
		arena.Push();
	arena.Restore();

	struct Case { const char* name; int failures; int checks; };
	std::vector<Case> cases;
	auto t0 = Clock::now();

	// --- 1. successive siblings: Push, advance, Restore, re-advance, compare
	{
		int fail = 0, checks = 0;
		size_t at = 0;
		uint64_t ref = Fingerprint(duel);
		for(int step = 0; step < 12 && at + 8 < yrp.responses.size(); ++step) {
			if(opt.verbose)
				std::printf("    [siblings] step %d, decision "
											"%zu, depth %zu\n",
							step, at, arena.Depth());
			arena.Push();
			size_t k = Advance(duel, yrp, at, 8);
			uint64_t a = Fingerprint(duel);
			arena.Restore();
			if(Fingerprint(duel) != ref) { ++fail; }
			++checks;
			size_t k2 = Advance(duel, yrp, at, 8);
			uint64_t b = Fingerprint(duel);
			if(a != b || k != k2) { ++fail; }
			++checks;
			arena.Pop();
			if(Fingerprint(duel) != ref) { ++fail; }
			++checks;
			// advance for real
			at += Advance(duel, yrp, at, 8);
			ref = Fingerprint(duel);
		}
		while(arena.Depth() > 1)
			arena.Pop();
		arena.Restore();
		cases.push_back({ "freres successifs (Push/Restore/Pop)", fail, checks });
	}

	// --- 2. deep nesting: push N levels then pop them all
	{
		int fail = 0, checks = 0;
		std::vector<uint64_t> refs;
		size_t at = 0;
		for(int depth = 0; depth < 20 && at + 6 < yrp.responses.size(); ++depth) {
			refs.push_back(Fingerprint(duel));
			arena.Push();
			at += Advance(duel, yrp, at, 6);
		}
		while(!refs.empty()) {
			arena.Pop();
			if(Fingerprint(duel) != refs.back()) ++fail;
			++checks;
			refs.pop_back();
		}
		while(arena.Depth() > 1)
			arena.Pop();
		arena.Restore();
		cases.push_back({ "imbrication profonde (20 niveaux)", fail, checks });
	}

	// --- 3. the duel must stay playable to the end after all that
	{
		int fail = 0;
		LineResult full = RunLine(duel, yrp, opt, false);
		if(full.retries || full.fingerprint_final != expected_final)
			++fail;
		arena.Restore();
		cases.push_back({ "full line replayed after stress", fail, 1 });
	}

	double ms = MsSince(t0);
	int total_fail = 0, total_checks = 0;
	for(const auto& c : cases) {
		if(g_verbose)
			std::printf("  %-42s %3d/%-3d %s\n", c.name, c.checks - c.failures,
						c.checks, c.failures ? "<-- FAILED" : "ok");
		total_fail += c.failures;
		total_checks += c.checks;
	}
	if(g_verbose)
		std::printf("  test duration : %.0f ms\n", ms);
	g_stress_line = "stress " + std::to_string(total_checks - total_fail) + "/" +
					std::to_string(total_checks) + "/" + std::to_string(total_fail);
	if(total_fail)
		std::printf("!! snapshot stress test: %d check(s) FAILED\n", total_fail);
	return total_fail ? 1 : 0;
}

// ENUMERATOR VALIDATION.
//
// Defined further down, with the transplantation driver.
// `opp_hand`: cards added to the opponent's hand in the VERIFICATION duel.
// Null in same-deck mode (the reference has none), the search's in
// transplantation: the verification must replay the SAME duel as the search.
size_t WriteSolutions(const std::vector<Solution>& sols, const Replay& start_yrp,
					  const BoardKey& target, const Options& opt, CardDB& db,
					  ScriptProvider& scripts, const std::string& outdir,
					  const LineConstraints& cons,
					  const std::vector<uint32_t>* opp_hand = nullptr,
					  const std::vector<BoardKey>* target_alts = nullptr);

// Verdict of the reference against the line constraints, and its summon
// sequence: it is what lets one choose the "n" of a constraint. Returns false
// when the reference violates something; the check "at zero deviations the
// reference is found again" is then suspended, by construction rather than by
// default.
bool ReportConstraints(const LineConstraints& cons, const LineResult& ref,
					   const CardDB& db) {
	if(!cons.Any())
		return true;
	std::printf("\n--- line constraints ---\n");
	for(const auto& [n, allowed] : cons.summons) {
		std::printf("  summon #%u among:", n);
		for(uint32_t c : allowed)
			std::printf(" %s;", db.Name(c).c_str());
		std::printf("\n");
	}
	if(!cons.guard.empty()) {
		std::printf("  guard from summon #%u on, at the opponent "
							"windows:\n",
					cons.guard_after);
		for(size_t i = 0; i < cons.guard.size(); ++i) {
			std::printf("    %s", i ? "OR  " : "    ");
			for(size_t j = 0; j < cons.guard[i].size(); ++j)
				std::printf("%s%s", j ? " + " : "",
							db.Name(cons.guard[i][j].code).c_str());
			std::printf("\n");
		}
		if(cons.guard_opp_hand_release >= 0)
			std::printf("    off once the opponent hand is <= %d "
									"card(s) (handrip)\n", cons.guard_opp_hand_release);
	}
	// The derived negation list is printed at load time; naming it HERE too is
	// what makes this section the full account of the disciplines in force.
	if(!cons.self_negate.empty())
		std::printf("  no self-negation: %zu of the deck's own negation effect(s)"
					" never chained onto our own links (--no-self-negate)\n",
					cons.self_negate.size());
	// --guard-keep OUTSIDE the guard block: with no --guard there is no clause for
	// the resource to rest on, so the discipline is INERT. Saying it here is the
	// difference between "the window never came up" and "the flag does nothing".
	for(uint32_t c : cons.guard_keep)
		std::printf("  guard resource: %s%s\n", db.Name(c).c_str(),
					cons.guard.empty()
						? " !! INERT: --guard-keep needs --guard, there is no"
						  " clause for it to rest on"
						: " never activated in the main phase while no other"
						  " clause covers (--guard-keep)");
	for(const auto& [code, zones] : cons.no_activate)
		std::printf("  activation forbidden: %s (zone mask 0x%x)\n",
					db.Name(code).c_str(), zones);
	for(const auto& rq : cons.resolve_min)
		std::printf("  resolutions required : %s x%u%s%s\n",
					db.Name(rq.code).c_str(), rq.min_count,
					rq.zones ? ", activated from" : "",
					rq.zones ? ZoneMaskName(rq.zones).c_str() : "");
	for(const auto& [code, mask] : cons.material_req)
		std::printf("  material required    : %s summoned with >=1 "
							"attribute 0x%x\n",
					db.Name(code).c_str(), mask);

	std::printf("\n  sequence of the reference (%zu summons up to the "
					"board):\n",
				ref.summons_at_target);
	for(size_t i = 0; i < ref.summons_at_target && i < ref.summon_codes.size(); ++i)
		std::printf("      #%-3zu %s\n", i + 1,
					db.Name(db.Canonical(ref.summon_codes[i])).c_str());
	bool ok = true;
	for(const auto& [n, allowed] : cons.summons) {
		if(n > ref.summons_at_target)
			continue;   // conditional semantics: no n-th summon, no fault
		uint32_t canon = db.Canonical(ref.summon_codes[n - 1]);
		if(std::find(allowed.begin(), allowed.end(), canon) == allowed.end()) {
			std::printf("\n  the reference VIOLATES --summon #%u: "
									"its summon #%u was %s\n", n, n, db.Name(canon).c_str());
			ok = false;
		}
	}
	if(ref.guard_violations) {
		std::printf("\n  the reference VIOLATES the guard: %zu "
							"opponent window(s) uncovered out of %zu under "
							"threat\n     (the first after summon #%zu, "
							"opponent hand: %u card(s))\n",
					ref.guard_violations, ref.guard_checks,
					ref.first_guard_violation_summon,
					ref.first_guard_violation_opp_hand);
		ok = false;
	} else if(!cons.guard.empty()) {
		std::printf("\n  guard: %zu opponent window(s) under threat "
							"checked on the reference, all covered.\n", ref.guard_checks);
	}
	if(ref.forbidden_activations) {
		std::printf("  the reference USES a forbidden activation (%zu "
							"time(s))\n",
					ref.forbidden_activations);
		ok = false;
	}
	for(size_t i = 0; i < cons.resolve_min.size(); ++i) {
		size_t have = i < ref.resolve_counts.size() ? ref.resolve_counts[i] : 0;
		if(have < cons.resolve_min[i].min_count) {
			std::printf("  the reference DOES NOT RESOLVE %s "
									"enough: %zu/%u\n",
						db.Name(cons.resolve_min[i].code).c_str(), have,
						cons.resolve_min[i].min_count);
			ok = false;
		} else {
			std::printf("  resolutions %s: %zu/%u on the "
									"reference\n",
						db.Name(cons.resolve_min[i].code).c_str(), have,
						cons.resolve_min[i].min_count);
		}
	}
	if(ok)
		std::printf("\n  the reference satisfies the constraints.\n");
	else
		std::printf("  => at zero discrepancy the search CANNOT "
							"recover the reference: 0 solution\n"
							"     there is an expected result, not an engine "
							"defect.\n");
	return ok;
}

// A search is only worth what its enumerator is worth: if it cannot offer the
// choices a player really made, it is exploring another game. So we replay the
// reference line and check, at every decision, that the recorded answer is
// indeed among the enumerated answers.
int RunEnumeratorCheck(Duel& duel, const Replay& yrp, const Options& opt,
					   Arena& arena) {
	if(g_verbose)
		std::printf("\n--- enumerator coverage ---\n");
	while(arena.Depth() > 1)
		arena.Pop();
	if(arena.Depth() == 0)
		arena.Push();
	arena.Restore();

	EnumOptions eo;
	eo.dedup_by_code = true;
	eo.max_subsets = opt.max_subsets;

	// Comparing bytes would be too strict: EDOPro encodes its selections as bitsets
	// (type 3), the enumerator as index lists (type 2), and deduplication by code
	// picks a representative that is not necessarily the one the player designated.
	// The only criterion that makes sense is the STATE REACHED: an enumerated
	// answer covers the recorded one when it leads to exactly the same state. The
	// arena makes that test affordable.
	// Returns the EXACT fingerprint of the state, and along the way the hash of the
	// BOARD in the sense of the search's equivalence criterion. Both, because they
	// separate three causes the discrepancy message would otherwise conflate:
	//   want == 0            : the RECORDED answer is rejected here, a defect of
	//                          the harness rather than of the enumerator;
	//   board equal, state not: a proposal carries out the SAME INTENT but
	//                          `Fingerprint` separates them (deduplication by code
	//                          picks a representative the player did not designate,
	//                          and two identical copies do not occupy the same
	//                          sequence). That is a REPRESENTATION GAP, not a hole
	//                          in the space;
	//   neither of the two   : the move is REALLY absent from the space.
	// Without that separation, a deduplication artefact reads as "the solution is
	// out of reach", the family of trap this project catalogues.
	uint64_t last_board = 0;
	auto advance_one = [&](const std::vector<uint8_t>& resp) -> uint64_t {
		last_board = 0;
		duel.SetResponse(resp);
		for(;;) {
			int st = duel.Process();
			bool retry = false;
			for(const Message& m : duel.Messages())
				if(m.type == MSG_RETRY)
					retry = true;
			if(retry)
				return 0;   // answer rejected by the core
			if(st != OCG_DUEL_STATUS_CONTINUE)
				break;
		}
		last_board = ComputeBoardKey(
						 duel, static_cast<uint8_t>(opt.target_player)).hash;
		return Fingerprint(duel);
	};

	// `by_board`: discrepancies where a proposal carries out the SAME INTENT (same
	// board in the sense of the equivalence criterion) without reaching the same
	// exact state. Those remove NOTHING from the search space, and counting them
	// separately is what tells an artefact from a real hole.
	struct Cov { int total = 0, covered = 0, empty = 0, by_board = 0; };
	std::map<uint8_t, Cov> cov;
	size_t ri = 0;
	uint8_t ptype = 0;
	std::vector<uint8_t> payload;
	int player = -1;
	std::vector<std::string> misses;
	std::vector<uint64_t> digests;
	// PROBE (instrument, enters no cost, no search touched): the DECOMPOSITION
	// of each digest, so that a fusion says WHICH component merged.
	std::vector<DigestParts> dparts;
	std::vector<uint8_t> dtypes;

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
			continue;   // CONTINUE: the core still has work to do
		if(ri >= yrp.responses.size())
			break;

		const auto recorded = yrp.responses[ri];
		auto choices = Enumerate(ptype, payload.data(),
								 static_cast<uint32_t>(payload.size()), eo);
		Cov& c = cov[ptype];
		++c.total;
		if(choices.empty())
			++c.empty;

		arena.Push();
		uint64_t want = advance_one(recorded);
		const uint64_t want_board = last_board;
		arena.Restore();
		bool found = false, same_board = false;
		for(const auto& ch : choices) {
			const uint64_t got = advance_one(ch.response);
			if(got == want && want != 0) {
				found = true;
				arena.Restore();
				break;
			}
			// SAME BOARD, different state: the intent is covered, the
			// representation is not.
			if(got && want && last_board == want_board)
				same_board = true;
			arena.Restore();
		}
		arena.Pop();

		if(found) {
			++c.covered;
		} else {
			if(same_board)
				++c.by_board;
			if(misses.size() < 8) {
				std::string why;
				if(!want)
					why = "  - THE RECORDED ANSWER IS "
											"REJECTED HERE (harness defect, "
											"not the enumerator's)";
				else if(same_board)
					why = "  - but one proposal reaches "
											"the SAME BOARD: a "
											"REPRESENTATION gap (dedup by "
											"code), not a hole in the space";
				else
					why = "  - and none even reaches the "
											"same BOARD: the move is "
											"GENUINELY absent from the "
											"space";
				misses.push_back(std::string(PromptName(ptype)) + " #" +
								 std::to_string(ri) + " player" +
								 std::to_string(player) + ": none of the" +
								 std::to_string(choices.size()) +
								 " proposals reaches "
																"the recorded state" + why);
			}
		}
		// The line's 290 states are pairwise distinct by construction (the board
		// changes at every action). If the digest merges any of them, it will cut
		// the combo branch without signalling anything.
		digests.push_back(StateDigest(duel, ptype, payload));
		dparts.push_back(StateDigestParts(duel, ptype, payload));
		dtypes.push_back(ptype);

		duel.SetResponse(recorded);
		++ri;
	}
	arena.Restore();

	{
		std::map<uint64_t, size_t> first_seen;
		size_t collisions = 0;
		size_t first_at = 0, first_with = 0;
		for(size_t i = 0; i < digests.size(); ++i) {
			auto [it, fresh] = first_seen.emplace(digests[i], i);
			if(!fresh) {
				if(!collisions) { first_at = i; first_with = it->second; }
				++collisions;
			}
		}
		if(g_verbose)
			std::printf("  digest: %zu states on the line, %zu "
									"distinct, %zu merges\n",
						digests.size(), first_seen.size(), collisions);
		// A fusion where EVERY component matches is not an under-hash: it is a
		// CYCLE in the recorded line: the player left a menu and reopened it, and
		// the state really is the same. The search is RIGHT to cut it; only the
		// zero deviation replay must not (SearchConfig::no_memo).
		auto is_cycle = [&](size_t i, size_t j) {
			return dparts[i].zones == dparts[j].zones &&
			       dparts[i].with_payload == dparts[j].with_payload &&
			       dparts[i].procstate == dparts[j].procstate;
		};
		if(collisions)
			std::printf("    first merge: decision #%zu conflated "
									"with #%zu  <-- %s\n", first_at, first_with,
						is_cycle(first_at, first_with)
							? "CYCLE in the "
														"recorded line: a "
														"prunable detour, "
														"NOT a defect"
							: "the digest "
														"under-hashes, the "
														"search will lose "
														"solutions");
		// WHICH component merged. `full` = zones (+) prompt (+) payload (+)
		// procstate: naming the components that ALREADY match says where the
		// missing information sits, instead of leaving it to be guessed.
		if(collisions) {
			std::map<uint64_t, size_t> seen2;
			for(size_t i = 0; i < digests.size(); ++i) {
				auto [it, fresh] = seen2.emplace(digests[i], i);
				if(fresh)
					continue;
				const DigestParts& x = dparts[i];
				const DigestParts& y = dparts[it->second];
				std::printf("      #%zu = #%zu : zones %s | "
											"col.ignoree %s | +prompt %s | "
											"procstate %s | %s / %s  => %s\n",
						i, it->second,
						x.zones == y.zones ? "EGAL" : "differe",
						x.zones_sorted == y.zones_sorted ? "EGAL" : "differe",
						x.with_payload == y.with_payload ? "EGAL" : "differe",
						x.procstate == y.procstate ? "EGAL" : "differe",
						PromptName(dtypes[i]), PromptName(dtypes[it->second]),
						is_cycle(i, it->second) ? "CYCLE" : "SOUS-HACHAGE");
			}
		}
	}

	// The totals are ACCUMULATED whatever the output level: the verdict below is
	// read off them, and the caller gates the search on that verdict.
	int total = 0, covered = 0, by_board = 0;
	for(const auto& [type, c] : cov) {
		total += c.total;
		covered += c.covered;
		by_board += c.by_board;
	}
	if(g_verbose) {
		std::printf("  %-24s %8s %8s %8s %10s\n", "type", "n", "covered", "rate",
					"board-eq");
		for(const auto& [type, c] : cov)
			std::printf("  %-24s %8d %8d %7.0f%% %10d%s\n", PromptName(type),
						c.total, c.covered,
						c.total ? 100.0 * c.covered / c.total : 0.0,
						c.by_board, c.empty ? "   (empty "
																	"enumeration)" : "");
		std::printf("  %-24s %8d %8d %7.0f%% %10d\n", "TOTAL", total, covered,
					total ? 100.0 * covered / total : 0.0, by_board);
		// EFFECTIVE COVERAGE, the one that matters to the search: a discrepancy
		// where a proposal carries out the same INTENT removes nothing from the
		// space.
		if(by_board)
			std::printf("  %-24s %8d %8d %7.0f%%   <-- EFFECTIVE "
									"coverage (intents); the %d \"board-eq\" "
									"gap(s) are REPRESENTATION\n",
						"TOTAL (at board)", total, covered + by_board,
						total ? 100.0 * (covered + by_board) / total : 0.0, by_board);
	}
	if(!misses.empty() && g_verbose) {
		std::printf("\n  first gaps:\n");
		for(const auto& s : misses)
			std::printf("    %s\n", s.c_str());
	}
	if(total == 0) {
		std::printf("!! enumerator coverage: NO decision observed - "
							"the check tested nothing (harness bug)\n");
		return 1;
	}
	// EFFECTIVE coverage is the figure to read: a "board-eq" gap is a difference
	// of REPRESENTATION, not a hole in the space. The exact count is named too
	// when the two differ, so the line never flatters itself.
	char sum[128];
	if(by_board)
		std::snprintf(sum, sizeof(sum),
					  "%zu digests, coverage %d/%d (%d "
										"exact + %d by board)",
					  digests.size(), covered + by_board, total, covered, by_board);
	else
		std::snprintf(sum, sizeof(sum), "%zu digests, coverage %d/%d",
					  digests.size(), covered, total);
	g_enum_line = sum;
	if(covered + by_board != total)
		std::printf("!! ENUMERATOR DOES NOT COVER THE LINE (%d/%d): "
							"any search explores an incomplete space\n", covered + by_board, total);
	return covered == total ? 0 : 1;
}

// Guided search: reach the target board, then do better than the reference
// line. Returns the number of solutions found.
size_t RunSolve(Duel& duel, const Replay& yrp, const Options& opt, Arena& arena,
				const LineResult& ref, CardDB& db, ScriptProvider& scripts,
				uint32_t patience, const LineConstraints& cons) {
	std::printf("\n=== guided search towards the target board ===\n");
	while(arena.Depth() > 1)
		arena.Pop();
	if(arena.Depth() == 0)
		arena.Push();
	arena.Restore();

	BoardKey target;
	{
		size_t at = 0;
		at += Advance(duel, yrp, at, ref.target_at);
		target = ComputeBoardKey(duel, static_cast<uint8_t>(opt.target_player));
		arena.Restore();
	}
	// Non-negotiable preliminary check: the reference line IS a solution. If the
	// goal test does not fire when replaying it, the defect is in the test, not in
	// the search strategy, and any search result would be worthless.
	uint32_t ref_actions = 0, ref_burned = 0;
	size_t ref_decisions = 0;
	{
		arena.Restore();
		size_t at = 0, hit_at = 0;
		bool hit = false;
		uint32_t best = 0;
		uint32_t acts = 0;
		while(at < yrp.responses.size()) {
			at += Advance(duel, yrp, at, 1, &acts);
			BoardKey k = ComputeBoardKey(duel, static_cast<uint8_t>(opt.target_player));
			uint32_t common = 0, i = 0, j = 0;
			while(i < k.codes.size() && j < target.codes.size()) {
				if(k.codes[i] == target.codes[j]) { ++common; ++i; ++j; }
				else if(k.codes[i] < target.codes[j]) ++i;
				else ++j;
			}
			best = (std::max)(best, common);
			if(k == target) {
				hit = true;
				hit_at = at;
				// The reference's cost must be measured AT THE INSTANT the board is
				// reached, not at the end of the recording: the replay continues
				// afterwards, and comparing against the totals would pass the end of the
				// turn off as progress.
				auto con = static_cast<uint8_t>(opt.target_player);
				ref_actions = acts;
				ref_decisions = at;
				ref_burned = duel.Count(con, LOCATION_GRAVE) +
							 duel.Count(con, LOCATION_REMOVED);
				break;
			}
		}
		arena.Restore();
		std::printf("  control        : the goal test %s when "
							"replaying the reference%s\n",
					hit ? "SE DECLENCHE" : "NE SE DECLENCHE PAS",
					hit ? "" : "  <-- a defect of the goal test, not of the search");
		if(hit)
			std::printf("                   reached at decision "
									"#%zu\n", hit_at);
		else
			std::printf("                   at best %u of the %zu "
									"target cards assembled\n", best, target.codes.size());
		if(!hit)
			return 0;
	}

	std::printf("  target         : %zu cards\n", target.entries.size());
	std::printf("  reference      : %u actions, %zu decisions, %u cards "
					"burned  (measured at the board)\n",
				ref_actions, ref_decisions, ref_burned);

	SearchConfig cfg;
	ApplyMechanisms(opt, cfg);   // one single wiring point
	// Bounds derived from the reference line: we only look for lines that are worse
	// neither in actions nor in decisions.
	cfg.max_decisions = static_cast<uint32_t>(ref_decisions);
	cfg.max_actions = ref_actions;
	cfg.time_limit_ms = opt.solve_ms;
	cfg.max_nodes = 50000000;
	cfg.max_solutions = 16;
	// Anytime optimisation: a pass no longer stops at 16 solutions of equal cost,
	// it EXHAUSTS the space of k deviations. That is what makes the "resistance of
	// the reference" a proof rather than a premature stop (measured: stopping at 16
	// variants takes 0.2 s / 330 states).
	// The action/decision bounds are relaxed: a line that RECOVERS burned cards
	// pays extra actions, and bounds of <= reference would forbid exactly the lines
	// that are cheaper in tier 1.
	if(opt.optimize) {
		cfg.anytime = true;
		cfg.max_solutions = 24;
		cfg.max_actions = ref_actions + 8;
		cfg.max_decisions = static_cast<uint32_t>(ref_decisions) + 48;
		std::printf("  ANYTIME OPTIMISATION: bounds relaxed to %u "
							"actions / %u decisions,\n"
							"  passes run to exhaustion under the cost bound.\n",
					cfg.max_actions, cfg.max_decisions);
	}
	cfg.enumeration.dedup_by_code = true;
	cfg.enumeration.max_subsets = opt.max_subsets;
	// Canonical zones: off unless asked, and wired identically in every mode.
	cfg.enumeration.canonical_zones = opt.canonical_zones;
	// POSTED target -> inclusion by default; CAPTURED target -> exact equality.
	cfg.goal_subset = opt.target_subset ||
					  (!opt.target_specs.empty() && !opt.target_exact);
	cfg.summon_constraints = cons.summons;
	cfg.guard_after = cons.guard_after;
	cfg.guard_clauses = cons.guard;
	cfg.guard_keep = cons.guard_keep.empty() ? nullptr : &cons.guard_keep;
	cfg.guard_opp_hand_release = cons.guard_opp_hand_release;
	cfg.resolve_min = cons.resolve_min;
	CheckSaturations(target.codes.size(), cons.resolve_min);
	cfg.material_req = cons.material_req;
	cfg.hint_cards = cons.hints;
	if(!cons.no_activate.empty()) {
		cfg.enumeration.no_activate = &cons.no_activate;
		// The filter compares canonical codes: the alias table is needed.
		cfg.enumeration.db = &db;
	}
	if(!cons.no_chain.empty())
		cfg.enumeration.no_chain = &cons.no_chain;
	if(!cons.self_negate.empty())
		cfg.self_negate = &cons.self_negate;   // chosen discipline
	cfg.enumeration.mp1_only = opt.mp1_only;   // combo in Main Phase 1 only
	const bool ref_meets_cons = ReportConstraints(cons, ref, db);

	// Reference line recorded once for all the workers: digests (exact
	// resynchronisation, where a state that IS a point further along the line picks
	// the recorded suffix up again) and plan_keys per index (the WINDOWED
	// repertoire, where after a deviation replaying a move near the reference is
	// free). Without both, swapping summons #4/#5 was unfindable up to k=12
	// (measured).
	std::unordered_map<uint64_t, size_t> ref_digests;
	std::vector<uint64_t> ref_keys;
	{
		auto t0 = Clock::now();
		// The recording REPLAYS the reference, which FINISHES ITS TURN: removing the
		// phase exits from it would make its last answer non-enumerable and hole the
		// repertoire silently. --no-phase-change bounds the SEARCH, not the reading of
		// what was played.
		EnumOptions leo = cfg.enumeration;
		RefLineStats rls;
		LiftRefLine(duel, arena, yrp, opt.target_player, ref_decisions,
					leo, ref_digests, ref_keys, &rls);
		arena.Restore();
		size_t known = 0;
		for(uint64_t k : ref_keys)
			known += k != 0;
		std::printf("  line recorded  : %zu digests, %zu/%zu moves "
							"identified (%.0f ms)\n", ref_digests.size(), known, ref_keys.size(),
					MsSince(t0));

		// THE REFERENCE'S LIKELIHOOD, AND IT IS THE ONLY FIGURE THAT SAYS WHETHER
		// SAMPLING HAS A CHANCE.
		//
		// With a NEW policy, all the `plan_key` weights start at ZERO and no bias is
		// armed, so the logits of `search.cpp` are all equal and the softmax is
		// UNIFORM. The probability that a rollout reproduces the line is exactly the
		// product of the inverse arities: a computation, not an estimate. Earlier notes
		// gave an approximation (`0.66^32`) over the idle decisions of a partial plan
		// alone; here it is exact and over the whole line.
		//
		// And COVERAGE is finally read without confusion: "found" (the move is in the
		// action space) is not "identified" (it is in the repertoire). A single move
		// NOT FOUND makes the line unreachable at any budget, and no counter used to
		// say so.
		{
			size_t own = 0, forced = 0, branchy = 0, found = 0, absent = 0;
			double log10p = 0.0;
			uint32_t worst = 0;
			size_t worst_at = 0;
			for(size_t i = 0; i < rls.arity.size(); ++i) {
				const uint32_t a = rls.arity[i];
				if(!a)
					continue;   // opponent decision: not enumerated
				++own;
				if(a == 1)
					++forced;
				else {
					++branchy;
					log10p -= std::log10(static_cast<double>(a));
					if(a > worst) { worst = a; worst_at = i; }
				}
				if(rls.matched[i]) ++found; else ++absent;
			}
			std::printf("\n  --- LIKELIHOOD OF THE REFERENCE "
									"(FRESH policy = uniform softmax) ---\n");
			std::printf("  player decisions: %zu  (forced %zu, "
									"with a choice %zu)\n", own, forced, branchy);
			std::printf("  COVERAGE: reference move FOUND in the "
									"enumeration %zu/%zu", found, own);
			if(absent)
				std::printf("   <<< %zu MISSING: the line is "
											"OUTSIDE the action space", absent);
			std::printf("\n");
			// WHICH move is missing, and at what arity. A coverage hole one cannot
			// locate cannot be fixed, and a single one is enough to make the line
			// unreachable at any budget.
			for(const RefLineStats::Miss& m : rls.misses) {
				std::printf("     decision #%zu  %s: %zu "
											"choice(s) enumerated, NONE "
											"reproduces the reference\n", m.index,
							PromptName(m.prompt), m.offered.size());
				auto hex = [](const std::vector<uint8_t>& v) {
					std::string s;
					char b[8];
					for(uint8_t x : v) {
						std::snprintf(b, sizeof b, "%02x ", x);
						s += b;
					}
					return s;
				};
				std::printf("        REELLE  : %s\n", hex(m.recorded).c_str());
				for(size_t j = 0; j < m.offered.size(); ++j)
					std::printf("        offerte : %s\n",
								hex(m.offered[j]).c_str());
			}
			std::printf("  arite max : %u (decision #%zu)   |   "
									"arite moyenne geometrique : %.2f\n", worst, worst_at,
						branchy ? std::pow(10.0, -log10p / double(branchy))
								: 1.0);
			std::printf("  log10 P(a uniform rollout reproduces "
									"the line) = %.1f   =>  ONE CHANCE IN "
									"10^%.0f\n", log10p, -log10p);
			// WHERE DOES THE IMPROBABILITY GO? A single number does not say, and
			// the answer separates two opposite kinds of work: if most of it comes
			// from decisions that CANNOT affect the goal (position, zone, order of
			// equivalent materials), a mechanical QUOTIENT is enough; if it is all
			// in IDLECMD and the selections, only a SERIALISATION into subgoals can
			// help. The computation is free: every decision's arity is already
			// recorded.
			{
				struct Agg { double log10p = 0.0; size_t n = 0; uint32_t mx = 0; };
				std::map<uint8_t, Agg> by;
				for(size_t i = 0; i < rls.arity.size(); ++i) {
					const uint32_t a = rls.arity[i];
					if(a < 2)
						continue;
					Agg& g = by[rls.prompt[i]];
					g.log10p += std::log10(static_cast<double>(a));
					++g.n;
					g.mx = (std::max)(g.mx, a);
				}
				std::vector<std::pair<double, uint8_t>> ord;
				for(const auto& [t, g] : by)
					ord.emplace_back(g.log10p, t);
				std::sort(ord.rbegin(), ord.rend());
				std::printf("  --- where the %.0f orders of "
											"magnitude come from ---\n", -log10p);
				for(const auto& [v, t] : ord) {
					const Agg& g = by[t];
					std::printf("   %-24s %6.1f  (%zu "
													"decision(s), arite max "
													"%u, moy. geo. %.2f)\n", PromptName(t), v, g.n,
								g.mx, std::pow(10.0, v / double(g.n)));
				}
			}
		}
	}
	cfg.ref_digests = &ref_digests;
	// The windowed repertoire only serves repair UNDER constraints, its use case:
	// with no constraint it spends the budget on permutations of the reference
	// (equal cost by construction) and halves the depth k reached at a fixed budget
	// (measured: k=4 against k=8 at 90 s).
	if(cons.Any())
		cfg.ref_keys = &ref_keys;

	// Progressive deepening of the number of deviations. At zero deviations the
	// search replays the reference, so it always finds at least one solution: "no
	// solution" becomes a defect signal again, not a possible result.
	// resultat possible.
	std::vector<Solution> sols;
	double spent = 0;
	unsigned threads = opt.threads ? opt.threads
								   : (std::max)(1u, std::thread::hardware_concurrency());
	std::printf("  workers        : %u\n", threads);
	std::printf("  novelty        : %s (patience %u)\n",
				patience ? "active" : "desactivee", patience);
	// The `--solve` mode is the HEALTH CHECK, and it never saw a single mechanism
	// until now. It receives them, and it SAYS which ones, which is the only way to
	// know whether a health figure was measured bare or not.
	ReportMechanisms(cfg, "solve");

	struct PassOut {
		std::vector<Solution> found;
		uint64_t nodes = 0, transpos = 0, cuts = 0, resyncs = 0;
		uint64_t goal_hits = 0;   // post-goal re-reaches included
		CutCounts cut;
		bool timed_out = false;
		double ms = 0;
	};
	// One pass at k deviations, with or without novelty pruning. Factored out so
	// the control compares EXACTLY the same engine.
	auto run_pass = [&](uint32_t k, uint32_t pat, double budget) {
		PassOut out;
		auto t0 = Clock::now();
		// k = 0 follows a single path: nothing to parallelise, and it is the
		// control that must find the reference again.
		unsigned n = (k == 0) ? 1u : threads;
		// One token per possible deviation point along the spine. The key is the
		// reference index, bounded by the depth: the table is sized at 4x and cannot
		// saturate here (unlike the transplantation pass, where the key is a state
		// digest).
		ClaimTable claims(ref_decisions + 1);
		// SHARED transposition table for the pass (lazy SMP): a state solved by one
		// worker prunes for all, where private tables redid the same work. Fresh per
		// pass, as the private tables were.
		// tables privees.
		std::unique_ptr<SharedTT> stt;
		if(opt.tt_mb && n > 1)
			stt = std::make_unique<SharedTT>(opt.tt_mb);
		std::mutex merge;

		auto worker = [&](unsigned) {
			// Each worker has ITS own arena and ITS own duel: snapshots do not travel
			// between threads (distinct bases).
			Arena local_arena;
			std::string err;
			if(!local_arena.Init(opt.arena_mb << 20, 0, err))
				return WorkerAbort("arena (repair)", err);
			// The duel lives IN the arena: it must be destroyed before it, otherwise
			// OCG_DestroyDuel works on memory returned to the OS.
			{
				Duel local(db, scripts, &local_arena);
				if(local.Create(yrp.seed, yrp.duel_flags, yrp.start_lp,
								yrp.start_hand, yrp.draw_count, err) &&
				   local.Setup(yrp, err)) {
					if(opt.stop_gc)
						local.SetLuaGc(false);
					SearchConfig wcfg = cfg;
					wcfg.time_limit_ms = budget;
					wcfg.novelty_patience = pat;
					wcfg.shared_tt = stt.get();
					// The zero deviation pass replays the reference: no memoisation, or
					// its own round trips cut it (see SearchConfig::no_memo).
					wcfg.no_memo = (k == 0);
					if(n > 1) {
						// At a single deviation there is no second level: we share the first,
						// for want of better.
						wcfg.claim_level = (k <= 1) ? 0u : 1u;
						wcfg.claims = &claims;
					}
					Search s(local, local_arena, yrp, wcfg);
					s.RunRepair(target, k);
					std::lock_guard<std::mutex> lock(merge);
					for(const auto& x : s.Solutions())
						out.found.push_back(x);
					out.nodes += s.Stats().nodes;
					out.transpos += s.Stats().transpositions;
					out.cuts += s.Stats().novelty_cuts;
					out.resyncs += s.Stats().resyncs;
					out.goal_hits += s.Stats().goal_hits;
					out.cut.Add(s.Stats());
					out.timed_out |= s.Stats().hit_time_limit;
				} else {
					WorkerAbort("duel (reparation)", err);
				}
			}
			ReportPoison("reparation", local_arena);
			local_arena.Shutdown();
		};

		// No worker on the main thread: its arena is already owner there, and a
		// second Init would steal the routing of its frees (the main duel's
		// objects would go to free()).
		std::vector<std::thread> pool;
		for(unsigned i = 0; i < n; ++i)
			pool.emplace_back(worker, i);
		for(auto& t : pool)
			t.join();
		out.ms = MsSince(t0);
		if(prof::enabled) {
			char lbl[32];
			std::snprintf(lbl, sizeof(lbl), "discrep. k=%u", k);
			prof::PrintPhase(lbl);
		}
		return out;
	};

	// Best solution in the lexicographic sense we use.
	auto best_of = [](const std::vector<Solution>& v) -> const Solution* {
		const Solution* best = nullptr;
		for(const auto& s : v) {
			if(!best ||
			   std::tie(s.burned, s.actions, s.decisions) <
				   std::tie(best->burned, best->actions, best->decisions))
				best = &s;
		}
		return best;
	};

	std::printf("\n  %-8s %10s %12s %11s %10s %9s\n", "discrep.", "solutions",
				"states", "transpos.", "cuts", "time");
	uint32_t reached = 0;
	{
		PassOut o = run_pass(0, 0, opt.solve_ms - spent);
		spent += o.ms;
		std::printf("  %-8u %10zu %12llu %11llu %10llu %8.1f s%s\n", 0u,
					o.found.size(), (unsigned long long)o.nodes,
					(unsigned long long)o.transpos, (unsigned long long)o.cuts,
					o.ms / 1000.0, o.timed_out ? "  (budget epuise)" : "");
		PrintCuts(o.cut);
		for(const auto& x : o.found)
			sols.push_back(x);
		if(o.found.empty()) {
			if(!ref_meets_cons) {
				std::printf("       (expected: the reference "
											"violates a line constraint, so "
											"it cannot be recovered)\n");
			} else {
				std::printf("\n  At zero discrepancy the "
											"reference must be recovered. It "
											"is not: engine defect.\n");
				return 0;
			}
		}
	}

	uint32_t k_start = 1;
	if(opt.novelty_ab && patience && spent < opt.solve_ms) {
		// CONTROL (--novelty-ab), off by default: it costs two extra passes,
		// up to 2 x 20 s taken from --solve-ms, and moves the ladder to k = 2.
		// A pruning that gains 10x in states but loses solutions must SAY SO
		// ITSELF, so the control stays available; it is no longer paid for by
		// every run.
		reached = 1;
		double slice = (std::min)((opt.solve_ms - spent) / 4.0, 20000.0);
		PassOut a = run_pass(1, 0, slice);
		spent += a.ms;
		PassOut b = run_pass(1, patience, slice);
		spent += b.ms;
		const Solution* ba = best_of(a.found);
		const Solution* bb = best_of(b.found);
		std::printf("\n--- novelty A/B control (k=1, %.0f s each) ---\n",
					slice / 1000.0);
		std::printf("  without: %10llu states  %5zu solutions\n",
					(unsigned long long)a.nodes, a.found.size());
		std::printf("  with   : %10llu states  %5zu solutions  %llu "
							"cuts  (states %+.0f%%)\n",
					(unsigned long long)b.nodes, b.found.size(),
					(unsigned long long)b.cuts,
					a.nodes ? 100.0 * (double(b.nodes) - double(a.nodes)) /
								  double(a.nodes) : 0.0);
		bool lost_best = ba && (!bb ||
			std::tie(bb->burned, bb->actions, bb->decisions) >
				std::tie(ba->burned, ba->actions, ba->decisions));
		if(ba && bb)
			std::printf("  best cost: without (%u,%u,%u)  with "
									"(%u,%u,%u)  => %s\n",
						ba->burned, ba->actions, ba->decisions,
						bb->burned, bb->actions, bb->decisions,
						lost_best ? "novelty LOSES the "
															"best cost"
								  : "best cost preserved");
		else if(ba && !bb)
			std::printf("  best cost: without (%u,%u,%u)  with "
									"NONE  => novelty LOSES solutions\n",
						ba->burned, ba->actions, ba->decisions);
		for(const auto& x : a.found)
			sols.push_back(x);
		for(const auto& x : b.found)
			sols.push_back(x);
		k_start = 2;
	}

	std::printf("\n  %-8s %10s %12s %11s %10s %9s\n", "discrep.", "solutions",
				"states", "transpos.", "cuts", "time");
	for(uint32_t k = k_start; k <= opt.max_ecarts && spent < opt.solve_ms; ++k) {
		reached = k;
		PassOut o = run_pass(k, patience, opt.solve_ms - spent);
		spent += o.ms;
		char resync[48] = "";
		if(o.resyncs)
			std::snprintf(resync, sizeof(resync), "  %llu resync",
						  (unsigned long long)o.resyncs);
		// Goal reaches (post-goal re-reaches included under --optimize) were readable
		// nowhere.
		char goals[40] = "";
		if(opt.optimize && o.goal_hits)
			std::snprintf(goals, sizeof(goals), "  %llu atteinte(s)",
						  (unsigned long long)o.goal_hits);
		std::printf("  %-8u %10zu %12llu %11llu %10llu %8.1f s%s%s%s\n", k,
					o.found.size(), (unsigned long long)o.nodes,
					(unsigned long long)o.transpos, (unsigned long long)o.cuts,
					o.ms / 1000.0, o.timed_out ? "  (budget epuise)" : "", resync,
					goals);
		PrintCuts(o.cut);
		for(const auto& x : o.found)
			sols.push_back(x);
	}
	arena.Restore();

	if(sols.empty()) {
		std::printf("\n  NO solution reached.\n");
		return 0;
	}

	std::printf("\n  %zu solution(s), ranked by burned cards, then "
					"actions, then decisions.\n\n", sols.size());
	std::sort(sols.begin(), sols.end(), [](const Solution& a, const Solution& b) {
		if(a.burned != b.burned) return a.burned < b.burned;
		if(a.actions != b.actions) return a.actions < b.actions;
		return a.decisions < b.decisions;
	});
	std::printf("  %-4s %10s %9s %11s %8s %8s %8s\n", "#", "burned", "actions",
				"decisions", "main", "deck", "extra");
	int better = 0;
	for(size_t i = 0; i < sols.size() && i < 10; ++i) {
		const Solution& x = sols[i];
		// Strictly better in the lexicographic sense we use: fewer cards burned,
		// or as many but fewer actions.
		bool wins = x.burned < ref_burned ||
					(x.burned == ref_burned && x.actions < ref_actions);
		if(wins)
			++better;
		std::printf("  %-4zu %10u %9u %11u %8u %8u %8u%s\n", i, x.burned, x.actions,
					x.decisions, x.hand_left, x.deck_left, x.extra_left,
					wins ? "   <-- better than the "
											"reference" : "");
	}
	std::printf("\n  %-4s %10u %9u %11zu   (reference)\n", "ref", ref_burned,
				ref_actions, ref_decisions);
	WriteSolutions(sols, yrp, target, opt, db, scripts, opt.outdir, cons);
	if(!better)
		std::printf("\n  No strictly better line found.\n"
							"  The reference resists %u simultaneous "
							"deviation(s).\n",
					reached);
	(void)ref;
	(void)db;
	return sols.size();
}

// Opening hand of a replay, read on a throwaway duel set up for the occasion.
// It is the only way to know it: it depends on the seed and on the core's
// shuffle, not on the file.
std::vector<uint32_t> OpeningHand(const Replay& yrp, uint8_t con, CardDB& db,
								  ScriptProvider& scripts, size_t arena_mb) {
	std::vector<uint32_t> out;
	// On its own thread, imperatively: Arena::Init takes ownership of the current
	// thread's free routing (t_owner). Setting up a second arena on the main thread
	// would dispossess the reference duel's arena, whose objects would then go to
	// free().
	std::thread([&] {
		Arena a;
		std::string err;
		if(!a.Init(arena_mb << 20, 0, err))
			return WorkerAbort("arena (hand probe)", err);
		{
			Duel d(db, scripts, &a);
			if(d.Create(yrp.seed, yrp.duel_flags, yrp.start_lp, yrp.start_hand,
						yrp.draw_count, err) && d.Setup(yrp, err)) {
				while(d.Process() == OCG_DUEL_STATUS_CONTINUE) {}
				for(const auto& c : d.Query(con, LOCATION_HAND,
											QUERY_CODE | QUERY_ALIAS))
					if(c.present)
						out.push_back(db.Canonical(c.Code()));
			} else {
				WorkerAbort("duel (hand probe)", err);
			}
		}
		a.Shutdown();
	}).join();
	return out;
}

// Loads a .ydk decklist: main and extra codes, side ignored.
bool LoadYdk(const std::string& path, Deck& out, std::string& error) {
	std::ifstream in(path);
	if(!in) {
		error = "unreadable decklist: " + path;
		return false;
	}
	std::vector<uint32_t>* section = nullptr;
	std::string line;
	while(std::getline(in, line)) {
		line = Trimmed(line);
		if(line.empty())
			continue;
		if(line[0] == '#' || line[0] == '!') {
			if(line == "#main")       section = &out.main;
			else if(line == "#extra") section = &out.extra;
			else if(line[0] == '!')   section = nullptr;   // side: outside the duel
			continue;
		}
		char* end = nullptr;
		unsigned long code = std::strtoul(line.c_str(), &end, 10);
		if(end && *end == '\0' && code > 0 && section)
			section->push_back(static_cast<uint32_t>(code));
	}
	if(out.main.empty()) {
		error = "no card in the main deck of " + path;
		return false;
	}
	return true;
}

// Copies the fields of a Replay (the class is non-copyable because of the
// embedded yrp1), everything EXCEPT answers, packets and the embedded yrp: this
// is a starting position, not a line.
void CopyReplayHeader(const Replay& src, Replay& dst) {
	dst.id = src.id;
	dst.version = src.version;
	dst.flag = src.flag;
	dst.timestamp = src.timestamp;
	dst.header_version = src.header_version;
	std::memcpy(dst.seed, src.seed, sizeof(dst.seed));
	std::memcpy(dst.props, src.props, sizeof(dst.props));
	dst.start_lp = src.start_lp;
	dst.start_hand = src.start_hand;
	dst.draw_count = src.draw_count;
	dst.duel_flags = src.duel_flags;
	dst.scriptname = src.scriptname;
	dst.home_count = src.home_count;
	dst.opposing_count = src.opposing_count;
	dst.players = src.players;
	dst.decks = src.decks;
	dst.rule_cards = src.rule_cards;
}

// Builds a SYNTHETIC starting position: the reference's duel, with the same
// parameters and the same opponent, but with THIS deck and THIS hand.
//
// The hand is forced through DUEL_PSEUDO_SHUFFLE (the core no longer shuffles)
// plus a reordering of the main deck. Which end gets drawn depends on the core:
// we do not guess it, we VERIFY it. A throwaway duel draws the hand, and when
// it does not match we try the other end. Failure on both sides is a hard
// error, never a search on a hand one only believes one has.
bool BuildSyntheticStart(const Replay& ref, const Deck& ydk,
						 const std::vector<uint32_t>& hand, CardDB& db,
						 ScriptProvider& scripts, size_t arena_mb,
						 Replay& out, std::string& error) {
	CopyReplayHeader(ref, out);
	out.duel_flags |= DUEL_PSEUDO_SHUFFLE;
	out.start_hand = static_cast<uint32_t>(hand.size());
	out.decks.resize(2);
	out.decks[1] = ref.decks.size() > 1 ? ref.decks[1] : Deck{};
	out.decks[0].extra = ydk.extra;

	// Remove ONE occurrence of each hand card from the rest of the deck, by
	// canonical identity (the decklist may carry another artwork).
	std::vector<uint32_t> rest = ydk.main, hand_codes;
	for(uint32_t want : hand) {
		uint32_t canon = db.Canonical(want);
		bool found = false;
		for(size_t i = 0; i < rest.size(); ++i) {
			if(db.Canonical(rest[i]) == canon) {
				hand_codes.push_back(rest[i]);
				rest.erase(rest.begin() + i);
				found = true;
				break;
			}
		}
		if(!found) {
			error = "the requested hand holds " + db.Name(canon) +
					" which is not (sufficiently) in the "
										"decklist";
			return false;
		}
	}

	auto matches = [&](const std::vector<uint32_t>& got) {
		if(got.size() != hand.size())
			return false;
		std::vector<uint32_t> a = got, b;
		for(uint32_t c : hand)
			b.push_back(db.Canonical(c));
		std::sort(a.begin(), a.end());
		std::sort(b.begin(), b.end());
		return a == b;
	};

	// Attempt 1: the hand at the END of the main deck (the core draws from the top,
	// which is the tail of the list); attempt 2: at the START.
	for(int attempt = 0; attempt < 2; ++attempt) {
		out.decks[0].main.clear();
		if(attempt == 0) {
			out.decks[0].main = rest;
			out.decks[0].main.insert(out.decks[0].main.end(),
									 hand_codes.begin(), hand_codes.end());
		} else {
			out.decks[0].main = hand_codes;
			out.decks[0].main.insert(out.decks[0].main.end(),
									 rest.begin(), rest.end());
		}
		auto got = OpeningHand(out, 0, db, scripts, arena_mb);
		if(matches(got))
			return true;
	}
	error = "cannot force the requested hand: the core draws neither the "
				"head nor the tail of the deck in that order (check the "
				"reference duel's flags)";
	return false;
}

// What the search managed to place, against what was needed. A count ("5 of 8")
// only turns into a decision when one knows WHICH are missing.
void ReportBestBoard(const std::vector<uint32_t>& best, const BoardKey& target,
					 const CardDB& db) {
	if(best.empty())
		return;
	std::map<uint32_t, int> delta;
	for(uint32_t c : target.codes) ++delta[c];
	for(uint32_t c : best) --delta[c];
	std::printf("\n--- best board reached, against the target ---\n");
	for(uint32_t c : best)
		std::printf("      placed    %9u  %s\n", c, db.Name(c).c_str());
	for(const auto& [code, n] : delta)
		if(n > 0)
			std::printf("      MANQUE %dx %9u  %s\n", n, code,
						db.Name(code).c_str());
	for(const auto& [code, n] : delta)
		if(n < 0)
			std::printf("      surplus%3dx %9u  %s\n", -n, code,
						db.Name(code).c_str());
}

// Writes the solutions as replayable replays, after VERIFYING them.
//
// A solution comes out of a search that deduplicates and canonicalises: nothing
// guarantees a priori that its sequence of answers, replayed from scratch,
// rebuilds the board. So we replay it in a fresh duel and only write what holds.
size_t WriteSolutions(const std::vector<Solution>& sols, const Replay& start_yrp,
					  const BoardKey& target, const Options& opt, CardDB& db,
					  ScriptProvider& scripts, const std::string& outdir,
					  const LineConstraints& cons,
					  const std::vector<uint32_t>* opp_hand,
					  const std::vector<BoardKey>* target_alts) {
	std::error_code ec;
	std::filesystem::create_directories(outdir, ec);
	// Write ceiling, named instead of being a bare 16 at the bottom of a loop.
	constexpr size_t kMaxWritten = 16;
	size_t written = 0, rejected = 0;
	size_t rej_retry = 0, rej_cons = 0, rej_board = 0;
	// WITNESSES OF THE REJECTED CANDIDATES: a line that touched the goal during
	// the search is no longer LOST in a validator disagreement; it is written,
	// marked with its reason. Once measured: 6 lines at the COMPLETE goal rejected
	// as "non-conforming board" (an S/T zone required empty against the Assault
	// Zone every line of the deck sets on its first move), zero bytes on disk,
	// lines lost with the process. Ceiling 8.
	size_t rej_written = 0;
	// Rejections for a --summon constraint NEVER REACHED: distinct from a VIOLATED
	// constraint, and far more informative, since they say the line stops before
	// the point the experiment aims at.
	size_t rej_never = 0;
	const auto con = static_cast<uint8_t>(opt.target_player);
	// Summon sequence of the first solution written: the visible proof that a
	// --summon constraint is met.
	std::vector<uint32_t> first_summons;

	// Dedicated thread: an arena is never initialised on a thread that already owns
	// one.
	std::thread([&] {
		Arena a;
		std::string err;
		if(!a.Init(opt.arena_mb << 20, 0, err))
			return WorkerAbort("arena (corpus replay)", err);
		{
			Duel d(db, scripts, &a);
			if(!d.Create(start_yrp.seed, start_yrp.duel_flags, start_yrp.start_lp,
						 start_yrp.start_hand, start_yrp.draw_count, err) ||
			   !d.Setup(start_yrp, err, opp_hand,
						static_cast<uint8_t>(1 - opt.target_player)))
				return WorkerAbort("duel (corpus replay)", err);
			if(opt.stop_gc)
				d.SetLuaGc(false);
			a.Push();
			// Write ceiling. Under --optimize, max_solutions is 24 PER worker and
			// the sets merge, so truncation is the rule, not the exception. At the
			// sites that do not sort first (--fire, AR.sols), the sixteen kept are
			// not the cheapest, they are the sixteen that ARRIVED FIRST, and
			// `written`/`rejected` did not let one see it, since sols.size() was
			// never printed.
			// imprime (4.9).
			for(size_t i = 0; i < sols.size() && i < kMaxWritten; ++i) {
				size_t used = 0;
				bool retry = false;
				bool guard_ok = true;
				bool material_ok = true;
				int pplayer = -1;
				std::vector<uint32_t> summons, mats;
				std::vector<size_t> resolves(cons.resolve_min.size(), 0);
				auto scan = [&] {
					for(const Message& m : d.Messages()) {
						if(m.type == MSG_RETRY)
							retry = true;
						if(m.type == MSG_MOVE && !cons.material_req.empty() &&
						   m.size >= 28) {
							uint32_t c = 0, reason = 0;
							std::memcpy(&c, m.data, 4);
							std::memcpy(&reason, m.data + 24, 4);
							if((reason & REASON_SYNCHRO) &&
							   (reason & REASON_MATERIAL))
								mats.push_back(c);
						}
						if((m.type == MSG_SUMMONING || m.type == MSG_SPSUMMONING) &&
						   m.size >= 4) {
							uint32_t c = 0;
							std::memcpy(&c, m.data, 4);
							summons.push_back(c);
							if(!cons.resolve_min.empty() && c) {
								const uint32_t sc = db.Canonical(c);
								for(size_t k = 0; k < cons.resolve_min.size(); ++k)
									if(cons.resolve_min[k].on_summon &&
									   cons.resolve_min[k].code == sc)
										++resolves[k];
							}
							if(!cons.material_req.empty() && c) {
								uint32_t canon = db.Canonical(c);
								for(const auto& [card, attrs] : cons.material_req) {
									if(card != canon)
										continue;
									bool ok = false;
									for(uint32_t mc : mats) {
										const CardRow* row = db.Find(mc);
										if(row && (row->attribute & attrs)) {
											ok = true;
											break;
										}
									}
									if(!ok)
										material_ok = false;
								}
								mats.clear();
							}
						}
						if(m.type == MSG_CHAINING && !cons.resolve_min.empty() &&
						   m.size >= 4) {
							uint32_t c = 0;
							std::memcpy(&c, m.data, 4);
							c = db.Canonical(c);
							const uint32_t loc = ChainingLocation(m.data, m.size);
							for(size_t k = 0; k < cons.resolve_min.size(); ++k)
								if(!cons.resolve_min[k].on_summon &&
								   cons.resolve_min[k].code == c &&
								   (!cons.resolve_min[k].zones ||
									(loc & cons.resolve_min[k].zones)))
									++resolves[k];
						}
						if(IsPrompt(m.type))
							pplayer = m.size ? m.data[0] : -1;
					}
				};
				while(used < sols[i].responses.size() && !retry) {
					int status = d.Process();
					scan();
					if(status == OCG_DUEL_STATUS_AWAITING) {
						// An opponent window under guard: re-checking it here is part of the
						// "verify before writing" contract.
						if(!cons.guard.empty() && guard_ok &&
						   pplayer == 1 - opt.target_player &&
						   summons.size() >= cons.guard_after &&
						   (cons.guard_opp_hand_release < 0 ||
							static_cast<int>(d.Count(
								static_cast<uint8_t>(1 - opt.target_player),
								LOCATION_HAND)) > cons.guard_opp_hand_release)) {
							BoardKey fk = ComputeBoardKey(d, con);
							if(!GuardHolds(d, con, cons.guard, fk.codes))
								guard_ok = false;
						}
						d.SetResponse(sols[i].responses[used++]);
					} else if(status != OCG_DUEL_STATUS_CONTINUE)
						break;
				}
				while(d.Process() == OCG_DUEL_STATUS_CONTINUE)
					scan();
				// The search already imposed the constraints along the path; we
				// re-check here because "verify before writing" admits no exception.
				bool cons_ok = guard_ok && material_ok;
				size_t summon_never = 0;
				for(const auto& [n, allowed] : cons.summons) {
					// The n-th summon NEVER happened. On the search side that is a
					// PREFIX constraint, hence conditional by construction. Here we are
					// in the "verify before writing" check, which admits no exception:
					// letting it through amounted to writing as CONFORMING a line on
					// which the very constraint the experiment is built around was never
					// exercised, and then counting it in "N lines reaching the board".
					if(n > summons.size()) {
						++summon_never;
						cons_ok = false;
						continue;
					}
					uint32_t canon = db.Canonical(summons[n - 1]);
					if(std::find(allowed.begin(), allowed.end(), canon) ==
					   allowed.end())
						cons_ok = false;
				}
				for(size_t k = 0; k < cons.resolve_min.size(); ++k)
					if(resolves[k] < cons.resolve_min[k].min_count)
						cons_ok = false;
				// Main goal, or one of the ALTERNATIVE goals (--fire: the board
				// without the cards sacrificed to answer the threat).
				const BoardKey fin = ComputeBoardKey(d, con);
				// Under --target-subset, INCLUSION also governs at WRITE time: the
				// search judge was already using inclusion for a posted target, but
				// this validator required strict equality, so the flag changed
				// NOTHING here and the 6 lines at the goal would be rejected
				// identically. `entries` is sorted (ComputeBoardKeyInto), so
				// inclusion is std::includes. Without the flag: strict equality, the
				// historical behaviour byte for byte.
				// stricte, comportement historique a l'octet.
				const bool full_board =
					opt.target_subset
						? std::includes(fin.entries.begin(), fin.entries.end(),
										target.entries.begin(),
										target.entries.end())
						: fin == target;
				bool alt_board = false;
				if(!full_board && target_alts)
					for(const BoardKey& ab : *target_alts)
						if(fin == ab) {
							alt_board = true;
							break;
						}
				const bool ok = !retry && cons_ok && (full_board || alt_board);
				if(ok) {
					char name[64];
					std::snprintf(name, sizeof(name),
								  "solution_%02zu_b%u_a"
																"%u%s.yrp", i,
								  sols[i].burned, sols[i].actions,
								  full_board ? "" : "_alt");
					std::string path = outdir + "/" + name;
					std::string werr;
					if(WriteYrp1(path, start_yrp, sols[i].responses, werr)) {
						if(written == 0)
							first_summons = summons;
						++written;
					} else
						std::printf("  !! %s\n", werr.c_str());
				} else {
					++rejected;
					const char* why = "board";
					if(retry) {
						++rej_retry;
						why = "retry";
					} else if(!cons_ok) {
						++rej_cons;
						if(summon_never)
							++rej_never;
						why = "constraint";
					}
					else
						++rej_board;
					// THE WITNESS: the candidate is written anyway, marked.
					if(rej_written < 8) {
						char name[64];
						std::snprintf(name, sizeof(name),
									  "but_rejete_%02zu_%s."
																		"yrp", i, why);
						std::string werr;
						if(WriteYrp1(outdir + "/" + name, start_yrp,
									 sols[i].responses, werr))
							++rej_written;
					}
				}
				a.Restore();
			}
			a.Pop();
		}
		a.Shutdown();
	}).join();

	std::printf("\n--- output ---\n");
	std::printf("  %zu replay(s) written to %s  (out of %zu candidate(s))\n",
				written, outdir.c_str(), sols.size());
	if(sols.size() > kMaxWritten)
		std::printf("      !! %zu candidate(s) NOT EXAMINED: the "
							"write ceiling is %zu.\n"
							"      If the caller did not sort, these are the "
							"first to ARRIVE, not the cheapest.\n",
					sols.size() - kMaxWritten, kMaxWritten);
	if(rejected)
		std::printf("  %zu rejected: %zu MSG_RETRY, %zu "
							"constraint(s), %zu non-conforming board\n", rejected, rej_retry, rej_cons,
					rej_board);
	if(rej_never)
		std::printf("      of which %zu where a --summon constraint "
							"is NEVER REACHED:\n      the line ends before "
							"the targeted summon, so the constraint was never "
							"exercised\n", rej_never);
	if(cons.Any() && !first_summons.empty() && g_verbose) {
		std::printf("\n  summons of the best solution written:\n");
		for(size_t i = 0; i < first_summons.size(); ++i) {
			bool constrained =
				cons.summons.count(static_cast<uint32_t>(i + 1)) != 0;
			std::printf("      #%-3zu %s%s\n", i + 1,
						db.Name(db.Canonical(first_summons[i])).c_str(),
						constrained ? "   <-- constraint" : "");
		}
	}
	return written;
}

// ADAPTATION REPLAY OF THE CORPUS (--adapt), the remaining route of
// arXiv:2401.10431 once the WEIGHT prior is set aside. Same corpus, same
// recording on the duel of ITS OWN header, same injection point (initial policy
// of the rollouts): what changes is the FORM of the signal. The prior said
// "this move exists in the solutions" and rewarded it everywhere; the
// adaptation says "at THIS junction, the solution took this one against those",
// which is the NRPA gradient itself, applied to already solved sequences
// instead of to the run's rollouts.
//
// So the comparison is clean: --prior and --adapt inject at the same place, and
// the only measurable difference is per-move bonus against discriminative
// gradient.
// --- LANDMARK LEARNING ------------------------------------------------------
//
// Replays each RESOLVED plan of the corpus and records, at every decision, the
// multiset of facts (code, zone). The graph then takes the INTERSECTION and the
// mean order (see LandmarkGraph).
//
// WHY THIS IS NOT `--adapt` UNDER ANOTHER NAME. `--adapt` records JUNCTIONS
// (which moves were legal, which one was played) and pours them into the
// policy's gradient. It learns to REPRODUCE lines, and its ceiling was
// measured: one weight per move, blind to the state, with corpus agreement
// stuck at 59 against a pure-memorisation maximum of 64. Landmarks record
// STATES: what must have been held, and how many times. That is the difference
// between "replay this move here" and "you need two Leo Dancer in the graveyard
// before hoping for three Ligers", and it is the second that carries over to a
// state the corpus never visited.
void BuildLandmarkGraph(const Options& opt, CardDB& db, ScriptProvider& scripts,
						const std::vector<PlanStep>& plan,
						const BoardKey& target, LandmarkGraph& graph) {
	if(opt.landmark_files.empty())
		return;
	namespace fs = std::filesystem;
	std::vector<std::string> files;
	for(const std::string& p : opt.landmark_files) {
		std::error_code ec;
		if(fs::is_directory(p, ec)) {
			for(const auto& e : fs::directory_iterator(p, ec)) {
				const auto ext = e.path().extension();
				if(ext == L".yrp" || ext == L".yrpX")
					files.push_back(e.path().string());
			}
		} else {
			files.push_back(p);
		}
	}
	std::sort(files.begin(), files.end());
	if(files.empty()) {
		std::printf("!! --landmarks: no .yrp/.yrpX file found\n");
		return;
	}
	std::unordered_map<uint64_t, size_t> repertoire;
	for(size_t i = 0; i < plan.size(); ++i)
		if(plan[i].edge)
			repertoire.emplace(plan[i].edge, i);

	std::printf("\n--- landmarks appris : %zu plan(s) resolu(s) "
					"(--landmarks) ---\n",
				files.size());
	auto t0 = Clock::now();
	for(const std::string& f : files) {
		Replay holder;
		std::string err;
		if(!holder.Load(f, err)) {
			std::printf("  !! %s : %s\n", f.c_str(), err.c_str());
			continue;
		}
		const Replay* pr = holder.IsStreamed() ? holder.Embedded() : &holder;
		if(!pr || pr->responses.empty()) {
			std::printf("  !! %s: no readable answers\n", f.c_str());
			continue;
		}
		// The duel of ITS OWN header, on a dedicated thread: an arena is never
		// initialised on a thread that already owns one.
		std::thread([&] {
			Arena pa;
			std::string aerr;
			if(!pa.Init(opt.arena_mb << 20, 0, aerr)) {
				std::printf("  !! landmark arena: %s\n", aerr.c_str());
				return;
			}
			{
				Duel pd(db, scripts, &pa);
				if(!pd.Create(pr->seed, pr->duel_flags, pr->start_lp,
							  pr->start_hand, pr->draw_count, aerr) ||
				   !pd.Setup(*pr, aerr)) {
					std::printf("  !! %s: duel cannot be "
													"initialised: %s\n",
								f.c_str(), aerr.c_str());
				} else {
					if(opt.stop_gc)
						pd.SetLuaGc(false);
					EnumOptions eo;
					eo.dedup_by_code = true;
					eo.max_subsets = opt.max_subsets;
					eo.db = &db;
					NrpaRun run;
					LandmarkTrace trace;
					LiftPolicyRun(pd, pa, *pr, opt.target_player, SIZE_MAX, eo,
								  repertoire, target, run, nullptr, 0, &trace);
					trace.Normalize();
					std::printf("  %-44s %4u decisions, "
													"%zu distinct fact(s)\n",
								fs::path(f).filename().string().c_str(),
								trace.decisions, trace.first.size());
					if(trace.decisions)
						graph.AddPlan(std::move(trace));
				}
			}
			pa.Shutdown();
		}).join();
	}
	if(!graph.Plans()) {
		std::printf("  !! no usable plan: the graph stays EMPTY and "
							"the mechanism will be inert\n");
		return;
	}
	graph.Build();
	std::printf("  %zu plan(s) verses, %.0f ms\n", graph.Plans(), MsSince(t0));
	// THE HONESTY CAVEAT, printed rather than buried. With a single plan, the
	// intersection IS that plan: those are not generalised landmarks, they are the
	// trace of one line. Saying it here keeps a positive result from being read as
	// a generalisation when it would only be a repertoire of states.
	if(graph.Plans() < 2)
		std::printf("  !! ONE plan only: the intersection is that "
							"plan. The landmarks are NOT generalised -\n"
							"     they describe one line. Read them as such "
							"in any A/B.\n");
	if(graph.Empty()) {
		std::printf("  !! no landmark: every common fact was already "
							"true in the initial state (rule 1)\n");
		return;
	}
	// THE INSTRUMENT (quantify before letting it decide). What the graph learned,
	// in progress order, with the REPETITION LOOPS marked: that is the shape the
	// work called for, "Leo Dancer in the graveyard, COUNT".
	auto zname = [](uint8_t z) {
		switch(z) {
		case 0x02: return "main";
		case 0x10: return "cimetiere";
		case 0x20: return "banni";
		case 0x0c: return "terrain";
		case 0x82: return "ADV main";
		case 0x90: return "ADV cimet.";
		case 0xa0: return "ADV banni";
		case 0x8c: return "ADV terrain";
		default:   return "?";
		}
	};
	// How many copies at most per (code, zone): a count > 1 IS the paper's
	// repetition loop, and it is the one thing the flat `h` could not express.
	std::map<uint64_t, uint32_t> loops;
	for(const Landmark& lm : graph.Items()) {
		uint32_t& m = loops[LandmarkGraph::KeyOf(lm.code, lm.zone)];
		m = (std::max)(m, lm.count);
	}
	size_t nloop = 0;
	for(const auto& [k, m] : loops)
		if(m > 1)
			++nloop;
	std::printf("  %zu landmark(s) over %zu distinct fact(s), of which "
					"%zu REPETITION LOOP(S)\n", graph.Items().size(), loops.size(), nloop);
	// Order of FIRST achievement per (code, zone): it is the graph's axis, and the
	// table must follow it. Sorted by key, it would make the order unreadable and
	// "ordered progression" would be nothing but a promise.
	std::vector<std::pair<float, uint64_t>> rows;
	for(const auto& [key, m] : loops) {
		float first = 2.0f;
		for(const Landmark& lm : graph.Items())
			if(LandmarkGraph::KeyOf(lm.code, lm.zone) == key)
				first = (std::min)(first, lm.order);
		rows.push_back({ first, key });
	}
	std::sort(rows.begin(), rows.end());
	auto label = [&](uint64_t key) {
		return db.Name(LandmarkGraph::CodeOf(key)) + " @" +
			   zname(LandmarkGraph::ZoneOf(key));
	};
	std::printf("     %-6s %-44s %-12s %s\n", "ordre", "landmark", "zone",
				"exemplaires");
	for(const auto& [first, key] : rows) {
		// One row per (code, zone), carrying the order of its FIRST copy and the
		// count required: that is readable, where one row per (code, zone, k) would
		// drown the loop in its own repetitions.
		std::printf("     %-6.2f %-44s %-12s x%u%s\n", first,
					db.Name(LandmarkGraph::CodeOf(key)).c_str(),
					zname(LandmarkGraph::ZoneOf(key)), loops[key],
					loops[key] > 1 ? "   <-- BOUCLE" : "");
	}
	// THE PROGRESS EDGE: for each loop, the landmarks that PRECEDE it, i.e. the
	// sentence "you need one before every X", made checkable. IMMEDIATE
	// predecessors only (the last four before it): the full list would be half
	// the graph and would no longer say anything.
	for(const auto& [first, key] : rows) {
		if(loops[key] < 2)
			continue;
		std::printf("     loop \"%s x%u\" - preceded by:",
					label(key).c_str(), loops[key]);
		std::vector<std::string> before;
		for(const auto& [o2, k2] : rows) {
			if(k2 == key || o2 >= first)
				continue;
			before.push_back(label(k2));
		}
		if(before.size() > 4)
			before.erase(before.begin(),
						 before.end() - 4);   // the CLOSEST ones before it
		for(size_t i = 0; i < before.size(); ++i)
			std::printf("%s %s", i ? "," : "", before[i].c_str());
		std::printf("%s\n", before.empty() ? " (nothing)" : "");
	}
}

void BuildAdaptRuns(const Options& opt, CardDB& db, ScriptProvider& scripts,
					const std::vector<PlanStep>& plan, const BoardKey& target,
					std::vector<NrpaRun>& out, RecipeGraph* recipes = nullptr) {
	if(opt.adapt_files.empty())
		return;
	namespace fs = std::filesystem;
	std::vector<std::string> files;
	for(const std::string& p : opt.adapt_files) {
		std::error_code ec;
		if(fs::is_directory(p, ec)) {
			for(const auto& e : fs::directory_iterator(p, ec)) {
				const auto ext = e.path().extension();
				if(ext == L".yrp" || ext == L".yrpX")
					files.push_back(e.path().string());
			}
		} else {
			files.push_back(p);
		}
	}
	std::sort(files.begin(), files.end());
	if(files.empty()) {
		std::printf("!! --adapt: no .yrp/.yrpX file found\n");
		return;
	}
	// The reference's repertoire, as the sampling sees it: Adapt() must recompute
	// the SAME probabilities as PolicyRollout.
	std::unordered_map<uint64_t, size_t> repertoire;
	for(size_t i = 0; i < plan.size(); ++i)
		if(plan[i].edge)
			repertoire.emplace(plan[i].edge, i);

	std::printf("\n--- adaptation replay: %zu corpus line(s) (--adapt) "
					"---\n",
				files.size());
	auto t0 = Clock::now();
	size_t total_steps = 0, total_unknown = 0;
	for(const std::string& f : files) {
		Replay holder;
		std::string err;
		if(!holder.Load(f, err)) {
			std::printf("  !! %s : %s\n", f.c_str(), err.c_str());
			continue;
		}
		const Replay* pr = holder.IsStreamed() ? holder.Embedded() : &holder;
		if(!pr || pr->responses.empty()) {
			std::printf("  !! %s: no readable answers\n", f.c_str());
			continue;
		}
		// The duel of ITS OWN header, on a dedicated thread: an arena is never
		// initialised on a thread that already owns one.
		std::thread([&] {
			Arena pa;
			std::string aerr;
			if(!pa.Init(opt.arena_mb << 20, 0, aerr)) {
				std::printf("  !! adaptation arena: %s\n", aerr.c_str());
				return;
			}
			{
				Duel pd(db, scripts, &pa);
				if(!pd.Create(pr->seed, pr->duel_flags, pr->start_lp,
							  pr->start_hand, pr->draw_count, aerr) ||
				   !pd.Setup(*pr, aerr)) {
					std::printf("  !! %s: duel cannot be "
													"initialised: %s\n",
								f.c_str(), aerr.c_str());
				} else {
					if(opt.stop_gc)
						pd.SetLuaGc(false);
					EnumOptions eo;
					eo.dedup_by_code = true;
					eo.max_subsets = opt.max_subsets;
					eo.db = &db;
					NrpaRun run;
					size_t unknown =
						LiftPolicyRun(pd, pa, *pr, opt.target_player, SIZE_MAX,
									  eo, repertoire, target, run, recipes);
					if(!run.steps.empty()) {
						total_steps += run.steps.size();
						total_unknown += unknown;
						std::printf("  %-44s %4zu "
															"multi-choice "
															"decisions, %2zu "
															"unidentified\n",
									fs::path(f).filename().string().c_str(),
									run.steps.size(), unknown);
						out.push_back(std::move(run));
					} else {
						std::printf("  %-44s no decision "
															"recorded\n",
									fs::path(f).filename().string().c_str());
					}
				}
			}
			pa.Shutdown();
		}).join();
	}
	if(out.empty()) {
		std::printf("  !! empty adaptation: no sequence recorded\n");
		return;
	}
	// INSTRUMENT BEFORE CALIBRATING: does the policy really reproduce the corpus's
	// sequences after the passes? On a virgin policy the agreement equals the mean
	// of -log(number of legal choices); if it does not rise, the mechanism is inert
	// and no run is needed to say so.
	std::printf("  adaptation: %zu line(s), %zu decisions (%zu "
					"unidentified), %.0f ms\n",
				out.size(), total_steps, total_unknown, MsSince(t0));
	// SATURATION CURVE, computed before any run (instrument BEFORE calibrating).
	// `p(chosen)` is the mean probability the policy gives to the moves the
	// solutions played: on a virgin policy it equals the mean of 1/(number of legal
	// choices), and if it does not rise with the passes the mechanism is inert, and
	// there is no point paying for a 600 s run to find out. The number of weights
	// says how many distinct moves the corpus touches.
	SearchConfig defaults;
	{
		size_t ctx_span = 0;
		std::map<uint16_t, size_t> by_ctx;
		for(const NrpaRun& r : out)
			for(const PolicyStep& s : r.steps)
				++by_ctx[s.ctx];
		ctx_span = by_ctx.size();
		std::printf("  distinct contexts: %zu (target cards placed x "
							"hand left)\n", ctx_span);
	}
	// THE CEILING, measured before any curve. Two steps presenting the SAME set of
	// legal moves in the SAME context are indistinguishable to a policy of this
	// family; if the corpus plays different moves there, the gap is IRREDUCIBLE. So
	// we return the fraction of steps that play the majority move of their decision
	// point: that is what a perfect TABLE over those points would reach. It is an
	// upper bound (the model with weights SHARED between decision points does not
	// necessarily reach it), but a bound that says at once whether the corpus
	// contradicts itself or whether it is the learning that stalls.
	{
		size_t g0 = 0, g1 = 0;
		const double c0 = CorpusCoherence(out, false, &g0);
		const double c1 = CorpusCoherence(out, true, &g1);
		std::printf("  family ceiling: %.1f%% without context (%zu "
							"distinct decision points) -> %.1f%% with (%zu)\n",
					100.0 * c0, g0, 100.0 * c1, g1);
	}
	{
		NrpaPolicy probe;
		NrpaResidual res;
		AdaptCorpus(probe, &res, out, opt.adapt_passes, defaults.nrpa_alpha,
					defaults.nrpa_bias_known, kReportHintBias,
					static_cast<float>(opt.ctx_shrink), kReportTemp);
		std::printf("  kept: %u pass(es), shrink k=%.1f%s, %zu global "
							"weights + %zu contextual\n",
					opt.adapt_passes, opt.ctx_shrink,
					opt.ctx_shrink < 0 ? " (niveau contextuel "
															"ETEINT)" : "",
					probe.size(), res.size());
		if(!opt.adapt_passes)
			std::printf("  (--adapt-passes 0: recording done, "
									"adaptation DISABLED - the A/B control "
									"arm)\n");
	}
}

// OPPONENT TEST (--fire). The guard was a static proxy ("a counter is available
// at every window"); this mode plays the threat FOR REAL: the card is added to
// the opponent's hand, the opponent ACTIVATES it at every window where it is
// legal (one attempt per window), and the rooted search must close the board
// back up from the post-injection state, either the complete board (a free
// answer, Crystal Wing) or the board without the sacrificed card (--fire-spare:
// answering with Zalen consumes Junk Signal, per the player's ruling).
//
// Aligning the replay on the AUGMENTED duel: adding a playable card opens NEW
// opponent windows (the core only asks when a legal answer exists), so the
// recorded answers shift. We therefore replay PER PLAYER: our answers in file
// order; at a RECORDED opponent window (where answers other than the drawn card
// exist), the recorded pass; at a NEW window (where the drawn card is the ONLY
// chainable one, i.e. exactly 2 choices, it plus the pass), a synthetic pass.
// The discovery pass must reach the board with 0 retries, which is the
// alignment proof, required before any injection.
//
// The guard is deliberately ABSENT from the closing search: the threat has just
// been spent (a single copy added). --resolve, --no-activate and --no-chain
// remain.
void RunFireTest(Duel& duel, Arena& arena, const Replay& yrp,
				 const Options& opt, const LineResult& ref, CardDB& db,
				 ScriptProvider& scripts, const LineConstraints& cons) {
	std::printf("\n=== opponent test: the opponent PLAYS the threat "
					"(--fire) ===\n");
	uint32_t fire_code = 0;
	if(!ResolveCard(opt.fire_spec, db, "--fire", fire_code))
		return;
	std::vector<uint32_t> spare_codes;
	for(const std::string& spec : opt.fire_spare_specs) {
		uint32_t c = 0;
		if(!ResolveCard(spec, db, "--fire-spare", c))
			return;
		spare_codes.push_back(c);
	}

	// The mechanism report, on this path too. The per-window config comes from the
	// same ApplyMechanisms, so a lever whose dependency this mode does not serve
	// (recipe graph, landmarks, ladder) is named INERT once here instead of being
	// silently dropped in every window.
	{
		SearchConfig probe;
		ApplyMechanisms(opt, probe);
		probe.resolve_min = cons.resolve_min;
		ReportMechanisms(probe, "fire");
	}
	if(!ref.have_target) {
		std::printf("!! no target board (the line does not reach the "
							"end of turn 1)\n");
		return;
	}
	const auto con = static_cast<uint8_t>(opt.target_player);
	std::printf("  threat: %s - added to the opponent hand, PLAYED at "
					"every legal window\n", db.Name(fire_code).c_str());
	// No-chain specific to the continuation (--fire-no-chain): staging a precise
	// answer. Must outlive the search threads.
	std::vector<uint32_t> fire_no_chain;
	for(const std::string& spec : opt.fire_no_chain_specs) {
		uint32_t c = 0;
		if(!ResolveCard(spec, db, "--fire-no-chain", c))
			return;
		fire_no_chain.push_back(c);
		std::printf("  continuation: %s NEVER chains "
							"(--fire-no-chain)\n",
					db.Name(c).c_str());
	}

	while(arena.Depth() > 1)
		arena.Pop();
	if(arena.Depth() == 0)
		arena.Push();
	arena.Restore();

	// --- 1. The two goal boards: complete, and without the sacrificial card.
	BoardKey target;
	{
		size_t at = 0;
		at += Advance(duel, yrp, at, ref.target_at);
		target = ComputeBoardKey(duel, con);
		arena.Restore();
	}
	// Alternative goals: the target board MINUS each non-empty SUBSET of the
	// sacrificial cards. A line that answers by spending only Junk Signal must
	// match, as must a line that also loses the building piece that spending
	// broke.
	std::vector<BoardKey> alts;
	if(!spare_codes.empty()) {
		if(spare_codes.size() > 3) {
			std::printf("!! --fire-spare: at most 3 cards\n");
			return;
		}
		const size_t n = spare_codes.size();
		for(size_t mask = 1; mask < (size_t(1) << n); ++mask) {
			auto mz = ref.target_self.mzone;
			auto sz = ref.target_self.szone;
			bool all_removed = true;
			std::string names;
			for(size_t i = 0; i < n; ++i) {
				if(!(mask & (size_t(1) << i)))
					continue;
				bool removed = false;
				for(auto* zone : { &mz, &sz }) {
					for(auto& c : *zone)
						if(c.present &&
						   db.Canonical(c.Code()) == spare_codes[i]) {
							c.present = false;
							removed = true;
							break;
						}
					if(removed)
						break;
				}
				if(!removed) {
					all_removed = false;
					break;
				}
				if(!names.empty())
					names += " + ";
				names += db.Name(spare_codes[i]);
			}
			if(!all_removed)
				continue;
			alts.push_back(MakeBoardKey(mz, sz, db));
			std::printf("  alternative goal: the board WITHOUT %s\n", names.c_str());
		}
		if(alts.empty())
			std::printf("  !! --fire-spare: none of the cards is "
									"on the target board - alternative goals "
									"ignored\n");
	}
	const bool have_alt = !alts.empty();

	// --- 1b. WATCHABLE variant (--fire-bake): the drawn card is BAKED into the
	// header, inserted into the opponent's deck where the pseudo-shuffle serves
	// the hand (the tail of the list: the core draws from the top). Since
	// start_hand is shared between the two players, the card takes the place of
	// the last card of the opponent's original hand (moved to the deck): the
	// duel differs from the default mode by ONE opponent hand card, and the
	// alignment proof decides whether it stays replayable. In exchange, the
	// replays produced replay FROM THEIR FILE, so EDOPro can watch them with no
	// flag.
	Replay baked;
	const Replay* fyrp = &yrp;
	const std::vector<uint32_t> fire_hand_v{ fire_code };
	const std::vector<uint32_t>* extra = &fire_hand_v;
	const uint8_t oppo = static_cast<uint8_t>(1 - opt.target_player);
	if(opt.fire_bake) {
		CopyReplayHeader(yrp, baked);
		bool in_hand = false;
		// Attempt 1: tail of the list (the top of the deck); attempt 2: head.
		for(int attempt = 0; attempt < 2 && !in_hand; ++attempt) {
			baked.decks = yrp.decks;
			if(baked.decks.size() <= oppo)
				baked.decks.resize(oppo + 1);
			auto& main = baked.decks[oppo].main;
			if(attempt == 0)
				main.push_back(fire_code);
			else
				main.insert(main.begin(), fire_code);
			auto got = OpeningHand(baked, oppo, db, scripts, opt.arena_mb);
			for(uint32_t c : got)
				if(c == fire_code)
					in_hand = true;
		}
		if(!in_hand) {
			std::printf("!! --fire-bake: the card does not reach "
									"the opponent hand through the deal "
									"(probe) - abandoned\n");
			return;
		}
		std::printf("  --fire-bake: %s baked into the header "
							"(opponent deck), verified in hand by probe;\n"
							"  the replays produced play in EDOPro as they "
							"are.\n",
					db.Name(fire_code).c_str());
		fyrp = &baked;
		extra = nullptr;
	}

	// --- 2. The reference's repertoire guides the closing.
	std::vector<PlanStep> plan;
	{
		EnumOptions eo;
		eo.dedup_by_code = true;
		eo.max_subsets = opt.max_subsets;
		eo.db = &db;
		// The return value is the number of UNIDENTIFIED steps. The three other
		// sites print it; here it was thrown away, and a plan with 90 % holes acted
		// as a repertoire as if it were complete, with the windows' failure then
		// blamed on the search.
		const size_t unknown = LiftPlan(duel, arena, yrp, opt.target_player,
										ref.target_at, eo, plan);
		std::printf("  recovery repertoire: %zu step(s), %zu "
							"unidentified%s\n", plan.size(), unknown,
					(plan.size() && unknown * 2 > plan.size())
						? "   <-- the repertoire is "
												"mostly blind"
						: "");
		arena.Restore();
	}
	// Prior by replay (--prior): INITIAL policy of the per-window searches. The
	// measured wall of the early windows is the re-derivation of the three rips
	// from the post-injection state, which is exactly what the corpus encodes.
	// Must outlive the search threads.
	// Adaptation replay (--adapt): same corpus, same injection point, a
	// discriminative signal instead of a per-move bonus.
	std::vector<NrpaRun> adapt_runs;
	BuildAdaptRuns(opt, db, scripts, plan, target, adapt_runs);

	// --- 3. Labelling per player: the augmented replay cannot consume the flat
	// list (the new windows shift everything).
	std::vector<std::vector<uint8_t>> ours, theirs;
	{
		size_t used = 0;
		int pplayer = -1;
		bool retry = false;
		bool done = false;
		while(!done) {
			int st = duel.Process();
			for(const Message& m : duel.Messages()) {
				if(m.type == MSG_RETRY)
					retry = true;
				if(IsPrompt(m.type))
					pplayer = m.size ? m.data[0] : -1;
			}
			if(retry)
				break;
			if(st == OCG_DUEL_STATUS_AWAITING) {
				if(used >= ref.target_at || used >= yrp.responses.size())
					break;
				if(pplayer == opt.target_player)
					ours.push_back(yrp.responses[used]);
				else
					theirs.push_back(yrp.responses[used]);
				duel.SetResponse(yrp.responses[used]);
				++used;
			} else if(st != OCG_DUEL_STATUS_CONTINUE)
				done = true;
		}
		arena.Restore();
		if(retry) {
			std::printf("!! labelling: MSG_RETRY on the baseline "
									"replay - abandoned\n");
			return;
		}
		std::printf("  baseline: %zu answers of ours, %zu opponent "
							"passes, board at answer %zu\n",
					ours.size(), theirs.size(), ref.target_at);
	}

	// --- 4. Discovery of the injection windows on the augmented duel, with an
	// alignment proof (board reached, 0 retries).
	struct FireWindow {
		size_t our_at = 0;      // our decisions already played at the window
		uint32_t summons = 0, actions = 0, turns = 0;
		uint64_t resolved = 0;
		std::vector<std::vector<uint8_t>> prefix;   // answers already sent
		std::vector<uint8_t> inject;   // the opponent answer that PLAYS the card
		bool fresh = false;            // NEW window (opened by the addition)
		// State of the chain at the window: 0 = EMPTY chain (the drawn card
		// STARTS a chain, the real threat); otherwise the code of the effect on
		// top (the card would be chained over it).
		uint32_t over = 0;
	};
	std::vector<FireWindow> windows;
	bool aligned = false;
	std::thread([&] {
		Arena fa;
		std::string err;
		if(!fa.Init(opt.arena_mb << 20, 0, err))
			return WorkerAbort("arena (opponent test)", err);
		{
			Duel fd(db, scripts, &fa);
			if(fd.Create(fyrp->seed, fyrp->duel_flags, fyrp->start_lp,
						 fyrp->start_hand, fyrp->draw_count, err) &&
			   fd.Setup(*fyrp, err, extra, oppo)) {
				if(opt.stop_gc)
					fd.SetLuaGc(false);
				EnumOptions oeo;   // OPPONENT enumeration: raw, without our filters
				oeo.dedup_by_code = true;
				oeo.max_subsets = opt.max_subsets;
				oeo.db = &db;
				std::vector<std::vector<uint8_t>> prefix;
				size_t oi = 0, ti = 0;
				uint8_t ptype = 0;
				int pplayer = -1;
				std::vector<uint8_t> ppayload;
				uint32_t summons = 0, actions = 0, turns = 0;
				uint64_t resolved = 0;
				// Depth of the current chain and effect on top: that is what
				// distinguishes an OPEN window (the threat starts a chain) from a
				// response window in mid-resolution.
				uint32_t chain_depth = 0, chain_top = 0;
				bool retry = false;
				auto scan = [&] {
					for(const Message& m : fd.Messages()) {
						switch(m.type) {
						case MSG_RETRY: retry = true; break;
						case MSG_NEW_TURN: ++turns; break;
						case MSG_SUMMONING:
						case MSG_SPSUMMONING: {
							++summons;
							++actions;
							if(!cons.resolve_min.empty() && m.size >= 4) {
								uint32_t c = 0;
								std::memcpy(&c, m.data, 4);
								if(c) {
									const uint32_t sc = db.Canonical(c);
									for(size_t k = 0; k < cons.resolve_min.size(); ++k)
										if(cons.resolve_min[k].on_summon &&
										   cons.resolve_min[k].code == sc)
											resolved += 1ull << (16 * k);
								}
							}
							break;
						}
						case MSG_FLIPSUMMONING: ++actions; break;
						case MSG_CHAINING: {
							++actions;
							++chain_depth;
							if(m.size >= 4)
								std::memcpy(&chain_top, m.data, 4);
							if(!cons.resolve_min.empty() && m.size >= 4) {
								uint32_t c = 0;
								std::memcpy(&c, m.data, 4);
								c = db.Canonical(c);
								const uint32_t loc = ChainingLocation(m.data, m.size);
								for(size_t k = 0; k < cons.resolve_min.size(); ++k)
									if(!cons.resolve_min[k].on_summon &&
									   cons.resolve_min[k].code == c &&
									   (!cons.resolve_min[k].zones ||
										(loc & cons.resolve_min[k].zones)))
										resolved += 1ull << (16 * k);
							}
							break;
						}
						case MSG_CHAIN_END:
							chain_depth = 0;
							chain_top = 0;
							break;
						default: break;
						}
						if(IsPrompt(m.type)) {
							ptype = m.type;
							pplayer = m.size ? m.data[0] : -1;
							ppayload.assign(m.data, m.data + m.size);
						}
					}
				};
				auto send = [&](const std::vector<uint8_t>& r) {
					fd.SetResponse(r);
					prefix.push_back(r);
				};
				for(;;) {
					int st = fd.Process();
					scan();
					if(retry || turns >= 2)
						break;
					if(st == OCG_DUEL_STATUS_AWAITING) {
						if(pplayer == opt.target_player) {
							if(oi >= ours.size())
								break;   // the board is done: end of the pass
							send(ours[oi++]);
							continue;
						}
						// Opponent window: is the drawn card playable there?
						auto choices = Enumerate(
							ptype, ppayload.data(),
							static_cast<uint32_t>(ppayload.size()), oeo);
						int fire_at = -1;
						for(size_t i = 0; i < choices.size(); ++i)
							if(choices[i].card &&
							   db.Canonical(choices[i].card) == fire_code) {
								fire_at = static_cast<int>(i);
								break;
							}
						// NEW iff the drawn card is the only chainable one (it plus
						// the pass). Otherwise the window existed in the recording and
						// its recorded pass applies.
						const bool fresh =
							fire_at >= 0 && choices.size() == 2;
						// --fire-open: the threat must START a chain, so windows in
						// mid-resolution are set aside.
						if(fire_at >= 0 &&
						   (!opt.fire_open || chain_depth == 0)) {
							FireWindow w;
							w.our_at = oi;
							w.summons = summons;
							w.actions = actions;
							w.turns = turns;
							w.resolved = resolved;
							w.prefix = prefix;
							w.inject = choices[static_cast<size_t>(fire_at)].response;
							w.fresh = fresh;
							w.over = chain_depth ? chain_top : 0;
							windows.push_back(std::move(w));
						}
						if(fresh) {
							send(choices.back().response);   // synthetic pass
						} else if(ti < theirs.size()) {
							send(theirs[ti++]);              // recorded pass
						} else if(!choices.empty()) {
							send(choices.back().response);
						} else {
							std::vector<uint8_t> def;
							if(!DefaultResponse(ptype, ppayload.data(),
												static_cast<uint32_t>(ppayload.size()),
												def))
								break;
							send(def);
						}
					} else if(st != OCG_DUEL_STATUS_CONTINUE)
						break;
				}
				if(!retry && oi >= ours.size()) {
					BoardKey fin = ComputeBoardKey(fd, con);
					aligned = fin == target;
				}
			} else {
				std::printf("  !! augmented duel cannot be "
											"initialised: %s\n",
							err.c_str());
			}
		}
		ReportPoison("test adverse", fa);
		fa.Shutdown();
	}).join();

	if(!aligned) {
		std::printf("!! the discovery pass does not reach the board "
							"on the augmented duel\n"
							"   (per-player replay misalignment) - no "
							"injection attempted.\n");
		return;
	}
	std::printf("  alignment PROVEN: the baseline redoes the board on the "
					"augmented duel.\n");
	std::printf("  %zu window(s) where %s is playable%s", windows.size(),
				db.Name(fire_code).c_str(),
				opt.fire_open ? " EN OUVERTURE DE CHAINE "
												"(--fire-open)" : "");
	{
		size_t open_n = 0;
		for(const FireWindow& w : windows)
			open_n += w.over ? 0 : 1;
		std::printf(" (%zu on an empty chain, %zu on top of an "
							"effect)\n",
					open_n, windows.size() - open_n);
	}
	if(windows.empty())
		return;

	// --- 5. One injection per window, rooted search towards the goal(s).
	std::printf("\n--- injections: %.0f s of search per window ---\n",
				opt.fire_ms / 1000.0);
	uint64_t base_seed = opt.seed;
	if(!base_seed) {
		base_seed = static_cast<uint64_t>(
			std::chrono::high_resolution_clock::now().time_since_epoch().count());
		if(!base_seed)
			base_seed = 1;
	}
	struct FireResult {
		bool tried = false, converted = false, alt = false;
		uint32_t b = 0, a = 0, d = 0;
		uint32_t best_overlap = 0;
		uint64_t rollouts = 0;
	};
	std::vector<FireResult> results(windows.size());
	std::vector<Solution> all_sols;
	std::mutex mx;
	std::atomic<size_t> next{ 0 };
	unsigned threads = opt.threads
		? opt.threads
		: (std::max)(1u, std::thread::hardware_concurrency());
	unsigned nw = (std::min<unsigned>)(threads,
									   static_cast<unsigned>(windows.size()));
	std::vector<std::thread> pool;
	for(unsigned t = 0; t < nw; ++t) {
		pool.emplace_back([&] {
			Arena fa;
			std::string err;
			if(!fa.Init(opt.arena_mb << 20, 0, err))
				return WorkerAbort("arena (--fire "
												"continuation)", err);
			{
				Duel fd(db, scripts, &fa);
				if(fd.Create(fyrp->seed, fyrp->duel_flags, fyrp->start_lp,
							 fyrp->start_hand, fyrp->draw_count, err) &&
				   fd.Setup(*fyrp, err, extra, oppo)) {
					if(opt.stop_gc)
						fd.SetLuaGc(false);
					fa.Push();   // starting state of the augmented duel
					for(;;) {
						size_t w = next.fetch_add(1);
						if(w >= windows.size())
							break;
						const FireWindow& W = windows[w];
						fa.Restore();
						// Blind replay of the prefix: the bytes are exact for THIS
						// duel (they come from the discovery pass).
						bool retry = false;
						size_t used = 0;
						while(used < W.prefix.size() && !retry) {
							int st = fd.Process();
							for(const Message& m : fd.Messages())
								if(m.type == MSG_RETRY)
									retry = true;
							if(st == OCG_DUEL_STATUS_AWAITING)
								fd.SetResponse(W.prefix[used++]);
							else if(st != OCG_DUEL_STATUS_CONTINUE)
								break;
						}
						if(retry || used < W.prefix.size()) {
							std::lock_guard<std::mutex> lk(mx);
							std::printf("  window %2zu: !! "
																	"prefix not "
																	"replayable "
																	"(%zu/%zu)\n", w, used,
										W.prefix.size());
							continue;
						}
						// PROCESS the prefix's last answer: advance to the next
						// prompt (the firing window) BEFORE posting the injection.
						// Without that, SetResponse(inject) OVERWRITES the pending
						// answer: the injection is played at the previous prompt, the
						// search starts from a state shifted by one answer, and the
						// assembled path does not replay (measured: 16/16 MSG_RETRY at
						// write time, a "window shift" divergence at +3 from the
						// injection).
						{
							int st = 0;
							do {
								st = fd.Process();
								for(const Message& m : fd.Messages())
									if(m.type == MSG_RETRY)
										retry = true;
							} while(st == OCG_DUEL_STATUS_CONTINUE && !retry);
							if(retry || st != OCG_DUEL_STATUS_AWAITING) {
								std::lock_guard<std::mutex> lk(mx);
								std::printf("  window %2zu: !! "
																			"the firing window "
																			"does not open on "
																			"replay\n", w);
								continue;
							}
						}
						// THE INJECTION: the opponent plays the card. Validity check
						// BEFORE the search: an answer the core rejects would kill
						// every rollout on its first step (millions of instant deaths,
						// best 0/8) while passing for a game-level infeasibility.
						// Arena Push/Pop: the search requires the "answer posted, not
						// processed" convention, so the check must consume nothing.
						fd.SetResponse(W.inject);
						bool inj_retry = false;
						{
							fa.Push();
							int st = 0;
							uint8_t rtype = 0;
							std::vector<uint8_t> rpayload;
							do {
								st = fd.Process();
								for(const Message& m : fd.Messages()) {
									if(m.type == MSG_RETRY)
										inj_retry = true;
									if(IsPrompt(m.type)) {
										rtype = m.type;
										rpayload.assign(m.data,
														m.data + m.size);
									}
								}
							} while(st == OCG_DUEL_STATUS_CONTINUE &&
									!inj_retry);
							// The decisive DIAGNOSIS: what can be answered to the
							// threat? The choices enumerated at the first prompt after
							// the injection, with the muzzled ones marked.
							if(!inj_retry && st == OCG_DUEL_STATUS_AWAITING &&
							   rtype) {
								EnumOptions reo;
								reo.dedup_by_code = true;
								reo.max_subsets = opt.max_subsets;
								reo.db = &db;
								auto ropts = Enumerate(
									rtype, rpayload.data(),
									static_cast<uint32_t>(rpayload.size()),
									reo);
								std::string names;
								for(const auto& c : ropts) {
									if(!c.card)
										continue;
									if(!names.empty())
										names += ", ";
									names += db.Name(db.Canonical(c.card));
									for(uint32_t nc : fire_no_chain)
										if(db.Canonical(c.card) == nc)
											names += " [muselee]";
								}
								std::lock_guard<std::mutex> lk(mx);
								std::printf("  window %2zu: "
																			"answers to the "
																			"threat: %s\n", w,
											names.empty() ? "(passer seulement)"
														  : names.c_str());
							}
							fa.Pop();
						}
						if(inj_retry) {
							std::lock_guard<std::mutex> lk(mx);
							std::printf("  window %2zu (dec. "
																	"%3zu, summon %2u): "
																	"injection REJECTED "
																	"by the core "
																	"(activation illegal "
																	"here)\n",
										w, W.our_at, W.summons);
							continue;
						}
						SearchConfig fcfg;
						// ONE SINGLE WIRING POINT, here too. This config used to be hand-wired
						// field by field, which left every mechanism absent from that list
						// silently inert under --fire while the report said nothing.
						ApplyMechanisms(opt, fcfg);
						fcfg.max_decisions = FinisherDepth(
							static_cast<uint32_t>(ref.target_at * 3 / 2 + 32),
							W.prefix.size());
						fcfg.max_actions = 0;
						fcfg.time_limit_ms = opt.fire_ms;
						fcfg.max_nodes = 50000000;
						fcfg.max_solutions = 4;
						fcfg.enumeration.dedup_by_code = true;
						fcfg.enumeration.max_subsets = opt.max_subsets;
						fcfg.enumeration.db = &db;
						if(!cons.no_activate.empty())
							fcfg.enumeration.no_activate = &cons.no_activate;
						// NO GLOBAL no_chain here: the player's rule is "never negate
						// OUR OWN cards", and it assumed solo play, where every chain
						// answers our actions. Chaining onto the REAL threat is the
						// guards' job (measured: with the filter, answering with
						// Crystal Wing was forbidden at enumeration and no window
						// converted to a complete board). --fire-no-chain, on the
						// other hand, does apply: it stages a precise answer.
						if(!fire_no_chain.empty())
							fcfg.enumeration.no_chain = &fire_no_chain;
						fcfg.resolve_min = cons.resolve_min;
						// GUARD ABSENT: the threat has just been spent.
						fcfg.initial_summons = W.summons;
						fcfg.initial_turns = W.turns;
						fcfg.initial_resolved = W.resolved;
						fcfg.hint_cards = cons.hints;
						for(const ResolveReq& req : cons.resolve_min)
							if(std::find(fcfg.hint_cards.begin(),
										 fcfg.hint_cards.end(), req.code) ==
							   fcfg.hint_cards.end())
								fcfg.hint_cards.push_back(req.code);
						if(have_alt)
							fcfg.target_alts = &alts;
						// Prior: the policy starts knowing how to rip.
						// Adaptation replay: it starts knowing which move the
						// solution took at each junction.
						if(!adapt_runs.empty()) {
							fcfg.nrpa_adapt_runs = &adapt_runs;
						}
						fcfg.enumeration.canonical_zones = opt.canonical_zones;
						fcfg.goal_subset =
							opt.target_subset ||
							(!opt.target_specs.empty() && !opt.target_exact);
						Search fs(fd, fa, *fyrp, fcfg);
						fs.RunNrpa(target, plan,
								   base_seed + w * 0x9E3779B97F4A7C15ull + 1);
						const SearchStats& st = fs.Stats();
						FireResult r;
						r.tried = true;
						r.best_overlap = st.best_overlap;
						r.rollouts = st.rollout_count;
						const Solution* best = nullptr;
						for(const Solution& s : fs.Solutions())
							if(!best ||
							   std::tie(s.burned, s.actions, s.decisions) <
								   std::tie(best->burned, best->actions,
											best->decisions))
								best = &s;
						if(best) {
							r.converted = true;
							r.alt = best->alt;
							r.b = best->burned;
							r.a = best->actions;
							r.d = best->decisions +
								  static_cast<uint32_t>(W.prefix.size());
						}
						std::lock_guard<std::mutex> lk(mx);
						results[w] = r;
						// The target cards MISSING at the peak: that is what says
						// whether the answer consumes a board card (the alternative
						// goal must then spare it too).
						std::string miss;
						if(!r.converted) {
							size_t a = 0, b = 0;
							int shown = 0;
							while(b < target.codes.size() && shown < 3) {
								if(a < st.best_board.size() &&
								   st.best_board[a] == target.codes[b]) {
									++a;
									++b;
								} else if(a < st.best_board.size() &&
										  st.best_board[a] < target.codes[b]) {
									++a;
								} else {
									if(!miss.empty())
										miss += ", ";
									miss += db.Name(target.codes[b]);
									++shown;
									++b;
								}
							}
							if(!miss.empty())
								miss = "  manque : " + miss;
						}
						const std::string wpos =
							W.over ? "over " + db.Name(W.over)
								   : std::string("OUVERTE");
						std::printf("  window %2zu (dec. "
															"%3zu, summon %2u, "
															"%s): %s  [%llu "
															"rollouts, best "
															"%u/%zu]%s\n", w,
									W.our_at, W.summons, wpos.c_str(),
									r.converted
										? (r.alt ? "CONVERTED (board "
																						"WITHOUT the "
																						"sacrificed card(s))"
												 : "CONVERTIE (board "
																								"COMPLET)")
										: "pas convertie",
									(unsigned long long)r.rollouts,
									st.best_overlap, target.codes.size(),
									miss.c_str());
						bool self_checked = false;
						for(Solution s : fs.Solutions()) {
							std::vector<std::vector<uint8_t>> full = W.prefix;
							full.push_back(W.inject);
							full.insert(full.end(), s.responses.begin(),
										s.responses.end());
							// SELF-CHECK of the first solution: replay the assembled
							// path from scratch on THIS duel and locate any
							// divergence. A path that does not replay here will not
							// be written either.
							if(!self_checked) {
								self_checked = true;
								fa.Restore();
								size_t fed = 0;
								bool sc_retry = false;
								uint8_t sc_ptype = 0;
								int sc_player = -1;
								std::vector<uint8_t> sc_payload;
								while(fed < full.size() && !sc_retry) {
									int st2 = fd.Process();
									for(const Message& m : fd.Messages()) {
										if(m.type == MSG_RETRY)
											sc_retry = true;
										if(IsPrompt(m.type)) {
											sc_ptype = m.type;
											sc_player = m.size ? m.data[0] : -1;
											sc_payload.assign(m.data,
															  m.data + m.size);
										}
									}
									if(st2 == OCG_DUEL_STATUS_AWAITING)
										fd.SetResponse(full[fed++]);
									else if(st2 != OCG_DUEL_STATUS_CONTINUE)
										break;
								}
								if(sc_retry || fed < full.size()) {
									// Is the rejected answer among the choices enumerated
									// COLD at this prompt? Yes = the STATE diverges (same
									// prompt, different contents); no = the answer comes
									// from ANOTHER prompt (window shift).
									// AUTRE prompt (decalage de fenetres).
									EnumOptions deo;
									deo.dedup_by_code = true;
									deo.max_subsets = opt.max_subsets;
									deo.db = &db;
									auto cold = Enumerate(
										sc_ptype, sc_payload.data(),
										static_cast<uint32_t>(sc_payload.size()),
										deo);
									const std::vector<uint8_t>& bad =
										full[fed ? fed - 1 : 0];
									bool listed = false;
									for(const auto& c : cold)
										if(c.response == bad) {
											listed = true;
											break;
										}
									std::printf("  window %2zu: !! "
																					"self-check DIVERGED "
																					"at answer %zu/%zu "
																					"(prefix %zu, "
																					"injection %zu) - "
																					"prompt %u player "
																					"%d, %zu cold "
																					"choices, path "
																					"answer %s\n", w,
												fed, full.size(),
												W.prefix.size(),
												W.prefix.size() + 1,
												sc_ptype, sc_player,
												cold.size(),
												listed ? "LISTED (divergent "
																										"state)"
													   : "NOT LISTED (window "
																											"offset)");
								}
							}
							s.responses = std::move(full);
							s.decisions += static_cast<uint32_t>(W.prefix.size());
							s.actions += W.actions;
							all_sols.push_back(std::move(s));
						}
					}
				} else {
					WorkerAbort("duel (continuation "
													"--fire)", err);
				}
			}
			ReportPoison("continuation --fire", fa);
			fa.Shutdown();
		});
	}
	for(auto& t : pool)
		t.join();

	// --- 6. Global verdict and writing (verification included, guard omitted
	// since the threat is spent; --resolve/--no-activate re-checked).
	size_t converted = 0, full_n = 0, alt_n = 0;
	for(const FireResult& r : results) {
		if(!r.converted)
			continue;
		++converted;
		if(r.alt)
			++alt_n;
		else
			++full_n;
	}
	std::printf("\n=== --fire verdict: %zu window(s) out of %zu converted "
					"(%zu full board, %zu without the sacrificed card) ===\n",
				converted, windows.size(), full_n, alt_n);
	if(converted < windows.size())
		std::printf("  unconverted windows are NOT proofs of absence: "
							"%0.f s of sampling\n"
							"  budget per window - go deeper with --fire-ms.\n", opt.fire_ms / 1000.0);
	if(!all_sols.empty()) {
		LineConstraints fcons = cons;
		fcons.guard.clear();
		fcons.guard_after = 0;
		fcons.guard_opp_hand_release = -1;
		if(opt.fire_bake)
			std::printf("\n  replays of the recoveries (header "
									"BAKED: replayable from their own file, "
									"EDOPro included):\n");
		else
			std::printf("\n  replays of the recoveries "
									"(replayable in judge mode with "
									"--opp-hand \"%u\"):\n", fire_code);
		WriteSolutions(all_sols, *fyrp, target, opt, db, scripts, opt.outdir,
					   fcons, extra, have_alt ? &alts : nullptr);
	}
	arena.Restore();
}

// TRANSPLANTATION: rebuild the reference board from ANOTHER deck.
//
// The problem is no longer to improve a known line but to rebuild one: the
// recorded answers designate nothing in a duel whose deck, hand and seed all
// differ. What is carried over is the line's INTENT (LiftPlan), used as a visit
// order.
// The balance model is LENT to the search (cfg.balance) for the
// re-serialisation from the frontier, so it must outlive the block that builds
// it. A process-wide static, like the landmark graph.
static BalanceModel g_balance_model;
static bool g_balance_armed = false;

// The RESULT of one call, for the internal loop: what the next round needs to
// know, i.e. how many solutions, and which joint line to re-inject. Filled on
// the POSTED target path (the mode the rounds use).
struct TransplantOutcome {
	size_t solutions = 0;
	uint32_t best_overlap = 0;
	uint32_t joint_rp = 0, joint_overlap = 0;
	std::string joint_file;   // empty: nothing written this round
};

// WHAT PERSISTS BETWEEN ROUNDS (--carry, full Go-Explore). The global archive
// (finisher included under --archive-fin) and the merged NRPA policy outlive
// the call: the next round SEEDS its rollout workers with those cells and
// starts its finisher policy from those weights. The paths stay valid from one
// round to the next because the starting template is THE SAME (same seed, same
// hand, same duel); the property same_gabarit checks for the approaches is
// structural here.
struct RoundCarry {
	std::unordered_map<uint64_t, ArchiveEntry> archive;
	NrpaPolicy policy;
	unsigned policy_workers = 0;
};

void RunTransplantSolve(Duel& duel, const Replay& ref_yrp, const Replay& start_yrp,
						const Options& opt, Arena& arena, const LineResult& ref,
						CardDB& db, ScriptProvider& scripts, uint32_t patience,
						const LineConstraints& cons,
						TransplantOutcome* outres = nullptr,
						RoundCarry* carry = nullptr) {
	std::printf("\n=== transplanting the combo onto another deck ===\n");
	if(!cons.opp_hand.empty()) {
		std::printf("  opponent hand  : +%zu card(s) (--opp-hand):",
					cons.opp_hand.size());
		for(uint32_t c : cons.opp_hand)
			std::printf(" %s;", db.Name(c).c_str());
		std::printf("\n                   the replays produced only "
							"replay with the same --opp-hand\n");
	}
	while(arena.Depth() > 1)
		arena.Pop();
	if(arena.Depth() == 0)
		arena.Push();
	arena.Restore();

	const auto con = static_cast<uint8_t>(opt.target_player);

	// --- 1. The board to rebuild, and what it cost the reference.
	BoardKey target;
	uint32_t ref_actions = 0, ref_burned = 0;
	size_t ref_decisions = 0;
	{
		size_t at = 0;
		uint32_t acts = 0;
		at += Advance(duel, ref_yrp, at, ref.target_at, &acts);
		target = ComputeBoardKey(duel, con);
		ref_actions = acts;
		ref_decisions = at;
		ref_burned = duel.Count(con, LOCATION_GRAVE) + duel.Count(con, LOCATION_REMOVED);
		arena.Restore();
	}
	// --- 1b. Editing the target board: remove / require cards. We start from the
	// cards CAPTURED on the reference board (positions, materials and counters
	// included) and recompose the key; never a hand-made key.
	// The target ACTUALLY used, kept for the report. The "all the codes are there"
	// diagnostic block must NOT print `ref.target_self`, the TEMPLATE's board,
	// under the label "target:": under --no-ref that is not the target, and this is
	// exactly the instrument that has to show that a posted target has an EMPTY S/T
	// zone.
	std::vector<QueriedCard> posed_mz, posed_sz;
	bool posed = false;
	if(cons.AnyBoardEdit()) {
		// --target: a CLEAN slate. The reference's board does not enter, and that is
		// the difference between "editing the reference's target" and "posting a
		// target". Without it one needs a cascade of --board-remove, which depends on
		// what the reference had placed (hence on a reference).
		auto mz = cons.target_scratch ? std::vector<QueriedCard>{}
									  : ref.target_self.mzone;
		auto sz = cons.target_scratch ? std::vector<QueriedCard>{}
									  : ref.target_self.szone;
		bool edit_ok = true;
		if(cons.target_scratch && !cons.board_remove.empty()) {
			std::printf("!! --target and --board-remove are "
									"exclusive: the target board is already "
									"built from scratch\n");
			return;
		}
		for(uint32_t code : cons.board_remove) {
			bool found = false;
			for(auto* zone : { &mz, &sz }) {
				for(auto& c : *zone) {
					if(c.present && db.Canonical(c.Code()) == code) {
						c.present = false;
						found = true;
						break;
					}
				}
				if(found)
					break;
			}
			if(!found) {
				std::printf("!! --board-remove: %s is not on "
											"the target board\n", db.Name(code).c_str());
				edit_ok = false;
			}
		}
		for(const auto& [code, pos] : cons.board_add) {
			QueriedCard c;
			c.present = true;
			c.code = code;
			c.position = pos;
			mz.push_back(c);   // the additions are monsters, in the MZONE
		}
		if(!edit_ok)
			return;
		if(cons.target_scratch && mz.empty() && sz.empty()) {
			std::printf("!! --target: empty target, nothing to "
									"reach\n");
			return;
		}
		target = MakeBoardKey(mz, sz, db);
		posed_mz = mz;
		posed_sz = sz;
		posed = true;
		std::printf("\n--- target board %s ---\n",
					cons.target_scratch ? "POSTED (--target, "
															"no reference)"
										: "EDITED");
		for(const auto& c : mz)
			if(c.present)
				std::printf("      MZONE %9u  %-36.36s %s\n", c.Code(),
							db.Name(c.Code()).c_str(), PosName(c.position));
		for(const auto& c : sz)
			if(c.present)
				std::printf("      SZONE %9u  %-36.36s %s\n", c.Code(),
							db.Name(c.Code()).c_str(), PosName(c.position));
		if(target.mzone_count > 6)
			std::printf("  !! %u monsters required: MORE than the "
									"6 usable zones, target unreachable\n", target.mzone_count);
	}

	std::printf("  target         : %zu cards\n", target.entries.size());
	std::printf("  reference      : %u actions, %zu decisions, %u cards "
					"burned\n",
				ref_actions, ref_decisions, ref_burned);

	// --- 2. Feasibility. A board card must come from the player's main deck or
	// extra deck: there is no other source. If it is not there, the board is out of
	// reach and any search would be wasted time.
	{
		const Deck& deck = start_yrp.decks[opt.target_player];
		std::map<uint32_t, uint32_t> avail;
		for(const auto* list : { &deck.main, &deck.extra })
			for(uint32_t c : *list)
				++avail[db.Canonical(c)];

		std::map<uint32_t, uint32_t> need;
		for(uint32_t c : target.codes)
			++need[c];

		std::vector<std::pair<uint32_t, uint32_t>> missing;
		for(const auto& [code, n] : need) {
			uint32_t have = avail.count(code) ? avail[code] : 0;
			if(have < n)
				missing.emplace_back(code, n - have);
		}
		std::printf("\n--- feasibility: are the board's cards in this "
							"deck? ---\n");
		if(missing.empty()) {
			std::printf("  all %zu target board cards are "
									"present.\n",
						need.size());
		} else {
			for(const auto& [code, n] : missing)
				std::printf("  MANQUE %dx %9u  %s\n", n, code, db.Name(code).c_str());
			std::printf("\n  This deck does not hold the board's "
									"cards: the target is out of\n"
									"  reach whatever the line. Search "
									"cancelled.\n");
			return;
		}
	}

	// --- 2b. The cards the line BORROWED on the way. The board can be present in
	// the deck while the intermediates have disappeared: that is what makes the
	// plan inapplicable, and it is the real explanation of a failure.
	if(ref.have_target) {
		const Deck& deck = start_yrp.decks[opt.target_player];
		std::map<uint32_t, uint32_t> avail;
		for(const auto* list : { &deck.main, &deck.extra })
			for(uint32_t c : *list)
				++avail[db.Canonical(c)];

		std::map<uint32_t, uint32_t> engaged;
		for(const auto* zone : { &ref.target_self.grave_cards,
								 &ref.target_self.removed_cards,
								 &ref.target_self.mzone, &ref.target_self.szone })
			for(const auto& [code, n] : CodeCounts(*zone, db))
				engaged[code] += n;

		std::vector<std::pair<uint32_t, uint32_t>> gone;
		for(const auto& [code, n] : engaged) {
			uint32_t have = avail.count(code) ? avail[code] : 0;
			if(have < n)
				gone.emplace_back(code, n - have);
		}
		std::printf("\n--- cards the line borrows, absent from this "
							"deck ---\n");
		if(gone.empty()) {
			std::printf("  none: the starting deck can supply the "
									"%zu cards the line engages.\n", engaged.size());
		} else {
			for(const auto& [code, n] : gone)
				std::printf("  MANQUE %dx %9u  %s\n", n, code, db.Name(code).c_str());
			std::printf("\n  The board is reachable in principle, "
									"but the reference line went\n"
									"  through those cards: its plan cannot "
									"be followed as it stands,\n"
									"  another route is needed.\n");
		}

		// The opening hand decides everything: two similar decks with different hands
		// do not play the same game.
		std::printf("\n--- mains d'ouverture ---\n");
		std::printf("  reference :");
		for(const auto& c : ref.start_self.hand_cards)
			if(c.present)
				std::printf(" %s;", db.Name(db.Canonical(c.Code())).c_str());
		std::printf("\n  start     :");
		for(uint32_t code : OpeningHand(start_yrp, con, db, scripts, opt.arena_mb))
			std::printf(" %s;", db.Name(code).c_str());
		std::printf("\n");
	}

	// --- 3. Record the reference line as intents.
	std::vector<PlanStep> plan;
	size_t unknown = 0;
	{
		EnumOptions eo;
		eo.dedup_by_code = true;
		eo.max_subsets = opt.max_subsets;
		eo.db = &db;
		arena.Restore();
		auto t0 = Clock::now();
		unknown = LiftPlan(duel, arena, ref_yrp, opt.target_player, ref.target_at,
						   eo, plan);
		double ms = MsSince(t0);
		arena.Restore();
		std::printf("\n--- plan recorded from the reference line ---\n");
		std::printf("  etapes            : %zu  (%.0f ms)\n", plan.size(), ms);
		std::printf("  non identifiees   : %zu%s\n", unknown,
					unknown ? "   <-- that many holes in "
												"the guide" : "");
		std::map<uint8_t, size_t> by_type;
		for(const auto& s : plan)
			++by_type[s.prompt_type];
		for(const auto& [type, n] : by_type)
			std::printf("      %-24s %4zu\n", PromptName(type), n);

		// The line's opening says what the new deck must be able to reproduce. It is
		// the part of the plan that fails first.
		std::printf("\n  opening of the line:\n");
		size_t shown = 0;
		for(const auto& s : plan) {
			if(shown >= 18)
				break;
			// The structural steps (empty chain, zone choice) drown the point: we
			// only show what engages a card.
			if(s.prompt_type != MSG_SELECT_IDLECMD && s.prompt_type != MSG_SELECT_CARD)
				continue;
			uint32_t code = 0;
			if(std::sscanf(s.label.c_str(), "%*[^0-9]%u", &code) == 1 && code > 1000)
				std::printf("      %-14s %-18s %s\n", PromptName(s.prompt_type),
							s.label.c_str(), db.Name(db.Canonical(code)).c_str());
			else
				std::printf("      %-14s %s\n", PromptName(s.prompt_type),
							s.label.c_str());
			++shown;
		}
	}
	// GOAL-ONLY MODE: the repertoire is discarded AFTER being recorded and
	// printed. The recording stays, so the measurement says exactly WHAT IS BEING
	// REMOVED; a mechanism neutralised in silence cannot be judged.
	if(opt.no_plan) {
		std::printf("\n  --no-plan: the %zu steps above are SET "
							"ASIDE.\n  The NRPA policy starts uniform: no "
							"`known` bias, in the rollouts or in the\n"
							"  finisher. This is GOAL-ONLY mode - the "
							"reference now serves only as a duel\n"
							"  template%s.\n", plan.size(),
					opt.no_ref ? " (--no-ref: target "
													"board and line set "
													"aside too)"
							   : "");
		plan.clear();
	}
	if(plan.empty() && !opt.no_plan) {
		std::printf("\n  Empty plan: nothing to transplant.\n");
		return;
	}

	// Prior by replay (--prior): INITIAL weights of the rollouts' policy, recorded
	// from the solution corpus (each line on ITS own duel). Must outlive every
	// phase (rollouts, finisher).
	// RECIPE GRAPH. Created BEFORE the adaptation replay, so that the corpus's
	// summons feed it with OBSERVED recipes: real materials and zones, exception
	// routes included. That is what breaks the chicken-and-egg of the
	// observational graph without reading a single effect text. One graph for the
	// whole run: recipes are FACTS, and merging them can only enrich.
	// `static`: it outlives every search of the run.
	static RecipeGraph recipe_graph;
	// The subgoals derived from the material balance are computed HIGHER UP than
	// the construction of the `SearchConfig` (the operator seeding precedes the
	// search): they travel through here and are poured into `cfg` when it exists.
	// One single pouring point, as the wiring rule requires.
	std::vector<SearchConfig::SerialReq> serial_from_balance;
	// The QUOTA hosts: poured into cfg at the same point as serial_from_balance.
	std::vector<uint32_t> quota_hosts_wiring;
	if(opt.probe_repeat && cons.resolve_min.empty())
		std::printf("!! --probe-repeat without --summon-min or "
							"--resolve: no card to watch, the probe will stay "
							"MUTE\n");
	if(opt.recipes >= 0) {
		std::printf("  recipe graph: ACTIVE, weight %.2f%s\n",
					static_cast<float>(opt.recipes),
					opt.recipes == 0.0
						? "  (fed and MEASURED, does "
												"not enter the cost)"
						: "  (the recipe distance "
												"weighs in h)");
		std::vector<uint32_t> watched;
		for(const ResolveReq& rq : cons.resolve_min)
			watched.push_back(rq.code);
		// SEEDING FROM TEXT, without which the card never placed has no recipe
		// and its distance falls back to the floor, i.e. to the flat `h`.
		// plat.
		if(opt.seed_recipes && !start_yrp.decks.empty()) {
			const Deck& sd = start_yrp.decks[
				opt.target_player < static_cast<int>(start_yrp.decks.size())
					? opt.target_player : 0];
			const size_t n = SeedRecipesFromText(db, sd, target, recipe_graph,
												 opt.seed_cardinal, watched);
			std::printf("  seeded from card text: %zu recipe(s) "
									"posted, %zu known product(s)\n", n, recipe_graph.Products());
			ReportSeededDistances(target, recipe_graph, db, watched);
		}
		// SEEDING FROM THE DECLARED OPERATORS.
		if(opt.op_recipes && !start_yrp.decks.empty()) {
			const Deck& sd = start_yrp.decks[
				opt.target_player < static_cast<int>(start_yrp.decks.size())
					? opt.target_player : 0];
			std::vector<uint32_t> codes;
			for(const auto* l : { &sd.main, &sd.extra })
				for(uint32_t c : *l)
					codes.push_back(c);
			for(uint32_t c : target.codes)
				codes.push_back(c);
			ConstantTable kt;
			if(!kt.Load(scripts)) {
				std::printf("!! --op-recipes: no constant "
											"read (constant.lua missing from "
											"--scriptdir). The seeding would "
											"be EMPTY: it is skipped rather "
											"than declared active.\n");
			} else {
				OperatorTable tbl;
				tbl.Build(db, scripts, kt, codes);
				// (a) THE DECLARED RECIPES. `Fusion.AddProcMixN(c,...,24550676, 1,
				//     IsSetCard(SET_LUNALIGHT), 3)` carries CODES: no more English
				//     name to re-resolve, no more "dead route" deduced from a
				//     sentence.
				std::vector<uint32_t> owned;
				for(const auto* l : { &sd.main, &sd.extra })
					for(uint32_t c : *l)
						owned.push_back(db.Canonical(c));
				std::sort(owned.begin(), owned.end());
				owned.erase(std::unique(owned.begin(), owned.end()), owned.end());
				size_t nrec = 0, dead = 0;
				for(const auto& [c, co] : tbl.All()) {
					for(const DeclaredRecipe& rc : co.recipes) {
						if(rc.named.empty() && rc.setcode.empty())
							continue;
						std::vector<Requirement> mats;
						bool route_morte = false;
						for(const auto& [mc, n] : rc.named) {
							if(!std::binary_search(owned.begin(), owned.end(),
												   db.Canonical(mc))) {
								route_morte = true;   // dead route, unchanged
								break;
							}
							for(uint32_t k = 0; k < n; ++k)
								mats.push_back(Requirement{ db.Canonical(mc),
															kZoneAny, kReqCard, 1 });
						}
						if(route_morte) { ++dead; continue; }
						if(opt.seed_cardinal)
							for(const auto& [sc, n] : rc.setcode)
								mats.push_back(Requirement{
									static_cast<uint32_t>(sc), kZoneAny,
									kReqSetcode, static_cast<uint8_t>(n) });
						if(mats.empty())
							continue;
						recipe_graph.Observe(c, mats, /*primed=*/true);
						++nrec;
					}
				}
				// (b) THE MISSING NODE TYPE: a CODE one ACQUIRES.
				const std::vector<AcquirableCode> acq =
					AcquirableCodesOf(tbl, db, owned);
				// THE SOURCE'S ZONE IS WHERE THE CARD IS. `Requirement::zone` is
				// ONE normalised bucket: it cannot say "DECK or EXTRA". But the
				// operator sweeps both, and collapsing the mask blindly
				// (NormalizeZone returns EXTRA for DECK|EXTRA) would require every
				// source to be in the extra, making the requirement false for
				// fourteen fifteenths of them, silently. So we decide by the DECK,
				// which is at hand: that is a fact, not a convention.
				auto in_list = [&db](const std::vector<uint32_t>& l, uint32_t c2) {
					for(uint32_t x : l)
						if(db.Canonical(x) == c2)
							return true;
					return false;
				};
				for(const AcquirableCode& a : acq) {
					uint8_t src = NormalizeZone(static_cast<uint8_t>(a.source_zone));
					if(in_list(sd.extra, a.code) && (a.source_zone & LOCATION_EXTRA))
						src = NormalizeZone(LOCATION_EXTRA);
					else if(in_list(sd.main, a.code) && (a.source_zone & LOCATION_DECK))
						src = NormalizeZone(LOCATION_DECK);
					std::vector<Requirement> mats;
					mats.push_back(Requirement{ a.host,
						NormalizeZone(static_cast<uint8_t>(a.host_range
														   ? a.host_range : 0x0c)),
						kReqCard, 1 });
					mats.push_back(Requirement{ a.code, src, kReqCard, 1 });
					recipe_graph.Observe(a.code, mats, /*primed=*/true);
				}
				std::printf("  seeded from the OPERATORS: %zu "
											"declared recipe(s) (%zu dead "
											"route(s)), %zu code ACQUISITION "
											"edge(s), %zu known product(s)\n",
							nrec, dead, acq.size(), recipe_graph.Products());
				// The mechanism's LIVENESS, and it is by name: without it, a seeding
				// with zero edges would read as an active seeding.
				for(const AcquirableCode& a : acq)
					std::printf("      %s can ACQUIRE the "
													"code of %s  (%s, host "
													"@%s, source @%s)\n", db.Name(a.host).c_str(),
								db.Name(a.code).c_str(), a.grant.c_str(),
								ZoneMaskName(static_cast<uint32_t>(a.host_range)).c_str(),
								ZoneMaskName(static_cast<uint32_t>(a.source_zone)).c_str());
				if(acq.empty())
					std::printf("      (no acquisition "
													"edge: no card in the "
													"deck grants EFFECT_ADD_CO"
													"DE)\n");
				ReportSeededDistances(target, recipe_graph, db, watched);

				// --- SERIALISATION BY x* --------------------------------------
				//
				// The arithmetic leaves one route: ~110 real decisions of geometric
				// arity 5.9 make `10^85` in ONE block and `2x10^7` in fourteen
				// blocks of eight. What was missing was not the mechanism
				// (`novelty_serialize` already reopens the table) but its
				// CRITERION: "a target card placed" only moves at the very end. The
				// material balance's subgoals, on the other hand, exist from the
				// first brick, and they are COMPUTED (theorems), not guessed.
				// devines.
				std::vector<std::pair<uint32_t, uint32_t>> gc2;
				for(uint32_t c : target.codes) {
					auto it = std::find_if(gc2.begin(), gc2.end(),
										   [&](const auto& g) {
											   return g.first == c;
										   });
					if(it == gc2.end())
						gc2.emplace_back(c, 1u);
					else
						++it->second;
				}
				// `owned` is DEDUPLICATED (it serves as a membership set): passing
				// it to the material balance would give ONE copy per code instead
				// of three, and "three Ligers" would become infeasible for a reason
				// that has nothing to do with the deck. The model wants the
				// PHYSICAL copies.
				std::vector<uint32_t> deck_mult;
				for(const auto* l : { &sd.main, &sd.extra })
					for(uint32_t c : *l)
						deck_mult.push_back(c);
				// THE RESOLUTIONS ARE COMPILED INTO THE BALANCE. The goal has two
				// halves of equal rank, the final state (--target) and the required
				// passages (--resolve/--summon-min), and only the first was
				// compiled: all the credit of the second was post-event, hence no
				// mid-line rung, hence the card to resolve only lived in the
				// terminal spasm (measured: Omega's first summon at decision 240.5,
				// with 12.9 of life left, and a conjunction of ~2e-8 per rollout).
				// ONE presence demand @AVAILABLE per code (never N: the same body
				// coming back is legal, and requiring 1 keeps h admissible).
				std::vector<std::pair<uint32_t, uint32_t>> gtr;
				for(const ResolveReq& rr : cons.resolve_min) {
					const uint32_t cc = db.Canonical(rr.code);
					if(std::find_if(gtr.begin(), gtr.end(),
									[&](const auto& g) {
										return g.first == cc;
									}) == gtr.end())
						gtr.emplace_back(cc, 1u);
				}
				if(!cons.resolve_min.empty()) {
					std::string vie;
					for(const auto& g : gtr)
						vie += "x1 " + db.Name(g.first) + " @DISPO ; ";
					std::printf("  RESOLUTIONS -> "
													"BALANCE: %s\n", vie.c_str());
				}
				BalanceModel& bm = g_balance_model;
				g_balance_armed = false;
				if(opt.serial && !gc2.empty() &&
				   bm.Build(tbl, db, kt, deck_mult, gc2, gtr)) {
					LPResult lr;
					const double h0 = bm.Solve(deck_mult, {}, {}, &lr);
					if(lr.feasible && lr.primal_ok && lr.optimal_ok) {
						g_balance_armed = true;
						for(const BalanceModel::Need& n : bm.NeedsFrom(lr)) {
							if(n.zone == 0)
								continue;   // the RESERVE is not progress
							SearchConfig::SerialReq rq;
							rq.code = n.code;
							rq.arch = n.arch;
							rq.zone = n.zone;
							rq.count = n.count;
							serial_from_balance.push_back(rq);
						}
						// THE CONSUMPTION RUNGS. The gap profile showed that all the
						// ASSEMBLY is invisible to the production subgoals: three
						// deserts of 46 to 73 answers, each out of a sampler's reach
						// (b^37 ~ 10^28). The negative column of x*, returned as
						// @GRAVEYARD arrivals, climbs during those deserts, and that is
						// the grain the closed form requires.
						for(const BalanceModel::Need& n : bm.ConsumedFrom(lr)) {
							SearchConfig::SerialReq rq;
							rq.code = n.code;
							rq.arch = n.arch;
							rq.zone = n.zone;
							rq.count = n.count;
							serial_from_balance.push_back(rq);
						}
						// THE RESERVE DEPARTURES. The intermediate summons (the extra
						// deck vehicles x* does not fire because the LP fuses
						// "directly") all consume the reserve: it is the turn's
						// IRREVERSIBLE resource, the only quantity that climbs STEADILY
						// during the +46/+47 deserts of the gap profile. One unit per
						// card that left deck+extra since the root; ceiling 15 = the
						// packing (over-counting is harmless, since a unit never
						// reached creates no cell).
						// Two SLICES of 15 (the `arch` field carries the offset): the
						// real line makes ~25 departures, and the first slice alone
						// saturated at answer 118, just before the deserts to cover.
						for(uint64_t off : { 0ull, 15ull }) {
							SearchConfig::SerialReq rq;
							rq.zone = 5;
							rq.arch = off;
							rq.count = 15;
							serial_from_balance.push_back(rq);
						}
						// THE ENABLERS AND THE QUOTAS, derived from the LP duals.
						std::unordered_map<uint32_t, uint32_t> dcop;
						for(uint32_t c : deck_mult)
							++dcop[db.Canonical(c)];
						uint64_t k_efm = 0;
						kt.Lookup("EFFECT_EXTRA_FUSION_"
														"MATERIAL", k_efm);
						// (b) THE DERIVATION FROM THE DUALS: the default. Quotas:
						// hosts with a FINITE bound fired by x*, saturated, with a
						// positive dual, or producing a place x* consumes
						// (robustness to the degeneracy of zero-cost igniters).
						// Enablers: hosts of the transitions FIRED by x* that PERSIST
						// in play. Chick comes back through its rename, Wolf through
						// its ignition; nobody pushed them. The hosts of an
						// EFFECT_EXTRA_FUSION_MATERIAL are added as long as x* fires a
						// Fusion ignition: the AVAILABLE aggregate (which contains the
						// graveyard) is only optimistic BY their concession, which is a
						// STRUCTURAL hypothesis of the model, written here rather than
						// a card name.
						std::vector<uint32_t> presence_hosts;
						std::vector<uint32_t> quota_duals =
							bm.QuotaHostsFrom(lr, &presence_hosts);
						bool fusion_pulled = false;
						for(size_t t2 = 0; t2 < bm.Transitions(); ++t2)
							fusion_pulled =
								fusion_pulled ||
								(t2 < lr.x.size() && lr.x[t2] > 1e-6 &&
								 bm.TrNames()[t2] == "igniter");
						if(fusion_pulled && k_efm)
							for(const auto& kvv : tbl.All()) {
								const uint32_t host = db.Canonical(kvv.first);
								if(!dcop.count(host))
									continue;
								for(const DeclaredEffect& g :
									kvv.second.grants)
									if(g.code_is_effect &&
									   g.code_value == k_efm)
										presence_hosts.push_back(host);
							}
						// @IN PLAY presence: only the deck's hosts that REMAIN in
						// play carry a rung (a one-shot spell already leaves its
						// trace in a zone).
						uint64_t t_cont2 = 0;
						kt.Lookup("TYPE_CONTINUOUS", t_cont2);
						std::vector<SearchConfig::SerialReq> presence_duals;
						{
							std::sort(presence_hosts.begin(),
									  presence_hosts.end());
							presence_hosts.erase(
								std::unique(presence_hosts.begin(),
											presence_hosts.end()),
								presence_hosts.end());
							for(uint32_t host : presence_hosts) {
								auto dit = dcop.find(host);
								if(dit == dcop.end())
									continue;
								const CardRow* hrow = db.Find(host);
								const bool persists2 =
									hrow && ((hrow->type & 0x1u) ||
											 (t_cont2 &&
											  (hrow->type & t_cont2)));
								if(!persists2)
									continue;
								SearchConfig::SerialReq rq;
								rq.code = host;
								rq.zone = 6;
								rq.count = (std::min)(dit->second, 3u);
								presence_duals.push_back(rq);
							}
						}
						auto finish_hosts = [](std::vector<uint32_t>& v) {
							std::sort(v.begin(), v.end());
							v.erase(std::unique(v.begin(), v.end()), v.end());
							if(v.size() > 12)
								v.resize(12);
						};
						finish_hosts(quota_duals);
						// THE LIVENESS OF BOTH DERIVATIONS, always printed: the
						// three wrong derivations were seen ONLY through this line.
						auto print_hosts = [&db](const char* tag,
												 const std::vector<uint32_t>&
													 v) {
							std::printf("  %s : ", tag);
							if(v.empty())
								std::printf("(none)");
							for(uint32_t qh : v)
								std::printf("%s ; ", db.Name(qh).c_str());
							std::printf("\n");
						};
						print_hosts("QUOTAS tracked "
															"(duals)", quota_duals);
						{
							std::string ph;
							for(const auto& rq : presence_duals)
								ph += db.Name(rq.code) + " ; ";
							std::printf("  PRESENCE @EN JEU "
																	"(x*) : %s\n",
										ph.empty() ? "(none)" : ph.c_str());
						}
							quota_hosts_wiring = quota_duals;
							for(const auto& rq : presence_duals)
								serial_from_balance.push_back(rq);
						std::printf("  SERIALISATION "
															"from the material "
															"balance: h(start) = "
															"%.0f, %zu "
															"subgoal(s)\n",
									h0, serial_from_balance.size());
						for(const SearchConfig::SerialReq& rq :
							serial_from_balance)
							std::printf("      x%-2u %-34s "
																	"@%s\n", rq.count,
										rq.zone == 5
											? "(from reserve)"
											: rq.code
												  ? db.Name(rq.code).c_str()
												  : "(archetype)",
										rq.zone == 2   ? "TERRAIN"
										: rq.zone == 3 ? "CIMETIERE"
										: rq.zone == 4 ? "BANNIE"
										: rq.zone == 5 ? "RESERVE"
										: rq.zone == 6 ? "EN JEU"
													   : "DISPO");
					} else {
						// An infinite `h` HERE would mean the goal is proved out of
						// reach from this deck: we SAY so, we do not serialise on
						// nothing.
						std::printf("!! SERIALISATION: "
															"the material "
															"balance returns no "
															"plan (h = INFINITE "
															"or solver at fault) "
															"- no subgoal "
															"posted.\n");
					}
				}
			}
		}
		// THE BACKWARD DECOMPOSITION, PRINTED. It is the judge of the operator
		// seeding, and it is STRUCTURAL: no seed, no budget, no rollout. The NUMBER of
		// subproducts (`snap_backward`) alone says nothing about whether the operator
		// being sought is in there.
		{
			std::vector<uint32_t> roots = target.codes;
			std::sort(roots.begin(), roots.end());
			roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
			std::vector<Requirement> reqs;
			std::vector<uint32_t> prods;
			recipe_graph.Expand(roots, 3, reqs, prods);
			std::printf("  backward decomposition (BUILD order, "
									"%zu subproduct(s)):\n", prods.size());
			for(uint32_t p : prods)
				std::printf("      %s\n", db.Name(p).c_str());
			std::printf("  exigences rencontrees : %zu\n", reqs.size());
			for(const Requirement& q : reqs)
				if(q.kind == kReqCard)
					std::printf("      NOMMEE   %s @%s\n", db.Name(q.code).c_str(),
								q.zone ? ZoneMaskName(q.zone).c_str() : "any playable zone");
				else
					std::printf("      CARDINAL %s 0x%x "
													"x%u\n",
								q.kind == kReqLevel ? "niveau" : "archetype",
								q.code, q.count);
		}
	}

	// LEARNED LANDMARK GRAPH. Built BEFORE the workers and never modified
	// afterwards: read-only, hence no lock on the hot path. That is the difference
	// in kind from the recipe graph, which learns during the run.
	static LandmarkGraph landmark_graph;
	BuildLandmarkGraph(opt, db, scripts, plan, target, landmark_graph);

	// Adaptation replay (--adapt): the corpus enters no longer as per-move bonuses
	// but as a gradient over its own junctions.
	std::vector<NrpaRun> adapt_runs;
	BuildAdaptRuns(opt, db, scripts, plan, target, adapt_runs,
				   opt.recipes >= 0 ? &recipe_graph : nullptr);

	// OPTIONS: the catalogue is mined ONCE, here, and outlives every phase; the
	// workers read it as const. With no corpus there is nothing to mine, and
	// saying so beats leaving a mechanism silently absent from the path.
	OptionCatalog option_catalog;
	// ONLINE MINING: `--options-online` implies a catalogue ceiling. Without it
	// the flag would be accepted and INERT (options_n = 0 cuts everything), the
	// exact family of "a mechanism silently absent from the path".
	const uint32_t options_n =
		(opt.options_online && !opt.options_n) ? 256u : opt.options_n;
	if(options_n) {
		if(adapt_runs.empty()) {
			if(opt.options_online)
				std::printf("\n  options: no external corpus "
											"(--adapt) - the run starts BARE "
											"and will mine its own lines "
											"every %u s (--options-online).\n", opt.options_online);
			else
				std::printf("\n!! --options %u: no corpus "
											"recorded (--adapt missing or "
											"empty) - EMPTY catalogue, "
											"mechanism off.\n",
							options_n);
		} else {
			option_catalog = MineOptionCatalog(adapt_runs, options_n,
											   opt.options_support,
											   Options::kOptionsMaxLen,
											   Options::kOptionsWindow,
											   Options::kOptionsCtxTol);
			size_t max_len = 0, sum_len = 0;
			for(const auto& s : option_catalog.seqs) {
				max_len = (std::max)(max_len, s.size());
				sum_len += s.size();
			}
			// The size is a RESULT of the selection by Levin loss (it stops when
			// nothing improves any more), not the parameter.
			std::printf("\n  options: %zu macro(s) kept under a "
									"ceiling of %u (support >= %u, length "
									"2-%zu, mean %.1f); model loss %.1f -> "
									"%.1f log10\n",
						option_catalog.Size(), options_n,
						opt.options_support, max_len,
						option_catalog.Size()
							? double(sum_len) / double(option_catalog.Size())
							: 0.0,
						option_catalog.model_flat, option_catalog.model_opt);
		}
	}

	// --- 4. Search, by progressive deepening of the number of deviations.
	SearchConfig cfg;
	ApplyMechanisms(opt, cfg);   // one single wiring point
	cfg.serial_reqs = serial_from_balance;   // the computed subgoals
	cfg.quota_hosts = quota_hosts_wiring;    // (board, quotas) as the key
	// At equal progress, the archive score prefers the cell with FRESH quotas.
	// Part of the "duals" package; the --quota-legacy control replays the older
	// behaviour identically (a key with no preference).
	cfg.quota_fresh_pref = true;
	// The self-refining ladder. The model is only lent when serialisation has
	// armed: with no ladder there is nothing to refine, and the mechanism
	// announces itself inert through ReportMechanisms.
	cfg.refine_after = static_cast<uint32_t>(opt.refine_after);
	cfg.balance = g_balance_armed ? &g_balance_model : nullptr;
	// The plan has 273 steps; another deck will need more of them to get to the
	// same place. We leave a margin, otherwise the bound would cut before the
	// board.
	//
	// UNDER --no-ref the ceiling CANNOT come from the reference: the mode promises
	// to take nothing from it, and the depth bound is the most structuring thing it
	// could take. It is then derived from the DECKLIST (at most twelve
	// decisions per playable card, which covers summon, targeting, materials and
	// chain windows) and it is printed in every case, because a ceiling that cuts
	// without naming itself produces false "EXHAUSTED" verdicts.
	size_t deck_span = 0;
	if(!start_yrp.decks.empty()) {
		const Deck& d = start_yrp.decks[opt.target_player < static_cast<int>(
											start_yrp.decks.size())
											? opt.target_player : 0];
		deck_span = d.main.size() + d.extra.size();
	}
	const size_t from_deck = deck_span ? deck_span * 12 + 32 : 700;
	const size_t from_ref = ref_decisions * 3 / 2 + 32;
	const size_t derived = opt.no_ref ? from_deck : from_ref;
	cfg.max_decisions = opt.max_decisions ? opt.max_decisions
										  : static_cast<uint32_t>(derived);
	if(opt.max_decisions)
		std::printf("  decision ceiling: %u (--max-decisions; the "
							"default would have been %zu, %s)\n",
					opt.max_decisions, derived,
					opt.no_ref ? "derived from the "
													"decklist" : "derived from the reference");
	else
		std::printf("  decision ceiling: %zu (%s)\n", derived,
					opt.no_ref
						? (deck_span ? "derived from the "
															"decklist: 12 per "
															"card + 32"
									 : "no readable "
																		"decklist: fixed "
																		"default - set "
																		"--max-decisions")
						: "derived from the "
												"reference: 1.5x + 32");
	cfg.max_actions = 0;         // no bound: we first look for a way TO reach it
	cfg.max_nodes = 50000000;
	cfg.max_solutions = 16;
	cfg.enumeration.dedup_by_code = true;
	cfg.enumeration.max_subsets = opt.max_subsets;
	cfg.enumeration.db = &db;
	cfg.enumeration.canonical_zones = opt.canonical_zones;
	// POSTED target -> inclusion by default; CAPTURED target -> exact equality.
	cfg.goal_subset = opt.target_subset ||
					  (!opt.target_specs.empty() && !opt.target_exact);
	cfg.plan_window = 32;
	cfg.summon_constraints = cons.summons;
	cfg.guard_after = cons.guard_after;
	cfg.guard_clauses = cons.guard;
	cfg.guard_keep = cons.guard_keep.empty() ? nullptr : &cons.guard_keep;
	cfg.guard_opp_hand_release = cons.guard_opp_hand_release;
	cfg.resolve_min = cons.resolve_min;
	CheckSaturations(target.codes.size(), cons.resolve_min);
	// COUNTING DERIVED FROM THE TARGET BOARD. Free, and it answers before the
	// search a question the search took minutes not to answer: how many summon
	// EVENTS the board needs, and whether the decklist can even supply them.
	if(!start_yrp.decks.empty() && !target.codes.empty()) {
		const Deck& tdeck = start_yrp.decks[
			opt.target_player < static_cast<int>(start_yrp.decks.size())
				? opt.target_player : 0];
		std::map<Mech, uint32_t> events;
		auto counts = CountTarget(target, tdeck, db, events);
		ReportTargetCounting(counts, events, db);
		if(opt.derive_summon_min)
			DeriveSummonMin(counts, cons, db, cfg);
	}
	cfg.material_req = cons.material_req;
	cfg.hint_cards = cons.hints;
	cfg.options = option_catalog.Size() ? &option_catalog : nullptr;
	// ONLINE MINING: the run's living corpus. It is declared HERE so it outlives
	// every phase, but it is only WIRED into the rollout phase's workers, the only
	// phase that produces complete lines. The catalogue it ends up mining is then
	// passed to the finisher STATICALLY (nobody re-mines after the rollouts).
	OnlineOptions online;
	// The end-of-rollouts catalogue, kept alive for the finisher.
	std::shared_ptr<const OptionCatalog> final_online;
	if(opt.options_online) {
		online.max_options = options_n;
		online.support = opt.options_support;
		online.max_len = Options::kOptionsMaxLen;
		online.window = Options::kOptionsWindow;
		online.ctx_tol = Options::kOptionsCtxTol;
		online.period_ms = opt.options_online * 1000.0;
		online.max_pool = Options::kOptionsPool ? Options::kOptionsPool : 1;
		online.per_worker = opt.options_per_worker ? opt.options_per_worker : 1;
		if(!adapt_runs.empty())
			online.seed = &adapt_runs;
		if(option_catalog.Size()) {
			online.cat = std::make_shared<const OptionCatalog>(option_catalog);
			online.gen = 1;   // the workers adopt it from the first round
		}
		std::printf("  ONLINE options: re-mined every %u s, living "
							"corpus %zu line(s) max (%zu per worker)%s\n",
					opt.options_online, online.max_pool, online.per_worker,
					online.seed ? ", corpus --adapt en "
													"amorce" : "");
	}
	std::printf("  hint bias         : %.2f (%s)\n", cfg.hint_bias,
				opt.hint_bias >= 0 ? "--hint-bias" : "engine default");
	// DETERMINISTIC MODE. Printed, because a budget that is no longer time changes
	// how EVERY throughput counter of the report reads.
	if(opt.max_nodes)
		cfg.max_nodes = opt.max_nodes;
	if(opt.max_rollouts || opt.max_nodes) {
		std::printf("  BUDGET IN COUNT: %llu rollout(s), %llu node(s) "
							"per worker\n",
					(unsigned long long)opt.max_rollouts,
					(unsigned long long)cfg.max_nodes);
		if(opt.threads != 1)
			std::printf("     !! several workers: the budget is "
									"deterministic, the ORDER of exchanges is "
									"not. Add --threads 1 for a reproducible "
									"run.\n");
	}
	if(opt.adapt_to_peak)
		std::printf("  gradient TRUNCATED AT THE PEAK of the score "
							"(--adapt-to-peak)\n");
	// `--elide-forced` must be wired on the SEARCH path, not only in `--growth`.
	// Wired only there it does nothing here, provably: in deterministic mode
	// `--no-elide-forced` returns "3000 rollouts, 149334 states, 3002 adaptations"
	// byte for byte. That is the "live but inert" trap: a flag that declares itself
	// on while being off.
	//
	// What is debatable is its DEFAULT VALUE, and that stays "off", because the
	// +61 % throughput was measured on the `--growth` path, never on this one.
	if(opt.elide_forced)
		std::printf("  FORCED moves played inline (--elide-forced) - "
							"NOT JUDGED on this path\n");
	// The recipe graph is created and seeded HIGHER UP (before the adaptation
	// replay, which feeds it); here, only the wiring into cfg.
	if(opt.recipes >= 0) {
		cfg.recipes = &recipe_graph;
	}
	// --- THE FOUR LEVERS -----------------------------------------------------
	// The recipe-reading levers all read the recipe graph: without it they are
	// live and inert. The implication is applied HIGHER UP (with --probe-repeat's)
	// and restated here for each flag that triggered it.
	if(opt.op_bias > 0.0) {
		// THE TWO WAYS THIS MECHANISM CAN BE INERT, STATED BEFORE THE RUN: a
		// mechanism that is off while declaring itself on makes one measure the
		// same arm twice.
		if(opt.recipes < 0.0)
			std::printf("!! --op-bias without --recipes: the "
									"mechanism reads `snap_operators`, which "
									"only the graph snapshot fills. It would "
									"be INERT.\n");
		else if(!opt.op_recipes)
			std::printf("!! --op-bias without --op-recipes: the "
									"decomposition has no ACQUISITION edge, "
									"hence no on-field presence requirement, "
									"hence the list will be EMPTY.\n");
		else
			std::printf("  OPERATOR bias: %.2f on the moves that "
									"play a card the backward decomposition "
									"requires IN PLAY\n",
						opt.op_bias);
	}
	if(opt.assign_bias > 0.0) {
		std::printf("  --assign-bias %.2f: choices engaging a "
							"MATERIAL of the recipe graph are favoured\n"
							"                     (selection prompts included "
							"- card identity is unconditional there)\n",
					opt.assign_bias);
		// THE MECHANISM READS `snap_useful`, WHICH COMES FROM THE GRAPH. With no
		// graph it can read nothing and the flag is INERT; it was, silently, while
		// printing the line above. The report now says so, and the "graph
		// snapshots" line of the rollout summary gives the mechanism's liveness.
		if(!cfg.recipes)
			std::printf("!! --assign-bias WITHOUT a recipe graph: "
									"the mechanism is INERT (it reads "
									"`snap_useful`).\n"
									"   Add --recipes 0 - the graph is then "
									"fed and read without entering any cost.\n");
	}
	// The transposition key does NOT conflate the COLUMNS. On benchmark A that
	// divides placements by 2 and by 12, and the cause is NAMED: that deck carries
	// three Link monsters, whose ARROWS point at columns, and a pointed zone allows
	// a summon from the extra deck. The gain on the exhaustive search is real
	// (x1.67, +4 depths) and it does not convert.
	if(opt.assign || opt.backward) {
		if(!cfg.recipes)
			std::printf("!! --assign / --backward without a "
									"recipe graph: the mechanisms are INERT.\n");
		else
			std::printf("  assign %s, backward %s (graph snapshot "
									"every %llu rollouts)\n",
						opt.assign ? "OUI" : "non",
						opt.backward ? "OUI" : "non",
						(unsigned long long)cfg.recipe_snap_period);
	}
	if(opt.hindsight > 0.0)
		std::printf("  hindsight %.2f x alpha, at most %zu substitute "
							"goal(s) per worker\n",
					opt.hindsight, opt.hindsight_k);
	// THE "OFFER -> CHOICE CONVERSION" JUDGE needs to know which card the move
	// kept engages. There is nothing to turn on: `Choice::card` is filled
	// UNCONDITIONALLY, selection prompts included. No arbitration between "a mute
	// probe" and "a run modified under it" is needed, because it is never the
	// identity that changes the run but the HINT BIAS applied to it, and that one
	// is guarded by `IsSubsetPrompt`.
	// `--watch`: PURE observation. No entry in `cons.resolve_min`, no
	// `cfg.hint_cards`, no gradient; that is the whole point of the flag. Resolved
	// by name or by code, like the others.
	for(const std::string& spec : opt.watch_specs) {
		if(cfg.probe_watch.size() >= 4) {
			std::printf("!! --watch: at most 4 cards (packed "
									"counters) - \"%s\" ignored\n", spec.c_str());
			continue;
		}
		uint32_t code = 0;
		if(!ResolveCard(Trimmed(spec), db, "--watch", code))
			return;
		cfg.probe_watch.push_back(db.Canonical(code));
	}
	if(!cfg.probe_watch.empty()) {
		std::printf("  --watch: %zu card(s) OBSERVED with no "
							"constraint and no bias -", cfg.probe_watch.size());
		for(uint32_t c : cfg.probe_watch)
			std::printf(" %s;", db.Name(c).c_str());
		std::printf("\n");
	}
	// LANDMARKS: the graph travels with BOTH of its weights. A weight with no
	// graph would be a flag accepted and inert; a graph with no weight is the
	// "learned and MEASURED, does not enter the cost" mode, and that has to be
	// said, otherwise the two are conflated in the logs.
	if(!landmark_graph.Empty()) {
		cfg.landmarks = &landmark_graph;
		std::printf("  landmarks: ACTIVE, %zu achievement(s), rollout "
							"weight %.2f, finisher weight %.2f%s\n",
					landmark_graph.Items().size(), opt.landmark_weight,
					opt.landmark_h,
					(opt.landmark_weight == 0.0 && opt.landmark_h == 0.0)
						? "  (learned and MEASURED, "
												"enter no cost)"
						: "");
	} else if(opt.landmark_weight > 0.0 || opt.landmark_h > 0.0) {
		std::printf("!! --landmark-w/--landmark-h without a landmark "
							"graph: the weight is INERT (--landmarks is "
							"missing)\n");
	}
	// The pointers are wired: the report can say what is ACTIVE and what is
	// requested but INERT. Read BEFORE the first rollout, it makes a measurement
	// arm disposable before spending the budget, not after.
	ReportMechanisms(cfg, "transplant");
	// Anytime cost optimisation: the search continues past the first solution
	// (each solution tightens the bound), the per-worker set is bounded by
	// replacing the worst, and the NRPA goal score is lexicographic.
	if(opt.optimize) {
		cfg.anytime = true;
		cfg.max_solutions = 24;
		std::printf("\n  ANYTIME OPTIMISATION: lexicographic cost "
							"(burned, actions, decisions),\n"
							"  the reference costs %u/%u/%zu - the bound to "
							"beat.%s\n", ref_burned, ref_actions, ref_decisions,
					opt.burn_limit
						? "  (burned bound seeded)" : "");
	}
	// The cards to resolve (--resolve) receive the hint bias AUTOMATICALLY: the
	// line MUST engage them, and the measurement is unambiguous, since the policy
	// NEVER rips without a nudge despite the +100 gradient per resolution.
	for(const ResolveReq& req : cons.resolve_min)
		if(std::find(cfg.hint_cards.begin(), cfg.hint_cards.end(), req.code) ==
		   cfg.hint_cards.end())
			cfg.hint_cards.push_back(req.code);
	if(!cons.no_activate.empty())
		cfg.enumeration.no_activate = &cons.no_activate;
	if(!cons.no_chain.empty())
		cfg.enumeration.no_chain = &cons.no_chain;
	if(!cons.self_negate.empty())
		cfg.self_negate = &cons.self_negate;   // chosen discipline
	cfg.enumeration.mp1_only = opt.mp1_only;   // combo in Main Phase 1 only
	// Informative verdict: here the reference plays on ANOTHER deck, so its
	// conformity conditions no invariant, but it says whether the plan served as a
	// repertoire respects the requested constraint itself.
	ReportConstraints(cons, ref, db);

	// Feasibility of the --resolve entries: the card to resolve must EXIST in the
	// starting deck (hand + extra), otherwise the constraint is unsatisfiable and
	// NO line exists, whatever the search does. The minimum, on the other hand,
	// may exceed the number of copies: a card can be recovered (player's ruling:
	// Omega comes back from the banished zone through Dis Pater).
	if(!cons.resolve_min.empty()) {
		const Deck& deck = start_yrp.decks[opt.target_player];
		std::map<uint32_t, uint32_t> avail;
		for(const auto* list : { &deck.main, &deck.extra })
			for(uint32_t c : *list)
				++avail[db.Canonical(c)];
		bool impossible = false;
		std::printf("\n--- feasibility of the required resolutions "
							"---\n");
		for(const ResolveReq& req : cons.resolve_min) {
			uint32_t have = avail.count(req.code) ? avail[req.code] : 0;
			std::printf("  %-40s x%u required, %u copy/copies in "
									"the deck%s\n",
						db.Name(req.code).c_str(), req.min_count, have,
						have ? "" : "   <-- ABSENTE");
			if(!have)
				impossible = true;
		}
		if(impossible) {
			std::printf("\n  A card to resolve does not exist in "
									"this deck: the constraint is\n"
									"  UNSATISFIABLE - no line exists. Search "
									"cancelled.\n");
			return;
		}
	}

	unsigned threads = opt.threads ? opt.threads
								   : (std::max)(1u, std::thread::hardware_concurrency());
	std::vector<Solution> sols;
	uint32_t best_overlap = 0, best_monsters = 0;
	std::vector<uint32_t> best_board;
	// Path leading to the best state met, across all passes: the finisher's input.
	// The field detail says what differs when all the codes are there.
	std::vector<std::vector<uint8_t>> best_path;
	std::vector<QueriedCard> best_mzone, best_szone;
	// The best JOINT line: lexicographic max (rips, board), across all passes.
	// Written at the end of the run (best_joint_*.yrp) to be re-injected through
	// --approach; without it the lines with complete rips die with the run (47
	// rollouts at 3 rips in one run, none kept).
	uint32_t best_joint_rp = 0, best_joint_overlap = 0;
	std::vector<std::vector<uint8_t>> best_joint_path;
	auto merge_joint = [&](const SearchStats& st,
						   const std::vector<std::vector<uint8_t>>& pre) {
		const size_t cand_len = pre.size() + st.best_joint_path.size();
		if(st.best_joint_rp &&
		   (st.best_joint_rp > best_joint_rp ||
			(st.best_joint_rp == best_joint_rp &&
			 (st.best_joint_overlap > best_joint_overlap ||
			  (st.best_joint_overlap == best_joint_overlap &&
			   !best_joint_path.empty() &&
			   cand_len < best_joint_path.size()))))) {
			best_joint_rp = st.best_joint_rp;
			best_joint_overlap = st.best_joint_overlap;
			best_joint_path = pre;
			best_joint_path.insert(best_joint_path.end(),
								   st.best_joint_path.begin(),
								   st.best_joint_path.end());
		}
	};
	// A path rooted on an APPROACH is only start-rooted when the approach shares
	// the starting TEMPLATE, which is true for the best_joint/best_approach files
	// this pipeline writes (WriteYrp1 copies the seed and the parameters). VERIFIED
	// rather than assumed: one iteration once lost its best joint line because the
	// exclusion was blind.
	auto same_gabarit = [&](const Replay* src) {
		return src &&
			   std::equal(std::begin(src->seed), std::end(src->seed),
						  std::begin(start_yrp.seed)) &&
			   src->duel_flags == start_yrp.duel_flags &&
			   src->start_lp == start_yrp.start_lp &&
			   src->start_hand == start_yrp.start_hand &&
			   src->draw_count == start_yrp.draw_count;
	};
	// GLOBAL budget: the three passes share solve_ms, they do not stack it; a
	// --solve-ms of 600 s must last ~600 s.
	double spent = 0;

	// GLOBAL Go-Explore archive (merge of the passes' archives, one entry per cell
	// = complete board) and merged NRPA policy (mean of the workers' weights): the
	// finisher's raw material. A policy dying with the run throws away exactly the
	// guide the conversion needs; that is the lock measured three times (a finisher
	// exhausted at ~6 states from the single best state).
	std::unordered_map<uint64_t, ArchiveEntry> global_archive;
	NrpaPolicy merged_policy;
	unsigned policy_workers = 0;
	// THE PREVIOUS ROUND'S INHERITANCE (--carry). Adopted BEFORE any phase: this
	// round's finisher will read those cells among its roots, and the rollout
	// workers will be seeded from them below. The liveness is stated; an archive
	// carried in silence would be indistinguishable from an empty one.
	if(carry && opt.carry &&
	   (!carry->archive.empty() || !carry->policy.empty())) {
		global_archive = std::move(carry->archive);
		merged_policy = std::move(carry->policy);
		policy_workers = carry->policy_workers;
		std::printf("  ARCHIVE CARRIED (--carry): %zu cell(s), policy "
							"%zu weight(s) (%u worker(s)) taken from the "
							"previous round\n",
					global_archive.size(), merged_policy.size(),
					policy_workers);
	}
	// THE SEEDING SNAPSHOT (--carry): the INHERITED cells, frozen BEFORE this
	// round's phases write anything, so the probe and the rollout workers are
	// seeded from YESTERDAY's round, not from today's noise. Paths are
	// start-rooted by construction (same template between rounds); NEVER seed a
	// finisher rooted on a prefix.
	std::vector<ArchiveEntry> carry_seed;
	if(opt.carry)
		for(const auto& [cell_, e_] : global_archive)
			carry_seed.push_back(e_);
	// Burned bound SHARED between workers: seeded by --burn-limit, tightened by
	// every improvement of every phase, so a worker that finds 19 cuts for the
	// other fifteen from the next decision on. --no-burn-share disconnects it.
	std::atomic<uint32_t> shared_burn{ opt.burn_limit ? opt.burn_limit
													  : UINT32_MAX };
	auto merge_archive = [&](const std::vector<ArchiveEntry>& a) {
		for(const ArchiveEntry& e : a) {
			auto [it, fresh] = global_archive.try_emplace(e.cell, e);
			if(!fresh && e.score > it->second.score)
				it->second = e;
		}
	};
	// FULL GO-EXPLORE, the RE-ROOTED merge (--archive-fin). The finisher's search
	// archives are relative to THEIR root (the prefix is replayed before the
	// Search is built), so we re-root: path = prefix + path, decisions cumulated,
	// and the score's depth tail recalibrated EXACTLY (low bits = ~depth; the
	// STATE components, i.e. progress, rips, overlap and burned, are read from the
	// duel and stay correct). Two KNOWN and SAFE approximations: the cell key and
	// the "fresh quotas" nibble date from the finisher's root (they lack the
	// prefix's uses), giving at worst EXTRA cells (over-partitioning, the safe
	// direction) and never a corrupted representative; a re-observation by a
	// seeded worker takes the live key back. CALL UNDER fmx (global_archive is not
	// protected).
	size_t fin_cells_new = 0, fin_cells_upd = 0;
	auto merge_rebased = [&](const std::vector<ArchiveEntry>& a,
							 const std::vector<std::vector<uint8_t>>& pre) {
		for(ArchiveEntry e : a) {
			if(e.path.empty())
				continue;
			e.decisions += static_cast<uint32_t>(pre.size());
			std::vector<std::vector<uint8_t>> full = pre;
			full.insert(full.end(), e.path.begin(), e.path.end());
			e.path = std::move(full);
			const uint64_t tail =
				0xFFFFFFFFull -
				(std::min<uint64_t>)(e.decisions, 0xFFFFFF00ull);
			if(cfg.serial_reqs.empty())
				e.score = (e.score & ~0xFFFFFFFFull) | tail;
			else
				e.score = (e.score & ~0xFFFFFFull) | (tail >> 8);
			auto [it, fresh] = global_archive.try_emplace(e.cell, e);
			if(fresh)
				++fin_cells_new;
			else if(e.score > it->second.score) {
				it->second = std::move(e);
				++fin_cells_upd;
			}
		}
	};

	// --- 4a. Greedy probe. The bounded-discrepancy search only descends as
	// deep as its deviation budget allows; when the plan does not apply from the
	// opening on, that caps out at a dozen decisions where the board needs
	// hundreds. The guided descent, on the other hand, goes all the way down: it
	// says how far this deck can get, which no LDS failure reveals.
	{
		std::printf("\n--- probe: how far does this deck go from this "
							"hand? ---\n");
		auto t0 = Clock::now();
		// Dedicated thread, for the same reason as OpeningHand: an arena is never
		// initialised on a thread that already owns one.
		std::thread([&] {
		Arena probe_arena;
		std::string err;
		if(probe_arena.Init(opt.arena_mb << 20, 0, err)) {
			{
				Duel probe(db, scripts, &probe_arena);
				if(probe.Create(start_yrp.seed, start_yrp.duel_flags,
								start_yrp.start_lp, start_yrp.start_hand,
								start_yrp.draw_count, err) &&
				   probe.Setup(start_yrp, err,
							   cons.opp_hand.empty() ? nullptr : &cons.opp_hand,
							   static_cast<uint8_t>(1 - opt.target_player))) {
					if(opt.stop_gc)
						probe.SetLuaGc(false);
					SearchConfig pcfg = cfg;
					pcfg.time_limit_ms = (std::min)(opt.solve_ms / 6.0, 20000.0);
					pcfg.archive_k = opt.archive_k;
					Search s(probe, probe_arena, start_yrp, pcfg);
					// Seeding from the previous round (--carry): the probe starts again
					// from yesterday's frontier.
					if(!carry_seed.empty())
						std::printf("  archive seeding "
															"(--carry): %zu "
															"cell(s) -> probe\n",
									s.SeedArchive(carry_seed));
					s.RunGuided(target);
					merge_archive(s.Archive());
					const SearchStats& st = s.Stats();
					std::printf("  %llu states, %.1f s: "
													"at best %u of the %zu "
													"target cards, %u "
													"monster(s)\n",
								(unsigned long long)st.nodes, st.ms / 1000.0,
								st.best_overlap, target.codes.size(),
								st.best_monsters);
					if(st.best_overlap > best_overlap) {
						best_board = st.best_board;
						best_path = st.best_path;
						best_mzone = st.best_mzone;
						best_szone = st.best_szone;
					}
					best_overlap = (std::max)(best_overlap, st.best_overlap);
					best_monsters = (std::max)(best_monsters, st.best_monsters);
					merge_joint(st, {});
					for(const auto& x : s.Solutions())
						sols.push_back(x);
				} else {
					std::printf("  !! starting duel "
													"cannot be initialised: "
													"%s\n",
								err.c_str());
				}
			}
			probe_arena.Shutdown();
		}
		}).join();
		spent += MsSince(t0);
		prof::PrintPhase("probe");
	}

	// --- 4b. Deep rollouts. This is the pass that has a chance of going all the
	// way: it descends to the end of the turn on every attempt, where the other
	// two stop after a few dozen decisions. Two engines share the workers:
	//   - pure GREEDY rollouts: child evaluation, locally strong;
	//   - NRPA rollouts: a policy learned per move code (plan_key), the repertoire
	//     as a bias, and NO child evaluation, so each decision costs several times
	//     less and the policy concentrates the rollouts.
	{
		std::printf("\n--- deep rollouts guided by the repertoire ---\n");
		// BUDGET SPLIT BETWEEN PHASES. Seven judgement-picked constants decide the
		// relative volume of the three passes whose yields are compared, and none was
		// printed: two runs with the same --solve-ms could give very different volumes
		// with not a word about it.
		double budget = (std::max)(0.0, (opt.solve_ms - spent) * 0.7);
		// Budget reserved for the finisher (--finisher-min): the rollouts give way
		// when conversion is the question.
		if(opt.finisher_min > 0)
			budget = (std::max)(0.0, (std::min)(
				budget, opt.solve_ms - spent - opt.finisher_min));
		std::mutex merge;
		struct ModeStats {
			uint64_t nodes = 0, rollouts = 0, cuts = 0, turn_cuts = 0, adapts = 0;
			// Per-rollout progress profile: additive across workers.
			uint64_t sp_final[72] = {};
			uint64_t sp_at_sum = 0, sp_lines = 0;
			// Liveness of the return to the rung (--reenter): additive.
			uint64_t reenter_rollouts = 0, reenter_fail = 0,
					 reenter_base_sum = 0;
			// Liveness of the refinement: refined workers, sub-rungs, states
			// archived beyond the gate.
			uint64_t refine_done = 0, refine_subrungs = 0, refine_top_hits = 0;
			uint64_t hint_seen = 0, hint_taken = 0;
			// Breakdown: `sel` = SUBSET prompts, where the card identity is
			// approximate; `exact` = the rest, where it really designates the move.
			// Without that separation the counter had three causes.
			uint64_t hint_exact = 0, hint_sel = 0;
			// Liveness of --adapt-to-peak, and work redone by the transposition.
			uint64_t peak_trunc = 0, tt_reexplored = 0;
			uint64_t rr[4] = { 0, 0, 0, 0 };
			uint64_t burn_cuts = 0, goal_hits = 0;
			// THE CONSTRAINTS CUT HERE, in the rollouts, not in the LDS passes
			// where `PrintCuts` already displayed them at zero. Wiring them into
			// the rollout phase line is the only way to answer the open question:
			// does the guard prune usefully, or does it raze the space?
			uint64_t constraint_cuts = 0, guard_cuts = 0, self_negate_cuts = 0,
					 guard_keep_cuts = 0;
			// Options: picks / decisions absorbed / aborted.
			uint64_t macro_taken = 0, macro_absorbed = 0, macro_aborted = 0;
			size_t ctx_entries = 0;
			bool ctx_capped = false;
			uint32_t overlap = 0, monsters = 0, overlap_ripped = 0;
			// HEAD BANDIT (--qhat): the mechanism's liveness, aggregated.
			uint64_t qhat_decisions = 0, qhat_first = 0, qhat_playouts = 0;
			uint64_t qhat_fallback = 0;
			double qhat_reward_sum = 0;
			size_t qhat_nodes = 0, qhat_codes = 0, qhat_bytes = 0;
			// LANDMARKS: the mechanism's liveness WHERE IT ACTS. The counter
			// already exists, but printing it only from `PrintCuts` would miss the
			// rollout phase, i.e. precisely the phase where the `--landmark-w` weight
			// works. The "live but inert" trap, on this very mechanism.
			double lm_h_sum = 0.0;
			uint64_t lm_h_count = 0;
			// The liveness of the four mechanisms. Same reason as above: without it
			// one weights landmarks without knowing whether `h` decreases.
			double rec_roll_sum = 0.0, rec_roll_d0_sum = 0.0;
			uint64_t rec_roll_count = 0;
			double backward_sum = 0.0;
			uint64_t backward_count = 0;
			uint64_t hindsight_goals = 0, hindsight_adapts = 0;
			uint64_t hindsight_quota_spent = 0, hindsight_quota_fresh = 0;
			uint64_t recipe_snaps = 0, snap_products = 0, snap_useful = 0,
					 snap_backward = 0;
		// Liveness of the operator bias (--op-bias).
		uint64_t op_offered = 0, op_taken = 0, op_listed = 0;
			// REPETITION PROBE (--probe-repeat). Everything in it is ADDITIVE
			// across workers except min/max and the d0 reference, which is the same
			// for all of them (same starting state), so any one of them will do.
			RepeatProbe rep[4];
			void AddRepeat(const SearchStats& s) {
				for(int i = 0; i < 4; ++i) {
					const RepeatProbe& r = s.rep[i];
					RepeatProbe& a = rep[i];
					if(!a.code)
						a.code = r.code;
					for(int k = 0; k < 5; ++k)
						a.reached[k] += r.reached[k];
					a.more_n += r.more_n;
					a.more_sum += r.more_sum;
					a.rest_sum += r.rest_sum;
					a.more_kept += r.more_kept;
					a.more_lost += r.more_lost;
					a.after_sum += r.after_sum;
					a.first_depth_sum += r.first_depth_sum;
					a.d0_samples += r.d0_samples;
					a.known |= r.known;
					a.offer_steps += r.offer_steps;
					a.offer_rollouts += r.offer_rollouts;
					a.offer_msgs |= r.offer_msgs;
					a.act_rollouts += r.act_rollouts;
					a.act_total += r.act_total;
					for(int z = 0; z < 6; ++z)
						a.zone_rollouts[z] += r.zone_rollouts[z];
					a.taken_steps += r.taken_steps;
					a.yn_steps += r.yn_steps;
					a.yn_yes += r.yn_yes;
					for(int k = 0; k < 6; ++k)
						a.offer_by[k] += r.offer_by[k];
					if(r.more_n) {
						a.more_min = (std::min)(a.more_min, r.more_min);
						a.more_max = (std::max)(a.more_max, r.more_max);
					}
					// The preserved/consumed CLASSIFICATION is done INSIDE the worker,
					// against its contemporary reference, which is the only valid
					// comparison. The aggregated d0 is informative only: we keep the
					// largest, which is the most recent in the graph's sense (it only
					// ever grows).
					if(r.d0 != 0xffffffffu &&
					   (a.d0 == 0xffffffffu || r.d0 > a.d0)) {
						a.d0 = r.d0;
						a.rest0 = r.rest0;
					}
				}
			}
		};
		ModeStats greedy, nrpa;
		// PROBE OF THE FIRST DECISION, aggregated across workers: the instrument
		// required BEFORE any measurement, i.e. n^ and Q^ per opening. The sample sizes
		// and the reward sums are additive, so the aggregation is exact and not a
		// mean of means.
		std::map<uint64_t, BanditProbe> qhat_root;
		// Seed derived from the clock by default, and PRINTED: the old constant made
		// every relaunch the same run (measured: 8/8 on one seed, 7/8 on three
		// others, and relaunching must re-draw).
		uint64_t base_seed = opt.seed;
		if(!base_seed) {
			base_seed = static_cast<uint64_t>(
				std::chrono::high_resolution_clock::now().time_since_epoch().count());
			base_seed ^= base_seed >> 33;
			base_seed *= 0xff51afd7ed558ccdull;
			base_seed ^= base_seed >> 33;
			if(!base_seed)
				base_seed = 1;
		}
		std::printf("  seed: %llu  (--seed %llu to replay)\n",
					(unsigned long long)base_seed, (unsigned long long)base_seed);
		{
			const int lvl = opt.nrpa_level > 0 ? opt.nrpa_level
											  : ((budget > 180000.0) ? 3 : 2);
			// The number of rollouts per level call is iters^L: printing it
			// hard-coded (576/13824) would lie as soon as --nrpa-iters moved,
			// exactly the hidden-variable problem.
			const uint32_t it = opt.nrpa_iters ? opt.nrpa_iters
											   : SearchConfig{}.nrpa_iters;
			double rollouts = 1;
			for(int k = 0; k < lvl; ++k)
				rollouts *= it;
			std::printf("  NRPA level: %d  (%s; ~%.0f rollouts "
									"per level call, iters %u)\n", lvl,
						opt.nrpa_level > 0 ? "--nrpa-level"
										   : "default, 180 s "
																					"threshold on the "
																					"rollout budget",
						rollouts, it);
			// The adaptation dial, printed as soon as it leaves the default: a setting
			// that changes the algorithm without naming itself is a hidden variable.
			// cachee (C15).
			if(opt.nrpa_alpha > 0)
				std::printf("  adaptation  : alpha %.3f (%s), "
											"level exit on stagnation alone\n",
							opt.nrpa_alpha,
							"--nrpa-alpha");
		}
		// Best GLOBAL sequence, shared between the NRPA workers: restarts begin
		// again from the best line known to all instead of relearning the same
		// sublines separately.
		NrpaShared shared_best;
		auto t0 = Clock::now();
		// First online mining deadline: one period after the start, since before
		// that the living corpus has nothing complete to offer.
		online.next = std::chrono::steady_clock::now() +
					  std::chrono::milliseconds(
						  static_cast<long long>(online.period_ms));

		auto worker = [&](unsigned id) {
			// Seven workers out of eight on NRPA. The original greedy quarter was
			// re-measured on disciplined runs: a peak of 2/8 for ~6 M states, three
			// runs out of three, while NRPA does 7-8/8. It keeps a residual share
			// (different exploration), plus the quarter.
			const bool use_nrpa = opt.nrpa && (id % 8 != 1);
			Arena la;
			std::string err;
			if(!la.Init(opt.arena_mb << 20, 0, err))
				return WorkerAbort("arena (NRPA rollouts)", err);
			{
				Duel local(db, scripts, &la);
				if(local.Create(start_yrp.seed, start_yrp.duel_flags,
								start_yrp.start_lp, start_yrp.start_hand,
								start_yrp.draw_count, err) &&
				   local.Setup(start_yrp, err,
							   cons.opp_hand.empty() ? nullptr : &cons.opp_hand,
							   static_cast<uint8_t>(1 - opt.target_player))) {
					if(opt.stop_gc)
						local.SetLuaGc(false);
					SearchConfig wcfg = cfg;
					wcfg.time_limit_ms = budget;
					wcfg.novelty_patience = patience;
					// NRPA nesting level. This is NOT a fine setting: it changes the
					// cost of a level call from iters^2 (~576 rollouts) to iters^3
					// (~13 824), i.e. the sampling algorithm itself. A 180 s threshold
					// choosing it by itself falls exactly on the dividing line between
					// otherwise comparable commands: a 600 s run without --finisher-min
					// leaves 420 s to the rollouts (level 3), and the SAME run with
					// --finisher-min 420000 leaves ~180 (level 2), a hidden variable. It
					// is now explicit, and its value is printed below.
					wcfg.nrpa_level = opt.nrpa_level > 0
										  ? opt.nrpa_level
										  : ((budget > 180000.0) ? 3 : 2);
					if(opt.nrpa_bias >= 0)
						wcfg.nrpa_bias_known = static_cast<float>(opt.nrpa_bias);
					wcfg.nrpa_restart_keep = static_cast<float>(opt.nrpa_keep);
					wcfg.nrpa_shared = &shared_best;
					// Montparnasse recipe: adaptation step and iterations per level,
					// hard-coded until now. 0 = the engine default, byte for byte.
					// moteur, a l'octet pres.
					if(opt.nrpa_alpha > 0)
						wcfg.nrpa_alpha = static_cast<float>(opt.nrpa_alpha);
					if(opt.nrpa_iters)
						wcfg.nrpa_iters = opt.nrpa_iters;
					// ONLINE mining: this worker pours its best lines into the living
					// corpus and buys the catalogue back between two upper-level
					// iterations. `worker_id` serves the per-worker quota (the
					// diversity pump).
					if(opt.options_online) {
						wcfg.options_online = &online;
						wcfg.worker_id = id;
					}
					wcfg.archive_k = opt.archive_k;
					// Burned bound shared between workers.
					if(opt.optimize && opt.burn_share)
						wcfg.shared_burn = &shared_burn;
					// Prior by replay: the policy starts knowing how to rip
					// (attenuated afterwards like the learned weights).
					// Adaptation replay: same injection, a gradient instead of a
					// bonus.
					if(!adapt_runs.empty()) {
						wcfg.nrpa_adapt_runs = &adapt_runs;
						wcfg.nrpa_adapt_passes = opt.adapt_passes;
					}
					// Two-level policy.
					wcfg.ctx_shrink = static_cast<float>(opt.ctx_shrink);
					// Path conditioning (MCPS) and the contextual table's cap: the
					// two must travel TOGETHER, otherwise the context is computed
					// and never bounded.
					// HEAD BANDIT (--qhat): the four dials travel together;
					// without the window and the cap, the depth alone would make an
					// unbounded mechanism.
					wcfg.qhat_depth = opt.qhat_depth;
					wcfg.qhat_window = opt.qhat_window;
					wcfg.qhat_rho = opt.qhat_rho;
					wcfg.qhat_max_nodes = opt.qhat_nodes;
					wcfg.ctx_max = opt.ctx_max;
					wcfg.nrpa_temp = static_cast<float>(opt.nrpa_temp);
					Search s(local, la, start_yrp, wcfg);
					// Seeding from the previous round (--carry): each rollout
					// worker starts again from yesterday's frontier, and the return
					// to the rung re-enters cells no rollout of THIS round has
					// reached yet. Liveness stated once (worker 0).
					if(!carry_seed.empty()) {
						const size_t sown = s.SeedArchive(carry_seed);
						if(id == 0)
							std::printf("  archive seeding "
																	"(--carry): %zu "
																	"cell(s) per worker\n", sown);
					}
					// A distinct seed per worker: without it the sixteen draw
					// exactly the same sequence of lines.
					uint64_t seed = base_seed + id * 0x100000001b3ull;
					if(use_nrpa)
						s.RunNrpa(target, plan, seed);
					else
						s.RunRollouts(target, plan, 1000000, seed);
					std::lock_guard<std::mutex> lock(merge);
					for(const auto& x : s.Solutions())
						sols.push_back(x);
					merge_archive(s.Archive());
					if(use_nrpa && !s.LearnedPolicy().empty()) {
						for(const auto& [k2, w] : s.LearnedPolicy())
							merged_policy[k2] += w;
						++policy_workers;
					}
					ModeStats& m = use_nrpa ? nrpa : greedy;
					m.nodes += s.Stats().nodes;
					m.rollouts += s.Stats().rollout_count;
					m.cuts += s.Stats().novelty_cuts;
					m.turn_cuts += s.Stats().turn_cuts;
					for(int k = 0; k < 72; ++k)
						m.sp_final[k] += s.Stats().sp_final[k];
					m.sp_at_sum += s.Stats().sp_at_sum;
					m.sp_lines += s.Stats().sp_lines;
					m.reenter_rollouts += s.Stats().reenter_rollouts;
					m.reenter_fail += s.Stats().reenter_fail;
					m.reenter_base_sum += s.Stats().reenter_base_sum;
					m.refine_done += s.Stats().refine_done;
					m.refine_subrungs += s.Stats().refine_subrungs;
					m.refine_top_hits += s.Stats().refine_top_hits;
					m.constraint_cuts += s.Stats().constraint_cuts;
					m.guard_cuts += s.Stats().guard_cuts;
					m.self_negate_cuts += s.Stats().self_negate_cuts;
					m.guard_keep_cuts += s.Stats().guard_keep_cuts;
					m.adapts += s.Stats().nrpa_adapts;
					m.ctx_entries = (std::max)(m.ctx_entries,
											   s.Stats().ctx_entries);
					m.ctx_capped |= s.Stats().ctx_capped;
					m.macro_taken += s.Stats().macro_taken;
					m.macro_absorbed += s.Stats().macro_absorbed;
					m.macro_aborted += s.Stats().macro_aborted;
					m.qhat_decisions += s.Stats().qhat_decisions;
					m.qhat_first += s.Stats().qhat_first;
					m.qhat_playouts += s.Stats().qhat_playouts;
					m.qhat_fallback += s.Stats().qhat_fallback;
					m.qhat_reward_sum += s.Stats().qhat_reward_sum;
					m.qhat_nodes = (std::max)(m.qhat_nodes,
											  s.Stats().qhat_nodes);
					m.qhat_codes = (std::max)(m.qhat_codes,
											  s.Stats().qhat_codes);
					m.qhat_bytes += s.Stats().qhat_bytes;
					for(const BanditProbe& b : s.RootBandit()) {
						BanditProbe& agg = qhat_root[b.key];
						agg.key = b.key;
						if(!agg.code)
							agg.code = b.code;
						agg.n += b.n;
						agg.w += b.w;
						agg.nhat += b.nhat;
						agg.qhat_sum += b.qhat_sum;
					}
					m.hint_seen += s.Stats().hint_seen;
					m.hint_taken += s.Stats().hint_taken;
					m.hint_exact += s.Stats().hint_seen_exact;
					m.hint_sel += s.Stats().hint_seen_sel;
					m.peak_trunc += s.Stats().peak_truncations;
					m.tt_reexplored += s.Stats().tt_reexplored;
					m.AddRepeat(s.Stats());
					m.lm_h_sum += s.Stats().landmark_h_sum;
					m.lm_h_count += s.Stats().landmark_h_count;
					// The liveness of the four mechanisms.
					m.rec_roll_sum += s.Stats().rec_roll_sum;
					m.rec_roll_d0_sum += s.Stats().rec_roll_d0_sum;
					m.rec_roll_count += s.Stats().rec_roll_count;
					m.backward_sum += s.Stats().backward_sum;
					m.backward_count += s.Stats().backward_count;
					m.hindsight_goals += s.Stats().hindsight_goals;
					m.hindsight_quota_spent += s.Stats().hindsight_quota_spent;
					m.hindsight_quota_fresh += s.Stats().hindsight_quota_fresh;
					m.hindsight_adapts += s.Stats().hindsight_adapts;
					m.recipe_snaps += s.Stats().recipe_snaps;
					// Snapshots: the SIZE is the same for every worker (same graph,
					// same target), so we keep the largest; a worker that has not
					// refreshed yet would return zero.
					m.snap_products = (std::max)(m.snap_products,
												 s.Stats().snap_products);
					m.snap_useful = (std::max)(m.snap_useful,
											   s.Stats().snap_useful);
					m.snap_backward = (std::max)(m.snap_backward,
												 s.Stats().snap_backward);
					// The SIZE of the list is the same for all (same graph); the USES,
					// on the other hand, add up, since they are decisions.
					m.op_listed = (std::max)(m.op_listed,
											 s.Stats().op_bias_listed);
					m.op_offered += s.Stats().op_bias_offered;
					m.op_taken += s.Stats().op_bias_taken;
					for(int k = 0; k < 4; ++k)
						m.rr[k] += s.Stats().resolve_reached[k];
					m.burn_cuts += s.Stats().burn_cuts;
					m.goal_hits += s.Stats().goal_hits;
					m.overlap_ripped = (std::max)(m.overlap_ripped,
												  s.Stats().best_overlap_ripped);
					m.overlap = (std::max)(m.overlap, s.Stats().best_overlap);
					m.monsters = (std::max)(m.monsters, s.Stats().best_monsters);
					if(s.Stats().best_overlap > best_overlap) {
						best_board = s.Stats().best_board;
						best_path = s.Stats().best_path;
						best_mzone = s.Stats().best_mzone;
						best_szone = s.Stats().best_szone;
					}
					best_overlap = (std::max)(best_overlap, s.Stats().best_overlap);
					best_monsters = (std::max)(best_monsters, s.Stats().best_monsters);
					merge_joint(s.Stats(), {});
				} else {
					WorkerAbort("duel (NRPA rollouts)", err);
				}
			}
			ReportPoison("NRPA rollouts", la);
			la.Shutdown();
		};

		std::vector<std::thread> pool;
		for(unsigned i = 0; i < threads; ++i)
			pool.emplace_back(worker, i);
		for(auto& t : pool)
			t.join();
		// Mean of the weights: the workers' policies are additive logits of the
		// same scale, so their mean is the standard merge.
		if(policy_workers > 1)
			for(auto& [k2, w] : merged_policy)
				w /= static_cast<float>(policy_workers);
		prof::PrintPhase("rollouts");
		double secs = MsSince(t0) / 1000.0;
		spent += secs * 1000.0;
		if(greedy.rollouts)
			std::printf("  greedy+novelty    : %8llu rollouts "
									"%10llu states  %7llu novelty cuts      "
									"best %u/%zu, %u mon.\n",
						(unsigned long long)greedy.rollouts,
						(unsigned long long)greedy.nodes,
						(unsigned long long)greedy.cuts, greedy.overlap,
						target.codes.size(), greedy.monsters);
		if(nrpa.rollouts)
			std::printf("  NRPA              : %8llu rollouts "
									"%10llu states  %7llu adaptations         "
									"best %u/%zu, %u mon.\n",
						(unsigned long long)nrpa.rollouts,
						(unsigned long long)nrpa.nodes,
						(unsigned long long)nrpa.adapts, nrpa.overlap,
						target.codes.size(), nrpa.monsters);
		// The OPTIONS liveness triptych: at zero `picks` the catalogue is never
		// chosen (weight or applicability), and when `aborted` dominates it does
		// not match the prompts met. The exponent gain is absorbed/picks (target:
		// mean length - 1).
		if(greedy.macro_taken + nrpa.macro_taken + greedy.macro_aborted +
		   nrpa.macro_aborted)
			std::printf("      options : %llu prises, %llu "
									"decisions absorbees (%.1f/prise), %llu "
									"avortees\n",
						(unsigned long long)(greedy.macro_taken +
											 nrpa.macro_taken),
						(unsigned long long)(greedy.macro_absorbed +
											 nrpa.macro_absorbed),
						(greedy.macro_taken + nrpa.macro_taken)
							? double(greedy.macro_absorbed +
									 nrpa.macro_absorbed) /
								  double(greedy.macro_taken + nrpa.macro_taken)
							: 0.0,
						(unsigned long long)(greedy.macro_aborted +
											 nrpa.macro_aborted));
		// The liveness of the CONTEXTUAL LEVEL. Under path conditioning it is the
		// reading that says whether it had room to learn: at the cap it degrades
		// towards the global weight, and what is then measured is not the paper's
		// mechanism but a truncated version of it.
		if(opt.ctx_shrink >= 0 && nrpa.ctx_entries)
			std::printf("      niveau contextuel : %zu case(s) au "
									"plus grand worker%s  (conditionnement : "
									"%s)\n",
						nrpa.ctx_entries,
						nrpa.ctx_capped ? "  !! CEILING "
																"REACHED (--ctx-max)" : "",
"placed+hand");
		// --- THE HEAD BANDIT'S LIVENESS, AND ITS PROBE (--qhat) ---
		//
		// Three readings before any other. (1) `decisions` at zero = the bandit
		// never decided (zero depth, or the node cap reached from the start:
		// `fallback` says so). (2) the MEAN reward of the window: stuck at zero,
		// it means every rollout is worth the same and Q^ can separate nothing,
		// so the mechanism would be inert whatever happens. (3) the MEMORY, paid
		// per worker: it is the mechanism's only cost and it must not be tuned
		// blind.
		if(opt.qhat_depth && nrpa.qhat_playouts) {
			std::printf("      Q^ bandit (--qhat %u): %llu "
									"decisions of which %llu at the 1st, %llu "
									"rollouts in window, mean reward %.3f\n",
						opt.qhat_depth,
						(unsigned long long)nrpa.qhat_decisions,
						(unsigned long long)nrpa.qhat_first,
						(unsigned long long)nrpa.qhat_playouts,
						nrpa.qhat_reward_sum /
							double(nrpa.qhat_playouts ? nrpa.qhat_playouts : 1));
			std::printf("                       tree %zu node(s) "
									"in the largest worker, %zu code(s) in "
									"window, %.1f MB total%s\n",
						nrpa.qhat_nodes, nrpa.qhat_codes,
						double(nrpa.qhat_bytes) / (1024.0 * 1024.0),
						nrpa.qhat_fallback
							? "  !! NODE CEILING "
														"(--qhat-nodes)" : "");
		}
		// THE PROBE. The corpus agreement curve CANNOT judge Q^: it measures the
		// reproduction of a corpus containing ONLY good lines, whereas Q^ draws
		// its signal from the FAILURES. So this is the only free instrument that
		// answers "does the solver concentrate on the right opening?". If the
		// right target does not stand out clearly there, no measurement is useful.
		if(opt.qhat_depth && opt.qhat_probe && !qhat_root.empty()) {
			std::vector<BanditProbe> rows;
			rows.reserve(qhat_root.size());
			for(const auto& [k, b] : qhat_root)
				rows.push_back(b);
			auto val = [](const BanditProbe& b) {
				const double d = double(b.n) + double(b.nhat);
				return d > 0 ? (b.w + b.qhat_sum) / d : 0.0;
			};
			auto ligne = [&](const BanditProbe& b) {
				std::string name = b.code ? db.Name(b.code) : std::string("-");
				if(name.empty())
					name = std::to_string(b.code);
				std::printf("      %-38.38s %9u %6.3f %9u "
											"%6.3f %6.3f\n",
							name.c_str(), b.n,
							b.n ? b.w / double(b.n) : 0.0, b.nhat,
							b.nhat ? b.qhat_sum / double(b.nhat) : 0.0,
							val(b));
			};
			auto entete = [&](const char* titre) {
				std::printf("\n  --- %s ---\n", titre);
				std::printf("      %-38.38s %9s %6s %9s %6s "
											"%6s\n",
							"move", "n", "Q", "n^", "Q^", "val");
			};
			// TABLE 1: WHAT THE BANDIT DECIDES at the first decision. It is short
			// by nature (the openings of an idle prompt) and mostly serves to
			// check that the mechanism decides something at all.
			std::vector<BanditProbe> dec;
			for(const BanditProbe& b : rows)
				if(b.n)
					dec.push_back(b);
			std::sort(dec.begin(), dec.end(),
					  [&](const BanditProbe& a, const BanditProbe& b) {
						  return val(a) > val(b);
					  });
			entete("bandit probe: DECISIONS AT THE ROOT (s = {})");
			for(size_t i = 0; i < dec.size() && i < 8; ++i)
				ligne(dec[i]);
			// TABLE 2: THE PERMUTATION STATISTIC ITSELF, Q^({}, a), i.e. the mean
			// reward of the lines that played a, anywhere and in any order,
			// AVERAGED OVER ALL ROLLOUTS, the bad ones included. It is THE
			// reading: if a searcher's right target (Tenki) does not stand out
			// clearly after a few thousand rollouts, the mechanism separates
			// nothing and no measurement is useful. Moves with a starved sample size are
			// set aside: a mean over three rollouts is not a mean.
			uint32_t seuil = 0;
			for(const BanditProbe& b : rows)
				seuil = (std::max)(seuil, b.nhat);
			seuil = seuil / 100 + 1;   // 1 % of the most frequent move
			std::vector<BanditProbe> perm;
			for(const BanditProbe& b : rows)
				if(b.nhat >= seuil)
					perm.push_back(b);
			std::sort(perm.begin(), perm.end(),
					  [](const BanditProbe& a, const BanditProbe& b) {
						  const double qa =
							  a.nhat ? a.qhat_sum / double(a.nhat) : 0.0;
						  const double qb =
							  b.nhat ? b.qhat_sum / double(b.nhat) : 0.0;
						  return qa > qb;
					  });
			std::printf("\n  --- bandit probe: PERMUTATION Q^({}, "
									"a) over %zu move(s) with sample >= %u "
									"---\n",
						perm.size(), seuil);
			std::printf("      %-38.38s %9s %6s %9s %6s %6s\n",
						"move", "n", "Q", "n^", "Q^", "val");
			const size_t show = perm.size() < 24 ? perm.size() : size_t(24);
			for(size_t i = 0; i < show; ++i)
				ligne(perm[i]);
			if(perm.size() > show) {
				std::printf("      ... %zu more, including "
											"the WORST:\n",
							perm.size() - show);
				for(size_t i = perm.size() < 4 ? 0 : perm.size() - 4;
					i < perm.size(); ++i)
					ligne(perm[i]);
			}
		}
		// The liveness of ONLINE MINING. Three readings decide its fate: the number
		// of rounds (at zero, the run never had anything to mine), the final
		// catalogue (size and model loss, the same reading as the static
		// catalogue), and the DURATION of the mining, which runs inside the run's
		// budget: above a second, the living corpus should be made smaller or the
		// period longer.
		if(opt.options_online) {
			std::lock_guard<std::mutex> lock(online.mu);
			std::printf("      online options: %u mining "
									"round(s), living corpus %zu line(s) "
									"(%llu offered, %llu kept, %llu "
									"duplicates)\n",
						online.rounds, online.pool.size(),
						(unsigned long long)online.offered,
						(unsigned long long)online.kept,
						(unsigned long long)online.dups);
			if(online.rounds)
				std::printf("                       last "
											"catalogue: %zu macro(s) (avg "
											"%.1f, max %zu) over %zu line(s), "
											"model loss %.1f -> %.1f log10; "
											"mining %.0f ms average, %.0f ms "
											"worst\n",
							online.last_size, online.last_avglen,
							online.last_maxlen, online.last_lines,
							online.last_flat, online.last_opt,
							online.mine_ms_total / double(online.rounds),
							online.mine_ms_max);
			// The catalogue mined online OUTLIVES the rollouts: it is the best knowledge
			// of macros the run has, and the finisher's rooted rollouts must inherit it,
			// otherwise the run would disarm itself exactly when it converts. Nobody
			// re-mines after this point: the catalogue becomes static again.
			if(online.cat && online.cat->Size()) {
				final_online = online.cat;
				cfg.options = final_online.get();
			}
		}
		// What the LINE CONSTRAINTS cut, PER MODE. Three mechanisms active in every
		// disciplined run, and none was printed WHERE IT WORKS: `PrintCuts` only
		// covered the LDS passes, where they are zero by construction. `guard`
		// answers the open question; `constraint` says how many rollouts
		// --resolve/--summon-min kill; `turn` separates "decision budget too short"
		// from "the line overflows turn 1".
		for(int mi = 0; mi < 2; ++mi) {
			const ModeStats& m = mi ? nrpa : greedy;
			if(!m.rollouts)
				continue;
			std::printf("      %-7s cuts: constraint %llu, guard "
									"%llu, turn %llu   (%.0f%% of rollouts)\n",
						mi ? "NRPA" : "greedy",
						(unsigned long long)m.constraint_cuts,
						(unsigned long long)m.guard_cuts,
						(unsigned long long)m.turn_cuts,
						100.0 * double(m.constraint_cuts + m.guard_cuts +
									   m.turn_cuts) / double(m.rollouts));
			// Liveness of --no-self-negate.
			if(m.self_negate_cuts)
				std::printf("      %-7s discipline: %llu "
											"self-negation(s) removed\n", mi ? "NRPA" : "greedy",
							(unsigned long long)m.self_negate_cuts);
			// Liveness of --guard-keep on the rollout phase.
			if(m.guard_keep_cuts)
				std::printf("      %-7s discipline: %llu "
											"guard-resource activation(s) "
											"removed\n",
							mi ? "NRPA" : "greedy",
							(unsigned long long)m.guard_keep_cuts);
		}
		// PER-ROLLOUT PROGRESS PROFILE, and the verdict it returns is binary: a
		// single PEAK in the histogram = every rollout dies at the same rung of the
		// x* ladder (a NAMEABLE lock; look for the missing option at that rung); a
		// SPREAD = it is arity that kills, and the closed form (cost = Sigma
		// b^(l_i), dominated by b^(l_max)) says one must cut finer rather than look
		// for one more mechanism.
		for(int mi = 0; mi < 2; ++mi) {
			const ModeStats& m = mi ? nrpa : greedy;
			if(!m.sp_lines)
				continue;
			int hi = 71;
			while(hi > 0 && !m.sp_final[hi])
				--hi;
			int mode_k = 0;
			uint64_t mode_n = 0;
			std::printf("      %-7s progress profile (max "
									"SerialProgress per rollout, %llu "
									"measurements):\n",
						mi ? "NRPA" : "greedy",
						(unsigned long long)m.sp_lines);
			for(int k = 0; k <= hi; ++k) {
				if(!m.sp_final[k])
					continue;
				if(m.sp_final[k] > mode_n) {
					mode_n = m.sp_final[k];
					mode_k = k;
				}
				std::printf("        %2d unite(s) : %8llu  "
											"(%5.1f %%)\n", k,
							(unsigned long long)m.sp_final[k],
							100.0 * double(m.sp_final[k]) / double(m.sp_lines));
			}
			std::printf("        last decision that made "
									"progress: %.1f on average; verdict: %s\n",
						double(m.sp_at_sum) / double(m.sp_lines),
						2 * mode_n > m.sp_lines
							? "SINGLE PEAK - a "
														"nameable lock at "
														"this rung"
							: "DISPERSION  -  "
														"l'arite tue, couper "
														"plus fin");
		}
		// THE LIVENESS OF THE RETURN TO THE RUNG (--reenter). At zero re-entries
		// with the flag armed, the mechanism is inert (an empty archive or no
		// serialisation) and no search judge concerns it. A FAILED replay is not
		// noise: an archive path must replay from the root, and a failure is a
		// defect to look at.
		for(int mi = 0; mi < 2; ++mi) {
			const ModeStats& m = mi ? nrpa : greedy;
			if(!m.reenter_rollouts && !m.reenter_fail)
				continue;
			std::printf("      %-7s return to the rung: %llu "
									"rollout(s) re-entered (%.1f %% of "
									"rollouts), mean base %.1f unit(s)%s",
						mi ? "NRPA" : "greedy",
						(unsigned long long)m.reenter_rollouts,
						m.rollouts ? 100.0 * double(m.reenter_rollouts) /
										 double(m.rollouts)
								   : 0.0,
						m.reenter_rollouts
							? double(m.reenter_base_sum) /
								  double(m.reenter_rollouts)
							: 0.0,
						m.reenter_fail ? "" : "\n");
			if(m.reenter_fail)
				std::printf(", %llu replay(s) FAILED <-- "
											"defect\n",
							(unsigned long long)m.reenter_fail);
		}
		// Liveness of the refinement. `top_hits` at zero with refined workers = the
		// sub-ladder is posted but never walked.
		if(greedy.refine_done + nrpa.refine_done)
			std::printf("      refinement: %llu worker(s) "
									"re-serialised, %llu sub-rung(s) in "
									"total, %llu state(s) archived beyond the "
									"door\n",
						(unsigned long long)(greedy.refine_done +
											 nrpa.refine_done),
						(unsigned long long)(greedy.refine_subrungs +
											 nrpa.refine_subrungs),
						(unsigned long long)(greedy.refine_top_hits +
											 nrpa.refine_top_hits));
		std::printf("  total: %.1f s, at best %u of the %zu target "
							"cards, %u monster(s)\n", secs, best_overlap, target.codes.size(),
					best_monsters);
		if(!cfg.hint_cards.empty()) {
			const uint64_t hs = nrpa.hint_seen + greedy.hint_seen;
			const uint64_t hx = nrpa.hint_exact + greedy.hint_exact;
			const uint64_t hl = nrpa.hint_sel + greedy.hint_sel;
			std::printf("  hint visibility: legal in %llu "
									"state(s), taken %llu time(s)%s\n",
						(unsigned long long)hs,
						(unsigned long long)(nrpa.hint_taken + greedy.hint_taken),
						hs == 0 ? "  <-- NEVER LEGAL: "
														"the problem is "
														"material availabilit"
														"y, not the search"
								: "");
			// BREAKDOWN: without it this counter had THREE causes (the
			// subset-prompt identity, the run's quality and the volume of work) and
			// could therefore judge none of them. Only the `exact move` column talks
			// about a move; the `subsets` column designates the first code of a
			// selection, not the card engaged.
			std::printf("     of which EXACT move "
									"(idle/chain/position) %llu, subset "
									"(approximate identity) %llu\n",
						(unsigned long long)hx, (unsigned long long)hl);
		}
		// LIVENESS OF --adapt-to-peak: at zero the mechanism is INERT and no search
		// judge concerns it.
		if(cfg.adapt_to_peak)
			std::printf("  gradient tronque au pic : %llu pas "
									"retires\n",
						(unsigned long long)(nrpa.peak_trunc +
											 greedy.peak_trunc));
		// WORK REDONE BY THE TRANSPOSITION: states already seen but with a smaller
		// budget, hence RE-EXPANDED. Counting only the cut would not say whether the
		// mechanism pays.
		if(nrpa.tt_reexplored + greedy.tt_reexplored)
			std::printf("  transposition: %llu state(s) "
									"RE-EXPLORED for want of budget on the "
									"first visit\n",
						(unsigned long long)(nrpa.tt_reexplored +
											 greedy.tt_reexplored));
		// THE handrip diagnosis: do any rollouts reach even ONE required
		// resolution? Zero at >=1 = the rip is never legal/possible (a game
		// matter); some >=1 without >=3 = the complete sequence is out of the
		// sampling's reach (a search matter).
		if(!cons.resolve_min.empty()) {
			std::printf("  resolutions reached per rollout: >=1 "
									"%llu  >=2 %llu  >=3 %llu  >=4 %llu%s\n",
						(unsigned long long)(nrpa.rr[0] + greedy.rr[0]),
						(unsigned long long)(nrpa.rr[1] + greedy.rr[1]),
						(unsigned long long)(nrpa.rr[2] + greedy.rr[2]),
						(unsigned long long)(nrpa.rr[3] + greedy.rr[3]),
						(nrpa.rr[0] + greedy.rr[0]) == 0
							? "  <-- NEVER: rip "
														"illegal or out of "
														"reach from this "
														"start"
							: "");
			// The measurement that separates search from resources: how far do the lines
			// WITH COMPLETE RESOLUTIONS climb?
			std::printf("  best peak AT complete resolutions: "
									"%u/%zu\n",
						(std::max)(nrpa.overlap_ripped, greedy.overlap_ripped),
						target.codes.size());
		}
		// LANDMARKS: does the learned `h` GO DOWN? It is the mechanism's INTERNAL
		// criterion, and it is read here, in the rollout phase where the weight
		// works. Stuck to the total learned: the search achieves nothing. Stuck to
		// zero: the landmarks are too easy and do not guide. Without this line, a
		// live `h` is indistinguishable from an inert one.
		if(nrpa.lm_h_count + greedy.lm_h_count) {
			const double s0 = nrpa.lm_h_sum + greedy.lm_h_sum;
			const uint64_t n0 = nrpa.lm_h_count + greedy.lm_h_count;
			std::printf("  landmarks: mean h %.2f over %zu "
									"learned, %llu evaluation(s) in the "
									"rollouts\n",
						s0 / double(n0),
						landmark_graph.Items().size(),
						(unsigned long long)n0);
		}
		// THE LIVENESS OF THE FOUR MECHANISMS, in the phase where they work. The
		// question is not "was the flag on" but "did the distance REALLY decrease"
		// and "is the decomposition advancing": a mechanism that is live and inert
		// is the mistake to avoid.
		if(nrpa.recipe_snaps + greedy.recipe_snaps) {
			std::printf("  recipes: snapshot taken %llu time(s) - "
									"%llu product(s), %llu useful code(s), "
									"%llu backward subproduct(s)\n",
						(unsigned long long)(nrpa.recipe_snaps +
											 greedy.recipe_snaps),
						(unsigned long long)(std::max)(nrpa.snap_products,
													   greedy.snap_products),
						(unsigned long long)(std::max)(nrpa.snap_useful,
													   greedy.snap_useful),
						(unsigned long long)(std::max)(nrpa.snap_backward,
													   greedy.snap_backward));
		}
		// THE LIVENESS OF THE OPERATOR BIAS (--op-bias). Printed BEFORE any search
		// judge: at `offered = 0` the mechanism is INERT and one would measure the
		// same arm twice.
		if(opt.op_bias > 0.0) {
			const uint64_t off = nrpa.op_offered + greedy.op_offered;
			const uint64_t tak = nrpa.op_taken + greedy.op_taken;
			std::printf("  OPERATOR bias: %llu card(s) designated "
									"by the decomposition; %llu decision(s) "
									"offered one, %llu took it (%.2f %%)%s\n",
						(unsigned long long)(std::max)(nrpa.op_listed,
													   greedy.op_listed),
						(unsigned long long)off, (unsigned long long)tak,
						off ? 100.0 * double(tak) / double(off) : 0.0,
						off ? "" : "   <-- INERT: no judge concerns it");
		}
		if(nrpa.rec_roll_count + greedy.rec_roll_count) {
			const uint64_t n1 = nrpa.rec_roll_count + greedy.rec_roll_count;
			const double d = (nrpa.rec_roll_sum + greedy.rec_roll_sum) /
							 double(n1);
			const double d0 = (nrpa.rec_roll_d0_sum + greedy.rec_roll_d0_sum) /
							  double(n1);
			std::printf("  recipes: mean distance %.2f against "
									"%.2f at the start (%llu evaluation(s) in "
									"the rollouts)%s\n",
						d, d0, (unsigned long long)n1,
						d >= d0 ? "  <-- N'A PAS DECRU" : "");
		}
		if(nrpa.backward_count + greedy.backward_count) {
			const uint64_t n2 = nrpa.backward_count + greedy.backward_count;
			std::printf("  backward: %.2f subproduct(s) built on "
									"average out of %llu (%llu evaluation(s))\n",
						(nrpa.backward_sum + greedy.backward_sum) / double(n2),
						(unsigned long long)(std::max)(nrpa.snap_backward,
													   greedy.snap_backward),
						(unsigned long long)n2);
		}
		if(nrpa.hindsight_goals + greedy.hindsight_goals ||
		   opt.hindsight > 0.0) {
			std::printf("  hindsight: %llu substitute goal(s) "
									"kept, %llu adaptation(s)%s\n",
						(unsigned long long)(nrpa.hindsight_goals +
											 greedy.hindsight_goals),
						(unsigned long long)(nrpa.hindsight_adapts +
											 greedy.hindsight_adapts),
						(nrpa.hindsight_goals + greedy.hindsight_goals) == 0
							? "  <-- NONE: no "
														"extra-deck monster "
														"summoned"
							: "");
			// Breakdown: does hindsight reinforce the fusions that SPENT a tracked
			// quota? A measurement, not a fix.
			const uint64_t hq_sp = nrpa.hindsight_quota_spent +
								   greedy.hindsight_quota_spent;
			const uint64_t hq_fr = nrpa.hindsight_quota_fresh +
								   greedy.hindsight_quota_fresh;
			if(hq_sp + hq_fr)
				std::printf("      broken down by tracked "
											"quota: %llu committed with quota "
											"SPENT (%.0f %%), %llu fresh "
											"quotas\n",
							(unsigned long long)hq_sp,
							100.0 * double(hq_sp) / double(hq_sp + hq_fr),
							(unsigned long long)hq_fr);
		}
		// REPETITION PROBE: the instrument that separates "the 2nd copy is NEVER
		// ATTEMPTED" (the material was there, a sampling failure) from "it is ALWAYS
		// LOST" (the chain was consumed, an h failure). The two call for opposite
		// work and `best_overlap` does not separate them.
		if(opt.probe_repeat) {
			RepeatProbe both[4];
			for(int i = 0; i < 4; ++i) {
				// The two modes add up: same question, same unit.
				both[i] = nrpa.rep[i];
				const RepeatProbe& g = greedy.rep[i];
				if(!both[i].code)
					both[i].code = g.code;
				for(int k = 0; k < 5; ++k)
					both[i].reached[k] += g.reached[k];
				both[i].more_n += g.more_n;
				both[i].more_sum += g.more_sum;
				both[i].rest_sum += g.rest_sum;
				both[i].more_kept += g.more_kept;
				both[i].more_lost += g.more_lost;
				both[i].after_sum += g.after_sum;
				both[i].first_depth_sum += g.first_depth_sum;
				both[i].d0_samples += g.d0_samples;
				both[i].known |= g.known;
				if(g.more_n) {
					both[i].more_min = (std::min)(both[i].more_min, g.more_min);
					both[i].more_max = (std::max)(both[i].more_max, g.more_max);
				}
				if(g.d0 != 0xffffffffu &&
				   (both[i].d0 == 0xffffffffu || g.d0 > both[i].d0)) {
					both[i].d0 = g.d0;
					both[i].rest0 = g.rest0;
				}
			}
			PrintRepeatProbe(both, nrpa.rollouts + greedy.rollouts, db,
							 true, true,
							 "ROLLOUT phase");
		}
		// The B&B bound no longer runs blind: goal reaches (post-goal re-reaches
		// included) and cuts.
		if(opt.optimize) {
			uint32_t bb = opt.burn_limit ? opt.burn_limit : UINT32_MAX;
			for(const Solution& s : sols)
				bb = (std::min)(bb, s.burned);
			char bstr[32] = "none";
			if(bb != UINT32_MAX)
				std::snprintf(bstr, sizeof(bstr), "%u (marge +%u)", bb,
							  opt.burn_slack);
			std::printf("  anytime: %llu goal hit(s), %llu "
									"burned-bound cut(s), best burned %s%s\n",
						(unsigned long long)(nrpa.goal_hits + greedy.goal_hits),
						(unsigned long long)(nrpa.burn_cuts + greedy.burn_cuts),
						bstr, opt.burn_share ? "" : "  [partage OFF]");
		}
		if(!sols.empty())
			std::printf("  %zu line(s) reaching the board.\n", sols.size());
	}

	// --- 4c. FINISHER. The rollouts know how to CLIMB (measured: three runs out
	// of three stop at 7-8 cards out of 8) but the last step (converting a body
	// into the missing card, correcting a position) is a needle sampling does not
	// find. Two engines (--finisher):
	//   - mono (the old one): a GUIDED search from the SINGLE best state, measured
	//     exhausted three times in ~6 states. The space is locked there from the
	//     summon on, and searching the final state cannot correct it;
	//   - levin (default): a Go-Explore archive (arXiv:2004.12919) + backtrack
	//     prefixes + Levin Tree Search (arXiv:2103.11505) over the NRPA policy, so
	//     K DISTINCT roots, some of them states from BEFORE the lock, and from
	//     each a complete best-first search ordered by the learned policy.
	//     --finisher ab: both at equal budget, the measurement.

	// Replays a prefix while counting what the search will need to know: summons
	// (constraints), turns (cut-off), resolutions (--resolve), actions (cost of
	// the complete solutions).
	struct PrefixCount {
		uint32_t actions = 0, summons = 0, turns = 0;
		uint64_t resolved = 0;
		size_t used = 0;
		bool ok = false;
	};
	auto replay_prefix = [&](Duel& fd,
							 const std::vector<std::vector<uint8_t>>& pre)
		-> PrefixCount {
		// The cost of a prefix replay (in the finisher, once per root) has its own
		// probe; its self time excludes the internal Process calls.
		prof::Scope ps(prof::kPrefix);
		PrefixCount pc;
		bool retry = false;
		while(pc.used < pre.size() && !retry) {
			int st = fd.Process();
			for(const Message& m : fd.Messages()) {
				switch(m.type) {
				case MSG_SUMMONING:
				case MSG_SPSUMMONING:
					++pc.summons;
					++pc.actions;
					// Watched summons (--summon-min) of the prefix.
					if(!cons.resolve_min.empty() && m.size >= 4) {
						uint32_t c = 0;
						std::memcpy(&c, m.data, 4);
						if(c) {
							const uint32_t sc = db.Canonical(c);
							for(size_t k = 0; k < cons.resolve_min.size(); ++k)
								if(cons.resolve_min[k].on_summon &&
								   cons.resolve_min[k].code == sc)
									pc.resolved += 1ull << (16 * k);
						}
					}
					break;
				case MSG_FLIPSUMMONING:
					++pc.actions;
					break;
				case MSG_CHAINING:
					if(!cons.resolve_min.empty() && m.size >= 4) {
						uint32_t c = 0;
						std::memcpy(&c, m.data, 4);
						c = db.Canonical(c);
						const uint32_t loc = ChainingLocation(m.data, m.size);
						for(size_t k = 0; k < cons.resolve_min.size(); ++k)
							if(!cons.resolve_min[k].on_summon &&
							   cons.resolve_min[k].code == c &&
							   (!cons.resolve_min[k].zones ||
								(loc & cons.resolve_min[k].zones)))
								pc.resolved += 1ull << (16 * k);
					}
					++pc.actions;
					break;
				case MSG_NEW_TURN:
					++pc.turns;
					break;
				case MSG_RETRY:
					retry = true;
					break;
				default:
					break;
				}
			}
			if(st == OCG_DUEL_STATUS_AWAITING)
				fd.SetResponse(pre[pc.used++]);
			else if(st != OCG_DUEL_STATUS_CONTINUE)
				break;
		}
		pc.ok = !retry && pc.used == pre.size();
		return pc;
	};

	// Solutions coming from APPROACH roots (--approach). They replay on the
	// approach's duel (its file's header), not necessarily on the starting duel:
	// on a hand test start (--start), the yrp1 header's pseudo-shuffle diverges
	// from the reload (measured: an 8/8 approach judges 239/239 but dies at
	// 39/239 when replayed on the starting duel). So they are written against
	// THEIR own header, and judged with the same --opp-hand, like any replay
	// produced.
	struct ApproachSols {
		std::unique_ptr<Replay> holder;
		std::string file;
		std::vector<Solution> sols;
	};
	std::vector<ApproachSols> approach_runs;

	// The mono finisher, kept behind --finisher mono|ab.
	auto run_mono = [&](double budget) {
		std::printf("\n--- mono finisher: guided search from the best "
							"state (%u/%zu, %zu decisions) ---\n", best_overlap,
					target.codes.size(), best_path.size());
		std::thread([&] {
			Arena fa;
			std::string err;
			if(!fa.Init(opt.arena_mb << 20, 0, err))
				return WorkerAbort("arena (guided mono "
												"finisher)", err);
			{
				Duel fd(db, scripts, &fa);
				if(!fd.Create(start_yrp.seed, start_yrp.duel_flags,
							  start_yrp.start_lp, start_yrp.start_hand,
							  start_yrp.draw_count, err) ||
				   !fd.Setup(start_yrp, err,
							 cons.opp_hand.empty() ? nullptr : &cons.opp_hand,
							 static_cast<uint8_t>(1 - opt.target_player)))
					return WorkerAbort("duel (guided mono "
														"finisher)", err);
				if(opt.stop_gc)
					fd.SetLuaGc(false);
				PrefixCount pc = replay_prefix(fd, best_path);
				if(!pc.ok) {
					std::printf("  !! the prefix does not "
													"replay (%zu/%zu): "
													"finisher cancelled\n", pc.used, best_path.size());
				} else {
					SearchConfig fcfg = cfg;
					fcfg.time_limit_ms = budget;
					fcfg.max_decisions =
						FinisherDepth(cfg.max_decisions, best_path.size());
					fcfg.initial_summons = pc.summons;
					fcfg.initial_turns = pc.turns;
					fcfg.initial_resolved = pc.resolved;
					fcfg.max_solutions = 8;
					Search fs(fd, fa, start_yrp, fcfg);
					fs.RunGuided(target);
					std::printf("  %llu states in %.1f s, "
													"at best %u/%zu  (%s)\n",
								(unsigned long long)fs.Stats().nodes,
								fs.Stats().ms / 1000.0,
								fs.Stats().best_overlap, target.codes.size(),
								SearchOutcome(fs.Stats()));
					PrintCuts([&] {
						CutCounts c;
						c.Add(fs.Stats());
						return c;
					}());
					for(Solution x : fs.Solutions()) {
						// The complete solution = prefix + the suffix found.
						std::vector<std::vector<uint8_t>> full = best_path;
						full.insert(full.end(), x.responses.begin(),
									x.responses.end());
						x.responses = std::move(full);
						x.decisions += static_cast<uint32_t>(best_path.size());
						x.actions += pc.actions;
						sols.push_back(std::move(x));
					}
				}
			}
			ReportPoison("guided mono finisher", fa);
			fa.Shutdown();
		}).join();
		prof::PrintPhase("mono finisher");
	};

	// The archive + LTS finisher: roots = the archive sorted by score, then the
	// best global path and its backtrack prefixes (N decisions removed, i.e.
	// states from BEFORE the lock, the practicable approximation of sqrt-LTS's
	// re-rooting, arXiv:2412.05196).
	auto run_levin = [&](double budget) {
		struct FinishRoot {
			std::string label;
			uint32_t overlap;   // 0 = unknown (backtrack)
			std::vector<std::vector<uint8_t>> pre;
		};
		std::vector<FinishRoot> roots;
		auto add_root = [&](std::string label, uint32_t overlap,
							std::vector<std::vector<uint8_t>> p) {
			if(p.empty())
				return;
			for(const FinishRoot& r : roots)
				if(r.pre == p)
					return;
			roots.push_back({ std::move(label), overlap, std::move(p) });
		};
		{
			std::vector<const ArchiveEntry*> ents;
			ents.reserve(global_archive.size());
			for(const auto& [cell, e] : global_archive)
				ents.push_back(&e);
			std::sort(ents.begin(), ents.end(),
					  [](const ArchiveEntry* a, const ArchiveEntry* b) {
						  return a->score > b->score;
					  });
			if(opt.archive_k && ents.size() > opt.archive_k)
				ents.resize(opt.archive_k);
			char lbl[48];
			for(size_t i = 0; i < ents.size(); ++i) {
				std::snprintf(lbl, sizeof(lbl), "archive %02zu "
																"%u/%zu r%u", i,
							  ents[i]->overlap, target.codes.size(),
							  ents[i]->resolves);
				add_root(lbl, ents[i]->overlap, ents[i]->path);
			}
			// The best archive states are massively LOCKED (measured: 1-3
			// expansions then exhaustion, since the lock is set at the summon, well
			// before the final state); their backtrack prefixes, on the other hand,
			// start before the lock. TOP-12: three cells only covered one summit of
			// the fine ladder, and the goal's conjunction (enablers + materials +
			// quota) can live in any of the frontier cells.
			// Backtracks 45/60: the frontier cells' vector carries the CONJUNCTION
			// (Leo@graveyard + enablers in play) but the state is locked, and Wolf's
			// once-per-turn quota, INVISIBLE to the vector, is spent ~30-50 answers
			// before the end. One has to backtrack to BEFORE the spending.
			// MARKER: the backtracks {15,30,45,60} and the TOP-12 are TUNED ON
			// BENCHMARK A and never swept, like the departure slices (2 x 15), the
			// tournament of 2 and the cap of 12 quota hosts. None is wrong; none is
			// derived. Only sweep them if a free judge asks for it.
			for(size_t i = 0; i < ents.size() && i < 12; ++i)
				for(uint32_t back : { 15u, 30u, 45u, 60u })
					if(ents[i]->path.size() > back) {
						std::snprintf(lbl, sizeof(lbl), "arch%02zu back %u", i,
									  back);
						add_root(lbl, 0,
								 std::vector<std::vector<uint8_t>>(
									 ents[i]->path.begin(),
									 ents[i]->path.end() - back));
					}
		}
		add_root("best", best_overlap, best_path);
		for(uint32_t back : { 5u, 10u, 20u, 40u, 60u, 90u })
			if(best_path.size() > back)
				add_root("back " + std::to_string(back), 0,
						 std::vector<std::vector<uint8_t>>(
							 best_path.begin(), best_path.end() - back));
		// Optimisation: the prefixes of the CHEAPEST solutions are the best roots,
		// since perturbing the end of a complete line that already costs little is
		// where a backtrack saving ONE burned card is a win.
		uint32_t best_known_burn = opt.burn_limit;
		if(opt.optimize && !sols.empty()) {
			std::vector<const Solution*> cheap;
			cheap.reserve(sols.size());
			for(const Solution& s : sols)
				cheap.push_back(&s);
			std::sort(cheap.begin(), cheap.end(),
					  [](const Solution* a, const Solution* b) {
						  return std::tie(a->burned, a->actions, a->decisions) <
								 std::tie(b->burned, b->actions, b->decisions);
					  });
			if(!best_known_burn || cheap[0]->burned < best_known_burn)
				best_known_burn = cheap[0]->burned;
			// The shared bound inherits the best pre-finisher cost.
			if(opt.burn_share && best_known_burn) {
				uint32_t cur = shared_burn.load();
				while(best_known_burn < cur &&
					  !shared_burn.compare_exchange_weak(cur, best_known_burn)) {}
			}
			char slbl[48];
			for(size_t i = 0; i < cheap.size() && i < 3; ++i)
				for(uint32_t back : { 30u, 60u, 90u, 120u })
					if(cheap[i]->responses.size() > back) {
						std::snprintf(slbl, sizeof(slbl), "sol%zu(b%u) back %u",
									  i, cheap[i]->burned, back);
						add_root(slbl, 0,
								 std::vector<std::vector<uint8_t>>(
									 cheap[i]->responses.begin(),
									 cheap[i]->responses.end() - back));
					}
		}
		if(roots.empty() && opt.approach_files.empty())
			return;
		std::printf("\n--- finisher: LTS over %zu root(s) + %zu "
							"approach(es) (archive %zu cells, policy %zu "
							"weights) ---\n",
					roots.size(), opt.approach_files.size(),
					global_archive.size(), merged_policy.size());
		auto t0 = Clock::now();
		std::atomic<size_t> next_root{ 0 };
		std::atomic<uint32_t> found{ 0 };
		// In optimisation, "a few solutions are enough" no longer exists: every
		// remaining root can carry a CHEAPER line.
		const uint32_t found_stop = opt.optimize ? 0x7fffffffu : 4u;
		std::mutex fmx;
		// Counters of the B&B bound, aggregated over every finisher root (LTS +
		// rooted rollouts).
		std::atomic<uint64_t> fin_goal_hits{ 0 }, fin_burn_cuts{ 0 };
		// REPETITION PROBE, FINISHER side. The rollout phase's probe does not see
		// the ROOTED rollouts, and that is exactly the instrument trap: on benchmark
		// A a second Liger was once found there, invisible in the summary line. A
		// probe covering only the rollout phase would conclude "never" on a run that
		// gets there. Protected by `fmx`, like the printing.
		RepeatProbe fin_rep[4];
		uint64_t fin_rollouts = 0;

		// --- phase 1: approach roots, each on ITS OWN duel (see ApproachSols). Two
		// engines, chosen by the backtrack's depth: SHORT backtracks go to the LTS,
		// where exhaustion is a PROOF of absence in a few seconds; DEEP backtracks
		// go to the rooted NRPA rollouts (phase A2 below), since the systematic
		// search dies on budget 60-150 decisions from the goal, and deep sampling is
		// made for that.
		for(size_t a = 0; a < opt.approach_files.size(); ++a) {
			auto holder = std::make_unique<Replay>();
			std::string aerr;
			if(!holder->Load(opt.approach_files[a], aerr)) {
				std::printf("  !! --approach %s : %s\n",
							opt.approach_files[a].c_str(), aerr.c_str());
				continue;
			}
			approach_runs.push_back(
				{ std::move(holder), opt.approach_files[a], {} });
		}
		const double a1_deadline = budget * 0.2;
		const double a2_deadline = budget * 0.75;
		std::printf("  approach phase split: A1 %.0f s (0.20), A2 "
							"%.0f s (0.75), rest %.0f s\n", a1_deadline / 1000.0,
					a2_deadline / 1000.0, (budget - a2_deadline) / 1000.0);
		for(size_t a = 0; a < approach_runs.size(); ++a) {
			ApproachSols& AR = approach_runs[a];
			const Replay* ap = AR.holder->IsStreamed() ? AR.holder->Embedded()
													   : AR.holder.get();
			if(!ap || ap->responses.empty()) {
				std::printf("  !! --approach %s: no readable "
											"answers\n",
							AR.file.c_str());
				continue;
			}
			// Short backtracks only: the LTS exhausts the space there (a proof);
			// deep backtracks go to sampling (phase A2).
			const std::vector<uint32_t> backs{ 0u, 10u, 20u, 30u, 45u };
			std::atomic<size_t> anext{ 0 };
			unsigned anw = (std::min<unsigned>)(
				threads, static_cast<unsigned>(backs.size()));
			std::vector<std::thread> apool;
			for(unsigned w = 0; w < anw; ++w) {
				apool.emplace_back([&] {
					Arena fa;
					std::string err;
					if(!fa.Init(opt.arena_mb << 20, 0, err))
						return WorkerAbort("arena (--approach "
																"finisher)", err);
					{
						Duel fd(db, scripts, &fa);
						if(fd.Create(ap->seed, ap->duel_flags, ap->start_lp,
									 ap->start_hand, ap->draw_count, err) &&
						   fd.Setup(*ap, err,
									cons.opp_hand.empty() ? nullptr
														  : &cons.opp_hand,
									static_cast<uint8_t>(
										1 - opt.target_player))) {
							if(opt.stop_gc)
								fd.SetLuaGc(false);
							fa.Push();
							for(;;) {
								size_t i = anext.fetch_add(1);
								if(i >= backs.size())
									break;
								double left = a1_deadline - MsSince(t0);
								if(left < 2000 || found.load() >= found_stop)
									break;
								const uint32_t back = backs[i];
								if(ap->responses.size() <= back)
									continue;
								fa.Restore();
								std::vector<std::vector<uint8_t>> pre(
									ap->responses.begin(),
									ap->responses.end() - back);
								PrefixCount pc = replay_prefix(fd, pre);
								char lbl[48];
								std::snprintf(lbl, sizeof(lbl),
											  "appr%zu back %u", a, back);
								if(!pc.ok) {
									std::lock_guard<std::mutex> lk(fmx);
									std::printf("  %-14s !! prefixe "
																					"non rejouable "
																					"(%zu/%zu)\n", lbl,
												pc.used, pre.size());
									continue;
								}
								SearchConfig fcfg = cfg;
								fcfg.time_limit_ms = left;
								fcfg.max_decisions =
									FinisherDepth(cfg.max_decisions, pre.size());
								fcfg.initial_summons = pc.summons;
								fcfg.initial_turns = pc.turns;
								fcfg.initial_resolved = pc.resolved;
								fcfg.max_solutions = opt.optimize ? 12 : 4;
								if(opt.optimize && best_known_burn)
									fcfg.burn_limit = best_known_burn;
								Search fs(fd, fa, start_yrp, fcfg);
								fs.RunLevin(target, plan, merged_policy);
								const SearchStats& st = fs.Stats();
								fin_goal_hits += st.goal_hits;
								fin_burn_cuts += st.burn_cuts;
								std::lock_guard<std::mutex> lk(fmx);
								std::printf("  %-14s %9llu exp. "
																			"%7.1f s  best "
																			"%u/%zu  b=%llu  "
																			"%s%s\n", lbl,
											(unsigned long long)st.nodes,
											st.ms / 1000.0, st.best_overlap,
											target.codes.size(),
											(unsigned long long)st.edges_skipped,
											SearchOutcome(st),
											fs.Solutions().empty() ? "" : "  <-- GOAL");
								for(Solution x : fs.Solutions()) {
									std::vector<std::vector<uint8_t>> full =
										pre;
									full.insert(full.end(),
												x.responses.begin(),
												x.responses.end());
									x.responses = std::move(full);
									x.decisions += static_cast<uint32_t>(
										pre.size());
									x.actions += pc.actions;
									AR.sols.push_back(std::move(x));
									found.fetch_add(1);
								}
								// The joint line of the LTS conversion too
								// (template checked): this is WHERE an approach
								// with complete rips closes.
								if(same_gabarit(ap)) {
									merge_joint(st, pre);
									// Full Go-Explore: the cells of the approach
									// LTS enter the global archive, re-rooted. Same
									// template guard as the joint line.
									if(opt.archive_fin)
										merge_rebased(fs.Archive(), pre);
								}
							}
							fa.Pop();
						} else {
							std::lock_guard<std::mutex> lk(fmx);
							std::printf("  !! duel of "
																	"approach %zu cannot "
																	"be initialised: %s\n", a, err.c_str());
						}
					}
					ReportPoison("finisher --approach", fa);
					fa.Shutdown();
				});
			}
			for(auto& t : apool)
				t.join();
			prof::PrintPhase("finisher approaches");
		}

		// --- phase A2: NRPA rollouts rooted on the DEEP backtracks of the
		// approaches. All the workers, split per root; best sequence shared PER
		// ROOT (different prefixes make the sequences incompatible between
		// roots); initial policy = the merged policy of the rollout phase; the
		// --resolve cards carry the hint bias. This is the pass that has a chance
		// of finding the whole suffix (rips + closing, ~60-150 decisions).
		{
			struct NrpaRoot {
				int64_t a;   // approach index; -1 = the STARTING duel
				uint32_t back;
				std::string label;
				std::vector<std::vector<uint8_t>> pre;
			};
			std::vector<NrpaRoot> roots2;
			char rlbl[48];
			// RIPPED archive states (starting duel): the roots that have already
			// crossed the lock. Closing the board from them is the class of problem
			// the engine can solve, and it is the missing measured bridge (tens of
			// thousands of lines at 3 rips on one side, mute 8/8 on the other, never
			// both).
			{
				std::vector<const ArchiveEntry*> ripped;
				const ArchiveEntry* deepest = nullptr;
				for(const auto& [cell, e] : global_archive)
					if(e.resolves > 0) {
						ripped.push_back(&e);
						// The most RIPPED cell is always a root: the score (sp_eff)
						// is dominated by the board rungs (~40 against <= 4 for
						// rips), so the top-3 by score can offer nothing but r1
						// cells. Measured on a diagnostic run: the three roots were
						// "5/6 r1" while an r2 cell existed further down. The max of
						// an axis is MEASURED, not a choice.
						if(!deepest || e.resolves > deepest->resolves ||
						   (e.resolves == deepest->resolves &&
							e.score > deepest->score))
							deepest = &e;
					}
				std::sort(ripped.begin(), ripped.end(),
						  [](const ArchiveEntry* x, const ArchiveEntry* y) {
							  return x->score > y->score;
						  });
				if(ripped.size() > 3)
					ripped.resize(3);
				if(deepest &&
				   std::find(ripped.begin(), ripped.end(), deepest) ==
					   ripped.end())
					ripped.push_back(deepest);
				for(size_t i = 0; i < ripped.size(); ++i) {
					// Backtracks DERIVED from the length of the archived path,
					// plus a benchmark constant ({0,20,40}: below the detour's
					// point of no return, ~60-90 decisions upstream on the
					// diagnostic run, where 200k rollouts at backtrack 0 gave zero
					// rips). L/12, L/6, L/3 cover re-preparing a rip (~8 %), a
					// manoeuvre (~17 %) and a third of a line, whatever the deck's
					// scale.
					const uint32_t plen =
						static_cast<uint32_t>(ripped[i]->path.size());
					uint32_t prev = ~0u;
					for(uint32_t back : { 0u, plen / 12u, plen / 6u,
										  plen / 3u }) {
						if(back == prev)
							continue;   // short paths: backtracks conflated
						prev = back;
						if(ripped[i]->path.size() > back) {
							std::snprintf(rlbl, sizeof(rlbl),
										  "rip%zu(%u/%zu r%u) "
																				"back %u", i,
										  ripped[i]->overlap,
										  target.codes.size(),
										  ripped[i]->resolves, back);
							roots2.push_back(
								{ -1, back, rlbl,
								  std::vector<std::vector<uint8_t>>(
									  ripped[i]->path.begin(),
									  ripped[i]->path.end() - back) });
						}
					}
				}
			}
			for(size_t a = 0; a < approach_runs.size(); ++a) {
				const Replay* ap = approach_runs[a].holder->IsStreamed()
					? approach_runs[a].holder->Embedded()
					: approach_runs[a].holder.get();
				if(!ap)
					continue;
				// Measured window: at backtrack 60 the complete rip sequence is
				// no longer playable (0 lines at 3 rips), at 70-80 it is (~4 000
				// per worker), so the grid covers the point of no return.
				for(uint32_t back : { 60u, 70u, 80u, 90u, 110u, 150u })
					if(ap->responses.size() > back) {
						std::snprintf(rlbl, sizeof(rlbl), "appr%zu back %u",
									  a, back);
						roots2.push_back(
							{ static_cast<int64_t>(a), back, rlbl,
							  std::vector<std::vector<uint8_t>>(
								  ap->responses.begin(),
								  ap->responses.end() - back) });
					}
			}
			// Optimisation: DEEP backtracks of the cheapest solutions (starting
			// duel). Restructuring the end of a line happens 60-150 decisions from
			// the goal, which is sampling's territory.
			if(opt.optimize && !sols.empty()) {
				std::vector<const Solution*> cheap;
				cheap.reserve(sols.size());
				for(const Solution& s : sols)
					cheap.push_back(&s);
				std::sort(cheap.begin(), cheap.end(),
						  [](const Solution* x, const Solution* y) {
							  return std::tie(x->burned, x->actions,
											  x->decisions) <
									 std::tie(y->burned, y->actions,
											  y->decisions);
						  });
				for(size_t i = 0; i < cheap.size() && i < 2; ++i)
					for(uint32_t back : { 60u, 90u, 120u, 150u })
						if(cheap[i]->responses.size() > back) {
							std::snprintf(rlbl, sizeof(rlbl),
										  "sol%zu(b%u) back %u", i,
										  cheap[i]->burned, back);
							roots2.push_back(
								{ -1, back, rlbl,
								  std::vector<std::vector<uint8_t>>(
									  cheap[i]->responses.begin(),
									  cheap[i]->responses.end() - back) });
						}
			}
			if(!roots2.empty() && found.load() < found_stop &&
			   a2_deadline - MsSince(t0) > 2000) {
				std::vector<std::unique_ptr<NrpaShared>> shared2;
				for(size_t i = 0; i < roots2.size(); ++i)
					shared2.push_back(std::make_unique<NrpaShared>());
				uint64_t fseed = opt.seed
					? opt.seed
					: static_cast<uint64_t>(
						  std::chrono::high_resolution_clock::now()
							  .time_since_epoch().count());
				// THE WORK QUEUE PER SOURCE. The frozen assignment r = w % N only
				// serves roots {0..min(W,N)-1}: on a closing run, 16 rip roots
				// occupied the 16 workers and the approach's 6 deep backtracks,
				// i.e. the MEASURED closing window (backtrack 70-80), received NO
				// budget. That is the starvation lemma: a combinatorial fact, not a
				// setting. A worker stays bound to ONE duel source (the same
				// pattern as phase A1: one duel per arena, Push once, Restore per
				// root) but DRAWS its roots from its group's shared queue, so every
				// root is served, budget is returned as in phase 2, and there are at
				// most three passes.
				struct RootGroup {
					int64_t a = -1;
					std::vector<size_t> idx;
					std::atomic<size_t> next{ 0 };
				};
				std::vector<std::unique_ptr<RootGroup>> groups;
				for(size_t i = 0; i < roots2.size(); ++i) {
					RootGroup* g = nullptr;
					for(auto& gg : groups)
						if(gg->a == roots2[i].a) { g = gg.get(); break; }
					if(!g) {
						groups.push_back(std::make_unique<RootGroup>());
						groups.back()->a = roots2[i].a;
						g = groups.back().get();
					}
					g->idx.push_back(i);
				}
				// Workers split between groups by highest quotient (D'Hondt):
				// proportional to the size, and every group is served before a big
				// one doubles its share.
				std::vector<RootGroup*> wplan(threads, nullptr);
				{
					std::vector<unsigned> got(groups.size(), 0);
					for(unsigned w = 0; w < threads; ++w) {
						size_t best_g = 0;
						double best_q = -1.0;
						for(size_t gi = 0; gi < groups.size(); ++gi) {
							const double q =
								double(groups[gi]->idx.size()) /
								double(got[gi] + 1);
							if(q > best_q) { best_q = q; best_g = gi; }
						}
						++got[best_g];
						wplan[w] = groups[best_g].get();
					}
				}
				std::vector<std::thread> npool;
				for(unsigned w = 0; w < threads; ++w) {
					npool.emplace_back([&, w] {
						// This worker's group (hence its duel SOURCE): the approach's
						// header, or the STARTING duel for the archive's ripped states.
						RootGroup* grp = wplan[w];
						if(!grp)
							return;
						const Replay* src = &start_yrp;
						if(grp->a >= 0) {
							src = approach_runs[static_cast<size_t>(grp->a)]
									  .holder->IsStreamed()
								? approach_runs[static_cast<size_t>(grp->a)]
									  .holder->Embedded()
								: approach_runs[static_cast<size_t>(grp->a)]
									  .holder.get();
							if(!src)
								return;
						}
						Arena fa;
						std::string err;
						if(!fa.Init(opt.arena_mb << 20, 0, err))
							return WorkerAbort("arena (finisher, "
																		"backtrack roots)", err);
						{
							Duel fd(db, scripts, &fa);
							if(fd.Create(src->seed, src->duel_flags,
										 src->start_lp, src->start_hand,
										 src->draw_count, err) &&
							   fd.Setup(*src, err,
										cons.opp_hand.empty()
											? nullptr : &cons.opp_hand,
										static_cast<uint8_t>(
											1 - opt.target_player))) {
								if(opt.stop_gc)
									fd.SetLuaGc(false);
								fa.Push();   // starting position of the source
								for(;;) {
									const size_t pk = grp->next.fetch_add(1);
									// At most three passes: beyond that one only
									// re-reinforces the same roots.
									if(pk >= grp->idx.size() * 3)
										break;
									double left = a2_deadline - MsSince(t0);
									if(left < 2000 ||
									   found.load() >= found_stop)
										break;
									const size_t r =
										grp->idx[pk % grp->idx.size()];
									const NrpaRoot& R = roots2[r];
									fa.Restore();
									PrefixCount pc = replay_prefix(fd, R.pre);
									if(!pc.ok) {
										std::lock_guard<std::mutex> lk(fmx);
										std::printf("  %-22s !! prefixe "
																							"non rejouable "
																							"(%zu/%zu)\n",
													R.label.c_str(), pc.used,
													R.pre.size());
									} else {
										SearchConfig fcfg = cfg;
										// Budget per root WITH RETURN (the shape of
										// phase 2): the remainder divided by the
										// remaining roots of the pass, so a root that
										// exhausts early gives its balance back.
										const size_t pass_left =
											grp->idx.size() -
											(pk % grp->idx.size());
										fcfg.time_limit_ms = (std::min)(
											left,
											(std::max)(2000.0,
													   left /
														   double(pass_left)));
										fcfg.max_decisions = FinisherDepth(
											cfg.max_decisions, R.pre.size());
										fcfg.initial_summons = pc.summons;
										fcfg.initial_turns = pc.turns;
										fcfg.initial_resolved = pc.resolved;
										fcfg.max_solutions =
											opt.optimize ? 12 : 4;
										if(opt.optimize && best_known_burn)
											fcfg.burn_limit = best_known_burn;
										// Shared bound here too: the roots aim at the
										// same board.
										if(opt.optimize && opt.burn_share)
											fcfg.shared_burn = &shared_burn;
										fcfg.nrpa_shared = shared2[r].get();
										fcfg.nrpa_init = &merged_policy;
										fcfg.nrpa_restart_keep =
											static_cast<float>(opt.nrpa_keep);
										Search fs(fd, fa, start_yrp, fcfg);
										fs.RunNrpa(target, plan,
												   fseed +
													   w * 0x9E3779B97F4A7C15ull +
													   pk * 0x100000001b3ull +
													   1);
										const SearchStats& st = fs.Stats();
										fin_goal_hits += st.goal_hits;
										fin_burn_cuts += st.burn_cuts;
										std::lock_guard<std::mutex> lk(fmx);
										if(opt.probe_repeat) {
											fin_rollouts += st.rollout_count;
											for(int q = 0; q < 4; ++q) {
												const RepeatProbe& s0 = st.rep[q];
												RepeatProbe& a0 = fin_rep[q];
												if(!a0.code)
													a0.code = s0.code;
												for(int k = 0; k < 5; ++k)
													a0.reached[k] += s0.reached[k];
												a0.more_n += s0.more_n;
												a0.more_sum += s0.more_sum;
												a0.rest_sum += s0.rest_sum;
												a0.more_kept += s0.more_kept;
												a0.more_lost += s0.more_lost;
												a0.after_sum += s0.after_sum;
												a0.first_depth_sum +=
													s0.first_depth_sum;
												a0.d0_samples += s0.d0_samples;
												a0.known |= s0.known;
												if(s0.more_n) {
													a0.more_min = (std::min)(
														a0.more_min, s0.more_min);
													a0.more_max = (std::max)(
														a0.more_max, s0.more_max);
												}
												if(s0.d0 != 0xffffffffu &&
												   (a0.d0 == 0xffffffffu ||
													s0.d0 > a0.d0)) {
													a0.d0 = s0.d0;
													a0.rest0 = s0.rest0;
												}
											}
										}
										std::printf(
											"  %-22s w%-2u %8llu "
																						"rollouts %9llu "
																						"states  best %u/%zu "
																						" rips %llu/%llu/%llu"
																						"  rip peak %u/%zu%s\n",
											R.label.c_str(), w,
											(unsigned long long)st.rollout_count,
											(unsigned long long)st.nodes,
											st.best_overlap,
											target.codes.size(),
											(unsigned long long)st.resolve_reached[0],
											(unsigned long long)st.resolve_reached[1],
											(unsigned long long)st.resolve_reached[2],
											st.best_overlap_ripped,
											target.codes.size(),
											fs.Solutions().empty()
												? "" : "  <-- BUT");
										for(Solution x : fs.Solutions()) {
											std::vector<std::vector<uint8_t>>
												full = R.pre;
											full.insert(full.end(),
														x.responses.begin(),
														x.responses.end());
											x.responses = std::move(full);
											x.decisions +=
												static_cast<uint32_t>(
													R.pre.size());
											x.actions += pc.actions;
											if(R.a >= 0)
												approach_runs[static_cast<size_t>(
																  R.a)]
													.sols.push_back(
														std::move(x));
											else
												sols.push_back(std::move(x));
											found.fetch_add(1);
										}
										// A root of the STARTING duel that improves the
										// peak is worth keeping (a complete
										// best_approach).
										if(R.a < 0 &&
										   st.best_overlap > best_overlap) {
											best_overlap = st.best_overlap;
											best_board = st.best_board;
											best_mzone = st.best_mzone;
											best_szone = st.best_szone;
											best_path = R.pre;
											best_path.insert(
												best_path.end(),
												st.best_path.begin(),
												st.best_path.end());
										}
										// The joint line too, from the STARTING duel or
										// from an approach with the SAME template
										// (checked, not assumed).
										// And the cells of those rooted rollouts
										// (--archive-fin): this is the phase that
										// produces the ripped lines, exactly the ones
										// the archive has never seen.
										if(R.a < 0) {
											merge_joint(st, R.pre);
											if(opt.archive_fin)
												merge_rebased(fs.Archive(),
															  R.pre);
										} else {
											ApproachSols& JAR = approach_runs
												[static_cast<size_t>(R.a)];
											const Replay* asrc =
												JAR.holder->IsStreamed()
													? JAR.holder->Embedded()
													: JAR.holder.get();
											if(same_gabarit(asrc)) {
												merge_joint(st, R.pre);
												if(opt.archive_fin)
													merge_rebased(fs.Archive(),
																  R.pre);
											}
										}
									}
								}
								fa.Pop();
							} else {
								WorkerAbort("duel (finisher, "
																			"backtrack roots)", err);
							}
						}
						ReportPoison("finisher, backtrack "
															"roots", fa);
						fa.Shutdown();
					});
				}
				for(auto& t : npool)
					t.join();
				prof::PrintPhase("A2 rollouts (backtracks)");
			}
		}

		// --- phase 2: roots on the starting duel.
		unsigned nw = (std::min<unsigned>)(
			threads, static_cast<unsigned>(roots.size()));
		std::vector<std::thread> pool;
		for(unsigned w = 0; w < nw; ++w) {
			pool.emplace_back([&] {
				Arena fa;
				std::string err;
				if(!fa.Init(opt.arena_mb << 20, 0, err))
					return WorkerAbort("arena (finisher, "
														"phase 2)", err);
				{
					Duel fd(db, scripts, &fa);
					if(fd.Create(start_yrp.seed, start_yrp.duel_flags,
								 start_yrp.start_lp, start_yrp.start_hand,
								 start_yrp.draw_count, err) &&
					   fd.Setup(start_yrp, err,
								cons.opp_hand.empty() ? nullptr : &cons.opp_hand,
								static_cast<uint8_t>(1 - opt.target_player))) {
						if(opt.stop_gc)
							fd.SetLuaGc(false);
						// Base of the reserve departures (zone 5) for the root probe:
						// the RESERVE at the duel's start.
						const uint32_t root_res0 =
							fd.Count(static_cast<uint8_t>(opt.target_player),
									 0x01u) +
							fd.Count(static_cast<uint8_t>(opt.target_player),
									 0x40u);
						fa.Push();   // starting position of the duel
						for(;;) {
							size_t i = next_root.fetch_add(1);
							if(i >= roots.size())
								break;
							double left = budget - MsSince(t0);
							// A few solutions are enough: the remaining roots
							// would only bring variants. (In optimisation:
							// never enough, hence found_stop.)
							if(left < 2000 || found.load() >= found_stop)
								break;
							fa.Restore();
							PrefixCount pc = replay_prefix(fd, roots[i].pre);
							if(!pc.ok) {
								std::lock_guard<std::mutex> lk(fmx);
								std::printf("  %-14s !! prefixe "
																			"non rejouable "
																			"(%zu/%zu)\n",
											roots[i].label.c_str(), pc.used,
											roots[i].pre.size());
								continue;
							}
							// ROOT PROBE: WHICH rungs this state serves, i.e. the
							// packed vector, 4 bits per requirement in wiring order.
							// It is the answer to "does the goal's conjunction exist
							// in a cell?", readable root by root.
							char spv[40] = "";
							if(!cfg.serial_reqs.empty()) {
								uint64_t pk = 0;
								SerialProgress(
									fd,
									static_cast<uint8_t>(opt.target_player),
									cfg.serial_reqs, db, root_res0, &pk);
								std::snprintf(spv, sizeof spv, " sp=%013llx",
											  (unsigned long long)pk);
							}
							SearchConfig fcfg = cfg;
							// PER-ROOT CEILING, with return. The first backtrack
							// root ate the whole budget (138-399 s measured) and
							// the following roots NEVER ran. Each root receives
							// the remainder divided by the remaining roots (floor
							// 2 s); a root that exhausts early RETURNS its balance
							// to the following ones through the re-reading of
							// `left`. Under several workers the denominator is
							// approximate; the return stays exact.
							const size_t roots_left =
								roots.size() > i ? roots.size() - i : 1;
							fcfg.time_limit_ms = (std::min)(
								left,
								(std::max)(2000.0,
										   left / double(roots_left)));
							fcfg.max_decisions = FinisherDepth(
								cfg.max_decisions, roots[i].pre.size());
							fcfg.initial_summons = pc.summons;
							fcfg.initial_turns = pc.turns;
							fcfg.initial_resolved = pc.resolved;
							fcfg.max_solutions = opt.optimize ? 12 : 4;
							if(opt.optimize && best_known_burn)
								fcfg.burn_limit = best_known_burn;
							Search fs(fd, fa, start_yrp, fcfg);
							fs.RunLevin(target, plan, merged_policy);
							const SearchStats& st = fs.Stats();
							fin_goal_hits += st.goal_hits;
							fin_burn_cuts += st.burn_cuts;
							std::lock_guard<std::mutex> lk(fmx);
							std::printf("  %-14s %9llu exp. "
																	"%7.1f s  best "
																	"%u/%zu%s  b=%llu  "
																	"%s%s\n",
										roots[i].label.c_str(),
										(unsigned long long)st.nodes,
										st.ms / 1000.0, st.best_overlap,
										target.codes.size(),
										spv,
										(unsigned long long)st.edges_skipped,
										SearchOutcome(st),
										fs.Solutions().empty() ? "" : "  <-- GOAL");
							for(Solution x : fs.Solutions()) {
								std::vector<std::vector<uint8_t>> full =
									roots[i].pre;
								full.insert(full.end(), x.responses.begin(),
											x.responses.end());
								x.responses = std::move(full);
								x.decisions += static_cast<uint32_t>(
									roots[i].pre.size());
								x.actions += pc.actions;
								sols.push_back(std::move(x));
								found.fetch_add(1);
							}
							// An approach improved by the finisher is worth
							// keeping (best_approach), with the COMPLETE path,
							// prefix included.
							if(st.best_overlap > best_overlap) {
								best_overlap = st.best_overlap;
								best_board = st.best_board;
								best_mzone = st.best_mzone;
								best_szone = st.best_szone;
								best_path = roots[i].pre;
								best_path.insert(best_path.end(),
												 st.best_path.begin(),
												 st.best_path.end());
							}
							merge_joint(st, roots[i].pre);
							// Full Go-Explore: starting duel, so direct
							// re-rooting and no template to check.
							if(opt.archive_fin)
								merge_rebased(fs.Archive(), roots[i].pre);
						}
						fa.Pop();
					} else {
						WorkerAbort("duel (finisher, "
															"phase 2)", err);
					}
				}
				ReportPoison("finisher, phase 2", fa);
				fa.Shutdown();
			});
		}
		for(auto& t : pool)
			t.join();
		prof::PrintPhase("finisher starting duel");
		// The merge's liveness (--archive-fin): ALWAYS stated when the
		// mechanism is armed, "+0" included. A finisher that brings no cell is
		// information (empty archives? template refused?), not silence.
		if(opt.archive_fin)
			std::printf("  ARCHIVE DU FINISSEUR (s24) : +%zu "
									"cellule(s) nouvelle(s), %zu amelioree(s) "
									" -  archive globale %zu\n",
						fin_cells_new, fin_cells_upd, global_archive.size());
		// REPETITION PROBE, FINISHER side: the ROOTED rollouts are invisible in
		// the rollout phase's table, and that is precisely where a second Liger
		// was once found. Without this second table, a "never" would read as a
		// never for the whole RUN.
		if(opt.probe_repeat && fin_rollouts)
			PrintRepeatProbe(fin_rep, fin_rollouts, db,
							 true, true,
							 "ROOTED finisher "
														"rollouts");
		// Summary of the finisher's B&B bound.
		if(opt.optimize)
			std::printf("  finisher: %llu goal hit(s), %llu "
									"burned-bound cut(s)%s\n",
						(unsigned long long)fin_goal_hits.load(),
						(unsigned long long)fin_burn_cuts.load(),
						opt.burn_share ? "" : "  [partage OFF]");
	};

	// In optimisation, the finisher runs EVEN when the rollouts have lines: the
	// prefixes of the cheapest solutions are its roots, and a backtrack that
	// saves one burned card is a win.
	if((sols.empty() || opt.optimize) &&
	   (spent < opt.solve_ms || opt.finisher_min > 0) &&
	   (!best_path.empty() || !global_archive.empty() ||
		!opt.approach_files.empty() || !sols.empty())) {
		double budget = opt.finisher_min > 0
			? (std::max)(opt.finisher_min, (opt.solve_ms - spent) * 0.8)
			: (std::min)((opt.solve_ms - spent) * 0.8, 240000.0);
		std::printf("  finisher budget: %.0f s out of %.0f s left "
							"(0.8x%s)\n",
					budget / 1000.0, (opt.solve_ms - spent) / 1000.0,
					opt.finisher_min > 0 ? ", plancher "
															"--finisher-min"
										 : ", capped at 240 s");
		auto t0 = Clock::now();
		if(opt.finisher == "mono") {
			if(!best_path.empty())
				run_mono(budget);
		} else if(opt.finisher == "ab") {
			// The measurement: both engines, equal budget, same starting roots (the
			// mono one only knows one, and that is precisely what is being
			// measured).
			size_t before = sols.size();
			if(!best_path.empty())
				run_mono(budget / 2);
			std::printf("  [A/B] mono : %zu solution(s)\n", sols.size() - before);
			before = sols.size();
			run_levin(budget / 2);
			std::printf("  [A/B] levin : %zu solution(s)\n", sols.size() - before);
		} else {
			run_levin(budget);
		}
		spent += MsSince(t0);
		if(!sols.empty())
			std::printf("  %zu line(s) reaching the board through "
									"the finisher.\n",
						sols.size());
	}

	// Solutions of the approach roots: written against THEIR file's header (see
	// ApproachSols), verification before writing included, on the approach's
	// duel, augmented with the same --opp-hand.
	bool approach_found = false;
	for(ApproachSols& AR : approach_runs) {
		if(AR.sols.empty())
			continue;
		approach_found = true;
		std::sort(AR.sols.begin(), AR.sols.end(),
				  [](const Solution& x, const Solution& y) {
					  if(x.burned != y.burned) return x.burned < y.burned;
					  if(x.actions != y.actions) return x.actions < y.actions;
					  return x.decisions < y.decisions;
				  });
		const Replay* ap = AR.holder->IsStreamed() ? AR.holder->Embedded()
												   : AR.holder.get();
		std::printf("\n  %zu line(s) through approach %s\n"
							"  (replayable on THIS file, with the same "
							"--opp-hand):\n",
					AR.sols.size(), AR.file.c_str());
		std::printf("  %-4s %10s %9s %11s\n", "#", "burned", "actions",
					"decisions");
		for(size_t i = 0; i < AR.sols.size() && i < 8; ++i)
			std::printf("  %-4zu %10u %9u %11u\n", i, AR.sols[i].burned,
						AR.sols[i].actions, AR.sols[i].decisions);
		WriteSolutions(AR.sols, *ap, target, opt, db, scripts, opt.outdir,
					   cons, cons.opp_hand.empty() ? nullptr : &cons.opp_hand);
	}

	if(!sols.empty()) {
		// Dedup by path: the anytime workers often converge on the same line, and
		// writing it sixteen times brings nothing.
		{
			std::unordered_set<uint64_t> seen;
			std::vector<Solution> uniq;
			uniq.reserve(sols.size());
			for(Solution& s : sols) {
				uint64_t h = 1469598103934665603ull;
				for(const auto& r : s.responses) {
					for(uint8_t b : r) {
						h ^= b;
						h *= 1099511628211ull;
					}
					h ^= 0xff;
					h *= 1099511628211ull;
				}
				if(seen.insert(h).second)
					uniq.push_back(std::move(s));
			}
			if(uniq.size() < sols.size())
				std::printf("\n  %zu distinct line(s) (%zu "
											"duplicate paths merged)\n", uniq.size(),
							sols.size() - uniq.size());
			sols = std::move(uniq);
		}
		std::sort(sols.begin(), sols.end(), [](const Solution& a, const Solution& b) {
			if(a.burned != b.burned) return a.burned < b.burned;
			if(a.actions != b.actions) return a.actions < b.actions;
			return a.decisions < b.decisions;
		});
		std::printf("\n  %-4s %10s %9s %11s %8s %8s %8s\n", "#", "burned",
					"actions", "decisions", "main", "deck", "extra");
		for(size_t i = 0; i < sols.size() && i < 10; ++i) {
			const Solution& x = sols[i];
			std::printf("  %-4zu %10u %9u %11u %8u %8u %8u\n", i, x.burned,
						x.actions, x.decisions, x.hand_left, x.deck_left,
						x.extra_left);
		}
		std::printf("\n  %-4s %10u %9u %11zu   (reference, on its own "
							"deck)\n",
					"ref", ref_burned, ref_actions, ref_decisions);
		WriteSolutions(sols, start_yrp, target, opt, db, scripts, opt.outdir,
					   cons, cons.opp_hand.empty() ? nullptr : &cons.opp_hand);
		arena.Restore();
		return;
	}
	if(approach_found) {
		// The approach lines answer the question asked; the bounded-discrepancy
		// pass would only add variants to them.
		arena.Restore();
		return;
	}

	std::printf("\n--- bounded-discrepancy search around the plan ---\n");
	std::printf("  workers        : %u,  max depth %u decisions,  novelty "
					"%s (patience %u, strict)\n", threads,
				cfg.max_decisions, patience ? "active" : "desactivee", patience);
	std::printf("\n  %-8s %10s %12s %11s %10s %9s\n", "discrep.", "solutions",
				"states", "transpos.", "cuts", "time");

	uint32_t reached = 0;
	for(uint32_t k = 0; k <= opt.max_ecarts && spent < opt.solve_ms && sols.empty(); ++k) {
		reached = k;
		double budget = opt.solve_ms - spent;
		auto t0 = Clock::now();
		unsigned n = (k == 0) ? 1u : threads;
		// One token per DISTINCT STATE at the claim level, not per plan step: in
		// goal-only mode the plan is empty, and the earlier table then offered a
		// single cell for sixteen workers.
		//
		// Generous sizing (65 536 cells, 512 KB): the number of claim points is not
		// known in advance and runs into the thousands (every node of the prefix
		// opens a dozen deviations). At saturation the table forbids nothing, but it
		// stops PARTITIONING, so the workers start redoing the same work, and that
		// is what `Overflow()` says, printed below.
		ClaimTable claims(16384);
		// Shared transposition table for the pass (lazy SMP).
		std::unique_ptr<SharedTT> stt;
		if(opt.tt_mb && n > 1)
			stt = std::make_unique<SharedTT>(opt.tt_mb);
		std::mutex merge;
		std::vector<Solution> found;
		uint64_t nodes = 0, transpos = 0, cuts = 0;
		CutCounts cut;
		bool timed_out = false;

		auto worker = [&](unsigned) {
			// Each worker has its arena and its duel, and that duel is set up on the
			// STARTING replay, not on the reference.
			// (best_overlap / best_monsters come back through `merge`.)
			Arena local_arena;
			std::string err;
			if(!local_arena.Init(opt.arena_mb << 20, 0, err))
				return WorkerAbort("arena (transplantation)", err);
			{
				Duel local(db, scripts, &local_arena);
				if(local.Create(start_yrp.seed, start_yrp.duel_flags,
								start_yrp.start_lp, start_yrp.start_hand,
								start_yrp.draw_count, err) &&
				   local.Setup(start_yrp, err,
							   cons.opp_hand.empty() ? nullptr : &cons.opp_hand,
							   static_cast<uint8_t>(1 - opt.target_player))) {
					if(opt.stop_gc)
						local.SetLuaGc(false);
					SearchConfig wcfg = cfg;
					wcfg.time_limit_ms = budget;
					wcfg.novelty_patience = patience;
					// Strict: only facts never seen count. The bounded-discrepancy pass
					// looks for NEW material, not variants; and in depth-aware mode its
					// DFS (deep deviations first) made any shorter branch "new", 3 584
					// cuts over 1 M states, a facade of pruning.
					wcfg.novelty_strict = true;
					wcfg.trace = opt.verbose && k == 0;
					// The trace reads the labels; the hot paths no longer build them by
					// default.
					wcfg.enumeration.labels = wcfg.trace;
					wcfg.shared_tt = stt.get();
					if(n > 1) {
						wcfg.claim_level = (k <= 1) ? 0u : 1u;
						wcfg.claims = &claims;
					}
					Search s(local, local_arena, start_yrp, wcfg);
					s.RunTransplant(target, plan, k);
					std::lock_guard<std::mutex> lock(merge);
					for(const auto& x : s.Solutions())
						found.push_back(x);
					nodes += s.Stats().nodes;
					transpos += s.Stats().transpositions;
					cuts += s.Stats().novelty_cuts;
					cut.Add(s.Stats());
					timed_out |= s.Stats().hit_time_limit;
					if(s.Stats().best_overlap > best_overlap) {
						best_board = s.Stats().best_board;
						best_path = s.Stats().best_path;
						best_mzone = s.Stats().best_mzone;
						best_szone = s.Stats().best_szone;
					}
					best_overlap = (std::max)(best_overlap, s.Stats().best_overlap);
					best_monsters = (std::max)(best_monsters, s.Stats().best_monsters);
					merge_joint(s.Stats(), {});
				} else {
					std::lock_guard<std::mutex> lock(merge);
					std::printf("  !! starting duel "
													"cannot be initialised: "
													"%s\n",
								err.c_str());
				}
			}
			ReportPoison("transplantation", local_arena);
			local_arena.Shutdown();
		};

		std::vector<std::thread> pool;
		for(unsigned i = 0; i < n; ++i)
			pool.emplace_back(worker, i);
		for(auto& t : pool)
			t.join();
		if(prof::enabled) {
			char lbl[40];
			std::snprintf(lbl, sizeof(lbl), "plan discrep. k=%u", k);
			prof::PrintPhase(lbl);
		}

		double ms = MsSince(t0);
		spent += ms;
		std::printf("  %-8u %10zu %12llu %11llu %10llu %8.1f s   "
							"%u/%zu %u mon.%s\n",
					k, found.size(), (unsigned long long)nodes,
					(unsigned long long)transpos, (unsigned long long)cuts,
					ms / 1000.0, best_overlap,
					target.codes.size(), best_monsters,
					timed_out ? "  (budget epuise)" : "");
		PrintCuts(cut);
		if(claims.Overflow())
			std::printf("           !! partition saturated %llu "
									"time(s): the workers redo the same work\n",
						(unsigned long long)claims.Overflow());
		for(const auto& x : found)
			sols.push_back(x);
	}
	arena.Restore();

	if(sols.empty()) {
		std::printf("\n  NO line found up to %u discrepancy(ies).\n", reached);
		std::printf("  Best approach: %u of the %zu board cards "
							"assembled, %u monster(s) placed.\n", best_overlap, target.codes.size(),
					best_monsters);
		if(best_monsters < 2)
			std::printf("  This hand places almost nothing: the "
									"blockage is at the opening, not\n"
									"  in the search depth.\n");
		ReportBestBoard(best_board, target, db);

		// All the codes are there but the goal does not fire: the difference is a
		// DETAIL, i.e. position, materials or counters. We display it next to the
		// target, since it is the only information one can act on.
		// laquelle on puisse agir.
		if(best_overlap == target.codes.size() && !best_mzone.empty()) {
			auto dump = [&](const char* label,
							const std::vector<QueriedCard>& zone) {
				for(const auto& c : zone) {
					if(!c.present)
						continue;
					std::string extra;
					if(!c.overlay.empty())
						extra += " +" + std::to_string(c.overlay.size()) + " mat";
					if(!c.counters.empty())
						extra += " +compteurs";
					std::printf("      %s %9u  %-36.36s "
													"%-8s%s\n", label,
								c.Code(), db.Name(c.Code()).c_str(),
								PosName(c.position), extra.c_str());
				}
			};
			std::printf("\n  every code is there: the goal "
									"differs only in DETAIL.\n  best state:\n");
			dump("MZONE", best_mzone);
			dump("SZONE", best_szone);
			std::printf("  cible :\n");
			// THE REAL TARGET, not the template's. An empty S/T zone then reads as
			// what it is: a requirement of ABSENCE, which --target-subset lifts.
			// --target-subset leve.
			if(posed)
				std::printf("      (POSTED target: an S/T "
											"zone missing below is REQUIRED "
											"EMPTY,\n      unless "
											"--target-subset)\n");
			dump("MZONE", posed ? posed_mz : ref.target_self.mzone);
			dump("SZONE", posed ? posed_sz : ref.target_self.szone);
		}
		// The best approach is worth KEEPING: replayable in EDOPro, judgeable, and
		// reusable as a repertoire for a later run. It is NOT a solution, and
		// the name says so.
		if(!best_path.empty()) {
			std::error_code ec;
			std::filesystem::create_directories(opt.outdir, ec);
			char name[64];
			std::snprintf(name, sizeof(name), "best_approach_%uof%z"
														"u.yrp",
						  best_overlap, target.codes.size());
			std::string apath = opt.outdir + "/" + name;
			std::string werr;
			if(WriteYrp1(apath, start_yrp, best_path, werr))
				std::printf("\n  best approach written: %s "
											"(%zu decisions, NOT a solution)\n", apath.c_str(),
							best_path.size());
			else
				std::printf("  !! %s\n", werr.c_str());
		}
		// The best JOINT line: rips first, board second. Re-injectable through
		// --approach (the isolation instrument: the machinery converts when it is
		// close, 3/3 measured). It is the bridge between "lines with complete rips"
		// and "a board from them", which the counters alone let die with the run.
		if(best_joint_rp && !best_joint_path.empty()) {
			std::error_code ec;
			std::filesystem::create_directories(opt.outdir, ec);
			char name[64];
			std::snprintf(name, sizeof(name), "best_joint_%ur_%uof%"
														"zu.yrp",
						  best_joint_rp, best_joint_overlap,
						  target.codes.size());
			std::string jpath = opt.outdir + "/" + name;
			std::string werr;
			if(WriteYrp1(jpath, start_yrp, best_joint_path, werr)) {
				std::printf("  best JOINT line written: %s "
											"(%u rip(s), %u/%zu at the board, "
											"%zu decisions, NOT a solution)\n",
							jpath.c_str(), best_joint_rp, best_joint_overlap,
							target.codes.size(), best_joint_path.size());
				if(outres)
					outres->joint_file = jpath;
			} else
				std::printf("  !! %s\n", werr.c_str());
		}
		if(outres) {
			// The solutions of the approach roots count: a conversion coming from the
			// re-injected line must stop the internal loop.
			outres->solutions = sols.size();
			for(const ApproachSols& AR : approach_runs)
				outres->solutions += AR.sols.size();
			outres->best_overlap = best_overlap;
			outres->joint_rp = best_joint_rp;
			outres->joint_overlap = best_joint_overlap;
		}
		// The CARRY (--carry): everything this round learned, i.e. the global
		// archive (finisher included under --archive-fin) and the merged policy,
		// outlives the call for the next round.
		if(carry && opt.carry) {
			carry->archive = std::move(global_archive);
			carry->policy = std::move(merged_policy);
			carry->policy_workers = policy_workers;
		}
		return;
	}

	std::sort(sols.begin(), sols.end(), [](const Solution& a, const Solution& b) {
		if(a.burned != b.burned) return a.burned < b.burned;
		if(a.actions != b.actions) return a.actions < b.actions;
		return a.decisions < b.decisions;
	});
	std::printf("\n  %zu line(s) reaching the board from this deck.\n\n", sols.size());
	std::printf("  %-4s %10s %9s %11s %8s %8s %8s\n", "#", "burned", "actions",
				"decisions", "main", "deck", "extra");
	for(size_t i = 0; i < sols.size() && i < 10; ++i) {
		const Solution& x = sols[i];
		std::printf("  %-4zu %10u %9u %11u %8u %8u %8u\n", i, x.burned, x.actions,
					x.decisions, x.hand_left, x.deck_left, x.extra_left);
	}
	std::printf("\n  %-4s %10u %9u %11zu   (reference, on its own deck)\n", "ref",
				ref_burned, ref_actions, ref_decisions);
	WriteSolutions(sols, start_yrp, target, opt, db, scripts, opt.outdir,
				   cons, cons.opp_hand.empty() ? nullptr : &cons.opp_hand);
}

// GROWTH MEASUREMENT: the figure that decides everything.
//
// The size of the action tree (10^97) says nothing about feasibility: what
// matters is the number of DISTINCT states, after merging the paths that
// converge. Nobody can guess it, since it depends on the deck's structure. We
// measure it by expanding the graph exhaustively at increasing depth and
// watching the curve.
void RunGrowthMeasurement(Duel& duel, const Replay& yrp, const Options& opt,
						  Arena& arena, const LineResult& ref) {
	std::printf("\n=== growth curve of the state graph ===\n");
	std::printf("  The number of distinct states per depth decides "
					"whether\n  \"exhaustive\" is realistic. This is a "
					"measurement, not an estimate.\n\n");

	// Target board: the one at the end of the reference's turn.
	while(arena.Depth() > 1)
		arena.Pop();
	if(arena.Depth() == 0)
		arena.Push();
	arena.Restore();

	// We replay the line to capture the target board's key, then come back.
	BoardKey target;
	{
		size_t at = 0;
		at += Advance(duel, yrp, at, ref.target_at);
		target = ComputeBoardKey(duel, static_cast<uint8_t>(opt.target_player));
		arena.Restore();
	}
	std::printf("  target board: %zu cards, fingerprint %016llx\n\n",
				target.entries.size(), (unsigned long long)target.hash);

	// BOARDS AND NOT STATES. `states` is what the transposition distinguishes
	// (`StateDigest`: zones + prompt payload, hence the once-per-turn counters);
	// the next three columns count BOARDS, coarser and coarser, all indifferent
	// to position and column:
	//   b.exact = (zone, code, face, materials, counters)
	//   b.loose = (zone, code, face)
	//   b.codes = the codes alone
	// The states/b.exact ratio is the PRICE of the key's fineness.
	// `st.idle` / `b.idle`: the same, restricted to the STABLE points (idle
	// prompt). It is THEIR ratio that measures the price of the transposition
	// key; elsewhere we are in mid-resolution and two states with the same board
	// are legitimately distinct.
	std::printf("  %-6s %10s %8s %8s %9s %8s %9s %7s\n", "prof.", "states",
				"b.exact", "b.codes", "et.idle", "b.idle", "time", "statut");
	uint64_t prev = 0;
	for(uint32_t depth = 2; depth <= opt.growth_max; depth += 2) {
		SearchConfig cfg;
		ApplyMechanisms(opt, cfg);   // one single wiring point
		cfg.max_decisions = depth;
		cfg.time_limit_ms = opt.growth_ms;
		cfg.max_nodes = 5000000;
		cfg.enumeration.dedup_by_code = true;
		cfg.enumeration.max_subsets = opt.max_subsets;
		// The operator's question: how many BOARDS, not how many states.
		cfg.count_boards = true;
		// `--elide-forced` comes from `ApplyMechanisms`, like everywhere else, and
		// not from here: wired here and only here it would leave the search path
		// untouched. `--growth` is still the best place to MEASURE it (it returns
		// the depth reached at equal budget), not to wire it.
		// cabler.
		if(depth == 2)   // once, at the first depth step
			ReportMechanisms(cfg, "growth");

		Search search(duel, arena, yrp, cfg);
		search.Run(target);
		arena.Restore();

		const SearchStats& s = search.Stats();
		const char* status = s.hit_time_limit ? "temps"
							 : s.hit_node_limit ? "nodes" : "epuise";
		std::printf("  %-6u %10llu %8zu %8zu %9llu %8zu %6.0f ms %7s",
					depth, (unsigned long long)s.nodes, s.boards_entries,
					s.boards_codes, (unsigned long long)s.states_idle,
					s.boards_idle, s.ms, status);
		if(prev)
			std::printf("   x%.1f", double(s.nodes) / double(prev));
		std::printf("\n");
		prev = s.nodes;
		if(depth == opt.growth_max || s.hit_time_limit || s.hit_node_limit) {
			// Profile per depth: shows where the exploration really stops, and hence
			// whether saturation comes from a closed space or from a bound that bites.
			std::printf("\n  profile of the last pass (new states "
									"by depth):\n");
			for(size_t i = 0; i < s.distinct_by_depth.size(); ++i)
				if(s.distinct_by_depth[i])
					std::printf(" %zu:%llu", i,
								(unsigned long long)s.distinct_by_depth[i]);
			// REAL MERGE FACTOR of the transposition table: expansions / distinct, per
			// depth. `expansions_by_depth` was sized and incremented in four places
			// and read nowhere, yet it is the only figure that says whether the table
			// merges anything where the space explodes.
			std::printf("\n  merges by depth (expansions / "
									"distinct):\n");
			for(size_t i = 0; i < s.expansions_by_depth.size() &&
							  i < s.distinct_by_depth.size(); ++i)
				if(s.distinct_by_depth[i])
					std::printf(" %zu:x%.1f", i,
								double(s.expansions_by_depth[i]) /
									double(s.distinct_by_depth[i]));
			std::printf("\n  terminals: %llu   dead ends: %llu\n",
						(unsigned long long)s.terminals,
						(unsigned long long)s.dead_ends);
			// GLOBAL ATTRIBUTION: the breakdown of EVERY node. Without it, one fixes
			// the key on the strength of a count restricted to idle points, and an
			// attribution over a subset does not carry over to the whole.
			const uint64_t tot = s.nodes_forced + s.nodes_idle + s.nodes_multi;
			if(tot) {
				auto pc = [&](uint64_t v) { return 100.0 * double(v) / double(tot); };
				std::printf("\n  breakdown of expanded nodes:\n"
											"    FORCED (a single legal "
											"answer)    %10llu   %5.1f %%\n"
											"    idle   (stable decision "
											"point)    %10llu   %5.1f %%\n"
											"    other  (selections, chains)  "
											"     %10llu   %5.1f %%\n",
							(unsigned long long)s.nodes_forced, pc(s.nodes_forced),
							(unsigned long long)s.nodes_idle, pc(s.nodes_idle),
							(unsigned long long)s.nodes_multi, pc(s.nodes_multi));
				if(s.elided)
					std::printf("    of which PLAYED "
													"INLINE (--elide-forced) "
													"%7llu\n",
								(unsigned long long)s.elided);
			}
		}
		if(s.hit_time_limit || s.hit_node_limit) {
			std::printf("\n  Stopped: the budget is exhausted at "
									"depth %u already,\n"
									"  far short of the %u decisions of the "
									"reference line.\n",
						depth, (unsigned)ref.responses_used);
			break;
		}
	}
}

} // namespace

int main(int argc, char** argv) {
#if defined(__EMSCRIPTEN__)
	// UNDER WASM IT IS THE OPPOSITE: every unbuffered write is PROXIED to the
	// browser's main thread (emscripten routes stdout there from the pthreads).
	// The solver prints thousands of lines during the search; unbuffered, that
	// wakes the main thread constantly and steals time from the sixteen workers.
	// So we buffer generously, and `exit` flushes.
	static char stdout_buf[1 << 20];
	std::setvbuf(stdout, stdout_buf, _IOFBF, sizeof stdout_buf);
#else
	// Without this, a crash takes the tail of the buffer and hides the exact place.
	std::setvbuf(stdout, nullptr, _IONBF, 0);
#endif
	Options opt;
	if(!ParseArgs(argc, argv, opt)) {
		Usage();
		return opt.help ? 0 : 2;
	}
	g_verbose = opt.verbose;
	// Before any thread is created: publication of the flag rides on the workers'
	// launch.
	if(opt.profile)
		prof::Enable();
	// REPETITION PROBE: it measures DISTANCES over the recipe graph. Turning it on
	// with no graph would return only a histogram, mute on the one question asked,
	// the exact family of "a mechanism silently absent from the path". The
	// implication is applied HERE (after the whole command line, hence insensitive
	// to flag order) and it is STATED.
	if(opt.probe_repeat && opt.recipes < 0) {
		opt.recipes = 0.0;
		std::printf("  --probe-repeat implies --recipes 0: the probe "
							"measures distances over the recipe graph\n");
	}
	// SAME REASON for the levers that read the graph: without it they run, cost
	// nothing and do NOTHING, giving a result indistinguishable from doing
	// nothing and a false conclusion at the end.
	// `--recipes 0` feeds and measures the graph WITHOUT introducing it into the
	// finisher's `h`, so the levers stay the only factors changed.
	if((opt.assign || opt.backward) &&
	   opt.recipes < 0) {
		opt.recipes = 0.0;
		std::printf("  --assign / --backward imply --recipes 0: both "
							"read the recipe graph\n");
	}
	std::string error;
	if(!opt.deck_file.empty() && !opt.start_replay.empty()) {
		std::printf("!! --deck and --start are exclusive: one builds "
							"the starting position, the other reads it\n");
		return 2;
	}

	Replay replay;
	if(!replay.Load(opt.replay, error)) {
		std::printf("!! %s\n", error.c_str());
		return 1;
	}
	// A yrpX wraps a yrp1; a yrp1 stands on its own, and it is the format
	// WriteSolutions produces, so it must be accepted as input again.
	const Replay* yrp = replay.IsStreamed() ? replay.Embedded() : &replay;
	if(!yrp) {
		std::printf("!! no yrp1 embedded in %s: the player's "
							"decisions cannot be recovered.\n"
							"   The solver needs an exportable replay.\n", opt.replay.c_str());
		return 1;
	}

	// Starting replay: we keep only its yrp1, the sole carrier of the decks, the
	// seed and the duel parameters. Its stream and its answers are useless here:
	// this is a starting position, not a line to follow.
	Replay start_replay;
	const Replay* start_yrp = nullptr;
	if(!opt.start_replay.empty()) {
		if(!start_replay.Load(opt.start_replay, error)) {
			std::printf("!! starting replay: %s\n", error.c_str());
			return 1;
		}
		start_yrp = start_replay.IsStreamed() ? start_replay.Embedded()
											  : &start_replay;
		if(!start_yrp) {
			std::printf("!! no yrp1 embedded in %s: deck and seed "
									"unreadable.\n", opt.start_replay.c_str());
			return 1;
		}
		if(start_yrp->decks.size() <= static_cast<size_t>(opt.target_player)) {
			std::printf("!! the starting replay carries no deck "
									"for player %d.\n", opt.target_player);
			return 1;
		}
	}

	std::printf("loading\n");
	auto t_db = Clock::now();
	CardDB db;
	if(!db.Load(opt.workdir, error)) {
		std::printf("!! %s\n", error.c_str());
		return 1;
	}
	double ms_db = MsSince(t_db);
	std::printf("  cards             : %zu from %zu database(s)  (%.0f "
					"ms)\n",
				db.Size(), db.Sources().size(), ms_db);

	// The constraints are resolved as soon as the database is there: a card that
	// cannot be found or is ambiguous must stop the run BEFORE any search.
	LineConstraints cons;
	if(!ResolveConstraints(opt, db, cons))
		return 2;
	// Two rerooters active at once do not compose in our cost: the second would
	// silently overwrite the first. The paper combines its own through a WEIGHTED
	// sum (Th. 3.2), not through an implicit "and".
	if(opt.levin_reroot && opt.reroot_h > 0) {
		std::printf("!! --reroot and --reroot-h are exclusive (hard "
							"rerooter against soft rerooter).\n");
		return 2;
	}
	// --no-ref promises that the reference only supplies the duel's parameters.
	// Without --target it would still supply the TARGET BOARD and the promise
	// would be false: we refuse rather than lie in the report.
	if(opt.no_ref && !cons.target_scratch) {
		std::printf("!! --no-ref needs --target: without it the "
							"target board still comes from the reference.\n");
		return 2;
	}
	if(opt.no_ref && opt.deck_file.empty() && opt.start_replay.empty()) {
		std::printf("!! --no-ref needs --deck (or --start): the "
							"positional replay is only a template now.\n");
		return 2;
	}

	if(opt.mp1_only)
		std::printf("  constraint: combo in MAIN PHASE 1 only - "
							"entering the Battle Phase (hence Main 2) is "
							"removed from the enumeration; -> End Phase "
							"remains.\n");
	ScriptProvider scripts;
	scripts.Init(opt.workdir, opt.scriptdirs);
	std::printf("  script dirs       : %zu%s\n", scripts.Dirs().size(),
				opt.scriptdirs.empty() ? "" : "  (override)");

	// --no-self-negate: the deck's NEGATION effects are derived HERE, the one
	// place where the database, the scripts and the decks coexist before any
	// mode. Codes = the template's target player's deck, plus the --deck
	// decklist when given (a superset: a pair with no card matches nothing).
	// Categories read from constant.lua; zero card name compiled in.
	if(opt.no_self_negate) {
		std::vector<uint32_t> sn_codes;
		const int tp0 = opt.target_player;
		if(yrp && tp0 >= 0 &&
		   static_cast<size_t>(tp0) < yrp->decks.size())
			for(const auto* l :
				{ &yrp->decks[tp0].main, &yrp->decks[tp0].extra })
				for(uint32_t c : *l)
					sn_codes.push_back(c);
		if(!opt.deck_file.empty()) {
			Deck ydk;
			std::string derr;
			if(LoadYdk(opt.deck_file, ydk, derr))
				for(const auto* l : { &ydk.main, &ydk.extra })
					for(uint32_t c : *l)
						sn_codes.push_back(c);
		}
		ConstantTable snkt;
		uint64_t c_negate = 0, c_disable = 0;
		if(!snkt.Load(scripts) ||
		   (!snkt.Lookup("CATEGORY_NEGATE", c_negate) &
			!snkt.Lookup("CATEGORY_DISABLE", c_disable))) {
			std::printf("!! --no-self-negate: constants "
									"unreadable (constant.lua missing?) - "
									"discipline OFF, said rather than silent.\n");
		} else if(!sn_codes.empty()) {
			OperatorTable sntbl;
			sntbl.Build(db, scripts, snkt, sn_codes);
			std::string listing;
			for(const auto& [code, co] : sntbl.All())
				for(const DeclaredEffect& e : co.operators) {
					if(!e.at_init ||
					   !(e.category & (c_negate | c_disable)))
						continue;
					const uint32_t cc = db.Canonical(code);
					cons.self_negate.emplace_back(
						cc, e.has_desc ? e.desc_value : 0ull);
					listing += db.Name(cc) + " ; ";
				}
			std::sort(cons.self_negate.begin(), cons.self_negate.end());
			cons.self_negate.erase(std::unique(cons.self_negate.begin(),
											   cons.self_negate.end()),
								   cons.self_negate.end());
			std::printf("  discipline --no-self-negate: %zu "
									"negation effect(s) derived: %s\n",
						cons.self_negate.size(),
						listing.empty() ? "(none)" : listing.c_str());
		}
	}

	int major = 0, minor = 0;
	OCG_GetVersion(&major, &minor);
	std::printf("  ocgcore           : v%d.%d\n", major, minor);

	Arena arena;
	Arena* arena_ptr = nullptr;
	if(!opt.no_arena) {
		// Fixed base outside the usual ranges: useful later for carrying a snapshot
		// between processes. A failure is benign, and Init falls back on a free
		// address.
		constexpr std::uintptr_t kPreferredBase = 0x0000400000000000ull;
		if(!arena.Init(opt.arena_mb << 20, kPreferredBase, error)) {
			std::printf("!! arena: %s\n", error.c_str());
			return 1;
		}
		arena_ptr = &arena;
		std::printf("  arena             : base 0x%llx, %zu MB "
							"reserved, dirty pages %s\n",
					static_cast<unsigned long long>(arena.BaseAddress()),
					opt.arena_mb,
					arena.DirtyTrackingAvailable() ? "tracked" : "UNAVAILABLE");
		if(std::string problem = arena.SelfCheck(); !problem.empty()) {
			std::printf("!! arena inconsistent: %s\n", problem.c_str());
			return 1;
		}
	}

	int exit_code = 0;
	{
		// The duel must die before the arena: its destruction frees into the arena.
		auto t_create = Clock::now();
		Duel duel(db, scripts, arena_ptr);
		if(!duel.Create(yrp->seed, yrp->duel_flags, yrp->start_lp, yrp->start_hand,
						yrp->draw_count, error)) {
			std::printf("!! %s\n", error.c_str());
			return 1;
		}
		double ms_create = MsSince(t_create);

		bool gc_stopped = false;
		if(opt.stop_gc && arena_ptr)
			gc_stopped = duel.SetLuaGc(false);

		auto t_setup = Clock::now();
		// --opp-hand in JUDGE mode only: a replay produced with an augmented opponent
		// hand only replays with the same one. In --solve mode the main duel replays
		// the REFERENCE, recorded without those cards, and adding them would
		// desynchronise the replay (the rips change).
		const bool judge_opp_hand = !opt.solve && !cons.opp_hand.empty();
		if(judge_opp_hand)
			std::printf("  opponent hand: +%zu card(s) "
									"(--opp-hand)\n",
						cons.opp_hand.size());
		if(!duel.Setup(*yrp, error,
					   judge_opp_hand ? &cons.opp_hand : nullptr,
					   static_cast<uint8_t>(1 - opt.target_player))) {
			std::printf("!! %s\n", error.c_str());
			return 1;
		}
		double ms_setup = MsSince(t_setup);

		std::printf("\nreference replay\n");
		std::printf("  mode              : %s\n",
					yrp->IsHandTest() ? "HAND TEST" : "duel normal");
		std::printf("  garbage collector : %s\n",
					gc_stopped ? "STOPPED (memory "
													"reclaimed by restore)"
							   : "actif");
		std::printf("  answers to replay : %zu\n", yrp->responses.size());

		// Resume point taken BEFORE the line: the fidelity test consists in coming
		// back to it, replaying the 290 decisions and requiring a rigorously identical
		// state.
		double ms_push = 0, ms_pop = 0;
		size_t push_bytes = 0;
		if(arena_ptr) {
			auto t = Clock::now();
			arena_ptr->Push();
			ms_push = MsSince(t);
			push_bytes = arena_ptr->LastPush().bytes;
		}

		LineResult first = RunLine(duel, *yrp, opt, true, &cons);
		ReportLine(first, *yrp, db, opt);

		// --- THE VALIDATION HARNESS ------------------------------------------
		//
		// The order is the deliverable: extract, PRINT, then confront the plan the
		// core has just replayed. A table that does not explain a line known to be
		// valid (0 MSG_RETRY) is a wrong table, and anything built on it would be
		// wrong too.
		if(opt.operators) {
			ConstantTable kt;
			const size_t nconst = kt.Load(scripts);
			if(!nconst) {
				std::printf("\n!! --operators: no constant "
											"read. The game's `constant.lua` "
											"files are not in the "
											"--scriptdir: the table would be "
											"empty and the harness would "
											"return a false verdict.\n");
			} else {
				// The DECK's codes, duplicates included: it is the number of
				// COPIES that decides a "per COPY" capacity, and step 1 depends on
				// it. Codes outside the decklist are added afterwards, into
				// `codes` only; counting them as copies would manufacture
				// capacities that do not exist.
				std::vector<uint32_t> deck_codes;
				const size_t dk = static_cast<size_t>(opt.target_player);
				if(dk < yrp->decks.size()) {
					for(uint32_t c : yrp->decks[dk].main) deck_codes.push_back(c);
					for(uint32_t c : yrp->decks[dk].extra) deck_codes.push_back(c);
				}
				std::vector<uint32_t> codes = deck_codes;
				// The target board can name cards outside the decklist (--target
				// mode): read those too, otherwise an activation of the line would
				// fall into "card outside the table" for a reason that has nothing
				// to do with the extraction.
				for(const auto& [c, pos] : cons.board_add)
					codes.push_back(c);
				for(const ResolveReq& rq : cons.resolve_min)
					codes.push_back(rq.code);
				OperatorTable tbl;
				const size_t nread = tbl.Build(db, scripts, kt, codes);
				tbl.Print(db, kt);
				tbl.PrintGrants(db, kt);
				// The NEGATIVE column. The goal's zone is the EXTRA DECK: that is
				// where the copies the goal claims sleep, and it is the place
				// twelve edges out of thirteen were shown to empty. A constant
				// READ, never hard-coded.
				uint64_t extra = 0;
				kt.Lookup("LOCATION_EXTRA", extra);
				tbl.PrintConsumption(db, kt, extra);
				// STEP 1. Three `--target 54701958` are not three goals: it is ONE
				// goal with THREE copies, and that is the whole difference.
				// Multiplicity is what `RecipeDistance` does not carry and what
				// the operator bias cannot designate.
				std::vector<std::pair<uint32_t, uint32_t>> goal_counts;
				for(const auto& [gc, gpos] : cons.board_add) {
					(void)gpos;
					auto git = std::find_if(
						goal_counts.begin(), goal_counts.end(),
						[&](const auto& g) { return g.first == gc; });
					if(git == goal_counts.end())
						goal_counts.emplace_back(gc, 1u);
					else
						++git->second;
				}
				tbl.PrintFiringCounts(db, kt, deck_codes, goal_counts);
				// The transient demands on the bench too: the free judge must see
				// exactly the model the search will see, with the same flags and
				// the same compilation.
				std::vector<std::pair<uint32_t, uint32_t>> goal_transient;
				for(const ResolveReq& rr : cons.resolve_min) {
					const uint32_t cc = db.Canonical(rr.code);
					if(std::find_if(goal_transient.begin(),
									goal_transient.end(),
									[&](const auto& g) {
										return g.first == cc;
									}) == goal_transient.end())
						goal_transient.emplace_back(cc, 1u);
				}
				// THE SOLVER PROVES ITSELF BEFORE SERVING. Five instances with
				// known solutions, covering the four theorems. A wrong simplex
				// would return plausible and NON-admissible values: it is the only
				// point that cannot be proved, so it is the only one tested on
				// every execution.
				{
					size_t tot = 0;
					const size_t ok = SelfTestOperatorLP(&tot);
					std::printf("\n  simplex self-test: "
													"%zu/%zu%s\n", ok,
								tot, ok == tot ? "" :
								"   <<< SOLVER "
																"WRONG: every h "
																"value is to be "
																"discarded");
					if(ok == tot)
						BuildAndSolveBalance(tbl, db, kt, deck_codes,
											 goal_counts, goal_transient);
				}
				// --- THEOREM 2, OBSERVED ON A REAL LINE ---------------------
				//
				// `h` is proved consistent: along a plan it CANNOT drop by more
				// than the cost of the move. What cannot be proved is that the
				// MODEL describes this game, and a valid 331-decision line is the
				// only bench that says so.
				//
				// Three readings, and the third is the point:
				//   - `h` must end at ZERO (the goal is reached);
				//   - no drop of more than 1 per decision (otherwise the model or
				//     the solver lies, and the guard says so on the spot);
				//   - the DENSITY of descent, to compare with the 15 % of
				//     non-mute states of the novelty measure: the gradient the
				//     score never had.
				if(!goal_counts.empty() && arena_ptr) {
					BalanceModel bm;
					if(bm.Build(tbl, db, kt, deck_codes, goal_counts,
								goal_transient)) {
						while(arena.Depth() > 1)
							arena.Pop();
						if(arena.Depth() == 0)
							arena.Push();
						arena.Restore();
						const uint8_t con =
							static_cast<uint8_t>(opt.target_player);
						std::vector<uint32_t> res, ava, fld, grv, rmv, fzn, tmp;
						auto snap = [&]() {
							res.clear(); ava.clear(); fld.clear();
							for(uint32_t loc : { 0x01u, 0x40u }) {   // DECK, EXTRA
								duel.QueryCodes(con, loc, tmp);
								res.insert(res.end(), tmp.begin(), tmp.end());
							}
							// AVAILABLE = everything that can serve as a material.
							for(uint32_t loc : { 0x02u, 0x04u, 0x08u, 0x10u,
												 0x20u }) {
								duel.QueryCodes(con, loc, tmp);
								ava.insert(ava.end(), tmp.begin(), tmp.end());
							}
							duel.QueryCodes(con, 0x04u, tmp);   // MZONE
							fld.assign(tmp.begin(), tmp.end());
							duel.QueryCodes(con, 0x10u, tmp);   // GRAVE
							grv.assign(tmp.begin(), tmp.end());
							duel.QueryCodes(con, 0x20u, tmp);   // BANISHED
							rmv.assign(tmp.begin(), tmp.end());
							// IN PLAY (zone 6) = MZONE + SZONE.
							fzn = fld;
							duel.QueryCodes(con, 0x08u, tmp);
							fzn.insert(fzn.end(), tmp.begin(), tmp.end());
						};
						// --- PROFILE OF THE GAPS BETWEEN RUNGS ----------------
						// The closed form: the cost of a serialised run with
						// unequal blocks is Sigma b^(l_i), DOMINATED by
						// b^(l_max). The ladder of x* exists (16-18 cells
						// measured) and the bare run fails anyway, so the only
						// unknown left is the PROFILE of the l_i along a line
						// that reaches the goal. We measure it HERE, on the same
						// walk as the theorem 2 bench: same needs as the
						// serialisation (NeedsFrom of the balance at the start,
						// RESERVE excluded), same counting as SerialProgress.
						// SerialProgress.
						snap();
						LPResult r0;
						bm.Solve(res, ava, fld, &r0);
						std::vector<BalanceModel::Need> needs;
						if(r0.feasible) {
							for(const BalanceModel::Need& n : bm.NeedsFrom(r0))
								if(n.zone != 0)
									needs.push_back(n);
							// The CONSUMPTION rungs: the same grain as the
							// serialisation of the search path, so this bench
							// judges the ladder the bare run really climbs.
							for(const BalanceModel::Need& n :
								bm.ConsumedFrom(r0))
								needs.push_back(n);
							// The RESERVE DEPARTURES (zone 5), the same two
							// slices as the wiring (`arch` = the offset).
							for(uint64_t off : { 0ull, 15ull }) {
								BalanceModel::Need dep;
								dep.zone = 5;
								dep.arch = off;
								dep.count = 15;
								needs.push_back(dep);
							}
							// The HOSTS OF THE GRANTED EFFECTS @FIELD, the same
							// derivation as the wiring.
							{
								uint64_t k_add = 0, k_efm = 0;
								kt.Lookup("EFFECT_ADD_CODE", k_add);
								kt.Lookup("EFFECT_EXTRA_FUSION_"
																		"MATERIAL",
										  k_efm);
								std::unordered_map<uint32_t, uint32_t> dcop;
								for(uint32_t c : deck_codes)
									++dcop[db.Canonical(c)];
								for(const auto& kvv : tbl.All()) {
									const uint32_t host =
										db.Canonical(kvv.first);
									auto dit = dcop.find(host);
									if(dit == dcop.end())
										continue;
									bool enabler = false;
									for(const DeclaredEffect& g :
										kvv.second.grants)
										enabler =
											enabler ||
											(g.code_is_effect &&
											 ((k_add &&
											   g.code_value == k_add) ||
											  (k_efm &&
											   g.code_value == k_efm)));
									if(!enabler)
										continue;
									BalanceModel::Need pres;
									pres.code = host;
									pres.zone = 6;   // IN PLAY
									pres.count = (std::min)(dit->second, 3u);
									needs.push_back(pres);
								}
							}
						}
						const uint32_t res0b =
							static_cast<uint32_t>(res.size());
						auto served_now = [&]() {
							std::pair<uint64_t, uint32_t> out{ 0, 0 };
							uint32_t slot = 0;
							for(const BalanceModel::Need& n : needs) {
								uint32_t have = 0;
								if(n.zone == 5) {
									const uint32_t cur = static_cast<uint32_t>(
										res.size());
									const uint32_t dep =
										res0b > cur ? res0b - cur : 0;
									const uint32_t off =
										static_cast<uint32_t>(n.arch);
									have = dep > off ? dep - off : 0;
								} else {
								const std::vector<uint32_t>& pool =
									n.zone == 2   ? fld
									: n.zone == 3 ? grv
									: n.zone == 4 ? rmv
									: n.zone == 6 ? fzn
												  : ava;
								for(uint32_t raw : pool) {
									const uint32_t c = db.Canonical(raw);
									bool okc = false;
									if(n.code)
										okc = c == n.code;
									else if(const CardRow* row = db.Find(c))
										for(uint16_t sc : row->setcodes)
											if(sc && (sc & 0x0fffu) ==
														 (n.arch & 0x0fffu)) {
												okc = true;
												break;
											}
									if(okc && ++have >= n.count)
										break;
								}
								}
								const uint32_t got = (std::min)(have, n.count);
								out.second += got;
								if(slot < 16)
									out.first |=
										static_cast<uint64_t>(
											(std::min)(got, 15u))
										<< (slot * 4);
								++slot;
							}
							return out;
						};
						uint32_t max_served = served_now().second;
						// Attribution PER SUBGOAL: which requirement each unit
						// serves, and at which answer. That is what turns "there
						// is a desert of 73 answers" into "the desert is BETWEEN
						// these two subgoals", the question one more rung has to
						// answer.
						std::vector<uint32_t> got_max(needs.size(), 0);
						{
							uint32_t slot2 = 0;
							const uint64_t pk0 = served_now().first;
							for(size_t i = 0; i < needs.size() && slot2 < 16;
								++i, ++slot2)
								got_max[i] = static_cast<uint32_t>(
									(pk0 >> (slot2 * 4)) & 15u);
						}
						std::vector<size_t> unit_at;   // answer of the n-th +1
						std::vector<size_t> unit_need; // index of the subgoal served
						std::vector<uint64_t> rung_vals;
						size_t at = 0, steps = 0, down = 0, up = 0, bad = 0;
						double prev = -1.0, first_h = -1.0, last_h = -1.0;
						size_t infeasible = 0;
						// THE DIAGNOSIS: an "h = INFINITE" in the middle of a line
						// that succeeds is an OVER-CONSTRAINT of the model, and a
						// count alone does not say which. We keep the first
						// occurrences with their phase 1 rows.
						std::vector<std::pair<size_t, std::string>> inf_diag;
						// THE WALK WITH QUOTAS: the MANDATED JUDGE of the
						// red-black relaxation. The same walk, the same LP, plus
						// the MSG_CHAINING uses accumulated along the line (same
						// host derivation and same cap of 12 as the run's
						// wiring). The reference line SUCCEEDS, so every state it
						// crosses is alive, and any "h_quota = INFINITE" on a
						// state where h was finite is an OVER-CONSTRAINT of the
						// aggregate: 0 new ones are required before promoting the
						// mechanism beyond refinement.
						std::vector<uint32_t> qwalk_hosts, qwalk_spent;
						std::vector<uint32_t> qwalk_chain;
						std::vector<uint32_t> qunb_union, qover_union;
						size_t infeasible_q = 0, infeasible_new = 0;
						double first_hq = -1.0, last_hq = -1.0;
						uint32_t qmax_applied = 0;
						std::vector<std::pair<size_t, std::string>> infq_diag;
						if(opt.quota_h && r0.feasible) {
							qwalk_hosts = bm.QuotaHostsFrom(r0, nullptr);
							if(qwalk_hosts.size() > 12)
								qwalk_hosts.resize(12);
							qwalk_spent.assign(qwalk_hosts.size(), 0u);
						}
						while(at < yrp->responses.size()) {
							qwalk_chain.clear();
							const size_t used = Advance(
								duel, *yrp, at, 1, nullptr,
								qwalk_hosts.empty() ? nullptr : &qwalk_chain);
							if(!used)
								break;
							at += used;
							++steps;
							for(uint32_t raw : qwalk_chain) {
								const uint32_t cq = db.Canonical(raw);
								for(size_t qi = 0; qi < qwalk_hosts.size(); ++qi)
									if(qwalk_hosts[qi] == cq)
										++qwalk_spent[qi];
							}
							snap();
							if(!needs.empty()) {
								const auto [pk, sv] = served_now();
								rung_vals.push_back(pk);
								for(size_t i = 0;
									i < needs.size() && i < 16; ++i) {
									const uint32_t g = static_cast<uint32_t>(
										(pk >> (i * 4)) & 15u);
									while(got_max[i] < g) {
										++got_max[i];
										unit_at.push_back(steps);
										unit_need.push_back(i);
									}
								}
								if(sv > max_served)
									max_served = sv;
							}
							LPResult lrx;
							const double h = bm.Solve(res, ava, fld, &lrx);
							// The twin walk with quotas: same markings, plus the
							// path's capacities. It compares with `h` DECISION BY
							// DECISION; an infeasible where h was finite is a NEW
							// one, and that is what the judge counts.
							if(!qwalk_hosts.empty()) {
								std::vector<std::pair<uint32_t, uint32_t>> qsp;
								for(size_t qi = 0; qi < qwalk_hosts.size(); ++qi)
									if(qwalk_spent[qi])
										qsp.emplace_back(qwalk_hosts[qi],
														 qwalk_spent[qi]);
								LPResult lqx;
								const double hq =
									bm.Solve(res, ava, fld, &lqx, qsp);
								qmax_applied = (std::max)(qmax_applied,
														  lqx.quota_applied);
								auto once = [](std::vector<uint32_t>& v,
											   uint32_t c) {
									if(std::find(v.begin(), v.end(), c) ==
									   v.end())
										v.push_back(c);
								};
								for(uint32_t sk : lqx.quota_unbounded_hosts)
									once(qunb_union, sk);
								for(uint32_t sk : lqx.quota_overrun_hosts)
									once(qover_union, sk);
								if(hq < 0) {
									++infeasible_q;
									if(h >= 0) {
										++infeasible_new;
										if(infq_diag.size() < 6) {
											std::string rows;
											for(const std::string& lb :
												lqx.infeasible_rows) {
												if(!rows.empty())
													rows += " ; ";
												rows += lb;
											}
											if(rows.empty())
												rows = "(solveur en defaut)";
											infq_diag.emplace_back(steps, rows);
										}
									}
								} else {
									if(first_hq < 0)
										first_hq = hq;
									last_hq = hq;
								}
							}
							if(h < 0) {
								++infeasible;
								if(inf_diag.size() < 6) {
									std::string rows;
									for(const std::string& lb :
										lrx.infeasible_rows) {
										if(!rows.empty())
											rows += " ; ";
										rows += lb;
									}
									if(rows.empty())
										rows = "(solver at fault, "
																					"not a proof)";
									inf_diag.emplace_back(steps, rows);
								}
								continue;
							}
							if(first_h < 0)
								first_h = h;
							if(prev >= 0) {
								if(h < prev - 1.0 - 1e-6)
									++bad;      // drop > 1: th. 2 VIOLATED
								else if(h < prev - 1e-6)
									++down;
								else if(h > prev + 1e-6)
									++up;
							}
							prev = h;
							last_h = h;
						}
						arena.Restore();
						std::printf("\n=== h ALONG THE "
															"REAL LINE (theorem "
															"2) ===\n");
						std::printf("  %zu decision(s) "
															"parcourue(s) ; h : "
															"%.0f -> %.0f\n", steps, first_h, last_h);
						std::printf("  descentes %zu "
															"(%.0f %%), montees "
															"%zu, plats %zu\n", down,
									steps ? 100.0 * double(down) / double(steps)
										  : 0.0,
									up, steps - down - up - bad - infeasible);
						// THIS COUNTER IS NOT A JUDGE OF THEOREM 2, and saying so
						// is the correction. The theorem's unit is the
						// TRANSITION; this walk's unit is the DECISION, and a
						// single decision sometimes resolves a whole chain, hence
						// several operators. A drop of k on one decision is
						// LEGITIMATE as soon as k operators fired. The first run
						// of this bench counted it as a violation: it was the
						// instrument, not the model.
						std::printf("  drops of more "
															"than 1 on ONE "
															"decision: %zu  "
															"(expected: one "
															"decision sometimes "
															"resolves a whole "
															"chain)\n", bad);
						// THREE CASES, AND CONFLATING THEM WOULD MAKE THE REPORT
						// LIE. The first version printed "the model RECOGNISES
						// the goal" on a goal PROVED IMPOSSIBLE (h = infinite
						// everywhere, `last_h` left at its sentinel): an
						// instrument that congratulates the model when it
						// declares the line dead is worse than no instrument.
						if(infeasible == steps && steps)
							std::printf("  h = INFINITE "
																	"along the WHOLE "
																	"line: the model "
																	"declares this goal "
																	"out of reach "
																	"(consistent with "
																	"the proven dead "
																	"end)\n");
						else if(last_h < 0)
							std::printf("  final h: "
																	"UNDETERMINED (no "
																	"feasible state met)\n");
						else
							std::printf("  h final = %.0f  "
																	"%s\n",
										std::fabs(last_h) < 1e-9 ? 0.0 : last_h,
										std::fabs(last_h) < 1e-6
											? " - the model "
																						"RECOGNISES the goal"
											: "<<< the goal is "
																						"reached and h does "
																						"not see it: model "
																						"INCOMPLETE");
						if(infeasible) {
							std::printf("  states where h = "
																	"INFINITE: %zu   <<< "
																	"the model declares "
																	"dead a line that "
																	"SUCCEEDS: "
																	"over-constrained\n",
										infeasible);
							for(const auto& [st, rows] : inf_diag)
								std::printf("      decision %zu "
																			": %s\n", st,
											rows.c_str());
						}
						// --- verdict of the walk with quotas ------------------
						if(!qwalk_hosts.empty()) {
							std::printf("\n  --- the same "
																	"walk WITH PATH "
																	"QUOTAS (--quota-h) "
																	"---\n");
							std::printf("  hosts tracked: "
																	"%zu; quota lines "
																	"posted (max along "
																	"the line): %u\n",
										qwalk_hosts.size(), qmax_applied);
							// The reason for each ignored host: `unbounded` calls
							// for extracting the bounds, `over budget` calls for
							// the model's coverage, and conflating them would send
							// the fix to the wrong place.
							auto qprint = [&db](const char* tag,
												const std::vector<uint32_t>& v) {
								if(v.empty())
									return;
								std::string sk;
								for(uint32_t c : v)
									sk += db.Name(c) + " ; ";
								std::printf("  ignores (%s) : %s\n", tag,
											sk.c_str());
							};
							qprint("effect with NO "
															"declared bound - "
															"extraction",
								   qunb_union);
							qprint("observations > "
															"budget declare  -  "
															"couverture", qover_union);
							std::printf("  h_quota: %.0f -> "
																	"%.0f; infeasible "
																	"%zu (control "
																	"without quotas %zu)\n",
										first_hq, last_hq, infeasible_q,
										infeasible);
							std::printf("  NEW infeasible "
																	"states: %zu  %s\n", infeasible_new,
										infeasible_new
											? "<<< red-black "
																						"OVER-CONSTRAINT: "
																						"the aggregate lies "
																						"on this line, do "
																						"not promote"
											: "(JUDGE: 0 new - the "
																						"aggregate is along "
																						"the reference)");
							for(const auto& [st, rows] : infq_diag)
								std::printf("      decision %zu "
																			": %s\n", st,
											rows.c_str());
						}
						// --- the profile, and its verdict in closed form ------
						if(!needs.empty() && !unit_at.empty()) {
							std::sort(rung_vals.begin(), rung_vals.end());
							rung_vals.erase(std::unique(rung_vals.begin(),
														rung_vals.end()),
											rung_vals.end());
							std::vector<size_t> gaps;
							size_t prev_at = 0;
							for(size_t p : unit_at) {
								gaps.push_back(p - prev_at);
								prev_at = p;
							}
							std::sort(gaps.rbegin(), gaps.rend());
							std::printf("\n=== GAP PROFILE "
																	"BETWEEN RUNGS "
																	"(along the real "
																	"line) ===\n");
							std::printf("  %zu subgoal "
																	"unit(s) served in "
																	"%zu answers; %zu "
																	"distinct step(s) of "
																	"the ladder\n", unit_at.size(), steps,
										rung_vals.size());
							// The ladder NAMED: each rung, its answer, and the gap
							// from the previous one. The deserts are read here,
							// between WHICH subgoals, not only how long.
							// quelle longueur.
							{
								size_t pat = 0;
								for(size_t i = 0; i < unit_at.size(); ++i) {
									const BalanceModel::Need& n =
										needs[unit_need[i]];
									char nb[64];
									if(n.zone == 5)
										std::snprintf(nb, sizeof nb,
													  "(from reserve)");
									else if(n.code)
										std::snprintf(nb, sizeof nb, "%s",
													  db.Name(n.code).c_str());
									else
										std::snprintf(nb, sizeof nb,
													  "archetype 0x%llx",
													  (unsigned long long)n.arch);
									std::printf("    rung %2zu  "
																					"answer %3zu (+%3zu) "
																					" %-34s @%s\n", i + 1,
												unit_at[i], unit_at[i] - pat, nb,
												n.zone == 2   ? "TERRAIN"
												: n.zone == 3 ? "CIMETIERE"
												: n.zone == 4 ? "BANNIE"
												: n.zone == 5 ? "RESERVE"
												: n.zone == 6 ? "EN JEU"
															  : "DISPO");
									pat = unit_at[i];
								}
							}
							std::printf("  gaps between "
																	"consecutive units, "
																	"in ANSWERS "
																	"(opponent and "
																	"forced included),\n"
																	"  sorted descending:");
							for(size_t i = 0; i < gaps.size() && i < 12; ++i)
								std::printf(" %zu", gaps[i]);
							if(gaps.size() > 12)
								std::printf(" ...");
							std::printf("\n");
							// The closed form: cost ~ Sigma b^(l_i) in CHOICE
							// decisions. On liger.yrpX, 170 of the 331 answers are
							// choice decisions (~0.51), so the gap in answers is an
							// UPPER BOUND on the useful gap.
							const double ratio = 0.51, b = 5.9;
							const double lmax =
								static_cast<double>(gaps.empty() ? 0 : gaps[0]);
							std::printf("  l_max = %.0f "
																	"answers (~%.0f "
																	"decisions with a "
																	"choice); dominant "
																	"term b^l: 10^%.1f "
																	"at b=%.1f\n", lmax,
										lmax * ratio,
										lmax * ratio * std::log10(b), b);
						}
					}
				}
				std::printf("\n  (%zu script(s) read, %zu not "
											"found)\n",
							nread, tbl.Missing());
				HarnessVerdict hv = ConfrontPlan(tbl, db, kt, first.activations);
				PrintVerdict(hv);
			}
		}

		// Verdict of the constraints on THIS replay, even with no search: the tool is
		// also a JUDGE, so a replay produced yesterday (or played by hand) is checked
		// by passing it as input with the same flags.
		if(cons.Any()) {
			std::printf("\n--- verdict of the constraints on this "
									"replay ---\n");
			for(const auto& [n, allowed] : cons.summons) {
				if(n > first.summon_codes.size()) {
					std::printf("  --summon #%u: not "
													"applicable (%zu summons)\n",
								n, first.summon_codes.size());
					continue;
				}
				uint32_t canon = db.Canonical(first.summon_codes[n - 1]);
				bool match = std::find(allowed.begin(), allowed.end(), canon) !=
							 allowed.end();
				std::printf("  --summon #%u : %s (%s)\n", n,
							match ? "respectee" : "VIOLEE",
							db.Name(canon).c_str());
			}
			if(!cons.guard.empty()) {
				std::printf("  --guard: %zu opponent "
											"window(s) under threat, %zu "
											"uncovered%s\n", first.guard_checks,
							first.guard_violations,
							first.guard_violations ? "  <-- VIOLEE" : "");
				if(first.guard_violations)
					std::printf("            first: after "
													"summon #%zu, opponent "
													"hand %u card(s)\n",
								first.first_guard_violation_summon,
								first.first_guard_violation_opp_hand);
			}
			if(!cons.no_activate.empty())
				std::printf("  --no-activate : %zu "
											"activation(s) interdite(s) "
											"utilisee(s)%s\n", first.forbidden_activations,
							first.forbidden_activations ? "  <-- VIOLEE" : "");
			for(size_t i = 0; i < cons.resolve_min.size(); ++i) {
				size_t have = i < first.resolve_counts.size()
					? first.resolve_counts[i] : 0;
				std::printf("  --resolve %s%s%s : %zu/%u%s\n",
							db.Name(cons.resolve_min[i].code).c_str(),
							cons.resolve_min[i].zones ? "@" : "",
							cons.resolve_min[i].zones
								? ZoneMaskName(cons.resolve_min[i].zones).c_str()
								: "",
							have, cons.resolve_min[i].min_count,
							have < cons.resolve_min[i].min_count
								? "  <-- VIOLEE" : "");
			}
		}

		if(g_verbose) {
			std::printf("\n--- execution cost ---\n");
			std::printf("  database load        : %8.1f ms  (once "
									"per process)\n",
						ms_db);
			std::printf("  duel + scripts       : %8.1f ms  <-- "
									"paid for EVERY new duel\n", ms_create);
			std::printf("  deck setup           : %8.1f ms\n", ms_setup);
			std::printf("  line replay          : %8.1f ms for "
									"%zu decisions  (%.3f ms/decision)\n",
						first.ms, first.responses_used,
						first.responses_used ? first.ms / first.responses_used : 0.0);
			std::printf("  => full re-simulation from the root: "
									"%.1f ms\n",
						ms_create + ms_setup + first.ms);
		}

		if(arena_ptr) {
			if(g_verbose) {
				const double ms_per_decision = first.responses_used
					? (first.ms - first.ms_write_watch) / first.responses_used : 0.0;
				ReportDirty(first, *arena_ptr, ms_per_decision);
				const ArenaStats st = arena_ptr->Stats();
				std::printf("\n--- arena ---\n");
				std::printf("  committed         : %.2f MB\n", st.committed / 1048576.0);
				std::printf("  served            : %.2f MB  "
											"(snapshot bound)\n",
							st.in_use / 1048576.0);
				std::printf("  live blocks       : %.2f MB\n", st.live_bytes / 1048576.0);
				std::printf("  allocations       : %zu, frees "
											"%zu\n",
							st.alloc_count, st.free_count);
				// Scope: THIS arena (the main thread). The workers report through
				// ReportPoison.
				std::printf("  arena escapes     : %zu  %s\n", st.host_fallbacks,
							st.poisoned
								? "<-- ARENA POISONED: "
																"state outside the "
																"snapshot"
								: "(none on the main "
																"arena; workers "
																"report separately)");
			}

			// FIDELITY OF THE SNAPSHOT. The verdict gates everything downstream, so it
			// is always printed; the byte counts and the two fingerprints are detail.
			if(g_verbose)
				std::printf("\n--- snapshot fidelity ---\n"
											"  initial push            : %.2f "
											"MB, %.2f ms\n",
							push_bytes / 1048576.0, ms_push);

			// Restore and not Pop: we keep the level so we can come back to it as often
			// as we like. It is the operation the solver uses on every sibling of a node.
			auto t = Clock::now();
			arena_ptr->Restore();
			ms_pop = MsSince(t);
			LineResult second = RunLine(duel, *yrp, opt, false);
			arena_ptr->Restore();   // the stress test restarts from the top of the line

			bool ok = second.retries == 0 &&
					  second.responses_used == first.responses_used &&
					  second.turns == first.turns &&
					  second.fingerprint_final == first.fingerprint_final &&
					  second.fingerprint_at_target == first.fingerprint_at_target;
			if(g_verbose) {
				std::printf("  restore                 : %.2f "
											"ms  (%.2f MB copied back)\n",
							ms_pop, arena_ptr->LastRestore().bytes / 1048576.0);
				std::printf("  replay after restore    : %zu "
											"answers, %zu retries, %d turns\n",
							second.responses_used, second.retries, second.turns);
				std::printf("  target board print      : "
											"%016llx vs %016llx\n",
							(unsigned long long)first.fingerprint_at_target,
							(unsigned long long)second.fingerprint_at_target);
				std::printf("  final state print       : "
											"%016llx vs %016llx\n",
							(unsigned long long)first.fingerprint_final,
							(unsigned long long)second.fingerprint_final);
			}
			if(!ok) {
				std::printf("!! snapshot fidelity: DIVERGENCE "
											"- the snapshot leaves state "
											"outside it\n");
				exit_code = 1;
			}

			if(ok)
				exit_code |= RunStressTest(duel, *yrp, opt, *arena_ptr,
										   first.fingerprint_final);
			if(ok && (opt.growth || opt.solve)) {
				// The order matters: a search run with an incomplete enumerator or a
				// digest that merges measures nothing.
				RunEnumeratorCheck(duel, *yrp, opt, *arena_ptr);
			}
			// Width is MEASURED before anything is promised: it is what calibrates the
			// patience of the novelty pruning.
			uint32_t patience = 0;
			if(ok && (opt.width || opt.solve))
				patience = MeasureWidth(duel, *yrp, opt, *arena_ptr, first);

			// THE SELF-CHECK LINE. One line at the default level carrying every number
			// the health gate is read on; --verbose opens the blocks they came from.
			// A failing check has already said so on its own, above.
			if(!g_verbose) {
				std::string sc = ok ? "pass" : "FAILED";
				for(const std::string* s : { &g_enum_line, &g_stress_line, &g_width_line })
					if(!s->empty())
						sc += ", " + *s;
				std::printf("  self-checks  : %s\n", sc.c_str());
			}
			if(ok && opt.growth)
				RunGrowthMeasurement(duel, *yrp, opt, *arena_ptr, first);
			if(ok && !opt.fire_spec.empty()) {
				// Opponent test: an exclusive mode. The threat is played for real, and
				// the search rebuilds the board from each injection.
				RunFireTest(duel, *arena_ptr, *yrp, opt, first, db, scripts,
							cons);
			} else if(ok && opt.solve) {
				// A constraint the reference violates changes the nature of the
				// same-deck problem: the recorded line IS NO LONGER a solution, and
				// repair, which perturbs that line, leads nowhere. The right engine is
				// then the transplantation one (repertoire + NRPA), applied to the same
				// deck: same target board, free line.
				bool ref_violates = first.guard_violations > 0 ||
									first.forbidden_activations > 0;
				for(size_t i = 0; i < cons.resolve_min.size(); ++i) {
					size_t have = i < first.resolve_counts.size()
						? first.resolve_counts[i] : 0;
					if(have < cons.resolve_min[i].min_count)
						ref_violates = true;
				}
				for(const auto& [n, allowed] : cons.summons) {
					if(n > first.summons_at_target)
						continue;
					uint32_t canon = db.Canonical(first.summon_codes[n - 1]);
					if(std::find(allowed.begin(), allowed.end(), canon) ==
					   allowed.end())
						ref_violates = true;
				}
				// SYNTHETIC starting position: decklist + hand, with no replay. The
				// target, the opponent and the parameters stay the reference's; the
				// hand is forced and VERIFIED by a throwaway duel before any search.
				Replay synth;
				bool synth_ok = false;
				if(!opt.deck_file.empty()) {
					Deck ydk;
					std::string derr;
					std::vector<uint32_t> hand;
					bool ok_input = LoadYdk(opt.deck_file, ydk, derr);
					if(!ok_input)
						std::printf("!! %s\n", derr.c_str());
					if(ok_input) {
						if(!opt.hand_spec.empty()) {
							for(const std::string& item :
								SplitOn(opt.hand_spec, '|')) {
								if(item.empty())
									continue;
								uint32_t code = 0;
								if(!ResolveCard(item, db, "--hand", code)) {
									ok_input = false;
									break;
								}
								hand.push_back(code);
							}
						} else {
							// Default hand: the reference's.
							for(const auto& c : first.start_self.hand_cards)
								if(c.present)
									hand.push_back(db.Canonical(c.code));
						}
					}
					if(ok_input && hand.empty()) {
						std::printf("!! --deck: no "
															"opening hand\n");
						ok_input = false;
					}
					if(ok_input) {
						std::printf("\n=== synthetic "
															"start ===\n");
						std::printf("  decklist: %s (%zu "
															"main, %zu extra)\n",
									opt.deck_file.c_str(), ydk.main.size(),
									ydk.extra.size());
						// What the TEMPLATE contributes, and nothing else: without this
						// record, "without a reference" is not verifiable.
						std::printf("  template : %s\n"
															"             flags "
															"0x%llx, %u LP, hand "
															"%u, draw %u, "
															"opponent %zu+%zu "
															"cards\n",
									opt.replay.c_str(),
									(unsigned long long)yrp->duel_flags,
									yrp->start_lp, yrp->start_hand,
									yrp->draw_count,
									yrp->decks.size() > 1 ? yrp->decks[1].main.size() : 0,
									yrp->decks.size() > 1 ? yrp->decks[1].extra.size() : 0);
						std::printf("  opening hand:");
						for(uint32_t c : hand)
							std::printf(" %s;", db.Name(db.Canonical(c)).c_str());
						std::printf("\n");
						if(BuildSyntheticStart(*yrp, ydk, hand, db, scripts,
											   opt.arena_mb, synth, derr)) {
							std::printf("  hand forced and "
																	"VERIFIED on a "
																	"throwaway duel.\n");
							synth_ok = true;
						} else {
							std::printf("!! %s\n", derr.c_str());
						}
					}
				}

				if(cons.AnyBoardEdit() && !synth_ok && !start_yrp) {
					std::printf("\n!! --board-add/--board-"
													"remove edit the TARGET: "
													"they need --deck or "
													"--start\n   (in repair "
													"mode the reference can "
													"no longer act as a "
													"control on a target\n"
													"   it does not reach).\n");
					exit_code = 1;
				} else if(synth_ok) {
					// THE INTERNAL LOOP (Go-Explore / Expert Iteration shape). One
					// command, N rounds: each round is a run of today byte for byte
					// (budget = an equal share of the remainder); between two rounds,
					// the best JOINT line written is re-injected as the next round's
					// approach, i.e. the Go-Explore archive persisting without the
					// operator iterating. It REPLACES the previous round's line (no
					// stacking); the command's --approach entries remain.
					// rounds=1: one call, no banner, the historical behaviour.
					Options ropt = opt;
					const double total_ms = opt.solve_ms;
					const auto tstart = Clock::now();
					const uint64_t nrounds = (std::max<uint64_t>)(1, opt.rounds);
					// The inter-round carry (--carry): the archive and the policy
					// outlive the calls, i.e. full Go-Explore.
					RoundCarry rcarry;
					for(uint64_t round = 0; round < nrounds; ++round) {
						ropt.solve_ms = (std::max)(0.0,
							(total_ms - MsSince(tstart)) /
								double(nrounds - round));
						if(nrounds > 1) {
							char carried[64] = "";
							if(opt.carry && !rcarry.archive.empty())
								std::snprintf(carried, sizeof(carried),
											  ", archive portee "
																						"%zu cellule(s)",
											  rcarry.archive.size());
							std::printf("\n===== ROUND "
																	"%llu/%llu  -  "
																	"budget %.0f s%s%s "
																	"=====\n",
										(unsigned long long)(round + 1),
										(unsigned long long)nrounds,
										ropt.solve_ms / 1000.0,
										ropt.approach_files.size() >
												opt.approach_files.size()
											? ", joint line "
																						"re-injected"
											: "", carried);
						}
						TransplantOutcome tout;
						RunTransplantSolve(duel, *yrp, synth, ropt, *arena_ptr,
										   first, db, scripts, patience, cons,
										   &tout, &rcarry);
						if(tout.solutions) {
							if(nrounds > 1)
								std::printf("\n===== ROUND "
																			"%llu/%llu: %zu "
																			"solution(s) - the "
																			"loop stops =====\n",
											(unsigned long long)(round + 1),
											(unsigned long long)nrounds,
											tout.solutions);
							break;
						}
						if(round + 1 >= nrounds)
							break;
						// With neither a joint line NOR a carried archive, the next
						// round would be a plain re-run: we stop. Under --carry, the
						// carried archive is a re-injection in its own right and the
						// loop continues with no line.
						if(tout.joint_file.empty() &&
						   !(opt.carry && !rcarry.archive.empty())) {
							std::printf("\n===== ROUND "
																	"%llu/%llu: no joint "
																	"line written - the "
																	"loop stops =====\n",
										(unsigned long long)(round + 1),
										(unsigned long long)nrounds);
							break;
						}
						ropt.approach_files = opt.approach_files;
						if(!tout.joint_file.empty())
							ropt.approach_files.push_back(tout.joint_file);
					}
				} else if(!opt.deck_file.empty()) {
					// --deck requested but the start cannot be built: do not fall
					// back silently onto another mode.
					exit_code = 1;
				} else if(start_yrp)
					RunTransplantSolve(duel, *yrp, *start_yrp, opt, *arena_ptr,
									   first, db, scripts, patience, cons);
				else if(ref_violates) {
					// A constraint violated by the reference is often fixed by a SMALL
					// perturbation of its line (reordering two summons): the
					// bounded-discrepancy repair is the exact tool for that
					// neighbourhood, and the constraints force the deviations in the
					// right place there. NRPA, which rebuilds from scratch, only comes
					// as a backup, with the rest of the budget.
					std::printf("\n  The reference "
													"violates a line "
													"constraint.\n"
													"  1) bounded-discrepancy "
													"repair UNDER the "
													"constraints (40%% of the "
													"budget);\n"
													"  2) otherwise, "
													"repertoire + NRPA on the "
													"same deck.\n");
					Options ropt = opt;
					ropt.solve_ms = opt.solve_ms * 0.4;
					size_t found = RunSolve(duel, *yrp, ropt, *arena_ptr, first,
											db, scripts, patience, cons);
					if(!found) {
						Options topt = opt;
						topt.solve_ms = opt.solve_ms * 0.6;
						RunTransplantSolve(duel, *yrp, *yrp, topt, *arena_ptr,
										   first, db, scripts, patience, cons);
					}
				} else {
					// --adapt is only wired into the --start/--fire paths
					// (BuildAdaptRuns). Accepting it here without reading it would be a
					// mechanism silently absent from the path, the exact family of "check
					// that it COULD have produced the effect". That is how the options
					// forecast goes unseen: its script runs in repair mode, where
					// the table is never printed.
					if(!opt.adapt_files.empty())
						std::printf("\n!! --adapt is "
															"IGNORED in repair "
															"mode (without "
															"--start/--fire): "
															"the corpus\n"
															"   only enters "
															"through the "
															"transplantation's "
															"adaptation replay.\n");
					RunSolve(duel, *yrp, opt, *arena_ptr, first, db, scripts,
							 patience, cons);
				}
			}
		}

		if(first.retries)
			exit_code = 1;
		if(!scripts.Misses().empty()) {
			std::printf("\n  !! scripts not found (%zu):", scripts.Misses().size());
			int shown = 0;
			for(const auto& m : scripts.Misses()) {
				if(shown++ >= 6) { std::printf(" ..."); break; }
				std::printf(" %s", m.c_str());
			}
			std::printf("\n");
		}
		// The liveness of the script cache: how many disk re-reads the run would
		// have paid without it. A high figure names the "script loaded after the
		// root Push, undone by every Restore" traffic, invisible until now
		// because nothing counted it.
		if(scripts.CacheHits() && g_verbose)
			std::printf("\n  script cache: %llu disk re-read(s) "
									"avoided (%zu script(s) in memory)\n",
						(unsigned long long)scripts.CacheHits(),
						scripts.CacheEntries());
		if(g_depth_fallbacks.load(std::memory_order_relaxed))
			std::printf("\n  !! %llu finisher search(es) received "
									"the FALLBACK depth ceiling (%u "
									"decisions):\n     their prefix already "
									"reached the global ceiling. Two arms "
									"compared \"at equal budget\" then do not "
									"have the same DEPTH budget.\n",
									(unsigned long long)g_depth_fallbacks.load(
											std::memory_order_relaxed),
						kFinisherFallbackDepth);
		if(duel.EmptyProcessorStates())
			std::printf("\n  !! %zu EMPTY processor-state "
									"query(ies): the digest loses its chain "
									"component,\n     two instants of the "
									"same resolution merge, and the combo "
									"branch is pruned\n     from the start.\n",
						duel.EmptyProcessorStates());
		if(!scripts.Unreadable().empty()) {
			std::printf("\n  !! scripts PRESENTS mais illisibles "
									"(%zu) :",
						scripts.Unreadable().size());
			int shown = 0;
			for(const auto& u : scripts.Unreadable()) {
				if(shown++ >= 4) { std::printf(" ..."); break; }
				std::printf(" %s", u.c_str());
			}
			std::printf("\n     The loader does NOT fall back to "
									"a lower-priority repository (that would\n"
									"     be another version of the script) - "
									"these cards are absent from the space.\n");
			exit_code = 1;
		}
		if(!duel.Errors().empty()) {
			std::printf("\n  !! core errors (%zu):\n", duel.Errors().size());
			for(size_t i = 0; i < duel.Errors().size() && i < 6; ++i)
				std::printf("      %s\n", duel.Errors()[i].c_str());
		}
		// Codes absent from cards.cdb: the DATABASE twin of a script-set mismatch,
		// and quieter than it. The core gives the unknown card a vanilla body with
		// no effect, the deck loads, the duel starts, the line diverges, and
		// nothing would report it.
		if(!db.UnknownCodes().empty()) {
			std::printf("\n  !! codes missing from cards.cdb "
									"(%zu):",
						db.UnknownCodes().size());
			int shown = 0;
			for(uint32_t c : db.UnknownCodes()) {
				if(shown++ >= 8) { std::printf("..."); break; }
				std::printf("%u ", c);
			}
			std::printf("\n     These cards are EFFECTLESS "
									"VANILLAS to the core: any line that goes\n"
									"     through them is wrong. cards.cdb is "
									"stale or incomplete.\n");
		}
		// Answers the filter decoder could not read.
		if(UndecodableResponses()) {
			std::printf("\n  !! %llu answer(s) UNDECODABLE by the "
									"--no-activate/--no-chain filter.\n"
									"     The core message layout has "
									"drifted: the filters no longer mean "
									"anything,\n     and this run is to be "
									"discarded.\n",
						(unsigned long long)UndecodableResponses());
			exit_code = 1;
		}
	}
	arena.Shutdown();
#if defined(__EMSCRIPTEN__)
	// Summary of the write barrier verifier (R2V_ARENA_VERIFY=1): under wasm it
	// is what plays the role of hardware dirty-page tracking, and an incomplete
	// barrier corrupts SILENTLY. Mute when not requested.
	Arena::PrintVerifyReport();
#endif
	// Whole-run total: the phases already printed plus whatever ran outside them
	// (reference replay, growth measurements...).
	prof::PrintTotal();
	return exit_code;
}
